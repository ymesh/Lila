// CadImport: STEP, IGES and BREP through OpenCascade, tessellated to Lila meshes.

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/FileTranslator.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/MeshOps.h>
#include <lila/Node.h>
#include <lila/OptionBox.h>
#include <lila/Paths.h>
#include <lila/Prefs.h>
#include <lila/Transform.h>

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <IGESCAFControl_Reader.hxx>
#include <Interface_Static.hxx>
#include <Poly_Triangulation.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <TDF_ChildIterator.hxx>
#include <TDF_Tool.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Trsf.hxx>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <numbers>
#include <set>
#include <optional>

using namespace lila;
using scene::Graph;
using scene::Node;
using scene::NodeId;

namespace cad
{
namespace fs = std::filesystem;

/** The tessellation a part is meshed with. */
struct Quality
{
    double chordal = 0.05;  // cm the triangles may stray from the surface
    double angular = 20.0;  // degrees between neighbouring normals within a face
    bool relative = false;  // chordal as a fraction of each edge's length instead
};

std::string utf8(const TCollection_ExtendedString& s)
{
    std::string out(size_t(s.LengthOfCString()), '\0');
    Standard_PCharacter p = out.data();
    s.ToUTF8CString(p);
    out.resize(std::strlen(out.c_str()));
    return out;
}

std::string labelName(const TDF_Label& l)
{
    Handle(TDataStd_Name) name;
    if (l.FindAttribute(TDataStd_Name::GetID(), name))
        return utf8(name->Get());
    return {};
}

/** A Lila node name from a CAD name: letters, digits and underscores. */
std::string nodeName(std::string s, const char* fallback)
{
    for (char& c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            c = '_';
    if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0])))
        s = fallback + (s.empty() ? std::string() : "_" + s);
    return Graph::validName(s) ? s : std::string(fallback);
}

glm::dmat4 toGlm(const gp_Trsf& t)
{
    glm::dmat4 m(1.0);
    for (int r = 1; r <= 3; ++r)
        for (int c = 1; c <= 4; ++c)
            m[c - 1][r - 1] = t.Value(r, c);
    return m;
}

/** A linear colour's material slot name: one material per colour, shared by every part that has it. */
std::string slotFor(const Quantity_Color& c)
{
    const auto byte = [](double v) { return int(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5); };
    return std::format("CAD_{:02X}{:02X}{:02X}", byte(c.Red()), byte(c.Green()), byte(c.Blue()));
}

/**
 * A shape's triangulation as one mesh, in the shape's own space: each B-rep face its own fan of triangles with the
 * surface's normals, welded to its neighbours by position so the mesh is one piece while the normals stay sharp across
 * face boundaries. Faces take the slot of their own colour, else the part's.
 */
geo::Mesh meshOf(const TopoDS_Shape& shape, const Handle(XCAFDoc_ColorTool) & colors, const std::optional<Quantity_Color>& partColor,
                 double scale)
{
    geo::Mesh mesh;
    std::vector<glm::vec3> normals;
    std::vector<int32_t> faceSlot;
    std::map<std::string, int32_t> slots;
    auto slotIndex = [&](const std::string& s) {
        auto [it, made] = slots.try_emplace(s, int32_t(mesh.materialNames().size()));
        if (made)
            mesh.materialNames().push_back(s);
        return it->second;
    };
    for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next())
    {
        const TopoDS_Face& face = TopoDS::Face(e.Current());
        TopLoc_Location loc;
        const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull() || tri->NbTriangles() == 0)
            continue;
        if (!tri->HasNormals())
            tri->ComputeNormals();
        const gp_Trsf t = loc.Transformation();
        const bool reversed = face.Orientation() == TopAbs_REVERSED;
        Quantity_Color c;
        std::optional<Quantity_Color> color = partColor;
        if (!colors.IsNull() && (colors->GetColor(face, XCAFDoc_ColorSurf, c) || colors->GetColor(face, XCAFDoc_ColorGen, c)))
            color = c;
        const int32_t slot = color ? slotIndex(slotFor(*color)) : -1;
        const geo::Index base = geo::Index(mesh.pointCapacity());
        std::vector<glm::vec3> faceNormals(size_t(tri->NbNodes()));
        for (int i = 1; i <= tri->NbNodes(); ++i)
        {
            const gp_Pnt p = tri->Node(i).Transformed(t);
            mesh.addPoint(glm::dvec3(p.X(), p.Y(), p.Z()) * scale);
            gp_Dir n = tri->Normal(i);
            n.Transform(t);
            if (reversed)
                n.Reverse();
            faceNormals[size_t(i - 1)] = glm::vec3(float(n.X()), float(n.Y()), float(n.Z()));
        }
        for (int i = 1; i <= tri->NbTriangles(); ++i)
        {
            int a = 0, b = 0, d = 0;
            tri->Triangle(i).Get(a, b, d);
            if (reversed)
                std::swap(b, d);
            if (a == b || b == d || a == d)
                continue;
            mesh.addFace({base + geo::Index(a - 1), base + geo::Index(b - 1), base + geo::Index(d - 1)});
            for (int k : {a, b, d})
                normals.push_back(faceNormals[size_t(k - 1)]);
            faceSlot.push_back(slot);
        }
    }
    // Corner normals in face order (faces were added in order, each three corners from its first).
    std::span<glm::vec3> corner = mesh.cornerAttrs().getOrAdd<glm::vec3>(geo::attr::kNormal);
    size_t k = 0;
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        geo::Index c = mesh.faceCorner(f);
        for (int j = 0; j < 3; ++j, c = mesh.cornerNext(c))
            corner[size_t(c)] = normals[k * 3 + size_t(j)];
        ++k;
    }
    if (!slots.empty())
    {
        std::span<int32_t> m = mesh.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
        k = 0;
        for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
            if (mesh.faceValid(f))
                m[size_t(f)] = std::max(faceSlot[k++], 0);
    }
    geo::mergeByDistance(mesh, {}, {.distance = 1e-4 * std::max(scale, 1e-6)});
    return mesh;
}

/** One imported file's parts and the nodes an earlier import made for them, matched by their path in the assembly. */
struct Importer
{
    Graph& g;
    Handle(XCAFDoc_ShapeTool) shapes;
    Handle(XCAFDoc_ColorTool) colors;
    std::string source;
    double scale = 0.1; // OCCT reads in mm
    glm::dmat4 axes{1.0};
    std::map<std::string, NodeId> existing;
    std::map<std::string, std::shared_ptr<const geo::Mesh>> meshes; // by label entry: a part referenced many times is meshed once
    std::vector<NodeId> made, updated;
    std::map<std::string, std::string> stringAttrs;
    size_t parts = 0, triangles = 0;

    static constexpr const char* kPathAttr = "cadPath";
    static constexpr const char* kSourceAttr = "cadSource";

    void tag(Node& n, const std::string& path)
    {
        for (const auto& [attr, value] : {std::pair<std::string, std::string>{kPathAttr, path}, {kSourceAttr, source}})
        {
            int k = n.type().attrIndex(attr);
            if (k < 0)
                k = g.addAttr(n.id(), {.name = attr, .type = scene::PlugType::String, .defaultValue = std::string()});
            g.setValue({n.id(), k, -1}, value);
        }
    }

    void ensureMaterials(const geo::Mesh& m)
    {
        for (const std::string& slot : m.materialNames())
        {
            if (scene::materials::find(g, slot))
                continue;
            Node* mat = scene::materials::create(g, slot);
            unsigned r = 0, gr = 0, b = 0;
            if (std::sscanf(slot.c_str(), "CAD_%02X%02X%02X", &r, &gr, &b) == 3)
            {
                // Slot names hold sRGB bytes; material colours are linear.
                auto lin = [](unsigned v) { const double s = v / 255.0; return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4); };
                g.setValue({mat->id(), scene::material::kBaseColor, -1}, glm::dvec3(lin(r), lin(gr), lin(b)));
            }
        }
    }

    std::optional<Quantity_Color> colorOf(const TDF_Label& l)
    {
        Quantity_Color c;
        if (!colors.IsNull() && (colors->GetColor(l, XCAFDoc_ColorSurf, c) || colors->GetColor(l, XCAFDoc_ColorGen, c)))
            return c;
        return std::nullopt;
    }

    /** A label (assembly, reference or part) under parent, placed by local (the reference's location). */
    void visit(const TDF_Label& label, NodeId parent, const glm::dmat4& local, const std::string& path, std::optional<Quantity_Color> inherited)
    {
        TDF_Label def = label;
        if (XCAFDoc_ShapeTool::IsReference(label))
            XCAFDoc_ShapeTool::GetReferredShape(label, def);
        // An instance's own name, unless it is OCCT's "=>[0:1:1:2]" pointer to what it places: then the part's.
        std::string name = labelName(label);
        if (name.empty() || name.rfind("=>", 0) == 0)
            name = labelName(def);
        if (auto c = colorOf(label))
            inherited = c;
        else if (auto d = colorOf(def))
            inherited = d;
        const std::string here = path + "/" + (name.empty() ? std::string("part") : name);
        Node* node = nullptr;
        if (auto it = existing.find(here); it != existing.end())
            node = g.node(it->second);
        const bool assembly = XCAFDoc_ShapeTool::IsAssembly(def);
        if (!node)
        {
            node = g.createNode(*scene::types::find(assembly ? "transform" : "mesh"), nodeName(name, assembly ? "Assembly" : "Part"), parent, true);
            tag(*node, here);
            // Placement on new nodes only: a re-import keeps where the user moved things.
            const glm::dmat4 m = local;
            g.setValue({node->id(), scene::Transform::kTranslate, -1}, glm::dvec3(m[3]));
            glm::dmat3 r(m);
            g.setValue({node->id(), scene::Transform::kRotate, -1}, scene::matrixToEuler(r, scene::RotateOrder::XYZ));
            made.push_back(node->id());
        }
        else
            updated.push_back(node->id());
        if (assembly)
        {
            NCollection_Sequence<TDF_Label> components;
            XCAFDoc_ShapeTool::GetComponents(def, components);
            for (int i = 1; i <= components.Length(); ++i)
            {
                const TDF_Label& c = components.Value(i);
                const glm::dmat4 placed = toGlm(XCAFDoc_ShapeTool::GetLocation(c).Transformation());
                // Locations are in mm: the translation scales with the geometry.
                glm::dmat4 m = placed;
                m[3] = glm::dvec4(glm::dvec3(placed[3]) * scale, 1.0);
                visit(c, node->id(), m, here + "#" + std::to_string(i), inherited);
            }
            return;
        }
        TCollection_AsciiString entry;
        TDF_Tool::Entry(def, entry);
        std::shared_ptr<const geo::Mesh>& mesh = meshes[entry.ToCString()];
        if (!mesh)
        {
            mesh = std::make_shared<const geo::Mesh>(meshOf(XCAFDoc_ShapeTool::GetShape(def), colors, inherited, scale));
            triangles += size_t(mesh->numFaces());
        }
        ensureMaterials(*mesh);
        g.setValue({node->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::const_pointer_cast<geo::Mesh>(mesh)));
        ++parts;
    }
};

json importCad(const Args& a)
{
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError("importCad: path required");
    const fs::path path = paths::fromUtf8(a.positional[0].get<std::string>());
    if (!fs::exists(path))
        throw CommandError("importCad: no file " + path.string());
    std::string ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    Quality q{a.get<double>("chordal"), a.get<double>("angular"), a.get<bool>("relative")};

    Handle(XCAFApp_Application) app = XCAFApp_Application::GetApplication();
    Handle(TDocStd_Document) doc;
    app->NewDocument("MDTV-XCAF", doc);
    Interface_Static::SetCVal("xstep.cascade.unit", "MM");
    const std::string file = path.string();
    if (ext == ".step" || ext == ".stp")
    {
        STEPCAFControl_Reader r;
        r.SetNameMode(true);
        r.SetColorMode(true);
        r.SetLayerMode(true);
        if (r.ReadFile(file.c_str()) != IFSelect_RetDone || !r.Transfer(doc))
            throw CommandError("importCad: OpenCascade could not read " + path.filename().string() + " as STEP");
    }
    else if (ext == ".iges" || ext == ".igs")
    {
        IGESCAFControl_Reader r;
        r.SetNameMode(true);
        r.SetColorMode(true);
        if (r.ReadFile(file.c_str()) != IFSelect_RetDone || !r.Transfer(doc))
            throw CommandError("importCad: OpenCascade could not read " + path.filename().string() + " as IGES");
    }
    else if (ext == ".brep" || ext == ".brp")
    {
        TopoDS_Shape shape;
        BRep_Builder b;
        if (!BRepTools::Read(shape, file.c_str(), b))
            throw CommandError("importCad: OpenCascade could not read " + path.filename().string() + " as BREP");
        XCAFDoc_DocumentTool::ShapeTool(doc->Main())->AddShape(shape);
    }
    else
        throw CommandError("importCad: " + ext + " is not STEP, IGES or BREP");

    Importer im{Graph::current(), XCAFDoc_DocumentTool::ShapeTool(doc->Main()), XCAFDoc_DocumentTool::ColorTool(doc->Main()), paths::toUtf8(path)};
    // Tessellate everything once, at the quality asked for, in OCCT's millimetres.
    NCollection_Sequence<TDF_Label> roots;
    im.shapes->GetFreeShapes(roots);
    for (int i = 1; i <= roots.Length(); ++i)
    {
        const TopoDS_Shape s = XCAFDoc_ShapeTool::GetShape(roots.Value(i));
        BRepMesh_IncrementalMesh(s, q.relative ? q.chordal : q.chordal * 10.0, q.relative, q.angular * std::numbers::pi / 180.0, true);
    }

    Graph& g = im.g;
    g.evaluate();
    // Nodes an earlier import of this file made, so a re-import updates them where they are.
    if (a.get<bool>("update"))
        for (NodeId id : g.nodes())
            if (Node* n = g.node(id))
            {
                const int s = n->type().attrIndex(Importer::kSourceAttr), p = n->type().attrIndex(Importer::kPathAttr);
                if (s >= 0 && p >= 0 && n->as<std::string>(s) == im.source)
                    im.existing[n->as<std::string>(p)] = id;
            }
    Graph::Batch batch(g);
    // CAD is Z up; a Y-up scene turns it (x, y, z) to (x, z, -y) on one top group.
    const bool zUp = PrefStore::get().value<std::string>("general", "/scene/upAxis", "y") == "z";
    const std::string rootPath = "/" + path.filename().string();
    Node* top = nullptr;
    if (auto it = im.existing.find(rootPath); it != im.existing.end())
        top = g.node(it->second);
    if (!top)
    {
        top = g.createNode(*scene::types::find("transform"), nodeName(path.stem().string(), "Cad"), scene::kNoNode, true);
        im.tag(*top, rootPath);
        if (!zUp)
            g.setValue({top->id(), scene::Transform::kRotate, -1}, glm::dvec3(-90.0, 0.0, 0.0));
        im.made.push_back(top->id());
    }
    for (int i = 1; i <= roots.Length(); ++i)
        im.visit(roots.Value(i), top->id(), glm::dmat4(1.0), rootPath, std::nullopt);

    // Parts the file no longer has are reported, not deleted: they may carry work.
    json stale = json::array();
    std::set<NodeId> seen(im.updated.begin(), im.updated.end());
    seen.insert(top->id());
    for (const auto& [p, id] : im.existing)
        if (!seen.contains(id))
            if (Node* n = g.node(id))
                stale.push_back(n->path());
    if (!stale.empty())
        log::warn("importCad: {} part(s) from the last import are no longer in the file; left in place", stale.size());
    g.select({top->id()});
    return {{"root", top->path()}, {"parts", im.parts}, {"triangles", im.triangles}, {"new", im.made.size()}, {"updated", im.updated.size()}, {"stale", stale}};
}

void registerCommands(ext::Context& ctx)
{
    using T = ArgType;
    ctx.registerCommand({
        .name = "importCad",
        .doc = "Import a STEP, IGES or BREP model through OpenCascade, tessellated to meshes: the assembly tree as groups with "
               "the file's names, one material per colour, millimetres to cm, normals sharp across B-rep face edges. Importing "
               "the same file again updates the parts it made where they are and reports parts the file no longer has. Returns "
               "the counts.",
        .positionalDoc = "path",
        .flags = {{"chordal", "", T::Float, 0.05, "How far the triangles may stray from the surface, cm (or a fraction of edge length with relative).", {}, "",
                   1e-5},
                  {"angular", "", T::Float, 20.0, "Most degrees between neighbouring normals within a face.", {}, "", 0.5, 90.0},
                  {"relative", "", T::Bool, false, "chordal is a fraction of each edge's length."},
                  {"update", "", T::Bool, true, "Update the parts an earlier import of this file made instead of adding new ones."}},
        .run = importCad,
        .examples = {"importCad(\"D:/cad/gearbox.step\", chordal=0.02)"},
    });
    ops::options::setOverride("importCad", {.order = {"chordal", "angular", "relative", "update"},
                                            .labels = {{"chordal", "Chordal Tolerance"}, {"angular", "Angular Tolerance"},
                                                       {"relative", "Relative Chordal"}, {"update", "Update Earlier Import"}}});
    io::translators::add(ctx, {.id = "cad",
                               .label = "CAD (STEP, IGES)",
                               .doc = "STEP, IGES and BREP models through OpenCascade, tessellated with the import's quality options.",
                               .extensions = {".step", ".stp", ".iges", ".igs", ".brep"},
                               .import = {.command = "importCad"}});
}
} // namespace cad

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    cad::registerCommands(ctx);
    return 0;
}

LILA_EXTENSION_EXPORT void lilaExtensionUninit(lila::ext::Context&) {}
