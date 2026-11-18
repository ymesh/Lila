#include "ui/ScopesPanel.h"

#include "core/Log.h"
#include "ui/AgentCapture.h"
#include "ui/Controls.h"
#include "ui/Panels.h"
#include "ui/RenderOutput.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QPainter>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>

namespace lila::ui
{
namespace
{
enum Scope
{
    kHistogram,
    kParade,
    kWaveform,
    kVectorscope
};

/** Pixels a scope reads: the picture shrunk to about this many across. */
constexpr int kSampleWidth = 384;

/** Draws one scope of a picture's display-encoded pixels. */
class ScopeView : public QWidget
{
public:
    explicit ScopeView(QWidget* parent) : QWidget(parent)
    {
        setObjectName("scopeView");
        setMinimumSize(200, 140);
    }

    void setImage(const QImage& img)
    {
        mImage = img.convertToFormat(QImage::Format_RGB32);
        rebuild();
    }

    void setScope(int scope)
    {
        mScope = scope;
        rebuild();
    }

    /** The trace image, for checks: how much of each column lit, by channel. */
    const QImage& trace() const { return mTrace; }

protected:
    void resizeEvent(QResizeEvent*) override { rebuild(); }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), themeColor("darkest"));
        p.setPen(QPen(themeColor("border"), 1.0));
        if (mScope == kVectorscope)
        {
            const QRectF r = vectorRect();
            p.drawEllipse(r);
            p.drawLine(QPointF(r.center().x(), r.top()), QPointF(r.center().x(), r.bottom()));
            p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
            p.drawImage(r, mTrace);
            // Targets for the six primaries and secondaries at 75%.
            p.setPen(QPen(themeColor("dimText"), 1.0));
            const char* names[] = {"R", "Mg", "B", "Cy", "G", "Yl"};
            const float targets[][3] = {{0.75f, 0, 0}, {0.75f, 0, 0.75f}, {0, 0, 0.75f}, {0, 0.75f, 0.75f}, {0, 0.75f, 0}, {0.75f, 0.75f, 0}};
            for (int i = 0; i < 6; ++i)
            {
                const QPointF q = vectorPoint(targets[i][0], targets[i][1], targets[i][2], r);
                p.drawRect(QRectF(q.x() - 4, q.y() - 4, 8, 8));
                p.drawText(q + QPointF(6, 4), names[i]);
            }
            return;
        }
        const QRectF r = rect().adjusted(4, 4, -4, -4);
        for (int i = 1; i < 4; ++i)
            p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 4.0), QPointF(r.right(), r.top() + r.height() * i / 4.0));
        p.drawImage(r, mTrace);
    }

private:
    QRectF vectorRect() const
    {
        const double side = std::min(width(), height()) - 8.0;
        return {(width() - side) / 2.0, (height() - side) / 2.0, side, side};
    }

    /** Where a colour lands on the vectorscope (Rec.709 Cb right, Cr up). */
    static QPointF vectorPoint(float r, float g, float b, const QRectF& area)
    {
        const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        const float cb = (b - y) / 1.8556f, cr = (r - y) / 1.5748f;
        return {area.center().x() + cb * area.width(), area.center().y() - cr * area.height()};
    }

    void rebuild()
    {
        if (mImage.isNull())
        {
            mTrace = QImage();
            update();
            return;
        }
        const int w = mImage.width(), h = mImage.height();
        if (mScope == kVectorscope)
        {
            const int n = 192;
            std::vector<float> acc(size_t(n) * n);
            for (int y = 0; y < h; ++y)
            {
                const QRgb* line = reinterpret_cast<const QRgb*>(mImage.constScanLine(y));
                for (int x = 0; x < w; ++x)
                {
                    const QPointF q = vectorPoint(qRed(line[x]) / 255.0f, qGreen(line[x]) / 255.0f, qBlue(line[x]) / 255.0f, QRectF(0, 0, n, n));
                    const int ix = std::clamp(int(q.x()), 0, n - 1), iy = std::clamp(int(q.y()), 0, n - 1);
                    acc[size_t(iy) * n + ix] += 1.0f;
                }
            }
            mTrace = QImage(n, n, QImage::Format_ARGB32);
            const float even = float(std::max(w * h, 1)) / float(n * n / 8);
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                {
                    const float v = 1.0f - std::exp(-acc[size_t(y) * n + x] / even);
                    mTrace.setPixel(x, y, qRgba(int(140 * v), int(230 * v), int(140 * v), int(255 * v)));
                }
            update();
            return;
        }
        const int levels = 256;
        if (mScope == kHistogram)
        {
            std::array<std::array<float, 256>, 4> bins{};
            for (int y = 0; y < h; ++y)
            {
                const QRgb* line = reinterpret_cast<const QRgb*>(mImage.constScanLine(y));
                for (int x = 0; x < w; ++x)
                {
                    const int r = qRed(line[x]), g = qGreen(line[x]), b = qBlue(line[x]);
                    bins[0][r] += 1;
                    bins[1][g] += 1;
                    bins[2][b] += 1;
                    bins[3][std::clamp(int(0.2126f * r + 0.7152f * g + 0.0722f * b + 0.5f), 0, 255)] += 1;
                }
            }
            float peak = 1.0f;
            for (const auto& c : bins)
                for (int i = 1; i < 255; ++i)
                    peak = std::max(peak, c[size_t(i)]);
            const int th = 128;
            mTrace = QImage(levels, th, QImage::Format_ARGB32);
            mTrace.fill(Qt::transparent);
            for (int i = 0; i < levels; ++i)
                for (int y = 0; y < th; ++y)
                {
                    const float level = float(th - y) / float(th);
                    int rgb[3] = {0, 0, 0};
                    for (int c = 0; c < 3; ++c)
                        if (bins[size_t(c)][size_t(i)] / peak >= level)
                            rgb[c] = 200;
                    const bool luma = bins[3][size_t(i)] / peak >= level;
                    if (rgb[0] || rgb[1] || rgb[2] || luma)
                        mTrace.setPixel(i, y, qRgba(std::max(rgb[0], luma ? 120 : 0), std::max(rgb[1], luma ? 120 : 0), std::max(rgb[2], luma ? 120 : 0), 200));
                }
            update();
            return;
        }
        // Waveform (luminance) and parade (red, green, blue side by side): level up, picture column across.
        const int cols = mScope == kParade ? 3 * 128 : std::min(w, 384);
        const int th = 128;
        std::vector<float> acc(size_t(cols) * th * 3);
        for (int y = 0; y < h; ++y)
        {
            const QRgb* line = reinterpret_cast<const QRgb*>(mImage.constScanLine(y));
            for (int x = 0; x < w; ++x)
            {
                const float c[3] = {qRed(line[x]) / 255.0f, qGreen(line[x]) / 255.0f, qBlue(line[x]) / 255.0f};
                if (mScope == kParade)
                    for (int k = 0; k < 3; ++k)
                    {
                        const int col = k * 128 + x * 128 / w;
                        const int row = std::clamp(int((1.0f - c[k]) * (th - 1) + 0.5f), 0, th - 1);
                        acc[(size_t(row) * cols + col) * 3 + k] += 1.0f;
                    }
                else
                {
                    const float yv = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
                    const int col = x * cols / w;
                    const int row = std::clamp(int((1.0f - yv) * (th - 1) + 0.5f), 0, th - 1);
                    for (int k = 0; k < 3; ++k)
                        acc[(size_t(row) * cols + col) * 3 + k] += 1.0f;
                }
            }
        }
        // Hits per cell against what one column of the picture spread evenly over the levels would give.
        const float even = float(std::max(w * h, 1)) / float(cols * th);
        auto glow = [even](float hits) { return 1.0f - std::exp(-hits / (even * 1.5f)); };
        mTrace = QImage(cols, th, QImage::Format_ARGB32);
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < cols; ++x)
            {
                const float* a = acc.data() + (size_t(y) * cols + x) * 3;
                const float v[3] = {glow(a[0]), glow(a[1]), glow(a[2])};
                const float m = std::max({v[0], v[1], v[2]});
                mTrace.setPixel(x, y, qRgba(int(230 * v[0]), int(230 * v[1]), int(230 * v[2]), int(255 * m)));
            }
        update();
    }

    QImage mImage, mTrace;
    int mScope = kWaveform;
};

/** Scopes of the active viewport's finished picture (grade and view included), or of the last render. */
class ScopesPanel : public QWidget
{
public:
    explicit ScopesPanel(QWidget* parent) : QWidget(parent)
    {
        setObjectName("scopesPanel");
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(4, 4, 4, 4);
        auto* bar = new QHBoxLayout;
        auto* pick = new controls::Segmented(this);
        pick->add("Histogram", {}, "Counts of each level, red, green, blue and luminance", kHistogram);
        pick->add("Parade", {}, "Red, green and blue levels side by side, across the picture", kParade);
        pick->add("Waveform", {}, "Luminance levels across the picture", kWaveform);
        pick->add("Vector", {}, "Hue and saturation: angle and distance from the centre", kVectorscope);
        pick->setCurrent(kWaveform);
        mSource = new QComboBox(this);
        mSource->addItem("Viewport", 0);
        mSource->addItem("Last Render", 1);
        mSource->setToolTip("What the scopes read: the active viewport's picture, or the last frame a render wrote.");
        bar->addWidget(pick, 1);
        bar->addWidget(mSource);
        v->addLayout(bar);
        mView = new ScopeView(this);
        v->addWidget(mView, 1);
        connect(pick, &controls::Segmented::picked, this, [this](int s) { mView->setScope(s); });
        connect(mSource, &QComboBox::activated, this, [this] { refresh(); });
        mTimer = new QTimer(this);
        mTimer->setInterval(250);
        connect(mTimer, &QTimer::timeout, this, [this] { refresh(); });
        mView->setScope(kWaveform);
    }

protected:
    void showEvent(QShowEvent*) override
    {
        mTimer->start();
        refresh();
    }
    void hideEvent(QHideEvent*) override { mTimer->stop(); }

private:
    void refresh()
    {
        QImage img;
        if (mSource->currentData().toInt() == 1)
            img = renderOutput::lastImage();
        else
            try
            {
                capture::Options o;
                o.view = "active";
                o.width = kSampleWidth;
                o.height = kSampleWidth * 9 / 16;
                o.display = "shaded";
                o.frame = false;
                o.showSelection = false;
                o.label = false;
                img = capture::render(o);
            }
            catch (const std::exception& e)
            {
                log::debug("scopes: {}", e.what());
            }
        if (!img.isNull() && img.width() > kSampleWidth)
            img = img.scaledToWidth(kSampleWidth, Qt::SmoothTransformation);
        mView->setImage(img);
    }

    QComboBox* mSource = nullptr;
    ScopeView* mView = nullptr;
    QTimer* mTimer = nullptr;
};
} // namespace

void registerScopesPanel()
{
    panels::add({.id = "scopesDock",
                 .title = "Scopes",
                 .create = [](QWidget* p) -> QWidget* { return new ScopesPanel(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "chart-histogram",
                 .open = false,
                 .toggleAction = "window.scopes",
                 .keywords = {"scopes", "waveform", "vectorscope", "histogram", "parade", "resolve", "nuke", "grading", "colour"},
                 .floatFirst = true,
                 .floatWidth = 520,
                 .floatHeight = 320});
}
} // namespace lila::ui
