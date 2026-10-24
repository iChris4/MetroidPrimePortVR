#!/usr/bin/python3
"""statecheck: save-state round-trip check. Dumps the world, saves, loads, dumps again and diffs.

    statecheck.py --room "artifact temple" --setup 'send 001003D3 SetToZero' --setup 'wait 600'
    statecheck.py --run sc1                       # a run already in the room you care about
    statecheck.py --rooms scenarios.txt           # many scenarios in one game

A scenario file has one scenario per line: `ROOM [| setup cmd ; setup cmd ...]` (ROOM is a name or
MLVL:MREA; `#` starts a comment). The first room starts the game, the rest `warp`.

Sequence (all with the game held, so ticks are exact; `statedump` is a frame command and runs between
ticks, also while held):
  1. setup commands run unheld, then `hold 1`;
  2. `statedump A`: the world exactly as the save will see it. PortSaveState::Tick runs at the start of
     CStateManager::Update (before any object updates), so the `state save` queued now is taken by the
     first stepped tick and captures exactly what A dumped;
  3. `state save 8`, `step 1` (the save happens in that tick), `statedump A1` (the same world one tick
     later: the natural drift of anything that moves or counts);
  4. `state load 8`, then `step 1` until the log shows `[savestate] room state: ...` (the restore of
     the rooms' objects, applied inside a tick), and `statedump B` at once. B holds the restored world
     plus at most the rest of the applying tick. When the line says `neighbour areas still streaming`
     (areas resident at the save, streamed in after the load), B0 is dumped at once and B waits for
     the later `room state, neighbour areas` line; an object B0 had but B lacks died during that wait
     ("gone-after-restore", a warning).
A field that differs between A and B is STRICT only if B also differs from A1: a value B shares with A1
is one that just moves each tick (a timer, a position), not a save-state gap.

Each object's `fields` (the plain-data fields of its class chain, from the generated entity schema,
platform/include/port_entity_schema.h, that the save's kTagFields block keeps) are diffed the same way,
all STRICT, and reported in their own "schema fields" section grouped by class and field. Its
`fields_other` (the rest: other classes, unsaved members) are listed after it and not counted.

Objects are matched by (area MREA, editor id, class); generated copies and objects with no editor id
(effects, projectiles: not saved by design) match by class and unique id order. Missing or extra objects
with an editor id are strict; those without one are warnings.

Exit 0 when nothing strict differs, 1 otherwise, 2 on a harness failure.
"""
import argparse
import collections
import importlib.util
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
RESTORE_RE = re.compile(r"\[savestate\] room state:")
LATE_RE = re.compile(r"\[savestate\] room state, neighbour areas")

# STRICT: any difference (floats within their tolerance) is a save-state gap.
STRICT = ["active", "hp", "kbres", "door_open", "timer", "timing", "counter", "boss", "ai", "body", "loco"]
# LOOSE: only reported beyond a tolerance, as warnings.
LOOSE = ["pos", "vel", "mom", "fwd", "anim", "ai_time"]
PLAYER_STRICT = ["morph", "hp"]
PLAYER_LOOSE = ["pos", "fwd"]
# Schema fields the port added (render interpolation, bookkeeping): never saved by design
# (SavedField in platform/port_savestate_fields.cpp), so not diffed.
PORT_FIELDS = ("xPort", "xpc_")


def load_mprig(path):
    spec = importlib.util.spec_from_file_location("mprig_mod", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Rig:
    """One game run, driven through mprig."""

    def __init__(self, a):
        self.a = a
        self.mprig_path = str(Path(a.mprig).resolve())
        self.mp = load_mprig(self.mprig_path)
        self.name = a.run
        self.owned = False
        self.run = None

    def mprig(self, *args):
        r = subprocess.run([sys.executable, self.mprig_path, *args], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"mprig {' '.join(args)} failed:\n{r.stdout}{r.stderr}")
        return r.stdout

    def start(self, room):
        self.name = self.name or f"sc{os.getpid()}"
        args = ["start", self.name, "--room", room, "--replace"]
        if self.a.build:
            args += ["--build", self.a.build]
        args += ["--mods", self.a.mods]
        if self.a.turbo:
            args += ["--turbo", str(self.a.turbo)]
        args += self.a.start_arg
        self.mprig(*args)
        self.owned = True

    def attach(self):
        self.run = self.mp.load_run(self.name)

    def cmd(self, *cmds, check=True):
        """Console commands -> list of (cmd, ok, lines). Raises on failure when `check`."""
        res = self.mp.console_seq(self.run, list(cmds), timeout=self.a.timeout)
        if len(res) < len(cmds):
            raise RuntimeError("the game closed the connection (crashed? mprig.py crash %s)" % self.name)
        if check:
            for c, ok, lines in res:
                if not ok:
                    raise RuntimeError(f"command '{c}' failed: {lines[-1]}")
        return res

    def restores(self):
        log = Path(self.run["log"])
        return len(RESTORE_RE.findall(log.read_text(errors="replace"))) if log.exists() else 0

    def lates(self):
        log = Path(self.run["log"])
        return len(LATE_RE.findall(log.read_text(errors="replace"))) if log.exists() else 0

    def last_restore_line(self):
        log = Path(self.run["log"])
        lines = [l for l in log.read_text(errors="replace").splitlines() if RESTORE_RE.search(l)]
        return lines[-1] if lines else ""

    def stop(self):
        if self.owned:
            try:
                self.mprig("stop", self.name)
            except RuntimeError as e:
                print(f"statecheck: stop failed: {e}", file=sys.stderr)


def selected_slot(rig):
    for c, ok, lines in rig.cmd("state list"):
        for l in lines:
            m = re.match(r"\*(\d+) ", l)
            if m:
                return int(m.group(1))
    return None


def read_dump(path):
    objs, player = [], None
    with open(path) as f:
        for l in f:
            d = json.loads(l)
            if d["t"] == "player":
                player = d
            else:
                objs.append(d)
    return {"player": player, "objs": objs}


def roundtrip(rig, tag, outdir, a):
    """Run the A / A1 / B (and with --post-wait, C) sequence on the current world. Returns the dump paths."""
    paths = {k: str(Path(outdir).resolve() / f"{tag}-{k}.jsonl") for k in ("A", "A1", "B0", "B", "C")}
    before = selected_slot(rig)
    rig.cmd("hold 1")
    rig.cmd(f"statedump {paths['A']}")
    rig.cmd(f"state save {a.slot}")
    rig.cmd("step 1")
    rig.cmd(f"statedump {paths['A1']}")
    n = rig.restores()
    rig.cmd(f"state load {a.slot}")
    for i in range(a.max_steps):
        rig.cmd("step 1")
        if rig.restores() > n:
            print(f"room state restore landed after {i + 1} steps")
            break
    else:
        return paths, f"no '[savestate] room state' line after {a.max_steps} steps of the load"
    paths["late"] = "neighbour areas still streaming" in rig.last_restore_line()
    if paths["late"]:
        # The resident neighbours of the saved room load after the room's own restore;
        # B is taken once they have their state too (or were given up on). B0 is the
        # world as the room's restore left it: what lived then but died before B
        # (short-lived debris) was restored.
        rig.cmd(f"statedump {paths['B0']}")
        for i in range(a.max_steps):
            rig.cmd("step 1")
            if rig.lates() > 0:
                print(f"neighbour areas restored after {i + 1} more steps")
                break
    rig.cmd(f"statedump {paths['B']}")
    if a.post_wait:
        rig.cmd(f"step {a.post_wait}")
        rig.cmd(f"statedump {paths['C']}")
    after = selected_slot(rig)
    if before is not None and after != before:
        rig.cmd(f"state slot {before}")
    return paths, None


# --- diff --------------------------------------------------------------------------------

def is_dynamic(o):
    return o["eid"][-6:] == "FFFFFF"


def keyed(objs):
    """{key: [obj...]} with each list in uid order; the key is (mrea, eid, class, generated)."""
    out = collections.defaultdict(list)
    for o in sorted(objs, key=lambda o: o["uid"]):
        eid = "-" if is_dynamic(o) else o["eid"]
        out[(o["mrea"], eid, o["class"], bool(o.get("generated")))].append(o)
    return out


def vec_dist(p, q):
    return sum((x - y) ** 2 for x, y in zip(p, q)) ** 0.5


def fields_differ(field, x, y, a):
    """None when equal (within tolerance), else a short text. x or y may be None (absent)."""
    if x is None and y is None:
        return None
    if x is None or y is None:
        return "absent" if x is None else "gone"
    if field in ("pos", "vel", "mom"):
        tol = a.tol_pos if field == "pos" else a.tol_vel
        return f"{vec_dist(x, y):.2f}" if vec_dist(x, y) > tol else None
    if field == "fwd":
        dot = sum(p * q for p, q in zip(x, y))
        return f"dot {dot:.3f}" if dot < 0.98 else None
    if field in ("hp", "kbres"):
        return None if abs(x - y) <= a.tol_hp else f"{abs(x - y):.2f}"
    if field in ("timer", "ai_time"):
        return None if abs(x - y) <= a.tol_time else f"{abs(x - y):.2f}"
    return None if x == y else "differs"


def two_ticks(x, z):
    """A1 + (A1 - A): where a value moving at a constant rate is a tick after A1, else None."""
    if isinstance(x, (int, float)) and isinstance(z, (int, float)) and not isinstance(x, bool):
        return 2 * z - x
    if isinstance(x, list) and isinstance(z, list) and len(x) == len(z):
        out = [two_ticks(p, q) for p, q in zip(x, z)]
        return None if any(v is None for v in out) else out
    return None


def restored_in_b0(f, x, z, w, same):
    """Whether B0's value of `f` is A's, A1's or one more tick of the same motion: restored at the
    restore's tick (which B0 follows by a tick), then moved on before B.
    x, z, w: the A, A1 (or None) and B0 dicts; same(p, q) compares two values."""
    if same(x.get(f), w.get(f)):
        return True
    if z is None:
        return False
    nxt = two_ticks(x.get(f), z.get(f))
    return same(z.get(f), w.get(f)) or (nxt is not None and same(nxt, w.get(f)))


def pair_copies(la, lb):
    """lb reordered to pair each copy in la with the most alike one (fewest differing schema fields,
    then nearest): a load makes generated copies again, with new uids in another order."""
    left = list(lb)
    out = []
    for x in la:
        def cost(y):
            fx, fy = x.get("fields") or {}, y.get("fields") or {}
            n = sum(fx.get(k) != fy.get(k) for k in set(fx) | set(fy))
            d = vec_dist(x["pos"], y["pos"]) if x.get("pos") and y.get("pos") else 0.0
            return (n, d)
        best = min(left, key=cost)
        left.remove(best)
        out.append(best)
    return out


def show(v):
    if isinstance(v, list):
        return "(" + ", ".join(f"{p:.1f}" for p in v) + ")"
    if isinstance(v, float):
        return f"{v:.2f}"
    if isinstance(v, str) and len(v) > 40:
        return v[:40] + f"..({len(v) // 2}B)"
    return str(v)


def schema_differ(x, y, a):
    """A `fields` entry: numbers within --tol-field (relative above 1), lists element-wise."""
    if x is None and y is None:
        return None
    if x is None or y is None:
        return "absent" if x is None else "gone"
    if isinstance(x, list) and isinstance(y, list) and len(x) == len(y):
        worst = max((schema_differ(p, q, a) is not None for p, q in zip(x, y)), default=False)
        return "differs" if worst else None
    if isinstance(x, (int, float)) and isinstance(y, (int, float)) and not isinstance(x, bool):
        if isinstance(x, int) and isinstance(y, int):
            return None if x == y else "differs"
        return None if abs(x - y) <= a.tol_field * max(1.0, abs(x)) else "differs"
    return None if x == y else "differs"


def boss_note(x, y):
    """Byte offsets where two boss block hex strings differ."""
    if x is None or y is None or len(x) != len(y):
        return ""
    offs = [i // 2 for i in range(0, len(x), 2) if x[i:i + 2] != y[i:i + 2]]
    return " bytes@" + ",".join(map(str, offs[:12]))


class Report:
    def __init__(self):
        self.rows = []  # (severity, class, label, kind, text)
        self.fields = []  # saved schema fields: (class, field, label, text), all strict
        self.other = []  # schema fields the save does not keep: same, informational
        self.drift = collections.Counter()
        self.field_drift = collections.Counter()
        self.total = 0

    def add(self, sev, cls, label, kind, text):
        self.rows.append((sev, cls, label, kind, text))

    def strict(self):
        return [r for r in self.rows if r[0] == "STRICT"]

    def failed(self):
        return bool(self.strict() or self.fields)


def compare(da, da1, db, a, db0=None):
    rep = Report()
    ka, ka1, kb = keyed(da["objs"]), keyed(da1["objs"]), keyed(db["objs"])
    kb0 = keyed(db0["objs"]) if db0 is not None else {}
    rep.total = len(da["objs"])
    per_class_total = collections.Counter(o["class"] for o in da["objs"])
    rep.per_class_total = per_class_total
    for key in sorted(set(ka) | set(kb)):
        mrea, eid, cls, gen = key
        la, lb = ka.get(key, []), kb.get(key, [])
        l1 = ka1.get(key, [])
        dyn = eid == "-"
        tag = "gen" if gen else ("dyn" if dyn else eid)
        if len(la) != len(lb):
            # Generated copies of a class the save leaves to its generator (sounds, maze
            # walls) have no saved fields: their count is not a save-state gap.
            unsaved = gen and not any(o.get("fields") for o in la + lb)
            sev = "WARN" if dyn or unsaved else "STRICT"
            kind = "missing" if len(lb) < len(la) else "extra"
            if not dyn and len(la) > 0 and len(l1) == len(lb):
                kind = "gone-in-tick"  # A1 (one tick later) already lacked it: not a restore gap
                sev = "WARN"
            elif not dyn and len(lb) < len(la) and db0 is not None and len(kb0.get(key, [])) >= len(la):
                kind = "gone-after-restore"  # B0 had them all: died during the neighbour wait
                sev = "WARN"
            rep.add(sev, cls, f"{mrea}:{tag}", kind, f"{len(la)} before, {len(lb)} after the round trip")
        # Copies pair up by uid order: once one is missing the rest pair with the wrong one.
        if dyn or (gen and len(la) != len(lb)):
            continue
        # B0 (the restore's own tick, before the neighbour wait) as a second chance: a value
        # B0 restored that then moved on during the wait (debris ageing) is not a gap.
        l0 = kb0.get(key, [])
        if gen and len(la) > 1:
            lb = pair_copies(la, lb)
            if len(l0) == len(la):
                l0 = pair_copies(la, l0)
        for i, (x, y) in enumerate(zip(la, lb)):
            z = l1[i] if i < len(l1) else None
            w = l0[i] if len(l0) == len(la) else None
            name = x.get("name") or ""
            label = f"{eid}{'#%d' % i if gen or len(la) > 1 else ''} {name}".strip()
            for strict, fields in ((True, STRICT), (False, LOOSE)):
                for f in fields:
                    vx, vy = x.get(f), y.get(f)
                    d = fields_differ(f, vx, vy, a)
                    if d is None:
                        continue
                    if z is not None and fields_differ(f, z.get(f), vy, a) is None:
                        rep.drift[(cls, f)] += 1  # B equals A1: the field just moved a tick
                        continue
                    if w is not None and restored_in_b0(f, x, z, w, lambda p, q: fields_differ(f, p, q, a) is None):
                        rep.drift[(cls, f)] += 1
                        continue
                    extra = boss_note(vx, vy) if f == "boss" else ""
                    rep.add("STRICT" if strict else "WARN", cls, label, f,
                            f"{show(vx)} -> {show(vy)} ({d}){extra}")
            for fkey, out in (("fields", rep.fields), ("fields_other", rep.other)):
                fx, fy = x.get(fkey) or {}, y.get(fkey) or {}
                fz = (z.get(fkey) or {}) if z is not None else None
                fw = (w.get(fkey) or {}) if w is not None else None
                for f in sorted(set(fx) | set(fy)):
                    if f.startswith(PORT_FIELDS):
                        continue
                    d = schema_differ(fx.get(f), fy.get(f), a)
                    if d is None:
                        continue
                    if (fz is not None and schema_differ(fz.get(f), fy.get(f), a) is None) or (
                            fw is not None and restored_in_b0(f, fx, fz, fw, lambda p, q: schema_differ(p, q, a) is None)):
                        if fkey == "fields":
                            rep.field_drift[(cls, f)] += 1
                        continue
                    out.append((cls, f, label, f"{show(fx.get(f))} -> {show(fy.get(f))}"))
    # the player
    pa, pb, p1 = da["player"], db["player"], da1["player"]
    for strict, fields in ((True, PLAYER_STRICT), (False, PLAYER_LOOSE)):
        for f in fields:
            d = fields_differ(f, pa.get(f), pb.get(f), a)
            if d is None:
                continue
            if fields_differ(f, p1.get(f), pb.get(f), a) is None:
                rep.drift[("player", f)] += 1
                continue
            rep.add("STRICT" if strict else "WARN", "player", "player", f,
                    f"{show(pa.get(f))} -> {show(pb.get(f))} ({d})")
    return rep


def print_report(title, rep, a):
    strict = rep.strict()
    warn = [r for r in rep.rows if r[0] == "WARN"]
    print(f"=== {title}: {rep.total} objects, {len(strict)} strict diff(s), {len(warn)} warning(s) ===")
    if rep.drift:
        print("tick drift (B equals the dump one tick after the save; ignored): " +
              ", ".join(f"{c}.{f} x{n}" for (c, f), n in sorted(rep.drift.items())))
    classes = sorted({r[1] for r in rep.rows})
    if not classes:
        print("no differences")
        print_fields(rep, a)
        return
    print("\nby class (objects in the room; STRICT diffs / warnings by kind):")
    for cls in classes:
        rows = [r for r in rep.rows if r[1] == cls]
        kinds = collections.Counter((r[0], r[3]) for r in rows)
        text = ", ".join(f"{'!' if s == 'STRICT' else '~'}{k} x{n}" for (s, k), n in sorted(kinds.items()))
        print(f"  {cls} ({rep.per_class_total.get(cls, 1)}): {text}")
    print("\n  ! = strict, ~ = warning\n\ndetails:")
    for cls in classes:
        rows = sorted((r for r in rep.rows if r[1] == cls), key=lambda r: (r[0] != "STRICT", r[3], r[2]))
        print(f"  {cls}")
        for sev, _, label, kind, text in rows[:a.detail] if not a.all else rows:
            print(f"    {'!' if sev == 'STRICT' else '~'} {label}: {kind} {text}")
        if not a.all and len(rows) > a.detail:
            print(f"    ... {len(rows) - a.detail} more (--all)")
    print()
    print_fields(rep, a)


def print_fields(rep, a):
    """The schema-field section: one line per class.field, then its objects. Then the fields
    the save does not keep, by class.field only (every detail with --other-fields)."""
    groups = collections.defaultdict(list)
    for cls, f, label, text in rep.fields:
        groups[(cls, f)].append((label, text))
    drift = len(rep.field_drift)
    print(f"schema fields: {len(rep.fields)} strict diff(s) in {len(groups)} class.field(s)"
          f"{f'; {drift} class.field(s) only moved a tick (ignored)' if drift else ''}")
    for (cls, f), rows in sorted(groups.items()):
        print(f"  ! {cls}.{f} x{len(rows)}")
        for label, text in rows[:a.detail] if not a.all else rows:
            print(f"      {label}: {text}")
        if not a.all and len(rows) > a.detail:
            print(f"      ... {len(rows) - a.detail} more (--all)")
    other = collections.defaultdict(list)
    for cls, f, label, text in rep.other:
        other[(cls, f)].append((label, text))
    if not other:
        return
    print(f"unsaved schema fields (not kept by the save; not counted): {len(rep.other)} diff(s) in "
          f"{len(other)} class.field(s)")
    if not a.other_fields:
        print("  " + ", ".join(f"{c}.{f} x{len(r)}" for (c, f), r in sorted(other.items())))
        return
    for (cls, f), rows in sorted(other.items()):
        print(f"  ~ {cls}.{f} x{len(rows)}")
        for label, text in rows:
            print(f"      {label}: {text}")


# --- scenarios ---------------------------------------------------------------------------

def parse_scenarios(a):
    if a.rooms:
        out = []
        for l in Path(a.rooms).read_text().splitlines():
            l = l.split("#", 1)[0].strip()
            if not l:
                continue
            room, _, setup = l.partition("|")
            out.append((room.strip(), [c.strip() for c in setup.split(";") if c.strip()]))
        return out
    return [(a.room or "", a.setup)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", help="use this existing run (not stopped); else start one named sc<pid>")
    ap.add_argument("--room", help="room name or MLVL:MREA to start (or warp) in")
    ap.add_argument("--setup", action="append", default=[], metavar="CMD",
                    help="console command run in the room before the round trip (repeatable)")
    ap.add_argument("--rooms", metavar="FILE", help="scenario file: `ROOM [| cmd ; cmd]` per line")
    ap.add_argument("--out", default="statecheck-out", help="directory for the JSONL dumps (default %(default)s)")
    ap.add_argument("--mprig", default=str(HERE / "mprig.py"), help="mprig.py to drive the game with")
    ap.add_argument("--build", help="build dir for `mprig start --build`")
    ap.add_argument("--mods", default="none", help="`mprig start --mods` (default none)")
    ap.add_argument("--turbo", type=int, help="`mprig start --turbo N` (needs an mprig that has it)")
    ap.add_argument("--start-arg", action="append", default=[], help="extra `mprig start` argument (repeatable)")
    ap.add_argument("--slot", type=int, default=8, help="scratch save slot (default 8)")
    ap.add_argument("--max-steps", type=int, default=600, help="ticks to wait for the restore (default 600)")
    ap.add_argument("--timeout", type=float, default=120.0, help="seconds per console command")
    ap.add_argument("--tol-pos", type=float, default=0.5)
    ap.add_argument("--tol-vel", type=float, default=0.5)
    ap.add_argument("--tol-hp", type=float, default=0.01)
    ap.add_argument("--tol-time", type=float, default=0.1, help="timer / ai_time difference in seconds")
    ap.add_argument("--tol-field", type=float, default=0.001,
                    help="schema float field difference (relative above 1; default %(default)s)")
    ap.add_argument("--post-wait", type=int, default=0, metavar="N",
                    help="also dump C N ticks after B and report C against A (informational, "
                         "not in the exit code): settles a restore that hands over late")
    ap.add_argument("--detail", type=int, default=8, help="detail lines per class (default 8)")
    ap.add_argument("--all", action="store_true", help="print every detail line")
    ap.add_argument("--other-fields", action="store_true",
                    help="print each diff of the schema fields the save does not keep")
    a = ap.parse_args()
    scenarios = parse_scenarios(a)
    if not a.run and not scenarios[0][0]:
        ap.error("give --run, --room or --rooms")
    Path(a.out).mkdir(parents=True, exist_ok=True)

    rig = Rig(a)
    failed = False
    summary = []
    try:
        if a.run:
            rig.attach()
        else:
            rig.start(scenarios[0][0])
            rig.attach()
        for i, (room, setup) in enumerate(scenarios):
            title = room or a.run
            if room and (a.run or i):
                rig.cmd("hold 0")
                mlvl, _, mrea = rig.mp.resolve_room(room).partition(":")
                rig.cmd(f"warp {mlvl} {mrea}".strip())
            if setup:
                rig.cmd("hold 0")
                rig.cmd(*setup)
            tag = f"{i + 1:02d}-" + re.sub(r"\W+", "_", title)[:32]
            paths, err = roundtrip(rig, tag, a.out, a)
            if err:
                print(f"=== {title}: FAIL: {err}")
                failed = True
                continue
            rig_line = rig.last_restore_line()
            rep = compare(read_dump(paths["A"]), read_dump(paths["A1"]), read_dump(paths["B"]), a,
                          read_dump(paths["B0"]) if paths["late"] else None)
            print_report(title, rep, a)
            print(f"{rig_line}\ndumps: {paths['A']} {paths['A1']} {paths['B']}\n")
            if a.post_wait:
                # No drift reference this far from the save: every moving value shows up,
                # so this report is read, not counted.
                late = compare(read_dump(paths["A"]), read_dump(paths["A"]), read_dump(paths["C"]), a)
                print_report(f"{title}, {a.post_wait} ticks after B (C vs A, not counted)", late, a)
                print(f"dump: {paths['C']}\n")
            failed = failed or rep.failed()
            kinds = collections.Counter((r[1], r[3]) for r in rep.strict())
            kinds.update((c, "fields") for c, *_ in rep.fields)
            summary.append((title, ", ".join(f"{c}.{k}" + (f" x{n}" if n > 1 else "")
                                             for (c, k), n in sorted(kinds.items())) or "ok"))
        rig.cmd("hold 0", check=False)
        if len(summary) > 1:
            print("=== summary (strict diffs per scenario) ===")
            for title, text in summary:
                print(f"  {title}: {text}")
    except RuntimeError as e:
        print(f"statecheck: {e}", file=sys.stderr)
        return 2
    finally:
        rig.stop()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
