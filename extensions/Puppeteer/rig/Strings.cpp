// Strings on Puppeteer's characters: the mode drags pose in, what holds still (pins and locked hands and feet), and
// stringsPose, which poses joints the way a Spinner drag does.

#include "Puppeteer.h"
#include "rig/Rig.h"

#include <lila/Math.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>
#include <lila/Curves.h>
#include <lila/Ghosts.h>
#include <lila/IkSolver.h>
#include <lila/OptionBox.h>
#include <lila/Paths.h>
#include <lila/Poses.h>
#include <lila/SplineChain.h>
#include <lila/Strings.h>
#include <lila/Tasks.h>
#include <lila/Transform.h>
#include <lila/UI.h>
#include <lila/ViewportTool.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <lila/Anim.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>

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

bool& isolateRef()
{
    static bool on = PrefStore::get().value<bool>(kPrefs, "/strings/isolate", false);
    return on;
}

/** How a turn across a time range lands on each frame: "local" about the joint where it is then, "pivot" about where it is now. */
std::string& rotationTypeRef()
{
    static std::string t = PrefStore::get().value<std::string>(kPrefs, "/strings/rotationType", "local");
    return t;
}

/** The mode names, for flags and messages. */
std::vector<std::string> modeChoices()
{
    std::vector<std::string> out;
    for (strings::Mode m : strings::kModes)
        out.push_back(strings::modeName(m));
    return out;
}

/** Whether a breakdown or tween in this mode keeps the contacts in world: the plain modes blend every joint locally. */
bool holdsInWorld(strings::Mode m) { return m != strings::Mode::ForwardAll && m != strings::Mode::All && m != strings::Mode::Free; }

std::string modeList()
{
    std::string out;
    const std::vector<std::string> names = modeChoices();
    for (size_t i = 0; i < names.size(); ++i)
        out += (i == 0 ? "" : i + 1 == names.size() ? " or " : ", ") + names[i];
    return out;
}

std::string bareJoint(const std::string& name) { return name.substr(name.find_last_of("|:") + 1); }

/** A character's rule web: the limbs its rig declares and the joints that hold by default (its contacts, feet). */
struct CharacterRules
{
    std::vector<std::vector<std::string>> chains; // joint names, top to end
    std::vector<std::string> contacts;
    /** Lines of action: body paths a stroke may bind to, by name, each [top joint, end joint]. */
    std::map<std::string, std::vector<std::string>> lines;
    /** Joints that keep their world rotation while Isolate Rotation is on (the head, the hands). */
    std::vector<std::string> isolate;
    /** Default mode: the mode a joint poses in, by joint name. */
    std::map<std::string, std::string> modes;
    /** Where each came from: "character", "template", "rig elements", "inferred" or "none". */
    std::string chainsFrom = "none", contactsFrom = "none", linesFrom = "none", isolateFrom = "none", modesFrom = "none";
};

/**
 * Isolation when nothing declares it: the head, and the end of each limb that isn't a contact and doesn't hang off
 * another limb's end or a contact (the hands, not the fingers or the balls of the feet).
 */
void inferIsolate(Graph& g, NodeId character, CharacterRules& r)
{
    r.isolate.clear();
    r.isolateFrom = "inferred";
    const NodeId root = characterRoot(g, character);
    if (!root)
        return;
    std::map<std::string, NodeId> joint;
    for (NodeId d : g.descendants(root))
        joint[bareJoint(g.node(d)->name())] = d;
    joint[bareJoint(g.node(root)->name())] = root;
    std::vector<NodeId> ends;
    for (const auto& chain : r.chains)
        if (const auto it = joint.find(bareJoint(chain.back())); it != joint.end())
            ends.push_back(it->second);
    for (const std::string& c : r.contacts)
        if (const auto it = joint.find(bareJoint(c)); it != joint.end())
            ends.push_back(it->second);
    auto keep = [&](const std::string& name) {
        if (std::find(r.isolate.begin(), r.isolate.end(), name) == r.isolate.end())
            r.isolate.push_back(name);
    };
    for (const auto& chain : r.chains)
    {
        const auto it = joint.find(bareJoint(chain.back()));
        if (it == joint.end() || std::find(r.contacts.begin(), r.contacts.end(), chain.back()) != r.contacts.end())
            continue;
        const bool below = std::any_of(ends.begin(), ends.end(), [&](NodeId e) { return e != it->second && isUnder(g, it->second, e); });
        if (!below)
            keep(chain.back());
    }
    if (joint.contains("head"))
        keep("head");
}

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
            if (c.contains("isolate"))
            {
                for (const json& m : c["isolate"])
                    out.isolate.push_back(jointOf(m.get<std::string>()));
                out.isolateFrom = "template";
            }
            else
                inferIsolate(g, character, out);
            if (c.contains("modes"))
            {
                for (const auto& [marker, mode] : c["modes"].items())
                    out.modes[jointOf(marker)] = mode.get<std::string>();
                out.modesFrom = "template";
            }
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
    inferIsolate(g, character, out);
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
    if (own.contains("isolate") && own["isolate"].is_array())
    {
        out.isolate = own["isolate"].get<std::vector<std::string>>();
        out.isolateFrom = "character";
    }
    if (own.contains("modes") && own["modes"].is_object())
    {
        out.modes = own["modes"].get<std::map<std::string, std::string>>();
        out.modesFrom = "character";
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
    for (const auto& [joint, name] : r.modes)
        if (const auto it = bone.find(bareJoint(joint)); it != bone.end())
            if (const auto m = strings::modeFromName(name))
                out.defaults.push_back({it->second, *m});
    return out;
}

/** The rules' isolated joints as bones, when Isolate Rotation is on. */
std::vector<int> isolatedBones(const strings::Skeleton& k, Graph& g, const CharacterRules& r)
{
    std::vector<int> out;
    if (!isolateRef())
        return out;
    for (const std::string& n : r.isolate)
        for (size_t i = 0; i < k.joints.size(); ++i)
            if (bareJoint(g.node(k.joints[i])->name()) == bareJoint(n))
                out.push_back(int(i));
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

/** While one lives, rulesOf reads each character's rules once: a pose across many frames asks for them on every frame. */
std::map<NodeId, CharacterRules>* gRulesCache = nullptr;

struct RulesCacheScope
{
    std::map<NodeId, CharacterRules> cache;
    std::map<NodeId, CharacterRules>* was = gRulesCache;
    RulesCacheScope() { gRulesCache = gRulesCache ? gRulesCache : &cache; }
    ~RulesCacheScope() { gRulesCache = was; }
    RulesCacheScope(const RulesCacheScope&) = delete;
    RulesCacheScope& operator=(const RulesCacheScope&) = delete;
};

const CharacterRules& cachedRules(Graph& g, NodeId character, CharacterRules& local)
{
    if (!gRulesCache)
        return local = rulesOf(g, character);
    const auto it = gRulesCache->find(character);
    return it != gRulesCache->end() ? it->second : gRulesCache->emplace(character, rulesOf(g, character)).first->second;
}

/** A joint's interaction set up: its skeleton as it is now and the request, ready to solve with any drag. */
struct Prepared
{
    strings::Skeleton k;
    strings::Request r;
    int bone = -1;
    NodeId joint = kNoNode;
    strings::Mode mode = strings::Mode::Forward;
};

Prepared prepareJoint(Graph& g, NodeId joint, strings::Mode mode, const PoseInput& in)
{
    const std::string path = g.node(joint)->path();
    const NodeId character = characterFor(g, joint);
    const NodeId root = characterRoot(g, character);
    if (!root)
        throw CommandError("stringsPose: " + path + " is not in a character");
    Prepared p;
    p.joint = joint;
    p.mode = mode;
    p.k = strings::snapshot(g, root, stiffnessOf(g, character, fullBodyJoints(g, joint)));
    p.bone = p.k.boneOf(joint);
    if (p.bone < 0)
        throw CommandError("stringsPose: " + path + " is not a deform joint of its character");
    CharacterRules local;
    const CharacterRules& rules = cachedRules(g, character, local);
    p.k.rules = bonesOf(p.k, g, rules);
    p.r = {.mode = mode,
           .grabbed = p.bone,
           .turn = in.rotation ? std::optional(turnTo(p.k.problem, p.bone, *in.rotation)) : in.turn,
           .move = in.move,
           .holds = in.holds ? holdsFor(g, character, p.k, in.anchors, in.contacts ? rules.contacts : std::vector<std::string>{})
                             : std::vector<strings::Hold>{},
           .hips = p.k.hips,
           .chainLength = in.chainLength,
           .maxStretch = in.stretch,
           .isolate = isolatedBones(p.k, g, rules)};
    std::erase_if(p.r.holds, [&](const strings::Hold& h) { return h.bone == p.bone; });
    return p;
}

/** Solves a prepared joint with the drag in r; sets fellBack when the mode couldn't move it and Forward did. Thread safe. */
strings::Result solvePrepared(const Prepared& p, strings::Request r, bool& fellBack)
{
    r.rules = &p.k.rules;
    // Full Body turning with nothing held is a plain turn: no solve has anything to keep.
    if (r.mode == strings::Mode::FullBody && !r.move && r.holds.empty())
        r.mode = strings::Mode::ForwardAll;
    strings::Result res = strings::solve(p.k.problem, r);
    fellBack = false;
    if (!res.handled)
    {
        r.mode = strings::Mode::Forward;
        res = strings::solve(p.k.problem, r);
        fellBack = true;
    }
    return res;
}

json poseJoint(Graph& g, NodeId joint, strings::Mode mode, const PoseInput& in)
{
    const std::string path = g.node(joint)->path();
    if (!characterRoot(g, characterFor(g, joint)))
        throw CommandError("stringsPose: " + path + " is not in a character");
    json out = {{"joint", path}, {"mode", strings::modeName(mode)}};

    // The auto-poser runs inside full-body moves only; the rest of Full Body goes through the same solve as the others.
    if (mode == strings::Mode::FullBody && in.move && !in.turn && !in.rotation && in.autoPose)
    {
        const std::vector<NodeId> posed = fullBodyMove(g, {joint}, *in.move, true);
        if (posed.empty())
            throw CommandError("stringsPose: " + path + " cannot be posed full body (a root above the hips moves the ordinary way)");
        return out;
    }

    Prepared p = prepareJoint(g, joint, mode, in);
    const strings::Skeleton& k = p.k;
    const int bone = p.bone;
    const strings::Request& r = p.r;
    bool fellBack = false;
    strings::Result res = solvePrepared(p, r, fellBack);
    if (fellBack)
    {
        // A joint above the hips has no body to swing or limb to bend: it turns and moves the plain way, and says so.
        out["fallback"] = std::string(strings::modeName(mode)) + " needs a joint at or below the hips; posed forward";
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
    if (mode == strings::Mode::Default)
        out["resolved"] = strings::modeName(res.resolved);
    if (res.resolved == strings::Mode::Ik)
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
        throw CommandError(std::string(command) + ": mode is " + modeList());
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
        if (mode == strings::Mode::Forward || mode == strings::Mode::ForwardAll || mode == strings::Mode::Free)
        {
            fallback = std::string(strings::modeName(mode)) + " moves nothing to keep what it is relative to still; zeroed backward";
            mode = strings::Mode::Backward;
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
    return {{"contacts", r.contacts},         {"chains", r.chains},         {"lines", r.lines},
            {"isolate", r.isolate},           {"modes", r.modes},           {"contactsFrom", r.contactsFrom},
            {"chainsFrom", r.chainsFrom},     {"linesFrom", r.linesFrom},   {"isolateFrom", r.isolateFrom},
            {"modesFrom", r.modesFrom}};
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
    if (a.has("isolate"))
    {
        json isolate = json::array();
        for (const json& j : a.flags["isolate"])
            isolate.push_back(bareJoint(jointNamed(g, j.get<std::string>(), "stringsRules").name()));
        own["isolate"] = isolate;
        changed = true;
    }
    if (a.has("defaultMode"))
    {
        const json& d = a.flags["defaultMode"];
        if (!d.is_array() || d.size() != 2)
            throw CommandError("stringsRules: defaultMode is [joint, mode]");
        const std::string joint = bareJoint(jointNamed(g, d[0].get<std::string>(), "stringsRules").name());
        const std::string mode = d[1].get<std::string>();
        if (!own.contains("modes"))
            own["modes"] = rulesOf(g, character).modes;
        if (mode.empty() || mode == "default")
            own["modes"].erase(joint);
        else if (!strings::modeFromName(mode))
            throw CommandError("stringsRules: the mode is " + modeList());
        else
            own["modes"][joint] = mode;
        changed = true;
    }
    if (changed)
        g.setValue(plug, own.dump());
    return rulesJson(rulesOf(g, character));
}

/** Lila space to a character's Unreal space as a 3x3 map on vectors (left-handed, so its determinant is -1). */
glm::dmat3 unrealBasis()
{
    glm::dmat3 b(0.0);
    for (int c = 0; c < 3; ++c)
    {
        lila::math::Vector e(c == 0 ? 1.0 : 0.0, c == 1 ? 1.0 : 0.0, c == 2 ? 1.0 : 0.0);
        const lila::math::Vector u = lila::math::characterToUnreal(e);
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

/** How heavy a member is for leading glue in Unreal: its character's skeleton length (cm), a prop nothing. */
double heftOf(Graph& g, NodeId member)
{
    const NodeId character = rig::isJoint(*g.node(member)) ? characterFor(g, member) : kNoNode;
    const NodeId root = character ? characterRoot(g, character) : kNoNode;
    if (!root)
        return 0.0;
    double length = 0.0;
    for (NodeId d : g.descendants(root))
        if (rig::isJoint(*g.node(d)))
            length += glm::length(vecOf(g, d, Transform::kTranslate));
    return length;
}

/**
 * Which member leads a set once it is bound in Unreal, where a binding runs one way: an attach's last member, as in Lila;
 * a glue's heaviest (a character over a prop, the larger skeleton over the smaller), the first on a tie.
 */
int leadOf(Graph& g, const Relation& rel, const std::vector<NodeId>& members)
{
    if (rel.attach)
        return int(members.size()) - 1;
    int lead = 0;
    double best = -1.0;
    for (size_t i = 0; i < members.size(); ++i)
        if (const double h = heftOf(g, members[i]); h > best + 1e-9)
            best = h, lead = int(i);
    return lead;
}

/**
 * An IK chain for the rule web: its end bone, whether it is locked now, and everything Lila's ikHandle solves it from,
 * carried into Unreal space: solver (twoBone, fabrik, spline), pole (from the start joint), pole angle and roll (radians,
 * signed for Unreal's handedness), softness, stretch, the spline's curve, per joint its rest local, orient, rotate axis,
 * limit and preferred angle, and how far the end follows the controller (alignWeight). "unsolved" only when a joint of
 * the chain isn't a bone of the skeleton.
 */
template <class BoneName, class UeVec>
json unrealLock(Graph& g, NodeId chain, const std::vector<NodeId>& joints, const BoneName& boneName, const UeVec& ueVec, const glm::dmat3& basis)
{
    json out = {{"bone", boneName(joints.back())}, {"locked", chainLocked(g, chain, g.time())}};
    // The lock as Unreal's Lock channel steps through it: a key wherever it turns on or off, frame by frame across its keys.
    const json times = ops::execute("keyframe", {{"args", {g.node(chain)->path()}}, {"attribute", "lock"}, {"query", true}});
    if (times.is_array() && !times.empty())
    {
        const double fps = ops::execute("playbackOptions", {{"query", true}}).value("fps", 30.0);
        const int64_t first = int64_t(std::floor(times.front().get<double>() * fps + 1e-6));
        const int64_t last = int64_t(std::ceil(times.back().get<double>() * fps - 1e-6));
        json keys = json::array();
        for (int64_t f = first; f <= last; ++f)
        {
            const double t = double(f) / fps;
            const bool on = chainLocked(g, chain, t);
            if (keys.empty() || keys.back()[1].get<bool>() != on)
                keys.push_back({t, on});
        }
        out["keys"] = keys;
    }
    const NodeId h = lila::scene::rigel::handleOf(g, chain);
    const Node* handle = h ? g.node(h) : nullptr;
    if (!handle)
        return out;
    json names = json::array();
    for (NodeId j : joints)
    {
        if (boneName(j).empty())
            return out["unsolved"] = g.node(j)->name() + " isn't a deform joint, so Unreal's skeleton has no bone for it", out;
        names.push_back(boneName(j));
    }
    const double handed = glm::determinant(basis) < 0.0 ? -1.0 : 1.0;
    auto value = [&](int attr) { return g.value({h, attr, -1}); };
    auto number = [&](const char* attr) { return std::get<double>(value(attrOf(*handle, attr))); };
    // A rotation carried into Unreal's local-rotation space, as the rest locals are.
    auto ueQuat = [&](const glm::dmat3& m) {
        const glm::dquat q = glm::normalize(glm::quat_cast(basis * m * glm::inverse(basis)));
        return json::array({q.x, q.y, q.z, q.w});
    };
    const int count = int(joints.size());
    std::string solver = std::get<std::string>(value(attrOf(*handle, "ikSolver")));
    if (solver.empty())
        solver = count == 3 ? "ik2BoneSolver" : "ikFabrikSolver";
    // As the handle does: a solver it can't run on this many joints falls to FABRIK.
    if (solver == "ik2BoneSolver" && count == 3)
        out["solver"] = "twoBone";
    else if (solver == "ikSplineSolver" && count >= 2)
        out["solver"] = "spline";
    else
        out["solver"] = "fabrik";
    const bool stretch = std::get<bool>(value(attrOf(*handle, "stretch")));
    out["maxStretch"] = stretch && out["solver"] != "spline" ? std::max(1.0, number("maxStretch")) : 1.0;
    out["fitCurve"] = stretch;
    out["softness"] = std::max(0.0, number("softness"));
    out["twist"] = glm::radians(number("poleAngle")) * handed;
    out["roll"] = glm::radians(number("roll")) * handed;
    if (const glm::dvec3 pv = vecOf(g, h, attrOf(*handle, "poleVector")); glm::length(pv) > 1e-12)
        out["pole"] = ueVec(pv);
    if (const auto* c = dynamic_cast<const ik::CurvePoints*>(std::get<DataPtr>(value(attrOf(*handle, "inCurve"))).get()))
    {
        json curve = json::array();
        for (const glm::dvec3& p : c->points)
            curve.push_back(ueVec(p));
        out["curve"] = curve;
    }
    // The handle's slots: each joint's rest local (what the solve starts from), its orient and rotate axis, its limit.
    const int base = attrOf(*handle, "restLocal0");
    auto slot = [&](int i, int k) { return value(base + i * 13 + k); };
    json slots = json::array();
    for (int i = 0; i < count; ++i)
    {
        const glm::dmat4 local = std::get<glm::dmat4>(slot(i, 0));
        const glm::dmat3 orient(std::get<glm::dmat4>(slot(i, 1))), axis(std::get<glm::dmat4>(slot(i, 2)));
        const glm::dvec3 t = basis * glm::dvec3(local[3]);
        json s = {{"rest", {t.x, t.y, t.z}}, {"restRotation", ueQuat(glm::dmat3(local))}, {"orient", ueQuat(orient)}, {"axis", ueQuat(axis)}};
        if (i + 1 < count)
        {
            const int kind = int(std::clamp<int64_t>(std::get<int64_t>(slot(i, 6)), 0, 2));
            const double stiffness = std::clamp(std::get<double>(slot(i, 11)), 0.0, 1.0);
            if (kind != 0 || stiffness > 0.0)
            {
                const glm::dvec3 a = std::get<glm::dvec3>(slot(i, 7));
                glm::dvec3 bone(1.0, 0.0, 0.0);
                if (const glm::dvec3 child(std::get<glm::dmat4>(slot(i + 1, 0))[3]); glm::length(child) > 1e-12)
                    bone = glm::normalize(axis * child);
                const glm::dvec3 ua = (glm::length(a) > 1e-12 ? glm::normalize(a) : glm::dvec3(0, 0, 1));
                s["limit"] = {{"kind", kind == 1 ? "hinge" : kind == 2 ? "cone" : "none"},
                              {"axis", ueVec(ua * handed)},
                              {"min", glm::radians(std::get<double>(slot(i, 8)))},
                              {"max", glm::radians(std::get<double>(slot(i, 9)))},
                              {"cone", glm::radians(std::get<double>(slot(i, 10)))},
                              {"bone", ueVec(bone)},
                              {"stiffness", stiffness}};
            }
            if (const glm::dvec3 pref = std::get<glm::dvec3>(slot(i, 12)); pref != glm::dvec3(0.0))
            {
                const auto order = RotateOrder(std::clamp<int64_t>(std::get<int64_t>(slot(i, 3)), 0, 5));
                // Lila's preferred local: frame * preferred * post, where frame and post are the orient and rotate axis.
                s["preferred"] = ueQuat(orient * glm::dmat3(eulerToMatrix(pref, order)) * axis);
            }
        }
        slots.push_back(s);
    }
    out["slots"] = slots;
    // The end turns with the controller through its align node, by the node's weight.
    double align = 0.0;
    if (const Node& c = *g.node(chain); attrOf(c, "align") >= 0)
        if (const Node* al = g.find(std::get<std::string>(g.value({chain, attrOf(c, "align"), -1}))))
            align = std::clamp(std::get<double>(g.value({al->id(), attrOf(*al, "weight"), -1})), 0.0, 1.0);
    out["alignWeight"] = align;
    out["chain"] = names;
    return out;
}

/**
 * The rule web as plain data for Unreal: bone names as the skeleton has them (the joints' names), the hips, contacts,
 * limbs, lines of action, each bone's full-body stiffness and joint limits (in Unreal's local-rotation space), the pins,
 * every IK chain end with whether it is locked now, and the glue and attach sets among the skeleton's bones. Nothing
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
    auto boneName = [&](NodeId j) { return k.boneOf(j) >= 0 ? bareJoint(g.node(j)->name()) : std::string(); };
    json pins = json::array(), locks = json::array(), glue = json::array();
    for (NodeId p : pinsOf(g, character))
        if (const std::string n = boneName(p); !n.empty() && std::find(pins.begin(), pins.end(), n) == pins.end())
            pins.push_back(n);
    const glm::dmat3 basis = unrealBasis();
    auto ueVec = [&](const glm::dvec3& v) {
        const glm::dvec3 u = basis * v;
        return json::array({u.x, u.y, u.z});
    };
    for (NodeId chain : lila::scene::rigel::chains(g))
        if (const auto js = lila::scene::rigel::chainJoints(g, chain); !js.empty())
            if (const std::string n = boneName(js.back()); !n.empty())
                locks.push_back(unrealLock(g, chain, js, boneName, ueVec, basis));
    // Glue and attach: this skeleton's bones by name; props and other characters' joints as frames in this character's
    // Unreal space (where they are now), which the rig carries as controls of their own.
    const NodeId rootParent = g.parent(root);
    const glm::dmat4 space = glm::inverse(rootParent ? worldOf(g, rootParent) : glm::dmat4(1.0));
    for (const Relation& rel : relations())
    {
        const std::vector<NodeId> members = membersOf(g, rel);
        json list = json::array();
        int skeletal = 0;
        for (NodeId m : members)
        {
            if (const std::string n = boneName(m); !n.empty())
            {
                list.push_back(n);
                ++skeletal;
                continue;
            }
            const glm::dmat4 w = space * worldOf(g, m);
            const glm::dquat q = glm::normalize(glm::quat_cast(basis * math::rotationOf(w) * glm::inverse(basis)));
            const glm::dvec3 t = basis * glm::dvec3(w[3]);
            json ext = {{"name", bareName(g.node(m)->path())}, {"location", {t.x, t.y, t.z}}, {"rotation", {q.x, q.y, q.z, q.w}}};
            const NodeId other = rig::isJoint(*g.node(m)) ? characterFor(g, m) : kNoNode;
            if (other && other != character)
                ext["character"] = g.node(other)->name(), ext["bone"] = bareJoint(g.node(m)->name());
            else
                ext["prop"] = g.node(m)->path();
            list.push_back(ext);
        }
        if (skeletal >= 1 && list.size() >= 2)
            glue.push_back({{"members", list}, {"attach", rel.attach}, {"lead", leadOf(g, rel, members)}});
    }
    return {{"version", 2},
            {"character", g.node(character)->name()},
            {"root", bareJoint(g.node(root)->name())},
            {"hips", k.hips >= 0 ? bareJoint(g.node(k.joints[size_t(k.hips)])->name()) : ""},
            {"bones", bones},
            {"contacts", r.contacts},
            {"chains", r.chains},
            {"lines", r.lines},
            {"isolate", r.isolate},
            {"modes", r.modes},
            {"stiffness", stiffness},
            {"limits", limits},
            {"pins", pins},
            {"locks", locks},
            {"glue", glue},
            {"basis", {{"up", ueVec(glm::dvec3(0.0, 1.0, 0.0))}, {"side", ueVec(glm::dvec3(1.0, 0.0, 0.0))}}},
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
 * foot, head and the hips and each pinned, locked or glued joint, each posing its bone through a Lila Strings unit given
 * the web, its Mode and Isolate Rotation channels starting at the mode and setting given, its Pin and Lock channels at
 * the joint's pin and lock, and a Glue or Attach channel per set.
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
    for (const json& p : web["pins"])
        addControl(p);
    for (const json& l : web["locks"])
        addControl(l["bone"]);
    for (const json& rel : web["glue"])
        for (const json& m : rel["members"])
            addControl(m);
    const strings::Mode mode = modeArg(a, "unrealStringsRig");
    const json payload = {{"skeletalMesh", mesh},
                          {"rig", rig},
                          {"ruleWeb", web},
                          {"controls", controls},
                          {"mode", strings::modeName(mode)},
                          {"isolate", a.get<bool>("isolate", isolateRef())},
                          {"sequence", a.get<std::string>("sequence", "")},
                          {"fps", ops::execute("playbackOptions", {{"query", true}}).value("fps", 30.0)}};
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
    json out = {{"rig", result.value("rig", rig)}, {"skeletalMesh", mesh}, {"unreal", result}};
    for (const json& l : web["locks"])
        if (l.contains("unsolved"))
        {
            const std::string w = "the IK chain to " + l["bone"].get<std::string>() + " isn't solved as Lila solves it (" +
                                  l["unsolved"].get<std::string>() + "); Unreal holds its end where it is while locked";
            out["warnings"].push_back(w);
            log::write(LogLevel::Warning, "unrealStringsRig: " + w);
        }
    return out;
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

/** The range a pose across frames acts on, from its flags, held the way the time slider holds its selection. */
lila::scene::anim::TimeSelection selectionArg(const Args& a, const char* command)
{
    namespace an = lila::scene::anim;
    an::TimeSelection sel;
    sel.active = true;
    const json& r = a.flags["range"];
    if (!r.is_array() || r.size() != 2)
        throw CommandError(std::string(command) + ": range is [start, end] in seconds");
    sel.start = std::min(r[0].get<double>(), r[1].get<double>());
    sel.end = std::max(r[0].get<double>(), r[1].get<double>());
    sel.falloff = std::max(0.0, a.get<double>("falloff", 0.0));
    if (a.has("falloffBefore"))
        sel.falloff = std::max(0.0, a.get<double>("falloffBefore"));
    if (a.has("falloffAfter"))
        sel.falloffAfter = std::max(0.0, a.get<double>("falloffAfter"));
    else if (a.has("falloffBefore"))
        sel.falloffAfter = std::max(0.0, a.get<double>("falloff", 0.0));
    if (a.has("falloffType"))
    {
        const auto shape = an::rangeFalloffFromName(a.get<std::string>("falloffType"));
        if (!shape)
            throw CommandError(std::string(command) + ": falloffType is smooth, linear, sharp, soft or constant");
        sel.shape = *shape;
    }
    sel.every = a.get<bool>("dense", false) ? 1 : std::clamp(a.get<int>("every", 0), 0, 100);
    return sel;
}

/** Bumped by any change to curves, connections or nodes: what a range preview was built from is stale once it moves. */
uint64_t& keysVersion()
{
    static uint64_t v = 0;
    return v;
}

void watchKeys(Graph& g)
{
    static Graph* watched = nullptr;
    if (watched == &g)
        return;
    watched = &g;
    g.addListener([&g](Change c, NodeId id) {
        if (c == Change::Selection || c == Change::Renamed)
            return;
        if (c == Change::Value)
            if (const Node* n = g.node(id); n && n->type().dag)
                return;
        ++keysVersion();
    });
}

/** What one frame of a pose across frames asks of a node: the drag as it lands there. */
PoseInput frameInput(const PoseInput& in, bool relative, double w, const glm::dmat3& carry, const glm::dmat4& start, const glm::dmat4& here,
                     bool pivot)
{
    PoseInput p = in;
    if (relative)
    {
        if (in.move)
            p.move = carry * *in.move * w;
        if (in.turn)
        {
            p.turn = partOf(glm::quat_cast(carry) * *in.turn * glm::inverse(glm::quat_cast(carry)), w);
            // Pivot: the frame swings about where the joint is now, so its path turns with the drag.
            if (pivot)
            {
                const glm::dvec3 c(start[3]), at(here[3]);
                p.move = p.move.value_or(glm::dvec3(0.0)) + (c + glm::dmat3(glm::mat3_cast(*p.turn)) * (at - c) - at);
            }
        }
    }
    else
    {
        if (in.move)
            p.move = (glm::dvec3(start[3]) + *in.move - glm::dvec3(here[3])) * w;
        if (in.turn)
        {
            const glm::dquat want = *in.turn * glm::quat_cast(math::rotationOf(start));
            p.turn = partOf(glm::normalize(want * glm::inverse(glm::quat_cast(math::rotationOf(here)))), w);
        }
    }
    return p;
}

/**
 * A drag across frames solved in memory: every frame's skeleton, holds, starting place and channel values, read from
 * the scene once, then solved on each update in parallel. A preview keeps it while the drag goes on.
 */
struct RangePreview
{
    struct Item
    {
        std::shared_ptr<Prepared> prep;
        glm::dmat4 here{1.0};
        glm::dmat3 carry{1.0};
        bool pivot = false;
        /** Keying only: each joint's rotate, translate and rotate order at the frame, to turn solved rotations back into its channels. */
        std::vector<glm::dvec3> rotate, translate;
        std::vector<RotateOrder> order;
    };
    struct Frame
    {
        double t = 0.0, w = 0.0;
        bool ghost = false;
        std::vector<Item> items;
    };
    Graph* graph = nullptr;
    std::string key;
    uint64_t keys = 0;
    std::vector<Frame> frames;
    std::vector<glm::dmat4> start;
    std::vector<PoseValues> unkeyed;
    bool pivotInPlace = false;
    /** Mesh ghosts recapture a few per update, round the ghosts in turn, so a long range stays interactive. */
    size_t nextMeshGhost = 0;
    std::map<double, std::shared_ptr<const ghosts::Frame>> meshGhosts;
};

std::unique_ptr<RangePreview>& rangePreview()
{
    static std::unique_ptr<RangePreview> p;
    return p;
}

/** The call as a cache key: everything but the drag itself, and the prefs a pose reads. */
std::string previewKey(const std::vector<NodeId>& nodes, const Args& a, strings::Mode mode, const std::vector<double>& times)
{
    json k = a.flags;
    for (const char* drag : {"by", "axis", "angle", "preview"})
        k.erase(drag);
    k["#nodes"] = nodes;
    k["#mode"] = strings::modeName(mode);
    k["#times"] = times;
    k["#prefs"] = {contactsRef(), isolateRef(), rotationTypeRef(), relations().size()};
    return k.dump();
}

/** The in-memory path: character joints only, nothing glued, no auto-pose. */
bool solvesInMemory(Graph& g, const std::vector<NodeId>& nodes, const PoseInput& in)
{
    if (!relations().empty() || in.autoPose)
        return false;
    for (NodeId n : nodes)
        if (const Node* node = g.node(n); !node || !rig::isJoint(*node) || !characterRoot(g, characterFor(g, n)))
            return false;
    return true;
}

std::unique_ptr<RangePreview> buildAcross(Graph& g, const std::vector<NodeId>& nodes, const PoseInput& in, strings::Mode mode,
                                          const std::vector<double>& times, const std::vector<double>& ghostTimes,
                                          const lila::scene::anim::TimeSelection& sel, const std::string& space, bool pivot, bool values)
{
    LILA_ZONE_N("strings range build");
    auto p = std::make_unique<RangePreview>();
    p->graph = &g;
    const double now = g.time();
    g.evaluate();
    p->unkeyed = savePoseValues(g, nodes);
    std::vector<glm::dmat3> space0;
    for (NodeId n : nodes)
    {
        p->start.push_back(worldOf(g, n));
        space0.push_back(spaceAt(g, n, space));
    }
    {
        RulesCacheScope rules;
        Graph::DeferMeshes defer;
        for (double t : times)
        {
            const double w = lila::scene::anim::timeSelectionWeight(sel, t);
            if (w <= 0.0)
                continue;
            g.setTime(t, false);
            g.evaluate();
            RangePreview::Frame f{.t = t, .w = w};
            f.ghost = std::any_of(ghostTimes.begin(), ghostTimes.end(), [&](double x) { return std::abs(x - t) < 1e-9; });
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                RangePreview::Item item;
                item.prep = std::make_shared<Prepared>(prepareJoint(g, nodes[i], mode, in));
                item.here = worldOf(g, nodes[i]);
                item.carry = spaceAt(g, nodes[i], space) * glm::transpose(space0[i]);
                const Prepared& pr = *item.prep;
                item.pivot = pivot && strings::movesOrigin(mode, pr.bone == pr.k.hips);
                if (pivot && !item.pivot)
                    p->pivotInPlace = true;
                for (NodeId j : values ? pr.k.joints : std::vector<NodeId>{})
                {
                    item.rotate.push_back(vecOf(g, j, Transform::kRotate));
                    item.translate.push_back(vecOf(g, j, Transform::kTranslate));
                    item.order.push_back(rotateOrderOf(g, j));
                }
                f.items.push_back(std::move(item));
            }
            p->frames.push_back(std::move(f));
        }
        g.setTime(now, false);
    }
    restorePoseValues(g, p->unkeyed);
    p->keys = keysVersion();
    return p;
}

/** Every frame's poses for the drag in, solved in parallel: the snapshots are plain data. */
std::vector<std::vector<strings::Result>> solveAcross(const RangePreview& p, const PoseInput& in, bool relative)
{
    LILA_ZONE_N("strings range solve");
    std::vector<std::vector<strings::Result>> solved(p.frames.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, p.frames.size()), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t fi = range.begin(); fi != range.end(); ++fi)
        {
            const RangePreview::Frame& f = p.frames[fi];
            for (size_t i = 0; i < f.items.size(); ++i)
            {
                const RangePreview::Item& item = f.items[i];
                const PoseInput fin = frameInput(in, relative, f.w, item.carry, p.start[i], item.here, item.pivot);
                strings::Request r = item.prep->r;
                r.move = fin.move;
                r.turn = fin.turn;
                bool fellBack = false;
                solved[fi].push_back(solvePrepared(*item.prep, r, fellBack));
            }
        }
    });
    return solved;
}

/** A ghost of a solved frame drawn from the solve itself: each joint to its parent, as the skeleton ghosts draw. */
std::shared_ptr<const ghosts::Frame> skeletonGhost(const RangePreview::Frame& f, const std::vector<strings::Result>& solved)
{
    auto out = std::make_shared<ghosts::Frame>();
    out->time = f.t;
    for (size_t i = 0; i < f.items.size(); ++i)
    {
        const strings::Skeleton& k = f.items[i].prep->k;
        const std::vector<glm::dmat4> w = strings::worlds(k.problem, solved[i].pose);
        for (size_t b = 0; b < w.size(); ++b)
            if (const int parent = k.problem.bones[b].parent; parent >= 0)
                out->bones.push_back({glm::dvec3(w[size_t(parent)][3]), glm::dvec3(w[b][3])});
    }
    return out;
}

/** Whether a cached build serves this call: the same call, keys and scene. */
bool reusable(const RangePreview* p, Graph& g, const std::string& key)
{
    return p && p->graph == &g && p->key == key && p->keys == keysVersion();
}

json previewAcross(Graph& g, const std::vector<NodeId>& nodes, const Args& a, const PoseInput& in, strings::Mode mode, const std::vector<double>& times,
                   const std::vector<double>& ghostTimes, const lila::scene::anim::TimeSelection& sel, bool relative, const std::string& space, bool pivot)
{
    LILA_ZONE();
    watchKeys(g);
    std::unique_ptr<RangePreview>& cached = rangePreview();
    const std::string key = previewKey(nodes, a, mode, times);
    if (!reusable(cached.get(), g, key))
    {
        cached = buildAcross(g, nodes, in, mode, times, ghostTimes, sel, space, pivot, false);
        cached->key = key;
    }
    RangePreview& p = *cached;
    const std::vector<std::vector<strings::Result>> solved = solveAcross(p, in, relative);

    json out = json::array();
    for (size_t fi = 0; fi < p.frames.size(); ++fi)
    {
        json at = json::array();
        for (size_t i = 0; i < p.frames[fi].items.size(); ++i)
        {
            const RangePreview::Item& item = p.frames[fi].items[i];
            at.push_back(vecJson(glm::dvec3(strings::worlds(item.prep->k.problem, solved[fi][i].pose)[size_t(item.prep->bone)][3])));
        }
        json entry = {{"time", p.frames[fi].t}, {"weight", p.frames[fi].w}, {"at", at}};
        if (p.pivotInPlace)
            entry["pivot"] = "in place";
        out.push_back(std::move(entry));
    }

    // Skeleton ghosts come straight from the solve. Mesh ghosts need the scene at their frame: a few per update, in turn,
    // within a time budget, the rest keeping their last capture.
    const ghosts::Settings gs = ghosts::Settings::fromPrefs();
    std::vector<size_t> ghostFrames;
    for (size_t fi = 0; fi < p.frames.size(); ++fi)
        if (p.frames[fi].ghost)
            ghostFrames.push_back(fi);
    std::vector<std::shared_ptr<const ghosts::Frame>> frames;
    if (!gs.mesh)
        for (size_t fi : ghostFrames)
            frames.push_back(skeletonGhost(p.frames[fi], solved[fi]));
    else if (!ghostFrames.empty())
    {
        const double now = g.time();
        const auto t0 = std::chrono::steady_clock::now();
        bool stepped = false;
        for (size_t n = 0; n < ghostFrames.size(); ++n)
        {
            if (n > 0 && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > 24.0)
                break;
            const size_t fi = ghostFrames[(p.nextMeshGhost + n) % ghostFrames.size()];
            const RangePreview::Frame& f = p.frames[fi];
            g.setTime(f.t, false);
            g.evaluate();
            stepped = true;
            for (size_t i = 0; i < f.items.size(); ++i)
                strings::apply(g, f.items[i].prep->k, solved[fi][i].pose);
            g.evaluate();
            p.meshGhosts[f.t] = ghosts::cache().captureNow(f.t);
            p.nextMeshGhost = (p.nextMeshGhost + n + 1) % ghostFrames.size();
        }
        if (stepped)
        {
            g.setTime(now, false);
            restorePoseValues(g, p.unkeyed);
        }
        for (size_t fi : ghostFrames)
            if (const auto it = p.meshGhosts.find(p.frames[fi].t); it != p.meshGhosts.end())
                frames.push_back(it->second);
            else
                frames.push_back(skeletonGhost(p.frames[fi], solved[fi]));
    }
    ghosts::cache().setPreview(std::move(frames));
    return out;
}

/**
 * Keys v on node.attr at t straight onto its curves, which is what makes keying a long range quick; on a layer, or a
 * channel something other than a curve drives, the slower keyVec way that works out what to key there.
 */
void keyChannels(Graph& g, NodeId id, int attr, const glm::dvec3& v, double t)
{
    namespace an = lila::scene::anim;
    if (an::activeLayer(g) == kNoNode)
        try
        {
            if (attr == Transform::kRotate && an::keysAsRotation(g, {id, attr, -1}))
                an::setRotationKey(g, {id, attr, -1}, t, v);
            else
                for (int c = 0; c < 3; ++c)
                    an::setKey(g, {id, attr, c}, t, v[c]);
            return;
        }
        catch (const std::invalid_argument&)
        {
        }
    keyVec(g.node(id)->path(), attr == Transform::kRotate ? "rotate" : attr == Transform::kScale ? "scale" : "translate", v, t);
}

/** The same solve keyed: each frame's changed channels, from the values it held, keyed once all are solved. */
json commitAcross(Graph& g, const std::vector<NodeId>& nodes, const Args& a, const PoseInput& in, strings::Mode mode, const std::vector<double>& times,
                  const lila::scene::anim::TimeSelection& sel, bool relative, const std::string& space, bool pivot)
{
    LILA_ZONE();
    namespace an = lila::scene::anim;
    const std::unique_ptr<RangePreview> built = buildAcross(g, nodes, in, mode, times, {}, sel, space, pivot, true);
    const RangePreview& p = *built;
    const std::vector<std::vector<strings::Result>> solved = solveAcross(p, in, relative);

    struct Keyed
    {
        double t;
        std::vector<std::tuple<NodeId, int, glm::dvec3>> values;
    };
    std::vector<Keyed> keyed;
    std::set<std::pair<NodeId, int>> plugs;
    json out = json::array();
    for (size_t fi = 0; fi < p.frames.size(); ++fi)
    {
        const RangePreview::Frame& f = p.frames[fi];
        Keyed k{f.t, {}};
        json poses = json::array();
        for (size_t i = 0; i < f.items.size(); ++i)
        {
            const RangePreview::Item& item = f.items[i];
            const strings::Skeleton& sk = item.prep->k;
            const strings::Pose& s = solved[fi][i].pose;
            for (size_t j = 0; j < sk.joints.size(); ++j)
            {
                const fbik::Bone& b = sk.problem.bones[j];
                if (glm::abs(glm::dot(s.rotation[j], b.rotation)) < 1.0 - 1e-14)
                {
                    const glm::dvec3 r = xformops::closestEuler(glm::mat3_cast(s.rotation[j]), item.order[j], item.rotate[j]);
                    if (glm::length(r - item.rotate[j]) > 1e-9)
                    {
                        k.values.push_back({sk.joints[j], int(Transform::kRotate), r});
                        plugs.insert({sk.joints[j], int(Transform::kRotate)});
                    }
                }
                const glm::dvec3 move = s.offset[j] + (s.stretch[j] - 1.0) * glm::dvec3(b.before[3]);
                if (glm::length(move) > 1e-9)
                {
                    k.values.push_back({sk.joints[j], int(Transform::kTranslate), item.translate[j] + move});
                    plugs.insert({sk.joints[j], int(Transform::kTranslate)});
                }
            }
            json pose = {{"joint", g.node(nodes[i])->path()}, {"mode", strings::modeName(mode)}, {"error", solved[fi][i].error}};
            if (mode == strings::Mode::Default)
                pose["resolved"] = strings::modeName(solved[fi][i].resolved);
            poses.push_back(std::move(pose));
        }
        keyed.push_back(std::move(k));
        json entry = {{"time", f.t}, {"weight", f.w}, {"poses", poses}};
        if (p.pivotInPlace)
            entry["pivot"] = "in place";
        out.push_back(std::move(entry));
    }
    // Keying every n frames: the frames just past the edit keep their pose, so the frames beyond them don't change.
    const double now = g.time();
    if (sel.every > 0 && !p.frames.empty())
    {
        for (double t : {an::secondsOf(an::frameOf(p.frames.front().t) - 1.0), an::secondsOf(an::frameOf(p.frames.back().t) + 1.0)})
        {
            g.setTime(t, false);
            g.evaluate();
            Keyed edge{t, {}};
            for (const auto& [id, attr] : plugs)
                edge.values.push_back({id, attr, vecOf(g, id, attr)});
            keyed.push_back(std::move(edge));
        }
        g.setTime(now, false);
        restorePoseValues(g, p.unkeyed);
    }
    if (p.pivotInPlace)
        log::write(LogLevel::Info, std::string("stringsPose: pivot swings only joints the ") + strings::modeName(mode) +
                                       " mode can move; the rest turned in place");
    // Auto key would key each value as it is set.
    const bool autoKeyWas = an::autoKey();
    an::setAutoKey(false);
    {
        LayerScope onLayer(g, a.get<std::string>("layer", ""));
        LILA_ZONE_N("strings range keys");
        for (const Keyed& k : keyed)
            for (const auto& [id, attr, v] : k.values)
                keyChannels(g, id, attr, v, k.t);
    }
    an::setAutoKey(autoKeyWas);
    g.setTime(now, false);
    g.evaluate();
    return out;
}

/**
 * The same pose at every frame of a set, each solved on that frame's own pose so holds and contacts keep per frame, and
 * keyed there. Relative offsets each frame by the drag (in space), scaled by the falloff weight; absolute brings each
 * frame to where the drag put the joint now. Rotation type pivot swings each frame about where the joint is now.
 */
json poseAcrossFrames(Graph& g, const std::vector<NodeId>& nodes, const Args& a, const PoseInput& in, strings::Mode mode,
                      const std::function<json(NodeId, const PoseInput&)>& poseOne)
{
    namespace an = lila::scene::anim;
    an::TimeSelection sel;
    sel.active = true;
    std::vector<double> times;
    if (a.has("frames"))
        for (const json& t : a.flags["frames"])
            times.push_back(t.get<double>());
    if (a.has("range"))
    {
        sel = selectionArg(a, "stringsPose");
        std::vector<NodeId> keyed;
        for (NodeId n : nodes)
            if (characterFor(g, n))
            {
                const std::vector<NodeId> js = fullBodyJoints(g, n);
                keyed.insert(keyed.end(), js.begin(), js.end());
            }
            else
                keyed.push_back(n);
        for (double t : an::timeSelectionFrames(sel, an::keyTimes(g, keyed)))
            times.push_back(t);
    }
    else if (!times.empty())
    {
        sel.start = *std::min_element(times.begin(), times.end());
        sel.end = *std::max_element(times.begin(), times.end());
    }
    const std::string rotationType = a.get<std::string>("rotationType", rotationTypeRef());
    if (rotationType != "local" && rotationType != "pivot")
        throw CommandError("stringsPose: rotationType is local or pivot");
    const bool relative = a.get<bool>("relative", true);
    const bool pivot = relative && rotationType == "pivot" && in.turn;
    const std::string space = a.get<std::string>("space", "world");
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
    else
        rangePreview().reset();
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(), [](double x, double y) { return std::abs(x - y) < 1e-9; }), times.end());
    if (times.empty())
        throw CommandError("stringsPose: no frames to pose in the range (no keys there; every or dense poses frames without keys)");
    if (solvesInMemory(g, nodes, in))
        return preview ? previewAcross(g, nodes, a, in, mode, times, ghostTimes, sel, relative, space, pivot)
                       : commitAcross(g, nodes, a, in, mode, times, sel, relative, space, pivot);

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
    const bool autoKeyWas = an::autoKey();
    an::setAutoKey(false);
    auto finish = [&] {
        gWritten = nullptr;
        an::setAutoKey(autoKeyWas);
        g.setTime(now, false);
        g.evaluate();
    };
    json out = json::array();
    std::vector<std::string> plugs;
    gWritten = &plugs;
    bool inPlace = false;
    try
    {
        RulesCacheScope rules;
        for (double t : times)
        {
            const double w = an::timeSelectionWeight(sel, t);
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
                bool swing = pivot;
                if (pivot && rig::isJoint(*g.node(n)) && characterRoot(g, characterFor(g, n)))
                {
                    const strings::Skeleton k = strings::snapshot(g, characterRoot(g, characterFor(g, n)));
                    swing = strings::movesOrigin(mode, k.boneOf(n) == k.hips);
                    inPlace = inPlace || !swing;
                }
                frame.push_back(poseOne(n, frameInput(in, relative, w, spaceAt(g, n, space) * glm::transpose(s0.space), s0.world, here, swing)));
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
            json entry = {{"time", t}, {"weight", w}, {"poses", frame}};
            out.push_back(std::move(entry));
        }
    }
    catch (...)
    {
        finish();
        if (preview)
            restorePoseValues(g, unkeyed);
        throw;
    }
    if (inPlace)
    {
        log::write(LogLevel::Info, std::string("stringsPose: pivot swings only joints the ") + strings::modeName(mode) +
                                       " mode can move; the rest turned in place");
        for (json& f : out)
            f["pivot"] = "in place";
    }
    if (preview)
    {
        finish();
        restorePoseValues(g, unkeyed);
        ghosts::cache().setPreview(std::move(ghostFrames));
        return out;
    }
    // Keying every n frames: the frames just past the edit keep their pose, so the frames beyond them don't change.
    if (sel.every > 0 && !solved.empty())
    {
        std::vector<std::string> all;
        for (const Solved& f : solved)
            for (const auto& v : f.values)
                all.push_back(v.first);
        std::sort(all.begin(), all.end());
        all.erase(std::unique(all.begin(), all.end()), all.end());
        for (double t : {an::secondsOf(an::frameOf(times.front()) - 1.0), an::secondsOf(an::frameOf(times.back()) + 1.0)})
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
                keyChannels(g, g.find(plug.substr(0, dot))->id(), plug.substr(dot + 1) == "rotate" ? int(Transform::kRotate) : int(Transform::kTranslate), v, t);
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

/** With Isolate Rotation on, the rules' isolated joints turn to their blended world rotation after an inbetween. */
void isolateInbetween(Graph& g, NodeId character, const std::vector<std::pair<NodeId, std::pair<glm::dmat4, glm::dmat4>>>& isolated, double f)
{
    if (isolated.empty())
        return;
    g.evaluate();
    for (const auto& [j, ends] : isolated)
    {
        setWorldRotation(g, j, math::rotationOf(blendWorld(ends.first, ends.second, f)));
        g.evaluate();
    }
    (void)character;
}

/** The joints Isolate Rotation keeps, with their worlds at two times. */
std::vector<std::pair<NodeId, std::pair<glm::dmat4, glm::dmat4>>> isolatedAt(Graph& g, NodeId character, strings::Mode mode, double a, double b)
{
    std::vector<std::pair<NodeId, std::pair<glm::dmat4, glm::dmat4>>> out;
    if (!isolateRef() || !holdsInWorld(mode))
        return out;
    const CharacterRules rules = rulesOf(g, character);
    std::vector<NodeId> joints;
    for (NodeId j : fullBodyJoints(g, characterRoot(g, character)))
        if (std::find(rules.isolate.begin(), rules.isolate.end(), bareJoint(g.node(j)->name())) != rules.isolate.end())
            joints.push_back(j);
    std::vector<glm::dmat4> wa, wb;
    atTime(g, a, [&] {
        for (NodeId j : joints)
            wa.push_back(worldOf(g, j));
    });
    atTime(g, b, [&] {
        for (NodeId j : joints)
            wb.push_back(worldOf(g, j));
    });
    for (size_t i = 0; i < joints.size(); ++i)
        out.push_back({joints[i], {wa[i], wb[i]}});
    return out;
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
    const strings::Mode mode = modeArg(a, "stringsBreakdown");
    const std::vector<NodeId> parts = holdsInWorld(mode) ? worldParts(g, character, a) : std::vector<NodeId>{};
    const auto isolated = isolatedAt(g, character, mode, g.time(), *other);
    g.evaluate();
    const Keyframe now = keyframeNow(g, joints, parts);
    const Keyframe target = keyframeAt(g, joints, parts, *other);
    inbetween(g, character, joints, parts, now, target, std::abs(w), breakdownWeights(g, character, a));
    isolateInbetween(g, character, isolated, std::abs(w));
    return {{"from", g.time()}, {"toward", *other}, {"joints", joints.size()}, {"mode", strings::modeName(mode)}};
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

/** The range a range command acts on: its flags, else the time slider's selection with its falloff and every. */
lila::scene::anim::TimeSelection rangeSelection(const Args& a, const char* command)
{
    if (a.has("range"))
        return selectionArg(a, command);
    const lila::scene::anim::TimeSelection& sel = lila::scene::anim::timeSelection();
    if (!sel.active)
        throw CommandError(std::string(command) + ": give a range, or Shift + drag one on the time slider");
    return sel;
}

/**
 * Tween a range: each frame of it blends toward the inbetween of the poses just outside the range and its falloff,
 * space-aware (rotations on arcs, contacts in world), by amount times the frame's falloff weight. 1 replaces the motion
 * with the inbetween, past 1 overshoots it, below 0 pushes the motion further from it.
 */
json stringsTween(const Args& a)
{
    namespace an = lila::scene::anim;
    Graph& g = graph();
    const std::vector<NodeId> picked = poseJoints(g, a, "stringsTween");
    const NodeId character = onlyCharacter(g, picked, "stringsTween");
    const std::vector<NodeId> joints = a.get<bool>("all", true) ? fullBodyJoints(g, characterRoot(g, character)) : picked;
    const an::TimeSelection sel = rangeSelection(a, "stringsTween");
    const strings::Mode mode = modeArg(a, "stringsTween");
    const double amount = std::clamp(a.get<double>("amount", 1.0), -1.0, 2.0);
    const std::string curve = a.get<std::string>("spacing", "linear");
    const double f0 = std::round(an::frameOf(sel.start) - sel.before() - 1.0), f1 = std::round(an::frameOf(sel.end) + sel.after() + 1.0);
    const std::vector<NodeId> parts = holdsInWorld(mode) ? worldParts(g, character, a) : std::vector<NodeId>{};
    const Keyframe from = keyframeAt(g, joints, parts, an::secondsOf(f0)), to = keyframeAt(g, joints, parts, an::secondsOf(f1));
    const auto isolated = isolatedAt(g, character, mode, an::secondsOf(f0), an::secondsOf(f1));
    const bool preview = a.get<bool>("preview", false);
    std::vector<double> frames = an::timeSelectionFrames(sel, an::keyTimes(g, joints));
    if (preview)
    {
        // A drag on the slider: only the frame shown and the ghosts' frames are solved, each frame stands alone.
        std::vector<double> shown{g.time()};
        for (const auto& [t, offset] : ghosts::cache().shownTimes(ghosts::Settings::fromPrefs()))
            shown.push_back(t);
        frames = shown;
    }
    if (frames.empty())
        throw CommandError("stringsTween: no keys in the range; set the range to key every n frames");

    auto tweenAt = [&](double t) {
        g.evaluate();
        const Keyframe self = keyframeNow(g, joints, parts);
        const double x = (an::frameOf(t) - f0) / (f1 - f0);
        inbetween(g, character, joints, parts, from, to, spacing(curve, x));
        isolateInbetween(g, character, isolated, spacing(curve, x));
        g.evaluate();
        const Keyframe target = keyframeNow(g, joints, parts);
        inbetween(g, character, joints, parts, self, target, amount * an::timeSelectionWeight(sel, t));
        g.evaluate();
    };

    const double now = g.time();
    if (preview)
    {
        const std::vector<PoseValues> unkeyed = savePoseValues(g, joints);
        std::vector<std::shared_ptr<const ghosts::Frame>> ghostFrames;
        {
            Graph::DeferMeshes defer;
            for (double t : frames)
            {
                if (std::abs(t - now) < 1e-9 || an::timeSelectionWeight(sel, t) <= 0.0)
                    continue;
                g.setTime(t, false);
                tweenAt(t);
                ghostFrames.push_back(ghosts::cache().captureNow(t));
            }
            g.setTime(now, false);
            restorePoseValues(g, unkeyed);
        }
        if (an::timeSelectionWeight(sel, now) > 0.0)
            tweenAt(now);
        ghosts::cache().setPreview(std::move(ghostFrames));
        return {{"frames", frames.size()}, {"preview", true}};
    }

    FrameKeys out(g);
    int tweened = 0;
    {
        Graph::DeferMeshes defer;
        for (double t : frames)
        {
            if (an::timeSelectionWeight(sel, t) <= 0.0)
                continue;
            g.setTime(t, false);
            tweenAt(t);
            out.record(t, joints);
            ++tweened;
        }
        // Keyed every n frames, the frames just outside keep their pose, so nothing beyond them changes.
        if (sel.every > 0)
            for (double f : {f0, f1})
            {
                g.setTime(an::secondsOf(f), false);
                g.evaluate();
                out.record(an::secondsOf(f), joints);
            }
    }
    out.key(a.get<std::string>("layer", ""));
    return {{"tweened", tweened}, {"from", an::secondsOf(f0)}, {"to", an::secondsOf(f1)}};
}

/**
 * Mirror in a mode: the selected joints take the pose of their other side (left arm onto right), posed through the
 * Strings mode so what holds stays: in Backward the feet keep planted while the body mirrors around them.
 */
json stringsMirror(const Args& a)
{
    Graph& g = graph();
    const std::vector<NodeId> joints = poseJoints(g, a, "stringsMirror");
    const NodeId character = onlyCharacter(g, joints, "stringsMirror");
    const NodeId root = characterRoot(g, character);
    const std::string axis = a.get<std::string>("mirrorAxis", "x");
    g.evaluate();
    const std::vector<NodeId> all = fullBodyJoints(g, root);
    const poses::Pose mirrored = poses::mirrored(g, poses::capture(g, all, {"rotate", "translate"}), all, axis.empty() ? 'x' : axis[0]);
    std::vector<NodeId> posed;
    std::vector<glm::dquat> target;
    json skipped = json::array();
    for (NodeId j : joints)
    {
        const std::string name = poses::stripNamespace(g.node(j)->name());
        const json& entry = mirrored.nodes.contains(name) ? mirrored.nodes[name] : json();
        if (!entry.is_object() || !entry.contains("rotate"))
        {
            skipped.push_back(g.node(j)->path());
            continue;
        }
        const json& q = entry["rotate"];
        posed.push_back(j);
        target.push_back(glm::normalize(glm::dquat(q[3].get<double>(), q[0].get<double>(), q[1].get<double>(), q[2].get<double>())));
    }
    // The hips carry the body: their place mirrors too, before the mode poses the rest around what holds.
    const strings::Skeleton k = strings::snapshot(g, root);
    if (const NodeId hips = k.hips >= 0 ? k.joints[size_t(k.hips)] : kNoNode; hips && std::find(joints.begin(), joints.end(), hips) != joints.end())
        if (const std::string name = poses::stripNamespace(g.node(hips)->name()); mirrored.nodes.contains(name) && mirrored.nodes[name].contains("translate"))
        {
            const json& t = mirrored.nodes[name]["translate"];
            g.setValue({hips, Transform::kTranslate, -1}, glm::dvec3(t[0].get<double>(), t[1].get<double>(), t[2].get<double>()));
        }
    const strings::Mode mode = modeArg(a, "stringsMirror");
    json out = {{"mode", strings::modeName(mode)}, {"poses", poseToRotations(g, posed, target, mode, a)}};
    if (!skipped.empty())
        out["skipped"] = skipped;
    return out;
}

/** A channel a curve range edit writes: the node, its vec3 attr and the component, from a plug like "hips.translateY". */
struct RangeChannel
{
    NodeId node = kNoNode;
    std::string attr; // "translate", "rotate", "scale"
    int component = 0;
    std::string plug;
};

RangeChannel rangeChannel(Graph& g, const std::string& name)
{
    PlugRef p;
    try
    {
        p = g.plug(name);
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(std::string("curveRangeEdit: ") + e.what());
    }
    const std::string& attr = g.attr(p).name;
    if ((attr != "translate" && attr != "rotate" && attr != "scale") || p.component < 0)
        throw CommandError("curveRangeEdit: " + name + " is not one axis of a translate, rotate or scale");
    return {p.node, attr, p.component, g.plugName(p)};
}

/**
 * The Curve Overlay's edits across a time range, in channel space: offset adds to every frame of the range (falloff
 * weighted), tween blends every frame toward the line between the values just outside the range and its falloff. Keys
 * the range's keys, or every n frames as the range says, and the frames just outside when it keys every n. Pivot turns
 * a rotate channel of the joint that carries a character (its hips) through stringsPose, so the body's path swings too.
 */
json curveRangeEdit(const Args& a)
{
    namespace an = lila::scene::anim;
    Graph& g = graph();
    if (!a.has("channels") || a.flags["channels"].empty())
        throw CommandError("curveRangeEdit: give channels, plugs such as hips.translateY");
    const bool tween = a.has("tween");
    if (tween == a.has("offset"))
        throw CommandError("curveRangeEdit: give offset or tween");
    const an::TimeSelection sel = rangeSelection(a, "curveRangeEdit");
    const double offset = a.get<double>("offset", 0.0), amount = std::clamp(a.get<double>("tween", 0.0), -1.0, 2.0);
    const std::string rotationType = a.get<std::string>("rotationType", rotationTypeRef());
    std::vector<RangeChannel> channels;
    json pivoted = json::array();
    for (const json& c : a.flags["channels"])
    {
        RangeChannel ch = rangeChannel(g, c.get<std::string>());
        // Pivot: a turn of the joint that carries the body swings its whole path about where it is now.
        if (!tween && rotationType == "pivot" && ch.attr == "rotate" && rig::isJoint(*g.node(ch.node)))
            if (const NodeId root = characterRoot(g, characterFor(g, ch.node)))
                if (const strings::Skeleton k = strings::snapshot(g, root); k.hips >= 0 && k.joints[size_t(k.hips)] == ch.node)
                {
                    g.evaluate();
                    const glm::dvec3 axis = glm::normalize(glm::dvec3(worldOf(g, ch.node)[ch.component]));
                    json call = {{"args", {g.node(ch.node)->path()}}, {"mode", "forwardAll"}, {"axis", vecJson(axis)}, {"angle", offset},
                                 {"rotationType", "pivot"}, {"holds", false}};
                    addRangeFlags(call, sel);
                    if (a.has("layer"))
                        call["layer"] = a.get<std::string>("layer");
                    ops::execute("stringsPose", call);
                    pivoted.push_back(ch.plug);
                    continue;
                }
        channels.push_back(std::move(ch));
    }
    json out = {{"channels", json::array()}, {"pivot", pivoted}};
    if (channels.empty())
        return out;

    std::vector<NodeId> nodes;
    for (const RangeChannel& ch : channels)
        if (std::find(nodes.begin(), nodes.end(), ch.node) == nodes.end())
            nodes.push_back(ch.node);
    std::vector<double> frames = an::timeSelectionFrames(sel, an::keyTimes(g, nodes));
    if (frames.empty())
        throw CommandError("curveRangeEdit: no keys in the range; set the range to key every n frames");
    const double f0 = std::round(an::frameOf(sel.start) - sel.before() - 1.0), f1 = std::round(an::frameOf(sel.end) + sel.after() + 1.0);
    auto attrIndex = [](const std::string& attr) {
        return attr == "translate" ? int(Transform::kTranslate) : attr == "rotate" ? int(Transform::kRotate) : int(Transform::kScale);
    };
    // Every (node, attr) the edit keys, read whole so each key carries its other axes as they were at that frame.
    std::vector<std::pair<NodeId, std::string>> groups;
    for (const RangeChannel& ch : channels)
        if (std::find(groups.begin(), groups.end(), std::pair{ch.node, ch.attr}) == groups.end())
            groups.push_back({ch.node, ch.attr});
    auto read = [&](double t) {
        std::vector<glm::dvec3> v;
        atTime(g, t, [&] {
            for (const auto& [n, attr] : groups)
                v.push_back(vecOf(g, n, attrIndex(attr)));
        });
        return v;
    };
    const std::vector<glm::dvec3> before = read(an::secondsOf(f0)), after = read(an::secondsOf(f1));
    std::vector<std::pair<double, std::vector<glm::dvec3>>> keyed;
    for (double t : frames)
    {
        const double w = an::timeSelectionWeight(sel, t);
        if (w <= 0.0)
            continue;
        std::vector<glm::dvec3> v = read(t);
        const double x = (an::frameOf(t) - f0) / (f1 - f0);
        for (const RangeChannel& ch : channels)
        {
            const size_t gi = size_t(std::find(groups.begin(), groups.end(), std::pair{ch.node, ch.attr}) - groups.begin());
            double& value = v[gi][ch.component];
            if (tween)
                value += amount * w * (glm::mix(before[gi][ch.component], after[gi][ch.component], x) - value);
            else
                value += offset * w;
        }
        keyed.push_back({t, std::move(v)});
    }
    if (sel.every > 0)
        for (double f : {an::frameOf(frames.front()) - 1.0, an::frameOf(frames.back()) + 1.0})
            keyed.push_back({an::secondsOf(f), read(an::secondsOf(f))});
    FrameKeys hold(g);
    {
        LayerScope onLayer(g, a.get<std::string>("layer", ""));
        for (const auto& [t, values] : keyed)
            for (size_t gi = 0; gi < groups.size(); ++gi)
                keyChannels(g, groups[gi].first, attrIndex(groups[gi].second), values[gi], t);
    }
    for (const RangeChannel& ch : channels)
        out["channels"].push_back(ch.plug);
    out["frames"] = keyed.size();
    return out;
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
        out = poseAcrossFrames(g, nodes, a, in, mode, poseOne);
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

constexpr ModeInfo kModeInfo[] = {
    {strings::Mode::Default, "puppeteer.strings.default", "Default", "wand",
     "The rules pick: each joint poses the way its character's rules say; without a rule, moves bend the limb (IK) and turns are Forward."},
    {strings::Mode::Free, "puppeteer.strings.free", "Free", "feather",
     "Drags turn or move the joint alone: its children keep their world rotation (and on a move their place), nothing above it moves."},
    {strings::Mode::Forward, "puppeteer.strings.forward", "Forward", "arrow-down-tail",
     "Drags turn the joint and its children follow (FK), except what is pinned or a contact below it outside its own limb."},
    {strings::Mode::ForwardAll, "puppeteer.strings.forwardAll", "Forward All", "arrow-down-tail",
     "Plain FK: everything below the joint follows, pins, contacts and Isolate Rotation ignored."},
    {strings::Mode::Backward, "puppeteer.strings.backward", "Backward", "arrow-up-tail",
     "Drags turn the hierarchy the other way: what is pinned or locked below stays, or the joint itself, and the body swings around it."},
    {strings::Mode::BackwardAll, "puppeteer.strings.backwardAll", "Backward All", "arrow-up-tail",
     "Backward with nothing else held: the whole body swings rigidly around the anchor."},
    {strings::Mode::Bidirectional, "puppeteer.strings.bidirectional", "Bidirectional", "arrows-down-up",
     "Both sides give: a turn swings the parts below and the body above half each about the joint; a move carries the body half way "
     "and bends the limb for the rest. Pins outside the parts below hold."},
    {strings::Mode::All, "puppeteer.strings.all", "All", "arrows-down-up",
     "Bidirectional with nothing held: both sides turn rigidly about the joint, and a move carries the whole body."},
    {strings::Mode::Ik, "puppeteer.strings.ik", "IK", "hand-grab",
     "Drags put the joint where you drag it by bending its limb; on the hips, the body moves and pinned feet stay."},
    {strings::Mode::FullBody, "puppeteer.strings.fullBody", "Full Body", "man",
     "Drags pose the whole body to put the joint where you drag it, pins and locked hands and feet holding."},
};

void registerActions(ext::Context& ctx)
{
    const char* cat = "Puppeteer Strings";
    for (const ModeInfo& m : kModeInfo)
    {
        const strings::Mode mode = m.mode;
        actions::add(ctx, {.id = m.id,
                           .label = m.label,
                           .category = cat,
                           .run = [mode] { setStringsMode(mode); },
                           .doc = m.doc,
                           .checked = [mode] { return stringsMode() == mode; },
                           .icon = m.icon,
                           .keywords = {"strings", "ephemeral", "mode", "manipulation mode", "fk", "ik"}});
    }
    actions::add(ctx, {.id = "puppeteer.strings.next",
                       .label = "Next Strings Mode",
                       .category = cat,
                       .run =
                           [] {
                               const auto at = std::find(std::begin(strings::kModes), std::end(strings::kModes), stringsMode());
                               const size_t i = size_t(at - std::begin(strings::kModes));
                               setStringsMode(strings::kModes[(i + 1) % std::size(strings::kModes)]);
                           },
                       .doc = "Each Strings mode in turn, Default to Full Body.",
                       .icon = "arrows-right-left"});
    actions::add(ctx, {.id = "puppeteer.strings.allVariant",
                       .label = "All Variant",
                       .category = cat,
                       .run = [] { setStringsMode(strings::allVariant(stringsMode(), !strings::isAllVariant(stringsMode()))); },
                       .enabled = [] { return strings::allVariant(stringsMode(), true) != strings::allVariant(stringsMode(), false); },
                       .doc = "Forward, Backward or Bidirectional to its All variant and back: the All variants move rigidly, "
                              "nothing held, nothing isolated. Right-click the mode on the Spinner does the same.",
                       .checked = [] { return strings::isAllVariant(stringsMode()); },
                       .icon = "arrows-maximize",
                       .keywords = {"strings", "forward all", "backward all", "rigid"}});
    actions::add(ctx, {.id = "puppeteer.strings.isolate",
                       .label = "Isolate Rotation",
                       .category = cat,
                       .run =
                           [] {
                               isolateRef() = !isolateRef();
                               PrefStore::get().set(kPrefs, "/strings/isolate", isolateRef());
                               viewportTools::requestRedraw();
                           },
                       .doc = "The isolated joints (the head and hands, or what the character's rules say) keep their world "
                              "rotation while a drag carries them, in Default, Forward, Backward and Bidirectional.",
                       .checked = [] { return isolateRef(); },
                       .icon = "compass",
                       .keywords = {"strings", "isolate", "orient", "world orient", "head", "keep orientation"}});
    for (const auto& [type, label, doc] : {std::tuple{"local", "Rotation Type: Local", "A turn across a time range turns each frame about the joint where it is on that frame."},
                                           std::tuple{"pivot", "Rotation Type: Pivot",
                                                      "A turn across a time range swings each frame about where the joint is now, so its path "
                                                      "turns with it (turn a walk's direction from the hips)."}})
    {
        const std::string t = type;
        actions::add(ctx, {.id = std::string("puppeteer.strings.rotation.") + type,
                           .label = label,
                           .category = cat,
                           .run =
                               [t] {
                                   rotationTypeRef() = t;
                                   PrefStore::get().set(kPrefs, "/strings/rotationType", t);
                                   viewportTools::requestRedraw();
                               },
                           .doc = doc,
                           .checked = [t] { return rotationTypeRef() == t; },
                           .icon = t == "pivot" ? "rotate-clockwise-2" : "rotate",
                           .keywords = {"strings", "range", "rotation type", "pivot", "time range"}});
    }
    actions::add(ctx, {.id = "puppeteer.strings.tween",
                       .label = "Tween Range",
                       .category = cat,
                       .doc = "Blend the time slider's range toward the inbetween of the poses either side of it, easing out "
                              "across its falloff. With a range selected, the Spinner's breakdown slider tweens it live.",
                       .command = "stringsTween",
                       .icon = "chart-line",
                       .keywords = {"tween", "tween machine", "inbetween", "range", "blend"}});
    actions::add(ctx, {.id = "puppeteer.strings.mirror",
                       .label = "Mirror Pose",
                       .category = cat,
                       .doc = "The selected joints take their other side's pose, posed through the current Strings mode so pins "
                              "and contacts hold.",
                       .command = "stringsMirror",
                       .icon = "flip-vertical",
                       .keywords = {"mirror", "flip", "symmetry", "opposite side"}});
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
                       {"NE", {{"label", "Isolate Rotation"}, {"action", "puppeteer.strings.isolate"}}},
                       {"NW", {{"label", "Glue"}, {"action", "puppeteer.strings.glue"}}},
                       {"SW", {{"label", "Mirror Pose"}, {"action", "puppeteer.strings.mirror"}}},
                       {"SE", {{"label", "Tween Range"}, {"action", "puppeteer.strings.tween"}}},
                       {"overflow", json::array({{{"label", "Rotation Type: Local"}, {"action", "puppeteer.strings.rotation.local"}},
                                                 {{"label", "Rotation Type: Pivot"}, {"action", "puppeteer.strings.rotation.pivot"}},
                                                 {{"separator", true}},
                                                 {{"label", "Attach to Last Selected"}, {"action", "puppeteer.strings.attach"}},
                                                 {{"label", "Unglue"}, {"action", "puppeteer.strings.unglue"}},
                                                 {{"separator", true}},
                                                 {{"label", "Motion Trail"}, {"action", "puppeteer.strings.trail"}},
                                                 {{"label", "Line of Action"}, {"action", "puppeteer.strings.lineOfAction"}},
                                                 {{"label", "Breakdown Parts"}, {"action", "puppeteer.strings.breakdownParts"}},
                                                 {{"label", "Restore Bone Lengths"}, {"action", "puppeteer.strings.restoreLengths"}}})}});
    // The modes by direction: up the hierarchy is north, down it south.
    markingMenus::add(ctx, "puppeteerStringsModes",
                      {{"N", {{"label", "Backward"}, {"action", "puppeteer.strings.backward"}}},
                       {"NE", {{"label", "Backward All"}, {"action", "puppeteer.strings.backwardAll"}}},
                       {"S", {{"label", "Forward"}, {"action", "puppeteer.strings.forward"}}},
                       {"SE", {{"label", "Forward All"}, {"action", "puppeteer.strings.forwardAll"}}},
                       {"E", {{"label", "IK"}, {"action", "puppeteer.strings.ik"}}},
                       {"W", {{"label", "Full Body"}, {"action", "puppeteer.strings.fullBody"}}},
                       {"NW", {{"label", "Default"}, {"action", "puppeteer.strings.default"}}},
                       {"SW", {{"label", "Free"}, {"action", "puppeteer.strings.free"}}},
                       {"overflow", json::array({{{"label", "Bidirectional"}, {"action", "puppeteer.strings.bidirectional"}},
                                                 {{"label", "All"}, {"action", "puppeteer.strings.all"}},
                                                 {{"separator", true}},
                                                 {{"label", "Isolate Rotation"}, {"action", "puppeteer.strings.isolate"}}})}});
}
} // namespace

strings::Mode stringsMode() { return modeRef(); }

const char* stringsModeLabel(strings::Mode m)
{
    switch (m)
    {
    case strings::Mode::Default: return "Default";
    case strings::Mode::Free: return "Free";
    case strings::Mode::Forward: return "Forward";
    case strings::Mode::ForwardAll: return "Forward All";
    case strings::Mode::Backward: return "Backward";
    case strings::Mode::BackwardAll: return "Backward All";
    case strings::Mode::Bidirectional: return "Bidirectional";
    case strings::Mode::All: return "All";
    case strings::Mode::Ik: return "IK";
    case strings::Mode::FullBody: return "Full Body";
    }
    return "";
}

void setStringsMode(strings::Mode m)
{
    modeRef() = m;
    PrefStore::get().set(kPrefs, kModePref, std::string(strings::modeName(m)));
    viewportTools::requestRedraw();
}

double stringsStretch() { return stretchRef() ? kStretchLimit : 1.0; }

void addRangeFlags(json& call, const lila::scene::anim::TimeSelection& sel)
{
    call["range"] = json::array({sel.start, sel.end});
    call["falloffBefore"] = sel.before();
    call["falloffAfter"] = sel.after();
    call["falloffType"] = lila::scene::anim::rangeFalloffName(sel.shape);
    call["every"] = sel.every;
}

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
            {"mode", "m", T::String, nullptr, "The Strings mode; default the current one.", modeChoices()},
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
            {"falloffBefore", "fob", T::Float, nullptr, "With range: frames before it the pose eases out over, instead of falloff."},
            {"falloffAfter", "foa", T::Float, nullptr, "With range: frames after it the pose eases out over, instead of falloff."},
            {"falloffType", "fot", T::String, "smooth", "With range: how the pose eases out across the falloff.",
             {"smooth", "linear", "sharp", "soft", "constant"}},
            {"every", "ev", T::Int, 0, "With range: key every this many frames across it and its falloff, besides its keys; 0 only its keys.",
             {}, {}, 0.0, 100.0},
            {"rotationType", "rt", T::String, nullptr, "Across frames, relative: local turns each frame about the joint where it is then; "
             "pivot swings each frame about where the joint is now, so its path turns too. Default the Rotation Type setting.",
             {"local", "pivot"}},
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
            {"mode", "m", T::String, nullptr, "The Strings mode; default the current one.", modeChoices()},
            {"holds", "h", T::Bool, true, "False ignores pins and locks."},
            {"stretch", "st", T::Float, 1.0, "Above 1 lets bones lengthen up to this multiple."},
            {"relativeTo", "rt", T::List, nullptr, "Joints that keep their place in the world while the others zero (relative zero)."},
        },
        .run = stringsZero,
    });
    ctx.registerCommand({
        .name = "stringsRules",
        .doc = "A character's Strings rules: its contacts (joints that hold still while you pose the rest, its feet), its "
               "limbs (what bends together when IK or a hold reaches), its isolated joints (what Isolate Rotation keeps "
               "level) and the modes Default poses joints in. They come from its rig template or rig elements; contacts, "
               "addChain, isolate and defaultMode set them on this character instead, reset goes back. Returns the rules "
               "and where each came from.",
        .positionalDoc = "character or one of its joints (default the selection)",
        .flags = {
            {"contacts", "ct", T::List, nullptr, "Joints that hold by default, replacing the inherited ones."},
            {"addChain", "ac", T::List, nullptr, "[top joint, end joint]: a limb from top down to end, replacing limbs it overlaps."},
            {"addLine", "al", T::List, nullptr, "[name, top joint, end joint]: a line of action a stroke may bind to."},
            {"removeChain", "rc", T::String, nullptr, "A joint: the limb it is in goes."},
            {"removeLine", "rl", T::String, nullptr, "The line of action with this name goes."},
            {"isolate", "iso", T::List, nullptr, "Joints that keep their world rotation while Isolate Rotation is on, replacing the inherited ones."},
            {"defaultMode", "dm", T::List, nullptr, "[joint, mode]: the mode the joint poses in under Default; mode default (or empty) removes it."},
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
            {"mode", "m", T::String, nullptr, "The Strings mode; default the current one.", modeChoices()},
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
            {"mode", "m", T::String, nullptr, "The Strings mode; default the current one.", modeChoices()},
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
            {"mode", "m", T::String, nullptr, "The Strings mode: forwardAll, all and free blend every joint in its parent's space, the "
             "others keep the contacts in world and, with Isolate Rotation on, the isolated joints' world rotation. Default the current one.",
             modeChoices()},
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
        .name = "stringsTween",
        .doc = "Tween a range: every frame of it blends toward the inbetween of the poses just outside the range and its "
               "falloff, rotations on arcs and contacts in world, by amount times the frame's falloff weight. Keys the "
               "range's keys, or every n frames as the range says. Acts on the time slider's range without one.",
        .positionalDoc = "joints or a character (default the selection)",
        .flags = {
            {"layer", "ly", T::String, nullptr, "Key the edit onto this additive animation layer (made if missing) instead of the base."},
            {"range", "r", T::List, nullptr, "[start, end] seconds; default the time slider's range."},
            {"falloff", "fo", T::Float, 0.0, "With range: frames either side the tween eases out over."},
            {"falloffType", "fot", T::String, "smooth", "With range: how it eases out.", {"smooth", "linear", "sharp", "soft", "constant"}},
            {"every", "ev", T::Int, 0, "With range: key every this many frames; 0 only its keys.", {}, {}, 0.0, 100.0},
            {"amount", "a", T::Float, 1.0, "1 takes the inbetween, 0 leaves the motion, past 1 overshoots, below 0 exaggerates.", {}, {}, -1.0, 2.0},
            {"spacing", "sp", T::String, "linear", "How the inbetween runs across the range.", {"linear", "easeIn", "easeOut", "easeInOut"}},
            {"mode", "m", T::String, nullptr, "The Strings mode: forwardAll, all and free blend every joint locally. Default the current one.", modeChoices()},
            {"all", "al", T::Bool, true, "The whole character; False only the given joints."},
            {"preview", "pv", T::Bool, false, "Tween the current frame and the ghosts' frames without keying, for a drag in progress."},
            nodeFlag({"world", "wo", T::List, nullptr, "Joints that blend in world space; default the character's contacts."}, "joint"),
        },
        .run = stringsTween,
    });
    ops::options::setOverride("stringsTween", {.order = {"amount", "spacing"}, .hidden = {"range", "preview", "world", "layer"}});
    ctx.registerCommand({
        .name = "curveRangeEdit",
        .doc = "Edit curves across a time range, as the Curve Overlay's drags do: offset adds to every frame of the range "
               "(eased out across its falloff), tween blends every frame toward the line between the values just outside it. "
               "Keys the range's keys, or every n frames as the range says. With rotationType pivot, a rotate channel of the "
               "joint that carries a character turns through stringsPose so its path swings about where it is now. Acts on "
               "the time slider's range without one. Returns {channels, pivot, frames}.",
        .flags = {
            {"channels", "ch", T::List, nullptr, "Plugs, one axis each: hips.translateY, spine_01.rotateX."},
            {"offset", "o", T::Float, nullptr, "Added to every frame, in the channel's units (cm, degrees)."},
            {"tween", "tw", T::Float, nullptr, "1 takes the line between the values either side, past 1 overshoots, below 0 exaggerates.", {}, {}, -1.0, 2.0},
            {"range", "r", T::List, nullptr, "[start, end] seconds; default the time slider's range."},
            {"falloff", "fo", T::Float, 0.0, "With range: frames either side the edit eases out over."},
            {"falloffBefore", "fob", T::Float, nullptr, "With range: frames before it the edit eases out over."},
            {"falloffAfter", "foa", T::Float, nullptr, "With range: frames after it the edit eases out over."},
            {"falloffType", "fot", T::String, "smooth", "With range: how the edit eases out.", {"smooth", "linear", "sharp", "soft", "constant"}},
            {"every", "ev", T::Int, 0, "With range: key every this many frames; 0 only its keys.", {}, {}, 0.0, 100.0},
            {"rotationType", "rt", T::String, nullptr, "local or pivot; default the Rotation Type setting.", {"local", "pivot"}},
            {"layer", "ly", T::String, nullptr, "Key the edit onto this additive animation layer (made if missing) instead of the base."},
        },
        .run = curveRangeEdit,
    });
    ctx.registerCommand({
        .name = "stringsMirror",
        .doc = "Mirror in a Strings mode: the joints take their other side's pose (left onto right, centre joints reflected), "
               "posed through the mode so what holds stays put: in Backward the planted feet stay while the body mirrors. "
               "Parents first. Returns what each pose did.",
        .positionalDoc = "joints or a character (default the selection)",
        .flags = {
            {"mode", "m", T::String, nullptr, "The Strings mode; default the current one.", modeChoices()},
            {"mirrorAxis", "ma", T::String, "x", "Axis the mirror plane faces.", {"x", "y", "z"}},
            {"holds", "h", T::Bool, true, "False ignores pins and locks."},
        },
        .run = stringsMirror,
    });
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
               "Strings unit given the character's rule web. Each control's Mode and Isolate Rotation channels pick how it poses; Pin, "
               "Lock and Glue/Attach channels carry pins, locked IK chains and glue, and keyed locks go into a Level Sequence. "
               "Returns {rig, skeletalMesh, unreal}, or {rig, skeletalMesh, payload} with dryRun.",
        .positionalDoc = "character or one of its joints (default the selection)",
        .flags = {{"skeletalMesh", "sm", T::String, nullptr, "The skeletal mesh asset. Default: the root's skeletal mapping."},
                  {"rig", "r", T::String, nullptr, "The Control Rig asset to make or update. Default: CR_<mesh>_Strings beside the mesh."},
                  {"mode", "m", T::String, nullptr, "The mode the controls start in; default the current one.", modeChoices()},
                  {"isolate", "iso", T::Bool, nullptr, "The controls start with Isolate Rotation on; default the Isolate Rotation setting."},
                  {"sequence", "sq", T::String, nullptr,
                   "Level Sequence for the IK chains' lock keys, keyed on the rig's Lock channels. Default: LS_<mesh>_Strings beside the "
                   "rig when a chain's lock is keyed."},
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
