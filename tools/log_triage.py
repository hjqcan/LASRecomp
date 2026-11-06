#!/usr/bin/env python3
"""Log triage for GitHub issues: a short summary of the lasrecomp.log files attached to an issue.

.github/workflows/log_triage.yml runs it when an issue is opened / edited or a comment is added. It reads the issue
and its comments through the GitHub API, downloads the attached logs (github.com/user-attachments/files/... links
whose file name starts with "lasrecomp", at most MAX_BYTES each), and keeps ONE bot comment per issue up to date
with a summary of the newest logs, plus a "log: crash" / "log: device-lost" / "log: ok" label for the newest one.
Shader variants the shipped list lacks ("[shader] record ..." lines) are listed in the comment, folded, and the issue
gets the "log: new shaders" label, so they can be merged into runtime/nvn/shader_variants.txt
(tools/shader_list_merge.py).

Everything in an issue and in a log is untrusted data: it is only ever parsed here, never run or handed to a shell.
Excerpts quoted in the comment are cut short, put in code spans, and have file paths reduced to their last part.

usage:
  log_triage.py parse <log>...      print the comment for local log files (no network)
  log_triage.py issue               the workflow: env GITHUB_TOKEN, GITHUB_REPOSITORY, ISSUE_NUMBER,
                                    DRY_RUN=1 prints the comment instead of posting it and leaves labels alone
Standard library only.
"""
import json
import os
import re
import statistics
import sys
import urllib.error
import urllib.parse
import urllib.request

MARKER = "<!-- lasrecomp-log-triage -->"
MAX_BYTES = 20 * 1024 * 1024  # per log
MAX_LOGS = 3                  # newest logs summarised per comment
LABELS = {"crash": ("log: crash", "b60205", "Newest attached log ends in a crash or hang"),
          "device-lost": ("log: device-lost", "d93f0b", "Newest attached log ends with the GPU device lost"),
          "ok": ("log: ok", "0e8a16", "Newest attached log ends with a clean exit")}
SHADER_LABEL = ("log: new shaders", "5319e7", "Attached log has shader variants the shipped list lacks")
MAX_RECORDS = 500             # shader records listed per comment
ATTACH_RE = re.compile(r"https://github\.com/user-attachments/files/\d+/[A-Za-z0-9._%+-]+")
REDIRECT_HOSTS = ("objects.githubusercontent.com",)  # (where github.com/user-attachments/files/... redirects)


# ------------------------------------------------------------------ parsing a log

def clean(s, n=160):
    """A log excerpt made safe to quote: paths cut to their last part, no backticks / control characters, short."""
    s = re.sub(r"[A-Za-z]:[\\/][^\s:;,)\"']*[\\/]([^\s\\/:;,)\"']+)", r"\1", s)        # C:\Users\x\...\file
    s = re.sub(r"(?<![\w.])/(?:[^\s/:;,)\"']+/)+([^\s/:;,)\"']+)", r"\1", s)           # /home/x/.../file
    s = "".join(c if c.isprintable() else " " for c in s).replace("`", "'").strip()
    return s if len(s) <= n else s[:n - 3] + "..."


def code(s, n=160):
    return "`" + clean(s, n) + "`"


FPS_RE = re.compile(r"^\[(vk|metal)\] t=([\d.]+) frame (\d+) ([\d.]+) fps max (\d+)ms level (\S+)")
BUILD_RE = re.compile(r"LAS[ ._-]?Recomp[ ._-]((?:beta_\d+(?:[._-]\d+)?)|(?:v\d+\.\d+\.\d+)|(?:test-[A-Za-z0-9]+(?:-[A-Za-z0-9]+)*?))"
                      r"(?:-(windows|macos|linux|android)\b|[\\/\s]|$)", re.I)
# runtime/main.c log_startup_info: "[info] LAS Recomp beta_5.4, Windows 11 (10.0.26300, x64), AMD Ryzen 5 7500F
# 6-Core Processor 6 cores / 12 threads, 31.7 GB RAM, zip" - the build; the OS, CPU and RAM; the package when known
INFO_RE = re.compile(r"^\[info\] LAS Recomp ([^\s,]+), (.+?(?:GB RAM|RAM unknown))(?:, (.+))?$")
MS_RE = re.compile(r"([\d.]+)\s*ms")
# runtime/nvn/shader_seen.c shader_seen_note: "[shader] record G vs=... fs=... f=1 ..." (hashes and pipeline state only)
RECORD_RE = re.compile(r"^\[shader\] record ([GC] [A-Za-z0-9=,:./+ -]{1,400})$")
# The game files check (runtime/ui/launcher.mm on the start screen, runtime/services/fs.c when the game starts; the
# same check, so a run logs it twice): files that aren't the game's (the first 20 named), the game's files missing or
# of another size, and a byte total other than the version's (issues #391, #553)
ROMFS_RE = re.compile(r"^\[(launcher|fs)\] romfs: (?:not one of the game's files: (.+) \(\d+ bytes\)|(\d+) more files "
                      r"that aren't the game's|(\d+) of the game's files missing or changed|(\d+) bytes in \d+ files, "
                      r"this version's are (\d+): damaged or changed game files)$")

# notable lines: (label, regex); each is reported once, with its count and first line
NOTABLE = [
    ("heap as driver memory failed", re.compile(r"heap as driver memory: (?!.*shared as before$).*(fail|not allowed|no way|can't|no cached|would be)")),
    ("heap as driver memory fell back", re.compile(r"heap as driver memory: .*shared as before")),
    ("guest heap imported in chunks (not enough GPU memory for one import)", re.compile(r"\[vk\] guest heap: .*(ran out of memory|imported in chunks)")),
    ("no BC texture formats", re.compile(r"no BC texture formats")),
    ("GPU device fault", re.compile(r"\[vk\] device fault")),
    ("shaders not found or failed in preparation", re.compile(r"\[shader\] prepared \d+ \+ \d+ seen before, not found [1-9]|\[shader\] prepared .*\(([1-9]\d*) failed")),
    ("shader translation failed", re.compile(r"translate failed|pipeline failed|\[shader\] stale program")),
    ("frame pacing switched to the game's 30 fps mode", re.compile(r"\[pace\] frames don't fit")),
    ("slow shader builds (stutter)", re.compile(r"\[shader\] slow build")),
    ("Metal command buffer error", re.compile(r"\[metal\] command buffer error")),
    ("drawable waits (Metal)", re.compile(r"\[metal\] nextDrawable blocked")),
    ("settings file could not be written", re.compile(r"\[settings\] cannot write")),
    ("developer overrides (LARECOMP_*) in use", re.compile(r"\(LARECOMP_[A-Z_]+\)")),
]


def parse(text):
    lines = text.splitlines()
    r = {"backend": None, "gpu": None, "gpus": [], "api": None, "driver": None, "os": None, "cpu": None,
         "system": None, "build": None, "settings": None, "swapchain": None, "integrated": False, "fps": [], "slow": [],
         "notable": {}, "records": [], "seed": None, "end": None, "end_line": None, "lines": len(lines), "romfs": None}
    romfs = {}  # per check ("launcher" / "fs"): what it found (each file once: a check can run again)
    fatal = exc = crash_head = None
    devlost = hang = closed = False
    for i, ln in enumerate(lines):
        if m := INFO_RE.match(ln):  # (the build's own name: wins over a folder name in the data path)
            r["build"] = m.group(1) + (f" ({m.group(3)})" if m.group(3) else "")
            r["system"] = m.group(2)
        elif ln.startswith("[paths] data: "):
            p = ln[14:]
            r["os"] = ("Windows" if re.match(r"[A-Za-z]:\\", p) else "Android" if p.startswith("/storage/")
                       else "macOS" if p.startswith("/Users/") or "Library/Application Support" in p
                       else "Linux" if p.startswith("/") else None)
            m = BUILD_RE.search(p)
            if m and not r["build"]: r["build"] = m.group(1) + (f" ({m.group(2).lower()})" if m.group(2) else "")
        elif ln.startswith("ANDROID:"):
            r["os"] = "Android"
        elif not r["build"] and re.match(r"^\[(version|build)\] ", ln):
            r["build"] = ln.split("] ", 1)[1]
        elif m := re.match(r"^\[vk\] gpu \d+: (.+) \(api ([\d.]+)\)", ln):
            r["gpus"].append(m.group(1))
            r["backend"] = "Vulkan"
        elif m := re.match(r"^\[vk\] using (.+)", ln):
            r["gpu"] = m.group(1)
            r["backend"] = "Vulkan"
            for g in lines[:i]:
                mm = re.match(r"^\[vk\] gpu \d+: (.+) \(api ([\d.]+)\)", g)
                if mm and mm.group(1) == r["gpu"]: r["api"] = mm.group(2)
        elif m := re.match(r"^\[vk\] driver (.+?)(?:; |$)", ln):  # ("[vk] driver NVIDIA 617.42; graphics pipeline library: ...")
            r["driver"] = m.group(1)
        elif m := re.match(r"^\[metal\] (.+), heap buffer \d+ MB", ln):
            r["gpu"], r["backend"] = m.group(1), "Metal"
        elif ln.startswith("[settings] ") and " GPU: upscaler " in ln:
            r["settings"] = ln.split(" GPU: ", 1)[1]
            r["integrated"] = ln.startswith("[settings] integrated")
        elif m := re.match(r"^\[vk\] swapchain (\d+x\d+), (\d+) images, (\w+)", ln):
            r["swapchain"] = f"{m.group(1)}, {m.group(3)}"
        elif m := re.match(r"^\[pcore\] CPU (.+?) \((\d+) logical processors", ln):
            r["cpu"] = f"{m.group(2)} logical processors" + (", hybrid" if m.group(1).startswith("hybrid") else "")
        elif m := re.match(r"^\[cpu\] fastest core \S+ \((\d+) MHz\)", ln):
            r["cpu"] = f"fastest core {m.group(1)} MHz"
        elif m := FPS_RE.match(ln):
            r["fps"].append((float(m.group(2)), float(m.group(4)), int(m.group(5)), m.group(6)))
        elif ln.startswith("[slow]"):
            r["slow"].append(ln)
        elif ln.startswith("FATAL: ") and not fatal:
            fatal = ln[7:]
            if "device lost" in ln or re.search(r"failed: -4 \(", ln): devlost = True
        elif ln.startswith("*** ") and not exc:
            exc = ln[4:]
        elif ln.startswith("[crash] "):
            crash_head = ln[8:]
        elif ln.startswith("[vk] last GPU operations before the device loss") or "VK_ERROR_DEVICE_LOST" in ln:
            devlost = True
        elif ln.startswith("[hang] ====="):
            hang = True
        elif m := re.match(r'^\[rando\] seed "([^"]+)" active(?:, seed ID (\S+))?', ln):
            r["seed"] = m.group(1) + (f" (seed ID {m.group(2)})" if m.group(2) else "")
        elif m := RECORD_RE.match(ln.rstrip()):
            r["records"].append(m.group(1).strip())
        elif ln.strip() == "window closed":
            closed = True
        elif m := ROMFS_RE.match(ln.rstrip()):
            c = romfs.setdefault(m.group(1), {"odd": [], "more": 0, "missing": 0, "bytes": None})
            if m.group(2) is not None:
                if m.group(2) not in c["odd"]: c["odd"].append(m.group(2))
            elif m.group(3): c["more"] = int(m.group(3))
            elif m.group(4): c["missing"] = int(m.group(4))
            else: c["bytes"] = (int(m.group(5)), int(m.group(6)))
        for label, rx in NOTABLE:
            if rx.search(ln):
                n, first = r["notable"].get(label, (0, ln))
                r["notable"][label] = (n + 1, first)
    r["romfs"] = romfs.get("fs") or romfs.get("launcher")  # (the game's own check when it got that far)
    if r["integrated"] or any("integrated GPU" in x and "not an integrated GPU" not in x for x in lines[:200]):
        r["integrated"] = True
    if devlost:
        r["end"], r["end_line"] = "device-lost", fatal or "the GPU device was lost"
    elif fatal or exc or crash_head:
        r["end"], r["end_line"] = "crash", fatal or exc or crash_head
        r["crash_head"] = crash_head
    elif hang:
        r["end"], r["end_line"] = "hang", next((x for x in lines if x.startswith("[hang] =====") and x.strip() != "[hang] ====="), None)
    elif closed:
        r["end"] = "ok"
    else:
        r["end"] = "unknown"
    return r


def fps_summary(fps):
    if not fps: return None
    vals = [f for _, f, _, _ in fps]
    span = fps[-1][0] - fps[0][0]
    worst_ms = max(m for _, _, m, _ in fps)
    by_level = {}
    for _, f, _, lv in fps: by_level.setdefault(lv, []).append(f)
    lows = sorted(((statistics.median(v), lv) for lv, v in by_level.items() if len(v) >= 2 and lv != "-"))[:2]
    s = (f"{len(fps)} samples over {span:.0f} s: median {statistics.median(vals):.1f} fps, lowest {min(vals):.1f} fps, "
         f"longest frame {worst_ms} ms")
    if len(by_level) > 1 and lows:
        s += "; slowest areas: " + ", ".join(f"{code(lv, 40)} {f:.1f} fps" for f, lv in lows)
    return s


def summarise(r):
    out = []
    out.append(f"- **Build:** {code(r['build']) if r['build'] else 'not in the log'}")
    sysl = [r["os"] or "OS not in the log"]
    if r["cpu"]: sysl.append(r["cpu"])
    out.append("- **System:** " + (code(r["system"]) if r["system"] else ", ".join(sysl)))
    if r["gpu"]:
        g = code(r["gpu"], 80) + (" (integrated)" if r["integrated"] else "")
        if r["api"]: g += f", Vulkan {r['api']}"
        if r["driver"]: g += f", driver {code(r['driver'], 60)}"
        if len(r["gpus"]) > 1: g += f" ({len(r['gpus'])} GPUs found)"
        out.append(f"- **GPU:** {g}")
    else:
        out.append("- **GPU:** not in the log")
    out.append(f"- **Backend:** {r['backend'] or 'not in the log'}")
    if r["seed"]: out.append(f"- **Randomizer seed:** {code(r['seed'], 120)}")
    if r["romfs"]:
        c, gf = r["romfs"], []
        if c["missing"]: gf.append(f"{c['missing']} of the game's files missing or changed")
        odd = len(c["odd"]) + c["more"]
        if odd:
            gf.append(f"{odd} file{' that isn' if odd == 1 else 's that aren'}'t the game's"
                      + (f" ({'first: ' if odd > 1 else ''}{code(c['odd'][0], 80)})" if c["odd"] else ""))
        if c["bytes"]: gf.append(f"{c['bytes'][0]} bytes, this version's are {c['bytes'][1]}")
        # (only extra files: not a damaged copy, the start screen doesn't warn about them since #553)
        out.append("- **Game files:** " + ("damaged or changed: " if c["missing"] or c["bytes"] else "") + "; ".join(gf))
    st = []
    if r["settings"]: st.append(code(r["settings"], 120))
    if r["swapchain"]: st.append(f"window {r['swapchain']}")
    out.append("- **Graphics settings:** " + ("; ".join(st) if st else "not in the log"))
    end = {"ok": "clean exit (window closed)",
           "crash": "crash" + (f" ({clean(r.get('crash_head'), 80)})" if r.get("crash_head") else "") + f": {code(r['end_line'] or '')}",
           "device-lost": f"the GPU device was lost: {code(r['end_line'] or '')}",
           "hang": f"hang (the hang report is in the log){': ' + code(r['end_line']) if r['end_line'] else ''}",
           "unknown": "the log stops without an exit line (closed from outside, frozen, or still running when copied)"}[r["end"]]
    out.append(f"- **How the run ended:** {end}")
    fs = fps_summary(r["fps"])
    out.append(f"- **Frame rate:** {fs or 'no frame rate lines'}")
    if r["slow"]:
        def ms(x):
            m = MS_RE.search(x)
            return float(m.group(1)) if m else 0.0
        worst = max(r["slow"], key=ms)
        out.append(f"- **Slow frames:** {len(r['slow'])} `[slow]` lines; worst: {code(worst, 140)}")
    if r["records"]:
        g = sum(1 for x in r["records"] if x[0] == "G")
        out.append(f"- **New shader variants:** {len(r['records'])} the shipped list lacks ({g} pipelines, "
                   f"{len(r['records']) - g} compute)")
    if r["notable"]:
        out.append("- **Notable lines:**")
        for label, (n, first) in r["notable"].items():
            out.append(f"  - {label}{f' ({n}x)' if n > 1 else ''}: {code(first, 140)}")
    return "\n".join(out)


def shader_records(entries):
    """every shader record in the summarised logs, once, sorted (the shipped list's order)"""
    return sorted({x for _, r in entries if isinstance(r, dict) for x in r["records"]})


def render(entries, total):
    """entries: newest first, (source description, parse result or error string)"""
    out = [MARKER,
           "This is an automatic summary of the LAS Recomp log files attached to this issue. It only reads the logs, "
           "so a person will still look at the details.",
           "",
           "For a new report, please attach `lasrecomp.log` from LAS Recomp's data folder (it always holds the latest "
           "run; `lasrecomp-backup-*.log` there are the earlier runs)."]
    for k, (src, r) in enumerate(entries):
        out += ["", f"#### {'Newest log' if k == 0 else 'Earlier log'}: {src}"]
        out.append(summarise(r) if isinstance(r, dict) else f"- Could not read this log: {clean(r, 120)}")
    recs = shader_records(entries)
    if recs:
        out += ["", "<details><summary>Shader variants the shipped list lacks (" + str(len(recs)) + ")</summary>", "",
                "```"] + recs[:MAX_RECORDS] + ["```", ""]
        if len(recs) > MAX_RECORDS: out.append(f"({len(recs) - MAX_RECORDS} more in the logs.)")
        out.append("</details>")
    if total > len(entries):
        out += ["", f"({total - len(entries)} earlier log{'s' if total - len(entries) > 1 else ''} on this issue not summarised.)"]
    return "\n".join(out) + "\n"


# ------------------------------------------------------------------ GitHub

class Api:
    def __init__(self, token, repo):
        self.token, self.repo = token, repo

    def call(self, method, path, body=None):
        url = path if path.startswith("https://") else f"https://api.github.com/repos/{self.repo}/{path}"
        req = urllib.request.Request(url, method=method, data=json.dumps(body).encode() if body is not None else None,
                                     headers={"Authorization": f"Bearer {self.token}", "Accept": "application/vnd.github+json",
                                              "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "lasrecomp-log-triage"})
        with urllib.request.urlopen(req, timeout=30) as resp:
            data = resp.read()
            link = resp.headers.get("Link", "")
        return (json.loads(data) if data else None), link

    def pages(self, path):
        out, url = [], f"https://api.github.com/repos/{self.repo}/{path}"
        for _ in range(50):
            data, link = self.call("GET", url)
            out += data
            m = re.search(r'<([^>]+)>;\s*rel="next"', link)
            if not m: break
            url = m.group(1)
        return out


class _Redirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code_, msg, headers, newurl):
        host = urllib.parse.urlparse(newurl).hostname or ""
        if urllib.parse.urlparse(newurl).scheme != "https" or host not in REDIRECT_HOSTS:
            raise urllib.error.HTTPError(newurl, code_, f"redirect to {host} not allowed", headers, fp)
        return super().redirect_request(req, fp, code_, msg, headers, newurl)


def download(url):
    if not ATTACH_RE.fullmatch(url): raise ValueError("not a GitHub attachment link")
    opener = urllib.request.build_opener(_Redirects)
    req = urllib.request.Request(url, headers={"User-Agent": "lasrecomp-log-triage"})  # (no token: public files)
    with opener.open(req, timeout=60) as resp:
        if int(resp.headers.get("Content-Length") or 0) > MAX_BYTES: raise ValueError("log larger than 20 MB")
        data = resp.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES: raise ValueError("log larger than 20 MB")
    return data.decode("utf-8", "replace")


def attachments(text):
    """lasrecomp*.log / *.txt attachment links in a body, in order, without repeats"""
    seen, out = set(), []
    for u in ATTACH_RE.findall(text or ""):
        name = urllib.parse.unquote(u.rsplit("/", 1)[1]).lower()
        if name.startswith("lasrecomp") and name.endswith((".log", ".txt")) and u not in seen:
            seen.add(u)
            out.append(u)
    return out


def run_issue():
    token, repo = os.environ["GITHUB_TOKEN"], os.environ["GITHUB_REPOSITORY"]
    num = os.environ.get("ISSUE_NUMBER", "")
    if not re.fullmatch(r"[1-9]\d{0,6}", num): sys.exit(f"bad issue number {num!r}")
    dry = os.environ.get("DRY_RUN", "") in ("1", "true")
    api = Api(token, repo)
    issue, _ = api.call("GET", f"issues/{num}")
    if issue.get("pull_request"): print("a pull request: skipped"); return
    comments = api.pages(f"issues/{num}/comments?per_page=100")
    found = [(u, "the issue text", issue["html_url"]) for u in attachments(issue.get("body"))]
    mine = None
    for c in comments:
        if (c.get("user") or {}).get("type") == "Bot":
            if MARKER in (c.get("body") or "") and c["user"]["login"] == "github-actions[bot]": mine = c
            continue
        found += [(u, f"comment from {c['created_at'][:10]}", c["html_url"]) for u in attachments(c.get("body"))]
    seen = set()  # (a quote reply repeats the quoted comment's links: each log once, where it was attached)
    found = [f for f in found if f[0] not in seen and not seen.add(f[0])]
    if not found: print("no lasrecomp log attached: nothing to do"); return
    newest = list(reversed(found))[:MAX_LOGS]
    entries = []
    for u, where, link in newest:
        name = urllib.parse.unquote(u.rsplit("/", 1)[1])
        src = f"[{clean(name, 60)}]({u}) in [{where}]({link})"
        try:
            entries.append((src, parse(download(u))))
        except Exception as e:  # (a bad download is reported in the comment, never fatal)
            entries.append((src, f"{type(e).__name__}: {e}"))
    body = render(entries, len(found))
    first = entries[0][1]
    label = None
    if isinstance(first, dict):
        label = LABELS["crash"][0] if first["end"] in ("crash", "hang") else LABELS["device-lost"][0] if first["end"] == "device-lost" \
            else LABELS["ok"][0] if first["end"] == "ok" else None
    print(body)
    if shader_records(entries): print(f"label: {SHADER_LABEL[0]}")
    print(f"label: {label or '(none)'}; existing bot comment: {mine['html_url'] if mine else '(none)'}")
    if dry:
        print("dry run: nothing posted")
        summary = os.environ.get("GITHUB_STEP_SUMMARY")
        if summary:
            with open(summary, "a", encoding="utf-8") as f:
                f.write(f"Dry run for issue #{num} (label: {label or 'none'}):\n\n{body}\n")
        return
    if mine:
        if mine["body"] != body: api.call("PATCH", f"issues/comments/{mine['id']}", {"body": body})
    else:
        api.call("POST", f"issues/{num}/comments", {"body": body})
    have = {l["name"] for l in issue.get("labels", [])}
    for name, color, desc in LABELS.values():
        if name in have and name != label:
            api.call("DELETE", f"issues/{num}/labels/{urllib.parse.quote(name)}")
    if label and label not in have:
        add_label(api, num, next(v for v in LABELS.values() if v[0] == label))
    if shader_records(entries) and SHADER_LABEL[0] not in have:  # (only added, never removed: the issue watch keys on it)
        add_label(api, num, SHADER_LABEL)


def add_label(api, num, spec):
    name, color, desc = spec
    try:
        api.call("POST", "labels", {"name": name, "color": color, "description": desc})
    except urllib.error.HTTPError as e:
        if e.code != 422: raise  # (422: it exists already)
    api.call("POST", f"issues/{num}/labels", {"labels": [name]})


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "parse":
        entries = []
        for p in reversed(sys.argv[2:]):
            with open(p, encoding="utf-8", errors="replace") as f:
                entries.append((os.path.basename(p), parse(f.read())))
        sys.stdout.write(render(entries[:MAX_LOGS], len(entries)))
        if shader_records(entries[:MAX_LOGS]): print("label:", SHADER_LABEL[0])
        print("label:", {"crash": "log: crash", "hang": "log: crash", "device-lost": "log: device-lost", "ok": "log: ok"}
              .get(entries[0][1]["end"], "(none)"))
    elif len(sys.argv) == 2 and sys.argv[1] == "issue":
        run_issue()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
