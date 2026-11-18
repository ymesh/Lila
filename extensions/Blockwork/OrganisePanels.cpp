#include "Tool.h"

#include <lila/Controls.h>
#include <lila/FileDialog.h>
#include <lila/Icons.h>
#include <lila/Log.h>
#include <lila/UI.h>
#include <lila/ViewportTool.h>

#include <QCheckBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <filesystem>

namespace blockwork
{
using namespace lila;
using namespace lila::ui;

namespace
{
json run(const char* cmd, json call)
{
    try
    {
        return ops::execute(cmd, call, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::warn("{}", e.what());
        return nullptr;
    }
}

QToolButton* iconButton(QWidget* parent, const char* ic, const QString& tip)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon(ic));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    return b;
}

/** Visgroups: a row per group, its eye showing and hiding it, with buttons for the selection. */
class VisgroupPanel final : public QWidget
{
public:
    explicit VisgroupPanel(QWidget* parent) : QWidget(parent)
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(8, 8, 8, 8);
        v->setSpacing(4);
        auto* bar = new QHBoxLayout;
        bar->setSpacing(2);
        QToolButton* add = iconButton(this, "plus", "New Visgroup: from the selection");
        QToolButton* put = iconButton(this, "row-insert-bottom", "Add Selection: to the current visgroup");
        QToolButton* take = iconButton(this, "row-remove", "Remove Selection: from the current visgroup");
        QToolButton* pick = iconButton(this, "pointer", "Select: the current visgroup's objects");
        QToolButton* del = iconButton(this, "trash", "Delete Visgroup: its objects stay");
        for (QToolButton* b : {add, put, take, pick})
            bar->addWidget(b);
        bar->addStretch();
        bar->addWidget(del);
        v->addLayout(bar);
        mList = new QListWidget(this);
        v->addWidget(mList, 1);

        connect(add, &QToolButton::clicked, this, [this] {
            controls::editInPlace(mList, "Visgroup1", [this](const QString& name) {
                run("blockworkVisgroup", {{"args", {name.toStdString()}}});
                refresh();
            });
        });
        auto onCurrent = [this](json call) {
            if (QListWidgetItem* it = mList->currentItem())
            {
                call["args"] = {it->data(Qt::UserRole).toString().toStdString()};
                run("blockworkVisgroup", call);
                refresh();
            }
        };
        connect(put, &QToolButton::clicked, this, [onCurrent] { onCurrent({{"add", json::array()}}); });
        connect(take, &QToolButton::clicked, this, [onCurrent] { onCurrent({{"remove", json::array()}}); });
        connect(pick, &QToolButton::clicked, this, [onCurrent] { onCurrent({{"select", true}}); });
        connect(del, &QToolButton::clicked, this, [onCurrent] { onCurrent({{"delete", true}}); });
        connect(mList, &QListWidget::itemChanged, this, [this](QListWidgetItem* it) {
            if (mFilling)
                return;
            run("blockworkVisgroup", {{"args", {it->data(Qt::UserRole).toString().toStdString()}}, {"hidden", it->checkState() != Qt::Checked}});
        });
        auto* timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setInterval(150);
        connect(timer, &QTimer::timeout, this, [this] { refresh(); });
        mListener = graph().addListener([timer](scene::Change, scene::NodeId) { timer->start(); });
        refresh();
    }

    ~VisgroupPanel() override { graph().removeListener(mListener); }

private:
    void refresh()
    {
        const json groups = ops::execute("blockworkVisgroup", json::object());
        const QString current = mList->currentItem() ? mList->currentItem()->data(Qt::UserRole).toString() : QString();
        mFilling = true;
        mList->clear();
        for (const json& gr : groups)
        {
            const QString name = QString::fromStdString(gr["name"].get<std::string>());
            auto* it = new QListWidgetItem(QString("%1  (%2)").arg(name).arg(gr["members"].size()), mList);
            it->setData(Qt::UserRole, name);
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(gr["hidden"].get<bool>() ? Qt::Unchecked : Qt::Checked);
            it->setToolTip("Tick shows the group; an object stays hidden while any of its hidden groups is");
            if (name == current)
                mList->setCurrentItem(it);
        }
        if (groups.empty())
        {
            auto* it = new QListWidgetItem("No visgroups. Click + to make one from the selection.", mList);
            it->setFlags(Qt::NoItemFlags);
        }
        mFilling = false;
    }

    QListWidget* mList = nullptr;
    bool mFilling = false;
    int mListener = 0;
};

/** Prefabs: the files in the prefab folder; clicking one starts placing it. */
class PrefabPanel final : public QWidget
{
public:
    explicit PrefabPanel(QWidget* parent) : QWidget(parent)
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(8, 8, 8, 8);
        v->setSpacing(4);
        auto* bar = new QHBoxLayout;
        bar->setSpacing(2);
        QToolButton* save = iconButton(this, "device-floppy", "Save Selection: as a prefab in the prefab folder");
        QToolButton* update = iconButton(this, "refresh", "Update: save the selected placed prefab back to its file and rebuild every copy");
        QToolButton* folder = iconButton(this, "folder-open", "Prefab Folder: use another folder");
        for (QToolButton* b : {save, update})
            bar->addWidget(b);
        bar->addStretch();
        bar->addWidget(folder);
        v->addLayout(bar);
        mFolder = new QLabel(this);
        mFolder->setWordWrap(true);
        v->addWidget(mFolder);
        mList = new QListWidget(this);
        mList->setToolTip("Click to place it with the Prefab tool");
        v->addWidget(mList, 1);
        mFollow = new QCheckBox("Follow Surface", this);
        mFollow->setToolTip("Follow Surface: placed prefabs turn their up to the surface they land on");
        mFollow->setChecked(settings().prefabFollowsNormal);
        connect(mFollow, &QCheckBox::clicked, this, [](bool on) { settings().prefabFollowsNormal = on, saveSettings(); });
        v->addWidget(mFollow);

        connect(save, &QToolButton::clicked, this, [this] {
            controls::editInPlace(mList, "Prefab", [this](const QString& name) {
                run("blockworkPrefabSave", {{"name", name.toStdString()}});
                refresh();
            });
        });
        connect(update, &QToolButton::clicked, this, [this] { run("blockworkPrefabUpdate", json::object()), refresh(); });
        connect(folder, &QToolButton::clicked, this, [this] {
            const QString d = fileDialog::getExistingDirectory(this, "Prefab Folder");
            if (!d.isEmpty())
                run("blockworkPrefabs", {{"folder", d.toStdString()}}), refresh();
        });
        connect(mList, &QListWidget::itemClicked, this, [](QListWidgetItem* it) {
            const QString file = it->data(Qt::UserRole).toString();
            if (file.isEmpty())
                return;
            settings().prefab = file.toStdString();
            saveSettings();
            startTool(Mode::Prefab);
        });
        refresh();
    }

protected:
    void showEvent(QShowEvent* e) override
    {
        refresh();
        QWidget::showEvent(e);
    }

private:
    void refresh()
    {
        json r;
        try
        {
            r = ops::execute("blockworkPrefabs", json::object());
        }
        catch (const std::exception& e)
        {
            log::warn("{}", e.what());
            return;
        }
        mFolder->setText(QString::fromStdString(r["folder"].get<std::string>()));
        mList->clear();
        for (const json& f : r["prefabs"])
        {
            const QString path = QString::fromStdString(f.get<std::string>());
            auto* it = new QListWidgetItem(icon("packages"), QFileInfo(path).completeBaseName(), mList);
            it->setData(Qt::UserRole, path);
            it->setToolTip(path);
        }
        if (mList->count() == 0)
        {
            auto* it = new QListWidgetItem("No prefabs. Select blocks and entities, then click Save Selection.", mList);
            it->setFlags(Qt::NoItemFlags);
        }
    }

    QLabel* mFolder = nullptr;
    QListWidget* mList = nullptr;
    QCheckBox* mFollow = nullptr;
};
} // namespace

void registerOrganisePanels(ext::Context& ctx)
{
    panels::add(ctx, {.id = kVisgroupPanelId,
                      .title = "Visgroups",
                      .create = [](QWidget* parent) -> QWidget* { return new VisgroupPanel(parent); },
                      .area = Qt::RightDockWidgetArea,
                      .icon = "eye",
                      .open = false});
    panels::add(ctx, {.id = kPrefabPanelId,
                      .title = "Prefabs",
                      .create = [](QWidget* parent) -> QWidget* { return new PrefabPanel(parent); },
                      .area = Qt::RightDockWidgetArea,
                      .icon = "packages",
                      .open = false});
    actions::add(ctx, {.id = "blockwork.visgroups",
                       .label = "Visgroups",
                       .category = "Blockwork",
                       .run = [] { panels::show(kVisgroupPanelId); },
                       .doc = "Open Visgroups: named sets of objects to hide and show together.",
                       .icon = "eye",
                       .keywords = {"visgroup", "layers", "hide groups"}});
    actions::add(ctx, {.id = "blockwork.prefabs",
                       .label = "Prefabs",
                       .category = "Blockwork",
                       .run = [] { panels::show(kPrefabPanelId); },
                       .doc = "Open Prefabs: save blocks and entities for reuse across scenes and place them.",
                       .icon = "packages",
                       .keywords = {"prefab", "instance", "kit"}});
    actions::add(ctx, {.id = "blockwork.merge",
                       .label = "Merge Level",
                       .category = "Blockwork",
                       .doc = "Merge the Level group's blocks into one mesh with collision hulls, tool faces left out.",
                       .python = "blockworkMerge('Level')",
                       .command = "blockworkMerge",
                       .call = json::object(),
                       .icon = "box-multiple",
                       .keywords = {"compile", "bake", "export"}});
    actions::add(ctx, {.id = "blockwork.toolMaterials",
                       .label = "Tool Materials",
                       .category = "Blockwork",
                       .doc = "Make the nodraw, clip and skip tool materials, for faces that are not drawn or do not collide.",
                       .python = "blockworkToolMaterials()",
                       .command = "blockworkToolMaterials",
                       .call = json::object(),
                       .icon = "texture",
                       .keywords = {"nodraw", "clip", "skip"}});
}
} // namespace blockwork
