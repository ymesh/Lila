#pragma once

#include "core/Export.h"

#include "media/Decode.h"
#include "scene/Node.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Sound and video on the timeline, Maya style: audio nodes play under the time slider, image planes play video, and
 * both can belong to takes so each clip brings its own reference sound and footage.
 *
 * A media node bound to takes (connected to a take's `media` array) is live only while one of them is active, and its
 * start frame counts from that take's start; an unbound one is always live and counts from time 0.
 */
namespace lila::media
{
using scene::Graph;
using scene::NodeId;

/**
 * A sound file placed on the timeline: the part of the file from sourceStart to sourceEnd plays from offset on. Video
 * files work too, with track picking among several sound tracks.
 */
class AudioNode : public scene::Node
{
public:
    enum Attr
    {
        kFileName,
        kTrack,
        kOffset,
        kSourceStart,
        kSourceEnd,
        kGain,
        kMute,
        kMedia,
        kCount
    };
};

LILA_SCENE_API bool isAudio(const scene::Node& n);
/** Audio nodes and image planes: the nodes a take can carry. */
LILA_SCENE_API bool isMedia(const scene::Node& n);

/** Takes the media node is bound to, in take order. */
LILA_SCENE_API std::vector<NodeId> takesOf(Graph& g, NodeId media);
/** Media nodes bound to the take. */
LILA_SCENE_API std::vector<NodeId> mediaOf(Graph& g, NodeId take);
/** Undoable. Throws std::invalid_argument for a node that isn't media or a take. */
LILA_SCENE_API void bind(Graph& g, NodeId take, NodeId media);
LILA_SCENE_API void unbind(Graph& g, NodeId take, NodeId media);
/** Unbound media always; bound media while one of its takes is active. */
LILA_SCENE_API bool isLive(Graph& g, NodeId media);
/** Scene seconds where the media's own time 0 falls: startFrame frames after its active take's start (or time 0). */
LILA_SCENE_API double startSeconds(Graph& g, NodeId media, double startFrame);

/** Scene data key of the time slider's sound (an audio node's path); absent plays every live audio node. */
inline constexpr const char* kSoundKey = "timeSlider.sound";
/** The audio nodes heard now: the time slider's sound if it names one, else every live, unmuted audio node. */
LILA_SCENE_API std::vector<NodeId> audibleSounds(Graph& g);

/**
 * A sound placed on the timeline: file time `from` plays at `start` scene seconds, and it stops at file time `to`. The
 * sound is the buffer (decoded whole) or, for long files, the stream.
 */
struct Voice
{
    AudioBufferPtr buffer;
    double start = 0.0;
    double gain = 1.0;
    std::string node;
    std::shared_ptr<AudioStream> stream;
    double from = 0.0;
    double to = std::numeric_limits<double>::infinity();

    int64_t frames() const { return buffer ? buffer->frames() : stream ? stream->frames() : 0; }
    int rate() const { return buffer ? buffer->rate : stream ? stream->rate() : 0; }
    /** Scene seconds of the sound's last heard moment: where `to` or the file's end falls. */
    double end() const { return start + std::min(to, rate() > 0 ? double(frames()) / rate() : 0.0) - from; }
};

/** A loaded sound file: decoded whole, or streamed when it is longer than streamSeconds(). */
struct Sound
{
    AudioBufferPtr buffer;
    std::shared_ptr<AudioStream> stream;
    explicit operator bool() const { return buffer || stream; }
};

/** Sounds longer than this stream instead of decoding whole; 90 s unless set (tests set it lower). */
LILA_SCENE_API double streamSeconds();
LILA_SCENE_API void setStreamSeconds(double seconds);
/**
 * The sound of an audio node's file and track, by stored path, as loadSound does: without wait it loads on a worker and
 * is empty until done. Main thread.
 */
LILA_SCENE_API Sound loadTrack(const std::string& stored, int track, bool wait = true, std::string* err = nullptr);
/**
 * Starts decoding the streamed voices where they will be heard from sceneTime on, reading ahead in direction (+1, -1,
 * or 0 to follow the move since the previous call, as scrubbing does).
 */
LILA_SCENE_API void prefetch(const std::vector<Voice>& voices, double sceneTime, int direction = 0);

/**
 * A sound file decoded at the output rate, by stored path (resolved from the workspace). Without wait it decodes on a
 * worker and returns null until done, then tells the sound listeners on the main thread. Reloads a changed file. Main
 * thread.
 */
LILA_SCENE_API AudioBufferPtr loadSound(const std::string& stored, bool wait = true, std::string* err = nullptr);
LILA_SCENE_API int addSoundListener(std::function<void()> fn);
LILA_SCENE_API void removeSoundListener(int id);
/** The output sample rate sounds are decoded at. */
inline constexpr int kSampleRate = 48000;
/** The voices of audibleSounds whose files have loaded (wait: load them first). */
LILA_SCENE_API std::vector<Voice> voices(Graph& g, bool wait = false);
/** The audio node's voice, heard or not; nullopt until its file has loaded (wait: load it first). */
LILA_SCENE_API std::optional<Voice> voiceOf(Graph& g, NodeId audio, bool wait = false);

/**
 * Mixes voices into out (interleaved stereo at rate) from scene time t0, one output frame advancing speed / rate
 * seconds of scene time. out is overwritten.
 */
LILA_SCENE_API void mix(const std::vector<Voice>& voices, double t0, double speed, int rate, float* out, int frames);

/** Which frame of a video an image plane shows at a scene time: start and fps override as on the plane. */
struct VideoTiming
{
    double start = 0.0;    // scene seconds of the video's first frame
    double fps = 0.0;      // the plane's override; 0 uses the file's
    int64_t offset = 0;    // frames added
};
LILA_SCENE_API int64_t videoFrame(const VideoTiming& timing, double fileFps, double sceneSeconds);

LILA_SCENE_API void registerMediaTypes();
LILA_SCENE_API void registerMediaCommands();
} // namespace lila::media
