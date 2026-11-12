@tool
extends EditorScript
## Builds a Lila settlement export (settlementExport format='godot') in the open scene. Copy the export folder into the
## project, open the scene to put the settlement in, open this script in the Script Editor and run it (File > Run).
## Streets and massing come in from the FBX files beside it (Godot 4.3 or later imports FBX). Each placement instances
## the scene at its asset path when that is a res:// path that exists, else a Marker3D stands in. Lots, envelopes and
## junctions become Marker3D nodes carrying their fields as metadata. Save the scene to keep it.

const SETTLEMENT_JSON := "%SETTLEMENT_JSON%"


func _run() -> void:
	var dir := (get_script() as Script).resource_path.get_base_dir()
	var text := FileAccess.get_file_as_string(dir.path_join(SETTLEMENT_JSON))
	var parsed: Variant = JSON.parse_string(text)
	if typeof(parsed) != TYPE_DICTIONARY:
		push_error("Lila: can't read %s" % dir.path_join(SETTLEMENT_JSON))
		return
	var info: Dictionary = parsed
	if info.get("format", "") != "lila.settlement.engine" or info.get("target", "") != "godot":
		push_error("Lila: %s isn't a Lila Godot settlement export" % dir.path_join(SETTLEMENT_JSON))
		return
	var scene_root := EditorInterface.get_edited_scene_root()
	if scene_root == null:
		push_error("Lila: open the scene to import the settlement into first")
		return

	var root := Node3D.new()
	root.name = str(info.get("name", "Settlement"))
	scene_root.add_child(root)
	root.owner = scene_root

	var meshes: Dictionary = info.get("meshes", {})
	_add_model(root, scene_root, dir, str(meshes.get("streets", "")), "Streets")
	_add_model(root, scene_root, dir, str(meshes.get("massing", "")), "Massing")

	var lots := _group(root, scene_root, "Lots")
	var lot_list: Array = info.get("lots", [])
	for lot: Dictionary in lot_list:
		var m := _marker(lots, scene_root, "%s_%s" % [lot.get("key", ""), lot.get("use", "")], _centroid(lot.get("outline", [])))
		for field: String in ["key", "use", "area", "frontages", "corner", "frontClass", "block"]:
			if lot.has(field):
				m.set_meta(field, lot[field])

	var envelopes := _group(root, scene_root, "Envelopes")
	var envelope_list: Array = info.get("envelopes", [])
	for env: Dictionary in envelope_list:
		var m := _marker(envelopes, scene_root, "%s_%s" % [env.get("key", ""), env.get("use", "")], _centroid(env.get("footprint", [])))
		for field: String in ["key", "use", "floors", "floorHeight", "height", "base", "ground", "top", "roof"]:
			if env.has(field):
				m.set_meta(field, env[field])

	var junctions := _group(root, scene_root, "Junctions")
	var junction_list: Array = info.get("junctions", [])
	for j: Dictionary in junction_list:
		var m := _marker(junctions, scene_root, "%s_%s" % [j.get("key", ""), j.get("control", "")], _vec(j.get("position", []), Vector3.ZERO))
		m.set_meta("control", j.get("control", ""))
		m.set_meta("approaches", j.get("approaches", 0))
		m.set_meta("phases", j.get("phases", []))
		m.set_meta("walks", j.get("walks", []))

	var placed := _group(root, scene_root, "Placements")
	var placement_list: Array = info.get("placements", [])
	var missing := 0
	for p: Dictionary in placement_list:
		var asset := str(p.get("asset", ""))
		var node: Node3D = null
		if asset.begins_with("res://") and ResourceLoader.exists(asset):
			var scene := load(asset) as PackedScene
			if scene != null:
				node = scene.instantiate() as Node3D
		if node == null:
			node = Marker3D.new()
			missing += 1
		node.name = "place_%s" % str(p.get("id", ""))
		placed.add_child(node)
		node.owner = scene_root
		node.position = _vec(p.get("position", []), Vector3.ZERO)
		var r: Array = p.get("rotation", [])
		if r.size() == 4:
			node.quaternion = Quaternion(float(r[0]), float(r[1]), float(r[2]), float(r[3]))
		node.scale = _vec(p.get("scale", []), Vector3.ONE)
		node.set_meta("asset", asset)
		node.set_meta("tags", p.get("tags", []))
	if missing > 0:
		push_warning("Lila: %d placements had no res:// scene at their asset path; Marker3D nodes stand in" % missing)
	print("Lila: imported settlement %s (%d lots, %d placements)" % [root.name, lot_list.size(), placement_list.size()])


func _group(root: Node3D, scene_root: Node, label: String) -> Node3D:
	var g := Node3D.new()
	g.name = label
	root.add_child(g)
	g.owner = scene_root
	return g


func _marker(parent: Node3D, scene_root: Node, label: String, at: Vector3) -> Marker3D:
	var m := Marker3D.new()
	m.name = label.validate_node_name()
	parent.add_child(m)
	m.owner = scene_root
	m.position = at
	return m


func _vec(v: Array, fallback: Vector3) -> Vector3:
	if v.size() < 3:
		return fallback
	return Vector3(float(v[0]), float(v[1]), float(v[2]))


func _centroid(flat: Array) -> Vector3:
	var n: int = floori(flat.size() / 3.0)
	if n == 0:
		return Vector3.ZERO
	var sum := Vector3.ZERO
	for i in n:
		sum += Vector3(float(flat[i * 3]), float(flat[i * 3 + 1]), float(flat[i * 3 + 2]))
	return sum / n


func _add_model(root: Node3D, scene_root: Node, dir: String, file: String, label: String) -> void:
	if file.is_empty():
		return
	var path := dir.path_join(file)
	if not ResourceLoader.exists(path):
		push_warning("Lila: %s isn't imported; %s skipped" % [path, label])
		return
	var scene := load(path) as PackedScene
	if scene == null:
		push_warning("Lila: %s isn't a scene; %s skipped" % [path, label])
		return
	var model := scene.instantiate()
	model.name = label
	root.add_child(model)
	model.owner = scene_root
