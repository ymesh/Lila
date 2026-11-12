// The commands every Stagehand tool is a front end for, so agents and scripts do in one call what a hand does with a
// drag: settle, push, wiggle, drop, shake; physicsBody sets a mesh's physics; dressInfo and dressCheck report on it.

#include "Session.h"
#include "Stagehand.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/Transform.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <random>
#include <set>

using namespace lila;

namespace stagehand
{
using T = ArgType;

namespace
{
glm::dvec3 vecArg(const Args& a, const char* flag, const glm::dvec3& fallback, const char* cmd)
{
    if (!a.has(flag))
        return fallback;
    const json& j = a.flags[flag];
    if (!j.is_array() || j.size() != 3)
        throw CommandError(std::string(cmd) + ": " + flag + " takes [x, y, z]");
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

NodeId meshNamed(Graph& g, const json& name, const char* cmd)
{
    if (!name.is_string())
        throw CommandError(std::string(cmd) + ": " + name.dump() + " is not a node name");
    Node* n = g.find(name.get<std::string>());
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named " + name.dump());
    if (!n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + n->path() + " is not a mesh");
    return n->id();
}

std::vector<NodeId> meshesNamed(Graph& g, const std::vector<std::string>& names, const char* cmd)
{
    std::vector<NodeId> out;
    for (const std::string& n : names)
        out.push_back(meshNamed(g, n, cmd));
    return out;
}

std::vector<NodeId> listArg(Graph& g, const Args& a, const char* flag, const char* cmd)
{
    std::vector<NodeId> out;
    if (!a.has(flag))
        return out;
    const json& j = a.flags[flag];
    if (j.is_string())
        return {meshNamed(g, j, cmd)};
    for (const json& n : j)
        out.push_back(meshNamed(g, n, cmd));
    return out;
}

std::vector<FlagDef> worldFlags()
{
    return {
        nodeFlag({"movable", "mv", T::List, nullptr, "Meshes that may move as well (default: those touching the targets, chained, smaller than the biggest of them x8)."}, "mesh"),
        nodeFlag({"colliders", "co", T::List, nullptr, "Meshes things hit but that never move (default: every visible mesh nearby)."}, "mesh"),
        {"ground", "gr", T::Bool, true, "A floor plane at groundHeight, under everything."},
        {"groundHeight", "gh", T::Float, 0.0, "Height of the floor plane, cm."},
        {"gravity", "g", T::List, json::array({0.0, -981.0, 0.0}), "Gravity, cm/s2."},
        {"time", "ti", T::Float, 120.0, "Longest it simulates, seconds: it runs until everything is at rest, and the report says capped when "
                                        "this stopped it first.", {}, {}, 0.0},
        {"reach", "re", T::Float, 300.0, "Without colliders=, meshes within this many cm of a mover join as colliders (more below it, the way "
                                         "it falls).", {}, {}, 0.0},
    };
}

bool boundsOf(Graph& g, NodeId id, glm::dvec3& lo, glm::dvec3& hi) { return worldBoundsOf(g, id, lo, hi); }

double volumeOf(const glm::dvec3& lo, const glm::dvec3& hi)
{
    const glm::dvec3 s = glm::max(hi - lo, glm::dvec3(0.1));
    return s.x * s.y * s.z;
}

WorldOptions optionsOf(Graph& g, const Args& a, std::vector<NodeId> movers, const char* cmd, bool chain)
{
    WorldOptions o;
    if (a.has("movable"))
        for (NodeId id : listArg(g, a, "movable", cmd))
            movers.push_back(id);
    else if (chain)
    {
        const std::set<NodeId> none;
        for (NodeId id : neighbours(g, movers, 2.0, 8.0, none))
            movers.push_back(id);
    }
    o.movers = std::move(movers);
    if (a.has("colliders"))
    {
        o.colliders = listArg(g, a, "colliders", cmd);
        o.collidersGiven = true;
    }
    o.ground = a.get<bool>("ground", true);
    o.groundHeight = a.get<double>("groundHeight", 0.0);
    o.gravity = vecArg(a, "gravity", o.gravity, cmd);
    o.reach = a.get<double>("reach", o.reach);
    return o;
}

double angleBetween(const glm::dmat4& a, const glm::dmat4& b)
{
    const glm::dquat qa = frameOf(a).rotation, qb = frameOf(b).rotation;
    return glm::degrees(2.0 * std::acos(std::clamp(std::abs(glm::dot(qa, qb)), 0.0, 1.0)));
}

/** What resting on what looks like: contacts whose normal points up into the mover from the thing under it. */
json supportsOf(const World& w, int body, const std::vector<physics::Contact>& contacts, Graph& g, const glm::dvec3& up)
{
    json out = json::array();
    for (const physics::Contact& c : contacts)
    {
        if (!c.resting || (c.a != body && c.b != body))
            continue;
        const int other = c.a == body ? c.b : c.a;
        // The normal points from a to b; seen from the mover, the support pushes along the normal toward it.
        const glm::dvec3 push = c.a == body ? -c.normal : c.normal;
        if (glm::dot(push, up) < 0.3)
            continue;
        if (other < 0)
            out.push_back("ground");
        else if (Node* n = g.node(w.node(other)))
            out.push_back(n->path());
    }
    return out;
}

/** The report every physics command returns: per mover where it went, whether it rests, what holds it up. */
json reportOf(Graph& g, const World& w, double seconds, const glm::dvec3& gravity, bool capped = false)
{
    const glm::dvec3 up = glm::length(gravity) > 1e-9 ? -glm::normalize(gravity) : glm::dvec3(0, 1, 0);
    const std::vector<physics::Contact> contacts = w.scene().contacts();
    json movers = json::array();
    bool rest = true;
    double deepest = 0.0;
    for (size_t i = 0; i < w.movers(); ++i)
    {
        const glm::dmat4 now = w.worldMatrix(int(i)), start = w.startMatrix(int(i));
        const double pen = w.scene().penetration(int(i));
        deepest = std::max(deepest, pen);
        // Asleep, or as good as: a settle stops once everything has been this still for a while.
        const bool asleep = w.scene().asleep(int(i)) || glm::length(w.scene().velocity(int(i))) < 1.0;
        rest = rest && asleep;
        Node* n = g.node(w.node(int(i)));
        movers.push_back({{"node", n ? n->path() : std::string()},
                          {"moved", glm::length(glm::dvec3(now[3]) - glm::dvec3(start[3]))},
                          {"turned", angleBetween(now, start)},
                          {"position", v3(glm::dvec3(now[3]))},
                          {"atRest", asleep},
                          {"penetration", pen},
                          {"restsOn", supportsOf(w, int(i), contacts, g, up)}});
    }
    return {{"movers", movers}, {"seconds", seconds}, {"steps", w.scene().steps()}, {"atRest", rest}, {"capped", capped},
            {"deepestPenetration", deepest}, {"colliders", int64_t(w.scene().size() - w.movers())}};
}

/** A point on the target's side away from the direction, high up: where a hand pushes to topple it. */
glm::dvec3 pushPoint(const glm::dvec3& lo, const glm::dvec3& hi, const glm::dvec3& dir, const glm::dvec3& up)
{
    const glm::dvec3 c = (lo + hi) * 0.5;
    const glm::dvec3 flat = dir - up * glm::dot(dir, up);
    const glm::dvec3 half = (hi - lo) * 0.5;
    const glm::dvec3 back = glm::length(flat) > 1e-9 ? -glm::normalize(flat) : glm::dvec3(0.0);
    return c + back * glm::dot(glm::abs(back), half) + up * glm::dot(glm::abs(up), half) * 0.6;
}

int64_t choiceIndex(const std::string& v, std::initializer_list<const char*> names, const char* what)
{
    int64_t i = 0;
    for (const char* n : names)
    {
        if (v == n)
            return i;
        ++i;
    }
    throw CommandError(std::string("physicsBody: unknown ") + what + " '" + v + "'");
}
} // namespace

std::vector<NodeId> neighbours(Graph& g, const std::vector<NodeId>& seeds, double margin, double sizeRatio, const std::set<NodeId>& exclude)
{
    LILA_ZONE();

    struct Box
    {
        NodeId id;
        glm::dvec3 lo, hi;
    };
    std::vector<Box> all;
    for (NodeId id : g.nodes())
    {
        if (!physical(g, id) || exclude.count(id))
            continue;
        const RoleChoice r = bodySettings(g, id).role;
        if (bodySettings(g, id).hasNode && (r == RoleChoice::Static || r == RoleChoice::Off))
            continue;
        Box b{id};
        if (boundsOf(g, id, b.lo, b.hi))
            all.push_back(b);
    }
    double biggest = 0.0;
    std::deque<Box> queue;
    std::set<NodeId> taken(seeds.begin(), seeds.end());
    for (NodeId s : seeds)
    {
        Box b{s};
        if (!boundsOf(g, s, b.lo, b.hi))
            continue;
        biggest = std::max(biggest, volumeOf(b.lo, b.hi));
        queue.push_back(b);
    }
    std::vector<NodeId> out;
    while (!queue.empty())
    {
        const Box b = queue.front();
        queue.pop_front();
        for (const Box& o : all)
        {
            if (taken.count(o.id) || volumeOf(o.lo, o.hi) > biggest * sizeRatio)
                continue;
            // What it stands on (top at or under its bottom, footprint wider) holds things up rather than getting knocked.
            const bool under = o.hi.y <= b.lo.y + margin && o.hi.x - o.lo.x >= b.hi.x - b.lo.x && o.hi.z - o.lo.z >= b.hi.z - b.lo.z;
            if (under)
                continue;
            const glm::dvec3 lo = b.lo - glm::dvec3(margin), hi = b.hi + glm::dvec3(margin);
            if (o.lo.x > hi.x || o.hi.x < lo.x || o.lo.y > hi.y || o.hi.y < lo.y || o.lo.z > hi.z || o.hi.z < lo.z)
                continue;
            taken.insert(o.id);
            out.push_back(o.id);
            queue.push_back(o);
        }
    }
    return out;
}

void registerCommands(ext::Context& ctx)
{
    std::vector<std::string> roles(std::begin(kRoleNames), std::end(kRoleNames));
    std::vector<std::string> shapes(std::begin(kShapeNames), std::end(kShapeNames));
    std::vector<std::string> mats;
    for (const Material& m : materials())
        mats.emplace_back(m.name);

    ctx.registerCommand({
        .name = "physicsBody",
        .doc = "Set how meshes take part in physics: role, material, shape. Makes or edits a stagehandBody child under each.\n\n"
               "Meshes without one are static colliders, except the ones a command moves. material= fills density, friction "
               "and bounce from a preset; give those flags too to override. Returns the body node of each mesh.",
        .positionalDoc = "meshes (default the selection)",
        .flags = {{"role", "r", T::String, nullptr, "dynamic, static, kinematic or off.", roles},
                  {"material", "m", T::String, nullptr, "Preset for density, friction and bounce.", mats},
                  {"density", "d", T::Float, nullptr, "kg/m3.", {}, {}, 1.0},
                  {"friction", "f", T::Float, nullptr, "0 to 1.", {}, {}, 0.0},
                  {"bounce", "b", T::Float, nullptr, "0 to 1.", {}, {}, 0.0, 1.0},
                  {"shape", "s", T::String, nullptr, "auto, box, hull, sphere, capsule, mesh or decompose (convex parts).", shapes},
                  {"maxParts", "mp", T::Int, nullptr, "Most convex parts for decompose.", {}, {}, 1.0, 64.0},
                  {"rounding", "ro", T::Float, nullptr, "Collision edge rounding, cm.", {}, {}, 0.0},
                  {"remove", "rm", T::Bool, false, "Delete the body nodes instead."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::vector<NodeId> meshes = meshesNamed(g, a.objects(), "physicsBody");
            json out = json::array();
            for (NodeId id : meshes)
            {
                Node* body = bodyNode(g, id);
                if (a.get<bool>("remove", false))
                {
                    if (body)
                        ops::execute("delete", {{"args", {body->path()}}});
                    continue;
                }
                if (!body)
                {
                    const json made = ops::execute("createNode", {{"args", {kBodyType}}, {"name", "body"}, {"parent", g.node(id)->path()}, {"skipSelect", true}});
                    body = g.find(made.get<std::string>());
                    if (!body)
                        throw CommandError("physicsBody: couldn't make a body under " + g.node(id)->path());
                }
                const std::string p = body->path();
                auto set = [&](const char* attr, const json& v) { ops::execute("setAttr", {{"args", {p + "." + attr, v}}}); };
                if (a.has("material"))
                {
                    const std::string m = a.get<std::string>("material");
                    const Material* mat = material(m);
                    if (!mat)
                        throw CommandError("physicsBody: unknown material '" + m + "'");
                    set("material", int64_t(mat - materials().data()));
                    set("density", mat->density);
                    set("friction", mat->friction);
                    set("bounce", mat->bounce);
                }
                if (a.has("role"))
                    set("role", choiceIndex(a.get<std::string>("role"), {"dynamic", "static", "kinematic", "off"}, "role"));
                if (a.has("shape"))
                    set("shape", choiceIndex(a.get<std::string>("shape"), {"auto", "box", "hull", "sphere", "capsule", "mesh", "decompose"}, "shape"));
                for (const char* f : {"density", "friction", "bounce", "rounding", "maxParts"})
                    if (a.has(f))
                        set(f, a.flags[f]);
                out.push_back(p);
            }
            return out;
        },
        .examples = {"physicsBody(['book_01', 'book_02'], role='dynamic', material='book')", "physicsBody('shelf', role='static', shape='mesh')"},
    });

    std::vector<FlagDef> settleFlags = worldFlags();
    ctx.registerCommand({
        .name = "settle",
        .doc = "Let meshes fall and come to rest under gravity, against everything around them, and keep where they end up.\n\n"
               "The targets move, so do meshes with a dynamic physicsBody; every other visible mesh nearby is hit but stays put. "
               "One undo step. Returns per mover how far it moved and turned, whether it came to rest, how deep it overlaps "
               "anything (cm) and what it rests on, then seconds simulated and atRest for the whole run.",
        .positionalDoc = "meshes to settle (default the selection)",
        .flags = settleFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            WorldOptions o = optionsOf(g, a, meshesNamed(g, a.objects(), "settle"), "settle", false);
            World w(g, o);
            const physics::SettleResult t = w.settle(a.get<double>("time", 120.0));
            w.write(g);
            return reportOf(g, w, t.seconds, o.gravity, t.capped);
        },
        .examples = {"settle(['mug', 'plate', 'book_01'])", "settle('crate', time=10, ground=False)"},
    });

    std::vector<FlagDef> pushFlags = worldFlags();
    pushFlags.insert(pushFlags.begin(),
                     {{"direction", "dir", T::List, nullptr, "Which way to push, [x, y, z]; its length doesn't matter."},
                      {"speed", "sp", T::Float, 150.0, "How hard: the speed given at the point pushed, cm/s. 50 nudges, 150 topples a book, 500 knocks it flying.", {}, {}, 0.0},
                      {"at", "", T::List, nullptr, "World point to push at (default high on the side away from the direction, which topples)."},
                      {"chain", "ch", T::Bool, true, "Without movable=, carry on to the meshes touching it, and those touching them."}});
    ctx.registerCommand({
        .name = "push",
        .doc = "Shove a mesh and let it, and whatever it knocks into, settle: topple a book along a shelf, punt a crate.\n\n"
               "Everything touching the target (chained, smaller than it x8) may move unless movable= says otherwise. One undo "
               "step. Returns the same report as settle.",
        .positionalDoc = "mesh to push",
        .flags = pushFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("push: give one mesh");
            if (!a.has("direction"))
                throw CommandError("push: give direction=[x, y, z]");
            const NodeId target = meshNamed(g, names[0], "push");
            const glm::dvec3 dir = vecArg(a, "direction", {1, 0, 0}, "push");
            if (glm::length(dir) < 1e-9)
                throw CommandError("push: direction is zero");
            WorldOptions o = optionsOf(g, a, {target}, "push", a.get<bool>("chain", true));
            World w(g, o);
            const glm::dvec3 up = glm::length(o.gravity) > 1e-9 ? -glm::normalize(o.gravity) : glm::dvec3(0, 1, 0);
            glm::dvec3 lo, hi;
            w.scene().bounds(0, lo, hi);
            const glm::dvec3 at = a.has("at") ? vecArg(a, "at", {}, "push") : pushPoint(lo, hi, glm::normalize(dir), up);
            w.scene().impulse(0, glm::normalize(dir) * a.get<double>("speed", 150.0) * w.scene().mass(0), at);
            const physics::SettleResult t = w.settle(a.get<double>("time", 120.0));
            w.write(g);
            json r = reportOf(g, w, t.seconds, o.gravity, t.capped);
            r["pushedAt"] = v3(at);
            return r;
        },
        .examples = {"push('book_01', direction=[1, 0, 0])", "push('crate', direction=[0, 0, -1], speed=400, movable=[])"},
    });

    std::vector<FlagDef> wiggleFlags = worldFlags();
    wiggleFlags.insert(wiggleFlags.begin(), {{"strength", "st", T::Float, 40.0, "Speed of each little push, cm/s.", {}, {}, 0.0},
                                             {"times", "n", T::Int, int64_t(3), "How many rounds of pushes, each settled before the next.", {}, {}, 1.0},
                                             {"seed", "s", T::Int, int64_t(1), "Another seed gives other pushes."}});
    ctx.registerCommand({
        .name = "wiggle",
        .doc = "Jostle meshes with small random pushes so they settle into a natural, slightly messy rest.\n\n"
               "Only the targets (and dynamic bodies) move. One undo step. Returns the same report as settle.",
        .positionalDoc = "meshes (default the selection)",
        .flags = wiggleFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            WorldOptions o = optionsOf(g, a, meshesNamed(g, a.objects(), "wiggle"), "wiggle", false);
            Session s(g, o);
            physics::RigidScene& scene = s.world().scene();
            std::mt19937 rng(uint32_t(a.get<int64_t>("seed", 1)));
            std::uniform_real_distribution<double> u(-1.0, 1.0);
            const double strength = a.get<double>("strength", 40.0);
            const int64_t rounds = std::max<int64_t>(1, a.get<int64_t>("times", 3));
            const double each = a.get<double>("time", 120.0) / double(rounds);
            double t = 0.0;
            bool capped = false;
            for (int64_t r = 0; r < rounds; ++r)
            {
                for (size_t i = 0; i < s.world().movers(); ++i)
                {
                    const glm::dvec3 dir = glm::normalize(glm::dvec3(u(rng), 0.35 + 0.3 * std::abs(u(rng)), u(rng)));
                    glm::dvec3 lo, hi;
                    scene.bounds(int(i), lo, hi);
                    const glm::dvec3 at = (lo + hi) * 0.5 + glm::dvec3(u(rng), u(rng), u(rng)) * (hi - lo) * 0.25;
                    scene.impulse(int(i), dir * strength * scene.mass(int(i)), at);
                }
                const physics::SettleResult done = s.world().settle(each);
                t += done.seconds;
                capped = done.capped;
            }
            s.world().write(g);
            return reportOf(g, s.world(), t, o.gravity, capped);
        },
        .examples = {"wiggle(['book_01', 'book_02', 'book_03'], strength=60)"},
    });

    std::vector<FlagDef> sweepFlags = worldFlags();
    sweepFlags.insert(sweepFlags.begin(), {{"path", "p", T::List, nullptr, "The broom's stroke, world points [[x, y, z], ...]; with gather=True, one point to gather toward."},
                                           {"radius", "r", T::Float, 30.0, "Size of the broom, cm.", {}, {}, 1.0},
                                           {"speed", "sp", T::Float, 60.0, "How fast the broom moves along the stroke, and what it sweeps with it, cm/s.", {}, {}, 1.0},
                                           {"gather", "ga", T::Bool, false, "Pull everything under the broom toward its centre instead of along the stroke."}});
    ctx.registerCommand({
        .name = "sweep",
        .doc = "Sweep meshes along a stroke like a broom, or gather them toward a point, then let them settle: push litter into "
               "a corner, herd scattered books into a heap.\n\n"
               "Only the targets (and dynamic bodies) move; the broom carries whatever is within radius of it as it travels "
               "the stroke at speed. One undo step. Returns the same report as settle.",
        .positionalDoc = "meshes (default the selection)",
        .flags = sweepFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            if (!a.has("path") || !a.flags["path"].is_array() || a.flags["path"].empty())
                throw CommandError("sweep: give path=[[x, y, z], ...]");
            std::vector<glm::dvec3> path;
            for (const json& p : a.flags["path"])
            {
                if (!p.is_array() || p.size() != 3)
                    throw CommandError("sweep: each point of path is [x, y, z]");
                path.emplace_back(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
            }
            const bool gather = a.get<bool>("gather", false);
            if (!gather && path.size() < 2)
                throw CommandError("sweep: a stroke needs two points or more (or gather=True)");
            WorldOptions o = optionsOf(g, a, meshesNamed(g, a.objects(), "sweep"), "sweep", false);
            Session s(g, o);
            const double radius = a.get<double>("radius", 30.0);
            const double speed = a.get<double>("speed", 60.0);
            const double dt = s.step();
            if (gather)
            {
                // Long enough for the rim of the broom to reach its centre.
                for (double t = 0.0; t < radius / speed * 3.0 + 0.5; t += dt)
                {
                    s.sweep(path[0], {}, radius, speed, true);
                    s.advance(dt);
                }
            }
            else
                for (size_t k = 1; k < path.size(); ++k)
                {
                    const glm::dvec3 seg = path[k] - path[k - 1];
                    const double len = glm::length(seg);
                    for (double d = 0.0; d < len; d += speed * dt)
                    {
                        s.sweep(path[k - 1] + seg * (d / len), seg, radius, speed, false);
                        s.advance(dt);
                    }
                }
            const physics::SettleResult done = s.world().settle(a.get<double>("time", 120.0));
            s.world().write(g);
            return reportOf(g, s.world(), s.time() + done.seconds, o.gravity, done.capped);
        },
        .examples = {"sweep(['can_01', 'can_02', 'wrapper_01'], path=[[0, 0, 0], [200, 0, 0]], radius=40)",
                     "sweep(ls('Book*'), path=[[50, 0, 50]], gather=True, radius=120)"},
    });

    std::vector<FlagDef> dropFlags = worldFlags();
    dropFlags.insert(dropFlags.begin(), {{"height", "h", T::Float, 0.0, "Lift this far first, cm."},
                                         nodeFlag({"onto", "on", T::String, nullptr, "Drop onto this mesh: moved above its top first (at= picks where)."}, "mesh"),
                                         {"at", "", T::List, nullptr, "With onto=, the world [x, y, z] to drop over; y is ignored."},
                                         {"spin", "sn", T::Float, 0.0, "Random turn given before the drop, degrees, so a row of drops lands varied.", {}, {}, 0.0},
                                         {"seed", "s", T::Int, int64_t(1), "Seed for spin."}});
    ctx.registerCommand({
        .name = "drop",
        .doc = "Drop meshes under gravity onto whatever is below them, or onto a given mesh, and keep where they land.\n\n"
               "With onto=, each target is moved above that mesh first (centred, or over at=) and stacked up so they don't "
               "start inside each other. One undo step. Returns the same report as settle.",
        .positionalDoc = "meshes to drop (default the selection)",
        .flags = dropFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const std::vector<NodeId> targets = meshesNamed(g, a.objects(), "drop");
            WorldOptions o = optionsOf(g, a, targets, "drop", false);
            const glm::dvec3 up = glm::length(o.gravity) > 1e-9 ? -glm::normalize(o.gravity) : glm::dvec3(0, 1, 0);
            if (a.has("onto"))
            {
                const NodeId onto = meshNamed(g, a.flags["onto"], "drop");
                o.movers.erase(std::remove(o.movers.begin(), o.movers.end(), onto), o.movers.end());
                o.colliders.push_back(onto);
            }
            World w(g, o);
            physics::RigidScene& scene = w.scene();
            std::mt19937 rng(uint32_t(a.get<int64_t>("seed", 1)));
            std::uniform_real_distribution<double> u(-1.0, 1.0);
            const double spin = glm::radians(a.get<double>("spin", 0.0));
            const double lift = a.get<double>("height", 0.0);

            glm::dvec3 ontoLo(0.0), ontoHi(0.0);
            const bool hasOnto = a.has("onto") && boundsOf(g, meshNamed(g, a.flags["onto"], "drop"), ontoLo, ontoHi);
            double stack = hasOnto ? glm::dot(ontoHi, up) : 0.0;
            for (size_t i = 0; i < targets.size() && i < w.movers(); ++i)
            {
                physics::Pose p = scene.pose(int(i));
                if (spin > 0.0)
                    p.rotation = glm::angleAxis(spin * u(rng), glm::normalize(glm::dvec3(u(rng), u(rng), u(rng)) + up * 1e-3)) * p.rotation;
                scene.setPose(int(i), p);
                glm::dvec3 lo, hi;
                scene.bounds(int(i), lo, hi);
                glm::dvec3 move = up * lift;
                if (hasOnto)
                {
                    const glm::dvec3 over = a.has("at") ? vecArg(a, "at", {}, "drop") : (ontoLo + ontoHi) * 0.5;
                    const glm::dvec3 c = (lo + hi) * 0.5;
                    const glm::dvec3 flat = (over - c) - up * glm::dot(over - c, up);
                    // Each lands from just above the last, so a batch doesn't start overlapping.
                    const double bottom = glm::dot(lo, up);
                    move = flat + up * (stack + 1.0 - bottom + lift);
                    stack += glm::dot(hi - lo, up) + 1.0;
                }
                p.position += move;
                scene.setPose(int(i), p);
            }
            const physics::SettleResult t = w.settle(a.get<double>("time", 120.0));
            w.write(g);
            return reportOf(g, w, t.seconds, o.gravity, t.capped);
        },
        .examples = {"drop('mug', onto='desk', at=[40, 0, 10])", "drop(['apple_01', 'apple_02', 'apple_03'], onto='bowl', spin=180)"},
    });

    std::vector<FlagDef> shakeFlags = worldFlags();
    shakeFlags.insert(shakeFlags.begin(), {{"amplitude", "a", T::List, json::array({3.0, 0.0, 0.0}), "How far it swings each way, [x, y, z] cm."},
                                           {"duration", "du", T::Float, 1.0, "How long the shaking lasts, seconds.", {}, {}, 0.0}});
    ctx.registerCommand({
        .name = "shake",
        .doc = "Shake a mesh (a shelf, a table) so what rests on it shuffles, slides and falls, then settles; the shaken mesh "
               "ends where it started.\n\n"
               "What moves: movable=, else what touches it (chained) and is smaller than it. One undo step. Returns the same "
               "report as settle.",
        .positionalDoc = "mesh to shake",
        .flags = shakeFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("shake: give one mesh");
            const NodeId target = meshNamed(g, names[0], "shake");
            WorldOptions o;
            std::vector<NodeId> movers;
            if (a.has("movable"))
                movers = listArg(g, a, "movable", "shake");
            else
            {
                // What rests on it: touching it and smaller, then whatever touches those.
                glm::dvec3 lo, hi;
                double size = 1.0;
                if (boundsOf(g, target, lo, hi))
                    size = volumeOf(lo, hi);
                for (NodeId id : neighbours(g, {target}, 2.0, 1.0, {}))
                {
                    glm::dvec3 a2, b2;
                    if (boundsOf(g, id, a2, b2) && volumeOf(a2, b2) < size)
                        movers.push_back(id);
                }
            }
            movers.erase(std::remove(movers.begin(), movers.end(), target), movers.end());
            o.movers = movers;
            o.colliders.push_back(target);
            if (a.has("colliders"))
                for (NodeId id : listArg(g, a, "colliders", "shake"))
                    o.colliders.push_back(id);
            o.collidersGiven = a.has("colliders");
            o.ground = a.get<bool>("ground", true);
            o.groundHeight = a.get<double>("groundHeight", 0.0);
            o.gravity = vecArg(a, "gravity", o.gravity, "shake");
            o.reach = a.get<double>("reach", o.reach);
            Session s(g, o);
            const int body = s.world().bodyOf(target);
            if (body < 0)
                throw CommandError("shake: " + g.node(target)->path() + " isn't visible to physics");
            s.shake(body, vecArg(a, "amplitude", {3, 0, 0}, "shake"), a.get<double>("duration", 1.0));
            const double total = a.get<double>("duration", 1.0) + a.get<double>("time", 120.0);
            double t = 0.0;
            while (t < total)
            {
                s.advance(s.step());
                t += s.step();
                if (t > a.get<double>("duration", 1.0) && s.world().scene().atRest())
                    break;
            }
            s.world().write(g);
            return reportOf(g, s.world(), t, o.gravity, t >= total);
        },
        .examples = {"shake('shelf', amplitude=[4, 0, 0], duration=1.5)"},
    });

    ctx.registerCommand({
        .name = "dressInfo",
        .doc = "Physics facts about meshes: body settings, collision shape, mass and bounds.",
        .positionalDoc = "meshes (default the selection)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            g.evaluate();
            json out = json::array();
            for (NodeId id : meshesNamed(g, a.objects(), "dressInfo"))
            {
                const BodySettings s = bodySettings(g, id);
                const physics::Shape shape = shapeOf(g, id, s.shape, s.rounding, s.role == RoleChoice::Static && s.hasNode, s.maxParts);
                const char* kinds[] = {"hull", "box", "sphere", "capsule", "mesh", "compound", "heightfield"};
                glm::dvec3 lo(0.0), hi(0.0);
                boundsOf(g, id, lo, hi);
                WorldOptions o;
                o.movers = {id};
                o.includeDynamicBodies = false;
                o.collidersGiven = true;
                o.ground = false;
                World w(g, o);
                out.push_back({{"node", g.node(id)->path()},
                               {"body", s.hasNode ? json(bodyNode(g, id)->path()) : json()},
                               {"role", kRoleNames[int(s.role)]},
                               {"material", s.material},
                               {"density", s.density},
                               {"friction", s.friction},
                               {"bounce", s.bounce},
                               {"shape", kinds[int(shape.kind)]},
                               {"parts", shape.kind == physics::Shape::Kind::Compound ? int64_t(shape.parts.size()) : int64_t(1)},
                               {"mass", w.movers() ? w.scene().mass(0) : 0.0},
                               {"boundsMin", v3(lo)},
                               {"boundsMax", v3(hi)}});
            }
            return out;
        },
        .examples = {"dressInfo('book_01')[0]['mass']"},
    });

    ctx.registerCommand({
        .name = "dressCheck",
        .doc = "Check a dressed set: would anything move if physics ran, is anything floating or inside something else.\n\n"
               "Simulates a copy for `time` seconds without changing the scene. Each issue names the mesh and a kind: unstable "
               "(would move more than tolerance cm or turn more than 5 deg), floating (falls more than tolerance before "
               "touching anything), overlapping (inside another mesh by more than tolerance). Returns issues, per-mesh results "
               "and ok.",
        .positionalDoc = "meshes to check (default the selection)",
        .flags = {{"tolerance", "tol", T::Float, 0.5, "How far, cm, counts as a problem.", {}, {}, 0.0},
                  {"time", "ti", T::Float, 3.0, "How long to watch, seconds.", {}, {}, 0.0},
                  {"ground", "gr", T::Bool, true, "A floor plane at groundHeight."},
                  {"groundHeight", "gh", T::Float, 0.0, "Height of the floor plane, cm."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            WorldOptions o;
            o.movers = meshesNamed(g, a.objects(), "dressCheck");
            o.includeDynamicBodies = false;
            o.ground = a.get<bool>("ground", true);
            o.groundHeight = a.get<double>("groundHeight", 0.0);
            World w(g, o);
            const double tol = a.get<double>("tolerance", 0.5);
            json issues = json::array();
            std::vector<double> overlap(w.movers());
            for (size_t i = 0; i < w.movers(); ++i)
                overlap[i] = w.scene().penetration(int(i));
            // Floating: one tiny settle step tells nothing, so watch the drop until first contact.
            std::vector<double> firstTouch(w.movers(), -1.0);
            const double dt = 1.0 / 120.0;
            double t = 0.0;
            const double watch = a.get<double>("time", 3.0);
            while (t < watch)
            {
                w.scene().step(dt);
                t += dt;
                const auto contacts = w.scene().contacts();
                for (const physics::Contact& c : contacts)
                    for (int b : {c.a, c.b})
                        if (b >= 0 && size_t(b) < w.movers() && firstTouch[size_t(b)] < 0.0)
                            firstTouch[size_t(b)] = glm::length(glm::dvec3(w.worldMatrix(b)[3]) - glm::dvec3(w.startMatrix(b)[3]));
                if (w.scene().atRest())
                    break;
            }
            json results = json::array();
            for (size_t i = 0; i < w.movers(); ++i)
            {
                const std::string name = g.node(w.node(int(i)))->path();
                const double moved = glm::length(glm::dvec3(w.worldMatrix(int(i))[3]) - glm::dvec3(w.startMatrix(int(i))[3]));
                const double turned = angleBetween(w.worldMatrix(int(i)), w.startMatrix(int(i)));
                const double fell = firstTouch[i] < 0.0 ? moved : firstTouch[i];
                if (overlap[i] > tol)
                    issues.push_back({{"node", name}, {"kind", "overlapping"}, {"depth", overlap[i]}});
                if (fell > tol)
                    issues.push_back({{"node", name}, {"kind", "floating"}, {"gap", fell}});
                else if (moved > tol || turned > 5.0)
                    issues.push_back({{"node", name}, {"kind", "unstable"}, {"moved", moved}, {"turned", turned}});
                results.push_back({{"node", name}, {"moved", moved}, {"turned", turned}, {"overlap", overlap[i]}, {"gap", fell}});
            }
            return {{"issues", issues}, {"results", results}, {"ok", issues.empty()}};
        },
        .examples = {"r = dressCheck(['book_01', 'book_02', 'mug'])\nfor i in r['issues']: print(i)"},
    });
}
} // namespace stagehand
