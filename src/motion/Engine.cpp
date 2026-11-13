// Input preparation, decoding and endpoint correction follow the MotionBricks SOMA runtime (Apache-2.0, NVIDIA;
// motionbricks/model/soma/kernels.py and model/kernels.py), so the model sees exactly what it was trained on.

#include "motion/Engine.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"

#include <nlohmann/json.hpp>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace lila::motion
{
namespace fs = std::filesystem;

namespace
{
constexpr std::array<const char*, kModelJoints> kJointNames = {
    "Hips",          "Spine1",           "Spine2",            "Chest",        "Neck1",       "Neck2",
    "Head",          "Jaw",              "LeftEye",           "RightEye",     "LeftShoulder", "LeftArm",
    "LeftForeArm",   "LeftHand",         "LeftHandThumbEnd",  "LeftHandMiddleEnd", "RightShoulder", "RightArm",
    "RightForeArm",  "RightHand",        "RightHandThumbEnd", "RightHandMiddleEnd", "LeftLeg",  "LeftShin",
    "LeftFoot",      "LeftToeBase",      "RightLeg",          "RightShin",    "RightFoot",   "RightToeBase",
};
constexpr int kSlots = 2 * kPosesPerToken;
const glm::vec3 kUp(0.0f, 1.0f, 0.0f);

/** Shortest-arc slerp as Warp computes it, so corrections match the reference. */
glm::quat slerp(const glm::quat& q0, const glm::quat& q1, float t)
{
    const glm::quat d = glm::conjugate(q0) * q1;
    const glm::vec3 v(d.x, d.y, d.z);
    const float len = glm::length(v);
    if (len <= 0.0f)
        return q0;
    const glm::vec3 axis = (d.w < 0.0f ? -v : v) / len;
    const float angle = 2.0f * std::atan2(len, std::abs(d.w));
    return q0 * glm::angleAxis(t * angle, axis);
}

Xform blend(const Xform& a, const Xform& b, float t) { return {glm::mix(a.p, b.p, t), slerp(a.q, b.q, t)}; }

/** Heading of a pose from its hip joints, as training measured it (compute_heading_method="hips_pos"). */
float headingFromHips(const glm::vec3& right, const glm::vec3& left)
{
    glm::vec3 across(right.x - left.x, 0.0f, right.z - left.z);
    const float len = glm::length(across);
    if (len <= 1.0e-6f)
        return 0.0f;
    across /= len;
    const glm::vec3 forward = glm::cross(kUp, across);
    return std::atan2(forward.x, forward.z);
}

glm::quat rotationFrom6d(const float* f)
{
    const glm::vec3 x = glm::normalize(glm::vec3(f[0], f[1], f[2]));
    const glm::vec3 yRaw(f[3], f[4], f[5]);
    const glm::vec3 z = glm::normalize(glm::cross(x, yRaw));
    const glm::vec3 y = glm::cross(z, x);
    return glm::normalize(glm::quat_cast(glm::mat3(x, y, z)));
}

/** How a skeleton's joints line up with the model's. */
struct Binding
{
    std::array<int, kModelJoints> modelToPose{};
    std::vector<int> poseToModel;
    int rightLeg = -1, leftLeg = -1;
};

Binding bind(const SomaSkeleton& s)
{
    Binding b;
    std::string missing;
    for (int m = 0; m < kModelJoints; ++m)
    {
        b.modelToPose[size_t(m)] = s.find(kJointNames[size_t(m)]);
        if (b.modelToPose[size_t(m)] < 0)
            missing += std::string(missing.empty() ? "" : ", ") + kJointNames[size_t(m)];
    }
    if (!missing.empty())
        throw std::invalid_argument("the skeleton is missing model joints: " + missing);
    if (b.modelToPose[0] != 0)
        throw std::invalid_argument("the skeleton must have Hips at index 0");
    b.poseToModel.assign(s.size(), -1);
    for (int m = 0; m < kModelJoints; ++m)
    {
        const int j = b.modelToPose[size_t(m)];
        const int parent = s.parents[size_t(j)];
        // Model joints outside the connected hierarchy keep their reference transforms when decoded.
        if (parent < 0 || b.poseToModel[size_t(parent)] >= 0)
            b.poseToModel[size_t(j)] = m;
    }
    b.rightLeg = s.find("RightLeg");
    b.leftLeg = s.find("LeftLeg");
    return b;
}

glm::vec3 aligned(const Xform& a, const glm::vec3& p) { return glm::conjugate(a.q) * (p - a.p); }

void checkPose(const SomaSkeleton& s, const Pose& p, size_t request, const char* what)
{
    if (p.size() != s.size())
        throw std::invalid_argument("request " + std::to_string(request) + ": a " + what + " pose has " + std::to_string(p.size()) +
                                    " joints, the skeleton " + std::to_string(s.size()));
}
} // namespace

const std::array<const char*, kModelJoints>& modelJointNames() { return kJointNames; }

int tokensForFrames(double frames)
{
    int best = kMinTokens;
    for (int t = kMinTokens; t <= kMaxTokens; ++t)
        if (std::abs(framesForTokens(t) - frames) < std::abs(framesForTokens(best) - frames))
            best = t;
    return best;
}

int SomaSkeleton::find(std::string_view name) const
{
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == name)
            return int(i);
    return -1;
}

Pose SomaSkeleton::world(const Pose& local) const
{
    Pose w(local.size());
    for (size_t j = 0; j < local.size(); ++j)
        w[j] = parents[j] < 0 ? local[j] : w[size_t(parents[j])] * local[j];
    return w;
}

Pose SomaSkeleton::local(const Pose& world) const
{
    Pose l(world.size());
    for (size_t j = 0; j < world.size(); ++j)
        l[j] = parents[j] < 0 ? world[j] : world[size_t(parents[j])].inverse() * world[j];
    return l;
}

SomaSkeleton SomaSkeleton::load(const fs::path& file)
{
    std::ifstream in(file);
    if (!in)
        throw std::runtime_error("cannot open " + paths::toUtf8(file));
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.contains("joints"))
        throw std::runtime_error(paths::toUtf8(file) + " is not a SOMA skeleton");
    SomaSkeleton s;
    for (const auto& e : j["joints"])
    {
        s.names.push_back(e.at("name").get<std::string>());
        const int parent = e.at("parent").get<int>();
        if (parent >= int(s.parents.size()))
            throw std::runtime_error(paths::toUtf8(file) + ": joint " + s.names.back() + " comes before its parent");
        s.parents.push_back(parent);
        const auto& t = e.at("translate");
        const auto& r = e.at("rotation"); // x, y, z, w
        s.reference.push_back({glm::vec3(t[0].get<float>(), t[1].get<float>(), t[2].get<float>()),
                               glm::normalize(glm::quat(r[3].get<float>(), r[0].get<float>(), r[1].get<float>(), r[2].get<float>()))});
    }
    bind(s);
    return s;
}

const SomaSkeleton& SomaSkeleton::shipped()
{
    static const SomaSkeleton s = load(paths::resourcesDir() / "motion" / "soma_skeleton.json");
    return s;
}

Inputs encode(const SomaSkeleton& s, std::span<const Request> requests)
{
    LILA_ZONE();
    const Binding b = bind(s);
    const size_t n = requests.size();
    Inputs in;
    in.count = int(n);
    in.globalRoot.assign(n * kSlots * 5, 0.0f);
    in.localRoot.assign(n * kSlots * 4, 0.0f);
    in.localPoses.assign(n * kSlots * kPoseFeatures, 0.0f);
    in.hasGlobalRoot.assign(n * kSlots, 0);
    in.hasLocalRoot.assign(n * kSlots, 0);
    in.hasLocalPoses.assign(n * kSlots, 0);
    in.allowedTokens.assign(n * kTokenChoices, 0);
    in.alignment.resize(n);

    for (size_t r = 0; r < n; ++r)
    {
        const Request& q = requests[r];
        const int targets = int(q.target.size());
        if (targets < 1 || targets > kPosesPerToken)
            throw std::invalid_argument("request " + std::to_string(r) + ": give 1 to 4 target poses");
        if ((q.tokens & tokenMask(kMinTokens, kMaxTokens)) == 0)
            throw std::invalid_argument("request " + std::to_string(r) + ": allow at least one length");
        if (glm::length(q.heading) <= 1.0e-6f || !std::isfinite(q.heading.x) || !std::isfinite(q.heading.y))
            throw std::invalid_argument("request " + std::to_string(r) + ": heading must be a finite, non-zero direction");

        std::array<Pose, kSlots> world;
        for (int k = 0; k < kPosesPerToken; ++k)
        {
            checkPose(s, q.context[size_t(k)], r, "context");
            world[size_t(k)] = s.world(q.context[size_t(k)]);
        }
        for (int k = 0; k < kPosesPerToken; ++k)
        {
            const Pose& p = q.target[size_t(std::min(k, targets - 1))];
            checkPose(s, p, r, "target");
            world[size_t(kPosesPerToken + k)] = s.world(p);
        }

        const Pose& last = world[kPosesPerToken - 1];
        const glm::vec3 hips = last[0].p;
        const float current = headingFromHips(last[size_t(b.rightLeg)].p, last[size_t(b.leftLeg)].p);
        const float desired = std::atan2(q.heading.x, q.heading.y);
        const Xform align{glm::vec3(hips.x, 0.0f, hips.z), glm::angleAxis(current - desired, kUp)};
        in.alignment[r] = align;
        const glm::quat inv = glm::conjugate(align.q);

        for (int f = 0; f < kSlots; ++f)
        {
            // Root velocities use the next pose, so they are zero at the last context and last target pose.
            const int next = f == kPosesPerToken - 1 ? f : std::min(f + 1, kSlots - 1);
            const Pose& w = world[size_t(f)];
            const Pose& wn = world[size_t(next)];
            const glm::vec3 h = aligned(align, w[0].p), hn = aligned(align, wn[0].p);
            const float heading = headingFromHips(aligned(align, w[size_t(b.rightLeg)].p), aligned(align, w[size_t(b.leftLeg)].p));
            const float headingNext = headingFromHips(aligned(align, wn[size_t(b.rightLeg)].p), aligned(align, wn[size_t(b.leftLeg)].p));
            const float dh = std::atan2(std::sin(headingNext - heading), std::cos(headingNext - heading));
            const float rate = float(kSampleRate);

            float* g = &in.globalRoot[(r * kSlots + size_t(f)) * 5];
            g[0] = h.x, g[1] = h.y, g[2] = h.z, g[3] = std::cos(heading), g[4] = std::sin(heading);
            float* l = &in.localRoot[(r * kSlots + size_t(f)) * 4];
            l[0] = dh * rate, l[1] = (hn.x - h.x) * rate, l[2] = (hn.z - h.z) * rate, l[3] = h.y;

            const glm::vec3 root(h.x, 0.0f, h.z);
            float* pose = &in.localPoses[(r * kSlots + size_t(f)) * kPoseFeatures];
            for (int m = 0; m < kModelJoints; ++m)
            {
                const Xform& jw = w[size_t(b.modelToPose[size_t(m)])];
                if (m > 0)
                {
                    const glm::vec3 c = aligned(align, jw.p) - root;
                    float* pp = pose + (m - 1) * 3;
                    pp[0] = c.x, pp[1] = c.y, pp[2] = c.z;
                }
                const glm::quat jr = inv * jw.q;
                const glm::vec3 x = jr * glm::vec3(1, 0, 0), y = jr * glm::vec3(0, 1, 0);
                float* pr = pose + (kModelJoints - 1) * 3 + m * 6;
                pr[0] = x.x, pr[1] = x.y, pr[2] = x.z, pr[3] = y.x, pr[4] = y.y, pr[5] = y.z;
            }

            const int32_t given = f < kPosesPerToken + targets ? 1 : 0;
            in.hasGlobalRoot[r * kSlots + size_t(f)] = given;
            in.hasLocalPoses[r * kSlots + size_t(f)] = given;
            // The derivatives are unknown at the last context pose and the last target pose.
            in.hasLocalRoot[r * kSlots + size_t(f)] = (f == kPosesPerToken - 1 || f == kPosesPerToken + targets - 1) ? 0 : given;
        }
        for (int i = 0; i < kTokenChoices; ++i)
            in.allowedTokens[r * kTokenChoices + size_t(i)] = (q.tokens >> i) & 1;
    }
    return in;
}

std::vector<Result> decode(const SomaSkeleton& s, std::span<const Request> requests, const Inputs& in, const Outputs& out)
{
    LILA_ZONE();
    const Binding b = bind(s);
    const size_t n = requests.size();
    if (out.tokens.size() < n || out.motion.size() < n * kRawFrames * kMotionFeatures || in.alignment.size() < n)
        throw std::invalid_argument("decode: outputs hold fewer requests than given");
    std::vector<Result> results(n);
    constexpr int posOffset = 5;
    constexpr int rotOffset = posOffset + (kModelJoints - 1) * 3;
    constexpr int contactOffset = kMotionFeatures - 4;

    for (size_t r = 0; r < n; ++r)
    {
        Result& res = results[r];
        const Request& q = requests[r];
        res.tokens = out.tokens[r];
        if (res.tokens < kMinTokens || res.tokens > kMaxTokens || !((q.tokens >> (res.tokens - kMinTokens)) & 1))
        {
            res.error = "the model chose " + std::to_string(res.tokens) + " tokens, outside the allowed lengths";
            continue;
        }
        const int rawCount = std::clamp(res.tokens * kPosesPerToken, 0, kRawFrames);
        const int count = std::max(0, rawCount - 2 * kBoundaryFrames);
        res.frames.resize(size_t(count));
        res.contacts.resize(size_t(count));
        const Xform& align = in.alignment[r];

        for (int f = 0; f < count; ++f)
        {
            const float* feat = &out.motion[(r * kRawFrames + size_t(kBoundaryFrames + f)) * kMotionFeatures];
            for (int c = 0; c < 4; ++c)
                res.contacts[size_t(f)][size_t(c)] = std::clamp(feat[contactOffset + c], 0.0f, 1.0f);

            Pose& pose = res.frames[size_t(f)];
            pose.resize(s.size());
            // Also restores the ground offset removed when the poses were encoded.
            pose[0] = align * Xform{glm::vec3(feat[0], feat[1], feat[2]), rotationFrom6d(feat + rotOffset)};
            for (size_t j = 1; j < s.size(); ++j)
            {
                const int m = b.poseToModel[j];
                if (m < 0)
                {
                    pose[j] = s.reference[j];
                    continue;
                }
                const int pm = b.poseToModel[size_t(s.parents[j])];
                // The common alignment cancels in parent-to-child space.
                const glm::quat local = glm::conjugate(rotationFrom6d(feat + rotOffset + pm * 6)) * rotationFrom6d(feat + rotOffset + m * 6);
                pose[j] = {s.reference[j].p, glm::normalize(local)};
            }
        }

        if (!q.correctEndpoints || count < 2)
            continue;
        const Pose& from = q.context.back();
        const Pose& to = q.target.front();
        const size_t joints = s.size();
        std::vector<Xform> start(joints), end(joints);
        auto rootDelta = [](const Xform& gen, const Xform& guide) {
            return Xform{guide.p - gen.p, glm::normalize(glm::conjugate(glm::normalize(gen.q)) * glm::normalize(guide.q))};
        };
        start[0] = rootDelta(res.frames.front()[0], from[0]);
        end[0] = rootDelta(res.frames.back()[0], to[0]);
        for (size_t j = 1; j < joints; ++j)
        {
            start[j] = res.frames.front()[j].inverse() * from[j];
            end[j] = res.frames.back()[j].inverse() * to[j];
        }
        const Xform identity;
        for (int f = 0; f < count; ++f)
        {
            const float we = float(f) / float(count - 1), ws = 1.0f - we;
            Pose& pose = res.frames[size_t(f)];
            Xform& root = pose[0];
            root = {root.p + start[0].p * ws + end[0].p * we,
                    glm::normalize(root.q * slerp(glm::quat(1, 0, 0, 0), start[0].q, ws) * slerp(glm::quat(1, 0, 0, 0), end[0].q, we))};
            for (size_t j = 1; j < joints; ++j)
            {
                const Xform corrected = pose[j] * blend(identity, start[j], ws);
                pose[j] = corrected * blend(identity, end[j], we);
            }
        }
    }
    return results;
}

fs::path modelPath()
{
    constexpr const char* kFile = "mmm_v6_soma_fp16.onnx";
    if (const char* env = std::getenv("LILA_MOTION_MODEL"); env && *env)
        return paths::fromUtf8(env);
    const fs::path beside = paths::executableDir() / "models" / "motionbricks" / kFile;
    if (fs::exists(beside))
        return beside;
    const fs::path res = paths::resourcesDir() / "models" / "motionbricks" / kFile;
    return fs::exists(res) ? res : beside;
}

namespace
{
fs::path moduleDir()
{
#ifdef _WIN32
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&moduleDir), &self);
    wchar_t buf[MAX_PATH * 4] = {};
    GetModuleFileNameW(self, buf, DWORD(std::size(buf)));
    return fs::path(buf).parent_path();
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&moduleDir), &info) && info.dli_fname)
        return fs::path(info.dli_fname).parent_path();
    return paths::executableDir();
#endif
}

/** Loads the ONNX Runtime beside LilaScene (never a system copy, which may be older). */
const OrtApi* loadRuntime(std::string& error, std::string& version)
{
    static std::once_flag once;
    static const OrtApi* api = nullptr;
    static std::string loadError, loadVersion;
    std::call_once(once, [] {
        using GetBase = const OrtApiBase* (*)();
        GetBase getBase = nullptr;
#ifdef _WIN32
        const fs::path dll = moduleDir() / "onnxruntime.dll";
        HMODULE h = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!h)
        {
            loadError = "ONNX Runtime is missing (" + paths::toUtf8(dll) + ")";
            return;
        }
        getBase = reinterpret_cast<GetBase>(GetProcAddress(h, "OrtGetApiBase"));
#else
        fs::path so = moduleDir() / "libonnxruntime.so.1";
        void* h = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h)
            h = dlopen("libonnxruntime.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!h)
        {
            loadError = "ONNX Runtime is missing (" + paths::toUtf8(so) + ")";
            return;
        }
        getBase = reinterpret_cast<GetBase>(dlsym(h, "OrtGetApiBase"));
#endif
        const OrtApiBase* base = getBase ? getBase() : nullptr;
        if (!base)
        {
            loadError = "ONNX Runtime has no OrtGetApiBase";
            return;
        }
        loadVersion = base->GetVersionString();
        api = base->GetApi(ORT_API_VERSION);
        if (!api)
            loadError = "ONNX Runtime " + loadVersion + " is older than Lila needs (" LILA_ORT_VERSION ")";
        else
            Ort::InitApi(api);
    });
    error = loadError;
    version = loadVersion;
    return api;
}

/** The first entry of OrtDmlApi (dml_provider_factory.h), declared here so building needs no Direct3D headers. */
struct DmlApiHead
{
    OrtStatus*(ORT_API_CALL* appendDml)(OrtSessionOptions* options, int deviceId) NO_EXCEPTION;
};

const std::array<const char*, 7> kInputNames = {"global_root_values",  "has_global_root_values", "local_root_values", "has_local_root_values",
                                                "local_poses",         "has_local_poses",        "allowed_pred_num_tokens"};
const std::array<const char*, 2> kOutputNames = {"pred_global_motions", "pred_num_tokens"};
} // namespace

struct Engine::Impl
{
    SomaSkeleton skeleton;
    Status status;
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    std::mutex run;
    std::array<double, 3> times{};
};

Engine::Engine() : mImpl(std::make_unique<Impl>()) {}
Engine::~Engine() = default;

std::unique_ptr<Engine> Engine::create(const SomaSkeleton& skeleton, const EngineOptions& options, Status* status)
{
    LILA_ZONE();
    Status st;
    st.model = options.model.empty() ? modelPath() : options.model;
    auto fail = [&](const std::string& why) {
        st.ready = false;
        st.message = why;
        if (status)
            *status = st;
        return nullptr;
    };
    std::string error;
    if (!loadRuntime(error, st.runtimeVersion))
        return fail(error);
    if (!fs::exists(st.model))
        return fail("The motion model is missing: " + paths::toUtf8(st.model) +
                    ". It ships with Lila; reinstall, or set LILA_MOTION_MODEL to mmm_v6_soma_fp16.onnx.");

    auto engine = std::unique_ptr<Engine>(new Engine);
    Impl& m = *engine->mImpl;
    m.skeleton = skeleton;
    try
    {
        bind(skeleton);
        m.env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "lila-motion");
    }
    catch (const std::exception& e)
    {
        return fail(e.what());
    }

    auto makeSession = [&](bool gpu) {
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        if (options.threads > 0)
            so.SetIntraOpNumThreads(options.threads);
        if (gpu)
        {
            const DmlApiHead* dml = nullptr;
            Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dml)));
            if (!dml)
                throw std::runtime_error("this ONNX Runtime has no DirectML");
            so.DisableMemPattern();
            so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
            Ort::ThrowOnError(dml->appendDml(so, 0));
        }
        m.session = std::make_unique<Ort::Session>(*m.env, st.model.c_str(), so);
    };

    // A short request proves the device works before anything relies on it.
    auto probe = [&] {
        Request r;
        for (Pose& p : r.context)
            p = skeleton.reference;
        r.target = {skeleton.reference};
        r.target[0][0].p.z += 1.0f;
        r.tokens = tokenMask(kMinTokens, kMinTokens);
        const Request batch[] = {r};
        const Outputs out = engine->run(encode(skeleton, batch));
        for (float v : out.motion)
            if (!std::isfinite(v))
                throw std::runtime_error("the GPU returned invalid numbers");
    };

    std::string gpuWhy;
#ifdef _WIN32
    const bool tryGpu = options.device != Device::Cpu;
#else
    const bool tryGpu = false;
    if (options.device != Device::Cpu)
        gpuWhy = "no GPU runtime for this platform";
#endif
    if (tryGpu)
    {
        try
        {
            makeSession(true);
            probe();
            st.device = "DirectML";
        }
        catch (const std::exception& e)
        {
            m.session.reset();
            gpuWhy = std::string("DirectML failed: ") + e.what();
            if (options.device == Device::Gpu)
                return fail("Motion on the GPU is unavailable: " + gpuWhy);
        }
    }
    if (!m.session)
    {
        try
        {
            makeSession(false);
            probe();
            st.device = "CPU";
        }
        catch (const std::exception& e)
        {
            return fail(std::string("The motion model could not load: ") + e.what());
        }
    }
    st.ready = true;
    st.message = "Motion runs on " + (st.device == "CPU" ? std::string("the CPU") : "the GPU (DirectML)");
    if (st.device == "CPU" && !gpuWhy.empty())
        st.message += ": " + gpuWhy;
    else if (st.device == "CPU" && options.device == Device::Cpu)
        st.message += " (Preferences > Motion > Device)";
    m.status = st;
    if (status)
        *status = st;
    log::write(st.device == "CPU" && !gpuWhy.empty() ? LogLevel::Warning : LogLevel::Info, "motion: " + st.message);
    return engine;
}

Outputs Engine::run(const Inputs& in)
{
    LILA_ZONE();
    const int64_t n = in.count;
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto f32 = [&](const std::vector<float>& v, std::initializer_list<int64_t> shape) {
        return Ort::Value::CreateTensor<float>(mem, const_cast<float*>(v.data()), v.size(), shape.begin(), shape.size());
    };
    auto i32 = [&](const std::vector<int32_t>& v, std::initializer_list<int64_t> shape) {
        return Ort::Value::CreateTensor<int32_t>(mem, const_cast<int32_t*>(v.data()), v.size(), shape.begin(), shape.size());
    };
    std::array<Ort::Value, 7> values = {
        f32(in.globalRoot, {n, kSlots, 5}),          i32(in.hasGlobalRoot, {n, kSlots}), f32(in.localRoot, {n, kSlots, 4}),
        i32(in.hasLocalRoot, {n, kSlots}),            f32(in.localPoses, {n, kSlots, kPoseFeatures}),
        i32(in.hasLocalPoses, {n, kSlots}),           i32(in.allowedTokens, {n, kTokenChoices}),
    };
    std::vector<Ort::Value> outs;
    {
        std::lock_guard lock(mImpl->run);
        outs = mImpl->session->Run(Ort::RunOptions{nullptr}, kInputNames.data(), values.data(), values.size(), kOutputNames.data(),
                                   kOutputNames.size());
    }
    Outputs o;
    const float* motion = outs[0].GetTensorData<float>();
    o.motion.assign(motion, motion + size_t(n) * kRawFrames * kMotionFeatures);
    const int32_t* tokens = outs[1].GetTensorData<int32_t>();
    o.tokens.assign(tokens, tokens + n);
    return o;
}

std::vector<Result> Engine::generate(std::span<const Request> requests)
{
    LILA_ZONE();
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::time_point a, clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    if (requests.empty())
        return {};
    // Bad requests are answered alone, so one never fails the rest of a batch.
    std::vector<Result> results(requests.size());
    std::vector<Request> good;
    std::vector<size_t> index;
    for (size_t i = 0; i < requests.size(); ++i)
    {
        try
        {
            encode(mImpl->skeleton, std::span(&requests[i], 1));
            good.push_back(requests[i]);
            index.push_back(i);
        }
        catch (const std::exception& e)
        {
            results[i].error = e.what();
        }
    }
    if (good.empty())
        return results;
    const auto t0 = clock::now();
    const Inputs in = encode(mImpl->skeleton, good);
    const auto t1 = clock::now();
    const Outputs out = run(in);
    const auto t2 = clock::now();
    std::vector<Result> decoded = decode(mImpl->skeleton, good, in, out);
    const auto t3 = clock::now();
    mImpl->times = {ms(t0, t1), ms(t1, t2), ms(t2, t3)};
    for (size_t k = 0; k < index.size(); ++k)
        results[index[k]] = std::move(decoded[k]);
    return results;
}

const SomaSkeleton& Engine::skeleton() const { return mImpl->skeleton; }
const Status& Engine::info() const { return mImpl->status; }
std::array<double, 3> Engine::lastTimes() const { return mImpl->times; }

namespace
{
std::mutex gSharedMutex;
// Never destroyed at exit: tearing down a DirectML session while the process unloads its DLLs fails fast.
Engine* gShared = nullptr;
Status gSharedStatus;
bool gTried = false;
Device gDevice = Device::Auto;
} // namespace

Engine* Engine::shared()
{
    std::lock_guard lock(gSharedMutex);
    if (!gTried)
    {
        gTried = true;
        try
        {
            EngineOptions o;
            o.device = gDevice;
            gShared = create(SomaSkeleton::shipped(), o, &gSharedStatus).release();
        }
        catch (const std::exception& e)
        {
            gSharedStatus = {};
            gSharedStatus.message = e.what();
        }
        if (!gShared)
            log::write(LogLevel::Warning, "motion: " + gSharedStatus.message);
    }
    return gShared;
}

Status Engine::status()
{
    shared();
    std::lock_guard lock(gSharedMutex);
    return gSharedStatus;
}

void Engine::resetShared()
{
    std::lock_guard lock(gSharedMutex);
    delete gShared;
    gShared = nullptr;
    gTried = false;
}

void Engine::setSharedDevice(Device device)
{
    std::lock_guard lock(gSharedMutex);
    if (device == gDevice)
        return;
    gDevice = device;
    delete gShared;
    gShared = nullptr;
    gTried = false;
}

Device Engine::sharedDevice()
{
    std::lock_guard lock(gSharedMutex);
    return gDevice;
}
} // namespace lila::motion
