#include <doctest/doctest.h>

#include "core/PrefStore.h"
#include "ops/Command.h"
#include "ui/ThemeModel.h"

#include <filesystem>
#include <fstream>

using namespace lila;
namespace themes = lila::ui::themes;

namespace
{
void ensureThemeCommands()
{
    if (!ops::find("setTheme"))
        themes::registerCommands();
}

void clearSaved()
{
    PrefStore::get().remove("general", themes::kThemesPref);
    PrefStore::get().remove("general", themes::kThemePref);
    themes::invalidate();
}
} // namespace

TEST_CASE("theme colours parse and format")
{
    CHECK(themes::parseColor("#fff") == themes::Rgba{255, 255, 255, 255});
    CHECK(themes::parseColor("#102030") == themes::Rgba{0x10, 0x20, 0x30, 255});
    CHECK(themes::parseColor("#10203080") == themes::Rgba{0x10, 0x20, 0x30, 0x80});
    CHECK_FALSE(themes::parseColor("102030"));
    CHECK_FALSE(themes::parseColor("#12345"));
    CHECK_FALSE(themes::parseColor("#zzzzzz"));
    CHECK(themes::formatColor({1, 2, 3, 255}) == "#010203");
    CHECK(themes::formatColor({1, 2, 3, 4}) == "#01020304");
    CHECK(themes::contrast({0, 0, 0, 255}, {255, 255, 255, 255}) == doctest::Approx(21.0));
}

TEST_CASE("every built-in theme sets every token, directly or through its base")
{
    for (const themes::Theme& t : themes::builtIns())
        for (const themes::TokenInfo& info : themes::tokens())
        {
            const themes::Rgba missing{255, 0, 255, 255};
            INFO(t.name, " lacks ", info.id);
            CHECK(t.color(info.id) != missing);
        }
    for (const char* base : {"dark", "light"})
        CHECK(themes::baseTheme(base).colors.size() == themes::tokens().size());
}

TEST_CASE("theme JSON round trips")
{
    themes::Theme t;
    t.name = "Round Trip";
    t.author = "Someone";
    t.base = "light";
    t.colors["highlight"] = {10, 20, 30, 255};
    t.colors["hover"] = {1, 2, 3, 128};
    themes::ParseResult r = themes::parseText(themes::toText(t));
    REQUIRE(r.theme);
    CHECK(r.errors.empty());
    CHECK(r.warnings.empty());
    CHECK(r.theme->name == t.name);
    CHECK(r.theme->author == t.author);
    CHECK(r.theme->base == "light");
    CHECK(r.theme->colors == t.colors);

    // full=true writes every token resolved, and reads back to the same colours.
    themes::ParseResult full = themes::parse(themes::toJson(*themes::builtIn("Colour"), true));
    REQUIRE(full.theme);
    CHECK(full.theme->colors.size() == themes::tokens().size());
    for (const themes::TokenInfo& info : themes::tokens())
        CHECK(full.theme->color(info.id) == themes::builtIn("Colour")->color(info.id));
}

TEST_CASE("missing tokens fall back to the base theme")
{
    themes::ParseResult dark = themes::parseText(R"({"name": "Sparse", "colors": {"highlight": "#ff0000"}})");
    REQUIRE(dark.theme);
    CHECK(dark.theme->color("highlight") == themes::Rgba{255, 0, 0, 255});
    CHECK(dark.theme->color("window") == themes::builtIn("Monotone")->color("window"));
    CHECK(dark.theme->color("axisX") == themes::builtIn("Monotone")->color("axisX"));

    themes::ParseResult light = themes::parseText(R"({"name": "Sparse Light", "base": "light"})");
    REQUIRE(light.theme);
    CHECK(light.theme->color("window") == themes::builtIn("Light")->color("window"));
}

TEST_CASE("theme import reports what is wrong")
{
    CHECK(themes::parseText("{not json").errors == std::vector<std::string>{"not valid JSON"});
    CHECK_FALSE(themes::parseText("[1, 2]").theme);
    CHECK_FALSE(themes::parseText(R"({"colors": {}})").theme); // no name
    CHECK_FALSE(themes::parseText(R"({"name": ""})").theme);
    CHECK_FALSE(themes::parseText(R"({"name": "x", "base": "purple"})").theme);
    CHECK_FALSE(themes::parseText(R"({"name": "x", "colors": []})").theme);

    themes::ParseResult bad = themes::parseText(R"({"name": "x", "colors": {"text": "white", "window": 12}})");
    CHECK_FALSE(bad.theme);
    CHECK(bad.errors.size() == 2);

    themes::ParseResult unknown = themes::parseText(R"({"name": "x", "colors": {"frobnicate": "#ffffff"}})");
    REQUIRE(unknown.theme);
    CHECK(unknown.warnings.size() == 1);
    CHECK(unknown.theme->colors.empty());
}

TEST_CASE("built-in themes keep text readable (WCAG AA)")
{
    for (const themes::Theme& t : themes::builtIns())
    {
        INFO(t.name);
        for (const char* bg : {"window", "base", "altBase", "button", "panel", "menu", "input"})
        {
            INFO(bg);
            CHECK(themes::contrast(t.color("text"), t.color(bg)) >= 4.5);
        }
        CHECK(themes::contrast(t.color("tooltipText"), t.color("tooltip")) >= 4.5);
        CHECK(themes::contrast(t.color("highlightText"), t.color("highlight")) >= 4.5);
        CHECK(themes::contrast(t.color("checkedText"), t.color("checked")) >= 4.5);
        CHECK(themes::contrast(t.color("text"), t.color("hover")) >= 4.5);
        // Secondary text: WCAG's 3:1 for large or incidental text.
        CHECK(themes::contrast(t.color("dimText"), t.color("window")) >= 3.0);
        CHECK(themes::contrast(t.color("dimText"), t.color("base")) >= 3.0);
    }
}

TEST_CASE("Monotone shades map greys to themselves")
{
    const themes::Theme& mono = *themes::builtIn("Monotone");
    for (int v = 0; v <= 255; ++v)
        CHECK(themes::shade(mono, v, 77) == themes::Rgba{uint8_t(v), uint8_t(v), uint8_t(v), 77});
    const themes::Theme& light = *themes::builtIn("Light");
    CHECK(themes::shade(light, 48) == light.color("window"));
    CHECK(themes::shade(light, 212) == light.color("text"));
    CHECK(themes::isDefault(*themes::builtIn("Colour"), "window"));
    CHECK_FALSE(themes::isDefault(*themes::builtIn("Colour"), "highlight"));
}

TEST_CASE("the Monotone pref greys a hued theme except where Monotone keeps hue")
{
    clearSaved();
    PrefStore::get().set("general", themes::kThemePref, "Colour");
    PrefStore::get().set("general", themes::kMonotonePref, true);
    themes::invalidate();
    std::shared_ptr<const themes::Theme> t = themes::current();
    themes::Rgba hi = t->color("highlight");
    CHECK(hi.r == hi.g);
    CHECK(hi.g == hi.b);
    CHECK(t->color("axisX") == themes::builtIn("Colour")->color("axisX"));
    themes::Rgba sock = t->color("socketVector");
    CHECK(sock.r == sock.g);
    CHECK(sock.g == sock.b);

    PrefStore::get().set("general", themes::kMonotonePref, false);
    themes::invalidate();
    CHECK(themes::current()->color("highlight") == themes::builtIn("Colour")->color("highlight"));
    PrefStore::get().remove("general", themes::kMonotonePref);
    clearSaved();
}

TEST_CASE("Monotone tells socket types and node families apart")
{
    const themes::Theme& mono = *themes::builtIn("Monotone");
    CHECK(mono.color("socketFloat") != mono.color("socketInt"));
    CHECK(mono.color("socketVector") != mono.color("socketRotation"));
    CHECK(mono.color("nodeInput") != mono.color("nodeGeometry"));
    CHECK(mono.color("nodeMesh") == mono.color("nodeGeometry"));
}

TEST_CASE("theme commands save, switch, rename and delete")
{
    ensureThemeCommands();
    clearSaved();
    CHECK(ops::execute("currentTheme", json::object()) == "Monotone");
    CHECK(ops::execute("listThemes", json::object()).size() == themes::builtIns().size());

    ops::execute("setTheme", {{"args", {"Colour"}}});
    CHECK(themes::current()->name == "Colour");
    CHECK_THROWS_AS(ops::execute("setTheme", {{"args", {"Nope"}}}), CommandError);

    std::string copy = ops::execute("duplicateTheme", {{"args", {"Colour"}}}).get<std::string>();
    CHECK(copy == "Colour Copy");
    ops::execute("setThemeColor", {{"args", {copy}}, {"token", "highlight"}, {"color", "#123456"}});
    CHECK(themes::find(copy)->color("highlight") == themes::Rgba{0x12, 0x34, 0x56, 255});
    CHECK_THROWS_AS(ops::execute("setThemeColor", {{"args", {"Colour"}}, {"token", "highlight"}, {"color", "#123456"}}), CommandError);

    ops::execute("setTheme", {{"args", {copy}}});
    ops::execute("renameTheme", {{"args", {copy}}, {"to", "Mine"}});
    CHECK(themes::currentName() == "Mine");
    CHECK_FALSE(themes::find(copy));

    std::string text = ops::execute("exportTheme", {{"args", {"Mine"}}}).get<std::string>();
    CHECK_THROWS_AS(ops::execute("importTheme", {{"text", text}}), CommandError); // exists
    CHECK(ops::execute("importTheme", {{"text", text}, {"name", "Shared"}}) == "Shared");
    CHECK(themes::find("Shared")->color("highlight") == themes::Rgba{0x12, 0x34, 0x56, 255});
    CHECK_THROWS_AS(ops::execute("importTheme", {{"text", "{\"name\": \"Monotone\"}"}}), CommandError);
    CHECK_THROWS_AS(ops::execute("importTheme", {{"text", "{\"name\": \"Broken\", \"colors\": {\"text\": \"#xyz\"}}"}}), CommandError);

    auto file = std::filesystem::temp_directory_path() / "lila_tests" / "shared.lilatheme";
    ops::execute("exportTheme", {{"args", {"Shared"}}, {"file", file.string()}});
    CHECK(ops::execute("importTheme", {{"file", file.string()}, {"name", "From File"}, {"apply", true}}) == "From File");
    CHECK(themes::currentName() == "From File");

    ops::execute("deleteTheme", {{"args", {"From File"}}});
    CHECK(themes::currentName() == "Monotone");
    CHECK_THROWS_AS(ops::execute("deleteTheme", {{"args", {"Colour"}}}), CommandError);
    clearSaved();
}
