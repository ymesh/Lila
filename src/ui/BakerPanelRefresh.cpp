#include "ui/BakerPanelInternal.h"
#include "ui/ChoiceTips.h"
#include "ui/UiCommands.h"

namespace lila::ui
{
using namespace bakerPanel;

namespace
{
namespace opt = ops::options;

/** A lint check as the groups tree shows it: a short title, what to do about it, and one issue's number in a few words. */
struct LintCheckView
{
    QString title;
    QString fix;
};

LintCheckView lintCheckView(const std::string& check)
{
    static const std::map<std::string, LintCheckView, std::less<>> views = {
        {"hardEdges", {"Hard edges off UV seams", "The normal map can't hide their shading split: split the UVs there or soften the edges."}},
        {"uvMissing", {"Missing UVs", "Texels without UV area get nothing: unwrap the mesh or fix the zero-area triangles."}},
        {"mirroredTangents", {"Mirrored or folded UVs", "Mirrored UVs decode inverted when the tangent sign disagrees; folded islands bake twice."}},
        {"uvOverlap", {"Overlapping UVs", "Overlapping islands writing one texture overwrite each other: offset or separate them."}},
        {"texelDensity", {"Texel density outliers", "Islands far from the project's median density look sharper or blurrier than the rest."}},
        {"highRange", {"Rays missing the high", "Move the high onto the low or raise the Frontal and Rear distances (or Auto Paint the cage)."}},
        {"cageContainment", {"High outside the cage", "Rays start inside the high there: raise Frontal or Rear, or Auto Paint the cage."}},
        {"triangulation", {"Non-planar faces", "They bake on Lila's triangle split: export triangulated so the game splits them the same way."}},
        {"cageTopology", {"Cage doesn't match", "The editable cage isn't used until it matches its low; the auto cage bakes instead."}},
    };
    if (auto it = views.find(check); it != views.end())
        return it->second;
    return {qs(check), QString()};
}

/** What clicking an issue row selects, as kComponentsRole holds it; empty when the check names no components. */
QString lintComponents(const LintIssue& li)
{
    json c = json::object();
    if (li.data.contains("edgePoints") && !li.data["edgePoints"].empty())
        c["edgePoints"] = li.data["edgePoints"];
    for (const char* k : {"faceIds", "faces"})
        if (li.data.contains(k) && li.data[k].is_array() && !li.data[k].empty())
        {
            c["faces"] = li.data[k];
            break;
        }
    return c.empty() ? QString() : qs(c.dump());
}

QString lintMetric(const LintIssue& li)
{
    const json& d = li.data;
    auto num = [&](const char* k) { return d.contains(k) && d[k].is_number() ? d[k].get<double>() : -1.0; };
    auto n = [&](const char* k, const char* unit) { return QString("%1 %2").arg(qlonglong(num(k))).arg(unit); };
    if (li.check == "hardEdges" && num("count") >= 0)
        return n("count", "edges");
    if (li.check == "uvMissing")
        return num("count") >= 0 ? n("count", "zero-area tris") : QString("no uv0");
    if (li.check == "mirroredTangents")
        return num("islands") >= 0 ? n("islands", "folded islands") : num("splitFaces") >= 0 ? n("splitFaces", "split faces") : n("count", "tris");
    if (li.check == "uvOverlap" && d.contains("other") && d["other"].is_string())
        return "with " + qs(shortName(d["other"].get<std::string>()));
    if (li.check == "texelDensity" && num("median") > 0)
        return QString("%1 islands, %2 vs %3 median").arg(qlonglong(num("islands"))).arg(num("max"), 0, 'g', 4).arg(num("median"), 0, 'g', 4);
    if (li.check == "highRange" && num("rays") > 0)
        return QString("%1% missed").arg(100.0 * num("missed") / num("rays"), 0, 'f', 1);
    if (li.check == "cageContainment" && num("sampled") > 0)
        return QString("%1 cm beyond").arg(num("maxBeyond"), 0, 'f', 2);
    if (li.check == "triangulation" && num("faces") >= 0)
        return QString("%1 faces, up to %2\u00b0").arg(qlonglong(num("faces"))).arg(num("maxFold"), 0, 'f', 0);
    return QString();
}

/** A bake's backendMode as the badge shows it: RT (ray query), GPU (compute traversal) or CPU. */
QString modeBadge(const std::string& mode) { return mode == "rt" ? "RT" : mode == "compute" ? "GPU" : "CPU"; }

QString modifierSummary(const json& stack)
{
    QStringList parts;
    for (const json& m : stack.is_array() ? stack : json::array())
    {
        if (m.is_string())
        {
            parts << qs(m.get<std::string>());
            continue;
        }
        QString text = qs(m.value("kind", std::string("?")));
        const json params = m.value("params", json::object());
        if (params.contains("levels"))
            text += " " + qs(params["levels"].dump());
        parts << text;
    }
    return parts.join(" > ");
}

std::string entryKind(const json& m)
{
    return m.is_string() ? m.get<std::string>() : m.is_object() ? m.value("kind", std::string()) : std::string();
}

/** A bake stack entry's node type: kind "subdivision" is subdivisionModifier, as BakeModifierStack builds it. */
const scene::NodeType* modifierType(const std::string& kind)
{
    return kind.empty() ? nullptr : scene::types::find(kind + "Modifier");
}

/** The plugs a stack entry's params can set: visible inputs, minus the switches a bake stack has no use for. */
std::vector<const scene::AttrDef*> editableParams(const scene::NodeType& t)
{
    std::vector<const scene::AttrDef*> out;
    for (const scene::AttrDef& d : t.attrs)
    {
        if ((d.flags & scene::AttrHidden) || !(d.flags & scene::AttrInput))
            continue;
        if (d.type == scene::PlugType::Mesh || d.type == scene::PlugType::Data || d.type == scene::PlugType::Matrix)
            continue;
        if (d.name == "enabled" || d.name == "exportEnabled")
            continue;
        out.push_back(&d);
    }
    return out;
}

/** An entry as {kind, params} with params in their plug's type and defaults left out, so equal stacks compare equal. */
json canonicalEntry(const json& m)
{
    const std::string kind = entryKind(m);
    const scene::NodeType* t = modifierType(kind);
    json params = json::object();
    const json given = m.is_object() ? m.value("params", json::object()) : json::object();
    for (auto it = given.begin(); it != given.end(); ++it)
    {
        const int a = t ? t->attrIndex(it.key()) : -1;
        if (a < 0)
        {
            params[it.key()] = it.value();
            continue;
        }
        const scene::AttrDef& def = t->attrs[size_t(a)];
        try
        {
            json typed = scene::toJson(scene::fromJson(def, it.value()));
            if (typed != scene::toJson(def.defaultValue))
                params[it.key()] = std::move(typed);
        }
        catch (const std::exception&)
        {
            params[it.key()] = it.value();
        }
    }
    return {{"kind", kind}, {"params", params}};
}

json canonicalStack(const json& stack)
{
    json out = json::array();
    for (const json& m : stack.is_array() ? stack : json::array())
        out.push_back(canonicalEntry(m));
    return out;
}

scene::PlugValue bakeParamValue(const scene::AttrDef& d, const json& params)
{
    if (params.is_object() && params.contains(d.name))
        try
        {
            return scene::fromJson(d, params[d.name]);
        }
        catch (const std::exception&)
        {
        }
    return d.defaultValue;
}

/** A plug value as its flag field holds it; enums go by name. */
json fieldValue(const scene::AttrDef& d, const scene::PlugValue& v)
{
    json j = scene::toJson(v);
    if (d.type == scene::PlugType::Enum && j.is_number_integer())
    {
        const int64_t i = j.get<int64_t>();
        return i >= 0 && i < int64_t(d.enumNames.size()) ? json(d.enumNames[size_t(i)]) : json("");
    }
    return j;
}

/** The highs' stacks a tree row edits: the High row's own, else the one every high of its group shares. */
json stackFor(const QTreeWidgetItem* it, bool* mixed = nullptr)
{
    if (mixed)
        *mixed = false;
    scene::Node* n = it ? findNode(ss(it->data(0, kGroupRole).toString())) : nullptr;
    if (!n || !bake::isGroup(*n))
        return json::array();
    const std::vector<bake::HighEntry> highs = bake::groupSettings(*n).highs;
    auto stackOf = [](const bake::HighEntry& h) { return h.modifiers.is_array() ? h.modifiers : json::array(); };
    if (it->data(0, kKindRole).toInt() == kHighItem)
    {
        const std::string node = ss(it->data(0, kNodeRole).toString());
        for (const bake::HighEntry& h : highs)
            if (node.empty() ? h.self : h.node == node)
                return stackOf(h);
        return json::array();
    }
    if (highs.empty())
        return json::array();
    const json first = canonicalStack(stackOf(highs.front()));
    for (const bake::HighEntry& h : highs)
        if (canonicalStack(stackOf(h)) != first)
        {
            if (mixed)
                *mixed = true;
            return json::array();
        }
    return stackOf(highs.front());
}

/** Known per-map options, so they get proper fields; any other key in a map's options gets a generic one. */
struct MapOption
{
    const char* map;
    const char* key;
    opt::FieldKind kind;
    json def;
    std::vector<std::string> choices;
    const char* doc;
};

const std::vector<MapOption>& mapOptions()
{
    static const std::vector<MapOption> o = {
        {"normal", "greenUp", opt::FieldKind::Check, false, {}, "OpenGL normals (green up); off writes DirectX, as Unreal reads them."},
        {"normal", "flipX", opt::FieldKind::Check, false, {}, "Flip the red channel."},
        {"normal", "highNormalMap", opt::FieldKind::Check, false, {}, "Add each high's material normal map. Self highs never use it."},
        {"height", "range", opt::FieldKind::Float, 0.0, {}, "Distance in cm that maps to 0 and 1 around the 0.5 low; 0 is auto."},
        {"id", "source", opt::FieldKind::Choice, "material", {"material", "object", "group", "vertexColor"}, "What gets a colour."},
        {"baseColor", "source", opt::FieldKind::Choice, "material", {"material", "vertexColor"},
         "Where the colour comes from: the high's material, or its vertex colours (polypaint)."},
        {"baseColor", "set", opt::FieldKind::Text, "", {}, "With vertexColor: the colour set to read; empty is the first."},
        {"ao", "rays", opt::FieldKind::Int, 64, {}, "Rays per sample."},
        {"ao", "distance", opt::FieldKind::Float, 0.0, {}, "How far occluders count, cm; 0 is the group's high bounds."},
        {"ao", "scope", opt::FieldKind::Choice, "group", {"group", "self", "all"}, "Occluders: the group's highs, the hit high only, or every group's."},
    };
    return o;
}

opt::Field field(std::string flag, std::string label, opt::FieldKind kind, json def, std::string doc = {})
{
    opt::Field f;
    f.flag = std::move(flag);
    f.label = std::move(label);
    f.kind = kind;
    f.defaultValue = std::move(def);
    f.doc = std::move(doc);
    return f;
}

/** The low's first material slot, for the {material} token in the preview. */
/** The {material} names the project's lows give (a mesh with no materials goes by its name), plus any with own settings. */
std::vector<std::string> textureSets(const scene::Node& project)
{
    std::set<std::string> out;
    for (const auto& [name, v] : bake::projectSettings(project).output.materials)
        out.insert(name);
    scene::Graph& g = graph();
    for (scene::NodeId id : bake::groupsOf(g, project.id()))
        for (const std::string& low : bake::groupSettings(*g.node(id)).lows)
        {
            scene::Node* n = findNode(low);
            if (!n || !n->type().isA("mesh"))
                continue;
            try
            {
                const scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
                if (!m)
                    continue;
                const auto usage = scene::materials::slotUsage(*m);
                for (const auto& [slot, faces] : usage)
                    if (faces > 0)
                        out.insert(slot == scene::materials::kDefaultSlot && usage.size() == 1 ? shortName(low) : slot);
            }
            catch (const std::exception&)
            {
            }
        }
    return {out.begin(), out.end()};
}

std::string firstMaterial(const std::string& mesh)
{
    scene::Node* n = findNode(mesh);
    if (!n || !n->type().isA("mesh"))
        return {};
    try
    {
        if (scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
            for (const auto& [slot, faces] : scene::materials::slotUsage(*m))
                if (faces > 0)
                    return slot;
    }
    catch (const std::exception&)
    {
    }
    return {};
}
} // namespace

void BakerPanel::setProject(const std::string& path)
{
    if (path != mProject)
        mResultFolder.clear();
    mProject = path;
    currentProject() = path;
    refresh();
}

void BakerPanel::scheduleRefresh()
{
    if (mPending)
        return;
    mPending = true;
    QTimer::singleShot(0, this, [this] {
        mPending = false;
        refresh();
    });
}

void BakerPanel::refresh()
{
    LILA_ZONE();

    mUpdating = true;
    refreshProjects();
    updateLintWatch();
    if (mProject != mLintedProject)
        scheduleLint();
    refreshTargets();
    refreshGroups();
    refreshDetails();
    refreshMaps();
    refreshOutput();
    mUpdating = false;
    updateButtons();
    if (scene::Node* p = findNode(mProject); p && bake::isProject(*p))
    {
        const json last = bake::lastResult(p->id());
        if (!last.is_null() && last.dump() != mShownResult)
            showResult(last);
    }
    syncPreviewNote();
}

void BakerPanel::refreshProjects()
{
    scene::Graph& g = graph();
    const std::vector<scene::NodeId> ids = bake::projects(g);
    std::vector<std::string> paths;
    for (scene::NodeId id : ids)
        paths.push_back(g.node(id)->path());
    if (std::ranges::find(paths, mProject) == paths.end())
    {
        const std::string& remembered = currentProject();
        mProject = std::ranges::find(paths, remembered) != paths.end() ? remembered : paths.empty() ? std::string() : paths.front();
    }
    currentProject() = mProject;
    QSignalBlocker b(mProjects);
    mProjects->clear();
    for (const std::string& p : paths)
        mProjects->addItem(icon("texture"), qs(shortName(p)), qs(p));
    if (paths.empty())
        mProjects->addItem("No Project", QString());
    mProjects->setCurrentIndex(std::max(0, mProjects->findData(qs(mProject))));
}

void BakerPanel::refreshTargets()
{
    if (mTarget.empty())
        mTarget = targetPref();
    if (scene::Node* p = findNode(mProject); p && bake::isProject(*p))
        if (std::string t = bake::projectSettings(*p).target; !t.empty())
            mTarget = t;
    QSignalBlocker b(mTargets);
    mTargets->clear();
    for (const bake::TargetProfile& t : bake::targets())
    {
        const bool preset = !bake::isBuiltinTarget(t);
        mTargets->addItem(qs(t.label.empty() ? t.name : t.label), qs(t.name));
        mTargets->setItemData(mTargets->count() - 1, preset, kPresetRole);
        const QString where = preset ? QString("Saved preset (%1)").arg(qs(t.file.generic_string())) : QString();
        mTargets->setItemData(mTargets->count() - 1, t.doc.empty() ? where : where.isEmpty() ? qs(t.doc) : qs(t.doc) + "\n" + where, Qt::ToolTipRole);
    }
    setComboDefault(mTargets, mTargets->findData(QString("substancePainter")));
    int at = mTargets->findData(qs(mTarget));
    if (at < 0 && mTargets->count() > 0)
    {
        at = 0;
        mTarget = ss(mTargets->itemData(0).toString());
    }
    mTargets->setCurrentIndex(at);
}

void BakerPanel::refreshGroups()
{
    scene::Graph& g = graph();
    const std::string current = currentGroup();
    const int currentKind = mGroups->currentItem() ? mGroups->currentItem()->data(0, kKindRole).toInt() : 0;
    const QString currentNode = mGroups->currentItem() ? mGroups->currentItem()->data(0, kNodeRole).toString() : QString();
    const QColor dim = palette().color(QPalette::Disabled, QPalette::Text);

    json stats = json::object();
    scene::Node* project = findNode(mProject);
    if (project && bake::isProject(*project))
        if (const json last = bake::lastResult(project->id()); last.is_object())
            for (const json& s : last["stats"].value("groups", json::array()))
                stats[s.value("group", std::string())] = s;

    mGroups->setUpdatesEnabled(false);
    mGroups->clear();
    QTreeWidgetItem* restore = nullptr;
    if (!project || !bake::isProject(*project))
    {
        auto* hint = new QTreeWidgetItem(mGroups, {"No bake project. Pick a target, then Bake for Target."});
        hint->setForeground(0, dim);
        mGroups->setUpdatesEnabled(true);
        return;
    }
    auto child = [&](QTreeWidgetItem* parent, int kind, const std::string& group, const QString& text, const QString& info, const char* iconName,
                     const std::string& node = {}) {
        auto* it = new QTreeWidgetItem(parent, {text, info});
        it->setData(0, kKindRole, kind);
        it->setData(0, kGroupRole, qs(group));
        it->setData(0, kNodeRole, qs(node));
        if (iconName)
            it->setIcon(0, icon(iconName));
        it->setForeground(1, dim);
        if (group == current && kind == currentKind && it->data(0, kNodeRole).toString() == currentNode)
            restore = it;
        return it;
    };
    // Issues -> one row per check (title and how many) -> one row per mesh with its number; the full message and the fix are tooltips.
    std::vector<LintIssue> issues = lintIssues(mLint);
    auto fold = [this](QTreeWidgetItem* it, const QString& key, bool open) {
        it->setData(0, kFoldKeyRole, key);
        it->setExpanded(open != mLintToggled.contains(key));
    };
    auto addIssues = [&](QTreeWidgetItem* parent, const std::string& group, const std::vector<LintIssue>& list) {
        if (list.empty())
            return;
        std::vector<std::pair<std::string, std::vector<const LintIssue*>>> byCheck;
        for (const LintIssue& li : list)
        {
            auto it = std::ranges::find_if(byCheck, [&](const auto& c) { return c.first == li.check; });
            (it == byCheck.end() ? byCheck.emplace_back(li.check, std::vector<const LintIssue*>{}).second : it->second).push_back(&li);
        }
        QTreeWidgetItem* folder = parent ? child(parent, kLintItem, group, "Issues", QString::number(list.size()), "alert-triangle")
                                         : new QTreeWidgetItem(mGroups, {"Issues", QString::number(list.size())});
        if (!parent)
        {
            folder->setData(0, kKindRole, kLintItem);
            folder->setIcon(0, icon("alert-triangle"));
            folder->setForeground(1, dim);
        }
        for (const auto& [check, rows] : byCheck)
        {
            const LintCheckView view = lintCheckView(check);
            const QString summary = rows.size() == 1 ? lintMetric(*rows.front()) : QString("%1 meshes").arg(rows.size());
            QTreeWidgetItem* c = child(folder, kLintItem, group, view.title, summary, nullptr, rows.size() == 1 ? rows.front()->mesh : std::string());
            c->setToolTip(0, view.fix.isEmpty() ? qs(rows.front()->message) : view.fix);
            if (rows.size() == 1)
            {
                c->setToolTip(0, qs(shortName(rows.front()->mesh)) + ": " + qs(rows.front()->message) + "\n\n" + view.fix);
                c->setData(0, kComponentsRole, lintComponents(*rows.front()));
                continue;
            }
            for (const LintIssue* li : rows)
            {
                const QString metric = lintMetric(*li);
                QTreeWidgetItem* m = child(c, kLintItem, group, li->mesh.empty() ? QString("(group)") : qs(shortName(li->mesh)),
                                           metric.isEmpty() ? qs(li->message) : metric, "box", li->mesh);
                m->setToolTip(0, qs(li->message));
                m->setToolTip(1, qs(li->message));
                m->setData(0, kComponentsRole, lintComponents(*li));
            }
            fold(c, qs(group) + "|" + qs(check), false);
        }
        fold(folder, qs(group) + "|", true);
    };
    for (scene::NodeId id : bake::groupsOf(g, project->id()))
    {
        scene::Node* grp = g.node(id);
        const std::string path = grp->path();
        const bake::GroupSettings s = bake::groupSettings(*grp);
        QString info = QString("%1 low, %2 high").arg(s.lows.size()).arg(s.highs.size());
        if (const json st = stats.value(grp->name(), json()); st.is_object() && st.value("samples", 0) > 0)
            info += QString(", %1% missed").arg(100.0 * st.value("misses", 0.0) / st.value("samples", 1.0), 0, 'f', 2);
        auto* item = new QTreeWidgetItem(mGroups, {qs(grp->name()), info});
        item->setData(0, kKindRole, kGroupItem);
        item->setData(0, kGroupRole, qs(path));
        item->setIcon(0, icon("box"));
        item->setForeground(1, dim);
        if (path == current && currentKind == kGroupItem)
            restore = item;

        QTreeWidgetItem* lows = child(item, kLowsItem, path, "Low", s.lows.empty() ? "drop meshes here" : QString(), "arrow-down");
        for (const std::string& l : s.lows)
        {
            QTreeWidgetItem* r = child(lows, kLowItem, path, qs(shortName(l)), QString(), "cube", l);
            if (!findNode(l))
                r->setText(1, "missing");
        }
        QTreeWidgetItem* highs = child(item, kHighsItem, path, "High", s.highs.empty() ? "drop meshes here, or tick Self as high" : QString(), "arrow-up");
        for (const bake::HighEntry& h : s.highs)
        {
            QTreeWidgetItem* r = child(highs, kHighItem, path, h.self ? QString("(self)") : qs(shortName(h.node)), modifierSummary(h.modifiers),
                                       h.self ? "refresh" : "cube", h.self ? std::string() : h.node);
            if (!h.self && !findNode(h.node))
                r->setText(1, "missing");
        }
        child(item, kCageItem, path, "Cage", s.cage.empty() ? QString("auto, offset x%1").arg(s.cageOffset) : qs(shortName(s.cage)), "box", s.cage);

        std::vector<LintIssue> mine;
        for (LintIssue& li : issues)
            if (!li.message.empty() &&
                (li.group == path || li.group == grp->name() ||
                 (!li.mesh.empty() && (std::ranges::find(s.lows, li.mesh) != s.lows.end() ||
                                       std::ranges::any_of(s.highs, [&](const bake::HighEntry& h) { return h.node == li.mesh; })))))
                mine.push_back(std::exchange(li, {}));
        addIssues(item, path, mine);
        item->setExpanded(true);
        lows->setExpanded(true);
        highs->setExpanded(true);
    }
    // Issues no group claims sit at the top level.
    std::erase_if(issues, [](const LintIssue& li) { return li.message.empty(); });
    addIssues(nullptr, std::string(), issues);
    if (restore)
        mGroups->setCurrentItem(restore);
    else if (mGroups->topLevelItemCount() > 0 && mGroups->topLevelItem(0)->data(0, kKindRole).toInt() == kGroupItem)
        mGroups->setCurrentItem(mGroups->topLevelItem(0));
    mGroups->setUpdatesEnabled(true);
}

void BakerPanel::removeSelectedRows()
{
    const QList<QTreeWidgetItem*> rows = mGroups->selectedItems();
    std::vector<std::string> doomed;
    for (QTreeWidgetItem* it : rows)
        if (it->data(0, kKindRole).toInt() == kGroupItem)
            doomed.push_back(ss(it->data(0, kGroupRole).toString()));
    UndoChunk chunk("Remove from Bake Groups");
    for (QTreeWidgetItem* it : rows)
    {
        const std::string g = ss(it->data(0, kGroupRole).toString());
        const std::string n = ss(it->data(0, kNodeRole).toString());
        if (std::ranges::find(doomed, g) != doomed.end())
            continue;
        switch (it->data(0, kKindRole).toInt())
        {
        case kLowItem: run("bakeGroup", {{"args", {g}}, {"removeLow", {n}}}, false); break;
        case kHighItem:
            if (n.empty())
                run("bakeGroup", {{"args", {g}}, {"self", false}}, false);
            else
                run("bakeGroup", {{"args", {g}}, {"removeHigh", {n}}}, false);
            break;
        case kCageItem:
            if (!n.empty())
                run("bakeGroup", {{"args", {g}}, {"cage", ""}}, false);
            break;
        default: break;
        }
    }
    if (!doomed.empty())
        runUi("delete", {{"args", doomed}});
}

void BakerPanel::removeSelectedMaps()
{
    QList<QTreeWidgetItem*> rows = mMaps->selectedItems();
    if (rows.isEmpty() && mMaps->currentItem())
        rows << mMaps->currentItem();
    std::vector<std::string> maps;
    for (QTreeWidgetItem* it : rows)
        maps.push_back(ss(it->data(0, kNodeRole).toString()));
    if (maps.empty())
        return;
    UndoChunk chunk("Remove Bake Maps");
    for (const std::string& m : maps)
        run("setBakeMap", {{"args", {m}}, {"remove", true}});
}

std::string BakerPanel::currentGroup() const
{
    if (QTreeWidgetItem* it = mGroups->currentItem())
        return ss(it->data(0, kGroupRole).toString());
    return {};
}

std::vector<std::string> BakerPanel::selectedGroups() const
{
    std::vector<std::string> out;
    for (QTreeWidgetItem* it : mGroups->selectedItems())
        if (const std::string g = ss(it->data(0, kGroupRole).toString()); !g.empty() && std::ranges::find(out, g) == out.end())
            out.push_back(g);
    return out;
}

void BakerPanel::refreshDetails()
{
    const bool was = std::exchange(mUpdating, true);
    const std::string g = currentGroup();
    scene::Node* n = g.empty() ? nullptr : findNode(g);
    const bool has = n && bake::isGroup(*n);
    if (mCageStatus && mCageGroup != g)
        mCageStatus->clear();
    mCageGroup = g;
    mCageRow->setEnabled(has);
    mDetailsEmpty->setVisible(!has);
    for (QWidget* w : {static_cast<QWidget*>(mFrontal), static_cast<QWidget*>(mRear), static_cast<QWidget*>(mCageOffset),
                       static_cast<QWidget*>(mAoScope), static_cast<QWidget*>(mBackfaces), static_cast<QWidget*>(mSelf),
                       static_cast<QWidget*>(mUnlinks->parentWidget())})
        w->setEnabled(has);
    mStackBox->setVisible(has);
    if (has)
    {
        const bake::GroupSettings s = bake::groupSettings(*n);
        // The latest measured Auto distances: a full Check's, else the last bake's.
        json measured;
        auto findMeasured = [&](const json& list) {
            if (measured.is_null() && list.is_array())
                for (const json& d : list)
                    if (d.is_object() && d.value("group", std::string()) == n->name() && d.value("measured", false))
                        measured = d;
        };
        findMeasured(mLint.is_object() ? mLint.value("distances", json()) : json());
        if (scene::Node* p = findNode(mProject); p && bake::isProject(*p))
            if (const json last = bake::lastResult(p->id()); last.is_object() && last.contains("stats"))
                findMeasured(last["stats"].value("groups", json()));
        auto autoText = [&](const char* key) {
            return measured.is_null() ? QString("Auto") : QString("Auto (%1 cm)").arg(measured.value(key, 0.0), 0, 'f', 1);
        };
        mFrontal->setSpecialValueText(autoText("frontal"));
        mRear->setSpecialValueText(autoText("rear"));
        mFrontal->setValue(s.frontal);
        mRear->setValue(s.rear);
        mCageOffset->setValue(s.cageOffset);
        mShowCage->setChecked(bakeCage::shown(n->id()));
        mLivePreview->setChecked(bakeCage::livePreview());
        mAoScope->setCurrentIndex(std::max(0, mAoScope->findData(qs(s.aoScope))));
        mUnlinks->clear();
        for (const bake::Unlink& u : s.unlinks)
        {
            QStringList lows;
            for (const std::string& l : u.lows)
                lows << qs(shortName(l));
            auto* row = new QTreeWidgetItem(mUnlinks, {lows.isEmpty() ? QString("All lows") : lows.join(", ")});
            row->setIcon(0, icon("arrow-down"));
            row->setToolTip(0, lows.isEmpty() ? QString("Every low in the group") : lows.join('\n'));
            row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
            row->setCheckState(1, u.projection ? Qt::Checked : Qt::Unchecked);
            for (const std::string& m : u.meshes)
            {
                auto* mesh = new QTreeWidgetItem(row, {qs(shortName(m))});
                mesh->setIcon(0, icon("box"));
                mesh->setToolTip(0, qs(m));
            }
            for (const std::string& m : u.materials)
            {
                auto* material = new QTreeWidgetItem(row, {qs(m)});
                material->setIcon(0, icon("palette"));
                material->setToolTip(0, QString("Material: every high with faces of %1").arg(qs(m)));
            }
            row->setExpanded(!mUnlinksCollapsed.contains(row->text(0)));
        }
        mBackfaces->setChecked(s.ignoreBackfaces);
        mSelf->setChecked(std::ranges::any_of(s.highs, [](const bake::HighEntry& h) { return h.self; }));

        QTreeWidgetItem* it = mGroups->currentItem();
        const bool high = it && it->data(0, kKindRole).toInt() == kHighItem;
        bool mixed = false;
        const json stack = stackFor(it, &mixed);
        const bool editable = high || (!mixed && !s.highs.empty());
        const int row = mStack->currentRow();
        {
            QSignalBlocker b(mStack);
            mStack->clear();
            for (const json& m : stack)
            {
                auto* r = new QListWidgetItem(m.is_string() ? qs(m.get<std::string>()) : modifierSummary(json::array({m})), mStack);
                if (m.is_object() && m.contains("params"))
                    r->setToolTip(qs(m["params"].dump()));
            }
            mStack->setCurrentRow(std::min(std::max(row, 0), mStack->count() - 1));
        }
        mStack->setVisible(editable);
        mStackButtons->setVisible(editable);
        mParams->setVisible(editable);
        if (high)
            mStackTitle->setText("Bake-only modifiers on " + it->text(0) + ", evaluated into the bake's snapshot only:");
        else if (s.highs.empty())
            mStackTitle->setText("Bake-only modifiers: add a high, or tick Self as high, to give it a stack.");
        else if (mixed)
            mStackTitle->setText("The highs carry different stacks. Pick a preset to give every high the same one, or select a High row to edit "
                                 "its own.");
        else
            mStackTitle->setText("Bake-only modifiers shared by every high in the group; edits apply to all of them:");
        mPreset->setEnabled(!s.highs.empty());
        mPreset->setItemText(0, mixed ? "Mixed" : "Custom");
        int preset = 0;
        if (!mixed)
        {
            const json canonical = canonicalStack(stack);
            for (int i = 1; i < mPreset->count() && preset == 0; ++i)
                if (const json p = bake::modifierPreset(ss(mPreset->itemData(i).toString())); p.is_array() && canonicalStack(p) == canonical)
                    preset = i;
        }
        mPreset->setCurrentIndex(preset);
        refreshParams();
    }
    mUpdating = was;
}

json BakerPanel::currentStack() const
{
    return stackFor(mGroups->currentItem());
}

void BakerPanel::setStack(const json& stack)
{
    QTreeWidgetItem* it = mGroups->currentItem();
    const std::string g = it ? ss(it->data(0, kGroupRole).toString()) : std::string();
    if (g.empty())
        return;
    json call = {{"args", {g}}, {"modifiers", stack}};
    // Without a high named, bakeGroup sets every high's stack.
    if (it->data(0, kKindRole).toInt() == kHighItem)
    {
        const std::string node = ss(it->data(0, kNodeRole).toString());
        if (node.empty())
            call["self"] = true;
        else
            call["addHigh"] = {node};
    }
    run("bakeGroup", call, false);
}

void BakerPanel::refreshParams()
{
    const json stack = currentStack();
    const int row = mStack->currentRow();
    const json entry = row >= 0 && row < int(stack.size()) ? stack[size_t(row)] : json();
    const std::string kind = entryKind(entry);
    const scene::NodeType* type = modifierType(kind);
    const json params = entry.is_object() ? entry.value("params", json::object()) : json::object();
    QTreeWidgetItem* it = mGroups->currentItem();
    std::string key;
    if (it && !kind.empty())
        key = ss(it->data(0, kGroupRole).toString()) + "|" + ss(it->data(0, kNodeRole).toString()) + "|" + std::to_string(it->data(0, kKindRole).toInt()) +
              "|" + std::to_string(row) + "|" + kind;
    // Rebuilding under a field being edited would drop its focus and spin repeat.
    if (!key.empty() && key == mParamKey)
    {
        for (const auto& set : mParamSetters)
            set(params);
        return;
    }
    mParamKey = key;
    mParamSetters.clear();
    while (mParamForm->rowCount() > 0)
        mParamForm->removeRow(0);
    if (kind.empty())
        return;
    if (!type)
    {
        mParamForm->addRow(new QLabel(qs("'" + kind + "' is not a registered modifier; bakes skip it"), mParams));
        return;
    }
    for (const scene::AttrDef* d : editableParams(*type))
    {
        opt::Field f = field(d->name, d->niceName.empty() ? opt::prettyLabel(d->name) : d->niceName, opt::FieldKind::Text, json(), d->doc);
        switch (d->type)
        {
        case scene::PlugType::Bool: f.kind = opt::FieldKind::Check; break;
        case scene::PlugType::Int: f.kind = opt::FieldKind::Int; break;
        case scene::PlugType::Float: f.kind = opt::FieldKind::Float; break;
        case scene::PlugType::Vec3: f.kind = opt::FieldKind::Vec3; break;
        case scene::PlugType::Enum:
            f.kind = opt::FieldKind::Choice;
            f.choices = d->enumNames;
            f.choiceDocs = choiceTips::forAttr(*d);
            break;
        default: break;
        }
        f.defaultValue = fieldValue(*d, d->defaultValue);
        if (d->minValue)
            f.min = *d->minValue;
        if (d->maxValue)
            f.max = *d->maxValue;
        if (d->step)
            f.step = *d->step;
        FlagFieldWidgets w = makeFlagField(
            f, fieldValue(*d, bakeParamValue(*d, params)),
            [this, row, name = d->name](const json& v) {
                if (mUpdating)
                    return;
                json s = currentStack();
                if (row < 0 || row >= int(s.size()))
                    return;
                json& m = s[size_t(row)];
                if (m.is_string())
                    m = {{"kind", m.get<std::string>()}};
                if (!m.contains("params") || !m["params"].is_object())
                    m["params"] = json::object();
                if (m["params"].contains(name) && m["params"][name] == v)
                    return;
                m["params"][name] = v;
                setStack(s);
            },
            mParams);
        w.field->setObjectName(qs("bakerParam_" + d->name));
        if (w.label)
            mParamForm->addRow(w.label, w.field);
        else
            mParamForm->addRow(QString(), w.field);
        mParamSetters.push_back([d, set = w.set](const json& p) {
            if (set)
                set(fieldValue(*d, bakeParamValue(*d, p)));
        });
    }
}

void BakerPanel::refreshMaps()
{
    const std::string current = mMaps->currentItem() ? ss(mMaps->currentItem()->data(0, kNodeRole).toString()) : std::string();
    QSignalBlocker b(mMaps);
    mMaps->clear();
    scene::Node* p = findNode(mProject);
    if (p && bake::isProject(*p))
    {
        const bake::ProjectSettings s = bake::projectSettings(*p);
        const QColor dim = palette().color(QPalette::Disabled, QPalette::Text);
        QTreeWidgetItem* restore = nullptr;
        for (const bake::MapSettings& m : s.maps)
        {
            const bake::MapDef* def = bake::findMap(m.map);
            auto* it = new QTreeWidgetItem(mMaps);
            it->setText(0, qs(def && !def->label.empty() ? def->label : m.map));
            it->setData(0, kNodeRole, qs(m.map));
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(0, m.enabled ? Qt::Checked : Qt::Unchecked);
            it->setText(1, qs(m.suffix.empty() && def ? def->suffix : m.suffix));
            it->setText(2, m.bits ? QString::number(m.bits) : QString("(%1)").arg(s.output.bits));
            it->setText(3, m.format.empty() ? QString("(%1)").arg(qs(s.output.format)) : qs(m.format));
            if (!def)
            {
                for (int c = 0; c < 4; ++c)
                    it->setForeground(c, dim);
                it->setToolTip(0, "Not implemented in this build; bakes skip it");
            }
            else
                it->setToolTip(0, qs(def->doc));
            if (m.map == current)
                restore = it;
        }
        mMaps->setCurrentItem(restore ? restore : mMaps->topLevelItem(0));
    }
    refreshMapSettings();
}

void BakerPanel::refreshMapSettings()
{
    QTreeWidgetItem* it = mMaps->currentItem();
    scene::Node* p = findNode(mProject);
    const std::string id = it ? ss(it->data(0, kNodeRole).toString()) : std::string();
    const bake::ProjectSettings s = p && bake::isProject(*p) ? bake::projectSettings(*p) : bake::ProjectSettings{};
    auto m = std::ranges::find_if(s.maps, [&](const bake::MapSettings& x) { return x.map == id; });
    // Rebuilding under a field being edited would drop its focus and spin repeat.
    const std::string formKey = m == s.maps.end() ? std::string() : mProject + m->toJson().dump() + s.output.format + std::to_string(s.output.bits);
    if (formKey == mMapFormKey && mMapForm->rowCount() > 0)
        return;
    mMapFormKey = formKey;
    while (mMapForm->rowCount() > 0)
        mMapForm->removeRow(0);
    if (m == s.maps.end())
        return;
    const bake::MapDef* def = bake::findMap(id);

    auto add = [this, id](const opt::Field& f, const json& value, std::function<json(const json&)> toFlag) {
        FlagFieldWidgets w = makeFlagField(
            f, value,
            [this, id, flag = f.flag, toFlag](const json& v) {
                if (!mUpdating)
                    run("setBakeMap", {{"args", {id}}, {flag, toFlag ? toFlag(v) : v}});
            },
            mMapSettings);
        if (w.label)
            mMapForm->addRow(w.label, w.field);
        else
            mMapForm->addRow(QString(), w.field);
    };
    add(field("suffix", "Suffix", opt::FieldKind::Text, def ? json(def->suffix) : json(""), "The {map} token; empty uses the map's default"), m->suffix,
        {});
    opt::Field bits = field("bits", "Bits", opt::FieldKind::Choice, "0", "PNG bit depth for this map");
    bits.choices = {"0", "8", "16", "32"};
    bits.choiceLabels = {QString("Output's (%1)").arg(s.output.bits).toStdString(), "8", "16", "32"};
    add(bits, std::to_string(m->bits), [](const json& v) { return json(std::stoi(v.get<std::string>())); });
    opt::Field format = field("format", "Format", opt::FieldKind::Choice, "", "File format for this map");
    format.choices = {"", "png", "exr", "tga", "tif"};
    format.choiceLabels = {"Output's (" + s.output.format + ")", "PNG", "OpenEXR", "TGA", "TIFF"};
    add(format, m->format, {});

    std::vector<std::string> shown;
    auto option = [&](const opt::Field& f, const json& value) {
        shown.push_back(f.flag);
        FlagFieldWidgets w = makeFlagField(
            f, value,
            [this, id, key = f.flag](const json& v) {
                if (!mUpdating)
                    run("setBakeMap", {{"args", {id}}, {"options", {{key, v}}}});
            },
            mMapSettings);
        mMapForm->addRow(w.label ? w.label : new QLabel(mMapSettings), w.field);
    };
    for (const MapOption& o : mapOptions())
        if (id == o.map)
        {
            opt::Field f = field(o.key, opt::prettyLabel(o.key), o.kind, o.def, o.doc);
            f.choices = o.choices;
            option(f, m->options.value(o.key, o.def));
        }
    if (def && def->pack.is_array())
    {
        shown.push_back("channels");
        addPackChannels(id, m->options.contains("channels") && m->options["channels"].is_array() ? m->options["channels"] : def->pack);
    }
    for (auto& [key, value] : m->options.items())
    {
        if (std::ranges::find(shown, key) != shown.end())
            continue;
        if (value.is_boolean())
            option(field(key, opt::prettyLabel(key), opt::FieldKind::Check, false), value);
        else if (value.is_number_integer())
            option(field(key, opt::prettyLabel(key), opt::FieldKind::Int, 0), value);
        else if (value.is_number())
            option(field(key, opt::prettyLabel(key), opt::FieldKind::Float, 0.0), value);
        else if (value.is_string())
            option(field(key, opt::prettyLabel(key), opt::FieldKind::Text, ""), value);
    }
    // Anything else (palettes, nested settings) through raw JSON.
    auto* raw = new QLineEdit(qs(m->options.dump()), mMapSettings);
    raw->setToolTip("Every option of this map as JSON, merged in on Enter (null removes a key)" + (def ? "\n\n" + qs(def->doc) : QString()));
    connect(raw, &QLineEdit::editingFinished, this, [this, raw, id, before = m->options] {
        json v = json::parse(ss(raw->text()), nullptr, false);
        if (v.is_discarded() || !v.is_object())
        {
            log::warn("Baker: map options must be a JSON object");
            return;
        }
        if (v != before)
            run("setBakeMap", {{"args", {id}}, {"options", v}});
    });
    mMapForm->addRow("Options", raw);
}

void BakerPanel::setUnlinks(const std::string& group, const std::vector<bake::Unlink>& unlinks)
{
    json list = json::array();
    for (const bake::Unlink& u : unlinks)
        list.push_back(u.toJson());
    run("bakeGroup", {{"args", {group}}, {"unlinks", list}}, false);
}

void BakerPanel::unlinkSelected(const std::string& group, const std::string& low, bool projection)
{
    scene::Node* n = findNode(group);
    const std::vector<std::string> meshes = selectedMeshes();
    if (!n || !bake::isGroup(*n) || meshes.empty())
    {
        log::warn("Unlink: pick a bake group (or one of its Low rows) and select the meshes to unlink");
        return;
    }
    std::vector<bake::Unlink> all = bake::groupSettings(*n).unlinks;
    const std::vector<std::string> lows = low.empty() ? std::vector<std::string>{} : std::vector<std::string>{low};
    auto same = std::ranges::find_if(all, [&](const bake::Unlink& u) { return u.lows == lows && u.projection == projection; });
    if (same == all.end())
        all.push_back({.lows = lows, .meshes = meshes, .projection = projection});
    else
        for (const std::string& m : meshes)
            if (std::ranges::find(same->meshes, m) == same->meshes.end())
                same->meshes.push_back(m);
    setUnlinks(group, all);
}

void BakerPanel::unlinkMaterial(const std::string& group, const std::string& low, const std::string& material, bool projection)
{
    scene::Node* n = findNode(group);
    if (!n || !bake::isGroup(*n) || material.empty())
        return;
    std::vector<bake::Unlink> all = bake::groupSettings(*n).unlinks;
    const std::vector<std::string> lows = low.empty() ? std::vector<std::string>{} : std::vector<std::string>{low};
    auto same = std::ranges::find_if(all, [&](const bake::Unlink& u) { return u.lows == lows && u.projection == projection; });
    if (same == all.end())
        all.push_back({.lows = lows, .materials = {material}, .projection = projection});
    else if (std::ranges::find(same->materials, material) == same->materials.end())
        same->materials.push_back(material);
    setUnlinks(group, all);
}

std::vector<std::string> BakerPanel::unlinkableMaterials() const
{
    scene::Graph& g = graph();
    scene::Node* grp = findNode(currentGroup());
    scene::Node* project = grp && bake::isGroup(*grp) ? bake::groupProject(g, *grp) : nullptr;
    if (!project)
        return {};
    std::set<std::string> out;
    auto add = [&](const std::string& mesh) {
        scene::Node* n = findNode(mesh);
        if (!n || !n->type().isA("mesh"))
            return;
        try
        {
            const scene::PlugValue v = g.value({n->id(), scene::MeshNode::kOutMesh, -1});
            if (const scene::MeshPtr* m = std::get_if<scene::MeshPtr>(&v); m && *m)
                for (const auto& [slot, faces] : scene::materials::slotUsage(**m))
                    if (faces > 0)
                        out.insert(slot);
        }
        catch (const std::exception&)
        {
        }
    };
    for (scene::NodeId id : bake::groupsOf(g, project->id()))
    {
        const bake::GroupSettings s = bake::groupSettings(*g.node(id));
        for (const bake::HighEntry& h : s.highs)
            if (h.self)
                std::ranges::for_each(s.lows, add);
            else if (h.file.empty())
                add(h.node);
    }
    return {out.begin(), out.end()};
}

void BakerPanel::removeSelectedUnlinks()
{
    scene::Node* n = findNode(currentGroup());
    if (!n || !bake::isGroup(*n))
        return;
    std::vector<bake::Unlink> keep;
    const std::vector<bake::Unlink> all = bake::groupSettings(*n).unlinks;
    for (int i = 0; i < int(all.size()); ++i)
    {
        QTreeWidgetItem* row = mUnlinks->topLevelItem(i);
        if (row && row->isSelected())
            continue;
        bake::Unlink u = all[size_t(i)];
        // Child rows are the meshes, then the materials.
        const int meshes = int(u.meshes.size());
        if (row)
            for (int c = meshes + int(u.materials.size()) - 1; c >= 0; --c)
                if (QTreeWidgetItem* child = row->child(c); child && child->isSelected())
                {
                    if (c < meshes)
                        u.meshes.erase(u.meshes.begin() + c);
                    else
                        u.materials.erase(u.materials.begin() + (c - meshes));
                }
        if (!u.empty())
            keep.push_back(std::move(u));
    }
    setUnlinks(currentGroup(), keep);
}

void BakerPanel::addPackChannels(const std::string& map, const json& channels)
{
    static const char* const kChannelNames[] = {"R", "G", "B", "A", "Luminance", "Max"};
    static const char* const kChannelKeys[] = {"r", "g", "b", "a", "luminance", "max"};
    // Four object entries, so a row edit rewrites the whole layout and keeps keys the rows don't show (options).
    auto layout = std::make_shared<json>(json::array());
    for (size_t c = 0; c < 4; ++c)
    {
        const json e = c < channels.size() ? channels[c] : json(c == 3 ? 1.0 : 0.0);
        if (e.is_number())
            layout->push_back({{"constant", e}});
        else if (e.is_string())
            layout->push_back({{"map", e}, {"channel", "r"}});
        else
            layout->push_back(e.is_object() ? e : json{{"constant", 0.0}});
    }
    auto commit = [this, map, layout] {
        if (!mUpdating)
            run("setBakeMap", {{"args", {map}}, {"options", {{"channels", *layout}}}});
    };
    std::vector<const bake::MapDef*> sources;
    for (const bake::MapDef* d : bake::allMaps())
        if (d->pack.is_null())
            sources.push_back(d);

    for (size_t c = 0; c < 4; ++c)
    {
        json& entry = (*layout)[c];
        auto* row = new QWidget(mMapSettings);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto* source = new QComboBox(row);
        source->addItem("Constant", QString());
        for (const bake::MapDef* d : sources)
            source->addItem(qs(d->label.empty() ? d->id : d->label), qs(d->id));
        auto* channel = new QComboBox(row);
        for (const char* n : kChannelNames)
            channel->addItem(n);
        channel->setToolTip("The source's channel; Luminance and Max reduce its RGB to one value (Max suits masks)");
        auto* constant = new QDoubleSpinBox(row);
        constant->setRange(0.0, 1.0);
        constant->setSingleStep(0.05);
        setSpinDefault(constant, c == 3 ? 1.0 : 0.0);
        auto* invert = new QCheckBox("Invert", row);
        setComboDefault(channel, 0);
        setCheckDefault(invert, false);

        const bool isConstant = entry.contains("constant") || !entry.contains("map");
        source->setCurrentIndex(isConstant ? 0 : std::max(0, source->findData(qs(entry.value("map", std::string())))));
        int ch = 0;
        if (const auto it = entry.find("channel"); it != entry.end() && it->is_string())
            ch = int(std::ranges::find(kChannelKeys, it->get<std::string>()) - std::begin(kChannelKeys)) % 6;
        else if (it != entry.end() && it->is_number_integer())
            ch = std::clamp(it->get<int>(), 0, 5);
        channel->setCurrentIndex(ch);
        constant->setValue(entry.value("constant", 0.0));
        invert->setChecked(entry.value("invert", false));
        channel->setVisible(!isConstant);
        invert->setVisible(!isConstant);
        constant->setVisible(isConstant);

        h->addWidget(source, 1);
        h->addWidget(channel);
        h->addWidget(constant);
        h->addWidget(invert);
        mMapForm->addRow(kChannelNames[c], row);

        connect(source, &QComboBox::currentIndexChanged, this, [layout, c, source, channel, constant, commit](int) {
            json& e = (*layout)[c];
            const std::string id = ss(source->currentData().toString());
            if (id.empty())
                e = {{"constant", constant->value()}};
            else
            {
                e.erase("constant");
                e["map"] = id;
                e["channel"] = kChannelKeys[channel->currentIndex()];
            }
            commit();
        });
        connect(channel, &QComboBox::currentIndexChanged, this, [layout, c, commit](int i) {
            (*layout)[c]["channel"] = kChannelKeys[i];
            commit();
        });
        auto commitConstant = [layout, c, constant, commit] {
            (*layout)[c] = {{"constant", constant->value()}};
            commit();
        };
        connect(constant, &QDoubleSpinBox::editingFinished, this, commitConstant);
        // A middle-click reset changes the value with no edit to finish; a held button means a scrub still going.
        connect(constant, &QDoubleSpinBox::valueChanged, this, [constant, commitConstant] {
            if (!constant->hasFocus() && QApplication::mouseButtons() == Qt::NoButton)
                commitConstant();
        });
        connect(invert, &QCheckBox::toggled, this, [layout, c, commit](bool on) {
            (*layout)[c]["invert"] = on;
            commit();
        });
    }
}

void BakerPanel::refreshOutput()
{
    scene::Node* p = findNode(mProject);
    const bool has = p && bake::isProject(*p);
    for (QWidget* w : {static_cast<QWidget*>(mFolder), static_cast<QWidget*>(mName), static_cast<QWidget*>(mFormat), static_cast<QWidget*>(mBits),
                       static_cast<QWidget*>(mResolution), static_cast<QWidget*>(mSupersampling), static_cast<QWidget*>(mPadding),
                       static_cast<QWidget*>(mAutoAssign), static_cast<QWidget*>(mRebakeOnSave)})
        w->setEnabled(has);
    if (!has)
    {
        mPreview->clear();
        mFolderResolved->clear();
        return;
    }
    const bool was = std::exchange(mUpdating, true);
    const bake::ProjectSettings ps = bake::projectSettings(*p);
    const bake::OutputSettings& project = ps.output;
    {
        const QString current = mTextureSet->currentData().toString();
        mTextureSet->clear();
        mTextureSet->addItem("Every texture set", QString());
        for (const std::string& ts : textureSets(*p))
            mTextureSet->addItem(qs(ts) + (project.materials.contains(ts) ? QString(" (own settings)") : QString()), qs(ts));
        mTextureSet->setCurrentIndex(std::max(0, mTextureSet->findData(current)));
        const bool perSet = bake::hasToken(project.name, "material");
        mTextureSet->setEnabled(perSet || mTextureSet->currentIndex() > 0);
        if (!perSet)
            mTextureSet->setCurrentIndex(0);
    }
    const std::string set = ss(mTextureSet->currentData().toString());
    mTextureSetReset->setEnabled(!set.empty() && project.materials.contains(set));
    const bake::OutputSettings o = set.empty() ? project : project.forMaterial(set);
    if (!mFolder->hasFocus())
        mFolder->setText(qs(o.folder));
    mFolderResolved->setText(qs(bake::resolveFolder(ss(mFolder->text())).generic_string()));
    mAutoAssign->setChecked(ps.autoAssign);
    mRebakeOnSave->setChecked(ps.rebakeOnSave);
    if (!mName->hasFocus())
        mName->setText(qs(o.name));
    mFormat->setCurrentIndex(std::max(0, mFormat->findData(qs(o.format))));
    mBits->setCurrentIndex(std::max(0, mBits->findData(o.bits)));
    mBits->setEnabled(o.format == "png");
    int res = mResolution->findData(o.width == o.height ? o.width : -1);
    if (res < 0)
    {
        const int custom = mResolution->findData(-1);
        const QString text = QString("%1 x %2").arg(o.width).arg(o.height);
        if (custom >= 0)
            mResolution->setItemText(custom, text);
        else
            mResolution->addItem(text, -1);
        res = mResolution->findData(-1);
    }
    mResolution->setCurrentIndex(res);
    mSupersampling->setCurrentIndex(std::max(0, mSupersampling->findData(o.supersampling)));
    mPadding->setValue(o.padding);
    mUpdating = was;
    updatePreview();
}

void BakerPanel::updatePreview()
{
    scene::Graph& g = graph();
    scene::Node* p = findNode(mProject);
    if (!p || !bake::isProject(*p))
        return;
    const bake::ProjectSettings s = bake::projectSettings(*p);
    bake::NameTokens t;
    t.project = p->name();
    t.ext = s.output.format;
    t.width = s.output.width;
    t.height = s.output.height;
    t.group = "group";
    t.mesh = "mesh";
    t.material = "material";
    for (scene::NodeId id : bake::groupsOf(g, p->id()))
    {
        const bake::GroupSettings gs = bake::groupSettings(*g.node(id));
        t.group = g.node(id)->name();
        if (!gs.lows.empty())
        {
            t.mesh = shortName(gs.lows.front());
            if (std::string m = firstMaterial(gs.lows.front()); !m.empty())
                t.material = m;
        }
        break;
    }
    const std::string tmpl = ss(mName->text());
    const QColor dim = palette().color(QPalette::Disabled, QPalette::Text);
    mPreview->setUpdatesEnabled(false);
    mPreview->clear();
    auto warn = [&](const QString& text, const QString& tip) {
        auto* w = new QTreeWidgetItem(mPreview, {text});
        w->setIcon(0, icon("alert-triangle"));
        w->setToolTip(0, tip);
        w->setFirstColumnSpanned(true);
    };
    if (const std::vector<std::string> unknown = bake::unknownTokens(tmpl); !unknown.empty())
    {
        QStringList u;
        for (const std::string& x : unknown)
            u << "{" + qs(x) + "}";
        warn("Unknown " + u.join(" "), "Kept as written");
    }
    if (!bake::hasToken(tmpl, "group") && !bake::hasToken(tmpl, "mesh") && !bake::hasToken(tmpl, "material") && bake::groupsOf(g, p->id()).size() > 1)
        warn("Groups share files", "Add {group}, {mesh} or {material} to the name");

    const bool perSet = bake::hasToken(tmpl, "material");
    std::vector<std::string> sets = perSet ? textureSets(*p) : std::vector<std::string>{};
    if (sets.empty())
        sets.push_back(t.material);
    auto sizeText = [](int w, int h) { return w == h ? QString::number(w) : QString("%1 x %2").arg(w).arg(h); };
    auto formatText = [](int bits, const std::string& format) { return QString("%1-bit %2").arg(bits).arg(qs(format).toUpper()); };
    for (const std::string& set : sets)
    {
        const bake::OutputSettings os = s.output.forMaterial(set);
        t.material = set;
        t.width = os.width;
        t.height = os.height;
        auto* row = new QTreeWidgetItem(mPreview, {perSet ? qs(set) : QString("All texture sets"), sizeText(os.width, os.height), formatText(os.bits, os.format)});
        row->setIcon(0, icon("color-swatch"));
        row->setData(0, Qt::UserRole, qs(set));
        if (s.output.materials.contains(set))
            row->setToolTip(0, "Own output settings");
        int files = 0;
        for (const bake::MapSettings& m : s.maps)
        {
            if (!m.enabled)
                continue;
            const bake::MapDef* def = bake::findMap(m.map);
            t.map = !m.suffix.empty() ? m.suffix : def ? def->suffix : m.map;
            t.ext = m.format.empty() ? os.format : m.format;
            const QString name = qs(bake::expandName(tmpl, t));
            auto* f = new QTreeWidgetItem(row, {name, sizeText(os.width, os.height), formatText(m.bits ? m.bits : os.bits, t.ext)});
            f->setIcon(0, icon("photo"));
            f->setToolTip(0, mFolderResolved->text().isEmpty() ? name : mFolderResolved->text() + "/" + name);
            f->setForeground(1, dim);
            f->setForeground(2, dim);
            ++files;
        }
        row->setText(0, row->text(0) + QString("  (%1)").arg(files));
        row->setExpanded(sets.size() == 1 || mPreviewOpen.contains(qs(set)));
    }
    mPreview->setUpdatesEnabled(true);
}

void BakerPanel::updateButtons()
{
    scene::Node* p = findNode(mProject);
    const bool has = p && bake::isProject(*p);
    const bool groups = has && !bake::groupsOf(graph(), p->id()).empty();
    mBake->setEnabled(groups);
    mBakeSelected->setEnabled(groups);
    mProjectMenu->menu()->actions().value(1)->setEnabled(has);
    mProjectMenu->menu()->actions().value(2)->setEnabled(has);
    mAddMap->setEnabled(has);
    if (mArrange)
    {
        mArrange->setEnabled(has);
        QSignalBlocker block(mArrange);
        mArrange->setChecked(has && bake::arrange::active(graph(), p->id()));
    }
    QString label = mTargets->currentText();
    mBakeTarget->setText("Bake for " + (label.isEmpty() ? QString("Target") : label));
    mBakeTarget->setEnabled(mTargets->count() > 0);
    const std::string backend = backendPref();
    const json last = p && bake::isProject(*p) ? bake::lastResult(p->id()) : json();
    if (!mRunningBackend.empty())
    {
        mBackend->setText(backendBadge(mRunningBackend));
        mBackend->setToolTip("Baking on the " + qs(mRunningBackend) + " backend; click to change where the next bake runs");
    }
    else if (last.is_object() && last.contains("backendMode"))
    {
        const std::string mode = last.value("backendMode", std::string("cpu"));
        mBackend->setText(modeBadge(mode));
        mBackend->setToolTip(QString("The last bake ran on the %1 backend (%2); the next runs on %3. Click to change.")
                                 .arg(qs(last.value("backend", std::string("cpu"))))
                                 .arg(mode == "rt" ? "ray query" : mode == "compute" ? "compute traversal" : "CPU")
                                 .arg(qs(backend)));
    }
    else
    {
        mBackend->setText(backendBadge(backend));
        mBackend->setToolTip("Bakes run on the " + qs(backend) + " backend; click to change");
    }
    mLintButton->setEnabled(has && ops::find("bakeLint"));
    mLintButton->setToolTip(ops::find("bakeLint") ? "Check the project for problems (bakeLint)" : "Lint is not available in this build");
}
} // namespace lila::ui
