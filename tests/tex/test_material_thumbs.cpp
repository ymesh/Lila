#include "TexTest.h"

#include "ops/Command.h"
#include "render/Renderer.h"
#include "tex/Library.h"
#include "ui/MaterialThumbnails.h"

#include <QImage>

#include <filesystem>
#include <fstream>

using namespace textest;
namespace lib = lila::tex::library;
namespace thumbs = lila::ui::materialThumbs;

namespace
{
namespace fs = std::filesystem;

bool thumbsGpu()
{
    render::Renderer& r = render::Renderer::get();
    if (r.running() || r.start())
        return true;
    MESSAGE("no GPU for material thumbnail tests");
    return false;
}
} // namespace

TEST_CASE("material thumbnails render headless on the shader ball and come from the cache the second time")
{
    if (!thumbsGpu())
        return;
    if (!ops::find("materialLibrary"))
        lib::registerCommands();
    if (!ops::find("renderMaterialThumbnail"))
        thumbs::registerCommands();
    run("newScene");

    Graph g = Graph::makeMaterial("Red");
    g.nodes.front().values = {{"baseColor", {0.6, 0.02, 0.02, 1.0}}, {"roughness", 0.5}};
    fs::path file = lib::tierDir("user") / "Test" / "thumbRed.lilamat";
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << json{{"format", "lilamat"}, {"version", 1}, {"name", "Red"}, {"meta", {{"name", "Thumb Red"}}}, {"graph", g.toJson()}}.dump();
    lib::reload();
    fs::remove_all(thumbs::cacheDir());

    thumbs::Options o;
    o.size = 128;
    thumbs::Result r = thumbs::render("Test/thumbRed", o);
    CHECK_FALSE(r.cached);
    REQUIRE(fs::exists(r.file));
    QImage img(QString::fromStdWString(r.file.wstring()));
    REQUIRE(img.width() == 128);
    QColor centre = img.pixelColor(64, 64), corner = img.pixelColor(2, 2);
    INFO(centre.name().toStdString(), " ", corner.name().toStdString());
    CHECK(centre.red() > centre.green() + 40);
    CHECK(std::abs(corner.red() - corner.green()) < 6);
    CHECK(thumbs::cachedFile("Test/thumbRed", 128) == r.file);

    CHECK(thumbs::render("Test/thumbRed", o).cached);

    // The command, on a mesh of the scene, copying the image out.
    std::string plane = run("polyPlane", {{"width", 200.0}, {"depth", 200.0}})[0];
    fs::path out = fs::temp_directory_path() / "lila_tex_gpu_tests" / "thumb_plane.png";
    fs::remove(out);
    json c = run("renderMaterialThumbnail", {{"args", {"Test/thumbRed"}}, {"mesh", plane}, {"size", 64}, {"path", out.generic_string()}});
    CHECK(c["cached"] == false);
    CHECK(fs::exists(out));
    fs::remove_all(file.parent_path());
    lib::reload();
}

TEST_CASE("smart materials render on the preview ball with its baked mesh maps")
{
    if (!thumbsGpu())
        return;
    run("newScene");
    fs::remove_all(thumbs::cacheDir());
    thumbs::Options o;
    o.size = 128;
    // Rusty Iron's rust follows cavity and curvature: flat mesh maps would leave the ball one colour.
    thumbs::Result r = thumbs::render("smart:Rusty Iron", o);
    REQUIRE(fs::exists(r.file));
    QImage img(QString::fromStdWString(r.file.wstring()));
    REQUIRE(img.width() == 128);
    int lo = 255, hi = 0;
    for (int y = 30; y < 90; ++y)
        for (int x = 30; x < 90; ++x)
        {
            const int v = qGray(img.pixel(x, y));
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    CHECK(hi - lo > 40);
    CHECK_THROWS(thumbs::render("smart:No Such Smart Material", o));
}
