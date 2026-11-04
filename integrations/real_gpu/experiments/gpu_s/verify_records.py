"""Read-only verification of the upstream minexp archive and a fresh CPU replay.

This rechecks historical evidence. It does NOT execute CUDA or validate the new
backend. Every generated audit goes to a new output directory, never into inputs.
"""
import argparse
import gzip
import json
from pathlib import Path
from common import read, rows, sha, write
from audit import numerical, stored_kv
from s7 import validate, replay, independent_audit


def require(ok, message):
    if not ok:
        raise ValueError(message)


def run(bundle, records, output):
    bundle, records, output = map(lambda p: Path(p).resolve(), (bundle, records, output))
    require(not output.exists(), 'Audit output already exists')
    require(not output.is_relative_to(records), 'Audit output must not modify original record tree')
    require(not output.is_relative_to(bundle), 'Audit output must not modify original bundle tree')
    output.mkdir(parents=True)
    summary = read(bundle/'summary.json')
    index_bytes = gzip.decompress((bundle/'file-index.json.gz').read_bytes())
    import hashlib
    require(hashlib.sha256(index_bytes).hexdigest() == summary['file_index_sha256'], 'File index hash mismatch')
    index = json.loads(index_bytes)
    archive_hash = hashlib.sha256()
    for part in index['parts']:
        path = (bundle/part['path']).resolve()
        require(path.is_relative_to(bundle), 'Archive part path escapes bundle')
        require(path.stat().st_size == part['bytes'] and sha(path) == part['sha256'], 'Archive part differs')
        with path.open('rb') as stream:
            for block in iter(lambda: stream.read(1024*1024), b''):
                archive_hash.update(block)
    require(archive_hash.hexdigest() == summary['archive_sha256'] == index['archive_sha256'], 'Archive identity differs')
    total = 0
    for rel, item in index['files'].items():
        path = (records/rel).resolve()
        require(path.is_relative_to(records), 'Original path escapes record root')
        require(path.is_file() and path.stat().st_size == item['bytes'] and sha(path) == item['sha256'], 'Missing/changed record: '+rel)
        total += item['bytes']
    require(len(index['files']) == summary['file_count'] and total == summary['total_original_bytes'], 'Record count/size differs')
    studies = records/'output/gpu-s'
    source = studies/'20261101-s6-001'
    qualified = studies/'20261101-s6-minimum-acceptance-001'
    tablefile = qualified/'compute-table.json'
    table = read(tablefile)
    require(sha(tablefile) == read(qualified/'stage-result.json')['qualified_table_sha256'], 'Qualified table hash differs')
    require(sha(source/'config.json') == table['header']['config_sha256'], 'Historical profile hash differs')
    require(sha(source/'sampling_protocol.json') == table['header']['protocol_sha256'], 'Sampling protocol differs')
    sample = source/table['header']['sample']
    require(sha(sample/'events.jsonl') == table['header']['events_sha256'], 'Selected event trace differs')
    require(read(sample/'measured-segments.json') == table['segments'], 'Frozen durations differ from measured sample')
    windows, events = rows(sample/'segments.jsonl'), rows(sample/'events.jsonl')
    regions = read(studies/'20261101-s7-001/G2/inputs/M2-regions.json')
    validate(table, table, windows, events)
    replay(table, windows, events, regions, output/'G2')
    replay_audit = independent_audit(output/'G2', table, events, windows, regions)
    expected = read(studies/'20261101-s7-001/G2/metrics.json')
    require(read(output/'G2/metrics.json') == expected, 'New replay differs from published metrics')
    profile = read(source/'config.json')
    target = read(studies/'20261101-s2-b2-v2-001/S0/numerical-targets-predeclared.json')
    g3 = studies/'20261101-s8-002/G3'
    checks = []
    for name, native in [('S6-reference', source/'reference-verification'),
                         ('S8-native', g3/'native-verify'), ('S8-G3-full', g3/'G3-full')]:
        n = numerical(native, g3/'cpu-8', profile, target, output/(name+'-numerical.json'))
        k = stored_kv(native, profile, output/(name+'-kv-publication.json'))
        require(n['status'] == k['status'] == 'pass', 'Historical numerical/KV audit failed: '+name)
        checks.append({'case': name, 'arrays': n['arrays_compared'], 'failed_arrays': n['failed_arrays'], 'valid_kv_arrays': k['valid_arrays']})
    result = {'status': 'pass', 'scope': 'historical-byte-integrity-and-independent-CPU-replay-audit',
              'new_GPU_or_backend_executed': False, 'files_verified': len(index['files']), 'bytes_verified': total,
              'archive_sha256': summary['archive_sha256'], 'table_sha256': sha(tablefile),
              'G2_metrics': expected, 'replay_segments': replay_audit['segments'], 'numerical_checks': checks,
              'precision_limits': read(qualified/'stage-result.json')['precision_qualification']}
    write(output/'result.json', result)
    print(json.dumps(result, indent=2))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--records', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    run(args.bundle, args.records, args.output)
