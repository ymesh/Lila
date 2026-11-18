#include "ui/RenderToolkit.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/RenderSettings.h"
#include "ui/Actions.h"
#include "ui/Controls.h"
#include "ui/FileDialog.h"
#include "ui/Icons.h"
#include "ui/OptionBox.h"
#include "ui/RenderOutput.h"
#include "ui/SpinScrub.h"
#include "ui/ToolkitLayout.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportToast.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace lila::ui
{
using json = nlohmann::json;

namespace renderRun
{
namespace
{
Result gLast;
std::map<int, std::function<void()>> gListeners;
int gNextListener = 1;

QString fileName(const std::string& path)
{
    return QFileInfo(QString::fromStdString(path)).fileName();
}
} // namespace

bool run(Shot shot)
{
    const char* command = shot == Shot::Frame ? "render" : shot == Shot::Range ? "renderSequence" : "renderTurntable";
    try
    {
        const json r = ops::execute(command, json::object(), CallSource::UI);
        Result res;
        if (shot == Shot::Frame)
        {
            res.file = r.get<std::string>();
            res.frames = 1;
            res.images = true;
        }
        else
        {
            res.frames = r.value("frames", 0);
            res.file = r.value("video", std::string());
            res.images = r.contains("path") && r["path"].is_array() && !r["path"].empty();
            if (res.file.empty() && r.contains("path") && r["path"].is_array() && !r["path"].empty())
                res.file = r["path"].back().get<std::string>();
        }
        gLast = res;
        log::info("Rendered {}", res.file);
        QString text = "Rendered " + fileName(res.file);
        if (res.frames > 1)
            text += QString(", %1 frames").arg(res.frames);
        viewportToast::showActive(text, 4000);
        for (const auto& [id, fn] : std::map(gListeners))
            fn();
        return true;
    }
    catch (const CommandError& e)
    {
        const std::string what = e.what();
        if (what.ends_with("cancelled"))
            viewportToast::showActive("Render cancelled", 2500);
        else
        {
            log::error("{}", what);
            viewportToast::showActive(QString::fromStdString(what), 8000, viewportToast::Kind::Error);
        }
    }
    return false;
}

const Result& last()
{
    return gLast;
}

void openLast()
{
    if (!gLast.file.empty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(gLast.file)));
}

void showFolder()
{
    if (!gLast.file.empty())
    {
        fileOps::reveal(QString::fromStdString(gLast.file));
        return;
    }
    const QString dir = QFileInfo(QString::fromStdString(renderOutput::defaultPattern())).absolutePath();
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

int addListener(std::function<void()> fn)
{
    const int id = gNextListener++;
    gListeners[id] = std::move(fn);
    return id;
}

void removeListener(int id)
{
    gListeners.erase(id);
}
} // namespace renderRun

namespace
{
constexpr const char* kRenderCollapsedPref = "/renderToolkit/collapsed";

const json& output()
{
    return scene::renderSettings().output;
}

template <class T> T value(const char* key, T fallback)
{
    const json& o = output();
    auto it = o.find(key);
    if (it == o.end() || it->is_null())
        return fallback;
    try
    {
        return it->get<T>();
    }
    catch (const json::exception&)
    {
        return fallback;
    }
}

QFormLayout* form(QWidget* parent)
{
    auto* f = new QFormLayout(parent);
    f->setContentsMargins(0, 0, 0, 4);
    f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    f->setHorizontalSpacing(8);
    f->setVerticalSpacing(4);
    return f;
}

QWidget* hbox(QWidget* parent, std::initializer_list<QWidget*> items)
{
    auto* w = new QWidget(parent);
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(4);
    for (QWidget* i : items)
    {
        i->setParent(w);
        h->addWidget(i, qobject_cast<QLabel*>(i) ? 0 : 1);
    }
    return w;
}

QSpinBox* intField(int lo, int hi, QWidget* parent)
{
    auto* s = new QSpinBox(parent);
    s->setRange(lo, hi);
    s->setKeyboardTracking(false);
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
    s->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return s;
}

QLabel* dimText(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    controls::setRole(l, "dim");
    return l;
}

/** A path field with a browse button; the button calls browse and commits what it returns. */
QWidget* pathField(QWidget* parent, QLineEdit*& edit, std::function<QString(const QString&)> browse)
{
    auto* row = new QWidget(parent);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(4);
    edit = new QLineEdit(row);
    auto* b = new QToolButton(row);
    b->setIcon(icon("folder-open"));
    b->setToolTip("Browse");
    b->setAutoRaise(true);
    h->addWidget(edit, 1);
    h->addWidget(b);
    QLineEdit* e = edit;
    QObject::connect(b, &QToolButton::clicked, row, [e, browse] {
        const QString path = browse(e->text().isEmpty() ? e->placeholderText() : e->text());
        if (!path.isEmpty())
        {
            e->setText(path);
            Q_EMIT e->editingFinished();
        }
    });
    return row;
}

const char* kShotLabels[] = {"Render Frame", "Render Range", "Render Turntable"};
const char* kWrites[] = {"images", "video", "both"};
const char* kSpins[] = {"object", "camera", "lights", "turntable"};
const char* kReflections[] = {"off", "screenSpace", "rayTraced"};
} // namespace

RenderToolkit::RenderToolkit(QWidget* parent) : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll);
    auto* body = new QWidget(scroll);
    auto* root = new QVBoxLayout(body);
    root->setContentsMargins(10, 6, 10, 8);
    root->setSpacing(4);
    scroll->setWidget(body);
    controls::fitScrollWidth(scroll);

    mRefresh = new QTimer(this);
    mRefresh->setSingleShot(true);
    mRefresh->setInterval(30);
    connect(mRefresh, &QTimer::timeout, this, [this] { refreshButtons(); });

    auto add = [&](const char* title, QWidget* content, bool open = true) {
        QWidget* s = collapsibleSection(title, content, body, kRenderCollapsedPref, open);
        root->addWidget(s);
        return s;
    };

    // What to render and the button that does it lead the panel; everything under them is how.
    mShot = new controls::Segmented(body);
    mShot->add("Frame", {}, "The current frame, as one image", 0);
    mShot->add("Range", {}, "The frame range, as images, a video or both", 1);
    mShot->add("Turntable", {}, "A looping turn around the selection, or everything shown when nothing is selected", 2);
    root->addWidget(mShot);
    mRender = new QPushButton(body);
    mRender->setDefault(true);
    mRender->setMinimumHeight(30);
    QFont bold = mRender->font();
    bold.setWeight(QFont::DemiBold);
    mRender->setFont(bold);
    root->addWidget(mRender);
    connect(mShot, &controls::Segmented::picked, this, [this](int i) {
        store("shot", std::string(i == 0 ? "frame" : i == 1 ? "range" : "turntable"));
        refresh();
    });
    connect(mRender, &QPushButton::clicked, this, [this] {
        const int i = std::max(mShot->current(), 0);
        renderRun::run(i == 0 ? renderRun::Shot::Frame : i == 1 ? renderRun::Shot::Range : renderRun::Shot::Turntable);
    });

    mResult = new QWidget(body);
    auto* rh = new QHBoxLayout(mResult);
    rh->setContentsMargins(0, 4, 0, 4);
    rh->setSpacing(8);
    mThumb = new QLabel(mResult);
    mThumb->setFixedSize(96, 54);
    mThumb->setAlignment(Qt::AlignCenter);
    mThumb->setCursor(Qt::PointingHandCursor);
    mThumb->setToolTip("Open");
    mThumb->installEventFilter(this);
    rh->addWidget(mThumb);
    auto* rv = new QVBoxLayout;
    rv->setSpacing(2);
    mResultName = new QLabel(mResult);
    mResultName->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rv->addWidget(mResultName);
    std::vector<QWidget*> resultButtons;
    for (auto [label, tip, fn] : {std::tuple{"Open", "Opens the render in the system's viewer", &renderRun::openLast},
                                  std::tuple{"Show in Folder", "Shows the render selected in its folder", &renderRun::showFolder}})
    {
        auto* b = new QToolButton(mResult);
        b->setText(label);
        b->setToolTip(tip);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        controls::setRole(b, "push");
        connect(b, &QToolButton::clicked, this, [fn] { fn(); });
        resultButtons.push_back(b);
    }
    rv->addWidget(controls::grid(resultButtons, 2, mResult));
    rh->addLayout(rv, 1);
    root->addWidget(mResult);

    // Picture
    {
        auto* box = new QWidget(body);
        QFormLayout* f = form(box);
        mCamera = new QComboBox(box);
        mCamera->setToolTip("The camera renders look through");
        f->addRow("Camera", mCamera);
        mWidth = intField(16, 16384, box);
        mHeight = intField(16, 16384, box);
        mWidth->setSuffix(" px");
        mHeight->setSuffix(" px");
        mWidth->setToolTip("Render width; the scene's resolution, shared with the gates and Camera Settings");
        mHeight->setToolTip("Render height; the scene's resolution, shared with the gates and Camera Settings");
        f->addRow("Size", hbox(box, {mWidth, dimText(QString(QChar(0x00D7)), box), mHeight}));
        mScale = controls::valueField(5.0, 800.0, 0, "%", box);
        mScale->setToolTip("Percent of the size: 50% for quick drafts");
        setSpinDefault(mScale, 100.0);
        mSize = dimText({}, box);
        mSize->setMinimumWidth(80);
        f->addRow("Scale", hbox(box, {mScale, mSize}));
        mSamples = intField(1, 4096, box);
        mSamples->setToolTip("Jittered frames averaged into each picture. More smooths edges, depth of field and motion blur.");
        f->addRow("Samples", mSamples);
        mMotionBlur = new QCheckBox("Motion Blur", box);
        mMotionBlur->setToolTip("Blurs what moves across the camera's shutter");
        mTransparent = new QCheckBox("Transparent", box);
        mTransparent->setToolTip("No backdrop: alpha is coverage (png, tif, exr, psd)");
        f->addRow("", hbox(box, {mMotionBlur, mTransparent}));
        add("Picture", box);

        connect(mCamera, &QComboBox::currentIndexChanged, this, [this](int) { store("camera", mCamera->currentData().toString().toStdString()); });
        auto resolution = [](const char* key, int v) {
            try
            {
                ops::execute("setRenderResolution", {{key, v}}, CallSource::UI);
            }
            catch (const CommandError& e)
            {
                log::error("{}", e.what());
            }
        };
        connect(mWidth, &QSpinBox::valueChanged, this, [resolution](int v) { resolution("width", v); });
        connect(mHeight, &QSpinBox::valueChanged, this, [resolution](int v) { resolution("height", v); });
        connect(mScale, &QDoubleSpinBox::valueChanged, this, [this](double v) { store("scale", v); });
        connect(mSamples, &QSpinBox::valueChanged, this, [this](int n) { store("samples", n); });
        connect(mMotionBlur, &QCheckBox::toggled, this, [this](bool on) { store("motionBlur", on); });
        connect(mTransparent, &QCheckBox::toggled, this, [this](bool on) { store("transparent", on); });
    }

    // Frames: the range for Range, the turn for Turntable, nothing for Frame.
    {
        auto* box = new QWidget(body);
        auto* v = new QVBoxLayout(box);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);

        mRangeBox = new QWidget(box);
        QFormLayout* rf = form(mRangeBox);
        mStart = controls::valueField(-1e6, 1e6, 0, {}, mRangeBox);
        mEnd = controls::valueField(-1e6, 1e6, 0, {}, mRangeBox);
        mStart->setToolTip("First frame; the playback range's until set");
        mEnd->setToolTip("Last frame; the playback range's until set");
        rf->addRow("Range", hbox(mRangeBox, {mStart, dimText("to", mRangeBox), mEnd}));
        mStep = controls::valueField(0.01, 1000.0, 2, {}, mRangeBox);
        mStep->setToolTip("Frames between renders: 2 renders every other frame");
        setSpinDefault(mStep, 1.0);
        rf->addRow("Step", mStep);
        v->addWidget(mRangeBox);

        mTurnBox = new QWidget(box);
        QFormLayout* tf = form(mTurnBox);
        mSpin = new QComboBox(mTurnBox);
        mSpin->addItem("Object", "object");
        mSpin->addItem("Camera", "camera");
        mSpin->addItem("Lights", "lights");
        mSpin->addItem("Turntable Node", "turntable");
        mSpin->setItemData(0, "The model turns in fixed light", Qt::ToolTipRole);
        mSpin->setItemData(1, "The camera circles; the light stays with the scene", Qt::ToolTipRole);
        mSpin->setItemData(2, "The model and camera hold; the light turns", Qt::ToolTipRole);
        mSpin->setItemData(3, "A turntable node turns its members through the view's own camera", Qt::ToolTipRole);
        tf->addRow("Spin", mSpin);
        mTurnFrames = intField(2, 10000, mTurnBox);
        mTurnFrames->setToolTip("Frames for the whole turn");
        setSpinDefault(mTurnFrames, 120);
        tf->addRow("Frames", mTurnFrames);
        mTurns = controls::valueField(0.01, 100.0, 2, {}, mTurnBox);
        mTurns->setToolTip("Turns over the frames");
        setSpinDefault(mTurns, 1.0);
        tf->addRow("Turns", mTurns);
        mPitch = controls::valueField(-89.0, 89.0, 0, QString(QChar(0x00B0)), mTurnBox);
        mPitch->setToolTip("Camera elevation: negative looks down");
        setSpinDefault(mPitch, -15.0);
        tf->addRow("Pitch", mPitch);
        mYaw = controls::valueField(-360.0, 360.0, 0, QString(QChar(0x00B0)), mTurnBox);
        mYaw->setToolTip("Starting angle around the up axis");
        setSpinDefault(mYaw, 35.0);
        tf->addRow("Start Angle", mYaw);
        v->addWidget(mTurnBox);
        mFramesSection = add("Frames", box);

        connect(mStart, &QDoubleSpinBox::valueChanged, this, [this](double x) { store("start", x); });
        connect(mEnd, &QDoubleSpinBox::valueChanged, this, [this](double x) { store("end", x); });
        connect(mStep, &QDoubleSpinBox::valueChanged, this, [this](double x) { store("step", x); });
        connect(mSpin, &QComboBox::currentIndexChanged, this, [this](int) { store("spin", mSpin->currentData().toString().toStdString()); });
        connect(mTurnFrames, &QSpinBox::valueChanged, this, [this](int n) { store("frames", n); });
        connect(mTurns, &QDoubleSpinBox::valueChanged, this, [this](double x) { store("turns", x); });
        connect(mPitch, &QDoubleSpinBox::valueChanged, this, [this](double x) { store("pitch", x); });
        connect(mYaw, &QDoubleSpinBox::valueChanged, this, [this](double x) { store("yaw", x); });
    }

    // Output
    {
        auto* box = new QWidget(body);
        mOutForm = form(box);
        mWrite = new controls::Segmented(box);
        mWrite->add("Images", {}, "Numbered images only", 0);
        mWrite->add("Video", {}, "An .mp4 only", 1);
        mWrite->add("Both", {}, "Numbered images and an .mp4 of them", 2);
        mOutForm->addRow("Write", mWrite);
        mOutForm->addRow("Images", pathField(box, mImages, [this](const QString& from) {
                             return fileDialog::getSaveFileName(this, "Rendered Images", from, "Images (*.png *.exr *.jpg *.tif *.tga *.psd)");
                         }));
        mImages->setToolTip("#### stands for the frame number. The extension picks the format: exr keeps linear light.");
        mOutForm->addRow("Video", pathField(box, mVideo, [this](const QString& from) {
                             return fileDialog::getSaveFileName(this, "Video", from, "MP4 video (*.mp4)");
                         }));
        mCodec = new controls::Segmented(box);
        mCodec->add("H.264", {}, "Plays everywhere", 0);
        mCodec->add("HEVC", {}, "Smaller files at the same quality; some players lack it", 1);
        mOutForm->addRow("Codec", mCodec);
        auto* quality = new controls::SliderField(box);
        quality->setRange(0.0, 1.0);
        quality->setDecimals(2);
        quality->setSingleStep(0.05);
        quality->setToolTip("Video quality: higher is sharper and larger");
        setSpinDefault(quality, 0.6);
        mQuality = quality;
        mOutForm->addRow("Quality", mQuality);
        mWrites = dimText({}, box);
        mWrites->setWordWrap(true);
        mWrites->setTextInteractionFlags(Qt::TextSelectableByMouse);
        mOutForm->addRow("Writes", mWrites);
        add("Output", box);

        connect(mWrite, &controls::Segmented::picked, this, [this](int i) {
            store("write", std::string(kWrites[std::clamp(i, 0, 2)]));
            refresh();
        });
        connect(mImages, &QLineEdit::editingFinished, this, [this] { store("path", mImages->text().trimmed().toStdString()); });
        connect(mVideo, &QLineEdit::editingFinished, this, [this] { store("video", mVideo->text().trimmed().toStdString()); });
        connect(mCodec, &controls::Segmented::picked, this, [this](int i) { store("codec", std::string(i ? "hevc" : "h264")); });
        connect(mQuality, &QDoubleSpinBox::valueChanged, this, [this](double x) { store("quality", x); });
    }

    // Action sections are rebuilt when actions come and go: extensions (lights, turntables) register theirs after the panel is made.
    mActions = new QWidget(body);
    auto* actionsLayout = new QVBoxLayout(mActions);
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    actionsLayout->setSpacing(4);
    root->addWidget(mActions);
    mRebuild = new QTimer(this);
    mRebuild->setSingleShot(true);
    mRebuild->setInterval(0);
    connect(mRebuild, &QTimer::timeout, this, [this] { buildActions(); });
    mActionListener = actions::registry().addListener([this](RegistryChange, const std::string&, const std::string&) { mRebuild->start(); });
    buildActions();

    root->addStretch(1);

    mSettingsListener = scene::addRenderSettingsListener([this] { refresh(); });
    QPointer<RenderToolkit> self(this);
    mPrefsListener = PrefStore::get().onChanged([self](const std::string& file) {
        if (self && file == "general")
            self->mRefresh->start();
    });
    mResultListener = renderRun::addListener([this] { refreshResult(); });
    refresh();
    refreshResult();
}

RenderToolkit::~RenderToolkit()
{
    actions::registry().removeListener(mActionListener);
    scene::removeRenderSettingsListener(mSettingsListener);
    PrefStore::get().removeOnChanged(mPrefsListener);
    renderRun::removeListener(mResultListener);
}

void RenderToolkit::addGroup(QWidget* box, QVBoxLayout* v, const QString& heading, std::vector<std::pair<const char*, const char*>> items,
                             std::vector<std::string> more)
{
    std::vector<QWidget*> rows;
    for (auto [id, label] : items)
        if (QToolButton* b = row(id, label, box))
            rows.push_back(b);
    std::erase_if(more, [](const std::string& id) { return !actions::find(id); });
    if (!more.empty())
        rows.push_back(controls::overflow(more, box));
    if (rows.empty())
        return;
    if (!heading.isEmpty())
    {
        QLabel* h = dimText(heading, box);
        h->setContentsMargins(2, 4, 0, 0);
        v->addWidget(h);
    }
    v->addWidget(controls::grid(rows, kToolkitColumns, box));
}

void RenderToolkit::buildActions()
{
    mButtons.clear();
    mReflections = nullptr;
    QLayout* layout = mActions->layout();
    while (QLayoutItem* item = layout->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    auto* root = static_cast<QVBoxLayout*>(layout);
    auto add = [&](const char* title, QWidget* content, bool open = true) { root->addWidget(collapsibleSection(title, content, mActions, kRenderCollapsedPref, open)); };

    // Look: the scene's grade and lens, and what the view and renders trace.
    {
        auto* box = new QWidget(mActions);
        auto* v = new QVBoxLayout(box);
        v->setContentsMargins(0, 0, 0, 4);
        v->setSpacing(4);
        auto* reflBox = new QWidget(box);
        QFormLayout* f = form(reflBox);
        mReflections = new controls::Segmented(reflBox);
        mReflections->add("Off", {}, QString::fromStdString(actions::tooltip("viewport.reflections.off")), 0);
        mReflections->add("Screen", {}, QString::fromStdString(actions::tooltip("viewport.reflections.screenSpace")), 1);
        mReflections->add("Traced", {}, QString::fromStdString(actions::tooltip("viewport.reflections.rayTraced")), 2);
        f->addRow("Reflections", mReflections);
        v->addWidget(reflBox);
        connect(mReflections, &controls::Segmented::picked, this,
                [](int i) { actions::run(json(std::string("viewport.reflections.") + kReflections[std::clamp(i, 0, 2)])); });

        addGroup(box, v, {}, {{"render.postProcess", "Post Process"}, {"render.colorManagement", "Color"}, {"camera.pickFocus", "Pick Focus"},
                              {"window.scopes", "Scopes"}},
                 {"render.exportGradeLut"});
        addGroup(box, v, "Viewport",
                 {{"viewport.toggleRtShadows", "Traced Shadows"}, {"viewport.toggleAO", "Occlusion"}, {"viewport.toggleDepthOfField", "Depth of Field"},
                  {"viewport.toggleMotionBlur", "Motion Blur"}, {"viewport.togglePostEffects", "Post Effects"}, {"viewport.toggleTAA", "Temporal AA"}},
                 {"viewport.toggleKuwahara", "viewport.toggleOutline"});
        add("Look", box);
    }

    {
        auto* box = new QWidget(mActions);
        auto* v = new QVBoxLayout(box);
        v->setContentsMargins(0, 0, 0, 4);
        v->setSpacing(4);
        addGroup(box, v, "Rigs", {{"render.lightRig.threePoint", "Three-Point"}, {"render.lightRig.rim", "Rim"}, {"render.lightRig.product", "Product"}}, {});
        addGroup(box, v, "Add",
                 {{"lights.create.point", "Point"}, {"lights.create.spot", "Spot"}, {"lights.create.area", "Area"}, {"lights.create.directional", "Directional"}},
                 {"lights.create.disc", "lights.create.dome", "viewport.toggleLighting"});
        if (v->count())
            add("Lights", box);
        else
            delete box;
    }

    auto actionGroup = [&](const char* title, std::vector<std::pair<const char*, const char*>> items, std::vector<std::string> more, bool open) {
        auto* box = new QWidget(mActions);
        auto* v = new QVBoxLayout(box);
        v->setContentsMargins(0, 0, 0, 4);
        addGroup(box, v, {}, std::move(items), std::move(more));
        if (v->count())
            add(title, box, open);
        else
            delete box;
    };
    actionGroup("Camera",
                {{"create.cameraFromView", "From View"},
                 {"viewport.cameraSettings", "Settings"},
                 {"viewport.toggleResolutionGate", "Resolution Gate"},
                 {"viewport.toggleSafeFrames", "Safe Frames"},
                 {"viewport.toggleThirds", "Thirds"},
                 {"viewport.toggleFilmGate", "Film Gate"}},
                {"create.camera"}, true);
    actionGroup("Turntable",
                {{"turntable.putOn", "Put On"}, {"turntable.spin", "Spin"}, {"turntable.controls", "Controls"}, {"turntable.takeOff", "Take Off"}},
                {"turntable.front", "turntable.stepBack", "turntable.stepNext", "turntable.ring", "turntable.pitch", "create.turntableBase"}, false);

    refreshButtons();
}

QToolButton* RenderToolkit::row(const std::string& id, const QString& label, QWidget* parent)
{
    if (!actions::find(id))
        return nullptr;
    QToolButton* b = controls::row(id, nullptr, parent, label);
    b->setProperty("lilaAction", QString::fromStdString(id));
    connect(b, &QToolButton::clicked, this, [this] { mRefresh->start(); });
    mButtons.push_back(b);
    return b;
}

void RenderToolkit::store(const char* key, const json& v)
{
    if (output().value(key, json()) == v)
        return;
    scene::renderSettings().output[key] = v;
    scene::renderSettingsChanged();
}

void RenderToolkit::refresh()
{
    const scene::RenderSettings& r = scene::renderSettings();
    QSignalBlocker b0(mCamera), b1(mScale), b2(mSamples), b3(mMotionBlur), b4(mTransparent), b5(mStart), b6(mEnd), b7(mStep), b8(mQuality),
        b9(mWidth), b10(mHeight), b11(mSpin), b12(mTurnFrames), b13(mTurns), b14(mPitch), b15(mYaw);

    const std::string shot = value<std::string>("shot", "range");
    const int shotIndex = shot == "frame" ? 0 : shot == "turntable" ? 2 : 1;
    mShot->setCurrent(shotIndex);
    mRender->setText(kShotLabels[shotIndex]);

    mCamera->clear();
    mCamera->addItem("Active View", QString());
    scene::Graph& g = scene::Graph::current();
    for (scene::NodeId id : g.nodes())
        if (const scene::Node* n = g.node(id); n && n->type().isA("camera"))
            mCamera->addItem(QString::fromStdString(n->name()), QString::fromStdString(n->name()));
    mCamera->setCurrentIndex(std::max(mCamera->findData(QString::fromStdString(value<std::string>("camera", ""))), 0));
    if (!mWidth->hasFocus())
        mWidth->setValue(r.width);
    if (!mHeight->hasFocus())
        mHeight->setValue(r.height);
    const double scale = value("scale", 100.0);
    mScale->setValue(scale);
    mSize->setText(QString("%1 %2 %3").arg(int(std::lround(r.width * scale / 100.0))).arg(QChar(0x00D7)).arg(int(std::lround(r.height * scale / 100.0))));
    mSize->setVisible(std::abs(scale - 100.0) > 1e-6);
    mSamples->setValue(value("samples", 64));
    mMotionBlur->setChecked(value("motionBlur", false));
    mTransparent->setChecked(value("transparent", false));

    mFramesSection->setVisible(shotIndex != 0);
    mRangeBox->setVisible(shotIndex == 1);
    mTurnBox->setVisible(shotIndex == 2);
    const scene::anim::Playback& pb = scene::anim::playback();
    mStart->setValue(value("start", std::round(scene::anim::frameOf(pb.minTime))));
    mEnd->setValue(value("end", std::round(scene::anim::frameOf(pb.maxTime))));
    mStep->setValue(value("step", 1.0));
    const std::string spin = value<std::string>("spin", "object");
    mSpin->setCurrentIndex(std::max(mSpin->findData(QString::fromStdString(spin)), 0));
    mTurnFrames->setValue(value("frames", 120));
    mTurns->setValue(value("turns", 1.0));
    mPitch->setValue(value("pitch", -15.0));
    mYaw->setValue(value("yaw", 35.0));
    // A turntable node frames through the view's own camera.
    mPitch->setEnabled(spin != "turntable");
    mYaw->setEnabled(spin != "turntable");

    const std::string write = value<std::string>("write", "video");
    const int writeIndex = write == "images" ? 0 : write == "both" ? 2 : 1;
    mWrite->setCurrent(writeIndex);
    // A frame is always one image, so the choice and the video rows only apply to runs of frames.
    const bool images = shotIndex == 0 || writeIndex != 1;
    const bool video = shotIndex != 0 && writeIndex != 0;
    mOutForm->setRowVisible(mWrite, shotIndex != 0);
    mOutForm->setRowVisible(mImages->parentWidget(), images);
    mOutForm->setRowVisible(mVideo->parentWidget(), video);
    mOutForm->setRowVisible(mCodec, video);
    mOutForm->setRowVisible(mQuality, video);
    if (!mImages->hasFocus())
    {
        mImages->setText(QString::fromStdString(value<std::string>("path", "")));
        mImages->setPlaceholderText(QString::fromStdString(renderOutput::defaultPattern()));
    }
    if (!mVideo->hasFocus())
    {
        mVideo->setText(QString::fromStdString(value<std::string>("video", "")));
        mVideo->setPlaceholderText(QString::fromStdString(renderOutput::defaultVideo()));
    }
    mCodec->setCurrent(value<std::string>("codec", "h264") == "hevc" ? 1 : 0);
    mQuality->setValue(value("quality", 0.6));

    // The files the button will write, as the render commands resolve them.
    const std::string suffix = shotIndex == 2 ? "_turntable" : "";
    const std::string pattern = renderOutput::withSuffix(value<std::string>("path", "").empty() ? renderOutput::defaultPattern() : value<std::string>("path", ""), suffix);
    const std::string videoPath = renderOutput::withSuffix(value<std::string>("video", "").empty() ? renderOutput::defaultVideo() : value<std::string>("video", ""), suffix);
    QStringList files, full;
    if (shotIndex == 0)
    {
        const std::string one = renderOutput::framePath(pattern, int(std::lround(scene::anim::frameOf(g.time()))));
        files << QFileInfo(QString::fromStdString(one)).fileName();
        full << QString::fromStdString(one);
    }
    else
    {
        if (images)
        {
            files << QFileInfo(QString::fromStdString(pattern)).fileName();
            full << QString::fromStdString(pattern);
        }
        if (video)
        {
            files << QFileInfo(QString::fromStdString(videoPath)).fileName();
            full << QString::fromStdString(videoPath);
        }
    }
    mWrites->setText(files.join("\n"));
    mWrites->setToolTip(full.join("\n"));
    refreshButtons();
}

void RenderToolkit::refreshResult()
{
    const renderRun::Result& r = renderRun::last();
    mResult->setVisible(!r.file.empty());
    if (r.file.empty())
        return;
    const QString name = QFileInfo(QString::fromStdString(r.file)).fileName();
    mResultName->setText(r.frames > 1 ? QString("%1  %2 frames").arg(name).arg(r.frames) : name);
    mResultName->setToolTip(QString::fromStdString(r.file));
    const QImage img = r.images ? renderOutput::lastImage() : QImage();
    if (img.isNull())
        mThumb->setPixmap(icon("movie").pixmap(32, 32));
    else
        mThumb->setPixmap(QPixmap::fromImage(img.scaled(mThumb->size() * devicePixelRatioF(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

bool RenderToolkit::eventFilter(QObject* o, QEvent* e)
{
    if (o == mThumb && e->type() == QEvent::MouseButtonRelease)
    {
        renderRun::openLast();
        return true;
    }
    return QWidget::eventFilter(o, e);
}

void RenderToolkit::refreshButtons()
{
    for (QToolButton* b : mButtons)
    {
        const std::string id = b->property("lilaAction").toString().toStdString();
        const Action* a = actions::find(id);
        if (!a)
            continue;
        QString tip = QString::fromStdString(actions::tooltip(id));
        if (optionBox::available(id))
            tip += "\nClick corner, right-click or Shift+click for options";
        b->setToolTip(tip);
        if (a->checked && b->isCheckable())
            b->setChecked(a->checked());
    }
    if (!mReflections)
        return;
    const std::string refl = PrefStore::get().value<std::string>("general", viewportDisplay::kReflections, "off");
    mReflections->setCurrent(refl == "screenSpace" ? 1 : refl == "rayTraced" ? 2 : 0);
}

void RenderToolkit::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refresh();
}
} // namespace lila::ui
