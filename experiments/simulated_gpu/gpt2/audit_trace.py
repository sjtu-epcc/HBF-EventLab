"""Bounded-memory, exactly-once lifecycle audit for monotonically issued GPU IDs."""
import csv
import gzip
import json
import sys

def validate(path):
    active={};last_id=0;counts={'INGRESS':0,'COMPLETED':0,'RETURNED':0};latency=0;peak=0
    opener=gzip.open if str(path).endswith('.gz') else open
    with opener(path,'rt',newline='') as stream:
        reader=csv.DictReader(stream)
        if reader.fieldnames!=['event','id','source','tick_ps','address','bytes','write']:
            raise ValueError('unexpected GPU lifecycle schema')
        for row in reader:
            event=row['event'];identity=int(row['id']);tick=int(row['tick_ps'])
            fields=tuple(int(row[k]) for k in ('source','address','bytes','write'))
            if event not in counts:raise ValueError('GPU request failure or unknown event: '+event)
            counts[event]+=1
            if event=='INGRESS':
                if identity<=last_id:raise ValueError('duplicate or nonmonotonic admission')
                last_id=identity;active[identity]=[fields,tick,tick,event];peak=max(peak,len(active))
            else:
                if identity not in active:raise ValueError('completion/return without admission')
                old=active[identity]
                predecessor='INGRESS' if event=='COMPLETED' else 'COMPLETED'
                if old[3]!=predecessor or old[0]!=fields or tick<old[2]:
                    raise ValueError('duplicate, reordered or corrupted completion')
                old[2]=tick;old[3]=event
                if event=='RETURNED':latency+=tick-old[1];del active[identity]
    if active or not counts['INGRESS'] or len(set(counts.values()))!=1:raise ValueError('incomplete GPU lifecycle')
    return dict(valid=True,requests=counts['INGRESS'],max_inflight=peak,mean_return_latency_us=latency/counts['INGRESS']/1e6)

if __name__=='__main__':print(json.dumps(validate(sys.argv[1]),indent=2))
