#include "ui/MaterialEditor.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/ProjectTextures.h"
#include "core/Profile.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "ui/AchievementRules.h"
#include "ui/Actions.h"
#include "ui/ChoiceTips.h"
#include "ui/ColorPopup.h"
#include "ui/Controls.h"
#include "ui/LocateFlash.h"
#include "ui/Deletion.h"
#include "ui/DockSidebar.h"
#include "ui/Icons.h"
#include "ui/ImagePicker.h"
#include "ui/MaterialDraw.h"
#include "ui/OutlinerFilter.h"
#include "ui/SpinScrub.h"
#include "ui/TextureCache.h"
#include "ui/TexturePicker.h"
#include "ui/UiCommands.h"
#include "ui/UiCommon.h"
#include "ui/ValueReset.h"

#include <QGuiApplication>
#include <QApplication>
#include <QCheckBox>
#include <QDoubleValidator>
#include <QDropEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QComboBox>
#include <QDockWidget>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <optional>
#include <set>

namespace lila::ui
{
namespace
{
using namespace scene::material;
namespace fs = std::filesystem;

constexpr int kPreviewThumb = 40;
constexpr int kPreviewSize = 112;

QColor displayColor(const glm::dvec3& linear)
{
    return QColor::fromRgbF(linearToSrgb(float(linear.x)), linearToSrgb(float(linear.y)), linearToSrgb(float(linear.z)));
}

/** assignMaterial with no objects would find something to assign to: faces in face mode, else selected meshes. */
bool hasAssignTargets()
{
    scene::Graph& g = scene::Graph::current();
    if (scene::components::mode() == scene::ComponentMode::Face)
        for (const auto& [node, ids] : scene::components::selected(scene::ComponentMode::Face))
            if (!ids.empty())
                return true;
    auto isMesh = [&](scene::NodeId id) {
        const scene::Node* n = g.node(id);
        return n && n->type().isA("mesh");
    };
    for (scene::NodeId id : g.selection())
        if (isMesh(id) || std::ranges::any_of(g.descendants(id), isMesh))
            return true;
    return false;
}

/** Creates a material and, when there is something to assign it to (or `always`), assigns it, as one undo step. */
json createAndAssign(bool always)
{
    UndoChunk chunk("New Material");
    json name = runUi("createMaterial", {{"args", json::array({"M_Material"})}});
    if (name.is_string() && (always || hasAssignTargets()))
        runUi("assignMaterial", {{"args", json::array({name})}});
    return name;
}

/** The local image files among dropped urls (Explorer, Lila's file dialog). */
std::vector<std::string> droppedImages(const QMimeData* mime)
{
    std::vector<std::string> files;
    if (!mime || !mime->hasUrls())
        return files;
    for (const QUrl& url : mime->urls())
    {
        if (!url.isLocalFile())
            continue;
        QFileInfo file(url.toLocalFile());
        if (file.isFile() && projectTextures::isImageExtension(file.suffix().toStdString()))
            files.push_back(file.absoluteFilePath().toStdString());
    }
    return files;
}

/** Sets each file on the channel its name suggests, as one undo step. Of two files for one channel the shorter name wins. */
void assignByName(const scene::Node& material, std::vector<std::string> files)
{
    std::ranges::sort(files, [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    UndoChunk chunk("Assign Textures");
    std::set<std::string> filled;
    std::string skipped;
    for (const std::string& file : files)
    {
        std::string channel = scene::materials::guessTextureChannel(file);
        std::string key = channel == "mro" ? "orm" : channel;
        if (channel.empty() || !filled.insert(key).second)
        {
            if (!skipped.empty())
                skipped += ", ";
            skipped += paths::toUtf8(paths::fromUtf8(file).filename());
            continue;
        }
        runUi("setMaterialTexture", {{"args", json::array({material.path()})}, {"channel", channel}, {"path", file}});
    }
    if (!skipped.empty())
        log::warn("Textures not assigned (no channel in the name, or the channel was already taken): {}", skipped);
    achievementRules::texturesDropped(int(filled.size()));
}

/** Accepts image files dropped on a texture slot or the editor and hands their paths to `drop`. */
class TextureDrop : public QObject
{
public:
    TextureDrop(QObject* parent, std::function<void(const std::vector<std::string>&)> drop)
        : QObject(parent), mDrop(std::move(drop))
    {
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        switch (event->type())
        {
        case QEvent::DragEnter:
        case QEvent::DragMove:
        {
            auto* e = static_cast<QDropEvent*>(event);
            if (droppedImages(e->mimeData()).empty())
                return QObject::eventFilter(watched, event);
            e->acceptProposedAction();
            return true;
        }
        case QEvent::Drop:
        {
            auto* e = static_cast<QDropEvent*>(event);
            std::vector<std::string> files = droppedImages(e->mimeData());
            if (files.empty())
                return QObject::eventFilter(watched, event);
            e->acceptProposedAction();
            mDrop(files);
            return true;
        }
        default:
            return QObject::eventFilter(watched, event);
        }
    }

private:
    std::function<void(const std::vector<std::string>&)> mDrop;
};

QIcon swatchIcon(const QColor& c)
{
    QPixmap pm(36, 18);
    pm.fill(c);
    QPainter p(&pm);
    p.setPen(QColor(20, 20, 20));
    p.drawRect(pm.rect().adjusted(0, 0, -1, -1));
    return QIcon(pm);
}

/** A lit ball, shaded on the CPU like the viewport's key light plus ambient; the base color map wraps it. */
QPixmap previewBall(glm::vec3 base, float metal, float rough, glm::vec3 emissive, const QImage& map)
{
    LILA_ZONE();

    rough = std::clamp(rough, 0.04f, 1.0f);
    QImage img(kPreviewSize, kPreviewSize, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    const glm::vec3 l = glm::normalize(glm::vec3(-0.5f, 0.7f, 0.6f)), v(0, 0, 1);
    const glm::vec3 h = glm::normalize(l + v);
    float r = kPreviewSize * 0.5f - 2.0f;
    for (int y = 0; y < kPreviewSize; ++y)
        for (int x = 0; x < kPreviewSize; ++x)
        {
            float px = (x + 0.5f - kPreviewSize * 0.5f) / r, py = (kPreviewSize * 0.5f - y - 0.5f) / r;
            float d2 = px * px + py * py;
            if (d2 > 1.0f)
                continue;
            glm::vec3 nrm(px, py, std::sqrt(1.0f - d2));
            glm::vec3 albedo = base;
            if (!map.isNull())
            {
                float u = 0.5f + std::atan2(nrm.x, nrm.z) / (2.0f * 3.14159265f), t = 0.5f - std::asin(nrm.y) / 3.14159265f;
                QRgb c = map.pixel(std::clamp(int(u * map.width()), 0, map.width() - 1), std::clamp(int(t * map.height()), 0, map.height() - 1));
                albedo = {srgbToLinear(qRed(c) / 255.0f), srgbToLinear(qGreen(c) / 255.0f), srgbToLinear(qBlue(c) / 255.0f)};
            }
            glm::vec3 f0 = glm::mix(glm::vec3(0.04f), albedo, metal);
            float nl = std::max(glm::dot(nrm, l), 0.0f), nh = std::max(glm::dot(nrm, h), 0.0f);
            float a2 = rough * rough * rough * rough;
            float dd = nh * nh * (a2 - 1.0f) + 1.0f;
            float spec = a2 / (3.14159265f * dd * dd) * 0.25f;
            glm::vec3 c = (albedo * (1.0f - metal) / 3.14159265f + f0 * spec) * nl * 2.6f;
            c += (albedo * (1.0f - metal) + f0 * 0.5f) * (0.18f + 0.1f * nrm.y) + emissive;
            c = c / (c + glm::vec3(0.6f)) * 1.25f;
            float edge = std::clamp((1.0f - std::sqrt(d2)) * r, 0.0f, 1.0f);
            img.setPixel(x, y, qPremultiply(qRgba(int(linearToSrgb(c.r) * 255), int(linearToSrgb(c.g) * 255), int(linearToSrgb(c.b) * 255), int(edge * 255))));
        }
    return QPixmap::fromImage(img);
}

QPixmap previewBall(const scene::Node& n)
{
    return previewBall(glm::vec3(n.as<glm::dvec3>(kBaseColor)), float(n.as<double>(kMetallic)), float(n.as<double>(kRoughness)),
                       glm::vec3(n.as<glm::dvec3>(kEmissiveColor)) * float(n.as<double>(kEmissiveIntensity)),
                       textures::thumbnail(scene::materials::resolvePath(n.as<std::string>(kBaseColorMap)), 128));
}

QPixmap defaultPreviewBall()
{
    const render::Material m;
    return previewBall(m.baseColor, m.metallic, m.roughness, m.emissive, QImage());
}

constexpr const char* kHideReferenced = "/materialEditor/hideReferenced";
constexpr const char* kHideUnused = "/materialEditor/hideUnused";
constexpr const char* kSelectionOnly = "/materialEditor/selectionOnly";
// Bit masks of the shading models and blend modes shown; 0 shows all.
constexpr const char* kModelFilter = "/materialEditor/shadingModels";
constexpr const char* kBlendFilter = "/materialEditor/blendModes";

const QStringList& shadingModelLabels()
{
    static const QStringList labels{"Default Lit", "Painterly", "Toon", "Foliage", "Unlit", "Skin", "Cloth", "Clearcoat", "Hair", "Eye"};
    return labels;
}

const QStringList& blendModeLabels()
{
    static const QStringList labels{"Opaque", "Masked", "Translucent"};
    return labels;
}

bool isDefaultRow(const QListWidgetItem* item)
{
    return item && item->data(Qt::UserRole).toBool();
}

bool hasTextureMaps(const scene::Node& material)
{
    for (const auto& channel : scene::materials::textureChannels())
        if (!material.as<std::string>(channel.second).empty())
            return true;
    return false;
}

/** The selected rows' materials, in list order; the default row has none. */
std::vector<scene::Node*> selectedMaterials(const QListWidget* list)
{
    std::vector<scene::Node*> out;
    for (int i = 0; i < list->count(); ++i)
        if (QListWidgetItem* item = list->item(i); item->isSelected() && !isDefaultRow(item))
            if (scene::Node* n = scene::materials::find(scene::Graph::current(), item->text().toStdString()))
                out.push_back(n);
    return out;
}

json materialPaths(const std::vector<scene::Node*>& materials)
{
    json out = json::array();
    for (const scene::Node* n : materials)
        out.push_back(n->path());
    return out;
}

/** The materials the meshes use, faces on a slot with no material counted under the default slot. */
std::set<std::string> usedMaterials(scene::Graph& g, const std::vector<scene::NodeId>& meshes)
{
    std::set<std::string> out;
    for (scene::NodeId id : meshes)
    {
        scene::Node* n = g.node(id);
        if (!n || !n->type().isA("mesh"))
            continue;
        if (const scene::MeshPtr& mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
            for (const auto& [slot, faces] : scene::materials::slotUsage(*mesh))
                out.insert(scene::materials::find(g, slot) ? slot : std::string(scene::materials::kDefaultSlot));
    }
    return out;
}

/** The materials on the selected objects. */
std::vector<scene::Node*> materialsOnSelection()
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    std::vector<scene::Node*> out;
    for (const std::string& name : usedMaterials(g, {g.selection().begin(), g.selection().end()}))
        if (scene::Node* n = scene::materials::find(g, name); n && n->type().isA("material"))
            out.push_back(n);
    return out;
}

QPointer<MaterialEditor>& floatingEditor()
{
    static QPointer<MaterialEditor> e;
    return e;
}

MaterialEditor* showEditor()
{
    if (QDockWidget* d = findDock(kMaterialEditorDock))
    {
        revealDock(d);
        return qobject_cast<MaterialEditor*>(d->widget());
    }
    auto& e = floatingEditor();
    if (!e)
    {
        e = new MaterialEditor(QApplication::activeWindow());
        e->setWindowFlag(Qt::Tool);
        e->setWindowTitle("Material Editor");
        e->setAttribute(Qt::WA_DeleteOnClose);
        e->resize(475, 720);
    }
    e->show();
    e->raise();
    flashLocate(e);
    return e;
}
} // namespace

MaterialEditor::MaterialEditor(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);

    auto* top = new QHBoxLayout;
    auto* listColumn = new QVBoxLayout;
    mFilter = new QLineEdit(this);
    mFilter->setPlaceholderText("Filter");
    mFilter->setClearButtonEnabled(true);
    mFilter->setToolTip("Space or comma separated names; any may match. * ? [a-z] match the whole name, other terms any part of it.");
    auto* filterRow = new QHBoxLayout;
    filterRow->addWidget(mFilter, 1);
    mFilterMenu = new QToolButton(this);
    mFilterMenu->setIcon(icon("filter"));
    mFilterMenu->setAutoRaise(true);
    mFilterMenu->setCheckable(true);
    mFilterMenu->setPopupMode(QToolButton::InstantPopup);
    mFilterMenu->setToolTip("Filters: shading model, blend mode, referenced, unused, or only those on the selected objects.");
    auto* filterMenu = new QMenu(mFilterMenu);
    auto syncFilterButton = [this] {
        PrefStore& p = PrefStore::get();
        mFilterMenu->setChecked(p.value<bool>("general", kHideReferenced, false) || p.value<bool>("general", kHideUnused, false) ||
                                p.value<bool>("general", kSelectionOnly, false) || p.value<int64_t>("general", kModelFilter, 0) ||
                                p.value<int64_t>("general", kBlendFilter, 0));
    };
    // Each entry toggles one bit; with none checked every type shows.
    auto maskMenu = [&](const char* title, const char* key, const QStringList& labels) {
        QMenu* sub = filterMenu->addMenu(title);
        const int64_t mask = PrefStore::get().value<int64_t>("general", key, 0);
        for (int i = 0; i < labels.size(); ++i)
        {
            QAction* a = sub->addAction(labels[i]);
            a->setCheckable(true);
            a->setChecked(mask & (int64_t(1) << i));
            connect(a, &QAction::toggled, this, [this, key, i, syncFilterButton](bool on) {
                PrefStore& p = PrefStore::get();
                const int64_t bit = int64_t(1) << i, m = p.value<int64_t>("general", key, 0);
                p.set("general", key, on ? m | bit : m & ~bit);
                syncFilterButton();
                applyFilter();
            });
        }
    };
    maskMenu("Shading Model", kModelFilter, shadingModelLabels());
    maskMenu("Blend Mode", kBlendFilter, blendModeLabels());
    filterMenu->addSeparator();
    for (auto [label, key] : {std::pair{"Hide Referenced", kHideReferenced}, {"Hide Unused", kHideUnused}, {"Selection Only", kSelectionOnly}})
    {
        QAction* a = filterMenu->addAction(label);
        a->setCheckable(true);
        a->setChecked(PrefStore::get().value<bool>("general", key, false));
        connect(a, &QAction::toggled, this, [this, key, syncFilterButton](bool on) {
            PrefStore::get().set("general", key, on);
            syncFilterButton();
            applyFilter();
        });
    }
    mFilterMenu->setMenu(filterMenu);
    // A click opens the menu; the checked look only says whether any filter is on.
    connect(filterMenu, &QMenu::aboutToHide, this, syncFilterButton);
    syncFilterButton();
    filterRow->addWidget(mFilterMenu);
    listColumn->addLayout(filterRow);
    mList = new QListWidget(this);
    mList->setMaximumHeight(140);
    mList->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    mList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    listColumn->addWidget(mList, 1);
    top->addLayout(listColumn, 1);
    connect(mFilter, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    auto* buttons = new QVBoxLayout;
    auto iconButton = [this](const char* iconName, const QString& tip) {
        auto* b = new QPushButton(icon(iconName), QString(), this);
        b->setToolTip(tip);
        return b;
    };
    mAssign = iconButton("brush", "Assign: to the selected objects, or the selected faces in face mode.");
    auto* add = iconButton("plus", "New: create a material; with objects or faces selected, also assign it to them.");
    auto* del = mDelete = iconButton("trash", "Delete: the material.");
    auto* showAssigned = iconButton("target", "Show Assigned: the material assigned to the selected object.");
    auto* selectUsers = mSelectUsers =
        iconButton("pointer", "Select Users: what this material is assigned to, whole objects or just the faces where it covers part of a mesh.");
    for (QPushButton* b : {mAssign, add, del, showAssigned, selectUsers})
        buttons->addWidget(b);
    buttons->addStretch();
    top->addLayout(buttons);
    mPreview = new QLabel(this);
    mPreview->setFixedSize(kPreviewSize, kPreviewSize);
    top->addWidget(mPreview);
    root->addLayout(top);

    auto* scroll = mScroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mBody = new QWidget;
    mBodyLayout = new QVBoxLayout(mBody);
    mBodyLayout->setContentsMargins(0, 0, 0, 0);
    scroll->setWidget(mBody);
    root->addWidget(scroll, 1);
    auto dropByName = [this](const std::vector<std::string>& files) {
        if (scene::Node* m = current())
            assignByName(*m, files);
    };
    for (QWidget* w : {static_cast<QWidget*>(mPreview), mBody})
    {
        w->setAcceptDrops(true);
        w->installEventFilter(new TextureDrop(w, dropByName));
    }

    connect(add, &QPushButton::clicked, this, [this] {
        json name = createAndAssign(false);
        if (name.is_string())
            showMaterial(name.get<std::string>());
    });
    auto deleteMaterial = [this] {
        json paths = materialPaths(selectedMaterials(mList));
        if (paths.empty())
            if (scene::Node* n = current())
                paths.push_back(n->path());
        if (!paths.empty())
            runUi("delete", {{"args", paths}});
    };
    connect(del, &QPushButton::clicked, this, deleteMaterial);
    deletion::claim(mList, deleteMaterial);
    connect(mAssign, &QPushButton::clicked, this, [this] {
        if (std::string name = currentName(); !name.empty())
            runUi("assignMaterial", {{"args", json::array({name})}});
    });
    connect(showAssigned, &QPushButton::clicked, this, [this] { followSelection(); });
    connect(selectUsers, &QPushButton::clicked, this, [this] {
        if (std::string name = currentName(); !name.empty())
            runUi("selectByMaterial", {{"args", json::array({name})}});
    });
    mList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(mList, &QListWidget::customContextMenuRequested, this, [this, deleteMaterial](const QPoint& at) {
        QListWidgetItem* item = mList->itemAt(at);
        if (!item)
            return;
        if (!item->isSelected())
            mList->setCurrentItem(item);
        const std::vector<scene::Node*> picked = selectedMaterials(mList);
        if (picked.empty())
            return;
        const bool one = picked.size() == 1;
        const std::string path = picked.front()->path();
        QMenu menu(this);
        menu.addAction(icon("brush"), "Assign", [path] { runUi("assignMaterial", {{"args", json::array({path})}}); })->setEnabled(one);
        menu.addAction(icon("pointer"), "Select Users", [path] { runUi("selectByMaterial", {{"args", json::array({path})}}); })
            ->setEnabled(one);
        menu.addSeparator();
        QAction* clear = menu.addAction(icon("photo-off"), one ? QString("Clear Texture Maps") : QString("Clear Texture Maps (%1)").arg(picked.size()),
                                        [paths = materialPaths(picked)] { runUi("clearMaterialTextures", {{"args", paths}}); });
        clear->setEnabled(std::any_of(picked.begin(), picked.end(), [](const scene::Node* n) { return hasTextureMaps(*n); }));
        menu.addSeparator();
        menu.addAction(icon("pencil"), "Rename", [this, item] { mList->editItem(item); })->setEnabled(one);
        menu.addAction(icon("trash"), one ? QString("Delete") : QString("Delete (%1)").arg(picked.size()), deleteMaterial);
        menu.exec(mList->viewport()->mapToGlobal(at));
    });
    connect(mList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) { pickRow(item); });
    connect(mList, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (isDefaultRow(item))
            return;
        scene::Node* n = current();
        std::string wanted = item->text().toStdString();
        if (n && wanted != n->name())
            runUi("renameMaterial", {{"args", json::array({n->path(), wanted})}});
        scheduleRefresh();
    });

    mListener = scene::Graph::current().addListener([this](scene::Change c, scene::NodeId) {
        if (c == scene::Change::Selection)
            QTimer::singleShot(0, this, [this] { followSelection(); });
        scheduleRefresh();
    });
    mTextureListener = textures::addListener([this] { scheduleRefresh(); });
    refreshList();
    fitWidth();
}

MaterialEditor::~MaterialEditor()
{
    scene::Graph::current().removeListener(mListener);
    textures::removeListener(mTextureListener);
}

scene::Node* MaterialEditor::current() const
{
    scene::Node* n = scene::Graph::current().node(mNode);
    return n && n->type().isA("material") ? n : nullptr;
}

std::string MaterialEditor::currentName() const
{
    if (scene::Node* n = current())
        return n->path();
    return mDefaultShown ? std::string(scene::materials::kDefaultSlot) : std::string();
}

void MaterialEditor::pickRow(QListWidgetItem* item)
{
    const bool def = isDefaultRow(item);
    scene::Node* n = item && !def ? scene::materials::find(scene::Graph::current(), item->text().toStdString()) : nullptr;
    scene::NodeId id = n ? n->id() : scene::kNoNode;
    if (id != mNode || def != mDefaultShown)
    {
        mNode = id;
        mDefaultShown = def;
        rebuild();
    }
}

void MaterialEditor::showMaterial(const std::string& name)
{
    refreshList();
    auto items = mList->findItems(QString::fromStdString(name), Qt::MatchExactly);
    if (!items.isEmpty())
        mList->setCurrentItem(items.front());
}

void MaterialEditor::followSelection()
{
    scene::Graph& g = scene::Graph::current();
    if (g.selection().empty())
        return;
    scene::Node* n = g.node(g.selection().back());
    if (!n || !n->type().isA("mesh"))
        return;
    g.evaluate();
    const scene::MeshPtr& mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    if (!mesh)
        return;
    bool unmapped = false;
    for (const auto& [slot, faces] : scene::materials::slotUsage(*mesh))
    {
        if (scene::materials::find(g, slot))
        {
            showMaterial(slot);
            return;
        }
        unmapped = true;
    }
    if (unmapped)
        showMaterial(scene::materials::kDefaultSlot);
}

void MaterialEditor::scheduleRefresh()
{
    if (mQueued)
        return;
    mQueued = true;
    // While a drag edits the scene every frame, the fields catch up ten times a second instead of every frame.
    QTimer::singleShot(QGuiApplication::mouseButtons() != Qt::NoButton ? 100 : 0, this, [this] {
        mQueued = false;
        refreshList();
        if (!current() && mNode != scene::kNoNode)
        {
            mNode = scene::kNoNode;
            rebuild();
        }
        updateValues();
    });
}

void MaterialEditor::refreshList()
{
    scene::Graph& g = scene::Graph::current();
    std::vector<std::string> names;
    for (scene::Node* n : scene::materials::all(g))
        names.push_back(n->name());
    std::sort(names.begin(), names.end());
    // The built-in default heads the list, unless a material of that name has taken its slot.
    const bool defaultRow = !scene::materials::find(g, scene::materials::kDefaultSlot);
    if (defaultRow)
        names.insert(names.begin(), scene::materials::kDefaultSlot);
    bool same = int(names.size()) == mList->count() && (!defaultRow || isDefaultRow(mList->item(0)));
    for (int i = 0; same && i < mList->count(); ++i)
        same = mList->item(i)->text().toStdString() == names[size_t(i)];
    if (same)
    {
        applyFilter();
        return;
    }
    const bool keepDefault = mDefaultShown && defaultRow;
    const std::string keep = current() ? current()->name() : std::string();
    {
        QSignalBlocker block(mList);
        mList->clear();
        for (size_t i = 0; i < names.size(); ++i)
        {
            auto* item = new QListWidgetItem(QString::fromStdString(names[i]), mList);
            const bool def = defaultRow && i == 0;
            if (def)
            {
                item->setData(Qt::UserRole, true);
                item->setToolTip("Built-in neutral material, on faces with no other. Read-only.");
                item->setForeground(mList->palette().color(QPalette::Disabled, QPalette::Text));
            }
            else
                item->setFlags(item->flags() | Qt::ItemIsEditable);
            if (def ? keepDefault : !keep.empty() && names[i] == keep)
                mList->setCurrentItem(item);
        }
        if (!mList->currentItem() && keep.empty() && !keepDefault && mList->count())
            mList->setCurrentRow(defaultRow && mList->count() > 1 ? 1 : 0);
    }
    pickRow(mList->currentItem());
    applyFilter();
}

void MaterialEditor::applyFilter()
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    PrefStore& p = PrefStore::get();
    const bool hideReferenced = p.value<bool>("general", kHideReferenced, false);
    const int64_t models = p.value<int64_t>("general", kModelFilter, 0), blends = p.value<int64_t>("general", kBlendFilter, 0);
    std::optional<std::set<std::string>> used, onSelection;
    const bool usage = p.value<bool>("general", kHideUnused, false) || p.value<bool>("general", kSelectionOnly, false);
    if (usage)
        g.evaluate();
    if (p.value<bool>("general", kHideUnused, false))
        used = usedMaterials(g, g.nodes());
    if (p.value<bool>("general", kSelectionOnly, false))
        onSelection = usedMaterials(g, {g.selection().begin(), g.selection().end()});
    static const QRegularExpression separators(R"([\s,]+)");
    const QStringList terms = mFilter->text().split(separators, Qt::SkipEmptyParts);
    auto nameMatches = [&](const QListWidgetItem* item) {
        for (const QString& term : terms)
        {
            const bool glob = term.contains('*') || term.contains('?') || term.contains('[');
            if (glob ? outliner::globMatch(term.toStdString(), item->text().toStdString()) : item->text().contains(term, Qt::CaseInsensitive))
                return true;
        }
        return terms.isEmpty();
    };
    for (int i = 0; i < mList->count(); ++i)
    {
        QListWidgetItem* item = mList->item(i);
        const std::string name = item->text().toStdString();
        bool hide = !nameMatches(item);
        // The built-in default is Default Lit and opaque.
        scene::Node* n = isDefaultRow(item) ? nullptr : scene::materials::find(g, name);
        const int64_t model = n ? n->as<int64_t>(kShadingModel) : 0, blend = n ? n->as<int64_t>(kBlendMode) : 0;
        hide = hide || (hideReferenced && n && n->referenceId() != 0);
        hide = hide || (models && !(models & (int64_t(1) << model))) || (blends && !(blends & (int64_t(1) << blend)));
        hide = hide || (used && !used->count(name)) || (onSelection && !onSelection->count(name));
        item->setHidden(hide);
    }
}

void MaterialEditor::setAttr(int attr, const json& value)
{
    scene::Node* n = current();
    if (!n)
        return;
    runUi("setAttr", {{"args", json::array({n->path() + "." + n->type().attrs[size_t(attr)].name, value})}});
}

void MaterialEditor::rebuild()
{
    LILA_ZONE();

    mUpdaters.clear();
    while (QLayoutItem* item = mBodyLayout->takeAt(0))
    {
        if (QWidget* w = item->widget())
            w->deleteLater();
        delete item;
    }
    scene::Node* n = current();
    mAssign->setEnabled(n || mDefaultShown);
    mSelectUsers->setEnabled(n || mDefaultShown);
    mDelete->setEnabled(n != nullptr);
    if (!n && mDefaultShown)
    {
        mPreview->setPixmap(defaultPreviewBall());
        auto* hint = new QLabel("Built-in neutral material. Read-only.", mBody);
        mBodyLayout->addWidget(hint);
        mBodyLayout->addStretch();
        return;
    }
    if (!n)
    {
        mPreview->clear();
        auto* hint = new QLabel("No material. Click + to create one, or pick one in the list.", mBody);
        hint->setForegroundRole(QPalette::PlaceholderText);
        hint->setWordWrap(true);
        mBodyLayout->addWidget(hint);
        mBodyLayout->addStretch();
        return;
    }

    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    int row = 0;
    auto label = [&](const char* text) {
        auto* l = new QLabel(text, mBody);
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(l, row, 0);
        return l;
    };
    // A middle click on the row's label restores the attribute's default.
    auto resettable = [&](QWidget* l, int attr, QWidget* field) {
        const scene::AttrDef& def = n->type().attrs[size_t(attr)];
        if (std::optional<json> d = valueReset::plugDefault(def))
            onMiddleClickReset(l, [this, attr, v = *d] { setAttr(attr, v); },
                               QString::fromStdString(valueReset::describe(*d, def.enumNames)), field);
    };
    auto section = [&](const char* text) {
        auto* l = new QLabel(text, mBody);
        QFont f = l->font();
        f.setWeight(QFont::DemiBold);
        l->setFont(f);
        l->setContentsMargins(0, row ? 8 : 0, 0, 2);
        grid->addWidget(l, row++, 0, 1, 3);
    };

    auto color = [&](const char* text, int attr) {
        QLabel* l = label(text);
        auto* b = new QToolButton(mBody);
        b->setIconSize(QSize(36, 18));
        resettable(l, attr, b);
        auto* value = new colorPopup::ValueSlider(mBody);
        value->dragged = [this, attr](const glm::dvec3& c) {
            mDragging = true;
            if (scene::Node* m = current())
            {
                UndoStack::get().setEnabled(false);
                scene::Graph::current().setValue({m->id(), attr, -1}, c);
                UndoStack::get().setEnabled(true);
            }
        };
        value->released = [this, attr](const glm::dvec3& start, const glm::dvec3& c) {
            mDragging = false;
            if (scene::Node* m = current())
            {
                UndoStack::get().setEnabled(false);
                scene::Graph::current().setValue({m->id(), attr, -1}, start);
                UndoStack::get().setEnabled(true);
                if (c != start)
                    setAttr(attr, json::array({c.x, c.y, c.z}));
            }
        };
        // Column 1 only, so the slider ends where the number sliders' tracks do.
        grid->addWidget(colorPopup::withEyedropper(b, [this, attr](const glm::dvec3& c) { setAttr(attr, json::array({c.x, c.y, c.z})); }, value),
                        row++, 1);
        connect(b, &QToolButton::clicked, this, [this, b, attr, text] {
            scene::Node* m = current();
            if (!m)
                return;
            const scene::NodeId id = m->id();
            const glm::dvec3 start = m->as<glm::dvec3>(attr);
            // Preview without undo while the picker moves; OK commits once, as one undo step, Cancel puts the start back.
            auto preview = [id, attr](const glm::dvec3& v) {
                scene::Graph& g = scene::Graph::current();
                if (!g.node(id))
                    return;
                UndoStack::get().setEnabled(false);
                g.setValue({id, attr, -1}, v);
                UndoStack::get().setEnabled(true);
            };
            colorPopup::Options o;
            o.value.color = start;
            o.title = text;
            o.changed = [preview](const colorPopup::Value& v) { preview(v.color); };
            const std::optional<colorPopup::Value> picked = colorPopup::exec(colorPopup::placeFor(b), std::move(o));
            preview(start);
            if (picked && current() && current()->id() == id && picked->color != start)
                setAttr(attr, json::array({picked->color.x, picked->color.y, picked->color.z}));
        });
        mUpdaters.push_back([this, b, value, attr] {
            if (scene::Node* m = current())
            {
                const glm::dvec3 c = m->as<glm::dvec3>(attr);
                b->setIcon(swatchIcon(displayColor(c)));
                value->setColor(c);
            }
        });
    };

    // A bounded number as a slider field: live without undo while it moves, one undo step once it settles.
    auto slider = [&](const char* text, int attr, double lo, double hi, const QString& unit = {}) {
        QLabel* l = label(text);
        auto* s = new controls::SliderField(mBody);
        s->setRange(lo, hi);
        s->setDecimals(3);
        s->setSingleStep((hi - lo) / 100.0);
        if (!unit.isEmpty())
            s->setSuffix(" " + unit);
        resettable(l, attr, s);
        if (std::optional<json> d = valueReset::plugDefault(n->type().attrs[size_t(attr)]); d && d->is_number())
            setSpinDefault(s, d->get<double>());
        l->setBuddy(s);
        grid->addWidget(s, row++, 1, 1, 2);
        auto* commit = new QTimer(s);
        commit->setSingleShot(true);
        commit->setInterval(300);
        connect(s, &QDoubleSpinBox::valueChanged, this, [this, s, attr, commit](double v) {
            scene::Node* m = current();
            if (!m)
                return;
            if (!commit->isActive())
                s->setProperty("start", m->as<double>(attr));
            mDragging = true;
            UndoStack::get().setEnabled(false);
            scene::Graph::current().setValue({m->id(), attr, -1}, v);
            UndoStack::get().setEnabled(true);
            commit->start();
        });
        connect(commit, &QTimer::timeout, this, [this, s, attr] {
            mDragging = false;
            if (scene::Node* m = current())
            {
                const double v = s->value();
                UndoStack::get().setEnabled(false);
                scene::Graph::current().setValue({m->id(), attr, -1}, s->property("start").toDouble());
                UndoStack::get().setEnabled(true);
                setAttr(attr, v);
            }
        });
        mUpdaters.push_back([this, s, attr, lo, hi, commit] {
            scene::Node* m = current();
            if (!m || commit->isActive())
                return;
            const double v = m->as<double>(attr);
            QSignalBlocker block(s);
            // A value set past the range from a script widens it rather than being clamped here.
            s->setRange(std::min(lo, v), std::max(hi, v));
            s->setValue(v);
        });
        return s;
    };

    // Items show `labels` when given (one per enum name), else the stored enum names.
    auto combo = [&](const char* text, int attr, QStringList labels = {}, const char* tip = nullptr) {
        QLabel* l = label(text);
        auto* c = new QComboBox(mBody);
        // Long enum labels would otherwise set the panel's minimum width.
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        c->setMinimumContentsLength(8);
        if (tip)
            c->setToolTip(tip);
        resettable(l, attr, c);
        resettable(c, attr, c);
        const auto& names = n->type().attrs[size_t(attr)].enumNames;
        for (size_t i = 0; i < names.size(); ++i)
            c->addItem(i < size_t(labels.size()) ? labels[qsizetype(i)] : QString::fromStdString(names[i]));
        choiceTips::apply(c, choiceTips::forAttr(n->type().attrs[size_t(attr)]));
        grid->addWidget(c, row++, 1, 1, 2);
        connect(c, &QComboBox::activated, this, [this, attr](int i) { setAttr(attr, i); });
        mUpdaters.push_back([this, c, attr] {
            if (scene::Node* m = current())
            {
                QSignalBlocker block(c);
                c->setCurrentIndex(int(m->as<int64_t>(attr)));
                choiceTips::refresh(c);
            }
        });
    };

    auto texture = [&](const char* text, const char* channel, int attr) {
        QLabel* l = label(text);
        auto* box = new ImagePicker(nullptr, text, mBody);
        grid->addWidget(box, row++, 1, 1, 2);
        auto apply = [this, channel](const std::string& p) {
            if (scene::Node* m = current())
                runUi("setMaterialTexture", {{"args", json::array({m->path()})}, {"channel", channel}, {"path", p}});
        };
        box->setResolver([](const std::string& p) { return paths::fromUtf8(scene::materials::resolvePath(p)); });
        box->setProblem([](const std::filesystem::path& f) { return textures::error(paths::toUtf8(f)); });
        // One file goes in this slot whatever its name; several are sorted into slots by name.
        box->setDropMany([this](const std::vector<std::string>& files) {
            if (scene::Node* m = current())
                assignByName(*m, files);
        });
        connect(box, &ImagePicker::chosen, this, [this, apply, attr](const QString& v) {
            scene::Node* m = current();
            if (m && v.toStdString() != m->as<std::string>(attr))
                apply(v.toStdString());
        });
        onMiddleClickReset(l, [apply] { apply(""); }, "none", box);
        if (const int first = scene::materials::samplingAttr(attr); first >= 0)
        {
            // Wrap U, wrap V, filter and colour space on a row of their own, shown while the map is set.
            auto* opts = new QWidget(mBody);
            auto* oh = new QHBoxLayout(opts);
            oh->setContentsMargins(kPreviewThumb + 6, 0, 0, 0);
            const char* tips[4] = {"What the map shows past its edges along U.", "What the map shows past its edges along V.",
                                   "Linear: smooth, mipmapped. Nearest: hard texels, for pixel art.",
                                   "How the file's values are read. Auto: sRGB for colour maps, linear for data maps. EXR and HDR "
                                   "are always linear."};
            const QString autoSpace = scene::materials::colorMap(attr) ? "Auto (sRGB)" : "Auto (Linear)";
            for (int k = 0; k < 4; ++k)
            {
                auto* c = new QComboBox(opts);
                c->setToolTip(tips[k]);
                c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
                c->setMinimumContentsLength(5);
                const QStringList labels = k < 2    ? QStringList{"Repeat", "Clamp", "Mirror", "Black"}
                                           : k == 2 ? QStringList{"Linear", "Nearest"}
                                                    : QStringList{autoSpace, "sRGB", "Linear"};
                for (const QString& s : labels)
                    c->addItem(k == 0 ? "U: " + s : k == 1 ? "V: " + s : s);
                oh->addWidget(c, 1);
                resettable(c, first + k, c);
                connect(c, &QComboBox::activated, this, [this, a = first + k](int i) { setAttr(a, i); });
                mUpdaters.push_back([this, c, a = first + k] {
                    if (scene::Node* m = current())
                    {
                        QSignalBlocker block(c);
                        c->setCurrentIndex(int(m->as<int64_t>(a)));
                    }
                });
            }
            grid->addWidget(opts, row++, 1, 1, 2);
            mUpdaters.push_back([this, opts, attr] {
                if (scene::Node* m = current())
                    opts->setVisible(!m->as<std::string>(attr).empty());
            });
        }
        mUpdaters.push_back([this, box, attr] {
            if (scene::Node* m = current())
                box->setValue(m->as<std::string>(attr));
        });
    };

    auto check = [&](const char* text, int attr) -> QCheckBox* {
        auto* c = new QCheckBox(text, mBody);
        grid->addWidget(c, row++, 1, 1, 2);
        resettable(c, attr, c);
        connect(c, &QCheckBox::toggled, this, [this, attr](bool on) { setAttr(attr, on); });
        mUpdaters.push_back([this, c, attr] {
            if (scene::Node* m = current())
            {
                QSignalBlocker block(c);
                c->setChecked(m->as<bool>(attr));
            }
        });
        return c;
    };

    auto intSpin = [&](const char* text, int attr, int lo, int hi) {
        QLabel* l = label(text);
        auto* spin = new QSpinBox(mBody);
        spin->setRange(lo, hi);
        spin->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        resettable(l, attr, spin);
        if (std::optional<json> d = valueReset::plugDefault(n->type().attrs[size_t(attr)]))
            setSpinDefault(spin, d->get<double>());
        grid->addWidget(spin, row++, 1, 1, 2, Qt::AlignLeft);
        connect(spin, &QSpinBox::valueChanged, this, [this, attr](int v) { setAttr(attr, v); });
        mUpdaters.push_back([this, spin, attr] {
            if (scene::Node* m = current())
            {
                QSignalBlocker block(spin);
                spin->setValue(int(m->as<int64_t>(attr)));
            }
        });
    };

    section("Shading");
    combo("Shading Model", kShadingModel, shadingModelLabels(),
          "Default Lit: Unreal's default lit.\nPainterly: brush strokes bend the normal and a palette recolours light and shadow.\n"
          "Toon: stepped light and shadow with a rim.\nFoliage: soft gradient shading lit through from behind.\n"
          "Unlit: the base colour as it is, plus emissive, with no lighting.\nSkin: light scattering under the surface.\n"
          "Cloth: a velvet sheen at grazing angles.\nClearcoat: a glossy layer over the base.\n"
          "Hair: highlights stretched across the strands, for hair cards.\nEye: an iris under a glossy cornea (UVs centred on the pupil).");
    mShownModel = int(n->as<int64_t>(kShadingModel));
    // Each model shows only its own inputs, so switching the model rebuilds the panel.
    mUpdaters.push_back([this] {
        scene::Node* m = current();
        if (m && int(m->as<int64_t>(kShadingModel)) != mShownModel)
            QTimer::singleShot(0, this, [this] { rebuild(); });
    });
    switch (ShadingModel(mShownModel))
    {
    case ShadingModel::Painterly:
        slider("Strength", kPainterlyStrength, 0.0, 1.0);
        texture("Stroke Sheet", "stroke", kStrokeMap);
        combo("Mapping", kStrokeMapping, {"Object", "World", "Screen", "UV"},
              "Object: triplanar in object space, strokes move with the object.\nWorld: triplanar in world space.\n"
              "Screen: strokes keep their size on screen at any distance.\nUV: along the UVs, for baking.");
        slider("Stroke Normal", kStrokeNormal, 0.0, 1.0);
        slider("Stroke Scale", kStrokeScale, 1.0, 200.0);
        slider("Layer Ratio", kStrokeLayerRatio, 1.0, 8.0);
        intSpin("Palette Colors", kPaletteCount, 2, 4);
        color("Shadow Color", kPaletteShadow);
        color("Light Color", kPaletteLight);
        color("Accent Color", kPaletteAccent);
        slider("Contrast", kPaletteContrast, 0.0, 1.0);
        slider("Stroke Spread", kPaletteSpread, 0.0, 1.0);
        slider("Specular Tint", kSpecularTint, 0.0, 1.0);
        check("Boil", kBoil);
        slider("Boil FPS", kBoilFps, 1.0, 24.0);
        break;
    case ShadingModel::Toon:
        color("Light Tint", kToonLightTint);
        color("Shadow Color", kToonShadowColor);
        slider("Threshold", kToonThreshold, -1.0, 1.0);
        slider("Softness", kToonSoftness, 0.0, 0.5);
        slider("Outer Band", kToonOuterOffset, 0.0, 1.0);
        color("Outer Color", kToonOuterColor);
        color("Rim Color", kRimColor);
        slider("Rim Width", kRimWidth, 0.0, 1.0);
        slider("Rim Strength", kRimStrength, 0.0, 2.0);
        break;
    case ShadingModel::Foliage:
        color("Bottom Color", kFoliageBottomColor);
        color("Top Color", kFoliageTopColor);
        slider("Gradient", kFoliageGradient, 0.0, 1.0);
        slider("Bottom Shadow", kFoliageBottomShadow, 0.0, 1.0);
        slider("Top Highlight", kFoliageTopHighlight, 0.0, 1.0);
        slider("Translucency", kFoliageTranslucency, 0.0, 1.0);
        slider("Wrap", kFoliageWrap, 0.0, 1.0);
        break;
    case ShadingModel::Skin:
        color("Subsurface Color", kSubsurfaceColor);
        slider("Subsurface", kSubsurfaceAmount, 0.0, 1.0);
        break;
    case ShadingModel::Cloth:
        color("Sheen Color", kSheenColor);
        slider("Sheen Roughness", kSheenRoughness, 0.0, 1.0);
        break;
    case ShadingModel::Clearcoat:
        slider("Clearcoat", kClearcoat, 0.0, 1.0);
        slider("Clearcoat Roughness", kClearcoatRoughness, 0.0, 1.0);
        break;
    case ShadingModel::Hair:
        slider("Anisotropy", kAnisotropy, 0.0, 1.0);
        slider("Strand Rotation", kAnisotropyRotation, -180.0, 180.0, QString(QChar(0x00B0)));
        slider("Highlight Shift", kHairShift, 0.0, 1.0);
        break;
    case ShadingModel::Eye:
        slider("Iris Radius", kIrisRadius, 0.0, 0.5);
        slider("Iris Depth", kIrisDepth, 0.0, 1.0);
        slider("Limbal Ring", kLimbalRing, 0.0, 1.0);
        slider("Cornea", kClearcoat, 0.0, 1.0);
        slider("Cornea Roughness", kClearcoatRoughness, 0.0, 1.0);
        break;
    default: break;
    }

    section("Surface");
    color("Base Color", kBaseColor);
    texture("Base Color Map", "baseColor", kBaseColorMap);
    slider("Metallic", kMetallic, 0.0, 1.0);
    texture("Metallic Map", "metallic", kMetallicMap);
    slider("Specular", kSpecular, 0.0, 1.0);
    slider("Roughness", kRoughness, 0.0, 1.0);
    texture("Roughness Map", "roughness", kRoughnessMap);
    slider("Bevel", kBevelRadius, 0.0, 5.0, "cm");
    texture("Occlusion Map", "ambientOcclusion", kAmbientOcclusionMap);
    texture("Packed Map", "orm", kOrmMap);
    combo("Packed Layout", kPackedLayout, {"ORM", "MRO"},
          "ORM: R occlusion, G roughness, B metallic (Unreal's packing).\n"
          "MRO: R metallic, G roughness, B occlusion.\n"
          "CRM maps (cavity in R) use ORM: the channels are laid out the same.");
    texture("Normal Map", "normal", kNormalMap);
    auto* flip = new QCheckBox("Flip Green", mBody);
    flip->setToolTip("The normal map is OpenGL style (green up: Blender, Substance's OpenGL preset). Off is DirectX "
                     "style (green down), Unreal's convention. Sent to Unreal as the texture's Flip Green Channel.");
    grid->addWidget(flip, row++, 1, 1, 2);
    resettable(flip, kNormalFormat, flip);
    connect(flip, &QCheckBox::toggled, this, [this](bool on) { setAttr(kNormalFormat, on ? 1 : 0); });
    mUpdaters.push_back([this, flip] {
        if (scene::Node* m = current())
        {
            QSignalBlocker block(flip);
            flip->setChecked(NormalFormat(m->as<int64_t>(kNormalFormat)) == NormalFormat::OpenGL);
        }
    });
    texture("Height Map", "height", kHeightMap);
    QCheckBox* fromHeight = check("Normals From Height", kNormalFromHeight);
    std::vector<QWidget*> heightFields = {slider("Height Depth", kHeightDepth, 0.0, 20.0, "cm"),
                                          slider("UV Width", kHeightUvWidth, 10.0, 1000.0, "cm"),
                                          slider("UV Height", kHeightUvHeight, 10.0, 1000.0, "cm")};
    mUpdaters.push_back([fromHeight, heightFields] {
        for (QWidget* f : heightFields)
            f->setEnabled(fromHeight->isChecked());
    });
    section("Emissive");
    color("Emissive Color", kEmissiveColor);
    texture("Emissive Map", "emissive", kEmissiveMap);
    slider("Intensity", kEmissiveIntensity, 0.0, 20.0);
    section("Blending");
    combo("Blend Mode", kBlendMode);
    slider("Opacity", kOpacity, 0.0, 1.0);
    slider("Mask Clip", kOpacityMaskClip, 0.0, 1.0);
    check("Two Sided", kTwoSided);
    QLabel* uvLabel = label("UV Set");
    auto* uv = new QSpinBox(mBody);
    uv->setRange(0, 1);
    uv->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    uv->setButtonSymbols(QAbstractSpinBox::NoButtons);
    resettable(uvLabel, kUvSet, uv);
    if (std::optional<json> d = valueReset::plugDefault(n->type().attrs[size_t(kUvSet)]))
        setSpinDefault(uv, d->get<double>());
    grid->addWidget(uv, row++, 1, 1, 2, Qt::AlignLeft);
    connect(uv, &QSpinBox::valueChanged, this, [this](int v) { setAttr(kUvSet, v); });
    mUpdaters.push_back([this, uv] {
        if (scene::Node* m = current())
        {
            QSignalBlocker block(uv);
            uv->setValue(int(m->as<int64_t>(kUvSet)));
        }
    });

    auto* gridHost = new QWidget(mBody);
    gridHost->setLayout(grid);
    mBodyLayout->addWidget(gridHost);
    mBodyLayout->addStretch();
    updateValues();
}

void MaterialEditor::updateValues()
{
    scene::Node* n = current();
    if (!n)
        return;
    for (auto& fn : mUpdaters)
        fn();
    mPreview->setPixmap(previewBall(*n));
    // Rows just added are shown on the next pass of the event loop; until then they take no room.
    QTimer::singleShot(0, this, [this] { fitWidth(); });
}

void MaterialEditor::fitWidth()
{
    // Set on the scroll area, not the panel: the dock code relaxes the panel's own minimum while its tab is hidden.
    const int rows = mBody->minimumSizeHint().width() + mScroll->verticalScrollBar()->sizeHint().width();
    mScroll->setMinimumWidth(std::max(rows, kPreviewSize + fontMetrics().averageCharWidth() * 30));
}

void registerMaterialUi()
{
    scene::materials::registerTypes();
    if (!ops::find("createMaterial"))
        scene::materials::registerCommands();

    actions::add({.id = "window.materialEditor", .label = "Material Editor", .category = "Windows", .run = [] {
                      if (QDockWidget* d = findDock(kMaterialEditorDock))
                          toggleDock(d);
                      else if (auto& e = floatingEditor(); e && e->isVisible())
                          e->close();
                      else
                          showEditor();
                  },
                  .doc = "Create and edit materials and assign them to the selection.",
                  .python = "lila.ui.run_action(\"window.materialEditor\")"});
    actions::add({.id = "material.assignNew", .label = "Assign New Material", .category = "Materials", .run = [] {
                      json name = createAndAssign(true);
                      if (!name.is_string())
                          return;
                      if (MaterialEditor* e = showEditor())
                          e->showMaterial(name.get<std::string>());
                  },
                  .doc = "Create a material and assign it to the selected objects or faces.",
                  .python = "assignMaterial(createMaterial('M_Material'))"});
    actions::add({.id = "material.clearTextures", .label = "Clear Texture Maps", .category = "Materials",
                  .run =
                      [] {
                          json paths = materialPaths(materialsOnSelection());
                          if (paths.empty())
                              log::warn("Clear Texture Maps: the selection has no materials");
                          else
                              runUi("clearMaterialTextures", {{"args", paths}});
                      },
                  .enabled = actions::hasSelection,
                  .doc = "Clear every texture map on the materials of the selected objects.",
                  .python = "clearMaterialTextures('M_Material', 'M_Other')",
                  .icon = "photo-off",
                  .keywords = {"remove textures", "strip textures", "unassign textures"}});
    actions::add({.id = "material.assignDefault", .label = "Assign Default Material", .category = "Materials",
                  .run = [] { runUi("assignMaterial", {{"args", json::array({scene::materials::kDefaultSlot})}}); },
                  .enabled = actions::hasSelection,
                  .doc = "Put the selected objects or faces back on the neutral default material.",
                  .python = "assignMaterial('Default')"});
    if (!ops::find("editMaterial"))
        ops::registerCommand({
            .name = "editMaterial",
            .doc = "Show a material in the Material Editor, without assigning it.",
            .positionalDoc = "material",
            .undoable = false,
            .run = [](const Args& a) -> json {
                if (a.positional.empty() || !a.positional[0].is_string())
                    throw CommandError("editMaterial: give a material name");
                std::string name = a.positional[0].get<std::string>();
                if (!scene::materials::find(scene::Graph::current(), name))
                    throw CommandError("editMaterial: " + name + " is not a material");
                if (MaterialEditor* e = showEditor())
                    e->showMaterial(name);
                return name;
            },
        });
    actions::add({.id = "material.textureFolders", .label = "Texture Folders...", .category = "Materials",
                  .run = [] { texturePicker::showFoldersDialog(QApplication::activeWindow()); },
                  .doc = "Choose which folders of the project the texture lists show, for the workspace and this scene.",
                  .python = "textureFolders(include=['Textures/**'], exclude=['**/*_old.*'])"});
    actions::add({.id = "viewport.textured", .label = "Textured", .category = "Display",
                  .run =
                      [] {
                          PrefStore::get().set("general", "/viewport/displayMode", "shaded");
                          setMaterialsShown(true);
                      },
                  .doc = "Draws meshes shaded with their materials and textures.",
                  .python = "lila.prefs.set(\"general\", \"/viewport/displayMode\", \"shaded\")\n"
                            "lila.prefs.set(\"general\", \"/viewport/materials\", True)"});
}
} // namespace lila::ui
