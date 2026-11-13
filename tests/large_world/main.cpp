#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "core/MainQueue.h"
#include "render/Renderer.h"

#include <cstdlib>
#include <filesystem>

int main(int argc, char** argv)
{
    auto dir = std::filesystem::temp_directory_path() / "lila_large_world_tests";
    std::filesystem::create_directories(dir);
#ifdef _WIN32
    _putenv_s("LILA_CONFIG_DIR", dir.string().c_str());
#else
    setenv("LILA_CONFIG_DIR", dir.string().c_str(), 1);
#endif
    lila::mainq::init();

    doctest::Context ctx(argc, argv);
    const int rc = ctx.run();
    lila::render::Renderer::get().stop();
    return rc;
}
