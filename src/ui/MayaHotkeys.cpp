#include "ui/MayaHotkeys.h"

#include "ops/Command.h"
#include "ui/Actions.h"
#include "ui/HotkeyChord.h"
#include "ui/Keymaps.h"

#include <QStandardPaths>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace lila::ui::mayaHotkeys
{
using json = nlohmann::json;

namespace
{
struct Token
{
    enum Kind
    {
        Word,
        String,
        Punct
    } kind;
    std::string text;
};

/** MEL split into statements of words, strings and punctuation, comments dropped. */
std::vector<std::vector<Token>> statements(const std::string& s)
{
    std::vector<std::vector<Token>> out(1);
    size_t i = 0, n = s.size();
    while (i < n)
    {
        char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c)))
            ++i;
        else if (c == '/' && i + 1 < n && s[i + 1] == '/')
            i = std::min(n, s.find('\n', i));
        else if (c == '/' && i + 1 < n && s[i + 1] == '*')
        {
            size_t end = s.find("*/", i + 2);
            i = end == std::string::npos ? n : end + 2;
        }
        else if (c == '"')
        {
            std::string text;
            for (++i; i < n && s[i] != '"'; ++i)
            {
                if (s[i] == '\\' && i + 1 < n)
                {
                    char e = s[++i];
                    text += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                }
                else
                    text += s[i];
            }
            ++i;
            out.back().push_back({Token::String, std::move(text)});
        }
        // Braces end statements too, so hotkeys inside `if (`about -mac`) { ... }` still read.
        else if (c == ';' || c == '{' || c == '}')
        {
            if (!out.back().empty())
                out.emplace_back();
            ++i;
        }
        else if (c == '(' || c == ')' || c == '+')
        {
            out.back().push_back({Token::Punct, std::string(1, c)});
            ++i;
        }
        else
        {
            size_t start = i;
            while (i < n && !std::isspace(static_cast<unsigned char>(s[i])) && !std::strchr(";\"(){}", s[i]))
                ++i;
            out.back().push_back({Token::Word, s.substr(start, i - start)});
        }
    }
    if (out.back().empty())
        out.pop_back();
    return out;
}

const std::unordered_map<std::string, std::string>& flagAliases()
{
    static const std::unordered_map<std::string, std::string> m = {
        {"k", "keyShortcut"},     {"n", "name"},           {"rn", "releaseName"},     {"ctl", "ctrlModifier"},
        {"alt", "altModifier"},   {"sht", "shiftModifier"}, {"cmd", "commandModifier"}, {"ctc", "ctxClient"},
        {"c", "command"},         {"stp", "sourceType"},    {"cl", "commandLanguage"},
    };
    return m;
}

struct Statement
{
    std::string command;
    std::map<std::string, std::string> flags; // long name -> value ("" for a bare boolean)
    std::string object;                       // trailing positional word
};

Statement read(const std::vector<Token>& tokens)
{
    Statement st;
    if (tokens.empty() || tokens[0].kind != Token::Word)
        return st;
    st.command = tokens[0].text;
    for (size_t i = 1; i < tokens.size(); ++i)
    {
        const Token& t = tokens[i];
        // A string outside any flag is the object too: nameCommand ... "NameComUndo";
        if (t.kind == Token::String ||
            (t.kind == Token::Word &&
             (t.text.size() < 2 || t.text[0] != '-' || std::isdigit(static_cast<unsigned char>(t.text[1])))))
        {
            st.object = t.text;
            continue;
        }
        if (t.kind != Token::Word)
            continue;
        std::string flag = t.text.substr(1);
        if (auto it = flagAliases().find(flag); it != flagAliases().end())
            flag = it->second;
        std::string value;
        size_t j = i + 1;
        auto takeStrings = [&](bool paren) {
            // ("a" + "b"), "a" + "b"
            bool wantString = true;
            for (; j < tokens.size(); ++j)
            {
                const Token& v = tokens[j];
                if (v.kind == Token::String && wantString)
                {
                    value += v.text;
                    wantString = false;
                }
                else if (v.kind == Token::Punct && v.text == "+")
                    wantString = true;
                else if (paren && v.kind == Token::Punct && v.text == ")")
                {
                    ++j;
                    break;
                }
                else
                    break;
            }
        };
        if (j < tokens.size() && tokens[j].kind == Token::Punct && tokens[j].text == "(")
        {
            ++j;
            takeStrings(true);
        }
        else if (j < tokens.size() && tokens[j].kind == Token::String)
            takeStrings(false);
        else if (j + 1 < tokens.size() && tokens[j].kind == Token::Word && tokens[j].text[0] != '-')
            value = tokens[j++].text; // the last word is the object, never a value
        st.flags[flag] = value;
        i = j - 1;
    }
    return st;
}

bool flagOn(const Statement& st, const char* flag)
{
    auto it = st.flags.find(flag);
    if (it == st.flags.end())
        return false;
    std::string v = it->second;
    return !(v == "false" || v == "0" || v == "off" || v == "no");
}

std::string trimmed(std::string s)
{
    auto notSpace = [](unsigned char c) { return !std::isspace(c) && c != ';'; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

bool isIdentifier(const std::string& s)
{
    return !s.empty() && !std::isdigit(static_cast<unsigned char>(s[0])) &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

/** Maya runtime commands (the ones Maya's own hotkeys and menus call) to Lila actions. */
const std::unordered_map<std::string, std::string>& actionTable()
{
    static const std::unordered_map<std::string, std::string> m = {
        // Tools
        {"SelectTool", "tool.select"},
        {"SelectToolMarkingMenu", "tool.select"},
        {"SelectToolOptionsMarkingMenu", "tool.select"},
        {"MoveTool", "tool.move"},
        {"TranslateToolMarkingMenu", "tool.move"},
        {"TranslateToolWithSnapMarkingMenu", "tool.move"},
        {"RotateTool", "tool.rotate"},
        {"RotateToolMarkingMenu", "tool.rotate"},
        {"RotateToolWithSnapMarkingMenu", "tool.rotate"},
        {"IncreaseManipulatorSize", "manip.sizeUp"},
        {"DecreaseManipulatorSize", "manip.sizeDown"},
        {"ScaleTool", "tool.scale"},
        {"ScaleToolMarkingMenu", "tool.scale"},
        {"ScaleToolWithSnapMarkingMenu", "tool.scale"},
        {"LassoTool", "select.lassoTool"},
        {"ArtPaintSelectTool", "select.paintTool"},
        {"EnterEditMode", "tool.pivotEditToggle"},
        {"EnterEditModePress", "tool.pivotEditOn"},
        {"EnterEditModeRelease", "tool.pivotEditOff"},
        // Snapping
        {"SnapToGridPress", "snap.gridOn"},
        {"SnapToGridRelease", "snap.gridOff"},
        {"SnapToCurvePress", "snap.edgeOn"},
        {"SnapToCurveRelease", "snap.edgeOff"},
        {"SnapToPointPress", "snap.vertexOn"},
        {"SnapToPointRelease", "snap.vertexOff"},
        // Selection
        {"SelectAll", "edit.selectAll"},
        {"SelectNone", "edit.deselect"},
        {"InvertSelection", "edit.invertSelection"},
        {"SelectHierarchy", "select.hierarchy"},
        {"PickWalkUp", "select.pickWalkUp"},
        {"PickWalkDown", "select.pickWalkDown"},
        {"PickWalkLeft", "select.pickWalkLeft"},
        {"PickWalkRight", "select.pickWalkRight"},
        {"SelectToggleMode", "select.objectMode"},
        {"SelectObjectMode", "select.objectMode"},
        {"SelectVertexMask", "select.vertexMode"},
        {"SelectEdgeMask", "select.edgeMode"},
        {"SelectFacetMask", "select.faceMode"},
        {"ConvertSelectionToVertices", "select.toVertices"},
        {"ConvertSelectionToEdges", "select.toEdges"},
        {"ConvertSelectionToFaces", "select.toFaces"},
        {"GrowPolygonSelectionRegion", "select.grow"},
        {"ShrinkPolygonSelectionRegion", "select.shrink"},
        {"SelectEdgeLoop", "select.loop"},
        {"SelectEdgeLoopSp", "select.loop"},
        {"SelectEdgeRing", "select.ring"},
        {"SelectEdgeRingSp", "select.ring"},
        {"SelectPolygonSelectionBoundary", "select.border"},
        // Edit
        {"Undo", "edit.undo"},
        {"Redo", "edit.redo"},
        {"RepeatLast", "edit.repeatLast"},
        {"Delete", "edit.delete"},
        {"Duplicate", "edit.duplicate"},
        {"DuplicateWithTransform", "edit.duplicateWithTransform"},
        {"Instance", "edit.instance"},
        {"Group", "edit.group"},
        {"Ungroup", "edit.ungroup"},
        {"Parent", "edit.parent"},
        {"Unparent", "edit.unparent"},
        {"FreezeTransformations", "modify.freezeTransforms"},
        {"ResetTransformations", "modify.resetTransforms"},
        {"CenterPivot", "modify.centerPivot"},
        {"BakeCustomPivot", "modify.bakePivot"},
        // Display
        {"HideSelectedObjects", "display.hide"},
        {"ShowSelectedObjects", "display.show"},
        {"ShowAll", "display.showAll"},
        {"ShowLastHidden", "display.showLastHidden"},
        {"ToggleVisibilityAndKeepSelection", "display.toggleVisibility"},
        {"LowQualityDisplay", "display.smoothOff"},
        {"MediumQualityDisplay", "display.smoothCage"},
        {"HighQualityDisplay", "display.smoothOn"},
        {"DisplayWireframe", "viewport.wireframe"},
        {"DisplayShaded", "viewport.shaded"},
        {"DisplayShadedAndTextured", "viewport.textured"},
        {"DisplayLight", "viewport.toggleLighting"},
        {"ToggleBackfaceGeometry", "viewport.toggleBackfaceCulling"},
        {"ToggleGrid", "viewport.toggleGrid"},
        {"ToggleFaceNormalDisplay", "viewport.aid.faceNormals"},
        {"ToggleVertexNormalDisplay", "viewport.aid.vertexNormals"},
        {"ToggleBorderEdges", "viewport.aid.borderEdges"},
        {"FrameSelected", "viewport.frameSelected"},
        {"FrameSelectedWithoutChildren", "viewport.frameSelected"},
        {"FrameSelectedInAllViews", "viewport.frameSelected"},
        {"FrameAll", "viewport.frameAll"},
        {"FrameAllInAllViews", "viewport.frameAll"},
        {"PanePop", "viewport.toggleQuad"},
        {"UndoViewChange", "viewport.cameraUndo"},
        {"RedoViewChange", "viewport.cameraRedo"},
        {"LookThroughSelected", "viewport.lookThroughSelected"},
        // Animation
        {"SetKey", "anim.setKey"},
        {"SetKeyTranslate", "anim.setKeyTranslate"},
        {"SetKeyRotate", "anim.setKeyRotate"},
        {"SetKeyScale", "anim.setKeyScale"},
        {"NextFrame", "anim.nextFrame"},
        {"PreviousFrame", "anim.prevFrame"},
        {"NextKey", "anim.nextKey"},
        {"PreviousKey", "anim.prevKey"},
        {"PlaybackToggle", "anim.play"},
        {"PlaybackForward", "anim.play"},
        {"PlaybackBackward", "anim.playBackward"},
        {"PlaybackStop", "anim.stop"},
        {"GoToMinFrame", "anim.firstFrame"},
        {"GoToMaxFrame", "anim.lastFrame"},
        {"AutoKeyOnOff", "anim.autoKeyToggle"},
        // Modeling
        {"PolyExtrude", "mesh.extrude"},
        {"BevelPolygon", "mesh.bevel"},
        {"BridgeEdge", "mesh.bridge"},
        {"InsertEdgeLoopTool", "mesh.insertLoop"},
        {"OffsetEdgeLoopTool", "mesh.offsetLoop"},
        {"MultiCutTool", "mesh.knife"},
        {"dR_multiCutTool", "mesh.knife"},
        {"dR_connectTool", "mesh.connect"},
        {"TargetWeldTool", "mesh.targetWeld"},
        {"dR_targetWeldTool", "mesh.targetWeld"},
        {"MergeVertices", "mesh.merge"},
        {"MergeToCenter", "mesh.mergeCenter"},
        {"PolygonCollapse", "mesh.collapse"},
        {"CombinePolygons", "mesh.combine"},
        {"SeparatePolygon", "mesh.separate"},
        {"ExtractFace", "mesh.extract"},
        {"DuplicateFace", "mesh.duplicateFaces"},
        {"FillHole", "mesh.fillHole"},
        {"PokePolygon", "mesh.poke"},
        {"Triangulate", "mesh.triangulate"},
        {"Quadrangulate", "mesh.quadrangulate"},
        {"SubdividePolygon", "mesh.subdivide"},
        {"SmoothPolygon", "mesh.subdivide"},
        {"ReducePolygon", "mesh.reduce"},
        {"ReversePolygonNormals", "mesh.flip"},
        {"PolygonSoftenEdge", "mesh.softenEdges"},
        {"PolygonHardenEdge", "mesh.hardenEdges"},
        {"PolyCircularize", "mesh.circularize"},
        {"PolyEditEdgeFlow", "mesh.editEdgeFlow"},
        {"PolySpinEdgeForward", "mesh.spin"},
        {"PolygonBooleanUnion", "mesh.booleanUnion"},
        {"PolygonBooleanDifference", "mesh.booleanDifference"},
        {"PolygonBooleanIntersection", "mesh.booleanIntersection"},
        {"CreatePolygonCube", "create.cube"},
        {"CreatePolygonSphere", "create.sphere"},
        {"CreatePolygonCylinder", "create.cylinder"},
        {"CreatePolygonCone", "create.cone"},
        {"CreatePolygonPlane", "create.plane"},
        {"CreatePolygonTorus", "create.torus"},
        {"CreatePolygonDisc", "create.disc"},
        {"CreatePolygonPipe", "create.pipe"},
        {"CreateLocator", "create.locator"},
        {"CreateCameraOnly", "create.camera"},
        {"CreateEmptyGroup", "create.emptyGroup"},
        // UVs
        {"TextureViewWindow", "window.uvEditor"},
        {"u3dUnfold", "uv.unwrap"},
        {"u3dLayout", "uv.pack"},
        {"PolygonCutUVs", "uv.cut"},
        {"PolygonSewUVs", "uv.sew"},
        {"UVAutomaticProjection", "uv.auto"},
        {"UVPlanarProjection", "uv.projectPlanar"},
        // Rigging
        {"JointTool", "rig.jointTool"},
        {"InsertJointTool", "rig.insertJoint"},
        {"IKHandleTool", "rig.ikHandle"},
        {"MirrorJoint", "rig.mirrorJoints"},
        {"OrientJoint", "rig.orientJoint"},
        {"RerootSkeleton", "rig.rerootSkeleton"},
        {"SmoothBindSkin", "rig.bindSkin"},
        {"DetachSkin", "rig.unbindSkin"},
        {"GoToBindPose", "rig.goToBindPose"},
        {"ArtPaintSkinWeightsTool", "rig.paintWeights"},
        {"MirrorSkinWeights", "rig.mirrorSkinWeights"},
        {"CopySkinWeights", "rig.copySkinWeights"},
        {"ParentConstraint", "rig.parentConstraint"},
        {"PointConstraint", "rig.pointConstraint"},
        {"OrientConstraint", "rig.orientConstraint"},
        {"AimConstraint", "rig.aimConstraint"},
        // Windows
        {"GraphEditor", "window.graphEditor"},
        {"NodeEditorWindow", "window.nodeEditor"},
        {"HypershadeWindow", "window.materialEditor"},
        {"ScriptEditor", "window.scriptEditor"},
        {"ComponentEditor", "window.componentSheet"},
        {"ReferenceEditor", "window.referenceEditor"},
        {"DisplayLayerEditorWindow", "window.displayLayers"},
        {"ShowModelingToolkit", "window.modelingToolkit"},
        {"HotkeyPreferencesWindow", "window.hotkeyEditor"},
        {"PreferencesWindow", "window.preferences"},
        {"ShowAttributeEditorOrChannelBox", "window.toggleChannelBox"},
        {"ToggleChannelsLayers", "window.toggleChannelBox"},
        {"ToggleUVEditor", "window.toggleUvEditor"},
        {"CreateCameraFromView", "create.cameraFromView"},
        {"DeletePolyElements", "mesh.dissolve"},
        {"ToggleWireframeOnShadedCmd", "viewport.wireOnShaded"},
        {"ToggleIsolateSelect", "viewport.isolate"},
        {"SelectAllMarkingMenu", "viewport.frameAll"},
        {"ShowHotbox", "hotbox.show"},
        {"GraphEditorFrameAll", "graphEditor.frameAll"},
        {"GraphEditorFrameSelected", "graphEditor.frameSelected"},
        {"OutlinerRevealSelected", "outliner.revealSelected"},
        {"Outliner_RenameSelectedItem", "outliner.rename"},
        {"NodeEditorCopy", "nodeEditor.copy"},
        {"NodeEditorCut", "nodeEditor.cut"},
        {"NodeEditorPaste", "nodeEditor.paste"},
        {"NodeEditorLayout", "nodeEditor.autoLayout"},
        {"SewUVs", "uv.sew"},
        {"CutUVs", "uv.cut"},
        {"UnfoldUV", "uv.unwrap"},
        {"LayoutUV", "uv.pack"},
        {"SelectToolOptions", "tool.select"},
        {"FrameSelectedWithoutChildrenInAllViews", "viewport.frameSelected"},
        {"Outliner_RevealSelected", "outliner.revealSelected"},
        {"OpenOrSwapAEWindow", "window.toggleChannelBox"},
        {"ToggleUseDefaultMaterial", "viewport.defaultMaterial"},
        {"PolyBevel", "mesh.bevel"},
        {"QuadDrawTool", "retopo.draw"},
        {"dR_quadDrawTool", "retopo.draw"},
        {"UVEditor_CutUVs", "uv.cut"},
        {"UVEditor_SewUVs", "uv.sew"},
        {"UVEditor_UnfoldUVs", "uv.unwrap"},
        {"UVEditor_LayoutUVs", "uv.pack"},
        {"UVEditor_DeleteUVs", "uv.delete"},
        // File
        {"NewScene", "file.new"},
        {"OpenScene", "file.open"},
        {"SaveScene", "file.save"},
        {"SaveSceneAs", "file.saveAs"},
        {"IncrementAndSave", "file.incrementAndSave"},
        {"Import", "file.import"},
        {"ExportSelection", "file.exportSelection"},
        {"CreateReference", "file.createReference"},
        {"Quit", "file.exit"},
    };
    return m;
}

/** Maya's own named commands (namedCommandSetup.mel) that older hotkey sets bind, to their runtime commands. */
const std::unordered_map<std::string, std::string>& builtinNameCommands()
{
    static const std::unordered_map<std::string, std::string> m = {
        {"NameComCreate_Camera_From_View", "CreateCameraFromView"},
        {"NameComCreate_Reference", "CreateReference"},
        {"NameComDuplicate_Selected", "Duplicate"},
        {"NameComDuplicate_Selected_with_Transforms", "DuplicateWithTransform"},
        {"NameComEnter_Tool_Edit_Mode", "EnterEditMode"},
        {"NameComEnter_Tool_Edit_Mode_Press", "EnterEditModePress"},
        {"NameComEnter_Tool_Edit_Mode_Release", "EnterEditModeRelease"},
        {"NameComFit_All_in_Active_Panel_MMenu", "SelectAllMarkingMenu"},
        {"NameComFit_All_in_All_Panels", "FrameAllInAllViews"},
        {"NameComFit_Select_with_Children_in_All_Panels", "FrameSelectedInAllViews"},
        {"NameComFit_Select_without_Children_in_All_Panels", "FrameSelectedWithoutChildrenInAllViews"},
        {"NameComFit_Selected_in_All_Panels", "FrameAll"},
        {"NameComFit_Selected_with_Children_in_Active_Panel", "FrameSelected"},
        {"NameComFit_Selected_without_Children_in_Active_Panel", "FrameSelectedWithoutChildren"},
        {"NameComGo_to_min_frame", "GoToMinFrame"},
        {"NameComGo_to_next_keyframe", "NextKey"},
        {"NameComGo_to_previous_keyframe", "PreviousKey"},
        {"NameComGroup_Selected", "Group"},
        {"NameComHide_Selected_Objects", "HideSelectedObjects"},
        {"NameComHigh_Quality_Display_Setting", "HighQualityDisplay"},
        {"NameComImport_File", "Import"},
        {"NameComInvert_Selection", "InvertSelection"},
        {"NameComLasso_Tool", "LassoTool"},
        {"NameComLow_Quality_Display_Setting", "LowQualityDisplay"},
        {"NameComMedium_Quality_Display_Setting", "MediumQualityDisplay"},
        {"NameComMove_Tool", "TranslateToolMarkingMenu"},
        {"NameComNew_File", "NewScene"},
        {"NameComOpen_File", "OpenScene"},
        {"NameComParent_Selected", "Parent"},
        {"NameComPlay_backwards_modifier", "PlaybackBackward"},
        {"NameComPlay_forwards_modifier", "PlaybackForward"},
        {"NameComPlay_forwards_modifier_release", "PlaybackStop"},
        {"NameComPop_hotBox", "ShowHotbox"},
        {"NameComQuit", "Quit"},
        {"NameComRedo", "Redo"},
        {"NameComRedo_View_Change", "RedoViewChange"},
        {"NameComRepeat_Last_Menu_Action", "RepeatLast"},
        {"NameComRotate_Tool", "RotateToolMarkingMenu"},
        {"NameComSave_File", "SaveScene"},
        {"NameComSave_File_As", "SaveSceneAs"},
        {"NameComSave_Keyframe_for_Rotate", "SetKeyRotate"},
        {"NameComSave_Keyframe_for_Scale", "SetKeyScale"},
        {"NameComSave_Keyframe_for_Translate", "SetKeyTranslate"},
        {"NameComScale_Tool", "ScaleToolMarkingMenu"},
        {"NameComSelectNone", "SelectNone"},
        {"NameComSelectTool_ObjectMMenu", "SelectToolMarkingMenu"},
        {"NameComSelect_All", "SelectAll"},
        {"NameComSelect_Tool", "SelectTool"},
        {"NameComSet_Edge_Component_Mask", "SelectEdgeMask"},
        {"NameComSet_Facet_Component_Mask", "SelectFacetMask"},
        {"NameComSet_Keyframe", "SetKey"},
        {"NameComSet_Vertex_Component_Mask", "SelectVertexMask"},
        {"NameComShaded_Display", "DisplayShaded"},
        {"NameComShaded_and_Textured_Display", "DisplayShadedAndTextured"},
        {"NameComShow_Last_Hidden", "ShowLastHidden"},
        {"NameComShow_Selected_Objects", "ShowSelectedObjects"},
        {"NameComSnap_to_Curve_modifierOFF", "SnapToCurveRelease"},
        {"NameComSnap_to_Curve_modifierON", "SnapToCurvePress"},
        {"NameComSnap_to_Grid_modifierOFF", "SnapToGridRelease"},
        {"NameComSnap_to_Grid_modifierON", "SnapToGridPress"},
        {"NameComSnap_to_Point_modifierOFF", "SnapToPointRelease"},
        {"NameComSnap_to_Point_modifierON", "SnapToPointPress"},
        {"NameComToggleVisibilityAndKeepSelection", "ToggleVisibilityAndKeepSelection"},
        {"NameComToggle_Isolate_Select", "ToggleIsolateSelect"},
        {"NameComToggle_ObjectComponent_Editing", "SelectToggleMode"},
        {"NameComToggle_Playback_OnOff", "PlaybackToggle"},
        {"NameComToggle_Use_Default_Material", "ToggleUseDefaultMaterial"},
        {"NameComToggle_WireframeOnShaded", "ToggleWireframeOnShadedCmd"},
        {"NameComTurn_on_Light_Display", "DisplayLight"},
        {"NameComUndo", "Undo"},
        {"NameComUndo_View_Change", "UndoViewChange"},
        {"NameComUnparent_Selected", "Unparent"},
        {"NameComWalk_down_the_current_hierarchy", "PickWalkDown"},
        {"NameComWalk_left_in_the_current_hierarchy", "PickWalkLeft"},
        {"NameComWalk_right_in_the_current_hierarchy", "PickWalkRight"},
        {"NameComWalk_up_the_current_hierarchy", "PickWalkUp"},
        {"NameComWireframe_Display", "DisplayWireframe"},
        {"NameIncSave_File", "IncrementAndSave"},
        {"Name_Frame_Backward", "PreviousFrame"},
        {"Name_Frame_Forward", "NextFrame"},
    };
    return m;
}

/** Maya editors (-ctxClient) to Lila hotkey contexts. */
std::string contextFor(const std::string& editor)
{
    static const std::unordered_map<std::string, std::string> m = {
        {"", "global"},
        {"nodeEditorPanel", "nodeEditor"},
        {"graphEditor", "graphEditor"},
        {"outlinerPanel", "outliner"},
        {"polyTexturePlacementPanel", "uvEditor"},
        {"scriptEditorPanel", "scriptEditor"},
    };
    auto it = m.find(editor);
    return it == m.end() ? std::string() : it->second;
}

/** An editor's own version of a general action, so F in the Node Editor frames nodes. */
std::string inContext(const std::string& context, const std::string& action)
{
    static const std::map<std::pair<std::string, std::string>, std::string> m = {
        {{"nodeEditor", "viewport.frameSelected"}, "nodeEditor.frameSelected"},
        {{"nodeEditor", "viewport.frameAll"}, "nodeEditor.frameAll"},
        {{"nodeEditor", "edit.delete"}, "nodeEditor.delete"},
        {{"nodeEditor", "edit.duplicate"}, "nodeEditor.duplicate"},
        {{"nodeEditor", "edit.group"}, "nodeEditor.groupNodes"},
        {{"nodeEditor", "edit.selectAll"}, "nodeEditor.selectAll"},
        {{"graphEditor", "viewport.frameSelected"}, "graphEditor.frameSelected"},
        {{"graphEditor", "viewport.frameAll"}, "graphEditor.frameAll"},
        {{"outliner", "viewport.frameSelected"}, "outliner.revealSelected"},
    };
    auto it = m.find({context, action});
    return it == m.end() ? action : it->second;
}

/** What a hotkey's -name or -releaseName runs: a Lila action, or the Maya command it couldn't map. */
struct Resolved
{
    std::string action;
    std::string maya;
    bool custom = false; // the user's own MEL or Python, not a Maya command
};

Resolved resolve(const Parsed& p, const std::string& name)
{
    Resolved r;
    std::string runtime = name;
    if (auto it = p.nameCommands.find(name); it != p.nameCommands.end())
    {
        std::string cmd = trimmed(it->second.command);
        if (it->second.language != "mel" || !isIdentifier(cmd))
            return {"", cmd, true};
        runtime = cmd;
    }
    else if (auto b = builtinNameCommands().find(name); b != builtinNameCommands().end())
        runtime = b->second;
    else if (runtime.size() > 11 && runtime.ends_with("NameCommand"))
        runtime.resize(runtime.size() - 11);
    // A user runtime command that only calls another command maps like that command.
    for (int depth = 0; depth < 4; ++depth)
    {
        auto it = p.runTimeCommands.find(runtime);
        if (it == p.runTimeCommands.end())
            break;
        std::string cmd = trimmed(it->second.command);
        if (it->second.language != "mel" || !isIdentifier(cmd))
            return {"", cmd, true};
        runtime = cmd;
    }
    return {actionFor(runtime), runtime, false};
}

std::string keyLabel(const MayaHotkey& h)
{
    if (auto chord = chordFor(h))
        return hotkeys::toText(*chord);
    std::string s;
    if (h.ctrl || h.command)
        s += "Ctrl+";
    if (h.alt)
        s += "Alt+";
    if (h.shift)
        s += "Shift+";
    return s + h.key;
}

std::string readFile(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

void parse(const std::string& mel, Parsed& into)
{
    for (const auto& tokens : statements(mel))
    {
        Statement st = read(tokens);
        if (st.command == "hotkey")
        {
            auto key = st.flags.find("keyShortcut");
            if (key == st.flags.end() || key->second.empty())
                continue;
            MayaHotkey h;
            h.key = key->second;
            h.ctrl = flagOn(st, "ctrlModifier");
            h.alt = flagOn(st, "altModifier");
            h.shift = flagOn(st, "shiftModifier");
            h.command = flagOn(st, "commandModifier");
            h.hasPress = st.flags.contains("name");
            h.hasRelease = st.flags.contains("releaseName");
            h.press = h.hasPress ? st.flags["name"] : "";
            h.release = h.hasRelease ? st.flags["releaseName"] : "";
            h.editor = st.flags.contains("ctxClient") ? st.flags["ctxClient"] : "";
            into.hotkeys.push_back(std::move(h));
        }
        else if ((st.command == "nameCommand" || st.command == "runTimeCommand") && !st.object.empty() &&
                 st.flags.contains("command"))
        {
            MayaCommand c;
            c.command = st.flags["command"];
            std::string lang = st.flags.contains("sourceType")       ? st.flags["sourceType"]
                               : st.flags.contains("commandLanguage") ? st.flags["commandLanguage"]
                                                                      : "mel";
            c.language = lang.empty() ? "mel" : lang;
            auto& table = st.command == "nameCommand" ? into.nameCommands : into.runTimeCommands;
            table[st.object] = std::move(c);
        }
    }
}

std::optional<QKeyCombination> chordFor(const MayaHotkey& h)
{
    static const std::unordered_map<std::string, int> named = {
        {"up", Qt::Key_Up},         {"down", Qt::Key_Down},         {"left", Qt::Key_Left},
        {"right", Qt::Key_Right},   {"home", Qt::Key_Home},         {"end", Qt::Key_End},
        {"page_up", Qt::Key_PageUp}, {"page_down", Qt::Key_PageDown}, {"insert", Qt::Key_Insert},
        {"delete", Qt::Key_Delete}, {"backspace", Qt::Key_Backspace}, {"return", Qt::Key_Return},
        {"enter", Qt::Key_Enter},   {"tab", Qt::Key_Tab},           {"space", Qt::Key_Space},
        {"esc", Qt::Key_Escape},    {"escape", Qt::Key_Escape},
    };
    int mods = 0;
    if (h.ctrl || h.command)
        mods |= Qt::ControlModifier;
    if (h.alt)
        mods |= Qt::AltModifier;
    if (h.shift)
        mods |= Qt::ShiftModifier;

    int key = 0;
    if (h.key.size() == 1)
    {
        unsigned char c = static_cast<unsigned char>(h.key[0]);
        if (std::isalpha(c))
        {
            // Maya spells Shift+W as "W".
            if (std::isupper(c))
                mods |= Qt::ShiftModifier;
            key = std::toupper(c);
        }
        else if (c == ' ')
            key = Qt::Key_Space;
        else if (c > ' ' && c < 0x7f)
            key = c; // Qt's key codes for printable ASCII are the characters
    }
    else
    {
        std::string lower = h.key;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (auto it = named.find(lower); it != named.end())
            key = it->second;
        else if (lower.size() <= 3 && lower[0] == 'f' && std::isdigit(static_cast<unsigned char>(lower[1])))
        {
            int f = std::atoi(lower.c_str() + 1);
            if (f >= 1 && f <= 12)
                key = Qt::Key_F1 + f - 1;
        }
    }
    if (!key)
        return std::nullopt;
    std::tie(mods, key) = hotkeys::normalizeShifted(mods, key);
    return QKeyCombination(Qt::KeyboardModifiers(mods), Qt::Key(key));
}

std::string actionFor(const std::string& runtimeCommand)
{
    auto it = actionTable().find(runtimeCommand);
    return it == actionTable().end() ? std::string() : it->second;
}

Plan plan(const Parsed& parsed)
{
    Plan out;
    for (const MayaHotkey& h : parsed.hotkeys)
    {
        std::string label = keyLabel(h);
        auto chord = chordFor(h);
        if (!chord)
        {
            out.skipped.push_back({label, h.press, "key Lila can't bind"});
            continue;
        }
        std::string context = contextFor(h.editor);
        if (context.empty())
        {
            out.skipped.push_back({label, h.press, "Maya editor " + h.editor});
            continue;
        }
        if (h.hasPress && h.press.empty() && h.release.empty())
        {
            out.assignments.push_back({context, *chord, hotkeys::Slot::Press, json()});
            out.assignments.push_back({context, *chord, hotkeys::Slot::Release, json()});
            continue;
        }
        auto add = [&](const std::string& name, hotkeys::Slot slot) {
            if (name.empty())
                return;
            Resolved r = resolve(parsed, name);
            // Lila's hotbox shows while the key is held, so a tap can still do something else.
            if (r.maya == "ShowHotbox")
                slot = slot == hotkeys::Slot::Press ? hotkeys::Slot::Hold : slot;
            if (!r.action.empty() && !(slot == hotkeys::Slot::Release && r.maya == "ShowHotbox"))
                out.assignments.push_back({context, *chord, slot, json(inContext(context, r.action))});
            // A press's release half (closing a marking menu, ending a hold) is reported with its press, not again.
            else if (slot != hotkeys::Slot::Release || h.press.empty())
                out.skipped.push_back({label, r.maya, r.custom ? "custom script" : "no Lila equivalent"});
        };
        add(h.press, hotkeys::Slot::Press);
        add(h.release, hotkeys::Slot::Release);
    }
    return out;
}

Result importFile(const std::filesystem::path& file, const std::string& keymap)
{
    Parsed parsed;
    std::filesystem::path dir = file.parent_path();
    for (const auto& folder : {dir.parent_path(), dir})
        for (const char* name : {"userRunTimeCommands.mel", "userNamedCommands.mel"})
        {
            std::error_code ec;
            std::filesystem::path p = folder / name;
            if (!folder.empty() && std::filesystem::is_regular_file(p, ec) && p != file)
                parse(readFile(p), parsed);
        }
    parse(readFile(file), parsed);
    Plan p = plan(parsed);

    Result result;
    result.skipped = std::move(p.skipped);
    result.keymap = keymaps::uniqueName(keymap);
    keymaps::create(result.keymap, true);
    hotkeys::Snapshot snap = hotkeys::snapshot();
    std::vector<hotkeys::Assignment> apply;
    for (hotkeys::Assignment a : p.assignments)
    {
        if (a.target.is_string() && !actions::find(a.target.get<std::string>()))
        {
            result.skipped.push_back({hotkeys::toText(a.chord), a.target.get<std::string>(), "action not loaded"});
            continue;
        }
        // A key Lila binds only in the viewport (W, F, 4) is rebound there, where the global one would be shadowed.
        if (a.context == "global" && !hotkeys::find(snap, "global", a.chord) && hotkeys::find(snap, "viewport", a.chord))
            a.context = "viewport";
        // Maya's unbinding of a key Lila leaves free needs no entry.
        if (a.target.is_null() && !hotkeys::find(snap, a.context, a.chord))
            continue;
        apply.push_back(a);
        if (a.slot != hotkeys::Slot::Release && !a.target.is_null())
            ++result.imported;
    }
    hotkeys::assignAll(apply);
    return result;
}

std::filesystem::path defaultFolder()
{
    std::filesystem::path maya =
        std::filesystem::path(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation).toStdWString()) / "maya";
    std::error_code ec;
    std::filesystem::path best;
    for (const auto& entry : std::filesystem::directory_iterator(maya, ec))
    {
        std::filesystem::path hotkeys = entry.path() / "prefs" / "hotkeys";
        // Version folders ("2024", "2025") sort newest last.
        if (std::filesystem::is_directory(hotkeys, ec) && (best.empty() || entry.path().filename() > best.parent_path().parent_path().filename()))
            best = hotkeys;
    }
    return best;
}

void registerCommands()
{
    ops::registerCommand({
        .name = "importMayaHotkeys",
        .doc = "Imports a Maya hotkey set (.mhk) or userHotkeys.mel into a new keymap on Lila's defaults and switches to it. "
               "Returns {keymap, imported, skipped: [{key, command, reason}]}.",
        .positionalDoc = "file",
        .flags = {
            {"file", "f", ArgType::String, nullptr, "Maya hotkey file"},
            {"keymap", "k", ArgType::String, "Maya", "Name for the new keymap; a number is added when it's taken."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string file = a.get<std::string>("file", "");
            if (file.empty() && !a.positional.empty() && a.positional[0].is_string())
                file = a.positional[0].get<std::string>();
            std::filesystem::path path(std::u8string(file.begin(), file.end()));
            std::error_code ec;
            if (file.empty() || !std::filesystem::is_regular_file(path, ec))
                throw CommandError("importMayaHotkeys: no file \"" + file + "\"");
            Result r;
            try
            {
                r = importFile(path, a.get<std::string>("keymap", "Maya"));
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(std::string("importMayaHotkeys: ") + e.what());
            }
            json skipped = json::array();
            for (const Skipped& s : r.skipped)
                skipped.push_back({{"key", s.key}, {"command", s.command}, {"reason", s.reason}});
            return {{"keymap", r.keymap}, {"imported", r.imported}, {"skipped", skipped}};
        },
    });
}
} // namespace lila::ui::mayaHotkeys
