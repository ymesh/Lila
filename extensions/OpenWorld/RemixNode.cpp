// The World Remix geometry node: the remixed map as a heightfield, so a remix is a terrain every Terrain command and
// node works on, and stays live as its spec changes.

#include "RemixNode.h"

#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Profile.h>

#include <map>
#include <mutex>

using namespace lila;

namespace openworld
{
namespace
{
std::mutex& lock()
{
    static std::mutex m;
    return m;
}

std::map<uint64_t, Published>& published()
{
    static std::map<uint64_t, Published> m;
    return m;
}
} // namespace

void forgetGoneRemixes()
{
    const lila::scene::Graph& g = lila::scene::Graph::current();
    std::lock_guard l(lock());
    std::erase_if(published(), [&](const auto& e) { return !g.node(lila::scene::NodeId(e.first)); });
}

bool publishedRemix(uint64_t modifier, Published* out)
{
    std::lock_guard l(lock());
    const auto it = published().find(modifier);
    if (it == published().end())
        return false;
    *out = it->second;
    return true;
}

std::vector<std::pair<uint64_t, Published>> publishedRemixes()
{
    std::lock_guard l(lock());
    return {published().begin(), published().end()};
}

void forgetRemix(uint64_t modifier)
{
    std::lock_guard l(lock());
    published().erase(modifier);
}

gn::NodeDecl remixDecl()
{
    using Socket = gn::SocketType;
    return {
        .type = "WorldRemix",
        .label = "World Remix",
        .category = "Terrain",
        .doc = "Real places composed onto one map: each source's elevation, OSM data and land cover scaled and placed, regions cut, moved, "
               "duplicated and deleted, pins warping the land between, seams blended and eroded, roads reconnected, rivers re-solved "
               "and coastline re-merged. The spec is the remix's JSON (lila://schema/remix-spec); before shows the base map with no "
               "regions or pins. Outputs the heightfield with land cover masks and remixSeam, remixHole.",
        .sockets = {.inputs = {{.id = "spec", .type = Socket::String, .defaultValue = "{}", .doc = "The remix spec, JSON.", .editor = "multiline"},
                               {.id = "before", .type = Socket::Bool, .defaultValue = false, .doc = "Show the base map: sources placed, nothing moved."}},
                    .outputs = {{.id = "heightfield", .type = Socket::Geometry}}},
        .exec =
            [](gn::ExecContext& c) {
                LILA_ZONE();

                RemixSpec spec;
                std::shared_ptr<const RemixResult> r;
                try
                {
                    const json j = json::parse(c.s("spec"));
                    spec = specFromJson(j);
                    r = cachedRemix(spec);
                }
                catch (const std::exception& e)
                {
                    throw gn::ExecError(c.node.name + ": " + e.what());
                }
                const bool before = c.b("before");
                if (c.env.owner)
                {
                    std::lock_guard l(lock());
                    published()[c.env.owner] = {spec, r, before};
                }
                c.out("heightfield", gn::heightfieldGeometry(before ? r->before : r->after));
            },
    };
}

bool registerRemixNode(ext::Context& ctx) { return gn::registerDecl(ctx, remixDecl()) != nullptr; }
} // namespace openworld
