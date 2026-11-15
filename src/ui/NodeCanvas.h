#pragma once

// The node editor's canvas: graph items and the view that edits them. Internal to lila_ui.

#include "ui/NodeEditor.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "gn/Group.h"
#include "gn/Routing.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "ui/Actions.h"
#include "ui/Deletion.h"
#include "ui/AnimatedSend.h"
#include "ui/DockSidebar.h"
#include "ui/Icons.h"
#include "ui/SpinScrub.h"
#include "ui/Theme.h"
#include "ui/UiCommon.h"
#include "ui/ValueReset.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStyleOptionGraphicsItem>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QElapsedTimer>
#include <QtMath>

#include <array>
#include <cctype>
#include <cmath>
#include <utility>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace lila::ui
{
using json = nlohmann::json;
class NodeCanvas;

namespace canvas
{
constexpr double kNodeWidth = 170.0;
constexpr double kHeader = 24.0;
constexpr double kRow = 20.0;
constexpr double kSocketR = 5.0;

/** general prefs the canvas reads. */
inline constexpr const char* kPrefAutoOffset = "/nodeEditor/autoOffset";
inline constexpr const char* kPrefMinimap = "/nodeEditor/minimap";
/** "curved", "straight" or "rightAngle". */
inline constexpr const char* kPrefWireStyle = "/nodeEditor/wireStyle";
inline constexpr const char* kPrefDimUnrelated = "/nodeEditor/dimUnrelated";
/** In a grey theme, draw every wire one grey rather than in its socket type's shade. */
inline constexpr const char* kPrefGreyWires = "/nodeEditor/greyWiresInMonotone";

enum class WireStyle : uint8_t
{
    Curved,
    Straight,
    RightAngle,
};

struct CanvasPrefs
{
    bool autoOffset = true;
    bool minimap = false;
    WireStyle wires = WireStyle::Curved;
    bool dimUnrelated = false;
    bool greyWires = false;
};

inline CanvasPrefs readCanvasPrefs()
{
    PrefStore& s = PrefStore::get();
    CanvasPrefs p;
    p.autoOffset = s.value<bool>("general", kPrefAutoOffset, true);
    p.minimap = s.value<bool>("general", kPrefMinimap, false);
    std::string style = s.value<std::string>("general", kPrefWireStyle, "curved");
    p.wires = style == "straight" ? WireStyle::Straight : style == "rightAngle" ? WireStyle::RightAngle : WireStyle::Curved;
    p.dimUnrelated = s.value<bool>("general", kPrefDimUnrelated, false);
    p.greyWires = s.value<bool>("general", kPrefGreyWires, false);
    return p;
}

/** How every canvas draws its wires; set from the prefs. */
inline WireStyle gWireStyle = WireStyle::Curved;

/** "sizeX" -> "Size X". */
inline QString pretty(const std::string& id)
{
    QString out;
    for (size_t i = 0; i < id.size(); ++i)
    {
        char c = id[i];
        bool upper = c >= 'A' && c <= 'Z';
        bool digit = c >= '0' && c <= '9';
        bool prevDigit = i > 0 && id[i - 1] >= '0' && id[i - 1] <= '9';
        if (i == 0)
            out += QChar(c).toUpper();
        else if (upper || (digit && !prevDigit))
            out += QString(' ') + QChar(c);
        else
            out += QChar(c);
    }
    return out;
}

/** "Spike Length" -> "spikeLength": the parameter name for a typed display name, empty when nothing usable is left. */
inline std::string identifierFrom(const QString& text)
{
    std::string out;
    bool up = false;
    for (QChar ch : text)
    {
        char c = ch.toLatin1();
        bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!alnum)
        {
            up = !out.empty();
            continue;
        }
        if (out.empty() && c >= '0' && c <= '9')
            out += '_';
        out += out.empty() ? char(std::tolower(c)) : up ? char(std::toupper(c)) : c;
        up = false;
    }
    return out;
}

/** Runs a command from the UI; failures go to the log rather than a dialog. Queries run quietly, edits echo like any UI action. */
inline json runCmd(const std::string& name, const json& call, bool query = false)
{
    try
    {
        return ops::execute(name, call, query ? CallSource::Script : CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::warn("{}", e.what());
        return nullptr;
    }
}

inline json args(std::initializer_list<json> a) { return {{"args", json(a)}}; }

inline json runVerb(const GraphBackend& b, std::string_view verb, const json& call)
{
    json adapted = call;
    b.adaptCall(verb, adapted);
    return runCmd(b.command(verb), adapted, b.isQuery(verb));
}

inline bool hasPlug(const std::string& path)
{
    try
    {
        return scene::Graph::current().plug(path).attr >= 0;
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
}

/** Nonzero while a side-panel field sets a value, which then doesn't rebuild the panel under the field. */
inline int gFieldEdits = 0;

inline void setFromField(const GraphBackend& b, const json& call)
{
    ++gFieldEdits;
    runVerb(b, "SetValue", call);
    --gFieldEdits;
}

/** Fields are diamonds (like Blender); otherwise the backend's shape and mark. */
inline void drawSocket(QPainter& p, QPointF c, const GraphBackend::SocketStyle& style, bool field)
{
    double r = style.radius;
    p.setPen(QPen(gray(18), 1.2));
    p.setBrush(style.color);
    if (field)
    {
        QPolygonF d;
        d << c + QPointF(0, -r - 1) << c + QPointF(r + 1, 0) << c + QPointF(0, r + 1) << c + QPointF(-r - 1, 0);
        p.drawPolygon(d);
    }
    else if (style.shape == GraphBackend::SocketShape::Square)
        p.drawRect(QRectF(c.x() - r + 0.5, c.y() - r + 0.5, 2 * r - 1, 2 * r - 1));
    else if (style.shape == GraphBackend::SocketShape::Pill)
        p.drawRoundedRect(QRectF(c.x() - r - 2, c.y() - r + 1.5, 2 * r + 4, 2 * r - 3), 3, 3);
    else
        p.drawEllipse(c, r, r);
    if (style.mark == GraphBackend::SocketMark::Dot)
    {
        p.setPen(Qt::NoPen);
        p.setBrush(gray(30));
        p.drawEllipse(c, 1.8, 1.8);
    }
    else if (style.mark == GraphBackend::SocketMark::Hole)
    {
        p.setPen(Qt::NoPen);
        p.setBrush(gray(45));
        p.drawEllipse(c, r * 0.5, r * 0.5);
    }
    else if (style.mark == GraphBackend::SocketMark::Ring)
    {
        p.setPen(QPen(gray(45), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, r * 0.55, r * 0.55);
    }
}

inline QString valueText(const json& v, const std::string& type)
{
    auto num = [](double d) {
        QString s = QString::number(d, 'f', 3);
        while (s.contains('.') && (s.endsWith('0') || s.endsWith('.')))
            s.chop(1);
        return s;
    };
    if (v.is_null())
        return {};
    if (type == "bool")
        return v.is_boolean() ? (v.get<bool>() ? "on" : "off") : (v.is_number() && v.get<double>() != 0.0 ? "on" : "off");
    if (v.is_array() && (v.size() == 3 || v.size() == 4) && v[0].is_number())
    {
        QString t = num(v[0].get<double>()) + ", " + num(v[1].get<double>()) + ", " + num(v[2].get<double>());
        return v.size() == 4 ? t + ", " + num(v[3].get<double>()) : t;
    }
    if (v.is_number())
        return num(v.get<double>());
    if (v.is_string())
        return qs(v.get<std::string>());
    return {};
}

struct SocketInfo
{
    std::string id;
    std::string type;
    bool field = false;
    bool multi = false;
    bool linked = false;
    json value;
    std::string implicit;
    std::string doc;
};

class NodeItem final : public QGraphicsItem
{
public:
    NodeItem(NodeCanvas* canvas, const json& info);

    std::string name;
    std::string type;
    std::string category;
    QString label;
    std::vector<SocketInfo> inputs, outputs;
    std::vector<std::pair<std::string, std::string>> props;
    /** Props with a text editor (code, multiline, font): drawn as typed, first line only. */
    std::set<std::string> textProps;
    bool viewer = false;
    bool viewerActive = false;
    std::string groupRef; // the group a Group node runs
    int thumbSize = 0;
    QImage thumb;

    bool isViewer() const { return viewer; }
    /** A reroute: a dot centred on its position, its input and output both at the centre. */
    bool reroute = false;
    /** Send or Receive: the name it carries. */
    bool portal = false;
    std::string portalName;
    /** Shown as a badge on the header, such as a Receive naming no Send. */
    QString problem;
    QRectF badgeRect() const { return QRectF(kNodeWidth - kHeader + 2, 3, kHeader - 6, kHeader - 6); }
    /** Header label, with a portal's name. */
    QString title() const;
    QString shownTitle() const { return muted ? title() + " (muted)" : title(); }
    static constexpr double kRerouteR = 6.0;
    /** The Viewer's eye toggle, in item coordinates. */
    QRectF eyeRect() const { return QRectF(kNodeWidth - kHeader, 2, kHeader - 4, kHeader - 4); }
    /** Socket under a scene point as a tooltip: id, type, field, doc. Empty when none. */
    QString socketTip(QPointF scenePos) const;

    QRectF boundingRect() const override;
    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override;

    /** Drawn as its header only; with only linked sockets; bypassed (drawn dimmed). */
    bool collapsed = false;
    bool hideUnused = false;
    bool muted = false;
    /** The socket rows drawn: indices into outputs and inputs. */
    std::vector<int> shownOut, shownIn;

    /** Picks the rows to draw from the flags and what is linked; call after either changes. */
    void layoutRows();

    /** Where the socket rows start: under the header and the thumbnail. */
    double top() const { return kHeader + (thumbSize > 0 ? thumbSize + 8.0 : 0.0); }
    double height() const;
    /** Set while the node hides in a collapsed frame: where its wires end instead, on the frame's title. */
    std::optional<QPointF> inProxy, outProxy;

    /** Socket centre in scene coordinates; a socket not drawn sits on the header's edge. */
    QPointF socketPos(bool input, int index) const;
    /** Socket under a scene point, -1 for none. */
    int socketAt(QPointF scenePos, bool& input) const;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

private:
    NodeCanvas* mCanvas;
};

/** A link by its ends: fromNode, fromSocket, toNode, toSocket. */
using LinkKey = std::array<std::string, 4>;

constexpr double kTieSpacing = 7.0; // between wires in a tie's ribbon
constexpr double kTieRun = 44.0;    // length of the parallel run through a tie
constexpr double kTieBand = 10.0;   // the band's width along the ribbon

/** The ribbon direction of a tie at angle degrees (0 left to right, y down). */
inline QPointF tieDirection(double angle)
{
    double r = qDegreesToRadians(angle);
    return {std::cos(r), std::sin(r)};
}

inline double dot(QPointF a, QPointF b) { return a.x() * b.x() + a.y() * b.y(); }

/** A wire from p leaving along tp to q arriving along tq, in the canvas's wire style. */
inline void bend(QPainterPath& path, QPointF p, QPointF tp, QPointF q, QPointF tq)
{
    if (gWireStyle == WireStyle::Straight)
    {
        path.lineTo(q);
        return;
    }
    if (gWireStyle == WireStyle::RightAngle)
    {
        // Out along the tangent, across, then in: a short stub each end keeps the turns clear of the sockets.
        QPointF out = p + tp * 16.0, in = q - tq * 16.0;
        path.lineTo(out);
        if (in.x() >= out.x())
        {
            double mid = (out.x() + in.x()) * 0.5;
            path.lineTo(mid, out.y());
            path.lineTo(mid, in.y());
        }
        else
        {
            double mid = (out.y() + in.y()) * 0.5;
            path.lineTo(out.x(), mid);
            path.lineTo(in.x(), mid);
        }
        path.lineTo(in);
        path.lineTo(q);
        return;
    }
    double k = std::max(24.0, QLineF(p, q).length() * 0.4);
    path.cubicTo(p + tp * k, q - tq * k, q);
}

class LinkItem final : public QGraphicsPathItem
{
public:
    LinkItem(NodeItem* from, int out, NodeItem* to, int in, QColor color = gray(165), double width = 2.0)
        : from(from), out(out), to(to), in(in), base(color, width)
    {
        setZValue(-1);
        if (from->outputs[out].field)
            base.setStyle(Qt::DashLine);
        setPen(base);
        update();
    }

    /** Folded: an end hides in a collapsed frame with the other. Filtered: the selection-only view leaves it out. */
    bool folded = false;
    bool filtered = false;
    void applyVisibility() { setVisible(!folded && !filtered); }

    LinkKey key() const { return {from->name, from->outputs[out].id, to->name, to->inputs[in].id}; }

    /** Drawn as selected: it runs through a selected reroute. */
    void setHighlight(bool on);
    bool highlight = false;

    void update();

    NodeItem* from;
    int out;
    NodeItem* to;
    int in;
    /** Where the wire runs through each of its ties, in order from its source: (entry, exit). */
    std::vector<std::pair<QPointF, QPointF>> passes;
    /** Its pen when not highlighted. */
    QPen base;
};

/** A frame's box behind the nodes it holds: a title band with its label and collapse arrow, and a resize grip. */
class FrameItem final : public QGraphicsItem
{
public:
    explicit FrameItem(int index) : index(index) { setZValue(-2.0); }

    static constexpr double kTitle = 22.0;
    int index;
    QString label;
    QSizeF size{300.0, 200.0};
    QColor color;
    bool collapsed = false;

    void setBox(QSizeF s, bool fold);
    QRectF box() const { return QRectF(0.0, 0.0, size.width(), collapsed ? kTitle : size.height()); }
    QRectF titleRect() const { return QRectF(0.0, 0.0, size.width(), kTitle); }
    QRectF arrowRect() const { return QRectF(3.0, 3.0, kTitle - 6.0, kTitle - 6.0); }
    QRectF gripRect() const { return collapsed ? QRectF() : QRectF(size.width() - 14.0, size.height() - 14.0, 14.0, 14.0); }
    QRectF boundingRect() const override { return box().adjusted(-2, -2, 2, 2); }
    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override;
};

/** A zip tie as the canvas holds it while editing. */
struct TieView
{
    std::vector<LinkKey> links;
    QPointF pos;
    double angle = 0.0;
    std::string color;
};

inline std::vector<TieView> tiesFrom(const json& def)
{
    std::vector<TieView> out;
    for (const gn::GTie& t : gn::tiesFromJson(def.value("ties", json::array())))
    {
        TieView v{{}, QPointF(t.position.x, t.position.y), t.angle, t.color};
        for (const gn::GLink& l : t.links)
            v.links.push_back({l.fromNode, l.fromSocket, l.toNode, l.toSocket});
        out.push_back(std::move(v));
    }
    return out;
}

inline json tiesJson(const std::vector<TieView>& ties)
{
    std::vector<gn::GTie> out;
    for (const TieView& v : ties)
    {
        gn::GTie t;
        for (const LinkKey& k : v.links)
            t.links.push_back({k[0], k[1], k[2], k[3]});
        t.position = {v.pos.x(), v.pos.y()};
        t.angle = v.angle;
        t.color = v.color;
        out.push_back(std::move(t));
    }
    return gn::tiesToJson(out);
}

/** Whether the theme colours its sockets; a grey theme (Monotone) gets grey tie choices instead of hues. */
inline bool chromaticTheme() { return themeColor("socketGeometry").hsvSaturationF() > 0.15f; }

struct TieSwatch
{
    const char* id;
    const char* label;
    const char* hex; // empty for a grey level
    int level = 0;
};

constexpr TieSwatch kTieHues[] = {
    {"red", "Red", "#e5534b"},    {"orange", "Orange", "#f0a33a"}, {"yellow", "Yellow", "#e6d14a"}, {"green", "Green", "#6cc24a"},
    {"teal", "Teal", "#2fb8a6"},  {"blue", "Blue", "#4a8fe5"},     {"purple", "Purple", "#a663c7"}, {"pink", "Pink", "#e56bb0"},
};
constexpr TieSwatch kTieGreys[] = {{"light", "Light", "", 215}, {"dark", "Dark", "", 85}};

/** A palette colour by name, for ties and frames; empty is the theme's grey at level. Hues stay hues only in a chromatic theme. */
inline QColor paletteColor(const std::string& id, int level)
{
    if (id.empty())
        return gray(level);
    for (const TieSwatch& s : kTieGreys)
        if (id == s.id)
            return gray(s.level);
    for (const TieSwatch& s : kTieHues)
        if (id == s.id)
        {
            QColor c(s.hex);
            return chromaticTheme() ? c : gray(qGray(c.rgb()));
        }
    return gray(level);
}

inline QColor tieColor(const std::string& id) { return paletteColor(id, 150); }

/** Default, the greys and, in a chromatic theme, the hues, each with a swatch. */
inline void fillPaletteMenu(QMenu* menu, std::function<void(std::string)> pick)
{
    auto swatch = [](const QColor& c) {
        QPixmap px(14, 14);
        px.fill(c);
        return QIcon(px);
    };
    menu->addAction(swatch(tieColor({})), "Default", menu, [pick] { pick({}); });
    for (const TieSwatch& s : kTieGreys)
        menu->addAction(swatch(tieColor(s.id)), s.label, menu, [pick, id = std::string(s.id)] { pick(id); });
    if (!chromaticTheme())
        return;
    menu->addSeparator();
    for (const TieSwatch& s : kTieHues)
        menu->addAction(swatch(tieColor(s.id)), s.label, menu, [pick, id = std::string(s.id)] { pick(id); });
}

/** The band a tie draws across its ribbon, centred on its position and turned to its angle. */
class TieItem final : public QGraphicsItem
{
public:
    explicit TieItem(int index) : index(index) { setZValue(-0.5); }

    int index;
    int count = 2;
    QColor color;
    bool hot = false;

    void setCount(int n);

    QRectF band() const;
    QRectF boundingRect() const override { return band().adjusted(-4, -8, 4, 4); }
    QPainterPath shape() const override;
    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override;
};

} // namespace canvas

using namespace canvas;


/** A wire being dragged from a socket, handed to the add-node popup so the new node connects to it. */
struct WireDrag
{
    std::string node;
    std::string socket;
    std::string type;
    bool input = false; // the loose end hangs off an input
    std::optional<std::pair<std::string, std::string>> detached; // (source, input) of a link picked up
};

/** The graph view. Builds items from the backend's GroupInfo and turns gestures into its commands. */
class NodeCanvas final : public QGraphicsView
{
public:
    /** Set by the editor before anything is built; never null after. */
    const GraphBackend* backend = nullptr;

    explicit NodeCanvas(QWidget* parent);

    std::function<void(const std::string&)> onSelect;
    std::function<void(QPointF)> onAddMenu;
    /** A fresh wire let go over empty canvas: offer the nodes it can connect to. */
    std::function<void(QPointF, const WireDrag&)> onWireDropped;
    std::function<void()> onFocused;
    /** Double-click on a Group node: the group it runs. */
    std::function<void(const std::string&)> onOpenGroup;
    /** Ctrl+Shift+click on a node: preview it in a Viewer. */
    std::function<void(const std::string&)> onViewNode;
    /** Group files (the backend's suffixes) dropped on the canvas. */
    std::function<void(const QStringList&)> onFilesDropped;

    const std::string& group() const { return mGroup; }

    /** Output sockets of a node as (id, type), for wiring helpers. */
    std::vector<std::pair<std::string, std::string>> outputsOf(const std::string& node) const;

    std::string typeOf(const std::string& node) const;

    QPointF positionOf(const std::string& node) const;

    void build(const std::string& group, const json& info, const std::set<std::string>& selected);

    /** Asks the backend for every node's thumbnail again; it calls back as newer ones come in. */
    void refreshThumbs();

    bool wireActive() const { return mWireFrom != nullptr; }

    /** Ends a wire drag without linking and returns it. A picked-up link stays hidden until the next rebuild. */
    WireDrag takeWire();

    /** The cursor in scene coordinates, or the view centre when the cursor is elsewhere. */
    QPointF cursorScenePos() const;

    std::vector<std::string> selectedNames() const;

    void selectAll();

    void nodeMoved(NodeItem* n);

    /** Held zip-tie key: a drag then gathers the wires it crosses into a tie (or into the tie it starts on). */
    void setZipKey(bool down);

    /**
     * From the selected portal node, selects and centres its Send (source) or every Receive of its name (readers). False
     * when there is none.
     */
    bool jumpToPortal(bool source);

    /** The selection's upstream and downstream drawn lit, the rest dimmed: as a toggle, or while a key is held. */
    void setFlowHighlight(bool on, bool hold);
    bool flowHighlight() const { return mFlowToggle; }

    /** Shows only the wires on the selected nodes, hiding the rest, until toggled off. */
    void setSelectedWiresOnly(bool on);
    bool selectedWiresOnly() const { return mSelectedWiresOnly; }

    enum class Align : uint8_t
    {
        Left,
        Center,
        Right,
        Top,
        Middle,
        Bottom,
    };
    std::vector<NodeItem*> selectedNodes() const;
    /** Lines the selected nodes' edges or centres up (NodeCanvasLayout.cpp). */
    void align(Align a);
    /** Spaces three or more selected nodes evenly between the outermost two. */
    void distribute(bool horizontal);
    /** Moves selected nodes up or down so the wire into each runs level, upstream first; a lone node lines up with its source. */
    void straighten();
    /**
     * Lays the selection (or, with fewer than two selected, the whole graph) out in columns from left to right by data
     * flow, ordered to keep wires short and uncrossed; frames refit around their members.
     */
    void autoLayout();

    enum class Grab : uint8_t
    {
        None,
        Move,
        Spacing,
        TieTurn,
        TieArmed, // E with no tie under the cursor: the next tie pressed turns
    };
    /**
     * W and R: the selection follows the cursor (Move) or its gaps grow and shrink from its centre (Spacing, never
     * packing nodes that started apart closer than a small gap) until a click or Enter places it; Esc or RMB puts it
     * back. X and Y lock an axis, Shift snaps (moves to the grid, spacing to tenths). One undo step (NodeCanvasGrab.cpp).
     * E (TieTurn) turns the zip tie under the cursor the same way, in 15 degree steps or freely with Ctrl. Away from
     * a tie it waits for one: pressing a tie then turns it while dragged, and letting go places it.
     *
     * action is the hotkey action that started it, empty from a menu: its key pressed again places the grab, and
     * released after a hold places it too, so a tap toggles and a hold drags.
     */
    void beginGrab(Grab mode, const std::string& action = {});
    Grab grab() const { return mGrab; }

    /** RMB on the canvas, outside ties and frames: the editor's menu of node actions. */
    std::function<void(QPoint)> onContextMenu;

    /** A frame around the selected nodes, or an empty one at the cursor when nothing is selected. */
    void frameSelection();

    const std::vector<gn::GFrame>& frames() const { return mFrames; }

    /**
     * Flips a node flag ("muted", "collapsed", "hideUnused") on the selection: on for all when any lacks it, else off.
     */
    void toggleFlag(const char* flag);

    /** The nodes as (name, what shows on the header), for the find popup. */
    std::vector<std::pair<std::string, QString>> nodeList() const;

    /** Selects a node and centres the view on it. */
    void focusNode(const std::string& name);

    /** Saves the view (centre and zoom) under a slot in the graph, or brings a saved one back. False when the slot is empty. */
    void storeBookmark(int slot);
    bool recallBookmark(int slot);

    void setMinimap(bool on);
    bool minimap() const { return mPrefs.minimap; }

    /**
     * Merges the selected nodes' outputs through the backend's merge node for their type: one multi-input node, or a
     * chain of two-input ones. Returns an error to show, empty when it worked.
     */
    QString mergeSelected(const std::string& op);

    /** Re-reads the canvas prefs and redraws with them. */
    void reloadPrefs();

    /** The ties as the canvas shows them, for tests. */
    const std::vector<TieView>& ties() const { return mTies; }

    void frame(bool selectedOnly);

protected:
    void drawForeground(QPainter* p, const QRectF&) override;

    void drawGhostLines(QPainter* p);
    /** The active grab's locked-axis line and spacing readout, in view coordinates. */
    void drawGrab(QPainter* p);
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;

    void drawBackground(QPainter* p, const QRectF& rect) override;
    /** Each zone as a tinted box around its input, output and the nodes between them, outer zones first. */
    void drawZones(QPainter* p);

    void wheelEvent(QWheelEvent* e) override;

    void focusInEvent(QFocusEvent* e) override;

    void mousePressEvent(QMouseEvent* e) override;

    void mouseMoveEvent(QMouseEvent* e) override;

    void mouseReleaseEvent(QMouseEvent* e) override;

    void mouseDoubleClickEvent(QMouseEvent* e) override;

    bool viewportEvent(QEvent* e) override;

    QStringList nodeFiles(const QMimeData* m) const;

    void dragEnterEvent(QDragEnterEvent* e) override;

    void dragMoveEvent(QDragMoveEvent* e) override;

    void dropEvent(QDropEvent* e) override;

private:
    static WireDrag wireOf(NodeItem* n, int socket, bool input);

    void removeTemp();

    static int outputIndex(NodeItem* n, const std::string& id);
    static int inputIndex(NodeItem* n, const std::string& id);

    NodeItem* nodeAt(QPointF sp) const;

    /** copy: a linked input hands out a second wire from its source and keeps its own link. */
    void startWire(NodeItem* n, int socket, bool input, bool copy = false);

    void finishWire(QPointF sp);

    /** Removes every link on a socket. Returns false when it had none. */
    bool disconnectSocket(NodeItem* n, int socket, bool input);

    void finishCut();

    /** Where a wire first crosses a straight stroke, if it does. */
    static std::optional<QPointF> crossing(const QPainterPath& path, const QLineF& stroke);

    static QPointF nearestOn(const QPainterPath& path, QPointF p);

    /** The visible wire within a few pixels of a scene point. */
    LinkItem* linkAt(QPointF sp) const;

    /** Splits each wire at its point with a new reroute, as one undo step. */
    void insertReroutes(const std::vector<std::pair<LinkItem*, QPointF>>& at);

    /**
     * The wires running through each selected reroute: up its chain of reroutes to the socket feeding it and down every
     * branch to the inputs it reaches.
     */
    std::set<LinkItem*> rerouteChains() const;

    /** The selected nodes and everything upstream and downstream of them. */
    std::set<NodeItem*> selectionFlow() const;

    /**
     * Draws wires and nodes by what matters now: wires through a selected reroute light up; with the flow highlight on,
     * the selection's upstream and downstream light up and everything else dims.
     */
    void updateEmphasis();

    /** Within reach of a wire's end, the node at its other end. */
    NodeItem* otherEnd(LinkItem* l, QPointF sp) const;

    int tieAt(QPointF sp) const;

    void hoverTie(int t);

    /**
     * Routes every tied wire through its ties. In each tie the wires sit side by side across the band, ordered by where
     * their ends lie across it so the ribbon untangles, and run kTieRun along the tie's direction.
     */
    void layoutTies();

    void dragTie(QPointF sp, Qt::KeyboardModifiers mods);

    /** A wire's ends and type, and what drives it when the backend can say cheaply. */
    QString wireTip(LinkItem* l) const;

    QString portalTip(NodeItem* n) const;

    /** Links whose wire crosses the segment a-b, in mLinks order. */
    std::vector<LinkKey> linksCrossing(QPointF a, QPointF b) const;

    void finishTieStroke(QPointF end);

    static QString linkLabel(const LinkKey& k) { return qs(k[0] + "." + k[1]) + QString("  %1  ").arg(QChar(0x2192)) + qs(k[2] + "." + k[3]); }

    void tieMenu(int t, QPoint global);

    void commitTies(const std::vector<TieView>& ties) { runVerb(*backend, "SetTies", {{"args", {mGroup, tiesJson(ties)}}}); }

    /** The one node being dragged, when it has no links yet and so can drop into a wire. */
    NodeItem* loneDraggedNode() const;

    /** Lights the wire a dragged unlinked node would drop into: the one running most through it. */
    void findInsertLink();

    /** The input and output of n that a wire would run through: the same types first, then any that link. */
    std::pair<int, int> insertSockets(NodeItem* n, LinkItem* l) const;

    /** Wires n into the link, then (with the auto-offset pref) slides what is downstream right to make room. */
    void insertOnLink(LinkItem* l);

    /** A wire's colour: its socket type's, or one grey in a grey theme when the pref asks. */
    QColor wireColor(const std::string& type) const;

    /** The minimap's box in the viewport's bottom-right corner. */
    QRect minimapRect() const;

    /** Everything on the canvas, padded. */
    QRectF graphBounds() const;

    /** Scene to minimap, keeping the aspect: the graph and the view both fit. */
    QTransform minimapTransform() const;

    QPointF fromMinimap(QPoint p) const { return minimapTransform().inverted().map(QPointF(p)); }

    void drawMinimap(QPainter* p);

    /** The topmost frame under a scene point: its title only, or anywhere in its box. */
    int frameAt(QPointF sp, bool titleOnly) const;

    /** Places the frame items and hides the nodes of collapsed frames, their wires ending on the frame's title. */
    void layoutFrames();

    void dragFrame(QPointF sp);

    void renameFrame(int f);

    void frameMenu(int f, QPoint global);

    void commitFrames(const std::vector<gn::GFrame>& frames);

    /** Moved nodes join the frame they were dropped in, and leave one they were dragged out of. */
    bool updateMembership(const json& moved, std::vector<gn::GFrame>& frames) const;

    /** One undoable MoveNodes for every node whose spot differs. */
    void moveTo(const std::map<NodeItem*, QPointF>& to, const char* label);

    /**
     * Links from to to through the best pair of sockets: same type into a free input first, then same type into a linked
     * one, then any linkable pair, earlier sockets winning ties.
     */
    void lazyConnect(NodeItem* from, NodeItem* to);

public:
    /**
     * Trades the links and values of the selected node's first two inputs of one type, such as a Math node's two
     * operands. False when it has no such pair.
     */
    bool swapInputs();

private:

    void commitMoves();

    void updateGrab(QPointF sp, Qt::KeyboardModifiers mods);
    void endGrab(bool keep);
    /** True when the grab took the key. */
    bool grabKey(QKeyEvent* e);
    bool grabKeyRelease(QKeyEvent* e);
    /** A mouse press or release during a grab; true when the grab took it. */
    bool grabPress(QMouseEvent* e);
    bool grabRelease(QMouseEvent* e);
    void turnTieAt(int tie, QPointF at);
    /** The smallest spacing factor that keeps every pair of grabbed nodes that started apart from touching. */
    double minSpacing(int axis) const;

    QGraphicsScene mScene;
    std::string mGroup;
    json mInfo;
    bool mThumbsQueued = false;
    std::map<std::string, NodeItem*> mNodes;
    std::vector<LinkItem*> mLinks;
    std::map<std::string, QPointF> mPressPositions;
    QGraphicsPathItem* mTemp = nullptr;
    NodeItem* mWireFrom = nullptr;
    int mWireSocket = -1;
    bool mWireInput = false;
    std::optional<std::pair<std::string, std::string>> mDetached; // (source, input) of a link picked up
    std::vector<QPointF> mCut;
    bool mCutting = false;
    std::vector<TieView> mTies;
    std::vector<TieItem*> mTieItems;
    bool mZipKey = false;
    bool mTieStroke = false;
    QPointF mTieStart;
    int mTieTarget = -1; // the tie a stroke started on, which it adds to
    int mTieDrag = -1;
    bool mTieRotate = false;
    QPointF mTieGrab;
    TieView mTieBefore;
    int mHotTie = -1;
    std::string mHoverPortal; // portal name whose ghost lines are drawn
    std::vector<gn::GFrame> mFrames;
    std::vector<FrameItem*> mFrameItems;
    int mFrameDrag = -1;
    bool mFrameResize = false;
    QPointF mFrameGrab;
    gn::GFrame mFrameBefore;
    std::map<std::string, QPointF> mFrameNodeStart;
    bool mFlowToggle = false;
    bool mSelectedWiresOnly = false;
    bool mFlowHold = false;
    bool mRerouteStroke = false;
    bool mMinimapDrag = false;
    NodeItem* mHoverNode = nullptr;
    NodeItem* mLazyFrom = nullptr; // Alt+RMB drag: the node to connect from
    LinkItem* mInsertLink = nullptr; // the wire a dragged node would drop into
    CanvasPrefs mPrefs = readCanvasPrefs();
    bool mPanning = false;
    bool mBuilding = false;
    Grab mGrab = Grab::None;
    std::map<NodeItem*, QPointF> mGrabStart;
    QPointF mGrabAnchor, mGrabCentre;
    QPoint mGrabCursor;
    int mGrabAxis = 0; // 0 free, 1 X, 2 Y
    double mGrabFactor = 1.0;
    bool mGrabEnded = false; // swallows the release of the click that placed a grab
    std::string mGrabAction;
    bool mGrabDragging = false; // a tie picked by a press turns until the button lets go
    std::optional<QPointF> mHoverScene; // the last cursor position over the canvas, kept from mouse moves
    QElapsedTimer mGrabHeld; // since the grab began, to tell a hold from a tap
    int mGrabTie = -1;
    TieView mGrabTieBefore;
    std::optional<double> mGrabTurnFrom; // the cursor's angle around the tie once it first clears the middle
    QPoint mLast;
};
} // namespace lila::ui
