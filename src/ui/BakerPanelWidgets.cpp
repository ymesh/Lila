#include "ui/BakerPanelInternal.h"
#include "ui/Controls.h"
#include "ui/UiCommands.h"

#include <QGridLayout>

namespace lila::ui
{
using namespace bakerPanel;

namespace
{
bool isRemoveKey(const QKeyEvent* k)
{
    return (k->key() == Qt::Key_Delete || k->key() == Qt::Key_Backspace) && (k->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
}

/** Drops of outliner nodes onto group rows; Delete and Backspace remove the selected rows, never scene objects. */
class GroupTree : public QTreeWidget
{
public:
    using QTreeWidget::QTreeWidget;
    std::function<void(const QStringList&, QTreeWidgetItem*)> dropped;
    std::function<void()> removeRows;

protected:
    bool event(QEvent* e) override
    {
        if (e->type() == QEvent::ShortcutOverride && removeRows && isRemoveKey(static_cast<QKeyEvent*>(e)))
        {
            e->accept();
            return true;
        }
        return QTreeWidget::event(e);
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (!removeRows || !isRemoveKey(e))
            return QTreeWidget::keyPressEvent(e);
        e->accept();
        removeRows();
    }
    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (!e->mimeData()->hasFormat(kNodeNamesMime))
            return QTreeWidget::dragEnterEvent(e);
        e->setDropAction(Qt::LinkAction);
        e->accept();
    }
    void dragMoveEvent(QDragMoveEvent* e) override
    {
        if (!e->mimeData()->hasFormat(kNodeNamesMime))
            return QTreeWidget::dragMoveEvent(e);
        e->setDropAction(Qt::LinkAction);
        e->accept();
    }
    void dropEvent(QDropEvent* e) override
    {
        if (!e->mimeData()->hasFormat(kNodeNamesMime))
            return QTreeWidget::dropEvent(e);
        const QStringList names = QString::fromUtf8(e->mimeData()->data(kNodeNamesMime)).split('\n', Qt::SkipEmptyParts);
        QTreeWidgetItem* at = itemAt(e->position().toPoint());
        e->setDropAction(Qt::LinkAction);
        e->accept();
        if (dropped)
            dropped(names, at);
    }
};

/**
 * Results thumbnails drag out with the middle button (the left one selects), as their files plus the bake outputs, so a
 * drop on a mesh knows each one's map.
 */
class ThumbList : public QListWidget
{
public:
    using QListWidget::QListWidget;

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::MiddleButton)
            return QListWidget::mousePressEvent(e);
        mPressed = itemAt(e->position().toPoint());
        mPressAt = e->position().toPoint();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!(e->buttons() & Qt::MiddleButton) || !mPressed)
            return QListWidget::mouseMoveEvent(e);
        if ((e->position().toPoint() - mPressAt).manhattanLength() < QApplication::startDragDistance())
            return;
        QList<QListWidgetItem*> items = mPressed->isSelected() ? selectedItems() : QList<QListWidgetItem*>{mPressed};
        mPressed = nullptr;
        auto* drag = new QDrag(this);
        drag->setMimeData(mimeData(items));
        drag->setPixmap(items.front()->icon().pixmap(iconSize() / 2));
        drag->exec(Qt::CopyAction);
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::MiddleButton)
            mPressed = nullptr;
        QListWidget::mouseReleaseEvent(e);
    }
    QStringList mimeTypes() const override { return {"text/uri-list", kOutputsMime}; }
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        auto* m = new QMimeData;
        QList<QUrl> urls;
        json outputs = json::array();
        for (const QListWidgetItem* it : items)
        {
            urls << QUrl::fromLocalFile(it->data(kFileRole).toString());
            outputs.push_back(json::parse(it->data(kOutputRole).toString().toStdString(), nullptr, false));
        }
        m->setUrls(urls);
        m->setData(kOutputsMime, QByteArray::fromStdString(outputs.dump()));
        return m;
    }

private:
    QListWidgetItem* mPressed = nullptr;
    QPoint mPressAt;
};

/** An issue row's edges or faces, selected on its mesh (ids of its outMesh). */
void selectIssueComponents(QTreeWidgetItem* it)
{
    const QString stored = it->data(0, kComponentsRole).toString();
    const std::string node = ss(it->data(0, kNodeRole).toString());
    scene::Node* n = stored.isEmpty() || node.empty() ? nullptr : findNode(node);
    if (!n || !n->type().isA("mesh"))
        return;
    const json c = json::parse(stored.toStdString(), nullptr, false);
    json call = {{"args", {n->path()}}};
    if (c.contains("edgePoints"))
    {
        scene::MeshPtr m;
        try
        {
            m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        }
        catch (const std::exception&)
        {
        }
        if (!m)
            return;
        json edges = json::array();
        for (const json& pr : c["edgePoints"])
        {
            const geo::Index a = pr[0].get<geo::Index>(), b = pr[1].get<geo::Index>();
            if (a >= m->pointCapacity() || b >= m->pointCapacity() || !m->pointValid(a))
                continue;
            m->forEachPointEdge(a, [&](geo::Index e) {
                if (m->edgeOtherPoint(e, a) == b)
                    edges.push_back(e);
            });
        }
        call["edges"] = std::move(edges);
    }
    else if (c.contains("faces"))
        call["faces"] = c["faces"];
    else
        return;
    UndoChunk chunk("Select Issue");
    runUi("select", {{"args", {n->path()}}});
    runUi("selectComponents", call);
}

/** A labelled button on the raised fill, for the Baker's weekly actions. */
QToolButton* bakerPush(QWidget* parent, const QString& text, const QString& tip, bool checkable = false)
{
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setAccessibleName(text);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    b->setCheckable(checkable);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    controls::setRole(b, "push");
    return b;
}

/** Label/field rows with the labels right-aligned in one column. */
QFormLayout* bakerForm(QWidget* parent = nullptr)
{
    auto* f = new QFormLayout(parent);
    f->setContentsMargins(0, 0, 0, 0);
    f->setHorizontalSpacing(6);
    f->setVerticalSpacing(4);
    f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    f->setRowWrapPolicy(QFormLayout::DontWrapRows);
    return f;
}

/** A dim one-line sub-header inside a tab. */
QLabel* bakerSubHeader(QWidget* parent, const QString& text)
{
    auto* l = new QLabel(text, parent);
    QFont f = l->font();
    f.setWeight(QFont::DemiBold);
    l->setFont(f);
    l->setContentsMargins(0, 6, 0, 0);
    return l;
}

void bakerNumberStyle(QAbstractSpinBox* s)
{
    s->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
}

QDoubleSpinBox* distanceSpin(QWidget* parent, const QString& tip)
{
    auto* s = new QDoubleSpinBox(parent);
    bakerNumberStyle(s);
    s->setRange(0.0, 1e6);
    s->setDecimals(3);
    s->setSingleStep(0.1);
    s->setSuffix(" cm");
    s->setSpecialValueText("Auto");
    s->setKeyboardTracking(false);
    s->setToolTip(tip);
    setSpinDefault(s, 0.0);
    return s;
}
} // namespace

QWidget* BakerPanel::buildToolbar()
{
    auto* w = new QWidget(this);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);

    auto* top = new QHBoxLayout;
    top->setSpacing(2);
    auto* topForm = bakerForm();
    topForm->addRow("Project", top);
    mProjects = new QComboBox(w);
    mProjects->setObjectName("bakerProjects");
    mProjects->setToolTip("The bake project shown: output settings, maps and groups");
    mProjects->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    mProjects->setMinimumContentsLength(10);
    connect(mProjects, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!mUpdating)
            setProject(ss(mProjects->currentData().toString()));
    });
    top->addWidget(mProjects, 1);
    mProjectMenu = toolButton(w, "dots", "Project: new, rename, delete, presets, output folder");
    mProjectMenu->setPopupMode(QToolButton::InstantPopup);
    auto* pm = new QMenu(mProjectMenu);
    pm->addAction(icon("plus"), "New Project", this, [this] {
        json r = run("createBakeProject", newProjectArgs(), false);
        if (r.is_string())
            setProject(r.get<std::string>());
    });
    pm->addAction(icon("pencil"), "Rename Project...", this, [this] {
        if (mProject.empty())
            return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Rename Bake Project", "Name", QLineEdit::Normal, qs(shortName(mProject)), &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
        json r = runUi("rename", {{"args", {mProject, ss(name)}}});
        if (r.is_string())
            setProject(r.get<std::string>());
    });
    pm->addAction(icon("trash"), "Delete Project", this, [this] {
        if (mProject.empty())
            return;
        std::vector<std::string> doomed;
        scene::Graph& g = graph();
        if (scene::Node* p = findNode(mProject))
            for (scene::NodeId id : bake::groupsOf(g, p->id()))
                doomed.push_back(g.node(id)->path());
        doomed.push_back(mProject);
        runUi("delete", {{"args", doomed}});
    });
    pm->addSeparator();
    pm->addAction(icon("device-floppy"), "Save Settings as Preset...", this, [this] {
        mPresetRow->show();
        mPresetName->setFocus();
        mPresetName->selectAll();
    });
    QAction* deletePreset = pm->addAction(icon("trash"), "Delete Preset", this, [this] {
        if (mTargets->currentData(kPresetRole).toBool() && runUi("deleteBakeTarget", {{"args", {mTarget}}}).is_string())
        {
            mTarget.clear();
            refreshTargets();
            updateButtons();
        }
    });
    pm->addSeparator();
    QAction* openOutput = pm->addAction(icon("folder"), "Open Output Folder", this, [this] { openFolder(outputFolder()); });
    connect(pm, &QMenu::aboutToShow, this, [this, deletePreset, openOutput] {
        openOutput->setEnabled(!outputFolder().isEmpty());
        const bool preset = mTargets->currentData(kPresetRole).toBool();
        deletePreset->setEnabled(preset);
        deletePreset->setText(preset ? "Delete Preset " + mTargets->currentText() : QString("Delete Preset"));
    });
    mProjectMenu->setMenu(pm);
    top->addWidget(mProjectMenu);

    mTargets = new QComboBox(w);
    mTargets->setObjectName("bakerTargets");
    mTargets->setToolTip("Target profile: the maps, conventions and file names a downstream tool expects. Picking one applies them to the project.");
    connect(mTargets, &QComboBox::currentIndexChanged, this, [this](int) {
        if (mUpdating)
            return;
        mTarget = ss(mTargets->currentData().toString());
        PrefStore::get().set(kPrefs, "/target", mTarget);
        if (const json r = run("applyBakeTarget", {{"args", {mTarget}}}, !mProject.empty()); r.is_object())
        {
            if (const std::string be = r.value("backend", std::string()); !be.empty())
                PrefStore::get().set(kPrefs, "/backend", be);
            if (mProject.empty())
                refreshProjects();
        }
        updateButtons();
    });
    mTargets->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    mTargets->setMinimumContentsLength(10);
    topForm->addRow("Target", mTargets);
    v->addLayout(topForm);

    mPresetRow = new QWidget(w);
    mPresetRow->setObjectName("bakerPresetRow");
    auto* prv = new QVBoxLayout(mPresetRow);
    prv->setContentsMargins(0, 0, 0, 0);
    prv->setSpacing(2);
    auto* pr = new QHBoxLayout;
    pr->setSpacing(2);
    mPresetName = new QLineEdit(mPresetRow);
    mPresetName->setObjectName("bakerPresetName");
    mPresetName->setPlaceholderText("Preset name");
    mPresetName->setToolTip("Saves the output, maps, backend and group defaults (no meshes) as a target you can pick in any scene");
    connect(mPresetName, &QLineEdit::returnPressed, this, &BakerPanel::savePreset);
    prv->addWidget(mPresetName);
    mPresetScope = new QComboBox(mPresetRow);
    mPresetScope->setObjectName("bakerPresetScope");
    mPresetScope->addItem("User", "user");
    mPresetScope->addItem("Workspace", "workspace");
    setComboDefault(mPresetScope, 0);
    mPresetScope->setToolTip("User: every project on this machine. Workspace: this workspace's .lila/bake/targets, shared with it");
    pr->addWidget(mPresetScope);
    mPresetReplace = new QCheckBox("Replace", mPresetRow);
    mPresetReplace->setToolTip("Overwrite a preset of the same name");
    setCheckDefault(mPresetReplace, false);
    pr->addWidget(mPresetReplace);
    pr->addStretch(1);
    QToolButton* savePresetButton = bakerPush(mPresetRow, "Save", "Save: save the preset");
    savePresetButton->setObjectName("bakerPresetSave");
    savePresetButton->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    connect(savePresetButton, &QToolButton::clicked, this, &BakerPanel::savePreset);
    pr->addWidget(savePresetButton);
    QToolButton* cancelPreset = bakerPush(mPresetRow, "Cancel", "Cancel: close without saving");
    cancelPreset->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    connect(cancelPreset, &QToolButton::clicked, mPresetRow, &QWidget::hide);
    pr->addWidget(cancelPreset);
    prv->addLayout(pr);
    mPresetRow->hide();
    v->addWidget(mPresetRow);

    mBakeTarget = new QToolButton(w);
    mBakeTarget->setObjectName("bakerBakeTarget");
    mBakeTarget->setIcon(icon("target"));
    mBakeTarget->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mBakeTarget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    mBakeTarget->setMinimumHeight(32);
    QFont bold = mBakeTarget->font();
    bold.setWeight(QFont::DemiBold);
    mBakeTarget->setFont(bold);
    mBakeTarget->setPopupMode(QToolButton::MenuButtonPopup);
    controls::setRole(mBakeTarget, "push");
    mBakeTarget->setToolTip("Bake for Target: apply the target's maps and naming, auto-group by _low/_high names when there are no "
                            "groups, and bake everything it needs");
    connect(mBakeTarget, &QToolButton::clicked, this, [this] { bakeForTarget(false); });
    auto* tm = new QMenu(mBakeTarget);
    QAction* optional = tm->addAction("Include Optional Maps");
    optional->setCheckable(true);
    optional->setChecked(PrefStore::get().value<bool>(kPrefs, "/optional", false));
    connect(optional, &QAction::toggled, this, [](bool on) { PrefStore::get().set(kPrefs, "/optional", on); });
    tm->addAction("Apply Target Only", this, [this] { bakeForTarget(true); })->setToolTip("Set the maps and naming without baking");
    mBakeTarget->setMenu(tm);
    v->addWidget(mBakeTarget);

    auto* row = new QHBoxLayout;
    row->setSpacing(2);
    mBake = bakerPush(w, "Bake All", "Bake All: bake every group of the project");
    mBake->setObjectName("bakerBake");
    connect(mBake, &QToolButton::clicked, this, [this] { bake({}); });
    row->addWidget(mBake);
    mBakeSelected = bakerPush(w, "Bake Selected", "Bake Selected: the groups selected here, or the groups holding the selected meshes");
    connect(mBakeSelected, &QToolButton::clicked, this, [this] {
        std::vector<std::string> groups = selectedGroups();
        if (groups.empty())
            groups = groupsForSelection(mProject);
        if (groups.empty())
        {
            log::warn("Bake Selected: select a group here, or a mesh one holds");
            return;
        }
        bake(groups);
    });
    row->addWidget(mBakeSelected);
    mCancel = bakerPush(w, "Cancel", "Cancel: stop the running bake");
    connect(mCancel, &QToolButton::clicked, this, [] { cancelBakes(); });
    row->addWidget(mCancel);
    v->addLayout(row);
    auto* status = new QHBoxLayout;
    status->setSpacing(4);
    mProgress = new QProgressBar(w);
    mProgress->setObjectName("bakerProgress");
    mProgress->setRange(0, 1000);
    mProgress->setTextVisible(false);
    mProgress->setMaximumHeight(10);
    status->addWidget(mProgress, 1);
    mLintButton = toolButton(w, "alert-triangle", "Check the project for problems (bakeLint)", "Check");
    mLintButton->setObjectName("bakerLint");
    connect(mLintButton, &QToolButton::clicked, this, [this] { runLint(true); });
    status->addWidget(mLintButton);
    mBackend = new QToolButton(w);
    mBackend->setObjectName("bakerBackend");
    mBackend->setPopupMode(QToolButton::InstantPopup);
    mBackend->setToolButtonStyle(Qt::ToolButtonTextOnly);
    mBackend->setToolTip("Where bakes run");
    QFont mono = mBackend->font();
    mono.setBold(true);
    if (mono.pointSizeF() > 0.0)
        mono.setPointSizeF(mono.pointSizeF() * 0.85);
    mBackend->setFont(mono);
    auto* bm = new QMenu(mBackend);
    connect(bm, &QMenu::aboutToShow, this, [this, bm] {
        bm->clear();
        const std::string cur = backendPref();
        for (const std::string& b : bake::backendNames())
        {
            QAction* a = bm->addAction(backendBadge(b) + "  (" + qs(b) + ")", this, [this, b] {
                PrefStore::get().set(kPrefs, "/backend", b);
                updateButtons();
            });
            a->setCheckable(true);
            a->setChecked(b == cur);
        }
    });
    mBackend->setMenu(bm);
    status->addWidget(mBackend);
    v->addLayout(status);
    return w;
}

QWidget* BakerPanel::buildGroups()
{
    auto* w = new QWidget(this);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(1);

    auto* daily = new QHBoxLayout;
    daily->setSpacing(2);
    auto* bar = new QHBoxLayout;
    bar->setSpacing(2);
    QToolButton* add = bakerPush(w, "New Group", "New Group: the selected meshes and the meshes under them become its lows. Shift+click uses only the selected meshes.");
    connect(add, &QToolButton::clicked, this, [this] {
        json call = json::object();
        if (const std::vector<std::string> m = selectedMeshes(); !m.empty())
            call["lows"] = m;
        run("bakeGroup", call);
    });
    bar->insertWidget(0, add);
    // No current group: bakeGroup without a name creates one.
    auto addSelected = [this](const char* flag) {
        const std::vector<std::string> meshes = selectedMeshes();
        if (meshes.empty())
            return;
        json call = {{flag, meshes}};
        if (const std::string g = currentGroup(); !g.empty())
            call["args"] = {g};
        run("bakeGroup", call);
    };
    QToolButton* low = bakerPush(w, "Add Low",
                                 "Add Low: the selected meshes and the meshes under them join the current group (a new one if none) "
                                 "as lows. Shift+click adds only the selected meshes.");
    connect(low, &QToolButton::clicked, this, [addSelected] { addSelected("addLow"); });
    daily->addWidget(low);
    QToolButton* high = bakerPush(w, "Add High",
                                  "Add High: the selected meshes and the meshes under them join the current group (a new one if none) "
                                  "as highs. Shift+click adds only the selected meshes.");
    connect(high, &QToolButton::clicked, this, [addSelected] { addSelected("addHigh"); });
    daily->addWidget(high);
    QToolButton* autoGroup = bakerPush(w, "Auto Group", "Auto Group: by name, _low/_high, _lo/_hi, _LP/_HP (the selection, else every mesh)");
    autoGroup->setObjectName("bakerAutoGroup");
    connect(autoGroup, &QToolButton::clicked, this, [this] { run("autoGroupBake", json::object()); });
    daily->addWidget(autoGroup);
    v->addLayout(daily);
    QToolButton* perMesh = bakerPush(w, "Per Mesh", "Per Mesh: one group per selected mesh, each baking onto itself");
    connect(perMesh, &QToolButton::clicked, this, [] { groupPerMesh(); });
    bar->addWidget(perMesh);
    mArrange = bakerPush(w, "Arrange", "Arrange: draw the groups side by side to inspect them; the meshes don't move and bakes run where they are", true);
    mArrange->setObjectName("bakerArrange");
    connect(mArrange, &QToolButton::clicked, this, [this] {
        run("bakeArrange", {{"on", mArrange->isChecked()}});
        updateButtons();
    });
    bar->addWidget(mArrange);
    bar->addSpacing(4);
    bar->addWidget(docs::helpButton("baking", w));
    QToolButton* remove = toolButton(w, "trash", "Remove: the selected rows, groups or meshes from their group (Delete)");
    remove->setObjectName("bakerRemoveRows");
    connect(remove, &QToolButton::clicked, this, &BakerPanel::removeSelectedRows);
    bar->addWidget(remove);
    v->addLayout(bar);

    auto* tree = new GroupTree(w);
    mGroups = tree;
    mGroups->setObjectName("bakerGroups");
    mGroups->setColumnCount(2);
    mGroups->setHeaderHidden(true);
    mGroups->setUniformRowHeights(true);
    mGroups->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mGroups->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mGroups->setAcceptDrops(true);
    mGroups->setDragDropMode(QAbstractItemView::DropOnly);
    mGroups->setContextMenuPolicy(Qt::CustomContextMenu);
    mGroups->setToolTip("Drag meshes here from the outliner: onto a group or its Low rows as lows, onto High rows as highs, "
                        "onto Cage as its cage, onto empty space for a new group");
    mGroups->header()->setStretchLastSection(true);
    mGroups->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    // Rebuilds run with updates off; only the user's folding is remembered.
    auto toggleFold = [this](QTreeWidgetItem* it) {
        const QString key = it->data(0, kFoldKeyRole).toString();
        if (key.isEmpty() || !mGroups->updatesEnabled())
            return;
        if (!mLintToggled.removeOne(key))
            mLintToggled << key;
    };
    connect(mGroups, &QTreeWidget::itemExpanded, this, toggleFold);
    connect(mGroups, &QTreeWidget::itemCollapsed, this, toggleFold);
    Hotkeys::passThrough(mGroups);
    mGroups->setProperty("hotkeyContext", "baker");
    deletion::claim(mGroups, [this] { removeSelectedRows(); });
    tree->dropped = [this](const QStringList& names, QTreeWidgetItem* at) { dropNodes(names, at); };
    tree->removeRows = [this] { removeSelectedRows(); };
    connect(mGroups, &QTreeWidget::currentItemChanged, this, [this] {
        if (!mUpdating)
            refreshDetails();
        updateButtons();
    });
    connect(mGroups, &QTreeWidget::itemClicked, this, [](QTreeWidgetItem* it, int) { selectIssueComponents(it); });
    connect(mGroups, &QTreeWidget::itemDoubleClicked, this, [](QTreeWidgetItem* it, int) {
        const std::string n = ss(it->data(0, kNodeRole).toString());
        if (!n.empty())
            runUi("select", {{"args", {n}}});
    });
    connect(mGroups, &QTreeWidget::customContextMenuRequested, this, &BakerPanel::groupMenu);
    v->addWidget(mGroups, 1);
    return w;
}

QWidget* BakerPanel::buildGroupDetails()
{
    mDetails = new QWidget(this);
    auto* v = new QVBoxLayout(mDetails);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);
    mDetailsEmpty = new QLabel("No group selected. Pick one in the list above, or add one with Add Low.", mDetails);
    mDetailsEmpty->setWordWrap(true);
    mDetailsEmpty->setForegroundRole(QPalette::PlaceholderText);
    v->addWidget(mDetailsEmpty);

    QFormLayout* form = bakerForm();
    auto edit = [this](const char* flag, double value) {
        const std::string g = currentGroup();
        if (mUpdating || g.empty())
            return;
        scene::Node* n = findNode(g);
        if (!n)
            return;
        const bake::GroupSettings s = bake::groupSettings(*n);
        const double cur = std::string_view(flag) == "frontal" ? s.frontal : std::string_view(flag) == "rear" ? s.rear : s.cageOffset;
        if (std::abs(cur - value) > 1e-9)
            run("bakeGroup", {{"args", {g}}, {flag, value}}, false);
    };
    mFrontal = distanceSpin(mDetails, "How far outside the low rays start; Auto measures it against the highs (shown after a bake or Check)");
    connect(mFrontal, &QDoubleSpinBox::valueChanged, this, [edit](double x) { edit("frontal", x); });
    form->addRow("Frontal", mFrontal);
    mRear = distanceSpin(mDetails, "How far inside the low rays reach; Auto measures it against the highs (shown after a bake or Check)");
    connect(mRear, &QDoubleSpinBox::valueChanged, this, [edit](double x) { edit("rear", x); });
    form->addRow("Rear", mRear);
    mCageOffset = new QDoubleSpinBox(mDetails);
    bakerNumberStyle(mCageOffset);
    mCageOffset->setRange(0.0, 100.0);
    mCageOffset->setDecimals(3);
    mCageOffset->setSingleStep(0.05);
    mCageOffset->setSuffix(QString(" %1").arg(QChar(0x00D7)));
    mCageOffset->setKeyboardTracking(false);
    mCageOffset->setToolTip("Cage Offset: scales the auto cage's push; a painted bakeOffset point attribute scales it per point");
    setSpinDefault(mCageOffset, 1.0);
    connect(mCageOffset, &QDoubleSpinBox::valueChanged, this, [edit](double x) { edit("cageOffset", x); });
    form->addRow("Cage Offset", mCageOffset);

    mCageRow = new QWidget(mDetails);
    auto* cageRow = new QGridLayout(mCageRow);
    cageRow->setContentsMargins(0, 0, 0, 0);
    cageRow->setSpacing(2);
    mShowCage = bakerPush(mCageRow, "Show Cage", "Show Cage: the group's cage in the viewport, a translucent shell where rays start", true);
    mShowCage->setObjectName("bakerShowCage");
    connect(mShowCage, &QToolButton::clicked, this, [this](bool on) {
        if (scene::Node* n = findNode(currentGroup()); n && bake::isGroup(*n))
            bakeCage::setShown(n->id(), on);
    });
    QToolButton* paint = bakerPush(mCageRow, "Paint", "Paint: the cage's offsets on the low, in the viewport or the UV editor");
    connect(paint, &QToolButton::clicked, this, [this] {
        scene::Node* n = findNode(currentGroup());
        if (!n || !bake::isGroup(*n) || bake::groupSettings(*n).lows.empty())
            return;
        runUi("select", {{"args", {bake::groupSettings(*n).lows.front()}}});
        actions::run(json("bake.paintCage"));
    });
    QToolButton* autoPaint = bakerPush(mCageRow, "Auto Paint", "Auto Paint: paint the offsets every ray needs to start outside the high and meet it, and set Frontal and Rear");
    autoPaint->setObjectName("bakerAutoPaintCage");
    connect(autoPaint, &QToolButton::clicked, this, [this] {
        const std::string g = currentGroup();
        if (g.empty())
            return;
        const json r = run("bakeAutoPaintCage", {{"args", {g}}}, false);
        if (r.is_object() && r.contains("after"))
            mCageStatus->setText(QString("Rays missing: %1% before, %2% after")
                                     .arg(r["before"].value("missRate", 0.0) * 100.0, 0, 'f', 2)
                                     .arg(r["after"].value("missRate", 0.0) * 100.0, 0, 'f', 2));
    });
    QToolButton* resetPaint = bakerPush(mCageRow, "Reset Paint", "Reset Paint: clear the painted cage offsets and skew on the group's lows");
    resetPaint->setObjectName("bakerResetCagePaint");
    connect(resetPaint, &QToolButton::clicked, this, [this] {
        scene::Node* n = findNode(currentGroup());
        if (!n || !bake::isGroup(*n) || bake::groupSettings(*n).lows.empty())
            return;
        const std::vector<std::string> lows = bake::groupSettings(*n).lows;
        UndoChunk chunk("Reset Bake Cage Paint");
        for (const char* attr : {"offset", "skew"})
            runUi("bakePaint", {{"args", lows}, {"attr", attr}, {"reset", true}});
        mCageStatus->clear();
    });
    mLivePreview = bakerPush(mCageRow, "Live Preview",
                             "Live Preview: rebake a low resolution preview on the lows as the cage changes; nothing is written", true);
    mLivePreview->setObjectName("bakerLivePreview");
    connect(mLivePreview, &QToolButton::clicked, this, [](bool on) { bakeCage::setLivePreview(on); });
    bakeCage::onChanged(this, [this] { scheduleRefresh(); });
    cageRow->addWidget(mShowCage, 0, 0);
    cageRow->addWidget(mLivePreview, 0, 1);
    cageRow->addWidget(paint, 1, 0);
    cageRow->addWidget(autoPaint, 1, 1);
    cageRow->addWidget(resetPaint, 2, 0);
    mAoScope = new QComboBox(mDetails);
    mAoScope->addItem("Group", "group");
    mAoScope->addItem("Self", "self");
    mAoScope->addItem("All Groups", "all");
    setComboDefault(mAoScope, mAoScope->findData(qs(bake::GroupSettings{}.aoScope)));
    mAoScope->setToolTip("AO Occluders: which highs occlude AO rays. Group: this group's, so overlapping groups never shadow each other. "
                         "Self: only the hit high. All Groups: every group's");
    connect(mAoScope, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!mUpdating && !currentGroup().empty())
            run("bakeGroup", {{"args", {currentGroup()}}, {"aoScope", ss(mAoScope->currentData().toString())}}, false);
    });
    form->addRow("AO Occluders", mAoScope);

    v->addLayout(form);
    v->addWidget(bakerSubHeader(mDetails, "Cage"));
    v->addWidget(mCageRow);
    mCageStatus = new QLabel(mDetails);
    mCageStatus->setWordWrap(true);
    v->addWidget(mCageStatus);

    auto* unlinkBox = new QWidget(mDetails);
    auto* uv = new QVBoxLayout(unlinkBox);
    uv->setContentsMargins(0, 0, 0, 0);
    uv->setSpacing(1);
    mUnlinks = new QTreeWidget(unlinkBox);
    mUnlinks->setObjectName("bakerUnlinks");
    mUnlinks->setMaximumHeight(170);
    mUnlinks->setUniformRowHeights(true);
    mUnlinks->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mUnlinks->setHeaderLabels({"Low", "Skip Projection"});
    mUnlinks->headerItem()->setToolTip(0, "Meshes and materials under a low don't occlude it");
    mUnlinks->headerItem()->setToolTip(1, "They don't project onto it either");
    mUnlinks->header()->setStretchLastSection(false);
    mUnlinks->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    mUnlinks->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    connect(mUnlinks, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        scene::Node* n = findNode(currentGroup());
        const int row = mUnlinks->indexOfTopLevelItem(item);
        if (mUpdating || column != 1 || row < 0 || !n || !bake::isGroup(*n))
            return;
        std::vector<bake::Unlink> all = bake::groupSettings(*n).unlinks;
        if (row >= int(all.size()))
            return;
        all[size_t(row)].projection = item->checkState(1) == Qt::Checked;
        setUnlinks(currentGroup(), all);
    });
    connect(mUnlinks, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) {
        if (!mUpdating && !mUnlinksCollapsed.contains(item->text(0)))
            mUnlinksCollapsed << item->text(0);
    });
    connect(mUnlinks, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        if (!mUpdating)
            mUnlinksCollapsed.removeAll(item->text(0));
    });
    uv->addWidget(mUnlinks);
    auto* ub = new QHBoxLayout;
    ub->setSpacing(1);
    QToolButton* unlinkAdd = bakerPush(unlinkBox, "Unlink Selected",
                                       "Unlink Selected: the selected meshes stop occluding the current Low row, or every low when a "
                                       "group row is current. Shift+click: they also stop projecting onto it.");
    connect(unlinkAdd, &QToolButton::clicked, this, [this] {
        QTreeWidgetItem* it = mGroups->currentItem();
        const std::string low = it && it->data(0, kKindRole).toInt() == kLowItem ? ss(it->data(0, kNodeRole).toString()) : std::string();
        unlinkSelected(currentGroup(), low, QApplication::keyboardModifiers() & Qt::ShiftModifier);
    });
    ub->addWidget(unlinkAdd);
    QToolButton* unlinkMaterialButton = bakerPush(unlinkBox, "Unlink Material",
                                                  "Unlink Material: every high with faces of the picked material stops occluding the current "
                                                  "Low row, or every low when a group row is current. Shift+click a material: they also stop "
                                                  "projecting onto it.");
    unlinkMaterialButton->setObjectName("bakerUnlinkMaterial");
    unlinkMaterialButton->setPopupMode(QToolButton::InstantPopup);
    auto* materialMenu = new QMenu(unlinkMaterialButton);
    unlinkMaterialButton->setMenu(materialMenu);
    connect(materialMenu, &QMenu::aboutToShow, this, [this, materialMenu] {
        materialMenu->clear();
        QTreeWidgetItem* it = mGroups->currentItem();
        const std::string low = it && it->data(0, kKindRole).toInt() == kLowItem ? ss(it->data(0, kNodeRole).toString()) : std::string();
        const std::string group = currentGroup();
        for (const std::string& m : unlinkableMaterials())
            materialMenu->addAction(icon("palette"), qs(m), this, [this, group, low, m] {
                unlinkMaterial(group, low, m, QApplication::keyboardModifiers() & Qt::ShiftModifier);
            });
        if (materialMenu->isEmpty())
            materialMenu->addAction("No materials on the highs")->setEnabled(false);
    });
    ub->addWidget(unlinkMaterialButton);
    QToolButton* unlinkRemove = toolButton(unlinkBox, "trash", "Remove: the selected meshes and materials, or everything under a selected low row");
    unlinkRemove->setObjectName("bakerUnlinkRemove");
    connect(unlinkRemove, &QToolButton::clicked, this, [this] { removeSelectedUnlinks(); });
    ub->addWidget(unlinkRemove);
    uv->addLayout(ub);
    mUnlinks->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(mUnlinks, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        QTreeWidgetItem* item = mUnlinks->itemAt(at);
        if (!item)
            return;
        if (!item->isSelected())
        {
            mUnlinks->clearSelection();
            item->setSelected(true);
        }
        QMenu menu(this);
        menu.addAction(icon("trash"), "Remove", this, [this] { removeSelectedUnlinks(); });
        menu.exec(mUnlinks->viewport()->mapToGlobal(at));
    });
    mBackfaces = new QCheckBox("Ignore backfaces", mDetails);
    setCheckDefault(mBackfaces, bake::GroupSettings{}.ignoreBackfaces);
    mBackfaces->setToolTip("Rays only hit high faces facing them");
    connect(mBackfaces, &QCheckBox::toggled, this, [this](bool on) {
        if (!mUpdating && !currentGroup().empty())
            run("bakeGroup", {{"args", {currentGroup()}}, {"ignoreBackfaces", on}}, false);
    });
    form->addRow(QString(), mBackfaces);
    mSelf = new QCheckBox("Self as high", mDetails);
    setCheckDefault(mSelf, false);
    mSelf->setToolTip("Bake the lows onto themselves through a bake-only modifier stack (e.g. Smooth x2): no high mesh needed");
    connect(mSelf, &QCheckBox::toggled, this, [this](bool on) {
        if (!mUpdating && !currentGroup().empty())
            run("bakeGroup", {{"args", {currentGroup()}}, {"self", on}}, false);
    });
    form->addRow(QString(), mSelf);
    v->addWidget(bakerSubHeader(mDetails, "Unlinked"));
    v->addWidget(unlinkBox);

    mStackBox = new QWidget(mDetails);
    auto* sv = new QVBoxLayout(mStackBox);
    sv->setContentsMargins(0, 4, 0, 0);
    sv->setSpacing(2);
    mStackTitle = new QLabel(mStackBox);
    mStackTitle->setWordWrap(true);
    {
        QFont f = mStackTitle->font();
        f.setWeight(QFont::DemiBold);
        mStackTitle->setFont(f);
    }
    sv->addWidget(mStackTitle);
    auto* pr = new QHBoxLayout;
    pr->addWidget(new QLabel("Preset", mStackBox));
    mPreset = new QComboBox(mStackBox);
    mPreset->setObjectName("bakerPreset");
    mPreset->addItem("Custom", QString());
    for (const char* p : {"None", "Smooth x1", "Smooth x2", "Smooth x3", "Smooth x4", "Auto Bevel + Smooth"})
        mPreset->addItem(p, p);
    mPreset->setToolTip("Bake-only modifiers evaluated into the bake's high snapshot; the scene never sees the dense mesh");
    connect(mPreset, &QComboBox::activated, this, [this](int i) {
        const QString preset = mPreset->itemData(i).toString();
        const std::string g = currentGroup();
        if (preset.isEmpty() || g.empty())
            return;
        json call = {{"args", {g}}, {"preset", ss(preset)}};
        if (QTreeWidgetItem* it = mGroups->currentItem(); it && it->data(0, kKindRole).toInt() == kHighItem)
        {
            const std::string n = ss(it->data(0, kNodeRole).toString());
            if (n.empty())
                call["self"] = true;
            else
                call["addHigh"] = {n};
        }
        run("bakeGroup", call, false);
    });
    pr->addWidget(mPreset, 1);
    sv->addLayout(pr);
    mStack = new QListWidget(mStackBox);
    mStack->setObjectName("bakerStack");
    mStack->setMaximumHeight(90);
    connect(mStack, &QListWidget::currentRowChanged, this, [this](int) { refreshParams(); });
    sv->addWidget(mStack);
    mStackButtons = new QWidget(mStackBox);
    auto* sb = new QHBoxLayout(mStackButtons);
    sb->setContentsMargins(0, 0, 0, 0);
    sb->setSpacing(1);
    QToolButton* addMod = toolButton(mStackButtons, "plus", "Add a modifier to the bake-only stack");
    addMod->setPopupMode(QToolButton::InstantPopup);
    auto* km = new QMenu(addMod);
    connect(km, &QMenu::aboutToShow, this, [this, km] {
        km->clear();
        for (const std::string& k : scene::modifiers::kinds())
        {
            QAction* a = km->addAction(qs(k), this, [this, k] {
                json s = currentStack();
                s.push_back({{"kind", k}, {"params", k == "subdivision" ? json{{"levels", 2}} : json::object()}});
                setStack(s);
                refreshDetails();
                mStack->setCurrentRow(mStack->count() - 1);
            });
            a->setToolTip(qs(scene::modifiers::kindDoc(k)));
        }
    });
    addMod->setMenu(km);
    sb->addWidget(addMod);
    auto move = [this](int delta) {
        json s = currentStack();
        const int r = mStack->currentRow(), to = r + delta;
        if (r < 0 || to < 0 || to >= int(s.size()))
            return;
        std::swap(s[r], s[to]);
        setStack(s);
        mStack->setCurrentRow(to);
    };
    QToolButton* up = toolButton(mStackButtons, "arrow-up", "Move the modifier earlier");
    connect(up, &QToolButton::clicked, this, [move] { move(-1); });
    sb->addWidget(up);
    QToolButton* down = toolButton(mStackButtons, "arrow-down", "Move the modifier later");
    connect(down, &QToolButton::clicked, this, [move] { move(1); });
    sb->addWidget(down);
    auto removeModifier = [this] {
        json s = currentStack();
        const int r = mStack->currentRow();
        if (r < 0 || r >= int(s.size()))
            return;
        s.erase(s.begin() + r);
        setStack(s);
    };
    deletion::claim(mStack, removeModifier);
    QToolButton* del = toolButton(mStackButtons, "trash", "Remove the modifier");
    connect(del, &QToolButton::clicked, this, removeModifier);
    sb->addWidget(del);
    sb->addStretch(1);
    sv->addWidget(mStackButtons);
    mParams = new QWidget(mStackBox);
    mParams->setObjectName("bakerStackParams");
    mParamForm = bakerForm(mParams);
    mParamForm->setContentsMargins(0, 2, 0, 0);
    sv->addWidget(mParams);
    v->addWidget(mStackBox);
    v->addStretch(1);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(mDetails);
    return scroll;
}

QWidget* BakerPanel::buildMaps()
{
    auto* w = new QWidget(this);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(2, 2, 2, 2);
    v->setSpacing(2);
    auto* bar = new QHBoxLayout;
    bar->setSpacing(1);
    mAddMap = bakerPush(w, "Add Map", "Add Map: add a map to the project");
    mAddMap->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    mAddMap->setPopupMode(QToolButton::InstantPopup);
    auto* am = new QMenu(mAddMap);
    connect(am, &QMenu::aboutToShow, this, [this, am] {
        am->clear();
        std::vector<std::string> have;
        if (scene::Node* p = findNode(mProject); p && bake::isProject(*p))
            for (const bake::MapSettings& m : bake::projectSettings(*p).maps)
                have.push_back(m.map);
        for (const bake::MapDef* d : bake::allMaps())
        {
            QAction* a = am->addAction(qs(d->label.empty() ? d->id : d->label), this, [this, id = d->id] { run("setBakeMap", {{"args", {id}}}); });
            a->setToolTip(qs(d->doc));
            a->setEnabled(std::ranges::find(have, d->id) == have.end());
        }
    });
    mAddMap->setMenu(am);
    bar->addWidget(mAddMap);
    bar->addStretch(1);
    QToolButton* remove = toolButton(w, "trash", "Remove: the selected maps from the project (Delete)");
    connect(remove, &QToolButton::clicked, this, [this] { removeSelectedMaps(); });
    bar->addWidget(remove);
    v->addLayout(bar);

    mMaps = new QTreeWidget(w);
    mMaps->setObjectName("bakerMaps");
    mMaps->setColumnCount(4);
    mMaps->setHeaderLabels({"Map", "Suffix", "Bits", "Format"});
    mMaps->setRootIsDecorated(false);
    mMaps->setUniformRowHeights(true);
    mMaps->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    mMaps->setSelectionMode(QAbstractItemView::ExtendedSelection);
    Hotkeys::passThrough(mMaps);
    deletion::claim(mMaps, [this] { removeSelectedMaps(); });
    connect(mMaps, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it, int column) {
        if (mUpdating || column != 0)
            return;
        run("setBakeMap", {{"args", {ss(it->data(0, kNodeRole).toString())}}, {"enabled", it->checkState(0) == Qt::Checked}});
    });
    connect(mMaps, &QTreeWidget::currentItemChanged, this, [this] {
        if (!mUpdating)
            refreshMapSettings();
    });
    v->addWidget(mMaps, 1);

    mMapSettings = new QWidget(w);
    mMapForm = bakerForm(mMapSettings);
    mMapForm->setContentsMargins(0, 2, 0, 0);
    v->addWidget(mMapSettings);
    return w;
}

QWidget* BakerPanel::buildOutput()
{
    auto* w = new QWidget(this);
    QFormLayout* form = bakerForm(w);
    form->setContentsMargins(4, 4, 4, 4);

    auto set = [this](const char* flag, json value) {
        if (mUpdating)
            return;
        json call = {{flag, std::move(value)}};
        if (const std::string ts = mTextureSet ? ss(mTextureSet->currentData().toString()) : std::string(); !ts.empty())
            call["material"] = ts;
        run("setBakeOutput", call);
    };

    auto* folderRow = new QHBoxLayout;
    mFolder = new QLineEdit(w);
    mFolder->setObjectName("bakerFolder");
    mFolder->setToolTip("Where textures are written; relative paths are under the workspace, else the scene file's folder");
    connect(mFolder, &QLineEdit::textEdited, this, [this] { mFolderResolved->setText(qs(bake::resolveFolder(ss(mFolder->text())).generic_string())); });
    connect(mFolder, &QLineEdit::editingFinished, this, [this, set] {
        if (scene::Node* p = findNode(mProject); p && bake::isProject(*p) && bake::projectSettings(*p).output.folder != ss(mFolder->text()))
            set("folder", ss(mFolder->text()));
    });
    folderRow->addWidget(mFolder, 1);
    QToolButton* browse = toolButton(w, "folder-open", "Browse: pick the output folder");
    connect(browse, &QToolButton::clicked, this, [this, set] {
        const QString start = qs(bake::resolveFolder(ss(mFolder->text())).generic_string());
        const QString dir = ui::fileDialog::getExistingDirectory(this, "Bake Output Folder", start);
        if (!dir.isEmpty())
            set("folder", ss(dir));
    });
    folderRow->addWidget(browse);
    QToolButton* open = toolButton(w, "folder", "Open Folder: show the output folder");
    connect(open, &QToolButton::clicked, this, [this] {
        openFolder(qs(bake::resolveFolder(ss(mFolder->text())).generic_string()));
    });
    folderRow->addWidget(open);
    form->addRow("Folder", folderRow);
    mFolderResolved = new QLabel(w);
    mFolderResolved->setObjectName("bakerFolderResolved");
    mFolderResolved->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Word wrap in a form row hits Qt's height-for-width bug and stretches the row over the whole tab.
    mFolderResolved->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    mFolderResolved->setToolTip("The folder the textures are written to");
    form->addRow(QString(), mFolderResolved);

    auto* nameRow = new QHBoxLayout;
    mName = new QLineEdit(w);
    mName->setObjectName("bakerName");
    mName->setToolTip("File name template; groups whose names resolve to the same file share one texture");
    connect(mName, &QLineEdit::textEdited, this, [this] { updatePreview(); });
    connect(mName, &QLineEdit::editingFinished, this, [this, set] {
        if (scene::Node* p = findNode(mProject); p && bake::isProject(*p) && bake::projectSettings(*p).output.name != ss(mName->text()))
            set("name", ss(mName->text()));
    });
    nameRow->addWidget(mName, 1);
    QToolButton* tokens = toolButton(w, "braces", "Tokens: insert a name token");
    tokens->setPopupMode(QToolButton::InstantPopup);
    auto* tokMenu = new QMenu(tokens);
    for (const std::string& t : bake::nameTokens())
        tokMenu->addAction("{" + qs(t) + "}", this, [this, t] {
            mName->insert("{" + qs(t) + "}");
            updatePreview();
            mName->setFocus();
        });
    tokens->setMenu(tokMenu);
    nameRow->addWidget(tokens);
    form->addRow("Name", nameRow);
    mPreview = new QTreeWidget(w);
    mPreview->setObjectName("bakerPreview");
    mPreview->setHeaderLabels({"File", "Size", "Format"});
    mPreview->setUniformRowHeights(true);
    mPreview->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mPreview->setMaximumHeight(180);
    mPreview->header()->setStretchLastSection(false);
    mPreview->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    mPreview->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    mPreview->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    auto openSet = [this](QTreeWidgetItem* it, bool open) {
        if (!mPreview->updatesEnabled() || it->parent())
            return;
        const QString set = it->data(0, Qt::UserRole).toString();
        mPreviewOpen.removeAll(set);
        if (open)
            mPreviewOpen << set;
    };
    connect(mPreview, &QTreeWidget::itemExpanded, this, [openSet](QTreeWidgetItem* it) { openSet(it, true); });
    connect(mPreview, &QTreeWidget::itemCollapsed, this, [openSet](QTreeWidgetItem* it) { openSet(it, false); });
    form->addRow("Files", mPreview);

    auto* setRow = new QHBoxLayout;
    mTextureSet = new QComboBox(w);
    mTextureSet->setObjectName("bakerTextureSet");
    mTextureSet->setToolTip("Which texture set the settings below are for: every set, or one material ({material}) with its own "
                            "format, bits, resolution, supersampling and padding. Needs {material} in the name.");
    connect(mTextureSet, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!mUpdating)
            refreshOutput();
    });
    setRow->addWidget(mTextureSet, 1);
    mTextureSetReset = toolButton(w, "arrow-back-up", "Reset: use the project's settings for this texture set again");
    connect(mTextureSetReset, &QToolButton::clicked, this, [this] {
        if (const std::string ts = ss(mTextureSet->currentData().toString()); !ts.empty())
            run("setBakeOutput", {{"material", ts}, {"clear", true}});
    });
    setRow->addWidget(mTextureSetReset);
    form->addRow(bakerSubHeader(w, "Format"));
    form->addRow("Texture Set", setRow);

    mFormat = new QComboBox(w);
    mFormat->addItem("PNG", "png");
    mFormat->addItem("OpenEXR (half)", "exr");
    mFormat->addItem("TGA", "tga");
    mFormat->addItem("TIFF", "tif");
    mFormat->addItem("PSD (layered)", "psd");
    const bake::OutputSettings defaults;
    setComboDefault(mFormat, mFormat->findData(qs(defaults.format)));
    connect(mFormat, &QComboBox::currentIndexChanged, this, [this, set](int) {
        set("format", ss(mFormat->currentData().toString()));
        updatePreview();
    });
    form->addRow("Format", mFormat);
    mBits = new QComboBox(w);
    mBits->addItem("8-bit", 8);
    mBits->addItem("16-bit", 16);
    mBits->addItem("32-bit Float", 32);
    setComboDefault(mBits, mBits->findData(defaults.bits));
    mBits->setToolTip("PNG and PSD take 8 or 16, TIFF 8, 16 or 32 float, EXR 16 (half) or 32 float; TGA is always 8");
    connect(mBits, &QComboBox::currentIndexChanged, this, [this, set](int) { set("bits", mBits->currentData().toInt()); });
    form->addRow("Bits", mBits);
    mResolution = new QComboBox(w);
    mResolution->setObjectName("bakerResolution");
    for (int r : {256, 512, 1024, 2048, 4096, 8192})
        mResolution->addItem(QString("%1 x %1").arg(r), r);
    setComboDefault(mResolution, mResolution->findData(defaults.width));
    connect(mResolution, &QComboBox::currentIndexChanged, this, [this, set](int) {
        if (const int r = mResolution->currentData().toInt(); r > 0)
            set("resolution", r);
    });
    form->addRow("Resolution", mResolution);
    mSupersampling = new QComboBox(w);
    for (int s : {1, 4, 16, 64})
        mSupersampling->addItem(s == 1 ? QString("Off") : QString("%1 (%2 %3 %2)").arg(s).arg(int(std::sqrt(s))).arg(QChar(0x00D7)), s);
    mSupersampling->setToolTip("Supersampling: samples per texel");
    setComboDefault(mSupersampling, mSupersampling->findData(defaults.supersampling));
    connect(mSupersampling, &QComboBox::currentIndexChanged, this, [this, set](int) { set("supersampling", mSupersampling->currentData().toInt()); });
    form->addRow("Supersampling", mSupersampling);
    mPadding = new QSpinBox(w);
    bakerNumberStyle(mPadding);
    mPadding->setRange(-1, 1024);
    mPadding->setSuffix(" px");
    mPadding->setSpecialValueText("Fill");
    mPadding->setKeyboardTracking(false);
    mPadding->setToolTip("Padding: texels of dilation past island borders; Fill floods the whole texture");
    setSpinDefault(mPadding, defaults.padding);
    connect(mPadding, &QSpinBox::valueChanged, this, [set](int x) { set("padding", x); });
    form->addRow("Padding", mPadding);

    auto pipeline = [this](const char* flag) {
        return [this, flag](bool on) {
            if (!mUpdating)
                run("setBakeProject", {{flag, on}});
        };
    };
    mAutoAssign = new QCheckBox("Assign after baking", w);
    mAutoAssign->setObjectName("bakerAutoAssign");
    mAutoAssign->setToolTip("Assign after baking: every bake wires its textures into the materials of the lows it was baked for (bakeAssign)");
    connect(mAutoAssign, &QCheckBox::toggled, this, pipeline("autoAssign"));
    setCheckDefault(mAutoAssign, bake::ProjectSettings{}.autoAssign);
    form->addRow(bakerSubHeader(w, "After Baking"));
    form->addRow(QString(), mAutoAssign);
    mRebakeOnSave = new QCheckBox("Rebake on save", w);
    mRebakeOnSave->setObjectName("bakerRebakeOnSave");
    mRebakeOnSave->setToolTip("Saving the scene rebakes this project when its meshes or settings changed since the last bake");
    connect(mRebakeOnSave, &QCheckBox::toggled, this, pipeline("rebakeOnSave"));
    setCheckDefault(mRebakeOnSave, false);
    form->addRow(QString(), mRebakeOnSave);
    return w;
}

QWidget* BakerPanel::buildResults()
{
    auto* w = new QWidget(this);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(2, 2, 2, 2);
    v->setSpacing(2);
    auto* split = new QSplitter(Qt::Vertical, w);
    split->setChildrenCollapsible(false);
    mSummaryBox = new QWidget(split);
    auto* row = new QHBoxLayout(mSummaryBox);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(2);
    mSummary = new QTreeWidget(mSummaryBox);
    mSummary->setObjectName("bakerSummary");
    mSummary->setHeaderHidden(true);
    mSummary->setColumnCount(2);
    mSummary->setUniformRowHeights(true);
    mSummary->header()->setStretchLastSection(true);
    mSummary->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    connect(mSummary, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int) {
        if (const QString file = it->data(0, kFileRole).toString(); !file.isEmpty())
            openFolder(file);
    });
    row->addWidget(mSummary, 1);
    auto* buttons = new QVBoxLayout;
    buttons->setSpacing(1);
    mAssign = bakerPush(mSummaryBox, "Assign", "Assign: wire the maps into the lows' materials");
    mAssign->setObjectName("bakerAssign");
    connect(mAssign, &QToolButton::clicked, this, [this] { assignResult(); });
    buttons->addWidget(mAssign);
    row->addLayout(buttons);
    auto* openOutput = bakerPush(mSummaryBox, "Open Folder", "Open Folder: show the output folder");
    openOutput->setObjectName("bakerOpenFolder");
    connect(openOutput, &QToolButton::clicked, this, [this] { openFolder(outputFolder()); });
    buttons->addWidget(openOutput);
    auto* clear = bakerPush(mSummaryBox, "Clear", "Clear: the bake messages");
    clear->setObjectName("bakerClearSummary");
    connect(clear, &QToolButton::clicked, this, [this] {
        mResultOcclusion = false;
        mResultBaked = false;
        mCacheDropped = 0.0;
        syncLightingNote();
        syncPreviewNote();
        syncCacheNote();
        mSummary->clear();
        mSummaryBox->hide();
    });
    buttons->addWidget(clear);
    mEnvironmentLighting = bakerPush(mSummaryBox, "Environment Lighting",
                                     "Environment Lighting: light the viewport with the HDRI, where baked occlusion shows. Sculpt lighting all but hides it");
    mEnvironmentLighting->setObjectName("bakerEnvironmentLighting");
    mEnvironmentLighting->hide();
    connect(mEnvironmentLighting, &QToolButton::clicked, this, [] { actions::run(json("viewport.lighting.environment")); });
    buttons->addWidget(mEnvironmentLighting);
    mShowExportMesh = bakerPush(mSummaryBox, "Show Export Mesh",
                                "Show Export Mesh: turn on, in the viewport, the modifiers these meshes export with, so the maps preview on the mesh they were baked for");
    mShowExportMesh->setObjectName("bakerShowExportMesh");
    mShowExportMesh->hide();
    connect(mShowExportMesh, &QToolButton::clicked, this, [this] { showExportMesh(); });
    buttons->addWidget(mShowExportMesh);
    mRaiseCache = bakerPush(mSummaryBox, "Raise Cache", "Raise Cache: let the snapshot cache hold every mesh of this bake, so re-bakes reuse them instead of preparing them again");
    mRaiseCache->setObjectName("bakerRaiseCache");
    mRaiseCache->hide();
    connect(mRaiseCache, &QToolButton::clicked, this, [this] { raiseSnapshotCache(); });
    buttons->addWidget(mRaiseCache);
    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file == "general")
            syncLightingNote();
    });
    buttons->addStretch(1);
    // One column width, so the buttons line up.
    for (QToolButton* b : {mAssign, openOutput, clear, mEnvironmentLighting, mShowExportMesh, mRaiseCache})
        b->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    mThumbs = new ThumbList(split);
    mThumbs->setObjectName("bakerThumbs");
    mThumbs->setViewMode(QListView::IconMode);
    mThumbs->setIconSize(QSize(120, 120));
    mThumbs->setGridSize(QSize(136, 156));
    mThumbs->setResizeMode(QListView::Adjust);
    mThumbs->setMovement(QListView::Static);
    mThumbs->setWordWrap(true);
    mThumbs->setToolTip("Double-click to open a map; right-click to show it in its folder or copy its path");
    mThumbs->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mThumbs->setContextMenuPolicy(Qt::CustomContextMenu);
    // Baked files are left on disk: removing them can't be undone.
    deletion::claim(mThumbs, nullptr);
    connect(mThumbs, &QListWidget::itemDoubleClicked, this,
            [](QListWidgetItem* it) { QDesktopServices::openUrl(QUrl::fromLocalFile(it->data(kFileRole).toString())); });
    connect(mThumbs, &QListWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        QListWidgetItem* hit = mThumbs->itemAt(at);
        if (!hit)
            return;
        if (!hit->isSelected())
            mThumbs->setCurrentItem(hit);
        QMenu* m = thumbnailMenu(this);
        m->setAttribute(Qt::WA_DeleteOnClose);
        m->popup(mThumbs->viewport()->mapToGlobal(at));
    });
    split->addWidget(mSummaryBox);
    split->addWidget(mThumbs);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({60, 400});
    v->addWidget(split, 1);
    return w;
}
} // namespace lila::ui
