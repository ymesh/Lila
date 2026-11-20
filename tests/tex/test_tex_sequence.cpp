#include "TexTest.h"

#include "core/Paths.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "tex/Sequence.h"

#include <filesystem>
#include <format>

using namespace textest;

namespace
{
namespace fs = std::filesystem;

/** Four grey frames numbered 10 to 13: 0.1, 0.3, 0.5, 0.7. */
fs::path writeFrames(const std::string& name)
{
    const fs::path dir = tempDir(name);
    for (int i = 0; i < 4; ++i)
    {
        const float v = 0.1f + 0.2f * float(i);
        image::Image img{4, 4, std::vector<float>(4 * 4 * 4)};
        for (size_t t = 0; t < 16; ++t)
        {
            img.rgba[t * 4 + 0] = img.rgba[t * 4 + 1] = img.rgba[t * 4 + 2] = v;
            img.rgba[t * 4 + 3] = 1.0f;
        }
        image::write(dir / std::format("fire_{:04}.exr", 10 + i), img, 32);
    }
    // Same folder, another name: not part of the sequence.
    image::write(dir / "smoke_0001.exr", image::Image{1, 1, {1.0f, 1.0f, 1.0f, 1.0f}}, 32);
    return dir;
}

void buildSet(const fs::path& dir, json values = json::object())
{
    run("newScene");
    run("texCreateSet", {{"args", {"Fx"}}, {"resolution", 8}});
    run("texAddNode", {{"args", {"Fx", "imageSequence"}}, {"name", "Seq"}});
    run("texSetValue", {{"args", {"Fx", "Seq.file", paths::toUtf8(dir / "fire_0011.exr")}}});
    for (const auto& [k, v] : values.items())
        run("texSetValue", {{"args", {"Fx", "Seq." + k, v}}});
    run("texConnect", {{"args", {"Fx", "Seq.out", "Output.baseColor"}}});
}

void goTo(double frame) { run("currentTime", {{"args", {scene::anim::secondsOf(frame)}}}); }

/** The set's base colour at the current time. */
float shown(const fs::path& dir)
{
    run("texEvaluate", {{"args", {"Fx"}}, {"folder", paths::toUtf8(dir / "out")}, {"channels", {"baseColor"}}, {"format", "exr"}, {"backend", "cpu"}});
    return image::read(dir / "out" / "Fx_baseColor.exr").rgba[0];
}
} // namespace

TEST_CASE("tex sequence: frames wrap by After End and blend between neighbours")
{
    using sequence::framesAt;
    CHECK(framesAt(5.0, 4, "loop", false).first == 1);
    CHECK(framesAt(-1.0, 4, "loop", false).first == 3);
    CHECK(framesAt(9.0, 4, "hold", false).first == 3);
    CHECK(framesAt(-2.0, 4, "hold", false).first == 0);
    CHECK(framesAt(4.0, 4, "pingPong", false).first == 2);
    CHECK(framesAt(6.0, 4, "pingPong", false).first == 0);
    const sequence::Frames f = framesAt(3.25, 4, "loop", true);
    CHECK(f.first == 3);
    CHECK(f.second == 0);
    CHECK(f.mix == doctest::Approx(0.25));
    CHECK(framesAt(3.25, 4, "loop", false).mix == 0.0);
    // Held on the end frame, there is no neighbour to fade into.
    CHECK(framesAt(7.5, 4, "hold", true).mix == 0.0);
}

TEST_CASE("tex sequence: the files are the picked frame's numbered siblings, in order")
{
    const fs::path dir = writeFrames("sequence_files");
    const std::vector<std::string> byFrame = sequence::files(paths::toUtf8(dir / "fire_0012.exr"));
    REQUIRE(byFrame.size() == 4);
    CHECK(fs::path(paths::fromUtf8(byFrame[0])).filename() == "fire_0010.exr");
    CHECK(fs::path(paths::fromUtf8(byFrame[3])).filename() == "fire_0013.exr");
    CHECK(sequence::files(paths::toUtf8(dir / "fire_####.exr")) == byFrame);
    CHECK(sequence::files(paths::toUtf8(dir / "none_####.exr")).empty());
}

TEST_CASE("tex sequence: the set shows the frame of the scene time")
{
    const fs::path dir = writeFrames("sequence_play");
    buildSet(dir);
    goTo(1.0);
    CHECK(shown(dir) == doctest::Approx(0.1).epsilon(0.01));
    goTo(3.0);
    CHECK(shown(dir) == doctest::Approx(0.5).epsilon(0.01));
    goTo(5.0);
    CHECK(shown(dir) == doctest::Approx(0.1).epsilon(0.01));

    run("texSetValue", {{"args", {"Fx", "Seq.loop", "hold"}}});
    CHECK(shown(dir) == doctest::Approx(0.7).epsilon(0.01));

    run("texSetValue", {{"args", {"Fx", "Seq.speed", 0.5}}});
    run("texSetValue", {{"args", {"Fx", "Seq.interpolation", "blend"}}});
    goTo(2.0);
    CHECK(shown(dir) == doctest::Approx(0.2).epsilon(0.01));
    run("texSetValue", {{"args", {"Fx", "Seq.interpolation", "hold"}}});
    CHECK(shown(dir) == doctest::Approx(0.1).epsilon(0.01));

    run("texSetValue", {{"args", {"Fx", "Seq.offset", 2}}});
    run("texSetValue", {{"args", {"Fx", "Seq.frames", 3}}});
    goTo(1.0);
    CHECK(shown(dir) == doctest::Approx(0.5).epsilon(0.01));
}

TEST_CASE("tex sequence: the planner's own parameters stay out of the node's fields")
{
    const fs::path dir = writeFrames("sequence_fields");
    buildSet(dir);
    const json info = run("texGraphInfo", {{"args", {"Fx"}}});
    for (const json& n : info["nodes"])
        if (n["name"] == "Seq")
            for (const json& p : n["params"])
            {
                CHECK(p["id"] != "next");
                CHECK(p["id"] != "mix");
            }
    // The stored graph keeps the picked file: frames resolve on the evaluated copy only.
    goTo(3.0);
    shown(dir);
    const json after = run("texGraphInfo", {{"args", {"Fx"}}});
    for (const json& n : after["definition"]["nodes"])
        if (n["name"] == "Seq")
            CHECK(fs::path(paths::fromUtf8(n["values"]["file"].get<std::string>())).filename() == "fire_0011.exr");
}

TEST_CASE("tex sequence: a driven speed keys on the set and accumulates")
{
    const fs::path dir = writeFrames("sequence_keyed");
    buildSet(dir, {{"loop", "hold"}});
    const json plug = run("texDriveValue", {{"args", {"Fx", "Seq.speed"}}});
    CHECK(plug == "Fx.Seq_speed");
    CHECK(run("getAttr", {{"args", {"Fx.Seq_speed"}}}) == 1.0);

    // Unkeyed, the attribute drives the parameter, and the field writes through to it.
    run("texSetValue", {{"args", {"Fx", "Seq.speed", 2.0}}});
    CHECK(run("getAttr", {{"args", {"Fx.Seq_speed"}}}) == 2.0);
    goTo(2.0);
    CHECK(shown(dir) == doctest::Approx(0.5).epsilon(0.01));

    // Speed 1 until frame 3, then stopped: the sequence stays where it got to instead of jumping back to its start.
    for (const auto& [frame, speed] : {std::pair{1.0, 1.0}, {3.0, 1.0}, {3.01, 0.0}, {10.0, 0.0}})
        run("setKeyframe", {{"args", {"Fx.Seq_speed"}}, {"time", scene::anim::secondsOf(frame)}, {"value", speed}, {"inTangentType", "linear"},
                            {"outTangentType", "linear"}});
    goTo(3.0);
    CHECK(shown(dir) == doctest::Approx(0.5).epsilon(0.01));
    goTo(9.0);
    CHECK(shown(dir) == doctest::Approx(0.5).epsilon(0.01));

    run("texDriveValue", {{"args", {"Fx", "Seq.speed"}}, {"remove", true}});
    CHECK_THROWS(run("getAttr", {{"args", {"Fx.Seq_speed"}}}));
    run("undo");
    CHECK_NOTHROW(run("getAttr", {{"args", {"Fx.Seq_speed"}}}));
}

TEST_CASE("tex sequence: texExportFlipbook lays the frames out in a sheet")
{
    const fs::path dir = writeFrames("sequence_flipbook");
    buildSet(dir);
    goTo(2.0);
    const json r = run("texExportFlipbook", {{"args", {"Fx"}},
                                             {"startFrame", 1},
                                             {"endFrame", 4},
                                             {"cellSize", 8},
                                             {"channels", {"baseColor"}},
                                             {"folder", paths::toUtf8(dir / "book")},
                                             {"format", "exr"},
                                             {"backend", "cpu"}});
    CAPTURE(r.dump());
    CHECK(r["columns"] == 2);
    CHECK(r["rows"] == 2);
    CHECK(r["frames"] == 4);
    const image::Image sheet = image::read(dir / "book" / "Fx_baseColor_flipbook_2x2.exr");
    REQUIRE(sheet.width == 16);
    REQUIRE(sheet.height == 16);
    CHECK(texel(sheet, 2, 2)[0] == doctest::Approx(0.1).epsilon(0.01));
    CHECK(texel(sheet, 10, 2)[0] == doctest::Approx(0.3).epsilon(0.01));
    CHECK(texel(sheet, 2, 10)[0] == doctest::Approx(0.5).epsilon(0.01));
    CHECK(texel(sheet, 10, 10)[0] == doctest::Approx(0.7).epsilon(0.01));
    // The export leaves the scene's time where it was.
    CHECK(scene::Graph::current().time() == doctest::Approx(scene::anim::secondsOf(2.0)));
}
