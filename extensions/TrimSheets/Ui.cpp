// Level > Trim Sheets and the Level Toolkit's Trim Sheets section: the current sheet drawn as a palette (click a trim to
// map the selection onto it), new sheets from presets, bake, auto trim with cycling, shifting, edge trims and sending.

#include "TrimSheets.h"
#include "TrimBake.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Controls.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/LevelToolkit.h>
#include <lila/Log.h>
#include <lila/Menus.h>
#include <lila/Panels.h>

#include <QComboBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include <filesystem>
#include <format>

using namespace lila;
using namespace lila::ui;

namespace trims
{
using json = nlohmann::json;

namespace
{
void run(const char* command, const json& call)
{
    try
    {
        ops::execute(command, call, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Warning, e.what());
    }
}

/**
 * The selected meshes (and the meshes under selected groups) to bake as the current sheet's detail, leaving out the sheet's
 * own generated high and low.
 */
std::vector<std::string> selectedDetail()
{
    scene::Graph& g = scene::Graph::current();
    std::string sheet;
    try
    {
        sheet = load(currentSheet()).name;
    }
    catch (const std::exception&)
    {
    }
    // Meshes mapped onto a sheet are its users, not detail modelled on its high.
    auto mapped = [&](scene::NodeId id) {
        for (scene::NodeId at : {id, g.parent(id)})
            if (scene::Node* n = g.node(at))
                try
                {
                    const scene::PlugValue v = g.value(g.plug(n->path() + "." + kNodeSheet));
                    if (const std::string* p = std::get_if<std::string>(&v); p && !p->empty())
                        return true;
                }
                catch (const std::exception&)
                {
                }
        return false;
    };
    std::vector<std::string> out;
    auto add = [&](scene::NodeId id) {
        scene::Node* n = g.node(id);
        if (!n || !n->type().isA("mesh") || n->name() == sheet + "_High" || n->name() == sheet + "_Low" || mapped(id))
            return;
        if (std::ranges::find(out, n->path()) == out.end())
            out.push_back(n->path());
    };
    for (scene::NodeId id : g.selection())
    {
        add(id);
        for (scene::NodeId d : g.descendants(id))
            add(d);
    }
    return out;
}

/** How many times Next Best Trim has run since the last Auto Trim: each press takes the next-best match. */
int gCycle = 0;

/** The painted preview the Trim Sheet Editor keeps when the sheet is textured, else the baked normal map. */
std::filesystem::path shownFile(const Sheet& s, const std::filesystem::path& f)
{
    const std::filesystem::path painted = textureFolder(s, f) / "Preview" / ("TS_" + materialName(s).substr(2) + "_baseColor.png");
    return std::filesystem::exists(painted) ? painted : mapFile(s, f, "normal");
}

/** The current sheet as a picture with its trims outlined; clicking a trim maps the selected faces onto it. */
class SheetView : public QWidget
{
public:
    explicit SheetView(QWidget* parent) : QWidget(parent)
    {
        setMouseTracking(true);
        setMinimumHeight(160);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { refresh(); });
        timer->start(1000);
        refresh();
    }

    QSize sizeHint() const override { return {240, 240}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return mSheet ? int(double(w) * mSheet->height / std::max(1, mSheet->width)) : w; }

    /** Called after each reload with the sheet, or null when there is none. */
    std::function<void(const Sheet*)> onSheet;

    /** Reloads when the current sheet or its textures changed on disk, or always when forced. */
    void refresh(bool force = false)
    {
        const std::string path = currentSheet();
        std::error_code ec;
        const auto stamp = [&](const std::filesystem::path& p) {
            return std::filesystem::exists(p, ec) ? std::filesystem::last_write_time(p, ec).time_since_epoch().count() : 0;
        };
        const long long sheetStamp = path.empty() ? 0 : stamp(path);
        long long texStamp = 0;
        if (mSheet)
            texStamp = stamp(shownFile(*mSheet, path));
        if (!force && path == mPath && sheetStamp == mSheetStamp && texStamp == mTexStamp)
            return;
        mPath = path;
        mSheetStamp = sheetStamp;
        mSheet.reset();
        mImage = QImage();
        if (!path.empty() && sheetStamp)
        {
            try
            {
                mSheet = load(path);
                const std::filesystem::path shown = shownFile(*mSheet, path);
                mTexStamp = stamp(shown);
                if (mTexStamp)
                    mImage.load(QString::fromStdString(shown.generic_string()));
            }
            catch (const std::exception& e)
            {
                log::write(LogLevel::Warning, e.what());
            }
        }
        if (onSheet)
            onSheet(mSheet ? &*mSheet : nullptr);
        updateGeometry();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        const QRect area = sheetRect();
        if (!mSheet)
        {
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, "No trim sheet.\nPick a preset and click New.");
            return;
        }
        if (!mImage.isNull())
            p.drawImage(area, mImage);
        else
            p.fillRect(area, palette().color(QPalette::Base));
        for (size_t i = 0; i < mSheet->trims.size(); ++i)
        {
            const QRect r = trimRect(i);
            const glm::vec3 c = trimColor(i);
            QColor col = QColor::fromRgbF(c.r, c.g, c.b);
            if (mImage.isNull())
                p.fillRect(r, col.darker(160));
            col.setAlphaF(int(i) == mHover ? 1.0f : 0.55f);
            p.setPen(QPen(col, int(i) == mHover ? 2 : 1));
            p.drawRect(r.adjusted(0, 0, -1, -1));
            if (r.height() >= 12 && r.width() >= 16)
            {
                p.setPen(Qt::white);
                p.drawText(r.adjusted(3, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, QString::number(i));
            }
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        const int hit = trimAt(e->position().toPoint());
        if (hit != mHover)
        {
            mHover = hit;
            update();
        }
        if (hit >= 0 && mSheet)
        {
            const Trim& t = mSheet->trims[size_t(hit)];
            const glm::dvec2 w = mSheet->worldSize(t);
            QToolTip::showText(e->globalPosition().toPoint(),
                               QString::fromStdString(std::format("{}  {}: {} x {} px, {:.0f} x {:.0f} cm\nClick to map the selected faces or meshes onto it.", hit,
                                                                  t.name, t.sizePx().x, t.sizePx().y, w.x, w.y)),
                               this);
        }
    }

    void leaveEvent(QEvent*) override
    {
        mHover = -1;
        update();
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        const int hit = trimAt(e->position().toPoint());
        if (hit >= 0 && e->button() == Qt::LeftButton)
            run("trimApply", {{"trim", hit}});
    }

private:
    QRect sheetRect() const
    {
        if (!mSheet)
            return rect();
        const double aspect = double(mSheet->width) / std::max(1, mSheet->height);
        int w = width(), h = int(w / aspect);
        if (h > height())
        {
            h = height();
            w = int(h * aspect);
        }
        return {(width() - w) / 2, 0, w, h};
    }

    QRect trimRect(size_t i) const
    {
        const QRect a = sheetRect();
        const Trim& t = mSheet->trims[i];
        const double sx = double(a.width()) / mSheet->width, sy = double(a.height()) / mSheet->height;
        return QRect(QPoint(a.x() + int(t.min.x * sx), a.y() + int(t.min.y * sy)), QPoint(a.x() + int(t.max.x * sx) - 1, a.y() + int(t.max.y * sy) - 1));
    }

    int trimAt(QPoint at) const
    {
        if (!mSheet)
            return -1;
        for (size_t i = 0; i < mSheet->trims.size(); ++i)
            if (trimRect(i).contains(at))
                return int(i);
        return -1;
    }

    std::string mPath;
    long long mSheetStamp = 0;
    long long mTexStamp = 0;
    std::optional<Sheet> mSheet;
    QImage mImage;
    int mHover = -1;
};

QWidget* sectionWidget(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);

    v->setSpacing(4);

    auto* row = new QHBoxLayout();
    auto* presetBox = new QComboBox(w);
    presetBox->setToolTip("Preset for a new sheet");
    for (const PresetInfo& p : presets())
    {
        presetBox->addItem(QString::fromStdString(p.label), QString::fromStdString(p.id));
        presetBox->setItemData(presetBox->count() - 1, QString::fromStdString(p.doc), Qt::ToolTipRole);
    }
    auto* make = new QPushButton("New", w);
    make->setToolTip("New sheet from the preset, generated and baked; an existing one of that name is reused");
    QObject::connect(make, &QPushButton::clicked, w, [presetBox] { run("trimSheetCreate", {{"preset", presetBox->currentData().toString().toStdString()}}); });
    row->addWidget(presetBox, 1);
    row->addWidget(make);
    v->addLayout(row);

    auto* name = new QLabel(w);
    name->setContentsMargins(2, 4, 0, 0);
    controls::setRole(name, "dim");
    v->addWidget(name);
    auto* view = new SheetView(w);
    v->addWidget(view);
    auto* hint = new QLabel("Click a trim to map the selection onto it", w);
    hint->setContentsMargins(2, 0, 0, 0);
    controls::setRole(hint, "hint");
    v->addWidget(hint);
    view->onSheet = [name, hint](const Sheet* s) {
        name->setVisible(s);
        hint->setVisible(s);
        if (s)
            name->setText(QString::fromStdString(std::format("{}  {} x {} px", s->name, s->width, s->height)));
    };
    view->refresh(true);
    return w;
}
} // namespace

void registerUi(ext::Context& ctx)
{
    struct Def
    {
        const char* id;
        const char* label;
        const char* command;
        json call;
        const char* doc;
        const char* python;
    };
    const Def defs[] = {
        {"trims.create", "New Trim Sheet", "trimSheetCreate", json{{"preset", "industrialMetal"}},
         "A new trim sheet from a preset: generated, baked and given its material in one step.", "trimSheetCreate(preset='industrialMetal')"},
        {"trims.bake", "Bake Trim Sheet", "trimSheetBake", json::object(),
         "Rebake the current sheet's maps (normal, AO, curvature, height, cavity) after changing its layout or detail.", "trimSheetBake()"},
        {"trims.generate", "Show Sheet High/Low", "trimSheetGenerate", json::object(),
         "Put the current sheet's high and low poly in the scene, lying flat at the origin, to model detail on.", "trimSheetGenerate()"},
        {"trims.test", "Test Blockout", "trimSheetTest", json::object(),
         "Put a test blockout (wall, beam, steps, pillar, slab) in the scene, auto trimmed onto the current sheet, to see how it reads.",
         "trimSheetTest()"},
        {"trims.auto", "Auto Trim", "trimAuto", json::object(),
         "Map the selected faces or meshes onto the current sheet: each island goes to the trim that best fits its shape and size.", "trimAuto()"},
        {"trims.live", "Live Trim", "trimLive", json::object(), "Keep the selected meshes trimmed while you edit them (a live Auto Trim modifier).",
         "trimLive()"},
        {"trims.check", "Check Trims", "trimCheck", json{{"showIssues", true}},
         "Paint red where the selected meshes' trims are wrong: texel density, alignment, orientation, stretch, repetition.",
         "trimCheck(showIssues=True)"},
        {"trims.shiftUp", "Shift Trim Up", "trimShift", json{{"steps", -1}}, "Move the selected faces to the trim above theirs on the sheet.",
         "trimShift(steps=-1)"},
        {"trims.shiftDown", "Shift Trim Down", "trimShift", json{{"steps", 1}}, "Move the selected faces to the trim below theirs on the sheet.",
         "trimShift(steps=1)"},
        {"trims.edges", "Chamfer Edges to Trim", "trimEdges", json::object(),
         "Chamfer the selected edges and map the chamfers onto the sheet's edge trims, so hard corners read bevelled.", "trimEdges(width=1.0)"},
        {"trims.curve", "Trim Along Curve", "trimAlongCurve", json::object(),
         "Build a strip of trim along the selected curves, capped at the ends: kerbs, edging, skirting, cornices.", "trimAlongCurve(width=15)"},
        {"trims.decal", "Edge Strip", "trimDecal", json::object(),
         "Float a thin trim strip over the selected edges, as a separate mesh: piping, seams, worn edges.", "trimDecal(width=2)"},
        {"trims.cut", "Edge Groove", "trimDecal", json{{"cut", true}}, "Carve a groove along the selected edges, its floor and walls on an edge trim.",
         "trimDecal(cut=True, depth=0.5)"},
        {"trims.decalSend", "Send Edge Strips to Unreal", "trimDecalSend", json::object(),
         "Send the placed unique decals to Unreal as deferred decal actors with the generated decal material.", "trimDecalSend()"},
        {"trims.weightedNormals", "Weighted Normals", "polyWeightedNormals", json::object(),
         "Face-weighted normals: big faces stay flat beside chamfers and bevels.", "polyWeightedNormals()"},
        {"trims.texture", "Texture Trim Sheet", "trimSheetTexture", json::object(),
         "Put a smart material on the sheet: wear, dirt and chips follow its baked bevels on every mesh using it.",
         "trimSheetTexture(material='Rusty Iron')"},
        {"trims.export", "Export Trim Layout", "trimSheetExport", json::object(), "Write the current sheet's layout as a Valve .rect, SVG, ID map or JSON.",
         "trimSheetExport(format='rect')"},
        {"trims.send", "Send Trim Sheet to Unreal", "trimSheetSend", json::object(),
         "Send the sheet's textures and material to Unreal and put it on the meshes using it.", "trimSheetSend()"},
    };
    for (const Def& d : defs)
    {
        Action act{.id = d.id, .label = d.label, .category = "Trim Sheets", .doc = d.doc, .python = d.python, .command = d.command, .call = d.call};
        // Auto Trim starts a new cycle; Next Best Trim continues it.
        if (std::string_view(d.id) == "trims.auto")
            act.run = [] {
                gCycle = 0;
                run("trimAuto", json::object());
            };
        actions::add(ctx, std::move(act));
    }
    actions::add(ctx, {.id = "trims.paint",
                       .label = "Paint Trim Sheet",
                       .category = "Trim Sheets",
                       .run =
                           [] {
                               try
                               {
                                   ops::execute("trimSheetPaint", json::object(), CallSource::UI);
                               }
                               catch (const std::exception& e)
                               {
                                   log::write(LogLevel::Warning, e.what());
                                   return;
                               }
                               log::write(LogLevel::Info, "Painting the trim sheet itself: every mesh using it shows the strokes.");
                               actions::run(json("tex.paint"));
                           },
                       .doc = "Paint the current trim sheet by hand on a flat preview plane; strokes show on every mesh using the sheet.",
                       .python = "trimSheetPaint()"});
    actions::add(ctx, {.id = "trims.detail",
                       .label = "Bake Selection as Sheet Detail",
                       .category = "Trim Sheets",
                       .run =
                           [] {
                               const std::vector<std::string> detail = selectedDetail();
                               if (detail.empty())
                               {
                                   log::write(LogLevel::Warning, "Select the meshes you modelled on the sheet's high poly (Show Sheet High/Low); meshes "
                                                                 "mapped onto the sheet don't count.");
                                   return;
                               }
                               run("trimSheetBake", {{"high", detail}});
                           },
                       .doc = "Bake the selected meshes into the current sheet on top of its generated relief: carvings, motifs, a sculpt. The sheet "
                              "keeps them for every later bake.",
                       .python = "trimSheetBake(high=['Rosette', 'Dentil'])"});
    actions::add(ctx, {.id = "trims.detailClear",
                       .label = "Bake Without Sheet Detail",
                       .category = "Trim Sheets",
                       .run = [] { run("trimSheetBake", {{"high", json::array()}}); },
                       .doc = "Forget the sheet's detail meshes and bake its generated relief alone.",
                       .python = "trimSheetBake(high=[])"});
    actions::add(ctx, {.id = "trims.editor",
                       .label = "Trim Sheet Editor",
                       .category = "Trim Sheets",
                       .run = [] { panels::show("trimSheetEditor"); },
                       .doc = "Lay out the current sheet: drag borders to resize strips, split and retype trims, set bevels and tags.",
                       .python = "lila.ui.run_action('trims.editor')"});
    actions::add(ctx, {.id = "trims.cycle",
                       .label = "Next Best Trim",
                       .category = "Trim Sheets",
                       .run = [] { run("trimAuto", {{"cycle", ++gCycle}}); },
                       .doc = "Move the selection to its next-best fitting trims; press again for the ones after.",
                       .python = "trimAuto(cycle=1)"});

    registerEditor(ctx);

    using Item = levelToolkit::Item;
    levelToolkit::add(ctx, {.id = "trims",
                            .title = "Trim Sheets",
                            .items = {Item{.action = "trims.auto", .icon = "wand", .text = "Auto Trim", .shelf = "Trim", .group = "Map"},
                                      Item{.action = "trims.cycle", .icon = "refresh", .text = "Next Best", .group = "Map"},
                                      Item{.action = "trims.shiftUp", .icon = "arrow-up", .text = "Shift Up", .group = "Map"},
                                      Item{.action = "trims.shiftDown", .icon = "arrow-down", .text = "Shift Down", .group = "Map"},
                                      Item{.action = "trims.edges", .icon = "border-sides", .text = "Chamfer", .group = "Edges"},
                                      Item{.action = "trims.decal", .icon = "sticker", .text = "Strip", .group = "Edges"},
                                      Item{.action = "trims.cut", .icon = "line-dashed", .text = "Groove", .group = "Edges"},
                                      Item{.action = "trims.curve", .icon = "route", .text = "Curve", .group = "Edges"},
                                      Item{.action = "trims.editor", .icon = "layout-rows", .text = "Layout", .group = "Sheet"},
                                      Item{.action = "trims.bake", .icon = "flame", .text = "Bake", .group = "Sheet"},
                                      Item{.action = "trims.texture", .icon = "brush", .text = "Texture", .group = "Sheet"},
                                      Item{.action = "trims.paint", .icon = "paint", .text = "Paint", .group = "Sheet"},
                                      Item{.action = "trims.live", .icon = "bolt", .text = "Live Trim"},
                                      Item{.action = "trims.check", .icon = "checklist", .text = "Check Trims"},
                                      Item{.action = "trims.send", .icon = "send", .text = "Send to Unreal"},
                                      Item{.action = "trims.generate", .rare = true},
                                      Item{.action = "trims.detail", .rare = true},
                                      Item{.action = "trims.detailClear", .rare = true},
                                      Item{.action = "trims.test", .rare = true},
                                      Item{.action = "trims.weightedNormals", .rare = true},
                                      Item{.action = "trims.decalSend", .rare = true},
                                      Item{.action = "trims.export", .rare = true}},
                            .create = sectionWidget,
                            .createFirst = true,
                            .order = 60,
                            .docs = "trim-sheets"});

    menus::addItem(ctx, {.id = "trims.menu", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Trim Sheets"});
    const char* order[] = {"trims.create",  "trims.editor",   "trims.bake",      "trims.texture",   "trims.paint",
                           "",              "trims.generate", "trims.detail",    "trims.detailClear", "",
                           "trims.auto",    "trims.cycle",    "trims.shiftUp",   "trims.shiftDown", "trims.live",
                           "trims.check",   "",               "trims.edges",     "trims.decal",     "trims.cut",
                           "trims.curve",   "trims.weightedNormals", "",          "trims.test",      "trims.export",
                           "trims.send",    "trims.decalSend"};
    int sep = 0;
    for (const char* a : order)
    {
        if (*a == '\0')
            menus::addItem(ctx, {.id = std::format("trims.menu.sep{}", sep++), .menu = "Level/Trim Sheets", .kind = menus::Kind::Separator});
        else
            menus::addItem(ctx, {.id = std::string("trims.menu.") + a, .menu = "Level/Trim Sheets", .action = a});
    }
}
} // namespace trims
