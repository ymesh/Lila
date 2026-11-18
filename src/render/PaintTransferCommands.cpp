#include "render/PaintGpu.h"

#include "bake/Bake.h"
#include "core/ImageIO.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "geo/Bvh.h"
#include "ops/Command.h"
#include "scene/Node.h"
#include "scene/Transform.h"
#include "tex/Plan.h"
#include "tex/PaintSurface.h"
#include "tex/TextureSet.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

/**
 * Moving a texture set's look onto another mesh: an old version to a new one, or a high mesh to its low one. Every texel
 * of the target finds the closest point of the source surface and takes the set's evaluated channels there.
 */
namespace lila::render::paint
{
// Named, not anonymous: unity builds put this beside the other paint command files.
namespace transfer
{
using scene::Node;
using json = nlohmann::json;

scene::Graph& graph() { return scene::Graph::current(); }

glm::vec4 bilinear(const image::Image& img, glm::vec2 uv)
{
    const float x = (uv.x - std::floor(uv.x)) * float(img.width) - 0.5f, y = (1.0f - (uv.y - std::floor(uv.y))) * float(img.height) - 0.5f;
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const float fx = x - float(x0), fy = y - float(y0);
    auto at = [&](int xx, int yy) {
        xx = std::clamp(xx, 0, img.width - 1);
        yy = std::clamp(yy, 0, img.height - 1);
        const float* t = &img.rgba[(size_t(yy) * size_t(img.width) + size_t(xx)) * 4];
        return glm::vec4(t[0], t[1], t[2], t[3]);
    };
    return glm::mix(glm::mix(at(x0, y0), at(x0 + 1, y0), fx), glm::mix(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy);
}

json transferSet(const Args& a)
{
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError("texTransferSet: texture set required");
    Node* set = tex::findSet(graph(), a.positional[0].get<std::string>());
    if (!set)
        throw CommandError("texTransferSet: no texture set named '" + a.positional[0].get<std::string>() + "'");
    Node* from = graph().find(a.get<std::string>("source", ""));
    Node* to = graph().find(a.get<std::string>("onto", ""));
    if (!from || !to)
        throw CommandError("texTransferSet: give source= (the mesh the set paints now) and onto= (the mesh to move it onto)");
    Node* target = a.has("into") ? tex::findSet(graph(), a.get<std::string>("into")) : set;
    if (!target)
        throw CommandError("texTransferSet: no texture set named '" + a.get<std::string>("into") + "'");
    graph().evaluate();
    std::shared_ptr<const PaintMesh> src = paintMesh(graph(), from->path(), setMaterial(*set));
    std::shared_ptr<const PaintMesh> dst = paintMesh(graph(), to->path(), setMaterial(*target));
    if (!src || !dst)
        throw CommandError("texTransferSet: both meshes need faces with the sets' materials");

    const tex::SetSettings ss = tex::readSettings(*set), ts = tex::readSettings(*target);
    std::map<std::string, image::Image> source;
    try
    {
        source = tex::evaluateOutputs(*set, tex::channelRequests(tex::readGraph(*set), ss), ss.tiles.front().udim, 0, 0);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("texTransferSet: ") + e.what());
    }
    const glm::dmat4 toSource = glm::inverse(from->as<glm::dmat4>(scene::Transform::kWorldMatrix)) * to->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    const double maxDistance = a.get<double>("maxDistance", 1e30);

    const tex::Tile& tile = ts.tiles.front();
    const auto [w, h] = ts.tileSize(tile);
    tex::paint::SurfaceAtlas atlas(dst, tile.udim, w, h);
    std::map<std::string, std::vector<float>> out;
    for (const auto& [name, img] : source)
        if (ts.channel(name))
            out[name].assign(size_t(w) * size_t(h) * 4, 0.0f);
    size_t texels = 0, missed = 0;
    for (uint32_t page = 0; page < uint32_t(atlas.pagesX() * atlas.pagesY()); ++page)
    {
        if (!atlas.pageUsed(page))
            continue;
        const auto& geo = atlas.page(page);
        const int px = int(page % uint32_t(atlas.pagesX())) * tex::paint::kPageSize, py = int(page / uint32_t(atlas.pagesX())) * tex::paint::kPageSize;
        for (int i = 0; i < tex::paint::kPageSize * tex::paint::kPageSize; ++i)
        {
            const tex::paint::GeoTexel& g = geo[size_t(i)];
            if (g.triangle == tex::paint::kNoTriangle)
                continue;
            const int x = px + i % tex::paint::kPageSize, y = py + i / tex::paint::kPageSize;
            if (x >= w || y >= h)
                continue;
            ++texels;
            const glm::dvec3 p = glm::dvec3(toSource * glm::dvec4(glm::dvec3(dst->point(g.triangle, g.u, g.v)), 1.0));
            geo::BvhClosest hit;
            if (!src->bvh->closestPoint(p, hit, maxDistance) || hit.triangle >= src->triangleCount())
            {
                ++missed;
                continue;
            }
            const glm::vec2 uv = src->uv(uint32_t(hit.triangle), float(hit.bary.y), float(hit.bary.z));
            for (auto& [name, rgba] : out)
            {
                glm::vec4 v = bilinear(source.at(name), uv);
                v.w = 1.0f;
                std::copy(&v[0], &v[0] + 4, &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4]);
            }
        }
    }
    if (texels == 0)
        throw CommandError("texTransferSet: the target mesh has no texels in " + target->name() + "'s first tile");

    const std::filesystem::path folder = bake::resolveFolder(a.get<std::string>("folder", "textures/" + target->name() + "/transferred"));
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    json paintRefs = json::object();
    for (const auto& [name, rgba] : out)
    {
        const std::filesystem::path f = folder / (target->name() + "_from_" + from->name() + "_" + name + ".exr");
        image::writeExr(f, w, h, rgba, 32);
        paintRefs[name] = paths::toUtf8(f);
    }
    const json layer = ops::execute("texAddLayer", {{"args", {target->name(), "paint"}}, {"name", "From " + from->name()}}, CallSource::Script);
    const std::string id = layer.value("id", std::string());
    ops::execute("texSetLayer", {{"args", {target->name(), id, {{"paint", paintRefs}}}}}, CallSource::Script);
    // Never silent: say how much of the target had nothing of the source near enough.
    if (missed > 0)
        log::warn("texTransferSet: {} of {} texels found no source surface within {}; they stay empty", missed, texels, maxDistance);
    return {{"layer", id}, {"texels", texels}, {"missed", missed}, {"channels", paintRefs}};
}
} // namespace transfer

void registerTransferCommands()
{
    ops::registerCommand({
        .name = "texTransferSet",
        .doc = "Move a texture set's look from one mesh onto another (an old version onto a new one, a high mesh onto its low one): each "
               "texel of the target takes the set's evaluated channels at the closest point of the source surface, as a new paint layer "
               "of the set into= (else the same set). Texels with no source within maxDistance stay empty and are counted. "
               "Returns {layer, texels, missed, channels}.",
        .positionalDoc = "set",
        .flags = {{"source", "", ArgType::String, nullptr, "The mesh the set paints now."},
                  {"onto", "", ArgType::String, nullptr, "The mesh to move it onto."},
                  {"into", "", ArgType::String, nullptr, "The set that gets the layer; else the same set."},
                  {"maxDistance", "", ArgType::Float, nullptr, "How far a target texel may be from the source surface."},
                  {"folder", "", ArgType::String, nullptr, "Where the pixels go; textures/<target>/transferred by default."}},
        .run = transfer::transferSet,
    });
}
} // namespace lila::render::paint
