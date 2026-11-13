#include "ui/ThemeModel.h"

#include "core/Log.h"
#include "ops/Command.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>

namespace lila::ui::themes
{
namespace
{
using Pairs = std::initializer_list<std::pair<const char*, const char*>>;

const std::vector<TokenInfo> kTokens = {
    // The neutral ramp: shade() interpolates between these by their Monotone level.
    {"darkest", "Darkest", "Interface", 0},
    {"shadow", "Shadow", "Interface", 18},
    {"tooltip", "Tooltip", "Interface", 28},
    {"base", "Base (lists, fields)", "Interface", 36},
    {"altBase", "Alternate base", "Interface", 42},
    {"window", "Window", "Interface", 48},
    {"button", "Button", "Interface", 58},
    {"controlHover", "Control hover", "Interface", 70},
    {"border", "Border", "Interface", 85},
    {"controlChecked", "Control checked", "Interface", 90},
    {"disabled", "Disabled text", "Interface", 110},
    {"dimText", "Dim text", "Interface", 150},
    {"text", "Text", "Interface", 212},
    {"brightText", "Bright text", "Interface", 255},
    {"panel", "Panel title", "Interface"},
    {"chrome", "Chrome (bars, gaps between panels)", "Interface"},
    {"pressed", "Pressed", "Interface"},
    {"sliderFill", "Slider fill", "Interface"},
    {"rule", "Section rule", "Interface"},
    {"input", "Input field", "Interface"},
    {"menu", "Menu", "Interface"},
    {"scrollbar", "Scroll bar", "Interface"},
    {"separator", "Separator", "Interface"},
    {"hover", "Hover", "Interface"},
    {"statusBar", "Status bar", "Interface"},
    {"checked", "Checked button", "Interface"},
    {"checkedText", "Checked text (buttons, list rows)", "Interface"},
    {"highlight", "Selection", "Interface"},
    {"highlightText", "Selected text", "Interface"},
    {"accent", "Accent (links, active)", "Interface"},
    {"tooltipText", "Tooltip text", "Interface"},
    {"statusWarning", "Status warning", "Interface"},
    {"statusError", "Status error", "Interface"},
    {"issueInfo", "Issue info", "Interface"},
    {"issueWarning", "Issue warning", "Interface"},
    {"issueError", "Issue error", "Interface"},
    {"iconNormal", "Icon", "Icons"},
    {"iconActive", "Icon active", "Icons"},
    {"iconDisabled", "Icon disabled", "Icons"},
    {"viewportBackground", "Background", "Viewport"},
    {"grid", "Grid", "Viewport"},
    {"gridAxisX", "Grid X axis", "Viewport"},
    {"gridAxisZ", "Grid Z axis (Y when Z-up)", "Viewport"},
    {"wireframe", "Wireframe", "Viewport"},
    {"wireSelected", "Selected wireframe", "Viewport"},
    {"wireSelectedOther", "Selected wireframe, secondary", "Viewport"},
    {"wirePreselect", "Preselected wireframe", "Viewport"},
    {"wireUnderSelected", "Child of selected", "Viewport"},
    {"curve", "Curves", "Viewport"},
    {"componentWire", "Component mode wire", "Viewport"},
    {"componentPoint", "Vertices", "Viewport"},
    {"componentSelected", "Selected components", "Viewport"},
    {"componentHover", "Preselected components", "Viewport"},
    {"axisX", "X axis", "Viewport"},
    {"axisY", "Y axis", "Viewport"},
    {"axisZ", "Z axis", "Viewport"},
    {"manipNeutral", "Manipulator", "Viewport"},
    {"manipActive", "Manipulator active", "Viewport"},
    {"hudText", "HUD text", "Viewport"},
    {"keyTick", "Key ticks", "Viewport"},
    {"nodeBody", "Node body", "Node Editor"},
    {"nodeHeader", "Node header", "Node Editor"},
    {"nodeSelected", "Selected node", "Node Editor"},
    {"nodeGroupIo", "Group input/output", "Node Editor"},
    {"nodeGeometry", "Geometry nodes", "Node Editor"},
    {"nodeMesh", "Mesh nodes", "Node Editor"},
    {"nodeCurve", "Curve nodes", "Node Editor"},
    {"nodePoint", "Point nodes", "Node Editor"},
    {"nodeInstances", "Instance nodes", "Node Editor"},
    {"nodeAttribute", "Attribute nodes", "Node Editor"},
    {"nodeInput", "Input nodes", "Node Editor"},
    {"nodeOutput", "Output nodes", "Node Editor"},
    {"nodeUtilities", "Utility nodes", "Node Editor"},
    {"nodeColor", "Colour nodes", "Node Editor"},
    {"nodeTexture", "Texture nodes", "Node Editor"},
    {"socketGeometry", "Geometry socket", "Node Editor"},
    {"socketFloat", "Float socket", "Node Editor"},
    {"socketInt", "Integer socket", "Node Editor"},
    {"socketBool", "Boolean socket", "Node Editor"},
    {"socketString", "String socket", "Node Editor"},
    {"socketVector", "Vector socket", "Node Editor"},
    {"socketRotation", "Rotation socket", "Node Editor"},
    {"socketColor", "Colour socket", "Node Editor"},
    {"socketMatrix", "Matrix socket", "Node Editor"},
    {"socketBundle", "Bundle socket", "Node Editor"},
    {"socketClosure", "Closure socket", "Node Editor"},
    {"zoneRepeat", "Repeat zone", "Node Editor"},
    {"zoneForEach", "For Each zone", "Node Editor"},
    {"zoneSimulation", "Simulation zone", "Node Editor"},
    {"zoneClosure", "Closure zone", "Node Editor"},
};

// The levels in kTokens are the greys widget code was written with; these colours are where those greys land (see
// shade), so fields sit one step above the panel and selection is the brightest fill.
const Pairs kMonotone = {
    {"darkest", "#000000"},       {"shadow", "#141414"},        {"tooltip", "#1f1f1f"},        {"base", "#333333"},
    {"altBase", "#2c2c2c"},       {"window", "#282828"},        {"button", "#333333"},         {"controlHover", "#3d3d3d"},
    {"border", "#3a3a3a"},        {"controlChecked", "#5c5c5c"}, {"disabled", "#666666"},      {"dimText", "#8c8c8c"},
    {"text", "#d6d6d6"},          {"brightText", "#f4f4f4"},
    {"panel", "#282828"},         {"input", "#333333"},         {"menu", "#202020"},           {"scrollbar", "#3a3a3a"},
    {"chrome", "#1b1b1b"},        {"pressed", "#474747"},       {"sliderFill", "#4a4a4a"},     {"rule", "#333333"},
    {"separator", "#1b1b1b"},     {"hover", "#3d3d3d"},         {"statusBar", "#1b1b1b"},      {"checked", "#5c5c5c"},
    {"highlight", "#5c5c5c"},     {"highlightText", "#ffffff"}, {"checkedText", "#ffffff"}, {"accent", "#ebebeb"},         {"tooltipText", "#dcdcdc"},
    {"statusWarning", "#f2f2f2"}, {"statusError", "#f2f2f2"},
    // Hued even here: an outliner badge has to say warning or error at a glance.
    {"issueInfo", "#5ea4e8"},     {"issueWarning", "#f2c14e"},  {"issueError", "#e5604b"},
    {"iconNormal", "#c8c8c8"},    {"iconActive", "#f2f2f2"},    {"iconDisabled", "#6e6e6e"},   {"viewportBackground", "#4d4d4d"},
    {"grid", "#adadad"},          {"gridAxisX", "#000000"},     {"gridAxisZ", "#000000"},      {"wireframe", "#272727"},
    {"wireSelected", "#f6f6f6"},  {"wireSelectedOther", "#d4d4d4"}, {"wirePreselect", "#e1e1e1"}, {"wireUnderSelected", "#c4c4c4"}, {"curve", "#a8a8a8"},
    {"componentWire", "#73c7ff"}, {"componentPoint", "#f26bc7"}, {"componentSelected", "#fff226"}, {"componentHover", "#ff2e2e"},
    {"axisX", "#d96666"},         {"axisY", "#80cc6b"},         {"axisZ", "#6b8fe6"},          {"manipNeutral", "#b3b3b3"},
    {"manipActive", "#ffffff"},   {"hudText", "#ffffff"},       {"keyTick", "#db5c5c"},        {"nodeBody", "#3a3a3a"},
    {"nodeHeader", "#4c4c4c"},    {"nodeSelected", "#ebebeb"},
    // Node headers: muted tints at one lightness (OKLCH 0.40, chroma 0.055), one hue per family of categories.
    {"nodeGroupIo", "#533f5c"},   {"nodeGeometry", "#235146"},  {"nodeMesh", "#235146"},       {"nodeCurve", "#235146"},
    {"nodePoint", "#235146"},     {"nodeInstances", "#235146"}, {"nodeAttribute", "#3c4666"},  {"nodeInput", "#623c39"},
    {"nodeOutput", "#5e3b4c"},    {"nodeUtilities", "#274c61"}, {"nodeColor", "#574423"},      {"nodeTexture", "#574423"},
    // Socket and wire colour says the data type.
    {"socketGeometry", "#00d6a3"}, {"socketFloat", "#a1a1a1"},  {"socketInt", "#598c5c"},      {"socketBool", "#cca6d6"},
    {"socketString", "#70b2ff"},  {"socketVector", "#6363c7"},  {"socketRotation", "#a663c7"}, {"socketColor", "#c7c729"},
    {"socketMatrix", "#7a4ad6"},  {"socketBundle", "#3fa59a"},  {"socketClosure", "#d6604a"},
    {"zoneRepeat", "#63506b"},    {"zoneForEach", "#386156"},   {"zoneSimulation", "#675537"}, {"zoneClosure", "#724d4a"},
};

// Monotone's neutrals with hue where it helps scanning: selection, active state, axes and node categories.
const Pairs kColour = {
    {"wireSelectedOther", "#f6f6f6"},
    {"highlight", "#30507c"},     {"accent", "#5b9cf5"},          {"checked", "#30507c"},          {"sliderFill", "#3c4c63"},
    {"iconActive", "#ffffff"},    {"viewportBackground", "#4a4e54"}, {"gridAxisX", "#8a3434"},     {"gridAxisZ", "#34508a"},
    {"wireSelected", "#5ce6a5"},  {"wirePreselect", "#f2d16b"},   {"wireUnderSelected", "#3fa07a"}, {"axisX", "#e5534b"},
    {"axisY", "#6cc24a"},         {"axisZ", "#4a8fe5"},           {"manipActive", "#ffd84a"},      {"keyTick", "#e5534b"},
    {"nodeSelected", "#f0a33a"},  {"nodeGroupIo", "#4a3a6a"},     {"nodeGeometry", "#1f5f52"},     {"nodeMesh", "#22584a"},
    {"nodeCurve", "#2d5a3a"},     {"nodePoint", "#3a5a2a"},       {"nodeInstances", "#2f4f6a"},    {"nodeAttribute", "#3a3a7a"},
    {"nodeInput", "#7a2f45"},     {"nodeOutput", "#5a2a3a"},      {"nodeUtilities", "#2a5873"},    {"nodeColor", "#6a6420"},
    {"nodeTexture", "#7a4a20"},   {"socketGeometry", "#00d6a3"},  {"socketFloat", "#a1a1a1"},      {"socketInt", "#598c5c"},
    {"socketBool", "#cca6d6"},    {"socketString", "#70b2ff"},    {"socketVector", "#6363c7"},     {"socketRotation", "#a663c7"},
    {"socketColor", "#c7c729"},   {"statusWarning", "#f2d16b"},    {"statusError", "#f2655c"},
    {"socketMatrix", "#7a4ad6"},  {"socketBundle", "#3fa59a"},     {"socketClosure", "#d6604a"},
    {"zoneRepeat", "#6a4a8a"},    {"zoneForEach", "#2f6a5a"},      {"zoneSimulation", "#7a5a2a"},   {"zoneClosure", "#7a3a3a"},
};

// Sampled from the Lila icon: navy ink, its sky and pale blues, and the icon's blue for selection.
const Pairs kLight = {
    {"wireSelectedOther", "#f6f6f6"},
    {"darkest", "#9fb2c8"},        {"shadow", "#b4c3d4"},         {"tooltip", "#fafdfe"},          {"base", "#ffffff"},
    {"altBase", "#f3f8fc"},        {"window", "#e9eef4"},         {"button", "#dbe3ec"},           {"border", "#c3cfdc"},
    {"controlHover", "#cfd9e5"},   {"controlChecked", "#a9c9f0"}, {"chrome", "#c9d3de"},           {"pressed", "#c0ccda"},
    {"sliderFill", "#c4d6ec"},     {"rule", "#d3dce6"},
    {"disabled", "#8496ab"},       {"dimText", "#4a5f7a"},        {"text", "#05215b"},             {"brightText", "#020f2e"},
    {"panel", "#e9eef4"},          {"input", "#ffffff"},          {"menu", "#fafdfe"},             {"scrollbar", "#c4d4e5"},
    {"separator", "#c9d3de"},      {"hover", "#cfd9e5"},          {"statusBar", "#c9d3de"},        {"checked", "#a9c9f0"},
    {"highlight", "#a9c9f0"},      {"highlightText", "#05215b"},  {"checkedText", "#05215b"},  {"accent", "#207ef4"},           {"tooltipText", "#05215b"},
    {"iconNormal", "#2a4470"},     {"iconActive", "#05215b"},     {"iconDisabled", "#9aaabd"},     {"viewportBackground", "#6b7787"},
    {"grid", "#c6d0dc"},           {"gridAxisX", "#8a3434"},      {"gridAxisZ", "#34508a"},        {"wireframe", "#1a2233"},
    {"wireSelected", "#5ce6a5"},   {"wirePreselect", "#f2d16b"},  {"wireUnderSelected", "#3fa07a"}, {"componentWire", "#73c7ff"},
    {"curve", "#e7eff7"},
    {"componentPoint", "#f26bc7"}, {"componentSelected", "#fff226"}, {"componentHover", "#ff2e2e"}, {"axisX", "#e5534b"},
    {"axisY", "#6cc24a"},          {"axisZ", "#4a8fe5"},          {"manipNeutral", "#c8d2de"},     {"manipActive", "#ffd84a"},
    {"hudText", "#ffffff"},        {"keyTick", "#d0362c"},        {"nodeBody", "#e7eff7"},         {"nodeHeader", "#b8cbe0"},
    {"nodeSelected", "#1767d0"},   {"nodeGroupIo", "#c9b8e0"},    {"nodeGeometry", "#a8dccf"},     {"nodeMesh", "#b0dcc4"},
    {"nodeCurve", "#bfe0b0"},      {"nodePoint", "#cfe0a8"},      {"nodeInstances", "#b0c8e6"},    {"nodeAttribute", "#c0c0ec"},
    {"nodeInput", "#ecb8c6"},      {"nodeOutput", "#e0b0c0"},     {"nodeUtilities", "#b0d4ec"},    {"nodeColor", "#e6e0a0"},
    {"nodeTexture", "#ecc8a0"},    {"socketGeometry", "#00a37c"}, {"socketFloat", "#6b6b6b"},      {"socketInt", "#3f7a43"},
    {"socketBool", "#9a6aa8"},     {"socketString", "#2f7fd6"},   {"socketVector", "#4a4ab0"},     {"socketRotation", "#8a4ab0"},
    {"socketColor", "#a39a1f"},    {"statusWarning", "#a36b00"},    {"statusError", "#c8261c"},
    {"issueInfo", "#2f6fb0"},     {"issueWarning", "#a36b00"},  {"issueError", "#c8261c"},
    {"socketMatrix", "#5a2ab0"},   {"socketBundle", "#1f8076"},     {"socketClosure", "#b0402a"},
    {"zoneRepeat", "#cbb8e6"},     {"zoneForEach", "#b0dccf"},      {"zoneSimulation", "#e6d0a8"},   {"zoneClosure", "#e6b8b0"},
};

// Deep navy with a cyan accent, for long sessions in a dark room.
const Pairs kMidnight = {
    {"darkest", "#05070c"},       {"shadow", "#0b0f18"},        {"tooltip", "#111725"},          {"base", "#252e44"},
    {"altBase", "#1f2738"},       {"window", "#1b2233"},        {"button", "#252e44"},           {"border", "#2c3854"},
    {"controlHover", "#2a3450"},  {"controlChecked", "#2b4870"}, {"chrome", "#10141f"},           {"pressed", "#2c3854"},
    {"sliderFill", "#2b3a58"},    {"rule", "#252e44"},
    {"disabled", "#5a6680"},      {"dimText", "#8a96b0"},       {"text", "#d3dbea"},             {"brightText", "#ffffff"},
    {"panel", "#1b2233"},         {"input", "#252e44"},         {"menu", "#141a28"},             {"scrollbar", "#2a3450"},
    {"separator", "#10141f"},     {"hover", "#2a3450"},         {"statusBar", "#10141f"},        {"checked", "#2b4870"},
    {"highlight", "#2b4870"},     {"highlightText", "#ffffff"}, {"accent", "#4fc3f7"},           {"tooltipText", "#d3dbea"},
    {"statusWarning", "#ffcc66"}, {"statusError", "#ff6b6b"},
    {"iconNormal", "#b8c4dc"},    {"iconActive", "#ffffff"},    {"iconDisabled", "#556078"},     {"viewportBackground", "#3a3f4a"},
    {"grid", "#7d8698"},          {"wireframe", "#10141f"},     {"wireSelected", "#6fe3ff"},     {"wirePreselect", "#ffd27a"},
    {"wireUnderSelected", "#3fa8c8"}, {"curve", "#9fb0d0"},     {"manipNeutral", "#a8b4cc"},     {"manipActive", "#ffd84a"},
    {"nodeBody", "#222b40"},      {"nodeHeader", "#2c3854"},    {"nodeSelected", "#4fc3f7"},
};

// Warm charcoal and amber: no blue in the chrome, easy on the eyes at night.
const Pairs kEmber = {
    {"darkest", "#0a0806"},       {"shadow", "#15110d"},        {"tooltip", "#1f1914"},          {"base", "#3b3128"},
    {"altBase", "#342b23"},       {"window", "#2f271f"},        {"button", "#3b3128"},           {"border", "#45392e"},
    {"controlHover", "#45392e"},  {"controlChecked", "#6a4a22"}, {"chrome", "#1a1510"},           {"pressed", "#4f4135"},
    {"sliderFill", "#5a4630"},    {"rule", "#3b3128"},
    {"disabled", "#6f6255"},      {"dimText", "#9c8e7e"},       {"text", "#e3d8c8"},             {"brightText", "#fff6e8"},
    {"panel", "#2f271f"},         {"input", "#3b3128"},         {"menu", "#241d17"},             {"scrollbar", "#3b3128"},
    {"separator", "#1a1510"},     {"hover", "#45392e"},         {"statusBar", "#1a1510"},        {"checked", "#6a4a22"},
    {"highlight", "#6a4a22"},     {"highlightText", "#ffffff"}, {"accent", "#f0a33a"},           {"tooltipText", "#e3d8c8"},
    {"statusWarning", "#f2c14e"}, {"statusError", "#e5604b"},
    {"iconNormal", "#d6c8b4"},    {"iconActive", "#ffd08a"},    {"iconDisabled", "#6f6255"},     {"viewportBackground", "#4b4845"},
    {"grid", "#a39c94"},          {"wireframe", "#221c16"},     {"wireSelected", "#ffd08a"},     {"wirePreselect", "#fff0c8"},
    {"wireUnderSelected", "#c79a5a"}, {"curve", "#b0a594"},     {"manipNeutral", "#b8ab9a"},     {"manipActive", "#ffd84a"},
    {"nodeBody", "#3b3128"},      {"nodeHeader", "#4a3e32"},    {"nodeSelected", "#f0a33a"},
};

// Warm off-white with dark ink and a terracotta accent, for bright rooms.
const Pairs kPaper = {
    {"darkest", "#b5ab9c"},       {"shadow", "#c8bfb1"},        {"tooltip", "#fffdf8"},          {"base", "#fffdf8"},
    {"altBase", "#f7f2e9"},       {"window", "#efe8dc"},        {"button", "#e2d9ca"},           {"border", "#d6cbb9"},
    {"controlHover", "#e2d9ca"},  {"controlChecked", "#f0cdb2"}, {"chrome", "#d8cebe"},           {"pressed", "#d6cbb9"},
    {"sliderFill", "#ecc9ad"},    {"rule", "#ddd3c4"},
    {"disabled", "#a0968a"},      {"dimText", "#6b6257"},       {"text", "#2b2620"},             {"brightText", "#14110d"},
    {"panel", "#efe8dc"},         {"input", "#fffdf8"},         {"menu", "#fffdf8"},             {"scrollbar", "#d8cebe"},
    {"separator", "#d8cebe"},     {"hover", "#e2d9ca"},         {"statusBar", "#d8cebe"},        {"checked", "#f0cdb2"},
    {"highlight", "#f0cdb2"},     {"highlightText", "#2b2620"}, {"checkedText", "#2b2620"}, {"accent", "#c0562f"},           {"tooltipText", "#2b2620"},
    {"statusWarning", "#9a6400"}, {"statusError", "#b3261e"},
    {"iconNormal", "#4a4238"},    {"iconActive", "#14110d"},    {"iconDisabled", "#b0a698"},     {"viewportBackground", "#74706a"},
    {"grid", "#c9c3ba"},          {"wireframe", "#1f1c18"},     {"curve", "#efe8dc"},            {"manipNeutral", "#d2cabe"},
    {"nodeBody", "#efe8dc"},      {"nodeHeader", "#ddd1bf"},    {"nodeSelected", "#c0562f"},
};

const Pairs kHighContrast = {
    {"wireSelectedOther", "#ffffff"},
    {"darkest", "#000000"},      {"shadow", "#000000"},        {"tooltip", "#000000"},       {"base", "#000000"},
    {"altBase", "#141414"},      {"window", "#0a0a0a"},        {"button", "#262626"},        {"border", "#c0c0c0"},
    {"controlHover", "#333333"}, {"controlChecked", "#5c4d00"}, {"chrome", "#000000"},        {"pressed", "#333333"},
    {"sliderFill", "#5c4d00"},   {"rule", "#5a5a5a"},
    {"disabled", "#8a8a8a"},     {"dimText", "#d0d0d0"},       {"text", "#ffffff"},          {"brightText", "#ffffff"},
    {"panel", "#1a1a1a"},        {"input", "#000000"},         {"menu", "#000000"},          {"scrollbar", "#3a3a3a"},
    {"separator", "#5a5a5a"},    {"hover", "#3d3d3d"},         {"statusBar", "#0a0a0a"},     {"checked", "#5c4d00"},
    {"highlight", "#ffd400"},    {"highlightText", "#000000"}, {"checkedText", "#ffffff"}, {"accent", "#ffd400"},        {"tooltipText", "#ffffff"},
    {"iconNormal", "#ffffff"},   {"iconActive", "#ffd400"},    {"iconDisabled", "#808080"},  {"wireSelected", "#ffd400"},
    {"wirePreselect", "#00e5ff"}, {"axisX", "#ff5c5c"},        {"axisY", "#5cff5c"},         {"axisZ", "#5c9dff"},
    {"manipActive", "#ffd400"},  {"keyTick", "#ff5c5c"},       {"nodeSelected", "#ffd400"},  {"nodeBody", "#1a1a1a"},
    {"nodeHeader", "#333333"},    {"statusWarning", "#ffd400"}, {"statusError", "#ff5c5c"},  {"curve", "#ffffff"},
};

Theme make(const char* name, const char* base, const char* description, Pairs colors)
{
    Theme t;
    t.name = name;
    t.author = "Lila";
    t.base = base;
    t.description = description;
    for (auto& [id, hex] : colors)
        t.colors[id] = *parseColor(hex);
    return t;
}

/** Colour's hues for axes, node categories and sockets, under a theme that sets only its own chrome. */
Theme withCategoryHues(Theme t)
{
    for (auto& [id, hex] : kColour)
    {
        std::string_view key = id;
        if ((key.starts_with("node") || key.starts_with("socket") || key.starts_with("zone") || key.starts_with("axis") || key.starts_with("gridAxis")) &&
            !t.colors.contains(key))
            t.colors[std::string(key)] = *parseColor(hex);
    }
    return t;
}

int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

double channelLinear(uint8_t v)
{
    double c = v / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

/** JSON pointer to a saved theme; names may hold '/' or '~'. */
std::string themeKey(std::string_view name)
{
    std::string key = std::string(kThemesPref) + "/";
    for (char c : name)
        key += c == '~' ? std::string("~0") : c == '/' ? std::string("~1") : std::string(1, c);
    return key;
}

json savedDocs()
{
    json all = PrefStore::get().value("general", kThemesPref, json::object());
    return all.is_object() ? all : json::object();
}

std::mutex gMutex;
std::shared_ptr<const Theme> gCurrent;
uint64_t gGeneration = 1;
bool gListening = false;

void listen()
{
    if (gListening)
        return;
    gListening = true;
    PrefStore::get().onChanged([](const std::string& file) {
        if (file == "general")
            invalidate();
    });
}

std::string requireName(const Args& a, const char* cmd)
{
    std::string name = a.get<std::string>("name", "");
    if (name.empty() && !a.positional.empty() && a.positional[0].is_string())
        name = a.positional[0].get<std::string>();
    if (name.empty())
        throw CommandError(std::string(cmd) + ": needs a theme name");
    return name;
}

std::string readFile(const std::string& path)
{
    std::ifstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    if (!f)
        throw CommandError("cannot read \"" + path + "\"");
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string joined(const std::vector<std::string>& lines)
{
    std::string out;
    for (const std::string& l : lines)
        out += (out.empty() ? "" : "; ") + l;
    return out;
}
} // namespace

std::optional<Rgba> parseColor(std::string_view s)
{
    if (s.empty() || s[0] != '#')
        return std::nullopt;
    s.remove_prefix(1);
    for (char c : s)
        if (hexDigit(c) < 0)
            return std::nullopt;
    auto byte = [&](size_t i) { return uint8_t(hexDigit(s[i]) * 16 + hexDigit(s[i + 1])); };
    if (s.size() == 3)
        return Rgba{uint8_t(hexDigit(s[0]) * 17), uint8_t(hexDigit(s[1]) * 17), uint8_t(hexDigit(s[2]) * 17), 255};
    if (s.size() == 6)
        return Rgba{byte(0), byte(2), byte(4), 255};
    if (s.size() == 8)
        return Rgba{byte(0), byte(2), byte(4), byte(6)};
    return std::nullopt;
}

std::string formatColor(Rgba c)
{
    char buf[16];
    if (c.a == 255)
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r, c.g, c.b);
    else
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
    return buf;
}

double luminance(Rgba c)
{
    return 0.2126 * channelLinear(c.r) + 0.7152 * channelLinear(c.g) + 0.0722 * channelLinear(c.b);
}

double contrast(Rgba a, Rgba b)
{
    double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

const std::vector<TokenInfo>& tokens()
{
    return kTokens;
}

const TokenInfo* token(std::string_view id)
{
    for (const TokenInfo& t : kTokens)
        if (id == t.id)
            return &t;
    return nullptr;
}

Rgba Theme::color(std::string_view id) const
{
    if (auto it = colors.find(id); it != colors.end())
        return it->second;
    const Theme& b = baseTheme(base);
    if (&b != this)
        if (auto it = b.colors.find(id); it != b.colors.end())
            return it->second;
    return {255, 0, 255, 255};
}

const std::vector<Theme>& builtIns()
{
    static const std::vector<Theme> themes = [] {
        std::vector<Theme> out;
        out.push_back(make("Monotone", "dark",
                           "Neutral grey, with colour only where it means something: axes, components, node types. Coloured interfaces bleed into your vision, affecting your colour perception, and reducing accuracy.",
                           kMonotone));
        out.push_back(make("Colour", "dark", "Dark, with colour for selection, active tools, axes and node categories. Friendlier and easier to scan.",
                           kColour));
        out.push_back(make("Light", "light", "Light blues and navy from the Lila icon.", kLight));
        out.push_back(make("High Contrast", "dark", "Black and white with yellow selection, for maximum legibility.", kHighContrast));
        out.push_back(withCategoryHues(make("Midnight", "dark", "Deep navy with a cyan accent, for long sessions in a dark room.", kMidnight)));
        out.push_back(withCategoryHues(
            make("Ember", "dark", "Warm charcoal and amber, with no blue in the interface. Easy on the eyes late at night.", kEmber)));
        out.push_back(make("Paper", "light", "Warm off-white with dark ink and a terracotta accent, for bright rooms.", kPaper));
        return out;
    }();
    return themes;
}

const Theme* builtIn(std::string_view name)
{
    for (const Theme& t : builtIns())
        if (t.name == name)
            return &t;
    return nullptr;
}

const Theme& baseTheme(std::string_view base)
{
    return builtIns()[base == "light" ? 2 : 0];
}

ParseResult parse(const json& doc)
{
    ParseResult r;
    if (!doc.is_object())
    {
        r.errors.push_back("a theme is a JSON object");
        return r;
    }
    Theme t;
    auto str = [&](const char* key, std::string& out, bool required) {
        auto it = doc.find(key);
        if (it == doc.end() || it->is_null())
        {
            if (required)
                r.errors.push_back(std::string("missing \"") + key + "\"");
            return;
        }
        if (!it->is_string())
            r.errors.push_back(std::string("\"") + key + "\" must be a string");
        else
            out = it->get<std::string>();
    };
    str("name", t.name, true);
    str("author", t.author, false);
    str("base", t.base, false);
    if (doc.contains("name") && doc["name"].is_string() && t.name.empty())
        r.errors.push_back("\"name\" is empty");
    if (t.base != "dark" && t.base != "light")
        r.errors.push_back("\"base\" must be \"dark\" or \"light\", not \"" + t.base + "\"");
    if (auto v = doc.find("lilaTheme"); v != doc.end() && !(v->is_number_integer() && v->get<int>() == 1))
        r.warnings.push_back("unknown lilaTheme version " + v->dump() + "; reading it as version 1");
    if (auto c = doc.find("colors"); c != doc.end())
    {
        if (!c->is_object())
            r.errors.push_back("\"colors\" must be an object of token: \"#rrggbb\"");
        else
            for (auto& [id, value] : c->items())
            {
                if (!token(id))
                {
                    r.warnings.push_back("unknown token \"" + id + "\" ignored");
                    continue;
                }
                std::optional<Rgba> col = value.is_string() ? parseColor(value.get<std::string>()) : std::nullopt;
                if (!col)
                    r.errors.push_back("\"" + id + "\": " + value.dump() + " is not a colour (#rgb, #rrggbb or #rrggbbaa)");
                else
                    t.colors[id] = *col;
            }
    }
    if (r.errors.empty())
        r.theme = std::move(t);
    return r;
}

ParseResult parseText(std::string_view text)
{
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded())
    {
        ParseResult r;
        r.errors.push_back("not valid JSON");
        return r;
    }
    return parse(doc);
}

json toJson(const Theme& t, bool full)
{
    json colors = json::object();
    for (const TokenInfo& info : kTokens)
    {
        auto it = t.colors.find(info.id);
        if (it != t.colors.end())
            colors[info.id] = formatColor(it->second);
        else if (full)
            colors[info.id] = formatColor(t.color(info.id));
    }
    json out = {{"lilaTheme", 1}, {"name", t.name}, {"base", t.base}, {"colors", colors}};
    if (!t.author.empty())
        out["author"] = t.author;
    return out;
}

std::string toText(const Theme& t, bool full)
{
    // Ordered for people reading or editing the text: header first, tokens in the editor's order.
    json doc = toJson(t, full);
    nlohmann::ordered_json out;
    for (const char* key : {"lilaTheme", "name", "author", "base"})
        if (doc.contains(key))
            out[key] = doc[key];
    nlohmann::ordered_json colors = nlohmann::ordered_json::object();
    for (const TokenInfo& info : kTokens)
        if (doc["colors"].contains(info.id))
            colors[info.id] = doc["colors"][info.id];
    out["colors"] = colors;
    return out.dump(2);
}

std::vector<std::string> names()
{
    std::vector<std::string> out;
    for (const Theme& t : builtIns())
        out.push_back(t.name);
    std::vector<std::string> saved;
    // Range-for keeps only items()'s view alive, not the json it reads.
    const json docs = savedDocs();
    for (const auto& [name, doc] : docs.items())
        if (!isBuiltIn(name))
            saved.push_back(name);
    std::sort(saved.begin(), saved.end());
    out.insert(out.end(), saved.begin(), saved.end());
    return out;
}

bool isBuiltIn(std::string_view name)
{
    return builtIn(name) != nullptr;
}

std::optional<Theme> find(std::string_view name)
{
    if (const Theme* b = builtIn(name))
        return *b;
    json docs = savedDocs();
    auto it = docs.find(std::string(name));
    if (it == docs.end())
        return std::nullopt;
    json doc = *it;
    if (doc.is_object())
        doc["name"] = std::string(name);
    ParseResult r = parse(doc);
    if (!r.theme)
        return std::nullopt;
    return r.theme;
}

std::string currentName()
{
    return PrefStore::get().value<std::string>("general", kThemePref, kDefaultTheme);
}

namespace
{
Rgba grayOf(Rgba c)
{
    const double l = luminance(c);
    const double s = l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
    const auto v = uint8_t(std::lround(std::clamp(s, 0.0, 1.0) * 255.0));
    return {v, v, v, c.a};
}

/**
 * Every token Monotone draws grey, and the node editor's type colours, go grey at their own lightness; the few hues
 * that must survive (axes, components, issue badges) stay. Sockets still differ by shape and mark.
 */
Theme monotoneOf(Theme t)
{
    const Theme& mono = *builtIn(kDefaultTheme);
    for (const TokenInfo& info : tokens())
        if (Rgba m = mono.color(info.id); (m.r == m.g && m.g == m.b) || std::string_view(info.group) == "Node Editor")
            t.colors[info.id] = grayOf(t.color(info.id));
    return t;
}
} // namespace

std::shared_ptr<const Theme> current()
{
    {
        std::scoped_lock lock(gMutex);
        listen();
        if (gCurrent)
            return gCurrent;
    }
    std::optional<Theme> t = find(currentName());
    if (t && PrefStore::get().value<bool>("general", kMonotonePref, false))
        t = monotoneOf(std::move(*t));
    auto made = std::make_shared<const Theme>(t ? std::move(*t) : builtIns()[0]);
    std::scoped_lock lock(gMutex);
    if (!gCurrent)
        gCurrent = made;
    return gCurrent;
}

uint64_t generation()
{
    std::scoped_lock lock(gMutex);
    return gGeneration;
}

void invalidate()
{
    std::scoped_lock lock(gMutex);
    gCurrent.reset();
    ++gGeneration;
}

void setCurrent(const std::string& name, PrefScope scope)
{
    if (!find(name))
        throw CommandError("no theme named \"" + name + "\"");
    PrefStore::get().set("general", kThemePref, name, scope);
    invalidate();
}

void save(const Theme& t, PrefScope scope)
{
    if (t.name.empty())
        throw CommandError("a theme needs a name");
    if (isBuiltIn(t.name))
        throw CommandError("\"" + t.name + "\" is a built-in theme; save it under another name");
    json doc = toJson(t);
    doc.erase("name");
    PrefStore::get().set("general", themeKey(t.name), doc, scope);
    invalidate();
}

bool remove(const std::string& name)
{
    if (isBuiltIn(name) || !savedDocs().contains(name))
        return false;
    PrefStore::get().remove("general", themeKey(name), PrefScope::Workspace);
    PrefStore::get().remove("general", themeKey(name), PrefScope::User);
    if (currentName() == name)
        PrefStore::get().set("general", kThemePref, std::string(kDefaultTheme));
    invalidate();
    return true;
}

void rename(const std::string& from, const std::string& to)
{
    if (isBuiltIn(from))
        throw CommandError("\"" + from + "\" is a built-in theme; duplicate it instead");
    std::optional<Theme> t = find(from);
    if (!t)
        throw CommandError("no saved theme named \"" + from + "\"");
    if (to.empty() || from == to)
        throw CommandError("rename needs a new name");
    if (find(to))
        throw CommandError("a theme named \"" + to + "\" already exists");
    bool wasCurrent = currentName() == from;
    t->name = to;
    save(*t);
    remove(from);
    if (wasCurrent)
        setCurrent(to);
}

std::string duplicate(const std::string& from, std::string to)
{
    std::optional<Theme> t = find(from);
    if (!t)
        throw CommandError("no theme named \"" + from + "\"");
    if (to.empty())
    {
        to = from + " Copy";
        for (int i = 2; find(to); ++i)
            to = from + " Copy " + std::to_string(i);
    }
    else if (find(to))
        throw CommandError("a theme named \"" + to + "\" already exists");
    // A copy of a built-in carries every token, so later changes to the built-in never shift it.
    Theme copy = *t;
    if (isBuiltIn(from))
        for (const TokenInfo& info : kTokens)
            copy.colors[info.id] = t->color(info.id);
    copy.name = to;
    copy.description.clear();
    save(copy);
    return to;
}

bool isDefault(const Theme& t, std::string_view id)
{
    return t.color(id) == builtIns()[0].color(id);
}

Rgba shade(const Theme& t, int level, uint8_t alpha)
{
    level = std::clamp(level, 0, 255);
    // The ramp anchors are the first tokens, in level order.
    const TokenInfo* lo = &kTokens[0];
    const TokenInfo* hi = lo;
    for (const TokenInfo& info : kTokens)
    {
        if (info.level < 0)
            break;
        hi = &info;
        if (info.level >= level)
            break;
        lo = &info;
    }
    Rgba a = t.color(lo->id), b = t.color(hi->id);
    double f = hi->level == lo->level ? 0.0 : double(level - lo->level) / double(hi->level - lo->level);
    auto mix = [f](uint8_t x, uint8_t y) { return uint8_t(std::lround(x + (double(y) - double(x)) * f)); };
    return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), alpha};
}

Rgba shade(int level, uint8_t alpha)
{
    return shade(*current(), level, alpha);
}

void registerCommands()
{
    using T = ArgType;
    FlagDef nameFlag{"name", "n", T::String, nullptr, "Theme name"};

    ops::registerCommand({
        .name = "listThemes",
        .doc = "Interface themes: [{name, builtIn, current, base, author, description}].",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::array();
            std::string cur = currentName();
            for (const std::string& n : names())
                if (std::optional<Theme> t = find(n))
                    out.push_back({{"name", n},
                                   {"builtIn", isBuiltIn(n)},
                                   {"current", n == cur},
                                   {"base", t->base},
                                   {"author", t->author},
                                   {"description", t->description}});
            return out;
        },
    });

    ops::registerCommand({
        .name = "currentTheme",
        .doc = "The current interface theme's name.",
        .undoable = false,
        .run = [](const Args&) -> json { return current()->name; },
    });

    ops::registerCommand({
        .name = "setTheme",
        .doc = "Switch the interface theme; takes effect at once. workspace=True sets it for the open folder only.",
        .positionalDoc = "name",
        .flags = {nameFlag, {"workspace", "ws", T::Bool, false, "Set in the workspace instead of the user prefs"}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string name = requireName(a, "setTheme");
            setCurrent(name, a.get<bool>("workspace", false) ? PrefScope::Workspace : PrefScope::User);
            return name;
        },
    });

    ops::registerCommand({
        .name = "exportTheme",
        .doc = "A theme as shareable JSON text (the current one when no name is given). file= also writes it to a .lilatheme "
               "file. full=True lists every token, not just the ones the theme sets.",
        .positionalDoc = "name",
        .flags = {nameFlag, {"file", "f", T::String, nullptr, "Write to this file"}, {"full", "", T::Bool, false, "Every token, resolved"}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string name = a.get<std::string>("name", "");
            if (name.empty() && !a.positional.empty() && a.positional[0].is_string())
                name = a.positional[0].get<std::string>();
            std::optional<Theme> t = name.empty() ? std::optional<Theme>(*current()) : find(name);
            if (!t)
                throw CommandError("exportTheme: no theme named \"" + name + "\"");
            std::string text = toText(*t, a.get<bool>("full", false) || isBuiltIn(t->name));
            if (std::string file = a.get<std::string>("file", ""); !file.empty())
            {
                std::ofstream f(std::filesystem::path(std::u8string(file.begin(), file.end())), std::ios::binary);
                f << text << "\n";
                if (!f)
                    throw CommandError("exportTheme: cannot write \"" + file + "\"");
            }
            return text;
        },
    });

    ops::registerCommand({
        .name = "importTheme",
        .doc = "Add a theme from JSON text or a .lilatheme file and return its name. Fails with the problems found when the "
               "text is not a valid theme; unknown tokens are skipped. name= saves it under another name, apply=True "
               "switches to it, replace=True overwrites a saved theme of the same name.",
        .flags = {{"text", "t", T::String, nullptr, "Theme JSON"},
                  {"file", "f", T::String, nullptr, "A .lilatheme file"},
                  nameFlag,
                  {"apply", "a", T::Bool, false, "Switch to it"},
                  {"replace", "r", T::Bool, false, "Overwrite a saved theme with the same name"},
                  {"workspace", "ws", T::Bool, false, "Save in the workspace instead of the user prefs"}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string text = a.get<std::string>("text", "");
            if (std::string file = a.get<std::string>("file", ""); !file.empty())
                text = readFile(file);
            if (text.empty())
                throw CommandError("importTheme: give text= or file=");
            ParseResult r = parseText(text);
            if (!r.theme)
                throw CommandError("importTheme: " + joined(r.errors));
            if (std::string as = a.get<std::string>("name", ""); !as.empty())
                r.theme->name = as;
            if (isBuiltIn(r.theme->name))
                throw CommandError("importTheme: \"" + r.theme->name + "\" is a built-in theme; import it with name= set");
            if (find(r.theme->name) && !a.get<bool>("replace", false))
                throw CommandError("importTheme: a theme named \"" + r.theme->name + "\" exists; pass replace=True or name=");
            for (const std::string& w : r.warnings)
                log::warn("importTheme: {}", w);
            save(*r.theme, a.get<bool>("workspace", false) ? PrefScope::Workspace : PrefScope::User);
            if (a.get<bool>("apply", false))
                setCurrent(r.theme->name);
            return r.theme->name;
        },
    });

    ops::registerCommand({
        .name = "deleteTheme",
        .doc = "Delete a saved theme. Built-ins can't be deleted; deleting the current theme switches to Monotone.",
        .positionalDoc = "name",
        .flags = {nameFlag},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string name = requireName(a, "deleteTheme");
            if (!remove(name))
                throw CommandError("deleteTheme: no saved theme named \"" + name + "\"");
            return name;
        },
    });

    ops::registerCommand({
        .name = "renameTheme",
        .doc = "Rename a saved theme.",
        .positionalDoc = "name",
        .flags = {nameFlag, {"to", "", T::String, nullptr, "New name"}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string name = requireName(a, "renameTheme");
            std::string to = a.get<std::string>("to", "");
            if (to.empty() && a.positional.size() > 1 && a.positional[1].is_string())
                to = a.positional[1].get<std::string>();
            rename(name, to);
            return to;
        },
    });

    ops::registerCommand({
        .name = "duplicateTheme",
        .doc = "Copy a theme to a new saved one you can edit; returns the new name.",
        .positionalDoc = "name",
        .flags = {nameFlag, {"to", "", T::String, nullptr, "Name of the copy; default \"<name> Copy\""}},
        .undoable = false,
        .run = [](const Args& a) -> json { return duplicate(requireName(a, "duplicateTheme"), a.get<std::string>("to", "")); },
    });

    ops::registerCommand({
        .name = "setThemeColor",
        .doc = "Set one token of a saved theme, e.g. setThemeColor('Mine', token='highlight', color='#3366cc'). An empty "
               "color clears the token back to the base theme's.",
        .positionalDoc = "name",
        .flags = {nameFlag, {"token", "", T::String, nullptr, "Token id (see listThemeTokens)"}, {"color", "c", T::String, nullptr, "#rrggbb"}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string name = requireName(a, "setThemeColor");
            if (isBuiltIn(name))
                throw CommandError("setThemeColor: \"" + name + "\" is built in; duplicateTheme it first");
            std::optional<Theme> t = find(name);
            if (!t)
                throw CommandError("setThemeColor: no saved theme named \"" + name + "\"");
            std::string id = a.get<std::string>("token", "");
            if (!token(id))
                throw CommandError("setThemeColor: unknown token \"" + id + "\"");
            std::string value = a.get<std::string>("color", "");
            if (value.empty())
                t->colors.erase(id);
            else if (std::optional<Rgba> c = parseColor(value))
                t->colors[id] = *c;
            else
                throw CommandError("setThemeColor: \"" + value + "\" is not a colour (#rrggbb)");
            save(*t);
            return formatColor(t->color(id));
        },
    });

    ops::registerCommand({
        .name = "listThemeTokens",
        .doc = "Every colour a theme can set: [{id, label, group}].",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::array();
            for (const TokenInfo& t : kTokens)
                out.push_back({{"id", t.id}, {"label", t.label}, {"group", t.group}});
            return out;
        },
    });
}
} // namespace lila::ui::themes
