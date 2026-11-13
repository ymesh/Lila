# Layout and theme files and commands, without Qt, so the tests and batch mode can use them.
lila_module(lila_layouts Scene Layouts.cpp Layouts.h ThemeModel.cpp ThemeModel.h)
target_link_libraries(lila_layouts PUBLIC lila_ops)

lila_module(lila_ui UI
    AchievementBanner.cpp AchievementBanner.h
    AchievementRules.cpp AchievementRules.h
    Actions.cpp
    ActionIcons.cpp
    AdjustLastOp.cpp AdjustLastOp.h
    CommandPreview.cpp CommandPreview.h
    AppMain.cpp AppMain.h
    FlagFields.cpp FlagFields.h
    OptionBox.cpp OptionBox.h
    MenuSearch.cpp MenuSearch.h
    CommandPalette.cpp CommandPalette.h CommandHistory.h
    TexturePicker.cpp TexturePicker.h
    AgentCapture.cpp AgentCapture.h
    HeightfieldDisplay.cpp HeightfieldDisplay.h
    RenderOutput.cpp RenderOutput.h
    RenderOutputDialog.cpp RenderOutputDialog.h
    AgentSetup.cpp AgentSetup.h
    Animation.cpp Animation.h
    ApiReference.cpp ApiReference.h
    Docs.cpp Docs.h
    DocsUi.cpp DocsUi.h
    AttributeEditor.cpp AttributeEditor.h
    AttrAuthoring.cpp AttrAuthoring.h
    AttrView.cpp AttrView.h
    DrivePopover.cpp DrivePopover.h
    ConnectPopover.cpp ConnectPopover.h
    ComponentSheet.cpp ComponentSheet.h
    RelationshipsPanel.cpp RelationshipsPanel.h
    FileDialog.cpp FileDialog.h FilePreview.cpp FilePreview.h FileDrop.cpp FileDrop.h
    CodeEdit.cpp CodeEdit.h FontPicker.cpp FontPicker.h NodePicker.cpp NodePicker.h MultilineEdit.cpp MultilineEdit.h
    AutoBevelUi.cpp AutoBevelUi.h
    SplitRingsUi.cpp SplitRingsUi.h
    SmartJoinUi.cpp SmartJoinUi.h
    Camera.cpp
    ChannelBox.cpp ChannelBox.h
    CurveTools.cpp CurveToolkit.cpp CurveToolkit.h CurveToolsCommon.h EzCurveTool.cpp CurvePointTool.cpp CurveUi.cpp CurveUi.h PipeTool.cpp PipeLevelTools.cpp CableTool.cpp SectionTool.cpp StrapTool.cpp PlanarCurveUi.cpp
    BlendCurveTool.cpp CurvePick.cpp CurvePick.h TrimCurveTool.cpp VehicleCurveUi.cpp
    SketchTool.cpp SketchUi.cpp SketchUi.h CurveSculptTool.cpp GroomBrushTool.cpp GroomGpu.cpp GroomGpu.h GroomTool.cpp
    DisplayLayersPanel.cpp DisplayLayersPanel.h
    AnimLayersPanel.cpp AnimLayersPanel.h
    SceneSettingsPanel.cpp SceneSettingsPanel.h
    PointCloudDisplay.cpp PointCloudDisplay.h
    OffsetsPanel.cpp OffsetsPanel.h
    RenamePanel.cpp RenamePanel.h
    RetargetUi.cpp RetargetUi.h
    BatchRunPanel.cpp BatchRunPanel.h
    BakerPanel.cpp BakerPanelWidgets.cpp BakerPanelRefresh.cpp BakerPanelMcp.cpp BakerPanel.h BakerPanelInternal.h
    BakeCageView.cpp BakeCageView.h
    BakePaintTool.cpp BakePaintTool.h
    TexturePaintTool.cpp TexturePaintTool.h
    TexPipelineUi.cpp TexPipelineUi.h
    Editors.cpp Editors.h
    Environments.cpp Environments.h
    ExtensionManagerDialog.cpp ExtensionManagerDialog.h
    GameExporter.cpp GameExporter.h
    MeshSequenceExporter.cpp MeshSequenceExporter.h
    AnimatedSend.cpp AnimatedSend.h
    Ghosting.cpp Ghosting.h
    GnViewerDraw.cpp GnViewerDraw.h
    GnSpreadsheet.cpp GnSpreadsheet.h
    GraphEditor.cpp GraphEditor.h GraphEditorCommon.h
    GraphCurveView.cpp GraphCurveView.h GraphModel.cpp GraphModel.h
    GraphMotion.cpp GraphMotion.h
    PoseLibrary.cpp PoseLibrary.h
    ControlPicker.cpp ControlPicker.h
    HotkeyBindings.cpp HotkeyBindings.h
    HotkeyCheatSheet.cpp HotkeyCheatSheet.h
    HotkeyEditor.cpp HotkeyEditor.h
    Hotkeys.cpp Hotkeys.h
    Keymaps.cpp Keymaps.h
    MayaHotkeys.cpp MayaHotkeys.h
    Hotbox.cpp Hotbox.h HotboxZone.h HotkeyChord.h
    Icons.cpp Icons.h
    ImagePlaneUi.cpp ImagePlaneUi.h
    VideoTextures.cpp VideoTextures.h
    AudioUi.cpp AudioUi.h
    MediaLane.cpp MediaLane.h
    LicensesDialog.cpp LicensesDialog.h
    PaintColor.cpp PaintColor.h
    ReferenceSet.cpp ReferenceSet.h ReferenceWindow.cpp ReferenceWindow.h ReferenceOverlay.cpp ReferenceOverlay.h
    LayoutsUi.cpp LayoutsUi.h
    MarkingMenu.cpp MarkingMenu.h
    MarkingMenuEditor.cpp MarkingMenuEditor.h
    UserMenus.cpp UserMenus.h UserMenuEditor.cpp UserMenuEditor.h
    MainWindow.cpp MainWindow.h MainWindowDetail.h MainWindowFiles.cpp MainWindowActions.cpp MainWindowMenus.cpp MainWindowLayout.cpp MenuContext.cpp MenuContext.h TopBar.cpp TopBar.h Controls.cpp Controls.h ViewportHeaderConfig.cpp ViewportHeaderConfig.h BarPresets.cpp BarPresets.h
    Menus.cpp Menus.h MenuSets.cpp MenuSets.h
    Modes.cpp Modes.h SceneLayout.cpp SceneLayout.h
    ExtensionUi.cpp ExtensionUi.h
    PythonSdk.cpp
    PythonViewport.cpp
    Owner.h
    SpinScrub.cpp SpinScrub.h
    NumberExpr.cpp NumberExpr.h
    PanelFilter.cpp PanelFilter.h
    PanelDefaults.cpp PanelDefaults.h
    ValueReset.h
    NodeLookup.h
    Manipulator.cpp Manipulator.h
    Gizmo.cpp Gizmo.h ViewInfo.cpp ViewInfo.h ViewportState.h
    DrawOverrides.cpp DrawOverrides.h RenderPasses.cpp RenderPasses.h SelectionHandlers.cpp SelectionHandlers.h
    ExtrudeHandles.cpp ExtrudeHandles.h
    NodeHandles.cpp NodeHandles.h
    ColorPopup.cpp ColorPopup.h
    ImagePicker.cpp ImagePicker.h
    MaterialDraw.cpp MaterialDraw.h
    MaterialEditor.cpp MaterialEditor.h
    MeasureTool.cpp MeasureTool.h
    DebugDrawUi.cpp DebugDrawUi.h
    PlacePrimitive.cpp PlacePrimitive.h
    MeshCache.cpp MeshCache.h
    SurfaceSnap.cpp SurfaceSnap.h
    GeometrySnap.cpp GeometrySnap.h
    Isolate.cpp Isolate.h
    Issues.cpp Issues.h
    ModelingToolkit.cpp ModelingToolkit.h LevelToolkit.cpp LevelToolkit.h
    SoftSelectPanel.cpp SoftSelectPanel.h
    ModifierStack.cpp ModifierStack.h
    NodeEditor.cpp NodeEditor.h NodeCanvas.cpp NodeCanvas.h NodeCanvasLayout.cpp NodeCanvasInput.cpp NodeCanvasWires.cpp NodeCanvasTies.cpp NodeCanvasFrames.cpp NodeCanvasGrab.cpp GraphBackend.cpp GraphBackend.h GnGraphBackend.cpp
    SceneGraphBackend.cpp
    TexGraphBackend.cpp TexParams.cpp TexParams.h TexPreview.cpp TexPreview.h Texture2DView.cpp TextureGraphUi.h TextureSetPanel.cpp TextureSetPanel.h
    Overlay.cpp Overlay.h
    CursorBadges.cpp CursorBadges.h
    Panels.cpp Panels.h
    RetopoDrawTool.cpp RetopoDrawTool.h
    RetopoPolesTool.cpp RetopoPolesTool.h
    TopologyTool.cpp TopologyTool.h TopologyModes.cpp TopologyToolkit.cpp TopologyToolkit.h
    KnifeTool.cpp KnifeTool.h
    TargetWeldTool.cpp TargetWeldTool.h
    ArrayTool.cpp ArrayTool.h
    ClayTool.cpp
    ClayUi.cpp ClayUi.h
    LayoutPencil.cpp LayoutPencil.h
    ConformTool.cpp ConformTool.h
    PythonBridge.cpp PythonBridge.h
    RigViewport.cpp RigViewport.h SoftNetDraw.cpp SoftNetDraw.h WeightPaintTool.cpp WeightPaintTool.h RigHandles.cpp RigHandles.h NodeTools.cpp NodeTools.h
    SpacesUi.cpp SpacesUi.h
    InstancingUi.cpp InstancingUi.h
    TurntableUi.cpp TurntableUi.h
    DroneCamera.cpp DroneCamera.h
    UnrealMappingUi.cpp UnrealMappingUi.h
    DeformUi.cpp DeformUi.h
    DeformHandles.cpp DeformHandles.h
    Deletion.cpp Deletion.h
    MorphPanel.cpp MorphPanel.h
    SculptPanel.cpp SculptPanel.h
    SculptTool.cpp SculptTool.h
    Tools.cpp Tools.h
    Outliner.cpp Outliner.h
    OutlinerFilter.cpp OutlinerFilter.h
    TokenCompleter.cpp TokenCompleter.h
    WheelGuard.cpp WheelGuard.h
    SetsUi.cpp SetsUi.h
    NotesPanel.cpp NotesPanel.h
    Presenter.cpp Presenter.h
    PreferencesDialog.cpp PreferencesDialog.h
    Telemetry.cpp Telemetry.h
    SupportBundle.cpp SupportBundle.h
    IssueReport.cpp IssueReport.h
    PerfTrace.cpp PerfTrace.h
    Updater.cpp Updater.h
    WelcomeNews.cpp WelcomeNews.h
    Credits.cpp Credits.h
    CreditsDialog.cpp CreditsDialog.h
    ProgressOverlay.cpp ProgressOverlay.h
    ReferenceEditor.cpp ReferenceEditor.h
    RenderSettingsDialog.cpp RenderSettingsDialog.h
    ScriptPanel.cpp ScriptPanel.h
    Shelf.cpp Shelf.h
    StatusOutput.cpp StatusOutput.h OutputTail.h
    SampleScenes.cpp SampleScenes.h
    SceneDialogs.cpp SceneDialogs.h
    SourceControlPanel.cpp SourceControlPanel.h
    TearOff.cpp TearOff.h TearOffLogic.h
    Terminal.cpp Terminal.h
    TexLive.cpp TexLive.h
    MaterialBrowser.cpp MaterialBrowser.h MaterialThumbnails.cpp MaterialThumbnails.h
    TextureCache.cpp TextureCache.h
    Theme.cpp Theme.h ThemeEditor.cpp ThemeEditor.h
    TimeSlider.cpp TimeSlider.h
    ToolColors.h ToolkitLayout.h UiCommands.h UiCommon.h ViewportToolMath.h
    UiTools.cpp
    UndoHistory.cpp UndoHistory.h
    UnfrozenTransforms.cpp UnfrozenTransforms.h
    UvEditor.cpp UvEditorView.cpp UvEditorDraw.cpp UvEditorPick.cpp UvEditorEdit.cpp UvEditor.h UvEditorInternal.h
    UvTexturePaint.cpp UvTexturePaint.h
    UvTools.cpp UvTools.h
    UvLive.cpp UvLive.h
    UvPackPanel.cpp UvPackPanel.h
    UvProjectTool.cpp UvProjectTool.h
    UvSeamOverride.cpp UvSeamOverride.h
    FlowLayout.cpp FlowLayout.h
    FixedRowLayout.cpp FixedRowLayout.h
    MenuButtons.cpp MenuButtons.h
    DockSidebar.cpp DockSidebar.h
    LocateFlash.cpp LocateFlash.h
    DockDragCancel.cpp DockDragCancel.h
    DominantInfluenceUi.cpp DominantInfluenceUi.h
    AnimProxyUi.cpp AnimProxyUi.h
    ViewportHooks.cpp ViewportHooks.h
    ViewportRenderers.cpp ViewportRenderers.h
    ViewportDisplay.cpp ViewportDisplay.h
    ViewportHud.cpp ViewportHud.h
    LodDisplay.cpp LodDisplay.h
    ViewportContext.cpp ViewportContext.h
    ViewportLayout.cpp ViewportLayout.h
    ViewportToolbar.cpp ViewportToolbar.h
    ViewportHeader.cpp ViewportHeader.h
    DisplayAids.cpp DisplayAids.h
    IconBars.cpp IconBars.h Arrange.h
    ToolBox.cpp ToolBox.h
    VertexPaintTool.cpp VertexPaintTool.h
    PaintSelect.cpp PaintSelect.h
    ViewportTool.cpp ViewportTool.h ViewportToast.cpp ViewportToast.h StepHistory.h SimPreview.cpp SimPreview.h
    ViewportWidget.cpp ViewportWidget.h ViewportWidgetDetail.h ViewportMeshes.cpp ViewportComponents.cpp ViewportViews.cpp ViewportInput.cpp
    CameraBookmarks.cpp CameraBookmarks.h
    ChoiceTips.cpp ChoiceTips.h)

set_target_properties(lila_ui PROPERTIES AUTOMOC ON)
target_link_libraries(lila_ui PUBLIC lila_scene lila_io lila_python lila_mcp lila_render lila_editor lila_vcs Qt6::Widgets)
target_link_libraries(lila_ui PUBLIC lila_term)
target_link_libraries(lila_ui PUBLIC lila_layouts)
target_link_libraries(lila_ui PUBLIC lila_uv_cmds)
target_link_libraries(lila_ui PUBLIC lila_gn)
target_link_libraries(lila_ui PUBLIC lila_tex)
target_link_libraries(lila_ui PUBLIC lila_retopo_cmds)
target_link_libraries(lila_ui PUBLIC lila_retarget)
target_link_libraries(lila_ui PUBLIC lila_sculpt_cmds)
target_link_libraries(lila_ui PUBLIC lila_vcolor)
target_link_libraries(lila_ui PRIVATE Python::Python Qt6::Svg)
find_package(miniz CONFIG REQUIRED)
target_link_libraries(lila_ui PRIVATE Qt6::Network miniz::miniz)
