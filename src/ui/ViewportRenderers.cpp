#include "ui/ViewportRenderers.h"

#include "render/GpuImage.h"

#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "ops/Command.h"

#include <map>
#include <mutex>
#include <optional>
#include <span>

namespace lila::ui::viewportRenderers
{
namespace
{
/** Where a session's frames land, shared with its Deliver functions so they outlive nothing. */
struct Delivered
{
    std::mutex mutex;
    bool closed = false;
    uint64_t newest = 0;
    render::TexturePtr color, depth;
    bool bottomUp = false, premultiplied = false;
    float exposure = 1.0f;
    std::optional<glm::vec4> depthUnproject;
    /** CPU frames upload into these, reused while the size and format hold. */
    render::GpuImageRef colorImage, depthImage;
};

struct View
{
    std::string renderer = kLila;
    std::shared_ptr<Session> session;
    std::shared_ptr<Delivered> delivered;
    uint64_t serial = 0;
    std::optional<Request> last;
    std::function<void()> redraw;
    std::function<double()> refreshHz;
    /** The renderer this view lost when its extension unloaded; it switches back when that id registers again. */
    std::string lost;
};

std::map<int, View>& views()
{
    static std::map<int, View> v;
    return v;
}

std::function<int()>& activeFn()
{
    static std::function<int()> fn;
    return fn;
}

uint64_t gRevision = 1;
int gGraphListener = 0;
constexpr const char* kSettingsFile = "viewportRenderers";

std::map<int, std::function<void(int)>>& listeners()
{
    static std::map<int, std::function<void(int)>> l;
    return l;
}

void notify(int panel)
{
    auto copy = listeners();
    for (auto& [id, fn] : copy)
        fn(panel);
}

void redrawPanel(int panel)
{
    auto it = views().find(panel);
    if (it != views().end() && it->second.redraw)
        it->second.redraw();
}

/** Ends v's session; frames still on their way are dropped. */
void close(View& v)
{
    if (v.delivered)
    {
        std::scoped_lock lock(v.delivered->mutex);
        v.delivered->closed = true;
    }
    v.delivered.reset();
    v.session.reset();
    v.renderer = kLila;
    v.last.reset();
}

/** Forwards scene changes to the sessions and counts the ones that change what a renderer draws. */
void ensureGraphListener()
{
    if (gGraphListener)
        return;
    gGraphListener = scene::Graph::current().addListener([](scene::Change change, scene::NodeId node) {
        if (change != scene::Change::Selection)
            ++gRevision;
        std::vector<std::shared_ptr<Session>> sessions;
        for (auto& [panel, v] : views())
            if (v.session)
                sessions.push_back(v.session);
        for (const std::shared_ptr<Session>& s : sessions)
        {
            try
            {
                s->sceneChanged(change, node);
            }
            catch (const std::exception& e)
            {
                log::error("viewport renderer: sceneChanged: {}", e.what());
            }
        }
    });
}

render::GpuImageRef imageFor(render::GpuImageRef& keep, int w, int h, render::TextureFormat format, bool srgb)
{
    if (!keep || keep->width() != w || keep->height() != h || keep->format() != format || keep->desc().srgb != srgb)
        keep = render::GpuImage::create({.width = w, .height = h, .format = format, .srgb = srgb, .name = "viewport renderer"});
    return keep;
}

/** Checks a frame's buffers against its size; the reason it is unusable, or empty. */
std::string invalid(const Frame& f)
{
    const size_t pixels = size_t(std::max(f.width, 0)) * size_t(std::max(f.height, 0));
    if (!f.image && (f.width <= 0 || f.height <= 0))
        return "no size";
    if (!f.image && f.rgba8.size() != pixels * 4 && f.rgba32f.size() != pixels * 4 &&
        (f.texels.empty() || f.texels.size() != pixels * size_t(render::texelBytes(f.texelFormat))))
        return "colour does not match its size";
    if (!f.image && !f.texels.empty() && f.texelFormat != render::TextureFormat::RGBA8 && f.texelFormat != render::TextureFormat::RGBA16F)
        return "texels are not RGBA8 or RGBA16F";
    if (!f.depthImage && !f.depth.empty() && f.depth.size() != pixels)
        return "depth does not match its size";
    if (f.image && f.image->format() != render::TextureFormat::RGBA8 && f.image->format() != render::TextureFormat::RGBA16F)
        return "image is not RGBA8 or RGBA16F";
    if (f.depthImage && f.depthImage->format() != render::TextureFormat::R32F)
        return "depth image is not R32F";
    return {};
}

void accept(const std::shared_ptr<Delivered>& d, int panel, Frame f)
{
    if (std::string why = invalid(f); !why.empty())
    {
        log::warn("viewport renderer: dropped a frame for panel {}: {}", panel, why);
        return;
    }
    {
        std::scoped_lock lock(d->mutex);
        if (d->closed || f.serial < d->newest)
            return;
        d->newest = f.serial;
        if (f.image)
            d->color = f.image->asTexture();
        else if (f.rgba8.size() == size_t(f.width) * f.height * 4)
        {
            render::GpuImageRef img = imageFor(d->colorImage, f.width, f.height, render::TextureFormat::RGBA8, true);
            img->update(0, 0, f.width, f.height, f.rgba8);
            d->color = img->asTexture();
        }
        else if (!f.texels.empty())
        {
            render::GpuImageRef img = imageFor(d->colorImage, f.width, f.height, f.texelFormat, false);
            img->update(0, 0, f.width, f.height, std::move(f.texels));
            d->color = img->asTexture();
        }
        else
        {
            render::GpuImageRef img = imageFor(d->colorImage, f.width, f.height, render::TextureFormat::RGBA16F, false);
            img->updateRgba(0, 0, f.width, f.height, f.rgba32f);
            d->color = img->asTexture();
        }
        if (f.depthImage)
            d->depth = f.depthImage->asTexture();
        else if (!f.depth.empty())
        {
            render::GpuImageRef img = imageFor(d->depthImage, f.width, f.height, render::TextureFormat::R32F, false);
            const auto* bytes = reinterpret_cast<const uint8_t*>(f.depth.data());
            img->update(0, 0, f.width, f.height, std::span(bytes, f.depth.size() * sizeof(float)));
            d->depth = img->asTexture();
        }
        else
            d->depth = nullptr;
        d->bottomUp = f.bottomUp;
        d->premultiplied = f.premultiplied;
        d->exposure = f.exposure;
        d->depthUnproject.reset();
        if (f.glDepthProj)
        {
            const glm::dmat4 inv = glm::inverse(*f.glDepthProj);
            // View z = (zz * ndc + zw) / (wz + ww - wz * (1 - ndc)), as the composite shader takes it.
            d->depthUnproject = glm::vec4(float(inv[2][2]), float(inv[3][2]), float(inv[3][3] + inv[2][3]), float(-inv[2][3]));
        }
    }
    mainq::post([panel] { redrawPanel(panel); });
}

bool sameRequest(const Request& a, const Request& b)
{
    return a.width == b.width && a.height == b.height && a.view.view == b.view.view && a.view.proj == b.view.proj && a.view.ortho == b.view.ortho &&
           a.view.camera == b.view.camera && a.nearClip == b.nearClip && a.farClip == b.farClip && a.time == b.time &&
           a.sceneRevision == b.sceneRevision && a.exposure == b.exposure && a.gamma == b.gamma;
}

void dropRenderer(const std::string& id)
{
    std::vector<int> dropped;
    for (auto& [panel, v] : views())
        if (v.renderer == id)
        {
            close(v);
            v.lost = id;
            if (v.redraw)
                v.redraw();
            dropped.push_back(panel);
            log::info("viewport {}: renderer {} was removed, drawing with Lila's until it comes back", panel, id);
        }
    for (int panel : dropped)
        notify(panel);
}

void restoreRenderer(const std::string& id)
{
    std::vector<int> panels;
    for (auto& [panel, v] : views())
        if (v.lost == id)
            panels.push_back(panel);
    for (int panel : panels)
        if (select(panel, id))
            log::info("viewport {}: renderer {} is back", panel, id);
}

void applySaved(const std::string& renderer, Session& s)
{
    const nlohmann::json saved = savedSettings(renderer);
    for (auto& [key, value] : saved.items())
        try
        {
            if (!s.setSetting(key, value))
                log::warn("viewport renderer {}: setting {} = {} was refused", renderer, key, value.dump());
        }
        catch (const std::exception& e)
        {
            log::error("viewport renderer {}: setting {}: {}", renderer, key, e.what());
        }
}
} // namespace

void Request::ray(double x, double y, glm::dvec3& origin, glm::dvec3& dir) const
{
    const double sx = width > 0 ? view.width / width : 1.0, sy = height > 0 ? view.height / height : 1.0;
    view.ray(QPointF(x * sx, y * sy), origin, dir);
}

double Request::depthOf(const glm::dvec3& world) const { return -(view.view * glm::dvec4(world, 1.0)).z; }

Registry<Renderer>& registry()
{
    static Registry<Renderer> r;
    static const int listener = r.addListener([](RegistryChange change, const std::string& id, const std::string&) {
        if (change == RegistryChange::Removed)
            dropRenderer(id);
        else
            // After the registering extension finishes loading, so the session opens on a ready renderer.
            mainq::post([id] { restoreRenderer(id); });
    });
    (void)listener;
    return r;
}

const Renderer& add(Renderer renderer)
{
    if (renderer.id == kLila || renderer.id.empty())
        throw std::invalid_argument("'" + renderer.id + "' is not a usable viewport renderer id");
    std::string id = renderer.id;
    return registry().add(std::move(id), std::move(renderer));
}

const Renderer* add(ext::Context& ctx, Renderer renderer)
{
    std::string id = renderer.id;
    try
    {
        if (id == kLila || id.empty())
            throw std::invalid_argument("'" + id + "' is not a usable viewport renderer id");
        ctx.add(registry(), "viewportRenderer", id, std::move(renderer));
        return registry().find(id);
    }
    catch (const std::exception& e)
    {
        log::error("extension {}: viewport renderer: {}", ctx.owner(), e.what());
        return nullptr;
    }
}

bool remove(std::string_view id) { return registry().remove(id); }

std::vector<std::pair<std::string, std::string>> choices()
{
    std::vector<std::pair<std::string, std::string>> out{{kLila, "Lila"}};
    for (const Renderer* r : registry().allInOrder())
        out.emplace_back(r->id, r->name.empty() ? r->id : r->name);
    return out;
}

bool select(int panel, std::string_view id)
{
    if (id.empty())
        id = kLila;
    View& v = views()[panel];
    // Any pick, the user's or the switch back, settles what the view waits for.
    v.lost.clear();
    if (v.renderer == id)
        return true;
    if (id == kLila)
    {
        close(v);
        if (v.redraw)
            v.redraw();
        notify(panel);
        return true;
    }
    const Renderer* r = registry().find(id);
    if (!r || !r->open)
        return false;
    std::unique_ptr<Session> session;
    try
    {
        session = r->open(panel);
    }
    catch (const std::exception& e)
    {
        log::error("viewport renderer {}: {}", r->id, e.what());
    }
    if (!session)
        return false;
    applySaved(r->id, *session);
    close(v);
    v.renderer = std::string(id);
    v.session = std::move(session);
    v.delivered = std::make_shared<Delivered>();
    ensureGraphListener();
    if (v.redraw)
        v.redraw();
    notify(panel);
    return true;
}

std::string selected(int panel)
{
    auto it = views().find(panel);
    return it == views().end() ? kLila : it->second.renderer;
}

nlohmann::json savedSettings(std::string_view renderer)
{
    nlohmann::json s = PrefStore::get().value(kSettingsFile, renderer, nlohmann::json::object());
    return s.is_object() ? s : nlohmann::json::object();
}

bool setSetting(int panel, const std::string& key, const nlohmann::json& value)
{
    auto it = views().find(panel);
    if (it == views().end() || !it->second.session)
        return false;
    const std::string renderer = it->second.renderer;
    if (!it->second.session->setSetting(key, value))
        return false;
    nlohmann::json saved = savedSettings(renderer);
    saved[key] = value;
    PrefStore::get().set(kSettingsFile, renderer, saved);
    for (auto& [other, v] : views())
    {
        if (v.renderer != renderer || !v.session)
            continue;
        if (other != panel)
            v.session->setSetting(key, value);
        v.last.reset();
        if (v.redraw)
            v.redraw();
    }
    return true;
}

std::vector<Setting> settings(int panel)
{
    auto it = views().find(panel);
    return it == views().end() || !it->second.session ? std::vector<Setting>{} : it->second.session->settings();
}

nlohmann::json stats(int panel)
{
    auto it = views().find(panel);
    return it == views().end() || !it->second.session ? nlohmann::json() : it->second.session->stats();
}

int addListener(std::function<void(int panel)> fn)
{
    static int next = 0;
    listeners()[++next] = std::move(fn);
    return next;
}

void removeListener(int id) { listeners().erase(id); }

void setActivePanel(std::function<int()> fn) { activeFn() = std::move(fn); }

int activePanel() { return activeFn() ? activeFn()() : 0; }

bool apply(int panel, Request r, render::FrameState& s)
{
    auto it = views().find(panel);
    if (it == views().end() || !it->second.session)
        return false;
    View& v = it->second;
    r.panel = panel;
    r.graph = &scene::Graph::current();
    r.sceneRevision = gRevision;
    if (!v.last || !sameRequest(*v.last, r))
    {
        r.serial = ++v.serial;
        std::weak_ptr<Delivered> weak = v.delivered;
        r.deliver = [weak, panel](Frame f) {
            if (std::shared_ptr<Delivered> d = weak.lock())
                accept(d, panel, std::move(f));
        };
        v.last = r;
        // The session may deliver at once, or even switch the view to another renderer, so it is held across the call.
        std::shared_ptr<Session> session = v.session;
        try
        {
            session->render(r);
        }
        catch (const std::exception& e)
        {
            log::error("viewport renderer {}: {}", v.renderer, e.what());
        }
    }
    auto again = views().find(panel);
    if (again == views().end() || !again->second.delivered)
        return false;
    s.externalScene = true;
    std::scoped_lock lock(again->second.delivered->mutex);
    const Delivered& d = *again->second.delivered;
    s.externalColor = d.color;
    s.externalDepth = d.depth;
    s.externalBottomUp = d.bottomUp;
    s.externalPremultiplied = d.premultiplied;
    s.externalExposure = d.exposure;
    s.externalDepthUnproject = d.depthUnproject;
    return true;
}

void setRedraw(int panel, std::function<void()> fn) { views()[panel].redraw = std::move(fn); }

void setRefreshRate(int panel, std::function<double()> fn) { views()[panel].refreshHz = std::move(fn); }

double refreshInterval(int panel)
{
    auto it = views().find(panel);
    const double hz = it != views().end() && it->second.refreshHz ? it->second.refreshHz() : 0.0;
    return 1.0 / (hz > 1.0 ? hz : 60.0);
}

void release(int panel)
{
    auto it = views().find(panel);
    if (it == views().end())
        return;
    close(it->second);
    views().erase(it);
}

void registerCommands()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    ops::registerCommand({
        .name = "viewportRenderer",
        .doc = "Pick the renderer a viewport draws with: \"lila\" for Lila's own, or one an extension registered (viewportRenderers lists "
               "them). Lila still draws selection, gizmos and the HUD over another renderer's image. With no renderer, only returns the "
               "viewport's current one.",
        .positionalDoc = "renderer id",
        .flags = {{.name = "panel", .shortName = "p", .type = ArgType::Int, .doc = "The viewport, numbered in the order they were made; "
                                                                                  "default the active one.",
                   .example = "viewportRenderer(\"lila\", panel=1)"}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const int panel = a.has("panel") ? a.get<int>("panel") : activePanel();
            if (a.positional.empty())
                return selected(panel);
            if (!a.positional[0].is_string())
                throw CommandError("viewportRenderer: a renderer id is expected");
            const std::string id = a.positional[0].get<std::string>();
            if (id != kLila && !registry().contains(id))
            {
                std::string known;
                for (const auto& [cid, name] : choices())
                    known += (known.empty() ? "" : ", ") + cid;
                throw CommandError("viewportRenderer: no renderer '" + id + "' (known: " + known + ")");
            }
            if (!select(panel, id))
                throw CommandError("viewportRenderer: '" + id + "' could not start for viewport " + std::to_string(panel));
            return selected(panel);
        },
        .examples = {"viewportRenderer(\"cpuShaded\")  # the active view draws with the example renderer\nviewportRenderer(\"lila\")"},
    });
    ops::registerCommand({
        .name = "viewportRendererSetting",
        .doc = "Read or change a setting of the renderer a viewport draws with (a Hydra delegate's render settings). With no key, returns "
               "them all as [{key, name, value}]; with a key, its value; with a key and a value, sets it. A set value is saved and "
               "given to every viewport using that renderer, now and whenever it starts.",
        .positionalDoc = "key, value",
        .flags = {{.name = "panel", .shortName = "p", .type = ArgType::Int, .doc = "The viewport; default the active one."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const int panel = a.has("panel") ? a.get<int>("panel") : activePanel();
            const std::vector<Setting> all = settings(panel);
            if (a.positional.empty())
            {
                json out = json::array();
                for (const Setting& s : all)
                    out.push_back({{"key", s.key}, {"name", s.name}, {"value", s.value}});
                return out;
            }
            if (!a.positional[0].is_string())
                throw CommandError("viewportRendererSetting: a setting key is expected");
            const std::string key = a.positional[0].get<std::string>();
            if (a.positional.size() < 2)
            {
                for (const Setting& s : all)
                    if (s.key == key)
                        return s.value;
                throw CommandError("viewportRendererSetting: " + selected(panel) + " has no setting '" + key + "'");
            }
            if (!setSetting(panel, key, a.positional[1]))
                throw CommandError("viewportRendererSetting: " + selected(panel) + " refused " + key + " = " + a.positional[1].dump());
            return a.positional[1];
        },
        .examples = {"viewportRendererSetting()  # [{key, name, value}, ...]\nviewportRendererSetting(\"myEngine:metersPerUnit\", 0.01)"},
    });
    ops::registerCommand({
        .name = "viewportRenderers",
        .doc = "The renderers a viewport can draw with, Lila's own first: [{id, name, doc}].",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::array();
            for (const auto& [id, name] : choices())
            {
                const Renderer* r = registry().find(id);
                out.push_back({{"id", id}, {"name", name}, {"doc", r ? r->doc : std::string("Lila's own viewport renderer.")}});
            }
            return out;
        },
    });
}
} // namespace lila::ui::viewportRenderers
