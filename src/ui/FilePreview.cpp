#include "ui/FilePreview.h"

#include "core/PrefStore.h"
#include "fbx/Fbx.h"
#include "geo/ObjIO.h"
#include "io/ScenePreview.h"
#include "tex/PainterFile.h"
#include "ui/TextureCache.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <thread>

namespace lila::ui
{
namespace
{
constexpr int kFpDefaultMaxMb = 50;
constexpr float kFpFov = 0.6f;
constexpr int kFpThumbSize = 1024;

bool fpIsImage(const QString& ext)
{
    static const QStringList exts = {"png", "jpg", "jpeg", "tga", "bmp", "tif", "tiff", "exr", "hdr", "psd", "webp", "dds", "gif"};
    return exts.contains(ext);
}

std::vector<glm::vec3> fpObjTriangles(const QString& path)
{
    geo::ObjReadOptions opts;
    opts.splitObjects = false;
    opts.smoothingGroupsToHardEdges = false;
    opts.keepNormals = false;
    const geo::ObjReadResult r = geo::readObjFile(path.toStdString(), opts);
    std::vector<glm::vec3> out;
    std::vector<geo::Index> points;
    for (const geo::ObjObject& o : r.objects)
        for (geo::Index f = 0; f < o.mesh.numFaces(); ++f)
        {
            points.clear();
            o.mesh.forEachFaceCorner(f, [&](geo::Index c) { points.push_back(o.mesh.cornerPoint(c)); });
            for (size_t i = 1; i + 1 < points.size(); ++i)
                for (geo::Index p : {points[0], points[i], points[i + 1]})
                    out.emplace_back(glm::vec3(o.mesh.position(p)));
        }
    return out;
}

std::vector<glm::vec3> fpLoad(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == "obj")
        return fpObjTriangles(path);
    if (ext == "lila" || ext == "lilaa")
        return io::previewTriangles(path.toStdWString());
    return fbx::previewTriangles(path.toStdWString());
}
} // namespace

FilePreview::FilePreview(QWidget* parent) : QWidget(parent), mAlive(std::make_shared<bool>(true))
{
    setMinimumHeight(160);
    setCursor(Qt::OpenHandCursor);
    mTextureListener = textures::addListener([this] { pollImage(); });
}

FilePreview::~FilePreview()
{
    *mAlive = false;
    textures::removeListener(mTextureListener);
}

bool FilePreview::canPreview(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    return ext == "fbx" || ext == "obj" || ext == "lila" || ext == "lilaa" || ext == "spsm" || fpIsImage(ext);
}

bool FilePreview::setFile(const QString& path)
{
    if (path == mFile)
        return !mFile.isEmpty();
    ++mGeneration;
    mFile.clear();
    mCorners.clear();
    mMessage.clear();
    mLoading = false;
    mDirty = true;
    mIsImage = false;
    mThumb = {};
    setCursor(Qt::OpenHandCursor);
    const QFileInfo fi(path);
    if (path.isEmpty() || !canPreview(path) || !fi.isFile())
    {
        update();
        return false;
    }
    mFile = path;
    const qint64 maxBytes = qint64(PrefStore::get().value<int>("general", kMaxMbPref, kFpDefaultMaxMb)) * 1024 * 1024;
    if (fi.size() > maxBytes)
    {
        mMessage = QString("%1: too large to preview").arg(QLocale().formattedDataSize(fi.size()));
        update();
        return true;
    }
    mMessage = "Loading...";
    if (fi.suffix().toLower() == "spsm")
    {
        // Painter's own preview of the smart material; read off the UI thread, as the file holds its textures too.
        mIsImage = true;
        setCursor(Qt::ArrowCursor);
        update();
        std::thread([path, generation = mGeneration, alive = mAlive, self = this] {
            QImage thumb;
            try
            {
                const image::Image img = tex::painter::readSpsmPreview(path.toStdWString());
                if (img.width > 0)
                {
                    thumb = QImage(img.width, img.height, QImage::Format_RGBA8888);
                    for (int y = 0; y < img.height; ++y)
                    {
                        uchar* row = thumb.scanLine(y);
                        for (int x = 0; x < img.width * 4; ++x)
                            row[x] = uchar(std::clamp(img.rgba[size_t(y) * size_t(img.width) * 4 + size_t(x)], 0.0f, 1.0f) * 255.0f + 0.5f);
                    }
                }
            }
            catch (const std::exception&)
            {
            }
            QMetaObject::invokeMethod(qApp, [alive, self, generation, thumb = std::move(thumb)]() mutable {
                if (!*alive || self->mGeneration != generation)
                    return;
                self->mThumb = std::move(thumb);
                self->mMessage = self->mThumb.isNull() ? QString("No preview") : QString("Painter smart material");
                self->update();
            });
        }).detach();
        return true;
    }
    mLoading = true;
    if (fpIsImage(fi.suffix().toLower()))
    {
        mIsImage = true;
        setCursor(Qt::ArrowCursor);
        pollImage();
        update();
        return true;
    }
    update();
    std::thread([path, generation = mGeneration, alive = mAlive, self = this] {
        std::vector<glm::vec3> corners = fpLoad(path);
        QMetaObject::invokeMethod(qApp, [alive, self, generation, corners = std::move(corners)]() mutable {
            if (*alive && self->mGeneration == generation)
                self->loaded(std::move(corners));
        });
    }).detach();
    return true;
}

void FilePreview::loaded(std::vector<glm::vec3> corners)
{
    mLoading = false;
    mCorners = std::move(corners);
    mMessage = mCorners.empty() ? QString("No meshes") : QLocale().toString(qulonglong(mCorners.size() / 3)) + " triangles";
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (const glm::vec3& p : mCorners)
    {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    mCenter = mCorners.empty() ? glm::vec3(0.0f) : (lo + hi) * 0.5f;
    mRadius = mCorners.empty() ? 1.0f : std::max(glm::length(hi - lo) * 0.5f, 1e-6f);
    resetView();
}

void FilePreview::pollImage()
{
    if (!mIsImage || !mLoading)
        return;
    const std::string path = mFile.toStdString();
    mThumb = textures::thumbnail(path, kFpThumbSize);
    if (mThumb.isNull())
    {
        if (textures::thumbnailError(path, kFpThumbSize).empty())
            return;
        mMessage = "Can't read image";
    }
    else
    {
        // The thumbnail is scaled down, so the file's own size comes from its header when Qt can read one.
        const QSize size = QImageReader(mFile).size();
        mMessage = size.isValid() ? QString("%1 x %2").arg(size.width()).arg(size.height()) : QString();
    }
    mLoading = false;
    update();
}

void FilePreview::resetView()
{
    mYaw = 0.6f;
    mPitch = 0.35f;
    mZoom = 1.0f;
    mDirty = true;
    update();
}

void FilePreview::rasterize()
{
    const qreal dpr = devicePixelRatioF();
    const int w = std::max(1, int(width() * dpr)), h = std::max(1, int(height() * dpr));
    mImage = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
    mImage.setDevicePixelRatio(dpr);
    mImage.fill(Qt::transparent);
    mDirty = false;
    if (mCorners.empty())
        return;

    const float cy = std::cos(mYaw), sy = std::sin(mYaw), cp = std::cos(mPitch), sp = std::sin(mPitch);
    const glm::mat3 rot = glm::mat3(1, 0, 0, 0, cp, sp, 0, -sp, cp) * glm::mat3(cy, 0, -sy, 0, 1, 0, sy, 0, cy);
    const float dist = mRadius / std::sin(kFpFov * 0.5f) * mZoom;
    const float focal = 0.5f * float(std::min(w, h)) / std::tan(kFpFov * 0.5f);
    const glm::vec3 light = glm::normalize(glm::vec3(-0.3f, 0.5f, 1.0f));
    const QColor base = themeColor("text");

    std::vector<float> depth(size_t(w) * h, 0.0f); // 1/z, larger is nearer
    auto* pixels = reinterpret_cast<QRgb*>(mImage.bits());
    const int stride = int(mImage.bytesPerLine() / 4);
    for (size_t t = 0; t + 2 < mCorners.size(); t += 3)
    {
        glm::vec3 v[3], s[3];
        bool behind = false;
        for (int i = 0; i < 3; ++i)
        {
            v[i] = rot * (mCorners[t + i] - mCenter);
            const float z = dist - v[i].z;
            behind |= z < mRadius * 0.01f;
            s[i] = glm::vec3(0.5f * w + focal * v[i].x / z, 0.5f * h - focal * v[i].y / z, 1.0f / z);
        }
        if (behind)
            continue;
        const glm::vec3 n = glm::cross(v[1] - v[0], v[2] - v[0]);
        const float len = glm::length(n);
        if (len <= 0.0f)
            continue;
        const float shade = 0.25f + 0.75f * std::abs(glm::dot(n / len, light));
        const QRgb color = qRgb(int(base.red() * shade), int(base.green() * shade), int(base.blue() * shade));
        const float area = (s[1].x - s[0].x) * (s[2].y - s[0].y) - (s[2].x - s[0].x) * (s[1].y - s[0].y);
        if (std::abs(area) < 1e-12f)
            continue;
        const int x0 = std::max(0, int(std::floor(std::min({s[0].x, s[1].x, s[2].x}))));
        const int x1 = std::min(w - 1, int(std::ceil(std::max({s[0].x, s[1].x, s[2].x}))));
        const int y0 = std::max(0, int(std::floor(std::min({s[0].y, s[1].y, s[2].y}))));
        const int y1 = std::min(h - 1, int(std::ceil(std::max({s[0].y, s[1].y, s[2].y}))));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const float px = x + 0.5f, py = y + 0.5f;
                const float b0 = ((s[1].x - px) * (s[2].y - py) - (s[2].x - px) * (s[1].y - py)) / area;
                const float b1 = ((s[2].x - px) * (s[0].y - py) - (s[0].x - px) * (s[2].y - py)) / area;
                const float b2 = 1.0f - b0 - b1;
                if (b0 < 0.0f || b1 < 0.0f || b2 < 0.0f)
                    continue;
                const float z = b0 * s[0].z + b1 * s[1].z + b2 * s[2].z;
                float& d = depth[size_t(y) * w + x];
                if (z <= d)
                    continue;
                d = z;
                pixels[y * stride + x] = color;
            }
    }
}

void FilePreview::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), palette().color(QPalette::Base));
    if (mIsImage)
    {
        const QRect area = rect().adjusted(4, 4, -4, mMessage.isEmpty() ? -4 : -4 - fontMetrics().height());
        if (!mThumb.isNull() && area.isValid())
        {
            const QSizeF fitted = QSizeF(mThumb.size()).scaled(QSizeF(area.size()).boundedTo(mThumb.size()), Qt::KeepAspectRatio);
            const QRectF target(area.center().x() - fitted.width() / 2.0, area.top() + (area.height() - fitted.height()) / 2.0,
                                fitted.width(), fitted.height());
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(target, mThumb);
        }
        if (!mMessage.isEmpty())
        {
            p.setPen(themeColor("dimText"));
            p.drawText(rect().adjusted(6, 4, -6, -4), mThumb.isNull() ? Qt::AlignCenter : Qt::AlignHCenter | Qt::AlignBottom, mMessage);
        }
        return;
    }
    if (mDirty)
        rasterize();
    p.drawImage(0, 0, mImage);
    if (!mMessage.isEmpty())
    {
        p.setPen(themeColor("dimText"));
        p.drawText(rect().adjusted(6, 4, -6, -4), mCorners.empty() ? Qt::AlignCenter : Qt::AlignLeft | Qt::AlignBottom, mMessage);
    }
}

void FilePreview::mousePressEvent(QMouseEvent* event) { mLastMouse = event->position().toPoint(); }

void FilePreview::mouseMoveEvent(QMouseEvent* event)
{
    if (mIsImage || !(event->buttons() & Qt::LeftButton))
        return;
    const QPoint d = event->position().toPoint() - mLastMouse;
    mLastMouse = event->position().toPoint();
    mYaw += d.x() * 0.01f;
    mPitch = std::clamp(mPitch + d.y() * 0.01f, -1.55f, 1.55f);
    mDirty = true;
    update();
}

void FilePreview::mouseDoubleClickEvent(QMouseEvent*)
{
    if (!mIsImage)
        resetView();
}

void FilePreview::wheelEvent(QWheelEvent* event)
{
    if (mIsImage)
        return;
    mZoom = std::clamp(mZoom * std::pow(0.999f, float(event->angleDelta().y())), 0.2f, 5.0f);
    mDirty = true;
    update();
}

void FilePreview::resizeEvent(QResizeEvent*) { mDirty = true; }
} // namespace lila::ui
