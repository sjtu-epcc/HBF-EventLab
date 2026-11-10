"""Qualify existing S6 data for the user-authorized fixed-trace minimum scope."""
import copy
from pathlib import Path
from common import read,rows,write,sha
from batches import batch_path, MANIFEST
from study import ROOT

def run():
 source=batch_path('s6');out=batch_path('s6-qualified')
 assert not out.exists();out.mkdir(parents=True)
 old=read(source/'stage-result.json');assert old['status']=='inconclusive' and old['aggregate_sampling_status']=='pass'
 table=read(source/'compute-table.json');assert table['header']['sample']==old['chosen_sample']['name']
 # The selected sample and every measured value remain unchanged.
 selected=source/table['header']['sample'];raw=read(selected/'measured-segments.json');assert table['segments']==raw
 evidence=[]
 for i in range(1,old['samples']+1):
  d=source/f'sample-{i}';segments=read(d/'measured-segments.json')
  assert read(d/'segment-audit.json')['status']=='pass'
  a={x['activity_id']:x for f in ['kernels.jsonl','transfers.jsonl'] for x in rows(d/f)}
  total=sum(s['duration_ps'] for s in segments);owned=[]
  for s in segments:
   members=[a[j] for j in s['activity_ids']];owned+=s['activity_ids'];intervals=sorted((x['start_ns'],x['end_ns']) for x in members)
   union=0
   if intervals:
    lo,hi=intervals[0]
    for start,end in intervals[1:]:
     if start>hi:union+=hi-lo;lo,hi=start,end
     else:hi=max(hi,end)
    union+=hi-lo
   assert s['duration_ps']==union*1000
  assert len(set(owned))==len(owned)
  short=sum(x['end_ns']-x['start_ns'] for x in a.values() if x['end_ns']-x['start_ns']<1000)*1000
  gap_tail=sum(s['duration_ps'] for s in segments if s['kind'] in ['gap','tail'])
  evidence.append({'sample':i,'total_ps':total,'gap_tail_ps':gap_tail,'gap_tail_fraction':gap_tail/total,'sub_us_activity_duration_sum_ps':short,'sub_us_duration_sum_fraction':short/total})
 approval={'stage':'S6','status':'pass','acceptance_scope':'minimum_fixed_trace_mechanism_experiment',
 'precision_qualification':'unresolved_for_individual_short_segments_and_real_hardware_prediction',
 'user_authorization':'User explicitly permits a qualification annotation ignoring local fluctuations if they do not prevent the original minimum experiment.',
 'basis':'Original handoff sections 4 and 8 require measured reusable segments, correctness, complete boundaries and same-table G2/G3. They require acknowledging short-interval resolution limits, not per-segment repeatability thresholds.',
 'ignored_for_acceptance_only':['Large relative variability of individual short segments','No precision guarantee for sub-microsecond activity durations'],
 'retained':['All measured durations, activities, segment boundaries and raw samples','Numerical and causal correctness gates','Exact table identity, deterministic clock accounting and failures/drain checks for S7/S8'],
 'forbidden_claims':['Short activities have proven microsecond/sub-microsecond accuracy','Small simulated differences prove observable real-hardware speedup','Selected trace represents all native executions or calibrated HBF hardware'],
 'chosen_sample':table['header']['sample'],'sample_reselected':False,'measured_durations_changed':False,'historical_stage_result_sha256':sha(source/'stage-result.json'),
 'source_table_sha256':sha(source/'compute-table.json'),'protocol_sha256':sha(source/'sampling_protocol.json'),'evidence':evidence,
 'scope_note':'Variability remains a limitation. This is a retrospective user-authorized scope qualification, not a new pre-registered accuracy test or evidence that variability disappeared.'}
 frozen=copy.deepcopy(table);frozen['header'].update(frozen_G1=True,qualification='approved_for_minimum_fixed_trace_S7_S8',acceptance_scope=approval['acceptance_scope'],source_table_sha256=approval['source_table_sha256'],precision_limit=approval['precision_qualification'])
 write(out/'compute-table.json',frozen);approval['qualified_table_sha256']=sha(out/'compute-table.json');write(out/'stage-result.json',approval)
 if not MANIFEST: write(ROOT/'docs/validation/gpu-s6-minimum.json',approval)
 (out/'qualification-source.py').write_bytes(Path(__file__).read_bytes())
 write(out/'artifact-index.json',{'files':[{'path':p.relative_to(out).as_posix(),'bytes':p.stat().st_size,'sha256':sha(p)} for p in sorted(out.rglob('*')) if p.is_file() and p.name!='artifact-index.json']})
 print('S6 minimum fixed-trace scope pass; source sample/durations unchanged; historical inconclusive retained.')
if __name__=='__main__':run()
