#include "ui/MediaLane.h"

#include "core/MainQueue.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "media/Decode.h"
#include "media/Media.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/ImagePlane.h"
#include "scene/SceneData.h"
#include "scene/Take.h"
#include "ui/Actions.h"
#include "ui/Animation.h"
#include "ui/AudioUi.h"
#include "ui/FileDialog.h"
#include "ui/Menus.h"
#include "ui/ReferenceOverlay.h"
#include "ui/Theme.h"
#include "ui/UiCommands.h"
#include "ui/UiCommon.h"
#include "ui/VideoTextures.h"

#include <QApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>

namespace lila::ui
{
namespace
{
namespace anim = scene::anim;
using scene::Graph;
using scene::Node;
using scene::NodeId;

constexpr double kMargin = 10.0; // the ruler's
constexpr int kLaneRowHeight = 18;
constexpr int kMaxRows = 4;
constexpr double kEdgePx = 5.0;
constexpr int kThumbs = 48;

const char* const kReferenceFilter =
    "Video and Sound (*.mp4 *.mov *.mkv *.webm *.avi *.m4v *.wav *.mp3 *.ogg *.flac *.aac *.m4a *.aif *.aiff *.opus);;All Files (*)";

/** Small frames of a video, evenly spaced over its length, decoded once on a thread of their own. */
struct Thumbs
{
    std::mutex m;
    std::vector<std::pair<int64_t, QImage>> frames;
    std::atomic<bool> quit{false};
    std::thread worker;

    ~Thumbs()
    {
        quit = true;
        if (worker.joinable())
            worker.join();
    }
};

std::map<std::string, std::shared_ptr<Thumbs>>& thumbCache()
{
    static std::map<std::string, std::shared_ptr<Thumbs>> m;
    return m;
}

std::atomic<int>& thumbsLanded()
{
    static std::atomic<int> n{0};
    return n;
}

std::shared_ptr<Thumbs> thumbsOf(const std::string& path)
{
    auto& slot = thumbCache()[path];
    if (slot)
        return slot;
    slot = std::make_shared<Thumbs>();
    Thumbs* t = slot.get();
    t->worker = std::thread([t, path] {
        std::unique_ptr<media::VideoDecoder> d = media::VideoDecoder::open(paths::fromUtf8(path), 96);
        if (!d)
            return;
        const int64_t count = std::max<int64_t>(1, d->info().frameCount);
        const int n = int(std::min<int64_t>(kThumbs, count));
        for (int i = 0; i < n && !t->quit; ++i)
        {
            const int64_t index = n > 1 ? int64_t(std::llround(double(i) * double(count - 1) / double(n - 1))) : 0;
            media::VideoFramePtr f = d->frame(index);
            if (!f)
                continue;
            QImage img = QImage(f->rgba.data(), f->width, f->height, f->width * 4, QImage::Format_RGBA8888).copy();
            {
                std::lock_guard lk(t->m);
                t->frames.emplace_back(index, std::move(img));
            }
            ++thumbsLanded();
        }
    });
    return slot;
}

/** One bar: the media nodes sharing a file, the span they play over and what the bar can trim. */
struct Row
{
    std::string file;
    std::string label;
    std::vector<NodeId> nodes;
    std::vector<NodeId> sounds;
    NodeId plane = scene::kNoNode;
    double start = 0.0; // scene seconds
    double end = 0.0;
    std::vector<media::Voice> voices;
    const media::VideoInfo* video = nullptr;
    /** Scene seconds where the plane's video frame 0 shows. */
    double videoZero = 0.0;
    double videoFps = 0.0;
};

std::vector<Row> buildRows()
{
    Graph& g = Graph::current();
    std::map<std::string, Row> byFile;
    std::vector<std::string> order;
    auto rowFor = [&](const std::string& file) -> Row& {
        auto [it, added] = byFile.try_emplace(file);
        if (added)
        {
            it->second.file = file;
            it->second.label = paths::toUtf8(paths::fromUtf8(file).filename());
            order.push_back(file);
        }
        return it->second;
    };
    for (NodeId id : g.nodes())
    {
        const Node* n = g.node(id);
        if (!media::isMedia(*n) || !media::isLive(g, id))
            continue;
        if (media::isAudio(*n))
        {
            std::optional<media::Voice> v = media::voiceOf(g, id, false);
            if (!v)
                continue;
            const std::string file = paths::toUtf8(paths::resolvePath(n->as<std::string>(media::AudioNode::kFileName)));
            Row& r = rowFor(file);
            const double s = v->start, e = v->end();
            r.start = r.nodes.empty() ? s : std::min(r.start, s);
            r.end = r.nodes.empty() ? e : std::max(r.end, e);
            r.nodes.push_back(id);
            r.sounds.push_back(id);
            r.voices.push_back(std::move(*v));
            continue;
        }
        const std::string stored = n->as<std::string>(scene::ImagePlane::kImageName);
        if (!media::isVideoFile(stored))
            continue;
        const std::string file = paths::toUtf8(paths::resolvePath(stored));
        const media::VideoInfo* info = videoTextures::info(file);
        if (!info || info->frameCount <= 0)
            continue;
        const double fps = n->as<double>(scene::ImagePlane::kFps) > 0.0 ? n->as<double>(scene::ImagePlane::kFps) : info->fps;
        const double zero = media::startSeconds(g, id, n->as<double>(scene::ImagePlane::kStartFrame)) -
                            double(n->as<int64_t>(scene::ImagePlane::kFrameOffset)) / fps;
        Row& r = rowFor(file);
        const double s = zero, e = zero + double(info->frameCount) / fps;
        r.start = r.nodes.empty() ? s : std::min(r.start, s);
        r.end = r.nodes.empty() ? e : std::max(r.end, e);
        r.nodes.push_back(id);
        if (r.plane == scene::kNoNode)
        {
            r.plane = id;
            r.video = info;
            r.videoZero = zero;
            r.videoFps = fps;
        }
    }
    std::vector<Row> out;
    for (const std::string& f : order)
        out.push_back(std::move(byFile[f]));
    return out;
}

std::vector<std::string> laneNodeNames(const std::vector<NodeId>& ids)
{
    std::vector<std::string> out;
    Graph& g = Graph::current();
    for (NodeId id : ids)
        if (const Node* n = g.node(id))
            out.push_back(n->path());
    return out;
}

/** The file length of an audio node's sound in its frames, for trimming its tail when sourceEnd is 0 (to the end). */
double fileFrames(const media::Voice& v)
{
    return v.rate() > 0 ? anim::frameOf(double(v.frames()) / v.rate()) : 0.0;
}
} // namespace

struct MediaLane::Impl
{
    enum class Drag
    {
        None,
        Slide,
        TrimHead,
        TrimTail,
        Scrub,
    };

    std::vector<Row> rows;
    bool dirty = false;
    int graphListener = 0;
    int soundListener = 0;
    int videoListener = 0;
    int playbackListener = 0;
    int thumbsSeen = -1;
    Drag drag = Drag::None;
    int dragRow = -1;
    double pressX = 0.0;
    int delta = 0; // frames
    int hoverRow = -1;
};

MediaLane::MediaLane(QWidget* parent) : QWidget(parent), mImpl(std::make_unique<Impl>())
{
    setObjectName("mediaLane");
    setMouseTracking(true);
    setVisible(false);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    mImpl->graphListener = Graph::current().addListener([this](scene::Change c, NodeId id) {
        if (c == scene::Change::Selection || c == scene::Change::Renamed || c == scene::Change::Reparented)
            return;
        if (c == scene::Change::Value)
        {
            const Node* n = Graph::current().node(id);
            if (!n || (!media::isMedia(*n) && !anim::isTake(*n)))
                return;
        }
        invalidate();
    });
    mImpl->soundListener = media::addSoundListener([this] { invalidate(); });
    mImpl->videoListener = videoTextures::addListener([this] {
        if (thumbsLanded().load() != mImpl->thumbsSeen)
            update();
    });
    mImpl->playbackListener = anim::addPlaybackListener([this] { invalidate(); });
    onTimeChanged(this, [this] {
        if (isVisible())
            update();
    });
    invalidate();
}

MediaLane::~MediaLane()
{
    Graph::current().removeListener(mImpl->graphListener);
    media::removeSoundListener(mImpl->soundListener);
    videoTextures::removeListener(mImpl->videoListener);
    anim::removePlaybackListener(mImpl->playbackListener);
}

void MediaLane::invalidate()
{
    // One rebuild for a burst of changes (an import adds thousands of nodes), on the next event loop turn.
    if (std::exchange(mImpl->dirty, true))
        return;
    QTimer::singleShot(0, this, [this] {
        mImpl->dirty = false;
        mImpl->rows = buildRows();
        const int rows = std::min<int>(kMaxRows, int(mImpl->rows.size()));
        setFixedHeight(rows * kLaneRowHeight + 2);
        setVisible(rows > 0);
        update();
    });
}

namespace
{
double pxPerFrame(const QWidget* w)
{
    const anim::Playback& pb = anim::playback();
    const double span = std::max(1.0, std::round(anim::frameOf(pb.maxTime)) - std::round(anim::frameOf(pb.minTime)));
    return (w->width() - 2.0 * kMargin) / span;
}

double xOfSeconds(const QWidget* w, double seconds)
{
    return kMargin + (anim::frameOf(seconds) - std::round(anim::frameOf(anim::playback().minTime))) * pxPerFrame(w);
}

double secondsAtX(const QWidget* w, double x)
{
    return anim::secondsOf((x - kMargin) / pxPerFrame(w) + std::round(anim::frameOf(anim::playback().minTime)));
}

/** The video's thumbnails across the bar: each slot shows the nearest decoded frame. */
void drawThumbs(QPainter& p, const QRectF& bar, const Row& r, const QWidget* w, double shift)
{
    std::shared_ptr<Thumbs> thumbs = thumbsOf(r.file);
    std::vector<std::pair<int64_t, QImage>> frames;
    {
        std::lock_guard lk(thumbs->m);
        frames = thumbs->frames;
    }
    if (frames.empty() || frames[0].second.isNull())
        return;
    std::ranges::sort(frames, {}, &std::pair<int64_t, QImage>::first);
    const double h = bar.height() - 2.0;
    const double aspect = double(frames[0].second.width()) / std::max(1, frames[0].second.height());
    const double slot = std::max(8.0, h * aspect);
    p.save();
    p.setClipRect(bar.adjusted(1, 1, -1, -1));
    p.setOpacity(0.55);
    for (double x = bar.left() + 1.0; x < bar.right(); x += slot)
    {
        const double t = secondsAtX(w, x + slot * 0.5) - shift;
        const int64_t index = std::clamp<int64_t>(int64_t(std::floor((t - r.videoZero) * r.videoFps)), 0, r.video->frameCount - 1);
        auto it = std::ranges::min_element(frames, {}, [&](const auto& f) { return std::abs(f.first - index); });
        p.drawImage(QRectF(x, bar.top() + 1.0, slot, h), it->second);
    }
    p.restore();
}
} // namespace

void MediaLane::paintEvent(QPaintEvent*)
{
    LILA_ZONE();

    QPainter p(this);
    p.fillRect(rect(), gray(34));
    Impl& d = *mImpl;
    d.thumbsSeen = thumbsLanded().load();
    const double ppf = pxPerFrame(this);
    const QFontMetrics fm(font());
    for (int i = 0; i < int(d.rows.size()) && i < kMaxRows; ++i)
    {
        const Row& r = d.rows[size_t(i)];
        double shift = 0.0, headTrim = 0.0, tailTrim = 0.0;
        if (d.dragRow == i)
        {
            const double by = anim::secondsOf(d.delta);
            if (d.drag == Impl::Drag::Slide)
                shift = by;
            else if (d.drag == Impl::Drag::TrimHead)
                headTrim = by;
            else if (d.drag == Impl::Drag::TrimTail)
                tailTrim = by;
        }
        const double x0 = xOfSeconds(this, r.start + shift + headTrim), x1 = xOfSeconds(this, r.end + shift + tailTrim);
        const QRectF bar(x0, 1 + i * kLaneRowHeight, std::max(3.0, x1 - x0), kLaneRowHeight - 2);
        const bool hot = d.hoverRow == i || d.dragRow == i;
        p.fillRect(bar, gray(hot ? 70 : 56));
        if (r.video)
            drawThumbs(p, bar, r, this, shift);
        if (!r.voices.empty())
        {
            std::vector<media::Voice> shown = r.voices;
            for (media::Voice& v : shown)
            {
                v.start += shift;
                if (headTrim != 0.0)
                {
                    v.from = std::max(0.0, v.from + headTrim);
                    v.start = v.start + headTrim;
                }
                if (tailTrim != 0.0)
                    v.to = std::max(v.from, std::min(v.to, double(v.frames()) / std::max(1, v.rate())) + tailTrim);
            }
            audio::drawVoices(p, bar.adjusted(0, 2, 0, -2), secondsAtX(this, bar.left()), secondsAtX(this, bar.right()), gray(185, 200), shown);
        }
        p.setPen(gray(hot ? 150 : 100));
        p.setBrush(Qt::NoBrush);
        p.drawRect(bar.adjusted(0.5, 0.5, -0.5, -0.5));
        const QRectF text = bar.adjusted(4, 0, -4, 0).intersected(QRectF(kMargin, bar.top(), width() - 2 * kMargin, bar.height()));
        if (text.width() > 20)
        {
            const QString label = fm.elidedText(QString::fromStdString(r.label), Qt::ElideRight, int(text.width()));
            // A plate under the name keeps it readable over thumbnails and waveforms.
            p.fillRect(QRectF(text.left() - 2, bar.top() + 2, fm.horizontalAdvance(label) + 4, bar.height() - 4), gray(30, 190));
            p.setPen(gray(220));
            p.drawText(text, Qt::AlignVCenter | Qt::AlignLeft, label);
        }
        if (d.dragRow == i && d.drag != Impl::Drag::Scrub)
        {
            const QString readout = QString("%1%2 f").arg(d.delta > 0 ? "+" : "").arg(d.delta);
            const QRectF box(std::clamp(x0, kMargin, width() - 60.0), bar.top(), 56, bar.height());
            p.fillRect(box, gray(26, 220));
            p.setPen(gray(230));
            p.drawText(box, Qt::AlignCenter, readout);
        }
    }
    const double cx = xOfSeconds(this, Graph::current().time());
    p.fillRect(QRectF(cx - std::max(1.0, ppf * 0.5) / 2, 0, std::max(1.0, ppf * 0.5), height()), gray(230, 160));
}

namespace
{
int rowAt(const MediaLane* w, const std::vector<Row>& rows, QPointF at, double* x0 = nullptr, double* x1 = nullptr)
{
    const int i = int(at.y() - 1) / kLaneRowHeight;
    if (i < 0 || i >= int(rows.size()) || i >= kMaxRows)
        return -1;
    const double a = xOfSeconds(w, rows[size_t(i)].start), b = xOfSeconds(w, rows[size_t(i)].end);
    if (x0)
        *x0 = a;
    if (x1)
        *x1 = b;
    return at.x() >= a - kEdgePx && at.x() <= b + kEdgePx ? i : -1;
}

void scrubLane(const QWidget* w, double x)
{
    const anim::Playback& pb = anim::playback();
    const double t = anim::secondsOf(std::clamp(std::round(anim::frameOf(secondsAtX(w, x))), std::round(anim::frameOf(pb.minTime)),
                                                std::round(anim::frameOf(pb.maxTime))));
    if (std::abs(t - Graph::current().time()) < 1e-9)
        return;
    ops::execute("currentTime", {{"args", {t}}});
    notifyTimeChanged();
    audio::scrubbed(t);
}

json argsOf(const std::vector<NodeId>& ids)
{
    json a = json::array();
    for (const std::string& n : laneNodeNames(ids))
        a.push_back(n);
    return a;
}

/** Moves every node of the row by frames, as one undo step. */
void slideRow(const Row& r, int frames)
{
    if (frames != 0)
        runUi("slideMedia", {{"args", argsOf(r.nodes)}, {"frames", double(frames)}});
}

/** Trims the row's sounds by frames at the head (keeping the rest where it plays) or the tail. */
void trimRow(const Row& r, int frames, bool head)
{
    Graph& g = Graph::current();
    for (size_t k = 0; k < r.sounds.size(); ++k)
    {
        const Node* n = g.node(r.sounds[k]);
        if (!n)
            continue;
        const std::string name = n->path();
        const double ss = n->as<double>(media::AudioNode::kSourceStart);
        const double se = n->as<double>(media::AudioNode::kSourceEnd);
        const double length = k < r.voices.size() ? fileFrames(r.voices[k]) : 0.0;
        if (head)
        {
            const double next = std::clamp(ss + frames, 0.0, (se > 0.0 ? se : length) - 1.0);
            runUi("audio", {{"args", {name}}, {"sourceStart", next}, {"offset", n->as<double>(media::AudioNode::kOffset) + (next - ss)}});
        }
        else
        {
            const double end = std::clamp((se > 0.0 ? se : length) + frames, ss + 1.0, length);
            runUi("audio", {{"args", {name}}, {"sourceEnd", end >= length - 0.5 ? 0.0 : end}});
        }
    }
}

void showRowMenu(QWidget* parent, const Row& r, QPoint at)
{
    Graph& g = Graph::current();
    QMenu menu(parent);
    const json args = argsOf(r.nodes);
    const double now = Graph::current().time();
    menu.addAction("Start at Playhead", [r, now] { slideRow(r, int(std::lround(anim::frameOf(now) - anim::frameOf(r.start)))); });
    menu.addAction("End at Playhead", [r, now] { slideRow(r, int(std::lround(anim::frameOf(now) - anim::frameOf(r.end)))); });
    menu.addSeparator();

    QMenu* takes = menu.addMenu("Takes");
    const std::vector<NodeId> all = anim::takes(g);
    if (all.empty())
        takes->addAction("No takes")->setEnabled(false);
    for (NodeId t : all)
    {
        const std::string take = g.node(t)->path();
        const std::vector<NodeId> has = media::mediaOf(g, t);
        const bool bound = std::ranges::all_of(r.nodes, [&](NodeId m) { return std::ranges::find(has, m) != has.end(); });
        QAction* a = takes->addAction(QString::fromStdString(take), [take, args, bound] {
            json call = {{"args", json::array({take})}};
            for (const json& n : args)
                call["args"].push_back(n);
            if (bound)
                call["remove"] = true;
            runUi("bindMedia", call);
        });
        a->setCheckable(true);
        a->setChecked(bound);
    }

    if (!r.sounds.empty())
    {
        const Node* first = g.node(r.sounds[0]);
        const std::vector<media::AudioTrack> tracks = media::audioTracks(paths::fromUtf8(r.file));
        if (tracks.size() > 1)
        {
            QMenu* trackMenu = menu.addMenu("Sound Track");
            const int64_t current = first->as<int64_t>(media::AudioNode::kTrack);
            for (const media::AudioTrack& t : tracks)
            {
                QString label = QString("Track %1").arg(t.index + 1);
                if (!t.title.empty())
                    label += QString(": %1").arg(QString::fromStdString(t.title));
                else if (!t.language.empty())
                    label += QString(" (%1)").arg(QString::fromStdString(t.language));
                const std::vector<std::string> names = laneNodeNames(r.sounds);
                QAction* a = trackMenu->addAction(label, [names, index = t.index] {
                    for (const std::string& n : names)
                        runUi("audio", {{"args", {n}}, {"track", index}});
                });
                a->setCheckable(true);
                a->setChecked(current == t.index);
            }
        }
        const bool muted = first->as<bool>(media::AudioNode::kMute);
        QAction* mute = menu.addAction("Mute", [names = laneNodeNames(r.sounds), muted] {
            for (const std::string& n : names)
                runUi("audio", {{"args", {n}}, {"mute", !muted}});
        });
        mute->setCheckable(true);
        mute->setChecked(muted);
        const scene::data::json picked = scene::data::get(media::kSoundKey);
        const std::string alone = first->path();
        QAction* solo = menu.addAction("Hear Alone", [alone, on = picked.is_string() && picked.get<std::string>() == alone] {
            runUi("timeSliderSound", {{"sound", on ? std::string() : alone}});
        });
        solo->setCheckable(true);
        solo->setChecked(picked.is_string() && picked.get<std::string>() == alone);
    }
    menu.addSeparator();
    menu.addAction("Select", [args] { runUi("select", {{"args", args}}); });
    menu.addAction("Delete", [args] { runUi("delete", {{"args", args}}); });
    menu.exec(at);
}

void showLaneMenu(QWidget* parent, QPoint at)
{
    QMenu menu(parent);
    menus::addActionRow(&menu, "anim.importFootage");
    menus::addActionRow(&menu, "create.audio");
    menu.exec(at);
}
} // namespace

void MediaLane::mousePressEvent(QMouseEvent* e)
{
    Impl& d = *mImpl;
    double x0 = 0.0, x1 = 0.0;
    const int row = rowAt(this, d.rows, e->position(), &x0, &x1);
    if (e->button() == Qt::RightButton)
    {
        if (row >= 0)
            showRowMenu(this, d.rows[size_t(row)], e->globalPosition().toPoint());
        else
            showLaneMenu(this, e->globalPosition().toPoint());
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    d.pressX = e->position().x();
    d.delta = 0;
    if (row < 0)
    {
        d.drag = Impl::Drag::Scrub;
        scrubLane(this, d.pressX);
        return;
    }
    d.dragRow = row;
    const bool sound = !d.rows[size_t(row)].sounds.empty();
    if (sound && std::abs(d.pressX - x0) <= kEdgePx)
        d.drag = Impl::Drag::TrimHead;
    else if (sound && std::abs(d.pressX - x1) <= kEdgePx)
        d.drag = Impl::Drag::TrimTail;
    else
        d.drag = Impl::Drag::Slide;
    update();
}

void MediaLane::mouseMoveEvent(QMouseEvent* e)
{
    Impl& d = *mImpl;
    if (d.drag == Impl::Drag::Scrub)
    {
        scrubLane(this, e->position().x());
        return;
    }
    if (d.drag != Impl::Drag::None)
    {
        const int delta = int(std::lround((e->position().x() - d.pressX) / pxPerFrame(this)));
        if (delta != d.delta)
        {
            d.delta = delta;
            update();
        }
        return;
    }
    double x0 = 0.0, x1 = 0.0;
    const int row = rowAt(this, d.rows, e->position(), &x0, &x1);
    const bool sound = row >= 0 && !d.rows[size_t(row)].sounds.empty();
    const bool edge = sound && (std::abs(e->position().x() - x0) <= kEdgePx || std::abs(e->position().x() - x1) <= kEdgePx);
    setCursor(edge ? Qt::SizeHorCursor : row >= 0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
    if (row != d.hoverRow)
    {
        d.hoverRow = row;
        setToolTip(row >= 0 ? QString::fromStdString(d.rows[size_t(row)].file) +
                                  (sound ? "\nDrag to slide, drag an end to trim, right-click for takes and lining up"
                                         : "\nDrag to slide, right-click for takes and lining up")
                            : QString("Drag to scrub, right-click to import reference"));
        update();
    }
}

void MediaLane::mouseReleaseEvent(QMouseEvent*)
{
    Impl& d = *mImpl;
    const Impl::Drag drag = std::exchange(d.drag, Impl::Drag::None);
    const int row = std::exchange(d.dragRow, -1);
    const int delta = std::exchange(d.delta, 0);
    if (row < 0 || row >= int(d.rows.size()) || delta == 0)
    {
        update();
        return;
    }
    const Row r = d.rows[size_t(row)];
    if (drag == Impl::Drag::Slide)
        slideRow(r, delta);
    else if (drag == Impl::Drag::TrimHead || drag == Impl::Drag::TrimTail)
        trimRow(r, delta, drag == Impl::Drag::TrimHead);
    invalidate();
}

void MediaLane::mouseDoubleClickEvent(QMouseEvent* e)
{
    const int row = rowAt(this, mImpl->rows, e->position());
    if (row >= 0)
        runUi("select", {{"args", argsOf(mImpl->rows[size_t(row)].nodes)}});
}

void MediaLane::leaveEvent(QEvent*)
{
    if (mImpl->hoverRow >= 0)
    {
        mImpl->hoverRow = -1;
        update();
    }
}

namespace mediaLane
{
std::vector<std::pair<std::string, std::vector<std::string>>> rows()
{
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    for (const Row& r : buildRows())
        out.emplace_back(r.file, laneNodeNames(r.nodes));
    return out;
}

void importFootage(QWidget* parent)
{
    QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
    const fileDialog::ContextScope scope("referenceVideo", "Reference Video");
    const QString file = fileDialog::getOpenFileName(parent, "Import Footage", start, kReferenceFilter);
    if (file.isEmpty())
        return;
    runUi("importFootage", {{"args", {file.toStdString()}}, {"camera", referenceOverlays::activeViewName()}});
}

void install()
{
    actions::add({.id = "anim.importFootage",
                  .label = "Import Footage...",
                  .category = "Animation",
                  .run = [] { importFootage(QApplication::activeWindow()); },
                  .doc = "Imports reference video and its sound for the shot: an image plane on the active view and an audio "
                         "node, both on the active take, starting on its first frame. Line them up in the time slider's media lane.",
                  .python = "importFootage('ref/shot010.mov')",
                  .icon = "movie",
                  .keywords = {"video", "footage", "reference video", "rotoscope", "audio", "sound", "import video", "camera sequencer"}});
}
} // namespace mediaLane
} // namespace lila::ui
