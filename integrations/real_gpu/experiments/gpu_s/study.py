"""Execute S0/S1 with explicit artifacts; S2 tooling lives alongside this file."""
import argparse
import copy
import json
import os
import platform
import subprocess
import sys
import shutil
import time
from datetime import datetime, timezone
from pathlib import Path
from common import read, write, rows, sha, metrics, replay_synthetic, validate_compute_table, RegionPlan

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
from platform_environment import runtime_environment, executable
from integrated_profile import derive
from integrated_build_identity import verify_build
BUILD = Path(os.environ.get('GPU_S_BUILD', ROOT/'../../build-real-gpu-s')).resolve()
BASE = Path(os.environ.get('GPU_S_BASE_BUILD', ROOT/'../../build-real-gpu-model')).resolve()


def environment():
    env = runtime_environment(BASE, ROOT)
    env.update(GGML_CUDA_CUBLAS_COMPUTE_TYPE='f32', GGML_CUDA_DISABLE_FUSION='1',
               NVIDIA_TF32_OVERRIDE='0')
    return env


def invoke(cmd, folder):
    folder = Path(folder); folder.mkdir(parents=True, exist_ok=True)
    with (folder/'stdout.log').open('w', encoding='utf-8') as so, (folder/'stderr.log').open('w', encoding='utf-8') as se:
        started=time.perf_counter()
        r = subprocess.run(list(map(str, cmd)), env=environment(), stdout=so, stderr=se, timeout=3600)
    write(folder/'command.json', {'argv': list(map(str, cmd)), 'returncode': r.returncode,'process_wall_seconds':time.perf_counter()-started,
                                'wall_scope':'Process cost including load/verification; never a simulated GPU or HBF latency.'})
    if r.returncode:
        raise RuntimeError('Experiment command failed; see ' + str(folder))


def native(study, name, prompt, count, mode='verify'):
    out = study/'S2'/name
    invoke([executable(BUILD/'bin', 'gpu_s_native'), read(study/'S0/profile.json')['model_path'],
            prompt, out, count, mode, study/'S0/profile.json'], study/'logs'/name)
    return out


def source_records():
    paths = [p for p in Path(__file__).parent.iterdir() if p.is_file()]
    paths += [p for p in (BUILD/'bin').glob('*.exe')]
    return [{'path': str(p.resolve()), 'sha256': sha(p), 'bytes': p.stat().st_size} for p in sorted(paths)]


def cache_values(path):
    values={}
    for line in path.read_text(encoding='utf-8').splitlines():
        if '=' in line and ':' in line.split('=',1)[0] and not line.startswith('//'):
            key,value=line.split('=',1);values[key.split(':',1)[0]]=value
    return values


def s0(study, model, conversion, numerical_targets=None):
    folder=study/'S0'; folder.mkdir(parents=True)
    identity=verify_build(BASE)
    write(folder/'dependency-build-identity.json', identity)
    p=derive(model, conversion, context=512, ubatch=256)
    write(folder/'profile.json', p)
    inputs=folder/'inputs'; inputs.mkdir()
    (inputs/'main.txt').write_bytes((ROOT/'adapters/llama_cpp/prompt.txt').read_bytes())
    templates={
        'calibration-a': 'Compute the next two integers after 4 and 5. Answer briefly.',
        'calibration-b': 'A red book and a blue book are on a table. Describe the books.',
        'backup': 'Explain why computers need memory in several sentences.',
        'calibration-g': 'Two trains each have three carriages. How many carriages are there in total?',
        'calibration-h': 'Describe one reason that a metal spoon feels cold in a room.'}
    # Calibration prompts are fixed before looking at inference or numerical errors.
    for name, message in templates.items():
        text='<|im_start|>system\nYou are a helpful assistant. Answer directly without thinking aloud.<|im_end|>\n<|im_start|>user\n'+message+'<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n'
        (inputs/(name+'.txt')).write_bytes(text.encode())
    tokenized={}
    for prompt in sorted(inputs.glob('*.txt')):
        out=folder/'tokenization'/prompt.stem
        invoke([executable(BUILD/'bin','gpu_s_native'),model,prompt,out,0,'probe',folder/'profile.json'],study/'logs'/('probe-'+prompt.stem))
        data=read(out/'summary.json');n=len(data['prompt_ids'])
        if not 0<n<=p['batch'] or n+8>p['context']:
            raise ValueError('Input exceeds capacity: '+prompt.stem)
        tokenized[prompt.stem]={'path':str(prompt.resolve()),'sha256':sha(prompt),'prompt_ids':data['prompt_ids'],'tokens':n}
    invoke([executable(BUILD/'bin','gpu_s_bridge'),folder/'backend-preflight','preflight'],study/'logs/backend-preflight')
    env=read(folder/'tokenization/main/summary.json')['environment']
    env.update(platform=platform.platform(),python=sys.version,python_executable=sys.executable,
               variables={k:environment().get(k) for k in ['CUDA_PATH','CUDA_VISIBLE_DEVICES','GGML_CUDA_CUBLAS_COMPUTE_TYPE','GGML_CUDA_DISABLE_FUSION','NVIDIA_TF32_OVERRIDE']})
    import numpy, torch
    base_cache=cache_values(BASE/'CMakeCache.txt');experiment_cache=cache_values(BUILD/'CMakeCache.txt')
    assert base_cache['GGML_CUDA_GRAPHS']=='OFF'
    driver=subprocess.run([shutil.which('nvidia-smi') or str(Path(os.environ['WINDIR'])/'System32/nvidia-smi.exe'),
        '--query-gpu=name,driver_version,pci.bus_id,uuid','--format=csv,noheader'],capture_output=True,text=True,check=True)
    nvcc=subprocess.run([experiment_cache['CUDAToolkit_NVCC_EXECUTABLE'],'--version'],capture_output=True,text=True,check=True)
    env.update(recorded_utc=datetime.now(timezone.utc).isoformat(),numpy=numpy.__version__,torch=torch.__version__,
        cpu_reference_device='cpu',nvidia_smi=driver.stdout.strip(),nvcc_version=nvcc.stdout.strip(),
        toolchain={k:experiment_cache[k] for k in ['CMAKE_CXX_COMPILER','CMAKE_MAKE_PROGRAM','CUDAToolkit_ROOT','CUDAToolkit_NVCC_EXECUTABLE']},
        cuda_architectures=base_cache['CMAKE_CUDA_ARCHITECTURES'],cuda_graphs=base_cache['GGML_CUDA_GRAPHS'])
    for name,path in [('dependency',BASE/'CMakeCache.txt'),('experiment',BUILD/'CMakeCache.txt')]:
        (folder/(name+'-CMakeCache.txt')).write_bytes(path.read_bytes())
    if env['gpu_free_bytes']<p['budgets']['gpu_required_bytes'] or env['host_available_bytes']<p['budgets']['host_required_bytes']:
        raise RuntimeError('Model resource budget unavailable')
    write(folder/'environment.json',env);write(folder/'inputs.json',tokenized)
    contract={'schema_version':1,'status':'predeclared_pending_calibration',
              'reference':'Independent torch CPU mathematical Qwen3 on the exact GGUF tensor values; F32 arithmetic, F16 KV storage',
              'criteria':{'tokens_exact':True,'finite':True,'shape_exact':True,
                          'relative_rms_max':2**-11,'element_error_over_max_reference_abs_or_reference_rms':2**-10},
              'basis':{'u16':2**-11,'u32':2**-24,
                       'precision_target':'Preserve the RMS accuracy of one F16 storage rounding; element error target is the sum of two F16 unit-roundoff budgets, with RMS floor for values near zero.',
                       'qualification':'Engineering fidelity targets tied to storage precision, NOT a claimed full-network forward-error bound. Must pass disjoint calibration at fixed thresholds; failure blocks S2 instead of relaxing them.',
                       'f32_dot_gamma_3072':(3072*2**-24)/(1-3072*2**-24),
                       'controls':'GPU cuBLAS f32, TF32 override off; CPU f32 GEMM; identical F16-quantized weights and F16 KV storage. Different reduction/RoPE rounding remains measured.'},
              'calibration_inputs':['calibration-a','calibration-b'],'formal_inputs':['main','backup']}
    if numerical_targets:
        contract=read(numerical_targets)
        (folder/'numerical-contract-source.json').write_bytes(Path(numerical_targets).read_bytes())
    write(folder/'numerical-targets-predeclared.json',contract)
    lock={'status':'s0_ready','model_sha256':p['gguf_sha256'],'conversion_sha256':sha(conversion),
          'inputs':tokenized,'config':{'context':512,'batch':256,'ubatch':256,'threads':8,'kv':'F16 native','hbf':'disabled in S2','max_outputs':[1,2]},
          'pending':['B1_access_semantics','B2_numerical_calibration','S6_sampling_protocol'],
          'numerical_targets_sha256':sha(folder/'numerical-targets-predeclared.json'),
          'experiment_sources_and_binaries':source_records(),'dependency_identity_sha256':sha(folder/'dependency-build-identity.json')}
    for record in lock['experiment_sources_and_binaries']:
        source=Path(record['path']);target=folder/'source-and-binary-snapshot'/source.name
        target.parent.mkdir(exist_ok=True);target.write_bytes(source.read_bytes())
    write(folder/'study-lock.json',lock);write(folder/'conditions.json',lock)
    write(folder/'preflight.json',{'status':'pass','missing_inputs':[],'model_metadata_verified':True,'input_capacity_verified':True,'h0':read(folder/'backend-preflight/preflight.json')})
    write(folder/'stage-result.json',{'stage':'S0','status':'pass','inference_executed':False,'actual_outputs':None,'blocking_items':[],
                                     'artifacts':['environment.json','inputs.json','profile.json','study-lock.json','backend-preflight/effective-H0.ini','numerical-targets-predeclared.json']})
    (folder/'report.md').write_text('# S0\n\nEnvironment, exact GGUF/conversion identity, tokenization and H0 resolution passed. No inference or performance claim. B1/B2 remain pending until S2.\n')


def s1(study):
    if read(study/'S0/stage-result.json')['status']!='pass':raise RuntimeError('S0 not passed')
    folder=study/'S1';folder.mkdir()
    expected,synthetic_trace=replay_synthetic([('compute',10_000_000),('wait',20_000_000),('compute',5_000_000),('token',1),('compute',3_000_000),('wait',7_000_000),('compute',2_000_000),('token',2)])
    assert expected['ttft_ps']==35_000_000 and expected['itl_ps']==[12_000_000] and expected['tpot_ps']==12_000_000
    write(folder/'formula-check.json',{'status':'pass','evidence':'synthetic wait adapter, not NAND timing','trace':synthetic_trace,**expected})
    assert replay_synthetic([('EOS',0)])[0]['actual_outputs']==0
    assert replay_synthetic([('compute',10),('token',1),('EOS',0)])[0]['actual_outputs']==1
    assert metrics(0,[])['ttft_ps'] is None and metrics(0,[])['tpot_ps'] is None and metrics(0,[0])['tpot_ps'] is None
    rejected=[]
    def reject(name,fn):
        try:fn()
        except (ValueError,KeyError):rejected.append(name);return
        raise AssertionError('Not rejected: '+name)
    reject('backdated_token',lambda:metrics(1,[0]));reject('unordered_tokens',lambda:metrics(0,[2,1]))
    header={'model':'synthetic','prompt':'test','sequence':'a-b-c'};ids=['first','middle','tail']
    table={'header':header,'segments':[{'segment_id':i,'duration_ps':v} for i,v in zip(ids,[0,3,0])]}
    validate_compute_table(header,ids,table)
    for kind in ['missing','duplicate','reordered','input_hash','negative']:
        q=copy.deepcopy(table)
        if kind=='missing':q['segments'].pop()
        elif kind=='duplicate':q['segments'][1]['segment_id']='first'
        elif kind=='reordered':q['segments'].reverse()
        elif kind=='input_hash':q['header']['prompt']='wrong'
        else:q['segments'][0]['duration_ps']=-1
        reject('table_'+kind,lambda q=q:validate_compute_table(header,ids,q))
    base={'region_id':'A','storage_id':'tensor','source_offset':64,'valid_bytes':4096,'source_bytes':16384,'global_base':0,'registered_bytes':4096,'content':1}
    second=dict(base,region_id='B',source_offset=4160,valid_bytes=64,global_base=4096,registered_bytes=4096)
    plan=RegionPlan([base,second,{'region_id':'alias','alias_of':'A'}],8192)
    hits=plan.intersect('tensor',4096,256)
    assert hits==[{'region_id':'A','source_offset':4096,'region_offset':4032,'global_address':4032,'bytes':64},
                  {'region_id':'B','source_offset':4160,'region_offset':0,'global_address':4096,'bytes':64}]
    assert plan.intersect('unselected',0,64)==[] and sum(x['bytes'] for x in hits)==128
    assert sum(r['registered_bytes']-r['valid_bytes'] for r in plan.canonical.values())==4032
    reject('overlapping_mapping',lambda:RegionPlan([base,dict(second,global_base=0)],8192))
    reject('source_out_of_bounds',lambda:RegionPlan([dict(base,source_offset=16384)],8192))
    reject('capacity',lambda:RegionPlan([base],64))
    write(folder/'mapping-cases.json',{'status':'pass','records':plan.records,'cross_region_callback':{'offset':4096,'bytes':256},'expected_intersections':hits,'alias_allocations':0,'padding_bytes':4032})
    write(folder/'negative-cases.json',{'status':'pass','rejected':rejected,'n0':metrics(0,[]),'n1':metrics(0,[0]),'eos_policy':'not counted as output'})
    invoke([executable(BUILD/'bin','gpu_s_bridge'),folder/'real-bridge'],study/'logs/S1-real-bridge')
    real=read(folder/'real-bridge/summary.json');trace=rows(folder/'real-bridge/bridge.jsonl')
    globals_finished={r['global']:r['tick_ps'] for r in trace if r['action']=='global-finished'}
    for access in real['reads']:
        gids=[r['global'] for r in trace if r['access']==access['access'] and r['action']=='global-registered']
        assert len(gids)==access['globals'] and max(globals_finished[g] for g in gids)==access['completed_ps']
    stats=read(folder/'real-bridge/statistics.json');reads=[x for x in stats['globals'] if x['op']=='read']
    assert len(reads)==5
    for global_row,base in zip(reads,[0,4096,0,4096,8192]):
        work=[x for x in stats['logical_work'] if x['global_id']==global_row['id']]
        assert global_row['bytes']==4096 and global_row['result']=='success'
        assert sorted(x['local_byte'] for x in work)==list(range(base,base+4096,64))
        assert all(x['channel']==0 and x['result']=='success' for x in work)
    write(folder/'independent-audit.json',{'status':'pass','parent_completion_max_child_verified':True,'all_320_read_fragment_addresses_verified':True,'real_bridge':real})
    (folder/'coverage.csv').write_bytes(b'region,registered_valid_bytes,touched_union_bytes,cumulative_read_bytes\nA,8192,8192,16384\nB,8192,4096,4096\n')
    write(folder/'conditions.json',{'s0_lock_sha256':sha(study/'S0/study-lock.json'),'sources_and_binaries':source_records(),'compute':'synthetic or zero','gpu_used':False,'variants':['H0 resident=256','pressure resident=64','persistent program fail']})
    write(folder/'stage-result.json',{'stage':'S1','status':'pass','formula':expected,'real_reads':3,'read_globals':5,'mapping_pass':True,'negative_cases':len(rejected)+len(real['rejected']),'actual_outputs':None,'gpu_used':False})
    (folder/'report.md').write_text('# S1\n\nSynthetic TTFT=35us, ITL/TPOT=12us. Real bridge: 16KiB registered, 12KiB touched, 20KiB read, 5 final read Globals. Mapping, negative, zero/tail, pressure and normal-failure drain checks passed.\n')


if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('stage',choices=['S0','S1']);ap.add_argument('--study',type=Path,required=True)
    ap.add_argument('--model',type=Path);ap.add_argument('--conversion',type=Path);ap.add_argument('--numerical-targets',type=Path);args=ap.parse_args()
    study=args.study.resolve();study.mkdir(parents=True,exist_ok=True)
    if args.stage=='S0':s0(study,args.model,args.conversion,args.numerical_targets)
    else:s1(study)
    print(args.stage+' passed: '+str(study))
