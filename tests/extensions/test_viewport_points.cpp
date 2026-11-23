#include "Isolate.h"

#include "extensions/ExtensionManager.h"
#include "io/SceneFile.h"
#include "scene/SceneCommands.h"
#include "python/Python.h"
#include "scene/Graph.h"
#include "core/Log.h"
#include "ui/Actions.h"
#include "ui/CursorBadges.h"
#include "ui/DrawOverrides.h"
#include "ui/Gizmo.h"
#include "ui/Overlay.h"
#include "ui/RenderPasses.h"
#include "ui/SelectionHandlers.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>

using namespace lila;
using namespace lila::ui;

namespace
{
/** Every viewport registry an extension can add to, as ids. */
json viewportRegistries()
{
    return {{"gizmos", gizmos::registry().ids()},
            {"badges", cursorBadges::ids()},
            {"icons", overlayIcons::ids()},
            {"drawers", viewportHooks::drawerIds()},
            {"pickers", viewportHooks::pickerIds()},
            {"drawOverrides", drawOverrides::registry().ids()},
            {"selectionHandlers", selectionHandlers::registry().ids()},
            {"renderPasses", renderPasses::registry().ids()},
            {"tools", viewportTools::registry().ids()},
            {"types", scene::types::registry().ids()}};
}
} // namespace

TEST_CASE("viewport extension points: an extension registers one of each, and unloading restores every registry")
{
    isolate();
    extensions::registerCommands();
    scene::registerCommands();
    io::registerCommands();
    run("newScene");

    const json before = viewportRegistries();
    extensions::ExtensionManager& em = extensions::ExtensionManager::get();
    REQUIRE(em.load(LILA_TEST_VIEWPORT_POINTS_PACKAGE) == "viewportPoints");
    const json loaded = viewportRegistries();
    // It draws through a draw override rather than a drawer or picker.
    for (auto& [key, ids] : loaded.items())
        if (key != "drawers" && key != "pickers")
            CHECK_MESSAGE(ids != before[key], key);

    // Owned by the extension.
    CHECK(*gizmos::registry().ownerOf("viewportPoints.ground") == "viewportPoints");
    CHECK(*selectionHandlers::registry().ownerOf("viewportPoints.proxy") == "viewportPoints");
    CHECK(*drawOverrides::registry().ownerOf("viewportPoints.marker") == "viewportPoints");

    // The node type draws through its override; the gizmo, tool and handler work through the registries.
    const scene::NodeType* marker = scene::types::find("viewportPointsMarker");
    REQUIRE(marker);
    REQUIRE(drawOverrides::forType(*marker));
    CHECK(drawOverrides::forType(*marker)->id == "viewportPoints.marker");
    std::unique_ptr<Gizmo> gz = gizmos::create("viewportPoints.ground");
    REQUIRE(gz);
    CHECK(gz->supports("move"));
    CHECK_FALSE(gz->supports("rotate"));
    CHECK(gizmos::setActive("viewportPoints.ground"));
    CHECK(viewportTools::activate("viewportPoints.place"));
    CHECK(std::string(viewportTools::active()->name()) == "viewportPointsPlace");
    CHECK(selectionHandlers::setActive("viewportPoints.proxy"));
    CHECK(selectionHandlers::redirect({selectionHandlers::Hit{.name = "arm_proxy"}, selectionHandlers::Hit{.name = "leg"}}) ==
          std::vector<std::string>{"arm", "leg"});
    ViewportState st;
    st.tool = "viewportPointsPlace";
    std::vector<const cursorBadges::Badge*> on = cursorBadges::active(st);
    CHECK(std::any_of(on.begin(), on.end(), [](const cursorBadges::Badge* b) { return b->id == "viewportPoints.placing"; }));

    // The instance a viewport would hold must go before the code does.
    gz.reset();
    em.unload("viewportPoints");
    CHECK(viewportRegistries() == before);
    CHECK_FALSE(viewportTools::active());
    CHECK(selectionHandlers::activeId().empty());
    CHECK(gizmos::activeId() == gizmos::kDefault);
    gizmos::setActive("");
}

TEST_CASE("viewport extension points from Python: owned by the loading extension, gone when it unloads" * doctest::skip())
{
    isolate();
    REQUIRE(python::init({"lila_viewport_extension_tests"}));
    const json before = viewportRegistries();

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "lila_py_viewport_points";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "pyviewportpoints.py")
        << "from lila import viewport\n"
           "def initialize_extension():\n"
           "    assert viewport.available()\n"
           "    assert 'lila.trs' in [g['id'] for g in viewport.gizmos()]\n"
           "    viewport.add_icon('pyvp.icon', tabler='point')\n"
           "    viewport.add_badge('pyvp.badge', 'pyvp.icon', lambda s: s['tool'] == 'pyvp', message='py')\n"
           "    viewport.add_drawer('pyvp.drawer', lambda v: {'lines': [{'a': [0, 0, 0], 'b': [0, 10, 0]}]})\n"
           "    viewport.add_picker('pyvp.picker', lambda q: [['pyThing', 1.0]] if q['click'] else [])\n"
           "    viewport.add_draw_override('pyvp.override', 'pyvpType', draw=lambda c: {}, pick=lambda c: None)\n"
           "    viewport.add_selection_handler('pyvp.handler', redirect=lambda h: [h['name'] + '_target'])\n"
           "    viewport.add_render_pass('pyvp.pass', lambda c: {}, stage='final')\n"
           "    viewport.add_tool('pyvp.tool', lambda e: e['type'] == 'press', name='pyvp')\n";
    extensions::ExtensionManager& em = extensions::ExtensionManager::get();
    em.addSearchPath(dir);
    REQUIRE(em.load("pyviewportpoints") == "pyviewportpoints");

    CHECK(*selectionHandlers::registry().ownerOf("pyvp.handler") == "pyviewportpoints");
    CHECK(*viewportTools::registry().ownerOf("pyvp.tool") == "pyviewportpoints");
    REQUIRE(selectionHandlers::setActive("pyvp.handler"));
    CHECK(selectionHandlers::redirect({selectionHandlers::Hit{.name = "cube"}}) == std::vector<std::string>{"cube_target"});
    CHECK(python::run("from lila import viewport\nassert viewport.selection_handler() == 'pyvp.handler'").ok);
    CHECK(python::run("from lila import viewport\nassert viewport.activate_tool('pyvp.tool') and viewport.tool() == 'pyvp.tool'").ok);
    std::vector<viewportHooks::PickHit> hits;
    for (const auto& p : viewportHooks::pickers())
        for (const auto& h : p(QPointF(1, 1), QPointF(1, 1), true, ViewInfo()))
            hits.push_back(h);
    CHECK(std::any_of(hits.begin(), hits.end(), [](const viewportHooks::PickHit& h) { return h.name == "pyThing"; }));
    render::FrameState s;
    OverlayPainter painter(s.overlayTris, 1.0);
    for (const auto& d : viewportHooks::drawers())
        d(s, painter, ViewInfo());
    CHECK(s.lines.size() >= 2);

    em.unload("pyviewportpoints");
    CHECK_FALSE(viewportTools::active());
    CHECK(viewportRegistries() == before);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("a Python action that raises reports its message and leaves the app running" * doctest::skip())
{
    isolate();
    REQUIRE(python::init({"lila_viewport_extension_tests"}));
    REQUIRE(python::run("import lila\n"
                        "from lila import ui\n"
                        "def _fail():\n"
                        "    raise lila.CommandError('select the objects to send')\n"
                        "ui.add_action('pyfail.send', _fail, 'Send Somewhere')\n")
                .ok);
    REQUIRE(actions::find("pyfail.send"));
    const std::vector<LogEntry> before = log::since(0);
    const uint64_t mark = before.empty() ? 0 : before.back().seq;
    CHECK_FALSE(actions::run(nlohmann::json("pyfail.send")));
    bool reported = false;
    for (const LogEntry& e : log::since(mark))
        reported |= e.level == LogLevel::Error && e.text == "Send Somewhere: select the objects to send";
    CHECK(reported);
    actions::remove("pyfail.send");
}
