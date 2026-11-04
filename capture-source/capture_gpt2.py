"""Real pretrained GPT-2, dataset-selected input and per-output trace boundaries.

Native wall-clock samples are diagnostic only, never simulated TTFT/TPOT.
"""
import argparse
import ctypes
import hashlib
import json
import re
import time
from pathlib import Path
import torch
import pyarrow.parquet as pq
from transformers import AutoModelForCausalLM, AutoTokenizer

def sha(path):
    h=hashlib.sha256()
    with open(path,'rb') as stream:
        for block in iter(lambda:stream.read(1<<20),b''):h.update(block)
    return h.hexdigest()

def profiler(start):
    driver=ctypes.CDLL('libcuda.so.1')
    rc=getattr(driver,'cuProfilerStart' if start else 'cuProfilerStop')()
    if rc:raise RuntimeError('CUDA profiler status '+str(rc))

def kernel_ids():
    return sorted(int(re.match(r'kernel-(\d+)-',p.name)[1]) for p in Path('traces').glob('kernel-*.trace.xz'))

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--model',default='/home/server/gpt2')
    parser.add_argument('--dataset',default='/home/server/wikitext/wikitext-2-raw-v1/test-00000-of-00001.parquet')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--capture',action='store_true')
    parser.add_argument('--prompt-tokens',type=int,default=8)
    parser.add_argument('--new-tokens',type=int,default=4)
    parser.add_argument('--embedding-lib',type=Path)
    args=parser.parse_args()
    if args.new_tokens<2:raise ValueError('TPOT needs at least two generated tokens')
    args.output.mkdir(parents=True,exist_ok=False)
    torch.set_num_threads(1);torch.manual_seed(0)
    torch.backends.cuda.matmul.allow_tf32=False
    torch.backends.cudnn.allow_tf32=False
    tokenizer=AutoTokenizer.from_pretrained(args.model,local_files_only=True)
    rows=pq.read_table(args.dataset,columns=['text']).column('text').to_pylist()
    for row_index,text in enumerate(rows):
        if not text.strip() or text.strip().startswith('='):continue
        tokens=tokenizer.encode(text,add_special_tokens=False)
        if len(tokens)>=args.prompt_tokens:break
    else:raise ValueError('dataset has no suitable non-heading prompt')
    tokens=tokens[:args.prompt_tokens]
    model=AutoModelForCausalLM.from_pretrained(args.model,local_files_only=True,
        torch_dtype=torch.float32,attn_implementation='eager').cuda().eval()
    if args.embedding_lib:
        library=ctypes.CDLL(str(args.embedding_lib.resolve()))
        function=library.hbf_embedding
        function.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_longlong,ctypes.c_longlong,ctypes.c_int,ctypes.c_ulonglong]
        function.restype=ctypes.c_int
        def attach(layer):
            original=layer.forward
            def forward(indices):
                if indices.dtype!=torch.int64 or not indices.is_cuda or not indices.is_contiguous():
                    raise ValueError('embedding indices must be contiguous CUDA int64')
                weight=layer.weight
                if weight.dtype!=torch.float32 or not weight.is_contiguous():raise ValueError('embedding weights must be contiguous FP32')
                result=torch.empty((*indices.shape,weight.shape[1]),device=indices.device,dtype=weight.dtype)
                rc=function(weight.data_ptr(),indices.data_ptr(),result.data_ptr(),indices.numel(),weight.shape[0],weight.shape[1],torch.cuda.current_stream().cuda_stream)
                if rc:raise RuntimeError('embedding CUDA error '+str(rc))
                return result
            if not args.capture:
                test=torch.tensor([[0,1,layer.weight.shape[0]-1]],device='cuda',dtype=torch.long)
                if not torch.equal(forward(test),original(test)):raise ValueError('custom embedding differs from PyTorch')
            layer.forward=forward
        attach(model.transformer.wte);attach(model.transformer.wpe)
    weights=[]
    for name,p in model.named_parameters():
        weights.append(dict(names=[name],address=p.data_ptr(),bytes=p.numel()*p.element_size(),shape=list(p.shape),dtype=str(p.dtype)))
    (args.output/'weights.json').write_text(json.dumps(weights,indent=2))
    current=torch.tensor([tokens],device='cuda',dtype=torch.long)
    torch.cuda.synchronize()
    print('MODEL_READY',len(tokens),'prompt tokens',sum(x['bytes'] for x in weights),'weight bytes',flush=True)
    previous=None;generated=[];boundaries=[];native=[];past=None
    with torch.inference_mode():
        for index in range(args.new_tokens):
            before=set(kernel_ids())
            if args.capture:profiler(True)
            begin=time.perf_counter()
            state=model.transformer(input_ids=current,past_key_values=past,use_cache=True,return_dict=True)
            # GPT-2's tied LM head is only needed for the last position.
            logits=model.lm_head(state.last_hidden_state[:,-1:,:])
            token=logits[:,-1,:].argmax(dim=-1)
            torch.cuda.synchronize()
            native.append(time.perf_counter()-begin)
            if args.capture:profiler(False)
            after=set(kernel_ids());selected=sorted(after-before)
            if args.capture and not selected:raise RuntimeError('No GPU instruction trace for output token '+str(index))
            token_id=int(token.item());generated.append(token_id)
            boundaries.append(dict(output_index=index,phase='prefill' if index==0 else 'decode',
                token_id=token_id,kernel_ids=selected))
            (args.output/'token_boundaries.json').write_text(json.dumps(boundaries,indent=2))
            print('TOKEN_COMPLETE',index,token_id,'kernels',len(selected),flush=True)
            past=state.past_key_values
            current=token.reshape(1,1)
            if token_id==tokenizer.eos_token_id:break
    if len(generated)<2:raise RuntimeError('EOS before TPOT can be measured')
    report=dict(model='GPT-2 124M',model_path=args.model,model_sha256=sha(Path(args.model)/'model.safetensors'),
        dataset=args.dataset,dataset_sha256=sha(args.dataset),dataset_row=row_index,
        prompt=tokenizer.decode(tokens),prompt_token_ids=tokens,generated_token_ids=generated,
        generated_text=tokenizer.decode(generated),precision='float32',batch_size=1,
        torch=torch.__version__,transformers=__import__('transformers').__version__,cuda=torch.version.cuda,
        gpu=torch.cuda.get_device_name(),capture=args.capture,weight_bytes=sum(w['bytes'] for w in weights),
        embedding_backend='standalone_cuda' if args.embedding_lib else 'pytorch',
        embedding_library_sha256=sha(args.embedding_lib) if args.embedding_lib else None,
        native_instrumented_wall_seconds=native,token_boundaries=boundaries,
        timing_boundary='GPU prefill/last-position LM head/argmax; then cached autoregressive decode; excludes tokenization, loading and host launch overhead',
        stop_policy='EOS or requested count; no forward pass after last output')
    (args.output/'result.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2),flush=True)

if __name__=='__main__':main()
