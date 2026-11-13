#include "ui/VideoTextures.h"

#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "render/GpuImage.h"

#include <atomic>
#include <cmath>
#include <map>
#include <memory>

namespace lila::ui::videoTextures
{
namespace
{
/** Viewport frames are capped at this many pixels on the longer side, which keeps 4K footage scrubbable. */
constexpr int kVideoMaxSize = 2048;

struct VideoEntry
{
    std::shared_ptr<media::VideoStream> stream;
    std::string error;
    render::GpuImageRef gpu;
    render::TexturePtr texture;
    int64_t uploaded = -1;
    int64_t imaged = -1;
    QImage qimage;
};

std::map<std::string, VideoEntry>& videoEntries()
{
    static std::map<std::string, VideoEntry> m;
    return m;
}

std::map<int, std::function<void()>>& videoListeners()
{
    static std::map<int, std::function<void()>> m;
    return m;
}

std::atomic<bool>& videoNotifyQueued()
{
    static std::atomic<bool> b{false};
    return b;
}

void notifyVideoListeners()
{
    // One queued notification covers every frame that lands before the main thread gets to it.
    if (videoNotifyQueued().exchange(true))
        return;
    mainq::post([] {
        videoNotifyQueued() = false;
        auto copy = videoListeners();
        for (auto& [id, fn] : copy)
            fn();
    });
}

VideoEntry* videoEntry(const std::string& path)
{
    if (path.empty())
        return nullptr;
    VideoEntry& e = videoEntries()[path];
    if (!e.stream && e.error.empty())
    {
        media::VideoStream::Options o;
        o.maxSize = kVideoMaxSize;
        std::string err;
        e.stream = media::VideoStream::open(paths::fromUtf8(path), o, &err);
        if (!e.stream)
        {
            e.error = err.empty() ? "can't open" : err;
            log::warn("video {}: {}", path, e.error);
        }
        else
            e.stream->setOnFrame(notifyVideoListeners);
    }
    return e.stream ? &e : nullptr;
}
} // namespace

const media::VideoInfo* info(const std::string& path)
{
    VideoEntry* e = videoEntry(path);
    return e ? &e->stream->info() : nullptr;
}

std::string error(const std::string& path)
{
    auto it = videoEntries().find(path);
    return it == videoEntries().end() ? std::string() : it->second.error;
}

render::TexturePtr texture(const std::string& path, int64_t index)
{
    LILA_ZONE();

    VideoEntry* e = videoEntry(path);
    if (!e)
        return nullptr;
    media::VideoFramePtr f = e->stream->get(index);
    if (!f)
        return e->texture;
    if (!e->gpu || e->gpu->width() != f->width || e->gpu->height() != f->height)
    {
        render::GpuImageDesc d;
        d.width = f->width;
        d.height = f->height;
        d.format = render::TextureFormat::RGBA8;
        d.srgb = true;
        d.name = "video " + paths::toUtf8(paths::fromUtf8(path).filename());
        e->gpu = render::GpuImage::create(d);
        e->texture = e->gpu->asTexture();
        e->uploaded = -1;
    }
    if (f->index != e->uploaded)
    {
        e->gpu->update(0, 0, f->width, f->height, f->rgba);
        e->uploaded = f->index;
    }
    return e->texture;
}

QImage image(const std::string& path, int64_t index)
{
    VideoEntry* e = videoEntry(path);
    if (!e)
        return {};
    media::VideoFramePtr f = e->stream->get(index);
    if (!f)
        return e->qimage;
    if (f->index != e->imaged)
    {
        e->qimage = QImage(f->rgba.data(), f->width, f->height, f->width * 4, QImage::Format_RGBA8888).copy();
        e->imaged = f->index;
    }
    return e->qimage;
}

int64_t referenceFrame(const std::string& path, double sceneSeconds, double fps, int64_t offset)
{
    const media::VideoInfo* i = info(path);
    if (!i || i->frameCount <= 0)
        return 0;
    const int64_t f = int64_t(std::floor(sceneSeconds * (fps > 0.0 ? fps : i->fps) + 1e-6)) + offset;
    return ((f % i->frameCount) + i->frameCount) % i->frameCount;
}

bool waitFor(const std::string& path, int64_t index, double timeoutSeconds)
{
    VideoEntry* e = videoEntry(path);
    return e && e->stream->wait(index, timeoutSeconds) != nullptr;
}

int addListener(std::function<void()> fn)
{
    static int next = 1;
    videoListeners().emplace(next, std::move(fn));
    return next++;
}

void removeListener(int id)
{
    videoListeners().erase(id);
}

void release(const std::string& path)
{
    videoEntries().erase(path);
}
} // namespace lila::ui::videoTextures
