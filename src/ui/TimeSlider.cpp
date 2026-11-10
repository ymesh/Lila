#include "ui/Theme.h"
#include "ui/TimeSlider.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/Animation.h"
#include "ui/AudioUi.h"
#include "ui/Controls.h"
#include "ui/Deletion.h"
#include "ui/Ghosting.h"
#include "ui/Icons.h"
#include "ui/Menus.h"
#include "ui/SpinScrub.h"
#include "ui/UiCommands.h"
#include "ui/UiCommon.h"
#include "ui/ViewportDisplay.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace lila::ui
{
namespace
{
namespace anim = scene::anim;
using scene::Graph;

int frameOf(double seconds) { return int(std::lround(anim::frameOf(seconds))); }

/** A step that keeps labelled ticks at least minPx apart: 1, 2, 5, 10, 20, 50... frames. */
int frameTickStep(double pxPerFrame, double minPx)
{
    for (int base = 1;; base *= 10)
        for (int m : {1, 2, 5})
            if (base * m * pxPerFrame >= minPx || base > 1000000)
                return base * m;
}

enum class TransportGlyph
{
    ToStart,
    StepBack,
    PrevKey,
    PlayBack,
    Play,
    Pause,
    StepForward,
    NextKey,
    ToEnd,
};

QIcon glyphIcon(TransportGlyph g)
{
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(gray(205));
    auto tri = [&](double x0, double x1) {
        QPainterPath path;
        path.moveTo(x0, 8);
        path.lineTo(x0, 24);
        path.lineTo(x1, 16);
        path.closeSubpath();
        p.drawPath(path);
    };
    auto bar = [&](double x) { p.drawRect(QRectF(x, 8, 3, 16)); };
    auto key = [&](double cx) { p.drawPolygon(QPolygonF({{cx, 10}, {cx + 6, 16}, {cx, 22}, {cx - 6, 16}})); };
    switch (g)
    {
    case TransportGlyph::ToStart: bar(7); tri(24, 11); break;
    case TransportGlyph::StepBack: tri(20, 7); bar(22); break;
    case TransportGlyph::PlayBack: tri(23, 9); break;
    case TransportGlyph::Play: tri(9, 23); break;
    case TransportGlyph::Pause: bar(10); bar(19); break;
    case TransportGlyph::StepForward: bar(7); tri(12, 25); break;
    case TransportGlyph::PrevKey: key(10); tri(27, 17); break;
    case TransportGlyph::NextKey: tri(5, 15); key(22); break;
    case TransportGlyph::ToEnd: tri(8, 21); bar(22); break;
    }
    return QIcon(pm);
}

QLabel* timeDimLabel(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    QPalette pal = l->palette();
    pal.setColor(QPalette::WindowText, themeColor("dimText"));
    l->setPalette(pal);
    return l;
}

QString actionTip(const char* id)
{
    return QString::fromStdString(actions::tooltip(id));
}

QToolButton* transportButton(QWidget* parent, const QString& tip)
{
    auto* b = new QToolButton(parent);
    b->setAutoRaise(true);
    b->setToolTip(tip);
    b->setIconSize(QSize(14, 14));
    b->setFixedSize(22, 22);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

QSpinBox* frameField(QWidget* parent, const QString& tip)
{
    auto* s = new QSpinBox(parent);
    s->setRange(-1000000, 1000000);
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
    s->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    s->setFixedWidth(52);
    s->setToolTip(tip);
    s->setKeyboardTracking(false);
    return s;
}
} // namespace

/** Frame ruler over the playback range: scrub with LMB, key ticks for the selection, right-click for keys. */
class TimeRuler : public QWidget
{
public:
    explicit TimeRuler(QWidget* parent) : QWidget(parent)
    {
        setMinimumHeight(28);
        setMouseTracking(false);
        setFocusPolicy(Qt::ClickFocus);
        mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
            if (file == "general")
                update();
        });
    }
    ~TimeRuler() override { PrefStore::get().removeOnChanged(mPrefListener); }

    void invalidateKeys()
    {
        mKeysValid = false;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        LILA_ZONE();

        QPainter p(this);
        p.fillRect(rect(), gray(40));
        const anim::Playback& pb = anim::playback();
        int first = frameOf(pb.minTime), last = frameOf(pb.maxTime);
        double ppf = pxPerFrame();
        int h = height();
        audio::drawWaveform(p, QRectF(xOf(first - 0.5), 1, xOf(last + 0.5) - xOf(first - 0.5), h - 2), anim::secondsOf(first - 0.5),
                            anim::secondsOf(last + 0.5), QColor(90, 160, 120, 150));

        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 0.9);
        p.setFont(f);
        int labelStep = frameTickStep(ppf, 46);
        int tickStep = std::max(1, frameTickStep(ppf, 6));
        for (int fr = first - (first % tickStep); fr <= last; fr += tickStep)
        {
            if (fr < first)
                continue;
            double x = xOf(fr);
            bool major = fr % labelStep == 0;
            p.setPen(gray(major ? 120 : 78));
            p.drawLine(QPointF(x, h - (major ? 10 : 5)), QPointF(x, h));
            QString label = QString::number(fr);
            if (major && x + 3 + p.fontMetrics().horizontalAdvance(label) < width())
            {
                p.setPen(gray(140));
                p.drawText(QPointF(x + 3, h - 12), label);
            }
        }

        if (!mKeysValid)
        {
            Graph& g = Graph::current();
            mKeys = anim::keyTimes(g, g.selection());
            mLoopKeys = anim::loopKeyTimes(g, g.selection());
            mKeysValid = true;
        }
        double tickW = std::max(2.0, ppf * 0.35);
        QColor tick = gray(175);
        if (viewportDisplay::accentColors(viewportDisplay::kTimeKeyColors))
        {
            glm::vec3 k = viewportDisplay::keyTickColor();
            tick = QColor::fromRgbF(k.r, k.g, k.b);
        }
        for (double t : mKeys)
        {
            double fr = anim::frameOf(t);
            if (fr < first - 0.5 || fr > last + 0.5)
                continue;
            p.fillRect(QRectF(xOf(fr) - tickW / 2, 2, tickW, h - 4), tick);
        }
        // Loop keys: the tick with caps top and bottom, bracketing the cycle.
        for (double t : mLoopKeys)
        {
            double fr = anim::frameOf(t);
            if (fr < first - 0.5 || fr > last + 0.5)
                continue;
            double capW = tickW * 3.0;
            p.fillRect(QRectF(xOf(fr) - capW, 1, capW, 2), tick);
            p.fillRect(QRectF(xOf(fr) - capW, h - 3, capW, 2), tick);
        }

        mDrewCache = false;
        if (Graph::current().cachedPlayback())
        {
            const QColor cached = viewportDisplay::accentColors(viewportDisplay::kTimeKeyColors) ? QColor(70, 130, 200) : gray(110);
            for (double t : Graph::current().playbackCacheTimes())
            {
                double fr = anim::frameOf(t);
                if (fr >= first - 0.5 && fr <= last + 0.5)
                {
                    p.fillRect(QRectF(xOf(fr) - ppf / 2, h - 3, ppf, 3), cached);
                    mDrewCache = true;
                }
            }
        }

        drawSelection(p, first, last);

        int cur = frameOf(Graph::current().time());
        double cx = xOf(cur);
        double cw = std::max(3.0, ppf);
        p.fillRect(QRectF(cx - cw / 2, 0, cw, h), gray(26, 220));
        QString label = QString::number(cur);
        QRect tr = p.fontMetrics().boundingRect(label).adjusted(-4, -1, 4, 1);
        tr.moveTopLeft(QPoint(int(cx + cw / 2 + 2), 2));
        if (tr.right() > width())
            tr.moveRight(int(cx - cw / 2 - 2));
        p.fillRect(tr, gray(26));
        p.setPen(gray(225));
        p.drawText(tr, Qt::AlignCenter, label);
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        // Shift + drag selects a range for edits across frames; a plain click drops it.
        if (e->button() == Qt::LeftButton && e->modifiers().testFlag(Qt::ShiftModifier))
        {
            mSelecting = true;
            mSelectFrom = frameAt(e->position().x());
            selectTo(e->position().x());
            return;
        }
        if (e->button() == Qt::LeftButton && anim::timeSelection().active)
        {
            anim::timeSelection().active = false;
            anim::playbackChanged();
        }
        // MMB moves the time but holds the pose, so a key set next copies it there (Maya's MMB drag).
        if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton)
        {
            mScrubbing = true;
            mHoldPose = e->button() == Qt::MiddleButton;
            scrubTo(e->position().x());
        }
        else if (e->button() == Qt::RightButton)
            showMenu(e->globalPosition().toPoint());
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (mSelecting)
            selectTo(e->position().x());
        else if (mScrubbing)
            scrubTo(e->position().x());
    }

    void mouseReleaseEvent(QMouseEvent*) override
    {
        mScrubbing = false;
        mSelecting = false;
    }

private:
    double pxPerFrame() const
    {
        const anim::Playback& pb = anim::playback();
        int span = std::max(1, frameOf(pb.maxTime) - frameOf(pb.minTime));
        return (width() - 2.0 * kMargin) / span;
    }
    double xOf(double frame) const { return kMargin + (frame - frameOf(anim::playback().minTime)) * pxPerFrame(); }

    int frameAt(double x) const
    {
        const anim::Playback& pb = anim::playback();
        const int fr = int(std::lround((x - kMargin) / pxPerFrame())) + frameOf(pb.minTime);
        return std::clamp(fr, frameOf(pb.minTime), frameOf(pb.maxTime));
    }

    void selectTo(double x)
    {
        const int to = frameAt(x);
        anim::TimeSelection& sel = anim::timeSelection();
        sel.active = true;
        sel.start = anim::secondsOf(std::min(mSelectFrom, to));
        sel.end = anim::secondsOf(std::max(mSelectFrom, to));
        anim::playbackChanged();
        update();
    }

    /** The selected range as a light band over its frames, fading out across the falloff on each side. */
    void drawSelection(QPainter& p, int first, int last)
    {
        const anim::TimeSelection& sel = anim::timeSelection();
        if (!sel.active)
            return;
        const double from = anim::frameOf(sel.start), to = anim::frameOf(sel.end);
        const int h = height();
        const double ppf = pxPerFrame();
        for (int fr = int(std::floor(from - sel.falloff)); fr <= int(std::ceil(to + sel.falloff)); ++fr)
        {
            if (fr < first || fr > last)
                continue;
            const double w = anim::timeSelectionWeight(sel, anim::secondsOf(fr));
            if (w <= 0.0)
                continue;
            p.fillRect(QRectF(xOf(fr) - ppf / 2, 0, ppf, h), gray(230, int(70 * w)));
        }
        p.setPen(gray(230, 160));
        p.drawLine(QPointF(xOf(from) - ppf / 2, 0), QPointF(xOf(from) - ppf / 2, h));
        p.drawLine(QPointF(xOf(to) + ppf / 2, 0), QPointF(xOf(to) + ppf / 2, h));
    }

    void scrubTo(double x)
    {
        const anim::Playback& pb = anim::playback();
        int fr = int(std::lround((x - kMargin) / pxPerFrame())) + frameOf(pb.minTime);
        fr = std::clamp(fr, frameOf(pb.minTime), frameOf(pb.maxTime));
        double t = anim::secondsOf(fr);
        if (std::abs(t - Graph::current().time()) < 1e-9)
            return;
        ops::execute("currentTime", {{"args", {t}}, {"update", !mHoldPose}});
        notifyTimeChanged();
        audio::scrubbed(t);
    }

    void showMenu(QPoint at)
    {
        double now = Graph::current().time();
        QMenu menu(this);
        menu.addAction("Set Key", [] { actions::run(json("anim.setKey")); });
        menu.addAction("Delete Key", [now] { runUi("cutKey", {{"time", now}, {"clear", true}}); });
        menu.addSeparator();
        menu.addAction("Copy", [now] { runUi("copyKey", {{"time", now}}); });
        menu.addAction("Cut", [now] { runUi("cutKey", {{"time", now}}); });
        menu.addAction("Paste", [] { runUi("pasteKey"); });
        menu.addSeparator();
        if (anim::timeSelection().active)
        {
            QMenu* falloff = menu.addMenu("Range Falloff");
            for (int frames : {0, 2, 5, 10, 20})
            {
                QAction* item = falloff->addAction(frames ? QString("%1 frames").arg(frames) : QString("None"),
                                                   [frames] { runUi("timeRange", {{"falloff", double(frames)}}); });
                item->setCheckable(true);
                item->setChecked(std::abs(anim::timeSelection().falloff - frames) < 1e-9);
            }
            menu.addAction("Clear Range", [] { runUi("timeRange", {{"clear", true}}); });
            menu.addSeparator();
            if (!menus::itemsIn(menus::kTimeSliderRange).empty())
            {
                for (const menus::Item* item : menus::itemsIn(menus::kTimeSliderRange))
                    if (item->kind == menus::Kind::Separator)
                        menu.addSeparator();
                    else if (item->kind == menus::Kind::Action)
                        menus::addActionRow(&menu, item->action);
                menu.addSeparator();
            }
        }
        audio::fillMenu(menu.addMenu("Audio"));
        QMenu* speed = menu.addMenu("Playback Speed");
        for (anim::PlaybackSpeed s : anim::kPlaybackSpeeds)
        {
            std::string id = "anim.speed." + std::string(anim::speedName(s));
            QString label = s == anim::PlaybackSpeed::Custom ? QString("Custom (%1x)...").arg(anim::playback().customSpeed)
                                                              : QString::fromUtf8(anim::speedLabel(s).data());
            QAction* item = speed->addAction(label, [id] { actions::run(json(id)); });
            item->setCheckable(true);
            item->setChecked(anim::playback().speed == s);
        }
        menu.exec(at);
    }

    static constexpr double kMargin = 10.0;
    std::vector<double> mKeys;
    std::vector<double> mLoopKeys;
    bool mKeysValid = false;
    bool mScrubbing = false;
    bool mHoldPose = false;
    bool mSelecting = false;
    int mSelectFrom = 0;
    int mPrefListener = 0;

public:
    /** The last paint showed cached frames, which any edit clears. */
    bool mDrewCache = false;
};

/** Outer animation range with the playback range as a draggable bar inside it. */
class RangeBar : public QWidget
{
public:
    explicit RangeBar(QWidget* parent) : QWidget(parent)
    {
        setMinimumHeight(18);
        setMinimumWidth(80);
        setCursor(Qt::SizeHorCursor);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF track = QRectF(rect()).adjusted(1, 3, -1, -3);
        p.setPen(Qt::NoPen);
        p.setBrush(gray(34));
        p.drawRoundedRect(track, 3, 3);
        QRectF bar = barRect();
        p.setBrush(gray(mDrag == Drag::None ? 72 : 88));
        p.drawRoundedRect(bar, 3, 3);
        p.setBrush(gray(124));
        p.drawRect(QRectF(bar.left(), bar.top(), 3, bar.height()));
        p.drawRect(QRectF(bar.right() - 3, bar.top(), 3, bar.height()));
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        QRectF bar = barRect();
        double x = e->position().x();
        const anim::Playback& pb = anim::playback();
        if (std::abs(x - bar.left()) < 6)
            mDrag = Drag::Start;
        else if (std::abs(x - bar.right()) < 6)
            mDrag = Drag::End;
        else if (x > bar.left() && x < bar.right())
            mDrag = Drag::Move;
        else
            return;
        mPressFrame = frameAt(x);
        mStartMin = frameOf(pb.minTime);
        mStartMax = frameOf(pb.maxTime);
        update();
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (mDrag == Drag::None)
            return;
        const anim::Playback& pb = anim::playback();
        int lo = frameOf(pb.animStart), hi = frameOf(pb.animEnd);
        int d = frameAt(e->position().x()) - mPressFrame;
        int mn = mStartMin, mx = mStartMax;
        if (mDrag == Drag::Move)
        {
            d = std::clamp(d, lo - mStartMin, hi - mStartMax);
            mn += d;
            mx += d;
        }
        else if (mDrag == Drag::Start)
            mn = std::clamp(mStartMin + d, lo, mStartMax);
        else
            mx = std::clamp(mStartMax + d, mStartMin, hi);
        if (mn != frameOf(pb.minTime) || mx != frameOf(pb.maxTime))
            ops::execute("playbackOptions", {{"minTime", anim::secondsOf(mn)}, {"maxTime", anim::secondsOf(mx)}});
    }

    void mouseReleaseEvent(QMouseEvent*) override
    {
        mDrag = Drag::None;
        update();
    }

private:
    enum class Drag
    {
        None,
        Start,
        End,
        Move,
    };

    double pxPerFrame() const
    {
        const anim::Playback& pb = anim::playback();
        int span = std::max(1, frameOf(pb.animEnd) - frameOf(pb.animStart));
        return (width() - 2.0) / span;
    }
    int frameAt(double x) const { return int(std::lround((x - 1.0) / pxPerFrame())) + frameOf(anim::playback().animStart); }
    QRectF barRect() const
    {
        const anim::Playback& pb = anim::playback();
        double x0 = 1.0 + (frameOf(pb.minTime) - frameOf(pb.animStart)) * pxPerFrame();
        double x1 = 1.0 + (frameOf(pb.maxTime) - frameOf(pb.animStart)) * pxPerFrame();
        return QRectF(x0, 3, std::max(8.0, x1 - x0), height() - 6);
    }

    Drag mDrag = Drag::None;
    int mPressFrame = 0;
    int mStartMin = 0;
    int mStartMax = 0;
};

TimeSlider::TimeSlider(QWidget* parent) : QWidget(parent)
{
    setProperty("hotkeyContext", "timeSlider");
    // The selection's keys at the current frame, as the ruler's Delete Key does.
    deletion::claim(this, [] { runUi("cutKey", {{"time", Graph::current().time()}, {"clear", true}}); });
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 2, 4, 2);
    outer->setSpacing(2);

    auto* top = new QHBoxLayout();
    top->setSpacing(2);
    mRuler = new TimeRuler(this);
    top->addWidget(mRuler, 1);
    top->addSpacing(4);
    top->addWidget(timeDimLabel("Frame", this));
    mFrame = frameField(this, "Current Frame");
    top->addWidget(mFrame);
    top->addSpacing(6);
    // The playback buttons as one strip, so they read as a single control.
    auto* transport = new QWidget(this);
    transport->setAttribute(Qt::WA_StyledBackground);
    controls::setRole(transport, "segmented");
    auto* strip = new QHBoxLayout(transport);
    strip->setContentsMargins(2, 2, 2, 2);
    strip->setSpacing(0);
    auto addTransport = [&](QToolButton* b) {
        b->setParent(transport);
        strip->addWidget(b);
        return b;
    };
    auto addButton = [&](TransportGlyph g, const char* action) {
        QToolButton* b = addTransport(transportButton(this, actionTip(action)));
        b->setIcon(glyphIcon(g));
        connect(b, &QToolButton::clicked, this, [action] { actions::run(json(action)); });
        return b;
    };
    addButton(TransportGlyph::ToStart, "anim.firstFrame");
    addButton(TransportGlyph::PrevKey, "anim.prevKey");
    addButton(TransportGlyph::StepBack, "anim.prevFrame");
    mPlayBack = addTransport(transportButton(this, actionTip("anim.playBackward") + "\nClick again to stop"));
    connect(mPlayBack, &QToolButton::clicked, this, [] {
        if (anim::playDirection() < 0)
            actions::run(json("anim.stop"));
        else
            actions::run(json("anim.playBackward"));
    });
    mPlay = addTransport(transportButton(this, actionTip("anim.play") + "\nClick again to stop"));
    connect(mPlay, &QToolButton::clicked, this, [] {
        if (anim::playDirection() > 0)
            actions::run(json("anim.stop"));
        else
            runUi("play");
    });
    addButton(TransportGlyph::StepForward, "anim.nextFrame");
    addButton(TransportGlyph::NextKey, "anim.nextKey");
    addButton(TransportGlyph::ToEnd, "anim.lastFrame");
    top->addWidget(transport);
    outer->addLayout(top);

    auto* bottom = new QHBoxLayout();
    bottom->setSpacing(2);
    mAnimStart = frameField(this, "Animation Start\nThe first frame of the scene's animation");
    mMin = frameField(this, "Playback Start\nThe first frame playback and the ruler show");
    mRange = new RangeBar(this);
    mRange->setToolTip("Playback Range\nDrag the bar to slide it, its ends to resize it");
    mMax = frameField(this, "Playback End\nThe last frame playback and the ruler show");
    mAnimEnd = frameField(this, "Animation End\nThe last frame of the scene's animation");
    bottom->addWidget(timeDimLabel("Start", this));
    bottom->addWidget(mAnimStart);
    bottom->addWidget(timeDimLabel("From", this));
    bottom->addWidget(mMin);
    bottom->addWidget(mRange, 1);
    bottom->addWidget(timeDimLabel("To", this));
    bottom->addWidget(mMax);
    bottom->addWidget(timeDimLabel("End", this));
    bottom->addWidget(mAnimEnd);
    bottom->addSpacing(6);

    mFps = new QComboBox(this);
    for (double fps : {15.0, 24.0, 25.0, 30.0, 48.0, 50.0, 60.0, 120.0})
        mFps->addItem(QString("%1 fps").arg(fps), fps);
    mFps->setToolTip("Frame Rate");
    setComboDefault(mFps, mFps->findData(anim::Playback{}.fps));
    mFps->setFocusPolicy(Qt::NoFocus);
    bottom->addWidget(mFps);
    bottom->addSpacing(6);
    mLoop = transportButton(this, "Loop Playback\nPlayback starts over at the end of the range");
    mLoop->setIcon(icon("repeat"));
    mLoop->setCheckable(true);
    bottom->addWidget(mLoop);
    mAutoKey = transportButton(this, actionTip("anim.autoKeyToggle"));
    mAutoKey->setIcon(icon("key"));
    mAutoKey->setCheckable(true);
    bottom->addWidget(mAutoKey);
    {
        QToolButton* ghost = transportButton(this, QString::fromStdString(actions::tooltip("anim.toggleGhosts")) + "\nRight-click for ghost options");
        ghost->setIcon(icon("ghost"));
        ghost->setCheckable(true);
        ghost->setChecked(ghostActionState("anim.toggleGhosts").value_or(false));
        ghost->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(ghost, &QToolButton::clicked, this, [] { actions::run(json("anim.toggleGhosts")); });
        connect(ghost, &QToolButton::customContextMenuRequested, this, [ghost](const QPoint& at) {
            QMenu menu;
            for (const char* id : {"anim.ghostMode.keys", "anim.ghostMode.frames", "", "anim.ghostShow.mesh", "anim.ghostShow.skeleton",
                                   "anim.ghostShow.both", "", "anim.refreshGhosts"})
            {
                if (!*id)
                {
                    menu.addSeparator();
                    continue;
                }
                QAction* qa = menu.addAction(QString::fromStdString(actions::displayLabel(id)), [id] { actions::run(json(id)); });
                if (std::optional<bool> on = ghostActionState(id))
                {
                    qa->setCheckable(true);
                    qa->setChecked(*on);
                }
            }
            menu.exec(ghost->mapToGlobal(at));
        });
        int listener = PrefStore::get().onChanged([ghost](const std::string& file) {
            if (file != "general")
                return;
            QMetaObject::invokeMethod(ghost, [ghost] {
                QSignalBlocker block(ghost);
                ghost->setChecked(ghostActionState("anim.toggleGhosts").value_or(false));
            });
        });
        connect(ghost, &QObject::destroyed, [listener] { PrefStore::get().removeOnChanged(listener); });
        bottom->addWidget(ghost);
    }
    {
        QToolButton* cache = transportButton(this, "Cached Playback\nKeep evaluated frames in memory so playback replays them\n"
                                              "Right-click to fill or clear the cache");
        cache->setIcon(icon("database"));
        cache->setCheckable(true);
        const bool on = PrefStore::get().value<bool>("general", scene::kCachedPlaybackPref, false);
        Graph::current().setCachedPlayback(on);
        cache->setChecked(on);
        cache->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(cache, &QToolButton::toggled, this, [](bool on) { runUi("cachedPlayback", {{"state", on}}); });
        connect(cache, &QToolButton::customContextMenuRequested, this, [this, cache](const QPoint& at) {
            QMenu menu;
            menu.addAction("Fill Cache", [this] {
                runUi("cachedPlayback", {{"fill", true}});
                mRuler->update();
            });
            menu.addAction("Clear Cache", [this] {
                runUi("cachedPlayback", {{"clear", true}});
                mRuler->update();
            });
            menu.exec(cache->mapToGlobal(at));
        });
        int listener = PrefStore::get().onChanged([cache](const std::string& file) {
            if (file != "general")
                return;
            QMetaObject::invokeMethod(cache, [cache] {
                QSignalBlocker block(cache);
                cache->setChecked(Graph::current().cachedPlayback());
            });
        });
        connect(cache, &QObject::destroyed, [listener] { PrefStore::get().removeOnChanged(listener); });
        bottom->addWidget(cache);
    }
    outer->addLayout(bottom);

    connect(mFrame, &QSpinBox::valueChanged, this, [](int f) {
        ops::execute("currentTime", {{"args", {anim::secondsOf(f)}}});
        notifyTimeChanged();
    });
    auto rangeField = [this](QSpinBox* s, const char* flag) {
        connect(s, &QSpinBox::valueChanged, this, [flag](int f) { runUi("playbackOptions", {{flag, anim::secondsOf(f)}}); });
    };
    rangeField(mAnimStart, "animationStartTime");
    rangeField(mMin, "minTime");
    rangeField(mMax, "maxTime");
    rangeField(mAnimEnd, "animationEndTime");
    connect(mFps, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (i >= 0)
            runUi("playbackOptions", {{"fps", mFps->itemData(i).toDouble()}});
    });
    connect(mLoop, &QToolButton::toggled, this, [](bool on) { runUi("playbackOptions", {{"loop", on}}); });
    connect(mAutoKey, &QToolButton::toggled, this, [](bool on) {
        if (on != anim::autoKey())
            runUi("autoKeyframe", {{"state", on}});
    });

    mGraphListener = Graph::current().addListener([this](scene::Change c, scene::NodeId id) {
        if (c == scene::Change::Renamed || c == scene::Change::Reparented)
            return;
        // Ticks come from the selection's curves (non-DAG nodes): a DAG node's values never move them.
        const Graph& g = Graph::current();
        if (c == scene::Change::Value)
            if (const scene::Node* n = g.node(id); n && n->type().dag)
            {
                if (mRuler->mDrewCache)
                    mRuler->update();
                return;
            }
        mRuler->invalidateKeys();
    });
    mPlaybackListener = anim::addPlaybackListener([this] { refreshOptions(); });
    onTimeChanged(this, [this] { refreshTime(); });
    refreshOptions();
}

TimeSlider::~TimeSlider()
{
    Graph::current().removeListener(mGraphListener);
    anim::removePlaybackListener(mPlaybackListener);
}

QSize TimeSlider::sizeHint() const
{
    return {800, 56};
}

void TimeSlider::refreshOptions()
{
    const anim::Playback& pb = anim::playback();
    auto set = [](QSpinBox* s, double seconds) {
        QSignalBlocker block(s);
        s->setValue(frameOf(seconds));
    };
    set(mAnimStart, pb.animStart);
    set(mMin, pb.minTime);
    set(mMax, pb.maxTime);
    set(mAnimEnd, pb.animEnd);
    {
        QSignalBlocker block(mFps);
        int i = mFps->findData(pb.fps);
        if (i < 0)
        {
            mFps->addItem(QString("%1 fps").arg(pb.fps), pb.fps);
            i = mFps->count() - 1;
        }
        mFps->setCurrentIndex(i);
    }
    {
        QSignalBlocker block(mLoop);
        mLoop->setChecked(pb.loop);
    }
    {
        QSignalBlocker block(mAutoKey);
        mAutoKey->setChecked(anim::autoKey());
    }
    int dir = anim::playDirection();
    mPlay->setIcon(glyphIcon(dir > 0 ? TransportGlyph::Pause : TransportGlyph::Play));
    mPlayBack->setIcon(glyphIcon(dir < 0 ? TransportGlyph::Pause : TransportGlyph::PlayBack));
    mRuler->update();
    mRange->update();
    refreshTime();
}

void TimeSlider::refreshTime()
{
    if (!mFrame->hasFocus())
    {
        QSignalBlocker block(mFrame);
        mFrame->setValue(frameOf(Graph::current().time()));
    }
    mRuler->update();
}
} // namespace lila::ui
