#pragma once

#include "core/Export.h"
#include "core/ExtensionContext.h"

#include "render/FrameState.h"

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lila::ui
{
struct OverlayAtlasData;

/**
 * Builds a viewport's 2D overlay: thick anti-aliased lines, fills and text, in widget (logical) pixels. Converted to
 * physical pixels for the renderer, so callers never think about DPI.
 */
class LILA_UI_API OverlayPainter
{
public:
    /** Text and icons use the atlas current at the first that needs it; icons registered later show from the next painter on. */
    OverlayPainter(std::vector<render::OverlayVertex>& out, double dpr);
    ~OverlayPainter();

    /** The atlas this painter's text and icons refer to: what the frame's FrameState::atlas must be. */
    std::shared_ptr<const render::OverlayAtlas> atlas() const;

    void line(QPointF a, QPointF b, float width, const glm::vec4& color);
    void dashedLine(QPointF a, QPointF b, float width, float dash, const glm::vec4& color);
    void polyline(const std::vector<QPointF>& pts, float width, const glm::vec4& color, bool closed = false);
    /** A polyline broken into dashes of `dash` pixels by arc length, so curves dash evenly however finely sampled. */
    void dashedPolyline(const std::vector<QPointF>& pts, float width, float dash, const glm::vec4& color, bool closed = false);
    void triangle(QPointF a, QPointF b, QPointF c, const glm::vec4& color);
    /** Convex polygon fill. */
    void fill(const std::vector<QPointF>& pts, const glm::vec4& color);
    void rect(QRectF r, const glm::vec4& color);
    void disc(QPointF center, float radius, const glm::vec4& color, int segments = 20);
    /**
     * A ring segment between radii r0 and r1, from one screen angle to another in degrees (0 right, 90 down), shaded
     * from `inner` at r0 to `outer` at r1. `corner` rounds its four corners (half the thickness gives round ends); every
     * edge fades out over `feather` px so it stays smooth without multisampling. A span of 360 or more is a full ring,
     * and r0 = 0 a disc.
     */
    void sector(QPointF center, double r0, double r1, double fromDeg, double toDeg, const glm::vec4& inner, const glm::vec4& outer,
                double corner = 0.0, float feather = 1.0f);
    /** A feathered arc stroke of `width` px along radius r; round ends when `roundEnds`. */
    void arc(QPointF center, double r, double fromDeg, double toDeg, float width, const glm::vec4& color, bool roundEnds = false);
    /** A straight stroke with feathered sides and square ends. */
    void smoothLine(QPointF a, QPointF b, float width, const glm::vec4& color, float feather = 1.0f);
    /** A feathered filled circle. */
    void smoothDisc(QPointF center, double radius, const glm::vec4& color, float feather = 1.0f);
    /** A soft round shadow: `alpha` black inside radius, fading to nothing over `spread` px beyond it. */
    void softShadow(QPointF center, double radius, double spread, float alpha);

    enum Align
    {
        Left,
        Center,
        Right,
    };
    /** Text with its baseline at `at`. Returns the advance width. */
    float text(QPointF at, const QString& s, const glm::vec4& color, Align align = Left);
    /** Text over a dark rounded plate, for readouts that must stay legible on any background. */
    void label(QPointF at, const QString& s, const glm::vec4& color = {0.95f, 0.95f, 0.95f, 1.0f});
    static float textWidth(const QString& s);
    /** An icon registered with overlayIcons, by id, filling r; false when there is none by that id. */
    bool icon(QRectF r, std::string_view name, const glm::vec4& color);

    /** While on, lines, triangles and text get an under-stroke that contrasts with their own color. */
    void setShadow(bool on) { mShadow = on; }

    /**
     * Which parts of the background are bright: 1 or 0 per cell of cols x rows over a view of the given logical size,
     * top row first, from classifyBackdrop(). Without it the backdrop counts as dark.
     */
    void setBackdrop(std::vector<float> bright, int cols, int rows, QSizeF view);
    /**
     * Turns render::backdropLuminance() cells into setBackdrop()'s bright flags, updating state in place. Around the
     * switch point (where black and white text contrast equally) a cell keeps its previous answer, so orbiting across it
     * never flickers.
     */
    static void classifyBackdrop(const std::vector<float>& luminance, std::vector<float>& state);
    /** Whether most of the backdrop under r is bright; an empty r means the whole view. */
    bool brightBackdrop(QRectF r = {}) const;
    /**
     * A chrome gray that reads on the backdrop under r. shade is the gray as it would be on a dark background (0.85
     * bright, 0.45 dim); over a bright backdrop it comes back as a dark gray, the dim ones a little lighter.
     */
    glm::vec4 ink(float shade, float alpha = 1.0f, QRectF r = {}) const;

private:
    void vertex(QPointF p, glm::vec2 uv, const glm::vec4& color);

    /** The under-stroke for a stroke of color c at alpha a: white under dark grays, black under everything else. */
    static glm::vec4 shadowFor(const glm::vec4& c, float a);

    std::vector<render::OverlayVertex>& mOut;
    double mDpr;
    const OverlayAtlasData& atlasData() const;
    mutable std::shared_ptr<const OverlayAtlasData> mAtlas;
    bool mShadow = false;
    std::vector<float> mBackdrop;
    int mBackdropCols = 0, mBackdropRows = 0;
    QSizeF mBackdropView;
    float mBackdropMean = 0.0f;
};

/** The glyph and icon atlas every viewport shares. Built on first use from the UI font, rebuilt when icons change. */
LILA_UI_API std::shared_ptr<const render::OverlayAtlas> overlayAtlas();

/**
 * The icons OverlayPainter::icon() can draw, baked into the overlay atlas as coverage (alpha only: the painter tints
 * them). Adding or removing one rebuilds the atlas before the next frame. Lila registers the Tabler icons its cursor
 * badges use; anything else is registered by id from a Tabler name, an SVG/PNG file or an image.
 */
namespace overlayIcons
{
struct Source
{
    /** A Tabler icon name (resources/icons/tabler), or empty. */
    QString tabler;
    /** An SVG or PNG file, or empty. */
    QString file;
    /** Used when tabler and file are empty. */
    QImage image;
};

/** Adds id, or replaces what it showed. owner is an extension id, empty for Lila. */
LILA_UI_API void add(std::string id, Source source, std::string owner = {});
/** For extensions: owned by ctx and removed when it unloads. 0, with a log message, when the id is taken. */
LILA_UI_API ext::Context::Handle add(ext::Context& ctx, std::string id, Source source);
LILA_UI_API bool remove(std::string_view id);
/** Everything owner added. Returns the count. */
LILA_UI_API size_t removeOwned(std::string_view owner);
LILA_UI_API bool contains(std::string_view id);
LILA_UI_API std::vector<std::string> ids();
/** Counts changes to the set, so callers can tell when the atlas will be rebuilt. */
LILA_UI_API uint64_t generation();
} // namespace overlayIcons
} // namespace lila::ui
