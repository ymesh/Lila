#include <doctest/doctest.h>

#include "PrintGeo.h"

#include "extensions/ExtensionManager.h"
#include "ops/Command.h"

#include <lila/Diagnostics.h>
#include <lila/Graph.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/OptionBox.h>
#include <lila/Prefs.h>

#include <filesystem>
#include <random>

using namespace lila;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

void loadExtension()
{
    if (!ops::find("printCheck"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_PRINT_PACKAGE) == "Print");
}

/** A fresh scene holding one 10 cm cube, without history. */
std::string freshCube(const char* name = "Block")
{
    loadExtension();
    run("newScene");
    run("clearIssues");
    return run("polyCube", {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}, {"name", name}, {"generator", false}})[0];
}

scene::MeshPtr outMesh(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(name);
    REQUIRE(n);
    return n->as<scene::MeshPtr>(n->type().attrIndex("outMesh"));
}

fs::path scratch()
{
    std::random_device rd;
    fs::path dir = fs::temp_directory_path() / ("lila_print_cmd_" + std::to_string(rd()));
    fs::create_directories(dir);
    return dir;
}
} // namespace

TEST_CASE("print commands: a clean cube reports nothing, a holed one reports an issue with select and fix")
{
    const std::string cube = freshCube();
    json counts = run("printCheck", {{"args", {cube}}});
    REQUIRE(counts.size() == 1);
    CHECK(counts.begin().value()["holes"] == 0);
    CHECK(run("listIssues", {{"source", "print"}}).empty());

    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("deleteComponents");
    counts = run("printCheck", {{"args", {cube}}});
    CHECK(counts.begin().value()["holes"] == 1);
    const json issues = run("listIssues", {{"source", "print"}});
    REQUIRE(issues.size() == 1);
    CHECK(issues[0]["severity"] == "error");
    CHECK(issues[0]["fix"].get<std::string>().find("meshRepair") != std::string::npos);
    CHECK(issues[0]["fix"].get<std::string>().find("holes=True") != std::string::npos);
    CHECK(issues[0]["select"].get<std::string>().find("printSelect") != std::string::npos);

    CHECK(run("printSelect", {{"args", {cube}}, {"check", "holes"}}) == 4);
    const json sel = run("componentSelection");
    CHECK(sel.begin().value()["edges"].size() == 4);

    run("validate", {{"args", {"print"}}});
    CHECK(run("listIssues", {{"source", "print"}}).size() == 1);
}

TEST_CASE("print commands: the validator checks only meshes a Print command has worked on")
{
    const std::string cube = freshCube();
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("deleteComponents");
    const std::string other = run("polyCube", {{"width", 10.0}, {"name", "Other"}, {"generator", false}})[0];
    run("validate");
    CHECK(run("listIssues", {{"source", "print"}}).empty());

    run("printInfo", {{"args", {cube}}});
    run("validate");
    const json issues = run("listIssues", {{"source", "print"}});
    REQUIRE(issues.size() == 1);
    CHECK(issues[0]["target"].get<std::string>().find(cube) != std::string::npos);

    run("delete", {{"args", {cube}}});
    run("clearIssues");
    run("validate");
    CHECK(run("listIssues", {{"source", "print"}}).empty());
    CHECK_FALSE(other.empty());
}

TEST_CASE("print commands: meshes exported to STL are checked for printing")
{
    const std::string cube = freshCube();
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("deleteComponents");
    const std::string other = run("polyCube", {{"width", 10.0}, {"name", "Other"}, {"generator", false}})[0];
    const fs::path dir = scratch();
    run("exportStl", {{"args", {(dir / "block.stl").generic_string(), cube}}});
    const json issues = run("listIssues", {{"source", "print"}});
    REQUIRE(issues.size() == 1);
    CHECK(issues[0]["target"].get<std::string>().find(cube) != std::string::npos);
    // An OBJ export is not taken as a print: the other cube stays unchecked.
    run("exportObj", {{"args", {(dir / "other.obj").generic_string(), other}}});
    CHECK(run("listIssues", {{"source", "print"}}).size() == 1);
    fs::remove_all(dir);
}

TEST_CASE("print commands: info, scale to fit and printer presets")
{
    const std::string cube = freshCube();
    PrefStore::get().set("print", "/printer", "custom");
    PrefStore::get().set("print", "/custom/width", 50.0);
    PrefStore::get().set("print", "/custom/depth", 50.0);
    PrefStore::get().set("print", "/custom/height", 50.0);
    json info = run("printInfo", {{"args", {cube}}});
    CHECK(info["size"][0] == doctest::Approx(100.0));
    CHECK(info["volume"] == doctest::Approx(1000.0));
    CHECK(info["fits"] == false);
    CHECK(info["printer"]["id"] == "custom");

    const json fit = run("printScaleToFit", {{"args", {cube}}, {"margin", 5.0}});
    CHECK(fit["scale"] == doctest::Approx(0.4));
    info = run("printInfo", {{"args", {cube}}});
    CHECK(info["size"][2] == doctest::Approx(40.0));
    CHECK(info["fits"] == true);
    PrefStore::get().remove("print", "/printer");

    const ops::options::Override* note = ops::options::findOverride("exportStl");
    REQUIRE(note);
    CHECK(note->notes.contains("unit"));
}

TEST_CASE("print commands: thickness and overhang maps write a colour set, off removes it")
{
    const std::string cube = freshCube();
    json r = run("printAnalyze", {{"args", {cube}}, {"mode", "thickness"}, {"minWall", 1.0}});
    CHECK(r.begin().value()["thinnest"] == doctest::Approx(100.0));
    CHECK(geo::colors::current(*outMesh(cube)) == "printThickness");
    r = run("printAnalyze", {{"args", {cube}}, {"mode", "overhang"}});
    CHECK(r.begin().value()["overhangFaces"] == 0);
    CHECK(geo::colors::has(*outMesh(cube), "printOverhang"));
    CHECK_FALSE(geo::colors::has(*outMesh(cube), "printThickness"));
    run("printAnalyze", {{"mode", "off"}});
    CHECK_FALSE(geo::colors::has(*outMesh(cube), "printOverhang"));
}

TEST_CASE("print commands: hollow, drain hole, split and lay flat")
{
    const std::string cube = freshCube();
    const json h = run("printHollow", {{"args", {cube}}, {"thickness", 10.0}, {"holeRadius", 2.0}});
    const json res = h.begin().value();
    CHECK(res["volumeBefore"] == doctest::Approx(1000.0));
    CHECK(res["drainHoles"] == 1);
    CHECK(res["volumeAfter"].get<double>() < 488.0);
    CHECK(res["volumeAfter"].get<double>() > 487.0);
    CHECK(print::checkPrintability(*outMesh(cube)).of(print::Check::Holes).count == 0);
    run("undo");
    CHECK(print::volume(*outMesh(cube)) == doctest::Approx(1000.0));

    const json parts = run("printSplit", {{"args", {cube}}, {"pegs", 1}, {"pegRadius", 5.0}, {"pegLength", 10.0}});
    REQUIRE(parts["parts"].size() == 2);
    CHECK(parts["pegs"] == 1);
    for (const json& p : parts["parts"])
        CHECK(print::checkPrintability(*outMesh(p.get<std::string>())).of(print::Check::Holes).count == 0);

    const std::string tall = run("polyCube", {{"width", 2.0}, {"height", 10.0}, {"depth", 4.0}, {"name", "Tall"}, {"generator", false}})[0];
    run("printLayFlat", {{"args", {tall}}});
    const json info = run("printInfo", {{"args", {tall}}});
    CHECK(info["size"][2] == doctest::Approx(20.0));
}

TEST_CASE("print commands: 3MF export and import through the translator")
{
    const std::string cube = freshCube();
    const fs::path dir = scratch();
    const std::string path = (dir / "cube.3mf").string();
    run("export3mf", {{"args", {path, cube}}});
    REQUIRE(fs::exists(path));
    const json names = run("import3mf", {{"args", {path}}});
    REQUIRE(names.size() == 1);
    const scene::MeshPtr m = outMesh(names[0].get<std::string>());
    CHECK(print::volume(*m) == doctest::Approx(1000.0));
    CHECK(m->numFaces() == 12);
    const json translators = run("fileTranslators");
    bool found = false;
    for (const json& t : translators)
        found |= t.value("id", "") == "3mf";
    CHECK(found);
    std::error_code ec;
    fs::remove_all(dir, ec);
}
