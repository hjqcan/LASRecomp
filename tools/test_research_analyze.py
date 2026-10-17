import unittest
import tempfile
from pathlib import Path
from research_analyze import camera_report, divergence, input_motion_segments, inputs, matrix_product, phase_report, pose_comparison, timing, update_timing
from research_run import scenario_movement, scenario_transition, update_driver
from research_semantics import actor_comparison, actor_report, rng_comparison, rng_distance, rng_next, rng_report, rng_state, external_comparison, external_report


class ResearchAnalysis(unittest.TestCase):
    def test_matrix_order(self):
        view = [1, 0, 0, 3, 0, 1, 0, 4, 0, 0, 1, 5, 0, 0, 0, 1]
        projection = [2, 0, 0, 0, 0, 3, 0, 0, 0, 0, -1, -1, 0, 0, -1, 0]
        vp = matrix_product(projection, view)
        row = {f'{p}{i}': str(v) for p, values in [('v', view), ('p', projection), ('vp', vp)] for i, v in enumerate(values)}
        report = camera_report([row])
        self.assertTrue(report['p_times_v_consistent'])
        self.assertGreater(report['v_times_p_max_relative_error'], 0)
        self.assertEqual(report['view_rotation_max_orthogonality_error'], 0)
        self.assertEqual(report['view_rotation_determinant_range'], [1, 1])
        row['vp4'] = 'nan'
        with self.assertRaises(ValueError):
            camera_report([row])

    def sample(self, poll=0):
        return dict(poll=str(poll), frame=str(poll), elapsed_ns=str(poll * 1000000000), timer_calls=str(poll),
                    vsyncs=str(poll * 60), interval='1', observed='1', playing='1', level='Field', hp='12', position='1',
                    save_digest='123', x=str(poll), y='0', z='0')

    def test_divergence_and_length(self):
        a = [self.sample(0), self.sample(1)]
        self.assertTrue(divergence(a, a)['all_observations_match'])
        b = [dict(r) for r in a]
        b[1]['save_digest'] = '456'
        self.assertEqual(divergence(a, b)['equal_prefix_polls'], 1)
        self.assertFalse(divergence(a, a[:1])['all_observations_match'])
        self.assertFalse(divergence([], [])['all_observations_match'])

    def test_timing(self):
        r = timing([self.sample(0), self.sample(1)])
        self.assertEqual(r['observed_vsync_hz'], 60)
        self.assertEqual(r['xz_path_length'], 1)
        self.assertEqual(r['presentation_fps'], 1)

    def test_missing_positions_and_divergence(self):
        a = [self.sample(0), self.sample(1)]
        b = [dict(r) for r in a]
        b[0]['position'] = '0'
        b[1]['x'] = '1.5'
        report = pose_comparison(a, b)
        self.assertEqual(report['available_pairs'], 1)
        self.assertEqual(report['max_position_difference'], .5)
        self.assertEqual(report['first_position_difference']['comparison_index'], 1)

    def test_phase_pairs_and_unfinished_exit(self):
        a = dict(self.sample(), event='0', phase='0', before='1', thread='10', object='20', caller='30', counter='10', delta='0.016666667', poll_count='4')
        b = dict(a, event='1', thread='11')
        c = dict(a, event='2', before='0', counter='11', poll_count='5', x='.5')
        d = dict(b, event='3', before='0', counter='11', poll_count='5')
        e = dict(a, event='4')
        report = phase_report([a, b, c, d, e])
        self.assertEqual(report['unpaired_events'], 1)
        phase = report['callbacks']['update_dispatch']
        self.assertEqual(phase['complete_pairs'], 2)
        self.assertEqual(phase['counter_advances'], {'1': 2})
        self.assertEqual(phase['position_changes'], 1)

    def test_update_timing(self):
        rows = [dict(self.sample(i), update=str(i), counter_before=str(i), counter_after=str(i+1), delta='0.016666667') for i in range(2)]
        report = update_timing(rows)
        self.assertEqual(report['captured_updates'], 2)
        self.assertEqual(report['observed_update_hz'], 1)
        self.assertEqual(report['counter_advances'], {'1': 2})

    def test_input_boundary_and_ordinals(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            header = ('LASINPUT2\nbuild_id=' + 'a' * 40 + '\nboundary=las_update_dispatch_v1\n'
                      'update,buttons,lx,ly,rx,ry\n')
            tape = root / 'inputs.lastape'
            tape.write_text(header + '0,1,0,0,0,0\n', encoding='utf-8')
            _, boundary, values = inputs(root)
            self.assertEqual(boundary, 'boundary=las_update_dispatch_v1')
            self.assertEqual(values, [(1, 0, 0, 0, 0)])
            tape.write_text(header + '1,1,0,0,0,0\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                inputs(root)
            tape.write_text(header.replace('las_update_dispatch_v1', 'unknown') + '0,1,0,0,0,0\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                inputs(root)

    def test_held_input_motion(self):
        rows = [self.sample(i) for i in range(4)]
        values = [(0, 0, 0, 0, 0), (1 << 18, 32767, 0, 0, 0), (1 << 18, 32767, 0, 0, 0), (0, 0, 0, 0, 0)]
        segment = input_motion_segments(rows, values)[0]
        self.assertEqual(segment['updates'], 2)
        self.assertEqual(segment['xz_path_length'], 2)
        self.assertEqual(segment['path_units_per_host_second'], 1)
        rows[1]['position'] = '0'
        self.assertIsNone(input_motion_segments(rows, values)[0]['path_units_per_host_second'])

    def test_movement_coverage_rejects_static_dialogue(self):
        rows = [self.sample() for _ in range(1400)]
        self.assertFalse(scenario_movement(rows)['all_windows_moved'])
        for i, row in enumerate(rows):
            row['x'] = str(i / 10)
        result = scenario_movement(rows)
        self.assertTrue(result['all_windows_moved'])
        self.assertEqual([w['changing_samples'] for w in result['windows']], [25] * 4)
        self.assertFalse(scenario_movement(rows[:1200])['all_windows_moved'])

    def test_rng_consumption_and_scope(self):
        seed = (123456789, 362436069, 521288629, 88675123)
        one = rng_next(seed)
        self.assertEqual(one, (362436069, 521288629, 88675123, 3701687786))
        two = rng_next(one)
        self.assertEqual(rng_distance(seed, two), 2)
        self.assertIsNone(rng_distance(seed, two, 1))
        self.assertIsNone(rng_distance((0, 0, 0, 0), (0, 0, 0, 0)))
        def row(before, after):
            return dict(rng_before_observed='1', rng_observed='1',
                        **{p+str(i): str(x) for p, values in [('rng_before', before), ('rng', after)]
                           for i, x in enumerate(values)})
        rows = [row(seed, one), row(two, two)]
        report = rng_report(rows)
        self.assertEqual(report['within_dispatch_advances'], {'1': 1, '0': 1})
        self.assertEqual(report['between_dispatch_advances'], {'1': 1})
        self.assertTrue(rng_comparison(rows, rows)['all_observed_states_match'])
        self.assertFalse(rng_comparison(rows, rows[:1])['all_observed_states_match'])
        changed = [row(seed, two), rows[1]]
        self.assertEqual(rng_comparison(rows, changed)['first_difference']['update'], 0)
        self.assertIsNone(rng_state({}))
        self.assertEqual(rng_report([{}])['observed_pairs'], 0)
        self.assertFalse(rng_comparison([{}], [{}])['all_observed_states_match'])
        with self.assertRaises(ValueError):
            rng_state(dict(rows[0], rng0=str(1 << 32)))
        with self.assertRaises(ValueError):
            rng_state(dict(rows[0], rng_observed='2'))

    def test_external_provenance(self):
        row = dict(event='0', kind='0', pc='123', thread='4', index='0', value='987',
                   elapsed_ns='20', completed_updates='0', gate_ready='0')
        self.assertEqual(external_report([row])['by_source'], {'entropy': 1})
        report = external_comparison([row], [dict(row, value='988')])
        self.assertFalse(report['entropy']['all_returns_match'])
        self.assertEqual(report['entropy']['first_difference']['source_ordinal'], 0)
        self.assertFalse(report['user_system_clock']['all_returns_match'])
        with self.assertRaises(ValueError):
            external_report([dict(row, event='1')])

    def test_actor_identity_positions_and_partial_snapshots(self):
        sample = dict(update='0', level='House', complete='1', issue='0', registry_count='1', selected_count='1')
        row = dict(update='0', key='1', vtable='123', name='NpcTest-0000000000000001',
                   component='CharacterMovement', position='1', x='1', y='0', z='2')
        self.assertEqual(actor_report([sample], [row])[0]['position_rows'], 1)
        self.assertTrue(actor_comparison([sample], [row], [sample], [row])['all_samples_match'])
        report = actor_comparison([sample], [row], [sample], [dict(row, x='1.5')])
        self.assertEqual(report['max_component_position_difference'], .5)
        self.assertEqual(report['first_difference']['actor'], row['name'])
        partial = dict(sample, complete='0', issue='1')
        self.assertFalse(actor_comparison([partial], [], [partial], [])['all_samples_match'])
        with self.assertRaises(ValueError):
            actor_report([partial], [row])
        with self.assertRaises(ValueError):
            actor_report([sample], [])
        with self.assertRaises(ValueError):
            actor_report([sample], [dict(row, key='2')])

    def test_exit_scenario_and_level_coverage(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            update_driver(root / 'inputs.lastape', 'a' * 40, 4800, 'house-exit')
            values = inputs(root)[2]
            self.assertEqual(values[1600], (1 << 18, 32767, 0, 0, 0))
            self.assertEqual(values[1740], (0, 0, 0, 0, 0))
            self.assertEqual(values[1760], (2, 0, 0, 0, 0))
            self.assertEqual(values[2850], (1 << 19, 0, -32767, 0, 0))
        rows = [dict(self.sample(i), update=str(i), level='MarinTarinHouse' if i < 2 else 'Field') for i in range(4)]
        report = scenario_transition(rows)
        self.assertTrue(report['house_to_field_observed'])
        self.assertEqual(report['field_position_samples'], 2)
        self.assertFalse(scenario_transition(rows[:2])['house_to_field_observed'])


if __name__ == '__main__':
    unittest.main()
