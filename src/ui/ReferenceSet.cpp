#include "ui/ReferenceSet.h"

#include "core/Events.h"
#include "core/FileBytes.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "io/SceneFile.h"
#include "media/Decode.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "ui/VideoTextures.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QTimer>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>

namespace lila::ui::references
{
namespace
{
namespace fs = std::filesystem;

const char* const kAnchors[] = {"center", "topLeft", "topRight", "bottomLeft", "bottomRight"};
const char* const kDepths[] = {"behind", "front"};
const char* const kModes[] = {"normal", "grayscale", "outline"};

template <size_t N> int indexOf(const char* const (&names)[N], const std::string& s, int fallback)
{
    for (size_t i = 0; i < N; ++i)
        if (s == names[i])
            return int(i);
    return fallback;
}

json vec(glm::dvec2 v) { return json::array({v.x, v.y}); }
json vec(glm::dvec3 v) { return json::array({v.x, v.y, v.z}); }

glm::dvec2 vec2(const json& j, glm::dvec2 fallback)
{
    if (!j.is_array() || j.size() < 2 || !j[0].is_number() || !j[1].is_number())
        return fallback;
    return {j[0].get<double>(), j[1].get<double>()};
}

glm::dvec3 vec3(const json& j, glm::dvec3 fallback)
{
    if (!j.is_array() || j.size() < 3 || !j[0].is_number() || !j[1].is_number() || !j[2].is_number())
        return fallback;
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

struct State
{
    Set set;
    std::map<int, std::function<void()>> listeners;
    int nextListener = 1;
    bool writePending = false;
    bool installed = false;
};

State& state()
{
    static State s;
    return s;
}

/** The first window exists, and each window's current tab is one of its own. */
void ensureMainWindow(Set& s)
{
    if (!s.window(kMainWindow))
        s.windows.insert(s.windows.begin(), Window{.id = kMainWindow});
    for (Window& w : s.windows)
        if (const Image* cur = s.image(w.current); !cur || cur->window != w.id)
        {
            auto tabs = s.tabs(w.id);
            w.current = tabs.empty() ? std::string() : tabs.front()->id;
        }
}

void notify()
{
    auto copy = state().listeners;
    for (auto& [id, fn] : copy)
        if (fn)
            fn();
}

void writeStorageFile()
{
    state().writePending = false;
    const fs::path file = storageFile(storage());
    if (file.empty())
        return;
    try
    {
        saveFile(state().set, file);
    }
    catch (const std::exception& e)
    {
        log::warn("reference images: {}", e.what());
    }
}

/** A changed set: the scene counts as modified, or the project/user file is rewritten shortly. */
void changed()
{
    if (storage() == Storage::Scene)
        io::uiChanged(kSection);
    else if (!state().writePending && QCoreApplication::instance())
    {
        state().writePending = true;
        QTimer::singleShot(500, QCoreApplication::instance(), writeStorageFile);
    }
    notify();
}

void replace(Set next)
{
    ensureMainWindow(next);
    state().set = std::move(next);
    changed();
}

struct SetEdit final : UndoAction
{
    json before, after;
    void undo() override { replace(Set::fromJson(before)); }
    void redo() override { replace(Set::fromJson(after)); }
    size_t bytes() const override { return before.dump().size() + after.dump().size(); }
};

/** Loads the project or user set when that is where sets live; a missing file leaves an empty set. */
void loadFromStorage()
{
    const Storage s = storage();
    if (s == Storage::Scene)
        return;
    Set next;
    if (const fs::path file = storageFile(s); !file.empty() && fs::exists(file))
        try
        {
            next = loadFile(file);
        }
        catch (const std::exception& e)
        {
            log::warn("reference images: {}", e.what());
        }
    ensureMainWindow(next);
    state().set = std::move(next);
    notify();
}

std::string requireImage(const Set& s, const std::string& id)
{
    if (!s.image(id))
        throw CommandError("no reference image '" + id + "'");
    return id;
}

std::string nameOf(const std::string& path)
{
    return paths::toUtf8(paths::fromUtf8(path).stem());
}

/** Applies the display flags of a referenceImage call to img. */
void applyImageFlags(Image& img, const Args& a)
{
    if (a.has("name"))
        img.name = a.get<std::string>("name");
    if (a.has("flipH"))
        img.flipH = a.get<bool>("flipH");
    if (a.has("flipV"))
        img.flipV = a.get<bool>("flipV");
    if (a.has("grayscale"))
        img.grayscale = a.get<bool>("grayscale");
    if (a.has("quarterTurns"))
        img.quarterTurns = ((a.get<int>("quarterTurns") % 4) + 4) % 4;
    if (a.has("brightness"))
        img.brightness = a.get<double>("brightness");
    if (a.has("contrast"))
        img.contrast = a.get<double>("contrast");
    if (a.has("window"))
        img.window = a.get<std::string>("window");
    if (a.has("videoOffset"))
        img.videoOffset = a.get<int>("videoOffset");
    if (a.has("videoFps"))
        img.videoFps = std::max(0.0, a.get<double>("videoFps"));
    if (a.has("pausedFrame"))
        img.pausedFrame = std::max<int64_t>(0, a.get<int64_t>("pausedFrame"));
    if (a.has("paused"))
    {
        const bool on = a.get<bool>("paused");
        // Pausing holds the frame showing now, unless the call names one.
        if (on && !img.paused && !a.has("pausedFrame"))
            img.pausedFrame = videoFrame(img, scene::Graph::current().time());
        img.paused = on;
    }
    if (a.has("embed"))
    {
        if (a.get<bool>("embed"))
        {
            if (isVideo(img))
                throw CommandError("videos stay linked to their file: embedding would copy the whole video into the set");
            if (!embed(img))
                throw CommandError("can't read '" + img.path + "' to embed it");
        }
        else if (!img.data.empty())
        {
            if (img.path.empty())
                throw CommandError("'" + img.id + "' has no file to link to; it only exists embedded");
            img.data.clear();
            img.ext.clear();
        }
    }
}

json imageJson(const Image& img)
{
    json j = Set{.images = {img}}.toJson()["images"][0];
    j.erase("data");
    j["embedded"] = !img.data.empty();
    j["file"] = fileOf(img);
    return j;
}

void registerCommands()
{
    ops::registerCommand({
        .name = "referenceImage",
        .doc = "Add, replace, edit, remove or list reference images.\n\n"
               "Reference images show as tabs in the Reference windows and as overlays in the viewports (referenceOverlay). "
               "Files given are added as tabs of a window (the first one unless window is set) and their ids returned. "
               "With edit, the flags change one image; replace swaps its file and keeps every other setting.",
        .positionalDoc = "files",
        .flags = {{.name = "edit", .shortName = "e", .type = ArgType::String, .doc = "Id of the image the flags change."},
                  {.name = "replace", .shortName = "rp", .type = ArgType::String, .doc = "Id of an image whose file becomes the one given."},
                  {.name = "remove", .shortName = "rm", .type = ArgType::List, .doc = "Ids of images to remove, with their overlays."},
                  {.name = "query", .shortName = "q", .type = ArgType::Bool, .doc = "Return the images (or the one named by edit) as dicts."},
                  {.name = "name", .shortName = "n", .type = ArgType::String, .doc = "Tab label."},
                  {.name = "window", .shortName = "w", .type = ArgType::String,
                   .doc = "Reference window id the images are tabs of (\"main\" is the first); empty takes them out of every window."},
                  {.name = "index", .shortName = "i", .type = ArgType::Int, .doc = "Tab position for added images; default the end."},
                  {.name = "current", .shortName = "c", .type = ArgType::Bool, .doc = "Make the (first) image its window's shown tab."},
                  {.name = "flipH", .shortName = "fh", .type = ArgType::Bool, .doc = "Mirror left to right."},
                  {.name = "flipV", .shortName = "fv", .type = ArgType::Bool, .doc = "Mirror top to bottom."},
                  {.name = "grayscale", .shortName = "g", .type = ArgType::Bool, .doc = "Show without color, to judge values."},
                  {.name = "quarterTurns", .shortName = "qt", .type = ArgType::Int, .doc = "Clockwise quarter turns, 0 to 3."},
                  {.name = "brightness", .shortName = "b", .type = ArgType::Float, .doc = "Added in display space, -1 to 1.", .min = -1.0, .max = 1.0},
                  {.name = "contrast", .shortName = "ct", .type = ArgType::Float, .doc = "Around mid gray, 0 to 4.", .min = 0.0, .max = 4.0},
                  {.name = "embed", .shortName = "em", .type = ArgType::Bool,
                   .doc = "Keep the file's bytes in the set, so it travels without the file; False links to the file again. "
                          "Videos always stay linked."},
                  {.name = "videoOffset", .shortName = "vo", .type = ArgType::Int, .doc = "Video: frames added to the frame shown."},
                  {.name = "videoFps", .shortName = "vf", .type = ArgType::Float, .doc = "Video: playback rate; 0 for the file's own.", .min = 0.0},
                  {.name = "paused", .shortName = "pa", .type = ArgType::Bool, .doc = "Video: hold the frame showing now instead of following the time."},
                  {.name = "pausedFrame", .shortName = "pf", .type = ArgType::Int, .doc = "Video: the frame a paused video holds."}},
        .run = [](const Args& a) -> json {
            Set next = current();
            if (a.get<bool>("query"))
            {
                if (a.has("edit"))
                    return imageJson(*next.image(requireImage(next, a.get<std::string>("edit"))));
                json out = json::array();
                for (const Image& img : next.images)
                    out.push_back(imageJson(img));
                return out;
            }
            if (a.has("remove"))
            {
                json ids = a.flags["remove"];
                if (ids.is_string())
                    ids = json::array({ids});
                for (const json& j : ids)
                {
                    const std::string id = requireImage(next, j.get<std::string>());
                    std::erase_if(next.images, [&](const Image& i) { return i.id == id; });
                    std::erase_if(next.overlays, [&](const Overlay& o) { return o.image == id; });
                    for (Window& w : next.windows)
                        if (w.current == id)
                            w.current.clear();
                }
                // A torn-off window left without tabs goes away; the first one stays.
                std::erase_if(next.windows, [&](const Window& w) { return w.id != kMainWindow && next.tabs(w.id).empty(); });
                apply(std::move(next), "Remove Reference Image");
                return ids;
            }
            if (a.has("replace"))
            {
                const std::string id = requireImage(next, a.get<std::string>("replace"));
                if (a.positional.empty() || !a.positional[0].is_string())
                    throw CommandError("replace needs the new file");
                const std::string file = a.positional[0].get<std::string>();
                if (!isImageFile(file))
                    throw CommandError("'" + file + "' is not an image file Lila reads");
                Image& img = *next.image(id);
                const bool wasEmbedded = !img.data.empty();
                img.path = paths::storedPath(paths::fromUtf8(file));
                img.data.clear();
                img.ext.clear();
                img.name = a.has("name") ? a.get<std::string>("name") : nameOf(file);
                if (wasEmbedded)
                    embed(img);
                apply(std::move(next), "Replace Reference Image");
                return id;
            }
            if (a.has("edit"))
            {
                const std::string id = requireImage(next, a.get<std::string>("edit"));
                Image& img = *next.image(id);
                applyImageFlags(img, a);
                if (!img.window.empty() && !next.window(img.window))
                    next.windows.push_back(Window{.id = img.window, .current = id});
                if (a.get<bool>("current") && !img.window.empty())
                    next.window(img.window)->current = id;
                apply(std::move(next), "Edit Reference Image");
                return id;
            }
            if (a.positional.empty())
                throw CommandError("give image files to add, or edit, replace, remove or query");
            const std::string window = a.has("window") ? a.get<std::string>("window") : std::string(kMainWindow);
            if (!window.empty() && !next.window(window))
                next.windows.push_back(Window{.id = window});
            // Insert position among the window's tabs, as an index into images.
            auto insertAt = next.images.end();
            if (a.has("index") && !window.empty())
            {
                int want = std::max(0, a.get<int>("index")), seen = 0;
                for (auto it = next.images.begin(); it != next.images.end(); ++it)
                    if (it->window == window && seen++ == want)
                    {
                        insertAt = it;
                        break;
                    }
            }
            json ids = json::array();
            std::vector<Image> added;
            for (const json& p : a.positional)
            {
                const std::string file = p.get<std::string>();
                if (!isImageFile(file))
                    throw CommandError("'" + file + "' is not an image file Lila reads");
                if (!fs::exists(paths::fromUtf8(file)))
                    throw CommandError("no file '" + file + "'");
                Image img;
                img.path = paths::storedPath(paths::fromUtf8(file));
                img.name = nameOf(file);
                img.window = window;
                {
                    Set probe = next;
                    probe.images.insert(probe.images.end(), added.begin(), added.end());
                    img.id = probe.newImageId();
                }
                applyImageFlags(img, a);
                img.window = window;
                ids.push_back(img.id);
                added.push_back(std::move(img));
            }
            const size_t at = size_t(insertAt - next.images.begin());
            next.images.insert(next.images.begin() + std::ptrdiff_t(at), added.begin(), added.end());
            if (Window* w = next.window(window); w && (w->current.empty() || a.get<bool>("current", true)))
                w->current = added.front().id;
            apply(std::move(next), "Add Reference Image");
            return ids.size() == 1 ? ids[0] : ids;
        },
        .examples = {"referenceImage(\"refs/front.jpg\", \"refs/side.jpg\")",
                     "referenceImage(\"refs/front_v2.jpg\", replace=\"ref1\")",
                     "referenceImage(edit=\"ref2\", flipH=True, grayscale=True)"},
    });

    ops::registerCommand({
        .name = "referenceSet",
        .doc = "Save, load, clear the reference image set, or say where it is kept.\n\n"
               "Storage scene keeps the set in the scene file; project in references.lilaref in the project folder; user in "
               "Lila's config folder, shared by every scene. Changing storage carries the current set over.",
        .flags = {{.name = "save", .shortName = "s", .type = ArgType::String, .doc = "Write the set to a .lilaref file."},
                  {.name = "load", .shortName = "l", .type = ArgType::String, .doc = "Replace the set with a .lilaref file's."},
                  {.name = "clear", .shortName = "c", .type = ArgType::Bool, .doc = "Remove every image and overlay."},
                  {.name = "storage", .shortName = "st", .type = ArgType::String, .doc = "Where the set is kept.",
                   .choices = {"scene", "project", "user"}},
                  {.name = "query", .shortName = "q", .type = ArgType::Bool, .doc = "Return the storage and the whole set."}},
        .run = [](const Args& a) -> json {
            if (a.get<bool>("query"))
            {
                json j = current().toJson();
                for (json& img : j["images"])
                    img.erase("data");
                return {{"storage", storageName(storage())}, {"file", paths::toUtf8(storageFile(storage()))}, {"set", j}};
            }
            if (a.has("storage"))
            {
                auto s = storageFromName(a.get<std::string>("storage"));
                if (!s)
                    throw CommandError("storage is scene, project or user");
                if (*s == Storage::Project && !Workspace::get().isOpen())
                    throw CommandError("no project is open to keep the set in");
                setStorage(*s);
                return storageName(*s);
            }
            if (a.has("save"))
            {
                fs::path file = paths::fromUtf8(a.get<std::string>("save"));
                try
                {
                    saveFile(current(), file);
                }
                catch (const std::exception& e)
                {
                    throw CommandError(e.what());
                }
                return paths::toUtf8(file);
            }
            if (a.has("load"))
            {
                Set next;
                try
                {
                    next = loadFile(paths::fromUtf8(a.get<std::string>("load")));
                }
                catch (const std::exception& e)
                {
                    throw CommandError(e.what());
                }
                apply(std::move(next), "Load Reference Set");
                return current().images.size();
            }
            if (a.get<bool>("clear"))
            {
                apply(Set{}, "Clear Reference Images");
                return true;
            }
            throw CommandError("give save, load, clear, storage or query");
        },
        .examples = {"referenceSet(save=\"refs/hero.lilaref\")", "referenceSet(storage=\"project\")"},
    });
}
} // namespace

nlohmann::json Set::toJson() const
{
    json imageList = json::array();
    for (const Image& i : this->images)
    {
        json j = {{"id", i.id}, {"name", i.name}, {"window", i.window}};
        if (!i.path.empty())
            j["path"] = i.path;
        if (!i.data.empty())
        {
            j["data"] = i.data;
            j["ext"] = i.ext;
        }
        if (i.flipH)
            j["flipH"] = true;
        if (i.flipV)
            j["flipV"] = true;
        if (i.grayscale)
            j["grayscale"] = true;
        if (i.quarterTurns)
            j["quarterTurns"] = i.quarterTurns;
        if (i.brightness != 0.0)
            j["brightness"] = i.brightness;
        if (i.contrast != 1.0)
            j["contrast"] = i.contrast;
        if (i.videoOffset)
            j["videoOffset"] = i.videoOffset;
        if (i.videoFps > 0.0)
            j["videoFps"] = i.videoFps;
        if (i.paused)
        {
            j["paused"] = true;
            j["pausedFrame"] = i.pausedFrame;
        }
        imageList.push_back(std::move(j));
    }
    json overlayList = json::array();
    for (const Overlay& o : this->overlays)
    {
        json j = {{"id", o.id},
                  {"image", o.image},
                  {"view", o.view},
                  {"anchor", kAnchors[int(o.anchor)]},
                  {"offset", vec(o.offset)},
                  {"scale", o.scale},
                  {"rotation", o.rotation},
                  {"opacity", o.opacity},
                  {"depth", kDepths[int(o.depth)]},
                  {"mode", kModes[int(o.mode)]},
                  {"visible", o.visible},
                  {"ghost", o.ghost},
                  {"outlineColor", json::array({o.outlineColor.r, o.outlineColor.g, o.outlineColor.b})},
                  {"fadeAngle", o.fadeAngle},
                  {"focalLength", o.focalLength}};
        if (o.crop != glm::dvec4(0.0, 0.0, 1.0, 1.0))
            j["crop"] = json::array({o.crop.x, o.crop.y, o.crop.z, o.crop.w});
        if (o.bound.set)
            j["bound"] = {{"target", vec(o.bound.target)}, {"distance", o.bound.distance}, {"yaw", o.bound.yaw},
                          {"pitch", o.bound.pitch}, {"focalLength", o.bound.focalLength}, {"ortho", o.bound.ortho},
                          {"orthoWidth", o.bound.orthoWidth}, {"forward", vec(o.bound.forward)}};
        overlayList.push_back(std::move(j));
    }
    json windowList = json::array();
    for (const Window& w : this->windows)
        windowList.push_back({{"id", w.id}, {"current", w.current}, {"opacity", w.opacity}, {"onTop", w.onTop}, {"clickThrough", w.clickThrough}});
    return {{"version", 1}, {"images", imageList}, {"overlays", overlayList}, {"windows", windowList}};
}

Set Set::fromJson(const nlohmann::json& j)
{
    Set s;
    if (!j.is_object())
        return s;
    for (const json& i : j.value("images", json::array()))
    {
        if (!i.is_object())
            continue;
        Image img;
        img.id = i.value("id", "");
        img.name = i.value("name", "");
        img.path = i.value("path", "");
        img.data = i.value("data", "");
        img.ext = i.value("ext", "");
        img.window = i.value("window", "");
        img.flipH = i.value("flipH", false);
        img.flipV = i.value("flipV", false);
        img.grayscale = i.value("grayscale", false);
        img.quarterTurns = ((i.value("quarterTurns", 0) % 4) + 4) % 4;
        img.brightness = i.value("brightness", 0.0);
        img.contrast = i.value("contrast", 1.0);
        img.videoOffset = i.value("videoOffset", 0);
        img.videoFps = i.value("videoFps", 0.0);
        img.paused = i.value("paused", false);
        img.pausedFrame = i.value("pausedFrame", int64_t(0));
        if (img.id.empty() || (img.path.empty() && img.data.empty()) || s.image(img.id))
            continue;
        s.images.push_back(std::move(img));
    }
    for (const json& i : j.value("overlays", json::array()))
    {
        if (!i.is_object())
            continue;
        Overlay o;
        o.id = i.value("id", "");
        o.image = i.value("image", "");
        o.view = i.value("view", "persp");
        o.anchor = Anchor(indexOf(kAnchors, i.value("anchor", ""), 0));
        o.offset = vec2(i.value("offset", json()), glm::dvec2(0.0));
        o.scale = std::max(0.01, i.value("scale", 0.6));
        o.rotation = i.value("rotation", 0.0);
        o.opacity = std::clamp(i.value("opacity", 0.3), 0.0, 1.0);
        o.depth = Depth(indexOf(kDepths, i.value("depth", ""), 1));
        o.mode = Mode(indexOf(kModes, i.value("mode", ""), 0));
        o.visible = i.value("visible", true);
        o.ghost = i.value("ghost", false);
        glm::dvec3 c = vec3(i.value("outlineColor", json()), glm::dvec3(o.outlineColor));
        o.outlineColor = glm::vec3(c);
        o.fadeAngle = std::clamp(i.value("fadeAngle", 15.0), 1.0, 180.0);
        o.focalLength = std::max(1.0, i.value("focalLength", 105.0));
        if (const json& cr = i.value("crop", json()); cr.is_array() && cr.size() == 4)
            o.crop = glm::clamp(glm::dvec4(cr[0].get<double>(), cr[1].get<double>(), cr[2].get<double>(), cr[3].get<double>()), 0.0, 1.0);
        if (const json& b = i.value("bound", json()); b.is_object())
        {
            o.bound.set = true;
            o.bound.target = vec3(b.value("target", json()), glm::dvec3(0.0));
            o.bound.distance = b.value("distance", 0.0);
            o.bound.yaw = b.value("yaw", 0.0);
            o.bound.pitch = b.value("pitch", 0.0);
            o.bound.focalLength = b.value("focalLength", 35.0);
            o.bound.ortho = b.value("ortho", false);
            o.bound.orthoWidth = b.value("orthoWidth", 0.0);
            o.bound.forward = vec3(b.value("forward", json()), glm::dvec3(0.0, 0.0, -1.0));
        }
        if (o.id.empty() || !s.image(o.image) || s.overlay(o.id))
            continue;
        s.overlays.push_back(std::move(o));
    }
    for (const json& i : j.value("windows", json::array()))
    {
        if (!i.is_object() || i.value("id", "").empty() || s.window(i.value("id", "")))
            continue;
        s.windows.push_back(Window{.id = i.value("id", ""),
                                   .current = i.value("current", ""),
                                   .opacity = std::clamp(i.value("opacity", 1.0), 0.1, 1.0),
                                   .onTop = i.value("onTop", false),
                                   .clickThrough = i.value("clickThrough", false)});
    }
    for (const Image& img : s.images)
        if (!img.window.empty() && !s.window(img.window))
            s.windows.push_back(Window{.id = img.window});
    ensureMainWindow(s);
    return s;
}

Image* Set::image(const std::string& id)
{
    auto it = std::ranges::find(images, id, &Image::id);
    return it == images.end() ? nullptr : &*it;
}

const Image* Set::image(const std::string& id) const { return const_cast<Set*>(this)->image(id); }

Overlay* Set::overlay(const std::string& id)
{
    auto it = std::ranges::find(overlays, id, &Overlay::id);
    return it == overlays.end() ? nullptr : &*it;
}

const Overlay* Set::overlay(const std::string& id) const { return const_cast<Set*>(this)->overlay(id); }

Window* Set::window(const std::string& id)
{
    auto it = std::ranges::find(windows, id, &Window::id);
    return it == windows.end() ? nullptr : &*it;
}

const Window* Set::window(const std::string& id) const { return const_cast<Set*>(this)->window(id); }

std::vector<const Image*> Set::tabs(const std::string& window) const
{
    std::vector<const Image*> out;
    for (const Image& i : images)
        if (i.window == window)
            out.push_back(&i);
    return out;
}

std::vector<const Overlay*> Set::overlaysIn(const std::string& view) const
{
    std::vector<const Overlay*> out;
    for (const Overlay& o : overlays)
        if (o.view == view)
            out.push_back(&o);
    return out;
}

namespace
{
template <class T> std::string freshId(const std::vector<T>& items, const char* prefix)
{
    for (int n = 1;; ++n)
    {
        std::string id = prefix + std::to_string(n);
        if (std::ranges::none_of(items, [&](const T& t) { return t.id == id; }))
            return id;
    }
}
} // namespace

std::string Set::newImageId() const { return freshId(images, "ref"); }
std::string Set::newOverlayId() const { return freshId(overlays, "overlay"); }
std::string Set::newWindowId() const { return freshId(windows, "window"); }

Storage storage()
{
    return storageFromName(PrefStore::get().value<std::string>("general", kStoragePref, "scene")).value_or(Storage::Scene);
}

void setStorage(Storage s)
{
    if (s == storage())
        return;
    const bool wasScene = storage() == Storage::Scene;
    PrefStore::get().set("general", kStoragePref, storageName(s));
    if (wasScene)
        io::uiChanged(kSection);
    if (s != Storage::Scene)
        writeStorageFile();
    else
        io::uiChanged(kSection);
    notify();
}

std::string storageName(Storage s)
{
    switch (s)
    {
    case Storage::Project: return "project";
    case Storage::User: return "user";
    default: return "scene";
    }
}

std::optional<Storage> storageFromName(const std::string& name)
{
    if (name == "scene")
        return Storage::Scene;
    if (name == "project")
        return Storage::Project;
    if (name == "user")
        return Storage::User;
    return std::nullopt;
}

std::filesystem::path storageFile(Storage s)
{
    if (s == Storage::User)
        return paths::userConfigDir() / kFileName;
    if (s == Storage::Project && Workspace::get().isOpen())
        return Workspace::get().root() / kFileName;
    return {};
}

const Set& current()
{
    return state().set;
}

void apply(Set next, const std::string& label)
{
    ensureMainWindow(next);
    if (label.empty())
    {
        replace(std::move(next));
        return;
    }
    auto edit = std::make_unique<SetEdit>();
    edit->before = state().set.toJson();
    edit->after = next.toJson();
    UndoStack& undo = UndoStack::get();
    const bool chunk = !undo.chunkOpen();
    if (chunk)
        undo.openChunk(label);
    undo.record(std::move(edit));
    replace(std::move(next));
    if (chunk)
        undo.closeChunk();
}

void tweak(const std::function<void(Set&)>& edit)
{
    edit(state().set);
    ensureMainWindow(state().set);
    changed();
}

int addListener(std::function<void()> fn)
{
    const int id = state().nextListener++;
    state().listeners[id] = std::move(fn);
    return id;
}

void removeListener(int id)
{
    state().listeners.erase(id);
}

std::string fileOf(const Image& img)
{
    if (img.data.empty())
        return paths::toUtf8(paths::resolvePath(img.path));
    const std::string name = std::to_string(std::hash<std::string>{}(img.data)) + "." + (img.ext.empty() ? "png" : img.ext);
    const fs::path file = paths::userConfigDir() / "referenceCache" / name;
    std::error_code ec;
    if (!fs::exists(file, ec))
        try
        {
            fs::create_directories(file.parent_path(), ec);
            const QByteArray bytes = QByteArray::fromBase64(QByteArray::fromStdString(img.data));
            writeFileBytes(file, std::string_view(bytes.constData(), size_t(bytes.size())));
        }
        catch (const std::exception& e)
        {
            log::warn("reference image {}: {}", img.id, e.what());
        }
    return paths::toUtf8(file);
}

bool isVideo(const Image& img)
{
    return media::isVideoFile(img.data.empty() ? img.path : "x." + img.ext);
}

int64_t videoFrame(const Image& img, double sceneSeconds)
{
    if (img.paused)
        return img.pausedFrame;
    return videoTextures::referenceFrame(fileOf(img), sceneSeconds, img.videoFps, img.videoOffset);
}

bool embed(Image& img)
{
    if (!img.data.empty())
        return true;
    if (isVideo(img))
    {
        log::warn("reference image {}: videos stay linked to their file", img.id);
        return false;
    }
    const fs::path file = paths::resolvePath(img.path);
    try
    {
        auto bytes = readFileBytes(file);
        img.data = QByteArray(bytes.data(), qsizetype(bytes.size())).toBase64().toStdString();
        std::string ext = paths::toUtf8(file.extension());
        img.ext = ext.empty() ? "png" : ext.substr(1);
        return true;
    }
    catch (const std::exception& e)
    {
        log::warn("reference image {}: {}", img.id, e.what());
        return false;
    }
}

bool isImageFile(const std::string& path)
{
    static const char* const kSuffixes[] = {".png", ".jpg", ".jpeg", ".tga", ".hdr", ".bmp", ".tif", ".tiff", ".webp", ".exr"};
    std::string ext = paths::toUtf8(paths::fromUtf8(path).extension());
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return std::ranges::any_of(kSuffixes, [&](const char* s) { return ext == s; }) || media::isVideoFile(path);
}

void saveFile(const Set& set, const std::filesystem::path& file)
{
    // Paths in the file are relative to the file, so a board folder can move as a whole.
    json j = set.toJson();
    const fs::path dir = file.parent_path();
    for (json& img : j["images"])
        if (img.contains("path"))
            img["path"] = paths::storedPath(paths::resolvePath(img["path"].get<std::string>()), dir);
    std::error_code ec;
    fs::create_directories(dir, ec);
    try
    {
        writeFileBytes(file, j.dump(1));
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error("can't write " + paths::toUtf8(file) + ": " + e.what());
    }
}

Set loadFile(const std::filesystem::path& file)
{
    json j;
    try
    {
        auto bytes = readFileBytes(file);
        j = json::parse(std::string_view(bytes.data(), bytes.size()));
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error("can't read " + paths::toUtf8(file) + ": " + e.what());
    }
    const fs::path dir = file.parent_path();
    for (json& img : j["images"])
        if (img.is_object() && img.contains("path"))
            img["path"] = paths::storedPath(paths::resolvePath(img["path"].get<std::string>(), dir));
    return Set::fromJson(j);
}

DropAction dropAction(bool shift, bool hasCurrentTab)
{
    return shift && hasCurrentTab ? DropAction::Replace : DropAction::AddTabs;
}

namespace
{
glm::dvec2 anchorPoint(Anchor a, glm::dvec2 view)
{
    switch (a)
    {
    case Anchor::TopLeft: return {0.0, 0.0};
    case Anchor::TopRight: return {view.x, 0.0};
    case Anchor::BottomLeft: return {0.0, view.y};
    case Anchor::BottomRight: return view;
    default: return view * 0.5;
    }
}
} // namespace

namespace
{
glm::dvec2 centreOf4(const std::array<glm::dvec2, 4>& c)
{
    return (c[0] + c[1] + c[2] + c[3]) * 0.25;
}

/** The cropped image's shown width over height, before quarter turns. */
double croppedAspect(const Overlay& o, double imageAspect)
{
    return imageAspect * std::max(o.crop.z - o.crop.x, 1e-6) / std::max(o.crop.w - o.crop.y, 1e-6);
}
} // namespace

std::array<glm::dvec2, 4> overlayCorners(const Overlay& o, const Image* img, double imageAspect, glm::dvec2 viewSize)
{
    imageAspect = croppedAspect(o, imageAspect);
    const int turns = img ? img->quarterTurns : 0;
    const double h = std::max(viewSize.y, 1.0);
    // Shown height is scale view heights; on a quarter turn that is the image's own width.
    double iw, ih;
    if (turns % 2 == 0)
    {
        ih = o.scale * h;
        iw = ih * imageAspect;
    }
    else
    {
        iw = o.scale * h;
        ih = iw / std::max(imageAspect, 1e-6);
    }
    const glm::dvec2 c = anchorPoint(o.anchor, viewSize) + o.offset * h;
    const double a = glm::radians(o.rotation + 90.0 * turns);
    const glm::dvec2 ax(std::cos(a), std::sin(a)), ay(-std::sin(a), std::cos(a));
    auto at = [&](double x, double y) { return c + ax * (x * iw * 0.5) + ay * (y * ih * 0.5); };
    return {at(-1, -1), at(1, -1), at(1, 1), at(-1, 1)};
}

bool overlayContains(const Overlay& o, const Image* img, double imageAspect, glm::dvec2 viewSize, glm::dvec2 px)
{
    auto c = overlayCorners(o, img, imageAspect, viewSize);
    bool pos = false, neg = false;
    for (int i = 0; i < 4; ++i)
    {
        glm::dvec2 e = c[size_t(i + 1) % 4] - c[size_t(i)], d = px - c[size_t(i)];
        double cross = e.x * d.y - e.y * d.x;
        (cross >= 0.0 ? pos : neg) = true;
    }
    return !(pos && neg);
}

glm::dvec4 overlayUv(const Overlay& o, const Image* img)
{
    glm::dvec4 uv = o.crop;
    if (img && img->flipH)
        std::swap(uv.x, uv.z);
    if (img && img->flipV)
        std::swap(uv.y, uv.w);
    return uv;
}

glm::dvec2 overlayImageUv(const Overlay& o, const Image* img, double imageAspect, glm::dvec2 viewSize, glm::dvec2 px)
{
    const auto c = overlayCorners(o, img, imageAspect, viewSize);
    const glm::dvec2 ex = c[1] - c[0], ey = c[3] - c[0], d = px - c[0];
    // Fractions across the shown quad from its first corner, which carries uv.xy.
    const double fx = glm::dot(d, ex) / std::max(glm::dot(ex, ex), 1e-12);
    const double fy = glm::dot(d, ey) / std::max(glm::dot(ey, ey), 1e-12);
    const glm::dvec4 uv = overlayUv(o, img);
    return {uv.x + (uv.z - uv.x) * fx, uv.y + (uv.w - uv.y) * fy};
}

void setCrop(Overlay& o, const Image* img, double imageAspect, glm::dvec2 viewSize, glm::dvec4 crop)
{
    crop = glm::clamp(crop, 0.0, 1.0);
    if (crop.z - crop.x < 0.01 || crop.w - crop.y < 0.01)
        return;
    const auto c = overlayCorners(o, img, imageAspect, viewSize);
    const glm::dvec4 uv = overlayUv(o, img);
    // Screen position of an image point under the current crop, so the new crop's centre lands where it shows now.
    auto at = [&](double u, double v) {
        const double fx = (u - uv.x) / (uv.z - uv.x), fy = (v - uv.y) / (uv.w - uv.y);
        return c[0] + (c[1] - c[0]) * fx + (c[3] - c[0]) * fy;
    };
    const glm::dvec2 centre = at((crop.x + crop.z) * 0.5, (crop.y + crop.w) * 0.5);
    const bool turned = img && img->quarterTurns % 2;
    const double before = turned ? o.crop.z - o.crop.x : o.crop.w - o.crop.y;
    const double after = turned ? crop.z - crop.x : crop.w - crop.y;
    o.scale *= after / std::max(before, 1e-9);
    o.crop = crop;
    const double h = std::max(viewSize.y, 1.0);
    const glm::dvec2 now = centreOf4(overlayCorners(o, img, imageAspect, viewSize));
    o.offset += (centre - now) / h;
}

double boundFade(const Overlay& o, const glm::dvec3& forward)
{
    if (!o.bound.set)
        return 1.0;
    const double len = glm::length(forward) * glm::length(o.bound.forward);
    if (len <= 0.0)
        return 1.0;
    const double angle = glm::degrees(std::acos(std::clamp(glm::dot(forward, o.bound.forward) / len, -1.0, 1.0)));
    // Full strength for the first third of the range, so a small orbit to check a form doesn't flicker it.
    const double t = std::clamp((angle - o.fadeAngle / 3.0) / (o.fadeAngle * 2.0 / 3.0), 0.0, 1.0);
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

void park(Set& set, const std::string& view, const std::function<double(const Image&)>& imageAspect, glm::dvec2 viewSize)
{
    std::vector<Overlay*> mine;
    for (Overlay& o : set.overlays)
        if (o.view == view)
            mine.push_back(&o);
    if (mine.empty())
        return;
    // Starts below the view cube in the top right corner.
    const double margin = 0.02, top = 0.12;
    const double slot = std::min(0.4, (1.0 - top) / double(mine.size()));
    const double maxWidth = 0.35 * viewSize.x / std::max(viewSize.y, 1.0);
    double y = top;
    for (Overlay* o : mine)
    {
        const Image* img = set.image(o->image);
        double aspect = img ? croppedAspect(*o, imageAspect(*img)) : 1.0;
        if (img && img->quarterTurns % 2)
            aspect = 1.0 / std::max(aspect, 1e-6);
        o->anchor = Anchor::TopRight;
        o->rotation = 0.0;
        o->scale = std::min(slot - margin, maxWidth / std::max(aspect, 1e-6));
        o->opacity = 1.0;
        o->depth = Depth::Front;
        o->mode = Mode::Normal;
        o->visible = true;
        o->offset = {-(o->scale * aspect) * 0.5 - margin, y + o->scale * 0.5};
        y += o->scale + margin;
    }
}

void reanchor(Overlay& o, Anchor anchor, glm::dvec2 viewSize)
{
    const double h = std::max(viewSize.y, 1.0);
    const glm::dvec2 centre = anchorPoint(o.anchor, viewSize) + o.offset * h;
    o.anchor = anchor;
    o.offset = (centre - anchorPoint(anchor, viewSize)) / h;
}

void install()
{
    if (std::exchange(state().installed, true))
        return;
    ensureMainWindow(state().set);
    registerCommands();
    io::registerUiSection(
        kSection, []() -> json { return storage() == Storage::Scene && !current().images.empty() ? current().toJson() : json(); },
        [](const json& section) {
            if (storage() != Storage::Scene)
                return;
            state().set = Set::fromJson(section);
            notify();
        });
    events::subscribe(events::Event::SceneNew, [](const events::EventData&) {
        if (storage() != Storage::Scene)
            return;
        state().set = Set{};
        ensureMainWindow(state().set);
        notify();
    });
    Workspace::get().onChanged([](auto&&...) {
        if (storage() == Storage::Project)
            loadFromStorage();
    });
    loadFromStorage();
}
} // namespace lila::ui::references
