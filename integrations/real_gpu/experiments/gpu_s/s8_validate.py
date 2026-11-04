"""Exercise S8 executable rejection before model loading or dependency submission."""
import argparse
import copy
import subprocess
from pathlib import Path
from common import read, write, sha
from study import ROOT, environment


def run(study):
    folder = study/'G3'; assert read(folder/'stage-result.json')['status'] == 'pass'
    binary = ROOT/'build-s3/bin/gpu_s_s8.exe'
    table = read(folder/'compute-table.json')
    cfg = read(folder/'configs/native-verify.json')
    tests = {
        'wrong_model_header': lambda t: t['header'].update(model_sha256='0'*64),
        'missing_segment': lambda t: t['segments'].pop(),
        'reordered_segment_ids': lambda t: t['segments'].reverse(),
        'noninteger_duration': lambda t: t['segments'][0].update(duration_ps=0.5),
    }
    evidence = []
    for name, mutate in tests.items():
        d = folder/'negative-table-cases'/name; d.mkdir(parents=True)
        invalid = copy.deepcopy(table); mutate(invalid)
        path = d/'table.json'; write(path, invalid)
        config = copy.deepcopy(cfg); config['compute_table_path'] = str(path)
        cf = d/'config.json'; write(cf, config)
        output = d/'run'
        cmd = [binary, config['model_path'], folder/'prompt.txt', output, 8, 'timing', cf]
        result = subprocess.run(list(map(str, cmd)), env=environment(), capture_output=True, timeout=60)
        (d/'stdout.log').write_bytes(result.stdout); (d/'stderr.log').write_bytes(result.stderr)
        assert result.returncode == 1
        assert not (output/'events.jsonl').exists()
        assert not (output/'tokens.jsonl').exists()
        assert not (output/'summary.json').exists()
        evidence.append({'case': name, 'status': 'pass', 'returncode': result.returncode,
                         'before_model_load_and_GPU_request': True,
                         'stderr': result.stderr.decode('utf-8', errors='replace').strip()})
    write(folder/'negative-table-validation.json', {'status': 'pass', 'cases': evidence,
          'binary_sha256': sha(binary), 'validator_sha256': sha(Path(__file__))})
    (folder/'source-and-binary-snapshot'/Path(__file__).name).write_bytes(Path(__file__).read_bytes())
    print('Four invalid tables rejected before model loading or GPU/HBF request.', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--study', type=Path, required=True)
    run(parser.parse_args().study.resolve())
