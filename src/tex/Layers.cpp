#include "tex/Layers.h"
#include "tex/TextImage.h"

#include "core/Profile.h"
#include "gn/Group.h"
#include "tex/Generators.h"
#include "tex/NodeDef.h"
#include "tex/Plan.h"
#include "tex/Ports.h"

#include <algorithm>
#include <functional>
#include <set>
#include <stdexcept>

namespace lila::tex
{
bool hasProjection(const json& projection)
{
    return projection.is_object() && projection.contains("mode") && projection["mode"].is_string() && projection["mode"].get<std::string>() != "uv";
}

namespace
{
constexpr const char* kStackKey = "layerStack";

const std::vector<std::pair<LayerKind, const char*>> kLayerKinds = {
    {LayerKind::Fill, "fill"},         {LayerKind::Paint, "paint"},       {LayerKind::Procedural, "procedural"},
    {LayerKind::Folder, "folder"},     {LayerKind::Instance, "instance"}, {LayerKind::Filter, "filter"},
    {LayerKind::Decal, "decal"},       {LayerKind::Text, "text"},         {LayerKind::Scatter, "scatter"},
};

const std::vector<std::pair<MaskKind, const char*>> kMaskKinds = {
    {MaskKind::Fill, "fill"},     {MaskKind::Generator, "generator"}, {MaskKind::Filter, "filter"},
    {MaskKind::Levels, "levels"}, {MaskKind::Paint, "paint"},         {MaskKind::Anchor, "anchor"},
};

const std::vector<std::string> kNormalModes = {"rnm", "normal"};

template <class T> T field(const json& j, const char* key, const T& fallback)
{
    auto it = j.find(key);
    if (it == j.end() || it->is_null())
        return fallback;
    return it->get<T>();
}

bool isColorType(PortType t) { return t == PortType::Color || t == PortType::Vector; }

std::string paintImage(const Layer& l, const std::string& channel)
{
    auto it = l.paint.is_object() ? l.paint.find(channel) : l.paint.end();
    return it != l.paint.end() && it->is_string() ? it->get<std::string>() : std::string();
}

/** A node output feeding something; an empty node means the stack's base value. */
struct Src
{
    std::string node;
    std::string socket;

    bool empty() const { return node.empty(); }
    bool operator==(const Src&) const = default;
};

using Heads = std::map<std::string, Src>;

/** A value the stack holds that the compiled graph shows as a node value: read back when it's edited there. */
struct Binding
{
    /** Empty for the top graph. */
    std::string subgroup;
    /** Empty: the whole subgroup is the value. */
    std::string node;
    /** Empty: the node's whole values object. */
    std::string key;
    std::string pointer;
};

class Compiler
{
public:
    Compiler(const LayerStack& stack, const SetSettings& settings, const Graph& previous)
        : mStack(withVariant(stack)), mStored(stack.toJson()), mSet(settings), mPrev(previous)
    {
    }

    StackCompile run(std::vector<Binding>* bindings)
    {
        LILA_ZONE();
        mBindings = bindings;
        const GNode* prevOut = mPrev.firstOfType(kMaterialOutputType);
        mG.name = mPrev.name;
        mG.description = mPrev.description;
        GNode out{prevOut ? prevOut->name : "Output", kMaterialOutputType, prevOut ? prevOut->position : glm::dvec2{0.0, 0.0}, json::object()};

        Heads heads;
        compileList(mStack.layers, heads, "/layers");

        const double right = 260.0 * double(mColumn + 1);
        if (!prevOut)
            out.position = {right + 260.0, 0.0};
        for (const Channel& c : mSet.channels)
        {
            Src src = heads[c.name];
            if (c.name == "normal" && mSet.channel("height") && !heads["height"].empty() && mStack.heightNormal != 0.0)
            {
                GNode hn = topNode("StackHeightNormal", "heightToNormal", {right, 600.0});
                link(heads["height"], hn.name, "height");
                hn.values["intensity"] = mStack.heightNormal;
                // Only values off the node's defaults are written, so a graph without them still reads back as this stack.
                if (mSet.heightDepth != 5.0)
                    hn.values["depth"] = mSet.heightDepth;
                if (mSet.uvWidth != 100.0)
                    hn.values["sizeU"] = mSet.uvWidth;
                if (mSet.uvHeight != 100.0)
                    hn.values["sizeV"] = mSet.uvHeight;
                bind({}, hn.name, "intensity", "/heightNormal");
                GNode sn = topNode("StackNormal", "layerBlendNormal", {right + 130.0, 600.0});
                feed(sn, "below", src, c);
                link({hn.name, "normal"}, sn.name, "above");
                sn.values["mode"] = "rnm";
                sn.values["opacity"] = 1.0;
                src = {sn.name, "out"};
                mG.nodes.push_back(std::move(hn));
                mG.nodes.push_back(std::move(sn));
            }
            if (c.name == "normal" && mSet.normalFromHeight)
                src = rebuildFromHeight(src, heads, right);
            if (src.empty())
            {
                out.values[c.name] = baseValue(c);
                bind({}, out.name, c.name, "/base/" + c.name);
            }
            else
                link(src, out.name, c.name);
        }
        out.values[kStackKey] = mStored;
        mG.nodes.insert(mG.nodes.begin(), std::move(out));
        return {std::move(mG), std::move(mWarnings)};
    }

private:
    const LayerStack mStack;
    const json mStored;
    const SetSettings& mSet;
    const Graph& mPrev;
    Graph mG;
    std::vector<std::string> mWarnings;
    bool mWarnedProjection = false;
    std::vector<Binding>* mBindings = nullptr;
    std::map<std::string, Heads> mAnchors;
    int mColumn = 0;

    void bind(const std::string& subgroup, const std::string& node, const std::string& key, const std::string& pointer)
    {
        if (mBindings)
            mBindings->push_back({subgroup, node, key, pointer});
    }

    json baseValue(const Channel& c) const
    {
        auto it = mStack.base.find(c.name);
        return it != mStack.base.end() ? *it : channelDefault(c.name, c.type);
    }

    GNode topNode(const std::string& name, const std::string& type, glm::dvec2 pos)
    {
        if (const GNode* p = mPrev.node(name))
            pos = p->position;
        return {name, type, pos, json::object()};
    }

    void link(const Src& from, const std::string& toNode, const std::string& toSocket) { mG.links.push_back({from.node, from.socket, toNode, toSocket}); }

    /**
     * The normal as a Normals From Height material shows it: rebuilt from the height an engine derives from (the height
     * channel, or the baked height for heightSource mesh), so the viewport matches the export. Unchanged without a height.
     */
    Src rebuildFromHeight(const Src& normal, Heads& heads, double right)
    {
        Src height;
        if (mSet.heightSource == "mesh")
        {
            GNode mh = topNode("StackRebuildHeight", "meshHeight", {right + 130.0, 760.0});
            height = {mh.name, "out"};
            mG.nodes.push_back(std::move(mh));
        }
        else if (mSet.channel("height") && !heads["height"].empty())
            height = heads["height"];
        if (height.empty())
            return normal;
        GNode rb = topNode("StackRebuildNormal", "heightNormalRebuild", {right + 260.0, 600.0});
        if (!normal.empty())
            link(normal, rb.name, "normal");
        link(height, rb.name, "height");
        rb.values["depth"] = mSet.heightDepth;
        rb.values["sizeU"] = mSet.uvWidth;
        rb.values["sizeV"] = mSet.uvHeight;
        rb.values["fade"] = mSet.normalFade;
        mG.nodes.push_back(std::move(rb));
        return {"StackRebuildNormal", "out"};
    }

    /** A top-level input fed from a head, or the channel's base value when the head is the base. */
    void feed(GNode& n, const std::string& socket, const Src& src, const Channel& c)
    {
        if (src.empty())
        {
            n.values[socket] = baseValue(c);
            bind({}, n.name, socket, "/base/" + c.name);
        }
        else
            link(src, n.name, socket);
    }

    Src blend(const Layer& l, const std::string& name, const Channel& c, const Src& below, const Src& above, const Src& mask, const std::string& ptr,
              glm::dvec2 pos)
    {
        const bool normal = c.name == "normal";
        GNode n = topNode(name, normal ? "layerBlendNormal" : "layerBlend", pos);
        feed(n, "below", below, c);
        link(above, n.name, "above");
        link(mask, n.name, "mask");
        std::string mode = channelMode(l, c.name);
        const auto& modes = normal ? kNormalModes : layerBlendModes();
        if (std::find(modes.begin(), modes.end(), mode) == modes.end())
        {
            mWarnings.push_back(l.id + "." + c.name + ": blend mode " + mode + " doesn't apply, using " + modes.front());
            mode = modes.front();
        }
        n.values["mode"] = mode;
        auto it = l.channels.find(c.name);
        n.values["opacity"] = it != l.channels.end() ? it->second.opacity : 1.0;
        bind({}, n.name, "mode", ptr + "/channels/" + c.name + "/mode");
        bind({}, n.name, "opacity", ptr + "/channels/" + c.name + "/opacity");
        Src out{n.name, "out"};
        mG.nodes.push_back(std::move(n));
        return out;
    }

    void compileList(const std::vector<Layer>& list, Heads& heads, const std::string& ptr)
    {
        for (size_t i = 0; i < list.size(); ++i)
        {
            const Layer& l = list[i];
            const std::string lp = ptr + "/" + std::to_string(i);
            const double x = 260.0 * double(mColumn++);
            emitGroup(l, heads, lp, x);
            if (l.enabled)
            {
                if (l.kind == LayerKind::Folder)
                {
                    const Heads before = heads;
                    compileList(l.children, heads, lp + "/children");
                    const double fx = 260.0 * double(mColumn++);
                    for (size_t ci = 0; ci < mSet.channels.size(); ++ci)
                    {
                        const Channel& c = mSet.channels[ci];
                        const Src prior = before.contains(c.name) ? before.at(c.name) : Src{};
                        if (heads[c.name] == prior)
                            continue;
                        if (!channelOn(mStack, l, c.name))
                        {
                            heads[c.name] = prior;
                            continue;
                        }
                        heads[c.name] = blend(l, l.id + "_" + c.name, c, prior, heads[c.name], {l.id, "mask"}, lp, {fx, 140.0 * double(ci)});
                    }
                }
                else
                {
                    Src mask{l.id, "mask"};
                    if (l.heightBlend.enabled && mSet.channel("height") && channelOn(mStack, l, "height"))
                    {
                        GNode hm = topNode(l.id + "_heightMask", "layerHeightMask", {x + 130.0, -140.0});
                        link(mask, hm.name, "mask");
                        feed(hm, "below", heads["height"], *mSet.channel("height"));
                        link({l.id, "height"}, hm.name, "above");
                        hm.values["sharpness"] = l.heightBlend.sharpness;
                        hm.values["influence"] = l.heightBlend.influence;
                        bind({}, hm.name, "sharpness", lp + "/heightBlend/sharpness");
                        bind({}, hm.name, "influence", lp + "/heightBlend/influence");
                        mask = {hm.name, "out"};
                        mG.nodes.push_back(std::move(hm));
                    }
                    for (size_t ci = 0; ci < mSet.channels.size(); ++ci)
                    {
                        const Channel& c = mSet.channels[ci];
                        if (!channelOn(mStack, l, c.name) || (l.kind == LayerKind::Instance && !sourceHas(l, c.name)))
                            continue;
                        Src channelMask = mask;
                        if (covers(l, c.name))
                        {
                            GNode cov = topNode(l.id + "_" + c.name + "_coverage", "layerMaskBlend", {x + 65.0, 140.0 * double(ci) + 70.0});
                            link(mask, cov.name, "below");
                            link({l.id, "cov_" + c.name}, cov.name, "above");
                            cov.values["mode"] = "multiply";
                            channelMask = {cov.name, "out"};
                            mG.nodes.push_back(std::move(cov));
                        }
                        heads[c.name] = blend(l, l.id + "_" + c.name, c, heads[c.name], {l.id, c.name}, channelMask, lp, {x + 130.0, 140.0 * double(ci)});
                    }
                }
            }
            if (!l.anchor.empty())
                mAnchors[l.anchor] = heads;
        }
    }

    /** Whether the layer's channel carries its own coverage (painted alpha), on top of the layer mask. */
    bool covers(const Layer& l, const std::string& channel) const
    {
        if (l.kind == LayerKind::Instance)
        {
            const Layer* src = mStack.find(l.source);
            return src && src->kind == LayerKind::Paint && covers(*src, channel);
        }
        return l.kind == LayerKind::Paint && !paintImage(l, channel).empty();
    }

    bool sourceHas(const Layer& l, const std::string& channel) const
    {
        const Layer* src = mStack.find(l.source);
        return src && src->kind != LayerKind::Folder && channelOn(mStack, *src, channel);
    }

    /** Builds a layer's subgroup and its Group node. */
    struct Sub
    {
        Graph g;
        GNode top;
        std::optional<Graph> prev;
        std::set<std::string> meshNodes;
    };

    GNode& subNode(Sub& s, const std::string& name, const std::string& type, glm::dvec2 pos, json values = json::object())
    {
        if (s.prev)
            if (const GNode* p = s.prev->node(name))
                pos = p->position;
        s.g.nodes.push_back({name, type, pos, std::move(values)});
        return s.g.nodes.back();
    }

    static void subLink(Sub& s, const Src& from, const std::string& toNode, const std::string& toSocket)
    {
        s.g.links.push_back({from.node, from.socket, toNode, toSocket});
    }

    /** A port input of the subgroup fed at the top level from src (or the channel's base value). */
    Src subInput(Sub& s, const std::string& name, const Channel& c, const Src& src)
    {
        if (!s.g.input(name))
        {
            GraphInput gi;
            gi.name = name;
            gi.port = c.type;
            gi.defaultValue = baseValue(c);
            s.g.inputs.push_back(std::move(gi));
            if (src.empty())
                s.top.values[name] = baseValue(c);
            else
                link(src, s.top.name, name);
        }
        return {"GroupInput", name};
    }

    Src anchorInput(Sub& s, const Layer& l, const std::string& anchor, const std::string& channel)
    {
        const Channel* c = mSet.channel(channel);
        Channel fallback{channel, PortType::Gray, 8};
        if (!c)
        {
            mWarnings.push_back(l.id + ": anchor " + anchor + " reads " + channel + ", which the set doesn't have");
            c = &fallback;
        }
        auto it = mAnchors.find(anchor);
        if (it == mAnchors.end())
            mWarnings.push_back(l.id + ": no anchor named " + anchor + " below it");
        Src src = it != mAnchors.end() && it->second.contains(channel) ? it->second.at(channel) : Src{};
        return subInput(s, "anchor_" + anchor + "_" + channel, *c, src);
    }

    /** src read through a projection (layerProjection's values); src itself when there is none. */
    Src projected(Sub& s, const Src& src, const json& projection, const std::string& name, const std::string& pointer, glm::dvec2 pos)
    {
        if (!hasProjection(projection))
            return src;
        if (mSet.bakeProject.empty() && !mWarnedProjection)
        {
            mWarnings.push_back("projections read the mesh's position and normal maps, and the set has no bake project: they project the uvs instead");
            mWarnedProjection = true;
        }
        GNode& n = subNode(s, name, "layerProjection", pos, projection);
        bind(s.g.name, n.name, {}, pointer);
        subLink(s, src, n.name, "input");
        return {n.name, "out"};
    }

    Src constantNode(Sub& s, const std::string& name, const Channel& c, const json& v, const std::string& pointer, glm::dvec2 pos)
    {
        const bool color = isColorType(c.type);
        GNode& n = subNode(s, name, color ? "color" : "constant", pos);
        n.values[color ? "color" : "value"] = v;
        bind(s.g.name, name, color ? "color" : "value", pointer);
        return {name, "out"};
    }

    void emitGroup(const Layer& l, const Heads& heads, const std::string& lp, double x)
    {
        Sub s;
        s.g.name = l.id;
        s.top = topNode(l.id, kGroupType, {x, -320.0});
        s.top.values["group"] = l.id;
        if (auto it = mPrev.subgroups.find(l.id); it != mPrev.subgroups.end())
        {
            try
            {
                s.prev = Graph::fromJson(it->second);
            }
            catch (const std::exception&)
            {
            }
        }
        subNode(s, "GroupInput", kGroupInputType, {-700.0, 0.0});

        std::vector<const Channel*> outs;
        if (l.kind != LayerKind::Folder)
            for (const Channel& c : mSet.channels)
                if (channelOn(mStack, l, c.name) && (l.kind != LayerKind::Instance || sourceHas(l, c.name)))
                    outs.push_back(&c);

        std::optional<Src> contentMask;
        double y = 0.0;
        auto out = [&](const Channel& c, const Src& src) {
            s.g.outputs.push_back({c.name, c.type, {}});
            subLink(s, src, "GroupOutput", c.name);
        };

        switch (l.kind)
        {
        case LayerKind::Fill:
            for (const Channel* c : outs)
            {
                json v = l.material.contains(c->name) ? l.material[c->name] : channelDefault(c->name, c->type);
                if (v.is_string() && !v.get<std::string>().starts_with("#"))
                {
                    // An image fill: read in the layer's projection, so it lands where it is placed.
                    GNode& n = subNode(s, "I_" + c->name, "layerImage", {-600.0, y});
                    n.values["file"] = v;
                    bind(s.g.name, n.name, "file", lp + "/material/" + c->name);
                    out(*c, projected(s, {n.name, "out"}, l.projection, "Proj_" + c->name, lp + "/projection", {-400.0, y}));
                }
                else
                    out(*c, constantNode(s, "C_" + c->name, *c, v, lp + "/material/" + c->name, {-400.0, y}));
                y += 120.0;
            }
            break;
        case LayerKind::Paint:
            for (const Channel* c : outs)
            {
                const std::string file = paintImage(l, c->name);
                if (!file.empty())
                {
                    GNode& n = subNode(s, "P_" + c->name, "layerPaint", {-400.0, y});
                    n.values["file"] = file;
                    bind(s.g.name, n.name, "file", lp + "/paint/" + c->name);
                    out(*c, {n.name, "out"});
                    out({"cov_" + c->name, PortType::Gray, 8}, {n.name, "coverage"});
                }
                else
                {
                    json v = l.material.contains(c->name) ? l.material[c->name] : channelDefault(c->name, c->type);
                    out(*c, constantNode(s, "C_" + c->name, *c, v, lp + "/material/" + c->name, {-400.0, y}));
                }
                y += 120.0;
            }
            {
                // A painted layer mask is optional: coverage lives in each channel image's alpha.
                std::string file = l.paint.is_object() ? field<std::string>(l.paint, "mask", "") : std::string();
                if (!file.empty())
                {
                    GNode& n = subNode(s, "PaintMask", "layerImage", {-400.0, y + 200.0});
                    n.values["file"] = file;
                    bind(s.g.name, n.name, "file", lp + "/paint/mask");
                    contentMask = Src{n.name, "out"};
                }
            }
            break;
        case LayerKind::Decal:
        case LayerKind::Text:
        {
            // Placed like a decal: planar and following the surface unless the layer says otherwise.
            json place = hasProjection(l.projection) ? l.projection : json{{"mode", "planar"}, {"follow", 1.0}};
            json maps = l.kind == LayerKind::Decal ? l.decal.value("maps", json::object()) : json::object();
            if (l.kind == LayerKind::Text)
            {
                TextImageSpec spec;
                spec.text = l.text.value("text", std::string());
                spec.font = l.text.value("font", std::string());
                spec.tracking = l.text.value("tracking", 0.0);
                spec.outline = l.text.value("outline", 0.0);
                spec.resolution = l.text.value("resolution", 256);
                auto colour = [](const json& v, glm::vec3 d) {
                    return v.is_array() && v.size() >= 3 ? glm::vec3(v[0].get<float>(), v[1].get<float>(), v[2].get<float>()) : d;
                };
                spec.color = colour(l.material.value("baseColor", json()), glm::vec3(1.0f));
                spec.outlineColor = colour(l.text.value("outlineColor", json()), glm::vec3(0.0f));
                const TextImage t = renderText(spec);
                if (!t.error.empty())
                    mWarnings.push_back(l.id + ": " + t.error);
                else
                {
                    maps["baseColor"] = t.file;
                    // The box keeps the words' proportions unless its width was set.
                    if (!place.contains("sizeX"))
                        place["sizeX"] = place.value("sizeY", 20.0) * t.aspect;
                }
            }
            json values = place;
            for (const char* k : {"columns", "rows", "cell"})
                if (l.decal.contains(k))
                    values[k] = l.decal[k];
            if (values.value("mode", "planar") == "uv")
                values["mode"] = "planar";
            std::string first;
            for (const Channel* c : outs)
            {
                if (maps.contains(c->name) && maps[c->name].is_string())
                {
                    GNode& img = subNode(s, "I_" + c->name, "layerImage", {-700.0, y});
                    img.values["file"] = maps[c->name];
                    const std::string imgName = img.name;
                    GNode& d = subNode(s, "D_" + c->name, "layerDecal", {-400.0, y}, values);
                    const std::string decalName = d.name;
                    subLink(s, {imgName, "out"}, decalName, "input");
                    out(*c, {decalName, "out"});
                    if (first.empty() || c->name == "baseColor")
                        first = decalName;
                }
                else
                {
                    json v = l.material.contains(c->name) ? l.material[c->name] : channelDefault(c->name, c->type);
                    out(*c, constantNode(s, "C_" + c->name, *c, v, lp + "/material/" + c->name, {-400.0, y}));
                }
                y += 120.0;
            }
            if (first.empty())
            {
                mWarnings.push_back(l.id + ": the decal has no picture, so it covers nothing");
                GNode& none = subNode(s, "NoCover", "constant", {-400.0, y});
                none.values["value"] = 0.0;
                contentMask = Src{none.name, "out"};
            }
            else
                contentMask = Src{first, "mask"};
            break;
        }
        case LayerKind::Scatter:
        {
            const json& sc = l.scatter;
            json sv = {{"countX", sc.value("countX", 6)},
                       {"countY", sc.value("countY", sc.value("countX", 6))},
                       {"perCell", sc.value("perCell", 3)},
                       {"size", sc.value("size", 0.6)},
                       {"sizeRandom", sc.value("sizeRandom", 0.4)},
                       {"rotationRandom", sc.value("rotationRandom", 1.0)},
                       {"seed", sc.value("seed", 0)},
                       {"blend", "top"}};
            // No overlap: one copy per cell, kept inside it.
            if (sc.value("avoidOverlap", false))
            {
                sv["perCell"] = 1;
                sv["offsetRandom"] = std::clamp(1.0 - sc.value("size", 0.6), 0.0, 1.0);
            }
            GNode& sp = subNode(s, "Scatter", "splatter", {-500.0, 400.0}, sv);
            const std::string spn = sp.name;
            if (sc.contains("pattern") && sc["pattern"].is_string())
            {
                GNode& pat = subNode(s, "Pattern", "layerImage", {-800.0, 400.0});
                pat.values["file"] = sc["pattern"];
                const std::string patName = pat.name;
                subLink(s, {patName, "out"}, spn, "pattern");
            }
            else
                mWarnings.push_back(l.id + ": the scatter has no pattern image; it scatters the splatter's own shape");
            const json maps = sc.value("maps", json::object());
            for (const Channel* c : outs)
            {
                if (maps.contains(c->name) && maps[c->name].is_string())
                {
                    GNode& img = subNode(s, "I_" + c->name, "layerImage", {-700.0, y});
                    img.values["file"] = maps[c->name];
                    const std::string imgName = img.name;
                    GNode& at = subNode(s, "S_" + c->name, "layerSampleAt", {-300.0, y},
                                        {{"hue", c->name == "baseColor" ? sc.value("hueRandom", 0.0) : 0.0}});
                    const std::string atName = at.name;
                    subLink(s, {imgName, "out"}, atName, "input");
                    subLink(s, {spn, "instanceUv"}, atName, "coord");
                    subLink(s, {spn, "random"}, atName, "random");
                    out(*c, {atName, "out"});
                }
                else
                {
                    json v = l.material.contains(c->name) ? l.material[c->name] : channelDefault(c->name, c->type);
                    out(*c, constantNode(s, "C_" + c->name, *c, v, lp + "/material/" + c->name, {-400.0, y}));
                }
                y += 120.0;
            }
            contentMask = Src{spn, "out"};
            break;
        }
        case LayerKind::Procedural:
        {
            const std::string name = l.id + "_content";
            json graph = l.graph.is_object() ? l.graph : Graph::makeGroup(name).toJson();
            graph["name"] = name;
            Graph content;
            try
            {
                content = Graph::fromJson(graph);
            }
            catch (const std::exception& e)
            {
                mWarnings.push_back(l.id + ": " + e.what());
                break;
            }
            mG.subgroups[name] = graph;
            bind(name, {}, {}, lp + "/graph");
            GNode& n = subNode(s, "Content", kGroupType, {-400.0, 0.0});
            n.values["group"] = name;
            // Each output resampled through the layer's tiling, so the content repeats without editing its graph.
            auto tiled = [&](const std::string& socket, double yy) -> Src {
                if (hasProjection(l.projection))
                    return projected(s, {"Content", socket}, l.projection, "Proj_" + socket, lp + "/projection", {-200.0, yy});
                if (l.tiling.identity())
                    return {"Content", socket};
                GNode& t = subNode(s, "Tiling_" + socket, "layerTiling", {-200.0, yy});
                t.values = {{"tiles", l.tiling.scale}, {"rotation", l.tiling.rotation}, {"offsetX", l.tiling.offsetX}, {"offsetY", l.tiling.offsetY}};
                for (const char* k : {"tiles", "rotation", "offsetX", "offsetY"})
                    bind(s.g.name, t.name, k, lp + "/tiling/" + (std::string_view(k) == "tiles" ? std::string("scale") : std::string(k)));
                subLink(s, {"Content", socket}, t.name, "input");
                return {t.name, "out"};
            };
            double ty = 0.0;
            for (const Channel* c : outs)
            {
                if (content.output(c->name))
                    out(*c, tiled(c->name, ty));
                else
                    mWarnings.push_back(l.id + ": its graph has no " + c->name + " output");
                ty += 120.0;
            }
            if (content.output("mask"))
                contentMask = tiled("mask", ty);
            break;
        }
        case LayerKind::Filter:
        {
            const NodeDef* def = findNode(l.filter);
            if (!def)
            {
                mWarnings.push_back(l.id + ": unknown filter node " + l.filter);
                break;
            }
            bool bound = false;
            for (const Channel* c : outs)
            {
                GNode probe{"F_" + c->name, l.filter, {}, l.filterValues};
                NodePorts ports = portsOf(s.g, probe);
                if (!ports.error.empty() || ports.inputs.empty() || ports.outputs.empty())
                {
                    mWarnings.push_back(l.id + ": " + l.filter + " can't filter (it needs an input and an output)");
                    break;
                }
                Src below = subInput(s, "below_" + c->name, *c, heads.contains(c->name) ? heads.at(c->name) : Src{});
                GNode& n = subNode(s, probe.name, l.filter, {-400.0, y}, l.filterValues);
                if (!bound)
                    bind(s.g.name, n.name, {}, lp + "/filterValues");
                bound = true;
                subLink(s, below, n.name, ports.inputs.front().id);
                out(*c, {n.name, ports.outputs.front().id});
                y += 120.0;
            }
            break;
        }
        case LayerKind::Instance:
        {
            const Layer* src = mStack.find(l.source);
            if (!src || src->kind == LayerKind::Folder || src->id == l.id)
            {
                mWarnings.push_back(l.id + ": instance of " + (l.source.empty() ? std::string("nothing") : l.source) + ", which isn't a layer with values");
                break;
            }
            Channel coverage{"coverage", PortType::Gray, 8};
            for (const Channel* c : outs)
            {
                out(*c, subInput(s, "src_" + c->name, *c, {src->id, c->name}));
                if (covers(l, c->name))
                {
                    coverage.name = "cov_" + c->name;
                    out(coverage, subInput(s, "srccov_" + c->name, coverage, {src->id, "cov_" + c->name}));
                }
            }
            if (!s.g.input("srcMask"))
            {
                GraphInput gi;
                gi.name = "srcMask";
                gi.port = PortType::Gray;
                gi.defaultValue = 0.0;
                s.g.inputs.push_back(std::move(gi));
                link({src->id, "mask"}, s.top.name, "srcMask");
            }
            contentMask = Src{"GroupInput", "srcMask"};
            break;
        }
        case LayerKind::Folder: break;
        }

        // The mask stack, bottom to top.
        std::vector<size_t> active;
        for (size_t j = 0; j < l.masks.size(); ++j)
            if (l.masks[j].enabled)
                active.push_back(j);
        GNode& baseMask = subNode(s, "MaskBase", "constant", {-400.0, 600.0});
        baseMask.values["value"] = active.empty() ? 1.0 : 0.0;
        Src m{"MaskBase", "out"};
        double mx = -250.0;
        for (size_t j : active)
        {
            const Mask& e = l.masks[j];
            const std::string mp = lp + "/masks/" + std::to_string(j);
            std::optional<Src> v;
            switch (e.kind)
            {
            case MaskKind::Fill:
            {
                GNode& n = subNode(s, e.id, "constant", {mx, 750.0});
                n.values["value"] = e.value;
                bind(s.g.name, e.id, "value", mp + "/value");
                v = Src{e.id, "out"};
                break;
            }
            case MaskKind::Paint:
            {
                GNode& n = subNode(s, e.id, "layerImage", {mx, 750.0});
                n.values["file"] = e.image;
                bind(s.g.name, e.id, "file", mp + "/image");
                v = Src{e.id, "out"};
                break;
            }
            case MaskKind::Anchor: v = anchorInput(s, l, e.anchor, e.channel); break;
            case MaskKind::Generator:
            case MaskKind::Filter:
            case MaskKind::Levels:
            {
                const std::string type = e.kind == MaskKind::Levels ? std::string("levels") : e.type;
                GNode probe{e.id, type, {}, e.values};
                NodePorts ports = portsOf(s.g, probe);
                if (!ports.error.empty() || ports.outputs.empty() || (e.kind != MaskKind::Generator && ports.inputs.empty()))
                {
                    mWarnings.push_back(l.id + "." + e.id + ": " + (ports.error.empty() ? type + " has no usable input or output" : ports.error));
                    break;
                }
                subNode(s, e.id, type, {mx, 750.0}, e.values);
                bind(s.g.name, e.id, {}, mp + "/values");
                if (e.kind == MaskKind::Generator)
                {
                    for (const PortDecl& p : ports.inputs)
                        if (std::optional<Src> b = generatorInput(s, l, e, p))
                            subLink(s, *b, e.id, p.id);
                    v = Src{e.id, ports.output("mask") ? std::string("mask") : ports.outputs.front().id};
                }
                else
                {
                    subLink(s, m, e.id, ports.inputs.front().id);
                    v = Src{e.id, ports.outputs.front().id};
                }
                break;
            }
            }
            if (!v)
                continue;
            if (e.kind == MaskKind::Generator || e.kind == MaskKind::Paint)
                v = projected(s, *v, e.projection, e.id + "_proj", mp + "/projection", {mx + 50.0, 850.0});
            GNode& b = subNode(s, e.id + "_blend", "layerMaskBlend", {mx + 100.0, 600.0});
            b.values["mode"] = e.mode;
            b.values["opacity"] = e.opacity;
            b.values["invert"] = e.invert;
            const std::string bn = b.name;
            subLink(s, m, bn, "below");
            subLink(s, *v, bn, "above");
            bind(s.g.name, bn, "mode", mp + "/mode");
            bind(s.g.name, bn, "opacity", mp + "/opacity");
            bind(s.g.name, bn, "invert", mp + "/invert");
            m = {bn, "out"};
            mx += 260.0;
        }
        if (contentMask)
        {
            GNode& b = subNode(s, "ContentMask", "layerMaskBlend", {mx + 100.0, 600.0});
            b.values["mode"] = "multiply";
            subLink(s, m, "ContentMask", "below");
            subLink(s, *contentMask, "ContentMask", "above");
            m = {"ContentMask", "out"};
        }
        s.g.outputs.push_back({"mask", PortType::Gray, {}});
        subLink(s, m, "GroupOutput", "mask");
        subNode(s, "GroupOutput", kGroupOutputType, {mx + 400.0, 0.0});

        mG.subgroups[l.id] = s.g.toJson();
        mG.nodes.push_back(std::move(s.top));
    }

    std::optional<Src> generatorInput(Sub& s, const Layer& l, const Mask& e, const PortDecl& p)
    {
        auto it = e.bindings.find(p.id);
        const std::string role = it != e.bindings.end() ? it->second : p.id;
        if (role.empty() || role == "none")
            return std::nullopt;
        if (role.starts_with("anchor:"))
        {
            const std::string ref = role.substr(7);
            const auto dot = ref.find('.');
            return anchorInput(s, l, ref.substr(0, dot), dot == std::string::npos ? std::string("height") : ref.substr(dot + 1));
        }
        const MeshRole* r = findMeshRole(role);
        if (!r)
        {
            mWarnings.push_back(l.id + "." + e.id + ": " + p.id + " binds to " + role + ", which is no mesh map");
            return std::nullopt;
        }
        const std::string name = "Mesh_" + r->name;
        if (s.meshNodes.insert(name).second)
            subNode(s, name, r->nodeType, {-700.0, 900.0 + 100.0 * double(s.meshNodes.size())});
        return Src{name, r->socket};
    }
};

/** A graph as compared for graph mode: no positions, nodes by name, links sorted, no stack metadata. */
json normalized(const json& graph, int depth = 0)
{
    json out = json::object();
    out["inputs"] = graph.value("inputs", json::array());
    out["outputs"] = graph.value("outputs", json::array());
    json nodes = json::object();
    for (const json& n : graph.value("nodes", json::array()))
    {
        json values = n.value("values", json::object());
        values.erase(kStackKey);
        nodes[n.value("name", std::string())] = {{"type", n.value("type", std::string())}, {"values", values}};
    }
    out["nodes"] = nodes;
    std::vector<std::string> links;
    for (const json& l : graph.value("links", json::array()))
        links.push_back(l.dump());
    std::sort(links.begin(), links.end());
    out["links"] = links;
    json subs = json::object();
    if (depth < 8)
    {
        const json groups = graph.value("subgroups", json::object());
        for (auto& [k, v] : groups.items())
            subs[k] = normalized(v, depth + 1);
    }
    out["subgroups"] = subs;
    return out;
}

json defaultFor(const Graph& container, const GNode& n, const std::string& key)
{
    NodePorts p = portsOf(container, n);
    if (const ParamDecl* d = p.param(key))
        return d->defaultValue;
    if (const PortDecl* d = p.input(key))
        return d->defaultValue;
    return json();
}

void collectExternals(const json& graph, const std::map<std::string, json>& subgroups, std::set<std::string>& out, int depth)
{
    if (depth > 16 || !graph.is_object())
        return;
    std::map<std::string, json> subs = subgroups;
    if (auto it = graph.find("subgroups"); it != graph.end() && it->is_object() && !it->empty())
        subs = it->get<std::map<std::string, json>>();
    for (const json& n : graph.value("nodes", json::array()))
    {
        const std::string type = n.value("type", std::string());
        if (type == kGroupType)
        {
            auto it = subs.find(n.value("values", json::object()).value("group", std::string()));
            if (it != subs.end())
                collectExternals(it->second, subs, out, depth + 1);
            continue;
        }
        const NodeDef* d = findNode(type);
        if (!d)
            continue;
        out.insert(d->externals.begin(), d->externals.end());
        if (d->kind == NodeKind::Graph)
            collectExternals(d->graph, {}, out, depth + 1);
    }
}

json layerView(const LayerStack& stack, const Layer& l, const SetSettings& s)
{
    json j = l.toJson();
    json ch = json::object();
    for (const Channel& c : s.channels)
    {
        auto it = l.channels.find(c.name);
        ch[c.name] = {{"enabled", channelOn(stack, l, c.name)}, {"mode", channelMode(l, c.name)},
                      {"opacity", it != l.channels.end() ? it->second.opacity : 1.0}};
    }
    j["resolved"] = ch;
    if (!l.children.empty())
    {
        json kids = json::array();
        for (const Layer& c : l.children)
            kids.push_back(layerView(stack, c, s));
        j["children"] = kids;
    }
    return j;
}
} // namespace

// Names -------------------------------------------------------------------------------------------------------------

const char* layerKindName(LayerKind k)
{
    for (auto& [kind, name] : kLayerKinds)
        if (kind == k)
            return name;
    return "fill";
}

bool parseLayerKind(std::string_view s, LayerKind* out)
{
    for (auto& [kind, name] : kLayerKinds)
        if (s == name)
        {
            *out = kind;
            return true;
        }
    return false;
}

const char* maskKindName(MaskKind k)
{
    for (auto& [kind, name] : kMaskKinds)
        if (kind == k)
            return name;
    return "fill";
}

bool parseMaskKind(std::string_view s, MaskKind* out)
{
    for (auto& [kind, name] : kMaskKinds)
        if (s == name)
        {
            *out = kind;
            return true;
        }
    return false;
}

const std::vector<std::string>& layerBlendModes()
{
    // Same order as layerBlend's mode options: the shader takes the index.
    static const std::vector<std::string> modes = {"normal",     "add",        "subtract",  "multiply",    "screen",     "overlay",
                                                   "darken",     "lighten",    "difference", "softLight",  "addSigned",  "colorBurn",
                                                   "linearBurn", "colorDodge", "hardLight",  "vividLight", "linearLight", "pinLight",
                                                   "exclusion",  "divide",     "saturation", "color",      "value"};
    return modes;
}

const std::vector<std::string>& maskBlendModes()
{
    static const std::vector<std::string> modes = {"normal", "add", "subtract", "multiply", "screen", "overlay", "darken", "lighten", "difference"};
    return modes;
}

// Model JSON --------------------------------------------------------------------------------------------------------

json Mask::toJson() const
{
    json j = {{"id", id}, {"kind", maskKindName(kind)}, {"enabled", enabled}, {"mode", mode}, {"opacity", opacity}, {"invert", invert}};
    if (!name.empty())
        j["name"] = name;
    switch (kind)
    {
    case MaskKind::Fill: j["value"] = value; break;
    case MaskKind::Generator:
    case MaskKind::Filter:
        j["type"] = type;
        j["values"] = values;
        if (!bindings.empty())
            j["bindings"] = bindings;
        break;
    case MaskKind::Levels: j["values"] = values; break;
    case MaskKind::Paint: j["image"] = image; break;
    case MaskKind::Anchor:
        j["anchor"] = anchor;
        j["channel"] = channel;
        break;
    }
    if (hasProjection(projection))
        j["projection"] = projection;
    return j;
}

Mask Mask::fromJson(const json& j)
{
    if (!j.is_object())
        throw std::invalid_argument("a mask must be an object");
    try
    {
        Mask m;
        m.id = field<std::string>(j, "id", "");
        if (!parseMaskKind(field<std::string>(j, "kind", "fill"), &m.kind))
            throw std::invalid_argument("unknown mask kind " + field<std::string>(j, "kind", ""));
        m.name = field<std::string>(j, "name", "");
        m.enabled = field<bool>(j, "enabled", true);
        m.mode = field<std::string>(j, "mode", "normal");
        if (std::find(maskBlendModes().begin(), maskBlendModes().end(), m.mode) == maskBlendModes().end())
            throw std::invalid_argument("unknown mask blend mode " + m.mode);
        m.opacity = field<double>(j, "opacity", 1.0);
        m.invert = field<bool>(j, "invert", false);
        m.value = field<double>(j, "value", 1.0);
        m.type = field<std::string>(j, "type", "");
        m.values = field<json>(j, "values", json::object());
        if (!m.values.is_object())
            throw std::invalid_argument("mask values must be an object");
        m.bindings = field<std::map<std::string, std::string>>(j, "bindings", {});
        m.image = field<std::string>(j, "image", "");
        m.anchor = field<std::string>(j, "anchor", "");
        m.channel = field<std::string>(j, "channel", "height");
        m.projection = field<json>(j, "projection", json::object());
        if (!m.projection.is_object())
            throw std::invalid_argument("mask projection must be an object");
        return m;
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(std::string("bad mask: ") + e.what());
    }
}

json Layer::toJson() const
{
    json j = {{"id", id}, {"name", name}, {"kind", layerKindName(kind)}, {"enabled", enabled}};
    if (!channels.empty())
    {
        json ch = json::object();
        for (const auto& [k, c] : channels)
        {
            json e = {{"enabled", c.enabled}, {"opacity", c.opacity}};
            if (!c.mode.empty())
                e["mode"] = c.mode;
            ch[k] = e;
        }
        j["channels"] = ch;
    }
    if (kind == LayerKind::Fill || kind == LayerKind::Paint || kind == LayerKind::Decal || kind == LayerKind::Text || kind == LayerKind::Scatter)
        j["material"] = material;
    if (kind == LayerKind::Decal)
        j["decal"] = decal;
    if (kind == LayerKind::Text)
        j["text"] = text;
    if (kind == LayerKind::Scatter)
        j["scatter"] = scatter;
    if (kind == LayerKind::Paint)
        j["paint"] = paint;
    if (kind == LayerKind::Procedural)
        j["graph"] = graph;
    if (kind == LayerKind::Instance)
        j["source"] = source;
    if (kind == LayerKind::Filter)
    {
        j["filter"] = filter;
        j["filterValues"] = filterValues;
    }
    if (!anchor.empty())
        j["anchor"] = anchor;
    if (!tiling.identity())
        j["tiling"] = {{"scale", tiling.scale}, {"rotation", tiling.rotation}, {"offsetX", tiling.offsetX}, {"offsetY", tiling.offsetY}};
    if (hasProjection(projection))
        j["projection"] = projection;
    if (heightBlend.enabled || heightBlend.sharpness != 0.8 || heightBlend.influence != 1.0)
        j["heightBlend"] = {{"enabled", heightBlend.enabled}, {"sharpness", heightBlend.sharpness}, {"influence", heightBlend.influence}};
    if (!masks.empty())
    {
        json ms = json::array();
        for (const Mask& m : masks)
            ms.push_back(m.toJson());
        j["masks"] = ms;
    }
    if (kind == LayerKind::Folder)
    {
        json kids = json::array();
        for (const Layer& c : children)
            kids.push_back(c.toJson());
        j["children"] = kids;
    }
    return j;
}

Layer Layer::fromJson(const json& j)
{
    if (!j.is_object())
        throw std::invalid_argument("a layer must be an object");
    try
    {
        Layer l;
        l.id = field<std::string>(j, "id", "");
        l.name = field<std::string>(j, "name", "");
        if (!parseLayerKind(field<std::string>(j, "kind", "fill"), &l.kind))
            throw std::invalid_argument("unknown layer kind " + field<std::string>(j, "kind", ""));
        l.enabled = field<bool>(j, "enabled", true);
        const json channels = field<json>(j, "channels", json::object());
        for (auto& [k, c] : channels.items())
        {
            if (!c.is_object())
                throw std::invalid_argument("channel " + k + " must be an object");
            ChannelBlend b;
            b.enabled = field<bool>(c, "enabled", true);
            b.mode = field<std::string>(c, "mode", "");
            b.opacity = field<double>(c, "opacity", 1.0);
            if (!b.mode.empty() && std::find(layerBlendModes().begin(), layerBlendModes().end(), b.mode) == layerBlendModes().end() &&
                std::find(kNormalModes.begin(), kNormalModes.end(), b.mode) == kNormalModes.end())
                throw std::invalid_argument("unknown blend mode " + b.mode);
            l.channels[k] = b;
        }
        l.material = field<json>(j, "material", json::object());
        l.paint = field<json>(j, "paint", json::object());
        if (!l.material.is_object() || !l.paint.is_object())
            throw std::invalid_argument("material and paint must be objects");
        l.graph = field<json>(j, "graph", json());
        l.source = field<std::string>(j, "source", "");
        l.filter = field<std::string>(j, "filter", "");
        l.filterValues = field<json>(j, "filterValues", json::object());
        l.anchor = field<std::string>(j, "anchor", "");
        if (!l.anchor.empty() && !gn::validIdentifier(l.anchor))
            throw std::invalid_argument("anchor names are identifiers, not '" + l.anchor + "'");
        if (auto it = j.find("tiling"); it != j.end() && it->is_object())
        {
            l.tiling.scale = std::max(0.001, field<double>(*it, "scale", 1.0));
            l.tiling.rotation = field<double>(*it, "rotation", 0.0);
            l.tiling.offsetX = field<double>(*it, "offsetX", 0.0);
            l.tiling.offsetY = field<double>(*it, "offsetY", 0.0);
        }
        l.decal = field<json>(j, "decal", json::object());
        l.text = field<json>(j, "text", json::object());
        l.scatter = field<json>(j, "scatter", json::object());
        if (!l.decal.is_object() || !l.text.is_object() || !l.scatter.is_object())
            throw std::invalid_argument("decal, text and scatter must be objects");
        l.projection = field<json>(j, "projection", json::object());
        if (!l.projection.is_object())
            throw std::invalid_argument("projection must be an object");
        if (auto it = j.find("heightBlend"); it != j.end() && it->is_object())
        {
            l.heightBlend.enabled = field<bool>(*it, "enabled", false);
            l.heightBlend.sharpness = field<double>(*it, "sharpness", 0.8);
            l.heightBlend.influence = field<double>(*it, "influence", 1.0);
        }
        for (const json& m : field<json>(j, "masks", json::array()))
            l.masks.push_back(Mask::fromJson(m));
        for (const json& c : field<json>(j, "children", json::array()))
            l.children.push_back(Layer::fromJson(c));
        return l;
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(std::string("bad layer: ") + e.what());
    }
}

namespace
{
template <class L> L* findIn(std::vector<L>& list, std::string_view id)
{
    for (L& l : list)
    {
        if (l.id == id)
            return &l;
        if (L* c = findIn(l.children, id))
            return c;
    }
    return nullptr;
}

std::vector<Layer>* listIn(std::vector<Layer>& list, std::string_view id, size_t* index)
{
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (list[i].id == id)
        {
            *index = i;
            return &list;
        }
        if (std::vector<Layer>* c = listIn(list[i].children, id, index))
            return c;
    }
    return nullptr;
}

void eachLayer(const std::vector<Layer>& list, const std::function<void(const Layer&)>& fn)
{
    for (const Layer& l : list)
    {
        fn(l);
        eachLayer(l.children, fn);
    }
}
} // namespace

Layer* LayerStack::find(std::string_view id) { return findIn(layers, id); }

const Layer* LayerStack::find(std::string_view id) const { return findIn(const_cast<std::vector<Layer>&>(layers), id); }

std::vector<Layer>* LayerStack::listOf(std::string_view id, size_t* index) { return listIn(layers, id, index); }

std::string LayerStack::newId(char prefix, const std::set<std::string>& taken)
{
    std::set<std::string> used = taken;
    eachLayer(layers, [&](const Layer& l) {
        used.insert(l.id);
        for (const Mask& m : l.masks)
            used.insert(m.id);
    });
    for (;;)
    {
        std::string id = std::string(1, prefix) + std::to_string(nextId++);
        if (!used.contains(id))
            return id;
    }
}

std::map<std::string, std::string> LayerStack::anchors() const
{
    std::map<std::string, std::string> out;
    eachLayer(layers, [&](const Layer& l) {
        if (!l.anchor.empty())
            out[l.anchor] = l.id;
    });
    return out;
}

json LayerStack::toJson() const
{
    json ls = json::array();
    for (const Layer& l : layers)
        ls.push_back(l.toJson());
    json out = {{"version", 1}, {"nextId", nextId}, {"base", base}, {"heightNormal", heightNormal}, {"layers", ls}};
    if (!variants.empty())
        out["variants"] = variants;
    if (!variant.empty())
        out["variant"] = variant;
    return out;
}

void mergeOverride(json& into, const json& patch)
{
    if (!into.is_object() || !patch.is_object())
    {
        into = patch;
        return;
    }
    for (const auto& [k, v] : patch.items())
        if (v.is_object() && into.contains(k) && into[k].is_object())
            mergeOverride(into[k], v);
        else
            into[k] = v;
}

namespace
{
/** Applies an override: merge patch semantics (null drops a value), with maskOverrides going to the masks by id. */
void applyOverride(Layer& l, const json& patch)
{
    json j = l.toJson();
    const json id = j["id"];
    json p = patch;
    json masks;
    if (p.contains("maskOverrides"))
    {
        masks = p["maskOverrides"];
        p.erase("maskOverrides");
    }
    j.merge_patch(p);
    j["id"] = id;
    if (masks.is_object() && j.contains("masks") && j["masks"].is_array())
        for (json& m : j["masks"])
            if (m.is_object() && masks.contains(m.value("id", std::string())))
            {
                const json mid = m["id"];
                m.merge_patch(masks[m.value("id", std::string())]);
                m["id"] = mid;
            }
    l = Layer::fromJson(j);
}
} // namespace

LayerStack withVariant(const LayerStack& stack)
{
    LayerStack out = stack;
    if (stack.variant.empty())
        return out;
    auto it = stack.variants.find(stack.variant);
    if (it == stack.variants.end() || !it->is_object())
        return out;
    for (const auto& [id, patch] : it->items())
        if (Layer* l = out.find(id); l && patch.is_object())
            applyOverride(*l, patch);
    return out;
}

LayerStack LayerStack::fromJson(const json& j)
{
    if (!j.is_object())
        throw std::invalid_argument("a layer stack must be an object");
    try
    {
        LayerStack s;
        s.nextId = field<int>(j, "nextId", 1);
        s.base = field<json>(j, "base", json::object());
        if (!s.base.is_object())
            throw std::invalid_argument("base must be an object");
        s.heightNormal = field<double>(j, "heightNormal", 1.0);
        for (const json& l : field<json>(j, "layers", json::array()))
            s.layers.push_back(Layer::fromJson(l));
        s.variants = field<json>(j, "variants", json::object());
        if (!s.variants.is_object())
            throw std::invalid_argument("variants must be an object");
        s.variant = field<std::string>(j, "variant", std::string());
        return s;
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(std::string("bad layer stack: ") + e.what());
    }
}

// Channels ----------------------------------------------------------------------------------------------------------

bool channelOn(const LayerStack& stack, const Layer& layer, const std::string& channel)
{
    if (auto it = layer.channels.find(channel); it != layer.channels.end())
        return it->second.enabled;
    switch (layer.kind)
    {
    case LayerKind::Fill: return layer.material.contains(channel);
    case LayerKind::Decal: return layer.material.contains(channel) || layer.decal.value("maps", json::object()).contains(channel);
    case LayerKind::Text: return channel == "baseColor" || layer.material.contains(channel);
    case LayerKind::Scatter: return layer.material.contains(channel) || layer.scatter.value("maps", json::object()).contains(channel);
    // Unpainted channels leave the stack below alone; the material is what the brushes start from.
    case LayerKind::Paint: return !paintImage(layer, channel).empty();
    case LayerKind::Procedural:
        for (const json& o : layer.graph.is_object() ? layer.graph.value("outputs", json::array()) : json::array())
            if (o.value("name", std::string()) == channel)
                return true;
        return false;
    case LayerKind::Instance:
    {
        const Layer* src = stack.find(layer.source);
        return src && src != &layer && src->kind != LayerKind::Instance && channelOn(stack, *src, channel);
    }
    default: return true;
    }
}

std::string channelMode(const Layer& layer, const std::string& channel)
{
    if (auto it = layer.channels.find(channel); it != layer.channels.end() && !it->second.mode.empty())
        return it->second.mode;
    if (channel == "normal")
        return "rnm";
    if (channel == "height" && layer.kind != LayerKind::Filter)
        return "addSigned";
    return "normal";
}

json channelDefault(const std::string& name, PortType type)
{
    if (const NodeDef* d = findNode(kMaterialOutputType))
        if (const PortDecl* p = d->input(name))
            return p->defaultValue;
    if (isColorType(type))
        return json::array({0.0, 0.0, 0.0, 1.0});
    return 0.0;
}

// Compile and read back ---------------------------------------------------------------------------------------------

StackCompile compileStack(const LayerStack& stack, const SetSettings& settings, const Graph& previous)
{
    return Compiler(stack, settings, previous).run(nullptr);
}

StackState readStack(const Graph& g, const SetSettings& settings)
{
    LILA_ZONE();
    StackState st;
    const GNode* out = g.firstOfType(kMaterialOutputType);
    auto meta = out ? out->values.find(kStackKey) : json::const_iterator();
    if (!out || meta == out->values.end())
    {
        // A graph never layered: empty (only its output, nothing linked) reads as an empty stack.
        const bool empty = out && g.nodes.size() == 1 && g.links.empty();
        st.layered = empty;
        st.graphMode = !empty;
        if (empty)
            for (const auto& [k, v] : out->values.items())
                st.stack.base[k] = v;
        return st;
    }
    try
    {
        st.stack = LayerStack::fromJson(*meta);
    }
    catch (const std::exception&)
    {
        st.graphMode = st.edited = true;
        return st;
    }

    std::vector<Binding> bindings;
    StackCompile first;
    try
    {
        first = Compiler(st.stack, settings, g).run(&bindings);
    }
    catch (const std::exception&)
    {
        st.graphMode = st.edited = true;
        return st;
    }

    // Fold in what was edited in the graph: wherever a bound value there differs from what the stack compiles to.
    json sj = st.stack.toJson();
    bool changed = false;
    auto findIn = [](const Graph& graph, const Binding& b, std::optional<Graph>& holder) -> const Graph* {
        if (b.subgroup.empty())
            return &graph;
        auto it = graph.subgroups.find(b.subgroup);
        if (it == graph.subgroups.end())
            return nullptr;
        try
        {
            holder = Graph::fromJson(it->second);
        }
        catch (const std::exception&)
        {
            return nullptr;
        }
        return &*holder;
    };
    for (const Binding& b : bindings)
    {
        json actual, compiled;
        if (b.node.empty())
        {
            auto a = g.subgroups.find(b.subgroup);
            auto c = first.graph.subgroups.find(b.subgroup);
            if (a == g.subgroups.end() || c == first.graph.subgroups.end())
                continue;
            if (normalized(a->second) == normalized(c->second))
                continue;
            actual = a->second;
            actual.erase("name");
        }
        else
        {
            std::optional<Graph> ha, hc;
            const Graph* ga = findIn(g, b, ha);
            const Graph* gc = findIn(first.graph, b, hc);
            const GNode* na = ga ? ga->node(b.node) : nullptr;
            const GNode* nc = gc ? gc->node(b.node) : nullptr;
            if (!na || !nc || na->type != nc->type)
                continue;
            if (b.key.empty())
            {
                actual = na->values;
                compiled = nc->values;
            }
            else
            {
                actual = na->values.contains(b.key) ? na->values[b.key] : defaultFor(*ga, *na, b.key);
                compiled = nc->values.contains(b.key) ? nc->values[b.key] : json();
            }
            if (actual == compiled)
                continue;
        }
        try
        {
            sj[json::json_pointer(b.pointer)] = actual;
            changed = true;
        }
        catch (const json::exception&)
        {
        }
    }
    LayerStack folded = st.stack;
    if (changed)
    {
        try
        {
            folded = LayerStack::fromJson(sj);
        }
        catch (const std::exception&)
        {
            st.graphMode = st.edited = true;
            return st;
        }
    }
    StackCompile second;
    try
    {
        second = changed ? Compiler(folded, settings, g).run(nullptr) : std::move(first);
    }
    catch (const std::exception&)
    {
        st.graphMode = st.edited = true;
        return st;
    }
    if (normalized(second.graph.toJson()) != normalized(g.toJson()))
    {
        st.graphMode = st.edited = true;
        return st;
    }
    st.stack = std::move(folded);
    st.layered = true;
    return st;
}

Layer graphAsLayer(const Graph& g, const SetSettings& settings, json* base)
{
    Graph c = g;
    c.subgroups = g.subgroups;
    Layer l;
    l.kind = LayerKind::Procedural;
    l.name = "Graph";
    GNode* out = nullptr;
    for (GNode& n : c.nodes)
        if (n.type == kMaterialOutputType)
            out = &n;
    if (!out)
    {
        l.graph = c.toJson();
        return l;
    }
    const std::string old = out->name;
    out->type = kGroupOutputType;
    out->name = c.uniqueNodeName("GroupOutput");
    const json values = out->values;
    out->values = json::object();
    for (GLink& k : c.links)
        if (k.toNode == old)
            k.toNode = out->name;
    std::vector<GLink> material = c.linksInto(out->name, "material");
    std::erase_if(c.links, [&](const GLink& k) { return k.toNode == out->name && k.toSocket == "material"; });
    const std::string outName = out->name;
    std::string split;
    if (!material.empty())
    {
        split = c.uniqueNodeName("MaterialSplit");
        c.nodes.push_back({split, "materialChannels", {0.0, 0.0}, json::object()});
        c.links.push_back({material.front().fromNode, material.front().fromSocket, split, "material"});
    }
    for (const Channel& ch : settings.channels)
    {
        const bool linked = !c.linksInto(outName, ch.name).empty();
        const bool fromMaterial = !linked && !split.empty() &&
                                  std::any_of(standardChannels().begin(), standardChannels().end(), [&](const auto& sc) { return sc.first == ch.name; });
        if (fromMaterial)
            c.links.push_back({split, ch.name, outName, ch.name});
        if (linked || fromMaterial)
            c.outputs.push_back({ch.name, ch.type, {}});
        else if (base && values.contains(ch.name))
            (*base)[ch.name] = values[ch.name];
    }
    l.graph = c.toJson();
    return l;
}

std::vector<std::string> externalsUsed(const Graph& g)
{
    std::set<std::string> out;
    collectExternals(g.toJson(), g.subgroups, out, 0);
    return {out.begin(), out.end()};
}

json stackView(const StackState& state, const SetSettings& settings, const Graph& g)
{
    // The layers as the active variant shows them.
    const LayerStack shown = withVariant(state.stack);
    json layers = json::array();
    for (const Layer& l : shown.layers)
        layers.push_back(layerView(shown, l, settings));
    json channels = json::array();
    for (const Channel& c : settings.channels)
        channels.push_back(c.name);
    json variants = json::array();
    for (const auto& [name, v] : state.stack.variants.items())
        variants.push_back(name);
    return {{"mode", state.graphMode ? "graph" : "stack"},
            {"variant", state.stack.variant},
            {"variants", variants},
            {"edited", state.edited},
            {"channels", channels},
            {"base", state.stack.base},
            {"heightNormal", state.stack.heightNormal},
            {"layers", layers},
            {"anchors", state.stack.anchors()},
            {"maps", externalsUsed(g)}};
}
} // namespace lila::tex
