// The Strings Rules editor: a character's joints seen from the front, with its contacts, limbs and lines of action as
// relations to switch on and off, and a test drag that poses the character through them without keeping the pose.

#include "Puppeteer.h"
#include "rig/Rig.h"

#include <lila/Controls.h>
#include <lila/Events.h>
#include <lila/Panels.h>
#include <lila/UI.h>
#include <lila/Undo.h>
#include <lila/ViewportTool.h>

#include <QHBoxLayout>
#include <QCursor>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

#include <QVariantMap>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace puppeteer
{
namespace
{
enum class Layer
{
    Contacts,
    Limbs,
    Lines,
    Isolate,
    Modes,
    Test,
};

std::string jointLeaf(const std::string& name) { return name.substr(name.find_last_of("|:") + 1); }

/** The character the editor shows: the selection's, else the only one in the scene. */
NodeId shownCharacter()
{
    Graph& g = Graph::current();
    for (NodeId id : g.selection())
    {
        if (Node* n = g.node(id); n && n->type().isA(kCharacterType))
            return id;
        if (const NodeId c = characterFor(g, id))
            return c;
    }
    NodeId only = kNoNode;
    for (NodeId id : g.nodes())
        if (g.node(id)->type().isA(kCharacterType))
        {
            if (only)
                return kNoNode;
            only = id;
        }
    return only;
}

class RulesCanvas final : public QWidget
{
public:
    explicit RulesCanvas(QWidget* parent) : QWidget(parent)
    {
        setObjectName("stringsRulesCanvas");
        setMouseTracking(true);
        setMinimumSize(240, 300);
    }

    Layer layer = Layer::Contacts;
    std::function<void(const QString&)> status;

    /** Reads the character's joints and rules again. */
    void refresh()
    {
        mJoints.clear();
        mRules = json();
        Graph& g = Graph::current();
        mCharacter = shownCharacter();
        if (mCharacter && characterRoot(g, mCharacter))
        {
            g.evaluate();
            for (NodeId j : fullBodyJoints(g, characterRoot(g, mCharacter)))
            {
                const glm::dvec3 p(worldOf(g, j)[3]);
                mJoints.push_back({j, jointLeaf(g.node(j)->name()), p, g.parent(j)});
            }
            try
            {
                mRules = ops::execute("stringsRules", {{"args", {g.node(mCharacter)->path()}}});
            }
            catch (const std::exception&)
            {
            }
        }
        setProperty("jointCount", int(mJoints.size()));
        setProperty("contactCount", mRules.is_object() ? int(mRules["contacts"].size()) : 0);
        setProperty("limbCount", mRules.is_object() ? int(mRules["chains"].size()) : 0);
        setProperty("lineCount", mRules.is_object() ? int(mRules["lines"].size()) : 0);
        setProperty("isolateCount", mRules.is_object() ? int(mRules["isolate"].size()) : 0);
        setProperty("modeCount", mRules.is_object() ? int(mRules["modes"].size()) : 0);
        publishJoints();
        update();
    }

    NodeId character() const { return mCharacter; }


protected:
    void resizeEvent(QResizeEvent*) override { publishJoints(); }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), palette().color(QPalette::Base));
        if (mJoints.empty())
        {
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(rect(), Qt::AlignCenter, "No character. Select a joint of one.");
            return;
        }
        fit();
        const QColor text = palette().color(QPalette::Text), dim = palette().color(QPalette::PlaceholderText), bone = palette().color(QPalette::Mid);
        auto pos = [&](size_t i) { return toScreen(mJoints[i].world); };
        // Bones.
        p.setPen(QPen(bone, 1.5));
        for (size_t i = 0; i < mJoints.size(); ++i)
            if (const int pi = indexOf(mJoints[i].parent); pi >= 0)
                p.drawLine(pos(size_t(pi)), pos(i));
        // Limbs: a broad band along each, bright in the Limbs layer.
        const bool limbs = layer == Layer::Limbs, lines = layer == Layer::Lines;
        for (const json& c : rule("chains"))
            drawPath(p, names(c), QPen(limbs ? withAlpha(text, 110) : withAlpha(dim, 50), limbs ? 9.0 : 6.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        // Lines of action: a dashed curve beside the body path.
        for (const auto& [name, ends] : rule("lines").items())
            if (ends.size() == 2)
                drawLine(p, ends[0].get<std::string>(), ends[1].get<std::string>(), lines ? text : withAlpha(dim, 90), lines);
        // Joints, contacts ringed, isolated joints squared, a Default mode pick named beside its joint.
        std::vector<std::string> contacts, isolated;
        for (const json& c : rule("contacts"))
            contacts.push_back(c.get<std::string>());
        for (const json& c : rule("isolate"))
            isolated.push_back(c.get<std::string>());
        for (size_t i = 0; i < mJoints.size(); ++i)
        {
            const bool contact = std::find(contacts.begin(), contacts.end(), mJoints[i].name) != contacts.end();
            const bool hot = int(i) == mHover || int(i) == mPress;
            p.setPen(Qt::NoPen);
            p.setBrush(hot ? text : dim);
            p.drawEllipse(pos(i), hot ? 4.0 : 3.0, hot ? 4.0 : 3.0);
            if (contact)
            {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(layer == Layer::Contacts ? text : dim, 2.0));
                p.drawEllipse(pos(i), 8.0, 8.0);
            }
            if (std::find(isolated.begin(), isolated.end(), mJoints[i].name) != isolated.end())
            {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(layer == Layer::Isolate ? text : dim, 1.5));
                p.drawRect(QRectF(pos(i) - QPointF(6.0, 6.0), QSizeF(12.0, 12.0)));
            }
            if (layer == Layer::Modes && rule("modes").contains(mJoints[i].name))
                if (const auto m = strings::modeFromName(rule("modes")[mJoints[i].name].get<std::string>()))
                {
                    p.setPen(text);
                    p.drawText(pos(i) + QPointF(9.0, 4.0), QString::fromUtf8(stringsModeLabel(*m)));
                }
        }
        // A drag between two joints shows the limb or line it would make.
        if (mPress >= 0 && mHover >= 0 && mHover != mPress && (limbs || lines))
        {
            p.setPen(QPen(text, 2.0, Qt::DashLine));
            p.drawLine(pos(size_t(mPress)), pos(size_t(mHover)));
        }
        if (mHover >= 0)
        {
            // Kept inside the canvas: left of the joint when it sits near the right edge.
            const QString name = QString::fromStdString(mJoints[size_t(mHover)].name);
            const QPointF at = pos(size_t(mHover));
            const double w = p.fontMetrics().horizontalAdvance(name);
            p.setPen(text);
            p.drawText(at + QPointF(at.x() + 10.0 + w > width() - 4.0 ? -10.0 - w : 10.0, -8.0), name);
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        mHover = nearest(e->position());
        if (layer == Layer::Test && mPress >= 0)
            testDrag(e->position());
        update();
    }

    void leaveEvent(QEvent*) override
    {
        mHover = -1;
        update();
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        mPress = nearest(e->position());
        mPressAt = e->position();
        if (layer == Layer::Test && mPress >= 0)
            beginTest();
        update();
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || mPress < 0)
            return;
        const int from = mPress, to = nearest(e->position());
        mPress = -1;
        if (layer == Layer::Test)
            endTest();
        else if (to >= 0)
            edit(from, to);
        update();
    }

    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Escape && mPress >= 0 && layer == Layer::Test)
        {
            mPress = -1;
            endTest();
            update();
            return;
        }
        QWidget::keyPressEvent(e);
    }

private:
    struct J
    {
        NodeId id;
        std::string name;
        glm::dvec3 world;
        NodeId parent;
    };

    /** One part of the rules ("contacts", "chains", "lines"), empty when there are none. */
    const json& rule(const char* key) const
    {
        static const json none;
        return mRules.is_object() && mRules.contains(key) ? mRules[key] : none;
    }

    static QColor withAlpha(QColor c, int a)
    {
        c.setAlpha(a);
        return c;
    }

    /** Front view: world X right, Y up, fit with a margin and the same scale on both axes. */
    void fit()
    {
        if (mFitFrozen)
            return;
        glm::dvec2 lo(1e30), hi(-1e30);
        for (const J& j : mJoints)
        {
            lo = glm::min(lo, glm::dvec2(j.world.x, j.world.y));
            hi = glm::max(hi, glm::dvec2(j.world.x, j.world.y));
        }
        const double margin = 24.0;
        const glm::dvec2 size = glm::max(hi - lo, glm::dvec2(1.0));
        mScale = std::min((width() - 2 * margin) / size.x, (height() - 2 * margin) / size.y);
        mCentre = (lo + hi) * 0.5;
    }

    /** Where each joint draws, as the "joints" property ({name: point}), for scripts and tests. */
    void publishJoints()
    {
        QVariantMap at;
        if (!mJoints.empty())
        {
            fit();
            for (const J& j : mJoints)
                at[QString::fromStdString(j.name)] = toScreen(j.world);
        }
        setProperty("joints", at);
    }

    QPointF toScreen(const glm::dvec3& w) const
    {
        return {width() * 0.5 + (w.x - mCentre.x) * mScale, height() * 0.5 - (w.y - mCentre.y) * mScale};
    }

    int indexOf(NodeId id) const
    {
        for (size_t i = 0; i < mJoints.size(); ++i)
            if (mJoints[i].id == id)
                return int(i);
        return -1;
    }

    int indexOf(const std::string& name) const
    {
        for (size_t i = 0; i < mJoints.size(); ++i)
            if (mJoints[i].name == name)
                return int(i);
        return -1;
    }

    int nearest(QPointF px)
    {
        if (mJoints.empty())
            return -1;
        fit();
        int best = -1;
        double bestD = 12.0;
        for (size_t i = 0; i < mJoints.size(); ++i)
        {
            const QPointF q = toScreen(mJoints[i].world);
            if (const double d = std::hypot(q.x() - px.x(), q.y() - px.y()); d < bestD)
            {
                bestD = d;
                best = int(i);
            }
        }
        return best;
    }

    static std::vector<std::string> names(const json& list)
    {
        std::vector<std::string> out;
        for (const json& n : list)
            out.push_back(n.get<std::string>());
        return out;
    }

    void drawPath(QPainter& p, const std::vector<std::string>& path, const QPen& pen)
    {
        QPainterPath line;
        bool first = true;
        for (const std::string& n : path)
            if (const int i = indexOf(n); i >= 0)
            {
                const QPointF q = toScreen(mJoints[size_t(i)].world);
                first ? line.moveTo(q) : line.lineTo(q);
                first = false;
            }
        p.strokePath(line, pen);
    }

    /** The joints from top down to end, top first; empty when end isn't below top. */
    std::vector<std::string> pathDown(const std::string& top, const std::string& end) const
    {
        std::vector<std::string> out;
        for (int i = indexOf(end); i >= 0; i = indexOf(mJoints[size_t(i)].parent))
        {
            out.insert(out.begin(), mJoints[size_t(i)].name);
            if (mJoints[size_t(i)].name == top)
                return out;
        }
        return {};
    }

    void drawLine(QPainter& p, const std::string& top, const std::string& end, const QColor& c, bool bright)
    {
        const std::vector<std::string> path = pathDown(top, end);
        if (path.size() < 2)
            return;
        // Set off to one side of the body path so it reads apart from the limb bands.
        QPainterPath line;
        for (size_t k = 0; k < path.size(); ++k)
        {
            const QPointF a = toScreen(mJoints[size_t(indexOf(path[k]))].world);
            const QPointF b = toScreen(mJoints[size_t(indexOf(path[std::min(k + 1, path.size() - 1)]))].world);
            const QPointF b0 = toScreen(mJoints[size_t(indexOf(path[k == 0 ? 0 : k - 1]))].world);
            QPointF d = (k + 1 < path.size() ? b : a) - (k > 0 ? b0 : a);
            const double len = std::hypot(d.x(), d.y());
            const QPointF n = len > 1e-6 ? QPointF(-d.y() / len, d.x() / len) * 7.0 : QPointF();
            k == 0 ? line.moveTo(a + n) : line.lineTo(a + n);
        }
        p.strokePath(line, QPen(c, bright ? 2.0 : 1.5, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin));
    }

    void run(const json& call)
    {
        runCommand("stringsRules", call);
        refresh();
    }

    void edit(int from, int to)
    {
        Graph& g = Graph::current();
        const std::string character = g.node(mCharacter)->path();
        const std::string a = mJoints[size_t(from)].name, b = mJoints[size_t(to)].name;
        if (layer == Layer::Isolate)
        {
            if (from != to)
                return;
            json isolate = json::array();
            bool had = false;
            for (const json& c : rule("isolate"))
                if (c.get<std::string>() == a)
                    had = true;
                else if (const int i = indexOf(c.get<std::string>()); i >= 0)
                    isolate.push_back(g.node(mJoints[size_t(i)].id)->path());
            if (!had)
                isolate.push_back(g.node(mJoints[size_t(from)].id)->path());
            run({{"args", {character}}, {"isolate", isolate}});
            return;
        }
        if (layer == Layer::Modes)
        {
            if (from != to)
                return;
            // The joint's Default pick from a menu of the modes; No Rule goes back to moves IK, turns Forward.
            QMenu menu(this);
            const std::string joint = g.node(mJoints[size_t(from)].id)->path();
            const std::string current = rule("modes").contains(a) ? rule("modes")[a].get<std::string>() : std::string();
            QAction* none = menu.addAction("No Rule", [this, character, joint] { run({{"args", {character}}, {"defaultMode", {joint, ""}}}); });
            none->setCheckable(true);
            none->setChecked(current.empty());
            menu.addSeparator();
            for (strings::Mode m : strings::kModes)
            {
                if (m == strings::Mode::Default)
                    continue;
                const std::string name = strings::modeName(m);
                QAction* item = menu.addAction(QString::fromUtf8(stringsModeLabel(m)),
                                               [this, character, joint, name] { run({{"args", {character}}, {"defaultMode", {joint, name}}}); });
                item->setCheckable(true);
                item->setChecked(current == name);
            }
            menu.exec(QCursor::pos());
            return;
        }
        if (layer == Layer::Contacts)
        {
            if (from != to)
                return;
            json contacts = json::array();
            bool had = false;
            for (const json& c : rule("contacts"))
                if (c.get<std::string>() == a)
                    had = true;
                else
                    contacts.push_back(g.node(mJoints[size_t(indexOf(c.get<std::string>()))].id)->path());
            if (!had)
                contacts.push_back(g.node(mJoints[size_t(from)].id)->path());
            run({{"args", {character}}, {"contacts", contacts}});
            return;
        }
        if (from == to)
        {
            // A click on a limb or a line takes it away: the shortest line through the joint.
            if (layer == Layer::Limbs)
            {
                for (const json& c : rule("chains"))
                    for (const json& n : c)
                        if (n.get<std::string>() == a)
                            return run({{"args", {character}}, {"removeChain", g.node(mJoints[size_t(from)].id)->path()}});
                return report("No limb through " + a + ": drag from a joint to one below it to make one");
            }
            std::string pick;
            size_t shortest = SIZE_MAX;
            for (const auto& [name, ends] : rule("lines").items())
                if (ends.size() == 2)
                {
                    const std::vector<std::string> path = pathDown(ends[0].get<std::string>(), ends[1].get<std::string>());
                    if (std::find(path.begin(), path.end(), a) != path.end() && path.size() < shortest)
                    {
                        shortest = path.size();
                        pick = name;
                    }
                }
            if (pick.empty())
                return report("No line through " + a + ": drag from a joint to one below it to make one");
            return run({{"args", {character}}, {"removeLine", pick}});
        }
        // Either drag direction works: the higher joint is the top.
        std::string top = a, end = b;
        if (pathDown(top, end).empty())
            std::swap(top, end);
        if (pathDown(top, end).size() < 2)
            return report(a + " and " + b + " aren't on one path down the body");
        const std::string topPath = g.node(mJoints[size_t(indexOf(top))].id)->path(), endPath = g.node(mJoints[size_t(indexOf(end))].id)->path();
        if (layer == Layer::Limbs)
            run({{"args", {character}}, {"addChain", {topPath, endPath}}});
        else
            run({{"args", {character}}, {"addLine", {end, topPath, endPath}}});
    }

    void report(const std::string& s)
    {
        if (status)
            status(QString::fromStdString(s));
    }

    // Test drags: the pose shows in the viewport while dragging and goes back on release.
    void beginTest()
    {
        fit();
        mFitFrozen = true;
        Graph& g = Graph::current();
        mSaved.clear();
        for (const J& j : mJoints)
            mSaved.push_back({j.id, g.value({j.id, Transform::kTranslate, -1}), g.value({j.id, Transform::kRotate, -1})});
    }

    template <class Fn> static std::string quiet(Fn&& fn)
    {
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        std::string error;
        try
        {
            fn();
        }
        catch (const std::exception& e)
        {
            error = e.what();
        }
        u.setEnabled(was);
        return error;
    }

    void restoreTest()
    {
        Graph& g = Graph::current();
        quiet([&] {
            for (const Saved& s : mSaved)
                if (g.node(s.id))
                {
                    if (g.value({s.id, Transform::kTranslate, -1}) != s.translate)
                        g.setValue({s.id, Transform::kTranslate, -1}, s.translate);
                    if (g.value({s.id, Transform::kRotate, -1}) != s.rotate)
                        g.setValue({s.id, Transform::kRotate, -1}, s.rotate);
                }
            g.evaluate();
        });
    }

    void testDrag(QPointF px)
    {
        restoreTest();
        Graph& g = Graph::current();
        const QPointF d = px - mPressAt;
        const glm::dvec3 by(d.x() / mScale, -d.y() / mScale, 0.0);
        // Forward has nothing that bends to put a joint somewhere: the test drags in IK there.
        const strings::Mode mode = strings::movesOrigin(stringsMode(), false) ? stringsMode() : strings::Mode::Ik;
        json result;
        const std::string error = quiet([&] {
            result = ops::execute("stringsPose", {{"args", {g.node(mJoints[size_t(mPress)].id)->path()}},
                                                  {"mode", strings::modeName(mode)},
                                                  {"by", vecJson(by)},
                                                  {"stretch", stringsStretch()}},
                                  CallSource::Script);
        });
        mTesting = true;
        std::string s = "Test " + std::string(stringsModeLabel(mode)) + ": " + mJoints[size_t(mPress)].name;
        if (!error.empty())
            s += "  " + error;
        else if (result.is_object() && result.contains("anchor"))
            s += "  from " + jointLeaf(result["anchor"].get<std::string>());
        report(s);
        followPose();
        viewportTools::requestRedraw();
    }

    /** The joints where the scene has them now. */
    void followPose()
    {
        Graph& g = Graph::current();
        quiet([&] { g.evaluate(); });
        for (J& j : mJoints)
            if (g.node(j.id))
                j.world = glm::dvec3(worldOf(g, j.id)[3]);
    }

    void endTest()
    {
        if (mTesting)
            restoreTest();
        mTesting = false;
        mFitFrozen = false;
        followPose();
        mSaved.clear();
        report("");
        viewportTools::requestRedraw();
    }

    struct Saved
    {
        NodeId id;
        PlugValue translate, rotate;
    };

    NodeId mCharacter = kNoNode;
    std::vector<J> mJoints;
    json mRules;
    double mScale = 1.0;
    glm::dvec2 mCentre{0.0};
    int mHover = -1, mPress = -1;
    QPointF mPressAt;
    bool mTesting = false;
    /** While a test drag runs the view holds still, so the drag maps to the same centimetres throughout. */
    bool mFitFrozen = false;
    std::vector<Saved> mSaved;
};

QWidget* makeRulesEditor(QWidget* parent)
{
    auto* w = new QWidget(parent);
    w->setObjectName("stringsRulesEditor");
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);
    auto* layers = new controls::Segmented(w);
    layers->setObjectName("stringsRulesLayers");
    layers->add("Contacts", {}, "Click a joint: it holds still while you pose the rest", int(Layer::Contacts));
    layers->add("Limbs", {}, "Drag from a joint to one below it: a limb that bends together. Click a limb to remove it", int(Layer::Limbs));
    layers->add("Lines", {}, "Drag from a joint to one below it: a line of action strokes bind to. Click a line to remove it", int(Layer::Lines));
    layers->add("Isolate", {}, "Click a joint: it keeps its world rotation while Isolate Rotation is on (squared)", int(Layer::Isolate));
    layers->add("Modes", {}, "Click a joint: the mode it poses in under Default", int(Layer::Modes));
    layers->add("Test", {}, "Drag a joint: the character poses through its rules in the current Strings mode, and goes back on release", int(Layer::Test));
    layers->setCurrent(int(Layer::Contacts));
    auto* canvas = new RulesCanvas(w);
    auto* status = new QLabel(w);
    status->setObjectName("stringsRulesStatus");
    status->setForegroundRole(QPalette::PlaceholderText);
    auto* source = new QLabel(w);
    source->setObjectName("stringsRulesSource");
    source->setForegroundRole(QPalette::PlaceholderText);
    auto* reset = controls::push("puppeteer.strings.resetRules", w, "Reset to Template");
    auto* row = new QHBoxLayout;
    row->addWidget(source, 1);
    row->addWidget(reset);
    v->addWidget(layers);
    v->addWidget(canvas, 1);
    v->addWidget(status);
    v->addLayout(row);
    canvas->status = [status](const QString& s) { status->setText(s); };
    auto refresh = [canvas, source] {
        canvas->refresh();
        Graph& g = Graph::current();
        if (!canvas->character())
        {
            source->clear();
            return;
        }
        try
        {
            const json r = ops::execute("stringsRules", {{"args", {g.node(canvas->character())->path()}}});
            auto from = [](const json& v) {
                const std::string s = v.get<std::string>();
                return QString::fromStdString(s == "character" ? "own" : s);
            };
            source->setText(QString("%1    Contacts: %2    Limbs: %3")
                                .arg(QString::fromStdString(g.node(canvas->character())->name()), from(r["contactsFrom"]), from(r["chainsFrom"])));
        }
        catch (const std::exception&)
        {
        }
    };
    QObject::connect(layers, &controls::Segmented::picked, canvas, [canvas, status](int id) {
        canvas->layer = Layer(id);
        status->clear();
        canvas->update();
    });
    refresh();
    std::vector<events::SubscriptionId> subs;
    for (events::Event e : {events::Event::UndoStackChanged, events::Event::SelectionChanged})
        subs.push_back(events::subscribe(e, [refresh](const events::EventData&) { refresh(); }, "Puppeteer"));
    QObject::connect(w, &QObject::destroyed, [subs] {
        for (events::SubscriptionId s : subs)
            events::unsubscribe(s);
    });
    return w;
}
} // namespace

void registerRulesEditor(ext::Context& ctx)
{
    panels::add(ctx, {.id = "puppeteerStringsRules",
                      .title = "Strings Rules",
                      .create = makeRulesEditor,
                      .icon = "affiliate",
                      .open = false,
                      .toggleAction = "window.puppeteerStringsRules",
                      .keywords = {"strings", "rules", "rule web", "contacts", "limbs", "line of action", "ephemeral"},
                      .floatFirst = true,
                      .floatWidth = 420,
                      .floatHeight = 620});
}
} // namespace puppeteer
