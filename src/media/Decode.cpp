#include "media/Decode.h"
#include "media/DecodeBackend.h"

#include "core/Paths.h"
#include "core/Profile.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <map>

namespace lila::media
{
namespace
{
std::string extensionOf(const std::string& path)
{
    std::string ext = paths::toUtf8(paths::fromUtf8(path).extension());
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext;
}

std::string avError(int code)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buf{};
    av_strerror(code, buf.data(), buf.size());
    return buf.data();
}

bool fail(std::string* err, std::string message)
{
    if (err)
        *err = std::move(message);
    return false;
}

struct Packet
{
    AVPacket* p = av_packet_alloc();
    ~Packet() { av_packet_free(&p); }
};

struct Frame
{
    AVFrame* f = av_frame_alloc();
    ~Frame() { av_frame_free(&f); }
};

/** An opened file with one stream's decoder; shared by the video and audio readers. */
struct Input
{
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    int stream = -1;
    bool flushed = false; // the decoder has been sent its end of stream

    ~Input()
    {
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
    }

    /**
     * Opens the container and finds a stream of type without a decoder: the best one, or with track >= 0 the track-th
     * stream of that type in file order.
     */
    bool probe(const fs::path& path, AVMediaType type, std::string* err, int track = -1)
    {
        const std::string utf8 = paths::toUtf8(path);
        if (int r = avformat_open_input(&fmt, utf8.c_str(), nullptr, nullptr); r < 0)
            return fail(err, utf8 + ": " + avError(r));
        if (int r = avformat_find_stream_info(fmt, nullptr); r < 0)
            return fail(err, utf8 + ": " + avError(r));
        if (track >= 0)
        {
            int seen = 0;
            stream = -1;
            for (unsigned i = 0; i < fmt->nb_streams && stream < 0; ++i)
                if (fmt->streams[i]->codecpar->codec_type == type && seen++ == track)
                    stream = int(i);
            if (stream < 0 && seen > 0)
                return fail(err, utf8 + ": no " + (type == AVMEDIA_TYPE_VIDEO ? "video" : "audio") + " track " + std::to_string(track + 1) +
                                     " (it has " + std::to_string(seen) + ")");
        }
        else
            stream = av_find_best_stream(fmt, type, -1, -1, nullptr, 0);
        if (stream < 0)
            return fail(err, utf8 + (type == AVMEDIA_TYPE_VIDEO ? ": no video stream" : ": no audio stream"));
        return true;
    }

    bool openDecoder(const fs::path& path, std::string* err)
    {
        const AVCodecID id = fmt->streams[stream]->codecpar->codec_id;
        const AVCodec* codec = avcodec_find_decoder(id);
        if (!codec && (id == AV_CODEC_ID_H264 || id == AV_CODEC_ID_HEVC))
            return fail(err, paths::toUtf8(path) + ": " + (id == AV_CODEC_ID_H264 ? "H.264" : "HEVC") +
                                 " video is not supported on this platform yet; convert it to VP9, AV1, ProRes or MPEG-4");
        if (!codec)
            return fail(err, paths::toUtf8(path) + ": unsupported codec (" + avcodec_get_name(id) + ")");
        dec = avcodec_alloc_context3(codec);
        if (!dec || avcodec_parameters_to_context(dec, fmt->streams[stream]->codecpar) < 0)
            return fail(err, paths::toUtf8(path) + ": unsupported codec");
        dec->thread_count = 0;
        if (int r = avcodec_open2(dec, codec, nullptr); r < 0)
            return fail(err, paths::toUtf8(path) + ": " + avError(r));
        return true;
    }

    bool open(const fs::path& path, AVMediaType type, std::string* err, int track = -1)
    {
        return probe(path, type, err, track) && openDecoder(path, err);
    }

    AVStream* st() const { return fmt->streams[stream]; }

    /** The next decoded frame: 0, AVERROR_EOF at the end, or another error. */
    int receive(AVFrame* out, AVPacket* pkt)
    {
        for (;;)
        {
            int r = avcodec_receive_frame(dec, out);
            if (r != AVERROR(EAGAIN))
                return r;
            if (flushed)
                return AVERROR_EOF;
            r = av_read_frame(fmt, pkt);
            if (r < 0)
            {
                avcodec_send_packet(dec, nullptr);
                flushed = true;
                continue;
            }
            if (pkt->stream_index == stream)
                r = avcodec_send_packet(dec, pkt);
            av_packet_unref(pkt);
            if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_INVALIDDATA)
                return r;
        }
    }
};
} // namespace

bool isVideoFile(const std::string& path)
{
    static const char* const kExt[] = {".mp4", ".mov", ".mkv", ".webm", ".avi", ".m4v", ".mpg", ".mpeg", ".mxf", ".wmv", ".flv", ".gif"};
    const std::string ext = extensionOf(path);
    return std::ranges::any_of(kExt, [&](const char* e) { return ext == e; });
}

bool isAudioFile(const std::string& path)
{
    static const char* const kExt[] = {".wav", ".mp3", ".ogg", ".flac", ".aac", ".m4a", ".aif", ".aiff", ".opus", ".wma"};
    const std::string ext = extensionOf(path);
    return std::ranges::any_of(kExt, [&](const char* e) { return ext == e; });
}

// ---------------------------------------------------------------- VideoDecoder

namespace
{
struct FfmpegVideo final : VideoDecoder::Impl
{
    Input in;
    Packet pkt;
    Frame decoded;
    /** The best frame at or before the last target, and a frame decoded past it that the next request starts from. */
    Frame held;
    Frame pending;
    bool hasPending = false;
    SwsContext* sws = nullptr;
    AVRational tb{1, 1};
    int64_t startPts = 0;
    /** The last decoded frame's index; a frame without a timestamp (a stream's delayed last picture) follows it. */
    int64_t lastAt = -1;

    ~FfmpegVideo() override { sws_freeContext(sws); }

    int64_t indexOf(const AVFrame* f)
    {
        int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        lastAt = pts == AV_NOPTS_VALUE ? lastAt + 1 : std::llround(double(pts - startPts) * av_q2d(tb) * info.fps);
        return lastAt;
    }

    void seek(int64_t index, double backSeconds) override
    {
        const double seconds = std::max(0.0, double(index) / info.fps - backSeconds);
        const int64_t ts = startPts + int64_t(seconds / av_q2d(tb));
        av_seek_frame(in.fmt, in.stream, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(in.dec);
        in.flushed = false;
        hasPending = false;
        av_frame_unref(pending.f);
        av_frame_unref(held.f);
        lastAt = -1;
        ++seeks;
    }

    int64_t heldAt() const override { return held.f->buf[0] ? held.f->pts : -1; }

    bool holdPending(int64_t index) override
    {
        if (!hasPending)
            return false;
        av_frame_unref(held.f);
        av_frame_move_ref(held.f, pending.f);
        hasPending = false;
        held.f->pts = index;
        return true;
    }

    VideoFramePtr convertHeld(int64_t index) override
    {
        const AVFrame* f = held.f;
        sws = sws_getCachedContext(sws, f->width, f->height, AVPixelFormat(f->format), outW, outH, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                                   nullptr, nullptr);
        if (!sws)
            return nullptr;
        const int* coeffs = sws_getCoefficients(f->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_DEFAULT);
        sws_setColorspaceDetails(sws, coeffs, f->color_range == AVCOL_RANGE_JPEG ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
        auto out = std::make_shared<VideoFrame>();
        out->index = index;
        out->width = outW;
        out->height = outH;
        out->rgba.resize(size_t(outW) * size_t(outH) * 4);
        uint8_t* dst[4] = {out->rgba.data(), nullptr, nullptr, nullptr};
        int stride[4] = {outW * 4, 0, 0, 0};
        sws_scale(sws, f->data, f->linesize, 0, f->height, dst, stride);
        return out;
    }

    bool decodeTo(int64_t index, std::string* err) override
    {
        bool have = held.f->buf[0] != nullptr;
        for (;;)
        {
            AVFrame* f = decoded.f;
            if (hasPending)
            {
                av_frame_unref(f);
                av_frame_move_ref(f, pending.f);
                hasPending = false;
            }
            else if (int r = in.receive(f, pkt.p); r == AVERROR_EOF)
                return have;
            else if (r < 0)
                return fail(err, avError(r));
            const int64_t at = indexOf(f);
            if (at > index)
            {
                av_frame_move_ref(pending.f, f);
                hasPending = true;
                return have;
            }
            av_frame_unref(held.f);
            av_frame_move_ref(held.f, f);
            held.f->pts = at;
            have = true;
            if (at == index)
                return true;
        }
    }
};

/** Rate, length and size of the container's video stream, as FFmpeg reads them. */
VideoInfo probedInfo(const Input& in)
{
    const AVStream* st = in.st();
    VideoInfo info;
    info.width = st->codecpar->width;
    info.height = st->codecpar->height;
    info.codec = avcodec_get_name(st->codecpar->codec_id);
    double fps = av_q2d(st->avg_frame_rate);
    if (!(fps > 0.0) || !std::isfinite(fps))
        fps = av_q2d(st->r_frame_rate);
    info.fps = fps > 0.0 && std::isfinite(fps) ? fps : 25.0;
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
        info.duration = double(st->duration) * av_q2d(st->time_base);
    else if (in.fmt->duration != AV_NOPTS_VALUE)
        info.duration = double(in.fmt->duration) / AV_TIME_BASE;
    info.frameCount = st->nb_frames > 0 ? st->nb_frames : std::max<int64_t>(1, std::llround(info.duration * info.fps));
    return info;
}
} // namespace

VideoDecoder::VideoDecoder(std::unique_ptr<Impl> impl) : mImpl(std::move(impl)) {}
VideoDecoder::~VideoDecoder() = default;

std::unique_ptr<VideoDecoder> VideoDecoder::open(const fs::path& path, int maxSize, std::string* err)
{
    LILA_ZONE();

    auto impl = std::make_unique<FfmpegVideo>();
    if (!impl->in.probe(path, AVMEDIA_TYPE_VIDEO, err))
        return nullptr;
    VideoInfo probed = probedInfo(impl->in);
    if (probed.width <= 0 || probed.height <= 0)
    {
        fail(err, paths::toUtf8(path) + ": no picture size");
        return nullptr;
    }
#ifdef _WIN32
    if (const AVCodecID id = impl->in.st()->codecpar->codec_id; id == AV_CODEC_ID_H264 || id == AV_CODEC_ID_HEVC)
    {
        std::unique_ptr<Impl> mf = openMediaFoundation(path, probed, maxSize, err);
        return mf ? std::unique_ptr<VideoDecoder>(new VideoDecoder(std::move(mf))) : nullptr;
    }
#endif
    if (!impl->in.openDecoder(path, err))
        return nullptr;
    impl->info = probed;
    impl->info.decoder = "ffmpeg";
    impl->tb = impl->in.st()->time_base;
    impl->startPts = impl->in.st()->start_time != AV_NOPTS_VALUE ? impl->in.st()->start_time : 0;
    fitOutput(*impl, maxSize);
    // Frame 0 is the first picture: containers without presentation times (AVI with B-frames) stamp it a frame late.
    if (impl->in.receive(impl->pending.f, impl->pkt.p) == 0)
    {
        const AVFrame* f = impl->pending.f;
        if (const int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts; pts != AV_NOPTS_VALUE)
            impl->startPts = pts;
        impl->hasPending = true;
    }
    return std::unique_ptr<VideoDecoder>(new VideoDecoder(std::move(impl)));
}

const VideoInfo& VideoDecoder::info() const
{
    return mImpl->info;
}

int64_t VideoDecoder::seeks() const
{
    return mImpl->seeks;
}

VideoFramePtr VideoDecoder::frame(int64_t index, std::string* err)
{
    LILA_ZONE();

    Impl& d = *mImpl;
    index = std::clamp<int64_t>(index, 0, std::max<int64_t>(0, d.info.frameCount - 1));
    if (d.last && d.last->index == index)
        return d.last;
    // Decoding on is cheaper than a seek for a short step forward; a GOP is rarely longer than this.
    constexpr int64_t kForward = 48;
    const int64_t heldAt = d.heldAt();
    const bool ahead = heldAt >= 0 && index >= heldAt && index - heldAt <= kForward;
    bool found = false;
    if (ahead)
        found = d.decodeTo(index, err);
    for (double back : {0.0, 1.0, 5.0})
    {
        if (found)
            break;
        d.seek(index, back);
        found = d.decodeTo(index, err);
    }
    // A stream whose first picture is stamped after the asked time shows that picture.
    if (!found)
        found = d.holdPending(index);
    if (!found)
    {
        if (err && err->empty())
            *err = "no frame at " + std::to_string(index);
        return nullptr;
    }
    VideoFramePtr out = d.convertHeld(index);
    if (!out)
    {
        fail(err, "can't convert the picture");
        return nullptr;
    }
    d.last = out;
    return out;
}

// ---------------------------------------------------------------- VideoStream

std::shared_ptr<VideoStream> VideoStream::open(const fs::path& path, const Options& options, std::string* err)
{
    std::unique_ptr<VideoDecoder> dec = VideoDecoder::open(path, options.maxSize, err);
    if (!dec)
        return nullptr;
    std::shared_ptr<VideoStream> s(new VideoStream());
    s->mInfo = dec->info();
    s->mOptions = options;
    s->mDecoder = std::move(dec);
    s->mThread = std::thread([raw = s.get()] { raw->run(); });
    return s;
}

VideoStream::~VideoStream()
{
    {
        std::lock_guard lk(mMutex);
        mQuit = true;
    }
    mWake.notify_all();
    if (mThread.joinable())
        mThread.join();
}

void VideoStream::setOnFrame(std::function<void()> fn)
{
    std::lock_guard lk(mMutex);
    mOnFrame = std::move(fn);
}

size_t VideoStream::cachedBytes() const
{
    std::lock_guard lk(mMutex);
    return mBytes;
}

VideoFramePtr VideoStream::nearest(int64_t index) const
{
    if (mCache.empty())
        return nullptr;
    auto it = mCache.lower_bound(index);
    if (it == mCache.end())
        return std::prev(it)->second;
    if (it == mCache.begin())
        return it->second;
    auto before = std::prev(it);
    return index - before->first <= it->first - index ? before->second : it->second;
}

VideoFramePtr VideoStream::get(int64_t index, bool* exact)
{
    index = std::clamp<int64_t>(index, 0, std::max<int64_t>(0, mInfo.frameCount - 1));
    std::lock_guard lk(mMutex);
    if (index != mLastWanted)
    {
        if (mLastWanted >= 0)
            mDirection = index > mLastWanted ? 1 : -1;
        mLastWanted = index;
        mWake.notify_one();
    }
    if (auto it = mCache.find(index); it != mCache.end())
    {
        if (exact)
            *exact = true;
        return it->second;
    }
    mWanted = index;
    mWake.notify_one();
    if (exact)
        *exact = false;
    return nearest(index);
}

VideoFramePtr VideoStream::wait(int64_t index, double timeoutSeconds)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    bool exact = false;
    VideoFramePtr f = get(index, &exact);
    std::unique_lock lk(mMutex);
    const int64_t key = std::clamp<int64_t>(index, 0, std::max<int64_t>(0, mInfo.frameCount - 1));
    while (!exact)
    {
        if (auto it = mCache.find(key); it != mCache.end())
            return it->second;
        if (mLanded.wait_until(lk, until) == std::cv_status::timeout)
            return nullptr;
    }
    return f;
}

void VideoStream::store(VideoFramePtr f)
{
    const size_t bytes = f->rgba.size();
    auto [it, added] = mCache.try_emplace(f->index, f);
    if (!added)
        return;
    mBytes += bytes;
    while (mBytes > mOptions.cacheBytes && mCache.size() > 1)
    {
        auto front = mCache.begin(), back = std::prev(mCache.end());
        auto victim = std::abs(front->first - mLastWanted) >= std::abs(back->first - mLastWanted) ? front : back;
        mBytes -= victim->second->rgba.size();
        mCache.erase(victim);
    }
}

void VideoStream::run()
{
    std::unique_lock lk(mMutex);
    while (!mQuit)
    {
        int64_t job = -1;
        bool requested = false;
        if (mWanted >= 0)
        {
            job = std::exchange(mWanted, -1);
            requested = !mCache.contains(job);
            if (!requested)
                job = -1;
        }
        if (job < 0 && mLastWanted >= 0)
            for (int k = 1; k <= mOptions.readAhead; ++k)
            {
                const int64_t i = mLastWanted + int64_t(mDirection) * k;
                if (i < 0 || i >= mInfo.frameCount)
                    break;
                if (!mCache.contains(i))
                {
                    job = i;
                    break;
                }
            }
        if (job < 0)
        {
            mWake.wait(lk);
            continue;
        }
        lk.unlock();
        VideoFramePtr f = mDecoder->frame(job);
        lk.lock();
        if (f)
        {
            if (f->index != job)
            {
                auto copy = std::make_shared<VideoFrame>(*f);
                copy->index = job;
                f = std::move(copy);
            }
            store(f);
        }
        else
        {
            // An undecodable frame holds its neighbour, so the read-ahead moves past it instead of retrying forever.
            if (VideoFramePtr n = nearest(job))
            {
                auto copy = std::make_shared<VideoFrame>(*n);
                copy->index = job;
                store(copy);
            }
            else
                mLastWanted = -1;
        }
        mLanded.notify_all();
        if (requested && mOnFrame)
        {
            std::function<void()> fn = mOnFrame;
            lk.unlock();
            fn();
            lk.lock();
        }
    }
}

// ---------------------------------------------------------------- audio

namespace
{
/** Resamples the input's decoder output to stereo float at rate; null when the layout can't be converted. */
SwrContext* makeResampler(const Input& in, int rate)
{
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout source{};
    if (av_channel_layout_check(&in.dec->ch_layout) && in.dec->ch_layout.nb_channels > 0)
        av_channel_layout_copy(&source, &in.dec->ch_layout);
    else
        av_channel_layout_default(&source, std::max(1, in.dec->ch_layout.nb_channels));
    SwrContext* swr = nullptr;
    const int r = swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, rate, &source, in.dec->sample_fmt, in.dec->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&source);
    if (r < 0 || swr_init(swr) < 0)
    {
        swr_free(&swr);
        return nullptr;
    }
    return swr;
}

/** Appends count source samples, resampled, to out; null data drains what the resampler holds. */
void resampleInto(SwrContext* swr, const uint8_t** data, int count, std::vector<float>& out)
{
    const int room = swr_get_out_samples(swr, count);
    if (room <= 0)
        return;
    const size_t at = out.size();
    out.resize(at + size_t(room) * 2);
    uint8_t* dst = reinterpret_cast<uint8_t*>(out.data() + at);
    const int got = swr_convert(swr, &dst, room, data, count);
    out.resize(at + size_t(std::max(got, 0)) * 2);
}

const uint8_t** samplesOf(const AVFrame* f)
{
    return const_cast<const uint8_t**>(f->extended_data);
}

double streamDuration(const Input& in)
{
    const AVStream* st = in.st();
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
        return double(st->duration) * av_q2d(st->time_base);
    if (in.fmt->duration != AV_NOPTS_VALUE && in.fmt->duration > 0)
        return double(in.fmt->duration) / AV_TIME_BASE;
    return 0.0;
}

std::string tagOf(const AVStream* st, const char* key)
{
    const AVDictionaryEntry* e = av_dict_get(st->metadata, key, nullptr, 0);
    return e && e->value ? std::string(e->value) : std::string();
}
} // namespace

std::vector<AudioTrack> audioTracks(const fs::path& path, std::string* err)
{
    std::vector<AudioTrack> out;
    Input in;
    std::string why;
    if (!in.probe(path, AVMEDIA_TYPE_AUDIO, &why))
    {
        // A readable file without sound has no tracks; only an unreadable one is an error.
        if (!in.fmt || !in.fmt->nb_streams)
            fail(err, why);
        return out;
    }
    for (unsigned i = 0; i < in.fmt->nb_streams; ++i)
    {
        const AVStream* st = in.fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        AudioTrack t;
        t.index = int(out.size());
        t.codec = avcodec_get_name(st->codecpar->codec_id);
        t.channels = st->codecpar->ch_layout.nb_channels;
        t.rate = st->codecpar->sample_rate;
        in.stream = int(i);
        t.duration = streamDuration(in);
        t.language = tagOf(st, "language");
        t.title = tagOf(st, "title");
        out.push_back(std::move(t));
    }
    return out;
}

AudioBufferPtr decodeAudio(const fs::path& path, int rate, std::string* err, int track)
{
    LILA_ZONE();

    Input in;
    if (!in.open(path, AVMEDIA_TYPE_AUDIO, err, track))
        return nullptr;
    SwrContext* swr = makeResampler(in, rate);
    if (!swr)
    {
        fail(err, paths::toUtf8(path) + ": can't resample the audio");
        return nullptr;
    }
    auto out = std::make_shared<AudioBuffer>();
    out->rate = rate;
    out->channels = 2;
    Packet pkt;
    Frame frame;
    for (;;)
    {
        const int r = in.receive(frame.f, pkt.p);
        if (r == AVERROR_EOF)
            break;
        if (r < 0)
        {
            swr_free(&swr);
            fail(err, paths::toUtf8(path) + ": " + avError(r));
            return nullptr;
        }
        resampleInto(swr, samplesOf(frame.f), frame.f->nb_samples, out->samples);
        av_frame_unref(frame.f);
    }
    resampleInto(swr, nullptr, 0, out->samples);
    swr_free(&swr);
    return out;
}

// ---------------------------------------------------------------- AudioStream

struct AudioStream::Impl
{
    Options o;
    fs::path path;

    // Decode thread only.
    Input in;
    SwrContext* swr = nullptr;
    Packet pkt;
    Frame frame;
    AVRational tb{1, 1};
    int64_t startPts = 0;
    /** Resampled samples from output frame carryStart on; carryStart < 0 after a seek until a frame lands. */
    std::vector<float> carry;
    int64_t carryStart = 0;
    bool eof = false;

    mutable std::mutex m;
    std::condition_variable wake;
    mutable std::condition_variable landed;
    std::map<int64_t, Block> cache;
    size_t bytes = 0;
    int64_t wanted = -1;
    int64_t last = -1;
    int dir = 1;
    double lastPrefetch = -1.0;
    std::atomic<bool> quit{false};
    std::atomic<int64_t> frames{0};
    std::atomic<int64_t> seeks{0};
    std::shared_ptr<const Peaks> peaks = std::make_shared<const Peaks>();
    std::atomic<bool> scanned{false};
    std::thread decodeThread;
    std::thread scanThread;

    ~Impl() { swr_free(&swr); }

    int64_t carryFrames() const { return int64_t(carry.size()) / 2; }

    void seekTo(int64_t at)
    {
        const double seconds = double(at) / o.rate;
        av_seek_frame(in.fmt, in.stream, startPts + int64_t(seconds / av_q2d(tb)), AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(in.dec);
        in.flushed = false;
        eof = false;
        swr_free(&swr);
        swr = makeResampler(in, o.rate);
        carry.clear();
        carryStart = -1;
        ++seeks;
    }

    void decodeOne(int64_t fallbackStart)
    {
        const int r = swr ? in.receive(frame.f, pkt.p) : AVERROR_EOF;
        if (r < 0)
        {
            if (swr)
                resampleInto(swr, nullptr, 0, carry);
            if (carryStart < 0)
                carryStart = fallbackStart;
            eof = true;
            return;
        }
        if (carryStart < 0)
        {
            const AVFrame* f = frame.f;
            const int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
            carryStart = pts == AV_NOPTS_VALUE ? fallbackStart : std::llround(double(pts - startPts) * av_q2d(tb) * o.rate);
        }
        resampleInto(swr, samplesOf(frame.f), frame.f->nb_samples, carry);
        av_frame_unref(frame.f);
    }

    std::vector<float> decodeBlock(int64_t index)
    {
        LILA_ZONE();

        const int64_t want = index * o.blockFrames, end = want + o.blockFrames;
        if (carryStart < 0 || want < carryStart || want > carryStart + carryFrames())
            seekTo(want);
        while (!eof && !quit && (carryStart < 0 || carryStart + carryFrames() < end))
            decodeOne(want);
        std::vector<float> out;
        if (carryStart < 0)
            return out;
        if (carryStart > want)
        {
            carry.insert(carry.begin(), size_t(carryStart - want) * 2, 0.0f);
            carryStart = want;
        }
        const size_t from = size_t(want - carryStart) * 2;
        const size_t to = std::min(carry.size(), size_t(end - carryStart) * 2);
        if (from < to)
            out.assign(carry.begin() + std::ptrdiff_t(from), carry.begin() + std::ptrdiff_t(to));
        const size_t used = std::min(carry.size(), std::max(from, to));
        carry.erase(carry.begin(), carry.begin() + std::ptrdiff_t(used));
        carryStart += int64_t(used) / 2;
        return out;
    }

    void store(int64_t index, Block b)
    {
        auto [it, added] = cache.try_emplace(index, std::move(b));
        if (!added)
            return;
        bytes += it->second->size() * sizeof(float);
        while (bytes > o.cacheBytes && cache.size() > 1)
        {
            auto front = cache.begin(), back = std::prev(cache.end());
            auto victim = std::abs(front->first - last) >= std::abs(back->first - last) ? front : back;
            bytes -= victim->second->size() * sizeof(float);
            cache.erase(victim);
        }
    }

    int64_t blockCount() const { return (frames.load() + o.blockFrames - 1) / o.blockFrames; }

    /** The first block not decoded yet among last + step * k for k in [from, to]; -1 when all are in. */
    int64_t firstMissing(int step, int from, int to) const
    {
        for (int k = from; k <= to; ++k)
        {
            const int64_t i = last + int64_t(step) * k;
            if (i < 0 || i >= blockCount())
                break;
            if (!cache.contains(i))
                return i;
        }
        return -1;
    }

    void runDecode()
    {
        // Both windows fit the cache with a block to spare, or filling one would evict the other forever.
        const size_t blockBytes = size_t(o.blockFrames) * 2 * sizeof(float);
        const int room = std::max(1, int(o.cacheBytes / blockBytes) - 1);
        const int ahead = std::clamp(int(std::ceil(o.readAheadSeconds * o.rate / o.blockFrames)), 1, room);
        const int behind = std::clamp(int(std::ceil(o.readBehindSeconds * o.rate / o.blockFrames)), 0, room - ahead);
        std::unique_lock lk(m);
        while (!quit)
        {
            int64_t job = -1;
            if (wanted >= 0)
            {
                job = std::exchange(wanted, -1);
                if (cache.contains(job))
                    job = -1;
            }
            if (job < 0 && last >= 0)
                job = firstMissing(dir, 0, ahead);
            if (job < 0 && last >= 0)
                job = firstMissing(-dir, 1, behind);
            if (job < 0)
            {
                wake.wait(lk);
                continue;
            }
            lk.unlock();
            auto b = std::make_shared<const std::vector<float>>(decodeBlock(job));
            lk.lock();
            store(job, std::move(b));
            landed.notify_all();
        }
    }

    void runScan()
    {
        LILA_ZONE();

        Input scan;
        SwrContext* r = scan.open(path, AVMEDIA_TYPE_AUDIO, nullptr, o.track) ? makeResampler(scan, o.rate) : nullptr;
        if (!r)
        {
            scanned = true;
            landed.notify_all();
            return;
        }
        const int64_t per = std::max<int64_t>(1, std::llround(o.peakSeconds * o.rate));
        Peaks p;
        p.secondsPerBucket = double(per) / o.rate;
        float lo = 0.0f, hi = 0.0f;
        int64_t inBucket = 0, total = 0;
        size_t published = 0;
        std::vector<float> chunk;
        Packet sp;
        Frame sf;
        auto fold = [&] {
            for (size_t i = 0; i + 1 < chunk.size(); i += 2)
            {
                lo = std::min({lo, chunk[i], chunk[i + 1]});
                hi = std::max({hi, chunk[i], chunk[i + 1]});
                if (++inBucket == per)
                {
                    p.low.push_back(lo);
                    p.high.push_back(hi);
                    lo = hi = 0.0f;
                    inBucket = 0;
                }
            }
            total += int64_t(chunk.size()) / 2;
            chunk.clear();
        };
        auto publish = [&] {
            std::lock_guard lk(m);
            peaks = std::make_shared<const Peaks>(p);
            published = p.low.size();
        };
        while (!quit)
        {
            if (scan.receive(sf.f, sp.p) < 0)
                break;
            resampleInto(r, samplesOf(sf.f), sf.f->nb_samples, chunk);
            av_frame_unref(sf.f);
            fold();
            // A snapshot every few seconds of sound, so the waveform fills in while the scan runs.
            if (p.low.size() - published >= size_t(4.0 / p.secondsPerBucket))
                publish();
        }
        resampleInto(r, nullptr, 0, chunk);
        fold();
        swr_free(&r);
        if (quit)
            return;
        if (inBucket > 0)
        {
            p.low.push_back(lo);
            p.high.push_back(hi);
        }
        publish();
        {
            std::lock_guard lk(m);
            frames = std::max<int64_t>(1, total);
            scanned = true;
        }
        wake.notify_all();
        landed.notify_all();
    }
};

AudioStream::AudioStream() : mImpl(std::make_unique<Impl>()) {}

AudioStream::~AudioStream()
{
    {
        std::lock_guard lk(mImpl->m);
        mImpl->quit = true;
    }
    mImpl->wake.notify_all();
    mImpl->landed.notify_all();
    if (mImpl->decodeThread.joinable())
        mImpl->decodeThread.join();
    if (mImpl->scanThread.joinable())
        mImpl->scanThread.join();
}

std::shared_ptr<AudioStream> AudioStream::open(const fs::path& path, const Options& options, std::string* err)
{
    LILA_ZONE();

    std::shared_ptr<AudioStream> s(new AudioStream());
    Impl& d = *s->mImpl;
    d.o = options;
    d.o.blockFrames = std::max(256, d.o.blockFrames);
    d.path = path;
    if (!d.in.open(path, AVMEDIA_TYPE_AUDIO, err, options.track))
        return nullptr;
    d.swr = makeResampler(d.in, d.o.rate);
    if (!d.swr)
    {
        fail(err, paths::toUtf8(path) + ": can't resample the audio");
        return nullptr;
    }
    d.tb = d.in.st()->time_base;
    d.startPts = d.in.st()->start_time != AV_NOPTS_VALUE ? d.in.st()->start_time : 0;
    d.frames = std::max<int64_t>(1, std::llround(streamDuration(d.in) * d.o.rate));
    d.decodeThread = std::thread([&d] { d.runDecode(); });
    d.scanThread = std::thread([&d] { d.runScan(); });
    return s;
}

int AudioStream::rate() const
{
    return mImpl->o.rate;
}

int AudioStream::blockFrames() const
{
    return mImpl->o.blockFrames;
}

int64_t AudioStream::frames() const
{
    return mImpl->frames.load();
}

AudioStream::Block AudioStream::block(int64_t index)
{
    Impl& d = *mImpl;
    std::lock_guard lk(d.m);
    const bool moved = index != d.last;
    d.last = index;
    if (auto it = d.cache.find(index); it != d.cache.end())
    {
        if (moved)
            d.wake.notify_one();
        return it->second;
    }
    d.wanted = index;
    d.wake.notify_one();
    return nullptr;
}

void AudioStream::prefetch(double seconds, int direction)
{
    {
        std::lock_guard lk(mImpl->m);
        Impl& d = *mImpl;
        if (direction != 0)
            d.dir = direction > 0 ? 1 : -1;
        else if (d.lastPrefetch >= 0.0 && seconds != d.lastPrefetch)
            d.dir = seconds < d.lastPrefetch ? -1 : 1;
        // A play start is no reference for the next scrub's direction: the first scrub after it keeps the play's.
        d.lastPrefetch = direction != 0 ? -1.0 : seconds;
    }
    mImpl->wake.notify_one();
    block(std::max<int64_t>(0, int64_t(seconds * rate()) / blockFrames()));
}

int AudioStream::direction() const
{
    std::lock_guard lk(mImpl->m);
    return mImpl->dir;
}

bool AudioStream::ready(int64_t index) const
{
    std::lock_guard lk(mImpl->m);
    return mImpl->cache.contains(index);
}

bool AudioStream::waitFor(double seconds, double timeoutSeconds)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    const int64_t index = std::max<int64_t>(0, int64_t(seconds * rate()) / blockFrames());
    if (block(index))
        return true;
    std::unique_lock lk(mImpl->m);
    while (!mImpl->cache.contains(index))
        if (mImpl->landed.wait_until(lk, until) == std::cv_status::timeout)
            return false;
    return true;
}

std::shared_ptr<const Peaks> AudioStream::peaks() const
{
    std::lock_guard lk(mImpl->m);
    return mImpl->peaks;
}

bool AudioStream::scanned() const
{
    return mImpl->scanned.load();
}

bool AudioStream::waitScanned(double timeoutSeconds) const
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    std::unique_lock lk(mImpl->m);
    while (!mImpl->scanned)
        if (mImpl->landed.wait_until(lk, until) == std::cv_status::timeout)
            return mImpl->scanned.load();
    return true;
}

size_t AudioStream::cachedBytes() const
{
    std::lock_guard lk(mImpl->m);
    return mImpl->bytes;
}

int64_t AudioStream::seeks() const
{
    return mImpl->seeks.load();
}

Peaks peaks(const AudioBuffer& buffer, double secondsPerBucket)
{
    Peaks p;
    p.secondsPerBucket = secondsPerBucket;
    if (!(secondsPerBucket > 0.0) || buffer.channels <= 0)
        return p;
    const int64_t per = std::max<int64_t>(1, std::llround(secondsPerBucket * buffer.rate));
    const int64_t frames = buffer.frames();
    const size_t buckets = size_t((frames + per - 1) / per);
    p.low.assign(buckets, 0.0f);
    p.high.assign(buckets, 0.0f);
    for (size_t b = 0; b < buckets; ++b)
    {
        float lo = 0.0f, hi = 0.0f;
        const int64_t end = std::min(frames, int64_t(b + 1) * per);
        for (int64_t f = int64_t(b) * per; f < end; ++f)
            for (int c = 0; c < buffer.channels; ++c)
            {
                const float v = buffer.samples[size_t(f * buffer.channels + c)];
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        p.low[b] = lo;
        p.high[b] = hi;
    }
    return p;
}
} // namespace lila::media
