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
#include <QCoreApplication>
#include <QFileInfo>
#include <QPixmap>
#include <QProcess>
#include <QStackedWidget>
#include <QTabBar>
#include <QPointer>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>
#include <set>

using namespace lila;
using namespace lila::ui;

namespace stagehand
{
namespace
{

constexpr const char* kMime = "application/x-lila-library-asset";
constexpr const char* kPresetMime = "application/x-lila-arrangement-preset";
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

class PresetList final : public QListWidget
{
public:
    using QListWidget::QListWidget;

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        if (items.isEmpty())
            return nullptr;
        auto* m = new QMimeData();
        m->setData(kPresetMime, items.front()->data(kIdRole).toString().toUtf8());
        return m;
    }
    QStringList mimeTypes() const override { return {kPresetMime}; }
};

/**
 * Draws missing thumbnails (starter props, extension tiers such as placeholder pieces, arrangement presets) in a
 * lilabatch process beside lila, so the open scene and the UI are never touched while they render. One run at a time;
 * done is called on the UI thread when it finishes.
 */
class ThumbnailBatch
{
public:
    static ThumbnailBatch& get()
    {
        static ThumbnailBatch b;
        return b;
    }

    bool running() const { return mProcess && mProcess->state() != QProcess::NotRunning; }

    /** Starts a run of the Python in script unless one is going; done runs when it ends either way. */
    void start(const QString& script, std::function<void()> done)
    {
        mDone.push_back(std::move(done));
        if (running())
            return;
        const QString exe = QCoreApplication::applicationDirPath() + "/lilabatch" +
#ifdef Q_OS_WIN
                            ".exe";
#else
                            "";
#endif
        if (!QFileInfo::exists(exe))
        {
            log::write(LogLevel::Warning, "Library: thumbnails need lilabatch beside lila (" + exe.toStdString() + "); they stay blank");
            finish();
            return;
        }
        mProcess = std::make_unique<QProcess>();
#ifdef Q_OS_WIN
        // No console window flashing up while it runs.
        mProcess->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) { a->flags |= 0x08000000; });
#endif
        QObject::connect(mProcess.get(), &QProcess::finished, [this](int code, QProcess::ExitStatus) {
            if (code != 0)
                log::write(LogLevel::Warning, "Library: drawing thumbnails stopped: " + QString::fromUtf8(mProcess->readAllStandardError()).trimmed().toStdString());
            finish();
        });
        mProcess->start(exe, {"-c", script});
    }

private:
    void finish()
    {
        library::reload();
        std::vector<std::function<void()>> done;
        done.swap(mDone);
        for (auto& f : done)
            f();
    }

    std::unique_ptr<QProcess> mProcess;
    std::vector<std::function<void()>> mDone;
};

/**
 * Presets a finished run still left without a thumbnail (registered only in this session by an extension, or with
 * nothing to show): later panels don't start another run for them.
 */
std::set<std::string>& undrawablePresets()
{
    static std::set<std::string> names;
    return names;
}

/** Every thumbnail the library shows that is still missing. */
const char* kAllThumbnails = "libraryStarter()\nlibraryThumbnails(tier='placeholder')\narrangementThumbnails()\n";

/** The preset's spec by name, or null. */
json presetNamed(const std::string& name)
{
    const json all = runUi("arrangementPresets", json::object());
    for (const json& p : all.is_array() ? all : json::array())
        if (p.value("name", std::string()) == name)
            return p;
    return nullptr;
}

/**
 * Runs a preset on objects: weathering presets through weather (every mesh under them), others with the objects in their
 * first place slot (the floor, the boards, the ground).
 */
void applyPreset(const std::string& name, const std::vector<std::string>& targets)
{
    if (targets.empty())
    {
        log::write(LogLevel::Warning, "Presets: select what to dress, or drag the preset onto it");
        return;
    }
    const json spec = presetNamed(name);
    if (spec.is_null())
        return;
    const json slots = spec.value("slots", json::object());
    if (spec.value("category", std::string()) == "Weathering" && slots.contains("targets"))
    {
        runUi("weather", {{"args", targets}, {"preset", name}});
        return;
    }
    json fill = json::object();
    for (const auto& [slot, s] : slots.items())
    {
        const std::string kind = s.value("kind", std::string());
        if (kind == "mesh")
            fill[slot] = targets.front();
        else if (kind == "meshes")
            fill[slot] = targets;
        else
            continue;
        break;
    }
    runUi("arrangePreset", {{"args", {name}}, {"slots", fill}});
}

std::vector<std::string> selectedPaths()
{
    Graph& g = Graph::current();
    std::vector<std::string> out;
    for (NodeId id : g.selection())
        if (Node* n = g.node(id))
            out.push_back(n->path());
    return out;
}

/** Presets as picture tiles: double-click runs one on the selection, dragging one onto an object runs it there. */
class PresetsPage final : public QWidget
{
public:
    explicit PresetsPage(QWidget* parent) : QWidget(parent)
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(4);
        auto* top = new QHBoxLayout();
        top->setSpacing(2);
        mSearch = new QLineEdit(this);
        mSearch->setPlaceholderText("Search presets");
        mSearch->setClearButtonEnabled(true);
        mKind = new QComboBox(this);
        mKind->addItems({"All", "Scatter", "Ground", "Surface", "Growth", "Generators", "Set Dressing"});
        mKind->setToolTip("Kind: Age presets dress round objects, Ground presets dress a ground, Set Dressing fills a place");
        mStyle = new QComboBox(this);
        mStyle->addItems({"Any Style", "Realistic", "Toon"});
        mStyle->setToolTip("Style of the preset's look and placeholder pieces");
        top->addWidget(mSearch, 1);
        top->addWidget(mKind);
        top->addWidget(mStyle);
        v->addLayout(top);
        mList = new PresetList(this);
        mList->setViewMode(QListView::IconMode);
        mList->setIconSize(QSize(96, 96));
        mList->setGridSize(QSize(112, 132));
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

        QObject::connect(mSearch, &QLineEdit::textChanged, this, [this] { fill(); });
        QObject::connect(mKind, &QComboBox::currentIndexChanged, this, [this] { fill(); });
        QObject::connect(mStyle, &QComboBox::currentIndexChanged, this, [this] { fill(); });
        QObject::connect(mList, &QListWidget::itemDoubleClicked, this,
                         [](QListWidgetItem* it) { applyPreset(it->data(kIdRole).toString().toStdString(), selectedPaths()); });
        QObject::connect(mList, &QWidget::customContextMenuRequested, this, [this](QPoint pos) { contextMenu(pos); });
    }

    void showing() { fill(); }

    void fill()
    {
        fillList();
        if (ThumbnailBatch::get().running())
            mStatus->setText(mStatus->text() + ", drawing thumbnails");
    }

private:
    static std::string kindOf(const json& p)
    {
        if (p.value("category", std::string()) != "Weathering")
            return "Set Dressing";
        const std::string kind = p.value("kind", std::string());
        return kind == "ground" ? "Ground" : kind == "surface" ? "Surface" : kind == "growth" ? "Growth" : kind == "generator" ? "Generators" : "Scatter";
    }

    void fillList()
    {
        const json all = runUi("arrangementPresets", json::object());
        const QString words = mSearch->text().trimmed().toLower();
        const std::string kind = mKind->currentIndex() > 0 ? mKind->currentText().toStdString() : std::string();
        const std::string style = mStyle->currentIndex() == 1 ? "realistic" : mStyle->currentIndex() == 2 ? "toon" : "";
        mList->clear();
        int shown = 0;
        for (const json& p : all.is_array() ? all : json::array())
        {
            const std::string name = p.value("name", std::string());
            const std::string description = p.value("description", std::string());
            if (!kind.empty() && kindOf(p) != kind)
                continue;
            if (!style.empty() && p.value("style", std::string("realistic")) != style)
                continue;
            if (!words.isEmpty() && !qs(name + " " + description).toLower().contains(words))
                continue;
            auto* it = new QListWidgetItem(qs(name), mList);
            it->setData(kIdRole, qs(name));
            it->setToolTip(qs(description) + "\nDouble-click to dress the selection, or drag onto an object.");
            if (const std::string t = p.value("thumbnail", std::string()); !t.empty())
                it->setIcon(QIcon(QPixmap(qs(t))));
            else
                it->setIcon(icon("photo"));
            ++shown;
        }
        mStatus->setText(shown ? QString("%1 presets").arg(shown) : QString("No presets match."));
    }

    void contextMenu(QPoint pos)
    {
        QListWidgetItem* it = mList->itemAt(pos);
        if (!it)
            return;
        const std::string name = it->data(kIdRole).toString().toStdString();
        QMenu m(this);
        m.addAction("Dress Selection", [name] { applyPreset(name, selectedPaths()); });
        m.addAction("Redraw Thumbnail", [this, name] {
            QPointer<PresetsPage> self(this);
            const QString literal = QString::fromStdString(json(name).dump());
            ThumbnailBatch::get().start("arrangementThumbnails([" + literal + "], force=True)\n", [self] {
                if (self)
                    self->fill();
            });
            fill();
        });
        m.addSeparator();
        m.addAction("Copy Name", [name] { QGuiApplication::clipboard()->setText(qs(name)); });
        m.exec(mList->viewport()->mapToGlobal(pos));
    }

    QLineEdit* mSearch = nullptr;
    QComboBox* mKind = nullptr;
    QComboBox* mStyle = nullptr;
    PresetList* mList = nullptr;
    QLabel* mStatus = nullptr;
};

class LibraryPanel;
/** The open Library panel, for actions that show one of its tabs. */
LibraryPanel* gLibraryPanel = nullptr;

class LibraryPanel final : public QWidget
{
public:
    explicit LibraryPanel(QWidget* parent) : QWidget(parent)
    {
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(12, 8, 12, 8);
        outer->setSpacing(4);
        gLibraryPanel = this;
        auto* tabs = new QTabBar(this);
        mTabs = tabs;
        tabs->addTab("Props");
        tabs->addTab("Presets");
        tabs->setExpanding(false);
        tabs->setDrawBase(false);
        outer->addWidget(tabs);
        auto* pages = new QStackedWidget(this);
        auto* props = new QWidget(pages);
        mPresets = new PresetsPage(pages);
        pages->addWidget(props);
        pages->addWidget(mPresets);
        outer->addWidget(pages, 1);
        QObject::connect(tabs, &QTabBar::currentChanged, this, [this, pages](int i) {
            pages->setCurrentIndex(i);
            if (i == 1)
                mPresets->showing();
        });
        auto* v = new QVBoxLayout(props);
        v->setContentsMargins(0, 0, 0, 0);
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
        QTimer::singleShot(0, this, [this] { drawThumbnails(); });
    }

private:
    /** Starts the background run for whatever thumbnails are missing; both tabs refill when it ends. */
    void drawThumbnails()
    {
        const bool props = std::ranges::any_of(library::all(), [](const library::Asset& a) {
            return (a.tier == "starter" || a.tier == "placeholder") && a.thumbnail.empty();
        });
        const json presets = runUi("arrangementPresets", json::object());
        const bool presetsMissing = std::ranges::any_of(presets.is_array() ? presets : json::array(), [](const json& p) {
            return !p.contains("thumbnail") && !undrawablePresets().contains(p.value("name", std::string()));
        });
        if (!props && !presetsMissing)
            return;
        QPointer<LibraryPanel> self(this);
        ThumbnailBatch::get().start(kAllThumbnails, [self] {
            const json after = runUi("arrangementPresets", json::object());
            std::string left;
            for (const json& p : after.is_array() ? after : json::array())
                if (!p.contains("thumbnail") && undrawablePresets().insert(p.value("name", std::string())).second)
                    left += (left.empty() ? "" : ", ") + p.value("name", std::string());
            if (!left.empty())
                log::write(LogLevel::Info, "Library: no thumbnail could be drawn for " + left + "; they show a plain icon");
            if (!self)
                return;
            self->fill();
            self->mPresets->fill();
        });
        mPresets->fill();
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
        std::vector<library::Asset> found = library::search(q);
        // Placeholder pieces are stand-ins presets use; they show when their collection is picked or searched for.
        if (q.collection.empty() && q.text.empty())
            std::erase_if(found, [](const library::Asset& a) { return a.tier == "placeholder"; });
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
    PresetsPage* mPresets = nullptr;
    QTabBar* mTabs = nullptr;

public:
    ~LibraryPanel() override
    {
        if (gLibraryPanel == this)
            gLibraryPanel = nullptr;
    }
    void showPresets() { mTabs->setCurrentIndex(1); }
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
    if (mime.hasFormat(kPresetMime))
    {
        if (!commit)
            return true;
        // Dropped on an object: that object; dropped on empty space: the selection. Applied once the drag has ended, so
        // the drop itself returns at once.
        Node* n = t.face != UINT32_MAX ? Graph::current().node(t.node) : nullptr;
        const std::string name = QString::fromUtf8(mime.data(kPresetMime)).toStdString();
        const std::vector<std::string> targets = n ? std::vector<std::string>{n->path()} : selectedPaths();
        QTimer::singleShot(0, qApp, [name, targets] { applyPreset(name, targets); });
        return true;
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
    actions::add(ctx, {.id = "window.libraryPresets",
                       .label = "Library Presets",
                       .category = "Windows",
                       .run = [] {
                           runUi("showPanel", {{"args", {kPanel}}});
                           if (gLibraryPanel)
                               gLibraryPanel->showPresets();
                       },
                       .doc = "Open the Prop Library on its Presets tab: scatter sets, grunge, edge decals, ivy and generators to drag onto meshes.",
                       .icon = "photo",
                       .keywords = {"weathering", "presets", "scatter", "grunge", "asset browser", "drag and drop"}});
    viewportHooks::setDropHandler("stagehand.library", onDrop);
    ctx.track("drop:stagehand.library", [] { viewportHooks::removeDropHandler("stagehand.library"); });
    viewportHooks::addDrawer(ctx, "stagehand.libraryGhost", drawGhost);
    menus::addItem(ctx, {.menu = levelToolkit::kMenu, .action = "window.propLibrary", .before = "stagehand.menu.tools"});
}
} // namespace stagehand
