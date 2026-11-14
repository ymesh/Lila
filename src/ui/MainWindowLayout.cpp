#include "ui/MainWindowDetail.h"

namespace lila::ui
{
using namespace mainWindowDetail;

void MainWindow::restoreLayout()
{
    mRestoring = true;
    mDefaultState = saveState();
    PrefStore& p = PrefStore::get();
    std::string geometry = p.value<std::string>(kLayoutFile, "/geometry", "");
    std::string state = p.value<int>(kLayoutFile, "/version", 1) >= kLayoutVersion ? p.value<std::string>(kLayoutFile, "/state", "") : "";
    if (!geometry.empty())
        restoreGeometry(QByteArray::fromBase64(QByteArray::fromStdString(geometry)));
    else
    {
        // First run: the largest 16:9 that fits in most of the screen, centred.
        const QRect avail = screen()->availableGeometry();
        const QSize size = QSize(16, 9).scaled(avail.size() * 0.85, Qt::KeepAspectRatio);
        setGeometry(QRect(avail.center() - QPoint(size.width() / 2, size.height() / 2), size));
    }
    if (!state.empty())
    {
        std::vector<std::string> tearOffs;
        for (const json& name : p.value(kLayoutFile, "/tearOffs", json::array()))
            if (name.is_string())
                tearOffs.push_back(name.get<std::string>());
        tearoff::recreate(this, tearOffs);
        // The saved state already has every own-column dock where it was; arranged on first show, they would be placed
        // from geometry the restored layout hasn't applied yet.
        holdOwnColumns(this);
        restoreState(QByteArray::fromBase64(QByteArray::fromStdString(state)));
        panels::hideToolPalettes(this);
        toolBox::place(this);
        QStringList collapsed;
        for (auto& name : p.value(kLayoutFile, "/collapsed", json::array()))
            if (name.is_string())
                collapsed << QString::fromStdString(name.get<std::string>());
        mSidebars->restoreCollapsed(collapsed);
        QList<QStringList> drawers;
        for (const json& group : p.value(kLayoutFile, "/collapsedGroups", json::array()))
        {
            QStringList names;
            if (group.is_array())
                for (const json& n : group)
                    if (n.is_string())
                        names << QString::fromStdString(n.get<std::string>());
            drawers << names;
        }
        mSidebars->restoreCollapsedGroups(drawers);
        std::map<QString, int> tabSizes;
        json savedSizes = p.value(kLayoutFile, "/tabSizes", json::object());
        for (auto& [name, size] : savedSizes.items())
            if (size.is_number_integer())
                tabSizes[QString::fromStdString(name)] = size.get<int>();
        mSidebars->setTabSizes(std::move(tabSizes));
        for (const json& entry : p.value(kLayoutFile, "/maximizeHidden", json::array()))
            if (auto* d = entry.is_object() ? findChild<QDockWidget*>(QString::fromStdString(entry.value("name", ""))) : nullptr; d && d->isHidden())
                mMaximizeHidden.emplace_back(d, entry.value("showing", false));
    }
    else
        QTimer::singleShot(0, this, [this] {
            applyDefaultDockSizes();
            mSidebars->restoreCollapsed(kDefaultCollapsed);
            run("applyLayout", {{"name", "Model"}});
        });
    QTimer::singleShot(0, this, &MainWindow::fitToScreen);
    mRestoring = false;
}

void MainWindow::applyDefaultDockSizes()
{
    auto* tools = findChild<QDockWidget*>(kModelingToolkitDock);
    auto* outliner = findChild<QDockWidget*>("outliner");
    auto* channels = findChild<QDockWidget*>(kChannelBoxDock);
    if (tools && tools->isVisible())
        resizeDocks({tools}, {kToolsColumnWidth}, Qt::Horizontal);
    if (outliner && outliner->isVisible())
        resizeDocks({outliner}, {kDataColumnWidth}, Qt::Horizontal);
    if (outliner && channels && outliner->isVisible() && channels->isVisible())
        resizeDocks({outliner, channels}, {height() * 9 / 25, height() * 16 / 25}, Qt::Vertical);
}

void MainWindow::applyChromePrefs()
{
    PrefStore& p = PrefStore::get();
    menuBar()->setVisible(p.value<bool>("general", "/ui/showMenuBar", true));
    QDockWidget* shelf = findChild<QDockWidget*>("shelf");
    bool showShelf = p.value<bool>("general", "/ui/showShelf", false);
    if (!shelf)
        return;
    shelf->toggleViewAction()->setEnabled(showShelf);
    shelf->toggleViewAction()->setToolTip(showShelf ? QString() : "Hidden by Preferences > Interface > Show shelf. ` still opens the floating shelf.");
    if (showShelf == mShelfShown)
        return;
    mShelfShown = showShelf;
    if (showShelf)
        shelf->show();
    else
        mSidebars->close(shelf);
}

void MainWindow::createCameraFromView()
{
    const ViewportWidget* view = mViewports->active();
    const Camera& c = view->camera();
    const scene::Node* through = scene::Graph::current().node(view->lookThroughCamera());
    auto vec = [](const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); };
    try
    {
        UndoChunk chunk("Create Camera from View");
        std::string name = ops::execute("createNode", {{"args", {"camera"}}}, CallSource::UI).get<std::string>();
        auto set = [&](const char* attr, json value) { ops::execute("setAttr", {{"args", {name + "." + attr, std::move(value)}}}, CallSource::UI); };
        if (through)
        {
            for (int attr = scene::Camera::kFocalLength; attr < scene::Camera::kCount; ++attr)
                if (attr != scene::Camera::kLocked && attr != scene::Camera::kBookmarks)
                    set(through->type().attrs[size_t(attr)].name.c_str(), scene::toJson(through->value(attr)));
        }
        else
        {
            set("focalLength", c.focalLength);
            set("horizontalFilmAperture", c.horizontalAperture / 25.4);
            set("verticalFilmAperture", c.verticalAperture / 25.4);
            set("filmFit", int(c.filmFit));
            set("nearClipPlane", c.nearPlane());
            if (c.ortho)
            {
                set("orthographic", true);
                set("orthographicWidth", c.orthoWidth);
            }
        }
        // Orthographic views sit far out along their axis; the camera only needs to be clear of the scene.
        glm::dvec3 eye = c.ortho && !through ? c.target - c.forward() * 1000.0 : c.eye();
        ops::execute("viewPlace", {{"args", {name}}, {"eye", vec(eye)}, {"lookAt", vec(c.target)}, {"up", vec(c.viewUp())}},
                      CallSource::UI);
        ops::execute("selectMode", {{"args", {"object"}}}, CallSource::UI);
        ops::execute("select", {{"args", {name}}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

void MainWindow::renameSelection()
{
    json selection = ops::execute("ls", {{"selection", true}});
    if (!selection.is_array() || selection.empty())
        return;
    std::vector<std::string> names;
    for (const json& n : selection)
        names.push_back(n.get<std::string>());

    auto* edit = new QLineEdit(this);
    edit->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    edit->setAttribute(Qt::WA_DeleteOnClose);
    edit->setText(QString::fromStdString(names.front()));
    edit->selectAll();
    edit->setToolTip(names.size() > 1 ? "Renames every selected object; Lila numbers duplicates" : "Enter to rename, Esc to cancel");
    edit->setMinimumWidth(220);
    edit->move(QCursor::pos() - QPoint(20, edit->sizeHint().height() / 2));
    edit->installEventFilter(new EscapeCloses(edit, "edit.rename"));
    connect(edit, &QLineEdit::returnPressed, this, [this, edit, names] {
        std::string to = edit->text().trimmed().toStdString();
        edit->close();
        if (to.empty() || to == names.front())
            return;
        UndoChunk chunk("Rename");
        for (const std::string& from : names)
            run("rename", {{"args", {from, to}}});
    });
    edit->show();
    edit->setFocus();
}

void MainWindow::resetLayout()
{
    mMaximizeHidden.clear();
    mSidebars->expandAll();
    restoreState(mDefaultState);
    toolBox::place(this);
    for (QDockWidget* d : findChildren<QDockWidget*>())
        if (tearoff::parseDockName(d->objectName().toStdString()))
            d->hide();
    applyDefaultDockSizes();
    mSidebars->restoreCollapsed(kDefaultCollapsed);
    fitToScreen();
    scheduleLayoutSave();
}

void MainWindow::fitToScreen()
{
    QRect avail = screen()->availableGeometry();
    if (geometry().width() <= avail.width() && geometry().height() <= avail.height())
        return;
    if (isMaximized())
    {
        // Re-maximizing is what makes Windows drop the oversized frame; resizing a maximized window does nothing.
        showNormal();
        showMaximized();
        return;
    }
    QMargins frame(geometry().left() - frameGeometry().left(), geometry().top() - frameGeometry().top(),
                   frameGeometry().right() - geometry().right(), frameGeometry().bottom() - geometry().bottom());
    setGeometry(avail.marginsRemoved(frame));
}

void MainWindow::scheduleLayoutSave()
{
    if (!mRestoring && mLayoutTimer)
        mLayoutTimer->start();
}

void MainWindow::saveLayout()
{
    PrefStore& p = PrefStore::get();
    p.set(kLayoutFile, "/geometry", saveGeometry().toBase64().toStdString());
    p.set(kLayoutFile, "/state", saveState().toBase64().toStdString());
    json collapsed = json::array();
    for (const QString& name : mSidebars->collapsedNames())
        collapsed.push_back(name.toStdString());
    p.set(kLayoutFile, "/collapsed", collapsed);
    json collapsedGroups = json::array();
    for (const QStringList& group : mSidebars->collapsedGroups())
    {
        json names = json::array();
        for (const QString& name : group)
            names.push_back(name.toStdString());
        collapsedGroups.push_back(std::move(names));
    }
    p.set(kLayoutFile, "/collapsedGroups", collapsedGroups);
    json tabSizes = json::object();
    for (auto& [name, size] : mSidebars->tabSizes())
        tabSizes[name.toStdString()] = size;
    p.set(kLayoutFile, "/tabSizes", tabSizes);
    json tearOffs = json::array();
    for (QDockWidget* d : findChildren<QDockWidget*>())
        if (tearoff::parseDockName(d->objectName().toStdString()) && (d->toggleViewAction()->isChecked() || mSidebars->isCollapsed(d)))
            tearOffs.push_back(d->objectName().toStdString());
    p.set(kLayoutFile, "/tearOffs", tearOffs);
    json maximizeHidden = json::array();
    for (auto& [d, showing] : mMaximizeHidden)
        if (d)
            maximizeHidden.push_back({{"name", d->objectName().toStdString()}, {"showing", showing}});
    p.set(kLayoutFile, "/maximizeHidden", maximizeHidden);
    p.set(kLayoutFile, "/version", kLayoutVersion);
}

void MainWindow::resizeEvent(QResizeEvent* e)
{
    QMainWindow::resizeEvent(e);
    // A dock drag can end with Qt still holding its mid-drag layout, which then ignores every resize; restoring the
    // state drops it. The status bar is laid out by the same pass, so a stale width shows the resize was skipped.
    QStatusBar* status = findChild<QStatusBar*>(QString(), Qt::FindDirectChildrenOnly);
    if (status && status->isVisible() && status->width() != width() && QGuiApplication::mouseButtons() == Qt::NoButton)
        restoreState(saveState());
    scheduleLayoutSave();
}

void MainWindow::moveEvent(QMoveEvent* e)
{
    QMainWindow::moveEvent(e);
    scheduleLayoutSave();
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (!confirmDiscard())
    {
        e->ignore();
        return;
    }
    saveLayout();
    // Hiding the window resizes its viewports, and a swapchain rebuilt for a closing window crashes some drivers.
    render::Renderer::get().removeAllViewports();
    QMainWindow::closeEvent(e);
}

} // namespace lila::ui
