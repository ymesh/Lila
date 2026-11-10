#include "ui/MainWindowDetail.h"
#include "ui/MenuContext.h"
#include "ui/TopBar.h"
#include "ui/Credits.h"
#include "ui/CreditsDialog.h"
#include "ui/DocsUi.h"
#include "ui/FileDialog.h"
#include "ui/HeightfieldDisplay.h"
#include "ui/LicensesDialog.h"
#include "ui/MenuSets.h"
#include "ui/Shelf.h"
#include "ui/TearOffLogic.h"

#include <QToolBar>

namespace lila::ui
{
using namespace mainWindowDetail;

QMenu* MainWindow::refreshKeysOnShow(QMenu* menu)
{
    menu->setToolTipsVisible(true);
    // Hotkeys are live-editable, so the key column is refreshed each time the menu opens.
    connect(menu, &QMenu::aboutToShow, menu, [menu] {
        for (QAction* qa : menu->actions())
        {
            std::string id = qa->data().toString().toStdString();
            if (id.empty())
                continue;
            QString key = Hotkeys::get().keyFor(id);
            qa->setText(QString::fromStdString(actions::displayLabel(id)) + (key.isEmpty() ? "" : "	" + key));
            qa->setToolTip(QString::fromStdString(actions::tooltip(id)));
            qa->setEnabled(actions::isEnabled(json(id)));
        }
    });
    optionBox::decorateMenu(menu);
    return menu;
}

void MainWindow::buildMenus()
{
    using namespace menus;
    auto menu = [](const char* id, const char* title, Area area = Area::MenuBar, std::function<void(QMenu*)> setup = {}, const char* docs = "",
                   std::vector<std::string> sets = {}) {
        addMenu({.id = id, .title = title, .area = area, .setup = std::move(setup), .docs = docs, .sets = std::move(sets)});
    };
    // Menu sets (see MenuContext.h): menus without one are always on the menu bar.
    const std::vector<std::string> rigAnim{"Rig", "Animate"};

    menu("File", "File");
    menus::addActions("File", {"#Scene", "file.new", "file.open", "file.reload"});
    addSubmenu("File", "Recent Scenes", [this](QMenu* recent) {
        recent->setProperty(menuSearch::kNoSearchProperty, true);
        connect(recent, &QMenu::aboutToShow, this, [this, recent] {
            recent->clear();
            json files = PrefStore::get().value("general", "/recentFiles", json::array());
            for (const json& r : files.is_array() ? files : json::array())
                if (r.is_string())
                {
                    std::string p = r.get<std::string>();
                    recent->addAction(QString::fromStdString(p), [this, p] { openScene(std::filesystem::path(p)); });
                }
            if (recent->isEmpty())
                recent->addAction("(none)")->setEnabled(false);
        });
    });
    addSubmenu("File", "Recovered Scenes", [this](QMenu* menu) {
        menu->setProperty(menuSearch::kNoSearchProperty, true);
        menu->setToolTipsVisible(true);
        connect(menu, &QMenu::aboutToShow, this, [this, menu] {
            menu->clear();
            for (const io::recovery::Recovered& r : io::recovery::recovered())
            {
                QString text = r.scene.empty() ? QString("untitled") : QString::fromStdWString(r.scene.filename().wstring());
                text += "    " + QDateTime::fromSecsSinceEpoch(r.time).toString("yyyy-MM-dd hh:mm");
                if (r.kind == io::history::Kind::Emergency)
                    text += "    emergency save";
                if (!r.scene.empty())
                    text += "    " + QString::fromStdWString(r.scene.generic_wstring());
                QAction* a = menu->addAction(text, [this, file = r.file] { openRecoveredScene(file); });
                a->setToolTip("Opens as a new untitled scene. Recovered from " + QString::fromStdWString(r.file.wstring()));
            }
            if (menu->isEmpty())
                menu->addAction("(none)")->setEnabled(false);
            menu->addSeparator();
            for (const char* id : {"file.openRecoveryFolder", "file.clearRecoveredScenes"})
                menu->addAction(QString::fromStdString(actions::displayLabel(id)), [id] { actions::run(id); })
                    ->setToolTip(QString::fromStdString(actions::tooltip(id)));
        });
    });
    menus::addActions("File", {"file.save", "file.saveAs", "file.incrementAndSave", "file.versions", "file.saveSceneTemplate", "#Validate", "validate.run", "window.validation", "#Import and Export", "file.import", "file.convertMaya",
                        "file.exportSelection", "file.gameExporter", "file.exportMeshSequence", "file.exportWebViewer", "window.batchRun", "file.createReference", "#Unreal", "unreal.send", "animated.sendOptions", "unreal.sync", "unreal.sendBake", "groom.sendToUnreal",
                        "deform.exportMLDeformer", "unreal.expressionRig", "unreal.connect","#Workspace", "file.openFolder"});
    addSubmenu("File", "Recent Folders", [this](QMenu* recent) {
        recent->setProperty(menuSearch::kNoSearchProperty, true);
        mRecentMenu = recent;
        refreshRecentFolders();
    });
    menus::addActions("File", {"file.closeFolder", "", "file.welcome", "", "file.exit"});

    menu("Edit", "Edit", Area::MenuBar, {}, "duplicate");
    menus::addActions("Edit", {"#History", "edit.undo", "edit.redo", "edit.repeatLast", "tool.repeatLast", "adjustLastOp.show", "edit.flushUndo", "edit.flushRedo", "edit.flushAll", "#Selection",
                        "edit.selectAll", "edit.deselect", "edit.invertSelection", "edit.delete"});
    addSubmenu("Edit", "Quick Select Sets", [](QMenu* m) { sets::attachQuickSelectMenu(m); });
    menus::addActions("Edit", {"#Hierarchy", "edit.duplicate", "edit.duplicateWithTransform", "edit.instance", "edit.instanceReference", "edit.array", "edit.bakeArray",
                        "edit.makeUnique", "edit.convertToInstance", "select.instances", "select.instanceSource", "edit.group", "edit.lodGroup", "edit.generateLods", "edit.parent", "edit.parentChain", "edit.unparent", "#Naming", "edit.rename", "edit.batchRename", "edit.conformNames", "#Settings",
                        "window.preferences", "window.hotkeyEditor", "window.extensionManager"});

    menu("Modify", "Modify");
    menus::addActions("Modify", {"#Transforms", "modify.freezeTransforms", "modify.findUnfrozenTransforms", "modify.resetTransforms", "modify.centerPivot", "modify.resetPivot", "modify.zeroPivot", "modify.zeroPivotRotation", "modify.bakePivot", "modify.matchPivot", "modify.matchPivotPosition", "modify.matchPivotOrient", "modify.orientPivot", "modify.pinComponentPivot", "modify.resetComponentPivot",
                                 "#Snapping", "snapSurface.set", "snapSurface.clear", "snapSurface.toggle", "snapSurface.alignToNormal"});
    addSubmenu("Modify", "Scene Origin");
    menus::addActions("Modify/Scene Origin", {"modify.relocateScene", "modify.setDisplayOrigin", "modify.clearDisplayOrigin"});
    addSubmenu("Modify", "Turntable");
    menus::addActions("Modify/Turntable", {"turntable.putOn", "turntable.takeOff", "", "turntable.spin", "turntable.stepBack", "turntable.stepNext",
                                           "turntable.front", "", "turntable.controls", "turntable.ring", "turntable.pitch", "create.turntableBase"});

    menu("Create", "Create");
    addSubmenu("Create", "Polygon Primitives");
    menus::addActions("Create/Polygon Primitives", {"create.cube", "create.sphere", "create.cylinder", "create.capsule", "create.cone", "create.plane", "create.torus",
                                             "create.disc", "create.pipe", "create.quadSphere", "create.icoSphere"});
    menus::addActions("Create", {"create.locator", "create.camera", "create.cameraFromView", "create.emptyGroup", "create.turntableBase", "create.audio"});
    addSubmenu("Create", "Reference Images");
    menus::addActions("Create/Reference Images", {"create.imagePlane", "imagePlane.cut", "imagePlane.guides", "reference.addImages", "referenceOverlay.addImage", "", "referenceOverlay.adjust",
                                                   "referenceOverlay.toggle", "referenceOverlay.next", ""});
    menus::addItem({.menu = "Create/Reference Images", .action = docs::pageAction("reference-images")});
    addSubmenu("Create", "Sets");
    menus::addActions("Create/Sets", {"sets.create", "sets.createQuickSelect"});

    // Toggles show their state, read from the active view when they are per view.
    auto syncChecks = [this](QMenu* m) {
        connect(m, &QMenu::aboutToShow, m, [this, m] {
            for (QAction* qa : m->actions())
                if (std::optional<bool> on = viewportActionState(qa->data().toString().toStdString(), mViewports->active()))
                {
                    qa->setCheckable(true);
                    qa->setChecked(*on);
                }
        });
    };
    menu("Select", "Select", Area::MenuBar, syncChecks);
    menus::addActions("Select", {"#Mode", "select.objectMode", "select.vertexMode", "select.edgeMode", "select.faceMode", "#Components", "select.allComponents",
                          "select.grow", "select.loop", "select.ring", "select.symmetrize", "select.toggleSymmetric", "#Hierarchy", "select.hierarchy", "select.pickWalkUp", "select.pickWalkDown",
                          "select.pickWalkLeft", "select.pickWalkRight", "#Convert", "select.toVertices", "select.toEdges", "select.toFaces",
                          "select.containedFaces", "#Tools", "softSelect.toggle", "select.paintTool", "select.toggleLasso", "select.toggleCameraBased", "select.toggleBackfaceCulling",
                          "select.edgesByCenter", "select.facesByCenter"});

    menu("Mesh", "Mesh", Area::MenuBar, {}, "", {"Model", "Sculpt", "Level"});
    menus::addActions("Mesh", {"#Combine", "mesh.combine", "mesh.smartJoin", "mesh.separate", "mesh.splitMeshTool", "mesh.extract", "mesh.duplicateFaces", "mesh.boundingMesh", "mesh.multiBoxBounds",
                               "#Mirror", "mesh.mirror", "#Booleans", "mesh.booleanUnion", "mesh.booleanDifference", "mesh.booleanIntersection"});
    addSubmenu("Mesh", "Clay");
    menus::addActions("Mesh/Clay", {"clay.create", "clay.tool", "clay.addShape", "clay.addCut", "clay.group", "", "clay.refresh", "clay.convert"});
    addSubmenu("Mesh", "Plating");
    menus::addActions("Mesh/Plating", {"layout.pencil", "layout.plates", "layout.fasteners", "layout.patterns", "", "layout.loosen", "layout.deleteLines", "layout.clear"});
    addSubmenu("Mesh", "Symmetry");
    menus::addActions("Mesh/Symmetry", {"mesh.toggleSymmetry", "mesh.symmetryX", "mesh.symmetryY", "mesh.symmetryZ", "mesh.symmetryRadial", "mesh.symmetryOff", "viewport.toggleSymmetryLine", "",
                                 "mesh.symmetryCheck", "mesh.symmetrizePositive", "mesh.symmetrizeNegative", "mesh.symmetryRebuild", "mesh.symmetrizeRadial",
                                 "mesh.symmetryRepair", "mesh.symmetryRebuildTopological"});
    menus::addActions("Mesh", {"#Tessellation", "mesh.triangulate", "mesh.quadrangulate", "mesh.subdivide", "#Edge Weights",
                               "mesh.crease", "mesh.uncrease", "mesh.bevelWeight", "#Attributes",
                               "mesh.transferAttributes"});
    menus::addActions("Mesh", {"#Cleanup", "mesh.repair", "mesh.selectIssues", "#Nodes", "window.nodeEditor", "nodeEditor.showHistory"});

    menu("Topology", "Topology", Area::MenuBar, syncChecks, "topology", {"Model", "Sculpt"});
    menus::addActions("Topology", {"#Session", "retopo.setReference", "window.topologyToolkit", "#Draw", "retopo.draw", "topo.pencil", "topo.patch",
                                   "topo.strip", "topo.stroke", "topo.cylinder", "topo.cap", "#Shape", "topo.handles", "topo.relax", "topo.settle", "mesh.relax",
                                   "#Auto Retopo", "topo.region", "topo.runRegion", "mesh.quadRemesh", "retopo.poles", "mesh.voxelRemesh",
                                   "#Constructs", "topo.freezeAll", "#Display", "topo.qualityOverlay"});

    menu("Model", "Model", Area::MenuBar, {}, "", {"Model"});
    menus::addActions("Model", {"#Components", "mesh.extrude", "mesh.inset", "mesh.insetRegion", "mesh.bevel", "mesh.insertLoop", "mesh.splitRings", "mesh.offsetLoop", "mesh.knife", "mesh.bridge",
                                "mesh.fillHole", "mesh.gridFill", "mesh.poke", "#Merge", "mesh.merge", "mesh.mergeCenter", "mesh.targetWeld", "mesh.collapse", "#Edges", "mesh.dissolve",
                                "mesh.spin", "#Shape", "mesh.editEdgeFlow", "mesh.straighten", "mesh.circularize", "mesh.spherize", "mesh.flatten", "mesh.conformShape",
                                "mesh.conformToObject"});

    menu("Curves", "Curves", Area::MenuBar, {}, "curves", {"Model", "Level", "Groom"});
    menus::addActions("Curves", {"window.curveToolkit", "#Create", "curve.ezCurve", "curve.bezierPen", "curve.cvTool", "curve.epTool", "curve.pencil", "curve.catmullRom", "curve.kappa",
                                 "curve.circle", "curve.fromEdges", "curve.sketchTool", "#Surfaces", "curve.extrude", "curve.revolve", "curve.loft",
                                 "curve.patchFill", "curve.network", "curve.borderBlend", "#Pipes, Cables, Stitches and Straps", "curve.pipeTool", "curve.pipe",
                                 "curve.pipePresetPipe", "curve.pipePresetHose", "curve.pipePresetCable", "curve.pipePresetConduit", "curve.pipeRouteTool",
                                 "curve.pipeSupports", "curve.carveChannel", "curve.cableTool",
                                 "curve.cable", "curve.stitch", "curve.strapTool", "curve.strapPresetStrap", "curve.strapPresetRope", "curve.strapPresetTape",
                                 "curve.strapPresetMetalBand", "curve.strapPresetZipTie", "#Edit", "curve.fillet", "curve.offset", "curve.rebuild", "curve.evenSpacing", "curve.fit", "curve.project",
                                 "curve.attach", "curve.detach", "curve.reverse", "curve.close", "curve.open", "curve.deletePoints", "#Blend and Trim",
                                 "curve.blendTool", "curve.blend", "curve.match", "curve.trimTool", "curve.extend", "curve.split", "curve.fair"});
    addSubmenu("Curves", "Planar");
    menus::addActions("Curves/Planar", {"curve.text", "curve.textMesh", "curve.bevelPlus", "", "curve.booleanUnion", "curve.booleanSubtract", "curve.booleanIntersect",
                                        "curve.booleanXor", "", "curve.offsetRegion", "curve.insetSkeleton", "curve.centerLine", "", "curve.roof"});
    addSubmenu("Curves", "Deform Curves");
    menus::addActions("Curves/Deform Curves", {"curve.bend", "curve.curl", "curve.scaleCurvature", "curve.smooth"});
    addSubmenu("Curves", "Along Curve");
    menus::addActions("Curves/Along Curve", {"curve.deformAlongCurve", "curve.arrayAlongCurve", "curve.instanceAlongCurve", "", "curve.flowAlongSurface"});
    addSubmenu("Curves", "Symmetry");
    menus::addActions("Curves/Symmetry", {"curve.symmetryX", "curve.symmetryY", "curve.symmetryZ", "curve.symmetryOff", "", "curve.bakeSymmetry", "",
                                          "curve.drawSymmetryX", "curve.drawSymmetryY", "curve.drawSymmetryZ", "curve.drawSymmetryOff"});
    addSubmenu("Curves", "On Surface");
    menus::addActions("Curves/On Surface", {"curve.drawOnSurfaces", "curve.keepDrawnOnSurface", "", "curve.keepOnSurface", "curve.releaseFromSurface"});
    addSubmenu("Curves", "Sections");
    menus::addActions("Curves/Sections", {"curve.contour", "curve.sectionTool"});
    addSubmenu("Curves", "Sketch");
    menus::addActions("Curves/Sketch", {"curve.sketchTool", "", "curve.sketchCombine", "curve.sketchSolve", "curve.sketchClear"});
    addSubmenu("Curves", "Groom");
    menus::addActions("Curves/Groom", {"groom.create", "groom.drawHair", "groom.brushes", "curve.sculptCurves", "groom.cards", "groom.bakeCards", "groom.import", "groom.importYeti", "groom.export", "groom.sendToUnreal"});
    addSubmenu("Curves/Groom", "Style");
    menus::addActions("Curves/Groom/Style", {"groom.style.split", "groom.style.clump", "groom.style.curl", "groom.style.wave", "groom.style.frizz",
                                             "groom.style.noise", "groom.style.length", "groom.style.cut", "groom.style.flyaways"});
    menus::addActions("Curves/Groom", {"", "groom.settle", "groom.shell", "", "curve.groomSurface", "curve.groomGuides", "curve.hairCards"});
    menus::addActions("Curves", {"#Handles", "curve.handleAuto", "curve.handleVector", "curve.handleAligned", "curve.handleFree", "#Convert",
                                 "curve.toBezier", "curve.toNurbs", "curve.toPoly", "#Display", "curve.toggleComb", "#Unreal", "curve.sendToUnreal"});

    menu("Surface", "Surface", Area::MenuBar, syncChecks, "", {"Model", "Texture"});
    menus::addActions("Surface", {"#Normals", "mesh.softenEdges", "mesh.hardenEdges", "mesh.setNormalAngle", "mesh.flip", "mesh.conform",
                                  "mesh.softFoliageNormals", "#Vertex Colors", "vcolor.paint", "vcolor.createSet", "vcolor.fill", "vcolor.gradient",
                                  "vcolor.invert", "vcolor.levels", "vcolor.blur", "#Bake to Vertex Colors", "vcolor.bakeAO", "vcolor.bakeCavity",
                                  "vcolor.bakeCurvature", "vcolor.bakeHeight", "#Display", "viewport.vertexColors.off", "viewport.vertexColors.shaded",
                                  "viewport.vertexColors.unlit", "viewport.toggleBackfaceCulling"});

    menu("Sculpt", "Sculpt", Area::MenuBar, {}, "", {"Sculpt"});
    menus::addActions("Sculpt", {"#Sculpt", "sculpt.tool", "sculpt.enter", "sculpt.exit", "sculpt.cancel", "sculpt.toggleSeeThrough", "viewport.toggleSculptLighting", "window.sculpt", "#Levels",
                                 "sculpt.levelUp", "sculpt.levelDown", "sculpt.addLevel", "sculpt.reconstruct", "sculpt.deleteLower", "sculpt.deleteHigher",
                                 "sculpt.deleteLevels", "#Duplicate", "sculpt.duplicate", "sculpt.duplicateHide", "#Shape", "sculpt.remesh",
                                 "mesh.relaxKeepForm", "mesh.reproject", "mesh.transferShape", "#Hard Surface", "layout.pencil", "layout.plates", "layout.fasteners", "layout.patterns"});

    // The modifier kinds fill in above these rows each time it opens.
    menu("Modifiers", "Modifiers", Area::MenuBar, [](QMenu* m) { fillAddModifierMenu(m, false); }, "", {"Model", "Level"});
    menus::addActions("Modifiers", {"#Stack", "window.modifierStack", "modifier.applyAll", "mesh.bakeModifierStack", "#Auto Bevel",
                                    "select.foldEdges", "bevel.editEdges", "bevel.applyEdges", "bevel.cancelEdges", "bevel.measureThin",
                                    "bevel.clearOverrides", "bevel.showEdges"});

    menu("UV", "UV", Area::MenuBar, {}, "", {"UV", "Texture"});
    menus::addActions("UV", {"window.uvEditor", "uv.auto", "uv.autoSeams", "uv.unwrap", "uv.projectPlanar", "uv.projectBox", "uv.projectSpherical", "uv.hose",
                      "uv.projectCylindrical", "uv.projectCamera", "uv.cut", "uv.sew", "uv.straighten", "uv.orient", "uv.rotateLeft", "uv.rotateRight", "uv.flipX", "uv.flipY", "uv.pack", "uv.lightmap", "uv.delete", "uv.seamView", "", "uv.autoSeams2",
                      "uv.bff", "uv.bffDisk", "uv.bffRectangle", "uv.bffCones", "uv.unwrapSymmetric", "uv.unwrapSymmetricStack"});
    addSubmenu("UV", "Pack");
    menus::addActions("UV/Pack", {"uv.pack", "uv.packAroundSelected", "uv.packSelectionOnly", "uv.packStacked", "uv.packPerObject", "uv.packPerMaterial", "uv.packUdimByMaterial", "uv.selectStacks"});
    addSubmenu("UV", "Live");
    menus::addActions("UV/Live", {"uv.liveMode", "uv.liveRelax", "uv.liveFlatten", "uv.liveSettle", "uv.pin", "uv.unpin", "uv.weld", "uv.brush3d",
                           "uv.checker3d", "uv.seams3d", "uv.includeChildren", "uv.heatmapTwoColour"});
    addSubmenu("UV", "Optimize");
    menus::addActions("UV/Optimize", {"uv.optimizeConformal", "uv.optimizeAuthalic", "uv.optimizeBlend", "uv.optimizeIsometric", "uv.mosaic"});
    addSubmenu("UV", "Align");
    menus::addActions("UV/Align", {"uv.straightenEdges", "uv.alignU", "uv.alignV", "uv.orientByEdge"});

    menu("Rig", "Rig", Area::MenuBar, {}, "", rigAnim);
    menus::addActions("Rig", {"#Skeleton", "rig.jointTool", "rig.orientJoint", "rig.mirrorJoints", "rig.insertJoint", "rig.rerootSkeleton", "rig.centerJoint",
                       "rig.snapToComponentCenter", "#Skin", "rig.bindSkin",
                       "rig.bindSkinHeatmap", "rig.unbindSkin", "rig.goToBindPose", "rig.editSkeleton", "#Weights", "rig.paintWeights", "rig.mirrorSkinWeights",
                       "rig.copySkinWeights", "rig.normalizeWeights", "rig.pruneWeights", "rig.relaxWeights", "rig.settleWeights", "rig.settleWeightsSimulate", "rig.selectAutomaticWeights", "rig.acceptWeights", "#Constraints", "rig.parentConstraint", "rig.pointConstraint",
                       "rig.orientConstraint", "rig.aimConstraint", "rig.removeConstraint", "rig.pin", "rig.bakePin", "#Curve Controls", "curve.controls",
                       "curve.removeControls", "#IK", "rig.ikHandle", "#Bend Bones", "rig.bendBones", "rig.splineChain", "rig.hoseLimb", "rig.hoseFingers", "rig.bendBoneCV", "rig.mirrorBoneCVs", "rig.bakeBendBones", "#Soft Net", "rig.softNet", "rig.softNetLimbStations", "rig.softNetRebind", "rig.mirrorSoftNetKnots", "rig.softNetShowAllKnots", "rig.softNetUnreal", "#Display", "rig.xrayJoints", "rig.showIkLimits",
                       "rig.dominantInfluenceSelect"});
    addSubmenu("Rig", "Weight Proxy");
    menus::addActions("Rig/Weight Proxy", {"rig.createWeightProxy", "rig.pushProxyWeights", "rig.pullProxyWeights"});
    addSubmenu("Rig", "Spaces");
    setupSpacesMenu();
    addSubmenu("Rig", "Skinning Method");
    menus::addActions("Rig/Skinning Method", {"rig.skinLinear", "rig.skinDualQuaternion", "rig.skinCentresOfRotation", "rig.skinDirectDeltaMush"});
    addSubmenu("Rig", "Anim Proxy");
    menus::addActions("Rig/Anim Proxy", {"rig.animProxyTool", "rig.createAnimProxy", "mesh.splitMeshTool", "", "rig.createProxySet", "rig.addToProxySet",
                                        "rig.removeFromProxySet", "", "rig.toggleAnimProxy", "rig.toggleProxyPiece"});

    menu("Deform", "Deform", Area::MenuBar, {}, "", rigAnim);
    menus::addActions("Deform", {"#Morph Targets", "deform.morph", "deform.addMorphTarget", "window.morphTargets"});
    addSubmenu("Deform", "Morph Tools");
    menus::addActions("Deform/Morph Tools", {"deform.editMorphTarget", "deform.extractMorphTarget", "deform.setMorphTarget", "deform.addInBetween",
                                             "", "deform.flipMorphTarget", "deform.symmetrizeMorphTarget", "", "deform.removeMorphTarget"});
    addSubmenu("Deform", "Correctives");
    menus::addActions("Deform/Correctives", {"deform.sculptCorrective", "deform.addCorrective", "", "deform.poseDriver", "deform.setPoseDriverTarget"});
    menus::addActions("Deform", {"deform.driveChannel", "window.drivenChannels", "#Deformers", "deform.lattice"});
    addSubmenu("Deform", "Nonlinear");
    menus::addActions("Deform/Nonlinear", {"deform.bend", "deform.twist", "deform.taper", "deform.squash", "deform.sine", "deform.wave"});
    menus::addActions("Deform", {"deform.softMod", "deform.deltaMush", "deform.wrap", "deform.cage", "curve.deformAlongCurve", "curve.flowAlongSurface", "rig.softNet"});
    addSubmenu("Deform", "Cage Coordinates");
    menus::addActions("Deform/Cage Coordinates", {"deform.cageGreen", "deform.cageSomigliana"});
    addSubmenu("Deform", "Handles");
    menus::addActions("Deform/Handles", {"deform.kelvinlet", "", "deform.arap", "deform.laplacian", "deform.addHandle", "deform.setAnchors", "",
                                         "deform.bbw", "deform.bbwSkinWeights"});
    addSubmenu("Deform", "Shape");
    menus::addActions("Deform/Shape", {"deform.shear", "deform.inflate", "deform.cast", "deform.smooth", "deform.warp", "deform.noise", "deform.textureDeformer", "",
                                       "deform.tension"});
    addSubmenu("Deform", "Surface");
    menus::addActions("Deform/Surface", {"deform.shrinkwrap", "deform.surfaceDeform", "deform.addWrapDriver", "", "deform.collision", "deform.addCollider", "",
                                         "deform.rigidify", "deform.rigidBind", "deform.skinSlide", "", "deform.uvPin", "deform.proximityPin"});
    menus::addActions("Deform", {"deform.correction", "deform.editCorrection"});
    addSubmenu("Deform", "Fields");
    menus::addActions("Deform/Fields", {"deform.field", "deform.addFieldLayer", "deform.connectField", "deform.disconnectField", "", "deform.softSelectField"});
    addSubmenu("Deform", "Weights");
    menus::addActions("Deform/Weights", {"deform.paintWeights", "deform.floodWeights", "deform.smoothWeights", "deform.mirrorWeights", "deform.copyWeights", "",
                                         "deform.exportWeights", "deform.importWeights", "", "deform.setMembership", "deform.clearMembership"});
    addSubmenu("Deform", "Display");
    menus::addActions("Deform/Display", {"deform.heatmap", "deform.timing"});
    menus::addActions("Deform", {"", "deform.rebind", "#Secondary Motion", "deform.jiggle"});
    addSubmenu("Deform", "Bake for Unreal");
    menus::addActions("Deform/Bake for Unreal", {"deform.bakeSkinWeights", "deform.bakeHelperJoints", "deform.bakeCorrectiveMorphs", "",
                                                 "deform.rangeOfMotion", "deform.exportMLDeformer", "", "deform.bakeTension", "deform.bakeSurfacePin", "deform.rigidBindToSkin"});

    menu(levelToolkit::kMenu, "Level", Area::MenuBar, {}, "", {"Level"});
    menus::addActions(levelToolkit::kMenu, {"window.levelToolkit"});
    addItem({.id = levelToolkit::kBuildSection, .menu = levelToolkit::kMenu, .kind = Kind::Section, .label = "Build"});
    addItem({.id = "curves.pipesMenu", .menu = levelToolkit::kMenu, .kind = Kind::Submenu, .label = "Pipes and Channels"});
    menus::addActions("Level/Pipes and Channels", {"curve.pipeTool", "curve.pipeRouteTool", "curve.pipeSupports", "curve.cableTool", "", "curve.carveChannel"});
    addItem({.id = levelToolkit::kDressSection, .menu = levelToolkit::kMenu, .kind = Kind::Section, .label = "Dress"});
    addItem({.id = levelToolkit::kAssetsSection, .menu = levelToolkit::kMenu, .kind = Kind::Section, .label = "Assets"});

    menu("Display", "Display", Area::MenuBar, syncChecks);
    menus::addActions("Display", {"#Shading", "viewport.wireframe", "viewport.shaded", "viewport.textured", "viewport.wireOnShaded", "viewport.defaultMaterial",
                           "viewport.greyBaseColor",
                           "viewport.toggleXray", "viewport.toggleBackfaceCulling", "#Lighting and Effects"});
    addSubmenu("Display", "Lighting", syncChecks);
    menus::addActions("Display/Lighting", {"viewport.lighting.environment", "viewport.lighting.studio", "viewport.lighting.sculpt", "viewport.lighting.flat", "viewport.resetLight"});
    addSubmenu("Display", "Vertex Colors", syncChecks);
    menus::addActions("Display/Vertex Colors", {"viewport.vertexColors.off", "viewport.vertexColors.shaded", "viewport.vertexColors.unlit", "",
                                         "viewport.colorChannel.rgb", "viewport.colorChannel.r", "viewport.colorChannel.g",
                                         "viewport.colorChannel.b", "viewport.colorChannel.a", "", "viewport.grayscaleWeights",
                                         "viewport.monotone"});
    menus::addActions("Display", {"viewport.toggleShadows", "viewport.toggleAO", "viewport.toggleAA", "viewport.toggleKuwahara",
                                  "viewport.toggleOutline"});
    addSubmenu("Display", "Antialiasing Samples", syncChecks);
    menus::addActions("Display/Antialiasing Samples", {"viewport.aaSamples2", "viewport.aaSamples4", "viewport.aaSamples8"});
    addSubmenu("Display", "Environment", [](QMenu* env) { connect(env, &QMenu::aboutToShow, env, [env] { fillEnvironmentMenu(env); }); });
    menus::addActions("Display", {"viewport.toggleEnvBackground", "#Visibility", "display.toggleVisibility", "display.hide", "display.show", "display.showLastHidden", "display.showAll",
                           "viewport.isolate", "viewport.isolateAdd", "viewport.isolateRemove"});
    addSubmenu("Display", "Level of Detail", syncChecks);
    menus::addActions("Display/Level of Detail", {"viewport.lod.screenSize", "viewport.lod.all", "", "viewport.lod.level0", "viewport.lod.level1",
                                                  "viewport.lod.level2", "viewport.lod.level3", "viewport.lod.level4", "viewport.lod.level5",
                                                  "viewport.lod.level6", "viewport.lod.level7"});
    addSubmenu("Display", "Mesh Display", syncChecks);
    for (const displayAids::Info& i : displayAids::all())
        addItem({.menu = "Display/Mesh Display", .kind = Kind::Action, .action = std::string("viewport.aid.") + i.id});
    addItem({.id = "Display/Mesh Display/normalLength", .menu = "Display/Mesh Display", .kind = Kind::Custom,
             .setup = [](QMenu* m) { m->addAction(displayAids::normalLengthRow(m)); }});
    addSubmenu("Display", "Terrain", syncChecks);
    heightfieldDisplay::addMenuItems("Display/Terrain");
    addSubmenu("Display", "Measure", syncChecks);
    menus::addActions("Display/Measure", {"measure.selection", "measure.copyDistance", "measure.copyDiameter", "measure.copyLength", "",
                                   "measure.distanceTool", "measure.angleTool", "measure.radiusTool", "", "measure.distanceFromSelection",
                                   "measure.angleFromSelection", "measure.radiusFromSelection", "", "measure.toggleShow"});
    menus::addActions("Display", {"#Layers"});
    addSubmenu("Display", "Display Layers");
    menus::addActions("Display/Display Layers", {"window.displayLayers", "layers.create", "layers.createFromSelected", "layers.addSelected",
                                          "layers.removeSelected"});
    menus::addActions("Display", {"#Camera"});
    addSubmenu("Display", "Views", syncChecks);
    menus::addActions("Display/Views", {"viewport.view.persp", "viewport.view.top", "viewport.view.front", "viewport.view.right", "viewport.view.left",
                                 "viewport.view.back", "viewport.view.bottom"});
    menus::addActions("Display", {"viewport.frameSelected", "viewport.frameAll", "viewport.frameUnderCursor", "viewport.cameraUndo", "viewport.cameraRedo", "#Overlays",
                           "viewport.toggleGrid", "viewport.toggleResolutionGate", "viewport.toggleFilmGate", "viewport.cameraSettings",
                           "window.renderSettings", "#View", "viewport.toggleToolbar", "window.toggleToolBox", "viewport.resetExposure"});

    menu("Animation", "Animation", Area::MenuBar, {}, "", {"Animate", "Rig", "Effects"});
    menus::addActions("Animation", {"#Keys", "anim.setKey", "anim.setKeyTranslate", "anim.setKeyRotate", "anim.setKeyScale", "anim.autoKeyToggle", "#Playback",
                             "anim.play", "anim.playBackward", "anim.stop", "anim.firstFrame", "anim.lastFrame", "anim.nextFrame",
                             "anim.prevFrame", "anim.nextKey", "anim.prevKey", "#Editors", "window.graphEditor", "window.timeSlider"});
    menus::addActions("Animation", {"#Ghosting and Poses"});
    addGhostingMenu("Animation");
    menus::addActions("Animation", {"window.poseLibrary", "pose.mirror", "window.controlPicker", "picker.editMode"});
    menus::addActions("Animation", {"#Offsets", "anim.createOffset", "anim.bakeOffset", "anim.deleteOffset", "window.offsets"});
    menus::addActions("Animation", {"#Retarget", "anim.retarget", "anim.characterize", "window.retargeter"});

    menu("Render", "Render", Area::MenuBar, syncChecks, "", {"Texture", "Level", "Animate", "Effects"});
    menus::addActions("Render", {"#Render", "render.frame", "render.sequence", "render.turntable", "window.renderOutput", "window.renderSettings", "#Look",
                                 "render.postProcess", "render.lightRig.threePoint", "render.lightRig.rim", "render.lightRig.product", "viewport.toggleRtShadows", "viewport.togglePostEffects", "viewport.toggleTAA", "#Framing", "viewport.toggleResolutionGate",
                                 "viewport.toggleSafeFrames", "viewport.toggleThirds"});

    menu("Windows", "Windows", Area::MenuBar, syncChecks);
    addSubmenu("Windows", "Editors", [this](QMenu* m) {
        // Filled on open: extensions add and remove panels at any time.
        connect(m, &QMenu::aboutToShow, m, [this, m] { fillEditorsMenu(m); });
        fillEditorsMenu(m);
    });
    addItem({.id = "Windows/layouts", .menu = "Windows", .kind = Kind::Custom, .setup = [this](QMenu* m) { m->addMenu(mLayouts->menu()); }});
    menus::addActions("Windows", {"window.resetLayout", "window.expandPanels", "window.toggleMaximizePanel", "#Menu Set", "menuSet.follow", "menuSet.model",
                                  "menuSet.uv", "menuSet.sculpt", "menuSet.texture", "menuSet.rig", "menuSet.animate", "menuSet.level", "#Tools",
                                  "window.modelingToolkit", "window.topologyToolkit", "window.curveToolkit", "window.uvToolkit", "window.levelToolkit",
                                  "#Show", "window.toggleToolBox",
                           "window.toggleChannelBox", "window.toggleUvEditor", "window.commandPalette", "view.presenter", "reference.addImages", "reference.clickable", "#Customize",
                           "viewport.customizeHeader", "window.customizeToolBox", "window.markingMenuEditor", "window.userMenuEditor", "window.extensionManager"});

    menu("Help", "Help");
    menus::addActions("Help", {"#Learn", "help.docs"});
    addSubmenu("Help", "Docs Topics", [](QMenu* m) { docs::attachTopicsMenu(m); });
    menus::addActions("Help", {"help.hotkeyCheatSheet", "viewport.toggleKeys", "help.updateDocs", "#Search", "window.searchMenus", "window.searchScene", "window.searchTearOffs",
                               "#Problems", "help.reportIssue", "help.myReports", "help.perfTrace", "help.supportBundle", "help.copyLastCrash",
                               "help.crashReports", "#Setup", "help.agentSetup", "help.telemetry", "help.checkUpdates", "help.prefsFolder", "",
                               "help.licenses", "help.credits", "help.about"});

    buildPanelMenus();

    mMenus = new MenuBuilder(this, [this](QMenu* m, bool topLevel) {
        if (topLevel)
            tearoff::enable(m);
        refreshKeysOnShow(m);
    });
    connect(mMenus, &MenuBuilder::rebuilt, this, [this] {
        menuBar()->clear();
        for (QMenu* m : mMenus->menus(Area::MenuBar))
            menuBar()->addMenu(m);
        applyMenuSet();
        // The layout switcher lives in the menu bar corner, so the hotbox carries it too for when the menu bar is hidden.
        std::vector<QMenu*> panels = mMenus->menus(Area::Hotbox);
        panels.push_back(mLayouts->menu());
        if (Hotbox* hb = Hotbox::instance())
            hb->setPanelMenus(std::move(panels));
        else
            Hotbox::install(this, std::move(panels));
    });
    mMenus->rebuild();

    menuContext::install(this);
    menuContext::onChanged([this] {
        applyMenuSet();
        raiseToolsFor(menuContext::current());
    });
    PrefStore::get().onChanged([this](const std::string& file) {
        if (file == "general")
            applyMenuSet();
    });
    applyMenuSet();
}

void MainWindow::raiseToolsFor(const std::string& set)
{
    // The tools column follows the work too: its tab for the set comes forward when that panel is open.
    static const std::map<std::string, const char*> kTools = {{"Model", kModelingToolkitDock}, {"Level", kLevelToolkitDock}, {"Texture", "textureSets"}, {"Cloth", "tailorToolkit"}};
    auto it = kTools.find(set);
    if (it == kTools.end())
        return;
    QDockWidget* d = findChild<QDockWidget*>(it->second);
    if (!d || d->isHidden() || d->isFloating() || dockShowing(d))
        return;
    // A tab of the same set already in front (Topology for Model) stays.
    for (QDockWidget* t : tabifiedDockWidgets(d))
        if (dockShowing(t) && menuContext::setForDock(t->objectName().toStdString()) == set)
            return;
    d->raise();
}

void MainWindow::applyMenuSet()
{
    // The menu bar's rows are the area's menus in registry order.
    std::vector<const menus::Menu*> defs = menus::menusIn(menus::Area::MenuBar);
    const QList<QAction*> rows = menuBar()->actions();
    for (qsizetype i = 0; i < rows.size() && i < qsizetype(defs.size()); ++i)
        menuSets::setHiddenBySet(rows[i], !menuSets::shows(*defs[size_t(i)], menuContext::current()));
    if (mTopBar)
        mTopBar->relayout();
}

void MainWindow::fillEditorsMenu(QMenu* menu)
{
    // Panels by what they are for; a panel not named here (most extension panels) goes under Other.
    static const std::vector<std::pair<const char*, std::vector<const char*>>> kGroups = {
        {"Scene", {"Outliner", "Attributes", "Channels", "Layers", "Quick Select Sets", "Rename", "Reference Editor",
                   "Source Control", "Undo History", "Notes", "Shelf", "Docs", "Relationships"}},
        {"Modeling", {"Modeling", "Topology", "Curves", "Modifiers", "Node Editor", "Morph Targets", "Driven Channels", "Component Spreadsheet",
                      "Foliage Blades", "Tree Generator", "Ground Tiles", "Effects"}},
        {"UVs and Texturing", {"UV Editor", "Material Editor", "Material Browser", "Texture Set", "Texture Graph", "Texture", "2D View", "Baker"}},
        {"Animation and Rigging", {"Graph Editor", "Time Slider", "Anim Layers", "Pose Library", "Control Picker", "Offsets", "Rig Templates",
                                   "Puppeteer", "Anim Bank", "Stacker", "Mixer", "Cycle Maker", "Overlap", "Physics Animation"}},
        {"Level", {"Level", "Dress", "Prop Library"}},
        {"Scripting", {"Script Editor", "Script Output", "API Reference", "Terminal", "Batch Script Execution"}},
    };
    menu->clear();
    const QList<QDockWidget*> found = findChildren<QDockWidget*>();
    // Torn-off menus carry their menu's title, so listing them here doubles the panels they share a name with; Ctrl+T's
    // Tear-offs tab lists them.
    std::vector<QDockWidget*> docks;
    for (QDockWidget* d : found)
        if (!tearoff::parseDockName(d->objectName().toStdString()))
            docks.push_back(d);
    std::sort(docks.begin(), docks.end(), [](QDockWidget* a, QDockWidget* b) {
        return QString::localeAwareCompare(a->windowTitle(), b->windowTitle()) < 0;
    });
    std::vector<QDockWidget*> placed;
    auto section = [&](const QString& title, const std::vector<QDockWidget*>& members) {
        if (members.empty())
            return;
        menu->addSection(title);
        for (QDockWidget* d : members)
        {
            if (const panels::Panel* p = panels::registry().find(d->objectName().toStdString()))
                d->toggleViewAction()->setIcon(icon(QString::fromStdString(p->icon)));
            menu->addAction(d->toggleViewAction());
        }
    };
    for (const auto& [group, titles] : kGroups)
    {
        std::vector<QDockWidget*> members;
        for (QDockWidget* d : docks)
            if (std::find(titles.begin(), titles.end(), d->windowTitle().trimmed().toStdString()) != titles.end())
                members.push_back(d);
        placed.insert(placed.end(), members.begin(), members.end());
        section(group, members);
    }
    std::vector<QDockWidget*> rest;
    for (QDockWidget* d : docks)
        if (std::find(placed.begin(), placed.end(), d) == placed.end() && !d->windowTitle().trimmed().isEmpty())
            rest.push_back(d);
    section("Other", rest);
}

void MainWindow::buildPanelMenus()
{
    // Maya's viewport panel menus. Lila's views have no menu bar of their own, so these live in the hotbox.
    using namespace menus;
    for (const char* title : {"View", "Shading", "Show", "Renderer", "Panels"})
        addMenu({.id = std::string("panel.") + title, .title = title, .area = Area::Hotbox});
    menus::addActions("panel.View", {"viewport.view.persp", "viewport.view.top", "viewport.view.front", "viewport.view.right", "viewport.view.left",
                              "viewport.view.back", "viewport.view.bottom", "", "viewport.frameSelected", "viewport.frameAll",
                              "viewport.cameraUndo", "viewport.cameraRedo", "", "viewport.toggleResolutionGate", "viewport.toggleFilmGate",
                              "viewport.cameraSettings", "", "viewport.imagePlaneImport", "referenceOverlay.addImage", "referenceOverlay.adjust",
                              "referenceOverlay.bindView", "referenceOverlay.goToView", "referenceOverlay.park"});
    menus::addActions("panel.Shading", {"viewport.wireframe", "viewport.shaded", "viewport.textured", "viewport.wireOnShaded", "", "viewport.toggleAO",
                                 "viewport.aid.twoSidedLighting", "viewport.aid.backfaceTint",
                                 "", "display.smoothOff", "display.smoothCage", "display.smoothOn", "viewport.smoothSubdivisions"});
    menus::addActions("panel.Show", {"display.hide", "display.show", "display.showLastHidden", "display.showAll", "", "viewport.isolate", "viewport.isolateAdd",
                              "viewport.isolateRemove", "viewport.show.curve", "viewport.show.mesh", "viewport.show.joint", "viewport.show.light",
                              "viewport.show.locator", "rig.xrayJoints", "measure.toggleShow",
                              "viewport.toggleImagePlanes", "referenceOverlay.toggle", "", "viewport.toggleDebugDraw", "viewport.clearDebugDraw"});
    menus::addActions("panel.Renderer", {"window.renderSettings"});
    menus::addActions("panel.Panels", {"viewport.lookThroughSelected"});
    // Maya's Panels > Perspective / Orthographic: the views plus every camera node, rebuilt as the menu opens.
    addSubmenu("panel.Panels", "Cameras", [this](QMenu* cameras) {
        connect(cameras, &QMenu::aboutToShow, cameras, [this, cameras] { fillCameraMenu(cameras, mViewports->active()); });
    });
    menus::addActions("panel.Panels", {"", "viewport.toggleQuad", "window.toggleMaximizePanel", "", "window.uvEditor", "window.graphEditor",
                                "window.nodeEditor", "window.materialEditor"});
}

void MainWindow::buildStatusBar()
{
    auto* help = new QLabel(this);
    help->setMinimumWidth(1);
    statusBar()->addWidget(help, 1);
    qApp->installEventFilter(new HelpLine(help, this));
    menuSearch::install();
    // Held-key states take the help line while they last, then hand back whatever it said before.
    cursorBadges::setStatusSink([help, saved = std::make_shared<std::optional<QString>>()](const QString& text) {
        if (!text.isEmpty() && !*saved)
            *saved = help->text();
        if (!text.isEmpty())
            help->setText(text);
        else if (*saved)
        {
            help->setText(**saved);
            saved->reset();
        }
    });
    if (QDockWidget* output = findChild<QDockWidget*>("scriptOutput"))
    {
        auto* line = new StatusOutput([output] { revealDock(output); },
                                      [output] { return dockShowing(output); }, this);
        // At the left, after the bottom panels' tabs when they are already there, so the last output line reads first.
        const bool tabs = statusBar()->findChild<QToolBar*>("sidebarStatus", Qt::FindDirectChildrenOnly) != nullptr;
        statusBar()->insertWidget(tabs ? 1 : 0, line, 1);
        connect(output, &QDockWidget::visibilityChanged, line, qOverload<>(&QWidget::update));
    }
    installSpinScrub(this);
    installWheelGuard(this);

    mWorkspaceLabel = new QToolButton(this);
    mWorkspaceLabel->setObjectName("topFolder");
    mWorkspaceLabel->setIcon(icon("folder"));
    mWorkspaceLabel->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mWorkspaceLabel->setFocusPolicy(Qt::NoFocus);
    auto* folderMenu = new QMenu(mWorkspaceLabel);
    connect(folderMenu, &QMenu::aboutToShow, this, [this, folderMenu] {
        folderMenu->clear();
        folderMenu->addAction(icon("folder-open"), "Open Folder...", this, [this] { openFolder(); });
        if (mRecentMenu)
            folderMenu->addMenu(mRecentMenu);
        if (Workspace::get().isOpen())
        {
            const QString root = QString::fromStdWString(Workspace::get().root().wstring());
            folderMenu->addSeparator();
            folderMenu->addAction(icon("folder-search"), "Open Containing Folder", this, [root] { fileOps::reveal(root); });
        }
    });
    mWorkspaceLabel->setMenu(folderMenu);
    // With no folder open a click opens one; with one open it offers another or a recent one.
    connect(mWorkspaceLabel, &QToolButton::clicked, this, [this] {
        if (!Workspace::get().isOpen())
            openFolder();
        else
            mWorkspaceLabel->showMenu();
    });
    refreshWorkspaceLabel();
    mTopBar = new TopBar(menuBar(), mLayouts->tabs(), mWorkspaceLabel, modes::createSwitcher(this), this);
    menuBar()->setCornerWidget(mTopBar, Qt::TopRightCorner);

    mSourceLabel = new QLabel(this);
    mSourceLabel->setStyleSheet("color: #9a9a9a;");
    statusBar()->addPermanentWidget(mSourceLabel);

    mMcpButton = new QToolButton(this);
    mMcpButton->setText("MCP");
    mMcpButton->setCheckable(true);
    mMcpButton->setChecked(mcp::Server::get().running());
    mMcpButton->setToolTip("Let AI tools drive Lila over MCP (localhost only). Connection details: <prefs>/mcp.json");
    connect(mMcpButton, &QToolButton::toggled, this, [this](bool on) {
        PrefStore& p = PrefStore::get();
        p.set("general", "/mcp/enabled", on);
        if (on)
            mcp::Server::get().start(p.value<int>("general", "/mcp/port", 8740), p.value<std::string>("general", "/mcp/token", ""));
        else
            mcp::Server::get().stop();
        QSignalBlocker block(mMcpButton);
        mMcpButton->setChecked(mcp::Server::get().running());
    });
    statusBar()->addPermanentWidget(mMcpButton);

    auto* prefs = new QToolButton(this);
    prefs->setIcon(icon("settings"));
    prefs->setToolTip("Preferences");
    prefs->setAutoRaise(true);
    connect(prefs, &QToolButton::clicked, this, [this] { PreferencesDialog::open(this); });
    statusBar()->addPermanentWidget(prefs);
    progressui::install(this);
}

void MainWindow::refreshWorkspaceLabel()
{
    auto& ws = Workspace::get();
    if (ws.isOpen())
    {
        std::filesystem::path root = ws.root();
        QString name = QString::fromStdWString(root.filename().wstring());
        QString parent = root.has_parent_path() && root.parent_path() != root.root_path()
                             ? QString::fromStdWString(root.parent_path().filename().wstring()) + "\\"
                             : QString();
        mWorkspaceLabel->setText(parent + name);
        QString full = QDir::toNativeSeparators(QString::fromStdWString(root.wstring()));
        mWorkspaceLabel->setToolTip("Project folder: " + full + "\nClick to open another or a recent one");
        mWorkspaceLabel->setStatusTip(full);
    }
    else
    {
        mWorkspaceLabel->setText("Open Folder...");
        mWorkspaceLabel->setToolTip("Set the project folder: scenes, references and layouts are found from it (File > Open Folder)");
        mWorkspaceLabel->setStatusTip(QString());
    }
    refreshTitle();
}

void MainWindow::refreshRecentFolders()
{
    if (!mRecentMenu)
        return;
    mRecentMenu->clear();
    json recent = PrefStore::get().value("general", "/recentFolders", json::array());
    if (!recent.is_array() || recent.empty())
    {
        mRecentMenu->addAction("(none)")->setEnabled(false);
        return;
    }
    for (const json& r : recent)
    {
        if (!r.is_string())
            continue;
        QString path = QString::fromStdString(r.get<std::string>());
        mRecentMenu->addAction(path, [path] { Workspace::get().open(path.toStdWString()); });
    }
}

void MainWindow::openFolder()
{
    QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
    QString dir = ui::fileDialog::getExistingDirectory(this, "Open Folder", start);
    if (!dir.isEmpty())
        Workspace::get().open(dir.toStdWString());
}

void MainWindow::showAbout()
{
    QDialog dlg(this);
    dlg.setWindowTitle("About Lila");

    auto* logo = new QLabel;
    logo->setPixmap(windowIcon().pixmap(64));
    logo->setAlignment(Qt::AlignTop);

    auto* name = new QLabel(QString("<h2>Lila %1</h2>").arg(version::number()));
    auto* copy = new QToolButton;
    copy->setIcon(icon("copy"));
    copy->setAutoRaise(true);
    copy->setToolTip("Copy version and build");
    connect(copy, &QToolButton::clicked, &dlg, [copy] {
        QApplication::clipboard()->setText(QString::fromStdString("Lila " + version::full()));
        copy->setIcon(icon("check"));
        QTimer::singleShot(1500, copy, [copy] { copy->setIcon(icon("copy")); });
    });
    auto* heading = new QHBoxLayout;
    heading->addWidget(name);
    heading->addWidget(copy);
    heading->addStretch();

    auto* body = new QLabel(QString("<p>Build %1</p>"
                                    "<p><i>Lila (&#2354;&#2368;&#2354;&#2366;, LEE-laah): Sanskrit for divine play, or creation.</i></p>"
                                    "<p>Copyright &copy; 2026 Jared Taylor. All rights reserved.</p>")
                                .arg(version::buildId()));
    body->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* text = new QVBoxLayout;
    text->addLayout(heading);
    text->addWidget(body);
    text->addStretch();
    auto* row = new QHBoxLayout;
    row->setSpacing(16);
    row->addWidget(logo);
    row->addLayout(text, 1);

    std::vector<std::string> named;
    for (const credits::Supporter& s : credits::current().supporters)
        if (s.tier >= credits::Tier::Mythic)
            named.push_back(s.name);
    if (!named.empty())
    {
        QStringList list;
        for (const std::string& n : named)
            list << QString::fromStdString(n).toHtmlEscaped();
        auto* thanks = new QLabel(QString("<p>Made possible by <b>%1</b>.</p>").arg(list.join(", ")));
        thanks->setWordWrap(true);
        thanks->setTextInteractionFlags(Qt::TextSelectableByMouse);
        text->insertWidget(text->count() - 1, thanks);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QPushButton* creditsButton = buttons->addButton("Credits", QDialogButtonBox::ActionRole);
    // The About box is modal, so it closes before the credits open.
    connect(creditsButton, &QPushButton::clicked, &dlg, [this, &dlg] {
        dlg.accept();
        QTimer::singleShot(0, this, [this] { showCredits(this); });
    });
    QPushButton* licenseButton = buttons->addButton("License", QDialogButtonBox::ActionRole);
    connect(licenseButton, &QPushButton::clicked, &dlg, [this, &dlg] {
        dlg.accept();
        QTimer::singleShot(0, this, [this] { showLilaLicense(this); });
    });
    auto* layout = new QVBoxLayout(&dlg);
    layout->addLayout(row);
    layout->addWidget(buttons);
    dlg.exec();
}

} // namespace lila::ui
