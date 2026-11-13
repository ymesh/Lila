#include "seq/Vat.h"

#include "core/ImageIO.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "fbx/Fbx.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Triangulate.h"
#include "math/MathCore.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "seq/RigidPieces.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace lila::seq
{
namespace
{
using geo::Index;

/**
 * Leaves out the faces on points with the int "debris" attribute set (Debris From Events chunks). Debris appears as
 * things break, so it would change the point count from frame to frame; it goes to Unreal as dressing instead.
 */
void dropDebris(geo::Mesh& m)
{
    const geo::Attribute* debris = m.pointAttrs().find("debris");
    if (!debris || debris->type() != geo::AttrType::Int)
        return;
    std::span<const int32_t> flag = debris->values<int32_t>();
    std::vector<Index> keep;
    bool any = false;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            const Index p = m.cornerPoint(m.faceCorner(f));
            if (p < flag.size() && flag[p] != 0)
                any = true;
            else
                keep.push_back(f);
        }
    if (any)
        m = geo::extractFaces(m, keep);
}


std::string patternedBaseName(scene::Graph& g, const std::vector<scene::NodeId>& meshes, const std::string& pattern)
{
    const scene::Node* n = meshes.empty() ? nullptr : g.node(meshes.front());
    std::string name = n ? n->name() : std::string("mesh");
    std::string s = pattern.empty() ? std::string("{name}") : pattern;
    for (size_t at = s.find("{name}"); at != std::string::npos; at = s.find("{name}", at + name.size()))
        s.replace(at, 6, name);
    return fileSafe(s);
}


/** One sampled frame in Lila space: a column per point (constant) and per soup corner (soup). */
struct Sample
{
    double frame = 0.0;
    Index points = 0;
    Index faces = 0;
    std::vector<glm::dvec3> pointPos, pointNrm;
    std::vector<glm::dvec3> soupPos, soupNrm;
};

geo::Mesh combine(FrameMeshes& f)
{
    geo::Mesh out;
    for (geo::Mesh& m : f.meshes)
    {
        m.compact();
        out.append(m);
    }
    out.compact();
    return out;
}

std::vector<glm::vec3> cornerNormalsOf(const geo::Mesh& m)
{
    std::span<const glm::vec3> custom = m.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    if (custom.size() == m.cornerCapacity())
        return {custom.begin(), custom.end()};
    return geo::cornerNormals(m);
}

/** positionsOnly fills pointPos and the counts, all rigid mode reads. */
Sample measure(const geo::Mesh& m, double frame, bool positionsOnly = false)
{
    LILA_ZONE();

    Sample s;
    s.frame = frame;
    s.points = m.numPoints();
    s.faces = m.numFaces();
    if (positionsOnly)
    {
        s.pointPos.reserve(s.points);
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
                s.pointPos.push_back(m.position(p));
        return s;
    }
    const std::vector<glm::vec3> cn = cornerNormalsOf(m);

    std::vector<glm::dvec3> sum(m.pointCapacity(), glm::dvec3(0.0));
    for (Index c = 0; c < m.cornerCapacity(); ++c)
        if (m.cornerValid(c))
            sum[m.cornerPoint(c)] += glm::dvec3(cn[c]);
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        s.pointPos.push_back(m.position(p));
        const double len = glm::length(sum[p]);
        s.pointNrm.push_back(len > 1e-12 ? sum[p] / len : glm::dvec3(0, 0, 1));
    }

    const geo::Triangulation tris = geo::triangulate(m);
    s.soupPos.reserve(tris.corners.size());
    s.soupNrm.reserve(tris.corners.size());
    for (Index c : tris.corners)
    {
        s.soupPos.push_back(m.position(m.cornerPoint(c)));
        const glm::dvec3 n(cn[c]);
        const double len = glm::length(n);
        s.soupNrm.push_back(len > 1e-12 ? n / len : glm::dvec3(0, 0, 1));
    }
    return s;
}

/** The rest frame's triangles as an unwelded soup of `slots` triangles; the slots past its own collapse at the origin. */
geo::Mesh soupMesh(const geo::Mesh& m, Index slots)
{
    LILA_ZONE();

    const geo::Triangulation tris = geo::triangulate(m);
    const std::vector<glm::vec3> cn = cornerNormalsOf(m);
    std::vector<std::string> uvSets;
    for (int set = 0; set < 8; ++set)
        if (m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(set)).size() == m.cornerCapacity())
            uvSets.push_back(geo::attr::uvName(set));

    geo::Mesh out;
    out.reserve(size_t(slots) * 3, size_t(slots) * 3, slots, size_t(slots) * 3);
    for (Index t = 0; t < slots; ++t)
    {
        Index p[3];
        for (int k = 0; k < 3; ++k)
            p[k] = out.addPoint(t < tris.triangleCount() ? m.position(m.cornerPoint(tris.corners[3 * t + k])) : glm::dvec3(0.0));
        out.addFace({p[0], p[1], p[2]});
    }
    std::span<glm::vec3> normals = out.cornerAttrs().getOrAdd<glm::vec3>(geo::attr::kNormal);
    std::vector<std::span<glm::vec2>> uvOut;
    std::vector<std::span<const glm::vec2>> uvIn;
    for (const std::string& set : uvSets)
    {
        out.cornerAttrs().getOrAdd<glm::vec2>(set);
        uvIn.push_back(m.cornerAttrs().get<glm::vec2>(set));
    }
    for (const std::string& set : uvSets)
        uvOut.push_back(out.cornerAttrs().get<glm::vec2>(set));
    normals = out.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    for (Index c = 0; c < out.cornerCapacity(); ++c)
    {
        if (!out.cornerValid(c))
            continue;
        const Index slot = out.cornerPoint(c);
        if (slot / 3 >= tris.triangleCount())
        {
            normals[c] = glm::vec3(0, 0, 1);
            continue;
        }
        const Index src = tris.corners[slot];
        normals[c] = cn[src];
        for (size_t s = 0; s < uvSets.size(); ++s)
            uvOut[s][c] = uvIn[s][src];
    }
    return out;
}

using Pieces = RigidPieces;
using RigidTrack = RigidFit;

RigidTrack fitRigid(const std::vector<Sample>& samples, const Sample& rest, const Pieces& pieces, const glm::dmat3& toUe)
{
    std::vector<std::span<const glm::dvec3>> frames;
    std::vector<double> times;
    frames.reserve(samples.size());
    for (const Sample& f : samples)
    {
        frames.push_back(f.pointPos);
        times.push_back(f.frame);
    }
    return fitRigidPieces(frames, times, rest.pointPos, pieces, toUe);
}

void checkOptions(const VatOptions& o)
{
    if (o.format != "exr" && o.format != "png16" && o.format != "png8")
        throw std::invalid_argument("format must be exr, png16 or png8");
    if (o.mode != "auto" && o.mode != "constant" && o.mode != "soup" && o.mode != "rigid")
        throw std::invalid_argument("mode must be auto, constant, soup or rigid");
    if (o.exrBits != 0 && o.exrBits != 16 && o.exrBits != 32)
        throw std::invalid_argument("exrBits must be 16 or 32");
    if (o.positions != "offset" && o.positions != "absolute")
        throw std::invalid_argument("positions must be offset or absolute");
    if (o.normalBits != 8 && o.normalBits != 16)
        throw std::invalid_argument("normalBits must be 8 or 16");
    if (o.maxWidth < 1 || o.maxWidth > 16384)
        throw std::invalid_argument("maxWidth must be 1 to 16384");
}

nlohmann::json vec(const glm::dvec3& v) { return {v.x, v.y, v.z}; }

std::string base(const VatResult& r) { return r.metadata.value("name", std::string()); }

VatResult planned(scene::Graph& g, const std::vector<scene::NodeId>& meshes, const VatOptions& o)
{
    checkOptions(o);
    const std::string base = patternedBaseName(g, meshes, o.name);
    const fs::path folder(o.folder);
    VatResult r;
    r.mesh = folder / (base + ".fbx");
    const std::string ext = o.format == "exr" ? ".exr" : ".png";
    r.positions = folder / (base + "_pos" + ext);
    if (o.mode == "rigid")
        r.rotations = folder / (base + "_rot" + ext);
    else if (o.normals)
        r.normals = folder / (base + "_nrm.png");
    r.meta = folder / (base + ".json");
    const std::vector<double> list = frames(o.sampling);
    const double fps = o.sampling.fps > 0.0 ? o.sampling.fps : scene::anim::playback().fps;
    r.metadata = {{"version", 1},
                  {"name", base},
                  {"frameCount", list.size()},
                  {"fps", fps / o.sampling.step},
                  {"start", list.front()},
                  {"end", list.back()},
                  {"step", o.sampling.step},
                  {"format", o.format},
                  {"modeRequested", o.mode},
                  {"positions", o.positions},
                  {"normals", !r.normals.empty()},
                  {"normalBits", o.normalBits},
                  {"profile", o.sampling.profile},
                  {"scale", o.sampling.scale},
                  {"space", "unreal"},
                  {"files", {{"mesh", r.mesh.filename().generic_string()},
                             {"positions", r.positions.filename().generic_string()},
                             {"normals", r.normals.empty() ? json() : json(r.normals.filename().generic_string())},
                             {"meta", r.meta.filename().generic_string()}}}};
    if (!r.rotations.empty())
        r.metadata["files"]["rotations"] = r.rotations.filename().generic_string();
    return r;
}

int exrBitsFor(const VatOptions& o, const std::string& mode)
{
    return o.exrBits ? o.exrBits : mode == "rigid" ? 32 : 16;
}

void writeMeta(const VatResult& r)
{
    std::ofstream out(r.meta, std::ios::binary);
    out << r.metadata.dump(2);
    if (!out)
        throw std::runtime_error("can't write " + r.meta.string());
}

/** The first of `count` consecutive free UV sets on m; throws when the 8 sets cannot hold them. */
int freeUvSets(const geo::Mesh& m, int count)
{
    int set = 0;
    while (set < 8 && m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(set)).size() == m.cornerCapacity())
        ++set;
    for (int k = set; k < set + count && k < 8; ++k)
        if (m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(k)).size() == m.cornerCapacity())
            throw std::runtime_error(std::format("UV set {} is taken; the vertex animation needs sets {} to {} free", k, set, set + count - 1));
    if (set + count > 8)
        throw std::runtime_error(count == 1 ? std::string("the mesh uses all 8 UV sets; the vertex animation needs one free")
                                            : std::format("the mesh uses {} UV sets; the rigid vertex animation needs {} more of the 8", set, count));
    return set;
}

void writeRestFbx(geo::Mesh&& restMesh, const VatResult& r, const SequenceOptions& so, bool tangents)
{
    LILA_ZONE();

    std::error_code ec;
    fs::create_directories(r.mesh.parent_path(), ec);
    fbx::ExportScene es;
    es.upAxis = math::upAxis();
    fbx::ExportNode node;
    node.name = r.metadata["name"].get<std::string>();
    node.mesh = std::make_shared<const geo::Mesh>(std::move(restMesh));
    es.nodes.push_back(std::move(node));
    fbx::ExportOptions fo;
    fbx::parseProfile(so.profile, &fo.profile);
    fo.triangulate = true;
    fo.normals = true;
    fo.tangents = tangents;
    std::string err;
    if (!fbx::writeFbx(es, r.mesh, fo, &err))
        throw std::runtime_error(err);
}

/** Writes the rigid bake's textures, rest mesh and metadata into r. */
void writeRigid(VatResult& r, const VatOptions& o, const SequenceOptions& so, const std::vector<Sample>& samples, const Sample& restSample,
                geo::Mesh restMesh, const Pieces& pieces, const RigidTrack& track, const glm::dmat3& toUe)
{
    LILA_ZONE();

    const size_t n = pieces.count;
    if (n == 0)
        throw std::runtime_error("nothing to bake: no pieces");
    const int width = int(std::min<size_t>(n, size_t(o.maxWidth)));
    const int rowsPerFrame = int((n + size_t(width) - 1) / size_t(width));
    const int height = int(samples.size()) * rowsPerFrame;
    if (height > kVatMaxHeight)
        throw std::runtime_error(std::format("{} frames of {} rows need a texture {} rows tall; Unreal takes {} at most (raise maxWidth or "
                                             "export fewer frames)",
                                             samples.size(), rowsPerFrame, height, kVatMaxHeight));
    if (track.residual > kVatRigidTolerance)
        log::warn("Vertex animation {}: the pieces are not rigid; the worst fit is {:.4g} units off (piece {}, frame {:g}), so the bake "
                  "will not match the animation there",
                  base(r), track.residual, track.residualPiece, track.residualFrame);

    const bool absolute = o.positions == "absolute";
    geo::Bounds valueBounds, meshBounds;
    for (size_t f = 0; f < samples.size(); ++f)
    {
        for (size_t k = 0; k < n; ++k)
            valueBounds.extend(absolute ? track.centres[f * n + k] : track.centres[f * n + k] - track.pivots[k]);
        for (const glm::dvec3& p : samples[f].pointPos)
            meshBounds.extend(toUe * p);
    }
    for (const glm::dvec3& p : restSample.pointPos)
        meshBounds.extend(toUe * p);

    const bool png = o.format != "exr";
    const glm::dvec3 lo = valueBounds.min, extent = valueBounds.max - valueBounds.min;
    std::vector<float> pos(size_t(width) * size_t(height) * 4, 0.0f), rot(pos.size(), 0.0f);
    for (size_t f = 0; f < samples.size(); ++f)
        for (size_t k = 0; k < n; ++k)
        {
            const size_t texel = (f * size_t(rowsPerFrame) + k / size_t(width)) * size_t(width) + k % size_t(width);
            const glm::dvec3 v = absolute ? track.centres[f * n + k] : track.centres[f * n + k] - track.pivots[k];
            float* px = &pos[texel * 4];
            for (int c = 0; c < 3; ++c)
                px[c] = png ? (extent[c] > 0.0 ? float((v[c] - lo[c]) / extent[c]) : 0.0f) : float(v[c]);
            px[3] = 1.0f;
            const glm::dvec4 q = track.rotations[f * n + k];
            float* rx = &rot[texel * 4];
            for (int c = 0; c < 4; ++c)
                rx[c] = png ? float(q[c] * 0.5 + 0.5) : float(q[c]);
        }

    const int uvSet = freeUvSets(restMesh, 3);
    {
        for (int k = 0; k < 3; ++k)
            restMesh.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(uvSet + k));
        std::span<glm::vec2> address = restMesh.cornerAttrs().get<glm::vec2>(geo::attr::uvName(uvSet));
        std::span<glm::vec2> pivotXY = restMesh.cornerAttrs().get<glm::vec2>(geo::attr::uvName(uvSet + 1));
        std::span<glm::vec2> pivotZ = restMesh.cornerAttrs().get<glm::vec2>(geo::attr::uvName(uvSet + 2));
        std::vector<Index> column(restMesh.pointCapacity(), 0);
        Index next = 0;
        for (Index p = 0; p < restMesh.pointCapacity(); ++p)
            if (restMesh.pointValid(p))
                column[p] = next++;
        for (Index c = 0; c < restMesh.cornerCapacity(); ++c)
        {
            if (!restMesh.cornerValid(c))
                continue;
            const size_t k = pieces.of[column[restMesh.cornerPoint(c)]];
            const float u = float((double(k % size_t(width)) + 0.5) / width);
            const float v = float((double(k / size_t(width)) + 0.5) / height);
            const glm::dvec3 pivot = track.pivots[k];
            // Unreal flips V on import, so V is written as 1 - V.
            address[c] = glm::vec2(u, 1.0f - v);
            pivotXY[c] = glm::vec2(float(pivot.x), float(1.0 - pivot.y));
            pivotZ[c] = glm::vec2(float(pivot.z), 1.0f);
        }
    }
    writeRestFbx(std::move(restMesh), r, so, true);
    const int exrBits = exrBitsFor(o, "rigid");
    if (png)
    {
        image::writePng(r.positions, width, height, pos, o.format == "png8" ? 8 : 16);
        image::writePng(r.rotations, width, height, rot, o.format == "png8" ? 8 : 16);
    }
    else
    {
        image::writeExr(r.positions, width, height, pos, exrBits);
        image::writeExr(r.rotations, width, height, rot, exrBits);
    }

    json& m = r.metadata;
    const int bits = png ? (o.format == "png8" ? 8 : 16) : exrBits;
    m["mode"] = "rigid";
    m["frameCount"] = samples.size();
    m["restFrame"] = restSample.frame;
    m["vertexCount"] = restSample.points;
    m["pieceCount"] = n;
    m["pieceSource"] = pieces.fromAttribute ? "attribute" : "components";
    m["width"] = width;
    m["height"] = height;
    m["rowsPerFrame"] = rowsPerFrame;
    m["uvChannel"] = uvSet;
    m["pivotChannels"] = {uvSet + 1, uvSet + 2};
    m["bounds"] = {{"min", vec(valueBounds.min)}, {"max", vec(valueBounds.max)}};
    m["positionBits"] = bits;
    m["rotationBits"] = bits;
    m["rigid"] = {{"maxResidual", track.residual}, {"piece", track.residualPiece}, {"frame", track.residualFrame}, {"tolerance", kVatRigidTolerance}};
    json mp = {{"HDR", png ? 0 : 1},
               {"BoundsMin", vec(valueBounds.min)},
               {"BoundsMax", vec(valueBounds.max)},
               {"FrameCount", samples.size()},
               {"RowsPerFrame", rowsPerFrame},
               {"TextureHeight", height},
               {"FrameRate", m["fps"]},
               {"PieceCount", n},
               {"PivotChannelXY", uvSet + 1},
               {"PivotChannelZ", uvSet + 2},
               {"PositionTexture", r.positions.filename().generic_string()},
               {"RotationTexture", r.rotations.filename().generic_string()}};
    m["materialParams"] = mp;
    m["meshBounds"] = {{"min", vec(meshBounds.min)}, {"max", vec(meshBounds.max)}};
    writeMeta(r);
    log::info("Vertex animation {} (rigid mode, {} pieces, {} frames, {}x{}, worst fit {:.3g}): {}", base(r), n, samples.size(), width, height,
              track.residual, vatMaterialSummary(mp));
}
} // namespace

std::string vatMaterialSummary(const nlohmann::json& p)
{
    auto num = [](const nlohmann::json& v) { return v.is_number() ? std::format("{:g}", v.get<double>()) : v.dump(); };
    auto vec3 = [&](const nlohmann::json& v) {
        return v.is_array() && v.size() == 3 ? std::format("({}, {}, {})", num(v[0]), num(v[1]), num(v[2])) : v.dump();
    };
    std::string s = std::format("HDR={} BoundsMin={} BoundsMax={} FrameCount={} RowsPerFrame={} TextureHeight={} FrameRate={} PositionTexture={}",
                                num(p.value("HDR", json(0))), vec3(p.value("BoundsMin", json())), vec3(p.value("BoundsMax", json())),
                                num(p.value("FrameCount", json(0))), num(p.value("RowsPerFrame", json(1))), num(p.value("TextureHeight", json(1))),
                                num(p.value("FrameRate", json(0))), p.value("PositionTexture", std::string()));
    if (p.contains("NormalTexture"))
        s += " NormalTexture=" + p["NormalTexture"].get<std::string>();
    if (p.contains("RotationTexture"))
        s += std::format(" RotationTexture={} PieceCount={}", p["RotationTexture"].get<std::string>(), num(p.value("PieceCount", json(0))));
    return s;
}

int vatDefaultMaxWidth()
{
    return std::clamp(PrefStore::get().value<int>("vat", "/maxWidth", 4096), 16, 16384);
}

VatOptions vatFromJson(const nlohmann::json& j, const SequenceOptions& sampling)
{
    VatOptions o;
    o.sampling = sampling;
    o.maxWidth = vatDefaultMaxWidth();
    if (!j.is_object())
        return o;
    o.folder = j.value("folder", o.folder);
    o.name = j.value("name", o.name);
    o.format = j.value("format", o.format);
    o.mode = j.value("mode", o.mode);
    o.positions = j.value("positions", o.positions);
    o.normals = j.value("normals", o.normals);
    o.normalBits = j.value("normalBits", o.normalBits);
    o.maxWidth = j.value("maxWidth", o.maxWidth);
    o.exrBits = j.value("exrBits", o.exrBits);
    if (j.contains("restFrame") && j["restFrame"].is_number())
        o.restFrame = j["restFrame"].get<double>();
    checkOptions(o);
    return o;
}

nlohmann::json VatResult::files() const
{
    nlohmann::json f = {{"mesh", mesh.generic_string()}, {"positions", positions.generic_string()}, {"meta", meta.generic_string()}};
    if (!normals.empty())
        f["normals"] = normals.generic_string();
    if (!rotations.empty())
        f["rotations"] = rotations.generic_string();
    return f;
}

VatResult plannedVat(scene::Graph& g, const std::vector<scene::NodeId>& meshes, const VatOptions& o)
{
    return planned(g, meshes, o);
}

VatResult exportVat(scene::Graph& g, const std::vector<scene::NodeId>& meshes, const VatOptions& o, const MeshGetter& getter)
{
    LILA_ZONE();

    VatResult r = planned(g, meshes, o);
    SequenceOptions so = o.sampling;
    so.triangulate = false;
    so.normals = true;

    std::vector<Sample> samples;
    std::optional<geo::Mesh> rest;
    std::optional<Sample> restSample;
    int restIndex = -1;
    const std::vector<double> list = frames(so);
    if (!std::isnan(o.restFrame))
        for (size_t i = 0; i < list.size(); ++i)
            if (std::abs(list[i] - o.restFrame) < 1e-6)
                restIndex = int(i);
    if (std::isnan(o.restFrame))
        restIndex = 0;

    std::optional<Pieces> restPieces;
    const bool positionsOnly = o.mode == "rigid";
    sample(g, meshes, so, [&](FrameMeshes& f) {
        for (geo::Mesh& part : f.meshes)
            dropDebris(part);
        geo::Mesh m = combine(f);
        samples.push_back(measure(m, f.frame, positionsOnly));
        if (f.index == restIndex)
        {
            restPieces = findRigidPieces(f.meshes);
            rest = std::move(m);
        }
    }, "Exporting vertex animation", getter);
    if (restIndex >= 0)
        restSample = samples[size_t(restIndex)];
    else
    {
        SequenceOptions one = so;
        one.playbackRange = false;
        one.start = one.end = o.restFrame;
        one.step = 1.0;
        sample(g, meshes, one, [&](FrameMeshes& f) {
            for (geo::Mesh& part : f.meshes)
                dropDebris(part);
            rest = combine(f);
            restSample = measure(*rest, f.frame, positionsOnly);
            restPieces = findRigidPieces(f.meshes);
        }, "Sampling the rest frame", getter);
    }
    if (!rest || rest->numPoints() == 0)
        throw std::runtime_error("the rest frame has no vertices");

    // auto and constant need every frame to keep the rest frame's points and faces.
    const Sample* mismatch = nullptr;
    for (const Sample& s : samples)
        if (s.points != restSample->points || s.faces != restSample->faces)
        {
            mismatch = &s;
            break;
        }
    if (o.mode == "rigid" && mismatch)
        throw std::runtime_error(std::format("frame {} has {} points and {} faces where the rest frame has {} and {}; rigid needs the "
                                             "same points on every frame",
                                             mismatch->frame, mismatch->points, mismatch->faces, restSample->points, restSample->faces));
    const glm::dmat3 toUe = unrealBasis(so.profile);
    std::optional<RigidTrack> track;
    if (o.mode == "rigid" || (o.mode == "auto" && !mismatch && restPieces->fromAttribute))
        track = fitRigid(samples, *restSample, *restPieces, toUe);
    std::string mode = o.mode;
    if (o.mode == "auto")
        mode = mismatch ? "soup" : track && track->residual <= kVatRigidTolerance ? "rigid" : "constant";
    if (mode == "rigid")
    {
        if (o.mode == "auto")
        {
            VatOptions ro = o;
            ro.mode = "rigid";
            const json requested = r.metadata["modeRequested"];
            r = planned(g, meshes, ro);
            r.metadata["modeRequested"] = requested;
        }
        writeRigid(r, o, so, samples, *restSample, std::move(*rest), *restPieces, *track, toUe);
        return r;
    }
    if (mode == "constant" && mismatch)
        throw std::runtime_error(std::format("frame {} has {} points and {} faces where the rest frame has {} and {}; use mode soup",
                                             mismatch->frame, mismatch->points, mismatch->faces, restSample->points, restSample->faces));

    const bool soup = mode == "soup";
    size_t maxTris = restSample->soupPos.size() / 3;
    for (const Sample& s : samples)
        maxTris = std::max(maxTris, s.soupPos.size() / 3);
    const size_t n = soup ? maxTris * 3 : size_t(restSample->points);
    if (n == 0)
        throw std::runtime_error("nothing to bake: no triangles");

    const int width = int(std::min<size_t>(n, size_t(o.maxWidth)));
    const int rowsPerFrame = int((n + size_t(width) - 1) / size_t(width));
    const int height = int(samples.size()) * rowsPerFrame;
    if (height > kVatMaxHeight)
        throw std::runtime_error(std::format("{} frames of {} rows need a texture {} rows tall; Unreal takes {} at most (raise maxWidth or "
                                             "export fewer frames)",
                                             samples.size(), rowsPerFrame, height, kVatMaxHeight));

    const std::vector<glm::dvec3>& restPos = soup ? restSample->soupPos : restSample->pointPos;
    auto restAt = [&](size_t i) { return i < restPos.size() ? restPos[i] : glm::dvec3(0.0); };
    // Slots a frame does not fill collapse onto their triangle's first rest corner.
    auto valueAt = [&](const Sample& s, size_t i, glm::dvec3* pos, glm::dvec3* nrm) {
        const std::vector<glm::dvec3>& p = soup ? s.soupPos : s.pointPos;
        const std::vector<glm::dvec3>& nn = soup ? s.soupNrm : s.pointNrm;
        if (i < p.size())
        {
            *pos = p[i];
            *nrm = nn[i];
            return;
        }
        *pos = restAt(i - i % 3);
        const std::vector<glm::dvec3>& rn = soup ? restSample->soupNrm : restSample->pointNrm;
        *nrm = i < rn.size() ? rn[i] : glm::dvec3(0, 0, 1);
    };

    geo::Bounds valueBounds, meshBounds;
    std::vector<glm::dvec3> values(n * samples.size()), normals(n * samples.size());
    for (size_t f = 0; f < samples.size(); ++f)
        for (size_t i = 0; i < n; ++i)
        {
            glm::dvec3 p, nr;
            valueAt(samples[f], i, &p, &nr);
            const glm::dvec3 ue = toUe * p;
            meshBounds.extend(ue);
            const glm::dvec3 v = o.positions == "offset" ? ue - toUe * restAt(i) : ue;
            valueBounds.extend(v);
            values[f * n + i] = v;
            normals[f * n + i] = toUe * nr;
        }
    for (size_t i = 0; i < n; ++i)
        meshBounds.extend(toUe * restAt(i));

    const bool png = o.format != "exr";
    const glm::dvec3 lo = valueBounds.min, extent = valueBounds.max - valueBounds.min;
    std::vector<float> pos(size_t(width) * size_t(height) * 4, 0.0f), nrm;
    if (o.normals)
        nrm.assign(pos.size(), 0.0f);
    for (size_t f = 0; f < samples.size(); ++f)
        for (size_t i = 0; i < n; ++i)
        {
            const size_t row = f * size_t(rowsPerFrame) + i / size_t(width), col = i % size_t(width);
            float* px = &pos[(row * size_t(width) + col) * 4];
            const glm::dvec3 v = values[f * n + i];
            for (int c = 0; c < 3; ++c)
                px[c] = png ? (extent[c] > 0.0 ? float((v[c] - lo[c]) / extent[c]) : 0.0f) : float(v[c]);
            px[3] = 1.0f;
            if (o.normals)
            {
                float* nx = &nrm[(row * size_t(width) + col) * 4];
                const glm::dvec3 nv = normals[f * n + i];
                for (int c = 0; c < 3; ++c)
                    nx[c] = float(nv[c] * 0.5 + 0.5);
                nx[3] = 1.0f;
            }
        }

    // The rest mesh with the addressing UV in the first free set.
    geo::Mesh restMesh = soup ? soupMesh(*rest, Index(maxTris)) : *rest;
    const int uvSet = freeUvSets(restMesh, 1);
    {
        std::span<glm::vec2> uv = restMesh.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(uvSet));
        std::vector<Index> column(restMesh.pointCapacity(), 0);
        Index next = 0;
        for (Index p = 0; p < restMesh.pointCapacity(); ++p)
            if (restMesh.pointValid(p))
                column[p] = next++;
        for (Index c = 0; c < restMesh.cornerCapacity(); ++c)
        {
            if (!restMesh.cornerValid(c))
                continue;
            const size_t i = column[restMesh.cornerPoint(c)];
            const float u = float((double(i % size_t(width)) + 0.5) / width);
            const float v = float((double(i / size_t(width)) + 0.5) / height);
            // Unreal flips V on import.
            uv[c] = glm::vec2(u, 1.0f - v);
        }
    }

    writeRestFbx(std::move(restMesh), r, so, uvSet > 0);
    const int exrBits = exrBitsFor(o, mode);
    if (png)
        image::writePng(r.positions, width, height, pos, o.format == "png8" ? 8 : 16);
    else
        image::writeExr(r.positions, width, height, pos, exrBits);
    if (o.normals)
        image::writePng(r.normals, width, height, nrm, o.normalBits);

    json& m = r.metadata;
    m["mode"] = mode;
    m["frameCount"] = samples.size();
    m["restFrame"] = restSample->frame;
    m["vertexCount"] = n;
    m["triangles"] = soup ? maxTris : restSample->soupPos.size() / 3;
    m["width"] = width;
    m["height"] = height;
    m["rowsPerFrame"] = rowsPerFrame;
    m["uvChannel"] = uvSet;
    m["bounds"] = {{"min", vec(valueBounds.min)}, {"max", vec(valueBounds.max)}};
    m["positionBits"] = o.format == "exr" ? exrBits : o.format == "png8" ? 8 : 16;
    json mp = {{"HDR", png ? 0 : 1},
               {"BoundsMin", vec(valueBounds.min)},
               {"BoundsMax", vec(valueBounds.max)},
               {"FrameCount", samples.size()},
               {"RowsPerFrame", rowsPerFrame},
               {"TextureHeight", height},
               {"FrameRate", m["fps"]},
               {"PositionTexture", r.positions.filename().generic_string()}};
    if (o.normals)
        mp["NormalTexture"] = r.normals.filename().generic_string();
    m["materialParams"] = mp;
    m["meshBounds"] = {{"min", vec(meshBounds.min)}, {"max", vec(meshBounds.max)}};
    writeMeta(r);
    log::info("Vertex animation {} ({} mode, {} frames, {}x{}): {}", base(r), mode, samples.size(), width, height, vatMaterialSummary(mp));
    return r;
}

VatImage readVatImage(const fs::path& file, std::string* err)
{
    std::string ext = file.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    image::Image img = ext == ".exr" ? image::readExr(file, err) : image::readPng(file, err);
    return {img.width, img.height, std::move(img.rgba)};
}
} // namespace lila::seq
