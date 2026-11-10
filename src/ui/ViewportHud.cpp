#include "ui/ViewportHud.h"

#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/Anim.h"
#include "scene/Components.h"
#include "scene/HudStats.h"
#include "scene/Isolation.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Relocate.h"
#include "scene/SoftSelect.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/LocateFlash.h"
#include "ui/MeshCache.h"
#include "ui/OptionBox.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportWidget.h"
#include "uv/Metrics.h"
#include "uv/Uv.h"
#include "uv/UvEdit.h"

#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace lila::ui::hud
{
namespace
{
constexpr const char* kTexelResolution = "/viewport/hud/texelResolution";
constexpr double kMargin = 12.0;
constexpr double kTop = 10.0;
constexpr double kLine = 16.0;
constexpr double kAscent = 12.0; // baseline below a row's top
constexpr double kBlockGap = 8.0;
constexpr double kColumnGap = 14.0;
constexpr double kCountGap = 20.0; // between count columns, which are all digits and need more air than words
constexpr float kText = 0.8f; // OverlayPainter::ink shades
constexpr float kDim = 0.6f;
constexpr float kAlpha = 0.9f;

std::string itemKey(int panel, std::string_view item)
{
    return "/viewport/hud/panel" + std::to_string(panel) + "/" + std::string(item);
}

std::string partKey(int panel, std::string_view item, std::string_view part)
{
    return itemKey(panel, "parts/" + std::string(item) + "/" + std::string(part));
}

bool itemDefault(std::string_view item)
{
    if (item == "polyCount")
        return PrefStore::get().value<bool>("general", "/viewport/polyCount", true);
    if (item == "viewSwitcher")
        return viewportDisplay::viewSwitcherDefault();
    return item == "cameraNames" || item == "viewAxis" || item == "viewCube" || item == "symmetry";
}

/** Whether a part of the item being built is on, by part id. */
using PartOn = std::function<bool(const char*)>;

struct Row
{
    QString label;
    std::vector<QString> values;
    /** Each value column is at least this wide, so a count can change without the block resizing. */
    double minWidth = 0.0;
    double gap = kColumnGap;
    /** Column names, drawn dim like the labels. */
    bool header = false;
};
using Block = std::vector<Row>;

/** Label column, then each value column right-aligned to its widest entry. */
struct Columns
{
    double label = 0.0;
    double gap = kColumnGap;
    std::vector<double> values;

    explicit Columns(const Block& b)
    {
        for (const Row& r : b)
        {
            label = std::max(label, double(OverlayPainter::textWidth(r.label)));
            gap = std::max(gap, r.gap);
            if (values.size() < r.values.size())
                values.resize(r.values.size(), 0.0);
            for (size_t i = 0; i < r.values.size(); ++i)
                values[i] = std::max({values[i], r.minWidth, double(OverlayPainter::textWidth(r.values[i]))});
        }
    }
    double width() const
    {
        double w = label;
        for (double v : values)
            w += gap + v;
        return w;
    }
};

QSizeF sizeOf(const Block& b)
{
    return {Columns(b).width(), kLine * double(b.size())};
}

void drawBlock(OverlayPainter& p, const Block& b, const QRectF& at)
{
    Columns cols(b);
    // A soft plate keeps the block legible over a mid-gray grid or a bright model, where neither light nor dark ink holds up.
    p.rect(at.adjusted(-6, -3, 6, 3), {0.07f, 0.07f, 0.07f, 0.45f});
    const glm::vec4 text{kText, kText, kText, kAlpha}, dim{kDim, kDim, kDim, kAlpha};
    double y = at.top() + kAscent;
    for (const Row& r : b)
    {
        p.text(QPointF(at.left(), y), r.label, dim);
        double x = at.left() + cols.label;
        for (size_t i = 0; i < r.values.size(); ++i)
        {
            x += cols.gap + cols.values[i];
            p.text(QPointF(x, y), r.values[i], r.header ? dim : text, OverlayPainter::Right);
        }
        y += kLine;
    }
}

QString count(uint64_t n) { return QString::number(n); }

/** Room for a seven digit count; larger ones widen the block. */
double countWidth()
{
    static const double w = OverlayPainter::textWidth("0000000");
    return w;
}

QString fixed(double v, int decimals = 2) { return QString::number(v, 'f', decimals); }

QString capitalized(const char* s)
{
    QString out = QString::fromLatin1(s);
    if (!out.isEmpty())
        out[0] = out[0].toUpper();
    return out;
}

/** The selected meshes, lead (last selected) last. */
std::vector<scene::Node*> selectedMeshes(scene::Graph& g)
{
    std::vector<scene::Node*> out;
    for (scene::NodeId id : g.selection())
        if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
            out.push_back(n);
    return out;
}

Block polyCountBlock(const scene::hud::PolyStats& s, const PartOn& on)
{
    using PC = scene::hud::PolyCount;
    Block b{{"", {"Scene", "Selected", "Components"}, countWidth(), kCountGap, true}};
    for (auto [part, label, field] : {std::tuple{"verts", "Verts:", &PC::verts}, {"edges", "Edges:", &PC::edges}, {"faces", "Faces:", &PC::faces},
                                      {"tris", "Tris:", &PC::tris}, {"uvs", "UVs:", &PC::uvs}})
        if (on(part))
        {
            b.push_back({label, {count(s.scene.*field), count(s.objects.*field), count(s.components.*field)}, countWidth(), kCountGap});
            // Instances draw their mesh again without adding data: what the scene holds next to what it draws.
            if (field == &PC::tris && s.uniqueTris != s.scene.tris)
                b.push_back({"Unique Tris:", {count(s.uniqueTris), count(s.uniqueObjectTris)}, countWidth(), kCountGap});
        }
    if (b.size() == 1)
        b.clear();
    return b;
}

Block smoothBlock(scene::Graph& g, const Context& ctx, const PartOn& on)
{
    uint64_t objects = 0, faces[2] = {}, tris[2] = {};
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n->type().isA("mesh") || !scene::hud::visibleInHierarchy(g, id) || (ctx.shownNode && !ctx.shownNode(id)))
            continue;
        int level = int(n->as<int64_t>(scene::MeshNode::kSmoothPreview));
        const scene::MeshPtr& cage = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        if (level <= 0 || !cage)
            continue;
        // The viewport draws this level already, so the subdivided snapshot comes from its cache.
        scene::MeshPtr smooth = smoothedFor(cage, level);
        if (!smooth)
            continue;
        ++objects;
        bool picked = g.isSelected(id);
        for (int c = 0; c < (picked ? 2 : 1); ++c)
        {
            faces[c] += smooth->numFaces();
            tris[c] += uint64_t(smooth->numCorners()) - 2 * uint64_t(smooth->numFaces());
        }
    }
    Block b;
    if (on("objects"))
        b.push_back({"Smooth Preview:", {objects == 0 ? QString("Off") : count(objects) + (objects == 1 ? " object" : " objects")}});
    if (objects == 0)
        return b;
    if (on("faces"))
        b.push_back({"Smooth Faces:", {count(faces[0]), count(faces[1])}, countWidth(), kCountGap});
    if (on("tris"))
        b.push_back({"Smooth Tris:", {count(tris[0]), count(tris[1])}, countWidth(), kCountGap});
    return b;
}

/** Whether the view draws the node: hidden things never count. */
bool drawnIn(scene::Graph& g, const Context& ctx, scene::NodeId id)
{
    return g.node(id) && scene::hud::visibleInHierarchy(g, id) && (!ctx.shownNode || ctx.shownNode(id));
}

uint64_t drawnSelectedObjects(scene::Graph& g, const Context& ctx)
{
    return uint64_t(std::ranges::count_if(g.selection(), [&](scene::NodeId id) { return drawnIn(g, ctx, id); }));
}

/** Selected components the view draws: on drawn nodes, and on a mesh isolated by components only those on its shown faces. */
uint64_t drawnSelectedComponents(scene::Graph& g, const Context& ctx, scene::ComponentMode mode)
{
    uint64_t n = 0;
    for (const auto& [node, ids] : scene::components::selected(mode))
    {
        if (!drawnIn(g, ctx, node))
            continue;
        const scene::Node* mn = g.node(node);
        if (ctx.isolation && ctx.isolation->isPartial(node) && mn->type().isA("mesh"))
            if (const scene::MeshPtr& mesh = mn->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
            {
                const auto vis = scene::isolation::visibility(*mesh, ctx.isolation->shownFaces(node, *mesh));
                n += scene::isolation::visibleOnly(vis, mode, ids).size();
                continue;
            }
        n += ids.size();
    }
    return n;
}

Block selectionBlock(scene::Graph& g, const Context& ctx, const PartOn& on)
{
    scene::ComponentMode mode = scene::components::mode();
    QString what;
    if (mode == scene::ComponentMode::Object)
    {
        const uint64_t n = drawnSelectedObjects(g, ctx);
        what = count(n) + (n == 1 ? " object" : " objects");
    }
    else
    {
        const uint64_t n = drawnSelectedComponents(g, ctx, mode);
        const char* kind = mode == scene::ComponentMode::Vertex ? "vert" : mode == scene::ComponentMode::Edge ? "edge" : "face";
        what = count(n) + " " + kind + (n == 1 ? "" : "s");
    }
    Block b;
    if (on("mode"))
        b.push_back({"Select Mode:", {capitalized(scene::components::modeName(mode))}});
    if (on("selected"))
        b.push_back({"Selected:", {what}});
    return b;
}

/** Texel density of the lead mesh (its selected faces in face mode), recomputed only when something it reads changes. */
class TexelDensityCache
{
public:
    std::optional<double> get(scene::Graph& g, int resolution)
    {
        std::vector<scene::Node*> meshes = selectedMeshes(g);
        scene::ComponentMode mode = scene::components::mode();
        scene::Node* lead = meshes.empty() ? nullptr : meshes.back();
        if (mode == scene::ComponentMode::Face)
            for (const auto& [node, ids] : scene::components::selected(mode))
                if (!ids.empty())
                    lead = g.node(node);
        if (!lead)
            return std::nullopt;
        const scene::MeshPtr& mesh = lead->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        if (!mesh)
            return std::nullopt;
        glm::dmat3 linear(lead->as<glm::dmat4>(scene::Transform::kWorldMatrix));
        const std::string& uvSet = uv::currentUvSet();
        if (mNode != lead->id() || mMesh.lock() != mesh || mLinear != linear || mGeneration != generation() || mResolution != resolution ||
            mMode != mode || mUvSet != uvSet)
        {
            std::span<const uint32_t> faces;
            if (mode == scene::ComponentMode::Face)
                faces = scene::components::selected(lead->id(), mode);
            // Selected face ids are the cage's; with modifiers the density is measured on the export, per cage face.
            if (const scene::MeshPtr cage = uv::exportStack(*lead).empty() ? nullptr : scene::modifiers::cage(g, lead->id()))
            {
                const uv::FinalMap trace = uv::exportTraceOf(*cage, *lead, true);
                mValue = uv::texelDensity(*cage, uvSet, faces, double(resolution), linear, &trace);
            }
            else
                mValue = uv::texelDensity(*mesh, uvSet, faces, double(resolution), linear);
            mNode = lead->id();
            mMesh = mesh;
            mLinear = linear;
            mGeneration = generation();
            mResolution = resolution;
            mMode = mode;
            mUvSet = uvSet;
        }
        return mValue;
    }

private:
    static uint64_t& counter()
    {
        static uint64_t n = 1;
        return n;
    }
    static uint64_t generation()
    {
        static int listener = scene::components::addListener([] { ++counter(); });
        (void)listener;
        return counter();
    }

    scene::NodeId mNode = scene::kNoNode;
    std::weak_ptr<const geo::Mesh> mMesh;
    glm::dmat3 mLinear{0.0};
    uint64_t mGeneration = 0;
    int mResolution = 0;
    scene::ComponentMode mMode = scene::ComponentMode::Object;
    std::string mUvSet;
    double mValue = 0.0;
};

Block texelBlock(scene::Graph& g, const PartOn& on)
{
    static TexelDensityCache cache;
    int res = PrefStore::get().value<int>("general", kTexelResolution, 1024);
    Block b;
    if (on("density"))
    {
        std::optional<double> d = cache.get(g, res);
        b.push_back({"Texel Density:", {d ? fixed(*d) + " px/cm" : QString("-")}});
    }
    if (on("size"))
        b.push_back({"Texture Size:", {count(uint64_t(res))}});
    return b;
}

Block objectBlock(scene::Graph& g, const Context& ctx, const PartOn& on)
{
    const std::vector<scene::NodeId>& sel = g.selection();
    Block b;
    if (scene::Node* n = sel.empty() ? nullptr : g.node(sel.back()))
    {
        if (on("name"))
            b.push_back({"Object:", {QString::fromStdString(n->path())}});
        if (on("type"))
            b.push_back({"Type:", {QString::fromStdString(n->type().name)}});
        if (n->type().dag)
        {
            if (on("visibility"))
                b.push_back({"Visibility:", {scene::hud::visibleInHierarchy(g, n->id()) ? "On" : "Off"}});
            glm::dvec3 at(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
            if (on("distance"))
                b.push_back({"Distance From Camera:", {fixed(glm::length(at - ctx.view.eye))}});
        }
        if (n->type().isA("mesh") && on("smoothness"))
        {
            int64_t level = n->as<int64_t>(scene::MeshNode::kSmoothPreview);
            b.push_back({"Smoothness:", {level > 0 ? "Level " + QString::number(level) : QString("Off")}});
        }
    }
    if (on("count"))
        b.push_back({"Selected Objects:", {count(drawnSelectedObjects(g, ctx))}});
    return b;
}

/** The selected meshes' shared symmetry, nullopt with none selected. Mixed axes read as on, "Mixed". */
std::optional<ToolSymmetry> selectionSymmetry(scene::Graph& g)
{
    std::optional<QString> label;
    bool mixed = false;
    for (scene::Node* n : selectedMeshes(g))
    {
        const std::optional<geo::SymmetrySpec> spec = scene::symmetrySpec(*n);
        const QString a = !spec ? QString() : spec->radial ? "Radial " + QString::number(spec->radial->count()) + (spec->radial->mirror() ? " + mirror" : "")
                                                           : "Object " + QString(QChar('X' + spec->axis));
        mixed = mixed || (label && *label != a);
        label = a;
    }
    if (!label)
        return std::nullopt;
    if (mixed)
        return ToolSymmetry{true, "Mixed"};
    return ToolSymmetry{!label->isEmpty(), *label};
}

Block softSelectBlock(const PartOn& on)
{
    namespace ss = scene::softSelect;
    const PrefStore& prefs = PrefStore::get();
    Block b;
    if (!prefs.value<bool>("general", ss::kEnabled, false))
        return b;
    if (on("state"))
        b.push_back({"Soft Select:", {"On"}});
    if (on("radius"))
        b.push_back({"Falloff Radius:", {fixed(prefs.value<double>("general", ss::kRadius, 25.0))}});
    if (on("mode"))
        b.push_back({"Falloff Mode:", {capitalized(prefs.value<std::string>("general", ss::kFalloffMode, "volume").c_str())}});
    return b;
}

Block frameBlock(scene::Graph& g)
{
    double frame = scene::anim::frameOf(g.time());
    bool whole = std::abs(frame - std::round(frame)) < 1e-6;
    return {{"Current Frame:", {whole ? QString::number(int64_t(std::round(frame))) : fixed(frame)}}};
}

/** Places blocks top-down (or bottom-up) at one edge, remembering the rectangles taken. */
struct Stack
{
    bool right = false;
    bool upward = false;
    double edge = 0.0; // the y the next block starts from
    double width = 0.0;
    std::vector<std::pair<Block, QRectF>> placed;

    QRectF place(const Block& b)
    {
        if (b.empty())
            return {};
        QSizeF s = sizeOf(b);
        double x = right ? width - kMargin - s.width() : kMargin;
        double y = upward ? edge - s.height() : edge;
        QRectF r(QPointF(x, y), s);
        edge = upward ? y - kBlockGap : y + s.height() + kBlockGap;
        placed.push_back({b, r});
        return r;
    }
    QRectF bounds() const
    {
        QRectF out;
        for (const auto& [b, r] : placed)
            out = out.united(r);
        return out;
    }
};
} // namespace

const std::vector<Item>& items()
{
    static const std::vector<Item> list = {
        {"polyCount", "Poly Count", "Verts, edges, faces, triangles and uvs of the visible scene, the selected objects and the selected components.",
         {{"verts", "Verts"}, {"edges", "Edges"}, {"faces", "Faces"}, {"tris", "Tris"}, {"uvs", "UVs"}}},
        {"smoothDetails", "Smooth Preview Details", "How many meshes show smooth preview and the faces and triangles they draw.",
         {{"objects", "Smooth Preview"}, {"faces", "Smooth Faces"}, {"tris", "Smooth Tris"}}},
        {"selectionDetails", "Selection Details", "The selection mode and how much is selected.", {{"mode", "Select Mode"}, {"selected", "Selected"}}},
        {"texelDensity", "Texel Density", "Texel density of the selected mesh, or of its selected faces, for the HUD's texture size.",
         {{"density", "Texel Density"}, {"size", "Texture Size"}}},
        {"objectDetails", "Object Details", "Name, type, visibility, smoothness and camera distance of the lead selected object.",
         {{"name", "Object"}, {"type", "Type"}, {"visibility", "Visibility"}, {"distance", "Distance From Camera"}, {"smoothness", "Smoothness"},
          {"count", "Selected Objects"}}},
        {"symmetry", "Symmetry", "The mirror axis of the selected meshes, while symmetry is on."},
        {"softSelect", "Soft Select", "Soft selection with its falloff radius and mode, while it is on.",
         {{"state", "Soft Select"}, {"radius", "Falloff Radius"}, {"mode", "Falloff Mode"}}},
        {"cameraNames", "Camera Names", "The view's camera name along the bottom."},
        {"frameRate", "Frame Rate", "How many frames per second the view draws, with the GPU time of the last frame and its ambient occlusion share.",
         {{"fps", "Frame Rate"}, {"gpu", "GPU Time"}, {"ao", "AO Time"}}},
        {"currentFrame", "Current Frame", "The current frame of the time slider."},
        {"viewAxis", "View Axis", "The axis triad in the bottom-left corner."},
        {"viewCube", "View Cube", "A cube in the top-right corner turning with the view; click a face to look at that side."},
        {"viewSwitcher", "View Switcher", "Persp, Front, Side and Top buttons under the view cube, with the scene's cameras. Click the current view again for its opposite side."},
        {"originAxis", "Origin Axis", "The world axes drawn at the origin."},
    };
    return list;
}

QRectF viewCubeRect(double viewWidth)
{
    constexpr double size = 84.0;
    return {viewWidth - kMargin - size, kTop, size, size};
}

std::vector<SwitcherSegment> viewSwitcherSegments(double viewWidth, double viewHeight, bool belowCube)
{
    constexpr double kHeight = 22.0, kPad = 16.0, kCamera = 26.0, kGap = 2.0;
    // Each segment is as wide as its longer label, so flipping to the opposite side never resizes the strip.
    static const std::pair<const char*, const char*> kPairs[] = {{"persp", "Persp"}, {"front", "Front"}, {"right", "Right"}, {"top", "Bottom"}};
    std::vector<double> widths;
    double total = kCamera;
    for (const auto& [view, longest] : kPairs)
    {
        widths.push_back(std::ceil(OverlayPainter::textWidth(longest)) + kPad);
        total += widths.back() + kGap;
    }
    const double top = belowCube ? viewCubeRect(viewWidth).bottom() + 6.0 : kTop;
    if (viewWidth < total + 2.0 * kMargin + 120.0 || viewHeight < top + kHeight + 140.0)
        return {};
    double x = viewWidth - kMargin - total;
    std::vector<SwitcherSegment> out;
    for (size_t i = 0; i < std::size(kPairs); ++i)
    {
        out.push_back({kPairs[i].first, QRectF(x, top, widths[i], kHeight)});
        x += widths[i] + kGap;
    }
    out.push_back({"camera", QRectF(x, top, kCamera, kHeight)});
    return out;
}

bool itemOn(int panel, std::string_view item)
{
    return PrefStore::get().value<bool>("general", itemKey(panel, item), itemDefault(item));
}

void setItemOn(int panel, std::string_view item, bool on)
{
    PrefStore::get().set("general", itemKey(panel, item), on);
    if (on)
        setAllShown(panel, true);
}

bool allShown(int panel)
{
    return PrefStore::get().value<bool>("general", itemKey(panel, "show"), true);
}

void setAllShown(int panel, bool on)
{
    PrefStore::get().set("general", itemKey(panel, "show"), on);
}

bool shown(int panel, std::string_view item)
{
    return allShown(panel) && itemOn(panel, item);
}

bool partOn(int panel, std::string_view item, std::string_view part)
{
    return PrefStore::get().value<bool>("general", partKey(panel, item, part), true);
}

void setPartOn(int panel, std::string_view item, std::string_view part, bool on)
{
    PrefStore::get().set("general", partKey(panel, item, part), on);
}

std::optional<bool> actionState(const std::string& id, const ViewportWidget* view)
{
    if (!view || !id.starts_with("viewport.hud."))
        return std::nullopt;
    std::string item = id.substr(13);
    if (item == "toggleAll")
        return allShown(view->hudPanel());
    for (const Item& i : items())
    {
        if (item == i.id)
            return itemOn(view->hudPanel(), item);
        for (const Part& part : i.parts)
            if (item == std::string(i.id) + "." + part.id)
                return partOn(view->hudPanel(), i.id, part.id);
    }
    return std::nullopt;
}

void fillMenu(QMenu* menu, ViewportWidget* view)
{
    int panel = view->hudPanel();
    QAction* all = menu->addAction("Show Heads Up Display");
    all->setCheckable(true);
    all->setChecked(allShown(panel));
    QObject::connect(all, &QAction::triggered, menu, [panel](bool on) { setAllShown(panel, on); });
    menu->addSeparator();
    for (const Item& i : items())
    {
        QAction* a = menu->addAction(i.label);
        a->setCheckable(true);
        a->setChecked(itemOn(panel, i.id));
        a->setToolTip(i.doc);
        // The option box finds the item's parts dialog by its action id.
        a->setData(QString("viewport.hud.") + i.id);
        std::string id = i.id;
        QObject::connect(a, &QAction::triggered, menu, [panel, id](bool on) { setItemOn(panel, id, on); });
    }
    menu->addSeparator();
    QMenu* size = menu->addMenu("Texel Density Texture Size");
    auto* group = new QActionGroup(size);
    int current = PrefStore::get().value<int>("general", kTexelResolution, 1024);
    for (int res : {256, 512, 1024, 2048, 4096, 8192})
    {
        QAction* a = size->addAction(QString::number(res));
        a->setCheckable(true);
        a->setChecked(res == current);
        group->addAction(a);
        QObject::connect(a, &QAction::triggered, size, [res] { PrefStore::get().set("general", kTexelResolution, res); });
    }
    menu->setToolTipsVisible(true);
}

namespace
{
/** Maya's HUD options window for one item of a panel: a checkbox per part, applied as it is clicked. */
void openPartsDialog(int panel, const Item& item)
{
    static std::map<std::pair<int, std::string>, QPointer<QDialog>> open;
    QPointer<QDialog>& d = open[{panel, item.id}];
    if (!d)
    {
        QWidget* parent = nullptr;
        for (QWidget* w : QApplication::topLevelWidgets())
            if (w->inherits("QMainWindow") && w->isVisible())
                parent = w;
        d = new QDialog(parent);
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->setWindowTitle(QString("HUD %1 Options").arg(item.label));
        auto* layout = new QVBoxLayout(d);
        std::string id = item.id;
        for (const Part& part : item.parts)
        {
            auto* box = new QCheckBox(part.label, d);
            box->setChecked(partOn(panel, id, part.id));
            std::string pid = part.id;
            QObject::connect(box, &QCheckBox::toggled, d, [panel, id, pid](bool on) { setPartOn(panel, id, pid, on); });
            layout->addWidget(box);
        }
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, d);
        QObject::connect(buttons, &QDialogButtonBox::rejected, d, &QDialog::close);
        layout->addWidget(buttons);
    }
    d->show();
    d->raise();
    d->activateWindow();
    flashLocate(d);
}
} // namespace

void registerActions(std::function<ViewportWidget*()> active)
{
    actions::add({"viewport.hud.toggleAll", "Show Heads Up Display", "View",
                  [active] {
                      if (ViewportWidget* v = active())
                          setAllShown(v->hudPanel(), !allShown(v->hudPanel()));
                  },
                  nullptr, "Shows or hides every heads-up display readout in the active view."});
    for (const Item& i : items())
    {
        std::string id = i.id;
        actions::add({"viewport.hud." + id, std::string("HUD ") + i.label, "View",
                      [active, id] {
                          if (ViewportWidget* v = active())
                              setItemOn(v->hudPanel(), id, !itemOn(v->hudPanel(), id));
                      },
                      nullptr, std::string(i.doc) + " Toggles it in the active view."});
        for (const Part& part : i.parts)
        {
            std::string pid = part.id;
            actions::add({"viewport.hud." + id + "." + pid, std::string("HUD ") + i.label + ": " + part.label, "View",
                          [active, id, pid] {
                              if (ViewportWidget* v = active())
                                  setPartOn(v->hudPanel(), id, pid, !partOn(v->hudPanel(), id, pid));
                          },
                          nullptr, std::string("Shows or hides the ") + part.label + " line of the " + i.label + " readout in the active view."});
        }
        if (!i.parts.empty())
            optionBox::addCustom("viewport.hud." + id, [active, item = &i] {
                if (ViewportWidget* v = active())
                    openPartsDialog(v->hudPanel(), *item);
            });
    }
}

bool draw(OverlayPainter& p, const Context& ctx)
{
    LILA_ZONE();
    if (!allShown(ctx.panel))
        return false;
    scene::Graph& g = scene::Graph::current();
    auto on = [&](const char* item) { return itemOn(ctx.panel, item); };
    auto parts = [&](const char* item) { return PartOn([&ctx, item](const char* part) { return partOn(ctx.panel, item, part); }); };
    bool pending = false;

    Stack topLeft{.edge = kTop, .width = ctx.view.width};
    if (on("polyCount"))
    {
        scene::hud::PolyStats stats = scene::hud::polyStats(g, uv::currentUvSet(), ctx.shownNode, ctx.isolation, ctx.shownKey);
        pending = stats.pending;
        topLeft.place(polyCountBlock(stats, parts("polyCount")));
    }
    if (on("smoothDetails"))
        topLeft.place(smoothBlock(g, ctx, parts("smoothDetails")));
    if (on("selectionDetails"))
        topLeft.place(selectionBlock(g, ctx, parts("selectionDetails")));
    if (on("texelDensity"))
        topLeft.place(texelBlock(g, parts("texelDensity")));

    // The top-right stack drops below the top-left one when the view is too narrow for both side by side.
    Block right;
    if (on("objectDetails"))
        for (Row& r : objectBlock(g, ctx, parts("objectDetails")))
            right.push_back(std::move(r));
    std::vector<Block> rightBlocks;
    if (!right.empty())
        rightBlocks.push_back(std::move(right));
    if (on("symmetry"))
        if (std::optional<ToolSymmetry> sym = selectionSymmetry(g); sym && sym->on)
            drawSelectionSymmetry(*sym);
    if (on("softSelect"))
        rightBlocks.push_back(softSelectBlock(parts("softSelect")));
    // Shown whenever it is set, so nobody reads display-relative numbers as world ones.
    if (scene::relocate::hasDisplayOrigin())
    {
        const glm::dvec3 o = scene::relocate::displayOrigin();
        rightBlocks.push_back({{"Display Origin:", {fixed(o.x, 1) + ", " + fixed(o.y, 1) + ", " + fixed(o.z, 1)}}});
    }
    Stack topRight{.right = true, .edge = kTop, .width = ctx.view.width};
    if (on("viewCube"))
        topRight.edge = viewCubeRect(ctx.view.width).bottom() + kBlockGap;
    std::vector<SwitcherSegment> switcher;
    if (on("viewSwitcher"))
        switcher = viewSwitcherSegments(ctx.view.width, ctx.view.height, on("viewCube"));
    if (!switcher.empty())
        topRight.edge = switcher.back().rect.bottom() + kBlockGap;
    double widest = 0.0;
    for (const Block& b : rightBlocks)
        widest = std::max(widest, sizeOf(b).width());
    QRectF left = topLeft.bounds();
    if (!left.isNull() && ctx.view.width - kMargin - widest < left.right() + kColumnGap)
        topRight.edge = std::max(topRight.edge, left.bottom() + kBlockGap);
    for (const Block& b : rightBlocks)
        topRight.place(b);

    // Bottom right sits on the camera name's line unless the name would run into it.
    std::vector<Block> bottomBlocks;
    if (on("frameRate"))
    {
        static const double wide = OverlayPainter::textWidth("0000.0 fps");
        PartOn part = parts("frameRate");
        Block rate;
        if (part("fps"))
            rate.push_back({"Frame Rate:", {fixed(ctx.framesPerSecond, 1) + " fps"}, wide});
        if (ctx.gpuMs > 0.0 && part("gpu"))
            rate.push_back({"GPU Time:", {fixed(ctx.gpuMs) + " ms"}, wide});
        if (ctx.aoMs > 0.0 && part("ao"))
            rate.push_back({"AO Time:", {fixed(ctx.aoMs) + " ms"}, wide});
        bottomBlocks.push_back(std::move(rate));
    }
    if (on("currentFrame"))
        bottomBlocks.push_back(frameBlock(g));
    Stack bottomRight{.right = true, .upward = true, .edge = ctx.view.height - 8.0, .width = ctx.view.width};
    if (on("cameraNames"))
    {
        double nameRight = ctx.view.width / 2.0 + OverlayPainter::textWidth(QString::fromStdString(ctx.view.camera)) / 2.0;
        double widestBottom = 0.0;
        for (const Block& b : bottomBlocks)
            widestBottom = std::max(widestBottom, sizeOf(b).width());
        if (ctx.view.width - kMargin - widestBottom < nameRight + kColumnGap)
            bottomRight.edge -= kLine + 4.0;
    }
    for (const Block& b : bottomBlocks)
        bottomRight.place(b);

    // A view too small for everything drops the readouts that would land on something already there.
    std::vector<QRectF> taken;
    if (on("viewAxis"))
        taken.emplace_back(0.0, ctx.view.height - 72.0, 72.0, 72.0);
    if (on("viewCube"))
        taken.push_back(viewCubeRect(ctx.view.width));
    for (const SwitcherSegment& seg : switcher)
        taken.push_back(seg.rect);
    if (on("cameraNames"))
    {
        double w = OverlayPainter::textWidth(QString::fromStdString(ctx.view.camera));
        taken.emplace_back((ctx.view.width - w) / 2.0, ctx.view.height - 24.0, w, kLine);
    }
    p.setShadow(true);
    for (const Stack* s : {&topLeft, &topRight, &bottomRight})
        for (const auto& [b, r] : s->placed)
            if (std::none_of(taken.begin(), taken.end(), [&](const QRectF& t) { return t.intersects(r); }))
            {
                drawBlock(p, b, r);
                taken.push_back(r);
                if (ctx.drawn)
                    ctx.drawn->push_back(r);
            }
    p.setShadow(false);
    return pending;
}

void drawOriginAxis(render::FrameState& s, OverlayPainter& p, const ViewInfo& view)
{
    const glm::dvec3 origin = scene::relocate::displayOrigin();
    if (!view.project(origin))
        return;
    double length = view.unitsPerPixel(origin) * 48.0;
    const glm::vec4 line(0.45f, 0.45f, 0.45f, 1.0f); // linear, like the other 3D lines
    p.setShadow(true);
    for (int axis = 0; axis < 3; ++axis)
    {
        glm::dvec3 tip = origin;
        tip[axis] += length;
        s.lines.push_back({glm::vec3(origin), line});
        s.lines.push_back({glm::vec3(tip), line});
        if (auto at = view.project(origin + (tip - origin) * 1.15))
            p.text(*at + QPointF(0, 4), QString(QChar('X' + axis)), p.ink(kText, kAlpha, QRectF(*at - QPointF(6, 8), QSizeF(12, 16))),
                   OverlayPainter::Center);
    }
    p.setShadow(false);
}
} // namespace lila::ui::hud
