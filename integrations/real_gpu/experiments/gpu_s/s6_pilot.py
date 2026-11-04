"""S6 feasibility pilot; not the pre-registered formal sampling run."""
import sys, statistics
from pathlib import Path
from common import read,write,rows,sha
from study import ROOT,invoke
from segment_audit import audit,signature
from s5_validate import operations

def main():
 prior=ROOT/'output/gpu-s/20261101-s5b-002/S5'; out=ROOT/'output/gpu-s/20261101-s6-pilot-001'
 assert not out.exists();out.mkdir(parents=True)
 binary=ROOT/'build-s3/bin/gpu_s_segmented.exe'; c=read(prior/'S6-main-case.json');assert sha(binary)==c['binary_sha256']
 cfg=read(prior/'configurations/native-timing-8.json');write(out/'config.json',cfg)
 write(out/'pilot-protocol.json',{'status':'feasibility_only','samples':3,'warmup':0,'binary_sha256':sha(binary),'purpose':'Check CUPTI GPU-activity coverage and within-segment idle gaps before selecting S6 measurement semantics. Not a formal G1 protocol or sampling run.'})
 base=operations(prior/'native-timing-8'); summaries=[]
 for i in range(3):
  d=out/f'sample-{i+1}';invoke([binary,cfg['model_path'],prior/'prompt.txt',d,8,'timing',out/'config.json'],out/'logs'/f'sample-{i+1}')
  audit(d,1000);assert operations(d)==base
  assert read(d/'summary.json')['tokens']==c['tokens']
  acts={a['activity_id']:a for f in ['kernels.jsonl','transfers.jsonl'] for a in rows(d/f)}
  total_union=total_span=multi=0; critical=[]
  for w in rows(d/'segments.jsonl'):
   if not w['modeled']:continue
   m=[acts[j] for j in range(w['first_activity_id'],w['first_activity_id']+w['activity_count'])];v=sorted((a['start_ns'],a['end_ns']) for a in m); start,end=v[0]; union=0
   for lo,hi in v[1:]:
    if lo>end:union+=end-start;start,end=lo,hi
    else:end=max(end,hi)
   union+=end-start;span=max(y for x,y in v)-min(x for x,y in v);total_union+=union;total_span+=span;multi+=len(v)>1
   if w['kind'] in ['gap','tail']:critical.append({'step':w['step'],'kind':w['kind'],'union_ns':union,'span_ns':span,'activities':len(v)})
  summaries.append({'sample':i+1,'gpu_union_ns':total_union,'activity_span_ns':total_span,'within_segment_idle_ns':total_span-total_union,'multi_activity_segments':multi,'critical_segments':critical,'process_wall_seconds':read(out/'logs'/f'sample-{i+1}'/'command.json')['process_wall_seconds']})
  print('pilot',i+1,'union_ns',total_union,'span_ns',total_span,flush=True)
 write(out/'pilot-result.json',{'status':'diagnostic_only','samples':summaries,'interpretation':'CUPTI activity union excludes idle gaps; activity span includes gaps. Neither alone establishes equivalence to CUDA events placed around the complete GPU segment. Process wall time includes startup and logging and is not request latency. Formal S6 not passed.'})
 write(out/'artifact-index.json',{'files':[{'path':x.relative_to(out).as_posix(),'bytes':x.stat().st_size,'sha256':sha(x)} for x in sorted(out.rglob('*')) if x.is_file() and x.name!='artifact-index.json']})
if __name__=='__main__':main()
