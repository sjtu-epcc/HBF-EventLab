"""Recheck saved FFN evidence without GPU; optionally test corrupted record copies."""
import argparse
import copy
from pathlib import Path
from unittest.mock import patch
import run as ffn


def negative_checks(output, regions, table):
    original_read, original_rows = ffn.read, ffn.rows
    def change_first(data, field, increment):
        data[0][field] += increment
        return data
    def change_fragments(data):
        data['online_audit']['read_fragments'] -= 1
        return data
    cases = [
        ('missing_parent', 'rows', 'gates.jsonl', lambda data: data[:-1]),
        ('wrong_global_address', 'rows', 'gates.jsonl', lambda data: change_first(data, 'global_address', 4096)),
        ('missing_read_fragment', 'read', 'session.json', change_fragments),
        ('wrong_token_clock', 'rows', 'tokens.jsonl', lambda data: change_first(data, 'relative_ps', 1)),
        ('wrong_compute_duration', 'rows', 'segments.jsonl', lambda data: change_first(data, 'duration_ps', 1)),
    ]
    results = []
    for name, method, filename, change in cases:
        original = original_read if method == 'read' else original_rows
        target = (output/ffn.output_stage(output)/filename).resolve()
        def intercept(path):
            data = original(path)
            return change(copy.deepcopy(data)) if Path(path).resolve() == target else data
        reason = None
        with patch.object(ffn, method, side_effect=intercept):
            try:
                ffn.audit(output, regions, table)
            except ValueError as exc:
                reason = str(exc)
        ffn.require(reason is not None, 'Auditor accepted corrupted copy: '+name)
        results.append({'case': name, 'rejected': True, 'reason': reason})
    return {'status': 'pass', 'scope': 'In-memory corrupt copies of saved records; no real backend fault injection', 'cases': results}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--negative-checks', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    regions, table = ffn.read(output/'regions.json'), ffn.read(output/'compute-table.json')
    result = ffn.audit(output, regions, table)
    print('Saved '+result['stage']+' audit:', result['status'])
    if args.negative_checks:
        checks = negative_checks(output, regions, table)
        checks['validator_sha256'] = ffn.sha(Path(__file__))
        ffn.write(output/'negative-checks.json', checks)
        print('Corrupt-record rejection:', len(checks['cases']), 'passed')
