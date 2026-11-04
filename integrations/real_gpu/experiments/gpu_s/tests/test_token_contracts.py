"""Self-contained CPU fixtures: no CUDA, torch, backend or model imports.

Run: python -m unittest discover -s tests -p test_token_contracts.py -v
Synthetic CUPTI-shaped records only exercise audit logic, not real GPU EOS.
"""
import ast
import copy
import csv
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

EXPERIMENT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(EXPERIMENT))
import common
import token_contracts as contracts


def jsonl(path, records):
    path.write_text(''.join(json.dumps(r)+'\n' for r in records), encoding='utf-8')


class TokenContractsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='s8-token-contracts-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.g2, self.g3 = self.root/'G2', self.root/'G3'
        self.g2.mkdir()
        self.g3.mkdir()

    def fixture(self, outputs=8, eos=False, eos_wait=None):
        steps = outputs+int(eos)
        table = {'header': {'frozen_G1': True, 'trace': 'synthetic-only'}, 'segments': []}
        tokens2, tokens3, events, windows, gates, kernels, transfers, apis = ([] for _ in range(8))
        origin, compute, wait = 1_000_000, 0, 0

        def event(kind, step, at, **extra):
            record = dict(type=kind, request_id=1, event_id=len(events)+1,
                          step=step, timestamp_ns=at, **extra)
            events.append(record)
            return record

        event('request_begin', -1, 1)
        for step in range(steps):
            base, node = 100+step*1000, step+1
            event('execution_begin', step, base-10)
            duration = [10+step, 20+step, 30+step]
            step_wait = eos_wait if eos and step == outputs and eos_wait is not None else (step+1)*7
            for index, kind in enumerate(('gap', 'node', 'tail')):
                sid = len(windows)+1
                start = base+index*130
                before = origin+compute+wait
                compute += duration[index]
                frozen = dict(segment_id=sid, kind=kind, node_id=node, step=step,
                              duration_ps=duration[index])
                table['segments'].append(frozen)
                window = dict(frozen, table_segment_id=sid, start_ns=start, end_ns=start+100,
                              first_activity_id=sid, activity_count=1, modeled=True,
                              advance=dict(offered_ps=before, completed_ps=origin+compute+wait))
                windows.append(window)
                activity = dict(activity_id=sid, step=step, start_ns=start+20,
                                end_ns=start+30, correlation=sid*2, stream=1)
                if kind == 'node':
                    kernels.append(dict(activity, name='synthetic_kernel'))
                else:
                    transfers.append(dict(activity, kind='memcpy', copy_kind=1 if kind == 'gap' else 2, bytes=64))
                apis.extend([
                    dict(correlation=sid*2, domain='runtime', cbid=1, start_ns=start+1,
                         end_ns=start+10, return_value=0),
                    dict(correlation=sid*2+1, domain='runtime', cbid=165, start_ns=start+70,
                         end_ns=start+90, return_value=0),
                ])
                if kind == 'gap':
                    event('node_before', step, base+110, node_id=node)
                    gates.append(dict(step=step, node_id=node, before_ns=base+115, after_ns=base+125,
                                      offered_ps=origin+compute+wait, completed_ps=origin+compute+wait+step_wait,
                                      complete=True, success=True))
                    wait += step_wait
                elif kind == 'node':
                    event('node_after', step, base+240, node_id=node)
            event('execution_end', step, base+370)
            is_eos = eos and step == outputs
            kind, token_id = ('EOS', 999) if is_eos else ('token_emitted', 100+step)
            semantic = event('eos' if is_eos else kind, step, base+380, token_id=token_id,
                             **({} if is_eos else {'output_index': step+1}))
            token = dict(type=kind, token_id=token_id, output_index=step+1,
                         request_t0_ps=0, tick_ps=compute, relative_ps=compute, compute_ps=compute, wait_ps=0)
            tokens2.append(dict(token, request_id=1, step=step, source_event_id=semantic['event_id']))
            # Match existing hbf_gate.hpp: step is absent in G3 tokens.jsonl.
            tokens3.append(dict(token, request_t0_ps=origin, tick_ps=origin+compute+wait,
                                relative_ps=compute+wait, wait_ps=wait))
        event('request_end', steps-1, 100+steps*1000, actual_outputs=outputs,
              reason='EOS' if eos else 'length')
        for folder, tokens in ((self.g2, tokens2), (self.g3, tokens3)):
            jsonl(folder/'tokens.jsonl', tokens)
            jsonl(folder/'events.jsonl', events)
        common.write(self.g2/'metrics.json', common.metrics(0, [t['tick_ps'] for t in tokens2 if t['type'] == 'token_emitted']))
        for filename, data in [('gates.jsonl', gates), ('segments.jsonl', windows),
                               ('kernels.jsonl', kernels), ('transfers.jsonl', transfers), ('apis.jsonl', apis)]:
            jsonl(self.g3/filename, data)
        common.write(self.g3/'summary.json', dict(cupti_dropped=0, raw_tokens=[t['token_id'] for t in tokens3]))
        common.write(self.g3/'session.json', dict(request_t0_ps=origin, finished_ps=origin+compute+wait,
                                                compute_ps=compute, accumulated_wait_ps=wait))
        return table

    def pair(self, table):
        return contracts.pair(self.g3, self.g2, table)

    def mutate_records(self, folder, filename, mutation):
        records = common.rows(folder/filename)
        mutation(records)
        jsonl(folder/filename, records)

    def test_legacy_eight_non_eos_and_raw_coverage_audit(self):
        table = self.fixture()
        result = self.pair(table)
        self.assertEqual(result['metrics_G3']['actual_outputs'], 8)
        self.assertEqual(result['tpot_difference_denominator'], 7)
        self.assertEqual(result['tpot_difference_numerator_ps'], sum((i+1)*7 for i in range(1, 8)))
        self.assertEqual(result['all_integer_residuals_ps'], [0]*8)
        self.assertIsNone(result['eos_ready'])
        audit = contracts.table_audit(self.g3, table, copy.deepcopy(table['header']))
        self.assertEqual(audit['window_count'], 24)
        self.assertTrue(audit['all_activities_owned_once'])

    def test_one_token_has_null_tpot(self):
        result = self.pair(self.fixture(outputs=1))
        self.assertEqual(result['metrics_G3'], common.metrics(0, [67]))
        self.assertIsNone(result['tpot_difference_numerator_ps'])
        self.assertIsNone(result['tpot_difference_denominator'])

    def test_zero_outputs_eos_ready_conserved_but_not_an_output(self):
        table = self.fixture(outputs=0, eos=True)
        result = self.pair(table)
        self.assertEqual(result['metrics_G3'], common.metrics(0, []))
        self.assertEqual(result['paired_tokens'], [])
        self.assertEqual(result['eos_ready']['G3_relative_ps'], 67)
        self.assertEqual(result['all_integer_residuals_ps'], [0])
        self.assertIsNone(result['tpot_difference_denominator'])
        with (self.g3/'G2-G3-paired.csv').open(newline='', encoding='utf-8') as stream:
            reader = csv.DictReader(stream)
            self.assertEqual(reader.fieldnames, contracts.PAIRED_FIELDS)
            self.assertEqual(list(reader), [])
        self.assertEqual(contracts.table_audit(self.g3, table)['window_count'], 3)

    def test_tail_eos_excluded_from_metrics_and_tpot_wait(self):
        table = self.fixture(outputs=3, eos=True, eos_wait=123456)
        result = self.pair(table)
        self.assertEqual(result['metrics_G3']['actual_outputs'], 3)
        self.assertEqual(len(result['paired_events']), 4)
        self.assertEqual(result['tpot_difference_denominator'], 2)
        self.assertEqual(result['tpot_difference_numerator_ps'], 14+21)
        self.assertEqual(result['eos_ready']['step_wait_ps'], 123456)
        self.assertEqual(result['all_integer_residuals_ps'], [0]*4)
        self.assertEqual(contracts.table_audit(self.g3, table)['window_count'], 12)

    def test_one_output_then_eos_has_null_tpot(self):
        result = self.pair(self.fixture(outputs=1, eos=True))
        self.assertEqual(result['metrics_G3']['actual_outputs'], 1)
        self.assertIsNone(result['metrics_G3']['tpot_ps'])
        self.assertIsNone(result['tpot_difference_numerator_ps'])
        self.assertIsNone(result['tpot_difference_denominator'])

    def test_zero_requested_outputs_empty_complete_table(self):
        table = self.fixture(outputs=0)
        result = self.pair(table)
        self.assertEqual(result['metrics_G3'], common.metrics(0, []))
        self.assertEqual(result['all_integer_residuals_ps'], [])
        self.assertIsNone(result['eos_ready'])
        self.assertEqual(contracts.table_audit(self.g3, table)['window_count'], 0)

    def test_variable_output_count_not_a_new_native_count_claim(self):
        result = self.pair(self.fixture(outputs=11))
        self.assertEqual(result['metrics_G3']['actual_outputs'], 11)
        self.assertEqual(result['tpot_difference_denominator'], 10)

    def test_metrics_reuses_common_and_never_wall_time(self):
        table = self.fixture(outputs=3, eos=True)
        original = self.pair(table)
        for folder in (self.g2, self.g3):
            self.mutate_records(folder, 'events.jsonl', lambda rs: [r.update(timestamp_ns=-100-i) for i, r in enumerate(rs)])
        self.mutate_records(self.g3, 'gates.jsonl', lambda rs: [r.update(before_ns=-1, after_ns=10**50) for r in rs])
        with patch.object(contracts, 'metrics', wraps=common.metrics) as shared:
            result = self.pair(table)
            self.assertEqual(shared.call_count, 2)
        self.assertEqual(result, original)

    def test_header_equality_and_true_frozen_flag(self):
        table = self.fixture(outputs=1)
        with self.assertRaisesRegex(ValueError, 'provenance'):
            contracts.table_audit(self.g3, table, {'frozen_G1': True, 'trace': 'different'})
        for value in (False, 1, 'true', None):
            with self.subTest(frozen=value):
                bad = copy.deepcopy(table)
                bad['header']['frozen_G1'] = value
                with self.assertRaises(ValueError):
                    self.pair(bad)

    def test_invalid_duration_types_and_uint64_boundary(self):
        table = self.fixture(outputs=1)
        for value in (-1, 1.5, True, False, 2**64, '1', None):
            with self.subTest(duration=value):
                bad = copy.deepcopy(table)
                bad['segments'][0]['duration_ps'] = value
                with self.assertRaises(ValueError):
                    contracts.validate_frozen_table(bad)
        table['segments'][0]['duration_ps'] = 2**64-1
        contracts.validate_frozen_table(table)
        table['segments'][0]['duration_ps'] = 0
        contracts.validate_frozen_table(table)

    def test_invalid_table_identities(self):
        table = self.fixture(outputs=2)
        mutations = {
            'missing': lambda ss: ss.pop(1),
            'reordered': lambda ss: ss.reverse(),
            'duplicate': lambda ss: ss.append(copy.deepcopy(ss[0])),
            'duplicate_key': lambda ss: ss[1].update(kind=ss[0]['kind']),
            'boolean_id': lambda ss: ss[0].update(segment_id=True),
            'boolean_node': lambda ss: ss[0].update(node_id=True),
            'float_step': lambda ss: ss[0].update(step=0.0),
            'missing_step': lambda ss: ss[3].update(step=2),
            'step_overflow': lambda ss: ss[0].update(step=2**31),
            'verification': lambda ss: ss[0].update(kind='verification'),
        }
        for label, mutate in mutations.items():
            with self.subTest(case=label):
                bad = copy.deepcopy(table)
                mutate(bad['segments'])
                with self.assertRaises(ValueError):
                    contracts.validate_frozen_table(bad)

    def test_full_consumption_and_stable_window_identity(self):
        table = self.fixture(outputs=2)
        windows = common.rows(self.g3/'segments.jsonl')
        mutations = {
            'missing': lambda ws: ws.pop(),
            'duplicate': lambda ws: ws.append(copy.deepcopy(ws[0])),
            'reordered': lambda ws: ws.reverse(),
            'kind': lambda ws: ws[0].update(kind='node'),
            'node': lambda ws: ws[0].update(node_id=99),
            'step': lambda ws: ws[0].update(step=1),
            'duration': lambda ws: ws[0].update(duration_ps=0),
            'empty_positive': lambda ws: ws[0].update(activity_count=0, modeled=False),
            'wrong_modeled': lambda ws: ws[0].update(modeled=False),
            'boolean_count': lambda ws: ws[0].update(activity_count=True),
        }
        for label, mutate in mutations.items():
            with self.subTest(case=label):
                bad = copy.deepcopy(windows)
                mutate(bad)
                with self.assertRaises(ValueError):
                    contracts.validate_table_windows(table, bad)
        table['segments'][0]['duration_ps'] = windows[0]['duration_ps'] = 0
        with self.assertRaisesRegex(ValueError, 'coverage'):
            contracts.validate_table_windows(table, windows)
        windows[0].update(activity_count=0, modeled=False)
        contracts.validate_table_windows(table, windows)

    def test_zero_duration_table_row_cannot_be_dropped(self):
        table = self.fixture(outputs=1)
        windows = common.rows(self.g3/'segments.jsonl')
        table['segments'][0]['duration_ps'] = windows[0]['duration_ps'] = 0
        windows[0].update(activity_count=0, modeled=False)
        with self.assertRaisesRegex(ValueError, 'unconsumed'):
            contracts.validate_table_windows(table, windows[1:])

    def test_verification_is_excluded_not_model_time(self):
        table = self.fixture(outputs=1)
        windows = common.rows(self.g3/'segments.jsonl')
        verification = dict(windows[0], kind='verification', table_segment_id=None, duration_ps=0, modeled=False)
        contracts.validate_table_windows(table, windows+[verification])
        for change in ({'duration_ps': 1}, {'table_segment_id': 1}, {'modeled': True}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                contracts.validate_table_windows(table, windows+[dict(verification, **change)])

    def test_early_eos_cannot_ignore_frozen_suffix(self):
        table = self.fixture(outputs=3)
        self.fixture(outputs=0, eos=True)
        with self.assertRaisesRegex(ValueError, 'complete token/EOS trajectory'):
            self.pair(table)
        with self.assertRaisesRegex(ValueError, 'unconsumed'):
            contracts.table_audit(self.g3, table)

    def test_missing_eos_table_step_rejected(self):
        table = self.fixture(outputs=2, eos=True)
        table['segments'] = table['segments'][:-3]
        with self.assertRaisesRegex(ValueError, 'complete token/EOS trajectory'):
            self.pair(table)

    def test_strict_token_event_pairing_mutations(self):
        mutations = {
            'missing_token': ('tokens.jsonl', lambda rs: rs.pop()),
            'token_id': ('tokens.jsonl', lambda rs: rs[0].update(token_id=777)),
            'token_type': ('tokens.jsonl', lambda rs: rs[0].update(type='EOS')),
            'token_step': ('tokens.jsonl', lambda rs: rs[0].update(step=1)),
            'wrong_index': ('tokens.jsonl', lambda rs: rs[0].update(output_index=0)),
            'reordered_tokens': ('tokens.jsonl', lambda rs: rs.reverse()),
            'missing_event': ('events.jsonl', lambda rs: rs.pop(-2)),
            'event_step': ('events.jsonl', lambda rs: next(r for r in rs if r['type'] == 'token_emitted').update(step=1)),
            'event_token_id': ('events.jsonl', lambda rs: next(r for r in rs if r['type'] == 'token_emitted').update(token_id=777)),
            'event_type': ('events.jsonl', lambda rs: next(r for r in rs if r['type'] == 'token_emitted').update(type='eos')),
            'request_count': ('events.jsonl', lambda rs: rs[-1].update(actual_outputs=999)),
            'request_reason': ('events.jsonl', lambda rs: rs[-1].update(reason='EOS')),
        }
        for label, (filename, mutate) in mutations.items():
            with self.subTest(case=label):
                table = self.fixture(outputs=2)
                self.mutate_records(self.g3, filename, mutate)
                with self.assertRaises(ValueError):
                    self.pair(table)

    def test_cross_trace_type_id_step_identity(self):
        table = self.fixture(outputs=1)
        # Each G3 record still matches its own semantic event, but not G2.
        self.mutate_records(self.g3, 'tokens.jsonl', lambda rs: rs[0].update(token_id=777))
        self.mutate_records(self.g3, 'events.jsonl', lambda rs: next(r for r in rs if r['type'] == 'token_emitted').update(token_id=777))
        with self.assertRaisesRegex(ValueError, 'Unpaired G2/G3'):
            self.pair(table)
        table = self.fixture(outputs=1)
        self.mutate_records(self.g3, 'tokens.jsonl', lambda rs: rs[0].update(type='EOS'))
        self.mutate_records(self.g3, 'events.jsonl', lambda rs: (
            next(r for r in rs if r['type'] == 'token_emitted').update(type='eos'),
            rs[-1].update(actual_outputs=0, reason='EOS')))
        with self.assertRaisesRegex(ValueError, 'Unpaired G2/G3'):
            self.pair(table)

    def test_replay_semantic_model_clock_binding(self):
        table = self.fixture(outputs=1)
        token = common.rows(self.g2/'tokens.jsonl')[0]
        self.mutate_records(self.g2, 'events.jsonl', lambda rs: next(
            r for r in rs if r['type'] == 'token_emitted').update(tick_ps=token['tick_ps']))
        self.pair(table)
        self.mutate_records(self.g2, 'events.jsonl', lambda rs: next(
            r for r in rs if r['type'] == 'token_emitted').update(tick_ps=token['tick_ps']+1))
        with self.assertRaisesRegex(ValueError, 'semantic model clock'):
            self.pair(table)

    def test_source_event_link_mismatch(self):
        table = self.fixture(outputs=1)
        self.mutate_records(self.g2, 'tokens.jsonl', lambda rs: rs[0].update(source_event_id=1))
        with self.assertRaisesRegex(ValueError, 'source event'):
            self.pair(table)

    def test_event_after_eos_is_rejected(self):
        table = self.fixture(outputs=2, eos=True)
        for folder in (self.g2, self.g3):
            self.mutate_records(folder, 'tokens.jsonl', lambda rs: rs[0].update(type='EOS'))
            self.mutate_records(folder, 'events.jsonl', lambda rs: next(r for r in rs if r['type'] == 'token_emitted').update(type='eos'))
        with self.assertRaisesRegex(ValueError, 'terminal'):
            self.pair(table)

    def test_gate_failures_and_unconsumed_wait_rejected(self):
        mutations = {
            'unknown_step': lambda rs: rs[0].update(step=10),
            'negative': lambda rs: rs[0].update(offered_ps=-1),
            'fraction': lambda rs: rs[0].update(completed_ps=1.5),
            'boolean': lambda rs: rs[0].update(completed_ps=True),
            'failed': lambda rs: rs[0].update(success=False),
            'incomplete': lambda rs: rs[0].update(complete=False),
            'backdated': lambda rs: rs[0].update(completed_ps=0),
            'after_token': lambda rs: rs[0].update(completed_ps=2**64-1),
            'missing': lambda rs: rs.pop(),
            'duplicate': lambda rs: rs.append(copy.deepcopy(rs[0])),
            'reordered': lambda rs: rs.reverse(),
        }
        for label, mutate in mutations.items():
            with self.subTest(case=label):
                table = self.fixture(outputs=2, eos=True)
                self.mutate_records(self.g3, 'gates.jsonl', mutate)
                with self.assertRaises(ValueError):
                    self.pair(table)

    def test_integer_token_conservation_and_origin(self):
        changes = [{'tick_ps': 0}, {'relative_ps': 1}, {'compute_ps': 1}, {'wait_ps': 1},
                   {'request_t0_ps': 0}, {'compute_ps': True}, {'tick_ps': 2**64}]
        for change in changes:
            with self.subTest(change=change):
                table = self.fixture(outputs=2)
                self.mutate_records(self.g3, 'tokens.jsonl', lambda rs: rs[1].update(change))
                with self.assertRaises(ValueError):
                    self.pair(table)

    def test_stored_metrics_cannot_count_eos_as_output(self):
        table = self.fixture(outputs=1, eos=True)
        common.write(self.g2/'metrics.json', common.metrics(0, [t['tick_ps'] for t in common.rows(self.g2/'tokens.jsonl')]))
        with self.assertRaisesRegex(ValueError, 'Stored G2 metrics'):
            self.pair(table)

    def test_independent_raw_coverage_audit_is_not_bypassed(self):
        table = self.fixture(outputs=1)
        self.mutate_records(self.g3, 'kernels.jsonl', lambda rs: rs.clear())
        with self.assertRaises(AssertionError):
            contracts.table_audit(self.g3, table)

    def test_s8_compatibility_wrappers_without_importing_torch(self):
        # Load only the two allowed function definitions: no legacy runner import.
        tree = ast.parse((EXPERIMENT/'s8.py').read_text(encoding='utf-8'))
        selected = [node for node in tree.body if isinstance(node, ast.FunctionDef)
                    and node.name in ('pair', 'table_audit')]
        namespace = {}
        exec(compile(ast.Module(body=selected, type_ignores=[]), 's8-contract-wrappers', 'exec'), namespace)
        table = self.fixture(outputs=1)
        self.assertEqual(namespace['pair'](self.g3, self.g2, table)['metrics_G3']['actual_outputs'], 1)
        self.assertEqual(namespace['table_audit'](self.g3, table)['status'], 'pass')


if __name__ == '__main__':
    unittest.main()
