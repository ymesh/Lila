#pragma once

#include "core/Export.h"

#include <QImage>
#include <QWidget>

#include <string>

class QLabel;
class QTabBar;
class QToolButton;
class QSpinBox;

namespace lila::ui::references
{
struct Image;
enum class DropAction;

/** Drag mime type of a reference tab: json {"image": id, "window": id}. */
inline constexpr const char* kDragMime = "application/x-lila-reference-image";

/** The picture of a reference image with its flips, turns and grading applied; null when it can't be read. */
LILA_UI_API QImage displayImage(const Image& img);

/**
 * One Reference window's content: a tab per image (the tab bar shows from the second one), the image with zoom and pan,
 * and a toolbar. Images dropped on it become tabs, or with Shift replace the shown one. A tab dragged out of the bar
 * becomes a window of its own where it is let go, or joins the window it is dropped on; dropped on a viewport it shows
 * there as an overlay.
 */
class LILA_UI_API ReferenceWindow : public QWidget
{
public:
    ReferenceWindow(std::string windowId, QWidget* parent = nullptr);
    ~ReferenceWindow() override;

    const std::string& windowId() const { return mWindow; }
    QTabBar* tabBar() const;
    /** The shown image's id; empty with no tabs. */
    std::string currentImage() const;

    /** What a drop of files at the current modifiers would do, for the drop badge; public for tests. */
    static DropAction dropActionFor(Qt::KeyboardModifiers mods, bool hasCurrent);

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    void rebuild();
    void startTabDrag(int index);
    void showDropHint(QPoint at, const QString& icon, const QString& text);
    void hideDropHint();
    void openFiles();

    std::string mWindow;
    int mListener = 0;
    int mTextureListener = 0;
    int mVideoListener = 0;
    bool mRebuilding = false;
    class TabBar* mTabs = nullptr;
    class ImageView* mView = nullptr;
    class DropHint* mHint = nullptr;
    QWidget* mToolbar = nullptr;
    QToolButton* mFlipH = nullptr;
    QToolButton* mFlipV = nullptr;
    QToolButton* mGray = nullptr;
    QToolButton* mPick = nullptr;
    QToolButton* mMore = nullptr;
    /** Shown while the current tab is a video: pause, frame steps, offset. */
    QWidget* mVideoBar = nullptr;
    QToolButton* mPause = nullptr;
    QSpinBox* mVideoOffset = nullptr;
    QLabel* mEmpty = nullptr;
};

namespace windows
{
/**
 * Registers the Reference Images panel ("window.referenceImages") and keeps one dock per window of the set, and the
 * reference.* actions. Call once, after references::install().
 */
LILA_UI_API void install();
/** The dock object name of a set window's panel. */
LILA_UI_API std::string panelId(const std::string& windowId);
/** Applies a window's floating options (stay on top, opacity, click-through) to its dock. */
LILA_UI_API void applyWindowOptions(const std::string& windowId);
} // namespace windows
} // namespace lila::ui::references
