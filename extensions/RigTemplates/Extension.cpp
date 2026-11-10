// Rig Templates: markers that describe a skeleton, built into joints, used to orient existing skeletons, and the
// pose library, picker and naming that go with a character of that kind.

#include "RigTemplates.h"

#include <lila/Command.h>
#include <lila/Log.h>
#include <lila/Node.h>
#include <lila/Picker.h>
#include <lila/Poses.h>
#include <lila/Relocate.h>
#include <lila/Rig.h>
#include <lila/RigElements.h>
#include <lila/Spaces.h>
#include <lila/Transform.h>
#include <lila/Undo.h>

#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

using namespace lila;
using namespace lila::scene;

namespace rigt
{
namespace
{
using Character = poses::Character;

std::map<std::string, Entry> gTemplates;
ext::Context* gCtx = nullptr;
std::map<int, std::function<void()>> gListeners;
int gNextListener = 1;

void notify()
{
    auto copy = gListeners;
    for (auto& [id, fn] : copy)
        fn();
}

Graph& graph() { return Graph::current(); }

std::string bare(const std::string& name) { return poses::stripNamespace(name); }

/** "a, b,c" -> {a, b, c}. */
std::vector<std::string> splitNames(const std::string& s)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= s.size())
    {
        size_t comma = std::min(s.find(',', at), s.size());
        std::string n = s.substr(at, comma - at);
        n.erase(0, n.find_first_not_of(' '));
        n.erase(n.find_last_not_of(' ') + 1);
        if (!n.empty())
            out.push_back(n);
        at = comma + 1;
    }
    return out;
}

json vecJson(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }
glm::dvec3 vecOf(const json& j) { return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()}; }

Node& need(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named " + name);
    return *n;
}

glm::dmat4 worldOf(Graph& g, NodeId id)
{
    g.evaluate();
    return g.node(id)->as<glm::dmat4>(Transform::kWorldMatrix);
}

glm::dquat rotationOf(const glm::dmat4& m)
{
    glm::dmat3 r(glm::normalize(glm::dvec3(m[0])), glm::normalize(glm::dvec3(m[1])), glm::normalize(glm::dvec3(m[2])));
    return glm::normalize(glm::quat_cast(r));
}

glm::dvec3 degrees(const glm::dquat& q, RotateOrder order = RotateOrder::XYZ) { return matrixToEuler(glm::mat3_cast(q), order); }

/** The top joint above a joint, and every joint under it parent first. */
NodeId topJoint(Graph& g, NodeId id)
{
    while (true)
    {
        NodeId p = g.parent(id);
        if (p == kNoNode || !rig::isJoint(*g.node(p)))
            return id;
        id = p;
    }
}

std::vector<NodeId> skeletonOf(Graph& g, NodeId top)
{
    std::vector<NodeId> out;
    std::vector<NodeId> stack{top};
    while (!stack.empty())
    {
        NodeId id = stack.back();
        stack.pop_back();
        out.push_back(id);
        const std::vector<NodeId>& kids = g.children(id);
        for (auto it = kids.rbegin(); it != kids.rend(); ++it)
            if (rig::isJoint(*g.node(*it)))
                stack.push_back(*it);
    }
    return out;
}

/** Template marker -> joint in the skeleton, by joint name, marker name, label and aliases, ignoring namespace and case. */
std::map<std::string, NodeId> mapJoints(Graph& g, const Template& t, const std::vector<NodeId>& joints)
{
    std::map<std::string, NodeId> byName;
    for (NodeId id : joints)
        byName.emplace(lower(bare(g.node(id)->name())), id);
    std::map<std::string, NodeId> out;
    std::set<NodeId> used;
    for (const Marker& m : t.markers)
    {
        std::vector<std::string> names{m.joint, m.name};
        if (!m.label.empty())
            names.push_back(m.label);
        if (t.aliases.contains(m.joint))
            for (const json& a : t.aliases[m.joint])
                names.push_back(a.get<std::string>());
        for (const std::string& a : aliasesFor(m.label))
            names.push_back(a);
        for (const std::string& n : names)
            if (auto it = byName.find(lower(n)); it != byName.end() && !used.contains(it->second))
            {
                out[m.name] = it->second;
                used.insert(it->second);
                break;
            }
    }
    return out;
}

/** The template a skeleton matches best (most labelled free markers found), and how many matched. */
std::pair<const Entry*, size_t> bestTemplate(Graph& g, const std::vector<NodeId>& joints)
{
    const Entry* best = nullptr;
    size_t bestCount = 0, bestTotal = 1;
    for (auto& [id, e] : gTemplates)
    {
        std::map<std::string, NodeId> m = mapJoints(g, e.tmpl, joints);
        size_t total = 0, found = 0;
        for (const Marker& mk : e.tmpl.markers)
            if (mk.isFree() && !mk.label.empty())
            {
                ++total;
                found += m.contains(mk.name);
            }
        // Ties go to the shipped template.
        const bool better = found * bestTotal > bestCount * total;
        const bool tie = found * bestTotal == bestCount * total && best && e.tier == "builtin" && best->tier != "builtin";
        if (total && (better || tie))
            best = &e, bestCount = found, bestTotal = total;
    }
    return {best, bestCount * 2 >= bestTotal ? bestCount : 0};
}

std::vector<std::string> stringList(const json& j)
{
    std::vector<std::string> out;
    if (j.is_string())
        out.push_back(j.get<std::string>());
    else if (j.is_array())
        for (const json& e : j)
            out.push_back(e.get<std::string>());
    return out;
}

// Instances

const Template& instanceTemplate(Graph& g, NodeId root, Template& storage)
{
    const Node* n = g.node(root);
    std::string data = str(*n, "templateData");
    if (!data.empty())
    {
        try
        {
            storage = Template::fromJson(json::parse(data));
            return storage;
        }
        catch (const std::exception& e)
        {
            log::warn("rig template {}: stored template unreadable ({}); using the library's", n->path(), e.what());
        }
    }
    const Entry* e = findTemplate(str(*n, "templateId"));
    if (!e)
        throw CommandError("rig template " + n->path() + ": template '" + str(*n, "templateId") + "' is not loaded");
    storage = e->tmpl;
    return storage;
}

/** Marker name -> marker node under an instance. */
std::map<std::string, NodeId> markersOf(Graph& g, NodeId root)
{
    std::map<std::string, NodeId> out;
    for (NodeId d : g.descendants(root))
        if (const Node* n = g.node(d); n && n->type().isA(kMarkerType))
            out[str(*n, "marker")] = d;
    return out;
}

std::map<std::string, glm::dvec3> markerPositions(Graph& g, NodeId root)
{
    std::map<std::string, glm::dvec3> out;
    for (auto& [name, id] : markersOf(g, root))
        out[name] = glm::dvec3(worldOf(g, id)[3]);
    return out;
}

NodeId instanceArg(Graph& g, const std::string& name, const char* cmd)
{
    NodeId root = instanceOf(g, need(g, name, cmd).id());
    if (root == kNoNode)
        throw CommandError(std::string(cmd) + ": " + name + " is not part of a rig template instance");
    return root;
}

std::vector<NodeId> meshesArg(Graph& g, const json& names, const char* cmd)
{
    std::vector<NodeId> out;
    for (const std::string& n : stringList(names))
        out.push_back(need(g, n, cmd).id());
    return out;
}

void setMeshes(Graph& g, NodeId node, const std::vector<NodeId>& meshes)
{
    if (int a = attrOf(*g.node(node), "centerMeshes"); a >= 0)
        g.setValue({node, a, -1}, rig::makeCenterMeshList(g, meshes));
}

std::string placeInstance(Graph& g, const Template& t, const std::string& name, const std::vector<NodeId>& meshes)
{
    UndoChunk chunk("Place Rig Template");
    const NodeType* rootType = types::find(kRootType);
    const NodeType* markerType = types::find(kMarkerType);
    Node* root = g.createNode(*rootType, name.empty() ? t.id + "_template" : name);
    const NodeId rootId = root->id();
    g.setValue({rootId, attrOf(*root, "templateId"), -1}, t.id);
    g.setValue({rootId, attrOf(*root, "templateData"), -1}, t.toJson().dump());

    std::map<std::string, NodeId> made;
    std::map<std::string, glm::dvec3> placed;
    for (const Marker& m : t.markers)
    {
        if (!m.isFree())
            continue;
        // Under the nearest free ancestor, so dragging a marker carries the ones below it.
        std::string up = m.parent;
        while (!up.empty() && !made.contains(up))
            up = t.marker(up) ? t.marker(up)->parent : std::string();
        const NodeId parent = up.empty() ? rootId : made[up];
        const glm::dvec3 parentPos = up.empty() ? glm::dvec3(0.0) : placed[up];
        Node* n = g.createNode(*markerType, m.name + "_marker", parent, false);
        const NodeId id = n->id();
        auto set = [&](const char* attr, PlugValue v) { g.setValue({id, attrOf(*g.node(id), attr), -1}, std::move(v)); };
        set("marker", m.name);
        set("label", m.label);
        set("joint", m.joint);
        set("mirror", m.mirror);
        set("side", int64_t(m.side == "left" ? 1 : m.side == "right" ? 2 : 0));
        set("aimAt", m.orient == Orient::Aim ? m.aimAt : std::string());
        set("aimAxis", m.aim);
        set("upAxis", m.up);
        set("upVector", m.upVector);
        set("radius", m.radius);
        set("centerInMesh", m.centerInMesh);
        g.setValue({id, Transform::kTranslate, -1}, m.position - parentPos);
        if (!meshes.empty())
            setMeshes(g, id, meshes);
        made[m.name] = id;
        placed[m.name] = m.position;
    }
    if (!meshes.empty())
        setMeshes(g, rootId, meshes);
    return g.node(rootId)->path();
}

/** Template, marker positions and bound meshes from an instance root or a template id. */
struct Source
{
    Template tmpl;
    std::map<std::string, glm::dvec3> positions;
    std::vector<NodeId> meshes;
    NodeId root = kNoNode;
};

Source sourceArg(Graph& g, const std::string& arg, const char* cmd)
{
    Source s;
    if (Node* n = g.find(arg); n && instanceOf(g, n->id()) != kNoNode)
    {
        s.root = instanceOf(g, n->id());
        instanceTemplate(g, s.root, s.tmpl);
        s.positions = markerPositions(g, s.root);
        s.meshes = rig::centerMeshes(g, s.root);
        return s;
    }
    const Entry* e = findTemplate(arg);
    if (!e)
        throw CommandError(std::string(cmd) + ": '" + arg + "' is neither a template instance nor a loaded template");
    s.tmpl = e->tmpl;
    return s;
}

/** Gives the built joints the template's default spaces, at the pose they were built in. */
void addTemplateSpaces(Graph& g, const Template& t, const std::map<std::string, NodeId>& made)
{
    for (const Marker& m : t.markers)
    {
        auto joint = made.find(m.name);
        if (joint == made.end())
            continue;
        for (const std::string& s : t.spacesFor(m))
        {
            std::string space = s;
            if (s != "world" && s != "parent")
            {
                const Marker* target = t.markerFor(s);
                auto built = target ? made.find(target->name) : made.end();
                if (built == made.end())
                {
                    log::warn("buildRigSkeleton: {} has no joint for space '{}'", m.name, s);
                    continue;
                }
                space = g.node(built->second)->path();
            }
            try
            {
                spaces::addSpace(g, joint->second, space);
            }
            catch (const std::invalid_argument& e)
            {
                log::warn("buildRigSkeleton: {}", e.what());
            }
        }
    }
}

/** Sets the template's IK limits on the built joints through ikLimit, which also refreshes any handle solving them. */
void applyLimits(Graph& g, const Template& t, const std::map<std::string, NodeId>& made)
{
    const json limits = t.rig.value("limits", json::object());
    for (const auto& [marker, l] : limits.items())
    {
        auto it = made.find(marker);
        if (it == made.end())
            continue;
        json call = l;
        call["args"] = json::array({g.node(it->second)->path()});
        ops::execute("ikLimit", call);
    }
}

/** Whether an ikChains entry builds as a rubber hose under the build's hose mode. */
bool hosed(const json& chain, const std::string& mode)
{
    if (mode == "template")
        return chain.value("hose", json::object()).value("enabled", false);
    if (mode == "all" || mode == "none")
        return mode == "all";
    const std::string name = chain.value("name", std::string());
    for (const std::string& n : splitNames(mode))
        if (n == name)
            return true;
    return false;
}

/**
 * The limb from start to end as a rubber hose through hoseLimb. Its twist and corrective joints stop deforming with the
 * limb's own: the hose joints carry the skin from shoulder or hip to wrist or ankle.
 */
NodeId buildHose(Graph& g, NodeId start, NodeId end, const json& opts)
{
    const json made = ops::execute("hoseLimb", {{"args", {g.node(start)->path(), g.node(end)->path()}},
                                                {"joints", opts.value("joints", 12)},
                                                {"hose", opts.value("amount", 1.0)},
                                                {"autoArc", opts.value("autoArc", 1.0)}});
    const std::vector<NodeId> below = g.descendants(end);
    for (NodeId d : g.descendants(start))
        if (d != end && rig::isJoint(*g.node(d)) && std::find(below.begin(), below.end(), d) == below.end())
            g.setValue({d, Joint::kDeform, -1}, false);
    return g.find(made.get<std::string>())->id();
}

/**
 * The template's rig elements on the built joints: IK limits, IK chains with their poles, reverse feet on the sole
 * pivots (or the skinned sole), lookers, twist drivers, rubber hose limbs (hose: see hosed) and deform flags, grouped
 * under <top>_rig. Entries naming joints the build skipped are left out. Returns the elements made.
 */
json buildTemplateRig(Graph& g, const Template& t, const Solved& solved, const std::map<std::string, NodeId>& made, const std::string& top,
                      bool skinned, const std::string& hose)
{
    namespace rigel = lila::scene::rigel;
    json out = json::array();
    auto joint = [&](const json& e, const char* key) -> NodeId {
        if (!e.contains(key))
            return kNoNode;
        auto it = made.find(e[key].get<std::string>());
        return it == made.end() ? kNoNode : it->second;
    };
    auto markerOf = [&](NodeId id) {
        for (const auto& [marker, j] : made)
            if (j == id)
                return marker;
        return std::string();
    };
    auto record = [&](NodeId id) { out.push_back(g.node(id)->path()); };
    const json& rig = t.rig;
    if (rig.contains("nonDeform"))
        for (const json& n : rig["nonDeform"])
            if (auto it = made.find(n.get<std::string>()); it != made.end())
                g.setValue({it->second, Joint::kDeform, -1}, false);
    applyLimits(g, t, made);
    if (!rig.contains("ikChains") && !rig.contains("lookers") && !rig.contains("twists"))
        return out;
    const NodeId group = g.createNode(*types::find("transform"), top.substr(top.rfind('|') + 1) + "_rig")->id();
    std::map<std::string, NodeId> chains;
    for (const json& c : rig.value("ikChains", json::array()))
    {
        const NodeId s = joint(c, "start"), e = joint(c, "end");
        if (!s || !e)
            continue;
        lila::scene::rigel::ChainOptions o{.name = g.node(e)->name() + "_ik", .parent = group};
        if (c.contains("pole"))
            o.poleVector = vecOf(c["pole"]);
        try
        {
            const NodeId id = rigel::createIkChain(g, s, e, o);
            chains[c.value("name", std::string())] = id;
            record(id);
        }
        catch (const std::invalid_argument& err)
        {
            log::warn("buildRigSkeleton: IK chain {}: {}", c.value("name", std::string()), err.what());
        }
    }
    for (const json& f : rig.value("reverseFeet", json::array()))
    {
        auto chain = chains.find(f.value("chain", std::string()));
        if (chain == chains.end())
            continue;
        lila::scene::rigel::FootOptions o{.ball = joint(f, "ball"), .toe = joint(f, "toe"), .parent = group};
        const std::vector<NodeId> js = rigel::chainJoints(g, chain->second);
        const std::string ankle = js.empty() ? std::string() : markerOf(js.back());
        // The skinned sole is the better guide when there is one; the template's pivots stand in otherwise.
        if (!skinned && f.contains("pivots") && solved.position.contains(ankle))
        {
            const glm::dvec3 at = solved.position.at(ankle);
            const glm::dquat r = solved.rotation.at(ankle);
            std::array<glm::dvec3, 5> pv;
            const char* keys[5] = {"heel", "ball", "toe", "inner", "outer"};
            bool ok = true;
            for (int i = 0; i < 5 && ok; ++i)
            {
                ok = f["pivots"].contains(keys[i]);
                if (ok)
                    pv[size_t(i)] = at + r * vecOf(f["pivots"][keys[i]]);
            }
            if (ok)
                o.pivots = pv;
        }
        try
        {
            record(rigel::createReverseFoot(g, chain->second, o));
        }
        catch (const std::invalid_argument& err)
        {
            log::warn("buildRigSkeleton: reverse foot on {}: {}", chain->first, err.what());
        }
    }
    for (const json& l : rig.value("lookers", json::array()))
    {
        const NodeId j = joint(l, "joint");
        if (!j)
            continue;
        lila::scene::rigel::LookerOptions o{.aimAxis = l.value("aimAxis", std::string("x")), .upAxis = l.value("upAxis", std::string("y")),
                                            .distance = l.value("distance", 50.0), .weight = l.value("weight", 0.0), .parent = group};
        try
        {
            record(rigel::createLooker(g, j, o));
        }
        catch (const std::invalid_argument& err)
        {
            log::warn("buildRigSkeleton: looker on {}: {}", g.node(j)->path(), err.what());
        }
    }
    for (const json& tw : rig.value("twists", json::array()))
    {
        const NodeId j = joint(tw, "joint");
        if (!j)
            continue;
        lila::scene::rigel::TwistOptions o{.source = joint(tw, "source")};
        if (tw.contains("weight"))
            o.weight = tw["weight"].get<double>();
        try
        {
            record(rigel::createTwistDriver(g, j, o));
        }
        catch (const std::invalid_argument& err)
        {
            log::warn("buildRigSkeleton: twist driver on {}: {}", g.node(j)->path(), err.what());
        }
    }
    for (const json& c : rig.value("ikChains", json::array()))
    {
        const NodeId s = joint(c, "start"), e = joint(c, "end");
        if (!s || !e || !hosed(c, hose))
            continue;
        try
        {
            record(buildHose(g, s, e, c.value("hose", json::object())));
        }
        catch (const std::exception& err)
        {
            log::warn("buildRigSkeleton: hose limb {}: {}", c.value("name", std::string()), err.what());
        }
    }
    return out;
}

json buildSkeleton(Graph& g, const Source& src, const std::string& prefix, bool includeIk, bool bindSkin, const std::vector<NodeId>& meshes,
                   bool withSpaces, bool withRig, const std::string& hose)
{
    const Template& t = src.tmpl;
    Solved solved = solve(t, src.positions);
    const NodeType* jointType = types::find("joint");
    if (!jointType)
        throw CommandError("buildRigSkeleton: joints are not available");

    UndoChunk chunk("Build Skeleton");
    std::map<std::string, NodeId> made;
    json names = json::array();
    std::string top;
    for (const Marker& m : t.markers)
    {
        if (!includeIk && m.hasFlag("ik"))
            continue;
        std::string up = m.parent;
        while (!up.empty() && !made.contains(up))
            up = t.marker(up) ? t.marker(up)->parent : std::string();
        const NodeId parent = up.empty() ? kNoNode : made[up];
        Node* j = g.createNode(*jointType, prefix + m.joint, parent, false);
        const NodeId id = j->id();
        const glm::dvec3 p = solved.position[m.name];
        const glm::dquat r = solved.rotation[m.name];
        const glm::dvec3 pp = up.empty() ? glm::dvec3(0.0) : solved.position[up];
        const glm::dquat pr = up.empty() ? glm::dquat(1, 0, 0, 0) : solved.rotation[up];
        g.setValue({id, Transform::kTranslate, -1}, glm::inverse(pr) * (p - pp));
        g.setValue({id, Joint::kJointOrient, -1}, degrees(glm::inverse(pr) * r));
        g.setValue({id, Joint::kRadius, -1}, m.radius);
        g.setValue({id, Joint::kSide, -1}, int64_t(m.side == "left" ? Joint::kLeft : m.side == "right" ? Joint::kRight : Joint::kCenter));
        g.setValue({id, Joint::kCenterInMesh, -1}, m.centerInMesh && !meshes.empty());
        made[m.name] = id;
        names.push_back(g.node(id)->path());
        if (up.empty() && top.empty())
            top = g.node(id)->path();
    }
    if (withSpaces)
        addTemplateSpaces(g, t, made);
    json skins = json::array();
    if (bindSkin)
    {
        if (meshes.empty())
            throw CommandError("buildRigSkeleton: bindSkin needs meshes (give meshes or bind the instance first)");
        for (NodeId mesh : meshes)
            skins.push_back(ops::execute("skinCluster", {{"args", {top, g.node(mesh)->path()}}, {"bindTo", "hierarchy"}}));
    }
    // After the bind, so the auto rule can prefer bones the skin weights. Redirects set before it are kept.
    if (Node* root = g.find(top))
        rig::applyAutoSelectAs(g, {root->id()});
    json elements = withRig ? buildTemplateRig(g, t, solved, made, top, bindSkin, hose) : json::array();
    return {{"root", top}, {"joints", names}, {"skinClusters", skins}, {"rig", elements}};
}

/** Whether anything drives the joints' translate, rotate or scale (keys, layers, constraints). */
bool animated(Graph& g, const std::set<NodeId>& joints)
{
    for (const auto& [dst, src] : g.connections())
        if (joints.contains(dst.node) && (dst.attr == Transform::kTranslate || dst.attr == Transform::kRotate || dst.attr == Transform::kScale))
            return true;
    return false;
}

/** Whether a skinCluster takes any of the joints as an influence. */
bool skinned(Graph& g, const std::set<NodeId>& joints)
{
    for (const auto& [dst, src] : g.connections())
        if (joints.contains(src.node) && g.node(dst.node)->type().isA("skinInfluence"))
            return true;
    return false;
}

glm::dquat eulerQuat(const glm::dvec3& deg, RotateOrder order) { return glm::quat_cast(glm::dmat3(eulerToMatrix(deg, order))); }

/** jointOrient giving the joint world rotation `want` under its current parent, rotate and rotateAxis. */
glm::dquat orientFor(Graph& g, NodeId id, const glm::dquat& want, bool zeroRotate)
{
    const Node& j = *g.node(id);
    const glm::dquat parentRot = g.parent(id) == kNoNode ? glm::dquat(1, 0, 0, 0) : rotationOf(worldOf(g, g.parent(id)));
    glm::dquat r(1, 0, 0, 0), ra(1, 0, 0, 0);
    if (!zeroRotate)
    {
        r = eulerQuat(j.as<glm::dvec3>(Transform::kRotate), RotateOrder(j.as<int64_t>(Transform::kRotateOrder)));
        ra = eulerQuat(j.as<glm::dvec3>(Transform::kRotateAxis), RotateOrder::XYZ);
    }
    // world = parent * jointOrient * rotate * rotateAxis, solved for jointOrient.
    return glm::normalize(glm::inverse(parentRot) * want * glm::inverse(r * ra));
}

json orientSkeleton(Graph& g, const Template& t, NodeId anyJoint, const std::vector<std::string>& limbs, std::string preserve)
{
    const NodeId top = topJoint(g, anyJoint);
    const std::vector<NodeId> joints = skeletonOf(g, top);
    const std::set<NodeId> jointSet(joints.begin(), joints.end());
    const bool bound = skinned(g, jointSet) || animated(g, jointSet);
    if (preserve.empty())
        preserve = bound ? "worldPose" : "rest";
    if (preserve == "rest" && bound)
        throw CommandError("orientRigSkeleton: preserve=rest is for skeletons with no skin or animation; use worldPose or channels");
    std::map<std::string, NodeId> mapped = mapJoints(g, t, joints);
    if (mapped.empty())
        throw CommandError("orientRigSkeleton: no joint of " + g.node(top)->path() + " matches template " + t.id);

    std::set<std::string> wanted;
    for (const std::string& l : limbs)
    {
        const Limb* limb = t.limb(l);
        if (!limb)
            throw CommandError("orientRigSkeleton: template " + t.id + " has no limb '" + l + "'");
        wanted.insert(limb->markers.begin(), limb->markers.end());
        // A limb's helpers (twist joints and the like) follow their limb joints.
        for (const Marker& m : t.markers)
            if (!m.isFree() && wanted.contains(m.parent))
                wanted.insert(m.name);
    }

    std::map<std::string, glm::dvec3> known;
    for (auto& [name, id] : mapped)
        known[name] = glm::dvec3(worldOf(g, id)[3]);
    Solved solved = solve(t, known, false);

    // Parents first, as they appear in the skeleton.
    std::map<NodeId, std::string> byJoint;
    for (auto& [name, id] : mapped)
        if (wanted.empty() || wanted.contains(name))
            byJoint[id] = name;
    std::vector<std::pair<NodeId, std::string>> order;
    for (NodeId id : joints)
        if (auto it = byJoint.find(id); it != byJoint.end())
            order.push_back({id, it->second});

    UndoChunk chunk("Orient Skeleton To Template");
    json changed = json::array();
    if (preserve == "rest")
    {
        // A plain skeleton: the joints take the template's axes with rotate and rotateAxis zeroed, children stay put.
        for (auto& [id, name] : order)
        {
            std::vector<std::pair<NodeId, glm::dmat4>> kids;
            for (NodeId c : g.children(id))
                if (g.node(c)->type().isA("transform"))
                    kids.push_back({c, worldOf(g, c)});
            const glm::dquat jo = orientFor(g, id, solved.rotation[name], true);
            g.setValue({id, Transform::kRotate, -1}, glm::dvec3(0.0));
            g.setValue({id, Transform::kRotateAxis, -1}, glm::dvec3(0.0));
            g.setValue({id, Joint::kJointOrient, -1}, degrees(jo));
            const glm::dmat4 inv = glm::inverse(worldOf(g, id));
            for (auto& [c, w] : kids)
                Transform::setLocalMatrix(g, c, inv * w);
            changed.push_back(g.node(id)->path());
        }
        return changed;
    }

    const bool wasEditing = [&] {
        for (NodeId id : g.nodes())
            if (g.node(id)->type().name == "skeletonEdit")
                return true;
        return false;
    }();
    if (!wasEditing)
        ops::execute("editSkeleton", {{"args", {g.node(top)->path()}}, {"on", true}});
    for (auto& [id, name] : order)
    {
        const Node& j = *g.node(id);
        const glm::dquat jo = orientFor(g, id, solved.rotation[name], false);
        if (std::abs(glm::dot(eulerQuat(j.as<glm::dvec3>(Joint::kJointOrient), RotateOrder::XYZ), jo)) > 1.0 - 1e-14)
            continue;
        ops::execute("moveSkinnedJoints", {{"args", {j.path()}}, {"jointOrient", vecJson(degrees(jo))}, {"preserve", preserve}});
        changed.push_back(j.path());
    }
    if (!wasEditing)
        ops::execute("editSkeleton", {{"on", false}, {"preserve", preserve}});
    return changed;
}

std::vector<SourceJoint> sourceJoints(Graph& g, NodeId top)
{
    std::vector<SourceJoint> out;
    for (NodeId id : skeletonOf(g, top))
    {
        SourceJoint s;
        s.name = bare(g.node(id)->name());
        s.parent = id == top ? std::string() : bare(g.node(g.parent(id))->name());
        glm::dmat4 w = worldOf(g, id);
        s.position = glm::dvec3(w[3]);
        s.rotation = rotationOf(w);
        out.push_back(std::move(s));
    }
    return out;
}

json summary(const Entry& e)
{
    size_t free = 0;
    for (const Marker& m : e.tmpl.markers)
        free += m.isFree();
    return {{"id", e.tmpl.id}, {"name", e.tmpl.name}, {"description", e.tmpl.description}, {"tier", e.tier},
            {"file", e.file.empty() ? std::string() : e.file.generic_string()}, {"markers", e.tmpl.markers.size()}, {"freeMarkers", free},
            {"limbs", e.tmpl.limbs.size()}, {"poses", e.tmpl.poses.size()}};
}

std::optional<Character> resolveCharacter(Graph& g, NodeId node)
{
    const Node* n = g.node(node);
    if (!n || !rig::isJoint(*n))
        return std::nullopt;
    NodeId top = topJoint(g, node);
    std::vector<NodeId> joints = skeletonOf(g, top);
    auto [entry, count] = bestTemplate(g, joints);
    if (!entry || count == 0)
        return std::nullopt;
    Character c;
    c.id = entry->tmpl.id;
    c.templateId = entry->tmpl.id;
    c.root = top;
    c.nameSpace = g.node(top)->nameSpace();
    c.nodes.push_back(top);
    for (NodeId d : g.descendants(top))
        if (g.node(d)->type().isA("transform"))
            c.nodes.push_back(d);
    return c;
}
} // namespace

std::vector<const Entry*> templates()
{
    std::vector<const Entry*> out;
    for (auto& [id, e] : gTemplates)
        out.push_back(&e);
    return out;
}

const Entry* findTemplate(const std::string& id)
{
    auto it = gTemplates.find(id);
    return it == gTemplates.end() ? nullptr : &it->second;
}

const Entry& addTemplate(Template t, std::filesystem::path file, std::string tier)
{
    const std::string owner = gCtx ? gCtx->owner() : std::string("RigTemplates");
    for (auto& [name, pj] : t.poses.items())
    {
        try
        {
            json j = pj;
            j["name"] = j.value("name", name);
            poses::Pose p = poses::Pose::fromJson(j);
            p.character = t.id;
            p.templateId = t.id;
            poses::registerTemplatePose(p, owner);
        }
        catch (const std::exception& e)
        {
            log::warn("rig template {}: pose {}: {}", t.id, name, e.what());
        }
    }
    if (!t.aliases.empty())
        poses::registerAliases(t.id, t.aliases, owner);
    if (gCtx && t.picker.is_object() && !t.picker.empty())
    {
        try
        {
            picker::Document doc = picker::generateFromLayout(t.picker);
            if (doc.name.empty())
                doc.name = t.id;
            doc.templateId = t.id;
            picker::registerTemplatePicker(*gCtx, doc);
        }
        catch (const std::exception& e)
        {
            log::warn("rig template {}: picker: {}", t.id, e.what());
        }
    }
    std::string id = t.id;
    gTemplates[id] = Entry{std::move(t), std::move(file), std::move(tier)};
    notify();
    return gTemplates[id];
}

int addListener(std::function<void()> fn)
{
    gListeners[gNextListener] = std::move(fn);
    return gNextListener++;
}

void removeListener(int id) { gListeners.erase(id); }

std::filesystem::path userDir() { return poses::tierDir(poses::Tier::User).parent_path() / "rigTemplates"; }

std::filesystem::path workspaceDir()
{
    std::filesystem::path p = poses::tierDir(poses::Tier::Workspace);
    return p.empty() ? p : p.parent_path() / "rigTemplates";
}

NodeId instanceOf(Graph& g, NodeId node)
{
    for (NodeId id = node; id != kNoNode; id = g.parent(id))
    {
        const Node* n = g.node(id);
        if (!n)
            return kNoNode;
        if (n->type().isA(kRootType))
            return id;
        if (!n->type().isA(kMarkerType))
            return kNoNode;
    }
    return kNoNode;
}

void loadTemplates(ext::Context& ctx)
{
    gCtx = &ctx;
    struct Dir
    {
        std::filesystem::path path;
        const char* tier;
    };
    for (const Dir& d : {Dir{ctx.resources() / "templates", "builtin"}, Dir{userDir(), "user"}, Dir{workspaceDir(), "workspace"}})
    {
        std::error_code ec;
        if (d.path.empty() || !std::filesystem::is_directory(d.path, ec))
            continue;
        std::vector<std::filesystem::path> files;
        for (const auto& f : std::filesystem::directory_iterator(d.path, ec))
            if (f.path().extension() == ".lilarig")
                files.push_back(f.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files)
            try
            {
                addTemplate(readFile(f), f, d.tier);
            }
            catch (const std::exception& e)
            {
                ctx.log(LogLevel::Warning, std::string("skipped template ") + e.what());
            }
    }
    poses::setCharacterResolver(resolveCharacter);
    const std::string owner = ctx.owner();
    ctx.track("rigTemplates:library", [owner] {
        poses::setCharacterResolver({});
        poses::unregisterOwned(owner);
        gTemplates.clear();
        notify();
    });
}

void shutdown()
{
    gListeners.clear();
    gCtx = nullptr;
}

bool registerTypes(ext::Context& ctx)
{
    const NodeType* transform = types::find("transform");
    if (!transform)
        return false;
    const uint32_t stored = AttrInput | AttrStorable;
    auto meshList = [] { return rig::makeCenterMeshList(Graph::current(), {}); };

    NodeType root{.name = kRootType,
                  .base = transform,
                  .dag = true,
                  .doc = "A rig template instance: the markers under it place a skeleton of the template's kind.",
                  .icon = "template",
                  .ownAttrs = {
                      {.name = "templateId", .type = PlugType::String, .defaultValue = std::string(), .flags = stored, .doc = "The template it was placed from."},
                      {.name = "templateData", .type = PlugType::String, .defaultValue = std::string(), .flags = stored | AttrHidden,
                       .doc = "The template itself, JSON, so the scene carries it."},
                      {.name = "centerInMesh", .type = PlugType::Bool, .defaultValue = false, .flags = stored | AttrHidden,
                       .doc = "Unused on the root; present so the bound meshes can live in centerMeshes."},
                      {.name = "centerMeshes", .type = PlugType::Data, .defaultValue = meshList(), .flags = stored | AttrHidden,
                       .doc = "Meshes the instance is bound to."},
                  },
                  .create = transform->create};
    lila::scene::relocate::registerNeutral(root.name);
    if (!types::registerType(ctx, std::move(root)))
        return false;

    NodeType marker{.name = kMarkerType,
                    .base = transform,
                    .dag = true,
                    .doc = "A rig template marker: where one joint of the template's skeleton goes. Drag it, snap it to "
                           "components or centre it in the bound meshes, then build or orient a skeleton from the instance.",
                    .icon = "point",
                    .ownAttrs = {
                        {.name = "marker", .type = PlugType::String, .defaultValue = std::string(), .flags = stored, .doc = "Template marker name."},
                        {.name = "label", .type = PlugType::String, .defaultValue = std::string(), .flags = stored, .doc = "Semantic label (hand_l, spine_02 ...)."},
                        {.name = "joint", .type = PlugType::String, .defaultValue = std::string(), .flags = stored, .doc = "Joint the build makes here."},
                        {.name = "mirror", .type = PlugType::String, .defaultValue = std::string(), .flags = stored, .doc = "The opposite side's marker."},
                        {.name = "side", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = stored, .enumNames = {"center", "left", "right"}, .doc = "Side."},
                        {.name = "aimAt", .type = PlugType::String, .defaultValue = std::string(), .flags = stored | AttrHidden, .doc = "Marker the joint aims at."},
                        {.name = "aimAxis", .type = PlugType::String, .defaultValue = std::string("z"), .flags = stored, .doc = "Joint axis aimed at aimAt."},
                        {.name = "upAxis", .type = PlugType::String, .defaultValue = std::string("y"), .flags = stored, .doc = "Joint axis leaned toward the up vector."},
                        {.name = "upVector", .type = PlugType::Vec3, .defaultValue = glm::dvec3(0.0, 1.0, 0.0), .flags = stored | AttrHidden, .doc = "Up reference."},
                        {.name = "radius", .type = PlugType::Float, .defaultValue = 2.0, .flags = stored, .doc = "Drawn size, cm.", .minValue = 0.0},
                        {.name = "centerInMesh", .shortName = "cim", .type = PlugType::Bool, .defaultValue = false, .flags = stored,
                         .doc = "Centre in the bound meshes' cross-section whenever it is moved."},
                        {.name = "centerMeshes", .shortName = "cms", .type = PlugType::Data, .defaultValue = meshList(), .flags = stored | AttrHidden,
                         .doc = "Meshes it centres in."},
                    },
                    .create = transform->create};
    lila::scene::relocate::registerWorldAttrs(marker.name, {.directions = {"upVector"}});
    return types::registerType(ctx, std::move(marker)) != nullptr;
}

bool registerCommands(ext::Context& ctx)
{
    using T = ArgType;
    std::vector<CommandDef> defs;

    defs.push_back({.name = "listRigTemplates",
                    .doc = "The loaded rig templates: id, name, where each came from and what it holds.",
                    .undoable = false,
                    .run = [](const Args&) -> json {
                        json out = json::array();
                        for (const Entry* e : templates())
                            out.push_back(summary(*e));
                        return out;
                    }});

    defs.push_back({.name = "rigTemplateInfo",
                    .doc = "A loaded template's full contents (the .lilarig JSON), or an instance's.",
                    .positionalDoc = "template id or instance",
                    .undoable = false,
                    .run = [](const Args& a) -> json {
                        if (a.positional.size() != 1)
                            throw CommandError("rigTemplateInfo: give one template id or instance");
                        return sourceArg(graph(), a.positional[0].get<std::string>(), "rigTemplateInfo").tmpl.toJson();
                    }});

    defs.push_back({.name = "loadRigTemplate",
                    .doc = "Load a .lilarig file into the template library (replacing a template with the same id). Returns its id.",
                    .positionalDoc = "path",
                    .undoable = false,
                    .run = [](const Args& a) -> json {
                        if (a.positional.size() != 1)
                            throw CommandError("loadRigTemplate: give one path");
                        std::string p = a.positional[0].get<std::string>();
                        std::filesystem::path file(std::u8string(p.begin(), p.end()));
                        try
                        {
                            return addTemplate(readFile(file), file, "file").tmpl.id;
                        }
                        catch (const std::exception& e)
                        {
                            throw CommandError(std::string("loadRigTemplate: ") + e.what());
                        }
                    }});

    defs.push_back({.name = "saveRigTemplate",
                    .doc = "Write a template to a .lilarig file. Without a path it goes to the user's template folder "
                           "(or the workspace's with workspace=True). Returns the path.",
                    .positionalDoc = "template id or instance, [path]",
                    .flags = {{"workspace", "ws", T::Bool, false, "Save into <workspace>/.lila/rigTemplates."}},
                    .undoable = false,
                    .run = [](const Args& a) -> json {
                        if (a.positional.empty() || a.positional.size() > 2)
                            throw CommandError("saveRigTemplate: give a template id or instance, and optionally a path");
                        Template t = sourceArg(graph(), a.positional[0].get<std::string>(), "saveRigTemplate").tmpl;
                        std::filesystem::path file;
                        if (a.positional.size() == 2)
                        {
                            std::string p = a.positional[1].get<std::string>();
                            file = std::filesystem::path(std::u8string(p.begin(), p.end()));
                        }
                        else
                        {
                            std::filesystem::path dir = a.get<bool>("workspace") ? workspaceDir() : userDir();
                            if (dir.empty())
                                throw CommandError("saveRigTemplate: no workspace is open");
                            file = dir / (t.id + ".lilarig");
                        }
                        try
                        {
                            writeFile(t, file);
                        }
                        catch (const std::exception& e)
                        {
                            throw CommandError(std::string("saveRigTemplate: ") + e.what());
                        }
                        if (const Entry* e = findTemplate(t.id); e && e->file.empty())
                            addTemplate(t, file, a.get<bool>("workspace") ? "workspace" : "user");
                        return file.generic_string();
                    }});

    defs.push_back({.name = "placeRigTemplate",
                    .doc = "Place a template's markers in the scene under a new instance root, at the template's "
                           "positions. With meshes, the instance is bound to them so its markers centre in them. Returns the root.",
                    .positionalDoc = "template id",
                    .flags = {{"name", "n", T::String, "", "Name for the instance root."},
                              nodeFlag({"meshes", "m", T::List, nullptr, "Meshes to bind to."}, "mesh")},
                    .run = [](const Args& a) -> json {
                        if (a.positional.size() != 1)
                            throw CommandError("placeRigTemplate: give one template id");
                        const Entry* e = findTemplate(a.positional[0].get<std::string>());
                        if (!e)
                            throw CommandError("placeRigTemplate: no template '" + a.positional[0].get<std::string>() + "'");
                        Graph& g = graph();
                        std::vector<NodeId> meshes = a.has("meshes") ? meshesArg(g, a.flags["meshes"], "placeRigTemplate") : std::vector<NodeId>{};
                        return placeInstance(g, e->tmpl, a.get<std::string>("name"), meshes);
                    }});

    defs.push_back({.name = "bindRigTemplate",
                    .doc = "Bind a template instance to meshes: its markers centre in them. Replaces any earlier binding.",
                    .positionalDoc = "instance, meshes...",
                    .run = [](const Args& a) -> json {
                        Graph& g = graph();
                        std::vector<std::string> objs = a.objects();
                        if (objs.size() < 2)
                            throw CommandError("bindRigTemplate: give the instance (or a marker) and at least one mesh");
                        NodeId root = instanceArg(g, objs[0], "bindRigTemplate");
                        json rest = json::array();
                        for (size_t i = 1; i < objs.size(); ++i)
                            rest.push_back(objs[i]);
                        std::vector<NodeId> meshes = meshesArg(g, rest, "bindRigTemplate");
                        UndoChunk chunk("Bind Rig Template");
                        setMeshes(g, root, meshes);
                        for (auto& [name, id] : markersOf(g, root))
                            setMeshes(g, id, meshes);
                        return rest;
                    }});

    defs.push_back({.name = "unbindRigTemplate",
                    .doc = "Unbind a template instance from its meshes; the markers stay where they are.",
                    .positionalDoc = "instance",
                    .run = [](const Args& a) -> json {
                        Graph& g = graph();
                        std::vector<std::string> objs = a.objects();
                        if (objs.empty())
                            throw CommandError("unbindRigTemplate: give an instance");
                        NodeId root = instanceArg(g, objs[0], "unbindRigTemplate");
                        UndoChunk chunk("Unbind Rig Template");
                        setMeshes(g, root, {});
                        for (auto& [name, id] : markersOf(g, root))
                            setMeshes(g, id, {});
                        return g.node(root)->path();
                    }});

    defs.push_back({.name = "centerRigMarkers",
                    .doc = "Centre markers in the bound meshes' cross-sections across their limb, parents first. Takes an "
                           "instance (every marker with centerInMesh on) or markers (each given one). Returns the markers moved.",
                    .positionalDoc = "instance or markers",
                    .run = [](const Args& a) -> json {
                        Graph& g = graph();
                        std::vector<std::string> objs = a.objects();
                        if (objs.empty())
                            throw CommandError("centerRigMarkers: give an instance or markers");
                        std::vector<std::string> names;
                        for (const std::string& o : objs)
                        {
                            Node& n = need(g, o, "centerRigMarkers");
                            if (n.type().isA(kRootType))
                            {
                                Template storage;
                                const Template& t = instanceTemplate(g, n.id(), storage);
                                std::map<std::string, NodeId> mk = markersOf(g, n.id());
                                for (const Marker& m : t.markers)
                                    if (auto it = mk.find(m.name); it != mk.end() && rig::centerEnabled(*g.node(it->second)))
                                        names.push_back(g.node(it->second)->path());
                            }
                            else if (n.type().isA(kMarkerType))
                                names.push_back(n.path());
                            else
                                throw CommandError("centerRigMarkers: " + o + " is not a marker or instance");
                        }
                        UndoChunk chunk("Centre Markers");
                        json moved = json::array();
                        for (const std::string& n : names)
                            for (const json& m : ops::execute("centerJoint", {{"args", {n}}, {"keepChildren", true}}))
                                moved.push_back(m);
                        return moved;
                    }});

    defs.push_back({.name = "mirrorRigMarkers",
                    .doc = "Copy one side's marker positions onto their mirror partners, reflected across the instance's "
                           "centre plane (across the template's lateral axis). Returns the markers moved.",
                    .positionalDoc = "instance",
                    .flags = {{"from", "f", T::String, "left", "The side copied: left or right.", {"left", "right"}}},
                    .run = [](const Args& a) -> json {
                        Graph& g = graph();
                        std::vector<std::string> objs = a.objects();
                        if (objs.empty())
                            throw CommandError("mirrorRigMarkers: give an instance");
                        NodeId root = instanceArg(g, objs[0], "mirrorRigMarkers");
                        Template storage;
                        const Template& t = instanceTemplate(g, root, storage);
                        const std::string from = a.get<std::string>("from", "left");
                        std::map<std::string, NodeId> mk = markersOf(g, root);
                        const glm::dmat4 rootWorld = worldOf(g, root);
                        const glm::dmat4 rootInv = glm::inverse(rootWorld);
                        const glm::dvec3 lat = glm::normalize(t.lateral);
                        UndoChunk chunk("Mirror Markers");
                        json moved = json::array();
                        for (const Marker& m : t.markers)
                        {
                            if (m.side != from || m.mirror.empty() || !mk.contains(m.name) || !mk.contains(m.mirror))
                                continue;
                            glm::dvec3 p(rootInv * glm::dvec4(glm::dvec3(worldOf(g, mk[m.name])[3]), 1.0));
                            p -= 2.0 * lat * glm::dot(p, lat);
                            glm::dvec3 w(rootWorld * glm::dvec4(p, 1.0));
                            NodeId other = mk[m.mirror];
                            glm::dmat4 parentInv = glm::inverse(worldOf(g, g.parent(other)));
                            // Children keep their place: they are mirrored on their own turn.
                            std::vector<std::pair<NodeId, glm::dvec3>> kids;
                            for (NodeId c : g.children(other))
                                kids.push_back({c, glm::dvec3(worldOf(g, c)[3])});
                            g.setValue({other, Transform::kTranslate, -1}, glm::dvec3(parentInv * glm::dvec4(w, 1.0)));
                            for (auto& [c, cw] : kids)
                                g.setValue({c, Transform::kTranslate, -1}, glm::dvec3(glm::inverse(worldOf(g, other)) * glm::dvec4(cw, 1.0)));
                            moved.push_back(g.node(other)->path());
                        }
                        return moved;
                    }});

    defs.push_back({.name = "buildRigSkeleton",
                    .doc = "Build joints from a template instance's markers (or a template's defaults): template names, "
                           "hierarchy and orientations from the template's rules, plus (rig) the template's IK limits, IK chains, "
                           "reverse feet, lookers, twist drivers, deform flags and any rubber hose limbs (hose), so the skeleton is "
                           "a working rig in one step. bindSkin "
                           "binds the instance's meshes (or the given ones). Returns the root joint, the joints, any "
                           "skinClusters and the rig elements.",
                    .positionalDoc = "instance or template id",
                    .flags = {{"bindSkin", "bs", T::Bool, false, "Bind the meshes to the new skeleton."},
                              nodeFlag({"meshes", "m", T::List, nullptr, "Meshes to bind instead of the instance's."}, "mesh"),
                              {"includeIk", "ik", T::Bool, true, "Build the template's ik helper joints too."},
                              {"prefix", "p", T::String, "", "Prefix for the joint names."},
                              {"spaces", "sp", T::Bool, true, "Give the joints the template's default spaces (hands in world, chest, head ...)."},
                              {"rig", "rg", T::Bool, true, "Build the template's rig elements too: IK limits, IK chains, reverse feet, lookers, twist drivers, hose limbs and deform flags."},
                              {"hose", "hs", T::String, "template",
                               "Limbs built as rubber hoses (with rig): template (each limb's own setting), all, none (all jointed), or IK chain names separated by commas (arm_l, leg_r)."}},
                    .run = [](const Args& a) -> json {
                        Graph& g = graph();
                        std::vector<std::string> objs = a.objects();
                        if (objs.size() != 1)
                            throw CommandError("buildRigSkeleton: give one instance or template id");
                        Source src = sourceArg(g, objs[0], "buildRigSkeleton");
                        std::vector<NodeId> meshes = a.has("meshes") ? meshesArg(g, a.flags["meshes"], "buildRigSkeleton") : src.meshes;
                        const std::string hose = a.get<std::string>("hose", "template");
                        if (hose != "template" && hose != "all" && hose != "none")
                            for (const std::string& n : splitNames(hose))
                            {
                                bool known = false;
                                for (const json& c : src.tmpl.rig.value("ikChains", json::array()))
                                    known = known || c.value("name", std::string()) == n;
                                if (!known)
                                    throw CommandError("buildRigSkeleton: hose: the template has no IK chain '" + n + "'");
                            }
                        return buildSkeleton(g, src, a.get<std::string>("prefix"), a.get<bool>("includeIk", true), a.get<bool>("bindSkin"), meshes,
                                             a.get<bool>("spaces", true), a.get<bool>("rig", true), hose);
                    }});

    defs.push_back({.name = "orientRigSkeleton",
                    .doc = "Orient an existing skeleton's joints to a template's rules without moving them: joints are "
                           "matched by name and alias. A skinned or animated skeleton changes through editSkeleton, so "
                           "its meshes, animation and poses are kept. limbs limits it to some of the template's limbs. Without "
                           "template, the best matching one is used. Returns the joints changed.",
                    .positionalDoc = "a joint of the skeleton",
                    .flags = {{"template", "t", T::String, nullptr, "Template id."},
                              {"limbs", "l", T::List, nullptr, "Limb names (spine, arm_l, leg_r, index_l ...); every limb when omitted."},
                              {"preserve", "pr", T::String, nullptr,
                               "rest: the joints take the template's axes with rotate zeroed (skeletons with no skin "
                               "or animation). worldPose: every pose keeps its world placement and rotate takes up the "
                               "change, so the bind pose has the template's axes. channels: keys keep their values. "
                               "Default: rest for a plain skeleton, worldPose otherwise.",
                               {"rest", "worldPose", "channels"}}},
                    .run = [](const Args& a) -> json {
                        Graph& g = graph();
                        std::vector<std::string> objs = a.objects();
                        if (objs.empty())
                            throw CommandError("orientRigSkeleton: give a joint of the skeleton");
                        Node& j = need(g, objs[0], "orientRigSkeleton");
                        if (!rig::isJoint(j))
                            throw CommandError("orientRigSkeleton: " + j.path() + " is not a joint");
                        const Entry* e = nullptr;
                        if (a.has("template"))
                        {
                            e = findTemplate(a.get<std::string>("template"));
                            if (!e)
                                throw CommandError("orientRigSkeleton: no template '" + a.get<std::string>("template") + "'");
                        }
                        else
                        {
                            e = bestTemplate(g, skeletonOf(g, topJoint(g, j.id()))).first;
                            if (!e)
                                throw CommandError("orientRigSkeleton: no loaded template matches " + j.path() + "'s joint names; give template");
                        }
                        std::vector<std::string> limbs = a.has("limbs") ? stringList(a.flags["limbs"]) : std::vector<std::string>{};
                        return orientSkeleton(g, e->tmpl, j.id(), limbs, a.get<std::string>("preserve", ""));
                    }});

    defs.push_back({.name = "rigFromTemplate",
                    .doc = "Give an existing skeleton its template's rig elements, matching joints by name and alias: IK limits, "
                           "IK chains with poles, reverse feet on the skinned sole, lookers, twist drivers and deform flags, grouped "
                           "under <root>_rig. A skeleton that already has IK chains is left alone. Without template, the best matching "
                           "one is used. Returns the elements made.",
                    .positionalDoc = "a joint of the skeleton",
                    .flags = {{"template", "t", T::String, nullptr, "Template id."}},
                    .run = [](const Args& a) -> json {
                        Graph& g = graph();
                        std::vector<std::string> objs = a.objects();
                        if (objs.empty())
                            throw CommandError("rigFromTemplate: give a joint of the skeleton");
                        Node& j = need(g, objs[0], "rigFromTemplate");
                        if (!rig::isJoint(j))
                            throw CommandError("rigFromTemplate: " + j.path() + " is not a joint");
                        const NodeId top = topJoint(g, j.id());
                        const std::vector<NodeId> joints = skeletonOf(g, top);
                        const Entry* e = a.has("template") ? findTemplate(a.get<std::string>("template")) : bestTemplate(g, joints).first;
                        if (!e)
                            throw CommandError(a.has("template") ? "rigFromTemplate: no template '" + a.get<std::string>("template") + "'"
                                                                 : "rigFromTemplate: no loaded template matches " + j.path() + "'s joint names; give template");
                        // A skeleton that already has IK chains is rigged; this only fills in a bare one.
                        for (NodeId id : joints)
                            if (lila::scene::rigel::chainContaining(g, id))
                                return json::array();
                        const std::map<std::string, NodeId> mapped = mapJoints(g, e->tmpl, joints);
                        if (mapped.empty())
                            return json::array();
                        return buildTemplateRig(g, e->tmpl, Solved{}, mapped, g.node(top)->path(), true, "none");
                    }});

    defs.push_back({.name = "templateFromSkeleton",
                    .doc = "Make a rig template from a skeleton: a marker per joint with its position, recorded "
                           "orientation, semantic label, side and mirror partner, orientation rules that reproduce it, "
                           "limb groups, a picker layout, bind/T/A poses and naming aliases. Helper joints (twist, "
                           "corrective) and ik joints follow the main ones. Adds it to the library and, with file, "
                           "saves it. Returns its summary.",
                    .positionalDoc = "a joint of the skeleton",
                    .flags = {{"id", "", T::String, nullptr, "Template id; the top joint's name by default."},
                              {"name", "n", T::String, "", "Display name."},
                              {"description", "d", T::String, "", "Description."},
                              {"file", "f", T::String, nullptr, "Save it here as .lilarig."},
                              {"includeIk", "ik", T::Bool, true, "Keep ik_ joints (flagged ik)."},
                              nodeFlag({"free", "", T::List, nullptr, "Joints to keep as placeable markers even if they look like helpers."}, "joint"),
                              {"source", "", T::Any, nullptr, "Provenance to record in the template (an object)."}},
                    .undoable = false,
                    .run = [](const Args& a) -> json {
                        Graph& g = graph();
                        std::vector<std::string> objs = a.objects();
                        if (objs.empty())
                            throw CommandError("templateFromSkeleton: give a joint of the skeleton");
                        Node& j = need(g, objs[0], "templateFromSkeleton");
                        if (!rig::isJoint(j))
                            throw CommandError("templateFromSkeleton: " + j.path() + " is not a joint");
                        const NodeId top = topJoint(g, j.id());
                        FromSkeletonOptions o;
                        o.id = a.has("id") ? a.get<std::string>("id") : bare(g.node(top)->name());
                        o.name = a.get<std::string>("name");
                        o.includeIk = a.get<bool>("includeIk", true);
                        if (a.has("free"))
                            for (const std::string& f : stringList(a.flags["free"]))
                                o.free.insert(f);
                        Template t;
                        try
                        {
                            t = fromSkeleton(sourceJoints(g, top), o);
                        }
                        catch (const std::exception& e)
                        {
                            throw CommandError(std::string("templateFromSkeleton: ") + e.what());
                        }
                        t.description = a.get<std::string>("description");
                        t.source = {{"skeleton", bare(g.node(top)->name())}, {"joints", t.markers.size()}};
                        if (a.has("source") && a.flags["source"].is_object())
                            t.source.update(a.flags["source"]);
                        t.picker = pickerLayout(t);
                        t.poses = defaultPoses(t);
                        std::filesystem::path file;
                        if (a.has("file"))
                        {
                            std::string p = a.get<std::string>("file");
                            file = std::filesystem::path(std::u8string(p.begin(), p.end()));
                            try
                            {
                                writeFile(t, file);
                            }
                            catch (const std::exception& e)
                            {
                                throw CommandError(std::string("templateFromSkeleton: ") + e.what());
                            }
                        }
                        const bool saved = !file.empty();
                        return summary(addTemplate(std::move(t), file, saved ? "file" : "session"));
                    }});

    defs.push_back({.name = "rigTemplatePicker",
                    .doc = "Make a Control Picker document from a template's picker layout and store it in the scene "
                           "(undoable), named after the template unless name is given. Returns the document's name.",
                    .positionalDoc = "template id or instance",
                    .flags = {{"name", "n", T::String, "", "Document name."},
                              {"namespace", "ns", T::String, "", "Default namespace for its targets."}},
                    .run = [](const Args& a) -> json {
                        if (a.positional.size() != 1)
                            throw CommandError("rigTemplatePicker: give one template id or instance");
                        Template t = sourceArg(graph(), a.positional[0].get<std::string>(), "rigTemplatePicker").tmpl;
                        if (!t.picker.is_object() || t.picker.empty())
                            throw CommandError("rigTemplatePicker: template " + t.id + " has no picker layout");
                        picker::Document doc = picker::generateFromLayout(t.picker);
                        doc.templateId = t.id;
                        if (!a.get<std::string>("name").empty())
                            doc.name = a.get<std::string>("name");
                        if (doc.name.empty())
                            doc.name = t.id;
                        doc.nameSpace = a.get<std::string>("namespace");
                        picker::store(doc);
                        return doc.name;
                    }});

    defs.push_back({.name = "installRigTemplatePoses",
                    .doc = "Copy a template's poses into a pose library tier (scene, workspace or user) for its "
                           "character, so they can be edited there; the template tier always has them. Returns the pose names.",
                    .positionalDoc = "template id",
                    .flags = {{"tier", "", T::String, "scene", "scene, workspace or user.", {"scene", "workspace", "user"}},
                              {"character", "c", T::String, "", "Library group; the template id by default."}},
                    .run = [](const Args& a) -> json {
                        if (a.positional.size() != 1)
                            throw CommandError("installRigTemplatePoses: give one template id");
                        const Entry* e = findTemplate(a.positional[0].get<std::string>());
                        if (!e)
                            throw CommandError("installRigTemplatePoses: no template '" + a.positional[0].get<std::string>() + "'");
                        std::optional<poses::Tier> tier = poses::tierFromName(a.get<std::string>("tier", "scene"));
                        if (!tier || *tier == poses::Tier::Template)
                            throw CommandError("installRigTemplatePoses: tier is scene, workspace or user");
                        std::string character = a.get<std::string>("character");
                        json names = json::array();
                        for (auto& [name, pj] : e->tmpl.poses.items())
                        {
                            json j = pj;
                            j["name"] = j.value("name", name);
                            poses::Pose p = poses::Pose::fromJson(j);
                            p.character = character.empty() ? e->tmpl.id : character;
                            p.templateId = e->tmpl.id;
                            try
                            {
                                poses::save(p, *tier);
                            }
                            catch (const std::exception& ex)
                            {
                                throw CommandError(std::string("installRigTemplatePoses: ") + ex.what());
                            }
                            names.push_back(p.name);
                        }
                        return names;
                    }});

    for (CommandDef& d : defs)
        if (!ctx.registerCommand(std::move(d)))
            return false;
    return true;
}
} // namespace rigt

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!rigt::registerTypes(ctx))
        return 1;
    if (!rigt::registerCommands(ctx))
        return 2;
    rigt::loadTemplates(ctx);
    rigt::registerUi(ctx);
    return 0;
}

LILA_EXTENSION_EXPORT void lilaExtensionUninit(lila::ext::Context&) { rigt::shutdown(); }
