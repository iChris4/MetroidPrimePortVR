#!/usr/bin/env python3
"""Generate platform/port_ap_locations.inc: the built-in Archipelago location table.

Joins the Metroid Prime AP world's ``Locations.py`` (location names, ids and the
memory relay each check is read from) with randomprime's ``pickup_meta.rs.in``
(the pickup, HUD memo and memory relay instance ids per room)::

    python3 tools/gen_ap_locations.py build/MetroidAPrime/src/Locations.py \
        build/randomprime/src/pickup_meta.rs.in > platform/port_ap_locations.inc

Add ``--dump FILE`` (a MP_RANDO_DUMP log) to check every pickup key against the
pickups the port actually built.
"""

import argparse
import ast
import re
import sys

PAK_WORLDS = {
    "metroid1.pak": 0x158EFE17,
    "metroid2.pak": 0x83F6FF6F,
    "metroid3.pak": 0xA8BE6291,
    "metroid4.pak": 0x39F2DE28,
    "metroid5.pak": 0xB1AC4D65,
    "metroid6.pak": 0x3EF8237C,
    "metroid7.pak": 0xC13B09D1,
    "metroid8.pak": 0x13D79165,
}

AP_LEVELS = {
    "Chozo_Ruins": 0x83F6FF6F,
    "Phendrana_Drifts": 0xA8BE6291,
    "Tallon_Overworld": 0x39F2DE28,
    "Phazon_Mines": 0xB1AC4D65,
    "Magmoor_Caverns": 0x3EF8237C,
}


# AP relays with no pickup_meta counterpart, mapped to the pickup's own memory relay.
RELAY_OVERRIDES = {
    # Sunchamber - Ghosts: the AP world watches another relay in the room; the
    # pickup itself is the layer-6 missile (memory relay 0x252F7E).
    0x00253094: 0x00252F7E,
}


def read_ap(path):
    tree = ast.parse(open(path, encoding="utf-8").read())
    names = {}
    pickups = None
    for node in tree.body:
        target = None
        if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name):
            target = node.targets[0].id
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            target = node.target.id
        if target is None:
            continue
        if target.endswith("_location_table") and isinstance(node.value, ast.Dict):
            for key, value in zip(node.value.keys, node.value.values):
                names[ast.literal_eval(value)] = ast.literal_eval(key)
        elif target == "PICKUP_LOCATIONS":
            pickups = []
            for element in node.value.elts:
                level, relay = element.elts
                pickups.append((AP_LEVELS[level.attr], ast.literal_eval(relay)))
    base = min(names)
    return [(base + i, names[base + i], world, relay) for i, (world, relay) in enumerate(pickups)]


OBJ = r"ScriptObjectLocation \{ layer: (\d+), instance_id: (\d+) \}"


def read_rooms(path):
    text = open(path, encoding="utf-8").read()
    world = None
    room = None
    name = None
    out = []
    pickup_re = re.compile(
        r"location: " + OBJ + r",\s*attainment_audio: " + OBJ + r",\s*hudmemo: " + OBJ
        + r",\s*memory_relay: " + OBJ)
    for line_match in re.finditer(
            r'\("([Mm]etroid\d\.pak)", &\[|room_id: ResId::<res_id::MREA>::new\((0x[0-9A-Fa-f]+)\)'
            r'|name: "([^"]*)"|PickupLocation \{', text):
        if line_match.group(1):
            world = PAK_WORLDS[line_match.group(1).lower()]
        elif line_match.group(2):
            room = int(line_match.group(2), 16)
        elif line_match.group(3) is not None:
            name = line_match.group(3)
        else:
            m = pickup_re.match(text, text.index("location:", line_match.end()))
            v = [int(x) for x in m.groups()]
            out.append({
                "world": world, "room": room, "room_name": name,
                # The port keys entities by TEditorId::Value(), which drops the
                # layer bits (26-31) that some stored instance ids carry.
                "pickup": v[1] & 0x3FFFFFF,
                "audio": v[3] & 0x3FFFFFF,
                "hudmemo": v[5] & 0x3FFFFFF,
                "relay": v[7] & 0x3FFFFFF,
            })
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("locations")
    parser.add_argument("pickup_meta")
    parser.add_argument("--dump", action="append", default=[])
    args = parser.parse_args()

    ap = read_ap(args.locations)
    rooms = read_rooms(args.pickup_meta)
    dumped = set()
    for path in args.dump:
        for line in open(path, encoding="utf-8", errors="replace"):
            if line.startswith("LOC "):
                dumped.add(line.split()[1])

    rows = []
    errors = 0
    for ap_id, name, world, relay in ap:
        relay = RELAY_OVERRIDES.get(relay, relay)
        matches = [r for r in rooms if r["world"] == world
                   and (r["relay"] & 0xFFFFFF) == (relay & 0xFFFFFF)]
        if len(matches) != 1:
            print(f"error: {name}: {len(matches)} matches for relay {relay:08X}", file=sys.stderr)
            errors += 1
            continue
        r = matches[0]
        key = f"{world:08X}:{r['room']:08X}:{r['pickup']:08X}"
        if dumped and key not in dumped:
            print(f"warning: {name}: {key} not in the dump", file=sys.stderr)
        rows.append((ap_id, name, world, r))
    if errors:
        sys.exit(1)

    print("// Generated by tools/gen_ap_locations.py from the Metroid Prime AP world's")
    print("// Locations.py and randomprime's pickup_meta.rs.in. Do not edit.")
    print("// { AP id, world MLVL, area MREA, pickup, HUD memo, memory relay, attainment audio, name }")
    for ap_id, name, world, r in rows:
        print(f'{{{ap_id}, 0x{world:08X}u, 0x{r["room"]:08X}u, 0x{r["pickup"]:08X}u, '
              f'0x{r["hudmemo"]:08X}u, 0x{r["relay"]:08X}u, 0x{r["audio"]:08X}u, "{name}"}},')


if __name__ == "__main__":
    main()
