"""Profile-derived, independent M6 accounting and three-mode numerical acceptance."""
import argparse
import json
import os
import subprocess
import time
from pathlib import Path
import numpy as np
from integrated_profile import ROOT, derive, require, ceildiv
from setup_llama_baseline import sha
from platform_environment import executable, runtime_environment
from integrated_build_identity import DEFAULT_BUILD, add_build_arguments, verify_build


def read(p):
    return json.loads(p.read_text(encoding='utf-8'))


def write(p, value):
    p.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def rows(p):
    with p.open(encoding='utf-8') as f:
        return [json.loads(line) for line in f if line.strip()]


def environment(build=DEFAULT_BUILD, cuda_root=None, gpu_device='0'):
    return runtime_environment(build, ROOT, cuda_root, gpu_device)


def page_plan(length, p):
    first=0;groups=0;split=0;row=p['row_bytes'];block=p['block_tokens']
    while first<length:
        offset=(first%block)*row%4096
        pages=ceildiv(offset+row,4096)
        count=1 if pages>1 else (4096-offset)//row
        count=min(count,length-first,block-first%block)
        require(count>0,'bad independent page plan')
        groups+=1;split+=pages>1;first+=count
    return groups,split


def transactions(prompt, generated, ubatch):
    result=[];past=0
    for step in range(generated):
        left=prompt if step==0 else 1
        while left:
            n=min(left,ubatch);result.append((step,past,n));past+=n;left-=n
    return result


def validate(out):
    p=read(out/'profile.json');identity=sha(out/'profile.json');layers=p['layers'];hidden=p['hidden'];ffn=p['ffn']
    native=read(out/'native/summary.json');count=len(native['generated_tokens']);prompt=len(native['prompt_tokens']);length=prompt+count-1
    tx=transactions(prompt,count,p['ubatch']);orig=np.fromfile(out/'native/logits.f32',dtype='<f4')
    require(orig.size==count*native['vocab_size'] and np.isfinite(orig).all(),'native logits shape/finite')
    require(read(out/'native/hbf-summary.json')['kv_policy']=='native GPU KV reference','native KV policy')
    reports={};bypass=None
    for mode in ['native','bypass','modeled']:
        folder=out/mode;s=read(folder/'summary.json');h=read(folder/'hbf-summary.json')
        require(s['profile_sha256']==h['profile_sha256']==identity and sha(folder/'profile.json')==identity,'profile binding mismatch')
        require(s['generated_tokens']==native['generated_tokens'] and s['prompt_tokens']==native['prompt_tokens'],'native token mismatch '+mode)
        require(h['borrowed_unchanged'] and h['steps']==count,'borrowed lifetime/step mismatch')
        if mode=='native':continue
        a=np.fromfile(folder/'logits.f32',dtype='<f4');require(a.shape==orig.shape and np.isfinite(a).all(),'invalid logits')
        delta=a.astype(np.float64)-orig;rms=float(np.sqrt(np.mean(delta*delta)));err=float(np.max(np.abs(delta)))
        scale=float(np.sqrt(np.mean(orig.astype(np.float64)**2)));num=p['numerical']
        if 'max_abs' in num:
            require(err<=num['max_abs'] and rms<=num['rms'],'frozen A tolerance exceeded')
        else:
            require(scale>0 and rms/scale<=num['relative_l2'] and err/scale<=num['max_abs_over_native_rms'],'predeclared normalized tolerance exceeded')
        if mode=='bypass':bypass=a
        else:require(np.array_equal(bypass,a),'modeled changed paged arithmetic')
        kv=read(folder/'kv-summary.json');released=read(folder/'kv-release.json')
        require(kv['profile_sha256']==identity and kv['length']==length and kv['layers']==layers and kv['steps']==count,'KV shape/length')
        require(kv['transactions']==len(tx) and kv['appended_layer_tokens']==layers*length,'KV publication count')
        allocated=layers*2*ceildiv(length,p['block_tokens'])*ceildiv(p['block_tokens']*p['row_bytes'],4096)
        require(kv['allocated_pages']==allocated and kv['frame_bytes']==p['frames']*4096 and kv['native_gpu_kv_bytes']==0,'finite frames/allocations')
        require(allocated>p['frames'] and kv['evictions']>0 and kv['writebacks']>0 and kv['faults']>allocated,'missing eviction/writeback/reread')
        require(kv['dirty_frames']==kv['pins']==0 and all(v==0 for v in released.values()),'resource leak')
        require(kv['kv_commit_d2h_bytes']==layers*length*p['row_bytes']*2,'KV copy count')
        calls=rows(folder/'kv-attention.jsonl');require(len(calls)==layers*len(tx),'layer coverage')
        total_pairs=0;total_split=0
        for i,c in enumerate(calls):
            step,past,n=tx[i//layers];pairs,split=page_plan(past+n,p)
            require((c['step'],c['layer'],c['past'],c['batch'],c['committed'],c['visible'])==(step,i%layers,past,n,past,past+n),'transaction/layer publication')
            require(c['page_pairs']==pairs and c['cross_page_rows']==split and c['frame_bytes']==p['frames']*4096 and c['read_faults']>0,'page consumption plan')
            total_pairs+=pairs;total_split+=split
        require(kv['page_pairs']==total_pairs and kv['cross_page_rows']==total_split,'page totals')
        require('KV buffer size' not in (out/(mode+'.stderr.log')).read_text(errors='replace'),'native KV still allocated')
        require(h['kv_policy']=='paged F16 KV through Capacity','KV policy')
        reports[mode]={'native_tokens_exact':True,'native_max_abs_error':err,'native_rms_error':rms,'relative_l2':rms/scale,'max_abs_over_native_rms':err/scale,'kv':kv,'released':released}
    folder=out/'modeled';h=read(folder/'hbf-summary.json');runtime=h['runtime'];backend=h['backend']
    require(backend['interface']=='SimulationSession -> HostDriver -> DeviceDriver' and backend['input_closed'],'incremental path/close')
    require(backend['registered_globals']==backend['completed_globals'] and backend['host_logical_failed']==0,'Global final completion accounting')
    require(backend['registered_fragments']==backend['host_logical_success'] and backend['registered_fragments']>0,'Host fragment outcome accounting')
    require(backend['peak_resident_fragments']<=backend['resident_fragment_limit'],'Host resident budget exceeded')
    host=rows(folder/'hbf-host.jsonl');device=rows(folder/'hbf-device.jsonl');media=rows(folder/'kv-media.jsonl')
    helper_per_row=ffn//64;weight_count=length*(hidden+helper_per_row)
    require(len(host)==runtime['submitted']==runtime['completed']==weight_count and len(device)==length*helper_per_row,'weight coverage')
    require(runtime['drained'] and runtime['audit_dropped']==0 and runtime['launches_submitted']==runtime['launches_completed'],'runtime drain')
    require(len(media)==runtime['media_submitted']==runtime['media_completed']==reports['modeled']['kv']['media_records'],'media completion')
    require([m['ticket'] for m in media]==list(range(1,len(media)+1)),'media identities')
    logical_base=(ceildiv(hidden*4,4096)+ceildiv(ffn*2,4096))*4096
    for m in media:
        require(m['producer']==p['budgets']['producer_slots'] and logical_base<=m['logical']<logical_base+p['logical_pages']*4096 and m['bytes']==4096 and m['arrival_ns']>=m['floor_ns'],'media bounds/admission')
        require(m['resumed_wall_ns']>=m['completion_wall_ns']>=m['ready_wall_ns'] and m['resumed_wall_ns']-m['ready_wall_ns']>=m['delay_ns'],'media actual wait')
    for d in device:
        require(d['status']==0 and d['resumed_ns']>=d['completion_seen_ns']>=d['arrival_ns']>=d['floor_ns'] and d['resumed_ns']-d['arrival_ns']>=d['delay_ns'],'helper device wait')
    combined=sorted(host+media,key=lambda x:x['dispatch']);require([x['dispatch'] for x in combined]==list(range(1,len(combined)+1)),'dispatch gaps')
    keys=[(x['arrival_ns'],x['producer'],x['ticket']) for x in combined];require(keys==sorted(keys),'shared arrival order')
    for x in combined:require(x['arrival_ps']==runtime['model_epoch_ps']+(x['arrival_ns']-runtime['gpu_epoch_ns'])*1000,'shared model clock')
    reads=sum(not m['write'] for m in media);writes=len(media)-reads
    require(backend['requests']==weight_count+reads and backend['sectors']==length*(hidden+ffn//32)+reads*64,'read billing')
    require(backend['writebacks']==writes==reports['modeled']['kv']['writebacks'] and backend['accepted']==backend['delivered'],'write billing/drain')
    require(writes<=backend['replayed_pages']<=p['pages_per_block']*writes,'replay budget')
    init=read(folder/'hbf-initialization.json');programs=p['logical_pages']+logical_base//4096
    require(init['programs']==programs and backend['accepted']==programs+backend['sectors']+backend['replayed_pages'],'unified command accounting')
    module=read(folder/'ptx-module.json');ptx=rows(folder/'ptx-calls.jsonl');ps=read(folder/'ptx-summary.json')
    require(module['frontend']==4 and module['protocol']==1 and module['source_sha256']==sha(folder/'weight_mul.ptx') and module['rewritten_sha256']==sha(folder/'weight_mul.rewritten.ptx'),'PTX identities')
    require(len(ptx)==ps['rows']==length and ps['modeled_requests']==length*hidden,'PTX rows')
    launches={}
    for t in host:launches.setdefault(t['launch'],[]).append(t)
    for call in ptx:
        trace=launches.pop(call['launch'],[]);base=call['weight']
        require(len(trace)==hidden and sorted(t['address']-base for t in trace)==list(range(0,hidden*4,4)),'PTX byte coverage')
        require(all(t['bytes']==4 and t['span_count']==1 and t['sectors']==1 for t in trace),'PTX access semantics')
        require(call['modeled_requests']==hidden and call['bytes']==hidden*4,'PTX report mismatch')
    helpers=rows(folder/'hbf-calls.jsonl')
    for call in helpers:
        trace=launches.pop(call['launch'],[]);base=call['address'];n=call['rows']
        require(call['kind']==1 and call['range_bytes']==ffn*2 and len(trace)==n*helper_per_row,'helper launch coverage')
        require(sorted(t['address']-base for t in trace)==sorted(list(range(0,ffn*2,128))*n),'helper byte coverage')
        require(all(t['bytes']==128 and t['sectors']==2 and t['span_count']==1 for t in trace),'helper span semantics')
    require(not launches,'unexpected weight launches')
    for mode in ['native','bypass']:
        require(read(out/mode/'ptx-summary.json')['rows']==length and read(out/mode/'ptx-module.json')['source_sha256']==module['source_sha256'],'reference PTX mismatch')
    kernels=rows(folder/'kernels.jsonl')
    require(sum('page_attention' in k['name'] for k in kernels)==reports['modeled']['kv']['page_pairs'],'actual page kernels')
    require(sum('m6_weight_mul' in k['name'] for k in kernels)==length,'actual PTX kernels')
    result={'status':'pass','model_id':p['model_id'],'architecture':p['architecture'],'profile_sha256':identity,'tokens':count,'prompt_tokens':prompt,
            'numerical_contract':p['numerical'],'comparisons':reports,'paged_bypass_modeled_logits_bitwise_equal':True,
            'ptx_weight_requests':length*hidden,'helper_weight_requests':len(device),'media_reads':reads,'media_writes':writes,
            'shared_arrival_clock_billing_verified':True,'backend':backend,'scope':p['coverage']}
    write(out/'validation.json',result);return result


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--profile',type=Path);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--prompt',type=Path,default=ROOT/'adapters/llama_cpp/prompt.txt');parser.add_argument('--tokens',type=int,default=32)
    parser.add_argument('--validate-only',action='store_true');add_build_arguments(parser);a=parser.parse_args();out=a.output.resolve()
    if a.validate_only:print(json.dumps(validate(out),indent=2));return
    identity=verify_build(a.build_dir,a.expected_backend)
    require(a.profile is not None,'profile required');p=read(a.profile)
    # Re-derive from actual GGUF on every run. A hand-edited profile cannot weaken capabilities or expectations.
    expected=derive(Path(p['model_path']),Path(p['conversion']),context=p['context'],ubatch=p['ubatch'],block_tokens=p['block_tokens'],frames=p['frames'],layer=p['layer'])
    require(p==expected,'profile differs from actual GGUF/capability contract')
    require(not out.exists(),'choose a fresh evidence directory');out.mkdir(parents=True)
    (out/'profile.json').write_bytes(a.profile.read_bytes());(out/'prompt.txt').write_bytes(a.prompt.read_bytes())
    write(out/'build-identity.json',identity)
    write(out/'predeclared.json',{'numerical':p['numerical'],'profile_sha256':sha(out/'profile.json'),'prompt_sha256':sha(out/'prompt.txt'),'tokens':a.tokens,'created_unix':time.time(),'backend_commit':identity['backend']['commit'],'build_identity_sha256':sha(out/'build-identity.json')})
    build=a.build_dir.resolve();env=environment(build,a.cuda_root,a.gpu_device);commands=[]
    for mode in ['native','bypass','modeled']:
        cmd=[str(executable(build/'bin','realgpu_llama')),p['model_path'],str(out/'prompt.txt'),str(out/mode),str(a.tokens),'1' if mode=='modeled' else '0',mode,str(out/'profile.json')]
        commands.append(cmd);print('Running '+p['model_id']+' '+mode,flush=True)
        with (out/(mode+'.stdout.log')).open('w',encoding='utf-8') as so,(out/(mode+'.stderr.log')).open('w',encoding='utf-8') as se:
            result=subprocess.run(cmd,env=env,cwd=build/'bin',stdout=so,stderr=se,timeout=3600)
        require(result.returncode==0,mode+' failed: '+str(result.returncode))
    result=validate(out)
    require(verify_build(build,a.expected_backend)==identity,'build changed during model execution')
    files=list(out.rglob('*'))+list((build/'bin').glob('*'))+list(build.glob('*.ptx'))+[build/'CMakeCache.txt']
    files+=list((ROOT/'adapters/llama_cpp').glob('integrated*'))+list((ROOT/'tools').glob('*integrated*.py'))+[ROOT/'adapters/llama_cpp/integrated/CMakeLists.txt',ROOT/'third_party/llama.cpp-integrated/hbflab-overlay.json']
    files += [ROOT/'third_party/llama.cpp-integrated'/n for n in read(ROOT/'third_party/llama.cpp-integrated/hbflab-overlay.json')['modified_sha256']]
    write(out/'manifest.json',{'commands':commands,'backend':identity['backend'],'build_directory':str(build),'build_identity_sha256':sha(out/'build-identity.json'),'gguf_sha256':p['gguf_sha256'],'profile_sha256':sha(out/'profile.json'),'files':[{'path':str(f.resolve()),'bytes':f.stat().st_size,'sha256':sha(f)} for f in sorted(set(files)) if f.is_file()]})
    print(json.dumps({'status':result['status'],'tokens':result['tokens'],'prompt_tokens':result['prompt_tokens']}),flush=True)


if __name__=='__main__':main()
