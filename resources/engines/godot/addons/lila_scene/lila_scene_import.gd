@tool
extends EditorImportPlugin
## Imports a Lila scene (.lila, .lilaa) as a PackedScene: lilabatch exports it to a .glb under res://.godot/lila, and
## Godot's glTF loader builds the scene from that. Godot re-imports whenever the file changes, so saving in Lila is
## enough. lilabatch is found through Editor Settings > Lila > Batch Path, then the LILA_BATCH environment variable,
## then the default install folders, then PATH.

const BATCH_SETTING := "lila/batch_path"
const TEMP_DIR := "res://.godot/lila"


func _get_importer_name() -> String:
	return "lila.scene"


func _get_visible_name() -> String:
	return "Lila Scene"


func _get_recognized_extensions() -> PackedStringArray:
	return PackedStringArray(["lila", "lilaa"])


func _get_save_extension() -> String:
	return "scn"


func _get_resource_type() -> String:
	return "PackedScene"


func _get_priority() -> float:
	return 1.0


func _get_import_order() -> int:
	return IMPORT_ORDER_SCENE


func _get_preset_count() -> int:
	return 0


func _get_preset_name(_preset_index: int) -> String:
	return ""


func _get_import_options(_path: String, _preset_index: int) -> Array[Dictionary]:
	return []


func _get_option_visibility(_path: String, _option_name: StringName, _options: Dictionary) -> bool:
	return true


func _can_import_threaded() -> bool:
	return false


func _import(source_file: String, save_path: String, _options: Dictionary, _platform_variants: Array[String], _gen_files: Array[String]) -> Error:
	var temp_dir := ProjectSettings.globalize_path(TEMP_DIR)
	DirAccess.make_dir_recursive_absolute(temp_dir)
	var glb := temp_dir.path_join("%s.glb" % source_file.md5_text())
	var scene := ProjectSettings.globalize_path(source_file)

	OS.set_environment("LILA_CRASH_NO_UI", "1")
	var output: Array = []
	var code := OS.execute(_find_batch(), PackedStringArray(["--quiet", "--export", scene, glb]), output, true)
	if code == -1:
		push_error("Lila: lilabatch wasn't found. Set it in Editor Settings > Lila > Batch Path")
		return ERR_FILE_NOT_FOUND
	if code != 0 or not FileAccess.file_exists(glb):
		push_error("Lila: lilabatch couldn't export %s\n%s" % [source_file, "\n".join(output)])
		return ERR_CANT_CREATE

	var document := GLTFDocument.new()
	var state := GLTFState.new()
	# The default writes each texture out as a file beside the .glb.
	state.handle_binary_image = GLTFState.HANDLE_BINARY_EMBED_AS_UNCOMPRESSED
	var err := document.append_from_file(glb, state)
	DirAccess.remove_absolute(glb)
	if err != OK:
		push_error("Lila: Godot couldn't read the glTF exported from %s" % source_file)
		return err

	var root := document.generate_scene(state)
	if root == null:
		return ERR_CANT_CREATE
	root = _to_mesh_instances(root)
	root.name = source_file.get_file().get_basename()
	_own(root, root)
	var packed := PackedScene.new()
	err = packed.pack(root)
	root.free()
	if err != OK:
		return err
	return ResourceSaver.save(packed, "%s.%s" % [save_path, _get_save_extension()])


func _find_batch() -> String:
	var exe := "lilabatch.exe" if OS.get_name() == "Windows" else "lilabatch"
	var settings := EditorInterface.get_editor_settings()
	var setting := ""
	if settings.has_setting(BATCH_SETTING):
		setting = str(settings.get_setting(BATCH_SETTING))
	var candidates: Array[String] = [
		setting,
		OS.get_environment("LILA_BATCH"),
		OS.get_environment("LOCALAPPDATA").path_join("Programs/Lila").path_join(exe),
		OS.get_environment("ProgramFiles").path_join("Lila").path_join(exe),
	]
	for candidate: String in candidates:
		if not candidate.is_empty() and FileAccess.file_exists(candidate):
			return candidate
	return exe


## In the editor the glTF loader builds ImporterMeshInstance3D nodes, which only exist during import. Returns the node
## that stands in for `node`.
func _to_mesh_instances(node: Node) -> Node:
	for child: Node in node.get_children():
		_to_mesh_instances(child)
	var importer := node as ImporterMeshInstance3D
	if importer == null:
		return node
	var instance := MeshInstance3D.new()
	instance.name = importer.name
	instance.transform = importer.transform
	if importer.mesh != null:
		instance.mesh = importer.mesh.get_mesh()
	instance.skin = importer.skin
	instance.skeleton = importer.skeleton_path
	importer.replace_by(instance)
	importer.free()
	return instance


## PackedScene.pack keeps only the nodes owned by the root.
func _own(node: Node, root: Node) -> void:
	for child: Node in node.get_children():
		child.owner = root
		_own(child, root)
