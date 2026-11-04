"""Run the pinned minimum S experiment against the current EventLab backend.

Historical fixed durations remain an explicit input from the upstream GPU. This
run proves local CUDA execution and new-backend coupling, NOT a new local timing
measurement. Inputs are read-only; all results go into a fresh directory.
"""
import argparse
import copy
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from common import read, rows, sha, write
from build_identity import verify, FRONTEND, require
from platform_environment import runtime_environment, executable
from s7 import validate, replay, independent_audit
from token_contracts import pair, table_audit
from s5_audit import positive, negative, normalized
from audit import numerical, stored_kv, boundaries


def run(args):
    build, model, records, output = [p.resolve() for p in (args.build, args.model, args.records, args.output)]
    identity = verify(build)
    base = Path(identity['base_build'])
    require(not output.exists(), 'Output already exists; use a new experiment id')
    require(not output.is_relative_to(records), 'Output may not modify historical records')
    require(model.is_file(), 'Model input missing')
    history = records/'output/gpu-s'
    s6 = history/'20261101-s6-001'
    qualification = history/'20261101-s6-minimum-acceptance-001'
    original_table = qualification/'compute-table.json'
    table = read(original_table)
    require(sha(original_table) == read(qualification/'stage-result.json')['qualified_table_sha256'], 'Table hash mismatch')
    require(sha(model) == table['header']['model_sha256'], 'GGUF differs from the frozen trace; do not rewrite its header to bypass this check')
    profile = read(s6/'config.json')
    require(sha(s6/'config.json') == table['header']['config_sha256'], 'Original profile hash mismatch')
    require(sha(s6/'sampling_protocol.json') == table['header']['protocol_sha256'], 'Protocol hash mismatch')
    sample = s6/table['header']['sample']
    require(sha(sample/'events.jsonl') == table['header']['events_sha256'], 'Source trace hash mismatch')
    require(table['segments'] == read(sample/'measured-segments.json'), 'Frozen durations changed')
    events, windows = rows(sample/'events.jsonl'), rows(sample/'segments.jsonl')
    validate(table, table, windows, events)
    old_g2 = history/'20261101-s7-001/G2'
    old_g2_result = read(old_g2/'stage-result.json')
    require(old_g2_result['status'] == 'pass' and old_g2_result['table_sha256'] == sha(original_table), 'Historical G2 table identity mismatch')
    require(sha(old_g2/'inputs/compute-table.json') == sha(original_table), 'Historical G2 input table differs')
    regions = read(old_g2/'inputs/M2-regions.json')
    old_g3 = history/'20261101-s8-002/G3'
    prompt_source = old_g3/'prompt.txt'
    require(sha(prompt_source) == table['header']['prompt_sha256'], 'Prompt differs from table')
    # This is the published 8-step fixture, not a general workload selector.
    count = len({s['step'] for s in table['segments']})
    require(0 < count <= 8, 'Native S executor supports at most eight steps')
    output.mkdir(parents=True)
    inputs = output/'inputs'; inputs.mkdir()
    for source, name in [(original_table, 'compute-table.json'), (prompt_source, 'prompt.txt'),
                         (old_g3/'effective-H0.ini', 'effective-H0.ini'),
                         (old_g2/'inputs/M2-regions.json', 'M2-regions.json'),
                         (history/'20261101-s2-b2-v2-001/S0/numerical-targets-predeclared.json', 'numerical-contract.json'),
                         (s6/'config.json', 'historical-profile.json'),
                         (qualification/'stage-result.json', 'historical-qualification.json')]:
        shutil.copyfile(source, inputs/name)
    write(output/'experiment-build-identity.json', identity)
    profile.update(model_path=str(model), compute_table_path=str(inputs/'compute-table.json'), expected_compute_header=table['header'])
    profile['hbf'] = {'enabled': False}
    env = runtime_environment(base, FRONTEND)
    env['PATH'] = str(build/'bin')+os.pathsep+env['PATH']
    env.update(GGML_CUDA_CUBLAS_COMPUTE_TYPE='f32', GGML_CUDA_DISABLE_FUSION='1', NVIDIA_TF32_OVERRIDE='0')
    binary = executable(build/'bin', 'gpu_s_s8')
    conditions = {'schema': 1, 'scope': 'new-backend-real-CUDA-fixed-historical-trace-mechanism',
                  'compute_table_sha256': sha(original_table), 'model_sha256': sha(model),
                  'backend_source_sha256': identity['backend_source_sha256'],
                  'local_GPU_compute_durations_measured_for_table': False,
                  'inherited_compute_header': table['header'], 'requested_steps': count,
                  'table_source_note': 'Durations from the published upstream GPU are input assumptions, not measurements of this local GPU.',
                  'precision_limits': read(qualification/'stage-result.json')['precision_qualification'],
                  'weights_remain_on_GPU': True, 'native_KV': True,
                  'HBF_symbolic_selected_weights_only': True, 'H0_hardware_calibrated': False,
                  'environment_controls': {k: env.get(k) for k in ['CUDA_VISIBLE_DEVICES','GGML_CUDA_DISABLE_GRAPHS','GGML_CUDA_CUBLAS_COMPUTE_TYPE','GGML_CUDA_DISABLE_FUSION','NVIDIA_TF32_OVERRIDE']},
                  'inputs': {p.name: sha(p) for p in inputs.iterdir()}}
    write(output/'conditions.json', conditions)

    def execute(name, mode='verify', hbf=True, expected=0, **overrides):
        config = copy.deepcopy(profile)
        config['hbf'] = ({'enabled': True, 'effective_config_path': str(inputs/'effective-H0.ini'), 'regions': regions, **overrides}
                         if hbf else {'enabled': False})
        path = output/'configs'/(name+'.json'); write(path, config)
        folder = output/name
        command = [str(binary), str(model), str(inputs/'prompt.txt'), str(folder), str(count), mode, str(path)]
        logs = output/'logs'/name; logs.mkdir(parents=True)
        before = time.perf_counter()
        with (logs/'stdout.log').open('wb') as stdout, (logs/'stderr.log').open('wb') as stderr:
            process = subprocess.run(command, env=env, stdout=stdout, stderr=stderr, timeout=args.timeout)
        write(logs/'command.json', {'argv': command, 'returncode': process.returncode, 'expected_returncode': expected,
                                  'wall_seconds': time.perf_counter()-before, 'wall_time_used_in_model': False})
        require(process.returncode == expected, 'Unexpected return code for '+name+'; inspect '+str(logs))
        return folder

    # New zero-wait record replay has no CUDA/backend, exactly as in S7.
    replay(table, windows, events, regions, output/'G2')
    independent_audit(output/'G2', table, events, windows, regions)
    require(read(output/'G2/metrics.json') == read(old_g2/'metrics.json'), 'G2 changed')
    for filename in ('tokens.jsonl', 'events.jsonl'):
        require(rows(output/'G2'/filename) == rows(old_g2/filename), 'Historical/new G2 semantic trace differs: '+filename)
    native = execute('native-verify', hbf=False)
    native_segments = table_audit(native, table)
    boundaries(native, profile)
    require(stored_kv(native, profile)['status'] == 'pass', 'Native KV publication failed')
    # Cross-device kernels need not have identical symbols, but fixed logical
    # boundaries and active/empty coverage must pass table_audit unchanged.
    require(normalized(rows(native/'events.jsonl')) == normalized(events), 'Local logical event trace differs from frozen input')
    from reference import CpuQwen
    cpu = output/'cpu-reference'
    ref = CpuQwen(profile); ref.run(read(native/'summary.json')['prompt_ids'], count, cpu); del ref
    contract = read(inputs/'numerical-contract.json')
    native_numeric = numerical(native, cpu, profile, contract)
    require(native_numeric['status'] == 'pass', 'Local native/CPU numerical audit failed')
    print('Local native CUDA, exact KV publication and independent CPU reference passed.', flush=True)
    full = execute('G3-full')
    segments = table_audit(full, table)
    paired = pair(full, output/'G2', table)
    behavior = positive(full, native, regions, profile, segments['compute_by_step_ps'])
    require(stored_kv(full, profile)['status'] == 'pass', 'G3 KV publication failed')
    numeric = numerical(full, cpu, profile, contract)
    require(numeric['status'] == 'pass', 'G3/CPU numerical audit failed')
    results = [{'case': 'G3-full', 'metrics': paired['metrics_G3'], 'wait_ps': read(full/'session.json')['accumulated_wait_ps']}]
    def model_state(value):
        if isinstance(value, dict): return {k: model_state(v) for k,v in value.items() if k not in ('before_ns','after_ns')}
        if isinstance(value, list): return [model_state(v) for v in value]
        return value
    for i in range(1, args.timing_repeats+1):
        timing = execute('G3-timing-'+str(i), mode='timing')
        table_audit(timing, table)
        audit = pair(timing, output/'G2', table)
        require(not rows(timing/'snapshots.jsonl'), 'Timing variant contains verification snapshots')
        require((timing/'logits.f32').read_bytes() == (full/'logits.f32').read_bytes(), 'Timing logits differ')
        require(normalized(rows(timing/'events.jsonl')) == normalized(rows(full/'events.jsonl')), 'Timing semantic trace differs')
        for filename in ('tokens.jsonl', 'statistics.json', 'bridge.jsonl'):
            require((timing/filename).read_bytes() == (full/filename).read_bytes(), 'Nondeterministic model record: '+filename)
        require(model_state(read(timing/'session.json')) == model_state(read(full/'session.json')), 'Session model state differs')
        require(model_state(rows(timing/'gates.jsonl')) == model_state(rows(full/'gates.jsonl')), 'Gate model state differs')
        results.append({'case': timing.name, 'metrics': audit['metrics_G3'], 'wait_ps': read(timing/'session.json')['accumulated_wait_ps']})
    failed_init = execute('initialization-program-fail', persistent_program_fail=True, expected=87)
    negative(failed_init, 'program-fail')
    # Stop inside the first real dependency after its preceding frozen compute.
    first_gate = rows(full/'gates.jsonl')[0]
    limited = execute('consumer-time-limit', max_tick_ps=first_gate['offered_ps']+1, expected=88)
    negative(limited, 'time-limit')
    require(verify(build) == identity, 'Build identity changed during experiment')
    result = {'status': 'pass', 'scope': conditions['scope'], 'backend_source_sha256': identity['backend_source_sha256'],
              'local_environment': read(native/'summary.json')['environment'], 'G2_metrics': read(output/'G2/metrics.json'),
              'runs': results, 'local_native_CPU_arrays': native_numeric['arrays_compared'],
              'local_G3_CPU_arrays': numeric['arrays_compared'], 'local_native_G3_exact': behavior['gpu_native_exact'],
              'coverage': behavior['coverage'], 'negative_dependency_tests': 2, 'closed_and_drained': True,
              'timing_repeats': args.timing_repeats, 'all_integer_residuals_ps': paired['all_integer_residuals_ps'],
              'new_local_compute_measurements': False, 'HBF_hardware_prediction': False}
    write(output/'result.json', result)
    write(output/'artifact-index.json', {'files': [{'path': p.relative_to(output).as_posix(), 'bytes': p.stat().st_size, 'sha256': sha(p)}
          for p in sorted(output.rglob('*')) if p.is_file() and p.name != 'artifact-index.json']})
    print(json.dumps(result, indent=2), flush=True)
    return result


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--records', type=Path, required=True)
    p.add_argument('--model', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--timing-repeats', type=int, default=2)
    p.add_argument('--timeout', type=int, default=3600)
    a = p.parse_args()
    if a.timing_repeats < 1: p.error('At least one independent timing variant is required')
    run(a)
