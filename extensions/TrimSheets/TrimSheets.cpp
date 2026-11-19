// Trim Sheets: author a trim sheet (layout plus a profile per trim), generate and bake it in one go, and map meshes onto
// it by hand, automatically, or live as geometry nodes. The Hotspot UV node re-maps a mesh whenever its input changes,
// so procedural blockouts stay trimmed as they are edited.

#include "TrimSheets.h"
#include "Generator.h"

#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Paths.h>
#include <lila/Profile.h>
#include <lila/Relocate.h>
#include <lila/TextureGraph.h>
#include <lila/Workspace.h>

using namespace lila;

namespace trims
{
namespace
{
using gn::ExecContext;
using gn::Geometry;
using T = gn::SocketType;

gn::SocketDecl in(std::string id, T t, gn::json def, std::string doc, std::optional<double> lo = std::nullopt, std::optional<double> hi = std::nullopt)
{
    gn::SocketDecl s{.id = std::move(id), .type = t, .defaultValue = std::move(def), .doc = std::move(doc)};
    s.min = lo;
    s.max = hi;
    return s;
}

gn::SocketDecl geoSocket(std::string id, std::string doc) { return {.id = std::move(id), .type = T::Geometry, .doc = std::move(doc)}; }

/** The sheet a node names, or the current one; relative paths are under the workspace. */
Sheet sheetFor(const ExecContext& c)
{
    std::string p = c.s("sheet");
    if (p.empty())
        p = currentSheet();
    if (p.empty())
        throw gn::ExecError("no trim sheet: set the sheet input or make one with trimSheetCreate");
    std::filesystem::path f(p);
    if (f.is_relative() && Workspace::get().isOpen())
        f = Workspace::get().root() / f;
    try
    {
        return load(f);
    }
    catch (const std::exception& e)
    {
        throw gn::ExecError(e.what());
    }
}

void hotspotExec(ExecContext& c)
{
    LILA_ZONE();

    const gn::GeometryPtr src = c.geometry("geometry");
    if (!src || !src->mesh)
    {
        c.out("geometry", src);
        return;
    }
    const Sheet sheet = sheetFor(c);
    uv::HotspotOptions o;
    o.splitAngle = c.f("splitAngle");
    const std::string orient = c.s("orient");
    o.orient = orient == "asIs" ? uv::HotspotOrient::AsIs : orient == "longestU" ? uv::HotspotOrient::LongestU : uv::HotspotOrient::WorldUp;
    o.tag = c.s("tag");
    o.seed = uint32_t(c.i("seed"));
    o.randomFlip = c.b("randomFlip");
    o.randomOffset = c.b("randomOffset");

    geo::Mesh m = *src->mesh;
    const std::vector<uv::HotspotTrim> trims = hotspotTrims(sheet);
    const std::string set = c.s("uvSet").empty() ? std::string("uv0") : c.s("uvSet");
    const uv::HotspotResult r = uv::hotspot(m, {}, trims, set, o);
    std::span<int32_t> trimAttr = m.faceAttrs().getOrAdd<int32_t>(kFaceTrim);
    std::vector<uint32_t> faces;
    for (const uv::HotspotIsland& is : r.islands)
        for (geo::Index f : is.faces)
        {
            trimAttr[f] = is.trim + 1;
            faces.push_back(f);
        }
    const std::string palette = c.s("palette") == "Default" ? std::string() : c.s("palette");
    if (!palette.empty() && !sheet.palette(palette))
        throw gn::ExecError(sheet.name + " has no palette " + palette);
    scene::materials::assign(m, materialName(sheet, palette), faces);

    Geometry out = *src;
    out.mesh = std::make_shared<const geo::Mesh>(std::move(m));
    c.out("geometry", gn::makeGeometry(std::move(out)));
}

gn::NodeDecl hotspotDecl()
{
    return {
        .type = "TrimHotspotUv",
        .label = "Hotspot UV",
        .category = "Trim Sheets",
        .doc = "Maps the mesh's uvs onto a trim sheet: islands split by angle, rectified, measured in world size and matched to the "
               "trim that fits (strips for long islands, rectangles for panels, round trims for discs), with seeded variation. "
               "Faces get the sheet's material slot and a face int 'trim' (index + 1). Re-runs whenever the input changes, so a "
               "blockout stays trimmed while it is edited.",
        .sockets = {.inputs = {geoSocket("geometry", "The mesh to map."),
                               in("sheet", T::String, "", "The .lilatrim; the current sheet when empty."),
                               in("splitAngle", T::Float, 30.0, "Faces meeting sharper than this, degrees, become separate islands.", 0.0, 180.0),
                               in("orient", T::String, "worldUp", "worldUp, longestU or asIs."),
                               in("tag", T::String, "", "Only trims with this tag."),
                               in("seed", T::Int, 0, "Variation seed."),
                               in("randomOffset", T::Bool, true, "Slide islands along tiling strips."),
                               in("randomFlip", T::Bool, false, "Turn islands 180 degrees at random."),
                               in("uvSet", T::String, "uv0", "UV set to write."),
                               in("palette", T::String, "", "Palette the faces use; the sheet's default when empty.")},
                    .outputs = {geoSocket("geometry", "The mesh with its uvs on the sheet.")}},
        .exec = hotspotExec,
    };
}

void generatorExec(ExecContext& c)
{
    LILA_ZONE();

    const Sheet sheet = sheetFor(c);
    GenerateOptions go;
    go.tile = c.b("tile");
    Generated g = generate(sheet, go);
    c.out("high", gn::meshGeometry(std::move(g.high)));
    c.out("low", gn::meshGeometry(std::move(g.low)));
}

gn::NodeDecl generatorDecl()
{
    return {
        .type = "TrimSheetGenerator",
        .label = "Trim Sheet Generator",
        .category = "Trim Sheets",
        .doc = "The high and low poly a trim sheet bakes from: each trim's profile swept across its rectangle (strips along U, "
               "rectangles as framed panels, round trims as domes) with its details, on the XZ plane facing +Y.",
        .sockets = {.inputs = {in("sheet", T::String, "", "The .lilatrim; the current sheet when empty."),
                               in("tile", T::Bool, false, "Repeat tiling strips one sheet width to each side, as the bake does.")},
                    .outputs = {geoSocket("high", "Bevelled trims with corner colour per trim."), geoSocket("low", "The flat plane, uv0 over the sheet.")}},
        .exec = generatorExec,
    };
}
/** trimHotspotModifier's evaluation: the input hotspotted onto its sheet, read fresh each time the input or a parameter changes. */
geo::Mesh liveHotspot(const geo::Mesh& in, const scene::Node& n)
{
    LILA_ZONE();

    const scene::NodeType& t = n.type();
    auto str = [&](const char* name) { return n.as<std::string>(t.attrIndex(name)); };
    std::string p = str("sheet");
    if (p.empty())
        p = currentSheet();
    if (p.empty())
        throw std::runtime_error("no trim sheet: set its sheet");
    const Sheet sheet = load(paths::resolvePath(p));
    uv::HotspotOptions o;
    o.splitAngle = n.as<double>(t.attrIndex("splitAngle"));
    o.orient = std::array{uv::HotspotOrient::WorldUp, uv::HotspotOrient::LongestU, uv::HotspotOrient::AsIs}[size_t(
        std::clamp<int64_t>(n.as<int64_t>(t.attrIndex("orient")), 0, 2))];
    o.tag = str("tag");
    o.seed = uint32_t(n.as<int64_t>(t.attrIndex("seed")));
    o.randomOffset = n.as<bool>(t.attrIndex("randomOffset"));
    o.randomFlip = n.as<bool>(t.attrIndex("randomFlip"));
    o.sliceOversized = n.as<bool>(t.attrIndex("sliceOversized"));
    const std::string palette = str("palette") == "Default" ? std::string() : str("palette");
    if (!palette.empty() && !sheet.palette(palette))
        throw std::runtime_error(sheet.name + " has no palette " + palette);
    const std::string set = str("uvSet").empty() ? std::string("uv0") : str("uvSet");

    geo::Mesh m = in;
    std::vector<geo::Index> open;
    bool kept = false;
    const std::span<int32_t> keep = m.faceAttrs().get<int32_t>(uv::kHotspotKeepAttr);
    for (geo::Index f = 0; f < geo::Index(keep.size()); ++f)
        if (!m.faceValid(f))
            continue;
        else if (keep[f])
            kept = true;
        else
            open.push_back(f);
    if (kept && open.empty())
        return m;
    const uv::HotspotResult r = uv::hotspot(m, open, hotspotTrims(sheet), set, o);
    std::span<int32_t> trimAttr = m.faceAttrs().getOrAdd<int32_t>(kFaceTrim);
    std::vector<uint32_t> faces;
    for (const uv::HotspotIsland& is : r.islands)
        for (geo::Index f : is.faces)
        {
            trimAttr[f] = is.trim + 1;
            faces.push_back(f);
        }
    scene::materials::assign(m, materialName(sheet, palette), faces);
    return m;
}

scene::AttrDef liveParam(std::string name, scene::PlugType type, scene::PlugValue def, std::string doc, std::optional<double> lo = {},
                         std::optional<double> hi = {})
{
    using namespace scene;
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc), .minValue = lo, .maxValue = hi};
}

bool registerLiveHotspot(ext::Context& ctx)
{
    using namespace scene;
    NodeType t;
    t.name = kLiveModifier;
    t.icon = "stack-2";
    t.doc = "Keeps a mesh hotspotted onto a trim sheet: whenever the mesh under it changes (a blockout edited, a generator's "
            "parameters, a modifier lower in the stack) its islands are split, measured and mapped onto the trims again, faces "
            "on the sheet's material and their trim recorded.";
    AttrDef orient = liveParam("orient", PlugType::Enum, int64_t(0), "worldUp keeps world up running up the trim; longestU lays the long side along it.");
    orient.enumNames = {"worldUp", "longestU", "asIs"};
    t.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
        liveParam("enabled", PlugType::Bool, true, "Turn the modifier off without removing it."),
        {.name = "exportEnabled", .type = PlugType::Bool, .defaultValue = true, .flags = AttrInput | AttrStorable, .doc = "Apply the modifier when exporting."},
        {.name = "sheet", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable,
         .doc = "The .lilatrim, relative to the workspace or the scene; the current sheet when empty."},
        liveParam("splitAngle", PlugType::Float, 30.0, "Faces meeting sharper than this, degrees, become separate islands.", 0.0, 180.0),
        orient,
        {.name = "tag", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable, .doc = "Only trims with this tag."},
        liveParam("seed", PlugType::Int, int64_t(0), "Variation seed."),
        liveParam("randomOffset", PlugType::Bool, true, "Slide islands along tiling strips."),
        liveParam("randomFlip", PlugType::Bool, false, "Turn islands 180 degrees at random where the trim allows."),
        liveParam("sliceOversized", PlugType::Bool, true, "Cut islands taller than every strip into courses."),
        {.name = "palette", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable,
         .doc = "Palette the faces use; the sheet's default when empty."},
        {.name = "uvSet", .type = PlugType::String, .defaultValue = std::string("uv0"), .flags = AttrInput | AttrStorable, .doc = "UV set to write."},
        {.name = "sheetRevision", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrInput | AttrHidden,
         .doc = "Raised when the sheet file changes, so the mapping follows it."},
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
    };
    const int out = int(t.ownAttrs.size()) - 1;
    t.create = [out] {
        auto n = std::make_unique<ModifierNode>();
        n->apply = liveHotspot;
        n->outAttr = out;
        return n;
    };
    lila::scene::relocate::registerNeutral(t.name);
    return types::registerType(ctx, std::move(t)) != nullptr;
}
} // namespace

bool registerNodes(ext::Context& ctx)
{
    return gn::registerDecl(ctx, hotspotDecl()) != nullptr && gn::registerDecl(ctx, generatorDecl()) != nullptr && registerLiveHotspot(ctx);
}
} // namespace trims

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!trims::registerNodes(ctx))
        return 1;
    // Trim Sheet Normal, Trim Mask and Trim Gradient: texture nodes reading the sheet's bake.
    lila::tex::registerExtensionDir(ctx, ctx.resources() / "texnodes");
    trims::registerCommands(ctx);
    trims::registerUi(ctx);
    return 0;
}
