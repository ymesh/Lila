#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "bake/Bake.h"
#include "core/MainQueue.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "render/PaintGpu.h"
#include "render/Renderer.h"
#include "render/TexEval.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "sculpt/SculptSession.h"
#include "tex/TextureSet.h"
#include "ui/PreviewQuality.h"
#include "ui/TexLive.h"
#include "vcs/Process.h"
#include "vcs/SourceControl.h"

#include <cstdlib>
#include <filesystem>

namespace
{
class NoTools final : public lila::vcs::ProcessRunner
{
public:
    lila::vcs::ProcessResult run(const lila::vcs::ProcessSpec&) override { return {}; }
};
} // namespace

int main(int argc, char** argv)
{
    auto dir = std::filesystem::temp_directory_path() / "lila_tex_gpu_tests";
    std::filesystem::create_directories(dir);
#ifdef _WIN32
    _putenv_s("LILA_CONFIG_DIR", dir.string().c_str());
#else
    setenv("LILA_CONFIG_DIR", dir.string().c_str(), 1);
#endif
    lila::mainq::init();
    lila::ops::registerBuiltins();
    lila::scene::types::registerBuiltins();
    lila::tex::registerTypes();
    lila::scene::registerCommands();
    lila::io::registerCommands();
    lila::bake::registerCommands();
    lila::tex::registerCommands();
    lila::vcs::SourceControl::get().setRunner(std::make_shared<NoTools>());
    lila::render::texgpu::registerTexGpu();
    lila::render::paint::registerCommands();
    lila::sculpt::registerCommands();
    lila::ui::previewQuality::registerCommands();

    doctest::Context ctx(argc, argv);
    const int rc = ctx.run();
    lila::ui::texLive::shutdown();
    lila::render::paint::shutdown();
    lila::render::Renderer::get().stop();
    return rc;
}
