import os

from lila import ops

SPEC = {
    "name": "PyLift",
    "inputs": [{"name": "height", "type": "float", "default": 5.0}],
    "nodes": [{"name": "Up", "type": "CombineXYZ"}, {"name": "Move", "type": "SetPosition"}],
    "links": ["GroupInput.height -> Up.z", "Up.vector -> Move.offset", "GroupInput.geometry -> Move.geometry",
              "Move.geometry -> GroupOutput.geometry"],
}


def initialize_extension():
    # Builds its group on every load, replacing the scene's copy, as an extension under development does. The test
    # changes the group's interface between loads through PYGROUP_INPUTS.
    spec = dict(SPEC)
    extra = int(os.environ.get("PYGROUP_INPUTS", "0"))
    spec["inputs"] = SPEC["inputs"] + [{"name": f"spare{i}", "type": "float", "default": 1.0} for i in range(extra)]
    ops.gnBuildGroup(spec)
