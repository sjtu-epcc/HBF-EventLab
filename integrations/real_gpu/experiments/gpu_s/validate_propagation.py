"""Register fresh inputs before execution and test usefulness as well as coverage."""
import argparse
from pathlib import Path
from unittest.mock import patch
import numpy as np
from common import read,write,rows,sha
from study import ROOT,BASE,invoke
from platform_environment import executable
from integrated_build_identity import verify_build
from reference import CpuQwen
from propagation import Propagation,U
from s3_audit import snapshot_value
import audit

def run(out):
    out.mkdir(parents=True,exist_ok=False)
    prior=ROOT/'output/gpu-s/20261031-s0-s2-002';p=read(prior/'S0/profile.json')
    target=read(prior/'S0/numerical-targets-predeclared.json');write(out/'profile.json',p)
    prompts={'holdout-c':'A gardener has three pots and plants two seeds in each pot. State the total number of seeds.',
             'holdout-d':'Name one difference between a triangle and a square in a short sentence.'}
    for name,message in prompts.items():
        text='<|im_start|>system\nYou are a helpful assistant. Answer directly without thinking aloud.<|im_end|>\n<|im_start|>user\n'+message+'<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n'
        (out/(name+'.txt')).write_bytes(text.encode())
    (out/'main.txt').write_bytes((prior/'S0/inputs/main.txt').read_bytes())
    names=['holdout-c','holdout-d','main'];binary=executable(ROOT/'build-s3/bin','gpu_s_joint')
    write(out/'registration.json',{'inputs':{n:sha(out/(n+'.txt')) for n in names},'outputs':2,
        'model_sha256':p['gguf_sha256'],'binary_sha256':sha(binary),'reference_sha256':sha(Path(__file__).with_name('reference.py')),
        'propagation_source_sha256':sha(Path(__file__).with_name('propagation.py')),
        'assumptions':{'u32':U,'dot_gamma':'gamma_(2n) times sum absolute products plus propagated input radii',
            'quantization':'u16*(abs(center)+radius)+2^-25, plus center recentering; possible overflow explicitly invalidates certification',
            'softmax_lipschitz_infinity':0.5,'silu_lipschitz':2,
            'nonlinear_local_allowance_units_u32':64,'nonlinear_allowance_verified':False,
            'outward_rounded_interval_arithmetic':False},
        'acceptance':'Coverage alone never qualifies. Need independently justified local bounds, no possible overflow, finite radii, and pairwise radii certify the original per-array precision goals.',
        'criteria':target['criteria'],'observed_GPU_errors_used_to_construct_radii':False})
    write(out/'dependency-identity.json',verify_build(BASE))
    for f in ['propagation.py','validate_propagation.py','reference.py']:
        (out/f).write_bytes(Path(__file__).with_name(f).read_bytes())
    results=[]
    for name in names:
        case=out/name;case.mkdir();gpu=case/'gpu';cpu=case/'cpu';bounds=case/'propagation'
        invoke([binary,p['model_path'],out/(name+'.txt'),gpu,2,'verify',out/'profile.json'],case/'log')
        ids=read(gpu/'summary.json')['prompt_ids'];reference=CpuQwen(p);reference.run(ids,2,cpu);del reference
        model=Propagation(p);model.execute(ids,read(cpu/'summary.json')['raw_tokens'],bounds);del model
        with patch.object(audit,'write',lambda path,value:write(case/'independent-comparison.json',value)):
            comparison=audit.numerical(gpu,cpu,p,target)
        bs=np.load(bounds/'bounds.npz');checks=[]
        metas=rows(gpu/'snapshots.jsonl')
        for check in comparison['checks']:
            name0=check['name'];step=check.get('step')
            if name0=='complete_logits':continue
            m=next(m for m in metas if m['node_id']==check['node_id'])
            value=snapshot_value(gpu,m,p).reshape(-1).astype('float64')
            center=bs[f'{step}:{name0}:center'];radius=bs[f'{step}:{name0}:radius']
            if name0==f"l_out-{p['layers']-1}" and value.size==p['hidden']:center=center[-1:];radius=radius[-1:]
            center=center.reshape(-1);radius=radius.reshape(-1)
            assert len(center)==len(value)
            scale=check['reference_rms'];rms_radius=float(np.sqrt(np.mean(radius*radius)))
            checks.append({'step':step,'name':name0,'GPU_covered':bool(np.all(np.abs(value-center)<=radius)),
                'pairwise_rms_bound_over_reference_rms':2*rms_radius/scale if scale else None,
                'certifies_original_RMS_target':bool(scale>0 and 2*rms_radius/scale<=target['criteria']['relative_rms_max'])})
        actual=np.fromfile(gpu/'logits.f32',dtype='<f4').reshape(-1,read(gpu/'summary.json')['vocab_size'])
        cpu_logits=np.fromfile(cpu/'logits.f32',dtype='<f4').reshape(actual.shape)
        for step,values in enumerate(actual):
            c=bs[f'{step}:logits:center'].reshape(-1);r=bs[f'{step}:logits:radius'].reshape(-1)
            scale=float(np.sqrt(np.mean(cpu_logits[step].astype('float64')**2)))
            pairwise=2*float(np.sqrt(np.mean(r*r)))/scale
            checks.append({'step':step,'name':'logits','GPU_covered':bool(np.all(np.abs(values-c)<=r)),
                'max_radius':float(r.max()),'pairwise_rms_bound_over_reference_rms':pairwise,
                'certifies_original_RMS_target':pairwise<=target['criteria']['relative_rms_max']})
        write(case/'bound-checks.json',checks)
        result={'case':name,'actual_outputs':len(read(gpu/'summary.json')['tokens']),
            'token_boundary_exact':comparison['token_boundary_exact'],'original_criteria':comparison['status'],
            'checked_arrays':len(checks),'covered_arrays':sum(x['GPU_covered'] for x in checks),
            'RMS_certified_arrays':sum(x['certifies_original_RMS_target'] for x in checks),
            'first_RMS_uncertified':next((x for x in checks if not x['certifies_original_RMS_target']),None),
            **read(bounds/'model-result.json')}
        results.append(result);print(name,'covered',result['covered_arrays'],'/',len(checks),'RMS certified',result['RMS_certified_arrays'],flush=True)
    write(out/'result.json',{'status':'inconclusive','B2_qualified':False,'cases':results,
        'reason':'Conditional local nonlinear assumptions remain unverified; broad enclosures do not certify original precision targets.'})

if __name__=='__main__':
    a=argparse.ArgumentParser();a.add_argument('--out',type=Path,required=True);run(a.parse_args().out.resolve())
