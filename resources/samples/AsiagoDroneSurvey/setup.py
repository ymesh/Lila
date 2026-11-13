"""Turns the downloaded Asiago survey into asiago.lila. Lila runs it after the download with SOURCE (the downloaded folder)
and OUT (the .lila to write) set; to run it by hand, set both first."""

import os

from lila import ops

scan = next((os.path.join(root, f) for root, _, files in os.walk(SOURCE) for f in files if f.lower().endswith(".laz")), None)
if not scan:
    raise RuntimeError("the survey's .laz is not in the download")

# The file carries no CRS; it was published as UTM zone 32N (WGS 84), so that goes beside it as the survey tool's .prj.
prj = os.path.splitext(scan)[0] + ".prj"
if not os.path.exists(prj):
    with open(prj, "w") as f:
        f.write("EPSG:32632")

ops.newScene()
cloud = ops.importPointCloud(scan, name="AsiagoSurvey")["node"]
info = ops.pointCloudInfo(cloud)
lo, hi = info["boundsMin"], info["boundsMax"]
centre = [(a + b) * 0.5 for a, b in zip(lo, hi)]
size = max(b - a for a, b in zip(lo, hi))
# An overview from above one corner of the stand.
ops.viewCamera(eye=[centre[0] + size * 0.45, centre[1] + size * 0.4, centre[2] + size * 0.45], lookAt=centre)
ops.select(clear=True)
ops.saveScene(OUT)
