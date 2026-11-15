#include "tex/PainterFile.h"

#include "core/Paths.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>

namespace lila::tex::painter
{
namespace fs = std::filesystem;

namespace
{
[[noreturn]] void fail(const std::string& what)
{
    throw std::runtime_error(what);
}

std::vector<uint8_t> readAll(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in)
        fail("can't open " + paths::toUtf8(file));
    const std::streamsize size = in.tellg();
    std::vector<uint8_t> b(size_t(std::max<std::streamsize>(size, 0)));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(b.data()), size))
        fail("can't read " + paths::toUtf8(file));
    return b;
}

// HDF5 -------------------------------------------------------------------------------------------------------------

/** Enough of HDF5 for Painter's files: superblock 0/1, version 1 object headers, symbol table groups. */
class Hdf5
{
public:
    explicit Hdf5(const std::vector<uint8_t>& b) : mB(b)
    {
        static const uint8_t sig[8] = {0x89, 'H', 'D', 'F', '\r', '\n', 0x1a, '\n'};
        uint64_t sb = std::numeric_limits<uint64_t>::max();
        for (uint64_t at = 0; at + 8 <= mB.size(); at = at ? at * 2 : 512)
            if (std::memcmp(mB.data() + at, sig, 8) == 0)
            {
                sb = at;
                break;
            }
        if (sb == std::numeric_limits<uint64_t>::max())
            fail("not an HDF5 file");
        const int version = u8(sb + 8);
        if (version > 1)
            fail("HDF5 superblock version " + std::to_string(version) + " isn't supported");
        mO = u8(sb + 13);
        mL = u8(sb + 14);
        if ((mO != 4 && mO != 8) || (mL != 4 && mL != 8))
            fail("unexpected HDF5 offset sizes");
        uint64_t p = sb + 24 + (version == 1 ? 4 : 0);
        mBase = off(p);
        p += 4ull * mO;
        mRoot = p;
    }

    std::map<std::string, std::vector<uint8_t>> datasets()
    {
        std::map<std::string, std::vector<uint8_t>> out;
        walkEntry(mRoot, "", out, 0);
        return out;
    }

private:
    uint8_t u8(uint64_t at) const
    {
        need(at, 1);
        return mB[size_t(at)];
    }
    uint64_t le(uint64_t at, int n) const
    {
        need(at, uint64_t(n));
        uint64_t v = 0;
        for (int i = n - 1; i >= 0; --i)
            v = (v << 8) | mB[size_t(at) + size_t(i)];
        return v;
    }
    uint64_t off(uint64_t at) const { return le(at, mO); }
    uint64_t len(uint64_t at) const { return le(at, mL); }
    bool undefined(uint64_t a) const { return mO == 8 ? a == std::numeric_limits<uint64_t>::max() : a == 0xffffffffull; }
    uint64_t addr(uint64_t a) const { return mBase + a; }
    void need(uint64_t at, uint64_t n) const
    {
        if (at > mB.size() || n > mB.size() - at)
            fail("HDF5 file is truncated or damaged");
    }
    void sig(uint64_t at, const char* s) const
    {
        need(at, 4);
        if (std::memcmp(mB.data() + at, s, 4) != 0)
            fail(std::string("HDF5 file is damaged (no ") + s + ")");
    }

    struct Message
    {
        uint16_t type;
        uint64_t at;
        uint16_t size;
    };

    std::vector<Message> messages(uint64_t header) const
    {
        if (u8(header) != 1)
            fail("HDF5 object header version " + std::to_string(u8(header)) + " isn't supported");
        const uint64_t count = le(header + 2, 2);
        std::vector<std::pair<uint64_t, uint64_t>> blocks{{header + 16, le(header + 8, 4)}};
        std::vector<Message> out;
        for (size_t bi = 0; bi < blocks.size() && out.size() < count; ++bi)
        {
            uint64_t p = blocks[bi].first;
            const uint64_t end = p + blocks[bi].second;
            while (p + 8 <= end && out.size() < count)
            {
                Message m{uint16_t(le(p, 2)), p + 8, uint16_t(le(p + 2, 2))};
                need(m.at, m.size);
                if (m.type == 0x10)
                    blocks.emplace_back(addr(off(m.at)), len(m.at + mO));
                out.push_back(m);
                p = m.at + m.size;
            }
        }
        return out;
    }

    void walkEntry(uint64_t entry, const std::string& path, std::map<std::string, std::vector<uint8_t>>& out, int depth)
    {
        if (depth > 32)
            fail("HDF5 groups nest too deep");
        const uint64_t header = addr(off(entry + mO));
        uint64_t btree = 0, heap = 0;
        bool group = false;
        if (le(entry + 2ull * mO, 4) == 1)
        {
            btree = addr(off(entry + 2ull * mO + 8));
            heap = addr(off(entry + 2ull * mO + 8 + mO));
            group = true;
        }
        std::vector<Message> msgs;
        if (!group)
        {
            msgs = messages(header);
            for (const Message& m : msgs)
                if (m.type == 0x11)
                {
                    btree = addr(off(m.at));
                    heap = addr(off(m.at + mO));
                    group = true;
                }
        }
        if (group)
        {
            sig(heap, "HEAP");
            const uint64_t names = addr(off(heap + 8 + 2ull * mL));
            walkTree(btree, names, path, out, depth);
            return;
        }
        out[path] = readDataset(msgs, path);
    }

    void walkTree(uint64_t node, uint64_t names, const std::string& path, std::map<std::string, std::vector<uint8_t>>& out, int depth)
    {
        sig(node, "TREE");
        const int level = u8(node + 5);
        const uint64_t used = le(node + 6, 2);
        uint64_t p = node + 8 + 2ull * mO + mL;
        for (uint64_t i = 0; i < used; ++i, p += mO + mL)
        {
            const uint64_t child = addr(off(p));
            if (level > 0)
            {
                walkTree(child, names, path, out, depth + 1);
                continue;
            }
            sig(child, "SNOD");
            const uint64_t n = le(child + 6, 2);
            const uint64_t entrySize = 2ull * mO + 24;
            for (uint64_t k = 0; k < n; ++k)
            {
                const uint64_t e = child + 8 + k * entrySize;
                const uint64_t name = names + off(e);
                std::string s;
                for (uint64_t c = name; u8(c) != 0 && s.size() < 4096; ++c)
                    s.push_back(char(u8(c)));
                walkEntry(e, path.empty() ? s : path + "/" + s, out, depth + 1);
            }
        }
    }

    std::vector<uint8_t> readDataset(const std::vector<Message>& msgs, const std::string& path) const
    {
        uint64_t elements = 1, elemSize = 1;
        const Message* layout = nullptr;
        for (const Message& m : msgs)
        {
            switch (m.type)
            {
            case 0x01:
            {
                const int version = u8(m.at);
                const int dims = u8(m.at + 1);
                uint64_t p = m.at + (version == 1 ? 8 : 4);
                for (int d = 0; d < dims; ++d, p += mL)
                    elements *= len(p);
                break;
            }
            case 0x03: elemSize = le(m.at + 4, 4); break;
            case 0x08: layout = &m; break;
            case 0x0B: fail(path + ": filtered (compressed) HDF5 datasets aren't supported");
            default: break;
            }
        }
        if (!layout)
            fail(path + ": dataset without a layout");
        const uint64_t total = elements * elemSize;
        if (total > mB.size() * 4 + (1u << 20))
            fail(path + ": implausible dataset size");
        std::vector<uint8_t> data(size_t(total), 0);
        const uint64_t at = layout->at;
        const int version = u8(at);
        int cls = 0;
        uint64_t address = 0, p = 0;
        std::vector<uint64_t> chunkDims;
        if (version == 3)
        {
            cls = u8(at + 1);
            if (cls == 0)
            {
                const uint64_t n = std::min<uint64_t>(le(at + 2, 2), total);
                need(at + 4, n);
                std::memcpy(data.data(), mB.data() + at + 4, size_t(n));
                return data;
            }
            address = off(at + 2);
            if (cls == 2)
            {
                const int dims = u8(at + 2);
                address = off(at + 3);
                p = at + 3 + mO;
                for (int d = 0; d < dims; ++d, p += 4)
                    chunkDims.push_back(le(p, 4));
            }
        }
        else if (version == 1 || version == 2)
        {
            const int dims = u8(at + 1);
            cls = u8(at + 2);
            p = at + 8;
            if (cls != 0)
            {
                address = off(p);
                p += mO;
            }
            for (int d = 0; d < dims; ++d, p += 4)
                chunkDims.push_back(le(p, 4));
            if (cls == 0)
            {
                const uint64_t n = std::min<uint64_t>(le(p, 4), total);
                need(p + 4, n);
                std::memcpy(data.data(), mB.data() + p + 4, size_t(n));
                return data;
            }
            if (cls == 2 && !chunkDims.empty())
                chunkDims.pop_back();
            else if (cls == 2)
                fail(path + ": chunked layout without dimensions");
        }
        else
            fail(path + ": HDF5 layout version " + std::to_string(version) + " isn't supported");

        if (undefined(address))
            return data;
        if (cls == 1)
        {
            need(addr(address), total);
            std::memcpy(data.data(), mB.data() + addr(address), size_t(total));
            return data;
        }
        if (cls != 2)
            fail(path + ": unknown HDF5 layout class");
        // Version 3 chunk dims include the element size as the last dimension.
        const size_t rank = version == 3 ? chunkDims.size() : chunkDims.size() + 1;
        readChunks(addr(address), rank, elemSize, data, path, 0);
        return data;
    }

    void readChunks(uint64_t node, size_t rank, uint64_t elemSize, std::vector<uint8_t>& data, const std::string& path, int depth) const
    {
        if (depth > 32)
            fail(path + ": chunk index nests too deep");
        sig(node, "TREE");
        const int level = u8(node + 5);
        const uint64_t used = le(node + 6, 2);
        const uint64_t keySize = 8 + 8ull * rank;
        uint64_t p = node + 8 + 2ull * mO;
        for (uint64_t i = 0; i < used; ++i)
        {
            const uint64_t size = le(p, 4);
            const uint64_t mask = le(p + 4, 4);
            const uint64_t first = le(p + 8, 8);
            const uint64_t child = addr(off(p + keySize));
            p += keySize + mO;
            if (level > 0)
            {
                readChunks(child, rank, elemSize, data, path, depth + 1);
                continue;
            }
            if (mask != 0)
                fail(path + ": filtered HDF5 chunks aren't supported");
            if (rank != 2)
                fail(path + ": only one-dimensional chunked datasets are supported");
            const uint64_t start = first * elemSize;
            if (start >= data.size())
                continue;
            const uint64_t n = std::min<uint64_t>(size, data.size() - start);
            need(child, n);
            std::memcpy(data.data() + start, mB.data() + child, size_t(n));
        }
    }

    const std::vector<uint8_t>& mB;
    int mO = 8;
    int mL = 8;
    uint64_t mBase = 0;
    uint64_t mRoot = 0;
};

// Presets ----------------------------------------------------------------------------------------------------------

class PresetReader
{
public:
    explicit PresetReader(const std::vector<uint8_t>& b) : mB(b) {}

    json read()
    {
        if (mB.size() < 12 || le32(0) != 0x1b7c2fddu)
            fail("not a Painter preset");
        const uint32_t version = le32(4);
        if (version == 1)
        {
            mAt = 20;
            return objectV1();
        }
        if (version == 0)
        {
            mAt = 12;
            return typedV0();
        }
        fail("Painter preset version " + std::to_string(version) + " isn't supported");
    }

private:
    struct Class
    {
        std::string name;
        std::vector<std::pair<std::string, uint32_t>> fields;
    };

    void need(size_t n) const
    {
        if (mAt > mB.size() || n > mB.size() - mAt)
            fail("Painter preset is truncated or damaged");
    }
    uint32_t le32(size_t at) const
    {
        uint32_t v;
        std::memcpy(&v, mB.data() + at, 4);
        return v;
    }
    template <class T> T take()
    {
        need(sizeof(T));
        T v;
        std::memcpy(&v, mB.data() + mAt, sizeof(T));
        mAt += sizeof(T);
        return v;
    }
    std::string str()
    {
        const uint32_t n = take<uint32_t>();
        need(n);
        std::string s(reinterpret_cast<const char*>(mB.data() + mAt), n);
        mAt += n;
        return s;
    }
    json floats(int n)
    {
        json a = json::array();
        for (int i = 0; i < n; ++i)
        {
            const float f = take<float>();
            a.push_back(std::isfinite(f) ? double(f) : 0.0);
        }
        return a;
    }

    /** The value types both layouts share. */
    json scalar(uint32_t t)
    {
        switch (t)
        {
        case 0x01: return floats(1)[0];
        case 0x02: return floats(2);
        case 0x03: return floats(3);
        case 0x04: return floats(4);
        case 0x05:
        case 0x0b: return take<int32_t>();
        case 0x06:
        {
            const int32_t a = take<int32_t>();
            return json::array({a, take<int32_t>()});
        }
        case 0x0a: return take<uint8_t>() != 0;
        case 0x0c:
        case 0x0f:
        case 0x15: return take<uint64_t>();
        case 0x0d: return floats(9);
        case 0x0e: return floats(16);
        case 0x10: return str();
        default: fail("Painter preset has an unknown value type " + std::to_string(t));
        }
    }

    const Class& classV1()
    {
        const int32_t ref = take<int32_t>();
        if (ref == -1)
        {
            Class c;
            c.name = str();
            const uint32_t n = take<uint32_t>();
            for (uint32_t i = 0; i < n; ++i)
            {
                std::string f = str();
                c.fields.emplace_back(std::move(f), take<uint32_t>());
            }
            mClasses.push_back(std::move(c));
            return mClasses.back();
        }
        if (ref < 0 || size_t(ref) >= mClasses.size())
            fail("Painter preset refers to an undefined class");
        return mClasses[size_t(ref)];
    }

    json fieldsV1(const Class& c)
    {
        json o = {{"@", c.name}};
        for (const auto& [name, type] : c.fields)
            o[name] = valueV1(type);
        return o;
    }

    json objectV1()
    {
        const size_t start = mAt;
        const uint32_t end = take<uint32_t>();
        if (end == 0)
            return nullptr;
        if (end <= start || end > mB.size())
            fail("Painter preset is damaged");
        // Copy: classV1 may grow mClasses while the fields are read.
        const Class c = classV1();
        json o = fieldsV1(c);
        if (mAt != end)
            fail("Painter preset object " + c.name + " doesn't match its recorded size");
        return o;
    }

    json valueV1(uint32_t t)
    {
        switch (t)
        {
        case 0x09: return take<int32_t>();
        case 0x11:
        {
            const uint32_t n = take<uint32_t>();
            json a = json::array();
            if (n == 0)
                return a;
            const Class c = classV1();
            for (uint32_t i = 0; i < n; ++i)
                a.push_back(fieldsV1(c));
            return a;
        }
        case 0x12:
        case 0x14: return objectV1();
        case 0x13:
        {
            const uint32_t n = take<uint32_t>();
            json a = json::array();
            for (uint32_t i = 0; i < n; ++i)
                a.push_back(objectV1());
            return a;
        }
        default: return scalar(t);
        }
    }

    json typedV0() { return valueV0(take<uint8_t>()); }

    json valueV0(uint32_t t)
    {
        switch (t)
        {
        case 0x09: return take<int64_t>();
        case 0x12:
        case 0x14:
        {
            if (take<uint8_t>() != 0)
                return nullptr;
            const size_t start = mAt;
            const uint32_t end = take<uint32_t>();
            if (end <= start || end > mB.size())
                fail("Painter preset is damaged");
            json o = {{"@", str()}};
            const uint16_t n = take<uint16_t>();
            for (uint16_t i = 0; i < n; ++i)
            {
                std::string f = str();
                o[f] = typedV0();
            }
            if (mAt != end)
                fail("Painter preset object " + o["@"].get<std::string>() + " doesn't match its recorded size");
            return o;
        }
        case 0x11:
        case 0x13:
        {
            const uint32_t end = take<uint32_t>();
            const uint32_t n = take<uint32_t>();
            json a = json::array();
            for (uint32_t i = 0; i < n; ++i)
                a.push_back(typedV0());
            if (mAt != end)
                fail("Painter preset list doesn't match its recorded size");
            return a;
        }
        default: return scalar(t);
        }
    }

    const std::vector<uint8_t>& mB;
    size_t mAt = 0;
    std::vector<Class> mClasses;
};

// Images -----------------------------------------------------------------------------------------------------------

/** A QVariantMap written by QDataStream (big-endian), as Painter puts in front of image pixels. */
class QtProps
{
public:
    QtProps(const std::vector<uint8_t>& b) : mB(b) {}

    std::map<std::string, json> read()
    {
        std::map<std::string, json> out;
        const uint32_t n = be32();
        for (uint32_t i = 0; i < n; ++i)
        {
            std::string key = qstring();
            out[key] = variant();
        }
        return out;
    }
    size_t at() const { return mAt; }

private:
    void need(size_t n) const
    {
        if (mAt > mB.size() || n > mB.size() - mAt)
            fail("Painter image is truncated");
    }
    uint64_t be(int n)
    {
        need(size_t(n));
        uint64_t v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 8) | mB[mAt++];
        return v;
    }
    uint32_t be32() { return uint32_t(be(4)); }
    std::string qstring()
    {
        const uint32_t n = be32();
        if (n == 0xffffffffu)
            return {};
        need(n);
        std::string s;
        for (uint32_t i = 0; i + 1 < n; i += 2)
        {
            const char16_t c = char16_t((mB[mAt + i] << 8) | mB[mAt + i + 1]);
            s.push_back(c < 0x80 ? char(c) : '?');
        }
        mAt += n;
        return s;
    }
    json variant()
    {
        const uint32_t type = be32();
        be(1);
        switch (type)
        {
        case 1: return be(1) != 0;
        case 2: return int32_t(be32());
        case 3: return be32();
        case 4: return int64_t(be(8));
        case 5: return be(8);
        case 6: return std::bit_cast<double>(be(8));
        case 10: return qstring();
        default: fail("Painter image header has an unknown property type " + std::to_string(type));
        }
    }

    const std::vector<uint8_t>& mB;
    size_t mAt = 0;
};

std::vector<uint8_t> lz4Block(const uint8_t* src, size_t srcSize, size_t outSize)
{
    std::vector<uint8_t> out;
    out.reserve(outSize);
    size_t i = 0;
    auto length = [&](size_t base) {
        size_t n = base;
        if (base == 15)
            for (uint8_t b = 255; b == 255;)
            {
                if (i >= srcSize)
                    fail("Painter image data is truncated");
                b = src[i++];
                n += b;
            }
        return n;
    };
    while (i < srcSize)
    {
        const uint8_t token = src[i++];
        const size_t lit = length(token >> 4);
        if (lit > srcSize - i || out.size() + lit > outSize)
            fail("Painter image data is damaged");
        out.insert(out.end(), src + i, src + i + lit);
        i += lit;
        if (i >= srcSize)
            break;
        if (i + 2 > srcSize)
            fail("Painter image data is damaged");
        const size_t offset = size_t(src[i]) | (size_t(src[i + 1]) << 8);
        i += 2;
        const size_t match = length(token & 15) + 4;
        if (offset == 0 || offset > out.size() || out.size() + match > outSize)
            fail("Painter image data is damaged");
        const size_t from = out.size() - offset;
        for (size_t k = 0; k < match; ++k)
            out.push_back(out[from + k]);
    }
    if (out.size() != outSize)
        fail("Painter image data has the wrong size");
    return out;
}
} // namespace

std::map<std::string, std::vector<uint8_t>> readHdf5Datasets(const fs::path& file)
{
    const std::vector<uint8_t> bytes = readAll(file);
    return Hdf5(bytes).datasets();
}

json decodePreset(const std::vector<uint8_t>& bytes)
{
    return PresetReader(bytes).read();
}

image::Image decodeImage(const std::vector<uint8_t>& bytes)
{
    QtProps props(bytes);
    const std::map<std::string, json> p = props.read();
    auto num = [&](const char* k) -> uint64_t {
        auto it = p.find(k);
        if (it == p.end() || !it->second.is_number())
            fail(std::string("Painter image header has no ") + k);
        return it->second.get<uint64_t>();
    };
    auto text = [&](const char* k) {
        auto it = p.find(k);
        return it != p.end() && it->second.is_string() ? it->second.get<std::string>() : std::string();
    };
    const uint64_t w = num("width"), h = num("height");
    if (w == 0 || h == 0 || w > 32768 || h > 32768)
        fail("Painter image has an implausible size");
    const std::string format = text("format");
    int channels = 0;
    std::string rest;
    bool bgr = false;
    for (const auto& [prefix, n] : std::initializer_list<std::pair<const char*, int>>{{"RGBA", 4}, {"BGRA", 4}, {"RGB", 3}, {"BGR", 3}, {"LUMA", 2}, {"LUM", 1}})
        if (format.starts_with(prefix))
        {
            channels = n;
            bgr = prefix[0] == 'B';
            rest = format.substr(std::strlen(prefix));
            break;
        }
    const bool isFloat = !rest.empty() && rest.back() == 'F';
    if (isFloat)
        rest.pop_back();
    const int bits = rest.empty() ? 0 : std::atoi(rest.c_str());
    if (channels == 0 || (bits != 8 && bits != 16 && bits != 32) || (isFloat && bits == 8))
        fail("Painter image format " + format + " isn't supported");
    const uint64_t pitch = p.contains("pitch") ? num("pitch") : w * uint64_t(channels) * uint64_t(bits / 8);
    const uint64_t raw = num("dataSizeRaw");
    if (raw < pitch * h)
        fail("Painter image data is smaller than its size");
    std::vector<uint8_t> px;
    const size_t start = props.at();
    if (text("compression") == "lz4")
    {
        const uint64_t packed = num("dataSizeLZ4");
        if (start + packed > bytes.size())
            fail("Painter image data is truncated");
        px = lz4Block(bytes.data() + start, size_t(packed), size_t(raw));
    }
    else
    {
        if (start + raw > bytes.size())
            fail("Painter image data is truncated");
        px.assign(bytes.begin() + std::ptrdiff_t(start), bytes.begin() + std::ptrdiff_t(start + raw));
    }

    image::Image img;
    img.width = int(w);
    img.height = int(h);
    img.rgba.assign(size_t(w * h * 4), 1.0f);
    const size_t bpc = size_t(bits / 8);
    auto sample = [&](const uint8_t* s) -> float {
        if (bits == 8)
            return float(*s) / 255.0f;
        if (bits == 16 && !isFloat)
        {
            uint16_t v;
            std::memcpy(&v, s, 2);
            return float(v) / 65535.0f;
        }
        if (bits == 16)
        {
            uint16_t hv;
            std::memcpy(&hv, s, 2);
            const uint32_t sign = uint32_t(hv & 0x8000) << 16, e = (hv >> 10) & 31, m = hv & 1023;
            uint32_t f = 0;
            if (e == 0)
                f = m ? std::bit_cast<uint32_t>(float(m) / 16777216.0f) : 0;
            else if (e == 31)
                f = 0x7f800000u | (m << 13);
            else
                f = ((e + 112) << 23) | (m << 13);
            return std::bit_cast<float>(f | sign);
        }
        float v;
        std::memcpy(&v, s, 4);
        return v;
    };
    for (uint64_t y = 0; y < h; ++y)
        for (uint64_t x = 0; x < w; ++x)
        {
            const uint8_t* s = px.data() + y * pitch + x * uint64_t(channels) * bpc;
            float* d = img.rgba.data() + (y * w + x) * 4;
            float c[4] = {0, 0, 0, 1};
            for (int k = 0; k < channels; ++k)
                c[k] = sample(s + size_t(k) * bpc);
            if (channels <= 2)
            {
                d[0] = d[1] = d[2] = c[0];
                d[3] = channels == 2 ? c[1] : 1.0f;
                continue;
            }
            d[0] = bgr ? c[2] : c[0];
            d[1] = c[1];
            d[2] = bgr ? c[0] : c[2];
            d[3] = channels == 4 ? c[3] : 1.0f;
        }
    return img;
}

image::Image readSpsmPreview(const fs::path& file)
{
    const std::map<std::string, std::vector<uint8_t>> sets = readHdf5Datasets(file);
    auto it = sets.find(".alg_meta/preset.bin");
    if (it == sets.end())
        return {};
    // The preview is a WebP file inside Painter's metadata: find its RIFF header and take the size it records.
    const std::vector<uint8_t>& b = it->second;
    for (size_t i = 0; i + 12 <= b.size(); ++i)
    {
        if (std::memcmp(b.data() + i, "RIFF", 4) != 0 || std::memcmp(b.data() + i + 8, "WEBP", 4) != 0)
            continue;
        uint32_t size;
        std::memcpy(&size, b.data() + i + 4, 4);
        if (size_t(size) + 8 > b.size() - i)
            break;
        return image::decodeWebp(b.data() + i, size_t(size) + 8);
    }
    return {};
}

SpsmFile readSpsm(const fs::path& file)
{
    std::map<std::string, std::vector<uint8_t>> sets = readHdf5Datasets(file);
    SpsmFile out;
    std::vector<std::string> presetNames;
    for (auto& [name, bytes] : sets)
    {
        if (name.starts_with("resources/") && name.find("/.alg_meta/") == std::string::npos)
            out.resources[name.substr(10)] = std::move(bytes);
        else if (name.starts_with("preset") && name.ends_with(".bin"))
            presetNames.push_back(name);
    }
    if (presetNames.empty())
        fail(paths::toUtf8(file.filename()) + " holds no layer preset");
    // preset.bin, preset_1.bin, preset_2.bin ... in that order.
    std::ranges::sort(presetNames, [](const std::string& a, const std::string& b) {
        auto index = [](const std::string& s) { return s == "preset.bin" ? 0 : std::atoi(s.c_str() + 7) ; };
        return index(a) < index(b);
    });
    for (const std::string& n : presetNames)
    {
        try
        {
            out.presets.push_back(decodePreset(sets.at(n)));
        }
        catch (const std::exception& e)
        {
            fail(n + ": " + e.what());
        }
    }
    return out;
}
} // namespace lila::tex::painter
