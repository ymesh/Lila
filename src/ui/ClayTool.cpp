#include "ui/ClayUi.h"

#include "clay/ClayNodes.h"
#include "core/Log.h"
#include "geo/Bvh.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"
#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/Controls.h"
#include "ui/DrawOverrides.h"
#include "ui/Icons.h"
#include "ui/Panels.h"
#include "ui/SpinScrub.h"
#include "ui/Tools.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <map>

namespace lila::ui
{
namespace
{
using scene::Graph;
using scene::NodeId;

constexpr std::array<const char*, 7> kToolShapes = {"sphere", "ellipsoid", "box", "capsule", "cylinder", "cone", "torus"};
const glm::vec4 kAddColor{0.55f, 0.85f, 1.0f, 1.0f};
const glm::vec4 kCutColor{1.0f, 0.55f, 0.45f, 1.0f};

json callClay(const char* name, json flags)
{
    try
    {
        return ops::execute(name, flags, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return nullptr;
    }
}

json jsonVec(const glm::dvec3& v) { return {v.x, v.y, v.z}; }

using Polyline = std::vector<glm::dvec3>;

/** A ring in the plane of axes a and b (0-2) at offset along the third, radii ra and rb, from angle t0 to t1. */
Polyline ring(int a, int b, double ra, double rb, glm::dvec3 centre, double t0 = 0.0, double t1 = glm::two_pi<double>())
{
    Polyline out;
    for (int i = 0; i <= 32; ++i)
    {
        const double t = t0 + (t1 - t0) * i / 32.0;
        glm::dvec3 p = centre;
        p[a] += ra * std::cos(t);
        p[b] += rb * std::sin(t);
        out.push_back(p);
    }
    return out;
}

/** A clay shape's outline in its own space, the same size the surface gives it. */
std::vector<Polyline> outline(const clay::Primitive& prim)
{
    const glm::dvec3 e = prim.extents;
    const double r = prim.radius, r1 = prim.endRadius, half = 0.5 * prim.length;
    const glm::dvec3 o(0.0), lo(0.0, -half, 0.0), hi(0.0, half, 0.0);
    const double pi = glm::pi<double>();
    switch (int(prim.shape))
    {
    case 0: return {ring(0, 1, r, r, o), ring(1, 2, r, r, o), ring(0, 2, r, r, o)};
    case 1: return {ring(0, 1, e.x, e.y, o), ring(1, 2, e.y, e.z, o), ring(0, 2, e.x, e.z, o)};
    case 2:
    {
        std::vector<Polyline> out;
        for (int axis = 0; axis < 3; ++axis)
            for (int k = 0; k < 4; ++k)
            {
                const int u = (axis + 1) % 3, v = (axis + 2) % 3;
                glm::dvec3 from(0.0), to(0.0);
                from[axis] = -e[axis], to[axis] = e[axis];
                from[u] = to[u] = (k & 1) ? e[u] : -e[u];
                from[v] = to[v] = (k & 2) ? e[v] : -e[v];
                out.push_back({from, to});
            }
        return out;
    }
    case 3:
        return {ring(0, 2, r, r, lo), ring(0, 2, r1, r1, hi), {lo + glm::dvec3(r, 0, 0), hi + glm::dvec3(r1, 0, 0)},
                {lo - glm::dvec3(r, 0, 0), hi - glm::dvec3(r1, 0, 0)}, {lo + glm::dvec3(0, 0, r), hi + glm::dvec3(0, 0, r1)},
                {lo - glm::dvec3(0, 0, r), hi - glm::dvec3(0, 0, r1)}, ring(0, 1, r, r, lo, pi, 2.0 * pi), ring(2, 1, r, r, lo, pi, 2.0 * pi),
                ring(0, 1, r1, r1, hi, 0.0, pi), ring(2, 1, r1, r1, hi, 0.0, pi)};
    case 4:
    case 5:
    {
        const double top = prim.shape == clay::Shape::Cylinder ? r : r1;
        return {ring(0, 2, r, r, lo), ring(0, 2, top, top, hi), {lo + glm::dvec3(r, 0, 0), hi + glm::dvec3(top, 0, 0)},
                {lo - glm::dvec3(r, 0, 0), hi - glm::dvec3(top, 0, 0)}, {lo + glm::dvec3(0, 0, r), hi + glm::dvec3(0, 0, top)},
                {lo - glm::dvec3(0, 0, r), hi - glm::dvec3(0, 0, top)}};
    }
    case 6:
        return {ring(0, 2, r + r1, r + r1, o), ring(0, 2, r - r1, r - r1, o), ring(0, 2, r, r, glm::dvec3(0, r1, 0)),
                ring(0, 2, r, r, glm::dvec3(0, -r1, 0))};
    }
    return {};
}

/** A shown clay shape or link as it joins the clay, in world space; nothing for a link missing an end. */
std::optional<std::pair<clay::Primitive, glm::dmat4>> placedShape(const scene::Node& n)
{
    const scene::DataPtr d = n.as<scene::DataPtr>(n.type().attrIndex("outClay"));
    if (!d || std::string_view(d->kind()) != "clayItem")
        return std::nullopt;
    const clay::Item& item = static_cast<const clay::ItemData&>(*d).item;
    if (!item.shape)
        return std::nullopt;
    return std::pair{*item.shape, item.matrix};
}

/** The clay objects whose shapes show: each one selected or holding a selected shape. */
std::vector<NodeId> shownObjects()
{
    std::vector<NodeId> objects;
    for (NodeId id : Graph::current().selection())
        if (scene::Node* o = clay::objectOf(id); o && std::ranges::find(objects, o->id()) == objects.end())
            objects.push_back(o->id());
    return objects;
}

/** The clay shapes whose outlines show: those of every clay object selected or holding a selected shape, or under the Clay tool. */
std::vector<scene::Node*> shownShapes()
{
    Graph& g = Graph::current();
    const std::vector<NodeId> objects = shownObjects();
    std::vector<scene::Node*> out;
    std::vector<NodeId> stack(objects.rbegin(), objects.rend());
    while (!stack.empty())
    {
        const NodeId id = stack.back();
        stack.pop_back();
        for (NodeId c : g.children(id))
            if (scene::Node* n = g.node(c))
            {
                if ((n->type().isA("clayPrimitive") || n->type().isA("clayLink")) && scene::effectivelyVisible(g, c))
                    out.push_back(n);
                if (n->type().isA("clayGroup"))
                    stack.push_back(c);
            }
    }
    return out;
}

void drawShapes(render::FrameState& f, OverlayPainter&, const ViewInfo&)
{
    Graph& g = Graph::current();
    const auto& selection = g.selection();
    for (scene::Node* n : shownShapes())
    {
        const bool selected = std::ranges::find(selection, n->id()) != selection.end();
        const bool cut = n->as<int64_t>(n->type().attrIndex("operation")) != 0;
        const glm::vec4 color = selected ? glm::vec4(1.0f, 0.85f, 0.3f, 1.0f) : cut ? glm::vec4(1.0f, 0.4f, 0.35f, 0.9f) : glm::vec4(0.55f, 0.8f, 1.0f, 0.45f);
        const auto shape = placedShape(*n);
        if (!shape)
            continue;
        const glm::dmat4& world = shape->second;
        for (const Polyline& line : outline(shape->first))
            for (size_t i = 1; i < line.size(); ++i)
            {
                f.linesOnTop.push_back({glm::dvec3(world * glm::dvec4(line[i - 1], 1.0)), color});
                f.linesOnTop.push_back({glm::dvec3(world * glm::dvec4(line[i], 1.0)), color});
            }
    }
}

std::vector<QPointF> circlePx(QPointF c, double r)
{
    std::vector<QPointF> out;
    for (int i = 0; i < 48; ++i)
    {
        const double a = glm::two_pi<double>() * i / 48.0;
        out.emplace_back(c.x() + r * std::cos(a), c.y() + r * std::sin(a));
    }
    return out;
}

double segmentPx(QPointF p, QPointF a, QPointF b)
{
    const glm::dvec2 ab(b.x() - a.x(), b.y() - a.y()), ap(p.x() - a.x(), p.y() - a.y());
    const double t = std::clamp(glm::dot(ap, ab) / std::max(glm::dot(ab, ab), 1e-12), 0.0, 1.0);
    return glm::length(ap - t * ab);
}

/** Clicking a shown outline picks its shape, so cuts, which leave no surface of their own, can be selected. */
std::vector<viewportHooks::PickHit> pickShapes(QPointF a, QPointF b, bool click, const ViewInfo& view)
{
    std::vector<viewportHooks::PickHit> hits;
    if (!click)
        return hits;
    (void)b;
    for (scene::Node* n : shownShapes())
    {
        const auto shape = placedShape(*n);
        if (!shape)
            continue;
        const glm::dmat4& world = shape->second;
        double best = 1e300;
        for (const Polyline& line : outline(shape->first))
            for (size_t i = 1; i < line.size(); ++i)
            {
                auto p0 = view.project(glm::dvec3(world * glm::dvec4(line[i - 1], 1.0)));
                auto p1 = view.project(glm::dvec3(world * glm::dvec4(line[i], 1.0)));
                if (p0 && p1)
                    best = std::min(best, segmentPx(a, *p0, *p1));
            }
        if (best < 5.0)
            hits.push_back({n->path(), best});
    }
    if (!hits.empty())
        return hits;
    // A click on the surface of clay whose shapes show picks the shape or link under it, not the clay's mesh.
    Graph& g = Graph::current();
    std::optional<std::pair<NodeId, glm::dvec3>> nearest;
    double nearestT = 1e300;
    for (NodeId object : shownObjects())
        for (NodeId c : g.children(object))
            if (scene::Node* n = g.node(c); n && n->type().isA("mesh"))
                if (scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh); m && m->faceCapacity() > 0)
                {
                    static geo::MeshBvh bvh;
                    static scene::MeshPtr built;
                    if (built != m)
                    {
                        bvh.build(*m);
                        built = m;
                    }
                    const glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix), inv = glm::inverse(world);
                    glm::dvec3 o, d;
                    view.ray(a, o, d);
                    geo::BvhHit hit;
                    if (bvh.raycast(glm::dvec3(inv * glm::dvec4(o, 1.0)), glm::dvec3(inv * glm::dvec4(d, 0.0)), hit))
                    {
                        const glm::dvec3 at(world * glm::dvec4(hit.point, 1.0));
                        if (const double t = glm::length(at - o); t < nearestT)
                        {
                            nearestT = t;
                            nearest = std::pair{object, at};
                        }
                    }
                }
    if (nearest)
        try
        {
            if (json shape = ops::execute("clayShapeAt", {{"args", {g.node(nearest->first)->path()}}, {"point", jsonVec(nearest->second)}}); shape.is_string())
                hits.push_back({shape.get<std::string>(), 5.0});
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    return hits;
}

/** What the Clay tool places next. Shared by the tool and its palette, and kept between uses of the tool. */
struct ClaySettings
{
    int shape = 0;       // into kToolShapes
    bool stroke = false; // capsule strokes instead of shapes
    bool taper = true;
    bool chain = false;     // each new shape links to the selected one
    double thickness = 1.0; // of a link's ends, as a fraction of their shapes' sizes
    bool cut = false; // Ctrl gives the other
    double radius = 5.0, blend = 2.0;
    double lastRadius = 0.0; // of the last shape placed; 0 before the first
};

ClaySettings& claySettings()
{
    static ClaySettings s;
    return s;
}

constexpr const char* kPaletteId = "clayPalette";

void settingsChanged();

/** The clay object of the selection, or null. */
scene::Node* selectedClay()
{
    for (NodeId id : Graph::current().selection())
        if (scene::Node* o = clay::objectOf(id))
            return o;
    return nullptr;
}

/** The clay object of the selection as commands take it, or empty. */
std::string selectedClayPath()
{
    scene::Node* o = selectedClay();
    return o ? o->path() : std::string();
}

/**
 * The signed world distance from p to the selected clay's mirror plane (its YZ plane), with the plane's normal, or
 * nothing when the clay isn't mirrored.
 */
std::optional<std::pair<double, glm::dvec3>> mirrorOffset(const glm::dvec3& p)
{
    scene::Node* o = selectedClay();
    const scene::Node* surface = o ? clay::surfaceOf(*o) : nullptr;
    if (!surface || surface->as<int64_t>(surface->type().attrIndex("symmetry")) != 1)
        return std::nullopt;
    const glm::dmat4 world = o->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    const glm::dvec3 n = glm::cross(glm::dvec3(world[1]), glm::dvec3(world[2]));
    if (glm::length(n) < 1e-12)
        return std::nullopt;
    const glm::dvec3 unit = glm::normalize(n);
    return std::pair{glm::dot(p - glm::dvec3(world[3]), unit), unit};
}

/** How near the mirror plane a shape of radius r snaps onto it: where it would overlap its mirror, wider with X held. */
double mirrorReach(double r)
{
    return tools::snaps() & SnapGrid ? 4.0 * r : r;
}

/** p moved onto the mirror plane when it is within reach of it for a shape of radius r. */
bool snapToMirror(glm::dvec3& p, double r)
{
    if (auto off = mirrorOffset(p); off && std::abs(off->first) < mirrorReach(r))
    {
        p -= off->first * off->second;
        return true;
    }
    return false;
}

/** A stroke span's far end, where a new stroke continues it. */
struct StrokeEnd
{
    NodeId span = scene::kNoNode;
    glm::dvec3 at{0.0};
    double radius = 0.0;
};

/** The stroke end of the selected clay nearest p, when p is within reach of it. */
std::optional<StrokeEnd> strokeEndNear(const glm::dvec3& p)
{
    scene::Node* object = selectedClay();
    if (!object)
        return std::nullopt;
    Graph& g = Graph::current();
    std::optional<StrokeEnd> best;
    double bestDistance = 1e300;
    std::vector<NodeId> stack(g.children(object->id()).begin(), g.children(object->id()).end());
    while (!stack.empty())
    {
        scene::Node* n = g.node(stack.back());
        stack.pop_back();
        if (!n)
            continue;
        if (n->type().isA("clayGroup"))
        {
            stack.insert(stack.end(), g.children(n->id()).begin(), g.children(n->id()).end());
            continue;
        }
        // Strokes are capsule chains in a group of their own.
        const scene::Node* parent = g.node(g.parent(n->id()));
        if (!n->type().isA("clayPrimitive") || !parent || !parent->type().isA("clayGroup") || n->as<int64_t>(n->type().attrIndex("shape")) != 3)
            continue;
        const glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        const double half = 0.5 * n->as<double>(n->type().attrIndex("length"));
        const glm::dvec3 end(world * glm::dvec4(0.0, half, 0.0, 1.0));
        const double r = n->as<double>(n->type().attrIndex("endRadius")) * glm::length(glm::dvec3(world[1]));
        const double d = glm::length(p - end);
        if (d <= 1.5 * r + 1e-6 && d < bestDistance)
        {
            bestDistance = d;
            best = StrokeEnd{n->id(), end, std::max(0.05, r)};
        }
    }
    return best;
}

/** In Chain mode, the selected clay shape the next shape links to, or null. */
scene::Node* chainParent()
{
    if (!claySettings().chain || claySettings().stroke)
        return nullptr;
    Graph& g = Graph::current();
    if (g.selection().size() != 1)
        return nullptr;
    scene::Node* n = g.node(g.selection().front());
    return n && n->type().isA("clayPrimitive") ? n : nullptr;
}

/** The selected clay links, whose thickness the palette shows and edits. */
std::vector<scene::Node*> selectedLinks()
{
    Graph& g = Graph::current();
    std::vector<scene::Node*> out;
    for (NodeId id : g.selection())
        if (scene::Node* n = g.node(id); n && n->type().isA("clayLink"))
            out.push_back(n);
    return out;
}

glm::dvec3 centreOf(const scene::Node& n) { return glm::dvec3(n.as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]); }

/** The Clay tool's keys, with the ones that only matter now. */
std::vector<KeyHint> clayKeys()
{
    const ClaySettings& s = claySettings();
    std::vector<KeyHint> keys = {{"Drag", s.stroke ? (s.cut ? "Cut stroke" : "Stroke") : s.cut ? "Cut" : "Add"},
                                 {"Ctrl+drag", s.cut ? "Add" : "Cut"},
                                 {"Shift+click", s.chain && !s.stroke ? "Chain from a shape" : "Select a shape"},
                                 {"Drag, Shift", "Last shape's radius"},
                                 {"S", "Strokes"},
                                 {"C", s.chain ? "Chain off" : "Chain"}};
    if (const scene::Node* o = selectedClay(); o && mirrorOffset(glm::dvec3(o->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3])))
        keys.push_back({"X+click", "Snap to mirror"});
    if (s.stroke)
        keys.push_back({"T", s.taper ? "Taper off" : "Taper on"});
    keys.insert(keys.end(), {{"B+drag", "Radius"}, {"[  ]", "Smaller, larger"}, {",  .", "Blend"}, {"Esc", "Exit"}});
    return keys;
}

struct ShapeButton
{
    const char* icon;
    const char* label;
    const char* key;
};
constexpr std::array<ShapeButton, 8> kShapeButtons = {{{"sphere", "Sphere", nullptr},
                                                       {"oval", "Ellipsoid", nullptr},
                                                       {"cube", "Box", nullptr},
                                                       {"capsule", "Capsule", nullptr},
                                                       {"cylinder", "Cylinder", nullptr},
                                                       {"cone", "Cone", nullptr},
                                                       {"lifebuoy", "Torus", nullptr},
                                                       {"scribble", "Stroke", "S"}}};

/** The Clay tool's palette: what the next click places, and the selected clay's mirror. Shown only while the tool is. */
class ClayPalette final : public QWidget
{
public:
    explicit ClayPalette(QWidget* parent) : QWidget(parent)
    {
        constexpr const char* kSections = "clayPalette";
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 8, 12, 8);
        v->setSpacing(4);

        mOperation = new controls::Segmented(this);
        mOperation->add("Add", {}, "Add: shapes add to the clay. Hold Ctrl to cut once.", 0);
        mOperation->add("Cut", {}, "Cut: shapes carve the clay. Hold Ctrl to add once.", 1);
        connect(mOperation, &controls::Segmented::picked, this, [](int id) {
            claySettings().cut = id == 1;
            settingsChanged();
        });
        v->addWidget(mOperation);

        std::vector<QWidget*> tiles;
        for (size_t i = 0; i < kShapeButtons.size(); ++i)
        {
            const ShapeButton& def = kShapeButtons[i];
            auto* b = new QToolButton;
            b->setIcon(icon(def.icon));
            b->setIconSize(QSize(24, 24));
            b->setText(def.label);
            b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            b->setCheckable(true);
            b->setFocusPolicy(Qt::NoFocus);
            controls::setRole(b, "tile");
            b->setToolTip(def.key ? QString("%1  (%2)").arg(def.label, def.key) : QString(def.label));
            connect(b, &QToolButton::clicked, this, [i] {
                ClaySettings& s = claySettings();
                s.stroke = i == kToolShapes.size();
                if (!s.stroke)
                    s.shape = int(i);
                settingsChanged();
            });
            tiles.push_back(b);
            mShapes[i] = b;
        }
        v->addWidget(controls::section("Shape", controls::grid(tiles, 4, this), this, kSections));

        auto* sizeBox = new QWidget(this);
        auto* form = new QFormLayout(sizeBox);
        form->setContentsMargins(0, 0, 0, 0);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setVerticalSpacing(4);
        auto spin = [&](const char* label, double lo, const char* tip, double ClaySettings::*field) {
            QDoubleSpinBox* s = controls::valueField(lo, 1e4, 2, "cm", sizeBox);
            s->setToolTip(tip);
            setSpinDefault(s, ClaySettings{}.*field);
            connect(s, &QDoubleSpinBox::valueChanged, this, [field](double value) {
                claySettings().*field = value;
                settingsChanged();
            });
            form->addRow(label, s);
            return s;
        };
        mRadius = spin("Radius", 0.05, "Radius: size of the next shape or stroke  (B+drag, [ ])", &ClaySettings::radius);
        mBlend = spin("Blend", 0.0, "Blend: how far the next shape melts into the clay  (, .)", &ClaySettings::blend);
        mThickness = new controls::SliderField(sizeBox);
        mThickness->setRange(0.05, 1.0);
        mThickness->setSingleStep(0.05);
        mThickness->setDecimals(2);
        mThickness->setToolTip("Thickness: link radius at each end, as a fraction of that shape's size: the selected links', else the next one's");
        setSpinDefault(mThickness, ClaySettings{}.thickness);
        connect(mThickness, &QDoubleSpinBox::valueChanged, this, [](double value) {
            const std::vector<scene::Node*> links = selectedLinks();
            if (links.empty())
                claySettings().thickness = value;
            else
            {
                UndoStack::get().openChunk("Link Thickness");
                for (scene::Node* n : links)
                    callClay("setAttr", {{"args", {n->path() + ".thickness", value}}});
                UndoStack::get().closeChunk();
            }
            settingsChanged();
        });
        form->addRow("Thickness", mThickness);
        v->addWidget(controls::section("Size", sizeBox, this, kSections));

        auto* optionsBox = new QWidget(this);
        auto* options = new QVBoxLayout(optionsBox);
        options->setContentsMargins(0, 0, 0, 0);
        options->setSpacing(4);
        mTaper = new QCheckBox("Taper", optionsBox);
        mTaper->setToolTip("Taper: strokes thin toward their end  (T)");
        // Toggled rather than clicked so a middle-click reset applies too; sync() setting the current state is skipped.
        connect(mTaper, &QCheckBox::toggled, this, [](bool on) {
            if (on == claySettings().taper)
                return;
            claySettings().taper = on;
            settingsChanged();
        });
        options->addWidget(mTaper);
        mChain = new QCheckBox("Chain", optionsBox);
        mChain->setToolTip("Chain: each shape links to the selected one; Shift-click a shape to chain from it  (C)");
        connect(mChain, &QCheckBox::toggled, this, [](bool on) {
            if (on == claySettings().chain)
                return;
            claySettings().chain = on;
            settingsChanged();
        });
        options->addWidget(mChain);
        mMirror = new QCheckBox("Mirror X", optionsBox);
        connect(mMirror, &QCheckBox::toggled, this, [](bool on) {
            if (scene::Node* o = selectedClay())
                if (scene::Node* surface = clay::surfaceOf(*o); surface && on != (surface->as<int64_t>(surface->type().attrIndex("symmetry")) == 1))
                    callClay("setAttr", {{"args", {surface->path() + ".symmetry", on ? "x" : "off"}}});
        });
        options->addWidget(mMirror);
        mKeys = new QCheckBox("Show Keys", optionsBox);
        mKeys->setToolTip("Show Keys: list the tool's keys in the corner of the view.");
        connect(mKeys, &QCheckBox::toggled, this, [](bool on) {
            if (on == PrefStore::get().value<bool>("general", kShowKeysPref, true))
                return;
            PrefStore::get().set("general", kShowKeysPref, on);
            viewportTools::requestRedraw();
        });
        options->addWidget(mKeys);
        setCheckDefault(mTaper, ClaySettings{}.taper);
        setCheckDefault(mChain, ClaySettings{}.chain);
        setCheckDefault(mMirror, false);
        setCheckDefault(mKeys, true);
        v->addWidget(controls::section("Options", optionsBox, this, kSections));
        v->addStretch();

        mRefresh = new QTimer(this);
        mRefresh->setSingleShot(true);
        mRefresh->setInterval(60);
        connect(mRefresh, &QTimer::timeout, this, [this] { sync(); });
        mGraphListener = Graph::current().addListener([this](scene::Change, NodeId) { mRefresh->start(); });
        sync();
    }

    ~ClayPalette() override { Graph::current().removeListener(mGraphListener); }

    void sync()
    {
        const ClaySettings& s = claySettings();
        mOperation->setCurrent(s.cut ? 1 : 0);
        const size_t current = s.stroke ? kToolShapes.size() : size_t(s.shape);
        for (size_t i = 0; i < mShapes.size(); ++i)
            mShapes[i]->setChecked(i == current);
        mTaper->setChecked(s.taper);
        mTaper->setEnabled(s.stroke);
        mChain->setChecked(s.chain);
        mChain->setEnabled(!s.stroke);
        const std::vector<scene::Node*> links = selectedLinks();
        mThickness->setEnabled(!links.empty() || (s.chain && !s.stroke));
        const double thickness = links.empty() ? s.thickness : links.front()->as<double>(links.front()->type().attrIndex("thickness"));
        for (auto [box, value] : {std::pair<QDoubleSpinBox*, double>{mRadius, s.radius}, std::pair<QDoubleSpinBox*, double>{mBlend, s.blend},
                                  std::pair<QDoubleSpinBox*, double>{mThickness, thickness}})
        {
            const QSignalBlocker block(box);
            box->setValue(value);
        }
        const scene::Node* o = selectedClay();
        const scene::Node* surface = o ? clay::surfaceOf(*o) : nullptr;
        mMirror->setEnabled(surface);
        mMirror->setChecked(surface && surface->as<int64_t>(surface->type().attrIndex("symmetry")) == 1);
        mMirror->setToolTip(surface ? QString("Mirror X: mirror %1 across its YZ plane.").arg(QString::fromStdString(o->name())) : "Mirror X: select clay to mirror it.");
        mKeys->setChecked(PrefStore::get().value<bool>("general", kShowKeysPref, true));
    }

private:
    controls::Segmented* mOperation = nullptr;
    std::array<QToolButton*, kShapeButtons.size()> mShapes{};
    QCheckBox *mTaper = nullptr, *mChain = nullptr, *mMirror = nullptr, *mKeys = nullptr;
    QDoubleSpinBox *mRadius = nullptr, *mBlend = nullptr;
    controls::SliderField* mThickness = nullptr;
    QTimer* mRefresh = nullptr;
    int mGraphListener = 0;
};

QPointer<ClayPalette> gPalette;

void settingsChanged()
{
    if (gPalette)
        gPalette->sync();
    viewportTools::requestRedraw();
}

QDockWidget* paletteDock()
{
    for (QWidget* w = gPalette; w; w = w->parentWidget())
        if (auto* d = qobject_cast<QDockWidget*>(w))
            return d;
    return nullptr;
}

void showPalette()
{
    panels::show(kPaletteId);
    panels::placeToolPalette(paletteDock());
}

void hidePalette()
{
    if (QDockWidget* d = paletteDock())
        d->hide();
}

/**
 * Clay: click to place a shape on the clay where you click, drag to size it; Ctrl places a cut. S switches to strokes,
 * which lay a capsule chain along the drag. C turns on chaining: each new shape is linked to the selected one by a live
 * round cone. Shift-click selects the shape under the cursor. Works on the selected clay object, or makes one at the
 * first click.
 */
class ClayTool final : public ViewportTool
{
public:
    const char* name() const override { return "Clay"; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }
    void activate() override
    {
        showPalette();
        // Tools hear key presses but not releases: watch Ctrl so the ring turns to a cut and back as it goes down and up.
        mModifierWatch = std::make_unique<QTimer>();
        QObject::connect(mModifierWatch.get(), &QTimer::timeout, [this] {
            const bool ctrl = QGuiApplication::queryKeyboardModifiers() & Qt::ControlModifier;
            if (ctrl != mCtrl)
            {
                mCtrl = ctrl;
                viewportTools::requestRedraw();
            }
        });
        mModifierWatch->start(50);
    }

    void deactivate() override
    {
        mModifierWatch.reset();
        finishLive(false);
        hidePalette();
        viewportTools::requestRedraw();
    }

    bool ownsBrushRadius() const override { return true; }
    // The shape outlines stand in for the selection, so the clay's dense mesh wire stays off.
    bool showsSelection(scene::NodeId mesh) const override { return !clay::objectOf(mesh); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (tools::brushRadiusDrag(e, false))
        {
            mRadiusDrag = true;
            mPressPx = mNowPx = e->position();
            mStartRadius = e->button() == Qt::MiddleButton ? 0.05 : claySettings().radius;
            mRadiusUnit = view.unitsPerPixel(mHover);
            return true;
        }
        if (e->button() != Qt::LeftButton)
            return false;
        const bool hit = surfaceHit(e->position(), view, mPress);
        if (e->modifiers() & Qt::ShiftModifier)
        {
            if (hit)
                if (json shape = callClay("clayShapeAt", {{"args", {selectedClayPath()}}, {"point", jsonVec(mPress)}}); shape.is_string())
                    callClay("select", {{"args", {shape}}});
            return true;
        }
        scene::Node* parent = chainParent();
        if (!hit)
            mPress = parent ? onViewPlane(e->position(), view, centreOf(*parent)) : onViewPlane(e->position(), view);
        const ClaySettings& s = claySettings();
        mCut = bool(e->modifiers() & Qt::ControlModifier) != s.cut;
        mPressPx = mNowPx = e->position();
        mDragging = true;
        mSnapRadius.reset();
        mView = view.camera;
        mExtend = scene::kNoNode;
        mStrokeRadius = s.radius;
        if (s.stroke)
        {
            if (std::optional<StrokeEnd> end = hit ? strokeEndNear(mPress) : std::nullopt)
            {
                mExtend = end->span;
                mPress = end->at;
                mStrokeRadius = end->radius;
            }
            mStrokePoints = {mPress};
            return true;
        }
        snapToMirror(mPress, s.radius);
        // The shape goes in now and follows the drag; the whole press is one undo step.
        UndoStack::get().openChunk("Clay Shape");
        mChunk = true;
        std::string object = ensureObject();
        if (!object.empty())
        {
            json call = sizeCall(s.radius);
            call["args"] = {object};
            call["shape"] = kToolShapes[size_t(s.shape)];
            call["operation"] = mCut ? "subtract" : "add";
            call["blend"] = s.blend;
            call["position"] = jsonVec(mPress);
            const std::string from = parent ? parent->path() : std::string();
            if (json made = callClay("addClayPrimitive", call); made.is_string())
                if (scene::Node* n = Graph::current().find(made.get<std::string>()))
                {
                    mLive = n->id();
                    if (!from.empty())
                    {
                        json link = {{"args", {from, made}}, {"thickness", s.thickness}, {"operation", call["operation"]}, {"skipSelect", true}};
                        // At full thickness the command picks a hard join, which keeps the joints from swelling.
                        if (s.thickness < 0.999)
                            link["blend"] = s.blend;
                        callClay("addClayLink", link);
                    }
                }
        }
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        if (mRadiusDrag)
        {
            claySettings().radius = std::clamp(mStartRadius + (e->position().x() - mPressPx.x()) * mRadiusUnit, 0.05, 1e4);
            settingsChanged();
            return true;
        }
        if (!mDragging)
        {
            // Where the next shape would go, for the radius ring under the cursor.
            mHoverPx = e->position();
            const bool hit = surfaceHit(mHoverPx, view, mHover);
            if (!hit)
            {
                const scene::Node* parent = chainParent();
                mHover = parent ? onViewPlane(mHoverPx, view, centreOf(*parent)) : onViewPlane(mHoverPx, view);
            }
            mHoverView = view.camera;
            mHoverEnd = hit && claySettings().stroke ? strokeEndNear(mHover) : std::nullopt;
            mHoverSnapped = !claySettings().stroke && snapToMirror(mHover, claySettings().radius);
            viewportTools::requestRedraw();
            return false;
        }
        mNowPx = e->position();
        if (claySettings().stroke)
        {
            glm::dvec3 p;
            if (!surfaceHit(mNowPx, view, p))
                p = onViewPlane(mNowPx, view, mStrokePoints.back());
            if (glm::length(p - mStrokePoints.back()) >= std::max(0.5 * mStrokeRadius, 1e-3))
                mStrokePoints.push_back(p);
        }
        else
        {
            mSnapRadius = snapRadius(e->modifiers());
            if (mSnapRadius)
                resizeLive(*mSnapRadius);
            else if (dragPx() >= 4.0)
                resizeLive(dragPx() * view.unitsPerPixel(mPress));
        }
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override
    {
        if (mRadiusDrag)
        {
            mRadiusDrag = false;
            return true;
        }
        if (e->button() != Qt::LeftButton || !mDragging)
            return false;
        mDragging = false;
        const ClaySettings& s = claySettings();
        if (s.stroke)
        {
            if (mStrokePoints.size() >= 2)
            {
                // A stroke that runs along the mirror plane is one stroke on it, not a pair hugging it.
                const bool onPlane = std::ranges::all_of(mStrokePoints, [this](const glm::dvec3& p) {
                    auto off = mirrorOffset(p);
                    return off && std::abs(off->first) < mirrorReach(mStrokeRadius);
                });
                if (onPlane)
                    for (glm::dvec3& p : mStrokePoints)
                        snapToMirror(p, mStrokeRadius);
                json pts = json::array();
                for (const glm::dvec3& p : mStrokePoints)
                    pts.push_back(jsonVec(p));
                json call = {{"points", pts}, {"radius", mStrokeRadius}, {"endRadius", s.taper ? mStrokeRadius * 0.4 : mStrokeRadius}};
                const scene::Node* extend = Graph::current().node(mExtend);
                if (!extend)
                {
                    std::string object = ensureObject();
                    call["args"] = {object};
                    call["operation"] = mCut ? "subtract" : "add";
                    call["blend"] = s.blend;
                }
                else
                {
                    call["args"] = {extend->path()};
                    call["extend"] = true;
                }
                callClay("addClayStroke", call);
            }
            mStrokePoints.clear();
        }
        else
        {
            if (std::optional<double> snapped = snapRadius(e->modifiers()))
            {
                claySettings().radius = *snapped;
                resizeLive(*snapped);
            }
            else if (dragPx() >= 4.0)
            {
                claySettings().radius = std::max(0.05, dragPx() * view.unitsPerPixel(mPress));
                resizeLive(claySettings().radius);
            }
            if (mLive != scene::kNoNode)
                claySettings().lastRadius = claySettings().radius;
            mSnapRadius.reset();
            finishLive(true);
            settingsChanged();
        }
        viewportTools::requestRedraw();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        const int k = e->key();
        ClaySettings& s = claySettings();
        if (k == Qt::Key_S)
            s.stroke = !s.stroke;
        else if (k == Qt::Key_C)
            s.chain = !s.chain;
        else if (k == Qt::Key_T)
            s.taper = !s.taper;
        else if (k == Qt::Key_BracketLeft || k == Qt::Key_BracketRight)
            s.radius = std::clamp(s.radius * (k == Qt::Key_BracketRight ? 1.25 : 0.8), 0.05, 1e4);
        else if (k == Qt::Key_Comma || k == Qt::Key_Period)
            s.blend = std::clamp(k == Qt::Key_Period ? std::max(s.blend * 1.25, 0.25) : (s.blend < 0.3 ? 0.0 : s.blend * 0.8), 0.0, 1e3);
        else if (k == Qt::Key_Escape)
        {
            if (mDragging)
            {
                mDragging = false;
                mStrokePoints.clear();
                finishLive(false);
            }
            else // Not from inside the tool's own handler: set() destroys the tool.
                QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
        }
        else
            return false;
        settingsChanged();
        return true;
    }

    bool hotkey(const QKeyEvent& e, std::string_view) override
    {
        // The tool's keys are app hotkeys too (S, T, C); it takes them while active.
        if (e.modifiers() & (Qt::ControlModifier | Qt::AltModifier))
            return false;
        return keyPress(const_cast<QKeyEvent*>(&e), ViewInfo{});
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        const ClaySettings& s = claySettings();
        const glm::vec4 color = (mDragging ? mCut : mCtrl != s.cut) ? kCutColor : kAddColor;
        // The brush: the current radius at the surface under the cursor (at the press while B-dragging it).
        if (!mDragging && view.camera == mHoverView)
        {
            QPointF at = mRadiusDrag ? mPressPx : mHoverPx;
            if (!mRadiusDrag && mHoverSnapped)
                if (auto snapped = view.project(mHover))
                    at = *snapped;
            const double r = s.radius / std::max(mRadiusDrag ? mRadiusUnit : view.unitsPerPixel(mHover), 1e-9);
            std::vector<QPointF> ring;
            for (int i = 0; i < 48; ++i)
            {
                const double a = glm::two_pi<double>() * i / 48.0;
                ring.emplace_back(at.x() + r * std::cos(a), at.y() + r * std::sin(a));
            }
            p.polyline(ring, 1.5f, glm::vec4(glm::vec3(color), 0.7f), true);
            // Where the next link runs: from the shape it chains from to the cursor.
            if (const scene::Node* parent = mRadiusDrag ? nullptr : chainParent())
                if (auto from = view.project(centreOf(*parent)))
                    p.dashedPolyline({*from, at}, 1.5f, 6.0f, glm::vec4(glm::vec3(color), 0.7f), false);
            // A stroke end under the cursor lights up: a stroke started here continues it.
            if (mHoverEnd && !mRadiusDrag)
                if (auto end = view.project(mHoverEnd->at))
                {
                    p.disc(*end, 4.0f, color);
                    p.polyline(circlePx(*end, mHoverEnd->radius / std::max(view.unitsPerPixel(mHoverEnd->at), 1e-9)), 2.5f, color, true);
                }
        }
        if (mDragging && view.camera == mView)
        {
            if (s.stroke)
            {
                std::vector<QPointF> line;
                for (const glm::dvec3& w : mStrokePoints)
                    if (auto px = view.project(w))
                        line.push_back(*px);
                p.polyline(line, 2.0f, color);
            }
            else
            {
                const double r = mSnapRadius ? *mSnapRadius / view.unitsPerPixel(mPress) : dragPx() >= 4.0 ? dragPx() : s.radius / view.unitsPerPixel(mPress);
                std::vector<QPointF> ring;
                for (int i = 0; i < 48; ++i)
                {
                    const double a = glm::two_pi<double>() * i / 48.0;
                    ring.emplace_back(mPressPx.x() + r * std::cos(a), mPressPx.y() + r * std::sin(a));
                }
                p.polyline(ring, 1.5f, color, true);
            }
        }
        // Above the view axis in the bottom-left corner.
        if (PrefStore::get().value<bool>("general", kShowKeysPref, true))
            drawKeyList(p, QPointF(16.0, view.height - 84.0), clayKeys());
    }

private:
    double dragPx() const { return glm::length(glm::dvec2(mNowPx.x() - mPressPx.x(), mNowPx.y() - mPressPx.y())); }

    /** With Shift held while placing a shape, the last placed shape's radius (the current one before any). */
    static std::optional<double> snapRadius(Qt::KeyboardModifiers mods)
    {
        if (!(mods & Qt::ShiftModifier))
            return std::nullopt;
        const ClaySettings& s = claySettings();
        return s.lastRadius > 0.0 ? s.lastRadius : s.radius;
    }

    /** The selection's clay object, or a new one at the press. */
    std::string ensureObject()
    {
        std::string object = selectedClayPath();
        if (object.empty())
            if (json made = callClay("createClay", {{"position", jsonVec(mPress)}, {"empty", true}}); made.is_array())
                object = made[0];
        return object;
    }

    /** The size flags for the current shape at radius r. */
    static json sizeCall(double r)
    {
        const int shape = claySettings().shape;
        return {{"radius", r},
                {"endRadius", shape == 5 ? 0.0 : shape == 6 ? r * 0.3 : r},
                {"length", 2.0 * r},
                {"extents", shape == 1 ? json{r, r * 0.7, r} : jsonVec(glm::dvec3(r))}};
    }

    void resizeLive(double r)
    {
        Graph& g = Graph::current();
        scene::Node* n = g.node(mLive);
        if (!n)
            return;
        r = std::max(0.05, r);
        const int shape = claySettings().shape;
        auto set = [&](const char* attr, scene::PlugValue v) { g.setValue({n->id(), n->type().attrIndex(attr), -1}, std::move(v)); };
        set("radius", r);
        set("endRadius", shape == 5 ? 0.0 : shape == 6 ? r * 0.3 : r);
        set("length", 2.0 * r);
        set("extents", shape == 1 ? glm::dvec3(r, r * 0.7, r) : glm::dvec3(r));
    }

    /** Closes the press's undo step: kept, or undone and dropped. */
    void finishLive(bool keep)
    {
        if (!mChunk)
            return;
        mChunk = false;
        mLive = scene::kNoNode;
        keep ? UndoStack::get().closeChunk() : UndoStack::get().cancelChunk();
    }

    bool surfaceHit(QPointF px, const ViewInfo& view, glm::dvec3& out) const
    {
        Graph& g = Graph::current();
        scene::Node* object = selectedClay();
        if (!object)
            return false;
        g.evaluate();
        for (NodeId c : g.children(object->id()))
            if (scene::Node* n = g.node(c); n && n->type().isA("mesh"))
                if (scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh); m && m->faceCapacity() > 0)
                {
                    const glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix), inv = glm::inverse(world);
                    glm::dvec3 o, d;
                    view.ray(px, o, d);
                    if (mBvhMesh != m)
                    {
                        mBvh.build(*m);
                        mBvhMesh = m;
                    }
                    geo::BvhHit hit;
                    if (mBvh.raycast(glm::dvec3(inv * glm::dvec4(o, 1.0)), glm::dvec3(inv * glm::dvec4(d, 0.0)), hit))
                    {
                        out = glm::dvec3(world * glm::dvec4(hit.point, 1.0));
                        return true;
                    }
                }
        return false;
    }

    /** Where px meets the plane through the clay object's pivot (or `at`) facing the view. */
    glm::dvec3 onViewPlane(QPointF px, const ViewInfo& view, std::optional<glm::dvec3> at = std::nullopt) const
    {
        glm::dvec3 centre(0.0);
        if (at)
            centre = *at;
        else if (scene::Node* o = selectedClay())
            centre = glm::dvec3(o->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
        glm::dvec3 o, d;
        view.ray(px, o, d);
        const glm::dvec3 n = view.viewDir(centre);
        const double den = glm::dot(d, n);
        return std::abs(den) < 1e-9 ? centre : o + d * (glm::dot(centre - o, n) / den);
    }

    bool mCut = false, mDragging = false;
    glm::dvec3 mPress{0.0};
    QPointF mPressPx, mNowPx;
    std::vector<glm::dvec3> mStrokePoints;
    std::string mView;
    QPointF mHoverPx;
    glm::dvec3 mHover{0.0};
    std::string mHoverView;
    std::optional<StrokeEnd> mHoverEnd;
    bool mHoverSnapped = false;
    bool mRadiusDrag = false;
    bool mCtrl = false;
    std::unique_ptr<QTimer> mModifierWatch;
    double mStartRadius = 5.0, mRadiusUnit = 0.1;
    scene::NodeId mLive = scene::kNoNode;   // the shape being placed and sized
    std::optional<double> mSnapRadius;      // set while Shift holds the shape at the last one's radius
    scene::NodeId mExtend = scene::kNoNode; // the span a stroke continues from
    double mStrokeRadius = 5.0;
    bool mChunk = false;
    mutable geo::MeshBvh mBvh;
    mutable scene::MeshPtr mBvhMesh;
};
} // namespace

/** The one selected clay shape when no viewport tool is active, the one whose size and blend rings show. */
scene::Node* handleShape()
{
    Graph& g = Graph::current();
    if (viewportTools::active() || g.selection().size() != 1)
        return nullptr;
    scene::Node* n = g.node(g.selection().front());
    return n && n->type().isA("clayPrimitive") ? n : nullptr;
}

double attrOf(const scene::Node& n, const char* attr) { return n.as<double>(n.type().attrIndex(attr)); }

/** The shape's size as its rings show it: the radius, or a box's or ellipsoid's largest half size. */
double sizeOf(const scene::Node& n)
{
    const int64_t shape = n.as<int64_t>(n.type().attrIndex("shape"));
    if (shape == 1 || shape == 2)
    {
        const glm::dvec3 e = n.as<glm::dvec3>(n.type().attrIndex("extents"));
        return std::max({e.x, e.y, e.z});
    }
    return attrOf(n, "radius");
}

struct Rings
{
    QPointF centre;
    double size = 0.0, blend = 0.0; // pixels
    double unit = 1.0;              // world units per pixel, through the shape's scale
};

std::optional<Rings> ringsOf(const scene::Node& n, const ViewInfo& view)
{
    const glm::dmat4 world = n.as<glm::dmat4>(scene::Transform::kWorldMatrix);
    const glm::dvec3 centre(world[3]);
    auto px = view.project(centre);
    if (!px)
        return std::nullopt;
    const double scale = std::min({glm::length(glm::dvec3(world[0])), glm::length(glm::dvec3(world[1])), glm::length(glm::dvec3(world[2]))});
    const double unit = view.unitsPerPixel(centre) / std::max(scale, 1e-9);
    return Rings{*px, sizeOf(n) / unit, (sizeOf(n) + attrOf(n, "blend")) / unit, unit};
}

std::vector<QPointF> circle(QPointF c, double r)
{
    std::vector<QPointF> out;
    for (int i = 0; i < 64; ++i)
    {
        const double a = glm::two_pi<double>() * i / 64.0;
        out.emplace_back(c.x() + r * std::cos(a), c.y() + r * std::sin(a));
    }
    return out;
}

void drawRings(render::FrameState&, OverlayPainter& p, const ViewInfo& view)
{
    const scene::Node* n = handleShape();
    if (!n)
        return;
    if (auto r = ringsOf(*n, view))
    {
        p.polyline(circle(r->centre, r->size), 1.5f, glm::vec4(1.0f, 0.85f, 0.3f, 0.9f), true);
        if (r->blend > r->size + 1.0)
            p.dashedPolyline(circle(r->centre, r->blend), 1.0f, 6.0f, glm::vec4(1.0f, 0.85f, 0.3f, 0.6f), true);
    }
}

/**
 * Dragging a selected clay shape's ring: the solid one sizes the shape (radius, end radius, extents and rounding
 * together), the dashed one sets its blend. One undo step.
 */
class RingDrag final : public ViewportTool
{
public:
    RingDrag(NodeId shape, bool blend, Rings rings) : mShape(shape), mBlend(blend), mRings(rings)
    {
        UndoStack::get().openChunk(blend ? "Clay Blend" : "Clay Size");
        if (scene::Node* n = Graph::current().node(mShape))
            for (const char* attr : {"radius", "endRadius", "rounding", "blend"})
                mStart[attr] = attrOf(*n, attr);
        if (scene::Node* n = Graph::current().node(mShape))
            mExtents = n->as<glm::dvec3>(n->type().attrIndex("extents"));
        if (scene::Node* n = Graph::current().node(mShape))
            mSize = sizeOf(*n);
    }
    const char* name() const override { return "Clay Ring"; }
    void deactivate() override { finish(); }

    // The viewport hands releases only to a tool that took the press, and the click hook took this one: the drag also
    // ends at the first move without the button, or the next press.
    bool mousePress(QMouseEvent*, const ViewInfo&) override
    {
        end();
        return false;
    }
    bool mouseMove(QMouseEvent* e, const ViewInfo&) override
    {
        if (!(e->buttons() & Qt::LeftButton))
        {
            end();
            return false;
        }
        Graph& g = Graph::current();
        scene::Node* n = g.node(mShape);
        if (!n)
            return true;
        const double px = glm::length(glm::dvec2(e->position().x() - mRings.centre.x(), e->position().y() - mRings.centre.y()));
        auto set = [&](const char* attr, scene::PlugValue v) { g.setValue({n->id(), n->type().attrIndex(attr), -1}, std::move(v)); };
        if (mBlend)
            set("blend", std::max(0.0, px * mRings.unit - mSize));
        else
        {
            const double k = std::max(px * mRings.unit, 0.01) / std::max(mSize, 1e-9);
            for (const char* attr : {"radius", "endRadius", "rounding"})
                set(attr, mStart[attr] * k);
            set("extents", mExtents * k);
        }
        viewportTools::requestRedraw();
        return true;
    }
    bool mouseRelease(QMouseEvent*, const ViewInfo&) override
    {
        end();
        return true;
    }
    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        if (scene::Node* n = Graph::current().node(mShape))
            if (auto r = ringsOf(*n, view))
            {
                p.polyline(circle(r->centre, r->size), mBlend ? 1.0f : 2.5f, glm::vec4(1.0f, 0.85f, 0.3f, 1.0f), true);
                p.dashedPolyline(circle(r->centre, r->blend), mBlend ? 2.5f : 1.0f, 6.0f, glm::vec4(1.0f, 0.85f, 0.3f, 0.8f), true);
            }
    }

private:
    void finish()
    {
        if (mOpen)
            UndoStack::get().closeChunk();
        mOpen = false;
    }

    void end()
    {
        finish();
        // Not from inside the tool's own handler: set() destroys the tool.
        QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
    }

    NodeId mShape = scene::kNoNode;
    bool mBlend = false;
    Rings mRings;
    std::map<std::string, double> mStart;
    glm::dvec3 mExtents{0.0};
    double mSize = 1.0;
    bool mOpen = true;
};

/** A click on a ring of the selected clay shape starts dragging it. */
bool pressRings(QPointF px, const ViewInfo& view)
{
    const scene::Node* n = handleShape();
    auto r = n ? ringsOf(*n, view) : std::nullopt;
    if (!r)
        return false;
    const double d = glm::length(glm::dvec2(px.x() - r->centre.x(), px.y() - r->centre.y()));
    const bool onSize = std::abs(d - r->size) < 6.0, onBlend = r->blend > r->size + 1.0 && std::abs(d - r->blend) < 6.0;
    if (!onSize && !onBlend)
        return false;
    viewportTools::set(std::make_unique<RingDrag>(n->id(), onBlend && (!onSize || std::abs(d - r->blend) < std::abs(d - r->size)), *r));
    return true;
}

void registerClayTool()
{
    viewportHooks::addDrawer("lila.clayRings", drawRings);
    viewportHooks::setClickHook("lila.clayRings", pressRings);
    viewportHooks::addDrawer("lila.clayShapes", drawShapes);
    viewportHooks::addPicker("lila.clayShapes", pickShapes);
    // Shapes are locators underneath, but their outlines and the clay surface stand in for the locator cross.
    if (!drawOverrides::registry().find("lila.clayPrimitive"))
        drawOverrides::add({.id = "lila.clayPrimitive", .nodeType = "clayPrimitive", .draw = [](drawOverrides::DrawContext&) {}});
    panels::add({.id = kPaletteId,
                 .title = "Clay",
                 .create = [](QWidget* parent) -> QWidget* { return gPalette = new ClayPalette(parent); },
                 .area = Qt::LeftDockWidgetArea,
                 .icon = "brush",
                 .open = false,
                 .place = [](QDockWidget* d) { d->setProperty(panels::kOwnSizeProperty, true); },
                 .floatFirst = true,
                 .toolPalette = true});
    if (!viewportTools::registry().find("clay.tool"))
        viewportTools::add({.id = "clay.tool", .label = "Clay Tool", .icon = "brush", .create = [] { return std::make_unique<ClayTool>(); }});
    actions::add({.id = "clay.tool",
                  .label = "Clay Tool",
                  .category = "Mesh",
                  .run = [] { viewportTools::activate("clay.tool"); },
                  .doc = "Place clay shapes and cuts by clicking on the clay, sized by dragging, or draw capsule strokes over it. Works on "
                         "the selected clay object, or makes one at the first click.",
                  .python = "lila.ui.run_action(\"clay.tool\")",
                  .icon = "brush"});
}
} // namespace lila::ui
