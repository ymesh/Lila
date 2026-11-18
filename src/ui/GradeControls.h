#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <QWidget>

#include <functional>
#include <vector>

/**
 * Grading controls in the Controls.h vocabulary: a colour wheel (a hue disc with a puck for a balance shift, a master
 * value under it) and a curve editor (points on a 0..1 square).
 */
namespace lila::ui::controls
{
/**
 * A colour balance wheel. The puck's direction is the hue pushed toward, its distance how far; the shift it stands for
 * has no luminance of its own (the master field below moves that). Double-click puts the puck back in the middle.
 */
class LILA_UI_API ColorWheel : public QWidget
{
    Q_OBJECT

public:
    /** reach: the shift at the rim, per channel. */
    explicit ColorWheel(const QString& title, double reach, QWidget* parent = nullptr);
    /** The balance shift shown: a colour with its luminance taken out. */
    void setShift(const glm::dvec3& shift);
    glm::dvec3 shift() const { return mShift; }
    QSize sizeHint() const override { return {96, 112}; }
    QSize minimumSizeHint() const override { return {72, 88}; }

    /** The luminance-free chroma of a colour offset from neutral: what setShift shows of it. */
    static glm::dvec3 chroma(const glm::dvec3& c);

Q_SIGNALS:
    /** final is false while dragging, true on release (and on reset). */
    void shiftChanged(glm::dvec3 shift, bool final);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    QRectF discRect() const;
    void setFromPoint(QPointF p, bool final);

    QString mTitle;
    double mReach;
    glm::dvec3 mShift{0.0};
    bool mDragging = false;
};

/**
 * Points on a curve over a 0..1 square: click empty space to add a point, drag to move, double-click a point to remove
 * it. The first and last points stay at the ends unless periodic (hue curves). neutral(x) draws the reference line.
 */
class LILA_UI_API CurveEditor : public QWidget
{
    Q_OBJECT

public:
    explicit CurveEditor(QWidget* parent = nullptr);
    void setPoints(std::vector<glm::vec2> points, bool periodic, float neutralLevel, const QColor& stroke, bool hueBackground);
    const std::vector<glm::vec2>& points() const { return mPoints; }
    /** How the curve between points is drawn: the same evaluation the grade uses. */
    void setEvaluator(std::function<float(const std::vector<glm::vec2>&, float)> fn) { mEval = std::move(fn); }
    QSize sizeHint() const override { return {240, 180}; }
    QSize minimumSizeHint() const override { return {160, 120}; }

Q_SIGNALS:
    void pointsChanged(std::vector<glm::vec2> points, bool final);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    QRectF plot() const;
    QPointF toScreen(glm::vec2 p) const;
    glm::vec2 fromScreen(QPointF p) const;
    int pointAt(QPointF p) const;
    /** The neutral line's points, so a first click bends a straight curve rather than replacing it. */
    std::vector<glm::vec2> seeded() const;

    std::vector<glm::vec2> mPoints;
    std::function<float(const std::vector<glm::vec2>&, float)> mEval;
    bool mPeriodic = false;
    float mNeutral = -1.0f; // the neutral value for flat curves; negative for the diagonal
    QColor mStroke;
    bool mHueBackground = false;
    int mDrag = -1;
};
} // namespace lila::ui::controls
