"""S6 CUPTI active-device work sampling; CPU request latency is a separate control."""
import statistics,json,shutil
from pathlib import Path
import numpy as np
from common import read,write,rows,sha,metrics
from batches import batch_path
from study import ROOT,invoke, JOINT_BUILD
from segment_audit import audit
from s5_validate import operations
from s5_audit import exact,normalized
from audit import numerical,stored_kv

def measure(d):
 acts={a['activity_id']:a for f in ['kernels.jsonl','transfers.jsonl'] for a in rows(d/f)}; table=[]
 for w in rows(d/'segments.jsonl'):
  if w['kind']=='verification':raise AssertionError('Verification I/O in timing')
  members=[acts[i] for i in range(w['first_activity_id'],w['first_activity_id']+w['activity_count'])]
  intervals=sorted((a['start_ns'],a['end_ns']) for a in members);total=0
  if intervals:
   start,end=intervals[0]
   for lo,hi in intervals[1:]:
    if lo>end:total+=end-start;start,end=lo,hi
    else:end=max(end,hi)
   total+=end-start
  table.append({'segment_id':w['segment_id'],'request_id':1,'step':w['step'],'phase':'prefill' if w['step']==0 else 'decode','kind':w['kind'],'node_id':w['node_id'],'duration_ps':int(total)*1000,'source':'CUPTI GPU activity interval union','activity_ids':[a['activity_id'] for a in members],'intervals_ns':intervals})
 return table

def wall(d):
 e=rows(d/'events.jsonl');t0=next(x['timestamp_ns'] for x in e if x['type']=='request_begin');ts=[x['timestamp_ns'] for x in e if x['type']=='token_emitted'];m=metrics(t0,ts)
 return {'ttft_ns':m['ttft_ps'],'itl_ns':m['itl_ps'],'request_to_last_token_ns':ts[-1]-t0,'scope':'Request begin to token ID available; excludes model/context load; includes request instrumentation.'}

def run():
 prior=batch_path('s5')/'S5';out=batch_path('s6');assert not out.exists();out.mkdir(parents=True)
 binary=JOINT_BUILD/'bin/gpu_s_segmented.exe';reference=JOINT_BUILD/'bin/gpu_s_reference.exe';identity=read(prior/'S6-main-case.json');assert sha(binary)==identity['binary_sha256']
 cfg=read(prior/'configurations/native-timing-8.json');write(out/'config.json',cfg)
 protocol={'version':1,'measurement':'Union of CUPTI kernel/memcpy/memset GPU timestamp intervals inside each S5 synchronized segment. Idle gaps excluded, overlapping streams counted once. Empty windows remain zero.',
 'design_revision':'CUPTI activity timestamps replace proposed CUDA event brackets; models GPU active work only, not segment makespan. CPU launch gaps and host costs excluded and reported separately.',
 'warmup_pairs':3,'minimum_pairs':10,'maximum_pairs':20,'batch_size':5,'check_at':[10,15,20],
 'stability':'For total GPU work and each of 8 token-step GPU-work sums: consecutive 5-sample batch medians differ <=10%, each batch (max-min)/median <=30%. All must pass. Stop at first pass or 20 pairs.',
 'basis':'Engineering precision target, not proof of population convergence. 10% drift and 30% range bound the accepted observed variability; no tail-latency or small speedup claims.',
 'resolution':'CUPTI integer ns storage is not proof of 1ns accuracy. Flag individual activities <1000ns as resolution-sensitive; report counts. No rounding up or fabricated tick. No per-short-segment accuracy claim.',
 'selection':'Complete sample whose total GPU work is nearest median of all measured samples; ties choose earliest. No selection using HBF waits.',
 'invalid':'Any sequence/coverage/numeric mismatch aborts acceptance, retained samples not silently discarded.',
 'binary_sha256':sha(binary),'reference_binary_sha256':sha(reference),'config_sha256':sha(out/'config.json'),'S5_result_sha256':sha(prior/'stage-result.json')}
 write(out/'sampling_protocol.json',protocol)
 snap=out/'sources';snap.mkdir()
 for p in [Path(__file__),Path(__file__).with_name('native_reference.cpp'),binary,reference]:shutil.copyfile(p,snap/p.name)
 def execute(name,exe,mode='timing'):
  d=out/name;invoke([exe,cfg['model_path'],prior/'prompt.txt',d,8,mode,out/'config.json'],out/'logs'/name);return d
 # Verify baseline precision and KV with full exports before timing.
 v=execute('reference-verification',reference,'verify');exact(prior/'native-8',v,cfg);assert stored_kv(v,cfg)['status']=='pass'
 target=read(batch_path('s2')/'S0/numerical-targets-predeclared.json');assert numerical(v,prior/'cpu-8',cfg,target)['status']=='pass'
 expected=operations(prior/'native-timing-8');samples=[];assessments=[];passed=False
 for i in range(-3,20):
  label='warmup-'+str(i+4) if i<0 else 'sample-'+str(i+1)
  d=execute(label,binary);audit(d,1000);assert operations(d)==expected
  ref=execute(label+'-reference',reference)
  for x in [d,ref]:
   assert read(x/'summary.json')['tokens']==identity['tokens'];assert (x/'logits.f32').read_bytes()==(prior/'native-8/logits.f32').read_bytes()
  table=measure(d);write(d/'measured-segments.json',table)
  sums=[sum(x['duration_ps'] for x in table if x['step']==s) for s in range(8)]
  a={'sample':i+1,'name':label,'total_gpu_ps':sum(sums),'step_gpu_ps':sums,'instrumented_wall':wall(d),'reference_wall':wall(ref),'low_duration_activities':sum(hi-lo<1000 for x in table for lo,hi in x['intervals_ns'])}
  write(d/'sample.json',a)
  print(label,'GPU ms',a['total_gpu_ps']/1e9,'reference request ms',a['reference_wall']['request_to_last_token_ns']/1e6,flush=True)
  if i<0:continue
  samples.append(a)
  with (out/'compute-samples.jsonl').open('a',encoding='utf-8') as f:f.write(json.dumps(a)+'\n')
  n=len(samples)
  if n in protocol['check_at']:
   data=np.array([[s['total_gpu_ps']]+s['step_gpu_ps'] for s in samples]);a,b=data[-10:-5],data[-5:];ma,mb=np.median(a,axis=0),np.median(b,axis=0)
   drift=np.abs(mb-ma)/ma;ra=np.ptp(a,axis=0)/ma;rb=np.ptp(b,axis=0)/mb;passed=bool(np.all(drift<=.10) and np.all(ra<=.30) and np.all(rb<=.30))
   assessments.append({'n':n,'median_drift':drift.tolist(),'previous_range_ratio':ra.tolist(),'latest_range_ratio':rb.tolist(),'pass':passed});write(out/'sampling-assessment.json',assessments)
   if passed:break
 if passed:
  median=statistics.median(s['total_gpu_ps'] for s in samples);chosen=min(samples,key=lambda s:(abs(s['total_gpu_ps']-median),s['sample']))
  selected = out/chosen['name']
  table = read(selected/'measured-segments.json')
  summary = read(selected/'summary.json')
  header = {
   'protocol_sha256': sha(out/'sampling_protocol.json'), 'binary_sha256': sha(binary),
   'model_sha256': cfg['gguf_sha256'], 'config_sha256': sha(out/'config.json'),
   'events_sha256': sha(selected/'events.jsonl'), 'sample': chosen['name'],
   'prompt_sha256': sha(prior/'prompt.txt'), 'prompt_ids': summary['prompt_ids'],
   'tokens': summary['tokens'], 'environment': summary['environment'],
   'unit': 'ps', 'conversion': 'CUPTI integer ns * 1000 exactly', 'semantics': protocol['measurement'],
  }
  write(out/'compute-table.json', {'header': header, 'segments': table})
 else:chosen=None
 write(out/'stage-result.json',{'stage':'S6','status':'inconclusive','aggregate_sampling_status':'pass' if passed else 'inconclusive','G1_table_frozen':False,'reason':'Critical-segment and resolution qualification requires separate review; aggregate stability alone is insufficient.','samples':len(samples),'chosen_sample':chosen,'formal_performance_claim':'GPU active work only; paired CPU request latencies reported separately','S7_started':False,'resolution_limit':'Individual sub-microsecond activities unresolved for precision claims; table retains measured values without padding.'})
 write(out/'artifact-index.json',{'files':[{'path':p.relative_to(out).as_posix(),'bytes':p.stat().st_size,'sha256':sha(p)} for p in sorted(out.rglob('*')) if p.is_file() and p.name!='artifact-index.json']})
 print('S6 aggregate protocol', 'pass' if passed else 'inconclusive', '; full S6 pending critical-segment review',flush=True)
if __name__=='__main__':run()
