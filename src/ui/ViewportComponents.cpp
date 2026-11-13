#include "ui/ViewportWidgetDetail.h"

#include "ui/UvLive.h"
#include "uv/Chart.h"
#include "uv/Uv.h"
#include "ui/SculptTool.h"
#include "sculpt/SculptSession.h"

#include <limits>

namespace lila::ui
{
using namespace viewportDetail;

namespace
{
/** Whether face picks grow to whole uv islands (the UV editor's island select mode). */
bool uvIslandPick()
{
    return scene::components::mode() == scene::ComponentMode::Face && uvLive::islandSelect();
}

/** Every face of the uv islands (in the current uv set) that hold one of the faces. */
std::vector<uint32_t> viewportUvIslands(const geo::Mesh& m, std::span<const uint32_t> faces)
{
    uv::ChartOptions co;
    co.source = uv::IslandSource::Uvs;
    co.uvSet = uv::currentUvSet();
    return uv::islandsOfFaces(m, faces, co);
}
} // namespace

scene::componentPick::Hover ViewportWidget::componentAt(QPointF a)
{
    LILA_ZONE();

    using scene::ComponentMode;
    namespace cp = scene::componentPick;
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    ComponentMode mode = scene::components::mode();
    cp::Hover best{mode};
    if (mode == ComponentMode::Object)
        return best;
    ViewInfo view = viewInfo();
    glm::dvec3 rayO, rayD;
    view.ray(a, rayO, rayD);
    std::vector<scene::NodeId> live = hilited(mIsolation);

    // Visible means nothing on any hilited mesh is closer along the view ray.
    auto visible = [&](const glm::dvec3& world) {
        glm::dvec3 dir = world - view.eye;
        double dist = glm::length(dir);
        dir /= dist;
        for (scene::NodeId id : live)
            if (occludes(&mIsolation, id, view.eye, dir, dist))
                return false;
        return true;
    };

    cp::Screen screen{mCamera.proj(aspect()) * mCamera.view(), double(width()), double(height())};
    const glm::dvec3 fwd = -glm::dvec3(view.view[0][2], view.view[1][2], view.view[2][2]);
    const double nearDepth = view.ortho ? -std::numeric_limits<double>::infinity() : view.proj[3][2] / (view.proj[2][2] + 1.0);
    auto depthOf = [&](const glm::dvec3& p) { return glm::dot(p - view.eye, fwd); };
    // An edge reaching behind the camera is tested for occlusion on its part in view.
    auto edgeAnchor = [&](const ComponentShape& shape, uint32_t e) {
        const glm::dvec3 mid = shape.edgeAnchor(e);
        if (depthOf(mid) >= nearDepth)
            return mid;
        glm::dvec3 a = shape.point(shape.cage.edgePoint(e, 0)), b = shape.point(shape.cage.edgePoint(e, 1));
        if (depthOf(a) < depthOf(b))
            std::swap(a, b);
        return (a + glm::mix(a, b, (depthOf(a) - nearDepth) / (depthOf(a) - depthOf(b)))) * 0.5;
    };
    const bool cull = paintSelect::backfaceCulling();
    const bool byCenter = cp::selectByCenter(mode);
    double bestPx = byCenter ? cp::centerRadius() : cp::radius(mode);
    double bestRay = 1e300;
    for (scene::NodeId id : live)
    {
        scene::Node* n = g.node(id);
        scene::MeshPtr mesh = scene::modifiers::cage(g, id);
        if (!mesh)
            continue;
        glm::dmat4 world = shownWorld(*n);
        const ComponentShape shape(*n, *mesh, mesh, world);
        if (byCenter)
        {
            auto centerOf = [&](uint32_t c) { return mode == ComponentMode::Edge ? edgeAnchor(shape, c) : shape.faceCentre(c); };
            auto vis = isolate::visibility(&mIsolation, id, mesh);
            for (const cp::Candidate& c : cp::nearbyCenters(*mesh, mode, centerOf, screen, {a.x(), a.y()}, bestPx))
                if (vis->shows(mode, c.id) && (!cull || paintSelect::frontFacing(*mesh, world, mode, c.id, view)) && visible(centerOf(c.id)))
                {
                    bestPx = c.px;
                    best.node = id;
                    best.id = c.id;
                    break;
                }
            continue;
        }
        if (mode == ComponentMode::Face)
        {
            const scene::MeshPtr& target = shape.smooth ? shape.smooth->mesh : mesh;
            auto pd = pickDataFor(target);
            if (!pd)
                continue;
            auto hit = isolate::raycast(&mIsolation, id, target, world, rayO, rayD, shape.level);
            if (!hit || hit->distance >= bestRay || hit->triangle >= pd->triangleFaces.size())
                continue;
            const uint32_t f = shape.cageOf(mode, pd->triangleFaces[hit->triangle]);
            if (f != geo::kInvalid && (!cull || paintSelect::frontFacing(*mesh, world, mode, f, view)))
            {
                bestRay = hit->distance;
                best.node = id;
                best.id = f;
            }
            continue;
        }
        auto vis = isolate::visibility(&mIsolation, id, mesh);
        for (const cp::Candidate& c : cp::nearby(shape.picked(), world, mode, screen, {a.x(), a.y()}, bestPx))
        {
            const uint32_t cid = shape.cageOf(mode, c.id);
            if (cid != geo::kInvalid && vis->shows(mode, cid) && (!cull || paintSelect::frontFacing(*mesh, world, mode, cid, view)) &&
                visible(mode == ComponentMode::Vertex ? shape.point(cid) : edgeAnchor(shape, cid)))
            {
                bestPx = c.px;
                best.node = id;
                best.id = cid;
                break;
            }
        }
    }
    if (mode == ComponentMode::Vertex)
        for (scene::NodeId id : pointNodes(mIsolation))
            for (auto& [cid, p] : worldPoints(g, id))
                if (auto s = view.project(p))
                    if (double d = std::hypot(s->x() - a.x(), s->y() - a.y()); d < bestPx)
                    {
                        bestPx = d;
                        best.node = id;
                        best.id = cid;
                    }
    return best;
}

void ViewportWidget::pickComponents(QPointF a, QPointF b, Qt::KeyboardModifiers mods)
{
    LILA_ZONE();

    using scene::ComponentMode;
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    ComponentMode mode = scene::components::mode();
    QRectF box = QRectF(a, b).normalized();
    bool click = box.width() < 4 && box.height() < 4;

    std::map<scene::NodeId, std::vector<uint32_t>> hits;
    if (click)
    {
        if (scene::componentPick::Hover h = componentAt(a); !h.empty())
        {
            hits[h.node].push_back(h.id);
            mClicked[1] = mClicked[0];
            mClicked[0] = h;
        }
    }
    else
    {
        // A marquee picks through the mesh unless camera-based selection limits it to what the camera sees.
        bool cameraBased = PrefStore::get().value<bool>("general", "/select/cameraBased", false);
        glm::dvec3 eye = viewInfo().eye;
        auto visible = [&](const glm::dvec3& world) {
            if (!cameraBased)
                return true;
            glm::dvec3 dir = world - eye;
            double dist = glm::length(dir);
            dir /= dist;
            for (scene::NodeId id : hilited(mIsolation))
                if (occludes(&mIsolation, id, eye, dir, dist))
                    return false;
            return true;
        };
        // Edges and faces count when the region touches them anywhere, or with the pref off only when it holds their centre.
        const bool touch = PrefStore::get().value<bool>("general", "/select/marqueeTouch", true);
        const scene::componentPick::Screen screen{mCamera.proj(aspect()) * mCamera.view(), double(width()), double(height())};
        for (scene::NodeId id : hilited(mIsolation))
        {
            scene::Node* n = g.node(id);
            scene::MeshPtr mesh = scene::modifiers::cage(g, id);
            if (!mesh)
                continue;
            const geo::Mesh& m = *mesh;
            const ComponentShape shape(*n, m, mesh, shownWorld(*n));
            if (mode == ComponentMode::Vertex)
            {
                for (uint32_t p = 0; p < m.pointCapacity(); ++p)
                    if (m.pointValid(p))
                        if (auto s = project(shape.point(p)); s && inSelectRegion(box, *s) && visible(shape.point(p)))
                            hits[id].push_back(p);
            }
            else if (mode == ComponentMode::Edge)
            {
                for (uint32_t e = 0; e < m.edgeCapacity(); ++e)
                {
                    if (!m.edgeValid(e))
                        continue;
                    bool in = false;
                    if (touch)
                        shape.forEachSegment(e, [&](const glm::dvec3& a, const glm::dvec3& b) {
                            if (in)
                                return;
                            auto seg = screen.projectSegment(a, b);
                            in = seg && touchesSelectRegion(box, {seg->first, seg->second});
                        });
                    else if (auto s = project(shape.edgeAnchor(e)))
                        in = inSelectRegion(box, *s);
                    if (in && visible(shape.edgeAnchor(e)))
                        hits[id].push_back(e);
                }
            }
            else if (mode == ComponentMode::Face)
            {
                for (uint32_t f = 0; f < m.faceCapacity(); ++f)
                    if (m.faceValid(f))
                    {
                        glm::dvec3 c = shape.faceCentre(f);
                        bool in = false;
                        if (touch)
                            shape.forEachPolygon(f, [&](const std::vector<glm::dvec3>& corners) {
                                if (in)
                                    return;
                                const std::vector<glm::dvec2> poly = screen.projectPolygon(corners);
                                in = !poly.empty() && touchesSelectRegion(box, poly);
                            });
                        else if (auto s = project(c))
                            in = inSelectRegion(box, *s);
                        if (in && visible(c))
                            hits[id].push_back(f);
                    }
            }
        }
    }

    if (!click && mode == ComponentMode::Vertex)
        for (scene::NodeId id : pointNodes(mIsolation))
            for (auto& [cid, p] : worldPoints(g, id))
                if (auto sp = project(p); sp && inSelectRegion(box, *sp))
                    hits[id].push_back(cid);

    for (auto it = hits.begin(); it != hits.end();)
    {
        auto vis = isolate::visibility(&mIsolation, it->first, scene::modifiers::cage(g, it->first));
        std::erase_if(it->second, [&](uint32_t id) { return !vis->shows(mode, id); });
        it = it->second.empty() ? hits.erase(it) : std::next(it);
    }

    if (!click && paintSelect::backfaceCulling())
    {
        ViewInfo view = viewInfo();
        for (auto it = hits.begin(); it != hits.end();)
        {
            scene::MeshPtr mesh = scene::modifiers::cage(g, it->first);
            if (!mesh && scene::components::pointProvider(*g.node(it->first)))
            {
                ++it;
                continue;
            }
            glm::dmat4 world = shownWorld(*g.node(it->first));
            std::erase_if(it->second, [&](uint32_t id) { return !mesh || !paintSelect::frontFacing(*mesh, world, mode, id, view); });
            it = it->second.empty() ? hits.erase(it) : std::next(it);
        }
    }

    if (uvIslandPick())
        for (auto& [id, ids] : hits)
            if (scene::MeshPtr mesh = scene::modifiers::cage(g, id))
                ids = viewportUvIslands(*mesh, ids);

    bool shift = mods & Qt::ShiftModifier, ctrl = mods & Qt::ControlModifier;
    if (click && hits.empty() && !shift && !ctrl && pickOtherObject(a))
        return;
    const char* flag = mode == ComponentMode::Vertex ? "vertices" : mode == ComponentMode::Edge ? "edges" : "faces";
    try
    {
        UndoChunk chunk("Select Components");
        bool first = true;
        if (hits.empty() && !shift && !ctrl)
            for (scene::NodeId id : hilited(mIsolation))
            {
                ops::execute("selectComponents", {{"args", {g.node(id)->path()}}, {flag, json::array()}}, CallSource::UI);
                break;
            }
        for (auto& [id, ids] : hits)
        {
            json call = {{"args", {g.node(id)->path()}}, {flag, ids}};
            if (shift && ctrl)
                call["add"] = true;
            else if (ctrl)
                call["deselect"] = true;
            else if (shift)
                call["toggle"] = true;
            else if (!first)
                call["add"] = true;
            first = false;
            call["symmetric"] = PrefStore::get().value<bool>("general", "/select/symmetric", true);
            ops::execute("selectComponents", call, CallSource::UI);
        }
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

bool ViewportWidget::pickOtherObject(QPointF at)
{
    std::vector<std::string> hits = hitTest(at, at);
    const scene::Node* n = hits.empty() ? nullptr : findNode(hits.front());
    if (!n)
        return false;
    const std::vector<scene::NodeId> live = hilited(mIsolation);
    if (std::ranges::find(live, n->id()) != live.end())
        return false;
    try
    {
        ops::execute("select", {{"args", {n->path()}}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    return true;
}

void ViewportWidget::drawObjectSymmetryEdges(render::FrameState& s)
{
    LILA_ZONE();

    const PrefStore& prefs = PrefStore::get();
    if (!viewportDisplay::highlightSelection() || !prefs.value<bool>("general", viewportDisplay::kSymmetryEdges, true) ||
        !prefs.value<bool>("general", viewportDisplay::kSymmetryEdgesObjectMode, true))
        return;
    scene::Graph& g = scene::Graph::current();
    if (g.selection().size() != 1)
        return;
    s.symmetryLineWidth = viewportDisplay::hairlineWidth(viewportDisplay::componentThickness().symmetryEdge, devicePixelRatioF());
    for (scene::NodeId id : g.selection())
    {
        scene::Node* n = g.node(id);
        if (!n || !n->type().isA("mesh") || !scene::effectivelyVisible(g, id) || !isolate::shows(&mIsolation, id))
            continue;
        if (auto sculpting = sculpt::active(); sculpting && sculpting->node == id && !sculptSettings().symmetryLine)
            continue;
        scene::MeshPtr mesh = scene::modifiers::cage(g, id);
        if (!mesh)
            continue;
        const ComponentShape shape(*n, *mesh, mesh, shownWorld(*n));
        const auto vis = isolate::visibility(&mIsolation, id, mesh);
        for (uint32_t e : symmetryPlaneEdges(*n, mesh))
            if (vis->edge(e))
                shape.forEachSegment(e, [&](const glm::dvec3& a, const glm::dvec3& b) {
                    s.symmetryLines.push_back({glm::dvec3(a), kSymmetryRed});
                    s.symmetryLines.push_back({glm::dvec3(b), kSymmetryRed});
                });
        radialPlaneLines(*n, *mesh, shownWorld(*n), s.lines);
    }
}

/**
 * A mesh's vertex dots in world space, kept until the mesh, its placement, smoothing, isolation, symmetry or the
 * component selection changes, so a dense mesh's dots are built once instead of every frame.
 */
static std::shared_ptr<const render::PointCloud> vertexDots(scene::NodeId id, const ComponentShape& shape, const scene::MeshPtr& mesh,
                                                            const std::shared_ptr<const scene::isolation::Visibility>& vis,
                                                            const geo::SymmetryMap* sym, const scene::components::Ids& selected)
{
    struct Entry
    {
        std::weak_ptr<const geo::Mesh> mesh;
        std::shared_ptr<const SmoothCage> smooth;
        std::shared_ptr<const scene::isolation::Visibility> vis;
        glm::dmat4 world;
        const geo::SymmetryMap* sym = nullptr;
        uint64_t serial = 0;
        std::shared_ptr<const render::PointCloud> cloud;
    };
    // Never destroyed: at exit the meshes' owners (extension DLLs among them) may already be unloaded.
    static auto& cache = *new std::unordered_map<scene::NodeId, Entry>();
    Entry& e = cache[id];
    const uint64_t serial = scene::components::serial();
    if (e.cloud && e.mesh.lock() == mesh && e.smooth == shape.smooth && e.vis == vis && e.world == shape.world && e.sym == sym && e.serial == serial)
        return e.cloud;
    LILA_ZONE_N("vertexDots");

    const geo::Mesh& m = *mesh;
    std::vector<uint8_t> on(m.pointCapacity(), render::PointDraw::Normal);
    if (sym)
        for (uint32_t p : sym->expandPoints(selected))
            if (p < on.size())
                on[p] = render::PointDraw::Mirror;
    for (uint32_t p : selected)
        if (p < on.size())
            on[p] = render::PointDraw::Selected;
    auto cloud = std::make_shared<render::PointCloud>();
    cloud->points.resize(m.pointCapacity());
    // Relative to the mesh's placement, so the dots stay exact far from the world origin.
    cloud->origin = glm::dvec3(shape.world[3]);
    tbb::parallel_for(tbb::blocked_range<geo::Index>(0, m.pointCapacity(), 16384), [&](const tbb::blocked_range<geo::Index>& r) {
        for (geo::Index p = r.begin(); p != r.end(); ++p)
        {
            const bool shown = m.pointValid(p) && vis->point(p);
            cloud->points[p] = {shown ? glm::vec3(shape.point(p) - cloud->origin) : glm::vec3(0.0f), shown ? uint32_t(on[p]) : uint32_t(render::PointDraw::Hidden)};
        }
    });
    // Dots of meshes that are gone are not needed again, and each holds a point per vertex.
    std::erase_if(cache, [&](const auto& kv) { return kv.first != id && kv.second.mesh.expired(); });
    if (cache.size() > 64)
        std::erase_if(cache, [&](const auto& kv) { return kv.first != id; });
    Entry& slot = cache[id];
    slot = {mesh, shape.smooth, vis, shape.world, sym, serial, std::move(cloud)};
    return slot.cloud;
}

void ViewportWidget::drawComponents(render::FrameState& s)
{
    LILA_ZONE();

    using scene::ComponentMode;
    ComponentMode mode = scene::components::mode();
    if (mode == ComponentMode::Object)
    {
        drawObjectSymmetryEdges(s);
        return;
    }
    scene::Graph& g = scene::Graph::current();
    std::vector<scene::NodeId> live = hilited(mIsolation);
    const ComponentPalette palette = componentPalette();
    const glm::vec4 dotLinear = palette.point, hotLinear = palette.selected, faceFillLinear = palette.selectedFill;
    const viewportDisplay::ComponentThickness thickness = viewportDisplay::componentThickness();
    const float wireWidth = viewportDisplay::hairlineWidth(thickness.edge, devicePixelRatioF());
    s.selectedEdgeWidth = viewportDisplay::hairlineWidth(thickness.edgeSelected, devicePixelRatioF());
    s.symmetryLineWidth = viewportDisplay::hairlineWidth(thickness.symmetryEdge, devicePixelRatioF());
    const bool showSymmetry = PrefStore::get().value<bool>("general", viewportDisplay::kSymmetryEdges, true);
    // The plane's edges only mark a lone mesh; across several they read as clutter.
    const bool showSymmetryPlane = showSymmetry && live.size() == 1;
    for (scene::NodeId id : live)
    {
        scene::Node* n = g.node(id);
        scene::MeshPtr mesh = scene::modifiers::cage(g, id);
        if (!mesh)
            continue;
        const geo::Mesh& m = *mesh;
        glm::dmat4 world = shownWorld(*n);
        // With modifiers on, the cage's wire shows what you're editing.
        if (mesh != n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
            if (auto rm = renderMeshFor(mesh))
            {
                render::MeshDraw cageDraw;
                cageDraw.mesh = rm;
                if (auto part = isolate::masked(&mIsolation, id, mesh))
                    cageDraw.mesh = part->mesh;
                cageDraw.world = world;
                // Depth tested against the result, as picking is: hidden cage edges stay hidden.
                cageDraw.flags = render::DrawWire | render::DrawWireDepth;
                cageDraw.wireColor = palette.wire;
                cageDraw.wireWidth = wireWidth;
                s.meshes.push_back(std::move(cageDraw));
            }
        const ComponentShape shape(*n, m, mesh, world);
        auto wp = [&](uint32_t p) { return glm::dvec3(shape.point(p)); };
        auto edgeLines = [&](uint32_t e, std::vector<render::LineVertex>& out, const glm::vec4& col) {
            shape.forEachSegment(e, [&](const glm::dvec3& a, const glm::dvec3& b) {
                out.push_back({glm::dvec3(a), col});
                out.push_back({glm::dvec3(b), col});
            });
        };
        auto faceLines = [&](uint32_t f, const glm::vec4& fill, const glm::vec4& line) {
            shape.forEachFillVertex(f, [&](const glm::dvec3& p) { s.tris3d.push_back({glm::dvec3(p), fill}); });
            shape.forEachOutline(f, [&](const glm::dvec3& a, const glm::dvec3& b) {
                s.linesOnTop.push_back({glm::dvec3(a), line});
                s.linesOnTop.push_back({glm::dvec3(b), line});
            });
        };
        const auto vis = isolate::visibility(&mIsolation, id, mesh);
        const auto& selFaces = scene::components::selected(id, ComponentMode::Face);
        const auto& selEdges = scene::components::selected(id, ComponentMode::Edge);
        const auto& selPoints = scene::components::selected(id, ComponentMode::Vertex);

        // Depth-tested so components behind the surface stay hidden. These streams take linear colors.
        if (mode == ComponentMode::Face)
            for (uint32_t f : selFaces)
                if (m.faceValid(f) && vis->face(f))
                    faceLines(f, faceFillLinear, hotLinear);
        if (mode == ComponentMode::Edge)
            for (uint32_t e : selEdges)
                if (m.edgeValid(e) && vis->edge(e))
                    edgeLines(e, s.selectedEdges, hotLinear);
        if (const geo::SymmetryMap* sym = viewSymmetry(*n, mesh))
        {
            const glm::vec4 ghost(glm::vec3(hotLinear), kMirrorAlpha);
            if (mode == ComponentMode::Face)
                for (uint32_t f : sym->expandFaces(m, selFaces))
                    if (f != geo::kInvalid && m.faceValid(f) && vis->face(f) && !std::ranges::binary_search(selFaces, f))
                        faceLines(f, glm::vec4(glm::vec3(faceFillLinear), faceFillLinear.a * kMirrorAlpha), ghost);
            if (mode == ComponentMode::Edge)
                for (uint32_t e : sym->expandEdges(m, selEdges))
                    if (e != geo::kInvalid && m.edgeValid(e) && vis->edge(e) && !std::ranges::binary_search(selEdges, e))
                        edgeLines(e, s.selectedEdges, ghost);
        }
        if (showSymmetry)
        {
            if (showSymmetryPlane)
            {
                for (uint32_t e : symmetryPlaneEdges(*n, mesh))
                    if (vis->edge(e) && !(mode == ComponentMode::Edge && std::ranges::binary_search(selEdges, e)))
                        edgeLines(e, s.symmetryLines, kSymmetryRed);
                radialPlaneLines(*n, m, world, s.lines);
            }
            // Radial symmetry's mismatch: the points without a full orbit, marked where they are.
            if (const geo::SymmetryMap* sym = viewSymmetry(*n, mesh); sym && sym->isRadial())
            {
                const ViewInfo view = viewInfo();
                const glm::dvec3 right(view.view[0][0], view.view[1][0], view.view[2][0]), up(view.view[0][1], view.view[1][1], view.view[2][1]);
                const glm::dvec3 fwd = -glm::dvec3(view.view[0][2], view.view[1][2], view.view[2][2]);
                const double atUnit = view.unitsPerPixel(view.eye + fwd);
                for (geo::Index p : sym->unmatchedPoints(m))
                {
                    if (!vis->point(p))
                        continue;
                    const glm::dvec3 c(wp(p));
                    const double depth = glm::dot(c - view.eye, fwd);
                    if (depth <= 0.0)
                        continue;
                    const double half = 0.5 * thickness.vertexSelected * atUnit * (view.ortho ? 1.0 : depth);
                    const glm::dvec3 r = right * half, u = up * half;
                    const glm::dvec3 a(c - r - u), b(c + r - u), cc(c + r + u), d(c - r + u);
                    for (const glm::dvec3& v : {a, b, cc, a, cc, d})
                        s.tris3d.push_back({v, kSymmetryRed});
                }
            }
        }
        bool soft = PrefStore::get().value<bool>("general", "/softSelect/enabled", false);
        if (mode == ComponentMode::Vertex && !soft)
        {
            render::PointDraw dots;
            dots.cloud = vertexDots(id, shape, mesh, vis, viewSymmetry(*n, mesh), selPoints);
            dots.colors[render::PointDraw::Normal] = dotLinear;
            dots.colors[render::PointDraw::Selected] = hotLinear;
            dots.colors[render::PointDraw::Mirror] = glm::mix(dotLinear, hotLinear, kMirrorAlpha);
            dots.size = float(thickness.vertex * devicePixelRatioF());
            dots.selectedSize = float(thickness.vertexSelected * devicePixelRatioF());
            s.points.push_back(std::move(dots));
            continue;
        }
        std::vector<float> weights;
        const float emphasis = float(std::clamp(
            PrefStore::get().value<double>("general", viewportDisplay::kSoftSelectEmphasis, viewportDisplay::kSoftSelectEmphasisDefault), 0.0, 1.0));
        if (soft)
        {
            const auto& sel = scene::components::selected(id, mode);
            weights = scene::softWeights(m, world, scene::components::convert(m, mode, sel, ComponentMode::Vertex));
            const bool gray = viewportDisplay::grayscaleWeights();
            std::vector<render::LineVertex>& softEdges = emphasis > 0.5f ? s.selectedEdges : s.linesOnTop;
            auto edgeColor = [&](float w) { return w > 0.0f ? glm::mix(palette.wire, viewportDisplay::falloffColor(w, gray), emphasis) : palette.wire; };
            for (uint32_t e = 0; emphasis > 0.0f && e < m.edgeCapacity(); ++e)
            {
                if (!m.edgeValid(e) || !vis->edge(e) || (mode == ComponentMode::Edge && std::ranges::binary_search(selEdges, e)))
                    continue;
                const uint32_t a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
                const float wa = a < weights.size() ? weights[a] : 0.0f, wb = b < weights.size() ? weights[b] : 0.0f;
                if (wa <= 0.0f && wb <= 0.0f)
                    continue;
                softEdges.push_back({wp(a), edgeColor(wa)});
                softEdges.push_back({wp(b), edgeColor(wb)});
            }
        }
        const bool edgeFaceVertices =
            mode != ComponentMode::Vertex && PrefStore::get().value<bool>("general", viewportDisplay::kVerticesInEdgeFaceModes, false);
        if (mode == ComponentMode::Vertex || soft || edgeFaceVertices)
        {
            // 1 selected, 2 the mirror of a selected point.
            std::vector<char> on(m.pointCapacity(), 0);
            if (mode == ComponentMode::Vertex)
            {
                if (const geo::SymmetryMap* sym = viewSymmetry(*n, mesh))
                    for (uint32_t p : sym->expandPoints(selPoints))
                        if (p < on.size())
                            on[p] = 2;
                for (uint32_t p : selPoints)
                    if (p < on.size())
                        on[p] = 1;
            }
            else if (edgeFaceVertices)
                for (uint32_t p : scene::components::convert(m, mode, scene::components::selected(id, mode), ComponentMode::Vertex))
                    if (p < on.size())
                        on[p] = 1;
            // Camera-facing squares a few pixels wide; size scales with depth so they stay constant on screen.
            ViewInfo view = viewInfo();
            glm::dvec3 right(view.view[0][0], view.view[1][0], view.view[2][0]), up(view.view[0][1], view.view[1][1], view.view[2][1]);
            glm::dvec3 fwd = -glm::dvec3(view.view[0][2], view.view[1][2], view.view[2][2]);
            // Measured one unit ahead of the eye: the mesh centre can sit behind the camera up close.
            const double atUnit = view.unitsPerPixel(view.eye + fwd);
            const bool grayWeights = viewportDisplay::grayscaleWeights();
            for (uint32_t p = 0; p < m.pointCapacity(); ++p)
            {
                if (!m.pointValid(p) || !vis->point(p))
                    continue;
                glm::dvec3 c(wp(p));
                double depth = glm::dot(c - view.eye, fwd);
                if (depth <= 0.0)
                    continue;
                float wgt = p < weights.size() ? weights[p] : 0.0f;
                // Outside vertex mode only the falloff shows, as dots shaded by weight, and the selection's own vertices by pref.
                if (mode != ComponentMode::Vertex && (wgt >= 1.0f ? !edgeFaceVertices : (wgt <= 0.0f && on[p] != 1)))
                    continue;
                const double size = on[p] == 1 ? thickness.vertexSelected : thickness.vertex + (thickness.vertexSelected - thickness.vertex) * wgt * emphasis;
                double half = 0.5 * size * atUnit * (view.ortho ? 1.0 : depth);
                glm::dvec3 r = right * half, u = up * half;
                glm::vec4 col = on[p] == 1 ? hotLinear
                                : wgt > 0.0f ? viewportDisplay::falloffColor(wgt, grayWeights)
                                : on[p] == 2 ? glm::mix(dotLinear, hotLinear, kMirrorAlpha)
                                             : dotLinear;
                glm::dvec3 a(c - r - u), b(c + r - u), cc(c + r + u), d(c - r + u);
                for (const glm::dvec3& v : {a, b, cc, a, cc, d})
                    s.tris3d.push_back({v, col});
            }
        }
    }
}

void ViewportWidget::queuePreselect(QPointF at)
{
    mPreselectAt = at;
    if (mPreselectQueued)
        return;
    mPreselectQueued = true;
    // A slow pick (dense mesh) waits out its own cost, so hovering never takes more than half the main thread.
    QTimer::singleShot(int(std::min(50.0, mPreselectCostMs)), this, [this] {
        mPreselectQueued = false;
        updatePreselect();
    });
}

void ViewportWidget::updatePreselect()
{
    LILA_ZONE();

    if (!mPreselectAt)
        return;
    QPointF at = *mPreselectAt;
    mPreselectAt.reset();
    using scene::ComponentMode;
    ComponentMode mode = scene::components::mode();
    const char* pref = mode == ComponentMode::Object ? "/viewport/preselect/objects" : "/viewport/preselect/components";
    scene::componentPick::Hover next{mode};
    QElapsedTimer timer;
    timer.start();
    Gizmo* gz = currentGizmo();
    bool busy = (gz && (gz->hovering() || gz->dragging())) || viewportTools::active() || mNav != Nav::None || mSelecting;
    const selectionHandlers::Handler* handler = mode == ComponentMode::Object && !busy ? selectionHandlers::active() : nullptr;
    std::optional<selectionHandlers::Highlight> highlight;
    if (handler && handler->hover)
    {
        selectionHandlers::Hit hit = selectionHit(at);
        try
        {
            highlight = handler->hover(hit);
        }
        catch (const std::exception& e)
        {
            log::error("selection handler {}: {}", handler->id, e.what());
        }
        if (PrefStore::get().value<bool>("general", pref, false))
            next.node = hit.node;
    }
    else if (!busy && PrefStore::get().value<bool>("general", pref, mode != ComponentMode::Object))
    {
        if (mode != ComponentMode::Object)
            next = componentAt(at);
        else if (std::vector<std::string> names = hitTest(at, at); !names.empty())
            if (scene::Node* n = findNode(names.front()))
                next.node = n->id();
    }
    bool islandChanged = false;
    if (!uvIslandPick() || next.empty())
    {
        islandChanged = !mPreselectIsland.empty();
        mPreselectIsland.clear();
    }
    else if (next != mPreselect || mPreselectIsland.empty())
        if (scene::MeshPtr mesh = scene::modifiers::cage(scene::Graph::current(), next.node))
        {
            const uint32_t seed[] = {next.id};
            mPreselectIsland = viewportUvIslands(*mesh, seed);
            islandChanged = true;
        }
    mPreselectCostMs = double(timer.nsecsElapsed()) / 1e6;
    const bool highlightChanged = !sameHighlight(highlight, mHandlerHover);
    mHandlerHover = std::move(highlight);
    if (next == mPreselect && !highlightChanged && !islandChanged)
        return;
    mPreselect = next;
    requestFrame();
}

void ViewportWidget::clearPreselect()
{
    mPreselectAt.reset();
    if (mPreselect.empty() && !mHandlerHover)
        return;
    mPreselect = {};
    mPreselectIsland.clear();
    mHandlerHover.reset();
    requestFrame();
}

selectionHandlers::Hit ViewportWidget::selectionHit(QPointF px)
{
    selectionHandlers::Hit hit;
    hit.px = px;
    std::vector<std::string> names = hitTest(px, px);
    if (names.empty())
        return hit;
    hit.name = names.front();
    scene::Node* n = findNode(hit.name);
    if (!n)
        return hit;
    hit.node = n->id();
    if (!n->type().isA("mesh") || overridden(*n))
        return hit;
    scene::MeshPtr mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    auto rm = renderMeshFor(mesh);
    auto pd = pickDataFor(mesh);
    if (!rm || !pd)
        return hit;
    const glm::dmat4 world = shownWorld(*n);
    glm::dvec3 o, d;
    viewInfo().ray(px, o, d);
    std::optional<RayHit> ray = isolate::raycast(&mIsolation, hit.node, mesh, world, o, d);
    if (!ray || ray->triangle >= pd->triangleFaces.size() || 3 * size_t(ray->triangle) + 2 >= pd->indices.size())
        return hit;
    hit.face = pd->triangleFaces[ray->triangle];
    hit.position = o + d * ray->distance;
    double nearest = 1e300;
    for (int k = 0; k < 3; ++k)
    {
        uint32_t v = pd->indices[3 * size_t(ray->triangle) + k];
        if (v >= pd->positions.size() || v >= pd->vertexPoint.size())
            continue;
        double dist = glm::length(glm::dvec3(world * glm::dvec4(glm::dvec3(pd->positions[v]), 1.0)) - hit.position);
        if (dist < nearest)
        {
            nearest = dist;
            hit.point = pd->vertexPoint[v];
        }
    }
    return hit;
}

void ViewportWidget::drawHandlerHover(render::FrameState& s)
{
    if (!mHandlerHover || scene::components::mode() != scene::ComponentMode::Object)
        return;
    using Domain = selectionHandlers::Highlight::Domain;
    const selectionHandlers::Highlight& h = *mHandlerHover;
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.node(h.mesh);
    if (!n || !n->type().isA("mesh"))
        return;
    scene::MeshPtr mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    if (!mesh)
        return;
    const geo::Mesh& m = *mesh;
    const glm::dmat4 world = shownWorld(*n);
    const glm::vec4 color = srgbToLinear(h.color);
    auto weight = [&](uint32_t i) { return i < h.weights.size() ? std::clamp(h.weights[i], 0.0f, 1.0f) : 0.0f; };
    if (!mIsolation.shows(h.mesh))
        return;
    const auto vis = isolate::visibility(&mIsolation, h.mesh, mesh);
    for (uint32_t f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f) || !vis->face(f) || (h.domain == Domain::Face && weight(f) <= 0.0f))
            continue;
        std::vector<uint32_t> tris = scene::componentPick::fillTriangles(m, f);
        for (size_t t = 0; t + 2 < tris.size(); t += 3)
        {
            float w[3];
            for (int k = 0; k < 3; ++k)
                w[k] = h.domain == Domain::Face ? weight(f) : weight(tris[t + k]);
            if (w[0] <= 0.0f && w[1] <= 0.0f && w[2] <= 0.0f)
                continue;
            for (int k = 0; k < 3; ++k)
                s.tris3d.push_back({glm::dvec3(world * glm::dvec4(m.position(tris[t + k]), 1.0)), glm::vec4(glm::vec3(color), color.a * w[k])});
        }
    }
}

namespace
{
/** Linear color of a topology callout, or nullopt when its pref is off or it is plain. */
std::optional<glm::vec4> topologyTint(scene::componentPick::Topology t)
{
    namespace cp = scene::componentPick;
    if (t == cp::Topology::Plain || !PrefStore::get().value<bool>("general", cp::topologyEnabledKey(t), true))
        return std::nullopt;
    QColor c(QString::fromStdString(PrefStore::get().value<std::string>("general", cp::topologyColorKey(t), cp::topologyDefaultColor(t))));
    if (!c.isValid())
        c = QColor(cp::topologyDefaultColor(t));
    auto lin = [](double v) { return float(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4)); };
    return glm::vec4(lin(c.redF()), lin(c.greenF()), lin(c.blueF()), 1.0f);
}
} // namespace

void ViewportWidget::drawPreselect(render::FrameState& s)
{
    using scene::ComponentMode;
    ComponentMode mode = scene::components::mode();
    if (mPreselect.empty() || mPreselect.mode != mode || mode == ComponentMode::Object)
        return;
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.node(mPreselect.node);
    scene::MeshPtr mesh = n ? scene::modifiers::cage(g, mPreselect.node) : nullptr;
    if (!mesh)
        return;
    const geo::Mesh& m = *mesh;
    uint32_t id = mPreselect.id;
    if ((mode == ComponentMode::Vertex && !m.pointValid(id)) || (mode == ComponentMode::Edge && !m.edgeValid(id)) ||
        (mode == ComponentMode::Face && !m.faceValid(id)))
        return;
    const ComponentShape shape(*n, m, mesh, shownWorld(*n));
    auto wp = [&](uint32_t p) { return shape.point(p); };
    ViewInfo view = viewInfo();
    glm::dvec3 right(view.view[0][0], view.view[1][0], view.view[2][0]), up(view.view[0][1], view.view[1][1], view.view[2][1]);
    glm::dvec3 fwd = -glm::dvec3(view.view[0][2], view.view[1][2], view.view[2][2]);
    // Thicker than the selection, so hovered and selected read apart.
    const ComponentPalette palette = componentPalette();
    glm::vec4 hot = palette.hover, faceFill = palette.hoverFill;
    const viewportDisplay::ComponentThickness thickness = viewportDisplay::componentThickness();
    namespace cp = scene::componentPick;
    cp::Topology topo = mode == ComponentMode::Face ? cp::faceTopology(m, id) : mode == ComponentMode::Vertex ? cp::pointTopology(m, id) : cp::Topology::Plain;
    if (std::optional<glm::vec4> tint = topologyTint(topo))
    {
        hot = *tint;
        faceFill = glm::vec4(glm::vec3(*tint), 0.3f);
    }
    auto quad = [&](glm::dvec3 a, glm::dvec3 b, glm::dvec3 c, glm::dvec3 d, const glm::vec4& col) {
        for (const glm::dvec3& v : {a, b, c, a, c, d})
            s.tris3d.push_back({glm::dvec3(v), col});
    };
    // Scaled by depth from one unit ahead, not measured at the point: a hovered face can reach behind the camera.
    const double atUnit = view.unitsPerPixel(view.eye + fwd);
    const double nearDepth = view.ortho ? -std::numeric_limits<double>::infinity() : view.proj[3][2] / (view.proj[2][2] + 1.0);
    auto depthOf = [&](const glm::dvec3& p) { return glm::dot(p - view.eye, fwd); };
    auto unitsAt = [&](const glm::dvec3& p) { return atUnit * (view.ortho ? 1.0 : depthOf(p)); };
    // Screen-constant width, facing the camera, cut at the near plane.
    auto ribbon = [&](glm::dvec3 p0, glm::dvec3 p1, double halfPx, const glm::vec4& col) {
        const double d0 = depthOf(p0), d1 = depthOf(p1);
        if (d0 < nearDepth && d1 < nearDepth)
            return;
        if (d0 < nearDepth)
            p0 = glm::mix(p0, p1, (nearDepth - d0) / (d1 - d0));
        else if (d1 < nearDepth)
            p1 = glm::mix(p1, p0, (nearDepth - d1) / (d0 - d1));
        glm::dvec3 side = glm::cross(p1 - p0, fwd);
        if (glm::length(side) < 1e-12)
            return;
        side = glm::normalize(side);
        glm::dvec3 s0 = side * (halfPx * unitsAt(p0)), s1 = side * (halfPx * unitsAt(p1));
        quad(p0 - s0, p1 - s1, p1 + s1, p0 + s0, col);
    };
    auto square = [&](glm::dvec3 c, double halfPx, const glm::vec4& col) {
        if (depthOf(c) < nearDepth)
            return;
        double half = halfPx * unitsAt(c);
        glm::dvec3 r = right * half, u = up * half;
        quad(c - r - u, c + r - u, c + r + u, c - r + u, col);
    };
    auto component = [&](uint32_t id, float alpha) {
        const glm::vec4 col(glm::vec3(hot), hot.a * alpha), fill(glm::vec3(faceFill), faceFill.a * alpha);
        if (mode == ComponentMode::Vertex)
            square(wp(id), 0.5 * thickness.vertexHover, col);
        else if (mode == ComponentMode::Edge)
            shape.forEachSegment(id, [&](const glm::dvec3& a, const glm::dvec3& b) { ribbon(a, b, 0.5 * thickness.edgeHover, col); });
        else
        {
            shape.forEachFillVertex(id, [&](const glm::dvec3& p) { s.tris3d.push_back({glm::dvec3(p), fill}); });
            shape.forEachOutline(id, [&](const glm::dvec3& a, const glm::dvec3& b) { ribbon(a, b, 1.0, col); });
        }
    };
    if (const geo::SymmetryMap* sym = viewSymmetry(*n, mesh))
    {
        const auto vis = isolate::visibility(&mIsolation, mPreselect.node, mesh);
        std::vector<uint32_t> drawn{id};
        for (int k = 1; k < sym->order(); ++k)
        {
            const uint32_t mirror = mode == ComponentMode::Vertex ? sym->image(k, id)
                                    : mode == ComponentMode::Edge  ? sym->imageEdge(m, k, id)
                                                                   : sym->imageFace(m, k, id);
            if (mirror == geo::kInvalid || std::ranges::count(drawn, mirror))
                continue;
            const bool shown = mode == ComponentMode::Vertex ? m.pointValid(mirror) && vis->point(mirror)
                               : mode == ComponentMode::Edge ? m.edgeValid(mirror) && vis->edge(mirror)
                                                             : m.faceValid(mirror) && vis->face(mirror);
            if (shown)
                component(mirror, kMirrorAlpha);
            drawn.push_back(mirror);
        }
    }
    if (!mPreselectIsland.empty() && uvIslandPick())
    {
        const auto vis = isolate::visibility(&mIsolation, mPreselect.node, mesh);
        for (uint32_t f : mPreselectIsland)
            if (f != id && m.faceValid(f) && vis->face(f))
                component(f, 1.0f);
    }
    component(id, 1.0f);
    // Edge hover calls out the poles at its ends.
    if (mode == ComponentMode::Edge)
        if (std::optional<glm::vec4> pole = topologyTint(cp::Topology::Pole))
            for (int k = 0; k < 2; ++k)
                if (cp::pointTopology(m, m.edgePoint(id, k)) == cp::Topology::Pole)
                    square(wp(m.edgePoint(id, k)), 3.5, *pole);
}

bool ViewportWidget::touchesSelectRegion(const QRectF& box, const std::vector<glm::dvec2>& shape) const
{
    if (mLassoPick)
        return geo::lasso::touches(mLasso, shape);
    return geo::lasso::touches({{box.left(), box.top()}, {box.right(), box.top()}, {box.right(), box.bottom()}, {box.left(), box.bottom()}}, shape);
}

bool ViewportWidget::inSelectRegion(const QRectF& box, QPointF p) const
{
    return mLassoPick ? geo::lasso::contains(mLasso, {p.x(), p.y()}) : box.contains(p);
}

void ViewportWidget::pickLasso(Qt::KeyboardModifiers mods)
{
    auto [lo, hi] = geo::lasso::bounds(mLasso);
    if (mLasso.size() < 3 || (hi.x - lo.x < 4 && hi.y - lo.y < 4))
    {
        pick(mPressPos, mPressPos, mods);
        return;
    }
    mLassoPick = true;
    pick(QPointF(lo.x, lo.y), QPointF(hi.x, hi.y), mods);
    mLassoPick = false;
}

std::optional<bool> ViewportWidget::selectedUnder(QPointF at)
{
    scene::Graph& g = scene::Graph::current();
    scene::ComponentMode mode = scene::components::mode();
    if (mode != scene::ComponentMode::Object)
    {
        scene::componentPick::Hover h = componentAt(at);
        if (h.empty())
            return std::nullopt;
        const scene::components::Ids& sel = scene::components::selected(h.node, mode);
        return std::ranges::find(sel, h.id) != sel.end();
    }
    std::vector<std::string> hits = hitTest(at, at);
    const scene::Node* n = hits.empty() ? nullptr : findNode(hits.front());
    if (!n)
        return std::nullopt;
    return std::ranges::find(g.selection(), n->id()) != g.selection().end();
}

void ViewportWidget::updateTabHover(QPointF at)
{
    std::optional<bool> under = rect().contains(at.toPoint()) ? selectedUnder(at) : std::nullopt;
    if (under != mTabUnder)
        requestFrame();
    mTabUnder = under;
}

void ViewportWidget::pick(QPointF a, QPointF b, Qt::KeyboardModifiers mods)
{
    if (scene::components::mode() != scene::ComponentMode::Object)
    {
        pickComponents(a, b, mods);
        return;
    }
    json hits = hitTest(a, b);
    if (selectionHandlers::anyRedirect())
    {
        QRectF box = QRectF(a, b).normalized();
        std::vector<selectionHandlers::Hit> asked;
        if (box.width() < 4 && box.height() < 4)
        {
            if (!hits.empty())
                asked.push_back(selectionHit(a));
        }
        else
            for (const std::string& name : hits.get<std::vector<std::string>>())
            {
                selectionHandlers::Hit h;
                h.name = name;
                if (scene::Node* n = findNode(name))
                    h.node = n->id();
                h.px = (a + b) * 0.5;
                h.click = false;
                asked.push_back(std::move(h));
            }
        hits = selectionHandlers::redirect(asked);
    }

    // Maya: Shift toggles, Ctrl removes, Ctrl+Shift adds.
    bool shift = mods & Qt::ShiftModifier, ctrl = mods & Qt::ControlModifier;
    json call = {{"args", hits}};
    if (shift && ctrl)
        call["add"] = true;
    else if (ctrl)
        call["deselect"] = true;
    else if (shift)
        call["toggle"] = true;
    else if (hits.empty())
        call = {{"clear", true}};
    if (hits.empty() && (shift || ctrl))
        return;
    try
    {
        ops::execute("select", call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

} // namespace lila::ui
