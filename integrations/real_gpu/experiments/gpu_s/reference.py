"""Independent CPU Qwen3 equations on exact, already-rounded GGUF weights.

No llama inference or CUDA kernels are used. K/V storage is rounded to F16 at
publication; reductions, attention and nonlinear operations use torch CPU F32.
"""
import math
import sys
from pathlib import Path
import numpy as np
import torch
from common import read, write
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'third_party/llama.cpp-integrated/gguf-py'))
from gguf import GGUFReader


class CpuQwen:
    def __init__(self, profile):
        torch.set_num_threads(8);torch.set_grad_enabled(False)
        self.p=profile;self.reader=GGUFReader(profile['model_path'])
        self.tensors={t.name:t for t in self.reader.tensors};self.weights={}
        self.eps=float(self.reader.get_field('qwen3.attention.layer_norm_rms_epsilon').contents())
        self.base=float(self.reader.get_field('qwen3.rope.freq_base').contents())
        self.qh=profile['q_heads'];self.kh=profile['kv_heads'];self.d=profile['head_dim'];self.h=profile['hidden']

    def weight(self,name):
        if name not in self.weights:
            t=self.tensors[name]
            if t.tensor_type.name not in ['F16','F32']:raise ValueError('Only exact F16/F32 tensor values supported')
            self.weights[name]=torch.from_numpy(np.array(t.data,dtype=np.float32,copy=True))
        return self.weights[name]

    def norm(self,x,w):
        return x*torch.rsqrt((x*x).mean(dim=-1,keepdim=True)+self.eps)*w

    def rope(self,x,positions):
        half=self.d//2
        # The executor's RoPE configuration rounds the per-pair frequency ratio
        # to F32 before exponentiation. Preserve that public precision boundary.
        ratio=torch.tensor(self.base,dtype=torch.float32).pow(-2/self.d)
        frequencies=torch.pow(ratio,torch.arange(half,dtype=torch.float32))
        angles=positions.float()[:,None]*frequencies[None,:]
        c,s=torch.cos(angles)[:,None,:],torch.sin(angles)[:,None,:]
        a,b=x[...,:half],x[...,half:]
        return torch.cat((a*c-b*s,a*s+b*c),dim=-1)

    def run(self,ids,count,out):
        out=Path(out);out.mkdir(parents=True,exist_ok=False)
        cache_k=[None]*self.p['layers'];cache_v=[None]*self.p['layers']
        snapshots={};generated=[];raw=[];logits=[];past=0;inputs=ids;stop='length'
        eos={int(self.reader.get_field('tokenizer.ggml.eos_token_id').contents())}
        for key in ['tokenizer.ggml.eot_token_id','tokenizer.ggml.eom_token_id']:
            field=self.reader.get_field(key)
            if field is not None:eos.add(int(field.contents()))
        def save(step,name,value):snapshots[f'{step}:{name}']=value.detach().cpu().numpy().copy()
        for step in range(count):
            x=self.weight('token_embd.weight')[torch.tensor(inputs)]
            positions=torch.arange(past,past+len(inputs));n=len(inputs)
            for layer in range(self.p['layers']):
                prefix=f'blk.{layer}.'
                z=self.norm(x,self.weight(prefix+'attn_norm.weight'))
                q=(z@self.weight(prefix+'attn_q.weight').T).reshape(n,self.qh,self.d)
                k=(z@self.weight(prefix+'attn_k.weight').T).reshape(n,self.kh,self.d)
                v=(z@self.weight(prefix+'attn_v.weight').T).reshape(n,self.kh,self.d)
                q=self.rope(self.norm(q,self.weight(prefix+'attn_q_norm.weight')),positions)
                k=self.rope(self.norm(k,self.weight(prefix+'attn_k_norm.weight')),positions)
                save(step,f'Qcur-{layer}',q);save(step,f'Kcur-{layer}',k);save(step,f'Vcur-{layer}',v)
                ks=k.to(torch.float16);vs=v.to(torch.float16)
                cache_k[layer]=ks if cache_k[layer] is None else torch.cat((cache_k[layer],ks),dim=0)
                cache_v[layer]=vs if cache_v[layer] is None else torch.cat((cache_v[layer],vs),dim=0)
                save(step,f'Kstore-{layer}',cache_k[layer]);save(step,f'Vstore-{layer}',cache_v[layer])
                keys=cache_k[layer].float().repeat_interleave(self.qh//self.kh,dim=1)
                values=cache_v[layer].float().repeat_interleave(self.qh//self.kh,dim=1)
                scores=q.permute(1,0,2)@keys.permute(1,2,0)/math.sqrt(self.d)
                mask=torch.arange(past+n)[None,:]>positions[:,None]
                scores=scores.masked_fill(mask[None,:,:],-float('inf'))
                attended=(torch.softmax(scores,dim=-1)@values.permute(1,0,2)).permute(1,0,2).contiguous().reshape(n,self.qh*self.d)
                residual=x+attended@self.weight(prefix+'attn_output.weight').T
                z=self.norm(residual,self.weight(prefix+'ffn_norm.weight'))
                if layer==self.p['layer']:
                    save(step,f'ffn_inp-{layer}',residual);save(step,f'ffn_norm-{layer}',z)
                gate=z@self.weight(prefix+'ffn_gate.weight').T
                up=z@self.weight(prefix+'ffn_up.weight').T
                hidden=torch.nn.functional.silu(gate)*up
                x=residual+hidden@self.weight(prefix+'ffn_down.weight').T
                save(step,f'l_out-{layer}',x)
            final=self.norm(x[-1:],self.weight('output_norm.weight'))
            save(step,'result_norm',final)
            output_weight=self.weight('output.weight') if 'output.weight' in self.tensors else self.weight('token_embd.weight')
            output=(final@output_weight.T).flatten();logits.append(output.numpy().copy())
            token=int(torch.argmax(output));raw.append(token);past+=n
            if token in eos:stop='EOS';break
            generated.append(token);inputs=[token]
        np.asarray(logits,dtype='<f4').tofile(out/'logits.f32')
        np.savez(out/'snapshots.npz',**snapshots)
        result={'status':'completed','implementation':'Independent torch CPU Qwen3 equations / exact GGUF values / F32 compute / F16 KV',
                'torch':torch.__version__,'device':'cpu','prompt_ids':ids,'tokens':generated,'raw_tokens':raw,'stop_reason':stop,'positions':past,
                'cpu_threads':torch.get_num_threads(),'rms_epsilon':self.eps,'rope_base':self.base,'arrays':len(snapshots)}
        write(out/'summary.json',result);return result
