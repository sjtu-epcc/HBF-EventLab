"""S2 diagnostic execution and explicit acceptance gates; never promotes failed calibration."""
import argparse
import subprocess
from pathlib import Path
from common import read,write,rows,sha
from study import native,environment,source_records,ROOT,BUILD
from platform_environment import executable
from reference import CpuQwen
from audit import numerical,boundaries,stored_kv


def run(study):
    assert read(study/'S0/stage-result.json')['status']=='pass'
    assert read(study/'S1/stage-result.json')['status']=='pass'
    folder=study/'S2';folder.mkdir()
    profile=read(study/'S0/profile.json');target=read(study/'S0/numerical-targets-predeclared.json')
    write(folder/'conditions.json',{'s0_lock_sha256':sha(study/'S0/study-lock.json'),'s1_stage_result_sha256':sha(study/'S1/stage-result.json'),
        'sources_and_binaries':source_records(),'arithmetic':'F16 exact weights/F32 matrix precision/F16 KV storage',
        'numerical_targets_sha256':sha(study/'S0/numerical-targets-predeclared.json'),
        'hbf_submitted':0,'shared_backend_modified':False,'warning':'Native diagnostics may continue if CPU calibration fails, but are not numerical acceptance or formal performance results.'})
    cpu=CpuQwen(profile);calibration=[];all_kv=[];all_boundaries=[]
    for name in target.get('calibration_inputs',['calibration-a','calibration-b']):
        gpu=native(study,name+'-2',study/'S0/inputs'/(name+'.txt'),2)
        reference=folder/(name+'-cpu');cpu.run(read(gpu/'summary.json')['prompt_ids'],2,reference)
        comparison=numerical(gpu,reference,profile,target);boundary=boundaries(gpu,profile);kv=stored_kv(gpu,profile)
        all_kv.append(kv);all_boundaries.append(boundary)
        calibration.append({'case':name,'native':str(gpu),'cpu':str(reference),
                            **{k:v for k,v in comparison.items() if k not in ['checks','criteria']}})
        print(name,comparison['status'],'arrays',comparison['arrays_compared'],flush=True)
    calibrated=all(x['status']=='pass' for x in calibration)
    contract=dict(target,status='qualified' if calibrated else 'not_qualified',calibration=calibration)
    write(folder/'numerical_contract.json',contract)
    # These formal-input native runs establish dispatch/token semantics only if
    # calibration is unqualified. They are never used to adjust CPU thresholds.
    native_cases=[];formal_checks=[]
    for count in [1,2]:
        gpu=native(study,'main-'+str(count),study/'S0/inputs/main.txt',count)
        boundary=boundaries(gpu,profile);kv=stored_kv(gpu,profile)
        all_boundaries.append(boundary);all_kv.append(kv);native_cases.append(gpu)
        if calibrated:
            reference=folder/('main-'+str(count)+'-cpu');cpu.run(read(gpu/'summary.json')['prompt_ids'],count,reference)
            comparison=numerical(gpu,reference,profile,target)
            formal_checks.append({'count':count,'status':comparison['status'],'artifact':str(gpu/'numerical.json')})
        print('main',count,read(gpu/'summary.json')['tokens'],flush=True)
    timing=native(study,'main-2-timing',study/'S0/inputs/main.txt',2,'timing')
    one,two=map(lambda x:read(x/'summary.json'),native_cases);timing_summary=read(timing/'summary.json')
    verify_timing_exact=(two['tokens']==timing_summary['tokens'] and
                         (native_cases[1]/'logits.f32').read_bytes()==(timing/'logits.f32').read_bytes())
    prefix_exact=(one['tokens']==two['tokens'][:1] and
                  (native_cases[0]/'logits.f32').read_bytes()==(native_cases[1]/'logits.f32').read_bytes()[:one['vocab_size']*4])
    assert verify_timing_exact and prefix_exact
    config=dict(profile,stop_before_selected=True);write(folder/'stop-config.json',config)
    stop=folder/'pre-dispatch-stop';cmd=[str(executable(BUILD/'bin','gpu_s_native')),profile['model_path'],str(study/'S0/inputs/main.txt'),str(stop),'1','verify',str(folder/'stop-config.json')]
    log=study/'logs/pre-dispatch-stop';log.mkdir()
    with (log/'stdout.log').open('w') as so,(log/'stderr.log').open('w') as se:
        result=subprocess.run(cmd,env=environment(),stdout=so,stderr=se,timeout=120)
    write(log/'command.json',{'argv':cmd,'returncode':result.returncode,'expected_returncode':86})
    assert result.returncode==86
    ev=rows(stop/'events.jsonl');ks=rows(stop/'kernels.jsonl')
    stopped=next(x for x in ev if x['type']=='consumer_stop_before_dispatch')
    assert not any(x['type']=='node_after' and x['node_id']==stopped['node_id'] for x in ev)
    assert not any(x['type']=='token_emitted' for x in ev)
    assert ks and max(x['end_ns'] for x in ks)<=stopped['timestamp_ns']
    assert read(stop/'intentional-stop.json')['cupti_dropped']==0
    stop_result={'status':'pass','node_id':stopped['node_id'],'consumer_dispatched_after_stop':False,
                 'tokens_emitted_after_stop':0,'evidence':'Actual CUDA run intentionally stopped before dispatch; NOT an HBF failure injection.'}
    write(folder/'pre-dispatch-audit.json',stop_result)
    assert all(x['status']=='pass' for x in all_kv)
    selected=all_boundaries[0]['selected_consumers']
    assert all(x['full_source_bytes']>=8192 for x in selected if x['tensor']==profile['down_name'])
    candidates=[{'id':'M1-norm','tensor':profile['norm_name'],'offset':0,'bytes':profile['hidden']*4},
        {'id':'M2-down','tensor':profile['down_name'],'offset':0,'bytes':profile['ffn']*2},
        {'id':'M8-down','tensor':profile['down_name'],'offset':0,'bytes':8192},
        {'id':'M2-offset-norm','tensor':profile['norm_name'],'offset':64,'bytes':profile['hidden']*4-64},
        {'id':'M2-offset-down','tensor':profile['down_name'],'offset':64,'bytes':profile['ffn']*2-64}]
    for candidate in candidates:
        hits=[x for x in selected if x['tensor']==candidate['tensor']]
        assert len(hits)==2 and all(candidate['offset']+candidate['bytes']<=x['full_source_bytes'] for x in hits)
        candidate['covered_steps']=[x['step'] for x in hits]
        candidate['consumer_callback_ids']=[x['callback_id'] for x in hits]
    semantics={'schema_version':1,'status':'qualified_for_S_logical_input_footprints',
        'scope':'Full source byte footprint ONCE per actual consumer node; selected regions intersect this footprint; not per-token-row multiplication or physical load counts.',
        'hook':all_boundaries[0]['hook_contract'],'scheduler_source':str(ROOT/'third_party/llama.cpp-integrated/ggml/src/ggml-backend.cpp'),
        'scheduler_source_sha256':sha(ROOT/'third_party/llama.cpp-integrated/ggml/src/ggml-backend.cpp'),
        'observed_selected_consumers':selected,'native_positions':[one['positions'],two['positions']],
        'm1_bytes':profile['hidden']*4,'m2_down_bytes':profile['ffn']*2,'m8_range_supported':True,'candidate_ranges':candidates,
        'prefill_weight_reuse':'One logical footprint per consumer invocation, irrespective of batch rows; GPU kernel internal physical reuse is outside this model.',
        'future_gate':'A future S3 bridge call must be placed in this pre-dispatch branch and throw on dependency failure before returning to the scheduler.',
        'pre_dispatch_stop_audit':'pre-dispatch-audit.json','alias_scope':'Main selected tensors are distinct allocations; generic alias mapping is tested in S1.'}
    write(folder/'access_semantics.json',semantics)
    boundary_result={'status':'pass','main_1_tokens':one['tokens'],'main_2_tokens':two['tokens'],
        'positions':[one['positions'],two['positions']],'no_extra_step':True,'verify_timing_logits_exact':verify_timing_exact,
        'actual_outputs':[len(one['tokens']),len(two['tokens'])],'stop_reasons':[one['stop_reason'],two['stop_reason']],
        'one_vs_two_first_logits_exact':prefix_exact,'kv_publication_arrays_checked':sum(x['valid_arrays'] for x in all_kv),
        'kv_publication_exact':True,'hbf_submitted':0,'intentional_stop':stop_result}
    write(folder/'boundary-audit.json',boundary_result)
    accepted=calibrated and len(formal_checks)==2 and all(x['status']=='pass' for x in formal_checks)
    lock=dict(read(study/'S0/study-lock.json'));lock.update(status='s2_pass' if accepted else 's2_blocked_on_numerical_validation',
        pending=([] if accepted else ['B2_numerical_validation'])+['S6_sampling_protocol'],
        access_semantics_sha256=sha(folder/'access_semantics.json'),numerical_contract_sha256=sha(folder/'numerical_contract.json'),current_experiment_records=source_records())
    write(folder/'study-lock.json',lock)
    output={'stage':'S2','status':'pass' if accepted else ('blocked' if not calibrated else 'fail'),'B1_access_semantics':'pass',
        'B2_numerical_contract':'pass' if calibrated else 'not_qualified',
        'calibration':calibration,'native_boundary':boundary_result,
        'formal_main_cpu_acceptance':('pass' if accepted else 'fail') if calibrated else None,'formal_checks':formal_checks,
        'blocking_items':[] if accepted else (['Disjoint CPU/GPU calibration exceeds predeclared precision targets; no qualified threshold contract, so main CPU numerical acceptance and entry to S3 are withheld.'] if not calibrated else ['CPU calibration passed, but formal input failed the frozen numerical criteria. Entry to S3 is withheld.']),
        'thresholds_relaxed':bool(target.get('supersedes_previous',False)),
        'qualification_scope':'User-approved engineering criteria for this model, inputs and execution settings; not a proven forward-error bound.',
        'S3_started':False}
    write(folder/'stage-result.json',output)
    text='# S2\n\nStatus: '+output['status']+'.\n\nNative 1/2-output execution, actual consumer/kernel pairing, exact valid F16 KV publication, no extra prediction step, verify/timing logits equivalence and pre-dispatch interruption passed.\n\n'
    text+='Independent CPU calibration '+('passed.' if calibrated else 'did not qualify the predeclared precision targets. All raw calibration data and per-array errors are retained; thresholds were not relaxed. Main native results are diagnostic only. No S3 or formal performance experiment was executed.')+'\n'
    (folder/'report.md').write_bytes(text.encode())
    print('S2 status:',output['status'],flush=True)


if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--study',type=Path,required=True);args=ap.parse_args();run(args.study.resolve())
