#include "ui/TexGraphPreview.h"

#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Node.h"
#pragma push_macro("slots")
#undef slots
#include "tex/TextureSet.h"
#pragma pop_macro("slots")
#include "ui/Controls.h"
#include "ui/Icons.h"
#include "ui/MaterialThumbnails.h"
#include "ui/NodeEditor.h"
#include "ui/TextureGraphUi.h"
#include "ui/UiCommon.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <format>

namespace lila::ui
{
namespace
{
constexpr int kPreviewRender = 256;
constexpr int kPreviewMaxSide = 260;
} // namespace

TexGraphPreview::TexGraphPreview(NodeEditor& editor, QWidget* parent) : QWidget(parent), mEditor(editor)
{
    setObjectName("texGraphPreview");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(10, 4, 10, 4);
    v->setSpacing(4);

    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(2);
    auto* title = new QLabel("Preview", this);
    QFont f = title->font();
    f.setBold(true);
    title->setFont(f);
    bar->addWidget(title);
    bar->addStretch(1);
    mShapes = new controls::Segmented(this);
    mShapes->add("Ball", {}, "Ball\nThe preview ball, with wear and dirt on its edges from its own mesh maps", int(Shape::Ball));
    mShapes->add("Mesh", {}, "Mesh\nThe first mesh whose material uses the set", int(Shape::Mesh));
    mShapes->setCurrent(int(Shape::Ball));
    connect(mShapes, &controls::Segmented::picked, this, [this](int id) { setShape(Shape(id)); });
    bar->addWidget(mShapes);
    mPin = new QToolButton(this);
    mPin->setCheckable(true);
    mPin->setIcon(icon("pin"));
    mPin->setToolTip("Pin\nKeep showing this set while the editor opens others");
    connect(mPin, &QToolButton::toggled, this, [this](bool on) {
        mPin->setIcon(icon(on ? "pinned" : "pin"));
        if (!on)
            follow();
    });
    bar->addWidget(mPin);
    v->addLayout(bar);

    mView = new QLabel(this);
    mView->setAlignment(Qt::AlignCenter);
    mView->setMinimumHeight(120);
    v->addWidget(mView);
    mStatus = new QLabel(this);
    mStatus->setStyleSheet("color: rgb(150,150,150);");
    mStatus->setAlignment(Qt::AlignCenter);
    mStatus->setWordWrap(true);
    mStatus->hide();
    v->addWidget(mStatus);

    mListener = NodeEditor::addGroupListener([this](NodeEditor& e) {
        if (&e == &mEditor)
            follow();
    });
    mPoll = new QTimer(this);
    mPoll->setInterval(400);
    connect(mPoll, &QTimer::timeout, this, [this] { poll(); });
    mPoll->start();
    follow();
}

TexGraphPreview::~TexGraphPreview() { NodeEditor::removeGroupListener(mListener); }

void TexGraphPreview::setPinned(bool on) { mPin->setChecked(on); }

bool TexGraphPreview::pinned() const { return mPin->isChecked(); }

void TexGraphPreview::setShape(Shape shape)
{
    mShape = shape;
    mShapes->setCurrent(int(shape));
    poll();
}

std::string TexGraphPreview::mesh() const
{
    scene::Graph& g = scene::Graph::current();
    if (mSet.empty())
        return {};
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n || !n->type().isA("mesh"))
            continue;
        scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        if (!m)
            continue;
        const size_t slots = std::max<size_t>(m->materialNames().size(), 1);
        for (size_t i = 0; i < slots; ++i)
            if (textureSetForSlot(g, scene::materials::slotName(*m, int32_t(i))) == mSet)
                return n->path();
    }
    return {};
}

QString TexGraphPreview::status() const { return mStatus->isVisible() ? mStatus->text() : QString(); }

void TexGraphPreview::follow()
{
    if (pinned() && tex::findSet(scene::Graph::current(), mSet))
        return;
    const std::string set = tex::splitAddress(mEditor.group().toStdString()).set;
    if (set == mSet)
        return;
    mSet = set;
    poll();
}

void TexGraphPreview::setStatus(const QString& text)
{
    mStatus->setText(text);
    mStatus->setVisible(!text.isEmpty());
}

void TexGraphPreview::poll()
{
    if (!isVisible())
        return;
    scene::Node* n = mSet.empty() ? nullptr : tex::findSet(scene::Graph::current(), mSet);
    if (!n)
    {
        mImage = {};
        mView->clear();
        mShownKey.clear();
        setStatus("No texture set");
        return;
    }
    const std::string mesh = mShape == Shape::Mesh ? this->mesh() : std::string();
    if (mShape == Shape::Mesh && mesh.empty())
    {
        mImage = {};
        mView->clear();
        mShownKey.clear();
        setStatus("No mesh uses " + qs(mSet));
        return;
    }
    const std::string key = std::format("{}|{}|{}", mSet, mesh, tex::setStamp(*n));
    if (key != mShownKey && mBusyKey.empty())
        render(key);
}

void TexGraphPreview::render(const std::string& key)
{
    mBusyKey = key;
    if (mImage.isNull())
        setStatus("Rendering");
    materialThumbs::Options o;
    o.size = kPreviewRender;
    if (mShape == Shape::Mesh)
        o.mesh = mesh();
    QPointer<TexGraphPreview> self(this);
    materialThumbs::renderAsync(mSet, o, [self, key](const materialThumbs::Result& r, const std::string& error) {
        if (!self)
            return;
        self->mBusyKey.clear();
        self->mShownKey = key;
        if (!error.empty())
        {
            self->setStatus(qs(error));
            return;
        }
        QPixmap image(qs(r.file.string()));
        if (image.isNull())
        {
            self->setStatus("No image");
            return;
        }
        self->mImage = image;
        self->setStatus({});
        self->showImage();
        // Edits made while it rendered are picked up now rather than at the next tick.
        self->poll();
    });
}

void TexGraphPreview::showImage()
{
    if (mImage.isNull())
        return;
    const int side = std::min(kPreviewMaxSide, std::max(64, width() - 16));
    QPixmap scaled = mImage.scaled(side * devicePixelRatio(), side * devicePixelRatio(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(devicePixelRatio());
    mView->setPixmap(scaled);
}

void TexGraphPreview::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    showImage();
}
} // namespace lila::ui
