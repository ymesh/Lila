#include "curve/Text.h"
#include "ui/TextureSetPanel.h"

// Plan.h has a member named like Qt's slots macro.
#pragma push_macro("slots")
#undef slots
#include "render/PaintGpu.h"
#include "tex/Generators.h"
#include "tex/Layers.h"
#include "tex/NodeDef.h"
#include "tex/PaintStrokes.h"
#include "tex/SmartMaterial.h"
#include "tex/TextureSet.h"
#pragma pop_macro("slots")

#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Attribute.h"
#include "scene/MeshNodes.h"
#include "ui/ViewportHooks.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/Node.h"
#include "ui/Controls.h"
#include "ui/Actions.h"
#include "ui/Deletion.h"
#include "ui/GraphBackend.h"
#include "ui/Icons.h"
#include "ui/MaterialBrowser.h"
#include "ui/MaterialThumbnails.h"
#include "ui/Menus.h"
#include "ui/NodeEditor.h"
#include "ui/PainterImportUi.h"
#include "ui/Panels.h"
#include "ui/ShelfTags.h"
#include "ui/SpinScrub.h"
#include "ui/TexParams.h"
#include "ui/TexProjectionTool.h"
#include "ui/TexPreview.h"
#include "ui/TextureGraphUi.h"
#include "ui/Favourites.h"
#include "ui/TexturePaintTool.h"
#include "ui/FileDialog.h"
#include "ui/Theme.h"
#include "brush/Alphas.h"
#include "brush/Brush.h"
#include "core/Workspace.h"
#include "tex/BrushPresets.h"
#include "ui/UiCommands.h"
#include "ui/UiCommon.h"
#include "ui/ViewportLayout.h"

#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QPainter>
#include <QPolygonF>
#include <QImageReader>
#include <QMimeData>
#include <QStackedWidget>
#include <QUrl>
#include <QDoubleSpinBox>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <QTreeWidgetItemIterator>

#include <algorithm>
#include <map>
#include <optional>

namespace lila::ui
{
namespace
{

constexpr int kLayerThumb = 36;
constexpr int kRoleKind = Qt::UserRole;
constexpr int kRoleId = Qt::UserRole + 1;
constexpr int kRoleLayer = Qt::UserRole + 2;
constexpr int kRoleFolder = Qt::UserRole + 3;
constexpr const char* kAllChannels = "";
constexpr const char* kPrefLinkResolution = "/textureSets/linkResolution";
/** From this width the panel shows two columns: the set's settings and the smart materials beside the layers. */
constexpr int kWideWidth = 600;
constexpr int kSideWidth = 300;

/** A scroll area as tall as its content while there is room for it, scrolling the content when there isn't. */
class FitScrollArea final : public QScrollArea
{
public:
    explicit FitScrollArea(QWidget* parent) : QScrollArea(parent)
    {
        setWidgetResizable(true);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    }

    QSize sizeHint() const override { return widget() ? widget()->sizeHint() : QScrollArea::sizeHint(); }

protected:
    bool event(QEvent* e) override
    {
        if (e->type() == QEvent::LayoutRequest)
            updateGeometry();
        return QScrollArea::event(e);
    }

    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (o == widget() && e->type() == QEvent::LayoutRequest)
            updateGeometry();
        return QScrollArea::eventFilter(o, e);
    }
};

/** The layer stack column. It asks only for its minimum height, so the settings above it get their full height before it
 *  grows; a tall stack hint would otherwise take an equal share of any shortfall from the settings. */
class StackColumn final : public QWidget
{
public:
    using QWidget::QWidget;
    QSize sizeHint() const override { return {QWidget::sizeHint().width(), minimumSizeHint().height()}; }
};

/** A middle click picks item `index` and commits it as a user pick does (these combos act on activated). */
void texComboDefaultIndex(QComboBox* c, int index)
{
    if (index < 0)
        return;
    onMiddleClickReset(
        c,
        [c, index] {
            c->setCurrentIndex(index);
            Q_EMIT c->activated(index);
        },
        c->itemText(index));
}

void texComboDefault(QComboBox* c, const QString& value)
{
    texComboDefaultIndex(c, c->findData(value));
}

// Active paint layers ------------------------------------------------------------------------------------------------

struct ActiveState
{
    std::map<std::string, std::string> layers;
    std::map<int, std::function<void(const std::string&, const std::string&)>> listeners;
    int next = 1;
};

ActiveState& activeState()
{
    static ActiveState s;
    return s;
}

std::optional<tex::StackState> stackOf(const std::string& set)
{
    scene::Node* n = tex::findSet(scene::Graph::current(), set);
    if (!n)
        return std::nullopt;
    try
    {
        return tex::readStack(tex::readGraph(*n), tex::readSettings(*n));
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

std::vector<TextureSetPanel*>& panelList()
{
    static std::vector<TextureSetPanel*> list;
    return list;
}

TextureSetPanel*& lastFocusedPanel()
{
    static TextureSetPanel* p = nullptr;
    return p;
}

// Names and icons ----------------------------------------------------------------------------------------------------

QString kindIcon(const std::string& kind, bool open = false)
{
    if (kind == "fill")
        return "bucket";
    if (kind == "paint")
        return "brush";
    if (kind == "procedural")
        return "topology-star-3";
    if (kind == "folder")
        return open ? "folder-open" : "folder";
    if (kind == "instance")
        return "link";
    if (kind == "filter")
        return "filter";
    if (kind == "decal")
        return "sticker";
    if (kind == "text")
        return "typography";
    if (kind == "scatter")
        return "confetti";
    return "square";
}

QString maskIcon(const std::string& kind)
{
    if (kind == "fill")
        return "square";
    if (kind == "generator")
        return "wand";
    if (kind == "filter")
        return "filter";
    if (kind == "levels")
        return "adjustments";
    if (kind == "paint")
        return "brush";
    if (kind == "anchor")
        return "anchor";
    return "mask";
}

QString capitalised(const std::string& s)
{
    QString q = qs(s);
    if (!q.isEmpty())
        q[0] = q[0].toUpper();
    return q;
}

QString nodeLabel(const std::string& type)
{
    const tex::NodeDef* d = tex::findNode(type);
    return d && !d->label.empty() ? qs(d->label) : qs(type);
}

QString maskName(const json& m)
{
    if (std::string n = m.value("name", ""); !n.empty())
        return qs(n);
    const std::string kind = m.value("kind", "fill");
    if (kind == "generator" || kind == "filter")
        return nodeLabel(m.value("type", ""));
    if (kind == "fill")
        return m.value("value", 1.0) >= 0.5 ? "Fill White" : "Fill Black";
    if (kind == "anchor")
        return "Anchor " + qs(m.value("anchor", ""));
    return capitalised(kind);
}

/** The node types usable as filters: Filter category, with an input and an output. */
std::vector<const tex::NodeDef*> filterNodes()
{
    std::vector<const tex::NodeDef*> out;
    for (const tex::NodeDef* d : tex::allNodes())
        if (d->category == "Filter" && !d->inputs.empty() && !d->outputs.empty())
            out.push_back(d);
    return out;
}

/** A node as texGraphInfo lists it, for texParams::nodePanel: the definition's parameters with values. */
json defNodeJson(const std::string& name, const std::string& type, const json& values)
{
    json j = {{"name", name}, {"type", type}};
    const tex::NodeDef* d = tex::findNode(type);
    if (!d)
    {
        j["error"] = "unknown node type " + type;
        return j;
    }
    j["label"] = d->label;
    j["category"] = d->category;
    json prms = json::array();
    for (const tex::ParamDecl& p : d->params)
    {
        if (p.hidden)
            continue;
        json e = {{"id", p.id}, {"type", tex::paramTypeName(p.type)}, {"default", p.defaultValue}};
        e["value"] = values.is_object() && values.contains(p.id) ? values[p.id] : p.defaultValue;
        if (!p.options.empty())
            e["options"] = p.options;
        for (auto [key, v] : {std::pair{"min", &p.min}, {"max", &p.max}, {"softMin", &p.softMin}, {"softMax", &p.softMax}, {"step", &p.step}})
            if (*v)
                e[key] = **v;
        if (!p.label.empty())
            e["label"] = p.label;
        if (!p.doc.empty())
            e["doc"] = p.doc;
        prms.push_back(e);
    }
    j["params"] = prms;
    j["inputs"] = json::array();
    return j;
}

/** A procedural layer's exposed settings (its graph's param inputs) as a node for texParams::nodePanel. */
json proceduralSettings(const json& layer)
{
    json j = {{"name", ""}, {"type", "procedural"}, {"label", "Settings"}, {"inputs", json::array()}, {"params", json::array()}};
    const json graph = layer.value("graph", json::object());
    for (const json& in : graph.value("inputs", json::array()))
    {
        if (in.value("kind", "") != "param" && !in.contains("targets"))
            continue;
        json e = {{"id", in.value("name", "")}, {"label", in.value("name", "")}, {"type", in.value("type", "float")}, {"default", in.value("default", json())},
                  {"value", in.value("default", json())}};
        for (const char* k : {"min", "max", "softMin", "softMax", "step", "options", "label", "doc"})
            if (in.contains(k))
                e[k] = in[k];
        j["params"].push_back(e);
    }
    return j;
}

QLabel* dim(QWidget* parent, const QString& text)
{
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setStyleSheet("color: #8a8a8a;");
    return l;
}

QLabel* boldHeading(QWidget* parent, const QString& text)
{
    auto* l = new QLabel(text, parent);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

QDoubleSpinBox* percentSpin(QWidget* parent, double opacity)
{
    auto* s = new controls::SliderField(parent);
    s->setRange(0.0, 100.0);
    s->setDecimals(0);
    s->setSuffix("%");
    s->setValue(std::round(opacity * 100.0));
    s->setToolTip("Opacity");
    setSpinDefault(s, 100.0);
    return s;
}

/** Layer and mask thumbnails: a hairline frame on the darkest step, so a black or panel-grey map still reads as a tile. */
constexpr const char* kThumbFrame = "border: 1px solid #6e6e6e; border-radius: 3px; background: #242424;";

/** The shelf's kinds, in the switch's order. */
enum ShelfKind
{
    kShelfMaterials,
    kShelfMasks,
    kShelfBrushes,
    kShelfAlphas,
    kShelfTextures,
};

constexpr const char* kShelfKindNames[] = {"material", "mask", "brush", "alpha", "texture"};

/** Images users bring in to paint through: their own folder and the open workspace's. */
std::vector<std::filesystem::path> textureFolders()
{
    std::vector<std::filesystem::path> out{PrefStore::get().userDir() / "textures"};
    if (Workspace::get().isOpen())
        out.push_back(Workspace::get().stateDir(false) / "textures");
    return out;
}

bool isImageFile(const std::filesystem::path& f)
{
    std::string ext = f.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* e : {".png", ".jpg", ".jpeg", ".tga", ".tif", ".tiff", ".exr", ".bmp", ".psd", ".hdr"})
        if (ext == e)
            return true;
    return false;
}

std::vector<std::filesystem::path> textureFiles()
{
    std::vector<std::filesystem::path> out;
    for (const std::filesystem::path& dir : textureFolders())
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec))
            continue;
        for (const auto& e : std::filesystem::recursive_directory_iterator(dir, ec))
            if (e.is_regular_file(ec) && isImageFile(e.path()))
                out.push_back(e.path());
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.filename() < b.filename(); });
    return out;
}

/** Copies images into the user's textures folder, renaming on a clash; returns the copies. */
std::vector<std::filesystem::path> importTextures(const QStringList& files)
{
    std::vector<std::filesystem::path> out;
    const std::filesystem::path dir = textureFolders().front();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    for (const QString& f : files)
    {
        const std::filesystem::path src = std::filesystem::path(f.toStdWString());
        if (!isImageFile(src))
            continue;
        std::filesystem::path dst = dir / src.filename();
        for (int n = 2; std::filesystem::exists(dst, ec); ++n)
            dst = dir / (src.stem().wstring() + L" " + std::to_wstring(n) + src.extension().wstring());
        if (std::filesystem::copy_file(src, dst, ec))
            out.push_back(dst);
        else
            log::warn("Textures: couldn't copy {}: {}", src.string(), ec.message());
    }
    return out;
}

/** A shelf tile for an image file, read at tile size. */
QIcon imageTile(const std::filesystem::path& file, int size)
{
    QImageReader r(QString::fromStdWString(file.wstring()));
    const QSize full = r.size();
    if (full.isValid())
        r.setScaledSize(full.scaled(size, size, Qt::KeepAspectRatio));
    const QImage img = r.read();
    return img.isNull() ? icon("photo") : QIcon(QPixmap::fromImage(img));
}

/** The tool icon a brush preset's payload kind shows. */
const char* layerPresetIcon(const tex::brushes::Preset& p)
{
    if (p.particles.is_object())
        return "droplet";
    const std::string kind = p.payload.value("kind", std::string("paint"));
    static const std::map<std::string, const char*> kIcons = {
        {"paint", "brush"},       {"direction", "direction-sign"}, {"normal", "ripple"}, {"smudge", "hand-finger"}, {"blur", "blur"},
        {"sharpen", "focus-2"},   {"clone", "copy"},               {"heal", "bandage"},  {"fill", "bucket-droplet"}, {"project", "photo"}};
    auto it = kIcons.find(kind);
    if (p.payload.contains("source"))
        return "texture";
    return it != kIcons.end() ? it->second : "brush";
}

/**
 * A brush preset's tile: an S stroke drawn with its own tip (falloff, roundness, angle, alpha, spacing, flow) in the
 * colour it paints, a fill's patch for fills, relief for normal brushes, with its tool icon in the corner.
 */
QIcon presetPreview(const tex::brushes::Preset& p)
{
    constexpr int W = 96, H = 64;
    brush::BrushSettings b = brush::BrushSettings::fromJson(p.brush);
    b.ensureAlpha();
    const brush::FalloffTable falloff(b.resolveFalloff());
    const std::string kind = p.payload.value("kind", std::string("paint"));
    std::vector<float> cov(size_t(W) * H, 0.0f);
    auto stamp = [&](double cx, double cy, double r, double angle, double flow) {
        brush::Dab d;
        d.radius = r;
        d.normal = {0.0, 0.0, 1.0};
        d.roundness = b.roundness;
        d.alpha = b.alpha;
        d.tangent = (b.roundness < 1.0 || b.alpha) ? glm::dvec3(std::cos(angle), std::sin(angle), 0.0) : glm::dvec3(0.0);
        for (int y = std::max(0, int(cy - r - 1)); y < std::min(H, int(cy + r + 2)); ++y)
            for (int x = std::max(0, int(cx - r - 1)); x < std::min(W, int(cx + r + 2)); ++x)
            {
                const float w = brush::tipWeight(d, falloff, {x + 0.5 - cx, y + 0.5 - cy, 0.0}) * float(flow);
                float& c = cov[size_t(y) * W + x];
                c = 1.0f - (1.0f - c) * (1.0f - std::clamp(w, 0.0f, 1.0f));
            }
    };
    if (kind == "fill")
    {
        const QPolygonF tri({QPointF(14, 54), QPointF(48, 8), QPointF(82, 54)});
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                cov[size_t(y) * W + x] = tri.containsPoint(QPointF(x + 0.5, y + 0.5), Qt::OddEvenFill) ? 1.0f : 0.0f;
    }
    else
    {
        const double r = 11.0;
        const double step = std::max(0.04, b.spacing) * r;
        const double flow = std::clamp(b.flow, 0.05, 1.0);
        double lastX = -1e9, lastY = 0.0;
        for (double t = 0.0; t <= 1.0; t += 0.002)
        {
            const double x = 12.0 + t * 72.0, y = 32.0 - 13.0 * std::sin(t * 6.2831853);
            if (std::hypot(x - lastX, y - lastY) < step)
                continue;
            const double dir = std::atan2(-13.0 * 6.2831853 * std::cos(t * 6.2831853), 72.0);
            const double angle = glm::radians(b.angle) + (b.followStroke ? dir : 0.0);
            stamp(x, y, r * (0.55 + 0.45 * std::sin(t * 3.14159265)), angle, flow);
            lastX = x;
            lastY = y;
        }
        if (p.particles.is_object())
            for (int i = 0; i < 9; ++i)
            {
                const double x = 16.0 + i * 8.0, y0 = 32.0 - 13.0 * std::sin((x - 12.0) / 72.0 * 6.2831853);
                for (double y = y0; y < std::min(double(H - 2), y0 + 10.0 + (i * 7) % 13); y += 1.0)
                    stamp(x, y, 1.6, 0.0, 0.6);
            }
    }
    const float strength = float(std::clamp(b.strength, 0.15, 1.0));
    glm::vec3 col(0.78f);
    if (const json ch = p.payload.value("channels", json::object()); ch.contains("baseColor") && ch["baseColor"].is_array())
        col = {ch["baseColor"][0].get<float>(), ch["baseColor"][1].get<float>(), ch["baseColor"][2].get<float>()};
    const bool relief = kind == "normal" || kind == "direction";
    QImage img(W, H, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const float c = cov[size_t(y) * W + x] * strength;
            if (c <= 0.002f)
                continue;
            glm::vec3 rgb = col;
            if (relief)
            {
                // Lit from the upper left, so a bump reads as one.
                const auto at = [&](int xx, int yy) { return cov[size_t(std::clamp(yy, 0, H - 1)) * W + std::clamp(xx, 0, W - 1)]; };
                const float shade = 0.55f + 1.6f * ((at(x - 1, y) - at(x + 1, y)) + (at(x, y - 1) - at(x, y + 1)));
                rgb = glm::vec3(std::clamp(shade, 0.15f, 1.0f));
            }
            const auto enc = [](float v) { return int(std::lround(std::pow(std::clamp(v, 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f)); };
            const int a = int(std::lround(c * 255.0f));
            img.setPixel(x, y, qRgba(enc(rgb.r) * a / 255, enc(rgb.g) * a / 255, enc(rgb.b) * a / 255, a));
        }
    QPainter painter(&img);
    icon(layerPresetIcon(p)).paint(&painter, QRect(W - 17, H - 17, 16, 16));
    painter.end();
    return QIcon(QPixmap::fromImage(img));
}

/** A label that reports double-clicks, for inline rename. */
class LayerNameLabel final : public QLabel
{
public:
    using QLabel::QLabel;
    std::function<void()> onDoubleClick;

protected:
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (onDoubleClick)
        {
            onDoubleClick();
            e->accept();
            return;
        }
        QLabel::mouseDoubleClickEvent(e);
    }
};

/** A smart material or mask dragged from the shelf: its name and kind on two lines. */
constexpr const char* kSmartMime = "application/x-lila-smart-material";

std::pair<std::string, std::string> smartFromMime(const QMimeData& mime)
{
    const QStringList parts = QString::fromUtf8(mime.data(kSmartMime)).split(QChar('\n'));
    return {parts.value(0).toStdString(), parts.value(1, "material").toStdString()};
}

/** The stack's rows: drags reorder and drop into folders through the panel, library materials drop in as layers. */
class LayerTree final : public QTreeWidget
{
public:
    explicit LayerTree(QWidget* parent) : QTreeWidget(parent)
    {
        setHeaderHidden(true);
        setColumnCount(1);
        setRootIsDecorated(true);
        setIndentation(14);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setDragEnabled(true);
        setAcceptDrops(true);
        setDropIndicatorShown(true);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::MoveAction);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setContextMenuPolicy(Qt::CustomContextMenu);
        setProperty("hotkeyContext", "textureSets");
    }

    std::function<void(QTreeWidgetItem* dragged, QTreeWidgetItem* target, TextureSetPanel::Drop where)> onMove;
    std::function<void(const std::string& material, QTreeWidgetItem* target, TextureSetPanel::Drop where)> onMaterial;
    std::function<void(const std::string& name, const std::string& kind, QTreeWidgetItem* target, TextureSetPanel::Drop where)> onSmart;

    /** Where a drop at pos lands: above or below a row, or into a folder's middle. */
    TextureSetPanel::Drop dropAt(QTreeWidgetItem* target, QPoint pos) const
    {
        if (!target)
            return TextureSetPanel::Drop::Below;
        const QRect r = visualItemRect(target);
        const int y = pos.y() - r.top();
        if (target->data(0, kRoleFolder).toBool() && y > r.height() / 4 && y < r.height() * 3 / 4)
            return TextureSetPanel::Drop::Into;
        return y < r.height() / 2 ? TextureSetPanel::Drop::Above : TextureSetPanel::Drop::Below;
    }

protected:
    QStringList mimeTypes() const override
    {
        QStringList t = QTreeWidget::mimeTypes();
        t << kMaterialMime << kSmartMime;
        return t;
    }

    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (e->mimeData()->hasFormat(kMaterialMime) || e->mimeData()->hasFormat(kSmartMime))
        {
            e->acceptProposedAction();
            return;
        }
        QTreeWidget::dragEnterEvent(e);
    }

    void dragMoveEvent(QDragMoveEvent* e) override
    {
        QTreeWidget::dragMoveEvent(e);
        if (e->mimeData()->hasFormat(kMaterialMime) || e->mimeData()->hasFormat(kSmartMime))
            e->acceptProposedAction();
    }

    void dropEvent(QDropEvent* e) override
    {
        const QPoint pos = e->position().toPoint();
        QTreeWidgetItem* target = itemAt(pos);
        const TextureSetPanel::Drop where = dropAt(target, pos);
        if (e->mimeData()->hasFormat(kMaterialMime))
        {
            if (onMaterial)
                onMaterial(e->mimeData()->data(kMaterialMime).toStdString(), target, where);
        }
        else if (e->mimeData()->hasFormat(kSmartMime))
        {
            if (onSmart)
            {
                auto [name, kind] = smartFromMime(*e->mimeData());
                onSmart(name, kind, target, where);
            }
        }
        else if (e->source() == this && currentItem() && onMove)
            onMove(currentItem(), target, where);
        // The panel rebuilds the rows from the stack; Qt must not move them itself.
        e->setDropAction(Qt::IgnoreAction);
        e->accept();
    }
};

/** The library shelf: smart materials and masks drag onto meshes in a viewport and onto the layer stack; image files dropped on it become textures. */
class SmartShelf final : public QListWidget
{
public:
    using QListWidget::QListWidget;
    std::function<void(const QStringList& files)> onFiles;

protected:
    QStringList mimeTypes() const override { return {kSmartMime}; }

    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        if (items.isEmpty())
            return nullptr;
        const QString kind = items.front()->data(Qt::UserRole + 1).toString();
        if (kind != "material" && kind != "mask")
            return nullptr;
        auto* m = new QMimeData();
        m->setData(kSmartMime, (items.front()->data(Qt::UserRole).toString() + QChar('\n') + items.front()->data(Qt::UserRole + 1).toString()).toUtf8());
        return m;
    }

    static QStringList localFiles(const QMimeData* m)
    {
        QStringList out;
        if (m && m->hasUrls())
            for (const QUrl& u : m->urls())
                if (u.isLocalFile())
                    out << u.toLocalFile();
        return out;
    }
    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (onFiles && !localFiles(e->mimeData()).isEmpty())
            e->acceptProposedAction();
        else
            QListWidget::dragEnterEvent(e);
    }
    void dragMoveEvent(QDragMoveEvent* e) override
    {
        if (onFiles && !localFiles(e->mimeData()).isEmpty())
            e->acceptProposedAction();
        else
            QListWidget::dragMoveEvent(e);
    }
    void dropEvent(QDropEvent* e) override
    {
        if (const QStringList files = localFiles(e->mimeData()); onFiles && !files.isEmpty())
        {
            e->acceptProposedAction();
            onFiles(files);
            return;
        }
        QListWidget::dropEvent(e);
    }
};

std::vector<QTreeWidgetItem*> allItems(QTreeWidget* tree)
{
    std::vector<QTreeWidgetItem*> out;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        out.push_back(*it);
    return out;
}

struct Thumb
{
    QPointer<QLabel> label;
    QPointer<QToolButton> button;
    texPreview::Request request;
    QString fallback;
};
/** Texture Graph editors on another set switch to set, so the graph, the panel and the viewport show one set. */
void showInTextureGraphs(const std::string& set)
{
    if (set.empty())
        return;
    for (NodeEditor* e : NodeEditor::all())
        if (e->backend().id() == kTextureGraphBackend && e->group().toStdString() != set)
            e->setGroup(QString::fromStdString(set));
}

/** The set the first Texture Graph editor shows, or empty. */
std::string textureGraphSet()
{
    for (NodeEditor* e : NodeEditor::all())
        if (e->backend().id() == kTextureGraphBackend && !e->group().isEmpty())
            return e->group().toStdString();
    return {};
}

/** Whether the selection holds a mesh, or a group with one under it. */
bool selectsMeshes(scene::Graph& g)
{
    for (scene::NodeId id : g.selection())
    {
        if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
            return true;
        for (scene::NodeId d : g.descendants(id))
            if (scene::Node* n = g.node(d); n && n->type().isA("mesh"))
                return true;
    }
    return false;
}

} // namespace

// Panel ----------------------------------------------------------------------------------------------------------------

struct TextureSetPanel::Impl
{
    TextureSetPanel* self = nullptr;
    std::string set;
    json view;
    tex::SetSettings settings;
    std::string viewKey, settingsKey, shelfKey;
    bool shelfStale = false;
    std::string selLayer, selMask;
    std::map<std::string, bool> expanded;
    bool building = false;
    bool keepProps = false;
    bool thumbsQueued = false;
    std::string propsKey;

    QComboBox* sets = nullptr;
    QComboBox* variants = nullptr;
    QToolButton* follow = nullptr;
    QToolButton* settingsToggle = nullptr;
    QWidget* settingsBox = nullptr;
    QComboBox* channel = nullptr;
    QWidget* bannerBox = nullptr;
    QLabel* bannerText = nullptr;
    QPushButton* rebuild = nullptr;
    LayerTree* tree = nullptr;
    QScrollArea* propsScroll = nullptr;
    QWidget* props = nullptr;
    QListWidget* shelf = nullptr;
    QWidget* shelfBox = nullptr;
    controls::Segmented* shelfKind = nullptr;
    QPushButton* shelfApply = nullptr;
    QLabel* shelfHint = nullptr;
    QLineEdit* shelfSearch = nullptr;
    ShelfTagFilter* shelfTags = nullptr;
    QToolButton* shelfAdd = nullptr;
    controls::Segmented* lowerKind = nullptr;
    QStackedWidget* lower = nullptr;
    QBoxLayout* columns = nullptr;
    QWidget* side = nullptr;
    QVBoxLayout* sideLayout = nullptr;
    QSplitter* split = nullptr;
    bool wide = false;
    QWidget* empty = nullptr;
    QLabel* emptyText = nullptr;
    /** Follow found meshes selected and no set on their materials: the panel shows none rather than the last one. */
    bool noSetForSelection = false;
    QTimer* refreshTimer = nullptr;
    int sceneListener = 0;
    int activeListener = 0;
    int graphListener = 0;
    std::vector<Thumb> thumbs;

    // Reading ------------------------------------------------------------------------------------------------------------

    scene::Node* setNode() const { return set.empty() ? nullptr : tex::findSet(scene::Graph::current(), set); }

    const json* findLayer(const json& layers, const std::string& id) const
    {
        for (const json& l : layers)
        {
            if (l.value("id", "") == id)
                return &l;
            if (auto kids = l.find("children"); kids != l.end())
                if (const json* c = findLayer(*kids, id))
                    return c;
        }
        return nullptr;
    }

    const json* layer(const std::string& id) const { return view.is_object() ? findLayer(view["layers"], id) : nullptr; }

    const json* mask(const std::string& layerId, const std::string& id) const
    {
        const json* l = layer(layerId);
        if (!l)
            return nullptr;
        auto masks = l->find("masks");
        if (masks == l->end())
            return nullptr;
        for (const json& m : *masks)
            if (m.value("id", "") == id)
                return &m;
        return nullptr;
    }

    std::vector<std::string> channelNames() const
    {
        std::vector<std::string> out;
        for (const tex::Channel& c : settings.channels)
            out.push_back(c.name);
        return out;
    }

    /** The channel the rows' blend and opacity edit; empty for all. */
    std::string shownChannel() const { return channel ? channel->currentData().toString().toStdString() : std::string(); }

    bool graphMode() const { return view.is_object() && view.value("mode", "") == "graph"; }

    // Commands ----------------------------------------------------------------------------------------------------------

    json args(std::initializer_list<json> list) const
    {
        json a = json::array({set});
        for (const json& x : list)
            a.push_back(x);
        return {{"args", a}};
    }

    json setLayer(const std::string& id, const json& patch) { return runUi("texSetLayer", args({id, patch})); }

    json setMask(const std::string& layerId, const std::string& maskId, const json& patch)
    {
        return runUi("texSetMask", args({layerId, maskId, patch}));
    }

    /** A layer channel's settings with the given keys changed, keeping the rest as resolved (so an explicit entry never flips it). */
    json channelEntry(const json& l, const std::string& c, const json& changes) const
    {
        json r = l.value("resolved", json::object()).value(c, json::object());
        json e = {{"enabled", r.value("enabled", true)}, {"opacity", r.value("opacity", 1.0)}};
        const json explicitMode = l.value("channels", json::object()).value(c, json::object()).value("mode", json());
        if (explicitMode.is_string())
            e["mode"] = explicitMode;
        e.update(changes);
        return e;
    }

    /** The channels a row edit applies to: the shown one, or every channel the layer writes (blend modes skip normal and height). */
    std::vector<std::string> targetChannels(const json& l, bool forMode) const
    {
        const std::string shown = shownChannel();
        if (!shown.empty())
            return {shown};
        std::vector<std::string> out;
        const json resolved = l.value("resolved", json::object());
        for (const tex::Channel& c : settings.channels)
            if (resolved.value(c.name, json::object()).value("enabled", false) && (!forMode || (c.name != "normal" && c.name != "height")))
                out.push_back(c.name);
        return out;
    }

    void setRowOpacity(const std::string& id, double opacity)
    {
        const json* l = layer(id);
        if (!l)
            return;
        json ch = json::object();
        for (const std::string& c : targetChannels(*l, false))
            ch[c] = channelEntry(*l, c, {{"opacity", opacity}});
        if (ch.empty())
            return;
        setLayer(id, {{"channels", ch}});
    }

    void setRowMode(const std::string& id, const std::string& mode)
    {
        const json* l = layer(id);
        if (!l)
            return;
        json ch = json::object();
        for (const std::string& c : targetChannels(*l, true))
            ch[c] = channelEntry(*l, c, {{"mode", mode}});
        if (ch.empty())
            return;
        setLayer(id, {{"channels", ch}});
    }

    void toggleChannel(const std::string& id, const std::string& c, bool on)
    {
        const json* l = layer(id);
        if (!l)
            return;
        json patch = {{"channels", {{c, channelEntry(*l, c, {{"enabled", on}})}}}};
        // A fill layer turned on for a channel it has no value for starts from the channel's default.
        if (on && l->value("kind", "") == "fill" && !l->value("material", json::object()).contains(c))
            if (const tex::Channel* ch = settings.channel(c))
                patch["material"] = {{c, tex::channelDefault(c, ch->type)}};
        setLayer(id, patch);
    }

    /** Placement flags for a new layer: above the selected layer (in its folder), else on top. */
    json placement() const
    {
        if (!selLayer.empty() && layer(selLayer))
            return {{"above", selLayer}};
        return json::object();
    }

    std::string addLayer(const std::string& kind, const std::string& filter)
    {
        if (!setNode())
            return {};
        json call = args({kind});
        call.update(placement());
        if (kind == "filter")
        {
            if (filter.empty())
                return {};
            call["filter"] = filter;
            call["name"] = nodeLabel(filter).toStdString();
        }
        if (kind == "instance")
        {
            const json* src = layer(selLayer);
            if (!src)
            {
                log::warn("Select the layer to instance first");
                return {};
            }
            call["source"] = selLayer;
            call["name"] = src->value("name", selLayer) + " instance";
        }
        if (kind == "decal" || kind == "scatter")
        {
            const QString f = ui::fileDialog::getOpenFileName(self, kind == "decal" ? "Decal Picture" : "Scatter Pattern", {},
                                                              "Images (*.png *.jpg *.jpeg *.tga *.tif *.tiff *.exr *.psd)");
            if (f.isEmpty())
                return {};
            if (kind == "decal")
                call["decal"] = {{"maps", {{"baseColor", f.toStdString()}}}};
            else
                call["scatter"] = {{"pattern", f.toStdString()}};
        }
        std::string id;
        {
            UndoChunk chunk("Add " + capitalised(kind).toStdString() + " Layer");
            json r = runUi("texAddLayer", call);
            id = r.is_object() ? r.value("id", std::string()) : std::string();
            if (!id.empty() && kind == "paint")
                wirePaint(id);
        }
        if (!id.empty())
        {
            selLayer = id;
            selMask.clear();
            if (kind == "paint")
                textureSets::setActiveLayer(set, id);
        }
        self->refresh();
        if (!id.empty() && kind == "paint")
            actions::run(json("tex.paint"));
        return id;
    }

    /** Points a new paint layer's channels and mask at its paint buffer, as the paint tool does. */
    void wirePaint(const std::string& id)
    {
        scene::Node* n = setNode();
        if (!n)
            return;
        std::vector<std::string> channels = channelNames();
        channels.push_back(tex::paint::kMaskChannel);
        render::paint::wireLayer(*n, id, channels);
    }

    void addIdLayers()
    {
        runUi("texAddIdLayers", args({}));
        self->refresh();
    }

    std::string addMask(const std::string& kind, const std::string& type)
    {
        if (selLayer.empty() || !layer(selLayer))
        {
            log::warn("Select a layer to add a mask to");
            return {};
        }
        json r;
        if (kind == "generator")
            r = runUi("texAddGenerator", args({selLayer, type}));
        else
        {
            json call = args({selLayer, kind == "black" || kind == "white" ? std::string("fill") : kind});
            if (kind == "black")
                call["value"] = 0.0;
            if (kind == "filter")
                call["type"] = type;
            if (kind == "anchor")
                call["anchor"] = type;
            r = runUi("texAddMask", call);
        }
        std::string id = r.is_object() ? r.value("id", std::string()) : std::string();
        if (!id.empty())
        {
            selMask = id;
            expanded[selLayer] = true;
        }
        self->refresh();
        return id;
    }

    // Building -----------------------------------------------------------------------------------------------------------

    void requestThumb(QLabel* label, QToolButton* button, const texPreview::Request& r, const QString& fallback)
    {
        thumbs.push_back({label, button, r, fallback});
    }

    void updateThumbs()
    {
        LILA_ZONE();
        thumbsQueued = false;
        QPointer<TextureSetPanel> guard(self);
        for (Thumb& t : thumbs)
        {
            if (!t.label && !t.button)
                continue;
            texPreview::Result r = texPreview::get(t.request, [guard] {
                if (guard && !guard->d->thumbsQueued)
                {
                    guard->d->thumbsQueued = true;
                    QTimer::singleShot(0, guard, [guard] {
                        if (guard)
                            guard->d->updateThumbs();
                    });
                }
            });
            QPixmap pm;
            if (r.image)
            {
                const std::string name = t.request.socket.empty() ? t.request.channel : t.request.socket;
                QImage img = texPreview::toQImage(*r.image, r.type, texPreview::isColor(r.type, name));
                pm = QPixmap::fromImage(img.scaled(kLayerThumb - 2, kLayerThumb - 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            }
            else if (!r.error.empty())
                pm = icon(t.fallback).pixmap(20, 20);
            if (pm.isNull())
                continue;
            if (t.label)
                t.label->setPixmap(pm);
            if (t.button)
                t.button->setIcon(QIcon(pm));
        }
    }

    /** The Outliner's visibility mark: a small eye at the row's trailing edge, eye-off and dimmed while hidden. */
    QToolButton* visibilityButton(QWidget* parent, bool shown, const QString& tip)
    {
        auto* b = new QToolButton(parent);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setIconSize(QSize(16, 16));
        b->setFixedSize(20, 20);
        const QIcon ic = icon(shown ? "eye" : "eye-off");
        b->setIcon(shown ? ic : QIcon(ic.pixmap(16, 16, QIcon::Disabled)));
        b->setToolTip(tip);
        return b;
    }

    QLabel* thumbLabel(QWidget* parent, const QString& name)
    {
        auto* l = new QLabel(parent);
        l->setObjectName(name);
        l->setFixedSize(kLayerThumb, kLayerThumb);
        l->setAlignment(Qt::AlignCenter);
        l->setStyleSheet(themedStyleSheet(kThumbFrame));
        return l;
    }

    void beginRename(QWidget* row, LayerNameLabel* label, const std::string& id, const std::string& current, const std::string& maskOf)
    {
        auto* edit = new QLineEdit(qs(current), row);
        edit->setObjectName("rename:" + qs(id));
        auto* box = qobject_cast<QBoxLayout*>(label->parentWidget()->layout());
        if (box)
        {
            int at = box->indexOf(label);
            box->insertWidget(at, edit, 1);
        }
        label->hide();
        edit->selectAll();
        edit->setFocus();
        QPointer<QLineEdit> guard(edit);
        QObject::connect(edit, &QLineEdit::editingFinished, edit, [this, guard, id, current, maskOf] {
            if (!guard || guard->property("done").toBool())
                return;
            guard->setProperty("done", true);
            const std::string next = guard->text().trimmed().toStdString();
            if (!next.empty() && next != current)
            {
                if (maskOf.empty())
                    setLayer(id, {{"name", next}});
                else
                    setMask(maskOf, id, {{"name", next}});
            }
            QTimer::singleShot(0, self, [this] { self->refresh(); });
        });
    }

    QWidget* layerRow(const json& l)
    {
        const std::string id = l.value("id", "");
        const std::string kind = l.value("kind", "fill");
        const bool enabled = l.value("enabled", true);
        const json resolved = l.value("resolved", json::object());
        auto* row = new QWidget();
        row->setObjectName("layer:" + qs(id));
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(2, 2, 2, 2);
        h->setSpacing(6);

        auto* eye = visibilityButton(row, enabled, enabled ? "Shown: click to hide the layer" : "Hidden: click to show the layer");
        eye->setObjectName("eye:" + qs(id));
        QObject::connect(eye, &QToolButton::clicked, row, [this, id, enabled] { setLayer(id, {{"enabled", !enabled}}); });

        QLabel* thumb = thumbLabel(row, "thumb:" + qs(id));
        thumb->setToolTip(capitalised(kind) + " layer");
        const std::string shown = shownChannel();
        if (kind == "folder")
            thumb->setPixmap(icon(kindIcon(kind)).pixmap(22, 22));
        else
        {
            std::string socket = shown.empty() ? std::string("baseColor") : shown;
            if (!settings.channel(socket) && !settings.channels.empty())
                socket = settings.channels.front().name;
            requestThumb(thumb, nullptr, {set, id, socket, {}, 64}, kindIcon(kind));
        }
        h->addWidget(thumb);

        const json masks = l.value("masks", json::array());
        if (!masks.empty())
        {
            auto* mt = new QToolButton(row);
            mt->setObjectName("maskThumb:" + qs(id));
            mt->setAutoRaise(true);
            mt->setIconSize(QSize(kLayerThumb - 4, kLayerThumb - 4));
            mt->setFixedSize(kLayerThumb, kLayerThumb);
            mt->setIcon(icon("mask"));
            mt->setStyleSheet(themedStyleSheet(QString("QToolButton { %1 padding: 0; }").arg(kThumbFrame)));
            mt->setToolTip("Mask stack: click to show or hide it");
            QObject::connect(mt, &QToolButton::clicked, row, [this, id] {
                expanded[id] = !expanded[id];
                for (QTreeWidgetItem* it : allItems(tree))
                    if (it->data(0, kRoleKind).toInt() == 0 && it->data(0, kRoleId).toString().toStdString() == id)
                        it->setExpanded(expanded[id]);
            });
            if (kind != "folder")
                requestThumb(nullptr, mt, {set, id, "mask", {}, 64}, "mask");
            h->addWidget(mt);
        }

        auto* name = new LayerNameLabel(qs(l.value("name", id)), row);
        name->setObjectName("name:" + qs(id));
        QStringList written;
        for (const tex::Channel& c : settings.channels)
            if (resolved.value(c.name, json::object()).value("enabled", false))
                written << qs(c.name);
        const QString writes = written.isEmpty() ? QString("Writes no channels") : "Writes " + written.join(", ");
        name->setToolTip(writes + "\nDouble-click to rename");
        if (!enabled)
            controls::setRole(name, "dim");
        if (kind == "paint" && textureSets::activeLayer(set) == id)
        {
            QFont f = name->font();
            f.setBold(true);
            name->setFont(f);
            name->setToolTip("Active paint layer\n" + writes + "\nDouble-click to rename");
        }
        const std::string current = l.value("name", id);
        name->onDoubleClick = [this, row, name, id, current] { beginRename(row, name, id, current, {}); };
        h->addWidget(name, 1);
        if (!l.value("anchor", "").empty())
        {
            auto* a = new QLabel(row);
            a->setPixmap(icon("anchor").pixmap(14, 14));
            a->setToolTip("Anchor " + qs(l.value("anchor", "")));
            h->addWidget(a);
        }

        auto* right = new QWidget(row);
        auto* rv = new QVBoxLayout(right);
        rv->setContentsMargins(0, 0, 0, 0);
        rv->setSpacing(0);
        right->setFixedWidth(76);
        std::string modeChannel = shown;
        if (modeChannel.empty())
            for (const tex::Channel& c : settings.channels)
                if (c.name != "normal" && c.name != "height" && resolved.value(c.name, json::object()).value("enabled", false))
                {
                    modeChannel = c.name;
                    break;
                }
        if (modeChannel.empty() && !settings.channels.empty())
            modeChannel = settings.channels.front().name;
        const json mr = resolved.value(modeChannel, json::object());
        auto* blend = new QToolButton(right);
        blend->setObjectName("blend:" + qs(id));
        controls::setRole(blend, "inline");
        blend->setPopupMode(QToolButton::InstantPopup);
        blend->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        blend->setToolTip(shown.empty() ? "Blend mode of every channel (normal and height keep theirs)" : "Blend mode of " + qs(shown));
        const std::vector<std::string> normalModes = {"rnm", "normal"};
        const auto& modes = modeChannel == "normal" && !shown.empty() ? normalModes : tex::layerBlendModes();
        const std::string currentMode = mr.value("mode", "normal");
        blend->setText(capitalised(currentMode));
        auto* blendMenu = new QMenu(blend);
        for (const std::string& m : modes)
        {
            QAction* a = blendMenu->addAction(capitalised(m), row, [this, id, m] { setRowMode(id, m); });
            a->setCheckable(true);
            a->setChecked(m == currentMode);
        }
        blend->setMenu(blendMenu);
        onMiddleClickReset(blend, [this, id] { setRowMode(id, "normal"); }, "Normal");
        rv->addWidget(blend);
        auto* opacity = controls::valueField(0.0, 100.0, 0, {}, right);
        opacity->setSuffix("%");
        opacity->setValue(std::round(mr.value("opacity", 1.0) * 100.0));
        opacity->setToolTip("Opacity");
        setSpinDefault(opacity, 100.0);
        opacity->setKeyboardTracking(false);
        controls::setRole(opacity, "inline");
        opacity->setObjectName("opacity:" + qs(id));
        QObject::connect(opacity, &QDoubleSpinBox::valueChanged, row, [this, id](double v) { setRowOpacity(id, v / 100.0); });
        rv->addWidget(opacity);
        h->addWidget(right);
        h->addWidget(eye);
        return row;
    }

    QWidget* maskRow(const std::string& layerId, const json& m)
    {
        const std::string id = m.value("id", "");
        const bool enabled = m.value("enabled", true);
        auto* row = new QWidget();
        row->setObjectName("mask:" + qs(layerId) + ":" + qs(id));
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(2, 1, 4, 1);
        h->setSpacing(4);
        auto* eye = visibilityButton(row, enabled, enabled ? "On: click to turn the mask off" : "Off: click to turn the mask on");
        eye->setObjectName("maskEye:" + qs(id));
        QObject::connect(eye, &QToolButton::clicked, row, [this, layerId, id, enabled] { setMask(layerId, id, {{"enabled", !enabled}}); });
        auto* k = new QLabel(row);
        k->setPixmap(icon(maskIcon(m.value("kind", ""))).pixmap(16, 16));
        k->setToolTip(capitalised(m.value("kind", "")) + " mask");
        h->addWidget(k);
        auto* name = new LayerNameLabel(maskName(m), row);
        name->setObjectName("maskName:" + qs(id));
        if (!enabled)
            name->setStyleSheet("color: #7a7a7a;");
        const std::string current = maskName(m).toStdString();
        name->onDoubleClick = [this, row, name, id, current, layerId] { beginRename(row, name, id, current, layerId); };
        h->addWidget(name, 1);
        auto* inv = toolButton(row, "circle-half-2", "Invert");
        inv->setObjectName("maskInvert:" + qs(id));
        inv->setCheckable(true);
        inv->setChecked(m.value("invert", false));
        QObject::connect(inv, &QToolButton::toggled, row, [this, layerId, id](bool on) { setMask(layerId, id, {{"invert", on}}); });
        h->addWidget(inv);
        auto* mode = new QToolButton(row);
        mode->setObjectName("maskMode:" + qs(id));
        controls::setRole(mode, "inline");
        mode->setPopupMode(QToolButton::InstantPopup);
        mode->setToolTip("How it combines with the masks below");
        const std::string currentMode = m.value("mode", "normal");
        mode->setText(capitalised(currentMode));
        auto* modeMenu = new QMenu(mode);
        for (const std::string& md : tex::maskBlendModes())
        {
            QAction* a = modeMenu->addAction(capitalised(md), row, [this, layerId, id, md] { setMask(layerId, id, {{"mode", md}}); });
            a->setCheckable(true);
            a->setChecked(md == currentMode);
        }
        mode->setMenu(modeMenu);
        onMiddleClickReset(mode, [this, layerId, id] { setMask(layerId, id, {{"mode", "normal"}}); }, "Normal");
        h->addWidget(mode);
        auto* op = controls::valueField(0.0, 100.0, 0, {}, row);
        op->setSuffix("%");
        op->setValue(std::round(m.value("opacity", 1.0) * 100.0));
        op->setToolTip("Opacity");
        setSpinDefault(op, 100.0);
        op->setKeyboardTracking(false);
        controls::setRole(op, "inline");
        op->setFixedWidth(52);
        op->setObjectName("maskOpacity:" + qs(id));
        QObject::connect(op, &QDoubleSpinBox::valueChanged, row, [this, layerId, id](double v) { setMask(layerId, id, {{"opacity", v / 100.0}}); });
        h->addWidget(op);
        h->addWidget(eye);
        return row;
    }

    QTreeWidgetItem* addRow(QTreeWidgetItem* parent, QWidget* row)
    {
        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
        item->setSizeHint(0, row->sizeHint());
        tree->setItemWidget(item, 0, row);
        return item;
    }

    void addLayers(QTreeWidgetItem* parent, const json& layers)
    {
        for (auto it = layers.rbegin(); it != layers.rend(); ++it)
        {
            const json& l = *it;
            const std::string id = l.value("id", "");
            const bool folder = l.value("kind", "") == "folder";
            QTreeWidgetItem* item = addRow(parent, layerRow(l));
            item->setData(0, kRoleKind, 0);
            item->setData(0, kRoleId, qs(id));
            item->setData(0, kRoleLayer, qs(id));
            item->setData(0, kRoleFolder, folder);
            const json masks = l.value("masks", json::array());
            for (auto m = masks.rbegin(); m != masks.rend(); ++m)
            {
                QTreeWidgetItem* mi = addRow(item, maskRow(id, *m));
                mi->setData(0, kRoleKind, 1);
                mi->setData(0, kRoleId, qs(m->value("id", "")));
                mi->setData(0, kRoleLayer, qs(id));
            }
            if (folder)
                addLayers(item, l.value("children", json::array()));
            auto e = expanded.find(id);
            item->setExpanded(e != expanded.end() ? e->second : folder);
        }
    }

    void rebuildTree()
    {
        LILA_ZONE();
        building = true;
        const int scroll = tree->verticalScrollBar()->value();
        thumbs.clear();
        tree->clear();
        if (view.is_object())
            addLayers(nullptr, view["layers"]);
        QTreeWidgetItem* sel = nullptr;
        for (QTreeWidgetItem* it : allItems(tree))
        {
            const int kind = it->data(0, kRoleKind).toInt();
            const std::string id = it->data(0, kRoleId).toString().toStdString();
            const std::string owner = it->data(0, kRoleLayer).toString().toStdString();
            if ((selMask.empty() && kind == 0 && id == selLayer) || (!selMask.empty() && kind == 1 && id == selMask && owner == selLayer))
                sel = it;
        }
        if (sel)
            tree->setCurrentItem(sel);
        else
        {
            selLayer.clear();
            selMask.clear();
        }
        tree->verticalScrollBar()->setValue(scroll);
        building = false;
        updateThumbs();
    }

    void rebuildSettings()
    {
        LILA_ZONE();
        delete settingsBox->layout();
        // Children of nested layouts too, which taking the top layout's items would miss.
        for (QWidget* w : settingsBox->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))
        {
            w->hide();
            w->deleteLater();
        }
        auto* v = new QVBoxLayout(settingsBox);
        v->setContentsMargins(4, 2, 4, 4);
        v->setSpacing(3);
        if (!setNode())
            return;

        v->addWidget(boldHeading(settingsBox, "Channels"));
        auto* chGrid = new QWidget(settingsBox);
        auto* g = new QGridLayout(chGrid);
        g->setContentsMargins(0, 0, 0, 0);
        g->setHorizontalSpacing(6);
        g->setVerticalSpacing(2);
        const tex::ChannelList extra = tex::extraChannels(settings);
        for (size_t i = 0; i < settings.channels.size(); ++i)
        {
            const tex::Channel& c = settings.channels[i];
            g->addWidget(new QLabel(qs(c.name), chGrid), int(i), 0);
            g->addWidget(dim(chGrid, qs(tex::portTypeName(c.type))), int(i), 1);
            auto* bits = new QComboBox(chGrid);
            bits->setObjectName("bits:" + qs(c.name));
            bits->addItem("8-bit", 8);
            bits->addItem("16-bit", 16);
            bits->addItem("32-bit Float", 32);
            bits->setCurrentIndex(std::max(0, bits->findData(c.bits)));
            int defBits = tex::Channel{}.bits;
            for (const tex::Channel& shipped : tex::SetSettings::makeDefault().channels)
                if (shipped.name == c.name)
                    defBits = shipped.bits;
            texComboDefaultIndex(bits, bits->findData(defBits));
            QObject::connect(bits, &QComboBox::activated, chGrid, [this, i, bits](int) {
                json s = settings.toJson();
                s["channels"][i]["bits"] = bits->currentData().toInt();
                runUi("texSetSettings", args({json{{"channels", s["channels"]}}}));
            });
            g->addWidget(bits, int(i), 2);
            if (std::any_of(extra.begin(), extra.end(), [&](const auto& e) { return e.first == c.name; }))
            {
                auto* rm = toolButton(chGrid, "trash", "Remove the channel");
                QObject::connect(rm, &QToolButton::clicked, chGrid, [this, i] {
                    json s = settings.toJson();
                    s["channels"].erase(s["channels"].begin() + std::ptrdiff_t(i));
                    runUi("texSetSettings", args({json{{"channels", s["channels"]}}}));
                });
                g->addWidget(rm, int(i), 3);
            }
        }
        v->addWidget(chGrid);
        auto* addRowW = new QWidget(settingsBox);
        auto* ah = new QHBoxLayout(addRowW);
        ah->setContentsMargins(0, 0, 0, 0);
        auto* newName = new QLineEdit(addRowW);
        newName->setObjectName("newChannel");
        newName->setPlaceholderText("New channel (wetness, strokeDir...)");
        auto* newType = new QComboBox(addRowW);
        for (auto [label, t] : {std::pair{"Gray", "gray"}, {"Color", "color"}, {"Vector", "vector"}})
            newType->addItem(label, t);
        setComboDefault(newType, 0);
        auto* add = toolButton(addRowW, "plus", "Add Channel: add the named channel");
        auto addChannel = [this, newName, newType] {
            const std::string n = newName->text().trimmed().toStdString();
            if (n.empty() || settings.channel(n))
                return;
            json s = settings.toJson();
            s["channels"].push_back({{"name", n}, {"type", newType->currentData().toString().toStdString()}, {"bits", 8}});
            runUi("texSetSettings", args({json{{"channels", s["channels"]}}}));
        };
        QObject::connect(add, &QToolButton::clicked, addRowW, addChannel);
        QObject::connect(newName, &QLineEdit::returnPressed, addRowW, addChannel);
        ah->addWidget(newName, 1);
        ah->addWidget(newType);
        ah->addWidget(add);
        v->addWidget(addRowW);

        v->addWidget(boldHeading(settingsBox, "Resolution"));
        auto sizeBox = [](QWidget* parent, int value, bool allowDefault) {
            auto* b = new QSpinBox(parent);
            b->setRange(allowDefault ? 0 : 16, 16384);
            if (allowDefault)
                b->setSpecialValueText("Set");
            b->setValue(value);
            b->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            // Typing commits on Enter or focus out; steps commit at once.
            b->setKeyboardTracking(false);
            std::vector<double> presets(kTextureSizePresets.begin(), kTextureSizePresets.end());
            if (allowDefault)
                presets.push_back(0.0);
            controls::setPresets(b, presets);
            return b;
        };
        auto* res = new QWidget(settingsBox);
        auto* rg = new QGridLayout(res);
        rg->setContentsMargins(0, 0, 0, 0);
        rg->setHorizontalSpacing(4);
        rg->setVerticalSpacing(2);
        rg->setColumnStretch(1, 1);
        rg->setColumnStretch(3, 1);
        rg->addWidget(new QLabel("Default", res), 0, 0);
        auto* w = sizeBox(res, settings.width, false);
        w->setObjectName("width");
        auto* hgt = sizeBox(res, settings.height, false);
        hgt->setObjectName("height");
        setSpinDefault(w, tex::SetSettings{}.width);
        setSpinDefault(hgt, tex::SetSettings{}.height);
        auto* link = toolButton(res, "link", "Link: width and height change together");
        link->setObjectName("linkResolution");
        link->setCheckable(true);
        link->setChecked(PrefStore::get().value<bool>("general", kPrefLinkResolution, true));
        QObject::connect(link, &QToolButton::toggled, res, [](bool on) { PrefStore::get().set("general", kPrefLinkResolution, on); });
        auto setSize = [this, w, hgt, link](QSpinBox* changed) {
            const bool both = link->isChecked();
            runUi("texSetSettings", args({json{{"width", (both ? changed : w)->value()}, {"height", (both ? changed : hgt)->value()}}}));
        };
        QObject::connect(w, &QSpinBox::valueChanged, res, [setSize, w](int) { setSize(w); });
        QObject::connect(hgt, &QSpinBox::valueChanged, res, [setSize, hgt](int) { setSize(hgt); });
        rg->addWidget(w, 0, 1);
        rg->addWidget(new QLabel(QString(QChar(0x00D7)), res), 0, 2);
        rg->addWidget(hgt, 0, 3);
        rg->addWidget(link, 0, 4);
        for (size_t i = 0; i < settings.tiles.size(); ++i)
        {
            const tex::Tile& t = settings.tiles[i];
            const int r = int(i) + 1;
            rg->addWidget(new QLabel(QString::number(t.udim), res), r, 0);
            auto* tw = sizeBox(res, t.width, true);
            tw->setObjectName("tileWidth:" + QString::number(t.udim));
            auto* th = sizeBox(res, t.height, true);
            tw->setToolTip("This tile's width; Set uses the default");
            th->setToolTip("This tile's height; Set uses the default");
            th->setObjectName("tileHeight:" + QString::number(t.udim));
            setSpinDefault(tw, 0);
            setSpinDefault(th, 0);
            auto setTile = [this, i, tw, th, link](QSpinBox* changed) {
                const bool both = link->isChecked();
                json s = settings.toJson();
                json& tile = s["tiles"][i];
                tile.erase("width");
                tile.erase("height");
                if (int v = (both ? changed : tw)->value(); v > 0)
                    tile["width"] = v;
                if (int v = (both ? changed : th)->value(); v > 0)
                    tile["height"] = v;
                runUi("texSetSettings", args({json{{"tiles", s["tiles"]}}}));
            };
            QObject::connect(tw, &QSpinBox::valueChanged, res, [setTile, tw](int) { setTile(tw); });
            QObject::connect(th, &QSpinBox::valueChanged, res, [setTile, th](int) { setTile(th); });
            rg->addWidget(tw, r, 1);
            rg->addWidget(new QLabel(QString(QChar(0x00D7)), res), r, 2);
            rg->addWidget(th, r, 3);
            if (settings.tiles.size() > 1)
            {
                auto* rm = toolButton(res, "trash", "Remove the tile");
                QObject::connect(rm, &QToolButton::clicked, res, [this, i] {
                    json s = settings.toJson();
                    s["tiles"].erase(s["tiles"].begin() + std::ptrdiff_t(i));
                    runUi("texSetSettings", args({json{{"tiles", s["tiles"]}}}));
                });
                rg->addWidget(rm, r, 5);
            }
        }
        v->addWidget(res);
        auto* tileRow = new QWidget(settingsBox);
        auto* tr = new QHBoxLayout(tileRow);
        tr->setContentsMargins(0, 0, 0, 0);
        auto* udim = new QSpinBox(tileRow);
        udim->setRange(1001, 1999);
        int next = 1001;
        for (const tex::Tile& t : settings.tiles)
            next = std::max(next, t.udim + 1);
        udim->setValue(std::min(next, 1999));
        auto* addTile = new QPushButton(icon("plus"), "Add UDIM Tile", tileRow);
        QObject::connect(addTile, &QPushButton::clicked, tileRow, [this, udim] {
            json s = settings.toJson();
            for (const json& t : s["tiles"])
                if (t.value("udim", 0) == udim->value())
                    return;
            s["tiles"].push_back({{"udim", udim->value()}});
            runUi("texSetSettings", args({json{{"tiles", s["tiles"]}}}));
        });
        tr->addWidget(udim);
        tr->addWidget(addTile, 1);
        v->addWidget(tileRow);

        auto* form = new QFormLayout();
        form->setContentsMargins(0, 0, 0, 0);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        v->addWidget(boldHeading(settingsBox, "Surface"));
        auto* conv = new QComboBox(settingsBox);
        conv->addItem("OpenGL (Y+)", "opengl");
        conv->addItem("DirectX (Y-)", "directx");
        conv->setItemData(0, "Green points up. Unity, Godot, Blender, Maya", Qt::ToolTipRole);
        conv->setItemData(1, "Green points down. Unreal, 3ds Max, Source", Qt::ToolTipRole);
        conv->setToolTip("Normals: which way the normal map's green channel points\n"
                         "OpenGL (Y+): Unity, Godot, Blender, Maya\n"
                         "DirectX (Y-): Unreal, 3ds Max, Source");
        conv->setCurrentIndex(std::max(0, conv->findData(qs(settings.normalConvention))));
        texComboDefault(conv, qs(tex::SetSettings{}.normalConvention));
        QObject::connect(conv, &QComboBox::activated, settingsBox,
                         [this, conv](int) { runUi("texSetSettings", args({json{{"normalConvention", conv->currentData().toString().toStdString()}}})); });
        form->addRow("Normals", conv);
        auto cmSpin = [&](double value, double def, double lo, double hi, const char* key, const char* tip) {
            QDoubleSpinBox* sp = controls::valueField(lo, hi, 2, {}, settingsBox);
            sp->setDecimals(2);
            sp->setSuffix(" cm");
            sp->setValue(value);
            sp->setToolTip(tip);
            setSpinDefault(sp, def);
            auto commit = [this, sp, key] { runUi("texSetSettings", args({json{{key, sp->value()}}})); };
            QObject::connect(sp, &QDoubleSpinBox::editingFinished, settingsBox, commit);
            // A middle-click reset changes the value with no edit to finish; a held button means a scrub still going.
            QObject::connect(sp, &QDoubleSpinBox::valueChanged, settingsBox, [sp, commit] {
                if (!sp->hasFocus() && QApplication::mouseButtons() == Qt::NoButton)
                    commit();
            });
            return sp;
        };
        const tex::SetSettings defaults;
        form->addRow("Height Depth", cmSpin(settings.heightDepth, defaults.heightDepth, 0.0, 1000.0, "heightDepth",
                                            "cm from height 0 to height 1: the relief the height channel stands for"));
        auto* uvRow = new QWidget(settingsBox);
        auto* uvl = new QHBoxLayout(uvRow);
        uvl->setContentsMargins(0, 0, 0, 0);
        uvl->addWidget(cmSpin(settings.uvWidth, defaults.uvWidth, 0.01, 100000.0, "uvWidth", "World size of the UV square along U"), 1);
        uvl->addWidget(new QLabel(QString(QChar(0x00D7)), uvRow));
        uvl->addWidget(cmSpin(settings.uvHeight, defaults.uvHeight, 0.01, 100000.0, "uvHeight", "World size of the UV square along V"), 1);
        form->addRow("UV Size", uvRow);
        auto* fromHeight = new QCheckBox("Normals from height", settingsBox);
        fromHeight->setChecked(settings.normalFromHeight);
        setCheckDefault(fromHeight, defaults.normalFromHeight);
        fromHeight->setToolTip("Exports write the normal as detail over a 16-bit height, for materials that rebuild normals from it "
                               "(Unreal's MF_LilaHeightNormal), so steep sides light as walls");
        QObject::connect(fromHeight, &QCheckBox::toggled, settingsBox,
                         [this](bool on) { runUi("texSetSettings", args({json{{"normalFromHeight", on}}})); });
        form->addRow("Export", fromHeight);
        auto* tileable = new QCheckBox("Tileable", settingsBox);
        tileable->setObjectName("tileable");
        tileable->setChecked(settings.tileable);
        setCheckDefault(tileable, defaults.tileable);
        tileable->setToolTip("The texture repeats (trim sheets, tiling materials): strokes wrap round the uv square's edges, and the 2D View tiles it");
        QObject::connect(tileable, &QCheckBox::toggled, settingsBox, [this](bool on) { runUi("texSetSettings", args({json{{"tileable", on}}})); });
        form->addRow("Painting", tileable);
        form->addRow("Used By", usedBy(settingsBox));
        auto* bake = new QLabel(settings.bakeProject.empty() ? QString("None") : qs(settings.bakeProject), settingsBox);
        bake->setToolTip("The bake project whose maps feed the set's mesh inputs (texSetSettings bakeProject=)");
        form->addRow("Bake Project", bake);
        v->addLayout(form);
    }

    /** The materials that pick the set, as names; a material picks it in its Texture Set field. */
    QLabel* usedBy(QWidget* parent)
    {
        std::string names;
        for (const std::string& m : tex::materialsUsing(scene::Graph::current(), set))
            names += (names.empty() ? "" : ", ") + m;
        auto* l = new QLabel(names.empty() ? QString("No material") : qs(names), parent);
        l->setObjectName("usedBy");
        l->setWordWrap(true);
        l->setToolTip("Materials whose Texture Set (or one of whose maps) is this set. Pick it in the Material Editor.");
        return l;
    }

    /** Wide: the smart materials sit under the settings in the side column. Narrow: one column, with them under the layers. */
    void setWide(bool on)
    {
        if (on == wide)
            return;
        wide = on;
        columns->setDirection(on ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
        columns->setSpacing(on ? 8 : 4);
        if (on)
        {
            side->setFixedWidth(kSideWidth);
            sideLayout->addWidget(shelfBox, 1);
            split->setSizes({580, 420});
        }
        else
        {
            side->setMinimumWidth(0);
            side->setMaximumWidth(QWIDGETSIZE_MAX);
            split->addWidget(shelfBox);
            split->setStretchFactor(2, 3);
            split->setSizes({260, 450, 290});
        }
        shelfBox->show();
    }

    void rebuildChannelPicker()
    {
        LILA_ZONE();
        const QString keep = channel->currentData().toString();
        channel->blockSignals(true);
        channel->clear();
        channel->addItem("All channels", QString(kAllChannels));
        for (const tex::Channel& c : settings.channels)
            channel->addItem(qs(c.name), qs(c.name));
        channel->setCurrentIndex(std::max(0, channel->findData(keep)));
        channel->blockSignals(false);
    }

    void rebuildBanner()
    {
        LILA_ZONE();
        if (!graphMode())
        {
            bannerBox->hide();
            return;
        }
        const bool edited = view.value("edited", false);
        bannerText->setText(edited ? "This set was edited as nodes in ways its layers can't show. Rebuild Layers makes the graph from the "
                                     "stack again, dropping those edits."
                                   : "This set's graph was made as nodes. Adding a layer keeps it as the bottom layer of a new stack.");
        rebuild->setVisible(edited);
        bannerBox->show();
    }

    void addShelfItem(const QIcon& ic, const QString& text, const QString& id, const char* kind, const QString& tip, const QString& search,
                      const QStringList& tags = {})
    {
        auto* it = new QListWidgetItem(ic, text, shelf);
        it->setData(Qt::UserRole, id);
        it->setData(Qt::UserRole + 1, QString(kind));
        it->setData(Qt::UserRole + 2, (search + " " + tags.join(' ')).toLower());
        it->setData(Qt::UserRole + 3, tags);
        it->setToolTip(tags.isEmpty() ? tip : tip + (Qt::mightBeRichText(tip) ? "<br>" : "\n") + "Tags: " + tags.join(", "));
    }

    /** The favourites list a shelf item stars into, and its id; none for the Default and Round tiles. */
    static std::pair<std::string, std::string> shelfFavourite(const QString& kind, const QString& id)
    {
        if (id.isEmpty())
            return {};
        return {"textureShelf/" + kind.toStdString(), id.toStdString()};
    }

    /** Favourites move to the front of the shelf, after the Default and Round tiles, in shelf order; the rest keep theirs. */
    void favouritesFirst()
    {
        std::vector<QListWidgetItem*> fixed, fav, rest;
        while (shelf->count() > 0)
        {
            QListWidgetItem* it = shelf->takeItem(0);
            const auto [list, id] = shelfFavourite(it->data(Qt::UserRole + 1).toString(), it->data(Qt::UserRole).toString());
            (list.empty() ? fixed : favourites::has(list, id) ? fav : rest).push_back(it);
        }
        for (const auto* group : {&fixed, &fav, &rest})
            for (QListWidgetItem* it : *group)
                shelf->addItem(it);
    }

    static QStringList qtags(const std::vector<std::string>& tags)
    {
        QStringList out;
        for (const std::string& t : tags)
            out << qs(t);
        return out;
    }

    static QStringList categoryTag(const std::string& category)
    {
        return category.empty() ? QStringList() : QStringList{qs(category).toLower()};
    }

    /** Every kind's items, filtered to one by the switch: smart materials and masks, brush presets, alphas, textures. */
    void rebuildShelf()
    {
        LILA_ZONE();
        std::vector<tex::SmartMaterial> list;
        try
        {
            list = tex::listSmartMaterials();
        }
        catch (const std::exception& e)
        {
            log::warn("Smart materials: {}", e.what());
        }
        const std::vector<tex::brushes::Preset> presets = tex::brushes::all();
        const std::vector<brush::alphas::Entry> alphas = brush::alphas::library();
        const std::vector<std::filesystem::path> textures = textureFiles();
        std::string key;
        for (const tex::SmartMaterial& m : list)
        {
            key += m.name + "|" + m.kind + "|" + m.tier;
            for (const std::string& t : m.tags)
                key += "|" + t;
            key += "\n";
        }
        for (const tex::brushes::Preset& p : presets)
            key += "b|" + p.id + "\n";
        for (const brush::alphas::Entry& a : alphas)
            key += "a|" + a.ref + "\n";
        for (const std::filesystem::path& t : textures)
            key += "t|" + t.string() + "\n";
        if (key == shelfKey)
            return;
        shelfKey = key;
        shelf->clear();
        for (const tex::SmartMaterial& m : list)
        {
            addShelfItem(icon(m.kind == "mask" ? "mask" : "sparkles"), qs(m.name), qs(m.name), m.kind == "mask" ? "mask" : "material",
                         "<b>" + qs(m.name).toHtmlEscaped() + "</b><br>" + (m.description.empty() ? QString() : qs(m.description).toHtmlEscaped() + "<br>") +
                             (m.kind == "mask" ? "Smart mask" : "Smart material") + ", " +
                             qs(m.tier) + ". Double-click to apply.",
                         qs(m.name + " " + m.description), qtags(m.tags));
            materialThumbs::Options o;
            o.size = 128;
            QPointer<QListWidget> shelfView(shelf);
            const QString name = qs(m.name);
            materialThumbs::renderAsync("smart:" + m.name, o, [shelfView, name](const materialThumbs::Result& r, const std::string& err) {
                if (!shelfView || !err.empty())
                    return;
                QPixmap pm(QString::fromStdWString(r.file.wstring()));
                for (int i = 0; i < shelfView->count() && !pm.isNull(); ++i)
                    if (shelfView->item(i)->data(Qt::UserRole).toString() == name)
                        shelfView->item(i)->setIcon(QIcon(pm));
            });
        }
        std::vector<const tex::brushes::Preset*> sorted;
        for (const tex::brushes::Preset& p : presets)
            sorted.push_back(&p);
        std::stable_sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return a->category < b->category; });
        // Drawn like every preset, from the default brush.
        tex::brushes::Preset round;
        round.brush = defaultTexturePaintBrush();
        addShelfItem(presetPreview(round), "Default", QString(), "brush", "Default: the round brush as before any preset", "default round reset");
        for (const tex::brushes::Preset* p : sorted)
            addShelfItem(presetPreview(*p), qs(p->name), qs(p->id), "brush",
                         (p->category.empty() ? QString() : qs(p->category) + ": ") + qs(p->name) + (p->doc.empty() ? QString() : "\n" + qs(p->doc)),
                         qs(p->category + " " + p->name), categoryTag(p->category));
        addShelfItem(icon("circle"), "Round", QString(), "alpha", "Round: no alpha, the falloff alone shapes the tip", "round none");
        for (const brush::alphas::Entry& a : alphas)
            addShelfItem(imageTile(a.file, 64), qs(a.name), qs(a.ref), "alpha",
                         (a.category.empty() ? QString() : qs(a.category) + ": ") + qs(a.name) + "\n" + qs(a.sourceLabel),
                         qs(a.category + " " + a.name + " " + a.sourceLabel), categoryTag(a.category));
        for (const std::filesystem::path& t : textures)
            addShelfItem(imageTile(t, 64), QString::fromStdWString(t.stem().wstring()), QString::fromStdWString(t.wstring()), "texture",
                         QString::fromStdWString(t.filename().wstring()) + "\nClick to paint through it", QString::fromStdWString(t.stem().wstring()));
        favouritesFirst();
        filterShelf();
    }

    int shelfKindIndex() const { return shelfKind ? shelfKind->current() : kShelfMaterials; }
    bool shelfShowsMasks() const { return shelfKindIndex() == kShelfMasks; }

    /** The item the brush uses now on the brush kinds, so the shelf shows the current pick. */
    QString shelfCurrentId(int kind) const
    {
        if (kind == kShelfBrushes)
            return qs(texturePaintPreset());
        if (kind == kShelfAlphas)
            return qs(texturePaintAlpha());
        if (kind == kShelfTextures)
            return QString::fromStdWString(std::filesystem::path(texturePaintTexture()).wstring());
        return {};
    }

    /** Shows the kind the switch picks, narrowed by the search; masks need a layer to go on, so without one the shelf says so. */
    void filterShelf()
    {
        LILA_ZONE();
        const int kind = shelfKindIndex();
        const QString want = kShelfKindNames[kind];
        const QString needle = shelfSearch ? shelfSearch->text().trimmed().toLower() : QString();
        const bool picker = kind == kShelfBrushes || kind == kShelfAlphas || kind == kShelfTextures;
        const QString current = picker ? shelfCurrentId(kind) : QString();
        QListWidgetItem* pick = nullptr;
        // The tags offered are those of the kind's items the search leaves.
        QMap<QString, int> counts;
        for (int i = 0; i < shelf->count(); ++i)
        {
            QListWidgetItem* it = shelf->item(i);
            if (it->data(Qt::UserRole + 1).toString() == want && (needle.isEmpty() || it->data(Qt::UserRole + 2).toString().contains(needle)))
                for (const QString& t : it->data(Qt::UserRole + 3).toStringList())
                    ++counts[t];
        }
        if (shelfTags)
            shelfTags->setAvailable(counts);
        for (int i = 0; i < shelf->count(); ++i)
        {
            QListWidgetItem* it = shelf->item(i);
            const bool show = it->data(Qt::UserRole + 1).toString() == want && (needle.isEmpty() || it->data(Qt::UserRole + 2).toString().contains(needle)) &&
                              (!shelfTags || shelfTags->matches(it->data(Qt::UserRole + 3).toStringList()));
            it->setHidden(!show);
            if (show && picker && it->data(Qt::UserRole).toString() == current)
                pick = it;
        }
        if (picker)
        {
            const QSignalBlocker b(shelf);
            shelf->setCurrentItem(pick);
        }
        else if (QListWidgetItem* cur = shelf->currentItem(); cur && cur->isHidden())
            shelf->setCurrentItem(nullptr);
        const bool masks = kind == kShelfMasks;
        const bool haveLayer = layer(selLayer) != nullptr;
        shelfApply->setVisible(kind == kShelfMaterials || masks);
        shelfApply->setText(masks ? "Add Mask to Layer" : "Apply");
        shelfApply->setEnabled(!masks || haveLayer);
        shelfHint->setVisible(masks && !haveLayer);
        if (shelfAdd)
            shelfAdd->setVisible(kind == kShelfTextures);
        shelf->setDragEnabled(kind == kShelfMaterials || masks);
        // Brush strokes are wide; the rest are square tiles.
        shelf->setIconSize(kind == kShelfBrushes ? QSize(84, 56) : QSize(64, 64));
    }

    /** Uses a brush, alpha or texture item at once; smart materials and masks wait for a double-click or Apply. */
    void pickShelfItem(QListWidgetItem* it, bool apply)
    {
        if (!it)
            return;
        const QString kind = it->data(Qt::UserRole + 1).toString();
        const std::string id = it->data(Qt::UserRole).toString().toStdString();
        if (kind == "brush" && id.empty())
            resetTexturePaintBrush();
        else if (kind == "brush")
            applyTexturePaintPreset(id);
        else if (kind == "alpha")
            setTexturePaintAlpha(id);
        else if (kind == "texture")
            setTexturePaintTexture(paths::toUtf8(std::filesystem::path(it->data(Qt::UserRole).toString().toStdWString())));
        else if (apply)
            self->applySmartMaterial(id);
    }

    void setShelfKind(int kind)
    {
        shelfKind->setCurrent(kind);
        filterShelf();
    }

    void setLowerPage(int page)
    {
        lowerKind->setCurrent(page);
        lower->setCurrentIndex(page);
    }

    // Properties ---------------------------------------------------------------------------------------------------------

    /** Edits from the properties area keep it in place (a dragged slider survives the refresh its edit causes). */
    void propsLayer(const json& patch)
    {
        keepProps = true;
        setLayer(selLayer, patch);
    }

    void propsMask(const json& patch)
    {
        keepProps = true;
        setMask(selLayer, selMask, patch);
    }

    void layerProps(QWidget* w, QVBoxLayout* v, const json& l)
    {
        const std::string kind = l.value("kind", "fill");
        const std::string id = l.value("id", "");
        auto* head = boldHeading(w, qs(l.value("name", id)) + "  (" + capitalised(kind) + " layer, " + qs(id) + ")");
        v->addWidget(head);

        if (kind == "decal" || kind == "text" || kind == "scatter")
            placedProps(w, v, l, kind);
        if (kind == "fill" || kind == "paint" || kind == "decal" || kind == "text" || kind == "scatter")
        {
            json node = {{"name", id}, {"label", kind == "paint" ? "Brush Material" : "Material"}, {"params", json::array()}};
            json inputs = json::array();
            const json material = l.value("material", json::object());
            const json resolved = l.value("resolved", json::object());
            for (const tex::Channel& c : settings.channels)
            {
                if (!material.contains(c.name) && !resolved.value(c.name, json::object()).value("enabled", false))
                    continue;
                json def = tex::channelDefault(c.name, c.type);
                inputs.push_back({{"id", c.name}, {"type", tex::portTypeName(c.type)}, {"default", def}, {"value", material.value(c.name, def)}, {"linked", false}});
            }
            node["inputs"] = inputs;
            if (kind == "paint")
            {
                auto* paint = new QPushButton(icon("brush"), "Paint", w);
                paint->setToolTip("Paint: the texture paint tool, painting into this layer (Texture > Texture Paint)");
                const std::string layerId = id;
                connect(paint, &QPushButton::clicked, w, [this, layerId] {
                    textureSets::setActiveLayer(set, layerId);
                    actions::run(json("tex.paint"));
                });
                v->addWidget(paint);
            }
            v->addWidget(texParams::nodePanel(w, node, [this](const std::string& c, const json& value) { propsLayer({{"material", {{c, value}}}}); }));
            if (kind == "fill" || kind == "decal" || kind == "text")
                projectionProps(w, v, l.value("projection", json::object()), id, {}, [this](const json& p) { propsLayer({{"projection", p}}); });
        }
        else if (kind == "procedural")
        {
            v->addWidget(dim(w, "A texture graph whose outputs are named after channels (and mask). Open it as nodes to edit it."));
            auto* open = new QPushButton(icon("binary-tree"), "Open as Nodes", w);
            QObject::connect(open, &QPushButton::clicked, w, [this] { self->openSelectedAsNodes(); });
            v->addWidget(open);
            // The settings its graph exposes, then how the whole content repeats.
            if (json exposed = proceduralSettings(l); !exposed["params"].empty())
                v->addWidget(texParams::nodePanel(w, exposed, [this](const std::string& p, const json& value) { propsLayer({{"params", {{p, value}}}}); }));
            const json t = l.value("tiling", json::object());
            const json tilingValues = {{"tiles", t.value("scale", 1.0)}, {"rotation", t.value("rotation", 0.0)}, {"offsetX", t.value("offsetX", 0.0)},
                                       {"offsetY", t.value("offsetY", 0.0)}};
            json tilingNode = defNodeJson("", "layerTiling", tilingValues);
            tilingNode["label"] = "Tiling";
            v->addWidget(texParams::nodePanel(w, tilingNode, [this](const std::string& p, const json& value) {
                propsLayer({{"tiling", {{p == "tiles" ? std::string("scale") : p, value}}}});
            }));
            projectionProps(w, v, l.value("projection", json::object()), id, {}, [this](const json& p) { propsLayer({{"projection", p}}); });
        }
        else if (kind == "filter")
        {
            const std::string type = l.value("filter", "");
            v->addWidget(texParams::nodePanel(w, defNodeJson(id, type, l.value("filterValues", json::object())),
                                              [this](const std::string& p, const json& value) { propsLayer({{"filterValues", {{p, value}}}}); }));
        }
        else if (kind == "instance")
        {
            auto* form = new QFormLayout();
            auto* src = new QComboBox(w);
            src->setObjectName("instanceSource");
            std::function<void(const json&)> collect = [&](const json& layers) {
                for (const json& x : layers)
                {
                    const std::string k = x.value("kind", "");
                    if (k == "folder")
                        collect(x.value("children", json::array()));
                    else if (k != "instance")
                        src->addItem(qs(x.value("name", "")) + " (" + qs(x.value("id", "")) + ")", qs(x.value("id", "")));
                }
            };
            collect(view["layers"]);
            src->setCurrentIndex(src->findData(qs(l.value("source", ""))));
            QObject::connect(src, &QComboBox::activated, w, [this, src](int) { propsLayer({{"source", src->currentData().toString().toStdString()}}); });
            form->addRow("Source", src);
            v->addLayout(form);
        }

        v->addWidget(boldHeading(w, "Channels"));
        auto* grid = new QWidget(w);
        auto* g = new QGridLayout(grid);
        g->setContentsMargins(0, 0, 0, 0);
        g->setVerticalSpacing(2);
        int r = 0;
        const json resolved = l.value("resolved", json::object());
        for (const tex::Channel& c : settings.channels)
        {
            const json rc = resolved.value(c.name, json::object());
            auto* on = new QCheckBox(qs(c.name), grid);
            on->setObjectName("propChannel:" + qs(c.name));
            on->setChecked(rc.value("enabled", false));
            QObject::connect(on, &QCheckBox::toggled, grid, [this, id, c = c.name](bool b) {
                keepProps = false;
                toggleChannel(id, c, b);
            });
            g->addWidget(on, r, 0);
            auto* mode = new QComboBox(grid);
            mode->setObjectName("propMode:" + qs(c.name));
            const std::vector<std::string> normalModes = {"rnm", "normal"};
            for (const std::string& m : c.name == "normal" ? normalModes : tex::layerBlendModes())
                mode->addItem(capitalised(m), qs(m));
            mode->setCurrentIndex(std::max(0, mode->findData(qs(rc.value("mode", "normal")))));
            texComboDefault(mode, "normal");
            QObject::connect(mode, &QComboBox::activated, grid, [this, id, mode, c = c.name](int) {
                if (const json* cur = layer(id))
                    propsLayer({{"channels", {{c, channelEntry(*cur, c, {{"mode", mode->currentData().toString().toStdString()}})}}}});
            });
            g->addWidget(mode, r, 1);
            auto* op = percentSpin(grid, rc.value("opacity", 1.0));
            op->setObjectName("propOpacity:" + qs(c.name));
            QObject::connect(op, &QDoubleSpinBox::valueChanged, grid, [this, id, c = c.name](double value) {
                if (const json* cur = layer(id))
                    propsLayer({{"channels", {{c, channelEntry(*cur, c, {{"opacity", value / 100.0}})}}}});
            });
            g->addWidget(op, r, 2);
            ++r;
        }
        v->addWidget(grid);

        if (kind != "folder")
        {
            const json hb = l.value("heightBlend", json{{"enabled", false}, {"sharpness", 0.8}, {"influence", 1.0}});
            auto* hbOn = new QCheckBox("Height blend", w);
            hbOn->setToolTip("Mix the layer in by its height as well as its mask (needs its height channel on)");
            hbOn->setChecked(hb.value("enabled", false));
            setCheckDefault(hbOn, false);
            v->addWidget(hbOn);
            auto* form = new QFormLayout();
            form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
            auto* sharp = new controls::SliderField(w);
            sharp->setDecimals(2);
            sharp->setRange(0.0, 1.0);
            sharp->setSingleStep(0.05);
            sharp->setKeyboardTracking(false);
            sharp->setValue(hb.value("sharpness", 0.8));
            setSpinDefault(sharp, 0.8);
            auto* infl = new controls::SliderField(w);
            infl->setDecimals(2);
            infl->setRange(0.0, 4.0);
            infl->setSingleStep(0.05);
            infl->setKeyboardTracking(false);
            infl->setValue(hb.value("influence", 1.0));
            setSpinDefault(infl, 1.0);
            auto setHb = [this, hbOn, sharp, infl] {
                propsLayer({{"heightBlend", {{"enabled", hbOn->isChecked()}, {"sharpness", sharp->value()}, {"influence", infl->value()}}}});
            };
            QObject::connect(hbOn, &QCheckBox::toggled, w, setHb);
            sharp->setEnabled(hbOn->isChecked());
            infl->setEnabled(hbOn->isChecked());
            QObject::connect(hbOn, &QCheckBox::toggled, sharp, &QWidget::setEnabled);
            QObject::connect(hbOn, &QCheckBox::toggled, infl, &QWidget::setEnabled);
            QObject::connect(sharp, &QDoubleSpinBox::valueChanged, w, setHb);
            QObject::connect(infl, &QDoubleSpinBox::valueChanged, w, setHb);
            form->addRow("Sharpness", sharp);
            form->addRow("Influence", infl);
            v->addLayout(form);
        }

        auto* aForm = new QFormLayout();
        auto* anchor = new QLineEdit(qs(l.value("anchor", "")), w);
        anchor->setObjectName("anchorName");
        anchor->setPlaceholderText("None");
        anchor->setToolTip("Names the composite right after this layer, for anchor masks and generator inputs of layers above");
        QObject::connect(anchor, &QLineEdit::editingFinished, w, [this, anchor, id, was = l.value("anchor", "")] {
            const std::string next = anchor->text().trimmed().toStdString();
            if (next == was)
                return;
            if (next.empty())
                setLayer(id, {{"anchor", nullptr}});
            else
                runUi("texAddAnchor", args({id, next}));
        });
        aForm->addRow("Anchor", anchor);
        v->addLayout(aForm);
    }

    /** A row naming a picture file, picking another on click. */
    QToolButton* pictureButton(QWidget* w, const std::string& file, std::function<void(const std::string&)> picked)
    {
        auto* b = new QToolButton(w);
        controls::setRole(b, "row");
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setIcon(icon("photo"));
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        b->setText(file.empty() ? QString("None") : QFileInfo(qs(file)).fileName());
        b->setToolTip(file.empty() ? QString("Pick a picture") : qs(file));
        QObject::connect(b, &QToolButton::clicked, w, [this, picked] {
            const QString f = ui::fileDialog::getOpenFileName(this->self, "Picture", {}, "Images (*.png *.jpg *.jpeg *.tga *.tif *.tiff *.exr *.psd)");
            if (!f.isEmpty())
                picked(f.toStdString());
        });
        return b;
    }

    /** Decal, text and scatter: what they draw (pictures, words, pattern) and how. */
    void placedProps(QWidget* w, QVBoxLayout* v, const json& l, const std::string& kind)
    {
        auto* form = new QFormLayout();
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto spin = [&](const char* objectName, double lo, double hi, double step, int decimals, double value, std::function<void(double)> set) {
            auto* f = new QDoubleSpinBox(w);
            f->setObjectName(objectName);
            f->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            f->setRange(lo, hi);
            f->setSingleStep(step);
            f->setDecimals(decimals);
            f->setKeyboardTracking(false);
            f->setValue(value);
            QObject::connect(f, &QDoubleSpinBox::valueChanged, w, [set](double x) { set(x); });
            return f;
        };
        if (kind == "decal")
        {
            const json dec = l.value("decal", json::object());
            const json maps = dec.value("maps", json::object());
            for (const tex::Channel& c : settings.channels)
            {
                const std::string ch = c.name;
                form->addRow(qs(ch), pictureButton(w, maps.value(ch, std::string()), [this, ch](const std::string& f) {
                    propsLayer({{"decal", {{"maps", {{ch, f}}}}}});
                }));
            }
            auto* atlas = new QHBoxLayout;
            for (const char* k : {"columns", "rows", "cell"})
                atlas->addWidget(spin(k, k == std::string("cell") ? 0 : 1, 64, 1, 0, dec.value(k, k == std::string("cell") ? 0 : 1),
                                      [this, key = std::string(k)](double x) { propsLayer({{"decal", {{key, int(x)}}}}); }));
            auto* atlasRow = new QWidget(w);
            atlas->setContentsMargins(0, 0, 0, 0);
            atlasRow->setLayout(atlas);
            atlasRow->setToolTip("Atlas: columns, rows and the cell shown (left to right, top to bottom)");
            form->addRow("Atlas", atlasRow);
        }
        else if (kind == "text")
        {
            const json t = l.value("text", json::object());
            auto* words = new QLineEdit(qs(t.value("text", std::string())), w);
            words->setObjectName("layerText");
            QObject::connect(words, &QLineEdit::editingFinished, w, [this, words] { propsLayer({{"text", {{"text", words->text().toStdString()}}}}); });
            form->addRow("Text", words);
            auto* font = new QComboBox(w);
            font->setObjectName("layerFont");
            font->addItem("Default", QString());
            for (const std::string& f : curve::fontNames())
                font->addItem(qs(f), qs(f));
            font->setCurrentIndex(std::max(0, font->findData(qs(t.value("font", std::string())))));
            QObject::connect(font, &QComboBox::activated, w, [this, font](int) { propsLayer({{"text", {{"font", font->currentData().toString().toStdString()}}}}); });
            form->addRow("Font", font);
            form->addRow("Tracking", spin("layerTracking", -0.5, 2.0, 0.01, 3, t.value("tracking", 0.0),
                                          [this](double x) { propsLayer({{"text", {{"tracking", x}}}}); }));
            form->addRow("Outline", spin("layerOutline", 0.0, 0.5, 0.01, 3, t.value("outline", 0.0),
                                         [this](double x) { propsLayer({{"text", {{"outline", x}}}}); }));
        }
        else
        {
            const json sc = l.value("scatter", json::object());
            form->addRow("Pattern", pictureButton(w, sc.value("pattern", std::string()), [this](const std::string& f) {
                propsLayer({{"scatter", {{"pattern", f}}}});
            }));
            form->addRow("Picture", pictureButton(w, sc.value("maps", json::object()).value("baseColor", std::string()), [this](const std::string& f) {
                propsLayer({{"scatter", {{"maps", {{"baseColor", f}}}}}});
            }));
            struct Num
            {
                const char* key;
                const char* label;
                double lo, hi, step, def;
                int decimals;
            };
            const Num nums[] = {{"countX", "Across", 1, 64, 1, 6, 0},       {"countY", "Down", 1, 64, 1, 6, 0},
                                {"perCell", "Per cell", 1, 16, 1, 3, 0},    {"size", "Size", 0.01, 2, 0.05, 0.6, 2},
                                {"sizeRandom", "Size jitter", 0, 1, 0.05, 0.4, 2}, {"rotationRandom", "Turn jitter", 0, 1, 0.05, 1.0, 2},
                                {"hueRandom", "Hue jitter", 0, 1, 0.05, 0.0, 2},   {"seed", "Seed", 0, 99999, 1, 0, 0}};
            for (const Num& n : nums)
                form->addRow(n.label, spin(n.key, n.lo, n.hi, n.step, n.decimals, sc.value(n.key, n.def), [this, key = std::string(n.key), d = n.decimals](double x) {
                    propsLayer({{"scatter", {{key, d == 0 ? json(int(x)) : json(x)}}}});
                }));
            auto* avoid = new QCheckBox("Avoid overlap", w);
            avoid->setObjectName("scatterAvoid");
            avoid->setToolTip("One copy per cell, kept inside it");
            avoid->setChecked(sc.value("avoidOverlap", false));
            QObject::connect(avoid, &QCheckBox::toggled, w, [this](bool on) { propsLayer({{"scatter", {{"avoidOverlap", on}}}}); });
            form->addRow(avoid);
        }
        v->addLayout(form);
    }

    /** Where the layer's or mask's content lands: layerProjection's fields, and a button to place it on the mesh. */
    void projectionProps(QWidget* w, QVBoxLayout* v, const json& projection, const std::string& layer, const std::string& mask,
                         std::function<void(const json&)> patch)
    {
        json node = defNodeJson("", "layerProjection", projection);
        node["label"] = "Projection";
        v->addWidget(texParams::nodePanel(w, node, [patch](const std::string& p, const json& value) { patch({{p, value}}); }));
        auto* place = new QPushButton(icon("cube-3d-sphere"), "Place on Mesh", w);
        place->setObjectName("placeProjection");
        place->setToolTip("Place on Mesh: click the mesh to put the projection there; drag to move it, Shift turns, Ctrl scales");
        const std::string setName = set;
        QObject::connect(place, &QPushButton::clicked, w, [setName, layer, mask] { startProjectionPlacement(setName, layer, mask); });
        v->addWidget(place);
    }

    void maskProps(QWidget* w, QVBoxLayout* v, const json& m)
    {
        const std::string kind = m.value("kind", "fill");
        const std::string id = m.value("id", "");
        v->addWidget(boldHeading(w, maskName(m) + "  (" + capitalised(kind) + " mask, " + qs(id) + ")"));
        if (kind == "fill")
        {
            auto* form = new QFormLayout();
            auto* value = new controls::SliderField(w);
            value->setDecimals(2);
            value->setObjectName("maskValue");
            value->setRange(0.0, 1.0);
            value->setSingleStep(0.05);
            value->setKeyboardTracking(false);
            value->setValue(m.value("value", 1.0));
            setSpinDefault(value, 1.0);
            QObject::connect(value, &QDoubleSpinBox::valueChanged, w, [this](double x) { propsMask({{"value", x}}); });
            form->addRow("Value", value);
            v->addLayout(form);
        }
        else if (kind == "generator" || kind == "filter" || kind == "levels")
        {
            const std::string type = kind == "levels" ? std::string("levels") : m.value("type", "");
            v->addWidget(texParams::nodePanel(w, defNodeJson(id, type, m.value("values", json::object())),
                                              [this](const std::string& p, const json& value) { propsMask({{"values", {{p, value}}}}); }));
            if (kind == "generator")
                if (const tex::NodeDef* def = tex::findNode(type); def && !def->inputs.empty())
                {
                    v->addWidget(boldHeading(w, "Inputs"));
                    auto* form = new QFormLayout();
                    const json bindings = m.value("bindings", json::object());
                    for (const tex::PortDecl& in : def->inputs)
                    {
                        auto* b = new QComboBox(w);
                        b->setObjectName("binding:" + qs(in.id));
                        b->addItem(tex::findMeshRole(in.id) ? "Mesh " + qs(in.id) : QString("Default"), QString());
                        b->addItem("None", "none");
                        for (const tex::MeshRole& role : tex::meshRoles())
                            b->addItem("Mesh " + qs(role.name), qs(role.name));
                        const json anchors = view.value("anchors", json::object());
                        for (auto& [anchorName, owner] : anchors.items())
                            for (const tex::Channel& c : settings.channels)
                                b->addItem("Anchor " + qs(anchorName) + "." + qs(c.name), qs("anchor:" + anchorName + "." + c.name));
                        b->setCurrentIndex(std::max(0, b->findData(qs(bindings.value(in.id, "")))));
                        texComboDefaultIndex(b, 0);
                        QObject::connect(b, &QComboBox::activated, w, [this, b, port = in.id](int) {
                            const QString value = b->currentData().toString();
                            propsMask({{"bindings", {{port, value.isEmpty() ? json(nullptr) : json(value.toStdString())}}}});
                        });
                        form->addRow(qs(in.label.empty() ? in.id : in.label), b);
                    }
                    v->addLayout(form);
                }
        }
        if (kind == "generator" || kind == "paint")
            projectionProps(w, v, m.value("projection", json::object()), selLayer, id, [this](const json& p) { propsMask({{"projection", p}}); });
        if (kind == "anchor")
        {
            auto* form = new QFormLayout();
            auto* a = new QComboBox(w);
            a->setObjectName("maskAnchor");
            const json anchors = view.value("anchors", json::object());
            for (auto& [anchorName, owner] : anchors.items())
                a->addItem(qs(anchorName), qs(anchorName));
            a->setCurrentIndex(std::max(0, a->findData(qs(m.value("anchor", "")))));
            QObject::connect(a, &QComboBox::activated, w, [this, a](int) { propsMask({{"anchor", a->currentData().toString().toStdString()}}); });
            form->addRow("Anchor", a);
            auto* c = new QComboBox(w);
            for (const tex::Channel& ch : settings.channels)
                c->addItem(qs(ch.name), qs(ch.name));
            c->setCurrentIndex(std::max(0, c->findData(qs(m.value("channel", "height")))));
            texComboDefault(c, "height");
            QObject::connect(c, &QComboBox::activated, w, [this, c](int) { propsMask({{"channel", c->currentData().toString().toStdString()}}); });
            form->addRow("Channel", c);
            v->addLayout(form);
        }
        else if (kind == "paint")
        {
            v->addWidget(dim(w, "Image: " + (m.value("image", "").empty() ? QString("None") : qs(m.value("image", "")))));
        }
    }

    void rebuildProps()
    {
        LILA_ZONE();
        std::string key = selLayer + "|" + selMask + "|";
        if (const json* l = layer(selLayer))
            key += selMask.empty() ? l->dump() : (mask(selLayer, selMask) ? mask(selLayer, selMask)->dump() : std::string());
        key += "|" + settingsKey;
        if (key == propsKey && props)
            return;
        propsKey = key;
        auto* w = new QWidget();
        w->setObjectName("textureSetProperties");
        auto* v = new QVBoxLayout(w);
        v->setContentsMargins(6, 4, 6, 6);
        v->setSpacing(4);
        const json* l = layer(selLayer);
        const json* m = selMask.empty() ? nullptr : mask(selLayer, selMask);
        if (m)
            maskProps(w, v, *m);
        else if (l)
            layerProps(w, v, *l);
        else
            v->addWidget(dim(w, setNode() ? "No layer selected. Click a layer or mask above to edit it." : "No texture set."));
        v->addStretch(1);
        propsScroll->setWidget(w);
        props = w;
    }

    // Refresh ------------------------------------------------------------------------------------------------------------

    void refreshVariants()
    {
        LILA_ZONE();
        if (!variants)
            return;
        variants->blockSignals(true);
        variants->clear();
        variants->addItem("Base", QString());
        const json names = view.is_object() ? view.value("variants", json::array()) : json::array();
        for (const json& v : names)
            variants->addItem(qs(v.get<std::string>()), qs(v.get<std::string>()));
        variants->insertSeparator(variants->count());
        variants->addItem("New Variant...", QString("\x01new"));
        const std::string active = view.is_object() ? view.value("variant", std::string()) : std::string();
        if (!active.empty())
            variants->addItem("Remove " + qs(active), QString("\x01remove"));
        variants->setCurrentIndex(std::max(0, variants->findData(qs(active))));
        variants->setEnabled(setNode() != nullptr);
        variants->blockSignals(false);
    }

    void refreshSets()
    {
        LILA_ZONE();
        std::vector<std::string> names = tex::setNames(scene::Graph::current());
        sets->blockSignals(true);
        sets->clear();
        for (const std::string& n : names)
            sets->addItem(icon("texture"), qs(n), qs(n));
        if (!(set.empty() && noSetForSelection) && (set.empty() || std::find(names.begin(), names.end(), set) == names.end()))
            set = names.empty() ? std::string() : names.front();
        sets->setCurrentIndex(set.empty() ? -1 : sets->findData(qs(set)));
        sets->blockSignals(false);
    }

    void refresh()
    {
        LILA_ZONE();
        refreshSets();
        texture2d::showSet(set);
        scene::Node* n = setNode();
        json nextView;
        tex::SetSettings nextSettings;
        if (n)
        {
            try
            {
                tex::Graph g = tex::readGraph(*n);
                nextSettings = tex::readSettings(*n);
                nextView = tex::stackView(tex::readStack(g, nextSettings), nextSettings, g);
            }
            catch (const std::exception& e)
            {
                log::warn("Texture Set panel: {}", e.what());
            }
        }
        empty->setVisible(!n);
        emptyText->setText(noSetForSelection ? "The selected meshes' materials use no texture set. Click New Texture Set to make one, or pick one in the Material Editor."
                                             : "No texture set. Click New Texture Set to make one for the selected meshes.");
        const std::string sKey = n ? nextSettings.toJson().dump() : std::string();
        const std::string vKey = set + "|" + nextView.dump() + "|" + sKey + "|" + shownChannel() + "|" + textureSets::activeLayer(set);
        settings = nextSettings;
        view = nextView;
        refreshVariants();
        if (sKey != settingsKey || !settingsBox->layout())
        {
            settingsKey = sKey;
            QWidget* focus = QApplication::focusWidget();
            const QString focusName = focus && settingsBox->isAncestorOf(focus) ? focus->objectName() : QString();
            rebuildSettings();
            // The old widgets linger hidden until deleteLater runs, under the same names.
            for (QWidget* again : focusName.isEmpty() ? QList<QWidget*>() : settingsBox->findChildren<QWidget*>(focusName))
                if (!again->isHidden())
                {
                    again->setFocus();
                    break;
                }
            rebuildChannelPicker();
        }
        if (vKey != viewKey)
        {
            viewKey = vKey;
            rebuildTree();
        }
        else
            updateThumbs();
        rebuildBanner();
        // A hidden panel (a closed dock) scans the libraries and draws their thumbnails when it shows.
        if (self->isVisible())
            rebuildShelf();
        else
            shelfStale = true;
        filterShelf();
        if (!std::exchange(keepProps, false))
            rebuildProps();
    }

    void scheduleRefresh()
    {
        if (!refreshTimer->isActive())
            refreshTimer->start();
    }

    void onCurrentItem(QTreeWidgetItem* it)
    {
        if (building)
            return;
        if (!it)
        {
            selLayer.clear();
            selMask.clear();
        }
        else if (it->data(0, kRoleKind).toInt() == 1)
        {
            selLayer = it->data(0, kRoleLayer).toString().toStdString();
            selMask = it->data(0, kRoleId).toString().toStdString();
        }
        else
        {
            selLayer = it->data(0, kRoleId).toString().toStdString();
            selMask.clear();
        }
        if (const json* l = layer(selLayer); l && selMask.empty() && l->value("kind", "") == "paint")
            textureSets::setActiveLayer(set, selLayer);
        keepProps = false;
        rebuildProps();
        filterShelf();
    }

    // Menus --------------------------------------------------------------------------------------------------------------

    void fillLayerMenu(QMenu* menu)
    {
        menu->addAction(icon("bucket"), "Add Fill Layer", [this] { addLayer("fill", {}); });
        menu->addAction(icon("brush"), "Add Paint Layer", [this] { addLayer("paint", {}); });
        menu->addAction(icon("topology-star-3"), "Add Procedural Layer", [this] { addLayer("procedural", {}); });
        menu->addAction(icon("sticker"), "Add Decal Layer", [this] { addLayer("decal", {}); });
        menu->addAction(icon("typography"), "Add Text Layer", [this] { addLayer("text", {}); });
        menu->addAction(icon("confetti"), "Add Scatter Layer", [this] { addLayer("scatter", {}); });
        menu->addAction(icon("folder-plus"), "Add Folder", [this] { addLayer("folder", {}); });
        menu->addAction(icon("color-swatch"), "Add Layers per ID", [this] { addIdLayers(); });
        QMenu* filters = menu->addMenu(icon("filter"), "Add Filter Layer");
        for (const tex::NodeDef* def : filterNodes())
            filters->addAction(qs(def->label.empty() ? def->type : def->label), [this, t = def->type] { addLayer("filter", t); });
        const json* sel = layer(selLayer);
        const std::string selKind = sel ? sel->value("kind", "") : std::string();
        QAction* inst = menu->addAction(icon("link"), "Add Instance of Selected", [this] { addLayer("instance", {}); });
        inst->setEnabled(sel && selKind != "folder" && selKind != "instance");
    }

    void fillMaskMenu(QMenu* menu)
    {
        const bool any = layer(selLayer) != nullptr;
        QMenu* m = menu->addMenu(icon("mask"), "Add Mask");
        m->setEnabled(any);
        m->addAction(icon("square"), "Fill White", [this] { addMask("white", {}); });
        m->addAction(icon("square"), "Fill Black", [this] { addMask("black", {}); });
        m->addAction(icon("brush"), "Paint", [this] { addMask("paint", {}); });
        m->addAction(icon("adjustments"), "Levels", [this] { addMask("levels", {}); });
        QMenu* f = m->addMenu(icon("filter"), "Filter");
        for (const tex::NodeDef* def : filterNodes())
            f->addAction(qs(def->label.empty() ? def->type : def->label), [this, t = def->type] { addMask("filter", t); });
        QMenu* a = m->addMenu(icon("anchor"), "Anchor");
        const json anchors = view.is_object() ? view.value("anchors", json::object()) : json::object();
        a->setEnabled(!anchors.empty());
        for (auto& [name, owner] : anchors.items())
            a->addAction(qs(name), [this, n = name] { addMask("anchor", n); });
        QMenu* g = menu->addMenu(icon("wand"), "Add Generator");
        g->setEnabled(any);
        for (const std::string& t : tex::generatorTypes())
            g->addAction(nodeLabel(t), [this, t] { addMask("generator", t); });
        QAction* anchor = menu->addAction(icon("anchor"), "Add Anchor...", [this] {
            bool ok = false;
            QString name = QInputDialog::getText(self, "Add Anchor", "Anchor name", QLineEdit::Normal, "anchor", &ok);
            if (ok && !name.trimmed().isEmpty())
                runUi("texAddAnchor", args({selLayer, name.trimmed().toStdString()}));
        });
        anchor->setEnabled(any);
    }

    void contextMenu(const QPoint& pos)
    {
        if (QTreeWidgetItem* it = tree->itemAt(pos))
            tree->setCurrentItem(it);
        if (!setNode())
            return;
        QMenu menu(tree);
        fillLayerMenu(&menu);
        menu.addSeparator();
        fillMaskMenu(&menu);
        menu.addSeparator();
        const bool any = layer(selLayer) != nullptr;
        QAction* dup = menu.addAction(icon("copy"), "Duplicate", [this] { self->duplicateSelected(); });
        dup->setEnabled(any && selMask.empty());
        menu.addAction(icon("clipboard-copy"), "Copy Layer", [this] { self->copySelected(); })->setEnabled(any && selMask.empty());
        menu.addAction(icon("clipboard"), "Paste Layers", [this] { self->paste(); })
            ->setEnabled(QApplication::clipboard()->text().contains("\"lilaLayers\""));
        menu.addAction(icon("arrow-merge"), "Merge Down", [this] { self->flattenSelected(true); })->setEnabled(any && selMask.empty());
        menu.addAction(icon("stack-pop"), "Flatten Layer", [this] { self->flattenSelected(false); })->setEnabled(any && selMask.empty());
        menu.addAction(icon("file-export"), "Export Flattened...", [this] { self->exportFlattened(); })->setEnabled(any && selMask.empty());
        menu.addAction(icon("trash"), selMask.empty() ? "Delete Layer" : "Delete Mask", [this] { self->deleteSelected(); })->setEnabled(any);
        menu.addSeparator();
        QAction* nodes = menu.addAction(icon("binary-tree"), "Open as Nodes", [this] { self->openSelectedAsNodes(); });
        nodes->setShortcut(QKeySequence("Ctrl+G"));
        nodes->setEnabled(any);
        menu.addAction(icon("sparkles"), "Save as Smart Material...", [this] { saveSmart(false); })->setEnabled(any);
        const json* l = layer(selLayer);
        menu.addAction(icon("mask"), "Save Masks as Smart Mask...", [this] { saveSmart(true); })
            ->setEnabled(l && !l->value("masks", json::array()).empty());
        menu.exec(tree->viewport()->mapToGlobal(pos));
    }

    void saveSmart(bool masks)
    {
        const json* l = layer(selLayer);
        if (!l)
            return;
        bool ok = false;
        QString name = QInputDialog::getText(self, masks ? "Save Smart Mask" : "Save Smart Material", "Name", QLineEdit::Normal,
                                             qs(l->value("name", selLayer)), &ok);
        if (!ok || name.trimmed().isEmpty())
            return;
        json call = args({selLayer});
        call["name"] = name.trimmed().toStdString();
        if (masks)
            call["mask"] = true;
        if (json r = runUi("texSaveSmartMaterial", call); r.is_string())
            log::info("saved {}", r.get<std::string>());
        shelfKey.clear();
        rebuildShelf();
    }
};

TextureSetPanel::TextureSetPanel(QWidget* parent) : QWidget(parent), d(std::make_unique<Impl>())
{
    d->self = this;
    panelList().push_back(this);
    setObjectName("textureSetPanel");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);
    // The two columns need more width than the panel switches back to one at, so the layout must not hold it wide.
    v->setSizeConstraint(QLayout::SetNoConstraint);

    auto* top = new QHBoxLayout();
    d->sets = new QComboBox(this);
    d->sets->setObjectName("setPicker");
    d->sets->setToolTip("The texture set shown");
    d->sets->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    d->sets->setMinimumContentsLength(10);
    d->sets->setPlaceholderText("No texture set");
    d->follow = new QToolButton(this);
    d->follow->setText("Follow");
    d->follow->setToolTip("Follow: show the set of the selected mesh's material");
    d->follow->setCheckable(true);
    d->follow->setChecked(true);
    controls::setRole(d->follow, "push");
    auto* newSet = toolButton(this, "plus", "New Texture Set, with a paint layer, for the selected meshes. The arrow makes one per material.");
    QObject::connect(newSet, &QToolButton::clicked, this, [] { actions::run(json("texture.newSet")); });
    newSet->setPopupMode(QToolButton::MenuButtonPopup);
    auto* newMenu = new QMenu(newSet);
    newMenu->addAction("New Texture Set per Material", newSet, [] { actions::run(json("texture.newSetPerMaterial")); });
    newSet->setMenu(newMenu);
    auto* openGraph = toolButton(this, "node-editor", "Texture Graph: open this set's nodes");
    openGraph->setObjectName("openGraph");
    connect(openGraph, &QToolButton::clicked, this, [this] { openSelectedAsNodes(); });
    d->variants = new QComboBox(this);
    d->variants->setObjectName("variantPicker");
    d->variants->setToolTip("Variant: the set's look shown and edited. A variant overrides layer settings (visibility, opacity, "
                            "materials, seeds) over the base; Export with variants writes them all");
    d->variants->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    d->variants->setMinimumContentsLength(6);
    top->addWidget(d->sets, 1);
    top->addWidget(d->variants);
    top->addWidget(d->follow);
    top->addWidget(openGraph);
    top->addWidget(newSet);
    if (ViewportLayout* views = ViewportLayout::main())
    {
        auto* mode = new controls::Segmented(this);
        mode->setObjectName("viewMode");
        mode->add({}, "box", "3D: the viewport alone", int(ViewportLayout::TextureView::View3D));
        mode->add({}, "layout-columns", "Split: the viewport beside the texture laid out in UV space", int(ViewportLayout::TextureView::Split));
        mode->add({}, "texture", "2D: the texture laid out in UV space, in place of the viewport", int(ViewportLayout::TextureView::View2D));
        mode->setCurrent(int(views->textureView()));
        mode->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        connect(mode, &controls::Segmented::picked, views, [views](int id) { views->setTextureView(ViewportLayout::TextureView(id)); });
        connect(views, &ViewportLayout::textureViewChanged, mode, [mode, views] { mode->setCurrent(int(views->textureView())); });
        top->addSpacing(8);
        top->addWidget(mode);
    }
    v->addLayout(top);

    d->columns = new QBoxLayout(QBoxLayout::TopToBottom);
    d->columns->setContentsMargins(0, 0, 0, 0);
    d->columns->setSpacing(4);
    v->addLayout(d->columns, 1);
    d->side = new QWidget(this);
    d->sideLayout = new QVBoxLayout(d->side);
    d->sideLayout->setContentsMargins(0, 0, 0, 0);
    d->sideLayout->setSpacing(4);
    d->settingsBox = new QWidget(this);
    d->settingsBox->setObjectName("setSettings");
    auto* settingsScroll = new FitScrollArea(d->side);
    settingsScroll->setWidget(controls::section("Channels and Resolution", d->settingsBox, this, "/textureSets/collapsed", false, &d->settingsToggle));
    d->sideLayout->addWidget(settingsScroll);
    d->columns->addWidget(d->side);
    auto* main = new StackColumn(this);
    d->columns->addWidget(main, 1);
    v = new QVBoxLayout(main);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    d->bannerBox = new QWidget(this);
    d->bannerBox->setObjectName("graphModeBanner");
    auto* bh = new QHBoxLayout(d->bannerBox);
    bh->setContentsMargins(0, 4, 0, 4);
    auto* warn = new QLabel(d->bannerBox);
    warn->setPixmap(icon("alert-triangle").pixmap(16, 16));
    bh->addWidget(warn);
    d->bannerText = new QLabel(d->bannerBox);
    d->bannerText->setWordWrap(true);
    bh->addWidget(d->bannerText, 1);
    d->rebuild = new QPushButton(icon("refresh"), "Rebuild Layers", d->bannerBox);
    d->rebuild->setObjectName("rebuildLayers");
    connect(d->rebuild, &QPushButton::clicked, this, [this] { runUi("texRebuildLayers", d->args({})); });
    bh->addWidget(d->rebuild);
    d->bannerBox->hide();
    v->addWidget(d->bannerBox);

    auto* bar = new QHBoxLayout();
    bar->setSpacing(1);
    auto* addFill = toolButton(this, "bucket", "Add Fill Layer: above the selection");
    connect(addFill, &QToolButton::clicked, this, [this] { addLayer("fill"); });
    auto* addPaint = toolButton(this, "brush", "Add Paint Layer: above the selection");
    connect(addPaint, &QToolButton::clicked, this, [this] { addLayer("paint"); });
    auto* addFolder = toolButton(this, "folder-plus", "Add Folder: above the selection");
    connect(addFolder, &QToolButton::clicked, this, [this] { addLayer("folder"); });
    auto* more = toolButton(this, "plus", "Add Layer: any kind");
    more->setPopupMode(QToolButton::InstantPopup);
    auto* moreMenu = new QMenu(more);
    connect(moreMenu, &QMenu::aboutToShow, this, [this, moreMenu] {
        moreMenu->clear();
        d->fillLayerMenu(moreMenu);
    });
    more->setMenu(moreMenu);
    auto* maskBtn = toolButton(this, "mask", "Add Mask: a mask or generator on the selected layer");
    maskBtn->setPopupMode(QToolButton::InstantPopup);
    auto* maskMenu = new QMenu(maskBtn);
    connect(maskMenu, &QMenu::aboutToShow, this, [this, maskMenu] {
        maskMenu->clear();
        d->fillMaskMenu(maskMenu);
    });
    maskBtn->setMenu(maskMenu);
    auto* dup = toolButton(this, "copy", "Duplicate: the selected layer");
    connect(dup, &QToolButton::clicked, this, [this] { duplicateSelected(); });
    auto* del = toolButton(this, "trash", "Delete: the selected layer or mask (Delete)");
    connect(del, &QToolButton::clicked, this, [this] { deleteSelected(); });
    d->channel = new QComboBox(this);
    d->channel->setObjectName("rowChannel");
    d->channel->setToolTip("Channel: the one the rows' thumbnails, blend modes and opacities show and edit");
    d->channel->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    d->channel->setMinimumContentsLength(6);
    connect(d->channel, &QComboBox::currentIndexChanged, this, [this](int) { refresh(); });
    setComboDefault(d->channel, 0);
    for (QToolButton* b : {addFill, addPaint, addFolder, more})
        bar->addWidget(b);
    bar->addSpacing(8);
    bar->addWidget(maskBtn);
    bar->addSpacing(8);
    bar->addWidget(dup);
    bar->addWidget(del);
    bar->addStretch(1);
    bar->addWidget(d->channel);
    v->addLayout(bar);

    d->empty = new QWidget(this);
    auto* eh = new QVBoxLayout(d->empty);
    eh->setContentsMargins(0, 4, 0, 4);
    d->emptyText = dim(d->empty, "No texture set. Click New Texture Set to make one for the selected meshes.");
    eh->addWidget(d->emptyText);
    eh->addWidget(controls::split("texture.newSet", {"texture.newSetPerMaterial"}, d->empty));
    v->addWidget(d->empty);

    auto* split = new QSplitter(Qt::Vertical, this);
    d->tree = new LayerTree(split);
    d->tree->setObjectName("layerTree");
    deletion::claim(d->tree, [this] { deleteSelected(); });
    auto* lowerBox = new QWidget(split);
    auto* lv = new QVBoxLayout(lowerBox);
    lv->setContentsMargins(0, 4, 0, 0);
    lv->setSpacing(4);
    d->lowerKind = new controls::Segmented(lowerBox);
    d->lowerKind->setObjectName("lowerPage");
    d->lowerKind->add("Layer", {}, "Layer: the selected layer's or mask's properties", 0);
    d->lowerKind->add("Brush", {}, "Brush: the Texture Paint settings", 1);
    lv->addWidget(d->lowerKind, 0, Qt::AlignLeft);
    d->lower = new QStackedWidget(lowerBox);
    d->propsScroll = new QScrollArea(d->lower);
    d->propsScroll->setWidgetResizable(true);
    d->propsScroll->setFrameShape(QFrame::NoFrame);
    d->lower->addWidget(d->propsScroll);
    auto* brushScroll = new QScrollArea(d->lower);
    brushScroll->setObjectName("brushPage");
    brushScroll->setWidgetResizable(true);
    brushScroll->setFrameShape(QFrame::NoFrame);
    brushScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    brushScroll->setWidget(texturePaintPage(brushScroll));
    d->lower->addWidget(brushScroll);
    lv->addWidget(d->lower, 1);
    d->setLowerPage(0);
    connect(d->lowerKind, &controls::Segmented::picked, this, [this](int page) { d->setLowerPage(page); });
    d->split = split;
    auto* shelfBox = d->shelfBox = new QWidget(split);
    auto* sv = new QVBoxLayout(shelfBox);
    sv->setContentsMargins(0, 2, 0, 0);
    sv->setSpacing(2);
    auto* shelfHead = new QHBoxLayout();
    d->shelfKind = new controls::Segmented(shelfBox);
    d->shelfKind->setObjectName("shelfKind");
    d->shelfKind->add({}, "sparkles", "Materials: smart materials, saved layers applied above the selected layer", kShelfMaterials);
    d->shelfKind->add({}, "mask", "Masks: smart masks, saved mask stacks added to the selected layer", kShelfMasks);
    d->shelfKind->add({}, "brush", "Brushes: presets; click one to paint with it", kShelfBrushes);
    d->shelfKind->add({}, "droplet-half", "Alphas: the brush tip's shape, shared with sculpting", kShelfAlphas);
    d->shelfKind->add({}, "photo", "Textures: your images; click one to paint through it", kShelfTextures);
    d->shelfKind->setCurrent(kShelfMaterials);
    connect(d->shelfKind, &controls::Segmented::picked, this, [this](int) { d->filterShelf(); });
    shelfHead->addWidget(d->shelfKind);
    shelfHead->addStretch(1);
    sv->addLayout(shelfHead);
    auto* searchRow = new QHBoxLayout();
    searchRow->setSpacing(2);
    d->shelfSearch = new QLineEdit(shelfBox);
    d->shelfSearch->setObjectName("shelfSearch");
    d->shelfSearch->setPlaceholderText("Search");
    d->shelfSearch->setClearButtonEnabled(true);
    d->shelfSearch->addAction(icon("search"), QLineEdit::LeadingPosition);
    connect(d->shelfSearch, &QLineEdit::textChanged, this, [this] { d->filterShelf(); });
    searchRow->addWidget(d->shelfSearch, 1);
    d->shelfTags = new ShelfTagFilter(shelfBox);
    d->shelfTags->onChanged = [this] { d->filterShelf(); };
    searchRow->addWidget(d->shelfTags->button());
    d->shelfAdd = toolButton(shelfBox, "plus", "Add Textures: copy images into your textures folder (or drop image files on the shelf)");
    d->shelfAdd->setObjectName("addTextures");
    connect(d->shelfAdd, &QToolButton::clicked, this, [this] {
        const QStringList files = ui::fileDialog::getOpenFileNames(this, "Add Textures", {}, "Images (*.png *.jpg *.jpeg *.tga *.tif *.tiff *.exr *.bmp *.psd *.hdr)");
        if (!files.isEmpty())
            addTextures(files);
    });
    searchRow->addWidget(d->shelfAdd);
    auto* reload = toolButton(shelfBox, "refresh", "Reload: reread the libraries");
    connect(reload, &QToolButton::clicked, this, [this] {
        d->shelfKey.clear();
        d->rebuildShelf();
    });
    searchRow->addWidget(reload);
    sv->addLayout(searchRow);
    sv->addWidget(d->shelfTags->chips());
    d->shelf = new SmartShelf(shelfBox);
    d->shelf->setObjectName("smartShelf");
    d->shelf->setViewMode(QListView::IconMode);
    d->shelf->setFlow(QListView::LeftToRight);
    d->shelf->setWrapping(true);
    d->shelf->setResizeMode(QListView::Adjust);
    d->shelf->setIconSize(QSize(64, 64));
    // Two lines of name under each tile, so names like "Stylized Dirt Path" read whole.
    d->shelf->setGridSize(QSize(96, 104));
    d->shelf->setWordWrap(true);
    d->shelf->setTextElideMode(Qt::ElideRight);
    d->shelf->setMovement(QListView::Static);
    // After setMovement: Static switches dragging off.
    d->shelf->setDragEnabled(true);
    d->shelf->setDragDropMode(QAbstractItemView::DragOnly);
    connect(d->shelf, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) { d->pickShelfItem(it, true); });
    connect(d->shelf, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { d->pickShelfItem(it, false); });
    static_cast<SmartShelf*>(d->shelf)->onFiles = [this](const QStringList& files) {
        QStringList painter, images;
        for (const QString& f : files)
            (isPainterSmartMaterial(f) ? painter : images) << f;
        if (!images.isEmpty())
            addTextures(images);
        if (const PainterImported made = importPainterSmartMaterials(painter); made.total() > 0)
        {
            if (made.brushes)
                tex::brushes::reload();
            d->shelfKind->setCurrent(made.materials ? kShelfMaterials : made.masks ? kShelfMasks : kShelfBrushes);
            d->shelfKey.clear();
            d->rebuildShelf();
        }
    };
    favourites::attach(
        d->shelf,
        [](const QModelIndex& i) { return Impl::shelfFavourite(i.data(Qt::UserRole + 1).toString(), i.data(Qt::UserRole).toString()); },
        [this] {
            d->favouritesFirst();
            d->filterShelf();
        });
    d->shelf->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(d->shelf, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        QListWidgetItem* it = d->shelf->itemAt(p);
        const QString kind = it ? it->data(Qt::UserRole + 1).toString() : QString();
        if (kind != "material" && kind != "mask")
            return;
        QStringList known;
        for (int i = 0; i < d->shelf->count(); ++i)
            if (const QString k = d->shelf->item(i)->data(Qt::UserRole + 1).toString(); k == "material" || k == "mask")
                for (const QString& t : d->shelf->item(i)->data(Qt::UserRole + 3).toStringList())
                    if (!known.contains(t))
                        known << t;
        QMenu menu(d->shelf);
        addSmartTagMenu(&menu, it->data(Qt::UserRole).toString(), it->data(Qt::UserRole + 3).toStringList(), known, [this] {
            d->shelfKey.clear();
            d->rebuildShelf();
        });
        menu.exec(d->shelf->viewport()->mapToGlobal(p));
    });
    d->shelf->setAcceptDrops(true);
    sv->addWidget(d->shelf, 1);
    deletion::claim(d->shelf, nullptr);
    d->shelfHint = new QLabel("Select a layer to add a mask to.", shelfBox);
    d->shelfHint->setStyleSheet(themedStyleSheet("color: #969696;"));
    d->shelfHint->hide();
    sv->addWidget(d->shelfHint);
    auto* apply = d->shelfApply = new QPushButton("Apply", shelfBox);
    apply->setToolTip("Apply the chosen smart material above the selected layer, or add the chosen smart mask to it. Or drag one onto a mesh "
                      "or into the layers.");
    connect(apply, &QPushButton::clicked, this, [this] {
        if (QListWidgetItem* it = d->shelf->currentItem())
            applySmartMaterial(it->data(Qt::UserRole).toString().toStdString());
    });
    sv->addWidget(apply);
    split->addWidget(d->tree);
    split->addWidget(lowerBox);
    split->addWidget(shelfBox);
    // Layers about a quarter, the Layer and Brush pane under half, the library the rest; growing keeps those shares.
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 5);
    split->setStretchFactor(2, 3);
    split->setSizes({260, 450, 290});
    v->addWidget(split, 1);

    d->tree->onMove = [this](QTreeWidgetItem* dragged, QTreeWidgetItem* target, Drop where) {
        const bool draggedMask = dragged->data(0, kRoleKind).toInt() == 1;
        const std::string id = dragged->data(0, kRoleId).toString().toStdString();
        if (draggedMask)
        {
            if (!target || target->data(0, kRoleKind).toInt() != 1 ||
                target->data(0, kRoleLayer).toString() != dragged->data(0, kRoleLayer).toString())
                return;
            d->selLayer = dragged->data(0, kRoleLayer).toString().toStdString();
            moveLayer(id, target->data(0, kRoleId).toString().toStdString(), where);
            return;
        }
        // Dropped on a mask row: the mask's layer is the target.
        std::string targetId = target ? target->data(0, kRoleLayer).toString().toStdString() : std::string();
        moveLayer(id, targetId, where);
    };
    d->tree->onSmart = [this](const std::string& name, const std::string& kind, QTreeWidgetItem* target, Drop where) {
        const std::string layerId = target ? target->data(0, kRoleLayer).toString().toStdString() : std::string();
        if (kind == "mask")
        {
            if (layerId.empty())
            {
                log::warn("Smart mask {}: drop it onto a layer; the mask goes on that layer", name);
                return;
            }
            d->selLayer = layerId;
            d->selMask.clear();
            applySmartMaterial(name);
            return;
        }
        applySmartMaterial(name);
        if (!layerId.empty() && !d->selLayer.empty() && d->selLayer != layerId)
        {
            json move = d->args({d->selLayer});
            if (where == Drop::Into)
                move["parent"] = layerId;
            else
                move[where == Drop::Above ? "above" : "below"] = layerId;
            runUi("texMoveLayer", move);
            refresh();
        }
    };
    d->tree->onMaterial = [this](const std::string& material, QTreeWidgetItem* target, Drop where) {
        dropMaterial(material, target ? target->data(0, kRoleLayer).toString().toStdString() : std::string(), where);
    };
    connect(d->tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it, QTreeWidgetItem*) { d->onCurrentItem(it); });
    connect(d->tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& p) { d->contextMenu(p); });
    connect(d->tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* it) {
        if (!d->building)
            d->expanded[it->data(0, kRoleId).toString().toStdString()] = true;
    });
    connect(d->tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* it) {
        if (!d->building)
            d->expanded[it->data(0, kRoleId).toString().toStdString()] = false;
    });
    connect(d->sets, &QComboBox::activated, this, [this](int) { setTextureSet(d->sets->currentData().toString().toStdString()); });
    d->sets->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(d->sets, &QWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        if (d->set.empty())
            return;
        QMenu menu(this);
        menu.addAction(icon("pencil"), "Rename", this, [this] { startRename(); });
        menu.addAction(icon("trash"), "Delete", this, [set = d->set] { deleteTextureSet(set); });
        menu.exec(d->sets->mapToGlobal(at));
    });
    connect(d->variants, &QComboBox::activated, this, [this](int) {
        const QString data = d->variants->currentData().toString();
        const std::string set = d->set;
        json call;
        if (data == QString("\x01new"))
        {
            bool ok = false;
            const QString name = QInputDialog::getText(this, "New Variant", "Name", QLineEdit::Normal, "Variant", &ok).trimmed();
            if (ok && !name.isEmpty())
                call = {{"command", "texAddVariant"}, {"args", {{"args", {set, name.toStdString()}}}}};
        }
        else if (data == QString("\x01remove"))
            call = {{"command", "texRemoveVariant"},
                    {"args", {{"args", {set, d->view.is_object() ? d->view.value("variant", std::string()) : std::string()}}}}};
        else
            call = {{"command", "texSwitchVariant"}, {"args", {{"args", {set, data.toStdString()}}}}};
        if (!call.is_null())
            actions::run(call);
        d->refresh();
    });

    d->refreshTimer = new QTimer(this);
    d->refreshTimer->setSingleShot(true);
    d->refreshTimer->setInterval(0);
    connect(d->refreshTimer, &QTimer::timeout, this, [this] { d->refresh(); });

    QPointer<TextureSetPanel> guard(this);
    d->sceneListener = scene::Graph::current().addListener([this, guard](scene::Change c, scene::NodeId id) {
        if (!guard)
            return;
        if (c == scene::Change::Selection || c == scene::Change::Reset)
        {
            QTimer::singleShot(0, this, [this] {
                if (!d->follow->isChecked())
                    return;
                if (GraphBackendPtr b = graphBackends::find(kTextureGraphBackend))
                {
                    scene::Graph& g = scene::Graph::current();
                    GraphBackend::Owner o = b->followSelection(g);
                    if (!o.group.empty() && (o.group != d->set || d->noSetForSelection))
                    {
                        d->set = o.group;
                        d->noSetForSelection = false;
                        showInTextureGraphs(o.group);
                        d->selLayer.clear();
                        d->selMask.clear();
                        d->scheduleRefresh();
                    }
                    // Meshes picked whose materials have no set: show that, not the set picked before.
                    else if (o.group.empty() && selectsMeshes(g) && !d->noSetForSelection)
                    {
                        d->set.clear();
                        d->noSetForSelection = true;
                        d->selLayer.clear();
                        d->selMask.clear();
                        d->scheduleRefresh();
                    }
                }
            });
            if (c == scene::Change::Selection)
                return;
        }
        if (c == scene::Change::Value)
        {
            scene::Node* n = scene::Graph::current().node(id);
            if (!n || n->type().name != tex::kTextureSetType)
                return;
        }
        d->scheduleRefresh();
    });
    d->activeListener = textureSets::addActiveLayerListener([this](const std::string& set, const std::string& layer) {
        if (set != d->set)
            return;
        if (d->selLayer != layer || !d->selMask.empty())
        {
            d->selLayer = layer;
            d->selMask.clear();
        }
        d->scheduleRefresh();
    });

    // A Texture Graph editor switching sets takes this panel along.
    d->graphListener = NodeEditor::addGroupListener([this, guard](NodeEditor& e) {
        if (!guard || e.backend().id() != kTextureGraphBackend)
            return;
        const std::string group = e.group().toStdString();
        if (group.empty() || group == d->set)
            return;
        d->set = group;
        d->noSetForSelection = false;
        d->selLayer.clear();
        d->selMask.clear();
        d->scheduleRefresh();
    });

    QTimer::singleShot(0, this, [this] {
        if (GraphBackendPtr b = graphBackends::find(kTextureGraphBackend))
            if (GraphBackend::Owner o = b->followSelection(scene::Graph::current()); !o.group.empty())
                d->set = o.group;
        if (d->set.empty())
            d->set = textureGraphSet();
        d->refresh();
        showInTextureGraphs(d->set);
    });
    d->refresh();
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (now && isAncestorOf(now))
            lastFocusedPanel() = this;
    });
}

QSize TextureSetPanel::minimumSizeHint() const { return {240, 160}; }

void TextureSetPanel::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    d->setWide(width() >= kWideWidth);
}

TextureSetPanel::~TextureSetPanel()
{
    scene::Graph::current().removeListener(d->sceneListener);
    textureSets::removeActiveLayerListener(d->activeListener);
    NodeEditor::removeGroupListener(d->graphListener);
    std::erase(panelList(), this);
    if (lastFocusedPanel() == this)
        lastFocusedPanel() = nullptr;
}

void TextureSetPanel::startRename()
{
    if (d->set.empty())
        return;
    controls::editInPlace(d->sets, QString::fromStdString(d->set), [this, from = d->set](const QString& to) {
        std::string renamed = renameTextureSet(from, to.toStdString());
        if (!renamed.empty())
            setTextureSet(renamed);
    });
}

bool startSetRename()
{
    for (TextureSetPanel* p : panelList())
        if (p->isVisible())
        {
            p->startRename();
            return true;
        }
    return false;
}

void TextureSetPanel::setTextureSet(const std::string& set)
{
    if (set == d->set)
        return;
    d->set = set;
    d->noSetForSelection = false;
    d->selLayer.clear();
    d->selMask.clear();
    d->refresh();
    showInTextureGraphs(set);
}

std::string TextureSetPanel::textureSet() const { return d->set; }

void TextureSetPanel::refresh()
{
    LILA_ZONE();
    d->refreshTimer->stop();
    d->refresh();
}

std::string TextureSetPanel::selectedLayer() const { return d->selLayer; }
std::string TextureSetPanel::selectedMask() const { return d->selMask; }

void TextureSetPanel::selectLayer(const std::string& id)
{
    for (QTreeWidgetItem* it : allItems(d->tree))
        if (it->data(0, kRoleKind).toInt() == 0 && it->data(0, kRoleId).toString().toStdString() == id)
        {
            d->tree->setCurrentItem(it);
            return;
        }
}

void TextureSetPanel::selectMask(const std::string& layer, const std::string& mask)
{
    d->expanded[layer] = true;
    for (QTreeWidgetItem* it : allItems(d->tree))
        if (it->data(0, kRoleKind).toInt() == 1 && it->data(0, kRoleId).toString().toStdString() == mask &&
            it->data(0, kRoleLayer).toString().toStdString() == layer)
        {
            if (it->parent())
                it->parent()->setExpanded(true);
            d->tree->setCurrentItem(it);
            return;
        }
}

std::string TextureSetPanel::addLayer(const std::string& kind, const std::string& filter) { return d->addLayer(kind, filter); }

std::string TextureSetPanel::addMask(const std::string& kind, const std::string& type) { return d->addMask(kind, type); }

void TextureSetPanel::addIdLayers() { d->addIdLayers(); }

void TextureSetPanel::moveLayer(const std::string& id, const std::string& target, Drop where)
{
    if (!d->setNode() || id == target)
        return;
    // A mask among its layer's masks: the rows are top first, the stack bottom first.
    if (const json* m = d->mask(d->selLayer, id))
    {
        json masks = d->layer(d->selLayer)->value("masks", json::array());
        json moved = *m;
        masks.erase(std::remove_if(masks.begin(), masks.end(), [&](const json& x) { return x.value("id", "") == id; }), masks.end());
        auto at = std::find_if(masks.begin(), masks.end(), [&](const json& x) { return x.value("id", "") == target; });
        if (at == masks.end())
            return;
        masks.insert(where == Drop::Below ? at : at + 1, moved);
        d->setLayer(d->selLayer, {{"masks", masks}});
        return;
    }
    json call = d->args({id});
    if (target.empty())
        call["index"] = 0;
    else if (where == Drop::Into)
        call["parent"] = target;
    else
        call[where == Drop::Above ? "above" : "below"] = target;
    runUi("texMoveLayer", call);
    d->selLayer = id;
    d->selMask.clear();
    refresh();
}

void TextureSetPanel::dropMaterial(const std::string& material, const std::string& target, Drop where)
{
    if (!d->setNode() || material.empty())
        return;
    UndoChunk chunk("Add Material Layer");
    json call = {{"args", json::array({material})}, {"set", d->set}, {"layer", true}};
    if (std::string p = chosenPreset(material); !p.empty())
        call["preset"] = p;
    json r = runUi("applyLibraryMaterial", call);
    std::string id = r.is_object() ? r.value("layer", json::object()).value("id", std::string()) : std::string();
    if (id.empty())
        return;
    if (!target.empty())
    {
        json move = d->args({id});
        if (where == Drop::Into)
            move["parent"] = target;
        else
            move[where == Drop::Above ? "above" : "below"] = target;
        runUi("texMoveLayer", move);
    }
    d->selLayer = id;
    d->selMask.clear();
    refresh();
}

void TextureSetPanel::copySelected()
{
    if (!d->setNode() || !d->layer(d->selLayer))
        return;
    if (json r = runUi("texCopyLayers", d->args({d->selLayer})); r.is_string())
        QApplication::clipboard()->setText(QString::fromStdString(r.get<std::string>()));
}

void TextureSetPanel::paste()
{
    if (!d->setNode())
        return;
    json call = d->args({QApplication::clipboard()->text().toStdString()});
    if (d->layer(d->selLayer))
        call["above"] = d->selLayer;
    json r = runUi("texPasteLayers", call);
    if (r.is_object() && !r["layers"].empty())
        d->selLayer = r["layers"].back().get<std::string>();
    refresh();
}

void TextureSetPanel::flattenSelected(bool down)
{
    if (!d->setNode() || !d->layer(d->selLayer))
        return;
    json call = d->args({d->selLayer});
    call["down"] = down;
    json r = runUi("texFlattenLayers", call);
    if (r.is_object())
    {
        d->selLayer = r.value("layer", std::string());
        for (const json& w : r.value("warnings", json::array()))
            log::warn("{}: {}", down ? "Merge Down" : "Flatten Layer", w.get<std::string>());
    }
    refresh();
}

void TextureSetPanel::exportFlattened()
{
    if (!d->setNode() || !d->layer(d->selLayer))
        return;
    const QString dir = ui::fileDialog::getExistingDirectory(this, "Export Flattened");
    if (dir.isEmpty())
        return;
    json call = d->args({d->selLayer});
    call["folder"] = dir.toStdString();
    if (json r = runUi("texExportFlattened", call); r.is_object())
        log::info("exported {} images", r.value("files", json::object()).size());
}

void TextureSetPanel::duplicateSelected()
{
    scene::Node* n = d->setNode();
    const json* l = d->layer(d->selLayer);
    if (!n || !l || !d->selMask.empty())
        return;
    // A layer saved and applied as a smart material comes back with fresh layer and mask ids and unique anchor names.
    tex::SmartMaterial sm;
    sm.name = l->value("name", d->selLayer) + " copy";
    json copy = *l;
    copy.erase("resolved");
    std::function<void(json&)> strip = [&](json& x) {
        x.erase("resolved");
        if (x.contains("children"))
            for (json& c : x["children"])
                strip(c);
    };
    strip(copy);
    copy["name"] = sm.name;
    sm.layer = copy;
    const std::filesystem::path dir = paths::fromUtf8(QDir::temp().filePath("lila_layer_copies").toStdString());
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / (d->set + "_" + d->selLayer + ".lilasmart");
    try
    {
        tex::saveSmartMaterial(sm, file);
    }
    catch (const std::exception& e)
    {
        log::warn("Duplicate layer: {}", e.what());
        return;
    }
    json call = d->args({paths::toUtf8(file)});
    call["above"] = d->selLayer;
    call["addMaps"] = false;
    call["name"] = sm.name;
    json r = runUi("texApplySmartMaterial", call);
    std::filesystem::remove(file, ec);
    if (r.is_object() && r.contains("layer"))
    {
        d->selLayer = r["layer"].get<std::string>();
        d->selMask.clear();
    }
    refresh();
}

void TextureSetPanel::deleteSelected()
{
    if (!d->setNode() || !d->layer(d->selLayer))
        return;
    if (!d->selMask.empty())
        runUi("texRemoveMask", d->args({d->selLayer, d->selMask}));
    else
        runUi("texRemoveLayer", d->args({d->selLayer}));
    d->selMask.clear();
    d->selLayer.clear();
    refresh();
}

void TextureSetPanel::openSelectedAsNodes()
{
    if (!d->setNode())
        return;
    panels::show(kTextureGraphDock);
    for (QWidget* w : QApplication::allWidgets())
        if (auto* e = dynamic_cast<NodeEditor*>(w); e && e->backend().id() == kTextureGraphBackend)
        {
            if (d->layer(d->selLayer))
                e->showNode(qs(d->set), d->selLayer);
            else
                e->setGroup(qs(d->set));
        }
}

void TextureSetPanel::applySmartMaterial(const std::string& name)
{
    if (!d->setNode() || name.empty())
        return;
    bool mask = false;
    for (int i = 0; i < d->shelf->count(); ++i)
        if (d->shelf->item(i)->data(Qt::UserRole).toString().toStdString() == name)
            mask = d->shelf->item(i)->data(Qt::UserRole + 1).toString() == "mask";
    json call = d->args({name});
    if (mask)
    {
        if (!d->layer(d->selLayer))
        {
            log::warn("Smart mask {}: select a layer first; the mask goes on the selected layer", name);
            return;
        }
        call["layer"] = d->selLayer;
    }
    else
        call.update(d->placement());
    json r = runUi("texApplySmartMaterial", call);
    if (r.is_object() && r.contains("layer"))
    {
        d->selLayer = r["layer"].get<std::string>();
        d->selMask.clear();
    }
    else if (mask)
        d->expanded[d->selLayer] = true;
    refresh();
}

QTreeWidget* TextureSetPanel::tree() const { return d->tree; }
QComboBox* TextureSetPanel::setPicker() const { return d->sets; }
QListWidget* TextureSetPanel::smartShelf() const { return d->shelf; }
QWidget* TextureSetPanel::properties() const { return d->props; }
QWidget* TextureSetPanel::banner() const { return d->bannerBox; }

void TextureSetPanel::showBrushPage(bool on)
{
    d->setLowerPage(on ? 1 : 0);
}

void TextureSetPanel::showLibrary(const std::string& kind)
{
    for (int k = 0; k < int(std::size(kShelfKindNames)); ++k)
        if (kind == kShelfKindNames[k] || kind == std::string(kShelfKindNames[k]) + (k == kShelfBrushes ? "es" : "s"))
            d->setShelfKind(k);
}

void TextureSetPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (std::exchange(d->shelfStale, false))
        d->rebuildShelf();
}

void TextureSetPanel::refreshLibrary()
{
    d->shelfKey.clear();
    d->rebuildShelf();
}

void TextureSetPanel::addTextures(const QStringList& files)
{
    const std::vector<std::filesystem::path> added = importTextures(files);
    refreshLibrary();
    d->setShelfKind(kShelfTextures);
    if (added.size() == 1)
        setTexturePaintTexture(paths::toUtf8(added.front()));
}

TextureSetPanel* TextureSetPanel::active()
{
    QWidget* focus = QApplication::focusWidget();
    for (TextureSetPanel* p : panelList())
        if (focus && p->isAncestorOf(focus))
            return p;
    if (lastFocusedPanel())
        return lastFocusedPanel();
    for (TextureSetPanel* p : panelList())
        if (p->isVisible())
            return p;
    return panelList().empty() ? nullptr : panelList().front();
}

// Active layer API -------------------------------------------------------------------------------------------------------

namespace textureSets
{
void showBrushPage(bool on)
{
    // Painting needs its settings on screen, so the dock comes up even when it was tucked away.
    if (on)
        panels::show(kTextureSetsDock);
    for (TextureSetPanel* p : panelList())
        p->showBrushPage(on);
}

void showLibrary(const std::string& kind)
{
    panels::show(kTextureSetsDock);
    for (TextureSetPanel* p : panelList())
        p->showLibrary(kind);
}

void refreshLibrary()
{
    for (TextureSetPanel* p : panelList())
        p->refreshLibrary();
}

std::string activeLayer(const std::string& set)
{
    auto it = activeState().layers.find(set);
    if (it == activeState().layers.end() || it->second.empty())
        return {};
    std::optional<tex::StackState> st = stackOf(set);
    if (!st || st->graphMode)
        return {};
    const tex::Layer* l = st->stack.find(it->second);
    return l && l->kind == tex::LayerKind::Paint ? it->second : std::string();
}

void setActiveLayer(const std::string& set, const std::string& layer)
{
    std::string& cur = activeState().layers[set];
    if (cur == layer)
        return;
    cur = layer;
    auto listeners = activeState().listeners;
    for (auto& [id, fn] : listeners)
        fn(set, layer);
}

int addActiveLayerListener(std::function<void(const std::string&, const std::string&)> fn)
{
    const int id = activeState().next++;
    activeState().listeners[id] = std::move(fn);
    return id;
}

void removeActiveLayerListener(int id) { activeState().listeners.erase(id); }
} // namespace textureSets

// Registration -----------------------------------------------------------------------------------------------------------

namespace
{
/** The material slot of the face under a drop, else the mesh's first. */
std::string slotUnder(const geo::Mesh& mesh, uint32_t face)
{
    std::span<const int32_t> slots = mesh.faceAttrs().get<int32_t>(geo::attr::kMaterial);
    return scene::materials::slotName(mesh, face < slots.size() ? slots[face] : 0);
}

/** The texture set driving the slot; a new one when there is none, with a material of its own for a mesh on Default. */
std::string setForDrop(scene::Node& meshNode, std::string slot)
{
    scene::Graph& g = scene::Graph::current();
    if (std::string set = textureSetForSlot(g, slot); !set.empty())
        return set;
    if (slot == scene::materials::kDefaultSlot)
    {
        json made = runUi("createMaterial", {{"args", {"M_" + meshNode.name()}}});
        if (!made.is_string())
            return {};
        slot = made.get<std::string>();
        runUi("assignMaterial", {{"args", {slot, meshNode.path()}}});
    }
    std::string name = tex::defaultSetName(slot);
    for (int i = 1; !g.findAll(name).empty(); ++i)
        name = tex::defaultSetName(slot) + std::to_string(i);
    json r = runUi("texCreateSet", {{"args", {name}}, {"materials", {slot}}});
    if (!r.is_string())
        return {};
    log::info("{}: new texture set {} for material {}", meshNode.name(), r.get<std::string>(), slot);
    return r.get<std::string>();
}

/** A smart material dropped on a mesh lands on top of the texture set of the material under the cursor. */
bool dropSmartOnViewport(const QMimeData& mime, const viewportHooks::DropTarget& target, bool commit)
{
    if (!mime.hasFormat(kSmartMime))
        return false;
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = target.node == scene::kNoNode ? nullptr : g.node(target.node);
    // Drag-enter asks with no object under the cursor; refusing there rejects the whole drag.
    if (!commit)
        return !n || n->type().isA("mesh");
    if (!n || !n->type().isA("mesh"))
        return false;
    g.evaluate();
    scene::MeshPtr mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    if (!mesh)
        return false;
    auto [name, kind] = smartFromMime(mime);
    UndoChunk chunk("Apply Smart Material");
    const std::string set = setForDrop(*n, slotUnder(*mesh, target.face));
    if (set.empty())
        return true;
    TextureSetPanel* panel = TextureSetPanel::active();
    if (panel)
        panel->setTextureSet(set);
    if (kind == "mask")
    {
        log::warn("Smart mask {}: drop it onto a layer in the Texture Set panel; masks go on a layer, not a mesh", name);
        return true;
    }
    if (panel)
        panel->applySmartMaterial(name);
    else
        runUi("texApplySmartMaterial", {{"args", {set, name}}});
    return true;
}
} // namespace

void registerTextureSetPanel()
{
    viewportHooks::setDropHandler("textureSets.smart", dropSmartOnViewport);
    static bool done = false;
    if (std::exchange(done, true))
        return;
    panels::add({.id = kTextureSetsDock,
                 .title = "Texture Set",
                 .create = [](QWidget* p) -> QWidget* { return new TextureSetPanel(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "stack-2",
                 .open = false,
                 .toggleAction = "window.textureSets"});

    auto onPanel = [](auto fn) {
        return [fn] {
            if (TextureSetPanel* p = TextureSetPanel::active())
                fn(*p);
            else
                log::warn("Open the Texture Set panel (Texture > Texture Set) first");
        };
    };
    actions::add({.id = "textureSets.addFillLayer",
                  .label = "Add Fill Layer",
                  .category = "Texture",
                  .run = onPanel([](TextureSetPanel& p) { p.addLayer("fill"); }),
                  .doc = "Add a fill layer (a material and a mask) above the layer selected in the Texture Set panel.",
                  .python = "lila.ops.texAddLayer(\"RockTex\", \"fill\")"});
    actions::add({.id = "textureSets.addPaintLayer",
                  .label = "Add Paint Layer",
                  .category = "Texture",
                  .run = onPanel([](TextureSetPanel& p) { p.addLayer("paint"); }),
                  .doc = "Add a paint layer above the selection in the Texture Set panel and make it the one painting goes into.",
                  .python = "lila.ops.texAddLayer(\"RockTex\", \"paint\")"});
    actions::add({.id = "textureSets.addFolder",
                  .label = "Add Layer Folder",
                  .category = "Texture",
                  .run = onPanel([](TextureSetPanel& p) { p.addLayer("folder"); }),
                  .doc = "Add a folder above the selection in the Texture Set panel; drop layers onto it to put them inside.",
                  .python = "lila.ops.texAddLayer(\"RockTex\", \"folder\")"});
    actions::add({.id = "textureSets.addIdLayers",
                  .label = "Add Layers per ID",
                  .category = "Texture",
                  .run = onPanel([](TextureSetPanel& p) { p.addIdLayers(); }),
                  .doc = "Add a fill layer per material, object or group in the set's baked ID map, each masked to its own ID.",
                  .python = "lila.ops.texAddIdLayers(\"RockTex\")",
                  .keywords = {"id mask", "color selection", "auto mask", "material id"}});
    actions::add({.id = "textureSets.addMask",
                  .label = "Add Black Mask",
                  .category = "Texture",
                  .run = onPanel([](TextureSetPanel& p) { p.addMask("black"); }),
                  .doc = "Add a black fill mask to the selected layer, hiding it until generators or paint reveal it.",
                  .python = "lila.ops.texAddMask(\"RockTex\", \"L1\", \"fill\", value=0)"});
    actions::add({.id = "textureSets.duplicate",
                  .label = "Duplicate Layer",
                  .category = "Texture",
                  .run = onPanel([](TextureSetPanel& p) { p.duplicateSelected(); }),
                  .doc = "Copy the selected layer (with its masks, or a folder with its contents) right above it.",
                  .python = "lila.ops.texSaveSmartMaterial(\"RockTex\", \"L1\", \"copy.lilasmart\")\n"
                            "lila.ops.texApplySmartMaterial(\"RockTex\", \"copy.lilasmart\", above=\"L1\")"});
    actions::add({.id = "textureSets.copyLayer",
                  .label = "Copy Layer",
                  .category = "Texture",
                  .run = onPanel([](TextureSetPanel& p) { p.copySelected(); }),
                  .doc = "Copy the selected layer (masks, children and paint) to the clipboard, to paste into any texture set.",
                  .python = "text = texCopyLayers(\"RockTex\", \"L1\")",
                  .keywords = {"copy layer", "clipboard"}});
    actions::add({.id = "textureSets.pasteLayers",
                  .label = "Paste Layers",
                  .category = "Texture",
                  .run = onPanel([](TextureSetPanel& p) { p.paste(); }),
                  .doc = "Paste copied layers above the selection, with fresh ids and their own copies of the paint.",
                  .python = "texPasteLayers(\"RockTex\", text)",
                  .keywords = {"paste layer", "clipboard"}});
    actions::add({.id = "textureSets.mergeDown",
                  .label = "Merge Layer Down",
                  .category = "Texture",
                  .run = onPanel([](TextureSetPanel& p) { p.flattenSelected(true); }),
                  .doc = "Bake the selected layer and the one below it into one paint layer that shows the same.",
                  .python = "texFlattenLayers(\"RockTex\", \"L2\", down=True)",
                  .keywords = {"merge down", "combine layers"}});
    actions::add({.id = "textureSets.flatten",
                  .label = "Flatten Layer",
                  .category = "Texture",
                  .run = onPanel([](TextureSetPanel& p) { p.flattenSelected(false); }),
                  .doc = "Bake the selected layer or folder (with what is below it) into one paint layer that shows the same.",
                  .python = "texFlattenLayers(\"RockTex\", layers=[\"L1\", \"L2\"])",
                  .keywords = {"flatten", "bake layers", "rasterize"}});
    actions::add({.id = "textureSets.delete",
                  .label = "Delete Layer or Mask",
                  .category = "Texture",
                  .run = onPanel([](TextureSetPanel& p) { p.deleteSelected(); }),
                  .doc = "Remove the layer or mask selected in the Texture Set panel.",
                  .python = "lila.ops.texRemoveLayer(\"RockTex\", \"L1\")"});
    actions::add({.id = "textureSets.openAsNodes",
                  .label = "Open Layer as Nodes",
                  .category = "Texture",
                  .run = onPanel([](TextureSetPanel& p) { p.openSelectedAsNodes(); }),
                  .doc = "Show the selected layer's node in the Texture Graph editor: the stack is the set's graph, each layer a group node.",
                  .python = "lila.ops.texGraphInfo(\"RockTex\")"});
    actions::add({.id = "textureSets.rebuildLayers",
                  .label = "Rebuild Layers",
                  .category = "Texture",
                  .run =
                      [] {
                          std::string set = TextureSetPanel::active() ? TextureSetPanel::active()->textureSet() : currentTextureSet();
                          if (!set.empty())
                              runUi("texRebuildLayers", {{"args", json::array({set})}});
                      },
                  .doc = "Rebuild the texture set's graph from its layer stack, dropping node edits the stack can't show.",
                  .python = "lila.ops.texRebuildLayers(\"RockTex\")"});

    menus::addActions("Texture", {"", "window.textureSets", "textureSets.addFillLayer", "textureSets.addPaintLayer", "textureSets.addFolder", "textureSets.addIdLayers",
                                  "textureSets.rebuildLayers"});
}
} // namespace lila::ui
