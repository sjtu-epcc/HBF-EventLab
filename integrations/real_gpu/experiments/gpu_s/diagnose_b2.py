"""Causal KV intervention, strictly diagnostic and never an independent CPU oracle."""
import argparse
from pathlib import Path
from unittest.mock import patch
import numpy as np
import torch
from common import read,rows,write,sha
from s3_audit import snapshot_value
import audit


def run(prior,out):
    out.mkdir(parents=True,exist_ok=False)
    profile=read(prior/'S0/profile.json');target=read(prior/'S0/numerical-targets-predeclared.json')
    source=Path(__file__).with_name('reference.py');original=source.read_text(encoding='utf-8')
    marker="                save(step,f'Kstore-{layer}',cache_k[layer]);save(step,f'Vstore-{layer}',cache_v[layer])"
    assert original.count(marker)==1
    intervention="""                # Diagnostic intervention: replace published cache with observed GPU cache.
                cache_k[layer]=torch.from_numpy(injected[(step,'K',layer)].copy())
                cache_v[layer]=torch.from_numpy(injected[(step,'V',layer)].copy())
"""+marker
    modified=original.replace(marker,intervention)
    (out/'intervened-reference.py.txt').write_text(modified,encoding='utf-8')
    results=[]
    for name in ['calibration-a','calibration-b']:
        gpu=prior/'S2'/(name+'-2');injected={}
        for meta in rows(gpu/'snapshots.jsonl'):
            if 'kv_kind' in meta:
                injected[(meta['step'],meta['kv_kind'],meta['layer'])]=snapshot_value(gpu,meta,profile).reshape(-1,profile['kv_heads'],profile['head_dim'])
        namespace={'__file__':str(source),'__name__':'b2_intervention','injected':injected}
        exec(compile(modified,str(out/'intervened-reference.py.txt'),'exec'),namespace)
        cpu=out/(name+'-gpu-kv-intervention')
        namespace['CpuQwen'](profile).run(read(gpu/'summary.json')['prompt_ids'],2,cpu)
        # Explicitly overwrite the diagnostic label emitted by the reused runner.
        summary=read(cpu/'summary.json');summary.update(implementation='Hybrid CPU calculation with observed GPU KV cache intervention',independent_reference=False)
        write(cpu/'summary.json',summary)
        with patch.object(audit,'write',lambda path,value:write(cpu/'comparison.json',value)):
            comparison=audit.numerical(gpu,cpu,profile,target)
        baseline=read(gpu/'numerical.json')
        def remaining(r):
            checks=[x for x in r['checks'] if 'store-' not in x['name']]
            return {'arrays':len(checks),'failed_arrays':sum(not x['pass'] for x in checks),
                'max_relative_rms':max(x['relative_rms'] for x in checks),
                'max_normalized_error':max(x['normalized_max'] for x in checks),
                'complete_logits':next(x for x in checks if x['name']=='complete_logits')}
        result={'case':name,'independent_baseline':remaining(baseline),'gpu_kv_intervention':remaining(comparison),
            'injected_KV_arrays_excluded_from_conclusion':True,'intervention_is_independent_reference':False}
        results.append(result);print(result,flush=True)
    write(out/'diagnosis.json',{'status':'diagnostic_completed','reference_source_sha256':sha(source),
        'criteria_sha256':sha(prior/'S0/numerical-targets-predeclared.json'),'criteria_changed':False,
        'B2_qualified':False,'reason':'A GPU-fed intervention cannot qualify the independent CPU contract.', 'cases':results})


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--prior',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();run(a.prior.resolve(),a.out.resolve())
