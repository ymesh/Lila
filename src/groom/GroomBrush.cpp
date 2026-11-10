#include "groom/GroomBrush.h"

#include "groom/GroomGraph.h"
#include "groom/GroomLabBridge.h"
#include "groom/GroomStrokeHost.h"
#include "ops/Undo.h"
#include "scene/Graph.h"

#include <groomlab/brush_catalog.h>
#include <groomlab/flow.h>
#include <groomlab/length_key_tool.h>
#include <groomlab/parts.h>
#include <groomlab/stroke.h>
#include <groomlab/scalp_paint.h>
#include <groomlab/texture.h>
#include <groomlab/view_projection.h>

#include <algorithm>
#include <filesystem>
#include <map>

namespace lila::groom
{
namespace
{
using groomlab::StrokeField;
using scene::Graph;
using scene::Node;
using scene::NodeId;

/** The ids hosts start as tools of their own: not strokes. */
bool hostTool(const std::string& id)
{
    return id == "select";
}

/** The tabler icon for each catalog tool. */
std::string iconFor(const std::string& id)
{
    static const std::map<std::string, std::string> icons = {
        {"place", "circle-plus"},   {"draw", "pencil"},    {"part", "separator-vertical"}, {"flow", "arrow-wave-right-up"},
        {"comb", "hand-move"},      {"slide", "arrows-move-horizontal"}, {"smooth", "blur"}, {"lift", "arrow-bar-up"},
        {"twist", "rotate-clockwise"}, {"attract", "magnet"}, {"shape", "cylinder"},        {"cut", "scissors"},
        {"length", "ruler"},        {"lengths", "ruler-measure"}, {"erase", "eraser"},    {"paint", "brush"},
        {"freeze", "snowflake"},
    };
    const auto it = icons.find(id);
    return it == icons.end() ? std::string("brush") : it->second;
}

const char* attrOf(StrokeField field)
{
    switch (field)
    {
    case StrokeField::Guides: return "hierarchy";
    case StrokeField::Parts: return "parts";
    case StrokeField::Flow: return "flow";
    case StrokeField::Correctives: return "correctives";
    case StrokeField::ScalpMaps: return "scalpMaps";
    case StrokeField::Graph: return "graph";
    case StrokeField::Sculpts: return "sculpts";
    case StrokeField::ShotLayer: return nullptr;
    }
    return nullptr;
}

/** Map values equal as stored: maps keep 1/1000. */
bool sameValues(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::abs(a[i] - b[i]) > 2e-3f)
            return false;
    return true;
}

/** Sets an attribute with undo off: a stroke's live state, put back before it lands. */
void setQuietly(Graph& g, const scene::PlugRef& p, scene::PlugValue v)
{
    UndoStack& u = UndoStack::get();
    const bool was = u.enabled();
    u.setEnabled(false);
    g.setValue(p, std::move(v));
    u.setEnabled(was);
}

std::vector<glm::dvec3> world(const std::vector<groomlab::Vec3f>& points)
{
    std::vector<glm::dvec3> out;
    out.reserve(points.size());
    for (const groomlab::Vec3f& p : points)
        out.emplace_back(p.x, p.y, p.z);
    return out;
}

struct EvaluatorHolder
{
    GroomEvaluator evaluator;
};

/** GroomStrokeHost over a groom node: inputs from the node, previews live on it, a stroke landing as one undo step. */
class NodeStrokeHost final : private EvaluatorHolder, public GroomStrokeHost
{
public:
    explicit NodeStrokeHost(NodeId node) : GroomStrokeHost(EvaluatorHolder::evaluator), mNode(node) {}

    NodeId node() const { return mNode; }

    /** Reads the node into the evaluator; false when the node is gone or has no scalp. */
    bool sync()
    {
        Graph& g = Graph::current();
        Node* n = g.node(mNode);
        if (!n || !n->type().isA("groom"))
            return false;
        GroomEvaluator& e = evaluator();
        const scene::PlugValue mesh = g.value({mNode, n->type().attrIndex("inMesh"), -1});
        const scene::PlugValue matrix = g.value({mNode, n->type().attrIndex("meshMatrix"), -1});
        const auto* m = std::get_if<scene::MeshPtr>(&mesh);
        const auto* w = std::get_if<glm::dmat4>(&matrix);
        const geo::Mesh* raw = m && *m ? m->get() : nullptr;
        const glm::dmat4 at = w ? *w : glm::dmat4(1.0);
        if (raw != mScalpFrom || at != mScalpMatrix)
        {
            mScalpFrom = raw;
            mScalpMatrix = at;
            mScalp = raw && raw->numFaces() > 0 ? groomLabBridge::scalpMesh(*raw, at) : nullptr;
            mColorMaps = raw ? groomLabBridge::colorSetMaps(*raw) : groomlab::ScalpMaps{};
            mMapsText = "";
            e.setScalp(mScalp);
        }
        if (!mScalp)
            return false;
        const std::string graph = text(*n, "graph");
        try
        {
            e.setGraphJson(graph.empty() ? groomGraph::defaultGraph() : graph);
        }
        catch (const std::exception& ex)
        {
            mWarnings.push_back(std::string("groom graph: ") + ex.what());
        }
        const std::string hierarchy = text(*n, "hierarchy");
        e.setHierarchyJson(hierarchy.empty() ? groomlab::GuideHierarchy().toJson() : hierarchy);
        e.setPartsJson(text(*n, "parts"));
        e.setFlowJson(text(*n, "flow"));
        const std::string maps = text(*n, "scalpMaps");
        if (mColorMaps.maps.empty())
            e.setScalpMapsJson(maps);
        else if (maps != mMapsText)
        {
            try
            {
                e.setScalpMaps(groomLabBridge::withColorSetMaps(maps, mColorMaps));
            }
            catch (const std::exception& ex)
            {
                mWarnings.push_back(std::string("scalp maps: ") + ex.what());
            }
            mMapsText = maps;
        }
        e.setSculptsJson(text(*n, "sculpts"));
        e.setCorrectivesJson(text(*n, "correctives"));
        mSymmetry = int(std::get<int64_t>(n->value(n->type().attrIndex("symmetry")))) - 1;
        return true;
    }

    /** The node's mirror axis (0 x, 1 y, 2 z), -1 off. */
    int symmetry() const { return mSymmetry; }

    std::string storedText(StrokeField field) override
    {
        // The node's own text for the guides, so a stroke's before is exactly what it held, empty included.
        if (field == StrokeField::Guides || field == StrokeField::ScalpMaps)
            if (Node* n = Graph::current().node(mNode))
                return text(*n, field == StrokeField::Guides ? "hierarchy" : "scalpMaps");
        return GroomStrokeHost::storedText(field);
    }

    void previewHierarchy(std::shared_ptr<groomlab::GuideHierarchy> preview, bool final) override
    {
        GroomStrokeHost::previewHierarchy(preview, final);
        if (preview)
            live(StrokeField::Guides, preview->toJson());
    }

    void previewOverlays(std::shared_ptr<const groomlab::PartSet> parts, std::shared_ptr<const groomlab::FlowSet> flow) override
    {
        GroomStrokeHost::previewOverlays(parts, flow);
        if (parts)
            live(StrokeField::Parts, parts->toJson());
        if (flow)
            live(StrokeField::Flow, flow->toJson());
    }

    void liveStroke(bool on) override
    {
        GroomStrokeHost::liveStroke(on);
        if (!on)
            restore();
    }

    void commit(groomlab::StrokeEdit edit) override
    {
        restore();
        Graph& g = Graph::current();
        Node* n = g.node(mNode);
        if (!n)
            return;
        UndoStack& u = UndoStack::get();
        u.openChunk("Groom Brush");
        for (std::size_t i = 0; i < groomlab::kStrokeFields; ++i)
        {
            groomlab::StrokeEdit::Text& t = edit.texts[i];
            const char* attr = attrOf(StrokeField(i));
            if (!t.set || !attr)
                continue;
            t.before = storedText(StrokeField(i));
            t.hasBefore = true;
            if (StrokeField(i) == StrokeField::ScalpMaps)
                t.after = withoutColorSetMaps(t.before, t.after);
            if (t.after != t.before)
                g.setValue({mNode, n->type().attrIndex(attr), -1}, t.after);
        }
        u.closeChunk();
        if (!edit.textureMap.empty())
            if (const auto paint = mTexturePaints.find(edit.textureMap); paint != mTexturePaints.end())
            {
                const std::string& file = mTextureFiles[edit.textureMap];
                if (file.empty())
                    mWarnings.push_back("texture map " + edit.textureMap + " has no file to save to");
                else
                    paint->second->save(file);
            }
        applyStrokeEdit(edit, evaluator(), true);
        evaluator().setPreviewHierarchy(nullptr);
        evaluator().setPreviewOverlays(nullptr, nullptr);
    }

    void warn(const std::string& message) override { mWarnings.push_back(message); }

    /**
     * A texture map's paint: the map's image file read onto the scalp's uv0, or a blank one at `resolution` for a map
     * with no file yet. Kept between strokes; each stroke that paints it writes the file (PNG).
     */
    std::shared_ptr<groomlab::TexturePaint> texturePaint(const std::string& name, int channels, int resolution) override
    {
        if (const auto it = mTexturePaints.find(name); it != mTexturePaints.end())
            return it->second;
        const groomlab::ScalpMaps& maps = evaluator().scalpMaps();
        const auto entry = maps.maps.find(name);
        if (entry == maps.maps.end() || entry->second.kind != "texture")
            throw std::runtime_error("this groom has no texture map named " + name);
        const groomlab::MeshPtr scalp = paintScalp();
        if (!scalp || !scalp->hasUVs())
            throw std::runtime_error("the scalp has no uv0 to paint texture map " + name + " through");
        std::shared_ptr<groomlab::TexturePaint> paint;
        const std::string& file = entry->second.texture;
        if (!file.empty() && std::filesystem::exists(file))
            if (const groomlab::TexturePtr image = groomlab::loadTexture(file))
                paint = std::make_shared<groomlab::TexturePaint>(*scalp, *image, channels);
        if (!paint)
            paint = std::make_shared<groomlab::TexturePaint>(*scalp, resolution, resolution, 0.0f);
        mTexturePaints[name] = paint;
        mTextureFiles[name] = file;
        return paint;
    }

    void dropTexturePaint() override { mTexturePaints.clear(); }

    std::vector<std::string> takeWarnings() { return std::exchange(mWarnings, {}); }

private:
    static std::string text(const Node& n, const char* attr)
    {
        const int a = n.type().attrIndex(attr);
        return a < 0 ? std::string() : std::get<std::string>(n.value(a));
    }

    /**
     * The maps a stroke stores, less the scalp's color set maps it was handed: those stay on the mesh, painted with
     * vertex paint. A stroke on one of them is dropped with a warning.
     */
    std::string withoutColorSetMaps(const std::string& before, const std::string& after)
    {
        if (mColorMaps.maps.empty())
            return after;
        try
        {
            const groomlab::ScalpMaps stored = groomlab::ScalpMaps::fromJson(before);
            groomlab::ScalpMaps out = groomlab::ScalpMaps::fromJson(after);
            for (const auto& [name, entry] : mColorMaps.maps)
            {
                if (stored.maps.count(name))
                    continue;
                const auto it = out.maps.find(name);
                if (it == out.maps.end())
                    continue;
                if (!sameValues(it->second.values, entry.values))
                    mWarnings.push_back("map " + name + " is the scalp's color set: paint it with Vertex Paint");
                out.maps.erase(it);
            }
            return out.maps.empty() && before.empty() ? std::string() : out.toJson();
        }
        catch (const std::exception&)
        {
            return after;
        }
    }

    /** Writes a stroke's work in progress to the node, keeping what it held to put back. */
    void live(StrokeField field, std::string value)
    {
        Graph& g = Graph::current();
        Node* n = g.node(mNode);
        const char* attr = attrOf(field);
        if (!n || !attr)
            return;
        const scene::PlugRef p{mNode, n->type().attrIndex(attr), -1};
        mStart.try_emplace(field, text(*n, attr));
        setQuietly(g, p, std::move(value));
    }

    void restore()
    {
        Graph& g = Graph::current();
        if (Node* n = g.node(mNode))
            for (const auto& [field, value] : mStart)
                setQuietly(g, {mNode, n->type().attrIndex(attrOf(field)), -1}, value);
        mStart.clear();
    }

    GroomEvaluator& evaluator() { return EvaluatorHolder::evaluator; }

    NodeId mNode;
    const geo::Mesh* mScalpFrom = nullptr;
    glm::dmat4 mScalpMatrix{0.0};
    groomlab::MeshPtr mScalp;
    int mSymmetry = -1;
    groomlab::ScalpMaps mColorMaps;
    std::string mMapsText;
    std::map<StrokeField, std::string> mStart;
    std::map<std::string, std::shared_ptr<groomlab::TexturePaint>> mTexturePaints;
    std::map<std::string, std::string> mTextureFiles;
    std::vector<std::string> mWarnings;
};
} // namespace

const std::vector<GroomBrushEntry>& groomBrushCatalog()
{
    static const std::vector<GroomBrushEntry> entries = [] {
        std::vector<GroomBrushEntry> out;
        for (const groomlab::BrushEntry& e : groomlab::brushCatalog())
        {
            if (hostTool(e.id))
                continue;
            out.push_back({e.id, e.label, groomlab::brushCategoryName(e.category), iconFor(e.id), e.hint, e.defaultStrength, e.signedStrength, e.settings});
        }
        return out;
    }();
    return entries;
}

std::vector<std::string> groomBrushCategories()
{
    std::vector<std::string> out;
    for (groomlab::BrushCategory c : groomlab::brushCategories())
        out.emplace_back(groomlab::brushCategoryName(c));
    return out;
}

const GroomBrushEntry* groomBrushEntry(const std::string& id)
{
    for (const GroomBrushEntry& e : groomBrushCatalog())
        if (e.id == id)
            return &e;
    return nullptr;
}

std::pair<std::string, std::string> groomBrushSettingText(const std::string& id)
{
    if (const groomlab::BrushSettingSpec* s = groomlab::brushSettingSpec(id))
        return {s->label, s->hint};
    return {id, {}};
}

struct GroomBrush::Impl
{
    NodeId node = scene::kNoNode;
    std::unique_ptr<NodeStrokeHost> host;
    groomlab::BrushTool tool;
    GroomBrushSettings settings;
    std::vector<std::string> warnings;
    double height = 1.0;
    std::string lastGuides;

    /** The host for the groom, synced from it; null (with a warning) when there is nothing to brush. */
    NodeStrokeHost* ready()
    {
        if (node == scene::kNoNode)
            return nullptr;
        if (!host || host->node() != node)
            host = std::make_unique<NodeStrokeHost>(node);
        if (!tool.stroking() && !host->sync())
        {
            warnings.push_back("the groom has no scalp to brush");
            return nullptr;
        }
        // Guides changed under the tool (undo, a script): Length Keys reads its keys again.
        std::string guides = host->storedText(StrokeField::Guides);
        if (guides != lastGuides && !tool.lengthsPressed() && !tool.lengths().edited())
            tool.lengths().reset(host.get());
        lastGuides = std::move(guides);
        return host.get();
    }

    void apply()
    {
        groomlab::BrushToolSettings& t = tool.settings;
        const GroomBrushSettings& s = settings;
        try
        {
            t.setMode(s.mode == "freeze" ? "paint" : s.mode);
        }
        catch (const std::exception& e)
        {
            warnings.push_back(e.what());
        }
        t.setScreenRadius(float(s.radius));
        t.brush.strength = float(s.strength);
        t.setHardness(float(1.0 - std::clamp(s.falloff, 0.0, 1.0)));
        t.pressure = s.pressure;
        t.brush.collide = s.collide;
        t.maskByScalp = s.frontOnly;
        t.brush.stretch = s.stretch;
        t.selectedOnly = s.selectedOnly;
        t.brush.tubeChannel = groomlab::tubeChannelFromString(s.tubeChannel);
        try
        {
            t.setTargets(s.targets.empty() ? "guides" : s.targets);
        }
        catch (const std::exception& e)
        {
            warnings.push_back(e.what());
        }
        t.place.length = float(s.placeLength);
        t.setSegments(s.placeSegments);
        t.place.spacing = float(s.placeSpacing);
        t.place.matchNeighbors = s.matchNeighbors;
        t.place.drape = true;
        t.place.group = s.group.empty() ? "primary" : s.group;
        t.brush.level = s.level;
        if (s.mode == "freeze")
        {
            t.brush.attribute = "freeze";
            t.brush.value = 1.0f;
            t.paintKind = "guides";
        }
        else
        {
            t.brush.attribute = s.attribute;
            t.brush.value = float(s.value);
            t.paintKind = s.paintKind;
            if (s.paintKind == "texture")
                t.paintTexture = s.attribute;
        }
        t.smoothStrength = float(s.smoothStrength);
        t.partName = s.partName;
        t.flowSink = s.flowSink;
        const int axis = host ? host->symmetry() : -1;
        t.brush.mirrorAxis = axis;
        t.brush.mirrorCenter = 0.0f;
    }

    /** GroomLab's view: port pixels from the bottom left, as its tools take them. */
    groomlab::ToolView toolView(const GroomBrushView& v)
    {
        height = std::max(1.0, v.height);
        groomlab::ToolView tv;
        const double h = height;
        tv.hostProject = [v, h](const groomlab::Vec3f& p, float& x, float& y) {
            const std::optional<glm::dvec2> q = v.project ? v.project(glm::dvec3(p.x, p.y, p.z)) : std::nullopt;
            if (!q)
                return false;
            x = float(q->x);
            y = float(h - q->y);
            return true;
        };
        tv.hostRay = [v, h](float x, float y, groomlab::Vec3f& origin, groomlab::Vec3f& direction) {
            if (!v.ray)
                return false;
            glm::dvec3 o, d;
            v.ray({x, h - y}, o, d);
            d = glm::normalize(d);
            origin = groomlab::Vec3f(float(o.x), float(o.y), float(o.z));
            direction = groomlab::Vec3f(float(d.x), float(d.y), float(d.z));
            return true;
        };
        tv.hasCamera = true;
        tv.eye = groomlab::Vec3f(float(v.eye.x), float(v.eye.y), float(v.eye.z));
        tv.viewDirection = groomlab::Vec3f(float(v.viewDir.x), float(v.viewDir.y), float(v.viewDir.z));
        tv.orthographic = v.ortho;
        return tv;
    }

    void collect()
    {
        if (host)
            for (std::string& w : host->takeWarnings())
                warnings.push_back(std::move(w));
    }
};

GroomBrush::GroomBrush() : m(std::make_unique<Impl>()) {}

GroomBrush::~GroomBrush()
{
    cancel();
}

void GroomBrush::setGroom(NodeId groom)
{
    if (groom == m->node)
        return;
    cancel();
    m->node = groom;
    m->host.reset();
}

NodeId GroomBrush::groom() const
{
    return m->node;
}

void GroomBrush::setSettings(const GroomBrushSettings& settings)
{
    m->settings = settings;
}

const GroomBrushSettings& GroomBrush::settings() const
{
    return m->settings;
}

bool GroomBrush::press(const GroomBrushView& view, double x, double y, double pressure, unsigned modifiers)
{
    NodeStrokeHost* host = m->ready();
    if (!host)
        return false;
    m->apply();
    const groomlab::ToolView tv = m->toolView(view);
    unsigned mods = 0;
    if (modifiers & kGroomBrushCtrl)
        mods |= groomlab::kStrokeCtrl;
    if (modifiers & kGroomBrushShift)
        mods |= groomlab::kStrokeShift;
    try
    {
        m->tool.press(*host, tv, float(x), float(m->height - y), float(pressure), mods);
    }
    catch (const std::exception& e)
    {
        m->tool.cancel(host);
        m->warnings.push_back(e.what());
    }
    m->collect();
    return m->tool.stroking() || m->tool.shiftPending() || m->tool.lengthsPressed();
}

void GroomBrush::drag(const GroomBrushView& view, double x, double y, double pressure)
{
    if (!m->host)
        return;
    const groomlab::ToolView tv = m->toolView(view);
    try
    {
        m->tool.drag(*m->host, tv, float(x), float(m->height - y), float(pressure));
    }
    catch (const std::exception& e)
    {
        m->tool.cancel(m->host.get());
        m->warnings.push_back(e.what());
    }
    m->collect();
}

bool GroomBrush::release(const GroomBrushView& view)
{
    if (!m->host)
        return false;
    const groomlab::ToolView tv = m->toolView(view);
    bool changed = false;
    try
    {
        m->tool.release(*m->host, tv);
        changed = m->tool.changed();
    }
    catch (const std::exception& e)
    {
        m->tool.cancel(m->host.get());
        m->warnings.push_back(e.what());
    }
    m->collect();
    return changed;
}

void GroomBrush::cancel()
{
    if (m->tool.stroking() || m->tool.shiftPending())
        m->tool.cancel(m->host.get());
    if (m->host)
        m->host->liveStroke(false);
}

bool GroomBrush::stroking() const
{
    return m->tool.stroking();
}

std::vector<std::vector<glm::dvec3>> GroomBrush::affectedLines(const GroomBrushView& view, double x, double y)
{
    std::vector<std::vector<glm::dvec3>> out;
    NodeStrokeHost* host = m->tool.stroking() ? m->host.get() : m->ready();
    m->warnings.clear();
    if (!host)
        return out;
    m->apply();
    try
    {
        for (const auto& line : m->tool.affectedLines(*host, m->toolView(view), float(x), float(m->height - y)))
            out.push_back(world(line));
    }
    catch (const std::exception&)
    {
    }
    return out;
}

std::vector<glm::dvec3> GroomBrush::drawnPath() const
{
    return world(m->tool.drawnPath());
}

std::vector<std::vector<glm::dvec3>> GroomBrush::guideLines()
{
    std::vector<std::vector<glm::dvec3>> out;
    NodeStrokeHost* host = m->tool.stroking() ? m->host.get() : m->ready();
    m->warnings.clear();
    if (!host)
        return out;
    if (const std::shared_ptr<const groomlab::GuideHierarchy> shown = host->shownHierarchy())
        for (const groomlab::HierarchyGuide& g : shown->guides())
            out.push_back(world(g.points));
    return out;
}

int GroomBrush::pickLevel(const GroomBrushView& view, double x, double y)
{
    NodeStrokeHost* host = m->ready();
    if (!host)
        return -2;
    m->apply();
    const int level = m->tool.pickLevel(*host, m->toolView(view), float(x), float(m->height - y));
    if (level >= -1)
    {
        m->settings.level = m->tool.settings.brush.level;
        m->settings.group = m->tool.settings.brush.group;
    }
    return level;
}

std::vector<GroomBrush::ColoredLine> GroomBrush::lengthKeyLines(const GroomBrushView& view, double x, double y)
{
    std::vector<ColoredLine> out;
    NodeStrokeHost* host = m->tool.lengthsPressed() ? m->host.get() : m->ready();
    m->warnings.clear();
    if (!host)
        return out;
    m->apply();
    try
    {
        for (const auto& line : m->tool.lengths().overlayLines(*host, m->toolView(view), m->tool.settings, float(x), float(m->height - y)))
            out.push_back({world(line.points), glm::vec3(line.color[0], line.color[1], line.color[2])});
    }
    catch (const std::exception&)
    {
    }
    return out;
}

bool GroomBrush::lengthKeysEdited() const
{
    return m->tool.lengths().edited();
}

bool GroomBrush::applyLengthKeys()
{
    NodeStrokeHost* host = m->ready();
    if (!host)
        return false;
    m->apply();
    bool applied = false;
    try
    {
        applied = m->tool.lengths().apply(*host, m->tool.settings);
    }
    catch (const std::exception& e)
    {
        m->warnings.push_back(e.what());
    }
    m->collect();
    return applied;
}

void GroomBrush::cancelLengthKeys()
{
    if (NodeStrokeHost* host = m->ready())
        m->tool.lengths().cancel(*host);
}

void GroomBrush::deleteLengthKey()
{
    if (NodeStrokeHost* host = m->ready())
    {
        m->apply();
        m->tool.lengths().deleteKey(*host, m->tool.settings);
    }
}

void GroomBrush::clearLengthKeys()
{
    if (NodeStrokeHost* host = m->ready())
    {
        m->apply();
        m->tool.lengths().clear(*host, m->tool.settings);
    }
}

std::vector<std::string> GroomBrush::takeWarnings()
{
    m->collect();
    return std::exchange(m->warnings, {});
}
} // namespace lila::groom
