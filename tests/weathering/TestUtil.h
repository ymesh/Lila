#pragma once

#include "extensions/ExtensionManager.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <doctest/doctest.h>

#include <string>

namespace weatheringtest
{
using namespace lila;
using json = nlohmann::json;

inline json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

inline scene::Graph& g() { return scene::Graph::current(); }

/** Stagehand and Weathering loaded, and an empty scene. */
inline void fresh()
{
    auto& em = extensions::ExtensionManager::get();
    if (!em.isLoaded("Stagehand"))
        REQUIRE(em.load(LILA_TEST_STAGEHAND_PACKAGE) == "Stagehand");
    if (!em.isLoaded("Weathering"))
        REQUIRE(em.load(LILA_TEST_WEATHERING_PACKAGE) == "Weathering");
    run("newScene");
}

/** A box standing on y = 0 at a point, cm. */
inline std::string cube(const char* name, const glm::dvec3& at, const glm::dvec3& size)
{
    return run("polyCube", {{"name", name}, {"position", json::array({at.x, at.y, at.z})}, {"width", size.x}, {"height", size.y}, {"depth", size.z},
                            {"baseAtOrigin", true}, {"generator", false}})[0]
        .get<std::string>();
}

/** A mesh node's evaluated output, in its own space. */
inline geo::Mesh meshOf(const std::string& path)
{
    g().evaluate();
    scene::Node* n = g().find(path);
    REQUIRE(n);
    const auto m = n->as<scene::MeshPtr>(n->type().attrIndex("outMesh"));
    REQUIRE(m);
    return *m;
}
} // namespace weatheringtest
