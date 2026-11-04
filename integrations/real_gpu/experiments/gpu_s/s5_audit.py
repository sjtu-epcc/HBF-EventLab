"""Independent S3 reads of CUDA, bridge, and public statistics records."""
import csv
import numpy as np
from common import read,rows,write,metrics


def normalized(value):
    if isinstance(value,dict):return {k:normalized(v) for k,v in value.items() if k not in ['address','timestamp_ns']}
    if isinstance(value,list):return [normalized(v) for v in value]
    return value


def snapshot_value(folder,meta,profile):
    value=np.ndarray(tuple(reversed(meta['shape'])),dtype={'f16':'<f2','f32':'<f4'}[meta['dtype']],
        buffer=(folder/meta['file']).read_bytes(),strides=tuple(reversed(meta['strides']))).copy()
    if 'kv_kind' in meta:
        width=profile['kv_heads']*profile['head_dim'];valid=meta['valid_positions']
        value=value.reshape(-1,width)[:valid] if meta['kv_kind']=='K' else value.reshape(width,-1)[:,:valid].T
    assert np.isfinite(value).all(),'Non-finite valid snapshot'
    return value


def exact(left,right,profile):
    a,b=read(left/'summary.json'),read(right/'summary.json')
    assert all(a[k]==b[k] for k in ['tokens','raw_tokens','stop_reason','positions','prompt_ids'])
    assert (left/'logits.f32').read_bytes()==(right/'logits.f32').read_bytes(),'Complete logits differ'
    am,bm=rows(left/'snapshots.jsonl'),rows(right/'snapshots.jsonl');assert len(am)==len(bm)>0
    for x,y in zip(am,bm):
        assert normalized(x)==normalized(y),'Snapshot identity/layout differs'
        u,v=snapshot_value(left,x,profile),snapshot_value(right,y,profile)
        assert u.shape==v.shape and u.tobytes()==v.tobytes(),'Valid snapshot bytes differ'
    assert normalized(rows(left/'events.jsonl'))==normalized(rows(right/'events.jsonl')),'Consumer/event sequence differs'
    result={'status':'pass','tokens_exact':True,'complete_logits_exact':True,'valid_snapshot_arrays':len(am),
        'valid_snapshot_bytes_exact':True,'unused_kv_padding_excluded':True,'all_raw_events_and_callback_ids_exact':True}
    return result


def positive(folder,native,regions,profile,compute_by_step=None):
    compute_by_step=compute_by_step or {}
    total_compute=sum(compute_by_step.values())
    comparison=exact(native,folder,profile)
    e=rows(folder/'events.jsonl');kernels=rows(folder/'kernels.jsonl');gates=rows(folder/'gates.jsonl')
    trace=rows(folder/'bridge.jsonl');stats=read(folder/'statistics.json');session=read(folder/'session.json')
    before={x['node_id']:x for x in e if x['type']=='node_before'}
    after={x['node_id']:x for x in e if x['type']=='node_after'}
    assert before.keys()==after.keys()
    expected=[]
    # Compute intersections from raw sources, independently of the C++ gate.
    for node in before.values():
        for callback in node['weight_accesses']:
            for r in regions:
                if callback['source']['name']!=r['tensor']:continue
                lo=max(callback['source_offset'],r['source_offset'])
                hi=min(callback['source_offset']+callback['source_bytes'],r['source_offset']+r['valid_bytes'])
                if lo<hi:expected.append({'node_id':node['node_id'],'callback_id':callback['callback_id'],
                    'region_id':r['region_id'],'source_offset':lo,'region_offset':lo-r['source_offset'],
                    'global_address':r['global_base']+lo-r['source_offset'],'bytes':hi-lo,'content_id':r['content_id']})
    steps=len(read(folder/'summary.json')['raw_tokens'])
    assert len(expected)==len(gates)==len(regions)*steps,'Every selected region must appear once per execution step'
    assert len({g['access_id'] for g in gates})==len(gates)
    by_global={r['id']:r for r in stats['globals']};wait=0;consumer_rows=[]
    read_rows=[x for x in stats['globals'] if x['op']=='read']
    assert all(x['result']=='success' and x['remaining']==0 and x['finished'] is not None for x in stats['globals'])
    all_mapped_read_globals=[]
    for expected_gate,gate in zip(expected,gates):
        assert all(expected_gate[k]==gate[k] for k in expected_gate),'Independent source/global intersection mismatch'
        assert gate['complete'] and gate['success'] and gate['unsubmitted_bytes']==0
        children=[x['global_id'] for x in trace if x['access_id']==gate['access_id'] and x['action']=='global-registered']
        assert len(children)==gate['registered_globals']==gate['finished_globals']
        assert children and len(set(children))==len(children)
        all_mapped_read_globals+=children
        assert gate['completed_ps']==max(by_global[g]['finished'] for g in children),'Parent does not finish at last child'
        offset=gate['global_address']
        for gid in children:
            g=by_global[gid];assert g['op']=='read' and 0<g['bytes']<=4096 and g['bytes']%64==0
            work=[x for x in stats['logical_work'] if x['global_id']==gid]
            assert sorted(x['local_byte'] for x in work)==list(range(offset,offset+g['bytes'],64))
            # StatWorkDefined.content stores write versions. Read expected_content
            # is verified by the sealed Host, and is intentionally not this field.
            assert all(x['channel']==0 and x['content'] is None and x['result']=='success' for x in work)
            offset+=g['bytes']
        assert offset==gate['global_address']+gate['bytes']
        start,finish=before[gate['node_id']],after[gate['node_id']]
        assert start['timestamp_ns']<=gate['before_ns']<=gate['after_ns']<=finish['timestamp_ns']
        actual_kernels=[k for k in kernels if k['step']==start['step'] and k['start_ns']>=start['timestamp_ns'] and k['end_ns']<=finish['timestamp_ns']]
        assert actual_kernels and all(k['start_ns']>=gate['after_ns'] for k in actual_kernels),'Consumer kernel starts before successful dependency'
        wait+=gate['completed_ps']-gate['offered_ps']
        consumer_rows.append({'node_id':gate['node_id'],'callback_id':gate['callback_id'],'kernels':[x['name'] for x in actual_kernels],
            'gate_finish_ns':gate['after_ns'],'first_kernel_start_ns':min(k['start_ns'] for k in actual_kernels)})
    assert set(all_mapped_read_globals)=={r['id'] for r in read_rows},'Unowned/extra read Global'
    attempts=[x for x in stats['attempts'] if x['op']=='read']
    assert len(attempts)==sum(g['bytes']//64 for g in read_rows)
    assert all(x['host_success'] is True and x['status']==0 for x in attempts),'Host expected-content verification failed'
    assert stats['summary']['host_verification_failures']==0
    # Verify ordinary initialization fully covers registered storage before t0.
    writes=[x for x in stats['logical_work'] if by_global[x['global_id']]['op']=='write']
    addresses=[a for r in regions for a in range(r['global_base'],r['global_base']+r['registered_bytes'],64)]
    assert sorted(x['local_byte'] for x in writes)==sorted(addresses)
    assert all(x['channel']==0 and x['content']==1 and x['result']=='success' for x in writes)
    assert session['status']=='Completed' and session['workload_success'] and session['input_closed']
    assert session['host_drained'] and session['device_drained'] and not session['dependency_failed']
    assert all(x['success'] and x['complete'] and x['completed_ps']<=session['request_t0_ps'] for x in session['initialization'])
    assert read(folder/'summary.json')['cupti_dropped']==0
    tokens=rows(folder/'tokens.jsonl');assert len(tokens)==steps
    assert session['accumulated_wait_ps']==wait and session['compute_ps']==total_compute
    delivered=[]
    for step,token in enumerate(tokens):
        step_wait=sum(g['completed_ps']-g['offered_ps'] for g in gates if g['step']<=step)
        step_compute=sum(v for k,v in compute_by_step.items() if k<=step)
        assert token['relative_ps']==step_wait+step_compute and token['tick_ps']-session['request_t0_ps']==step_wait+step_compute and token['compute_ps']==step_compute
        if token['type']=='token_emitted':delivered.append(token['tick_ps'])
        else:assert token['type']=='EOS' and step==steps-1
    m=metrics(session['request_t0_ps'],delivered)
    summary=read(folder/'summary.json')
    assert len([x for x in e if x['type']=='execution_begin'])==steps and summary['positions']==len(summary['prompt_ids'])+max(0,steps-1)
    valid=sum(r['valid_bytes'] for r in regions);registered=sum(r['registered_bytes'] for r in regions)
    coverage={'registered_valid_bytes':valid,'registered_storage_bytes':registered,'padding_bytes':registered-valid,
        'touched_union_bytes':sum(r['valid_bytes'] for r in regions),'cumulative_read_bytes':sum(x['bytes'] for x in gates),
        'read_globals':len(read_rows),'read_logical_fragments':sum(x['bytes']//64 for x in read_rows),
        'read_attempts':sum(x['op']=='read' for x in stats['attempts']),
        'model_weight_bytes':sum(t['bytes'] for t in profile['tensors']),
        'selected_tensor_bytes':sum(r['source_bytes'] for r in regions)}
    coverage['registered_model_fraction']=valid/coverage['model_weight_bytes']
    coverage['selected_tensor_fraction']=valid/coverage['selected_tensor_bytes']
    with (folder/'coverage.csv').open('w',newline='',encoding='utf-8') as f:
        writer=csv.DictWriter(f,fieldnames=list(coverage));writer.writeheader();writer.writerow(coverage)
    result={'status':'pass','gpu_native_exact':comparison,'coverage':coverage,'metrics':m,'compute_ps':total_compute,'wait_ps':wait,
        'conservation_residual_ps':tokens[-1]['relative_ps']-wait-total_compute,'consumers':consumer_rows,
        'all_child_results_success':True,'parent_max_child_verified':True,'independent_fragment_addresses_content_verified':True,
        'read_content_evidence':'Complete ordinary writes have content=1; sealed bridge sets read expected_content; original Host verifies token_for(expected_content, global_address); every public read attempt host_success=true. StatWorkDefined.content is null for reads.',
        'unselected_callbacks_submitted':0,'initialized_before_t0':True,'closed_and_drained':True,'formal_performance_result':False}
    write(folder/'audit.json',result);return result


def negative(folder,kind):
    session=read(folder/'session.json');failure=read(folder/'failure.json');events=rows(folder/'events.jsonl');kernels=rows(folder/'kernels.jsonl')
    assert session['dependency_failed'] and session['input_closed']
    assert not any(x['type'] in ['token_emitted','eos'] for x in events)
    assert not rows(folder/'tokens.jsonl') and not (folder/'summary.json').exists()
    assert failure['cupti_dropped']==failure['kernel_errors']==0
    if kind=='program-fail':
        assert not session['request_started'] and not events and not kernels
        assert not session['workload_success'] and session['host_drained'] and session['device_drained']
        assert all(not x['success'] for x in session['initialization']) and session['read_globals']==0
        details={'phase':'initialization','gpu_nodes_dispatched':0,'normal_failure_drained':True,'reason':session['initialization'][0]['reason']}
    else:
        assert session['request_started'] and session['status']=='LimitReached'
        assert not (session['host_drained'] and session['device_drained']) and session['host_unfinished_globals']>0
        gates=rows(folder/'gates.jsonl');assert len(gates)==1 and not gates[0]['success'] and not gates[0]['complete']
        failed=gate_id=session['failed_consumer_node_id'];assert failed==gates[0]['node_id']
        assert not any(x['type']=='node_after' and x['node_id']==gate_id for x in events)
        assert kernels and max(k['end_ns'] for k in kernels)<=gates[0]['before_ns'],'CUDA continued after failed dependency'
        before={x['node_id'] for x in events if x['type']=='node_before'};after={x['node_id'] for x in events if x['type']=='node_after'}
        assert before-after=={failed} and max(before)==failed
        details={'phase':'consumer_dependency','consumer_node_id':failed,'all_kernels_end_before_failed_gate':True,
                 'pending_globals':session['host_unfinished_globals'],'host_drained':session['host_drained'],
                 'device_drained':session['device_drained'],'state_retained_at_limit':True}
    result={'status':'pass','expected_negative_outcome_verified':True,'dependent_consumer_dispatched':False,'tokens_emitted':0,**details}
    write(folder/'audit.json',result);return result
