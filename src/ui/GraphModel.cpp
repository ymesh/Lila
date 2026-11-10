#include "ui/GraphModel.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "scene/Anim.h"
#include "ui/Actions.h"
#include "ui/GraphEditor.h"
#include "ui/Icons.h"
#include "ui/MarkingMenu.h"
#include "ui/UiCommon.h"
#include "ui/ViewportWidget.h"

#include <QApplication>
#include <QMenu>

#include <algorithm>
#include <array>

namespace lila::ui::graphed
{
using scene::Graph;

namespace
{
double gmFps() { return anim::playback().fps > 0.0 ? anim::playback().fps : 30.0; }
double gmDegrees(double slope) { return std::atan(slope) * 180.0 / 3.14159265358979323846; }
double gmRadians(double degrees) { return degrees * 3.14159265358979323846 / 180.0; }

/** Previews only dirty the curves, which sends no graph change, so the viewports are asked for a frame directly. */
void redrawAllViewports()
{
    for (QWidget* top : QApplication::topLevelWidgets())
        for (ViewportWidget* v : top->findChildren<ViewportWidget*>())
            if (v->isVisible())
                v->requestFrame();
}

QString curveUnit(const scene::Node& curve)
{
    const std::string& t = curve.type().name;
    if (t == "animCurveTA" || t == "animCurveRQ")
        return QStringLiteral("°");
    if (t == "animCurveTL")
        return QStringLiteral("cm");
    return {};
}

GraphModel*& menuModel()
{
    static GraphModel* m = nullptr;
    return m;
}
} // namespace

SelectOp selectOpFor(bool shift, bool ctrl)
{
    if (shift && ctrl)
        return SelectOp::Add;
    if (shift)
        return SelectOp::Toggle;
    return ctrl ? SelectOp::Remove : SelectOp::Replace;
}

GraphModel::~GraphModel()
{
    if (menuModel() == this)
        menuModel() = nullptr;
}

std::vector<Channel> GraphModel::gather(Graph& g, const std::vector<NodeId>& nodes, RotationDisplay rotation)
{
    std::vector<Channel> channels;
    for (NodeId id : nodes)
    {
        if (!g.node(id))
            continue;
        auto addChannel = [&](const anim::AnimatedPlug& ap, NodeId cid, const QString& label, int axis) {
            Channel ch;
            ch.plug = ap.plug;
            ch.curve = cid;
            scene::Node* curveNode = g.node(ap.curve);
            ch.curveName = curveNode->path();
            ch.label = label;
            ch.unit = slotOf(cid) >= 3 ? QString() : curveUnit(*curveNode);
            ch.axis = axis;
            ch.modifiers = anim::modifiersOf(*curveNode);
            // Neighbouring channels never share a look: gray level and dash both cycle, at different periods.
            static constexpr int kLevels[] = {210, 150, 180, 120};
            static constexpr Qt::PenStyle kDashes[] = {Qt::SolidLine, Qt::DashLine, Qt::DotLine};
            ch.level = kLevels[channels.size() % 4];
            ch.dash = kDashes[channels.size() % 3];
            channels.push_back(std::move(ch));
        };
        for (const anim::AnimatedPlug& ap : anim::animatedPlugs(g, id))
        {
            const scene::AttrDef& a = g.attr(ap.plug);
            const bool isRotation = anim::isRotationCurve(*g.node(ap.curve));
            const std::string attr = ap.plug.component >= 0 ? a.childNames[size_t(ap.plug.component)] : a.name;
            if (!isRotation || rotation != RotationDisplay::Quaternion)
                addChannel(ap, isRotation ? slotId(ap.curve, ap.plug.component) : ap.curve, niceName(attr), [&] {
                    if (ap.plug.component < 0 || a.childNames.size() != 3 || a.childNames[size_t(ap.plug.component)].empty())
                        return -1;
                    char last = char(std::toupper(static_cast<unsigned char>(a.childNames[size_t(ap.plug.component)].back())));
                    return last == 'X' + ap.plug.component ? ap.plug.component : -1;
                }());
            if (isRotation && rotation != RotationDisplay::Euler && ap.plug.component == 2)
                for (int s = 3; s < 7; ++s)
                    addChannel(ap, slotId(ap.curve, s), niceName(a.name) + " " + QChar("WXYZ"[s - 3]), -1);
        }
    }
    return channels;
}

void GraphModel::reloadCurves()
{
    Graph& g = Graph::current();
    curves.clear();
    rotations.clear();
    loopCurves.clear();
    for (Channel& ch : channels)
    {
        scene::Node* n = g.node(realCurve(ch.curve));
        if (!n)
            continue;
        ch.modifiers = anim::modifiersOf(*n);
        if (anim::curveLoops(*n))
            loopCurves.insert(ch.curve);
        try
        {
            const int slot = slotOf(ch.curve);
            if (slot < 0)
            {
                curves[ch.curve] = anim::curveOf(*n);
                continue;
            }
            auto it = rotations.find(n->id());
            if (it == rotations.end())
                it = rotations.emplace(n->id(), anim::rotationCurveOf(*n).looped()).first;
            curves[ch.curve] = derivedChannel(it->second, slot);
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", n->path(), e.what());
        }
    }
    dropMissing();
}

std::string GraphModel::commandTarget(NodeId id) const
{
    Graph& g = Graph::current();
    const int slot = slotOf(id);
    if (slot < 0)
    {
        scene::Node* n = g.node(id);
        return n ? n->path() : std::string();
    }
    if (slot > 2 || !g.node(realCurve(id)))
        return {};
    const Channel* ch = channelOf(id);
    return ch ? g.plugName(ch->plug) : std::string();
}

const Channel* GraphModel::channelOf(NodeId curve) const
{
    for (const Channel& c : channels)
        if (c.curve == curve)
            return &c;
    return nullptr;
}

bool GraphModel::isSelected(NodeId curve, double t) const
{
    for (const KeySel& k : sel)
        if (k.curve == curve && std::abs(k.time - t) <= kEps)
            return true;
    return false;
}

std::vector<double> GraphModel::selectedTimes(NodeId id) const
{
    std::vector<double> out;
    for (const KeySel& k : sel)
        if (k.curve == id)
            out.push_back(k.time);
    return out;
}

std::vector<NodeId> GraphModel::targetCurves() const
{
    std::vector<NodeId> out;
    for (const KeySel& k : sel)
        if (std::find(out.begin(), out.end(), k.curve) == out.end())
            out.push_back(k.curve);
    if (out.empty() && activeCurve && channelOf(activeCurve))
        out.push_back(activeCurve);
    if (out.empty())
        for (const Channel& c : channels)
            if (c.shown)
                out.push_back(c.curve);
    return out;
}

std::vector<NodeId> GraphModel::activeSet() const
{
    std::vector<NodeId> out;
    for (const KeySel& k : sel)
        if (std::find(out.begin(), out.end(), k.curve) == out.end())
            out.push_back(k.curve);
    if (activeCurve && std::find(out.begin(), out.end(), activeCurve) == out.end())
        out.push_back(activeCurve);
    return out;
}

void GraphModel::dropMissing()
{
    std::erase_if(sel, [&](const KeySel& k) {
        auto it = curves.find(k.curve);
        const Channel* ch = channelOf(k.curve);
        return it == curves.end() || it->second.find(k.time) < 0 || !ch || !ch->shown;
    });
    if (activeCurve && !channelOf(activeCurve))
        activeCurve = scene::kNoNode;
}

json GraphModel::keysArgument() const
{
    std::map<NodeId, std::vector<double>> byCurve;
    for (const KeySel& k : sel)
        byCurve[k.curve].push_back(k.time);
    json keys = json::array();
    for (auto& [id, times] : byCurve)
        if (std::string target = commandTarget(id); !target.empty())
            keys.push_back({{"curve", target}, {"times", times}});
    return keys;
}

json GraphModel::targetNames() const
{
    json names = json::array();
    for (NodeId id : targetCurves())
        if (std::string target = commandTarget(id); !target.empty() && std::find(names.begin(), names.end(), target) == names.end())
            names.push_back(target);
    return names;
}

double GraphModel::defaultFor(const Channel& ch) const
{
    Graph& g = Graph::current();
    if (!ch.plug.node || !g.node(ch.plug.node))
        return 0.0;
    const scene::PlugValue& d = g.attr(ch.plug).defaultValue;
    if (const auto* v = std::get_if<double>(&d))
        return *v;
    if (const auto* v = std::get_if<glm::dvec3>(&d))
        return (*v)[std::clamp(ch.plug.component, 0, 2)];
    return 0.0;
}

void GraphModel::select(std::span<const KeySel> keys, SelectOp op)
{
    if (op == SelectOp::Replace)
        sel.clear();
    for (const KeySel& k : keys)
    {
        auto same = [&](const KeySel& s) { return s.curve == k.curve && std::abs(s.time - k.time) <= kEps; };
        const bool was = std::any_of(sel.begin(), sel.end(), same);
        if (op == SelectOp::Remove || (op == SelectOp::Toggle && was))
            std::erase_if(sel, same);
        else if (!was)
            sel.push_back(k);
    }
    selectionChanged();
}

void GraphModel::selectionChanged()
{
    if (onSelectionChanged)
        onSelectionChanged();
}

void GraphModel::applyTangent(const char* type)
{
    const bool step = std::string_view(type) == "step";
    {
        UndoChunk chunk("Key Tangents");
        if (!sel.empty())
        {
            for (const KeySel& k : sel)
                if (std::string name = commandTarget(k.curve); !name.empty())
                {
                    json call = {{"args", {name}}, {"time", k.time}, {"outTangentType", type}};
                    if (!step)
                        call["inTangentType"] = type;
                    runUnechoed("keyTangent", call);
                }
        }
        else
            for (NodeId c : targetCurves())
                if (std::string name = commandTarget(c); !name.empty())
                {
                    json call = {{"args", {name}}, {"outTangentType", type}};
                    if (!step)
                        call["inTangentType"] = type;
                    runUnechoed("keyTangent", call);
                }
    }
    log::write(LogLevel::Echo, std::string("# tangents: ") + type + "\n");
}

void GraphModel::lockTangents(bool lock)
{
    if (sel.empty())
    {
        log::warn("{} tangents: select keys first", lock ? "Unify" : "Break");
        return;
    }
    {
        UndoChunk chunk(lock ? "Unify Tangents" : "Break Tangents");
        for (const KeySel& k : sel)
            if (std::string name = commandTarget(k.curve); !name.empty())
                runUnechoed("keyTangent", {{"args", {name}}, {"time", k.time}, {"lock", lock}});
    }
    log::write(LogLevel::Echo, std::string("# tangents ") + (lock ? "unified\n" : "broken\n"));
}

void GraphModel::setWeighted(bool weighted)
{
    UndoChunk chunk(weighted ? "Weighted Tangents" : "Non-Weighted Tangents");
    for (NodeId c : targetCurves())
        if (std::string name = commandTarget(c); !name.empty())
            runUnechoed("keyTangent", {{"args", {name}}, {"weightedTangents", weighted}});
}

size_t GraphModel::deleteSelectedKeys()
{
    if (sel.empty())
        return 0;
    std::vector<KeySel> doomed = sel;
    sel.clear();
    {
        UndoChunk chunk("Delete Keys");
        for (const KeySel& k : doomed)
            if (std::string name = commandTarget(k.curve); !name.empty())
                runUnechoed("cutKey", {{"args", {name}}, {"time", k.time}, {"clear", true}});
    }
    log::write(LogLevel::Echo, "# deleted " + std::to_string(doomed.size()) + " keys\n");
    selectionChanged();
    return doomed.size();
}

anim::KeyTransform GraphModel::transformFor(NodeId id) const
{
    anim::KeyTransform x = edit.xform;
    const Channel* ch = channelOf(id);
    x.valueOffset = ch ? edit.displayOffset / ch->mapScale : edit.displayOffset;
    auto it = edit.valuePivots.find(id);
    x.valuePivot = it != edit.valuePivots.end() ? it->second : 0.0;
    x.falloff = edit.falloff;
    x.snap = std::abs(x.timeScale - 1.0) > 1e-12;
    return x;
}

double GraphModel::keyTimeBeforeDrag(NodeId curve, double t) const
{
    if (!transforming())
        return t;
    const anim::KeyTransform x = transformFor(curve);
    for (const KeySel& k : sel)
        if (k.curve == curve && std::abs(x.time(k.time, gmFps()) - t) <= kEps)
            return k.time;
    return t + 1e9;
}

double GraphModel::falloffWeightAt(NodeId id, double t) const
{
    if (!transforming() || edit.falloff <= 0.0 || isSelected(id, t))
        return 0.0;
    double nearest = 1e300;
    for (const KeySel& k : sel)
        if (k.curve == id)
            nearest = std::min(nearest, std::abs(k.time - t));
    return anim::falloffWeight(nearest, edit.falloff);
}

anim::RotationCurve GraphModel::previewRotation(NodeId realId) const
{
    anim::RotationCurve rc = rotations.at(realId);
    if (transforming())
    {
        std::array<std::vector<double>, 3> times;
        std::array<anim::KeyTransform, 3> xs;
        for (int a = 0; a < 3; ++a)
        {
            times[size_t(a)] = selectedTimes(slotId(realId, a));
            xs[size_t(a)] = transformFor(slotId(realId, a));
        }
        anim::transformKeys(rc, times, xs, gmFps());
    }
    if (tweening())
        for (int a = 0; a < 3; ++a)
            if (const Channel* ch = channelOf(slotId(realId, a)))
                if (std::vector<double> times = selectedTimes(ch->curve); !times.empty())
                    anim::blendKeys(rc, a, times, edit.tweenMode, edit.tweenAmount, defaultFor(*ch));
    return rc;
}

anim::Curve GraphModel::preview(NodeId id) const
{
    const int slot = slotOf(id);
    anim::Curve c;
    if (slot >= 0 && (transforming() || tweening()) && rotations.contains(realCurve(id)))
        c = derivedChannel(previewRotation(realCurve(id)), slot);
    else
    {
        c = curves.at(id);
        if (slot < 0 && transforming())
            anim::transformKeys(c, selectedTimes(id), transformFor(id), gmFps());
        if (slot < 0 && tweening())
            if (const Channel* ch = channelOf(id); ch && !selectedTimes(id).empty())
                anim::blendKeys(c, selectedTimes(id), edit.tweenMode, edit.tweenAmount, defaultFor(*ch));
    }
    if (edit.kind == KeyEdit::Kind::Tangent && edit.tangent.curve == id && edit.moved)
        if (int i = c.find(edit.tangent.time); i >= 0)
        {
            anim::Key& k = c.keys[size_t(i)];
            const bool out = edit.tangentSide > 0;
            if (!std::isnan(edit.tangentAngle))
            {
                if (k.broken)
                {
                    const double other = out ? c.inSlope(size_t(i)) : c.outSlope(size_t(i));
                    k.in = k.out = anim::Tangent::Fixed;
                    (out ? k.outAngle : k.inAngle) = edit.tangentAngle;
                    (out ? k.inAngle : k.outAngle) = gmDegrees(other);
                }
                else
                {
                    k.in = k.out = anim::Tangent::Fixed;
                    k.inAngle = k.outAngle = edit.tangentAngle;
                }
            }
            if (!std::isnan(edit.tangentWeight))
                (out ? k.outWeight : k.inWeight) = edit.tangentWeight;
        }
    if (edit.kind == KeyEdit::Kind::TangentRotate && edit.moved)
        if (const Channel* ch = channelOf(id))
            for (size_t i = 0; i < c.keys.size(); ++i)
                if (isSelected(id, c.keys[i].time))
                {
                    const double in = c.inSlope(i), out = c.outSlope(i);
                    anim::Key& k = c.keys[i];
                    k.in = k.out = anim::Tangent::Fixed;
                    k.inAngle = gmDegrees(slopeOfScreenAngle(*ch, screenAngleOf(*ch, in) + edit.rotateDelta));
                    k.outAngle = gmDegrees(slopeOfScreenAngle(*ch, screenAngleOf(*ch, out) + edit.rotateDelta));
                }
    return c;
}

std::function<double(double)> GraphModel::sampler(NodeId id, const anim::Curve& c) const
{
    const Channel* ch = channelOf(id);
    std::vector<anim::Modifier> mods = ch ? ch->modifiers : std::vector<anim::Modifier>{};
    const int slot = slotOf(id);
    auto it = rotations.find(realCurve(id));
    const bool tangentEdit = (edit.kind == KeyEdit::Kind::Tangent && edit.tangent.curve == id) ||
                             (edit.kind == KeyEdit::Kind::TangentRotate && !selectedTimes(id).empty());
    if (slot < 0 || it == rotations.end() || (slot < 3 && tangentEdit && edit.moved))
    {
        if (mods.empty())
            return [c](double t) { return c.evaluate(t); };
        return [c, mods](double t) { return anim::evaluateModified(c, mods, t); };
    }
    anim::RotationCurve rc = (transforming() || tweening()) ? previewRotation(realCurve(id)) : it->second;
    if (slot < 3)
    {
        if (mods.empty())
            return [rc, slot](double t) { return rc.evaluate(t)[slot]; };
        return [rc, slot, mods](double t) { return anim::evaluateModified(rc, mods, t)[slot]; };
    }
    glm::dquat ref = rc.keys.empty() ? glm::dquat(1.0, 0.0, 0.0, 0.0) : rc.keys.front().q;
    return [rc, slot, ref](double t) {
        glm::dquat q = rc.evaluateQuat(t);
        return quatComponent(glm::dot(q, ref) < 0.0 ? -q : q, slot - 3);
    };
}

double GraphModel::screenAngleOf(const Channel& ch, double slope) const
{
    return gmDegrees(slope * ch.mapScale * edit.aspect);
}

double GraphModel::slopeOfScreenAngle(const Channel& ch, double degrees) const
{
    return std::tan(gmRadians(std::clamp(degrees, -89.9, 89.9))) / (edit.aspect * ch.mapScale);
}

void GraphModel::pushPreviews()
{
    Graph& g = Graph::current();
    std::set<NodeId> rotationsDone;
    const bool tangentDrag = edit.kind == KeyEdit::Kind::Tangent || edit.kind == KeyEdit::Kind::TangentRotate;
    for (const KeySel& k : sel)
    {
        const NodeId real = realCurve(k.curve);
        if (!g.node(real))
            continue;
        if (slotOf(k.curve) < 0)
        {
            if (curves.contains(k.curve))
                anim::setPreview(g, real, preview(k.curve));
            continue;
        }
        // Tangent drags on a rotation channel edit only the drawn Euler channel; the scene follows on release.
        if (!tangentDrag && rotations.contains(real) && rotationsDone.insert(real).second)
            anim::setPreview(g, real, previewRotation(real));
    }
    redrawAllViewports();
}

void GraphModel::dropPreviews()
{
    anim::clearPreviews(Graph::current());
    redrawAllViewports();
}

void GraphModel::cancel()
{
    dropPreviews();
    edit = {};
}

bool GraphModel::commit()
{
    dropPreviews();
    bool done = false;
    switch (edit.kind)
    {
    case KeyEdit::Kind::Transform: done = commitTransform(); break;
    case KeyEdit::Kind::Tangent: done = commitTangent(); break;
    case KeyEdit::Kind::TangentRotate: done = commitTangentRotate(); break;
    case KeyEdit::Kind::Tween: done = commitTween(); break;
    case KeyEdit::Kind::None: break;
    }
    edit = {};
    return done;
}

bool GraphModel::commitTransform()
{
    if (!edit.moved)
        return false;
    const anim::KeyTransform base = edit.xform;
    std::map<NodeId, anim::KeyTransform> perCurve;
    for (const KeySel& k : sel)
        perCurve.try_emplace(k.curve, transformFor(k.curve));
    const bool any = base.timeOffset != 0.0 || std::abs(base.timeScale - 1.0) > 1e-12 || std::abs(base.valueScale - 1.0) > 1e-12 ||
                     edit.displayOffset != 0.0;
    if (!any)
        return false;
    std::map<NodeId, std::vector<double>> byCurve;
    for (const KeySel& k : sel)
        byCurve[k.curve].push_back(k.time);
    json keys = json::array();
    for (auto& [id, times] : byCurve)
    {
        std::string target = commandTarget(id);
        if (target.empty())
            continue;
        const anim::KeyTransform& x = perCurve[id];
        json entry = {{"curve", target}, {"times", times}};
        if (x.valueOffset != 0.0)
            entry["valueOffset"] = x.valueOffset;
        if (std::abs(x.valueScale - 1.0) > 1e-12)
            entry["valuePivot"] = x.valuePivot;
        keys.push_back(std::move(entry));
    }
    if (keys.empty())
        return false;
    json call = {{"keys", keys}};
    if (base.timeOffset != 0.0)
        call["timeOffset"] = base.timeOffset;
    if (std::abs(base.timeScale - 1.0) > 1e-12)
    {
        call["timeScale"] = base.timeScale;
        call["timePivot"] = base.timePivot;
        call["snap"] = true;
    }
    if (std::abs(base.valueScale - 1.0) > 1e-12)
        call["valueScale"] = base.valueScale;
    if (edit.falloff > 0.0)
        call["falloff"] = edit.falloff;
    try
    {
        ops::execute("transformKeys", call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return false;
    }
    for (KeySel& k : sel)
        k.time = perCurve[k.curve].time(k.time, gmFps());
    selectionChanged();
    return true;
}

bool GraphModel::commitTangent()
{
    if (!edit.moved || (std::isnan(edit.tangentAngle) && std::isnan(edit.tangentWeight)))
        return false;
    std::string name = commandTarget(edit.tangent.curve);
    auto it = curves.find(edit.tangent.curve);
    if (name.empty() || it == curves.end())
        return false;
    const int i = it->second.find(edit.tangent.time);
    if (i < 0)
        return false;
    const anim::Key& k = it->second.keys[size_t(i)];
    const bool out = edit.tangentSide > 0;
    json call = {{"args", {name}}, {"time", edit.tangent.time}};
    if (!std::isnan(edit.tangentAngle))
    {
        if (k.broken)
            call[out ? "outAngle" : "inAngle"] = edit.tangentAngle;
        else
            call["inAngle"] = call["outAngle"] = edit.tangentAngle;
    }
    if (!std::isnan(edit.tangentWeight))
        call[out ? "outWeight" : "inWeight"] = edit.tangentWeight;
    try
    {
        ops::execute("keyTangent", call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return false;
    }
    return true;
}

bool GraphModel::commitTangentRotate()
{
    if (!edit.moved || std::abs(edit.rotateDelta) < 1e-6)
        return false;
    int count = 0;
    {
        UndoChunk chunk("Rotate Tangents");
        for (const KeySel& k : sel)
        {
            auto it = curves.find(k.curve);
            const Channel* ch = channelOf(k.curve);
            std::string name = commandTarget(k.curve);
            if (it == curves.end() || !ch || name.empty())
                continue;
            const int i = it->second.find(k.time);
            if (i < 0)
                continue;
            const double in = it->second.inSlope(size_t(i)), out = it->second.outSlope(size_t(i));
            const double inAngle = gmDegrees(slopeOfScreenAngle(*ch, screenAngleOf(*ch, in) + edit.rotateDelta));
            const double outAngle = gmDegrees(slopeOfScreenAngle(*ch, screenAngleOf(*ch, out) + edit.rotateDelta));
            count += runUnechoed("keyTangent", {{"args", {name}}, {"time", k.time}, {"inAngle", inAngle}, {"outAngle", outAngle}}) ? 1 : 0;
        }
    }
    log::write(LogLevel::Echo, "# rotated " + std::to_string(count) + " tangents\n");
    return count > 0;
}

bool GraphModel::commitTween()
{
    if (std::abs(edit.tweenAmount) <= 1e-6)
        return false;
    try
    {
        ops::execute("blendKeys", {{"keys", keysArgument()}, {"mode", anim::blendModeName(edit.tweenMode)}, {"amount", edit.tweenAmount}},
                     CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return false;
    }
    return true;
}

void fillTangentMenu(QMenu* menu, GraphModel& model)
{
    for (const char* t : kTangentTypes)
        menu->addAction(icon(QString("curve-tangent-%1").arg(t)), niceName(t), [&model, t] { model.applyTangent(t); });
    menu->addSeparator();
    menu->addAction(icon("unlink"), "Break", [&model] { model.lockTangents(false); })->setEnabled(!model.sel.empty());
    menu->addAction(icon("link"), "Unify", [&model] { model.lockTangents(true); })->setEnabled(!model.sel.empty());
    menu->addSeparator();
    menu->addAction("Weighted", [&model] { model.setWeighted(true); });
    menu->addAction("Non-Weighted", [&model] { model.setWeighted(false); });
}

bool popupTangentMarkingMenu(GraphModel& model, QPoint global, Qt::MouseButton button)
{
    menuModel() = &model;
    return MarkingMenu::popup(kTangentMarkingMenu, MarkingMenu::Trigger::MouseButton, button, global) != nullptr;
}

GraphModel* tangentTarget()
{
    if (GraphModel* m = menuModel())
        return m;
    GraphEditor* ge = GraphEditor::instance();
    return ge ? &ge->model() : nullptr;
}

void registerTangentActions()
{
    auto add = [](std::string id, std::string label, std::function<void(GraphModel&)> fn, std::string doc, std::string iconName) {
        actions::add({.id = std::move(id),
                      .label = std::move(label),
                      .category = "Graph Editor",
                      .run =
                          [fn = std::move(fn)] {
                              if (GraphModel* m = tangentTarget())
                                  fn(*m);
                          },
                      .doc = std::move(doc),
                      .icon = std::move(iconName)});
    };
    for (const char* t : kTangentTypes)
        add(std::string("graphTangents.") + t, niceName(t).toStdString() + " Tangents", [t](GraphModel& m) { m.applyTangent(t); },
            "Set on the selected keys, or every key of the shown curves.", std::string("curve-tangent-") + t);
    add("graphTangents.break", "Break Tangents", [](GraphModel& m) { m.lockTangents(false); }, "Let the selected keys' in and out handles turn apart.",
        "unlink");
    add("graphTangents.unify", "Unify Tangents", [](GraphModel& m) { m.lockTangents(true); }, "Turn the selected keys' in and out handles together.",
        "link");
}
} // namespace lila::ui::graphed
