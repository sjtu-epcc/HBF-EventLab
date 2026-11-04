"""Inspect existing CUPTI kernel coverage; does not create a compute table."""
import argparse,bisect
from pathlib import Path
from common import read,rows,write,sha

def run(case,out):
    out.mkdir(parents=True,exist_ok=False)
    events=rows(case/'events.jsonl');kernels=rows(case/'kernels.jsonl');summary=read(case/'summary.json')
    before={x['node_id']:x for x in events if x['type']=='node_before'}
    after={x['node_id']:x for x in events if x['type']=='node_after'}
    assert before.keys()==after.keys() and summary['cupti_dropped']==0
    windows=sorted((n['timestamp_ns'],after[i]['timestamp_ns'],i,n['step'],n['op']) for i,n in before.items())
    starts=[x[0] for x in windows];assigned={i:[] for i in before};outside=[];invalid=[]
    for k in kernels:
        if k['end_ns']<k['start_ns']:invalid.append(k);continue
        j=bisect.bisect_right(starts,k['start_ns'])-1
        if j<0 or k['end_ns']>windows[j][1] or k['step']!=windows[j][3]:outside.append(k)
        else:assigned[windows[j][2]].append(k)
    entries=[{'node_id':i,'step':before[i]['step'],'op':before[i]['op'],
        'kernel_count':len(assigned[i]),'kernel_names':[k['name'] for k in assigned[i]],
        'kernel_durations_ns':[k['end_ns']-k['start_ns'] for k in assigned[i]]} for i in before]
    write(out/'node-kernel-inventory.json',entries);write(out/'outside-kernels.json',outside)
    durations=[k['end_ns']-k['start_ns'] for k in kernels]
    result={'stage':'S5-S6-preparation','status':'diagnostic_only','nodes':len(before),'kernels':len(kernels),
        'nodes_with_kernels':sum(bool(x) for x in assigned.values()),'nodes_without_kernels':sum(not x for x in assigned.values()),
        'nodes_with_multiple_kernels':sum(len(x)>1 for x in assigned.values()),'outside_node_kernels':len(outside),
        'invalid_kernel_timestamps':len(invalid),'shortest_kernel_ns':min(durations) if durations else None,
        'longest_kernel_ns':max(durations) if durations else None,'source_events_sha256':sha(case/'events.jsonl'),
        'source_kernels_sha256':sha(case/'kernels.jsonl'),'caveat':'Existing trace records CUDA kernels only, not CUDA memcpy/memset activity; timing mode and verify mode include different instrumentation. Missing transfers cannot be inferred from an empty kernel window.',
        'compute_table_created':False,'S5_b_passed':False,'S6_started':False}
    write(out/'result.json',result);print(result,flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--case',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();run(a.case.resolve(),a.out.resolve())
