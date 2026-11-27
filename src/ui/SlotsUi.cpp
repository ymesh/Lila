#include "ui/SlotsUi.h"

#include "core/Events.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "slots/Slots.h"
#include "ui/Actions.h"
#include "ui/AttributeEditor.h"
#include "ui/Controls.h"
#include "ui/DrawOverrides.h"
#include "ui/Hotkeys.h"
#include "ui/LevelToolkit.h"
#include "ui/Menus.h"
#include "ui/MeshCache.h"
#include "ui/Panels.h"
#include "ui/Theme.h"
#include "ui/UiCommands.h"
#include "ui/UiCommon.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QRectF>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

namespace lila::ui
{
namespace
{
constexpr const char* kSlotsDock = "slots";
constexpr const char* kPrefs = "slotsPanel";
constexpr int kSlotIdRole = Qt::UserRole;

QString qs(const std::string& s) { return QString::fromStdString(s); }
std::string trimmedStd(const QString& s) { return s.trimmed().toStdString(); }

json listOf(const QString& text)
{
    json out = json::array();
    for (const QString& part : text.split(',', Qt::SkipEmptyParts))
        if (!part.trimmed().isEmpty())
            out.push_back(trimmedStd(part));
    return out;
}

/** What a rule shows, as the form's Show choice. */
enum Show
{
    kKeep,
    kItem,
    kPoolTag,
    kMesh,
    kPlaceholder,
    kHidden,
};

QTreeWidget* makeTree(QWidget* parent, const QStringList& headers, const QString& empty)
{
    auto* t = new QTreeWidget(parent);
    t->setColumnCount(int(headers.size()));
    t->setHeaderLabels(headers);
    t->setRootIsDecorated(false);
    t->setIndentation(0);
    t->setUniformRowHeights(true);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->header()->setStretchLastSection(false);
    t->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < headers.size(); ++c)
        t->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    Hotkeys::passThrough(t);
    setEmptyHint(t, empty);
    return t;
}

class SlotsPanel : public QWidget
{
public:
    explicit SlotsPanel(QWidget* parent) : QWidget(parent)
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(4, 4, 4, 4);
        v->setSpacing(8);

        auto* top = new QHBoxLayout;
        top->setSpacing(4);
        mSummary = new QLabel(this);
        mSummary->setForegroundRole(QPalette::PlaceholderText);
        top->addWidget(mSummary, 1);
        auto* check = new QToolButton(this);
        check->setText("Check");
        check->setToolTip("Check Slots\nList orphaned overrides, pin conflicts and duplicate ids in the Validation panel");
        controls::setRole(check, "push");
        connect(check, &QToolButton::clicked, this, [] { runUi("slotReport", {{"validate", true}}); });
        top->addWidget(check);
        v->addLayout(top);
        mBanner = controls::warningBanner(this);
        v->addWidget(mBanner);

        mTypes = makeTree(this, {"Type", "Slots"}, "No slots. Set Dressing arrangements, Settlement furniture and Terrain ground cover publish them.");
        mTypes->setMinimumHeight(130);
        connect(mTypes, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* it) { mFindType->setText(it->data(0, kSlotIdRole).toString()); });
        v->addWidget(controls::section("Types", mTypes, this, kPrefs));

        v->addWidget(controls::section("Rules", buildRules(), this, kPrefs));
        v->addWidget(controls::section("Find", buildFind(), this, kPrefs));
        v->addStretch(1);

        auto* timer = new QTimer(this);
        timer->setInterval(400);
        connect(timer, &QTimer::timeout, this, [this] {
            scene::Graph& g = scene::Graph::current();
            const auto key = std::make_pair(slots::revision(), g.version());
            if (!isVisible() || key == mSeen)
                return;
            mSeen = key;
            refresh();
        });
        timer->start();
    }

private:
    QWidget* buildRules()
    {
        auto* box = new QWidget(this);
        auto* v = new QVBoxLayout(box);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(4);

        auto* bar = new QHBoxLayout;
        bar->setSpacing(2);
        QToolButton* add = toolButton(box, "plus", "Add Rule\nA rule for the type picked in Types (every type when none is)");
        connect(add, &QToolButton::clicked, this, [this] {
            QString type = mTypes->currentItem() ? mTypes->currentItem()->data(0, kSlotIdRole).toString() : QString("*");
            const json r = runUi("slotRule", {{"args", {"add"}}, {"type", trimmedStd(type)}, {"name", trimmedStd(type) + " swap"}});
            if (r.is_object())
                mSelectRule = r.value("id", std::string());
            mSeen = {};
        });
        QToolButton* up = toolButton(box, "arrow-up", "Move Up\nRules apply in list order within their level");
        QToolButton* down = toolButton(box, "arrow-down", "Move Down");
        auto move = [this](int by) {
            QTreeWidgetItem* it = mRules->currentItem();
            if (!it)
                return;
            const int row = mRules->indexOfTopLevelItem(it) + by;
            if (row < 0 || row >= mRules->topLevelItemCount())
                return;
            mSelectRule = trimmedStd(it->data(0, kSlotIdRole).toString());
            runUi("slotRule", {{"args", {"move"}}, {"id", mSelectRule}, {"index", row}});
        };
        connect(up, &QToolButton::clicked, this, [move] { move(-1); });
        connect(down, &QToolButton::clicked, this, [move] { move(1); });
        QToolButton* trash = toolButton(box, "trash", "Remove Rule");
        connect(trash, &QToolButton::clicked, this, [this] {
            if (QTreeWidgetItem* it = mRules->currentItem())
                runUi("slotRule", {{"args", {"remove"}}, {"id", trimmedStd(it->data(0, kSlotIdRole).toString())}});
        });
        bar->addWidget(add);
        bar->addStretch(1);
        bar->addWidget(up);
        bar->addWidget(down);
        bar->addWidget(trash);
        v->addLayout(bar);

        mRules = makeTree(box, {"Rule", "Level", "Slots"}, "No rules. Click + to swap a type's payload everywhere.");
        mRules->setMinimumHeight(90);
        connect(mRules, &QTreeWidget::currentItemChanged, this, [this] { loadForm(); });
        connect(mRules, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it, int col) {
            if (mFilling || col != 0)
                return;
            runUi("slotRule", {{"args", {"edit"}}, {"id", trimmedStd(it->data(0, kSlotIdRole).toString())}, {"enabled", it->checkState(0) == Qt::Checked}});
        });
        v->addWidget(mRules);

        mForm = new QWidget(box);
        auto* f = new QFormLayout(mForm);
        f->setContentsMargins(0, 4, 0, 0);
        f->setSpacing(4);
        f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        mName = new QLineEdit(mForm);
        f->addRow("Name", mName);
        mLevel = new controls::Segmented(mForm);
        mLevel->add("Global", {}, "Every slot of the type", 0);
        mLevel->add("Region", {}, "Slots in a region (a settlement, district)", 1);
        mLevel->add("Owner", {}, "Slots of one owner (a building, lot, street, arrangement)", 2);
        connect(mLevel, &controls::Segmented::picked, this, [this](int) { syncScope(); });
        f->addRow("Level", mLevel);
        mScope = new QLineEdit(mForm);
        mScopeLabel = new QLabel("Scope", mForm);
        f->addRow(mScopeLabel, mScope);
        mType = new QComboBox(mForm);
        mType->setEditable(true);
        f->addRow("Type", mType);
        mTags = new QLineEdit(mForm);
        mTags->setPlaceholderText("any");
        f->addRow("Tags", mTags);
        mShow = new QComboBox(mForm);
        addChoices(mShow, {{"Default", "keep"}, {"Library item", "item"}, {"Pool tag", "poolTag"}, {"Scene mesh", "mesh"}, {"Placeholder", "placeholder"}, {"Hidden", "hidden"}});
        mShowValue = new QLineEdit(mForm);
        auto* showRow = new QWidget(mForm);
        auto* sr = new QHBoxLayout(showRow);
        sr->setContentsMargins(0, 0, 0, 0);
        sr->setSpacing(4);
        sr->addWidget(mShow);
        sr->addWidget(mShowValue, 1);
        connect(mShow, &QComboBox::currentIndexChanged, this, [this] { mShowValue->setEnabled(mShow->currentIndex() != kKeep && mShow->currentIndex() != kHidden); });
        f->addRow("Show", showRow);
        mUeMesh = new QLineEdit(mForm);
        mUeMesh->setPlaceholderText("/Game/...");
        f->addRow("UE Mesh", mUeMesh);
        mUeClass = new QLineEdit(mForm);
        mUeClass->setPlaceholderText("/Game/...BP_Door_C");
        f->addRow("UE Class", mUeClass);
        mData = new QLineEdit(mForm);
        mData->setPlaceholderText("{\"locked\": true}");
        f->addRow("Data", mData);
        auto* apply = new QToolButton(mForm);
        apply->setText("Apply");
        controls::setRole(apply, "push");
        connect(apply, &QToolButton::clicked, this, [this] { applyForm(); });
        f->addRow("", apply);
        mForm->setEnabled(false);
        v->addWidget(mForm);
        return box;
    }

    QWidget* buildFind()
    {
        auto* box = new QWidget(this);
        auto* f = new QFormLayout(box);
        f->setContentsMargins(0, 0, 0, 0);
        f->setSpacing(4);
        f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        mFindType = new QLineEdit(box);
        mFindType->setPlaceholderText("any");
        mFindRegion = new QLineEdit(box);
        mFindRegion->setPlaceholderText("any");
        mFindTag = new QLineEdit(box);
        mFindTag->setPlaceholderText("any");
        f->addRow("Type", mFindType);
        f->addRow("Region", mFindRegion);
        f->addRow("Tag", mFindTag);
        auto* row = new QWidget(box);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto* select = new QToolButton(row);
        select->setText("Select");
        select->setToolTip("Select the nodes publishing the matching slots");
        controls::setRole(select, "push");
        mFound = new QLabel(row);
        mFound->setForegroundRole(QPalette::PlaceholderText);
        h->addWidget(select);
        h->addWidget(mFound, 1);
        connect(select, &QToolButton::clicked, this, [this] {
            json call = {{"select", true}, {"limit", 0}};
            if (!mFindType->text().trimmed().isEmpty())
                call["type"] = trimmedStd(mFindType->text());
            if (!mFindRegion->text().trimmed().isEmpty())
                call["region"] = trimmedStd(mFindRegion->text());
            if (!mFindTag->text().trimmed().isEmpty())
                call["tag"] = listOf(mFindTag->text());
            const json r = runUi("slotFind", call);
            if (r.is_object())
                mFound->setText(QString("%1 slots, %2 nodes").arg(r.value("count", 0)).arg(r.value("selected", json::array()).size()));
        });
        f->addRow("", row);
        return box;
    }

    void syncScope()
    {
        const int level = mLevel->current();
        mScopeLabel->setText(level == 2 ? "Owner" : "Region");
        mScope->setEnabled(level != 0);
        mScope->setPlaceholderText(level == 2 ? "lot/1a2b" : level == 1 ? "district:3" : "");
    }

    json currentRule() const
    {
        QTreeWidgetItem* it = mRules->currentItem();
        if (!it)
            return nullptr;
        const std::string id = trimmedStd(it->data(0, kSlotIdRole).toString());
        for (const json& r : mRuleJson)
            if (r.value("id", "") == id)
                return r;
        return nullptr;
    }

    void loadForm()
    {
        const json r = currentRule();
        mForm->setEnabled(r.is_object());
        if (!r.is_object())
            return;
        const json p = r.value("payload", json::object());
        mName->setText(qs(r.value("name", "")));
        const std::string level = r.value("level", "global");
        mLevel->setCurrent(level == "owner" ? 2 : level == "region" ? 1 : 0);
        mScope->setText(qs(level == "owner" ? r.value("owner", "") : r.value("region", "")));
        syncScope();
        mType->setCurrentText(qs(r.value("type", "*")));
        QStringList tags;
        for (const json& t : r.value("tags", json::array()))
            tags << qs(t.get<std::string>());
        mTags->setText(tags.join(", "));
        int show = kKeep;
        std::string value;
        if (p.value("hidden", false))
            show = kHidden;
        else if (p.contains("item"))
            show = kItem, value = p["item"];
        else if (p.contains("poolTag"))
            show = kPoolTag, value = p["poolTag"];
        else if (p.contains("mesh"))
            show = kMesh, value = p["mesh"];
        else if (p.contains("placeholder"))
            show = kPlaceholder, value = p["placeholder"];
        mShow->setCurrentIndex(show);
        mShowValue->setText(qs(value));
        mShowValue->setEnabled(show != kKeep && show != kHidden);
        mUeMesh->setText(qs(p.value("unrealMesh", "")));
        mUeClass->setText(qs(p.value("unrealClass", "")));
        mData->setText(p.contains("data") ? qs(p["data"].dump()) : QString());
    }

    void applyForm()
    {
        const json r = currentRule();
        if (!r.is_object())
            return;
        const int level = mLevel->current();
        json call = {{"args", {"edit"}}, {"id", r["id"]}, {"replacePayload", true}, {"name", trimmedStd(mName->text())},
                     {"level", level == 2 ? "owner" : level == 1 ? "region" : "global"}, {"type", trimmedStd(mType->currentText())},
                     {"tag", listOf(mTags->text())}};
        if (level == 1)
            call["region"] = trimmedStd(mScope->text());
        if (level == 2)
            call["owner"] = trimmedStd(mScope->text());
        const std::string value = trimmedStd(mShowValue->text());
        switch (mShow->currentIndex())
        {
        case kItem: call["item"] = value; break;
        case kPoolTag: call["poolTag"] = value; break;
        case kMesh: call["mesh"] = value; break;
        case kPlaceholder: call["placeholder"] = value; break;
        case kHidden: call["hidden"] = true; break;
        default: break;
        }
        if (!mUeMesh->text().trimmed().isEmpty())
            call["unrealMesh"] = trimmedStd(mUeMesh->text());
        if (!mUeClass->text().trimmed().isEmpty())
            call["unrealClass"] = trimmedStd(mUeClass->text());
        if (!mData->text().trimmed().isEmpty())
        {
            const json d = json::parse(trimmedStd(mData->text()), nullptr, false);
            if (!d.is_object())
            {
                controls::setWarning(mBanner, "Data is a JSON object: {\"key\": value}");
                return;
            }
            call["data"] = d;
        }
        mSelectRule = r["id"].get<std::string>();
        runUi("slotRule", call);
    }

    void refresh()
    {
        const json rep = ops::execute("slotReport", {{"validate", false}}, CallSource::Script);
        mSummary->setText(QString("%1 slots, %2 rules, %3 overrides, %4 pins")
                              .arg(rep.value("total", 0))
                              .arg(rep["rules"].size())
                              .arg(rep.value("overrides", 0))
                              .arg(rep.value("pins", 0)));
        QStringList warn;
        if (!rep["orphans"].empty())
            warn << QString("%1 orphaned overrides").arg(rep["orphans"].size());
        if (!rep["conflicts"].empty())
            warn << QString("%1 pin conflicts").arg(rep["conflicts"].size());
        if (!rep["duplicates"].empty())
            warn << QString("%1 duplicate ids").arg(rep["duplicates"].size());
        if (!rep["missingVisuals"].empty())
            warn << QString("%1 payloads drawn as placeholders").arg(rep["missingVisuals"].size());
        controls::setWarning(mBanner, warn.join(", "));

        const QString pickedType = mTypes->currentItem() ? mTypes->currentItem()->data(0, kSlotIdRole).toString() : QString();
        mTypes->clear();
        for (auto it = rep["byType"].begin(); it != rep["byType"].end(); ++it)
        {
            auto* item = new QTreeWidgetItem(mTypes);
            const slots::SlotType* t = slots::resolveType(it.key());
            item->setText(0, qs(it.key()));
            item->setData(0, kSlotIdRole, qs(it.key()));
            item->setToolTip(0, t ? qs(t->label + ": " + t->doc) : "Unregistered type");
            item->setText(1, QString::number(it->get<int64_t>()));
            item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            if (qs(it.key()) == pickedType)
                mTypes->setCurrentItem(item);
        }

        mFilling = true;
        const std::string picked = !mSelectRule.empty() ? mSelectRule
                                   : mRules->currentItem()  ? trimmedStd(mRules->currentItem()->data(0, kSlotIdRole).toString())
                                                            : std::string();
        mSelectRule.clear();
        mRuleJson = ops::execute("slotRule", {{"args", {"list"}}}, CallSource::Script);
        std::map<std::string, int64_t> counts;
        for (const json& r : rep["rules"])
            counts[r["id"].get<std::string>()] = r.value("slots", 0);
        mRules->clear();
        QTreeWidgetItem* select = nullptr;
        for (const json& r : mRuleJson)
        {
            auto* item = new QTreeWidgetItem(mRules);
            const std::string id = r["id"];
            const std::string name = r.value("name", "");
            item->setText(0, qs(name.empty() ? r.value("type", "*") : name));
            item->setData(0, kSlotIdRole, qs(id));
            item->setCheckState(0, r.value("enabled", true) ? Qt::Checked : Qt::Unchecked);
            item->setToolTip(0, qs(r.value("type", "*") + ": " + slots::Payload::fromJson(r.value("payload", json())).describe()));
            const std::string level = r.value("level", "global");
            item->setText(1, level == "owner" ? "Owner" : level == "region" ? "Region" : "Global");
            item->setText(2, QString::number(counts[id]));
            item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
            if (id == picked)
                select = item;
        }
        if (select)
            mRules->setCurrentItem(select);
        mFilling = false;

        const QString typed = mType->currentText();
        mType->blockSignals(true);
        mType->clear();
        mType->addItem("*");
        for (const std::string& id : slots::types().ids())
            mType->addItem(qs(id));
        mType->setCurrentText(typed);
        mType->blockSignals(false);
        loadForm();
    }

    QLabel* mSummary = nullptr;
    QWidget* mBanner = nullptr;
    QTreeWidget* mTypes = nullptr;
    QTreeWidget* mRules = nullptr;
    QWidget* mForm = nullptr;
    QLineEdit* mName = nullptr;
    controls::Segmented* mLevel = nullptr;
    QLabel* mScopeLabel = nullptr;
    QLineEdit* mScope = nullptr;
    QComboBox* mType = nullptr;
    QLineEdit* mTags = nullptr;
    QComboBox* mShow = nullptr;
    QLineEdit* mShowValue = nullptr;
    QLineEdit* mUeMesh = nullptr;
    QLineEdit* mUeClass = nullptr;
    QLineEdit* mData = nullptr;
    QLineEdit* mFindType = nullptr;
    QLineEdit* mFindRegion = nullptr;
    QLineEdit* mFindTag = nullptr;
    QLabel* mFound = nullptr;
    json mRuleJson = json::array();
    std::string mSelectRule;
    bool mFilling = false;
    std::pair<uint64_t, uint64_t> mSeen{0, 0};
};

/** A slotSet's draw batches, kept while the graph and slot state are unchanged. */
struct CachedDraw
{
    uint64_t graphVersion = 0;
    uint64_t revision = 0;
    struct Batch
    {
        std::shared_ptr<const render::RenderMesh> mesh;
        std::shared_ptr<const std::vector<glm::mat4>> instances;
        std::shared_ptr<const std::vector<glm::vec4>> colors; // null when no slot of it carries a colour
    };
    std::vector<Batch> batches;
    std::vector<glm::dvec3> points;
};
std::unordered_map<scene::NodeId, CachedDraw>& drawCache()
{
    static std::unordered_map<scene::NodeId, CachedDraw> c;
    return c;
}

const CachedDraw& cachedDraw(scene::NodeId id)
{
    scene::Graph& g = scene::Graph::current();
    CachedDraw& c = drawCache()[id];
    if (c.graphVersion == g.version() && c.revision == slots::revision() && c.graphVersion != 0)
        return c;
    c.graphVersion = g.version();
    c.revision = slots::revision();
    c.batches.clear();
    c.points.clear();
    for (slots::DrawBatch& b : slots::drawBatches(g, id))
    {
        for (const glm::mat4& m : b.transforms)
            c.points.push_back(glm::dvec3(m[3]));
        c.batches.push_back({renderMeshFor(b.mesh), std::make_shared<const std::vector<glm::mat4>>(std::move(b.transforms)),
                             b.colors.empty() ? nullptr : std::make_shared<const std::vector<glm::vec4>>(std::move(b.colors))});
    }
    return c;
}

void drawSlotSet(drawOverrides::DrawContext& c)
{
    const int on = c.node.type().attrIndex("drawSlots");
    if (on >= 0 && !c.node.as<bool>(on))
        return;
    const CachedDraw& d = cachedDraw(c.node.id());
    const bool selected = c.highlight == drawOverrides::Highlight::Selected || c.highlight == drawOverrides::Highlight::UnderSelected;
    for (const CachedDraw::Batch& b : d.batches)
    {
        if (!b.mesh || b.instances->empty())
            continue;
        render::MeshDraw m;
        m.mesh = b.mesh;
        m.instances = b.instances;
        m.instanceColors = b.colors;
        m.baseColor = glm::vec3(0.55f);
        m.flags = render::DrawShaded | (c.highlight != drawOverrides::Highlight::None ? render::DrawWire : 0u);
        m.wireColor = selected || c.highlight == drawOverrides::Highlight::Preselected ? c.color : glm::vec4(0.0f);
        m.node = c.node.id();
        c.frame.meshes.push_back(std::move(m));
    }
}

std::optional<double> pickSlotSet(const drawOverrides::PickContext& c)
{
    const CachedDraw& d = cachedDraw(c.node.id());
    const QRectF box = QRectF(c.a, c.b).normalized();
    std::optional<double> best;
    for (const glm::dvec3& p : d.points)
    {
        std::optional<QPointF> at = c.view.project(p);
        if (!at)
            continue;
        if (!c.click)
        {
            if (box.contains(*at))
                return 0.0;
            continue;
        }
        // A slot is a solid part, not a line: a click a few lines' widths from its base still takes it.
        const double dist = std::hypot(at->x() - c.a.x(), at->y() - c.a.y()) / 3.0;
        if (dist <= drawOverrides::kClickPixels && (!best || dist < *best))
            best = dist;
    }
    return best;
}

QWidget* slotSetSection(scene::NodeId id)
{
    scene::Graph& g = scene::Graph::current();
    const scene::Node* n = g.node(id);
    if (!n)
        return nullptr;
    const std::vector<slots::Slot> slotsHere = slots::readSet(g, id);
    std::map<std::string, int> byType;
    std::set<std::string> owners;
    size_t overridden = 0, pinned = 0;
    slots::StatePtr st = slots::state();
    for (const slots::Slot& s : slotsHere)
    {
        ++byType[s.type];
        owners.insert(s.owner);
        overridden += st->overrides.contains(s.id) ? 1 : 0;
        pinned += slots::isPinned(s.id) ? 1 : 0;
    }
    auto* w = new QWidget;
    auto* f = new QFormLayout(w);
    f->setContentsMargins(0, 0, 0, 0);
    f->setSpacing(4);
    f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    f->addRow("Slots", new QLabel(QString::number(slotsHere.size()), w));
    QStringList types;
    for (const auto& [t, count] : byType)
        types << QString("%1 %2").arg(qs(t)).arg(count);
    auto* typeLabel = new QLabel(types.join(", "), w);
    typeLabel->setWordWrap(true);
    f->addRow("Types", typeLabel);
    f->addRow("Overrides", new QLabel(QString::number(overridden), w));
    f->addRow("Pinned", new QLabel(QString::number(pinned), w));
    auto* row = new QWidget(w);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(4);
    const std::vector<std::string> ownerList(owners.begin(), owners.end());
    auto button = [&](const QString& text, const QString& tip, std::function<void()> fn) {
        auto* b = new QToolButton(row);
        b->setText(text);
        b->setToolTip(tip);
        controls::setRole(b, "push");
        QObject::connect(b, &QToolButton::clicked, b, fn);
        h->addWidget(b);
    };
    button("Pin", "Pin every slot here where it is, with what it shows", [ownerList] {
        json keys = json::array();
        for (const std::string& o : ownerList)
            keys.push_back("owner:" + o);
        runUi("slotPin", {{"args", keys}});
    });
    button("Unpin", "Unpin every slot here", [ownerList] {
        for (const std::string& o : ownerList)
            if (slots::isPinned("owner:" + o))
                runUi("slotPin", {{"args", {"owner:" + o}}, {"remove", true}});
    });
    button("Slots Panel", "Open the Slots panel", [] {
        if (const Action* a = actions::find("window.slots"); a && a->run)
            a->run();
    });
    h->addStretch(1);
    f->addRow("", row);
    return w;
}
} // namespace

void registerSlotsUi()
{
    static bool done = false;
    if (std::exchange(done, true))
        return;
    panels::add({.id = kSlotsDock,
                 .title = "Slots",
                 .create = [](QWidget* p) -> QWidget* { return new SlotsPanel(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "layout-grid-add",
                 .open = false,
                 .toggleAction = "window.slots",
                 .keywords = {"slots", "swap", "override", "variants", "doors", "signs", "props", "rules", "replace"},
                 .docs = "slots"});
    actions::add({.id = "slots.report",
                  .label = "Check Slots",
                  .category = "Level",
                  .doc = "List orphaned slot overrides, pin conflicts, duplicate ids and payloads drawn as placeholders in the Validation panel.",
                  .python = "slotReport()",
                  .command = "slotReport",
                  .call = json::object(),
                  .icon = "list-check",
                  .keywords = {"slots", "orphans", "validate"}});
    actions::add({.id = "slots.pinSelected",
                  .label = "Pin Slots",
                  .category = "Level",
                  .run =
                      [] {
                          scene::Graph& g = scene::Graph::current();
                          json keys = json::array();
                          std::set<std::string> owners;
                          for (scene::NodeId id : g.selection())
                              for (const slots::Slot& s : slots::readSet(g, id))
                                  owners.insert(s.owner);
                          for (const std::string& o : owners)
                              keys.push_back("owner:" + o);
                          if (!keys.empty())
                              runUi("slotPin", {{"args", keys}});
                      },
                  .doc = "Pin every slot of the selected slot sets where it is: regenerating leaves them as they are.",
                  .icon = "pin",
                  .keywords = {"slots", "freeze", "lock"}});
    menus::addItem({.id = "slots.menu", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Slots", .before = levelToolkit::kAssetsSection});
    menus::addActions("Level/Slots", {"window.slots", "slots.report", "slots.pinSelected"});
    levelToolkit::add({.id = "slots",
                       .title = "Slots",
                       .items = {{"window.slots", "layout-grid-add", "Slots", "Slots"}, {"slots.report", "list-check", "Check"}, {"slots.pinSelected", "pin", "Pin"}},
                       .order = 45,
                       .docs = "slots"});
    attributeEditor::addSection(slots::kSetType, "Slots", slotSetSection);
    drawOverrides::add({.id = "lila.slotSet", .nodeType = slots::kSetType, .draw = drawSlotSet, .pick = pickSlotSet});
    events::subscribe(events::Event::NodeDeleted, [](const events::EventData& d) { drawCache().erase(scene::NodeId(d.node)); });
}
} // namespace lila::ui
