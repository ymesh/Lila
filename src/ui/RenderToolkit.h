#pragma once

#include "core/Export.h"

#include <QString>
#include <QWidget>

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;
class QVBoxLayout;
class QToolButton;

namespace lila::ui
{
namespace controls
{
class Segmented;
}

/** Dock object name for the Render toolkit. */
inline constexpr const char* kRenderToolkitDock = "renderToolkit";

/** Renders run from the UI (toolkit, menus, hotkeys): each confirms in the viewport and is kept as the last render to open. */
namespace renderRun
{
enum class Shot
{
    Frame,
    Range,
    Turntable,
};

struct Result
{
    /** The file to open: the video when there is one, else the last image. */
    std::string file;
    int frames = 0;
    /** Images were written, so the last one stands for the render. */
    bool images = false;
};

/** Runs the shot with the Render Output settings. False when it failed or was cancelled; the viewport says which. */
LILA_UI_API bool run(Shot shot);
/** The last render this session; empty file before any. */
LILA_UI_API const Result& last();
/** Opens the last render in the system viewer. */
LILA_UI_API void openLast();
/** Shows the last render selected in its folder, or the render folder before any. */
LILA_UI_API void showFolder();
LILA_UI_API int addListener(std::function<void()> fn);
LILA_UI_API void removeListener(int id);
} // namespace renderRun

/**
 * The Rendering layout's toolkit: what to render (frame, range, turntable) and where it goes, the picture settings, the
 * look, lights and camera framing. Its settings are the scene's Render Output (RenderSettings::output).
 */
class LILA_UI_API RenderToolkit : public QWidget
{
    Q_OBJECT

public:
    explicit RenderToolkit(QWidget* parent = nullptr);
    ~RenderToolkit() override;

protected:
    void showEvent(QShowEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void refresh();
    void refreshResult();
    void refreshButtons();
    void buildActions();
    /** A grid of action rows under an optional dim heading, the rare ones in its overflow; nothing when none is registered. */
    void addGroup(QWidget* box, QVBoxLayout* v, const QString& heading, std::vector<std::pair<const char*, const char*>> items,
                  std::vector<std::string> more);
    void store(const char* key, const nlohmann::json& value);
    QToolButton* row(const std::string& id, const QString& label, QWidget* parent);

    int mSettingsListener = 0;
    int mPrefsListener = 0;
    int mResultListener = 0;
    QTimer* mRefresh = nullptr;
    QTimer* mRebuild = nullptr;
    QWidget* mActions = nullptr;
    int mActionListener = 0;
    std::vector<QToolButton*> mButtons;

    controls::Segmented* mShot = nullptr;
    QPushButton* mRender = nullptr;
    QWidget* mResult = nullptr;
    QLabel* mThumb = nullptr;
    QLabel* mResultName = nullptr;

    QComboBox* mCamera = nullptr;
    QSpinBox* mWidth = nullptr;
    QSpinBox* mHeight = nullptr;
    QDoubleSpinBox* mScale = nullptr;
    QLabel* mSize = nullptr;
    QSpinBox* mSamples = nullptr;
    QCheckBox* mMotionBlur = nullptr;
    QCheckBox* mTransparent = nullptr;

    QWidget* mFramesSection = nullptr;
    QWidget* mRangeBox = nullptr;
    QDoubleSpinBox* mStart = nullptr;
    QDoubleSpinBox* mEnd = nullptr;
    QDoubleSpinBox* mStep = nullptr;
    QWidget* mTurnBox = nullptr;
    QSpinBox* mTurnFrames = nullptr;
    QDoubleSpinBox* mTurns = nullptr;
    QComboBox* mSpin = nullptr;
    QDoubleSpinBox* mPitch = nullptr;
    QDoubleSpinBox* mYaw = nullptr;

    QFormLayout* mOutForm = nullptr;
    controls::Segmented* mWrite = nullptr;
    QLineEdit* mImages = nullptr;
    QLineEdit* mVideo = nullptr;
    controls::Segmented* mCodec = nullptr;
    QDoubleSpinBox* mQuality = nullptr;
    QLabel* mWrites = nullptr;

    controls::Segmented* mReflections = nullptr;
};
} // namespace lila::ui
