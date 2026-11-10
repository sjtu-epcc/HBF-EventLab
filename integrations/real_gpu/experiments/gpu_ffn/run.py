"""F1/F2/F3 full-FFN HBF runs with a shared native reference and bounded auditing."""
import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from platform_environment import runtime_environment, executable, cache_value
from integrated_build_identity import verify_build
sys.path.insert(0, str(Path(__file__).resolve().parent))
from ffn_identity import verify as verify_experiment

STAGE_LAYERS = {'F1': (10,), 'F2': (0, 10, 20, 27), 'F3': tuple(range(28))}


def select_regions(layout, stage):
    require(stage in STAGE_LAYERS, 'Unsupported experiment stage')
    regions = [r for r in layout if r['layer'] in STAGE_LAYERS[stage]]
    require(len(regions) == 3*len(STAGE_LAYERS[stage]), 'Incomplete stage matrix set')
    require(sum(r['valid_bytes'] for r in regions) == len(STAGE_LAYERS[stage])*18*1024**2, 'Stage byte count mismatch')
    return regions


def output_stage(output):
    plan = output/'plan.json'
    return read(plan)['stage'] if plan.is_file() else 'F1'


def require(ok, message):
    if not ok:
        raise ValueError(message)


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def write(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def sha(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def rows(path):
    with Path(path).open(encoding='utf-8') as f:
        return [json.loads(line) for line in f if line.strip()]


def make_layout(tensors, layers=28, hidden=1024, ffn=3072):
    """All stages use stable addresses in one GGUF-derived full-FFN layout."""
    result = []
    address = 0
    for layer in range(layers):
        for kind in ('gate', 'up', 'down'):
            name = f'blk.{layer}.ffn_{kind}.weight'
            tensor = tensors[name]
            expected = [ffn, hidden] if kind == 'down' else [hidden, ffn]
            require(tensor['shape'] == expected and tensor['dtype'] == 'F16', 'Unsupported FFN tensor: ' + name)
            size = hidden * ffn * 2
            require(tensor['bytes'] == size and size % 4096 == 0, 'FFN byte count/alignment mismatch: ' + name)
            result.append({'region_id': name, 'tensor': name, 'source_offset': 0,
                           'source_bytes': size, 'valid_bytes': size, 'registered_bytes': size,
                           'global_base': address, 'content_id': 1, 'layer': layer, 'kind': kind})
            address += size
    require(address <= 512 * 1024**2, 'FFN layout exceeds configured capacity')
    return result


def model_layout(model):
    sys.path.insert(0, str(ROOT / 'third_party/llama.cpp-integrated/gguf-py'))
    from gguf import GGUFReader
    reader = GGUFReader(str(model))
    field = lambda name: reader.get_field(name).contents()
    require(field('general.architecture') == 'qwen3', 'Expected Qwen3')
    require([field('qwen3.' + k) for k in ('block_count', 'embedding_length', 'feed_forward_length')] == [28, 1024, 3072], 'Expected Qwen3-0.6B dimensions')
    tensors = {t.name: {'shape': [int(n) for n in t.shape], 'bytes': int(t.n_bytes), 'dtype': t.tensor_type.name} for t in reader.tensors}
    return make_layout(tensors)


def expected_counts(regions, steps):
    size = sum(r['valid_bytes'] for r in regions)
    return {'registered_valid_bytes': size, 'parent_reads': len(regions)*steps,
            'cumulative_read_bytes': size*steps, 'read_globals': size*steps//4096,
            'read_fragments': size*steps//64, 'write_fragments': size//64}


def union_ns(intervals):
    intervals = sorted(intervals)
    if not intervals:
        return 0
    start, end = intervals[0]
    total = 0
    for lo, hi in intervals[1:]:
        require(hi > lo, 'Invalid GPU interval')
        if lo > end:
            total += end - start
            start, end = lo, hi
        else:
            end = max(end, hi)
    return total + end - start


def capture_table(folder, header):
    activities = [a for name in ('kernels.jsonl', 'transfers.jsonl') for a in rows(folder/name)]
    by_id = {a['activity_id']: a for a in activities}
    require(len(by_id) == len(activities), 'Duplicate activity IDs')
    segments = []
    owned = set()
    previous_gpu_end = 0
    for i, window in enumerate(rows(folder/'segments.jsonl')):
        require(window['segment_id'] == i+1 and window['kind'] in ('gap', 'node', 'tail'), 'Bad capture window order/kind')
        ids = range(window['first_activity_id'], window['first_activity_id']+window['activity_count'])
        require(not owned.intersection(ids), 'Overlapping capture ownership')
        members = [by_id[n] for n in ids]
        require(all(a['step'] == window['step'] and a['end_ns'] > a['start_ns'] for a in members), 'GPU activity step/interval mismatch')
        if members:
            require(min(a['start_ns'] for a in members) >= previous_gpu_end, 'GPU activity overlaps across synchronized windows')
            previous_gpu_end = max(a['end_ns'] for a in members)
        owned.update(ids)
        segments.append({'segment_id': i+1, 'kind': window['kind'], 'node_id': window['node_id'],
                         'step': window['step'], 'duration_ps': 1000*union_ns([(a['start_ns'], a['end_ns']) for a in members])})
    require({a['activity_id'] for a in activities if a['step'] >= 0} == owned, 'Request GPU activity omitted from table')
    require(segments and any(s['duration_ps'] for s in segments), 'Empty measured GPU work')
    return {'header': dict(header, frozen_G1=True, unit='ps',
                           semantics='Single native sample; union of CUPTI kernel/memcpy/memset activity per synchronized window; excludes host gaps'),
            'segments': segments}


def normalize(value):
    if isinstance(value, dict):
        return {k: normalize(v) for k, v in value.items() if k not in ('timestamp_ns', 'address')}
    if isinstance(value, list):
        return [normalize(v) for v in value]
    return value


def metrics(times):
    return {'actual_outputs': len(times), 'ttft_ps': times[0] if times else None,
            'tpot_ps': (times[-1]-times[0])/(len(times)-1) if len(times)>1 else None,
            'itl_ps': [b-a for a, b in zip(times, times[1:])]}


def audit(output, regions, table):
    import numpy as np
    stage = output_stage(output)
    require(stage in STAGE_LAYERS, 'Unsupported saved stage')
    require(regions == select_regions(read(output/'layout-all-ffn.json'), stage), 'Saved stage selection mismatch')
    native, joint = output/'native', output/stage
    a, b, session = read(native/'summary.json'), read(joint/'summary.json'), read(joint/'session.json')
    for key in ('tokens', 'raw_tokens', 'stop_reason', 'positions', 'prompt_ids', 'nodes', 'callbacks'):
        require(a[key] == b[key], 'Native/'+stage+' mismatch: ' + key)
    require(a['status'] == b['status'] == 'completed' and a['cupti_dropped'] == b['cupti_dropped'] == 0, 'GPU execution/recording failed')
    require(a['native_kv'] and b['native_kv'], 'KV must stay native')
    values = np.fromfile(native/'logits.f32', dtype='<f4')
    require(values.size == a['vocab_size']*len(a['raw_tokens']) and np.isfinite(values).all(), 'Bad native logits')
    require((native/'logits.f32').read_bytes() == (joint/'logits.f32').read_bytes(), 'Full logits differ')
    native_events, joint_events = rows(native/'events.jsonl'), rows(joint/'events.jsonl')
    require(normalize(native_events) == normalize(joint_events), 'Consumer sequence differs')
    require(all(session[k] for k in ('workload_success', 'input_closed', 'host_drained', 'device_drained')), 'HBF did not succeed/drain')
    require(session['status'] == 'Completed' and not session['dependency_failed'], 'HBF dependency failed')
    require(session['capacity_bytes'] == 512*1024**2, 'Expected 512 MiB backend capacity')
    steps = len(a['raw_tokens'])
    require(steps > 0, 'No inference steps')
    expected = expected_counts(regions, steps)
    manifest = {r['tensor']: r for r in regions}
    consumers = []
    for event in native_events:
        if event['type'] != 'node_before':
            continue
        for cb in event['weight_accesses']:
            if cb['source']['name'] in manifest:
                r = manifest[cb['source']['name']]
                require(event['op'] == 'MUL_MAT' and cb['source_offset'] == 0 and cb['source_bytes'] == r['valid_bytes'], 'Wrong native FFN consumer')
                consumers.append((event, cb, r))
    counts = Counter((e['step'], r['tensor']) for e, _, r in consumers)
    require(counts == Counter({(step, r['tensor']): 1 for step in range(steps) for r in regions}), 'Missing/extra native FFN consumers')
    gates = rows(joint/'gates.jsonl')
    require(len(consumers) == len(gates) == expected['parent_reads'], 'Parent read count mismatch')
    windows = rows(joint/'segments.jsonl')
    require(len(windows) == len(table['segments']), 'Compute table length mismatch')
    before = {e['node_id']: e for e in joint_events if e['type'] == 'node_before'}
    after = {e['node_id']: e for e in joint_events if e['type'] == 'node_after'}
    node_windows = {w['node_id']: w for w in windows if w['kind'] == 'node'}
    gap_windows = {w['node_id']: w for w in windows if w['kind'] == 'gap'}
    kernels = {k['activity_id']: k for k in rows(joint/'kernels.jsonl')}
    apis = {api['correlation']: api for api in rows(joint/'apis.jsonl')}
    for w, s in zip(windows, table['segments']):
        require(all(w[k] == s[k] for k in ('segment_id', 'kind', 'node_id', 'step', 'duration_ps')), 'Compute table mismatch')
    for (event, cb, r), gate in zip(consumers, gates):
        require(gate['node_id'] == event['node_id'] and gate['callback_id'] == cb['callback_id'] and gate['step'] == event['step'], 'Gate ownership mismatch')
        require(gate['tensor'] == r['tensor'] and gate['global_address'] == r['global_base'] and gate['bytes'] == r['valid_bytes'], 'Gate interval mismatch')
        require(gate['success'] and gate['complete'] and gate['registered_globals'] == gate['finished_globals'] == r['valid_bytes']//4096, 'Incomplete gate')
        node = gate['node_id']
        require(gap_windows[node]['end_ns'] <= before[node]['timestamp_ns'] <= gate['before_ns'] <= gate['after_ns'] <= node_windows[node]['start_ns'] < node_windows[node]['end_ns'] <= after[node]['timestamp_ns'], 'Dependency dispatch order mismatch')
        window = node_windows[node]
        consumers_gpu = [kernels[n] for n in range(window['first_activity_id'], window['first_activity_id']+window['activity_count']) if n in kernels]
        require(consumers_gpu, 'Selected FFN consumer has no GPU kernel')
        require(all(apis[k['correlation']]['start_ns'] >= gate['after_ns'] for k in consumers_gpu), 'GPU consumer launch precedes dependency success')
    require(len(session['initialization']) == len(regions), 'Initialization count mismatch')
    for init, r in zip(session['initialization'], regions):
        require(init['tensor'] == r['tensor'] and init['bytes'] == r['registered_bytes'] and init['global_address'] == r['global_base'], 'Wrong initialization region')
        require(init['success'] and init['complete'] and init['registered_globals'] == init['finished_globals'] == r['registered_bytes']//4096 and init['completed_ps'] <= session['request_t0_ps'], 'Incomplete initialization')
    online = session['online_audit']
    require(online['read_fragments'] == expected['read_fragments'] and online['write_fragments'] == expected['write_fragments'], 'Online fragment totals mismatch')
    require(not online['logical_failures'] and not session['host_logical_failed'], 'Logical failures')
    require(session['host_logical_success'] == expected['read_fragments']+expected['write_fragments'], 'Host outcome count mismatch')
    require(online['peak_live_fragments'] <= 256 and online['peak_live_attempts'] <= 256, 'Active audit budget exceeded')
    compute, waits = defaultdict(int), defaultdict(int)
    for s in table['segments']:
        compute[s['step']] += s['duration_ps']
    for gate in gates:
        waits[gate['step']] += gate['completed_ps']-gate['offered_ps']
    paired = []
    c = w = 0
    tokens = rows(joint/'tokens.jsonl')
    require(len(tokens) == steps, 'Token/EOS record count mismatch')
    # Reconstruct the complete ordered model clock, independently of token totals.
    timeline = [(s['start_ns'], 0, s['advance']) for s in windows]
    timeline += [(g['before_ns'], 1, g) for g in gates]
    tick = session['request_t0_ps']
    for _, _, record in sorted(timeline, key=lambda x: (x[0], x[1])):
        require(record['offered_ps'] == tick and record['completed_ps'] >= tick, 'Model clock discontinuity')
        tick = record['completed_ps']
    for step, token in enumerate(tokens):
        c += compute[step]
        w += waits[step]
        require(token['token_id'] == a['raw_tokens'][step], 'Token ID mismatch')
        require(token['type'] == ('EOS' if step == steps-1 and a['stop_reason'] == 'EOS' else 'token_emitted'), 'Wrong token/EOS semantics')
        require(token['relative_ps'] == c+w and token['compute_ps'] == c and token['wait_ps'] == w and token['tick_ps'] == session['request_t0_ps']+c+w, 'Token time conservation failed')
        paired.append({'step': step, 'token_id': token['token_id'], 'type': token['type'],
                       'G2_relative_ps': c, 'G3_relative_ps': c+w, 'step_wait_ps': waits[step],
                       'cumulative_wait_ps': w, 'conservation_residual_ps': token['relative_ps']-c-w})
    require(session['compute_ps'] == c and session['accumulated_wait_ps'] == w and session['finished_ps'] == tick == session['request_t0_ps']+c+w, 'Final clock mismatch')
    delivered = [p for p in paired if p['type'] == 'token_emitted']
    require(len(delivered) == len(a['tokens']), 'Useful output count mismatch')
    with (output/'paired-tokens.csv').open('w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=list(paired[0]))
        writer.writeheader()
        writer.writerows(paired)
    return {'stage': stage, 'status': 'pass', 'layers': list(STAGE_LAYERS[stage]),
            'scope': 'layers '+','.join(map(str, STAGE_LAYERS[stage]))+' full FFN gate/up/down; native GPU KV; symbolic synchronous HBF',
            'coverage': expected, 'native_logits_bitwise_equal': True, 'native_tokens_equal': True,
            'G2': metrics([p['G2_relative_ps'] for p in delivered]), 'G3': metrics([p['G3_relative_ps'] for p in delivered]),
            'total_wait_ps': w, 'compute_ps': c, 'all_conservation_residuals_zero': True,
            'online_audit': online, 'events_processed': session['events_processed'], 'closed_and_drained': True,
            'measured_software_phases_seconds': {
                'backend_initialization': session['initialization_wall_ns']/1e9,
                'backend_read_calls_including_online_audit': sum(g['after_ns']-g['before_ns'] for g in gates)/1e9,
                'backend_finish': session['finish_wall_ns']/1e9},
            'limitations': 'One native GPU activity trace; no hardware calibration, full weight offload, wall-clock slowdown or population timing claim.'}


def execute(binary, model, prompt, folder, count, config, build, cuda_root, timeout):
    cfg = folder.parent/(folder.name+'-config.json')
    write(cfg, config)
    argv = [str(binary), str(model), str(prompt), str(folder), str(count), 'timing', str(cfg)]
    began = time.perf_counter()
    # Poll the native Windows working set; snapshots need no third-party package.
    peak_rss = 0
    with (folder.parent/(folder.name+'.stdout.log')).open('w') as stdout, (folder.parent/(folder.name+'.stderr.log')).open('w') as stderr:
        process = subprocess.Popen(argv, stdout=stdout, stderr=stderr, env=runtime_environment(build, ROOT, cuda_root))
        try:
            while process.poll() is None:
                if os.name == 'nt':
                    import ctypes
                    from ctypes import wintypes
                    class Counters(ctypes.Structure):
                        _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD)] + [(name, ctypes.c_size_t) for name in ('PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage', 'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage', 'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage')]
                    counters = Counters()
                    counters.cb = ctypes.sizeof(counters)
                    if ctypes.windll.psapi.GetProcessMemoryInfo(wintypes.HANDLE(int(process._handle)), ctypes.byref(counters), counters.cb):
                        peak_rss = max(peak_rss, counters.PeakWorkingSetSize)
                if time.perf_counter()-began > timeout:
                    raise TimeoutError('Process exceeded configured wall-time budget')
                time.sleep(0.25)
        except BaseException:
            process.kill()
            process.wait()
            raise
    info = {'argv': argv, 'returncode': process.returncode, 'process_wall_seconds': time.perf_counter()-began,
            'peak_working_set_bytes': peak_rss if os.name == 'nt' else None,
            'wall_scope': 'Whole process including load, initialization, inference, CUPTI and recording; never simulated HBF time.'}
    write(folder.parent/(folder.name+'-command.json'), info)
    require(process.returncode == 0, f'{folder.name} failed with exit {process.returncode}; see stderr and failure records')
    return info


def reuse_reference(reference, output, config, layout, binary, build, tokens):
    """Copy verified native evidence; do not execute another GPU reference."""
    reference = reference.resolve()
    prior = read(reference/'result.json')
    require(prior['status'] == 'pass', 'Reference batch has not passed')
    plan, identity = read(reference/'plan.json'), read(reference/'build-identity.json')
    require(plan['model_sha256'] == config['gguf_sha256'], 'Reference model mismatch')
    require(plan['prompt_sha256'] == sha(output/'prompt.txt'), 'Reference prompt mismatch')
    require(plan['requested_outputs'] == tokens, 'Reference output budget mismatch')
    require(read(reference/'layout-all-ffn.json') == layout, 'Reference weight layout mismatch')
    require(sha(binary) == identity['binary_sha256'], 'GPU executable changed since reference capture')
    require(sha(build/'experiment-identity.json') == identity['experiment_identity_sha256'], 'Experiment/backend build changed since reference capture')
    require(all(sha(build/'bin'/name) == h for name, h in identity['runtime_dlls'].items()), 'Runtime DLL changed since reference capture')
    for name, h in identity['sources'].items():
        path = ROOT/name
        if path.suffix.lower() in ('.cpp', '.cu', '.c', '.h', '.hpp', '.cuh', '.inl'):
            require(sha(path) == h, 'Compiled source changed since reference capture: '+name)
    native_config = read(reference/'native-config.json')
    require(all(native_config[k] == config[k] for k in ('gguf_sha256', 'context', 'batch', 'ubatch', 'fixed_segment_ps')), 'Reference execution settings mismatch')
    require(sha(reference/'H0-512MiB.ini') == sha(output/'H0-512MiB.ini'), 'Reference HBF settings mismatch')
    indexed = {str(Path(r['path'])): r for r in read(reference/'artifact-index.json')['files']}
    files = [reference/'compute-table.json', reference/'native-config.json', reference/'native-command.json']
    files += [p for p in (reference/'native').rglob('*') if p.is_file()]
    for source in files:
        key = str(source.relative_to(reference))
        require(key in indexed and sha(source) == indexed[key]['sha256'], 'Reference artifact changed: '+key)
        target = output/source.relative_to(reference)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    table = read(output/'compute-table.json')
    require(table['header']['model_sha256'] == config['gguf_sha256'] and table['header']['prompt_sha256'] == sha(output/'prompt.txt'), 'Reference table identity mismatch')
    require(table['header']['native_config_sha256'] == sha(output/'native-config.json') and table['header']['native_events_sha256'] == sha(output/'native/events.jsonl'), 'Reference table provenance mismatch')
    require(table['header']['binary_sha256'] == sha(binary), 'Reference table binary mismatch')
    require(capture_table(output/'native', {k: v for k, v in table['header'].items() if k not in ('frozen_G1', 'unit', 'semantics')}) == table, 'Reference table differs from raw GPU capture')
    binding = {'reference': str(reference), 'reference_result_sha256': sha(reference/'result.json'),
               'reference_index_sha256': sha(reference/'artifact-index.json'),
               'compute_table_sha256': sha(output/'compute-table.json'),
               'native_reexecuted': False, 'copied_files_verified': len(files)}
    write(output/'reference-binding.json', binding)
    return table, binding


def run(args):
    model, output, build = args.model.resolve(), args.output.resolve(), args.build_dir.resolve()
    require(model.is_file(), 'GGUF not found: '+str(model))
    require(not output.exists(), 'Use a fresh output directory')
    layout = model_layout(model)
    regions = select_regions(layout, args.stage)
    require(args.stage == 'F1' or args.reference is not None, args.stage+' needs --reference pointing to a passed batch')
    output.mkdir(parents=True)
    args.output_created = True
    write(output/'layout-all-ffn.json', layout)
    write(output/'regions.json', regions)
    prompt = output/'prompt.txt'
    shutil.copyfile(args.prompt, prompt)
    h0 = output/'H0-512MiB.ini'
    h0.write_bytes(Path(__file__).with_name('H0-512MiB.ini').read_bytes())
    config = {'model_path': str(model), 'gguf_sha256': sha(model), 'context': 512, 'batch': 256, 'ubatch': 256,
              'capture_compute': True, 'fixed_segment_ps': 0, 'hbf': {'enabled': False}}
    write(output/'plan.json', {'stage': args.stage, 'layers': list(STAGE_LAYERS[args.stage]), 'status': 'prepared', 'requested_outputs': args.tokens,
                              'model_sha256': config['gguf_sha256'], 'prompt_sha256': sha(prompt),
                              'capacity_bytes': 512*1024**2, 'process_timeout_seconds': args.timeout,
                              'expected_if_no_EOS': expected_counts(regions, args.tokens)})
    if args.prepare_only:
        return
    binary = executable(build/'bin', 'gpu_ffn')
    require(binary.is_file(), 'Build gpu_ffn first: '+str(binary))
    base = cache_value(build, 'BASE_BUILD')
    require(base is not None, 'F build has no verified BASE_BUILD')
    experiment = verify_experiment(build)
    dependency = verify_build(Path(base))
    expected_dlls = {Path(item['path']).name: item['sha256'] for item in dependency['artifacts']
                     if Path(item['path']).suffix.lower() == '.dll'}
    require(all((build/'bin'/name).is_file() and sha(build/'bin'/name) == digest
                for name, digest in expected_dlls.items()), 'F runtime DLLs differ from sealed dependency build')
    sources = [p for d in ('include', 'src', 'adapters', 'cmake', 'third_party', 'experiments/gpu_ffn') for p in (ROOT/d).rglob('*') if p.is_file() and '__pycache__' not in p.parts]
    sources += [ROOT/'experiments/gpu_s/native_s8.cpp']
    write(output/'build-identity.json', {'binary_sha256': sha(binary),
        'backend': dependency['backend'],
        'experiment_identity_sha256': sha(build/'experiment-identity.json'),
        'dependency_identity_sha256': sha(Path(base)/'release-identity.json'),
        'sources': {str(p.relative_to(ROOT)): sha(p) for p in sources},
        'runtime_dlls': {p.name: sha(p) for p in (build/'bin').glob('*.dll')},
        'cmake_cache_sha256': sha(build/'CMakeCache.txt')})
    binding = None
    if args.reference:
        table, binding = reuse_reference(args.reference, output, config, layout, binary, build, args.tokens)
        costs = {}
    else:
        costs = {'native': execute(binary, model, prompt, output/'native', args.tokens, config, build, args.cuda_root, args.timeout)}
        header = {'model_sha256': config['gguf_sha256'], 'prompt_sha256': sha(prompt), 'binary_sha256': sha(binary),
                  'native_config_sha256': sha(output/'native-config.json'), 'native_events_sha256': sha(output/'native/events.jsonl')}
        table = capture_table(output/'native', header)
        write(output/'compute-table.json', table)
    config.update(capture_compute=False, compute_table_path=str(output/'compute-table.json'), expected_compute_header=table['header'])
    config['hbf'] = {'enabled': True, 'effective_config_path': str(h0), 'regions': regions}
    costs[args.stage] = execute(binary, model, prompt, output/args.stage, args.tokens, config, build, args.cuda_root, args.timeout)
    result = audit(output, regions, table)
    result['software_costs'] = costs
    if binding:
        result['reference_binding'] = binding
    verify_experiment(build)
    write(output/'result.json', result)
    write(output/'artifact-index.json', {'files': [{'path': str(p.relative_to(output)), 'bytes': p.stat().st_size, 'sha256': sha(p)} for p in sorted(output.rglob('*')) if p.is_file()]})
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, default=ROOT/'build-ffn')
    parser.add_argument('--cuda-root', type=Path)
    parser.add_argument('--prompt', type=Path, default=Path(__file__).with_name('prompt.txt'))
    parser.add_argument('--tokens', type=int, choices=range(1, 9), default=8)
    parser.add_argument('--timeout', type=float, default=3600)
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--stage', choices=tuple(STAGE_LAYERS), default='F1')
    parser.add_argument('--reference', type=Path, help='Passed batch supplying the native capture and fixed table; required for F2/F3')
    args = parser.parse_args()
    try:
        require(args.timeout > 0, 'Timeout must be positive')
        run(args)
    except Exception as exc:
        if getattr(args, 'output_created', False) and args.output.is_dir():
            write(args.output/'runner-failure.json', {'status': 'failed', 'error': str(exc)})
        raise
