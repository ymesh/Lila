#include "ui/CurveUi.h"

#include "brush/BrushUi.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "groom/GroomBrush.h"
#include "groom/GroomNodes.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/ViewInfo.h"
#include "ui/ViewportTool.h"

#include "groom/Groom.h"
#include "ui/Controls.h"
#include "ui/Icons.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include <map>
#include <set>

namespace lila::ui
{
namespace
{
using scene::Graph;
using scene::Node;
using scene::NodeId;

constexpr const char* kBrushSettingsPref = "/groomBrush/settings";
constexpr const char* kToolId = "groom.brushes";

struct GroomBrushState
{
    groom::GroomBrushSettings settings;
    /** Each tool's strength as last left, so a pick brings back its own. */
    std::map<std::string, double> strengths;
};

GroomBrushState& state()
{
    static GroomBrushState st = [] {
        GroomBrushState out;
        groom::GroomBrushSettings& s = out.settings;
        try
        {
            const std::string text = PrefStore::get().value<std::string>("general", kBrushSettingsPref, std::string());
            if (!text.empty())
            {
                const nlohmann::json j = nlohmann::json::parse(text);
                s.mode = j.value("mode", s.mode);
                s.radius = std::clamp(j.value("radius", s.radius), 2.0, 2000.0);
                s.falloff = std::clamp(j.value("falloff", s.falloff), 0.0, 1.0);
                s.pressure = j.value("pressure", s.pressure);
                s.collide = j.value("collide", s.collide);
                s.frontOnly = j.value("frontOnly", s.frontOnly);
                s.stretch = j.value("stretch", s.stretch);
                s.tubeChannel = j.value("tubeChannel", s.tubeChannel);
                s.targets = j.value("targets", s.targets);
                s.placeLength = std::max(0.01, j.value("placeLength", s.placeLength));
                s.placeSegments = std::clamp(j.value("placeSegments", s.placeSegments), 1, 256);
                s.placeSpacing = std::max(0.0, j.value("placeSpacing", s.placeSpacing));
                s.matchNeighbors = j.value("matchNeighbors", s.matchNeighbors);
                s.attribute = j.value("attribute", s.attribute);
                s.value = j.value("value", s.value);
                s.paintKind = j.value("paintKind", s.paintKind);
                s.smoothStrength = std::max(0.0, j.value("smoothStrength", s.smoothStrength));
                s.partName = j.value("partName", s.partName);
                s.flowSink = j.value("flowSink", s.flowSink);
                if (j.contains("strengths") && j["strengths"].is_object())
                    for (auto& [k, v] : j["strengths"].items())
                        out.strengths[k] = v.get<double>();
            }
        }
        catch (const std::exception&)
        {
        }
        if (!groom::groomBrushEntry(s.mode))
            s.mode = "comb";
        const groom::GroomBrushEntry* e = groom::groomBrushEntry(s.mode);
        const auto it = out.strengths.find(s.mode);
        s.strength = it != out.strengths.end() ? it->second : e->defaultStrength;
        return out;
    }();
    return st;
}

void save()
{
    GroomBrushState& st = state();
    const groom::GroomBrushSettings& s = st.settings;
    st.strengths[s.mode] = s.strength;
    nlohmann::json j = {{"mode", s.mode},
                        {"radius", s.radius},
                        {"falloff", s.falloff},
                        {"pressure", s.pressure},
                        {"collide", s.collide},
                        {"frontOnly", s.frontOnly},
                        {"stretch", s.stretch},
                        {"tubeChannel", s.tubeChannel},
                        {"targets", s.targets},
                        {"placeLength", s.placeLength},
                        {"placeSegments", s.placeSegments},
                        {"placeSpacing", s.placeSpacing},
                        {"matchNeighbors", s.matchNeighbors},
                        {"attribute", s.attribute},
                        {"value", s.value},
                        {"paintKind", s.paintKind},
                        {"smoothStrength", s.smoothStrength},
                        {"partName", s.partName},
                        {"flowSink", s.flowSink},
                        {"strengths", st.strengths}};
    PrefStore::get().set("general", kBrushSettingsPref, j.dump());
    syncGroomPalette();
    viewportTools::requestRedraw();
}

/** The groom of the selection, or of the only groom in the scene. */
Node* selectedGroomNode(Graph& g)
{
    for (NodeId id : g.selection())
        if (Node* n = g.node(id))
            if (Node* gr = groom::findGroom(g, *n))
                return gr;
    Node* only = nullptr;
    int count = 0;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA("groom"))
        {
            only = n;
            ++count;
        }
    return count == 1 ? only : nullptr;
}

QString label(const std::string& mode)
{
    const groom::GroomBrushEntry* e = groom::groomBrushEntry(mode);
    return e ? QString::fromStdString(e->label) : QString::fromStdString(mode);
}

groom::GroomBrushView brushView(const ViewInfo& view)
{
    groom::GroomBrushView v;
    v.project = [view](const glm::dvec3& p) -> std::optional<glm::dvec2> {
        if (const std::optional<QPointF> q = view.project(p))
            return glm::dvec2(q->x(), q->y());
        return std::nullopt;
    };
    v.ray = [view](glm::dvec2 px, glm::dvec3& o, glm::dvec3& d) { view.ray(QPointF(px.x, px.y), o, d); };
    v.eye = view.eye;
    v.viewDir = -glm::normalize(glm::dvec3(view.view[0][2], view.view[1][2], view.view[2][2]));
    v.ortho = view.ortho;
    v.height = view.height;
    return v;
}

/**
 * Groom Brushes: GroomLab's tools on the selected groom. Place, Draw, Part and Flow create; Comb, Slide, Smooth, Lift,
 * Swing, Attract and Tube shape; Cut, Length, Length Keys and Erase trim; Paint and Freeze paint. Shift+drag smooths and
 * Ctrl works the other way. B+drag or Ctrl+scroll sets the radius, M+drag or Shift+scroll the strength. A stroke shows
 * live on the groom and lands as one undo step.
 */
class GroomBrushTool;
groom::GroomBrush* gActiveBrush = nullptr;

class GroomBrushTool final : public ViewportTool
{
public:
    const char* name() const override { return "Groom Brushes"; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::BlankCursor); }
    bool ownsBrushRadius() const override { return true; }

    void activate() override
    {
        Node* gr = groomForSelection();
        mBrush.setGroom(gr ? gr->id() : scene::kNoNode);
        gActiveBrush = &mBrush;
        mControls.activate({}, {});
        showGroomPalette(GroomPalettePage::Groom);
    }

    void deactivate() override
    {
        if (gActiveBrush == &mBrush)
            gActiveBrush = nullptr;
        if (mBrush.lengthKeysEdited())
            mBrush.cancelLengthKeys();
        mBrush.cancel();
        mControls.deactivate();
        viewportTools::setHoverHighlight(scene::kNoNode);
        hideGroomPalette();
        viewportTools::requestRedraw();
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        pullMirror();
        if (mControls.mousePress(e))
            return true;
        if (e->button() != Qt::LeftButton)
            return false;
        if (mBrush.groom() == scene::kNoNode)
            mBrush.setGroom(groomId());
        if (mBrush.groom() == scene::kNoNode)
        {
            // No groom yet: the click picks the mesh it grows from.
            if (Node* gr = startGroomOn(groomScalpUnder(view, e->position())))
                mBrush.setGroom(gr->id());
            viewportTools::requestRedraw();
            return true;
        }
        mBrush.setSettings(state().settings);
        unsigned mods = 0;
        if (e->modifiers() & Qt::ControlModifier)
            mods |= groom::kGroomBrushCtrl;
        if (e->modifiers() & Qt::ShiftModifier)
            mods |= groom::kGroomBrushShift;
        mInvert = (mods & groom::kGroomBrushCtrl) != 0;
        mPressed = mBrush.press(brushView(view), e->position().x(), e->position().y(), pressureOf(e), mods);
        report();
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mControls.mouseMove(e))
            return true;
        if (mBrush.groom() == scene::kNoNode)
            groomScalpUnder(view, e->position());
        if (mPressed)
        {
            mBrush.drag(brushView(view), e->position().x(), e->position().y(), pressureOf(e));
            report();
            viewportTools::requestRedraw();
            return true;
        }
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override
    {
        if (mControls.mouseRelease(e))
            return true;
        if (e->button() != Qt::LeftButton)
            return false;
        if (mPressed)
        {
            mPressed = false;
            mBrush.release(brushView(view));
            report();
            if (state().settings.mode == "lengths")
                syncGroomPalette();
            viewportTools::requestRedraw();
        }
        return true;
    }

    bool wheel(QWheelEvent* e, const ViewInfo&) override
    {
        const double steps = e->angleDelta().y() / 120.0;
        groom::GroomBrushSettings& s = state().settings;
        if (e->modifiers() & Qt::ControlModifier)
        {
            s.radius = std::clamp(s.radius * std::pow(1.1, steps), 2.0, 2000.0);
            save();
            return true;
        }
        if (e->modifiers() & Qt::ShiftModifier)
        {
            const groom::GroomBrushEntry* en = groom::groomBrushEntry(s.mode);
            const double lo = en && en->signedStrength ? -1.0 : 0.0;
            s.strength = std::clamp(s.strength + 0.05 * steps, lo, 1.0);
            save();
            return true;
        }
        return false;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        pullMirror();
        if (mControls.keyPress(e))
            return true;
        if (state().settings.mode == "lengths")
        {
            if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
            {
                mBrush.applyLengthKeys();
                report();
                syncGroomPalette();
                return true;
            }
            if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace)
            {
                mBrush.deleteLengthKey();
                syncGroomPalette();
                viewportTools::requestRedraw();
                return true;
            }
            if (e->key() == Qt::Key_Escape && mBrush.lengthKeysEdited())
            {
                mBrush.cancelLengthKeys();
                syncGroomPalette();
                viewportTools::requestRedraw();
                return true;
            }
        }
        if (e->key() == Qt::Key_Escape)
        {
            if (mPressed)
            {
                mBrush.cancel();
                mPressed = false;
                viewportTools::requestRedraw();
                return true;
            }
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        }
        return false;
    }

    void draw(render::FrameState& f, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();
        const groom::GroomBrushSettings& s = state().settings;
        Node* gr = Graph::current().node(mBrush.groom());
        if (gr && s.mode != "lengths" && !mPressed && mControls.adjusting() == brush::ui::BrushControls::Adjust::None)
        {
            mBrush.setSettings(s);
            const glm::vec4 hot(0.95f, 0.75f, 0.35f, 1.0f);
            for (const auto& line : mBrush.affectedLines(brushView(view), mHover.x(), mHover.y()))
                for (size_t i = 1; i < line.size(); ++i)
                {
                    f.lines.push_back({glm::dvec3(line[i - 1]), hot});
                    f.lines.push_back({glm::dvec3(line[i]), hot});
                }
        }
        if (gr && s.mode == "lengths")
            for (const groom::GroomBrush::ColoredLine& line : mBrush.lengthKeyLines(brushView(view), mHover.x(), mHover.y()))
                for (size_t i = 1; i < line.points.size(); ++i)
                {
                    f.lines.push_back({glm::dvec3(line.points[i - 1]), glm::vec4(line.color, 1.0f)});
                    f.lines.push_back({glm::dvec3(line.points[i]), glm::vec4(line.color, 1.0f)});
                }
        const std::vector<glm::dvec3> path = mBrush.drawnPath();
        const glm::vec4 drawn(0.4f, 0.8f, 1.0f, 1.0f);
        for (size_t i = 1; i < path.size(); ++i)
        {
            f.lines.push_back({glm::dvec3(path[i - 1]), drawn});
            f.lines.push_back({glm::dvec3(path[i]), drawn});
        }
        const Qt::KeyboardModifiers mods = QApplication::keyboardModifiers();
        const bool invert = mPressed ? mInvert : (mods & Qt::ControlModifier) != 0;
        const QString mode = !mPressed && (mods & Qt::ShiftModifier) ? QString("Smooth") : label(s.mode);
        const int axis = gr ? int(std::get<int64_t>(gr->value(gr->type().attrIndex("symmetry")))) - 1 : -1;
        drawToolStatus(p, view,
                       {.tool = "Groom Brushes",
                        .subject = gr ? QString::fromStdString(gr->name()) : QString(),
                        .mode = mode,
                        .fields = {{"radius", QString("%1 px").arg(int(std::lround(s.radius)))}, {"strength", QString::number(s.strength, 'f', 2)}},
                        .flags = {{"invert", invert}},
                        .prompt = gr ? QString() : QString("click the mesh hair grows from"),
                        .symmetry = axis >= 0 ? std::optional<ToolSymmetry>(ToolSymmetry{true, QString("XYZ").mid(axis, 1)}) : std::nullopt});
        if (groomKeysShown())
            drawKeyList(p, QPointF(16.0, view.height - 84.0), groomBrushKeys());
        pullMirror();
        brush::ui::drawCursor(p, view, mMirror, mControls.anchor(mHover), std::nullopt,
                              {.strengthRing = mControls.adjusting() == brush::ui::BrushControls::Adjust::Strength});
    }

private:
    static NodeId groomId()
    {
        Node* gr = selectedGroomNode(Graph::current());
        return gr ? gr->id() : scene::kNoNode;
    }

    double pressureOf(const QMouseEvent* e) const { return state().settings.pressure ? mControls.input(e).pressure : 1.0; }

    /** The ring and the B and M drags work on a brush::BrushSettings mirroring the radius and the strength's size. */
    void pullMirror()
    {
        const groom::GroomBrushSettings& s = state().settings;
        mMirror.radiusUnit = brush::RadiusUnit::Screen;
        mMirror.radius = s.radius;
        mMirror.strength = std::abs(s.strength);
    }

    void pushMirror()
    {
        groom::GroomBrushSettings& s = state().settings;
        s.radius = std::clamp(mMirror.radius, 2.0, 2000.0);
        s.strength = std::copysign(std::clamp(mMirror.strength, 0.0, 1.0), s.strength < 0.0 ? -1.0 : 1.0);
        save();
    }

    void report()
    {
        for (const std::string& w : mBrush.takeWarnings())
            if (mReported.insert(w).second)
                log::warn("Groom Brushes: {}", w);
    }

    groom::GroomBrush mBrush;
    brush::BrushSettings mMirror;
    brush::ui::BrushControls mControls{[this]() -> brush::BrushSettings& { return mMirror; }, [this] { pushMirror(); }, {}};
    QPointF mHover;
    bool mPressed = false;
    bool mInvert = false;
    std::set<std::string> mReported;
};

/** The lock brushes Sculpt Curves keeps on a groom's guides: what GroomLab's tools have no counterpart for. */
struct LockBrushTile
{
    const char* brush;
    const char* icon;
    const char* label;
};
constexpr LockBrushTile kLockBrushes[] = {{"grab", "hand-grab", "Grab"},     {"density", "grid-dots", "Density"}, {"curl", "curly-loop", "Curl"},
                                          {"frizz", "sparkles", "Frizz"},    {"clump", "circles", "Clump"},      {"split", "arrows-split", "Split"},
                                          {"merge", "arrows-join", "Merge"}, {"settle", "arrow-bar-to-down", "Settle"}};

QString caption(const char* id)
{
    return QString::fromStdString(groom::groomBrushSettingText(id).first);
}

/** A catalog hint's first sentence without its asides: the tile says what the tool does, the docs the rest. */
QString firstSentence(const QString& hint)
{
    qsizetype end = hint.size();
    for (const QString& stop : {QString(". "), QString(" ("), QString("; ")})
        if (const qsizetype at = hint.indexOf(stop); at >= 0)
            end = std::min(end, at);
    QString out = hint.left(end).trimmed();
    if (out.endsWith('.'))
        out.chop(1);
    return out;
}

QString brushTip(const char* id, const QString& fallback)
{
    const std::string t = groom::groomBrushSettingText(id).second;
    return t.empty() ? fallback : QString("%1: %2").arg(caption(id), QString::fromStdString(t));
}

/**
 * The Groom palette's page for Groom Brushes: GroomLab's tools by category, the lock brushes Sculpt Curves adds, and the
 * picked tool's settings only.
 */
class GroomBrushPage final : public QWidget
{
public:
    explicit GroomBrushPage(QWidget* parent) : QWidget(parent)
    {
        constexpr const char* kSections = "groomBrushPage";
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(4);
        auto tile = [this](const QString& iconName, const QString& text, const QString& hint, std::function<void()> fn) {
            auto* b = new QToolButton;
            b->setIcon(icon(iconName));
            b->setIconSize(QSize(24, 24));
            b->setText(text);
            b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            b->setCheckable(true);
            b->setFocusPolicy(Qt::NoFocus);
            controls::setRole(b, "tile");
            const QString first = firstSentence(hint);
            b->setToolTip(first.isEmpty() ? text : QString("%1: %2").arg(text, first));
            connect(b, &QToolButton::clicked, this, std::move(fn));
            return b;
        };
        // Select is the host's own and dropped from the catalog, which leaves Guides one tile: it joins Paint.
        std::map<std::string, std::vector<QWidget*>> sections;
        for (const groom::GroomBrushEntry& e : groom::groomBrushCatalog())
        {
            const std::string id = e.id;
            QToolButton* b = tile(QString::fromStdString(e.icon), QString::fromStdString(e.label), QString::fromStdString(e.hint), [id] { setGroomBrushMode(id); });
            mTools.push_back({id, b});
            sections[e.category == "Guides" ? "Paint" : e.category].push_back(b);
        }
        for (const std::string& c : groom::groomBrushCategories())
            if (auto it = sections.find(c); it != sections.end())
                v->addWidget(controls::section(QString::fromStdString(c), controls::grid(it->second, 4, this), this, kSections));
        std::vector<QWidget*> locks;
        for (const LockBrushTile& t : kLockBrushes)
        {
            groom::Brush brush;
            groom::parseBrush(t.brush, &brush);
            const int index = int(brush);
            QToolButton* b = tile(t.icon, t.label, {}, [index] {
                setCurveSculptBrush(index);
                viewportTools::activate("curve.sculptCurves");
            });
            mLocks.push_back({index, b});
            locks.push_back(b);
        }
        v->addWidget(controls::section("Locks", controls::grid(locks, 4, this), this, kSections));

        auto* fields = new QWidget(this);
        mForm = new QFormLayout(fields);
        mForm->setContentsMargins(0, 0, 0, 0);
        mForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        mForm->setVerticalSpacing(4);
        using Set = groom::GroomBrushSettings;
        auto number = [&](const char* id, const QString& label, double lo, double hi, int decimals, const QString& unit, const QString& fallbackTip,
                          std::function<void(Set&, double)> set) {
            QDoubleSpinBox* box = controls::valueField(lo, hi, decimals, unit, fields);
            box->setToolTip(brushTip(id, QString("%1: %2").arg(label, fallbackTip)));
            connect(box, &QDoubleSpinBox::valueChanged, this, [set](double x) {
                Set s = groomBrushSettings();
                set(s, x);
                setGroomBrushSettings(s);
            });
            mForm->addRow(label, box);
            mRows.push_back({id, box});
            return box;
        };
        auto check = [&](const char* id, const QString& label, const QString& fallbackTip, std::function<void(Set&, bool)> set) {
            auto* box = new QCheckBox(label, fields);
            box->setFocusPolicy(Qt::NoFocus);
            box->setToolTip(brushTip(id, QString("%1: %2").arg(label, fallbackTip)));
            connect(box, &QCheckBox::toggled, this, [set](bool on) {
                Set s = groomBrushSettings();
                set(s, on);
                setGroomBrushSettings(s);
            });
            mForm->addRow(QString(), box);
            mRows.push_back({id, box});
            return box;
        };
        auto segmented = [&](const char* id, const QString& label, const std::vector<std::pair<QString, std::string>>& choices,
                             std::function<void(Set&, const std::string&)> set) {
            auto* seg = new controls::Segmented(fields);
            for (size_t i = 0; i < choices.size(); ++i)
                seg->add(choices[i].first, {}, choices[i].first, int(i));
            seg->setToolTip(brushTip(id, label));
            connect(seg, &controls::Segmented::picked, this, [choices, set](int i) {
                Set s = groomBrushSettings();
                set(s, choices[size_t(i)].second);
                setGroomBrushSettings(s);
            });
            mForm->addRow(label, seg);
            mRows.push_back({id, seg});
            return seg;
        };
        mRadius = number("radius", "Radius", 2.0, 2000.0, 0, "px", "the brush circle on screen  (B+drag, [ ], Ctrl+scroll)", [](Set& s, double x) { s.radius = x; });
        mStrength = number("strength", "Strength", -1.0, 1.0, 2, {}, "how much each dab does  (M+drag, Shift+scroll)", [](Set& s, double x) { s.strength = x; });
        mFalloff = number("falloff", "Falloff", 0.0, 1.0, 2, {}, "0 full strength to the edge, 1 fading from the centre", [](Set& s, double x) { s.falloff = x; });
        mSmooth = number("smoothStrength", "Smoothing", 0.0, 4.0, 2, {}, "how much Shift+drag smooths a map per dab", [](Set& s, double x) { s.smoothStrength = x; });
        mTube = segmented("tubeChannel", "Paint", {{"Width", "width"}, {"Flatness", "flatness"}, {"Twist", "twist"}}, [](Set& s, const std::string& c) { s.tubeChannel = c; });
        mPaintOn = segmented("map", "Paint On", {{"Guides", "guides"}, {"Scalp", "vertices"}, {"Texture", "texture"}},
                             [](Set& s, const std::string& c) { s.paintKind = c; });
        mAttribute = new QComboBox(fields);
        mAttribute->setEditable(true);
        mAttribute->addItems({"clump", "density", "length", "width", "curl", "frizz"});
        mAttribute->setToolTip("Map: the attribute or scalp map painted; type a name for a new one.");
        connect(mAttribute, &QComboBox::currentTextChanged, this, [](const QString& t) {
            Set s = groomBrushSettings();
            s.attribute = t.trimmed().toStdString();
            setGroomBrushSettings(s);
        });
        mForm->addRow("Map", mAttribute);
        mRows.push_back({"map", mAttribute});
        mValue = number("map", "Value", -1000.0, 1000.0, 2, {}, "what the map is painted toward; Ctrl paints toward 0", [](Set& s, double x) { s.value = x; });
        mLength = number("newGuides", "Length", 0.01, 1000.0, 2, "cm", "new guides' length where none are near to match", [](Set& s, double x) { s.placeLength = x; });
        mSegments = number("newGuides", "Segments", 1.0, 256.0, 0, {}, "points along new guides", [](Set& s, double x) { s.placeSegments = int(std::lround(x)); });
        mSpacing = number("spacing", "Spacing", 0.0, 100.0, 2, "cm", "no new root closer than this", [](Set& s, double x) { s.placeSpacing = x; });
        mMatch = check("newGuides", "Match Neighbors", "new guides take the shape of the nearest ones", [](Set& s, bool on) { s.matchNeighbors = on; });
        mPartName = new QLineEdit(fields);
        mPartName->setToolTip("Name: the new part's name, made unique.");
        connect(mPartName, &QLineEdit::editingFinished, this, [this] {
            Set s = groomBrushSettings();
            s.partName = mPartName->text().trimmed().toStdString();
            if (s.partName.empty())
                s.partName = "part";
            setGroomBrushSettings(s);
        });
        mForm->addRow("Name", mPartName);
        mRows.push_back({"parts", mPartName});
        mSink = segmented("flow", "Place", {{"Arrows", ""}, {"Whorl", "whorl"}, {"Whorl CW", "whorlcw"}, {"Converge", "converge"}, {"Part", "part"}},
                          [](Set& s, const std::string& c) { s.flowSink = c; });
        mTargets = new QWidget(fields);
        auto* tr = new QHBoxLayout(mTargets);
        tr->setContentsMargins(0, 0, 0, 0);
        tr->setSpacing(8);
        for (const char* t : {"guides", "parts", "flow"})
        {
            auto* box = new QCheckBox(QString(t).left(1).toUpper() + QString(t).mid(1), mTargets);
            box->setFocusPolicy(Qt::NoFocus);
            connect(box, &QCheckBox::toggled, this, [this](bool) {
                std::string list;
                for (const auto& [n, b] : mTargetBoxes)
                    if (b->isChecked())
                        list += (list.empty() ? "" : ",") + n;
                Set s = groomBrushSettings();
                s.targets = list.empty() ? "none" : list;
                setGroomBrushSettings(s);
            });
            mTargetBoxes.push_back({t, box});
            tr->addWidget(box);
        }
        tr->addStretch();
        mTargets->setToolTip(brushTip("targets", "Acts On: what the brush shapes"));
        mForm->addRow("Acts On", mTargets);
        mRows.push_back({"targets", mTargets});
        mPressure = check("pressure", "Pen Pressure", "pen pressure scales the strength", [](Set& s, bool on) { s.pressure = on; });
        mCollide = check("collide", "Keep Off Scalp", "edited guides are pushed out of the scalp", [](Set& s, bool on) { s.collide = on; });
        mFront = check("frontFacingOnly", "Front Only", "what the scalp hides from the camera is left alone", [](Set& s, bool on) { s.frontOnly = on; });
        mStretch = check("stretch", "Stretch", "guides stretch with the drag instead of keeping their length", [](Set& s, bool on) { s.stretch = on; });
        mSettings = controls::section("Settings", fields, this, kSections);
        v->addWidget(mSettings);

        auto* keys = new QWidget(this);
        auto* kr = new QHBoxLayout(keys);
        kr->setContentsMargins(0, 0, 0, 0);
        kr->setSpacing(4);
        auto push = [&](const QString& text, const QString& t, std::function<void()> fn) {
            auto* b = new QToolButton(keys);
            b->setText(text);
            b->setToolTip(t);
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            b->setFocusPolicy(Qt::NoFocus);
            controls::setRole(b, "push");
            connect(b, &QToolButton::clicked, this, [fn] {
                fn();
                syncGroomPalette();
            });
            kr->addWidget(b);
            return b;
        };
        mApply = push("Apply", "Apply: set the guides to the keyed lengths  (Enter)", [] {
            if (gActiveBrush)
                gActiveBrush->applyLengthKeys();
        });
        mCancel = push("Cancel", "Cancel: drop the key edits  (Esc)", [] {
            if (gActiveBrush)
                gActiveBrush->cancelLengthKeys();
        });
        push("Delete Key", "Delete Key: remove the selected key  (Delete)", [] {
            if (gActiveBrush)
                gActiveBrush->deleteLengthKey();
        });
        push("Clear", "Clear: remove every key", [] {
            if (gActiveBrush)
                gActiveBrush->clearLengthKeys();
        });
        mKeysSection = controls::section("Length Keys", keys, this, kSections);
        v->addWidget(mKeysSection);
        sync();
    }

    void sync()
    {
        const groom::GroomBrushSettings& s = groomBrushSettings();
        const bool groomBrushes = groomBrushToolActive();
        const bool lockBrush = !groomBrushes && viewportTools::active() && std::string_view(viewportTools::active()->name()) == "Sculpt Curves";
        for (const auto& [id, b] : mTools)
            b->setChecked(groomBrushes && id == s.mode);
        for (const auto& [index, b] : mLocks)
            b->setChecked(lockBrush && index == curveSculptBrush());
        const groom::GroomBrushEntry* e = groom::groomBrushEntry(s.mode);
        const auto has = [&](const std::string& id) { return e && std::find(e->settings.begin(), e->settings.end(), id) != e->settings.end(); };
        for (const auto& [id, w] : mRows)
            mForm->setRowVisible(w, has(id));
        mSettings->setVisible(groomBrushes);
        auto set = [](QDoubleSpinBox* box, double x) {
            const QSignalBlocker block(box);
            box->setValue(x);
        };
        {
            const QSignalBlocker block(mStrength);
            mStrength->setMinimum(e && e->signedStrength ? -1.0 : 0.0);
        }
        set(mRadius, s.radius);
        set(mStrength, s.strength);
        set(mFalloff, s.falloff);
        set(mSmooth, s.smoothStrength);
        set(mValue, s.value);
        set(mLength, s.placeLength);
        set(mSegments, s.placeSegments);
        set(mSpacing, s.placeSpacing);
        auto tick = [](QCheckBox* box, bool on) {
            const QSignalBlocker block(box);
            box->setChecked(on);
        };
        tick(mMatch, s.matchNeighbors);
        tick(mPressure, s.pressure);
        tick(mCollide, s.collide);
        tick(mFront, s.frontOnly);
        tick(mStretch, s.stretch);
        for (const auto& [n, b] : mTargetBoxes)
            tick(b, s.targets.find(n) != std::string::npos);
        {
            const QSignalBlocker block(mTube);
            mTube->setCurrent(s.tubeChannel == "flatness" ? 1 : s.tubeChannel == "twist" ? 2 : 0);
        }
        {
            const QSignalBlocker block(mPaintOn);
            mPaintOn->setCurrent(s.paintKind == "vertices" ? 1 : s.paintKind == "texture" ? 2 : 0);
        }
        {
            const QSignalBlocker block(mAttribute);
            mAttribute->setEditText(QString::fromStdString(s.attribute));
        }
        mAttribute->setEnabled(s.mode != "freeze");
        mValue->setEnabled(s.mode != "freeze");
        if (!mPartName->hasFocus())
            mPartName->setText(QString::fromStdString(s.partName));
        const std::vector<std::string> sinks = {"", "whorl", "whorlcw", "converge", "part"};
        const auto sink = std::find(sinks.begin(), sinks.end(), s.flowSink);
        {
            const QSignalBlocker block(mSink);
            mSink->setCurrent(sink == sinks.end() ? 0 : int(sink - sinks.begin()));
        }
        mKeysSection->setVisible(groomBrushes && s.mode == "lengths");
        const bool edited = gActiveBrush && gActiveBrush->lengthKeysEdited();
        mApply->setEnabled(edited);
        mCancel->setEnabled(edited);
    }

private:
    std::vector<std::pair<std::string, QToolButton*>> mTools;
    std::vector<std::pair<int, QToolButton*>> mLocks;
    std::vector<std::pair<std::string, QWidget*>> mRows;
    QFormLayout* mForm = nullptr;
    QWidget *mSettings = nullptr, *mKeysSection = nullptr, *mTargets = nullptr;
    QDoubleSpinBox *mRadius = nullptr, *mStrength = nullptr, *mFalloff = nullptr, *mSmooth = nullptr, *mValue = nullptr, *mLength = nullptr,
                   *mSegments = nullptr, *mSpacing = nullptr;
    QCheckBox *mMatch = nullptr, *mPressure = nullptr, *mCollide = nullptr, *mFront = nullptr, *mStretch = nullptr;
    std::vector<std::pair<std::string, QCheckBox*>> mTargetBoxes;
    controls::Segmented *mTube = nullptr, *mPaintOn = nullptr, *mSink = nullptr;
    QComboBox* mAttribute = nullptr;
    QLineEdit* mPartName = nullptr;
    QToolButton *mApply = nullptr, *mCancel = nullptr;
};
} // namespace

const groom::GroomBrushSettings& groomBrushSettings()
{
    return state().settings;
}

void setGroomBrushSettings(const groom::GroomBrushSettings& settings)
{
    state().settings = settings;
    save();
}

void setGroomBrushMode(const std::string& id)
{
    GroomBrushState& st = state();
    const groom::GroomBrushEntry* e = groom::groomBrushEntry(id);
    if (!e)
        return;
    st.strengths[st.settings.mode] = st.settings.strength;
    st.settings.mode = id;
    const auto it = st.strengths.find(id);
    st.settings.strength = it != st.strengths.end() ? it->second : e->defaultStrength;
    save();
    if (!viewportTools::active() || std::string_view(viewportTools::active()->name()) != "Groom Brushes")
        viewportTools::activate(kToolId);
}

void resetGroomBrushSettings()
{
    GroomBrushState& st = state();
    const std::string mode = st.settings.mode;
    st.settings = groom::GroomBrushSettings{};
    st.settings.mode = mode;
    st.strengths.erase(mode);
    if (const groom::GroomBrushEntry* e = groom::groomBrushEntry(mode))
        st.settings.strength = e->defaultStrength;
    save();
}

bool groomBrushToolActive()
{
    return viewportTools::active() && std::string_view(viewportTools::active()->name()) == "Groom Brushes";
}

std::vector<KeyHint> groomBrushKeys()
{
    const groom::GroomBrushSettings& s = state().settings;
    std::vector<KeyHint> keys = {{"Drag", label(s.mode)}, {"Shift+drag", "Smooth"}, {"Ctrl+drag", "Other way"}};
    keys.insert(keys.end(), {{"B+drag", "Radius"}, {"M+drag", "Strength"}, {"[  ]", "Smaller, larger"}, {"Esc", "Exit"}});
    return keys;
}

QWidget* makeGroomBrushPage(QWidget* parent)
{
    return new GroomBrushPage(parent);
}

void syncGroomBrushPage(QWidget* page)
{
    if (auto* p = dynamic_cast<GroomBrushPage*>(page))
        p->sync();
}

void registerGroomBrushTool()
{
    if (!viewportTools::registry().find(kToolId))
        viewportTools::add({.id = kToolId, .label = "Groom Brushes", .icon = "brush", .create = [] { return std::make_unique<GroomBrushTool>(); }});
    actions::add({.id = kToolId,
                  .label = "Groom Brushes",
                  .category = "Groom",
                  .run = [] { viewportTools::activate(kToolId); },
                  .doc = "Places, draws, combs, slides, smooths, lifts, swings, attracts, shapes, cuts, lengthens, erases and paints the selected "
                         "groom's guides, parts and flow with brushes.",
                  .python = "lila.ui.run_action(\"groom.brushes\")",
                  .keywords = {"hair", "groom", "brush", "comb", "sculpt", "xgen", "yeti", "ornatrix", "place", "part", "flow"}});
    for (const groom::GroomBrushEntry& e : groom::groomBrushCatalog())
    {
        const std::string id = e.id;
        actions::add({.id = std::string(kToolId) + "." + id,
                      .label = "Groom Brushes: " + e.label,
                      .category = "Groom",
                      .run = [id] { setGroomBrushMode(id); },
                      .doc = e.hint,
                      .python = "lila.ui.run_action(\"groom.brushes." + id + "\")"});
    }
}
} // namespace lila::ui
