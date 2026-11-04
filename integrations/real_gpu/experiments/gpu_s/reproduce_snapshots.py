"""Reproduce missing S6/S8 arrays in a new study; keep old evidence unchanged."""
import argparse
import sys
import shutil
from pathlib import Path
from common import read, write, rows, sha
from study import ROOT, BASE, invoke
from integrated_build_identity import verify_build
from reference import CpuQwen
from audit import numerical, stored_kv
from s5_audit import exact, normalized, positive
from s8 import table_audit, pair
from s5_validate import operations


def run(study):
    assert not study.exists(); study.mkdir(parents=True)
    s6 = ROOT/'output/gpu-s/20261101-s6-001'
    s8root = ROOT/'output/gpu-s/20261101-s8-002'; s8 = s8root/'G3'
    cfg = read(s8/'configs/G3-full.json')
    b6, b8 = ROOT/'build-s3/bin/gpu_s_reference.exe', ROOT/'build-s3/bin/gpu_s_s8.exe'
    c8 = read(s8/'conditions.json'); protocol = read(s6/'sampling_protocol.json')
    assert sha(b6) == protocol['reference_binary_sha256']
    assert sha(b8) == c8['binary_sha256']
    assert sha(Path(__file__).with_name('reference.py')) == c8['source_hashes']['reference.py']
    assert sha(cfg['model_path']) == cfg['gguf_sha256']
    write(study/'dependency-build-identity.json', verify_build(BASE))
    snap = study/'sources'; snap.mkdir()
    for p in [b6, b8, Path(__file__), Path(__file__).with_name('reference.py'),
              s6/'config.json', s8/'configs/native-verify.json', s8/'configs/G3-full.json']:
        name = p.name if p.name != 'config.json' else 'S6-config.json'
        shutil.copyfile(p, snap/name)
    write(study/'conditions.json', {'purpose': 'Missing numerical snapshot reproduction only; no new timing-table selection.',
        'original_S6': str(s6), 'original_S8': str(s8root),
        'model_sha256': cfg['gguf_sha256'], 'S6_binary_sha256': sha(b6), 'S8_binary_sha256': sha(b8),
        'CPU_reference_sha256': sha(Path(__file__).with_name('reference.py')),
        'original_records_modified': False, 'fixed_compute_table_unchanged': True})
    mappings = []
    for label, original, config_path, binary in [
        ('S6-reference-verification', s6/'reference-verification', s6/'config.json', b6),
        ('S8-native-verify', s8/'native-verify', s8/'configs/native-verify.json', b8),
        ('S8-G3-full', s8/'G3-full', s8/'configs/G3-full.json', b8)]:
        old_command = read((s6 if label.startswith('S6') else s8root)/'logs'/original.name/'command.json')
        out = study/label
        invoke([binary, cfg['model_path'], old_command['argv'][2], out, 8, 'verify', config_path], study/'logs'/label)
        a, b = read(original/'summary.json'), read(out/'summary.json')
        assert all(a[k] == b[k] for k in ['tokens','raw_tokens','prompt_ids','positions','stop_reason'])
        assert (original/'logits.f32').read_bytes() == (out/'logits.f32').read_bytes()
        assert normalized(rows(original/'snapshots.jsonl')) == normalized(rows(out/'snapshots.jsonl'))
        assert normalized(rows(original/'events.jsonl')) == normalized(rows(out/'events.jsonl'))
        assert stored_kv(out, cfg)['status'] == 'pass'
        if label.startswith('S8'):
            table_audit(out, read(s8/'compute-table.json'))
            assert operations(out) == operations(original)
        if label == 'S8-G3-full':
            pair(out, ROOT/'output/gpu-s/20261101-s7-001/G2', read(s8/'compute-table.json'))
        mappings.append((label, original, out))
        print(label, 'rerun semantic/logits/events/KV checks pass', flush=True)
    cpu = study/'S8-cpu-8'
    ref = CpuQwen(cfg); ref.run(read(study/'S8-native-verify/summary.json')['prompt_ids'], 8, cpu); del ref
    target = read(ROOT/'output/gpu-s/20261101-s2-b2-v2-001/S0/numerical-targets-predeclared.json')
    numerical_results = {}
    for label, _, out in mappings:
        n = numerical(out, cpu, cfg, target); assert n['status'] == 'pass'
        numerical_results[label] = {k:n[k] for k in ['status','arrays_compared','failed_arrays']}
    exact(study/'S8-native-verify', study/'S8-G3-full', cfg)
    # S6 verify uses the same mathematical path; valid arrays must also match.
    exact(study/'S6-reference-verification', study/'S8-native-verify', cfg)
    positive(study/'S8-G3-full', study/'S8-native-verify',
             read(ROOT/'output/gpu-s/20261101-s4-001/S4/M2-regions.json'), cfg,
             {int(k):v for k,v in read(study/'S8-G3-full/segment-audit.json')['compute_by_step_ps'].items()})
    reports = []
    for label, original, out in mappings+[('S8-cpu-8', s8/'cpu-8', cpu)]:
        root = s6 if label.startswith('S6') else s8root
        expected = {r['path']:r for r in read(root/'artifact-index.json')['files']}
        files = [p for p in out.iterdir() if p.is_file() and (p.name.startswith('snapshot-') and p.suffix=='.bin' or p.name=='snapshots.npz')]
        records = []
        for p in sorted(files):
            old_path = original/p.name; rel = old_path.relative_to(root).as_posix()
            old = expected[rel]; digest = sha(p)
            records.append({'original_path': old_path.relative_to(ROOT).as_posix(),
                'new_path': p.relative_to(study).as_posix(), 'original_sha256': old['sha256'],
                'new_sha256': digest, 'original_bytes': old['bytes'], 'new_bytes': p.stat().st_size,
                'byte_exact': digest == old['sha256'], 'was_missing': not old_path.exists()})
        reports += records
        print(label, 'raw file hashes exact', sum(r['byte_exact'] for r in records), '/', len(records), flush=True)
    write(study/'file-comparison.json', reports)
    result = {'status': 'pass', 'purpose': 'numerical_snapshot_reproduction', 'new_files': len(reports),
        'byte_exact_files': sum(r['byte_exact'] for r in reports),
        'different_raw_files': sum(not r['byte_exact'] for r in reports),
        'all_missing_paths_reproduced': True, 'valid_GPU_arrays_exact_across_new_runs': True,
        'all_original_logits_tokens_events_match': True, 'numerical': numerical_results,
        'old_paths_filled_or_overwritten': False, 'original_timing_samples_replaced': False,
        'note': 'Raw mismatches, if any, are retained as new data and never labelled original bytes. Valid KV checks exclude unused padding; raw-file equality is stronger.'}
    write(study/'stage-result.json', result)
    write(study/'artifact-index.json', {'files':[{'path':p.relative_to(study).as_posix(), 'bytes':p.stat().st_size,'sha256':sha(p)}
        for p in sorted(study.rglob('*')) if p.is_file() and p.name!='artifact-index.json']})
    print(result, flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--study', type=Path, required=True)
    run(p.parse_args().study.resolve())
