#pragma once

#include "core/Export.h"

#include <QDoubleSpinBox>
#include <QToolButton>
#include <QWidget>

#include <functional>
#include <string>
#include <vector>

class QButtonGroup;
class QHBoxLayout;
class QLabel;
class QScrollArea;

/**
 * The control vocabulary every panel is built from. Each control carries a "role" property the app style sheet styles
 * (Theme.cpp), so the look lives in one place:
 *
 *   section header   text with a chevron, no filled bar; sections are split by a hairline rule
 *   tool tile        icon over label, no fill at rest, for a toolkit's daily actions
 *   push button      label on the raised fill, for weekly actions
 *   split button     a push button whose arrow picks a variant to run
 *   overflow         "... N more", a menu of the rare actions
 *   segmented        an exclusive choice as one joined strip, the active segment the brightest fill
 *   value field      a drag field one step above the panel, numbers right-aligned, unit inside
 *   slider field     a value field with a fill showing where a bounded number sits in its range
 *   vector field     three value fields in a row, each with its axis letter in the axis colour
 *   cycle button     a button stepping through states, left click forward, right click back
 *
 * Buttons that run an action with an option box get the corner glyph (optionBox::withOptionBox).
 */
namespace lila::ui::controls
{
/** A collapsible section: `content` under a text header. Collapsed state is remembered under prefsKey + "/" + title. */
LILA_UI_API QWidget* section(const QString& title, QWidget* content, QWidget* parent, const char* prefsKey, bool openByDefault = true,
                             QToolButton** headerOut = nullptr);

/** A tile running the action: icon over label. The label defaults to the action's own. */
LILA_UI_API QToolButton* tile(const std::string& actionId, const char* iconName, QWidget* parent, const QString& label = {});

/** A labelled button running the action. */
LILA_UI_API QToolButton* push(const std::string& actionId, QWidget* parent, const QString& label = {});

/** A button running `actionId`, whose arrow lists `variants` (action ids) to run instead. */
LILA_UI_API QToolButton* split(const std::string& actionId, const std::vector<std::string>& variants, QWidget* parent, const QString& label = {});

/** "... N more": a menu of the actions, with their option boxes. Hidden when the list is empty. */
LILA_UI_API QToolButton* overflow(const std::vector<std::string>& actionIds, QWidget* parent);

/** A grid of tiles or buttons, `columns` wide, 2 px apart. */
LILA_UI_API QWidget* grid(const std::vector<QWidget*>& items, int columns, QWidget* parent);

/** An exclusive choice drawn as one strip. */
class LILA_UI_API Segmented : public QWidget
{
    Q_OBJECT

public:
    explicit Segmented(QWidget* parent = nullptr);
    /** Adds a segment with text or an icon (never both in one control), returning its button. */
    QToolButton* add(const QString& text, const QString& iconName, const QString& tip, int id);
    void setCurrent(int id);
    /** No segment current, for a mixed selection. */
    void clearCurrent();
    int current() const;

Q_SIGNALS:
    void picked(int id);

private:
    void restyle();

    QHBoxLayout* mRow;
    QButtonGroup* mGroup;
};

/** A number field: right-aligned, drag to scrub, unit as a suffix, no step buttons. */
LILA_UI_API QDoubleSpinBox* valueField(double lo, double hi, int decimals, const QString& unit, QWidget* parent);

/** A bounded number with a fill showing its place between minimum and maximum. */
class LILA_UI_API SliderField : public QDoubleSpinBox
{
public:
    explicit SliderField(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* e) override;
};

/** A value field marked with its axis letter, in the axis colour, inside its left edge. */
LILA_UI_API QDoubleSpinBox* axisField(char axis, double lo, double hi, int decimals, const QString& unit, QWidget* parent);

/** Three axis fields in a row, equal widths. */
LILA_UI_API QWidget* vectorField(std::vector<QDoubleSpinBox*>& out, double lo, double hi, int decimals, const QString& unit, QWidget* parent);

/**
 * Gives a number field preset values: a chevron inside its right edge lists them, and the wheel, arrow keys and
 * middle-drag step from one to the next instead of by the single step. Any other value can still be typed.
 */
LILA_UI_API void setPresets(QAbstractSpinBox* box, std::vector<double> values);

/** Moves the field `steps` presets up (down when negative) from its value; false when it has no presets. */
LILA_UI_API bool stepPresets(QAbstractSpinBox* box, int steps);

/** Keeps the scroll area at least as wide as its content needs, so a narrow dock never clips a row of tiles. */
LILA_UI_API void fitScrollWidth(QScrollArea* scroll);

/**
 * Makes a click-to-cycle button step both ways: left click calls `step(+1)`, right click `step(-1)`. Use it for every
 * button that cycles through states, so right click always goes back.
 */
LILA_UI_API void cycle(QAbstractButton* button, std::function<void(int)> step);

/** Sets a role property and re-polishes, for sheet rules keyed on it. */
LILA_UI_API void setRole(QWidget* w, const char* role);
} // namespace lila::ui::controls
