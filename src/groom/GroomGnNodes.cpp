#include "groom/GroomGnNodes.h"

#include "core/Log.h"
#include "gn/NodesCommon.h"
#include "groom/GroomLabBridge.h"
#include "scene/Anim.h"

#include <groomlab/graph_io.h>
#include <groomlab/groom.h>
#include <groomlab/guide_set.h>
#include <groomlab/node_catalog.h>
#include <groomlab/operators.h>
#include <groomlab/scalp_mesh.h>
#include <groomlab/strand_set.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <regex>
#include <set>

namespace lila::groom
{
namespace groomGn
{
using namespace gn::nodes;
using gn::ExecContext;
using gn::ExecError;
using gn::Geometry;
using gn::GeometryPtr;
using gn::SocketType;
using json = nlohmann::json;

/** Curve attributes that carry a strand's root on the scalp between groom nodes. */
constexpr const char* kRootFace = "groomRootFace";
constexpr const char* kRootUv = "groomRootUV";
/** A strand's 64-bit ID in two halves ("id" holds its low bits for display): what seeds its randomness downstream. */
constexpr const char* kIdLow = "groomIdLow";
constexpr const char* kIdHigh = "groomIdHigh";

/** Spline data and what the conversion writes itself: never strand channels. */
const std::set<std::string, std::less<>>& reservedAttrs()
{
    static const std::set<std::string, std::less<>> names{"width", "id", kRootFace, kRootUv, kIdLow, kIdHigh, "curve_type", "degree", "resolution",
                                                          "handle_left", "handle_right", "handle_type_left", "handle_type_right",
                                                          "weight", "tilt", "radius", "cornerRadius"};
    return names;
}

/** Node types that read the host's inputs or keep frames of their own: no single node's work on what it is handed. */
bool hostBound(const std::string& type)
{
    static const std::set<std::string> types{"Output", "Input", "Compound", "Script", "Reference", "Scalp", "Guides",
                                             "Jiggle", "Simulate", "GuideCache", "ColliderMesh", "Sculpt"};
    return types.count(type) != 0;
}

/** GroomLab's text without its asides about other hosts ("(Maya's groom outMesh)"). */
std::string hostFree(std::string text)
{
    static const std::regex aside(R"(\s*\([^()]*\b(Maya|Blender|Houdini)\b[^()]*\))");
    return std::regex_replace(text, aside, "");
}

bool geometryPort(groomlab::PortType t)
{
    return t == groomlab::PortType::Strands || t == groomlab::PortType::Guides || t == groomlab::PortType::Mesh ||
           t == groomlab::PortType::Curves;
}

/** The socket of a node that has no scalp input of its own's: the scalp it measures and roots on. */
constexpr const char* kScalpSocket = "scalp";

/** The scalp a node reads: its first mesh input with faces, else its scalp socket's. */
std::shared_ptr<const groomlab::ScalpMesh> scalpOf(const ExecContext& c, const std::vector<groomlab::PortSpec>& ports, bool scalpSocket)
{
    for (const groomlab::PortSpec& p : ports)
        if (p.type == groomlab::PortType::Mesh)
            if (const GeometryPtr g = c.geometry(p.name); g->mesh && g->mesh->numFaces() > 0)
                return groomLabBridge::scalpMesh(*g->mesh);
    if (scalpSocket)
        if (const GeometryPtr g = c.geometry(kScalpSocket); g->mesh && g->mesh->numFaces() > 0)
            return groomLabBridge::scalpMesh(*g->mesh);
    return nullptr;
}

const gn::CurveSet* polyCurves(const Geometry& g, gn::CurveSet& storage)
{
    if (!g.curves)
        return nullptr;
    if (g.curves->isPoly())
        return g.curves.get();
    storage = g.curves->evaluated();
    return &storage;
}

std::uint64_t curveId(const gn::CurveSet& curves, size_t c)
{
    const auto low = curves.curveAttrs.get<int32_t>(kIdLow), high = curves.curveAttrs.get<int32_t>(kIdHigh);
    if (low.size() > c && high.size() > c)
        return std::uint64_t(std::uint32_t(high[c])) << 32 | std::uint32_t(low[c]);
    if (const auto ids = curves.curveAttrs.get<int32_t>("id"); ids.size() > c)
        return static_cast<std::uint64_t>(std::max(ids[c], 0));
    if (const auto ids = curves.curveAttrs.get<float>("id"); ids.size() > c)
        return static_cast<std::uint64_t>(std::max(ids[c], 0.0f));
    return c + 1;
}

/** The curves as strands: roots from the root attributes when they fit the scalp, else the closest point to each first point. */
groomlab::StrandsPtr toStrands(const Geometry& g, const groomlab::ScalpMesh* scalp)
{
    auto out = std::make_shared<groomlab::StrandSet>();
    gn::CurveSet storage;
    const gn::CurveSet* curves = polyCurves(g, storage);
    if (!curves)
        return out;
    const auto widths = curves->pointAttrs.get<float>("width");
    const auto faces = curves->curveAttrs.get<int32_t>(kRootFace);
    const auto uvs = curves->curveAttrs.get<glm::vec2>(kRootUv);
    const int32_t triangles = scalp ? static_cast<int32_t>(scalp->triangles().size()) : 0;
    std::vector<groomlab::Vec3f> points;
    std::vector<float> w;
    for (size_t c = 0; c < curves->numCurves(); ++c)
    {
        points.clear();
        w.clear();
        for (geo::Index p = curves->offsets[c]; p < curves->offsets[c + 1]; ++p)
        {
            points.push_back(groomLabBridge::toVec(curves->positions[p]));
            if (widths.size() == curves->numPoints())
                w.push_back(widths[p]);
        }
        groomlab::StrandRoot root;
        if (faces.size() > c && uvs.size() > c && faces[c] >= 0 && faces[c] < triangles)
            root = {faces[c], uvs[c].x, uvs[c].y};
        else if (scalp && !points.empty() && triangles > 0)
            root = scalp->closestPoint(points.front()).root;
        out->addStrand(curveId(*curves, c), points, w, root);
    }
    // Other float attributes ride along as the strands' channels.
    for (const std::string& name : curves->pointAttrs.names())
        if (!reservedAttrs().count(name))
            if (const auto values = curves->pointAttrs.get<float>(name); values.size() == curves->numPoints())
                out->addPointChannel(name).assign(values.begin(), values.end());
    for (const std::string& name : curves->curveAttrs.names())
        if (!reservedAttrs().count(name))
            if (const auto values = curves->curveAttrs.get<float>(name); values.size() == curves->numCurves())
                out->addStrandChannel(name).assign(values.begin(), values.end());
    return out;
}

groomlab::GuidesPtr toGuides(const Geometry& g, const groomlab::ScalpMesh* scalp)
{
    auto out = std::make_shared<groomlab::GuideSet>();
    gn::CurveSet storage;
    if (const gn::CurveSet* curves = polyCurves(g, storage))
        for (size_t c = 0; c < curves->numCurves(); ++c)
        {
            if (curves->curveSize(c) < 2)
                continue;
            std::vector<groomlab::Vec3f> points;
            for (const glm::dvec3& p : curves->curvePoints(c))
                points.push_back(groomLabBridge::toVec(p));
            out->add(curveId(*curves, c), std::move(points));
        }
    if (scalp && !scalp->triangles().empty())
        out->rootOnMesh(*scalp);
    return out;
}

GeometryPtr fromStrands(const groomlab::StrandSet& strands)
{
    gn::CurveSet curves;
    std::vector<glm::dvec3> points;
    for (size_t s = 0; s < strands.strandCount(); ++s)
    {
        const groomlab::Vec3f* p = strands.strandPoints(s);
        points.assign(strands.strandPointCount(s), {});
        for (size_t k = 0; k < points.size(); ++k)
            points[k] = {p[k].x, p[k].y, p[k].z};
        curves.addCurve(points, false);
    }
    curves.syncSizes();
    if (strands.widths().size() == strands.pointCount())
    {
        const auto w = curves.pointAttrs.getOrAdd<float>("width");
        std::copy(strands.widths().begin(), strands.widths().end(), w.begin());
    }
    const auto ids = curves.curveAttrs.getOrAdd<int32_t>("id");
    const auto low = curves.curveAttrs.getOrAdd<int32_t>(kIdLow);
    const auto high = curves.curveAttrs.getOrAdd<int32_t>(kIdHigh);
    const auto faces = curves.curveAttrs.getOrAdd<int32_t>(kRootFace);
    const auto uvs = curves.curveAttrs.getOrAdd<glm::vec2>(kRootUv);
    for (size_t s = 0; s < strands.strandCount(); ++s)
    {
        const std::uint64_t id = strands.ids()[s];
        ids[s] = static_cast<int32_t>(id);
        low[s] = static_cast<int32_t>(std::uint32_t(id));
        high[s] = static_cast<int32_t>(std::uint32_t(id >> 32));
        const groomlab::StrandRoot& r = strands.roots()[s];
        faces[s] = r.face;
        uvs[s] = {r.u, r.v};
    }
    for (const std::string& name : strands.pointChannelNames())
        if (const std::vector<float>* values = strands.pointChannel(name); values && values->size() == curves.numPoints())
            std::copy(values->begin(), values->end(), curves.pointAttrs.getOrAdd<float>(name).begin());
    for (const std::string& name : strands.strandChannelNames())
        if (const std::vector<float>* values = strands.strandChannel(name); values && values->size() == curves.numCurves())
            std::copy(values->begin(), values->end(), curves.curveAttrs.getOrAdd<float>(name).begin());
    return gn::curveGeometry(std::move(curves));
}

GeometryPtr fromGuides(const groomlab::GuideSet& guides)
{
    gn::CurveSet curves;
    std::vector<glm::dvec3> points;
    for (const groomlab::Guide& guide : guides.guides())
    {
        points.clear();
        for (const groomlab::Vec3f& p : guide.points)
            points.push_back({p.x, p.y, p.z});
        curves.addCurve(points, false);
    }
    curves.syncSizes();
    const auto ids = curves.curveAttrs.getOrAdd<int32_t>("id");
    const auto low = curves.curveAttrs.getOrAdd<int32_t>(kIdLow);
    const auto high = curves.curveAttrs.getOrAdd<int32_t>(kIdHigh);
    for (size_t c = 0; c < guides.size(); ++c)
    {
        const std::uint64_t id = guides.guides()[c].id;
        ids[c] = static_cast<int32_t>(id);
        low[c] = static_cast<int32_t>(std::uint32_t(id));
        high[c] = static_cast<int32_t>(std::uint32_t(id >> 32));
    }
    return gn::curveGeometry(std::move(curves));
}

GeometryPtr fromScalp(const groomlab::ScalpMesh& scalp)
{
    geo::Mesh mesh;
    for (const groomlab::Vec3f& p : scalp.points())
        mesh.addPoint({p.x, p.y, p.z});
    for (const groomlab::ScalpMesh::Triangle& t : scalp.triangles())
        mesh.addFace({geo::Index(t[0]), geo::Index(t[1]), geo::Index(t[2])});
    return gn::meshGeometry(std::move(mesh));
}

GeometryPtr toGeometry(const groomlab::Value& v)
{
    if (const auto strands = groomlab::valueAs<groomlab::StrandsPtr>(v))
        return fromStrands(*strands);
    if (const auto guides = groomlab::valueAs<groomlab::GuidesPtr>(v))
        return fromGuides(*guides);
    if (const auto mesh = groomlab::valueAs<groomlab::MeshPtr>(v))
        return fromScalp(*mesh);
    return gn::makeGeometry(Geometry{});
}

/** A param shown as a socket or a choice; ramps and maps go through the node's params text. */
struct ParamSocket
{
    std::string name;
    SocketType type = SocketType::Float;
    bool choice = false;
};

/** A node's sockets and props from its operator: geometry inputs and outputs, its own params, and the time it reads. */
struct Wrapped
{
    std::vector<groomlab::PortSpec> inputs, outputs;
    std::vector<ParamSocket> params;
    bool readsTime = false;
    /** The operator takes no mesh: the node has a scalp socket of its own. */
    bool scalpSocket = false;
};

std::optional<gn::NodeDecl> declFor(const std::string& type)
{
    if (hostBound(type))
        return std::nullopt;
    const groomlab::NodeInfo* info = groomlab::nodeInfo(type);
    if (info && info->hidden)
        return std::nullopt;
    std::unique_ptr<groomlab::Operator> op;
    try
    {
        op = groomlab::OperatorRegistry::instance().create(type);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
    if (!op)
        return std::nullopt;
    op->applyDefaults();
    auto wrapped = std::make_shared<Wrapped>();
    for (const groomlab::PortSpec& p : op->inputs())
    {
        if (geometryPort(p.type))
            wrapped->inputs.push_back(p);
        else if (!p.optional)
            return std::nullopt; // a field, curve or value it can't run without
    }
    if (wrapped->inputs.empty())
        return std::nullopt;
    for (const groomlab::PortSpec& p : op->outputs())
        if (geometryPort(p.type))
            wrapped->outputs.push_back(p);
    if (wrapped->outputs.empty())
        return std::nullopt;
    groomlab::EvalContext then, now;
    now.time = 4.0 / 3.0;
    wrapped->readsTime = op->contextSalt(then) != op->contextSalt(now);

    gn::NodeDecl d;
    d.type = "groom" + type;
    d.label = groomlab::nodeLabel(type);
    d.category = "Hair";
    d.doc = info && !info->description.empty() ? hostFree(info->description) : d.label + ".";
    std::set<std::string> taken;
    for (const groomlab::PortSpec& p : wrapped->inputs)
    {
        const char* what = p.type == groomlab::PortType::Mesh     ? "The scalp mesh."
                           : p.type == groomlab::PortType::Guides ? "Guide curves."
                                                                  : "Strand curves.";
        d.sockets.inputs.push_back(geoIn(p.name, what));
        taken.insert(p.name);
    }
    wrapped->scalpSocket = std::none_of(wrapped->inputs.begin(), wrapped->inputs.end(),
                                        [](const groomlab::PortSpec& p) { return p.type == groomlab::PortType::Mesh; }) &&
                           !taken.count(kScalpSocket);
    if (wrapped->scalpSocket)
    {
        d.sockets.inputs.push_back(geoIn(kScalpSocket, "The scalp the strands grow on: what the node measures them against."));
        taken.insert(kScalpSocket);
    }
    if (wrapped->readsTime)
    {
        d.sockets.inputs.push_back(in("seconds", SocketType::Float, 0.0, "Time in seconds; link Scene Time to animate."));
        taken.insert("seconds");
    }
    for (const groomlab::ParamSpec& spec : op->paramSpecs())
    {
        if (taken.count(spec.name) || spec.name == "params")
            continue;
        ParamSocket ps{spec.name};
        const std::string doc = hostFree(spec.description);
        std::optional<gn::SocketDecl> socket;
        std::visit(
            [&](const auto& v) {
                using V = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<V, bool>)
                {
                    ps.type = SocketType::Bool;
                    socket = in(spec.name, SocketType::Bool, v, doc);
                }
                else if constexpr (std::is_same_v<V, std::int64_t>)
                {
                    ps.type = SocketType::Int;
                    socket = bounded(in(spec.name, SocketType::Int, v, doc), spec.min, spec.max);
                }
                else if constexpr (std::is_same_v<V, double>)
                {
                    ps.type = SocketType::Float;
                    socket = bounded(in(spec.name, SocketType::Float, v, doc), spec.min, spec.max);
                }
                else if constexpr (std::is_same_v<V, groomlab::Vec3f>)
                {
                    ps.type = SocketType::Vector;
                    socket = in(spec.name, SocketType::Vector, json::array({v.x, v.y, v.z}), doc);
                }
                else if constexpr (std::is_same_v<V, std::string>)
                {
                    if (!spec.choices.empty())
                    {
                        ps.choice = true;
                        d.props.push_back(prop(spec.name, spec.choices, v, doc));
                    }
                    else
                    {
                        ps.type = SocketType::String;
                        socket = in(spec.name, SocketType::String, v, doc);
                    }
                }
            },
            spec.defaultValue);
        if (!socket && !ps.choice)
            continue;
        if (socket)
            d.sockets.inputs.push_back(*socket);
        wrapped->params.push_back(ps);
        taken.insert(spec.name);
    }
    d.props.push_back(textProp("params", "multiline", "",
                               "Other params as a groom graph's JSON holds them (ramps, maps); the sockets above win."));
    for (const groomlab::PortSpec& p : wrapped->outputs)
        d.sockets.outputs.push_back(geoOut(p.name));

    d.exec = [type, wrapped](ExecContext& c) {
        json params = json::object();
        if (const std::string text = c.prop("params"); !text.empty())
        {
            params = json::parse(text, nullptr, false);
            if (!params.is_object())
                throw ExecError("params: not a JSON object");
        }
        for (const ParamSocket& p : wrapped->params)
        {
            if (p.choice)
                params[p.name] = c.prop(p.name);
            else if (p.type == SocketType::Bool)
                params[p.name] = c.b(p.name);
            else if (p.type == SocketType::Int)
                params[p.name] = c.i(p.name);
            else if (p.type == SocketType::Float)
                params[p.name] = c.f(p.name);
            else if (p.type == SocketType::Vector)
            {
                const glm::dvec3 v = c.v(p.name);
                params[p.name] = json::array({v.x, v.y, v.z});
            }
            else
                params[p.name] = c.s(p.name);
        }
        std::unique_ptr<groomlab::Operator> op;
        try
        {
            op = groomlab::operatorFromJson(type, params.dump());
        }
        catch (const std::exception& e)
        {
            throw ExecError(e.what());
        }
        const std::shared_ptr<const groomlab::ScalpMesh> scalp = scalpOf(c, wrapped->inputs, wrapped->scalpSocket);
        std::vector<groomlab::Value> inputs;
        for (const groomlab::PortSpec& p : op->inputs())
        {
            const auto it = std::find_if(wrapped->inputs.begin(), wrapped->inputs.end(), [&](const auto& w) { return w.name == p.name; });
            if (it == wrapped->inputs.end())
            {
                inputs.emplace_back();
                continue;
            }
            const GeometryPtr g = c.geometry(p.name);
            if (p.type == groomlab::PortType::Mesh)
                inputs.emplace_back(g->mesh && g->mesh->numFaces() > 0 ? groomlab::MeshPtr(groomLabBridge::scalpMesh(*g->mesh)) : nullptr);
            else if (p.type == groomlab::PortType::Guides)
                inputs.emplace_back(g->curves ? toGuides(*g, scalp.get()) : nullptr);
            else
                inputs.emplace_back(g->curves ? toStrands(*g, scalp.get()) : nullptr);
            if (!p.optional && std::holds_alternative<std::monostate>(inputs.back()))
                throw ExecError(p.name + ": nothing connected");
        }
        // Operators read the scalp through the externals too (sizes, surface distances), as a groom graph hands it.
        groomlab::ExternalInputs externals;
        groomlab::setGroomExternals(externals, scalp, nullptr, nullptr);
        groomlab::EvalContext ctx;
        ctx.time = wrapped->readsTime ? c.f("seconds") : 0.0;
        ctx.framesPerSecond = scene::anim::playback().fps;
        ctx.externals = &externals;
        std::vector<groomlab::Value> made;
        try
        {
            made = op->evaluateOutputs(ctx, inputs);
        }
        catch (const std::exception& e)
        {
            throw ExecError(e.what());
        }
        const std::vector<groomlab::PortSpec> outputs = op->outputs();
        for (size_t k = 0; k < outputs.size() && k < made.size(); ++k)
            if (geometryPort(outputs[k].type))
                c.out(outputs[k].name, toGeometry(made[k]));
    };
    return d;
}
} // namespace groomGn

std::vector<std::string> registerGroomGnNodes()
{
    std::vector<std::string> types;
    for (const std::string& type : groomlab::builtinNodeTypes())
        if (std::optional<gn::NodeDecl> d = groomGn::declFor(type))
        {
            types.push_back(d->type);
            gn::registerDecl(std::move(*d));
        }
    return types;
}
} // namespace lila::groom
