#include "Isolate.h"

#include "ops/Command.h"
#include "core/Events.h"
#include "extensions/ExtensionManager.h"
#include "extensions/Library.h"
#include "python/Python.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "stream/ChangeStream.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <thread>

using namespace lila;
using lila::extensions::ExtensionManager;

TEST_CASE("native extension: load, use, unload, reload")
{
    isolate();
    extensions::registerCommands();
    ExtensionManager& pm = ExtensionManager::get();

    std::vector<std::string> heard;
    events::SubscriptionId sub = events::subscribe(events::Event::ExtensionLoaded, [&](const events::EventData& e) { heard.push_back(e.name); });

    CHECK(run("loadExtension", {{"args", {LILA_TEST_HELLO_PACKAGE}}}) == "hello");
    CHECK(pm.isLoaded("hello"));
    CHECK(heard == std::vector<std::string>{"hello"});
    events::unsubscribe(sub);
    const CommandDef* cmd = ops::find("helloExtension");
    REQUIRE(cmd);
    CHECK(cmd->owner == "hello");
    CHECK(run("helloExtension", {{"args", {"Bob"}}, {"loud", true}}) == "HELLO, BOB");

    const scene::NodeType* type = scene::types::find("helloMultiply");
    REQUIRE(type);
    CHECK(type->owner == "hello");

    json info = run("extensionInfo", {{"args", {"hello"}}});
    CHECK(info["kind"] == "native");
    CHECK(info["version"] == "1.0.0");
    CHECK(info["vendor"] == "Lila");
    CHECK(info["sdk"] == "1.0");
    CHECK(info["commands"] == json::array({"helloExtension"}));
    CHECK(info["types"] == json::array({"helloMultiply"}));
    CHECK(info["registrations"] == json::array({"command:helloExtension", "nodeType:helloMultiply"}));

    // compute() runs code from the library.
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.createNode(*type, "mult");
    REQUIRE(n);
    std::string name = n->name();
    g.setValue(g.plug(name + ".input"), 3.0);
    CHECK(std::get<double>(g.value(g.plug(name + ".output"))) == doctest::Approx(6.0));

    CHECK_THROWS_WITH_AS(run("unloadExtension", {{"args", {"hello"}}}), doctest::Contains("node(s) of its types"), CommandError);
    g.deleteNode(n->id());

    run("unloadExtension", {{"args", {"hello"}}});
    CHECK_FALSE(pm.isLoaded("hello"));
    CHECK_FALSE(ops::find("helloExtension"));
    CHECK_FALSE(scene::types::find("helloMultiply"));

    // Loading by id through a search path, then reload.
    pm.addSearchPath(std::filesystem::path(LILA_TEST_HELLO_PACKAGE).parent_path());
    json listed = run("listExtensions");
    bool found = false;
    for (const json& p : listed)
        found |= p["name"] == "hello" && p["kind"] == "native" && !p["loaded"].get<bool>() && p["version"] == "1.0.0";
    CHECK(found);
    CHECK(run("loadExtension", {{"args", {"hello"}}}) == "hello");
    CHECK(run("loadExtension", {{"args", {"hello"}}}) == "hello");
    CHECK(run("reloadExtension", {{"args", {"hello"}}}) == "hello");
    CHECK(ops::find("helloExtension"));
    CHECK(run("listExtensions", {{"loaded", true}}).size() == 1);
    pm.unload("hello");
    CHECK_FALSE(ops::find("helloExtension"));

    CHECK_THROWS_WITH_AS(run("loadExtension", {{"args", {"noSuchExtension"}}}), doctest::Contains("no extension named"), CommandError);
    CHECK_THROWS_AS(run("unloadExtension", {{"args", {"hello"}}}), CommandError);
}

TEST_CASE("dev packages: libraries load from a copy, and the watcher sends extensionChanged and reloads")
{
    namespace fs = std::filesystem;
    using namespace std::chrono;
    isolate();
    extensions::registerCommands();
    ExtensionManager& pm = ExtensionManager::get();

    const fs::path root = fs::temp_directory_path() / ("lila_dev_package_" + std::to_string(std::random_device{}()));
    const fs::path pkg = root / "hello.lilaext";
    fs::create_directories(root);
    fs::copy(LILA_TEST_HELLO_PACKAGE, pkg, fs::copy_options::recursive);
    struct Cleanup
    {
        fs::path dir;
        ~Cleanup()
        {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    } cleanup{root};

    CHECK(pm.load(pkg.string()) == "hello");
    fs::path lib;
    for (const fs::directory_entry& e : fs::recursive_directory_iterator(pkg))
        if (e.path().extension() == extensions::Library::extension())
            lib = e.path();
    REQUIRE_FALSE(lib.empty());
    CHECK(std::ofstream(lib, std::ios::binary | std::ios::app).is_open());

    auto pump = [](auto done, milliseconds limit) {
        const auto end = steady_clock::now() + limit;
        while (!done() && steady_clock::now() < end)
        {
            mainq::drain();
            std::this_thread::sleep_for(milliseconds(20));
        }
    };
    auto touch = [&](const fs::path& p) {
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::app) << "x";
    };

    std::vector<events::EventData> changed;
    int loads = 0;
    events::SubscriptionId onChange = events::subscribe(events::Event::ExtensionChanged, [&](const events::EventData& e) { changed.push_back(e); });
    events::SubscriptionId onLoad = events::subscribe(events::Event::ExtensionLoaded, [&](const events::EventData&) { ++loads; });

    // Event only: several writes settle into one event naming them; caches are ignored.
    json watched = run("watchExtension", {{"args", {"hello"}}, {"reload", false}, {"settle", 150}});
    CHECK(watched["hello"]["reload"] == false);
    pump([] { return false; }, milliseconds(300));
    touch(pkg / "notes.txt");
    touch(pkg / "python" / "a.py");
    touch(pkg / "__pycache__" / "a.cpython.pyc");
    pump([&] { return !changed.empty(); }, seconds(5));
    pump([] { return false; }, milliseconds(300));
    REQUIRE(changed.size() == 1);
    CHECK(changed[0].name == "hello");
    CHECK(changed[0].paths.size() == 2);
    CHECK(loads == 0);
    CHECK(pm.isLoaded("hello"));

    // Reload: the change reloads it after the event.
    run("watchExtension", {{"args", {"hello"}}, {"settle", 50}});
    CHECK(run("extensionInfo", {{"args", {"hello"}}})["watch"]["reload"] == true);
    pump([] { return false; }, milliseconds(300));
    touch(pkg / "notes.txt");
    pump([&] { return loads == 1; }, seconds(5));
    CHECK(changed.size() == 2);
    CHECK(loads == 1);
    CHECK(pm.isLoaded("hello"));

    CHECK(run("watchExtension", {{"args", {"hello"}}, {"off", true}}).empty());
    events::unsubscribe(onChange);
    events::unsubscribe(onLoad);
    pm.unload("hello");
}

TEST_CASE("packages: SDK version checks and broken manifests")
{
    isolate();
    ExtensionManager& pm = ExtensionManager::get();
    pm.addSearchPath(LILA_TEST_PACKAGES_DIR);
    const size_t commands = ops::all().size();

    // Rejected by Lila before any of the extension's code runs (future's library does not even exist).
    CHECK_THROWS_WITH_AS(pm.load("future"), doctest::Contains("the major version must match"), extensions::ExtensionError);
    CHECK_FALSE(pm.isLoaded("future"));
    CHECK(pm.lastError("future").find("SDK 2.0") != std::string::npos);
    CHECK_THROWS_WITH_AS(pm.load("newer"), doctest::Contains("newer than this Lila's SDK"), extensions::ExtensionError);
    CHECK(ops::all().size() == commands);

    bool brokenListed = false;
    for (const extensions::ExtensionInfo& info : pm.available())
        if (info.name == "broken")
        {
            brokenListed = true;
            CHECK(info.error.find("'version' is missing") != std::string::npos);
            CHECK(info.error.find("'sdk' must be") != std::string::npos);
            CHECK(info.error.find("inside the package") != std::string::npos);
        }
    CHECK(brokenListed);
    CHECK_THROWS_WITH_AS(pm.load("broken"), doctest::Contains("'version' is missing"), extensions::ExtensionError);

    // Autoload skips what it can't load and says so instead of failing startup.
    const std::vector<std::string> loadedBefore = pm.loaded();
    pm.setAutoload("future", true);
    pm.loadAutoload();
    CHECK_FALSE(pm.isLoaded("future"));
    pm.setAutoload("future", false);
    // Extensions shipped next to the test (enabled by default) loaded too; later tests expect only their own.
    for (const std::string& name : pm.loaded())
        if (std::find(loadedBefore.begin(), loadedBefore.end(), name) == loadedBefore.end())
            pm.unload(name);
}

TEST_CASE("python extension: load, call, errors, unload")
{
    isolate();
    REQUIRE(python::init({"lila_extensions_tests"}));

    ExtensionManager& pm = ExtensionManager::get();
    pm.addSearchPath(LILA_TEST_EXAMPLES_DIR);

    bool listed = false;
    for (const extensions::ExtensionInfo& p : pm.available())
        listed |= p.name == "hello_py" && p.kind == extensions::ExtensionKind::Python;
    CHECK(listed);

    CHECK(pm.load("hello_py") == "hello_py");
    const CommandDef* cmd = ops::find("helloPy");
    REQUIRE(cmd);
    CHECK(cmd->owner == "hello_py");
    CHECK(run("helloPy", {{"args", {"world"}}, {"loud", true}}) == "HELLO, WORLD");
    CHECK(run("helloPy", {{"args", {"a", "b"}}}) == "Hello, a, b");
    CHECK_THROWS_WITH_AS(run("helloPy"), doctest::Contains("give someone to greet"), CommandError);

    // Visible to scripts, as lila.ops is refreshed on load.
    python::Result r = python::run("lila.ops.helloPy('x')");
    CHECK(r.ok);
    CHECK(r.value == "'Hello, x'");

    pm.unload("hello_py");
    CHECK_FALSE(ops::find("helloPy"));
    CHECK(python::run("assert not hasattr(lila.ops, 'helloPy')").ok);

    // Direct registration, a dict flag with a short name, and a non-CommandError exception.
    REQUIRE(python::run("from lila import extension\n"
                        "def _add(a=1, b=2):\n"
                        "    return a + b\n"
                        "def _boom():\n"
                        "    raise ValueError('kaput')\n"
                        "extension.register_command('pyAdd', _add, 'Add.', flags=[{'name': 'a', 'short': 'x', 'type': int},"
                        " ('b', int, 10, 'Second.')], owner='tests')\n"
                        "extension.register_command('pyBoom', _boom)\n")
                .ok);
    CHECK(run("pyAdd", {{"x", 5}}) == 15);
    CHECK(ops::find("pyAdd")->owner == "tests");
    CHECK_THROWS_WITH_AS(run("pyBoom"), doctest::Contains("kaput"), CommandError);
    CHECK(python::run("assert extension.unregister_command('pyAdd')").ok);
    CHECK_FALSE(ops::find("pyAdd"));

    // An extension can't take over a built-in.
    CHECK_FALSE(python::run("extension.register_command('undo', _add)").ok);
    CHECK(ops::find("undo")->owner.empty());

    // Python packages: dependencies load first, and can't unload from under a dependent.
    pm.addSearchPath(LILA_TEST_PACKAGES_DIR);
    CHECK(pm.load("pyuser") == "pyuser");
    CHECK(pm.isLoaded("pybase"));
    CHECK(pm.loaded() == std::vector<std::string>{"pybase", "pyuser"});
    CHECK(run("pyUserHello") == "user+base");
    CHECK(ops::find("pyBaseHello")->owner == "pybase");
    CHECK_THROWS_WITH_AS(pm.unload("pybase"), doctest::Contains("pyuser depends on it"), extensions::ExtensionError);
    json info = pm.describe("pybase");
    CHECK(info["displayName"] == "Python Base");
    CHECK(info["vendor"] == "Lila Tests");
    CHECK(info["kind"] == "python");

    pm.unloadAll();
    CHECK_FALSE(ops::find("pyUserHello"));
    CHECK_FALSE(ops::find("pyBaseHello"));
    CHECK(pm.loaded().empty());

    // Command docs: one registration feeds help(), the stubs the script editor's language server reads, and its namespace.
    REQUIRE(python::run("extension.register_command('pyDocs', _add, 'Summary line.\\n\\nLonger text.', positional='Things.',"
                        " flags=[{'name': 'mode', 'short': 'md', 'type': str, 'default': 'a', 'doc': 'How', 'choices': ['a', 'b'],"
                        " 'example': 'pyDocs(mode=\"b\")'}], owner='tests', examples=['pyDocs(\"x\", mode=\"a\")'])\n")
                .ok);
    const CommandDef* docs = ops::find("pyDocs");
    REQUIRE(docs);
    CHECK(docs->examples == std::vector<std::string>{"pyDocs(\"x\", mode=\"a\")"});
    REQUIRE(docs->flags.size() == 1);
    CHECK(docs->flags[0].choices == std::vector<std::string>{"a", "b"});
    CHECK(docs->flags[0].example == "pyDocs(mode=\"b\")");
    CHECK(python::run("from lila import _stubgen, ops\n"
                      "stub = _stubgen._render(next(c['owner'] for c in _stubgen._lila.commands() if c['name'] == 'pyDocs'))\n"
                      "assert 'def pyDocs(*objects: Any, mode: str | None = None, md: str | None = None) -> Any:' in stub, stub\n"
                      "doc = ops.pyDocs.__doc__\n"
                      "for part in ('Summary line.', 'Args:', 'objects: Things.',"
                      " \"mode: How. Short form md. One of 'a', 'b'. Default 'a'. Example: `pyDocs(mode=\\\"b\\\")`\","
                      " 'Examples:', '    pyDocs(\\\"x\\\", mode=\\\"a\\\")'):\n"
                      "    assert part in doc, (part, doc)\n"
                      "    assert part.strip() in stub, part\n"
                      "assert 'from lila.ops import pyDocs as pyDocs' in _stubgen._render_builtins()\n"
                      "assert 'from lila.ops import print as print' not in _stubgen._render_builtins()\n"
                      // ruff's builtins and the language server's namespace stub come from one list.
                      "import re, tomllib\n"
                      "ruff = set(tomllib.loads(_stubgen._render_ruff_builtins())['builtins'])\n"
                      "stub = set(re.findall(r' as (\\w+)', _stubgen._render_builtins()))\n"
                      "assert ruff == stub, (ruff ^ stub)\n"
                      "assert {'lila', 'ops', 'Node', 'pyDocs'} <= ruff\n")
              .ok);
    CHECK(python::run("assert extension.unregister_command('pyDocs')").ok);

    // lila.stream: the change stream's groups as dicts, filtered, and a snapshot. No scene commands here, so the
    // edits are made directly and flushed.
    scene::Graph& g = scene::Graph::current();
    g.clear();
    REQUIRE(python::run("from lila import stream\n"
                        "heard = []\n"
                        "sub = stream.subscribe(heard.append, attrs=['translate'])\n")
                .ok);
    scene::Node* p = g.createNode(*scene::types::find("transform"), "P");
    g.setValue(g.plug("P.tx"), 2.0);
    g.setValue(g.plug("P.sx"), 2.0);
    stream::flush();
    CHECK(python::run("try:\n"
                      "    assert len(heard) == 1, heard\n"
                      "    kinds = [c['kind'] for c in heard[0]['changes']]\n"
                      "    assert kinds == ['created', 'value'], kinds\n"
                      "    assert heard[0]['changes'][1]['value'] == [2.0, 0.0, 0.0]\n"
                      "    snap = stream.snapshot(under=['P'], world=True)\n"
                      "    assert [n['path'] for n in snap['nodes']] == ['|P'], snap\n"
                      "    assert snap['nodes'][0]['world'][12] == 2.0, snap\n"
                      "    snap = stream.snapshot(under=['P'], evaluated=True)\n"
                      "    assert snap['nodes'][0]['outputs']['matrix'][12] == 2.0, snap\n"
                      "finally:\n"
                      "    assert stream.unsubscribe(sub)\n")
              .ok);
    CHECK_FALSE(stream::recording());
    g.deleteNode(p->id());
    python::shutdown();
}
