// Props, the UI: the Props panel (presets, settings, points, holds and handoffs, aim, the track over time, solve,
// Unreal), point handles drawn in the viewport, and the prop actions and menus. Every edit is a command.

#include "prop/Props.h"

#include "Puppeteer.h"
#include "hand/Hands.h"
#include "rig/Rig.h"

#include <lila/Controls.h>
#include <lila/Events.h>
#include <lila/Grapple.h>
#include <lila/Math.h>
#include <lila/Icons.h>
#include <lila/Pins.h>
#include <lila/Prefs.h>
#include <lila/Simulation.h>
#include <lila/Spaces.h>
#include <lila/UI.h>
#include <lila/Viewport.h>
#include <lila/ViewportTool.h>

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace puppeteer::prop
{
namespace
{
constexpr const char* kCat = "Puppeteer Props";
constexpr const char* kSections = "/props/sections";
constexpr const char* kPanelId = "puppeteerProps";

const std::vector<std::string>& keywords()
{
    static const std::vector<std::string> kw = {"prop", "weapon", "sword", "gun", "pistol", "rifle", "shotgun", "bazooka", "launcher", "bow",
                                                "shield", "axe", "spear", "dagger", "attach", "hold", "wield", "equip", "holster", "grip", "aim"};
    return kw;
}

/** The panel's own values, kept between sessions. */
struct PanelPrefs
{
    double blendFrames = 6.0;
    double aimWeight = 1.0;
};

PanelPrefs& settings()
{
    static PanelPrefs s = [] {
        const PrefStore& p = PrefStore::get();
        PanelPrefs c;
        c.blendFrames = std::max(0.0, p.value<double>(kPrefs, "/props/blendFrames", 6.0));
        c.aimWeight = std::clamp(p.value<double>(kPrefs, "/props/aimWeight", 1.0), 0.0, 1.0);
        return c;
    }();
    return s;
}

void saveSettings()
{
    PrefStore& p = PrefStore::get();
    const PanelPrefs& s = settings();
    p.set(kPrefs, "/props/blendFrames", s.blendFrames);
    p.set(kPrefs, "/props/aimWeight", s.aimWeight);
}

double blendSeconds() { return settings().blendFrames / std::max(anim::playback().fps, 1e-6); }

/** The prop the UI works on: the selection's, else the scene's only one. */
NodeId currentProp(Graph& g)
{
    for (NodeId id : g.selection())
        if (const NodeId p = propFor(g, id))
            return p;
    const std::vector<NodeId> all = props(g);
    return all.size() == 1 ? all.front() : kNoNode;
}

std::optional<Prop> current(Graph& g)
{
    const NodeId p = currentProp(g);
    return p ? load(g, p) : std::nullopt;
}

json propCall(Graph& g, const Prop& p)
{
    json call = {{"args", {g.path(p.root)}}, {"blend", blendSeconds()}};
    // The selected character, when one is: otherwise the command picks the holder or the only one.
    for (NodeId id : g.selection())
        if (const NodeId c = characterFor(g, id))
        {
            call["character"] = g.path(c);
            break;
        }
    return call;
}

void holdAction(const std::string& hand)
{
    Graph& g = Graph::current();
    const std::optional<Prop> p = current(g);
    if (!p)
        return void(log::write(LogLevel::Warning, "Props: select a prop (Make Prop makes one from a mesh)"));
    json call = propCall(g, *p);
    call["hand"] = hand;
    runCommand("puppeteerPropHold", call);
}

/** Holds the prop in the hand that isn't holding it now, keeping one or two hands as it is. */
void handOffAction()
{
    Graph& g = Graph::current();
    const std::optional<Prop> p = current(g);
    if (!p)
        return;
    g.evaluate();
    const NodeId holder = holderNow(g, *p);
    std::string hand = p->rightHanded ? "left" : "right";
    if (holder)
        if (const NodeId hn = hand::handFor(g, holder))
            if (const std::optional<hand::Hand> h = hand::load(g, hn))
                hand = h->side == hand::Side::Left ? "right" : "left";
    holdAction(hand);
}

void simpleAction(const char* command)
{
    Graph& g = Graph::current();
    const std::optional<Prop> p = current(g);
    if (!p)
        return void(log::write(LogLevel::Warning, "Props: select a prop first"));
    json call = propCall(g, *p);
    if (std::string(command) == "puppeteerPropDrop")
        call.erase("character");
    runCommand(command, call);
}

/** The aim target: the other selected transform, else a new locator in front of the character, selected to place. */
void aimAction()
{
    Graph& g = Graph::current();
    const std::optional<Prop> p = current(g);
    if (!p)
        return void(log::write(LogLevel::Warning, "Props: select a prop to aim"));
    std::string target;
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && !propFor(g, id) && !characterFor(g, id) && n->type().attrIndex("worldMatrix") >= 0)
            target = n->path();
    g.evaluate();
    if (target.empty())
    {
        const NodeId holder = holderNow(g, *p);
        const NodeId ch = holder ? characterFor(g, holder) : kNoNode;
        glm::dvec3 at = glm::dvec3(worldOf(g, p->root)[3]);
        const glm::dvec3 up = lila::math::axisBasis().up;
        glm::dvec3 forward = lila::math::axisBasis().forward;
        // The character faces across the line from its left hand to its right.
        glm::dvec3 right(0.0), left(0.0);
        for (NodeId hn : hand::handsOf(g, ch))
            if (const std::optional<hand::Hand> h = hand::load(g, hn))
                (h->side == hand::Side::Left ? left : right) = glm::dvec3(worldOf(g, h->joint)[3]);
        glm::dvec3 across = right - left;
        across -= up * glm::dot(across, up);
        if (ch && glm::length(across) > 1e-3)
            forward = glm::normalize(glm::cross(up, across));
        at += forward * 500.0;
        const json made = runCommand("spaceLocator", {{"name", "propTarget"}});
        if (!made.is_array() || made.empty())
            return;
        target = made.at(0).get<std::string>();
        runCommand("setAttr", {{"args", {target + ".translate", at.x, at.y, at.z}}});
    }
    json call = propCall(g, *p);
    call["target"] = target;
    call["weight"] = settings().aimWeight;
    runCommand("puppeteerPropAim", call);
    if (Node* t = named(g, target))
        g.select({t->id()});
}

void aimOffAction()
{
    Graph& g = Graph::current();
    const std::optional<Prop> p = current(g);
    if (!p)
        return;
    json call = propCall(g, *p);
    call["off"] = true;
    runCommand("puppeteerPropAim", call);
}

/** The selection's meshes and groups as prop roots, characters left out. */
json propRootsOfSelection(Graph& g)
{
    json args = json::array();
    for (NodeId id : g.selection())
    {
        NodeId root = id;
        if (const NodeId pn = propFor(g, id))
            root = g.parent(pn);
        else if (characterFor(g, id))
            continue;
        const std::string path = g.path(root);
        if (std::find(args.begin(), args.end(), path) == args.end())
            args.push_back(path);
    }
    return args;
}

/** Makes the selection props, their settings from their shapes, or with a preset's. */
void setupAction(const Preset* preset)
{
    Graph& g = Graph::current();
    const json args = propRootsOfSelection(g);
    if (args.empty())
        return void(log::write(LogLevel::Warning, "Props: select a mesh or group to make a prop"));
    json call = {{"args", args}};
    if (preset)
        call["preset"] = preset->id;
    runCommand("puppeteerPropSetup", call);
}

/** The selected points of the current prop. */
std::vector<NodeId> selectedPoints(Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && n->type().isA(kPointType) && propFor(g, id))
            out.push_back(id);
    return out;
}
bool hasProp() { return currentProp(Graph::current()) != kNoNode; }

/** Whether the character holding the prop (or the selected one) has every bone Unreal's hand IK bones need. */
bool hasIkBones(Graph& g, NodeId character)
{
    if (!character)
        return false;
    int found = 0;
    const NodeId root = characterRoot(g, character);
    for (NodeId d : g.descendants(root))
    {
        std::string n = g.node(d)->name();
        if (const size_t c = n.rfind(':'); c != std::string::npos)
            n = n.substr(c + 1);
        found += n == "ik_hand_gun" || n == "ik_hand_r" || n == "ik_hand_l";
    }
    return found == 3;
}

NodeId propCharacter(Graph& g, const Prop& p)
{
    for (const Hold& h : holdsOf(g, p))
        if (h.target)
            if (const NodeId c = characterFor(g, h.target))
                return c;
    const std::vector<NodeId> all = characters(g);
    return all.size() == 1 ? all.front() : kNoNode;
}

std::string bare(const std::string& path)
{
    std::string n = path.substr(path.rfind('|') == std::string::npos ? 0 : path.rfind('|') + 1);
    if (const size_t c = n.rfind(':'); c != std::string::npos)
        n = n.substr(c + 1);
    return n;
}

QString holdLabel(const Hold& h)
{
    if (h.kind == "hand")
        return h.side == "left" ? "Left hand" : "Right hand";
    if (h.kind == "holster")
        return "Holster";
    if (h.kind == "world")
        return "World";
    if (h.kind == "parent")
        return "Parent";
    return QString::fromStdString(bare(h.space));
}

// Panel -------------------------------------------------------------------------------------------------------------

/** One row of the Props panel's track. */
struct TrackRow
{
    double time = 0.0;
    QString label, tip;
    /** What a click selects besides going to the frame (a throw's arc); empty for nothing. */
    QString select;
};

QString roleLabel(Role r)
{
    static const char* kLabels[] = {"Main grip", "Off grip", "Grip", "Muzzle", "Tip", "Stock", "Holster"};
    return kLabels[std::clamp(int(r), 0, int(Role::Count) - 1)];
}

/** A hand's row label, with its character's name when the scene has more than one. */
QString handLabel(Graph& g, const Hold& h)
{
    QString label = holdLabel(h);
    if (characters(g).size() > 1)
        if (const NodeId c = h.target ? characterFor(g, h.target) : kNoNode)
            label = QString::fromStdString(g.node(c)->name()) + ", " + label.toLower();
    return label;
}

/** The prop's holds and handoffs in time order, each named by what happened; the scene's own place before the first left out. */
std::vector<TrackRow> trackRows(Graph& g, const Prop& p)
{
    std::vector<TrackRow> out;
    const std::vector<Hold> holds = holdsOf(g, p);
    const json events = eventsOf(g, p);
    const double half = 0.5 / std::max(anim::playback().fps, 1e-6);
    bool held = false;
    for (const Hold& h : holds)
    {
        if (h.kind == "parent" && &h == &holds.front() && holds.size() > 1)
            continue;
        held |= h.kind != "parent";
        json ev;
        for (const json& e : events)
            if (std::abs(e.value("time", 0.0) - h.time) < half)
                ev = e;
        const std::string kind = ev.is_object() ? ev.value("kind", std::string()) : std::string();
        QString label = h.kind == "hand" ? handLabel(g, h) : holdLabel(h), select;
        if (kind == "take")
            label = "Taken, " + label.toLower();
        else if (kind == "catch")
            label = "Caught, " + label.toLower();
        else if (kind == "pass")
            label = "Passed, " + label.toLower();
        else if (kind == "drop")
            label = "Dropped";
        else if (kind == "plant")
            label = "Planted";
        else if (kind == "throw")
        {
            label = ev.value("physics", false) ? "Thrown, physics" : "Thrown";
            select = QString::fromStdString(ev.value("arc", std::string()));
        }
        else if (kind == "place")
            label = "On " + QString::fromStdString(bare(ev.value("on", std::string())));
        out.push_back({h.time, label, QString::fromStdString(h.space), select});
    }
    if (!held)
        out.clear();
    return out;
}

/** Sets one attr of the current prop node, as a command so it undoes. */
void setPropAttr(const char* attr, const json& value)
{
    Graph& g = Graph::current();
    if (const std::optional<Prop> p = current(g))
        runCommand("setAttr", {{"args", {g.path(p->node) + "." + attr, value}}});
}

/** The hand poses a prop can start from: the built-in ones and the saved ones. */
std::vector<std::pair<QString, QString>> handPoses()
{
    std::vector<std::pair<QString, QString>> out;
    try
    {
        for (const json& p : lila::ops::execute("puppeteerHandPoses", json::object()))
            out.push_back({QString::fromStdString(p.value("name", std::string())), QString::fromStdString(p.value("label", p.value("name", std::string())))});
    }
    catch (const std::exception&)
    {
    }
    return out;
}

class PropsPanel final : public QWidget
{
public:
    explicit PropsPanel(QWidget* parent) : QWidget(parent)
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 8, 12, 8);
        v->setSpacing(4);

        auto* head = new QHBoxLayout;
        head->setSpacing(8);
        mTitle = new QLabel(this);
        QFont f = mTitle->font();
        f.setBold(true);
        mTitle->setFont(f);
        head->addWidget(mTitle);
        mPreset = new QLabel(this);
        mPreset->setForegroundRole(QPalette::PlaceholderText);
        mPreset->setToolTip("The preset the prop's settings match, or Custom");
        head->addWidget(mPreset, 1);
        v->addLayout(head);
        mEmpty = new QLabel("No prop selected. Select a mesh, then Make Prop or a preset.", this);
        mEmpty->setWordWrap(true);
        mEmpty->setForegroundRole(QPalette::PlaceholderText);
        v->addWidget(mEmpty);

        auto label = [](QGridLayout* grid, const char* text, int row, QWidget* owner) {
            auto* l = new QLabel(text, owner);
            l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            grid->addWidget(l, row, 0);
            return l;
        };

        // Presets: one tile each, lit when the prop's settings match it.
        auto* presetBox = new QWidget(this);
        std::vector<QWidget*> tiles;
        for (const Preset& p : presets())
        {
            QToolButton* t = controls::tile(std::string("puppeteer.prop.preset.") + p.id, p.icon, presetBox, p.label);
            tiles.push_back(t);
            mTiles.push_back(t);
        }
        auto* pv = new QVBoxLayout(presetBox);
        pv->setContentsMargins(0, 0, 0, 0);
        pv->addWidget(controls::grid(tiles, 4, presetBox));
        v->addWidget(controls::section("Presets", presetBox, this, kSections));

        // Settings: how the prop is held, aimed and put away.
        auto* setBox = new QWidget(this);
        auto* form = new QGridLayout(setBox);
        form->setContentsMargins(0, 0, 0, 0);
        form->setHorizontalSpacing(8);
        form->setVerticalSpacing(4);
        form->setColumnStretch(1, 1);
        int row = 0;
        mHand = new controls::Segmented(setBox);
        mHand->add("Right", {}, "Hand: the prop's main hand, which Hold uses", 0);
        mHand->add("Left", {}, "Hand: the prop's main hand, which Hold uses", 1);
        connect(mHand, &controls::Segmented::picked, this, [](int id) { setPropAttr("hand", id); });
        label(form, "Hand", row, setBox);
        form->addWidget(mHand, row++, 1);
        mHands = new controls::Segmented(setBox);
        mHands->add("One", {}, "Hands: Hold puts it in the main hand only", 1);
        mHands->add("Both", {}, "Hands: Hold puts the other hand on the off grip too, both arms sharing the reach", 2);
        connect(mHands, &controls::Segmented::picked, this, [](int id) { setPropAttr("twoHanded", id == 2); });
        label(form, "Hands", row, setBox);
        form->addWidget(mHands, row++, 1);
        mAim = new controls::Segmented(setBox);
        mAim->add("Turn", {}, "Aim: turned where it is", int(AimStyle::Turn));
        mAim->add("Shoulder", {}, "Aim: the stock to the shoulder, then turned onto the target", int(AimStyle::Shoulder));
        mAim->add("Extend", {}, "Aim: held out toward the target", int(AimStyle::Extend));
        mAim->add("Guard", {}, "Aim: held out, below the line to the target", int(AimStyle::Guard));
        connect(mAim, &controls::Segmented::picked, this, [](int id) { setPropAttr("aimStyle", id); });
        label(form, "Aim", row, setBox);
        form->addWidget(mAim, row++, 1);
        mReach = controls::valueField(0.0, 1.0, 2, "", setBox);
        mReach->setToolTip("Reach: the share of the arm's length an extend or guard aim holds the grip out");
        connect(mReach, &QDoubleSpinBox::editingFinished, this, [this] { setPropAttr("aimReach", mReach->value()); });
        mReachLabel = label(form, "Reach", row, setBox);
        form->addWidget(mReach, row++, 1);
        mDrop = controls::valueField(0.0, 100.0, 1, "cm", setBox);
        mDrop->setToolTip("Drop: how far below the line to the target a guard holds the grip");
        connect(mDrop, &QDoubleSpinBox::editingFinished, this, [this] { setPropAttr("aimDrop", mDrop->value()); });
        mDropLabel = label(form, "Drop", row, setBox);
        form->addWidget(mDrop, row++, 1);
        mHolster = new QComboBox(setBox);
        for (int i = 0; i < int(HolsterSpot::Count); ++i)
            mHolster->addItem(holsterSpot(HolsterSpot(i)).label, i);
        mHolster->setToolTip("Holster: where Holster puts it away the first time; then move the holster point it made");
        connect(mHolster, &QComboBox::activated, this, [this](int i) { setPropAttr("holsterSpot", mHolster->itemData(i).toInt()); });
        label(form, "Holster", row, setBox);
        form->addWidget(mHolster, row++, 1);
        for (QComboBox** box : {&mMainPose, &mOffPose})
        {
            *box = new QComboBox(setBox);
            for (const auto& [name, text] : handPoses())
                (*box)->addItem(text, name);
        }
        mMainPose->setToolTip("Grip pose: the main hand's pose before its fingers wrap the prop");
        mOffPose->setToolTip("Other pose: the other hand's pose");
        connect(mMainPose, &QComboBox::activated, this, [this](int i) { setPropAttr("mainPose", mMainPose->itemData(i).toString().toStdString()); });
        connect(mOffPose, &QComboBox::activated, this, [this](int i) { setPropAttr("offPose", mOffPose->itemData(i).toString().toStdString()); });
        label(form, "Grip pose", row, setBox);
        form->addWidget(mMainPose, row++, 1);
        label(form, "Other pose", row, setBox);
        form->addWidget(mOffPose, row++, 1);
        mShape = new QComboBox(setBox);
        for (const char* shapeName : {"Auto", "Blade", "Haft", "Pole", "Firearm", "Tube", "Plate", "Bow", "Other"})
            mShape->addItem(shapeName);
        mShape->setToolTip("Shape: how Fit Points reads the mesh. Auto reads it from the mesh; pick one when it reads wrong, then Fit Points");
        connect(mShape, &QComboBox::activated, this, [](int i) { setPropAttr("fitShape", i); });
        label(form, "Shape", row, setBox);
        form->addWidget(mShape, row++, 1);
        mTrigger = new QCheckBox("Trigger finger", setBox);
        mTrigger->setToolTip("Trigger finger: the main hand's index finger stays posed instead of wrapping");
        connect(mTrigger, &QCheckBox::clicked, this, [](bool on) { setPropAttr("trigger", on); });
        form->addWidget(mTrigger, row++, 1);
        v->addWidget(controls::section("Settings", setBox, this, kSections));

        // Points: where hands take it and what aims; a row selects its point to move in the viewport.
        auto* ptBox = new QWidget(this);
        auto* ptv = new QVBoxLayout(ptBox);
        ptv->setContentsMargins(0, 0, 0, 0);
        ptv->setSpacing(4);
        mPoints = new QTreeWidget(ptBox);
        mPoints->setColumnCount(2);
        mPoints->setHeaderHidden(true);
        mPoints->setRootIsDecorated(false);
        mPoints->setSelectionMode(QAbstractItemView::ExtendedSelection);
        mPoints->header()->setStretchLastSection(true);
        mPoints->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        mPoints->setMinimumHeight(70);
        mPoints->setToolTip("The prop's points. Select one to move or turn it in the viewport");
        connect(mPoints, &QTreeWidget::itemSelectionChanged, this, [this] {
            json args = json::array();
            for (QTreeWidgetItem* it : mPoints->selectedItems())
                args.push_back(it->data(0, Qt::UserRole).toString().toStdString());
            if (!args.empty())
                runCommand("select", {{"args", args}});
        });
        ptv->addWidget(mPoints);
        auto* ptRow = new QHBoxLayout;
        ptRow->setSpacing(4);
        ptRow->addWidget(controls::push("puppeteer.prop.fit", ptBox));
        ptRow->addStretch();
        ptRow->addWidget(controls::push("puppeteer.prop.addGrip", ptBox, "Add"));
        ptRow->addWidget(controls::push("puppeteer.prop.removePoint", ptBox, "Remove"));
        ptv->addLayout(ptRow);
        v->addWidget(controls::section("Points", ptBox, this, kSections));

        // Hold: the daily actions, then the blend they share.
        auto* holdBox = new QWidget(this);
        auto* hv = new QVBoxLayout(holdBox);
        hv->setContentsMargins(0, 0, 0, 0);
        hv->setSpacing(4);
        hv->addWidget(controls::grid({controls::row("puppeteer.prop.hold", "hand-grab", holdBox), controls::row("puppeteer.prop.handOff", "arrows-exchange", holdBox),
                                      controls::row("puppeteer.prop.holster", "backpack", holdBox), controls::row("puppeteer.prop.drop", "hand-stop", holdBox)},
                                     2, holdBox));
        auto* blendRow = new QGridLayout;
        blendRow->setHorizontalSpacing(8);
        mBlend = controls::valueField(0.0, 240.0, 0, "f", holdBox);
        mBlend->setToolTip("Blend: frames a hold, handoff or aim takes to settle after the current frame");
        connect(mBlend, &QDoubleSpinBox::valueChanged, this, [](double fr) {
            settings().blendFrames = fr;
            saveSettings();
        });
        label(blendRow, "Blend", 0, holdBox);
        blendRow->addWidget(mBlend, 0, 1);
        blendRow->setColumnStretch(1, 1);
        hv->addLayout(blendRow);
        v->addWidget(controls::section("Hold", holdBox, this, kSections));

        // Handoffs: the prop leaving the hand and coming back, each with its option box.
        auto* offBox = new QWidget(this);
        auto* ov = new QVBoxLayout(offBox);
        ov->setContentsMargins(0, 0, 0, 0);
        ov->addWidget(controls::grid({controls::row("puppeteer.prop.plant", "arrow-bar-to-down", offBox), controls::row("puppeteer.prop.place", "table", offBox),
                                      controls::row("puppeteer.prop.throw", "ball-baseball", offBox), controls::row("puppeteer.prop.catch", "hand-move", offBox),
                                      controls::row("puppeteer.prop.take", "hand-finger", offBox), controls::row("puppeteer.prop.pass", "transfer", offBox)},
                                     2, offBox));
        v->addWidget(controls::section("Handoff", offBox, this, kSections));

        // Aim: the prop's own style (Settings); here only on, off and how fully.
        auto* aimBox = new QWidget(this);
        auto* av = new QVBoxLayout(aimBox);
        av->setContentsMargins(0, 0, 0, 0);
        av->setSpacing(4);
        av->addWidget(controls::grid({controls::row("puppeteer.prop.aim", "target-arrow", aimBox), controls::row("puppeteer.prop.aimOff", "target-off", aimBox)},
                                     2, aimBox));
        auto* aimForm = new QGridLayout;
        aimForm->setHorizontalSpacing(8);
        mWeight = new controls::SliderField(aimBox);
        mWeight->setRange(0.0, 1.0);
        mWeight->setDecimals(2);
        mWeight->setSingleStep(0.05);
        mWeight->setToolTip("Weight: how fully Aim turns the prop onto the target");
        connect(mWeight, &QDoubleSpinBox::valueChanged, this, [](double w) {
            settings().aimWeight = w;
            saveSettings();
        });
        label(aimForm, "Weight", 0, aimBox);
        aimForm->addWidget(mWeight, 0, 1);
        aimForm->setColumnStretch(1, 1);
        av->addLayout(aimForm);
        v->addWidget(controls::section("Aim", aimBox, this, kSections));

        // The track: who or what has the prop from which frame; a row jumps to its frame.
        auto* listBox = new QWidget(this);
        auto* lv = new QVBoxLayout(listBox);
        lv->setContentsMargins(0, 0, 0, 0);
        lv->setSpacing(4);
        mHolds = new QTreeWidget(listBox);
        mHolds->setColumnCount(2);
        mHolds->setHeaderHidden(true);
        mHolds->setRootIsDecorated(false);
        mHolds->header()->setStretchLastSection(true);
        mHolds->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        mHolds->setMinimumHeight(70);
        connect(mHolds, &QTreeWidget::itemClicked, this, [](QTreeWidgetItem* it) {
            // Going to the frame rebuilds the list, so the row is read first.
            const double time = it->data(0, Qt::UserRole).toDouble();
            const QString sel = it->data(1, Qt::UserRole).toString();
            runCommand("currentTime", {{"args", {time}}});
            if (!sel.isEmpty())
                runCommand("select", {{"args", {sel.toStdString()}}});
        });
        lv->addWidget(mHolds);
        mHoldsEmpty = new QLabel("Not held yet. Hold puts it in a hand at the current frame.", listBox);
        mHolds->setToolTip("Who or what has the prop from which frame. Click a row to go to its frame (a throw selects its arc to drag)");
        mHoldsEmpty->setWordWrap(true);
        mHoldsEmpty->setForegroundRole(QPalette::PlaceholderText);
        lv->addWidget(mHoldsEmpty);
        v->addWidget(controls::section("Track", listBox, this, kSections));

        // Solve, with the rarer actions in overflow.
        auto* solveBox = new QWidget(this);
        auto* sv = new QHBoxLayout(solveBox);
        sv->setContentsMargins(0, 0, 0, 0);
        sv->setSpacing(4);
        sv->addWidget(controls::push("puppeteer.prop.solve", solveBox));
        mStatus = new QLabel(solveBox);
        mStatus->setForegroundRole(QPalette::PlaceholderText);
        mStatus->setToolTip("Gap: the largest distance left between a hand and its grip, or an aim and its target, at this frame");
        sv->addWidget(mStatus, 1);
        sv->addWidget(controls::overflow({"puppeteer.prop.unreal"}, solveBox));
        v->addWidget(controls::section("Solve", solveBox, this, kSections));

        // Unreal: the socket the prop attaches by, and the optional hand IK bones.
        auto* ueBox = new QWidget(this);
        auto* uf = new QGridLayout(ueBox);
        uf->setContentsMargins(0, 0, 0, 0);
        uf->setHorizontalSpacing(8);
        uf->setVerticalSpacing(4);
        mBone = new QLineEdit(ueBox);
        mBone->setToolTip("Bone: the bone of the character's skeletal mesh the prop attaches to. Empty: the hand holding it (hand_r)");
        label(uf, "Bone", 0, ueBox);
        uf->addWidget(mBone, 0, 1);
        mSocket = new QLineEdit(ueBox);
        mSocket->setToolTip("Socket: the socket made on that bone, at the offset the prop is held at. Empty: weapon_r or weapon_l");
        label(uf, "Socket", 1, ueBox);
        uf->addWidget(mSocket, 1, 1);
        mIkBones = new QCheckBox("IK Bones", ueBox);
        mIkBones->setToolTip("IK Bones: also key ik_hand_gun, ik_hand_r and ik_hand_l to follow the hands, for projects whose locomotion reads them");
        uf->addWidget(mIkBones, 2, 1);
        auto* setUp = new QToolButton(ueBox);
        setUp->setText("Set Up");
        setUp->setToolTip("Set the prop up for Unreal with this bone and socket; the Unreal link makes the socket when it sends the prop");
        connect(setUp, &QToolButton::clicked, this, [this] {
            Graph& g = Graph::current();
            const std::optional<Prop> p = current(g);
            if (!p)
                return;
            json call = {{"args", {g.path(p->root)}}};
            if (!mBone->text().trimmed().isEmpty())
                call["bone"] = mBone->text().trimmed().toStdString();
            if (!mSocket->text().trimmed().isEmpty())
                call["socket"] = mSocket->text().trimmed().toStdString();
            if (mIkBones->isVisible() && mIkBones->isChecked())
                call["ikBones"] = true;
            runCommand("puppeteerPropUnreal", call);
            sync();
        });
        uf->addWidget(setUp, 3, 1, Qt::AlignLeft);
        uf->setColumnStretch(1, 1);
        mUnreal = controls::section("Unreal", ueBox, this, kSections, false);
        v->addWidget(mUnreal);
        v->addStretch();

        mPropOnly = {setBox, ptBox};
        for (events::Event e : {events::Event::UndoStackChanged, events::Event::SelectionChanged, events::Event::TimeChanged, events::Event::SceneOpened,
                                events::Event::SceneNew})
            mSubs.push_back(events::subscribe(e, [p = QPointer<PropsPanel>(this)](const events::EventData&) {
                if (p)
                    p->sync();
            }, "Puppeteer"));
        sync();
    }

    ~PropsPanel() override
    {
        for (events::SubscriptionId s : mSubs)
            events::unsubscribe(s);
    }

    void sync()
    {
        Graph& g = Graph::current();
        const PanelPrefs& s = settings();
        {
            const QSignalBlocker b1(mBlend), b2(mWeight);
            mBlend->setValue(s.blendFrames);
            mWeight->setValue(s.aimWeight);
        }
        const std::optional<Prop> p = current(g);
        mTitle->setVisible(bool(p));
        mPreset->setVisible(bool(p));
        mEmpty->setVisible(!p);
        for (QWidget* w : mPropOnly)
            w->setEnabled(bool(p));
        mHolds->clear();
        {
            const QSignalBlocker b(mPoints);
            mPoints->clear();
        }
        if (!p)
        {
            mHand->clearCurrent();
            mHands->clearCurrent();
            mAim->clearCurrent();
            mHoldsEmpty->setVisible(true);
            mHolds->setVisible(false);
            mStatus->clear();
            mUnreal->setVisible(false);
            return;
        }
        const Settings& st = p->settings;
        mTitle->setText(QString::fromStdString(g.node(p->root)->name()));
        const Preset* match = matchingPreset(st);
        mPreset->setText(match ? match->label : "Custom");
        mHand->setCurrent(p->rightHanded ? 0 : 1);
        mHands->setCurrent(st.twoHanded ? 2 : 1);
        mAim->setCurrent(int(st.aim));
        {
            const QSignalBlocker b1(mReach), b2(mDrop);
            mReach->setValue(st.reach);
            mDrop->setValue(st.drop);
        }
        // Reach and drop only mean something for the aims that use them.
        const bool reaches = st.aim == AimStyle::Extend || st.aim == AimStyle::Guard;
        for (QWidget* w : {static_cast<QWidget*>(mReach), static_cast<QWidget*>(mReachLabel)})
            w->setVisible(reaches);
        for (QWidget* w : {static_cast<QWidget*>(mDrop), static_cast<QWidget*>(mDropLabel)})
            w->setVisible(st.aim == AimStyle::Guard);
        mHolster->setCurrentIndex(mHolster->findData(int(st.holster)));
        for (const auto& [box, pose] : {std::pair{mMainPose, st.mainPose}, std::pair{mOffPose, st.offPose}})
        {
            int i = box->findData(QString::fromStdString(pose));
            if (i < 0)
            {
                box->addItem(QString::fromStdString(pose), QString::fromStdString(pose));
                i = box->count() - 1;
            }
            box->setCurrentIndex(i);
        }
        mTrigger->setChecked(st.trigger);
        mShape->setCurrentIndex(int(st.shape));

        {
            const QSignalBlocker b(mPoints);
            const std::vector<NodeId> sel = g.selection();
            for (NodeId pt : p->points)
            {
                const Node& n = *g.node(pt);
                std::string name = n.name();
                const std::string prefix = g.node(p->root)->name() + "_";
                if (name.starts_with(prefix))
                    name = name.substr(prefix.size());
                if (name.ends_with("_point"))
                    name.resize(name.size() - 6);
                auto* it = new QTreeWidgetItem(mPoints, {QString::fromStdString(name), roleLabel(roleOf(n))});
                it->setData(0, Qt::UserRole, QString::fromStdString(g.path(pt)));
                it->setForeground(1, palette().color(QPalette::PlaceholderText));
                it->setSelected(std::find(sel.begin(), sel.end(), pt) != sel.end());
            }
        }

        const std::vector<TrackRow> rows = trackRows(g, *p);
        const double fps = std::max(anim::playback().fps, 1e-6);
        for (const TrackRow& r : rows)
        {
            auto* it = new QTreeWidgetItem(mHolds, {QString::number(r.time * fps, 'f', 0), r.label});
            it->setTextAlignment(0, Qt::AlignRight | Qt::AlignVCenter);
            it->setData(0, Qt::UserRole, r.time);
            it->setData(1, Qt::UserRole, r.select);
            it->setToolTip(1, r.tip);
        }
        mHolds->setVisible(!rows.empty());
        mHoldsEmpty->setVisible(rows.empty());

        const NodeId ch = propCharacter(g, *p);
        QString status;
        if (const NodeId gr = ch ? propGrapple(g, ch, false) : kNoNode)
        {
            const std::string layers = textOf(*g.node(gr), sim::kRecordingsAttr);
            const bool solved = !layers.empty() && layers != "[]";
            if (auto f = sim::cached(g, gr, sim::frameOf(g.time())); solved && f && f->state.size() >= 3)
                status = QString("Gap %1 cm").arg(f->state[f->state.size() - 3], 0, 'f', 2);
            else
                status = solved ? "Solved" : "Not solved";
        }
        mStatus->setText(status);
        mUnreal->setVisible(true);
        mIkBones->setVisible(hasIkBones(g, ch));
        const Node& root = *g.node(p->root);
        if (!mBone->hasFocus())
            mBone->setText(QString::fromStdString(lila::scene::pins::socketBoneOf(root)));
        if (!mSocket->hasFocus())
            mSocket->setText(QString::fromStdString(lila::scene::pins::socketOf(root)));
    }

private:
    QLabel *mTitle = nullptr, *mPreset = nullptr, *mEmpty = nullptr, *mHoldsEmpty = nullptr, *mStatus = nullptr, *mReachLabel = nullptr,
           *mDropLabel = nullptr;
    std::vector<QToolButton*> mTiles;
    std::vector<QWidget*> mPropOnly;
    controls::Segmented *mHand = nullptr, *mHands = nullptr, *mAim = nullptr;
    QDoubleSpinBox *mBlend = nullptr, *mReach = nullptr, *mDrop = nullptr;
    controls::SliderField* mWeight = nullptr;
    QComboBox *mHolster = nullptr, *mMainPose = nullptr, *mOffPose = nullptr, *mShape = nullptr;
    QCheckBox* mTrigger = nullptr;
    QTreeWidget *mHolds = nullptr, *mPoints = nullptr;
    QLineEdit *mBone = nullptr, *mSocket = nullptr;
    QCheckBox* mIkBones = nullptr;
    QWidget* mUnreal = nullptr;
    std::vector<events::SubscriptionId> mSubs;
};

// Points in the viewport ----------------------------------------------------------------------------------------------

const glm::vec4 kGrip{0.92f, 0.78f, 0.22f, 1.0f};
const glm::vec4 kAim{0.85f, 0.35f, 0.3f, 1.0f};
const glm::vec4 kRest{0.75f, 0.75f, 0.75f, 0.9f};
const glm::vec4 kSelectedPoint{1.0f, 1.0f, 1.0f, 1.0f};

/** Points show while they, their prop, or (for a holster) the prop put away there is selected. */
bool pointsShown(Graph& g, NodeId point)
{
    for (NodeId id : g.selection())
    {
        if (id == point || g.parent(id) == point)
            return true;
        const NodeId pn = propFor(g, point), sel = propFor(g, id);
        if (pn && sel == pn)
            return true;
        if (Node* n = g.node(point); n && roleOf(*n) == Role::Holster && sel)
            if (const std::optional<Prop> p = load(g, sel); p && textOf(*g.node(sel), "holster") == g.path(point))
                return true;
    }
    return false;
}

struct Drawn
{
    QPointF at, axisEnd, palmEnd;
    Role role = Role::Main;
};

std::optional<Drawn> drawnPoint(const ViewInfo& view, const Node& n, const glm::dmat4& world)
{
    const Role r = roleOf(n);
    const glm::dvec3 o(world[3]);
    const glm::dvec3 x = glm::normalize(glm::dvec3(world[0])), z = glm::normalize(glm::dvec3(world[2]));
    const double axisLen = r == Role::Muzzle || r == Role::Tip ? 25.0 : 6.0;
    const auto a = view.project(o), b = view.project(o + x * axisLen), c = view.project(o + z * 3.0);
    if (!a || !b || !c)
        return std::nullopt;
    return Drawn{*a, *b, *c, r};
}

void drawPoint(drawOverrides::DrawContext& c)
{
    Graph& g = Graph::current();
    if (!pointsShown(g, c.node.id()))
        return;
    const std::optional<Drawn> d = drawnPoint(c.view, c.node, c.world);
    if (!d)
        return;
    // Only the point itself reads as selected: a selected prop would otherwise whiten every handle on it.
    const std::vector<NodeId> sel = g.selection();
    const bool selected = std::find(sel.begin(), sel.end(), c.node.id()) != sel.end();
    const bool aims = d->role == Role::Muzzle || d->role == Role::Tip;
    const glm::vec4 col = selected ? kSelectedPoint : aims ? kAim : isGrip(d->role) ? kGrip : kRest;
    c.painter.smoothLine(d->at, d->axisEnd, selected ? 2.5f : 2.0f, col);
    if (isGrip(d->role) || d->role == Role::Holster)
    {
        // The palm side, short, and a ring for the handle.
        c.painter.smoothLine(d->at, d->palmEnd, 1.5f, col);
        c.painter.arc(d->at, selected ? 6.0 : 5.0, 0.0, 360.0, 1.5f, col);
        if (d->role == Role::Main)
            c.painter.smoothDisc(d->at, 2.5, col);
    }
    else if (aims)
        c.painter.smoothDisc(d->axisEnd, selected ? 4.0 : 3.0, col);
    else
        c.painter.rect(QRectF(d->at - QPointF(3, 3), QSizeF(6, 6)), col);
}

std::optional<double> pickPoint(const drawOverrides::PickContext& c)
{
    Graph& g = Graph::current();
    if (!pointsShown(g, c.node.id()))
        return std::nullopt;
    const std::optional<Drawn> d = drawnPoint(c.view, c.node, c.world);
    if (!d)
        return std::nullopt;
    if (!c.click)
    {
        const QRectF box = QRectF(c.a, c.b).normalized();
        return box.contains(d->at) ? std::optional<double>(0.0) : std::nullopt;
    }
    const double dist = std::hypot(c.a.x() - d->at.x(), c.a.y() - d->at.y()) - 5.0;
    return dist <= drawOverrides::kClickPixels ? std::optional<double>(std::max(dist, 0.0)) : std::nullopt;
}

void addAction(ext::Context& ctx, const char* id, const char* label, std::function<void()> run, const char* doc, const char* icon,
               std::vector<std::string> extra = {}, std::function<bool()> enabled = hasProp)
{
    std::vector<std::string> kw = keywords();
    kw.insert(kw.end(), extra.begin(), extra.end());
    actions::add(ctx, {.id = id, .label = label, .category = kCat, .run = std::move(run), .enabled = std::move(enabled), .doc = doc, .icon = icon,
                       .keywords = kw});
}

/** A handoff action: its command run on the current prop at the current frame with the option box's values. */
void addHandoff(ext::Context& ctx, const char* id, const char* label, const char* command, std::function<bool(Graph&, const Prop&, json&)> fill,
                const char* doc, const char* icon, std::vector<std::string> extra)
{
    std::vector<std::string> kw = keywords();
    kw.insert(kw.end(), extra.begin(), extra.end());
    actions::add(ctx, {.id = id,
                       .label = label,
                       .category = kCat,
                       .run =
                           [command, fill] {
                               Graph& g = Graph::current();
                               const std::optional<Prop> p = current(g);
                               if (!p)
                                   return void(log::write(LogLevel::Warning, "Props: select a prop first"));
                               json call = {{"args", {g.path(p->root)}}, {"time", g.time()}};
                               if (fill && !fill(g, *p, call))
                                   return;
                               actions::runWithOptions(command, call);
                           },
                       .enabled = hasProp,
                       .doc = doc,
                       .command = command,
                       .icon = icon,
                       .keywords = kw});
}

/** The selected transforms that are neither part of the prop nor of a character. */
std::vector<NodeId> otherSelected(Graph& g, const Prop& p)
{
    std::vector<NodeId> out;
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && n->type().attrIndex("worldMatrix") >= 0 && propFor(g, id) != p.node && !characterFor(g, id) && !isUnder(g, id, p.root))
            out.push_back(id);
    return out;
}

/** The selected character that isn't the one holding the prop now, else the only other character. */
NodeId otherCharacter(Graph& g, const Prop& p)
{
    const NodeId holder = holderNow(g, p);
    const NodeId holding = holder ? characterFor(g, holder) : kNoNode;
    for (NodeId id : g.selection())
        if (const NodeId c = characterFor(g, id); c && c != holding)
            return c;
    std::vector<NodeId> others;
    for (NodeId c : characters(g))
        if (c != holding)
            others.push_back(c);
    return others.size() == 1 ? others.front() : kNoNode;
}

// Throw arcs in the viewport --------------------------------------------------------------------------------------------

/** An arc's path as keyed: seconds and the prop's middle there. */
std::vector<std::pair<double, glm::dvec3>> arcPath(const Node& arc)
{
    std::vector<std::pair<double, glm::dvec3>> out;
    const json j = json::parse(textOf(arc, "path").empty() ? std::string("[]") : textOf(arc, "path"), nullptr, false);
    if (j.is_array())
        for (const json& s : j)
            if (s.is_array() && s.size() == 4)
                out.push_back({s[0].get<double>(), {s[1].get<double>(), s[2].get<double>(), s[3].get<double>()}});
    return out;
}

/** Shown while the arc, its prop, or a point of it is selected. */
bool arcShown(Graph& g, NodeId arc)
{
    const NodeId pn = propFor(g, arc);
    for (NodeId id : g.selection())
        if (id == arc || (pn && propFor(g, id) == pn))
            return true;
    return false;
}

/** The path a drag would give: the arc again through a point at a time, keyed arcs only, before it is keyed. */
struct ArcPreview
{
    NodeId arc = kNoNode;
    std::vector<std::pair<double, glm::dvec3>> path;
    QString readout;
};
ArcPreview& arcPreview()
{
    static ArcPreview p;
    return p;
}

const glm::vec4 kArcPath{0.78f, 0.78f, 0.78f, 0.9f};
const glm::vec4 kArcHot{1.0f, 1.0f, 1.0f, 1.0f};

void drawArcPath(OverlayPainter& painter, const ViewInfo& view, const std::vector<std::pair<double, glm::dvec3>>& path, int hot, bool dashed)
{
    std::vector<QPointF> pts;
    for (const auto& [t, p] : path)
        if (const auto q = view.project(p))
            pts.push_back(*q);
    if (pts.size() < 2)
        return;
    if (dashed)
        painter.dashedPolyline(pts, 1.5f, 6.0f, kArcPath);
    else
        painter.polyline(pts, 1.5f, kArcPath);
    for (size_t i = 0; i < path.size(); ++i)
        if (const auto q = view.project(path[i].second))
            painter.smoothDisc(*q, int(i) == hot ? 4.5 : 2.5, int(i) == hot ? kArcHot : kArcPath);
    // Where it ends: landed, caught or out of time.
    if (const auto q = view.project(path.back().second))
        painter.arc(*q, 6.0, 0.0, 360.0, 1.5f, kArcPath);
}

void drawArc(drawOverrides::DrawContext& c)
{
    Graph& g = Graph::current();
    if (!arcShown(g, c.node.id()))
        return;
    const ArcPreview& pv = arcPreview();
    if (pv.arc == c.node.id() && !pv.path.empty())
    {
        drawArcPath(c.painter, c.view, arcPath(c.node), -1, true);
        drawArcPath(c.painter, c.view, pv.path, -1, false);
        return;
    }
    drawArcPath(c.painter, c.view, arcPath(c.node), -1, false);
}

std::optional<double> pickArc(const drawOverrides::PickContext& c)
{
    Graph& g = Graph::current();
    if (!arcShown(g, c.node.id()))
        return std::nullopt;
    double best = 1e300;
    for (const auto& [t, p] : arcPath(c.node))
        if (const auto q = c.view.project(p))
        {
            if (!c.click)
            {
                if (QRectF(c.a, c.b).normalized().contains(*q))
                    return 0.0;
                continue;
            }
            best = std::min(best, std::hypot(c.a.x() - q->x(), c.a.y() - q->y()));
        }
    return c.click && best <= drawOverrides::kClickPixels ? std::optional<double>(best) : std::nullopt;
}

/** Dragging a frame of the arc: the prop passes through the cursor there, the launch kept; one command on release. */
class ArcDrag final : public nodeHandles::Drag
{
public:
    ArcDrag(NodeId arc, int frame, const ViewInfo& view) : mArc(arc), mFrame(frame)
    {
        Graph& g = Graph::current();
        const Node& n = *g.node(arc);
        mPath = arcPath(n);
        mTime = mPath[size_t(frame)].first;
        mAt = mPath[size_t(frame)].second;
        mStart = n.as<double>(attrOf(n, "start"));
        mLaunch = n.as<glm::dvec3>(attrOf(n, "launch"));
        mGravity = n.as<double>(attrOf(n, "gravity"));
        mPhysics = n.as<bool>(attrOf(n, "physics"));
        mNormal = view.viewDir(mAt);
    }
    void move(QPointF px, const ViewInfo& view, Qt::KeyboardModifiers) override
    {
        glm::dvec3 o, d;
        view.ray(px, o, d);
        const double den = glm::dot(d, mNormal);
        if (std::abs(den) < 1e-9)
            return;
        mTo = o + d * (glm::dot(mAt - o, mNormal) / den);
        mMoved = true;
        // The keyed arc through the cursor, drawn until the command keys it.
        const double tau = std::max(mTime - mStart, 1e-3);
        const glm::dvec3 gv = -lila::math::axisBasis().up * mGravity;
        const glm::dvec3 v = (mTo - mLaunch - 0.5 * gv * tau * tau) / tau;
        ArcPreview& pv = arcPreview();
        pv.arc = mArc;
        pv.path.clear();
        for (const auto& [t, p] : mPath)
        {
            const double s = t - mStart;
            pv.path.push_back({t, mLaunch + v * s + 0.5 * gv * s * s});
        }
        pv.readout = QString("%1 cm/s%2").arg(glm::length(v), 0, 'f', 0).arg(mPhysics ? ", physics on release" : "");
        viewportTools::requestRedraw();
    }
    void release(bool moved) override
    {
        arcPreview() = {};
        if (!moved || !mMoved)
            return;
        Graph& g = Graph::current();
        runCommand("puppeteerPropArc", {{"args", {g.path(mArc)}}, {"through", {mTo.x, mTo.y, mTo.z}}, {"at", mTime}});
    }
    void cancel() override
    {
        arcPreview() = {};
        viewportTools::requestRedraw();
    }
    QString readout() const override { return arcPreview().readout; }

private:
    NodeId mArc;
    int mFrame;
    std::vector<std::pair<double, glm::dvec3>> mPath;
    double mTime = 0.0, mStart = 0.0, mGravity = 981.0;
    glm::dvec3 mAt{0.0}, mLaunch{0.0}, mNormal{0.0, 0.0, 1.0}, mTo{0.0};
    bool mPhysics = false, mMoved = false;
};

void drawArcHandles(OverlayPainter& p, const nodeHandles::Context& c, int hovered, int active)
{
    if (arcPreview().arc == c.node.id())
        return;
    drawArcPath(p, c.view, arcPath(c.node), active >= 0 ? active : hovered, false);
}

int pickArcHandle(QPointF px, const nodeHandles::Context& c)
{
    const auto path = arcPath(c.node);
    int best = -1;
    double bestD = 8.0;
    // The launch frame stays put: it is where the hand let go.
    for (size_t i = 1; i < path.size(); ++i)
        if (const auto q = c.view.project(path[i].second))
            if (const double d = std::hypot(px.x() - q->x(), px.y() - q->y()); d < bestD)
            {
                bestD = d;
                best = int(i);
            }
    return best;
}
} // namespace
} // namespace puppeteer::prop

namespace puppeteer
{
void registerPropUi(lila::ext::Context& ctx)
{
    using namespace prop;
    panels::add(ctx, {.id = kPanelId,
                      .title = "Props",
                      .create = [](QWidget* parent) -> QWidget* { return new PropsPanel(parent); },
                      .area = Qt::LeftDockWidgetArea,
                      .icon = "sword",
                      .open = false,
                      .keywords = keywords(),
                      .docs = "props"});
    drawOverrides::add(ctx, {.id = "puppeteer.propPoint", .nodeType = kPointType, .draw = drawPoint, .pick = pickPoint});
    drawOverrides::add(ctx, {.id = "puppeteer.propArc", .nodeType = kArcType, .draw = drawArc, .pick = pickArc});
    nodeHandles::add(ctx, {.id = "puppeteer.propArc",
                           .nodeType = kArcType,
                           .draw = drawArcHandles,
                           .pick = pickArcHandle,
                           .begin = [](int handle, QPointF, const nodeHandles::Context& c) -> std::unique_ptr<nodeHandles::Drag> {
                               return std::make_unique<ArcDrag>(c.node.id(), handle, c.view);
                           }});

    addHandoff(ctx, "puppeteer.prop.plant", "Plant", "puppeteerPropPlant", nullptr,
               "Stand the held prop on its tip or base on the ground or a surface ahead from the current frame, leaning; the hands carry "
               "it down and let go.",
               "arrow-bar-to-down", {"plant", "stand", "stick in ground", "lean", "rest"});
    addHandoff(ctx, "puppeteer.prop.throw", "Throw", "puppeteerPropThrow", nullptr,
               "Throw the held prop from the current frame: a keyed arc with spin (or physics), from the hand's motion or the option "
               "box's speed and angle. Select its row in Track to drag the arc.",
               "ball-baseball", {"throw", "toss", "launch", "lob", "physics", "arc", "trajectory"});
    addHandoff(ctx, "puppeteer.prop.catch", "Catch", "puppeteerPropCatch",
               [](Graph& g, const Prop& p, json& call) {
                   for (NodeId id : g.selection())
                       if (const NodeId c = characterFor(g, id))
                       {
                           call["character"] = g.path(c);
                           break;
                       }
                   (void)p;
                   return true;
               },
               "Catch the thrown prop where its arc meets a hand (the selected character's, else the thrower's); the hand reaches to take it.",
               "hand-move", {"catch", "grab", "receive"});
    addHandoff(ctx, "puppeteer.prop.place", "Put On", "puppeteerPropPlace",
               [](Graph& g, const Prop& p, json& call) {
                   const std::vector<NodeId> others = otherSelected(g, p);
                   if (others.empty())
                   {
                       log::write(LogLevel::Warning, "Props: select the object to put it on too (a table, a rack)");
                       return false;
                   }
                   call["on"] = g.path(others.front());
                   if (others.size() > 1)
                       call["hook"] = g.path(others.back());
                   return true;
               },
               "Put the held prop on the other selected object from the current frame (on its top, or hung from a second selected "
               "transform as a hook); it rides the object after.",
               "table", {"put down", "place", "set down", "table", "rack", "hook", "hang", "shelf"});
    addHandoff(ctx, "puppeteer.prop.take", "Take", "puppeteerPropHold",
               [](Graph&, const Prop&, json& call) {
                   call["reach"] = std::max(blendSeconds(), 0.1);
                   call["blend"] = 0.0;
                   return true;
               },
               "Take the prop where it is (on a table, the ground, a rack) at the current frame, the hand reaching for it over the "
               "panel's Blend before.",
               "hand-finger", {"pick up", "take", "grab", "lift"});
    addHandoff(ctx, "puppeteer.prop.pass", "Pass", "puppeteerPropPass",
               [](Graph& g, const Prop& p, json& call) {
                   const NodeId to = otherCharacter(g, p);
                   if (!to)
                   {
                       log::write(LogLevel::Warning, "Props: select the character to pass it to");
                       return false;
                   }
                   call["to"] = g.path(to);
                   return true;
               },
               "Hand the held prop to the selected (or only other) character from the current frame, both hands sharing it.",
               "transfer", {"pass", "hand over", "give", "handoff", "exchange"});

    for (const Preset& p : presets())
        actions::add(ctx, {.id = std::string("puppeteer.prop.preset.") + p.id,
                           .label = p.label,
                           .category = kCat,
                           .run = [pp = &p] { setupAction(pp); },
                           .doc = std::string("Fill the selected prop's settings from the ") + p.label + " preset and fit its points again (a "
                                  "selected mesh becomes a prop first).",
                           .checked =
                               [pp = &p] {
                                   const std::optional<Prop> cur = current(Graph::current());
                                   return cur && matchingPreset(cur->settings) == pp;
                               },
                           .icon = p.icon,
                           .keywords = {"prop", "weapon", "preset", p.id, hand::lowerOf(p.label)}});

    addAction(ctx, "puppeteer.prop.make", "Make Prop", [] { setupAction(nullptr); },
              "Make the selected mesh or group a prop: its grips, tip and other points fitted to its shape, its settings read from it.", "sword",
              {"make prop", "mark as prop", "weapon setup", "object", "attach"}, [] { return actions::hasSelection(); });
    addAction(ctx, "puppeteer.prop.removePoint", "Remove Point",
              [] {
                  Graph& g = Graph::current();
                  for (NodeId pt : selectedPoints(g))
                      runCommand("puppeteerPropPoint", {{"args", {g.path(pt)}}, {"remove", true}});
              },
              "Remove the selected points from their prop.", "trash", {"delete point", "grip point"},
              [] { return !selectedPoints(Graph::current()).empty(); });
    addAction(ctx, "puppeteer.prop.hold", "Hold", [] { holdAction("main"); },
              "Put the prop in the character's main hand from the current frame (both hands when it is set to), settling over the panel's Blend.",
              "hand-grab", {"pick up", "grab", "draw", "unholster"});
    addAction(ctx, "puppeteer.prop.handOff", "Other Hand", handOffAction,
              "Hand the prop to the character's other hand from the current frame, without a pop.", "arrows-exchange", {"handoff", "hand off", "swap hands", "switch hands"});
    addAction(ctx, "puppeteer.prop.holster", "Holster", [] { simpleAction("puppeteerPropHolster"); },
              "Put the prop away on the character (at its holster spot) from the current frame; Hold draws it again.", "backpack",
              {"sheathe", "put away", "stow", "back", "hip"});
    addAction(ctx, "puppeteer.prop.drop", "Drop", [] { simpleAction("puppeteerPropDrop"); },
              "Let go of the prop at the current frame: it stays in the world where it is and the hands open.", "hand-stop",
              {"release", "let go", "detach"});
    addAction(ctx, "puppeteer.prop.aim", "Aim", aimAction,
              "Aim the held prop at the other selected object, or at a new target placed in front of the character, from the current frame; "
              "the arms and body follow.",
              "target-arrow", {"point at", "look at", "shoulder", "aim at"});
    addAction(ctx, "puppeteer.prop.aimOff", "Aim Off", aimOffAction, "Blend the prop's aim out from the current frame.", "target-off", {"stop aiming"});
    addAction(ctx, "puppeteer.prop.solve", "Solve Props",
              [] {
                  Graph& g = Graph::current();
                  if (const std::optional<Prop> p = current(g))
                      runCommand("puppeteerPropSolve", {{"args", {g.path(p->root)}}});
              },
              "Solve the arms and body holding props over the playback range: off hands on their grips, aims on their targets.", "player-record",
              {"bake", "solve", "two hands"});
    addAction(ctx, "puppeteer.prop.fit", "Fit Points",
              [] {
                  Graph& g = Graph::current();
                  if (const std::optional<Prop> p = current(g))
                      runCommand("puppeteerPropFit", {{"args", {g.path(p->root)}}});
              },
              "Fit the prop's points to its shape again; points added or moved by hand stay.", "focus-centered", {"refit", "auto fit", "grip points"});
    addAction(ctx, "puppeteer.prop.addGrip", "Add Grip",
              [] {
                  Graph& g = Graph::current();
                  if (const std::optional<Prop> p = current(g))
                  {
                      const json made = runCommand("puppeteerPropPoint", {{"args", {g.path(p->root)}}, {"role", "alt"}, {"name", "grip"}});
                      if (made.is_object())
                          if (Node* n = named(g, made.value("point", std::string())))
                              g.select({n->id()});
                  }
              },
              "Add another grip to the prop, starting on the main grip, to move where a hand should take it.", "plus", {"alternate grip", "grip point"});
    actions::add(ctx, {.id = "puppeteer.prop.unreal",
                       .label = "Set Up for Unreal",
                       .category = kCat,
                       .enabled = hasProp,
                       .doc = "Attach the prop to the holding bone (hand_r) through a socket at the offset it is held at, for the Unreal link; "
                              "optionally key the hand IK bones.",
                       .command = "puppeteerPropUnreal",
                       .icon = "package-export",
                       .keywords = [] {
                           std::vector<std::string> kw = keywords();
                           for (const char* k : {"unreal", "socket", "ik_hand_gun", "export", "ue5"})
                               kw.push_back(k);
                           return kw;
                       }()});
    actions::setEnabledWhen("puppeteer.prop.unreal", hasProp);

    menus::addItem(ctx, {.id = "puppeteer.props.menu", .menu = "Puppeteer", .kind = menus::Kind::Submenu, .label = "Props"});
    menus::addItem(ctx, {.id = "anim.props.menu", .menu = "Animation", .kind = menus::Kind::Submenu, .label = "Props"});
    int sep = 0;
    for (const std::string m : {"Puppeteer/Props", "Animation/Props"})
    {
        for (const char* a : {"window.puppeteerProps", "puppeteer.prop.make", "", "puppeteer.prop.hold", "puppeteer.prop.handOff", "puppeteer.prop.holster",
                              "puppeteer.prop.drop", "", "puppeteer.prop.plant", "puppeteer.prop.place", "puppeteer.prop.throw", "puppeteer.prop.catch",
                              "puppeteer.prop.take", "puppeteer.prop.pass", "", "puppeteer.prop.aim", "puppeteer.prop.aimOff", "", "puppeteer.prop.solve", "puppeteer.prop.fit",
                              "puppeteer.prop.addGrip", "puppeteer.prop.removePoint", "puppeteer.prop.unreal"})
            if (*a)
                menus::addItem(ctx, {.id = "puppeteer.props." + m + "." + a, .menu = m, .action = a});
            else
                menus::addItem(ctx, {.id = "puppeteer.props.sep" + std::to_string(sep++), .menu = m, .kind = menus::Kind::Separator});
        menus::addItem(ctx, {.id = "puppeteer.props." + m + ".presets", .menu = m, .kind = menus::Kind::Submenu, .label = "Preset"});
        for (const Preset& p : presets())
            menus::addItem(ctx, {.id = "puppeteer.props." + m + ".preset." + p.id, .menu = m + "/Preset", .action = std::string("puppeteer.prop.preset.") + p.id});
    }

    const events::SubscriptionId sub =
        events::subscribe(events::Event::SelectionChanged, [](const events::EventData&) { viewportTools::requestRedraw(); }, "Puppeteer");
    ctx.track("puppeteer:propsSelection", [sub] { events::unsubscribe(sub); });
}
} // namespace puppeteer
