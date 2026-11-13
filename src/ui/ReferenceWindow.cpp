#include "ui/ReferenceWindow.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Workspace.h"
#include "media/Decode.h"
#include "scene/Graph.h"
#include "ops/Command.h"
#include "ui/Actions.h"
#include "ui/FileDialog.h"
#include "ui/Icons.h"
#include "ui/PaintColor.h"
#include "ui/Panels.h"
#include "ui/ReferenceOverlay.h"
#include "ui/ReferenceSet.h"
#include "ui/Animation.h"
#include "ui/TextureCache.h"
#include "ui/VideoTextures.h"

#include <QActionGroup>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDockWidget>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QMainWindow>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>
#include <map>
#include <optional>
#include <set>

namespace lila::ui::references
{
namespace
{
const char* const kImageFilter =
    "Images and Videos (*.png *.jpg *.jpeg *.tga *.hdr *.exr *.bmp *.tif *.tiff *.webp *.mp4 *.mov *.mkv *.webm *.avi *.m4v);;All Files (*)";
constexpr const char* kPanelPrefix = "referenceImages";

json runCommand(const char* name, const json& call)
{
    try
    {
        return ops::execute(name, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return nullptr;
    }
}

QImage loadImage(const Image& ref, const std::string& file)
{
    if (media::isVideoFile(file))
        return videoTextures::image(file, videoFrame(ref, scene::Graph::current().time()));
    QImageReader reader(QString::fromStdString(file));
    reader.setAutoTransform(true);
    QImage img = reader.read();
    // Formats Qt can't read (EXR, HDR) come through the texture loader's thumbnailer.
    if (img.isNull())
        img = textures::thumbnail(file, 4096);
    return img;
}

struct Cached
{
    std::string file;
    QImage raw;
    std::string key;
    QImage shown;
};

std::map<std::string, Cached>& cache()
{
    static std::map<std::string, Cached> c;
    return c;
}

/** Grading on display-encoded 8-bit values, as the viewport's image planes do it. */
void grade(QImage& img, bool gray, double brightness, double contrast)
{
    uint8_t lut[256];
    for (int i = 0; i < 256; ++i)
        lut[i] = uint8_t(std::lround(std::clamp((i / 255.0 - 0.5) * contrast + 0.5 + brightness, 0.0, 1.0) * 255.0));
    for (int y = 0; y < img.height(); ++y)
    {
        auto* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x)
        {
            QRgb c = row[x];
            int r = qRed(c), g = qGreen(c), b = qBlue(c);
            if (gray)
                r = g = b = std::clamp(int(std::lround(0.2126 * r + 0.7152 * g + 0.0722 * b)), 0, 255);
            row[x] = qRgba(lut[r], lut[g], lut[b], qAlpha(c));
        }
    }
}

/** The dock showing a set window's panel, or null. */
QDockWidget* dockFor(const std::string& windowId)
{
    const QString name = QString::fromStdString(windows::panelId(windowId));
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* mw = qobject_cast<QMainWindow*>(w))
            if (auto* d = mw->findChild<QDockWidget*>(name))
                return d;
    return nullptr;
}

/** Where the next window torn off should appear, by window id, in global pixels. */
std::map<std::string, QPoint>& pendingPositions()
{
    static std::map<std::string, QPoint> m;
    return m;
}

std::vector<std::string> droppedFiles(const QMimeData* mime)
{
    std::vector<std::string> out;
    if (!mime || !mime->hasUrls())
        return out;
    for (const QUrl& url : mime->urls())
        if (url.isLocalFile() && isImageFile(url.toLocalFile().toStdString()))
            out.push_back(url.toLocalFile().toStdString());
    return out;
}

/** A picture dragged from a browser arrives as image data: kept as a file in the reference cache. */
std::string savedDropImage(const QMimeData* mime)
{
    if (!mime || !mime->hasImage())
        return {};
    QImage img = qvariant_cast<QImage>(mime->imageData());
    if (img.isNull())
        return {};
    const std::filesystem::path dir = paths::userConfigDir() / "referenceCache";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / ("dropped-" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmsszzz").toStdString() + ".png");
    if (!img.save(QString::fromStdWString(file.wstring()), "PNG"))
        return {};
    return paths::toUtf8(file);
}

json dragPayload(const QMimeData* mime)
{
    if (!mime || !mime->hasFormat(kDragMime))
        return nullptr;
    json j = json::parse(mime->data(kDragMime).toStdString(), nullptr, false);
    return j.is_object() ? j : json();
}
} // namespace

QImage displayImage(const Image& img)
{
    Cached& c = cache()[img.id];
    const std::string file = fileOf(img);
    // A video's raw picture changes with the scene time; a new frame redoes the grading.
    if (c.file != file || c.raw.isNull() || media::isVideoFile(file))
    {
        QImage raw = loadImage(img, file);
        if (c.file != file || raw.cacheKey() != c.raw.cacheKey())
            c.key.clear();
        c.raw = std::move(raw);
        c.file = file;
    }
    if (c.raw.isNull())
        return {};
    const std::string key = std::to_string(img.flipH) + std::to_string(img.flipV) + std::to_string(img.quarterTurns) + std::to_string(img.grayscale) +
                            std::to_string(img.brightness) + "," + std::to_string(img.contrast);
    if (c.key != key)
    {
        QImage out = c.raw.convertToFormat(QImage::Format_ARGB32);
        if (img.grayscale || img.brightness != 0.0 || img.contrast != 1.0)
            grade(out, img.grayscale, img.brightness, img.contrast);
        // Flips in the image's own frame, then the turns: the order overlays use too.
        if (img.flipH || img.flipV)
            out = out.flipped((img.flipH ? Qt::Horizontal : Qt::Orientations()) | (img.flipV ? Qt::Vertical : Qt::Orientations()));
        if (img.quarterTurns)
            out = out.transformed(QTransform().rotate(90.0 * img.quarterTurns));
        c.shown = std::move(out);
        c.key = key;
    }
    return c.shown;
}

/** The image of the shown tab, with wheel zoom at the cursor, drag to pan and an eyedropper. */
class ImageView final : public QWidget
{
public:
    explicit ImageView(QWidget* parent) : QWidget(parent)
    {
        setFocusPolicy(Qt::ClickFocus);
        setMouseTracking(true);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setMinimumSize(80, 60);
    }

    void setImage(const Image* img)
    {
        const std::string id = img ? img->id : std::string();
        QImage next = img ? displayImage(*img) : QImage();
        if (id != mId)
        {
            mZoom = 0.0;
            mPan = {};
        }
        else if (!next.isNull() && !mImage.isNull() && next.size() != mImage.size())
            mZoom = 0.0;
        mId = id;
        mImage = std::move(next);
        update();
    }

    void fit()
    {
        mZoom = 0.0;
        mPan = {};
        update();
    }

    void setPicking(bool on)
    {
        mPicking = on;
        setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
    }
    bool picking() const { return mPicking; }

    std::function<void(bool)> onPickingChanged;

protected:
    double scale() const
    {
        if (mImage.isNull())
            return 1.0;
        if (mZoom > 0.0)
            return mZoom;
        return std::min(double(width()) / mImage.width(), double(height()) / mImage.height());
    }

    QRectF imageRect() const
    {
        const double s = scale();
        const QSizeF size(mImage.width() * s, mImage.height() * s);
        const QPointF c = QPointF(width(), height()) * 0.5 + mPan;
        return {c - QPointF(size.width(), size.height()) * 0.5, size};
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(38, 38, 38));
        if (mImage.isNull())
        {
            if (!mId.empty())
            {
                p.setPen(QColor(200, 120, 120));
                p.drawText(rect(), Qt::AlignCenter, "Can't read this image");
            }
            return;
        }
        p.setRenderHint(QPainter::SmoothPixmapTransform, scale() < 2.0);
        p.drawImage(imageRect(), mImage);
    }

    void wheelEvent(QWheelEvent* e) override
    {
        if (mImage.isNull())
            return;
        const double steps = e->angleDelta().y() / 120.0;
        const double before = scale(), after = std::clamp(before * std::pow(1.2, steps), 0.02, 64.0);
        // Keep the image point under the cursor where it is.
        const QPointF c = QPointF(width(), height()) * 0.5;
        const QPointF cursor = e->position();
        mPan = cursor - (cursor - c - mPan) * (after / before) - c;
        mZoom = after;
        update();
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && (mPicking || (e->modifiers() & Qt::AltModifier)))
        {
            pick(e->position());
            if (mPicking && !(e->modifiers() & Qt::ShiftModifier))
            {
                setPicking(false);
                if (onPickingChanged)
                    onPickingChanged(false);
            }
            return;
        }
        if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton)
        {
            mDrag = e->position();
            setCursor(Qt::ClosedHandCursor);
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!mDrag)
            return;
        mPan += e->position() - *mDrag;
        mDrag = e->position();
        update();
    }

    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (mDrag)
        {
            mDrag.reset();
            setCursor(mPicking ? Qt::CrossCursor : Qt::ArrowCursor);
        }
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (mImage.isNull() || e->button() != Qt::LeftButton)
            return;
        if (mZoom > 0.0 && std::abs(mZoom - 1.0) < 1e-6)
            return fit();
        // Actual pixels, centred on what was double-clicked.
        const QRectF r = imageRect();
        const QPointF img = (e->position() - r.topLeft()) / scale();
        mZoom = 1.0;
        mPan = QPointF(mImage.width() * 0.5, mImage.height() * 0.5) - img;
        update();
    }

    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_F)
            return fit();
        if (e->key() == Qt::Key_1)
        {
            mZoom = 1.0;
            mPan = {};
            return update();
        }
        if (e->key() == Qt::Key_Escape && mPicking)
        {
            setPicking(false);
            if (onPickingChanged)
                onPickingChanged(false);
            return;
        }
        QWidget::keyPressEvent(e);
    }

    void pick(QPointF at)
    {
        const QRectF r = imageRect();
        if (mImage.isNull() || !r.contains(at))
            return;
        const QPoint px(std::clamp(int((at.x() - r.left()) / scale()), 0, mImage.width() - 1), std::clamp(int((at.y() - r.top()) / scale()), 0, mImage.height() - 1));
        const QColor c = mImage.pixelColor(px);
        auto toLinear = [](double v) { return float(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4)); };
        paintColor::pick({toLinear(c.redF()), toLinear(c.greenF()), toLinear(c.blueF())});
        setToolTip(QString("Picked %1 for painting").arg(c.name()));
    }

private:
    std::string mId;
    QImage mImage;
    double mZoom = 0.0; // screen pixels per image pixel; 0 fits
    QPointF mPan;
    std::optional<QPointF> mDrag;
    bool mPicking = false;
};

/** Tabs that reorder in place and, dragged clear of the bar, leave it as a drag of their own. */
class TabBar final : public QTabBar
{
public:
    explicit TabBar(QWidget* parent) : QTabBar(parent)
    {
        setMovable(true);
        setExpanding(false);
        setDocumentMode(true);
        setElideMode(Qt::ElideRight);
        setUsesScrollButtons(true);
    }

    std::function<void(int index)> onTearOff;

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton)
        {
            mPress = e->position().toPoint();
            mPressIndex = tabAt(mPress);
        }
        QTabBar::mousePressEvent(e);
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if ((e->buttons() & Qt::LeftButton) && mPressIndex >= 0)
        {
            const QPoint p = e->position().toPoint();
            const int margin = std::max(24, height());
            if (p.y() < -margin || p.y() > height() + margin || p.x() < -margin || p.x() > width() + margin)
            {
                const int index = mPressIndex;
                mPressIndex = -1;
                // Ends the bar's own reorder drag before the tab leaves as a drag of its own.
                QMouseEvent release(QEvent::MouseButtonRelease, e->position(), e->globalPosition(), Qt::LeftButton, Qt::NoButton, e->modifiers());
                QTabBar::mouseReleaseEvent(&release);
                if (onTearOff)
                    onTearOff(index);
                return;
            }
        }
        QTabBar::mouseMoveEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        mPressIndex = -1;
        QTabBar::mouseReleaseEvent(e);
    }

private:
    QPoint mPress;
    int mPressIndex = -1;
};

/** The drop feedback: a tinted wash over the window and a badge at the cursor saying what letting go does. */
class DropHint final : public QWidget
{
public:
    explicit DropHint(QWidget* parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
    }

    void showAt(QPoint at, const QString& iconName, const QString& text)
    {
        mAt = at;
        mIcon = iconName;
        mText = text;
        setGeometry(parentWidget()->rect());
        raise();
        show();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor accent = palette().color(QPalette::Highlight);
        QColor wash = accent;
        wash.setAlpha(40);
        p.fillRect(rect(), wash);
        p.setPen(QPen(accent, 2.0));
        p.drawRect(rect().adjusted(1, 1, -2, -2));
        const QFontMetrics fm(font());
        const int iconSize = 20, pad = 6;
        const QSize box(iconSize + pad * 3 + fm.horizontalAdvance(mText), iconSize + pad * 2);
        QPoint topLeft = mAt + QPoint(18, 14);
        topLeft.setX(std::clamp(topLeft.x(), 2, std::max(2, width() - box.width() - 2)));
        topLeft.setY(std::clamp(topLeft.y(), 2, std::max(2, height() - box.height() - 2)));
        const QRect r(topLeft, box);
        QPainterPath path;
        path.addRoundedRect(r, 5, 5);
        p.fillPath(path, QColor(24, 24, 24, 230));
        p.setPen(QPen(accent, 1.0));
        p.drawPath(path);
        icon(mIcon).paint(&p, QRect(r.left() + pad, r.top() + pad, iconSize, iconSize));
        p.setPen(QColor(235, 235, 235));
        p.drawText(QRect(r.left() + pad * 2 + iconSize, r.top(), box.width(), box.height()), Qt::AlignVCenter | Qt::AlignLeft, mText);
    }

private:
    QPoint mAt;
    QString mIcon, mText;
};

ReferenceWindow::ReferenceWindow(std::string windowId, QWidget* parent) : QWidget(parent), mWindow(std::move(windowId))
{
    setAcceptDrops(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    mToolbar = new QWidget(this);
    auto* bar = new QHBoxLayout(mToolbar);
    bar->setContentsMargins(4, 2, 4, 2);
    bar->setSpacing(2);
    auto button = [this, bar](const char* iconName, const QString& tip, std::function<void()> fn, bool checkable = false) {
        auto* b = new QToolButton(mToolbar);
        b->setIcon(icon(iconName));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setCheckable(checkable);
        connect(b, &QToolButton::clicked, this, [fn = std::move(fn)] { fn(); });
        bar->addWidget(b);
        return b;
    };
    auto edit = [this](const char* flag, auto value) {
        if (std::string id = currentImage(); !id.empty())
            runCommand("referenceImage", {{"edit", id}, {flag, value}});
    };
    button("photo-plus", "Add images as tabs (or drop them here; hold Shift while dropping to replace the shown one)", [this] { openFiles(); });
    mFlipH = button("flip-vertical", "Mirror left to right", [this, edit] {
        if (const Image* i = current().image(currentImage()))
            edit("flipH", !i->flipH);
    }, true);
    mFlipV = button("flip-horizontal", "Mirror top to bottom", [this, edit] {
        if (const Image* i = current().image(currentImage()))
            edit("flipV", !i->flipV);
    }, true);
    button("rotate-clockwise", "Turn a quarter clockwise", [this, edit] {
        if (const Image* i = current().image(currentImage()))
            edit("quarterTurns", (i->quarterTurns + 1) % 4);
    });
    mGray = button("contrast", "Grayscale, to judge values", [this, edit] {
        if (const Image* i = current().image(currentImage()))
            edit("grayscale", !i->grayscale);
    }, true);
    button("zoom-reset", "Fit (F); 1 shows actual pixels, double-click toggles", [this] { mView->fit(); });
    mPick = button("color-picker", "Pick a paint color from the image (or Alt+click it). Shift keeps picking", [this] { mView->setPicking(mPick->isChecked()); }, true);
    button("layers-subtract", "Show in the active viewport as an overlay (or drag the tab onto a viewport)", [this] {
        if (std::string id = currentImage(); !id.empty())
            referenceOverlays::show(id);
    });
    {
        mVideoBar = new QWidget(mToolbar);
        auto* vb = new QHBoxLayout(mVideoBar);
        vb->setContentsMargins(8, 0, 0, 0);
        vb->setSpacing(2);
        auto videoButton = [this, vb](const char* iconName, const QString& tip, std::function<void()> fn, bool checkable = false) {
            auto* b = new QToolButton(mVideoBar);
            b->setIcon(icon(iconName));
            b->setToolTip(tip);
            b->setAutoRaise(true);
            b->setCheckable(checkable);
            connect(b, &QToolButton::clicked, this, [fn = std::move(fn)] { fn(); });
            vb->addWidget(b);
            return b;
        };
        // A step moves the held frame while paused, and nudges the sync with the scene otherwise.
        auto step = [this](int by) {
            const std::string id = currentImage();
            const Image* i = current().image(id);
            if (!i)
                return;
            if (i->paused)
                runCommand("referenceImage", {{"edit", id}, {"pausedFrame", std::max<int64_t>(0, i->pausedFrame + by)}});
            else
                runCommand("referenceImage", {{"edit", id}, {"videoOffset", i->videoOffset + by}});
        };
        mPause = videoButton("player-pause", "Pause\nHold this frame instead of following the time slider", [this] {
            if (const Image* i = current().image(currentImage()))
                runCommand("referenceImage", {{"edit", i->id}, {"paused", !i->paused}});
        }, true);
        videoButton("chevron-left", "Previous Frame\nWhile paused, the held frame; while playing, moves the video's sync a frame", [step] { step(-1); });
        videoButton("chevron-right", "Next Frame\nWhile paused, the held frame; while playing, moves the video's sync a frame", [step] { step(1); });
        auto* label = new QLabel("Offset", mVideoBar);
        label->setEnabled(false);
        vb->addSpacing(4);
        vb->addWidget(label);
        mVideoOffset = new QSpinBox(mVideoBar);
        mVideoOffset->setRange(-100000, 100000);
        mVideoOffset->setSuffix(" f");
        mVideoOffset->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        mVideoOffset->setKeyboardTracking(false);
        mVideoOffset->setToolTip("Video Offset\nFrames added to the frame shown, to line the video up with the scene");
        connect(mVideoOffset, &QSpinBox::valueChanged, this, [this](int v) {
            if (const Image* i = current().image(currentImage()); i && i->videoOffset != v)
                runCommand("referenceImage", {{"edit", i->id}, {"videoOffset", v}});
        });
        vb->addWidget(mVideoOffset);
        bar->addWidget(mVideoBar);
    }
    bar->addStretch(1);
    mMore = new QToolButton(mToolbar);
    mMore->setIcon(icon("dots"));
    mMore->setAutoRaise(true);
    mMore->setToolTip("Tab, window and set options");
    mMore->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(mMore);
    mMore->setMenu(menu);
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        menu->clear();
        const std::string id = currentImage();
        const Image* img = current().image(id);
        if (img)
        {
            QAction* emb = menu->addAction("Embed in Set");
            emb->setCheckable(true);
            emb->setChecked(!img->data.empty());
            const bool video = isVideo(*img);
            emb->setEnabled(!video || !img->data.empty());
            emb->setToolTip(video ? "Videos stay linked to their file" : "Keep the image's bytes in the set, so it travels without the file");
            connect(emb, &QAction::triggered, this, [id](bool on) { runCommand("referenceImage", {{"edit", id}, {"embed", on}}); });
            if (video)
            {
                QMenu* rate = menu->addMenu("Video Rate");
                for (double fps : {0.0, 12.0, 24.0, 25.0, 30.0, 48.0, 50.0, 60.0})
                {
                    const media::VideoInfo* info = videoTextures::info(fileOf(*img));
                    const QString text = fps > 0.0 ? QString("%1 fps").arg(fps)
                                                   : info ? QString("File (%1 fps)").arg(info->fps, 0, 'g', 4) : QString("File");
                    QAction* r = rate->addAction(text, this, [id, fps] { runCommand("referenceImage", {{"edit", id}, {"videoFps", fps}}); });
                    r->setCheckable(true);
                    r->setChecked(std::abs(img->videoFps - fps) < 1e-6);
                }
            }
            menu->addAction("Rename...", this, [this, id] {
                const Image* i = current().image(id);
                bool ok = false;
                QString name = QInputDialog::getText(this, "Rename Reference", "Name", QLineEdit::Normal, QString::fromStdString(i ? i->name : ""), &ok);
                if (ok && !name.isEmpty())
                    runCommand("referenceImage", {{"edit", id}, {"name", name.toStdString()}});
            });
            if (!img->path.empty())
                menu->addAction(icon("folder"), "Show in Explorer", this, [img] {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(QString::fromStdString(fileOf(*img))).absolutePath()));
                });
            menu->addAction("Reload", this, [this, id] {
                cache().erase(id);
                if (const Image* i = current().image(id))
                    textures::release(fileOf(*i), true);
                mView->setImage(current().image(id));
            });
            menu->addAction("Close Tab", this, [id] { runCommand("referenceImage", {{"remove", json::array({id})}}); });
            if (current().tabs(mWindow).size() > 1)
                menu->addAction("Close Other Tabs", this, [this, id] {
                    json ids = json::array();
                    for (const Image* t : current().tabs(mWindow))
                        if (t->id != id)
                            ids.push_back(t->id);
                    runCommand("referenceImage", {{"remove", ids}});
                });
            menu->addSeparator();
        }
        if (const Window* w = current().window(mWindow))
        {
            QDockWidget* dock = dockFor(mWindow);
            const bool floating = dock && dock->isFloating();
            QMenu* op = menu->addMenu("Window Opacity");
            op->setEnabled(floating);
            for (int pct : {100, 85, 70, 50, 35})
            {
                QAction* a = op->addAction(QString("%1%").arg(pct));
                a->setCheckable(true);
                a->setChecked(std::abs(w->opacity * 100.0 - pct) < 1.0);
                connect(a, &QAction::triggered, this, [this, pct] {
                    tweak([&](Set& s) {
                        if (Window* win = s.window(mWindow))
                            win->opacity = pct / 100.0;
                    });
                    windows::applyWindowOptions(mWindow);
                });
            }
            QAction* top = menu->addAction("Stay on Top of Other Apps");
            top->setCheckable(true);
            top->setChecked(w->onTop);
            top->setEnabled(floating);
            connect(top, &QAction::triggered, this, [this](bool on) {
                tweak([&](Set& s) {
                    if (Window* win = s.window(mWindow))
                        win->onTop = on;
                });
                windows::applyWindowOptions(mWindow);
            });
            QAction* through = menu->addAction("Click-Through");
            through->setCheckable(true);
            through->setChecked(w->clickThrough);
            through->setEnabled(floating);
            through->setToolTip("Clicks pass through the floating window to what is under it; Windows > Reference Windows Clickable undoes it");
            connect(through, &QAction::triggered, this, [this](bool on) {
                tweak([&](Set& s) {
                    if (Window* win = s.window(mWindow))
                        win->clickThrough = on;
                });
                windows::applyWindowOptions(mWindow);
            });
            menu->addSeparator();
        }
        QMenu* where = menu->addMenu("Keep Set In");
        auto* group = new QActionGroup(where);
        for (Storage s : {Storage::Scene, Storage::Project, Storage::User})
        {
            static const char* const kLabels[] = {"Scene", "Project Folder", "User Settings (every scene)"};
            QAction* a = where->addAction(kLabels[int(s)]);
            a->setCheckable(true);
            a->setChecked(storage() == s);
            a->setEnabled(s != Storage::Project || Workspace::get().isOpen());
            group->addAction(a);
            connect(a, &QAction::triggered, this, [s] { runCommand("referenceSet", {{"storage", storageName(s)}}); });
        }
        menu->addAction("Save Set...", this, [this] {
            QString file = fileDialog::getSaveFileName(this, "Save Reference Set", {}, "Reference sets (*.lilaref)");
            if (!file.isEmpty())
                runCommand("referenceSet", {{"save", file.toStdString()}});
        });
        menu->addAction("Load Set...", this, [this] {
            QString file = fileDialog::getOpenFileName(this, "Load Reference Set", {}, "Reference sets (*.lilaref)");
            if (!file.isEmpty())
                runCommand("referenceSet", {{"load", file.toStdString()}});
        });
    });
    bar->addWidget(mMore);
    layout->addWidget(mToolbar);

    mTabs = new TabBar(this);
    mTabs->onTearOff = [this](int index) { startTabDrag(index); };
    connect(mTabs, &QTabBar::currentChanged, this, [this](int index) {
        if (mRebuilding || index < 0)
            return;
        const std::string id = mTabs->tabData(index).toString().toStdString();
        tweak([&](Set& s) {
            if (Window* w = s.window(mWindow))
                w->current = id;
        });
    });
    connect(mTabs, &QTabBar::tabMoved, this, [this](int, int) {
        if (mRebuilding)
            return;
        // The set keeps the bar's order: this window's images in tab order, everything else where it was.
        std::vector<std::string> order;
        for (int i = 0; i < mTabs->count(); ++i)
            order.push_back(mTabs->tabData(i).toString().toStdString());
        tweak([&](Set& s) {
            std::vector<Image> mine, rest;
            for (const std::string& id : order)
                if (Image* img = s.image(id))
                    mine.push_back(*img);
            std::vector<Image> out;
            size_t k = 0;
            for (Image& img : s.images)
                out.push_back(img.window == mWindow && k < mine.size() ? mine[k++] : img);
            s.images = std::move(out);
        });
    });
    mTabs->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(mTabs, &QWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        const int index = mTabs->tabAt(at);
        if (index < 0)
            return;
        mTabs->setCurrentIndex(index);
        mMore->menu()->exec(mTabs->mapToGlobal(at));
    });
    layout->addWidget(mTabs);

    mView = new ImageView(this);
    mView->onPickingChanged = [this](bool on) { mPick->setChecked(on); };
    layout->addWidget(mView, 1);

    mEmpty = new QLabel("Drop reference images here\nor add them with the + button", this);
    mEmpty->setAlignment(Qt::AlignCenter);
    mEmpty->setEnabled(false);
    layout->addWidget(mEmpty, 1);

    mHint = new DropHint(this);
    mListener = addListener([this] { rebuild(); });
    mTextureListener = textures::addListener([this] {
        if (mView->isVisible() && !currentImage().empty())
            mView->setImage(current().image(currentImage()));
    });
    auto refreshVideo = [this] {
        if (!mView->isVisible() || currentImage().empty())
            return;
        if (const Image* img = current().image(currentImage()); img && media::isVideoFile(fileOf(*img)))
            mView->setImage(img);
    };
    mVideoListener = videoTextures::addListener(refreshVideo);
    onTimeChanged(this, refreshVideo);
    rebuild();
}

ReferenceWindow::~ReferenceWindow()
{
    removeListener(mListener);
    textures::removeListener(mTextureListener);
    videoTextures::removeListener(mVideoListener);
}

QTabBar* ReferenceWindow::tabBar() const
{
    return mTabs;
}

std::string ReferenceWindow::currentImage() const
{
    const int i = mTabs->currentIndex();
    return i < 0 ? std::string() : mTabs->tabData(i).toString().toStdString();
}

DropAction ReferenceWindow::dropActionFor(Qt::KeyboardModifiers mods, bool hasCurrent)
{
    return dropAction(mods & Qt::ShiftModifier, hasCurrent);
}

void ReferenceWindow::rebuild()
{
    const Set& s = current();
    const auto tabs = s.tabs(mWindow);
    const Window* w = s.window(mWindow);
    mRebuilding = true;
    // Only touch the bar when its tabs changed, so a tweak doesn't fight a drag in progress.
    bool same = mTabs->count() == int(tabs.size());
    for (int i = 0; same && i < mTabs->count(); ++i)
        same = mTabs->tabData(i).toString().toStdString() == tabs[size_t(i)]->id && mTabs->tabText(i).toStdString() == tabs[size_t(i)]->name;
    if (!same)
    {
        while (mTabs->count())
            mTabs->removeTab(0);
        for (const Image* img : tabs)
        {
            const int i = mTabs->addTab(QString::fromStdString(img->name));
            mTabs->setTabData(i, QString::fromStdString(img->id));
            mTabs->setTabToolTip(i, QString::fromStdString(img->data.empty() ? fileOf(*img) : img->name + " (embedded)"));
            auto* close = new QToolButton(mTabs);
            close->setIcon(icon("x"));
            close->setIconSize(QSize(12, 12));
            close->setAutoRaise(true);
            close->setToolTip("Close tab");
            connect(close, &QToolButton::clicked, this, [id = img->id] { runCommand("referenceImage", {{"remove", json::array({id})}}); });
            mTabs->setTabButton(i, QTabBar::RightSide, close);
        }
    }
    int currentIndex = 0;
    for (size_t i = 0; i < tabs.size(); ++i)
        if (w && tabs[i]->id == w->current)
            currentIndex = int(i);
    if (!tabs.empty())
        mTabs->setCurrentIndex(currentIndex);
    mRebuilding = false;

    mTabs->setVisible(tabs.size() > 1);
    mView->setVisible(!tabs.empty());
    mEmpty->setVisible(tabs.empty());
    const Image* img = tabs.empty() ? nullptr : tabs[size_t(currentIndex)];
    mView->setImage(img);
    for (QToolButton* b : {mFlipH, mFlipV, mGray, mPick})
        b->setEnabled(img != nullptr);
    mFlipH->setChecked(img && img->flipH);
    mFlipV->setChecked(img && img->flipV);
    mGray->setChecked(img && img->grayscale);
    const bool video = img && isVideo(*img);
    mVideoBar->setVisible(video);
    if (video)
    {
        mPause->setChecked(img->paused);
        mPause->setIcon(icon(img->paused ? "player-play" : "player-pause"));
        mPause->setToolTip(img->paused ? "Play\nFollow the time slider again" : "Pause\nHold this frame instead of following the time slider");
        QSignalBlocker block(mVideoOffset);
        mVideoOffset->setValue(img->videoOffset);
    }
    if (QDockWidget* dock = dockFor(mWindow); dock && mWindow != kMainWindow)
        dock->setWindowTitle(img ? QString::fromStdString(img->name) : QString("Reference Images"));
}

void ReferenceWindow::openFiles()
{
    QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
    const fileDialog::ContextScope scope("referenceImages", "Images");
    QStringList files = fileDialog::getOpenFileNames(this, "Add Reference Images", start, kImageFilter);
    if (files.isEmpty())
        return;
    json call = {{"window", mWindow}};
    call["args"] = json::array();
    for (const QString& f : files)
        call["args"].push_back(f.toStdString());
    runCommand("referenceImage", call);
}

void ReferenceWindow::startTabDrag(int index)
{
    const std::string id = mTabs->tabData(index).toString().toStdString();
    const Image* img = current().image(id);
    if (!img)
        return;
    auto* mime = new QMimeData;
    mime->setData(kDragMime, QByteArray::fromStdString(json{{"image", id}, {"window", mWindow}}.dump()));
    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    if (QImage shown = displayImage(*img); !shown.isNull())
    {
        QPixmap pm = QPixmap::fromImage(shown.scaled(180, 140, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        drag->setPixmap(pm);
        drag->setHotSpot(QPoint(pm.width() / 2, 10));
    }
    QPointer<ReferenceWindow> self(this);
    const std::string window = mWindow;
    const Qt::DropAction result = drag->exec(Qt::MoveAction | Qt::CopyAction, Qt::MoveAction);
    if (result != Qt::IgnoreAction)
        return;
    // Let go where nothing took it: the tab becomes a window of its own there, as a browser tab does.
    const Image* still = current().image(id);
    if (!still || still->window != window)
        return;
    const QPoint at = QCursor::pos();
    if (window != kMainWindow && current().tabs(window).size() == 1)
    {
        if (QDockWidget* dock = dockFor(window))
        {
            dock->setFloating(true);
            dock->move(at - QPoint(60, 15));
        }
        return;
    }
    Set next = current();
    const std::string newId = next.newWindowId();
    next.image(id)->window = newId;
    next.windows.push_back(Window{.id = newId, .current = id});
    if (Window* src = next.window(window); src && src->current == id)
    {
        auto rest = next.tabs(window);
        src->current = rest.empty() ? std::string() : rest.front()->id;
    }
    pendingPositions()[newId] = at;
    apply(std::move(next), "Tear Off Reference Tab");
    (void)self;
}

void ReferenceWindow::showDropHint(QPoint at, const QString& iconName, const QString& text)
{
    mHint->showAt(at, iconName, text);
}

void ReferenceWindow::hideDropHint()
{
    mHint->hide();
}

void ReferenceWindow::dragEnterEvent(QDragEnterEvent* e)
{
    dragMoveEvent(e);
}

void ReferenceWindow::dragMoveEvent(QDragMoveEvent* e)
{
    const QMimeData* mime = e->mimeData();
    const QPoint at = e->position().toPoint();
    if (json payload = dragPayload(mime); payload.is_object())
    {
        const bool copy = e->modifiers() & Qt::ControlModifier;
        const bool own = payload.value("window", "") == mWindow;
        e->setDropAction(copy ? Qt::CopyAction : Qt::MoveAction);
        e->accept();
        if (own && !copy)
            showDropHint(at, "arrows-move-horizontal", "Back in its place");
        else
            showDropHint(at, copy ? "copy" : "arrow-forward-up", copy ? "Copy tab here" : "Move tab here");
        return;
    }
    const std::vector<std::string> files = droppedFiles(mime);
    if (files.empty() && !(mime && mime->hasImage()))
    {
        hideDropHint();
        return e->ignore();
    }
    const bool hasCurrent = !currentImage().empty();
    if (dropActionFor(e->modifiers(), hasCurrent) == DropAction::Replace)
    {
        // Move shows no +: Windows' own cue for Shift in a drag.
        e->setDropAction(Qt::MoveAction);
        const Image* cur = current().image(currentImage());
        showDropHint(at, "replace", QString("Replace %1").arg(QString::fromStdString(cur ? cur->name : "")));
    }
    else
    {
        e->setDropAction(Qt::CopyAction);
        const size_t n = std::max<size_t>(files.size(), 1);
        showDropHint(at, "photo-plus", n == 1 ? QString("New tab   (Shift: replace)") : QString("%1 new tabs").arg(n));
    }
    e->accept();
}

void ReferenceWindow::dragLeaveEvent(QDragLeaveEvent*)
{
    hideDropHint();
}

void ReferenceWindow::dropEvent(QDropEvent* e)
{
    hideDropHint();
    const QMimeData* mime = e->mimeData();
    if (json payload = dragPayload(mime); payload.is_object())
    {
        const std::string id = payload.value("image", ""), from = payload.value("window", "");
        const bool copy = e->modifiers() & Qt::ControlModifier;
        e->setDropAction(copy ? Qt::CopyAction : Qt::MoveAction);
        e->accept();
        if (from == mWindow && !copy)
            return;
        Set next = current();
        Image* img = next.image(id);
        if (!img)
            return;
        if (copy)
        {
            Image dup = *img;
            dup.id = next.newImageId();
            dup.window = mWindow;
            next.images.push_back(dup);
            next.window(mWindow)->current = dup.id;
            apply(std::move(next), "Copy Reference Tab");
            return;
        }
        img->window = mWindow;
        // Moved to the end of this window's tabs.
        Image moved = *img;
        std::erase_if(next.images, [&](const Image& i) { return i.id == id; });
        next.images.push_back(moved);
        next.window(mWindow)->current = id;
        if (Window* src = next.window(from); src && src->current == id)
        {
            auto rest = next.tabs(from);
            src->current = rest.empty() ? std::string() : rest.front()->id;
        }
        std::erase_if(next.windows, [&](const Window& w) { return w.id != kMainWindow && w.id == from && next.tabs(w.id).empty(); });
        apply(std::move(next), "Move Reference Tab");
        return;
    }
    std::vector<std::string> files = droppedFiles(mime);
    if (files.empty())
        if (std::string saved = savedDropImage(mime); !saved.empty())
            files.push_back(saved);
    if (files.empty())
        return e->ignore();
    const std::string cur = currentImage();
    if (dropActionFor(e->modifiers(), !cur.empty()) == DropAction::Replace)
    {
        e->setDropAction(Qt::MoveAction);
        e->accept();
        runCommand("referenceImage", {{"args", json::array({files.front()})}, {"replace", cur}});
        return;
    }
    e->setDropAction(Qt::CopyAction);
    e->accept();
    json call = {{"window", mWindow}, {"args", json::array()}};
    for (const std::string& f : files)
        call["args"].push_back(f);
    if (mTabs->isVisible())
        if (int at = mTabs->tabAt(mTabs->mapFrom(this, e->position().toPoint())); at >= 0)
            call["index"] = at + 1;
    runCommand("referenceImage", call);
}

namespace windows
{
namespace
{
/** Tab counts per window at the last sync, so a window that gains tabs comes forward. */
std::map<std::string, size_t>& lastCounts()
{
    static std::map<std::string, size_t> m;
    return m;
}

/** Closing a torn-off window closes its tabs, as closing a browser window does; Undo brings them back. */
class CloseFilter final : public QObject
{
public:
    CloseFilter(std::string window, QObject* parent) : QObject(parent), mWindow(std::move(window)) {}

    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() == QEvent::Close)
        {
            json ids = json::array();
            for (const Image* t : current().tabs(mWindow))
                ids.push_back(t->id);
            if (!ids.empty())
                QTimer::singleShot(0, qApp, [ids] { runCommand("referenceImage", {{"remove", ids}}); });
        }
        return QObject::eventFilter(obj, e);
    }

private:
    std::string mWindow;
};

void placeDock(QDockWidget* dock, const std::string& window)
{
    QObject::connect(dock, &QDockWidget::topLevelChanged, dock, [window](bool) { applyWindowOptions(window); });
    if (window == kMainWindow)
        return;
    if (const Window* w = current().window(window))
        if (const Image* img = current().image(w->current))
            dock->setWindowTitle(QString::fromStdString(img->name));
    dock->installEventFilter(new CloseFilter(window, dock));
    auto* mw = qobject_cast<QMainWindow*>(dock->parentWidget());
    auto pending = pendingPositions().find(window);
    if (pending == pendingPositions().end() && mw && mw->restoreDockWidget(dock))
    {
        dock->show();
        return;
    }
    dock->setFloating(true);
    dock->resize(520, 420);
    QPoint at = pending != pendingPositions().end() ? pending->second : (mw ? mw->geometry().center() : QCursor::pos());
    if (pending != pendingPositions().end())
        pendingPositions().erase(pending);
    dock->move(at - QPoint(60, 15));
    dock->show();
    dock->raise();
    QTimer::singleShot(0, dock, [window] { applyWindowOptions(window); });
}

void sync()
{
    const Set& s = current();
    std::set<std::string> wanted;
    for (const Window& w : s.windows)
        wanted.insert(panelId(w.id));
    for (const std::string& id : panels::registry().ids())
        if (id.starts_with(std::string(kPanelPrefix) + "_") && !wanted.contains(id))
            panels::remove(id);
    for (const Window& w : s.windows)
    {
        const std::string pid = panelId(w.id);
        if (w.id != kMainWindow && !panels::registry().find(pid))
        {
            const std::string window = w.id;
            panels::add({.id = pid,
                         .title = "Reference Images",
                         .create = [window](QWidget* parent) { return new ReferenceWindow(window, parent); },
                         .area = Qt::RightDockWidgetArea,
                         .icon = "photo",
                         .open = true,
                         .place = [window](QDockWidget* d) { placeDock(d, window); },
                         .docs = "reference-images"});
        }
        const size_t count = s.tabs(w.id).size();
        auto& last = lastCounts()[w.id];
        if (count > last && w.id == kMainWindow)
            panels::show(pid);
        last = count;
    }
    std::erase_if(lastCounts(), [&](const auto& kv) { return !s.window(kv.first); });
}
} // namespace

std::string panelId(const std::string& windowId)
{
    return windowId == kMainWindow ? std::string(kPanelPrefix) : std::string(kPanelPrefix) + "_" + windowId;
}

void applyWindowOptions(const std::string& windowId)
{
    QDockWidget* dock = dockFor(windowId);
    const Window* w = current().window(windowId);
    if (!dock || !w)
        return;
    if (!dock->isFloating())
    {
        dock->setWindowOpacity(1.0);
        return;
    }
    dock->setWindowOpacity(std::clamp(w->opacity, 0.1, 1.0));
    const Qt::WindowFlags before = dock->windowFlags();
    Qt::WindowFlags flags = before;
    flags.setFlag(Qt::WindowStaysOnTopHint, w->onTop);
    flags.setFlag(Qt::WindowTransparentForInput, w->clickThrough);
    if (flags != before)
    {
        const QRect geo = dock->geometry();
        dock->setWindowFlags(flags);
        dock->setGeometry(geo);
        dock->show();
    }
}

void install()
{
    static bool done = false;
    if (std::exchange(done, true))
        return;
    panels::add({.id = kPanelPrefix,
                 .title = "Reference Images",
                 .create = [](QWidget* parent) { return new ReferenceWindow(kMainWindow, parent); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "photo",
                 .open = false,
                 .place = [](QDockWidget* d) { placeDock(d, kMainWindow); },
                 .toggleAction = "window.referenceImages",
                 .docs = "reference-images"});
    addListener([] { QTimer::singleShot(0, qApp, sync); });
    QTimer::singleShot(0, qApp, sync);

    actions::add({.id = "reference.addImages",
                  .label = "Add Reference Images...",
                  .category = "Windows",
                  .run =
                      [] {
                          panels::show(kPanelPrefix);
                          QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
                          const fileDialog::ContextScope scope("referenceImages", "Images");
                          QStringList files = fileDialog::getOpenFileNames(QApplication::activeWindow(), "Add Reference Images", start, kImageFilter);
                          if (files.isEmpty())
                              return;
                          json call = {{"args", json::array()}};
                          for (const QString& f : files)
                              call["args"].push_back(f.toStdString());
                          runCommand("referenceImage", call);
                      },
                  .doc = "Opens images as tabs of the Reference Images window.",
                  .python = "referenceImage(\"refs/front.jpg\")",
                  .icon = "photo-plus"});
    actions::add({.id = "reference.clickable",
                  .label = "Reference Windows Clickable",
                  .category = "Windows",
                  .run =
                      [] {
                          std::vector<std::string> ids;
                          tweak([&](Set& s) {
                              for (Window& w : s.windows)
                                  if (std::exchange(w.clickThrough, false))
                                      ids.push_back(w.id);
                          });
                          for (const std::string& id : ids)
                              applyWindowOptions(id);
                      },
                  .doc = "Turns click-through off on every floating Reference window, so it takes the mouse again.",
                  .icon = "click"});
}
} // namespace windows
} // namespace lila::ui::references
