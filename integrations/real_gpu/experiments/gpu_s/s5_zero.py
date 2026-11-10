"""S5-a zero-compute functional cases; S5-b remains a separate prerequisite."""
import argparse,copy
from pathlib import Path
from common import read,write,sha
from study import ROOT,BASE,invoke, JOINT_BUILD
from audit import numerical,boundaries,stored_kv
from s5_audit import positive
from reference import CpuQwen
from platform_environment import executable
from integrated_build_identity import verify_build

def run(s4,s2,study):
    assert read(s4/'S4/stage-result.json')['status']=='pass' and read(s2/'S2/stage-result.json')['status']=='pass'
    assert not study.exists();folder=study/'S5';folder.mkdir(parents=True)
    p=read(s2/'S0/profile.json');target=read(s2/'S0/numerical-targets-predeclared.json');regions=read(s4/'S4/M2-regions.json')
    verify_build(BASE);assert sha(p['model_path'])==p['gguf_sha256']
    binary=executable(JOINT_BUILD/'bin','gpu_s_joint');prompt=folder/'prompt.txt';prompt.write_bytes((s2/'S0/inputs/main.txt').read_bytes())
    ini=folder/'effective-H0.ini';ini.write_bytes((s2/'S0/backend-preflight/effective-H0.ini').read_bytes())
    write(folder/'conditions.json',{'s4_result_sha256':sha(s4/'S4/stage-result.json'),'target_sha256':sha(s2/'S0/numerical-targets-predeclared.json'),
        'binary_sha256':sha(binary),'model_sha256':p['gguf_sha256'],'prompt_sha256':sha(prompt),'h0_sha256':sha(ini),
        'source_hashes':{x.name:sha(x) for x in Path(__file__).parent.iterdir() if x.is_file()},
        'M2_regions':regions,'compute_ps':0,'output_limits':[2,8],'scope':'S5-a only; S5-b fixed compute remains required'})
    results=[]
    for count in [2,8]:
        native=folder/f'native-{count}';gpu=folder/f'M2-zero-{count}';cpu=folder/f'cpu-{count}'
        for name,out,hbf in [('native',native,False),('hbf',gpu,True)]:
            cfg=copy.deepcopy(p);cfg['hbf']={'enabled':True,'effective_config_path':str(ini),'regions':regions} if hbf else {'enabled':False}
            config=folder/f'{name}-{count}-config.json';write(config,cfg)
            invoke([binary,p['model_path'],prompt,out,count,'verify',config],study/'logs'/f'{name}-{count}')
        bs=boundaries(native,p);assert stored_kv(native,p)['status']=='pass' and stored_kv(gpu,p)['status']=='pass'
        c=CpuQwen(p);c.run(read(native/'summary.json')['prompt_ids'],count,cpu);del c
        cpu_native=numerical(native,cpu,p,target);cpu_gpu=numerical(gpu,cpu,p,target)
        behavior=positive(gpu,native,regions,p)
        assert cpu_native['status']==cpu_gpu['status']=='pass'
        summary=read(gpu/'summary.json');results.append({'limit':count,'tokens':summary['tokens'],'raw_tokens':summary['raw_tokens'],
            'stop_reason':summary['stop_reason'],'positions':summary['positions'],'status':'pass','metrics':behavior['metrics'],
            'coverage':behavior['coverage'],'numeric_arrays':cpu_gpu['arrays_compared'],'zero_compute_conservation_ps':behavior['conservation_residual_ps'],
            'native_hbf_exact':behavior['gpu_native_exact'],'paired_nodes':bs['nodes_paired']})
        print('S5-a',count,'pass actual',len(summary['tokens']),flush=True)
    write(folder/'stage-result.json',{'stage':'S5','status':'blocked','S5-a':'pass','S5-b':'not_executed',
        'blocking_items':['S5-b fixed 1000ps segment advancement and independent timing audit are not yet implemented.'],
        'cases':results,'S6_started':False,'formal_performance_measured':False})
    write(study/'artifact-index.json',{'files':[{'path':x.relative_to(study).as_posix(),'bytes':x.stat().st_size,'sha256':sha(x)} for x in sorted(study.rglob('*')) if x.is_file() and x.name!='artifact-index.json']})
    print('S5 remains blocked pending S5-b',flush=True)

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--s4',type=Path,required=True);ap.add_argument('--s2',type=Path,required=True);ap.add_argument('--study',type=Path,required=True)
    a=ap.parse_args();run(a.s4.resolve(),a.s2.resolve(),a.study.resolve())
