"""Decompose fixed S8 input into disjoint kernel/transfer/overlap intervals."""
import argparse
import csv
import itertools
from pathlib import Path
from common import read, rows, write, sha
from batches import batch_path
from study import ROOT


def run(study):
    folder = study/'G3'
    assert read(folder/'stage-result.json')['status'] == 'pass'
    table = read(folder/'compute-table.json')
    source = batch_path('s6')/table['header']['sample']
    activities = {x['activity_id']: (x, 0) for x in rows(source/'kernels.jsonl')}
    activities.update({x['activity_id']: (x, 1) for x in rows(source/'transfers.jsonl')})
    step_parts = [[0, 0, 0] for _ in range(8)]
    for s in table['segments']:
        endpoints = []
        for aid in s['activity_ids']:
            a, kind = activities[aid]
            endpoints += [(a['start_ns'], kind, 1), (a['end_ns'], kind, -1)]
        counts = [0, 0]; parts = [0, 0, 0]; previous = None
        for at, group in itertools.groupby(sorted(endpoints), key=lambda x: x[0]):
            if previous is not None and any(counts):
                category = 2 if all(counts) else 0 if counts[0] else 1
                parts[category] += (at-previous)*1000
            for _, kind, change in group:
                counts[kind] += change
            assert min(counts) >= 0
            previous = at
        assert counts == [0, 0] and sum(parts) == s['duration_ps']
        for i in range(3):
            step_parts[s['step']][i] += parts[i]
    with (folder/'G2-G3-paired.csv').open(newline='', encoding='utf-8') as f:
        paired = list(csv.DictReader(f))
    records = []
    for i, (parts, pair) in enumerate(zip(step_parts, paired)):
        assert sum(parts) == int(pair['step_compute_ps'])
        records.append({'step': i, 'phase': 'prefill' if i == 0 else 'decode',
                        'gpu_kernel_only_ps': parts[0], 'gpu_transfer_only_ps': parts[1],
                        'gpu_kernel_transfer_overlap_ps': parts[2],
                        'gpu_active_total_ps': sum(parts), 'HBF_wait_ps': int(pair['step_wait_ps']),
                        'G3_interval_ps': int(pair['G3_interval_ps'])})
        assert records[-1]['G3_interval_ps'] == sum(parts)+records[-1]['HBF_wait_ps']
    with (folder/'G2-G3-decomposition.csv').open('w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=list(records[0])); writer.writeheader(); writer.writerows(records)
    write(folder/'decomposition-audit.json', {'status': 'pass', 'all_segment_union_residuals_ps': 0,
          'categories_disjoint': True, 'transfer_includes_memcpy_and_memset': True,
          'records': records, 'source_table_sha256': sha(folder/'compute-table.json'),
          'source_code_sha256': sha(Path(__file__))})
    (folder/'source-and-binary-snapshot'/Path(__file__).name).write_bytes(Path(__file__).read_bytes())
    print('Fixed input kernel/transfer/overlap decomposition verified; no double counting.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--study', type=Path, required=True)
    run(parser.parse_args().study.resolve())
