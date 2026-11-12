#include "gn/NodesCommon.h"

#include "core/ImageIO.h"
#include "core/Workspace.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>

namespace lila::gn::nodes::sceneinfo
{
namespace fs = std::filesystem;

struct ImageFacts
{
    int width = 0, height = 0, channels = 0;
    bool alpha = false;
};

/** 1 when every texel is grey, 3 when colour, 4 when any texel is not opaque. */
ImageFacts factsOf(const image::Image& img)
{
    ImageFacts f{img.width, img.height, 1, false};
    for (size_t k = 0; k + 3 < img.rgba.size(); k += 4)
    {
        const float* p = &img.rgba[k];
        if (p[3] < 1.0f)
            f.alpha = true;
        if (p[0] != p[1] || p[1] != p[2])
            f.channels = 3;
    }
    if (f.alpha)
        f.channels = 4;
    return f;
}

/** Facts read once per path and modification time. */
ImageFacts imageFacts(const fs::path& path)
{
    static std::mutex mutex;
    static std::map<std::string, std::pair<fs::file_time_type, ImageFacts>> cache;
    std::error_code ec;
    const auto stamp = fs::last_write_time(path, ec);
    if (ec)
        throw ExecError("Image Info: can't find " + path.string());
    std::lock_guard lock(mutex);
    if (auto it = cache.find(path.string()); it != cache.end() && it->second.first == stamp)
        return it->second.second;
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    std::string err;
    image::Image img;
    if (ext == ".png")
        img = image::readPng(path, &err);
    else if (ext == ".exr")
        img = image::readExr(path, &err);
    else if (ext == ".tif" || ext == ".tiff")
        img = image::readTiff(path, &err);
    else if (ext == ".psd")
        img = image::readPsd(path, &err);
    else if (ext == ".tga")
        img = image::readTga(path, &err);
    else
        throw ExecError("Image Info: " + path.string() + " isn't png, exr, tif, psd or tga");
    if (img.width < 1 || img.height < 1)
        throw ExecError("Image Info: " + path.string() + ": " + (err.empty() ? std::string("unreadable") : err));
    if (cache.size() > 64)
        cache.clear();
    ImageFacts f = factsOf(img);
    cache[path.string()] = {stamp, f};
    return f;
}

/** Files of the numbered sequence the path belongs to (name.0001.png and its siblings); 1 when its name ends in no number. */
int64_t sequenceLength(const fs::path& path)
{
    const std::string stem = path.stem().string();
    size_t digits = stem.size();
    while (digits > 0 && std::isdigit(static_cast<unsigned char>(stem[digits - 1])))
        --digits;
    if (digits == stem.size())
        return 1;
    const std::string prefix = stem.substr(0, digits), ext = path.extension().string();
    int64_t n = 0;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(path.parent_path().empty() ? fs::path(".") : path.parent_path(), ec))
    {
        if (!e.is_regular_file(ec) || e.path().extension().string() != ext)
            continue;
        const std::string s = e.path().stem().string();
        if (s.size() > prefix.size() && s.starts_with(prefix) &&
            std::all_of(s.begin() + ptrdiff_t(prefix.size()), s.end(), [](unsigned char ch) { return std::isdigit(ch); }))
            ++n;
    }
    return std::max<int64_t>(n, 1);
}

void imageInfo()
{
    registerDecl({
        .type = "ImageInfo",
        .label = "Image Info",
        .category = "Texture",
        .doc = "Size of an image file (png, exr, tif, psd, tga) in pixels, how many channels it uses (1 grey, 3 colour, 4 with "
               "transparency) and, for a numbered file like name.0001.png, how many frames its sequence has. Relative paths are "
               "under the workspace.",
        .sockets = {.inputs = {in("file", T::String, "", "Image path.")},
                    .outputs = {out("width", T::Int), out("height", T::Int), out("channels", T::Int), out("hasAlpha", T::Bool), out("frameCount", T::Int)}},
        .exec =
            [](ExecContext& c) {
                fs::path file = c.s("file");
                ImageFacts f;
                int64_t frames = 0;
                if (!file.empty())
                {
                    if (file.is_relative())
                        if (const auto& root = Workspace::get().root(); !root.empty())
                            file = root / file;
                    f = imageFacts(file);
                    frames = sequenceLength(file);
                }
                c.out("width", constantInt(f.width));
                c.out("height", constantInt(f.height));
                c.out("channels", constantInt(f.channels));
                c.out("hasAlpha", constantBool(f.alpha));
                c.out("frameCount", constantInt(frames));
            },
    });
}

void outTransform(ExecContext& c, const glm::dmat4& m)
{
    D3 s(glm::length(D3(m[0])), glm::length(D3(m[1])), glm::length(D3(m[2])));
    glm::dmat3 r(D3(m[0]) / std::max(s.x, 1e-12), D3(m[1]) / std::max(s.y, 1e-12), D3(m[2]) / std::max(s.z, 1e-12));
    c.out("transform", constantMatrix(m));
    c.out("location", constantVector(D3(m[3])));
    c.out("rotation", constantVector(matrixToEuler(r), T::Rotation));
    c.out("scale", constantVector(s));
}

void boneInfo()
{
    registerDecl({
        .type = "BoneInfo",
        .label = "Bone Info",
        .category = "Input",
        .doc = "A joint's transform, posed: in world space, or local to its parent joint. The joint is connected into the modifier, "
               "so posing it re-evaluates the group. Any transform can stand in for a joint.",
        .props = {prop("joint", {}, "", "Name of the joint."), prop("space", {"world", "local"}, "world"),
                  prop("relativeTo", {"world", "owner"}, "world", "World space only: owner reads it relative to the mesh this group runs on.")},
        .sockets = {.outputs = {out("transform", T::Matrix), out("location", T::Vector), out("rotation", T::Rotation), out("scale", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                glm::dmat4 m(1.0);
                if (auto it = c.env.objects.find(c.node.name); it != c.env.objects.end())
                {
                    if (c.prop("space") == "local")
                        m = glm::inverse(it->second.parent) * it->second.matrix;
                    else
                        m = c.prop("relativeTo") == "owner" ? glm::inverse(c.env.ownerMatrix) * it->second.matrix : it->second.matrix;
                }
                outTransform(c, m);
            },
    });
}

GeometryPtr memberGeometry(const ObjectInput& o, const std::string& name)
{
    Geometry g;
    if (o.curves)
        g.curves = o.curves;
    else if (o.mesh && o.mesh->hasDeleted())
        g = *meshGeometry(*o.mesh);
    else
        g.mesh = o.mesh;
    g.name = name;
    return makeGeometry(std::move(g));
}

void collectionInfo()
{
    registerDecl({
        .type = "CollectionInfo",
        .label = "Collection Info",
        .category = "Input",
        .doc = "The mesh and curve objects under a transform (its children, theirs and so on), each placed as it stands in the scene. "
               "Original places them relative to that transform, Relative to the mesh this group runs on. Separate gives one "
               "instance per object, named after it (Geometry Name reads it); otherwise one instance holds them all. Reset puts each "
               "at the origin. Objects moved, reparented or added under it are picked up when the group is next edited or reapplied.",
        .props = {prop("parent", {}, "", "Name of the transform whose objects to use."), prop("transformSpace", {"original", "relative"}, "original")},
        .sockets = {.inputs = {in("separateChildren", T::Bool, false), in("resetChildren", T::Bool, false)}, .outputs = {geoOut("instances")}},
        .exec =
            [](ExecContext& c) {
                auto it = c.env.collections.find(c.node.name);
                if (it == c.env.collections.end())
                    return;
                const CollectionInput& col = it->second;
                const glm::dmat4 space = glm::inverse(c.prop("transformSpace") == "relative" ? c.env.ownerMatrix : col.parent);
                const bool separate = c.b("separateChildren"), reset = c.b("resetChildren");
                Instances inner;
                for (size_t k = 0; k < col.members.size(); ++k)
                {
                    inner.references.push_back(memberGeometry(col.members[k], col.names[k]));
                    inner.add(reset ? glm::dmat4(1.0) : space * col.members[k].matrix, int(k));
                }
                Geometry g;
                if (separate)
                    g.instances = std::make_shared<const Instances>(std::move(inner));
                else
                {
                    Geometry all;
                    all.instances = std::make_shared<const Instances>(std::move(inner));
                    Instances outer;
                    outer.references.push_back(makeGeometry(std::move(all)));
                    outer.add(glm::dmat4(1.0), 0);
                    g.instances = std::make_shared<const Instances>(std::move(outer));
                }
                c.out("instances", makeGeometry(std::move(g)));
            },
    });
}
} // namespace lila::gn::nodes::sceneinfo

namespace lila::gn::nodes
{
void registerSceneInfoNodes()
{
    sceneinfo::imageInfo();
    sceneinfo::boneInfo();
    sceneinfo::collectionInfo();
}
} // namespace lila::gn::nodes
