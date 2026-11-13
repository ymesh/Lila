#pragma once

#include "ui/ViewportWidget.h"


#include "bake/Arrange.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/ColorSets.h"
#include "geo/Lasso.h"
#include "geo/Mesh.h"
#include "geo/RenderMesh.h"
#include "io/SceneFile.h"
#include "render/Backdrop.h"
#include "scene/Anim.h"
#include "scene/Camera.h"
#include "scene/ComponentPick.h"
#include "scene/Components.h"
#include "scene/DisplayLayers.h"
#include "scene/Graph.h"
#include "geo/Symmetry.h"
#include "geo/Topology.h"
#include "gn/InstanceView.h"
#include "scene/ArrayGroup.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/NodeBounds.h"
#include "scene/RenderSettings.h"
#include "scene/Skin.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/Animation.h"
#include "ui/ChannelBox.h"
#include "ui/CursorBadges.h"
#include "ui/DroneCamera.h"
#include "ui/Deletion.h"
#include "ui/NodeLookup.h"
#include "ui/DrawOverrides.h"
#include "ui/MarkingMenu.h"
#include "ui/Environments.h"
#include "ui/MaterialDraw.h"
#include "ui/Isolate.h"
#include "ui/LodDisplay.h"
#include "ui/DisplayAids.h"
#include "ui/MeshCache.h"
#include "ui/PaintSelect.h"
#include "ui/Outliner.h"
#include "ui/Overlay.h"
#include "ui/RenderPasses.h"
#include "ui/SelectionHandlers.h"
#include "ui/SurfaceSnap.h"
#include "ui/TabSelectLogic.h"
#include "ui/Theme.h"
#include "ui/Tools.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportHud.h"
#include "ui/ViewportRenderers.h"
#include "ui/ViewportTool.h"
#include "uv/UvCommands.h"

#include <QColor>
#include <QCursor>
#include <QDragEnterEvent>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QScreen>
#include <QKeyEvent>
#include <QMimeData>
#include <QApplication>
#include <QMouseEvent>
#include <QTimer>
#include <QWheelEvent>

// Qt's emit macro collides with a TBB member of the same name.
#pragma push_macro("emit")
#undef emit
#include <tbb/parallel_for.h>
#pragma pop_macro("emit")

#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <unordered_map>
#include <unordered_set>


/** Helpers shared by the ViewportWidget source files. Private to them. */
namespace lila::ui::viewportDetail
{
// Grayscale only (see plan: nothing colored near the work).
inline const glm::vec4 kLocatorColor(0.02f, 0.02f, 0.02f, 1.0f);
inline const glm::vec4 kSelectedColor(0.95f, 0.95f, 0.95f, 1.0f);
inline const glm::vec4 kChildOfSelectedColor(0.55f, 0.55f, 0.55f, 1.0f);
inline const glm::vec4 kPreselectColor(0.75f, 0.75f, 0.75f, 1.0f);
inline const glm::vec4 kTemplateWireColor(0.22f, 0.22f, 0.22f, 1.0f);
inline const glm::vec4 kSnapSurfaceWireColor(0.10f, 0.35f, 0.22f, 1.0f);

/** True when `hot`, the node under the cursor, is an array above `id`: hovering a copy highlights the whole array. */
inline bool preselectedArray(const scene::Graph& g, scene::NodeId id, scene::NodeId hot)
{
    for (scene::NodeId p = g.parent(id); p != scene::kNoNode; p = g.parent(p))
        if (p == hot)
            return scene::arrays::isArray(*g.node(p));
    return false;
}

/** Where the viewport draws and picks a node: its world matrix plus any view-only display offset (bake Arrange). */
inline glm::dmat4 shownWorld(const scene::Node& n)
{
    return bake::arrange::displayWorld(scene::Graph::current(), n.id(), n.as<glm::dmat4>(scene::Transform::kWorldMatrix));
}

/**
 * A geometry nodes result that ends in instances: d (the node's draw, without a mesh) once per referenced mesh with that
 * mesh's placements, plus the geometry beside the instances.
 */
inline void pushInstanceDraws(render::FrameState& s, const render::MeshDraw& d, const gn::InstanceView& view, bool textured)
{
    auto push = [&](const scene::MeshPtr& mesh, std::shared_ptr<const std::vector<glm::mat4>> placements) {
        const bool tangents = textured && needsTangents(*mesh);
        auto rm = renderMeshFor(mesh, tangents);
        if (!rm)
            return;
        render::MeshDraw part = d;
        part.mesh = std::move(rm);
        // Array copies of the node repeat every instance placement.
        if (d.instances && placements)
        {
            auto both = std::make_shared<std::vector<glm::mat4>>();
            both->reserve(d.instances->size() * placements->size());
            for (const glm::mat4& copy : *d.instances)
                for (const glm::mat4& p : *placements)
                    both->push_back(copy * p);
            part.instances = std::move(both);
        }
        else
            part.instances = placements ? std::move(placements) : d.instances;
        if (textured)
            part.materials = drawMaterials(*mesh);
        s.meshes.push_back(std::move(part));
    };
    if (view.base)
        push(view.base, nullptr);
    for (const gn::InstancePart& p : view.parts)
        push(p.mesh, p.transforms);
}

/**
 * Where a node's array copies draw relative to its world matrix, the node itself first, for an instanced draw. Null when
 * no array repeats it.
 */
inline std::shared_ptr<const std::vector<glm::mat4>> arrayPlacements(const scene::Graph& g, scene::NodeId id, const glm::dmat4& world)
{
    const std::vector<scene::arrays::Copy> copies = scene::arrays::copiesOf(g, id);
    if (copies.empty())
        return nullptr;
    auto out = std::make_shared<std::vector<glm::mat4>>();
    out->reserve(copies.size() + 1);
    out->push_back(glm::mat4(1.0f));
    const glm::dmat4 inv = glm::inverse(world);
    for (const scene::arrays::Copy& c : copies)
        out->push_back(glm::mat4(inv * c.transform * world));
    return out;
}

/** An unhighlighted object's line color as its display layer has it: the layer's tint, template grey, else fallback. */
inline glm::vec4 layerLineColor(const scene::Graph& g, scene::NodeId id, const glm::vec4& fallback)
{
    const scene::displayLayers::Display layer = scene::displayLayers::display(g, id);
    if (layer.color)
        return glm::vec4(viewportDisplay::layerTint(glm::vec3(*layer.color)), 1.0f);
    return layer.type == scene::displayLayers::DisplayType::Template ? kTemplateWireColor : fallback;
}

/** Linear colors for component display: Maya's hues, or greys when monotone. */
struct ComponentPalette
{
    glm::vec4 wire;     // the hilited meshes' wire; alpha 0 keeps the renderer's default
    glm::vec4 point;    // unselected vertices
    glm::vec4 selected; // selected vertices, edges and face outlines
    glm::vec4 selectedFill;
    glm::vec4 hover;
    glm::vec4 hoverFill;
};

inline ComponentPalette componentPalette()
{
    if (!viewportDisplay::accentColors(viewportDisplay::kComponentColors))
        return {glm::vec4(0.0f), glm::vec4(0.007f, 0.007f, 0.007f, 1.0f), glm::vec4(1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 0.4f),
                glm::vec4(0.5f, 0.5f, 0.5f, 1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 0.12f)};
    auto lin = [](float r, float g, float b, float a) {
        auto c = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
        return glm::vec4(c(r), c(g), c(b), a);
    };
    auto tok = [&](const char* token, float r, float g, float b, float a) {
        glm::vec4 c = themeSrgb(token, glm::vec4(r, g, b, a));
        return lin(c.r, c.g, c.b, a);
    };
    return {tok("componentWire", 0.45f, 0.78f, 1.0f, 1.0f),      tok("componentPoint", 0.95f, 0.42f, 0.78f, 1.0f),
            tok("componentSelected", 1.0f, 0.95f, 0.15f, 1.0f),  tok("componentSelected", 1.0f, 0.95f, 0.15f, 0.25f),
            tok("componentHover", 1.0f, 0.18f, 0.18f, 1.0f),     tok("componentHover", 1.0f, 0.18f, 0.18f, 0.25f)};
}

inline std::vector<scene::NodeId> hilited(const scene::isolation::Set& iso);

/** In the viewport's pick: shown, and not templated or referenced by a display layer. */
inline bool viewportPickable(const scene::Graph& g, scene::NodeId id, bool inherit)
{
    return scene::effectivelyVisible(g, id, inherit) && scene::displayLayers::pickable(g, id);
}

inline constexpr const char* kBuiltInViews[] = {"persp", "top", "bottom", "front", "back", "left", "right"};

inline bool builtInView(const std::string& name)
{
    return std::ranges::any_of(kBuiltInViews, [&](const char* v) { return name == v; });
}

inline double nowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** The attrs a navigation edit changes on a camera node, saved to put it back before the committing command. */
inline constexpr int kEditedAttrs[] = {scene::Transform::kTranslate, scene::Transform::kRotate, scene::Transform::kScale,
                                scene::Camera::kOrthographicWidth};

/** Drawn and picked by a draw override rather than as the viewport's own mesh. */
inline bool overridden(const scene::Node& n) { return drawOverrides::forType(n.type()) != nullptr; }

inline glm::vec4 srgbToLinear(const glm::vec4& c)
{
    auto lin = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return {lin(c.r), lin(c.g), lin(c.b), c.a};
}

inline const glm::vec4 kSymmetryRed = srgbToLinear({0.95f, 0.18f, 0.16f, 1.0f});

/** What a mesh's symmetry setting is, as a cache key: its layer and where its plane or axis lies. */
inline std::string symmetryKey(const geo::SymmetrySpec& spec)
{
    if (!spec.radial)
        return std::format("{}:{}", geo::symmetryLayerName(spec.axis), spec.offset);
    const glm::dvec3 o = spec.radial->origin();
    return std::format("{}:{},{},{}", geo::radialLayerName(*spec.radial), o.x, o.y, o.z);
}

/** The edges of a mesh with symmetry on that lie in its plane (radial: on its axis or mirror planes), cached per mesh and setting. */
inline const std::vector<uint32_t>& symmetryPlaneEdges(const scene::Node& n, const scene::MeshPtr& mesh)
{
    struct Cached
    {
        std::weak_ptr<const geo::Mesh> mesh;
        std::string key;
        std::vector<uint32_t> edges;
    };
    static std::unordered_map<scene::NodeId, Cached> cache;
    static const std::vector<uint32_t> none;
    const std::optional<geo::SymmetrySpec> spec = scene::symmetrySpec(n);
    if (!spec || !mesh)
        return none;
    const std::string key = symmetryKey(*spec);
    Cached& c = cache[n.id()];
    if (c.mesh.lock() == mesh && c.key == key)
        return c.edges;
    const geo::Mesh& m = *mesh;
    const double tol = std::max(1e-4, glm::length(m.bounds().size()) * 1e-5);
    c = {mesh, key, {}};
    // The stored pairs mark the on-plane points even after they drift; without them, go by position.
    std::span<const int32_t> keys = spec->radial ? std::span<const int32_t>() : m.pointAttrs().get<int32_t>(geo::symmetryLayerName(spec->axis));
    auto on = [&](geo::Index p) {
        if (const std::optional<geo::RadialSymmetry>& r = spec->radial)
        {
            if (r->axisDistance(m.position(p)) <= tol)
                return true;
            for (int k = 0; r->mirror() && k < r->count(); ++k)
                if (std::abs(r->mirrorPlane(r->count() + k).distance(m.position(p))) <= tol)
                    return true;
            return false;
        }
        return keys.empty() ? std::abs(m.position(p)[spec->axis] - spec->offset) <= tol : keys[p] == geo::kSymmetryOnPlane;
    };
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && on(m.edgePoint(e, 0)) && on(m.edgePoint(e, 1)))
            c.edges.push_back(e);
    return c.edges;
}

/** Opacity of the mirror images of hovered and selected components while symmetry is on. */
inline constexpr float kMirrorAlpha = 0.22f;

/** The mirror map (or radial orbits) of a mesh with symmetry on, null when off. Moving points keeps the pairs, so only topology rebuilds it. */
inline const geo::SymmetryMap* viewSymmetry(const scene::Node& n, const scene::MeshPtr& mesh)
{
    struct Cached
    {
        geo::Index points = 0, edges = 0, faces = 0;
        std::string key;
        std::optional<geo::SymmetryMap> map;
    };
    static std::unordered_map<scene::NodeId, Cached> cache;
    const std::optional<geo::SymmetrySpec> spec = scene::symmetrySpec(n);
    if (!spec || !mesh)
        return nullptr;
    const std::string key = symmetryKey(*spec);
    const geo::Mesh& m = *mesh;
    Cached& c = cache[n.id()];
    if (!c.map || c.key != key || c.points != m.pointCapacity() || c.edges != m.edgeCapacity() || c.faces != m.faceCapacity())
    {
        geo::Mesh copy = m;
        c = {m.pointCapacity(), m.edgeCapacity(), m.faceCapacity(), key,
             geo::SymmetryMap::persistent(copy, *spec, std::max(1e-4, glm::length(m.bounds().size()) * 1e-5))};
    }
    return &*c.map;
}

inline const glm::vec4 kWedgePlaneGray = srgbToLinear({0.62f, 0.62f, 0.62f, 0.55f});

/**
 * A radially symmetric mesh's wedge boundaries (with mirror, its mirror planes) as half-plane outlines from the axis out
 * past the mesh, spanning its height along the axis. Nothing for other meshes.
 */
inline void radialPlaneLines(const scene::Node& n, const geo::Mesh& m, const glm::dmat4& world, std::vector<render::LineVertex>& out)
{
    const std::optional<geo::SymmetrySpec> spec = scene::symmetrySpec(n);
    if (!spec || !spec->radial || m.numPoints() == 0)
        return;
    const geo::RadialSymmetry& r = *spec->radial;
    double lo = std::numeric_limits<double>::max(), hi = -lo, reach = 0.0;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            const double h = glm::dot(m.position(p) - r.origin(), r.axis());
            lo = std::min(lo, h), hi = std::max(hi, h), reach = std::max(reach, r.axisDistance(m.position(p)));
        }
    reach *= 1.15;
    auto wp = [&](const glm::dvec3& p) { return glm::vec3(world * glm::dvec4(p, 1.0)); };
    auto line = [&](const glm::dvec3& a, const glm::dvec3& b) {
        out.push_back({wp(a), kWedgePlaneGray});
        out.push_back({wp(b), kWedgePlaneGray});
    };
    const glm::dvec3 bottom = r.origin() + r.axis() * lo, top = r.origin() + r.axis() * hi;
    line(bottom, top);
    const glm::dvec3 v = glm::cross(r.axis(), r.reference());
    const int planes = r.mirror() ? 2 * r.count() : r.count();
    for (int k = 0; k < planes; ++k)
    {
        const double t = 2.0 * 3.14159265358979323846 * k / planes;
        const glm::dvec3 out2 = (std::cos(t) * r.reference() + std::sin(t) * v) * reach;
        line(bottom, bottom + out2);
        line(top, top + out2);
        line(bottom + out2, top + out2);
    }
}

inline bool sameHighlight(const std::optional<selectionHandlers::Highlight>& a, const std::optional<selectionHandlers::Highlight>& b)
{
    if (!a || !b)
        return !a && !b;
    return a->mesh == b->mesh && a->domain == b->domain && a->color == b->color && a->weights == b->weights;
}

/** Meshes components can be picked on: the selected ones (Maya's hilite) and those under selected groups, or every visible mesh if nothing selected has components. */
inline std::vector<scene::NodeId> hilited(const scene::isolation::Set& iso)
{
    scene::Graph& g = scene::Graph::current();
    std::vector<scene::NodeId> out;
    const bool inherit = scene::inheritVisibility();
    // Meshes under a selected group count too; hidden ones only when selected themselves.
    for (scene::NodeId id : uv::meshesUnder(g, g.selection()))
        if ((std::ranges::find(g.selection(), id) != g.selection().end() || viewportPickable(g, id, inherit)) && iso.shows(id))
            out.push_back(id);
    if (out.empty() && !scene::components::selectionHasPointNodes(g))
        for (scene::NodeId id : g.nodes())
            if (scene::Node* n = g.node(id); n->type().isA("mesh") && viewportPickable(g, id, inherit) && iso.shows(id))
                out.push_back(id);
    return out;
}

/** Selected nodes whose points a provider gives (curves), and those already holding a point selection. */
inline std::vector<scene::NodeId> pointNodes(const scene::isolation::Set& iso)
{
    scene::Graph& g = scene::Graph::current();
    std::vector<scene::NodeId> out;
    auto add = [&](scene::NodeId id) {
        scene::Node* n = g.node(id);
        if (n && scene::components::pointProvider(*n) && iso.shows(id) && std::ranges::find(out, id) == out.end())
            out.push_back(id);
    };
    for (scene::NodeId id : g.selection())
        add(id);
    for (auto& [id, ids] : scene::components::selected(scene::ComponentMode::Vertex))
        add(id);
    return out;
}

/**
 * Where a mesh's components draw and pick, world space: on its cage, or along the surface of its smooth preview when it
 * has one and is edited directly (modifiers put the cage somewhere else).
 */
struct ComponentShape
{
    const geo::Mesh& cage;
    glm::dmat4 world;
    std::shared_ptr<const SmoothCage> smooth;
    int level = 0;

    ComponentShape(const scene::Node& n, const geo::Mesh& cageMesh, const scene::MeshPtr& cagePtr, const glm::dmat4& w) : cage(cageMesh), world(w)
    {
        if (n.type().isA("mesh") && cagePtr == n.as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
            level = int(n.as<int64_t>(scene::MeshNode::kSmoothPreview));
        smooth = smoothCageFor(cagePtr, level);
        if (!smooth)
            level = 0;
    }

    /** The mesh components are picked on: the smooth one, whose ids map back with cageOf. */
    const geo::Mesh& picked() const { return smooth ? *smooth->mesh : cage; }
    /** A component of picked() as the cage's, kInvalid for one that is not on the cage. */
    uint32_t cageOf(scene::ComponentMode mode, uint32_t id) const
    {
        if (!smooth)
            return id;
        const std::vector<geo::Index>& map = mode == scene::ComponentMode::Vertex ? smooth->cagePoints
                                             : mode == scene::ComponentMode::Edge ? smooth->map.edges
                                                                                  : smooth->map.faces;
        return id < map.size() ? map[id] : geo::kInvalid;
    }

    glm::dvec3 at(const geo::Mesh& m, geo::Index p) const { return glm::dvec3(world * glm::dvec4(m.position(p), 1.0)); }
    glm::dvec3 point(uint32_t p) const { return smooth ? at(*smooth->mesh, smooth->map.points[p]) : at(cage, p); }
    /** A point on the surface along the edge, for occlusion tests. */
    glm::dvec3 edgeAnchor(uint32_t e) const
    {
        if (!smooth || smooth->segments(e).empty())
            return (point(cage.edgePoint(e, 0)) + point(cage.edgePoint(e, 1))) * 0.5;
        std::span<const geo::Index> segs = smooth->segments(e);
        return at(*smooth->mesh, smooth->mesh->edgePoint(segs[segs.size() / 2], 0));
    }
    glm::dvec3 faceCentre(uint32_t f) const
    {
        return smooth ? at(*smooth->mesh, smooth->map.facePoints[f]) : glm::dvec3(world * glm::dvec4(cage.faceCentroid(f), 1.0));
    }
    /** fn(a, b) for each straight piece of edge e. */
    template <class Fn> void forEachSegment(uint32_t e, Fn&& fn) const
    {
        if (!smooth)
            return fn(point(cage.edgePoint(e, 0)), point(cage.edgePoint(e, 1)));
        const geo::Mesh& sm = *smooth->mesh;
        for (geo::Index s : smooth->segments(e))
            fn(at(sm, sm.edgePoint(s, 0)), at(sm, sm.edgePoint(s, 1)));
    }
    /** fn(a, b) for each straight piece of face f's border. */
    template <class Fn> void forEachOutline(uint32_t f, Fn&& fn) const
    {
        cage.forEachFaceCorner(f, [&](geo::Index c) { forEachSegment(cage.cornerEdge(c), fn); });
    }
    /** fn(p) for each corner of the triangles filling face f, three per triangle. */
    template <class Fn> void forEachFillVertex(uint32_t f, Fn&& fn) const
    {
        if (!smooth)
        {
            for (uint32_t p : scene::componentPick::fillTriangles(cage, f))
                fn(at(cage, p));
            return;
        }
        for (geo::Index part : smooth->parts(f))
            for (uint32_t p : scene::componentPick::fillTriangles(*smooth->mesh, part))
                fn(at(*smooth->mesh, p));
    }
    /** Screen-space polygons covering face f: the face itself, or each smoothed piece of it. */
    template <class Fn> void forEachPolygon(uint32_t f, Fn&& fn) const
    {
        auto corners = [&](const geo::Mesh& m, geo::Index face) {
            std::vector<glm::dvec3> poly;
            m.forEachFaceCorner(face, [&](geo::Index c) { poly.push_back(at(m, m.cornerPoint(c))); });
            fn(poly);
        };
        if (!smooth)
            return corners(cage, f);
        for (geo::Index part : smooth->parts(f))
            corners(*smooth->mesh, part);
    }
};

/** Whether the node's surface as drawn, smooth preview included, is hit along the ray nearer than dist. */
inline bool occludes(const isolate::Set* set, scene::NodeId id, const glm::dvec3& eye, const glm::dvec3& dir, double dist)
{
    scene::Node* n = scene::Graph::current().node(id);
    const int level = n->type().isA("mesh") ? int(n->as<int64_t>(scene::MeshNode::kSmoothPreview)) : 0;
    scene::MeshPtr shown = smoothedFor(n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh), level);
    auto hit = isolate::raycast(set, id, shown, shownWorld(*n), eye, dir, level);
    return hit && hit->distance < dist * (1.0 - 1e-4) - 1e-3;
}

/** Provider points of the node, world space, with their component ids. */
inline std::vector<std::pair<uint32_t, glm::dvec3>> worldPoints(scene::Graph& g, scene::NodeId id)
{
    scene::Node* n = g.node(id);
    const scene::components::PointProvider* pp = n ? scene::components::pointProvider(*n) : nullptr;
    if (!pp)
        return {};
    glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    auto pts = pp->points(g, id);
    for (auto& [cid, p] : pts)
        p = glm::dvec3(w * glm::dvec4(p, 1.0));
    return pts;
}
} // namespace lila::ui::viewportDetail
