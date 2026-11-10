"""Predeclare engineering criteria, then test unseen calibration and main input.

This does not infer a mathematical forward-error bound from F16 roundoff.
"""
import argparse
import json
from pathlib import Path
from unittest.mock import patch
import numpy as np
from common import read,write,sha
from batches import batch_path
from study import ROOT,BASE,invoke, JOINT_BUILD
from reference import CpuQwen
from audit import numerical,boundaries,stored_kv
from integrated_build_identity import verify_build
from platform_environment import executable

def run(out):
    out.mkdir(parents=True,exist_ok=False)
    prior=batch_path('s2-original');p=read(prior/'S0/profile.json');lock=read(prior/'S0/study-lock.json')
    assert sha(p['model_path'])==p['gguf_sha256'] and sha(p['conversion'])==lock['conversion_sha256']
    binary=executable(JOINT_BUILD/'bin','gpu_s_joint');verify_build(BASE)
    prompts={'calibration-e':'A box contains four pencils and two pens. How many objects are in the box?',
             'calibration-f':'Give a short explanation of why a shadow appears when light is blocked.'}
    for name,message in prompts.items():
        text='<|im_start|>system\nYou are a helpful assistant. Answer directly without thinking aloud.<|im_end|>\n<|im_start|>user\n'+message+'<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n'
        (out/(name+'.txt')).write_bytes(text.encode())
    (out/'main.txt').write_bytes((prior/'S0/inputs/main.txt').read_bytes());write(out/'profile.json',p)
    # Engineering budgets, not a proved full-network bound. Selected after prior
    # diagnostics were seen, before any of the new calibration runs.
    criteria={'logits':{'relative_rms_max':2**-9,'normalized_max':2**-7},
              'hidden_qkv':{'relative_rms_max':2**-8,'normalized_max':2**-6},
              'kv_storage':{'relative_rms_max':2**-8,'normalized_max':2**-5}}
    write(out/'candidate-predeclared.json',{'status':'candidate_not_qualified','criteria':criteria,
        'definitions':{'relative_rms':'RMS(GPU-CPU)/RMS(CPU)','normalized_max':'max(abs(GPU-CPU)/max(abs(CPU),RMS(CPU)))',
          'zero_rms':'exact equality','classification':'complete_logits=logits; Kstore/Vstore=kv_storage; other snapshots=hidden_qkv'},
        'basis':'Engineering fidelity budgets in powers of F16 unit roundoff u=2^-11, not full-network forward-error bounds: logits RMS 4u/max 16u, hidden RMS 8u/max 32u, KV RMS 8u/max 64u. The budgets express provisional numerical fidelity goals, with greater local tolerance for quantized KV and accumulated intermediates; they are not guaranteed by F16 arithmetic.',
        'selection_disclosure':'Prior S2/S3/ablation errors were already known when these budgets were selected; new calibration e/f were not run or inspected before registration. The proposed budgets are judgment choices, not independently established error bounds.',
        'hard_conditions':'tokens/raw/EOS/stop/positions exact; finite; shape exact; all recorded arrays pass their category; no CUDA drops; valid GPU KV publication exact.',
        'not_proven':'GPU libm bounds; complete per-operation CPU/GPU correctness; valid for unseen models or different execution settings',
        'input_hashes':{name:sha(out/(name+'.txt')) for name in [*prompts,'main']},
        'model_sha256':p['gguf_sha256'],'binary_sha256':sha(binary),'reference_sha256':sha(Path(__file__).with_name('reference.py')),
        'script_sha256':sha(Path(__file__)), 'old_targets_sha256':sha(prior/'S0/numerical-targets-predeclared.json')})
    # Unit-scale negative controls show which large errors these budgets detect;
    # they do not prove the tests detect every implementation error.
    controls=[]
    for category,limits in criteria.items():
        expected=np.ones(128);bad=expected.copy();bad[0]+=limits['normalized_max']*2
        e=audit_error(bad,expected)
        assert e['normalized_max']>limits['normalized_max']
        controls.append({'category':category,'single_element_perturbation':limits['normalized_max']*2,'rejected':True})
    write(out/'sensitivity-controls.json',controls)
    results=[]
    for name in [*prompts,'main']:
        case=out/name;case.mkdir();gpu=case/'gpu';cpu=case/'cpu'
        invoke([binary,p['model_path'],out/(name+'.txt'),gpu,2,'verify',out/'profile.json'],case/'logs')
        c=CpuQwen(p);c.run(read(gpu/'summary.json')['prompt_ids'],2,cpu);del c
        target=read(prior/'S0/numerical-targets-predeclared.json')
        with patch('audit.write',lambda path,value:write(case/'old-target-comparison.json',value)):
            old=numerical(gpu,cpu,p,target)
        boundary=boundaries(gpu,p);kv=stored_kv(gpu,p)
        checks=[]
        for x in old['checks']:
            category='logits' if x['name']=='complete_logits' else ('kv_storage' if 'store-' in x['name'] else 'hidden_qkv')
            limit=criteria[category];r=x.get('relative_rms',float('inf'));m=x.get('normalized_max',float('inf'))
            checks.append({'name':x['name'],'step':x.get('step'),'category':category,'relative_rms':r,'normalized_max':m,
                           'pass':r<=limit['relative_rms_max'] and m<=limit['normalized_max']})
        okay=old['token_boundary_exact'] and len(checks)==old['expected_arrays'] and all(x['pass'] for x in checks) and kv['status']=='pass' and boundary['status']=='pass'
        summary={'case':name,'status':'pass' if okay else 'fail','tokens':read(gpu/'summary.json')['tokens'],
            'positions':read(gpu/'summary.json')['positions'],'token_boundary_exact':old['token_boundary_exact'],
            'arrays':len(checks),'failed_arrays':sum(not x['pass'] for x in checks),
            'by_category':{k:{'arrays':sum(x['category']==k for x in checks),'failed':sum(x['category']==k and not x['pass'] for x in checks),
                 'max_relative_rms':max(x['relative_rms'] for x in checks if x['category']==k),
                 'max_normalized':max(x['normalized_max'] for x in checks if x['category']==k)} for k in criteria},
            'GPU_KV_publication':kv['status'],'boundary':boundary['status'],'old_threshold_status':old['status']}
        write(case/'candidate-comparison.json',{'summary':summary,'checks':checks});results.append(summary)
        print(name,summary['status'],summary['failed_arrays'],summary['tokens'],flush=True)
    write(out/'stage-result.json',{'status':'candidate_validation_only','B2_qualified':False,
        'calibration_status':'pass' if all(r['status']=='pass' for r in results[:2]) else 'fail',
        'main_status':results[2]['status'],'cases':results,'reason':'Engineering thresholds and limited implementation review are provisional; local operation correctness and error bounds not independently established.'})

def audit_error(a,b):
    d=a-b;s=float(np.sqrt(np.mean(b*b)))
    return {'normalized_max':float(np.max(np.abs(d)/np.maximum(np.abs(b),s)))}

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True);run(ap.parse_args().out.resolve())
