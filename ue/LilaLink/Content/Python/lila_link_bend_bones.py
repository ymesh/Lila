"""Bends a skeletal mesh live as Lila's bend bones do. Lila's unrealBendBones command calls this."""

import json

import unreal


def setup(p):
    mesh = unreal.load_asset(p["skeletalMesh"])
    if mesh is None or not isinstance(mesh, unreal.SkeletalMesh):
        return {"errors": ["no skeletal mesh at " + p["skeletalMesh"]]}
    made = unreal.LilaBendBonesLibrary.make_bend_bones_deformer(p["deformer"])
    deformer, error = made if isinstance(made, tuple) else (made, "")
    if deformer is None:
        return {"errors": ["no Lila Bend Bones deformer: " + (error or "unknown")]}
    report = json.loads(unreal.LilaBendBonesLibrary.bind_bend_bones(mesh, json.dumps(p)))
    if report.get("errors"):
        return report
    if error:
        report.setdefault("warnings", []).append(error)
    mesh.set_editor_property("default_mesh_deformer", deformer)
    unreal.EditorAssetLibrary.save_loaded_asset(mesh)
    report["deformer"] = deformer.get_path_name().split(".")[0]
    return report


def setup_and_print(text):
    try:
        result = setup(json.loads(text))
    except Exception as e:  # noqa: BLE001 - reported back to Lila
        result = {"errors": ["{}: {}".format(type(e).__name__, e)]}
    print("LILA_RESULT:" + json.dumps(result))
