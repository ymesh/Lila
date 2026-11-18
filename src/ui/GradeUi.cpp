#include "ui/GradeUi.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "render/FrameState.h"
#include "render/Grade.h"
#include "scene/Graph.h"
#include "scene/PostProcess.h"
#include "ui/Actions.h"
#include "ui/AttributeEditor.h"
#include "ui/Controls.h"
#include "ui/GradeControls.h"
#include "ui/Theme.h"
#include "ui/ViewportDisplay.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QVBoxLayout>

#include <filesystem>
#include <fstream>

namespace lila::ui
{
namespace
{
namespace pp = scene::postProcess;
namespace gr = render::grade;
namespace fs = std::filesystem;

/** The grading attrs a preset sets; every other post process attr stays. */
const std::vector<std::string>& gradeAttrs()
{
    static const std::vector<std::string> names = {"temperature", "tint", "contrast", "saturation", "lift", "gamma", "gain", "offset",
                                                   "liftMaster", "gammaMaster", "gainMaster", "offsetMaster", "shadows", "midtones",
                                                   "highlights", "shadowsRange", "highlightsRange", "curveMaster", "curveRed", "curveGreen",
                                                   "curveBlue", "curveHueVsSat", "curveHueVsHue", "curveLumVsSat"};
    return names;
}

/** Built-in looks, Lila's own: values for gradeAttrs (the rest go back to their defaults). */
const std::vector<std::pair<std::string, json>>& builtinPresets()
{
    static const std::vector<std::pair<std::string, json>> presets = {
        {"Neutral", json::object()},
        {"Ember", {{"temperature", 5600.0}, {"shadows", {0.92, 1.0, 1.08}}, {"highlights", {1.08, 1.0, 0.9}}, {"contrast", 1.1}, {"saturation", 1.1}}},
        {"Overcast", {{"temperature", 7200.0}, {"saturation", 0.75}, {"liftMaster", 0.03}, {"curveMaster", "0 0, 0.25 0.27, 0.75 0.72, 1 0.97"}}},
        {"Night Shift",
         {{"temperature", 9000.0}, {"gammaMaster", 0.85}, {"shadows", {0.85, 0.95, 1.2}}, {"saturation", 0.8}, {"curveMaster", "0 0.02, 0.5 0.42, 1 0.92"}}},
        {"Bleach", {{"saturation", 0.45}, {"contrast", 1.3}, {"curveMaster", "0 0, 0.25 0.2, 0.75 0.82, 1 1"}}},
        {"Sunlit", {{"temperature", 5200.0}, {"gainMaster", 1.08}, {"highlights", {1.06, 1.02, 0.92}}, {"curveLumVsSat", "0 0.45, 0.6 0.56, 1 0.5"}}},
        {"Teal Dusk", {{"shadows", {0.86, 1.02, 1.1}}, {"highlights", {1.1, 1.0, 0.88}}, {"curveHueVsSat", "0 0.6, 0.5 0.62, 0.85 0.4"}}},
    };
    return presets;
}

fs::path userPresetDir()
{
    return paths::userConfigDir() / "presets" / "grades";
}

std::vector<std::string> userPresetNames()
{
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(userPresetDir(), ec))
        if (e.path().extension() == ".json")
            out.push_back(paths::toUtf8(e.path().stem()));
    std::ranges::sort(out);
    return out;
}

json runCmd(const char* name, json call)
{
    return ops::execute(name, call, CallSource::UI);
}

/** The values of gradeAttrs on a post process, as a preset stores them. */
json readGrade(const scene::Node& n)
{
    json out = json::object();
    for (const std::string& a : gradeAttrs())
        out[a] = runCmd("getAttr", {{"args", {n.name() + "." + a}}});
    return out;
}

/** The scene's post process node, created when it has none. */
std::string activePost()
{
    return runCmd("postProcess", {{"existing", true}}).get<std::string>();
}

/**
 * The display-space picture through the scene's whole post process for a grid of inputs, for LUT export. With
 * includeView the inputs are scene-linear over render::ViewTransform's log2 shaper range, else display code values.
 */
render::PostSettings settingsFor(float& exposure, float& gamma)
{
    render::FrameState s;
    s.exposure = 1.0f;
    viewportDisplay::applyPostProcess(s, true);
    viewportDisplay::applyColorManagement(s);
    exposure = std::log2(std::max(s.exposure, 1e-6f));
    gamma = 1.0f;
    return s.post;
}

/**
 * The grade on a display code value, for LUTs applied after the view: the scene-linear part (white balance, contrast,
 * saturation) on the decoded value, which matches the picture where the tonemapper is near linear, then the display part.
 */
glm::vec3 displayGrade(glm::vec3 s, const render::PostSettings& p)
{
    auto decode = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    auto encode = [](float v) {
        v = std::clamp(v, 0.0f, 1.0f);
        return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    };
    glm::vec3 lin(decode(s.r), decode(s.g), decode(s.b));
    lin = gr::applyScene(lin, p);
    return gr::applyDisplay({encode(lin.r), encode(lin.g), encode(lin.b)}, p);
}

QWidget* wheelColumn(controls::ColorWheel* wheel, QWidget* master)
{
    auto* col = new QWidget;
    auto* v = new QVBoxLayout(col);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    v->addWidget(wheel, 1);
    if (master)
        v->addWidget(master);
    return col;
}

/** The Grade section: colour wheels with masters, tonal balance, curves, looks and LUT export. */
QWidget* gradePanel(scene::NodeId id)
{
    scene::Graph& g = scene::Graph::current();
    const scene::Node* n = g.node(id);
    if (!n)
        return nullptr;
    const std::string name = n->name();
    const int first = pp::firstAttr();
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);

    // Drags set attrs live inside one undo chunk, closed on release.
    auto chunkOpen = std::make_shared<bool>(false);
    auto setLive = [name, chunkOpen](const std::string& attr, json value, bool final) {
        if (!*chunkOpen)
        {
            UndoStack::get().openChunk("Grade " + attr);
            *chunkOpen = true;
        }
        try
        {
            runCmd("setAttr", {{"args", {name + "." + attr, value}}});
        }
        catch (const std::exception& e)
        {
            log::error("{}", e.what());
        }
        if (final)
        {
            UndoStack::get().closeChunk();
            *chunkOpen = false;
        }
    };

    auto vecAttr = [&](int attr) { return n->as<glm::dvec3>(first + attr); };
    auto numAttr = [&](int attr) { return n->as<double>(first + attr); };
    auto master = [&](const char* attr, int index, double lo, double hi) {
        auto* f = new controls::SliderField;
        f->setRange(lo, hi);
        f->setDecimals(3);
        f->setSingleStep(0.01);
        f->setValue(numAttr(index));
        f->setToolTip(QString::fromStdString(n->type().attrs[size_t(first + index)].doc));
        QObject::connect(f, &QDoubleSpinBox::valueChanged, f, [setLive, a = std::string(attr)](double value) { setLive(a, value, true); });
        return f;
    };
    // A wheel edits an rgb attr around its neutral: the puck sets the chroma, the attr's own luminance part stays.
    auto wheel = [&](const QString& title, const char* attr, int index, double neutral, double reach) {
        auto* wh = new controls::ColorWheel(title, reach);
        const glm::dvec3 value = vecAttr(index);
        wh->setShift(value - glm::dvec3(neutral));
        const glm::dvec3 luma = value - glm::dvec3(neutral) - controls::ColorWheel::chroma(value - glm::dvec3(neutral));
        QObject::connect(wh, &controls::ColorWheel::shiftChanged, wh, [setLive, a = std::string(attr), neutral, luma](glm::dvec3 shift, bool final) {
            const glm::dvec3 c = glm::dvec3(neutral) + luma + shift;
            setLive(a, json::array({c.r, c.g, c.b}), final);
        });
        return wh;
    };

    auto* wheels = new QGridLayout;
    wheels->setHorizontalSpacing(4);
    wheels->addWidget(wheelColumn(wheel("Lift", "lift", pp::kLift, 0.0, 0.25), master("liftMaster", pp::kLiftMaster, -1.0, 1.0)), 0, 0);
    wheels->addWidget(wheelColumn(wheel("Gamma", "gamma", pp::kGamma, 1.0, 0.5), master("gammaMaster", pp::kGammaMaster, 0.1, 4.0)), 0, 1);
    wheels->addWidget(wheelColumn(wheel("Gain", "gain", pp::kGain, 1.0, 0.5), master("gainMaster", pp::kGainMaster, 0.0, 4.0)), 0, 2);
    wheels->addWidget(wheelColumn(wheel("Offset", "offset", pp::kOffset, 0.0, 0.25), master("offsetMaster", pp::kOffsetMaster, -1.0, 1.0)), 0, 3);
    v->addWidget(controls::section("Wheels", [&] {
        auto* box = new QWidget;
        box->setLayout(wheels);
        return box;
    }(), w, "/ae/grade/wheels"));

    auto* tonal = new QGridLayout;
    tonal->setHorizontalSpacing(4);
    tonal->addWidget(wheelColumn(wheel("Shadows", "shadows", pp::kShadows, 1.0, 0.3), master("shadowsRange", pp::kShadowsRange, 0.0, 1.0)), 0, 0);
    tonal->addWidget(wheelColumn(wheel("Midtones", "midtones", pp::kMidtones, 1.0, 0.3), nullptr), 0, 1);
    tonal->addWidget(wheelColumn(wheel("Highlights", "highlights", pp::kHighlights, 1.0, 0.3), master("highlightsRange", pp::kHighlightsRange, 0.0, 1.0)), 0, 2);
    v->addWidget(controls::section("Tonal Balance", [&] {
        auto* box = new QWidget;
        box->setLayout(tonal);
        box->setToolTip("Colour gains on the shadows, mid-tones and highlights; the fields under Shadows and Highlights set where each range ends.");
        return box;
    }(), w, "/ae/grade/tonal"));

    // Curves: one editor, a segmented strip picking which curve it shows.
    auto* curvesBox = new QWidget;
    auto* cv = new QVBoxLayout(curvesBox);
    cv->setContentsMargins(0, 0, 0, 0);
    // Two strips, one choice: the tone curves, then the hue and saturation ones.
    auto* pick = new controls::Segmented;
    auto* pickHue = new controls::Segmented;
    const char* labels[] = {"Master", "R", "G", "B", "Hue vs Sat", "Hue vs Hue", "Luma vs Sat"};
    const char* tips[] = {"Tone curve on every channel", "Tone curve on red", "Tone curve on green", "Tone curve on blue",
                          "Saturation by hue", "Hue shift by hue", "Saturation by luminance"};
    for (int i = 0; i < gr::kCurveCount; ++i)
        (i <= gr::kBlue ? pick : pickHue)->add(labels[i], {}, tips[i], i);
    auto* editor = new controls::CurveEditor;
    auto current = std::make_shared<int>(0);
    auto show = [editor, current, n, first](int c) {
        *current = c;
        static const char* strokes[] = {"text", "axisX", "axisY", "axisZ", "text", "text", "text"};
        editor->setEvaluator([c](const std::vector<glm::vec2>& p, float x) { return gr::evaluate(gr::Curve(c), p, x); });
        editor->setPoints(gr::parseCurve(n->as<std::string>(first + pp::kCurveMaster + c)), c == gr::kHueVsSat || c == gr::kHueVsHue,
                          c <= gr::kBlue ? -1.0f : 0.5f, themeColor(strokes[c]), c == gr::kHueVsSat || c == gr::kHueVsHue);
    };
    QObject::connect(pick, &controls::Segmented::picked, editor, [show, pickHue](int c) {
        pickHue->clearCurrent();
        show(c);
    });
    QObject::connect(pickHue, &controls::Segmented::picked, editor, [show, pick](int c) {
        pick->clearCurrent();
        show(c);
    });
    QObject::connect(editor, &controls::CurveEditor::pointsChanged, editor, [setLive, current, n, first](std::vector<glm::vec2> pts, bool final) {
        setLive(n->type().attrs[size_t(first + pp::kCurveMaster + *current)].name, gr::formatCurve(pts), final);
    });
    pick->setCurrent(0);
    show(0);
    cv->addWidget(pick);
    cv->addWidget(pickHue);
    cv->addWidget(editor, 1);
    v->addWidget(controls::section("Curves", curvesBox, w, "/ae/grade/curves"));

    // Looks and export.
    auto* looks = new QWidget;
    auto* lf = new QFormLayout(looks);
    auto* presetBox = new QComboBox;
    presetBox->addItem("Choose...", QString());
    for (const auto& [preset, values] : builtinPresets())
        presetBox->addItem(QString::fromStdString(preset), QString::fromStdString(preset));
    for (const std::string& preset : userPresetNames())
        presetBox->addItem(QString::fromStdString(preset), QString::fromStdString(preset));
    presetBox->setToolTip("Puts a look's grade on this post process (its other settings stay). Undo puts the grade back.");
    QObject::connect(presetBox, &QComboBox::activated, presetBox, [presetBox, name](int) {
        const QString preset = presetBox->currentData().toString();
        presetBox->setCurrentIndex(0);
        if (preset.isEmpty())
            return;
        try
        {
            runCmd("gradePreset", {{"args", {preset.toStdString()}}, {"node", name}});
        }
        catch (const std::exception& e)
        {
            log::error("{}", e.what());
        }
    });
    lf->addRow("Look", presetBox);
    std::vector<QWidget*> buttons = {controls::push("render.saveGradePreset", looks, "Save Look"), controls::push("render.exportGradeLut", looks, "Export LUT")};
    lf->addRow("", controls::grid(buttons, 2, looks));
    v->addWidget(controls::section("Looks", looks, w, "/ae/grade/looks"));
    return w;
}
} // namespace

void registerGradeUi()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "gradePreset",
        .doc = "Put a look on a post process (the active one by default): its grade attrs (white balance, contrast, saturation, wheels, "
               "tonal balance, curves) take the look's values, the rest of the grade goes back to neutral. With save, store the post "
               "process's grade as a look under that name instead. With no name, list the looks. Returns the look names.",
        .positionalDoc = "look: the look's name",
        .flags = {{"node", "n", T::String, nullptr, "The post process; the active one (created if needed) when left out."},
                  {"save", "s", T::Bool, false, "Store the post process's grade as this look, in the user's looks."}},
        .run = [](const Args& a) -> json {
            json names = json::array();
            for (const auto& [preset, values] : builtinPresets())
                names.push_back(preset);
            for (const std::string& preset : userPresetNames())
                names.push_back(preset);
            const std::string look = a.positional.empty() ? std::string() : a.positional[0].get<std::string>();
            if (look.empty())
                return names;
            const std::string node = a.has("node") ? a.get<std::string>("node") : activePost();
            scene::Node* n = scene::Graph::current().find(node);
            if (!n || !n->type().isA("postProcess"))
                throw CommandError("gradePreset: " + node + " is not a post process");
            const fs::path file = userPresetDir() / paths::fromUtf8(look + ".json");
            if (a.get<bool>("save", false))
            {
                std::error_code ec;
                fs::create_directories(userPresetDir(), ec);
                std::ofstream(file) << readGrade(*n).dump(2);
                names.push_back(look);
                return names;
            }
            json values;
            for (const auto& [preset, v] : builtinPresets())
                if (preset == look)
                    values = v;
            if (values.is_null())
            {
                std::ifstream in(file);
                if (!in)
                    throw CommandError("gradePreset: no look named " + look);
                values = json::parse(in, nullptr, false);
                if (values.is_discarded())
                    throw CommandError("gradePreset: " + paths::toUtf8(file) + " is not JSON");
            }
            UndoChunk chunk("Look " + look);
            const auto& defs = n->type().attrs;
            for (const std::string& attr : gradeAttrs())
            {
                json value = values.contains(attr) ? values[attr] : json();
                if (value.is_null())
                    for (const auto& d : defs)
                        if (d.name == attr)
                            value = toJson(d.defaultValue);
                ops::execute("setAttr", {{"args", {node + "." + attr, value}}});
            }
            return names;
        },
    });

    ops::registerCommand({
        .name = "exportGradeLut",
        .doc = "Bake the active post process's grade into a LUT file for Resolve, Nuke or Unreal. .cube (Resolve, Nuke): display code "
               "values in and out, for after the view; with includeView, scene-linear light in through a log2 shaper, the tonemapper "
               "or OCIO view and the grade out. .png: a 256 by 16 strip in Unreal's colour grading LUT layout (display in and out). "
               "Lens effects, bloom, grain and vignette are not in a LUT. Returns the path.",
        .positionalDoc = "path: where to write, .cube or .png",
        .flags = {{"size", "sz", T::Int, 33, "Points per axis of a .cube (17 to 129).", {}, {}, 17.0, 129.0},
                  {"includeView", "iv", T::Bool, false, "Also bake exposure and the tonemapper or OCIO view (.cube only)."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string path = a.positional.empty() ? std::string() : a.positional[0].get<std::string>();
            if (path.empty())
                throw CommandError("exportGradeLut: give a path");
            float exposure = 0.0f, gamma = 1.0f;
            const render::PostSettings p = settingsFor(exposure, gamma);
            const fs::path file = paths::fromUtf8(path);
            std::error_code ec;
            if (file.has_parent_path())
                fs::create_directories(file.parent_path(), ec);
            std::string ext = paths::toUtf8(file.extension());
            std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (ext == ".png")
            {
                QImage img(256, 16, QImage::Format_RGB888);
                for (int b = 0; b < 16; ++b)
                    for (int gg = 0; gg < 16; ++gg)
                        for (int r = 0; r < 16; ++r)
                        {
                            const glm::vec3 c = displayGrade(glm::vec3(r, gg, b) / 15.0f, p);
                            img.setPixelColor(b * 16 + r, gg, QColor::fromRgbF(c.r, c.g, c.b));
                        }
                if (!img.save(QString::fromStdString(path)))
                    throw CommandError("exportGradeLut: cannot write " + path);
                return path;
            }
            if (ext != ".cube")
                throw CommandError("exportGradeLut: write a .cube or a .png");
            const int n = std::clamp(a.get<int>("size", 33), 17, 129);
            const bool view = a.get<bool>("includeView", false);
            std::ofstream out(file);
            if (!out)
                throw CommandError("exportGradeLut: cannot write " + path);
            const render::ViewTransform shaper;
            out << "TITLE \"Lila grade\"\n";
            if (view)
            {
                // A 1D shaper takes scene-linear light to log2 between the shaper's ends, then the 3D table grades it.
                const int n1 = 4096;
                const float hi = std::exp2(shaper.shaperMax);
                out << "LUT_1D_SIZE " << n1 << "\nLUT_1D_INPUT_RANGE 0 " << hi << "\n";
                for (int i = 0; i < n1; ++i)
                {
                    const float x = hi * float(i) / float(n1 - 1);
                    const float s = std::clamp((std::log2(std::max(x, std::exp2(shaper.shaperMin))) - shaper.shaperMin) / (shaper.shaperMax - shaper.shaperMin),
                                               0.0f, 1.0f);
                    out << s << ' ' << s << ' ' << s << '\n';
                }
            }
            out << "LUT_3D_SIZE " << n << "\n";
            out.setf(std::ios::fixed);
            out.precision(6);
            for (int b = 0; b < n; ++b)
                for (int gg = 0; gg < n; ++gg)
                    for (int r = 0; r < n; ++r)
                    {
                        glm::vec3 c = glm::vec3(r, gg, b) / float(n - 1);
                        if (view)
                        {
                            for (int k = 0; k < 3; ++k)
                                c[k] = std::exp2(shaper.shaperMin + c[k] * (shaper.shaperMax - shaper.shaperMin));
                            c = gr::applyAll(c, p, exposure, gamma);
                        }
                        else
                            c = displayGrade(c, p);
                        out << c.r << ' ' << c.g << ' ' << c.b << '\n';
                    }
            return path;
        },
    });

    actions::add({.id = "render.exportGradeLut",
                  .label = "Export Grade LUT...",
                  .category = "Render",
                  .doc = "Bakes the post process's grade into a .cube for Resolve or Nuke, or a .png strip for Unreal's colour grading LUT.",
                  .command = "exportGradeLut",
                  .call = json::object(),
                  .icon = "file-export",
                  .keywords = {"lut", "cube", "resolve", "nuke", "unreal", "grade", "export"}});
    actions::add({.id = "render.saveGradePreset",
                  .label = "Save Look...",
                  .category = "Render",
                  .run =
                      [] {
                          bool ok = false;
                          const QString name = QInputDialog::getText(nullptr, "Save Look", "Name", QLineEdit::Normal, {}, &ok);
                          if (!ok || name.trimmed().isEmpty())
                              return;
                          try
                          {
                              runCmd("gradePreset", {{"args", {name.trimmed().toStdString()}}, {"save", true}});
                          }
                          catch (const std::exception& e)
                          {
                              log::error("{}", e.what());
                          }
                      },
                  .doc = "Stores the post process's grade (white balance, contrast, saturation, wheels, tonal balance, curves) as a look of your own.",
                  .python = "lila.ops.gradePreset(\"My Look\", save=True)",
                  .icon = "device-floppy",
                  .keywords = {"preset", "look", "grade", "powergrade", "resolve"}});
    attributeEditor::addSection("postProcess", "Grade", [](scene::NodeId id) { return gradePanel(id); });
    attributeEditor::addSection("postProcess", "Lens", [](scene::NodeId id) -> QWidget* {
        const scene::Node* n = scene::Graph::current().node(id);
        if (!n)
            return nullptr;
        auto* w = new QWidget;
        auto* form = new QFormLayout(w);
        auto* box = new QComboBox;
        box->addItem("Choose...", QString());
        for (const std::string& lens : pp::lensPresetNames())
            box->addItem(QString::fromStdString(lens), QString::fromStdString(lens));
        box->setToolTip("Sets the lens flare, streaks and dirt below to a kind of lens. Undo puts them back.");
        QObject::connect(box, &QComboBox::activated, box, [box, name = n->name()](int) {
            const QString lens = box->currentData().toString();
            box->setCurrentIndex(0);
            if (lens.isEmpty())
                return;
            try
            {
                UndoChunk chunk("Lens " + lens.toStdString());
                runCmd("lensPreset", {{"args", {lens.toStdString()}}, {"node", name}});
            }
            catch (const std::exception& e)
            {
                log::error("{}", e.what());
            }
        });
        form->addRow("Lens", box);
        return w;
    });
}
} // namespace lila::ui
