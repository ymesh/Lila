#include "ui/BakerPanelInternal.h"
#include "ui/UiCommands.h"

namespace lila::ui
{
using namespace bakerPanel;

namespace
{
/** The folder a result wrote to: its manifest's, else its first output's. */
QString resultFolder(const json& result)
{
    std::string file = result.value("manifest", json()).is_string() ? result["manifest"].get<std::string>() : std::string();
    if (file.empty())
        for (const json& o : result.value("outputs", json::array()))
            if (file = o.value("file", std::string()); !file.empty())
                break;
    return file.empty() ? QString() : qs(std::filesystem::path(file).parent_path().generic_string());
}

/** project= for a command: the panel's project when it still exists, else none (the command picks the only one). */
json projectCall(json call = json::object())
{
    const std::string& p = currentProject();
    if (!p.empty())
        if (scene::Node* n = findNode(p); n && bake::isProject(*n))
            call["project"] = p;
    return call;
}
} // namespace

namespace bakerPanel
{
std::string ss(const QString& s) { return s.toStdString(); }

std::string shortName(const std::string& path)
{
    const size_t bar = path.rfind('|');
    return bar == std::string::npos ? path : path.substr(bar + 1);
}

std::string& currentProject()
{
    static std::string p;
    return p;
}

scene::Graph& graph() { return scene::Graph::current(); }

void openFolder(const QString& dir)
{
    if (!dir.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

std::string backendPref()
{
    const std::vector<std::string> names = bake::backendNames();
    const bool gpu = std::ranges::find(names, std::string("gpu")) != names.end();
    std::string b = PrefStore::get().value<std::string>(kPrefs, "/backend", gpu ? "gpu" : "cpu");
    return std::ranges::find(names, b) != names.end() ? b : std::string("cpu");
}

json newProjectArgs()
{
    return {{"target", PrefStore::get().value<std::string>(kPrefs, "/target", "substancePainter")}};
}

std::string targetPref()
{
    if (scene::Node* n = findNode(currentProject()); n && bake::isProject(*n))
        if (std::string t = bake::projectSettings(*n).target; !t.empty())
            return t;
    return PrefStore::get().value<std::string>(kPrefs, "/target", "substancePainter");
}

QString backendBadge(const std::string& name) { return name == "cpu" ? "CPU" : "GPU"; }

std::vector<std::string> groupsForSelection(const std::string& project)
{
    scene::Graph& g = graph();
    std::vector<scene::NodeId> projects;
    if (scene::Node* p = findNode(project); p && bake::isProject(*p))
        projects.push_back(p->id());
    else
        projects = bake::projects(g);
    std::vector<std::string> selected;
    for (scene::NodeId id : g.selection())
        if (scene::Node* n = g.node(id))
            selected.push_back(n->path());
    std::vector<std::string> out;
    for (scene::NodeId p : projects)
        for (scene::NodeId id : bake::groupsOf(g, p))
        {
            scene::Node* grp = g.node(id);
            const bake::GroupSettings s = bake::groupSettings(*grp);
            auto has = [&](const std::string& path) { return std::ranges::find(selected, path) != selected.end(); };
            bool hit = has(grp->path()) || has(s.cage) || std::ranges::any_of(s.lows, has);
            for (const bake::HighEntry& h : s.highs)
                hit = hit || (h.self ? std::ranges::any_of(s.lows, has) : has(h.node));
            if (hit)
                out.push_back(grp->path());
        }
    return out;
}

std::vector<std::string> selectedMeshes()
{
    std::vector<std::string> out;
    scene::Graph& g = graph();
    auto add = [&](scene::NodeId id) {
        if (scene::Node* n = g.node(id); n && n->type().isA("mesh") && std::ranges::find(out, n->path()) == out.end())
            out.push_back(n->path());
    };
    const bool onlySelected = QApplication::keyboardModifiers() & Qt::ShiftModifier;
    for (scene::NodeId id : g.selection())
    {
        add(id);
        if (!onlySelected)
            for (scene::NodeId d : g.descendants(id))
                add(d);
    }
    return out;
}

void groupPerMesh()
{
    const std::vector<std::string> meshes = selectedMeshes();
    if (meshes.empty())
    {
        log::warn("One Group per Mesh: select meshes first");
        return;
    }
    UndoChunk chunk("One Bake Group per Mesh");
    for (const std::string& m : meshes)
    {
        std::string name = bake::fileSafe(shortName(m)) + "_bake";
        if (findNode(name))
            name.clear();
        json call = projectCall({{"lows", {m}}, {"self", true}});
        if (!name.empty())
            call["args"] = {name};
        runUi("bakeGroup", call);
    }
}

void cancelBakes()
{
    for (const progress::Snapshot& s : progress::snapshot())
        if (s.title.starts_with("Baking") || s.title.starts_with("Preparing bake"))
            progress::cancel(s.id);
}

std::vector<LintIssue> lintIssues(const json& r)
{
    const json* list = &r;
    if (r.is_object())
        for (const char* k : {"issues", "warnings", "findings", "problems"})
            if (r.contains(k) && r[k].is_array())
            {
                list = &r[k];
                break;
            }
    std::vector<LintIssue> out;
    if (!list->is_array())
        return out;
    auto str = [](const json& o, std::initializer_list<const char*> keys) {
        for (const char* k : keys)
            if (o.contains(k) && o[k].is_string())
                return o[k].get<std::string>();
        return std::string();
    };
    for (const json& i : *list)
    {
        if (i.is_string())
        {
            out.push_back({.message = i.get<std::string>()});
            continue;
        }
        if (!i.is_object())
            continue;
        LintIssue li{.group = str(i, {"group"}),
                     .mesh = str(i, {"mesh", "node", "low", "high"}),
                     .message = str(i, {"message", "text", "what", "detail"}),
                     .severity = str(i, {"severity", "level"}),
                     .check = str(i, {"check", "code", "kind"}),
                     .data = i.value("data", json::object())};
        if (li.message.empty())
            li.message = li.check.empty() ? i.dump() : li.check;
        out.push_back(li);
    }
    return out;
}
} // namespace bakerPanel

QImage bakeThumbnail(const std::filesystem::path& file, int size)
{
    LILA_ZONE();

    std::string err;
    const image::Image img = image::read(file, &err);
    if (img.width <= 0 || img.height <= 0)
        return {};
    const double scale = std::min(1.0, double(size) / double(std::max(img.width, img.height)));
    const int tw = std::max(1, int(std::lround(img.width * scale)));
    const int th = std::max(1, int(std::lround(img.height * scale)));
    QImage out(tw, th, QImage::Format_RGB32);
    constexpr float kGround = 0.38f;
    for (int y = 0; y < th; ++y)
    {
        const int y0 = y * img.height / th, y1 = std::max(y0 + 1, (y + 1) * img.height / th);
        auto* line = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < tw; ++x)
        {
            const int x0 = x * img.width / tw, x1 = std::max(x0 + 1, (x + 1) * img.width / tw);
            // Box filter with a stride so huge maps don't cost a full pass per thumbnail.
            const int sy = std::max(1, (y1 - y0) / 4), sx = std::max(1, (x1 - x0) / 4);
            float acc[3] = {0.0f, 0.0f, 0.0f};
            int n = 0;
            for (int yy = y0; yy < y1; yy += sy)
                for (int xx = x0; xx < x1; xx += sx)
                {
                    const float* p = &img.rgba[(size_t(yy) * size_t(img.width) + size_t(xx)) * 4];
                    const float a = std::clamp(p[3], 0.0f, 1.0f);
                    for (int c = 0; c < 3; ++c)
                        acc[c] += std::clamp(p[c], 0.0f, 1.0f) * a + kGround * (1.0f - a);
                    ++n;
                }
            auto byte = [&](int c) { return int(std::lround(acc[c] / float(n) * 255.0f)); };
            line[x] = qRgb(byte(0), byte(1), byte(2));
        }
    }
    return out;
}

QImage bakeContactSheet(const json& result, int tile, int columns)
{
    LILA_ZONE();

    const json outs = result.value("outputs", json::array());
    const int n = int(outs.size());
    if (n == 0)
        return {};
    if (columns <= 0)
        columns = std::max(1, int(std::ceil(std::sqrt(double(n)))));
    const int rows = (n + columns - 1) / columns;
    constexpr int kLabel = 18, kGap = 4;
    QImage sheet(columns * (tile + kGap) + kGap, rows * (tile + kLabel + kGap) + kGap, QImage::Format_RGB32);
    sheet.fill(QColor(40, 40, 40));
    QPainter p(&sheet);
    QFont f = p.font();
    f.setPixelSize(11);
    p.setFont(f);
    for (int i = 0; i < n; ++i)
    {
        const json& o = outs[i];
        const int cx = kGap + (i % columns) * (tile + kGap), cy = kGap + (i / columns) * (tile + kLabel + kGap);
        const QImage t = bakeThumbnail(std::filesystem::path(o.value("file", std::string())), tile);
        if (!t.isNull())
            p.drawImage(cx + (tile - t.width()) / 2, cy + (tile - t.height()) / 2, t);
        else
        {
            p.setPen(QColor(150, 150, 150));
            p.drawText(QRect(cx, cy, tile, tile), Qt::AlignCenter, "unreadable");
        }
        p.setPen(QColor(210, 210, 210));
        const QString label = QString("%1  %2").arg(qs(o.value("map", std::string()))).arg(o.value("udim", 1001));
        p.drawText(QRect(cx, cy + tile, tile, kLabel), Qt::AlignCenter, p.fontMetrics().elidedText(label, Qt::ElideRight, tile));
    }
    return sheet;
}

BakerPanel::BakerPanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->setSpacing(2);
    v->addWidget(buildToolbar());

    auto* split = new QSplitter(Qt::Vertical, this);
    split->setChildrenCollapsible(false);
    split->addWidget(buildGroups());
    mTabs = new QTabWidget(split);
    mTabs->setDocumentMode(true);
    mTabs->addTab(buildGroupDetails(), "Group");
    mTabs->addTab(buildMaps(), "Maps");
    mTabs->addTab(buildOutput(), "Output");
    mTabs->addTab(buildResults(), "Results");
    split->addWidget(mTabs);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    v->addWidget(split, 1);

    mPoll = new QTimer(this);
    mPoll->setInterval(150);
    connect(mPoll, &QTimer::timeout, this, &BakerPanel::pollProgress);
    mPoll->start();

    mLintTimer = new QTimer(this);
    mLintTimer->setSingleShot(true);
    mLintTimer->setInterval(500);
    connect(mLintTimer, &QTimer::timeout, this, [this] { runLint(false); });

    QPointer<BakerPanel> self(this);
    mGraphListener = graph().addListener([self](scene::Change c, scene::NodeId id) {
        if (!self)
            return;
        if (c == scene::Change::Selection)
        {
            self->updateButtons();
            return;
        }
        if (c == scene::Change::Value)
        {
            const scene::Node* n = graph().node(id);
            if (!n)
                return;
            if (!(bake::isProject(*n) || bake::isGroup(*n)))
            {
                if (self->watchedByLint(id))
                    self->scheduleLint();
                return;
            }
        }
        self->scheduleRefresh();
        self->scheduleLint();
    });
    mBakeListener = bake::addBakeListener([self](const bake::BakeEvent& e) {
        if (self)
            self->onBakeEvent(e);
    });
    mArrangeListener = bake::arrange::addListener([self] {
        if (self)
            self->updateButtons();
    });
    mExecListener = ops::addExecuteListener([self](const ExecuteEvent& e) {
        if (!self || !e.done)
            return;
        if (e.name == "bake" || e.name == "bakeForTarget" || e.name == "newScene" || e.name == "openScene")
        {
            self->mLintedProject.clear();
            self->scheduleRefresh();
        }
    });
    refresh();
}

BakerPanel::~BakerPanel()
{
    graph().removeListener(mGraphListener);
    ops::removeExecuteListener(mExecListener);
    bake::removeBakeListener(mBakeListener);
    bake::arrange::removeListener(mArrangeListener);
}

void BakerPanel::onBakeEvent(const bake::BakeEvent& e)
{
    scene::Node* p = findNode(mProject);
    if (!p || p->id() != e.project)
        return;
    switch (e.kind)
    {
    case bake::BakeEvent::Kind::Started:
        mRunningBackend = e.data.value("backend", std::string());
        ++mThumbGeneration;
        mThumbs->clear();
        mThumbJobs = 0;
        showStatus("Baking");
        updateButtons();
        break;
    case bake::BakeEvent::Kind::Texture: addThumbnail(e.data, mThumbGeneration); break;
    case bake::BakeEvent::Kind::Finished:
        mRunningBackend.clear();
        if (e.data.contains("error"))
        {
            if (e.data.value("cancelled", false))
                showStatus("Cancelled");
            else
                showStatus("Failed", qs(e.data.value("error", std::string())), true);
        }
        else
            showResult(e.data);
        updateButtons();
        break;
    }
}

void BakerPanel::addThumbnail(const json& o, uint64_t generation)
{
    const std::string file = o.value("file", std::string());
    for (int i = 0; i < mThumbs->count(); ++i)
        if (mThumbs->item(i)->data(kFileRole).toString() == qs(file))
            return;
    auto* item = new QListWidgetItem(icon("photo"), QString("%1\n%2").arg(qs(o.value("map", std::string()))).arg(qs(std::filesystem::path(file).filename().string())),
                                     mThumbs);
    item->setData(kFileRole, qs(file));
    item->setData(kOutputRole, qs(o.dump()));
    item->setToolTip(QString("%1\n%2 x %3, %4 bit %5, UDIM %6")
                         .arg(qs(file))
                         .arg(o.value("width", 0))
                         .arg(o.value("height", 0))
                         .arg(o.value("bits", 8))
                         .arg(qs(o.value("format", std::string())))
                         .arg(o.value("udim", 1001)));
    ++mThumbJobs;
    QPointer<BakerPanel> self(this);
    const QString key = qs(file);
    QThreadPool::globalInstance()->start([self, file, key, generation] {
        QImage img = bakeThumbnail(std::filesystem::path(file), 128);
        QMetaObject::invokeMethod(qApp, [self, img = std::move(img), key, generation] {
            if (!self || self->mThumbGeneration != generation)
                return;
            --self->mThumbJobs;
            for (int i = 0; i < self->mThumbs->count(); ++i)
                if (QListWidgetItem* it = self->mThumbs->item(i); it->data(kFileRole).toString() == key && !img.isNull())
                    it->setIcon(QIcon(QPixmap::fromImage(img)));
        }, Qt::QueuedConnection);
    });
}

void BakerPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refresh();
    scheduleLint();
}

void BakerPanel::scheduleLint()
{
    if (ops::find("bakeLint") && !mProject.empty() && isVisible())
        mLintTimer->start();
}

bool BakerPanel::watchedByLint(scene::NodeId id) const
{
    const scene::Graph& g = graph();
    for (; id != scene::kNoNode; id = g.parent(id))
        if (const scene::Node* n = g.node(id); n && std::ranges::find(mLintWatch, n->path()) != mLintWatch.end())
            return true;
    return false;
}

void BakerPanel::updateLintWatch()
{
    mLintWatch.clear();
    scene::Graph& g = graph();
    const scene::Node* p = findNode(mProject);
    if (!p || !bake::isProject(*p))
        return;
    for (scene::NodeId gid : bake::groupsOf(g, p->id()))
    {
        const bake::GroupSettings s = bake::groupSettings(*g.node(gid));
        mLintWatch.insert(mLintWatch.end(), s.lows.begin(), s.lows.end());
        if (!s.cage.empty())
            mLintWatch.push_back(s.cage);
        for (const bake::HighEntry& h : s.highs)
            if (!h.node.empty())
                mLintWatch.push_back(h.node);
    }
}

void BakerPanel::runLint(bool full)
{
    if (!ops::find("bakeLint") || mProject.empty() || !findNode(mProject))
        return;
    json r;
    try
    {
        // The automatic pass skips the highs: loading and tracing them is a bake's worth of work.
        r = ops::execute("bakeLint", {{"project", mProject}, {"highs", full}}, CallSource::Script);
    }
    catch (const std::exception& e)
    {
        log::warn("bakeLint: {}", e.what());
        return;
    }
    mLintedProject = mProject;
    if (r == mLint)
        return;
    mLint = r;
    const bool was = std::exchange(mUpdating, true);
    refreshGroups();
    refreshDetails();
    mUpdating = was;
}

void BakerPanel::pollProgress()
{
    if (!isVisible())
        return;
    double fraction = -1.0;
    bool cancellable = false;
    QString status;
    for (const progress::Snapshot& s : progress::snapshot())
        if (s.title.starts_with("Baking") || s.title.starts_with("Preparing bake"))
        {
            fraction = s.determinate ? s.fraction : 0.0;
            cancellable = s.cancellable && !s.cancelled;
            status = qs(s.title + (s.status.empty() ? "" : ": " + s.status));
        }
    mCancel->setEnabled(cancellable);
    mAssign->setEnabled(fraction < 0.0);
    if (fraction < 0.0)
    {
        mProgress->setValue(0);
        mProgress->setToolTip("No bake running");
        return;
    }
    mProgress->setValue(int(fraction * 1000.0));
    mProgress->setToolTip(status);
}

json BakerPanel::run(const char* command, json call, bool withProject)
{
    if (withProject && !mProject.empty() && !call.contains("project"))
        call["project"] = mProject;
    return runUi(command, call);
}

void BakerPanel::bake(const std::vector<std::string>& groups)
{
    json call = {{"backend", backendPref()}, {"async", true}};
    if (!groups.empty())
        call["groups"] = groups;
    if (run("bake", call).is_object())
        mTabs->setCurrentIndex(mTabs->count() - 1);
}

void BakerPanel::savePreset()
{
    const QString name = mPresetName->text().trimmed();
    if (name.isEmpty())
    {
        log::warn("Baker: name the preset");
        return;
    }
    const json r = run("saveBakeTarget", {{"args", {ss(name)}},
                                          {"scope", ss(mPresetScope->currentData().toString())},
                                          {"overwrite", mPresetReplace->isChecked()},
                                          {"backend", backendPref()}});
    if (!r.is_object())
        return;
    mPresetRow->hide();
    mPresetName->clear();
    mTarget = r.value("name", ss(name));
    PrefStore::get().set(kPrefs, "/target", mTarget);
    const bool was = std::exchange(mUpdating, true);
    refreshTargets();
    {
        QSignalBlocker b(mTargets);
        mTargets->setCurrentIndex(std::max(0, mTargets->findData(qs(mTarget))));
    }
    mUpdating = was;
    updateButtons();
}

void BakerPanel::bakeForTarget(bool dryRun)
{
    if (mTarget.empty())
        return;
    json call = {{"target", mTarget}, {"optional", PrefStore::get().value<bool>(kPrefs, "/optional", false)}, {"backend", backendPref()}};
    if (dryRun)
        call["dryRun"] = true;
    else
        call["async"] = true;
    json r = run("bakeForTarget", call, !mProject.empty());
    if (!r.is_object())
        return;
    if (mProject.empty())
        refreshProjects();
    if (!dryRun)
        mTabs->setCurrentIndex(mTabs->count() - 1);
}

void BakerPanel::showResult(const json& result)
{
    mShownResult = result.dump();
    mResultFolder = resultFolder(result);
    const json stats = result.value("stats", json::object());
    const double samples = stats.value("samples", 0.0);
    const bool dryRun = result.value("dryRun", false);
    const QColor dim = palette().color(QPalette::Disabled, QPalette::Text);
    mSummary->clear();
    auto add = [&](QTreeWidgetItem* parent, const QString& name, const QString& value, const char* iconName = nullptr, const QString& tip = {}) {
        auto* it = parent ? new QTreeWidgetItem(parent, {name, value}) : new QTreeWidgetItem(mSummary, {name, value});
        if (iconName)
            it->setIcon(0, icon(iconName));
        it->setForeground(1, dim);
        if (!tip.isEmpty())
        {
            it->setToolTip(0, tip);
            it->setToolTip(1, tip);
        }
        return it;
    };
    add(nullptr, dryRun ? "Planned" : "Files", QString::number(result.value("outputs", json::array()).size()), "photo");
    if (!dryRun)
    {
        add(nullptr, "Time", QString("%1 s").arg(result.value("seconds", 0.0), 0, 'f', 2), "clock");
        const double missed = samples > 0.0 ? 100.0 * stats.value("misses", 0.0) / samples : 0.0;
        add(nullptr, "Missed", QString("%1%").arg(missed, 0, 'f', 2), missed > 1.0 ? "alert-triangle" : "target",
            QString("%1 of %2 rays; %3 hit backfaces").arg(stats.value("misses", 0.0), 0, 'f', 0).arg(samples, 0, 'f', 0).arg(stats.value("backfaces", 0)));
    }
    if (!mResultFolder.isEmpty())
    {
        QTreeWidgetItem* f = add(nullptr, "Folder", mResultFolder, "folder", "Double-click to open");
        f->setData(0, kFileRole, mResultFolder);
        if (const json m = result.value("manifest", json()); m.is_string())
            f->setToolTip(1, "Manifest: " + qs(m.get<std::string>()));
    }
    if (scene::Node* p = findNode(mProject); !dryRun && p && bake::isProject(*p) && bake::projectSettings(*p).autoAssign)
        if (const json a = bake::autoAssigned(p->id()); a.is_object())
        {
            if (a.contains("error"))
                add(nullptr, "Assigned", "failed", "alert-triangle", qs(a.value("error", std::string())));
            else
                add(nullptr, "Assigned", QString("%1 maps").arg(a.value("assigned", json::array()).size()), "link");
        }
    QTreeWidgetItem* misses = nullptr;
    for (const json& g : stats.value("groups", json::array()))
        if (const double s = g.value("samples", 0.0); s > 0.0 && g.value("misses", 0.0) / s > 0.01)
        {
            if (!misses)
                misses = add(nullptr, "Rays missing the high", QString(), "alert-triangle", "Raise Frontal or Rear, or Auto Paint the cage");
            add(misses, qs(g.value("group", std::string())), QString("%1%").arg(100.0 * g.value("misses", 0.0) / s, 0, 'f', 1), "box");
        }
    if (misses)
        misses->setExpanded(true);
    int lint = 0;
    QTreeWidgetItem* other = nullptr;
    for (const json& w : result.value("warnings", json::array()))
    {
        if (!w.is_string())
            continue;
        const QString text = qs(w.get<std::string>());
        if (text.startsWith("lint "))
        {
            ++lint;
            continue;
        }
        if (!other)
            other = add(nullptr, "Warnings", QString(), "alert-triangle");
        const int colon = text.indexOf(": ");
        add(other, colon > 0 ? text.left(colon) : text, colon > 0 ? text.mid(colon + 2) : QString(), nullptr, text);
    }
    if (other)
    {
        other->setText(1, QString::number(other->childCount()));
        other->setExpanded(true);
    }
    if (lint)
        add(nullptr, "Issues", QString::number(lint), "alert-triangle", "Listed under each group in the groups tree");
    mSummaryBox->show();
    if (result.contains("lint") && result["lint"].is_object() && result["lint"] != mLint)
    {
        mLint = result["lint"];
        const bool was = std::exchange(mUpdating, true);
        refreshGroups();
        mUpdating = was;
    }

    // Thumbnails that filled in while the bake ran stay; files of an earlier result go.
    std::set<std::string> files;
    for (const json& o : result.value("outputs", json::array()))
        files.insert(o.value("file", std::string()));
    for (int i = mThumbs->count() - 1; i >= 0; --i)
        if (!files.contains(ss(mThumbs->item(i)->data(kFileRole).toString())))
            delete mThumbs->takeItem(i);
    for (const json& o : result.value("outputs", json::array()))
    {
        if (result.value("dryRun", false))
        {
            auto* item = new QListWidgetItem(icon("photo"), QString("%1\n%2")
                                                                .arg(qs(o.value("map", std::string())))
                                                                .arg(qs(std::filesystem::path(o.value("file", std::string())).filename().string())),
                                             mThumbs);
            item->setData(kFileRole, qs(o.value("file", std::string())));
            continue;
        }
        addThumbnail(o, mThumbGeneration);
    }
}

void BakerPanel::showStatus(const QString& text, const QString& detail, bool error)
{
    mSummary->clear();
    auto* it = new QTreeWidgetItem(mSummary, {text, detail});
    it->setIcon(0, icon(error ? "alert-triangle" : "loader"));
    it->setToolTip(1, detail);
    mSummaryBox->show();
}

void BakerPanel::assignResult()
{
    if (mProject.empty())
        return;
    json r;
    try
    {
        r = ops::execute("bakeAssign", {{"project", mProject}}, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        viewportToast::showActive(QString::fromStdString(e.what()), 4000, viewportToast::Kind::Error);
        return;
    }
    // One toast per material: which channels it took.
    std::map<std::string, QStringList> byMaterial;
    for (const json& a : r.value("assigned", json::array()))
        byMaterial[a.value("material", std::string())] << qs(a.value("channel", std::string()));
    for (const auto& [material, channels] : byMaterial)
        viewportToast::showActive(QString("%1: %2").arg(qs(material), channels.join(", ")), 3500);
    const size_t skipped = r.value("skipped", json::array()).size();
    if (byMaterial.empty())
        viewportToast::showActive("Nothing assigned: no map has a material input", 4000, viewportToast::Kind::Error);
    else if (skipped)
        viewportToast::showActive(QString("%1 maps have no material input").arg(skipped), 3500);
}

QString BakerPanel::outputFolder() const
{
    if (!mResultFolder.isEmpty())
        return mResultFolder;
    scene::Node* p = findNode(mProject);
    return p && bake::isProject(*p) ? qs(bake::resolveFolder(bake::projectSettings(*p).output.folder).generic_string()) : QString();
}

QMenu* BakerPanel::thumbnailMenu(QWidget* parent)
{
    QStringList files;
    for (QListWidgetItem* it : mThumbs->selectedItems())
        files << it->data(kFileRole).toString();
    if (files.isEmpty())
        if (QListWidgetItem* it = mThumbs->currentItem())
            files << it->data(kFileRole).toString();
    auto* m = new QMenu(parent);
    m->setObjectName("bakerThumbMenu");
    const bool any = !files.isEmpty();
    m->addAction(icon("external-link"), files.size() > 1 ? "Open All" : "Open", m, [files] {
          for (const QString& f : files)
              QDesktopServices::openUrl(QUrl::fromLocalFile(f));
      })->setEnabled(any);
    m->addAction(icon("folder-search"), "Open Containing Folder", m, [files] { fileOps::reveal(files.front()); })->setEnabled(any);
    m->addSeparator();
    m->addAction(icon("copy"), files.size() > 1 ? "Copy Paths" : "Copy Path", m, [files] {
          QStringList native;
          for (const QString& f : files)
              native << QDir::toNativeSeparators(f);
          QApplication::clipboard()->setText(native.join('\n'));
      })->setEnabled(any);
    m->addAction(icon("file-text"), files.size() > 1 ? "Copy File Names" : "Copy File Name", m, [files] {
          QStringList names;
          for (const QString& f : files)
              names << QFileInfo(f).fileName();
          QApplication::clipboard()->setText(names.join('\n'));
      })->setEnabled(any);
    m->addSeparator();
    m->addAction(icon("folder"), "Open Output Folder", m, [this] { openFolder(outputFolder()); })->setEnabled(!outputFolder().isEmpty());
    return m;
}

void BakerPanel::dropNodes(const QStringList& names, QTreeWidgetItem* target)
{
    // A dropped mesh joins alone; a dropped group brings every mesh under it.
    std::vector<std::string> meshes;
    auto add = [&](const scene::Node& n) {
        if (std::ranges::find(meshes, n.path()) == meshes.end())
            meshes.push_back(n.path());
    };
    scene::Graph& g = graph();
    for (const QString& n : names)
    {
        scene::Node* node = findNode(ss(n));
        if (!node)
            continue;
        if (node->type().isA("mesh"))
            add(*node);
        else
            for (scene::NodeId id : g.descendants(node->id()))
                if (scene::Node* d = g.node(id); d && d->type().isA("mesh"))
                    add(*d);
    }
    if (meshes.empty())
    {
        log::warn("Baker: no meshes to add to the bake group");
        return;
    }
    if (!target || target->data(0, kGroupRole).toString().isEmpty())
    {
        if (mProject.empty())
            if (json r = run("createBakeProject", newProjectArgs(), false); r.is_string())
                setProject(r.get<std::string>());
        run("bakeGroup", {{"lows", meshes}});
        return;
    }
    const std::string grp = ss(target->data(0, kGroupRole).toString());
    switch (target->data(0, kKindRole).toInt())
    {
    case kHighsItem:
    case kHighItem: run("bakeGroup", {{"args", {grp}}, {"addHigh", meshes}}, false); break;
    case kCageItem: run("bakeGroup", {{"args", {grp}}, {"cage", meshes.front()}}, false); break;
    default: run("bakeGroup", {{"args", {grp}}, {"addLow", meshes}}, false); break;
    }
}

void BakerPanel::groupMenu(const QPoint& at)
{
    QTreeWidgetItem* it = mGroups->itemAt(at);
    QMenu menu(this);
    if (!it)
    {
        menu.addAction(icon("plus"), "New Group from Selection", this, [this] { run("bakeGroup", {{"lows", selectedMeshes()}}); });
        menu.addAction(icon("wand"), "Auto Group by Name", this, [this] { run("autoGroupBake", json::object()); });
        menu.exec(mGroups->viewport()->mapToGlobal(at));
        return;
    }
    const std::string g = ss(it->data(0, kGroupRole).toString());
    const std::string node = ss(it->data(0, kNodeRole).toString());
    const int kind = it->data(0, kKindRole).toInt();
    if (!g.empty())
    {
        menu.addAction(icon("player-play"), "Bake This Group", this, [this, g] { bake({g}); });
        menu.addSeparator();
        menu.addAction("Add Selected as Low", this, [this, g] { run("bakeGroup", {{"args", {g}}, {"addLow", selectedMeshes()}}, false); });
        menu.addAction("Add Selected as High", this, [this, g] { run("bakeGroup", {{"args", {g}}, {"addHigh", selectedMeshes()}}, false); });
        menu.addAction("Use Selected as Cage", this, [this, g] {
            if (const std::vector<std::string> m = selectedMeshes(); !m.empty())
                run("bakeGroup", {{"args", {g}}, {"cage", m.front()}}, false);
        });
    }
    if (!node.empty())
        menu.addAction(icon("pointer"), "Select", this, [node] { runUi("select", {{"args", {node}}}); });
    if (kind == kGroupItem)
    {
        menu.addAction(icon("pointer"), "Select Lows and Highs", this, [g] {
            scene::Node* n = findNode(g);
            if (!n)
                return;
            const bake::GroupSettings s = bake::groupSettings(*n);
            std::vector<std::string> all = s.lows;
            for (const bake::HighEntry& h : s.highs)
                if (!h.self)
                    all.push_back(h.node);
            runUi("select", {{"args", all}});
        });
        menu.addAction(icon("pencil"), "Rename...", this, [this, g] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, "Rename Bake Group", "Name", QLineEdit::Normal, qs(shortName(g)), &ok).trimmed();
            if (ok && !name.isEmpty())
                runUi("rename", {{"args", {g, ss(name)}}});
        });
        menu.addAction(icon("trash"), "Delete Group", this, [g] { runUi("delete", {{"args", {g}}}); });
    }
    if (kind == kCageItem && !node.empty())
        menu.addAction("Use Auto Cage", this, [this, g] { run("bakeGroup", {{"args", {g}}, {"cage", ""}}, false); });
    if (kind == kLowItem && !node.empty())
    {
        menu.addSeparator();
        menu.addAction(icon("link-off"), "Selected Don't Occlude This Low", this, [this, g, node] { unlinkSelected(g, node, false); });
        menu.addAction("Selected Don't Occlude or Project onto This Low", this, [this, g, node] { unlinkSelected(g, node, true); });
    }
    if (kind == kLowItem || kind == kHighItem)
    {
        menu.addSeparator();
        menu.addAction(icon("trash"), "Remove from Group", this, [this, it] {
            if (!it->isSelected())
                mGroups->setCurrentItem(it, 0, QItemSelectionModel::ClearAndSelect);
            removeSelectedRows();
        });
    }
    if (kind == kLintItem)
    {
        const QString text = it->toolTip(0).isEmpty() ? it->text(0) : it->toolTip(0);
        menu.addAction(icon("copy"), "Copy Message", this, [text] { QApplication::clipboard()->setText(text); });
    }
    menu.exec(mGroups->viewport()->mapToGlobal(at));
}

namespace
{
/**
 * Textures dropped on a mesh in a view (files, or Results thumbnails) go into the material on the face under the cursor,
 * each in the channel its map or file name says (assignTexture).
 */
bool dropTexture(const QMimeData& mime, const viewportHooks::DropTarget& t, bool commit)
{
    json items = json::array();
    if (mime.hasFormat(kOutputsMime))
        items = json::parse(mime.data(kOutputsMime).toStdString(), nullptr, false);
    else if (mime.hasUrls())
    {
        static const QStringList kImages = {"png", "exr", "tga", "tif", "tiff", "psd", "jpg", "jpeg", "bmp", "hdr", "dds"};
        for (const QUrl& u : mime.urls())
            if (u.isLocalFile() && kImages.contains(QFileInfo(u.toLocalFile()).suffix().toLower()))
                items.push_back({{"file", ss(u.toLocalFile())}});
    }
    if (!items.is_array() || items.empty())
        return false;
    scene::Node* n = t.node != scene::kNoNode ? graph().node(t.node) : nullptr;
    if (!n || !n->type().isA("mesh"))
        return false;
    if (!commit)
        return true;
    UndoChunk chunk("Assign Textures");
    for (const json& it : items)
    {
        json call = {{"args", {it.value("file", std::string())}}, {"mesh", n->path()}, {"face", t.face == UINT32_MAX ? -1 : int64_t(t.face)}};
        if (const std::string map = it.value("map", std::string()); !map.empty())
            call["map"] = map;
        if (it.contains("normalFormat"))
            call["normalFormat"] = it["normalFormat"];
        const QString file = QFileInfo(qs(it.value("file", std::string()))).fileName();
        try
        {
            const json r = ops::execute("assignTexture", call, CallSource::UI);
            for (const json& a : r.value("assigned", json::array()))
                viewportToast::showActive(QString("%1 → %2 %3").arg(file, qs(a.value("material", std::string())), qs(a.value("channel", std::string()))), 3000);
            for (const json& s : r.value("skipped", json::array()))
                viewportToast::showActive(QString("%1 not assigned: %2").arg(file, qs(s.value("reason", std::string()))), 4000, viewportToast::Kind::Error);
        }
        catch (const CommandError& e)
        {
            log::warn("{}", e.what());
            viewportToast::showActive(QString("%1 not assigned: no channel in its name").arg(file), 4000, viewportToast::Kind::Error);
        }
    }
    return true;
}
} // namespace

void registerBakerPanel()
{
    static bool done = false;
    if (std::exchange(done, true))
        return;
    panels::add({.id = kBakerDock,
                 .title = "Baker",
                 .create = [](QWidget* p) -> QWidget* { return new BakerPanel(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "texture",
                 .open = false,
                 .toggleAction = "window.baker"});
    viewportHooks::setDropHandler("bakeTexture", dropTexture);

    actions::add({.id = "bake.forTarget",
                  .label = "Bake for Target",
                  .category = "Bake",
                  .run =
                      [] {
                          json call = projectCall({{"target", targetPref()},
                                                   {"optional", PrefStore::get().value<bool>(kPrefs, "/optional", false)},
                                                   {"backend", backendPref()},
                                                   {"async", true}});
                          runUi("bakeForTarget", call);
                      },
                  .doc = "One click: apply the Baker's target profile (Smart Material Inputs, Unreal, ...) to the project, auto-group "
                         "_low/_high meshes when it has no groups, and bake every map the target needs with the names it expects.",
                  .python = "lila.bake.bake_for_target(\"substancePainter\")"});
    actions::add({.id = "bake.bake",
                  .label = "Bake",
                  .category = "Bake",
                  .run = [] { runUi("bake", projectCall({{"backend", backendPref()}, {"async", true}})); },
                  .doc = "Bake every group of the bake project and write its maps.",
                  .python = "lila.bake.bake()"});
    actions::add({.id = "bake.bakeSelected",
                  .label = "Bake Selected",
                  .category = "Bake",
                  .run =
                      [] {
                          const std::vector<std::string> groups = groupsForSelection(currentProject());
                          if (groups.empty())
                          {
                              log::warn("Bake Selected: no bake group holds the selection");
                              return;
                          }
                          runUi("bake", projectCall({{"groups", groups}, {"backend", backendPref()}, {"async", true}}));
                      },
                  .enabled = [] { return actions::hasSelection(); },
                  .doc = "Bake only the groups holding the selected meshes (as lows, highs or cage).",
                  .python = "lila.bake.bake(groups=[\"crate_bake\"])"});
    actions::add({.id = "bake.cancel",
                  .label = "Cancel Bake",
                  .category = "Bake",
                  .run = [] { cancelBakes(); },
                  .doc = "Stop the running bake; nothing is written for the maps not finished.",
                  .python = "lila.ui.run_action(\"bake.cancel\")"});
    actions::add({.id = "bake.removeRows",
                  .label = "Remove from Bake Group",
                  .category = "Bake",
                  .run =
                      [] {
                          for (QWidget* w = QApplication::focusWidget(); w; w = w->parentWidget())
                              if (auto* panel = qobject_cast<BakerPanel*>(w))
                              {
                                  panel->removeSelectedRows();
                                  return;
                              }
                      },
                  .doc = "In the Baker's groups tree: take the selected Low and High meshes out of their group and delete selected "
                         "groups. The meshes themselves stay in the scene.",
                  .python = "bakeGroup(\"crate_bake\", removeHigh=[\"crate_high\"])"});
    actions::add({.id = "bake.newProject",
                  .label = "New Bake Project",
                  .category = "Bake",
                  .run =
                      [] {
                          if (json r = runUi("createBakeProject", newProjectArgs()); r.is_string())
                              currentProject() = r.get<std::string>();
                          panels::show(kBakerDock);
                      },
                  .doc = "Create a bake project (output folder, naming, resolution, maps) and show it in the Baker.",
                  .python = "createBakeProject()"});
    actions::add({.id = "bake.autoGroup",
                  .label = "Auto Group by Name",
                  .category = "Bake",
                  .run = [] { runUi("autoGroupBake", projectCall()); },
                  .doc = "Make bake groups by name: _low/_high, _lo/_hi and _LP/_HP pairs from the selection, else every mesh.",
                  .python = "autoGroupBake()"});
    actions::add({.id = "bake.groupPerMesh",
                  .label = "One Group per Selected Mesh",
                  .category = "Bake",
                  .run = [] { groupPerMesh(); },
                  .enabled = [] { return actions::hasSelection(); },
                  .doc = "Give each selected mesh its own bake group baking onto itself, so overlapping meshes never affect each other.",
                  .python = "bakeGroup(\"rock_bake\", lows=[\"rock\"], self=True)"});
    actions::add({.id = "bake.lint",
                  .label = "Check Bake Setup",
                  .category = "Bake",
                  .run =
                      [] {
                          if (!ops::find("bakeLint"))
                              return;
                          json r = runUi("bakeLint", projectCall());
                          const std::vector<LintIssue> issues = lintIssues(r);
                          if (issues.empty())
                              log::info("Bake setup: no problems found");
                          for (const LintIssue& i : issues)
                              log::warn("Bake setup: {}", i.message);
                          panels::show(kBakerDock);
                      },
                  .enabled = [] { return ops::find("bakeLint") != nullptr; },
                  .doc = "Look for what spoils bakes: hard edges off UV seams, overlapping or missing UVs, texel density outliers, "
                         "lows with no high in range, cage mismatches.",
                  .python = "bakeLint()"});

    menus::addMenu({.id = "Bake", .title = "Bake", .before = "Rig", .sets = {"Texture"}});
    menus::addActions("Bake", {"window.baker", "", "bake.forTarget", "bake.bake", "bake.bakeSelected", "bake.cancel", "", "bake.newProject",
                               "bake.autoGroup", "bake.groupPerMesh", "", "bake.lint"});
}
} // namespace lila::ui
