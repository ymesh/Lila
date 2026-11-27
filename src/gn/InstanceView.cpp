#include "gn/InstanceView.h"

#include "core/Profile.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <map>
#include <mutex>

namespace lila::gn
{
namespace
{
struct InstanceViewStore
{
    std::mutex mutex;
    std::map<scene::NodeId, InstanceViewPtr> views;
};

InstanceViewStore& instanceViews()
{
    // Never destroyed: the scene graph's static nodes clear their entries during exit, after this would be gone.
    static InstanceViewStore* s = new InstanceViewStore;
    return *s;
}

bool hasAnonymous(const geo::Mesh& m)
{
    for (size_t d = 0; d < size_t(geo::Domain::Count); ++d)
        for (const std::string& n : m.attributes(geo::Domain(d)).names())
            if (n.starts_with("__"))
                return true;
    return false;
}

/** The mesh as the output path would clean it, shared as is when nothing needs cleaning. */
scene::MeshPtr cleaned(const std::shared_ptr<const geo::Mesh>& m)
{
    if (!m->hasDeleted() && !hasAnonymous(*m))
        return m;
    geo::Mesh copy = *m;
    stripAnonymous(copy);
    copy.compact();
    return std::make_shared<const geo::Mesh>(std::move(copy));
}

struct CollectedPart
{
    std::shared_ptr<const geo::Mesh> mesh;
    std::vector<glm::mat4> transforms;
    std::vector<glm::vec4> colors;  // parallel to transforms once some placement has a colour
    std::vector<glm::vec4> colors2; // the second tone, likewise
    bool colored = false, colored2 = false;
};

struct Collected
{
    std::vector<const geo::Mesh*> order;
    std::map<const geo::Mesh*, CollectedPart> parts;
};

void collect(const Instances& inst, const glm::dmat4& xf, const glm::vec4& outer, const glm::vec4& outer2, Collected& out, int depth)
{
    // Instances can nest but never cycle; the cap only guards a malformed geometry.
    if (depth > 32)
        return;
    const bool colored = inst.attrs.has(kInstanceColorAttr), colored2 = inst.attrs.has(kInstanceColor2Attr);
    for (size_t k = 0; k < inst.size(); ++k)
    {
        const int r = inst.reference[k];
        if (r < 0 || size_t(r) >= inst.references.size() || !inst.references[r])
            continue;
        const Geometry& ref = *inst.references[r];
        const glm::dmat4 m = xf * inst.transforms[k];
        glm::vec4 color = outer, color2 = outer2;
        if (colored)
            if (const glm::vec4 own = instanceColor(inst, k); own.a > 0.0f)
                color = own;
        if (colored2)
            if (const glm::vec4 own = instanceColor2(inst, k); own.a > 0.0f)
                color2 = own;
        if (ref.mesh && ref.mesh->numFaces() > 0)
        {
            auto [it, added] = out.parts.try_emplace(ref.mesh.get());
            CollectedPart& part = it->second;
            if (added)
            {
                part.mesh = ref.mesh;
                out.order.push_back(ref.mesh.get());
            }
            if (color.a > 0.0f && !part.colored)
            {
                part.colored = true;
                part.colors.assign(part.transforms.size(), glm::vec4(0.0f));
            }
            if (color2.a > 0.0f && !part.colored2)
            {
                part.colored2 = true;
                part.colors2.assign(part.transforms.size(), glm::vec4(0.0f));
            }
            part.transforms.push_back(glm::mat4(m));
            if (part.colored)
                part.colors.push_back(color);
            if (part.colored2)
                part.colors2.push_back(color2);
        }
        if (ref.instances)
            collect(*ref.instances, m, color, color2, out, depth + 1);
    }
}
} // namespace

InstanceViewPtr makeInstanceView(const Geometry& g, scene::MeshPtr output)
{
    LILA_ZONE();

    if (!g.instances || g.instances->size() == 0)
        return nullptr;
    Collected c;
    collect(*g.instances, glm::dmat4(1.0), glm::vec4(0.0f), glm::vec4(0.0f), c, 0);
    if (c.order.empty())
        return nullptr;
    auto view = std::make_shared<InstanceView>();
    view->output = std::move(output);
    if (g.mesh && g.mesh->numFaces() > 0)
        view->base = cleaned(g.mesh);
    for (const geo::Mesh* key : c.order)
    {
        CollectedPart& p = c.parts[key];
        view->instanceCount += p.transforms.size();
        view->parts.push_back({cleaned(p.mesh), std::make_shared<const std::vector<glm::mat4>>(std::move(p.transforms)),
                               !p.colored ? nullptr : std::make_shared<const std::vector<glm::vec4>>(std::move(p.colors)),
                               !p.colored2 ? nullptr : std::make_shared<const std::vector<glm::vec4>>(std::move(p.colors2))});
    }
    return view;
}

namespace
{
glm::vec4 colorAttr(const Instances& inst, size_t k, const char* name)
{
    const geo::Attribute* a = inst.attrs.find(name);
    if (!a || k >= a->size())
        return glm::vec4(0.0f);
    if (a->type() == geo::AttrType::Float3)
        return glm::vec4(a->values<glm::vec3>()[k], 1.0f);
    if (a->type() == geo::AttrType::Float4)
        return a->values<glm::vec4>()[k];
    return glm::vec4(0.0f);
}
} // namespace

glm::vec4 instanceColor(const Instances& inst, size_t k) { return colorAttr(inst, k, kInstanceColorAttr); }

glm::vec4 instanceColor2(const Instances& inst, size_t k) { return colorAttr(inst, k, kInstanceColor2Attr); }

void setInstanceView(scene::NodeId modifier, InstanceViewPtr view)
{
    std::lock_guard lock(instanceViews().mutex);
    instanceViews().views[modifier] = std::move(view);
}

void clearInstanceView(scene::NodeId modifier)
{
    std::lock_guard lock(instanceViews().mutex);
    instanceViews().views.erase(modifier);
}

InstanceViewPtr instanceView(scene::NodeId modifier)
{
    std::lock_guard lock(instanceViews().mutex);
    auto it = instanceViews().views.find(modifier);
    return it != instanceViews().views.end() ? it->second : nullptr;
}

InstanceViewPtr instanceViewFor(scene::Graph& g, scene::NodeId mesh)
{
    scene::Node* n = g.node(mesh);
    if (!n || !n->type().isA("mesh"))
        return nullptr;
    auto src = g.source({mesh, scene::MeshNode::kInMesh, -1});
    if (!src)
        return nullptr;
    InstanceViewPtr view = instanceView(src->node);
    if (!view || view->output != n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
        return nullptr;
    return view;
}
} // namespace lila::gn
