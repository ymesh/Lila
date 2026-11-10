#pragma once

#include "core/Dds.h"
#include "core/Export.h"
#include "core/UninitVector.h"
#include "render/TextureFormat.h"

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace lila::render
{
/** Linear RGBA in half floats, as colour sets store it (geo/ColorSets.cpp). */
struct HalfColor
{
    uint64_t bits = 0x3c003c003c003c00ull; // white

    HalfColor() = default;
    HalfColor(const glm::vec4& c) : bits(glm::packHalf4x16(c)) {}
    operator glm::vec4() const { return glm::unpackHalf4x16(bits); }
};

/**
 * 64 bytes, so each vertex starts on a 32-byte sector and what the lean pipelines read (position, normal, first uv)
 * is one sector.
 */
struct MeshVertex
{
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec2 uv1;
    glm::vec4 tangent; // w: bitangent sign
    HalfColor color;   // from the mesh's current color set; white without one
};
static_assert(sizeof(MeshVertex) == 64, "rtbevel.slang reads MeshVertex by this stride");

/** A run of triangles sharing one material slot. */
struct MeshSection
{
    uint32_t material = 0; // slot index into MeshDraw::materials
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
};

/** Unique across the process, whichever library asks. */
LILA_FOUNDATION_API uint64_t nextResourceKey();

/**
 * Vertices edited in place while the topology stays (sculpting a dense mesh): the editor writes vertices and marks
 * them dirty under the mutex, and the renderer copies only the dirty ones to its GPU copy before the next frame.
 */
struct DynamicVertices
{
    std::mutex mutex;
    std::vector<MeshVertex> vertices;
    /** Vertex ids written since the renderer last looked; may repeat. */
    std::vector<uint32_t> dirty;
    /** Every vertex changed (or the renderer has not uploaded them yet). */
    bool allDirty = true;
    /** Something changed since the renderer last looked; read without the mutex, so unchanged meshes cost no lock. */
    std::atomic<bool> pending{true};

    void write(uint32_t v, const MeshVertex& value)
    {
        vertices[v] = value;
        if (!allDirty)
            dirty.push_back(v);
        pending.store(true, std::memory_order_release);
    }
    /** Every vertex changed. Under the mutex. */
    void markAllDirty()
    {
        allDirty = true;
        dirty.clear();
        pending.store(true, std::memory_order_release);
    }
};

/**
 * GPU-ready triangles, built on the main thread or a worker from a mesh and never changed afterwards. The renderer
 * uploads each one once and keeps it while frames still reference it; a mesh edit makes a new RenderMesh.
 */
struct RenderMesh
{
    /** When set, the vertices live (and change) here and `vertices` stays empty; indices and sections stay fixed. */
    std::shared_ptr<DynamicVertices> dynamic;
    /** When set, draws with this mesh's vertex buffer and `vertices` stays empty: a subset of its triangles (isolated faces). */
    std::shared_ptr<const RenderMesh> shareVertices;
    UninitVector<MeshVertex> vertices;
    std::vector<uint32_t> indices;     // triangle list, grouped by section
    std::vector<MeshSection> sections; // empty means one section of slot 0
    std::vector<glm::vec3> edgePoints; // line list for wireframe, original polygon edges only; with edgeIndices, the points they index
    /** Line list into edgePoints: each point stored once, so dense meshes draw their wire with far fewer vertices. */
    std::vector<uint32_t> edgeIndices;
    /**
     * Which sides of each triangle are polygon edges, 4 bits per triangle and 8 triangles per word: bit 0 is the side
     * from its first vertex to its second, bit 1 second to third, bit 2 third to first. Lets the mesh shader draw the
     * wire itself; empty draws it as lines.
     */
    std::vector<uint32_t> edgeMask;

    size_t edgeVertexCount() const { return edgeIndices.empty() ? edgePoints.size() : edgeIndices.size(); }
    const glm::vec3& edgePoint(size_t k) const { return edgeIndices.empty() ? edgePoints[k] : edgePoints[edgeIndices[k]]; }
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    uint64_t key = nextKey();

    static uint64_t nextKey() { return nextResourceKey(); }
};

class GpuImage;
struct TextureImage;
namespace vt
{
class Texture;
}

/** One tile of a UDIM set: udim 1001 covers UV 0..1, 1002 the next one along U, 1011 the next row up. */
struct UdimTile
{
    int udim = 1001;
    std::shared_ptr<const TextureImage> image;
};

/**
 * A material texture in one of four forms, never changed once built:
 * - texels (the default): row 0 at the top of the image. The renderer uploads them once (with a full mip chain) and
 *   keeps them while frames reference them; a changed file makes a new TextureImage.
 * - a UDIM set (tiles not empty): no texels of its own; the viewport picks the tile from floor(uv).
 * - a GPU image (gpu set): texels the renderer owns and other code updates in place (see GpuImage).
 * - a virtual texture's layer (vt set): pages streamed in as the viewport samples them (see VirtualTexture.h).
 */
struct TextureImage
{
    int width = 0;
    int height = 0;
    TextureFormat format = TextureFormat::RGBA8;
    bool srgb = true; // RGBA8 color data; false for linear data (normals, ORM). Float formats are always linear.
    /** width * height texels tightly packed in format (half floats for the 16F formats), or the blocks below. */
    std::vector<uint8_t> rgba;
    /**
     * Block-compressed texels (a DDS file's BC1 to BC5 or BC7) uploaded as they are: rgba then holds mipLevels levels,
     * largest first, one after another. format says what the blocks decode to (RGBA8, R8 for BC4, RG8 for BC5).
     */
    std::optional<dds::Format> blocks;
    int mipLevels = 1;
    /** Ascending by udim. */
    std::vector<UdimTile> tiles;
    std::shared_ptr<GpuImage> gpu;
    std::shared_ptr<vt::Texture> vt;
    int vtLayer = 0;
    uint64_t key = nextResourceKey();

    bool isUdimSet() const { return !tiles.empty(); }
};
using TexturePtr = std::shared_ptr<const TextureImage>;

enum MaterialFlags : uint32_t
{
    MatBaseColorMap = 1 << 0,
    MatNormalMap = 1 << 1,
    MatNormalGreenUp = 1 << 2, // OpenGL-style normal map
    MatMetallicMap = 1 << 3,   // ORM blue
    MatRoughnessMap = 1 << 4,  // ORM green
    MatOcclusionMap = 1 << 5,  // ORM red
    MatEmissiveMap = 1 << 6,
    MatMasked = 1 << 7,
    MatTranslucent = 1 << 8,
    MatTwoSided = 1 << 9,
    MatUv1 = 1 << 10,
};

enum class ShadingModel : uint32_t
{
    DefaultLit,
    Painterly,
    Toon,
    Foliage,
    Unlit,
    Skin,
    Cloth,
    Clearcoat,
    Hair,
    Eye,
    /** The base colour map is a lit sphere looked up by the view-space normal, unlit otherwise. Viewport only. */
    Matcap,
};

/** The stylised models' inputs (colours linear). Each model reads only its own group; see scene::material for meanings. */
struct MaterialStyle
{
    ShadingModel model = ShadingModel::DefaultLit;

    float painterlyStrength = 1.0f;
    float strokeNormal = 0.6f;
    float strokeScale = 20.0f; // cm per tile
    float strokeLayerRatio = 2.7f;
    uint32_t strokeMapping = 0; // object, world, screen, uv
    int paletteCount = 3;
    glm::vec3 paletteShadow{0.10f, 0.12f, 0.25f};
    glm::vec3 paletteLight{0.95f, 0.80f, 0.55f};
    glm::vec3 paletteAccent{0.60f, 0.30f, 0.35f};
    float paletteContrast = 0.5f;
    float paletteSpread = 0.35f;
    float specularTint = 0.5f;
    /** Stroke sheet offset for the current boil frame; zero while not boiling. */
    glm::vec2 boilOffset{0.0f};

    glm::vec3 toonShadowColor{0.35f, 0.38f, 0.55f};
    glm::vec3 toonLightTint{1.0f, 0.97f, 0.92f};
    float toonThreshold = 0.0f;
    float toonSoftness = 0.08f;
    float toonOuterOffset = 0.45f;
    glm::vec3 toonOuterColor{0.20f, 0.20f, 0.35f};
    glm::vec3 rimColor{1.0f};
    float rimWidth = 0.3f;
    float rimStrength = 0.4f;

    glm::vec3 foliageBottomColor{0.05f, 0.12f, 0.06f};
    glm::vec3 foliageTopColor{0.35f, 0.50f, 0.15f};
    float foliageGradient = 0.6f;
    float foliageBottomShadow = 0.5f;
    float foliageTopHighlight = 0.3f;
    float foliageTranslucency = 0.4f;
    float foliageWrap = 0.5f;

    glm::vec3 subsurfaceColor{0.9f, 0.25f, 0.15f};
    float subsurfaceAmount = 0.8f;
    glm::vec3 sheenColor{0.5f};
    float sheenRoughness = 0.5f;
    float clearcoat = 1.0f, clearcoatRoughness = 0.05f;
    float anisotropy = 0.8f, anisotropyRotation = 0.0f; // degrees
    float hairShift = 0.1f;
    float irisRadius = 0.15f, irisDepth = 0.3f, limbalRing = 0.5f;

    /** Painterly stroke sheet, linear: RG normal tilt, B stroke shade, A per-stroke random. Null draws without strokes. */
    TexturePtr strokeMap;
};

/** Words in one packed style block; mesh.slang reads it as eight float4s. */
constexpr int kStyleWords = 32;

/**
 * The style block a draw's material section reads from the frame's table. boundsMin/Max are the mesh's object-space
 * bounds (the foliage gradient runs between them), strokeHandle the stroke sheet's table handle (or slot), up the
 * object-space up axis (1 Y, 2 Z).
 */
LILA_UI_API std::array<uint32_t, kStyleWords> packStyle(const MaterialStyle& s, uint32_t strokeHandle, const glm::vec3& boundsMin,
                                                                const glm::vec3& boundsMax, bool zUp);

/** Unreal default lit inputs for one slot. A map flag set means that map replaces its constant. */
struct Material
{
    glm::vec3 baseColor{0.5f}; // linear
    float opacity = 1.0f;
    float metallic = 0.0f;
    float roughness = 0.55f;
    float specular = 0.5f;
    float opacityClip = 0.3333f;
    glm::vec3 emissive{0.0f}; // linear; scales the emissive map when there is one
    uint32_t flags = MatTwoSided;
    TexturePtr baseColorMap; // sRGB, alpha is opacity
    TexturePtr normalMap;    // linear
    TexturePtr ormMap;       // linear: R occlusion, G roughness, B metallic
    TexturePtr emissiveMap;  // sRGB
    /**
     * How each map samples, 8 bits per map (base color, normal, ORM, emissive from the low byte): wrap U in bits 0-1 and
     * wrap V in bits 2-3 (0 repeat, 1 clamp, 2 mirror, 3 black), bit 4 nearest filtering. 0 repeats and filters linearly.
     */
    uint32_t sampling = 0;
    MaterialStyle style;
    /** Live bevel radius, cm: edges round as they shade (ray traced); 0 is off. */
    float bevelRadius = 0.0f;
    /** Shows its own base colour even under the Grey Base Color display (the UV checker). */
    bool keepBaseColor = false;
};

/**
 * A draw's materials, one per slot, shared between copies of the draw: copying one is a reference, and a copy that is
 * changed gets its own list first. Reads like a std::vector<Material>.
 */
class MaterialList
{
public:
    using value_type = Material;
    using const_iterator = std::vector<Material>::const_iterator;
    using iterator = std::vector<Material>::iterator;

    MaterialList() = default;
    MaterialList(std::vector<Material> v) : mList(v.empty() ? nullptr : std::make_shared<std::vector<Material>>(std::move(v))) {}
    MaterialList& operator=(std::vector<Material> v)
    {
        mList = v.empty() ? nullptr : std::make_shared<std::vector<Material>>(std::move(v));
        return *this;
    }

    size_t size() const { return mList ? mList->size() : 0; }
    bool empty() const { return size() == 0; }
    const Material& operator[](size_t i) const { return (*mList)[i]; }
    const Material& front() const { return mList->front(); }
    const_iterator begin() const { return list().begin(); }
    const_iterator end() const { return list().end(); }
    const std::vector<Material>& list() const { return mList ? *mList : empty_(); }
    operator const std::vector<Material>&() const { return list(); }
    /** True when both are the same shared list (or both empty), so equal without comparing fields. */
    bool sameList(const MaterialList& o) const { return mList == o.mList; }

    Material& operator[](size_t i) { return own()[i]; }
    Material& front() { return own().front(); }
    iterator begin() { return own().begin(); }
    iterator end() { return own().end(); }
    void push_back(Material m) { own().push_back(std::move(m)); }
    void resize(size_t n) { own().resize(n); }
    void assign(size_t n, const Material& m) { own().assign(n, m); }
    void clear() { mList.reset(); }

private:
    static const std::vector<Material>& empty_()
    {
        static const std::vector<Material> e;
        return e;
    }
    std::vector<Material>& own()
    {
        if (!mList)
            mList = std::make_shared<std::vector<Material>>();
        else if (mList.use_count() > 1)
            mList = std::make_shared<std::vector<Material>>(*mList);
        return *mList;
    }
    std::shared_ptr<std::vector<Material>> mList;
};

enum MeshDrawFlags : uint32_t
{
    DrawShaded = 1 << 0,
    DrawWire = 1 << 1,
    DrawSelected = 1 << 2,
    DrawUnderSelected = 1 << 3,
    DrawPreselect = 1 << 4, // under the cursor: a click would select it
    DrawWireDepth = 1 << 5, // wire without shading still hides behind other surfaces (templated objects)
    DrawSelectedOther = 1 << 6, // with DrawSelected: selected, but not the last selected
};

/** How a draw shows its vertex colors: multiplied into the lit base color, or flat and unlit. */
enum class VertexColorMode : uint8_t
{
    Off,
    Shaded,
    Unlit,
};

/** Every RGB channel, or one channel (alpha included) as grayscale, the way engines use them as masks. */
enum class ColorChannel : uint8_t
{
    Rgb,
    R,
    G,
    B,
    A,
};

struct MeshDraw
{
    std::shared_ptr<const RenderMesh> mesh;
    glm::mat4 world{1.0f};
    /**
     * Placements relative to world: when set and not empty the mesh draws once per entry, at world * entry, as one
     * instanced draw. Shared so frames can reuse one list.
     */
    std::shared_ptr<const std::vector<glm::mat4>> instances;
    /** Used for every section whose slot has no entry in materials. */
    glm::vec3 baseColor{0.5f};
    float roughness = 0.55f;
    MaterialList materials; // per slot
    uint32_t flags = DrawShaded;
    VertexColorMode vertexColors = VertexColorMode::Off;
    ColorChannel colorChannel = ColorChannel::Rgb;
    /** Linear wire color when not selected or preselected; alpha 0 keeps the default near black. */
    glm::vec4 wireColor{0.0f};
    float wireWidth = 1.0f; // device pixels
    /** The scene node it draws (scene::NodeId), 0 for none: renders key object mattes by it. */
    uint64_t node = 0;
    /** Lighting channel bits: lights whose bits share none with these leave it unlit. */
    uint32_t lightingChannels = 1;
};

/**
 * The frame's mesh draws, shared copy-on-write: a frame that draws what the last one did hands the same list over
 * without copying it, and the renderer tells it is the same list without comparing it. Reading never copies; edit()
 * and the appending calls copy first while another frame still holds the list.
 */
class DrawList
{
public:
    using value_type = MeshDraw;
    using const_iterator = std::vector<MeshDraw>::const_iterator;

    DrawList() = default;
    DrawList(std::vector<MeshDraw> v) : mList(v.empty() ? nullptr : std::make_shared<std::vector<MeshDraw>>(std::move(v))) {}
    DrawList(std::initializer_list<MeshDraw> l) : DrawList(std::vector<MeshDraw>(l)) {}

    size_t size() const { return mList ? mList->size() : 0; }
    bool empty() const { return size() == 0; }
    const MeshDraw& operator[](size_t i) const { return (*mList)[i]; }
    const MeshDraw& front() const { return mList->front(); }
    const MeshDraw& back() const { return mList->back(); }
    const_iterator begin() const { return list().begin(); }
    const_iterator end() const { return list().end(); }
    const std::vector<MeshDraw>& list() const { return mList ? *mList : empty_(); }
    const MeshDraw* data() const { return list().data(); }
    /** Both hold the same shared list (or both none), so they are equal without comparing a draw. */
    bool sameList(const DrawList& o) const { return mList == o.mList; }

    /** The list to change, copied first when shared. */
    std::vector<MeshDraw>& edit()
    {
        if (!mList)
            mList = std::make_shared<std::vector<MeshDraw>>();
        else if (mList.use_count() > 1)
            mList = std::make_shared<std::vector<MeshDraw>>(*mList);
        return *mList;
    }
    MeshDraw& push_back(MeshDraw d) { return edit().emplace_back(std::move(d)); }
    template <typename... Args>
    MeshDraw& emplace_back(Args&&... args)
    {
        return edit().emplace_back(std::forward<Args>(args)...);
    }
    void reserve(size_t n) { edit().reserve(n); }
    void clear() { mList.reset(); }

private:
    static const std::vector<MeshDraw>& empty_()
    {
        static const std::vector<MeshDraw> e;
        return e;
    }
    std::shared_ptr<std::vector<MeshDraw>> mList;
};
} // namespace lila::render
