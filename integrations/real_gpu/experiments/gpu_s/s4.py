"""S4: two selected regions and nonzero-offset regression, zero compute."""
import argparse,copy
from pathlib import Path
from common import read,write,sha,RegionPlan
from study import ROOT,BASE,invoke
from audit import numerical,stored_kv
from s4_audit import positive
from platform_environment import executable
from integrated_build_identity import verify_build

def run(prior,s2,study):
    assert read(prior/'S3/stage-result.json')['status']=='pass' and read(s2/'S2/stage-result.json')['status']=='pass'
    assert not study.exists();folder=study/'S4';folder.mkdir(parents=True)
    p=read(s2/'S0/profile.json');target=read(s2/'S0/numerical-targets-predeclared.json');native=prior/'S3/native-1';cpu=prior/'S3/cpu-main-1'
    assert sha(p['model_path'])==p['gguf_sha256'];verify_build(BASE)
    binary=executable(ROOT/'build-s3/bin','gpu_s_joint');h0=folder/'effective-H0.ini';h0.write_bytes((s2/'S0/backend-preflight/effective-H0.ini').read_bytes())
    prompt=folder/'prompt.txt';prompt.write_bytes((s2/'S0/inputs/main.txt').read_bytes())
    def region(name,tensor,offset,size,base):
        source=next(x for x in p['tensors'] if x['name']==tensor)
        return {'region_id':name,'tensor':tensor,'storage_id':tensor,'source_offset':offset,'source_bytes':source['bytes'],
            'valid_bytes':size,'registered_bytes':(size+4095)//4096*4096,'global_base':base,'content_id':1,'canonical_region':name,'aliases':[]}
    def manifest(offset):
        x=[region('norm',p['norm_name'],0,p['hidden']*4,0),region('down',p['down_name'],offset,p['ffn']*2-offset,4096)]
        RegionPlan(x,8388608);return x
    write(folder/'conditions.json',{'s3_result_sha256':sha(prior/'S3/stage-result.json'),'s2_result_sha256':sha(s2/'S2/stage-result.json'),
        'target_sha256':sha(s2/'S0/numerical-targets-predeclared.json'),'binary_sha256':sha(binary),'source_hashes':{x.name:sha(x) for x in Path(__file__).parent.iterdir() if x.is_file()},
        'prompt_sha256':sha(prompt),'h0_sha256':sha(h0),'compute_ps':0,'outputs':1,'cases':['M2','M2-offset']})
    results=[]
    for name,offset in [('M2',0),('M2-offset',64)]:
        regions=manifest(offset);write(folder/(name+'-regions.json'),regions);cfg=copy.deepcopy(p)
        cfg['hbf']={'enabled':True,'effective_config_path':str(h0),'regions':regions};cfgfile=folder/(name+'-config.json');write(cfgfile,cfg)
        out=folder/name;invoke([binary,p['model_path'],prompt,out,1,'verify',cfgfile],study/'logs'/name)
        behavioral=positive(out,native,regions,p);assert stored_kv(out,p)['status']=='pass'
        comparison=numerical(out,cpu,p,target)
        if comparison['status']!='pass':raise RuntimeError('S4 CPU numerical validation failed: '+name)
        results.append({'case':name,'status':'pass','coverage':behavioral['coverage'],'wait_ps':behavioral['wait_ps'],
            'metrics':behavioral['metrics'],'numerical_arrays':comparison['arrays_compared'],'tokens':read(out/'summary.json')['tokens'],
            'gpu_native_exact':behavioral['gpu_native_exact'],'conservation_residual_ps':behavioral['conservation_residual_ps']})
        print(name,'pass wait_ps',behavioral['wait_ps'],flush=True)
    write(folder/'stage-result.json',{'stage':'S4','status':'pass','cases':results,'S5_started':False,'formal_performance_measured':False})
    (folder/'report.md').write_text('# S4\n\nM2 and nonzero-offset cases passed numerical, event identity, coverage, dependency and drain audits. Times are zero-compute functional results.\n',encoding='utf-8')
    records=[{'path':x.relative_to(study).as_posix(),'bytes':x.stat().st_size,'sha256':sha(x)} for x in sorted(study.rglob('*')) if x.is_file() and x.name!='artifact-index.json']
    write(study/'artifact-index.json',{'files':records,'file_count':len(records)})

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--s3',type=Path,required=True);ap.add_argument('--s2',type=Path,required=True);ap.add_argument('--study',type=Path,required=True)
    a=ap.parse_args();run(a.s3.resolve(),a.s2.resolve(),a.study.resolve())
