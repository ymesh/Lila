#include <doctest/doctest.h>

#include "core/PrefStore.h"
#include "ui/Controls.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportHeader.h"
#include "ui/ViewportHeaderConfig.h"
#include "ui/ViewportHud.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportWidget.h"

#include <QDoubleSpinBox>
#include <QMenu>
#include <QToolButton>

using namespace lila::ui;

namespace
{
QToolButton* chevronOf(QWidget& header)
{
    for (QToolButton* b : header.findChildren<QToolButton*>(Qt::FindDirectChildrenOnly))
        if (b->toolTip() == "More viewport options")
            return b;
    return nullptr;
}

QToolButton* buttonWithText(QWidget& header, const QString& prefix)
{
    for (QToolButton* b : header.findChildren<QToolButton*>())
        if (b->text().startsWith(prefix))
            return b;
    return nullptr;
}
} // namespace

TEST_CASE("viewport header drops labels, then folds items into the chevron as it narrows")
{
    ViewportWidget view;
    ViewportHeader header(&view);
    header.resize(2000, header.sizeHint().height());
    header.show();
    QToolButton* chevron = chevronOf(header);
    REQUIRE(chevron);
    QToolButton* snap = buttonWithText(header, "Snap");
    QToolButton* symmetry = buttonWithText(header, "Symmetry");
    REQUIRE(snap);
    REQUIRE(symmetry);
    CHECK_FALSE(chevron->isVisible());
    CHECK(snap->toolButtonStyle() == Qt::ToolButtonTextBesideIcon);

    header.resize(520, header.height());
    CHECK(snap->toolButtonStyle() == Qt::ToolButtonIconOnly);

    header.resize(260, header.height());
    CHECK(chevron->isVisible());
    Q_EMIT chevron->menu()->aboutToShow();
    CHECK_FALSE(chevron->menu()->actions().isEmpty());
}

TEST_CASE("a saved viewport header layout picks up entries added to the defaults, but not ones the user removed")
{
    using nlohmann::json;
    auto items = [](const json& c) {
        for (const json& e : c["left"])
            if (e.is_object() && e.value("menu", std::string()) == "Display")
                return e["items"];
        return json();
    };
    auto has = [](const json& list, const std::string& id) { return std::find(list.begin(), list.end(), json(id)) != list.end(); };

    // Saved before Joint Size existed: no known list, and the Display menu without it.
    json saved = viewportHeader::defaultConfig();
    for (json& e : saved["left"])
        if (e.is_object() && e.value("menu", std::string()) == "Display")
        {
            e["items"].erase(e["items"].end() - 1);
            e["items"].erase(e["items"].end() - 1);
        }
    saved["right"].erase(saved["right"].begin());
    saved["left"].erase(std::find(saved["left"].begin(), saved["left"].end(), json("lighting")));
    lila::PrefStore::get().set("general", viewportHeader::kPref, saved);
    json merged = viewportHeader::config();
    CHECK(merged["left"][2] == json("lighting"));
    CHECK(has(items(merged), "jointSize"));
    CHECK(items(merged).size() == items(viewportHeader::defaultConfig()).size());
    CHECK(merged["right"].front() == json("snap"));

    // Removed after the layout knows about it: stays removed.
    json edited = merged;
    edited.erase("known");
    for (json& e : edited["left"])
        if (e.is_object() && e.value("menu", std::string()) == "Display")
            e["items"].erase(e["items"].end() - 1);
    viewportHeader::setConfig(edited);
    CHECK_FALSE(has(items(viewportHeader::config()), "jointSize"));

    lila::PrefStore::get().remove("general", viewportHeader::kPref);
}

TEST_CASE("Lighting popover: four modes, rotation follows the mode, and lighting is out of Overlays")
{
    lila::PrefStore& p = lila::PrefStore::get();
    p.set("general", "/viewport/lighting", std::string("sculpt"));
    p.set("general", "/viewport/rigRotation", 30.0);
    ViewportWidget view;
    ViewportHeader header(&view);
    header.resize(1600, header.sizeHint().height());
    header.show();
    QToolButton* lighting = buttonWithText(header, "Lighting");
    REQUIRE(lighting);
    Q_EMIT lighting->menu()->aboutToShow();
    auto* modes = lighting->menu()->findChild<controls::Segmented*>();
    REQUIRE(modes);
    CHECK(modes->current() == 2);
    QDoubleSpinBox* rotation = nullptr;
    for (QDoubleSpinBox* s : lighting->menu()->findChildren<QDoubleSpinBox*>())
        if (s->toolTip().startsWith("Turns the light"))
            rotation = s;
    REQUIRE(rotation);
    CHECK(rotation->value() == doctest::Approx(30.0));

    QToolButton* overlays = buttonWithText(header, "Overlays");
    REQUIRE(overlays);
    Q_EMIT overlays->menu()->aboutToShow();
    for (QAction* a : overlays->menu()->actions())
        CHECK_FALSE(a->data().toString().startsWith("viewport.lighting."));

    // Turning follows the mode: the rig in sculpt, the HDRI in environment, nothing in flat.
    p.set("general", "/viewport/env/rotation", 0.0);
    viewportDisplay::turnLight(200.0);
    CHECK(p.value<double>("general", "/viewport/rigRotation", 0.0) == doctest::Approx(-130.0));
    p.set("general", "/viewport/lighting", std::string("environment"));
    viewportDisplay::turnLight(15.0);
    CHECK(p.value<double>("general", "/viewport/env/rotation", 0.0) == doctest::Approx(15.0));
    p.set("general", "/viewport/lighting", std::string("flat"));
    viewportDisplay::turnLight(15.0);
    CHECK(p.value<double>("general", "/viewport/env/rotation", 0.0) == doctest::Approx(15.0));

    for (const char* key : {"/viewport/lighting", "/viewport/rigRotation", "/viewport/env/rotation"})
        p.remove("general", key);
}

TEST_CASE("layout keys: registered per layout, unbound actions left out; Reset Light puts the current mode's light back")
{
    viewportKeys::add("TestLayout", [] {
        std::vector<KeyHint> keys = {{"Shift+Alt+right-drag", "Move the light"}};
        viewportKeys::addAction(keys, "test.noSuchAction", "Nothing");
        return keys;
    });
    const std::vector<KeyHint> keys = viewportKeys::forLayout("TestLayout");
    REQUIRE(keys.size() == 1);
    CHECK(keys[0].what == "Move the light");
    CHECK(viewportKeys::forLayout("NoSuchLayout").empty());

    lila::PrefStore& p = lila::PrefStore::get();
    CHECK(keysShown());
    p.set("general", kShowKeysPref, false);
    CHECK_FALSE(keysShown());
    p.remove("general", kShowKeysPref);

    p.set("general", "/viewport/lighting", std::string("sculpt"));
    p.set("general", "/viewport/rigRotation", 40.0);
    p.set("general", "/viewport/rigTilt", 80.0);
    p.set("general", "/viewport/env/rotation", 25.0);
    viewportDisplay::resetLight();
    CHECK(p.value<double>("general", "/viewport/rigRotation", -1.0) == doctest::Approx(0.0));
    CHECK(p.value<double>("general", "/viewport/rigTilt", -1.0) == doctest::Approx(viewportDisplay::kDefaultRigTilt));
    CHECK(p.value<double>("general", "/viewport/env/rotation", -1.0) == doctest::Approx(25.0));
    p.set("general", "/viewport/lighting", std::string("environment"));
    viewportDisplay::resetLight();
    CHECK(p.value<double>("general", "/viewport/env/rotation", -1.0) == doctest::Approx(0.0));
    for (const char* key : {"/viewport/lighting", "/viewport/rigRotation", "/viewport/rigTilt", "/viewport/env/rotation"})
        p.remove("general", key);
}

TEST_CASE("view switcher: segments under the cube, a second click flips to the opposite side")
{
    using namespace lila::ui;
    const auto segs = hud::viewSwitcherSegments(1000.0, 700.0, true);
    REQUIRE(segs.size() == 5);
    CHECK(std::string(segs[0].view) == "persp");
    CHECK(std::string(segs[4].view) == "camera");
    CHECK(segs[0].rect.top() > hud::viewCubeRect(1000.0).bottom());
    CHECK(segs[4].rect.right() <= 1000.0);
    for (size_t i = 1; i < segs.size(); ++i)
        CHECK(segs[i].rect.left() > segs[i - 1].rect.right());
    // Too small a pane drops it rather than squeezing it.
    CHECK(hud::viewSwitcherSegments(240.0, 700.0, true).empty());

    ViewportWidget view;
    view.switchView("front");
    CHECK(view.viewName() == "front");
    view.switchView("front");
    CHECK(view.viewName() == "back");
    view.switchView("front");
    CHECK(view.viewName() == "front");
    view.switchView("right");
    CHECK(view.viewName() == "right");
    view.switchView("persp");
    CHECK(view.viewName() == "persp");
}

TEST_CASE("viewport header settles at every width instead of flipping labels on each layout pass")
{
    ViewportWidget view;
    ViewportHeader header(&view);
    header.resize(2000, header.sizeHint().height());
    header.show();
    auto styles = [&] {
        std::vector<int> out;
        for (QToolButton* b : header.findChildren<QToolButton*>())
            out.push_back(b->isVisibleTo(&header) ? int(b->toolButtonStyle()) : -1);
        return out;
    };
    int labelSwaps = 0;
    auto before = styles();
    for (int w = 2000; w >= 200; --w)
    {
        header.resize(w, header.height());
        QCoreApplication::sendPostedEvents();
        const auto settled = styles();
        labelSwaps += settled != before;
        before = settled;
        for (int pass = 0; pass < 3; ++pass)
        {
            QCoreApplication::postEvent(&header, new QEvent(QEvent::LayoutRequest));
            QCoreApplication::sendPostedEvents();
        }
        CHECK_MESSAGE(styles() == settled, "width " << w);
    }
    CHECK(labelSwaps > 0);
}
