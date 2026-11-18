@tool
extends EditorPlugin

const LilaSceneImport := preload("lila_scene_import.gd")
const LilaSync := preload("lila_sync.gd")

var _importer: EditorImportPlugin
var _sync := LilaSync.new()
var _sync_button := CheckButton.new()


func _enter_tree() -> void:
	var settings := EditorInterface.get_editor_settings()
	if not settings.has_setting(LilaSceneImport.BATCH_SETTING):
		settings.set_setting(LilaSceneImport.BATCH_SETTING, "")
	settings.set_initial_value(LilaSceneImport.BATCH_SETTING, "", false)
	settings.add_property_info({
		"name": LilaSceneImport.BATCH_SETTING,
		"type": TYPE_STRING,
		"hint": PROPERTY_HINT_GLOBAL_FILE,
	})
	_importer = LilaSceneImport.new()
	add_import_plugin(_importer)

	_sync_button.text = "Lila Sync"
	_sync_button.tooltip_text = "Move objects from Lila as they move in Lila"
	_sync_button.toggled.connect(_on_sync_toggled)
	add_control_to_container(CONTAINER_TOOLBAR, _sync_button)
	set_process(false)


func _exit_tree() -> void:
	remove_import_plugin(_importer)
	_importer = null
	_sync.stop()
	remove_control_from_container(CONTAINER_TOOLBAR, _sync_button)
	_sync_button.queue_free()


func _on_sync_toggled(on: bool) -> void:
	if on:
		_sync.start()
	else:
		_sync.stop()
	set_process(on)


func _process(_delta: float) -> void:
	var moved := _sync.poll()
	if _sync.has_failed():
		push_warning("Lila: can't reach Lila on port %d. Turn on Preferences > General > Change Stream in Lila." % _sync.port)
		_sync_button.button_pressed = false
		return
	var root := EditorInterface.get_edited_scene_root()
	if root != null and not moved.is_empty():
		LilaSync.apply(root, moved)
		EditorInterface.mark_scene_as_unsaved()
