// Grapple ragdolls: one physics solver holding every character of a grapple, its contacts as springs between their bodies,
// so throws, pushes, falls and struggles simulate with both bodies reacting to each other.

#include "PhysicsAnimation.h"

#include <lila/Anim.h>
#include <lila/Command.h>
#include <lila/Grapple.h>
#include <lila/Log.h>
#include <lila/Simulation.h>

using namespace lila;
using namespace lila::scene;

namespace physanim
{
namespace grappleragdoll
{
namespace gp = lila::scene::grapple;

NodeId grappleFrom(Graph& g, const Args& a)
{
    for (const std::string& s : a.objects())
        if (Node* n = named(g, s))
            if (const NodeId gr = gp::grappleOf(g, n->id()))
                return gr;
    NodeId only = kNoNode;
    for (NodeId id : g.nodes())
        if (g.node(id)->type().isA(gp::kType))
        {
            if (only)
                throw CommandError("grappleRagdoll: the scene has several grapples; name one");
            only = id;
        }
    if (!only)
        throw CommandError("grappleRagdoll: no grapple; make contacts with grappleContact first");
    return only;
}

/** The solver already simulating the grapple's contacts, if one does. */
NodeId grappleSolver(Graph& g, NodeId grapple)
{
    for (NodeId id : g.nodes())
        if (Node* s = g.node(id); s && s->type().isA(kSolverType))
            if (Node* gr = named(g, text(*s, "grapple")); gr && gr->id() == grapple)
                return id;
    return kNoNode;
}
} // namespace grappleragdoll

void registerGrappleRagdoll(ext::Context& ctx)
{
    using namespace grappleragdoll;
    using T = ArgType;
    ctx.registerCommand({
        .name = "grappleRagdoll",
        .doc = "Make the grapple's characters one ragdoll: a physics solver with markers on every character (from their joints' labels, "
               "fitted to the skinned meshes), pulled toward the animation (with the grapple's bake) by pose matching, the grapple's "
               "contacts as springs between the bodies and the characters colliding with each other. Simulate, then recordPhysics "
               "bakes it. Returns {'solver', 'markers'}.",
        .positionalDoc = "grapple or one of its contacts (default the selection, else the scene's only grapple)",
        .flags = {{"name", "n", T::String, nullptr, "Name for a new solver. Default: the grapple's name with _physics."},
                  {"poseStiffness", "ps", T::Float, nullptr, "Pose match spring frequency for every new marker, Hz. Default: each label's own.", {}, {}, 0.0}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const NodeId grapple = grappleFrom(g, a);
            const std::vector<gp::FigureSetup> figures = gp::figuresOf(g, grapple);
            if (figures.empty())
                throw CommandError("grappleRagdoll: the grapple has no characters yet");
            NodeId solver = grappleSolver(g, grapple);
            if (!solver)
            {
                const std::string name = a.has("name") ? a.get<std::string>("name") : g.node(grapple)->name() + "_physics";
                Node* s = named(g, ops::execute("createPhysicsSolver", {{"name", name}}).get<std::string>());
                if (!s)
                    throw CommandError("grappleRagdoll: could not make the solver");
                solver = s->id();
                g.setValue({solver, attr(*s, "grapple"), -1}, g.path(grapple));
                g.setValue({solver, attr(*s, "startFrame"), -1}, integer(*g.node(grapple), "startFrame"));
            }
            const std::string solverPath = g.path(solver);
            json markers = json::object();
            for (const gp::FigureSetup& f : figures)
            {
                const std::string root = g.path(f.root);
                json made = json::array();
                try
                {
                    made = ops::execute("physicsMarkersFromTemplate", {{"args", {root}}, {"solver", solverPath}});
                }
                catch (const CommandError& e)
                {
                    log::write(LogLevel::Warning, std::string("grappleRagdoll: ") + root + ": " + e.what());
                }
                if (a.has("poseStiffness"))
                    for (const json& m : made)
                        if (Node* n = named(g, m.get<std::string>()))
                            g.setValue({n->id(), attr(*n, "poseStiffness"), -1}, a.get<double>("poseStiffness"));
                // Run again, the markers made before are what the character has.
                size_t count = made.size();
                if (count == 0)
                    for (NodeId m : membersOf(g, solver, kMarkerType))
                        if (const NodeId j = jointOf(g, *g.node(m)); j && gp::skeletonRoot(g, j) == f.root)
                            ++count;
                if (count == 0)
                    throw CommandError("grappleRagdoll: " + root + " has no joints a ragdoll can be made from (or they are all in another solver)");
                markers[root] = count;
            }
            sim::clearCache(g, solver);
            g.select({solver});
            return {{"solver", solverPath}, {"markers", markers}};
        },
        .examples = {"grappleRagdoll(\"grapple1\")", "recordPhysics(\"grapple1_physics\")"},
    });
}
} // namespace physanim
