#include "ui/GradeControls.h"

#include "ui/Theme.h"

#include <QConicalGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lila::ui::controls
{
namespace
{
constexpr glm::dvec3 kLuma{0.2126, 0.7152, 0.0722};

/** The fully saturated colour at a hue angle (radians, 0 red, counter-clockwise on screen). */
glm::dvec3 hueColor(double angle)
{
    const double h = std::fmod(angle / (2.0 * std::numbers::pi) + 1.0, 1.0) * 6.0;
    const double x = 1.0 - std::abs(std::fmod(h, 2.0) - 1.0);
    switch (int(h))
    {
    case 0: return {1, x, 0};
    case 1: return {x, 1, 0};
    case 2: return {0, 1, x};
    case 3: return {0, x, 1};
    case 4: return {x, 0, 1};
    default: return {1, 0, x};
    }
}

/** A hue direction's chroma at unit length, so a puck at the rim shifts by exactly the wheel's reach. */
glm::dvec3 unitChroma(double angle)
{
    glm::dvec3 c = ColorWheel::chroma(hueColor(angle));
    const double len = glm::length(c);
    return len > 1e-9 ? c / len : c;
}
} // namespace

ColorWheel::ColorWheel(const QString& title, double reach, QWidget* parent) : QWidget(parent), mTitle(title), mReach(reach)
{
    setMinimumSize(72, 92);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setToolTip(title + ": drag the puck toward a colour; double-click to reset.");
}

glm::dvec3 ColorWheel::chroma(const glm::dvec3& c)
{
    return c - glm::dvec3(glm::dot(c, kLuma));
}

void ColorWheel::setShift(const glm::dvec3& shift)
{
    mShift = chroma(shift);
    update();
}

QRectF ColorWheel::discRect() const
{
    const double label = fontMetrics().height() + 4.0;
    const double side = std::max(10.0, std::min(double(width()), double(height()) - label) - 6.0);
    return {(width() - side) / 2.0, label + (height() - label - side) / 2.0, side, side};
}

void ColorWheel::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(themeColor(isEnabled() ? "text" : "disabled"));
    p.drawText(QRectF(0, 0, width(), fontMetrics().height() + 2), Qt::AlignHCenter | Qt::AlignTop, mTitle);
    const QRectF r = discRect();
    // Hue around, washed to grey toward the centre, muted so the wheel never outshouts the viewport.
    QConicalGradient hue(r.center(), 0.0);
    for (int i = 0; i <= 6; ++i)
    {
        const glm::dvec3 c = hueColor(2.0 * std::numbers::pi * i / 6.0);
        hue.setColorAt(i / 6.0, QColor::fromRgbF(float(0.35 + 0.4 * c.r), float(0.35 + 0.4 * c.g), float(0.35 + 0.4 * c.b)));
    }
    p.setPen(Qt::NoPen);
    p.setBrush(hue);
    p.drawEllipse(r);
    QRadialGradient wash(r.center(), r.width() / 2.0);
    QColor grey = themeColor("altBase");
    wash.setColorAt(0.0, grey);
    grey.setAlphaF(0.0);
    wash.setColorAt(1.0, grey);
    p.setBrush(wash);
    p.drawEllipse(r);
    p.setPen(QPen(themeColor("border"), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(r);
    p.drawLine(QPointF(r.center().x() - 4, r.center().y()), QPointF(r.center().x() + 4, r.center().y()));
    p.drawLine(QPointF(r.center().x(), r.center().y() - 4), QPointF(r.center().x(), r.center().y() + 4));
    // The puck: the shift's hue as an angle, its size against the reach as a distance.
    const double len = glm::length(mShift);
    QPointF puck = r.center();
    if (len > 1e-9)
    {
        double best = 0.0, bestDot = -2.0;
        for (int i = 0; i < 360; ++i)
        {
            const double a = 2.0 * std::numbers::pi * i / 360.0;
            const double d = glm::dot(unitChroma(a), mShift / len);
            if (d > bestDot)
            {
                bestDot = d;
                best = a;
            }
        }
        const double dist = std::min(len / mReach, 1.0) * r.width() / 2.0;
        puck += QPointF(std::cos(best) * dist, -std::sin(best) * dist);
    }
    p.setPen(QPen(themeColor("brightText"), 1.5));
    p.setBrush(themeColor(len > 1e-6 ? "highlight" : "button"));
    p.drawEllipse(puck, 5.0, 5.0);
}

void ColorWheel::setFromPoint(QPointF pt, bool final)
{
    const QRectF r = discRect();
    const QPointF d = pt - r.center();
    const double radius = r.width() / 2.0;
    double dist = std::min(std::hypot(d.x(), d.y()) / radius, 1.0);
    const double angle = std::atan2(-d.y(), d.x());
    mShift = dist < 0.03 ? glm::dvec3(0.0) : unitChroma(angle) * dist * mReach;
    update();
    Q_EMIT shiftChanged(mShift, final);
}

void ColorWheel::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    mDragging = true;
    setFromPoint(e->position(), false);
}

void ColorWheel::mouseMoveEvent(QMouseEvent* e)
{
    if (mDragging)
        setFromPoint(e->position(), false);
}

void ColorWheel::mouseReleaseEvent(QMouseEvent* e)
{
    if (!mDragging)
        return;
    mDragging = false;
    setFromPoint(e->position(), true);
}

void ColorWheel::mouseDoubleClickEvent(QMouseEvent*)
{
    mDragging = false;
    mShift = glm::dvec3(0.0);
    update();
    Q_EMIT shiftChanged(mShift, true);
}

CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(160, 150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setToolTip("Click to add a point, drag to move it, double-click a point to remove it.");
}

void CurveEditor::setPoints(std::vector<glm::vec2> points, bool periodic, float neutralLevel, const QColor& stroke, bool hueBackground)
{
    mPoints = std::move(points);
    mPeriodic = periodic;
    mNeutral = neutralLevel;
    mStroke = stroke;
    mHueBackground = hueBackground;
    mDrag = -1;
    update();
}

QRectF CurveEditor::plot() const
{
    return QRectF(6, 6, width() - 12, height() - 12);
}

QPointF CurveEditor::toScreen(glm::vec2 p) const
{
    const QRectF r = plot();
    return {r.left() + p.x * r.width(), r.bottom() - p.y * r.height()};
}

glm::vec2 CurveEditor::fromScreen(QPointF p) const
{
    const QRectF r = plot();
    return {std::clamp(float((p.x() - r.left()) / r.width()), 0.0f, 1.0f), std::clamp(float((r.bottom() - p.y()) / r.height()), 0.0f, 1.0f)};
}

int CurveEditor::pointAt(QPointF p) const
{
    for (size_t i = 0; i < mPoints.size(); ++i)
    {
        const QPointF q = toScreen(mPoints[i]);
        if (std::hypot(q.x() - p.x(), q.y() - p.y()) < 7.0)
            return int(i);
    }
    return -1;
}

std::vector<glm::vec2> CurveEditor::seeded() const
{
    if (!mPoints.empty())
        return mPoints;
    if (mNeutral >= 0.0f)
        return {};
    return {{0.0f, 0.0f}, {1.0f, 1.0f}};
}

void CurveEditor::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = plot();
    p.fillRect(r, themeColor("base"));
    if (mHueBackground)
    {
        QLinearGradient g(r.topLeft(), r.topRight());
        for (int i = 0; i <= 6; ++i)
        {
            const double h = i / 6.0;
            QColor c = QColor::fromHsvF(float(std::fmod(h, 1.0)), 0.6f, 0.45f);
            c.setAlphaF(0.35f);
            g.setColorAt(h, c);
        }
        p.fillRect(QRectF(r.left(), r.bottom() - 6, r.width(), 6), g);
    }
    p.setPen(QPen(themeColor("border"), 1.0));
    for (int i = 1; i < 4; ++i)
    {
        const double x = r.left() + r.width() * i / 4.0, y = r.top() + r.height() * i / 4.0;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
    p.drawRect(r);
    // The neutral reference.
    p.setPen(QPen(themeColor("dimText"), 1.0, Qt::DashLine));
    if (mNeutral >= 0.0f)
        p.drawLine(toScreen({0.0f, mNeutral}), toScreen({1.0f, mNeutral}));
    else
        p.drawLine(toScreen({0.0f, 0.0f}), toScreen({1.0f, 1.0f}));
    QPainterPath path;
    const int steps = 96;
    for (int i = 0; i <= steps; ++i)
    {
        const float x = float(i) / steps;
        const float y = mEval ? mEval(mPoints, x) : x;
        const QPointF q = toScreen({x, std::clamp(y, 0.0f, 1.0f)});
        if (i == 0)
            path.moveTo(q);
        else
            path.lineTo(q);
    }
    p.setPen(QPen(mStroke.isValid() ? mStroke : themeColor("text"), 1.6));
    p.drawPath(path);
    p.setPen(QPen(themeColor("brightText"), 1.2));
    for (size_t i = 0; i < mPoints.size(); ++i)
    {
        p.setBrush(int(i) == mDrag ? themeColor("highlight") : themeColor("button"));
        p.drawEllipse(toScreen(mPoints[i]), 4.0, 4.0);
    }
}

void CurveEditor::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    mDrag = pointAt(e->position());
    if (mDrag < 0)
    {
        mPoints = seeded();
        const glm::vec2 q = fromScreen(e->position());
        mPoints.push_back(q);
        std::ranges::sort(mPoints, [](glm::vec2 a, glm::vec2 b) { return a.x < b.x; });
        mDrag = pointAt(e->position());
        Q_EMIT pointsChanged(mPoints, false);
    }
    update();
}

void CurveEditor::mouseMoveEvent(QMouseEvent* e)
{
    if (mDrag < 0 || mDrag >= int(mPoints.size()))
        return;
    glm::vec2 q = fromScreen(e->position());
    // Points keep their order; the ends of a tone curve stay at its ends.
    const float lo = mDrag > 0 ? mPoints[size_t(mDrag) - 1].x + 0.005f : 0.0f;
    const float hi = mDrag + 1 < int(mPoints.size()) ? mPoints[size_t(mDrag) + 1].x - 0.005f : 1.0f;
    q.x = std::clamp(q.x, lo, std::max(lo, hi));
    mPoints[size_t(mDrag)] = q;
    update();
    Q_EMIT pointsChanged(mPoints, false);
}

void CurveEditor::mouseReleaseEvent(QMouseEvent*)
{
    if (mDrag < 0)
        return;
    mDrag = -1;
    update();
    Q_EMIT pointsChanged(mPoints, true);
}

void CurveEditor::mouseDoubleClickEvent(QMouseEvent* e)
{
    const int i = pointAt(e->position());
    if (i < 0)
        return;
    mPoints.erase(mPoints.begin() + i);
    // A tone curve left with its two straight end points is the neutral curve again.
    if (mNeutral < 0.0f && mPoints.size() == 2 && mPoints[0] == glm::vec2(0.0f) && mPoints[1] == glm::vec2(1.0f))
        mPoints.clear();
    mDrag = -1;
    update();
    Q_EMIT pointsChanged(mPoints, true);
}
} // namespace lila::ui::controls
