#include "pointcloud/Octree.h"

#include "core/Profile.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>

namespace lila::pointcloud
{
namespace fs = std::filesystem;

namespace
{
constexpr char kMagic[8] = {'L', 'I', 'L', 'A', 'P', 'C', '\0', '\1'};
constexpr uint32_t kVersion = 1;
/** Points a node keeps without splitting further. */
constexpr size_t kLeafMax = 40000;
constexpr int kMaxDepth = 24;
constexpr size_t kBatch = 1 << 18;

/** A point on its way into the tree: cloud-local cm, double, so no cube of any size loses precision. */
struct WorkPoint
{
    glm::dvec3 p;
    uint8_t rgb[3];
    uint8_t classification;
    uint16_t intensity;
    uint16_t pad;
};
static_assert(sizeof(WorkPoint) == 32);

std::FILE* openFile(const fs::path& p, const char* mode)
{
#ifdef _WIN32
    std::wstring m(mode, mode + std::strlen(mode));
    std::FILE* f = _wfopen(p.c_str(), m.c_str());
#else
    std::FILE* f = std::fopen(p.c_str(), mode);
#endif
    if (!f)
        throw std::runtime_error("can't open " + p.string());
    return f;
}

void seek(std::FILE* f, uint64_t at)
{
#ifdef _WIN32
    _fseeki64(f, int64_t(at), SEEK_SET);
#else
    fseeko(f, off_t(at), SEEK_SET);
#endif
}

struct TreeNode
{
    glm::dvec3 min{0.0};
    double size = 0.0;
    uint64_t offset = 0;
    uint32_t count = 0;
    std::array<int, 8> children{-1, -1, -1, -1, -1, -1, -1, -1};
    uint8_t depth = 0;
};

class Builder
{
public:
    Builder(const BuildOptions& o, const fs::path& temp, uint64_t total) : mOptions(o), mTemp(temp), mTotal(std::max<uint64_t>(total, 1))
    {
        mData = openFile(mTemp / "nodes.bin", "wb");
    }
    ~Builder()
    {
        if (mData)
            std::fclose(mData);
    }

    /** The tree over points already in memory. */
    int build(std::vector<WorkPoint>&& points, const glm::dvec3& min, double size, int depth)
    {
        if (depth == 0 || points.size() > kLeafMax)
        {
            // Shuffled once at the top of an in-memory cube, so each grid cell keeps a random point, not the first scanned.
            std::mt19937_64 rng(uint64_t(points.size()) * 0x9E3779B97F4A7C15ull + uint64_t(depth));
            std::shuffle(points.begin(), points.end(), rng);
        }
        return split(std::move(points), min, size, depth);
    }

    /** The tree over a temporary file of count points: sampled and split through child files while it is too big for memory. */
    int buildFile(const fs::path& file, uint64_t count, const glm::dvec3& min, double size, int depth)
    {
        if (count <= mOptions.inMemory || depth >= kMaxDepth)
        {
            std::vector<WorkPoint> pts(count);
            std::FILE* f = openFile(file, "rb");
            const size_t got = std::fread(pts.data(), sizeof(WorkPoint), pts.size(), f);
            std::fclose(f);
            fs::remove(file);
            pts.resize(got);
            return build(std::move(pts), min, size, depth);
        }
        LILA_ZONE_N("pointcloud split file");
        const int index = int(mTree.size());
        mTree.push_back(node(min, size, depth));
        std::vector<uint64_t> occupied(size_t(kGrid) * kGrid * kGrid / 64, 0);
        std::vector<WorkPoint> kept;
        std::array<std::FILE*, 8> children{};
        std::array<uint64_t, 8> childCount{};
        std::array<fs::path, 8> childPath;
        std::array<std::vector<WorkPoint>, 8> buffers;
        auto flush = [&](int o) {
            if (buffers[o].empty())
                return;
            if (!children[o])
            {
                childPath[o] = mTemp / ("b" + std::to_string(mNextFile++) + ".bin");
                children[o] = openFile(childPath[o], "wb");
            }
            std::fwrite(buffers[o].data(), sizeof(WorkPoint), buffers[o].size(), children[o]);
            buffers[o].clear();
        };
        std::FILE* f = openFile(file, "rb");
        std::vector<WorkPoint> batch(kBatch);
        while (size_t n = std::fread(batch.data(), sizeof(WorkPoint), batch.size(), f))
        {
            for (size_t i = 0; i < n; ++i)
            {
                const glm::ivec3 c = cell(batch[i].p, min, size);
                const size_t k = size_t(c.x) + size_t(kGrid) * (size_t(c.y) + size_t(kGrid) * size_t(c.z));
                if (!(occupied[k / 64] & (1ull << (k % 64))))
                {
                    occupied[k / 64] |= 1ull << (k % 64);
                    kept.push_back(batch[i]);
                    continue;
                }
                const int o = (c.x >= kGrid / 2 ? 1 : 0) | (c.y >= kGrid / 2 ? 2 : 0) | (c.z >= kGrid / 2 ? 4 : 0);
                buffers[o].push_back(batch[i]);
                ++childCount[o];
                if (buffers[o].size() >= kBatch)
                    flush(o);
            }
            tick(0);
        }
        std::fclose(f);
        fs::remove(file);
        write(index, kept);
        kept = {};
        for (int o = 0; o < 8; ++o)
        {
            flush(o);
            if (children[o])
                std::fclose(children[o]);
        }
        for (int o = 0; o < 8; ++o)
            if (childCount[o])
                mTree[index].children[o] = buildFile(childPath[o], childCount[o], childMin(min, size, o), size * 0.5, depth + 1);
        return index;
    }

    /** Writes the cache: header, node table in breadth-first order with each node's children together, then the points. */
    BuildStats finish(const fs::path& out, int root, const nlohmann::json& meta)
    {
        LILA_ZONE();
        std::fclose(mData);
        mData = nullptr;
        std::vector<OctreeNode> table;
        std::vector<int> order{root};
        table.reserve(mTree.size());
        int depth = 0;
        for (size_t i = 0; i < order.size(); ++i)
        {
            const TreeNode& t = mTree[size_t(order[i])];
            OctreeNode n;
            n.min = t.min;
            n.size = t.size;
            n.offset = t.offset;
            n.count = t.count;
            n.depth = t.depth;
            n.firstChild = uint32_t(order.size());
            for (int o = 0; o < 8; ++o)
                if (t.children[o] >= 0)
                {
                    n.childMask |= uint8_t(1u << o);
                    order.push_back(t.children[o]);
                }
            if (!n.childMask)
                n.firstChild = 0;
            depth = std::max(depth, int(t.depth));
            table.push_back(n);
        }
        nlohmann::json header = meta;
        header["points"] = mWritten;
        const std::string text = header.dump();
        const uint64_t base = sizeof(kMagic) + 4 + 4 + text.size() + 8 + table.size() * sizeof(OctreeNode);
        for (OctreeNode& n : table)
            n.offset += base;
        const fs::path part = fs::path(out).concat(".part");
        std::FILE* f = openFile(part, "wb");
        const uint32_t jsonBytes = uint32_t(text.size());
        const uint64_t nodeCount = table.size();
        std::fwrite(kMagic, 1, sizeof(kMagic), f);
        std::fwrite(&kVersion, 4, 1, f);
        std::fwrite(&jsonBytes, 4, 1, f);
        std::fwrite(text.data(), 1, text.size(), f);
        std::fwrite(&nodeCount, 8, 1, f);
        std::fwrite(table.data(), sizeof(OctreeNode), table.size(), f);
        std::FILE* d = openFile(mTemp / "nodes.bin", "rb");
        std::vector<char> buf(1 << 22);
        while (size_t n = std::fread(buf.data(), 1, buf.size(), d))
            std::fwrite(buf.data(), 1, n, f);
        std::fclose(d);
        const bool ok = std::ferror(f) == 0;
        std::fclose(f);
        if (!ok)
            throw std::runtime_error("writing " + out.string() + " failed (disk full?)");
        std::error_code ec;
        fs::remove(out, ec);
        fs::rename(part, out);
        return {mWritten, table.size(), depth, 0.0};
    }

    void tick(uint64_t added)
    {
        mProgressPoints += added;
        if (mOptions.progress && !mOptions.progress(0.5 + 0.5 * std::min(1.0, double(mWritten) / double(mTotal))))
            throw std::runtime_error("cancelled");
    }

    static glm::ivec3 cell(const glm::dvec3& p, const glm::dvec3& min, double size)
    {
        return glm::clamp(glm::ivec3(glm::floor((p - min) / size * double(kGrid))), glm::ivec3(0), glm::ivec3(kGrid - 1));
    }

    static TreeNode node(const glm::dvec3& min, double size, int depth)
    {
        TreeNode t;
        t.min = min;
        t.size = size;
        t.depth = uint8_t(depth);
        return t;
    }

    static glm::dvec3 childMin(const glm::dvec3& min, double size, int o)
    {
        const double h = size * 0.5;
        return min + glm::dvec3((o & 1) ? h : 0.0, (o & 2) ? h : 0.0, (o & 4) ? h : 0.0);
    }

private:
    int split(std::vector<WorkPoint>&& points, const glm::dvec3& min, double size, int depth)
    {
        const int index = int(mTree.size());
        mTree.push_back(node(min, size, depth));
        if (points.size() <= kLeafMax || depth >= kMaxDepth)
        {
            write(index, points);
            return index;
        }
        std::vector<uint64_t> occupied(size_t(kGrid) * kGrid * kGrid / 64, 0);
        std::vector<WorkPoint> kept;
        std::array<std::vector<WorkPoint>, 8> children;
        for (const WorkPoint& w : points)
        {
            const glm::ivec3 c = cell(w.p, min, size);
            const size_t k = size_t(c.x) + size_t(kGrid) * (size_t(c.y) + size_t(kGrid) * size_t(c.z));
            if (!(occupied[k / 64] & (1ull << (k % 64))))
            {
                occupied[k / 64] |= 1ull << (k % 64);
                kept.push_back(w);
                continue;
            }
            children[(c.x >= kGrid / 2 ? 1 : 0) | (c.y >= kGrid / 2 ? 2 : 0) | (c.z >= kGrid / 2 ? 4 : 0)].push_back(w);
        }
        points = {};
        occupied = {};
        write(index, kept);
        kept = {};
        for (int o = 0; o < 8; ++o)
            if (!children[o].empty())
                mTree[index].children[o] = split(std::move(children[o]), childMin(min, size, o), size * 0.5, depth + 1);
        return index;
    }

    void write(int index, const std::vector<WorkPoint>& points)
    {
        TreeNode& t = mTree[size_t(index)];
        t.offset = mDataBytes;
        t.count = uint32_t(points.size());
        std::vector<PackedPoint> packed(points.size());
        const double k = 65535.0 / t.size;
        for (size_t i = 0; i < points.size(); ++i)
        {
            const glm::dvec3 q = glm::clamp(glm::round((points[i].p - t.min) * k), glm::dvec3(0.0), glm::dvec3(65535.0));
            PackedPoint& o = packed[i];
            o.x = uint16_t(q.x);
            o.y = uint16_t(q.y);
            o.z = uint16_t(q.z);
            o.r = points[i].rgb[0];
            o.g = points[i].rgb[1];
            o.b = points[i].rgb[2];
            o.classification = points[i].classification;
            o.intensity = points[i].intensity;
        }
        std::fwrite(packed.data(), sizeof(PackedPoint), packed.size(), mData);
        mDataBytes += packed.size() * sizeof(PackedPoint);
        mWritten += points.size();
        tick(0);
    }

    const BuildOptions& mOptions;
    fs::path mTemp;
    uint64_t mTotal;
    std::FILE* mData = nullptr;
    uint64_t mDataBytes = 0;
    uint64_t mWritten = 0;
    uint64_t mProgressPoints = 0;
    int mNextFile = 0;

public:
    std::vector<TreeNode> mTree;
};

WorkPoint work(const RawPoint& r, const glm::dmat4& m)
{
    WorkPoint w{};
    w.p = glm::dvec3(m * glm::dvec4(r.position, 1.0));
    std::memcpy(w.rgb, r.rgb, 3);
    w.classification = r.classification;
    w.intensity = r.intensity;
    return w;
}
} // namespace

BuildStats buildOctree(PointSource& source, const fs::path& out, const BuildOptions& o)
{
    LILA_ZONE();
    const auto start = std::chrono::steady_clock::now();
    const SourceInfo& info = source.info();
    auto report = [&](double f) {
        if (o.progress && !o.progress(f))
            throw std::runtime_error("cancelled");
    };

    // Bounds in cloud-local space: the file's own, carried through toLocal, or a pass over the points.
    glm::dvec3 lo(1e300), hi(-1e300);
    std::vector<RawPoint> batch;
    if (info.bounds)
        for (int c = 0; c < 8; ++c)
        {
            const glm::dvec3 a = info.bounds->first, b = info.bounds->second;
            const glm::dvec3 p((c & 1) ? b.x : a.x, (c & 2) ? b.y : a.y, (c & 4) ? b.z : a.z);
            const glm::dvec3 q(o.toLocal * glm::dvec4(p, 1.0));
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
        }
    else
    {
        LILA_ZONE_N("pointcloud bounds pass");
        uint64_t seen = 0;
        while (source.next(batch, kBatch))
        {
            for (const RawPoint& r : batch)
            {
                const glm::dvec3 q(o.toLocal * glm::dvec4(r.position, 1.0));
                lo = glm::min(lo, q);
                hi = glm::max(hi, q);
            }
            seen += batch.size();
            report(0.25 * double(seen) / double(std::max<uint64_t>(info.count, 1)));
        }
        source.rewind();
    }
    if (!(lo.x <= hi.x))
        throw std::runtime_error("the file has no points");
    const glm::dvec3 extent = hi - lo;
    const double size = std::max(std::max({extent.x, extent.y, extent.z}) * 1.0001, 1.0);
    const glm::dvec3 min = lo - glm::dvec3(size * 0.00005);

    const fs::path temp = o.tempDir.empty() ? fs::path(out).concat(".build") : o.tempDir;
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::create_directories(temp);
    Builder b(o, temp, info.count);
    int root = 0;
    {
        // Small clouds go straight into memory; large ones through a file of work points first.
        uint64_t read = 0;
        if (info.count <= o.inMemory)
        {
            std::vector<WorkPoint> pts;
            pts.reserve(size_t(info.count));
            while (source.next(batch, kBatch))
            {
                for (const RawPoint& r : batch)
                    pts.push_back(work(r, o.toLocal));
                read += batch.size();
                report(0.25 + 0.25 * double(read) / double(std::max<uint64_t>(info.count, 1)));
            }
            root = b.build(std::move(pts), min, size, 0);
        }
        else
        {
            const fs::path all = temp / "all.bin";
            std::FILE* f = openFile(all, "wb");
            std::vector<WorkPoint> w;
            while (source.next(batch, kBatch))
            {
                w.clear();
                for (const RawPoint& r : batch)
                    w.push_back(work(r, o.toLocal));
                std::fwrite(w.data(), sizeof(WorkPoint), w.size(), f);
                read += batch.size();
                report(0.25 + 0.25 * double(read) / double(std::max<uint64_t>(info.count, 1)));
            }
            std::fclose(f);
            root = b.buildFile(all, read, min, size, 0);
        }
    }
    nlohmann::json meta = o.metadata;
    meta["color"] = info.color;
    meta["intensity"] = info.intensity;
    meta["classification"] = info.classification;
    meta["format"] = info.format;
    meta["boundsMin"] = {lo.x, lo.y, lo.z};
    meta["boundsMax"] = {hi.x, hi.y, hi.z};
    BuildStats stats = b.finish(out, root, meta);
    fs::remove_all(temp, ec);
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return stats;
}

PointCloudFile::PointCloudFile(const fs::path& path) : mPath(path)
{
    mFile = openFile(path, "rb");
    char magic[8];
    uint32_t version = 0, jsonBytes = 0;
    if (std::fread(magic, 1, 8, mFile) != 8 || std::memcmp(magic, kMagic, 8) != 0 || std::fread(&version, 4, 1, mFile) != 1 ||
        version != kVersion || std::fread(&jsonBytes, 4, 1, mFile) != 1)
    {
        std::fclose(mFile);
        mFile = nullptr;
        throw std::runtime_error(path.filename().string() + " is not a Lila point cloud cache (or an older one)");
    }
    std::string text(jsonBytes, '\0');
    uint64_t count = 0;
    std::fread(text.data(), 1, jsonBytes, mFile);
    std::fread(&count, 8, 1, mFile);
    mMeta = nlohmann::json::parse(text, nullptr, false);
    mNodes.resize(size_t(count));
    if (std::fread(mNodes.data(), sizeof(OctreeNode), mNodes.size(), mFile) != mNodes.size())
    {
        std::fclose(mFile);
        mFile = nullptr;
        throw std::runtime_error(path.filename().string() + " is cut short");
    }
    mPoints = mMeta.value("points", uint64_t(0));
}

PointCloudFile::~PointCloudFile()
{
    if (mFile)
        std::fclose(mFile);
}

std::vector<PackedPoint> PointCloudFile::read(uint32_t node) const
{
    LILA_ZONE();
    const OctreeNode& n = mNodes.at(node);
    std::vector<PackedPoint> out(n.count);
    std::scoped_lock lock(mMutex);
    seek(mFile, n.offset);
    out.resize(std::fread(out.data(), sizeof(PackedPoint), out.size(), mFile));
    return out;
}
} // namespace lila::pointcloud
