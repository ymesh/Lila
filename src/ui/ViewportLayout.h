#pragma once

#include "core/Export.h"

#include <QWidget>

#include <array>

class QGridLayout;
class QSplitter;

namespace lila::ui
{
class ViewportPanel;
class ViewportWidget;

/**
 * The central viewport area: one view, or Maya's four (top, persp, front, side). Tapping Space toggles between them
 * around the view under the cursor. Actions that target "the viewport" act on the active one: the last clicked.
 * Each panel's view, its camera poses and the camera node it looks through save with the scene ("ui"/"viewports").
 */
class LILA_UI_API ViewportLayout : public QWidget
{
    Q_OBJECT

public:
    /** What the area shows in a texturing layout: the viewports, the 2D View beside them, or the 2D View alone. */
    enum class TextureView
    {
        View3D,
        Split,
        View2D,
    };

    explicit ViewportLayout(QWidget* parent = nullptr);

    /** The main window's viewport area; null before it exists. */
    static ViewportLayout* main();
    /** The viewport area a widget sits in, or null. */
    static ViewportLayout* of(const QWidget* inside);

    TextureView textureView() const { return mTextureView; }
    /** Remembered across sessions; other layouts keep showing the viewports alone. */
    void setTextureView(TextureView mode);
    /** The 2D View beside the viewports; null until Split or 2D first shows it. */
    QWidget* texture2d() const { return mTexture2d; }

    ViewportWidget* active() const { return mActive; }
    /** Top, persp, front and side panels, in Maya's quad order. */
    const std::array<ViewportWidget*, 4>& views() const { return mViews; }
    bool quad() const { return mQuad; }
    void setQuad(bool on);
    void toggleQuad();
    /** Frames everything in the active view unless the scene just loaded brought its own cameras. */
    void frameUnlessRestored();

Q_SIGNALS:
    void textureViewChanged();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void relayout();
    void applyTextureView();

    std::array<ViewportPanel*, 4> mPanels{}; // each a view with its toolbar
    std::array<ViewportWidget*, 4> mViews{}; // top, persp, front, side (Maya's quad order)
    ViewportWidget* mActive = nullptr;
    QGridLayout* mGrid = nullptr;
    QSplitter* mSplit = nullptr;
    QWidget* mViewsBox = nullptr;
    QWidget* mTexture2d = nullptr;
    TextureView mTextureView = TextureView::View3D;
    bool mQuad = false;
    bool mCamerasRestored = false;
};
} // namespace lila::ui
