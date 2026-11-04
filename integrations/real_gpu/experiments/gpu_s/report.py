"""Summarize completed S0/S1/S2 records and hash their artifacts."""
import argparse
from datetime import datetime, timezone
from pathlib import Path
from common import read, write, sha


def criteria_text(criteria):
    if 'logits' in criteria:
        groups = []
        for name in ('hidden_qkv', 'kv_storage', 'logits'):
            limit = criteria[name]
            groups.append(f"{name}: relative RMS ≤ {limit['relative_rms_max']:.12g}, normalized max ≤ {limit['normalized_max']:.12g}")
        return '分类预登记目标：' + '；'.join(groups) + '。另要求 shape、有限性、token/EOS/位置精确一致。'
    return (f"预登记目标：RMS(error)/RMS(reference) ≤ {criteria['relative_rms_max']:.12g}；"
            f"max(|error|/max(|reference|, RMS(reference))) ≤ {criteria['element_error_over_max_reference_abs_or_reference_rms']:.12g}。"
            '另要求 shape、有限性、token/EOS/位置精确一致。')


def run(study):
    stages={name:read(study/name/'stage-result.json') for name in ['S0','S1','S2']}
    env=read(study/'S0/environment.json');profile=read(study/'S0/profile.json')
    s2=stages['S2'];contract=read(study/'S2/numerical_contract.json')
    bridge=read(study/'S1/real-bridge/summary.json');criteria=contract['criteria']
    report=['# S0—S2 实验执行结果','',f"Study：`{study.name}`。S0={stages['S0']['status']}；S1={stages['S1']['status']}；S2={s2['status']}。",
        '',f"设备：{env['gpu']}，CUDA capability {env['compute_capability']}；驱动信息 `{env['nvidia_smi']}`。",
        f"模型：Qwen3-0.6B-F16，SHA256 `{profile['gguf_sha256']}`。主输入 {len(read(study/'S0/inputs.json')['main']['prompt_ids'])} tokens，context/batch/ubatch=512/256/256。",
        '', '## 阶段结果','',
        '- S0：资源、输入分词与容量、GGUF/conversion 身份、依赖构建身份、H0 解析检查通过；未运行推理。',
        '- S1：合成事件回放得到 TTFT=35μs、ITL/TPOT=12μs；N=0/1、EOS、错配表和非法映射检查通过。',
        '- S1 真实桥接：16KiB 有效登记、12KiB 唯一触达、20KiB 累计读；3 个父读、5 个读 Global；320 个逻辑读片段的地址和父完成=max子完成独立核对通过。',
        f"- S1 resident=64 背压拒绝 {bridge['pressure']['rejections']} 次后完成；非零偏移、零计算、123456ps 尾段、持续 program-fail 和正常失败排空检查通过。",
        f"- S2 B1 访问语义：{s2['B1_access_semantics']}。真实消费者前后回调与 CUPTI kernel 配对；每次消费者仅计一次源范围，prefill 不按行复制访问。M1/M2/M8 和非零偏移范围均有实际消费者覆盖。",
        f"- S2 主输入原生输出：上限1得到 {s2['native_boundary']['main_1_tokens']}，上限2得到 {s2['native_boundary']['main_2_tokens']}；实际位置 {s2['native_boundary']['positions']}，均因长度结束，覆盖 prefill/decode。",
        f"- S2：检查 {s2['native_boundary']['kv_publication_arrays_checked']} 个有效 F16 KV 发布数组，GPU 存储与自身生成值的 F16 舍入逐位一致；未用填充字节参与比较。",
        '- S2：1/2 输出首步 logits 完全相同，verify/timing 的输出及完整 logits 完全相同；最后输出后没有额外预测步。预发射中止 fixture 阻止后续消费者和 token 发射；该 fixture 不代表 HBF 故障注入。',
        '', '## 独立 CPU 校准与阻断','',
        criteria_text(criteria),
        '目标依据为 F16 存储精度的工程保真要求；不声称它是完整网络前向误差上界。阈值文件在校准前写入，未按结果放宽。',
        '', '| 校准输入 | 比较数组 | 失败数组 | token/边界精确 | 最大相对 RMS | 最大归一化元素误差 |',
        '|---|---:|---:|---|---:|---:|']
    for row in s2['calibration']:
        report.append(f"| {row['case']} | {row['arrays_compared']} | {row['failed_arrays']} | {row['token_boundary_exact']} | {row['max_relative_rms']:.10g} | {row['max_normalized_element_error']:.10g} |")
    if contract['status']!='qualified':
        report += ['', '两组校准均有数组超过既定目标，B2 数值合同未成立。主输入 CPU 正式数值验收未开展；其原生运行结果仅支持执行边界诊断。S2 未通过进入条件，S3 未启动。',
            '每组检查包括全部层 hidden/QKV、有效 F16 KV 和完整 logits。已有证据表明 token 选择和 GPU KV 写入一致；剩余跨 CPU/GPU 数值差异的归因尚未完成，不能据此直接判定模型实现错误。']
    else:
        report += ['', f"B2 数值合同状态：{contract['status']}。主输入正式验收：{s2['formal_main_cpu_acceptance']}。"]
    report += ['', '## 复现与证据','',
        '- `S0/`：environment、inputs、profile、H0、预登记目标、study-lock、源码/二进制快照、依赖身份和构建缓存。',
        '- `S1/`：formula-check、mapping/negative-cases、coverage、独立地址审计、真实桥日志和完整 StatisticsCollector 输出。',
        '- `S2/`：各独立进程的事件、CUPTI kernel、完整 logits、快照与逐数组误差；CPU 独立快照；access_semantics、未合格 numerical_contract、boundary/stop 审计。',
        '- `logs/`：命令、退出码、stdout/stderr 与进程墙钟成本。`artifact-index.json`：全部产物字节数和 SHA256。',
        '- 执行命令见工作区 `experiments/gpu_s/README.md`；每次使用新目录，并与本次冻结身份核对。',
        '', '本轮没有 S3+ HBF 门控 GPU 实验，没有正式实测计算表或性能 TTFT/TPOT；35μs/12μs 仅为 S1 合成验收值。']
    (study/'report.md').write_bytes(('\n'.join(report)+'\n').encode('utf-8'))
    summary={'schema_version':1,'study':study.name,'recorded_utc':datetime.now(timezone.utc).isoformat(),
        'stages':{k:v['status'] for k,v in stages.items()},'model_sha256':profile['gguf_sha256'],
        'S2_B1':s2['B1_access_semantics'],'S2_B2':s2['B2_numerical_contract'],
        'calibration':s2['calibration'],'native_boundary':s2['native_boundary'],
        'thresholds_relaxed':s2['thresholds_relaxed'],'S3_started':False,'formal_performance_measured':False,
        'raw_results_directory':str(study),'report':'report.md'}
    write(study/'stage-summary.json',summary)
    paths=[p for p in sorted(study.rglob('*')) if p.is_file() and p.name!='artifact-index.json']
    index=[{'path':p.relative_to(study).as_posix(),'bytes':p.stat().st_size,'sha256':sha(p)} for p in paths]
    write(study/'artifact-index.json',{'files':index,'file_count':len(index),'total_bytes':sum(x['bytes'] for x in index)})
    print('Report:',study/'report.md','artifacts:',len(index),'bytes:',sum(x['bytes'] for x in index))


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--study',type=Path,required=True)
    run(parser.parse_args().study.resolve())
