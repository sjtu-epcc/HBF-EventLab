"""CPU-only S8 contracts for one complete, already-frozen execution trajectory.

These checks do not qualify a new GPU trace. EOS/different output counts require
that trajectory's own complete frozen table; no table suffix is discarded.
Wall timestamps never contribute to token metrics or integer conservation.
"""
import csv
from pathlib import Path

from common import metrics, read, rows, validate_compute_table, write
from s8_segment_audit import audit as segment_audit

UINT64_MAX = 2**64 - 1
KINDS = {'gap', 'node', 'tail'}
PAIRED_FIELDS = [
    'output_index', 'type', 'step', 'token_id', 'G2_relative_ps', 'G3_relative_ps',
    'step_compute_ps', 'step_wait_ps', 'cumulative_wait_ps', 'G2_interval_ps',
    'G3_interval_ps', 'conservation_residual_ps',
]


def require(okay, message):
    if not okay:
        raise ValueError(message)


def uint64(value, name):
    require(type(value) is int and 0 <= value <= UINT64_MAX, 'Invalid uint64 '+name)
    return value


def validate_frozen_table(table, expected_header=None):
    """Validate identities, not a historical segment-count constant."""
    header = table['header']
    require(isinstance(header, dict), 'Invalid compute header')
    require(header.get('frozen_G1') is True, 'Compute table not frozen')
    segments = table['segments']
    require(isinstance(segments, list), 'Invalid compute segments')
    validate_compute_table(header if expected_header is None else expected_header,
                           list(range(1, len(segments)+1)), table)
    identities = set()
    previous_step = 0
    for index, segment in enumerate(segments):
        uint64(segment['segment_id'], 'segment_id')
        require(segment['kind'] in KINDS, 'Invalid compute segment kind')
        uint64(segment['node_id'], 'node_id')
        owner = uint64(segment['step'], 'step')
        require(owner <= 2**31-1, 'Compute step out of range')
        require(owner == 0 if index == 0 else owner in (previous_step, previous_step+1),
                'Missing/reordered compute step')
        previous_step = owner
        key = (owner, segment['kind'], segment['node_id'])
        require(key not in identities, 'Duplicate compute boundary')
        identities.add(key)
    return segments


def validate_table_windows(table, windows, expected_header=None):
    """Full table consumption and active/empty coverage before the CUPTI audit."""
    expected = validate_frozen_table(table, expected_header)
    actual = []
    for window in windows:
        uint64(window['activity_count'], 'activity_count')
        uint64(window['duration_ps'], 'duration_ps')
        require(type(window['modeled']) is bool, 'Invalid modeled flag')
        if window['kind'] == 'verification':
            require(window['table_segment_id'] is None and window['duration_ps'] == 0
                    and not window['modeled'], 'Verification must not consume model time/table')
        else:
            actual.append(window)
    require(len(actual) == len(expected), 'Missing/extra/unconsumed compute segments')
    for actual_row, frozen in zip(actual, expected):
        uint64(actual_row['table_segment_id'], 'table_segment_id')
        uint64(actual_row['node_id'], 'node_id')
        uint64(actual_row['step'], 'step')
        require(actual_row['table_segment_id'] == frozen['segment_id'],
                'Missing/duplicate/reordered compute segment')
        require(all(actual_row[key] == frozen[key]
                    for key in ('kind', 'node_id', 'step', 'duration_ps')),
                'Compute segment boundary/duration mismatch')
        active = actual_row['activity_count'] > 0
        require(active == (frozen['duration_ps'] > 0),
                'Actual GPU activity coverage differs from frozen table')
        require(actual_row['modeled'] == active, 'Actual modeled coverage mismatch')


def table_audit(folder, table, expected_header=None):
    folder = Path(folder)
    validate_table_windows(table, rows(folder/'segments.jsonl'), expected_header)
    # Keep the independent raw activity/API/causality audit, not just row equality.
    return segment_audit(folder, table)


def _token_trace(folder):
    records = rows(folder/'tokens.jsonl')
    events = rows(folder/'events.jsonl')
    require([uint64(e['event_id'], 'event_id') for e in events] == list(range(1, len(events)+1)),
            'Missing/duplicate/reordered source event')
    require(events and events[0]['type'] == 'request_begin' and events[-1]['type'] == 'request_end',
            'Incomplete request events')
    require(sum(e['type'] == 'request_begin' for e in events) == 1
            and sum(e['type'] == 'request_end' for e in events) == 1,
            'Expected exactly one request')
    require(all(type(e['request_id']) is int and e['request_id'] == 1 for e in events),
            'Unexpected request identity')
    semantic = [e for e in events if e['type'] in ('token_emitted', 'eos', 'EOS')]
    require(len(records) == len(semantic), 'Unpaired token/semantic event')
    normalized = []
    origin = None
    emitted = 0
    for index, (token, event) in enumerate(zip(records, semantic)):
        require(event['type'] in ('token_emitted', 'eos'), 'Invalid semantic EOS type')
        kind = 'EOS' if event['type'] == 'eos' else 'token_emitted'
        require(token['type'] == kind, 'Token/semantic event type mismatch')
        token_id = uint64(token['token_id'], 'token_id')
        require(token_id == uint64(event['token_id'], 'event token_id'), 'Token/semantic ID mismatch')
        owner = uint64(event['step'], 'event step')
        require(owner == index, 'Missing/duplicate/reordered token step')
        # Legacy S3Gate::token records omit step: bind to the actual semantic
        # event, never infer identity only from the record's ordinal position.
        if 'step' in token:
            require(uint64(token['step'], 'token step') == owner, 'Token/semantic step mismatch')
        if 'request_id' in token:
            require(type(token['request_id']) is int and token['request_id'] == event['request_id'],
                    'Token/semantic request mismatch')
        if 'source_event_id' in token:
            require(uint64(token['source_event_id'], 'source_event_id') == event['event_id'],
                    'Token/source event mismatch')
        require(uint64(token['output_index'], 'output_index') == emitted+1, 'Invalid output index')
        if kind == 'EOS':
            require(index == len(semantic)-1, 'EOS must be the terminal ready event')
        else:
            emitted += 1
            require(uint64(event['output_index'], 'event output_index') == emitted,
                    'Invalid semantic output index')
        for name in ('request_t0_ps', 'tick_ps', 'relative_ps', 'compute_ps', 'wait_ps'):
            uint64(token[name], name)
        if 'tick_ps' in event:
            require(uint64(event['tick_ps'], 'event tick_ps') == token['tick_ps'],
                    'Token/semantic model clock mismatch')
        if origin is None:
            origin = token['request_t0_ps']
        require(token['request_t0_ps'] == origin, 'Request clock origin changed')
        require(token['tick_ps']-origin == token['relative_ps'], 'Relative token clock mismatch')
        require(token['relative_ps'] == token['compute_ps']+token['wait_ps'],
                'Token compute/wait conservation mismatch')
        require(not normalized or token['tick_ps'] >= normalized[-1]['tick_ps'], 'Backdated token clock')
        normalized.append(dict(token, step=owner))
    require(uint64(events[-1]['actual_outputs'], 'actual_outputs') == emitted, 'Request output count mismatch')
    eos = bool(normalized and normalized[-1]['type'] == 'EOS')
    require(events[-1]['reason'] == ('EOS' if eos else 'length'), 'Request stop reason mismatch')
    # With no ready events there are no observed timestamps: metrics([], t0)
    # is independent of t0. No synthetic latency is introduced by this origin.
    return normalized, 0 if origin is None else origin


def pair(folder, g2, table):
    folder, g2 = Path(folder), Path(g2)
    segments = validate_frozen_table(table)
    t3, origin3 = _token_trace(folder)
    t2, origin2 = _token_trace(g2)
    identity = lambda token: (token['type'], token['token_id'], token['step'])
    require([identity(t) for t in t3] == [identity(t) for t in t2],
            'Unpaired G2/G3 event type/token/step')
    table_sums = {}
    for segment in segments:
        owner = segment['step']
        table_sums[owner] = uint64(table_sums.get(owner, 0)+segment['duration_ps'], 'step compute sum')
    require(list(table_sums) == [t['step'] for t in t3],
            'Table must cover exactly the complete token/EOS trajectory; no suffix may be discarded')
    waits = {owner: 0 for owner in table_sums}
    previous_gate_end = origin3
    previous_gate_step = -1
    for gate in rows(folder/'gates.jsonl'):
        owner = uint64(gate['step'], 'gate step')
        require(owner in waits and owner >= previous_gate_step, 'Unowned/reordered HBF gate step')
        offered = uint64(gate['offered_ps'], 'offered_ps')
        completed = uint64(gate['completed_ps'], 'completed_ps')
        require(gate['complete'] is True and gate['success'] is True, 'Unsuccessful HBF gate')
        require(previous_gate_end <= offered <= completed, 'Backdated/overlapping HBF gate')
        lower = t3[owner-1]['tick_ps'] if owner else origin3
        require(lower <= offered <= completed <= t3[owner]['tick_ps'], 'HBF gate outside its token step')
        waits[owner] = uint64(waits[owner]+completed-offered, 'step wait sum')
        previous_gate_end, previous_gate_step = completed, owner
    paired_events = []
    cumulative_compute = cumulative_wait = 0
    previous2 = previous3 = 0
    for a, b in zip(t3, t2):
        owner = a['step']
        compute, wait = table_sums[owner], waits[owner]
        cumulative_compute = uint64(cumulative_compute+compute, 'cumulative compute')
        cumulative_wait = uint64(cumulative_wait+wait, 'cumulative wait')
        require(a['compute_ps'] == b['compute_ps'] == cumulative_compute, 'Frozen compute conservation mismatch')
        require(b['wait_ps'] == 0 and a['wait_ps'] == cumulative_wait, 'HBF wait conservation mismatch')
        residual = a['relative_ps']-b['relative_ps']-cumulative_wait
        interval2, interval3 = b['relative_ps']-previous2, a['relative_ps']-previous3
        require(residual == 0 and interval2 == compute and interval3-interval2 == wait,
                'Integer interval conservation mismatch')
        paired_events.append(dict(zip(PAIRED_FIELDS, [
            a['output_index'] if a['type'] == 'token_emitted' else None,
            a['type'], owner, a['token_id'], b['relative_ps'], a['relative_ps'],
            compute, wait, cumulative_wait, interval2, interval3, residual,
        ])))
        previous2, previous3 = b['relative_ps'], a['relative_ps']
    outputs = [r for r in paired_events if r['type'] == 'token_emitted']
    m3 = metrics(origin3, [t['tick_ps'] for t in t3 if t['type'] == 'token_emitted'])
    m2 = metrics(origin2, [t['tick_ps'] for t in t2 if t['type'] == 'token_emitted'])
    require(read(g2/'metrics.json') == m2, 'Stored G2 metrics differ from actual emitted-token metrics')
    numerator = denominator = None
    if len(outputs) > 1:
        first, last = outputs[0], outputs[-1]
        numerator = ((last['G3_relative_ps']-first['G3_relative_ps'])
                     -(last['G2_relative_ps']-first['G2_relative_ps']))
        denominator = len(outputs)-1
        require(numerator == sum(r['step_wait_ps'] for r in outputs[1:]), 'TPOT integer difference mismatch')
    result = {
        'status': 'pass', 'metrics_G2': m2, 'metrics_G3': m3,
        'paired_tokens': outputs, 'paired_events': paired_events,
        'eos_ready': paired_events[-1] if paired_events and paired_events[-1]['type'] == 'EOS' else None,
        'tpot_difference_numerator_ps': numerator, 'tpot_difference_denominator': denominator,
        'all_integer_residuals_ps': [r['conservation_residual_ps'] for r in paired_events],
        'scope': 'Offline audit of the complete supplied frozen trajectory; not qualification of a new GPU/EOS trace.',
    }
    with (folder/'G2-G3-paired.csv').open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fieldnames=PAIRED_FIELDS)
        writer.writeheader()
        writer.writerows(outputs)
    write(folder/'paired-audit.json', result)
    return result
