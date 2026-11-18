#include "app/Host.h"

#include "ops/Command.h"
#include "bake/Bake.h"
#include "bake/BakePipeline.h"
#include "batch/BatchCommands.h"
#include "batch/BatchRunner.h"
#include "blend/Blend.h"
#include "clay/ClayNodes.h"
#include "layout/LayoutNode.h"
#include "groom/GroomNodes.h"
#include "core/CrashUpload.h"
#include "core/Events.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Version.h"
#include "core/Progress.h"
#include "core/Workspace.h"
#include "extensions/ExtensionManager.h"
#include "fbx/Fbx.h"
#include "collada/Collada.h"
#include "gltf/Gltf.h"
#include "gn/Gn.h"
#include "io/FileTranslators.h"
#include "io/FileTypes.h"
#include "ops/StudioRestrictions.h"
#include "io/Examples.h"
#include "io/ExportOnSave.h"
#include "io/SceneTemplate.h"
#include "io/Samples.h"
#include "io/Recovery.h"
#include "io/SceneFile.h"
#include "link/Exporters.h"
#include "link/UnrealLink.h"
#include "math/MathCore.h"
#include "maya/Maya.h"
#include "python/Python.h"
#include "curve/CurveNodes.h"
#include "retopo/RetopoCommands.h"
#include "retarget/SceneRetarget.h"
#include "motion/Scene.h"
#include "rigmath/MathNodes.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "brush/Presets.h"
#include "sculpt/SculptSession.h"
#include "stream/ChangeStream.h"
#include "seq/MeshSequence.h"
#include "tex/GraphFiles.h"
#include "tex/Interop.h"
#include "tex/Library.h"
#include "tex/TextureSet.h"
#include "ui/Layouts.h"
#include "ui/ThemeModel.h"
#include "uv/UvCommands.h"
#include "vcolor/VertexColors.h"
#include "vcs/SourceControl.h"
#include "vcs/VcsCommands.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lila::host
{
namespace
{
bool stderrIsConsole()
{
#ifdef _WIN32
    return _isatty(_fileno(stderr));
#else
    return isatty(fileno(stderr));
#endif
}

/** lilabatch on a console: long operations as one line on stderr, rewritten in place. */
void installConsoleProgress()
{
    if (!stderrIsConsole())
        return;
    progress::setReporter(
        [](const std::vector<progress::Snapshot>& roots) {
            static size_t shown = 0;
            std::vector<progress::Snapshot> visible;
            for (const progress::Snapshot& s : roots)
                if (s.elapsed >= 0.3)
                    visible.push_back(s);
            const std::string line = progress::consoleLine(visible);
            if (line.empty() && shown == 0)
                return;
            // Pad over the longer previous line, then park the cursor at the end of this one.
            std::string out = "\r" + line;
            if (line.size() < shown)
                out += std::string(shown - line.size(), ' ') + "\r" + line;
            shown = line.size();
            std::fputs(out.c_str(), stderr);
            std::fflush(stderr);
        },
        150);
}

/** --export: opens the scene, exports it and says what went wrong in one line when it fails. The exit code. */
int runExport(const Options& o)
{
    const fs::path scene = fs::absolute(paths::fromUtf8(o.exportScene));
    const fs::path out = fs::absolute(paths::fromUtf8(o.exportOut));
    auto fail = [](const std::string& why) {
        log::error("lilabatch --export: {}", why);
        return 1;
    };
    if (!fs::is_regular_file(scene))
        return fail("no scene at " + paths::toUtf8(scene));
    json options = json::object();
    if (!o.exportArgs.empty())
    {
        python::Result r = python::evalJson("dict(" + o.exportArgs + ")");
        if (!r.ok)
            return fail("--export-args \"" + o.exportArgs + "\" isn't valid Python keyword arguments: " + r.value);
        options = json::parse(r.value);
    }
    try
    {
        if (auto ws = Workspace::detect(scene))
            Workspace::get().open(*ws);
        io::translators::run(io::translators::Op::Open, scene);
        const auto started = std::chrono::steady_clock::now();
        io::exportScene(out, options);
        log::info("Exported {} to {} in {:.1f} s", paths::toUtf8(scene.filename()), paths::toUtf8(out),
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    }
    catch (const std::exception& e)
    {
        return fail(e.what());
    }
    return 0;
}
} // namespace

Options parse(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--batch-run")
        {
            o.batch = true;
            o.batchRunCli = true;
            o.batchRun.assign(argv + i + 1, argv + argc);
            break;
        }
        if (a == "--batch-run-worker" && i + 2 < argc)
        {
            o.batch = true;
            o.workerJob = argv[i + 1];
            o.workerIndex = size_t(std::strtoull(argv[i + 2], nullptr, 10));
            break;
        }
        if (a == "--batch" || a == "-b")
        {
            o.batch = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                o.batchScript = argv[++i];
                o.scriptArgs.assign(argv + i + 1, argv + argc);
                break;
            }
        }
        else if (a == "--version")
            o.printVersion = true;
        else if (a == "-h" || a == "--help" || a == "/?")
            o.printHelp = true;
        else if (a == "-q" || a == "--quiet")
            o.quiet = true;
        else if (a == "--export" || a == "--convert")
        {
            o.batch = true;
            if (i + 2 >= argc)
            {
                o.error = a + " takes a scene and an output file: " + a + " <scene> <out>";
                break;
            }
            o.exportScene = argv[++i];
            o.exportOut = argv[++i];
        }
        else if (a == "--export-args")
        {
            if (i + 1 >= argc)
            {
                o.error = "--export-args takes the export flags as Python keyword arguments, e.g. --export-args \"units='m'\"";
                break;
            }
            o.exportArgs = argv[++i];
        }
        else if (a == "--from-setup")
            o.fromSetup = true;
        else if ((a == "-c" || a == "--command") && i + 1 < argc)
            o.command = argv[++i];
        else if (a == "--change-stream")
        {
            o.changeStreamPort = stream::kDefaultPort;
            if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0])))
                o.changeStreamPort = std::atoi(argv[++i]);
        }
        else if (a.starts_with("--change-stream="))
            o.changeStreamPort = std::atoi(a.c_str() + 16);
        else if (a[0] != '-')
            o.path = a;
    }
    return o;
}

std::string usage()
{
    return "Usage: lilabatch [options] [scene | workspace folder]\n"
           "\n"
           "Runs Lila without a window: opens the scene or workspace given, runs what the options ask, then exits.\n"
           "\n"
           "Options:\n"
           "  -h, --help                        Show this help and exit.\n"
           "  --version                         Print the version and exit.\n"
           "  -b, --batch <script> [args...]    Run a Python script; the arguments after it are its sys.argv[1:].\n"
           "  -c, --command <python>            Run Python code, e.g. -c \"print(ls())\".\n"
           "  --export <scene> <out>            Open the scene and export all of it to out, in the format its extension\n"
           "                                    names (.fbx, .glb, .gltf, .obj, .dae, .stl, .ply, ...). Alias: --convert.\n"
           "  --export-args \"<kwargs>\"          Flags for that format's export command as Python keyword arguments,\n"
           "                                    e.g. \"profile='unity', units='m'\" for exportFbx.\n"
           "  -q, --quiet                       Print only warnings and errors (LILA_LOG_LEVEL=warning does the same).\n"
           "  --batch-run <inputs...> [flags]   Run a script over many files in parallel workers; --batch-run --help\n"
           "                                    lists its flags. Takes the rest of the command line.\n"
           "  --change-stream [port]            Serve the change stream on 127.0.0.1 while the batch runs.\n"
           "\n"
           "Exit code: 0 on success, 1 when a command, script or export failed, 2 for a bad command line or no Python.\n"
           "\n"
           "Examples:\n"
           "  lilabatch --quiet --export Ship.lila Ship.glb\n"
           "  lilabatch --export Ship.lila Ship.fbx --export-args \"profile='unity', units='m', textures=True\"\n";
}

void startCore()
{
    log::info("Lila {}", version::full());
    mainq::init();
    PrefStore::get().start();
    auto syncUpAxis = [] {
        bool z = PrefStore::get().value<std::string>("general", "/scene/upAxis", "y") == "z";
        math::setUpAxis(z ? math::UpAxis::Z : math::UpAxis::Y);
    };
    syncUpAxis();
    PrefStore::get().onChanged([syncUpAxis](const std::string& file) {
        if (file == "general")
            syncUpAxis();
    });
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    gn::registerTypes();
    curve::registerTypes();
    tex::registerTypes();
    clay::registerTypes();
    layout::registerTypes();
    groom::registerTypes();
    rigmath::registerTypes();
    scene::registerCommands();
    io::registerCommands();
    io::exportOnSave::registerCommands();
    io::recovery::registerCommands();
    io::examples::registerCommands();
    io::sceneTemplate::registerCommands();
    io::samples::registerCommands();
    gn::registerCommands();
    seq::registerCommands();
    bake::registerCommands();
    tex::registerCommands();
    tex::registerFileCommands();
    tex::library::registerCommands();
    uv::registerCommands();
    retopo::registerCommands();
    curve::registerCommands();
    clay::registerCommands();
    layout::registerCommands();
    groom::registerCommands();
    sculpt::registerCommands();
    brush::presets::registerCommands();
    vcolor::registerCommands();
    vcs::registerCommands();
    extensions::registerCommands();
    link::registerCommands();
    stream::registerCommands();
    fbx::registerCommands();
    gltf::registerCommands();
    collada::registerCommands();
    maya::registerCommands();
    blend::registerCommands();
    batch::registerCommands();
    io::translators::registerBuiltins();
    io::translators::registerCommands();
    io::fileTypes::registerCommands();
    restrictions::registerBuiltins();
    retarget::registerCommands();
    motion::registerCommands();
    rigmath::registerCommands();
    ui::layouts::registerCommands();
    ui::themes::registerCommands();
    link::registerExporter("fbx", [](const std::vector<scene::NodeId>& nodes, const fs::path& path, std::string* err) {
        return fbx::exportForUnreal(nodes, path, "ue5", err);
    });
    for (std::string kind : {"skeletal", "animation"})
        link::registerExporter("fbx:" + kind, [kind](const std::vector<scene::NodeId>& nodes, const fs::path& path, std::string* err) {
            return fbx::exportForUnreal(nodes, path, "ue5", err, kind);
        });
    for (std::string kind : {"skeletal", "animation"})
        link::registerTakeExporter("fbx:" + kind, [kind](const std::vector<scene::NodeId>& nodes, const fs::path& path,
                                                         const std::string& take, std::string* err) {
            return fbx::exportForUnreal(nodes, path, "ue5", err, kind, true, take);
        });
    // An instance source alone at the origin, without its children: the asset its placements reference.
    link::registerExporter("fbx:instanceSource", [](const std::vector<scene::NodeId>& nodes, const fs::path& path, std::string* err) {
        return fbx::exportForUnreal(nodes, path, "ue5", err, "static", true, {}, false);
    });
    for (std::string kind : {"static", "skeletal", "animation"})
        link::registerExporter("fbx:set:" + kind, [kind](const std::vector<scene::NodeId>& nodes, const fs::path& path, std::string* err) {
            return fbx::exportForUnreal(nodes, path, "ue5", err, kind, false);
        });
    // Rebakes first, so a mapped mesh's sync to Unreal picks up fresh textures.
    io::onSaved([](const fs::path&) { bake::onSceneSaved(); });
    io::onSaved([](const fs::path& p) { tex::onSceneSaved(p); });
    io::onSaved([](const fs::path&) { link::onSceneSaved(); });
    io::onSaved([](const fs::path& p) { io::exportOnSave::onSceneSaved(p); });
}

fs::path openStartupWorkspace(const Options& o)
{
    if (!o.path.empty())
    {
        fs::path p = fs::absolute(o.path);
        if (fs::is_directory(p))
            Workspace::get().open(p);
        else if (auto ws = Workspace::detect(p))
            Workspace::get().open(*ws);
        const bool scene = io::translators::forPath(io::translators::Op::Open, p) != nullptr;
        if (scene && !o.batch)
            return p;
        if (scene)
        {
            try
            {
                io::translators::run(io::translators::Op::Open, p);
            }
            catch (const std::exception& e)
            {
                log::error("{}", e.what());
            }
        }
        return {};
    }
    if (o.batch)
        return {};
    if (PrefStore::get().value<bool>("general", "/reopenLastFolder", true))
    {
        json recent = PrefStore::get().value("general", "/recentFolders", json::array());
        if (recent.is_array() && !recent.empty() && recent[0].is_string() && fs::is_directory(recent[0].get<std::string>()))
            Workspace::get().open(recent[0].get<std::string>());
    }
    return {};
}

bool startPython(const Options& o)
{
    std::vector<std::string> pyArgs{o.batchScript.empty() ? "lila" : o.batchScript};
    pyArgs.insert(pyArgs.end(), o.scriptArgs.begin(), o.scriptArgs.end());
    bool ok = python::init(pyArgs);
    if (ok)
        extensions::autoload();
    crash::addSecret(PrefStore::get().value<std::string>("general", "/mcp/token", ""));
    crash::setContext("app", "mode", o.batch ? "batch" : "gui");
    return ok;
}

crash::WatchOptions crashOptions(bool gui)
{
    crash::WatchOptions o;
    o.ui = gui;
    o.registerWer = gui;
    o.keepReports = size_t(std::max(1, PrefStore::get().value<int>("general", "/crash/keepReports", 20)));
    o.fullDump = PrefStore::get().value<bool>("general", "/crash/fullDump", false);
    // Nothing is sent from lilabatch.
    if (gui)
        o.consentFile = PrefStore::get().userDir() / "telemetry" / crash::upload::kConsentFile;
    return o;
}

int runBatch(const Options& o, bool pythonOk)
{
    // A worker's crash is its run's to report, and its output is already captured to a log.
    if (!o.workerJob.empty())
    {
        int rc = batch::runWorker(o.workerJob, o.workerIndex, pythonOk);
        mainq::drain();
        return rc;
    }
    crash::startWatchdog(crashOptions(false));
    installConsoleProgress();
    if (!pythonOk)
        return 2;
    // Opt-in rather than the GUI's prefs: batch jobs run many at once, and none should open a port unasked.
    if (std::string err; !stream::startForBatch(o.changeStreamPort, &err))
        log::error("change stream: {}", err);
    int rc = 0;
    if (!o.exportOut.empty())
        rc = runExport(o);
    if (!o.command.empty() && !python::run(o.command, "<command>").ok)
        rc = 1;
    if (!o.batchScript.empty() && !python::runFile(o.batchScript).ok)
        rc = 1;
    if (o.batchRunCli)
        rc = std::max(rc, batch::runCli(o.batchRun));
    mainq::drain();
    return rc;
}

void shutdown()
{
    events::send({.type = events::Event::Shutdown});
    batch::shutdown();
    // Evaluated values can hold data an extension allocated (a terrain's heightfield in a geometry nodes cache), whose
    // deleter lives in its library; they have to go before the libraries do.
    scene::Graph::current().clear();
    extensions::ExtensionManager::get().unloadAll();
    vcs::SourceControl::get().shutdown();
    link::Link::get().shutdown();
    python::shutdown();
    PrefStore::get().stop();
    crash::shutdown();
}
} // namespace lila::host
