"""Send to Unity: exports each selected object to <Unity project>/Assets/Lila/<name>.fbx, which Unity imports itself."""

import os

import lila
from lila import extension, prefs
from lila.ops import exportFbx, ls

PROJECT_PREF = "/unityLink/project"


def unity_connect(project=""):
    if not os.path.isdir(os.path.join(project, "Assets")) or not os.path.isdir(os.path.join(project, "ProjectSettings")):
        raise lila.CommandError(f"{project} isn't a Unity project: it needs Assets and ProjectSettings folders")
    prefs.set("general", PROJECT_PREF, project)
    return project


def unity_send():
    project = prefs.get("general", PROJECT_PREF, "")
    if not project:
        raise lila.CommandError("choose the Unity project first: File > Connect to Unity")
    objects = ls(selection=True)
    if not objects:
        raise lila.CommandError("select the objects to send")
    folder = os.path.join(project, "Assets", "Lila")
    os.makedirs(folder, exist_ok=True)
    sent = []
    for obj in objects:
        path = os.path.join(folder, obj.split("|")[-1] + ".fbx")
        # The unity profile is Y up facing +Z, which Unity imports with no rotation on the root.
        exportFbx(path, obj, profile="unity", units="m", textures=True, unrealCheck=False)
        sent.append(path)
    return sent


def _pick_project():
    from PySide6 import QtWidgets

    from lila import ui

    folder = QtWidgets.QFileDialog.getExistingDirectory(ui.main_window(), "Unity project", prefs.get("general", PROJECT_PREF, ""))
    if folder:
        unity_connect(project=folder)


def initialize_extension():
    extension.register_command("unityConnect", unity_connect, "Choose the Unity project that Send to Unity writes into.",
                               flags=[("project", str, "", "Folder holding Assets and ProjectSettings.")])
    extension.register_command("unitySend", unity_send, "Export each selected object to <project>/Assets/Lila/<name>.fbx.", undoable=False)

    from lila import ui

    try:
        ui.add_action("unity.send", lambda: unity_send(), "Send to Unity", category="Unity",
                      doc="Export the selected objects into the Unity project chosen with Connect to Unity.")
        ui.add_action("unity.connect", _pick_project, "Connect to Unity...", category="Unity", doc="Choose the Unity project to send to.")
        section = ui.add_menu_item("File", kind="section", label="Unity", after="File/unreal.connect")
        send = ui.add_menu_item("File", action="unity.send", after=section)
        ui.add_menu_item("File", action="unity.connect", after=send)
    except RuntimeError:
        pass  # lilabatch has no menus
