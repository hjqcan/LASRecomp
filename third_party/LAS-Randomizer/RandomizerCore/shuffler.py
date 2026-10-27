from PySide6 import QtCore
from .Paths.randomizer_paths import IS_RUNNING_FROM_SOURCE
import RandomizerCore.spoiler as spoiler
import re
import copy
import random
import traceback

KEY_RINGS = {}  # [LAS Recomp] key rings: 'keyring-Dn' -> that dungeon's keys, counted as key-Dn (tools/rando/key_rings.py)

TEST_PLACEMENTS = { # example: testing specific items in chests
    # 'woods-crossing-cave-chest': 'bottle',
    # 'woods-south-chest': 'bottle',
    # 'beach-chest': 'bottle'
}


class ItemShuffler(QtCore.QThread):
    
    # sends signals to main thread when emitted
    give_placements = QtCore.Signal(tuple)
    is_done = QtCore.Signal()
    error = QtCore.Signal(str)

    
    # initialize
    def __init__(self, out_dir, seed, logic, settings, item_defs, logic_defs, parent=None):
        QtCore.QThread.__init__(self, parent)

        self.out_dir = out_dir
        self.seed = seed
        random.seed(self.seed)
        self.logic = logic
        self.settings = settings
        self.item_defs = item_defs
        self.logic_defs = logic_defs
        
        self.force_chests = ('zol-trap', 'stalfos-note')

        self.progress_value = 0
        self.thread_active = True
    
    
    # thread automatically starts the run method
    def run(self):
        # change some logic & item pool things before we create the list of vanilla locations
        self.logicSettingsChanges()

        # TEMPORARY CODE HERE to make it so that everything that isn't randomized yet is set to vanilla
        self.vanilla_locations = {k for k, v in self.logic_defs.items()
                            if v['type'] == 'item'
                            and v['subtype'] not in ('chest', 'boss', 'enemy', 'drop', 'npc', 'standing', 'overworld-statue', 'dungeon-statue',
                                                     'underwater')  # (LARecomp: tools/rando/underwater.py)
                            # (LARecomp: Shellsanity - tools/rando/seashell_spots.py; spots a bush / pot / dig check
                            # took over are no LAS locations any more)
                            and not (self.settings.get('shellsanity') and v['subtype'] in ('dig', 'hidden', 'bomb-hole'))}
        if not self.settings.get('trendsanity'):  # LAS Recomp: tools/rando/trendsanity.py shuffles them
            self.vanilla_locations.add('trendy-prize-1') # yoshi doll stays until trendy is properly shuffled
            self.vanilla_locations.add('trendy-prize-2')
            self.vanilla_locations.add('trendy-prize-3')
            self.vanilla_locations.add('trendy-prize-4')
            self.vanilla_locations.add('trendy-prize-5')
            self.vanilla_locations.add('trendy-prize-6')
            self.vanilla_locations.add('trendy-prize-final')
        if self.settings.get('shopsanity', 'off') != 'off':  # LAS Recomp: tools/rando/shopsanity.py
            self.vanilla_locations -= {k for k, v in self.logic_defs.items() if v.get('subtype') == 'shop'}

        # if blupsanity is not enabled, add the checks to the vanilla locations
        if not self.settings['blupsanity']:
            for i in range(28):
                self.vanilla_locations.add(f'D0-rupee-{i+1}')
        
        # make changes to the logic & item pool based on starting items
        self.addStartingItems()
        
        # add traps to the item pool
        self.addTraps()
        
        # create the new dungeon entrances
        self.shuffleDungeons()

        try:
            # Create a placement and spoiler log
            if self.thread_active:
                placements, rand_state = self.makeRandomizedPlacement()
            
            if self.thread_active:
                self.give_placements.emit((placements, rand_state))
        
        except Exception:
            er = traceback.format_exc()
            print(er)
            self.error.emit(er)
        
        finally: # regardless if there was an error or not, we want to tell the progress window that this thread has finished
            self.is_done.emit()
    
    
    # executed when the user attempts to close the progress window, sets thread_active to false so further code will be skipped
    def stop(self):
        self.thread_active = False
    
    
    def addStartingItems(self):
        """Edits the logic & item pool to support the starting items"""

        instruments = [
            'full-moon-cello',
            'conch-horn',
            'sea-lilys-bell',
            'surf-harp',
            'wind-marimba',
            'coral-triangle',
            'evening-calm-organ',
            'thunder-drum'
        ]
        
        start_instruments = []
        for i in [x for x in self.settings['starting-items'] if x in instruments]:
            self.settings['starting-items'].remove(i)
            start_instruments.append(i)
            instruments.remove(i)
        
        instrument_locations = [k for k,v in self.logic_defs.items()
            if v['type'] == 'item'
            and v['subtype'] == 'standing'
            and v['content'] in instruments
        ]

        # shuffle the instrument placements, and for each starting instrument, remove one and store the content
        random.shuffle(instrument_locations)
        num = self.settings['starting-instruments']
        num = num - len(start_instruments)
        if num <= 0:
            num = 0
        for i in range(num):
            inst = instrument_locations.pop(0)
            start_instruments.append(self.logic_defs[inst]['content'])
        
        # if randomized instruments is off, make sure the remaining instruments are in their vanilla locations
        if not self.settings['shuffle-instruments']:
            for inst in instrument_locations:
                self.vanilla_locations.add(inst)
        
        # if start with compass & map setting is enabled, adding them into the starting item setting
        dungeon_item_setting = self.settings['dungeon-items']
        if dungeon_item_setting != 'none':
            to_check = ()
            if dungeon_item_setting == 'mc':
                to_check = ('map', 'compass')
            elif dungeon_item_setting == 'mcb':
                to_check = ('map', 'compass', 'stone-beak')
            elif dungeon_item_setting == 'stone-beak':
                to_check = 'stone-beak'

            start_dungeon_items = [s for s in self.item_defs if s.startswith(to_check)]
            for e, item in enumerate(start_dungeon_items):
                self.logic_defs[f'starting-dungeon-item-{e + 1}'] = {  # add a location for each starting item
                    'type': 'item',
                    'subtype': 'npc',
                    'content': item,
                    'region': 'mabe',
                    'spoiler-region': 'mabe-village'
                }
                self.vanilla_locations.add(f'starting-dungeon-item-{e + 1}')
                self.item_defs['rupee-50']['quantity'] += 1  # since we add a location for each item, add a 50 rupee in the pool for each

        # add the starting instruments to the list of starting items since we are done with them
        self.settings['starting-items'].extend(start_instruments)

        # do the same for the remaining starting items
        for e, item in enumerate(self.settings['starting-items']):
            self.logic_defs[f'starting-item-{e+1}'] = { # add a location for each starting item
                'type': 'item',
                'subtype': 'npc',
                'content': item,
                'region': 'mabe',
                'spoiler-region': 'mabe-village'
            }
            self.vanilla_locations.add(f'starting-item-{e+1}')
            self.item_defs['rupee-50']['quantity'] += 1 # since we add a location for each item, add a 50 rupee in the pool for each


    def logicSettingsChanges(self):
        """Changes logic & item pool depending on settings"""
        
        # remove some settings specific stuff from the logic before creating the vanilla placements
        if not self.settings['owl-overworld-gifts']:
            owls = [k for k, v in self.logic_defs.items()
                if v['type'] == 'item'
                and v['subtype'] == 'overworld-statue']
            for owl in owls:
                del self.logic_defs[owl]
        else:
            self.item_defs['rupee-20']['quantity'] += 9 # 33 total owl statues, 9 in overworld
        
        if not self.settings['owl-dungeon-gifts']:
            owls = [k for k, v in self.logic_defs.items()
                if v['type'] == 'item'
                and v['subtype'] == 'dungeon-statue']
            for owl in owls:
                del self.logic_defs[owl]
        else:
            self.item_defs['rupee-20']['quantity'] += 24 # 33 total owl statues, 24 in dungeons

        # if shuffled bombs or powder is on, we want to consider them important instead of junk
        if self.settings['shuffle-bombs']:
            self.item_defs['bomb']['type'] = 'important'
        if self.settings['shuffle-powder']:
            self.item_defs['powder']['type'] = 'important'


    def addTraps(self):
        """Adds traps to the item pool. The amount varies based on the trap level & other settings"""

        if self.settings['traps'] == 'none':
            return
        
        traps = [k for k in self.item_defs # get all non zol-traps, not optimal but can add traps without editing the shuffler
                if k[-4:] == 'trap'
                and k[:3] != 'zol']
        
        num_traps = {'few': 3, 'several': 9, 'many': 17, 'trapsanity': 19}
        num_traps = num_traps[self.settings['traps']]

        # trapsanity replaces every single 5(blupsanity), 20, and 50 rupee with a trap, on top of the base 19 traps
        if num_traps == 19:
            if self.settings['blupsanity']:
                blues = self.item_defs['rupee-5']['quantity']
                self.item_defs['rupee-5']['quantity'] = 0
                for i in range(blues):
                    trap = random.choice(traps)
                    self.item_defs[trap]['quantity'] += 1
            
            trap_items = ('rupee-20', 'medicine')
            for item_key in trap_items:
                quantity = self.item_defs[item_key]['quantity']
                self.item_defs[item_key]['quantity'] = 0
                for i in range(quantity):
                    trap = random.choice(traps)
                    self.item_defs[trap]['quantity'] += 1
            
            # we always replace purple rupees with traps, so just set the number here to be edited later
            num_traps = self.item_defs['rupee-50']['quantity']
        
        # remove duplicate zol-traps in exchange for more money
        self.item_defs['zol-trap']['quantity'] -= 3
        self.item_defs['rupee-100']['quantity'] += 2 # +200 rupees
        self.item_defs['rupee-300']['quantity'] += 1 # +300 rupees

        # remove 50-rupees to make room for the traps (LAS Recomp: then the most plentiful junk rupees - souls, pets,
        # trendsanity and the Escape Bottle take 50-rupees first; a negative count dropped the traps)
        for i in range(num_traps):
            src = 'rupee-50' if self.item_defs['rupee-50']['quantity'] > 0 else \
                max(('rupee-20', 'rupee-100', 'rupee-300'), key=lambda k: self.item_defs[k]['quantity'])
            if self.item_defs[src]['quantity'] <= 0:
                break
            self.item_defs[src]['quantity'] -= 1
            trap = random.choice(traps)
            self.item_defs[trap]['quantity'] += 1


    def shuffleDungeons(self):
        """Randomizes the dungeon entrances, and swaps the logic for each"""

        dungeons = [
            'tail-cave', 'bottle-grotto', 'key-cavern', 'angler-tunnel', 'catfish-maw',
            'face-shrine', 'eagle-tower', 'turtle-rock', 'color-dungeon'
        ]
        self.dungeon_entrances = {}
        
        if self.settings['shuffle-dungeons']:
            target_dungeons = copy.deepcopy(dungeons)
            conditions = {}
            random.shuffle(target_dungeons)
            
            # keep track of new destinations and the condition of the old one
            for dungeon in dungeons:
                dun = target_dungeons.pop(0)
                self.dungeon_entrances[dungeon] = dun
                conditions[dun] = self.logic_defs[dungeon]['condition-basic']
            
            # edit the new dungeon condition to be the condition of the old one
            for c in conditions:
                self.logic_defs[c]['condition-basic'] = conditions[c]
        else:
            for dungeon in dungeons:
                self.dungeon_entrances[dungeon] = dungeon


    # SHUFFLE ITEMS
    def addAccess(self, access, new):
        if new in access:
            access[new] += 1
        else:
            access[new] = 1
        if new in KEY_RINGS:
            access['key-' + new[8:]] = access.get('key-' + new[8:], 0) + KEY_RINGS[new]
        return access
    
    
    def removeAccess(self, access, toRemove):
        if toRemove in access:
            access[toRemove] -= 1
            if access[toRemove] == 0:
                access.pop(toRemove)
            if toRemove in KEY_RINGS:
                k = 'key-' + toRemove[8:]
                access[k] = access.get(k, 0) - KEY_RINGS[toRemove]
                if access[k] <= 0:
                    access.pop(k)
        return access
    
    
    def hasAccess(self, access, key, amount=1):
        return key in access and access[key] >= amount
    
    
    def checkAccess(self, newCheck, access, logic):
        # get the name of the check without the parameter sometimes applied to enemy checks
        no_params = re.match('[a-zA-Z0-9-]+', newCheck).group(0)
        
        if logic == 'none': return True
        
        if self.logic_defs[no_params]['type'] == 'enemy':
            node = self.logic_defs[no_params]
            param = re.search('\\[([a-z]+)\\]', newCheck)
            if param and eval(self.parseCondition(node['condition-'+param.group(1)])):
                return True
            if eval(self.parseCondition(node['condition-basic'])):
                return True
            # LAS Recomp: an enemy's harder-logic ways count too (they were never read)
            for lv, allowed in (('advanced', ('advanced', 'glitched', 'hell')), ('glitched', ('glitched', 'hell')), ('hell', ('hell',))):
                if 'condition-' + lv in node and logic in allowed and eval(self.parseCondition(node['condition-' + lv])):
                    return True
            return False
        else:
            # For item and follower checks, see if you have access to the region. Otherwise, check on the conditions, if they exist
            region_access = self.hasAccess(access, self.logic_defs[newCheck]['region']) if (self.logic_defs[newCheck]['type'] in ('item', 'follower')) else True
            basic        = eval(self.parseCondition(self.logic_defs[newCheck]['condition-basic']))    if ('condition-basic' in self.logic_defs[newCheck]) else True
            advanced     = eval(self.parseCondition(self.logic_defs[newCheck]['condition-advanced'])) if (('condition-advanced' in self.logic_defs[newCheck]) and (logic in ('advanced', 'glitched', 'hell'))) else False
            glitched     = eval(self.parseCondition(self.logic_defs[newCheck]['condition-glitched'])) if (('condition-glitched' in self.logic_defs[newCheck]) and logic in ('glitched', 'hell')) else False
            hell        = eval(self.parseCondition(self.logic_defs[newCheck]['condition-hell']))    if (('condition-hell' in self.logic_defs[newCheck]) and logic == 'hell') else False
            return region_access and (basic or advanced or glitched or hell)
    
    
    def parseCondition(self, condition):
        func = condition
        func = re.sub('([a-zA-Z0-9\\-\\[\\]]+)(:(\\d+))?', lambda match: f'self.hasAccess(access, "{match.group(1)}", {match.group(3) or 1})', func)
        func = re.sub('\\|', 'or', func)
        func = re.sub('&', 'and', func)
        func = re.sub('!', 'not ', func)
        # print(func)
        return func
    
    
    def canReachLocation(self, to_reach, placements, starting_access, logic):
        """Given a set of item placements, and a starting item set, verify whether the location toReach is possible from the start of the game
        
        Parameters
        ----------
        toReach : str
            The name of the location to check
        placements : dict
            Full of <location : str, item : str> pairs to represent items placed in locations. Currently empty locations have the value None.
        startingAccess : dict
            A dict of <item : str, quantity : int> pairs. The starting item/access set to consider, i.e. all items not yet placed
        logic : str
            The logic to use in verifying. 'basic', 'advanced', or 'glitched'
        
        Returns True or False depending on whether access is eventually gained to toReach.
        """
        
        # If this location is disabled (force junk), consider it to be unreachable. This will result in no important items being placed there.
        if to_reach in placements['force-junk']:
            return False
        
        # if using no logic, we don't have to check if it's reachable, we just assume it is.
        if logic == 'none':
            return True
        
        access = starting_access.copy()
        access_added = True
        
        while access_added and self.thread_active:
            access_added = False
            for key in self.logic_defs:
                if self.thread_active:
                    if key not in access:
                        if self.checkAccess(key, access, logic):
                            access = self.addAccess(access, key)
                            access_added = True
                            # if this is the location we were looking for, we're done!
                            if key == to_reach:
                                return True
                            
                            # if we're looking at an item or follower location, at the item it holds, if it has one
                            if (self.logic_defs[key]['type'] in ['item', 'follower']) and placements[key] != None:
                                access = self.addAccess(access, placements[key])
                            
                            # if we're looking at an enemy, and we CAN kill it, then we can also kill it with access to pits or heavy objects, so add those too
                            if self.logic_defs[key]['type'] == 'enemy':
                                access = self.addAccess(access, key+'[pit]')
                                access = self.addAccess(access, key+'[heavy]')
                        
                        # if we can't do the thing, but it's an enemy, we might be able to use pits or heavy throwables, so check those cases independently
                        elif self.logic_defs[key]['type'] == 'enemy':
                            if 'condition-pit' in self.logic_defs[key] and not self.hasAccess(access, key+'[pit]'):
                                if self.checkAccess(key+'[pit]', access, logic):
                                    access = self.addAccess(access, key+'[pit]')
                                    access_added = True
                            if 'condition-heavy' in self.logic_defs[key] and not self.hasAccess(access, key+'[heavy]'):
                                if self.checkAccess(key+'[heavy]', access, logic):
                                    access = self.addAccess(access, key+'[heavy]')
                                    access_added = True
                else: break
            
        # If we get stuck and can't find any more locations to add, then we're stuck and can't reach toReach
        return False
    

    FILL_STUCK = 150  # LAS Recomp: undos before a room-shuffle fill gives up (see makeRandomizedPlacement)

    def _reachable(self, placements, starting_access, logic):
        """LAS Recomp: everything canReachLocation could reach from these items (its search, run to the end)."""
        if logic == 'none':
            return set(self.logic_defs)
        access = starting_access.copy()
        access_added = True
        while access_added and self.thread_active:
            access_added = False
            for key in self.logic_defs:
                if key not in access:
                    if self.checkAccess(key, access, logic):
                        access = self.addAccess(access, key)
                        access_added = True
                        if (self.logic_defs[key]['type'] in ['item', 'follower']) and placements[key] != None:
                            access = self.addAccess(access, placements[key])
                        if self.logic_defs[key]['type'] == 'enemy':
                            access = self.addAccess(access, key+'[pit]')
                            access = self.addAccess(access, key+'[heavy]')
                    elif self.logic_defs[key]['type'] == 'enemy':
                        if 'condition-pit' in self.logic_defs[key] and not self.hasAccess(access, key+'[pit]'):
                            if self.checkAccess(key+'[pit]', access, logic):
                                access = self.addAccess(access, key+'[pit]')
                                access_added = True
                        if 'condition-heavy' in self.logic_defs[key] and not self.hasAccess(access, key+'[heavy]'):
                            if self.checkAccess(key+'[heavy]', access, logic):
                                access = self.addAccess(access, key+'[heavy]')
                                access_added = True
        return set(access)

    # LAS Recomp item pacing. Seeds with many early checks (bushes, pots, digs, Shellsanity) used to hand out most
    # progress items in the first spheres: Mabe Village opened up the whole game. Now each one tries the locations in
    # a weighted random order (Efraimidis-Spirakis) that steers spheres 0-5 toward what they hold in a seed with
    # default settings (PACE_CURVE, two kinds of items counted apart), whatever the settings: a sphere gets what its
    # target still lacks, over the items of that kind left to place, spread over its empty locations - fewer chances
    # in a sphere with 10x the checks, more in one with few. Only the order changes: the reachability checks, the
    # undo logic and every dungeon / keysanity rule stay as they were, so nothing placeable becomes unplaceable.
    # A location's sphere is taken from the items placed so far (reachable with the starting items = 0, with what
    # those hold = 1, ...); the items that open the most are placed first (_pace_first) so that the others see them.
    # what spheres 0-5 hold in a seed with default settings (beta_5, 40 seeds, on average): the inventory items, and
    # the other progress items (dungeon keys, golden leaves, instruments, ...)
    PACE_MAJOR = ('sword', 'bracelet', 'feather', 'boots', 'hookshot', 'flippers', 'rod', 'bow', 'shovel', 'boomerang',
                  'song-ballad', 'song-soul', 'ocarina', 'powder', 'bomb')
    PACE_CURVE = {True: (0.3, 3.42, 1.88, 1.57, 1.05, 0.95), False: (0.3, 0.93, 1.48, 1.75, 1.38, 2.08)}

    def _paces(self, item):
        return self.item_defs.get(item, {}).get('type') == 'important' and not re.search(r'D[0-8]$', item)

    def _pace_spheres(self, placements, starting_access):
        """-> {item location: sphere} for the locations reachable with the items placed so far, up to the last
        sphere PACE_CURVE counts (cached until the item changes in a location whose item counts for those)."""
        cache = getattr(self, '_pace_cache', None)
        if cache and cache[2] == starting_access and all(placements.get(l) == i for l, i in cache[1].items()):
            return cache[0]
        sphere = {}
        access = starting_access.copy()
        s = 0
        while self.thread_active:
            found = []
            grew = True
            while grew:  # within a sphere: logic states settle, items wait for the next sphere
                grew = False
                for key, node in self.logic_defs.items():
                    if key in access:
                        continue
                    if self.checkAccess(key, access, self.logic):
                        access = self.addAccess(access, key)
                        grew = True
                        if node['type'] in ('item', 'follower'):
                            sphere[key] = s
                            if placements.get(key) is not None:
                                found.append(placements[key])
                        if node['type'] == 'enemy':
                            access = self.addAccess(access, key + '[pit]')
                            access = self.addAccess(access, key + '[heavy]')
                    elif node['type'] == 'enemy':
                        for v in ('pit', 'heavy'):
                            if 'condition-' + v in node and not self.hasAccess(access, key + f'[{v}]') and \
                                    self.checkAccess(key + f'[{v}]', access, self.logic):
                                access = self.addAccess(access, key + f'[{v}]')
                                grew = True
            if s == len(self.PACE_CURVE[True]) - 1:  # (what this sphere holds would only open a later one)
                break
            s += 1
            if not found:
                break
            for i in found:
                access = self.addAccess(access, i)
        self._pace_cache = (sphere, {l: placements.get(l) for l, t in sphere.items() if t < s}, starting_access)
        return sphere

    def _pace_reach(self, access, placements):
        """-> how many item locations are reachable with [access] and what the placed items give."""
        access = access.copy()
        n = 0
        grew = True
        while grew and self.thread_active:
            grew = False
            for key, node in self.logic_defs.items():
                if key in access:
                    continue
                if self.checkAccess(key, access, self.logic):
                    access = self.addAccess(access, key)
                    grew = True
                    if node['type'] in ('item', 'follower'):
                        n += node['type'] == 'item'
                        if placements.get(key) is not None:
                            access = self.addAccess(access, placements[key])
                    if node['type'] == 'enemy':
                        access = self.addAccess(access, key + '[pit]')
                        access = self.addAccess(access, key + '[heavy]')
                elif node['type'] == 'enemy':
                    for v in ('pit', 'heavy'):
                        if 'condition-' + v in node and not self.hasAccess(access, key + f'[{v}]') and \
                                self.checkAccess(key + f'[{v}]', access, self.logic):
                            access = self.addAccess(access, key + f'[{v}]')
                            grew = True
        return n

    def _pace_first(self, items, placements, access):
        """The major items that open the most locations (the shovel with digs, ...) are placed first, so that the
        spheres the other ones see already have them. Only the order of the major items changes."""
        at = [k for k, i in enumerate(items) if self._paces(i)]
        if not at:
            return
        full = self._pace_reach(access, placements)
        opens = {}
        for i in dict.fromkeys(items[k] for k in at):
            without = {k: v for k, v in access.items() if k != i}
            opens[i] = full - self._pace_reach(without, placements)
        order = sorted((items[k] for k in at), key=lambda i: -opens[i])  # (stable: ties keep the shuffled order)
        for k, i in zip(at, order):
            items[k] = i

    def _paced_order(self, locations, placements, starting_access, items):
        major = items[0] in self.PACE_MAJOR
        curve = self.PACE_CURVE[major]
        # the items placed after the major ones (seashells, trade items, rupees, ...) are taken as found from the
        # start: wherever they go, a location they open must not look later than it will be
        assumed = dict(starting_access)
        for i in items:
            if not self._paces(i) and not re.search(r'D[0-8]$', i):
                assumed[i] = assumed.get(i, 0) + 1
        sphere = self._pace_spheres(placements, assumed)
        late = len(curve)
        skip = set(placements['force-junk']) | set(self.settings.get('absent-locations', ())) | \
            set(self.settings.get('junk-locations', ()))
        free = [0] * (late + 1)  # empty locations per sphere (late: every later or unreached one)
        have = [0] * late  # items of this kind already in each sphere
        for l, t in sphere.items():
            i = placements.get(l)
            if t < late and i is not None and self._paces(i) and (i in self.PACE_MAJOR) == major and \
                    not l.startswith('starting-'):
                have[t] += 1
        tier = [min(sphere.get(l, late), late) for l in locations]
        for l, t in zip(locations, tier):
            if l not in skip:
                free[t] += 1
        left = sum(1 for i in items if self._paces(i) and (i in self.PACE_MAJOR) == major)  # (this one included)
        # each early sphere's chance: what the target still lacks up to that sphere (counted together with the
        # earlier ones: items fixed early, like the vanilla shop, leave less room after them), over the items of
        # this kind left to place
        lack = []
        for t in range(late):
            lack.append(max(0.0, sum(curve[:t + 1]) - sum(have[:t + 1]) - sum(lack)))
        share = [lack[t] / left if free[t] else 0.0 for t in range(late)]
        if sum(share) > 1:
            share = [x / sum(share) for x in share]
        share.append(max(0.0, 1 - sum(share)))
        weight = [max(x / free[t] if free[t] else 0.0, 1e-6) for t, x in enumerate(share)]
        top = max(weight)
        keys = [random.random() ** (top / weight[t]) for t in tier]
        return [l for _, l in sorted(zip(keys, locations), key=lambda kl: -kl[0])]

    def verifyFreeLocations(self, placements, starting_access, logic, goal):
        """LAS Recomp Triforce hunt: are at least [goal] empty item locations reachable with what's placed so far?"""
        locations = 0
        access = starting_access.copy()
        access_added = True
        while access_added and self.thread_active:
            access_added = False
            for key in self.logic_defs:
                if key in access or not self.thread_active:
                    continue
                if self.checkAccess(key, access, logic) or logic == 'none':
                    access = self.addAccess(access, key)
                    access_added = True
                    if self.logic_defs[key]['type'] in ['item', 'follower'] and placements.get(key) is not None:
                        access = self.addAccess(access, placements[key])
                    if self.logic_defs[key]['type'] == 'item' and placements.get(key) is None and \
                            key not in placements['force-junk'] and \
                            key not in self.settings.get('absent-locations', ()) and \
                            key not in self.settings.get('junk-locations', ()):
                        locations += 1
                    if self.logic_defs[key]['type'] == 'enemy':
                        access = self.addAccess(access, key + '[pit]')
                        access = self.addAccess(access, key + '[heavy]')
                elif self.logic_defs[key]['type'] == 'enemy':
                    for v in ('pit', 'heavy'):
                        if 'condition-' + v in self.logic_defs[key] and not self.hasAccess(access, key + f'[{v}]'):
                            if self.checkAccess(key + f'[{v}]', access, logic):
                                access = self.addAccess(access, key + f'[{v}]')
                                access_added = True
        return locations >= goal


    def verifySeashellsAttainable(self, placements, starting_access, logic, goal):
        # Verify, given the starting access to items, whether it is possible to get up to [goal] seashells. This includes already placed shells (vanilla) or 
        locations = []
        access = starting_access.copy()
        access_added = True
        
        # This check is run before random shells are placed, so any seashell come across during this runthrough
        # must have been forced vanilla. We don't want to count these directly in access.
        vanilla_seashells = 0
        
        while access_added and self.thread_active:
            access_added = False
            for key in self.logic_defs:
                if self.thread_active:
                    if key not in access:
                        if self.checkAccess(key, access, logic) or logic == 'none':
                            access = self.addAccess(access, key)
                            access_added = True
                            
                            # if we're looking at an item or follower location, at the item it holds, if it has one
                            if (self.logic_defs[key]['type'] in ['item', 'follower']) and placements[key] != None:
                                if placements[key] == 'seashell':
                                    vanilla_seashells += 1
                                else:
                                    access = self.addAccess(access, placements[key])
                            
                            if self.logic_defs[key]['type'] == 'item' and placements[key] == None:
                                locations.append(key)
                            
                            # if we're looking at an enemy, and we CAN kill it, then we can also kill it with access to pits or heavy objects, so add those too
                            if self.logic_defs[key]['type'] == 'enemy':
                                access = self.addAccess(access, key+'[pit]')
                                access = self.addAccess(access, key+'[heavy]')
                        # if we can't do the thing, but it's an enemy, we might be able to use pits or heavy throwables, so check those cases independently
                        elif self.logic_defs[key]['type'] == 'enemy':
                            if 'condition-pit' in self.logic_defs[key] and not self.hasAccess(access, key+'[pit]'):
                                if self.checkAccess(key+'[pit]', access, logic):
                                    access = self.addAccess(access, key+'[pit]')
                                    access_added = True
                            if 'condition-heavy' in self.logic_defs[key] and not self.hasAccess(access, key+'[heavy]'):
                                if self.checkAccess(key+'[heavy]', access, logic):
                                    access = self.addAccess(access, key+'[heavy]')
                                    access_added = True
                else: break
        
        #print(len(locations), numRandom, access['seashell'], goal)
        #print(access)
        return len(locations) + vanilla_seashells >= goal
    
    
    
    def makeRandomizedPlacement(self):
        """Creates and returns a a randomized placement of items, adhering to the logic"""
        
        verbose = bool(__import__("os").environ.get("LARECOMP_LAS_VERBOSE"))  # LAS Recomp: env; was: change this to True to print item placements to help debug

        if not set(self.settings['excluded-locations']).isdisjoint(self.vanilla_locations):
            print('Warning! Some locations set as disabled are unrandomized. These locations will not actually be considered out of logic.')
            self.settings['excluded-locations'] = [l for l in self.settings['excluded-locations'] if l not in self.vanilla_locations]
        
        # Ensure all excluded locations are actually location names
        self.settings['excluded-locations'] = {l for l in self.settings['excluded-locations'] if l in self.logic_defs and self.logic_defs[l]['type'] == 'item'}
        
        # Initialize the item and location lists, and the structures for tracking placements and access
        access = {}
        important_items = []
        seashell_items = []
        triforce_items = []  # (LAS Recomp Triforce hunt: tools/rando/triforce.py)
        good_items = []
        junk_items = []
        dungeon_items = []
        locations = []
        placements = {}
        
        vanilla_seashells = 0 # Keep track of how many seashells were forced into their vanilla locations

        placements['settings'] = self.settings
        placements['force-junk'] = self.settings['excluded-locations']
        placements['force-vanilla'] = self.vanilla_locations
        placements['starting-items'] = self.settings['starting-items']
        placements['dungeon-entrances'] = self.dungeon_entrances
        placements['indexes'] = {}
        
        indexes_available = {'seashell': list(range(50)),
                             'heart-piece': list(range(32)),
                             'heart-container': list(range(9)),
                             'bottle': list(range(3)),
                             'golden-leaf': list(range(5)),
                             'chamber-stone': [3, 4, 8, 10, 11, 12, 13, 20, 21, 22, 23, 24, 25, 26]}
        # LAS Recomp Triforce hunt: each piece its own index (the runtime counts pieces by their bits)
        if self.settings.get('triforce-placed'):
            indexes_available['triforce-piece'] = list(range(int(self.settings['triforce-placed'])))
        tri_needed = int(self.settings.get('triforce-needed', 0) or 0)
        
        for key in self.logic_defs:
            if not self.thread_active:
                break

            if self.logic_defs[key]['type'] == 'item':
                locations.append(key)
                placements[key] = None
                # access = self.addAccess(access, self.logic_defs[key]['content']) # we're going to assume the player starts with everything, then slowly loses things as they get placed into the wild
        
        # For each type of item in the item pool, add its quantity to the item lists
        for key in self.item_defs:
            if not self.thread_active:
                break

            # we're going to assume the player starts with everything, then slowly loses things as they get placed into the wild
            for i in range(self.item_defs[key]['quantity']):
                access = self.addAccess(access, key)

            if len(key) >= 2 and re.match(r"D[0-8]", key[-2:]):
                dungeon_items += [key] * self.item_defs[key]['quantity']
            elif self.item_defs[key]['type'] == 'important':
                important_items += [key] * self.item_defs[key]['quantity']
            elif self.item_defs[key]['type'] == 'trade':
                important_items += [key] * self.item_defs[key]['quantity']
            elif self.item_defs[key]['type'] == 'seashell':
                seashell_items += [key] * self.item_defs[key]['quantity']
            elif self.item_defs[key]['type'] == 'triforce':
                triforce_items += [key] * self.item_defs[key]['quantity']
            elif self.item_defs[key]['type'] == 'good':
                good_items += [key] * self.item_defs[key]['quantity']
            elif self.item_defs[key]['type'] == 'important-health':
                good_items += [key] * self.item_defs[key]['quantity']
            elif self.item_defs[key]['type'] == 'junk':
                junk_items += [key] * self.item_defs[key]['quantity']
            # else:
            #     if settings['dungeon-items'] == 'keys':
            #         if key.startswith(('key', 'nightmare')):
            #             important_items += [key] * self.item_defs[key]['quantity']
            #         else:
            #             dungeon_items += [key] * self.item_defs[key]['quantity']
            #     elif settings['dungeon-items'] == 'keys+mcb':
            #         important_items += [key] * self.item_defs[key]['quantity']

        # Add the settings into the access. This affects some logic like with fast trendy, free fishing, etc.
        settings_access = {setting: 1 for setting in self.settings if self.settings[setting] == True}
        access.update(settings_access)
        
        # Force the followers to be vanilla (for now)
        placements['moblin-cave'] = 'bow-wow'
        placements['rooster-statue'] = 'rooster'
        
        # Shuffle item and location lists
        random.shuffle(important_items)
        random.shuffle(seashell_items)
        random.shuffle(good_items)

        items = important_items + seashell_items + triforce_items + good_items + junk_items + dungeon_items
        # print(len(items))

        # Assign vanilla contents to forceVanilla locations
        for loc in self.vanilla_locations:
            if not self.thread_active:
                break

            # If it's not a valid location name, or already used for forceJunk, just ignore it
            if loc not in locations:
                continue
            
            # Place the defined vanilla content
            placements[loc] = self.logic_defs[loc]['content']

            if placements[loc] not in items:  # (LAS Recomp: say which, instead of a bare ValueError)
                raise ValueError(f"vanilla content {placements[loc]!r} of {loc} is not in the item pool")
            items.remove(placements[loc])
            access = self.removeAccess(access, placements[loc])
            locations.remove(loc)
            
            # If the item is one that needs an index, assign it its vanilla item index and remove that from the available indexes
            if placements[loc] in indexes_available:
                if placements[loc] == 'seashell':
                    vanilla_seashells += 1
                placements['indexes'][loc] = self.logic_defs[loc]['index']
                indexes_available[placements[loc]].remove(placements['indexes'][loc])
        
        # Next, assign dungeon items into their own dungeons
        # Some may have been placed already because of forceVanilla so we need to factor that in
        dungeons = ['color-dungeon', 'tail-cave', 'bottle-grotto', 'key-cavern', 'angler-tunnel', 'catfish-maw', 'face-shrine', 'eagle-tower', 'turtle-rock']
        for i in range(len(dungeons)):
            if not self.thread_active:
                break

            # if settings['dungeon-items'] == 'keys+mcb':
            #     break

            # if settings['dungeon-items'] == 'keys':
            #     item_pool = [s for s in item_pool if s.startswith(('map', 'compass', 'stone'))]
            dungeon_item_setting = self.settings['dungeon-items']
            if dungeon_item_setting != 'none':
                to_check = ()
                if dungeon_item_setting == 'mc':
                    to_check = ('map', 'compass')
                elif dungeon_item_setting == 'mcb':
                    to_check = ('map', 'compass', 'stone-beak')
                elif dungeon_item_setting == 'stone-beak':
                    to_check = 'stone-beak'
                item_pool = [s for s in items if len(s) >= 2 and s[-2:] == f'D{i}' and not s.startswith(to_check)]
            else:
                item_pool = [s for s in items if len(s) >= 2 and s[-2:] == f'D{i}']

            # LARecomp keysanity: these dungeon items go to the general pool (anywhere) instead of their own dungeon
            ks = self.settings.get('keysanity', 'off')
            ks_free = {'keys': ('key-', 'keyring-'), 'keys+nk': ('key-', 'keyring-', 'nightmare-key-'),
                       'all': ('key-', 'keyring-', 'nightmare-key-', 'map', 'compass', 'stone-beak')}.get(ks, ())
            if ks_free:
                item_pool = [s for s in item_pool if not s.startswith(ks_free)]

            # LAS Recomp dungeon room shuffle: a check belongs to the dungeon its room is in now (tools/rando/room_logic.py)
            dungeon_of = self.settings.get('dungeon-of', {})
            location_pool = [s for s in locations if dungeon_of.get(s, s[:2] if len(s) >= 2 else '') == f'D{i}']
            random.shuffle(location_pool)
            
            # Keep track of where we placed items. this is necessary to undo placements if we get stuck
            placement_tracker = []
            
            # Iterate through the dungeon items for that dungeon (inherently in order of nightmare key, small keys, stone beak, compass, map)
            while item_pool and self.thread_active:
                item = item_pool[0]
                if verbose: print(item+' -> ', end='')
                first_location_tried = location_pool[0]
                
                # Until we make a valid placement for this item
                valid_placement = False
                while not valid_placement and self.thread_active:
                    # Try placing the first item in the list in the first location
                    placements[location_pool[0]] = item
                    access = self.removeAccess(access, item)
                    
                    # Check if it's reachable there
                    valid_placement = self.canReachLocation(location_pool[0], placements, access, self.logic)
                    if not valid_placement:
                        # If it's not, take back the item and shift that location to the end of the list
                        access = self.addAccess(access, item)
                        placements[location_pool[0]] = None
                        location_pool.append(location_pool.pop(0))
                        if location_pool[0] == first_location_tried: 
                            # If we tried every location and none work, undo the previous placement and try putting it somewhere else. Also rerandomize the location list to ensure things aren't placed back in the same spots
                            if not placement_tracker:  # (LAS Recomp: say what, instead of an IndexError)
                                raise RuntimeError(f"can't place {item} in D{i}: none of {sorted(location_pool)} is "
                                                   "reachable without it" + ''.join(
                                    f"\n  {l}: {self.logic_defs[l].get('condition-basic')}" for l in sorted(location_pool)
                                    if verbose))
                            undo_location = placement_tracker.pop(0)
                            location_pool.append(undo_location)
                            locations.append(undo_location)
                            random.shuffle(location_pool)
                            items.insert(0, placements[undo_location])
                            item_pool.insert(0, placements[undo_location])
                            access = self.addAccess(access, placements[undo_location])
                            placements[undo_location] = None
                            if verbose: print("can't place")
                            break
                
                if valid_placement and self.thread_active:
                    # After we successfully made a valid placement, remove the item and location from consideration
                    items.remove(item)
                    item_pool.remove(item)
                    if verbose: print(location_pool[0])
                    locations.remove(location_pool[0])
                    placement_tracker.append(location_pool.pop(0))
        
        # LAS Recomp Triforce hunt (tools/rando/triforce.py): the pieces take the place of filler the vanilla
        # locations left, in this order (within a tier the most plentiful first): rupees (20 and up), traps, low-value
        # refills (bombs / arrows / powder, hearts, 5- and 1-rupees), then Chamber Stones (no logic needs them)
        tri_placed = int(self.settings.get('triforce-placed', 0) or 0)
        if tri_placed:
            def _tier(k):
                if self.item_defs[k]['type'] != 'junk':
                    return None
                base = k[4:] if k.startswith(('pot-', 'dig-')) else k
                if base.startswith('rupee-') and base not in ('rupee-5', 'rupee-1'):
                    return 0
                if k.endswith('-trap') and k != 'zol-trap':  # (zol traps: chest-only, placed apart)
                    return 1
                if base in ('rupee-5', 'rupee-1') or k in ('pot-heart', 'pot-arrows', 'pot-bombs', 'pot-powder', 'bomb',
                                                          'powder', 'arrow'):
                    return 2
                if k == 'chamber-stone':
                    return 3
                return None
            fill = [k for k in items if _tier(k) is not None]
            print(f'triforce: {len(fill)} filler items in the pool for {tri_placed} pieces', flush=True)
            if len(fill) < tri_placed:
                raise RuntimeError(f'Triforce hunt: {tri_placed} pieces need {tri_placed} filler items to replace, but '
                                   f'this seed has only {len(fill)}. Turn on Potsanity, Bushsanity or Digsanity, or '
                                   'choose fewer pieces.')
            for _ in range(tri_placed):
                k = min(sorted(set(fill)), key=lambda k: (_tier(k), -fill.count(k)))
                fill.remove(k)
                del items[len(items) - 1 - items[::-1].index(k)]
                access = self.removeAccess(access, k)
            at = next((n for n, k in enumerate(items) if self.item_defs[k]['type'] not in ('important', 'trade', 'seashell')),
                      len(items))
            items[at:at] = ['triforce-piece'] * tri_placed
            for _ in range(tri_placed):
                access = self.addAccess(access, 'triforce-piece')

        # LAS Recomp keysanity: the freed dungeon items are progression - place them with the important items, not after
        # all the junk (last, only the leftover spots were left: long undo loops)
        ks_free = {'keys': ('key-', 'keyring-'), 'keys+nk': ('key-', 'keyring-', 'nightmare-key-'),
                   'all': ('key-', 'keyring-', 'nightmare-key-', 'map', 'compass', 'stone-beak')}.get(
                       self.settings.get('keysanity', 'off'), ())
        if ks_free:
            freed = [s for s in items if s.startswith(ks_free) and s[-2:-1] == 'D' and s[-1].isdigit()]
            rest = [s for s in items if s not in freed]
            n_imp = next((k for k, s in enumerate(rest) if self.item_defs[s]['type'] != 'important'), len(rest))
            items = rest[:n_imp] + freed + rest[n_imp:]

        # Shuffle remaining locations
        random.shuffle(locations)
        
        # Place the traps and master stalfos note. These HAVE to go in chests so we need to do them first
        to_place = [s for s in items if s in self.force_chests]
        chests = [s for s in locations if self.logic_defs[s]['subtype'] == 'chest'
                  and s not in self.settings.get('absent-locations', ())  # (LAS Recomp: not in the game)
                  and s not in self.settings.get('junk-locations', ())]  # (LAS Recomp: Link may lose the way there)
        for item in to_place:
            if not self.thread_active:
                break

            if verbose: print(item+' -> ', end='')
            chest = chests.pop(0)
            placements[chest] = item
            items.remove(item)
            locations.remove(chest)
            if verbose: print(chests[0])
                
        # Keep track of where we placed items. this is necessary to undo placements if we get stuck
        placement_tracker = []

        # Test placements only if running from source
        if IS_RUNNING_FROM_SOURCE:
            for k,v in TEST_PLACEMENTS.items():
                placements[k] = v
                locations.remove(k)
                items.remove(v)
                placement_tracker.append(k)

        # Since Tarin is the only check available with no items, he has to have something out of a certain subset of items
        # Only do this if Tarin has no item placed, i.e. not forced to be vanilla
        if placements['tarin'] == None and self.thread_active:
            success = False
            while not success and self.thread_active:
                placements['tarin'] = items[0]
                success = (self.canReachLocation('can-shop', placements, settings_access, self.logic)
                        or self.canReachLocation(self.dungeon_entrances['tail-cave'], placements, settings_access, self.logic)
                        or self.canReachLocation('beach', placements, settings_access, self.logic)
                        # or self.canReachLocation('mamasha', placements, settings_access, logic)
                        or self.canReachLocation('ciao-ciao', placements, settings_access, self.logic)
                        or self.canReachLocation('marin', placements, settings_access, self.logic)
                        or self.canReachLocation('trendy', placements, settings_access, self.logic))
                
                if items[0] == "boots":
                    success = False
                
                if not success:
                    items.insert(items.index('seashell'), items[0])
                    items.pop(0)
                        
            placement_tracker.append('tarin')
            
            if verbose: print(items[0]+' -> tarin')
            access = self.removeAccess(access, items.pop(0))
            locations.remove('tarin')
        
        # LAS Recomp: one item per location - a pool short of items (a count gone below 0) leaves locations empty:
        # junk fills them (said, so the cause can be found)
        short = len(locations) - len(items)
        if short > 0:
            print(f'pool short by {short}: rupee-20 added', flush=True)
            items += ['rupee-20'] * short
        # LAS Recomp: one item per location - room shuffle key counts can leave the pool a few items over; drop junk
        extra = len(items) - len(locations)
        for k in range(len(items) - 1, -1, -1):
            if extra <= 0:
                break
            if self.item_defs[items[k]]['type'] == 'junk':
                if verbose: print(f'(pool over by {extra}: {items[k]} dropped)')
                items.pop(k)
                extra -= 1

        # Do a very similar process for all other items
        self._pace_first(items, placements, access)
        stuck = 0  # LAS Recomp: items that found no place so far (see the undo below)
        paced = False
        while items and self.thread_active:
            item = items[0]
            if verbose: print(item+' -> ', end='')
            # LAS Recomp item pacing: a major item tries the locations in an order weighted away from the earliest
            # spheres (_paced_order); the other items keep an unweighted order
            if self._paces(item):
                locations[:] = self._paced_order(locations, placements, settings_access, items)
                paced = True
            elif paced:
                random.shuffle(locations)
                paced = False
            first_location_tried = locations[0]
            reach = None  # LAS Recomp: every location reachable without this item, worked out once (_reachable)
            
            # Until we make a valid placement for this item
            valid_placement = False
            while not valid_placement and self.thread_active:
                # Try placing the first item in the list in the first location
                placements[locations[0]] = item
                access = self.removeAccess(access, item)
                
                # Check for item type restrictions, i.e. songs can't be standing items
                subtype = self.logic_defs[locations[0]]['subtype']
                if item in ('red-tunic', 'blue-tunic') and subtype in ('standing', 'hidden', 'dig', 'bomb-hole', 'drop', 'underwater', 'shop', 'enemy'):
                    valid_placement = False
                elif item in self.force_chests and subtype != 'chest':
                    valid_placement = False
                # special case where if the actual check on the 5 chests room is a zol-trap, nothing happens with the 5th chest
                elif item == 'zol-trap' and locations[0] == 'taltal-5-chest-puzzle':
                    valid_placement = False
                # LAS Recomp: rooms the dungeon room shuffle left out aren't in the game, and rooms Link may lose the
                # way to (junk-locations: Eagle's Tower's 3rd floor before / after the collapse) - only junk there
                elif (locations[0] in self.settings.get('absent-locations', ()) or
                      locations[0] in self.settings.get('junk-locations', ())) and \
                        self.item_defs[item]['type'] != 'junk':
                    valid_placement = False
                elif self.item_defs[item]['type'] in ('important', 'trade', 'seashell') or (
                        self.item_defs[item]['type'] == 'triforce' and
                        sum(1 for l in placement_tracker if placements[l] == item) < tri_needed):
                    # Check if it's reachable. We only need to do this check for important items!
                    # (LAS Recomp Triforce hunt: the needed pieces must be reachable, the extras can be anywhere)
                    # LAS Recomp: one search for every location instead of one per location tried - the same answer
                    # (a location is reachable when the closure from these items holds it; the item tried there
                    # only counts once it is reached), but a placement that fails everywhere took a full search per
                    # location (5600+ with digs: minutes per item; seeds without a starting sword timed out)
                    if reach is None:
                        placements[locations[0]] = None
                        reach = self._reachable(placements, access, self.logic)
                        placements[locations[0]] = item
                    valid_placement = locations[0] not in placements['force-junk'] and locations[0] in reach
                else:
                    valid_placement = True
                
                # If it wasn't valid, put it back and shift the first location to the end of the list
                if not valid_placement:
                    access = self.addAccess(access, item)
                    placements[locations[0]] = None
                    locations.append(locations.pop(0))
                    if locations[0] == first_location_tried: 
                        # If we tried every location and none work, undo the previous placement and try putting it somewhere else
                        # LAS Recomp dungeon room shuffle: a layout whose first spheres hold too few locations (no
                        # starting sword: shutters, entrances shuffled) can't be filled at all - the undos went on
                        # forever (seed N2HB8-...: the 5th golden leaf, 4 locations before Kanalet). After this many
                        # the caller plans the next layout (tools/rando/las_headless.py, FILL_STUCK)
                        stuck += 1
                        if stuck > self.FILL_STUCK and self.settings.get('dungeon-room-shuffle', 'off') != 'off':
                            raise RuntimeError(f'fill stuck: {item} found no place {stuck} times')
                        undo_location = placement_tracker.pop(0)
                        locations.append(undo_location)
                        random.shuffle(locations)
                        items.insert(0, placements[undo_location])
                        access = self.addAccess(access, placements[undo_location])
                        placements[undo_location] = None
                        if verbose: print("can't place")
                        break
            
            if valid_placement and self.thread_active:
                # After we successfully made a valid placement, remove the item and location from consideration
                if verbose: print(locations[0])
                
                items.pop(0)
                placement_tracker.append(locations.pop(0))
                
                # LAS Recomp Triforce hunt: before the first piece, make sure the needed count has room (free
                # reachable locations), as for the seashells below
                if tri_needed and len(items) > 0 and items[0] == 'triforce-piece' and item != 'triforce-piece':
                    if not self.verifyFreeLocations(placements, settings_access, self.logic, tri_needed):
                        if verbose: print('no room for triforce pieces')
                        undo_location = placement_tracker.pop(0)
                        locations.append(undo_location)
                        random.shuffle(locations)
                        items.insert(0, placements[undo_location])
                        access = self.addAccess(access, placements[undo_location])
                        placements[undo_location] = None

                # If we placed the last important item (so that afterward we start placing seashells), we want to ensure there's enough available locations to place a number of seashells required.
                # i.e., are there 40 locations reachable without getting the 40 and 50 rewards? If not, we haven't made a valid placement, so we have to go back and undo things until this is resolved.
                if item != 'seashell' and len(items) > 0 and items[0] == 'seashell':
                    if not ((self.verifySeashellsAttainable(placements, settings_access, self.logic, 5)) 
                    and (self.verifySeashellsAttainable(placements, settings_access, self.logic, 15))
                    and (self.verifySeashellsAttainable(placements, settings_access, self.logic, 30))
                    and (self.verifySeashellsAttainable(placements, settings_access, self.logic, 40))
                    and (self.verifySeashellsAttainable(placements, settings_access, self.logic, 50))):
                        if verbose: 
                            print('no room for shells')
                            #print(placements)
                        undo_location = placement_tracker.pop(0)
                        locations.append(undo_location)
                        random.shuffle(locations)
                        items.insert(0, placements[undo_location])
                        access = self.addAccess(access, placements[undo_location])
                        placements[undo_location] = None
        
        # Now assign all non-vanilla indexes
        locs = [l for l in placement_tracker if placements[l] in indexes_available]
        for loc in locs:
            placements['indexes'][loc] = indexes_available[placements[loc]].pop(0)
        
        # LARecomp keysanity: a dungeon item outside its own dungeon carries its dungeon (upstream dev convention:
        # 0-7 = D1-D8, 9 = Color Dungeon); the runtime AddItem hook (runtime/hle/keysanity.c) credits that dungeon.
        # Items in their own dungeon keep index -1 (the game adds them to the current dungeon).
        for key in [k for k in placements if k not in ('settings', 'indexes') and isinstance(placements.get(k), str)
                    and placements[k].startswith(('compass', 'map', 'stone-beak', 'key-', 'nightmare-key'))
                    and placements[k][-2:-1] == 'D' and placements[k][-1].isdigit()]:
            n = int(placements[key][-1])
            if key.startswith('starting-'):  # (starting dungeon items: given on entering their dungeon)
                continue
            # (dungeon room shuffle: the dungeon the check's room is in now, not the one its name says)
            if self.settings.get('dungeon-of', {}).get(key, key[:2]) == f'D{n}':
                continue
            placements['indexes'][key] = 9 if n == 0 else n - 1
        # dungeon_indexes = [k for k in placement_tracker if placements[k].startswith(('compass', 'map', 'stone', 'key', 'nightmare'))]
        # for key in dungeon_indexes:
        #     # if key in placements['indexes']:
        #     #     continue
        #     level = int(placements[key][-1])
        #     level -= 1
        #     if level == -1:
        #         level = 9
        #     placements['indexes'][key] = level
        
        if self.thread_active and placements['settings']['create-spoiler']:
            spoiler.generateSpoilerLog(placements, self.logic_defs, self.out_dir, self.seed)
        
        return placements, random.getstate()
