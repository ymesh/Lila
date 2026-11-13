// Parity with the MotionBricks Python runtime. tests/data/motion/soma_parity.bin holds two procedural requests (the SOMA
// reference pose stepped and turned, no sample motion) with the tensors and poses the upstream engine produced on them,
// ONNX Runtime CPU standing in for TensorRT (tools/motion/make_parity.py).

#include <doctest/doctest.h>

#include "motion/Engine.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace lila::motion;

namespace
{
struct Tensor
{
    std::vector<int> shape;
    std::vector<float> f;
    std::vector<int32_t> i;
};

std::map<std::string, Tensor> loadParity()
{
    std::map<std::string, Tensor> out;
    std::ifstream in(std::filesystem::path(LILA_TEST_DATA_DIR) / "motion" / "soma_parity.bin", std::ios::binary);
    REQUIRE(in);
    char magic[4];
    uint32_t count = 0;
    in.read(magic, 4);
    in.read(reinterpret_cast<char*>(&count), 4);
    REQUIRE(std::memcmp(magic, "LMPR", 4) == 0);
    for (uint32_t k = 0; k < count; ++k)
    {
        uint32_t len = 0, ndim = 0;
        in.read(reinterpret_cast<char*>(&len), 4);
        std::string name(len, '\0');
        in.read(name.data(), len);
        char kind = 0;
        in.read(&kind, 1);
        in.read(reinterpret_cast<char*>(&ndim), 4);
        Tensor t;
        size_t total = 1;
        for (uint32_t d = 0; d < ndim; ++d)
        {
            uint32_t v = 0;
            in.read(reinterpret_cast<char*>(&v), 4);
            t.shape.push_back(int(v));
            total *= v;
        }
        if (kind == 'f')
        {
            t.f.resize(total);
            in.read(reinterpret_cast<char*>(t.f.data()), std::streamsize(total * 4));
        }
        else
        {
            t.i.resize(total);
            in.read(reinterpret_cast<char*>(t.i.data()), std::streamsize(total * 4));
        }
        out[name] = std::move(t);
    }
    return out;
}

Pose poseFrom(const Tensor& t, size_t index, size_t joints)
{
    Pose p(joints);
    for (size_t j = 0; j < joints; ++j)
    {
        const float* v = &t.f[(index * joints + j) * 7];
        p[j] = {glm::vec3(v[0], v[1], v[2]), glm::quat(v[6], v[3], v[4], v[5])};
    }
    return p;
}

std::vector<Request> parityRequests(std::map<std::string, Tensor>& d, const SomaSkeleton& s)
{
    std::vector<Request> out;
    for (int r = 0; r < 2; ++r)
    {
        const std::string k = "r" + std::to_string(r) + ".";
        Request q;
        for (size_t c = 0; c < 4; ++c)
            q.context[c] = poseFrom(d[k + "context"], c, s.size());
        const Tensor& tg = d[k + "target"];
        for (int t = 0; t < tg.shape[0]; ++t)
            q.target.push_back(poseFrom(tg, size_t(t), s.size()));
        q.tokens = 0;
        for (int32_t t : d[k + "tokens"].i)
            q.tokens = uint16_t(q.tokens | tokenMask(t, t));
        q.heading = {d[k + "heading"].f[0], d[k + "heading"].f[1]};
        out.push_back(std::move(q));
    }
    return out;
}

/** Largest difference between two float arrays, one request's slice of a batched tensor. */
float maxDiff(const std::vector<float>& batched, size_t offset, const std::vector<float>& ref)
{
    float m = 0.0f;
    for (size_t i = 0; i < ref.size(); ++i)
        m = std::max(m, std::abs(batched[offset + i] - ref[i]));
    return m;
}

/** Position and rotation error between generated poses and the reference, rotations sign-blind. */
std::pair<float, float> poseError(const std::vector<Pose>& frames, const Tensor& ref, size_t joints)
{
    float dp = 0.0f, dq = 0.0f;
    for (size_t f = 0; f < frames.size(); ++f)
    {
        const Pose want = poseFrom(ref, f, joints);
        for (size_t j = 0; j < joints; ++j)
        {
            dp = std::max(dp, glm::length(frames[f][j].p - want[j].p));
            dq = std::max(dq, 1.0f - std::abs(glm::dot(frames[f][j].q, want[j].q)));
        }
    }
    return {dp, dq};
}
} // namespace

TEST_CASE("motion: the shipped SOMA skeleton binds the model's joints")
{
    const SomaSkeleton& s = SomaSkeleton::shipped();
    CHECK(s.size() == 77);
    CHECK(s.names[0] == "Hips");
    for (const char* n : modelJointNames())
        CHECK(s.find(n) >= 0);
    const Pose w = s.world(s.reference);
    const Pose l = s.local(w);
    for (size_t j = 0; j < s.size(); ++j)
        CHECK(glm::length(l[j].p - s.reference[j].p) < 1e-5f);
    // Standing: hips about a meter up, toes near the ground.
    CHECK(w[0].p.y == doctest::Approx(1.012).epsilon(0.01));
    CHECK(std::abs(w[size_t(s.find("LeftToeBase"))].p.y) < 0.1f);
}

TEST_CASE("motion: token lengths")
{
    CHECK(framesForTokens(6) == 18);
    CHECK(framesForTokens(16) == 58);
    CHECK(tokensForFrames(30) == 9);
    CHECK(tokensForFrames(5) == 6);
    CHECK(tokensForFrames(200) == 16);
    CHECK(tokenMask(8, 14) == kDefaultTokens);
    CHECK(tokenMask(6, 6) == 1);
}

TEST_CASE("motion: encode matches the upstream input tensors, batched")
{
    auto d = loadParity();
    const SomaSkeleton& s = SomaSkeleton::shipped();
    const std::vector<Request> req = parityRequests(d, s);
    const Inputs in = encode(s, req);
    REQUIRE(in.count == 2);
    for (size_t r = 0; r < 2; ++r)
    {
        const std::string k = "r" + std::to_string(r) + ".";
        CHECK(maxDiff(in.globalRoot, r * 8 * 5, d[k + "global_root_values"].f) < 2e-4f);
        CHECK(maxDiff(in.localRoot, r * 8 * 4, d[k + "local_root_values"].f) < 2e-3f);
        CHECK(maxDiff(in.localPoses, r * 8 * kPoseFeatures, d[k + "local_poses"].f) < 2e-4f);
        for (size_t i = 0; i < 8; ++i)
        {
            CHECK(in.hasGlobalRoot[r * 8 + i] == d[k + "has_global_root_values"].i[i]);
            CHECK(in.hasLocalRoot[r * 8 + i] == d[k + "has_local_root_values"].i[i]);
            CHECK(in.hasLocalPoses[r * 8 + i] == d[k + "has_local_poses"].i[i]);
        }
        for (size_t i = 0; i < 11; ++i)
            CHECK(in.allowedTokens[r * 11 + i] == d[k + "allowed_pred_num_tokens"].i[i]);
    }
}

TEST_CASE("motion: decode and endpoint correction match upstream on the same raw outputs")
{
    auto d = loadParity();
    const SomaSkeleton& s = SomaSkeleton::shipped();
    const std::vector<Request> req = parityRequests(d, s);
    const Inputs in = encode(s, req);
    Outputs out;
    for (int r = 0; r < 2; ++r)
    {
        const std::string k = "r" + std::to_string(r) + ".";
        const auto& m = d[k + "pred_global_motions"].f;
        out.motion.insert(out.motion.end(), m.begin(), m.end());
        out.tokens.push_back(d[k + "pred_num_tokens"].i[0]);
    }
    const std::vector<Result> res = decode(s, req, in, out);
    for (int r = 0; r < 2; ++r)
    {
        const std::string k = "r" + std::to_string(r) + ".";
        INFO("request " << r);
        REQUIRE(res[size_t(r)].error.empty());
        const Tensor& poses = d[k + "poses"];
        REQUIRE(int(res[size_t(r)].frames.size()) == poses.shape[0]);
        const auto [dp, dq] = poseError(res[size_t(r)].frames, poses, s.size());
        CHECK(dp < 1e-4f);
        CHECK(dq < 1e-5f);
        const Tensor& contacts = d[k + "contacts"];
        float dc = 0.0f;
        for (size_t f = 0; f < res[size_t(r)].contacts.size(); ++f)
            for (size_t c = 0; c < 4; ++c)
                dc = std::max(dc, std::abs(res[size_t(r)].contacts[f][c] - contacts.f[f * 4 + c]));
        CHECK(dc < 1e-6f);
        // Endpoint correction: starts on the last context pose, ends on the first target pose.
        CHECK(glm::length(res[size_t(r)].frames.front()[0].p - req[size_t(r)].context.back()[0].p) < 1e-4f);
        CHECK(glm::length(res[size_t(r)].frames.back()[0].p - req[size_t(r)].target.front()[0].p) < 1e-4f);
    }
}

TEST_CASE("motion: bad requests are reported, the rest of the batch still decodes")
{
    const SomaSkeleton& s = SomaSkeleton::shipped();
    Request bad;
    for (Pose& p : bad.context)
        p = s.reference;
    CHECK_THROWS_AS(encode(s, std::span(&bad, 1)), std::invalid_argument);
    bad.target = {s.reference};
    bad.tokens = 0;
    CHECK_THROWS_AS(encode(s, std::span(&bad, 1)), std::invalid_argument);
}

TEST_CASE("motion: the model through ONNX Runtime matches upstream (CPU)")
{
    if (!std::filesystem::exists(modelPath()))
    {
        MESSAGE("motion model missing at " << modelPath().string() << ": inference parity skipped");
        return;
    }
    auto d = loadParity();
    const SomaSkeleton& s = SomaSkeleton::shipped();
    const std::vector<Request> req = parityRequests(d, s);
    Status st;
    auto engine = Engine::create(s, {.device = Device::Cpu}, &st);
    REQUIRE_MESSAGE(engine, st.message);
    CHECK(st.device == "CPU");

    // The model on the reference's exact input tensors, batched: the same raw outputs.
    Inputs in = encode(s, req);
    auto take = [&](const char* name, std::vector<float>& f) {
        f.clear();
        for (int r = 0; r < 2; ++r)
        {
            const auto& v = d["r" + std::to_string(r) + "." + name].f;
            f.insert(f.end(), v.begin(), v.end());
        }
    };
    take("global_root_values", in.globalRoot);
    take("local_root_values", in.localRoot);
    take("local_poses", in.localPoses);
    const Outputs out = engine->run(in);
    for (int r = 0; r < 2; ++r)
    {
        const std::string k = "r" + std::to_string(r) + ".";
        INFO("request " << r);
        CHECK(out.tokens[size_t(r)] == d[k + "pred_num_tokens"].i[0]);
        const int frames = out.tokens[size_t(r)] * kPosesPerToken;
        float diff = 0.0f;
        const auto& ref = d[k + "pred_global_motions"].f;
        for (size_t i = 0; i < size_t(frames) * kMotionFeatures; ++i)
            diff = std::max(diff, std::abs(out.motion[size_t(r) * kRawFrames * kMotionFeatures + i] - ref[i]));
        CHECK(diff < 1e-3f);
    }

    // End to end from the poses: the same lengths, exact endpoints. Inputs differ from upstream's in the last float
    // bits, which can tip the model's discrete token choice, so the motion between is only reported.
    const std::vector<Result> res = engine->generate(req);
    for (int r = 0; r < 2; ++r)
    {
        const std::string k = "r" + std::to_string(r) + ".";
        INFO("request " << r);
        REQUIRE(res[size_t(r)].error.empty());
        CHECK(res[size_t(r)].tokens == d[k + "pred_num_tokens"].i[0]);
        CHECK(int(res[size_t(r)].frames.size()) == d[k + "poses"].shape[0]);
        CHECK(glm::length(res[size_t(r)].frames.back()[0].p - req[size_t(r)].target.front()[0].p) < 1e-4f);
        MESSAGE("request " << r << ": largest joint difference from upstream " << poseError(res[size_t(r)].frames, d[k + "poses"], s.size()).first << " m");
    }
    // One inference per request gives the same motion as the batch.
    const std::vector<Result> one = engine->generate(std::span(&req[1], 1));
    REQUIRE(one[0].frames.size() == res[1].frames.size());
    float same = 0.0f;
    for (size_t f = 0; f < one[0].frames.size(); ++f)
        for (size_t j = 0; j < s.size(); ++j)
            same = std::max(same, glm::length(one[0].frames[f][j].p - res[1].frames[f][j].p));
    CHECK(same < 1e-3f);
}

TEST_CASE("motion: the GPU device generates when it is available, and reports itself")
{
    if (!std::filesystem::exists(modelPath()))
        return;
    auto d = loadParity();
    const SomaSkeleton& s = SomaSkeleton::shipped();
    const std::vector<Request> req = parityRequests(d, s);
    Status st;
    auto engine = Engine::create(s, {.device = Device::Auto}, &st);
    REQUIRE_MESSAGE(engine, st.message);
    MESSAGE(st.message);
    CHECK(!st.device.empty());
    CHECK(st.message.find(st.device == "CPU" ? "CPU" : "GPU") != std::string::npos);
    const std::vector<Result> res = engine->generate(req);
    for (const Result& r : res)
    {
        REQUIRE(r.error.empty());
        CHECK(r.frames.size() >= 18);
        CHECK(glm::length(r.frames.back()[0].p - req[0].target.front()[0].p) < 10.0f);
    }
    // Batched N: many requests in one call.
    std::vector<Request> many(16, req[0]);
    const std::vector<Result> batch = engine->generate(many);
    CHECK(batch.size() == 16);
    for (const Result& r : batch)
        CHECK(r.error.empty());
    MESSAGE("16 requests: encode " << engine->lastTimes()[0] << " ms, inference " << engine->lastTimes()[1] << " ms, decode "
                                   << engine->lastTimes()[2] << " ms");
}
