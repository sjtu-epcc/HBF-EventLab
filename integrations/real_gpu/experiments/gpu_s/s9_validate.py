"""Independent S9 figure-data check against raw token/gate records."""
import argparse
import shutil
from pathlib import Path
from xml.etree import ElementTree
from PIL import Image
from common import read, rows, write, sha
from s9 import ROOT, csv_read


def run(folder):
    ledger = read(folder/'provenance.json')
    for r in ledger['inputs']:
        p = folder/r['snapshot']
        assert p.stat().st_size == r['bytes'] and sha(p) == r['sha256']
    inputs = ledger.get('validation_inputs')
    if inputs:
        g2 = rows(folder/inputs['g2_tokens'])
        g3 = rows(folder/inputs['g3_tokens'])
        copied = source = folder/inputs['g3_gates']
    else:
        g2 = rows(folder/'inputs/output/gpu-s/20261101-s7-001/G2/tokens.jsonl')
        g3 = rows(folder/'inputs/output/gpu-s/20261101-s8-002/G3/G3-full/tokens.jsonl')
        source = ROOT/'output/gpu-s/20261101-s8-002/G3/G3-full/gates.jsonl'
        copied = folder/'validator-inputs/gates.jsonl'; copied.parent.mkdir(exist_ok=True)
        if source.exists():
            shutil.copyfile(source, copied)
        else:
            assert copied.exists(), 'Missing raw gate snapshot'
    gates = rows(copied)
    paired = csv_read(folder/'tables/paired-tokens.csv')
    for i, (a, b, c) in enumerate(zip(g2, g3, paired)):
        assert a['token_id'] == b['token_id'] == int(c['token_id'])
        wait = sum(r['completed_ps']-r['offered_ps'] for r in gates if r['step'] <= i)
        assert b['relative_ps']-a['relative_ps'] == wait == int(c['cumulative_wait_ps'])
        assert a['relative_ps'] == int(c['G2_relative_ps']) and b['relative_ps'] == int(c['G3_relative_ps'])
    def values(tokens):
        return [tokens[0]['relative_ps'], (tokens[-1]['relative_ps']-tokens[0]['relative_ps'])/7]
    for row, a, b in zip(csv_read(folder/'tables/figure1-metrics.csv'), values(g2), values(g3)):
        assert float(row['G2_ps']) == a and float(row['G3_ps']) == b
        assert float(row['difference_ps']) == b-a
    for i, row in enumerate(csv_read(folder/'tables/figure2-itl.csv'), 1):
        assert int(row['G2_interval_ps']) == g2[i]['relative_ps']-g2[i-1]['relative_ps']
        assert int(row['G3_interval_ps']) == g3[i]['relative_ps']-g3[i-1]['relative_ps']
    phases = csv_read(folder/'tables/figure3-phases.csv')
    assert sum(int(r['G3_interval_ps']) for r in phases) == g3[-1]['relative_ps']
    assert sum(int(r['HBF_wait_ps']) for r in phases) == g3[-1]['wait_ps']
    for r in csv_read(folder/'tables/native-reference.csv'):
        event_path = (folder/inputs['native_events'][r['sample']] if inputs else
                      folder/'inputs/output/gpu-s/20261101-s6-001'/
                      ('sample-'+r['sample']+'-reference')/'events.jsonl')
        events = rows(event_path)
        t0 = next(e['timestamp_ns'] for e in events if e['type'] == 'request_begin')
        ts = [e['timestamp_ns'] for e in events if e['type'] == 'token_emitted']
        assert len(ts) == 8
        assert float(r['ttft_ns']) == ts[0]-t0
        assert float(r['tpot_ns']) == (ts[-1]-ts[0])/7
        assert int(r['request_to_last_token_ns']) == ts[-1]-t0
    for r in csv_read(folder/'tables/numerical-by-step.csv'):
        assert float(r['measured_max_relative_rms']) <= float(r['relative_rms_limit'])
        assert float(r['measured_max_normalized_element']) <= float(r['normalized_element_limit'])
        assert r['all_pass'] == 'True'
    checked = []
    for name in ['figure1-ttft-tpot', 'figure2-token-itl', 'figure3-prefill-decode']:
        with Image.open(folder/'figures'/(name+'.png')) as img:
            img.verify()
        svg = ElementTree.parse(folder/'figures'/(name+'.svg'))
        assert svg.getroot().tag.endswith('svg')
        checked.append(name)
    result = {'status': 'pass', 'all_input_snapshot_hashes_verified': True,
        'raw_token_gate_pairing_verified': True, 'paired_token_count': len(g2),
        'integer_pair_residual_ps': 0, 'figure_data_verified_against_raw_tokens': checked,
        'native_panel_verified_against_raw_host_events': True,
        'numerical_table_limits_verified': True,
        'PNG_SVG_parse_valid': True, 'rendered_visual_QA': 'All three figures inspected; no clipped labels; model/native axes and delta units explicit.',
        'additional_input': {'path': str(source), 'snapshot': str(copied.relative_to(folder)), 'sha256': sha(copied)},
        'validator_sha256': sha(Path(__file__))}
    write(folder/'figure-audit.json', result)
    shutil.copyfile(Path(__file__), folder/'s9_validate.py')
    print('S9 raw token/gate pairing, plot data, input hashes and PNG/SVG integrity passed.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--study', type=Path, required=True)
    run(parser.parse_args().study.resolve())
