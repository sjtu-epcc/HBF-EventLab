"""S5-b with synchronized activity partitions, plus affected S2--S5 revalidation."""
import argparse
import copy
import shutil
import subprocess
from pathlib import Path
from datetime import datetime, timezone
from common import read, rows, write, sha
from study import ROOT, BASE, invoke, environment
from integrated_build_identity import verify_build
from audit import boundaries, numerical, stored_kv
from reference import CpuQwen
from s5_audit import positive, exact, normalized, negative
from segment_audit import audit as segment_audit, signature


def run(study):
    s2=ROOT/'output/gpu-s/20261101-s2-b2-v2-001'
    s3=ROOT/'output/gpu-s/20261101-s3-b2-v2-001'
    s4=ROOT/'output/gpu-s/20261101-s4-001'
    s5=ROOT/'output/gpu-s/20261101-s5-001'
    assert not study.exists();folder=study/'S5';folder.mkdir(parents=True)
    p=read(s2/'S0/profile.json');target=read(s2/'S0/numerical-targets-predeclared.json')
    regions=read(s4/'S4/M2-regions.json');binary=ROOT/'build-s3/bin/gpu_s_segmented.exe'
    assert sha(p['model_path'])==p['gguf_sha256']
    write(folder/'dependency-build-identity.json',verify_build(BASE))
    for prior,stage in [(s2,'S2'),(s3,'S3'),(s4,'S4')]:assert read(prior/stage/'stage-result.json')['status']=='pass'
    assert read(s5/'S5/stage-result.json')['S5-a']=='pass'
    prompt=folder/'prompt.txt';prompt.write_bytes((s2/'S0/inputs/main.txt').read_bytes())
    ini=folder/'effective-H0.ini';ini.write_bytes((s2/'S0/backend-preflight/effective-H0.ini').read_bytes())
    sources=[x for x in Path(__file__).parent.iterdir() if x.is_file()]+[binary,ROOT/'build-s3/CMakeCache.txt']
    snapshot=folder/'source-and-binary-snapshot';snapshot.mkdir()
    for x in sources:shutil.copyfile(x,snapshot/x.name)
    write(folder/'conditions.json',{'frozen_utc':datetime.now(timezone.utc).isoformat(),'binary_sha256':sha(binary),
        'source_hashes':{x.name:sha(x) for x in sources},'model_sha256':p['gguf_sha256'],
        'prompt_sha256':sha(prompt),'h0_sha256':sha(ini),'numerical_criteria':target,
        'prior_artifact_indexes':{str(d):sha(d/'artifact-index.json') for d in [s2,s3,s4,s5]},
        'output_limits':[2,8],'fixed_segment_ps':1000,'zero_control_ps':0,'M2_regions':regions,
        'partition':'Device synchronize and CUPTI flush at each gap/node/tail end. Nonempty production window = one segment; empty windows = zero; snapshot D2H bracketed separately and excluded.',
        'causality':'Close/advance preceding gap before gate; start node after gate; advance completed node before next gap; close/advance tail before token.',
        'stream_scope':'Exactly one visible CUDA device; cudaDeviceSynchronize joins all its streams.',
        'timing_scope':'Synthetic fixed segments only. Host wall time and verification D2H are excluded; no measured GPU performance claimed.',
        'revalidation':'S2 g/h and main; S3 M1/M8/failures; S4 M2/offset; S5 zero/fixed, verify/timing. CPU calibration references reused and hash-linked, main CPU rerun.',
        'core_host_device_modified':False})

    def execute(name,count=1,mode='verify',hbf=None,duration=0,prompt_path=None,expected=0):
        cfg=copy.deepcopy(p);cfg['hbf']=hbf or {'enabled':False};cfg['fixed_segment_ps']=duration
        config=folder/'configurations'/(name+'.json');write(config,cfg)
        out=folder/name;cmd=[binary,p['model_path'],prompt_path or prompt,out,count,mode,config]
        if expected==0:invoke(cmd,study/'logs'/name)
        else:
            logs=study/'logs'/name;logs.mkdir(parents=True)
            with (logs/'stdout.log').open('w') as so,(logs/'stderr.log').open('w') as se:
                r=subprocess.run(list(map(str,cmd)),env=environment(),stdout=so,stderr=se,timeout=3600)
            write(logs/'command.json',{'argv':list(map(str,cmd)),'returncode':r.returncode,'expected_returncode':expected})
            assert r.returncode==expected
        return out

    def hbf(rs=regions,**extra):return {'enabled':True,'effective_config_path':str(ini),'regions':rs,**extra}

    def numeric(out,cpu):
        assert stored_kv(out,p)['status']=='pass'
        result=numerical(out,cpu,p,target);assert result['status']=='pass'
        return result['arrays_compared']

    revalidation=[]
    for name in ['calibration-g','calibration-h']:
        out=execute(name,2,prompt_path=s2/'S0/inputs'/(name+'.txt'))
        boundaries(out,p);segment_audit(out,0);exact(s2/'S2'/(name+'-2'),out,p)
        n=numeric(out,s2/'S2'/(name+'-cpu'));revalidation.append({'case':name,'status':'pass','arrays':n})
        print(name,'S2 revalidation pass',flush=True)
    native1=execute('native-1');boundaries(native1,p);segment_audit(native1,0)
    exact(s2/'S2/main-1',native1,p);numeric(native1,s2/'S2/main-1-cpu')
    for name,prior,rs in [('M1',s3/'S3/M1',read(s3/'S3/M1/session.json')['regions']),
                           ('M8',s3/'S3/M8',read(s3/'S3/M8/session.json')['regions']),
                           ('M2',s4/'S4/M2',regions),
                           ('M2-offset',s4/'S4/M2-offset',read(s4/'S4/M2-offset-regions.json'))]:
        out=execute('revalidate-'+name,hbf=hbf(rs));seg=segment_audit(out,0)
        positive(out,native1,rs,p);exact(prior,out,p);n=numeric(out,s2/'S2/main-1-cpu')
        revalidation.append({'case':name,'status':'pass','arrays':n,'segments':seg['modeled_segments']})
        print(name,'functional revalidation pass',flush=True)
    m1=read(s3/'S3/M1/session.json')['regions']
    bad=execute('initialization-program-fail',hbf=hbf(m1,persistent_program_fail=True),expected=87)
    fail=negative(bad,'program-fail')
    t0=read(folder/'revalidate-M1/session.json')['request_t0_ps']
    bad=execute('request-time-limit',hbf=hbf(m1,max_tick_ps=t0+1),expected=88)
    limit=negative(bad,'time-limit')
    write(folder/'revalidation.json',{'status':'pass','cases':revalidation,'negative_cases':{'program_fail':fail,'time_limit':limit}})
    results=[];terminations=[]
    for count in [2,8]:
        native=execute(f'native-{count}',count,duration=1000)
        boundaries(native,p);sn=segment_audit(native,1000);exact(s5/'S5'/f'native-{count}',native,p)
        cpu=folder/f'cpu-{count}';ref=CpuQwen(p);ref.run(read(native/'summary.json')['prompt_ids'],count,cpu);del ref
        arrays=numeric(native,cpu)
        timing=execute(f'native-timing-{count}',count,mode='timing',duration=1000)
        segment_audit(timing,1000)
        assert signature(timing)==signature(native), 'Timing/verify production segment mismatch'
        assert (timing/'logits.f32').read_bytes()==(native/'logits.f32').read_bytes()
        assert normalized(rows(timing/'events.jsonl'))==normalized(rows(native/'events.jsonl'))
        assert read(timing/'summary.json')['tokens']==read(native/'summary.json')['tokens']
        for duration in [0,1000]:
            name=f'M2-{"zero" if duration==0 else "fixed"}-{count}'
            out=execute(name,count,hbf=hbf(),duration=duration)
            seg=segment_audit(out,duration);assert signature(out)==signature(native)
            behavior=positive(out,native,regions,p,seg['compute_by_step_ps']);assert numeric(out,cpu)==arrays
            summary=read(out/'summary.json')
            results.append({'case':name,'status':'pass','limit':count,'actual_outputs':len(summary['tokens']),
                'tokens':summary['tokens'],'stop_reason':summary['stop_reason'],'metrics':behavior['metrics'],
                'compute_ps':behavior['compute_ps'],'wait_ps':behavior['wait_ps'],'numerical_arrays':arrays,
                'segments':seg,'native_hbf_exact':behavior['gpu_native_exact'],'coverage':behavior['coverage']})
            term={'case':name,'requested_outputs':count,'actual_outputs':len(summary['tokens']),
                'stop_reason':summary['stop_reason'],'execution_steps':len(summary['raw_tokens']),
                'positions':summary['positions'],'extra_execution_step':False,'natural_EOS_observed':summary['stop_reason']=='EOS'}
            terminations.append(term)
            print(name,'pass',behavior['metrics'],'segments',seg['modeled_segments'],flush=True)
    write(folder/'termination-cases.json',{'real_GPU':terminations,'natural_EOS_observed':False,
        'N0_N1_EOS_table_mismatch_metric_fixtures':str(s2/'S1'),'evidence':'Real cases reached length 2/8. No natural EOS claim. S1 synthetic edge-case evidence retained.'})
    write(folder/'S6-main-case.json',{'status':'ready_for_S6_protocol','config':str(folder/'configurations/native-timing-8.json'),
        'config_sha256':sha(folder/'configurations/native-timing-8.json'),'binary_sha256':sha(binary),
        'production_segments_reference':str(folder/'native-timing-8/segments.jsonl'),
        'segments_sha256':sha(folder/'native-timing-8/segments.jsonl'),'tokens':read(folder/'native-8/summary.json')['tokens'],
        'fixed_times_are_not_G1':True,'S6_measured_sampling_not_executed':True})
    write(folder/'stage-result.json',{'stage':'S5','status':'pass','S5-a':'pass_revalidated','S5-b':'pass',
        'affected_S2_S4_revalidation':'pass','cases':results,'S6_started':False,'formal_performance_measured':False})
    write(study/'artifact-index.json',{'files':[{'path':x.relative_to(study).as_posix(),'bytes':x.stat().st_size,'sha256':sha(x)}
        for x in sorted(study.rglob('*')) if x.is_file() and x.name!='artifact-index.json']})
    print('S5-b complete; S6 not executed',flush=True)


if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--study',type=Path,required=True);a=ap.parse_args();run(a.study.resolve())
