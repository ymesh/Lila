#pragma once

#include "core/Export.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

/**
 * Video and audio decoding through FFmpeg (LGPL, linked dynamically, built without H.264 and HEVC decoders; on Windows
 * those go through Media Foundation): frame-accurate video frames for image planes and reference images, and whole
 * sound files resampled for the timeline.
 */
namespace lila::media
{
namespace fs = std::filesystem;

/** By extension: .mp4 .mov .mkv .webm .avi .m4v .mpg .mpeg .mxf .wmv .flv .gif. */
LILA_SCENE_API bool isVideoFile(const std::string& path);
/** By extension: .wav .mp3 .ogg .flac .aac .m4a .aif .aiff .opus .wma. */
LILA_SCENE_API bool isAudioFile(const std::string& path);

struct VideoInfo
{
    int width = 0;
    int height = 0;
    /** The stream's frame rate; frame n shows from n / fps seconds after its first frame. */
    double fps = 0.0;
    int64_t frameCount = 0;
    double duration = 0.0; // seconds
    std::string codec;
    /** "ffmpeg", or "mediaFoundation" for H.264 and HEVC on Windows. */
    std::string decoder;
};

/** One decoded picture: RGBA8, row 0 at the top. */
struct VideoFrame
{
    int64_t index = -1;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};
using VideoFramePtr = std::shared_ptr<const VideoFrame>;

/**
 * One video file's frames by index, decoded on the calling thread. Stepping to the next frame decodes on; any other
 * index seeks to the keyframe before it and decodes forward, so every index returns the frame that shows at that time.
 */
class LILA_SCENE_API VideoDecoder
{
public:
    /**
     * Null with err set when the file can't be read or holds no video. maxSize > 0 scales frames down so their longer
     * side is at most maxSize.
     */
    static std::unique_ptr<VideoDecoder> open(const fs::path& path, int maxSize = 0, std::string* err = nullptr);
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    const VideoInfo& info() const;
    /** Frame index, clamped to the stream; null with err set when decoding fails. */
    VideoFramePtr frame(int64_t index, std::string* err = nullptr);
    /** How many seeks frame() has made, for tests of the sequential path. */
    int64_t seeks() const;

    struct Impl;

private:
    explicit VideoDecoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> mImpl;
};

/**
 * A video decoded on a thread of its own into a frame cache, for drawing that must never wait: get returns at once
 * with the frame if it is cached, otherwise with the nearest cached one while the thread decodes the asked frame. The
 * newest request always wins, so scrubbing never queues up stale frames; while idle the thread reads ahead in the
 * direction of the requests.
 */
class LILA_SCENE_API VideoStream
{
public:
    struct Options
    {
        int maxSize = 0;
        /** Cache budget; the frames furthest from the last request go first. */
        size_t cacheBytes = size_t(384) << 20;
        /** Frames read ahead past each request while idle. */
        int readAhead = 12;
    };

    /** Opens on the calling thread; null with err set when the file can't be decoded. */
    static std::shared_ptr<VideoStream> open(const fs::path& path, const Options& options, std::string* err = nullptr);
    ~VideoStream();
    VideoStream(const VideoStream&) = delete;
    VideoStream& operator=(const VideoStream&) = delete;

    const VideoInfo& info() const { return mInfo; }
    /** Any thread. exact is set to whether the frame returned is the one asked for. */
    VideoFramePtr get(int64_t index, bool* exact = nullptr);
    /** Blocks until index is decoded or timeoutSeconds pass; for tests and offscreen captures. */
    VideoFramePtr wait(int64_t index, double timeoutSeconds);
    /** Called on the decode thread whenever a requested frame lands. */
    void setOnFrame(std::function<void()> fn);
    size_t cachedBytes() const;

private:
    VideoStream() = default;
    void run();
    void store(VideoFramePtr f);
    VideoFramePtr nearest(int64_t index) const;

    VideoInfo mInfo;
    Options mOptions;
    std::unique_ptr<VideoDecoder> mDecoder;
    mutable std::mutex mMutex;
    std::condition_variable mWake;
    std::condition_variable mLanded;
    std::map<int64_t, VideoFramePtr> mCache;
    size_t mBytes = 0;
    int64_t mWanted = -1;
    int64_t mLastWanted = -1;
    int mDirection = 1;
    bool mQuit = false;
    std::function<void()> mOnFrame;
    std::thread mThread;
};

/** Interleaved float samples at one rate. */
struct AudioBuffer
{
    int rate = 48000;
    int channels = 2;
    std::vector<float> samples;

    int64_t frames() const { return channels > 0 ? int64_t(samples.size()) / channels : 0; }
    double duration() const { return rate > 0 ? double(frames()) / rate : 0.0; }
};
using AudioBufferPtr = std::shared_ptr<const AudioBuffer>;

/** One audio stream of a file, as a choice for audio nodes reading video files with several sound tracks. */
struct AudioTrack
{
    /** 0-based among the file's audio tracks; what decodeAudio's and AudioStream's track take. */
    int index = 0;
    std::string codec;
    int channels = 0;
    int rate = 0;
    double duration = 0.0; // seconds
    /** From the container's tags, empty when it has none. */
    std::string language;
    std::string title;
};
/** The file's audio tracks in file order; empty with err set when it can't be read. A file without sound has none. */
LILA_SCENE_API std::vector<AudioTrack> audioTracks(const fs::path& path, std::string* err = nullptr);

/**
 * A whole sound file (one audio track of it, 0-based; the first by default) decoded and resampled to stereo float at
 * rate; null with err set on failure.
 */
LILA_SCENE_API AudioBufferPtr decodeAudio(const fs::path& path, int rate = 48000, std::string* err = nullptr, int track = 0);

/** Waveform envelope: the lowest and highest sample (both channels) per bucket of `secondsPerBucket`. */
struct Peaks
{
    double secondsPerBucket = 0.0;
    std::vector<float> low;
    std::vector<float> high;
};
LILA_SCENE_API Peaks peaks(const AudioBuffer& buffer, double secondsPerBucket);

/**
 * A long sound decoded in blocks on a thread of its own instead of whole: music beds and long takes keep a window of
 * blocks around where they are heard (stereo float at rate). block() never waits, so the audio callback can read it:
 * a block not decoded yet is silence until it lands, and asking for it moves the read-ahead there. The read-ahead runs
 * in the play or scrub direction (backwards too), and a shorter window is kept on the other side of the playhead, so
 * turning around or scrubbing back finds its blocks decoded. Stepping to the next block decodes on; any other block
 * seeks. A second thread scans the whole file once for the waveform and exact length.
 */
class LILA_SCENE_API AudioStream
{
public:
    struct Options
    {
        int rate = 48000;
        int track = 0;
        int blockFrames = 1 << 15;
        /** Decoded blocks kept; the ones furthest from the last request go first. */
        size_t cacheBytes = size_t(48) << 20;
        double readAheadSeconds = 6.0;
        /** Decoded and kept behind the playhead (against the direction), after the read-ahead is in. */
        double readBehindSeconds = 3.0;
        double peakSeconds = 1.0 / 400.0;
    };
    using Block = std::shared_ptr<const std::vector<float>>;

    /** Probes on the calling thread; null with err set when the file has no such audio track. */
    static std::shared_ptr<AudioStream> open(const fs::path& path, const Options& options, std::string* err = nullptr);
    ~AudioStream();
    AudioStream(const AudioStream&) = delete;
    AudioStream& operator=(const AudioStream&) = delete;

    int rate() const;
    int blockFrames() const;
    /** Length in output frames: the container's estimate, exact once the scan is done. */
    int64_t frames() const;
    double duration() const { return rate() > 0 ? double(frames()) / rate() : 0.0; }
    /** Any thread, never blocks: block index (frames index x blockFrames on), null while it decodes. */
    Block block(int64_t index);
    /**
     * Starts decoding at seconds into the file, ahead of playing or scrubbing there. direction is +1 forwards, -1
     * backwards, 0 to take it from the move since the previous prefetch (scrubbing); it holds until the next prefetch.
     */
    void prefetch(double seconds, int direction = 0);
    /** +1 or -1: the way the read-ahead runs. */
    int direction() const;
    /** Whether block index has decoded, without asking for it or moving the read-ahead; for tests. */
    bool ready(int64_t index) const;
    /** Blocks until the block holding seconds has decoded or timeoutSeconds pass; for tests. */
    bool waitFor(double seconds, double timeoutSeconds);
    /** The waveform scanned so far; complete once scanned(). */
    std::shared_ptr<const Peaks> peaks() const;
    bool scanned() const;
    /** Waits for the scan; for tests. */
    bool waitScanned(double timeoutSeconds) const;
    size_t cachedBytes() const;
    int64_t seeks() const;

    struct Impl;

private:
    AudioStream();
    std::unique_ptr<Impl> mImpl;
};
} // namespace lila::media
