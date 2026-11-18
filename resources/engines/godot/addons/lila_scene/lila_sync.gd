## Reads Lila's change stream (Preferences > General > Change Stream in Lila) and returns each object's world transform
## as it changes. apply() moves the matching nodes in scenes that came from Lila.
extends RefCounted

var port := 8742
var _tcp := StreamPeerTCP.new()
var _buffer := PackedByteArray()
var _subscribed := false
var _started_msec := 0


func start() -> void:
	_started_msec = Time.get_ticks_msec()
	_tcp.connect_to_host("127.0.0.1", port)


func stop() -> void:
	_tcp.disconnect_from_host()
	_buffer.clear()
	_subscribed = false


## True once Lila didn't answer within 3 seconds or went away.
func has_failed() -> bool:
	match _tcp.get_status():
		StreamPeerTCP.STATUS_ERROR:
			return true
		StreamPeerTCP.STATUS_CONNECTING:
			return Time.get_ticks_msec() - _started_msec > 3000
		StreamPeerTCP.STATUS_NONE:
			return _subscribed
	return false


## Object name -> Transform3D for everything that moved since the last call.
func poll() -> Dictionary:
	var moved := {}
	_tcp.poll()
	if _tcp.get_status() != StreamPeerTCP.STATUS_CONNECTED:
		return moved
	if not _subscribed:
		_send({"op": "subscribe", "snapshot": true, "world": true, "live": true, "types": ["transform"]})
		_subscribed = true
	var available := _tcp.get_available_bytes()
	if available > 0:
		_buffer.append_array(_tcp.get_data(available)[1])
	var end := _buffer.find(10)
	while end != -1:
		var message: Variant = JSON.parse_string(_buffer.slice(0, end).get_string_from_utf8())
		_buffer = _buffer.slice(end + 1)
		if message is Dictionary:
			_read(message, moved)
		end = _buffer.find(10)
	return moved


## Moves the nodes with those names inside every scene from Lila under root: sent ones (res://lila/) and .lila files.
static func apply(root: Node, moved: Dictionary) -> void:
	for instance: Node in _lila_instances(root, []):
		for object_name: String in moved:
			var node := instance.find_child(object_name, true, false) as Node3D
			if node != null:
				node.global_transform = moved[object_name]


func _read(message: Dictionary, moved: Dictionary) -> void:
	match message.get("type"):
		"snapshot":
			for node: Dictionary in message["nodes"]:
				if node.has("world"):
					moved[_name(node["path"])] = _to_transform(node["world"])
		"group", "live", "frame":
			for change: Dictionary in message["changes"]:
				if change["kind"] == "world":
					moved[_name(change["path"])] = _to_transform(change["matrix"])
		"resync":
			_send({"op": "snapshot"})


func _send(request: Dictionary) -> void:
	_tcp.put_data((JSON.stringify(request) + "\n").to_utf8_buffer())


func _name(path: String) -> String:
	return path.get_slice("|", path.get_slice_count("|") - 1)


## Lila's matrices are column by column in centimetres, Y up: the same axes as Godot.
func _to_transform(m: Array) -> Transform3D:
	var basis := Basis(Vector3(m[0], m[1], m[2]), Vector3(m[4], m[5], m[6]), Vector3(m[8], m[9], m[10]))
	return Transform3D(basis, Vector3(m[12], m[13], m[14]) / 100.0)


static func _lila_instances(node: Node, found: Array[Node]) -> Array[Node]:
	var source := node.scene_file_path
	if source.begins_with("res://lila/") or source.get_extension() in ["lila", "lilaa"]:
		found.append(node)
	for child: Node in node.get_children():
		_lila_instances(child, found)
	return found
