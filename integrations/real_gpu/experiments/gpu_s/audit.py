"""Independent readers of native GPU records and CPU-reference arrays."""
import re
import numpy as np
from common import read, rows, write


def array_error(actual, expected, target):
    a=np.asarray(actual,dtype=np.float64)
    b=np.asarray(expected,dtype=np.float64)
    if a.shape!=b.shape or not np.isfinite(a).all() or not np.isfinite(b).all():
        return {'pass':False,'reason':'shape or finite check failed','actual_size':a.size,'reference_size':b.size}
    a=a.reshape(-1);b=b.reshape(-1)
    d=a-b;rms=float(np.sqrt(np.mean(d*d)));scale=float(np.sqrt(np.mean(b*b)))
    relative=rms/scale if scale else (0.0 if rms==0 else float('inf'))
    denominator=np.maximum(np.abs(b),scale)
    ratio=float(np.max(np.abs(d)/denominator)) if scale else (0.0 if rms==0 else float('inf'))
    criteria=target['criteria']
    return {'pass':relative<=criteria['relative_rms_max'] and ratio<=criteria['element_error_over_max_reference_abs_or_reference_rms'],
            'elements':a.size,'max_abs':float(np.max(np.abs(d))),'rms':rms,'reference_rms':scale,
            'relative_rms':relative,'normalized_max':ratio}


def numerical(native, cpu, profile, target, result_path=None):
    ns=read(native/'summary.json');cs=read(cpu/'summary.json')
    checks=[];lookup={e['node_id']:e for e in rows(native/'events.jsonl') if e['type']=='node_before'}
    reference=np.load(cpu/'snapshots.npz')
    for meta in rows(native/'snapshots.jsonl'):
        node=lookup[meta['node_id']];name=meta['name'];op=node['op'];step=meta['step'];expected=None
        if 'kv_kind' in meta:
            name=meta['kv_kind']+'store-'+str(meta['layer']);expected=reference[f'{step}:{name}']
        elif re.fullmatch(r'[QK]cur-\d+',name) and op=='ROPE':expected=reference[f'{step}:{name}']
        elif re.fullmatch(r'Vcur-\d+',name) and op=='RESHAPE':expected=reference[f'{step}:{name}']
        elif re.fullmatch(r'l_out-\d+',name) and op=='ADD':expected=reference[f'{step}:{name}']
        elif name=='ffn_inp-10' and op=='ADD' or name in ['ffn_norm-10','result_norm'] and op=='MUL':expected=reference[f'{step}:{name}']
        if expected is None:continue
        dtype={'f16':np.dtype('<f2'),'f32':np.dtype('<f4')}[meta['dtype']]
        data=(native/meta['file']).read_bytes()
        actual=np.ndarray(tuple(reversed(meta['shape'])),dtype=dtype,buffer=data,strides=tuple(reversed(meta['strides']))).copy().squeeze()
        if 'kv_kind' in meta:
            # Native K cache is [context,kv_embd]; native V cache is transposed.
            valid=meta['valid_positions'];width=profile['kv_heads']*profile['head_dim']
            if meta['kv_kind']=='K':actual=actual.reshape(-1,width)[:valid]
            else:actual=actual.reshape(width,-1)[:,:valid].T
            expected=expected.reshape(valid,width)
        elif name==f"l_out-{profile['layers']-1}" and actual.size==profile['hidden']:
            expected=expected[-1:]
        if 'kv_kind' not in meta:expected=expected.squeeze()
        criteria=target['criteria']
        if 'logits' in criteria:
            group='kv_storage' if 'store-' in name else 'hidden_qkv';limit=criteria[group]
            current={'criteria':{'relative_rms_max':limit['relative_rms_max'],'element_error_over_max_reference_abs_or_reference_rms':limit['normalized_max']}}
        else:current=target;group='uniform'
        result=array_error(actual,expected,current);result.update(step=step,name=name,node_id=meta['node_id'],category=group)
        checks.append(result)
    gpu=np.fromfile(native/'logits.f32',dtype='<f4');oracle=np.fromfile(cpu/'logits.f32',dtype='<f4')
    if 'logits' in target['criteria']:
        limit=target['criteria']['logits'];logit_target={'criteria':{'relative_rms_max':limit['relative_rms_max'],'element_error_over_max_reference_abs_or_reference_rms':limit['normalized_max']}}
    else:logit_target=target
    checks.append(dict(array_error(gpu,oracle,logit_target),name='complete_logits',category='logits'))
    semantic=all(ns[k]==cs[k] for k in ['tokens','raw_tokens','stop_reason','positions','prompt_ids'])
    kv=sum('store-' in c['name'] for c in checks)
    expected_kv=2*profile['layers']*len(ns['raw_tokens'])
    expected_arrays=(6*profile['layers']+3)*len(ns['raw_tokens'])+1
    passed=semantic and len(checks)==expected_arrays and kv==expected_kv and all(c['pass'] for c in checks)
    result={'status':'pass' if passed else 'fail','token_boundary_exact':semantic,'valid_kv_arrays':kv,'expected_kv_arrays':expected_kv,
            'arrays_compared':len(checks),'expected_arrays':expected_arrays,'failed_arrays':sum(not c['pass'] for c in checks),'criteria':target['criteria'],'checks':checks,
            'max_abs':max(c.get('max_abs',float('inf')) for c in checks),
            'max_relative_rms':max(c.get('relative_rms',float('inf')) for c in checks),
            'max_normalized_element_error':max(c.get('normalized_max',float('inf')) for c in checks)}
    write(result_path if result_path is not None else native/'numerical.json',result);return result


def boundaries(native, profile):
    e=rows(native/'events.jsonl');k=rows(native/'kernels.jsonl');s=read(native/'summary.json')
    assert [x['event_id'] for x in e]==list(range(1,len(e)+1))
    before={x['node_id']:x for x in e if x['type']=='node_before'}
    after={x['node_id']:x for x in e if x['type']=='node_after'}
    assert before.keys()==after.keys()
    assert all(before[i]['event_id']<after[i]['event_id'] and before[i]['timestamp_ns']<=after[i]['timestamp_ns'] for i in before)
    selected=[];callbacks=[]
    for node in before.values():
        for access in node['weight_accesses']:
            callbacks.append(access['callback_id']);source=access['source']
            if source['name'] not in [profile['norm_name'],profile['down_name']]:continue
            assert node['op'] in ['MUL','MUL_MAT']
            finish=after[node['node_id']]
            matching=[x for x in k if x['step']==node['step'] and x['start_ns']>=node['timestamp_ns'] and x['end_ns']<=finish['timestamp_ns']]
            assert matching,'No actual GPU kernel between consumer callbacks'
            selected.append({'callback_id':access['callback_id'],'node_id':node['node_id'],'step':node['step'],
                             'tensor':source['name'],'full_source_bytes':access['source_bytes'],
                             'logical_accesses_per_consumer':1,'kernel_names':[x['name'] for x in matching],
                             'before_ns':node['timestamp_ns'],'after_ns':finish['timestamp_ns']})
    assert callbacks==list(range(1,len(callbacks)+1))
    steps=len(s['raw_tokens']);assert len(selected)==2*steps
    tokens=[x for x in e if x['type']=='token_emitted'];runs=[x for x in e if x['type']=='execution_begin']
    assert [x['token_id'] for x in tokens]==s['tokens']
    assert len(runs)==steps and runs[0]['position_begin']==0 and runs[-1]['position_end']==s['positions']
    assert s['positions']==len(s['prompt_ids'])+max(0,steps-1)
    assert len([x for x in e if x['type']=='prefill_finished'])==min(1,steps)
    for token in tokens:
        end=next(x for x in e if x['type']=='execution_end' and x['step']==token['step'])
        assert end['event_id']<token['event_id']
    assert s['hbf_submitted']==0 and s['cupti_dropped']==0
    result={'status':'pass','nodes_paired':len(before),'all_weight_callbacks':len(callbacks),'kernels':len(k),
            'selected_consumers':selected,'generated_tokens':s['tokens'],'positions':s['positions'],
            'native_kv':s['native_kv'],'extra_prediction_step':False,
            'hook_contract':'scheduler ask=true for EVERY node -> one-node graph dispatch -> backend synchronization -> ask=false',
            'scope':'Logical full input footprint once per actual consumer node; not physical load count. No expansion by prompt rows.'}
    write(native/'boundary-audit.json',result);return result


def stored_kv(native,profile,result_path=None):
    """Check exact GPU cache publication against GPU-produced values, not CPU tolerance."""
    metadata=rows(native/'snapshots.jsonl');nodes={x['node_id']:x for x in rows(native/'events.jsonl') if x['type']=='node_before'}
    published={};checks=[];width=profile['kv_heads']*profile['head_dim']
    for m in metadata:
        data=(native/m['file']).read_bytes();dtype={'f16':'<f2','f32':'<f4'}[m['dtype']]
        value=np.ndarray(tuple(reversed(m['shape'])),dtype=dtype,buffer=data,strides=tuple(reversed(m['strides']))).copy().squeeze()
        op=nodes[m['node_id']]['op'];name=m['name'];match=re.fullmatch(r'([KV])cur-(\d+)',name)
        if match and (match[1]=='K' and op=='ROPE' or match[1]=='V' and op=='RESHAPE'):
            key=(match[1],int(match[2]));current=value.reshape(-1,width).astype(np.float16)
            published[key]=current if key not in published else np.concatenate([published[key],current])
        if 'kv_kind' in m:
            key=(m['kv_kind'],m['layer']);valid=m['valid_positions']
            actual=value.reshape(-1,width)[:valid] if key[0]=='K' else value.reshape(width,-1)[:,:valid].T
            expected=published[key];exact=actual.shape==expected.shape and np.array_equal(actual.view(np.uint16),expected.view(np.uint16))
            checks.append({'step':m['step'],'kind':key[0],'layer':key[1],'valid_positions':valid,'exact':exact})
    expected_count=2*profile['layers']*len(read(native/'summary.json')['raw_tokens'])
    result={'status':'pass' if len(checks)==expected_count and all(c['exact'] for c in checks) else 'fail',
            'valid_arrays':len(checks),'expected_arrays':expected_count,'padding_excluded':True,'checks':checks}
    write(result_path if result_path is not None else native/'kv-publication-audit.json',result);return result
