#include "ui/UvEditorInternal.h"

#include <QApplication>
#include <QCursor>


namespace lila::ui
{
QColor UvView::gray(int v, int a)
{
    return themeShade(v, a);
}

std::vector<scene::NodeId> UvView::shownMeshes()
{
    return uv::shownMeshes();
}

json UvView::safeRun(const char* name, const json& call, CallSource src)
{
    try
    {
        return ops::execute(name, call, src);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    return nullptr;
}

UvView::UvView(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(240, 200);
    setAttribute(Qt::WA_OpaquePaintEvent);
    // Under the cursor the view's keys (F9-F12 modes) win over the focused panel's and the global ones.
    setProperty("hotkeyContext", "uvEditor");
    setProperty("hotkeyHover", true);
    // Its selection is the meshes' components, so Delete removes them as in the viewport.
    deletion::claimScene(this);
    mUvToolListener = uvTools::addRedrawListener([this] { update(); });
    mTexPaint.onUpdate = [this] { update(); };
}

UvView::~UvView()
{
    uvTools::removeRedrawListener(mUvToolListener);
    cursorBadges::publish(this, {});
}

uvTools::Canvas UvView::canvas() const
{
    return {[this](const glm::dvec2& uv) { return toScreen(uv); }, [this](const QPointF& s) { return toUv(s); }, mZoom};
}

void UvView::setTool(Tool t)
{
    mTool = t;
    update();
}

void UvView::setDisplay(Display d)
{
    mDisplay = d;
    update();
}

void UvView::setShowUdim(bool on)
{
    mUdim = on;
    update();
}

void UvView::setSelectedFacesOnly(bool on)
{
    PrefStore::get().set("general", kSelectedFacesOnlyPref, on);
    update();
}

void UvView::setShowStacks(bool on)
{
    mShowStacks = on;
    update();
}

void UvView::setPackPreview(bool on)
{
    if (mPackPreview == on)
        return;
    mPackPreview = on;
    mPackDirty = true;
    mPackFailed = false;
    // Rebuilt from the meshes, so the real uvs come back when it turns off.
    mMeshes.clear();
    mDrag = Handle::None;
    mSew = {};
    mLiveDrag = {};
    if (on)
        changeTool(Tool::Select);
    frameAll();
}

void UvView::packShown()
{
    LILA_ZONE();

    mPackDirty = false;
    scene::Graph& g = scene::Graph::current();
    std::vector<geo::Mesh> copies;
    std::vector<size_t> packed;
    copies.reserve(mMeshes.size());
    for (size_t mi = 0; mi < mMeshes.size(); ++mi)
        if (!mMeshes[mi].corner.empty())
        {
            copies.push_back(*mMeshes[mi].mesh);
            packed.push_back(mi);
        }
    std::vector<uv::PackTarget> targets(packed.size());
    for (size_t i = 0; i < packed.size(); ++i)
    {
        const MeshUv& mu = mMeshes[packed[i]];
        targets[i].mesh = &copies[i];
        targets[i].faces = mu.only;
        if (scene::Node* n = g.node(mu.node))
            targets[i].linear = glm::dmat3(n->as<glm::dmat4>(scene::Transform::kWorldMatrix));
    }
    const json o = uvPackOptions();
    auto opt = [&](const char* key, auto fallback) { return o.is_object() && o.contains(key) ? o[key].get<decltype(fallback)>() : fallback; };
    uv::PackOptions p;
    p.resolution = std::max(4, opt("resolution", 1024));
    p.padding = std::max(0.0, opt("padding", 4.0));
    p.rotateStep = std::clamp(opt("rotateStep", 90), 0, 180);
    p.freeRotation = opt("freeRotation", false);
    p.flip = opt("flip", false);
    p.preOrient = opt("orient", true);
    p.stack = opt("stack", false);
    // A preview: the quickest search, which lands within a few percent of a full pack's density.
    p.effort = 0;
    uv::PackResult r = uv::packMeshes(targets, mMeshes.empty() ? std::string() : mMeshes.front().set, p);
    if (!r.ok)
    {
        log::warn("UV Editor: Pack Preview could not pack the shown islands; showing the real uvs");
        mPackFailed = true;
        return;
    }
    mPackFailed = false;
    for (size_t i = 0; i < packed.size(); ++i)
    {
        MeshUv& mu = mMeshes[packed[i]];
        std::span<const glm::vec2> uv = copies[i].cornerAttrs().get<glm::vec2>(mu.set);
        for (size_t k = 0; k < mu.corner.size(); ++k)
            mu.uv[k] = glm::dvec2(uv[mu.corner[k]]);
        mu.distortionValid = false;
    }
}

void UvView::frameAll()
{
    sync();
    frameShown();
}

void UvView::frameShown()
{
    glm::dvec2 lo(0.0), hi(1.0);
    bool any = false;
    for (const MeshUv& m : mMeshes)
        for (const glm::dvec2& p : m.uv)
        {
            lo = any ? glm::min(lo, p) : p;
            hi = any ? glm::max(hi, p) : p;
            any = true;
        }
    if (any)
    {
        lo = glm::min(lo, glm::dvec2(0.0));
        hi = glm::max(hi, glm::dvec2(1.0));
    }
    frame(lo, hi);
}

bool UvView::anyShownInView() const
{
    const glm::dvec2 a = toUv(QPointF(0.0, height())), b = toUv(QPointF(width(), 0.0));
    for (const MeshUv& m : mMeshes)
        for (const glm::dvec2& p : m.uv)
            if (p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y)
                return true;
    return false;
}

void UvView::frameSelection()
{
    sync();
    glm::dvec2 lo, hi;
    if (selectionBounds(lo, hi))
        frame(lo, hi);
    else
        frameAll();
}

void UvView::frameUnderCursor()
{
    const glm::dvec2 at = toUv(mapFromGlobal(QCursor::pos().toPointF()));
    PrefStore& prefs = PrefStore::get();
    const double zoom = prefs.value<bool>("general", "/viewport/frameUnderCursor/zoom", true)
                            ? std::max(prefs.value<double>("general", "/viewport/frameUnderCursor/zoomFactor", 8.0), 0.01)
                            : 1.0;
    mFit.reset();
    mZoom = std::clamp(mZoom * zoom, 1.0, 1e7);
    mPan = glm::dvec2(width() * 0.5 - at.x * mZoom, height() * 0.5 + at.y * mZoom);
    update();
}

bool frameUvUnderCursor()
{
    for (QWidget* w = QApplication::widgetAt(QCursor::pos()); w; w = w->parentWidget())
        if (auto* view = dynamic_cast<UvView*>(w))
        {
            view->frameUnderCursor();
            return true;
        }
    return false;
}

void UvView::resizeEvent(QResizeEvent*)
{
    if (width() <= 10)
        return;
    if (!mFramed)
    {
        mFramed = true;
        frameAll();
    }
    else if (mFit)
        frame(mFit->first, mFit->second);
}

void UvView::paintEvent(QPaintEvent*)
{
    LILA_ZONE();

    sync();
    QPainter p(this);
    p.fillRect(rect(), gray(38));
    drawBackground(p);
    p.setRenderHint(QPainter::Antialiasing, false);
    drawFaces(p);
    p.setRenderHint(QPainter::Antialiasing, true);
    drawEdges(p);
    drawPoints(p);
    drawPins(p);
    drawPreselect(p);
    drawManipulator(p);
    drawSewDrag(p);
    drawBrush(p);
    if (!mPackPreview)
        mTexPaint.drawCursor(p, mMousePos, uvOf(), mZoom);
    if (uvTools::UvTool* t = uvTools::active(); t && !mPackPreview)
        t->draw(p, canvas());
    drawStacks(p);
    drawTabBadges(p);
    if (mMarquee)
    {
        p.setPen(QPen(gray(230), 1.0, Qt::DashLine));
        p.setBrush(gray(255, 24));
        p.drawRect(QRectF(mPressPos, mMousePos).normalized());
    }
    drawHud(p);
}

void UvView::wheelEvent(QWheelEvent* e)
{
    double f = std::pow(1.0015, e->angleDelta().y());
    zoomAbout(e->position(), f);
}

void UvView::mousePressEvent(QMouseEvent* e)
{
    setPreselect({});
    setFocus();
    mPressPos = mMousePos = e->position();
    mLastPos = e->position();
    bool alt = e->modifiers() & Qt::AltModifier;
    if (e->button() == Qt::MiddleButton && !alt && !mPackPreview && mTool != Tool::Select && mTool != Tool::Brush && hasSelection())
    {
        beginDrag(Handle::Free);
        return;
    }
    if (e->button() == Qt::MiddleButton || (alt && e->button() == Qt::LeftButton))
    {
        mNav = Nav::Pan;
        return;
    }
    if (alt && e->button() == Qt::RightButton)
    {
        mNav = Nav::Zoom;
        return;
    }
    if (e->button() == Qt::RightButton)
    {
        // Ctrl converts the component selection exactly as in the viewport.
        const char* menu = (e->modifiers() & Qt::ShiftModifier) ? "uvEditorShift" : (e->modifiers() & Qt::ControlModifier) ? "viewportCtrlObject" : "uvEditor";
        MarkingMenu::popup(menu, MarkingMenu::Trigger::MouseButton, Qt::RightButton, e->globalPosition().toPoint());
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    if (mTabHeld && tabSelectable())
    {
        tabSelect::Mode m = tabSelect::modeAtPress(selectedUnder(e->position()), e->modifiers() & Qt::ShiftModifier,
                                                   e->modifiers() & Qt::ControlModifier);
        mTabStroke.begin(m == tabSelect::Mode::Select ? paintSelect::Stroke::Op::Add : paintSelect::Stroke::Op::Remove, pickMode(),
                         pickMode() == ComponentMode::Vertex);
        tabPaint(e->position(), e->position());
        return;
    }
    // Pack Preview shows the uvs away from where they are, so editing on the canvas waits until it is off.
    if (mPackPreview)
    {
        mMods = e->modifiers();
        mMarquee = true;
        return;
    }
    if (mTexPaint.mousePress(e, uvOf(), mZoom))
    {
        update();
        return;
    }
    if (uvTools::UvTool* t = uvTools::active(); t && t->mousePress(e, canvas()))
    {
        mUvToolDrag = true;
        update();
        return;
    }
    if (mTool == Tool::Brush)
    {
        brushPress(e);
        return;
    }
    Handle h = handleAt(e->position());
    if (h != Handle::None)
    {
        beginDrag(h);
        return;
    }
    mMods = e->modifiers();
    if (mTool == Tool::Select && armSewDrag(e->position()))
        return;
    if (mTool == Tool::Select && uvLive::liveMode() && armLiveDrag(e->position()))
        return;
    mMarquee = true;
}

void UvView::mouseMoveEvent(QMouseEvent* e)
{
    QPointF d = e->position() - mLastPos;
    mLastPos = mMousePos = e->position();
    mCursorInside = true;
    if (mTabHeld)
        update();
    if (mNav == Nav::Pan)
    {
        mPan += glm::dvec2(d.x(), d.y());
        mFit.reset();
        update();
        return;
    }
    if (mNav == Nav::Zoom)
    {
        zoomAbout(mPressPos, std::pow(1.006, d.x() - d.y()));
        return;
    }
    if (mTabStroke.active())
    {
        tabPaint(e->position() - d, e->position());
        return;
    }
    if (mDrag != Handle::None)
    {
        updateDrag(e->modifiers());
        update();
        return;
    }
    if (!mPackPreview && (mTexPaint.mouseMove(e, uvOf(), mZoom) || mTexPaint.painting()))
    {
        update();
        if (e->buttons() & Qt::LeftButton)
            return;
    }
    if (uvTools::UvTool* t = uvTools::active(); t && !mPackPreview && (mUvToolDrag || !(e->buttons() & Qt::LeftButton)) && t->mouseMove(e, canvas()))
    {
        update();
        return;
    }
    if (mTool == Tool::Brush)
    {
        brushMove(e);
        update();
        return;
    }
    if (mSew.armed || mLiveDrag.armed)
    {
        QPointF moved = e->position() - mPressPos;
        bool far = std::abs(moved.x()) + std::abs(moved.y()) >= 4.0;
        if (mSew.armed && far)
            mSew.dragging = true;
        if (mLiveDrag.armed && far && !mLiveDrag.started)
            startLiveDrag();
        if (mLiveDrag.started)
            updateLiveDrag();
        update();
        return;
    }
    if (mMarquee)
    {
        update();
        return;
    }
    Handle h = handleAt(e->position());
    if (h != mHover)
    {
        mHover = h;
        update();
    }
    bool on = PrefStore::get().value<bool>("general", "/viewport/preselect/components", true);
    setPreselect(h == Handle::None && on && !(e->modifiers() & Qt::AltModifier) ? hoverAt(e->position()) : scene::componentPick::Hover{});
}

void UvView::leaveEvent(QEvent* e)
{
    if (uvTools::UvTool* t = uvTools::active())
    {
        t->leave();
        update();
    }
    setPreselect({});
    mCursorInside = false;
    update();
    QWidget::leaveEvent(e);
}

void UvView::mouseReleaseEvent(QMouseEvent* e)
{
    if (mNav != Nav::None)
    {
        mNav = Nav::None;
        return;
    }
    if (mTabStroke.active())
    {
        if (e->button() == Qt::LeftButton)
            mTabStroke.end();
        update();
        return;
    }
    if (mTexPaint.mouseRelease(e, uvOf(), mZoom))
    {
        update();
        return;
    }
    if (mDrag != Handle::None)
    {
        commitDrag();
        return;
    }
    if (mUvToolDrag && e->button() == Qt::LeftButton)
    {
        mUvToolDrag = false;
        if (uvTools::UvTool* t = uvTools::active())
            t->mouseRelease(e, canvas());
        update();
        return;
    }
    if (mTool == Tool::Brush && e->button() == Qt::LeftButton)
    {
        brushRelease(e);
        return;
    }
    if (mSew.armed && e->button() == Qt::LeftButton)
    {
        bool dragged = mSew.dragging;
        if (dragged)
            finishSewDrag(e->position());
        mSew = {};
        if (!dragged)
            pickAt(e->position(), mMods);
        update();
        return;
    }
    if (mLiveDrag.armed && e->button() == Qt::LeftButton)
    {
        bool started = mLiveDrag.started;
        if (started)
            endLiveDrag(e->modifiers());
        mLiveDrag = {};
        if (!started)
            pickAt(e->position(), mMods);
        update();
        return;
    }
    if (mMarquee && e->button() == Qt::LeftButton)
    {
        mMarquee = false;
        QPointF delta = e->position() - mPressPos;
        if (std::abs(delta.x()) + std::abs(delta.y()) < 4.0)
            pickAt(e->position(), mMods);
        else
            pickRect(QRectF(mPressPos, e->position()).normalized(), mMods);
        update();
    }
}

void UvView::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || (e->modifiers() & Qt::AltModifier))
        return;
    if (pickMode() == ComponentMode::Edge)
        selectLoopAt(e->position(), e->modifiers());
    else
        selectIslandAt(e->position(), e->modifiers());
}

bool UvView::event(QEvent* e)
{
    if (e->type() == QEvent::FocusOut)
    {
        mTabHeld = false;
        update();
    }
    if ((e->type() == QEvent::KeyPress || e->type() == QEvent::KeyRelease) && mTabHeld)
        if (int key = static_cast<QKeyEvent*>(e)->key(); key == Qt::Key_Control || key == Qt::Key_Shift)
            update();
    // Tab is held for drag selection, as in the viewport, not focus navigation.
    if (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress || e->type() == QEvent::KeyRelease)
        if (auto* k = static_cast<QKeyEvent*>(e); k->key() == Qt::Key_Tab || k->key() == Qt::Key_Backtab)
        {
            if (!k->isAutoRepeat() && e->type() != QEvent::ShortcutOverride)
            {
                mTabHeld = e->type() == QEvent::KeyPress;
                update();
            }
            e->accept();
            return true;
        }
    // Brush keys are held (B, M and F drag size and strength), so they must reach the view, not the hotkeys.
    if (e->type() == QEvent::ShortcutOverride)
    {
        auto* k = static_cast<QKeyEvent*>(e);
        bool brushKey = mTool == Tool::Brush && (k->key() == Qt::Key_B || k->key() == Qt::Key_M || k->key() == Qt::Key_F ||
                                                 k->key() == Qt::Key_BracketLeft || k->key() == Qt::Key_BracketRight);
        if ((brushKey || k->key() == Qt::Key_P) && !(k->modifiers() & (Qt::ControlModifier | Qt::AltModifier)))
        {
            e->accept();
            return true;
        }
    }
    return QWidget::event(e);
}

void UvView::keyPressEvent(QKeyEvent* e)
{
    if (uvTools::UvTool* t = uvTools::active(); t && !mPackPreview && t->keyPress(e, canvas()))
    {
        update();
        return;
    }
    if (mTool == Tool::Brush && brushKey(e, true))
        return;
    switch (e->key())
    {
    case Qt::Key_Q: changeTool(Tool::Select); break;
    case Qt::Key_W: if (!mPackPreview) changeTool(Tool::Move); break;
    case Qt::Key_E: if (!mPackPreview) changeTool(Tool::Rotate); break;
    case Qt::Key_R: if (!mPackPreview) changeTool(Tool::Scale); break;
    case Qt::Key_F: frameSelection(); break;
    case Qt::Key_A: frameAll(); break;
    case Qt::Key_P: if (!mPackPreview) togglePinAt(mMousePos); break;
    default: QWidget::keyPressEvent(e); return;
    }
}

void UvView::keyReleaseEvent(QKeyEvent* e)
{
    if (mTool == Tool::Brush && brushKey(e, false))
        return;
    QWidget::keyReleaseEvent(e);
}

UvTexturePaint::ToUv UvView::uvOf() const
{
    return [this](const QPointF& s) { return toUv(s); };
}

void UvView::frame(glm::dvec2 lo, glm::dvec2 hi)
{
    mFit = {lo, hi};
    glm::dvec2 ext = glm::max(hi - lo, glm::dvec2(1e-6));
    double z = std::min((width() - 40.0) / ext.x, (height() - 60.0) / ext.y);
    mZoom = std::clamp(z, 1.0, 1e7);
    glm::dvec2 c = (lo + hi) * 0.5;
    mPan = glm::dvec2(width() * 0.5 - c.x * mZoom, height() * 0.5 + 10.0 + c.y * mZoom);
    update();
}

void UvView::zoomAbout(QPointF at, double f)
{
    mFit.reset();
    glm::dvec2 before = toUv(at);
    mZoom = std::clamp(mZoom * f, 1.0, 1e7);
    QPointF after = toScreen(before);
    mPan += glm::dvec2(at.x() - after.x(), at.y() - after.y());
    update();
}

void UvView::changeTool(Tool t)
{
    setTool(t);
    if (onToolChanged)
        onToolChanged(t);
}

void UvView::sync()
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const std::string set = uv::currentUvSet();
    std::vector<scene::NodeId> ids = shownMeshes();
    std::vector<std::vector<Index>> only = shownFaces(ids);
    std::vector<MeshUv> next;
    bool added = false, changed = false;
    for (size_t i = 0; i < ids.size(); ++i)
    {
        const scene::NodeId id = ids[i];
        scene::Node* n = g.node(id);
        // The base under any modifiers: component ids and uv edits index it.
        scene::MeshPtr mesh = n && n->type().isA("mesh") ? scene::modifiers::cage(g, id) : nullptr;
        if (!mesh || (only.size() && only[i].empty()))
            continue;
        std::vector<Index> faces = only.size() ? std::move(only[i]) : std::vector<Index>{};
        auto old = std::find_if(mMeshes.begin(), mMeshes.end(), [&](const MeshUv& m) { return m.node == id; });
        // Meshes are immutable once published, so the same pointer means the same uvs.
        if (old != mMeshes.end() && old->mesh == mesh && old->set == set && old->only == faces)
        {
            old->name = n->path();
            next.push_back(std::move(*old));
            continue;
        }
        added |= old == mMeshes.end();
        changed = true;
        next.push_back(build(id, n->path(), mesh, set, std::move(faces)));
    }
    changed |= next.size() != mMeshes.size();
    mMeshes = std::move(next);
    if (mPackPreview && (changed || mPackDirty))
        packShown();
    else if (!mPackPreview)
        applyLive();
    // A mesh newly shown (a new selection) is framed, so its uvs never open off screen; so is an edit, such as a pack, that
    // leaves none of them in view.
    if (mFramed && (added || (changed && !mMeshes.empty() && !anyShownInView())))
        frameShown();
}

void UvView::applyLive()
{
    for (MeshUv& mu : mMeshes)
    {
        uv::live::Session* s = mu.corner.empty() ? nullptr : uv::live::find(mu.node);
        if (!s || s->uvSet() != mu.set)
            continue;
        auto f = s->solver().frame();
        if (f->epoch != s->epoch() || (f->generation == mu.liveGen && f->epoch == mu.liveEpoch))
            continue;
        mu.liveGen = f->generation;
        mu.liveEpoch = f->epoch;
        mu.pinned.assign(mu.corner.size(), 0);
        for (size_t k = 0; k < mu.corner.size(); ++k)
        {
            auto [i, v] = s->wedge(mu.corner[k]);
            if (i >= 0 && size_t(i) < f->islands.size() && f->islands[size_t(i)])
            {
                mu.uv[k] = f->islands[size_t(i)]->uv[v];
                mu.pinned[k] = f->islands[size_t(i)]->pinned[v];
            }
        }
        mu.distortionValid = false;
    }
}

std::vector<std::vector<Index>> UvView::shownFaces(const std::vector<scene::NodeId>& meshes) const
{
    std::vector<std::vector<Index>> out;
    if (scene::components::mode() != ComponentMode::Face || !PrefStore::get().value<bool>("general", kSelectedFacesOnlyPref, false))
        return out;
    bool any = false;
    out.resize(meshes.size());
    for (size_t i = 0; i < meshes.size(); ++i)
    {
        const auto& ids = scene::components::selected(meshes[i], ComponentMode::Face);
        out[i].assign(ids.begin(), ids.end());
        std::sort(out[i].begin(), out[i].end());
        any |= !out[i].empty();
    }
    if (!any)
        out.clear();
    return out;
}

UvView::MeshUv UvView::build(scene::NodeId id, const std::string& name, scene::MeshPtr mesh, const std::string& set,
                             std::vector<Index> only)
{
    LILA_ZONE();

    MeshUv out;
    out.node = id;
    out.name = name;
    out.mesh = mesh;
    out.set = set;
    out.only = std::move(only);
    const geo::Mesh& m = *mesh;
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(set);
    if (uv.empty())
        return out;
    std::span<const bool> seam = m.edgeAttrs().get<bool>(geo::attr::kSeam);
    out.entryOf.assign(m.cornerCapacity(), ~0u);
    std::vector<Index> all;
    if (out.only.empty())
        for (Index f = 0; f < m.faceCapacity(); ++f)
            all.push_back(f);
    for (Index f : out.only.empty() ? all : out.only)
    {
        if (f >= m.faceCapacity() || !m.faceValid(f))
            continue;
        out.faces.push_back(f);
        out.faceStart.push_back(uint32_t(out.corner.size()));
        m.forEachFaceCorner(f, [&](Index c) {
            out.entryOf[c] = uint32_t(out.corner.size());
            out.corner.push_back(c);
            out.uv.push_back(glm::dvec2(uv[c]));
        });
    }
    out.faceStart.push_back(uint32_t(out.corner.size()));
    if (std::span<const bool> pin = m.cornerAttrs().get<bool>("uvPin"); !pin.empty())
    {
        out.pinned.resize(out.corner.size());
        for (size_t k = 0; k < out.corner.size(); ++k)
            out.pinned[k] = pin[out.corner[k]] ? 1 : 0;
    }
    auto same = [](glm::vec2 a, glm::vec2 b) { return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec2(1e-6f))); };
    for (size_t fi = 0; fi + 1 < out.faceStart.size(); ++fi)
        for (uint32_t k = out.faceStart[fi]; k < out.faceStart[fi + 1]; ++k)
        {
            uint32_t k2 = k + 1 < out.faceStart[fi + 1] ? k + 1 : out.faceStart[fi];
            Index c = out.corner[k];
            Index e = m.cornerEdge(c);
            Index r = m.cornerRadial(c);
            bool split = r == c || m.cornerRadial(r) != c;
            if (!split)
            {
                bool sameDir = m.cornerPoint(r) == m.cornerPoint(c);
                glm::vec2 ra = sameDir ? uv[r] : uv[m.cornerNext(r)], rb = sameDir ? uv[m.cornerNext(r)] : uv[r];
                split = !same(ra, uv[c]) || !same(rb, uv[m.cornerNext(c)]);
                // Shared edges are drawn once, from the lower corner, unless its face is hidden.
                if (!split && r < c && out.entryOf[r] != ~0u)
                    continue;
            }
            out.segments.push_back({k, k2, e, split || (!seam.empty() && seam[e])});
        }
    return out;
}

std::vector<uint8_t> UvView::selectedEntries(const MeshUv& mu) const
{
    std::vector<uint8_t> mask(mu.corner.size(), 0);
    if (!mu.mesh || mu.corner.empty())
        return mask;
    const geo::Mesh& m = *mu.mesh;
    ComponentMode mode = scene::components::mode();
    if (mode == ComponentMode::Object)
        return mask;
    const auto& ids = scene::components::selected(mu.node, mode);
    if (ids.empty())
        return mask;
    for (Index c : uv::selectionCorners(m, mu.node, mode, ids, mu.set))
        if (c < mu.entryOf.size() && mu.entryOf[c] != ~0u)
            mask[mu.entryOf[c]] = 1;
    return mask;
}

const geo::SymmetryMap* UvView::symmetryOf(const MeshUv& mu) const
{
    scene::Node* n = scene::Graph::current().node(mu.node);
    const int axis = n && mu.mesh ? uv::uvSymmetryAxis(*n) : -1;
    if (axis != mu.symAxis)
    {
        mu.symAxis = axis;
        mu.sym.reset();
        mu.mirror.reset();
        if (axis >= 0)
            if (std::optional<geo::SymmetryMap> map = uv::uvSymmetryMap(*n, *mu.mesh))
                mu.sym = std::make_shared<const geo::SymmetryMap>(std::move(*map));
    }
    return mu.sym.get();
}

const uv::UvMirror* UvView::mirrorOf(const MeshUv& mu) const
{
    const geo::SymmetryMap* s = symmetryOf(mu);
    if (!s || mu.corner.empty())
        return nullptr;
    if (!mu.mirror)
        mu.mirror = std::make_shared<const uv::UvMirror>(uv::buildUvMirror(*mu.mesh, *s, mu.set));
    return mu.mirror.get();
}

std::vector<Index> UvView::withMirror(scene::NodeId node, ComponentMode mode, const std::vector<Index>& ids) const
{
    auto it = std::find_if(mMeshes.begin(), mMeshes.end(), [&](const MeshUv& m) { return m.node == node; });
    const geo::SymmetryMap* s = it != mMeshes.end() ? symmetryOf(*it) : nullptr;
    return s ? uv::mirrorComponents(*s, *it->mesh, mode, ids) : ids;
}

std::vector<Index> UvView::uvVertices(scene::NodeId node, const std::vector<Index>& corners) const
{
    auto it = std::find_if(mMeshes.begin(), mMeshes.end(), [&](const MeshUv& m) { return m.node == node; });
    if (it == mMeshes.end() || !it->mesh)
        return corners;
    const geo::Mesh& m = *it->mesh;
    std::vector<Index> out = geo::uvVertexCorners(m, it->set, corners);
    if (const geo::SymmetryMap* s = symmetryOf(*it))
        out = geo::uvVertexCorners(m, it->set, uv::mirrorCorners(*s, m, out));
    return out;
}

bool UvView::hasSelection() const
{
    for (const MeshUv& mu : mMeshes)
    {
        auto mask = selectedEntries(mu);
        if (std::find(mask.begin(), mask.end(), uint8_t(1)) != mask.end())
            return true;
    }
    return false;
}

bool UvView::selectionBounds(glm::dvec2& lo, glm::dvec2& hi, bool drivingSide) const
{
    bool any = false;
    for (const MeshUv& mu : mMeshes)
    {
        auto mask = selectedEntries(mu);
        if (const uv::UvMirror* mirror = drivingSide ? mirrorOf(mu) : nullptr)
        {
            std::vector<Index> corners;
            for (size_t i = 0; i < mask.size(); ++i)
                if (mask[i])
                    corners.push_back(mu.corner[i]);
            const int drive = uv::uvMirrorDrive(*mirror, corners);
            bool kept = false;
            for (size_t i = 0; i < mask.size(); ++i)
                kept |= mask[i] && mirror->side[mu.corner[i]] != -drive;
            for (size_t i = 0; i < mask.size() && kept; ++i)
                if (mirror->side[mu.corner[i]] == -drive)
                    mask[i] = 0;
        }
        for (size_t i = 0; i < mask.size(); ++i)
            if (mask[i])
            {
                lo = any ? glm::min(lo, mu.uv[i]) : mu.uv[i];
                hi = any ? glm::max(hi, mu.uv[i]) : mu.uv[i];
                any = true;
            }
    }
    return any;
}

glm::dvec2 UvView::drawnUv(size_t mesh, size_t entry) const
{
    const glm::dvec2& p = mMeshes[mesh].uv[entry];
    if (mDrag == Handle::None || mesh >= mDragMask.size())
        return p;
    if (mesh < mDragFollow.size() && !mDragFollow[mesh].empty() && mDragFollow[mesh][entry].first != ~0u)
    {
        auto& [src, linear] = mDragFollow[mesh][entry];
        const glm::dvec2& ps = mMeshes[mesh].uv[src];
        return p + linear * ((mDragMask[mesh][src] ? dragTransform(ps) : ps) - ps);
    }
    if (!mDragMask[mesh][entry])
        return p;
    return dragTransform(p);
}

glm::dvec2 UvView::dragTransform(const glm::dvec2& p) const
{
    glm::dvec2 d = (p - mPivot) * mDragScale;
    double c = std::cos(mDragAngle), s = std::sin(mDragAngle);
    return mPivot + glm::dvec2(d.x * c - d.y * s, d.x * s + d.y * c) + mDragMove;
}
} // namespace lila::ui
