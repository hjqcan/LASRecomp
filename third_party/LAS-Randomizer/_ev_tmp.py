import sys
sys.path.insert(0,'.')
from RandomizerCore.Tools import event_tools
flow = event_tools.readFlow('../../extracted/romfs/region_common/event/'+sys.argv[1]); fc = flow.flowchart
print([e.name for e in fc.entry_points])
ep = event_tools.findEntryPoint(fc, sys.argv[2])
seen=set(); stack=[ep.main_event.v]
while stack:
    ev=stack.pop()
    if ev is None or ev.name in seen: continue
    seen.add(ev.name); d=ev.data; k=type(d).__name__
    if k=='ActionEvent': print(ev.name,'A',d.actor.v.identifier.name,d.actor_action.v.v,dict(d.params.data) if d.params else {}); stack.append(d.nxt.v)
    elif k=='SwitchEvent': print(ev.name,'S',d.actor.v.identifier.name,d.actor_query.v.v,dict(d.params.data) if d.params else {},{c:v.v.name if v.v else None for c,v in d.cases.items()}); stack+=[c.v for c in d.cases.values()]
    elif k=='ForkEvent': stack+=[f.v for f in d.forks]+[d.join.v]
    else: stack.append(d.nxt.v)
