// Print: 3D print preparation. Printability checks as validation issues, wall thickness and overhang heatmaps,
// printer presets and sizing, hollowing with drain holes, plane splits with pegs, lay flat, and 3MF files.

#include "PrintGeo.h"
#include "ThreeMf.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Components.h>
#include <lila/Deformer.h>
#include <lila/Diagnostics.h>
#include <lila/Extension.h>
#include <lila/FileTranslator.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Menus.h>
#include <lila/Mesh.h>
#include <lila/MeshOps.h>
#include <lila/Node.h>
#include <lila/OptionBox.h>
#include <lila/Paths.h>
#include <lila/PreferencePages.h>
#include <lila/Prefs.h>
#include <lila/Retopo.h>
#include <lila/Shelves.h>
#include <lila/Transform.h>
#include <lila/Viewport.h>

#include <QString>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;
namespace fs = std::filesystem;
namespace dx = lila::scene::diagnostics;

namespace print
{
namespace
{
constexpr const char* kPrefs = "print";
constexpr const char* kSource = "print";
constexpr const char* kThicknessSet = "printThickness";
constexpr const char* kOverhangSet = "printOverhang";
constexpr double kMmToCm = 0.1;

// Printer and units

Printer currentPrinter()
{
    const PrefStore& prefs = PrefStore::get();
    const std::string id = prefs.value<std::string>(kPrefs, "/printer", "prusaMk4");
    const Printer* preset = findPrinter(id);
    Printer p = preset ? *preset : printers().front();
    if (p.id == "custom")
    {
        p.volume = {prefs.value<double>(kPrefs, "/custom/width", 200.0), prefs.value<double>(kPrefs, "/custom/depth", 200.0),
                    prefs.value<double>(kPrefs, "/custom/height", 200.0)};
        p.nozzle = prefs.value<double>(kPrefs, "/custom/nozzle", 0.4);
        p.minWall = prefs.value<double>(kPrefs, "/custom/minWall", 0.8);
        p.overhang = prefs.value<double>(kPrefs, "/custom/overhang", 45.0);
    }
    return p;
}

glm::dvec3 up() { return sceneUp(); }

json printerJson(const Printer& p)
{
    return {{"id", p.id}, {"label", p.label}, {"volume", {p.volume.x, p.volume.y, p.volume.z}}, {"nozzle", p.nozzle},
            {"minWall", p.minWall}, {"overhang", p.overhang}};
}

std::optional<glm::dvec3> vec3Flag(const Args& a, std::string_view flag)
{
    if (!a.has(flag))
        return std::nullopt;
    const json& v = a.flags[std::string(flag)];
    if (!v.is_array() || v.size() != 3)
        throw CommandError(std::format("{} takes three numbers", flag));
    return glm::dvec3(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
}

json vecJson(const glm::dvec3& v) { return {v.x, v.y, v.z}; }

/** File axes to scene axes and unit to cm, as the mesh file commands do. */
glm::dmat4 fileToScene(double cmPerUnit, bool fileZUp)
{
    const double s = cmPerUnit;
    const bool sceneZ = math::upAxis() == math::UpAxis::Z;
    glm::dmat4 m(s);
    m[3][3] = 1.0;
    if (fileZUp && !sceneZ)
        m = glm::dmat4(glm::dvec4(s, 0, 0, 0), glm::dvec4(0, 0, -s, 0), glm::dvec4(0, s, 0, 0), glm::dvec4(0, 0, 0, 1));
    else if (!fileZUp && sceneZ)
        m = glm::dmat4(glm::dvec4(s, 0, 0, 0), glm::dvec4(0, 0, s, 0), glm::dvec4(0, -s, 0, 0), glm::dvec4(0, 0, 0, 1));
    return m;
}

double unitCm(const std::string& unit, const char* cmd)
{
    if (unit == "mm")
        return 0.1;
    if (unit == "cm")
        return 1.0;
    if (unit == "m")
        return 100.0;
    if (unit == "in")
        return 2.54;
    throw CommandError(std::string(cmd) + ": unit must be mm, cm, m or in");
}

const char* unitName3mf(const std::string& unit)
{
    return unit == "cm" ? "centimeter" : unit == "m" ? "meter" : unit == "in" ? "inch" : "millimeter";
}

// Meshes

/** Meshes a Print command has worked on: the validator checks only these, so meshes bound elsewhere never get print issues. */
std::set<NodeId> gTargets;
int gTargetListener = -1;

std::vector<Node*> meshNodes(const Args& a, const char* cmd)
{
    Graph& g = Graph::current();
    std::vector<Node*> out;
    for (const std::string& name : meshObjects(a, cmd))
        if (Node* n = g.find(name))
        {
            out.push_back(n);
            gTargets.insert(n->id());
        }
    return out;
}

Node& oneMesh(const Args& a, const char* cmd)
{
    std::vector<Node*> nodes = meshNodes(a, cmd);
    if (nodes.size() != 1)
        throw CommandError(std::string(cmd) + ": give one mesh");
    return *nodes.front();
}

glm::dmat4 worldOf(Graph& g, const Node& n)
{
    g.evaluate();
    return n.as<glm::dmat4>(Transform::kWorldMatrix);
}

/** What the mesh draws and exports (modifiers applied); component ids index this one. */
MeshPtr drawn(Graph& g, Node& n)
{
    g.evaluate();
    return n.as<MeshPtr>(n.type().attrIndex("outMesh"));
}

geo::Mesh transformed(const geo::Mesh& m, const glm::dmat4& xf)
{
    geo::Mesh out = m;
    for (Index p = 0; p < out.pointCapacity(); ++p)
        if (out.pointValid(p))
            out.setPosition(p, glm::dvec3(xf * glm::dvec4(out.position(p), 1.0)));
    if (glm::determinant(glm::dmat3(xf)) < 0.0)
        for (Index f = 0; f < out.faceCapacity(); ++f)
            if (out.faceValid(f))
                out.flipFace(f);
    return out;
}

geo::Mesh worldMesh(Graph& g, Node& n)
{
    MeshPtr m = drawn(g, n);
    if (!m)
        throw CommandError(n.name() + " has no mesh");
    return transformed(*m, worldOf(g, n));
}

/** The base mesh direct edits change, and the plug it lives on. */
geo::Mesh editable(Graph& g, Node& n, PlugRef& plug)
{
    plug = modifiers::editPlug(g, n.id());
    g.evaluate();
    const MeshPtr& m = std::get<MeshPtr>(g.storedValue(plug));
    if (!m)
        throw CommandError(n.name() + " has no mesh");
    return *m;
}

void store(Graph& g, const PlugRef& plug, geo::Mesh&& m) { g.setValue(plug, MeshPtr(std::make_shared<geo::Mesh>(std::move(m)))); }

/** World lengths to object lengths for a mesh scaled by xf (the cube root of its volume scale). */
double objectScale(const glm::dmat4& xf)
{
    const double det = std::abs(glm::determinant(glm::dmat3(xf)));
    return det > 1e-30 ? std::cbrt(det) : 1.0;
}

geo::Bounds worldBounds(Graph& g, const std::vector<Node*>& nodes)
{
    geo::Bounds b;
    for (Node* n : nodes)
    {
        const geo::Bounds nb = worldMesh(g, *n).bounds();
        if (!nb.valid())
            continue;
        b.min = glm::min(b.min, nb.min);
        b.max = glm::max(b.max, nb.max);
    }
    return b;
}

/** Bounds in mm as (width, depth, height): the two scene axes across up, then up. */
glm::dvec3 printSize(const geo::Bounds& b)
{
    const glm::dvec3 s = b.size() / kMmToCm;
    return math::upAxis() == math::UpAxis::Z ? s : glm::dvec3(s.x, s.z, s.y);
}

/** Scale that fits size (mm) into the printer, the bed's sides taken either way round. */
double fitScale(const glm::dvec3& size, const Printer& p, double margin)
{
    auto room = [&](double v) { return std::max(v - 2.0 * margin, 1e-6); };
    double lo[2] = {std::min(size.x, size.y), std::max(size.x, size.y)};
    double bed[2] = {std::min(p.volume.x, p.volume.y), std::max(p.volume.x, p.volume.y)};
    double s = kInf;
    for (int i = 0; i < 2; ++i)
        if (lo[i] > 0.0)
            s = std::min(s, room(bed[i]) / lo[i]);
    if (size.z > 0.0)
        s = std::min(s, std::max(p.volume.z - margin, 1e-6) / size.z);
    return s;
}

std::string sizeText(const glm::dvec3& s) { return std::format("{:.1f} x {:.1f} x {:.1f} mm", s.x, s.y, s.z); }

void setWorld(Graph& g, Node& n, const glm::dmat4& m)
{
    json flat = json::array();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            flat.push_back(m[c][r]);
    ops::execute("xform", {{"args", {g.fullPath(n.id())}}, {"worldSpace", true}, {"matrix", flat}});
}

glm::dmat4 about(const glm::dvec3& pivot, const glm::dmat4& m)
{
    glm::dmat4 to(1.0), back(1.0);
    to[3] = glm::dvec4(pivot, 1.0);
    back[3] = glm::dvec4(-pivot, 1.0);
    return to * m * back;
}

std::string pyString(const std::string& s) { return json(s).dump(); }

// Checks

CheckOptions checkOptions(const Args& a)
{
    CheckOptions o;
    for (size_t i = 0; i < kCheckCount; ++i)
        o.checks[i] = a.get<bool>(checkName(Check(i)), true);
    const double wall = a.get<double>("minWall", -1.0);
    o.minWall = (wall > 0.0 ? wall : currentPrinter().minWall) * kMmToCm;
    return o;
}

dx::Severity severityOf(Check c)
{
    switch (c)
    {
    case Check::Shells: return dx::Severity::Info;
    case Check::ZeroArea:
    case Check::ThinWalls: return dx::Severity::Warning;
    default: return dx::Severity::Error;
    }
}

/** The meshRepair call that fixes a check, or empty. */
std::string fixFor(Check c, const std::string& path)
{
    std::vector<retopo::MeshIssue> fixes;
    switch (c)
    {
    case Check::NonManifold: fixes = {retopo::MeshIssue::NonManifoldEdge, retopo::MeshIssue::NonManifoldPoint}; break;
    case Check::Holes: fixes = {retopo::MeshIssue::Hole}; break;
    case Check::Flipped: fixes = {retopo::MeshIssue::Flipped}; break;
    case Check::ZeroArea: fixes = {retopo::MeshIssue::ZeroAreaFace}; break;
    case Check::SelfIntersections: return std::format("lila.ops.voxelRemesh({}, resolution=256)", pyString(path));
    default: return {};
    }
    std::string call = std::format("lila.ops.meshRepair({}, mode='repair'", pyString(path));
    for (size_t i = 0; i < retopo::kIssueCount; ++i)
    {
        const bool on = std::ranges::find(fixes, retopo::MeshIssue(i)) != fixes.end();
        call += std::format(", {}={}", retopo::issueName(retopo::MeshIssue(i)), on ? "True" : "False");
    }
    return call + ")";
}

std::string messageFor(Check c, const CheckResult& r, double minWallMm)
{
    switch (c)
    {
    case Check::Holes: return std::format("{} {} (open borders)", r.count, r.count == 1 ? "hole" : "holes");
    case Check::Shells: return std::format("{} separate shells; slicers print each as its own part", r.count);
    case Check::ThinWalls: return std::format("{} points under the {:.2f} mm minimum wall", r.count, minWallMm);
    default: return std::format("{} {}", r.count, checkLabel(c));
    }
}

bool failing(Check c, const CheckResult& r) { return c == Check::Shells ? r.count > 1 : r.count > 0; }

json runChecks(const std::vector<Node*>& nodes, const CheckOptions& opts, bool report)
{
    Graph& g = Graph::current();
    json out = json::object();
    for (Node* n : nodes)
    {
        const std::string path = g.fullPath(n->id());
        const CheckReport r = checkPrintability(worldMesh(g, *n), opts);
        json counts = json::object();
        if (report)
            dx::clear(kSource, n->id());
        for (size_t i = 0; i < kCheckCount; ++i)
        {
            if (!opts.checks[i])
                continue;
            const Check c = Check(i);
            const CheckResult& res = r.of(c);
            counts[checkName(c)] = res.count;
            if (!report || !failing(c, res))
                continue;
            dx::Issue issue;
            issue.source = kSource;
            issue.severity = severityOf(c);
            issue.target = path;
            issue.message = "Print: " + messageFor(c, res, opts.minWall / kMmToCm);
            issue.fix = fixFor(c, path);
            issue.select = std::format("lila.ops.printSelect({}, check='{}')", pyString(path), checkName(c));
            dx::report(std::move(issue));
        }
        out[path] = counts;
    }
    return out;
}

/** Selects the check's components on the mesh, choosing the component mode with the most of them. */
size_t selectCheck(Node& n, Check c, const CheckOptions& base)
{
    Graph& g = Graph::current();
    CheckOptions o = base;
    o.checks.fill(false);
    o.checks[size_t(c)] = true;
    const CheckResult r = checkPrintability(worldMesh(g, n), o).of(c);
    g.select({n.id()});
    std::map<NodeId, components::Ids> sets[3];
    if (!r.points.empty())
        sets[0][n.id()] = components::Ids(r.points.begin(), r.points.end());
    if (!r.edges.empty())
        sets[1][n.id()] = components::Ids(r.edges.begin(), r.edges.end());
    if (!r.faces.empty())
        sets[2][n.id()] = components::Ids(r.faces.begin(), r.faces.end());
    const size_t total[3] = {r.points.size(), r.edges.size(), r.faces.size()};
    ComponentMode mode = ComponentMode::Face;
    if (total[1] > total[2] && total[1] >= total[0])
        mode = ComponentMode::Edge;
    else if (total[0] > total[2] && total[0] > total[1])
        mode = ComponentMode::Vertex;
    components::setAll(std::move(sets[0]), std::move(sets[1]), std::move(sets[2]));
    if (total[0] + total[1] + total[2] > 0)
        components::setMode(mode);
    return total[0] + total[1] + total[2];
}

// Heatmaps

struct Legend
{
    std::string mode; // "", "thickness", "overhang"
    double minWallMm = 0.8;
    double threshold = 45.0;
    std::vector<NodeId> nodes;
    std::string savedVertexColors;
};
Legend gLegend;

void showVertexColors(bool on)
{
    PrefStore& prefs = PrefStore::get();
    if (on)
    {
        if (gLegend.savedVertexColors.empty())
            gLegend.savedVertexColors = prefs.value<std::string>("general", ui::viewportDisplay::kVertexColors, "off");
        prefs.set("general", ui::viewportDisplay::kVertexColors, "unlit");
    }
    else if (!gLegend.savedVertexColors.empty())
    {
        prefs.set("general", ui::viewportDisplay::kVertexColors, gLegend.savedVertexColors);
        gLegend.savedVertexColors.clear();
    }
}

json analyze(const Args& a)
{
    Graph& g = Graph::current();
    const std::string mode = a.get<std::string>("mode", "thickness");
    if (mode != "thickness" && mode != "overhang" && mode != "off")
        throw CommandError("printAnalyze: mode must be thickness, overhang or off");
    const Printer printer = currentPrinter();
    const double minWallMm = a.get<double>("minWall", -1.0) > 0.0 ? a.get<double>("minWall") : printer.minWall;
    const double threshold = a.get<double>("angle", -1.0) > 0.0 ? a.get<double>("angle") : printer.overhang;

    std::vector<Node*> nodes;
    if (mode == "off")
    {
        for (NodeId id : gLegend.nodes)
            if (Node* n = g.node(id))
                nodes.push_back(n);
        if (!a.positional.empty() || nodes.empty())
            try
            {
                nodes = meshNodes(a, "printAnalyze");
            }
            catch (const CommandError&)
            {
            }
    }
    else
        nodes = meshNodes(a, "printAnalyze");

    json out = json::object();
    for (Node* n : nodes)
    {
        PlugRef plug;
        geo::Mesh base = editable(g, *n, plug);
        if (mode == "off")
        {
            const bool had = geo::colors::remove(base, kThicknessSet) | geo::colors::remove(base, kOverhangSet);
            if (had)
                store(g, plug, std::move(base));
            out[g.fullPath(n->id())] = had;
            continue;
        }
        const glm::dmat4 world = worldOf(g, *n);
        const geo::Mesh w = transformed(base, world);
        json stats;
        if (mode == "thickness")
        {
            const std::vector<double> th = wallThickness(w);
            const double minWall = minWallMm * kMmToCm;
            std::vector<glm::vec4> colors(base.pointCapacity(), glm::vec4(1.0f));
            double thinnest = kInf;
            size_t thin = 0;
            for (Index p = 0; p < base.pointCapacity(); ++p)
                if (base.pointValid(p))
                {
                    thinnest = std::min(thinnest, th[p]);
                    thin += th[p] < minWall;
                    // Half the ramp from 0 to the minimum, the other half from it to three times it, as the legend reads.
                    colors[p] = heatColor(th[p] < minWall ? 0.5 * th[p] / minWall : 0.5 + 0.5 * (th[p] - minWall) / (minWall * 2.0));
                }
            geo::colors::remove(base, kOverhangSet);
            if (!geo::colors::has(base, kThicknessSet))
                geo::colors::add(base, kThicknessSet);
            geo::colors::setFromPoints(base, kThicknessSet, colors);
            geo::colors::setCurrent(base, kThicknessSet);
            stats = {{"thinnest", thinnest == kInf ? -1.0 : thinnest / kMmToCm}, {"thinPoints", thin}};
        }
        else
        {
            const std::vector<double> angles = overhangAngles(w, up());
            geo::colors::remove(base, kThicknessSet);
            std::span<glm::vec4> values = geo::colors::has(base, kOverhangSet) ? geo::colors::values(base, kOverhangSet)
                                                                                : geo::colors::add(base, kOverhangSet);
            size_t over = 0;
            for (Index f = 0; f < base.faceCapacity(); ++f)
            {
                if (!base.faceValid(f))
                    continue;
                over += angles[f] > threshold;
                const glm::vec4 c = heatColor((threshold + 10.0 - angles[f]) / 20.0);
                for (Index corner : base.faceCorners(f))
                    values[corner] = c;
            }
            geo::colors::setCurrent(base, kOverhangSet);
            stats = {{"overhangFaces", over}, {"supportArea", supportArea(w, up(), threshold)}};
        }
        store(g, plug, std::move(base));
        out[g.fullPath(n->id())] = stats;
    }

    if (mode == "off")
    {
        gLegend = Legend{.savedVertexColors = gLegend.savedVertexColors};
        showVertexColors(false);
    }
    else
    {
        gLegend.mode = mode;
        gLegend.minWallMm = minWallMm;
        gLegend.threshold = threshold;
        gLegend.nodes.clear();
        for (Node* n : nodes)
            gLegend.nodes.push_back(n->id());
        showVertexColors(true);
    }
    return out;
}

void drawLegend(ui::OverlayPainter& p, const ui::ViewInfo& view)
{
    if (gLegend.mode.empty())
        return;
    Graph& g = Graph::current();
    if (std::ranges::none_of(gLegend.nodes, [&](NodeId id) { return g.node(id) != nullptr; }))
        return;
    const bool thickness = gLegend.mode == "thickness";
    const double w = 14.0, h = 140.0, x = 16.0, y = view.height - h - 40.0;
    constexpr int kSteps = 28;
    for (int i = 0; i < kSteps; ++i)
    {
        const double t0 = double(i) / kSteps;
        // Top of the bar is the good end in both maps.
        const glm::vec4 c = heatColor(1.0 - t0);
        p.rect(QRectF(x, y + h * t0, w, h / kSteps + 0.5), c);
    }
    const glm::vec4 ink = p.ink(0.85f);
    const double lx = x + w + 6.0;
    p.text(QPointF(x, y - 8.0), thickness ? QStringLiteral("Wall") : QStringLiteral("Overhang"), ink);
    if (thickness)
    {
        p.text(QPointF(lx, y + 4.0), QString::fromStdString(std::format("{:.2f}+ mm", gLegend.minWallMm * 3.0)), ink);
        p.text(QPointF(lx, y + h * 0.5 + 4.0), QString::fromStdString(std::format("{:.2f} mm", gLegend.minWallMm)), ink);
        p.text(QPointF(lx, y + h + 4.0), QStringLiteral("0 mm"), ink);
    }
    else
    {
        p.text(QPointF(lx, y + 4.0), QString::fromStdString(std::format("{:.0f}°", gLegend.threshold - 10.0)), ink);
        p.text(QPointF(lx, y + h * 0.5 + 4.0), QString::fromStdString(std::format("{:.0f}°", gLegend.threshold)), ink);
        p.text(QPointF(lx, y + h + 4.0), QString::fromStdString(std::format("{:.0f}°", gLegend.threshold + 10.0)), ink);
    }
}

// Commands

json info(const Args& a)
{
    Graph& g = Graph::current();
    const Printer printer = currentPrinter();
    const double threshold = printer.overhang;
    std::vector<Node*> nodes = meshNodes(a, "printInfo");
    json meshes = json::object();
    double totalVolume = 0.0;
    for (Node* n : nodes)
    {
        const geo::Mesh w = worldMesh(g, *n);
        const double v = volume(w);
        totalVolume += v;
        const CheckReport r = checkPrintability(w, CheckOptions{.checks = {true, true, false, false, false, true, false}});
        meshes[g.fullPath(n->id())] = {{"size", vecJson(printSize(w.bounds()))},
                                       {"volume", v},
                                       {"area", surfaceArea(w)},
                                       {"watertight", r.of(Check::Holes).count == 0 && r.of(Check::NonManifold).count == 0},
                                       {"shells", r.of(Check::Shells).count},
                                       {"supportArea", supportArea(w, up(), threshold)}};
    }
    const glm::dvec3 size = printSize(worldBounds(g, nodes));
    const double fit = fitScale(size, printer, 0.0);
    return {{"size", vecJson(size)},
            {"volume", totalVolume},
            {"printer", printerJson(printer)},
            {"fits", fit >= 1.0},
            {"scaleToFit", fit},
            {"meshes", meshes}};
}

json scaleToFit(const Args& a)
{
    Graph& g = Graph::current();
    const Printer printer = currentPrinter();
    std::vector<Node*> nodes = meshNodes(a, "printScaleToFit");
    const geo::Bounds b = worldBounds(g, nodes);
    if (!b.valid())
        throw CommandError("printScaleToFit: nothing to measure");
    double s = fitScale(printSize(b), printer, a.get<double>("margin", 5.0));
    if (s >= 1.0 && !a.get<bool>("enlarge", false))
        s = 1.0;
    if (s != 1.0)
    {
        glm::dvec3 pivot = b.center();
        const glm::dvec3 u = up();
        pivot -= u * glm::dot(b.center() - b.min, u);
        for (Node* n : nodes)
            setWorld(g, *n, about(pivot, glm::scale(glm::dmat4(1.0), glm::dvec3(s))) * worldOf(g, *n));
    }
    return {{"scale", s}, {"size", vecJson(printSize(b) * s)}, {"printer", printer.label}};
}

json layFlat(const Args& a)
{
    Graph& g = Graph::current();
    const double threshold = a.get<double>("angle", -1.0) > 0.0 ? a.get<double>("angle") : currentPrinter().overhang;
    const glm::dvec3 u = up();
    json out = json::object();
    for (Node* n : meshNodes(a, "printLayFlat"))
    {
        const glm::dmat4 world = worldOf(g, *n);
        const geo::Mesh w = worldMesh(g, *n);
        const Orientation o = bestOrientation(w, u, threshold);
        const glm::dvec3 c = w.bounds().center();
        glm::dmat4 m = about(c, glm::dmat4(o.rotation)) * world;
        if (a.get<bool>("toBed", true))
        {
            double lo = kInf;
            for (Index p = 0; p < w.pointCapacity(); ++p)
                if (w.pointValid(p))
                    lo = std::min(lo, glm::dot(o.rotation * (w.position(p) - c) + c, u));
            glm::dmat4 drop(1.0);
            drop[3] = glm::dvec4(-u * lo, 1.0);
            m = drop * m;
        }
        setWorld(g, *n, m);
        out[g.fullPath(n->id())] = {{"supportArea", o.supportArea}, {"before", o.currentArea}};
    }
    return out;
}

/** depth 0: through the wall into the cavity behind, found by a ray from the surface. */
double holeDepth(const geo::Mesh& m, const SurfacePoint& at, double radius, double depth)
{
    if (depth > 0.0)
        return depth;
    const geo::MeshBvh bvh(m);
    const glm::dvec3 n = glm::normalize(at.normal);
    const double eps = std::max(glm::length(m.bounds().size()) * 1e-6, 1e-9);
    geo::BvhHit hit;
    if (bvh.raycast(at.position - n * eps, -n, hit))
        return hit.t + radius;
    return radius * 2.0;
}

json hollowCmd(const Args& a)
{
    Graph& g = Graph::current();
    const double thicknessMm = a.get<double>("thickness", 2.0);
    if (thicknessMm <= 0.0)
        throw CommandError("printHollow: thickness must be above 0");
    const int holes = a.get<int>("drainHoles", 1);
    const double radiusMm = a.get<double>("holeRadius", 2.5);
    json out = json::object();
    for (Node* n : meshNodes(a, "printHollow"))
    {
        PlugRef plug;
        geo::Mesh base = editable(g, *n, plug);
        const glm::dmat4 world = worldOf(g, *n);
        const double scale = objectScale(world);
        const double before = volume(base) * std::pow(scale, 3.0);
        geo::Mesh result = hollow(base, thicknessMm * kMmToCm / scale);
        int drilled = 0;
        if (holes > 0)
        {
            const glm::dvec3 localUp = glm::normalize(glm::inverse(glm::dmat3(world)) * up());
            const SurfacePoint at = lowestPoint(result, localUp);
            const double r = radiusMm * kMmToCm / scale;
            if (std::string e = drainHole(result, at, r, holeDepth(result, at, r, 0.0)); !e.empty())
                log::warn("printHollow {}: no drain hole: {}", n->name(), e);
            else
                ++drilled;
        }
        const double after = volume(result) * std::pow(scale, 3.0);
        store(g, plug, std::move(result));
        out[g.fullPath(n->id())] = {{"volumeBefore", before}, {"volumeAfter", after}, {"drainHoles", drilled}};
    }
    return out;
}

json drainHoleCmd(const Args& a)
{
    Graph& g = Graph::current();
    Node& n = oneMesh(a, "printDrainHole");
    PlugRef plug;
    geo::Mesh base = editable(g, n, plug);
    const glm::dmat4 world = worldOf(g, n), toLocal = glm::inverse(world);
    const double scale = objectScale(world);

    SurfacePoint at;
    if (auto p = vec3Flag(a, "position"))
    {
        at.position = glm::dvec3(toLocal * glm::dvec4(*p, 1.0));
        if (auto nrm = vec3Flag(a, "normal"))
            at.normal = glm::normalize(glm::transpose(glm::dmat3(world)) * *nrm);
        else
        {
            const geo::MeshBvh bvh(base);
            geo::BvhClosest c;
            if (!bvh.closestPoint(at.position, c))
                throw CommandError("printDrainHole: the mesh has no faces");
            at.position = c.point;
            at.normal = base.faceNormal(c.face);
        }
    }
    else if (components::mode() != ComponentMode::Object && !components::selected(n.id(), components::mode()).empty())
    {
        at.position = glm::dvec3(toLocal * glm::dvec4(scene::selectionCenter(), 1.0));
        at.normal = glm::normalize(glm::transpose(glm::dmat3(world)) * scene::selectionNormal());
    }
    else
        at = lowestPoint(base, glm::normalize(glm::inverse(glm::dmat3(world)) * up()));

    const double r = a.get<double>("radius", 2.5) * kMmToCm / scale;
    const double depth = holeDepth(base, at, r, a.get<double>("depth", 0.0) * kMmToCm / scale);
    if (std::string e = drainHole(base, at, r, depth); !e.empty())
        throw CommandError("printDrainHole: " + e);
    store(g, plug, std::move(base));
    return {{"position", vecJson(glm::dvec3(world * glm::dvec4(at.position, 1.0)))}};
}

json splitCmd(const Args& a)
{
    Graph& g = Graph::current();
    Node& n = oneMesh(a, "printSplit");
    const geo::Mesh w = worldMesh(g, n);
    const glm::dvec3 point = vec3Flag(a, "point").value_or(w.bounds().center());
    const glm::dvec3 normal = vec3Flag(a, "normal").value_or(up());
    PegOptions pegs{.count = a.get<int>("pegs", 1),
                    .radius = a.get<double>("pegRadius", 2.5) * kMmToCm,
                    .length = a.get<double>("pegLength", 6.0) * kMmToCm,
                    .tolerance = a.get<double>("tolerance", 0.2) * kMmToCm};
    SplitResult r = splitByPlane(w, point, normal, pegs);
    if (!r.ok())
        throw CommandError("printSplit: " + r.error);

    const NodeType* type = types::find("mesh");
    json names = json::array();
    std::vector<std::string> made;
    for (auto [suffix, mesh] : {std::pair<const char*, geo::Mesh*>{"Top", &r.above}, {"Bottom", &r.below}})
    {
        Node* part = g.createNode(*type, n.name() + suffix, kNoNode, true);
        g.setValue({part->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(*mesh))));
        gTargets.insert(part->id());
        made.push_back(g.fullPath(part->id()));
    }
    ops::execute("hide", {{"args", {g.fullPath(n.id())}}});
    if (a.get<bool>("layFlat", false))
        ops::execute("printLayFlat", {{"args", made}});
    g.select({});
    for (const std::string& m : made)
        names.push_back(m);
    ops::execute("select", {{"args", made}});
    return {{"parts", names}, {"pegs", r.pegs}};
}

json export3mf(const Args& a)
{
    Graph& g = Graph::current();
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError("export3mf: path required");
    const std::string path = a.positional[0].get<std::string>();
    Args rest = a;
    rest.positional.erase(rest.positional.begin());
    const std::string unit = a.get<std::string>("unit", "mm");
    const glm::dmat4 toFile = glm::inverse(fileToScene(unitCm(unit, "export3mf"), a.get<std::string>("upAxis", "z") == "z"));
    std::vector<threemf::Object> objects;
    for (Node* n : meshNodes(rest, "export3mf"))
        objects.push_back({n->name(), transformed(worldMesh(g, *n), toFile)});
    const std::string data = threemf::write(objects, {.unit = unitName3mf(unit), .colors = a.get<bool>("colors", true)});
    if (data.empty())
        throw CommandError("export3mf: could not build the package");
    std::ofstream f(paths::fromUtf8(path), std::ios::binary | std::ios::trunc);
    if (!f || !f.write(data.data(), std::streamsize(data.size())))
        throw CommandError("export3mf: can't write " + path);
    return path;
}

json import3mf(const Args& a)
{
    Graph& g = Graph::current();
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError("import3mf: path required");
    const std::string path = a.positional[0].get<std::string>();
    std::ifstream f(paths::fromUtf8(path), std::ios::binary);
    if (!f)
        throw CommandError("import3mf: can't read " + path);
    const std::string bytes{std::istreambuf_iterator<char>(f), {}};
    threemf::Model model = threemf::read(bytes);
    if (!model.ok())
        throw CommandError("import3mf: " + model.error);
    double cm = threemf::unitToCm(model.unit);
    if (cm <= 0.0)
        cm = 0.1;
    const glm::dmat4 m = fileToScene(cm, a.get<std::string>("upAxis", "z") == "z");
    const NodeType* type = types::find("mesh");
    json names = json::array();
    for (threemf::Object& o : model.objects)
    {
        Node* n = g.createNode(*type, Graph::validName(o.name) && !o.name.empty() ? o.name : fs::path(paths::fromUtf8(path)).stem().string(),
                               kNoNode, true);
        g.setValue({n->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(transformed(o.mesh, m))));
        gTargets.insert(n->id());
        names.push_back(g.fullPath(n->id()));
    }
    return names;
}

void registerCommands(ext::Context& ctx)
{
    using T = ArgType;
    std::vector<FlagDef> checkFlags = {
        {"minWall", "", T::Float, -1.0, "Thinnest wall that prints, mm; -1 takes the printer's."},
        {"report", "", T::Bool, true, "Replace the meshes' print issues in the Validation panel with this run's."},
    };
    for (size_t i = 0; i < kCheckCount; ++i)
        checkFlags.push_back({checkName(Check(i)), "", T::Bool, true, std::string("Look for ") + checkLabel(Check(i)) + "."});

    ctx.registerCommand({
        .name = "printCheck",
        .doc = "Check meshes (default the selection) for printing: non-manifold edges and points, holes, flipped and zero-area faces, "
               "self-intersections, separate shells and walls under the minimum. Each problem becomes an issue on the mesh whose "
               "click selects its components and whose Fix repairs it where a fix exists. Returns the counts per mesh.",
        .positionalDoc = "meshes",
        .flags = checkFlags,
        .undoable = false,
        .run = [](const Args& a) -> json { return runChecks(meshNodes(a, "printCheck"), checkOptions(a), a.get<bool>("report", true)); },
        .examples = {"printCheck()", "printCheck('Statue', minWall=1.2)"},
    });

    ctx.registerCommand({
        .name = "printSelect",
        .doc = "Select the components of a mesh that one print check finds (holes as edges, thin walls as points...). Returns how many.",
        .positionalDoc = "mesh",
        .flags = {{"check", "", T::String, "holes", "Which check.",
                   {"nonManifold", "holes", "flipped", "zeroArea", "selfIntersections", "shells", "thinWalls"}},
                  {"minWall", "", T::Float, -1.0, "Thinnest wall that prints, mm; -1 takes the printer's."}},
        .run = [](const Args& a) -> json {
            const std::string name = a.get<std::string>("check", "holes");
            for (size_t i = 0; i < kCheckCount; ++i)
                if (name == checkName(Check(i)))
                    return selectCheck(oneMesh(a, "printSelect"), Check(i), checkOptions(a));
            throw CommandError("printSelect: no check named " + name);
        },
    });

    ctx.registerCommand({
        .name = "printAnalyze",
        .doc = "Colour meshes by wall thickness (red under the minimum, green from three times it) or by overhang (red where faces lean "
               "past the support angle from vertical), as a colour set shown unlit with a legend in the viewport. mode='off' removes "
               "the colours. Returns per mesh the thinnest wall in mm, or the overhanging faces and support area in cm2.",
        .positionalDoc = "meshes",
        .flags = {{"mode", "", T::String, "thickness", "What to show.", {"thickness", "overhang", "off"}},
                  {"minWall", "", T::Float, -1.0, "Thickness map: the minimum wall, mm; -1 takes the printer's."},
                  {"angle", "", T::Float, -1.0, "Overhang map: degrees from vertical that print without support; -1 takes the printer's."}},
        .run = [](const Args& a) -> json { return analyze(a); },
        .examples = {"printAnalyze(mode='overhang', angle=50)", "printAnalyze(mode='off')"},
    });

    ctx.registerCommand({
        .name = "printInfo",
        .doc = "Real-world size (mm, width x depth x height), volume (cm3), area, watertightness, shells and support area of meshes, and "
               "whether they fit the printer chosen in Preferences > Print.",
        .positionalDoc = "meshes",
        .undoable = false,
        .run = [](const Args& a) -> json { return info(a); },
    });

    ctx.registerCommand({
        .name = "printScaleToFit",
        .doc = "Scale meshes together, about their base centre, so they fit the printer's build volume. Returns the scale.",
        .positionalDoc = "meshes",
        .flags = {{"margin", "", T::Float, 5.0, "Room kept free at the bed's edges, mm.", {}, {}, 0.0},
                  {"enlarge", "", T::Bool, false, "Also scale up what is smaller than the volume."}},
        .run = [](const Args& a) -> json { return scaleToFit(a); },
    });

    ctx.registerCommand({
        .name = "printLayFlat",
        .doc = "Turn each mesh onto the side that needs the least support (axis sides and its largest flat faces), and stand it on the bed.",
        .positionalDoc = "meshes",
        .flags = {{"angle", "", T::Float, -1.0, "Degrees from vertical that print without support; -1 takes the printer's."},
                  {"toBed", "", T::Bool, true, "Move it down onto the ground plane."}},
        .run = [](const Args& a) -> json { return layFlat(a); },
    });

    ctx.registerCommand({
        .name = "printHollow",
        .doc = "Hollow meshes, keeping a wall of the given thickness (an inner surface offset inward), and drill a drain hole at the "
               "lowest point. For resin prints and to save material. Returns the volumes before and after, cm3.",
        .positionalDoc = "meshes",
        .flags = {{"thickness", "", T::Float, 2.0, "Wall thickness, mm.", {}, {}, 0.01},
                  {"drainHoles", "", T::Int, 1, "Drain holes drilled at the lowest point: 0 or 1.", {}, {}, 0.0, 1.0},
                  {"holeRadius", "", T::Float, 2.5, "Drain hole radius, mm.", {}, {}, 0.01}},
        .run = [](const Args& a) -> json { return hollowCmd(a); },
        .examples = {"printHollow('Bust', thickness=2.5)"},
    });

    ctx.registerCommand({
        .name = "printDrainHole",
        .doc = "Drill a drain hole into a hollowed mesh: at position (world, onto the nearest surface), else at the selected components, "
               "else at the lowest point. Goes through the wall into the cavity unless depth is set.",
        .positionalDoc = "mesh",
        .flags = {{"position", "", T::Vec3, nullptr, "World point on or near the surface."},
                  {"normal", "", T::Vec3, nullptr, "Outward direction to drill against; default the surface's."},
                  {"radius", "", T::Float, 2.5, "mm.", {}, {}, 0.01},
                  {"depth", "", T::Float, 0.0, "mm; 0 goes through into the cavity.", {}, {}, 0.0}},
        .run = [](const Args& a) -> json { return drainHoleCmd(a); },
    });

    ctx.registerCommand({
        .name = "printSplit",
        .doc = "Cut a closed mesh by a plane into two capped parts (<name>Top and <name>Bottom), with registration pegs on the lower part "
               "and matching holes, widened by the tolerance, in the upper. Hides the original. Returns the parts.",
        .positionalDoc = "mesh",
        .flags = {{"point", "", T::Vec3, nullptr, "World point on the plane; default the bounds centre."},
                  {"normal", "", T::Vec3, nullptr, "Plane normal, toward the top part; default up."},
                  {"pegs", "", T::Int, 1, "Pegs along the cut; 0 for none.", {}, {}, 0.0, 8.0},
                  {"pegRadius", "", T::Float, 2.5, "mm.", {}, {}, 0.01},
                  {"pegLength", "", T::Float, 6.0, "How far a peg stands out of the cut, mm.", {}, {}, 0.01},
                  {"tolerance", "", T::Float, 0.2, "Clearance between peg and hole, mm.", {}, {}, 0.0},
                  {"layFlat", "", T::Bool, false, "Turn each part onto its best side afterwards."}},
        .run = [](const Args& a) -> json { return splitCmd(a); },
        .examples = {"printSplit('Statue', point=(0, 20, 0), normal=(0, 1, 0), pegs=2)"},
    });

    ctx.registerCommand({
        .name = "export3mf",
        .doc = "Export meshes (default the selection) to a 3MF file in world space: one object per mesh, triangulated, with the current "
               "colour set as vertex colours. Returns the path.",
        .positionalDoc = "path objects...",
        .flags = {{"unit", "", T::String, "mm", "The file's length unit.", {"mm", "cm", "m", "in"}},
                  {"upAxis", "", T::String, "z", "The file's up axis; 3MF is Z up.", {"y", "z"}},
                  {"colors", "", T::Bool, true, "Write the current colour set."}},
        .undoable = false,
        .run = [](const Args& a) -> json { return export3mf(a); },
    });

    ctx.registerCommand({
        .name = "import3mf",
        .doc = "Import a 3MF file: each built object as a mesh, in the file's unit, with vertex colours as a colour set. Returns the new meshes.",
        .positionalDoc = "path",
        .flags = {{"upAxis", "", T::String, "z", "The file's up axis.", {"y", "z"}}},
        .run = [](const Args& a) -> json { return import3mf(a); },
    });

    io::translators::add(ctx, {.id = "3mf",
                               .label = "3MF",
                               .doc = "3D Manufacturing Format: meshes for 3D printing, with units and colours.",
                               .extensions = {".3mf"},
                               .import = {.command = "import3mf"},
                               .exportSelection = {.command = "export3mf"}});

    ctx.add(dx::validators(), "validator", kSource,
            dx::Validator{"Meshes being prepared for print (ones a Print command or 3MF import has worked on): closed, manifold, consistently "
                          "wound, no self-intersections, walls above the printer's minimum.",
                          [] {
                              Graph& g = Graph::current();
                              std::vector<Node*> nodes;
                              for (NodeId id : gTargets)
                                  if (Node* n = g.node(id); n && n->type().isA("mesh"))
                                      nodes.push_back(n);
                              CheckOptions o;
                              o.minWall = currentPrinter().minWall * kMmToCm;
                              runChecks(nodes, o, true);
                          }});
}

int gExportListener = -1;

/**
 * Meshes written by exportStl become print targets and are checked straight away, so an STL exported for printing
 * gets the same printability issues as one prepared with the Print commands.
 */
void checkStlExport(const ExecuteEvent& e)
{
    if (e.name != "exportStl" || !e.done || !e.ok || ops::previewing())
        return;
    Graph& g = Graph::current();
    std::vector<NodeId> roots;
    const json& args = e.call.contains("args") && e.call["args"].is_array() ? e.call["args"] : json::array();
    for (size_t i = 1; i < args.size(); ++i)
        if (args[i].is_string())
            if (Node* n = g.find(args[i].get<std::string>()))
                roots.push_back(n->id());
    if (roots.empty())
        roots = g.selection();
    std::vector<Node*> nodes;
    std::set<NodeId> seen;
    auto add = [&](NodeId id) {
        if (Node* n = g.node(id); n && n->type().isA("mesh") && seen.insert(id).second)
            nodes.push_back(n);
    };
    for (NodeId r : roots)
    {
        add(r);
        for (NodeId d : g.descendants(r))
            add(d);
    }
    if (nodes.empty())
        return;
    for (Node* n : nodes)
        gTargets.insert(n->id());
    CheckOptions o;
    o.minWall = currentPrinter().minWall * kMmToCm;
    runChecks(nodes, o, true);
    size_t issues = 0;
    for (Node* n : nodes)
        for (const dx::Issue* i : dx::issuesOf(n->id()))
            issues += i->source == kSource;
    if (issues)
        log::warn("Print: {} printability issue(s) on the exported meshes (Validation lists them)", issues);
}

/** "Size 52.0 x 30.0 x 12.0 mm, fits Prusa MK4" for the selection, under an export's unit field. */
std::string selectionSizeNote()
{
    try
    {
        Graph& g = Graph::current();
        std::vector<Node*> nodes = meshNodes(Args{}, "export");
        const glm::dvec3 size = printSize(worldBounds(g, nodes));
        const Printer p = currentPrinter();
        return std::format("Size {}, {} {}", sizeText(size), fitScale(size, p, 0.0) >= 1.0 ? "fits" : "too big for", p.label);
    }
    catch (const std::exception&)
    {
        return {};
    }
}

/** Drain hole placement: armed by the action, the next click on a mesh drills there. */
bool gPlacingHole = false;

bool placeHole(QPointF px, const ui::ViewInfo& view)
{
    if (!gPlacingHole)
        return false;
    gPlacingHole = false;
    Graph& g = Graph::current();
    glm::dvec3 origin, dir;
    view.ray(px, origin, dir);
    Node* best = nullptr;
    double bestT = kInf;
    glm::dvec3 bestPoint(0.0), bestNormal(0.0);
    for (NodeId id : g.selection())
    {
        Node* n = g.node(id);
        if (!n || !n->type().isA("mesh"))
            continue;
        const geo::Mesh w = worldMesh(g, *n);
        const geo::MeshBvh bvh(w);
        geo::BvhHit hit;
        if (bvh.raycast(origin, dir, hit) && hit.t < bestT)
        {
            bestT = hit.t;
            best = n;
            bestPoint = hit.point;
            bestNormal = w.faceNormal(hit.face);
        }
    }
    if (!best)
    {
        ui::viewportToast::showActive("Drain hole: click the selected mesh", 2500, ui::viewportToast::Kind::Error);
        return true;
    }
    ui::actions::runWithOptions("printDrainHole", {{"args", {g.fullPath(best->id())}}, {"position", vecJson(bestPoint)}, {"normal", vecJson(bestNormal)}});
    return true;
}

void registerUi(ext::Context& ctx)
{
    using ops::options::Override;
    const Override::Range mm{0.01, 1000.0, 0.1, " mm"};
    ops::options::setOverride("printCheck", {.order = {"minWall"}, .hidden = {"report"}, .labels = {{"minWall", "Min Wall"}},
                                             .ranges = {{"minWall", {-1.0, 100.0, 0.1, " mm"}}}, .docs = "3d-printing"});
    ops::options::setOverride("printAnalyze", {.order = {"mode", "minWall", "angle"},
                                               .labels = {{"minWall", "Min Wall"}, {"angle", "Support Angle"}},
                                               .choices = {{"mode", {{"thickness", "Wall Thickness"}, {"overhang", "Overhang"}, {"off", "Off"}}}},
                                               .ranges = {{"minWall", {-1.0, 100.0, 0.1, " mm"}}, {"angle", {-1.0, 89.0, 1.0, " deg"}}},
                                               .docs = "3d-printing"});
    ops::options::setOverride("printHollow", {.order = {"thickness", "drainHoles", "holeRadius"},
                                              .labels = {{"drainHoles", "Drain Hole"}},
                                              .ranges = {{"thickness", mm}, {"holeRadius", mm}},
                                              .docs = "3d-printing"});
    ops::options::setOverride("printDrainHole", {.hidden = {"position", "normal"}, .ranges = {{"radius", mm}, {"depth", {0.0, 1000.0, 0.1, " mm"}}},
                                                 .docs = "3d-printing"});
    ops::options::setOverride("printSplit", {.order = {"point", "normal", "pegs", "pegRadius", "pegLength", "tolerance", "layFlat"},
                                             .ranges = {{"pegRadius", mm}, {"pegLength", mm}, {"tolerance", {0.0, 5.0, 0.05, " mm"}}},
                                             .docs = "3d-printing"});
    ops::options::setOverride("printScaleToFit", {.ranges = {{"margin", {0.0, 100.0, 1.0, " mm"}}}, .docs = "3d-printing"});
    ops::options::setOverride("printLayFlat", {.labels = {{"angle", "Support Angle"}, {"toBed", "To Bed"}},
                                               .ranges = {{"angle", {-1.0, 89.0, 1.0, " deg"}}}, .docs = "3d-printing"});
    for (const char* cmd : {"exportStl", "export3mf"})
    {
        Override o;
        if (const Override* existing = ops::options::findOverride(cmd))
            o = *existing;
        o.notes["unit"] = selectionSizeNote;
        ops::options::setOverride(cmd, o);
        ctx.track(std::string("optionNote:") + cmd, [c = std::string(cmd)] {
            if (const Override* existing = ops::options::findOverride(c))
            {
                Override back = *existing;
                back.notes.erase("unit");
                ops::options::setOverride(c, back);
            }
        });
    }

    std::vector<std::pair<std::string, std::string>> choices;
    for (const Printer& p : printers())
        choices.emplace_back(p.label, p.id);
    ui::prefPages::add(ctx, {.id = "print", .title = "Print", .file = kPrefs, .build = [choices](ui::prefPages::PageBuilder& b) {
                                 b.section("Printer");
                                 b.choice("Printer", "/printer", choices, "prusaMk4");
                                 b.section("Custom Printer");
                                 b.number("Width (mm)", "/custom/width", 200.0, 1.0, 5000.0, 1.0, 0);
                                 b.number("Depth (mm)", "/custom/depth", 200.0, 1.0, 5000.0, 1.0, 0);
                                 b.number("Height (mm)", "/custom/height", 200.0, 1.0, 5000.0, 1.0, 0);
                                 b.number("Nozzle (mm)", "/custom/nozzle", 0.4, 0.0, 5.0, 0.05, 2);
                                 b.number("Min wall (mm)", "/custom/minWall", 0.8, 0.05, 50.0, 0.05, 2);
                                 b.number("Support angle", "/custom/overhang", 45.0, 0.0, 89.0, 1.0, 0);
                             }});

    const char* cat = "Print";
    struct Row
    {
        const char* id;
        const char* label;
        const char* command;
        json call;
        const char* doc;
        const char* icon;
    };
    const Row rows[] = {
        {"print.check", "Check", "printCheck", json::object(), "Checks the selected meshes for printing and lists the problems in the Validation panel.", "checklist"},
        {"print.thickness", "Wall Thickness", "printAnalyze", {{"mode", "thickness"}}, "Colours the selected meshes by wall thickness.", "ruler-measure"},
        {"print.overhang", "Overhangs", "printAnalyze", {{"mode", "overhang"}}, "Colours the selected meshes where they need support.", "angle"},
        {"print.analysisOff", "Clear Analysis", "printAnalyze", {{"mode", "off"}}, "Removes the thickness and overhang colours.", "eraser"},
        {"print.info", "Info", "printInfo", json::object(), "Logs size in mm, volume, support area and whether the selection fits the printer.", "info-circle"},
        {"print.scaleToFit", "Scale to Fit", "printScaleToFit", json::object(), "Scales the selection to fit the printer's build volume.", "dimensions"},
        {"print.layFlat", "Lay Flat", "printLayFlat", json::object(), "Turns each selected mesh onto the side needing least support.", "layout-bottombar"},
        {"print.hollow", "Hollow", "printHollow", json::object(), "Hollows the selected meshes with a drain hole at the bottom.", "circle-dashed"},
        {"print.split", "Split", "printSplit", json::object(), "Cuts the selected mesh in two with registration pegs.", "slice"},
    };
    for (const Row& r : rows)
        ui::actions::add(ctx, {.id = r.id, .label = r.label, .category = cat, .doc = r.doc, .python = std::format("{}()", r.command),
                           .command = r.command, .call = r.call, .icon = r.icon});
    ui::actions::add(ctx, {.id = "print.drainHole",
                       .label = "Drain Hole",
                       .category = cat,
                       .run =
                           [] {
                               Graph& g = Graph::current();
                               if (components::mode() != ComponentMode::Object && !g.selection().empty() &&
                                   !components::selected(components::mode()).empty())
                               {
                                   ui::actions::runWithOptions("printDrainHole", json::object());
                                   return;
                               }
                               gPlacingHole = true;
                               ui::viewportToast::showActive("Drain hole: click the selected mesh");
                           },
                       .doc = "Drills a drain hole at the selected components, or where you next click on the selected mesh.",
                       .python = "printDrainHole()",
                       .command = "printDrainHole",
                       .icon = "circle-dot"});
    ui::viewportHooks::setClickHook("print.drainHole", placeHole);
    ctx.track("clickHook:print.drainHole", [] { ui::viewportHooks::removeClickHook("print.drainHole"); });
    ui::viewportHooks::addDrawer(ctx, "print.legend", [](render::FrameState&, ui::OverlayPainter& p, const ui::ViewInfo& view) { drawLegend(p, view); });

    ui::menus::addMenu(ctx, {.id = "Print", .title = "Print", .before = "Rig", .docs = "3d-printing", .sets = {"Print"}});
    auto item = [&](const char* action) { ui::menus::addItem(ctx, {.menu = "Print", .action = action}); };
    auto section = [&](const char* label) { ui::menus::addItem(ctx, {.menu = "Print", .kind = ui::menus::Kind::Section, .label = label}); };
    section("Check");
    item("print.check");
    item("print.thickness");
    item("print.overhang");
    item("print.analysisOff");
    item("print.info");
    section("Prepare");
    item("print.scaleToFit");
    item("print.layFlat");
    item("print.hollow");
    item("print.drainHole");
    item("print.split");

    json shelf = json::array();
    for (auto [label, icon, target] : {std::tuple{"Check", "checklist", "print.check"},
                                       {"Wall", "ruler-measure", "print.thickness"},
                                       {"Overhang", "angle", "print.overhang"},
                                       {"Fit", "dimensions", "print.scaleToFit"},
                                       {"Flat", "layout-bottombar", "print.layFlat"},
                                       {"Hollow", "circle-dashed", "print.hollow"},
                                       {"Drain", "circle-dot", "print.drainHole"},
                                       {"Split", "slice", "print.split"}})
        shelf.push_back({{"label", label}, {"icon", icon}, {"target", target}});
    ui::shelves::add(ctx, {"Print", std::move(shelf)});
}
} // namespace
} // namespace print

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    print::registerCommands(ctx);
    print::registerUi(ctx);
    print::gExportListener = lila::ops::addExecuteListener(print::checkStlExport);
    print::gTargetListener = lila::scene::Graph::current().addListener(
        [](lila::scene::Change c, lila::scene::NodeId id) {
            if (c == lila::scene::Change::Cleared)
                print::gTargets.clear();
            else if (c == lila::scene::Change::Deleted)
                print::gTargets.erase(id);
        },
        true);
    return 0;
}

LILA_EXTENSION_EXPORT void lilaExtensionUninit(lila::ext::Context&)
{
    print::gPlacingHole = false;
    print::gLegend = {};
    if (print::gExportListener >= 0)
        lila::ops::removeExecuteListener(print::gExportListener);
    print::gExportListener = -1;
    if (print::gTargetListener >= 0)
        lila::scene::Graph::current().removeListener(print::gTargetListener);
    print::gTargetListener = -1;
    print::gTargets.clear();
}
