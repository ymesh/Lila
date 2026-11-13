#include "ui/ViewportWidgetDetail.h"

namespace lila::ui
{
using namespace viewportDetail;

void ViewportWidget::mousePressEvent(QMouseEvent* e)
{
    finishViewTurn();
    clearPreselect();
    mLastPos = e->position();
    if (drone::mousePress(this, e))
        return;
    setFocus();
    if (mFramePickArmed && !(e->modifiers() & Qt::AltModifier))
    {
        mFramePickArmed = false;
        if (e->button() == Qt::LeftButton)
        {
            frameAt(e->position());
            mSwallowRelease = true;
        }
        updateCursor();
        return;
    }
    if (e->button() == Qt::LeftButton && !(e->modifiers() & Qt::AltModifier) && viewSwitcherClick(e->position()))
    {
        mSwallowRelease = true;
        return;
    }
    // The view cube moves the camera like Alt with the same button, each button switchable in the prefs.
    if (!(e->modifiers() & Qt::AltModifier) && hud::shown(mHudPanel, "viewCube") && hud::viewCubeRect(width()).contains(e->position()))
    {
        auto& prefs = PrefStore::get();
        if (e->button() == Qt::LeftButton)
        {
            mCubePress = e->position();
            mCubeDragged = false;
            return;
        }
        if (e->button() == Qt::MiddleButton && prefs.value<bool>("general", "/viewport/viewCube/middleDrag", true))
            return startNav(Nav::Track);
        if (e->button() == Qt::RightButton && prefs.value<bool>("general", "/viewport/viewCube/rightDrag", true))
            return startNav(Nav::Dolly);
    }
    if (e->button() == Qt::LeftButton && !(e->modifiers() & Qt::AltModifier) && viewportHooks::handleClick(e->position(), viewInfo()))
        return;
    if (viewportHooks::pressDrag(e, viewInfo()))
    {
        requestFrame();
        return;
    }
    if (mTabHeld && !viewportTools::active() && e->button() == Qt::LeftButton && !(e->modifiers() & Qt::AltModifier))
    {
        tabSelect::Mode m = tabSelect::modeAtPress(selectedUnder(e->position()), e->modifiers() & Qt::ShiftModifier,
                                                   e->modifiers() & Qt::ControlModifier);
        mTabStroke.begin(m == tabSelect::Mode::Select ? paintSelect::Stroke::Op::Add : paintSelect::Stroke::Op::Remove);
        mTabStroke.paint(viewInfo(), e->position(), e->position(), paintSelect::tabRadius(scene::components::mode()));
        submit();
        return;
    }
    // Brush tools take B + drag for their own radius.
    if (tools::softSelectAdjusting() && (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton) &&
        !(viewportTools::active() && viewportTools::active()->ownsBrushRadius()))
    {
        mSoftDrag = true;
        tools::markSoftSelectAdjusted();
        mPressPos = e->position();
        mSoftStartRadius = e->button() == Qt::MiddleButton ? 0.0 : PrefStore::get().value<double>("general", "/softSelect/radius", 25.0);
        if (e->button() == Qt::MiddleButton)
            PrefStore::get().set("general", "/softSelect/radius", 0.01);
        PrefStore::get().set("general", "/softSelect/enabled", true);
        requestFrame();
        return;
    }
    // Ctrl+Alt is not navigation, so tools can bind it.
    const bool altNav = (e->modifiers() & Qt::AltModifier) && !(e->modifiers() & Qt::ControlModifier);
    if (ViewportTool* tool = viewportTools::active(); tool && !altNav && tool->mousePress(e, viewInfo()))
    {
        mToolDrag = true;
        submit();
        return;
    }
    if (e->button() == Qt::RightButton && !(e->modifiers() & Qt::AltModifier))
        if (Gizmo* gz = drawnGizmo())
        {
            gz->setView(viewInfo());
            if (gz->wantsRightButton(e->position()) && gz->press(e->position(), e->button(), e->modifiers()))
            {
                mGizmoDrag = gz;
                mCursorWrap.reset();
                submit();
                return;
            }
        }
    if (e->button() == Qt::RightButton && !(e->modifiers() & Qt::AltModifier))
    {
        // Maya-style: the menu depends on the modifier and on whether the cursor is over an object.
        bool overObject = !hitTest(e->position(), e->position()).empty();
        std::string base = (e->modifiers() & Qt::ShiftModifier) ? "viewportShift" : (e->modifiers() & Qt::ControlModifier) ? "viewportCtrl" : "viewport";
        QPoint at = e->globalPosition().toPoint();
        if (ViewportTool* tool = viewportTools::active())
            if (const std::string own = tool->markingMenu(base); !own.empty())
                if (json def = markingMenus::resolve(own);
                    def.is_object() && !def.empty() && MarkingMenu::popupDefinition(def, MarkingMenu::Trigger::MouseButton, Qt::RightButton, at))
                    return;
        // Over a skinned mesh, a joint-picking selection handler offers the joints under the cursor instead.
        if (overObject && base == "viewport" && scene::components::mode() == scene::ComponentMode::Object)
            if (selectionHandlers::Hit hit = selectionHit(e->position()); hit.face != selectionHandlers::kNone)
                if (auto cands = selectionHandlers::candidates(hit); !cands.empty())
                {
                    json fallback = markingMenus::resolve(base + "Object");
                    if (!fallback.is_object() || fallback.empty())
                        fallback = markingMenus::resolve(base);
                    if (MarkingMenu::popupDefinition(selectionHandlers::candidateMenu(cands, fallback), MarkingMenu::Trigger::MouseButton,
                                                     Qt::RightButton, at))
                        return;
                }
        // Over a bound mesh, Maya offers Paint Skin Weights in the top left slot.
        if (overObject && base == "viewport")
            if (std::string mesh = skinnedMeshAt(e->position()); !mesh.empty())
                if (json def = markingMenus::resolve(base + "Object"); def.is_object() && !def.contains("NW"))
                {
                    def["NW"] = {{"label", "Paint Skin Weights"},
                                 {"action", {{"python", "lila.nodes.select(" + json(mesh).dump() + ")\nlila.ui.run_action(\"rig.paintWeights\")"}}}};
                    if (MarkingMenu::popupDefinition(def, MarkingMenu::Trigger::MouseButton, Qt::RightButton, at))
                        return;
                }
        bool componentMode = scene::components::mode() != scene::ComponentMode::Object;
        std::string name = markingMenus::viewportMenuName(base, overObject || !scene::Graph::current().selection().empty(), componentMode);
        json def = markingMenus::resolve(name);
        // Slots under the current mode's key, then under curveSelected while a curve is selected, replace the regular ones;
        // their overflow rows go in front of the regular ones.
        static const char* kModeKeys[] = {"objectMode", "vertexMode", "edgeMode", "faceMode"};
        auto overlay = [&def](const char* key) {
            if (const json& over = def.value(key, json()); over.is_object())
                for (auto& [slot, item] : over.items())
                    if (slot != "overflow")
                        def[slot] = item;
                    else if (item.is_array())
                    {
                        json rows = item;
                        for (const json& row : def.value("overflow", json::array()))
                            rows.push_back(row);
                        def["overflow"] = rows;
                    }
        };
        overlay(kModeKeys[int(scene::components::mode())]);
        scene::Graph& graph = scene::Graph::current();
        if (std::ranges::any_of(graph.selection(), [&](scene::NodeId id) { return graph.node(id) && graph.node(id)->type().isA("curve"); }))
            overlay("curveSelected");
        for (const char* key : kModeKeys)
            def.erase(key);
        def.erase("curveSelected");
        if (!MarkingMenu::popupDefinition(def, MarkingMenu::Trigger::MouseButton, Qt::RightButton, at))
            MarkingMenu::popup(base, MarkingMenu::Trigger::MouseButton, Qt::RightButton, at);
        return;
    }
    // Channels highlighted in the channel box turn a middle drag into Maya's virtual slider. Shift+MMB stays the
    // gizmo's pick-the-axis drag while one is drawn.
    Gizmo* shiftGizmo = (e->modifiers() & Qt::ShiftModifier) ? drawnGizmo() : nullptr;
    const bool gizmoAxisDrag = shiftGizmo && shiftGizmo->visible();
    if (e->button() == Qt::MiddleButton && !(e->modifiers() & Qt::AltModifier) && !gizmoAxisDrag &&
        channelSlider::begin(e->globalPosition().toPoint()))
    {
        mChannelDrag = true;
        return;
    }
    if (e->button() == Qt::LeftButton && !(e->modifiers() & Qt::AltModifier) && mNodeHandles.press(e->position(), viewInfo()))
    {
        updateCursor();
        submit();
        return;
    }
    if ((e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton) && !(e->modifiers() & Qt::AltModifier))
        if (Gizmo* gz = drawnGizmo())
        {
            gz->setView(viewInfo());
            if (gz->press(e->position(), e->button(), e->modifiers()))
            {
                mGizmoDrag = gz;
                mCursorWrap.reset();
                submit();
                return;
            }
        }
    if (e->button() == Qt::LeftButton && !(e->modifiers() & Qt::AltModifier))
    {
        mSelecting = true;
        mMarquee = false;
        mPressPos = e->position();
        mLassoDrag = paintSelect::lassoActive();
        mLasso.clear();
        if (mLassoDrag)
            mLasso.push_back({mPressPos.x(), mPressPos.y()});
        return;
    }
    if ((e->modifiers() & Qt::AltModifier) && (e->modifiers() & Qt::ShiftModifier) && e->button() == Qt::RightButton)
    {
        mLightDrag = true;
        mLightDragged = false;
        mPressPos = e->position();
        return;
    }
    if (e->modifiers() & Qt::AltModifier)
    {
        startNav(e->button() == Qt::LeftButton     ? Nav::Tumble
                 : e->button() == Qt::MiddleButton ? Nav::Track
                 : e->button() == Qt::RightButton  ? Nav::Dolly
                                                   : Nav::None);
    }
    setFocus();
}

void ViewportWidget::startNav(Nav nav)
{
    mNav = nav;
    if (mNav != Nav::None && mLookThrough != scene::kNoNode)
    {
        syncLookThrough();
        if (!beginCameraEdit())
            mNav = Nav::None;
    }
    mNavStart = cameraView();
}

void ViewportWidget::mouseMoveEvent(QMouseEvent* e)
{
    QPointF d = e->position() - mLastPos;
    mLastPos = e->position();
    if (drone::mouseMove(this, e))
        return;
    if (viewportHooks::dragging())
    {
        viewportHooks::moveDrag(e, viewInfo());
        requestFrame();
        return;
    }
    // Badges follow the cursor, which otherwise only redraws the view when the hover changes.
    if (e->buttons() == Qt::NoButton && !cursorBadges::active(state(e->position())).empty())
        requestFrame();
    if (e->buttons() == Qt::NoButton && hud::shown(mHudPanel, "viewCube"))
        if (const QRectF cube = hud::viewCubeRect(width()); cube.contains(e->position()) || cube.contains(mLastPos - d))
            requestFrame();
    if (mCubePress && (e->buttons() & Qt::LeftButton))
    {
        if (!mCubeDragged && (e->position() - *mCubePress).manhattanLength() > 3.0 &&
            PrefStore::get().value<bool>("general", "/viewport/viewCube/leftDrag", true))
        {
            mCubeDragged = true;
            startNav(Nav::Tumble);
        }
        if (mNav == Nav::None)
            return;
    }
    if (mTabStroke.active())
    {
        mTabStroke.paint(viewInfo(), mLastPos - d, e->position(), paintSelect::tabRadius(scene::components::mode()));
        submit();
        return;
    }
    if (mLightDrag)
    {
        mLightDragged = mLightDragged || (e->position() - mPressPos).manhattanLength() > QApplication::startDragDistance();
        if (!mLightDragged)
            return;
        viewportDisplay::turnLight(d.x() * 0.5);
        viewportDisplay::tiltLight(d.y() * 0.3);
        requestFrame();
        return;
    }
    if (mSoftDrag)
    {
        // Drag right to grow the falloff, left to shrink; scaled by how big a pixel is at the selection.
        double upp = viewInfo().unitsPerPixel(scene::symmetricSelectionCenter());
        double r = std::max(0.01, mSoftStartRadius + (e->position().x() - mPressPos.x()) * upp);
        PrefStore::get().set("general", "/softSelect/radius", r);
        requestFrame();
        return;
    }
    if (ViewportTool* tool = viewportTools::active(); tool && mNav == Nav::None && tool->mouseMove(e, viewInfo()))
    {
        submit();
        return;
    }
    if (mChannelDrag)
    {
        channelSlider::drag(e->globalPosition().toPoint(), e->modifiers());
        return;
    }
    if (mNodeHandles.dragging())
    {
        mNodeHandles.drag(e->position(), viewInfo(), e->modifiers());
        requestFrame();
        return;
    }
    if (Gizmo* gz = mGizmoDrag)
    {
        const glm::dvec2 at = mCursorWrap.map({e->position().x(), e->position().y()});
        const QPointF p(at.x, at.y);
        gz->setView(viewInfo());
        gz->drag(p, e->modifiers());
        if (!gz->dragging())
            mGizmoDrag = nullptr;
        if (gz->wrapsCursor())
            wrapCursor(p);
        // One frame for however many moves arrived since the last: drawing on every event backs the queue up and lags.
        requestFrame();
        return;
    }
    if (e->buttons() == Qt::NoButton && mTabHeld && !viewportTools::active())
        updateTabHover(e->position());
    if (e->buttons() == Qt::NoButton)
    {
        const bool handleChanged = mNodeHandles.hover(e->position(), viewInfo());
        Gizmo* gz = drawnGizmo();
        if (gz)
        {
            gz->setView(viewInfo());
            // A node handle under the cursor takes the press, so the manipulator shows nothing hovered.
            if (gz->hover(mNodeHandles.hovering() ? QPointF(-1e6, -1e6) : e->position()) || handleChanged)
            {
                updateCursor();
                requestFrame();
            }
        }
        else if (handleChanged)
        {
            updateCursor();
            requestFrame();
        }
        if (mNodeHandles.hovering() || (gz && gz->hovering()) || (e->modifiers() & Qt::AltModifier) || viewportTools::active())
            clearPreselect();
        else
            queuePreselect(e->position());
        return;
    }
    if (mSelecting)
    {
        QPointF drag = e->position() - mPressPos;
        // A click that wanders a few pixels stays a click: a box that small around a point also takes any point sitting on it.
        mMarquee = mMarquee || std::abs(drag.x()) + std::abs(drag.y()) > QApplication::startDragDistance();
        if (mLassoDrag)
            geo::lasso::extend(mLasso, {e->position().x(), e->position().y()});
        if (mMarquee)
            submit();
        return;
    }
    switch (mNav)
    {
    case Nav::Tumble:
        // Orthographic views pan instead of tumbling, like Maya.
        if (mCamera.ortho)
            mCamera.track(d.x() / width(), d.y() / height(), double(width()) / height());
        else if (auto pivot = viewportTools::orbitPoint();
                 pivot && mLookThrough == scene::kNoNode && PrefStore::get().value<bool>("general", viewportTools::kOrbitStrokePref, true))
            mCamera.tumbleAbout(*pivot, d.x() * 0.4, d.y() * 0.4);
        else
            mCamera.tumble(d.x() * 0.4, d.y() * 0.4);
        break;
    case Nav::Track: mCamera.track(d.x() / width(), d.y() / height(), double(width()) / height()); break;
    case Nav::Dolly: mCamera.dolly(std::exp(-(d.x() + d.y()) * 0.005)); break;
    case Nav::None: return;
    }
    if (mLookThrough != scene::kNoNode)
        previewCameraEdit();
    requestFrame();
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (drone::mouseRelease(this, e))
        return;
    // Runs after this release is handled, so it sees the new selection and hilite.
    if (e->buttons() == Qt::NoButton && !(e->modifiers() & Qt::AltModifier))
        queuePreselect(e->position());
    if (viewportHooks::releaseDrag(e, viewInfo()))
    {
        requestFrame();
        return;
    }
    if (mTabStroke.active())
    {
        if (e->button() == Qt::LeftButton)
        {
            mTabStroke.end();
            updateTabHover(e->position());
        }
        submit();
        return;
    }
    if (mCubePress && e->button() == Qt::LeftButton)
    {
        const QPointF press = *std::exchange(mCubePress, std::nullopt);
        if (mNav != Nav::None)
        {
            recordCamera(mNavStart);
            commitCameraEdit();
            mNav = Nav::None;
            Q_EMIT viewStateChanged();
        }
        else if (!mCubeDragged)
            viewCubeClick(press);
        return;
    }
    if (mSwallowRelease && e->button() == Qt::LeftButton)
    {
        mSwallowRelease = false;
        mSelecting = false;
        return;
    }
    if (mSoftDrag)
    {
        mSoftDrag = false;
        requestFrame();
        return;
    }
    if (mLightDrag && e->button() == Qt::RightButton)
    {
        mLightDrag = false;
        // A click without a drag puts the light back.
        if (!mLightDragged)
        {
            viewportDisplay::resetLight();
            requestFrame();
        }
        return;
    }
    if (ViewportTool* tool = viewportTools::active(); tool && mToolDrag)
    {
        mToolDrag = false;
        tool->mouseRelease(e, viewInfo());
        submit();
        return;
    }
    if (mNav != Nav::None)
    {
        recordCamera(mNavStart);
        commitCameraEdit();
    }
    mNav = Nav::None;
    if (mChannelDrag && e->button() == Qt::MiddleButton)
    {
        mChannelDrag = false;
        channelSlider::end();
        return;
    }
    if (mNodeHandles.dragging() && e->button() == Qt::LeftButton)
    {
        mNodeHandles.release();
        updateCursor();
        submit();
        return;
    }
    if (mGizmoDrag && (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton || e->button() == Qt::RightButton))
    {
        Gizmo* gz = mGizmoDrag;
        mGizmoDrag = nullptr;
        gz->release();
        submit();
        return;
    }
    if (mSelecting && e->button() == Qt::LeftButton)
    {
        mSelecting = false;
        bool wasMarquee = mMarquee;
        mMarquee = false;
        if (wasMarquee && mLassoDrag)
            pickLasso(e->modifiers());
        else
            pick(mPressPos, wasMarquee ? e->position() : mPressPos, e->modifiers());
        mLassoDrag = false;
        mLasso.clear();
        submit();
    }
}

void ViewportWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    // The gizmo is ignored: the first click moved it under the cursor onto the component it selected.
    if (e->button() == Qt::LeftButton && !(e->modifiers() & Qt::AltModifier) && !viewportTools::active() &&
        !mTabHeld && scene::components::mode() != scene::ComponentMode::Object &&
        selectLoopAt(e->position(), e->modifiers()))
    {
        mSwallowRelease = true;
        mSelecting = false;
        submit();
        return;
    }
    mousePressEvent(e);
}

bool ViewportWidget::selectLoopAt(QPointF at, Qt::KeyboardModifiers mods)
{
    using scene::ComponentMode;
    ComponentMode mode = scene::components::mode();
    scene::componentPick::Hover h = componentAt(at);
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = h.empty() ? nullptr : g.node(h.node);
    scene::MeshPtr mesh = n ? scene::modifiers::cage(g, h.node) : nullptr;
    if (!mesh)
        return false;
    const bool symmetric = PrefStore::get().value<bool>("general", "/select/symmetric", true);
    const bool shift = mods & Qt::ShiftModifier, ctrl = mods & Qt::ControlModifier;
    // Matches single clicks: Ctrl removes, Shift+Ctrl adds, and Shift toggles, so a Shift click that deselected the edge removes its loop.
    const char* op = ctrl && shift ? "add" : ctrl ? "deselect" : nullptr;
    if (shift && !ctrl && !std::ranges::binary_search(scene::components::selected(h.node, mode), h.id))
        op = "deselect";
    auto addConnected = [&] {
        ops::execute("selectConnected",
                     {{"args", {n->path()}}, {"kind", scene::components::modeName(mode)}, {"id", h.id}, {"add", true}, {"symmetric", symmetric}},
                     CallSource::UI);
    };
    try
    {
        // A Ctrl click before this one on the same loop anchors a run: only the edges between the two are removed.
        if (ctrl && !shift && mode == ComponentMode::Edge)
        {
            const scene::componentPick::Hover& prev = mClicked[0] == h ? mClicked[1] : mClicked[0];
            if (prev.node == h.node && prev.mode == mode && prev.id != h.id &&
                !std::ranges::binary_search(scene::components::selected(h.node, mode), prev.id) &&
                std::ranges::count(geo::edgeLoop(*mesh, h.id), prev.id) != 0)
            {
                ops::execute("selectPathBetween",
                             {{"args", {n->path()}}, {"kind", "edge"}, {"from", prev.id}, {"to", h.id}, {"deselect", true}, {"symmetric", symmetric}},
                             CallSource::UI);
                return true;
            }
        }
        if (op)
        {
            json call = {{"args", {n->path()}}, {op, true}, {"symmetric", symmetric}};
            if (mode == ComponentMode::Edge)
            {
                call["edge"] = h.id;
                ops::execute("selectLoopThrough", call, CallSource::UI);
            }
            else
            {
                call["kind"] = scene::components::modeName(mode);
                call["id"] = h.id;
                ops::execute("selectConnected", call, CallSource::UI);
            }
            return true;
        }
        if (shift)
        {
            // The double-click's own first click is mClicked[0]; the one before it anchors the path.
            const scene::componentPick::Hover& prev = mClicked[0] == h ? mClicked[1] : mClicked[0];
            std::optional<uint32_t> from;
            const auto selectedNow = scene::components::selected(h.node, mode);
            // A click from before a marquee or deselect can be stale, so the anchor must still be selected.
            if (prev.node == h.node && prev.mode == mode && prev.id != h.id && std::ranges::binary_search(selectedNow, prev.id))
                from = prev.id;
            else
                for (uint32_t id : selectedNow)
                    if (id != h.id)
                    {
                        from = id;
                        break;
                    }
            // Nothing selected on this mesh to run a path from: vertices and faces add their connected piece.
            if (!from && mode == ComponentMode::Edge)
                return false;
            if (!from)
            {
                addConnected();
                return true;
            }
            // Like Maya, a second edge adds its own loop unless it shares one with the first, then only the run between them.
            if (mode == ComponentMode::Edge && std::ranges::count(geo::edgeLoop(*mesh, h.id), *from) == 0)
            {
                ops::execute("selectLoopThrough", {{"args", {n->path()}}, {"edge", h.id}, {"add", true}, {"symmetric", symmetric}}, CallSource::UI);
                return true;
            }
            json path = {{"args", {n->path()}}, {"kind", scene::components::modeName(mode)}, {"from", *from}, {"to", h.id}, {"symmetric", symmetric}};
            if (mode == ComponentMode::Edge)
            {
                ops::execute("selectPathBetween", path, CallSource::UI);
                return true;
            }
            // A separate piece of the same mesh has no path to the anchor, so it is added whole.
            try
            {
                ops::execute("selectPathBetween", path, CallSource::UI);
            }
            catch (const CommandError&)
            {
                addConnected();
            }
            return true;
        }
        // Edges select their loop; vertices and faces the whole connected piece.
        if (mode == ComponentMode::Edge)
            ops::execute("selectLoopThrough", {{"args", {n->path()}}, {"edge", h.id}, {"symmetric", symmetric}}, CallSource::UI);
        else
            ops::execute("selectConnected", {{"args", {n->path()}}, {"kind", scene::components::modeName(mode)}, {"id", h.id}, {"symmetric", symmetric}},
                         CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    return true;
}

void ViewportWidget::wheelEvent(QWheelEvent* e)
{
    finishViewTurn();
    if (drone::wheel(this, e))
        return;
    if (viewportHooks::handleWheel(e, viewInfo()))
    {
        requestFrame();
        return;
    }
    if (ViewportTool* tool = viewportTools::active(); tool && tool->wheel(e, viewInfo()))
    {
        submit();
        return;
    }
    if (Gizmo* gz = currentGizmo(); gz && gz->dragging() && e->angleDelta().y() != 0 && gz->wheel(e->angleDelta().y() > 0 ? 1 : -1))
    {
        submit();
        return;
    }
    if (mLookThrough != scene::kNoNode)
    {
        if (!beginCameraEdit())
            return;
        // Restarted per click, so a run of clicks commits (and auto keys) once.
        mWheelCommit->start();
    }
    CameraView before = cameraView();
    mCamera.dolly(std::exp(-e->angleDelta().y() / 120.0 * 0.12));
    if (mLookThrough != scene::kNoNode)
        previewCameraEdit();
    // A run of wheel clicks is one camera undo step.
    recordCamera(before, 1);
    if (e->buttons() == Qt::NoButton)
        queuePreselect(e->position());
    submit();
}

void ViewportWidget::keyPressEvent(QKeyEvent* e)
{
    if (ViewportTool* tool = viewportTools::active(); tool && tool->keyPress(e, viewInfo()))
    {
        submit();
        return;
    }
    if (Gizmo* gz = currentGizmo(); gz && gz->keyPress(e))
    {
        mGizmoDrag = gz->dragging() ? gz : nullptr;
        submit();
        return;
    }
    QWidget::keyPressEvent(e);
}

void ViewportWidget::escape()
{
    if (mFramePickArmed)
    {
        mFramePickArmed = false;
        updateCursor();
        return;
    }
    if (mNodeHandles.dragging())
    {
        mNodeHandles.cancel();
        updateCursor();
        submit();
        return;
    }
    QKeyEvent e(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    if (ViewportTool* tool = viewportTools::active(); tool && tool->keyPress(&e, viewInfo()))
    {
        submit();
        return;
    }
    if (Gizmo* gz = currentGizmo(); gz && gz->keyPress(&e))
    {
        mGizmoDrag = gz->dragging() ? gz : nullptr;
        submit();
        return;
    }
    // An Escape nothing else wanted drops the object selection.
    if (!viewportTools::active() && scene::components::mode() == scene::ComponentMode::Object && !scene::Graph::current().selection().empty())
        actions::run("edit.deselect");
}

} // namespace lila::ui
