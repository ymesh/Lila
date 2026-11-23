#include <doctest/doctest.h>

#include "TestUtil.h"

#include "core/MainQueue.h"
#include "scene/Graph.h"
#include "scene/Light.h"
#include "scene/SceneCommands.h"
#include "ui/Actions.h"
#include "ui/SceneLights.h"

#include <QToolButton>
#include <QWidget>

#include <memory>

using namespace lila;
using namespace lila::ui;
using dockstest::pump;
using dockstest::run;

TEST_CASE("Scene Lights off shows wherever a light is in the scene, and its toggle reads as one")
{
    mainq::init();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    sceneLights::registerActions();
    run("newScene");
    const bool before = sceneLights::on();

    sceneLights::setOn(false);
    CHECK_FALSE(sceneLights::offWithLights()); // nothing to miss without a light

    scene::lights::create(scene::Graph::current(), "key", scene::kNoNode, {});
    CHECK(sceneLights::offWithLights());

    const Action* toggle = actions::find("viewport.toggleLighting");
    REQUIRE(toggle);
    REQUIRE(toggle->checked);
    CHECK_FALSE(toggle->checked());

    auto parent = std::make_unique<QWidget>();
    QWidget* note = sceneLights::attributeNote(parent.get());
    CHECK_FALSE(note->isHidden());
    QToolButton* turnOn = note->findChild<QToolButton*>();
    REQUIRE(turnOn);
    turnOn->click();
    pump();
    CHECK(sceneLights::on());
    CHECK(toggle->checked());
    CHECK_FALSE(sceneLights::offWithLights());
    CHECK(note->isHidden());

    actions::run(nlohmann::json("viewport.toggleLighting"));
    pump();
    CHECK_FALSE(sceneLights::on());
    CHECK_FALSE(note->isHidden());

    sceneLights::setOn(before);
}
