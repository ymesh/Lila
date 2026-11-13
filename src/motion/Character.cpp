#include "motion/Character.h"

#include "core/Profile.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/Transform.h"

#include <cmath>
#include <stdexcept>

namespace lila::motion
{
namespace
{
const glm::dvec3 kUpD(0.0, 1.0, 0.0);

retarget::Chain chain(retarget::Role role, retarget::Side side, int index, const char* start, const char* end, const char* goal = "")
{
    retarget::Chain c;
    c.role = role;
    c.side = side;
    c.index = index;
    c.name = retarget::chainName(role, side, index);
    c.start = start;
    c.end = end;
    c.goal = goal;
    return c;
}

SomaRig buildSomaRig()
{
    using retarget::Role;
    using retarget::Side;
    const SomaSkeleton& s = SomaSkeleton::shipped();
    SomaRig out;
    for (size_t j = 0; j < s.size(); ++j)
        out.skeleton.add(s.names[j], s.parents[j]);
    out.reference = toRetargetPose(s.reference);

    retarget::Rig& r = out.rig;
    r.pelvis = "Hips";
    r.chains = {
        chain(Role::Spine, Side::Center, 0, "Spine1", "Chest"),
        chain(Role::Neck, Side::Center, 0, "Neck1", "Neck2"),
        chain(Role::Head, Side::Center, 0, "Head", "Head"),
        chain(Role::Jaw, Side::Center, 0, "Jaw", "Jaw"),
    };
    for (const auto& [side, pre] : {std::pair{Side::Left, std::string("Left")}, std::pair{Side::Right, std::string("Right")}})
    {
        auto n = [&](const char* rest) { return pre + rest; };
        auto add = [&](Role role, int index, const std::string& a, const std::string& b, const std::string& goal = {}) {
            retarget::Chain c = chain(role, side, index, "", "", "");
            c.start = a, c.end = b, c.goal = goal;
            r.chains.push_back(c);
        };
        add(Role::Clavicle, 0, n("Shoulder"), n("Shoulder"));
        add(Role::Arm, 0, n("Arm"), n("Hand"), n("Hand"));
        add(Role::Leg, 0, n("Leg"), n("Foot"), n("Foot"));
        add(Role::Toe, 0, n("ToeBase"), n("ToeBase"));
        const char* fingers[] = {"Thumb", "Index", "Middle", "Ring", "Pinky"};
        for (int f = 0; f < 5; ++f)
            add(Role::Finger, f, n("Hand") + fingers[f] + "1", n("Hand") + fingers[f] + (f == 0 ? "3" : "4"));
    }
    r.setRetargetPose(out.skeleton, out.reference);

    const std::vector<glm::dmat4> w = retarget::worldOf(out.skeleton, out.reference);
    const glm::dvec3 lh = retarget::positionOf(w[size_t(out.skeleton.find("LeftLeg"))]);
    const glm::dvec3 rh = retarget::positionOf(w[size_t(out.skeleton.find("RightLeg"))]);
    glm::dvec3 left = lh - rh;
    left.y = 0.0;
    r.up = kUpD;
    r.forward = glm::normalize(glm::cross(glm::normalize(left), kUpD));
    double ground = 1e9;
    for (const char* toe : {"LeftToeBase", "RightToeBase", "LeftToeEnd", "RightToeEnd", "LeftFoot", "RightFoot"})
        if (const int j = out.skeleton.find(toe); j >= 0)
            ground = std::min(ground, retarget::positionOf(w[size_t(j)]).y);
    r.ground = std::min(ground, 0.0);
    return out;
}

/** World matrices (cm) of a SOMA pose. */
std::vector<glm::dmat4> somaWorld(const Pose& p) { return retarget::worldOf(somaRig().skeleton, toRetargetPose(p)); }
} // namespace

const SomaRig& somaRig()
{
    static const SomaRig r = buildSomaRig();
    return r;
}

retarget::Pose toRetargetPose(const Pose& soma)
{
    retarget::Pose p;
    p.rotation.reserve(soma.size());
    p.translate.reserve(soma.size());
    for (const Xform& x : soma)
    {
        p.rotation.push_back(glm::normalize(glm::dquat(x.q)));
        p.translate.push_back(glm::dvec3(x.p) * kCmPerMeter);
    }
    return p;
}

Pose fromRetargetPose(const retarget::Pose& pose)
{
    Pose out(pose.rotation.size());
    for (size_t j = 0; j < out.size(); ++j)
        out[j] = {glm::vec3(pose.translate[j] / kCmPerMeter), glm::normalize(glm::quat(pose.rotation[j]))};
    return out;
}

float headingOf(const Pose& soma)
{
    const SomaSkeleton& s = SomaSkeleton::shipped();
    const Pose w = s.world(soma);
    const glm::vec3 r = w[size_t(s.find("RightLeg"))].p, l = w[size_t(s.find("LeftLeg"))].p;
    glm::vec3 across(r.x - l.x, 0.0f, r.z - l.z);
    if (glm::length(across) <= 1e-6f)
        return 0.0f;
    const glm::vec3 f = glm::cross(glm::vec3(0, 1, 0), glm::normalize(across));
    return std::atan2(f.x, f.z);
}

Pose placed(const Pose& soma, float yaw, glm::vec2 ground)
{
    Pose out = soma;
    const glm::quat turn = glm::angleAxis(yaw, glm::vec3(0, 1, 0));
    out[0].q = glm::normalize(turn * soma[0].q);
    out[0].p = glm::vec3(ground.x, soma[0].p.y, ground.y);
    return out;
}

struct CharacterMap::Impl
{
    retarget::SceneSkeleton scene;
    retarget::Skeleton skeleton;
    retarget::Rig rig;      // the character's, its retarget pose aligned to SOMA's
    retarget::Pose current;
    std::unique_ptr<retarget::Retargeter> fromSoma, toSoma;
    int rootJoint = -1, pelvis = -1, leftThigh = -1, rightThigh = -1;
    glm::dquat rootRest{1, 0, 0, 0};
    double scale = 1.0;
};

CharacterMap::CharacterMap(const retarget::Skeleton& skeleton, const retarget::Rig& rig, const retarget::Pose& current)
{
    LILA_ZONE();
    auto m = std::make_shared<Impl>();
    m->skeleton = skeleton;
    m->current = current;
    const SomaRig& soma = somaRig();
    if (skeleton.find(rig.pelvis) < 0)
        throw std::invalid_argument("the character's retarget rig has no pelvis");

    retarget::Settings toChar = retarget::autoMap(soma.rig, rig);
    toChar.warp.enabled = false;
    m->rig = rig;
    retarget::Pose aligned = retarget::alignRetargetPose(soma.skeleton, soma.rig, soma.reference, skeleton, rig, current, toChar);
    // The pelvis stands over the same ground point as SOMA's in the compared poses, wherever the character was posed
    // when it was characterised, so generated travel lands where the model puts it.
    if (const int pj = skeleton.find(rig.pelvis); pj >= 0)
    {
        const std::vector<glm::dmat4> w = retarget::worldOf(skeleton, aligned);
        const glm::dvec3 hips = retarget::positionOf(retarget::worldOf(soma.skeleton, soma.reference)[0]);
        const glm::dvec3 now = retarget::positionOf(w[size_t(pj)]);
        aligned.translate[size_t(pj)] = retarget::translateFor(retarget::parentWorldOf(skeleton, w, pj), glm::dvec3(hips.x, now.y, hips.z));
    }
    m->rig.setRetargetPose(skeleton, aligned);
    m->fromSoma = std::make_unique<retarget::Retargeter>(soma.skeleton, soma.rig, soma.reference, skeleton, m->rig, current, toChar);
    m->scale = m->fromSoma->scale();

    retarget::Settings back = retarget::autoMap(m->rig, soma.rig);
    back.copyCurves = false;
    back.fullBody = false;
    m->toSoma = std::make_unique<retarget::Retargeter>(skeleton, m->rig, current, soma.skeleton, soma.rig, soma.reference, back);

    m->pelvis = skeleton.find(m->rig.pelvis);
    for (const retarget::Chain& c : m->rig.chains)
    {
        if (c.role == retarget::Role::Root)
            m->rootJoint = skeleton.find(c.start);
        if (c.role == retarget::Role::Leg && c.index == 0 && c.side == retarget::Side::Left)
            m->leftThigh = skeleton.find(c.start);
        if (c.role == retarget::Role::Leg && c.index == 0 && c.side == retarget::Side::Right)
            m->rightThigh = skeleton.find(c.start);
    }
    // Root motion only where the pelvis hangs straight off the root.
    if (m->rootJoint >= 0 && (m->pelvis < 0 || skeleton.parent[size_t(m->pelvis)] != m->rootJoint))
        m->rootJoint = -1;
    if (m->rootJoint >= 0)
    {
        const retarget::Pose rp = m->rig.retargetPose(skeleton, current);
        m->rootRest = retarget::rotationOf(retarget::worldOf(skeleton, rp)[size_t(m->rootJoint)]);
    }
    mImpl = std::move(m);
}

CharacterMap CharacterMap::forJoint(scene::Graph& g, scene::NodeId joint)
{
    retarget::SceneSkeleton sk = retarget::sceneSkeleton(g, joint);
    const retarget::Rig rig = retarget::rigOf(g, retarget::ensureRig(g, sk.top));
    const retarget::Pose now = retarget::currentPose(g, sk);
    CharacterMap map(sk.skeleton, rig, now);
    std::const_pointer_cast<Impl>(map.mImpl)->scene = std::move(sk);
    return map;
}

const retarget::SceneSkeleton& CharacterMap::scene() const { return mImpl->scene; }
const retarget::Skeleton& CharacterMap::skeleton() const { return mImpl->skeleton; }
const retarget::Rig& CharacterMap::rig() const { return mImpl->rig; }
double CharacterMap::scale() const { return mImpl->scale; }

retarget::Pose CharacterMap::toCharacter(const Pose& soma) const
{
    std::vector<Pose> one{soma};
    retarget::Result r = toCharacter(one, {}, {.plantFeet = false});
    return r.poses.front();
}

retarget::Result CharacterMap::toCharacter(const std::vector<Pose>& frames, const std::vector<Contacts>& contacts, const MapOptions& o) const
{
    LILA_ZONE();
    const Impl& m = *mImpl;
    retarget::Frames f;
    f.fps = kSampleRate;
    f.world.reserve(frames.size());
    for (const Pose& p : frames)
        f.world.push_back(somaWorld(p));
    if (o.plantFeet && contacts.size() == frames.size())
    {
        auto& left = f.channels["plantLeftLeg"];
        auto& right = f.channels["plantRightLeg"];
        for (const Contacts& c : contacts)
        {
            left.push_back(std::max(c[0], c[1]) >= o.contactThreshold ? 1.0 : 0.0);
            right.push_back(std::max(c[2], c[3]) >= o.contactThreshold ? 1.0 : 0.0);
        }
    }
    retarget::Result res = frames.size() == 1 ? retarget::Result{.poses = {m.fromSoma->frame(f.world.front())}} : m.fromSoma->clip(f);
    if (!o.plantFeet)
        res.planted.clear();

    if (o.rootMotion && m.rootJoint >= 0 && m.leftThigh >= 0 && m.rightThigh >= 0)
        for (retarget::Pose& p : res.poses)
        {
            const std::vector<glm::dmat4> w = retarget::worldOf(m.skeleton, p);
            const glm::dvec3 pelvis = retarget::positionOf(w[size_t(m.pelvis)]);
            glm::dvec3 left = retarget::positionOf(w[size_t(m.leftThigh)]) - retarget::positionOf(w[size_t(m.rightThigh)]);
            left.y = 0.0;
            if (glm::length(left) < 1e-6)
                continue;
            const glm::dvec3 forward = glm::normalize(glm::cross(glm::normalize(left), kUpD));
            glm::dvec3 restF = m.rig.forward;
            restF.y = 0.0;
            const double yaw = std::atan2(forward.x, forward.z) - std::atan2(restF.x, restF.z);
            const glm::dquat rootRot = glm::angleAxis(yaw, kUpD) * m.rootRest;
            const glm::dvec3 rootPos(pelvis.x, m.rig.ground, pelvis.z);
            const glm::dmat4 rootParent = retarget::parentWorldOf(m.skeleton, w, m.rootJoint);
            p.rotation[size_t(m.rootJoint)] = retarget::localRotationFor(m.skeleton, m.rootJoint, rootParent, rootRot);
            p.translate[size_t(m.rootJoint)] = retarget::translateFor(rootParent, rootPos);
            std::vector<glm::dmat4> w2 = w;
            w2[size_t(m.rootJoint)] = retarget::worldOf(m.skeleton, p)[size_t(m.rootJoint)];
            p.rotation[size_t(m.pelvis)] = retarget::localRotationFor(m.skeleton, m.pelvis, w2[size_t(m.rootJoint)], retarget::rotationOf(w[size_t(m.pelvis)]));
            p.translate[size_t(m.pelvis)] = retarget::translateFor(w2[size_t(m.rootJoint)], pelvis);
        }
    return res;
}

Pose CharacterMap::toSoma(const std::vector<glm::dmat4>& characterWorld) const
{
    return fromRetargetPose(mImpl->toSoma->frame(characterWorld));
}

Pose CharacterMap::sampleSoma(scene::Graph& g, double t) const
{
    const retarget::Frames f = retarget::sample(g, mImpl->scene, t, t, kSampleRate);
    return toSoma(f.world.front());
}
} // namespace lila::motion
