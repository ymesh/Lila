"""Shapes a skeletal mesh live as its Lila Soft Net does. Lila's unrealSoftNet command calls this."""

import json

import unreal


def setup(p):
    mesh = unreal.load_asset(p["skeletalMesh"])
    if mesh is None or not isinstance(mesh, unreal.SkeletalMesh):
        return {"errors": ["no skeletal mesh at " + p["skeletalMesh"]]}
    made = unreal.LilaSoftNetLibrary.make_soft_net_deformer(p["deformer"])
    deformer, error = made if isinstance(made, tuple) else (made, "")
    if deformer is None:
        return {"errors": ["no Lila Soft Net deformer: " + (error or "unknown")]}
    # The net's offsets ride on the skin, so its weights keep their full precision rather than 8 bits.
    precise = []
    sub = unreal.get_editor_subsystem(unreal.SkeletalMeshEditorSubsystem)
    for lod in range(mesh.get_lod_num() if hasattr(mesh, "get_lod_num") else 1):
        settings = sub.get_lod_build_settings(mesh, lod)
        if not settings.get_editor_property("use_high_precision_skin_weights"):
            settings.set_editor_property("use_high_precision_skin_weights", True)
            sub.set_lod_build_settings(mesh, lod, settings)
            precise.append(lod)
    report = json.loads(unreal.LilaSoftNetLibrary.bind_soft_net(mesh, json.dumps(p)))
    if report.get("errors"):
        return report
    if error:
        report.setdefault("warnings", []).append(error)
    if precise:
        report.setdefault("warnings", []).append("turned on high precision skin weights for LODs " + ", ".join(str(l) for l in precise) + " so the skin matches Lila's")
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
