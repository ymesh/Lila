#include "ui/PoseLibrary.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Workspace.h"
#include "scene/Graph.h"
#include "scene/Poses.h"
#include "ui/Actions.h"
#include "ui/AgentCapture.h"
#include "ui/Controls.h"
#include "ui/Deletion.h"
#include "ui/Icons.h"
#include "ui/Panels.h"
#include "ui/SpinScrub.h"
#include "ui/Theme.h"
#include "ui/UiCommands.h"
#include "ui/UiCommon.h"

#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <map>
#include <utility>
#include <vector>

namespace lila::ui
{
namespace
{
namespace poses = scene::poses;
constexpr int kPoseThumb = 96;

/** How every pose picture is taken: the whole character from the front, shaded, nothing else drawn. */
QImage renderPoseShot()
{
    capture::Options o;
    o.view = "front";
    o.width = o.height = 2 * kPoseThumb;
    o.display = "shaded";
    o.grid = false;
    o.showSelection = false;
    o.label = false;
    return capture::render(o);
}

/** Renders of poses that carry no thumbnail (template poses), keyed by character, tier and the pose itself. */
std::map<std::string, QPixmap>& previews()
{
    static std::map<std::string, QPixmap> cache;
    return cache;
}

std::string previewKey(const poses::Entry& e)
{
    return e.pose.character + "|" + poses::tierName(e.tier) + "|" + e.pose.toJson().dump();
}

/**
 * The character posed as e, rendered from the front. The pose goes on with undo off and comes off again, so nothing in
 * the scene changes. A null pixmap when the selection isn't on that character.
 */
QPixmap renderPreview(const poses::Entry& e)
{
    scene::Graph& g = scene::Graph::current();
    if (g.selection().empty())
        return {};
    const poses::Character ch = poses::characterOf(g, g.selection().front());
    if (ch.nodes.empty() || ch.id != e.pose.character)
        return {};
    UndoStack& undo = UndoStack::get();
    const bool was = undo.enabled();
    undo.setEnabled(false);
    QPixmap out;
    const poses::Pose before = poses::capture(g, ch.nodes);
    try
    {
        poses::ApplyOptions opts;
        opts.scope = ch.nodes;
        poses::apply(g, e.pose, opts);
        g.evaluate();
        out = QPixmap::fromImage(renderPoseShot());
    }
    catch (const std::exception& ex)
    {
        log::warn("Pose preview for {} failed: {}", e.pose.name, ex.what());
    }
    poses::ApplyOptions back;
    back.scope = ch.nodes;
    poses::apply(g, before, back);
    g.evaluate();
    undo.setEnabled(was);
    return out;
}
constexpr int kNameRole = Qt::UserRole;
constexpr int kTierRole = Qt::UserRole + 1;
constexpr int kCharacterRole = Qt::UserRole + 2;

class PoseLibraryPanel : public QWidget
{
public:
    explicit PoseLibraryPanel(QWidget* parent) : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(4, 4, 4, 4);
        layout->setSpacing(4);

        auto* top = new QHBoxLayout;
        mCharacter = new QComboBox(this);
        mCharacter->setToolTip("Character\nWhose poses are shown; Selection follows the selected rig");
        mCharacter->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        top->addWidget(mCharacter);
        mTier = new QComboBox(this);
        mTier->setToolTip("Library Tier\nScene file, workspace (.lila/poses), user prefs or rig template");
        mTier->addItem("All Tiers", QString());
        for (poses::Tier t : poses::kTiers)
            mTier->addItem(QString(poses::tierName(t)).replace(0, 1, QString(poses::tierName(t)).at(0).toUpper()), poses::tierName(t));
        top->addWidget(mTier);
        mSearch = new QLineEdit(this);
        mSearch->setPlaceholderText("Search names and tags");
        mSearch->setClearButtonEnabled(true);
        top->addWidget(mSearch, 1);
        layout->addLayout(top);

        mList = new QListWidget(this);
        mList->setViewMode(QListView::IconMode);
        mList->setIconSize(QSize(kPoseThumb, kPoseThumb));
        mList->setGridSize(QSize(kPoseThumb + 16, kPoseThumb + 28));
        mList->setResizeMode(QListView::Adjust);
        mList->setMovement(QListView::Static);
        mList->setUniformItemSizes(true);
        mList->setWordWrap(true);
        mList->setContextMenuPolicy(Qt::CustomContextMenu);
        mList->setToolTip("Double-click to apply, Ctrl+double-click to apply mirrored");
        deletion::claim(mList, [this] {
            if (QListWidgetItem* it = mList->currentItem(); it && it->isSelected())
                deleteItem(it);
        });
        setEmptyHint(mList, "No poses for this character. Pose the rig and click Save.");
        layout->addWidget(mList, 1);

        auto* blendRow = new QHBoxLayout;
        blendRow->addWidget(new QLabel("Blend", this));
        mBlend = new controls::SliderField(this);
        mBlend->setRange(0.0, 100.0);
        mBlend->setDecimals(0);
        mBlend->setSuffix(" %");
        mBlend->setValue(100.0);
        setSpinDefault(mBlend, 100.0);
        mBlend->setToolTip("Blend\nHow far applying moves from the current pose toward the stored one");
        blendRow->addWidget(mBlend, 1);
        layout->addLayout(blendRow);

        auto* buttons = new QHBoxLayout;
        buttons->setSpacing(2);
        auto pushButton = [this](const QString& text, const QString& tip) {
            auto* b = new QToolButton(this);
            b->setText(text);
            b->setToolTip(tip);
            controls::setRole(b, "push");
            return b;
        };
        QToolButton* apply = pushButton("Apply", "Apply Pose\nApply the selected pose (Ctrl: mirrored)");
        QToolButton* mirror = pushButton("Mirror", "Apply Mirrored\nApply the selected pose mirrored, or mirror the current pose when none is selected");
        mSaveTier = new QComboBox(this);
        mSaveTier->setToolTip("Save To\nThe tier Save puts new poses in");
        for (const char* t : {"scene", "workspace", "user"})
            mSaveTier->addItem(QString(t).replace(0, 1, QString(t).at(0).toUpper()), t);
        setComboDefault(mSaveTier, 0);
        QToolButton* save = pushButton("Save", "Save Pose\nSave the selection's current pose under a new name");
        buttons->addWidget(apply);
        buttons->addWidget(mirror);
        buttons->addSpacing(10);
        buttons->addStretch(1);
        buttons->addWidget(new QLabel("Save To", this));
        buttons->addWidget(mSaveTier);
        buttons->addWidget(save);
        layout->addLayout(buttons);

        connect(mCharacter, &QComboBox::activated, this, [this] { refresh(); });
        connect(mTier, &QComboBox::activated, this, [this] { refresh(); });
        // These refresh on activated, which a plain default reset would not emit.
        for (QComboBox* c : {mCharacter, mTier})
            onMiddleClickReset(
                c,
                [c] {
                    c->setCurrentIndex(0);
                    Q_EMIT c->activated(0);
                },
                c == mTier ? mTier->itemText(0) : QString("Selection"));
        connect(mSearch, &QLineEdit::textChanged, this, [this] { refresh(); });
        connect(mList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
            applyItem(item, QApplication::keyboardModifiers().testFlag(Qt::ControlModifier));
        });
        connect(apply, &QToolButton::clicked, this, [this] { applyItem(mList->currentItem(), QApplication::keyboardModifiers().testFlag(Qt::ControlModifier)); });
        connect(mirror, &QToolButton::clicked, this, [this] {
            if (QListWidgetItem* item = mList->currentItem())
                applyItem(item, true);
            else
                tryRunUi("mirrorPose", json::object());
        });
        connect(save, &QToolButton::clicked, this, [this] { savePose(); });
        connect(mList, &QListWidget::customContextMenuRequested, this, [this](const QPoint& at) { contextMenu(at); });

        mPoseListener = poses::addListener([this] { scheduleRefresh(); });
        mGraphListener = scene::Graph::current().addListener([this](scene::Change c, scene::NodeId) {
            if (c == scene::Change::Selection || c == scene::Change::Reset || c == scene::Change::Cleared || c == scene::Change::Renamed)
                scheduleRefresh();
        });
        QPointer<PoseLibraryPanel> self(this);
        Workspace::get().onChanged([self] {
            if (self)
                self->scheduleRefresh();
        });
        refresh();
    }

    ~PoseLibraryPanel() override
    {
        poses::removeListener(mPoseListener);
        scene::Graph::current().removeListener(mGraphListener);
    }

protected:
    void showEvent(QShowEvent* e) override
    {
        QWidget::showEvent(e);
        refresh();
    }

private:
    void scheduleRefresh()
    {
        if (mRefreshQueued)
            return;
        mRefreshQueued = true;
        QTimer::singleShot(0, this, [this] {
            mRefreshQueued = false;
            if (isVisible())
                refresh();
        });
    }

    /** The character the selection belongs to, or empty. */
    std::string selectedCharacter() const
    {
        scene::Graph& g = scene::Graph::current();
        return g.selection().empty() ? std::string() : poses::characterOf(g, g.selection().front()).id;
    }

    std::string currentCharacter() const
    {
        QString stored = mCharacter->currentData().toString();
        return stored.isEmpty() ? selectedCharacter() : stored.toStdString();
    }

    void refreshCharacters()
    {
        QString keep = mCharacter->currentData().toString();
        QSignalBlocker block(mCharacter);
        mCharacter->clear();
        std::string sel = selectedCharacter();
        mCharacter->addItem(sel.empty() ? QString("Selection") : QString("Selection (%1)").arg(qs(sel)), QString());
        for (const std::string& c : poses::characters())
            mCharacter->addItem(qs(c), qs(c));
        int i = mCharacter->findData(keep);
        mCharacter->setCurrentIndex(i < 0 ? 0 : i);
    }

    void refresh()
    {
        refreshCharacters();
        mPendingPreviews.clear();
        QString keep = mList->currentItem() ? mList->currentItem()->data(kNameRole).toString() : QString();
        mList->clear();
        std::string character = currentCharacter();
        if (character.empty())
            return;
        std::optional<poses::Tier> tier = poses::tierFromName(mTier->currentData().toString().toStdString());
        QString search = mSearch->text().trimmed();
        for (const poses::Entry& e : poses::list(character, tier))
        {
            QString name = qs(e.pose.name);
            QStringList tags;
            for (const std::string& t : e.pose.tags)
                tags << qs(t);
            if (!search.isEmpty() && !name.contains(search, Qt::CaseInsensitive) && !tags.join(' ').contains(search, Qt::CaseInsensitive))
                continue;
            QPixmap thumb;
            if (!e.pose.thumbnail.empty())
                thumb.loadFromData(reinterpret_cast<const uchar*>(e.pose.thumbnail.data()), uint(e.pose.thumbnail.size()), "PNG");
            else if (auto it = previews().find(previewKey(e)); it != previews().end())
                thumb = it->second;
            else
                mPendingPreviews.push_back(e);
            auto* item = new QListWidgetItem(thumb.isNull() ? icon(poses::isReserved(e.pose.name) ? "lock" : "man") : QIcon(thumb), name);
            item->setData(kNameRole, name);
            item->setData(kTierRole, QString(poses::tierName(e.tier)));
            item->setData(kCharacterRole, qs(e.pose.character));
            item->setToolTip(QString("%1\n%2 tier, %3 nodes%4%5")
                                 .arg(name)
                                 .arg(QString(poses::tierName(e.tier)))
                                 .arg(e.pose.nodes.size())
                                 .arg(tags.isEmpty() ? QString() : "\nTags: " + tags.join(", "))
                                 .arg(poses::isReserved(e.pose.name) ? "\nReserved pose" : ""));
            mList->addItem(item);
            if (name == keep)
                mList->setCurrentItem(item);
        }
        if (!mPendingPreviews.empty())
            QTimer::singleShot(0, this, [this] { renderPending(); });
    }

    /** Renders the poses refresh found without a picture, then shows them; one that can't render keeps its icon. */
    void renderPending()
    {
        if (mPendingPreviews.empty())
            return;
        const std::vector<poses::Entry> todo = std::exchange(mPendingPreviews, {});
        for (const poses::Entry& e : todo)
            previews()[previewKey(e)] = renderPreview(e);
        refresh();
    }

    json poseCall(QListWidgetItem* item) const
    {
        return {{"args", {item->data(kNameRole).toString().toStdString()}},
                {"character", item->data(kCharacterRole).toString().toStdString()},
                {"tier", item->data(kTierRole).toString().toStdString()}};
    }

    void applyItem(QListWidgetItem* item, bool mirror)
    {
        if (!item)
            return;
        json call = poseCall(item);
        call["weight"] = mBlend->value() / 100.0;
        call["mirror"] = mirror;
        json report;
        if (tryRunUi("applyPose", call, &report) && report.contains("missing") && !report["missing"].empty())
            log::warn("Pose '{}': skipped {} node(s) this rig doesn't have", item->text().toStdString(), report["missing"].size());
    }

    void savePose()
    {
        if (scene::Graph::current().selection().empty())
        {
            QMessageBox::information(this, "Save Pose", "Select the rig (or the controls) whose pose to save.");
            return;
        }
        bool ok = false;
        QString name = QInputDialog::getText(this, "Save Pose", "Pose name (bind, ref and test are each character's reserved poses):",
                                             QLineEdit::Normal, QString(), &ok)
                           .trimmed();
        if (!ok || name.isEmpty())
            return;
        json call = {{"args", {name.toStdString()}}, {"tier", mSaveTier->currentData().toString().toStdString()}};
        if (QString c = mCharacter->currentData().toString(); !c.isEmpty())
            call["character"] = c.toStdString();
        tryRunUi("savePose", call);
    }

    /** Asks first: a pose saved to a file doesn't come back with undo. */
    void deleteItem(QListWidgetItem* item)
    {
        if (item->data(kTierRole).toString() == "template")
            return;
        if (QMessageBox::question(this, "Delete Pose", QString("Delete the pose '%1'?").arg(item->text())) != QMessageBox::Yes)
            return;
        json call = poseCall(item);
        call["force"] = poses::isReserved(item->data(kNameRole).toString().toStdString());
        tryRunUi("deletePose", call);
    }

    void contextMenu(const QPoint& at)
    {
        QListWidgetItem* item = mList->itemAt(at);
        QMenu menu(this);
        if (item)
        {
            menu.addAction("Apply", this, [this, item] { applyItem(item, false); });
            menu.addAction("Apply Mirrored", this, [this, item] { applyItem(item, true); });
            menu.addAction("Save Mirrored Copy", this, [this, item] { tryRunUi("mirrorPose", poseCall(item)); });
            menu.addSeparator();
            const bool readOnly = item->data(kTierRole).toString() == "template";
            const bool reserved = poses::isReserved(item->data(kNameRole).toString().toStdString());
            QAction* rename = menu.addAction("Rename...", this, [this, item] {
                bool ok = false;
                QString to = QInputDialog::getText(this, "Rename Pose", "New name:", QLineEdit::Normal, item->text(), &ok).trimmed();
                if (ok && !to.isEmpty() && to != item->text())
                {
                    json call = poseCall(item);
                    call["args"].push_back(to.toStdString());
                    tryRunUi("renamePose", call);
                }
            });
            rename->setEnabled(!readOnly && !reserved);
            QAction* del = menu.addAction("Delete", this, [this, item] { deleteItem(item); });
            del->setEnabled(!readOnly);
            menu.addSeparator();
        }
        menu.addAction("Mirror Current Pose", this, [] { tryRunUi("mirrorPose", json::object()); });
        menu.addAction("Refresh", this, [this] { refresh(); });
        menu.exec(mList->viewport()->mapToGlobal(at));
    }

    QComboBox* mCharacter = nullptr;
    QComboBox* mTier = nullptr;
    QComboBox* mSaveTier = nullptr;
    QLineEdit* mSearch = nullptr;
    QListWidget* mList = nullptr;
    std::vector<poses::Entry> mPendingPreviews;
    controls::SliderField* mBlend = nullptr;
    int mPoseListener = 0;
    int mGraphListener = 0;
    bool mRefreshQueued = false;
};

std::string thumbnail(const std::vector<scene::NodeId>&)
{
    try
    {
        return capture::png(renderPoseShot());
    }
    catch (const std::exception& e)
    {
        log::warn("Pose thumbnail failed: {}", e.what());
        return {};
    }
}
} // namespace

QWidget* createPoseLibraryPanel(QWidget* parent)
{
    return new PoseLibraryPanel(parent);
}

void registerPoseLibraryUi()
{
    poses::setThumbnailProvider(thumbnail);
    panels::add({.id = kPoseLibraryDock, .title = "Pose Library", .create = [](QWidget* parent) { return createPoseLibraryPanel(parent); },
                 .area = Qt::LeftDockWidgetArea, .icon = "man", .open = false, .toggleAction = "window.poseLibrary"});
    actions::add({.id = "pose.mirror", .label = "Mirror Pose", .category = "Animation", .run = [] { tryRunUi("mirrorPose", json::object()); },
                  .doc = "Mirror the selected character's current pose: left and right swap, the centre reflects.",
                  .python = "mirrorPose()"});
}
} // namespace lila::ui
