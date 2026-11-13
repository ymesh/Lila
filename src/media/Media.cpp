#include "media/Media.h"

#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/ImagePlane.h"
#include "scene/SceneData.h"
#include "scene/Take.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <thread>

namespace lila::media
{
namespace
{
using scene::Node;
using scene::PlugRef;
using scene::PlugType;
using scene::anim::TakeNode;

constexpr uint32_t kStored = scene::AttrInput | scene::AttrStorable;

int mediaAttr(const Node& n)
{
    return isAudio(n) ? int(AudioNode::kMedia) : int(scene::ImagePlane::kMedia);
}

struct SoundEntry
{
    AudioBufferPtr buffer;
    std::shared_ptr<AudioStream> stream;
    fs::file_time_type stamp{};
    bool loading = false;
    std::string error;
};

std::map<std::string, SoundEntry>& sounds()
{
    static std::map<std::string, SoundEntry> m;
    return m;
}

std::map<int, std::function<void()>>& soundListeners()
{
    static std::map<int, std::function<void()>> m;
    return m;
}

fs::file_time_type stampOf(const fs::path& p)
{
    std::error_code ec;
    fs::file_time_type t = fs::last_write_time(p, ec);
    return ec ? fs::file_time_type{} : t;
}

double& streamThreshold()
{
    static double s = 90.0;
    return s;
}

/** Whole, or a stream for a long track when streaming is allowed. */
Sound openSound(const fs::path& path, int track, bool allowStream, std::string* err)
{
    if (allowStream)
    {
        std::vector<AudioTrack> tracks = audioTracks(path, err);
        if (track >= 0 && track < int(tracks.size()) && tracks[size_t(track)].duration > streamThreshold())
        {
            AudioStream::Options o;
            o.rate = kSampleRate;
            o.track = track;
            return {nullptr, AudioStream::open(path, o, err)};
        }
    }
    return {decodeAudio(path, kSampleRate, err, std::max(0, track)), nullptr};
}

void tellSoundListeners()
{
    auto copy = soundListeners();
    for (auto& [id, fn] : copy)
        fn();
}

Sound load(const std::string& stored, int track, bool allowStream, bool wait, std::string* err)
{
    if (stored.empty())
        return {};
    const fs::path path = paths::resolvePath(stored);
    const std::string file = paths::toUtf8(path);
    const std::string key = file + "|" + std::to_string(track) + (allowStream ? "|s" : "");
    SoundEntry& e = sounds()[key];
    const fs::file_time_type stamp = stampOf(path);
    if ((e.buffer || e.stream) && e.stamp == stamp)
        return {e.buffer, e.stream};
    if (!e.error.empty() && e.stamp == stamp)
    {
        if (err)
            *err = e.error;
        return {};
    }
    auto land = [key, file, stamp](const Sound& s, const std::string& why) {
        SoundEntry& d = sounds()[key];
        d.loading = false;
        d.buffer = s.buffer;
        d.stream = s.stream;
        d.stamp = stamp;
        d.error = s ? std::string() : why;
        if (!s)
            log::warn("audio: {}", why);
        else if (s.stream)
            log::info("audio: {} is {:.0f} s long, so it streams from disk instead of loading whole", file, s.stream->duration());
    };
    if (wait)
    {
        std::string why;
        Sound s = openSound(path, track, allowStream, &why);
        land(s, why);
        if (err)
            *err = sounds()[key].error;
        return s;
    }
    if (e.loading)
        return {e.buffer, e.stream};
    e.loading = true;
    std::thread([path, track, allowStream, land] {
        std::string why;
        Sound s = openSound(path, track, allowStream, &why);
        mainq::post([s, why, land] {
            land(s, why);
            tellSoundListeners();
        });
    }).detach();
    return {e.buffer, e.stream};
}

Node* nodeNamed(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    return n;
}

json mediaJson(Graph& g, NodeId id)
{
    Node& n = *g.node(id);
    json takes = json::array();
    for (NodeId t : takesOf(g, id))
        takes.push_back(g.node(t)->path());
    const bool audio = isAudio(n);
    const std::string file = audio ? n.as<std::string>(AudioNode::kFileName) : n.as<std::string>(scene::ImagePlane::kImageName);
    const double startFrame = audio ? n.as<double>(AudioNode::kOffset) : n.as<double>(scene::ImagePlane::kStartFrame);
    json j = {{"name", n.path()}, {"type", n.type().name}, {"file", file}, {"live", isLive(g, id)},
              {"takes", takes}, {"start", startSeconds(g, id, startFrame)}};
    if (audio)
    {
        j["track"] = n.as<int64_t>(AudioNode::kTrack);
        j["sourceStart"] = n.as<double>(AudioNode::kSourceStart);
        j["sourceEnd"] = n.as<double>(AudioNode::kSourceEnd);
        if (std::optional<Voice> v = voiceOf(g, id, false))
        {
            j["end"] = v->end();
            j["streamed"] = v->stream != nullptr;
        }
    }
    return j;
}

json trackJson(const AudioTrack& t)
{
    return {{"index", t.index}, {"codec", t.codec}, {"channels", t.channels}, {"rate", t.rate},
            {"duration", t.duration}, {"language", t.language}, {"title", t.title}};
}

/** The active take, or none. */
NodeId activeTake(Graph& g)
{
    for (NodeId t : scene::anim::takes(g))
        if (g.node(t)->as<bool>(TakeNode::kActive))
            return t;
    return scene::kNoNode;
}
} // namespace

bool isAudio(const Node& n)
{
    return n.type().isA("audio");
}

bool isMedia(const Node& n)
{
    return isAudio(n) || n.type().isA("imagePlane");
}

std::vector<NodeId> takesOf(Graph& g, NodeId media)
{
    std::vector<NodeId> out;
    Node* n = g.node(media);
    if (!n || !isMedia(*n))
        return out;
    for (const PlugRef& d : g.destinations({media, mediaAttr(*n), -1}))
        if (Node* t = g.node(d.node); t && scene::anim::isTake(*t) && d.attr == TakeNode::kMedia)
            out.push_back(d.node);
    std::vector<NodeId> order = scene::anim::takes(g);
    std::ranges::sort(out, [&](NodeId a, NodeId b) { return std::ranges::find(order, a) < std::ranges::find(order, b); });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<NodeId> mediaOf(Graph& g, NodeId take)
{
    std::vector<NodeId> out;
    for (const PlugRef& e : g.elements({take, TakeNode::kMedia, -1}))
        if (auto src = g.source(e))
            out.push_back(src->node);
    return out;
}

void bind(Graph& g, NodeId take, NodeId media)
{
    Node* t = g.node(take);
    Node* m = g.node(media);
    if (!t || !scene::anim::isTake(*t))
        throw std::invalid_argument("bindMedia: not a take");
    if (!m || !isMedia(*m))
        throw std::invalid_argument("bindMedia: " + (m ? m->path() : std::string("?")) + " is not an audio node or image plane");
    if (std::vector<NodeId> has = mediaOf(g, take); std::ranges::find(has, media) != has.end())
        return;
    g.connect({media, mediaAttr(*m), -1}, {take, TakeNode::kMedia, -1});
}

void unbind(Graph& g, NodeId take, NodeId media)
{
    for (const PlugRef& e : g.elements({take, TakeNode::kMedia, -1}))
        if (auto src = g.source(e); src && src->node == media)
            g.removeElement(e);
}

bool isLive(Graph& g, NodeId media)
{
    std::vector<NodeId> takes = takesOf(g, media);
    return takes.empty() || std::ranges::any_of(takes, [&](NodeId t) { return g.node(t)->as<bool>(TakeNode::kActive); });
}

double startSeconds(Graph& g, NodeId media, double startFrame)
{
    double base = 0.0;
    std::vector<NodeId> takes = takesOf(g, media);
    auto active = std::ranges::find_if(takes, [&](NodeId t) { return g.node(t)->as<bool>(TakeNode::kActive); });
    if (active != takes.end())
        base = g.node(*active)->as<double>(TakeNode::kStart);
    else if (!takes.empty())
        base = g.node(takes.front())->as<double>(TakeNode::kStart);
    return base + scene::anim::secondsOf(startFrame);
}

std::vector<NodeId> audibleSounds(Graph& g)
{
    std::vector<NodeId> out;
    const scene::data::json picked = scene::data::get(kSoundKey);
    if (picked.is_string() && !picked.get<std::string>().empty())
    {
        if (Node* n = g.find(picked.get<std::string>()); n && isAudio(*n) && !n->as<bool>(AudioNode::kMute))
            out.push_back(n->id());
        return out;
    }
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); isAudio(*n) && !n->as<bool>(AudioNode::kMute) && isLive(g, id))
            out.push_back(id);
    return out;
}

AudioBufferPtr loadSound(const std::string& stored, bool wait, std::string* err)
{
    return load(stored, 0, false, wait, err).buffer;
}

Sound loadTrack(const std::string& stored, int track, bool wait, std::string* err)
{
    return load(stored, track, true, wait, err);
}

double streamSeconds()
{
    return streamThreshold();
}

void setStreamSeconds(double seconds)
{
    streamThreshold() = seconds;
}

void prefetch(const std::vector<Voice>& voices, double sceneTime, int direction)
{
    for (const Voice& v : voices)
        if (v.stream)
            v.stream->prefetch(std::max(v.from, sceneTime - v.start + v.from), direction);
}

int addSoundListener(std::function<void()> fn)
{
    static int next = 1;
    soundListeners().emplace(next, std::move(fn));
    return next++;
}

void removeSoundListener(int id)
{
    soundListeners().erase(id);
}

std::optional<Voice> voiceOf(Graph& g, NodeId id, bool wait)
{
    const Node* n = g.node(id);
    if (!n || !isAudio(*n))
        return std::nullopt;
    Sound s = loadTrack(n->as<std::string>(AudioNode::kFileName), int(n->as<int64_t>(AudioNode::kTrack)), wait);
    if (!s)
        return std::nullopt;
    Voice v;
    v.buffer = s.buffer;
    v.stream = s.stream;
    v.from = std::max(0.0, scene::anim::secondsOf(n->as<double>(AudioNode::kSourceStart)));
    if (const double end = n->as<double>(AudioNode::kSourceEnd); end > 0.0)
        v.to = std::max(v.from, scene::anim::secondsOf(end));
    v.start = startSeconds(g, id, n->as<double>(AudioNode::kOffset));
    v.gain = n->as<double>(AudioNode::kGain);
    v.node = n->path();
    return v;
}

std::vector<Voice> voices(Graph& g, bool wait)
{
    std::vector<Voice> out;
    for (NodeId id : audibleSounds(g))
        if (std::optional<Voice> v = voiceOf(g, id, wait))
            out.push_back(std::move(*v));
    return out;
}

namespace
{
/** Frames of a streamed sound through the block read last, so a run of samples locks the stream once per block. */
struct StreamCursor
{
    AudioStream& stream;
    int64_t index = -1;
    AudioStream::Block block;

    bool at(int64_t k, float& l, float& r)
    {
        const int64_t bf = stream.blockFrames();
        if (k / bf != index)
        {
            index = k / bf;
            block = stream.block(index);
        }
        const size_t i = size_t(k % bf) * 2;
        if (!block || i + 1 >= block->size())
            return false;
        l = (*block)[i];
        r = (*block)[i + 1];
        return true;
    }
};
} // namespace

void mix(const std::vector<Voice>& voices, double t0, double speed, int rate, float* out, int frames)
{
    std::fill(out, out + size_t(frames) * 2, 0.0f);
    const double step = speed / double(rate);
    for (const Voice& v : voices)
    {
        if (v.gain == 0.0 || !(v.to > v.from) || (!v.buffer && !v.stream) || (v.buffer && v.buffer->channels != 2))
            continue;
        const double perSecond = double(v.rate());
        const double first = v.from * perSecond;
        const double last = std::min(double(v.frames() - 1), v.to * perSecond);
        const double shift = v.from - v.start;
        const float gain = float(v.gain);
        if (v.buffer)
        {
            const float* samples = v.buffer->samples.data();
            for (int i = 0; i < frames; ++i)
            {
                const double s = (t0 + i * step + shift) * perSecond;
                if (s < first || s >= last)
                    continue;
                const int64_t k = int64_t(s);
                const float f = float(s - double(k));
                const float* a = samples + size_t(k) * 2;
                out[i * 2] += gain * (a[0] + (a[2] - a[0]) * f);
                out[i * 2 + 1] += gain * (a[1] + (a[3] - a[1]) * f);
            }
            continue;
        }
        StreamCursor c{*v.stream};
        for (int i = 0; i < frames; ++i)
        {
            const double s = (t0 + i * step + shift) * perSecond;
            if (s < first || s >= last)
                continue;
            const int64_t k = int64_t(s);
            const float f = float(s - double(k));
            float l0, r0, l1, r1;
            if (!c.at(k, l0, r0))
                continue;
            if (!c.at(k + 1, l1, r1))
            {
                l1 = l0;
                r1 = r0;
            }
            out[i * 2] += gain * (l0 + (l1 - l0) * f);
            out[i * 2 + 1] += gain * (r0 + (r1 - r0) * f);
        }
    }
}

int64_t videoFrame(const VideoTiming& timing, double fileFps, double sceneSeconds)
{
    const double fps = timing.fps > 0.0 ? timing.fps : (fileFps > 0.0 ? fileFps : 24.0);
    return int64_t(std::floor((sceneSeconds - timing.start) * fps + 1e-6)) + timing.offset;
}

void registerMediaTypes()
{
    if (scene::types::find("audio"))
        return;
    scene::NodeType t;
    t.name = "audio";
    t.icon = "music";
    t.doc = "A sound file on the timeline: heard during playback and scrubbing, drawn as a waveform in the time slider.";
    t.ownAttrs = {
        {.name = "fileName", .type = PlugType::String, .defaultValue = std::string(), .flags = kStored | scene::AttrFile,
         .doc = "Sound file (WAV, MP3, OGG, FLAC, AAC, M4A, AIFF, OPUS) or the sound of a video. Relative paths resolve from the workspace root."},
        {.name = "track", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = kStored,
         .doc = "Sound track of a file with several (a video's dialogue and music), from 0.", .minValue = 0.0},
        {.name = "offset", .type = PlugType::Float, .defaultValue = 0.0, .flags = kStored,
         .doc = "Frame the sound starts at (where Source Start plays); counted from the take's start while it belongs to takes."},
        {.name = "sourceStart", .type = PlugType::Float, .defaultValue = 0.0, .flags = kStored,
         .doc = "Frame of the file where the sound starts: trims its head.", .minValue = 0.0},
        {.name = "sourceEnd", .type = PlugType::Float, .defaultValue = 0.0, .flags = kStored,
         .doc = "Frame of the file where the sound stops: trims its tail. 0 plays to the end.", .minValue = 0.0},
        {.name = "gain", .type = PlugType::Float, .defaultValue = 1.0, .flags = kStored, .doc = "Volume multiplier.", .minValue = 0.0,
         .maxValue = 4.0},
        {.name = "mute", .type = PlugType::Bool, .defaultValue = false, .flags = kStored, .doc = "Silent, but still drawn."},
        {.name = "media", .type = PlugType::Bool, .defaultValue = false, .flags = scene::AttrOutput | scene::AttrHidden,
         .doc = "Connected to the media of the takes it belongs to."},
    };
    t.create = [] { return std::make_unique<AudioNode>(); };
    scene::types::registerType(std::move(t));
}

void registerMediaCommands()
{
    using T = ArgType;
    if (ops::find("audio"))
        return;

    ops::registerCommand({
        .name = "audio",
        .doc = "Create an audio node from a sound file, or edit the one named. Returns its name.\n\n"
               "Audio plays under the time slider during playback and scrubbing. With take, it belongs to that take: heard only "
               "while the take is active, its offset counted from the take's start.",
        .positionalDoc = "audio node to edit (omit to create one)",
        .flags = {{"fileName", "f", T::String, nullptr, "Sound file; stored relative to the workspace root."},
                  {"track", "tr", T::Int, nullptr, "Sound track of a file with several, from 0.", {}, {}, 0.0},
                  {"offset", "o", T::Float, nullptr, "Frame the sound starts at."},
                  {"sourceStart", "ss", T::Float, nullptr, "Frame of the file the sound starts from.", {}, {}, 0.0},
                  {"sourceEnd", "se", T::Float, nullptr, "Frame of the file the sound stops at; 0 for the end.", {}, {}, 0.0},
                  {"gain", "g", T::Float, nullptr, "Volume multiplier.", {}, {}, 0.0, 4.0},
                  {"mute", "m", T::Bool, nullptr, "Silence it."},
                  {"take", "t", T::String, nullptr, "Take it belongs to."},
                  {"name", "n", T::String, nullptr, "Name; made unique."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            Graph& g = Graph::current();
            Node* n = nullptr;
            if (!a.positional.empty())
            {
                n = nodeNamed(g, a.positional[0].get<std::string>(), "audio");
                if (!isAudio(*n))
                    throw CommandError("audio: " + n->path() + " is not an audio node");
            }
            else
            {
                std::string name = a.get<std::string>("name");
                if (name.empty() && a.has("fileName"))
                {
                    name = paths::toUtf8(paths::fromUtf8(a.get<std::string>("fileName")).stem());
                    if (!Graph::validName(name))
                        name.clear();
                }
                n = g.createNode(*scene::types::find("audio"), name.empty() ? "audio" : name);
            }
            const NodeId id = n->id();
            if (a.has("fileName"))
            {
                const std::string file = a.get<std::string>("fileName");
                g.setValue({id, AudioNode::kFileName}, paths::storedPath(paths::fromUtf8(file)));
            }
            if (a.has("track"))
                g.setValue({id, AudioNode::kTrack}, int64_t(std::max(0, a.get<int>("track"))));
            if (a.has("offset"))
                g.setValue({id, AudioNode::kOffset}, a.get<double>("offset"));
            if (a.has("sourceStart"))
                g.setValue({id, AudioNode::kSourceStart}, std::max(0.0, a.get<double>("sourceStart")));
            if (a.has("sourceEnd"))
                g.setValue({id, AudioNode::kSourceEnd}, std::max(0.0, a.get<double>("sourceEnd")));
            if (a.has("gain"))
                g.setValue({id, AudioNode::kGain}, std::clamp(a.get<double>("gain"), 0.0, 4.0));
            if (a.has("mute"))
                g.setValue({id, AudioNode::kMute}, a.get<bool>("mute"));
            if (a.has("take"))
            {
                Node* t = nodeNamed(g, a.get<std::string>("take"), "audio");
                try
                {
                    bind(g, t->id(), id);
                }
                catch (const std::invalid_argument& e)
                {
                    throw CommandError(std::string("audio: ") + e.what());
                }
            }
            return g.node(id)->path();
        },
        .examples = {"audio(fileName='sound/dialogue.wav', offset=12)"},
    });

    ops::registerCommand({
        .name = "bindMedia",
        .doc = "Give a take its own audio and reference video: the media plays only while the take is active and starts "
               "from the take's start. remove takes them off the take again.",
        .positionalDoc = "take, then audio nodes or image planes",
        .flags = {{"remove", "rm", T::Bool, false, "Unbind instead."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.size() < 2)
                throw CommandError("bindMedia: give a take and one or more media nodes");
            Node* t = nodeNamed(g, a.positional[0].get<std::string>(), "bindMedia");
            if (!scene::anim::isTake(*t))
                throw CommandError("bindMedia: " + t->path() + " is not a take");
            for (size_t i = 1; i < a.positional.size(); ++i)
            {
                Node* m = nodeNamed(g, a.positional[i].get<std::string>(), "bindMedia");
                try
                {
                    if (a.get<bool>("remove"))
                        unbind(g, t->id(), m->id());
                    else
                        bind(g, t->id(), m->id());
                }
                catch (const std::invalid_argument& e)
                {
                    throw CommandError(e.what());
                }
            }
            json out = json::array();
            for (NodeId m : mediaOf(g, t->id()))
                out.push_back(g.node(m)->path());
            return out;
        },
        .examples = {"bindMedia('Walk', 'walkVO', 'walkRef')"},
    });

    ops::registerCommand({
        .name = "timeSliderSound",
        .doc = "The sound the time slider plays and draws (Maya's Time Slider > Audio). Without sound, returns it; an empty "
               "sound plays every audio node of the active takes.",
        .flags = {{"sound", "s", T::String, nullptr, "Audio node to play alone, or empty for all."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.has("sound"))
            {
                const std::string s = a.get<std::string>("sound");
                if (s.empty())
                    scene::data::set(kSoundKey, nullptr);
                else
                {
                    Node* n = nodeNamed(g, s, "timeSliderSound");
                    if (!isAudio(*n))
                        throw CommandError("timeSliderSound: " + n->path() + " is not an audio node");
                    scene::data::set(kSoundKey, n->path());
                }
                scene::anim::playbackChanged();
            }
            json v = scene::data::get(kSoundKey);
            return v.is_string() ? v : json(std::string());
        },
    });

    ops::registerCommand({
        .name = "listMedia",
        .doc = "Every audio node and image plane with its file, whether it plays now (live), the takes it belongs to and "
               "the scene time its start falls on (seconds).",
        .undoable = false,
        .run = [](const Args&) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            for (NodeId id : g.nodes())
                if (isMedia(*g.node(id)))
                    out.push_back(mediaJson(g, id));
            return out;
        },
    });

    ops::registerCommand({
        .name = "mediaInfo",
        .doc = "What a video or sound file holds: its picture (size, rate, frames, codec, the decoder that reads it) and its "
               "sound tracks, the choices for an audio node's track.",
        .positionalDoc = "file",
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
                throw CommandError("mediaInfo: give a file");
            const fs::path path = paths::resolvePath(a.positional[0].get<std::string>());
            if (!fs::exists(path))
                throw CommandError("mediaInfo: no file '" + paths::toUtf8(path) + "'");
            json out = {{"file", paths::toUtf8(path)}, {"video", nullptr}, {"audioTracks", json::array()}};
            std::string err;
            if (isVideoFile(paths::toUtf8(path)))
            {
                if (std::unique_ptr<VideoDecoder> d = VideoDecoder::open(path, 0, &err))
                {
                    const VideoInfo& i = d->info();
                    out["video"] = {{"width", i.width}, {"height", i.height}, {"fps", i.fps}, {"frames", i.frameCount},
                                    {"duration", i.duration}, {"codec", i.codec}, {"decoder", i.decoder}};
                }
                else
                    out["videoError"] = err;
            }
            for (const AudioTrack& t : audioTracks(path, &err))
                out["audioTracks"].push_back(trackJson(t));
            return out;
        },
        .examples = {"mediaInfo('ref/shot010.mov')"},
    });

    ops::registerCommand({
        .name = "slideMedia",
        .doc = "Move audio nodes and image planes along the timeline by a number of frames: an audio node's offset, an "
               "image plane's start frame. Trims stay with the sound.",
        .positionalDoc = "audio nodes or image planes",
        .flags = {{"frames", "f", T::Float, 0.0, "Frames to move by; negative moves earlier."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.empty())
                throw CommandError("slideMedia: give audio nodes or image planes");
            const double by = a.get<double>("frames");
            json out = json::array();
            for (const json& p : a.positional)
            {
                Node* n = nodeNamed(g, p.get<std::string>(), "slideMedia");
                if (!isMedia(*n))
                    throw CommandError("slideMedia: " + n->path() + " is not an audio node or image plane");
                const int attr = isAudio(*n) ? int(AudioNode::kOffset) : int(scene::ImagePlane::kStartFrame);
                g.setValue({n->id(), attr}, n->as<double>(attr) + by);
                out.push_back(n->path());
            }
            return out;
        },
        .examples = {"slideMedia('shot010', 'shot010Ref', frames=-6)"},
    });

    ops::registerCommand({
        .name = "importFootage",
        .doc = "Bring in reference footage for the shot in one step: a video becomes an image plane on the camera, and its "
               "sound (or a sound file given alone) an audio node starting on the same frame. Both belong to the active "
               "take, so they play only with it and line up from its start. Returns the nodes made.",
        .positionalDoc = "video or sound file",
        .flags = {nodeFlag({"camera", "c", T::String, "persp", "View or camera the video is seen through."}, "camera"),
                  {"take", "t", T::String, nullptr, "Take the media belongs to; default the active one, empty for none."},
                  {"frame", "fr", T::Float, nullptr,
                   "Frame the footage starts on: default the take's first frame, or the playback start without a take."},
                  {"sound", "s", T::Bool, true, "Also make an audio node from a video's sound."},
                  {"track", "tr", T::Int, 0, "Sound track of a video with several, from 0.", {}, {}, 0.0},
                  {"name", "n", T::String, nullptr, "Base name; default the file's."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            Graph& g = Graph::current();
            if (a.positional.empty())
                throw CommandError("importFootage: give a video or sound file");
            const std::string file = a.positional[0].get<std::string>();
            const fs::path path = paths::resolvePath(file);
            if (!fs::exists(path))
                throw CommandError("importFootage: no file '" + file + "'");
            const bool video = isVideoFile(paths::toUtf8(path));
            if (!video && !isAudioFile(paths::toUtf8(path)))
                throw CommandError("importFootage: '" + file + "' is not a video or sound file");
            NodeId take = activeTake(g);
            if (a.has("take"))
            {
                const std::string t = a.get<std::string>("take");
                take = t.empty() ? scene::kNoNode : nodeNamed(g, t, "importFootage")->id();
                if (take != scene::kNoNode && !scene::anim::isTake(*g.node(take)))
                    throw CommandError("importFootage: " + t + " is not a take");
            }
            // Bound media counts from its take's start, unbound from time 0.
            const double frame = a.has("frame") ? a.get<double>("frame") : take != scene::kNoNode ? 0.0 : std::round(scene::anim::frameOf(scene::anim::playback().minTime));
            std::string name = a.get<std::string>("name");
            if (name.empty())
                name = paths::toUtf8(path.stem());
            if (!Graph::validName(name))
                name = video ? "reference" : "audio";
            json out = json::object();
            std::vector<NodeId> made;
            if (video)
            {
                const std::string plane = ops::execute("imagePlane", {{"fileName", file}, {"camera", a.get<std::string>("camera")},
                                                                      {"startFrame", frame}, {"name", name + "Ref"}, {"skipSelect", true}})
                                              .get<std::string>();
                out["imagePlane"] = plane;
                made.push_back(g.find(plane)->id());
            }
            const bool hasSound = !audioTracks(path).empty();
            if (hasSound && (!video || a.get<bool>("sound")))
            {
                const int track = std::max(0, a.get<int>("track"));
                const std::string sound = ops::execute("audio", {{"fileName", file}, {"offset", frame}, {"track", track}, {"name", name}})
                                              .get<std::string>();
                out["audio"] = sound;
                made.push_back(g.find(sound)->id());
            }
            else if (!video)
                throw CommandError("importFootage: '" + file + "' has no sound");
            if (take != scene::kNoNode)
            {
                for (NodeId m : made)
                    bind(g, take, m);
                out["take"] = g.node(take)->path();
            }
            return out;
        },
        .examples = {"importFootage('ref/shot010.mov')", "importFootage('ref/shot010.mov', camera='shotCam', frame=1001)"},
    });
}
} // namespace lila::media
