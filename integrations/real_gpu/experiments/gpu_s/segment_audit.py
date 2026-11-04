"""Independent coverage and serial-clock audit from raw CUPTI and public records."""
import csv
from bisect import bisect_left
from collections import Counter
from common import rows, read, write


def signature(folder):
    return [(s['step'],s['kind'],s['node_id'],s['activity_count'],s['modeled'])
            for s in rows(folder/'segments.jsonl') if s['kind']!='verification']


def audit(folder, duration):
    windows=rows(folder/'segments.jsonl');events=rows(folder/'events.jsonl')
    activities=[dict(k,kind='kernel') for k in rows(folder/'kernels.jsonl')]+rows(folder/'transfers.jsonl')
    activities.sort(key=lambda a:a['activity_id'])
    apis=rows(folder/'apis.jsonl');api_by_id={a['correlation']:a for a in apis}
    assert len(api_by_id)==len(apis), 'Duplicate API correlation'
    # CUDA 13.0 cupti_runtime_cbid.h: cudaDeviceSynchronize_v3020 = 165.
    syncs=sorted((a for a in apis if a['domain']=='runtime' and a['cbid']==165),key=lambda a:a['start_ns'])
    sync_starts=[a['start_ns'] for a in syncs]
    assert [a['activity_id'] for a in activities]==list(range(1,len(activities)+1)), 'Lost/duplicate activity ID'
    assert [s['segment_id'] for s in windows]==list(range(1,len(windows)+1))
    assert all(a['end_ns']<=b['start_ns'] for a,b in zip(windows,windows[1:])), 'Overlapping windows'
    assert read(folder/'summary.json')['cupti_dropped']==0
    owned={};by_id={a['activity_id']:a for a in activities};by_step={};counts=Counter();copies=Counter()
    timestamp_discrepancies=[];previous_gpu_end=0
    for w in windows:
        members=[by_id[i] for i in range(w['first_activity_id'],w['first_activity_id']+w['activity_count'])]
        sync=syncs[bisect_left(sync_starts,w['end_ns'])-1]
        assert w['start_ns']<=sync['start_ns']<sync['end_ns']<=w['end_ns'] and sync['return_value']==0
        assert w['modeled']==(bool(members) and w['kind']!='verification')
        assert w['duration_ps']==(duration if w['modeled'] else 0)
        for a in members:
            assert a['activity_id'] not in owned
            api=api_by_id[a['correlation']]
            assert w['start_ns']<=api['start_ns']<api['end_ns']<=sync['start_ns'], 'Enqueue outside segment or after its device synchronization'
            assert api['return_value']==0 and a['start_ns']<a['end_ns']
            if not w['start_ns']<=a['start_ns']<a['end_ns']<=w['end_ns']:
                timestamp_discrepancies.append({'activity_id':a['activity_id'],
                    'gpu_start_minus_cpu_window_start_ns':a['start_ns']-w['start_ns'],
                    'cpu_window_end_minus_gpu_end_ns':w['end_ns']-a['end_ns']})
            assert a['step']==w['step']
            owned[a['activity_id']]=w
            if w['kind']=='verification':assert a['kind']=='memcpy' and a['copy_kind']==2, 'Unexpected verification work'
            else:
                counts[a['kind']]+=1
                if a['kind']=='memcpy':copies[str(a['copy_kind'])]+=1
        if members:
            assert previous_gpu_end<=min(a['start_ns'] for a in members), 'GPU activities overlap across mutually exclusive segments'
            previous_gpu_end=max(a['end_ns'] for a in members)
        if w['modeled']:by_step[w['step']]=by_step.get(w['step'],0)+duration
    begin=next(e['timestamp_ns'] for e in events if e['type']=='request_begin')
    end=next(e['timestamp_ns'] for e in events if e['type']=='request_end')
    request=[a for a in activities if a['start_ns']>=begin and a['end_ns']<=end]
    assert {a['activity_id'] for a in request}==set(owned), 'Request activity omitted from partition'
    assert all(a['end_ns']<=begin or a['start_ns']>=end or a['activity_id'] in owned for a in activities)
    nodes=[e for e in events if e['type']=='node_before'];after={e['node_id']:e for e in events if e['type']=='node_after'}
    gaps={w['node_id']:w for w in windows if w['kind']=='gap'}
    work={w['node_id']:w for w in windows if w['kind']=='node'}
    assert len(gaps)==len(work)==len(nodes)
    for n in nodes:
        g,w=gaps[n['node_id']],work[n['node_id']]
        assert g['end_ns']<=n['timestamp_ns']<=w['start_ns']<w['end_ns']<=after[n['node_id']]['timestamp_ns']
    steps=len(read(folder/'summary.json')['raw_tokens'])
    tails=[w for w in windows if w['kind']=='tail'];assert len(tails)==steps
    for step in range(steps):
        ns=[n for n in nodes if n['step']==step];tail=tails[step]
        run=next(e for e in events if e['type']=='execution_begin' and e['step']==step)
        done=next(e for e in events if e['type']=='execution_end' and e['step']==step)
        assert tail['step']==step and tail['node_id']==ns[-1]['node_id']
        assert run['timestamp_ns']<=gaps[ns[0]['node_id']]['start_ns']
        assert after[ns[-1]['node_id']]['timestamp_ns']<=tail['start_ns']<tail['end_ns']<=done['timestamp_ns']
        tail_activities=[a for a in request if owned[a['activity_id']]['segment_id']==tail['segment_id']]
        assert any(a['kind']=='memcpy' and a['copy_kind']==2 for a in tail_activities), 'Missing output D2H tail'
        first=next(w for w in windows if w['step']==step and w['modeled'])
        assert first['kind']=='gap', 'Input prefix must precede GPU nodes (empty view nodes may precede it)'
        assert any(a['kind']=='memcpy' and a['copy_kind']==1 and owned[a['activity_id']]==first for a in request)
    conservation=[]
    if (folder/'session.json').exists():
        session=read(folder/'session.json');gates=rows(folder/'gates.jsonl');tokens=rows(folder/'tokens.jsonl')
        timeline=[(w['start_ns'],0,w) for w in windows if w['modeled']]+[(g['before_ns'],1,g) for g in gates]
        tick=session['request_t0_ps']
        for _,kind,x in sorted(timeline):
            record=x['advance'] if kind==0 else x
            assert record['offered_ps']==tick, 'Compute/wait clock discontinuity or reordering'
            tick=record['completed_ps']
            if kind==0:assert tick-record['offered_ps']==duration
            else:
                assert work[x['node_id']]['start_ns']>=x['after_ns']
                assert gaps[x['node_id']]['end_ns']<=x['before_ns']<=x['after_ns']<=work[x['node_id']]['start_ns']
                assert all(api_by_id[a['correlation']]['end_ns']<=x['before_ns'] or api_by_id[a['correlation']]['start_ns']>=x['after_ns'] for a in request), 'GPU enqueue overlaps HBF wait'
        assert tick==session['finished_ps']
        for step,t in enumerate(tokens):
            compute=sum(v for k,v in by_step.items() if k<=step)
            wait=sum(g['completed_ps']-g['offered_ps'] for g in gates if g['step']<=step)
            residual=t['relative_ps']-compute-wait;assert residual==0
            assert t['compute_ps']==compute and t['wait_ps']==wait
            conservation.append(residual)
        assert session['compute_ps']==sum(by_step.values())
    fields=['segment_id','kind','node_id','step','start_ns','end_ns','first_activity_id','activity_count','modeled','duration_ps']
    with (folder/'segments.csv').open('w',newline='',encoding='utf-8') as f:
        wr=csv.DictWriter(f,fieldnames=fields,extrasaction='ignore');wr.writeheader();wr.writerows(windows)
    result={'status':'pass','window_count':len(windows),'modeled_segments':sum(w['modeled'] for w in windows),
        'modeled_segments_per_step':[sum(w['modeled'] and w['step']==i for w in windows) for i in range(steps)],
        'compute_by_step_ps':by_step,'request_activities':len(request),'production_activities':dict(counts),
        'production_copy_kinds':dict(copies),'verification_copies_excluded':sum(w['activity_count'] for w in windows if w['kind']=='verification'),
        'all_activities_owned_once':True,'synchronized_nonoverlap':True,'prefix_and_each_token_tail_covered':True,
        'causality_evidence':'Every GPU activity correlation maps to a successful enqueue API inside its window, followed by successful cudaDeviceSynchronize; all preceding windows synchronized before gate, all consumer enqueues after gate.',
        'cpu_gpu_timestamp_projection_discrepancies':timestamp_discrepancies,
        'cpu_gpu_timestamp_projection_is_not_a_causality_test':True,
        'gpu_activity_overlaps_hbf_wait':False if (folder/'session.json').exists() else None,
        'conservation_residuals_ps':conservation,'fixed_duration_ps':duration,'formal_performance_result':False}
    write(folder/'segment-audit.json',result);return result
