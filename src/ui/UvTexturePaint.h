#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <QImage>
#include <QPointF>
#include <QRect>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class QMouseEvent;
class QPainter;

namespace lila::ui
{
/**
 * The UV editor's texture view and painting: a texture set channel drawn under the UVs (resampled on the GPU to the view,
 * only the view-sized result read back), and, with painting on, the texture paint tool's brush in UV space. Each dab
 * lands on the triangle under its uv and becomes an ordinary surface stroke record (radius scaled to the triangle's 3D
 * size), so UV and viewport strokes mix in one layer and replay alike.
 */
class LILA_UI_API UvTexturePaint
{
public:
    using ToScreen = std::function<QPointF(const glm::dvec2&)>;
    using ToUv = std::function<glm::dvec2(const QPointF&)>;

    UvTexturePaint();
    ~UvTexturePaint();

    /** Every (set, channel) the view can show. */
    static std::vector<std::pair<std::string, std::string>> choices();
    /** An empty set turns the view off. */
    void setChannel(const std::string& set, const std::string& channel);
    bool shown() const;
    void setPainting(bool on);
    bool painting() const;

    /** Under the UVs, over each tile's background. */
    void drawBackground(QPainter& p, const QRect& widget, const ToScreen& toScreen, const ToUv& toUv, bool udim);
    /** The brush ring at the size it paints: its object-space radius through the uv scale under the cursor and the zoom. */
    void drawCursor(QPainter& p, QPointF at, const ToUv& toUv, double pxPerUv) const;

    /** pxPerUv is the view's zoom. True when painting took the event. */
    bool mousePress(QMouseEvent* e, const ToUv& toUv, double pxPerUv);
    bool mouseMove(QMouseEvent* e, const ToUv& toUv, double pxPerUv);
    bool mouseRelease(QMouseEvent* e, const ToUv& toUv, double pxPerUv);

    /** Asked to repaint (paint landed, the channel changed). */
    std::function<void()> onUpdate;

private:
    struct State;
    std::unique_ptr<State> m;
};
} // namespace lila::ui
