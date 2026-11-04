"""Validate stream framing and reconstruct cross-flush observations against Collector.

Only test code keeps the whole stream in memory. The production sink does not.
"""
import json
from pathlib import Path
import subprocess
import sys


def validate(lines, snapshot):
    assert [e['sequence'] for e in lines] == list(range(1, len(lines) + 1))
    assert all(e['schema_version'] == 1 for e in lines)
    assert lines[-1]['type'] == 'StreamEnd'
    assert lines[-1]['at'] == snapshot['at']
    assert lines[-1]['stop_reason'] == snapshot['stop_reason']
    attempts, works, writes, globals_, banks, contexts = {}, {}, {}, {}, {}, {}
    current, areas, last = {}, {}, {}
    names = ['total', 'collecting', 'ready_waiting', 'programming']

    def integrate(ch, at):
        for name in names:
            areas[ch][name] += current[ch][name] * (at - last[ch])
        assert at >= last[ch]
        last[ch] = at

    def phase(p):
        return {0: 'collecting', 1: 'ready_waiting', 2: 'ready_waiting', 3: 'programming'}[p]

    for e in lines:
        kind = e['type']
        if kind == 'GlobalDefined':
            globals_[e['id']] = dict(e)
        elif kind == 'GlobalFinished':
            globals_[e['id']]['finished'] = e['at']
        elif kind == 'WorkDefined':
            works[e['id']] = e
            if e['content'] is not None:
                writes[e['channel'], e['local_byte'], e['content']] = e['id']
        elif kind == 'AttemptStarted':
            attempts[e['id']] = dict(e, handoff=e['at'])
        elif kind == 'AttemptLinked':
            a = attempts[e['id']]
            ids = e['work']
            if e['replay']:
                ids = [writes[a['channel'], a['local_byte'] + off, e['content']]
                       for off in range(0, a['bytes'], 64)]
            a['work_ids'] = ids
        elif kind == 'AttemptAdvanced':
            attempts[e['id']][['rx_arrival', 'accepted'][e['stage']]] = e['at']
        elif kind == 'AttemptDelivered':
            attempts[e['id']].update(delivered=e['at'], status=e['status'])
        elif kind == 'AttemptAssessed':
            attempts[e['id']]['assessed'] = e['at']
        elif kind == 'PendingInitialized':
            ch = e['channel']
            current[ch] = dict.fromkeys(names, 0)
            areas[ch] = dict.fromkeys(names, 0)
            last[ch] = e['at']
        elif kind == 'DluObserved':
            ch = e['channel']
            key = ch, e['dlu'], e['generation']
            integrate(ch, e['at'])
            if e['change'] == 0:
                contexts[key] = dict(opened_at=e['at'], phase=e['phase'], retired_at=None)
                current[ch]['total'] += 1
                current[ch][phase(e['phase'])] += 1
            else:
                d = contexts[key]
                current[ch][phase(d['phase'])] -= 1
                if e['change'] == 3:
                    current[ch]['total'] -= 1
                    d['retired_at'] = e['at']
                else:
                    current[ch][phase(e['phase'])] += 1
                d['phase'] = e['phase']
            assert current[ch]['total'] == sum(current[ch][n] for n in names[1:])
        elif kind == 'BankOperationStarted':
            banks[e['ticket']] = dict(issued_at=e['at'], completed_at=None)
        elif kind == 'BankOperationFinished':
            banks[e['ticket']]['completed_at'] = e['at']
    for g in snapshot['globals']:
        assert globals_[g['id']].get('finished') == g['finished']
    for a in snapshot['attempts']:
        for field in ('handoff', 'rx_arrival', 'accepted', 'delivered', 'assessed', 'status', 'work_ids'):
            assert attempts[a['id']].get(field) == a[field], (a['id'], field)
    for d in snapshot['dlu_contexts']:
        actual = contexts[d['channel'], d['local_dlu'], d['generation']]
        assert actual['opened_at'] == d['opened_at']
        assert actual['retired_at'] == d['retired_at']
    for ch in snapshot['pending_channels']:
        i = ch['channel']
        integrate(i, snapshot['at'])
        assert current[i] == ch['current']
        assert areas[i] == ch['area_context_ticks']
    for b in snapshot['bank_operations']:
        assert banks[b['ticket']] == {k: b[k] for k in ('issued_at', 'completed_at')}


def main():
    cli, workload, root = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    root.mkdir(parents=True, exist_ok=True)
    for case, args, expected in [('normal', [], 0), ('replay', ['--fail-sequence', '0'], 0),
                                 ('limited', ['--max-tick', '200000'], 4)]:
        outputs = []
        for buffer in (97, 65536):
            prefix = root / f'{case}-{buffer}'
            stream, summary = prefix.with_suffix('.jsonl'), prefix.with_suffix('.json')
            result = subprocess.run([str(cli), str(workload), '--stats-json', str(summary),
                                     '--stats-events', str(stream), '--set',
                                     f'output.statistics_buffer_bytes={buffer}', *args],
                                    capture_output=True, text=True)
            assert result.returncode == expected, result.stdout + result.stderr
            raw = stream.read_bytes()
            assert raw.endswith(b'\n') and b'\r\n' not in raw
            records = [json.loads(line) for line in raw.splitlines()]
            validate(records, json.loads(summary.read_text(encoding='utf-8')))
            outputs.append(raw)
        assert outputs[0] == outputs[1], 'flush threshold changed the observation stream'
    protected = root / 'protected.txt'
    protected.write_bytes(workload.read_bytes())
    before = protected.read_bytes()
    result = subprocess.run([str(cli), str(protected), '--stats-events', str(protected)], capture_output=True)
    assert result.returncode == 5 and protected.read_bytes() == before
    output = root / 'same.json'
    output.write_text('keep', encoding='utf-8')
    result = subprocess.run([str(cli), str(workload), '--stats-json', str(output), '--stats-events', str(output)], capture_output=True)
    assert result.returncode == 5 and output.read_text(encoding='utf-8') == 'keep'
    print('PASS event stream JSON, replay linkage, Pending integrals, Bank intervals, pause and path protection')


if __name__ == '__main__':
    main()
