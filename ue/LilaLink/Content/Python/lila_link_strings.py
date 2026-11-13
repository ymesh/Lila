"""Builds the Control Rig that poses a character the way Lila's Strings do. Lila's unrealStringsRig command calls this."""

import json

import unreal

from lila_link_expressions import _begin_pin, _ensure_rig

UNIT = "/Script/LilaLinkRig.RigUnit_LilaStrings"
PREFIX = "LilaStrings_"
CONTROL_PREFIX = "LS_"
EXTERNAL_PREFIX = "LS_x_"
# Lila's mode names in ELilaStringsMode's order: a control's Mode channel holds the index.
MODES = ["default", "free", "forward", "forwardAll", "backward", "backwardAll", "bidirectional", "all", "ik", "fullBody"]
MODE_CHANNEL = "Mode"
ISOLATE_CHANNEL = "Isolate Rotation"
PIN_CHANNEL = "Pin"
LOCK_CHANNEL = "Lock"


def _add_control(hierarchy, hc, bone):
    """A transform control named after the bone, where the bone is at rest, so dragging it poses the bone."""
    name = CONTROL_PREFIX + bone
    key = unreal.RigElementKey(unreal.RigElementType.CONTROL, name)
    if hierarchy.contains(key):
        return name
    bone_key = unreal.RigElementKey(unreal.RigElementType.BONE, bone)
    if not hierarchy.contains(bone_key):
        return None
    settings = unreal.RigControlSettings()
    settings.control_type = unreal.RigControlType.EULER_TRANSFORM
    settings.display_name = bone
    settings.shape_name = "Sphere_Thick"
    rest = hierarchy.get_global_transform(bone_key, True)
    value = unreal.RigHierarchy.make_control_value_from_euler_transform(unreal.EulerTransform(rest.translation, rest.rotation.rotator(), rest.scale3d))
    hc.add_control(name, unreal.RigElementKey(), settings, value, False, False)
    return name


def _channel(hierarchy, hc, control, name, settings, value):
    """The control's animation channel called name, added when missing, set to value (initial and current)."""
    parent = unreal.RigElementKey(unreal.RigElementType.CONTROL, control)
    key = None
    for child in hierarchy.get_children(parent):
        if child.type == unreal.RigElementType.CONTROL:
            settings_of = hierarchy.get_control_settings(child)
            if settings_of.animation_type == unreal.RigControlAnimationType.ANIMATION_CHANNEL and str(settings_of.display_name) == name:
                key = child
    if key is None:
        settings.display_name = name
        key = hc.add_animation_channel(name, parent, settings, False, False)
    for kind in (unreal.RigControlValueType.INITIAL, unreal.RigControlValueType.CURRENT):
        hierarchy.set_control_value(key, value, kind)
    return key


def _add_channels(hierarchy, hc, control, mode, isolate):
    """A Mode channel (Lila's modes as a dropdown) and an Isolate Rotation channel on the control, which its unit reads."""
    settings = unreal.RigControlSettings()
    settings.control_type = unreal.RigControlType.INTEGER
    channel = _channel(hierarchy, hc, control, MODE_CHANNEL, settings, unreal.RigHierarchy.make_control_value_from_int(MODES.index(mode)))
    unreal.LilaStringsLibrary.set_mode_enum(hierarchy, channel)
    settings = unreal.RigControlSettings()
    settings.control_type = unreal.RigControlType.BOOL
    _channel(hierarchy, hc, control, ISOLATE_CHANNEL, settings, unreal.RigHierarchy.make_control_value_from_bool(isolate))


def _add_external(hierarchy, hc, member):
    """A control for a prop or another character's joint glued or attached to this character, where it is now."""
    name = EXTERNAL_PREFIX + "".join(c if c.isalnum() else "_" for c in member["name"])
    key = unreal.RigElementKey(unreal.RigElementType.CONTROL, name)
    settings = unreal.RigControlSettings()
    settings.control_type = unreal.RigControlType.EULER_TRANSFORM
    settings.display_name = member["name"]
    settings.shape_name = "Box_Thick"
    loc, q = member["location"], member["rotation"]
    t = unreal.Transform(unreal.Vector(*loc), unreal.Quat(*q).rotator(), unreal.Vector(1.0, 1.0, 1.0))
    if not hierarchy.contains(key):
        value = unreal.RigHierarchy.make_control_value_from_euler_transform(unreal.EulerTransform(t.translation, t.rotation.rotator(), t.scale3d))
        hc.add_control(name, unreal.RigElementKey(), settings, value, False, False)
    unreal.LilaStringsLibrary.place_control(hierarchy, name, unreal.Vector(*loc), unreal.Quat(*q))
    return name


def _add_switch(hierarchy, hc, control, name, on):
    settings = unreal.RigControlSettings()
    settings.control_type = unreal.RigControlType.BOOL
    return _channel(hierarchy, hc, control, name, settings, unreal.RigHierarchy.make_control_value_from_bool(bool(on)))


SOURCE_UNIT = "/Script/LilaLinkRig.RigUnit_LilaStringsSource"
TAG_PREFIX = "LilaStrings_"


def _actors():
    return unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def _character_actor(rig, mesh, character):
    """The level's actor wearing the mesh this rig poses (made at the origin when missing), tagged with the character."""
    label = rig.get_name()
    actor = next((a for a in _actors().get_all_level_actors() if isinstance(a, unreal.SkeletalMeshActor) and a.get_actor_label() == label), None)
    if actor is None:
        actor = _actors().spawn_actor_from_class(unreal.SkeletalMeshActor, unreal.Vector(0.0, 0.0, 0.0))
        actor.set_actor_label(label)
    actor.skeletal_mesh_component.set_skeletal_mesh_asset(mesh)
    tag = unreal.Name(TAG_PREFIX + character)
    if tag not in actor.tags:
        actor.tags = list(actor.tags) + [tag]
    return actor


def _prop_actor(member):
    """The level actor Lila sent for a prop: labelled with the prop's name."""
    name = member["name"]
    return next((a for a in _actors().get_all_level_actors() if a.get_actor_label() == name), None)


def _bind(hierarchy, controller, rule_web, actor, warnings):
    """
    Binds what a glue or attach set's lead moves, one way: a prop following a bone of this character is attached to it;
    a prop or another character's bone that leads drives its control here through a Source node. Returns the nodes.
    """
    nodes = []
    for rel in rule_web.get("glue", []):
        members = rel["members"]
        lead = members[rel.get("lead", len(members) - 1 if rel.get("attach") else 0)]
        if isinstance(lead, str):
            for m in members:
                if isinstance(m, dict) and "prop" in m:
                    prop = _prop_actor(m)
                    if prop is None:
                        warnings.append("no actor labelled " + m["name"] + " to attach to " + lead + "; send the prop first")
                        continue
                    prop.attach_to_component(actor.skeletal_mesh_component, lead, unreal.AttachmentRule.KEEP_WORLD,
                                             unreal.AttachmentRule.KEEP_WORLD, unreal.AttachmentRule.KEEP_WORLD, False)
            continue
        key = unreal.RigElementKey(unreal.RigElementType.CONTROL, lead["control"])
        control_world = hierarchy.get_global_transform(key, True).multiply(actor.get_actor_transform())
        if "prop" in lead:
            source = _prop_actor(lead)
            if source is None:
                warnings.append("no actor labelled " + lead["name"] + " to lead the glue; send the prop first")
                continue
            tag, bone = TAG_PREFIX + "prop_" + lead["name"], ""
            if unreal.Name(tag) not in source.tags:
                source.tags = list(source.tags) + [unreal.Name(tag)]
            offset = control_world.make_relative(source.get_actor_transform())
        else:
            tag, bone = TAG_PREFIX + lead["character"], lead["bone"]
            other = next((a for a in _actors().get_all_level_actors() if unreal.Name(tag) in a.tags), None)
            socket = other.skeletal_mesh_component.get_socket_transform(bone) if other else None
            offset = control_world.make_relative(socket) if socket else unreal.Transform()
            if other is None:
                warnings.append("build " + lead["character"] + "'s Strings rig too, so " + lead["control"] + " follows its " + bone)
        node = controller.add_unit_node_from_struct_path(SOURCE_UNIT, "Execute", unreal.Vector2D(400.0, -200.0 - 200.0 * len(nodes)),
                                                         PREFIX + "Source_" + lead["control"], False)
        name = node.get_name()
        controller.set_pin_default_value(name + ".Control", lead["control"], False, False)
        controller.set_pin_default_value(name + ".SourceTag", tag, False, False)
        controller.set_pin_default_value(name + ".Bone", bone, False, False)
        t, r = offset.translation, offset.rotation
        controller.set_pin_default_value(name + ".Offset", "(Rotation=(X={},Y={},Z={},W={}),Translation=(X={},Y={},Z={}),Scale3D=(X=1,Y=1,Z=1))".format(
            r.x, r.y, r.z, r.w, t.x, t.y, t.z), False, False)
        nodes.append(name)
    return nodes


def _lock_sequence(p, rig, mesh, lock_channels, locks, errors, actor=None):
    """The chains' lock keys on the rig's Lock channels, in a Level Sequence binding the character's actor to the rig."""
    keyed = [l for l in locks if l.get("keys") and l["bone"] in lock_channels]
    if not keyed and actor is None:
        return None
    rig_path = rig.get_path_name().split(".")[0]
    path = p.get("sequence") or rig_path.rsplit("/", 1)[0] + "/LS_" + rig_path.rsplit("/", 1)[1]
    folder, name = path.rsplit("/", 1)
    seq = unreal.load_asset(path)
    if seq is None:
        seq = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, folder, unreal.LevelSequence, unreal.LevelSequenceFactoryNew())
    if seq is None:
        errors.append("couldn't make the Level Sequence " + path)
        return None
    fps = float(p.get("fps") or 30.0)
    seq.set_display_rate(unreal.FrameRate(int(round(fps)), 1))
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    label = actor.get_actor_label()
    binding = next((b for b in seq.get_possessables() if b.get_display_name() == label), None) or seq.add_possessable(actor)
    track = unreal.ControlRigSequencerLibrary.find_or_create_control_rig_track(world, seq, rig.generated_class(), binding)
    control_rig = None
    for proxy in unreal.ControlRigSequencerLibrary.get_control_rigs(seq):
        if proxy.track == track:
            control_rig = proxy.control_rig
    if control_rig is None:
        errors.append("the Level Sequence has no Control Rig track for " + rig_path)
        return None
    sections = track.get_sections() or [track.add_section()]
    frames = []
    written = {}
    for lock in keyed:
        channel = lock_channels[lock["bone"]]
        keys = [[int(round(t * fps)), bool(on)] for t, on in lock["keys"]]
        if not unreal.LilaStringsLibrary.key_rig_bools(sections[0], channel, [k[0] for k in keys], [k[1] for k in keys]):
            errors.append("couldn't key " + channel + " in " + path)
        frames += [k[0] for k in keys]
        written[lock["bone"]] = keys
    if frames:
        seq.set_playback_start(min(frames))
        seq.set_playback_end(max(frames) + 1)
    unreal.EditorAssetLibrary.save_loaded_asset(seq)
    return {"sequence": path, "keys": written, "channels": lock_channels, "rig": rig_path}


def _relation_channel(rel, taken):
    """Where a glue or attach set's switch goes: an attach's on the member the others follow, a glue's on its first."""
    members = [m if isinstance(m, str) else m["name"] for m in rel["members"]]
    bones = [m for m in rel["members"] if isinstance(m, str)]
    # The switch lives on a bone's control: the leader's for an attach when it is a bone, else the first bone.
    owner = members[-1] if rel.get("attach") and isinstance(rel["members"][-1], str) else bones[0]
    others = [m for m in members if m != owner]
    name = ("Attach " if rel.get("attach") else "Glue ") + others[0] + (" +{}".format(len(others) - 1) if len(others) > 1 else "")
    base, n = name, 2
    while (owner, name) in taken:
        name, n = "{} {}".format(base, n), n + 1
    taken.add((owner, name))
    return owner, name


def build(p):
    errors, warnings = [], []
    mesh = unreal.load_asset(p["skeletalMesh"])
    if mesh is None:
        return {"errors": ["no skeletal mesh at " + p["skeletalMesh"]]}
    rig, created = _ensure_rig(p["rig"], mesh, errors)
    if rig is None:
        return {"errors": errors}

    hierarchy = rig.get_hierarchy()
    hc = rig.get_hierarchy_controller()
    hc.import_bones_from_asset(mesh.get_path_name(), "None", True, True, False, False)

    controller = rig.get_controller_by_name("RigVMModel") or rig.get_controller()
    graph = controller.get_graph()
    begin = _begin_pin(controller)
    if begin is None:
        return {"errors": errors + ["the rig has no Forwards Solve event"]}

    # What ran after Forwards Solve before (the user's own graph) runs after the Strings units.
    after = sorted({link.get_target_pin().get_pin_path() for link in graph.get_links()
                    if link.get_source_pin().get_pin_path() == begin and not link.get_target_pin().get_node().get_name().startswith(PREFIX)})
    for node in list(graph.get_nodes()):
        if node.get_name().startswith(PREFIX):
            controller.remove_node(node, False, False)
    for path in after:
        controller.break_link(begin, path, False)

    mode = p.get("mode", "fullBody")
    if mode not in MODES:
        return {"errors": errors + ["no Strings mode " + mode + " (" + ", ".join(MODES) + ")"]}
    isolate = bool(p.get("isolate", False))
    rule_web = dict(p["ruleWeb"])
    pins = set(rule_web.get("pins", []))
    locks_list = rule_web.get("locks", [])
    locks = {l["bone"]: bool(l.get("locked")) for l in locks_list}
    controls = {}
    lock_channels = {}
    for bone in p.get("controls", []):
        control = _add_control(hierarchy, hc, bone)
        if control is None:
            warnings.append("no bone named " + bone + " on " + mesh.get_name())
            continue
        unreal.LilaStringsLibrary.place_control_at_bone(hierarchy, control, bone)
        _add_channels(hierarchy, hc, control, mode, isolate)
        _add_switch(hierarchy, hc, control, PIN_CHANNEL, bone in pins)
        if bone in locks:
            lock_channels[bone] = str(_add_switch(hierarchy, hc, control, LOCK_CHANNEL, locks[bone]).name)
        controls[bone] = control
    # The units find each bone's Pin and Lock channels, and each set's switch, through the web.
    rule_web["controls"] = controls
    glue, taken = [], set()
    externals = {}
    for rel in rule_web.get("glue", []):
        rel = dict(rel)
        # Props and other characters' joints get a control of their own where they are.
        members = []
        for m in rel["members"]:
            if not isinstance(m, str):
                m = dict(m)
                m["control"] = externals.setdefault(m["name"], _add_external(hierarchy, hc, m))
            members.append(m)
        rel["members"] = members
        owner, name = _relation_channel(rel, taken)
        if owner in controls:
            _add_switch(hierarchy, hc, controls[owner], name, True)
            rel["channel"] = {"control": controls[owner], "name": name}
        glue.append(rel)
    rule_web["glue"] = glue
    web = json.dumps(rule_web)
    prev = begin
    made = []
    externals_bound = any(isinstance(m, dict) for rel in glue for m in rel["members"])
    actor = _character_actor(rig, mesh, rule_web.get("character", "")) if externals_bound or any(l.get("keys") for l in locks_list) else None
    if actor is not None:
        for name in _bind(hierarchy, controller, rule_web, actor, warnings):
            controller.add_link(prev, name + ".ExecutePin", False)
            prev = name + ".ExecutePin"
            made.append(name)
    for i, (bone, control) in enumerate([(b, controls[b]) for b in p.get("controls", []) if b in controls] +
                                        [("", c) for c in externals.values()]):
        node = controller.add_unit_node_from_struct_path(UNIT, "Execute", unreal.Vector2D(400.0 + 360.0 * i, 200.0), PREFIX + (bone or control), False)
        if node is None:
            errors.append("couldn't add a Lila Strings unit (is the LilaLinkRig module loaded?)")
            break
        name = node.get_name()
        controller.set_pin_default_value(name + ".RuleWeb", web, False, False)
        controller.set_pin_default_value(name + ".Bone", bone, False, False)
        controller.set_pin_default_value(name + ".Control", control, False, False)
        controller.set_pin_default_value(name + ".Mode", "IK" if mode == "ik" else mode[0].upper() + mode[1:], False, False)
        controller.set_pin_default_value(name + ".bIsolateRotation", "True" if isolate else "False", False, False)
        controller.add_link(prev, name + ".ExecutePin", False)
        prev = name + ".ExecutePin"
        made.append(name)
    for path in after:
        controller.add_link(prev, path, False)

    unreal.BlueprintEditorLibrary.compile_blueprint(rig)
    unreal.EditorAssetLibrary.save_loaded_asset(rig)
    sequence = _lock_sequence(p, rig, mesh, lock_channels, locks_list, errors, actor)
    return {"rig": rig.get_path_name().split(".")[0], "created": created, "nodes": made, "mode": mode, "isolate": isolate,
            "pins": sorted(pins & set(controls)), "locks": sorted(b for b in locks if b in controls),
            "glue": [g.get("channel") for g in glue], "lockSequence": sequence, "errors": errors, "warnings": warnings}


def build_and_print(text):
    try:
        result = build(json.loads(text))
    except Exception as e:  # noqa: BLE001 - reported back to Lila
        result = {"errors": ["{}: {}".format(type(e).__name__, e)]}
    print("LILA_RESULT:" + json.dumps(result))
