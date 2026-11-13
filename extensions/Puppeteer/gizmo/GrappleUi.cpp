// Grapple: the tool that makes contacts by dragging from one character's body onto another's, its palette (kind, lead,
// blend, the grapple's contacts and characters, bake and ragdoll), how contacts draw and pick, and the actions and menus.

#include "Puppeteer.h"
#include "rig/Rig.h"

#include <lila/Anim.h>
#include <lila/Controls.h>
#include <lila/Events.h>
#include <lila/Grapple.h>
#include <lila/Icons.h>
#include <lila/Prefs.h>
#include <lila/Simulation.h>
#include <lila/UI.h>
#include <lila/Viewport.h>
#include <lila/ViewportTool.h>

#include <QApplication>
#include <QCheckBox>
#include <QDockWidget>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace puppeteer
{
namespace grappleui
{
namespace gp = lila::scene::grapple;

constexpr const char* kToolId = "puppeteer.grapple";
constexpr const char* kPaletteId = "puppeteer.grapplePalette";
constexpr const char* kSections = "puppeteerGrapplePalette";

/** cm behind the nearest hit a capsule may be and still compete for the cursor. */
constexpr double kFrontDepth = 4.0;

const glm::vec4 kBody{0.92f, 0.78f, 0.22f, 0.22f};
const glm::vec4 kBodyEdge{0.92f, 0.78f, 0.22f, 0.9f};
const glm::vec4 kLink{0.92f, 0.78f, 0.22f, 1.0f};
const glm::vec4 kLinkOff{0.6f, 0.6f, 0.6f, 0.6f};
const glm::vec4 kSelected{1.0f, 1.0f, 1.0f, 1.0f};

/** Contact kinds in segment order, as grappleContact takes them. */
constexpr std::array<const char*, 4> kKinds = {"grab", "hold", "push", "lift"};
/** Who yields, in segment order; "" lets the kind decide. A contact's lead attr counts from auto, so segment i is enum value i. */
constexpr std::array<const char*, 4> kLeads = {"", "source", "target", "even"};

struct Settings
{
    int kind = 0;
    int lead = 0;
    double blendFrames = 6.0;
    bool autoBake = true;
};

Settings& settings()
{
    static Settings s = [] {
        const PrefStore& p = PrefStore::get();
        Settings c;
        c.kind = std::clamp(p.value<int>(kPrefs, "/grapple/kind", 0), 0, int(kKinds.size()) - 1);
        c.lead = std::clamp(p.value<int>(kPrefs, "/grapple/lead", 0), 0, int(kLeads.size()) - 1);
        c.blendFrames = std::max(0.0, p.value<double>(kPrefs, "/grapple/blendFrames", 6.0));
        c.autoBake = p.value<bool>(kPrefs, "/grapple/autoBake", true);
        return c;
    }();
    return s;
}

void saveSettings()
{
    PrefStore& p = PrefStore::get();
    const Settings& s = settings();
    p.set(kPrefs, "/grapple/kind", s.kind);
    p.set(kPrefs, "/grapple/lead", s.lead);
    p.set(kPrefs, "/grapple/blendFrames", s.blendFrames);
    p.set(kPrefs, "/grapple/autoBake", s.autoBake);
}

bool isGrappleNode(const Node& n) { return n.type().isA(gp::kType); }
bool isContactNode(const Node& n) { return n.type().isA(gp::kContactType); }

std::vector<NodeId> grapples(Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
        if (isGrappleNode(*g.node(id)))
            out.push_back(id);
    return out;
}

/** The grapple the UI works on: the selection's, else the scene's first. */
NodeId currentGrapple(Graph& g)
{
    for (NodeId id : g.selection())
        if (const NodeId gr = gp::grappleOf(g, id))
            return gr;
    const auto all = grapples(g);
    return all.empty() ? kNoNode : all.front();
}

/** Selected contacts, else every contact of the current grapple. */
std::vector<std::string> targetContacts(Graph& g)
{
    std::vector<std::string> out;
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && isContactNode(*n))
            out.push_back(n->path());
    if (out.empty())
        if (const NodeId gr = currentGrapple(g))
            for (NodeId c : gp::contactsOf(g, gr))
                out.push_back(g.path(c));
    return out;
}

double blendSeconds() { return settings().blendFrames / sim::framesPerSecond(); }

void bake(NodeId grapple)
{
    Graph& g = Graph::current();
    if (grapple && g.node(grapple))
        runCommand("bakeGrapple", {{"args", {g.path(grapple)}}});
}

void keyContacts(bool on)
{
    Graph& g = Graph::current();
    const std::vector<std::string> contacts = targetContacts(g);
    if (contacts.empty())
        return log::write(LogLevel::Warning, "Grapple: no contacts; drag from one character onto another with the Grapple tool");
    const NodeId grapple = gp::grappleOf(g, g.find(contacts.front())->id());
    runCommand("grappleKey", {{"args", contacts}, {"on", on}, {"blend", blendSeconds()}});
    if (settings().autoBake)
        bake(grapple);
}

/** A body part under the cursor: the joint whose capsule the ray meets first, and where. */
struct Hit
{
    NodeId root = kNoNode;
    NodeId joint = kNoNode;
    glm::dvec3 point{0.0};
    gp::PosedCapsule capsule;
};

/** Every character's bodies as the tool picks them: grapples' fitted ones, else fitted on the spot. */
class Bodies
{
public:
    void refresh()
    {
        Graph& g = Graph::current();
        mFigures.clear();
        std::vector<NodeId> roots;
        for (NodeId gr : grapples(g))
            for (gp::FigureSetup& f : gp::figuresOf(g, gr))
                if (std::none_of(mFigures.begin(), mFigures.end(), [&](const gp::FigureSetup& x) { return x.root == f.root; }))
                    mFigures.push_back(std::move(f));
        // Every skeleton is grabbable, a character or not: each top joint with a body below it.
        for (NodeId id : g.nodes())
        {
            Node* n = g.node(id);
            if (!n || !lila::scene::rig::isJoint(*n) || (g.parent(id) && lila::scene::rig::isJoint(*g.node(g.parent(id)))))
                continue;
            if (g.descendants(id).size() < 4)
                continue;
            if (std::none_of(mFigures.begin(), mFigures.end(), [&](const gp::FigureSetup& x) { return x.root == id; }))
                mFigures.push_back(gp::fitFigure(g, id));
        }
    }

    std::optional<Hit> pick(QPointF px, const ViewInfo& view) const
    {
        Graph& g = Graph::current();
        glm::dvec3 o, d;
        view.ray(px, o, d);
        // Cast from the eye's end of the line, so the nearest hit is the one the user sees.
        if (glm::dot(d, o - view.eye) < 0.0)
        {
            o += d * glm::dot(view.eye - o, d);
            d = -d;
        }
        std::vector<std::pair<double, Hit>> hits;
        for (const gp::FigureSetup& f : mFigures)
            if (g.node(f.root))
                for (const gp::PosedCapsule& c : gp::posedCapsules(g, f))
                    if (const auto t = gp::rayCapsule(o, d, c.a, c.b, c.radius))
                        hits.push_back({*t, Hit{f.root, c.joint, o + glm::normalize(d) * *t, c}});
        if (hits.empty())
            return std::nullopt;
        const double nearest = std::min_element(hits.begin(), hits.end(), [](const auto& x, const auto& y) { return x.first < y.first; })->first;
        // Neighbouring capsules overlap at the joints: among the hits at the front, the one the cursor is most centred on.
        const glm::dvec3 far = o + glm::normalize(d) * (nearest + 1e4);
        auto centred = [&](const gp::PosedCapsule& c) {
            const auto [onAxis, onRay] = gp::closestPoints(c.a, c.b, o, far);
            return glm::length(onAxis - onRay) / std::max(c.radius, 1e-6);
        };
        const Hit* best = nullptr;
        double bestScore = 1e300;
        for (const auto& [t, h] : hits)
            if (t <= nearest + kFrontDepth)
                if (const double score = centred(h.capsule); score < bestScore)
                    bestScore = score, best = &h;
        return *best;
    }

private:
    std::vector<gp::FigureSetup> mFigures;
};

void drawCapsule(OverlayPainter& p, const ViewInfo& view, const gp::PosedCapsule& c, const glm::vec4& fill, const glm::vec4& edge)
{
    const auto a = view.project(c.a), b = view.project(c.b);
    if (!a || !b)
        return;
    // The capsule's width on screen, from how far its radius reaches at its middle.
    const glm::dvec3 mid = (c.a + c.b) * 0.5;
    const glm::dvec3 side = glm::normalize(glm::cross(view.viewDir(mid), glm::length(c.b - c.a) > 1e-6 ? c.b - c.a : glm::dvec3(0, 1, 0)));
    const auto m = view.project(mid), s = view.project(mid + side * c.radius);
    const double r = m && s ? std::hypot(s->x() - m->x(), s->y() - m->y()) : 6.0;
    p.smoothLine(*a, *b, float(r * 2.0), fill);
    p.smoothDisc(*a, r, fill);
    p.smoothDisc(*b, r, fill);
    p.line(*a, *b, 1.5f, edge);
}

std::string shortName(Graph& g, NodeId id) { return g.node(id) ? g.node(id)->name() : std::string(); }

class GrappleTool final : public ViewportTool
{
public:
    const char* name() const override { return "Grapple"; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

    void activate() override
    {
        mBodies.refresh();
        panels::show(kPaletteId);
        for (QWidget* w : QApplication::allWidgets())
            if (auto* d = qobject_cast<QDockWidget*>(w); d && d->objectName() == kPaletteId)
                panels::placeToolPalette(d);
    }

    void deactivate() override
    {
        for (QWidget* w : QApplication::allWidgets())
            if (auto* d = qobject_cast<QDockWidget*>(w); d && d->objectName() == kPaletteId)
                d->hide();
        viewportTools::requestRedraw();
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton || e->modifiers() & (Qt::AltModifier | Qt::ControlModifier))
            return false;
        mSource = mBodies.pick(e->position(), view);
        mTarget.reset();
        mCursor = e->position();
        if (!mSource)
            return false;
        mDragging = true;
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mCursor = e->position();
        const auto hit = mBodies.pick(e->position(), view);
        if (mDragging)
            mTarget = hit && valid(*hit) ? hit : std::nullopt;
        else
            mHover = hit;
        viewportTools::requestRedraw();
        return mDragging;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton || !mDragging)
            return false;
        mDragging = false;
        if (mSource && mTarget)
            make(*mSource, *mTarget);
        mSource.reset();
        mTarget.reset();
        viewportTools::requestRedraw();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (e->key() == Qt::Key_Escape)
        {
            if (mDragging)
            {
                mDragging = false;
                mSource.reset();
                mTarget.reset();
            }
            else
                viewportTools::set(nullptr);
            viewportTools::requestRedraw();
            return true;
        }
        return false;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        if (mDragging && mSource)
        {
            drawCapsule(p, view, mSource->capsule, kBody, kBodyEdge);
            if (mTarget)
                drawCapsule(p, view, mTarget->capsule, kBody, kBodyEdge);
            if (const auto a = view.project(mSource->point))
            {
                const QPointF end = mTarget ? view.project(mTarget->point).value_or(mCursor) : mCursor;
                p.smoothLine(*a, end, 2.0f, mTarget ? kLink : kLinkOff);
                p.smoothDisc(end, 4.0, mTarget ? kLink : kLinkOff);
            }
        }
        else if (mHover)
            drawCapsule(p, view, mHover->capsule, kBody, kBodyEdge);

        Graph& g = Graph::current();
        QString prompt = "drag from a body part onto another body";
        if (mDragging && mSource)
            prompt = mTarget ? QString::fromStdString(shortName(g, mSource->joint) + " to " + shortName(g, mTarget->joint)) : QString("release on the other body");
        else if (mHover)
            prompt = QString::fromStdString(shortName(g, mHover->joint));
        static const char* kNames[] = {"grab", "hold", "push", "lift"};
        ToolStatus s{.tool = "Grapple",
                     .mode = kNames[settings().kind],
                     .fields = {{"blend", QString("%1 f").arg(settings().blendFrames, 0, 'f', 0)}},
                     .flags = {{"auto bake", settings().autoBake}},
                     .prompt = prompt,
                     .keys = {{"Drag", "contact"}, {"Esc", "cancel, exit"}}};
        drawToolStatus(p, view, s);
    }

    /** Called when contacts or characters change, so picking sees new fits. */
    void refreshBodies() { mBodies.refresh(); }

private:
    bool valid(const Hit& h) const
    {
        if (!mSource || h.joint == mSource->joint)
            return false;
        Graph& g = Graph::current();
        // On one character, only parts that aren't neighbours (a hand on the hip, not on its own forearm).
        if (h.root == mSource->root && (g.parent(h.joint) == mSource->joint || g.parent(mSource->joint) == h.joint))
            return false;
        return true;
    }

    void make(const Hit& source, const Hit& target)
    {
        Graph& g = Graph::current();
        const Settings& s = settings();
        json call = {{"args", {g.path(source.joint), g.path(target.joint)}},
                     {"point", vecJson(target.point)},
                     {"kind", kKinds[size_t(s.kind)]},
                     {"blend", blendSeconds()}};
        if (s.lead > 0)
            call["lead"] = kLeads[size_t(s.lead)];
        const json made = runCommand("grappleContact", call);
        if (!made.is_object())
            return;
        if (Node* c = named(g, made.value("contact", std::string())))
            g.select({c->id()});
        mBodies.refresh();
        if (s.autoBake)
            if (Node* gr = named(g, made.value("grapple", std::string())))
                bake(gr->id());
    }

    Bodies mBodies;
    std::optional<Hit> mSource, mTarget, mHover;
    QPointF mCursor;
    bool mDragging = false;
};

// Palette -----------------------------------------------------------------------------------------------------------

class Palette final : public QWidget
{
public:
    explicit Palette(QWidget* parent) : QWidget(parent)
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 8, 12, 8);
        v->setSpacing(4);

        auto* contactBox = new QWidget(this);
        auto* form = new QGridLayout(contactBox);
        form->setContentsMargins(0, 0, 0, 0);
        form->setVerticalSpacing(4);
        form->setHorizontalSpacing(8);
        auto label = [&](const char* text, int row) {
            auto* l = new QLabel(text, contactBox);
            l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            form->addWidget(l, row, 0);
        };
        mKind = new controls::Segmented(contactBox);
        mKind->add("Grab", {}, "Grab: hold position and rotation (a wrist, a collar)", 0);
        mKind->add("Hold", {}, "Hold: position only, the part turns freely (an arm round a neck)", 1);
        mKind->add("Push", {}, "Push: the source leads, the other body is moved by it", 2);
        mKind->add("Lift", {}, "Lift: the source leads and the other body's feet leave the ground (lifts, carries)", 3);
        connect(mKind, &controls::Segmented::picked, this, [](int id) {
            settings().kind = id;
            saveSettings();
            viewportTools::requestRedraw();
        });
        label("Kind", 0);
        form->addWidget(mKind, 0, 1);
        mLead = new controls::Segmented(contactBox);
        mLead->add("Auto", {}, "Lead: by mass, the heavier character moving less; push and lift let the source lead", 0);
        mLead->add("Source", {}, "Lead: the source brings the other body to it", 1);
        mLead->add("Target", {}, "Lead: the source reaches all the way", 2);
        mLead->add("Even", {}, "Lead: both close half the gap", 3);
        connect(mLead, &controls::Segmented::picked, this, [](int id) {
            settings().lead = id;
            saveSettings();
            applyLeadToSelection(id);
        });
        label("Lead", 1);
        form->addWidget(mLead, 1, 1);
        mBlend = controls::valueField(0.0, 240.0, 0, "f", contactBox);
        mBlend->setToolTip("Blend: frames a contact takes to come on or go off");
        connect(mBlend, &QDoubleSpinBox::valueChanged, this, [](double f) {
            settings().blendFrames = f;
            saveSettings();
            viewportTools::requestRedraw();
        });
        label("Blend", 2);
        form->addWidget(mBlend, 2, 1);
        v->addWidget(controls::section("Contact", contactBox, this, kSections));

        auto* listBox = new QWidget(this);
        auto* lv = new QVBoxLayout(listBox);
        lv->setContentsMargins(0, 0, 0, 0);
        lv->setSpacing(4);
        mList = new QTreeWidget(listBox);
        mList->setColumnCount(2);
        mList->setHeaderHidden(true);
        mList->setRootIsDecorated(false);
        mList->setSelectionMode(QAbstractItemView::ExtendedSelection);
        mList->header()->setStretchLastSection(false);
        mList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        mList->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        mList->setMinimumHeight(90);
        connect(mList, &QTreeWidget::itemSelectionChanged, this, [this] {
            if (mSyncing)
                return;
            Graph& g = Graph::current();
            std::vector<NodeId> ids;
            for (QTreeWidgetItem* it : mList->selectedItems())
                if (Node* n = named(g, it->data(0, Qt::UserRole).toString().toStdString()))
                    ids.push_back(n->id());
            if (!ids.empty())
                g.select(ids);
        });
        lv->addWidget(mList);
        mEmpty = new QLabel("No contacts. Drag from one character onto another with the Grapple tool.", listBox);
        mEmpty->setWordWrap(true);
        mEmpty->setForegroundRole(QPalette::PlaceholderText);
        lv->addWidget(mEmpty);
        auto* row = new QHBoxLayout;
        row->setSpacing(2);
        for (const char* id : {"puppeteer.grapple.keyOn", "puppeteer.grapple.keyOff"})
            row->addWidget(controls::push(id, listBox));
        lv->addLayout(row);
        v->addWidget(controls::section("Contacts", listBox, this, kSections));

        mFiguresBox = new QWidget(this);
        mFigures = new QGridLayout(mFiguresBox);
        mFigures->setContentsMargins(0, 0, 0, 0);
        mFigures->setVerticalSpacing(4);
        mFigures->setHorizontalSpacing(8);
        mFiguresSection = controls::section("Characters", mFiguresBox, this, kSections);
        v->addWidget(mFiguresSection);

        auto* solveBox = new QWidget(this);
        auto* sv = new QVBoxLayout(solveBox);
        sv->setContentsMargins(0, 0, 0, 0);
        sv->setSpacing(4);
        auto* buttons = new QHBoxLayout;
        buttons->setSpacing(2);
        for (const char* id : {"puppeteer.grapple.bake", "puppeteer.grapple.ragdoll", "puppeteer.grapple.recordPhysics"})
            buttons->addWidget(controls::push(id, solveBox));
        sv->addLayout(buttons);
        auto check = [&](const char* text, const char* tip) {
            auto* c = new QCheckBox(text, solveBox);
            c->setToolTip(tip);
            sv->addWidget(c);
            return c;
        };
        mAutoBake = check("Auto Bake", "Auto Bake: bake again after every contact made or keyed here");
        connect(mAutoBake, &QCheckBox::clicked, this, [](bool on) {
            settings().autoBake = on;
            saveSettings();
            viewportTools::requestRedraw();
        });
        mCollide = check("Collide", "Collide: keep the characters' bodies from passing through each other");
        connect(mCollide, &QCheckBox::clicked, this, [this](bool on) { setGrappleFlag("collide", on); });
        mAutoPose = check("Auto-Pose", "Auto-Pose: the auto-poser places what the contacts don't hold");
        connect(mAutoPose, &QCheckBox::clicked, this, [this](bool on) { setGrappleFlag("autoPose", on); });
        mStatus = new QLabel(solveBox);
        mStatus->setForegroundRole(QPalette::PlaceholderText);
        mStatus->setToolTip("Gap: the largest contact gap left at this frame. Overlap: the deepest the bodies pass into each other.");
        sv->addWidget(mStatus);
        v->addWidget(controls::section("Solve", solveBox, this, kSections));
        v->addStretch();

        for (events::Event e : {events::Event::UndoStackChanged, events::Event::SelectionChanged, events::Event::TimeChanged, events::Event::SceneOpened,
                                events::Event::SceneNew})
            mSubs.push_back(events::subscribe(e, [p = QPointer<Palette>(this)](const events::EventData&) {
                if (p)
                    p->sync();
            }, "Puppeteer"));
        sync();
    }

    ~Palette() override
    {
        for (events::SubscriptionId s : mSubs)
            events::unsubscribe(s);
    }

    void sync()
    {
        Graph& g = Graph::current();
        const Settings& s = settings();
        mKind->setCurrent(s.kind);
        mLead->setCurrent(s.lead);
        {
            const QSignalBlocker b(mBlend);
            mBlend->setValue(s.blendFrames);
        }
        mAutoBake->setChecked(s.autoBake);
        const NodeId gr = currentGrapple(g);
        mGrapple = gr;

        mSyncing = true;
        mList->clear();
        std::vector<NodeId> selected = g.selection();
        if (gr)
            for (NodeId c : gp::contactsOf(g, gr))
            {
                const auto [sj, tj] = gp::contactJoints(g, c);
                const Node& n = *g.node(c);
                const double weight = std::get<double>(g.value({c, n.type().attrIndex("weight"), -1}));
                auto* it = new QTreeWidgetItem(mList, {QString::fromStdString(shortName(g, sj) + "  \xe2\x86\x92  " + shortName(g, tj)),
                                                       QString::number(weight, 'f', 2)});
                it->setData(0, Qt::UserRole, QString::fromStdString(g.path(c)));
                it->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
                it->setToolTip(0, QString::fromStdString(g.path(c)));
                it->setSelected(std::find(selected.begin(), selected.end(), c) != selected.end());
            }
        mSyncing = false;
        mList->setVisible(mList->topLevelItemCount() > 0);
        mEmpty->setVisible(mList->topLevelItemCount() == 0);

        syncFigures(g, gr);
        for (auto [box, attr] : {std::pair{mCollide, "collide"}, std::pair{mAutoPose, "autoPose"}})
        {
            const QSignalBlocker b(box);
            box->setEnabled(gr != kNoNode);
            if (gr)
                box->setChecked(g.node(gr)->as<bool>(g.node(gr)->type().attrIndex(attr)));
        }
        QString status;
        if (gr)
            if (auto f = sim::cached(g, gr, sim::frameOf(g.time())); f && f->state.size() >= 3)
                status = QString("Gap %1 cm    Overlap %2 cm")
                             .arg(f->state[f->state.size() - 3], 0, 'f', 2)
                             .arg(f->state[f->state.size() - 2], 0, 'f', 2);
        mStatus->setText(status);
        mStatus->setVisible(!status.isEmpty());
    }

private:
    static void applyLeadToSelection(int lead)
    {
        if (lead <= 0)
            return;
        Graph& g = Graph::current();
        std::vector<std::string> plugs;
        for (NodeId id : g.selection())
            if (Node* n = g.node(id); n && isContactNode(*n))
                plugs.push_back(n->path() + ".lead");
        if (plugs.empty())
            return;
        for (const std::string& p : plugs)
            runCommand("setAttr", {{"args", {p, lead}}});
        if (settings().autoBake)
            bake(currentGrapple(g));
    }

    void setGrappleFlag(const char* attr, bool on)
    {
        Graph& g = Graph::current();
        if (!mGrapple || !g.node(mGrapple))
            return;
        runCommand("setAttr", {{"args", {g.path(mGrapple) + "." + attr, on}}});
        if (settings().autoBake)
            bake(mGrapple);
    }

    void syncFigures(Graph& g, NodeId gr)
    {
        const std::vector<gp::FigureSetup> figures = gr ? gp::figuresOf(g, gr) : std::vector<gp::FigureSetup>{};
        std::string key;
        for (const gp::FigureSetup& f : figures)
            key += g.path(f.root) + ";";
        if (key != mFigureKey)
        {
            mFigureKey = key;
            while (QLayoutItem* it = mFigures->takeAt(0))
            {
                delete it->widget();
                delete it;
            }
            mMass.clear();
            mPlant.clear();
            int row = 0;
            for (const gp::FigureSetup& f : figures)
            {
                const std::string root = g.path(f.root);
                // A character's name, else the root's shortest path, which names the group two same-named skeletons sit in.
                std::string name = g.path(f.root);
                if (const NodeId c = characterFor(g, f.root))
                    name = g.node(c)->name();
                auto* l = new QLabel(QString::fromStdString(name), mFiguresBox);
                l->setToolTip(QString::fromStdString(root + "\nBodies fitted from " + gp::fitName(f.fit)));
                mFigures->addWidget(l, row, 0);
                auto* mass = controls::valueField(1.0, 1000.0, 0, "kg", mFiguresBox);
                mass->setToolTip("Mass: the heavier character yields less in every contact it shares by mass");
                connect(mass, &QDoubleSpinBox::editingFinished, this, [root, mass] {
                    runCommand("grappleFigure", {{"args", {root}}, {"mass", mass->value()}});
                    if (settings().autoBake)
                        bake(currentGrapple(Graph::current()));
                });
                mFigures->addWidget(mass, row, 1);
                auto* plant = new QCheckBox("Planted", mFiguresBox);
                plant->setToolTip("Planted: the feet stay where the animation puts them");
                connect(plant, &QCheckBox::clicked, this, [root](bool on) {
                    runCommand("grappleFigure", {{"args", {root}}, {"plant", on}});
                    if (settings().autoBake)
                        bake(currentGrapple(Graph::current()));
                });
                mFigures->addWidget(plant, row, 2);
                mMass.push_back(mass);
                mPlant.push_back(plant);
                ++row;
            }
        }
        for (size_t i = 0; i < figures.size() && i < mMass.size(); ++i)
        {
            const QSignalBlocker b1(mMass[i]), b2(mPlant[i]);
            if (!mMass[i]->hasFocus())
                mMass[i]->setValue(figures[i].mass);
            mPlant[i]->setChecked(figures[i].plant);
        }
        mFiguresSection->setVisible(!figures.empty());
    }

    controls::Segmented *mKind = nullptr, *mLead = nullptr;
    QDoubleSpinBox* mBlend = nullptr;
    QTreeWidget* mList = nullptr;
    QLabel *mEmpty = nullptr, *mStatus = nullptr;
    QWidget *mFiguresBox = nullptr, *mFiguresSection = nullptr;
    QGridLayout* mFigures = nullptr;
    std::vector<QDoubleSpinBox*> mMass;
    std::vector<QCheckBox*> mPlant;
    std::string mFigureKey;
    QCheckBox *mAutoBake = nullptr, *mCollide = nullptr, *mAutoPose = nullptr;
    NodeId mGrapple = kNoNode;
    bool mSyncing = false;
    std::vector<events::SubscriptionId> mSubs;
};

// Contacts in the viewport --------------------------------------------------------------------------------------------

void drawContact(drawOverrides::DrawContext& c)
{
    Graph& g = Graph::current();
    const NodeId id = c.node.id();
    const auto [sj, tj] = gp::contactJoints(g, id);
    if (!sj || !tj)
        return;
    const auto [s, t] = gp::contactPoints(g, id);
    const double weight = c.node.as<double>(c.node.type().attrIndex("weight"));
    const auto ps = c.view.project(s), pt = c.view.project(t);
    if (!ps || !pt)
        return;
    const bool selected = c.highlight != drawOverrides::Highlight::None;
    const glm::vec4 col = selected ? kSelected : weight > 1e-3 ? kLink : kLinkOff;
    c.painter.smoothLine(*ps, *pt, selected ? 2.0f : 1.5f, col);
    c.painter.smoothDisc(*pt, selected ? 5.0 : 4.0, col);
    c.painter.smoothDisc(*ps, 2.5, col);
}

std::optional<double> pickContact(const drawOverrides::PickContext& c)
{
    Graph& g = Graph::current();
    const auto [s, t] = gp::contactPoints(g, c.node.id());
    const auto ps = c.view.project(s), pt = c.view.project(t);
    if (!ps || !pt)
        return std::nullopt;
    if (!c.click)
    {
        const QRectF box = QRectF(c.a, c.b).normalized();
        return box.contains(*pt) ? std::optional<double>(0.0) : std::nullopt;
    }
    const QPointF d = *pt - *ps, p = c.a - *ps;
    const double l2 = d.x() * d.x() + d.y() * d.y();
    const double u = l2 > 1e-9 ? std::clamp((p.x() * d.x() + p.y() * d.y()) / l2, 0.0, 1.0) : 0.0;
    const QPointF q = *ps + u * d - c.a;
    const double dist = std::min(std::hypot(q.x(), q.y()), std::hypot(c.a.x() - pt->x(), c.a.y() - pt->y()) - 3.0);
    return dist <= drawOverrides::kClickPixels ? std::optional<double>(std::max(dist, 0.0)) : std::nullopt;
}
} // namespace grappleui

void registerGrapple(ext::Context& ctx)
{
    using namespace grappleui;
    const char* cat = "Puppeteer Grapple";
    const std::vector<std::string> keywords = {"grapple", "wrestle", "fight", "contact", "interaction", "two characters", "multi-character",
                                               "paired", "grab", "hold", "carry", "lift", "throw", "headlock", "push"};
    viewportTools::add(ctx, {.id = kToolId, .label = "Grapple", .icon = "hand-grab", .create = [] { return std::make_unique<GrappleTool>(); }});
    panels::add(ctx, {.id = kPaletteId,
                      .title = "Grapple",
                      .create = [](QWidget* parent) -> QWidget* { return new Palette(parent); },
                      .icon = "hand-grab",
                      .open = false,
                      .toggleAction = "window.puppeteerGrapple",
                      .keywords = keywords,
                      .toolPalette = true,
                      .docs = "grappling"});
    drawOverrides::add(ctx, {.id = "puppeteer.grappleContact", .nodeType = gp::kContactType, .draw = drawContact, .pick = pickContact});

    actions::add(ctx, {.id = "puppeteer.grapple.tool",
                       .label = "Grapple Tool",
                       .category = cat,
                       .run = [] { viewportTools::activate(kToolId); },
                       .doc = "Drag from a hand (or any body part) of one character onto another's body: the two are held there and "
                              "solved together, each yielding its share, kept from passing through each other.",
                       .icon = "hand-grab",
                       .keywords = keywords});
    actions::add(ctx, {.id = "puppeteer.grapple.keyOn",
                       .label = "Contact On",
                       .category = cat,
                       .run = [] { keyContacts(true); },
                       .doc = "Key the selected contacts (else all of the grapple's) on at the current frame, blending in over the palette's Blend.",
                       .icon = "link",
                       .keywords = {"grapple", "contact", "key on", "attach", "enable"}});
    actions::add(ctx, {.id = "puppeteer.grapple.keyOff",
                       .label = "Contact Off",
                       .category = cat,
                       .run = [] { keyContacts(false); },
                       .doc = "Key the selected contacts (else all of the grapple's) off at the current frame, blending out over the palette's Blend: "
                              "a release or a throw.",
                       .icon = "unlink",
                       .keywords = {"grapple", "contact", "key off", "release", "let go", "throw", "detach"}});
    actions::add(ctx, {.id = "puppeteer.grapple.bake",
                       .label = "Bake Grapple",
                       .category = cat,
                       .run = [] { bake(currentGrapple(Graph::current())); },
                       .enabled = [] { return currentGrapple(Graph::current()) != kNoNode; },
                       .doc = "Solve the grapple over the playback range and bake it to plain keys on its own additive layer, replacing the last bake.",
                       .icon = "player-record",
                       .keywords = {"grapple", "bake", "solve", "keys", "plot", "record"}});
    actions::add(ctx, {.id = "puppeteer.grapple.ragdoll",
                       .label = "Grapple Ragdoll",
                       .category = cat,
                       .run = [] {
                           Graph& g = Graph::current();
                           if (const NodeId gr = currentGrapple(g))
                               runCommand("grappleRagdoll", {{"args", {g.path(gr)}}});
                       },
                       .enabled = [] { return currentGrapple(Graph::current()) != kNoNode && ops::find("grappleRagdoll"); },
                       .doc = "Make the grapple's characters one ragdoll with its contacts as springs between them: throws, pushes, falls and "
                              "struggles that both bodies react to. Record Physics bakes it.",
                       .icon = "atom",
                       .keywords = {"grapple", "ragdoll", "physics", "throw", "fall", "struggle", "powered ragdoll", "simulate"}});
    actions::add(ctx, {.id = "puppeteer.grapple.recordPhysics",
                       .label = "Record Physics",
                       .category = cat,
                       .run = [] {
                           Graph& g = Graph::current();
                           const NodeId gr = currentGrapple(g);
                           for (NodeId id : g.nodes())
                               if (Node* s = g.node(id); s && s->type().name == "physicsSolver")
                                   if (Node* target = named(g, textOf(*s, "grapple")); target && target->id() == gr)
                                       return void(runCommand("recordPhysics", {{"args", {s->path()}}}));
                           log::write(LogLevel::Warning, "Grapple: no ragdoll for this grapple yet; run Grapple Ragdoll first");
                       },
                       .enabled = [] { return currentGrapple(Graph::current()) != kNoNode && ops::find("recordPhysics"); },
                       .doc = "Bake the grapple's ragdoll onto an additive layer as plain keys.",
                       .icon = "player-record",
                       .keywords = {"grapple", "record", "physics", "bake", "ragdoll"}});

    menus::addItem(ctx, {.id = "puppeteer.grapple.menu", .menu = "Puppeteer", .kind = menus::Kind::Submenu, .label = "Grapple"});
    menus::addItem(ctx, {.id = "anim.grapple.menu", .menu = "Animation", .kind = menus::Kind::Submenu, .label = "Grapple"});
    int sep = 0;
    for (const char* menu : {"Puppeteer/Grapple", "Animation/Grapple"})
        for (const char* a : {"puppeteer.grapple.tool", "", "puppeteer.grapple.keyOn", "puppeteer.grapple.keyOff", "", "puppeteer.grapple.bake",
                              "puppeteer.grapple.ragdoll", "puppeteer.grapple.recordPhysics", "", "window.puppeteerGrapple"})
            menus::addItem(ctx, *a ? menus::Item{.menu = menu, .action = a}
                                   : menus::Item{.id = "puppeteer.grapple.sep" + std::to_string(sep++), .menu = menu, .kind = menus::Kind::Separator});
    menus::addItem(ctx, {.menu = "Animation/Physics", .action = "puppeteer.grapple.ragdoll"});
}
} // namespace puppeteer
