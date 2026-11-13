"""Large-world demo, re-runnable as a check: a real georeferenced drone scan at UTM coordinates, opened, flown, edited and
sent to Unreal. Run inside Lila (Script Editor, MCP execute_python, or lilabatch) with LILA_DEMO_SCAN pointing at the scan,
by default Asiago_postTaglio_nuvola-densa-drone-UTM32.laz (F. Pirotti, "LiDAR and Photogrammetry Point Clouds",
https://doi.org/10.5281/zenodo.7421524, CC BY 4.0). Prints one JSON report; "ok" is false when a check fails.
"""

import json
import os
import tempfile
import time

from lila import ops

SCAN = os.environ.get("LILA_DEMO_SCAN", os.path.join(tempfile.gettempdir(), "lila_demo", "Asiago_postTaglio_nuvola-densa-drone-UTM32.laz"))
OUT = os.environ.get("LILA_DEMO_OUT", os.path.join(tempfile.gettempdir(), "lila_demo"))
os.makedirs(OUT, exist_ok=True)
report = {"scan": SCAN, "checks": {}, "seconds": {}}
_started = time.time()


def mark(step):
    """Seconds since the start, per step, also written to demo.log so a hang shows where it stopped."""
    report["seconds"][step] = round(time.time() - _started, 2)
    with open(os.path.join(OUT, "demo.log"), "a") as f:
        f.write("%s %.2f\n" % (step, time.time() - _started))


def check(name, ok, **detail):
    report["checks"][name] = dict(ok=bool(ok), **detail)


def capture(name, **kw):
    path = os.path.join(OUT, name + ".png")
    ops.captureView(view="active", width=480, height=320, label=False, grid=False, path=path, **kw)
    return path


def pixels(path):
    from PySide6 import QtGui

    img = QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB888)
    data = bytes(img.constBits())[: img.sizeInBytes()]
    return img.width(), img.height(), data


def diff(a, b):
    """Largest channel difference and how many bytes differ by more than 2."""
    worst, count = 0, 0
    for x, y in zip(a[2], b[2]):
        d = abs(x - y)
        worst = max(worst, d)
        count += d > 2
    return worst, count


# 1. Open, in a new scene: the scan's CRS sets the scene's georeference.
# This survey carries no CRS; its file name says UTM zone 32 (WGS 84), so the demo puts that beside it as a .prj, as the
# survey tool would have.
ops.newScene()
prj = os.path.splitext(SCAN)[0] + ".prj"
if not os.path.exists(prj):
    with open(prj, "w") as f:
        f.write("EPSG:32632")
    report["prjWritten"] = prj
ops.setGeoreference(clear=True)
t = time.time()
imported = ops.importPointCloud(SCAN)
report["import"] = imported
report["importSeconds"] = round(time.time() - t, 2)
mark("imported")
geo = ops.georeference()
report["georeference"] = {k: geo.get(k) for k in ("authority", "name", "origin", "wgs84")}
check("georeferenced", geo is not None and str(geo.get("authority", "")).startswith("EPSG:326"), crs=geo and geo.get("authority"))
cloud = imported["node"]
info = ops.pointCloudInfo(cloud)
lo, hi = info["boundsMin"], info["boundsMax"]
centre = [(a + b) * 0.5 for a, b in zip(lo, hi)]
size = max(b - a for a, b in zip(lo, hi))
report["extentMetres"] = [round((b - a) / 100.0, 1) for a, b in zip(lo, hi)]

# 2. Fly: a low pass over the scan, the camera at the scan's real UTM coordinates (the cloud moved out there) against
# the same view near the origin. Camera-relative drawing makes them the same picture, and a millimetre step a smooth one.
eye_offset = [size * 0.15, size * 0.08, size * 0.15]


def view_at(base):
    ops.viewCamera(eye=[base[i] + centre[i] + eye_offset[i] for i in range(3)], lookAt=[base[i] + centre[i] for i in range(3)])


home = [0.0, 0.0, 0.0]
view_at(home)
near = pixels(capture("fly_origin"))
mark("captured origin")
utm = [geo["origin"][1] * 100.0, 0.0, geo["origin"][0] * 100.0] if not geo.get("zUp") else [geo["origin"][0] * 100.0, geo["origin"][1] * 100.0, 0.0]
ops.setAttr(cloud + ".translate", utm)
view_at(utm)
far = pixels(capture("fly_utm"))
mark("captured utm")
worst, count = diff(near, far)
# Within 1 km the origin view draws in world space and the UTM view camera-relative, so a few splat edges may round
# differently; anything beyond a handful of edge pixels is a real difference.
check("sameAtUtm", worst <= 3 or count < len(near[2]) // 2000, maxDiff=worst, differing=count, utmCm=utm)
step = [utm[0] + 0.1, utm[1], utm[2]]
ops.setAttr(cloud + ".translate", step)
view_at(step)
crept = pixels(capture("fly_utm_1mm"))
worst, count = diff(far, crept)
check("noJitter", worst <= 8, maxDiff=worst, differing=count)
# The Drone Camera over the scan at its UTM place: hold W for a second, then the view must still be the scan, not noise.
try:
    from PySide6 import QtCore, QtGui, QtWidgets

    import lila.ui as lui

    app = QtWidgets.QApplication.instance()
    views = [w for w in app.allWidgets() if w.metaObject().className() == "lila::ui::ViewportWidget" and w.isVisible()]
except Exception:
    views = []
if views:
    ops.setAttr(cloud + ".translate", utm)
    view_at(utm)
    vp = views[0]
    vp.window().activateWindow()
    vp.setFocus(QtCore.Qt.OtherFocusReason)
    app.processEvents()
    lui.run_action("view.drone")
    start = ops.viewCamera()["persp"]["target"]
    QtWidgets.QApplication.sendEvent(vp, QtGui.QKeyEvent(QtCore.QEvent.KeyPress, QtCore.Qt.Key_W, QtCore.Qt.NoModifier, "w"))
    t0 = time.time()
    while time.time() - t0 < 1.0:
        app.processEvents(QtCore.QEventLoop.AllEvents, 16)
    QtWidgets.QApplication.sendEvent(vp, QtGui.QKeyEvent(QtCore.QEvent.KeyRelease, QtCore.Qt.Key_W, QtCore.Qt.NoModifier, "w"))
    app.processEvents()
    end = ops.viewCamera()["persp"]["target"]
    lui.run_action("view.drone")
    flown = sum((b - a) ** 2 for a, b in zip(start, end)) ** 0.5
    after = pixels(capture("drone_utm"))
    lit = sum(1 for i in range(0, len(after[2]), 3) if after[2][i + 1] > after[2][i] + 10)  # greenish pixels: the forest
    check("droneFlight", flown > 10.0 and lit > len(after[2]) // 30, flownCm=round(flown, 1), forestPixels=lit)
    mark("flown")
ops.setAttr(cloud + ".translate", home)

# 3. Edit: a clip box around the middle, its points out as a mesh, a marker block at the scan's centre.
box = ops.pointCloudClipBox(cloud)
extracted = ops.pointCloudExtract(cloud, spacing=10.0)
marker = ops.polyCube(width=200, height=200, depth=200)
marker = marker[0] if isinstance(marker, (list, tuple)) else marker
ops.setAttr(marker + ".translate", centre)
report["edits"] = {"clipBox": box, "extract": extracted, "marker": marker}
lat = ops.georeferenceConvert(points=[centre], to="geographic")[0]
report["markerLatLonHeight"] = lat
check("markerOnEarth", 45.0 < lat[0] < 47.0 and 10.0 < lat[1] < 13.0, latLon=lat[:2])

mark("edited")

# 4. Send to Unreal, only with LILA_DEMO_UNREAL=1: it changes the open level of whatever editor LilaLink reaches. The level
# takes the georeference and the marker lands at the scan's centre under Cesium.
if os.environ.get("LILA_DEMO_UNREAL") == "1":
    try:
        report["unreal"] = ops.unrealPlaceInstances(marker, wait=True)
        check("unrealSent", True)
    except Exception as e:
        report["unreal"] = "failed: " + str(e)
        check("unrealSent", False, error=str(e))
else:
    report["unreal"] = "skipped (set LILA_DEMO_UNREAL=1 with a throwaway Unreal project open)"

report["ok"] = all(c["ok"] for c in report["checks"].values())
print(json.dumps(report, indent=1, default=str))
