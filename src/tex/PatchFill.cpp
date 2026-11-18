#include "tex/PatchFill.h"

#include "bake/Bake.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "tex/Plan.h"
#include "tex/TextureSet.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <random>
#include <stdexcept>

namespace lila::tex
{
namespace
{
/** One pyramid level: c floats per texel, the hole, and where a patch may be taken from. */
struct Level
{
    int w = 0, h = 0, c = 0;
    std::vector<float> px;
    std::vector<uint8_t> hole, source;
    /** Centres whose whole patch is source. */
    std::vector<int> valid;
    std::vector<uint8_t> isValid;
};

void findValid(Level& l, int r)
{
    l.valid.clear();
    l.isValid.assign(size_t(l.w) * l.h, 0);
    std::vector<int> sat(size_t(l.w + 1) * (l.h + 1), 0);
    for (int y = 0; y < l.h; ++y)
        for (int x = 0; x < l.w; ++x)
            sat[size_t(y + 1) * (l.w + 1) + x + 1] = (l.source[size_t(y) * l.w + x] ? 1 : 0) + sat[size_t(y) * (l.w + 1) + x + 1] +
                                                    sat[size_t(y + 1) * (l.w + 1) + x] - sat[size_t(y) * (l.w + 1) + x];
    const int side = 2 * r + 1;
    for (int y = r; y < l.h - r; ++y)
        for (int x = r; x < l.w - r; ++x)
        {
            const int x0 = x - r, y0 = y - r, x1 = x + r + 1, y1 = y + r + 1;
            const int n = sat[size_t(y1) * (l.w + 1) + x1] - sat[size_t(y0) * (l.w + 1) + x1] - sat[size_t(y1) * (l.w + 1) + x0] + sat[size_t(y0) * (l.w + 1) + x0];
            if (n == side * side)
            {
                l.valid.push_back(y * l.w + x);
                l.isValid[size_t(y) * l.w + x] = 1;
            }
        }
}

Level downsample(const Level& f)
{
    Level l;
    l.w = std::max(1, f.w / 2);
    l.h = std::max(1, f.h / 2);
    l.c = f.c;
    l.px.assign(size_t(l.w) * l.h * l.c, 0.0f);
    l.hole.assign(size_t(l.w) * l.h, 0);
    l.source.assign(size_t(l.w) * l.h, 0);
    for (int y = 0; y < l.h; ++y)
        for (int x = 0; x < l.w; ++x)
        {
            int known = 0, src = 0, holes = 0;
            const size_t o = size_t(y) * l.w + x;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx)
                {
                    const int fx = std::min(f.w - 1, 2 * x + dx), fy = std::min(f.h - 1, 2 * y + dy);
                    const size_t fo = size_t(fy) * f.w + fx;
                    holes += f.hole[fo] ? 1 : 0;
                    src += f.source[fo] ? 1 : 0;
                    if (!f.hole[fo])
                    {
                        ++known;
                        for (int k = 0; k < l.c; ++k)
                            l.px[o * l.c + k] += f.px[fo * f.c + k];
                    }
                }
            if (known)
                for (int k = 0; k < l.c; ++k)
                    l.px[o * l.c + k] /= float(known);
            l.hole[o] = holes ? 1 : 0;
            l.source[o] = src == 4 ? 1 : 0;
        }
    return l;
}

/** Fills the hole by repeatedly averaging known neighbours inwards: the start for the coarsest level. */
void diffuse(Level& l)
{
    std::vector<uint8_t> known(l.hole.size());
    for (size_t i = 0; i < known.size(); ++i)
        known[i] = !l.hole[i];
    bool any = true;
    while (any)
    {
        any = false;
        std::vector<uint8_t> next = known;
        for (int y = 0; y < l.h; ++y)
            for (int x = 0; x < l.w; ++x)
            {
                const size_t o = size_t(y) * l.w + x;
                if (known[o])
                    continue;
                std::vector<float> sum(size_t(l.c), 0.0f);
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= l.w || ny >= l.h || !known[size_t(ny) * l.w + nx])
                            continue;
                        for (int k = 0; k < l.c; ++k)
                            sum[size_t(k)] += l.px[(size_t(ny) * l.w + nx) * l.c + k];
                        ++n;
                    }
                if (n)
                {
                    for (int k = 0; k < l.c; ++k)
                        l.px[o * l.c + k] = sum[size_t(k)] / float(n);
                    next[o] = 1;
                    any = true;
                }
            }
        known.swap(next);
    }
}

class Matcher
{
public:
    Matcher(Level& l, int r, std::mt19937& rng) : mL(l), mR(r), mRng(rng) {}

    /** Patch centres that touch the hole: the only ones whose matches matter. */
    void findTargets()
    {
        mTargets.clear();
        std::vector<uint8_t> near(size_t(mL.w) * mL.h, 0);
        for (int y = 0; y < mL.h; ++y)
            for (int x = 0; x < mL.w; ++x)
                if (mL.hole[size_t(y) * mL.w + x])
                    for (int dy = -mR; dy <= mR; ++dy)
                        for (int dx = -mR; dx <= mR; ++dx)
                        {
                            const int nx = x + dx, ny = y + dy;
                            if (nx >= 0 && ny >= 0 && nx < mL.w && ny < mL.h)
                                near[size_t(ny) * mL.w + nx] = 1;
                        }
        for (int i = 0; i < mL.w * mL.h; ++i)
            if (near[size_t(i)])
                mTargets.push_back(i);
    }

    float distance(int p, int q, float best) const
    {
        const int px = p % mL.w, py = p / mL.w, qx = q % mL.w, qy = q / mL.w;
        float d = 0.0f;
        for (int dy = -mR; dy <= mR; ++dy)
        {
            const int ty = py + dy;
            if (ty < 0 || ty >= mL.h)
                continue;
            for (int dx = -mR; dx <= mR; ++dx)
            {
                const int tx = px + dx;
                if (tx < 0 || tx >= mL.w)
                    continue;
                const float* a = &mL.px[(size_t(ty) * mL.w + tx) * mL.c];
                const float* b = &mL.px[(size_t(qy + dy) * mL.w + qx + dx) * mL.c];
                for (int k = 0; k < mL.c; ++k)
                {
                    const float e = a[k] - b[k];
                    d += e * e;
                }
            }
            if (d >= best)
                return d;
        }
        return d;
    }

    int randomValid() { return mL.valid[std::uniform_int_distribution<size_t>(0, mL.valid.size() - 1)(mRng)]; }

    /** Matches from the coarser level's, doubled; a random source patch where that lands off the source. */
    void init(const std::vector<int>* coarse, int coarseW)
    {
        mNnf.assign(size_t(mL.w) * mL.h, -1);
        mDist.assign(size_t(mL.w) * mL.h, std::numeric_limits<float>::max());
        for (int p : mTargets)
        {
            int q = -1;
            if (coarse)
            {
                const int x = p % mL.w, y = p / mL.w;
                const int cp = std::min(y / 2, int(coarse->size() / coarseW) - 1) * coarseW + std::min(x / 2, coarseW - 1);
                if (const int cq = (*coarse)[size_t(cp)]; cq >= 0)
                {
                    const int qx = (cq % coarseW) * 2 + (x & 1), qy = (cq / coarseW) * 2 + (y & 1);
                    if (qx < mL.w && qy < mL.h && mL.isValid[size_t(qy) * mL.w + qx])
                        q = qy * mL.w + qx;
                }
            }
            if (q < 0)
                q = randomValid();
            mNnf[size_t(p)] = q;
        }
    }

    void refreshDistances()
    {
        for (int p : mTargets)
            mDist[size_t(p)] = distance(p, mNnf[size_t(p)], std::numeric_limits<float>::max());
    }

    void iterate(int pass)
    {
        const bool forward = pass % 2 == 0;
        const int step = forward ? 1 : -1;
        const int n = int(mTargets.size());
        for (int i = forward ? 0 : n - 1; forward ? i < n : i >= 0; i += step)
        {
            const int p = mTargets[size_t(i)];
            const int x = p % mL.w, y = p / mL.w;
            // Propagation: the neighbour's match, shifted back by the same step.
            for (const auto [nx, ny] : {std::pair{x - step, y}, std::pair{x, y - step}})
            {
                if (nx < 0 || ny < 0 || nx >= mL.w || ny >= mL.h)
                    continue;
                const int nq = mNnf[size_t(ny) * mL.w + nx];
                if (nq < 0)
                    continue;
                const int qx = nq % mL.w + (x - nx), qy = nq / mL.w + (y - ny);
                if (qx < 0 || qy < 0 || qx >= mL.w || qy >= mL.h || !mL.isValid[size_t(qy) * mL.w + qx])
                    continue;
                tryMatch(p, qy * mL.w + qx);
            }
            // Random search in shrinking windows around the current match.
            for (int radius = std::max(mL.w, mL.h); radius >= 1; radius /= 2)
            {
                const int q = mNnf[size_t(p)];
                std::uniform_int_distribution<int> d(-radius, radius);
                const int qx = std::clamp(q % mL.w + d(mRng), 0, mL.w - 1), qy = std::clamp(q / mL.w + d(mRng), 0, mL.h - 1);
                if (mL.isValid[size_t(qy) * mL.w + qx])
                    tryMatch(p, qy * mL.w + qx);
            }
        }
    }

    /** Every hole texel becomes the average of what the patches covering it say it is. */
    void vote()
    {
        std::vector<float> acc(mL.px.size(), 0.0f), weight(size_t(mL.w) * mL.h, 0.0f);
        for (int p : mTargets)
        {
            const int q = mNnf[size_t(p)];
            const int px = p % mL.w, py = p / mL.w, qx = q % mL.w, qy = q / mL.w;
            for (int dy = -mR; dy <= mR; ++dy)
                for (int dx = -mR; dx <= mR; ++dx)
                {
                    const int tx = px + dx, ty = py + dy;
                    if (tx < 0 || ty < 0 || tx >= mL.w || ty >= mL.h || !mL.hole[size_t(ty) * mL.w + tx])
                        continue;
                    const size_t t = size_t(ty) * mL.w + tx, s = size_t(qy + dy) * mL.w + qx + dx;
                    for (int k = 0; k < mL.c; ++k)
                        acc[t * mL.c + k] += mL.px[s * mL.c + k];
                    weight[t] += 1.0f;
                }
        }
        for (size_t t = 0; t < weight.size(); ++t)
            if (weight[t] > 0.0f)
                for (int k = 0; k < mL.c; ++k)
                    mL.px[t * mL.c + k] = acc[t * mL.c + k] / weight[t];
    }

    const std::vector<int>& nnf() const { return mNnf; }

private:
    void tryMatch(int p, int q)
    {
        if (q == mNnf[size_t(p)])
            return;
        const float d = distance(p, q, mDist[size_t(p)]);
        if (d < mDist[size_t(p)])
        {
            mDist[size_t(p)] = d;
            mNnf[size_t(p)] = q;
        }
    }

    Level& mL;
    int mR;
    std::mt19937& mRng;
    std::vector<int> mTargets, mNnf;
    std::vector<float> mDist;
};
} // namespace

std::vector<image::Image> patchFill(const std::vector<image::Image>& images, std::span<const uint8_t> hole, std::span<const uint8_t> source,
                                    const PatchFillOptions& options)
{
    LILA_ZONE();
    if (images.empty())
        return {};
    const int w = images[0].width, h = images[0].height;
    for (const image::Image& im : images)
        if (im.width != w || im.height != h)
            throw std::runtime_error("Patch Fill: the images differ in size");
    if (hole.size() != size_t(w) * h || (!source.empty() && source.size() != hole.size()))
        throw std::runtime_error("Patch Fill: the hole or source mask doesn't match the image");
    const int r = std::max(1, options.patch / 2);

    std::vector<Level> levels;
    levels.reserve(8);
    levels.emplace_back();
    Level& base = levels[0];
    base.w = w;
    base.h = h;
    base.c = int(images.size()) * 4;
    base.px.resize(size_t(w) * h * base.c);
    for (size_t i = 0; i < size_t(w) * h; ++i)
        for (size_t m = 0; m < images.size(); ++m)
            for (int k = 0; k < 4; ++k)
                base.px[i * base.c + m * 4 + k] = images[m].rgba[i * 4 + k];
    base.hole.assign(hole.begin(), hole.end());
    base.source.resize(base.hole.size());
    for (size_t i = 0; i < base.hole.size(); ++i)
        base.source[i] = !base.hole[i] && (source.empty() || source[i]);
    findValid(base, r);
    if (base.valid.empty())
        throw std::runtime_error("Patch Fill: no patch fits in the source area");

    // Coarser levels while the hole stays several patches across and patches still fit.
    while (levels.size() < 8)
    {
        const Level& f = levels.back();
        if (std::min(f.w, f.h) < 8 * (2 * r + 1))
            break;
        Level c = downsample(f);
        findValid(c, r);
        if (c.valid.size() < 16)
            break;
        levels.push_back(std::move(c));
    }

    std::mt19937 rng(options.seed);
    diffuse(levels.back());
    std::vector<int> coarse;
    int coarseW = 0;
    for (int li = int(levels.size()) - 1; li >= 0; --li)
    {
        Level& l = levels[size_t(li)];
        Matcher m(l, r, rng);
        m.findTargets();
        m.init(coarse.empty() ? nullptr : &coarse, coarseW);
        if (!coarse.empty())
            m.vote();
        for (int it = 0; it < options.iterations; ++it)
        {
            m.refreshDistances();
            m.iterate(it);
            m.iterate(it + 1);
            m.vote();
        }
        coarse = m.nnf();
        coarseW = l.w;
    }

    std::vector<image::Image> out = images;
    for (size_t i = 0; i < size_t(w) * h; ++i)
        if (base.hole[i])
            for (size_t m = 0; m < images.size(); ++m)
                for (int k = 0; k < 4; ++k)
                    out[m].rgba[i * 4 + k] = base.px[i * base.c + m * 4 + k];
    return out;
}

namespace
{
using UvPolygon = std::vector<glm::vec2>;

UvPolygon polygonArg(const Args& a, const char* flag, const char* cmd)
{
    UvPolygon poly;
    if (!a.has(flag))
        return poly;
    for (const json& p : a.flags.at(flag))
    {
        if (!p.is_array() || p.size() < 2)
            throw CommandError(std::string(cmd) + ": " + flag + " is [[u, v], ...]");
        poly.push_back({p[0].get<float>(), p[1].get<float>()});
    }
    if (poly.size() < 3)
        throw CommandError(std::string(cmd) + ": " + flag + " needs at least three points");
    return poly;
}

bool insidePolygon(const UvPolygon& poly, glm::vec2 p)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i].y > p.y) != (poly[j].y > p.y) && p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x)
            in = !in;
    return in;
}

/** A mask from a uv polygon, or from an image file (white is in), at w x h. Empty when neither is given. */
std::vector<uint8_t> maskArg(const Args& a, const char* polyFlag, const char* fileFlag, int w, int h, const char* cmd)
{
    std::vector<uint8_t> m;
    const UvPolygon poly = polygonArg(a, polyFlag, cmd);
    if (!poly.empty())
    {
        m.resize(size_t(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                m[size_t(y) * w + x] = insidePolygon(poly, {(x + 0.5f) / w, 1.0f - (y + 0.5f) / h}) ? 1 : 0;
        return m;
    }
    const std::string file = a.get<std::string>(fileFlag, "");
    if (file.empty())
        return m;
    std::string err;
    image::Image img = image::read(bake::resolveFolder(file), &err);
    if (img.width <= 0)
        throw CommandError(std::string(cmd) + ": " + file + ": " + err);
    m.resize(size_t(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const int sx = std::min(img.width - 1, x * img.width / w), sy = std::min(img.height - 1, y * img.height / h);
            m[size_t(y) * w + x] = img.rgba[(size_t(sy) * img.width + sx) * 4] > 0.5f ? 1 : 0;
        }
    return m;
}

/** Each face's uv island (faces joined by edges whose uvs match on both sides) as its root face, and each texel's. */
struct Islands
{
    std::vector<int> faceRoot; // -1 for removed faces
    std::vector<int> texels;   // -1 outside every island
};

/** Corners of the face across c's edge at c's two points; kInvalid when the edge has no other face. */
std::pair<geo::Index, geo::Index> across(const geo::Mesh& m, geo::Index c)
{
    const geo::Index o = m.cornerRadial(c);
    if (o == c || o == geo::kInvalid)
        return {geo::kInvalid, geo::kInvalid};
    const geo::Index g = m.cornerFace(o);
    const geo::Index oa = m.findCorner(g, m.cornerPoint(c)), ob = m.findCorner(g, m.cornerNextPoint(c));
    if (oa == geo::kInvalid || ob == geo::kInvalid)
        return {geo::kInvalid, geo::kInvalid};
    return {oa, ob};
}

bool sameUvs(std::span<const glm::vec2> uv, const geo::Mesh& m, geo::Index c, geo::Index oa, geo::Index ob)
{
    return glm::distance(uv[c], uv[oa]) < 1e-5f && glm::distance(uv[m.cornerNext(c)], uv[ob]) < 1e-5f;
}

Islands islandMap(const geo::Mesh& m, int w, int h)
{
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    std::vector<int> parent(m.faceCapacity());
    for (size_t i = 0; i < parent.size(); ++i)
        parent[i] = int(i);
    auto root = [&](int f) {
        while (parent[size_t(f)] != f)
            f = parent[size_t(f)] = parent[size_t(parent[size_t(f)])];
        return f;
    };
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        geo::Index c = m.faceCorner(f);
        for (geo::Index k = 0; k < m.faceSize(f); ++k, c = m.cornerNext(c))
        {
            const auto [oa, ob] = across(m, c);
            if (oa != geo::kInvalid && sameUvs(uv, m, c, oa, ob))
                parent[size_t(root(int(f)))] = root(int(m.cornerFace(oa)));
        }
    }
    Islands out;
    out.faceRoot.assign(m.faceCapacity(), -1);
    out.texels.assign(size_t(w) * h, -1);
    std::vector<int>& ids = out.texels;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const int id = root(int(f));
        out.faceRoot[f] = id;
        const geo::Index c0 = m.faceCorner(f);
        for (geo::Index c1 = m.cornerNext(c0), c2 = m.cornerNext(c1); c2 != c0; c1 = c2, c2 = m.cornerNext(c2))
        {
            glm::vec2 p[3];
            const geo::Index cs[3] = {c0, c1, c2};
            for (int k = 0; k < 3; ++k)
                p[k] = {uv[cs[k]].x * w, (1.0f - uv[cs[k]].y) * h};
            const float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y);
            if (std::abs(area) < 1e-12f)
                continue;
            const int x0 = std::max(0, int(std::floor(std::min({p[0].x, p[1].x, p[2].x})))),
                      x1 = std::min(w - 1, int(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))));
            const int y0 = std::max(0, int(std::floor(std::min({p[0].y, p[1].y, p[2].y})))),
                      y1 = std::min(h - 1, int(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    const glm::vec2 q(x + 0.5f, y + 0.5f);
                    const float a1 = ((q.x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (q.y - p[0].y)) / area;
                    const float a2 = ((p[1].x - p[0].x) * (q.y - p[0].y) - (q.x - p[0].x) * (p[1].y - p[0].y)) / area;
                    if (a1 >= -1e-4f && a2 >= -1e-4f && a1 + a2 <= 1.0f + 1e-4f)
                        ids[size_t(y) * w + x] = id;
                }
        }
    }
    return out;
}

/** A texel outside a piece's islands standing in for what lies across a seam: where it samples, and on which island. */
struct Pad
{
    int texel = 0;
    glm::vec2 from{0.0f}; // image pixels
    int island = -1;
    float s = 0.0f; // texels out from the seam
};

/**
 * The texels within reach outside the islands marked in isles, across each of their seam edges: each mapped through the
 * shared 3D edge to the same distance into the face on the other side, scaled by the two edges' uv lengths.
 */
std::vector<Pad> seamPads(const geo::Mesh& m, const Islands& isl, const std::vector<uint8_t>& isles, int w, int h, float reach)
{
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    auto px = [&](glm::vec2 t) { return glm::vec2(t.x * w, (1.0f - t.y) * h); };
    auto centroid = [&](geo::Index f) {
        glm::vec2 sum(0.0f);
        geo::Index c = m.faceCorner(f);
        for (geo::Index k = 0; k < m.faceSize(f); ++k, c = m.cornerNext(c))
            sum += px(uv[c]);
        return sum / float(std::max<geo::Index>(m.faceSize(f), 1));
    };
    std::vector<Pad> best(size_t(w) * h);
    std::vector<uint8_t> has(size_t(w) * h, 0);
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f) || isl.faceRoot[f] < 0 || !isles[size_t(isl.faceRoot[f])])
            continue;
        geo::Index c = m.faceCorner(f);
        for (geo::Index k = 0; k < m.faceSize(f); ++k, c = m.cornerNext(c))
        {
            const auto [oa, ob] = across(m, c);
            if (oa == geo::kInvalid || sameUvs(uv, m, c, oa, ob))
                continue;
            const geo::Index g = m.cornerFace(oa);
            const glm::vec2 a0 = px(uv[c]), a1 = px(uv[m.cornerNext(c)]), b0 = px(uv[oa]), b1 = px(uv[ob]);
            const glm::vec2 ea = a1 - a0, eb = b1 - b0;
            const float la = glm::length(ea), lb = glm::length(eb);
            if (la < 1e-4f || lb < 1e-4f)
                continue;
            glm::vec2 na = glm::vec2(-ea.y, ea.x) / la, nb = glm::vec2(-eb.y, eb.x) / lb;
            if (glm::dot(centroid(f) - a0, na) > 0.0f)
                na = -na;
            if (glm::dot(centroid(g) - b0, nb) < 0.0f)
                nb = -nb;
            const glm::vec2 far0 = a0 + na * reach, far1 = a1 + na * reach;
            const int x0 = std::max(0, int(std::floor(std::min({a0.x, a1.x, far0.x, far1.x})))),
                      x1 = std::min(w - 1, int(std::ceil(std::max({a0.x, a1.x, far0.x, far1.x}))));
            const int y0 = std::max(0, int(std::floor(std::min({a0.y, a1.y, far0.y, far1.y})))),
                      y1 = std::min(h - 1, int(std::ceil(std::max({a0.y, a1.y, far0.y, far1.y}))));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    const size_t i = size_t(y) * w + x;
                    if (isl.texels[i] >= 0 && isles[size_t(isl.texels[i])])
                        continue;
                    const glm::vec2 d = glm::vec2(x + 0.5f, y + 0.5f) - a0;
                    const float t = glm::dot(d, ea) / (la * la), s = glm::dot(d, na);
                    if (t < 0.0f || t > 1.0f || s <= 0.0f || s > reach || (has[i] && best[i].s <= s))
                        continue;
                    has[i] = 1;
                    best[i] = {int(i), b0 + eb * t + nb * (s * lb / la), isl.faceRoot[g], s};
                }
        }
    }
    std::vector<Pad> out;
    for (size_t i = 0; i < best.size(); ++i)
        if (has[i])
            out.push_back(best[i]);
    return out;
}

/** The image at pixel position p from the texels of island that are filled, bilinear; false when none is near. */
bool sampleIsland(const image::Image& img, const Islands& isl, const std::vector<uint8_t>& pending, int island, glm::vec2 p, float* out)
{
    const int w = img.width, h = img.height;
    const float fx = p.x - 0.5f, fy = p.y - 0.5f;
    const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    const float tx = fx - float(x0), ty = fy - float(y0);
    float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f}, wsum = 0.0f;
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx)
        {
            const int x = x0 + dx, y = y0 + dy;
            if (x < 0 || y < 0 || x >= w || y >= h)
                continue;
            const size_t i = size_t(y) * w + x;
            if (isl.texels[i] != island || pending[i])
                continue;
            const float wt = (dx ? tx : 1.0f - tx) * (dy ? ty : 1.0f - ty) + 1e-6f;
            for (int k = 0; k < 4; ++k)
                sum[k] += img.rgba[i * 4 + size_t(k)] * wt;
            wsum += wt;
        }
    if (wsum <= 0.0f)
        return false;
    for (int k = 0; k < 4; ++k)
        out[k] = sum[k] / wsum;
    return true;
}

/** The hole's 4-connected pieces, as texel lists. */
std::vector<std::vector<int>> components(const std::vector<uint8_t>& hole, int w, int h)
{
    std::vector<std::vector<int>> out;
    std::vector<uint8_t> seen(hole.size(), 0);
    for (int i = 0; i < w * h; ++i)
    {
        if (!hole[size_t(i)] || seen[size_t(i)])
            continue;
        std::vector<int> piece, stack = {i};
        seen[size_t(i)] = 1;
        while (!stack.empty())
        {
            const int p = stack.back();
            stack.pop_back();
            piece.push_back(p);
            const int x = p % w, y = p / w;
            for (const auto [nx, ny] : {std::pair{x - 1, y}, std::pair{x + 1, y}, std::pair{x, y - 1}, std::pair{x, y + 1}})
                if (nx >= 0 && ny >= 0 && nx < w && ny < h && hole[size_t(ny) * w + nx] && !seen[size_t(ny) * w + nx])
                {
                    seen[size_t(ny) * w + nx] = 1;
                    stack.push_back(ny * w + nx);
                }
        }
        out.push_back(std::move(piece));
    }
    return out;
}

image::Image crop(const image::Image& img, int x0, int y0, int x1, int y1)
{
    image::Image out;
    out.width = x1 - x0 + 1;
    out.height = y1 - y0 + 1;
    out.rgba.resize(size_t(out.width) * out.height * 4);
    for (int y = 0; y < out.height; ++y)
        std::copy_n(&img.rgba[(size_t(y + y0) * img.width + x0) * 4], size_t(out.width) * 4, &out.rgba[size_t(y) * out.width * 4]);
    return out;
}

/** A file stem under folder no earlier Patch Fill of the set used. */
std::string freshStem(const std::filesystem::path& folder, const std::string& set)
{
    for (int n = 1;; ++n)
    {
        const std::string stem = set + "_patch" + std::to_string(n);
        if (!std::filesystem::exists(folder / (stem + "_mask.png")))
            return stem;
    }
}

json patchFillCommand(const Args& a)
{
    const char* cmd = "texPatchFill";
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError(std::string(cmd) + ": texture set required");
    const std::string setName = a.positional[0].get<std::string>();
    scene::Graph& g = scene::Graph::current();
    scene::Node* set = findSet(g, setName);
    if (!set)
        throw CommandError(std::string(cmd) + ": no texture set " + setName);
    const SetSettings settings = readSettings(*set);
    const Tile& tile = settings.tiles.front();
    const int w = std::min(tile.width > 0 ? tile.width : settings.width, 2048), h = std::min(tile.height > 0 ? tile.height : settings.height, 2048);

    const std::vector<uint8_t> hole = maskArg(a, "holeUv", "hole", w, h, cmd);
    if (hole.empty())
        throw CommandError(std::string(cmd) + ": give holeUv= (a uv polygon) or hole= (a mask image)");
    const std::vector<uint8_t> allowed = maskArg(a, "sourceUv", "source", w, h, cmd);
    scene::MeshPtr mesh;
    if (const std::string meshPath = a.get<std::string>("mesh", ""); !meshPath.empty())
    {
        g.evaluate();
        scene::Node* n = g.find(meshPath);
        mesh = n && n->type().isA("mesh") ? n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh) : nullptr;
        if (!mesh || mesh->cornerAttrs().get<glm::vec2>(geo::attr::uvName(0)).empty())
            throw CommandError(std::string(cmd) + ": " + meshPath + " isn't a mesh with uvs");
    }

    std::vector<std::string> names;
    std::vector<image::Image> filled;
    for (auto& [name, img] : evaluateOutputs(*set, channelRequests(readGraph(*set), settings), tile.udim, w, h))
    {
        names.push_back(name);
        filled.push_back(std::move(img));
    }
    if (filled.empty())
        throw CommandError(std::string(cmd) + ": the set has no channels");

    PatchFillOptions o;
    o.patch = int(a.get<int64_t>("patch", 7)) | 1;
    o.seed = uint32_t(a.get<int64_t>("seed", 1));
    const size_t pieces = components(hole, w, h).size();
    try
    {
        filled = patchFillUv(std::move(filled), hole, allowed, mesh.get(), a.get<bool>("acrossSeams", true), o);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what() + "; the area around the hole is too small for patch=" + std::to_string(o.patch));
    }

    const std::filesystem::path folder = bake::resolveFolder(a.get<std::string>("folder", "textures/" + set->name()));
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::string stem = freshStem(folder, set->name());
    json material = json::object();
    for (size_t m = 0; m < names.size(); ++m)
    {
        const std::filesystem::path f = folder / (stem + "_" + names[m] + ".exr");
        image::writeExr(f, w, h, filled[m].rgba, 32);
        material[names[m]] = paths::toUtf8(f);
    }
    std::vector<float> mask(size_t(w) * h * 4);
    for (size_t i = 0; i < hole.size(); ++i)
        for (int k = 0; k < 4; ++k)
            mask[i * 4 + k] = k == 3 || hole[i] ? 1.0f : 0.0f;
    const std::filesystem::path maskFile = folder / (stem + "_mask.png");
    image::writePng(maskFile, w, h, mask, 8);

    const std::string id = ops::execute("texAddLayer", {{"args", {setName, "fill"}}, {"name", a.get<std::string>("name", "Patch Fill")}, {"material", material}})
                               .at("id")
                               .get<std::string>();
    ops::execute("texAddMask", {{"args", {setName, id, "paint"}}, {"image", paths::toUtf8(maskFile)}});
    return {{"id", id}, {"pieces", pieces}, {"mask", paths::toUtf8(maskFile)}, {"channels", material}};
}
} // namespace

std::vector<image::Image> patchFillUv(std::vector<image::Image> images, std::span<const uint8_t> hole, std::span<const uint8_t> source,
                                      const geo::Mesh* mesh, bool acrossSeams, const PatchFillOptions& o)
{
    LILA_ZONE();
    if (images.empty())
        return images;
    const int w = images[0].width, h = images[0].height;
    if (hole.size() != size_t(w) * h || (!source.empty() && source.size() != hole.size()))
        throw std::runtime_error("Patch Fill: the hole or source mask doesn't match the image");
    Islands isl;
    if (mesh)
        isl = islandMap(*mesh, w, h);
    const std::vector<int>& islands = isl.texels;
    int islandCount = 0;
    for (int id : isl.faceRoot)
        islandCount = std::max(islandCount, id + 1);
    // Room for a few patches across the seam: what the matching sees beyond the island's edge.
    const float reach = float(std::max(3 * o.patch, 12));
    // Hole texels not filled yet; a piece filled earlier is content for the pieces after it, across seams too.
    std::vector<uint8_t> pending(hole.begin(), hole.end());
    for (const std::vector<int>& piece : components(pending, w, h))
    {
        // The piece's source: the islands it sits on (all of uv space without a mesh), less the hole, within source if given.
        std::vector<uint8_t> isles(size_t(islandCount), 0);
        for (int p : piece)
            if (!islands.empty() && islands[size_t(p)] >= 0)
                isles[size_t(islands[size_t(p)])] = 1;
        std::vector<uint8_t> src(hole.size(), 0);
        int x0 = w, y0 = h, x1 = -1, y1 = -1;
        auto grow = [&](int i) {
            x0 = std::min(x0, i % w);
            x1 = std::max(x1, i % w);
            y0 = std::min(y0, i / w);
            y1 = std::max(y1, i / w);
        };
        for (int i = 0; i < w * h; ++i)
        {
            const bool onIsland = islands.empty() || (islands[size_t(i)] >= 0 && isles[size_t(islands[size_t(i)])]);
            src[size_t(i)] = !pending[size_t(i)] && onIsland && (source.empty() || source[size_t(i)]) ? 1 : 0;
            if (src[size_t(i)])
                grow(i);
        }
        for (int p : piece)
            grow(p);

        // Across seams: texels past the islands' seam edges take what the far side holds; a far side still in the hole is
        // filled with the piece (unknown), so both sides match together, but only the piece is written back.
        struct Known
        {
            int texel;
            bool known;
            std::vector<float> values;
        };
        std::vector<Known> padded;
        if (mesh && acrossSeams)
        {
            std::vector<uint8_t> inPiece(hole.size(), 0);
            for (int p : piece)
                inPiece[size_t(p)] = 1;
            for (const Pad& pad : seamPads(*mesh, isl, isles, w, h, reach))
            {
                if (inPiece[size_t(pad.texel)])
                    continue;
                Known k{pad.texel, true, std::vector<float>(images.size() * 4, 0.0f)};
                const int nx = std::clamp(int(std::floor(pad.from.x)), 0, w - 1), ny = std::clamp(int(std::floor(pad.from.y)), 0, h - 1);
                const size_t near = size_t(ny) * w + nx;
                if (pending[near] && islands[near] == pad.island)
                    k.known = false;
                else
                    for (size_t m = 0; m < images.size() && k.known; ++m)
                        k.known = sampleIsland(images[m], isl, pending, pad.island, pad.from, &k.values[m * 4]);
                if (!k.known && !(pending[near] && islands[near] == pad.island))
                    continue;
                padded.push_back(std::move(k));
                grow(pad.texel);
            }
        }

        const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
        auto local = [&](int i) { return size_t(i / w - y0) * cw + size_t(i % w - x0); };
        std::vector<image::Image> cropped;
        for (const image::Image& im : images)
            cropped.push_back(crop(im, x0, y0, x1, y1));
        std::vector<uint8_t> cHole(size_t(cw) * ch, 0), cSrc(size_t(cw) * ch, 0);
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x)
                cSrc[size_t(y) * cw + x] = src[size_t(y + y0) * w + x + x0];
        for (const Known& k : padded)
        {
            const size_t li = local(k.texel);
            if (k.known)
            {
                for (size_t m = 0; m < images.size(); ++m)
                    std::copy_n(&k.values[m * 4], 4, &cropped[m].rgba[li * 4]);
                cSrc[li] = source.empty() || source[size_t(k.texel)] ? 1 : 0;
            }
            else
            {
                cHole[li] = 1;
                cSrc[li] = 0;
            }
        }
        for (int p : piece)
        {
            cHole[local(p)] = 1;
            cSrc[local(p)] = 0;
        }
        const std::vector<image::Image> out = patchFill(cropped, cHole, cSrc, o);
        for (size_t m = 0; m < images.size(); ++m)
            for (int p : piece)
                std::copy_n(&out[m].rgba[local(p) * 4], 4, &images[m].rgba[size_t(p) * 4]);
        for (int p : piece)
            pending[size_t(p)] = 0;
    }
    return images;
}

void registerPatchFillCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "texPatchFill",
        .doc = "Patch Fill a texture set in uv space: the hole (holeUv, a uv polygon, or hole, a mask image) is rebuilt from patches of its "
               "surroundings, every channel matched together, and lands as a new fill layer masked to the hole. With mesh=, each piece of the "
               "hole draws only from the uv islands it sits on, and sees across their seams to the content on the other side. Returns {id, pieces, mask, channels}.",
        .positionalDoc = "set",
        .flags = {{"holeUv", "", T::List, nullptr, "The hole: a polygon in uv space, [[u, v], ...]."},
                  {"hole", "", T::String, nullptr, "The hole: a mask image, white to fill."},
                  {"sourceUv", "", T::List, nullptr, "Only take patches from inside this uv polygon."},
                  {"source", "", T::String, nullptr, "Only take patches from where this mask image is white."},
                  {"mesh", "", T::String, nullptr, "The set's mesh: keeps each piece of the hole to its own uv islands."},
                  {"acrossSeams", "", T::Bool, true, "With mesh: match across uv seams, each island padded with what lies on the other side."},
                  {"patch", "", T::Int, 7, "Patch side in texels (odd).", {}, {}, 3.0, 31.0},
                  {"seed", "", T::Int, 1, "Random seed."},
                  {"name", "", T::String, "Patch Fill", "Layer name."},
                  {"folder", "", T::String, nullptr, "Where the filled images go; textures/<set> under the workspace by default."}},
        .undoable = false,
        .run = patchFillCommand,
    });
}
} // namespace lila::tex
