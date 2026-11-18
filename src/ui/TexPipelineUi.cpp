#include "ui/TexPipelineUi.h"

#include "core/ImageIO.h"
#include "core/Log.h"
#include "mcp/McpServer.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "tex/TextureSet.h"
#include "ui/Actions.h"
#include "ui/AgentCapture.h"
#include "ui/Menus.h"
#include "ui/TextureGraphUi.h"
#include "ui/FileDialog.h"
#include "ui/UiCommands.h"
#include "ui/UiCommon.h"

#include <QApplication>
#include <QByteArray>
#include <QFileDialog>
#include <QInputDialog>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <format>

namespace lila::ui
{
namespace
{

std::string needSet(const char* what)
{
    std::string set = currentTextureSet();
    if (set.empty())
        log::warn("{}: no texture set", what);
    return set;
}

/** What Export Image writes: the 2D View's output when it shows this set, else a channel picked from a list. */
std::string pickSource(const std::string& set)
{
    const texture2d::Source shown = texture2d::current();
    if (shown.set == set && !shown.node.empty())
        return shown.node + "." + (shown.socket.empty() ? std::string("out") : shown.socket);
    if (shown.set == set && !shown.channel.empty())
        return shown.channel;
    scene::Node* n = tex::findSet(scene::Graph::current(), set);
    if (!n)
        return {};
    QStringList channels;
    for (const tex::Channel& c : tex::readSettings(*n).channels)
        channels << qs(c.name);
    bool ok = false;
    const QString c = QInputDialog::getItem(QApplication::activeWindow(), "Export Image", "Channel (or show a node in the 2D View first):", channels, 0, false, &ok);
    return ok ? c.toStdString() : std::string();
}

void exportImage()
{
    const std::string set = needSet("Export Image");
    if (set.empty())
        return;
    const std::string source = pickSource(set);
    if (source.empty())
        return;
    std::string stem = set + "_" + source;
    std::replace(stem.begin(), stem.end(), '.', '_');
    const QString path = ui::fileDialog::getSaveFileName(QApplication::activeWindow(), "Export Image", qs(stem + ".png"),
                                                      "Images (*.png *.exr *.tga *.tif *.psd)");
    if (path.isEmpty())
        return;
    if (json r = runUi("texExportImage", {{"args", {set, source, path.toStdString()}}}); r.is_object())
        log::info("Exported {} ({}x{}) to {}", source, r.value("width", 0), r.value("height", 0), r.value("file", std::string()));
}

void exportPsd()
{
    const std::string set = needSet("Export Layers as PSD");
    if (set.empty())
        return;
    if (json r = runUi("texExportPsd", {{"args", {set}}}); r.is_object())
        for (const json& f : r.value("files", json::array()))
            log::info("Wrote {}", f.get<std::string>());
}

void importImages()
{
    const std::string set = needSet("Import Image Layers");
    if (set.empty())
        return;
    const QStringList files = ui::fileDialog::getOpenFileNames(QApplication::activeWindow(), "Import Image Layers", {},
                                                            "Images (*.psd *.png *.exr *.tga *.tif *.tiff)");
    if (files.isEmpty())
        return;
    json a = json::array({set});
    for (const QString& f : files)
        a.push_back(f.toStdString());
    if (json r = runUi("texImportImage", {{"args", a}}); r.is_object())
        log::info("Imported {} layer(s) into {}", r.value("layers", json::array()).size(), set);
}

QImage toQImage(const image::Image& img)
{
    QImage q(img.width, img.height, QImage::Format_RGB32);
    for (int y = 0; y < img.height; ++y)
    {
        auto* row = reinterpret_cast<QRgb*>(q.scanLine(y));
        for (int x = 0; x < img.width; ++x)
        {
            const float* p = &img.rgba[(size_t(y) * size_t(img.width) + size_t(x)) * 4];
            auto b = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
            row[x] = qRgb(b(p[0]), b(p[1]), b(p[2]));
        }
    }
    return q;
}

std::vector<scene::NodeId> meshesUsing(scene::Graph& g, const std::string& slot)
{
    std::vector<scene::NodeId> out;
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n || !n->type().isA("mesh"))
            continue;
        if (const scene::MeshPtr& m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
            for (const auto& [s, count] : scene::materials::slotUsage(*m))
                if (s == slot && count > 0)
                    out.push_back(id);
    }
    return out;
}

/** The meshes using the set's slot, rendered shaded on their own; null when none use it. */
QImage shadedView(const std::string& slot, int size, const std::string& view)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const std::vector<scene::NodeId> meshes = meshesUsing(g, slot);
    if (meshes.empty())
        return {};
    const std::vector<scene::NodeId> before = g.selection();
    capture::Options o;
    o.view = view;
    o.width = o.height = size;
    o.display = "shaded";
    o.isolate = true;
    o.frameSelection = true;
    o.grid = false;
    o.showSelection = false;
    o.label = false;
    g.select(meshes);
    QImage img;
    try
    {
        img = capture::render(o);
    }
    catch (...)
    {
        g.select(before);
        throw;
    }
    g.select(before);
    return img;
}

json previewContent(const json& args)
{
    scene::Graph& g = scene::Graph::current();
    std::string name = args.value("set", std::string());
    if (name.empty())
    {
        const std::vector<std::string> sets = tex::setNames(g);
        if (sets.size() != 1)
            throw std::runtime_error(sets.empty() ? "the scene has no texture sets" : "several texture sets; pass set");
        name = sets.front();
    }
    scene::Node* set = tex::findSet(g, name);
    if (!set)
        throw std::runtime_error("no texture set named " + name);
    const tex::SetSettings s = tex::readSettings(*set);
    const tex::Graph graph = tex::readGraph(*set);
    const int tile = std::clamp(args.value("tile", 192), 64, 512);
    std::vector<tex::OutputRequest> requests;
    for (const tex::OutputRequest& r : tex::channelRequests(graph, s))
        if (!args.contains("channels") || std::ranges::find(args["channels"], json(r.name)) != args["channels"].end())
            requests.push_back(r);
    if (requests.empty())
        throw std::runtime_error("no channels to show");
    std::vector<std::string> warnings;
    const auto out = tex::evaluateOutputs(*set, requests, args.value("udim", s.tiles.front().udim), tile, tile, args.value("backend", std::string()), &warnings);

    const std::vector<std::string> users = tex::materialsUsing(scene::Graph::current(), set->name());
    const std::string slot = users.empty() ? std::string() : users.front();
    QImage shaded;
    std::string text;
    if (args.value("shaded", true))
    {
        try
        {
            shaded = shadedView(slot, tile * 2, args.value("view", std::string("three-quarter")));
        }
        catch (const std::exception& e)
        {
            text += std::string("shaded view failed: ") + e.what() + "\n";
        }
        if (shaded.isNull() && text.empty())
            text += slot.empty() ? std::string("no material uses the set, so there is no shaded view\n")
                                 : "no mesh uses " + slot + ", so there is no shaded view\n";
    }

    // Shaded view on the left (2x2 tiles), channels in two rows to its right.
    const int left = shaded.isNull() ? 0 : 2 * tile;
    const int columns = int(requests.size() + 1) / 2;
    QImage sheet(left + columns * tile, 2 * tile, QImage::Format_RGB32);
    sheet.fill(QColor(40, 40, 40));
    QPainter p(&sheet);
    if (!shaded.isNull())
        p.drawImage(QRect(0, 0, 2 * tile, 2 * tile), shaded);
    text += std::format("Set {} (slot {}). {}Channels, column by column, top then bottom:\n", set->name(), slot,
                        shaded.isNull() ? "" : "Left: shaded mesh. ");
    for (size_t i = 0; i < requests.size(); ++i)
    {
        const image::Image& img = out.at(requests[i].name);
        p.drawImage(QRect(left + int(i / 2) * tile, int(i % 2) * tile, tile, tile), toQImage(img));
        double lo = 1e30, hi = -1e30, mean = 0.0;
        const size_t n = size_t(img.width) * size_t(img.height);
        for (size_t k = 0; k < n; ++k)
        {
            const double v = img.rgba[k * 4];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            mean += v;
        }
        text += std::format("  {} ({}): R min {:.3f} mean {:.3f} max {:.3f}\n", requests[i].name, tex::portTypeName(requests[i].type), lo,
                            n ? mean / double(n) : 0.0, hi);
    }
    p.end();
    for (const std::string& w : warnings)
        text += "  WARNING: " + w + "\n";
    const QByteArray b64 = QByteArray::fromStdString(capture::png(sheet)).toBase64();
    return json::array({{{"type", "image"}, {"data", b64.toStdString()}, {"mimeType", "image/png"}}, {{"type", "text"}, {"text", text}}});
}
} // namespace

void registerTexturePipelineUi()
{
    static bool done = false;
    if (std::exchange(done, true))
        return;
    actions::add({.id = "texture.exportImage",
                  .label = "Export Image...",
                  .category = "Texture",
                  .run = [] { exportImage(); },
                  .doc = "Write the output the 2D View shows (any node's socket or a channel) of the current texture set as an image; "
                         "without one, pick a channel.",
                  .python = "lila.ops.texExportImage(\"RockTex\", \"Noise.out\", \"noise.png\")"});
    actions::add({.id = "texture.exportPsd",
                  .label = "Export Layers as PSD",
                  .category = "Texture",
                  .run = [] { exportPsd(); },
                  .doc = "Write the current texture set's layer stack as one layered PSD per channel, each layer's mask in its alpha.",
                  .python = "lila.ops.texExportPsd(\"RockTex\", channels=[\"baseColor\"])"});
    actions::add({.id = "texture.importImages",
                  .label = "Import Image Layers...",
                  .category = "Texture",
                  .run = [] { importImages(); },
                  .doc = "Add images (each layer of a PSD separately) to the current texture set as paint layers on base colour, "
                         "their alpha as the painted mask.",
                  .python = "lila.ops.texImportImage(\"RockTex\", \"paintover.psd\")"});
    actions::add({.id = "texture.sendToUnreal",
                  .label = "Send Texture Set to Unreal",
                  .category = "Texture",
                  .run =
                      [] {
                          if (std::string set = needSet("Send Texture Set to Unreal"); !set.empty())
                              if (json r = runUi("unrealSendTextureSet", {{"args", {set}}}); r.is_object() && r.value("unchanged", false))
                                  log::info("{}: nothing changed since the last send", set);
                      },
                  .doc = "Export the current texture set through the Unreal preset and update its textures and material instance in "
                         "Unreal (only what changed). From then on it also goes on every save.",
                  .python = "lila.ops.unrealSendTextureSet(\"RockTex\")"});
    menus::addActions("Texture", {"", "texture.exportImage", "texture.exportPsd", "texture.importImages", "", "texture.sendToUnreal"});
}

void registerTexturePipelineMcpTools()
{
    mcp::Server::get().addTool({
        .name = "render_material_preview",
        .description = "Look at a texture set: one image with the meshes using its slot rendered shaded on their own (left) and every channel "
                       "(base colour, roughness, metallic, normal, height...) as tiles, plus per-channel min/mean/max. Use it after "
                       "editing layers or painting to check the result.",
        .inputSchema = {{"type", "object"},
                        {"properties",
                         {{"set", {{"type", "string"}, {"description", "Texture set; default the scene's only one."}}},
                          {"channels", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Only these channels."}}},
                          {"tile", {{"type", "integer"}, {"description", "Channel tile size in px (default 192; the shaded view is twice that)."}}},
                          {"udim", {{"type", "integer"}}},
                          {"shaded", {{"type", "boolean"}, {"description", "Include the shaded mesh (default true)."}}},
                          {"view", {{"type", "string"}, {"description", "Shaded view: three-quarter (default), front, right, top..."}}},
                          {"backend", {{"type", "string"}, {"description", "gpu or cpu for the channels."}}}}}},
        .runContent = [](const json& args) { return previewContent(args); },
    });
}
} // namespace lila::ui
