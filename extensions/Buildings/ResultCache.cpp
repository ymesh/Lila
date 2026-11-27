// A generated building as bytes, for the building cache a saved scene keeps beside it: reopening reads the buildings
// back instead of running the grammar again. Every field of Result and the structs it holds is written here; the
// size checks below fail the build when one of them changes, so the format follows (bump kFormat).

#include "Buildings.h"

#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace buildings
{
namespace
{
constexpr uint32_t kFormat = 1;

// Sizes on MSVC x64 when the format was written: a new or changed field fails here until it is written and read below.
static_assert(sizeof(Result) == 1048, "Result changed: update ResultCache.cpp and bump kFormat");
static_assert(sizeof(Module) == 240, "Module changed: update ResultCache.cpp and bump kFormat");
static_assert(sizeof(Facade) == 120, "Facade changed: update ResultCache.cpp and bump kFormat");
static_assert(sizeof(Bay) == 240, "Bay changed: update ResultCache.cpp and bump kFormat");
static_assert(sizeof(FloorPlate) == 112, "FloorPlate changed: update ResultCache.cpp and bump kFormat");
static_assert(sizeof(Opening) == 160, "Opening changed: update ResultCache.cpp and bump kFormat");
static_assert(sizeof(Palette) == 100, "Palette changed: update ResultCache.cpp and bump kFormat");
static_assert(sizeof(Soup) == 216, "Soup changed: update ResultCache.cpp and bump kFormat");


struct Writer
{
    std::string out;
    template <class T> void pod(const T& v)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        out.append(reinterpret_cast<const char*>(&v), sizeof(T));
    }
    template <class T> void pods(const std::vector<T>& v)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        pod(uint64_t(v.size()));
        out.append(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(T));
    }
    void str(const std::string& s)
    {
        pod(uint64_t(s.size()));
        out.append(s);
    }
    void strs(const std::vector<std::string>& v)
    {
        pod(uint64_t(v.size()));
        for (const std::string& s : v)
            str(s);
    }
    void js(const json& j)
    {
        if (j.is_null())
            pod(uint8_t(0));
        else
        {
            pod(uint8_t(1));
            str(j.dump());
        }
    }
    void soup(const Soup& s)
    {
        pods(s.pts);
        pods(s.uvs);
        pods(s.sizes);
        pods(s.mat);
        pods(s.trim);
        pods(s.owner);
        pods(s.part);
        pods(s.bay);
        pods(s.colour);
    }
    void rings(const std::vector<Ring>& v)
    {
        pod(uint64_t(v.size()));
        for (const Ring& r : v)
            pods(r);
    }
};

struct Reader
{
    std::string_view in;
    size_t at = 0;
    void need(size_t n)
    {
        if (n > in.size() - at)
            throw std::runtime_error("truncated");
    }
    template <class T> T pod()
    {
        need(sizeof(T));
        T v;
        std::memcpy(&v, in.data() + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    template <class T> void pod(T& v) { v = pod<T>(); }
    size_t count(size_t each)
    {
        const uint64_t n = pod<uint64_t>();
        if (each && n > (in.size() - at) / each)
            throw std::runtime_error("bad count");
        return size_t(n);
    }
    template <class T> void pods(std::vector<T>& v)
    {
        const size_t n = count(sizeof(T));
        v.resize(n);
        std::memcpy(v.data(), in.data() + at, n * sizeof(T));
        at += n * sizeof(T);
    }
    void str(std::string& s)
    {
        const size_t n = count(1);
        s.assign(in.data() + at, n);
        at += n;
    }
    void strs(std::vector<std::string>& v)
    {
        v.resize(count(8));
        for (std::string& s : v)
            str(s);
    }
    void js(json& j)
    {
        if (pod<uint8_t>() == 0)
        {
            j = nullptr;
            return;
        }
        std::string s;
        str(s);
        j = json::parse(s);
    }
    void soup(Soup& s)
    {
        pods(s.pts);
        pods(s.uvs);
        pods(s.sizes);
        pods(s.mat);
        pods(s.trim);
        pods(s.owner);
        pods(s.part);
        pods(s.bay);
        pods(s.colour);
    }
    void rings(std::vector<Ring>& v)
    {
        v.resize(count(8));
        for (Ring& r : v)
            pods(r);
    }
};
} // namespace

std::string encodeResult(const Result& r)
{
    Writer w;
    w.pod(kFormat);
    w.str(r.id);
    w.str(r.style);
    w.soup(r.shell);
    w.pod(uint64_t(r.modules.size()));
    for (const Module& m : r.modules)
    {
        w.str(m.type);
        w.str(m.path);
        w.str(m.kind);
        w.pod(m.origin);
        w.pod(m.x);
        w.pod(m.y);
        w.pod(m.z);
        w.pod(m.size);
        w.pod(int32_t(m.variant));
        w.pod(uint8_t(m.shell));
        w.js(m.data);
    }
    w.pod(uint64_t(r.facades.size()));
    for (const Facade& f : r.facades)
    {
        w.str(f.path);
        w.pod(int32_t(f.tier));
        w.pod(int32_t(f.edge));
        w.pod(uint8_t(f.courtyard));
        w.pod(int32_t(f.side));
        w.pod(f.a);
        w.pod(f.b);
        w.pod(f.normal);
        w.pod(f.bottom);
        w.pod(f.top);
        w.pod(int32_t(f.bays));
    }
    w.pod(uint64_t(r.bays.size()));
    for (const Bay& b : r.bays)
    {
        w.str(b.path);
        w.str(b.kind);
        for (const Vec3& c : b.corners)
            w.pod(c);
        w.pod(b.normal);
        w.pod(int32_t(b.tier));
        w.pod(int32_t(b.facade));
        w.pod(int32_t(b.floor));
        w.pod(int32_t(b.bay));
        w.pod(uint8_t(b.balcony));
        w.pod(b.openS);
        w.pod(b.openT);
        w.pod(b.openW);
        w.pod(b.openH);
    }
    w.pod(uint64_t(r.plates.size()));
    for (const FloorPlate& p : r.plates)
    {
        w.pod(int32_t(p.tier));
        w.pod(int32_t(p.floor));
        w.pod(p.y);
        w.pod(p.height);
        w.pods(p.outline);
        w.rings(p.holes);
        w.pods(p.ring);
        w.pod(p.wall);
        w.pod(uint8_t(p.tower));
    }
    w.pod(uint64_t(r.openings.size()));
    for (const Opening& o : r.openings)
    {
        w.str(o.bay);
        w.str(o.kind);
        w.str(o.slot);
        w.pod(o.origin);
        w.pod(o.normal);
        w.pod(o.width);
        w.pod(o.height);
    }
    w.pod(r.core);
    w.pod(r.coreSize);
    const Palette& p = r.palette;
    for (const glm::vec3& c : {p.render, p.trim, p.base, p.roof, p.frame, p.door, p.awning})
        w.pod(c);
    w.pod(p.dirt);
    w.pod(int32_t(p.renderIndex));
    w.pod(int32_t(p.trimIndex));
    w.pod(int32_t(p.roofIndex));
    w.pod(uint8_t(r.enterable));
    w.pod(int32_t(r.floors));
    w.pod(r.wallTop);
    w.pod(r.lo);
    w.pod(r.hi);
    w.strs(r.warnings);
    w.soup(r.hlod);
    w.soup(r.far);
    return std::move(w.out);
}

std::shared_ptr<const Result> decodeResult(std::string_view bytes)
{
    try
    {
        Reader rd{bytes};
        if (rd.pod<uint32_t>() != kFormat)
            return nullptr;
        auto out = std::make_shared<Result>();
        Result& r = *out;
        rd.str(r.id);
        rd.str(r.style);
        rd.soup(r.shell);
        r.modules.resize(rd.count(16));
        for (Module& m : r.modules)
        {
            rd.str(m.type);
            rd.str(m.path);
            rd.str(m.kind);
            rd.pod(m.origin);
            rd.pod(m.x);
            rd.pod(m.y);
            rd.pod(m.z);
            rd.pod(m.size);
            m.variant = rd.pod<int32_t>();
            m.shell = rd.pod<uint8_t>() != 0;
            rd.js(m.data);
        }
        r.facades.resize(rd.count(16));
        for (Facade& f : r.facades)
        {
            rd.str(f.path);
            f.tier = rd.pod<int32_t>();
            f.edge = rd.pod<int32_t>();
            f.courtyard = rd.pod<uint8_t>() != 0;
            f.side = rd.pod<int32_t>();
            rd.pod(f.a);
            rd.pod(f.b);
            rd.pod(f.normal);
            rd.pod(f.bottom);
            rd.pod(f.top);
            f.bays = rd.pod<int32_t>();
        }
        r.bays.resize(rd.count(16));
        for (Bay& b : r.bays)
        {
            rd.str(b.path);
            rd.str(b.kind);
            for (Vec3& c : b.corners)
                rd.pod(c);
            rd.pod(b.normal);
            b.tier = rd.pod<int32_t>();
            b.facade = rd.pod<int32_t>();
            b.floor = rd.pod<int32_t>();
            b.bay = rd.pod<int32_t>();
            b.balcony = rd.pod<uint8_t>() != 0;
            rd.pod(b.openS);
            rd.pod(b.openT);
            rd.pod(b.openW);
            rd.pod(b.openH);
        }
        r.plates.resize(rd.count(16));
        for (FloorPlate& p : r.plates)
        {
            p.tier = rd.pod<int32_t>();
            p.floor = rd.pod<int32_t>();
            rd.pod(p.y);
            rd.pod(p.height);
            rd.pods(p.outline);
            rd.rings(p.holes);
            rd.pods(p.ring);
            rd.pod(p.wall);
            p.tower = rd.pod<uint8_t>() != 0;
        }
        r.openings.resize(rd.count(16));
        for (Opening& o : r.openings)
        {
            rd.str(o.bay);
            rd.str(o.kind);
            rd.str(o.slot);
            rd.pod(o.origin);
            rd.pod(o.normal);
            rd.pod(o.width);
            rd.pod(o.height);
        }
        rd.pod(r.core);
        rd.pod(r.coreSize);
        Palette& p = r.palette;
        for (glm::vec3* c : {&p.render, &p.trim, &p.base, &p.roof, &p.frame, &p.door, &p.awning})
            rd.pod(*c);
        rd.pod(p.dirt);
        p.renderIndex = rd.pod<int32_t>();
        p.trimIndex = rd.pod<int32_t>();
        p.roofIndex = rd.pod<int32_t>();
        r.enterable = rd.pod<uint8_t>() != 0;
        r.floors = rd.pod<int32_t>();
        rd.pod(r.wallTop);
        rd.pod(r.lo);
        rd.pod(r.hi);
        rd.strs(r.warnings);
        rd.soup(r.hlod);
        rd.soup(r.far);
        if (rd.at != bytes.size())
            return nullptr;
        return out;
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}
} // namespace buildings
