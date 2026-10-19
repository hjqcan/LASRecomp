from types import SimpleNamespace as N
import unittest
from rando.version_compat import normalize_sinking_sword


def flow(updated):
    after = N(name='Event0', data=N())
    extra = N(name='Event11', data=N(actor=N(v=N(identifier='EventFlags')), actor_action=N(v='SetFlag'),
                                    params=N(data={'value': True, 'symbol': 'FirstClear'}), nxt=N(v=after)))
    before = N(name='Event2', data=N(nxt=N(v=extra if updated else after)))
    return N(events=[before, after, extra] if updated else [before, after]), before, after, extra


class EventCompatibilityTests(unittest.TestCase):
    def test_original_unchanged(self):
        f,b,a,_ = flow(False)
        self.assertFalse(normalize_sinking_sword(f)); self.assertIs(b.data.nxt.v,a)

    def test_updated_and_idempotent(self):
        f,b,a,e = flow(True)
        self.assertTrue(normalize_sinking_sword(f)); self.assertIs(b.data.nxt.v,a)
        self.assertIs(f.events[-1],e); self.assertFalse(normalize_sinking_sword(f))

    def test_unknown_layout_preserves_links(self):
        for field in ('actor','action','flag','successor'):
            f,b,a,e = flow(True)
            if field=='actor': e.data.actor.v.identifier='DifferentActor'
            if field=='action': e.data.actor_action.v='DifferentAction'
            if field=='flag': e.data.params.data['symbol']='DifferentFlag'
            if field=='successor': e.data.nxt.v=b
            with self.assertRaises(ValueError): normalize_sinking_sword(f)
            self.assertIs(b.data.nxt.v,e)

    def test_missing_event_rejected(self):
        f,b,a,e = flow(True); f.events.remove(a)
        with self.assertRaises(ValueError): normalize_sinking_sword(f)
        self.assertIs(b.data.nxt.v,e)


if __name__=='__main__': unittest.main()
