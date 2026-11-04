"""Application-level records, metrics and immutable region selection."""
import hashlib
import json
import sys
from pathlib import Path

if sys.flags.optimize:
    raise RuntimeError('S experiment audits require Python assertions; do not use -O or PYTHONOPTIMIZE')


def sha(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def write(path, value):
    path = Path(path); path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes((json.dumps(value, indent=2, ensure_ascii=False) + '\n').encode())


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def rows(path):
    return [json.loads(line) for line in Path(path).read_text(encoding='utf-8').splitlines() if line]


def metrics(t0, times):
    if any(t < t0 for t in times) or times != sorted(times):
        raise ValueError('Backdated or unordered token time')
    itl = [b-a for a, b in zip(times, times[1:])]
    return {'actual_outputs': len(times), 'ttft_ps': times[0]-t0 if times else None,
            'itl_ps': itl, 'tpot_ps': (times[-1]-times[0])/(len(times)-1) if len(times) > 1 else None}


def replay_synthetic(events):
    tick=0;tokens=[];trace=[]
    for kind,value in events:
        if kind in ['compute','wait']:
            if type(value) is not int or value<0:raise ValueError('Invalid synthetic duration')
            tick+=value
        elif kind=='token':tokens.append(tick)
        elif kind!='EOS':raise ValueError('Unknown synthetic event')
        trace.append({'kind':kind,'value':value,'tick_ps':tick})
    return metrics(0,tokens),trace


def validate_compute_table(expected_header, expected_ids, table):
    if table['header'] != expected_header:
        raise ValueError('Compute table provenance mismatch')
    ids = [row['segment_id'] for row in table['segments']]
    if ids != expected_ids or len(set(ids)) != len(ids):
        raise ValueError('Missing, repeated or reordered segment')
    if any(type(row['duration_ps']) is not int or not 0 <= row['duration_ps'] < 2**64
           for row in table['segments']):
        raise ValueError('Invalid compute duration')


class RegionPlan:
    """Aliases name the same registered source bytes; padding is never selected."""
    def __init__(self, records, capacity):
        self.records = records
        self.canonical = {}
        self.names = set()
        for r in records:
            if r['region_id'] in self.names:
                raise ValueError('Duplicate region')
            self.names.add(r['region_id'])
            if 'alias_of' in r:
                continue
            if any(type(r[k]) is not int for k in ['source_offset', 'valid_bytes', 'global_base', 'registered_bytes']):
                raise ValueError('Noninteger region')
            start, length = r['source_offset'], r['valid_bytes']
            if start < 0 or length <= 0 or start % 64 or length % 64 or start+length > r['source_bytes']:
                raise ValueError('Source range/alignment')
            base, size = r['global_base'], r['registered_bytes']
            if base < 0 or base % 4096 or size != (length+4095)//4096*4096 or base+size > capacity:
                raise ValueError('Global range/capacity/padding')
            for q in self.canonical.values():
                if max(base, q['global_base']) < min(base+size, q['global_base']+q['registered_bytes']):
                    raise ValueError('Overlapping Global ranges')
                if r['storage_id'] == q['storage_id'] and max(start, q['source_offset']) < min(start+length, q['source_offset']+q['valid_bytes']):
                    raise ValueError('Overlapping source requires canonical alias')
            self.canonical[r['region_id']] = r
        for r in records:
            if 'alias_of' in r and r['alias_of'] not in self.canonical:
                raise ValueError('Unknown alias target')

    def intersect(self, storage, offset, size):
        if offset < 0 or size <= 0 or offset+size >= 2**64:
            raise ValueError('Callback range')
        result = []
        for r in self.canonical.values():
            if r['storage_id'] != storage:
                continue
            begin, end = max(offset, r['source_offset']), min(offset+size, r['source_offset']+r['valid_bytes'])
            if begin < end:
                result.append({'region_id': r['region_id'], 'source_offset': begin,
                               'region_offset': begin-r['source_offset'],
                               'global_address': r['global_base']+begin-r['source_offset'], 'bytes': end-begin})
        return sorted(result, key=lambda r: (r['source_offset'], r['region_id']))
