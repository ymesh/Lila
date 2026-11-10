#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * The statement model both Maya readers produce: what a .ma file says, command by command, with the values already
 * tokenised. The .mb reader decodes its IFF chunks into the same model, so everything after reading is shared.
 *
 * Units: an ASCII file stores linear, angular and time values in the units its currentUnit line declares; a binary
 * file stores them in Maya's internal units (cm, radians, ticks). `internalUnits` says which, and the converter
 * scales accordingly. Binary key times are turned into frames of the file's time unit when read, as in ASCII.
 */
namespace lila::maya
{
/** A polygon mesh as Maya stores it: points, edges and faces as signed edge loops, with per-face-vertex indices. */
struct MeshData
{
    struct Loop
    {
        /** Edge indices; a negative entry e means edge ~e walked backwards (Maya's -1 - e). */
        std::vector<int32_t> edges;
        /** Per uv set: one uv index per edge of the loop, empty when the set has no uvs here. */
        std::vector<std::vector<int32_t>> uv;
        /** Per color set, as uv. -1 means no color. */
        std::vector<std::vector<int32_t>> color;
    };
    struct Face
    {
        std::vector<Loop> loops; // the first is the outline, the rest holes
    };
    struct UvSet
    {
        std::string name;
        std::vector<glm::vec2> uvs;
    };
    struct ColorSet
    {
        std::string name;
        int channels = 4; // 1 alpha, 3 rgb, 4 rgba
        std::vector<glm::vec4> colors;
    };

    std::vector<glm::dvec3> points;
    std::vector<std::array<int32_t, 2>> edges;
    std::vector<char> edgeHard;
    std::vector<Face> faces;
    std::vector<UvSet> uvSets;
    std::vector<ColorSet> colorSets;
    /** Per face-vertex in face order; may be shorter than the face-vertex count. Components of 1e20 or more are unset. */
    std::vector<glm::vec3> normals;
    std::vector<std::pair<int32_t, float>> edgeCreases;
    std::vector<std::pair<int32_t, float>> vertexCreases;
};

/** One `setAttr`: the attribute path without its leading dot, the -type, and the values. */
struct AttrValue
{
    std::string attr; // "t", "uvst[0].uvsp[0:13]", "wl[0:19].w"
    std::string type; // -type: "double3", "string", "componentList", "polyFaces", "matrix", ... or "" for plain values
    std::vector<double> nums;
    std::vector<std::string> strs; // strings, componentList items, or raw tokens of types kept as text
    std::shared_ptr<const MeshData> mesh; // polyFaces (faces only) or a binary mesh blob (everything)
    int size = -1; // -s
    int lock = -1; // -l: 1 on, 0 off, -1 not given
};

/**
 * One edit a scene made to a node of a referenced file (a reference node's "ed" attribute). kind is Maya's: 0 parent
 * (child, parent, flags), 1 addAttr (node, long, short, flags), 2 setAttr (node, attr, MEL value text),
 * 3 disconnectAttr (src, dst), 5 connectAttr (side, reference node, referenced plug, placeholder plug, flags; side 3
 * means the referenced plug is the source, 4 the destination), 7 deleteAttr (node, attr), 8 lock (node, attr).
 */
struct RefEdit
{
    int kind = -1;
    std::vector<std::string> args;
};

struct AddedAttr
{
    std::string longName, shortName, type;
    std::string parent; // -p: a child of this compound
    std::optional<double> defaultValue, min, max, softMin, softMax;
    std::vector<std::string> enumNames; // -en "a:b:c", or "a=0:b=2"
};

struct Node
{
    std::string type; // empty for nodes the file only selects (`select -ne :time1`)
    std::string name;
    std::string parent;
    bool shared = false; // -s: a startup node such as the persp camera
    bool existing = false; // `select -ne`: a node the file expects to exist already
    std::vector<AttrValue> attrs;
    std::vector<AddedAttr> added;
    std::vector<RefEdit> refEdits; // reference nodes only
};

struct Connection
{
    std::string src, dst;
    bool nextAvailable = false;
};

/**
 * A `relationship` statement, which is how .ma files store light linking: the type ("link", "ignore", "shadowLink",
 * ...), the linker node, and the plugs it relates, the objects first and the light (or light set) last.
 */
struct Relationship
{
    std::string type, linker;
    std::vector<std::string> plugs;
};

struct Reference
{
    std::string path, ns, type;
    std::string refNode; // the reference node holding this reference's edits (-rfn)
    bool loaded = true;
};

struct ParentEdit
{
    std::string child, parent;
    bool shape = false, add = false;
};

struct File
{
    bool binary = false;
    bool internalUnits = false;
    std::string linearUnit = "cm", angleUnit = "deg", timeUnit = "film";
    std::string version;
    std::map<std::string, std::string> fileInfo;
    std::vector<std::string> requires_;
    std::vector<Node> nodes;
    std::vector<Connection> connections;
    std::vector<Relationship> relationships;
    std::vector<Reference> references;
    std::vector<ParentEdit> parents;
    std::vector<std::string> warnings;
};

/** Frames per second of a Maya time unit ("film", "ntsc", "30fps", ...); 0 for an unknown one. */
LILA_SCENE_API double fpsOf(std::string_view unit);
/** Centimetres per Maya linear unit ("mm", "cm", "m", "in", ...); 0 for an unknown one. */
LILA_SCENE_API double cmPerUnit(std::string_view unit);

/** Never throws: anything it can't read becomes a warning. */
LILA_SCENE_API File readAscii(std::string_view text);
LILA_SCENE_API File readBinary(std::span<const uint8_t> bytes);
/** Reads either kind by its content. Throws std::runtime_error when the file can't be opened or is neither. */
LILA_SCENE_API File readFile(const std::filesystem::path& path);

/** True when the bytes start like a Maya binary (IFF) file. */
LILA_SCENE_API bool looksBinary(std::span<const uint8_t> bytes);

/** Maya type name for a binary node form id, or empty. */
LILA_SCENE_API std::string_view typeForCode(uint32_t code);

/** One segment of an attribute path: name[lo:hi] (lo = hi = -1 without brackets, -2 for [*]). */
struct AttrSeg
{
    std::string name;
    int lo = -1, hi = -1;
};
LILA_SCENE_API std::vector<AttrSeg> splitAttr(std::string_view attr);

/** Parses polyFaces tokens (f / h / mu / mc / fc ...). Unknown tags end the parse with a warning. */
LILA_SCENE_API std::shared_ptr<MeshData> parsePolyFaces(std::span<const std::string> tokens, std::string* warning);

/** dataReferenceEdits tokens as an ASCII file writes them. Stops at an edit kind it doesn't know, with a warning. */
LILA_SCENE_API std::vector<RefEdit> parseRefEdits(std::span<const std::string> tokens, std::string* warning);

/** "f[0:3]", "e[5]", "vtx[*]" to (kind, lo, hi); hi = -1 for [*]. False when it isn't a component. */
LILA_SCENE_API bool parseComponent(std::string_view s, std::string* kind, int* lo, int* hi);
} // namespace lila::maya
