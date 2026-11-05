#!/usr/bin/env python3
"""The English text the menus show, and the translation tables' state (runtime/ui/i18n.mm, runtime/ui/lang/<code>.inc).

    tools/menu_strings.py check                 what each table lacks (and has that nothing uses)
    tools/menu_strings.py missing <code> [out]  the English a table lacks, as JSON {"English": ""} (to fill in)
    tools/menu_strings.py merge <code> <json>   adds {"English": "translation"} to the table (rewritten in order)
    tools/menu_strings.py keys                  every English text, with where it comes from

What counts as menu text: L("..."), LN("...") and L_line("...") literals, the literals of option row calls (opt_*),
in runtime/ui and runtime/*.c; the Warp tab's places (runtime/ui/warp_data.h); the randomizer's progress lines
(tools/rando/las_headless.py stage()); and the spoiler log's lines (tools/rando/spoiler_view.py), split into the parts
i18n.mm's L_line puts together - "a  ->  b", "%s" patterns, "x (y)", "a: b", "a, b", "a / b". Spoiler logs in
rando/ under a data root (LARECOMP_DATA or the main checkout's) add the location names real seeds have.
"""
import ast
import collections
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG_DIR = os.path.join(ROOT, 'runtime', 'ui', 'lang')
CODES = ['ja', 'es', 'fr', 'de', 'it', 'nl', 'ru', 'ko', 'zh-Hans', 'zh-Hant', 'pt-BR']

# ---- C / C++ sources

LIT = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def unescape(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            if n in 'nt"\\\'?':
                out.append({'n': '\n', 't': '\t'}.get(n, n))
                i += 2
                continue
            if n == 'x':
                m = re.match(r'[0-9a-fA-F]+', s[i + 2:])
                out.append(chr(int(m.group(0), 16)))
                i += 2 + len(m.group(0))
                continue
        out.append(c)
        i += 1
    return ''.join(out).encode('latin-1', 'ignore').decode('utf-8', 'ignore') if any(ord(x) > 127 for x in out) else ''.join(out)


def strip_comments(src):
    # (keeps strings: a // inside one isn't a comment)
    out, i, n = [], 0, len(src)
    while i < n:
        if src.startswith('//', i):
            while i < n and src[i] != '\n':
                i += 1
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2)
            i = n if j < 0 else j + 2
        elif src[i] == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == '\\' else 1
            out.append(src[i:j + 1])
            i = j + 1
        elif src[i] == "'":
            j = i + 1
            while j < n and src[j] != "'":
                j += 2 if src[j] == '\\' else 1
            out.append(src[i:j + 1])
            i = j + 1
        else:
            out.append(src[i])
            i += 1
    return ''.join(out)


def call_args(src, at):
    """src[at] is '(': the text up to its matching ')'."""
    depth, i = 0, at
    while i < len(src):
        c = src[i]
        if c == '"':
            i += 1
            while i < len(src) and src[i] != '"':
                i += 2 if src[i] == '\\' else 1
        elif c == "'":
            i += 1
            while i < len(src) and src[i] != "'":
                i += 2 if src[i] == '\\' else 1
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return src[at + 1:i]
        i += 1
    return src[at + 1:]


def literals(args):
    """The string literals of a call's arguments, adjacent ones joined ("a" "b")."""
    out, last_end, cur = [], -1, None
    for m in LIT.finditer(args):
        if cur is not None and args[last_end:m.start()].strip() == '':
            cur += unescape(m.group(1))
        else:
            if cur is not None:
                out.append(cur)
            cur = unescape(m.group(1))
        last_end = m.end()
    if cur is not None:
        out.append(cur)
    return out


def shown(s):
    if s.startswith('##') or not re.search(r'[A-Za-z]', s):
        return None
    return s.split('###')[0] if '###' in s else s


CALL = re.compile(r'\b(LN|L_line|L|opt_[a-z_]+)\s*\(')


def source_keys():
    files = sorted(glob.glob(os.path.join(ROOT, 'runtime', 'ui', '*.mm')) + glob.glob(os.path.join(ROOT, 'runtime', 'ui', '*.h')) +
                   glob.glob(os.path.join(ROOT, 'runtime', '*.c')) + glob.glob(os.path.join(ROOT, 'runtime', 'hle', '*.c')))
    for f in files:
        name = os.path.relpath(f, ROOT)
        if name.endswith(('warp_data.h', 'i18n.h', 'option_rows.h')):
            continue
        src = strip_comments(open(f, encoding='utf-8').read())
        for m in CALL.finditer(src):
            fn = m.group(1)
            if fn.startswith('opt_') and src[m.start() - 5:m.start()].strip().endswith(('bool', 'void', 'int')):
                continue  # (a declaration / definition)
            args = call_args(src, m.end() - 1)
            for s in literals(args):
                s = shown(s)
                if s and name.endswith('.c') and fn == 'LN':
                    # (C text is filled in before the menus see it: they translate it with L_line, as a %s pattern)
                    s = re.sub(r'%[-0-9.]*(?:ll|l|z)?[dus]', '%s', s)
                if s:
                    yield s, name, fn == 'L_line'


# ---- the Warp tab (generated table)

def warp_keys():
    p = os.path.join(ROOT, 'runtime', 'ui', 'warp_data.h')
    src = open(p, encoding='utf-8').read()
    m = re.search(r'kWarpGroups\[\] = \{([^}]*)\}', src)
    for s in literals(m.group(1)):
        yield s, 'warp_data.h', False
    for m in re.finditer(r'\{\d, "((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)",', src):
        yield unescape(m.group(1)), 'warp_data.h', False
        yield unescape(m.group(2)), 'warp_data.h', True


# ---- Python: the randomizer's progress lines, the spoiler log

def fstring_template(node):
    parts = []
    for v in node.values:
        parts.append(v.value if isinstance(v, ast.Constant) else '%s')
    return ''.join(parts)


def py_strings(path, want):
    """Strings handed to calls named in want (f-strings as %s templates), and the out.append() lines."""
    tree = ast.parse(open(path, encoding='utf-8').read())
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call) or not node.args:
            continue
        fn = node.func.attr if isinstance(node.func, ast.Attribute) else getattr(node.func, 'id', '')
        if fn not in want:
            continue
        a = node.args[0]
        if isinstance(a, ast.Constant) and isinstance(a.value, str):
            yield a.value
        elif isinstance(a, ast.JoinedStr):
            yield fstring_template(a)
        elif isinstance(a, ast.BinOp) and isinstance(a.left, ast.Constant) and isinstance(a.left.value, str):
            yield a.left.value + '%s'  # ('Settings: ' + ', '.join(...))


MARKUP = re.compile(r'^(# |## |- |  )')


def spoiler_keys():
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'rando'))
    import spoiler_view as sv
    path = os.path.join(ROOT, 'tools', 'rando', 'spoiler_view.py')
    for s in py_strings(path, {'append'}):
        s = MARKUP.sub('', s)
        if s and re.search('[A-Za-z]', s) and s != '%s' and '  ->  ' not in s:
            yield s, 'spoiler_view.py', '%s' not in s
    yield "%s - enter through %s's entrance", 'spoiler_view.py', False  # (area_name: n += f"...")
    yield "%s's Soul", 'spoiler_view.py', False
    yield "%s' Soul", 'spoiler_view.py', False
    yield '%s:', 'spoiler_view.py', False  # ("- Mabe Village:")
    for d in (sv.AREAS, sv.DUNGEONS, sv.DPREFIX):
        for v in d.values():
            yield v, 'spoiler_view.py', True
    for k in sv.ITEMS:
        yield sv.pretty_item(k), 'spoiler_view.py', True
    for d in sv.DPREFIX:
        for kind in ('small-key', 'nightmare-key', 'compass', 'map', 'stone-beak'):
            yield sv.pretty_item(f'{d}-{kind}'), 'spoiler_view.py', True
    # every location the randomizer has (real seeds add the numbered digs, bushes and pots below)
    yml = os.path.join(ROOT, 'third_party', 'LAS-Randomizer', 'RandomizerCore', 'Data', 'locations.yml')
    if os.path.exists(yml):
        for m in re.finditer(r'^\s+-\s+([a-z0-9-]+)\s*$', open(yml).read(), re.M):
            yield sv.pretty_loc(m.group(1)), 'spoiler_view.py', True
    # settings ("Settings: " + the options that are on, dashes as spaces)
    for f in glob.glob(os.path.join(ROOT, 'runtime', 'ui', 'rando_tab.mm')) + glob.glob(os.path.join(ROOT, 'runtime', 'rando.c')):
        for m in re.finditer(r'"([a-z0-9]+(?:-[a-z0-9]+)+)"', open(f).read()):
            yield m.group(1).replace('-', ' '), 'spoiler_view.py', True
    roots = [os.environ.get('LARECOMP_DATA', ''), ROOT, os.path.join(ROOT, '..', '..', '..')]
    seen = set()
    for r in roots:
        for f in glob.glob(os.path.join(r, 'rando', '*', 'spoiler_view.txt')):
            f = os.path.realpath(f)
            if f in seen:
                continue
            seen.add(f)
            for line in open(f, encoding='utf-8', errors='ignore'):
                line = MARKUP.sub('', line.rstrip('\n'))
                if line.startswith('Seed '):
                    continue  # (the seed's own name)
                yield line, 'spoiler logs', True


def tracker_keys():
    """The check tracker's names (tools/rando/tracker.py writes them into rando/<seed>/tracker.tsv with check_names.py)."""
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'rando'))
    import check_names as cn
    import tracker
    for v in list(cn.NAMES.values()) + list(cn.KIND_WORD.values()) + list(cn.HOUSES.values()) + [p for p in tracker.PLACE if p]:
        yield v, 'tracker.py', True
    for w in ('north', 'south', 'east', 'west', 'north-west', 'north-east', 'south-west', 'south-east', 'centre', 'middle',
              'inside'):
        yield w, 'tracker.py', False
    for p in ('Trendy Game: prize %s', 'Pot (%s)', 'Pot in %s', '%s of 3'):
        yield p, 'tracker.py', False
    yml = os.path.join(ROOT, 'third_party', 'LAS-Randomizer', 'RandomizerCore', 'Data', 'locations.yml')
    if os.path.exists(yml):
        for m in re.finditer(r'^\s+-\s+([a-zA-Z0-9-]+)\s*$', open(yml).read(), re.M):
            yield cn.display_name(m.group(1)), 'tracker.py', True
    for r in (os.environ.get('LARECOMP_DATA', ''), ROOT, os.path.join(ROOT, '..', '..', '..')):
        for f in glob.glob(os.path.join(r, 'rando', '*', 'tracker.tsv')):
            for line in open(f, encoding='utf-8', errors='ignore'):
                c = line.rstrip('\n').split('\t')
                if c[0] == 'C':
                    for i in (11, 12):
                        if len(c) > i and c[i] != '-':
                            yield c[i], 'tracker logs', True


def rando_progress_keys():
    path = os.path.join(ROOT, 'tools', 'rando', 'las_headless.py')
    for s in py_strings(path, {'stage'}):
        yield s, 'las_headless.py', False
    yield 'Triforce hunt: %s', 'las_headless.py', False


# ---- splitting a line into the parts L_line translates (as i18n.mm's compose)

COORD = re.compile(r'^(\d{2}[A-Pa-p](\s+\d+)?|\d+|D\d|#\d+|[\d/.%+x-]+|\d+[A-Z]?)$')


def pat_re(p):
    return re.compile('^' + '(.+?)'.join(re.escape(x) for x in p.split('%s')) + '$', re.S)


def split_all(entries):
    """entries: (text, source, composed). The keys: whole texts, and composed ones in parts."""
    keys = collections.OrderedDict()
    known = set(s for s, src, composed in entries if not composed)
    pats = sorted({s for s in known if '%s' in s and not re.search(r'%[^s]', s)},
                  key=lambda p: -len(p.replace('%s', '')))
    pat_res = [(p, pat_re(p)) for p in pats]

    def add(s, src):
        if s and not COORD.match(s.strip()) and re.search('[A-Za-z]', s):
            keys.setdefault(s, src)

    def comp(s, src, d=0):
        if s in known or d > 8:
            return add(s, src)
        if '  ->  ' in s[1:]:
            return [comp(x, src, d + 1) for x in s.split('  ->  ')]
        for p, r in pat_res:
            m = r.match(s)
            if m and all(m.groups()):
                add(p, src)
                for g in m.groups():
                    comp(g, src, d + 1)
                return
        if s.endswith(')') and ' (' in s[1:]:
            i = s.rfind(' (')
            return comp(s[:i], src, d + 1), comp(s[i + 2:-1], src, d + 1)
        m = re.match(r'^(.*\S) \d+$', s)  # ("Bush, Mabe Village (north) 2": the second of that name)
        if m:
            return comp(m.group(1), src, d + 1)
        for sep in (': ', ', ', ' / '):
            if sep in s[1:]:
                return [comp(x, src, d + 1) for x in s.split(sep)]
        add(s, src)

    for s, src, composed in entries:
        if composed:
            comp(s, src)
        else:
            add(s, src)
    return keys


# the patterns that names put together in code have (the Warp tab's spots, the spoiler log's checks): L_line
# matches them, so they're texts of their own
PATTERNS = ['leaving %s', 'outside %s', 'Outside %s', 'Room %s', 'Warp hole: %s', 'Level %s', '%s #%s',
            'Bush %s', 'Dig %s', 'Pot %s', 'pot %s', 'pot %s in room %s', '%s: pot %s', '%s: pot %s in room %s',
            'rupee %s', 'Apple tree (%s), %s of 3', 'Trendy prize %s', 'Trendy figure %s', 'fairy %s', 'Step %s',
            'Seed %s']
# the separators L_line joins parts with: a language may have its own ("、" for ", ", " : " in French)
SEPARATORS = [', ', ': ']


def all_keys():
    entries = [(p, 'patterns (tools/menu_strings.py)', False) for p in PATTERNS] + list(source_keys()) + list(warp_keys()) + list(rando_progress_keys()) + list(spoiler_keys()) + \
        list(tracker_keys())
    keys = split_all(entries)
    for sep in SEPARATORS:
        keys[sep] = 'separators (tools/menu_strings.py)'
    return keys


# ---- the tables

def c_escape(s):
    return s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\t', '\\t')


ENTRY = re.compile(r'^\s*\{"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"\},')


def read_table(code):
    p = os.path.join(LANG_DIR, code + '.inc')
    t = collections.OrderedDict()
    if os.path.exists(p):
        for line in open(p, encoding='utf-8'):
            m = ENTRY.match(line)
            if m:
                t[unescape_utf8(m.group(1))] = unescape_utf8(m.group(2))
    return t


def unescape_utf8(s):
    return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), s)


def write_table(code, t, keys):
    p = os.path.join(LANG_DIR, code + '.inc')
    order = list(keys) + [k for k in t if k not in keys]
    out = [f'// {code}: the menus\' text by its English (runtime/ui/i18n.mm). tools/menu_strings.py merge adds to it.\n']
    last = None
    for k in order:
        if k not in t:
            continue
        src = keys.get(k, 'not used any more')
        if src != last:
            out.append(f'// {src}\n')
            last = src
        out.append(f'{{"{c_escape(k)}", "{c_escape(t[k])}"}},\n')
    open(p, 'w', encoding='utf-8').write(''.join(out))


def check_placeholders(en, tr):
    """The same printf specifiers in the same order (positional %1$s for %s patterns only)."""
    spec = re.compile(r'%(?:\d\$)?[-+#0]*\d*(?:\.\d+)?(?:ll|l|z|h)?[diouxXeEfgGcsp%]')
    a = [x for x in spec.findall(en)]
    b = [re.sub(r'^%\d\$', '%', x) for x in spec.findall(tr)]
    if '%' in en and re.search(r'%[^s]', en.replace('%%', '')):
        return a == b
    return sorted(a) == sorted(b)


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'check'
    if cmd == 'keys':
        for k, src in all_keys().items():
            print(f'{src}\t{json.dumps(k, ensure_ascii=False)}')
        return 0
    keys = all_keys()
    if cmd == 'check':
        bad = 0
        print(f'{len(keys)} texts')
        for code in CODES:
            t = read_table(code)
            miss = [k for k in keys if k not in t]
            unused = [k for k in t if k not in keys]
            wrong = [k for k in t if k in keys and not check_placeholders(k, t[k])]
            print(f'{code:8} {len(t):6} translated, {len(miss):5} missing, {len(unused):4} unused, {len(wrong)} with other % than the English')
            for k in wrong[:10]:
                print(f'    % mismatch: {k!r} -> {t[k]!r}')
            bad += len(miss) + len(wrong)
        return 1 if bad else 0
    if cmd == 'missing':
        code = sys.argv[2]
        t = read_table(code)
        miss = collections.OrderedDict((k, '') for k in keys if k not in t)
        data = json.dumps(miss, ensure_ascii=False, indent=1)
        if len(sys.argv) > 3:
            open(sys.argv[3], 'w', encoding='utf-8').write(data)
        else:
            print(data)
        return 0
    if cmd == 'merge':
        code, path = sys.argv[2], sys.argv[3]
        t = read_table(code)
        new = json.load(open(path, encoding='utf-8'))
        n = 0
        for k, v in new.items():
            if v:
                t[k] = v
                n += 1
        write_table(code, t, keys)
        print(f'{code}: {n} added, {len(t)} in all')
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main())
