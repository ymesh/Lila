// Full-body posing: dragging a joint poses the whole character around what holds still, its pinned joints and its
// locked hands and feet.

#include "Puppeteer.h"
#include "rig/Rig.h"

#include <lila/FullBodyIk.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>

using namespace lila;
using namespace lila::scene;

namespace puppeteer
{
NodeId characterRoot(Graph& g, NodeId character)
{
    Node* r = character ? named(g, textOf(*g.node(character), "root")) : nullptr;
    return r ? r->id() : kNoNode;
}

namespace
{
NodeId rootOf(Graph& g, NodeId character) { return characterRoot(g, character); }

void setPins(Graph& g, NodeId character, const std::vector<NodeId>& pins)
{
    std::string text;
    for (NodeId p : pins)
        text += (text.empty() ? "" : " ") + g.node(p)->path();
    g.setValue(plugOf(*g.node(character), "pins"), text);
}

/** Without a template saying otherwise, by joint name for skeletons named like UE's: the spine and shoulders resist. */
double stiffnessByName(std::string name)
{
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (name.find("clavicle") != std::string::npos)
        return 0.7;
    if (name.find("neck") != std::string::npos)
        return 0.5;
    if (name.find("spine") != std::string::npos)
        return 0.3;
    return 0.0;
}

/**
 * Whether the solve can move the joint: anything below the root, and the root itself where it is also the hips (it
 * branches), since the solve moves the hips. A lone root above the hips, like UE's, is moved the ordinary way.
 */
bool solvable(Graph& g, NodeId joint, NodeId root)
{
    if (joint != root)
        return true;
    int branches = 0;
    for (NodeId c : g.children(root))
        if (Node* n = g.node(c); n && lila::scene::rig::isJoint(*n) && lila::scene::rigel::isDeform(*n))
            ++branches;
    return branches >= 2;
}

glm::dquat worldRotation(const glm::dmat4& m) { return glm::normalize(glm::quat_cast(math::rotationOf(m))); }
} // namespace

std::vector<std::pair<NodeId, double>> stiffnessOf(Graph& g, NodeId character, const std::vector<NodeId>& joints)
{
    std::map<std::string, double> byJoint;
    bool fromTemplate = false;
    if (const std::string& id = textOf(*g.node(character), "templateId"); !id.empty() && ops::find("rigTemplateInfo"))
        try
        {
            const json t = ops::execute("rigTemplateInfo", {{"args", {id}}});
            const json stiffness = t.value("rig", json::object()).value("fullBody", json::object()).value("stiffness", json::object());
            for (const json& m : t.value("markers", json::array()))
                if (const std::string name = m.value("name", std::string()); stiffness.contains(name))
                    byJoint[m.value("joint", name)] = stiffness[name].get<double>();
            fromTemplate = !stiffness.empty();
        }
        catch (const std::exception&)
        {
        }
    std::vector<std::pair<NodeId, double>> out;
    for (NodeId j : joints)
    {
        std::string name = g.node(j)->name();
        name = name.substr(name.rfind(':') + 1);
        const auto it = byJoint.find(name);
        if (const double s = fromTemplate ? (it == byJoint.end() ? 0.0 : it->second) : stiffnessByName(name); s > 0.0)
            out.push_back({j, s});
    }
    return out;
}

std::vector<NodeId> pinsOf(Graph& g, NodeId character)
{
    std::vector<NodeId> out;
    std::istringstream in(textOf(*g.node(character), "pins"));
    for (std::string w; in >> w;)
        if (Node* n = named(g, w); n && lila::scene::rig::isJoint(*n))
            out.push_back(n->id());
    return out;
}

std::vector<NodeId> fullBodyJoints(Graph& g, NodeId joint)
{
    std::vector<NodeId> out;
    const NodeId root = rootOf(g, characterFor(g, joint));
    if (!root)
        return out;
    out.push_back(root);
    for (NodeId d : g.descendants(root))
        if (Node* n = g.node(d); n && lila::scene::rig::isJoint(*n))
            out.push_back(d);
    return out;
}

std::vector<NodeId> fullBodyMove(Graph& g, const std::vector<NodeId>& nodes, const glm::dvec3& by, bool autoPose)
{
    g.evaluate();
    std::map<NodeId, std::vector<NodeId>> moving;
    for (NodeId n : nodes)
        if (Node* j = g.node(n); j && lila::scene::rig::isJoint(*j) && lila::scene::rigel::isDeform(*j))
            if (NodeId c = characterFor(g, n); c && rootOf(g, c) && solvable(g, n, rootOf(g, c)))
                moving[c].push_back(n);

    std::vector<NodeId> handled;
    for (const auto& [character, joints] : moving)
    {
        const NodeId root = rootOf(g, character);
        fbik::PoseOptions opts;
        opts.autoPose = autoPose;
        for (NodeId j : joints)
            opts.pins.push_back({.joint = j, .position = glm::dvec3(worldOf(g, j)[3]) + by});
        auto hold = [&](NodeId j) {
            Node* n = g.node(j);
            if (!n || !lila::scene::rigel::isDeform(*n) || !isUnder(g, j, root) || std::find(joints.begin(), joints.end(), j) != joints.end())
                return;
            const glm::dmat4 w = worldOf(g, j);
            opts.pins.push_back({.joint = j, .position = glm::dvec3(w[3]), .rotation = worldRotation(w)});
        };
        for (NodeId p : pinsOf(g, character))
            hold(p);
        for (NodeId chain : lila::scene::rigel::chains(g))
            if (chainLocked(g, chain, g.time()))
                if (const auto js = lila::scene::rigel::chainJoints(g, chain); !js.empty())
                    hold(js.back());
        opts.stiffness = stiffnessOf(g, character, fullBodyJoints(g, joints.front()));
        try
        {
            fbik::pose(g, root, opts);
            handled.insert(handled.end(), joints.begin(), joints.end());
        }
        catch (const std::invalid_argument& e)
        {
            log::write(LogLevel::Warning, std::string("puppeteerMove: ") + e.what());
        }
    }
    return handled;
}

void registerFullBody(ext::Context& ctx)
{
    using T = ArgType;
    ctx.registerCommand({
        .name = "puppeteerPin",
        .doc = "Pin joints so full-body moves hold them where they are, or unpin them. With neither pin nor unpin given, "
               "toggles each. Returns the character's pinned joints.",
        .positionalDoc = "joints (default the selection)",
        .flags = {
            {"pin", "p", T::Bool, nullptr, "True pins, False unpins; leave out to toggle."},
            {"clear", "c", T::Bool, false, "Unpin every joint of the joints' characters first."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> joints;
            for (const std::string& s : objectArgs(a))
                joints.push_back(jointNamed(g, s, "puppeteerPin").id());
            std::map<NodeId, std::vector<NodeId>> byCharacter;
            for (NodeId j : joints)
            {
                const NodeId c = characterFor(g, j);
                if (!c)
                    throw CommandError("puppeteerPin: " + g.node(j)->path() + " is not in a character");
                byCharacter[c].push_back(j);
            }
            if (a.get<bool>("clear", false))
                for (NodeId c : characters(g))
                    if (byCharacter.count(c) || joints.empty())
                        byCharacter[c];
            json out = json::array();
            for (auto& [c, js] : byCharacter)
            {
                std::vector<NodeId> pins = a.get<bool>("clear", false) ? std::vector<NodeId>{} : pinsOf(g, c);
                for (NodeId j : js)
                {
                    const auto it = std::find(pins.begin(), pins.end(), j);
                    const bool on = a.has("pin") ? a.get<bool>("pin") : it == pins.end();
                    if (on && it == pins.end())
                        pins.push_back(j);
                    else if (!on && it != pins.end())
                        pins.erase(it);
                }
                setPins(g, c, pins);
                for (NodeId p : pins)
                    out.push_back(g.node(p)->path());
            }
            return out;
        },
    });
}
} // namespace puppeteer
