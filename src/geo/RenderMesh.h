#pragma once

#include "core/Export.h"
#include "core/UninitVector.h"

#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "geo/Triangulate.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace lila::geo
{
struct RenderMeshOptions
{
    int maxUvSets = 8;
    bool tangents = false;
    bool colors = true;
    /** Corner float4 layer the colors come from; empty is the mesh's current color set. */
    std::string colorLayer;
    bool wireframe = true;
    /** Group triangles into one section per face "material" id. */
    bool sections = true;
    /**
     * More corner layers (float or float4) that split vertices where their values differ, for per-corner data the shader
     * interpolates that must not leak from one face into the next (grunge distances).
     */
    std::vector<std::string> splitLayers;
    /** Used when the mesh has no corner "normal" attribute. */
    CornerNormalOptions normals;
    TriangulateOptions triangulation;
};

struct RenderSection
{
    int32_t material = 0;
    Index firstIndex = 0;
    Index indexCount = 0;
};

/**
 * Flat GPU-ready buffers. One vertex per unique combination of point and corner attributes, so
 * vertices split exactly along hard edges, uv seams and colour borders.
 */
struct RenderMesh
{
    UninitVector<glm::vec3> positions;
    UninitVector<glm::vec3> normals;
    std::vector<glm::vec4> tangents;            // empty unless requested and uv0 exists
    std::vector<glm::vec4> colors;              // empty unless the mesh has a color set
    std::vector<UninitVector<glm::vec2>> uvs;   // uv0, uv1, ... as present
    UninitVector<Index> vertexPoint;            // source point per vertex
    UninitVector<Index> vertexCorner;           // one source corner per vertex
    UninitVector<Index> cornerVertex;           // vertex per source corner, kInvalid for dead corners
    UninitVector<Index> indices;                // 3 per triangle, ordered by section
    UninitVector<Index> triangleFaces;          // source face per triangle
    UninitVector<uint8_t> triangleSides;        // per triangle, bit k set when side k (vertex k to k + 1) is a polygon edge
    std::vector<RenderSection> sections;
    UninitVector<Index> lineIndices;            // 2 per edge, into the vertex buffers
    UninitVector<Index> lineEdges;              // source edge per line

    Index vertexCount() const { return Index(positions.size()); }
    Index triangleCount() const { return Index(indices.size() / 3); }
};

/** Parallel. */
LILA_SCENE_API RenderMesh buildRenderMesh(const Mesh& mesh, const RenderMeshOptions& opts = {});
} // namespace lila::geo
