#!/usr/bin/env python3
"""Draw the accepted GPU-S fixed-trace result; never infer wall-clock latency.

Usage: python plot.py [--output DIRECTORY]
Dependencies: Python >=3.11 and matplotlib==3.10.7 (normal pip dependencies).
The checked-in evidence is sufficient; no GPU, model, or raw archive is needed.
"""
from __future__ import annotations

import argparse
import csv
from decimal import Decimal
import hashlib
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load_evidence():
    manifest = json.loads((HERE / 'evidence-manifest.json').read_text(encoding='utf-8'))
    for item in manifest['files']:
        path = (HERE / item['path']).resolve()
        require(path.is_relative_to(HERE), 'Evidence path escaped figure directory')
        data = path.read_bytes()
        require(len(data) == item['bytes'], 'Evidence size mismatch: ' + item['path'])
        require(hashlib.sha256(data).hexdigest() == item['sha256'], 'Evidence hash mismatch: ' + item['path'])
    def read(name):
        return json.loads((HERE / 'evidence' / name).read_text(encoding='utf-8'))
    paired, result, conditions = (read(name) for name in ('paired-audit.json', 'result.json', 'conditions.json'))
    require(paired['status'] == result['status'] == 'pass', 'Run must pass before visualization')
    require(result['new_local_compute_measurements'] is False, 'Figure is qualified only for the historical compute table')
    require(conditions['HBF_symbolic_selected_weights_only'] is True, 'Unexpected access scope')
    require(conditions['H0_hardware_calibrated'] is False, 'Unexpected calibration claim')
    require(result['coverage']['registered_valid_bytes'] == 10240, 'Unexpected coverage')
    require(conditions['inherited_compute_header']['environment']['gpu'] == 'NVIDIA GeForce RTX 5070', 'Unexpected table source')
    rows = paired['paired_tokens']
    require(len(rows) == 8 and paired['eos_ready'] is None, 'This figure depicts the accepted eight non-EOS outputs')
    compute = wait = 0
    previous_g2 = previous_g3 = 0
    for i, row in enumerate(rows, 1):
        require(row['output_index'] == i and row['type'] == 'token_emitted', 'Output identity mismatch')
        for name in ('G2_relative_ps', 'G3_relative_ps', 'step_compute_ps', 'step_wait_ps', 'cumulative_wait_ps'):
            require(type(row[name]) is int and row[name] >= 0, 'Timing values must be nonnegative integer ps')
        compute += row['step_compute_ps']
        wait += row['step_wait_ps']
        require(row['G2_relative_ps'] == compute, 'G2 compute conservation failed')
        require(row['G3_relative_ps'] == compute + wait, 'G3 time conservation failed')
        require(row['G3_relative_ps'] - row['G2_relative_ps'] == row['cumulative_wait_ps'] == wait, 'Delta/wait mismatch')
        require(row['conservation_residual_ps'] == 0, 'Nonzero conservation residual')
        require(row['G2_interval_ps'] == row['G2_relative_ps'] - previous_g2, 'G2 interval mismatch')
        require(row['G3_interval_ps'] == row['G3_relative_ps'] - previous_g3, 'G3 interval mismatch')
        previous_g2, previous_g3 = row['G2_relative_ps'], row['G3_relative_ps']
    require(result['G2_metrics'] == paired['metrics_G2'], 'G2 summary mismatch')
    for run in result['runs']:
        require(run['metrics'] == paired['metrics_G3'] and run['wait_ps'] == wait, 'Full/timing result mismatch')
    require(paired['metrics_G2']['ttft_ps'] == rows[0]['G2_relative_ps'], 'G2 TTFT mismatch')
    require(paired['metrics_G3']['ttft_ps'] == rows[0]['G3_relative_ps'], 'G3 TTFT mismatch')
    require(paired['tpot_difference_numerator_ps'] == wait - rows[0]['step_wait_ps'], 'TPOT numerator mismatch')
    require(paired['tpot_difference_denominator'] == len(rows) - 1, 'TPOT denominator mismatch')
    for name in ('G2', 'G3'):
        require(paired['metrics_' + name]['tpot_ps'] * (len(rows) - 1) == rows[-1][name + '_relative_ps'] - rows[0][name + '_relative_ps'], 'TPOT mismatch')
    return rows, manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=HERE)
    args = parser.parse_args()
    rows, manifest = load_evidence()
    output = args.output.resolve()
    require(not output.is_relative_to(HERE / 'evidence'), 'Cannot write figures into evidence inputs')
    output.mkdir(parents=True, exist_ok=True)

    # Convert integer ps with Decimal for the published table, not rounded pixels.
    csv_path = output / 'token-timing.csv'
    fields = ['output_index', 'token_id', 'G2_relative_ps', 'G3_relative_ps',
              'step_compute_ps', 'step_wait_ps', 'cumulative_wait_ps',
              'G2_ready_ms', 'G3_ready_ms', 'cumulative_delta_us', 'step_wait_us',
              'conservation_residual_ps']
    with csv_path.open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            record = {key: row[key] for key in fields if key in row}
            record.update(G2_ready_ms=str(Decimal(row['G2_relative_ps']) / 10**9),
                          G3_ready_ms=str(Decimal(row['G3_relative_ps']) / 10**9),
                          cumulative_delta_us=str(Decimal(row['cumulative_wait_ps']) / 10**6),
                          step_wait_us=str(Decimal(row['step_wait_ps']) / 10**6))
            writer.writerow(record)

    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.ticker import MultipleLocator

    plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 11,
                         'axes.labelsize': 12, 'axes.titlesize': 13,
                         'axes.titleweight': 'bold', 'axes.titlepad': 16,
                         'axes.spines.top': False, 'axes.spines.right': False,
                         'axes.edgecolor': '#94A3B8', 'axes.labelcolor': '#263449',
                         'xtick.color': '#475569', 'ytick.color': '#475569',
                         'text.color': '#172B45', 'pdf.fonttype': 42,
                         'svg.fonttype': 'none', 'svg.hashsalt': 'gpu-s-20261104',
                         'savefig.facecolor': 'white'})
    fig, (left, right) = plt.subplots(1, 2, figsize=(12.8, 6.05))
    fig.subplots_adjust(left=0.075, right=0.975, bottom=0.235, top=0.75, wspace=0.28)
    fig.text(0.075, 0.94, 'Token timing under a fixed compute trace', fontsize=20, weight='bold')
    fig.text(0.075, 0.891, 'Qwen3-0.6B  |  44 prompt tokens  |  8 output tokens  |  10 KiB selected HBF region',
             fontsize=11, color='#52657B')
    xs = [row['output_index'] for row in rows]
    g2 = [row['G2_relative_ps'] / 10**9 for row in rows]
    g3 = [row['G3_relative_ps'] / 10**9 for row in rows]
    delta = [row['cumulative_wait_ps'] / 10**6 for row in rows]
    blue, orange, teal = '#2563A6', '#D47722', '#087F8C'
    for ax in (left, right):
        ax.set_xlim(0.65, 8.35)
        ax.set_xticks(xs)
        ax.set_xlabel('Output token index', labelpad=9)
        ax.grid(axis='y', color='#E4EAF0', linewidth=0.8)
        ax.set_axisbelow(True)
        ax.tick_params(length=3)

    left.set_title('(a) Modeled token-ready time', loc='left')
    left.plot(xs, g2, '-o', color=blue, lw=2.2, ms=7.8, mfc='white', mew=1.6,
              label='G2: no added HBF wait', zorder=3)
    left.plot(xs, g3, '--x', color=orange, lw=1.8, ms=5.8, mew=1.5,
              label='G3: current HBF backend', zorder=4)
    left.set_ylim(0, 52)
    left.yaxis.set_major_locator(MultipleLocator(10))
    left.set_ylabel('Time since request start (ms)', labelpad=10)
    left.legend(loc='upper left', frameon=False, fontsize=10, handlelength=2.5)
    left.text(0.05, 0.035, 'Curves nearly overlap at this scale.\nThe difference is isolated in (b).',
              transform=left.transAxes, fontsize=10, color='#52657B',
              bbox={'facecolor': 'white', 'edgecolor': 'none', 'alpha': 0.92, 'pad': 3})

    right.set_title('(b) Cumulative HBF increment', loc='left')
    right.fill_between(xs, delta, 0, color=teal, alpha=0.075, linewidth=0)
    right.plot(xs, delta, '-o', color=teal, lw=2.2, ms=6.5, zorder=3)
    right.set_ylim(0, 2.85)
    right.yaxis.set_major_locator(MultipleLocator(0.5))
    right.set_ylabel('G3 − G2 token-ready time (µs)', labelpad=10)
    right.annotate(f'{delta[0]:.6f} µs', (xs[0], delta[0]), xytext=(12, 13),
                   textcoords='offset points', fontsize=10, weight='bold', color=teal)
    right.annotate(f'{delta[-1]:.6f} µs', (xs[-1], delta[-1]), xytext=(-8, 14),
                   textcoords='offset points', ha='right', fontsize=10, weight='bold', color=teal)
    interval_wait = [row['step_wait_ps'] for row in rows[1:]]
    require(len(set(interval_wait)) == 1, 'Annotation requires uniform decode wait')
    right.text(0.055, 0.83, f'After token 1:\n+{interval_wait[0] / 10**6:.6f} µs per token',
               transform=right.transAxes, fontsize=10.5, linespacing=1.5,
               bbox={'facecolor': 'white', 'edgecolor': '#DCE7EA', 'boxstyle': 'round,pad=0.55'})

    fig.text(0.075, 0.126, 'MODELED RESULTS — not measured device latency', fontsize=10.5, weight='bold', color='#36485F')
    fig.text(0.075, 0.087, 'Both cases use the same historical RTX 5070 compute table; G3 was executed on an RTX 4070 Laptop GPU.',
             fontsize=9.5, color='#52657B')
    fig.text(0.075, 0.051, 'Selected symbolic dependencies only; weights remain on GPU. H0 is uncalibrated. Lines connect token observations.',
             fontsize=9.5, color='#52657B')

    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()
    labels = list(fig.texts)
    for ax in (left, right):
        labels.extend([ax.xaxis.label, ax.yaxis.label, ax._left_title])
        labels.extend(ax.texts)
    for label in labels:
        bounds = label.get_window_extent(renderer)
        require(fig.bbox.contains(bounds.x0, bounds.y0) and fig.bbox.contains(bounds.x1, bounds.y1),
                'Figure text would be clipped: ' + label.get_text())
    note_bounds = left.texts[0].get_window_extent(renderer).expanded(1.05, 1.2)
    require(all(not note_bounds.contains(*left.transData.transform((x, y)))
                for x, y in zip(xs, g2)), 'Left annotation obscures data')

    for extension in ('png', 'svg', 'pdf'):
        metadata = {'Creator': 'HBF-EventLab GPU-S reproducible figure'}
        if extension == 'png':
            metadata['Software'] = metadata.pop('Creator')
        elif extension == 'svg':
            metadata['Date'] = None
        else:
            metadata.update(CreationDate=None, ModDate=None)
        path = output / ('token-timing.' + extension)
        fig.savefig(path, dpi=240, metadata=metadata)
        if extension == 'svg':
            # Matplotlib emits trailing spaces inside multi-line path attributes.
            # Removing those spaces preserves XML whitespace separators and geometry.
            text = '\n'.join(line.rstrip() for line in path.read_text(encoding='utf-8').splitlines()) + '\n'
            path.write_text(text, encoding='utf-8', newline='\n')
    plt.close(fig)
    check = {'status': 'pass', 'figure': 'fixed-trace modeled timing, not measured latency',
             'tokens': len(rows), 'integer_residuals_ps': [row['conservation_residual_ps'] for row in rows],
             'ttft_increment_ps': rows[0]['step_wait_ps'],
             'mean_tpot_increment_ps': str(Decimal(sum(interval_wait)) / len(interval_wait)),
             'final_cumulative_increment_ps': rows[-1]['cumulative_wait_ps'],
             'input_manifest_sha256': hashlib.sha256((HERE / 'evidence-manifest.json').read_bytes()).hexdigest(),
             'python_version': sys.version.split()[0], 'matplotlib_version': matplotlib.__version__}
    (output / 'figure-check.json').write_text(json.dumps(check, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(check, indent=2))


if __name__ == '__main__':
    main()
