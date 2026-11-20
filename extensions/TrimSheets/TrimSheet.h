#pragma once

#include <lila/ImageIO.h>
#include <lila/Mesh.h>
#include <lila/Uv.h>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/** Trim sheets: the .lilatrim definition the generator, the bake, the material and the hotspot matcher all read. */
namespace trims
{
using json = nlohmann::json;
namespace geo = lila::geo;
namespace uv = lila::uv;

/** File extension of a trim sheet definition. */
inline constexpr const char* kExtension = ".lilatrim";
/** Face int attribute holding the trim index + 1 a face was mapped to (0 none). */
inline constexpr const char* kFaceTrim = uv::kTrimFaceAttr;
/** Mesh node string attribute naming the .lilatrim a mesh is mapped with. */
inline constexpr const char* kNodeSheet = "trimSheet";
/** Mesh node string attribute naming the decal .lilatrim a placed decal quad shows, and its float projection depth, cm. */
inline constexpr const char* kNodeDecal = "trimDecal";
inline constexpr const char* kNodeDecalDepth = "decalDepth";
/** Mesh node string attribute naming the sheet a mesh decal's faces map (edge decal strips): sent as a static mesh on M_LilaDecal. */
inline constexpr const char* kNodeMeshDecal = "trimDecalMesh";

/** What the generator sweeps across a trim. */
enum class Style : uint8_t
{
    Flat,
    Raised,      // plate with a bevel on its borders
    Recessed,    // sunk plate with a bevel down into it
    DoubleBevel, // two raised steps
    Step,        // steep single step
    HalfPipe,    // rounded across the whole trim
    Groove,      // raised plate with a V groove along its centre
    Ribs,        // corrugation across the trim
    Rivets,      // raised plate with a rivet row
    Bolts,       // raised plate with hex bolts at the ends or along it
    Vents,       // raised plate with louvre slats
    Tread,       // raised plate with diamond tread bars
    Planks,      // planks of varied length with bevelled seams
    Channel,     // raised plate with a rounded channel carved along its centre
    Torus,       // big convex round between two fillets
    Cavetto,     // concave quarter round rising to the top
    Ogee,        // S-curve moulding: concave below, convex above
    Fluting,     // concave flutes across the strip, repeated along it
    Dentils,     // row of square blocks along the strip
    Meander,     // raised Greek key border
    Glyphs,      // carved band of inscription-like glyphs (seeded strokes, not text)
    Steps,       // stepped transition rising toward the top
    Chipped,     // weathered block: worn, chipped borders and pitting
    Grating,     // bar grating: bars across over open holes
    Rail,        // round rail tube along the strip
    Conduit,     // three conduits with clips across them
    Hazard,      // diagonal hazard stripes, slightly raised
};

std::string_view styleName(Style s);
std::optional<Style> styleFromName(std::string_view name);
std::vector<std::string> styleNames();

struct Profile
{
    Style style = Style::Raised;
    /** Bevel width on the sheet, px: the same on every trim keeps the bevel's world width constant. */
    double bevelPx = 8.0;
    /** Bevel angle from the sheet plane, degrees. */
    double angle = 45.0;
    /** Rounding segments per bevel (1 is a plain chamfer). */
    int segments = 1;
    /** Detail spacing for rivets, bolts, ribs, slats and planks, px. */
    double spacingPx = 64.0;
    /** Detail size (rivet diameter, rib depth, slat width), px. */
    double detailPx = 10.0;
    uint32_t seed = 0;

    json toJson() const;
    static Profile fromJson(const json& j);
};

struct Trim
{
    std::string name;
    /** Pixel rectangle, top-left origin like the texture (and Valve's .rect): min inclusive, max exclusive. */
    glm::ivec2 min{0};
    glm::ivec2 max{0};
    uv::TrimKind kind = uv::TrimKind::StripU;
    uv::TrimFit fit = uv::TrimFit::Seamless;
    /** Gap kept inside the trim when islands are placed, px. */
    double insetPx = 2.0;
    std::vector<std::string> tags;
    int altGroup = -1;
    bool allowRotate = true;
    /** Mirrored islands read right on this trim (a symmetric profile with no lettering or lighting baked one way). */
    bool allowMirror = false;
    bool reserved = false;
    /** World size override, cm; zero derives it from the sheet's density. */
    glm::dvec2 worldSize{0.0};
    Profile profile;

    glm::ivec2 sizePx() const { return max - min; }
    json toJson() const;
    static Trim fromJson(const json& j);
};

/**
 * One material family over the sheet's layout: the baked maps are shared, the paint is its own. Each palette has a Lila
 * material M_<sheet>_<name>, a texture set TS_<sheet>_<name> and in Unreal an instance MI_<sheet>_<name> of the trim master.
 * The sheet itself is the default palette (M_<sheet>, TS_<sheet>, MI_<sheet>).
 */
struct Palette
{
    std::string name;
    /** Smart material its texture set starts from; the sheet's when empty. */
    std::string material;
    /** Unreal instance parameters by MF_LilaTrim name: scalars {name: value}, vectors {name: [r, g, b]}, switches {name: bool}. */
    json scalars = json::object();
    json vectors = json::object();
    json switches = json::object();

    json toJson() const;
    static Palette fromJson(const json& j);
};

struct Sheet
{
    std::string name = "TrimSheet";
    int width = 2048;
    int height = 2048;
    /** Texels per metre: sets every trim's world size. */
    double pxPerM = 512.0;
    /** Texels dilated past each trim when baking, px. */
    int padding = 8;
    /** Trim borders on multiples of 2^mipLevel px stay clean down to that mip. */
    int mipLevel = 3;
    /** The preset this came from, empty when hand made. */
    std::string preset;
    /** Smart material or Lila material applied to the sheet's texture set. */
    std::string material;
    /** Maps the bake writes. */
    std::vector<std::string> maps = {"normal", "ao", "curvature", "height", "cavity", "detailNormal"};
    /** Bake folder, absolute or relative to the .lilatrim; empty is <file stem>_Textures beside it. */
    std::string folder;
    int supersampling = 16;
    /**
     * Scene meshes baked as high poly detail, in sheet space as trimSheetGenerate lays the sheet out (x right, z down the
     * texture, y up off it): motifs, real geometry, a sculpt. Added to the generated relief, or instead of it with highReplaces.
     */
    std::vector<std::string> high;
    bool highReplaces = false;
    /**
     * A unique decal rather than a sheet: one trim over the whole texture, baked flat around its relief, plus an opacity mask
     * (from where the relief or high meshes stand off the plane) and a base colour, for deferred decals.
     */
    bool decal = false;
    std::vector<Trim> trims;
    std::vector<Palette> palettes;
    /** Unreal parameters of the default palette, as Palette's. */
    json scalars = json::object();
    json vectors = json::object();
    json switches = json::object();

    /** The palette named name, null when none; the empty name is the default palette and also gives null. */
    const Palette* palette(std::string_view name) const;
    Palette* palette(std::string_view name);

    /** World size of a trim, cm. */
    glm::dvec2 worldSize(const Trim& t) const;
    /** Sheet size in cm. */
    glm::dvec2 worldExtent() const { return glm::dvec2(width, height) / pxPerM * 100.0; }
    /** The trim's rectangle in uv (V up). */
    glm::dvec2 uvMin(const Trim& t) const;
    glm::dvec2 uvMax(const Trim& t) const;
    /** Index of the trim holding the uv point, or -1. */
    int trimAt(const glm::dvec2& uv) const;

    json toJson() const;
    static Sheet fromJson(const json& j);
};

Sheet load(const std::filesystem::path& file);
void save(const Sheet& sheet, const std::filesystem::path& file);
/** The bake folder of a sheet saved at file. */
std::filesystem::path textureFolder(const Sheet& sheet, const std::filesystem::path& file);

/** Problems worth showing: overlaps, trims off the sheet, borders off the mip grid, mixed bevel widths. */
std::vector<std::string> validate(const Sheet& sheet);

/** The sheet's trims as the matcher sees them. */
std::vector<uv::HotspotTrim> hotspotTrims(const Sheet& sheet);

// Layout edits, as the sheet editor makes them.

/**
 * Moves the border on the line axis = at (0 a vertical line at x = at, 1 a horizontal one at y = at) to `to`, for the run
 * of trims along it that holds `along` (the other coordinate), so a strip boundary moves for the whole strip and a row of
 * rects moves together. `to` is clamped so no trim gets thinner than a pixel. Returns the trims changed (none when no
 * border is there).
 */
std::vector<size_t> moveBorder(Sheet& sheet, int axis, int at, int along, int to);
/** Splits trim i on axis (0 vertically at x = at, 1 horizontally at y = at) into two; the new half comes right after it. False when at isn't inside. */
bool splitTrim(Sheet& sheet, size_t i, int axis, int at);
/** at rounded to the sheet's mip grid (multiples of 2^mipLevel px). */
int snapToGrid(const Sheet& sheet, double at);

/** Preset ids with labels and one line on what each is for. */
struct PresetInfo
{
    std::string id;
    std::string label;
    std::string doc;
};
const std::vector<PresetInfo>& presets();
/** A preset's layout and generator settings at the given width (height follows the preset's aspect). Throws on an unknown id. */
Sheet preset(std::string_view id, int width = 0);

/** Strips stacked top to bottom with these heights in px, full width, bevelled raised plates. */
Sheet stripLadder(std::string name, int width, int height, std::span<const int> heights);

// Interchange.

/** Valve's KeyValues .rect: Rectangles { rectangle { min "x y" max "x y" } }, pixel coords, top-left origin. */
std::string toRect(const Sheet& sheet);
Sheet fromRect(std::string_view text, int width, int height);
/** One <rect> per trim with id and data attributes. */
std::string toSvg(const Sheet& sheet);
Sheet fromSvg(std::string_view text);
/** Each distinct colour's bounding box becomes a trim; regions under minPx on a side are skipped. */
Sheet fromIdMap(const lila::image::Image& image, double tolerance = 0.02, int minPx = 4);
/**
 * ZenUV trims, as its JSON (what Arcbrush copies for it) or a ZenUV trim preset script, read as text and never run: each
 * trim's name and uv rect (left, top, right, bottom, V up), laid on a width by height sheet. Empty when the text has none.
 */
Sheet fromZenUv(std::string_view text, int width, int height);
/** Each uv island's bounding box (0-1 uv) becomes a trim. */
Sheet fromUvs(const geo::Mesh& mesh, std::string_view uvSet, int width, int height);

/** Exact flat colour per trim, the same colours fromIdMap reads back. */
lila::image::Image idMap(const Sheet& sheet);
/** Trim index + 1 per texel in red (0 off every trim): what the Trim Mask texture node reads. */
lila::image::Image trimIndexMap(const Sheet& sheet);
/** Each texel's trim rectangle in uv (V up) as min u, min v, max u, max v (zero off every trim): what Trim Gradient reads. */
lila::image::Image trimRectMap(const Sheet& sheet);
/** Indices of the trims named by index, name or tag. */
std::vector<size_t> trimsMatching(const Sheet& sheet, const json& which);
/** The ID colour of trim i: well spread hues, stable per index. */
glm::vec3 trimColor(size_t i);
} // namespace trims
