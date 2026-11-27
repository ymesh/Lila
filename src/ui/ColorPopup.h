#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <QColor>
#include <QPoint>
#include <QRectF>
#include <QString>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class QMouseEvent;
class QPaintEvent;
class QToolButton;
class QWidget;

/**
 * Lila's colour picker: a hue ring around a saturation/value square, HSV, RGB and OKLCh fields in linear or sRGB, a
 * hex field, a screen-wide eyedropper, recent colours, saved palettes and shade strips. It pops up at a point, edits
 * report live, a click outside or Enter keeps them, Esc (or a click on the old swatch) puts the colour back.
 */
namespace lila::ui::colorPopup
{
struct Value
{
    glm::dvec3 color{1.0}; // linear, may exceed 1
    double alpha = 1.0;
    bool useTemperature = false;
    double temperature = 6500.0; // kelvin

    bool operator==(const Value&) const = default;
};

struct Options
{
    Value value;
    /** Shows the temperature row (a light's kelvin). */
    bool temperature = false;
    /** Shows the alpha row and takes #RRGGBBAA. */
    bool alpha = false;
    QString title;
    /** Every edit, live. */
    std::function<void(const Value&)> changed;
    /** Once, when it closes: accepted is false for Esc, and value is then the one it opened with. */
    std::function<void(const Value& value, bool accepted)> closed;
};

/** Opens the popup with its top left near globalPos, kept on screen. Closes any other open one first (accepting it). */
LILA_UI_API void show(QPoint globalPos, Options options);

/** Opens the popup and waits for it to close; nullopt when cancelled. changed still reports live. */
LILA_UI_API std::optional<Value> exec(QPoint globalPos, Options options);

/**
 * The QColorDialog::getColor shape for code holding sRGB QColors: an invalid QColor on cancel, or when anchor was
 * deleted while the popup was open. changed reports live, stops once anchor is gone, and gets the initial colour back
 * on cancel.
 */
LILA_UI_API QColor getColor(const QColor& initial, QWidget* anchor, const QString& title = {}, bool alpha = false,
                            std::function<void(const QColor&)> changed = {});

/** Samples a colour from anywhere on screen (a frozen overlay with a magnifier; Esc cancels): linear, nullopt on cancel. */
LILA_UI_API void pickFromScreen(std::function<void(std::optional<glm::dvec3> linear)> done);

/** An eyedropper icon button that samples the screen and hands over the linear colour. */
LILA_UI_API QToolButton* eyedropperButton(QWidget* parent, std::function<void(const glm::dvec3& linear)> picked);

/**
 * A brightness slider for a colour row. A drag scales the colour it had at the press, so hue and saturation survive a
 * pass through black, and a colour above 1 pins the handle at the end without being clamped. The track runs from black
 * to the colour at full brightness, spaced in sRGB.
 */
class LILA_UI_API ValueSlider final : public QWidget
{
public:
    explicit ValueSlider(QWidget* parent = nullptr);

    /** Shows a linear colour; ignored while a drag is under way. */
    void setColor(const glm::dvec3& linear);

    /** Each move of a drag, with the colour it makes. */
    std::function<void(const glm::dvec3& linear)> dragged;
    /** The end of a drag: the colour at the press and the one it ended on. */
    std::function<void(const glm::dvec3& start, const glm::dvec3& result)> released;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    QRectF track() const;
    glm::dvec3 colorAt(double x) const;

    glm::dvec3 mColor{0.0};
    glm::dvec3 mStart{0.0};
    bool mDragging = false;
};

/**
 * The swatch with an eyedropper button beside it, as one widget to put where the swatch went. A value slider, when
 * given, follows and takes the row's spare width.
 */
LILA_UI_API QWidget* withEyedropper(QWidget* swatch, std::function<void(const glm::dvec3& linear)> picked,
                                    ValueSlider* slider = nullptr);

/** Where a popup opened from anchor goes: under it for a button-sized widget, otherwise beside the cursor. */
LILA_UI_API QPoint placeFor(QWidget* anchor);
} // namespace lila::ui::colorPopup

namespace lila::ui::colorMath
{
/** sRGB transfer, extended above 1 so HDR values survive the round trip. */
LILA_UI_API double toSrgb(double linear);
LILA_UI_API double toLinear(double srgb);
LILA_UI_API glm::dvec3 toSrgb(const glm::dvec3& linear);
LILA_UI_API glm::dvec3 toLinear(const glm::dvec3& srgb);
/** A linear colour as an 8-bit sRGB QColor (clamped), and back. */
LILA_UI_API QColor display(const glm::dvec3& linear, double alpha = 1.0);
LILA_UI_API glm::dvec3 linear(const QColor& srgb);

/** h, s in 0..1; v unbounded. Hue and saturation fall back to the given ones where they are undefined (grey, black). */
LILA_UI_API glm::dvec3 rgbToHsv(const glm::dvec3& rgb, double fallbackHue = 0.0, double fallbackSat = 0.0);
LILA_UI_API glm::dvec3 hsvToRgb(const glm::dvec3& hsv);

LILA_UI_API glm::dvec3 linearToOklab(const glm::dvec3& linear);
LILA_UI_API glm::dvec3 oklabToLinear(const glm::dvec3& lab);
/** L 0..1, C 0..~0.37, h in degrees. */
LILA_UI_API glm::dvec3 linearToOklch(const glm::dvec3& linear, double fallbackHue = 0.0);
/** Chroma is reduced until the colour fits sRGB (above 1 only where L is). */
LILA_UI_API glm::dvec3 oklchToLinear(const glm::dvec3& lch);

/** #RRGGBB (or #RRGGBBAA) of the sRGB encoding, clamped to 0..1. */
LILA_UI_API QString hex(const glm::dvec3& linear, double alpha = 1.0, bool withAlpha = false);
/** #RGB, #RGBA, #RRGGBB, #RRGGBBAA, with or without '#' or 0x: linear rgb and alpha. */
LILA_UI_API std::optional<glm::dvec4> parseHex(QString text);
} // namespace lila::ui::colorMath

/** Recent colours and saved palettes, in the "colors" prefs file (so studio folders can ship palettes). */
namespace lila::ui::colorLibrary
{
struct Swatch
{
    glm::dvec3 color{0.0}; // linear
    double alpha = 1.0;
    std::string name;

    bool operator==(const Swatch&) const = default;
};

struct Palette
{
    std::string name;
    std::vector<Swatch> swatches;
};

inline constexpr size_t kHistorySize = 13;

/** Newest first. */
LILA_UI_API std::vector<Swatch> history();
/** Moves the colour to the front of the history. */
LILA_UI_API void remember(const Swatch& swatch);
LILA_UI_API void clearHistory();

/** The user's palettes, or a starter one when none are saved. */
LILA_UI_API std::vector<Palette> palettes();
LILA_UI_API void setPalettes(const std::vector<Palette>& palettes);
/** The palette Lila ships ("Basic"). */
LILA_UI_API Palette builtInPalette();
/** Drops the user's palettes, leaving the studio's or the built-in one. */
LILA_UI_API void resetPalettes();
LILA_UI_API std::string currentPalette();
LILA_UI_API void setCurrentPalette(const std::string& name);

/** GIMP .gpl, the palette format Krita, Inkscape, Aseprite and GIMP share. */
LILA_UI_API std::optional<Palette> readGpl(const std::filesystem::path& file, std::string* error = nullptr);
LILA_UI_API bool writeGpl(const std::filesystem::path& file, const Palette& palette);
} // namespace lila::ui::colorLibrary
