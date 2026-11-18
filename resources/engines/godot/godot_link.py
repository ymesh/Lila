"""Send to Godot: exports each selected object to <Godot project>/lila/<name>.glb, which Godot imports itself."""

import os

import lila
from lila import extension, prefs
from lila.ops import exportGltf, ls

PROJECT_PREF = "/godotLink/project"


def godot_connect(project=""):
    if not os.path.isfile(os.path.join(project, "project.godot")):
        raise lila.CommandError(f"{project} has no project.godot")
    prefs.set("general", PROJECT_PREF, project)
    return project


def godot_send():
    project = prefs.get("general", PROJECT_PREF, "")
    if not project:
        raise lila.CommandError("choose the Godot project first: File > Connect to Godot")
    objects = ls(selection=True)
    if not objects:
        raise lila.CommandError("select the objects to send")
    folder = os.path.join(project, "lila")
    os.makedirs(folder, exist_ok=True)
    sent = []
    for obj in objects:
        path = os.path.join(folder, obj.split("|")[-1] + ".glb")
        exportGltf(path, obj, unrealCheck=False)
        sent.append(path)
    return sent


def _pick_project():
    from PySide6 import QtWidgets

    from lila import ui

    folder = QtWidgets.QFileDialog.getExistingDirectory(ui.main_window(), "Godot project", prefs.get("general", PROJECT_PREF, ""))
    if folder:
        godot_connect(project=folder)


def initialize_extension():
    extension.register_command("godotConnect", godot_connect, "Choose the Godot project that Send to Godot writes into.",
                               flags=[("project", str, "", "Folder holding project.godot.")])
    extension.register_command("godotSend", godot_send, "Export each selected object to <project>/lila/<name>.glb.", undoable=False)

    from lila import ui

    try:
        ui.add_action("godot.send", lambda: godot_send(), "Send to Godot", category="Godot",
                      doc="Export the selected objects into the Godot project chosen with Connect to Godot.")
        ui.add_action("godot.connect", _pick_project, "Connect to Godot...", category="Godot", doc="Choose the Godot project to send to.")
        section = ui.add_menu_item("File", kind="section", label="Godot", after="File/unreal.connect")
        send = ui.add_menu_item("File", action="godot.send", after=section)
        ui.add_menu_item("File", action="godot.connect", after=send)
    except RuntimeError:
        pass  # lilabatch has no menus
