"""S7 G2: deterministic record-only replay of the qualified S6 trace."""
import argparse
import copy
import csv
import itertools
import shutil
from pathlib import Path
from common import read, write, rows, sha, metrics, validate_compute_table, RegionPlan

ROOT = Path(__file__).resolve().parents[2]


def validate(table, expected, windows, events):
    validate_compute_table(expected['header'], [w['segment_id'] for w in windows], table)
    if table['segments'] != expected['segments']:
        raise ValueError('Measured segment identity or duration differs from qualified trace')
    h = table['header']
    if not h['frozen_G1'] or h['qualification'] != 'approved_for_minimum_fixed_trace_S7_S8':
        raise ValueError('Table is not qualified for the minimum experiment')
    if [e['event_id'] for e in events] != list(range(1, len(events)+1)):
        raise ValueError('Missing or repeated source event')
    for s, w in zip(table['segments'], windows):
        if any(s[k] != w[k] for k in ['segment_id', 'step', 'kind', 'node_id']):
            raise ValueError('Segment boundary identity mismatch')
        if w['kind'] == 'verification':
            raise ValueError('Verification I/O in timing trace')


def schedule(table, windows, events):
    # CPU timestamps establish recorded order only; their differences never
    # become model durations. Close a segment before its next semantic event.
    work = [(w['end_ns'], 0, s) for s, w in zip(table['segments'], windows)]
    semantic = [(e['timestamp_ns'], 1, e) for e in events]
    return sorted(work+semantic, key=lambda x: (x[0], x[1]))


def replay(table, windows, events, regions, folder):
    folder.mkdir(parents=True)
    plan = RegionPlan(regions, 8388608)
    tick = 0
    tokens, selected, emitted, source_ids = [], [], [], []
    fields = ['segment_id', 'request_id', 'step', 'phase', 'kind', 'node_id',
              'start_ps', 'end_ps', 'duration_ps', 'wait_ps']
    with (folder/'segments.csv').open('w', newline='', encoding='utf-8') as f, \
         (folder/'events.jsonl').open('w', encoding='utf-8') as ef:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for _, kind, record in schedule(table, windows, events):
            if kind == 0:
                end = tick+record['duration_ps']
                if end >= 2**64:
                    raise ValueError('Replay Tick overflow')
                writer.writerow({**{k: record[k] for k in fields[:6]},
                                 'start_ps': tick, 'end_ps': end,
                                 'duration_ps': record['duration_ps'], 'wait_ps': 0})
                tick = end
                continue
            e = record
            source_ids.append(e['event_id'])
            model_event = {k: v for k, v in e.items() if k != 'timestamp_ns'}
            model_event.update(tick_ps=tick, source_timestamp_ns=e['timestamp_ns'])
            ef.write(__import__('json').dumps(model_event)+'\n')
            if e['type'] == 'node_before':
                for cb in e['weight_accesses']:
                    for hit in plan.intersect(cb['source']['name'], cb['source_offset'], cb['source_bytes']):
                        selected.append({**hit, 'event_id': e['event_id'], 'node_id': e['node_id'],
                                         'callback_id': cb['callback_id'], 'step': e['step'],
                                         'tick_ps': tick, 'submitted': False, 'wait_ps': 0})
            if e['type'] in ['token_emitted', 'eos']:
                t = {'type': 'EOS' if e['type'] == 'eos' else 'token_emitted',
                     'token_id': e['token_id'], 'request_id': 1, 'step': e['step'],
                     'output_index': len(emitted)+1, 'tick_ps': tick, 'relative_ps': tick,
                     'request_t0_ps': 0, 'compute_ps': tick, 'wait_ps': 0,
                     'source_event_id': e['event_id']}
                tokens.append(t)
                if e['type'] == 'token_emitted':
                    emitted.append(tick)
    assert source_ids == [e['event_id'] for e in events]
    with (folder/'tokens.jsonl').open('w', encoding='utf-8') as f:
        for t in tokens:
            f.write(__import__('json').dumps(t)+'\n')
    write(folder/'selected-callbacks.json', selected)
    m = metrics(0, emitted)
    write(folder/'metrics.json', m)
    # No CUDA, bridge, Session, Host or Device is instantiated by this runner.
    write(folder/'zero-access.json', {'mode': 'record_only', 'request_t0_ps': 0,
          'gpu_executions': 0, 'bridge_sessions': 0, 'access_submissions': 0,
          'read_globals': 0, 'write_globals': 0, 'NAND_commands': 0,
          'HBF_wait_ps': 0, 'selected_callbacks_retained': len(selected),
          'logits_recomputed': False, 'finished_ps': tick})
    return tokens, selected, m


def independent_audit(folder, table, events, windows, regions):
    actual_tokens = rows(folder/'tokens.jsonl')
    with (folder/'segments.csv').open(newline='', encoding='utf-8') as f:
        actual_segments = list(csv.DictReader(f))
    assert len(actual_segments) == len(table['segments']) == 18072
    previous = 0
    for a, s in zip(actual_segments, table['segments']):
        assert int(a['segment_id']) == s['segment_id']
        assert int(a['duration_ps']) == s['duration_ps']
        assert int(a['start_ps']) == previous and int(a['wait_ps']) == 0
        assert int(a['end_ps'])-previous == s['duration_ps']
        previous = int(a['end_ps'])
    semantic_tokens = [e for e in events if e['type'] in ['token_emitted', 'eos']]
    assert len(actual_tokens) == len(semantic_tokens)
    steps = sorted({s['step'] for s in table['segments']})
    totals = [sum(s['duration_ps'] for s in table['segments'] if s['step'] == step) for step in steps]
    expected_times = list(itertools.accumulate(totals))
    assert [t['relative_ps'] for t in actual_tokens] == expected_times
    assert [t['token_id'] for t in actual_tokens] == [e['token_id'] for e in semantic_tokens]
    assert all(t['wait_ps'] == 0 and t['tick_ps'] == t['compute_ps'] for t in actual_tokens)
    trace = rows(folder/'events.jsonl')
    assert [{k: v for k, v in e.items() if k not in ['tick_ps', 'source_timestamp_ns']} for e in trace] == [
        {k: v for k, v in e.items() if k != 'timestamp_ns'} for e in events]
    # Independently compute byte intersections, without RegionPlan or replay code.
    expected = []
    for e in events:
        if e['type'] != 'node_before':
            continue
        for cb in e['weight_accesses']:
            for r in regions:
                if cb['source']['name'] != r['tensor']:
                    continue
                lo = max(cb['source_offset'], r['source_offset'])
                hi = min(cb['source_offset']+cb['source_bytes'], r['source_offset']+r['valid_bytes'])
                if lo < hi:
                    expected.append((e['event_id'], cb['callback_id'], r['region_id'], lo,
                                     hi-lo, r['global_base']+lo-r['source_offset']))
    hits = read(folder/'selected-callbacks.json')
    assert [(h['event_id'], h['callback_id'], h['region_id'], h['source_offset'], h['bytes'],
             h['global_address']) for h in hits] == expected
    assert len(hits) == 16 and not any(h['submitted'] for h in hits)
    # Every semantic node boundary must settle the matching preceding segment.
    for s, w in zip(table['segments'], windows):
        boundary = 'node_before' if s['kind'] == 'gap' else 'node_after' if s['kind'] == 'node' else 'execution_end'
        e = next(x for x in events if x['type'] == boundary and x['step'] == s['step'] and
                 (boundary == 'execution_end' or x['node_id'] == s['node_id']))
        assert w['end_ns'] <= e['timestamp_ns']
    result = {'status': 'pass', 'segments': len(actual_segments), 'events': len(trace),
              'active_segments': sum(s['duration_ps'] > 0 for s in table['segments']),
              'step_compute_ps': totals, 'token_relative_ps': expected_times,
              'all_token_conservation_residuals_ps': [0]*len(actual_tokens),
              'all_source_events_retained': True, 'selected_callback_intersections': len(hits),
              'source_tokens_exact': True, 'HBF_wait_ps': 0, 'total_ps': previous}
    write(folder/'audit.json', result)
    return result


def run(study):
    source = ROOT/'output/gpu-s/20261101-s6-minimum-acceptance-001'
    original = ROOT/'output/gpu-s/20261101-s6-001'
    qualified = read(source/'stage-result.json')
    assert qualified['status'] == 'pass'
    tablefile = source/'compute-table.json'
    assert sha(tablefile) == qualified['qualified_table_sha256']
    table = read(tablefile)
    sample = original/table['header']['sample']
    assert sha(sample/'events.jsonl') == table['header']['events_sha256']
    assert sha(original/'config.json') == table['header']['config_sha256']
    assert sha(original/'sampling_protocol.json') == table['header']['protocol_sha256']
    events, windows = rows(sample/'events.jsonl'), rows(sample/'segments.jsonl')
    assert table['segments'] == read(sample/'measured-segments.json')
    regionsfile = ROOT/'output/gpu-s/20261101-s4-001/S4/M2-regions.json'
    regions = read(regionsfile)
    validate(table, table, windows, events)
    assert not study.exists()
    folder = study/'G2'
    folder.mkdir(parents=True)
    inputs = folder/'inputs'
    inputs.mkdir()
    files = [tablefile, sample/'events.jsonl', sample/'segments.jsonl', regionsfile,
             original/'config.json', source/'stage-result.json']
    for p in files:
        shutil.copyfile(p, inputs/p.name)
    shutil.copyfile(Path(__file__), folder/'runner-source.py')
    write(folder/'conditions.json', {'stage': 'S7', 'mode': 'record_only', 'request_t0_ps': 0,
          'replicates': 3, 'HBF_wait_ps': 0, 'qualification_scope': qualified['acceptance_scope'],
          'source_paths_sha256': {str(p): sha(p) for p in files},
          'runner_sha256': sha(Path(__file__)), 'sampling_uncertainty_inherited': True,
          'no_gpu_or_backend_execution': True})
    outputs = []
    audits = []
    for i in range(1, 4):
        d = folder/f'replay-{i}'
        replay(table, windows, events, regions, d)
        audits.append(independent_audit(d, table, events, windows, regions))
        outputs.append({p.name: sha(p) for p in d.iterdir() if p.is_file()})
    assert outputs[0] == outputs[1] == outputs[2]
    # Exercise rejection before emitting a replay, retaining the exact mutations.
    mutations = {
        'missing_segment': lambda t: t['segments'].pop(),
        'duplicate_segment': lambda t: t['segments'].append(copy.deepcopy(t['segments'][0])),
        'reordered_segments': lambda t: t['segments'].reverse(),
        'wrong_model_hash': lambda t: t['header'].update(model_sha256='0'*64),
        'wrong_prompt_hash': lambda t: t['header'].update(prompt_sha256='0'*64),
        'wrong_event_hash': lambda t: t['header'].update(events_sha256='0'*64),
        'unqualified_table': lambda t: t['header'].update(frozen_G1=False),
        'negative_duration': lambda t: t['segments'][0].update(duration_ps=-1),
        'fractional_duration': lambda t: t['segments'][0].update(duration_ps=0.5),
        'altered_measured_duration': lambda t: t['segments'][0].update(duration_ps=1),
        'wrong_step_owner': lambda t: t['segments'][0].update(step=7),
    }
    rejected = []
    for name, mutate in mutations.items():
        bad = copy.deepcopy(table)
        mutate(bad)
        try:
            validate(bad, table, windows, events)
        except ValueError:
            rejected.append(name)
        else:
            raise AssertionError('Invalid table accepted: '+name)
    write(folder/'negative-cases.json', {'status': 'pass', 'rejected': rejected,
          'scope': 'Mutated table copies; original S6 artifacts unchanged.'})
    for name in ['tokens.jsonl', 'segments.csv', 'events.jsonl', 'metrics.json',
                 'selected-callbacks.json', 'zero-access.json', 'audit.json']:
        shutil.copyfile(folder/'replay-1'/name, folder/name)
    result = {'stage': 'S7', 'status': 'pass', 'mode': 'record_only',
              'acceptance_scope': qualified['acceptance_scope'], 'metrics': read(folder/'metrics.json'),
              'table_sha256': sha(tablefile), 'deterministic_replays': 3,
              'replicate_all_artifact_hashes_exact': True, 'negative_cases_rejected': rejected,
              'audit': audits[0], 'zero_access': read(folder/'zero-access.json'),
              'S8_started': False, 'logits_recomputed': False}
    write(folder/'stage-result.json', result)
    write(ROOT/'docs/validation/gpu-s7.json', result)
    write(study/'artifact-index.json', {'files': [
        {'path': p.relative_to(study).as_posix(), 'bytes': p.stat().st_size, 'sha256': sha(p)}
        for p in sorted(study.rglob('*')) if p.is_file() and p.name != 'artifact-index.json']})
    print(result['metrics'])
    print('S7 pass; 3 deterministic replays, 11 invalid tables rejected; S8 not executed.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--study', type=Path, required=True)
    run(parser.parse_args().study.resolve())
