#include "Tool.h"

#include <lila/Components.h>
#include <lila/Controls.h>
#include <lila/Deformer.h>
#include <lila/Undo.h>
#include <lila/FileDialog.h>
#include <lila/Icons.h>
#include <lila/Log.h>
#include <lila/MaterialThumbnails.h>
#include <lila/Prefs.h>
#include <lila/UI.h>
#include <lila/ViewportTool.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <map>

namespace blockwork
{
using namespace lila;
using namespace lila::ui;
using namespace lila::scene;

namespace
{
constexpr const char* kFolderPref = "/blockwork/browser/folder";
constexpr const char* kFavouritesPref = "/blockwork/browser/favourites";
constexpr const char* kSceneEntry = "Scene Materials";

/**
 * Materials worth painting faces with: Weathering's own (its placeholder pieces' PiecePlaceholder_ materials and the
 * M_Weathering glass, interior and decal ones it makes) are left out.
 */
bool paintable(const Node& material)
{
    const std::string& n = material.name();
    return n.rfind("PiecePlaceholder_", 0) != 0 && n.rfind("M_Weathering", 0) != 0;
}
constexpr int kThumb = 64;

/** The faces the panel acts on: the selected faces of blocks, else every face of the selected blocks. */
struct Targets
{
    std::map<NodeId, std::vector<uint32_t>> faces;
    size_t count = 0;
};

Targets targets()
{
    Graph& g = graph();
    Targets t;
    for (NodeId id : g.selection())
    {
        Node* n = g.node(id);
        if (!n || !isBlock(*n))
            continue;
        std::vector<uint32_t> f = components::selected(id, ComponentMode::Face);
        if (f.empty())
            if (const MeshPtr& m = n->as<MeshPtr>(MeshNode::kOutMesh))
                for (geo::Index i = 0; i < m->faceCapacity(); ++i)
                    if (m->faceValid(i))
                        f.push_back(i);
        t.count += f.size();
        t.faces[id] = std::move(f);
    }
    return t;
}

void runOnTargets(const char* cmd, json call)
{
    const Targets t = targets();
    if (t.faces.empty())
    {
        log::warn("{}: select blocks or their faces", cmd);
        return;
    }
    try
    {
        UndoChunk chunk(cmd);
        for (const auto& [id, faces] : t.faces)
        {
            json c = call;
            c["args"] = {graph().fullPath(id)};
            c["faces"] = faces;
            ops::execute(cmd, c, CallSource::UI);
        }
    }
    catch (const std::exception& e)
    {
        log::warn("{}", e.what());
    }
}

/** A flat tile of a linear colour, for materials with no image to show. */
QIcon swatch(const glm::dvec3& linear)
{
    QPixmap px(kThumb, kThumb);
    auto srgb = [](double c) { return int(std::lround(255.0 * std::pow(std::clamp(c, 0.0, 1.0), 1.0 / 2.2))); };
    px.fill(QColor(srgb(linear.r), srgb(linear.g), srgb(linear.b)));
    return QIcon(px);
}

QPixmap thumbnail(const QString& file)
{
    static std::map<QString, QPixmap> cache;
    if (auto it = cache.find(file); it != cache.end())
        return it->second;
    QImageReader r(file);
    const QSize s = r.size();
    if (s.isValid())
        r.setScaledSize(s.scaled(kThumb, kThumb, Qt::KeepAspectRatio));
    QPixmap p = QPixmap::fromImage(r.read());
    cache[file] = p;
    return p;
}

class TexturePanel final : public QWidget
{
public:
    explicit TexturePanel(QWidget* parent) : QWidget(parent)
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(8, 8, 8, 8);
        v->setSpacing(4);

        // Browser: a folder of images or the scene's materials.
        auto* browse = new QWidget(this);
        auto* bv = new QVBoxLayout(browse);
        bv->setContentsMargins(0, 0, 0, 0);
        bv->setSpacing(4);
        auto* nav = new QHBoxLayout;
        nav->setSpacing(2);
        auto navButton = [&](const char* ic, const char* tip) {
            auto* b = new QToolButton(browse);
            b->setIcon(icon(ic));
            b->setToolTip(tip);
            b->setAutoRaise(true);
            nav->addWidget(b);
            return b;
        };
        mBack = navButton("arrow-left", "Back  (mouse button 4)");
        mForward = navButton("arrow-right", "Forward  (mouse button 5)");
        mPlaces = new QComboBox(browse);
        mPlaces->setToolTip("Places: the scene's materials and favourite folders");
        mPlaces->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        nav->addWidget(mPlaces);
        mFavourite = navButton("star", "Favourite: keep this folder in Places");
        mFavourite->setCheckable(true);
        QToolButton* open = navButton("folder-open", "Open Folder: browse images in another folder");
        bv->addLayout(nav);
        mFilter = new QLineEdit(browse);
        mFilter->setPlaceholderText("Filter");
        mFilter->setClearButtonEnabled(true);
        bv->addWidget(mFilter);
        mList = new QListWidget(browse);
        mList->setViewMode(QListView::IconMode);
        mList->setIconSize(QSize(kThumb, kThumb));
        mList->setGridSize(QSize(kThumb + 16, kThumb + 28));
        mList->setResizeMode(QListView::Adjust);
        mList->setMovement(QListView::Static);
        mList->setWordWrap(true);
        mList->setMinimumHeight(220);
        mList->setToolTip("Click to put it on the selected faces or blocks, and to paint it with the Paint tool");
        bv->addWidget(mList);
        auto* foot = new QHBoxLayout;
        mCount = new QLabel(browse);
        mCount->setToolTip("Faces the next pick goes onto");
        foot->addWidget(mCount, 1);
        auto* ph = new QToolButton(browse);
        ph->setText("Placeholder");
        ph->setToolTip("Placeholder: make a flat colour texture with a label in this folder");
        controls::setRole(ph, "push");
        foot->addWidget(ph);
        bv->addLayout(foot);
        v->addWidget(controls::section("Textures", browse, this, "blockworkTextures"));

        // The selected faces' mapping.
        auto* face = new QWidget(this);
        auto* f = new QFormLayout(face);
        f->setContentsMargins(0, 0, 0, 0);
        f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        f->setVerticalSpacing(4);
        mAlign = new controls::Segmented(face);
        mAlign->add("Face", {}, "On the face's own plane, upright: no stretch on slopes", 0);
        mAlign->add("World", {}, "Along the nearest world axis, so neighbouring slopes line up", 1);
        f->addRow("Align", mAlign);
        auto pair = [&](const char* label, const char* tip, double lo, double hi, int dec, const QString& unit, QDoubleSpinBox*& a, QDoubleSpinBox*& b) {
            auto* row = new QWidget(face);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(4);
            a = controls::axisField('U', lo, hi, dec, unit, row);
            b = controls::axisField('V', lo, hi, dec, unit, row);
            a->setToolTip(tip), b->setToolTip(tip);
            h->addWidget(a), h->addWidget(b);
            f->addRow(label, row);
        };
        pair("Shift", "Shift: offset in repeats", -1e4, 1e4, 3, {}, mShiftU, mShiftV);
        pair("Scale", "Scale: cm per repeat; 0 uses the block's tile size", 0.0, 1e6, 1, "cm", mScaleU, mScaleV);
        mRotate = controls::valueField(-360.0, 360.0, 1, "\xc2\xb0", face);
        mRotate->setToolTip("Rotate: turn on the face");
        f->addRow("Rotate", mRotate);
        for (QDoubleSpinBox* s : {mShiftU, mShiftV})
            s->setSingleStep(0.125);
        auto* fits = new QWidget(face);
        auto* fh = new QHBoxLayout(fits);
        fh->setContentsMargins(0, 0, 0, 0);
        fh->setSpacing(2);
        auto push = [&](QHBoxLayout* into, QWidget* parent, const QString& text, const char* tip, json call) {
            auto* b = new QToolButton(parent);
            b->setText(text);
            b->setToolTip(tip);
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            controls::setRole(b, "push");
            connect(b, &QToolButton::clicked, this, [call] { runOnTargets("blockworkTexture", call); });
            into->addWidget(b);
        };
        push(fh, fits, "Width", "Fit Width: one repeat across the face, keeping its proportions", {{"fit", "width"}});
        push(fh, fits, "Height", "Fit Height: one repeat up the face, keeping its proportions", {{"fit", "height"}});
        push(fh, fits, "Both", "Fit: one repeat covers the face exactly", {{"fit", "both"}});
        f->addRow("Fit", fits);
        auto* just = new QWidget(face);
        auto* jh = new QHBoxLayout(just);
        jh->setContentsMargins(0, 0, 0, 0);
        jh->setSpacing(2);
        push(jh, just, "L", "Justify Left: the texture's left edge on the face's", {{"justify", "left"}});
        push(jh, just, "R", "Justify Right", {{"justify", "right"}});
        push(jh, just, "T", "Justify Top", {{"justify", "top"}});
        push(jh, just, "B", "Justify Bottom", {{"justify", "bottom"}});
        push(jh, just, "C", "Justify Center", {{"justify", "center"}});
        f->addRow("Justify", just);
        auto* misc = new QWidget(face);
        auto* mh = new QHBoxLayout(misc);
        mh->setContentsMargins(0, 0, 0, 0);
        mh->setSpacing(2);
        push(mh, misc, "Reset", "Reset: face aligned at the tile size, no shift or turn", {{"reset", true}});
        auto* hot = new QToolButton(misc);
        hot->setText("Hotspot");
        hot->setToolTip("Hotspot: map the faces onto the current trim sheet's best fitting trims (selected blocks stay hotspotted as they change)");
        hot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        controls::setRole(hot, "push");
        connect(hot, &QToolButton::clicked, this, [] {
            bool faces = false;
            for (const auto& [id, list] : components::selected(ComponentMode::Face))
                faces = faces || !list.empty();
            try
            {
                ops::execute("blockworkHotspot", faces ? json{{"live", false}} : json::object(), CallSource::UI);
            }
            catch (const std::exception& e)
            {
                log::warn("{}", e.what());
            }
        });
        mh->addWidget(hot);
        f->addRow("", misc);
        mLock = new QCheckBox("Texture Lock", face);
        mLock->setToolTip("Texture Lock: the texture stays put on the surface through edits; off, the uvs stretch with them");
        f->addRow("", mLock);
        v->addWidget(controls::section("Faces", face, this, "blockworkTextures"));
        v->addStretch();

        connect(mBack, &QToolButton::clicked, this, [this] { step(-1); });
        connect(mForward, &QToolButton::clicked, this, [this] { step(1); });
        connect(open, &QToolButton::clicked, this, [this] {
            const QString d = fileDialog::getExistingDirectory(this, "Texture Folder", mFolder);
            if (!d.isEmpty())
                go(d);
        });
        connect(mFavourite, &QToolButton::clicked, this, [this](bool on) { setFavourite(on); });
        connect(mPlaces, &QComboBox::activated, this, [this](int i) { go(mPlaces->itemData(i).toString()); });
        connect(mFilter, &QLineEdit::textChanged, this, [this] { fill(); });
        connect(mList, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { pick(it); });
        connect(ph, &QToolButton::clicked, this, [this] {
            try
            {
                json call = {{"name", "Placeholder"}};
                if (!mFolder.isEmpty() && mFolder != kSceneEntry)
                    call["folder"] = mFolder.toStdString();
                const std::string mat = ops::execute("blockworkPlaceholder", call, CallSource::UI).get<std::string>();
                settings().material = mat;
                saveSettings();
                fill();
            }
            catch (const std::exception& e)
            {
                log::warn("{}", e.what());
            }
        });
        connect(mAlign, &controls::Segmented::picked, this, [](int id) { runOnTargets("blockworkTexture", {{"align", id ? "world" : "face"}}); });
        auto edited = [this](auto&& build) {
            return [this, build](double) {
                if (!mSyncing)
                    runOnTargets("blockworkTexture", build());
            };
        };
        connect(mShiftU, &QDoubleSpinBox::valueChanged, this, edited([this] { return json{{"offset", {mShiftU->value(), mShiftV->value()}}}; }));
        connect(mShiftV, &QDoubleSpinBox::valueChanged, this, edited([this] { return json{{"offset", {mShiftU->value(), mShiftV->value()}}}; }));
        connect(mScaleU, &QDoubleSpinBox::valueChanged, this, edited([this] { return json{{"scale", {mScaleU->value(), mScaleV->value()}}}; }));
        connect(mScaleV, &QDoubleSpinBox::valueChanged, this, edited([this] { return json{{"scale", {mScaleU->value(), mScaleV->value()}}}; }));
        connect(mRotate, &QDoubleSpinBox::valueChanged, this, edited([this] { return json{{"rotate", mRotate->value()}}; }));
        connect(mLock, &QCheckBox::clicked, this, [](bool on) {
            try
            {
                ops::execute("blockworkTextureLock", {{"on", on}}, CallSource::UI);
            }
            catch (const std::exception& e)
            {
                log::warn("{}", e.what());
            }
        });

        mRefresh = new QTimer(this);
        mRefresh->setSingleShot(true);
        mRefresh->setInterval(60);
        connect(mRefresh, &QTimer::timeout, this, [this] {
            syncFaces();
            if (mFolder == kSceneEntry && materialNames() != mShownMaterials)
                fill();
        });
        mListeners.push_back(components::addListener([this] { mRefresh->start(); }));
        mGraphListener = graph().addListener([this](Change, NodeId) { mRefresh->start(); });

        const QString start = QString::fromStdString(PrefStore::get().value<std::string>("general", kFolderPref, kSceneEntry));
        go(start.isEmpty() ? QString(kSceneEntry) : start, false);
        syncFaces();
    }

    ~TexturePanel() override
    {
        for (int id : mListeners)
            components::removeListener(id);
        graph().removeListener(mGraphListener);
    }

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::BackButton || e->button() == Qt::ForwardButton)
            step(e->button() == Qt::BackButton ? -1 : 1);
        QWidget::mousePressEvent(e);
    }

private:
    QStringList favourites() const
    {
        QStringList out;
        for (const json& f : PrefStore::get().value("general", kFavouritesPref, json::array()))
            if (f.is_string())
                out << QString::fromStdString(f.get<std::string>());
        return out;
    }

    void setFavourite(bool on)
    {
        QStringList favs = favourites();
        favs.removeAll(mFolder);
        if (on && mFolder != kSceneEntry)
            favs << mFolder;
        json j = json::array();
        for (const QString& s : favs)
            j.push_back(s.toStdString());
        PrefStore::get().set("general", kFavouritesPref, j);
        places();
    }

    void places()
    {
        const QSignalBlocker b(mPlaces);
        mPlaces->clear();
        mPlaces->addItem(icon("cube"), kSceneEntry, QString(kSceneEntry));
        for (const QString& f : favourites())
            mPlaces->addItem(icon("star"), QFileInfo(f).fileName().isEmpty() ? f : QFileInfo(f).fileName(), f);
        if (mFolder != kSceneEntry && mPlaces->findData(mFolder) < 0)
            mPlaces->addItem(icon("folder"), QFileInfo(mFolder).fileName(), mFolder);
        mPlaces->setCurrentIndex(std::max(0, mPlaces->findData(mFolder)));
        mFavourite->setEnabled(mFolder != kSceneEntry);
        mFavourite->setChecked(favourites().contains(mFolder));
    }

    void go(const QString& folder, bool record = true)
    {
        if (record && folder != mFolder)
        {
            mHistory.resize(size_t(mAt + 1));
            mHistory.push_back(folder);
            mAt = int(mHistory.size()) - 1;
        }
        else if (mHistory.empty())
            mHistory = {folder}, mAt = 0;
        mFolder = folder;
        PrefStore::get().set("general", kFolderPref, folder.toStdString());
        places();
        fill();
    }

    void step(int d)
    {
        const int to = mAt + d;
        if (to < 0 || to >= int(mHistory.size()))
            return;
        mAt = to;
        go(mHistory[size_t(mAt)], false);
    }

    std::vector<std::string> materialNames() const
    {
        std::vector<std::string> out;
        for (Node* m : materials::all(graph()))
            if (paintable(*m))
                out.push_back(m->name());
        return out;
    }

    void fill()
    {
        mShownMaterials = materialNames();
        mBack->setEnabled(mAt > 0);
        mForward->setEnabled(mAt + 1 < int(mHistory.size()));
        mList->clear();
        const QString filter = mFilter->text().trimmed();
        const std::string current = settings().material;
        if (mFolder == kSceneEntry)
        {
            for (Node* m : materials::all(graph()))
            {
                if (!paintable(*m))
                    continue;
                const QString name = QString::fromStdString(m->name());
                if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive))
                    continue;
                QPixmap px;
                for (const auto& [ch, attr] : materials::textureChannels())
                    if (ch == "baseColor" && !m->as<std::string>(attr).empty())
                        px = thumbnail(QString::fromStdString(materials::resolvePath(m->as<std::string>(attr))));
                const std::string set = materials::textureSet(*m);
                auto* it = new QListWidgetItem(px.isNull() ? swatch(m->as<glm::dvec3>(material::kBaseColor)) : QIcon(px), name, mList);
                it->setData(Qt::UserRole, QString("material:") + name);
                it->setSelected(m->name() == current);
                // A material painted from a texture set (a trim sheet's palettes) shows the set on the preview ball.
                if (!set.empty())
                    showBall(it->data(Qt::UserRole).toString(), set);
            }
        }
        else
        {
            QStringList pats;
            for (const char* e : {"*.png", "*.jpg", "*.jpeg", "*.tga", "*.bmp", "*.tif", "*.tiff", "*.exr", "*.hdr", "*.webp"})
                pats << e;
            for (const QFileInfo& fi : QDir(mFolder).entryInfoList(pats, QDir::Files, QDir::Name | QDir::IgnoreCase))
            {
                if (!filter.isEmpty() && !fi.completeBaseName().contains(filter, Qt::CaseInsensitive))
                    continue;
                auto* it = new QListWidgetItem(QIcon(thumbnail(fi.absoluteFilePath())), fi.completeBaseName(), mList);
                it->setData(Qt::UserRole, QString("image:") + fi.absoluteFilePath());
                it->setToolTip(fi.absoluteFilePath());
            }
        }
        if (mList->count() == 0)
            new QListWidgetItem(mFolder == kSceneEntry ? "No materials. Open a folder of images, or make a Placeholder."
                                                       : "No images here. Open another folder.",
                                mList);
    }

    void pick(QListWidgetItem* it)
    {
        const QString what = it->data(Qt::UserRole).toString();
        if (what.isEmpty())
            return;
        json call;
        if (what.startsWith("image:"))
            call["image"] = what.mid(6).toStdString();
        else
            call["material"] = what.mid(9).toStdString();
        try
        {
            std::string mat = call.contains("material") ? call["material"].get<std::string>() : materialForImage(graph(), call["image"].get<std::string>());
            settings().material = mat;
            saveSettings();
            if (targets().count > 0)
                runOnTargets("blockworkMaterial", {{"material", mat}});
        }
        catch (const std::exception& e)
        {
            log::warn("{}", e.what());
        }
    }

    void syncFaces()
    {
        const Targets t = targets();
        mCount->setText(t.count ? QString("%1 face%2").arg(t.count).arg(t.count == 1 ? "" : "s") : QString("No faces selected"));
        mSyncing = true;
        // Show the first target face's mapping.
        bool any = false;
        for (const auto& [id, faces] : t.faces)
        {
            Node* n = graph().node(id);
            const MeshPtr& m = n ? n->as<MeshPtr>(MeshNode::kOutMesh) : MeshPtr();
            if (!m || faces.empty() || !m->faceValid(faces.front()))
                continue;
            const FaceMapping fm = faceMapping(*m, faces.front());
            for (auto [box, value] : {std::pair{mShiftU, double(fm.offset.x)}, {mShiftV, double(fm.offset.y)}, {mScaleU, double(fm.scale.x)},
                                      {mScaleV, double(fm.scale.y)}, {mRotate, double(fm.rotate)}})
            {
                const QSignalBlocker b(box);
                box->setValue(value);
            }
            {
                const QSignalBlocker b(mAlign);
                if (fm.mode == kMapWorld)
                    mAlign->setCurrent(1);
                else if (fm.mode == kMapFace)
                    mAlign->setCurrent(0);
                else
                    mAlign->clearCurrent();
            }
            bool locked = false;
            for (NodeId mod : modifiers::stack(graph(), id))
                if (Node* k = graph().node(mod); k && k->type().name == kTextureLockType)
                    locked = k->as<bool>(k->type().attrIndex("enabled"));
            mLock->setChecked(locked);
            any = true;
            break;
        }
        for (QWidget* w : std::initializer_list<QWidget*>{mShiftU, mShiftV, mScaleU, mScaleV, mRotate, mAlign, mLock})
            w->setEnabled(any);
        mSyncing = false;
    }

    /** Puts the texture set's preview ball on the tile keyed key, rendering it once per panel (the renderer caches by content). */
    void showBall(const QString& key, const std::string& set)
    {
        if (auto it = mBalls.find(set); it != mBalls.end())
        {
            if (!it->second.isNull())
                setTileIcon(key, it->second);
            return;
        }
        mBalls[set] = QPixmap();
        materialThumbs::Options o;
        o.size = 128;
        QPointer<TexturePanel> alive(this);
        materialThumbs::renderAsync(set, o, [alive, key, set](const materialThumbs::Result& r, const std::string& err) {
            if (!alive)
                return;
            if (!err.empty())
            {
                log::warn("Block Textures: no preview for {}: {}", set, err);
                return;
            }
            alive->mBalls[set] = QPixmap(QString::fromStdWString(r.file.wstring()));
            alive->setTileIcon(key, alive->mBalls[set]);
        });
    }

    void setTileIcon(const QString& key, const QPixmap& px)
    {
        for (int i = 0; i < mList->count(); ++i)
            if (mList->item(i)->data(Qt::UserRole).toString() == key)
                mList->item(i)->setIcon(QIcon(px));
    }

    std::map<std::string, QPixmap> mBalls;
    QToolButton *mBack = nullptr, *mForward = nullptr, *mFavourite = nullptr;
    QComboBox* mPlaces = nullptr;
    QLineEdit* mFilter = nullptr;
    QListWidget* mList = nullptr;
    QLabel* mCount = nullptr;
    controls::Segmented* mAlign = nullptr;
    QDoubleSpinBox *mShiftU = nullptr, *mShiftV = nullptr, *mScaleU = nullptr, *mScaleV = nullptr, *mRotate = nullptr;
    QCheckBox* mLock = nullptr;
    QTimer* mRefresh = nullptr;
    QString mFolder;
    std::vector<QString> mHistory;
    int mAt = -1;
    bool mSyncing = false;
    std::vector<std::string> mShownMaterials;
    std::vector<int> mListeners;
    int mGraphListener = 0;
};
} // namespace

void registerTexturePanel(ext::Context& ctx)
{
    panels::add(ctx, {.id = kTexturePanelId,
                      .title = "Block Textures",
                      .create = [](QWidget* parent) -> QWidget* { return new TexturePanel(parent); },
                      .area = Qt::RightDockWidgetArea,
                      .icon = "texture",
                      .open = false});
    actions::add(ctx, {.id = "blockwork.textures",
                       .label = "Block Textures",
                       .category = "Blockwork",
                       .run = [] { panels::show(kTexturePanelId); },
                       .doc = "Open the Block Textures panel: pick images and materials for blocks and set how they lie on the selected faces.",
                       .icon = "texture",
                       .keywords = {"texture browser", "material browser", "face edit", "uv"}});
}
} // namespace blockwork
