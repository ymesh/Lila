#include <doctest/doctest.h>

#include "TestClips.h"

#include "media/Decode.h"
#include "media/Media.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/ImagePlane.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include <cmath>
#include <cstring>
#include <fstream>
#include <random>
#include <thread>
#include <stdexcept>
#include <vector>

namespace mediatest
{
namespace fs = std::filesystem;

std::filesystem::path writeTracks(const std::string& name, int rate, double seconds, const std::vector<double>& clicks, int videoFrames)
{
    const fs::path path = scratch() / name;
    const std::string file = path.string();
    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, nullptr, file.c_str()) < 0 || !oc)
        throw std::runtime_error("no muxer for " + file);
    struct Out
    {
        AVStream* st = nullptr;
        AVCodecContext* c = nullptr;
    };
    std::vector<Out> outs;
    auto add = [&](AVCodecID id, auto&& setup) {
        const AVCodec* codec = avcodec_find_encoder(id);
        if (!codec)
            throw std::runtime_error(std::string("no encoder ") + avcodec_get_name(id));
        Out o;
        o.st = avformat_new_stream(oc, nullptr);
        o.c = avcodec_alloc_context3(codec);
        setup(o.c);
        if (oc->oformat->flags & AVFMT_GLOBALHEADER)
            o.c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(o.c, codec, nullptr) < 0)
            throw std::runtime_error("can't open an encoder");
        avcodec_parameters_from_context(o.st->codecpar, o.c);
        o.st->time_base = o.c->time_base;
        outs.push_back(o);
    };
    const int fps = 24;
    if (videoFrames > 0)
        add(AV_CODEC_ID_MPEG4, [&](AVCodecContext* c) {
            c->width = 64;
            c->height = 48;
            c->time_base = {1, fps};
            c->framerate = {fps, 1};
            c->pix_fmt = AV_PIX_FMT_YUV420P;
            c->gop_size = 6;
        });
    const size_t firstAudio = outs.size();
    for (size_t t = 0; t < clicks.size(); ++t)
        add(AV_CODEC_ID_PCM_S16LE, [&](AVCodecContext* c) {
            c->sample_rate = rate;
            c->sample_fmt = AV_SAMPLE_FMT_S16;
            c->time_base = {1, rate};
            AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
            av_channel_layout_copy(&c->ch_layout, &mono);
        });
    if (avio_open(&oc->pb, file.c_str(), AVIO_FLAG_WRITE) < 0 || avformat_write_header(oc, nullptr) < 0)
        throw std::runtime_error("can't write " + file);
    AVPacket* pkt = av_packet_alloc();
    auto drain = [&](const Out& o) {
        while (avcodec_receive_packet(o.c, pkt) == 0)
        {
            av_packet_rescale_ts(pkt, o.c->time_base, o.st->time_base);
            pkt->stream_index = o.st->index;
            av_interleaved_write_frame(oc, pkt);
        }
    };
    const int total = int(std::lround(seconds * rate));
    const int chunk = rate / 10;
    int vf = 0;
    for (int at = 0; at < total; at += chunk)
    {
        while (videoFrames > 0 && vf < videoFrames && double(vf) / fps <= double(at) / rate)
        {
            AVFrame* f = av_frame_alloc();
            f->width = 64;
            f->height = 48;
            f->format = AV_PIX_FMT_YUV420P;
            av_frame_get_buffer(f, 0);
            for (int p = 0; p < 3; ++p)
                for (int y = 0; y < (p ? 24 : 48); ++y)
                    std::memset(f->data[p] + y * f->linesize[p], p ? 128 : 20 + 7 * vf, size_t(p ? 32 : 64));
            f->pts = vf++;
            avcodec_send_frame(outs[0].c, f);
            av_frame_free(&f);
            drain(outs[0]);
        }
        const int n = std::min(chunk, total - at);
        for (size_t t = 0; t < clicks.size(); ++t)
        {
            const Out& o = outs[firstAudio + t];
            AVFrame* f = av_frame_alloc();
            f->nb_samples = n;
            f->format = AV_SAMPLE_FMT_S16;
            f->sample_rate = rate;
            av_channel_layout_copy(&f->ch_layout, &o.c->ch_layout);
            av_frame_get_buffer(f, 0);
            auto* s = reinterpret_cast<int16_t*>(f->data[0]);
            const int click = int(std::lround(clicks[t] * rate));
            for (int i = 0; i < n; ++i)
                s[i] = (at + i >= click && at + i < click + 10) ? int16_t(32767) : int16_t(0);
            f->pts = at;
            avcodec_send_frame(o.c, f);
            av_frame_free(&f);
            drain(o);
        }
    }
    for (const Out& o : outs)
    {
        avcodec_send_frame(o.c, nullptr);
        drain(o);
    }
    av_write_trailer(oc);
    avio_closep(&oc->pb);
    av_packet_free(&pkt);
    for (Out& o : outs)
        avcodec_free_context(&o.c);
    avformat_free_context(oc);
    return path;
}
} // namespace mediatest

namespace
{
using namespace lila;
using json = nlohmann::json;

/** A 16-bit mono WAV of a ramp-like signal, so every sample differs from its neighbours: value i at sample i. */
std::filesystem::path writeTone(const std::string& name, int rate, double seconds)
{
    const auto path = mediatest::scratch() / name;
    const int n = int(std::lround(seconds * rate));
    std::vector<int16_t> pcm(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        pcm[size_t(i)] = int16_t(std::lround(12000.0 * std::sin(i * 0.013) + 8000.0 * std::sin(i * 0.0007)));
    std::ofstream file(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { file.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { file.write(reinterpret_cast<const char*>(&v), 2); };
    file.write("RIFF", 4);
    u32(uint32_t(36 + n * 2));
    file.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(uint32_t(rate));
    u32(uint32_t(rate * 2));
    u16(2);
    u16(16);
    file.write("data", 4);
    u32(uint32_t(n * 2));
    file.write(reinterpret_cast<const char*>(pcm.data()), std::streamsize(pcm.size() * 2));
    return path;
}

int64_t loudest(const std::vector<float>& s, int64_t from, int64_t to)
{
    int64_t best = from;
    for (int64_t i = from; i < to && size_t(i * 2) < s.size(); ++i)
        if (std::abs(s[size_t(i * 2)]) > std::abs(s[size_t(best * 2)]))
            best = i;
    return best;
}
} // namespace

TEST_CASE("audio tracks: a file with several sound tracks lists them and decodes the one asked for")
{
    const auto path = mediatest::writeTracks("two_tracks.mkv", 48000, 2.0, {0.25, 1.5}, 12);
    std::string err;
    std::vector<media::AudioTrack> tracks = media::audioTracks(path, &err);
    REQUIRE_MESSAGE(tracks.size() == 2, err);
    CHECK(tracks[1].index == 1);
    CHECK(tracks[0].channels == 1);
    CHECK(tracks[0].rate == 48000);

    media::AudioBufferPtr first = media::decodeAudio(path, 48000, &err, 0);
    media::AudioBufferPtr second = media::decodeAudio(path, 48000, &err, 1);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(double(loudest(first->samples, 0, first->frames())) / 48000.0 == doctest::Approx(0.25).epsilon(0.002));
    CHECK(double(loudest(second->samples, 0, second->frames())) / 48000.0 == doctest::Approx(1.5).epsilon(0.002));
    CHECK_FALSE(media::decodeAudio(path, 48000, &err, 2));
    CHECK(err.find("no audio track 3") != std::string::npos);

    // A sound file is one track; a video without sound has none, which isn't an error.
    CHECK(media::audioTracks(mediatest::writeWav("one.wav", 22050, 0.2, 0.1)).size() == 1);
    err.clear();
    CHECK(media::audioTracks(mediatest::writeClip("mute.mkv", 4, 24, 4), &err).empty());
    CHECK(err.empty());

    json info = ops::execute("mediaInfo", {{"args", {path.string()}}});
    CHECK(info["audioTracks"].size() == 2);
    CHECK(info["video"]["width"] == 64);
}

TEST_CASE("audio stream: blocks match the whole-file decode, in order and after seeks")
{
    const auto path = writeTone("tone.wav", 48000, 12.0);
    std::string err;
    media::AudioBufferPtr whole = media::decodeAudio(path, 48000, &err);
    REQUIRE_MESSAGE(whole, err);

    media::AudioStream::Options o;
    o.blockFrames = 4096;
    o.cacheBytes = size_t(4096) * 2 * sizeof(float) * 24;
    o.readAheadSeconds = 0.2;
    std::shared_ptr<media::AudioStream> s = media::AudioStream::open(path, o, &err);
    REQUIRE_MESSAGE(s, err);
    CHECK(s->frames() == doctest::Approx(double(whole->frames())).epsilon(0.001));

    auto checkBlock = [&](int64_t index) {
        CAPTURE(index);
        REQUIRE(s->waitFor(double(index * o.blockFrames) / 48000.0, 10.0));
        media::AudioStream::Block b = s->block(index);
        REQUIRE(b);
        const size_t at = size_t(index * o.blockFrames) * 2;
        const size_t n = std::min(b->size(), whole->samples.size() - at);
        CHECK(n == std::min<size_t>(size_t(o.blockFrames) * 2, whole->samples.size() - at));
        double worst = 0.0;
        for (size_t i = 0; i < n; ++i)
            worst = std::max(worst, double(std::abs((*b)[i] - whole->samples[at + i])));
        CHECK(worst < 1e-5);
    };
    for (int64_t i = 0; i < 4; ++i)
        checkBlock(i);
    const int64_t seeksBefore = s->seeks();
    std::mt19937 rng(7);
    const int64_t blocks = (whole->frames() + o.blockFrames - 1) / o.blockFrames;
    for (int k = 0; k < 12; ++k)
        checkBlock(int64_t(rng() % uint32_t(blocks)));
    checkBlock(blocks - 1);
    CHECK(s->seeks() > seeksBefore);
    // The cache keeps to its budget, give or take the block being stored.
    CHECK(s->cachedBytes() <= o.cacheBytes + size_t(o.blockFrames) * 2 * sizeof(float));

    REQUIRE(s->waitScanned(20.0));
    CHECK(s->frames() == whole->frames());
    std::shared_ptr<const media::Peaks> p = s->peaks();
    media::Peaks ref = media::peaks(*whole, p->secondsPerBucket);
    REQUIRE(p->high.size() == ref.high.size());
    CHECK(p->high[1000] == doctest::Approx(ref.high[1000]).epsilon(1e-4));
    CHECK(p->low[3000] == doctest::Approx(ref.low[3000]).epsilon(1e-4));
}

TEST_CASE("audio stream: resampled seeks land on the right sample")
{
    const auto path = mediatest::writeTracks("clicks.mkv", 44100, 8.0, {6.3}, 0);
    std::string err;
    media::AudioStream::Options o;
    o.blockFrames = 2048;
    std::shared_ptr<media::AudioStream> s = media::AudioStream::open(path, o, &err);
    REQUIRE_MESSAGE(s, err);
    // Where the whole-file decode puts the click: a seek must land the stream on the same sample.
    media::AudioBufferPtr whole = media::decodeAudio(path, 48000, &err);
    REQUIRE(whole);
    const int64_t click = loudest(whole->samples, 0, whole->frames());
    CHECK(std::abs(click - std::llround(6.3 * 48000)) <= 16);
    const int64_t index = click / o.blockFrames;
    REQUIRE(s->waitFor(6.3, 10.0));
    media::AudioStream::Block b = s->block(index);
    REQUIRE(b);
    const int64_t found = index * o.blockFrames + loudest(*b, 0, int64_t(b->size() / 2));
    CHECK(std::abs(found - click) <= 1);
    CHECK(s->seeks() >= 1);
}

TEST_CASE("audio stream: playing backwards and scrubbing back are never silent")
{
    const auto path = writeTone("tone_reverse.wav", 48000, 12.0);
    std::string err;
    media::AudioBufferPtr whole = media::decodeAudio(path, 48000, &err);
    REQUIRE_MESSAGE(whole, err);
    media::AudioStream::Options o;
    o.blockFrames = 4096;
    o.readAheadSeconds = 0.5;
    o.readBehindSeconds = 0.3;
    const int ahead = 6, behind = 4;
    auto blockAt = [&](double seconds) { return int64_t(seconds * 48000.0) / o.blockFrames; };
    auto waitReady = [](media::AudioStream& s, int64_t index) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!s.ready(index))
        {
            if (std::chrono::steady_clock::now() > until)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    };

    SUBCASE("played backwards")
    {
        std::shared_ptr<media::AudioStream> s = media::AudioStream::open(path, o, &err);
        REQUIRE_MESSAGE(s, err);
        s->prefetch(10.0, -1);
        CHECK(s->direction() == -1);
        const int64_t at = blockAt(10.0);
        for (int k = 0; k <= ahead; ++k)
            CHECK(waitReady(*s, at - k));
        for (int k = 1; k <= behind; ++k)
            CHECK(waitReady(*s, at + k));
        CHECK_FALSE(s->ready(at + behind + 2));

        // Backwards through the mixer at four times real time, as the device would pull it: every chunk sounds.
        media::Voice a;
        a.buffer = whole;
        a.to = 12.0;
        media::Voice b = a;
        b.buffer = nullptr;
        b.stream = s;
        const int chunk = 1024;
        std::vector<float> wa(chunk * 2), wb(chunk * 2);
        int chunks = 0, wrong = 0;
        for (double t = 10.0; t > 4.0; t -= double(chunk) / 48000.0, ++chunks)
        {
            media::mix({a}, t, -1.0, 48000, wa.data(), chunk);
            media::mix({b}, t, -1.0, 48000, wb.data(), chunk);
            double worst = 0.0;
            for (size_t i = 0; i < wa.size(); ++i)
                worst = std::max(worst, double(std::abs(wa[i] - wb[i])));
            wrong += worst > 1e-5 ? 1 : 0;
            std::this_thread::sleep_for(std::chrono::microseconds(int64_t(1e6 * chunk / 48000.0 / 4.0)));
        }
        CHECK(chunks > 250);
        CHECK(wrong == 0);
    }

    SUBCASE("scrubbed back from where play started")
    {
        std::shared_ptr<media::AudioStream> s = media::AudioStream::open(path, o, &err);
        REQUIRE_MESSAGE(s, err);
        s->prefetch(8.0, 1);
        const int64_t at = blockAt(8.0);
        for (int k = 0; k <= ahead; ++k)
            CHECK(waitReady(*s, at + k));
        // Behind the playhead too, so the first step back has its block before it asks.
        for (int k = 1; k <= behind; ++k)
            CHECK(waitReady(*s, at - k));

        int steps = 0, missing = 0;
        for (double t = 8.0 - 1.0 / 24.0; t > 5.0; t -= 1.0 / 24.0, ++steps)
        {
            missing += s->ready(blockAt(t)) ? 0 : 1;
            s->prefetch(t);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(steps >= 70);
        CHECK(missing == 0);
        CHECK(s->direction() == -1);
        // Turning round forwards again follows the scrub.
        s->prefetch(5.5);
        CHECK(s->direction() == 1);
    }
}

TEST_CASE("mix: a streamed voice sounds like the same voice decoded whole, and trims cut both ends")
{
    const auto path = writeTone("tone_mix.wav", 48000, 4.0);
    media::AudioBufferPtr whole = media::decodeAudio(path, 48000);
    media::AudioStream::Options o;
    o.blockFrames = 1024;
    std::shared_ptr<media::AudioStream> s = media::AudioStream::open(path, o);
    REQUIRE(whole);
    REQUIRE(s);
    media::Voice a;
    a.buffer = whole;
    a.start = 1.0;
    a.from = 0.5;
    a.to = 2.0;
    media::Voice b = a;
    b.buffer = nullptr;
    b.stream = s;
    // Scene 1.25 s is file 0.75 s.
    REQUIRE(s->waitFor(0.75, 10.0));
    REQUIRE(s->waitFor(0.75 + 512.0 / 48000.0, 10.0));
    std::vector<float> wa(1024), wb(1024);
    media::mix({a}, 1.25, 1.0, 48000, wa.data(), 512);
    media::mix({b}, 1.25, 1.0, 48000, wb.data(), 512);
    double worst = 0.0, level = 0.0;
    for (size_t i = 0; i < wa.size(); ++i)
    {
        worst = std::max(worst, double(std::abs(wa[i] - wb[i])));
        level = std::max(level, double(std::abs(wa[i])));
    }
    CHECK(level > 0.05);
    CHECK(worst < 1e-5);
    // File 0.5 s plays at scene 1.0 s; just before it, and past file 2.0 s (scene 2.5 s), is silence.
    media::mix({a}, 0.999, 1.0, 48000, wa.data(), 200);
    CHECK(wa[0] == 0.0f);
    CHECK(wa[2 * 199] != 0.0f);
    media::mix({a}, 2.5, 1.0, 48000, wa.data(), 100);
    CHECK(std::ranges::all_of(wa.begin(), wa.begin() + 200, [](float v) { return v == 0.0f; }));
    CHECK(a.end() == doctest::Approx(2.5));
}

TEST_CASE("audio node: trim, track and streaming reach its voice")
{
    ops::execute("newScene", json::object());
    ops::execute("playbackOptions", {{"fps", 24.0}});
    const auto two = mediatest::writeTracks("node_tracks.mkv", 48000, 3.0, {0.5, 2.0}, 0);
    ops::execute("audio", {{"fileName", two.string()}, {"name", "dialog"}, {"offset", 24.0}, {"sourceStart", 12.0}, {"sourceEnd", 60.0}, {"track", 1}});
    scene::Graph& g = scene::Graph::current();
    std::optional<media::Voice> v = media::voiceOf(g, g.find("dialog")->id(), true);
    REQUIRE(v);
    CHECK(v->start == doctest::Approx(1.0));
    CHECK(v->from == doctest::Approx(0.5));
    CHECK(v->to == doctest::Approx(2.5));
    CHECK(v->end() == doctest::Approx(3.0));
    REQUIRE(v->buffer);
    CHECK(double(loudest(v->buffer->samples, 0, v->buffer->frames())) / 48000.0 == doctest::Approx(2.0).epsilon(0.002));

    // Longer than the threshold: the voice streams.
    const double before = media::streamSeconds();
    media::setStreamSeconds(1.0);
    const auto bed = writeTone("bed.wav", 48000, 3.0);
    ops::execute("audio", {{"fileName", bed.string()}, {"name", "bed"}});
    std::optional<media::Voice> streamed = media::voiceOf(g, g.find("bed")->id(), true);
    media::setStreamSeconds(before);
    REQUIRE(streamed);
    CHECK(streamed->stream);
    CHECK_FALSE(streamed->buffer);
    json list = ops::execute("listMedia", json::object());
    bool found = false;
    for (const json& m : list)
        if (m["name"] == "bed")
            found = m.value("streamed", false);
    CHECK(found);
}

TEST_CASE("import reference: video and its sound land on the active take, and slide together")
{
    ops::execute("newScene", json::object());
    ops::execute("playbackOptions", {{"fps", 24.0}, {"minTime", 1.0}});
    ops::execute("createTake", {{"name", "Shot"}, {"start", 2.0}, {"end", 6.0}, {"fps", 24.0}});
    ops::execute("setActiveTake", {{"args", {"Shot"}}});
    const auto clip = mediatest::writeTracks("shot010.mkv", 48000, 1.0, {0.5}, 24);
    json made = ops::execute("importFootage", {{"args", {clip.string()}}});
    REQUIRE(made.contains("imagePlane"));
    REQUIRE(made.contains("audio"));
    CHECK(made["take"] == "Shot");
    scene::Graph& g = scene::Graph::current();
    const scene::NodeId plane = g.find(made["imagePlane"].get<std::string>())->id();
    const scene::NodeId sound = g.find(made["audio"].get<std::string>())->id();
    CHECK(media::takesOf(g, plane).size() == 1);
    CHECK(media::takesOf(g, sound).size() == 1);
    CHECK(media::startSeconds(g, sound, 0.0) == doctest::Approx(2.0));

    ops::execute("slideMedia", {{"args", {made["imagePlane"], made["audio"]}}, {"frames", 6.0}});
    CHECK(g.node(sound)->as<double>(media::AudioNode::kOffset) == doctest::Approx(6.0));
    CHECK(g.node(plane)->as<double>(scene::ImagePlane::kStartFrame) == doctest::Approx(6.0));
    ops::execute("undo", json::object());
    CHECK(g.node(sound)->as<double>(media::AudioNode::kOffset) == doctest::Approx(0.0));

    // Without a take, at the playback start (the active take set it to the take's range); a sound file alone makes only the audio node.
    const auto wav = mediatest::writeWav("line.wav", 48000, 0.5, 0.1);
    json alone = ops::execute("importFootage", {{"args", {wav.string()}}, {"take", ""}});
    CHECK_FALSE(alone.contains("imagePlane"));
    CHECK(g.find(alone["audio"].get<std::string>())->as<double>(media::AudioNode::kOffset) ==
          doctest::Approx(std::round(scene::anim::frameOf(scene::anim::playback().minTime))));
    // A video without sound makes only the image plane.
    json silent = ops::execute("importFootage", {{"args", {mediatest::writeClip("nosound.mp4", 4, 24, 4).string()}}, {"take", ""}});
    CHECK(silent.contains("imagePlane"));
    CHECK_FALSE(silent.contains("audio"));
}
