#pragma once

#include "TailorCore.h"

#include <lila/Command.h>
#include <lila/Graph.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Transform.h>

#include <glm/mat4x4.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <string>

namespace lila::ext
{
class Context;
}

namespace tailor
{
/** The DG node holding a garment and producing its draped mesh. */
inline constexpr const char* kGarmentType = "tailorGarment";
/** A render mesh latched to a garment's cloth. */
inline constexpr const char* kBindingType = "tailorBinding";
/** Garments are solved in metres; the scene is in centimetres. */
inline constexpr double kCmPerM = 100.0;

/** The world matrix of the first mesh a garment feeds, which places its points. */
inline glm::dmat4 garmentWorld(lila::scene::Graph& g, lila::scene::NodeId garment)
{
    using namespace lila::scene;
    const Node* n = g.node(garment);
    if (!n)
        return glm::dmat4(1.0);
    for (const PlugRef& d : g.destinations({garment, n->type().attrIndex("outMesh")}))
        if (const Node* mesh = g.node(d.node))
            if (const PlugValue v = g.value({mesh->id(), Transform::kWorldMatrix}); const auto* m = std::get_if<glm::dmat4>(&v))
                return *m;
    return glm::dmat4(1.0);
}

/** A collider mesh in garment space (metres) as triangles. */
void colliderTriangles(const lila::geo::Mesh& m, const glm::dmat4& world, std::vector<Vec3>& points,
                       std::vector<std::array<int, 3>>& tris);

const char* panelStateName(PanelState s);
/** False for an unknown name. */
bool parsePanelState(const std::string& name, PanelState* out);

nlohmann::json garmentToJson(const Garment& g);
/** Throws std::exception on malformed input. Missing fields keep their defaults. */
Garment garmentFromJson(const nlohmann::json& j);

/** While one exists, garments drape inside their evaluation instead of on a worker (batch runs always do). */
struct DrapeNow
{
    DrapeNow();
    ~DrapeNow();
    DrapeNow(const DrapeNow&) = delete;
    DrapeNow& operator=(const DrapeNow&) = delete;
};
/** Cancels running drapes and waits for them; for unloading. */
void stopDrapes();

/**
 * A garment simulating continuously on its own thread, at most in real time, while its live attribute is on. The node
 * shows positions() every evaluation; the Grab Cloth tool pulls a vertex with grab().
 */
class LiveDrape
{
public:
    LiveDrape(lila::scene::NodeId garment, uint64_t key, std::shared_ptr<const ClothMesh> cloth, Garment g, Sdf collider,
              bool useGpu, SolverSettings settings, std::vector<Vec3> start);
    ~LiveDrape();
    LiveDrape(const LiveDrape&) = delete;
    LiveDrape& operator=(const LiveDrape&) = delete;

    /** The inputs it was started for; another key means start again. */
    uint64_t key() const { return mKey; }
    const ClothMesh& cloth() const { return *mCloth; }
    std::vector<Vec3> positions() const;
    /** Ends the thread; positions() keeps the last state. */
    void stop();
    /** Pins vertex to target (metres) until release(). */
    void grab(int vertex, const Vec3& target);
    void release();
    /** Steams the cloth around center (metres) on the next step: smooths it out and stills it. */
    void relax(const Vec3& center, float radius);

private:
    void run(lila::scene::NodeId garment, const Garment& g, const Sdf& collider, bool useGpu);

    uint64_t mKey;
    std::shared_ptr<const ClothMesh> mCloth;
    SolverSettings mSettings;
    mutable std::mutex mMutex;
    std::vector<Vec3> mLatest;
    std::optional<Pin> mGrab;
    std::optional<std::pair<Vec3, float>> mRelax;
    std::atomic<bool> mStop{false};
    std::atomic<bool> mRefreshQueued{false};
    std::shared_ptr<bool> mAlive = std::make_shared<bool>(true);
    std::thread mThread;
};

/** What a garment node drapes: its pattern with fabric and particle distance applied, meshed, and its solver settings. */
struct GarmentInputs
{
    Garment garment;
    ClothMesh cloth;
    SolverSettings settings;
    int64_t frames = 90;
    uint64_t key = 0;
    lila::scene::NodeId collider = lila::scene::kNoNode;
};
/** Throws std::invalid_argument when the garment can't be read. */
GarmentInputs garmentInputs(lila::scene::Graph& g, const lila::scene::Node& op);

/** The frame an animated garment last simulated to, positions in metres. */
struct AnimatedFrame
{
    int64_t frame = -1;
    std::vector<Vec3> positions;
};
/** Null until the simulation has a frame. Main thread. */
const AnimatedFrame* animatedFrame(lila::scene::NodeId garment);
void registerAnimation(lila::ext::Context& ctx);

/** Main thread. The garment node registers its live drape here, null to drop it. */
void registerLive(lila::scene::NodeId garment, const std::shared_ptr<LiveDrape>& live);
std::shared_ptr<LiveDrape> liveDrape(lila::scene::NodeId garment);
std::vector<lila::scene::NodeId> liveGarments();

bool registerNodes(lila::ext::Context& ctx);
void registerCommands(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
void registerPatternEditor(lila::ext::Context& ctx);
void registerToolkit(lila::ext::Context& ctx);
void registerGrabTool(lila::ext::Context& ctx);
} // namespace tailor
