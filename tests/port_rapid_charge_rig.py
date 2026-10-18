#!/usr/bin/env python3
"""Disc-backed regression for charged beam switching (issue #154).

Run with --build /absolute/build/dir --disc /path/to/GM8E01.iso.
Requires mprig's Xvfb setup; no saves, settings or mods are inherited.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--disc", required=True, type=Path)
    parser.add_argument("--evidence-dir", required=True, type=Path)
    parser.add_argument("--prefix", default="i154")
    parser.add_argument("--delay", type=int, default=60)
    args = parser.parse_args()
    evidence = args.evidence_dir.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["MPRIG_DISC"] = str(args.disc.resolve())
    env["MPRIG_USER"] = str(evidence)

    def rig(*commands):
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/mprig.py"), *map(str, commands)],
            cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        print(result.stdout, end="", flush=True)
        if result.returncode:
            raise RuntimeError(result.stdout)
        return result.stdout

    failures = []
    for rapid in (0, 1):
        name = f"{args.prefix}-{rapid}"
        rig("start", name, "--build", args.build.resolve(), "--room", "landing site",
            "--mods", "none", "--settings", "none", "--env", f"MP_RAPID_CHARGE={rapid}")
        try:
            rig("cmd", name, "give chargebeam", "give plasmabeam", "give morphball",
                "give morphballbombs", "give missiles 50", "god on")
            output = rig("cmd", name, "press a 0", "wait 150", "beam plasma",
                         f"wait {args.delay}", "beam power", "wait 120", "stick 0 0 1",
                         "wait 90", "beam", "press a 1", "wait 5", "objs energyproj",
                         "press y 1", "wait 5", "items", "press x 1", "wait 120",
                         "press a 1", "wait 5", "objs bomb")
            (evidence / f"rapid-{rapid}.txt").write_text(output)
            checks = {
                "beam returned to Power": "beam power" in output.splitlines(),
                "beam fired": "CEnergyProjectile" in output,
                "missile fired": bool(re.search(r"Missiles\s+49/50", output)),
                "bomb dropped": bool(re.search(r"\bCBomb\s", output)),
            }
            for label, passed in checks.items():
                print(f"rapid={rapid}: {label}: {'PASS' if passed else 'FAIL'}", flush=True)
                if not passed:
                    failures.append(f"rapid={rapid}: {label}")
        finally:
            rig("stop", name)
    if failures:
        raise RuntimeError("; ".join(failures))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
