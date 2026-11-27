# A small world built headless, saved (its building cache is written after the save) and left for lilabatch to exit.
# world_exit.cmake runs this and fails on a non-zero exit code or a crash report.
import os, sys
from lila import ops

out = sys.argv[1]
spec = {
    "name": "Tiny",
    "culture": "mexican",
    "terrain": {"size": [260000, 260000], "resolution": 129, "seed": 3, "source": {"type": "noise", "amplitude": 1500, "featureSize": 120000}},
    "settlements": [{"name": "Pueblo", "create": {"profile": "usTown", "seed": 2, "outline": [[-36000, -36000], [36000, -36000], [36000, 36000], [-36000, 36000]]},
                     "buildings": {"style": "colonial"}}],
    "sites": [{"kind": "village", "id": "v1", "at": [80000, 80000]}],
    "check": True,
}
r = ops.worldBuild(spec=spec, folder=out)
failed = [s for s in r["steps"] if not s.get("ok", True)]
if failed:
    raise SystemExit("worldBuild steps failed: %s" % failed)
ops.saveScene(os.path.join(out, "world.lila"))
print("world_exit: built %d steps and saved" % len(r["steps"]))
