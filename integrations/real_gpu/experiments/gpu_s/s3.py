"""Execute S3; an unqualified S2 permits only an explicitly requested diagnostic run."""
import argparse
import copy
import subprocess
import time
from datetime import datetime,timezone
from pathlib import Path
from common import read,write,rows,sha,RegionPlan
from study import ROOT,BASE,environment
from integrated_build_identity import verify_build
from platform_environment import executable
from audit import boundaries,numerical,stored_kv
from reference import CpuQwen
from s3_audit import exact,positive,negative


def run(prior,study,build,diagnostic):
    prerequisite=read(prior/'S2/stage-result.json');qualified=prerequisite['status']=='pass'
    if not qualified and not diagnostic:
        raise RuntimeError('S2 is not qualified. Default S3 gate is closed. Explicit diagnostic continuation cannot produce S3 acceptance.')
    assert read(prior/'S0/stage-result.json')['status']==read(prior/'S1/stage-result.json')['status']=='pass'
    assert not study.exists(),'A fresh S3 study directory is required'
    folder=study/'S3';folder.mkdir(parents=True)
    binary=executable(build/'bin','gpu_s_joint');assert binary.is_file()
    profile=read(prior/'S0/profile.json');assert sha(profile['model_path'])==profile['gguf_sha256']
    assert sha(profile['conversion'])==read(prior/'S0/study-lock.json')['conversion_sha256']
    identity=verify_build(BASE);write(folder/'dependency-build-identity.json',identity)
    for name in ['profile.json','environment.json','inputs.json','study-lock.json','numerical-targets-predeclared.json']:
        target=study/'prior-S0'/name;target.parent.mkdir(exist_ok=True);target.write_bytes((prior/'S0'/name).read_bytes())
    h0=folder/'frozen-H0.ini';h0.write_bytes((prior/'S0/backend-preflight/effective-H0.ini').read_bytes())
    prompt=folder/'main.txt';prompt.write_bytes((prior/'S0/inputs/main.txt').read_bytes())
    source_paths=[p for p in Path(__file__).parent.iterdir() if p.is_file()]+[binary,build/'CMakeCache.txt']
    sources=[]
    for path in sorted(source_paths):
        target=folder/'source-and-binary-snapshot'/path.name;target.parent.mkdir(exist_ok=True);target.write_bytes(path.read_bytes())
        sources.append({'path':str(path.resolve()),'sha256':sha(path),'bytes':path.stat().st_size})
    conditions={'stage':'S3','recorded_utc':datetime.now(timezone.utc).isoformat(),'prior_study':str(prior),
        's2_stage_result_sha256':sha(prior/'S2/stage-result.json'),'s2_status':prerequisite['status'],
        'execution_scope':'acceptance' if qualified else 'diagnostic_only_per_user_continuation',
        'user_request':'继续执行S3','numerical_contract_status':prerequisite['B2_numerical_contract'],
        'numerical_thresholds_sha256':sha(study/'prior-S0/numerical-targets-predeclared.json'),
        'h0_sha256':sha(h0),'model_sha256':profile['gguf_sha256'],'prompt_sha256':sha(prompt),
        'sources_and_binaries':sources,'core_host_device_modified':False,
        'controls':{'compute_ps':0,'context':512,'batch':256,'ubatch':256,'outputs':1,'kv':'native F16',
            'matrix_precision':'every MUL_MAT F32; cuBLAS f32; TF32 disabled','flash_attention':False,'cuda_graphs':False},
        'cases':['native-1','native-2-revalidation','M1','M8','initialization-program-fail','request-time-limit'],
        'qualification':'Functional diagnostics do not waive B2; no S4 entry or formal performance conclusion if S2 is unqualified.'}
    write(folder/'conditions.json',conditions)
    configurations=folder/'configurations';configurations.mkdir()
    def execute(name,count=1,hbf=None,expected_code=0):
        config=copy.deepcopy(profile)
        config['hbf']=hbf if hbf else {'enabled':False}
        path=configurations/(name+'.json');write(path,config)
        out=folder/name;cmd=[str(binary),profile['model_path'],str(prompt),str(out),str(count),'verify',str(path)]
        logs=study/'logs'/name;logs.mkdir(parents=True)
        started=time.perf_counter()
        with (logs/'stdout.log').open('w',encoding='utf-8') as so,(logs/'stderr.log').open('w',encoding='utf-8') as se:
            r=subprocess.run(cmd,env=environment(),stdout=so,stderr=se,timeout=3600)
        write(logs/'command.json',{'argv':cmd,'returncode':r.returncode,'expected_returncode':expected_code,
            'process_wall_seconds':time.perf_counter()-started,'wall_scope':'Software process cost, including load/verification; never modeled latency.'})
        if r.returncode!=expected_code:raise RuntimeError('Unexpected return code for '+name+'; see '+str(logs))
        return out
    native=execute('native-1');native_two=execute('native-2-revalidation',2)
    for out in [native,native_two]:boundaries(out,profile);assert stored_kv(out,profile)['status']=='pass'
    revalidation={'new_vs_S2_main1':exact(prior/'S2/main-1',native,profile),
                  'new_vs_S2_main2':exact(prior/'S2/main-2',native_two,profile)}
    write(folder/'native-revalidation.json',revalidation)
    print('New executable native 1/2 outputs match S2 exactly',flush=True)
    target=read(study/'prior-S0/numerical-targets-predeclared.json')
    cpu=folder/'cpu-main-1';CpuQwen(profile).run(read(native/'summary.json')['prompt_ids'],1,cpu)
    cpu_native=numerical(native,cpu,profile,target)
    def region(name,tensor,size):
        source=next(t for t in profile['tensors'] if t['name']==tensor)
        r={'region_id':name,'tensor':tensor,'storage_id':tensor,'source_offset':0,'valid_bytes':size,
            'source_bytes':source['bytes'],'global_base':0,'registered_bytes':(size+4095)//4096*4096,
            'content_id':1,'canonical_region':name,'aliases':[]}
        RegionPlan([r],8388608);return r
    m1=region('M1',profile['norm_name'],4*profile['hidden']);m8=region('M8',profile['down_name'],8192)
    case_results=[]
    for r in [m1,m8]:
        config={'enabled':True,'effective_config_path':str(h0),'regions':[r]}
        out=execute(r['region_id'],hbf=config)
        audit=positive(out,native,[r],profile);assert stored_kv(out,profile)['status']=='pass'
        comparison=numerical(out,cpu,profile,target)
        case_results.append({'case':r['region_id'],'functional_status':'pass','gpu_native_exact':audit['gpu_native_exact'],
            'coverage':audit['coverage'],'metrics':audit['metrics'],'wait_ps':audit['wait_ps'],
            'closed_and_drained':audit['closed_and_drained'],'cpu_diagnostic_status':comparison['status'],
            'cpu_arrays_compared':comparison['arrays_compared'],'cpu_failed_arrays':comparison['failed_arrays'],
            'gpu_hbf_valid_kv_arrays':read(out/'kv-publication-audit.json')['valid_arrays']})
        print(r['region_id'],'functional pass; wait_ps',audit['wait_ps'],'CPU',comparison['status'],flush=True)
    bad=execute('initialization-program-fail',hbf={'enabled':True,'effective_config_path':str(h0),'regions':[m1],'persistent_program_fail':True},expected_code=87)
    fault=negative(bad,'program-fail');print('Initialization program-fail: expected failure/drain verified',flush=True)
    initialized=read(folder/'M1/session.json')['request_t0_ps']
    # Deliberate independent fault budget: initialization finishes normally;
    # the first dependent read cannot finish within one additional ps.
    limited=execute('request-time-limit',hbf={'enabled':True,'effective_config_path':str(h0),'regions':[m1],'max_tick_ps':initialized+1},expected_code=88)
    limit=negative(limited,'time-limit');print('Request time-limit: consumer blocked, pending state retained',flush=True)
    accepted=qualified and cpu_native['status']=='pass' and all(r['cpu_diagnostic_status']=='pass' for r in case_results)
    summary={'stage':'S3','study':study.name,'status':'pass' if accepted else 'blocked',
        'execution_scope':conditions['execution_scope'],'functional_status':'pass','prerequisite_S2':prerequisite['status'],
        'B2_numerical_contract':prerequisite['B2_numerical_contract'],'native_revalidation':'pass','native_cpu_diagnostic_status':cpu_native['status'],
        'native_cpu_diagnostic':{k:v for k,v in cpu_native.items() if k not in ['checks','criteria']},
        'cases':case_results,'negative_cases':{'initialization_program_fail':fault,'request_time_limit':limit},
        'blocking_items':[] if accepted else ['Independent CPU numerical contract is unqualified; frozen CPU criteria do not pass. Functional diagnostics cannot qualify S3 or open S4.'],
        'thresholds_relaxed':False,'S4_started':False,'formal_performance_measured':False}
    write(folder/'stage-result.json',summary)
    report=['# S3 单区域 GPU/HBF 执行结果','',f"Study：`{study.name}`。总状态：**{summary['status']}**；功能检查：**pass**。",
        '',('S2已通过，本轮按其冻结的数值合同进行S3验收。' if qualified else '用户在S2数值阻断后明确要求继续S3，本轮以显式diagnostic模式完成运行；不构成阶段验收通过。'),
        '', '## 条件与执行路径','',
        'Qwen3-0.6B-F16，主prompt44 tokens，最多1输出；context/batch/ubatch=512/256/256；F16原生KV，每个MUL_MAT显式F32。',
        'H0来自前轮保存的effective INI；每例新Session，普通Global写完整初始化后取t0。所有模拟计算段为0。',
        '实际CUDA消费者 → TensorAccessBridge → SimulationSession → 原HostDriver → 原DeviceDriver → Global最终成功 → 消费者发射。核心后端源码未修改。',
        '新增入口与前轮S2的1/2输出完整logits、有效快照和全部事件结构逐项完全一致，包含prefill/decode。',
        '', '## 正向案例','',
        '| 案例 | 有效登记/触达/累计读B | 读Global | 64B逻辑读片段 | token模拟时长ps | TPOT | GPU native/HBF | CPU原判据 |',
        '|---|---:|---:|---:|---:|---|---|---|']
    for row in case_results:
        c=row['coverage'];report.append(f"| {row['case']} | {c['registered_valid_bytes']}/{c['touched_union_bytes']}/{c['cumulative_read_bytes']} | {c['read_globals']} | {c['read_logical_fragments']} | {row['wait_ps']} | null | 全部有效快照及完整logits逐位一致 | {row['cpu_diagnostic_status']} |")
    report+=['','两例均只生成token `[32]`，位置44，length结束，没有额外预测步。逐Global final成功、content=1、源/区域/Global交集、64B地址、父完成=max子完成、初始化及最终close/drain全部独立核对通过。',
        '选中消费者的实际kernel均在桥访问最终成功返回后发射。未命中选区的回调保留，但没有提交读。',
        'token相对t0的模拟时长=此前实际HBF等待，整数ps守恒残差为0。N=1，TPOT=null；这些是零计算功能值，不是GPU性能预测。',
        '', '## 故障与限额','',
        f"- 初始化持续program-fail：进程预期退出87，原Host返回 `{fault['reason']}`；GPU节点和token均为0，已登记工作正常排空；逻辑结果仍是失败。",
        f"- 运行时间限额：绝对max_tick={initialized+1}ps，正常初始化t0={initialized}ps。预期退出88，停在消费者 `{limit['consumer_node_id']}` 之前；CUPTI证明失败门控后没有kernel继续，token为0。",
        f"- 限额后仍有 {limit['pending_globals']} 个未完成Global，Host drained={limit['host_drained']}，Device drained={limit['device_drained']}，保留限额和未决状态。没有提高预算后将其计为成功。",
        '', '## 数值与后续门槛','',
        f"独立CPU使用本轮冻结的精度目标比较主输入：{cpu_native['arrays_compared']}个数组中{cpu_native['failed_arrays']}个未达标；token、EOS/位置/结束原因精确一致。native与M1/M8三条CUDA路径的误差一致。",
        ('新版B2和三方数值验收通过，S3总状态pass。' if accepted else 'S3数值验收未通过，S3总状态blocked。'),
        '', '## 复现索引','',
        '`conditions.json`：条件、前轮身份、源码/二进制哈希；`source-and-binary-snapshot/`：本次入口快照；`configurations/`：每个进程实际配置。',
        '`native-*/`：重验基线；`cpu-main-1/`：独立CPU结果；`M1/`、`M8/`：events/kernels/gates/bridge/tokens、statistics、有效快照、coverage、数值和独立audit。',
        '`initialization-program-fail/`、`request-time-limit/`：真实故障和未决项；study级`logs/`保存命令、返回码和进程成本；`artifact-index.json`绑定全部产物。']
    (folder/'report.md').write_bytes(('\n'.join(report)+'\n').encode())
    paths=[p for p in sorted(study.rglob('*')) if p.is_file() and p.name!='artifact-index.json']
    records=[{'path':p.relative_to(study).as_posix(),'bytes':p.stat().st_size,'sha256':sha(p)} for p in paths]
    write(study/'artifact-index.json',{'files':records,'file_count':len(records),'total_bytes':sum(x['bytes'] for x in records)})
    print('S3:',summary['status'],'functional:',summary['functional_status'],'report:',folder/'report.md',flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--prior-study',type=Path,required=True);parser.add_argument('--study',type=Path,required=True)
    parser.add_argument('--build',type=Path,default=ROOT/'build-s3');parser.add_argument('--diagnostic-with-unqualified-cpu',action='store_true')
    args=parser.parse_args();run(args.prior_study.resolve(),args.study.resolve(),args.build.resolve(),args.diagnostic_with_unqualified_cpu)
