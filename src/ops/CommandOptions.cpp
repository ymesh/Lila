#include "ops/CommandOptions.h"

#include "core/PrefStore.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace lila::ops::options
{
namespace
{
using O = Override;

void registerBuiltinOverrides(std::map<std::string, Override, std::less<>>& m)
{
    O transfer;
    transfer.order = {"uvs", "normals", "colors", "positions", "skinWeights", "attributes", "sampleSpace", "keepShape", "separateUnmatched", "searchMethod", "maxDistance",
                      "flip", "sourceUvSet", "targetUvSet", "sourceColorSet", "targetColorSet", "live"};
    transfer.shown = {"attributes"};
    for (const char* f : {"uvs", "normals", "colors", "positions", "skinWeights", "attributes"})
        transfer.sections[f] = "Attributes to Transfer";
    for (const char* f : {"sampleSpace", "keepShape", "separateUnmatched", "searchMethod", "maxDistance", "flip"})
        transfer.sections[f] = "Attribute Settings";
    for (const char* f : {"sourceUvSet", "targetUvSet"})
        transfer.sections[f] = "UV Set Mapping";
    for (const char* f : {"sourceColorSet", "targetColorSet"})
        transfer.sections[f] = "Color Set Mapping";
    transfer.sections["live"] = "Result";
    transfer.labels = {{"uvs", "UVs"}, {"colors", "Vertex Colors"}, {"positions", "Vertex Positions"}, {"normals", "Vertex Normals"},
                       {"attributes", "Other Layers"}, {"searchMethod", "Search Method"}, {"maxDistance", "Search Distance"},
                       {"flip", "Flip Across Symmetry"}, {"keepShape", "Keep Island Shape (Guided)"},
                       {"separateUnmatched", "Separate Unmatched Faces (Guided)"}, {"live", "Live (Modifier, No Bake)"}};
    transfer.choices["sampleSpace"] = {{"world", "World"}, {"object", "Local (Object)"}, {"component", "Component (Same Topology)"}, {"uv", "UV (Closest UV)"},
                                         {"guided", "Guided (Similar Shapes)"}};
    transfer.choices["searchMethod"] = {{"closest", "Closest Point on Surface"}, {"normal", "Closest Along Normal (Raycast)"}};
    transfer.choices["flip"] = {{"none", "Off"}, {"x", "X"}, {"y", "Y"}, {"z", "Z"}};
    transfer.ranges["maxDistance"] = {-1.0, 1e6, 0.1, " cm"};
    transfer.ranges["sourceUvSet"] = {-1.0, 7.0, 1.0, ""};
    transfer.ranges["targetUvSet"] = {-1.0, 7.0, 1.0, ""};
    m["transferAttributes"] = std::move(transfer);

    O remesh;
    remesh.order = {"symmetry", "method"};
    remesh.choices["symmetry"] = {{"off", "Off"}, {"x", "X"}, {"y", "Y"}, {"z", "Z"}};
    remesh.choices["method"] = {{"patches", "Patches"}, {"grid", "Grid (Flat Sheets)"}};
    remesh.choiceDocs["method"] = {{"patches", "Loops run unbroken between a few poles. For anything that deforms, gets baked or edited by hand."},
                                   {"grid", "A straight grid seen down the sheet's normal, for terrain tiles and panels. Closed shapes get Patches."}};
    for (const char* f : {"targetFaces", "countTolerance", "targetEdgeLength", "adaptivity", "jointDensity", "jointDensityReach", "edgesAround"})
        remesh.sections[f] = "Density";
    for (const char* f : {"useHardEdges", "sharpAngle", "jointPoleAvoidance", "pureQuads", "relaxIterations"})
        remesh.sections[f] = "Edge Flow";
    for (const char* f : {"guides", "featureGuides"})
        remesh.sections[f] = "Guides";
    for (const char* f : {"transferUvs", "transferNormals", "keepSource"})
        remesh.sections[f] = "Output";
    remesh.labels = {{"transferUvs", "Transfer UVs"}, {"guides", "Flow Curves"}, {"featureGuides", "Edge Loop Curves"}};
    remesh.notes["guides"] = [] { return std::string("Draw with Curves > Pencil Curve"); };
    remesh.ranges["sharpAngle"] = {0.0, 180.0, 1.0, " deg"};
    remesh.ranges["adaptivity"] = {0.0, 1.0, 0.05, ""};
    remesh.ranges["countTolerance"] = {0.0, 1.0, 0.01, ""};
    remesh.labels["countTolerance"] = "Count Tolerance";
    remesh.labels["edgesAround"] = "Edges Around Thin Parts";
    remesh.ranges["jointDensity"] = {0.0, 1.0, 0.05, ""};
    remesh.ranges["jointDensityReach"] = {1.0, 12.0, 0.5, ""};
    remesh.ranges["jointPoleAvoidance"] = {0.0, 100.0, 1.0, ""};
    m["quadRemesh"] = std::move(remesh);

    O bevel;
    bevel.ranges["profile"] = {0.0, 1.0, 0.05, ""};
    bevel.ranges["smoothingAngle"] = {0.0, 180.0, 1.0, " deg"};
    bevel.ranges["segments"] = {1.0, 64.0, 1.0, ""};
    m["polyBevel"] = std::move(bevel);

    O mirror;
    mirror.choices["axis"] = {{"x", "X"}, {"y", "Y"}, {"z", "Z"}};
    mirror.choices["direction"] = {{"positive", "+ to -"}, {"negative", "- to +"}, {"both", "Both"}};
    mirror.choices["space"] = {{"object", "Object"}, {"world", "World"}};
    mirror.order = {"axis", "direction", "space", "weld", "tolerance"};
    mirror.labels = {{"axis", "Mirror Axis"}, {"direction", "Mirror Direction"}, {"space", "Mirror Axis Position"}, {"tolerance", "Merge Threshold"}};
    mirror.ranges["tolerance"] = {0.0, 10.0, 0.001, " cm"};
    m["polyMirror"] = std::move(mirror);

    O quads;
    quads.ranges["maxAngle"] = {0.0, 180.0, 1.0, " deg"};
    m["polyQuadrangulate"] = std::move(quads);

    O relax;
    relax.ranges["strength"] = {0.0, 1.0, 0.05, ""};
    relax.ranges["iterations"] = {1.0, 1000.0, 1.0, ""};
    m["relaxMesh"] = std::move(relax);

    O subdivide;
    subdivide.ranges["levels"] = {1.0, 6.0, 1.0, ""};
    m["polySubdivide"] = std::move(subdivide);

    O repair;
    repair.choices["mode"] = {{"select", "Select Matching Components"}, {"repair", "Repair Matching Components"}};
    repair.order = {"mode",          "coincident",       "zeroLengthEdges", "zeroAreaFaces", "flippedNormals", "lamina",    "nonManifoldEdges",
                    "nonManifoldVertices", "holes",      "triangles",       "ngons",           "concave",       "zeroAreaUvs",    "tolerance", "removeLoose"};
    for (const char* f : {"coincident", "zeroLengthEdges", "zeroAreaFaces", "flippedNormals"})
        repair.sections[f] = "Geometry";
    for (const char* f : {"lamina", "nonManifoldEdges", "nonManifoldVertices", "holes", "triangles", "ngons", "concave"})
        repair.sections[f] = "Topology";
    repair.sections["zeroAreaUvs"] = "UVs";
    repair.sections["tolerance"] = "Tolerance";
    repair.sections["removeLoose"] = "Tolerance";
    repair.labels = {{"coincident", "Coincident Vertices"},
                     {"lamina", "Lamina Faces"},
                     {"nonManifoldEdges", "Non-Manifold Edges"},
                     {"nonManifoldVertices", "Non-Manifold Vertices"},
                     {"holes", "Holes (Border Edges)"},
                     {"triangles", "Triangles"},
                     {"ngons", "Faces With More Than 4 Sides"},
                     {"concave", "Concave Faces"},
                     {"zeroLengthEdges", "Zero-Length Edges"},
                     {"zeroAreaFaces", "Zero-Area Faces"},
                     {"zeroAreaUvs", "Zero-Area UV Faces"},
                     {"flippedNormals", "Flipped Normals"},
                     {"removeLoose", "Remove Loose Vertices and Edges"}};
    repair.ranges["tolerance"] = {-1.0, 100.0, 0.001, " cm"};
    m["meshRepair"] = std::move(repair);

    O voxel;
    voxel.ranges["resolution"] = {8.0, 1024.0, 8.0, ""};
    m["voxelRemesh"] = std::move(voxel);
}

std::map<std::string, Override, std::less<>>& overrides()
{
    static std::map<std::string, Override, std::less<>> m = [] {
        std::map<std::string, Override, std::less<>> init;
        registerBuiltinOverrides(init);
        return init;
    }();
    return m;
}

bool isWordChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '+';
}

std::string trim(std::string_view s)
{
    size_t a = s.find_first_not_of(" \t\n");
    if (a == std::string_view::npos)
        return {};
    size_t b = s.find_last_not_of(" \t\n");
    return std::string(s.substr(a, b - a + 1));
}

bool contains(const std::vector<std::string>& v, std::string_view s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

json kindZero(const Field& f)
{
    switch (f.kind)
    {
    case FieldKind::Check: return false;
    case FieldKind::Int: return 0;
    case FieldKind::Float: return 0.0;
    case FieldKind::Choice: return f.choices.empty() ? json("") : json(f.choices.front());
    case FieldKind::Text: return "";
    case FieldKind::Vec3: return json::array({0.0, 0.0, 0.0});
    case FieldKind::List: return json::array();
    }
    return nullptr;
}

/** The value as the field's kind would hold it, so 1 and 1.0 or true and 1 compare equal; null when it can't be. */
json normalize(const Field& f, const json& v)
{
    switch (f.kind)
    {
    case FieldKind::Check:
        if (v.is_boolean())
            return v;
        return v.is_number() ? json(v.get<double>() != 0.0) : json(nullptr);
    case FieldKind::Int: return v.is_number() ? json(int64_t(std::llround(v.get<double>()))) : json(nullptr);
    case FieldKind::Float: return v.is_number() ? json(v.get<double>()) : json(nullptr);
    case FieldKind::Choice: return v.is_string() && contains(f.choices, v.get<std::string>()) ? v : json(nullptr);
    case FieldKind::Text: return v.is_string() ? v : json(nullptr);
    case FieldKind::Vec3:
        if (v.is_array() && v.size() == 3 && std::all_of(v.begin(), v.end(), [](const json& x) { return x.is_number(); }))
            return json::array({v[0].get<double>(), v[1].get<double>(), v[2].get<double>()});
        return nullptr;
    case FieldKind::List: return v.is_array() ? v : json(nullptr);
    }
    return nullptr;
}

std::string prefKey(std::string_view command)
{
    return "/" + std::string(command);
}

bool callSets(const json& call, const FlagDef& f)
{
    return call.is_object() && ((call.contains(f.name) && !call[f.name].is_null()) || (!f.shortName.empty() && call.contains(f.shortName)));
}
} // namespace

void setOverride(const std::string& command, Override o)
{
    overrides()[command] = std::move(o);
}

const Override* findOverride(std::string_view command)
{
    auto it = overrides().find(command);
    return it == overrides().end() ? nullptr : &it->second;
}

std::string prettyLabel(std::string_view camel)
{
    std::vector<std::string> words;
    for (size_t i = 0; i < camel.size(); ++i)
    {
        char c = camel[i];
        bool upper = std::isupper(static_cast<unsigned char>(c));
        bool digitStart = std::isdigit(static_cast<unsigned char>(c)) && i > 0 && !std::isdigit(static_cast<unsigned char>(camel[i - 1]));
        if (words.empty() || c == '_' || upper || digitStart)
            words.emplace_back();
        if (c != '_')
            words.back() += c;
    }
    std::string out;
    for (std::string& w : words)
    {
        if (w.empty())
            continue;
        std::string lower = w;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        if (lower == "uv" || lower == "uvs")
            w = lower == "uv" ? "UV" : "UVs";
        else if (lower.size() == 1 && std::isalpha(static_cast<unsigned char>(lower[0])))
            w = std::string(1, char(std::toupper(static_cast<unsigned char>(lower[0]))));
        else
            w[0] = char(std::toupper(static_cast<unsigned char>(w[0])));
        if (!out.empty())
            out += ' ';
        out += w;
    }
    return out;
}

std::vector<std::string> choicesFromDoc(std::string_view doc, const json& defaultValue)
{
    std::string_view head = doc.substr(0, std::min({doc.find('.'), doc.find(';'), doc.find('('), doc.find(':')}));
    std::string s(head);
    for (const char* sep : {" or ", " | ", "|", "/"})
        for (size_t at = s.find(sep); at != std::string::npos; at = s.find(sep))
            s.replace(at, std::string_view(sep).size(), ",");
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size())
    {
        size_t end = s.find(',', start);
        std::string word = trim(std::string_view(s).substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (word.empty() || !std::all_of(word.begin(), word.end(), isWordChar))
            return {};
        if (!contains(out, word))
            out.push_back(word);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    if (out.size() < 2)
        return {};
    if (defaultValue.is_string() && !defaultValue.get<std::string>().empty() && !contains(out, defaultValue.get<std::string>()))
        return {};
    return out;
}

namespace
{
bool alnumAt(std::string_view s, size_t i)
{
    return i < s.size() && std::isalnum(static_cast<unsigned char>(s[i]));
}

/** From start to the end of its clause: a sentence's full stop or a semicolon, outside parentheses. */
std::string clauseFrom(std::string_view doc, size_t start)
{
    int depth = 0;
    size_t i = start;
    for (; i < doc.size(); ++i)
    {
        char c = doc[i];
        if (c == '(')
            ++depth;
        else if (c == ')')
            depth = std::max(0, depth - 1);
        else if (depth == 0 && (c == ';' || (c == '.' && (i + 1 == doc.size() || doc[i + 1] == ' '))))
            break;
    }
    return trim(doc.substr(start, i - start));
}

std::string sentence(std::string s)
{
    if (s.empty())
        return s;
    s[0] = char(std::toupper(static_cast<unsigned char>(s[0])));
    if (s.back() != '.' && s.back() != '!' && s.back() != '?' && s.back() != ')')
        s += '.';
    return s;
}

std::string docOfChoice(std::string_view doc, std::string_view lower, const std::string& choice)
{
    std::string c(choice);
    std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    if (c.empty())
        return {};
    for (size_t at = lower.find(c); at != std::string_view::npos; at = lower.find(c, at + 1))
    {
        const size_t end = at + c.size();
        if ((at > 0 && alnumAt(c, 0) && alnumAt(lower, at - 1)) || (alnumAt(c, c.size() - 1) && alnumAt(lower, end)))
            continue;
        if (end < doc.size() && doc[end] == ':')
            if (std::string d = clauseFrom(doc, end + 1); !d.empty())
                return sentence(d);
        if (doc.substr(end, 2) == " (")
        {
            size_t close = doc.find(')', end + 2);
            if (close != std::string_view::npos && close > end + 2)
                return sentence(std::string(doc.substr(end + 2, close - end - 2)));
        }
        // A clause that opens with the choice and goes on to describe it: "linear steps each copy by offset".
        size_t before = doc.find_last_not_of(' ', at == 0 ? std::string_view::npos : at - 1);
        const bool opens = at == 0 || before == std::string_view::npos || std::string_view(".;:").find(doc[before]) != std::string_view::npos;
        if (opens && end < doc.size() && doc[end] == ' ' && alnumAt(doc, end + 1))
            return sentence(clauseFrom(doc, at));
    }
    return {};
}
} // namespace

std::vector<std::string> choiceDocs(std::string_view doc, const std::vector<std::string>& choices)
{
    std::string lower(doc);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    std::vector<std::string> out;
    out.reserve(choices.size());
    for (const std::string& c : choices)
        out.push_back(docOfChoice(doc, lower, c));
    return out;
}

Field fieldFor(const FlagDef& flag)
{
    Field f;
    f.flag = flag.name;
    f.label = prettyLabel(flag.name);
    f.doc = flag.doc;
    switch (flag.type)
    {
    case ArgType::Bool: f.kind = FieldKind::Check; break;
    case ArgType::Int: f.kind = FieldKind::Int; break;
    case ArgType::Float: f.kind = FieldKind::Float; break;
    case ArgType::Vec3: f.kind = FieldKind::Vec3; break;
    case ArgType::List: f.kind = FieldKind::List; break;
    case ArgType::String:
    case ArgType::Any:
        f.choices = !flag.choices.empty() ? flag.choices : choicesFromDoc(flag.doc, flag.defaultValue);
        f.kind = f.choices.empty() ? FieldKind::Text : FieldKind::Choice;
        break;
    }
    for (const std::string& c : f.choices)
        f.choiceLabels.push_back(prettyLabel(c));
    f.choiceDocs = choiceDocs(flag.doc, f.choices);
    if (f.kind == FieldKind::Text || f.kind == FieldKind::List)
        f.nodeType = flag.nodeType;
    if (f.kind == FieldKind::Text && flag.name == "uvSet")
        f.editor = "uvSet";
    f.min = flag.min.value_or(f.min);
    f.max = flag.max.value_or(f.max);
    if (f.kind == FieldKind::Int || f.kind == FieldKind::Float)
        f.presets = flag.presets;
    const json& given = flag.defaultValue.is_null() ? flag.shownDefault : flag.defaultValue;
    json def = given.is_null() ? json(nullptr) : normalize(f, given);
    f.defaultValue = def.is_null() ? kindZero(f) : def;
    return f;
}

std::vector<Field> fields(std::string_view command, const json& fixed)
{
    std::vector<Field> out;
    const CommandDef* def = find(command);
    if (!def)
        return out;
    const Override* o = findOverride(command);
    for (const FlagDef& flag : def->flags)
    {
        if (callSets(fixed, flag))
            continue;
        bool forced = o && contains(o->shown, flag.name);
        bool overriddenChoices = o && o->choices.contains(flag.name);
        if (o && contains(o->hidden, flag.name))
            continue;
        if (!forced && (flag.name == "name" || (flag.type == ArgType::List && flag.nodeType.empty())))
            continue;
        if (!forced && flag.type == ArgType::Any && !overriddenChoices && choicesFromDoc(flag.doc, flag.defaultValue).empty())
            continue;
        Field f = fieldFor(flag);
        if (o)
        {
            if (auto it = o->choices.find(flag.name); it != o->choices.end())
            {
                f.kind = FieldKind::Choice;
                f.choices.clear();
                f.choiceLabels.clear();
                for (const auto& [value, label] : it->second)
                {
                    f.choices.push_back(value);
                    f.choiceLabels.push_back(label);
                }
                f.choiceDocs = choiceDocs(flag.doc, f.choices);
                json d = normalize(f, flag.defaultValue.is_null() ? flag.shownDefault : flag.defaultValue);
                f.defaultValue = d.is_null() ? kindZero(f) : d;
            }
            if (auto it = o->choiceDocs.find(flag.name); it != o->choiceDocs.end())
                for (size_t i = 0; i < f.choices.size(); ++i)
                    if (auto d = it->second.find(f.choices[i]); d != it->second.end())
                        f.choiceDocs[i] = d->second;
            if (auto it = o->labels.find(flag.name); it != o->labels.end())
                f.label = it->second;
            if (auto it = o->sections.find(flag.name); it != o->sections.end())
                f.section = it->second;
            if (auto it = o->ranges.find(flag.name); it != o->ranges.end())
            {
                f.min = it->second.min;
                f.max = it->second.max;
                f.step = it->second.step;
                f.suffix = it->second.suffix;
            }
            if (auto it = o->editors.find(flag.name); it != o->editors.end() && (f.kind == FieldKind::Text || f.kind == FieldKind::Choice))
                f.editor = it->second;
            if (auto it = o->presets.find(flag.name); it != o->presets.end())
                f.presets = it->second;
        }
        out.push_back(std::move(f));
    }
    if (o && !o->order.empty())
    {
        auto rank = [&](const Field& f) {
            auto it = std::find(o->order.begin(), o->order.end(), f.flag);
            return it == o->order.end() ? o->order.size() : size_t(it - o->order.begin());
        };
        std::stable_sort(out.begin(), out.end(), [&](const Field& a, const Field& b) { return rank(a) < rank(b); });
    }
    return out;
}

bool hasOptions(std::string_view command, const json& fixed)
{
    return !fields(command, fixed).empty();
}

json saved(std::string_view command)
{
    json v = PrefStore::get().value(kPrefFile, prefKey(command), json::object());
    return v.is_object() ? v : json::object();
}

json values(std::string_view command, const json& fixed)
{
    json stored = saved(command);
    json out = json::object();
    for (const Field& f : fields(command, fixed))
    {
        json v = stored.contains(f.flag) ? normalize(f, stored[f.flag]) : json(nullptr);
        out[f.flag] = v.is_null() ? f.defaultValue : v;
    }
    return out;
}

void setValue(std::string_view command, const std::string& flag, const json& value)
{
    std::vector<Field> all = fields(command);
    auto it = std::find_if(all.begin(), all.end(), [&](const Field& f) { return f.flag == flag; });
    if (it == all.end())
        return;
    json v = normalize(*it, value);
    json stored = saved(command);
    if (v.is_null() || v == it->defaultValue)
        stored.erase(flag);
    else
        stored[flag] = v;
    PrefStore& p = PrefStore::get();
    if (stored.empty())
        p.remove(kPrefFile, prefKey(command));
    else
        p.set(kPrefFile, prefKey(command), stored);
}

void reset(std::string_view command)
{
    PrefStore::get().remove(kPrefFile, prefKey(command));
}

json callWith(std::string_view command, const json& call)
{
    json out = call.is_object() ? call : json::object();
    const CommandDef* def = find(command);
    if (!def)
        return out;
    json stored = saved(command);
    if (stored.empty())
        return out;
    for (const Field& f : fields(command, out))
        if (stored.contains(f.flag))
            if (json v = normalize(f, stored[f.flag]); !v.is_null() && v != f.defaultValue)
                out[f.flag] = v;
    return out;
}
} // namespace lila::ops::options
