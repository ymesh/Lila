#include "ui/PointCloudDisplay.h"
#include "ui/AppMain.h"

#include "bake/Bake.h"
#include "bake/BakePipeline.h"
#include "bake/Maps.h"
#include "core/Profile.h"
#include "core/ImageIO.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Version.h"

#include "core/CrashHandler.h"
#include "core/Events.h"
#include "core/MainQueue.h"
#include "extensions/ExtensionManager.h"
#include "io/FileTypes.h"
#include "io/Recovery.h"
#include "io/SceneFile.h"
#include "core/Workspace.h"
#include "mcp/McpServer.h"
#include "python/Python.h"
#include "render/Baker.h"
#include "render/ClayGpu.h"
#include "render/DeformGpu.h"
#include "render/MeshMaps.h"
#include "render/PaintGpu.h"
#include "render/Renderer.h"
#include "render/TexEval.h"
#include "scene/Components.h"
#include "stream/ChangeStream.h"
#include "ui/AchievementRules.h"
#include "ui/Actions.h"
#include "ui/AdjustLastOp.h"
#include "ui/CommandPreview.h"
#include "ui/HeightfieldDisplay.h"
#include "ui/AgentCapture.h"
#include "ui/RenderOutput.h"
#include "ui/ArrayTool.h"
#include "ui/DocsUi.h"
#include "ui/MaterialThumbnails.h"
#include "ui/AgentSetup.h"
#include "ui/BakePaintTool.h"
#include "ui/SurfaceSnap.h"
#include "ui/ViewportRenderers.h"
#include "ui/TargetWeldTool.h"
#include "ui/TearOff.h"
#include "ui/Telemetry.h"
#include "ui/IssueReport.h"
#include "ui/Updater.h"
#include "ui/CurveUi.h"
#include "ui/ClayUi.h"
#include "ui/LayoutPencil.h"
#include "ui/ConformTool.h"
#include "ui/TexturePaintTool.h"
#include "ui/BakerPanel.h"
#include "ui/TexPipelineUi.h"
#include "ui/Animation.h"
#include "ui/AutoBevelUi.h"
#include "ui/SplitRingsUi.h"
#include "ui/InstancingUi.h"
#include "ui/TurntableUi.h"
#include "ui/UnrealMappingUi.h"
#include "ui/SmartJoinUi.h"
#include "ui/DebugDrawUi.h"
#include "ui/DeformUi.h"
#include "ui/Editors.h"
#include "ui/ExtensionUi.h"
#include "ui/Ghosting.h"
#include "ui/Hotkeys.h"
#include "ui/KnifeTool.h"
#include "ui/MainWindow.h"
#include "ui/Manipulator.h"
#include "ui/MaterialEditor.h"
#include "ui/Keymaps.h"
#include "ui/MayaHotkeys.h"
#include "ui/MeasureTool.h"
#include "ui/PlacePrimitive.h"
#include "ui/LevelToolkit.h"
#include "ui/ModelingToolkit.h"
#include "ui/Modes.h"
#include "ui/ModifierStack.h"
#include "ui/NodeEditor.h"
#include "ui/NodeTools.h"
#include "ui/Panels.h"
#include "ui/PoseLibrary.h"
#include "ui/ControlPicker.h"
#include "ui/RetopoDrawTool.h"
#include "ui/TopologyTool.h"
#include "ui/TopologyToolkit.h"
#include "ui/CurveToolkit.h"
#include "ui/RigViewport.h"
#include "ui/SculptPanel.h"
#include "ui/Shelf.h"
#include "ui/TexLive.h"
#include "ui/Theme.h"
#include "ui/Tools.h"
#include "ui/UiTools.h"
#include "ui/UvEditor.h"
#include "ui/VertexPaintTool.h"
#include "ui/PaintSelect.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportToast.h"
#include "ui/UserMenus.h"

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QImageReader>
#include <QImage>
#include <QTimer>

namespace fs = std::filesystem;

namespace lila::ui
{
namespace
{
std::string utf8(const fs::path& p)
{
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

/** What the crash report says Lila was doing; polled, so a crash costs nothing to describe. */
void updateCrashContext()
{
    crash::setContext("context", "scene", io::currentPath().empty() ? "untitled" : utf8(io::currentPath()));
    crash::setContext("context", "scene.modified", io::modified() ? "true" : "false");
    crash::setContext("context", "workspace", Workspace::get().isOpen() ? utf8(Workspace::get().root()) : "none");
    crash::setContext("context", "tool", tools::name(tools::current()));
    ViewportTool* vt = viewportTools::active();
    crash::setContext("context", "viewportTool", vt ? vt->name() : "none");
    crash::setContext("context", "componentMode", scene::components::modeName(scene::components::mode()));
    std::string loaded;
    for (const std::string& name : extensions::ExtensionManager::get().loaded())
        loaded += (loaded.empty() ? "" : ", ") + name;
    crash::setContext("context", "extensions", loaded.empty() ? "none" : loaded);
}

void registerUi()
{
    actions::registerBuiltins();
    tools::registerActions();
    surfaceSnap::registerCommands();
    viewportRenderers::registerCommands();
    mayaHotkeys::registerCommands();
    keymaps::registerCommands();
    registerShelfActions();
    levelToolkit::registerShelf();
    registerEditorActions();
    userMenus::install();
    registerPanelActions();
    docs::registerDocsUi();
    registerMaterialUi();
    registerNodeEditorActions();
    nodeTools::registerNodeToolUi();
    registerUvActions();
    registerModelingToolkitActions();
    registerModifierStackActions();
    adjustLastOp::install();
    commandPreview::install();
    Manipulator::installOrientPivotSink();
    registerRetopoActions();
    heightfieldDisplay::registerActions();
    registerTopologyActions();
    registerTopologyToolkitActions();
    registerCurveToolkitActions();
    registerKnifeActions();
    registerTargetWeldActions();
    registerArrayToolActions();
    registerCurveUi();
    pointCloudDisplay::registerUi();
    registerConformActions();
    registerClayUi();
    registerLayoutPencil();
    registerSculptUi();
    registerVertexPaintUi();
    registerBakePaintUi();
    registerTexturePaintUi();
    // Material textures in formats core ImageIO lacks (JPG, RGB PNG, BMP) come through Qt.
    bake::setTextureLoader([](const std::string& path, int& w, int& h, std::vector<float>& rgba, std::string& error) {
        image::Image img = image::read(paths::fromUtf8(path), &error);
        if (img.width <= 0)
        {
            QImage q(QString::fromUtf8(path.c_str()));
            if (q.isNull())
                return false;
            q.convertTo(QImage::Format_RGBA64);
            img.width = q.width();
            img.height = q.height();
            img.rgba.resize(size_t(img.width) * img.height * 4);
            for (int y = 0; y < img.height; ++y)
            {
                const auto* row = reinterpret_cast<const quint16*>(q.constScanLine(y));
                for (int i = 0; i < img.width * 4; ++i)
                    img.rgba[size_t(y) * img.width * 4 + i] = row[i] / 65535.0f;
            }
            error.clear();
        }
        w = img.width;
        h = img.height;
        rgba = std::move(img.rgba);
        return true;
    });
    paintSelect::registerPaintSelectUi();
    registerAnimationUi();
    registerGhostingUi();
    registerPoseLibraryUi();
    registerControlPickerUi();
    modes::registerBuiltins();
    extensionUi::install();
    python::uiHooks().runAction = [](const std::string& id) { return actions::run(json(id)); };
    python::uiHooks().listActions = [] {
        std::vector<std::array<std::string, 3>> out;
        for (const Action* a : actions::all())
            out.push_back({a->id, a->label, a->category});
        return out;
    };
}
} // namespace

void registerBatchCommands()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    capture::registerCommands();
    heightfieldDisplay::registerCommands();
    renderOutput::registerCommands();
    materialThumbs::registerCommands();
    // The gpu bake backend starts the renderer headless on first use.
    render::registerGpuBakeBackend();
    render::texgpu::registerTexGpu();
    render::paint::registerCommands();
    registerScreenshotCommand();
    viewportToast::registerCommands();
    modes::registerBuiltins();
    // captureView starts the renderer on demand in batch mode.
    events::subscribe(events::Event::Shutdown, [](const events::EventData&) { render::Renderer::get().stop(); });
}

int runApp(int argc, char** argv, const host::Options& opts, const fs::path& startupScene)
{
    QApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    // Qt reads QT_SCALE_FACTOR only before QApplication exists; a value set in the environment wins.
    if (qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR"))
        if (int percent = PrefStore::get().value<int>("general", kUiScalePref, 100); percent != 100 && percent >= 50 && percent <= 300)
            qputenv("QT_SCALE_FACTOR", QByteArray::number(percent / 100.0));
    QApplication app(argc, argv);
    LILA_THREAD_NAME("Main");
    QApplication::setApplicationName("Lila");
    QApplication::setApplicationVersion(version::number());
    QApplication::setOrganizationName("Lila");
    // Qt refuses images over 256 MB by default; texture maps reach 16K and beyond.
    QImageReader::setAllocationLimit(4096);
    applyTheme(app);

    mainq::setWakeup([&app] { QMetaObject::invokeMethod(&app, [] { mainq::drain(); }, Qt::QueuedConnection); });
    mainq::drain();

    registerUi();
    Hotkeys::get().install();
    render::Renderer::get().start();
    render::registerGpuBakeBackend();
    render::registerGpuDeformers();
    render::registerGpuClay();
    render::texgpu::registerTexGpu();
    render::paint::registerCommands();
    texLive::install();
    bake::setRebakeAsync(true);
    render::setLiveMapsAsync(true);
    registerUiTools();
    capture::registerMcpTools();
    registerBakerMcpTools();
    registerTexturePipelineMcpTools();
    agentSetup::registerActions();
    registerRigViewport();
    registerDeformActions();
    registerAutoBevelUi();
    registerSplitRingsUi();
    registerInstancingUi();
    registerTurntableUi();
    registerUnrealMappingUi();
    registerSmartJoinUi();
    registerMeasureUi();
    registerDebugDrawUi();
    registerPlacePrimitiveUi();
    tearoff::registerDropHandler();
    mcp::Server::get().holdUntilReady();
    mcp::startFromPrefs();
    stream::startFromPrefs();
    crash::addSecret(mcp::Server::get().token());
    crash::setContext("app", "qt", qVersion());
    crash::startWatchdog(host::crashOptions(true));
    telemetry::start();
    achievementRules::start();
    updateCrashContext();
    QTimer crashContext;
    QObject::connect(&crashContext, &QTimer::timeout, [] { updateCrashContext(); });
    crashContext.start(1000);
    // The event loop is about to wait: AppIdle, only while someone listens.
    QObject::connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock, [] {
        if (events::hasSubscribers(events::Event::AppIdle))
            events::send({.type = events::Event::AppIdle});
    });

    int rc = 0;
    // Read before our own session file exists, so it is never mistaken for a crashed one.
    std::vector<io::recovery::Session> stale = io::recovery::staleSessions(io::recovery::sessionsDir(), io::recovery::currentPid());
    io::recovery::beginSession();
    io::recovery::installCrashSave();
    {
        MainWindow window;
        window.show();
        // One callback, in order: a -c command must not run inside the scene load's event pumping.
        QTimer::singleShot(0, &window, [&window, &opts, stale, startupScene] {
            // lila.ops was built at Python boot, before the UI registered its own commands.
            python::run("__import__('lila.ops', fromlist=['refresh']).refresh()", "<startup>");
            window.startup(stale, io::currentPath().empty() && startupScene.empty());
            if (!startupScene.empty())
                window.openFile(startupScene);
            if (!opts.command.empty())
                python::run(opts.command, "<command>");
            mcp::Server::get().setReady();
        });
        QTimer::singleShot(3000, &window, [] { docs::startAutoUpdate(); });
        QTimer::singleShot(2000, &window, [] { io::fileTypes::applyPrefs(); });
        PrefStore::get().onChanged([](const std::string& file) {
            if (file == "general")
                io::fileTypes::applyPrefs();
        });
        updater::startAutoCheck(&window);
        issueReport::startup(&window);
        rc = app.exec();
        // Tearing the window down resizes and hides viewports, and a swapchain rebuilt for one of those crashes some drivers.
        render::Renderer::get().removeAllViewports();
        docs::shutdown();
    }
    // A bake still running on the GPU must stop before the renderer does.
    bake::shutdownRebakes();
    bake::cancelAsyncBake(true);
    telemetry::stop();
    achievementRules::stop();
    updater::installPendingOnExit();
    io::recovery::endSession();

    mcp::Server::get().stop();
    stream::stopServer();
    render::Renderer::get().stop();
    mainq::setWakeup(nullptr);
    return rc;
}
} // namespace lila::ui

void lilaUiRegisterBatchCommands()
{
    lila::ui::registerBatchCommands();
}
