"""Adapt the updated loose-item event to the randomizer's original event contract."""


def normalize_sinking_sword(flowchart):
    events = {event.name: event for event in flowchart.events}
    extra = events.get('Event11')
    if extra is None:
        return False  # original event flow
    try:
        before, after = events['Event2'], events['Event0']
        data = extra.data
        known = (str(data.actor.v.identifier) == 'EventFlags' and str(data.actor_action.v) == 'SetFlag' and
                 data.params.data == {'value': True, 'symbol': 'FirstClear'} and data.nxt.v is after)
        if not known or before.data.nxt.v not in (extra, after):
            raise ValueError('Unexpected updated loose-item event layout')
        if before.data.nxt.v is after:
            return False  # already adapted
    except (AttributeError, KeyError, TypeError) as error:
        raise ValueError('Unexpected updated loose-item event layout') from error
    # Updated game data inserts FirstClear before the original fork. Randomized
    # loose items must not set that flag; preserve the original Event2 -> Event0
    # contract. Keep the unused event so existing indices remain stable.
    before.data.nxt.v = after
    return True
