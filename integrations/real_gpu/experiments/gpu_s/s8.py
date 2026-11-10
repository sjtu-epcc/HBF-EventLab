"""S8 real CUDA consumers gated by HBF, with the exact S7 fixed table."""
import argparse
import copy
import csv
import shutil
import subprocess
from pathlib import Path
from common import read, rows, write, sha, metrics
from batches import batch_path, MANIFEST
from study import ROOT, BASE, invoke, environment, JOINT_BUILD
from integrated_build_identity import verify_build
from s5_audit import positive, exact, normalized
from s5_validate import operations
from s8_segment_audit import audit as segment_audit
from audit import numerical, stored_kv
from reference import CpuQwen


def pair(folder, g2, table):
    # Keep the historical s8.pair entry point; CPU-only users can import the
    # contract module without importing this legacy runner's model dependencies.
    from token_contracts import pair as pair_contract
    return pair_contract(folder, g2, table)


def table_audit(folder, table, expected_header=None):
    from token_contracts import table_audit as audit_contract
    return audit_contract(folder, table, expected_header)


def run(study):
    g2 = batch_path('s7')/'G2'
    s6 = batch_path('s6-qualified')
    prior = batch_path('s5')/'S5'
    binary = JOINT_BUILD/'bin/gpu_s_s8.exe'
    assert read(g2/'stage-result.json')['status'] == 'pass'
    tablefile = s6/'compute-table.json'; table = read(tablefile)
    assert sha(tablefile) == read(g2/'stage-result.json')['table_sha256']
    assert sha(g2/'inputs/compute-table.json') == sha(tablefile)
    assert table['header']['frozen_G1']
    assert not study.exists(); folder = study/'G3'; folder.mkdir(parents=True)
    write(folder/'dependency-build-identity.json', verify_build(BASE))
    config = read(batch_path('s6')/'config.json')
    assert sha(config['model_path']) == table['header']['model_sha256']
    prompt = folder/'prompt.txt'; prompt.write_bytes((prior/'prompt.txt').read_bytes())
    assert sha(prompt) == table['header']['prompt_sha256']
    tablecopy = folder/'compute-table.json'; shutil.copyfile(tablefile, tablecopy)
    regions = read(g2/'inputs/M2-regions.json')
    ini = folder/'effective-H0.ini'; ini.write_bytes((prior/'effective-H0.ini').read_bytes())
    config.update(compute_table_path=str(tablecopy), expected_compute_header=table['header'])
    write(folder/'conditions.json', {'stage': 'S8', 'acceptance_scope': read(s6/'stage-result.json')['acceptance_scope'],
        'table_sha256': sha(tablecopy), 'S7_result_sha256': sha(g2/'stage-result.json'),
        'binary_sha256': sha(binary), 'prompt_sha256': sha(prompt), 'H0_sha256': sha(ini),
        'runs': ['native-verify', 'G3-full', 'G3-timing-1', 'G3-timing-2'],
        'controls': 'Fixed S7 compute table, native KV, M2/H0. Measured live GPU wall time and snapshot I/O never advance model time.',
        'source_hashes': {p.name: sha(p) for p in Path(__file__).parent.iterdir() if p.is_file()},
        'core_Host_Device_modified': False})
    snap = folder/'source-and-binary-snapshot'; snap.mkdir()
    for p in [p for p in Path(__file__).parent.iterdir() if p.is_file()]+[binary, JOINT_BUILD/'CMakeCache.txt']:
        shutil.copyfile(p, snap/p.name)

    def execute(name, mode, hbf):
        cfg = copy.deepcopy(config)
        cfg['hbf'] = {'enabled': True, 'effective_config_path': str(ini), 'regions': regions} if hbf else {'enabled': False}
        path = folder/'configs'/(name+'.json'); write(path, cfg)
        out = folder/name
        invoke([binary, config['model_path'], prompt, out, 8, mode, path], study/'logs'/name)
        return out

    native = execute('native-verify', 'verify', False)
    table_audit(native, table)
    exact(prior/'native-8', native, config)
    assert stored_kv(native, config)['status'] == 'pass'
    cpu = folder/'cpu-8'
    ref = CpuQwen(config); ref.run(read(native/'summary.json')['prompt_ids'], 8, cpu); del ref
    target = read(batch_path('s2')/'S0/numerical-targets-predeclared.json')
    assert numerical(native, cpu, config, target)['status'] == 'pass'
    expected_ops = operations(batch_path('s6')/table['header']['sample'])
    assert operations(native) == expected_ops
    print('Final table-driven executable: native values/KV/events/CUDA operations match S5/S6; CPU pass.', flush=True)
    results = []
    for name, mode in [('G3-full', 'verify'), ('G3-timing-1', 'timing'), ('G3-timing-2', 'timing')]:
        out = execute(name, mode, True)
        seg = table_audit(out, table)
        assert operations(out) == expected_ops
        paired = pair(out, g2, table)
        if mode == 'verify':
            behavior = positive(out, native, regions, config, seg['compute_by_step_ps'])
            assert stored_kv(out, config)['status'] == 'pass'
            numeric = numerical(out, cpu, config, target); assert numeric['status'] == 'pass'
            full = out
        else:
            assert not rows(out/'snapshots.jsonl')
            assert (out/'logits.f32').read_bytes() == (full/'logits.f32').read_bytes()
            assert normalized(rows(out/'events.jsonl')) == normalized(rows(full/'events.jsonl'))
            for f in ['tokens.jsonl', 'statistics.json', 'bridge.jsonl']:
                assert (out/f).read_bytes() == (full/f).read_bytes(), 'Model state differs across full/timing runs: '+f
            def model_state(value):
                if isinstance(value, dict):
                    return {k:model_state(v) for k,v in value.items() if k not in ['before_ns','after_ns']}
                if isinstance(value, list):return [model_state(v) for v in value]
                return value
            assert model_state(read(out/'session.json')) == model_state(read(full/'session.json'))
            gs = lambda p: [{k:v for k,v in r.items() if k not in ['before_ns','after_ns']} for r in rows(p/'gates.jsonl')]
            assert gs(out) == gs(full)
        results.append({'case': name, 'status': 'pass', 'metrics': paired['metrics_G3'],
            'wait_ps': read(out/'session.json')['accumulated_wait_ps'], 'compute_ps': read(out/'session.json')['compute_ps'],
            'closed_and_drained': True, 'all_integer_residuals_ps': paired['all_integer_residuals_ps']})
        print(name, 'pass', paired['metrics_G3'], flush=True)
    shutil.copyfile(full/'G2-G3-paired.csv', folder/'G2-G3-paired.csv')
    summary = {'stage': 'S8', 'status': 'pass', 'acceptance_scope': read(s6/'stage-result.json')['acceptance_scope'],
        'table_sha256': sha(tablecopy), 'runs': results, 'G2_metrics': read(g2/'metrics.json'),
        'full_native_HBF_exact': behavior['gpu_native_exact'], 'numerical_arrays': numeric['arrays_compared'],
        'coverage': behavior['coverage'], 'deterministic_simulated_state_full_timing_exact': True,
        'S9_started': False, 'precision_limits_inherited': True}
    write(folder/'stage-result.json', summary)
    if not MANIFEST: write(ROOT/'docs/validation/gpu-s8.json', summary)
    write(study/'artifact-index.json', {'files': [{'path': p.relative_to(study).as_posix(), 'bytes': p.stat().st_size, 'sha256': sha(p)}
        for p in sorted(study.rglob('*')) if p.is_file() and p.name != 'artifact-index.json']})
    print('S8 pass; S9 not executed.', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--study', type=Path, required=True)
    run(parser.parse_args().study.resolve())
