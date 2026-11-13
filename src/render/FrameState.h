#pragma once

#include "render/RenderMesh.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lila::render
{
/** FrameState::frameCap: draw at the display's refresh rate. */
inline constexpr int kFrameCapRefresh = -1;

/**
 * A world-space (or, in FrameState::overlay, NDC) vertex. World positions are double: the renderer rebases them on the
 * eye before they become float, so lines stay exact far from the origin.
 */
struct LineVertex
{
    glm::dvec3 pos;
    glm::vec4 color;
};

/** World-space lines at one width in device pixels: depth tested like linesOnTop, or over every surface. */
struct LineBatch
{
    std::vector<LineVertex> vertices; // pairs
    float width = 1.0f;
    bool onTop = false;
};

/**
 * Everything a viewport needs for one frame, copied out of the scene on the main thread. The render thread only
 * ever sees these snapshots, never the live graph. An offline renderer would consume the same data.
 */
/** One component dot: a point relative to its cloud's origin, and how it shows. */
struct PointVertex
{
    glm::vec3 pos;
    uint32_t state = 0; // PointDraw::Normal, Selected, Mirror or Hidden
};

/** Component dots built once per mesh, selection and view state, so the renderer can keep them on the GPU. */
struct PointCloud
{
    std::vector<PointVertex> points;
    /** World position the points are relative to, so they keep their precision far from the world origin. */
    glm::dvec3 origin{0.0};
    uint64_t key = nextResourceKey();
};

/** Dots drawn as squares of a constant pixel size, depth tested and pulled toward the camera like lines. */
struct PointDraw
{
    enum State : uint32_t
    {
        Normal,
        Selected,
        Mirror, // the mirror image of a selected point
        Hidden,
    };
    std::shared_ptr<const PointCloud> cloud;
    glm::vec4 colors[3]{}; // linear, by state
    float size = 4.0f;     // device pixels
    float selectedSize = 6.0f;
};

/**
 * How hair strands are laid out, kept on the GPU while the strands only move: per segment its first point and its strand,
 * per strand its first point, point count, thinning rank (float bits: the strand's place in its clump's thinning order, 0
 * to 1, drawn while below the draw's keep) and sRGB colour (R in the low byte).
 */
struct StrandLayout
{
    std::vector<glm::uvec2> segments;
    std::vector<glm::uvec4> strands;
    size_t points = 0;
    uint64_t key = nextResourceKey();
};

/** Hair strands: their points (xyz, world width) on a layout. A new cloud on the same layout uploads only the points. */
struct StrandCloud
{
    std::vector<glm::vec4> points;
    std::shared_ptr<const StrandLayout> layout;
    uint64_t key = nextResourceKey();
};

/**
 * Strands drawn as camera-facing ribbons with hair shading. Thinner than a pixel they draw a pixel wide and dither to
 * their coverage; at a distance only the strands ranked under `keep` draw, widened by `widthScale` to cover the same.
 */
struct StrandDraw
{
    std::shared_ptr<const StrandCloud> cloud;
    glm::dmat4 world{1.0};
    float keep = 1.0f;
    float widthScale = 1.0f;
    /** Highlight colour (linear) mixed in for a selected or preselected node; black for none. */
    glm::vec3 tint{0.0f};
};

/**
 * One octree node of a streamed point cloud: its points packed as three words each (x | y << 16, z | r << 16 | g << 24,
 * b | class << 8 | intensity << 16), positions 0 to 65535 across the node's cube. Kept on the GPU by key.
 */
struct PointChunk
{
    std::vector<uint32_t> words;
    uint32_t count = 0;
    uint64_t key = nextResourceKey();
};

/**
 * A point cloud's visible nodes, drawn as points of a constant pixel size in the scene pass. Each chunk sits in its cube
 * (min, size) in the cloud's space, placed by world. Clip boxes are world-to-box matrices, inside where |x|, |y|, |z| <= 0.5.
 */
struct PointCloudDraw
{
    struct Chunk
    {
        std::shared_ptr<const PointChunk> points;
        glm::dvec3 min{0.0};
        double size = 1.0;
    };
    std::vector<Chunk> chunks;
    glm::dmat4 world{1.0};
    float pointSize = 2.0f; // device pixels
    float edl = 0.3f;       // eye-dome lighting strength, 0 off
    enum ColorMode : uint32_t
    {
        Rgb,
        Intensity,
        Elevation,
        Classification,
        Single,
    };
    ColorMode colorMode = Rgb;
    glm::vec3 color{0.8f}; // linear, Single
    /** Elevation: the cloud-space up axis (1 Y, 2 Z) and the height range the ramp spans. */
    int upAxis = 1;
    glm::vec2 elevation{0.0f, 1.0f};
    enum ClipMode : uint32_t
    {
        ClipOff,
        ClipInside,
        ClipOutside,
    };
    ClipMode clipMode = ClipOff;
    std::vector<glm::dmat4> clips; // at most 8
    /** Highlight colour (linear) mixed in for a selected or preselected cloud; black for none. */
    glm::vec3 tint{0.0f};
    uint64_t node = 0;
};

/** Pixel-space triangle vertex for the 2D overlay (gizmos, HUD, text). */
struct OverlayVertex
{
    glm::vec2 pos; // pixels, origin top left
    glm::vec2 uv;  // atlas coordinates; u < 0 means a solid fill
    glm::vec4 color;
};

/** Single-channel coverage texture for overlay text. Replace the pointer to upload a new one. */
struct OverlayAtlas
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

/**
 * A reference image on a world-space quad (an image plane). It is drawn in display space: no lighting, exposure, gamma,
 * tonemapping or AO, so the picture's colours stay as authored.
 */
struct ImagePlaneDraw
{
    enum class Depth
    {
        Behind, // under every mesh, no depth test
        Scene,  // depth tested like geometry
        Front,  // over every mesh
    };
    glm::dvec3 corners[4]{};              // top left, top right, bottom right, bottom left
    /** corners are view NDC (x, y; z unused) rather than world points: glued to the screen, untouched by the camera. */
    bool screen = false;
    glm::vec4 uv{0.0f, 0.0f, 1.0f, 1.0f}; // u0, v0, u1, v1; v0 is the image's top row
    /** Each corner's own image coordinates instead of uv's rectangle: a piece of a cut plane, drawn a triangle at a time. */
    bool cornerUvs = false;
    glm::vec2 cornerUv[4]{};
    /** rgb: a background color to clear (display-encoded); w: how far from it a texel still clears. 0 keys nothing. */
    glm::vec4 key{0.0f};
    TexturePtr image;                     // sRGB
    float opacity = 1.0f;
    float brightness = 0.0f; // added in display space
    float contrast = 1.0f;   // around mid gray, in display space
    bool grayscale = false;
    /** Only the image's edges, in outlineColor (display-encoded): a tracing sheet that leaves what is under it visible. */
    bool outline = false;
    glm::vec3 outlineColor{0.25f, 0.85f, 1.0f};
    Depth depth = Depth::Behind;
};

/** Values on a heightfield's grid (a named mask), one per sample, kept on the GPU while the same pointer comes back. */
using GridValues = std::shared_ptr<const std::vector<float>>;

/**
 * A paint layer of a heightfield: where its weights (0 to 1) lie, its colour, roughness and maps replace the material's.
 * The maps repeat every tiling cm across the terrain, seen from above.
 */
struct HeightfieldLayer
{
    GridValues weights;
    glm::vec3 color{0.5f}; // linear; multiplies the base colour map
    float roughness = 0.8f;
    TexturePtr baseColorMap; // sRGB
    TexturePtr normalMap;    // linear, DirectX green
    float tiling = 400.0f;
};

/**
 * A heightfield drawn from its heights alone (the viewport's heightfield draw path, an option): nx by nz samples cell
 * apart from origin over X and Z, heights up local Y, placed by world. The renderer keeps one GPU copy per heights
 * buffer, so pass the same pointer while they don't change. Only the visible detail is drawn: the grid's level of
 * detail falls with distance from the camera.
 */
struct HeightfieldDraw
{
    std::shared_ptr<const std::vector<float>> heights;
    int nx = 0;
    int nz = 0;
    float cell = 100.0f;
    glm::vec2 origin{0.0f};
    glm::dmat4 world{1.0};
    /** Used when materials is empty. */
    glm::vec3 color{0.5f};
    float roughness = 0.55f;
    /** The terrain's material (slot 0 shades it); uv 0 to 1 over the grid, as its mesh has. */
    MaterialList materials;
    /** Painted over the material in order, by their weights. */
    std::vector<HeightfieldLayer> layers;
    /** MeshDrawFlags: shaded, wire, and the selection highlights, which draw as a wire over the surface. */
    uint32_t flags = DrawShaded;
    glm::vec4 wireColor{0.0f}; // linear; alpha 0 keeps the default
    float wireWidth = 1.0f;    // device pixels
    uint32_t lightingChannels = 1;
    /** A mask shown over the surface in a colour ramp (0 grey, 1 turbo), its range stretched to fill the ramp. */
    GridValues overlay;
    int overlayRamp = 1;
    float overlayOpacity = 0.65f;
    /** Height contour lines every contourInterval cm (0 none), every contourMajor-th bolder. */
    float contourInterval = 0.0f;
    int contourMajor = 5;
    /** Arrows down the slope every arrowSpacing device pixels or so; weighted by flow (a 0 to 1 mask) when set. */
    bool flowArrows = false;
    GridValues flow;
    float arrowSpacing = 48.0f;
    /** How many device pixels the finest quads near the camera span: fewer draws more detail and costs more. */
    float quadPixels = 4.0f;
    /** The node drawn: its new heights at the same size upload only the tiles that changed. 0 uploads them whole. */
    uint64_t node = 0;
};

/** A 3D colour lookup table (an Adobe .cube): display-encoded rgb in, rgb out, red varying fastest. */
struct Lut3D
{
    int size = 0;
    std::vector<float> rgb; // size^3 entries of 3 floats
    uint64_t key = nextResourceKey();
};

/** How to turn the scene's light into the picture: tonemapper, grading, bloom and lens effects (scene::postProcess). */
struct PostSettings
{
    enum Tonemapper
    {
        Neutral, // Khronos PBR Neutral: base colours stay true, the default
        AgX,
        Aces,
        Linear, // clipped
    };
    Tonemapper tonemapper = Neutral;
    int agxLook = 0; // 0 none, 1 punchy, 2 golden
    /** Grading, bloom and lens effects below apply only when on; the tonemapper and auto exposure always do. */
    bool effects = false;
    bool autoExposure = false;
    float exposureCompensation = 0.0f;       // stops on top of the auto exposure
    float exposureMin = -6.0f, exposureMax = 6.0f; // stops the auto exposure may reach
    float exposureSpeed = 3.0f;              // per second; 0 jumps
    float contrast = 1.0f, saturation = 1.0f;
    glm::vec3 whiteBalance{1.0f};            // per channel gains (from temperature and tint)
    glm::vec3 lift{0.0f}, gamma{1.0f}, gain{1.0f};
    float bloom = 0.0f;                      // blend toward the bloom, 0..1
    float bloomRadius = 1.0f;                // tent filter radius in texels per level
    float vignette = 0.0f, grain = 0.0f;
    float chromaticAberration = 0.0f;        // pixels of colour split at the corners
    float distortion = 0.0f;                 // negative barrel, positive pincushion
    float sharpen = 0.0f;
    std::shared_ptr<const Lut3D> lut;
    float lutIntensity = 1.0f;
    /** Off for passes whose values must come out exact, such as object mattes. */
    bool dither = true;
};

/** A light the viewport shades with: the scene's lights its own provider handles, in world space. */
struct LightDraw
{
    enum Kind
    {
        Point,
        Spot,
        Directional,
        Area, // width by height, facing along direction
        Disc, // width is the diameter
    };
    Kind kind = Point;
    glm::dvec3 position{0.0};
    glm::vec3 direction{0.0f, -1.0f, 0.0f}; // where the light travels
    glm::vec3 right{1.0f, 0.0f, 0.0f};      // area lights: the width axis
    glm::vec3 radiance{1.0f};               // colour times intensity, in the shader's units (1 lights like the default environment)
    float range = 0.0f;                     // cm; 0 has no cutoff
    float cosInner = 1.0f, cosOuter = 0.0f; // spot cone
    float width = 0.0f, height = 0.0f;      // cm
    float radius = 0.0f;                    // cm, softens highlights and shadows
    /** The light casts shadows: traced on GPUs with ray queries (any kind), else a shadow map (directional and spot). */
    bool castsShadows = false;
    /** World to the shadow map's clip space (reversed-Z), when it has one. */
    bool shadows = false;
    glm::dmat4 shadowViewProj{1.0};
    float shadowTexel = 1.0f; // world size of a shadow texel at the light's focus, cm, for the normal offset
    /**
     * For contact-hardening shadow maps: the map's near and far distances (cm), the light's size (the sun's angular radius
     * as a tangent, a spot's source radius in cm), and shadow uv per cm across (per cm at 1 cm away for a spot).
     */
    glm::vec4 shadowDepth{0.0f};
    /** Lighting channel bits (at most 20): it lights only meshes that share one. */
    uint32_t lightingChannels = 1;
};

/**
 * World-space data comes in double. The renderer draws camera-relative: rebaseOnEye (render/CameraRelative.h) moves the
 * frame so the eye sits at its float space's origin, and renderOrigin records where that is in the world.
 */
struct FrameState
{
    glm::dmat4 view{1.0};
    glm::dmat4 proj{1.0}; // reversed-Z, infinite far
    glm::dvec3 eye{0.0};
    /** The world position the frame's float space is centred on: 0 as built, the eye once rebased. */
    glm::dvec3 renderOrigin{0.0};
    /** Rebase on the eye when it is far from the origin (render::kRebaseBeyond); off draws in world space everywhere. */
    bool cameraRelative = true;
    bool zUp = false;
    float gridSpacing = 10.0f; // cm between minor lines
    int gridMajorEvery = 10;
    glm::vec3 background{0.24f, 0.25f, 0.27f};
    /** Grid line colour and the two ground axes (X, then Z or Y when Z-up), in the grid shader's linear values. */
    glm::vec3 gridColor{0.42f};
    glm::vec3 gridAxisX{0.0f};
    glm::vec3 gridAxisZ{0.0f};
    /** Where the grid and its axes put zero: the scene's display origin, world cm. */
    glm::dvec3 gridOrigin{0.0};
    /** Linear object wire colours: selected, selected secondary (see selectionOrderColors), preselected, under a selected parent, and the default. */
    glm::vec4 wireSelected{0.92f, 0.92f, 0.92f, 1.0f};
    glm::vec4 wireSelectedOther{0.92f, 0.92f, 0.92f, 1.0f};
    glm::vec4 wirePreselect{0.75f, 0.75f, 0.75f, 1.0f};
    glm::vec4 wireUnderSelected{0.55f, 0.55f, 0.55f, 1.0f};
    glm::vec4 wireDefault{0.02f, 0.02f, 0.02f, 1.0f};
    DrawList meshes;
    float exposure = 1.0f; // linear multiplier
    float gamma = 1.0f;    // display gamma on top of the tonemap, like Maya's viewport gamma
    /** Requested MSAA count (1, 2, 4, 8); the renderer clamps it to the device and rebuilds its targets on change. */
    int msaaSamples = 8;
    /** Temporal antialiasing: the scene jitters per frame and blends into a history, which smooths shading and texture shimmer MSAA can't. */
    bool taa = true;
    /** Jittered frames a still view keeps drawing and averaging after it changes, for a supersampled result; 1 stops at once. */
    int accumulate = 16;
    /** Most frames a second the view draws: kFrameCapRefresh for the display's refresh rate, 0 for no limit. */
    int frameCap = kFrameCapRefresh;
    /** Presents in FIFO, waiting for each vblank; otherwise mailbox where the surface offers it. */
    bool vsync = false;
    PostSettings post;
    /**
     * Depth of field: the camera's aperture radius (cm; 0 for none) and focus distance along the view. With temporal
     * antialiasing on, each frame looks through another point of the aperture, so a still view or a render converges on
     * true lens blur. blades 0 is a round aperture.
     */
    float lensRadius = 0.0f;
    float focusDistance = 500.0f;
    int apertureBlades = 0;
    float apertureRotation = 0.0f; // degrees
    std::vector<LightDraw> lights;
    /** Up to four shadowed lights trace their shadows against the scene (GPUs with ray queries) instead of shadow maps. */
    bool rtShadows = false;
    /** Exponential height fog, lit by the first directional light as well as its own colour. Lengths cm. */
    struct Fog
    {
        bool on = false;
        glm::vec3 color{0.6f, 0.7f, 0.8f}; // linear, scaled by the environment intensity like ambient light
        float density = 0.0002f;           // per cm at the fog height
        float height = 0.0f;               // where the density is as given
        float falloff = 0.002f;            // per cm upward: density halves every ln2 / falloff
        float start = 0.0f;                // no fog nearer the camera than this
        float sunScatter = 0.5f;           // how much the sun lights the fog
        float anisotropy = 0.6f;           // forward scattering toward the sun, 0 to 0.95
    } fog;
    /** No backdrop: what no surface covers stays clear (alpha 0), for renders composited later. */
    bool transparentBackground = false;
    /**
     * Frames drawn after this one average into it as they are, with no reprojection: the subframes of a render, whose
     * cameras and objects may move between them for motion blur.
     */
    bool accumulateOnly = false;
    bool grid = true;
    bool backfaceCulling = false; // culls back faces of every material; off, every material shows them
    /** Back faces of two-sided materials shade with their normal flipped toward the camera; off, they light from behind. */
    bool twoSidedLighting = true;
    /** Display-encoded rgb mixed over back faces by alpha, to spot reversed normals; alpha 0 is off. */
    glm::vec4 backfaceTint{0.0f};
    bool xray = false;            // shaded meshes see-through
    /** Materials shade with a 50% grey base colour; their maps' alpha, normals, ORM, emission and shading models still apply. */
    bool greyBaseColor = false;
    /**
     * Shaded lighting: the environment (falls back to the studio rig while none is loaded), the studio rig, flat, or the
     * sculpt rig (one grazing key and little ambient, so surface form reads).
     */
    enum class Lighting
    {
        Environment,
        Studio,
        Flat,
        Sculpt
    };
    Lighting lighting = Lighting::Environment;
    /** Degrees the camera-relative studio and sculpt rigs turn around the view axis; 0 keys from above left. */
    float rigRotation = 0.0f;
    /** Degrees the sculpt rig's key sits off the view direction: 0 lights from the camera, 90 grazes. */
    float rigTilt = 45.0f;

    // Image-based lighting. An empty path is the built-in neutral studio; changing it loads the new HDRI off-thread.
    std::string envPath;
    float envRotation = 0.0f; // degrees around the up axis
    float envIntensity = 1.0f;
    bool envBackground = true;
    float envBlur = 0.6f;       // 0 sharp, 1 fully diffuse
    float envSaturation = 0.0f; // background only; 0 is gray
    /** Colour of the environment's light on surfaces: 0 lights with its brightness alone (gray), 1 with the HDRI's own tint. */
    float envLightSaturation = 1.0f;
    float envBackgroundExposure = 0.6f; // dims the backdrop only, so assets stand out
    float groundShadow = 0.5f;          // soft contact blob under meshes resting on the ground; 0 is off
    bool ambientOcclusion = true;       // screen-space, on indirect light only
    /** AO from the main pass's depth, darkening the whole finished mesh (Maya's way): cheaper, as it skips the prepass. */
    bool aoFast = false;
    /** AO traced against the scene (on GPUs with ray queries) instead of from the screen: no halos, and occluders off screen count. */
    bool aoRayTraced = false;
    float aoRadius = 30.0f;             // world units (cm)
    float aoIntensity = 1.0f;
    // Preview-only post over the shaded scene (not lines or overlays): a painterly anisotropic Kuwahara filter and outlines.
    bool kuwahara = false;
    float kuwaharaRadius = 4.0f; // device pixels
    bool outline = false;
    float outlineWidth = 1.0f;  // device pixels
    glm::vec3 outlineColor{0.01f}; // linear
    std::vector<ImagePlaneDraw> imagePlanes;
    /** Terrains on the heightfield draw path, shaded in the scene pass beside the meshes. */
    std::vector<HeightfieldDraw> heightfields;
    /**
     * An external viewport renderer draws the scene instead of Lila: its image replaces the backdrop, grid fill, AO,
     * post and shaded meshes, and meshes keep only their wires and selection highlights, depth tested against its depth.
     * On with no colour yet (the first frame has not arrived), the view shows the plain background.
     */
    bool externalScene = false;
    /** Straight alpha over the background, row 0 at the top: RGBA8 sRGB or RGBA16F linear. Stretched to the view. */
    TexturePtr externalColor;
    /** R32F distance along the view axis in cm, 0 or less where nothing was hit; null when the renderer gave none. */
    TexturePtr externalDepth;
    /** externalColor and externalDepth store row 0 at the bottom. */
    bool externalBottomUp = false;
    /** externalColor is premultiplied; the draw divides alpha out. */
    bool externalPremultiplied = false;
    /** Multiplies externalColor's rgb. */
    float externalExposure = 1.0f;
    /**
     * When set, externalDepth holds OpenGL window depth (1 or zero alpha: nothing hit), turned into view distance by
     * z = (x * ndc + y) / (z + w * (1 - ndc)): see viewportRenderers' Frame::glDepthProj.
     */
    std::optional<glm::vec4> externalDepthUnproject;
    std::vector<LineVertex> lines;
    std::vector<LineVertex> linesOnTop; // like lines, biased toward the camera and writing no depth (edges over faces)
    /** Selected component edges: drawn like linesOnTop, at their own width in device pixels. */
    std::vector<LineVertex> selectedEdges;
    float selectedEdgeWidth = 1.0f;
    /** UV seams over meshes: drawn like linesOnTop, at their own width in device pixels. */
    std::vector<LineVertex> seamLines;
    float seamLineWidth = 1.0f;
    /** Mesh edges on the symmetry plane: drawn like linesOnTop, under the selected edges and hover, at their own width in device pixels. */
    std::vector<LineVertex> symmetryLines;
    float symmetryLineWidth = 1.0f;
    /** Highlighted mesh edges (hard, border, non-manifold...): drawn like linesOnTop, under the symmetry and selected edges, at their own width in device pixels. */
    std::vector<LineVertex> highlightEdges;
    float highlightEdgeWidth = 1.0f;
    /**
     * Curve shapes, drawn like linesOnTop at their own widths in device pixels: each width's dark halo under its lines,
     * plain curves first, then the bold (highlighted) ones over them.
     */
    std::vector<LineVertex> curveHalo, curveLines, curveHaloBold, curveLinesBold;
    float curveWidth = 2.0f, curveBoldWidth = 3.0f, curveHaloWidth = 4.0f, curveHaloBoldWidth = 5.0f;
    std::vector<LineVertex> tris3d;     // world-space triangle list, blended and biased toward the camera, no depth write
    std::vector<PointDraw> points;      // component dots, drawn after tris3d
    std::vector<StrandDraw> strands;    // hair, drawn in the scene pass after the meshes
    std::vector<PointCloudDraw> pointClouds; // scans, drawn in the scene pass after the strands
    std::vector<LineVertex> linesXray;  // world-space lines with no depth test, over every surface (x-ray joints)
    /** Debug drawing (debug/DebugDraw.h), shared between frames while it doesn't change. Triangles are blended and depth tested. */
    std::shared_ptr<const std::vector<LineBatch>> debugLines;
    std::shared_ptr<const std::vector<LineVertex>> debugTris;
    std::vector<LineVertex> overlay; // NDC positions, drawn last with no depth test (marquee, HUD)
    std::vector<OverlayVertex> overlayTris; // drawn after everything else
    std::shared_ptr<const OverlayAtlas> atlas;
};
} // namespace lila::render
