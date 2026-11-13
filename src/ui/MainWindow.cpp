#include "ui/SceneSettingsPanel.h"
#include "ui/ConnectPopover.h"
#include "ui/MainWindowDetail.h"

namespace lila::ui
{
using namespace mainWindowDetail;

MainWindow::MainWindow()
{
    setWindowTitle(QString("Lila ") + version::number());
    resize(1600, 900);
#ifdef _WIN32
    BOOL dark = TRUE;
    DwmSetWindowAttribute(HWND(winId()), 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
#endif
    setDockOptions(AnimatedDocks | AllowNestedDocks | AllowTabbedDocks | GroupedDragging);
    // Tabs read horizontally everywhere, above the panels they switch.
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
    setAcceptDrops(true);
    dockDragCancel::install();

    mViewports = new ViewportLayout(this);
    setCentralWidget(mViewports);
    toolBox::install(this, [this](const std::string& id) { return viewportActionState(id, mViewports->active()); });

    registerPanels();
    for (panels::Panel* p : panels::registry().allInOrder())
        createPanelDock(*p);
    arrangeDefaultDocks();
    mPanelListener = panels::registry().addListener([this](RegistryChange change, const std::string& id, const std::string&) {
        if (change == RegistryChange::Added)
        {
            if (const panels::Panel* p = panels::registry().find(id))
                createPanelDock(*p);
        }
        else
            removePanelDock(id);
    });
    setShowDockHook([this](const char* name) { revealDock(findChild<QDockWidget*>(name)); });
    setToggleDockHook([this](const char* name) { toggleDock(findChild<QDockWidget*>(name)); });
    tearoff::setDockHook([this](const QString& title, const QString& name, QWidget* content) {
        return addDock(title, name, content, Qt::RightDockWidgetArea);
    });
    setUvEditorOpener([this] { toggleDock(findChild<QDockWidget*>(kUvEditorDock)); });
    setNodeEditorShowHook([this] { revealDock(findChild<QDockWidget*>(kNodeEditorDock)); });
    QDockWidget* shelf = findChild<QDockWidget*>("shelf");

    mLayoutTimer = new QTimer(this);
    mLayoutTimer->setSingleShot(true);
    mLayoutTimer->setInterval(400);
    connect(mLayoutTimer, &QTimer::timeout, this, &MainWindow::saveLayout);

    registerActions();
    optionBox::install();
    mLayouts = new LayoutController(this, mSidebars, mViewports, [this] { resetLayout(); });
    buildMenus();
    buildStatusBar();
    restoreLayout();
    applyChromePrefs();
    modes::restore();
    sceneLayout::install();
    connect(shelf, &QDockWidget::visibilityChanged, this, [this, shelf](bool visible) {
        // Layouts and Reset Layout reopen the shelf; the pref keeps it closed.
        if (visible && !mShelfShown)
            QTimer::singleShot(0, this, [this, shelf] {
                if (!mShelfShown && shelf->isVisible())
                    mSidebars->close(shelf);
            });
    });

    io::onStateChanged([self = QPointer<MainWindow>(this)] { if (self) self->refreshTitle(); });

    mAutosaveTimer = new QTimer(this);
    connect(mAutosaveTimer, &QTimer::timeout, this, [] { io::autosave(); });
    auto applyUndoLimit = [] {
        PrefStore& p = PrefStore::get();
        UndoStack::get().setLimit(p.value<bool>("general", "/undo/infinite", true) ? 0 : size_t(std::max(1.0, p.value<double>("general", "/undo/steps", 500.0))));
        UndoStack::get().setByteBudget(size_t(std::max(0.0, p.value<double>("general", "/undo/memoryMB", 2048.0)) * 1024.0 * 1024.0));
    };
    applyUndoLimit();
    PrefStore::get().onChanged([this, applyUndoLimit](const std::string& file) {
        if (file != "general")
            return;
        QMetaObject::invokeMethod(qApp, applyUndoLimit);
        QMetaObject::invokeMethod(this, [this] { applyChromePrefs(); });
    });
    auto autosaveInterval = [] { return int(std::max(5.0, PrefStore::get().value<double>("general", "/autosave/intervalSeconds", 30.0)) * 1000); };
    mAutosaveTimer->start(autosaveInterval());
    PrefStore::get().onChanged([this, autosaveInterval](const std::string& file) {
        if (file == "general" && mAutosaveTimer->interval() != autosaveInterval())
            QMetaObject::invokeMethod(this, [this, autosaveInterval] { mAutosaveTimer->start(autosaveInterval()); });
    });

    Workspace::get().onChanged([this] {
        refreshWorkspaceLabel();
        refreshRecentFolders();
    });
}

MainWindow::~MainWindow()
{
    panels::registry().removeListener(mPanelListener);
    // Main-thread tasks queued by extensions can still run after the window is gone.
    viewportRenderers::setActivePanel(nullptr);
    ops::unregisterCommand("lookThru");
}

QDockWidget* MainWindow::addDock(const QString& title, const QString& objectName, QWidget* content, Qt::DockWidgetArea area)
{
    auto* dock = new QDockWidget(title, this);
    dock->setObjectName(objectName);
    dock->setWidget(content);
    addDockWidget(area, dock);
    if (!mSidebars)
    {
        mSidebars = new DockSidebars(this);
        connect(mSidebars, &DockSidebars::changed, this, &MainWindow::scheduleLayoutSave);
    }
    mSidebars->manage(dock);
    connect(dock, &QDockWidget::dockLocationChanged, this, &MainWindow::scheduleLayoutSave);
    connect(dock, &QDockWidget::topLevelChanged, this, &MainWindow::scheduleLayoutSave);
    connect(dock, &QDockWidget::visibilityChanged, this, &MainWindow::scheduleLayoutSave);
    connect(dock, &QDockWidget::visibilityChanged, this, [this, dock](bool visible) {
        QDockWidget* channels = findChild<QDockWidget*>(kChannelBoxDock);
        if (visible && channels && dock != channels && tabifiedDockWidgets(channels).contains(dock))
            mTabBeforeChannelBox = dock;
    });
    return dock;
}

void MainWindow::registerPanels()
{
    using panels::Panel;
    auto dock = [this](const char* name) { return findChild<QDockWidget*>(name); };
    auto make = [](auto factory) { return std::function<QWidget*(QWidget*)>(factory); };

    panels::add({.id = "shelf", .title = "Shelf", .create = make([](QWidget* p) { return new ShelfPanel(p); }), .area = Qt::TopDockWidgetArea, .icon = "apps",
                 .place = [this](QDockWidget* shelf) {
                     shelf->setFeatures(shelf->features() & ~QDockWidget::DockWidgetVerticalTitleBar);
                     QWidget* grip = ShelfPanel::dockTitleBar(shelf);
                     grip->setToolTip("Drag to move the shelf; right-click to collapse, float or close it");
                     grip->setContextMenuPolicy(Qt::CustomContextMenu);
                     connect(grip, &QWidget::customContextMenuRequested, this, [this, shelf, grip](const QPoint& at) {
                         QMenu menu;
                         menu.addAction("Collapse", this, [this, shelf] { mSidebars->collapse(shelf); });
                         menu.addAction(shelf->isFloating() ? "Dock" : "Float", shelf, [shelf] { shelf->setFloating(!shelf->isFloating()); });
                         menu.addAction("Close", shelf, &QDockWidget::close);
                         menu.exec(grip->mapToGlobal(at));
                     });
                     shelf->setTitleBarWidget(grip);
                 }});
    panels::add({.id = "outliner", .title = "Outliner", .create = make([](QWidget* p) { return new OutlinerPanel(p); }),
                 .area = Qt::LeftDockWidgetArea, .icon = "list-tree", .docs = "essentials?id=filter-the-outliner"});
    panels::add({.id = "sourceControl", .title = "Source Control", .create = make([](QWidget* p) { return new SourceControlPanel(p); }),
                 .area = Qt::LeftDockWidgetArea, .icon = "git-branch", .open = false});
    panels::add({.id = kReferenceEditorDock, .title = "Reference Editor", .create = make([](QWidget* p) { return new ReferenceEditor(p); }),
                 .area = Qt::LeftDockWidgetArea, .icon = "file-symlink", .open = false});
    panels::add({.id = kDisplayLayersDock, .title = "Layers", .create = make([](QWidget* p) { return new DisplayLayersPanel(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "stack-2"});
    registerAnimLayersPanel();
    registerSceneSettingsPanel();
    issues::registerUi();
    registerOffsetsPanel();
    registerRetargetUi();
    registerNotesPanel();
    registerRenamePanel();
    registerUnfrozenTransformsPanel();
    registerBatchRunPanel();
    registerBakerPanel();
    registerTextureGraphUi();
    registerAttributeUi();
    registerMaterialBrowserUi();
    registerTextureSetPanel();
    registerTexturePipelineUi();
    panels::add({.id = kUndoHistoryDock, .title = "Undo History", .create = make([](QWidget* p) { return new UndoHistoryPanel(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "history", .open = false});
    panels::add({.id = kQuickSelectSetsDock, .title = "Quick Select Sets", .create = make([](QWidget* p) { return new QuickSelectSetsPanel(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "list-details", .open = false});
    panels::add({.id = "scriptOutput", .title = "Script Output", .create = make([](QWidget* p) { return new ScriptPanel(p); }),
                 .area = Qt::BottomDockWidgetArea, .icon = "file-text", .open = false});
    panels::add({.id = kScriptEditorDock, .title = "Script Editor", .create = make([](QWidget* p) {
                     auto* scriptEditor = new ScriptEditor(p);
                     scriptEditor->setEditorFont(monoFont());
                     scriptEditor->setIconProvider([](const QString& name) { return icon(name); });
                     scriptEditor->setRunKeyLookup([](QKeyEvent* k) -> QString {
                         for (const char* id : {"script.run", "script.runLine", "script.runCell"})
                             if (Hotkeys::get().isKeyFor(k, id))
                                 return id;
                         return {};
                     });
                     return scriptEditor;
                 }),
                 .area = Qt::BottomDockWidgetArea, .icon = "code", .open = false, .docs = "python/"});
    panels::add({.id = kApiReferenceDock, .title = "API Reference", .create = make([](QWidget* p) { return new ApiReference(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "api", .open = false, .place = [this, dock](QDockWidget* apiDock) {
                     QDockWidget* editorDock = dock(kScriptEditorDock);
                     auto* scriptEditor = editorDock ? qobject_cast<ScriptEditor*>(editorDock->widget()) : nullptr;
                     auto* apiRef = qobject_cast<ApiReference*>(apiDock->widget());
                     if (!scriptEditor || !apiRef)
                         return;
                     // The docs site's API page when there is one, else the live reference.
                     connect(scriptEditor, &ScriptEditor::showDocs, this, [apiRef, apiDock](const QString& symbol) {
                         if (docs::openApi(symbol.toStdString()))
                             return;
                         revealDock(apiDock);
                         apiRef->showSymbol(symbol);
                     });
                     connect(scriptEditor, &ScriptEditor::apiDocsRequested, this, [] { docs::open("python/"); });
                     connect(scriptEditor, &ScriptEditor::apiReferenceRequested, this, [] { panels::show(kApiReferenceDock); });
                     connect(scriptEditor, &ScriptEditor::batchRunRequested, this, [] { actions::run("window.batchRun"); });
                     connect(apiRef, &ApiReference::insertCode, this, [scriptEditor, editorDock](const QString& code) {
                         editorDock->show();
                         editorDock->raise();
                         CodeEditor* ed = scriptEditor->currentEditor();
                         if (!ed)
                             ed = scriptEditor->newTab();
                         ed->insertPlainText(code);
                         ed->setFocus();
                     });
                 },
                 .floatFirst = true, .docs = "python/"});
    panels::add({.id = kTerminalDock, .title = "Terminal", .create = make([](QWidget* p) { return new TerminalPanel(p); }),
                 .area = Qt::BottomDockWidgetArea, .icon = "terminal-2", .open = false});
    // The UV editor opens as its own column left of the channel box.
    panels::add({.id = kUvEditorDock, .title = "UV Editor", .create = make([](QWidget* p) { return new UvEditor(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "cube-unfolded", .open = false, .place = [](QDockWidget* uv) {
                     keepOwnColumn(uv);
                     // Opened from any layout, it comes in wide enough for its toolkit's four tile columns.
                     uv->setProperty(kDefaultTabExtent, 520);
                 }});
    panels::add({.id = kChannelBoxDock, .title = "Channels", .create = make([](QWidget* p) { return new ChannelBox(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "list"});
    panels::add({.id = "attributeEditor", .title = "Attributes", .create = make([](QWidget* p) { return new AttributeEditor(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "file-description"});
    panels::add({.id = kModelingToolkitDock, .title = "Modeling", .create = make([](QWidget* p) { return new ModelingToolkit(p); }),
                 .area = Qt::LeftDockWidgetArea, .icon = "tool"});
    panels::add({.id = kCurveToolkitDock, .title = "Curves", .create = make([](QWidget* p) { return new CurveToolkit(p); }),
                 .area = Qt::LeftDockWidgetArea, .icon = "vector-bezier-2", .open = false});
    panels::add({.id = kTopologyToolkitDock, .title = "Topology", .create = make([](QWidget* p) { return new TopologyToolkit(p); }),
                 .area = Qt::LeftDockWidgetArea, .icon = "grid-dots", .open = false, .docs = "topology"});
    panels::add({.id = kLevelToolkitDock, .title = "Level", .create = make([](QWidget* p) { return new LevelToolkit(p); }),
                 .area = Qt::LeftDockWidgetArea, .icon = "map", .open = false, .toggleAction = "window.levelToolkit"});
    panels::add({.id = kModifierStackDock, .title = "Modifiers", .create = make([](QWidget* p) { return new ModifierStackWidget(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "stack-front"});
    panels::add({.id = kMaterialEditorDock, .title = "Material Editor", .create = make([](QWidget* p) { return new MaterialEditor(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "color-swatch", .open = false});
    panels::add({.id = kTimeSliderDock, .title = "Time Slider", .create = make([](QWidget* p) { return new TimeSlider(p); }),
                 .area = Qt::BottomDockWidgetArea, .icon = "timeline", .open = false, .place = [](QDockWidget* timeSlider) {
                     timeSlider->setTitleBarWidget(new QWidget(timeSlider));
                     // Fixed height, so a bottom area left with only the slider shrinks to it instead of handing it the space.
                     timeSlider->widget()->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
                 }});
    panels::add({.id = kGraphEditorDock, .title = "Graph Editor", .create = make([](QWidget* p) { return new GraphEditor(p); }),
                 .area = Qt::BottomDockWidgetArea, .icon = "chart-line", .open = false});
    panels::add({.id = kNodeEditorDock, .title = "Node Editor", .create = make([](QWidget* p) { return new NodeEditor(p); }),
                 .area = Qt::BottomDockWidgetArea, .icon = "node-editor", .open = false, .place = [this, dock](QDockWidget* nodeDock) {
                     arrangeBottomDocks(this, {dock("scriptOutput"), dock(kScriptEditorDock), dock(kTerminalDock), dock(kGraphEditorDock), nodeDock},
                                        dock(kScriptEditorDock), dock(kTimeSliderDock));
                 }, .docs = "node-editor"});
}

void MainWindow::arrangeDefaultDocks()
{
    auto dock = [this](const char* name) { return findChild<QDockWidget*>(name); };
    auto moveTo = [this](QDockWidget* d, Qt::DockWidgetArea area) {
        const bool hidden = d->isHidden();
        removeDockWidget(d);
        addDockWidget(area, d);
        d->setHidden(hidden);
    };
    // Tools on the left: the toolkits and the panels that act like one, tabbed in one column.
    QDockWidget* tools = dock(kModelingToolkitDock);
    QDockWidget* outliner = dock("outliner");
    QDockWidget* channels = dock(kChannelBoxDock);
    if (!tools || !outliner || !channels)
        return;
    moveTo(tools, Qt::LeftDockWidgetArea);
    for (const char* name : {kTopologyToolkitDock, kCurveToolkitDock, kLevelToolkitDock, "tailorToolkit", "stagehand.library", "textureSets", "baker",
                             "materialBrowser", "sourceControl", kReferenceEditorDock})
        if (QDockWidget* d = dock(name))
        {
            if (dockWidgetArea(d) != Qt::LeftDockWidgetArea)
                moveTo(d, Qt::LeftDockWidgetArea);
            tabifyDockWidget(tools, d);
        }
    // Data on the right: the outliner over the inspector, whose tabs show the selection's channels, attributes and stack.
    moveTo(outliner, Qt::RightDockWidgetArea);
    removeDockWidget(channels);
    splitDockWidget(outliner, channels, Qt::Vertical);
    channels->show();
    for (const char* name : {"attributeEditor", kModifierStackDock, kDisplayLayersDock, kMaterialEditorDock, "relationships", "animLayers",
                             kUndoHistoryDock, kQuickSelectSetsDock})
        if (QDockWidget* d = dock(name))
        {
            if (dockWidgetArea(d) != Qt::RightDockWidgetArea)
                moveTo(d, Qt::RightDockWidgetArea);
            tabifyDockWidget(channels, d);
        }
    for (const char* name : {"relationships", "animLayers"})
        if (QDockWidget* d = dock(name))
            d->hide();
    tools->raise();
    channels->raise();
    // Moving tabbed docks leaves their old tab bars behind, shown over the viewport; a state round trip drops them.
    restoreState(saveState());
}

void MainWindow::createPanelDock(const panels::Panel& p)
{
    const QString name = QString::fromStdString(p.id);
    if (!p.create || findChild<QDockWidget*>(name))
        return;
    QWidget* content = nullptr;
    try
    {
        content = p.create(this);
    }
    catch (const std::exception& e)
    {
        log::error("panel {}: {}", p.id, e.what());
    }
    if (!content)
        return;
    QDockWidget* d = addDock(QString::fromStdString(p.title), name, content, p.area);
    if (!p.icon.empty())
        d->setProperty(kPanelIconProperty, QString::fromStdString(p.icon));
    if (!p.docs.empty())
        setDockDocs(d, p.docs);
    if (p.ownColumn)
        keepOwnColumn(d);
    if (p.place)
        p.place(d);
    if (!p.open)
        d->hide();
    if (mMenus)
        mMenus->scheduleRebuild();
}

void MainWindow::removePanelDock(const std::string& id)
{
    QDockWidget* d = findChild<QDockWidget*>(QString::fromStdString(id));
    if (!d)
        return;
    if (mSidebars->isCollapsed(d))
        mSidebars->expand(d);
    removeDockWidget(d);
    // Now rather than later: an extension's panel code goes away with its library right after.
    delete d;
    if (mMenus)
        mMenus->scheduleRebuild();
}

void MainWindow::toggleChannelBox()
{
    QDockWidget* channels = findChild<QDockWidget*>(kChannelBoxDock);
    if (!channels)
        return;
    if (PrefStore::get().value<std::string>("general", "/ui/channelBoxToggle", "lastTab") == "attributeEditor")
    {
        QDockWidget* attrs = findChild<QDockWidget*>("attributeEditor");
        revealDock(showingTab(channels) && attrs ? attrs : channels);
        return;
    }
    QList<QDockWidget*> group = tabifiedDockWidgets(channels);
    if (!showingTab(channels))
    {
        for (QDockWidget* d : group)
            if (showingTab(d))
                mTabBeforeChannelBox = d;
        revealDock(channels);
        return;
    }
    QDockWidget* back = mTabBeforeChannelBox && group.contains(mTabBeforeChannelBox) ? mTabBeforeChannelBox.data() : nullptr;
    QDockWidget* attrs = findChild<QDockWidget*>("attributeEditor");
    if (!back && (group.contains(attrs) || group.isEmpty()))
        back = attrs;
    for (QDockWidget* d : group)
        if (!back && d->toggleViewAction()->isChecked())
            back = d;
    revealDock(back ? back : attrs);
}

void MainWindow::toggleUvEditor()
{
    QDockWidget* uv = findChild<QDockWidget*>(kUvEditorDock);
    if (showingTab(uv))
        mSidebars->collapse(uv);
    else
        revealDock(uv);
}

void MainWindow::run(const char* command, const json& call)
{
    try
    {
        ops::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

} // namespace lila::ui
