#include "ui/ViewportLayout.h"

#include "core/PrefStore.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "ui/MenuContext.h"
#include "ui/NodeLookup.h"
#include "ui/TextureGraphUi.h"
#include "ui/ViewportToolbar.h"
#include "ui/ViewportWidget.h"

#include <QCursor>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPointer>
#include <QSplitter>

#include <algorithm>

namespace lila::ui
{
namespace
{
constexpr const char* kPrefTextureView = "/viewport/textureView";

ViewportLayout*& mainLayout()
{
    static ViewportLayout* layout = nullptr;
    return layout;
}
} // namespace

ViewportLayout* ViewportLayout::main() { return mainLayout(); }

ViewportLayout* ViewportLayout::of(const QWidget* inside)
{
    for (const QWidget* w = inside; w; w = w->parentWidget())
        if (auto* layout = qobject_cast<const ViewportLayout*>(w))
            return const_cast<ViewportLayout*>(layout);
    return nullptr;
}

ViewportLayout::ViewportLayout(QWidget* parent) : QWidget(parent)
{
    if (!mainLayout())
        mainLayout() = this;
    connect(this, &QObject::destroyed, [this] {
        if (mainLayout() == this)
            mainLayout() = nullptr;
    });
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    mSplit = new QSplitter(Qt::Horizontal, this);
    mSplit->setChildrenCollapsible(false);
    root->addWidget(mSplit);
    mViewsBox = new QWidget(mSplit);
    mGrid = new QGridLayout(mViewsBox);
    mGrid->setContentsMargins(0, 0, 0, 0);
    mGrid->setSpacing(2);
    const char* views[4] = {"top", "persp", "front", "right"};
    for (int i = 0; i < 4; ++i)
    {
        mPanels[i] = new ViewportPanel(mViewsBox);
        mViews[i] = mPanels[i]->view();
        mViews[i]->setView(views[i]);
        mViews[i]->installEventFilter(this);
        mGrid->addWidget(mPanels[i], i / 2, i % 2);
    }
    mActive = mViews[1];
    mQuad = PrefStore::get().value<bool>("general", "/viewport/quad", false);
    relayout();

    QPointer<ViewportLayout> self(this);
    mTextureView = TextureView(std::clamp(PrefStore::get().value<int>("general", kPrefTextureView, 0), 0, 2));
    menuContext::onChanged([self] {
        if (self)
            self->applyTextureView();
    });
    scene::isolation::setActiveProvider([self]() -> const scene::isolation::Set* {
        return self && self->mActive ? &self->mActive->isolation() : nullptr;
    });
    if (!ops::find("viewCamera"))
        ops::registerCommand({
            .name = "viewCamera",
            .doc = "The active view's camera: with eye and lookAt, put its perspective camera there (scenes save it, so a scene opens on "
                   "that view). Returns the view's state {view, persp: {target, distance, yaw, pitch, orthoWidth, focalLength}}.",
            .flags = {{"eye", "", ArgType::Vec3, nullptr, "Where the camera stands, cm."},
                      {"lookAt", "la", ArgType::Vec3, nullptr, "The point it looks at, cm."}},
            .undoable = false,
            .run = [self](const Args& a) -> nlohmann::json {
                if (!self || !self->mActive)
                    throw CommandError("viewCamera: no viewport");
                if (a.has("eye") || a.has("lookAt"))
                {
                    if (!a.has("eye") || !a.has("lookAt"))
                        throw CommandError("viewCamera: give both eye and lookAt");
                    const auto vec = [&](const char* k) {
                        const nlohmann::json v = a.flags[k];
                        return glm::dvec3(v.at(0).get<double>(), v.at(1).get<double>(), v.at(2).get<double>());
                    };
                    const glm::dvec3 eye = vec("eye"), at = vec("lookAt");
                    const double distance = glm::length(at - eye);
                    if (distance <= 1e-6)
                        throw CommandError("viewCamera: eye and lookAt are the same point");
                    const glm::dvec3 d = (at - eye) / distance;
                    // Camera::forward: yaw turns about +Y from -Z, pitch tilts towards +Y.
                    nlohmann::json pose = self->mActive->cameraState().value("persp", nlohmann::json::object());
                    pose["target"] = {at.x, at.y, at.z};
                    pose["distance"] = distance;
                    pose["pitch"] = glm::degrees(std::asin(std::clamp(d.y, -1.0, 1.0)));
                    pose["yaw"] = glm::degrees(std::atan2(-d.x, -d.z));
                    self->mActive->restoreCameraState({{"view", "persp"}, {"persp", pose}});
                    self->mActive->update();
                }
                return self->mActive->cameraState();
            },
        });
    io::registerUiSection(
        "viewports",
        [self]() -> nlohmann::json {
            if (!self)
                return nullptr;
            nlohmann::json names = nlohmann::json::array(), cameras = nlohmann::json::array(), isolation = nlohmann::json::array();
            bool any = false, anyIsolated = false;
            for (ViewportWidget* v : self->mViews)
            {
                const scene::Node* n = scene::Graph::current().node(v->lookThroughCamera());
                names.push_back(n ? n->fullPath() : std::string());
                any |= n != nullptr;
                cameras.push_back(v->cameraState());
                isolation.push_back(scene::isolation::toJson(v->isolation(), scene::Graph::current()));
                anyIsolated |= v->isolating();
            }
            nlohmann::json section{{"cameras", cameras}};
            if (any)
                section["lookThrough"] = names;
            if (anyIsolated)
                section["isolation"] = isolation;
            for (size_t i = 0; i < self->mViews.size(); ++i)
                if (self->mViews[i] == self->mActive)
                    section["active"] = i;
            return section;
        },
        [self](const nlohmann::json& section) {
            if (!self)
                return;
            nlohmann::json names = section.is_object() ? section.value("lookThrough", nlohmann::json::array()) : nlohmann::json::array();
            nlohmann::json cameras = section.is_object() ? section.value("cameras", nlohmann::json::array()) : nlohmann::json::array();
            for (size_t i = 0; i < self->mViews.size() && i < cameras.size(); ++i)
                self->mViews[i]->restoreCameraState(cameras[i]);
            self->mCamerasRestored = !cameras.empty();
            nlohmann::json isolation = section.is_object() ? section.value("isolation", nlohmann::json::array()) : nlohmann::json::array();
            for (size_t i = 0; i < self->mViews.size(); ++i)
                self->mViews[i]->setIsolation(scene::isolation::fromJson(i < isolation.size() ? isolation[i] : nlohmann::json(), scene::Graph::current()));
            for (size_t i = 0; i < self->mViews.size(); ++i)
            {
                ViewportWidget* v = self->mViews[i];
                std::string name = i < names.size() && names[i].is_string() ? names[i].get<std::string>() : std::string();
                const scene::Node* n = name.empty() ? nullptr : findNode(name);
                if (n && n->type().isA("camera"))
                    v->lookThrough(n->id());
                else if (v->lookThroughCamera() != scene::kNoNode)
                    v->stopLookingThrough();
            }
            if (size_t active = section.is_object() ? section.value("active", self->mViews.size()) : self->mViews.size(); active < self->mViews.size())
            {
                self->mActive = self->mViews[active];
                self->relayout();
            }
        });
}

void ViewportLayout::frameUnlessRestored()
{
    if (!mCamerasRestored && mActive)
    {
        mActive->frameSelection(true);
        io::acceptUi("viewports");
    }
    mCamerasRestored = false;
}

void ViewportLayout::relayout()
{
    for (int i = 0; i < 4; ++i)
        mPanels[i]->setVisible(mQuad || mViews[i] == mActive);
    if (mActive)
        mActive->setFocus();
}

void ViewportLayout::setTextureView(TextureView mode)
{
    if (mode == mTextureView)
        return;
    mTextureView = mode;
    PrefStore::get().set("general", kPrefTextureView, int(mode));
    applyTextureView();
    Q_EMIT textureViewChanged();
}

void ViewportLayout::applyTextureView()
{
    const bool texturing = menuContext::layoutSet() == "Texture";
    const bool show2d = texturing && mTextureView != TextureView::View3D;
    if (show2d && !mTexture2d)
    {
        mTexture2d = new Texture2DView(mSplit);
        mSplit->addWidget(mTexture2d);
        mSplit->setStretchFactor(0, 1);
        mSplit->setStretchFactor(1, 1);
        mSplit->setSizes({width() / 2, width() / 2});
    }
    if (mTexture2d)
        mTexture2d->setVisible(show2d);
    mViewsBox->setVisible(!texturing || mTextureView != TextureView::View2D);
}

void ViewportLayout::setQuad(bool on)
{
    if (on == mQuad)
        return;
    mQuad = on;
    PrefStore::get().set("general", "/viewport/quad", on);
    relayout();
}

void ViewportLayout::toggleQuad()
{
    // Like Maya, the view under the cursor is the one that stays (or the one that fills the area).
    if (mQuad)
        for (ViewportWidget* v : mViews)
            if (v->rect().contains(v->mapFromGlobal(QCursor::pos())))
                mActive = v;
    setQuad(!mQuad);
}

bool ViewportLayout::eventFilter(QObject* obj, QEvent* e)
{
    if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::FocusIn)
        if (auto* v = qobject_cast<ViewportWidget*>(obj))
            mActive = v;
    return QWidget::eventFilter(obj, e);
}
} // namespace lila::ui
