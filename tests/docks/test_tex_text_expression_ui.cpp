#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "ui/FontPicker.h"
#include "ui/MultilineEdit.h"
#include "ui/TexParams.h"
#include "ui/TextureGraphUi.h"

#include <QAction>
#include <QLabel>
#include <QLineEdit>

using namespace lila;
using json = nlohmann::json;
using namespace dockstest;

namespace
{
json nodeInfo(const std::string& set, const std::string& name)
{
    const json info = run("texGraphInfo", {{"args", {set}}});
    for (const json& n : info["nodes"])
        if (n.value("name", "") == name)
        {
            json out = n;
            out["set"] = set;
            return out;
        }
    return json();
}

json paramOf(const json& node, const std::string& id)
{
    for (const json& p : node["params"])
        if (p["id"] == id)
            return p;
    return json();
}

QAction* actionNamed(QWidget* panel, const QString& text)
{
    for (QWidget* w : panel->findChildren<QWidget*>())
        for (QAction* a : w->actions())
            if (a->text() == text)
                return a;
    return nullptr;
}
} // namespace

TEST_CASE("text node fields edit its words and font")
{
    registerTexGraphUi();
    run("newScene");
    const std::string set = run("texCreateSet", {{"args", {"Words"}}, {"resolution", 32}}).get<std::string>();
    const std::string text = run("texAddNode", {{"args", {set, "text"}}}).get<std::string>();
    std::map<std::string, json> sets;
    QWidget host;
    QWidget* panel = ui::texParams::nodePanel(&host, nodeInfo(set, text), [&](const std::string& id, const json& v) { sets[id] = v; });
    REQUIRE(panel);
    const QList<ui::MultilineEdit*> boxes = panel->findChildren<ui::MultilineEdit*>();
    REQUIRE(boxes.size() == 1);
    CHECK(boxes.front()->toPlainText() == "Lila");
    Q_EMIT boxes.front()->committed("Hello\nthere");
    CHECK(sets["text"] == "Hello\nthere");
    const QList<ui::FontPicker*> fonts = panel->findChildren<ui::FontPicker*>();
    REQUIRE(fonts.size() == 1);
    Q_EMIT fonts.front()->chosen("JetBrains Mono");
    CHECK(sets["font"] == "JetBrains Mono");
}

TEST_CASE("a number parameter's right-click adds, edits and removes an expression")
{
    registerTexGraphUi();
    run("newScene");
    const std::string set = run("texCreateSet", {{"args", {"Exprs"}}, {"resolution", 32}}).get<std::string>();
    run("texAddNode", {{"args", {set, "transform"}}, {"name", "A"}});
    run("texAddNode", {{"args", {set, "transform"}}, {"name", "B"}});
    run("texSetValue", {{"args", {set, "A.offsetX", 0.2}}});
    run("texSetValue", {{"args", {set, "B.offsetX", 0.4}}});
    QWidget host;
    auto panelFor = [&](const std::string& name) { return ui::texParams::nodePanel(&host, nodeInfo(set, name), [](const std::string&, const json&) {}); };

    QAction* add = actionNamed(panelFor("B"), "Add Expression");
    REQUIRE(add);
    add->trigger();
    pump();
    CHECK(paramOf(nodeInfo(set, "B"), "offsetX")["expression"] == "0.4");

    QWidget* panel = panelFor("B");
    QLineEdit* edit = nullptr;
    for (QLineEdit* e : panel->findChildren<QLineEdit*>())
        if (e->text().startsWith("= "))
            edit = e;
    REQUIRE(edit);
    CHECK(edit->text() == "= 0.4");
    edit->setText("= A.offsetX * 2");
    Q_EMIT edit->editingFinished();
    pump();
    const json p = paramOf(nodeInfo(set, "B"), "offsetX");
    CHECK(p["expression"] == "A.offsetX * 2");
    CHECK(p["value"].get<double>() == doctest::Approx(0.4));

    panel = panelFor("B");
    bool shown = false;
    for (QLabel* l : panel->findChildren<QLabel*>())
        shown = shown || l->text() == "0.4";
    CHECK(shown);
    QAction* remove = actionNamed(panel, "Remove Expression");
    REQUIRE(remove);
    remove->trigger();
    pump();
    CHECK_FALSE(paramOf(nodeInfo(set, "B"), "offsetX").contains("expression"));
}
