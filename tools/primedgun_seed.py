#!/usr/bin/env python3
"""Build and check PrimedGun's pipeline cache seed (assets/primedgun_pipeline_cache.db).

Upstream's seed (assets/initial_pipeline_cache.db, tools/pipeline_seed.py) holds the mono
shaders of a desktop tour at MSAA 1, most of them with indexed vertex attributes. PrimedGun
de-indexes vertices on every platform, draws the static world from native vertex records and
asks for an eye pipeline beside each mono one, so it needs other rows. This second, fork-only
seed takes them from the caches players already saved, on a PC (PC VR, D3D12) and on a Quest.
aurora merges its rows into the player's pipeline_cache.db at startup and compiles them in the
background, so the shaders exist before the game first draws with them.

The PC executable embeds it through CMake; the Quest APK packs it from assets/ and the activity
unpacks it into the app's files folder. One file serves every device:
  - msaaSamples is stored as 1; aurora rewrites it to the device's sample count when it merges
    the seed.
  - every MultiviewClip row (a multiview eye pass: Quest 3) has its EyeClipImmediate twin (one
    eye at a time: PC VR, Quest 2 and Pro) and the other way round, and every eye row has its
    mono twin, since the game always looks the mono pipeline up too. MultiviewFull rows stay
    as recorded.
  - the hash column is not aurora's key. aurora recomputes the key (xxh3 of the config) when
    it merges the seed, so the column only has to be unique: it holds the first 8 bytes of the
    config's blake2b digest, read as a signed little-endian integer.
It holds retail GX rows only: no Remastered-only or shadow-pass configs, no indexed attributes,
no clear, RmlUi, Vfx or Water rows. It keeps every row a device recorded, but not the rows a
cache only holds because aurora merged upstream's seed into it: a cache that holds at least 90%
of upstream's GX rows byte for byte merged it, and its rows identical to upstream's are dropped
(on the PC, msaa 1 rows while that PC plays at MSAA 4). A recorded row that only normalises to
an upstream config stays: the Quest APK does not ship upstream's seed, and upstream's rows are
msaa 1 only. `build --no-exclude` keeps the merged rows too.

Rebuild it when GXPipelineConfigVersion (extern/aurora/lib/gx/pipeline.hpp) or
PortGxLayoutEpoch (extern/aurora/lib/gfx/pipeline_cache.cpp) changes: aurora then ignores the
seed's rows, and `check` (a ctest) fails. Pull caches that a build of the new layout saved,
after quitting the game (or copy pipeline_cache.db-wal beside the pulled file too):

    Windows: %APPDATA%/Metroid Prime/pipeline_cache.db
    Quest: the app's private files, readable only from a debuggable build:
        adb exec-out run-as org.primedgun.v2 cat files/pipeline_cache.db > quest.db

    python tools/primedgun_seed.py build pc.db quest.db   # writes assets/primedgun_pipeline_cache.db
    python tools/primedgun_seed.py check                  # also run by ctest
    python tools/primedgun_seed.py stats quest.db         # what a pulled cache holds
"""

import argparse
import collections
import hashlib
import itertools
import os
import re
import shutil
import sqlite3
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pipeline_seed as ps  # noqa: E402  upstream's seed tool: SCHEMA, versions, Remastered rows

SEED = ps.ROOT / "assets" / "primedgun_pipeline_cache.db"
CACHE_SOURCE = ps.ROOT / "extern" / "aurora" / "lib" / "gfx" / "pipeline_cache.cpp"
TYPE_NAMES = {**ps.TYPE_NAMES, 3: "vfx", 4: "water"}  # gfx::ShaderType

# gx::PipelineConfig (extern/aurora/lib/gx/pipeline.hpp), little-endian and without padding:
# u32 version, u32 msaaSamples, gx::ShaderConfig (gx.hpp) from byte 8, ..., u32 shadowPass,
# then four bools. The ShaderConfig offsets are gx.hpp's static_asserts.
GX_CONFIG_SIZE = 2788
MSAA = 4
SC = 8
HUD_SAMPLE = SC + 9
PORT_BITS = SC + 13  # from bit 0: multiview:2, mapBatch:1, mapCull:2, nativeVertices:1, spare:2
ATTRS = SC + 16  # AttrConfig[GX_VA_MAX_ATTR], 8 bytes each, attrType first
ATTR_SIZE = 8
VTX_ATTRS = 21  # GX_VA_PNMTXIDX..GX_VA_TEX7, the ones populate_pipeline_config fills
SHADOW_PASS = GX_CONFIG_SIZE - 4 - 4
GX_INDEX8, GX_INDEX16 = 2, 3
MONO, MULTIVIEW_CLIP, MULTIVIEW_FULL, EYE_CLIP_IMMEDIATE = 0, 1, 2, 3
MODE_NAMES = ["mono", "MultiviewClip", "MultiviewFull", "EyeClipImmediate"]
# The eye pipeline the other kind of device asks for the same draw.
OTHER_EYE = {MULTIVIEW_CLIP: EYE_CLIP_IMMEDIATE, EYE_CLIP_IMMEDIATE: MULTIVIEW_CLIP}
# A cache holding this share of a seed's GX rows, byte for byte, got the seed merged into it.
# Not all of them: a cache can lose rows (a purge, a reset) and a seed can gain rows later.
MERGED_SHARE = 0.9


def port_constant(name):
    m = re.search(rf"constexpr\s+int\s+{name}\s*=\s*(\d+)\s*;", CACHE_SOURCE.read_text())
    if not m:
        sys.exit(f"primedgun_seed: {name} not found in {CACHE_SOURCE.relative_to(ps.ROOT)}")
    return int(m.group(1))


def u32(config, offset):
    return int.from_bytes(config[offset:offset + 4], "little")


def multiview(config):
    return config[PORT_BITS] & 3


def map_batch(config):
    return config[PORT_BITS] >> 2 & 1


def native_vertices(config):
    return config[PORT_BITS] >> 5 & 1


def indexed(config):
    return any(config[ATTRS + i * ATTR_SIZE] in (GX_INDEX8, GX_INDEX16) for i in range(VTX_ATTRS))


def with_multiview(config, mode):
    out = bytearray(config)
    out[PORT_BITS] = out[PORT_BITS] & ~3 | mode
    return bytes(out)


def normalised(config):
    out = bytearray(config)
    out[MSAA:MSAA + 4] = (1).to_bytes(4, "little")
    out[HUD_SAMPLE] = 0
    return bytes(out)


def row_hash(config):
    return int.from_bytes(hashlib.blake2b(config, digest_size=8).digest(), "little", signed=True)


def header(path):
    """(user_version, aurora_schema values, or None without the table) of a database."""
    db = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        user_version = db.execute("PRAGMA user_version").fetchone()[0]
        try:
            schema = [v for (v,) in db.execute("SELECT value FROM aurora_schema")]
        except sqlite3.OperationalError:
            schema = None
        return user_version, schema
    finally:
        db.close()


def read_cache(path):
    """(rows, user_version, schema) of a cache, read from a copy: the game keeps its cache in
    WAL mode, and opening the file in place can leave -wal and -shm files beside it."""
    path = Path(path)
    if not path.is_file():
        sys.exit(f"primedgun_seed: {path} not found")
    with tempfile.TemporaryDirectory() as tmp:
        copy = Path(tmp) / "cache.db"
        shutil.copyfile(path, copy)
        if Path(f"{path}-wal").is_file():
            shutil.copyfile(f"{path}-wal", f"{copy}-wal")
        try:
            user_version, schema = header(copy)
            return ps.read_rows(copy), user_version, schema
        except sqlite3.Error as e:
            sys.exit(f"primedgun_seed: {path}: not a pipeline cache ({e})")


def drop_reason(row, version, merged=None):
    """Why a cache row can't go into the seed, or None. Checked before normalising. merged maps
    the raw configs of the seeds merged into the row's cache to the seed's name."""
    t, _h, ver, size, config, _f = row
    if t != ps.TYPE_GX:
        return f"{TYPE_NAMES.get(t, f'type {t}')} row"
    if merged and config in merged:
        return f"merged from {merged[config]}"
    if ver != version:
        return f"GX version {ver}, current {version}"
    if size != GX_CONFIG_SIZE or len(config) != size:
        return f"size {size} (blob {len(config)}), expected {GX_CONFIG_SIZE}"
    if u32(config, 0) != ver:
        return "blob version differs from its row"
    if config[PORT_BITS] >> 6:
        return "spare bits of ShaderConfig byte 13 set (unknown layout)"
    if ps.remastered_reason(config):
        return f"Remastered-only ({ps.remastered_reason(config)})"
    if u32(config, SHADOW_PASS):
        return "shadow pass"
    if indexed(config):
        return "indexed attribute (the fork de-indexes)"
    return None


def seed_problem(row, version):
    """What a seed row must not have, or None: a drop reason, or what build normalises."""
    reason = drop_reason(row, version)
    if reason:
        return reason
    config = row[4]
    if u32(config, MSAA) != 1:
        return f"msaaSamples {u32(config, MSAA)}, expected 1"
    if config[HUD_SAMPLE]:
        return f"hudSample {config[HUD_SAMPLE]}, expected 0"
    return None


def keep(seeds, config, first):
    seeds[config] = min(seeds.get(config, first), first)


def merged_seeds(rows, excluded):
    """{raw config: seed name} of the excluded seeds aurora merged into a cache, and a line per
    seed saying whether it did."""
    raw = {r[4] for r in rows if r[0] == ps.TYPE_GX}
    merged, lines = {}, []
    for name, configs in excluded:
        held = len(configs & raw)
        if configs and held >= MERGED_SHARE * len(configs):
            merged.update((c, name) for c in configs)
            lines.append(f"  merged {name}: holds {held} of its {len(configs)} GX rows, so drops them")
        else:
            lines.append(f"  did not merge {name}: holds {held} of its {len(configs)} GX rows, drops none")
    return merged, lines


def read_source(path, version, epoch, seeds, excluded):
    """Add a cache's usable rows, normalised, to seeds; print what was dropped. Returns the
    source's own normalised configs."""
    rows, user_version, _schema = read_cache(path)
    merged, merge_lines = merged_seeds(rows, excluded)
    dropped = collections.Counter()
    mine = set()
    msaa = hud = 0
    for row in rows:
        reason = drop_reason(row, version, merged)
        if reason:
            dropped[reason] += 1
            continue
        config = row[4]
        msaa += u32(config, MSAA) != 1
        hud += config[HUD_SAMPLE] != 0
        config = normalised(config)
        mine.add(config)
        keep(seeds, config, row[5])
    kept = len(rows) - sum(dropped.values())
    print(f"{path} (user_version {user_version}): {len(rows)} rows read, {kept} kept "
          f"({len(mine)} distinct once normalised)")
    if user_version < epoch:
        print(f"  warning: user_version below PortGxLayoutEpoch {epoch}: rows of an older layout")
    for line in merge_lines:
        print(line)
    for reason, n in dropped.most_common():
        print(f"  dropped {n}: {reason}")
    print(f"  normalised: msaaSamples set to 1 on {msaa} rows, hudSample cleared on {hud}")
    return mine


def print_overlap(by_source, excluded):
    upstream = {normalised(c) for _name, configs in excluded for c in configs}
    for (a, ca), (b, cb) in itertools.combinations(by_source.items(), 2):
        mono_a = {c for c in ca if multiview(c) == MONO}
        mono_b = {c for c in cb if multiview(c) == MONO}
        both = mono_a & mono_b
        print(f"mono rows identical in {a} and {b}: {len(both)} of {len(mono_a)} and {len(mono_b)} "
              f"({len(both & upstream)} of them normalise to an excluded seed's config)")
        # The same draw's eye row, whichever kind of eye pass recorded it.
        eye_a = {with_multiview(c, MULTIVIEW_CLIP) for c in ca if multiview(c) in OTHER_EYE}
        eye_b = {with_multiview(c, MULTIVIEW_CLIP) for c in cb if multiview(c) in OTHER_EYE}
        print(f"eye rows of the same draw in {a} and {b} (MultiviewClip and EyeClipImmediate as one): "
              f"{len(eye_a & eye_b)} of {len(eye_a)} and {len(eye_b)}")


def add_twins(seeds):
    """Give every eye row its twin for the other kind of device and its mono twin. Returns the
    rows added, by multiview mode."""
    before = set(seeds)
    for config, first in list(seeds.items()):
        mode = multiview(config)
        if mode == MONO:
            continue
        keep(seeds, with_multiview(config, MONO), first)
        if mode in OTHER_EYE:
            keep(seeds, with_multiview(config, OTHER_EYE[mode]), first)
    return collections.Counter(multiview(c) for c in seeds if c not in before)


def write_seed(out, seeds, version, schema, epoch):
    rows = [(ps.TYPE_GX, row_hash(c), version, len(c), c, first)
            for c, first in sorted(seeds.items(), key=lambda kv: (kv[1], kv[0]))]
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(".tmp")
    tmp.unlink(missing_ok=True)
    db = sqlite3.connect(tmp)
    db.executescript(ps.SCHEMA)
    db.execute("INSERT INTO aurora_schema VALUES (?)", (schema,))
    db.executemany("INSERT INTO pipeline_cache VALUES (?, ?, ?, ?, ?, ?)", rows)
    db.commit()
    # aurora refuses a PrimedGun seed of another layout epoch.
    db.execute(f"PRAGMA user_version = {epoch}")
    db.execute("VACUUM")
    db.close()
    os.replace(tmp, out)


def mode_counts(configs):
    """'mono N (native n, mapBatch m), ...' for every multiview mode."""
    parts = []
    for mode, name in enumerate(MODE_NAMES):
        mine = [c for c in configs if multiview(c) == mode]
        parts.append(f"{name} {len(mine)} (native {sum(map(native_vertices, mine))}, "
                     f"mapBatch {sum(map(map_batch, mine))})")
    return ", ".join(parts)


def cmd_build(a):
    version = ps.current_versions()[ps.TYPE_GX]
    epoch = port_constant("PortGxLayoutEpoch")
    schema = port_constant("PipelineCacheSchema")
    # (name, raw GX configs) of each seed whose merged rows are left out.
    excluded = [(Path(p).name, {r[4] for r in read_cache(p)[0] if r[0] == ps.TYPE_GX})
                for p in ([] if a.no_exclude else a.exclude or [ps.SEED])]
    seeds, by_source = {}, {}
    for src in a.sources:
        name = Path(src).name if Path(src).name not in by_source else src
        by_source[name] = read_source(src, version, epoch, seeds, excluded)
    print(f"{len(seeds)} distinct rows from {len(a.sources)} sources")
    print_overlap(by_source, excluded)
    # After the merged rows are gone, so a kept eye row gets its mono twin back even when a
    # merged row had the same config.
    added = add_twins(seeds)
    print(f"twins added: {added[EYE_CLIP_IMMEDIATE]} EyeClipImmediate (of MultiviewClip rows), "
          f"{added[MULTIVIEW_CLIP]} MultiviewClip (of EyeClipImmediate rows), {added[MONO]} mono (of eye rows)")
    out = Path(a.out)
    write_seed(out, seeds, version, schema, epoch)
    print(f"wrote {out}: {len(seeds)} rows, {out.stat().st_size // 1024} KiB, "
          f"GX version {version}, user_version {epoch}")
    print(f"  {mode_counts(seeds)}")


def file_problems(user_version, schema, epoch, want_schema):
    out = []
    if schema is None:
        out.append("no aurora_schema table")
    elif schema != [want_schema]:
        out.append(f"aurora_schema holds {schema}, expected [{want_schema}]")
    if user_version != epoch:
        out.append(f"user_version {user_version}, PortGxLayoutEpoch is {epoch}: aurora refuses this seed")
    return out


def row_problems(rows, version):
    out = []
    good = []
    for i, row in enumerate(rows):
        problem = seed_problem(row, version)
        if problem:
            out.append(f"row {i}: {problem}")
        else:
            good.append((i, row[4]))
    configs = collections.Counter(r[4] for r in rows)
    out += [f"{n} rows share the config of hash {row_hash(c)}" for c, n in configs.items() if n > 1]
    hashes = collections.Counter(r[1] for r in rows)
    out += [f"{n} rows share hash {h}" for h, n in hashes.items() if n > 1]
    for i, config in good:
        mode = multiview(config)
        if mode != MONO and with_multiview(config, MONO) not in configs:
            out.append(f"row {i}: {MODE_NAMES[mode]} row without its mono twin")
        if mode in OTHER_EYE and with_multiview(config, OTHER_EYE[mode]) not in configs:
            out.append(f"row {i}: {MODE_NAMES[mode]} row without its {MODE_NAMES[OTHER_EYE[mode]]} twin")
    return out


def cmd_check(a):
    seed = Path(a.seed)
    if not seed.is_file():
        sys.exit(f"{seed} is missing; build it from saved caches: "
                 f"python tools/primedgun_seed.py build PC_CACHE.db QUEST_CACHE.db (see --help)")
    version = ps.current_versions()[ps.TYPE_GX]
    epoch = port_constant("PortGxLayoutEpoch")
    try:
        user_version, schema = header(seed)
        rows = ps.read_rows(seed)
    except sqlite3.Error as e:
        sys.exit(f"{seed}: not a pipeline cache ({e})")
    bad = file_problems(user_version, schema, epoch, port_constant("PipelineCacheSchema"))
    bad += row_problems(rows, version) if rows else ["no rows"]
    for line in bad[:20]:
        print(f"{seed}: {line}")
    if bad:
        print(f"{len(bad)} problem(s); rebuild the seed (python tools/primedgun_seed.py --help)")
        sys.exit(1)
    print(f"{seed}: {len(rows)} GX rows, epoch {epoch}, all current, retail and twinned: "
          f"{mode_counts([r[4] for r in rows])}")


def cmd_stats(a):
    versions = ps.current_versions()
    for path in a.dbs:
        rows, user_version, schema = read_cache(path)
        print(f"== {path}: user_version {user_version}, aurora_schema {schema}, {len(rows)} rows")
        for (t, ver, size), n in sorted(collections.Counter((r[0], r[2], r[3]) for r in rows).items()):
            stale = " (not current)" if t in versions and ver != versions[t] else ""
            print(f"  {TYPE_NAMES.get(t, f'type {t}')} v{ver} size {size}: {n}{stale}")
        gx = [r[4] for r in rows if r[0] == ps.TYPE_GX and len(r[4]) == GX_CONFIG_SIZE]
        groups = collections.Counter(
            (u32(c, MSAA), multiview(c), native_vertices(c), map_batch(c), u32(c, SHADOW_PASS),
             ps.remastered_reason(c) or "retail", indexed(c)) for c in gx)
        print("  GX rows by msaa, multiview, nativeVertices, mapBatch, shadowPass, content, attributes:")
        for (msaa, mode, native, batch, shadow, content, idx), n in sorted(groups.items()):
            print(f"    msaa {msaa}  {MODE_NAMES[mode]:<16}  native {native}  mapBatch {batch}  "
                  f"shadowPass {shadow}  {content:<7}  {'indexed' if idx else 'direct':<7}  {n}")
        hud = collections.Counter(c[HUD_SAMPLE] for c in gx)
        print(f"  hudSample: {dict(sorted(hud.items()))}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="write the seed from caches players saved")
    b.add_argument("sources", nargs="+", metavar="SOURCE.db")
    b.add_argument("--out", default=str(SEED))
    b.add_argument("--exclude", action="append", metavar="SEED.db",
                   help="leave out the rows a source got by merging this seed (repeatable; default: "
                        "upstream's assets/initial_pipeline_cache.db)")
    b.add_argument("--no-exclude", action="store_true", help="keep the rows sources got by merging a seed too")
    c = sub.add_parser("check", help="fail unless the seed is current, retail, normalised and twinned")
    c.add_argument("seed", nargs="?", default=str(SEED))
    s = sub.add_parser("stats", help="summarise pipeline caches (pulled from a device, or a seed)")
    s.add_argument("dbs", nargs="+", metavar="DB")
    a = p.parse_args()
    {"build": cmd_build, "check": cmd_check, "stats": cmd_stats}[a.cmd](a)


if __name__ == "__main__":
    main()
