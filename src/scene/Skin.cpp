#include "scene/Skin.h"

#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "geo/Symmetry.h"
#include "scene/Components.h"
#include "scene/Diagnostics.h"
#include "scene/DeformerUtil.h"
#include "scene/MeshNodes.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/SkinMethods.h"
#include "scene/SkinPaint.h"
#include "scene/WeightProxy.h"

#include <glm/gtc/quaternion.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <map>
#include <numeric>
#include <unordered_map>

namespace lila::scene
{
using namespace rigutil;
using namespace deformutil;

// ------------------------------------------------------------------------------------------------ weights data

json SkinWeights::toJson() const
{
    // Flat numeric arrays, so the binary format stores them as raw blobs.
    json j = {{"kind", kind()}, {"offsets", offsets}, {"influences", influences}, {"weights", weights}};
    if (!automatic.empty())
        j["automatic"] = automatic;
    return j;
}

DataPtr SkinWeights::fromJson(const json& j)
{
    auto w = std::make_shared<SkinWeights>();
    auto read = [&](const char* key, auto& out) {
        using V = typename std::decay_t<decltype(out)>::value_type;
        out.clear();
        if (!j.contains(key) || !j[key].is_array())
            return;
        out.reserve(j[key].size());
        for (const json& x : j[key])
            out.push_back(V(x.get<double>()));
    };
    read("offsets", w->offsets);
    read("influences", w->influences);
    read("weights", w->weights);
    read("automatic", w->automatic);
    if (w->automatic.size() != w->weights.size())
        w->automatic.clear();
    if (w->offsets.empty())
        w->offsets = {0};
    // A damaged file must not index past the entry arrays.
    size_t entries = std::min(w->influences.size(), w->weights.size());
    for (uint32_t& o : w->offsets)
        o = uint32_t(std::min<size_t>(o, entries));
    for (size_t i = 1; i < w->offsets.size(); ++i)
        w->offsets[i] = std::max(w->offsets[i], w->offsets[i - 1]);
    return w;
}

WeightTable WeightTable::from(const SkinWeights* w, size_t pointCount)
{
    WeightTable t;
    t.points.resize(pointCount);
    if (!w)
        return t;
    const bool flags = w->automatic.size() == w->weights.size() && !w->automatic.empty();
    for (size_t p = 0; p < std::min<size_t>(pointCount, w->pointCount()); ++p)
        for (uint32_t k = w->offsets[p]; k < w->offsets[p + 1]; ++k)
        {
            t.points[p].push_back({w->influences[k], w->weights[k]});
            if (flags && w->automatic[k])
                t.markAutomatic(p, w->influences[k]);
        }
    return t;
}

std::shared_ptr<const SkinWeights> WeightTable::pack() const
{
    auto w = std::make_shared<SkinWeights>();
    w->offsets.reserve(points.size() + 1);
    bool anyAutomatic = false;
    std::vector<uint8_t> flags;
    for (size_t p = 0; p < points.size(); ++p)
    {
        for (const Entry& e : points[p])
            if (e.second > 0.0f)
            {
                w->influences.push_back(e.first);
                w->weights.push_back(e.second);
                const bool a = isAutomatic(p, e.first);
                flags.push_back(uint8_t(a));
                anyAutomatic |= a;
            }
        w->offsets.push_back(uint32_t(w->influences.size()));
    }
    if (anyAutomatic)
        w->automatic = std::move(flags);
    return w;
}

float WeightTable::get(size_t p, uint32_t influence) const
{
    for (const Entry& e : points[p])
        if (e.first == influence)
            return e.second;
    return 0.0f;
}

void WeightTable::set(size_t p, uint32_t influence, float w)
{
    auto& pt = points[p];
    for (Entry& e : pt)
        if (e.first == influence)
        {
            e.second = w;
            return;
        }
    if (w > 0.0f)
        pt.push_back({influence, w});
}

void WeightTable::normalize(size_t p, const std::vector<char>& locked)
{
    auto& pt = points[p];
    auto isLocked = [&](uint32_t i) { return i < locked.size() && locked[i]; };
    float fixed = 0.0f, free = 0.0f;
    for (const Entry& e : pt)
        (isLocked(e.first) ? fixed : free) += e.second;
    float room = std::max(0.0f, 1.0f - fixed);
    if (free <= 0.0f)
        return;
    float s = room / free;
    int movers = 0;
    for (const Entry& e : pt)
        movers += !isLocked(e.first) && e.second > 0.0f;
    // A rescale of one entry has only one place to go; a split between several is Lila's choice.
    const bool chose = movers > 1 && std::abs(s - 1.0f) > 1e-3f;
    for (Entry& e : pt)
        if (!isLocked(e.first))
        {
            e.second *= s;
            if (chose)
                markAutomatic(p, e.first);
        }
    std::erase_if(pt, [](const Entry& e) { return e.second <= 0.0f; });
}

void WeightTable::prune(size_t p, int maxInfluences, float threshold, const std::vector<char>& locked)
{
    auto& pt = points[p];
    auto isLocked = [&](uint32_t i) { return i < locked.size() && locked[i]; };
    std::erase_if(pt, [&](const Entry& e) { return e.second < threshold && !isLocked(e.first); });
    if (maxInfluences > 0 && int(pt.size()) > maxInfluences)
    {
        std::sort(pt.begin(), pt.end(), [&](const Entry& a, const Entry& b) {
            if (isLocked(a.first) != isLocked(b.first))
                return isLocked(a.first);
            return a.second > b.second;
        });
        pt.resize(size_t(maxInfluences));
    }
    if (p < automatic.size())
        std::erase_if(automatic[p], [&](uint32_t i) { return std::none_of(pt.begin(), pt.end(), [&](const Entry& e) { return e.first == i; }); });
    normalize(p, locked);
}

bool WeightTable::isAutomatic(size_t p, uint32_t influence) const
{
    return p < automatic.size() && std::find(automatic[p].begin(), automatic[p].end(), influence) != automatic[p].end();
}

void WeightTable::markAutomatic(size_t p, uint32_t influence)
{
    if (automatic.size() <= p)
        automatic.resize(std::max(points.size(), p + 1));
    if (!isAutomatic(p, influence))
        automatic[p].push_back(influence);
}

void WeightTable::markAuthored(size_t p, uint32_t influence)
{
    if (p < automatic.size())
        std::erase(automatic[p], influence);
}

void WeightTable::markPointAutomatic(size_t p)
{
    if (p < points.size())
        for (const Entry& e : points[p])
            markAutomatic(p, e.first);
}

void WeightTable::markPointAuthored(size_t p)
{
    if (p < automatic.size())
        automatic[p].clear();
}

bool WeightTable::pointAutomatic(size_t p) const
{
    if (p >= automatic.size() || p >= points.size())
        return false;
    for (const Entry& e : points[p])
        if (e.second > 0.0f && isAutomatic(p, e.first))
            return true;
    return false;
}

std::vector<uint32_t> WeightTable::automaticPoints() const
{
    std::vector<uint32_t> out;
    for (size_t p = 0; p < std::min(points.size(), automatic.size()); ++p)
        if (pointAutomatic(p))
            out.push_back(uint32_t(p));
    return out;
}

// ------------------------------------------------------------------------------------------------ layers data

size_t SkinLayers::bytes() const
{
    size_t b = 0;
    for (const SkinLayer& l : layers)
    {
        b += l.mask.size() * 4 + l.name.size();
        for (const auto& pt : l.weights.points)
            b += pt.size() * 8;
    }
    return b;
}

json SkinLayers::toJson() const
{
    json arr = json::array();
    for (const SkinLayer& l : layers)
        arr.push_back({{"name", l.name}, {"enabled", l.enabled}, {"opacity", l.opacity}, {"weights", l.weights.pack()->toJson()}, {"mask", l.mask}});
    return {{"kind", kind()}, {"active", active}, {"layers", arr}};
}

DataPtr SkinLayers::fromJson(const json& j)
{
    auto out = std::make_shared<SkinLayers>();
    out->active = j.value("active", 0);
    if (j.contains("layers") && j["layers"].is_array())
        for (const json& lj : j["layers"])
        {
            SkinLayer l;
            l.name = lj.value("name", std::string("Layer"));
            l.enabled = lj.value("enabled", true);
            l.opacity = lj.value("opacity", 1.0f);
            if (lj.contains("mask") && lj["mask"].is_array())
                for (const json& m : lj["mask"])
                    l.mask.push_back(m.get<float>());
            if (lj.contains("weights"))
            {
                auto w = std::static_pointer_cast<const SkinWeights>(SkinWeights::fromJson(lj["weights"]));
                l.weights = WeightTable::from(w.get(), w->pointCount());
            }
            out->layers.push_back(std::move(l));
        }
    out->active = std::clamp(out->active, 0, std::max(0, int(out->layers.size()) - 1));
    return out;
}

// ------------------------------------------------------------------------------------------------ nodes

void SkinInfluenceNode::compute(const EvalContext&)
{
    auto link = std::make_shared<InfluenceLink>();
    link->prev = as<DataPtr>(kPrevious);
    link->index = uint32_t(std::max<int64_t>(0, as<int64_t>(kIndex)));
    link->matrix = as<glm::dmat4>(kMatrix) * as<glm::dmat4>(kBindPreMatrix);
    setOutput(kLink, DataPtr(std::move(link)));
}

namespace
{
struct DualQuat
{
    glm::dquat real{1, 0, 0, 0};
    glm::dquat dual{0, 0, 0, 0};
};

DualQuat toDualQuat(const glm::dmat4& m)
{
    DualQuat d;
    d.real = glm::normalize(glm::quat_cast(orthonormal(glm::dmat3(m))));
    glm::dvec3 t(m[3]);
    d.dual = glm::dquat(0.0, t.x, t.y, t.z) * d.real * 0.5;
    return d;
}
} // namespace

void SkinClusterNode::compute(const EvalContext&)
{
    LILA_ZONE();

    const MeshPtr& in = as<MeshPtr>(kInMesh);
    const auto* w = dynamic_cast<const SkinWeights*>(as<DataPtr>(kWeights).get());
    double envelope = as<double>(kEnvelope);
    if (!in || !as<bool>(kEnabled) || !w || envelope <= 0.0)
    {
        setOutput(kOutMesh, in);
        return;
    }

    glm::dmat4 geom = as<glm::dmat4>(kGeomMatrix);
    glm::dmat4 geomInv = glm::inverse(geom);
    std::vector<glm::dmat4> palette;
    std::vector<char> have;
    for (DataPtr d = as<DataPtr>(kInfluences); d;)
    {
        const auto* link = dynamic_cast<const InfluenceLink*>(d.get());
        if (!link)
            break;
        if (link->index >= palette.size())
        {
            palette.resize(link->index + 1, glm::dmat4(1.0));
            have.resize(link->index + 1, 0);
        }
        palette[link->index] = geomInv * link->matrix * geom;
        have[link->index] = 1;
        d = link->prev;
    }
    if (int64_t method = as<int64_t>(kSkinningMethod); method == kCentresOfRotation || method == kDirectDeltaMush)
    {
        setOutput(kOutMesh, skin::skinWithMethod(*this, *in, *w, palette, envelope));
        return;
    }
    bool dq = as<int64_t>(kSkinningMethod) == kDualQuaternion;
    std::vector<DualQuat> dqs;
    if (dq)
        for (const glm::dmat4& m : palette)
            dqs.push_back(toDualQuat(m));
    bool normalize = as<bool>(kNormalizeWeights);

    // Reuse the output from two computes ago when nothing holds it: only points and normals differ from the input.
    std::shared_ptr<geo::Mesh> out;
    if (olderOut && olderOut.use_count() == 1 && outSource.lock() == in)
        out = std::make_shared<geo::Mesh>(std::move(*olderOut));
    else
        out = std::make_shared<geo::Mesh>(*in);
    if (outSource.lock() != in)
        lastOut.reset();
    olderOut = std::move(lastOut);
    lastOut = out;
    outSource = in;
    size_t n = std::min<size_t>(in->pointCapacity(), w->pointCount());
    auto normals = out->cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    const auto restNormals = in->cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    std::vector<glm::dmat3> pointRot(normals.empty() ? 0 : n, glm::dmat3(1.0));

    tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 512), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t p = r.begin(); p != r.end(); ++p)
        {
            if (!in->pointValid(geo::Index(p)))
                continue;
            glm::dvec3 rest = in->position(geo::Index(p));
            out->setPosition(geo::Index(p), rest);
            double total = 0.0;
            glm::dvec3 moved;
            glm::dmat3 rot(1.0);
            if (dq)
            {
                DualQuat sum{{0, 0, 0, 0}, {0, 0, 0, 0}};
                glm::dquat pivot{0, 0, 0, 0};
                for (uint32_t k = w->offsets[p]; k < w->offsets[p + 1]; ++k)
                {
                    uint32_t i = w->influences[k];
                    if (i >= have.size() || !have[i])
                        continue;
                    double wt = w->weights[k];
                    // Blend in one hemisphere, or opposite quaternions cancel out.
                    if (total == 0.0)
                        pivot = dqs[i].real;
                    double s = glm::dot(pivot, dqs[i].real) < 0.0 ? -wt : wt;
                    sum.real += dqs[i].real * s;
                    sum.dual += dqs[i].dual * s;
                    total += wt;
                }
                if (total <= 0.0)
                    continue;
                if (!normalize && total < 1.0)
                {
                    sum.real += glm::dquat(1, 0, 0, 0) * (1.0 - total);
                    total = 1.0;
                }
                double len = glm::length(sum.real);
                if (len < 1e-12)
                    continue;
                glm::dquat q = sum.real / len;
                glm::dquat d = sum.dual / len;
                glm::dquat t = d * glm::conjugate(q) * 2.0;
                moved = q * rest + glm::dvec3(t.x, t.y, t.z);
                rot = glm::mat3_cast(q);
            }
            else
            {
                glm::dmat4 m(0.0);
                for (uint32_t k = w->offsets[p]; k < w->offsets[p + 1]; ++k)
                {
                    uint32_t i = w->influences[k];
                    if (i >= have.size() || !have[i])
                        continue;
                    double wt = w->weights[k];
                    m += palette[i] * wt;
                    total += wt;
                }
                if (total <= 0.0)
                    continue;
                if (normalize)
                    m = m * (1.0 / total);
                else if (total < 1.0)
                    m += glm::dmat4(1.0) * (1.0 - total);
                moved = glm::dvec3(m * glm::dvec4(rest, 1.0));
                rot = glm::dmat3(m);
            }
            out->setPosition(geo::Index(p), glm::mix(rest, moved, envelope));
            if (!pointRot.empty())
                pointRot[p] = glm::dmat3(1.0) + (rot - glm::dmat3(1.0)) * envelope;
        }
    });

    if (!normals.empty())
    {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, normals.size(), 1024), [&](const tbb::blocked_range<size_t>& r) {
            for (size_t c = r.begin(); c != r.end(); ++c)
            {
                if (!out->cornerValid(geo::Index(c)))
                    continue;
                normals[c] = restNormals[c];
                geo::Index p = out->cornerPoint(geo::Index(c));
                if (p >= pointRot.size())
                    continue;
                glm::dvec3 nn = glm::transpose(glm::inverse(pointRot[p])) * glm::dvec3(restNormals[c]);
                if (glm::length(nn) > 1e-12)
                    normals[c] = glm::vec3(glm::normalize(nn));
            }
        });
    }
    setOutput(kOutMesh, MeshPtr(std::move(out)));
}

// ------------------------------------------------------------------------------------------------ skin api

namespace skin
{
namespace
{
std::map<std::string, WeightHost, std::less<>>& hosts()
{
    static std::map<std::string, WeightHost, std::less<>> h;
    return h;
}

const WeightHost& skinHost()
{
    static const WeightHost h{.weights = SkinClusterNode::kWeights,
                              .layers = SkinClusterNode::kWeightLayers,
                              .geomMatrix = SkinClusterNode::kGeomMatrix,
                              .inMesh = SkinClusterNode::kInMesh,
                              .outMesh = SkinClusterNode::kOutMesh,
                              .maxInfluences = SkinClusterNode::kMaxInfluences};
    return h;
}
} // namespace

void registerWeightHost(const std::string& type, WeightHost host) { hosts()[type] = std::move(host); }

const WeightHost* weightHost(const Graph& g, NodeId node)
{
    const Node* n = g.node(node);
    if (!n)
        return nullptr;
    if (dynamic_cast<const SkinClusterNode*>(n))
        return &skinHost();
    const auto it = hosts().find(n->type().name);
    return it != hosts().end() ? &it->second : nullptr;
}

NodeId findPainted(Graph& g, NodeId mesh)
{
    if (const NodeId s = find(g, mesh); s != kNoNode)
        return s;
    for (NodeId id : modifiers::stack(g, mesh))
        if (weightHost(g, id))
            return id;
    return kNoNode;
}

int maxInfluencesOf(Graph& g, NodeId skin)
{
    const WeightHost* h = weightHost(g, skin);
    if (!h)
        return 4;
    return h->maxInfluences >= 0 ? int(g.node(skin)->as<int64_t>(h->maxInfluences)) : h->fixedMaxInfluences;
}

NodeId find(Graph& g, NodeId mesh)
{
    for (NodeId id : modifiers::stack(g, mesh))
        if (dynamic_cast<SkinClusterNode*>(g.node(id)))
            return id;
    return kNoNode;
}

NodeId resolve(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    if (dynamic_cast<SkinClusterNode*>(n))
        return n->id();
    if (n->type().isA("mesh"))
        if (NodeId s = find(g, n->id()); s != kNoNode)
            return s;
    throw CommandError(std::string(cmd) + ": " + name + " has no skinCluster");
}

NodeId meshOf(Graph& g, NodeId skin)
{
    NodeId cur = skin;
    for (int guard = 0; guard < 256; ++guard)
    {
        Node* n = g.node(cur);
        int out = n->type().attrIndex("outMesh");
        if (out < 0)
            return kNoNode;
        auto dsts = g.destinations({cur, out, -1});
        if (dsts.empty())
            return kNoNode;
        Node* d = g.node(dsts[0].node);
        if (d->type().isA("mesh"))
            return d->id();
        cur = d->id();
    }
    return kNoNode;
}

std::vector<Influence> influences(Graph& g, NodeId skin)
{
    if (const WeightHost* h = weightHost(g, skin); h && h->influences)
        return h->influences(g, skin);
    std::vector<Influence> out;
    PlugRef plug{skin, SkinClusterNode::kInfluences, -1};
    while (auto src = g.source(plug))
    {
        Node* link = g.node(src->node);
        if (!dynamic_cast<SkinInfluenceNode*>(link) || out.size() > 4096)
            break;
        Influence inf;
        inf.link = link->id();
        inf.index = uint32_t(std::max<int64_t>(0, link->as<int64_t>(SkinInfluenceNode::kIndex)));
        inf.locked = link->as<bool>(SkinInfluenceNode::kLockWeights);
        if (auto m = g.source({link->id(), SkinInfluenceNode::kMatrix, -1}))
        {
            inf.joint = m->node;
            inf.name = g.node(m->node)->name();
        }
        out.push_back(std::move(inf));
        plug = {link->id(), SkinInfluenceNode::kPrevious, -1};
    }
    std::reverse(out.begin(), out.end());
    return out;
}

int64_t indexOf(const std::vector<Influence>& infs, const std::string& joint)
{
    for (const Influence& i : infs)
        if (i.name == joint || (i.joint != kNoNode && Graph::current().path(i.joint) == joint))
            return i.index;
    return -1;
}

std::vector<char> lockMask(const std::vector<Influence>& infs)
{
    std::vector<char> mask;
    for (const Influence& i : infs)
    {
        if (i.index >= mask.size())
            mask.resize(i.index + 1, 0);
        mask[i.index] = i.locked;
    }
    return mask;
}

MeshPtr inputMesh(Graph& g, NodeId skin)
{
    const WeightHost* h = weightHost(g, skin);
    return std::get<MeshPtr>(g.value({skin, h ? h->inMesh : int(SkinClusterNode::kInMesh), -1}));
}

WeightTable table(Graph& g, NodeId skin)
{
    const WeightHost* h = weightHost(g, skin);
    MeshPtr m = inputMesh(g, skin);
    const auto* w = dynamic_cast<const SkinWeights*>(std::get<DataPtr>(g.storedValue({skin, h ? h->weights : int(SkinClusterNode::kWeights), -1})).get());
    if (!w && h && h->seed)
        return h->seed(g, skin);
    return WeightTable::from(w, m ? m->pointCapacity() : (w ? w->pointCount() : 0));
}

void setWeights(Graph& g, NodeId skin, const WeightTable& t)
{
    const WeightHost* h = weightHost(g, skin);
    g.setValue({skin, h ? h->weights : int(SkinClusterNode::kWeights), -1}, DataPtr(t.pack()));
}

uint32_t addInfluence(Graph& g, NodeId skin, NodeId joint)
{
    std::vector<Influence> infs = influences(g, skin);
    uint32_t next = 0;
    for (const Influence& i : infs)
    {
        if (i.joint == joint)
            return i.index;
        next = std::max(next, i.index + 1);
    }
    g.evaluate();
    Node* link = g.createNode(*types::find("skinInfluence"), g.node(skin)->name() + "Influence", kNoNode, true);
    NodeId id = link->id();
    g.setValue({id, SkinInfluenceNode::kIndex}, int64_t(next));
    // The joint's current pose becomes its bind pose, as in Maya.
    g.setValue({id, SkinInfluenceNode::kBindPreMatrix}, glm::inverse(worldOf(*g.node(joint))));
    g.connect({joint, Transform::kWorldMatrix, -1}, {id, SkinInfluenceNode::kMatrix, -1});
    PlugRef end{skin, SkinClusterNode::kInfluences, -1};
    if (auto prev = g.source(end))
    {
        g.disconnect(end);
        g.connect(*prev, {id, SkinInfluenceNode::kPrevious, -1});
    }
    g.connect({id, SkinInfluenceNode::kLink, -1}, end);
    return next;
}
const char* outcomeName(PaintOutcome o)
{
    switch (o)
    {
    case PaintOutcome::Painted: return "painted";
    case PaintOutcome::Unchanged: return "unchanged";
    case PaintOutcome::Ambiguous: return "ambiguous";
    case PaintOutcome::NowhereToGo: return "nowhereToGo";
    case PaintOutcome::InfluenceLimit: return "influenceLimit";
    case PaintOutcome::Locked: return "locked";
    }
    return "unchanged";
}

PaintOutcome paintPoint(WeightTable& t, size_t p, PaintMode mode, bool invert, float value, float a, const std::vector<char>& locked,
                        const PaintOptions& o, bool dryRun)
{
    if (p >= t.points.size() || mode == PaintMode::Smooth)
        return PaintOutcome::Unchanged;
    auto isLocked = [&](uint32_t i) { return i < locked.size() && locked[i]; };
    const uint32_t inf = o.influence;
    if (isLocked(inf))
        return PaintOutcome::Locked;
    const bool hasReceiver = o.receiver >= 0 && uint32_t(o.receiver) != inf;
    const auto receiver = uint32_t(std::max<int64_t>(0, o.receiver));
    if (hasReceiver && isLocked(receiver))
        return PaintOutcome::Locked;

    auto& pt = t.points[p];
    int present = 0;
    std::vector<uint32_t> partners;
    for (auto& [i, w] : pt)
    {
        if (w <= 0.0f)
            continue;
        ++present;
        if (!isLocked(i) && i != inf && !hasReceiver)
            partners.push_back(i);
    }
    if (hasReceiver)
        partners = {receiver};
    const float old = t.get(p, inf);
    float want = old;
    switch (mode)
    {
    case PaintMode::Add: want = invert ? old - value * a : old + value * a; break;
    case PaintMode::Replace: want = old + (value - old) * a; break;
    case PaintMode::Scale: want = old * (1.0f - a * (1.0f - value)); break;
    case PaintMode::Smooth: break;
    }
    if (std::abs(want - old) < 1e-7f)
        return PaintOutcome::Unchanged;

    // An unweighted point can only become this influence's, whole.
    if (present == 0)
    {
        if (want <= 0.0f)
            return PaintOutcome::Unchanged;
        if (!dryRun)
        {
            t.set(p, inf, 1.0f);
            t.markAuthored(p, inf);
        }
        return PaintOutcome::Painted;
    }

    float partnerSum = 0.0f;
    for (uint32_t i : partners)
        partnerSum += t.get(p, i);
    if (!hasReceiver && partners.empty())
        return want < old ? PaintOutcome::NowhereToGo : PaintOutcome::Unchanged;
    if (hasReceiver && want > old && partnerSum <= 0.0f)
        return PaintOutcome::NowhereToGo;
    if (!hasReceiver && partners.size() > 1 && o.strict)
        return PaintOutcome::Ambiguous;

    const float next = std::clamp(want, 0.0f, old + partnerSum);
    const float delta = next - old;
    if (std::abs(delta) < 1e-7f)
        return want < old ? PaintOutcome::NowhereToGo : PaintOutcome::Unchanged;

    // Slots: the painted influence may arrive or leave, the receiver may arrive or empty.
    int slots = present + (old <= 0.0f && next > 0.0f) - (old > 0.0f && next <= 0.0f);
    if (hasReceiver)
    {
        const float rw = t.get(p, receiver), rNext = rw - delta;
        slots += (rw <= 0.0f && rNext > 1e-6f) - (rw > 0.0f && rNext <= 1e-6f);
    }
    if (o.maxInfluences > 0 && slots > o.maxInfluences && (o.strict || hasReceiver))
        return PaintOutcome::InfluenceLimit;
    if (dryRun)
        return PaintOutcome::Painted;

    t.set(p, inf, next);
    t.markAuthored(p, inf);
    if (partners.size() == 1)
    {
        const float pw = t.get(p, partners[0]);
        t.set(p, partners[0], std::max(0.0f, pw - delta));
        t.markAuthored(p, partners[0]);
    }
    else if (partnerSum > 0.0f)
    {
        const float scale = std::max(0.0f, partnerSum - delta) / partnerSum;
        for (uint32_t i : partners)
        {
            t.set(p, i, t.get(p, i) * scale);
            t.markAutomatic(p, i);
        }
    }
    std::vector<char> keep = locked;
    keep.resize(std::max<size_t>(keep.size(), size_t(std::max(inf, receiver)) + 1), 0);
    keep[inf] = 1;
    if (hasReceiver)
        keep[receiver] = 1;
    t.prune(p, o.maxInfluences, 1e-4f, keep);
    return PaintOutcome::Painted;
}

namespace
{
/** Each listed point's edge neighbours on the rest mesh, weighted by inverse length so density doesn't bias a relax. */
std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, float>>> edgeWeights(const geo::Mesh& m, const std::vector<std::pair<uint32_t, float>>& pts)
{
    std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, float>>> out;
    out.reserve(pts.size());
    const double floor = std::max(1e-9, glm::length(m.bounds().size()) * 1e-6);
    for (auto [p, a] : pts)
    {
        if (p >= m.pointCapacity() || !m.pointValid(p) || out.contains(p))
            continue;
        auto& n = out[p];
        m.forEachPointEdge(p, [&](geo::Index e) {
            geo::Index q = m.edgeOtherPoint(e, p);
            n.push_back({q, float(1.0 / std::max(glm::length(m.position(q) - m.position(p)), floor))});
        });
    }
    return out;
}
} // namespace

void relaxPoints(const geo::Mesh& rest, WeightTable& t, const std::vector<std::pair<uint32_t, float>>& pts, const std::vector<char>& locked, int maxInf,
                 int iterations, const std::vector<char>* fixed)
{
    LILA_ZONE();

    auto isLocked = [&](uint32_t i) { return i < locked.size() && locked[i]; };
    auto nbrs = edgeWeights(rest, pts);
    std::vector<std::pair<uint32_t, float>> live;
    for (auto [p, a] : pts)
        if (p < t.points.size() && a > 0.0f && nbrs.contains(p) && !(fixed && p < fixed->size() && (*fixed)[p]))
            live.push_back({p, std::min(1.0f, a)});
    std::vector<std::vector<WeightTable::Entry>> result(live.size());
    for (int it = 0; it < std::max(1, iterations); ++it)
    {
        tbb::parallel_for(size_t(0), live.size(), [&](size_t k) {
            auto [p, a] = live[k];
            std::unordered_map<uint32_t, float> acc;
            float total = 0.0f;
            for (auto [q, c] : nbrs.at(p))
            {
                if (q >= t.points.size())
                    continue;
                for (auto& [i, w] : t.points[q])
                    acc[i] += w * c;
                total += c;
            }
            std::unordered_map<uint32_t, float> blended;
            float lockedSum = 0.0f;
            for (auto& [i, w] : t.points[p])
            {
                if (isLocked(i))
                    lockedSum += w;
                blended[i] += isLocked(i) ? w : w * (1.0f - a);
            }
            if (total > 0.0f)
                for (auto& [i, w] : acc)
                    if (!isLocked(i))
                        blended[i] += w / total * a;
            float free = 0.0f;
            for (auto& [i, w] : blended)
                if (!isLocked(i))
                    free += w;
            const float s = free > 0.0f ? std::max(0.0f, 1.0f - lockedSum) / free : 0.0f;
            auto& out = result[k];
            out.clear();
            for (auto& [i, w] : blended)
                if (float v = isLocked(i) ? w : w * s; v > 0.0f)
                    out.push_back({i, v});
            std::sort(out.begin(), out.end());
        });
        for (size_t k = 0; k < live.size(); ++k)
        {
            const uint32_t p = live[k].first;
            t.points[p] = result[k];
            t.prune(p, maxInf, 1e-4f, locked);
            t.markPointAuthored(p);
        }
    }
}

std::vector<glm::dmat4> objectMatrices(Graph& g, NodeId skin)
{
    g.evaluate();
    const glm::dmat4 geom = g.node(skin)->as<glm::dmat4>(SkinClusterNode::kGeomMatrix), inv = glm::inverse(geom);
    std::vector<glm::dmat4> out;
    for (const Influence& i : influences(g, skin))
    {
        const Node* link = g.node(i.link);
        if (!link)
            continue;
        if (out.size() <= i.index)
            out.resize(i.index + 1, glm::dmat4(1.0));
        out[i.index] = inv * link->as<glm::dmat4>(SkinInfluenceNode::kMatrix) * link->as<glm::dmat4>(SkinInfluenceNode::kBindPreMatrix) * geom;
    }
    return out;
}

std::shared_ptr<const SkinLayers> layers(Graph& g, NodeId skin)
{
    const WeightHost* h = weightHost(g, skin);
    if (!h || h->layers < 0)
        return nullptr;
    auto l = std::dynamic_pointer_cast<const SkinLayers>(std::get<DataPtr>(g.storedValue({skin, h->layers, -1})));
    return l && !l->layers.empty() ? l : nullptr;
}

void setLayers(Graph& g, NodeId skin, std::shared_ptr<const SkinLayers> l)
{
    const WeightHost* h = weightHost(g, skin);
    if (!h || h->layers < 0)
        throw CommandError(g.node(skin)->name() + " has no weight layers");
    g.setValue({skin, h->layers, -1}, DataPtr(std::move(l)));
}

namespace
{
/** One layer's point standing in for what the layer stores, while a stroke paints it. */
struct PointOverride
{
    int layer = -1;
    const std::vector<WeightTable::Entry>* entries = nullptr;
    const std::vector<uint32_t>* automatic = nullptr;
    float mask = 0.0f;
};

void compositeAt(const SkinLayers& l, int upTo, size_t p, WeightTable& out, int maxInf, const PointOverride& ov)
{
    if (l.layers.empty() || p >= out.points.size())
        return;
    static const std::vector<WeightTable::Entry> kNoEntries;
    static const std::vector<uint32_t> kNoIds;
    auto entriesOf = [&](int k) -> const std::vector<WeightTable::Entry>& {
        if (k == ov.layer)
            return *ov.entries;
        const WeightTable& t = l.layers[size_t(k)].weights;
        return p < t.points.size() ? t.points[p] : kNoEntries;
    };
    auto autosOf = [&](int k) -> const std::vector<uint32_t>& {
        if (k == ov.layer)
            return ov.automatic ? *ov.automatic : kNoIds;
        const WeightTable& t = l.layers[size_t(k)].weights;
        return p < t.automatic.size() ? t.automatic[p] : kNoIds;
    };
    std::vector<WeightTable::Entry> acc = entriesOf(0);
    std::vector<uint32_t> autos = autosOf(0);
    bool blended = false;
    for (int k = 1; k < std::min(upTo, int(l.layers.size())); ++k)
    {
        const SkinLayer& layer = l.layers[size_t(k)];
        const float mask = k == ov.layer ? ov.mask : p < layer.mask.size() ? layer.mask[p] : 0.0f;
        const float a = layer.enabled ? layer.opacity * mask : 0.0f;
        if (a <= 0.0f)
            continue;
        const auto& top = entriesOf(k);
        const auto& topAuto = autosOf(k);
        if (a >= 1.0f)
        {
            acc = top;
            autos = topAuto;
            continue;
        }
        std::unordered_map<uint32_t, float> m;
        for (auto& [i, w] : acc)
            m[i] += w * (1.0f - a);
        for (auto& [i, w] : top)
            m[i] += w * a;
        acc.assign(m.begin(), m.end());
        std::sort(acc.begin(), acc.end());
        blended = true;
        for (uint32_t i : topAuto)
            if (std::find(autos.begin(), autos.end(), i) == autos.end())
                autos.push_back(i);
    }
    out.points[p] = std::move(acc);
    out.markPointAuthored(p);
    for (uint32_t i : autos)
        out.markAutomatic(p, i);
    // Only a blend can need pruning; a point a layer covers fully or not at all keeps its weights bit for bit.
    if (blended)
        out.prune(p, maxInf, 0.0f, {});
}
} // namespace

void compositePoint(const SkinLayers& l, int upTo, size_t p, WeightTable& out, int maxInf) { compositeAt(l, upTo, p, out, maxInf, {}); }

WeightTable composite(const SkinLayers& l, size_t pointCount, int maxInf)
{
    LILA_ZONE();

    WeightTable out;
    out.points.resize(pointCount);
    // Sized up front: markAutomatic would grow it from several threads at once.
    out.automatic.resize(pointCount);
    tbb::parallel_for(size_t(0), pointCount, [&](size_t p) { compositePoint(l, int(l.layers.size()), p, out, maxInf); });
    return out;
}

WeightEdit::WeightEdit(Graph& g, NodeId skin) : mGraph(g), mSkin(skin)
{
    mInfluences = skin::influences(g, skin);
    mLocked = lockMask(mInfluences);
    mMaxInf = maxInfluencesOf(g, skin);
    mLayers = skin::layers(g, skin);
    if (!mLayers)
    {
        mTable = skin::table(g, skin);
        return;
    }
    MeshPtr m = inputMesh(g, skin);
    const size_t n = m ? m->pointCapacity() : mLayers->layers[0].weights.points.size();
    const auto active = size_t(std::clamp(mLayers->active, 0, int(mLayers->layers.size()) - 1));
    mSeed = mLayers->layers[active].weights;
    mSeed.points.resize(n);
    mSeed.automatic.resize(n);
    if (active > 0)
    {
        const auto& mask = mLayers->layers[active].mask;
        for (size_t p = 0; p < n; ++p)
            if (p >= mask.size() || mask[p] <= 0.0f)
                compositePoint(*mLayers, int(active), p, mSeed, mMaxInf);
    }
    mTable = mSeed;
}

bool WeightEdit::changed(size_t p) const
{
    if (p >= mTable.points.size())
        return false;
    if (p >= mSeed.points.size())
        return !mTable.points[p].empty();
    return mTable.points[p] != mSeed.points[p];
}

void WeightEdit::compositeInto(size_t p, WeightTable& out) const
{
    if (p >= out.points.size() || p >= mTable.points.size())
        return;
    auto copyFlags = [&](WeightTable& dst) {
        dst.markPointAuthored(p);
        if (p < mTable.automatic.size())
            for (uint32_t i : mTable.automatic[p])
                dst.markAutomatic(p, i);
    };
    if (!mLayers)
    {
        out.points[p] = mTable.points[p];
        copyFlags(out);
        return;
    }
    // The active layer's point comes from the edit; every other point and layer is read where it is stored.
    const auto active = std::clamp(mLayers->active, 0, int(mLayers->layers.size()) - 1);
    const auto& stored = mLayers->layers[size_t(active)].mask;
    PointOverride ov{.layer = active,
                     .entries = &mTable.points[p],
                     .automatic = p < mTable.automatic.size() ? &mTable.automatic[p] : nullptr,
                     .mask = std::max(p < stored.size() ? stored[p] : 0.0f, changed(p) ? 1.0f : 0.0f)};
    compositeAt(*mLayers, int(mLayers->layers.size()), p, out, mMaxInf, ov);
}

void WeightEdit::commit()
{
    if (!mLayers)
    {
        setWeights(mGraph, mSkin, mTable);
        return;
    }
    auto l = std::make_shared<SkinLayers>(*mLayers);
    const auto active = size_t(std::clamp(l->active, 0, int(l->layers.size()) - 1));
    SkinLayer& layer = l->layers[active];
    if (active > 0)
    {
        layer.mask.resize(mTable.points.size(), 0.0f);
        for (size_t p = 0; p < mTable.points.size(); ++p)
            if (layer.mask[p] <= 0.0f && changed(p))
                layer.mask[p] = 1.0f;
    }
    layer.weights = mTable;
    const size_t n = mTable.points.size();
    setLayers(mGraph, mSkin, l);
    setWeights(mGraph, mSkin, composite(*l, n, mMaxInf));
}

void reportAutomatic(Graph& g)
{
    diagnostics::clear("skinWeights");
    for (NodeId id : g.nodes())
    {
        const Node* n = g.node(id);
        if (!n || !dynamic_cast<const SkinClusterNode*>(n))
            continue;
        const auto* w = dynamic_cast<const SkinWeights*>(std::get<DataPtr>(g.storedValue({id, SkinClusterNode::kWeights, -1})).get());
        if (!w || w->automatic.empty())
            continue;
        MeshPtr m = inputMesh(g, id);
        size_t count = 0;
        for (uint32_t p = 0; p < w->pointCount(); ++p)
        {
            if (m && (p >= m->pointCapacity() || !m->pointValid(p)))
                continue;
            for (uint32_t k = w->offsets[p]; k < w->offsets[p + 1]; ++k)
                if (w->automatic[k])
                {
                    ++count;
                    break;
                }
        }
        const NodeId mesh = meshOf(g, id);
        if (!count || mesh == kNoNode)
            continue;
        const std::string path = g.node(mesh)->path();
        diagnostics::Issue issue;
        issue.source = "skinWeights";
        issue.severity = diagnostics::Severity::Warning;
        issue.target = path;
        issue.message = std::to_string(count) + (count == 1 ? " vertex has" : " vertices have") +
                        " automatic weights (Lila chose the split). Paint, relax or settle them, or accept them.";
        issue.select = "from lila import ops\nops.selectAutomaticWeights(" + json(path).dump() + ")";
        issue.fix = "from lila import ops\nops.acceptSkinWeights(" + json(path).dump() + ")";
        diagnostics::report(std::move(issue));
    }
}
} // namespace skin

// ------------------------------------------------------------------------------------------------ binding

namespace
{
double segmentDistance(const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& b, glm::dvec3* closest = nullptr)
{
    glm::dvec3 d = b - a;
    double len2 = glm::dot(d, d);
    double t = len2 > 0.0 ? std::clamp(glm::dot(p - a, d) / len2, 0.0, 1.0) : 0.0;
    glm::dvec3 q = a + d * t;
    if (closest)
        *closest = q;
    return glm::length(p - q);
}

/** Closest-distance weights (Maya's default bind): the nearest bones by distance, falling off as 1/d^dropoff. */
WeightTable closestWeights(const geo::Mesh& world, const std::vector<SkinBone>& bones, int maxInf, double dropoff, const geo::MeshBvh* bvh)
{
    LILA_ZONE();

    WeightTable t;
    t.points.resize(world.pointCapacity());
    double scale = std::max(1e-6, glm::length(world.bounds().size()));
    tbb::parallel_for(tbb::blocked_range<geo::Index>(0, world.pointCapacity(), 256), [&](const tbb::blocked_range<geo::Index>& r) {
        std::vector<std::pair<double, uint32_t>> cand;
        for (geo::Index p = r.begin(); p != r.end(); ++p)
        {
            if (!world.pointValid(p))
                continue;
            glm::dvec3 pos = world.position(p);
            cand.clear();
            for (const SkinBone& b : bones)
            {
                double best = 1e300;
                glm::dvec3 q, bestQ;
                for (auto& [a, e] : b.segments)
                    if (double d = segmentDistance(pos, a, e, &q); d < best)
                    {
                        best = d;
                        bestQ = q;
                    }
                // Heat-lite: bones hidden behind the surface (the other leg, the torso behind an arm) don't count.
                if (bvh)
                {
                    glm::dvec3 dir = bestQ - pos;
                    if (glm::length(dir) > 1e-9 && bvh->occluded(pos + dir * 1e-3, dir, 0.997))
                        best = 1e300;
                }
                cand.push_back({best, b.index});
            }
            std::sort(cand.begin(), cand.end());
            if (cand.empty())
                continue;
            if (cand[0].first >= 1e299)
            {
                // Nothing visible: fall back to plain distance for this point.
                cand.clear();
                for (const SkinBone& b : bones)
                {
                    double best = 1e300;
                    for (auto& [a, e] : b.segments)
                        best = std::min(best, segmentDistance(pos, a, e));
                    cand.push_back({best, b.index});
                }
                std::sort(cand.begin(), cand.end());
            }
            double eps = scale * 1e-4;
            auto& pt = t.points[p];
            double sum = 0.0;
            for (size_t k = 0; k < cand.size() && int(k) < maxInf; ++k)
            {
                if (cand[k].first >= 1e299)
                    break;
                double wt = 1.0 / std::pow(std::max(cand[k].first, eps) / std::max(cand[0].first, eps), dropoff);
                pt.push_back({cand[k].second, float(wt)});
                sum += wt;
            }
            for (auto& e : pt)
                e.second = float(e.second / sum);
        }
    });
    return t;
}

/** Laplacian smoothing of weights over mesh edges, keeping the influence cap. */
void smoothWeights(const geo::Mesh& m, WeightTable& t, int iterations, int maxInf, const std::vector<char>& locked, const std::vector<uint8_t>* mask = nullptr,
                   double amount = 0.5)
{
    LILA_ZONE();

    for (int it = 0; it < iterations; ++it)
    {
        WeightTable next = t;
        tbb::parallel_for(tbb::blocked_range<geo::Index>(0, m.pointCapacity(), 256), [&](const tbb::blocked_range<geo::Index>& r) {
            std::unordered_map<uint32_t, float> acc;
            for (geo::Index p = r.begin(); p != r.end(); ++p)
            {
                if (!m.pointValid(p) || p >= t.points.size() || (mask && (p >= mask->size() || !(*mask)[p])))
                    continue;
                acc.clear();
                int count = 0;
                m.forEachPointEdge(p, [&](geo::Index e) {
                    geo::Index o = m.edgeOtherPoint(e, p);
                    if (o >= t.points.size())
                        return;
                    for (auto& [i, wt] : t.points[o])
                        acc[i] += wt;
                    ++count;
                });
                if (count == 0)
                    continue;
                auto& pt = next.points[p];
                std::unordered_map<uint32_t, float> blended;
                for (auto& [i, wt] : t.points[p])
                    blended[i] += wt * float(1.0 - amount);
                for (auto& [i, wt] : acc)
                    blended[i] += wt / float(count) * float(amount);
                // Locked influences keep their exact value.
                for (auto& [i, wt] : blended)
                    if (i < locked.size() && locked[i])
                        wt = t.get(p, i);
                pt.assign(blended.begin(), blended.end());
                std::sort(pt.begin(), pt.end());
                next.prune(p, maxInf, 1e-4f, locked);
            }
        });
        t = std::move(next);
    }
}

geo::Mesh worldCopy(const geo::Mesh& m, const glm::dmat4& world)
{
    geo::Mesh w = m;
    for (geo::Index p = 0; p < w.pointCapacity(); ++p)
        w.setPosition(p, glm::dvec3(world * glm::dvec4(m.position(p), 1.0)));
    return w;
}

int influenceCap(int requested)
{
    int ceiling = PrefStore::get().value<int>("general", "/rig/maxInfluences", 8);
    if (ceiling > 0 && requested > ceiling)
    {
        log::warn("maxInfluences {} is above the scene limit {} (pref rig/maxInfluences); using {}", requested, ceiling, ceiling);
        return ceiling;
    }
    return std::max(1, requested);
}

Node& meshArg(const std::string& name, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (!n.type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return n;
}

/** Points the command works on: the vertices flag, the mesh's selected components, or every point. */
std::vector<uint32_t> pointsArg(const Args& a, Graph& g, NodeId skin, bool allIfNone)
{
    std::vector<uint32_t> out;
    if (a.has("vertices"))
    {
        for (const json& v : a.flags["vertices"])
            out.push_back(v.get<uint32_t>());
        return out;
    }
    NodeId mesh = skin::meshOf(g, skin);
    if (mesh != kNoNode && components::mode() != ComponentMode::Object)
        out = components::pointsOf(mesh);
    if (out.empty() && allIfNone)
        if (MeshPtr m = skin::inputMesh(g, skin))
            for (geo::Index p = 0; p < m->pointCapacity(); ++p)
                if (m->pointValid(p))
                    out.push_back(p);
    return out;
}

int maxInfOf(Graph& g, NodeId skin) { return int(g.node(skin)->as<int64_t>(SkinClusterNode::kMaxInfluences)); }

/** Splices a node out of a mesh stack and deletes it. */
void unlinkModifier(Graph& g, NodeId id)
{
    auto src = g.source({id, SkinClusterNode::kInMesh, -1});
    std::vector<PlugRef> dsts = g.destinations({id, SkinClusterNode::kOutMesh, -1});
    for (const PlugRef& d : dsts)
        g.disconnect(d);
    if (src)
        for (const PlugRef& d : dsts)
            g.connect(*src, d);
    g.deleteNode(id);
}

void registerSkinCommandsImpl()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "skinCluster",
        .doc = "Bind a mesh to joints. Give the joints and the mesh in any order (or select them). With "
               "bindTo=\"hierarchy\" every joint below the given ones is an influence too. Weights start from "
               "closest-bone distance; method=\"heatmap\" also ignores bones hidden behind the surface and smooths. "
               "Returns the skinCluster.",
        .positionalDoc = "joints..., mesh",
        .flags = {
            {"maxInfluences", "mi", T::Int, 4, "Influences per point (4 is mobile-safe, Unreal desktop takes 8+).", {}, {}, 1.0},
            {"method", "m", T::String, "closest", "closest | heatmap"},
            {"dropoffRate", "dr", T::Float, 4.0, "How fast a bone's weight falls off with distance.", {}, {}, 0.0},
            {"bindTo", "bt", T::String, "hierarchy", "hierarchy | selected"},
            {"skinningMethod", "sm", T::String, "linear", "linear | dualQuaternion | centresOfRotation | directDeltaMush"},
            {"name", "n", T::String, nullptr, "Name for the skinCluster."},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            NodeId mesh = kNoNode;
            std::vector<NodeId> joints;
            for (const std::string& name : a.objects())
            {
                Node& n = nodeNamed(name, "skinCluster");
                if (n.type().isA("mesh"))
                    mesh = n.id();
                else if (rig::isJoint(n))
                    joints.push_back(n.id());
                else
                    throw CommandError("skinCluster: " + name + " is neither a joint nor a mesh");
            }
            if (mesh == kNoNode || joints.empty())
                throw CommandError("skinCluster: give at least one joint and a mesh");
            if (skin::find(g, mesh) != kNoNode)
                throw CommandError("skinCluster: " + g.node(mesh)->path() + " is already skinned");
            if (a.get<std::string>("bindTo") == "hierarchy")
            {
                size_t given = joints.size();
                for (size_t i = 0; i < given; ++i)
                    for (NodeId d : g.descendants(joints[i]))
                        if (rig::isJoint(*g.node(d)) && std::find(joints.begin(), joints.end(), d) == joints.end())
                            joints.push_back(d);
            }
            int maxInf = influenceCap(a.get<int>("maxInfluences"));
            std::string method = a.get<std::string>("method");
            if (method != "closest" && method != "heatmap")
                throw CommandError("skinCluster: method must be closest or heatmap");

            // Insert at the top of the stack, after any modifiers, like a Maya deformer.
            g.evaluate();
            PlugRef top{mesh, MeshNode::kInMesh, -1};
            auto src = g.source(top);
            std::string name = a.has("name") ? a.get<std::string>("name") : "SkinCluster";
            Node* s = guarded([&] { return g.createNode(*types::find("skinCluster"), name, kNoNode, true); });
            NodeId skin = s->id();
            if (src)
            {
                g.disconnect(top);
                g.connect(*src, {skin, SkinClusterNode::kInMesh, -1});
            }
            else
            {
                Node* source = g.createNode(*types::find("meshSource"), "meshSource", kNoNode, true);
                g.setValue({source->id(), MeshSourceNode::kMesh, -1}, g.storedValue(top));
                g.setValue(top, MeshPtr());
                g.connect({source->id(), MeshSourceNode::kOutMesh, -1}, {skin, SkinClusterNode::kInMesh, -1});
            }
            g.connect({skin, SkinClusterNode::kOutMesh, -1}, top);
            g.setValue({skin, SkinClusterNode::kMaxInfluences}, int64_t(maxInf));
            g.setValue({skin, SkinClusterNode::kSkinningMethod}, skin::methodOf(a.get<std::string>("skinningMethod"), "skinCluster"));
            glm::dmat4 geom = worldOf(*g.node(mesh));
            g.setValue({skin, SkinClusterNode::kGeomMatrix}, geom);
            for (NodeId j : joints)
                skin::addInfluence(g, skin, j);

            MeshPtr input = skin::inputMesh(g, skin);
            if (!input)
                throw CommandError("skinCluster: the mesh has no geometry");
            geo::Mesh world = worldCopy(*input, geom);
            std::unique_ptr<geo::MeshBvh> bvh;
            if (method == "heatmap")
                bvh = std::make_unique<geo::MeshBvh>(world);
            g.evaluate();
            std::vector<skin::Influence> infs = skin::influences(g, skin);
            WeightTable t = closestWeights(world, skinBones(g, infs), maxInf, a.get<double>("dropoffRate"), bvh.get());
            if (method == "heatmap")
                smoothWeights(world, t, 6, maxInf, {});
            for (size_t p = 0; p < t.points.size(); ++p)
                t.markPointAutomatic(p);
            skin::setWeights(g, skin, t);
            skin::reportAutomatic(g);
            return g.node(skin)->path();
        },
    });

    ops::registerCommand({
        .name = "skinPercent",
        .doc = "Set exact weights: transformValue=[[joint, weight], ...] on the given (or selected) vertices; the "
               "other unlocked influences rescale to fill the rest. Without transformValue returns "
               "[{point, weights: {joint: w}}].",
        .positionalDoc = "mesh",
        .flags = {
            {"vertices", "v", T::List, nullptr, "Point ids; defaults to the selected vertices."},
            {"transformValue", "tv", T::Any, nullptr, "[[joint, weight], ...] or {joint: weight}"},
            {"normalize", "nrm", T::Bool, true, "Rescale the other influences so each point sums to 1."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("skinPercent: give a skinned mesh");
            NodeId skin = skin::resolve(g, objs.back(), "skinPercent");
            auto infs = skin::influences(g, skin);
            skin::WeightEdit edit(g, skin);
            WeightTable& t = edit.table();
            std::vector<uint32_t> pts = pointsArg(a, g, skin, !a.has("transformValue"));
            for (uint32_t p : pts)
                if (p >= t.points.size())
                    throw CommandError("skinPercent: point " + std::to_string(p) + " is out of range");
            if (!a.has("transformValue"))
            {
                // A query reads the weights the skin uses, not the active layer's.
                t = skin::table(g, skin);
                std::unordered_map<uint32_t, std::string> names;
                for (const auto& i : infs)
                    names[i.index] = i.joint != kNoNode ? g.path(i.joint) : i.name;
                json out = json::array();
                for (uint32_t p : pts)
                {
                    json w = json::object();
                    for (auto& [i, wt] : t.points[p])
                        w[names.contains(i) ? names[i] : std::to_string(i)] = wt;
                    out.push_back({{"point", p}, {"weights", w}});
                }
                return out;
            }
            if (pts.empty())
                throw CommandError("skinPercent: no vertices given or selected");
            std::vector<char> locked = skin::lockMask(infs);
            std::vector<char> mask = locked;
            std::vector<std::pair<uint32_t, float>> values;
            json tv = a.flags["transformValue"];
            if (tv.is_object())
            {
                json pairs = json::array();
                for (auto& [k, v] : tv.items())
                    pairs.push_back({k, v});
                tv = pairs;
            }
            for (const json& pair : tv)
            {
                if (!pair.is_array() || pair.size() != 2)
                    throw CommandError("skinPercent: transformValue entries are [joint, weight]");
                int64_t idx = skin::indexOf(infs, pair[0].get<std::string>());
                if (idx < 0)
                    throw CommandError("skinPercent: " + pair[0].get<std::string>() + " is not an influence");
                values.push_back({uint32_t(idx), std::clamp(pair[1].get<float>(), 0.0f, 1.0f)});
                if (size_t(idx) >= mask.size())
                    mask.resize(size_t(idx) + 1, 0);
                mask[size_t(idx)] = 1;
            }
            for (uint32_t p : pts)
            {
                for (auto& [i, wt] : values)
                {
                    t.set(p, i, wt);
                    t.markAuthored(p, i);
                }
                if (a.get<bool>("normalize"))
                    t.normalize(p, mask);
            }
            edit.commit();
            skin::reportAutomatic(g);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "copySkinWeights",
        .doc = "Copy weights from one skinned mesh to another by closest surface point (or point id). Influences match "
               "by joint name; missing ones are added to the destination.",
        .positionalDoc = "source, destination",
        .flags = {{"surfaceAssociation", "sa", T::String, "closestPoint", "closestPoint | pointIndex"}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            auto objs = a.objects();
            if (objs.size() < 2)
                throw CommandError("copySkinWeights: give a source and a destination");
            NodeId from = skin::resolve(g, objs[0], "copySkinWeights");
            NodeId to = skin::resolve(g, objs[1], "copySkinWeights");
            auto srcInfs = skin::influences(g, from);
            std::unordered_map<uint32_t, uint32_t> remap;
            for (const auto& i : srcInfs)
                if (i.joint != kNoNode)
                    remap[i.index] = skin::addInfluence(g, to, i.joint);

            MeshPtr srcMesh = skin::inputMesh(g, from), dstMesh = skin::inputMesh(g, to);
            if (!srcMesh || !dstMesh)
                throw CommandError("copySkinWeights: a mesh has no geometry");
            WeightTable src = skin::table(g, from);
            WeightTable dst = WeightTable::from(nullptr, dstMesh->pointCapacity());
            int maxInf = maxInfOf(g, to);
            if (a.get<std::string>("surfaceAssociation") == "pointIndex")
            {
                for (size_t p = 0; p < std::min(src.points.size(), dst.points.size()); ++p)
                    for (auto& [i, wt] : src.points[p])
                        if (remap.contains(i))
                            dst.set(p, remap[i], wt);
            }
            else
            {
                geo::Mesh srcWorld = worldCopy(*srcMesh, g.node(from)->as<glm::dmat4>(SkinClusterNode::kGeomMatrix));
                geo::Mesh dstWorld = worldCopy(*dstMesh, g.node(to)->as<glm::dmat4>(SkinClusterNode::kGeomMatrix));
                geo::MeshBvh bvh(srcWorld);
                const auto& tris = bvh.triangles();
                tbb::parallel_for(tbb::blocked_range<geo::Index>(0, dstWorld.pointCapacity(), 256), [&](const tbb::blocked_range<geo::Index>& r) {
                    for (geo::Index p = r.begin(); p != r.end(); ++p)
                    {
                        geo::BvhClosest hit;
                        if (!dstWorld.pointValid(p) || !bvh.closestPoint(dstWorld.position(p), hit))
                            continue;
                        std::unordered_map<uint32_t, float> acc;
                        for (int k = 0; k < 3; ++k)
                        {
                            geo::Index sp = srcWorld.cornerPoint(tris.corners[size_t(hit.triangle) * 3 + k]);
                            if (sp < src.points.size())
                                for (auto& [i, wt] : src.points[sp])
                                    if (remap.contains(i))
                                        acc[remap.at(i)] += wt * float(hit.bary[k]);
                        }
                        dst.points[p].assign(acc.begin(), acc.end());
                    }
                });
            }
            std::vector<char> none;
            for (size_t p = 0; p < dst.points.size(); ++p)
            {
                dst.prune(p, maxInf, 1e-4f, none);
                dst.markPointAutomatic(p);
            }
            skin::WeightEdit edit(g, to);
            edit.table() = std::move(dst);
            edit.commit();
            skin::reportAutomatic(g);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "mirrorSkinWeights",
        .doc = "Mirror weights across a world plane through the origin: points on the source side copy to their mirror "
               "partners, with influences swapped by searchReplace (an influence with no partner maps to itself).",
        .positionalDoc = "mesh",
        .flags = {
            {"axis", "ax", T::String, "x", "Plane normal: x mirrors across YZ."},
            {"searchReplace", "sr", T::List, json::array({"_l", "_r"}), "[search, replace] pairing left and right joints."},
            {"positiveToNegative", "ptn", T::Bool, true, "Copy from the + side to the - side."},
            {"tolerance", "tol", T::Float, 0.1, "How far a mirrored point may be from its partner, cm.", {}, {}, 0.0},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("mirrorSkinWeights: give a skinned mesh");
            NodeId skin = skin::resolve(g, objs.back(), "mirrorSkinWeights");
            glm::dvec3 axis;
            try
            {
                axis = rig::axisVector(a.get<std::string>("axis"));
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(std::string("mirrorSkinWeights: ") + e.what());
            }
            json sr = a.flags["searchReplace"];
            std::string search = sr.size() > 0 ? sr[0].get<std::string>() : "", replace = sr.size() > 1 ? sr[1].get<std::string>() : "";
            auto infs = skin::influences(g, skin);
            std::unordered_map<uint32_t, uint32_t> partner;
            for (const auto& i : infs)
            {
                std::string other = rig::mirrorName(i.name, search, replace);
                if (other == i.name)
                    other = rig::mirrorName(i.name, replace, search);
                int64_t j = skin::indexOf(infs, other);
                partner[i.index] = j >= 0 ? uint32_t(j) : i.index;
            }
            MeshPtr m = skin::inputMesh(g, skin);
            if (!m)
                throw CommandError("mirrorSkinWeights: the mesh has no geometry");
            geo::Mesh world = worldCopy(*m, g.node(skin)->as<glm::dmat4>(SkinClusterNode::kGeomMatrix));
            geo::SymmetryPlane plane{axis, 0.0};
            geo::SymmetryMap map = geo::SymmetryMap::build(world, plane, a.get<double>("tolerance"));
            skin::WeightEdit edit(g, skin);
            WeightTable& t = edit.table();
            double side = a.get<bool>("positiveToNegative") ? 1.0 : -1.0;
            int copied = 0;
            for (geo::Index p = 0; p < world.pointCapacity() && p < t.points.size(); ++p)
            {
                if (!world.pointValid(p) || plane.distance(world.position(p)) * side <= 0.0)
                    continue;
                geo::Index q = map.mirrorPoint(p);
                if (q == geo::kInvalid || q == p || q >= t.points.size())
                    continue;
                auto& dst = t.points[q];
                dst.clear();
                t.markPointAuthored(q);
                for (auto& [i, wt] : t.points[p])
                {
                    const uint32_t to = partner.contains(i) ? partner[i] : i;
                    dst.push_back({to, wt});
                    if (t.isAutomatic(p, i))
                        t.markAutomatic(q, to);
                }
                ++copied;
            }
            edit.commit();
            skin::reportAutomatic(g);
            return copied;
        },
    });

    ops::registerCommand({
        .name = "normalizeWeights",
        .doc = "Make each point's weights sum to 1 (locked influences keep their values). Defaults to every point.",
        .positionalDoc = "mesh",
        .flags = {{"vertices", "v", T::List, nullptr, "Point ids; defaults to the selected vertices, else all."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("normalizeWeights: give a skinned mesh");
            NodeId skin = skin::resolve(g, objs.back(), "normalizeWeights");
            skin::WeightEdit edit(g, skin);
            WeightTable& t = edit.table();
            for (uint32_t p : pointsArg(a, g, skin, true))
                if (p < t.points.size())
                    t.normalize(p, edit.locked());
            edit.commit();
            skin::reportAutomatic(g);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "pruneWeights",
        .doc = "Drop weights below a threshold and beyond the skin's maxInfluences, then renormalize.",
        .positionalDoc = "mesh",
        .flags = {
            {"threshold", "t", T::Float, 0.01, "Smallest weight kept.", {}, {}, 0.0, 1.0},
            {"vertices", "v", T::List, nullptr, "Point ids; defaults to the selected vertices, else all."},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("pruneWeights: give a skinned mesh");
            NodeId skin = skin::resolve(g, objs.back(), "pruneWeights");
            skin::WeightEdit edit(g, skin);
            WeightTable& t = edit.table();
            float threshold = a.get<float>("threshold");
            for (uint32_t p : pointsArg(a, g, skin, true))
                if (p < t.points.size())
                    t.prune(p, edit.maxInfluences(), threshold, edit.locked());
            edit.commit();
            skin::reportAutomatic(g);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "skinInfo",
        .doc = "A skinned mesh's skinCluster, influences, influence limit and weight health.",
        .positionalDoc = "mesh",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("skinInfo: give a skinned mesh");
            NodeId skin = skin::resolve(g, objs.back(), "skinInfo");
            auto infs = skin::influences(g, skin);
            WeightTable t = skin::table(g, skin);
            json names = json::array(), locked = json::array();
            for (const auto& i : infs)
            {
                names.push_back(i.name);
                if (i.locked)
                    locked.push_back(i.name);
            }
            int maxUsed = 0, unnormalized = 0, unweighted = 0, automatic = 0;
            MeshPtr m = skin::inputMesh(g, skin);
            for (size_t p = 0; p < t.points.size(); ++p)
            {
                if (m && !m->pointValid(geo::Index(p)))
                    continue;
                maxUsed = std::max(maxUsed, int(t.points[p].size()));
                float sum = 0.0f;
                for (auto& [i, wt] : t.points[p])
                    sum += wt;
                automatic += t.pointAutomatic(p);
                if (t.points[p].empty())
                    ++unweighted;
                else if (std::abs(sum - 1.0f) > 1e-3f)
                    ++unnormalized;
            }
            NodeId mesh = skin::meshOf(g, skin);
            return {
                {"skinCluster", g.node(skin)->path()},
                {"mesh", mesh != kNoNode ? json(g.node(mesh)->path()) : json(nullptr)},
                {"influences", names},
                {"locked", locked},
                {"maxInfluences", maxInfOf(g, skin)},
                {"maxUsed", maxUsed},
                {"points", t.points.size()},
                {"unnormalized", unnormalized},
                {"unweighted", unweighted},
                {"automatic", automatic},
                {"layers", [&] {
                     json layerNames = json::array();
                     if (auto l = skin::layers(g, skin))
                         for (const SkinLayer& layer : l->layers)
                             layerNames.push_back(layer.name);
                     return layerNames;
                 }()},
            };
        },
    });

    ops::registerCommand({
        .name = "unbindSkin",
        .doc = "Remove a mesh's skinCluster and its influence links; the mesh returns to its bind shape.",
        .positionalDoc = "mesh",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            for (const std::string& name : a.objects())
            {
                Node* n = g.find(name);
                if (n && rig::isJoint(*n))
                    continue;
                NodeId skin = skin::resolve(g, name, "unbindSkin");
                std::vector<skin::Influence> infs = skin::influences(g, skin);
                unlinkModifier(g, skin);
                for (const auto& i : infs)
                    g.deleteNode(i.link);
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "addInfluence",
        .doc = "Add joints to a skinCluster with zero weight; their current pose becomes their bind pose.",
        .positionalDoc = "mesh, joints...",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            auto objs = a.objects();
            NodeId skin = kNoNode;
            std::vector<NodeId> joints;
            for (const std::string& name : objs)
            {
                Node& n = nodeNamed(name, "addInfluence");
                if (rig::isJoint(n))
                    joints.push_back(n.id());
                else
                    skin = skin::resolve(g, name, "addInfluence");
            }
            if (skin == kNoNode || joints.empty())
                throw CommandError("addInfluence: give a skinned mesh and joints");
            for (NodeId j : joints)
                skin::addInfluence(g, skin, j);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "removeInfluence",
        .doc = "Remove joints from a skinCluster; their weight goes to the remaining influences.",
        .positionalDoc = "mesh, joints...",
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            NodeId skin = kNoNode;
            std::vector<std::string> joints;
            for (const std::string& name : a.objects())
            {
                Node& n = nodeNamed(name, "removeInfluence");
                if (rig::isJoint(n))
                    joints.push_back(name);
                else
                    skin = skin::resolve(g, name, "removeInfluence");
            }
            if (skin == kNoNode || joints.empty())
                throw CommandError("removeInfluence: give a skinned mesh and joints");
            auto infs = skin::influences(g, skin);
            WeightTable t = skin::table(g, skin);
            auto locked = skin::lockMask(infs);
            for (const std::string& j : joints)
            {
                auto it = std::find_if(infs.begin(), infs.end(), [&](const skin::Influence& i) { return i.name == j; });
                if (it == infs.end())
                    throw CommandError("removeInfluence: " + j + " is not an influence");
                for (size_t p = 0; p < t.points.size(); ++p)
                {
                    std::erase_if(t.points[p], [&](const WeightTable::Entry& e) { return e.first == it->index; });
                    t.normalize(p, locked);
                }
                if (auto l = skin::layers(g, skin))
                {
                    auto next = std::make_shared<SkinLayers>(*l);
                    for (SkinLayer& layer : next->layers)
                        for (size_t p = 0; p < layer.weights.points.size(); ++p)
                        {
                            std::erase_if(layer.weights.points[p], [&](const WeightTable::Entry& e) { return e.first == it->index; });
                            layer.weights.normalize(p, locked);
                        }
                    skin::setLayers(g, skin, next);
                }
                // Splice the link out of the chain.
                auto prev = g.source({it->link, SkinInfluenceNode::kPrevious, -1});
                auto dsts = g.destinations({it->link, SkinInfluenceNode::kLink, -1});
                g.deleteNode(it->link);
                if (prev)
                    for (const PlugRef& d : dsts)
                        g.connect(*prev, d);
            }
            skin::setWeights(g, skin, t);
            skin::reportAutomatic(g);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "goToBindPose",
        .doc = "Put a skinned mesh's joints back where they were bound.",
        .positionalDoc = "mesh",
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("goToBindPose: give a skinned mesh");
            NodeId skin = skin::resolve(g, objs.back(), "goToBindPose");
            auto infs = skin::influences(g, skin);
            std::vector<std::pair<int, const skin::Influence*>> order;
            for (const auto& i : infs)
            {
                if (i.joint == kNoNode)
                    continue;
                int depth = 0;
                for (NodeId p = g.parent(i.joint); p != kNoNode; p = g.parent(p))
                    ++depth;
                order.push_back({depth, &i});
            }
            std::stable_sort(order.begin(), order.end(), [](auto& x, auto& y) { return x.first < y.first; });
            for (auto& [depth, i] : order)
            {
                g.evaluate();
                glm::dmat4 bind = glm::inverse(g.node(i->link)->as<glm::dmat4>(SkinInfluenceNode::kBindPreMatrix));
                glm::dmat4 parent = g.node(i->joint)->as<glm::dmat4>(Transform::kParentMatrix);
                guarded([&] { Transform::setLocalMatrix(g, i->joint, glm::inverse(parent) * bind); return 0; });
            }
            return nullptr;
        },
    });
}
} // namespace

// ------------------------------------------------------------------------------------------------ registration

void registerSkinTypes()
{
    if (types::find("skinCluster"))
        return;
    data::registerKind("skinWeights", &SkinWeights::fromJson);
    data::registerKind("skinLayers", &SkinLayers::fromJson);

    NodeType inf;
    inf.name = "skinInfluence";
    inf.ownAttrs = {
        {.name = "matrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden,
         .doc = "The joint's worldMatrix."},
        {.name = "bindPreMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Inverse of the joint's world matrix at bind time."},
        {.name = "index", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable,
         .doc = "Influence id the weights refer to.", .minValue = 0.0},
        {.name = "lockWeights", .type = PlugType::Bool, .defaultValue = false, .flags = AttrInput | AttrStorable,
         .doc = "Painting and normalizing leave this influence's weights alone."},
        {.name = "previous", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrHidden,
         .doc = "The link before this one."},
        {.name = "link", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrOutput | AttrHidden,
         .doc = "This influence plus everything before it."},
    };
    inf.create = [] { return std::make_unique<SkinInfluenceNode>(); };
    types::registerType(std::move(inf));

    NodeType sc;
    sc.name = "skinCluster";
    sc.icon = "topology-star";
    sc.doc = "Deforms the mesh by its bound joints, each point following its joint weights. Made by Bind Skin; paint the "
             "weights to shape how it bends.";
    sc.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
        {.name = "enabled", .type = PlugType::Bool, .defaultValue = true, .flags = AttrInput | AttrStorable | AttrKeyable,
         .doc = "Turn skinning off without unbinding."},
        {.name = "envelope", .type = PlugType::Float, .defaultValue = 1.0, .flags = AttrInput | AttrStorable | AttrKeyable,
         .doc = "Blend between the bind shape (0) and the skinned shape (1).", .minValue = 0.0, .maxValue = 1.0},
        {.name = "skinningMethod", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable,
         .enumNames = {"linear", "dualQuaternion", "centresOfRotation", "directDeltaMush"},
         .doc = "Unreal skins linearly; dual quaternion keeps volume at twists; centres of rotation keeps volume without dual "
                "quaternion bulges; direct delta mush smooths like a delta mush in one pass. The last three export as linear "
                "weights fitted to them (bakeSkinWeights)."},
        {.name = "normalizeWeights", .type = PlugType::Bool, .defaultValue = true, .flags = AttrInput | AttrStorable,
         .doc = "Divide by each point's weight sum when skinning."},
        {.name = "maxInfluences", .type = PlugType::Int, .defaultValue = int64_t(4), .flags = AttrInput | AttrStorable,
         .doc = "Influences per point kept by binding, painting and pruning.", .minValue = 1.0},
        {.name = "geomMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "The mesh's world matrix at bind time."},
        {.name = "weights", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Per-point weights."},
        {.name = "influences", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrHidden,
         .doc = "The last skinInfluence link."},
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
        {.name = "ddmIterations", .type = PlugType::Int, .defaultValue = int64_t(20), .flags = AttrInput | AttrStorable,
         .doc = "Direct delta mush: smoothing passes; more spreads the smoothing further.", .minValue = 0.0},
        {.name = "ddmSmoothStep", .type = PlugType::Float, .defaultValue = 0.5, .flags = AttrInput | AttrStorable,
         .doc = "Direct delta mush: how far each pass moves toward the neighbours, 0 to 1.", .minValue = 0.0, .maxValue = 1.0},
        {.name = "corSigma", .type = PlugType::Float, .defaultValue = 0.1, .flags = AttrInput | AttrStorable,
         .doc = "Centres of rotation: how alike two points' weights must be to share a centre.", .minValue = 0.0},
        {.name = "exportWeights", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Linear weights fitted by bakeSkinWeights keep=true, exported in place of fitting at export time."},
        {.name = "weightLayers", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Weight layers whose composite is the weights (see skinLayers)."},
    };
    sc.create = [] {
        auto n = std::make_unique<SkinClusterNode>();
        n->inAttr = SkinClusterNode::kInMesh;
        n->enabledAttr = SkinClusterNode::kEnabled;
        n->outAttr = SkinClusterNode::kOutMesh;
        n->apply = [](const geo::Mesh& m, const Node&) { return m; };
        return n;
    };
    types::registerType(std::move(sc));
}

void registerSkinCommands()
{
    registerSkinCommandsImpl();
    skinpaint::registerCommands();
    weightproxy::registerCommands();
}
} // namespace lila::scene
