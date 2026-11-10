"""S9: source-bound CSV tables and standalone scientific figures for S7/S8."""
import argparse
import csv
import shutil
import statistics
from collections import defaultdict
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
from common import read, rows, write, sha

ROOT = Path(__file__).resolve().parents[2]
BLUE, ORANGE, GREEN = '#2674ac', '#d97920', '#468659'


def csv_write(path, records):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=list(records[0]))
        writer.writeheader(); writer.writerows(records)


def csv_read(path):
    with path.open(newline='', encoding='utf-8') as f:
        return list(csv.DictReader(f))


def plot(folder):
    plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 10,
                         'svg.fonttype': 'none', 'axes.spines.top': False, 'axes.spines.right': False})
    data = folder/'tables'; figures = folder/'figures'; figures.mkdir(exist_ok=True)
    saved = []
    def save(fig, name):
        fig.text(.5, .012, 'Fixed-trace mechanism experiment; uncalibrated H0. Short-interval precision limits apply.',
                 ha='center', fontsize=9, color='#555555')
        fig.tight_layout(rect=(0, .05, 1, .94))
        for ext in ['png', 'svg']:
            path = figures/(name+'.'+ext); fig.savefig(path, dpi=180); saved.append(path.name)
        plt.close(fig)

    m = csv_read(data/'figure1-metrics.csv'); reference = csv_read(data/'native-reference.csv')
    fig, ax = plt.subplots(1, 3, figsize=(14, 4.9))
    x = np.arange(2); width = .34
    for j, (name, color) in enumerate([('G2', BLUE), ('G3', ORANGE)]):
        bars = ax[0].bar(x+(j-.5)*width, [float(r[name+'_ps'])/1e9 for r in m], width, label=name, color=color)
        ax[0].bar_label(bars, fmt='%.6f', padding=3, fontsize=8)
    ax[0].set(xticks=x, xticklabels=['TTFT', 'Mean TPOT'], ylabel='Simulated time (ms)', ylim=(0, 12),
              title='(a) Same-table G2 / G3')
    ax[0].legend()
    bars = ax[1].bar(x, [float(r['difference_ps'])/1e6 for r in m], color=GREEN, width=.55)
    ax[1].bar_label(bars, fmt='%.6f', padding=3)
    ax[1].set(xticks=x, xticklabels=['TTFT', 'Mean TPOT'], ylabel='Added modeled wait (µs)',
              ylim=(0, .62), title='(b) G3 − G2 (model only)')
    values = [[float(r[k])/1e6 for r in reference] for k in ['ttft_ns', 'tpot_ns']]
    med = [statistics.median(v) for v in values]
    errors = np.array([[v-min(a) for v, a in zip(med, values)], [max(a)-v for v, a in zip(med, values)]])
    bars = ax[2].bar(x, med, color='#797d88', width=.55, yerr=errors, capsize=4)
    ax[2].bar_label(bars, fmt='%.3f', padding=5)
    ax[2].set(xticks=x, xticklabels=['TTFT', 'Mean TPOT'], ylabel='Measured host time (ms)',
              ylim=(0, max(max(v) for v in values)*1.3), title='(c) Low-instrument native reference')
    ax[2].text(.5, .87, '10 requests; median and min–max\nSeparate clock / composition', transform=ax[2].transAxes,
               ha='center', fontsize=9)
    fig.suptitle('Token latency: fixed model comparison and separate native measurement', fontsize=13)
    save(fig, 'figure1-ttft-tpot')

    itl = csv_read(data/'figure2-itl.csv'); x = [int(r['output_index']) for r in itl]
    fig, ax = plt.subplots(2, 1, figsize=(9, 6.2), sharex=True, gridspec_kw={'height_ratios': [2, 1]})
    ax[0].plot(x, [float(r['G2_interval_ps'])/1e9 for r in itl], '-o', color=BLUE, label='G2')
    ax[0].plot(x, [float(r['G3_interval_ps'])/1e9 for r in itl], '--x', color=ORANGE, label='G3')
    ax[0].set(ylabel='Simulated ITL (ms)', ylim=(0, 7), title='Same-table token intervals (curves nearly coincide)')
    ax[0].legend(); ax[0].grid(axis='y', alpha=.2)
    ax[1].bar(x, [float(r['step_wait_ps'])/1e6 for r in itl], color=GREEN, width=.5)
    ax[1].set(xlabel='Output token index', ylabel='Added wait (µs)', ylim=(0, .35), xticks=x)
    fig.suptitle('ITL for output tokens 2–8; no tail-percentile claim', fontsize=13)
    save(fig, 'figure2-token-itl')

    phases = csv_read(data/'figure3-phases.csv'); x = np.arange(len(phases))
    fig, ax = plt.subplots(1, 2, figsize=(11, 5))
    bottom = np.zeros(len(phases))
    for key, label, color in [('gpu_kernel_only_ps', 'GPU kernels', BLUE),
                               ('gpu_transfer_only_ps', 'GPU transfers / memset', '#8eb7d3'),
                               ('gpu_kernel_transfer_overlap_ps', 'Kernel/transfer overlap (once)', '#aaa'),
                               ('HBF_wait_ps', 'HBF wait', ORANGE)]:
        val = np.array([float(r[key])/1e9 for r in phases])
        ax[0].bar(x, val, bottom=bottom, color=color, label=label, width=.55); bottom += val
    ax[0].set(xticks=x, xticklabels=['Prefill → token 1', 'Decode → tokens 2–8'],
              ylabel='Simulated phase time (ms)', ylim=(0, 43), title='(a) Complete phase totals')
    ax[0].legend(fontsize=8)
    bars = ax[1].bar(x, [float(r['HBF_wait_ps'])/1e6 for r in phases], color=ORANGE, width=.55)
    ax[1].bar_label(bars, fmt='%.6f', padding=3)
    ax[1].set(xticks=x, xticklabels=['Prefill → token 1', 'Decode → tokens 2–8'],
              ylabel='Modeled HBF wait (µs)', ylim=(0, 2.5), title='(b) Wait shown separately for visibility')
    fig.suptitle('G3 compute, necessary GPU transfers and HBF waiting', fontsize=13)
    save(fig, 'figure3-prefill-decode')
    return saved


def run(study):
    from batches import BATCHES, MANIFEST, batch_path
    s7 = batch_path('s7'); s8 = batch_path('s8')
    s6 = batch_path('s6'); qualified = batch_path('s6-qualified')
    g2, g3 = s7/'G2', s8/'G3'; a, b = read(g2/'stage-result.json'), read(g3/'stage-result.json')
    assert a['status'] == b['status'] == 'pass'
    assert a['table_sha256'] == b['table_sha256'] == sha(qualified/'compute-table.json')
    assert not study.exists(); study.mkdir(parents=True); tables = study/'tables'; tables.mkdir()
    inputs = study/'inputs'; inputs.mkdir(); sources = []; snapshots = {}
    def source(p):
        p = Path(p)
        rel = p.relative_to(ROOT) if p.is_relative_to(ROOT) else Path('external')/sha(p)/p.name
        dest = inputs/rel; dest.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(p, dest)
        record = {'path': p.relative_to(ROOT).as_posix() if p.is_relative_to(ROOT) else str(p), 'sha256': sha(p), 'bytes': p.stat().st_size,
                  'snapshot': dest.relative_to(study).as_posix()}; sources.append(record)
        snapshots[p] = record['snapshot']
        return p
    essential = [g2/'stage-result.json', g2/'metrics.json', g2/'tokens.jsonl',
        g3/'stage-result.json', g3/'conditions.json', g3/'G2-G3-paired.csv', g3/'G2-G3-decomposition.csv',
        g3/'decomposition-audit.json', g3/'compute-table.json', g3/'effective-H0.ini',
        g3/'prompt.txt', g3/'G3-full/tokens.jsonl', g3/'G3-full/gates.jsonl', g3/'G3-full/audit.json',
        qualified/'stage-result.json', s6/'compute-samples.jsonl',
        s6/'sampling_protocol.json', batch_path('s2')/'S0/profile.json']
    for p in essential: source(p)
    def loadcsv(p): source(p); return csv_read(p)
    paired = csv_read(g3/'G2-G3-paired.csv'); decomp = csv_read(g3/'G2-G3-decomposition.csv')
    assert len(paired) == len(decomp) == 8
    for r, d in zip(paired, decomp):
        assert int(r['G3_relative_ps'])-int(r['G2_relative_ps']) == int(r['cumulative_wait_ps'])
        assert int(r['conservation_residual_ps']) == 0
        assert sum(int(d[k]) for k in ['gpu_kernel_only_ps', 'gpu_transfer_only_ps', 'gpu_kernel_transfer_overlap_ps', 'HBF_wait_ps']) == int(d['G3_interval_ps'])
    csv_write(tables/'paired-tokens.csv', paired); csv_write(tables/'step-decomposition.csv', decomp)
    summary = [{'metric': k, 'G2_ps': a['metrics'][k], 'G3_ps': b['runs'][0]['metrics'][k],
                'difference_ps': b['runs'][0]['metrics'][k]-a['metrics'][k]} for k in ['ttft_ps', 'tpot_ps']]
    csv_write(tables/'figure1-metrics.csv', summary)
    csv_write(tables/'figure2-itl.csv', paired[1:])
    phase_records = []
    for phase in ['prefill', 'decode']:
        members = [r for r in decomp if r['phase'] == phase]
        phase_records.append({'phase': phase, **{k: sum(int(r[k]) for r in members) for k in list(members[0])[2:]}})
    csv_write(tables/'figure3-phases.csv', phase_records)
    reference = []
    for s in rows(s6/'compute-samples.jsonl'):
        wall = s['reference_wall']
        reference.append({'sample': s['sample'], 'ttft_ns': wall['ttft_ns'],
            'tpot_ns': sum(wall['itl_ns'])/len(wall['itl_ns']),
            'request_to_last_token_ns': wall['request_to_last_token_ns'],
            'instrumented_request_ns': s['instrumented_wall']['request_to_last_token_ns'],
            'source': (s6/s['name']/'sample.json').as_posix()})
        source(s6/s['name']/'sample.json')
        source(s6/(s['name']+'-reference')/'events.jsonl')
    csv_write(tables/'native-reference.csv', reference)
    csv_write(tables/'coverage.csv', [{'quantity': k, 'value': v} for k, v in b['coverage'].items()])
    checks = []; numeric_summary = []
    for run in ['native-verify', 'G3-full']:
        n = read(source(g3/run/'numerical.json'))
        assert n['status'] == 'pass' and n['failed_arrays'] == 0
        by = defaultdict(list)
        for c in n['checks']:
            checks.append({'run': run, 'step': c.get('step', 'all_outputs'), 'name': c['name'],
                'category': c['category'], 'pass': c['pass'], 'relative_rms': c['relative_rms'],
                'normalized_max': c['normalized_max'], 'max_abs': c['max_abs']})
            by[(c.get('step', 'all_outputs'), c['category'])].append(c)
        for (step, category), cs in by.items():
            limit = n['criteria'][category]
            numeric_summary.append({'run': run, 'step': step, 'category': category, 'arrays': len(cs),
                'measured_max_relative_rms': max(c['relative_rms'] for c in cs),
                'relative_rms_limit': limit['relative_rms_max'],
                'measured_max_normalized_element': max(c['normalized_max'] for c in cs),
                'normalized_element_limit': limit['normalized_max'], 'all_pass': all(c['pass'] for c in cs)})
        source(g3/run/'kv-publication-audit.json')
    csv_write(tables/'numerical-arrays.csv', checks); csv_write(tables/'numerical-by-step.csv', numeric_summary)
    status = []; costs = []
    for run in ['native-verify', 'G3-full', 'G3-timing-1', 'G3-timing-2']:
        su = read(source(g3/run/'summary.json')); cmd = read(source(s8/'logs'/run/'command.json'))
        session = read(source(g3/run/'session.json')) if run != 'native-verify' else None
        status.append({'run': run, 'actual_outputs': len(su['tokens']), 'stop_reason': su['stop_reason'],
            'positions': su['positions'], 'cupti_dropped': su['cupti_dropped'],
            'host_drained': session['host_drained'] if session else 'not_applicable',
            'device_drained': session['device_drained'] if session else 'not_applicable',
            'dependency_failed': session['dependency_failed'] if session else 'not_applicable'})
        costs.append({'run': run, 'phase': 'whole_process', 'seconds': cmd['process_wall_seconds'],
            'measurement': 'measured', 'scope': 'Includes load, initialization, execution, recording and shutdown; never hardware latency.'})
        for phase in ['model_load', 'backend_initialize', 'backend_CPU_wait', 'close_and_drain']:
            costs.append({'run': run, 'phase': phase, 'seconds': None,
                'measurement': 'not_separately_recorded', 'scope': 'Not inferred from process time or simulation ticks.'})
        if session:
            assert session['status'] == 'Completed' and session['host_drained'] and session['device_drained']
            source(g3/run/'paired-audit.json'); source(g3/run/'segment-audit.json')
    csv_write(tables/'status.csv', status); csv_write(tables/'software-costs.csv', costs)
    histories = []
    for label, rel in [
        ('S0', 's2/S0'), ('S1', 's2/S1'),
        ('S2-old', 's2-original/S2'), ('S2-revised', 's2/S2'),
        ('S3-revalidated', 's3/S3'), ('S4', 's4/S4'),
        ('S5', 's5/S5'), ('S6-original', 's6'),
        ('S6-minimum-scope', 's6-qualified'),
        ('S7', 's7/G2'), ('S8-interrupted-audit', 's8-interrupted/G3'),
        ('S8', 's8/G3')]:
        key, _, subdir = rel.partition('/')
        if MANIFEST and key not in BATCHES:
            continue
        path = batch_path(key)/subdir/'stage-result.json'
        if path.exists():
            record = read(source(path))
            histories.append({'stage': label, 'status': record['status'],
                'scope': record.get('acceptance_scope', record.get('execution_scope', 'original_conditions')),
                'source': path.as_posix()})
    csv_write(tables/'stage-history.csv', histories)
    source(batch_path('s2')/'S0/environment.json')
    source(g3/'dependency-build-identity.json')
    write(study/'effective-settings.json', {'GPU': read(g3/'G3-full/summary.json')['environment'],
        'table_header': read(g3/'compute-table.json')['header'], 'M2': read(g2/'inputs/M2-regions.json'),
        'H0_path': next(r['snapshot'] for r in sources if r['path'].endswith('effective-H0.ini')),
        'initialization_simulated_ps': read(g3/'G3-full/session.json')['request_t0_ps'],
        'initialization_note': 'Simulation time only; not an initialization CPU-wall measurement.'})
    source(g2/'inputs/M2-regions.json')
    write(study/'figure-sources.json', {
        'figure1-ttft-tpot': ['tables/figure1-metrics.csv', 'tables/native-reference.csv'],
        'figure2-token-itl': ['tables/figure2-itl.csv'],
        'figure3-prefill-decode': ['tables/figure3-phases.csv'],
        'upstream_pair': (g3/'G2-G3-paired.csv').as_posix(),
        'upstream_decomposition': (g3/'G2-G3-decomposition.csv').as_posix()})
    shutil.copyfile(Path(__file__), study/'s9.py')
    shutil.copyfile(Path(__file__).with_name('common.py'), study/'common.py')
    write(study/'plot-environment.json', {'matplotlib': matplotlib.__version__,
        'numpy': np.__version__, 'backend': 'Agg', 'font': 'DejaVu Sans',
        'rebuild': 'python s9.py --study <artifact-folder> --plot-only'})
    validation_inputs = {
        'g2_tokens': snapshots[g2/'tokens.jsonl'],
        'g3_tokens': snapshots[g3/'G3-full/tokens.jsonl'],
        'g3_gates': snapshots[g3/'G3-full/gates.jsonl'],
        'native_events': {str(sample['sample']): snapshots[s6/(sample['name']+'-reference')/'events.jsonl']
                          for sample in rows(s6/'compute-samples.jsonl')},
    }
    write(study/'provenance.json', {'inputs': sources, 'validation_inputs': validation_inputs, 'plotter_sha256': sha(Path(__file__)),
        'acceptance_scope': b['acceptance_scope'], 'table_sha256': b['table_sha256'],
        'new_GPU_execution': False, 'new_A_series_regression': False,
        'phase_wall_costs_available': False, 'scope_note': 'Existing source-bound analysis only; no new calibration or precision guarantee.'})
    figures = plot(study)
    result = {'stage': 'S9', 'status': 'pass', 'acceptance_scope': b['acceptance_scope'],
        'figure_files': figures, 'table_files': [p.name for p in sorted(tables.iterdir())],
        'matched_table_sha256': b['table_sha256'], 'max_integer_pair_residual_ps': 0,
        'numerical_arrays_per_GPU_path': b['numerical_arrays'], 'all_numerical_checks_pass': True,
        'modeled_HBF_wait_ps': b['runs'][0]['wait_ps'], 'unmeasured_wall_subphases_explicitly_marked': True,
        'S10_started': False}
    write(study/'stage-result.json', result)
    if not MANIFEST: write(ROOT/'docs/validation/gpu-s9.json', result)
    print('S9 generated three PNG/SVG figures and source-bound tables; review report and rendered figures.')


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--study', type=Path, required=True)
    p.add_argument('--plot-only', action='store_true'); a = p.parse_args()
    if a.plot_only: plot(a.study.resolve())
    else: run(a.study.resolve())
