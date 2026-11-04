"""Supplemental S5 acceptance: operation identity and adversarial audit fixtures."""
import argparse
import copy
import csv
import tempfile
from pathlib import Path
from unittest.mock import patch
from common import rows, read, write, sha
import segment_audit


def operations(folder):
    activities={a['activity_id']:dict(a,kind='kernel') for a in rows(folder/'kernels.jsonl')}
    activities.update({a['activity_id']:a for a in rows(folder/'transfers.jsonl')})
    apis={a['correlation']:a for a in rows(folder/'apis.jsonl')}
    result=[]
    for w in rows(folder/'segments.jsonl'):
        if w['kind']=='verification':continue
        members=[activities[i] for i in range(w['first_activity_id'],w['first_activity_id']+w['activity_count'])]
        members.sort(key=lambda a:(apis[a['correlation']]['start_ns'],apis[a['correlation']]['end_ns']))
        result.append((w['step'],w['kind'],w['node_id'],[
            (a['kind'],a.get('name'),a.get('bytes'),a.get('copy_kind')) for a in members]))
    return result


def run(study):
    folder=study/'S5';stage=read(folder/'stage-result.json');assert stage['status']=='pass'
    identities=[]
    for count in [2,8]:
        baseline=operations(folder/f'native-{count}')
        for name in [f'native-timing-{count}',f'M2-zero-{count}',f'M2-fixed-{count}']:
            assert operations(folder/name)==baseline, name+' changed per-segment CUDA operation identity/order'
            identities.append({'case':name,'per_segment_enqueued_operations_exact':True})
    source=folder/'M2-fixed-2'
    data={p.name:rows(p) for p in source.glob('*.jsonl') if p.name in ['segments.jsonl','events.jsonl','kernels.jsonl','transfers.jsonl','apis.jsonl','gates.jsonl','tokens.jsonl']}
    info={name:read(source/name) for name in ['summary.json','session.json']}
    def first_work(d):return next(w for w in d['segments.jsonl'] if w['modeled'] and w['kind']=='node')
    def first_advance(d):return next(w for w in d['segments.jsonl'] if w['modeled'])['advance']
    def invalid_sync(d):
        for a in d['apis.jsonl']:
            if a['domain']=='runtime' and a['cbid']==165:a['return_value']=1
    mutations={
        'missing_activity':lambda d:d['kernels.jsonl'].pop(0),
        'duplicate_activity':lambda d:d['kernels.jsonl'].append(copy.deepcopy(d['kernels.jsonl'][0])),
        'missing_enqueue_correlation':lambda d:d['kernels.jsonl'][0].update(correlation=-1),
        'failed_device_synchronize':invalid_sync,
        'wrong_duration':lambda d:first_work(d).update(duration_ps=1001),
        'nonempty_work_not_modeled':lambda d:first_work(d).update(modeled=False),
        'production_kernel_disguised_as_snapshot':lambda d:first_work(d).update(kind='verification',modeled=False,duration_ps=0),
        'duplicate_or_missing_advance':lambda d:first_advance(d).update(offered_ps=first_advance(d)['offered_ps']+1000),
        'wrong_token_clock':lambda d:d['tokens.jsonl'][0].update(relative_ps=d['tokens.jsonl'][0]['relative_ps']+1),
        'wrong_tail_owner':lambda d:next(w for w in d['segments.jsonl'] if w['kind']=='tail').update(node_id=-1),
    }
    rejected=[]
    with tempfile.TemporaryDirectory(prefix='s5-audit-',dir=study) as tmp:
        out=Path(tmp).resolve();assert out.is_relative_to(study.resolve())
        (out/'session.json').touch()
        for name,mutate in mutations.items():
            altered=copy.deepcopy(data);mutate(altered)
            with patch.object(segment_audit,'rows',side_effect=lambda p:altered[p.name]),patch.object(segment_audit,'read',side_effect=lambda p:info[p.name]),patch.object(segment_audit,'write'):
                try:segment_audit.audit(out,1000)
                except (AssertionError,KeyError):rejected.append(name)
                else:raise AssertionError('Audit accepted mutation: '+name)
    result={'status':'pass','operation_identity':identities,'negative_audit_fixtures_rejected':rejected,
        'fixture_scope':'Mutated copies of raw records; not claims of real CUDA failures.',
        'source_sha256':sha(Path(__file__))}
    write(folder/'supplemental-validation.json',result)
    snapshot=folder/'source-and-binary-snapshot'/Path(__file__).name;snapshot.write_bytes(Path(__file__).read_bytes())
    with (folder/'metrics.csv').open('w',newline='',encoding='utf-8') as f:
        fields=['case','actual_outputs','segments','compute_ps','wait_ps','ttft_ps','tpot_ps','itl_ps','max_abs_residual_ps']
        wr=csv.DictWriter(f,fieldnames=fields);wr.writeheader()
        for c in stage['cases']:
            wr.writerow({'case':c['case'],'actual_outputs':c['actual_outputs'],'segments':c['segments']['modeled_segments'],
                'compute_ps':c['compute_ps'],'wait_ps':c['wait_ps'],'ttft_ps':c['metrics']['ttft_ps'],
                'tpot_ps':c['metrics']['tpot_ps'],'itl_ps':';'.join(map(str,c['metrics']['itl_ps'])),
                'max_abs_residual_ps':max(map(abs,c['segments']['conservation_residuals_ps']))})
    write(study/'artifact-index.json',{'files':[{'path':p.relative_to(study).as_posix(),'bytes':p.stat().st_size,'sha256':sha(p)}
        for p in sorted(study.rglob('*')) if p.is_file() and p.name!='artifact-index.json']})
    print(result)


if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--study',type=Path,required=True);a=ap.parse_args();run(a.study.resolve())
