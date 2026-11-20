#include "geo/ColorSets.h"

#include <glm/gtc/packing.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cstring>

namespace lila::geo::colors
{
std::string layerName(std::string_view set)
{
    return std::string(kLayerPrefix) + std::string(set);
}

std::string_view setOf(std::string_view layer)
{
    if (layer.size() <= kLayerPrefix.size() || layer.substr(0, kLayerPrefix.size()) != kLayerPrefix)
        return {};
    return layer.substr(kLayerPrefix.size());
}

std::vector<std::string> sets(const Mesh& mesh)
{
    std::vector<std::string> out;
    const AttributeSet& a = mesh.cornerAttrs();
    for (size_t i = 0; i < a.layerCount(); ++i)
        if (a.layer(i).type() == AttrType::Float4)
            if (std::string_view s = setOf(a.layer(i).name()); !s.empty())
                out.emplace_back(s);
    return out;
}

bool has(const Mesh& mesh, std::string_view set)
{
    const Attribute* a = set.empty() ? nullptr : mesh.cornerAttrs().find(layerName(set));
    return a && a->type() == AttrType::Float4;
}

std::span<glm::vec4> values(Mesh& mesh, std::string_view set)
{
    return set.empty() ? std::span<glm::vec4>{} : mesh.cornerAttrs().get<glm::vec4>(layerName(set));
}

std::span<const glm::vec4> values(const Mesh& mesh, std::string_view set)
{
    return set.empty() ? std::span<const glm::vec4>{} : mesh.cornerAttrs().get<glm::vec4>(layerName(set));
}

std::span<glm::vec4> add(Mesh& mesh, std::string_view set, const glm::vec4& fill)
{
    if (set.empty())
        return {};
    std::string name = layerName(set);
    if (Attribute* a = mesh.cornerAttrs().find(name); a && a->type() == AttrType::Float4)
        return a->values<glm::vec4>();
    Attribute& a = mesh.cornerAttrs().add(name, AttrType::Float4);
    a.setDefault(fill);
    std::span<glm::vec4> v = a.values<glm::vec4>();
    std::fill(v.begin(), v.end(), fill);
    if (sets(mesh).size() == 1)
        mesh.colorSetInfo().current = std::string(set);
    return v;
}

bool remove(Mesh& mesh, std::string_view set)
{
    if (!mesh.cornerAttrs().remove(layerName(set)))
        return false;
    ColorSetInfo& info = mesh.colorSetInfo();
    std::erase(info.half, std::string(set));
    if (info.current == set)
    {
        auto rest = sets(mesh);
        info.current = rest.empty() ? std::string() : rest.front();
    }
    return true;
}

bool rename(Mesh& mesh, std::string_view from, std::string_view to)
{
    if (to.empty() || !mesh.cornerAttrs().rename(layerName(from), layerName(to)))
        return false;
    ColorSetInfo& info = mesh.colorSetInfo();
    if (info.current == from)
        info.current = std::string(to);
    for (std::string& h : info.half)
        if (h == from)
            h = std::string(to);
    return true;
}

bool copy(Mesh& mesh, std::string_view from, std::string_view to)
{
    const Attribute* src = mesh.cornerAttrs().find(layerName(from));
    if (!src || src->type() != AttrType::Float4 || to.empty() || from == to)
        return false;
    glm::vec4 fill;
    std::memcpy(&fill, src->defaultRaw(), sizeof(fill));
    std::vector<glm::vec4> copied(src->values<glm::vec4>().begin(), src->values<glm::vec4>().end());
    std::span<glm::vec4> dst = add(mesh, to, fill);
    std::copy(copied.begin(), copied.end(), dst.begin());
    setHalf(mesh, to, isHalf(mesh, from));
    return true;
}

bool move(Mesh& mesh, std::string_view set, size_t index)
{
    AttributeSet& a = mesh.cornerAttrs();
    std::vector<size_t> layers;
    size_t at = SIZE_MAX;
    for (size_t i = 0; i < a.layerCount(); ++i)
        if (a.layer(i).type() == AttrType::Float4 && !setOf(a.layer(i).name()).empty())
        {
            if (setOf(a.layer(i).name()) == set)
                at = i;
            layers.push_back(i);
        }
    if (at == SIZE_MAX)
        return false;
    a.moveLayer(at, layers[std::min(index, layers.size() - 1)]);
    return true;
}

std::string current(const Mesh& mesh)
{
    const std::string& stored = mesh.colorSetInfo().current;
    if (!stored.empty() && mesh.cornerAttrs().has(layerName(stored)))
        return stored;
    auto all = sets(mesh);
    return all.empty() ? std::string() : all.front();
}

bool setCurrent(Mesh& mesh, std::string_view set)
{
    if (!mesh.cornerAttrs().has(layerName(set)))
        return false;
    mesh.colorSetInfo().current = std::string(set);
    return true;
}

bool isData(const Mesh& mesh, std::string_view set)
{
    const auto& d = mesh.colorSetInfo().data;
    return std::find(d.begin(), d.end(), set) != d.end();
}

void setData(Mesh& mesh, std::string_view set, bool data)
{
    auto& d = mesh.colorSetInfo().data;
    std::erase(d, std::string(set));
    if (data)
        d.emplace_back(set);
}

std::string displayed(const Mesh& mesh)
{
    const std::string c = current(mesh);
    return c.empty() || isData(mesh, c) ? std::string() : c;
}

bool isHalf(const Mesh& mesh, std::string_view set)
{
    const auto& h = mesh.colorSetInfo().half;
    return std::find(h.begin(), h.end(), set) != h.end();
}

void setHalf(Mesh& mesh, std::string_view set, bool half)
{
    auto& h = mesh.colorSetInfo().half;
    std::erase(h, std::string(set));
    if (!half || !mesh.cornerAttrs().has(layerName(set)))
        return;
    h.emplace_back(set);
    roundToHalf(values(mesh, set));
}

float roundToHalf(float v)
{
    return glm::unpackHalf1x16(glm::packHalf1x16(v));
}

void roundToHalf(std::span<glm::vec4> values)
{
    tbb::parallel_for(size_t(0), values.size(), [&](size_t i) { values[i] = glm::unpackHalf4x16(glm::packHalf4x16(values[i])); });
}

void settle(Mesh& mesh, std::string_view set)
{
    if (isHalf(mesh, set))
        roundToHalf(values(mesh, set));
}

std::string uniqueName(const Mesh& mesh, std::string_view base)
{
    std::string b(base.empty() ? "colorSet" : base);
    if (!mesh.cornerAttrs().has(layerName(b)))
        return b;
    for (int i = 1;; ++i)
        if (std::string n = b + std::to_string(i); !mesh.cornerAttrs().has(layerName(n)))
            return n;
}

std::vector<glm::vec4> pointColors(const Mesh& mesh, std::string_view set)
{
    std::vector<glm::vec4> out(mesh.pointCapacity(), glm::vec4(1.0f));
    std::span<const glm::vec4> v = values(mesh, set);
    if (v.empty())
        return out;
    tbb::parallel_for(Index(0), mesh.pointCapacity(), [&](Index p) {
        if (!mesh.pointValid(p))
            return;
        glm::vec4 sum(0.0f);
        int n = 0;
        mesh.forEachPointCorner(p, [&](Index c) {
            sum += v[c];
            ++n;
        });
        if (n)
            out[p] = sum / float(n);
    });
    return out;
}

void setFromPoints(Mesh& mesh, std::string_view set, std::span<const glm::vec4> perPoint)
{
    std::span<glm::vec4> v = values(mesh, set);
    if (v.empty())
        v = add(mesh, set);
    tbb::parallel_for(Index(0), mesh.cornerCapacity(), [&](Index c) {
        if (mesh.cornerValid(c) && mesh.cornerPoint(c) < perPoint.size())
            v[c] = perPoint[mesh.cornerPoint(c)];
    });
    settle(mesh, set);
}

void adopt(Mesh& mesh)
{
    for (const std::string& s : sets(mesh))
        mesh.cornerAttrs().find(layerName(s))->setDefault(glm::vec4(1.0f));

    auto legacyIndex = [](std::string_view n) -> int {
        if (n == "color")
            return 0;
        if (n.size() > 5 && n.substr(0, 5) == "color" && std::all_of(n.begin() + 5, n.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return std::stoi(std::string(n.substr(5)));
        return -1;
    };
    auto setName = [](int i) { return i == 0 ? std::string("colorSet") : "colorSet" + std::to_string(i); };

    for (const std::string& n : mesh.cornerAttrs().names())
    {
        int i = legacyIndex(n);
        if (i < 0 || mesh.cornerAttrs().find(n)->type() != AttrType::Float4)
            continue;
        std::string to = has(mesh, setName(i)) ? uniqueName(mesh, setName(i)) : setName(i);
        mesh.cornerAttrs().rename(n, layerName(to));
        mesh.cornerAttrs().find(layerName(to))->setDefault(glm::vec4(1.0f));
        if (mesh.colorSetInfo().current.empty())
            mesh.colorSetInfo().current = to;
    }
    for (const std::string& n : mesh.pointAttrs().names())
    {
        int i = legacyIndex(n);
        if (i < 0 || mesh.pointAttrs().find(n)->type() != AttrType::Float4)
            continue;
        std::vector<glm::vec4> perPoint(mesh.pointAttrs().get<glm::vec4>(n).begin(), mesh.pointAttrs().get<glm::vec4>(n).end());
        mesh.pointAttrs().remove(n);
        std::string to = has(mesh, setName(i)) ? uniqueName(mesh, setName(i)) : setName(i);
        add(mesh, to);
        setFromPoints(mesh, to, perPoint);
    }
}
} // namespace lila::geo::colors
