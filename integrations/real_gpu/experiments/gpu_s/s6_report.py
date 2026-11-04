"""Report S6 results without changing protocol, samples or selected table."""
import csv,statistics,sys
from pathlib import Path
import numpy as np
from common import read,write,rows,sha
from study import ROOT

def run():
 out=ROOT/'output/gpu-s/20261101-s6-001';result=read(out/'stage-result.json');samples=rows(out/'compute-samples.jsonl');p=read(out/'sampling_protocol.json')
 values=np.array([[s['total_gpu_ps']]+s['step_gpu_ps'] for s in samples]); tables=[read(out/s['name']/'measured-segments.json') for s in samples]
 ids=[x['segment_id'] for x in tables[0]];assert all([x['segment_id'] for x in t]==ids for t in tables)
 durations=np.array([[x['duration_ps'] for x in t] for t in tables]);med=np.median(durations,axis=0);active=med>0
 ratios=np.zeros(len(ids));ratios[active]=(np.max(durations[:,active],axis=0)-np.min(durations[:,active],axis=0))/med[active]
 critical=[{'segment_id':x['segment_id'],'step':x['step'],'kind':x['kind'],'median_ps':float(med[i]),'range_over_median':float(ratios[i])} for i,x in enumerate(tables[0]) if x['kind'] in ['gap','tail'] and med[i]>0]
 summary={'status':result['status'],'samples':len(samples),'gpu_work_median_ms':float(np.median(values[:,0])/1e9),'gpu_work_min_ms':float(min(values[:,0])/1e9),'gpu_work_max_ms':float(max(values[:,0])/1e9),'reference_request_median_ms':statistics.median(s['reference_wall']['request_to_last_token_ns'] for s in samples)/1e6,'instrumented_request_median_ms':statistics.median(s['instrumented_wall']['request_to_last_token_ns'] for s in samples)/1e6,'critical_transfer_distributions':critical,'segment_range_ratio_quantiles':np.quantile(ratios[active],[.5,.9,1]).tolist(),'sub_microsecond_activities_per_sample':[s['low_duration_activities'] for s in samples],'protocol_sha256':sha(out/'sampling_protocol.json')}
 write(out/'distribution-report.json',summary)
 with (out/'instrumentation-comparison.csv').open('w',newline='') as f:
  wr=csv.writer(f);wr.writerow(['sample','gpu_active_ms','instrumented_request_ms','reference_request_ms'])
  for s in samples:wr.writerow([s['sample'],s['total_gpu_ps']/1e9,s['instrumented_wall']['request_to_last_token_ns']/1e6,s['reference_wall']['request_to_last_token_ns']/1e6])
 if (out/'compute-table.json').exists():
  table=read(out/'compute-table.json');selected=result['chosen_sample'];assert sum(x['duration_ps'] for x in table['segments'])==selected['total_gpu_ps']
  table['header'].update(prompt_sha256=sha(ROOT/'output/gpu-s/20261101-s5b-002/S5/prompt.txt'),prompt_ids=read(out/selected['name']/'summary.json')['prompt_ids'],tokens=read(out/selected['name']/'summary.json')['tokens'],environment=read(out/selected['name']/'summary.json')['environment'])
  write(out/'compute-table.json',table)
 write(ROOT/'docs/validation/gpu-s6.json',summary)
 print(summary)
if __name__=='__main__':run()
