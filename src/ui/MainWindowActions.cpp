#include "ui/MainWindowDetail.h"
#include "core/Paths.h"
#include "ui/ViewportHeaderConfig.h"
#include "ui/CameraBookmarks.h"
#include "ui/DroneCamera.h"
#include "ui/CreditsDialog.h"
#include "ui/LicensesDialog.h"
#include "ui/AudioUi.h"
#include "ui/MediaLane.h"
#include "ui/RenderOutputDialog.h"
#include "ui/PerfTrace.h"
#include "ui/ReferenceOverlay.h"
#include "ui/ReferenceSet.h"
#include "ui/ReferenceWindow.h"
#include "ui/IssueReport.h"
#include "ui/SupportBundle.h"
#include "ui/Updater.h"
#include "ui/ViewportToast.h"
#include "ui/PanelFilter.h"

namespace lila::ui
{
using namespace mainWindowDetail;

void MainWindow::registerActions()
{
    viewportHeader::registerActions();
    actions::add({"file.new", "New Scene", "File", [this] {
                      if (!confirmDiscard())
                          return;
                      actions::run(json{{"command", "newScene"}});
                      if (io::sceneTemplate::apply())
                          mViewports->frameUnlessRestored();
                  }, nullptr, "Starts a new scene, from the New Scene template when one is set, asking to save changes first."});
    actions::add({.id = "file.saveSceneTemplate",
                  .label = "Save as New Scene Template",
                  .category = "File",
                  .run = [] {
                      const std::filesystem::path p = io::sceneTemplate::saveCurrent();
                      log::info("Saved the scene as the New Scene template: {}", paths::toUtf8(p));
                  },
                  .doc = "New Scene and launch start from a copy of this scene: its objects, cameras, lights and settings.",
                  .python = "saveSceneTemplate()"});
    actions::add({.id = "file.clearSceneTemplate",
                  .label = "Clear New Scene Template",
                  .category = "File",
                  .run = [] {
                      io::sceneTemplate::clear();
                      const std::filesystem::path p = io::sceneTemplate::path();
                      if (p.empty())
                          log::info("New scenes start empty");
                      else
                          log::info("New scenes start from the studio or workspace template: {}", paths::toUtf8(p));
                  },
                  .enabled = [] { return !io::sceneTemplate::path().empty(); },
                  .doc = "Removes your New Scene template, so new scenes start empty, or from the studio's or workspace's template.",
                  .python = "clearSceneTemplate()"});
    actions::add({"file.open", "Open Scene...", "File", [this] { openScene({}); }, nullptr, "Opens a Lila or Maya scene, replacing the current one. An FBX, OBJ or other importable file opens as a new untitled scene with it imported."});
    actions::add({"file.reload", "Reload Scene", "File", [this] { reloadScene(); }, [] { return !io::currentPath().empty(); },
                  "Reopens the scene from its file on disk, discarding unsaved changes after asking."});
    actions::add({"file.save", "Save Scene", "File", [this] { saveScene(false); }, nullptr, "Saves the scene to its file, asking for a name the first time."});
    actions::add({"file.saveAs", "Save Scene As...", "File", [this] { saveScene(true); }, nullptr, "Saves the scene under a new name and keeps working in that file."});
    actions::add({"file.incrementAndSave", "Increment and Save", "File", [this] { incrementAndSave(); }, nullptr,
                  "Save as the next free scene_v###, then keep working in it", "incrementAndSave()"});
    actions::add({"file.welcome", "Welcome...", "File", [this] { showStartupDialog(true); }, nullptr,
                  "The welcome dialog: example scenes and recent scenes."});
    actions::add({"file.versions", "Versions...", "File", [this] { showVersions(); }, nullptr,
                  "Backups, autosaves, emergency saves, increments and source control revisions of this scene",
                  "sceneVersions()"});
    actions::add({"file.openRecoveryFolder", "Open Recovery Folder", "File",
                  [] {
                      std::error_code ec;
                      std::filesystem::create_directories(io::recovery::recoveredDir(), ec);
                      QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(io::recovery::recoveredDir().wstring())));
                  },
                  nullptr, "Shows the folder of scenes recovered after a crash in the file browser."});
    actions::add({"file.clearRecoveredScenes", "Clear Recovered Scenes", "File",
                  [this] {
                      if (QMessageBox::question(this, "Clear Recovered Scenes", "Delete every recovered scene? This can't be undone.") == QMessageBox::Yes)
                          ops::execute("clearRecoveredScenes", json::object(), CallSource::UI);
                  },
                  nullptr, "Deletes the scenes recovered after crashes, listed under File > Recovered Scenes.", "clearRecoveredScenes()"});
    actions::add({"file.openFolder", "Open Folder...", "File", [this] { openFolder(); }, nullptr,
                  "Opens a project folder: file browsing, source control and workspace layouts work from it."});
    actions::add({"file.closeFolder", "Close Folder", "File", [] { Workspace::get().close(); }, nullptr, "Closes the project folder; the scene stays open."});
    actions::add({"file.exit", "Exit", "File", [this] { close(); }, nullptr, "Quits Lila, asking to save changes first."});
    actions::add({.id = "viewport.escape",
                  .label = "Escape",
                  .category = "View",
                  .run =
                      [this] {
                          if (scene::anim::playDirection() != 0)
                              actions::run("anim.stop");
                          else
                              mViewports->active()->escape();
                      },
                  .doc = "Stops playback, else cancels what the view is doing, else drops the object selection."});
    actions::add({"viewport.frameSelected", "Frame Selected", "View", [this] { mViewports->active()->frameSelection(false); }, nullptr,
                  "Moves the active view's camera to fit the selection."});
    actions::add({"viewport.frameAll", "Frame All", "View", [this] { mViewports->active()->frameSelection(true); }, nullptr,
                  "Moves the active view's camera to fit everything in the scene."});
    actions::add({"viewport.frameUnderCursor", "Frame Under Cursor", "View",
                  [this] {
                      if (frameUvUnderCursor())
                          return;
                      ViewportWidget* view = mViewports->active();
                      for (ViewportWidget* v : mViewports->views())
                          if (v->isVisible() && v->rect().contains(v->mapFromGlobal(QCursor::pos())))
                              view = v;
                      view->frameUnderCursor();
                  },
                  nullptr,
                  "Centres the view on the surface under the cursor (in the UV Editor, the UV point under it), keeping its distance unless Preferences > Viewport sets it to zoom in too. From the menu, click the surface "
                  "afterwards."});
    actions::add({"viewport.cameraUndo", "Undo View Change", "View", [this] { mViewports->active()->cameraUndo(); }, nullptr,
                  "Puts the active view's camera back where it was before its last move. Separate from scene undo."});
    actions::add({"viewport.cameraRedo", "Redo View Change", "View", [this] { mViewports->active()->cameraRedo(); }, nullptr,
                  "Redoes the camera move that Undo View Change took back."});
    actions::add({"file.import", "Import...", "File", [this] { importFile(); }, nullptr, "Adds the contents of an FBX, OBJ or Maya file to the scene.",
                  "importFbx(path)  # or importObj(path), mayaImport(path)"});
    actions::add({"file.convertMaya", "Convert Maya Files...", "File", [this] { convertMayaFolder(); }, nullptr,
                  "Converts every .ma and .mb file in a folder to a .lila scene.", "mayaConvert(source=folder)  # every .ma / .mb in it to .lila"});
    actions::add({"file.exportSelection", "Export Selection...", "File", [this] { exportSelection(); }, nullptr,
                  "Writes the selected objects to an FBX or OBJ file, with their modifiers applied.",
                  "exportFbx(path, profile=\"ue5\")  # or exportObj(path)"});
    actions::add({"file.exportWebViewer", "Export Web Viewer...", "File", [this] { exportWebViewer(); }, nullptr,
                  "Writes the selection (or the whole scene) to a folder with model.glb and an index.html that shows it "
                  "interactively, lit by the viewport's HDRI, for uploading to a portfolio site.",
                  "exportWebViewer(folder, title=\"Ship\", author=\"Name\")"});
    actions::add({"file.createReference", "Create Reference...", "File", [this] { ReferenceEditor::createReferenceDialog(this); }, nullptr,
                  "References a Lila scene or a Maya .ma / .mb file into this one under a namespace; Maya files are converted to a .lila beside them.",
                  "createReference(path)  # or mayaReference(path) for .ma / .mb"});
    actions::add({"unreal.send", "Send to Unreal", "Unreal", [this] { sendToUnreal(); }, nullptr,
                  "Maps the selection to Unreal assets and exports it to the connected editor.",
                  "unrealMap(node, contentPath=\"/Game/Meshes/SM_Name\"); unrealSync()"});
    actions::add({"unreal.sync", "Sync to Unreal", "Unreal", [this] { run("unrealSync"); }, nullptr,
                  "Exports and reimports every object already mapped to an Unreal asset, as saving does.", "unrealSync()"});
    actions::add({"unreal.expressionRig", "Send Expression Rig to Unreal", "Unreal",
                  [this] {
                      json args = json::object();
                      if (const auto sel = scene::Graph::current().selection(); !sel.empty())
                          args["args"] = {scene::Graph::current().node(sel.front())->path()};
                      run("unrealExpressionRig", args);
                  },
                  nullptr,
                  "Builds a Control Rig that runs the selected character's expressions in Unreal, exactly as in Lila, and plays it from "
                  "the mesh's post-process Anim Blueprint.",
                  "unrealExpressionRig(\"Char\")"});
    actions::add({"unreal.connect", "Connect to Unreal", "Unreal", [this] { run("unrealConnect"); }, nullptr,
                  "Connects to an Unreal Editor running the LilaLink plugin.", "unrealConnect()"});
    for (const char* view : {"persp", "top", "front", "right", "left", "back", "bottom"})
    {
        std::string label = std::string(1, char(std::toupper(view[0]))) + (view + 1) + " View";
        std::string doc = std::string(view) == "persp" ? "Switches the active view to the perspective camera."
                                                       : std::string("Switches the active view to the orthographic ") + view + " camera.";
        actions::add({std::string("viewport.view.") + view, label, "View", [this, view] { mViewports->active()->setView(view); }, nullptr, doc,
                      std::string("lila.ui.run_action(\"viewport.view.") + view + "\")"});
    }
    for (auto [view, label] : {std::pair{"persp", "Switch to Persp"}, {"front", "Switch to Front"}, {"right", "Switch to Side"}, {"top", "Switch to Top"}})
        actions::add({std::string("viewport.switch.") + (std::string(view) == "right" ? "side" : view), label, "View",
                      [this, view] { mViewports->active()->switchView(view); }, nullptr,
                      std::string(view) == "persp" ? "Switches the active view to the perspective camera."
                                                   : "Switches the active view to that side; again for the opposite side, as the View Switcher does.",
                      std::string("lila.ui.run_action(\"viewport.switch.") + (std::string(view) == "right" ? "side" : view) + "\")"});
    auto selectedCamera = []() -> scene::Node* {
        scene::Graph& g = scene::Graph::current();
        for (scene::NodeId id : g.selection())
            if (scene::Node* n = g.node(id); n && n->type().isA("camera"))
                return n;
        return nullptr;
    };
    actions::add({"viewport.lookThroughSelected", "Look Through Selected", "View",
                  [this, selectedCamera] {
                      if (scene::Node* n = selectedCamera())
                          run("lookThru", {{"args", {n->path()}}});
                  },
                  [selectedCamera] { return selectedCamera() != nullptr; }, "The active view looks through the selected camera; navigating it then moves the camera.",
                  "lila.ops.lookThru(\"camera1\")"});
    actions::add({"create.cameraFromView", "Camera from View", "Create", [this] { createCameraFromView(); }, nullptr,
                  "Creates a camera where the active view's camera is, with its lens, and selects it."});
    viewportRenderers::setActivePanel([this] { return mViewports->active()->hudPanel(); });
    ops::registerCommand({
        .name = "lookThru",
        .doc = "Make the active view look through a camera node, or switch it to a view (persp, top, front, side, ...). "
               "Returns the name of what the active view looks through; with no argument it only returns it.",
        .positionalDoc = "camera",
        .undoable = false,
        .run = [this](const Args& a) -> json {
            ViewportWidget* v = mViewports->active();
            if (a.positional.empty())
                return v->viewName();
            if (!a.positional[0].is_string())
                throw CommandError("lookThru: camera name expected");
            std::string name = a.positional[0].get<std::string>();
            if (name == "side")
                name = "right";
            v->setView(name);
            if (v->viewName() != name)
                throw CommandError("lookThru: '" + name + "' is not a camera or a view name");
            return v->viewName();
        },
    });
    actions::add({"edit.rename", "Rename...", "Edit", [this] { renameSelection(); }, actions::hasSelection,
                  "Renames the selected objects at the cursor; several get numbered names."});
    actions::add({"window.resetLayout", "Reset Layout", "Windows", [this] {
                      std::string current = layouts::current();
                      if (current.empty() || !layouts::find(current))
                      {
                          resetLayout();
                          return;
                      }
                      try
                      {
                          ops::execute("applyLayout", {{"name", current}, {"reset", true}}, CallSource::UI);
                          fitToScreen();
                      }
                      catch (const CommandError& e)
                      {
                          log::error("{}", e.what());
                      }
                  }, nullptr, "Puts every panel back where the current layout has it, keeping that layout picked. With none picked, "
                              "back to the default arrangement."});
    actions::add({"window.toggleChannelBox", "Toggle Channel Box", "Windows", [this] { toggleChannelBox(); }, nullptr,
                  "Brings the channel box to the front of its tabs; again to go back to the tab shown before it, or to the attribute "
                  "editor when Preferences > Interface says so."});
    actions::add({"window.toggleUvEditor", "Toggle UV Editor", "Windows", [this] { toggleUvEditor(); }, nullptr,
                  "Shows the UV editor, or tucks it back into its sidebar when it is showing."});
    actions::add({"window.expandPanels", "Expand Collapsed Panels", "Windows", [this] { mSidebars->expandAll(); }, nullptr,
                  "Opens every panel collapsed to a sidebar tab."});
    actions::add({"window.searchMenus", "Search Menus...", "Windows", [this] { menuSearch::openBox(menuBar(), this); }, nullptr,
                  "Finds any item in the main menus by typing part of its name or menu, and runs it or opens its options."});
    commandPalette::registerActions(this);
    presenter::install(this);
    actions::add({"window.searchScene", "Search Scene...", "Windows",
                  [this] {
                      // In a panel the same key finds in that panel.
                      if (QDockWidget* dock = panelFilter::dockOf(QApplication::focusWidget()); dock && dock->widget())
                          panelFilter::open(dock);
                      else
                          menuSearch::openSceneFinder(menuBar(), this);
                  },
                  nullptr,
                  "Finds any object in the scene by name, wildcard, type: or path, selects it and reveals it in the outliner. "
                  "With a panel focused, finds in that panel instead."});
    actions::add({"window.searchTearOffs", "Search Tear-offs...", "Windows", [this] { menuSearch::openTearOffSearch(menuBar(), this); }, nullptr,
                  "Finds any menu or menu section by name and tears it off under the cursor as a palette; the next click drops it."});
    auto scriptEditor = [this]() -> ScriptEditor* {
        QDockWidget* d = findChild<QDockWidget*>(kScriptEditorDock);
        return d ? qobject_cast<ScriptEditor*>(d->widget()) : nullptr;
    };
    actions::add({"script.run", "Run Script", "Script Editor", [scriptEditor] {
                      if (ScriptEditor* e = scriptEditor())
                          e->runCurrent(false);
                  }, nullptr, "Runs the script editor's selection, or its whole tab."});
    actions::add({"script.runLine", "Run Line", "Script Editor", [scriptEditor] {
                      if (ScriptEditor* e = scriptEditor())
                          e->runLine();
                  }, nullptr, "Runs the script editor's line under the caret, or the selection when there is one."});
    actions::add({"script.runCell", "Run Cell", "Script Editor", [scriptEditor] {
                      if (ScriptEditor* e = scriptEditor())
                          e->runCurrent(true);
                  }, nullptr, "Runs the script editor's cell at the caret: the lines between two # %% marker comments."});
    actions::add({"script.searchApi", "Search Scripting API...", "Script Editor", [this] {
                      QDockWidget* d = findChild<QDockWidget*>(kScriptEditorDock);
                      QPointer<ScriptEditor> editor = d ? qobject_cast<ScriptEditor*>(d->widget()) : nullptr;
                      if (!editor)
                          return;
                      menuSearch::openApiSearch(menuBar(), this,
                                                {.insert = [editor](const QString& code) {
                                                     if (!editor)
                                                         return;
                                                     CodeEditor* ed = editor->currentEditor();
                                                     ed = ed ? ed : editor->newTab();
                                                     ed->insertCall(code);
                                                 },
                                                 .showDocs = [editor](const QString& key) {
                                                     if (editor)
                                                         Q_EMIT editor->showDocs(key);
                                                 }});
                  }, nullptr,
                  "Finds a command, flag or lila Python class or function by name or description and inserts the call into the script editor."});
    actions::add({"window.preferences", "Preferences...", "Windows", [this] {
                      if (!closeShownWindow<PreferencesDialog>())
                          PreferencesDialog::open(this);
                  }, nullptr,
                  "Settings for the viewport, manipulators, files, Python and the rest of Lila."});
    actions::add({"window.extensionManager", "Extension Manager...", "Windows", [this] {
                      if (!closeShownWindow<ExtensionManagerDialog>())
                          ExtensionManagerDialog::open(this);
                  }, nullptr,
                  "Load, unload and auto load extensions, and see their versions and load errors."});
    actions::add({"viewport.isolate", "Isolate Select", "View", [this] { mViewports->active()->toggleIsolate(); },
                  [this] { return mViewports->active()->isolating() || actions::hasSelection(); },
                  "Shows only the selection in the active view: whole objects, or just the faces of selected components. Again to show everything."});
    actions::add({"viewport.isolateAdd", "Add Selected to Isolation", "View", [this] { mViewports->active()->addToIsolation(); },
                  [] { return actions::hasSelection(); },
                  "Adds the selected objects or components to what the active view isolates, starting isolation if it is off."});
    actions::add({"viewport.isolateRemove", "Remove Selected from Isolation", "View", [this] { mViewports->active()->removeFromIsolation(); },
                  [this] { return mViewports->active()->isolating() && actions::hasSelection(); },
                  "Hides the selected objects or components in the active view's isolation."});
    imagePlanes::install([this] { return mViewports->active(); });
    audio::install();
    mediaLane::install();
    references::install();
    references::windows::install();
    referenceOverlays::install([this] { return mViewports->active(); });
    sets::registerActions(this);
    GameExporter::registerActions(this);
    MeshSequenceExporter::registerActions(this);
    animatedSend::registerActions(this);
    hud::registerActions([this] { return mViewports->active(); });
    displayAids::registerActions([this] { return mViewports->active(); });
    cameraBookmarks::install([this] { return mViewports->active(); });
    drone::registerActions([this] { return mViewports->active(); });
    viewportToast::setActive([this] { return mViewports->active(); });
    lodDisplay::registerActions([this] { return mViewports->active(); });
    toolBox::registerItems([this] { return mViewports->active(); });
    cursorBadges::registerDefaults();
    actions::add({"viewport.toggleQuad", "Toggle Four Views", "View", [this] { mViewports->toggleQuad(); }, nullptr,
                  "Switches between one view and the top, front, side and perspective views together."});
    auto toggleGate = [this](const char* flag, const char* key) {
        std::string cam = mViewports->active()->viewName();
        bool on = scene::renderSettings().camera(cam).toJson()[key].get<bool>();
        try
        {
            ops::execute("cameraSettings", {{"args", json::array({cam})}, {flag, !on}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    };
    actions::add({"viewport.toggleResolutionGate", "Resolution Gate", "View", [toggleGate] { toggleGate("gate", "resolutionGate"); },
                  nullptr, "Shows the render resolution's frame in the active view, darkening outside it.",
                  "lila.ops.cameraSettings(\"persp\", gate=True)"});
    actions::add({"viewport.toggleFilmGate", "Film Gate", "View", [toggleGate] { toggleGate("filmGate", "filmGate"); }, nullptr,
                  "Shows the camera's film back in the active view.", "lila.ops.cameraSettings(\"persp\", filmGate=True)"});
    actions::add({"viewport.cameraSettings", "Camera Settings...", "View",
                  [this] { RenderSettingsDialog::open(this, mViewports->active()->viewName()); }, nullptr,
                  "The active view's camera gates, film fit, overscan and gate mask."});
    actions::add({"window.renderOutput", "Render Output...", "Render", [this] {
                      if (!closeShownWindow<RenderOutputDialog>())
                          RenderOutputDialog::open(this);
                  }, nullptr,
                  "Camera, samples, motion blur, frame range, images and video for renders.",
                  "lila.ops.renderSequence(video=\"shot.mp4\", samples=64)"});
    actions::add({"render.frame", "Render Frame", "Render", [] { RenderOutputDialog::renderFrame(); }, nullptr,
                  "Renders the current frame with the Render Output settings: post process, depth of field and motion blur included.",
                  "lila.ops.render(path=\"frame.png\", samples=64)"});
    actions::add({"render.sequence", "Render Sequence", "Render", [] { RenderOutputDialog::renderSequence(); }, nullptr,
                  "Renders the frame range into images and video with the Render Output settings.",
                  "lila.ops.renderSequence(path=\"shot.####.png\")"});
    actions::add({"render.turntable", "Render Turntable", "Render", [] {
                      try
                      {
                          nlohmann::json r = ops::execute("renderTurntable", nlohmann::json::object(), CallSource::UI);
                          if (r.contains("path") && r["path"].is_array() && !r["path"].empty())
                              log::info("Rendered a turntable to {}", r["path"].back().get<std::string>());
                      }
                      catch (const CommandError& e)
                      {
                          log::error("{}", e.what());
                      }
                  }, nullptr,
                  "Renders a looping turn around the selection (or everything shown) with the Render Output settings.",
                  "lila.ops.renderTurntable(video=\"turntable.mp4\", frames=120)"});
    for (const auto& [preset, label] : {std::pair{"threePoint", "Three-Point Light Rig"}, std::pair{"rim", "Rim Light Rig"}, std::pair{"product", "Product Light Rig"}})
    {
        const std::string p = preset;
        actions::add({std::string("render.lightRig.") + preset, label, "Render", [p] {
                          try
                          {
                              ops::execute("lightRig", {{"preset", p}}, CallSource::UI);
                          }
                          catch (const CommandError& e)
                          {
                              log::error("{}", e.what());
                          }
                      }, nullptr,
                      "Area lights around the selection (or everything shown), aimed and sized to fit it, under one group.",
                      "lila.ops.lightRig(preset=\"" + p + "\")"});
    }
    actions::add({"render.postProcess", "Post Process", "Render", [] {
                      try
                      {
                          const std::string name = ops::execute("postProcess", {{"existing", true}}, CallSource::UI).get<std::string>();
                          ops::execute("select", {{"args", {name}}}, CallSource::UI);
                      }
                      catch (const CommandError& e)
                      {
                          log::error("{}", e.what());
                      }
                  }, nullptr,
                  "Selects the scene's post process (creating one if it has none): tonemapper, exposure, grading, bloom and lens effects.",
                  "lila.ops.postProcess(existing=True)"});
    actions::add({"window.renderSettings", "Render Settings...", "Windows", [this] {
                      if (!closeShownWindow<RenderSettingsDialog>())
                          RenderSettingsDialog::open(this);
                  }, nullptr,
                  "Render resolution and per-camera settings.", "lila.ops.setRenderResolution(width=1920, height=1080)"});
    actions::add({"window.toggleMaximizePanel", "Maximize Viewport", "Windows", [this] {
                      if (!mMaximizeHidden.empty())
                      {
                          for (auto& [d, showing] : mMaximizeHidden)
                              if (d)
                                  d->show();
                          for (auto& [d, showing] : mMaximizeHidden)
                              if (d && showing)
                                  d->raise();
                          mMaximizeHidden.clear();
                          scheduleLayoutSave();
                          return;
                      }
                      for (QDockWidget* d : findChildren<QDockWidget*>())
                          if (d->parentWidget() == this && !d->isHidden() && !d->isFloating())
                              mMaximizeHidden.emplace_back(d, d->isVisible());
                      for (auto& [d, showing] : mMaximizeHidden)
                          d->hide();
                      scheduleLayoutSave();
                  }, nullptr, "Hides every docked panel so the viewports fill the window; again to bring them back."});
    actions::add({"help.about", "About Lila", "Help", [this] { showAbout(); }, nullptr, "Version and copyright."});
    actions::add({"help.licenses", "Licenses", "Help", [this] { showLicenses(this); }, nullptr,
                  "The third-party components in Lila and their licenses."});
    actions::add({"help.credits", "Credits", "Help", [this] { showCredits(this); }, nullptr,
                  "The sponsors, supporters, contributors and testers behind Lila."});
    actions::add({"help.prefsFolder", "Open Prefs Folder", "Help", [] {
                      QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(PrefStore::get().userDir().wstring())));
                  }, nullptr, "Opens the folder holding your preferences, hotkeys, shelves and layouts."});
    actions::add({"help.crashReports", "Open Crash Reports Folder", "Help", [] { openCrashFolder(crash::reportsDir()); }, nullptr,
                  "One folder per crash: report.txt (what crashed, stacks, log, system) and a minidump."});
    actions::add({"help.copyLastCrash", "Copy Last Crash Report", "Help", [this] {
                      std::vector<std::filesystem::path> all = crash::reports();
                      if (all.empty())
                      {
                          QMessageBox::information(this, "Copy Last Crash Report", "Lila has no crash reports.");
                          return;
                      }
                      copyCrashReport(all.front());
                      statusBar()->showMessage("Crash report copied: " + QString::fromStdWString(all.front().wstring()), 5000);
                  }, nullptr, "Puts the newest crash report on the clipboard, ready to paste into a bug report."});
    actions::add({"help.checkUpdates", "Check for Updates...", "Help", [this] { updater::checkNow(this); }, nullptr,
                  "Looks for a newer Lila and offers to install it when Lila closes."});
    actions::add({.id = "help.reportIssue", .label = "Report a Bug...", .category = "Help", .run = [this] { issueReport::showBug(this); },
                  .doc = "Sends a bug, crash or performance report to the Lila issue tracker, with your support bundle attached privately. No GitHub account needed.",
                  .keywords = {"issue", "feedback", "crash", "problem", "support"}});
    actions::add({.id = "help.requestFeature", .label = "Request a Feature...", .category = "Help", .run = [this] { issueReport::showRequest(this); },
                  .doc = "Asks for a new tool or a change on the Lila issue tracker. No GitHub account needed.",
                  .keywords = {"feedback", "suggest", "idea", "wish", "roadmap"}});
    actions::add({"help.myReports", "My Reports...", "Help", [this] { issueReport::showMyReports(this); }, nullptr,
                  "Reports sent from this machine, their status and replies."});
    actions::add({"help.supportBundle", "Save Support Bundle...", "Help", [this] { support::showDialog(this); }, nullptr,
                  "Saves a zip with your system, this session's log, recent crash reports and prefs, to attach to a bug report."});
    perfTrace::registerAll(this);
    actions::add({"help.telemetry", "Telemetry and Privacy...", "Help", [this] { showTelemetrySettings(this); }, nullptr,
                  "Choose whether Lila sends anonymous usage statistics and crash reports, and see what's sent."});
}

} // namespace lila::ui
