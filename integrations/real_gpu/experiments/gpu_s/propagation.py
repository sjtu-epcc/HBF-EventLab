"""Conditional, conservative full-network roundoff radii; never fitted to GPU errors.

Elementary nonlinear error allowance is an explicit unverified assumption.
This executable model tests whether such bounds are useful, not a proof of CUDA libm.
"""
import math
import numpy as np
import torch
from reference import CpuQwen
from common import read,write
U=2**-24
def gamma(n):return n*U/(1-n*U)

class Ball:
    def __init__(self,c,r=0):
        self.c=c.double();self.r=torch.zeros_like(self.c)+r
    def reshape(self,*shape):return Ball(self.c.reshape(*shape),self.r.reshape(*shape))
    def permute(self,*axes):return Ball(self.c.permute(*axes),self.r.permute(*axes))
    def __add__(self,b):
        c=self.c+b.c;r=self.r+b.r
        return Ball(c,r+gamma(2)*(self.c.abs()+b.c.abs()+r)+2**-149)
    def __mul__(self,b):
        c=self.c*b.c;r=self.c.abs()*b.r+self.r*b.c.abs()+self.r*b.r
        return Ball(c,r+gamma(2)*(c.abs()+r)+2**-149)

def mm(a,b):
    c=a.c@b.c
    absolute=a.c.abs()@b.c.abs()
    propagated=a.r@b.c.abs()+a.c.abs()@b.r+a.r@b.r
    return Ball(c,propagated+gamma(2*a.c.shape[-1])*(absolute+propagated)+2**-149)

class Propagation(CpuQwen):
    def linear(self,x,name):return mm(x,Ball(self.weight(name).double().T))
    def bn(self,x,w):
        w=w.double();n=x.c.shape[-1]
        center=x.c*torch.rsqrt(x.c.square().mean(-1,keepdim=True)+self.eps)*w
        lo=(x.c.abs()-x.r).clamp_min(0);hi=x.c.abs()+x.r
        low_mean=lo.square().mean(-1,keepdim=True)*(1-gamma(2*n))
        high_mean=hi.square().mean(-1,keepdim=True)*(1+gamma(2*n))
        dlow=(low_mean+self.eps).sqrt();dhigh=(high_mean+self.eps).sqrt()
        # Componentwise interval division; normalized component also obeys sqrt(n).
        candidates=torch.stack([(x.c-x.r)/dlow,(x.c-x.r)/dhigh,(x.c+x.r)/dlow,(x.c+x.r)/dhigh])
        cap=math.sqrt(n/(1-gamma(2*n)))
        lower=candidates.amin(0).clamp(-cap,cap)
        upper=candidates.amax(0).clamp(-cap,cap)
        radius=torch.maximum((lower*w-center).abs(),(upper*w-center).abs())
        return Ball(center,radius+64*U*(center.abs()+radius)+2**-149)
    def br(self,x,positions):
        # Same public F32 frequency-ratio boundary; CPU-only trigonometry.
        half=self.d//2;ratio=torch.tensor(self.base,dtype=torch.float32).pow(-2/self.d).double()
        freq=ratio.pow(torch.arange(half,dtype=torch.float64));theta=positions.double()[:,None]*freq
        angle_error=32*U*(theta.abs()+1)
        cb=Ball(theta.cos()[:,None,:],angle_error[:,None,:]+32*U)
        sb=Ball(theta.sin()[:,None,:],angle_error[:,None,:]+32*U)
        a,b=Ball(x.c[...,:half],x.r[...,:half]),Ball(x.c[...,half:],x.r[...,half:])
        left=a*cb+Ball(-(b*sb).c,(b*sb).r);right=a*sb+b*cb
        return Ball(torch.cat([left.c,right.c],-1),torch.cat([left.r,right.r],-1))
    def quantize(self,x,key):
        overflow=bool(((x.c.abs()+x.r)>65504).any())
        if overflow:self.overflow.append(key)
        center=x.c.half().double()
        radius=x.r+2**-11*(x.c.abs()+x.r)+2**-25+(center-x.c).abs()
        # No clipping: possible overflow remains an explicit failed premise.
        return Ball(center,radius)
    def execute(self,ids,tokens,out):
        out.mkdir();self.overflow=[];cachek=[None]*self.p['layers'];cachev=[None]*self.p['layers'];records={};past=0
        def save(step,name,b):records[f'{step}:{name}:center']=b.c.numpy();records[f'{step}:{name}:radius']=b.r.numpy()
        for step in range(len(tokens)):
            inputs=ids if step==0 else [tokens[step-1]];n=len(inputs);pos=torch.arange(past,past+n)
            x=Ball(self.weight('token_embd.weight')[torch.tensor(inputs)])
            for l in range(self.p['layers']):
                prefix=f'blk.{l}.';z=self.bn(x,self.weight(prefix+'attn_norm.weight'))
                q=self.linear(z,prefix+'attn_q.weight').reshape(n,self.qh,self.d)
                k=self.linear(z,prefix+'attn_k.weight').reshape(n,self.kh,self.d)
                v=self.linear(z,prefix+'attn_v.weight').reshape(n,self.kh,self.d)
                q=self.br(self.bn(q,self.weight(prefix+'attn_q_norm.weight')),pos)
                k=self.br(self.bn(k,self.weight(prefix+'attn_k_norm.weight')),pos)
                for kind,b in [('Q',q),('K',k),('V',v)]:save(step,f'{kind}cur-{l}',b)
                for kind,b,cache in [('K',k,cachek),('V',v,cachev)]:
                    b=self.quantize(b,f'{step}:{kind}:{l}')
                    if cache[l] is not None:b=Ball(torch.cat([cache[l].c,b.c]),torch.cat([cache[l].r,b.r]))
                    cache[l]=b;save(step,f'{kind}store-{l}',b)
                def heads(b):return Ball(b.c.repeat_interleave(self.qh//self.kh,1),b.r.repeat_interleave(self.qh//self.kh,1))
                keys,values=heads(cachek[l]),heads(cachev[l])
                score=mm(q.permute(1,0,2),keys.permute(1,2,0))*Ball(torch.tensor(1/math.sqrt(self.d)))
                mask=torch.arange(past+n)[None,:]>pos[:,None]
                center=torch.softmax(score.c.masked_fill(mask[None,:,:],-float('inf')),-1)
                error=score.r.masked_fill(mask[None,:,:],0).amax(-1,keepdim=True)
                # Softmax Jacobian infinity norm <=1/2; probability error <=1.
                prob=Ball(center,(error*.5+64*U).clamp_max(1+64*U).expand_as(center).masked_fill(mask[None,:,:],0))
                attended=mm(prob,values.permute(1,0,2)).permute(1,0,2).reshape(n,self.qh*self.d)
                residual=x+self.linear(attended,prefix+'attn_output.weight');z=self.bn(residual,self.weight(prefix+'ffn_norm.weight'))
                if l==self.p['layer']:save(step,f'ffn_inp-{l}',residual);save(step,f'ffn_norm-{l}',z)
                gate=self.linear(z,prefix+'ffn_gate.weight');up=self.linear(z,prefix+'ffn_up.weight')
                # |SiLU'| <= 2, deliberately conservative and independent of samples.
                silu=Ball(torch.nn.functional.silu(gate.c),2*gate.r+64*U*(gate.c.abs()+gate.r)+2**-149)
                x=residual+self.linear(silu*up,prefix+'ffn_down.weight');save(step,f'l_out-{l}',x)
            final=self.bn(Ball(x.c[-1:],x.r[-1:]),self.weight('output_norm.weight'));save(step,'result_norm',final)
            logits=self.linear(final,'output.weight' if 'output.weight' in self.tensors else 'token_embd.weight');save(step,'logits',logits)
            past+=n
        np.savez(out/'bounds.npz',**records)
        write(out/'model-result.json',{'status':'conditional_model_only','possible_F16_overflow':self.overflow,
              'all_radii_finite':all(np.isfinite(v).all() for k,v in records.items() if k.endswith(':radius')),
              'nonlinear_error_assumptions_verified':False,'certified_bound':False})
