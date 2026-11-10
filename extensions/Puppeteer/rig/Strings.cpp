// Strings on Puppeteer's characters: the mode drags pose in, what holds still (pins and locked hands and feet), and
// stringsPose, which poses joints the way a Spinner drag does.

#include "Puppeteer.h"
#include "rig/Rig.h"

#include <lila/Math.h>
#include <lila/Prefs.h>
#include <lila/Curves.h>
#include <lila/Ghosts.h>
#include <lila/OptionBox.h>
#include <lila/Paths.h>
#include <lila/SplineChain.h>
#include <lila/Strings.h>
#include <lila/UI.h>
#include <lila/ViewportTool.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <lila/Anim.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace puppeteer
{
namespace
{
constexpr const char* kModePref = "/strings/mode";
constexpr const char* kStretchPref = "/strings/stretch";
constexpr const char* kContactsPref = "/strings/contacts";
/** How far bones may lengthen with stretch on, as a multiple of their length. */
constexpr double kStretchLimit = 1.5;

strings::Mode& modeRef()
{
    static strings::Mode m = strings::modeFromName(PrefStore::get().value<std::string>(kPrefs, kModePref, "fullBody")).value_or(strings::Mode::FullBody);
    return m;
}

bool& stretchRef()
{
    static bool on = PrefStore::get().value<bool>(kPrefs, kStretchPref, false);
    return on;
}

bool& onLayerRef()
{
    static bool on = PrefStore::get().value<bool>(kPrefs, "/strings/onLayer", false);
    return on;
}

bool& contactsRef()
{
    static bool on = PrefStore::get().value<bool>(kPrefs, kContactsPref, true);
    return on;
}

std::string bareJoint(const std::string& name) { return name.substr(name.find_last_of("|:") + 1); }

/** A character's rule web: the limbs its rig declares and the joints that hold by default (its contacts, feet). */
struct CharacterRules
{
    std::vector<std::vector<std::string>> chains; // joint names, top to end
    std::vector<std::string> contacts;
    /** Lines of action: body paths a stroke may bind to, by name, each [top joint, end joint]. */
    std::map<std::string, std::vector<std::string>> lines;
    /** Where each came from: "character", "template", "rig elements" or "none". */
    std::string chainsFrom = "none", contactsFrom = "none", linesFrom = "none";
};

/** Lines when nothing declares them: each limb, and the hips to the head when there is one. */
void inferLines(Graph& g, NodeId character, CharacterRules& r)
{
    for (const auto& chain : r.chains)
        r.lines[chain.front()] = {chain.front(), chain.back()};
    const NodeId root = characterRoot(g, character);
    if (!root)
        return;
    const strings::Skeleton k = strings::snapshot(g, root);
    for (NodeId d : g.descendants(root))
        if (bareJoint(g.node(d)->name()) == "head" && k.hips >= 0 && isUnder(g, d, k.joints[size_t(k.hips)]))
        {
            r.lines["body"] = {bareJoint(g.node(k.joints[size_t(k.hips)])->name()), "head"};
            break;
        }
    r.linesFrom = "inferred";
}

/**
 * From the character's rig template when it has one: its limb groups, and rig.strings.contacts (else the ends of its
 * leg limbs). Without one, from the rig elements: IK chains as limbs, and the ends of chains with a reverse foot.
 */
CharacterRules inheritedRules(Graph& g, NodeId character)
{
    CharacterRules out;
    if (const std::string& id = textOf(*g.node(character), "templateId"); !id.empty() && ops::find("rigTemplateInfo"))
        try
        {
            const json t = ops::execute("rigTemplateInfo", {{"args", {id}}});
            std::map<std::string, std::string> joint;
            for (const json& m : t.value("markers", json::array()))
                joint[m.value("name", std::string())] = m.value("joint", m.value("name", std::string()));
            auto jointOf = [&](const std::string& marker) { return joint.contains(marker) ? joint[marker] : marker; };
            for (const json& limb : t.value("limbs", json::array()))
            {
                std::vector<std::string> chain;
                for (const json& m : limb.value("markers", json::array()))
                    chain.push_back(jointOf(m.get<std::string>()));
                if (chain.size() >= 2)
                    out.chains.push_back(chain);
                if (limb.value("name", std::string()).starts_with("leg") && !chain.empty())
                    out.contacts.push_back(chain.back());
            }
            out.chainsFrom = out.contactsFrom = "template";
            const json c = t.value("rig", json::object()).value("strings", json::object());
            if (c.contains("contacts"))
            {
                out.contacts.clear();
                for (const json& m : c["contacts"])
                    out.contacts.push_back(jointOf(m.get<std::string>()));
            }
            if (c.contains("lines"))
            {
                for (const auto& [name, ends] : c["lines"].items())
                    out.lines[name] = {jointOf(ends[0].get<std::string>()), jointOf(ends[1].get<std::string>())};
                out.linesFrom = "template";
            }
            else
                inferLines(g, character, out);
            return out;
        }
        catch (const std::exception&)
        {
        }
    const NodeId root = characterRoot(g, character);
    for (NodeId chain : lila::scene::rigel::chains(g))
        if (const auto js = lila::scene::rigel::chainJoints(g, chain); js.size() >= 2 && isUnder(g, js.front(), root))
        {
            std::vector<std::string> names;
            for (NodeId j : js)
                names.push_back(bareJoint(g.node(j)->name()));
            out.chains.push_back(names);
            if (footForChain(g, chain))
                out.contacts.push_back(names.back());
            out.chainsFrom = out.contactsFrom = "rig elements";
        }
    inferLines(g, character, out);
    return out;
}

/** The character's own rules (its strings attr) over what its template or rig elements say. */
CharacterRules rulesOf(Graph& g, NodeId character)
{
    CharacterRules out = inheritedRules(g, character);
    const std::string& text = textOf(*g.node(character), "strings");
    if (text.empty())
        return out;
    const json own = json::parse(text, nullptr, false);
    if (!own.is_object())
        return out;
    if (own.contains("contacts") && own["contacts"].is_array())
    {
        out.contacts = own["contacts"].get<std::vector<std::string>>();
        out.contactsFrom = "character";
    }
    if (own.contains("chains") && own["chains"].is_array())
    {
        out.chains = own["chains"].get<std::vector<std::vector<std::string>>>();
        out.chainsFrom = "character";
    }
    if (own.contains("lines") && own["lines"].is_object())
    {
        out.lines = own["lines"].get<std::map<std::string, std::vector<std::string>>>();
        out.linesFrom = "character";
    }
    return out;
}

/** The rules as the skeleton's bones; names match joints with namespaces ignored. */
strings::Rules bonesOf(const strings::Skeleton& k, Graph& g, const CharacterRules& r)
{
    std::map<std::string, int> bone;
    for (size_t i = 0; i < k.joints.size(); ++i)
        bone[bareJoint(g.node(k.joints[i])->name())] = int(i);
    strings::Rules out;
    for (const auto& names : r.chains)
    {
        std::vector<int> chain;
        for (const std::string& n : names)
            if (const auto it = bone.find(bareJoint(n)); it != bone.end())
                chain.push_back(it->second);
        if (chain.size() >= 2)
            out.chains.push_back(chain);
    }
    return out;
}

/** Pins, locked hands and feet, and the extra anchors, as holds at their bones' current worlds. */
std::vector<strings::Hold> holdsFor(Graph& g, NodeId character, const strings::Skeleton& k, const std::vector<NodeId>& extra,
                                    const std::vector<std::string>& contacts = {})
{
    std::vector<NodeId> joints = pinsOf(g, character);
    for (const std::string& c : contacts)
        for (NodeId j : k.joints)
            if (bareJoint(g.node(j)->name()) == bareJoint(c))
                joints.push_back(j);
    for (NodeId chain : lila::scene::rigel::chains(g))
        if (chainLocked(g, chain, g.time()))
            if (const auto js = lila::scene::rigel::chainJoints(g, chain); !js.empty())
                joints.push_back(js.back());
    joints.insert(joints.end(), extra.begin(), extra.end());
    const std::vector<glm::dmat4> w = strings::worlds(k.problem, strings::startPose(k.problem));
    std::vector<strings::Hold> out;
    for (NodeId j : joints)
        if (const int b = k.boneOf(j); b >= 0 && std::none_of(out.begin(), out.end(), [&](const strings::Hold& h) { return h.bone == b; }))
            out.push_back({.bone = b, .world = w[size_t(b)]});
    return out;
}

std::string bareName(const std::string& path) { return path.substr(path.find_last_of("|:") + 1); }

/**
 * Glue and attach: the members keep where they sit relative to each other through every pose. Glue is two-way (the
 * member that moves leads); attach makes every member but the last follow the last. Kept by name for the session,
 * never in the scene and never keyed.
 */
struct Relation
{
    std::vector<std::string> members;
    bool attach = false;
};

std::vector<Relation>& relations()
{
    static std::vector<Relation> r;
    return r;
}

std::vector<NodeId> membersOf(Graph& g, const Relation& r)
{
    std::vector<NodeId> out;
    for (const std::string& m : r.members)
        if (Node* n = g.find(m); n && n->type().isA("transform"))
            out.push_back(n->id());
    return out;
}

bool related(Graph& g, NodeId node)
{
    for (const Relation& r : relations())
        for (NodeId m : membersOf(g, r))
            if (m == node)
                return true;
    return false;
}

struct Moved
{
    NodeId node;
    glm::dmat4 before, after;
};

bool sameMatrix(const glm::dmat4& a, const glm::dmat4& b)
{
    for (int c = 0; c < 4; ++c)
        if (glm::length(a[c] - b[c]) > 1e-9)
            return false;
    return true;
}

/** Where the relations put their followers, given what moved; grabbed leads any glue it is in. */
std::vector<std::pair<NodeId, glm::dmat4>> followersOf(Graph& g, const std::vector<Moved>& moved, NodeId grabbed)
{
    auto find = [&](NodeId n) { return std::find_if(moved.begin(), moved.end(), [&](const Moved& m) { return m.node == n; }); };
    std::vector<std::pair<NodeId, glm::dmat4>> out;
    for (const Relation& r : relations())
    {
        const std::vector<NodeId> members = membersOf(g, r);
        if (members.size() < 2)
            continue;
        NodeId leader = kNoNode;
        if (r.attach)
            leader = members.back();
        else if (std::find(members.begin(), members.end(), grabbed) != members.end())
            leader = grabbed;
        else
            for (NodeId m : members)
                if (const auto it = find(m); it != moved.end() && !sameMatrix(it->before, it->after))
                {
                    leader = m;
                    break;
                }
        const auto it = find(leader);
        if (it == moved.end() || sameMatrix(it->before, it->after))
            continue;
        const glm::dmat4 delta = it->after * glm::inverse(it->before);
        for (NodeId m : members)
            if (m != leader && std::none_of(out.begin(), out.end(), [&](const auto& f) { return f.first == m; }))
            {
                const auto own = find(m);
                out.push_back({m, delta * (own != moved.end() ? own->before : worldOf(g, m))});
            }
    }
    return out;
}

/** Puts nodes outside the posed skeleton at their followers' worlds: props, or joints of other characters. */
/** While posing across frames: every node.attr a pose wrote, to key at that frame. */
std::vector<std::string>* gWritten = nullptr;

void written(const std::vector<std::string>& plugs)
{
    if (gWritten)
        gWritten->insert(gWritten->end(), plugs.begin(), plugs.end());
}

void applyPose(Graph& g, const strings::Skeleton& k, const strings::Pose& s) { written(strings::apply(g, k, s)); }

void placeObjects(Graph& g, const std::vector<std::pair<NodeId, glm::dmat4>>& targets)
{
    for (const auto& [n, w] : targets)
    {
        setWorldRotation(g, n, math::rotationOf(w));
        setWorldPosition(g, n, glm::dvec3(w[3]));
        written({g.node(n)->path() + ".rotate", g.node(n)->path() + ".translate"});
    }
}

/**
 * Followers that are joints of a character bend their limbs to their targets, the character's pins and locks holding;
 * the rest are placed directly.
 */
json follow(Graph& g, const std::vector<std::pair<NodeId, glm::dmat4>>& targets, double maxStretch)
{
    std::map<NodeId, std::vector<std::pair<NodeId, glm::dmat4>>> byCharacter;
    std::vector<std::pair<NodeId, glm::dmat4>> objects;
    for (const auto& t : targets)
        if (Node* n = g.node(t.first); n && rig::isJoint(*n) && characterRoot(g, characterFor(g, t.first)))
            byCharacter[characterFor(g, t.first)].push_back(t);
        else
            objects.push_back(t);
    json out = json::array();
    for (const auto& [character, ts] : byCharacter)
    {
        g.evaluate();
        strings::Skeleton k = strings::snapshot(g, characterRoot(g, character));
        const CharacterRules rules = rulesOf(g, character);
        k.rules = bonesOf(k, g, rules);
        std::vector<strings::Hold> holds = holdsFor(g, character, k, {}, contactsRef() ? rules.contacts : std::vector<std::string>{});
        for (const auto& [n, w] : ts)
            if (const int b = k.boneOf(n); b >= 0)
            {
                std::erase_if(holds, [&](const strings::Hold& h) { return h.bone == b; });
                holds.push_back({.bone = b, .world = w});
                out.push_back(g.node(n)->path());
            }
            else
                objects.push_back({n, w});
        strings::Pose s = strings::startPose(k.problem);
        strings::holdLimbs(k.problem, s, holds, k.hips, maxStretch, {}, &k.rules);
        applyPose(g, k, s);
    }
    placeObjects(g, objects);
    for (const auto& o : objects)
        out.push_back(g.node(o.first)->path());
    return out;
}

/** A node outside any character, moved or turned about its own origin, and whatever is glued or attached to it. */
json poseObject(Graph& g, NodeId node, const std::optional<glm::dvec3>& move, const std::optional<glm::dquat>& turn, double maxStretch)
{
    const glm::dmat4 before = worldOf(g, node);
    glm::dmat4 after = before;
    if (turn)
        after = glm::translate(glm::dmat4(1.0), glm::dvec3(before[3])) * glm::dmat4(glm::mat3_cast(*turn)) *
                glm::translate(glm::dmat4(1.0), -glm::dvec3(before[3])) * before;
    if (move)
        after = glm::translate(glm::dmat4(1.0), *move) * after;
    const auto targets = followersOf(g, {{node, before, after}}, node);
    placeObjects(g, {{node, after}});
    json out = {{"object", g.node(node)->path()}};
    if (!targets.empty())
        out["followed"] = follow(g, targets, maxStretch);
    return out;
}

std::optional<glm::dvec3> moveOf(const Args& a) { return a.has("by") ? std::optional(jsonVec(a.flags["by"])) : std::nullopt; }

std::optional<glm::dquat> turnOf(const Args& a)
{
    if (!a.has("axis") || a.get<double>("angle", 0.0) == 0.0)
        return std::nullopt;
    const glm::dvec3 axis = jsonVec(a.flags["axis"]);
    if (glm::length(axis) < 1e-12)
        throw CommandError("stringsPose: the axis is zero");
    return glm::angleAxis(math::radians(a.get<double>("angle", 0.0)), glm::normalize(axis));
}

/** What one pose does: a move or turn of the grabbed joint, and what holds while it happens. */
struct PoseInput
{
    std::optional<glm::dvec3> move;
    std::optional<glm::dquat> turn;
    /** Instead of turn: the local rotation the joint goes to (zero, a stored pose), as the turn that gets it there. */
    std::optional<glm::dquat> rotation;
    std::vector<NodeId> anchors;
    bool holds = true;
    /** The character's contacts (feet) hold too, as its rules say. */
    bool contacts = true;
    int chainLength = 0;
    double stretch = 1.0;
    bool autoPose = false;
};

PoseInput inputOf(Graph& g, const Args& a)
{
    PoseInput in{.move = moveOf(a),
                 .turn = turnOf(a),
                 .holds = a.get<bool>("holds", true),
                 .contacts = a.get<bool>("contacts", contactsRef()),
                 .chainLength = a.get<int>("chainLength", 0),
                 .stretch = std::max(1.0, a.get<double>("stretch", 1.0)),
                 .autoPose = a.get<bool>("autoPose", false)};
    if (a.has("anchors"))
        for (const json& n : a.flags["anchors"])
            in.anchors.push_back(jointNamed(g, n.get<std::string>(), "stringsPose").id());
    return in;
}

/** The world turn about the bone's origin that takes its local rotation from where it is to r. */
glm::dquat turnTo(const fbik::Problem& p, int bone, const glm::dquat& r)
{
    strings::Pose s = strings::startPose(p);
    const glm::dmat3 before = math::rotationOf(strings::worlds(p, s)[size_t(bone)]);
    s.rotation[size_t(bone)] = r;
    const glm::dmat3 after = math::rotationOf(strings::worlds(p, s)[size_t(bone)]);
    return glm::normalize(glm::quat_cast(after * glm::transpose(before)));
}

json poseJoint(Graph& g, NodeId joint, strings::Mode mode, const PoseInput& in)
{
    const std::string path = g.node(joint)->path();
    const NodeId character = characterFor(g, joint);
    const NodeId root = characterRoot(g, character);
    if (!root)
        throw CommandError("stringsPose: " + path + " is not in a character");
    json out = {{"joint", path}, {"mode", strings::modeName(mode)}};
    const std::optional<glm::dvec3> move = in.move;
    std::optional<glm::dquat> turn = in.turn;

    // The auto-poser runs inside full-body moves only; the rest of Full Body goes through the same solve as the others.
    if (mode == strings::Mode::FullBody && move && !turn && !in.rotation && in.autoPose)
    {
        const std::vector<NodeId> posed = fullBodyMove(g, {joint}, *move, true);
        if (posed.empty())
            throw CommandError("stringsPose: " + path + " cannot be posed full body (a root above the hips moves the ordinary way)");
        return out;
    }

    strings::Skeleton k = strings::snapshot(g, root, stiffnessOf(g, character, fullBodyJoints(g, joint)));
    const int bone = k.boneOf(joint);
    if (bone < 0)
        throw CommandError("stringsPose: " + path + " is not a deform joint of its character");
    if (in.rotation)
        turn = turnTo(k.problem, bone, *in.rotation);
    const CharacterRules rules = rulesOf(g, character);
    k.rules = bonesOf(k, g, rules);
    strings::Request r{.mode = mode,
                       .grabbed = bone,
                       .turn = turn,
                       .move = move,
                       .holds = in.holds ? holdsFor(g, character, k, in.anchors, in.contacts ? rules.contacts : std::vector<std::string>{})
                                         : std::vector<strings::Hold>{},
                       .hips = k.hips,
                       .chainLength = in.chainLength,
                       .maxStretch = in.stretch,
                       .rules = &k.rules};
    std::erase_if(r.holds, [&](const strings::Hold& h) { return h.bone == bone; });
    // Full Body turning with nothing held is a plain turn: no solve has anything to keep.
    if (mode == strings::Mode::FullBody && !move && r.holds.empty())
        r.mode = strings::Mode::Forward;

    strings::Result res = strings::solve(k.problem, r);
    if (!res.handled)
    {
        // A joint above the hips has no body to swing or limb to bend: it turns and moves the plain way, and says so.
        r.mode = strings::Mode::Forward;
        res = strings::solve(k.problem, r);
        out["fallback"] = std::string(strings::modeName(mode)) + " needs a joint at or below the hips; posed Forward";
        log::write(LogLevel::Info, "stringsPose: " + bareName(path) + " is above the hips, so it was posed Forward");
    }
    // Glue and attach: what follows a joint of this skeleton bends its limb there, holding the anchors; the rest is placed after.
    std::vector<std::pair<NodeId, glm::dmat4>> others;
    if (!relations().empty())
    {
        const std::vector<glm::dmat4> w0 = strings::worlds(k.problem, strings::startPose(k.problem)), w1 = strings::worlds(k.problem, res.pose);
        std::vector<Moved> moved;
        for (size_t i = 0; i < k.joints.size(); ++i)
            moved.push_back({k.joints[i], w0[i], w1[i]});
        std::vector<strings::Hold> glued = r.holds;
        json followed = json::array();
        for (const auto& [n, w] : followersOf(g, moved, joint))
            if (const int b = k.boneOf(n); b >= 0 && b != bone)
            {
                std::erase_if(glued, [&](const strings::Hold& h) { return h.bone == b; });
                glued.push_back({.bone = b, .world = w});
                followed.push_back(g.node(n)->path());
            }
            else if (b < 0)
                others.push_back({n, w});
        if (glued.size() > r.holds.size() || !followed.empty())
            res.error = std::max(res.error, strings::holdLimbs(k.problem, res.pose, glued, k.hips, r.maxStretch, {}, &k.rules));
        if (!followed.empty())
            out["followed"] = followed;
    }
    applyPose(g, k, res.pose);
    if (!others.empty())
    {
        const json more = follow(g, others, r.maxStretch);
        for (const json& m : more)
            out["followed"].push_back(m);
    }
    out["error"] = res.error;
    if (r.mode == strings::Mode::Ik)
    {
        json chain = json::array();
        for (int b : strings::limbOf(k.problem, bone, k.hips, -1, r.chainLength, &k.rules))
            chain.push_back(g.node(k.joints[size_t(b)])->path());
        out["chain"] = chain;
    }
    if (res.anchor >= 0)
        out["anchor"] = g.node(k.joints[size_t(res.anchor)])->path();
    json held = json::array();
    for (const strings::Hold& h : r.holds)
        held.push_back(g.node(k.joints[size_t(h.bone)])->path());
    out["holds"] = held;
    return out;
}

strings::Mode modeArg(const Args& a, const char* command)
{
    if (!a.has("mode"))
        return stringsMode();
    const auto m = strings::modeFromName(a.get<std::string>("mode"));
    if (!m)
        throw CommandError(std::string(command) + ": mode is forward, backward, ik or fullBody");
    return *m;
}

int depthIn(Graph& g, NodeId id)
{
    int d = 0;
    for (NodeId p = g.parent(id); p; p = g.parent(p))
        ++d;
    return d;
}

/** The command's joints in characters, parents first. */
std::vector<NodeId> characterJoints(Graph& g, const Args& a, const char* command)
{
    std::vector<NodeId> out;
    for (const std::string& s : objectArgs(a))
    {
        const NodeId j = jointNamed(g, s, command).id();
        if (!characterRoot(g, characterFor(g, j)))
            throw CommandError(std::string(command) + ": " + g.node(j)->path() + " is not in a character");
        out.push_back(j);
    }
    if (out.empty())
        throw CommandError(std::string(command) + ": give joints or select them");
    std::stable_sort(out.begin(), out.end(), [&](NodeId x, NodeId y) { return depthIn(g, x) < depthIn(g, y); });
    return out;
}

json stringsZero(const Args& a)
{
    Graph& g = graph();
    strings::Mode mode = modeArg(a, "stringsZero");
    PoseInput in{.rotation = glm::dquat(1.0, 0.0, 0.0, 0.0), .holds = a.get<bool>("holds", true), .stretch = std::max(1.0, a.get<double>("stretch", 1.0))};
    // Relative zero: what it is relative to holds still in the world while the rest zeroes around it.
    std::vector<NodeId> relative;
    if (a.has("relativeTo"))
        for (const json& n : a.flags["relativeTo"])
            relative.push_back(jointNamed(g, n.get<std::string>(), "stringsZero").id());
    std::string fallback;
    if (!relative.empty())
    {
        in.anchors = relative;
        in.holds = true;
        if (mode == strings::Mode::Forward)
        {
            mode = strings::Mode::Backward;
            fallback = "forward moves nothing to keep what it is relative to still; zeroed backward";
            log::write(LogLevel::Info, "stringsZero: " + fallback);
        }
    }
    json out = json::array();
    for (NodeId j : characterJoints(g, a, "stringsZero"))
    {
        if (std::find(relative.begin(), relative.end(), j) != relative.end())
            continue;
        g.evaluate();
        json r = poseJoint(g, j, mode, in);
        if (!fallback.empty())
            r["fallback"] = fallback;
        out.push_back(r);
    }
    return out;
}

/** The first character the command names: a character node, or a node in one. */
NodeId stringsCharacterArg(Graph& g, const Args& a, const char* command)
{
    for (const std::string& n : objectArgs(a))
        if (const NodeId c = characterFor(g, nodeNamed(g, n, command).id()))
            return c;
    throw CommandError(std::string(command) + ": give a character, or a node in one");
}

json rulesJson(const CharacterRules& r)
{
    return {{"contacts", r.contacts}, {"chains", r.chains},       {"lines", r.lines},
            {"contactsFrom", r.contactsFrom}, {"chainsFrom", r.chainsFrom}, {"linesFrom", r.linesFrom}};
}

/** The joints from top down to end, both included; empty when end is not under top. */
std::vector<std::string> pathBetween(Graph& g, NodeId top, NodeId end)
{
    std::vector<std::string> out;
    for (NodeId j = end; j; j = g.parent(j))
    {
        out.insert(out.begin(), bareJoint(g.node(j)->name()));
        if (j == top)
            return out;
    }
    return {};
}

json stringsRules(const Args& a)
{
    Graph& g = graph();
    const NodeId character = stringsCharacterArg(g, a, "stringsRules");
    const PlugRef plug = plugOf(*g.node(character), "strings");
    if (a.get<bool>("reset", false))
        g.setValue(plug, std::string());
    json own = json::parse(textOf(*g.node(character), "strings"), nullptr, false);
    if (!own.is_object())
        own = json::object();
    bool changed = false;
    if (a.has("contacts"))
    {
        json contacts = json::array();
        for (const json& j : a.flags["contacts"])
            contacts.push_back(bareJoint(jointNamed(g, j.get<std::string>(), "stringsRules").name()));
        own["contacts"] = contacts;
        changed = true;
    }
    if (a.has("addChain"))
    {
        const json& ends = a.flags["addChain"];
        if (!ends.is_array() || ends.size() != 2)
            throw CommandError("stringsRules: addChain is [top joint, end joint]");
        const NodeId top = jointNamed(g, ends[0].get<std::string>(), "stringsRules").id();
        const NodeId end = jointNamed(g, ends[1].get<std::string>(), "stringsRules").id();
        const std::vector<std::string> chain = pathBetween(g, top, end);
        if (chain.size() < 2)
            throw CommandError("stringsRules: " + g.node(end)->path() + " is not below " + g.node(top)->path());
        if (!own.contains("chains"))
            own["chains"] = rulesOf(g, character).chains;
        // A joint is in one limb at a time: the new one replaces any it overlaps.
        json kept = json::array();
        for (const json& c : own["chains"])
            if (std::none_of(c.begin(), c.end(), [&](const json& n) { return std::find(chain.begin(), chain.end(), n.get<std::string>()) != chain.end(); }))
                kept.push_back(c);
        kept.push_back(chain);
        own["chains"] = kept;
        changed = true;
    }
    if (a.has("addLine"))
    {
        const json& l = a.flags["addLine"];
        if (!l.is_array() || l.size() != 3)
            throw CommandError("stringsRules: addLine is [name, top joint, end joint]");
        const NodeId top = jointNamed(g, l[1].get<std::string>(), "stringsRules").id();
        const NodeId end = jointNamed(g, l[2].get<std::string>(), "stringsRules").id();
        if (pathBetween(g, top, end).size() < 2)
            throw CommandError("stringsRules: " + g.node(end)->path() + " is not below " + g.node(top)->path());
        if (!own.contains("lines"))
            own["lines"] = rulesOf(g, character).lines;
        own["lines"][l[0].get<std::string>()] = {bareJoint(g.node(top)->name()), bareJoint(g.node(end)->name())};
        changed = true;
    }
    if (a.has("removeChain"))
    {
        const std::string joint = bareJoint(jointNamed(g, a.get<std::string>("removeChain"), "stringsRules").name());
        if (!own.contains("chains"))
            own["chains"] = rulesOf(g, character).chains;
        json kept = json::array();
        for (const json& c : own["chains"])
            if (std::none_of(c.begin(), c.end(), [&](const json& n) { return n.get<std::string>() == joint; }))
                kept.push_back(c);
        if (kept.size() == own["chains"].size())
            throw CommandError("stringsRules: " + joint + " is in no limb");
        own["chains"] = kept;
        changed = true;
    }
    if (a.has("removeLine"))
    {
        if (!own.contains("lines"))
            own["lines"] = rulesOf(g, character).lines;
        if (!own["lines"].contains(a.get<std::string>("removeLine")))
            throw CommandError("stringsRules: no line named " + a.get<std::string>("removeLine"));
        own["lines"].erase(a.get<std::string>("removeLine"));
        changed = true;
    }
    if (changed)
        g.setValue(plug, own.dump());
    return rulesJson(rulesOf(g, character));
}

/** Lila space to Unreal space as a 3x3 map on vectors (left-handed, so its determinant is -1). */
glm::dmat3 unrealBasis()
{
    glm::dmat3 b(0.0);
    for (int c = 0; c < 3; ++c)
    {
        lila::math::Vector e(c == 0 ? 1.0 : 0.0, c == 1 ? 1.0 : 0.0, c == 2 ? 1.0 : 0.0);
        const lila::math::Vector u = lila::math::toUnreal(e);
        b[c] = glm::dvec3(u.x, u.y, u.z);
    }
    return b;
}

/**
 * A joint limit as Unreal's Rig Units clamp it. Unreal holds a bone's whole local rotation, Lila's frame * rotate * post
 * carried into Unreal space; there it is S * K with K the constant (frame * post) part and S rotate turned by the
 * frame, so the limit's axis and bone direction go through the frame and the basis, and its angles stay.
 */
json unrealLimit(const ik::JointLimit& l)
{
    const glm::dmat3 b = unrealBasis();
    const glm::dmat3 frame = b * glm::mat3_cast(l.frame);
    const double handed = glm::determinant(b) < 0.0 ? -1.0 : 1.0;
    const glm::dmat3 k = b * glm::mat3_cast(l.frame * l.post) * glm::inverse(b);
    const glm::dquat kq = glm::normalize(glm::quat_cast(k));
    auto vec = [](const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); };
    json out = {{"kind", l.kind == ik::JointLimit::Kind::Hinge ? "hinge" : "cone"}, {"post", {kq.x, kq.y, kq.z, kq.w}}};
    if (l.kind == ik::JointLimit::Kind::Hinge)
    {
        out["axis"] = vec(glm::normalize(frame * l.axis) * handed);
        out["min"] = l.minAngle;
        out["max"] = l.maxAngle;
    }
    else
    {
        out["bone"] = vec(glm::normalize(frame * l.bone));
        out["cone"] = l.cone;
    }
    return out;
}

/**
 * The rule web as plain data for Unreal: bone names as the skeleton has them (the joints' names), the hips, contacts,
 * limbs, lines of action, each bone's full-body stiffness and joint limits (in Unreal's local-rotation space). Nothing
 * positional: the Rig Units read the skeleton itself.
 */
json ruleWeb(Graph& g, NodeId character)
{
    const NodeId root = characterRoot(g, character);
    if (!root)
        throw CommandError("stringsRuleWeb: " + g.node(character)->path() + " has no skeleton");
    const CharacterRules r = rulesOf(g, character);
    // The bones exactly as Strings solves them, parents first, so Unreal builds the same problem.
    const strings::Skeleton k = strings::snapshot(g, root, stiffnessOf(g, character, fullBodyJoints(g, root)));
    json bones = json::array(), stiffness = json::object(), limits = json::object();
    for (size_t i = 0; i < k.joints.size(); ++i)
    {
        const fbik::Bone& b = k.problem.bones[i];
        const std::string name = bareJoint(g.node(k.joints[i])->name());
        json bone = {{"name", name}, {"parent", b.parent >= 0 ? bareJoint(g.node(k.joints[size_t(b.parent)])->name()) : ""}};
        if (!b.solve)
            bone["solve"] = false;
        bones.push_back(std::move(bone));
        if (b.stiffness != 0.0)
            stiffness[name] = b.stiffness;
        if (b.limit.kind != ik::JointLimit::Kind::None)
            limits[name] = unrealLimit(b.limit);
    }
    return {{"version", 1},
            {"character", g.node(character)->name()},
            {"root", bareJoint(g.node(root)->name())},
            {"hips", k.hips >= 0 ? bareJoint(g.node(k.joints[size_t(k.hips)])->name()) : ""},
            {"bones", bones},
            {"contacts", r.contacts},
            {"chains", r.chains},
            {"lines", r.lines},
            {"stiffness", stiffness},
            {"limits", limits},
            {"lengthScale", k.problem.lengthScale}};
}

json stringsRuleWeb(const Args& a)
{
    Graph& g = graph();
    const json web = ruleWeb(g, stringsCharacterArg(g, a, "stringsRuleWeb"));
    if (a.has("file"))
    {
        const std::string path = a.get<std::string>("file");
        std::ofstream out(lila::paths::fromUtf8(path));
        if (!out)
            throw CommandError("stringsRuleWeb: can't write " + path);
        out << web.dump(2);
    }
    return web;
}

/**
 * The rule web sent to Unreal: LilaLink makes or updates a Control Rig on the skeletal mesh with a control on each hand,
 * foot, head and the hips, each posing its bone through a Lila Strings Full Body unit given the web.
 */
json unrealStringsRig(const Args& a)
{
    Graph& g = graph();
    const NodeId character = stringsCharacterArg(g, a, "unrealStringsRig");
    const json web = ruleWeb(g, character);
    std::string mesh = a.get<std::string>("skeletalMesh", "");
    if (mesh.empty() && ops::find("unrealMappings"))
        try
        {
            const std::string root = g.node(characterRoot(g, character))->path();
            const json maps = ops::execute("unrealMappings", json::object());
            for (const json& m : maps.is_array() ? maps : maps.value("mappings", json::array()))
                if (m.value("kind", "") == "skeletal" && (m.value("node", "") == root || g.find(m.value("node", "")) == g.node(characterRoot(g, character))))
                    mesh = m.value("contentPath", "");
        }
        catch (const std::exception&)
        {
        }
    if (mesh.empty())
        throw CommandError("unrealStringsRig: give skeletalMesh, or map the character's root with unrealMap kind=skeletal");
    std::string rig = a.get<std::string>("rig", "");
    if (rig.empty())
    {
        const size_t slash = mesh.rfind('/');
        std::string name = mesh.substr(slash + 1);
        if (name.rfind("SK_", 0) == 0)
            name = name.substr(3);
        rig = mesh.substr(0, slash + 1) + "CR_" + name + "_Strings";
    }
    json controls = json::array();
    auto addControl = [&](const json& bone) {
        if (bone.is_string() && !bone.get<std::string>().empty() && std::find(controls.begin(), controls.end(), bone) == controls.end())
            controls.push_back(bone);
    };
    addControl(web["hips"]);
    for (const auto& [name, ends] : web["lines"].items())
        if (ends.size() == 2)
            addControl(ends[1]);
    for (const json& chain : web["chains"])
        if (!chain.empty())
            addControl(chain.back());
    for (const json& c : web["contacts"])
        addControl(c);
    const json payload = {{"skeletalMesh", mesh}, {"rig", rig}, {"ruleWeb", web}, {"controls", controls}};
    if (a.get<bool>("dryRun", false))
        return {{"rig", rig}, {"skeletalMesh", mesh}, {"payload", payload}};
    // Reloaded so a LilaLink update takes effect without restarting the editor.
    const std::string code = "import importlib, lila_link_strings\nimportlib.reload(lila_link_strings)\nlila_link_strings.build_and_print(" +
                             json(payload.dump()).dump() + ")\n";
    const json r = ops::execute("unrealCall", {{"args", {"runPython"}}, {"params", {{"code", code}}}, {"timeoutMs", 180000}});
    if (!r.value("ok", false))
        throw CommandError("unrealStringsRig: " + r.value("error", std::string("Unreal didn't answer")));
    json result;
    std::string log;
    for (const json& line : r.value("result", json::object()).value("log", json::array()))
    {
        const std::string text = line.value("text", "");
        if (text.rfind("LILA_RESULT:", 0) == 0)
            result = json::parse(text.substr(12), nullptr, false);
        else
            log += text + "\n";
    }
    if (!result.is_object())
        throw CommandError("unrealStringsRig: Unreal didn't build the rig" + (log.empty() ? std::string() : ":\n" + log));
    if (const json errors = result.value("errors", json::array()); !errors.empty())
        throw CommandError("unrealStringsRig: " + errors[0].get<std::string>());
    return {{"rig", result.value("rig", rig)}, {"skeletalMesh", mesh}, {"unreal", result}};
}

json stringsRestoreLengths(const Args& a)
{
    Graph& g = graph();
    json restored = json::array(), skipped = json::array();
    for (NodeId j : characterJoints(g, a, "stringsRestoreLengths"))
    {
        const NodeId parent = g.parent(j);
        const auto bind = strings::bindWorld(g, j);
        const auto parentBind = parent ? strings::bindWorld(g, parent) : std::nullopt;
        if (!bind || !parentBind)
        {
            skipped.push_back(g.node(j)->path());
            continue;
        }
        const double length = glm::length(glm::dvec3((*bind)[3]) - glm::dvec3((*parentBind)[3]));
        const glm::dvec3 t = vecOf(g, j, Transform::kTranslate);
        if (glm::length(t) < 1e-9 || std::abs(glm::length(t) - length) < 1e-9)
            continue;
        g.setValue({j, Transform::kTranslate, -1}, t * (length / glm::length(t)));
        restored.push_back(g.node(j)->path());
    }
    if (!skipped.empty())
        log::write(LogLevel::Info, "stringsRestoreLengths: " + std::to_string(skipped.size()) +
                                       " joint(s) drive no skin, so they have no bind length to go back to");
    return {{"restored", restored}, {"skipped", skipped}};
}

/** A rotation by w of the way from identity to q, the short way round. */
glm::dquat partOf(const glm::dquat& q, double w)
{
    const glm::dquat id(1.0, 0.0, 0.0, 0.0);
    return glm::slerp(id, glm::dot(q, id) < 0.0 ? -q : q, w);
}

/** The world rotation the offsets of a pose across frames are expressed in at the current time. */
glm::dmat3 spaceAt(Graph& g, NodeId grabbed, const std::string& space)
{
    if (space == "world" || space.empty())
        return glm::dmat3(1.0);
    if (space == "parent")
        return math::rotationOf(parentWorldOf(g, grabbed));
    return math::rotationOf(worldOf(g, nodeNamed(g, space, "stringsPose").id()));
}

/**
 * While one lives, keys go to the additive layer named (made when missing) instead of the base animation, so an edit can
 * be reviewed, weighted or dropped; the layer that was active comes back after. An empty name leaves keys where they go.
 */
struct LayerScope
{
    std::string was;

    explicit LayerScope(Graph& g, const std::string& name)
    {
        if (name.empty())
            return;
        const NodeId active = lila::scene::anim::activeLayer(g);
        was = active ? g.node(active)->name() : std::string(lila::scene::anim::kBaseLayerName);
        Node* n = g.find(name);
        if (!n || !n->type().isA("animLayer"))
            ops::execute("createAnimLayer", {{"name", name}, {"empty", true}});
        ops::execute("setActiveAnimLayer", {{"args", {name}}});
    }
    ~LayerScope()
    {
        if (!was.empty())
            try
            {
                ops::execute("setActiveAnimLayer", {{"args", {was}}});
            }
            catch (const std::exception& e)
            {
                log::write(LogLevel::Warning, std::string("strings: could not make ") + was + " active again: " + e.what());
            }
    }
    LayerScope(const LayerScope&) = delete;
    LayerScope& operator=(const LayerScope&) = delete;
};

/** A transform's translate and rotate as they were, to put back what a preview wrote into channels no curve drives. */
struct PoseValues
{
    NodeId id;
    PlugValue translate, rotate;
};

/** Every transform a pose of nodes can write: their characters' joints, what is glued or attached to them, themselves. */
std::vector<PoseValues> savePoseValues(Graph& g, const std::vector<NodeId>& nodes)
{
    std::vector<NodeId> ids(nodes.begin(), nodes.end());
    for (NodeId n : nodes)
        if (characterFor(g, n))
            for (NodeId j : fullBodyJoints(g, n))
                ids.push_back(j);
    for (const Relation& r : relations())
        for (NodeId m : membersOf(g, r))
        {
            ids.push_back(m);
            if (characterFor(g, m))
                for (NodeId j : fullBodyJoints(g, m))
                    ids.push_back(j);
        }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::vector<PoseValues> out;
    for (NodeId id : ids)
        if (Node* n = g.node(id); n && n->type().isA("transform"))
            out.push_back({id, g.value({id, Transform::kTranslate, -1}), g.value({id, Transform::kRotate, -1})});
    return out;
}

void restorePoseValues(Graph& g, const std::vector<PoseValues>& saved)
{
    for (const PoseValues& s : saved)
    {
        if (!g.node(s.id))
            continue;
        if (g.value({s.id, Transform::kTranslate, -1}) != s.translate)
            g.setValue({s.id, Transform::kTranslate, -1}, s.translate);
        if (g.value({s.id, Transform::kRotate, -1}) != s.rotate)
            g.setValue({s.id, Transform::kRotate, -1}, s.rotate);
    }
    g.evaluate();
}

/**
 * The same pose at every frame of a set, each solved on that frame's own pose so holds and contacts keep per frame, and
 * keyed there. Relative offsets each frame by the drag (in space), scaled by the falloff weight; absolute brings each
 * frame to where the drag put the joint now.
 */
json poseAcrossFrames(Graph& g, const std::vector<NodeId>& nodes, const Args& a, const PoseInput& in,
                      const std::function<json(NodeId, const PoseInput&)>& poseOne)
{
    lila::scene::anim::TimeSelection sel;
    sel.active = true;
    std::vector<double> times;
    if (a.has("frames"))
        for (const json& t : a.flags["frames"])
            times.push_back(t.get<double>());
    if (a.has("range"))
    {
        const json& r = a.flags["range"];
        if (!r.is_array() || r.size() != 2)
            throw CommandError("stringsPose: range is [start, end] in seconds");
        sel.start = std::min(r[0].get<double>(), r[1].get<double>());
        sel.end = std::max(r[0].get<double>(), r[1].get<double>());
        sel.falloff = std::max(0.0, a.get<double>("falloff", 0.0));
        const double from = lila::scene::anim::frameOf(sel.start) - sel.falloff, to = lila::scene::anim::frameOf(sel.end) + sel.falloff;
        if (a.get<bool>("dense", false))
            for (double f = std::ceil(from - 1e-6); f <= to + 1e-6; f += 1.0)
                times.push_back(lila::scene::anim::secondsOf(f));
        else
        {
            std::vector<NodeId> keyed;
            for (NodeId n : nodes)
                if (const NodeId c = characterFor(g, n))
                {
                    const std::vector<NodeId> js = fullBodyJoints(g, n);
                    keyed.insert(keyed.end(), js.begin(), js.end());
                    (void)c;
                }
                else
                    keyed.push_back(n);
            for (double t : lila::scene::anim::keyTimes(g, keyed))
                if (lila::scene::anim::frameOf(t) >= from - 1e-6 && lila::scene::anim::frameOf(t) <= to + 1e-6)
                    times.push_back(t);
        }
    }
    else if (!times.empty())
    {
        sel.start = *std::min_element(times.begin(), times.end());
        sel.end = *std::max_element(times.begin(), times.end());
    }
    // A preview also solves the frames the ghosts and a trail show, so they follow the drag; between keys that is
    // the solve of the interpolated pose, close to what the keys will give.
    const bool preview = a.get<bool>("preview", false);
    std::vector<double> ghostTimes;
    if (preview)
    {
        for (const auto& [t, offset] : ghosts::cache().shownTimes(ghosts::Settings::fromPrefs()))
            ghostTimes.push_back(t);
        times.insert(times.end(), ghostTimes.begin(), ghostTimes.end());
        if (a.has("show"))
            for (const json& t : a.flags["show"])
                times.push_back(t.get<double>());
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(), [](double x, double y) { return std::abs(x - y) < 1e-9; }), times.end());
    if (times.empty())
        throw CommandError("stringsPose: no frames to pose in the range (no keys there; dense poses every frame)");

    const bool relative = a.get<bool>("relative", true);
    const std::string space = a.get<std::string>("space", "world");
    const double now = g.time();
    g.evaluate();
    struct Start
    {
        glm::dmat4 world;
        glm::dmat3 space;
    };
    std::map<NodeId, Start> start;
    for (NodeId n : nodes)
        start[n] = {worldOf(g, n), spaceAt(g, n, space)};
    const std::vector<PoseValues> unkeyed = preview ? savePoseValues(g, nodes) : std::vector<PoseValues>{};
    std::vector<std::shared_ptr<const ghosts::Frame>> ghostFrames;

    // Every frame is solved on its pose as it was, so nothing is keyed until all are solved: a key set early would move
    // the frames interpolated after it. Auto key would key each value as it is set.
    struct Solved
    {
        double t;
        std::vector<std::pair<std::string, glm::dvec3>> values;
    };
    std::vector<Solved> solved;
    const bool autoKeyWas = lila::scene::anim::autoKey();
    lila::scene::anim::setAutoKey(false);
    auto finish = [&] {
        gWritten = nullptr;
        lila::scene::anim::setAutoKey(autoKeyWas);
        g.setTime(now, false);
        g.evaluate();
    };
    json out = json::array();
    std::vector<std::string> plugs;
    gWritten = &plugs;
    try
    {
        for (double t : times)
        {
            const double w = lila::scene::anim::timeSelectionWeight(sel, t);
            if (w <= 0.0)
                continue;
            g.setTime(t, false);
            g.evaluate();
            plugs.clear();
            json frame = json::array();
            for (NodeId n : nodes)
            {
                const Start& s0 = start[n];
                const glm::dmat4 here = worldOf(g, n);
                PoseInput p = in;
                if (relative)
                {
                    const glm::dmat3 carry = spaceAt(g, n, space) * glm::transpose(s0.space);
                    if (in.move)
                        p.move = carry * *in.move * w;
                    if (in.turn)
                        p.turn = partOf(glm::quat_cast(carry) * *in.turn * glm::inverse(glm::quat_cast(carry)), w);
                }
                else
                {
                    if (in.move)
                        p.move = (glm::dvec3(s0.world[3]) + *in.move - glm::dvec3(here[3])) * w;
                    if (in.turn)
                    {
                        const glm::dquat want = *in.turn * glm::quat_cast(math::rotationOf(s0.world));
                        p.turn = partOf(glm::normalize(want * glm::inverse(glm::quat_cast(math::rotationOf(here)))), w);
                    }
                }
                frame.push_back(poseOne(n, p));
            }
            if (preview)
            {
                g.evaluate();
                json at = json::array();
                for (NodeId n : nodes)
                    at.push_back(vecJson(glm::dvec3(worldOf(g, n)[3])));
                out.push_back({{"time", t}, {"weight", w}, {"at", at}});
                if (std::any_of(ghostTimes.begin(), ghostTimes.end(), [&](double x) { return std::abs(x - t) < 1e-9; }))
                    ghostFrames.push_back(ghosts::cache().captureNow(t));
                continue;
            }
            std::sort(plugs.begin(), plugs.end());
            plugs.erase(std::unique(plugs.begin(), plugs.end()), plugs.end());
            Solved here{t, {}};
            for (const std::string& plug : plugs)
            {
                const size_t dot = plug.rfind('.');
                const NodeId id = g.find(plug.substr(0, dot))->id();
                const int attr = plug.substr(dot + 1) == "rotate" ? int(Transform::kRotate) : int(Transform::kTranslate);
                here.values.push_back({plug, vecOf(g, id, attr)});
            }
            solved.push_back(std::move(here));
            out.push_back({{"time", t}, {"weight", w}, {"poses", frame}});
        }
    }
    catch (...)
    {
        finish();
        if (preview)
            restorePoseValues(g, unkeyed);
        throw;
    }
    if (preview)
    {
        finish();
        restorePoseValues(g, unkeyed);
        ghosts::cache().setPreview(std::move(ghostFrames));
        return out;
    }
    // Dense: the frames just past the edit keep their pose, so the frames beyond them don't change.
    if (a.get<bool>("dense", false) && !solved.empty())
    {
        std::vector<std::string> all;
        for (const Solved& f : solved)
            for (const auto& v : f.values)
                all.push_back(v.first);
        std::sort(all.begin(), all.end());
        all.erase(std::unique(all.begin(), all.end()), all.end());
        for (double t : {lila::scene::anim::secondsOf(lila::scene::anim::frameOf(times.front()) - 1.0),
                         lila::scene::anim::secondsOf(lila::scene::anim::frameOf(times.back()) + 1.0)})
        {
            g.setTime(t, false);
            g.evaluate();
            Solved edge{t, {}};
            for (const std::string& plug : all)
            {
                const size_t dot = plug.rfind('.');
                const int attr = plug.substr(dot + 1) == "rotate" ? int(Transform::kRotate) : int(Transform::kTranslate);
                edge.values.push_back({plug, vecOf(g, g.find(plug.substr(0, dot))->id(), attr)});
            }
            solved.push_back(std::move(edge));
        }
    }
    finish();
    LayerScope onLayer(g, a.get<std::string>("layer", ""));
    for (const Solved& f : solved)
        for (const auto& [plug, v] : f.values)
        {
            const size_t dot = plug.rfind('.');
            keyVec(plug.substr(0, dot), plug.substr(dot + 1), v, f.t);
        }
    g.evaluate();
    return out;
}

/** The range a command acts on: its range flag, else the time slider's selection. */
std::pair<double, double> rangeArg(const Args& a, const char* command)
{
    if (a.has("range"))
    {
        const json& r = a.flags["range"];
        if (!r.is_array() || r.size() != 2)
            throw CommandError(std::string(command) + ": range is [start, end] in seconds");
        return {std::min(r[0].get<double>(), r[1].get<double>()), std::max(r[0].get<double>(), r[1].get<double>())};
    }
    const lila::scene::anim::TimeSelection& sel = lila::scene::anim::timeSelection();
    if (!sel.active)
        throw CommandError(std::string(command) + ": give a range, or Shift + drag one on the time slider");
    return {sel.start, sel.end};
}

/** Every frame from start to end, in seconds. */
std::vector<double> framesOf(double start, double end)
{
    std::vector<double> out;
    for (double f = std::ceil(lila::scene::anim::frameOf(start) - 1e-6); f <= lila::scene::anim::frameOf(end) + 1e-6; f += 1.0)
        out.push_back(lila::scene::anim::secondsOf(f));
    return out;
}

/** Turns autokey off while frames are solved, keys their values once all are, and puts the time back. */
struct FrameKeys
{
    Graph& g;
    double now;
    bool autoKeyWas;
    std::vector<std::pair<double, std::vector<std::pair<std::string, glm::dvec3>>>> frames;

    explicit FrameKeys(Graph& graph) : g(graph), now(graph.time()), autoKeyWas(lila::scene::anim::autoKey())
    {
        lila::scene::anim::setAutoKey(false);
    }
    ~FrameKeys()
    {
        lila::scene::anim::setAutoKey(autoKeyWas);
        g.setTime(now, false);
        g.evaluate();
    }
    void record(double t, const std::vector<NodeId>& joints)
    {
        std::vector<std::pair<std::string, glm::dvec3>> values;
        for (NodeId j : joints)
        {
            values.push_back({g.node(j)->path() + ".rotate", vecOf(g, j, Transform::kRotate)});
            values.push_back({g.node(j)->path() + ".translate", vecOf(g, j, Transform::kTranslate)});
        }
        frames.push_back({t, std::move(values)});
    }
    void key(const std::string& layer = {})
    {
        LayerScope onLayer(g, layer);
        lila::scene::anim::setAutoKey(autoKeyWas);
        g.setTime(now, false);
        for (const auto& [t, values] : frames)
            for (const auto& [plug, v] : values)
            {
                const size_t dot = plug.rfind('.');
                keyVec(plug.substr(0, dot), plug.substr(dot + 1), v, t);
            }
    }
};

/** The joints a command acts on, parents first: the given joints, or every joint of the given characters. */
std::vector<NodeId> poseJoints(Graph& g, const Args& a, const char* command)
{
    std::vector<NodeId> out;
    for (const std::string& n : objectArgs(a))
    {
        Node& node = nodeNamed(g, n, command);
        if (node.type().isA(kCharacterType))
        {
            for (NodeId j : fullBodyJoints(g, characterRoot(g, node.id())))
                out.push_back(j);
            continue;
        }
        if (!rig::isJoint(node) || !characterRoot(g, characterFor(g, node.id())))
            throw CommandError(std::string(command) + ": " + node.path() + " is not a joint of a character");
        out.push_back(node.id());
    }
    if (out.empty())
        throw CommandError(std::string(command) + ": give joints or a character, or select them");
    std::stable_sort(out.begin(), out.end(), [&](NodeId x, NodeId y) { return depthIn(g, x) < depthIn(g, y); });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

glm::dquat rotationQuat(Graph& g, NodeId j)
{
    return glm::normalize(glm::quat_cast(glm::dmat3(eulerToMatrix(vecOf(g, j, Transform::kRotate), rotateOrderOf(g, j)))));
}

/**
 * Smooth over time: each frame's rotations (and translations) averaged with their neighbours', whole poses rather than
 * single curves, then the pins, locks and contacts put back where they were on that frame.
 */
json stringsSmooth(const Args& a)
{
    Graph& g = graph();
    const std::vector<NodeId> joints = poseJoints(g, a, "stringsSmooth");
    const auto [start, end] = rangeArg(a, "stringsSmooth");
    const double strength = std::clamp(a.get<double>("strength", 0.5), 0.0, 1.0);
    const int radius = std::clamp(a.get<int>("radius", 2), 1, 30);
    const std::vector<double> frames = framesOf(start, end);
    const std::vector<double> window = framesOf(lila::scene::anim::secondsOf(lila::scene::anim::frameOf(start) - radius),
                                                lila::scene::anim::secondsOf(lila::scene::anim::frameOf(end) + radius));
    FrameKeys keys(g);

    // Pass one: the poses as they are, and where every hold is on each frame.
    struct Sample
    {
        std::vector<glm::dquat> rotation;
        std::vector<glm::dvec3> translate;
        std::map<NodeId, std::vector<std::pair<NodeId, glm::dmat4>>> holds; // by character
    };
    std::map<int, Sample> sampled;
    std::vector<NodeId> characters;
    for (NodeId j : joints)
        if (const NodeId c = characterFor(g, j); std::find(characters.begin(), characters.end(), c) == characters.end())
            characters.push_back(c);
    for (double t : window)
    {
        g.setTime(t, false);
        g.evaluate();
        Sample& s = sampled[int(std::lround(lila::scene::anim::frameOf(t)))];
        for (NodeId j : joints)
        {
            s.rotation.push_back(rotationQuat(g, j));
            s.translate.push_back(vecOf(g, j, Transform::kTranslate));
        }
        if (a.get<bool>("holds", true))
            for (NodeId c : characters)
            {
                strings::Skeleton k = strings::snapshot(g, characterRoot(g, c));
                const CharacterRules rules = rulesOf(g, c);
                for (const strings::Hold& h : holdsFor(g, c, k, {}, contactsRef() ? rules.contacts : std::vector<std::string>{}))
                    s.holds[c].push_back({k.joints[size_t(h.bone)], h.world});
            }
    }

    // Pass two: each frame set to the smoothed pose, its holds put back, recorded; nothing keyed until all are done.
    for (double t : frames)
    {
        const int f = int(std::lround(lila::scene::anim::frameOf(t)));
        const Sample& here = sampled[f];
        g.setTime(t, false);
        g.evaluate();
        for (size_t i = 0; i < joints.size(); ++i)
        {
            glm::dvec4 sum(0.0);
            glm::dvec3 move(0.0);
            double total = 0.0;
            for (int o = -radius; o <= radius; ++o)
            {
                const auto it = sampled.find(f + o);
                if (it == sampled.end())
                    continue;
                const double w = std::exp(-0.5 * (o * o) / std::max(1.0, radius * radius * 0.25));
                glm::dquat q = it->second.rotation[i];
                if (glm::dot(q, here.rotation[i]) < 0.0)
                    q = -q;
                sum += w * glm::dvec4(q.x, q.y, q.z, q.w);
                move += w * it->second.translate[i];
                total += w;
            }
            const glm::dquat mean = glm::normalize(glm::dquat(sum.w, sum.x, sum.y, sum.z));
            const glm::dquat q = glm::slerp(here.rotation[i], mean, strength);
            g.setValue({joints[i], Transform::kRotate, -1}, rotateFor(g, joints[i], glm::dmat3(glm::mat3_cast(q))));
            // Only the hips' translation is motion; other joints' translations are their bones.
            if (depthIn(g, joints[i]) == depthIn(g, joints.front()))
                g.setValue({joints[i], Transform::kTranslate, -1}, glm::mix(here.translate[i], move / total, strength));
        }
        for (const auto& [c, holds] : here.holds)
        {
            g.evaluate();
            strings::Skeleton k = strings::snapshot(g, characterRoot(g, c));
            k.rules = bonesOf(k, g, rulesOf(g, c));
            std::vector<strings::Hold> bones;
            for (const auto& [j, w] : holds)
                if (const int b = k.boneOf(j); b >= 0)
                    bones.push_back({.bone = b, .world = w});
            strings::Pose pose = strings::startPose(k.problem);
            strings::holdLimbs(k.problem, pose, bones, k.hips, 1.0, {}, &k.rules);
            strings::apply(g, k, pose);
        }
        g.evaluate();
        std::vector<NodeId> all = joints;
        for (NodeId c : characters)
            for (NodeId j : fullBodyJoints(g, characterRoot(g, c)))
                all.push_back(j);
        std::sort(all.begin(), all.end());
        all.erase(std::unique(all.begin(), all.end()), all.end());
        keys.record(t, all);
    }
    keys.key(a.get<std::string>("layer", ""));
    return {{"frames", frames.size()}, {"joints", joints.size()}};
}

/** The time of the key before now on any of the joints, else the frame before. */
double previousTime(Graph& g, const std::vector<NodeId>& joints)
{
    const double now = g.time();
    double best = lila::scene::anim::secondsOf(lila::scene::anim::frameOf(now) - 1.0);
    bool found = false;
    for (double t : lila::scene::anim::keyTimes(g, joints))
        if (t < now - 1e-6 && (!found || t > best))
        {
            best = t;
            found = true;
        }
    return best;
}

/**
 * Looks at another time and comes back with unkeyed edits intact: stepping the time drops a held value (a pose set with
 * autokey off), so those are read first and set again after.
 */
template <class Fn> void atTime(Graph& g, double t, Fn&& fn)
{
    std::vector<std::pair<PlugRef, lila::scene::PlugValue>> held;
    for (const auto& [dst, src] : g.connections())
        if (g.isHeld(dst))
            held.push_back({dst, g.value(dst)});
    const double now = g.time();
    g.setTime(t, false);
    g.evaluate();
    fn();
    g.setTime(now, false);
    g.evaluate();
    for (const auto& [plug, v] : held)
        g.setValue(plug, v);
    g.evaluate();
}

/** Each joint's local rotation at time t. */
std::vector<glm::dquat> rotationsAt(Graph& g, const std::vector<NodeId>& joints, double t)
{
    std::vector<glm::dquat> out;
    atTime(g, t, [&] {
        for (NodeId j : joints)
            out.push_back(rotationQuat(g, j));
    });
    return out;
}

/** Poses each joint (parents first) to a local rotation in the mode, so holds keep. */
json poseToRotations(Graph& g, const std::vector<NodeId>& joints, const std::vector<glm::dquat>& target, strings::Mode mode, const Args& a)
{
    json out = json::array();
    PoseInput in{.holds = a.get<bool>("holds", true), .contacts = a.get<bool>("contacts", contactsRef())};
    for (size_t i = 0; i < joints.size(); ++i)
    {
        g.evaluate();
        in.rotation = target[i];
        out.push_back(poseJoint(g, joints[i], mode, in));
    }
    return out;
}

json stringsOvershoot(const Args& a)
{
    Graph& g = graph();
    const std::vector<NodeId> joints = poseJoints(g, a, "stringsOvershoot");
    const double amount = a.get<double>("amount", 0.25);
    const double from = a.has("from") ? a.get<double>("from") : previousTime(g, joints);
    const std::vector<glm::dquat> before = rotationsAt(g, joints, from);
    g.evaluate();
    std::vector<glm::dquat> target;
    for (size_t i = 0; i < joints.size(); ++i)
    {
        const glm::dquat now = rotationQuat(g, joints[i]);
        target.push_back(glm::normalize(now * partOf(glm::inverse(before[i]) * now, amount)));
    }
    return poseToRotations(g, joints, target, modeArg(a, "stringsOvershoot"), a);
}

json stringsBlend(const Args& a)
{
    Graph& g = graph();
    const std::vector<NodeId> joints = poseJoints(g, a, "stringsBlend");
    const double weight = std::clamp(a.get<double>("weight", 1.0), -1.0, 2.0);
    const std::string channels = a.get<std::string>("channels", "rotate");
    std::vector<glm::dquat> source(joints.size(), glm::dquat(1.0, 0.0, 0.0, 0.0));
    if (a.has("time"))
        source = rotationsAt(g, joints, a.get<double>("time"));
    g.evaluate();
    json out = json::object();
    if (channels == "rotate" || channels == "both")
    {
        std::vector<glm::dquat> target;
        for (size_t i = 0; i < joints.size(); ++i)
        {
            const glm::dquat now = rotationQuat(g, joints[i]);
            glm::dquat to = source[i];
            if (glm::dot(to, now) < 0.0)
                to = -to;
            target.push_back(glm::normalize(glm::slerp(now, to, weight)));
        }
        out["poses"] = poseToRotations(g, joints, target, modeArg(a, "stringsBlend"), a);
    }
    if (channels == "translate" || channels == "both")
    {
        // Translate toward the source time's, or toward the bind lengths for zero: a bone length reset.
        if (a.has("time"))
        {
            std::vector<glm::dvec3> to;
            atTime(g, a.get<double>("time"), [&] {
                for (NodeId j : joints)
                    to.push_back(vecOf(g, j, Transform::kTranslate));
            });
            for (size_t i = 0; i < joints.size(); ++i)
                g.setValue({joints[i], Transform::kTranslate, -1}, glm::mix(vecOf(g, joints[i], Transform::kTranslate), to[i], weight));
        }
        else
        {
            json names = json::array();
            for (NodeId j : joints)
                names.push_back(g.node(j)->path());
            out["lengths"] = ops::execute("stringsRestoreLengths", {{"args", names}});
        }
    }
    return out;
}

/** A character's pose to blend from or to: each joint's local rotation and translation, and its world parts' worlds. */
struct Keyframe
{
    std::vector<glm::dquat> rotation;
    std::vector<glm::dvec3> translate;
    std::vector<glm::dmat4> parts;
};

Keyframe keyframeNow(Graph& g, const std::vector<NodeId>& joints, const std::vector<NodeId>& parts)
{
    Keyframe k;
    for (NodeId j : joints)
    {
        k.rotation.push_back(rotationQuat(g, j));
        k.translate.push_back(vecOf(g, j, Transform::kTranslate));
    }
    for (NodeId p : parts)
        k.parts.push_back(worldOf(g, p));
    return k;
}

Keyframe keyframeAt(Graph& g, const std::vector<NodeId>& joints, const std::vector<NodeId>& parts, double t)
{
    Keyframe k;
    atTime(g, t, [&] { k = keyframeNow(g, joints, parts); });
    return k;
}

glm::dmat4 blendWorld(const glm::dmat4& a, const glm::dmat4& b, double f)
{
    glm::dquat qa = glm::quat_cast(math::rotationOf(a)), qb = glm::quat_cast(math::rotationOf(b));
    if (glm::dot(qa, qb) < 0.0)
        qb = -qb;
    glm::dmat4 out(glm::mat3_cast(glm::normalize(glm::slerp(qa, qb, f))));
    out[3] = glm::dvec4(glm::mix(glm::dvec3(a[3]), glm::dvec3(b[3]), f), 1.0);
    return out;
}

/** The joints of the character that stay in world space through an inbetween: its contacts (feet) unless told. */
std::vector<NodeId> worldParts(Graph& g, NodeId character, const Args& a)
{
    std::vector<NodeId> out;
    if (a.has("world"))
    {
        for (const json& n : a.flags["world"])
            out.push_back(jointNamed(g, n.get<std::string>(), "strings").id());
        return out;
    }
    const strings::Skeleton k = strings::snapshot(g, characterRoot(g, character));
    for (const std::string& c : rulesOf(g, character).contacts)
        for (NodeId j : k.joints)
            if (bareJoint(g.node(j)->name()) == bareJoint(c))
                out.push_back(j);
    return out;
}

/**
 * Sets the character, at the current time, to the pose f of the way from `from` to `to`: rotations blend in their parents'
 * space, so parts travel on arcs instead of cutting straight across, and the world parts (feet) blend in world, their
 * limbs bending to put them there.
 */
void inbetween(Graph& g, NodeId character, const std::vector<NodeId>& joints, const std::vector<NodeId>& parts, const Keyframe& from,
               const Keyframe& to, double f, const std::function<double(NodeId, double)>& weightOf = {})
{
    auto at = [&](NodeId j) { return weightOf ? weightOf(j, f) : f; };
    for (size_t i = 0; i < joints.size(); ++i)
    {
        const double fi = at(joints[i]);
        glm::dquat b = to.rotation[i];
        if (glm::dot(from.rotation[i], b) < 0.0)
            b = -b;
        g.setValue({joints[i], Transform::kRotate, -1}, rotateFor(g, joints[i], glm::dmat3(glm::mat3_cast(glm::normalize(glm::slerp(from.rotation[i], b, fi))))));
        if (glm::length(to.translate[i] - from.translate[i]) > 1e-9)
            g.setValue({joints[i], Transform::kTranslate, -1}, glm::mix(from.translate[i], to.translate[i], fi));
    }
    if (parts.empty())
        return;
    g.evaluate();
    strings::Skeleton k = strings::snapshot(g, characterRoot(g, character));
    k.rules = bonesOf(k, g, rulesOf(g, character));
    std::vector<strings::Hold> holds;
    for (size_t i = 0; i < parts.size(); ++i)
        if (const int b = k.boneOf(parts[i]); b >= 0)
            holds.push_back({.bone = b, .world = blendWorld(from.parts[i], to.parts[i], at(parts[i]))});
    strings::Pose pose = strings::startPose(k.problem);
    strings::holdLimbs(k.problem, pose, holds, k.hips, 1.0, {}, &k.rules);
    strings::apply(g, k, pose);
}

/** The keys either side of now on the joints: the previous strictly before, the next strictly after. */
std::pair<std::optional<double>, std::optional<double>> keysAround(Graph& g, const std::vector<NodeId>& joints, double now)
{
    std::optional<double> before, after;
    for (double t : lila::scene::anim::keyTimes(g, joints))
    {
        if (t < now - 1e-6)
            before = t;
        else if (t > now + 1e-6 && !after)
            after = t;
    }
    return {before, after};
}

/** The character a joints-or-characters command acts on; one per call. */
NodeId onlyCharacter(Graph& g, const std::vector<NodeId>& joints, const char* command)
{
    NodeId c = kNoNode;
    for (NodeId j : joints)
        if (const NodeId cj = characterFor(g, j); c && cj != c)
            throw CommandError(std::string(command) + ": give joints of one character");
        else
            c = cj;
    return c;
}

/** The distance from p to the nearest point of a polyline. */
double distanceToPath(const std::vector<glm::dvec3>& path, const glm::dvec3& p)
{
    double best = path.empty() ? 0.0 : glm::length(p - path.front());
    for (size_t i = 1; i < path.size(); ++i)
    {
        const glm::dvec3 d = path[i] - path[i - 1];
        const double t = glm::dot(d, d) > 1e-12 ? std::clamp(glm::dot(p - path[i - 1], d) / glm::dot(d, d), 0.0, 1.0) : 0.0;
        best = std::min(best, glm::length(path[i - 1] + t * d - p));
    }
    return best;
}

/** The joints of a line of action, top first: a line the rules name, two joints (top, end), or the line nearest the stroke. */
std::vector<NodeId> lineJoints(Graph& g, NodeId character, const Args& a, const std::vector<glm::dvec3>& stroke, std::string& name)
{
    const CharacterRules rules = rulesOf(g, character);
    auto resolve = [&](const std::vector<std::string>& ends) {
        if (ends.size() != 2)
            return std::vector<NodeId>{};
        const NodeId top = jointNamed(g, ends[0], "lineOfAction").id(), end = jointNamed(g, ends[1], "lineOfAction").id();
        std::vector<NodeId> out;
        for (const std::string& n : pathBetween(g, top, end))
            out.push_back(jointNamed(g, n, "lineOfAction").id());
        return out;
    };
    if (a.has("joints"))
    {
        name.clear();
        const std::vector<NodeId> out = resolve(a.flags["joints"].get<std::vector<std::string>>());
        if (out.size() < 2)
            throw CommandError("lineOfAction: joints is [top, end] with end below top");
        return out;
    }
    if (a.has("line"))
    {
        name = a.get<std::string>("line");
        const auto it = rules.lines.find(name);
        if (it == rules.lines.end())
            throw CommandError("lineOfAction: the character has no line named " + name);
        return resolve(it->second);
    }
    // The line whose joints lie nearest the stroke.
    double best = 1e300;
    std::vector<NodeId> out;
    g.evaluate();
    for (const auto& [n, ends] : rules.lines)
    {
        const std::vector<NodeId> js = resolve(ends);
        if (js.size() < 2)
            continue;
        std::vector<glm::dvec3> pts;
        for (NodeId j : js)
            pts.push_back(glm::dvec3(worldOf(g, j)[3]));
        double sum = 0.0;
        for (const glm::dvec3& p : stroke)
            sum += distanceToPath(pts, p);
        if (sum < best)
        {
            best = sum;
            out = js;
            name = n;
        }
    }
    if (out.empty())
        throw CommandError("lineOfAction: the character has no lines; give joints=[top, end]");
    return out;
}

/** What one frame's line of action did: the plugs it wrote, and how far the path missed. */
struct LineResult
{
    double error = 0.0;
    std::vector<std::string> plugs;
};

/** Points of a line and its bend parts (bone CVs, spline chain controls), each placed at its fraction along the path. */
struct LineParts
{
    std::vector<NodeId> solved;           // joints the full-body solve turns
    std::vector<NodeId> placed;           // CVs and curve controls moved onto the stroke
};

LineParts partsOf(Graph& g, const std::vector<NodeId>& joints)
{
    LineParts out;
    std::vector<NodeId> splines;
    for (NodeId j : joints)
    {
        if (const NodeId chain = lila::scene::spline::splineChainContaining(g, j))
        {
            if (std::find(splines.begin(), splines.end(), chain) == splines.end())
                splines.push_back(chain);
            continue;
        }
        out.solved.push_back(j);
        for (NodeId c : g.children(j))
            if (g.node(c)->type().name == "bendBoneCV")
                out.placed.push_back(c);
    }
    for (NodeId chain : splines)
        if (Node* curve = g.find(textOf(*g.node(chain), "curve")))
            for (NodeId c : lila::curve::curveControlNodes(g, *curve))
                out.placed.push_back(c);
    return out;
}

/** Poses one frame's line onto the stroke, weight w of the way. */
LineResult conformLine(Graph& g, NodeId character, const std::vector<NodeId>& joints, const std::vector<glm::dvec3>& stroke, double window,
                       double w, double maxStretch)
{
    g.evaluate();
    std::vector<glm::dvec3> path;
    for (NodeId j : joints)
        path.push_back(glm::dvec3(worldOf(g, j)[3]));
    const LineParts parts = partsOf(g, joints);
    std::vector<std::pair<NodeId, glm::dvec3>> placements;
    for (NodeId n : parts.placed)
        placements.push_back({n, strings::conformAt(path, stroke, window, strings::pathFraction(path, glm::dvec3(worldOf(g, n)[3])))});

    LineResult out;
    if (parts.solved.size() >= 2 || (parts.solved.size() == 1 && window >= 0.0))
    {
        const NodeId root = characterRoot(g, character);
        strings::Skeleton k = strings::snapshot(g, root, stiffnessOf(g, character, fullBodyJoints(g, joints.front())));
        const CharacterRules rules = rulesOf(g, character);
        std::vector<int> bones;
        std::vector<glm::dvec3> targets;
        for (NodeId j : parts.solved)
            if (const int b = k.boneOf(j); b >= 0)
            {
                bones.push_back(b);
                targets.push_back(strings::conformAt(path, stroke, window, strings::pathFraction(path, glm::dvec3(worldOf(g, j)[3]))));
            }
        const std::vector<strings::Hold> holds = holdsFor(g, character, k, {}, rules.contacts);
        strings::Result r = strings::conform(k.problem, bones, targets, holds, window >= 0.0 ? k.hips : -1, maxStretch);
        if (w < 1.0)
        {
            const strings::Pose s0 = strings::startPose(k.problem);
            for (size_t i = 0; i < r.pose.rotation.size(); ++i)
            {
                r.pose.rotation[i] = glm::slerp(s0.rotation[i], r.pose.rotation[i], w);
                r.pose.offset[i] = glm::mix(s0.offset[i], r.pose.offset[i], w);
                r.pose.stretch[i] = glm::mix(s0.stretch[i], r.pose.stretch[i], w);
            }
        }
        out.error = r.error;
        out.plugs = strings::apply(g, k, r.pose);
        g.evaluate();
    }
    for (auto& [n, target] : placements)
    {
        const NodeId parent = g.parent(n);
        const glm::dvec3 now(worldOf(g, n)[3]);
        const glm::dvec3 world = glm::mix(now, target, w);
        const glm::dmat4 parentWorld = parent ? worldOf(g, parent) : glm::dmat4(1.0);
        const glm::dvec3 local(glm::inverse(parentWorld) * glm::dvec4(world, 1.0));
        g.setValue({n, Transform::kTranslate, -1}, local);
        out.plugs.push_back(g.node(n)->path() + ".translate");
    }
    return out;
}

json lineOfAction(const Args& a)
{
    Graph& g = graph();
    std::vector<glm::dvec3> stroke;
    if (a.has("stroke"))
        for (const json& p : a.flags["stroke"])
            stroke.push_back({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()});
    if (stroke.size() < 2)
        throw CommandError("lineOfAction: give stroke, two or more world points");
    const NodeId character = stringsCharacterArg(g, a, "lineOfAction");
    std::string name;
    const std::vector<NodeId> joints = lineJoints(g, character, a, stroke, name);
    const double maxStretch = a.get<bool>("stretch", false) ? kStretchLimit : 1.0;
    const bool travel = a.get<bool>("travel", false);
    json out = {{"line", name}, {"joints", json::array()}};
    for (NodeId j : joints)
        out["joints"].push_back(g.node(j)->path());

    if (!a.has("range") && !a.has("frames"))
    {
        const LineResult r = conformLine(g, character, joints, stroke, travel ? 0.0 : -1.0, 1.0, maxStretch);
        written(r.plugs);
        out["error"] = r.error;
        return out;
    }

    // Across frames: each solved on its own pose, keyed once all are; a travelling stroke slides along the range.
    lila::scene::anim::TimeSelection sel{.active = true};
    std::vector<double> times;
    if (a.has("range"))
    {
        const auto [start, end] = rangeArg(a, "lineOfAction");
        sel.start = start;
        sel.end = end;
        sel.falloff = std::max(0.0, a.get<double>("falloff", 0.0));
        const double from = lila::scene::anim::frameOf(start) - sel.falloff, to = lila::scene::anim::frameOf(end) + sel.falloff;
        if (a.get<bool>("dense", false))
            times = framesOf(lila::scene::anim::secondsOf(from), lila::scene::anim::secondsOf(to));
        else
            for (double t : lila::scene::anim::keyTimes(g, fullBodyJoints(g, joints.front())))
                if (lila::scene::anim::frameOf(t) >= from - 1e-6 && lila::scene::anim::frameOf(t) <= to + 1e-6)
                    times.push_back(t);
    }
    else
    {
        times = a.flags["frames"].get<std::vector<double>>();
        sel.start = *std::min_element(times.begin(), times.end());
        sel.end = *std::max_element(times.begin(), times.end());
    }
    if (times.empty())
        throw CommandError("lineOfAction: no frames to pose in the range (no keys there; dense poses every frame)");
    FrameKeys keys(g);
    const double strokeLen = strings::strokeLength(stroke);
    json frames = json::array();
    for (double t : times)
    {
        const double w = lila::scene::anim::timeSelectionWeight(sel, t);
        if (w <= 0.0)
            continue;
        g.setTime(t, false);
        g.evaluate();
        double window = -1.0;
        if (travel)
        {
            double pathLen = 0.0;
            for (size_t i = 1; i < joints.size(); ++i)
                pathLen += glm::length(glm::dvec3(worldOf(g, joints[i])[3] - worldOf(g, joints[i - 1])[3]));
            const double u = sel.end > sel.start ? std::clamp((t - sel.start) / (sel.end - sel.start), 0.0, 1.0) : 0.0;
            window = u * std::max(0.0, strokeLen - pathLen);
        }
        const LineResult r = conformLine(g, character, joints, stroke, window, w, maxStretch);
        std::vector<NodeId> touched;
        for (const std::string& plug : r.plugs)
            touched.push_back(g.find(plug.substr(0, plug.rfind('.')))->id());
        std::sort(touched.begin(), touched.end());
        touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
        keys.record(t, touched);
        frames.push_back({{"time", t}, {"weight", w}, {"error", r.error}});
    }
    keys.key(a.get<std::string>("layer", ""));
    out["frames"] = frames;
    return out;
}

/**
 * Per-joint breakdown weights: favour lags the extremities behind the body (above 0) or leads with them (below 0), by
 * each joint's depth below the hips; parts names joints whose weight, and their children's, is set outright.
 */
/** Breakdown parts brushed over the mesh, by joint path: the fraction of the slider's weight that joint and its children take. */
std::map<std::string, double>& brushedParts()
{
    static std::map<std::string, double> parts;
    return parts;
}

json brushedJson()
{
    json out = json::object();
    for (const auto& [joint, f] : brushedParts())
        out[joint] = f;
    return out;
}

json stringsBreakdownParts(const Args& a)
{
    Graph& g = graph();
    // The selection may hold the character itself: only joints name parts.
    std::vector<std::string> joints;
    for (const std::string& n : objectArgs(a))
        if (Node* node = g.find(n); node && rig::isJoint(*node))
            joints.push_back(node->path());
        else if (a.has("factor"))
            throw CommandError("stringsBreakdownParts: " + n + " is not a joint");
    if (a.get<bool>("remove", false))
    {
        if (joints.empty())
            brushedParts().clear();
        for (const std::string& j : joints)
            brushedParts().erase(j);
    }
    else if (a.has("factor"))
    {
        if (joints.empty())
            throw CommandError("stringsBreakdownParts: give the joints whose part takes the factor");
        for (const std::string& j : joints)
            brushedParts()[j] = std::clamp(a.get<double>("factor"), 0.0, 2.0);
    }
    std::erase_if(brushedParts(), [&](const auto& e) { return !g.find(e.first); });
    viewportTools::requestRedraw();
    return brushedJson();
}

std::function<double(NodeId, double)> breakdownWeights(Graph& g, NodeId character, const Args& a)
{
    const double favour = std::clamp(a.get<double>("favour", 0.0), -1.0, 1.0);
    // {joint, value, scales}: an explicit part takes its value outright, a brushed one that fraction of the weight.
    struct Part
    {
        NodeId joint;
        double value;
        bool scales;
    };
    std::vector<Part> parts;
    if (a.has("parts"))
        for (const auto& [name, v] : a.flags["parts"].items())
            parts.push_back({jointNamed(g, name, "stringsBreakdown").id(), v.get<double>(), false});
    if (a.get<bool>("brushed", true))
        for (const auto& [path, f] : brushedParts())
            if (Node* n = g.find(path); n && characterFor(g, n->id()) == character &&
                std::none_of(parts.begin(), parts.end(), [&](const Part& p) { return p.joint == n->id(); }))
                parts.push_back({n->id(), f, true});
    if (favour == 0.0 && parts.empty())
        return {};
    const NodeId root = characterRoot(g, character);
    std::map<NodeId, int> depth;
    int deepest = 1;
    for (NodeId j : fullBodyJoints(g, root))
    {
        int d = 0;
        for (NodeId p = j; p && p != root; p = g.parent(p))
            ++d;
        depth[j] = d;
        deepest = std::max(deepest, d);
    }
    // Deeper parts listed later win, so a hand's override beats its arm's.
    std::stable_sort(parts.begin(), parts.end(), [&](const Part& x, const Part& y) { return depth[x.joint] < depth[y.joint]; });
    return [&g, favour, parts, depth, deepest](NodeId j, double f) {
        for (auto it = parts.rbegin(); it != parts.rend(); ++it)
            if (j == it->joint || isUnder(g, j, it->joint))
                return it->scales ? f * it->value : it->value;
        const auto d = depth.find(j);
        return f * std::clamp(1.0 - favour * (d == depth.end() ? 0.0 : double(d->second) / double(deepest)), 0.0, 2.0);
    };
}

/**
 * Breakdown in space: weight -1 is the previous key's pose, 0 now, 1 the next key's, past either overshoots. Rotations
 * blend in their parents' space (arcs), the feet in world, so planted feet stay planted between two planted keys.
 */
json stringsBreakdown(const Args& a)
{
    Graph& g = graph();
    const std::vector<NodeId> picked = poseJoints(g, a, "stringsBreakdown");
    const NodeId character = onlyCharacter(g, picked, "stringsBreakdown");
    const std::vector<NodeId> joints = a.get<bool>("all", true) ? fullBodyJoints(g, characterRoot(g, character)) : picked;
    const double w = std::clamp(a.get<double>("weight", 0.0), -2.0, 2.0);
    const auto [before, after] = keysAround(g, joints, g.time());
    const std::optional<double> other = w < 0.0 ? before : after;
    if (!other)
        throw CommandError(std::string("stringsBreakdown: no ") + (w < 0.0 ? "previous" : "next") + " key to blend toward");
    const std::vector<NodeId> parts = worldParts(g, character, a);
    g.evaluate();
    const Keyframe now = keyframeNow(g, joints, parts);
    const Keyframe target = keyframeAt(g, joints, parts, *other);
    inbetween(g, character, joints, parts, now, target, std::abs(w), breakdownWeights(g, character, a));
    return {{"from", g.time()}, {"toward", *other}, {"joints", joints.size()}};
}

/** Spacing from 0 to 1 over an inbetween: linear, slow in, slow out or both. */
double spacing(const std::string& curve, double x)
{
    if (curve == "easeIn")
        return x * x;
    if (curve == "easeOut")
        return 1.0 - (1.0 - x) * (1.0 - x);
    if (curve == "easeInOut")
        return x * x * (3.0 - 2.0 * x);
    return x;
}

/**
 * Fill a range with generated inbetweens: every frame (or every second or third) between each pair of keys in it becomes
 * a key of the space-aware blend, spaced by the curve. Each one is an ordinary key to edit after.
 */
json stringsFill(const Args& a)
{
    Graph& g = graph();
    const std::vector<NodeId> picked = poseJoints(g, a, "stringsFill");
    const NodeId character = onlyCharacter(g, picked, "stringsFill");
    const std::vector<NodeId> joints = fullBodyJoints(g, characterRoot(g, character));
    const auto [start, end] = rangeArg(a, "stringsFill");
    const int step = std::clamp(a.get<int>("step", 1), 1, 4);
    const std::string curve = a.get<std::string>("spacing", "easeInOut");
    const std::vector<NodeId> parts = worldParts(g, character, a);
    std::vector<double> keys;
    for (double t : lila::scene::anim::keyTimes(g, joints))
        if (t >= start - 1e-6 && t <= end + 1e-6)
            keys.push_back(t);
    if (keys.size() < 2)
        throw CommandError("stringsFill: the range needs two keys or more to fill between");
    std::vector<Keyframe> poses;
    for (double t : keys)
        poses.push_back(keyframeAt(g, joints, parts, t));
    FrameKeys out(g);
    int filled = 0;
    for (size_t i = 0; i + 1 < keys.size(); ++i)
    {
        const double f0 = lila::scene::anim::frameOf(keys[i]), f1 = lila::scene::anim::frameOf(keys[i + 1]);
        for (double f = f0 + step; f < f1 - 1e-6; f += step)
        {
            g.setTime(lila::scene::anim::secondsOf(f), false);
            g.evaluate();
            inbetween(g, character, joints, parts, poses[i], poses[i + 1], spacing(curve, (f - f0) / (f1 - f0)));
            g.evaluate();
            out.record(lila::scene::anim::secondsOf(f), joints);
            ++filled;
        }
    }
    out.key(a.get<std::string>("layer", ""));
    return {{"filled", filled}, {"between", keys.size() - 1}};
}

/**
 * A time-warp curve over a range: the fraction of the range each output fraction takes its pose from, monotone from
 * (0, 0) to (1, 1). warp points pass through a monotone cubic (Fritsch-Carlson); without them easeIn and easeOut set the
 * slopes at the ends, 1 holding the motion still there and -1 running it at double speed.
 */
std::function<double(double)> warpOf(const Args& a)
{
    std::vector<glm::dvec2> pts{{0.0, 0.0}};
    if (a.has("warp"))
        for (const json& p : a.flags["warp"])
        {
            if (!p.is_array() || p.size() != 2)
                throw CommandError("stringsRetime: warp is a list of [x, y] pairs, fractions of the range");
            const glm::dvec2 v(p[0].get<double>(), p[1].get<double>());
            if (v.x <= 0.0 || v.x >= 1.0)
                continue;
            if (v.x <= pts.back().x || v.y < pts.back().y || v.y > 1.0)
                throw CommandError("stringsRetime: warp points rise from (0, 0) to (1, 1), x increasing, y never falling");
            pts.push_back(v);
        }
    pts.push_back({1.0, 1.0});
    std::vector<double> m(pts.size());
    if (pts.size() == 2)
    {
        m[0] = 1.0 - std::clamp(a.get<double>("easeIn", 0.0), -1.0, 1.0);
        m[1] = 1.0 - std::clamp(a.get<double>("easeOut", 0.0), -1.0, 1.0);
    }
    else
    {
        std::vector<double> d(pts.size() - 1);
        for (size_t i = 0; i < d.size(); ++i)
            d[i] = (pts[i + 1].y - pts[i].y) / (pts[i + 1].x - pts[i].x);
        m.front() = d.front();
        m.back() = d.back();
        for (size_t i = 1; i + 1 < pts.size(); ++i)
            m[i] = d[i - 1] * d[i] <= 0.0 ? 0.0 : (d[i - 1] + d[i]) * 0.5;
        for (size_t i = 0; i < d.size(); ++i)
        {
            if (d[i] == 0.0)
            {
                m[i] = m[i + 1] = 0.0;
                continue;
            }
            const double al = m[i] / d[i], be = m[i + 1] / d[i];
            if (const double h = al * al + be * be; h > 9.0)
            {
                const double k = 3.0 / std::sqrt(h);
                m[i] = k * al * d[i];
                m[i + 1] = k * be * d[i];
            }
        }
    }
    return [pts, m](double x) {
        x = std::clamp(x, 0.0, 1.0);
        size_t i = 0;
        while (i + 2 < pts.size() && x > pts[i + 1].x)
            ++i;
        const double h = pts[i + 1].x - pts[i].x, t = (x - pts[i].x) / h;
        const double t2 = t * t, t3 = t2 * t;
        return std::clamp((2 * t3 - 3 * t2 + 1) * pts[i].y + (t3 - 2 * t2 + t) * h * m[i] + (-2 * t3 + 3 * t2) * pts[i + 1].y +
                              (t3 - t2) * h * m[i + 1],
                          0.0, 1.0);
    };
}

/**
 * Retime in place: every frame of the range takes the pose from the time the warp maps it to, sampled from the motion as
 * it is (keys or dense frames alike) and keyed on every frame. The range's ends keep their poses and the frames outside
 * it keep theirs: where the curves are sparse, the frames out to the neighbouring keys are keyed at what they showed.
 */
json stringsRetime(const Args& a)
{
    Graph& g = graph();
    const std::vector<NodeId> picked = poseJoints(g, a, "stringsRetime");
    const auto [start, end] = rangeArg(a, "stringsRetime");
    if (lila::scene::anim::frameOf(end) - lila::scene::anim::frameOf(start) < 2.0 - 1e-6)
        throw CommandError("stringsRetime: the range needs three frames or more");
    const std::function<double(double)> warp = warpOf(a);
    std::vector<std::pair<NodeId, int>> plugs;
    for (NodeId j : picked)
    {
        std::vector<int> attrs;
        for (const lila::scene::anim::AnimatedPlug& p : lila::scene::anim::animatedPlugs(g, j))
            if ((p.plug.attr == Transform::kTranslate || p.plug.attr == Transform::kRotate || p.plug.attr == Transform::kScale) &&
                std::find(attrs.begin(), attrs.end(), p.plug.attr) == attrs.end())
                attrs.push_back(p.plug.attr);
        for (int attr : attrs)
            plugs.push_back({j, attr});
    }
    if (plugs.empty())
        throw CommandError("stringsRetime: nothing given is animated");
    std::vector<NodeId> animated;
    for (const auto& [j, attr] : plugs)
        animated.push_back(j);
    const std::vector<double> keys = lila::scene::anim::keyTimes(g, animated);
    std::vector<double> edges;
    {
        const auto before = std::find_if(keys.rbegin(), keys.rend(), [&](double k) { return k < start - 1e-6; });
        const auto after = std::find_if(keys.begin(), keys.end(), [&](double k) { return k > end + 1e-6; });
        if (before != keys.rend())
            for (double t : framesOf(*before, start))
                if (t > *before + 1e-6 && t < start - 1e-6)
                    edges.push_back(t);
        if (after != keys.end())
            for (double t : framesOf(end, *after))
                if (t > end + 1e-6 && t < *after - 1e-6)
                    edges.push_back(t);
    }
    auto nameOf = [](int attr) { return attr == Transform::kTranslate ? "translate" : attr == Transform::kRotate ? "rotate" : "scale"; };

    // Everything is sampled before anything is keyed: a key set early would change the frames sampled after it.
    struct Sample
    {
        double t;
        std::vector<glm::dvec3> values;
    };
    std::vector<Sample> samples;
    const double now = g.time();
    auto sampleAt = [&](double src, double t) {
        g.setTime(src, false);
        g.evaluate();
        Sample s{t, {}};
        for (const auto& [j, attr] : plugs)
            s.values.push_back(vecOf(g, j, attr));
        samples.push_back(std::move(s));
    };
    const std::vector<double> frames = framesOf(start, end);
    auto sourceOf = [&](double t) { return start + warp((t - start) / (end - start)) * (end - start); };
    for (double t : frames)
        sampleAt(sourceOf(t), t);
    for (double t : edges)
        sampleAt(t, t);
    g.setTime(now, false);
    g.evaluate();

    const bool autoKeyWas = lila::scene::anim::autoKey();
    lila::scene::anim::setAutoKey(false);
    try
    {
        for (const Sample& s : samples)
            for (size_t i = 0; i < plugs.size(); ++i)
                keyVec(g.node(plugs[i].first)->path(), nameOf(plugs[i].second), s.values[i], s.t);
    }
    catch (...)
    {
        lila::scene::anim::setAutoKey(autoKeyWas);
        throw;
    }
    lila::scene::anim::setAutoKey(autoKeyWas);
    g.evaluate();
    json map = json::array();
    for (double t : frames)
        map.push_back(json::array({lila::scene::anim::frameOf(t), lila::scene::anim::frameOf(sourceOf(t))}));
    return {{"frames", frames.size()}, {"kept", edges.size()}, {"map", map}};
}

json stringsPose(const Args& a)
{
    Graph& g = graph();
    const strings::Mode mode = modeArg(a, "stringsPose");
    if (!a.has("by") && !a.has("axis"))
        throw CommandError("stringsPose: give by (a move) or axis and angle (a turn)");
    std::vector<NodeId> nodes;
    for (const std::string& s : objectArgs(a))
        nodes.push_back(nodeNamed(g, s, "stringsPose").id());
    if (nodes.empty())
        throw CommandError("stringsPose: give joints or select them");
    const PoseInput in = inputOf(g, a);
    auto poseOne = [&](NodeId n, const PoseInput& p) -> json {
        g.evaluate();
        Node& node = *g.node(n);
        if (rig::isJoint(node) && characterRoot(g, characterFor(g, n)))
            return poseJoint(g, n, mode, p);
        if (node.type().isA("transform") && related(g, n))
            return poseObject(g, n, p.move, p.turn, p.stretch);
        throw CommandError("stringsPose: " + node.path() + " is not in a character, nor glued or attached to anything");
    };
    json out = json::array();
    if (a.has("range") || a.has("frames"))
    {
        out = poseAcrossFrames(g, nodes, a, in, poseOne);
        return out;
    }
    for (NodeId n : nodes)
        out.push_back(poseOne(n, in));
    return out.size() == 1 ? out[0] : out;
}

struct ModeInfo
{
    strings::Mode mode;
    const char* id;
    const char* label;
    const char* icon;
    const char* doc;
};

constexpr ModeInfo kModes[] = {
    {strings::Mode::Forward, "puppeteer.strings.forward", "Forward", "arrow-down-tail",
     "Drags turn the joint and its children follow (FK). Pins and locks don't hold."},
    {strings::Mode::Backward, "puppeteer.strings.backward", "Backward", "arrow-up-tail",
     "Drags turn the hierarchy the other way: what is pinned or locked below stays, or the joint itself, and the body swings around it."},
    {strings::Mode::Ik, "puppeteer.strings.ik", "IK", "hand-grab",
     "Drags put the joint where you drag it by bending its limb; on the hips, the body moves and pinned feet stay."},
    {strings::Mode::FullBody, "puppeteer.strings.fullBody", "Full Body", "man",
     "Drags pose the whole body to put the joint where you drag it, pins and locked hands and feet holding."},
};

void registerActions(ext::Context& ctx)
{
    const char* cat = "Puppeteer Strings";
    for (const ModeInfo& m : kModes)
    {
        const strings::Mode mode = m.mode;
        actions::add(ctx, {.id = m.id,
                           .label = m.label,
                           .category = cat,
                           .run = [mode] { setStringsMode(mode); },
                           .doc = m.doc,
                           .checked = [mode] { return stringsMode() == mode; },
                           .icon = m.icon,
                           .keywords = {"strings", "ephemeral", "mode"}});
    }
    actions::add(ctx, {.id = "puppeteer.strings.next",
                       .label = "Next Strings Mode",
                       .category = cat,
                       .run = [] { setStringsMode(strings::Mode((int(stringsMode()) + 1) % 4)); },
                       .doc = "Forward, Backward, IK, Full Body, in turn.",
                       .icon = "arrows-right-left"});
    actions::add(ctx, {.id = "puppeteer.strings.stretch",
                       .label = "Stretch",
                       .category = cat,
                       .run =
                           [] {
                               stretchRef() = !stretchRef();
                               PrefStore::get().set(kPrefs, kStretchPref, stretchRef());
                               viewportTools::requestRedraw();
                           },
                       .doc = "Let bones lengthen, up to half again, when turning alone can't reach.",
                       .checked = [] { return stretchRef(); },
                       .icon = "arrows-diagonal"});
    actions::add(ctx, {.id = "puppeteer.strings.onLayer",
                       .label = "Range Edits on a Layer",
                       .category = cat,
                       .run =
                           [] {
                               onLayerRef() = !onLayerRef();
                               PrefStore::get().set(kPrefs, "/strings/onLayer", onLayerRef());
                           },
                       .doc = "Drags across a time range key onto the additive layer " + std::string(kStringsLayer) +
                              " instead of the base animation, to review, weight or drop.",
                       .checked = [] { return onLayerRef(); },
                       .icon = "stack-2",
                       .keywords = {"additive", "anim layer", "non-destructive"}});

    actions::add(ctx, {.id = "puppeteer.strings.contacts",
                       .label = "Contacts Hold",
                       .category = cat,
                       .run =
                           [] {
                               contactsRef() = !contactsRef();
                               PrefStore::get().set(kPrefs, kContactsPref, contactsRef());
                               viewportTools::requestRedraw();
                           },
                       .doc = "A character's contacts (its feet, from its rig template) hold still when you pose the rest, as pins do.",
                       .checked = [] { return contactsRef(); },
                       .icon = "shoe"});
    actions::add(ctx, {.id = "puppeteer.strings.zero",
                       .label = "Zero Pose",
                       .category = cat,
                       .doc = "Zero the selected joints' rotations in the current Strings mode: Backward keeps the pinned foot still.",
                       .command = "stringsZero",
                       .icon = "circle-dashed"});
    actions::add(ctx, {.id = "puppeteer.strings.restoreLengths",
                       .label = "Restore Bone Lengths",
                       .category = cat,
                       .doc = "Put stretched bones back to the length they were bound at.",
                       .command = "stringsRestoreLengths",
                       .icon = "ruler-measure"});
    actions::add(ctx, {.id = "puppeteer.strings.smooth",
                       .label = "Smooth Poses Over Range",
                       .category = cat,
                       .doc = "Smooth the selection's poses across the time slider's range (Shift + drag on it), feet and pins holding.",
                       .command = "stringsSmooth",
                       .icon = "wave-sine"});
    actions::add(ctx, {.id = "puppeteer.strings.overshoot",
                       .label = "Overshoot",
                       .category = cat,
                       .doc = "Push the selected joints past their pose, along the motion from the previous key.",
                       .command = "stringsOvershoot",
                       .icon = "arrow-bar-right"});
    actions::add(ctx, {.id = "puppeteer.strings.fill",
                       .label = "Fill Inbetweens",
                       .category = cat,
                       .doc = "Key inbetweens between the keys in the time slider's range: rotations on arcs, feet in world.",
                       .command = "stringsFill",
                       .icon = "timeline"});
    actions::add(ctx, {.id = "puppeteer.strings.retime",
                       .label = "Retime Range",
                       .category = cat,
                       .doc = "Retime the motion inside the time slider's range along a time-warp curve: ease into it, out of it, or "
                              "both, on every frame. The frames outside the range don't change.",
                       .command = "stringsRetime",
                       .icon = "clock-play",
                       .keywords = {"time warp", "timewarp", "speed ramp", "retime", "mocap", "dense"}});
    actions::add(ctx, {.id = "puppeteer.strings.glue",
                       .label = "Glue",
                       .category = cat,
                       .doc = "Glue the selection: whichever you pose, the others keep their place relative to it (hands together, "
                              "a hand on a prop). Lasts until unglued; never keyed.",
                       .command = "stringsGlue",
                       .icon = "link"});
    actions::add(ctx, {.id = "puppeteer.strings.attach",
                       .label = "Attach to Last Selected",
                       .category = cat,
                       .doc = "The selection follows the last selected as if parented to it (a hand on the head), without keying a space.",
                       .command = "stringsGlue",
                       .call = {{"attach", true}},
                       .icon = "paperclip"});
    actions::add(ctx, {.id = "puppeteer.strings.unglue",
                       .label = "Unglue",
                       .category = cat,
                       .doc = "Free the selection from glue and attach; with nothing selected, free everything.",
                       .command = "stringsGlue",
                       .call = {{"remove", true}},
                       .icon = "link-off"});

    // The modes live on the Spinner's arc; this menu holds what changes how a drag poses.
    markingMenus::add(ctx, "puppeteerStrings",
                      {{"N", {{"label", "Pin or Unpin"}, {"action", "puppeteer.anim.pin"}}},
                       {"W", {{"label", "Contacts Hold"}, {"action", "puppeteer.strings.contacts"}}},
                       {"E", {{"label", "Stretch"}, {"action", "puppeteer.strings.stretch"}}},
                       {"S", {{"label", "Zero Pose"}, {"action", "puppeteer.strings.zero"}}},
                       {"overflow", json::array({{{"label", "Glue"}, {"action", "puppeteer.strings.glue"}},
                                                 {{"label", "Attach to Last Selected"}, {"action", "puppeteer.strings.attach"}},
                                                 {{"label", "Unglue"}, {"action", "puppeteer.strings.unglue"}},
                                                 {{"separator", true}},
                                                 {{"label", "Motion Trail"}, {"action", "puppeteer.strings.trail"}},
                                                 {{"label", "Line of Action"}, {"action", "puppeteer.strings.lineOfAction"}},
                                                 {{"label", "Breakdown Parts"}, {"action", "puppeteer.strings.breakdownParts"}},
                                                 {{"label", "Restore Bone Lengths"}, {"action", "puppeteer.strings.restoreLengths"}}})}});
}
} // namespace

strings::Mode stringsMode() { return modeRef(); }

void setStringsMode(strings::Mode m)
{
    modeRef() = m;
    PrefStore::get().set(kPrefs, kModePref, std::string(strings::modeName(m)));
    viewportTools::requestRedraw();
}

double stringsStretch() { return stretchRef() ? kStretchLimit : 1.0; }

std::string stringsLayer() { return onLayerRef() ? kStringsLayer : std::string(); }

bool stringsRelated(NodeId node) { return related(graph(), node); }

std::vector<std::vector<NodeId>> stringsRelations()
{
    std::vector<std::vector<NodeId>> out;
    for (const Relation& r : relations())
        if (std::vector<NodeId> m = membersOf(graph(), r); m.size() >= 2)
            out.push_back(std::move(m));
    return out;
}

namespace
{
json relationsJson()
{
    json out = json::array();
    for (const Relation& r : relations())
        out.push_back({{"members", r.members}, {"attach", r.attach}});
    return out;
}

json stringsGlue(const Args& a)
{
    Graph& g = graph();
    std::vector<std::string> members;
    for (const std::string& n : objectArgs(a))
        members.push_back(nodeNamed(g, n, "stringsGlue").path());
    if (a.get<bool>("query", false))
        return relationsJson();
    if (a.get<bool>("remove", false))
    {
        if (members.empty())
            relations().clear();
        for (Relation& r : relations())
            std::erase_if(r.members, [&](const std::string& m) { return std::find(members.begin(), members.end(), m) != members.end(); });
        std::erase_if(relations(), [](const Relation& r) { return r.members.size() < 2; });
        viewportTools::requestRedraw();
        return relationsJson();
    }
    if (members.size() < 2)
        throw CommandError("stringsGlue: select two or more things to glue or attach");
    // A member belongs to one relation at a time: the newest wins.
    for (Relation& r : relations())
        std::erase_if(r.members, [&](const std::string& m) { return std::find(members.begin(), members.end(), m) != members.end(); });
    std::erase_if(relations(), [](const Relation& r) { return r.members.size() < 2; });
    relations().push_back({members, a.get<bool>("attach", false)});
    viewportTools::requestRedraw();
    return relationsJson();
}
} // namespace

void registerStrings(ext::Context& ctx)
{
    using T = ArgType;
    ctx.registerCommand({
        .name = "stringsPose",
        .doc = "Pose joints as a drag does, in a Strings mode: forward (FK), backward (the body swings around what holds "
               "still), ik (the limb bends to put the joint there) or fullBody (the whole body moves). Pins and locked "
               "hands and feet hold. Give by to move, axis and angle to turn. Returns what was posed, the anchor "
               "Backward kept still, and any fallback.",
        .positionalDoc = "joints (default the selection)",
        .flags = {
            {"layer", "ly", T::String, nullptr, "Key the edit onto this additive animation layer (made if missing) instead of the base."},
            {"mode", "m", T::String, nullptr, "forward, backward, ik or fullBody; default the current Strings mode.",
             {"forward", "backward", "ik", "fullBody"}},
            {"by", "by", T::Vec3, nullptr, "World offset, cm."},
            {"axis", "ax", T::Vec3, nullptr, "World axis to turn about."},
            {"angle", "a", T::Float, 0.0, "Degrees about axis."},
            {"anchors", "an", T::List, nullptr, "More joints that hold still for this pose, besides pins and locks."},
            {"holds", "h", T::Bool, true, "False ignores pins and locks."},
            {"contacts", "ct", T::Bool, nullptr, "The character's contacts (feet, from its rig template) hold too; default the Contacts setting."},
            {"chainLength", "cl", T::Int, 0, "ik: how many bones bend, the joint first, through branches; 0 its limb."},
            {"stretch", "st", T::Float, 1.0, "Above 1 lets bones lengthen up to this multiple when turning can't reach."},
            {"autoPose", "ap", T::Bool, false, "fullBody moves: the auto-poser places the unpinned body (UE5 Manny joint names)."},
            {"range", "r", T::List, nullptr, "[start, end] seconds: pose every keyed frame in it (or every frame with dense), keying each."},
            {"frames", "fr", T::List, nullptr, "Seconds to pose at, keying each, instead of a range."},
            {"dense", "dn", T::Bool, false, "With range: every frame, not only the keyed ones."},
            {"falloff", "fo", T::Float, 0.0, "With range: frames either side the pose eases out over."},
            {"preview", "pv", T::Bool, false, "With range or frames: solve without keying, for a drag in progress. Ghosts show the result; returns each frame's joint positions."},
            {"show", "sw", T::List, nullptr, "With preview: more seconds to solve, such as a motion trail's frames."},
            {"relative", "rel", T::Bool, true, "Across frames: offset each frame by the drag. False brings each to where the drag put it now."},
            {"space", "sp", T::String, "world", "Across frames, relative: what the offset follows: world, parent, or a node (move a hand in head space)."},
        },
        .run = stringsPose,
    });
    ctx.registerCommand({
        .name = "stringsGlue",
        .doc = "Glue things so they keep their place relative to each other through every Strings pose (two hands, a hand "
               "and a prop), or with attach make all but the last follow the last, as if parented. Lasts for the session, "
               "never saved or keyed. Returns every glue and attach.",
        .positionalDoc = "transforms or joints (default the selection)",
        .flags = {
            {"attach", "at", T::Bool, false, "All but the last follow the last; the last is not moved by them."},
            {"remove", "rm", T::Bool, false, "Free these from glue and attach; with none given, free everything."},
            {"query", "q", T::Bool, false, "Only return the glue and attach sets."},
        },
        .run = stringsGlue,
    });
    ctx.registerCommand({
        .name = "stringsZero",
        .doc = "Zero joints' rotations in a Strings mode: forward zeroes them as they are, backward keeps what is pinned below "
               "still and swings the body, and so on. relativeTo zeroes relative to other joints, which stay where they are. "
               "Parents first. Returns what each pose did.",
        .positionalDoc = "joints (default the selection)",
        .flags = {
            {"mode", "m", T::String, nullptr, "forward, backward, ik or fullBody; default the current Strings mode.",
             {"forward", "backward", "ik", "fullBody"}},
            {"holds", "h", T::Bool, true, "False ignores pins and locks."},
            {"stretch", "st", T::Float, 1.0, "Above 1 lets bones lengthen up to this multiple."},
            {"relativeTo", "rt", T::List, nullptr, "Joints that keep their place in the world while the others zero (relative zero)."},
        },
        .run = stringsZero,
    });
    ctx.registerCommand({
        .name = "stringsRules",
        .doc = "A character's Strings rules: its contacts (joints that hold still while you pose the rest, its feet) and its "
               "limbs (what bends together when IK or a hold reaches). They come from its rig template or rig elements; "
               "contacts and addChain set them on this character instead, reset goes back. Returns the rules and where "
               "each came from.",
        .positionalDoc = "character or one of its joints (default the selection)",
        .flags = {
            {"contacts", "ct", T::List, nullptr, "Joints that hold by default, replacing the inherited ones."},
            {"addChain", "ac", T::List, nullptr, "[top joint, end joint]: a limb from top down to end, replacing limbs it overlaps."},
            {"addLine", "al", T::List, nullptr, "[name, top joint, end joint]: a line of action a stroke may bind to."},
            {"removeChain", "rc", T::String, nullptr, "A joint: the limb it is in goes."},
            {"removeLine", "rl", T::String, nullptr, "The line of action with this name goes."},
            {"reset", "rs", T::Bool, false, "Forget this character's own rules first."},
        },
        .run = stringsRules,
    });
    ctx.registerCommand({
        .name = "lineOfAction",
        .doc = "Pose a body path to a stroke's shape (line of action): the path's joints bend along the stroke through a "
               "full-body solve with each joint's stiffness, contacts and pins holding; bone CVs and spline chain controls on "
               "the path move onto it too. The stroke is world points, scaled to the path's length and kept at its first "
               "joint. With range or frames it poses each frame, travel slides the body along the stroke over the range. "
               "Returns the line, its joints and the error.",
        .positionalDoc = "character or one of its joints (default the selection)",
        .flags = {
            {"layer", "ly", T::String, nullptr, "Key the edit onto this additive animation layer (made if missing) instead of the base."},
            {"stroke", "st", T::List, nullptr, "World points along the stroke, [[x, y, z], ...]."},
            {"line", "l", T::String, nullptr, "A line of action the character's rules name (body, a limb); default the one nearest the stroke."},
            {"joints", "j", T::List, nullptr, "[top joint, end joint] instead of a named line."},
            {"range", "r", T::List, nullptr, "[start, end] seconds to pose across; default this frame only."},
            {"frames", "f", T::List, nullptr, "Times in seconds to pose at, instead of a range."},
            {"falloff", "fo", T::Float, 0.0, "Frames either side of the range the edit eases out over.", {}, {}, 0.0},
            {"dense", "d", T::Bool, false, "Pose every frame of the range, not only its keys."},
            {"travel", "tr", T::Bool, false, "The body lies on the stroke unscaled and slides along it over the range (a path stroke)."},
            {"stretch", "sx", T::Bool, false, "Let bones lengthen when turning alone can't reach the stroke."},
        },
        .run = lineOfAction,
        .examples = {"lineOfAction(\"manny\", line=\"body\", stroke=[[0, 100, 0], [0, 140, 15], [0, 170, 40]])"},
    });
    ctx.registerCommand({
        .name = "stringsSmooth",
        .doc = "Smooth whole poses over a range of frames: each frame's rotations average with their neighbours' (and the "
               "top joint's translation), then pins, locks and contacts go back where they were on that frame. Keys every "
               "frame in the range. Acts on the time slider's range without one.",
        .positionalDoc = "joints or characters (default the selection)",
        .flags = {
            {"layer", "ly", T::String, nullptr, "Key the edit onto this additive animation layer (made if missing) instead of the base."},
            {"range", "r", T::List, nullptr, "[start, end] seconds; default the time slider's range."},
            {"strength", "s", T::Float, 0.5, "0 leaves the poses, 1 takes the average.", {}, {}, 0.0, 1.0},
            {"radius", "rad", T::Int, 2, "Frames either side averaged.", {}, {}, 1.0, 30.0},
            {"holds", "h", T::Bool, true, "False lets pins, locks and contacts move with the smoothing."},
        },
        .run = stringsSmooth,
    });
    ctx.registerCommand({
        .name = "stringsOvershoot",
        .doc = "Push joints past their pose, along the motion from the previous key (or from), in a Strings mode so holds "
               "keep. Negative amounts pull back toward it.",
        .positionalDoc = "joints or characters (default the selection)",
        .flags = {
            {"amount", "a", T::Float, 0.25, "Fraction of the motion since the previous key to carry on by."},
            {"from", "f", T::Float, nullptr, "Seconds to measure the motion from; default the previous key."},
            {"mode", "m", T::String, nullptr, "forward, backward, ik or fullBody; default the current Strings mode.",
             {"forward", "backward", "ik", "fullBody"}},
            {"holds", "h", T::Bool, true, "False ignores pins and locks."},
        },
        .run = stringsOvershoot,
    });
    ctx.registerCommand({
        .name = "stringsBlend",
        .doc = "Blend joints toward their pose at another time, or toward zero, in a Strings mode so holds keep. channels "
               "picks rotations, translations (toward zero: bind lengths) or both.",
        .positionalDoc = "joints or characters (default the selection)",
        .flags = {
            {"time", "t", T::Float, nullptr, "Seconds whose pose to blend toward; leave out for zero."},
            {"weight", "w", T::Float, 1.0, "How far: 1 all the way, 0 not at all, past 1 overshoots."},
            {"channels", "ch", T::String, "rotate", "rotate, translate or both.", {"rotate", "translate", "both"}},
            {"mode", "m", T::String, nullptr, "forward, backward, ik or fullBody; default the current Strings mode.",
             {"forward", "backward", "ik", "fullBody"}},
            {"holds", "h", T::Bool, true, "False ignores pins and locks."},
        },
        .run = stringsBlend,
    });
    ctx.registerCommand({
        .name = "stringsBreakdown",
        .doc = "A breakdown in space: blend the character toward its previous key's pose (weight -1) or next (1), "
               "rotations in their parents' space so parts travel on arcs, and the feet in world so planted feet stay "
               "planted. Past 1 overshoots.",
        .positionalDoc = "joints or a character (default the selection)",
        .flags = {
            {"weight", "w", T::Float, 0.0, "-1 the previous key's pose, 0 now, 1 the next key's."},
            {"favour", "fv", T::Float, 0.0, "Above 0 the hands, feet and head lag behind the body; below 0 they lead.", {}, {}, -1.0, 1.0},
            {"parts", "pt", T::Any, nullptr, "{joint: weight}: those joints and their children take that weight instead."},
            {"brushed", "br", T::Bool, true, "Parts brushed with the Breakdown Parts tool take their fraction of the weight; False ignores them."},
            {"all", "al", T::Bool, true, "The whole character; False only the given joints."},
            nodeFlag({"world", "wo", T::List, nullptr, "Joints that blend in world space; default the character's contacts."}, "joint"),
        },
        .run = stringsBreakdown,
    });
    ctx.registerCommand({
        .name = "stringsBreakdownParts",
        .doc = "Breakdown parts: joints whose part (the joint and its children) takes a fraction of the breakdown weight, as "
               "the Breakdown Parts tool brushes them over the mesh. 0 holds the part in its pose, 1 follows the slider, "
               "between lags it. Kept for the session, never keyed. Returns {joint: factor}.",
        .positionalDoc = "joints",
        .flags = {
            {"factor", "f", T::Float, nullptr, "Set the joints' parts to this fraction of the weight.", {}, {}, 0.0, 2.0},
            {"remove", "rm", T::Bool, false, "Remove the joints' parts; with no joints, all of them."},
        },
        .undoable = false,
        .run = stringsBreakdownParts,
    });
    ctx.registerCommand({
        .name = "stringsFill",
        .doc = "Fill a range with inbetweens: each frame (or every step frames) between its keys becomes a key of the "
               "blend between them, rotations on arcs and feet in world, spaced by the curve. Acts on the time slider's "
               "range without one.",
        .positionalDoc = "joints or a character (default the selection)",
        .flags = {
            {"layer", "ly", T::String, nullptr, "Key the edit onto this additive animation layer (made if missing) instead of the base."},
            {"range", "r", T::List, nullptr, "[start, end] seconds; default the time slider's range."},
            {"step", "st", T::Int, 1, "1 every frame, 2 on twos, 3 on threes.", {}, {}, 1.0, 4.0},
            {"spacing", "sp", T::String, "easeInOut", "How the inbetweens are spaced.", {"linear", "easeIn", "easeOut", "easeInOut"}},
            nodeFlag({"world", "wo", T::List, nullptr, "Joints that blend in world space; default the character's contacts."}, "joint"),
        },
        .run = stringsFill,
    });
    ctx.registerCommand({
        .name = "stringsRetime",
        .doc = "Retime in place along a time-warp curve: every frame of the range takes the pose from the time the curve "
               "maps it to, sampled from the motion as it is (keyed or dense) and keyed on every frame. The range's ends "
               "and every frame outside it keep their poses. Acts on the time slider's range without one. Returns "
               "{frames, kept, map}: frames keyed in the range, frames outside keyed to keep their pose, and [frame, "
               "source frame] pairs.",
        .positionalDoc = "joints or characters (default the selection)",
        .flags = {
            {"range", "r", T::List, nullptr, "[start, end] seconds; default the time slider's range."},
            {"easeIn", "ei", T::Float, 0.0, "1 eases into the range (the motion starts still), -1 starts at double speed.", {}, {}, -1.0, 1.0},
            {"easeOut", "eo", T::Float, 0.0, "1 eases out of the range (the motion settles), -1 ends at double speed.", {}, {}, -1.0, 1.0},
            {"warp", "wp", T::List, nullptr, "[[x, y], ...]: the curve through these points instead, x the fraction of the range and y the "
                                            "fraction its pose comes from, rising from (0, 0) to (1, 1)."},
        },
        .run = stringsRetime,
    });
    ops::options::setOverride("stringsRetime", {.order = {"easeIn", "easeOut"}, .hidden = {"range", "warp"}});
    ctx.registerCommand({
        .name = "stringsRuleWeb",
        .doc = "The character's rule web as data for Unreal: its hips, contacts, limbs, lines of action and full-body stiffness, "
               "by bone name, plus the bones and their parents. Writes it to file as JSON when given. Returns the web.",
        .positionalDoc = "character or one of its joints (default the selection)",
        .flags = {{"file", "f", T::String, nullptr, "A .json file to write the web to."}},
        .undoable = false,
        .run = stringsRuleWeb,
    });
    ctx.registerCommand({
        .name = "unrealStringsRig",
        .doc = "Pose the character in Unreal the way Strings poses it here: LilaLink makes or updates a Control Rig on its skeletal "
               "mesh: a control on the hips and the end of each line of action and limb (hands, feet, head) and each contact, each posing its bone through a Lila "
               "Strings Full Body unit given the character's rule web; swap a unit for Forward, Backward or IK to change how it poses. "
               "Returns {rig, skeletalMesh, unreal}, or {rig, skeletalMesh, payload} with dryRun.",
        .positionalDoc = "character or one of its joints (default the selection)",
        .flags = {{"skeletalMesh", "sm", T::String, nullptr, "The skeletal mesh asset. Default: the root's skeletal mapping."},
                  {"rig", "r", T::String, nullptr, "The Control Rig asset to make or update. Default: CR_<mesh>_Strings beside the mesh."},
                  {"dryRun", "dry", T::Bool, false, "Return what would be sent without connecting to Unreal."}},
        .undoable = false,
        .run = unrealStringsRig,
    });
    ctx.registerCommand({
        .name = "stringsRestoreLengths",
        .doc = "Put stretched bones back to the length they were bound at, each keeping its direction. Joints that drive no "
               "skin have no bind length and are skipped. Returns the joints restored and skipped.",
        .positionalDoc = "joints (default the selection)",
        .run = stringsRestoreLengths,
    });
    registerActions(ctx);
}
} // namespace puppeteer
