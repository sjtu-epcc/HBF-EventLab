"""Run independent CUDA numerical branches, PTX waits, and fail-closed profiles."""
import copy
import argparse
import json
import subprocess
from pathlib import Path
from run_llama_integrated import ROOT, environment, read, write, require, ceildiv
from integrated_build_identity import add_build_arguments, verify_build, records
from setup_llama_baseline import sha
from platform_environment import executable


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--profiles-dir',type=Path,default=ROOT/'configs/integrated');parser.add_argument('--output',type=Path,default=ROOT/'output/llama-m6/checks');add_build_arguments(parser);a=parser.parse_args()
    identity=verify_build(a.build_dir,a.expected_backend)
    out=a.output.resolve();require(not out.exists(),'choose a new checks directory');out.mkdir(parents=True)
    write(out/'build-identity.json',identity)
    env=environment(a.build_dir,a.cuda_root,a.gpu_device);build=a.build_dir.resolve()/'bin';results=[]
    profiles=[(name,read(a.profiles_dir/f'{name}.json')) for name in ['qwen3-0.6b','qwen3-1.7b','smollm2-135m']]
    # Synthetic layouts isolate dimensions and row fragmentation, independent of model logits.
    for name,hd,q,kv,block in [('mha32',32,5,5,16),('gqa256',256,6,3,4),('wide-row',128,24,24,3),('three-page-row',128,25,25,3)]:
        p=copy.deepcopy(profiles[0][1]);p.update(layers=2,head_dim=hd,q_heads=q,kv_heads=kv,block_tokens=block)
        row=kv*hd*2;p['row_bytes']=row;p['logical_pages']=2*2*ceildiv(p['context'],block)*ceildiv(row*block,4096)
        p['frames']=2*max(ceildiv(t*row%4096+row,4096) for t in range(block));profiles.append((name,p))
    for name,p in profiles:
        path=out/(name+'.json');write(path,p);folder=out/name
        cmd=[str(executable(build,'realgpu_attention_test')),str(path),str(folder)]
        r=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=120)
        write(out/(name+'-run.json'),{'command':cmd,'returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
        require(r.returncode==0,'attention oracle failed '+name+': '+r.stderr)
        results.append({'case':name,'result':read(folder/'oracle.json')});print(name+' oracle passed',flush=True)
    for width in [576,1024,2048]:
        folder=out/('ptx-'+str(width));cmd=[str(executable(build,'realgpu_ptx_test')),str(width),str(folder)]
        r=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=120)
        write(out/(f'ptx-{width}-run.json'),{'command':cmd,'returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
        require(r.returncode==0,'PTX wait/oracle failed: '+r.stderr)
        results.append({'case':f'ptx-{width}','result':read(folder/'validation.json')});print(f'PTX {width} passed',flush=True)
    for mode in ['position','shape']:
        folder=out/('input-'+mode)
        r=subprocess.run([str(executable(build,'realgpu_attention_test')),str(a.profiles_dir/'smollm2-135m.json'),str(folder),mode],env=env,capture_output=True,text=True,timeout=30)
        require(r.returncode==0,'input rejection failed '+mode+': '+r.stderr)
        results.append({'case':'input-'+mode,'result':read(folder/'rejection.json')})
    rejected=[('frames',{'frames':1},'page/frame'),('layers',{'layers':0},'model dimensions'),
              ('gqa',{'q_heads':17},'head dimensions'),('audit',{'budgets':dict(profiles[0][1]['budgets'],audit_records=1)},'audit/event'),
              ('dtype',{'kv_dtype':'Q8_0'},'rope/dtype'),('architecture',{'architecture':'unknown'},'architecture')]
    for name,mutation,message in rejected:
        p=copy.deepcopy(profiles[0][1]);p.update(mutation);path=out/(name+'-reject.json');write(path,p)
        folder=out/(name+'-reject');r=subprocess.run([str(executable(build,'realgpu_attention_test')),str(path),str(folder)],env=env,capture_output=True,text=True,timeout=30)
        require(r.returncode!=0 and message in r.stderr and not (folder/'kv-summary.json').exists(),'missing early rejection '+name)
        results.append({'case':name,'rejected':True,'diagnostic':r.stderr.strip()})
    require(verify_build(a.build_dir,a.expected_backend)==identity,'check build changed')
    write(out/'validation.json',{'status':'pass','cases':results,'backend_commit':identity['backend']['commit'],'build_identity_sha256':sha(out/'build-identity.json')})
    write(out/'manifest.json',{'backend_commit':identity['backend']['commit'],'build_identity_sha256':sha(out/'build-identity.json'),
                              'files':records([f for f in out.rglob('*') if f.is_file()])})


if __name__=='__main__':main()
