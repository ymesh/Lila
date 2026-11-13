// The hand rig: a puppeteerHand node per hand of a character, holding where its fingers are and each finger joint's
// anatomical frame, and the commands that pose fingers through those frames. Poses land on the joints' rotate (or a
// rubber hose finger's curl and spread), so keys are plain joint keys and export as they are.

#include "hand/Hands.h"

#include "Puppeteer.h"
#include "rig/Rig.h"

#include <lila/Mesh.h>
#include <lila/Poses.h>
#include <lila/Prefs.h>
#include <lila/Relocate.h>
#include <lila/SplineChain.h>
#include <lila/Undo.h>

#include <algorithm>
#include <map>
#include <set>

using namespace lila;
using namespace lila::scene;

namespace puppeteer::hand
{
namespace
{
constexpr const char* kPosesKey = "/hands/poses";
const std::vector<std::string> kSides = {"center", "left", "right"};

json vec(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }
glm::dvec3 vecFrom(const json& j) { return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()}; }
json mat(const glm::dmat3& m)
{
    json a = json::array();
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            a.push_back(m[c][r]);
    return a;
}
glm::dmat3 matFrom(const json& j)
{
    glm::dmat3 m(1.0);
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            m[c][r] = j.at(size_t(c * 3 + r)).get<double>();
    return m;
}
json frameJson(const Frame& f) { return {{"axes", mat(f.axes)}, {"zero", mat(f.zero)}, {"splaySign", f.splaySign}, {"twistSign", f.twistSign}}; }
Frame frameFrom(const json& j)
{
    Frame f;
    f.axes = matFrom(j.at("axes"));
    f.zero = matFrom(j.at("zero"));
    f.splaySign = j.value("splaySign", 1.0);
    f.twistSign = j.value("twistSign", -1.0);
    return f;
}
json anglesJson(const Angles& a) { return json::array({a.bend, a.splay, a.twist}); }
Angles anglesFrom(const json& j) { return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()}; }

glm::dmat3 rotOf(Graph& g, NodeId id) { return math::rotationOf(worldOf(g, id)); }
glm::dmat3 localNow(Graph& g, NodeId id) { return glm::transpose(math::rotationOf(parentWorldOf(g, id))) * rotOf(g, id); }
glm::dmat3 rotateAxisOf(Graph& g, NodeId id)
{
    return glm::dmat3(eulerToMatrix(vecOf(g, id, Transform::kRotateAxis), RotateOrder::XYZ));
}

double num(Graph& g, NodeId n, const char* attr)
{
    const Node* node = g.node(n);
    const int a = node ? attrOf(*node, attr) : -1;
    return a < 0 ? 0.0 : std::get<double>(g.value({n, a, -1}));
}

/** What a hose finger's curl and spread add up to through its hand's drivers, as the hose node works it out. */
struct HoseState
{
    double curl, spread, handCurl, handSpread, fist, fistCurl, spreadWeight, curlMax, spreadMax;
    double effectiveCurl() const { return glm::mix(curl + handCurl, fistCurl, std::clamp(fist, 0.0, 1.0)); }
    double effectiveSpread() const { return (spread + handSpread * spreadWeight) * (1.0 - std::clamp(fist, 0.0, 1.0)); }
};
HoseState hoseState(Graph& g, NodeId hose)
{
    return {num(g, hose, "curl"),     num(g, hose, "spread"),   num(g, hose, "handCurl"),
            num(g, hose, "handSpread"), num(g, hose, "fist"),   num(g, hose, "fistCurl"),
            num(g, hose, "spreadWeight"), std::max(num(g, hose, "curlMax"), 1e-6), std::max(num(g, hose, "spreadMax"), 1e-6)};
}

/** The hoseLimb whose chain lays the finger starting at base, or none. */
NodeId hoseOfFinger(Graph& g, NodeId base)
{
    const NodeId chain = spline::splineChainContaining(g, base);
    if (!chain)
        return kNoNode;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA(spline::kHoseType) && spline::hoseChain(g, id) == chain)
            return id;
    return kNoNode;
}

/** Joints at and under root, parents before children, as the detection reads them. */
std::vector<SkelJoint> skeletonOf(Graph& g, NodeId root, std::vector<NodeId>& ids)
{
    g.evaluate();
    std::vector<SkelJoint> out;
    std::map<NodeId, int> index;
    std::vector<NodeId> order{root};
    for (NodeId d : g.descendants(root))
        order.push_back(d);
    for (NodeId id : order)
    {
        Node* n = g.node(id);
        if (!n || !rig::isJoint(*n))
            continue;
        int parent = -1;
        for (NodeId p = g.parent(id); p; p = g.parent(p))
            if (auto it = index.find(p); it != index.end())
            {
                parent = it->second;
                break;
            }
        index[id] = int(out.size());
        ids.push_back(id);
        const glm::dmat4 w = worldOf(g, id);
        out.push_back({lila::scene::poses::stripNamespace(n->name()), parent, glm::dvec3(w[3]), math::rotationOf(w)});
    }
    return out;
}

/** The hose's curl and spread signs: which way each turns the finger, found by nudging it. */
std::pair<double, double> hoseSigns(Graph& g, NodeId hose, NodeId tipJoint, const glm::dvec3& palm, const glm::dvec3& thumbSide)
{
    UndoStack& u = UndoStack::get();
    const bool was = u.enabled();
    u.setEnabled(false);
    auto probe = [&](const char* attr) {
        const PlugRef p = plugOf(*g.node(hose), attr);
        const double v = std::get<double>(g.value(p));
        g.evaluate();
        const glm::dvec3 a(worldOf(g, tipJoint)[3]);
        g.setValue(p, v + 0.1);
        g.evaluate();
        const glm::dvec3 b(worldOf(g, tipJoint)[3]);
        g.setValue(p, v);
        g.evaluate();
        return b - a;
    };
    const glm::dvec3 dc = probe("curl"), ds = probe("spread");
    u.setEnabled(was);
    return {glm::dot(dc, palm) >= 0.0 ? 1.0 : -1.0, glm::dot(ds, thumbSide) >= 0.0 ? 1.0 : -1.0};
}

std::string pathOf(Graph& g, NodeId id) { return id ? g.node(id)->path() : std::string(); }
NodeId idOf(Graph& g, const json& j)
{
    if (!j.is_string())
        return kNoNode;
    Node* n = named(g, j.get<std::string>());
    return n ? n->id() : kNoNode;
}
} // namespace

bool Hand::hasMeta() const
{
    return std::any_of(fingers.begin(), fingers.end(), [](const Finger& f) { return f.meta != kNoNode; });
}
const Finger* Hand::finger(Role r) const
{
    for (const Finger& f : fingers)
        if (f.role == r)
            return &f;
    return nullptr;
}

json poseJson(const Pose& p)
{
    json out = json::object();
    for (int r = 0; r < kRoles; ++r)
        if (p.fingers[r])
        {
            json js = json::array();
            for (const Angles& a : p.fingers[r]->joints)
                js.push_back(anglesJson(a));
            out[roleName(Role(r))] = {{"meta", anglesJson(p.fingers[r]->meta)}, {"joints", js}};
        }
    return out;
}

Pose poseFromJson(const json& j)
{
    Pose p;
    if (!j.is_object())
        return p;
    for (int r = 0; r < kRoles; ++r)
        if (auto it = j.find(roleName(Role(r))); it != j.end() && it->is_object())
        {
            FingerPose f;
            if (it->contains("meta"))
                f.meta = anglesFrom(it->at("meta"));
            const json& js = it->value("joints", json::array());
            for (size_t k = 0; k < js.size() && k < size_t(kCanon); ++k)
                f.joints[k] = anglesFrom(js[k]);
            p.fingers[r] = f;
        }
    return p;
}

Mask maskFromJson(const json& j)
{
    if (j.is_null() || (j.is_array() && j.empty()))
        return allFingers();
    Mask m{};
    auto one = [&](const std::string& s) {
        if (const std::optional<Role> r = roleFromName(lowerOf(s)))
            m[int(*r)] = true;
        else
            throw CommandError("unknown finger '" + s + "': use thumb, index, middle, ring or pinky");
    };
    if (j.is_string())
        one(j.get<std::string>());
    else
        for (const json& s : j)
            one(s.get<std::string>());
    return m;
}

std::vector<NodeId> handsOf(Graph& g, NodeId character)
{
    std::vector<NodeId> out;
    if (!character)
        return out;
    for (NodeId id : g.descendants(character))
        if (Node* n = g.node(id); n && n->type().isA(kHandType))
            out.push_back(id);
    std::sort(out.begin(), out.end(), [&](NodeId a, NodeId b) {
        const int64_t sa = std::get<int64_t>(g.value(plugOf(*g.node(a), "side"))), sb = std::get<int64_t>(g.value(plugOf(*g.node(b), "side")));
        return sa != sb ? sa < sb : a < b;
    });
    return out;
}

std::optional<Hand> load(Graph& g, NodeId handNode)
{
    Node* n = g.node(handNode);
    if (!n || !n->type().isA(kHandType))
        return std::nullopt;
    json s;
    try
    {
        s = json::parse(textOf(*n, "setup"));
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
    Hand h;
    h.node = handNode;
    h.character = characterFor(g, handNode);
    h.joint = idOf(g, s.value("hand", json()));
    if (!h.joint)
        return std::nullopt;
    h.side = Side(std::get<int64_t>(g.value(plugOf(*n, "side"))));
    h.sideFrom = s.value("sideFrom", "");
    h.palm = vecFrom(s.at("palm"));
    h.forward = vecFrom(s.at("forward"));
    h.thumbSide = vecFrom(s.at("thumbSide"));
    for (const json& fj : s.value("fingers", json::array()))
    {
        Finger f;
        f.role = roleFromName(fj.value("role", "index")).value_or(Role::Index);
        f.roleFrom = fj.value("roleFrom", "");
        if (const json& m = fj.value("meta", json()); m.is_object())
        {
            f.meta = idOf(g, m.value("joint", json()));
            f.metaFrame = frameFrom(m.at("frame"));
            if (!f.meta)
                return std::nullopt;
        }
        for (const json& jj : fj.value("joints", json::array()))
        {
            const NodeId id = idOf(g, jj.value("joint", json()));
            if (!id)
                return std::nullopt;
            f.joints.push_back(id);
            f.frames.push_back(frameFrom(jj.at("frame")));
        }
        f.tip = idOf(g, fj.value("tip", json()));
        f.hose = idOf(g, fj.value("hose", json()));
        f.hoseCurlSign = fj.value("hoseCurlSign", 1.0);
        f.hoseSpreadSign = fj.value("hoseSpreadSign", 1.0);
        if (!f.joints.empty())
            h.fingers.push_back(std::move(f));
    }
    h.rest = poseFromJson(s.value("rest", json::object()));
    return h;
}

NodeId handFor(Graph& g, NodeId node)
{
    if (!node)
        return kNoNode;
    if (Node* n = g.node(node); n && n->type().isA(kHandType))
        return node;
    const NodeId ch = characterFor(g, node);
    for (NodeId hn : handsOf(g, ch))
    {
        const std::optional<Hand> h = load(g, hn);
        if (!h)
            continue;
        if (h->joint == node)
            return hn;
        for (const Finger& f : h->fingers)
            if (f.meta == node || f.tip == node || f.hose == node || std::find(f.joints.begin(), f.joints.end(), node) != f.joints.end())
                return hn;
    }
    return kNoNode;
}

NodeId oppositeOf(Graph& g, NodeId handNode)
{
    const std::optional<Hand> h = load(g, handNode);
    if (!h || h->side == Side::Center)
        return kNoNode;
    for (NodeId other : handsOf(g, h->character))
        if (const std::optional<Hand> o = load(g, other); o && other != handNode && o->side != h->side && o->side != Side::Center)
            return other;
    return kNoNode;
}

Pose currentPose(Graph& g, const Hand& h)
{
    g.evaluate();
    Pose p;
    for (const Finger& f : h.fingers)
    {
        FingerPose fp;
        if (f.hose)
        {
            const HoseState s = hoseState(g, f.hose);
            const double total = s.effectiveCurl() * s.curlMax * f.hoseCurlSign;
            const std::array<double, kCanon> full = fullCurl(f.role);
            const double sum = full[0] + full[1] + full[2];
            for (int j = 0; j < kCanon; ++j)
                fp.joints[j].bend = total * full[j] / sum;
            fp.joints[0].splay = s.effectiveSpread() * s.spreadMax * f.hoseSpreadSign;
        }
        else
        {
            if (f.meta && f.metaFrame)
                fp.meta = anglesOf(*f.metaFrame, localNow(g, f.meta));
            std::vector<Angles> js;
            for (size_t k = 0; k < f.joints.size(); ++k)
                js.push_back(anglesOf(f.frames[k], localNow(g, f.joints[k])));
            fp.joints = fromJoints(js);
        }
        p.fingers[int(f.role)] = fp;
    }
    return p;
}

namespace
{
void writeLocal(Graph& g, NodeId id, const glm::dmat3& local, std::vector<std::pair<std::string, std::string>>* written)
{
    const glm::dmat3 r = glm::transpose(orientOf(g, id)) * local * glm::transpose(rotateAxisOf(g, id));
    const glm::dvec3 now = vecOf(g, id, Transform::kRotate);
    const glm::dvec3 v = rotateFor(g, id, r);
    if (glm::length(v - now) < 1e-9)
        return;
    g.setValue({id, Transform::kRotate, -1}, v);
    if (written)
        written->push_back({g.node(id)->path(), "rotate"});
}

void writeNumber(Graph& g, NodeId id, const char* attr, double v, std::vector<std::pair<std::string, std::string>>* written)
{
    const PlugRef p = plugOf(*g.node(id), attr);
    if (std::abs(std::get<double>(g.value(p)) - v) < 1e-9)
        return;
    g.setValue(p, v);
    if (written)
        written->push_back({g.node(id)->path(), attr});
}

/** A finger's joints for its canonical pose, each within its joint's range. */
std::vector<Angles> jointAngles(const Finger& f, const FingerPose& fp)
{
    std::vector<Angles> js = toJoints(fp.joints, f.joints.size());
    const std::vector<Limit> lim = jointLimits(f.role, js.size());
    for (size_t k = 0; k < js.size(); ++k)
        js[k] = lim[k].clamp(js[k]);
    return js;
}

void writeFinger(Graph& g, const Finger& f, const FingerPose& fp, std::vector<std::pair<std::string, std::string>>* written)
{
    if (f.hose)
    {
        const HoseState s = hoseState(g, f.hose);
        const double fist = std::clamp(s.fist, 0.0, 1.0);
        if (fist > 1.0 - 1e-6)
            return;
        double total = 0.0, splay = 0.0;
        for (const Angles& a : fp.joints)
        {
            total += a.bend;
            splay += a.splay;
        }
        const double curl = (total * f.hoseCurlSign / s.curlMax - fist * s.fistCurl) / (1.0 - fist) - s.handCurl;
        const double spread = splay * f.hoseSpreadSign / s.spreadMax / (1.0 - fist) - s.handSpread * s.spreadWeight;
        writeNumber(g, f.hose, "curl", curl, written);
        writeNumber(g, f.hose, "spread", spread, written);
        return;
    }
    if (f.meta && f.metaFrame)
        writeLocal(g, f.meta, localFor(*f.metaFrame, metaLimit().clamp(fp.meta)), written);
    const std::vector<Angles> js = jointAngles(f, fp);
    for (size_t k = 0; k < f.joints.size(); ++k)
        writeLocal(g, f.joints[k], localFor(f.frames[k], js[k]), written);
}
} // namespace

void writePose(Graph& g, const Hand& h, const Pose& p, const Mask& mask, std::vector<std::pair<std::string, std::string>>* written)
{
    for (const Finger& f : h.fingers)
        if (mask[int(f.role)] && p.fingers[int(f.role)])
            writeFinger(g, f, *p.fingers[int(f.role)], written);
}

json savedPoses()
{
    json v = PrefStore::get().value(kPrefs, kPosesKey, json::object());
    return v.is_object() ? v : json::object();
}

std::optional<Pose> namedPose(const Hand& h, const std::string& name)
{
    if (name == "rest")
        return h.rest;
    if (const Library* l = findLibrary(name))
        return poseFrom(l->shape);
    const json saved = savedPoses();
    if (auto it = saved.find(name); it != saved.end())
        return poseFromJson(it->value("pose", json::object()));
    return std::nullopt;
}

json shapeJson(const Hand& h, const Pose& p)
{
    json fingers = json::object();
    for (const Finger& f : h.fingers)
        fingers[roleName(f.role)] = {{"curl", measureFingerCurl(p, f.role)}, {"spread", measureFingerSpread(p, f.role)}};
    return {{"curl", measureCurl(p)}, {"spread", measureSpread(p)}, {"thumb", measureThumb(p)}, {"cup", measureCup(p, h.hasMeta())}, {"fingers", fingers}};
}

/** Detects the character's hands and makes a hand node for each, replacing any it had. Returns the new nodes. */
std::vector<NodeId> setupHands(Graph& g, NodeId character)
{
    const NodeId root = characterRoot(g, character);
    if (!root)
        return {};
    for (NodeId old : handsOf(g, character))
        g.deleteNode(old);
    std::vector<NodeId> ids;
    const std::vector<SkelJoint> js = skeletonOf(g, root, ids);
    std::vector<NodeId> made;
    for (const HandRig& hr : detectHands(js))
    {
        const NodeId handJoint = ids[size_t(hr.hand)];
        const glm::dmat3 handRot = js[size_t(hr.hand)].rotation, toHand = glm::transpose(handRot);
        json fingers = json::array();
        for (const FingerRig& fr : hr.fingers)
        {
            const FingerFrames ff = fingerFrames(js, hr, fr);
            json fj = {{"role", roleName(fr.role)}, {"roleFrom", fr.roleFrom}};
            if (fr.meta >= 0)
                fj["meta"] = {{"joint", pathOf(g, ids[size_t(fr.meta)])}, {"frame", frameJson(*ff.meta)}};
            json jl = json::array();
            for (size_t k = 0; k < fr.joints.size(); ++k)
                jl.push_back({{"joint", pathOf(g, ids[size_t(fr.joints[k])])}, {"frame", frameJson(ff.joints[k])}});
            fj["joints"] = jl;
            if (fr.tip >= 0)
                fj["tip"] = pathOf(g, ids[size_t(fr.tip)]);
            if (const NodeId hose = hoseOfFinger(g, ids[size_t(fr.joints.front())]))
            {
                const NodeId end = fr.tip >= 0 ? ids[size_t(fr.tip)] : ids[size_t(fr.joints.back())];
                const auto [cs, ss] = hoseSigns(g, hose, end, hr.palm, hr.thumbSide);
                fj["hose"] = pathOf(g, hose);
                fj["hoseCurlSign"] = cs;
                fj["hoseSpreadSign"] = ss;
            }
            fingers.push_back(fj);
        }
        json setup = {{"version", 1},
                      {"hand", pathOf(g, handJoint)},
                      {"sideFrom", hr.sideFrom},
                      {"palm", vec(toHand * hr.palm)},
                      {"forward", vec(toHand * hr.forward)},
                      {"thumbSide", vec(toHand * hr.thumbSide)},
                      {"fingers", fingers}};
        Node* n = g.createNode(*types::find(kHandType), lila::scene::poses::stripNamespace(g.node(handJoint)->name()) + "_hand", character);
        const NodeId id = n->id();
        g.setValue(plugOf(*n, "side"), int64_t(hr.side));
        g.setValue(plugOf(*n, "setup"), setup.dump());
        if (const std::optional<Hand> h = load(g, id))
        {
            setup["rest"] = poseJson(currentPose(g, *h));
            g.setValue(plugOf(*g.node(id), "setup"), setup.dump());
        }
        made.push_back(id);
    }
    return made;
}

namespace
{
using Written = std::vector<std::pair<std::string, std::string>>;

/** Undo for a change to the saved poses, which live in the prefs rather than the scene. */
struct SavedPosesUndo : UndoAction
{
    json before, after;
    SavedPosesUndo(json b, json a) : before(std::move(b)), after(std::move(a)) {}
    void undo() override { PrefStore::get().set(kPrefs, kPosesKey, before); }
    void redo() override { PrefStore::get().set(kPrefs, kPosesKey, after); }
    bool selectionOnly() const override { return true; }
};

/** Keys what a pose wrote, at the current time. */
void keyWritten(Graph& g, const Written& written)
{
    const double t = g.time();
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto& w : written)
    {
        if (!seen.insert(w).second)
            continue;
        Node* n = named(g, w.first);
        if (!n)
            continue;
        if (w.second == "rotate")
            keyVec(w.first, "rotate", vecOf(g, n->id(), Transform::kRotate), t);
        else
            ops::execute("setKeyframe", {{"args", {w.first + "." + w.second}}, {"time", t}, {"value", std::get<double>(g.value(plugOf(*n, w.second.c_str())))}});
    }
}

/** Every plug the hand's poses write, for keying the whole hand. */
Written handPlugs(Graph& g, const Hand& h, const Mask& mask)
{
    Written out;
    for (const Finger& f : h.fingers)
    {
        if (!mask[int(f.role)])
            continue;
        if (f.hose)
        {
            out.push_back({pathOf(g, f.hose), "curl"});
            out.push_back({pathOf(g, f.hose), "spread"});
            continue;
        }
        if (f.meta)
            out.push_back({pathOf(g, f.meta), "rotate"});
        for (NodeId j : f.joints)
            out.push_back({pathOf(g, j), "rotate"});
    }
    return out;
}

/** The hands a command acts on: the hands its objects (or the selection) are in, then narrowed or widened by side. */
std::vector<NodeId> targetsOf(Graph& g, const Args& a, const char* command)
{
    std::vector<NodeId> nodes;
    for (const std::string& s : objectArgs(a))
        nodes.push_back(nodeNamed(g, s, command).id());
    if (nodes.empty())
        nodes = g.selection();
    std::vector<NodeId> chars;
    auto addChar = [&](NodeId c) {
        if (c && std::find(chars.begin(), chars.end(), c) == chars.end())
            chars.push_back(c);
    };
    for (NodeId n : nodes)
        addChar(characterFor(g, n));
    if (chars.empty())
        if (const std::vector<NodeId> all = characters(g); all.size() == 1)
            chars = all;
    if (chars.empty())
        throw CommandError(std::string(command) + ": select a hand, a finger or a character with hands");
    for (NodeId c : chars)
        if (handsOf(g, c).empty())
            setupHands(g, c);

    std::vector<NodeId> hands;
    auto addHand = [&](NodeId h) {
        if (h && std::find(hands.begin(), hands.end(), h) == hands.end())
            hands.push_back(h);
    };
    for (NodeId n : nodes)
        if (Node* node = g.node(n); node && node->type().isA(kCharacterType))
            for (NodeId h : handsOf(g, n))
                addHand(h);
        else
            addHand(handFor(g, n));
    const std::string side = a.get<std::string>("side", "");
    if (hands.empty() || side == "both" || side == "left" || side == "right")
    {
        std::vector<NodeId> all;
        for (NodeId c : chars)
            for (NodeId h : handsOf(g, c))
                all.push_back(h);
        if (side == "left" || side == "right")
        {
            const int64_t want = side == "left" ? int64_t(Side::Left) : int64_t(Side::Right);
            hands.clear();
            for (NodeId h : all)
                if (std::get<int64_t>(g.value(plugOf(*g.node(h), "side"))) == want)
                    hands.push_back(h);
        }
        else if (side == "both" || hands.empty())
            hands = all;
    }
    else if (side == "opposite")
    {
        std::vector<NodeId> other;
        for (NodeId h : hands)
            if (NodeId o = oppositeOf(g, h))
                other.push_back(o);
        hands = other;
    }
    else if (!side.empty() && side != "selected")
        throw CommandError(std::string(command) + ": side is left, right, both, opposite or selected");
    if (hands.empty())
        throw CommandError(std::string(command) + ": no hands found; a hand needs at least two finger chains under it");
    return hands;
}

/** Turns auto key off while poses are written, so keys come once, from keyWritten. */
struct AutoKeyOff
{
    bool was = lila::scene::anim::autoKey();
    AutoKeyOff() { lila::scene::anim::setAutoKey(false); }
    ~AutoKeyOff() { lila::scene::anim::setAutoKey(was); }
};

bool keyArg(const Args& a) { return a.get<bool>("key", lila::scene::anim::autoKey()); }

/** Runs edit on every target hand: its pose now in, the new pose out, written for the fingers in mask and keyed when asked. */
json editHands(const Args& a, const char* command, const std::function<Pose(const Hand&, const Pose&, const Mask&)>& edit)
{
    Graph& g = Graph::current();
    const Mask mask = maskFromJson(a.has("fingers") ? a.flags.at("fingers") : json());
    const bool key = keyArg(a);
    Written written;
    json out = json::array();
    {
        const AutoKeyOff off;
        for (NodeId hn : targetsOf(g, a, command))
        {
            const std::optional<Hand> h = load(g, hn);
            if (!h)
                throw CommandError(std::string(command) + ": " + g.node(hn)->path() + "'s joints are gone; run puppeteerHandSetup again");
            const Pose base = currentPose(g, *h);
            writePose(g, *h, edit(*h, base, mask), mask, &written);
            out.push_back({{"hand", g.node(hn)->path()}, {"side", sideName(h->side)}});
        }
    }
    if (key)
        keyWritten(g, written);
    for (json& r : out)
        if (const std::optional<Hand> h = load(g, g.find(r["hand"].get<std::string>())->id()))
            r["shape"] = shapeJson(*h, currentPose(g, *h));
    return out;
}

json handInfoJson(Graph& g, const Hand& h)
{
    json fingers = json::array();
    for (const Finger& f : h.fingers)
    {
        json joints = json::array();
        for (NodeId j : f.joints)
            joints.push_back(pathOf(g, j));
        fingers.push_back({{"role", roleName(f.role)}, {"roleFrom", f.roleFrom}, {"joints", joints}, {"metacarpal", pathOf(g, f.meta)},
                           {"tip", pathOf(g, f.tip)}, {"hose", pathOf(g, f.hose)}});
    }
    const Pose p = currentPose(g, h);
    return {{"name", pathOf(g, h.node)},      {"character", pathOf(g, h.character)}, {"joint", pathOf(g, h.joint)},
            {"side", sideName(h.side)},       {"sideFrom", h.sideFrom},              {"fingers", fingers},
            {"shape", shapeJson(h, p)},       {"pose", poseJson(p)}};
}

// ---------------------------------------------------------------------------------------------------------------------
// Grip.

struct Collider
{
    std::vector<std::unique_ptr<geo::MeshBvh>> bvhs;
    double distance(const glm::dvec3& p, double within) const
    {
        double best = std::numeric_limits<double>::max();
        for (const auto& b : bvhs)
        {
            geo::BvhClosest c;
            if (b->closestPoint(p, c, within))
                best = std::min(best, c.distance);
        }
        return best;
    }
    bool inside(const glm::dvec3& p) const
    {
        // Odd crossings along a ray: inside a closed mesh.
        for (const auto& b : bvhs)
        {
            int hits = 0;
            glm::dvec3 o = p;
            const glm::dvec3 dir = glm::normalize(glm::dvec3(0.31, 0.83, 0.47));
            for (int k = 0; k < 64; ++k)
            {
                geo::BvhHit h;
                if (!b->raycast(o, dir, h))
                    break;
                ++hits;
                o = o + dir * (h.t + 1e-4);
            }
            if (hits % 2 == 1)
                return true;
        }
        return false;
    }
};

std::unique_ptr<geo::MeshBvh> worldBvh(Graph& g, const Node& mesh)
{
    const int out = mesh.type().attrIndex("outMesh");
    if (out < 0)
        return nullptr;
    const MeshPtr m = std::get<MeshPtr>(g.value({mesh.id(), out, -1}));
    if (!m || m->numFaces() == 0)
        return nullptr;
    geo::Mesh world = *m;
    const glm::dmat4 xf = worldOf(g, mesh.id());
    for (glm::dvec3& q : world.positions())
        q = glm::dvec3(xf * glm::dvec4(q, 1.0));
    return std::make_unique<geo::MeshBvh>(world);
}

/** Meshes a grip target names: a mesh, a transform over meshes, or a character's meshes. */
std::vector<NodeId> targetMeshes(Graph& g, NodeId target)
{
    std::vector<NodeId> out;
    Node* n = g.node(target);
    if (!n)
        return out;
    if (n->type().isA(kCharacterType))
    {
        for (const std::string& m : skeletonMeshes(g, characterRoot(g, target)))
            if (Node* mn = named(g, m))
                out.push_back(mn->id());
        return out;
    }
    if (n->type().attrIndex("outMesh") >= 0)
        return {target};
    for (NodeId d : g.descendants(target))
        if (Node* dn = g.node(d); dn && dn->type().attrIndex("outMesh") >= 0)
            out.push_back(d);
    return out;
}

/** Points along a finger's bones, base first: each joint to the next, the last bone carried on to the tip. */
std::vector<std::vector<glm::dvec3>> segmentSamples(Graph& g, const Finger& f, int perBone = 4)
{
    std::vector<NodeId> chain = f.joints;
    std::vector<glm::dvec3> at;
    for (NodeId j : chain)
        at.push_back(glm::dvec3(worldOf(g, j)[3]));
    if (f.tip)
        at.push_back(glm::dvec3(worldOf(g, f.tip)[3]));
    else if (at.size() >= 2)
    {
        // No end joint: the last bone runs on along the last joint's axis that its parent's bone lies on.
        const glm::dmat3 r = math::rotationOf(worldOf(g, chain.back()));
        const glm::dvec3 prev = at[at.size() - 1] - at[at.size() - 2];
        int best = 0;
        double score = -1.0, sign = 1.0;
        for (int k = 0; k < 3; ++k)
            if (const double d = glm::dot(glm::normalize(r[k]), glm::normalize(prev)); std::abs(d) > score)
            {
                score = std::abs(d);
                best = k;
                sign = d >= 0.0 ? 1.0 : -1.0;
            }
        at.push_back(at.back() + glm::normalize(r[best]) * sign * glm::length(prev) * 0.8);
    }
    std::vector<std::vector<glm::dvec3>> out;
    for (size_t k = 0; k + 1 < at.size(); ++k)
    {
        std::vector<glm::dvec3> s;
        for (int i = 1; i <= perBone; ++i)
            s.push_back(glm::mix(at[k], at[k + 1], double(i) / perBone));
        out.push_back(s);
    }
    return out;
}

/** A finger's rotate values (or hose curl and spread), to put back exactly after trying poses on it. */
struct FingerSnapshot
{
    std::vector<std::pair<PlugRef, PlugValue>> values;
    FingerSnapshot(Graph& g, const Finger& f)
    {
        auto keep = [&](const PlugRef& p) { values.push_back({p, g.value(p)}); };
        if (f.hose)
        {
            keep(plugOf(*g.node(f.hose), "curl"));
            keep(plugOf(*g.node(f.hose), "spread"));
            return;
        }
        if (f.meta)
            keep({f.meta, Transform::kRotate, -1});
        for (NodeId j : f.joints)
            keep({j, Transform::kRotate, -1});
    }
    void restore(Graph& g) const
    {
        for (const auto& [p, v] : values)
            g.setValue(p, v);
    }
};

} // namespace

void Snapshot::restore(Graph& g) const
{
    for (const auto& [p, v] : values)
        if (g.node(p.node))
            g.setValue(p, v);
}

Snapshot snapshot(Graph& g, const std::vector<NodeId>& handNodes)
{
    Snapshot s;
    for (NodeId hn : handNodes)
        if (const std::optional<Hand> h = load(g, hn))
            for (const Finger& f : h->fingers)
                for (auto& v : FingerSnapshot(g, f).values)
                    s.values.push_back(std::move(v));
    return s;
}

namespace
{
/** The finger's thickness: how far its joints sit inside its own skin, else a share of its length. */
double thicknessOf(Graph& g, const Hand& h, const Finger& f, const Collider& own, bool& fromMesh)
{
    std::vector<double> d;
    for (NodeId j : f.joints)
    {
        const double v = own.distance(glm::dvec3(worldOf(g, j)[3]), 10.0);
        if (v < 10.0)
            d.push_back(v);
    }
    double length = 0.0;
    for (size_t k = 1; k < f.joints.size(); ++k)
        length += glm::length(glm::dvec3(worldOf(g, f.joints[k])[3]) - glm::dvec3(worldOf(g, f.joints[k - 1])[3]));
    (void)h;
    if (d.size() >= 2)
    {
        std::sort(d.begin(), d.end());
        fromMesh = true;
        return std::clamp(d[d.size() / 2], 0.1, std::max(0.2, 0.5 * length));
    }
    fromMesh = false;
    return std::max(0.1, 0.16 * length);
}

/** Poses a jointed finger opens toward to get out of what it starts in: lifted back, out to either side, and both. */
std::vector<std::vector<Angles>> waysOut(const Finger& f, const std::vector<Angles>& from)
{
    const std::vector<Limit> lim = jointLimits(f.role, from.size());
    std::vector<Angles> lifted = from;
    for (size_t k = 0; k < from.size(); ++k)
        lifted[k].bend = lim[k].bend.lo;
    std::vector<std::vector<Angles>> out{lifted};
    for (const double splay : {lim[0].splay.lo, lim[0].splay.hi})
        for (const std::vector<Angles>* v : std::initializer_list<const std::vector<Angles>*>{&from, &lifted})
        {
            std::vector<Angles> s = *v;
            s[0].splay = splay;
            out.push_back(s);
        }
    return out;
}

json gripCmd(const Args& a)
{
    Graph& g = Graph::current();
    const Mask mask = maskFromJson(a.has("fingers") ? a.flags.at("fingers") : json());
    const bool key = keyArg(a);
    const std::vector<NodeId> hands = targetsOf(g, a, "puppeteerHandGrip");

    std::vector<NodeId> meshes;
    std::vector<std::string> targetNames;
    if (a.has("target"))
        for (const json& t : a.flags.at("target").is_array() ? a.flags.at("target") : json::array({a.flags.at("target")}))
            targetNames.push_back(t.get<std::string>());
    else
        for (NodeId id : g.selection())
            if (Node* n = g.node(id); n && !rig::isJoint(*n) && !n->type().isA(kHandType) &&
                                      (!characterFor(g, id) || characterFor(g, id) != load(g, hands.front())->character || n->type().isA(kCharacterType)))
                targetNames.push_back(n->path());
    for (const std::string& t : targetNames)
        for (NodeId m : targetMeshes(g, nodeNamed(g, t, "puppeteerHandGrip").id()))
            meshes.push_back(m);
    if (meshes.empty())
        throw CommandError("puppeteerHandGrip: give target, a mesh or character to grip (or select one with the hand)");

    g.evaluate();
    Collider target;
    for (NodeId m : meshes)
        if (auto b = worldBvh(g, *g.node(m)))
            target.bvhs.push_back(std::move(b));
    if (target.bvhs.empty())
        throw CommandError("puppeteerHandGrip: the target has no faces");

    Written written;
    json out = json::array();
    {
        const AutoKeyOff off;
        for (NodeId hn : hands)
        {
            const std::optional<Hand> h = load(g, hn);
            if (!h)
                continue;
            Collider own;
            for (const std::string& m : skeletonMeshes(g, characterRoot(g, h->character)))
                if (Node* mn = named(g, m))
                    if (auto b = worldBvh(g, *mn))
                        own.bvhs.push_back(std::move(b));
            const Pose start = currentPose(g, *h);
            Pose closed = poseFrom(findLibrary("fist")->shape);
            // The thumb closes across and round, past the grip pose, so it meets what it wraps.
            closed.fingers[0] = poseFrom(Shape{{}, ts(0.8, 1.0, 0.6), 0.0}).fingers[0];
            json fingers = json::object();
            for (const Finger& f : h->fingers)
            {
                if (!mask[int(f.role)] || !start.fingers[int(f.role)])
                    continue;
                bool fromMesh = false;
                const double radius = a.has("thickness") ? a.get<double>("thickness") : thicknessOf(g, *h, f, own, fromMesh);
                const FingerPose base = *start.fingers[int(f.role)];
                // Hose fingers close as one; jointed ones stop joint by joint as each bone touches.
                const size_t n = f.hose ? 1 : f.joints.size();
                const FingerPose& shut = *closed.fingers[int(f.role)];
                auto expand = [&](const std::vector<Angles>& v) {
                    FingerPose fp = base;
                    if (f.hose)
                        for (int j = 0; j < kCanon; ++j)
                            fp.joints[j] = mix(base.joints[j], shut.joints[j], v[0].bend);
                    else
                        fp.joints = fromJoints(v);
                    return fp;
                };
                const std::vector<Angles> from = f.hose ? std::vector<Angles>{Angles{0.0}} : jointAngles(f, base);
                const std::vector<Angles> to = f.hose ? std::vector<Angles>{Angles{1.0}} : jointAngles(f, shut);
                auto contact = [&](const std::vector<Angles>& v) -> int {
                    writeFinger(g, f, expand(v), nullptr);
                    const std::vector<std::vector<glm::dvec3>> segs = segmentSamples(g, f);
                    for (size_t k = 0; k < segs.size(); ++k)
                        for (const glm::dvec3& p : segs[k])
                            if (target.distance(p, radius) <= radius || target.inside(p))
                                return f.hose ? 0 : int(std::min(k, n - 1));
                    return -1;
                };
                bool startedInside = false, stayed = false;
                std::vector<Angles> got = from;
                {
                    UndoStack& u = UndoStack::get();
                    const bool was = u.enabled();
                    u.setEnabled(false);
                    const FingerSnapshot before(g, f);
                    startedInside = contact(from) >= 0;
                    if (!startedInside)
                        got = wrap(from, to, contact);
                    else if (const std::optional<std::vector<Angles>> opened = f.hose ? std::nullopt : openOut(from, waysOut(f, from), contact))
                        got = wrap(*opened, to, contact);
                    else
                        stayed = true;
                    before.restore(g);
                    u.setEnabled(was);
                }
                const FingerPose result = stayed ? base : expand(got);
                if (!stayed)
                    writeFinger(g, f, result, &written);
                Pose one;
                one.fingers[int(f.role)] = result;
                fingers[roleName(f.role)] = {{"touching", !stayed && got != to},
                                             {"startedInside", startedInside},
                                             {"stayed", stayed},
                                             {"curl", measureFingerCurl(one, f.role)},
                                             {"thickness", radius},
                                             {"thicknessFrom", a.has("thickness") ? "flag" : fromMesh ? "mesh" : "estimate"}};
            }
            std::string stuck;
            for (const auto& [role, r] : fingers.items())
                if (r.value("stayed", false))
                    stuck += (stuck.empty() ? "" : ", ") + role;
            if (!stuck.empty())
                log::write(LogLevel::Warning, "puppeteerHandGrip: " + stuck + " on " + g.node(hn)->path() +
                                                  " can't open clear of the target within their joint limits (the knuckle sits against it), so "
                                                  "they stay as posed. Move the target off the knuckles and grip again");
            out.push_back({{"hand", g.node(hn)->path()}, {"fingers", fingers}});
        }
    }
    if (key)
        keyWritten(g, written);
    return out;
}

const FlagDef kFingersFlag{"fingers", "fi", ArgType::Any, nullptr, "Fingers to pose (thumb, index, middle, ring, pinky); default all."};
const FlagDef kSideFlag{"side", "sd", ArgType::String, nullptr, "selected (default), left, right, both or opposite: which of the character's hands."};
const FlagDef kKeyFlag{"key", "k", ArgType::Bool, nullptr, "Key what changes at the current time; default follows auto key."};

void registerHandType(ext::Context& ctx)
{
    const NodeType* transform = types::find("transform");
    NodeType t{.name = kHandType,
               .base = transform,
               .dag = true,
               .doc = "A character's hand: its hand joint, fingers and each finger joint's anatomical frame, which hand poses, "
                      "shapes and grips turn the joints through. Set up from the skeleton by puppeteerHandSetup.",
               .icon = "hand-stop",
               .create = transform->create};
    t.ownAttrs = {
        {.name = "side", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable, .enumNames = kSides, .doc = "Which hand: left or right."},
        text("setup", "The hand's joints and frames (JSON).", AttrHidden),
    };
    lila::scene::relocate::registerNeutral(t.name);
    types::registerType(ctx, std::move(t));
}

void registerHandCommands(ext::Context& ctx)
{
    using T = ArgType;
    ctx.registerCommand({
        .name = "puppeteerHandSetup",
        .doc = "Finds a character's hands in its skeleton (fingers by name, else by layout; jointed or rubber hose) and makes a "
               "hand node for each under the character, replacing any it had. The pose now is the hands' rest. Returns the hands.",
        .positionalDoc = "character or a node in it (default the selection's)",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> chars;
            for (const std::string& s : objectArgs(a))
                if (NodeId c = characterFor(g, nodeNamed(g, s, "puppeteerHandSetup").id()); c && std::find(chars.begin(), chars.end(), c) == chars.end())
                    chars.push_back(c);
            if (objectArgs(a).empty())
                for (NodeId id : g.selection())
                    if (NodeId c = characterFor(g, id); c && std::find(chars.begin(), chars.end(), c) == chars.end())
                        chars.push_back(c);
            if (chars.empty() && characters(g).size() == 1)
                chars = characters(g);
            if (chars.empty())
                throw CommandError("puppeteerHandSetup: give a character, or a node in one");
            json out = json::array();
            for (NodeId c : chars)
                for (NodeId h : setupHands(g, c))
                    if (const std::optional<Hand> hand = load(g, h))
                        out.push_back(handInfoJson(g, *hand));
            return out;
        },
        .examples = {"puppeteerHandSetup(\"manny_character\")"},
    });

    ctx.registerCommand({
        .name = "puppeteerHandInfo",
        .doc = "The hands of the selection or the given nodes: side, fingers and their joints, and the pose's shape (curl, spread, "
               "thumb, cup, each finger's curl and spread) and joint angles (bend, splay, twist in degrees, per finger).",
        .positionalDoc = "hands, fingers or a character (default the selection)",
        .flags = {kSideFlag},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            for (NodeId hn : targetsOf(g, a, "puppeteerHandInfo"))
                if (const std::optional<Hand> h = load(g, hn))
                    out.push_back(handInfoJson(g, *h));
            return out;
        },
    });

    ctx.registerCommand({
        .name = "puppeteerHandPose",
        .doc = "Puts hands into a pose: a built-in one (relaxed, fist, point, grip, pinch, ok, flat, spread, claw, thumbsUp, peace, "
               "cup, hook, trigger, ball, pen, gun, blade, horns, tense), a saved one, or rest. weight applies it partly; blendTo "
               "and mix blend it with a second pose first. Poses are anatomical, so one pose fits either hand and any character.",
        .positionalDoc = "hands, fingers or a character (default the selection)",
        .flags = {
            {"pose", "p", T::String, nullptr, "The pose."},
            {"blendTo", "bt", T::String, nullptr, "A second pose to blend the first with."},
            {"mix", "mx", T::Float, 0.5, "With blendTo: 0 the first pose, 1 the second."},
            {"weight", "w", T::Float, 1.0, "How far toward the pose: 1 all the way, 0 not at all."},
            kFingersFlag, kSideFlag, kKeyFlag,
        },
        .run = [](const Args& a) -> json {
            if (!a.has("pose"))
                throw CommandError("puppeteerHandPose: give pose");
            const std::string name = a.get<std::string>("pose");
            return editHands(a, "puppeteerHandPose", [&](const Hand& h, const Pose& base, const Mask& mask) {
                std::optional<Pose> p = namedPose(h, name);
                if (!p)
                    throw CommandError("puppeteerHandPose: no pose named '" + name + "'");
                if (a.has("blendTo"))
                {
                    const std::optional<Pose> b = namedPose(h, a.get<std::string>("blendTo"));
                    if (!b)
                        throw CommandError("puppeteerHandPose: no pose named '" + a.get<std::string>("blendTo") + "'");
                    p = mix(*p, *b, std::clamp(a.get<double>("mix", 0.5), 0.0, 1.0));
                }
                return mix(base, *p, std::clamp(a.get<double>("weight", 1.0), 0.0, 1.0), mask);
            });
        },
        .examples = {"puppeteerHandPose(\"hand_l\", pose=\"fist\")", "puppeteerHandPose(pose=\"relaxed\", blendTo=\"fist\", mix=0.3, side=\"both\")",
                     "puppeteerHandPose(\"index_01_r\", pose=\"point\", fingers=[\"index\"])"},
    });

    ctx.registerCommand({
        .name = "puppeteerHandShape",
        .doc = "Shapes hands by hand-level values: curl (0 straight, 1 a fist), spread (1 fanned, negative squeezed), thumb (-1 out, "
               "1 across the palm), cup (1 the palm hollowed), relax (toward the relaxed pose, negative toward tense), and per "
               "finger curl and spread. Each spreads over the joints the natural way and keeps whatever else the pose holds. "
               "add makes the values changes instead.",
        .positionalDoc = "hands, fingers or a character (default the selection)",
        .flags = {
            {"curl", "cu", T::Float, nullptr, "Curl of the fingers in fingers (the thumb has its own)."},
            {"spread", "sp", T::Float, nullptr, "Fan of the fingers."},
            {"thumb", "th", T::Float, nullptr, "The thumb: -1 out, 0 alongside, 1 across the palm."},
            {"cup", "cp", T::Float, nullptr, "Palm hollowing."},
            {"relax", "rl", T::Float, nullptr, "Toward the relaxed pose by this much, or the tense one when negative."},
            {"fingerCurl", "fc", T::Any, nullptr, "Per finger curl, {finger: value}."},
            {"fingerSpread", "fs", T::Any, nullptr, "Per finger spread, {finger: value}."},
            {"add", "ad", T::Bool, false, "The values are changes to the pose's own."},
            kFingersFlag, kSideFlag, kKeyFlag,
        },
        .run = [](const Args& a) -> json {
            const bool add = a.get<bool>("add", false);
            return editHands(a, "puppeteerHandShape", [&](const Hand& h, const Pose& base, const Mask& mask) {
                Pose p = base;
                if (a.has("relax"))
                    p = relax(p, a.get<double>("relax"), mask);
                if (a.has("curl"))
                    p = setCurl(p, a.get<double>("curl") + (add ? measureCurl(p, mask) : 0.0), mask);
                if (a.has("spread"))
                    p = setSpread(p, a.get<double>("spread") + (add ? measureSpread(p, mask) : 0.0), mask);
                if (a.has("cup"))
                    p = setCup(p, a.get<double>("cup") + (add ? measureCup(p, h.hasMeta(), mask) : 0.0), h.hasMeta(), mask);
                // After cup, which swings the thumb a little too, so the thumb lands where asked.
                if (a.has("thumb") && mask[0])
                    p = setThumb(p, a.get<double>("thumb") + (add ? measureThumb(p) : 0.0));
                for (const auto& [flag, isCurl] : {std::pair{"fingerCurl", true}, std::pair{"fingerSpread", false}})
                    if (a.has(flag))
                    {
                        const json& m = a.flags.at(flag);
                        if (!m.is_object())
                            throw CommandError(std::string("puppeteerHandShape: ") + flag + " is {finger: value}");
                        for (const auto& [k, v] : m.items())
                        {
                            const std::optional<Role> r = roleFromName(lowerOf(k));
                            if (!r)
                                throw CommandError("puppeteerHandShape: unknown finger '" + k + "'");
                            const double now = isCurl ? measureFingerCurl(p, *r) : measureFingerSpread(p, *r);
                            const double to = v.get<double>() + (add ? now : 0.0);
                            p = isCurl ? setFingerCurl(p, *r, to) : setFingerSpread(p, *r, to);
                        }
                    }
                return p;
            });
        },
        .examples = {"puppeteerHandShape(\"hand_l\", curl=0.5, spread=0.3)", "puppeteerHandShape(side=\"both\", fingerCurl={\"index\": 0.0})"},
    });

    ctx.registerCommand({
        .name = "puppeteerHandMirror",
        .doc = "Copies hands' poses onto the same character's other hand, mirrored. swap exchanges the two.",
        .positionalDoc = "hands, fingers or a character (default the selection)",
        .flags = {{"swap", "sw", T::Bool, false, "Exchange the two hands' poses."}, kFingersFlag, kSideFlag, kKeyFlag},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const Mask mask = maskFromJson(a.has("fingers") ? a.flags.at("fingers") : json());
            const bool key = keyArg(a), swap = a.get<bool>("swap", false);
            Written written;
            json out = json::array();
            {
                const AutoKeyOff off;
                std::set<NodeId> done;
                for (NodeId hn : targetsOf(g, a, "puppeteerHandMirror"))
                {
                    const NodeId other = oppositeOf(g, hn);
                    if (!other || done.count(hn))
                        continue;
                    const std::optional<Hand> from = load(g, hn), to = load(g, other);
                    if (!from || !to)
                        continue;
                    const Pose p = currentPose(g, *from), q = currentPose(g, *to);
                    writePose(g, *to, p, mask, &written);
                    if (swap)
                    {
                        writePose(g, *from, q, mask, &written);
                        done.insert(other);
                    }
                    done.insert(hn);
                    out.push_back({{"from", pathOf(g, hn)}, {"to", pathOf(g, other)}});
                }
            }
            if (out.empty())
                throw CommandError("puppeteerHandMirror: the character has no hand on the other side");
            if (key)
                keyWritten(g, written);
            return out;
        },
    });

    ctx.registerCommand({
        .name = "puppeteerHandSavePose",
        .doc = "Saves a hand's pose under a name, for any hand of any character (poses are anatomical). fingers saves only those, "
               "so applying it leaves the others. Saved poses live in the user preferences.",
        .positionalDoc = "the hand, a finger or its character (default the selection)",
        .flags = {{"name", "n", T::String, nullptr, "The pose's name."}, kFingersFlag, kSideFlag},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::string name = a.get<std::string>("name", "");
            if (name.empty() || name == "rest" || findLibrary(name))
                throw CommandError("puppeteerHandSavePose: give a name that is not rest or a built-in pose");
            const Mask mask = maskFromJson(a.has("fingers") ? a.flags.at("fingers") : json());
            const std::optional<Hand> h = load(g, targetsOf(g, a, "puppeteerHandSavePose").front());
            if (!h)
                throw CommandError("puppeteerHandSavePose: the hand's joints are gone");
            Pose p = currentPose(g, *h);
            for (int r = 0; r < kRoles; ++r)
                if (!mask[r])
                    p.fingers[r].reset();
            json all = savedPoses();
            const json before = all;
            all[name] = {{"pose", poseJson(p)}};
            PrefStore::get().set(kPrefs, kPosesKey, all);
            UndoStack::get().record(std::make_unique<SavedPosesUndo>(before, all));
            return name;
        },
        .examples = {"puppeteerHandSavePose(\"hand_r\", name=\"sword grip\")"},
    });

    ctx.registerCommand({
        .name = "puppeteerHandDeletePose",
        .doc = "Deletes a saved hand pose.",
        .positionalDoc = "the pose's name",
        .run = [](const Args& a) -> json {
            const std::vector<std::string> names = objectArgs(a);
            if (names.empty())
                throw CommandError("puppeteerHandDeletePose: give the pose's name");
            json all = savedPoses();
            const json before = all;
            for (const std::string& n : names)
                if (!all.erase(n))
                    throw CommandError("puppeteerHandDeletePose: no saved pose named '" + n + "'");
            PrefStore::get().set(kPrefs, kPosesKey, all);
            UndoStack::get().record(std::make_unique<SavedPosesUndo>(before, all));
            return names.size();
        },
    });

    ctx.registerCommand({
        .name = "puppeteerHandPoses",
        .doc = "Every hand pose: the built-in ones, then the saved ones, each with its fingers.",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::array();
            for (const Library& l : library())
                out.push_back({{"name", l.id}, {"label", l.label}, {"icon", l.icon}, {"builtIn", true}});
            const json saved = savedPoses();
            for (const auto& [k, v] : saved.items())
            {
                json fingers = json::array();
                const json pose = v.value("pose", json::object());
                for (const auto& [r, _] : pose.items())
                    fingers.push_back(r);
                out.push_back({{"name", k}, {"label", k}, {"builtIn", false}, {"fingers", fingers}});
            }
            return out;
        },
    });

    ctx.registerCommand({
        .name = "puppeteerHandGrip",
        .doc = "Closes hands' fingers onto a mesh or another character until they touch: each finger curls the natural way and, "
               "as a bone touches, the joints before it stop and the rest carry on, so it wraps the surface. Thickness is how far "
               "the finger's skin is from its bones, by default measured from the character's own mesh.",
        .positionalDoc = "hands, fingers or a character (default the selection)",
        .flags = {
            nodeFlag({"target", "tg", T::Any, nullptr, "Mesh, group or character to grip; default what else is selected."}, "mesh"),
            {"thickness", "tk", T::Float, nullptr, "Finger skin thickness, cm."},
            kFingersFlag, kSideFlag, kKeyFlag,
        },
        .run = gripCmd,
        .examples = {"puppeteerHandGrip(\"hand_r\", target=\"sword_grip\")"},
    });

    ctx.registerCommand({
        .name = "puppeteerHandKey",
        .doc = "Keys every finger joint of hands at the current time (a hose finger's curl and spread), whatever moved.",
        .positionalDoc = "hands, fingers or a character (default the selection)",
        .flags = {kFingersFlag, kSideFlag},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const Mask mask = maskFromJson(a.has("fingers") ? a.flags.at("fingers") : json());
            Written all;
            for (NodeId hn : targetsOf(g, a, "puppeteerHandKey"))
                if (const std::optional<Hand> h = load(g, hn))
                    for (auto& w : handPlugs(g, *h, mask))
                        all.push_back(w);
            keyWritten(g, all);
            return all.size();
        },
    });
}
} // namespace
} // namespace puppeteer::hand

namespace puppeteer
{
std::vector<NodeId> setupHands(Graph& g, NodeId character) { return hand::setupHands(g, character); }

void registerHands(lila::ext::Context& ctx)
{
    hand::registerHandType(ctx);
    hand::registerHandCommands(ctx);
}
} // namespace puppeteer
