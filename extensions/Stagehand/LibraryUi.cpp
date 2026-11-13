// The Library panel (thumbnails, search, collections, add selection) and dragging assets from it into a viewport, where
// a box previews where the asset lands. Shift drops it with physics, Ctrl places an independent copy.

#include "Library.h"
#include "Stagehand.h"

#include "PanelKit.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Icons.h>
#include <lila/LevelToolkit.h>
#include <lila/Log.h>
#include <lila/Menus.h>
#include <lila/Mesh.h>
#include <lila/Panels.h>
#include <lila/Transform.h>
#include <lila/Viewport.h>
#include <lila/ViewportTool.h>

#include <QClipboard>
#include <QComboBox>
#include <QIcon>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPixmap>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

using namespace lila;
using namespace lila::ui;

namespace stagehand
{
namespace
{

constexpr const char* kMime = "application/x-lila-library-asset";
constexpr const char* kPanel = "stagehand.library";
constexpr int kIdRole = Qt::UserRole + 1;

QString qs(const std::string& s) { return QString::fromStdString(s); }

json runUi(const char* command, const json& call)
{
    try
    {
        return ops::execute(command, call, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Warning, e.what());
        return nullptr;
    }
}

class AssetList final : public QListWidget
{
public:
    using QListWidget::QListWidget;

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        if (items.isEmpty())
            return nullptr;
        auto* m = new QMimeData();
        m->setData(kMime, items.front()->data(kIdRole).toString().toUtf8());
        return m;
    }
    QStringList mimeTypes() const override { return {kMime}; }
};

class LibraryPanel final : public QWidget
{
public:
    explicit LibraryPanel(QWidget* parent) : QWidget(parent)
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 8, 12, 8);
        v->setSpacing(4);
        auto* top = new QHBoxLayout();
        top->setSpacing(2);
        mSearch = new QLineEdit(this);
        mSearch->setPlaceholderText("Search props");
        mSearch->setClearButtonEnabled(true);
        mCollection = new QComboBox(this);
        mCollection->setToolTip("Collection: show one collection, or all");
        auto* reload = new QToolButton(this);
        reload->setIcon(icon("refresh"));
        reload->setAutoRaise(true);
        reload->setToolTip("Reload: read the library folders again");
        top->addWidget(mSearch, 1);
        top->addWidget(mCollection);
        top->addWidget(reload);
        v->addLayout(top);

        mList = new AssetList(this);
        mList->setViewMode(QListView::IconMode);
        mList->setIconSize(QSize(96, 96));
        mList->setGridSize(QSize(112, 128));
        mList->setResizeMode(QListView::Adjust);
        mList->setMovement(QListView::Static);
        mList->setDragEnabled(true);
        mList->setDragDropMode(QAbstractItemView::DragOnly);
        mList->setContextMenuPolicy(Qt::CustomContextMenu);
        mList->setWordWrap(true);
        v->addWidget(mList, 1);

        mStatus = new QLabel(this);
        mStatus->setWordWrap(true);
        mStatus->setEnabled(false);
        v->addWidget(mStatus);
        auto* add = new QToolButton(this);
        add->setText("Add Selection");
        add->setToolTip("Add Selection: add the selected objects to the library as a prop");
        add->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        panelkit::setRole(add, "push");
        v->addWidget(add);

        QObject::connect(mSearch, &QLineEdit::textChanged, this, [this] { fill(); });
        QObject::connect(mCollection, &QComboBox::currentIndexChanged, this, [this] { fill(); });
        QObject::connect(reload, &QToolButton::clicked, this, [this] {
            library::reload();
            fillCollections();
            fill();
        });
        QObject::connect(add, &QToolButton::clicked, this, [this] { addSelection(); });
        QObject::connect(mList, &QListWidget::itemDoubleClicked, this, [](QListWidgetItem* it) { placeOnSelection(it->data(kIdRole).toString().toStdString()); });
        QObject::connect(mList, &QWidget::customContextMenuRequested, this, [this](QPoint pos) { contextMenu(pos); });
        fillCollections();
        fill();
        // The starter props come without thumbnails; the first time the panel shows, they are drawn.
        QTimer::singleShot(0, this, [this] { drawStarterThumbnails(); });
    }

private:
    void drawStarterThumbnails()
    {
        if (!library::starterEnabled())
            return;
        const bool missing = std::ranges::any_of(library::all(), [](const library::Asset& a) { return a.tier == "starter" && a.thumbnail.empty(); });
        if (!missing)
            return;
        try
        {
            ops::execute("libraryStarter", json::object());
        }
        catch (const std::exception& e)
        {
            log::write(LogLevel::Warning, std::string("Library: starter thumbnails: ") + e.what());
        }
        fill();
    }

    void fillCollections()
    {
        const QString keep = mCollection->currentText();
        QSignalBlocker block(mCollection);
        mCollection->clear();
        mCollection->addItem("All");
        for (const std::string& c : library::collections())
            mCollection->addItem(qs(c));
        const int at = mCollection->findText(keep);
        mCollection->setCurrentIndex(at >= 0 ? at : 0);
    }

    void fill()
    {
        library::Query q;
        q.text = mSearch->text().toStdString();
        if (mCollection->currentIndex() > 0)
            q.collection = mCollection->currentText().toStdString();
        const std::vector<library::Asset> found = library::search(q);
        mList->clear();
        for (const library::Asset& a : found)
        {
            auto* it = new QListWidgetItem(qs(a.name), mList);
            it->setData(kIdRole, qs(a.id));
            QString tip = qs(a.id + "\n" + a.description);
            if (a.size.x > 0.0)
                tip += QString("\n%1 x %2 x %3 cm").arg(a.size.x, 0, 'f', 0).arg(a.size.y, 0, 'f', 0).arg(a.size.z, 0, 'f', 0);
            it->setToolTip(tip);
            if (!a.thumbnail.empty())
                it->setIcon(QIcon(QPixmap(qs(a.thumbnail.generic_string()))));
        }
        const size_t errors = library::errors().size();
        const QString failed = errors ? QString(", %1 failed to load").arg(errors) : QString();
        if (!found.empty())
            mStatus->setText(QString("%1 props%2").arg(found.size()).arg(failed));
        else if (library::all().empty())
            mStatus->setText("No props. Select objects and click Add Selection." + failed);
        else
            mStatus->setText("No props match." + failed);
    }

    void addSelection()
    {
        Graph& g = Graph::current();
        if (g.selection().empty())
        {
            log::write(LogLevel::Warning, "Library: select what to add");
            return;
        }
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Add to Library", "Name", QLineEdit::Normal, qs(g.node(g.selection().front())->name()), &ok);
        if (!ok || name.isEmpty())
            return;
        const QString collection = QInputDialog::getText(this, "Add to Library", "Collection", QLineEdit::Normal,
                                                         mCollection->currentIndex() > 0 ? mCollection->currentText() : QString("Props"), &ok);
        if (!ok)
            return;
        runUi("libraryAdd", {{"name", name.toStdString()}, {"collection", collection.isEmpty() ? std::string("Props") : collection.toStdString()}});
        fillCollections();
        fill();
    }

    static void placeOnSelection(const std::string& id)
    {
        Graph& g = Graph::current();
        json call = {{"args", {id}}};
        if (!g.selection().empty())
            if (Node* n = g.node(g.selection().back()); n && n->type().isA("mesh"))
                call["on"] = n->path();
        runUi("place", call);
    }

    void contextMenu(QPoint pos)
    {
        QListWidgetItem* it = mList->itemAt(pos);
        if (!it)
            return;
        const std::string id = it->data(kIdRole).toString().toStdString();
        QMenu m(this);
        m.addAction("Place on Selection", [id] { placeOnSelection(id); });
        m.addAction("Replace Selection", [id] {
            runUi("replaceWith", {{"asset", id}});
        });
        m.addAction("Replace Selection, Fit", [id] { runUi("replaceWith", {{"asset", id}, {"fit", true}}); });
        m.addSeparator();
        m.addAction("Copy ID", [id] { QGuiApplication::clipboard()->setText(qs(id)); });
        m.addAction("Remove from Library", [this, id] {
            runUi("libraryRemove", {{"args", {id}}});
            fill();
        });
        m.exec(mList->viewport()->mapToGlobal(pos));
    }

    QLineEdit* mSearch = nullptr;
    QComboBox* mCollection = nullptr;
    AssetList* mList = nullptr;
    QLabel* mStatus = nullptr;
};

/** Where a drag would put an asset: on the surface under the cursor, else on the ground plane. */
struct Ghost
{
    std::string asset;
    glm::dvec3 at{0.0};
    glm::dvec3 normal{0, 1, 0};
    glm::dvec3 size{50.0};
    bool active = false;
} gGhost;

std::optional<glm::dvec3> groundUnder(const viewportHooks::DropTarget& t)
{
    glm::dvec3 o, d;
    t.view.ray(t.px, o, d);
    if (std::abs(d.y) < 1e-9 || -o.y / d.y < 0.0)
        return std::nullopt;
    return o + d * (-o.y / d.y);
}

glm::dvec3 faceNormal(scene::NodeId id, uint32_t face)
{
    Node* n = Graph::current().node(id);
    if (!n || !n->type().isA("mesh"))
        return {0, 1, 0};
    const auto mesh = n->as<scene::MeshPtr>(n->type().attrIndex("outMesh"));
    if (!mesh || face >= mesh->faceCapacity() || !mesh->faceValid(face))
        return {0, 1, 0};
    const glm::dmat3 m = glm::transpose(glm::inverse(glm::dmat3(n->as<glm::dmat4>(scene::Transform::kWorldMatrix))));
    return glm::normalize(m * mesh->faceNormal(face));
}

bool onDrop(const QMimeData& mime, const viewportHooks::DropTarget& t, bool commit)
{
    if (t.left)
    {
        gGhost.active = false;
        viewportTools::requestRedraw();
        return false;
    }
    if (!mime.hasFormat(kMime))
        return false;
    const std::string id = QString::fromUtf8(mime.data(kMime)).toStdString();
    const std::optional<library::Asset> a = library::find(id);
    if (!a)
        return false;
    glm::dvec3 at, normal(0, 1, 0);
    const glm::dvec3 size = a->size.x > 0.0 ? a->size : glm::dvec3(50.0);
    if (t.face != UINT32_MAX)
    {
        at = t.position;
        normal = faceNormal(t.node, t.face);
    }
    else if (auto g = groundUnder(t))
        at = *g;
    else
        return true;
    // Dropped on a wall or a thing's side: stand on whatever is below, just clear of that face.
    if (normal.y < 0.7)
    {
        glm::dvec3 out(normal.x, 0.0, normal.z);
        out = glm::length(out) > 1e-6 ? glm::normalize(out) : glm::dvec3(0.0);
        at += out * (std::max(size.x, size.z) * 0.5 + 0.5);
        if (auto below = surfaceUnder(Graph::current(), at, {}, 1.0))
            at = below->point;
        normal = {0, 1, 0};
    }
    gGhost = {id, at, normal, size, !commit};
    viewportTools::requestRedraw();
    if (!commit)
        return true;

    const Qt::KeyboardModifiers mods = QGuiApplication::keyboardModifiers();
    json call = {{"args", {id}}, {"at", json::array({at.x, at.y, at.z})}, {"snap", false}};
    if (mods & Qt::ControlModifier)
        call["mode"] = "copy";
    if (mods & Qt::ShiftModifier)
        call["drop"] = true;
    const json placed = runUi("place", call);
    if (placed.is_array() && !placed.empty())
        runUi("select", {{"args", {placed[0]["node"]}}});
    return true;
}

/** The ghost: the asset's bounds as a box standing on the drop point. */
void drawGhost(render::FrameState& s, OverlayPainter&, const ViewInfo&)
{
    if (!gGhost.active)
        return;
    const glm::dvec3 up = glm::normalize(gGhost.normal);
    const glm::dvec3 side = std::abs(up.y) > 0.9 ? glm::dvec3(1, 0, 0) : glm::normalize(glm::cross(glm::dvec3(0, 1, 0), up));
    const glm::dvec3 fwd = glm::cross(side, up);
    const glm::dvec3 h = gGhost.size * 0.5;
    glm::dvec3 c[8];
    for (int k = 0; k < 8; ++k)
        c[k] = gGhost.at + side * ((k & 1) ? h.x : -h.x) + up * ((k & 2) ? gGhost.size.y : 0.0) + fwd * ((k & 4) ? h.z : -h.z);
    const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    const glm::vec4 color(0.95f, 0.95f, 0.95f, 0.9f);
    for (const auto& e : edges)
    {
        s.lines.push_back({glm::dvec3(c[e[0]]), color});
        s.lines.push_back({glm::dvec3(c[e[1]]), color});
    }
}
} // namespace

void registerLibraryUi(ext::Context& ctx)
{
    panels::add(ctx, {.id = kPanel,
                      .title = "Prop Library",
                      .create = [](QWidget* p) -> QWidget* { return new LibraryPanel(p); },
                      .area = Qt::RightDockWidgetArea,
                      .icon = "books",
                      .open = false,
                      .toggleAction = "window.propLibrary",
                      .docs = "stagehand"});
    viewportHooks::setDropHandler("stagehand.library", onDrop);
    ctx.track("drop:stagehand.library", [] { viewportHooks::removeDropHandler("stagehand.library"); });
    viewportHooks::addDrawer(ctx, "stagehand.libraryGhost", drawGhost);
    menus::addItem(ctx, {.menu = levelToolkit::kMenu, .action = "window.propLibrary", .before = "stagehand.menu.tools"});
}
} // namespace stagehand
