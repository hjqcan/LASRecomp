import unittest
from research_analyze import camera_report, divergence, matrix_product, pose_comparison, timing


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


if __name__ == '__main__':
    unittest.main()
