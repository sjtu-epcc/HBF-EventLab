"""Fail-closed extraction of token metrics from a complete GPU/HBF replay."""
import argparse
import csv
import json
import re
from pathlib import Path
from audit_trace import validate as validate_trace

def timing_rows(boundaries,trace_ids,cycles,core_mhz):
    if core_mhz<=0:raise ValueError('positive GPU clock required')
    if len(cycles)!=len(trace_ids):raise ValueError('incomplete GPU simulation')
    flat=[k for token in boundaries for k in token['kernel_ids']]
    if flat!=trace_ids or len(set(flat))!=len(flat):raise ValueError('token/kernel boundaries do not exactly cover the replay')
    if len(boundaries)<2:raise ValueError('TPOT requires at least two output tokens')
    if any(b<=a for a,b in zip([0]+cycles,cycles)):raise ValueError('nonmonotonic cumulative GPU cycles')
    index=0;previous=0;rows=[]
    for token in boundaries:
        if not token['kernel_ids']:raise ValueError('token has no GPU work')
        index+=len(token['kernel_ids']);end=cycles[index-1]
        rows.append(dict(output_index=token['output_index'],phase=token['phase'],token_id=token['token_id'],
                         kernel_count=len(token['kernel_ids']),end_cycle=end,completion_ms=end/(core_mhz*1000),
                         interval_ms=(end-previous)/(core_mhz*1000)))
        previous=end
    return rows

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--capture',type=Path,required=True)
    p.add_argument('--replay',type=Path,required=True)
    p.add_argument('--run',type=Path,required=True)
    p.add_argument('--reference',type=Path,required=True)
    p.add_argument('--core-mhz',type=float,default=1410)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    if (a.run/'run.rc').read_text().strip()!='0':raise ValueError('simulation did not exit successfully')
    capture=json.loads((a.capture/'metadata/result.json').read_text())
    reference=json.loads((a.reference/'result.json').read_text())
    if capture['generated_token_ids']!=reference['generated_token_ids']:raise ValueError('capture changed model outputs')
    if capture['prompt_token_ids']!=reference['prompt_token_ids']:raise ValueError('reference/capture prompt mismatch')
    manifest=json.loads((a.replay/'manifest.json').read_text())
    names=(a.replay/'kernelslist.g').read_text().splitlines()
    trace_ids=[int(re.match(r'kernel-(\d+)-',n)[1]) for n in names]
    log=(a.run/'run.log').read_text(errors='replace')
    matches=re.findall(r'kernel_launch_uid\s*=\s*(\d+)\s+.*?gpu_tot_sim_cycle\s*=\s*(\d+)',log,re.S)
    if [int(uid) for uid,_ in matches]!=list(range(1,len(names)+1)):raise ValueError('missing/duplicate GPU kernel completion')
    cycles=[int(c) for _,c in matches]
    def latest(prefix):
        lines=[line for line in log.splitlines() if line.startswith(prefix+' ')]
        if not lines:raise ValueError('missing '+prefix)
        return {k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',lines[-1])}
    gpu=latest('EVENTLAB_GPU');device=latest('EVENTLAB_DEVICE');pages=latest('EVENTLAB_PAGE')
    if not (gpu['admitted']==gpu['completed']==gpu['returned']>0 and gpu['failed']==gpu['owned']==0 and device['drained']==1):
        raise ValueError('HBF requests did not successfully drain')
    if pages['seeded_pages']!=0 or pages['image_pages']!=manifest['initialized_pages']:
        raise ValueError('weight image not initialized through normal Host path')
    preload_modes=re.findall(r'EVENTLAB_PRELOAD begin pages=\d+ write_mode=(\w+)',log)
    audit=validate_trace(a.run/'eventlab.csv')
    if audit['requests']!=gpu['admitted']:raise ValueError('GPU lifecycle count differs from simulator statistics')
    rows=timing_rows(capture['token_boundaries'],trace_ids,cycles,a.core_mhz)
    result=dict(status='complete',backend_base_commit='8ccb1f353630f893f761b017d621812b7b9d5bf2',
        model=capture['model'],dataset=capture['dataset'],prompt=capture['prompt'],
        generated_text=capture['generated_text'],generated_tokens=len(rows),precision=capture['precision'],
        ttft_ms=rows[0]['completion_ms'],tpot_ms=(rows[-1]['completion_ms']-rows[0]['completion_ms'])/(len(rows)-1),
        core_clock_mhz=a.core_mhz,total_gpu_cycles=cycles[-1],kernel_count=len(names),
        eventlab_gpu=gpu,eventlab_device=device,eventlab_page=pages,lifecycle_audit=audit,per_token=rows,
        host_write_mode=preload_modes[-1] if preload_modes else 'fragment64',
        model_sha256=capture['model_sha256'],dataset_sha256=capture['dataset_sha256'],
        scope='Trace-driven simulated GPU plus incremental EventLab; normal weight writes before timed inference; KV/activations in simulated HBM; excludes host/tokenization/loading/queue time; uncalibrated hardware model')
    a.output.write_text(json.dumps(result,indent=2)+'\n')
    with a.output.with_name('per-token.csv').open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
