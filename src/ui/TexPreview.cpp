#include "ui/TexPreview.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "io/SceneFile.h"
#include "render/PaintGpu.h"
#include "scene/Graph.h"
// Plan.h has a member named like Qt's slots macro.
#pragma push_macro("slots")
#undef slots
#include "tex/Evaluate.h"
#include "tex/PaintStrokes.h"
#include "tex/Ports.h"
#include "tex/Sequence.h"
#include "tex/TextureSet.h"
#pragma pop_macro("slots")

#include <QCoreApplication>
#include <QPointer>
#include <QThreadPool>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <set>

namespace lila::ui::texPreview
{
namespace
{
using json = nlohmann::json;

struct Entry
{
    std::shared_ptr<const image::Image> image;
    std::string type;
    std::string error;
    uint64_t lastUse = 0;
};

struct Job
{
    uint64_t key = 0;
    tex::Plan plan;
    std::string output;
    tex::EvalOptions options;
    std::string type;
};

/** One output at one size. A newer edit queues behind the running render instead of piling up. */
struct Slot
{
    bool running = false;
    uint64_t runningKey = 0;
    std::optional<Job> queued;
    std::vector<std::function<void()>> waiting;
    uint64_t lastKey = 0;
};

struct State
{
    std::map<uint64_t, Entry> cache;
    std::map<std::string, Slot> outputs;
    size_t bytes = 0;
    uint64_t clock = 0;
    Renderer renderer;
    std::mutex rendererMutex;
};

State& state()
{
    static State s;
    return s;
}

constexpr size_t kCacheBytes = size_t(256) << 20;

QThreadPool& pool()
{
    static QThreadPool* p = [] {
        auto* tp = new QThreadPool();
        tp->setMaxThreadCount(2);
        return tp;
    }();
    return *p;
}

/** What a request needs before it can render: its cache key and slot, and the graph to plan from. */
struct Prepared
{
    std::string error;
    uint64_t key = 0;
    scene::NodeId set = scene::kNoNode;
    bool readsPaint = false;
    std::string slot;
    tex::Graph graph;
    tex::OutputRequest output;
    std::string type;
};

/** The nodes an output depends on: node and everything linked into it, transitively. */
std::set<std::string> upstream(const tex::Graph& g, const std::string& node)
{
    std::set<std::string> seen;
    std::vector<std::string> stack{node};
    while (!stack.empty())
    {
        std::string cur = stack.back();
        stack.pop_back();
        if (!seen.insert(cur).second)
            continue;
        for (const tex::GLink& l : g.links)
            if (l.toNode == cur)
                stack.push_back(l.fromNode);
    }
    return seen;
}

/** Bumped each time a set's paint changes, so a preview reading paint layers evaluates again. */
std::map<scene::NodeId, uint64_t>& paintVersions()
{
    static std::map<scene::NodeId, uint64_t> versions;
    static const int listener = render::paint::addChangeListener([](scene::NodeId set) { ++versions[set]; });
    (void)listener;
    return versions;
}

/**
 * Everything the output's pixels depend on, and nothing they don't (positions, unrelated nodes). readsPaint says whether
 * a paint layer's buffer is among them.
 */
uint64_t keyOf(const tex::Graph& g, const tex::SetSettings& s, const tex::OutputRequest& out, int size, bool& readsPaint)
{
    std::set<std::string> nodes = upstream(g, out.node);
    std::string text = out.node + "." + out.socket + "|" + out.field + "|" + std::to_string(size) + "|";
    bool groups = false;
    for (const tex::GNode& n : g.nodes)
        if (nodes.contains(n.name))
        {
            text += n.name + ":" + n.type + ":" + n.values.dump() + ";";
            groups |= n.type == tex::kGroupType || !tex::findNode(n.type) || tex::findNode(n.type)->kind == tex::NodeKind::Graph;
        }
    for (const tex::GLink& l : g.links)
        if (nodes.contains(l.toNode))
            text += l.fromNode + "." + l.fromSocket + ">" + l.toNode + "." + l.toSocket + ";";
    if (groups)
        for (const auto& [name, sub] : g.subgroups)
            text += name + "=" + sub.dump();
    json inputs = json::array();
    for (const tex::GraphInput& in : g.inputs)
        inputs.push_back({in.name, in.defaultValue});
    text += inputs.dump();
    if (out.node == (g.firstOfType(tex::kMaterialOutputType) ? g.firstOfType(tex::kMaterialOutputType)->name : std::string()))
        text += s.toJson().dump();
    readsPaint = text.find("external:paint/") != std::string::npos;
    return tex::hash64(text);
}

Prepared prepare(const Request& r)
{
    Prepared p;
    scene::Node* set = nullptr;
    tex::SetSettings settings;
    try
    {
        // r.set may name a node group as "Set/Group"; its previews read the group's inputs at their defaults.
        p.graph = tex::readGraphAt(scene::Graph::current(), r.set, &set);
        settings = tex::readSettings(*set);
        // Sequence files and keyed values at the current frame; they land in the node values, so the key tells frames apart.
        if (tex::sequence::animated(p.graph))
            tex::sequence::followTime(p.graph, scene::Graph::current(), *set, scene::Graph::current().time(), imageRoot());
    }
    catch (const std::exception& e)
    {
        p.error = e.what();
        return p;
    }
    if (!r.channel.empty())
    {
        bool found = false;
        for (const tex::OutputRequest& o : tex::channelRequests(p.graph, settings))
            if (o.name == r.channel)
            {
                p.output = o;
                found = true;
            }
        if (!found)
        {
            p.error = p.graph.firstOfType(tex::kMaterialOutputType) ? "no channel '" + r.channel + "'" : "the graph has no Material Output";
            return p;
        }
        p.type = tex::portTypeName(p.output.type);
        p.slot = r.set + "#" + r.channel;
    }
    else
    {
        const tex::GNode* n = p.graph.node(r.node);
        if (!n)
        {
            p.error = "no node '" + r.node + "'";
            return p;
        }
        tex::ChannelList extra = tex::extraChannels(settings);
        tex::NodePorts ports = tex::portsOf(p.graph, *n, &extra);
        if (!ports.error.empty())
        {
            p.error = ports.error;
            return p;
        }
        const tex::PortDecl* o = r.socket.empty() ? (ports.outputs.empty() ? nullptr : &ports.outputs.front()) : ports.output(r.socket);
        if (!o)
        {
            p.error = r.socket.empty() ? r.node + " has no outputs" : r.node + " has no output '" + r.socket + "'";
            return p;
        }
        p.output = {"preview", n->name, o->id, o->type, {}};
        if (o->type == tex::PortType::Material)
        {
            p.output.field = "baseColor";
            p.output.type = tex::PortType::Color;
        }
        p.type = tex::portTypeName(p.output.type);
        p.slot = r.set + "/" + n->name + "." + o->id;
    }
    p.slot += "@" + std::to_string(r.size);
    p.set = set->id();
    // Only a preview that reads paint goes stale when the paint changes; the rest stay cached through strokes.
    p.key = keyOf(p.graph, settings, p.output, r.size, p.readsPaint);
    // Paint layers can sit in groups the key leaves out; the whole graph says whether any buffer is read.
    p.readsPaint = r.paint && (p.readsPaint || p.graph.toJson().dump().find("external:paint/") != std::string::npos);
    if (p.readsPaint)
        p.key ^= paintVersions()[p.set] * 0x9E3779B97F4A7C15ull;
    return p;
}

image::Image evaluate(const Renderer& renderer, const Job& job)
{
    if (renderer)
        return renderer(job.plan, job.output, job.options);
    // The GPU path caches compiled passes on disk; the CPU one compiles each pass through a C++ compiler, one at a time.
    tex::EvalResult r = tex::evaluate(job.plan, job.options, "");
    auto it = r.outputs.find(job.output);
    if (it == r.outputs.end())
        throw std::runtime_error("the evaluator returned no '" + job.output + "'");
    return std::move(it->second);
}

void evict()
{
    State& s = state();
    while (s.bytes > kCacheBytes && s.cache.size() > 1)
    {
        auto oldest = std::min_element(s.cache.begin(), s.cache.end(), [](const auto& a, const auto& b) { return a.second.lastUse < b.second.lastUse; });
        if (oldest->second.image)
            s.bytes -= oldest->second.image->rgba.size() * sizeof(float);
        s.cache.erase(oldest);
    }
}

void store(uint64_t key, Entry e)
{
    State& s = state();
    if (auto it = s.cache.find(key); it != s.cache.end() && it->second.image)
        s.bytes -= it->second.image->rgba.size() * sizeof(float);
    if (e.image)
        s.bytes += e.image->rgba.size() * sizeof(float);
    e.lastUse = ++s.clock;
    s.cache[key] = std::move(e);
    evict();
}

Job makeJob(Prepared& p, int size)
{
    Job job;
    job.key = p.key;
    tex::PlanRequest req;
    req.width = req.height = std::clamp(size, 4, 8192);
    req.outputs = {p.output};
    job.plan = tex::buildPlan(p.graph, req);
    job.output = p.output.name;
    job.options.imageRoot = imageRoot();
    // Paint layers read their buffers' tiles, as the live set does.
    for (const tex::BufferDesc& b : p.readsPaint ? job.plan.buffers : std::vector<tex::BufferDesc>{})
    {
        std::string buffer, channel;
        if (b.kind != tex::BufferKind::External || !tex::paint::parseExternalName(b.name, &buffer, &channel))
            continue;
        image::Image img = render::paint::readChannel(p.set, buffer, channel);
        if (img.width > 0)
            job.options.externals[b.name] = std::move(img);
    }
    job.type = p.type;
    return job;
}

void start(const std::string& slotName, Job job);

void finished(const std::string& slotName, uint64_t key, std::string type, std::shared_ptr<const image::Image> img, std::string error)
{
    State& s = state();
    store(key, {std::move(img), std::move(type), std::move(error)});
    Slot& slot = s.outputs[slotName];
    slot.running = false;
    slot.lastKey = key;
    if (slot.queued)
    {
        Job next = std::move(*slot.queued);
        slot.queued.reset();
        start(slotName, std::move(next));
    }
    std::vector<std::function<void()>> ready = std::move(slot.waiting);
    slot.waiting.clear();
    for (auto& fn : ready)
        if (fn)
            fn();
}

void start(const std::string& slotName, Job job)
{
    Slot& slot = state().outputs[slotName];
    slot.running = true;
    slot.runningKey = job.key;
    Renderer renderer;
    {
        std::lock_guard lock(state().rendererMutex);
        renderer = state().renderer;
    }
    QPointer<QCoreApplication> app(QCoreApplication::instance());
    pool().start([slotName, job = std::move(job), renderer = std::move(renderer), app] {
        LILA_ZONE_N("texPreview::render");
        std::shared_ptr<const image::Image> img;
        std::string error;
        try
        {
            img = std::make_shared<image::Image>(evaluate(renderer, job));
        }
        catch (const std::exception& e)
        {
            error = e.what();
        }
        if (!app)
            return;
        QMetaObject::invokeMethod(
            app, [slotName, key = job.key, type = job.type, img = std::move(img), error = std::move(error)]() mutable {
                finished(slotName, key, std::move(type), std::move(img), std::move(error));
            },
            Qt::QueuedConnection);
    });
}

Result resultOf(Entry& e, bool stale)
{
    e.lastUse = ++state().clock;
    return {e.image, e.type, stale, e.error};
}
} // namespace

std::filesystem::path imageRoot()
{
    if (Workspace::get().isOpen())
        return Workspace::get().root();
    if (!io::currentPath().empty())
        return io::currentPath().parent_path();
    return {};
}

Result get(const Request& request, std::function<void()> ready)
{
    LILA_ZONE();
    Prepared p = prepare(request);
    if (!p.error.empty())
        return {nullptr, {}, false, p.error};
    State& s = state();
    if (auto it = s.cache.find(p.key); it != s.cache.end())
        return resultOf(it->second, false);

    Slot& slot = s.outputs[p.slot];
    Result out{nullptr, p.type, true, {}};
    if (auto it = s.cache.find(slot.lastKey); slot.lastKey && it != s.cache.end())
        out = resultOf(it->second, true);
    if (ready)
        slot.waiting.push_back(std::move(ready));
    if ((slot.running && slot.runningKey == p.key) || (slot.queued && slot.queued->key == p.key))
        return out;
    Job job = makeJob(p, request.size);
    if (!job.plan.error.empty())
    {
        store(p.key, {nullptr, p.type, job.plan.error});
        slot.lastKey = p.key;
        slot.waiting.clear();
        return {nullptr, p.type, false, job.plan.error};
    }
    if (slot.running)
        slot.queued = std::move(job);
    else
        start(p.slot, std::move(job));
    return out;
}

Result render(const Request& request)
{
    Prepared p = prepare(request);
    if (!p.error.empty())
        return {nullptr, {}, false, p.error};
    if (auto it = state().cache.find(p.key); it != state().cache.end() && it->second.image)
        return resultOf(it->second, false);
    Job job = makeJob(p, request.size);
    if (!job.plan.error.empty())
        return {nullptr, p.type, false, job.plan.error};
    Renderer renderer;
    {
        std::lock_guard lock(state().rendererMutex);
        renderer = state().renderer;
    }
    try
    {
        auto img = std::make_shared<image::Image>(evaluate(renderer, job));
        store(p.key, {img, p.type, {}});
        state().outputs[p.slot].lastKey = p.key;
        return {img, p.type, false, {}};
    }
    catch (const std::exception& e)
    {
        return {nullptr, p.type, false, e.what()};
    }
}

bool animated(const std::string& set)
{
    if (set.empty())
        return false;
    try
    {
        return tex::sequence::animated(tex::readGraphAt(scene::Graph::current(), set));
    }
    catch (const std::exception&)
    {
        return false;
    }
}

void setRenderer(Renderer renderer)
{
    std::lock_guard lock(state().rendererMutex);
    state().renderer = std::move(renderer);
}

void clear()
{
    State& s = state();
    s.cache.clear();
    s.bytes = 0;
    for (auto& [name, slot] : s.outputs)
        slot.lastKey = 0;
}

bool isColor(const std::string& type, const std::string& name)
{
    if (type != "color")
        return false;
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return lower.find("normal") == std::string::npos;
}

QImage toQImage(const image::Image& img, const std::string& type, bool srgb, int channel)
{
    if (img.width <= 0 || img.height <= 0 || img.rgba.size() < size_t(img.width) * size_t(img.height) * 4)
        return {};
    QImage out(img.width, img.height, QImage::Format_RGBA8888);
    auto encode = [](float v, bool gamma) {
        v = std::clamp(v, 0.0f, 1.0f);
        if (gamma)
            v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        return uchar(std::lround(v * 255.0f));
    };
    const bool sdf = type == "sdf2d";
    for (int y = 0; y < img.height; ++y)
    {
        uchar* row = out.scanLine(y);
        const float* src = &img.rgba[size_t(y) * size_t(img.width) * 4];
        for (int x = 0; x < img.width; ++x, src += 4, row += 4)
        {
            if (channel >= 0 && channel < 4)
            {
                uchar v = encode(src[channel], false);
                row[0] = row[1] = row[2] = v;
                row[3] = 255;
                continue;
            }
            if (sdf)
            {
                // Inside dark, outside light, with the zero line crisp.
                float d = src[0];
                uchar v = encode(0.5f + d * 4.0f, false);
                row[0] = row[1] = row[2] = std::abs(d) < 0.004f ? uchar(255) : v;
                row[3] = 255;
                continue;
            }
            for (int c = 0; c < 3; ++c)
                row[c] = encode(src[c], srgb);
            row[3] = type == "color" ? encode(src[3], false) : uchar(255);
        }
    }
    return out;
}
} // namespace lila::ui::texPreview
