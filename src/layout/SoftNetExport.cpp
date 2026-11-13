#include "layout/SoftNetBind.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "math/Unreal.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "scene/Anim.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Morphs.h"
#include "scene/Node.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/RotationCurve.h"
#include "scene/Skin.h"
#include "scene/SkinBake.h"
#include "scene/Transform.h"
#include "scene/UnrealCheck.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <Eigen/Dense>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numeric>
#include <random>
#include <set>
#include <map>
#include <string>

namespace lila::layout
{
namespace
{
using namespace lila::scene;
using namespace lila::scene::rigutil;

struct Range
{
    std::vector<double> times;
    double fps = 30.0;
};

Range rangeOf(const Args& a)
{
    const anim::Playback& pb = anim::playback();
    Range r;
    r.fps = std::max(pb.fps, 1e-6);
    const int64_t start = a.has("start") ? a.get<int>("start") : int64_t(std::llround(pb.minTime * r.fps));
    const int64_t end = a.has("end") ? a.get<int>("end") : int64_t(std::llround(pb.maxTime * r.fps));
    for (int64_t f = start; f <= end; f += std::max(1, a.get<int>("step", 1)))
        r.times.push_back(double(f) / r.fps);
    if (r.times.empty())
        r.times.push_back(Graph::current().time());
    return r;
}

std::vector<glm::dvec3> outPoints(Graph& g, NodeId mesh)
{
    const MeshPtr m = g.node(mesh)->as<MeshPtr>(MeshNode::kOutMesh);
    return m ? std::vector<glm::dvec3>(m->positions().begin(), m->positions().end()) : std::vector<glm::dvec3>{};
}

void writeLocal(Graph& g, NodeId j, const glm::dmat4& local, std::optional<double> time, glm::dvec3& near)
{
    near = eulerNear(orthonormal(glm::dmat3(local)), RotateOrder(g.node(j)->as<int64_t>(Transform::kRotateOrder)), near);
    if (!time)
    {
        g.setValue({j, Transform::kTranslate, -1}, glm::dvec3(local[3]));
        g.setValue({j, Transform::kRotate, -1}, near);
        return;
    }
    for (int c = 0; c < 3; ++c)
        anim::setKey(g, {j, Transform::kTranslate, c}, *time, local[3][c]);
    anim::setRotationKey(g, {j, Transform::kRotate, -1}, *time, near);
}

/** The net's knots and handles as joints Unreal can carry, with each one's world matrix while the controls rest. */
struct NetJoints
{
    /** The bones a bake skins to: frames, controls and handles. */
    std::vector<NodeId> joints;
    std::vector<glm::dmat4> restWorld;
    /** Per knot index: its frame and control joints. */
    std::vector<std::pair<NodeId, NodeId>> knots;
    /** Per tangent handle node: its joint and its rest joint (under the frame, where the handle sits at rest). */
    std::map<NodeId, std::pair<NodeId, NodeId>> handles;
};

/**
 * Joints for a net, keyed over times: per knot a frame joint riding the surface (its frame) with a control joint under
 * it (the knot control), per tangent handle a joint under its knot's control. Frame joints go under the skin influence
 * that weighs most at the knot. Joints made earlier for the net are replaced.
 */
NetJoints makeJoints(Graph& g, NodeId hub, NodeId skin, const std::vector<double>& times)
{
    LILA_ZONE();
    const Node& h = *g.node(hub);
    const NetBind bind = NetBind::fromJson(h.as<std::string>(h.type().attrIndex("binding")));
    const glm::dmat4 meshWorld = h.as<glm::dmat4>(h.type().attrIndex("meshWorld"));
    const NodeType* knotType = types::find(kSoftNetKnotType);
    std::vector<NodeId> knots(bind.knots.size(), kNoNode);
    for (NodeId c : g.children(hub))
        if (const Node* n = g.node(c); n && n->type().name == kSoftNetKnotType)
            if (const int64_t i = n->as<int64_t>(knotType->attrIndex("index")); i >= 0 && size_t(i) < knots.size())
                knots[size_t(i)] = c;

    // The influence weighing most at each knot's surface point carries its frame joint.
    std::vector<NodeId> parentOf(knots.size(), kNoNode);
    if (skin != kNoNode)
    {
        const WeightTable t = skin::table(g, skin);
        std::map<uint32_t, NodeId> jointOf;
        for (const skin::Influence& i : skin::influences(g, skin))
            jointOf[i.index] = i.joint;
        for (size_t k = 0; k < knots.size(); ++k)
        {
            const glm::dvec3& w = bind.knots[k].at.bary;
            const int corner = w.x >= w.y && w.x >= w.z ? 0 : w.y >= w.z ? 1 : 2;
            const uint32_t p = bind.knots[k].at.points[size_t(corner)];
            float best = -1.0f;
            if (p < t.points.size())
                for (const auto& [inf, wt] : t.points[p])
                    if (wt > best && jointOf.contains(inf))
                    {
                        best = wt;
                        parentOf[k] = jointOf[inf];
                    }
        }
    }

    // What each joint follows: a frame joint the knot's frame (its parentMatrix), a control joint the knot's local
    // matrix, a handle joint the handle's rest offset times its local matrix.
    enum class Kind
    {
        Frame,
        Control,
        Handle,
        HandleRest
    };
    struct Made
    {
        NodeId joint;
        NodeId source;
        Kind kind;
    };
    std::vector<Made> made;
    NetJoints out;
    const NodeType& jointType = *types::find("joint");
    auto replace = [&](const std::string& name) {
        if (Node* old = g.find(name); old && rig::isJoint(*old))
            g.deleteNode(old->id());
    };
    for (size_t k = 0; k < knots.size(); ++k)
    {
        if (knots[k] == kNoNode)
            continue;
        const std::string name = g.node(knots[k])->name();
        replace(name + "_frame");
        replace(name + "_jnt");
        Node* frame = g.createNode(jointType, name + "_frame", parentOf[k]);
        Node* control = g.createNode(jointType, name + "_jnt", frame->id());
        glm::dmat4 rest(bind.knots[k].rest);
        rest[3] = glm::dvec4(bind.knots[k].origin, 1.0);
        rest = meshWorld * rest;
        made.push_back({frame->id(), knots[k], Kind::Frame});
        out.restWorld.push_back(rest);
        made.push_back({control->id(), knots[k], Kind::Control});
        out.restWorld.push_back(rest);
        out.knots.resize(std::max(out.knots.size(), k + 1), {kNoNode, kNoNode});
        out.knots[k] = {frame->id(), control->id()};
        for (NodeId t : std::vector<NodeId>(g.children(knots[k])))
            if (const Node* tn = g.node(t); tn && tn->type().name == kSoftNetTangentType)
            {
                replace(tn->name() + "_jnt");
                replace(tn->name() + "_rest");
                const NodeId handle = g.createNode(jointType, tn->name() + "_jnt", control->id())->id();
                const NodeId restJoint = g.createNode(jointType, tn->name() + "_rest", frame->id())->id();
                made.push_back({handle, t, Kind::Handle});
                out.restWorld.push_back(rest * tn->as<glm::dmat4>(Transform::kOffsetParentMatrix));
                made.push_back({restJoint, t, Kind::HandleRest});
                out.restWorld.push_back(rest * tn->as<glm::dmat4>(Transform::kOffsetParentMatrix));
                out.handles[t] = {handle, restJoint};
            }
    }

    std::vector<std::vector<glm::dmat4>> local(made.size());
    const double saved = g.time();
    for (double t : times)
    {
        g.setTime(t);
        g.evaluate();
        for (size_t i = 0; i < made.size(); ++i)
        {
            const Node& src = *g.node(made[i].source);
            switch (made[i].kind)
            {
            case Kind::Frame:
            {
                const NodeId p = g.parent(made[i].joint);
                local[i].push_back(glm::inverse(p != kNoNode ? worldOf(*g.node(p)) : glm::dmat4(1.0)) * src.as<glm::dmat4>(Transform::kParentMatrix));
                break;
            }
            case Kind::Control: local[i].push_back(src.as<glm::dmat4>(Transform::kMatrix)); break;
            case Kind::Handle: local[i].push_back(src.as<glm::dmat4>(Transform::kOffsetParentMatrix) * src.as<glm::dmat4>(Transform::kMatrix)); break;
            case Kind::HandleRest: local[i].push_back(src.as<glm::dmat4>(Transform::kOffsetParentMatrix)); break;
            }
        }
    }
    g.setTime(saved);
    g.evaluate();
    for (size_t i = 0; i < made.size(); ++i)
    {
        bool animated = false;
        for (size_t f = 1; f < local[i].size() && !animated; ++f)
            for (int c = 0; c < 4; ++c)
                animated = animated || glm::length(local[i][f][c] - local[i][0][c]) > 1e-9;
        glm::dvec3 near(0.0);
        for (size_t f = 0; f < times.size(); ++f)
        {
            writeLocal(g, made[i].joint, local[i][f], animated ? std::optional<double>(times[f]) : std::nullopt, near);
            if (!animated)
                break;
        }
        if (made[i].kind != Kind::HandleRest)
            out.joints.push_back(made[i].joint);
    }
    // restWorld lines up with joints: drop the rest joints' entries.
    std::vector<glm::dmat4> rests;
    for (size_t i = 0; i < made.size(); ++i)
        if (made[i].kind != Kind::HandleRest)
            rests.push_back(out.restWorld[i]);
    out.restWorld = std::move(rests);
    return out;
}

/** Each point's linear-skinning matrix (rest to posed, object space) from the skin's weights and influence matrices. */
std::vector<glm::dmat3> blendedLinear(Graph& g, NodeId skin, size_t points)
{
    const std::vector<glm::dmat4> m = skin::objectMatrices(g, skin);
    const WeightTable t = skin::table(g, skin);
    std::vector<glm::dmat3> out(points, glm::dmat3(1.0));
    for (size_t p = 0; p < points && p < t.points.size(); ++p)
    {
        glm::dmat3 b(0.0);
        double sum = 0.0;
        for (const auto& [i, w] : t.points[p])
            if (i < m.size())
            {
                b += double(w) * glm::dmat3(m[i]);
                sum += w;
            }
        if (sum > 0.0)
            out[p] = b;
    }
    return out;
}

struct Errors
{
    double rms = 0.0, max = 0.0;
};

Errors errorsAgainst(Graph& g, NodeId mesh, const std::vector<double>& times, const std::vector<std::vector<glm::dvec3>>& targets, std::vector<double>* perFrame = nullptr)
{
    Errors e;
    double sum = 0.0;
    size_t n = 0;
    const double saved = g.time();
    for (size_t f = 0; f < times.size(); ++f)
    {
        g.setTime(times[f]);
        g.evaluate();
        const std::vector<glm::dvec3> pts = outPoints(g, mesh);
        double frameMax = 0.0;
        for (size_t p = 0; p < pts.size() && p < targets[f].size(); ++p)
        {
            const double d = glm::length(pts[p] - targets[f][p]);
            sum += d * d;
            ++n;
            frameMax = std::max(frameMax, d);
        }
        e.max = std::max(e.max, frameMax);
        if (perFrame)
            perFrame->push_back(frameMax);
    }
    g.setTime(saved);
    g.evaluate();
    e.rms = n ? std::sqrt(sum / double(n)) : 0.0;
    return e;
}

/**
 * A pose feature a corrective reads: a translate component as it is, a rotate component as its sine and versine, which
 * a turn's matrix (and so the skin's residual) is linear in.
 */
struct Channel
{
    PlugRef plug;
    enum Form
    {
        Linear,
        Sine,
        Versine
    } form = Linear;
};

/** The features correctives read: translate and rotate of the net's control and handle joints, rotate of the skin's own joints. */
std::vector<Channel> poseChannels(Graph& g, const std::vector<NodeId>& netJoints, const std::vector<NodeId>& skinJoints)
{
    std::vector<Channel> out;
    auto turns = [&](NodeId j) {
        for (int c = 0; c < 3; ++c)
            for (Channel::Form f : {Channel::Sine, Channel::Versine})
                out.push_back({{j, int(Transform::kRotate), c}, f});
    };
    for (NodeId j : netJoints)
    {
        for (int c = 0; c < 3; ++c)
            out.push_back({{j, int(Transform::kTranslate), c}, Channel::Linear});
        turns(j);
    }
    for (NodeId j : skinJoints)
        turns(j);
    // Expressions name plugs without a path.
    std::erase_if(out, [&](const Channel& ch) { return g.plugName(ch.plug).find('|') != std::string::npos; });
    return out;
}

double formOf(Channel::Form form, double v)
{
    switch (form)
    {
    case Channel::Sine: return std::sin(glm::radians(v));
    case Channel::Versine: return 1.0 - std::cos(glm::radians(v));
    default: return v;
    }
}

double channelValue(Graph& g, const Channel& ch) { return formOf(ch.form, g.node(ch.plug.node)->as<glm::dvec3>(ch.plug.attr)[ch.plug.component]); }

/** Fit rows for the correctives: per sample its features (channel forms) and the rest-space residual per point. */
struct CorrectiveRows
{
    Eigen::MatrixXd x, r;
};


std::string channelText(Graph& g, const Channel& ch)
{
    const std::string p = g.plugName(ch.plug);
    switch (ch.form)
    {
    case Channel::Sine: return "sin(radians(" + p + "))";
    case Channel::Versine: return "(1 - cos(radians(" + p + ")))";
    default: return p;
    }
}

/**
 * Pose-driven morph correctives for what linear skinning leaves. The residual per frame, taken back through the skin into
 * rest space, is regressed on the pose channels (ridge, its strength picked by generalized cross-validation); the
 * principal components of the regressed residual become morph targets, each weighted by an expression linear in the
 * channels, so a pose outside the clip gets its correctives too. Up to cap, until tolerance.
 */
json bakeCorrectives(Graph& g, NodeId mesh, NodeId skin, const std::vector<Channel>& channels, const std::vector<double>& times,
                     const std::vector<std::vector<glm::dvec3>>& targets, const CorrectiveRows& extra, int cap, double tolerance)
{
    LILA_ZONE();
    if (cap <= 0 || times.size() < 2)
        return json::array();
    using Eigen::Index;
    const Index clip = Index(times.size());
    const Index n = clip + extra.x.rows();
    const size_t points = targets.front().size();
    Eigen::MatrixXd r(n, Index(points * 3));
    Eigen::MatrixXd x(n, Index(channels.size()));
    if (extra.x.rows() > 0)
    {
        r.bottomRows(extra.r.rows()) = extra.r;
        x.bottomRows(extra.x.rows()) = extra.x;
    }
    const double saved = g.time();
    for (Index f = 0; f < clip; ++f)
    {
        g.setTime(times[size_t(f)]);
        g.evaluate();
        const std::vector<glm::dvec3> pts = outPoints(g, mesh);
        const std::vector<glm::dmat3> b = blendedLinear(g, skin, points);
        for (size_t p = 0; p < points; ++p)
        {
            const glm::dvec3 miss = p < pts.size() ? targets[size_t(f)][p] - pts[p] : glm::dvec3(0.0);
            const double det = glm::determinant(b[p]);
            const glm::dvec3 rest = std::abs(det) > 1e-12 ? glm::inverse(b[p]) * miss : miss;
            for (int c = 0; c < 3; ++c)
                r(f, Index(p * 3 + size_t(c))) = rest[c];
        }
        for (size_t c = 0; c < channels.size(); ++c)
            x(f, Index(c)) = channelValue(g, channels[c]);
    }
    g.setTime(saved);
    g.evaluate();

    // Standardized channels that move in the clip; one that doesn't can't be told apart from the intercept.
    std::vector<size_t> used;
    std::vector<double> mean, scale;
    for (size_t c = 0; c < channels.size(); ++c)
    {
        const double m = x.col(Index(c)).mean();
        const double sd = std::sqrt((x.col(Index(c)).array() - m).square().sum() / double(n));
        if (sd > 1e-9)
        {
            used.push_back(c);
            mean.push_back(m);
            scale.push_back(sd);
        }
    }
    Eigen::MatrixXd z(n, Index(used.size()));
    for (size_t k = 0; k < used.size(); ++k)
        z.col(Index(k)) = (x.col(Index(used[k])).array() - mean[k]) / scale[k];

    const Eigen::MatrixXd gram = r * r.transpose();
    const Eigen::MatrixXd centre = Eigen::MatrixXd::Constant(n, n, 1.0 / double(n));
    Eigen::MatrixXd u = Eigen::MatrixXd::Zero(n, 0), v = Eigen::MatrixXd::Zero(Index(used.size()), 0);
    Eigen::VectorXd sv = Eigen::VectorXd::Zero(0);
    if (!used.empty())
    {
        Eigen::BDCSVD<Eigen::MatrixXd, Eigen::ComputeThinU | Eigen::ComputeThinV> svd(z);
        u = svd.matrixU();
        v = svd.matrixV();
        sv = svd.singularValues();
    }
    auto hat = [&](double lambda) {
        const Eigen::VectorXd shrink = sv.array().square() / (sv.array().square() + lambda);
        return Eigen::MatrixXd(centre + u * shrink.asDiagonal() * u.transpose());
    };
    // Generalized cross-validation over a log grid of ridge strengths.
    double lambda = double(n), best = std::numeric_limits<double>::infinity();
    const Eigen::MatrixXd identity = Eigen::MatrixXd::Identity(n, n);
    for (int e = -6; e <= 6; ++e)
    {
        const double l = std::pow(10.0, double(e)) * double(n);
        const Eigen::MatrixXd h = hat(l);
        const double dof = double(n) - h.trace();
        if (dof < 0.5)
            continue;
        const Eigen::MatrixXd m = identity - h;
        const double score = double(n) * (m * gram * m.transpose()).trace() / (dof * dof);
        if (score < best)
        {
            best = score;
            lambda = l;
        }
    }
    const Eigen::MatrixXd h = hat(lambda);
    const Eigen::VectorXd shrinkFit = sv.array() / (sv.array().square() + lambda);
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(Eigen::MatrixXd(h * gram * h.transpose()));
    const Eigen::VectorXd values = eig.eigenvalues();
    const Eigen::MatrixXd vectors = eig.eigenvectors();
    const double total = double(size_t(n) * points);
    double remaining = gram.trace();

    NodeId morph = morphs::find(g, mesh);
    if (morph == kNoNode)
        morph = morphs::create(g, mesh, "SoftNetCorrectives");
    json made = json::array();
    for (Index k = values.size() - 1; k >= 0 && int(made.size()) < cap; --k)
    {
        if (values[k] <= 1e-12 * std::max(1.0, values.maxCoeff()) || std::sqrt(std::max(remaining, 0.0) / total) <= tolerance)
            break;
        const double sigma = std::sqrt(values[k]);
        const Eigen::VectorXd w = vectors.col(k); // the corrective's weight per frame
        const Eigen::VectorXd hw = h.transpose() * w;
        Eigen::VectorXd shape = r.transpose() * hw;   // sigma times the unit shape
        const Eigen::VectorXd rv = gram * hw / sigma; // the residual along the unit shape, per frame
        // weight(x) = a0 + sum a_k z_k(x), the ridge regression of rv / sigma on the standardized channels.
        double a0 = rv.mean() / sigma;
        Eigen::VectorXd ak = used.empty() ? Eigen::VectorXd() : Eigen::VectorXd(v * shrinkFit.asDiagonal() * (u.transpose() * rv) / sigma);
        const double peak = w.cwiseAbs().maxCoeff();
        if (!(peak > 0.0))
            break;
        shape *= peak;
        a0 /= peak;
        ak /= peak;

        // The strongest terms, until they carry all but a millionth of the weight's variance.
        std::vector<size_t> order(used.size());
        std::iota(order.begin(), order.end(), size_t(0));
        std::sort(order.begin(), order.end(), [&](size_t i, size_t j) { return std::abs(ak[Index(i)]) > std::abs(ak[Index(j)]); });
        const double energy = ak.squaredNorm();
        double kept = 0.0, c0 = a0;
        std::string terms;
        for (size_t i : order)
        {
            if (kept >= energy * (1.0 - 1e-6) || ak[Index(i)] == 0.0)
                break;
            kept += ak[Index(i)] * ak[Index(i)];
            const double c = ak[Index(i)] / scale[i];
            c0 -= c * mean[i];
            terms += std::format(" + {:.9g} * {}", c, channelText(g, channels[used[i]]));
        }

        auto deltas = std::make_shared<MorphDeltas>();
        for (size_t p = 0; p < points; ++p)
        {
            const glm::dvec3 d(shape[Index(p * 3)], shape[Index(p * 3 + 1)], shape[Index(p * 3 + 2)]);
            if (glm::length(d) > 1e-7)
            {
                deltas->indices.push_back(uint32_t(p));
                deltas->deltas.push_back(d);
            }
        }
        const std::string name = "softNetFix" + std::to_string(made.size() + 1);
        const NodeId target = morphs::addTarget(g, morph, name, deltas, 0.0);
        ops::execute("expression", {{"args", {g.plugName({target, morphs::kWeight, -1})}}, {"string", std::format("{:.9g}", c0) + terms}});
        made.push_back(g.node(target)->path());
        remaining -= values[k];
    }
    log::write(LogLevel::Info, std::format("bakeSoftNet: correctives read {} of {} pose channels (ridge {:.3g})", used.size(), channels.size(), lambda));
    return made;
}

NodeId exportMeshArg(Graph& g, const Args& a, const char* cmd)
{
    if (a.objects().empty())
        throw CommandError(std::string(cmd) + ": give a mesh with a Soft Net");
    Node& n = nodeNamed(a.objects().front(), cmd);
    NodeId mesh = n.id();
    if (!n.type().isA("mesh"))
    {
        const NodeId hub = netOf(g, n.id());
        const std::vector<NodeId> ms = hub != kNoNode ? netMeshes(g, hub) : std::vector<NodeId>{};
        if (ms.empty())
            throw CommandError(std::string(cmd) + ": " + n.path() + " is not a mesh with a Soft Net");
        mesh = ms.front();
    }
    if (netOf(g, mesh) == kNoNode)
        throw CommandError(std::string(cmd) + ": " + g.node(mesh)->path() + " has no Soft Net");
    return mesh;
}

NodeId deformerOn(Graph& g, NodeId mesh)
{
    for (NodeId d : modifiers::stack(g, mesh))
        if (g.node(d)->type().name == kSoftNetType)
            return d;
    return kNoNode;
}

InfluencePtr sharedTable(Graph& g, NodeId d, const MeshPtr& in)
{
    const Node& dn = *g.node(d);
    return influenceTable(*in, dn.as<std::string>(dn.type().attrIndex("binding")), dn.as<double>(dn.type().attrIndex("reach")),
                          dn.as<double>(dn.type().attrIndex("stiffness")))
        .get();
}

/** The joint each of the net's degrees of freedom moves with: a knot's control joint, a tangent handle's joint. */
std::vector<NodeId> dofJoints(Graph& g, NodeId d, const NetJoints& joints, const MeshBind& b)
{
    std::vector<NodeId> out(deform::softnet::dofCount(b.knots, b.tangentRest.size()), kNoNode);
    for (size_t k = 0; k < b.knots && k < joints.knots.size(); ++k)
        out[k] = joints.knots[k].second;
    const int attr = g.node(d)->type().attrIndex("tangentMatrix");
    for (size_t t = 0; t < b.tangentRest.size(); ++t)
        if (const auto src = g.source({d, attr, -1, int(t)}))
            if (const auto it = joints.handles.find(src->node); it != joints.handles.end())
                out[b.knots + t] = it->second.first;
    return out;
}

/**
 * Starts each point on the knot and handle joints the net moves it with, by its influence weights, so the weight fit
 * considers them: Dem Bones only picks among the bones a point already leans on.
 */
WeightTable seedNetWeights(Graph& g, NodeId mesh, NodeId skin, const NetJoints& joints)
{
    LILA_ZONE();
    WeightTable w = skin::table(g, skin);
    const NodeId d = deformerOn(g, mesh);
    if (d == kNoNode)
        return w;
    g.evaluate();
    const Node& dn = *g.node(d);
    const MeshPtr in = dn.as<MeshPtr>(DeformerNode::kInMesh);
    const MeshBind b = MeshBind::fromJson(dn.as<std::string>(dn.type().attrIndex("binding")));
    if (!in || b.points != in->pointCapacity())
        return w;
    const InfluencePtr table = sharedTable(g, d, in);
    if (!table || table->points() == 0)
        return w;
    std::map<NodeId, uint32_t> idOf;
    for (const skin::Influence& i : skin::influences(g, skin))
        idOf[i.joint] = i.index;
    std::vector<int64_t> dofId;
    for (NodeId j : dofJoints(g, d, joints, b))
        dofId.push_back(idOf.contains(j) ? int64_t(idOf[j]) : -1);
    for (size_t p = 0; p < w.points.size() && p < table->points(); ++p)
    {
        std::map<uint32_t, float> net;
        double sum = 0.0;
        for (uint32_t k = table->offsets[p]; k < table->offsets[p + 1]; ++k)
            if (const auto& e = table->entries[k]; e.weight > 0.0 && e.dof < dofId.size() && dofId[e.dof] >= 0)
            {
                net[uint32_t(dofId[e.dof])] += float(e.weight);
                sum += e.weight;
            }
        if (net.empty())
            continue;
        const float keep = float(1.0 - std::min(sum, 0.9));
        const float scale = float(std::min(sum, 0.9) / sum);
        for (auto& [i, wt] : w.points[p])
            wt *= keep;
        for (const auto& [i, wt] : net)
            w.points[p].push_back({i, wt * scale});
    }
    return w;
}

/**
 * Range-of-motion rows at bind: the net's control and handle joints posed at random within (a quarter past) the span
 * each channel covers over times, the skin at its bind pose. The net's result and the fitted skin's are both worked
 * out directly (rest points, the influence table, the joints' bind palettes), so no keys are touched. Body mesh only.
 */
CorrectiveRows romRows(Graph& g, NodeId mesh, NodeId skin, NodeId hub, const NetJoints& joints, const std::vector<Channel>& channels,
                       const std::vector<double>& times, int count)
{
    LILA_ZONE();
    CorrectiveRows out;
    const NodeId d = deformerOn(g, mesh);
    if (d == kNoNode || count <= 0)
        return out;
    g.evaluate();
    const Node& dn = *g.node(d);
    if (dn.as<int64_t>(dn.type().attrIndex("meshIndex")) != 0)
        return out;
    const MeshBind b = MeshBind::fromJson(dn.as<std::string>(dn.type().attrIndex("binding")));
    const MeshPtr in = dn.as<MeshPtr>(DeformerNode::kInMesh);
    const MeshPtr restMesh = skin::inputMesh(g, skin);
    if (!in || !restMesh || b.points != in->pointCapacity() || restMesh->pointCapacity() != b.points)
        return out;
    const InfluencePtr table = sharedTable(g, d, in);
    if (!table || table->points() == 0)
        return out;
    const Node& h = *g.node(hub);
    const NetBind net = NetBind::fromJson(h.as<std::string>(h.type().attrIndex("binding")));
    std::vector<glm::dmat4> frames;
    for (const auto& k : net.knots)
    {
        glm::dmat4 f(k.rest);
        f[3] = glm::dvec4(k.origin, 1.0);
        frames.push_back(f);
    }
    std::map<NodeId, glm::dmat4> restWorld;
    for (size_t i = 0; i < joints.joints.size() && i < joints.restWorld.size(); ++i)
        restWorld[joints.joints[i]] = joints.restWorld[i];

    // Per tangent index: its handle joint and the handle's rest offset.
    const int tangentAttr = dn.type().attrIndex("tangentMatrix");
    std::vector<std::pair<NodeId, glm::dmat4>> handleOf(b.tangentRest.size(), {kNoNode, glm::dmat4(1.0)});
    for (size_t t = 0; t < b.tangentRest.size(); ++t)
        if (const auto src = g.source({d, tangentAttr, -1, int(t)}))
            if (const auto it = joints.handles.find(src->node); it != joints.handles.end())
                handleOf[t] = {it->second.first, g.node(src->node)->as<glm::dmat4>(Transform::kOffsetParentMatrix)};

    // The net joints' channels: their span over the clip, sampled; anything else reads its bind value.
    std::set<NodeId> netJoints;
    for (const auto& [frame, control] : joints.knots)
        netJoints.insert(control);
    for (const auto& [jt, offset] : handleOf)
        netJoints.insert(jt);
    std::map<NodeId, std::array<glm::dvec2, 6>> span; // translate xyz, rotate xyz: min, max
    const double saved = g.time();
    for (size_t f = 0; f < times.size(); ++f)
    {
        g.setTime(times[f]);
        g.evaluate();
        for (NodeId j : netJoints)
        {
            if (j == kNoNode)
                continue;
            const glm::dvec3 tr = g.node(j)->as<glm::dvec3>(Transform::kTranslate), ro = g.node(j)->as<glm::dvec3>(Transform::kRotate);
            auto [it, fresh] = span.try_emplace(j);
            for (int c = 0; c < 6; ++c)
            {
                const double v = c < 3 ? tr[c] : ro[c - 3];
                it->second[size_t(c)] = fresh ? glm::dvec2(v) : glm::dvec2(std::min(it->second[size_t(c)].x, v), std::max(it->second[size_t(c)].y, v));
            }
        }
    }
    g.setTime(saved);
    g.evaluate();

    const glm::dmat4 geom = g.node(skin)->as<glm::dmat4>(SkinClusterNode::kGeomMatrix), geomInv = glm::inverse(geom);
    std::vector<std::pair<NodeId, glm::dmat4>> bindPre; // by influence index
    for (const skin::Influence& i : skin::influences(g, skin))
    {
        if (bindPre.size() <= i.index)
            bindPre.resize(i.index + 1, {kNoNode, glm::dmat4(1.0)});
        bindPre[i.index] = {i.joint, g.node(i.link)->as<glm::dmat4>(SkinInfluenceNode::kBindPreMatrix)};
    }
    std::map<NodeId, glm::dvec3> bindRotate;
    for (const auto& [j, pre] : bindPre)
        if (j != kNoNode && !restWorld.contains(j))
        {
            const NodeId p = g.parent(j);
            glm::dmat4 parentWorld = p != kNoNode ? worldOf(*g.node(p)) : glm::dmat4(1.0);
            for (const auto& [pj, ppre] : bindPre)
                if (pj == p)
                    parentWorld = glm::inverse(ppre);
            const glm::dmat4 local = glm::inverse(parentWorld) * glm::inverse(pre);
            bindRotate[j] = eulerNear(orthonormal(glm::dmat3(local)), RotateOrder(g.node(j)->as<int64_t>(Transform::kRotateOrder)),
                                      g.node(j)->as<glm::dvec3>(Transform::kRotate));
        }

    const WeightTable weights = skin::table(g, skin);
    const std::span<const glm::dvec3> rest = restMesh->positions();
    const size_t points = rest.size();
    out.x.resize(count, Eigen::Index(channels.size()));
    out.r.resize(count, Eigen::Index(points * 3));
    std::mt19937_64 rng(1);
    for (int s = 0; s < count; ++s)
    {
        std::map<NodeId, std::pair<glm::dvec3, glm::dvec3>> pose; // translate, rotate
        std::map<NodeId, glm::dmat4> local;
        for (const auto& [j, sp] : span)
        {
            glm::dvec3 tr, ro;
            for (int c = 0; c < 6; ++c)
            {
                const double lo = sp[size_t(c)].x, hi = sp[size_t(c)].y, pad = 0.25 * (hi - lo);
                const double v = std::uniform_real_distribution<double>(lo - pad, hi + pad + 1e-12)(rng);
                (c < 3 ? tr[c] : ro[c - 3]) = v;
            }
            pose[j] = {tr, ro};
            glm::dmat4 m(glm::mat3_cast(anim::eulerToQuat(ro, RotateOrder(g.node(j)->as<int64_t>(Transform::kRotateOrder)))));
            m[3] = glm::dvec4(tr, 1.0);
            local[j] = m;
        }
        std::map<NodeId, glm::dmat4> world;
        std::map<int, glm::dmat4> knotMats, tangentMats;
        for (size_t k = 0; k < joints.knots.size(); ++k)
        {
            const auto [frame, control] = joints.knots[k];
            if (frame == kNoNode || !local.contains(control))
                continue;
            knotMats[int(k)] = local[control];
            world[frame] = restWorld[frame];
            world[control] = restWorld[frame] * local[control];
        }
        for (size_t t = 0; t < handleOf.size(); ++t)
        {
            const auto& [jt, offset] = handleOf[t];
            const NodeId control = jt != kNoNode ? g.parent(jt) : kNoNode;
            if (jt == kNoNode || !local.contains(jt) || !world.contains(control))
                continue;
            tangentMats[int(t)] = glm::inverse(offset) * local[jt];
            world[jt] = world[control] * local[jt];
        }
        const std::vector<glm::dvec3> moves = dofMoves(b, frames, knotMats, tangentMats, glm::dmat4(1.0));
        std::vector<glm::dvec3> offsets(points, glm::dvec3(0.0));
        table->apply(moves, offsets);
        std::vector<glm::dmat4> palette(bindPre.size(), glm::dmat4(1.0));
        for (size_t i = 0; i < bindPre.size(); ++i)
            if (const auto it = world.find(bindPre[i].first); it != world.end())
                palette[i] = geomInv * it->second * bindPre[i].second * geom;
        for (size_t p = 0; p < points; ++p)
        {
            glm::dmat4 m(0.0);
            double total = 0.0;
            if (p < weights.points.size())
                for (const auto& [i, w] : weights.points[p])
                    if (i < palette.size())
                    {
                        m += double(w) * palette[i];
                        total += w;
                    }
            m = total > 0.0 ? m * (1.0 / total) : glm::dmat4(1.0);
            const glm::dvec3 miss = rest[p] + offsets[p] - glm::dvec3(m * glm::dvec4(rest[p], 1.0));
            const glm::dmat3 m3(m);
            const glm::dvec3 back = std::abs(glm::determinant(m3)) > 1e-12 ? glm::inverse(m3) * miss : miss;
            for (int c = 0; c < 3; ++c)
                out.r(s, Eigen::Index(p * 3 + size_t(c))) = back[c];
        }
        for (size_t c = 0; c < channels.size(); ++c)
        {
            const Channel& ch = channels[c];
            double raw = 0.0;
            if (const auto it = pose.find(ch.plug.node); it != pose.end())
                raw = (ch.plug.attr == Transform::kTranslate ? it->second.first : it->second.second)[ch.plug.component];
            else if (const auto bt = bindRotate.find(ch.plug.node); bt != bindRotate.end() && ch.plug.attr == Transform::kRotate)
                raw = bt->second[ch.plug.component];
            else
                raw = g.node(ch.plug.node)->as<glm::dvec3>(ch.plug.attr)[ch.plug.component];
            out.x(s, Eigen::Index(c)) = formOf(ch.form, raw);
        }
    }
    return out;
}

json bake(Graph& g, NodeId mesh, const Args& a)
{
    LILA_ZONE();
    const NodeId skin = skin::find(g, mesh);
    if (skin == kNoNode)
        throw CommandError("bakeSoftNet: " + g.node(mesh)->path() + " has no skin to bake into; skin it first, or send it to Unreal as a geometry cache");
    const NodeId hub = netOf(g, mesh);
    const Range range = rangeOf(a);
    const int step = std::max(1, a.get<int>("step", 1));
    std::vector<double> checkTimes;
    if (a.has("checkTo"))
        for (int64_t f = std::llround(range.times.back() * range.fps) + step; f <= a.get<int>("checkTo"); f += step)
            checkTimes.push_back(double(f) / range.fps);
    std::vector<double> allTimes = range.times;
    allTimes.insert(allTimes.end(), checkTimes.begin(), checkTimes.end());

    // What the stack makes now, the net included, is what the bake must reach.
    std::vector<std::vector<glm::dvec3>> targets;
    const double saved = g.time();
    for (double t : allTimes)
    {
        g.setTime(t);
        g.evaluate();
        targets.push_back(outPoints(g, mesh));
    }
    g.setTime(saved);
    g.evaluate();

    // Knot joints as fixed bones: added to the skin with their rest as bind.
    std::vector<NodeId> skinJoints;
    for (const skin::Influence& i : skin::influences(g, skin))
        if (i.joint != kNoNode)
            skinJoints.push_back(i.joint);
    const NetJoints joints = makeJoints(g, hub, skin, allTimes);
    const std::vector<NodeId>& made = joints.joints;
    for (size_t i = 0; i < made.size(); ++i)
    {
        skin::addInfluence(g, skin, made[i]);
        for (const skin::Influence& inf : skin::influences(g, skin))
            if (inf.joint == made[i])
                g.setValue({inf.link, SkinInfluenceNode::kBindPreMatrix, -1}, glm::inverse(joints.restWorld[i]));
    }
    skinbake::FitOptions fo;
    fo.animation = true;
    fo.start = std::llround(range.times.front() * range.fps);
    fo.end = std::llround(range.times.back() * range.fps);
    fo.step = std::max(1, a.get<int>("step", 1));
    fo.helpers = std::max(0, a.get<int>("helpers"));
    fo.maxInfluences = a.get<int>("maxInfluences");
    fo.iterations = a.get<int>("iterations");
    fo.seed = seedNetWeights(g, mesh, skin, joints);
    const skinbake::FitResult fitted = skinbake::fitSkinWeights(g, skin, fo);
    json fit = skinbake::applyFit(g, skin, fitted, true);
    const std::vector<std::vector<glm::dvec3>> clipTargets(targets.begin(), targets.begin() + std::ptrdiff_t(range.times.size()));
    const std::vector<std::vector<glm::dvec3>> checkTargets(targets.begin() + std::ptrdiff_t(range.times.size()), targets.end());
    const Errors skinned = errorsAgainst(g, mesh, range.times, clipTargets);
    const Errors skinnedCheck = errorsAgainst(g, mesh, checkTimes, checkTargets);
    std::vector<NodeId> drivers;
    for (NodeId j : made)
        if (g.node(j)->name().ends_with("_jnt"))
            drivers.push_back(j);
    const std::vector<Channel> channels = poseChannels(g, drivers, skinJoints);
    // Helpers are keyed to the range, so poses outside it can't be sampled with them.
    const int romCount = fo.helpers > 0 ? 0 : std::clamp(int(channels.size()) * 2, 64, 256);
    if (fo.helpers > 0 && a.get<int>("correctives") > 0)
        log::write(LogLevel::Info, "bakeSoftNet: helper joints are keyed to the range, so correctives are fitted to the range only; poses outside it may miss them");
    const CorrectiveRows rom = romRows(g, mesh, skin, hub, joints, channels, range.times, romCount);
    const json correctives =
        bakeCorrectives(g, mesh, skin, channels, range.times, clipTargets, rom, a.get<int>("correctives"), a.get<double>("tolerance"));
    std::vector<double> perFrame;
    const Errors final = errorsAgainst(g, mesh, range.times, clipTargets, &perFrame);
    const Errors check = errorsAgainst(g, mesh, checkTimes, checkTargets);

    // U1: the skeleton under Unreal's transform rules over the baked frames.
    const uecheck::ExportCheck c = uecheck::checkExport(g, {mesh}, false, range.times.front(), range.times.back(), range.fps, "bakeSoftNet");
    json mismatches = json::array();
    for (const uecheck::Mismatch& m : c.report.mismatches)
        mismatches.push_back({{"joint", g.node(m.joint)->path()}, {"error", m.error}, {"time", m.time}});
    json names = json::array();
    for (NodeId j : made)
        names.push_back(g.node(j)->path());
    log::write(LogLevel::Info, std::format("bakeSoftNet: {} joints, {} correctives, {:.4f} cm RMS ({:.4f} cm skinned only), worst {:.4f} cm", made.size(),
                                           correctives.size(), final.rms, skinned.rms, final.max));
    return {{"joints", names},
            {"helpers", fit.value("helpers", json::array())},
            {"correctives", correctives},
            {"skinnedRmse", skinned.rms},
            {"rmse", final.rms},
            {"maxError", final.max},
            {"frameError", perFrame},
            {"check", {{"frames", checkTimes.size()}, {"skinnedRmse", skinnedCheck.rms}, {"rmse", check.rms}, {"maxError", check.max}}},
            {"frames", range.times.size()},
            {"disabled", fit.value("disabled", json::array())},
            {"unrealCheck", {{"worst", c.report.worst}, {"frames", c.report.frames}, {"mismatches", mismatches}, {"error", c.error}}}};
}

std::string bareName(const std::string& path) { return path.substr(path.find_last_of("|:") + 1); }

/**
 * The net live in Unreal: per knot its frame and control bones, per tangent handle its bone and rest bone (moves are
 * bone minus rest in component space), and per point its place at bind (Unreal space) and its influence entries
 * [dof, weight]. LilaLink binds every LOD of the skeletal mesh to them under the Lila Soft Net Deformer Graph.
 */
json unrealPayload(Graph& g, NodeId mesh, const NetJoints& joints, const std::string& skeletalMesh, const std::string& deformerPath)
{
    LILA_ZONE();
    const NodeId d = deformerOn(g, mesh);
    const NodeId skin = skin::find(g, mesh);
    g.evaluate();
    const Node& dn = *g.node(d);
    const MeshBind b = MeshBind::fromJson(dn.as<std::string>(dn.type().attrIndex("binding")));
    const MeshPtr in = dn.as<MeshPtr>(DeformerNode::kInMesh);
    const MeshPtr rest = skin != kNoNode ? skin::inputMesh(g, skin) : in;
    if (!in || !rest || b.points != in->pointCapacity())
        throw CommandError("unrealSoftNet: the mesh changed since the net was bound; run softNetRebind");
    const double reach = dn.as<double>(dn.type().attrIndex("reach"));
    const InfluencePtr shared =
        influenceTable(*in, dn.as<std::string>(dn.type().attrIndex("binding")), reach, dn.as<double>(dn.type().attrIndex("stiffness"))).get();
    if (!shared || shared->points() == 0)
        throw CommandError("unrealSoftNet: the net's influence couldn't be solved on this mesh");
    const deform::softnet::InfluenceTable& table = *shared;
    json knots = json::array(), tangents = json::array();
    for (const auto& [frame, control] : joints.knots)
        knots.push_back({{"frame", frame != kNoNode ? bareName(g.node(frame)->name()) : ""}, {"control", control != kNoNode ? bareName(g.node(control)->name()) : ""}});
    for (size_t t = 0; t < b.tangentRest.size(); ++t)
    {
        const auto src = g.source({d, dn.type().attrIndex("tangentMatrix"), -1, int(t)});
        const auto it = src ? joints.handles.find(src->node) : joints.handles.end();
        if (it == joints.handles.end())
            throw CommandError("unrealSoftNet: tangent handle " + std::to_string(t) + " has no control; run softNetRebind");
        tangents.push_back({{"bone", bareName(g.node(it->second.first)->name())}, {"rest", bareName(g.node(it->second.second)->name())}});
    }
    const glm::dmat4 toWorld = skin != kNoNode ? g.node(skin)->as<glm::dmat4>(SkinClusterNode::kGeomMatrix) : worldOf(*g.node(mesh));
    const std::span<const glm::dvec3> pos = rest->positions();
    std::vector<glm::dvec3> ue(pos.size());
    for (size_t i = 0; i < pos.size(); ++i)
    {
        const glm::dvec3 w(toWorld * glm::dvec4(pos[i], 1.0));
        const math::Vector u = math::characterToUnreal(math::Vector(w.x, w.y, w.z));
        ue[i] = {u.x, u.y, u.z};
    }
    // In Unreal's space, so the kernel turns its rest normals and tangents by them directly.
    const std::vector<glm::dvec3> grads = deform::softnet::entryGradients(table, *rest, ue);
    json points = json::array();
    for (size_t i = 0; i < table.points() && i < pos.size(); ++i)
    {
        json e = json::array();
        for (uint32_t k = table.offsets[i]; k < table.offsets[i + 1]; ++k)
            e.push_back({table.entries[k].dof, table.entries[k].weight, grads[k].x, grads[k].y, grads[k].z});
        points.push_back({{"p", {ue[i].x, ue[i].y, ue[i].z}}, {"i", e}});
    }
    json tris = json::array();
    for (geo::Index f = 0; f < rest->faceCapacity(); ++f)
    {
        if (!rest->faceValid(f))
            continue;
        std::vector<uint32_t> c;
        rest->forEachFaceCorner(f, [&](geo::Index k) { c.push_back(uint32_t(rest->cornerPoint(k))); });
        for (size_t i = 1; i + 1 < c.size(); ++i)
            tris.insert(tris.end(), {c[0], c[i], c[i + 1]});
    }
    return {{"skeletalMesh", skeletalMesh}, {"deformer", deformerPath}, {"knots", knots}, {"tangents", tangents}, {"dofs", table.dofs},
            {"points", points},         {"tris", tris}};
}

std::string mappedSkeletalMesh(Graph& g, NodeId mesh)
{
    if (!ops::find("unrealMappings"))
        return {};
    for (const json& m : ops::execute("unrealMappings", json::object()))
        if (Node* mapped = g.find(m.value("node", "")); mapped && m.value("kind", "") == "skeletal")
            for (NodeId n = mesh; n != kNoNode; n = g.parent(n))
                if (n == mapped->id())
                    return m.value("contentPath", "");
    return {};
}
} // namespace

void registerSoftNetExportCommands()
{
    using T = ArgType;
    const std::vector<FlagDef> rangeFlags = {
        {"start", "", T::Int, nullptr, "First frame (the playback start by default)."},
        {"end", "", T::Int, nullptr, "Last frame (the playback end by default)."},
        {"step", "", T::Int, 1, "Frames between samples.", {}, {}, 1.0},
    };
    std::vector<FlagDef> bakeFlags = rangeFlags;
    bakeFlags.insert(bakeFlags.end(), {
        {"helpers", "", T::Int, 0, "Helper joints solved with the weights for what knots and skin can't follow. They are keyed to the range, so poses outside it don't get them.", {}, {}, 0.0, 16.0},
        {"correctives", "", T::Int, 32, "Most morph correctives for the residual.", {}, {}, 0.0, 64.0},
        {"tolerance", "", T::Float, 0.001, "cm of RMS residual at which correctives stop.", {}, {}, 0.0},
        {"checkTo", "", T::Int, nullptr, "Frames after end up to this one are baked and checked against the net but not fitted: how the bake holds up on poses it wasn't fitted to."},
        {"maxInfluences", "mi", T::Int, 8, "Influences per point.", {}, {}, 1.0, 12.0},
        {"iterations", "", T::Int, 3, "Fit passes.", {}, {}, 1.0},
    });
    ops::registerCommand({
        .name = "bakeSoftNet",
        .doc = "Bake a skinned mesh's Soft Net for Unreal: each knot becomes a frame joint riding the surface with a control joint "
               "under it, each tangent handle a joint under its knot, keyed over the range and added to the skin as fixed bones; "
               "weights are fitted to the net's result (with up to helpers helper joints), and morph correctives take the residual, "
               "each weighted by an expression on the knot, handle and skin joints' channels, so poses outside the range get them "
               "too (unrealExpressionRig sends them to Unreal). The net is turned off so the viewport shows what Unreal gets. "
               "Returns {joints, helpers, correctives, skinnedRmse, rmse, maxError, frameError, frames, check, unrealCheck}: "
               "errors in cm against the net, per frame worst, the checkTo frames' errors, and the Unreal round-trip check.",
        .positionalDoc = "mesh (or its net)",
        .flags = bakeFlags,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            return bake(g, exportMeshArg(g, a, "bakeSoftNet"), a);
        },
        .examples = {"bakeSoftNet(\"Mochi\", correctives=32)"},
    });

    std::vector<FlagDef> routeFlags = bakeFlags;
    routeFlags.insert(routeFlags.begin(), {"route", "", T::String, "bake", "How the net reaches Unreal.", {"bake", "live", "cache"}});
    routeFlags.push_back({"cacheFormat", "", T::String, "alembic", "cache: Alembic or vertex animation (VAT).", {"alembic", "vat"}});
    routeFlags.push_back({"path", "", T::String, nullptr, "cache: the file to write; default beside the scene."});
    ops::registerCommand({
        .name = "softNetUnreal",
        .doc = "Send a Soft Net to Unreal one of three ways. bake (default): knot joints, fitted weights and morph correctives on "
               "the skin (bakeSoftNet), so any Unreal export carries it. live: the net runs in Unreal as a Deformer Graph over "
               "the skin, matching Lila (unrealSoftNet; knot joints are baked to drive it). cache: the deformed mesh as an Alembic "
               "or vertex animation cache, exact but fixed, for cinematics. Returns that route's result.",
        .positionalDoc = "mesh (or its net)",
        .flags = routeFlags,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const NodeId mesh = exportMeshArg(g, a, "softNetUnreal");
            const std::string route = a.get<std::string>("route", "bake");
            if (route == "bake")
                return bake(g, mesh, a);
            if (route == "live")
            {
                json call = a.flags;
                call["args"] = {g.node(mesh)->path()};
                for (const char* drop : {"route", "cacheFormat", "path", "helpers", "correctives", "tolerance", "maxInfluences", "iterations"})
                    call.erase(drop);
                return ops::execute("unrealSoftNet", call);
            }
            const Range range = rangeOf(a);
            json call = {{"args", {g.node(mesh)->path()}}, {"start", range.times.front() * range.fps}, {"end", range.times.back() * range.fps}};
            if (a.has("path"))
                call["path"] = a.get<std::string>("path");
            return ops::execute(a.get<std::string>("cacheFormat", "alembic") == "vat" ? "exportVertexAnimation" : "exportAlembic", call);
        },
        .examples = {"softNetUnreal(\"Mochi\")", "softNetUnreal(\"Mochi\", route=\"cache\", cacheFormat=\"alembic\")"},
    });
    std::vector<FlagDef> liveFlags = rangeFlags;
    liveFlags.insert(liveFlags.end(), {
        {"skeletalMesh", "sm", T::String, nullptr, "The skeletal mesh asset. Default: the mesh's mapping."},
        {"deformer", "df", T::String, "/Game/Lila/DG_LilaSoftNet", "The Deformer Graph asset to use, made when missing."},
        {"dryRun", "dry", T::Bool, false, "Make the joints and return what would be sent, without connecting to Unreal."},
    });
    ops::registerCommand({
        .name = "unrealSoftNet",
        .doc = "Run a skinned mesh's Soft Net live in Unreal: its knots and tangent handles become joints keyed over the range "
               "(export the skeleton with them, deformOnly off), and LilaLink binds every LOD of the skeletal mesh to the net's "
               "precomputed influence (each vertex takes the entries of the point at its place) under the Lila Soft Net "
               "Deformer Graph: linear skinning, then the net's offset from the knot bones. Returns {skeletalMesh, deformer, "
               "joints, unreal}.",
        .positionalDoc = "mesh (or its net)",
        .flags = liveFlags,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const NodeId mesh = exportMeshArg(g, a, "unrealSoftNet");
            const NodeId skin = skin::find(g, mesh);
            if (skin == kNoNode)
                throw CommandError("unrealSoftNet: " + g.node(mesh)->path() + " has no skin; the live net runs over a skinned mesh");
            std::string sm = a.get<std::string>("skeletalMesh", "");
            if (sm.empty())
                sm = mappedSkeletalMesh(g, mesh);
            if (sm.empty() && !a.get<bool>("dryRun"))
                throw CommandError("unrealSoftNet: give skeletalMesh, or map the mesh's character with unrealMap kind=skeletal");
            const NetJoints joints = makeJoints(g, netOf(g, mesh), skin, rangeOf(a).times);
            const json payload = unrealPayload(g, mesh, joints, sm, a.get<std::string>("deformer", "/Game/Lila/DG_LilaSoftNet"));
            json names = json::array();
            for (NodeId j : joints.joints)
                names.push_back(g.node(j)->path());
            if (a.get<bool>("dryRun"))
                return {{"skeletalMesh", sm}, {"deformer", payload["deformer"]}, {"joints", names}, {"payload", payload}};
            const std::string code = "import importlib, lila_link_soft_net\nimportlib.reload(lila_link_soft_net)\nlila_link_soft_net.setup_and_print(" +
                                     json(payload.dump()).dump() + ")\n";
            const json r = ops::execute("unrealCall", {{"args", {"runPython"}}, {"params", {{"code", code}}}, {"timeoutMs", 300000}});
            if (!r.value("ok", false))
                throw CommandError("unrealSoftNet: " + r.value("error", std::string("Unreal didn't answer")));
            json result;
            std::string text;
            const json reply = r.contains("result") && r["result"].is_object() ? r["result"] : json::object();
            const json lines = reply.contains("log") ? reply["log"] : json::array();
            for (const json& line : lines)
            {
                const std::string l = line.value("text", "");
                if (l.rfind("LILA_RESULT:", 0) == 0)
                    result = json::parse(l.substr(12), nullptr, false);
                else
                    text += l + "\n";
            }
            if (!result.is_object())
                throw CommandError("unrealSoftNet: Unreal didn't bind the mesh" + (text.empty() ? std::string() : ":\n" + text));
            if (const json errors = result.value("errors", json::array()); !errors.empty())
                throw CommandError("unrealSoftNet: " + errors[0].get<std::string>());
            const json warnings = result.contains("warnings") ? result["warnings"] : json::array();
            for (const json& w : warnings)
                log::write(LogLevel::Warning, "unrealSoftNet: " + w.get<std::string>());
            return {{"skeletalMesh", sm}, {"deformer", payload["deformer"]}, {"joints", names}, {"unreal", result}};
        },
        .examples = {"unrealSoftNet(\"Mochi\", skeletalMesh=\"/Game/Mochi/Mochi\")"},
    });
    {
        ops::options::Override o{.choices = {{"cacheFormat", {{"alembic", "Alembic"}, {"vat", "VAT"}}}},
                                 .editors = {{"route", "segmented"}, {"cacheFormat", "segmented"}}};
        for (const char* f : {"helpers", "correctives", "tolerance", "checkTo", "maxInfluences", "iterations"})
            o.enabledWhen[f] = {"route", {"bake"}};
        for (const char* f : {"cacheFormat", "path"})
            o.enabledWhen[f] = {"route", {"cache"}};
        ops::options::setOverride("softNetUnreal", std::move(o));
    }
}
} // namespace lila::layout
