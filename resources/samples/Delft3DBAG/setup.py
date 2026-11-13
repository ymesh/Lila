"""Turns the downloaded 3DBAG tiles into delft.lila. Lila runs it after the download with SOURCE (the downloaded folder) and
OUT (the .lila to write) set; to run it by hand, set both first."""

import os

from lila import ops

tileset = os.path.join(SOURCE, "tileset.json")
if not os.path.exists(tileset):
    raise RuntimeError("tileset.json is not in the download")

ops.newScene()
ops.setGeoreference(clear=True)
# Only the downloaded tiles: the ones within 350 m of the Markt.
ops.import3dTiles(tileset, centre=[52.0116, 4.3586], radius=350, name="Delft")
ops.viewCamera(eye=[-30000, 22000, 32000], lookAt=[0, 0, 0])
ops.select(clear=True)
ops.saveScene(OUT)
