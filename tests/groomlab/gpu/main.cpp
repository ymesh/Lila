#include <gtest/gtest.h>

#include "core/MainQueue.h"
#include "render/Renderer.h"

#include <cstdlib>
#include <filesystem>
#include <string>

int main(int argc, char** argv)
{
    // Keep the tests out of the real user prefs.
    const auto dir = std::filesystem::temp_directory_path() / "lila_groomlab_gpu_tests";
    std::filesystem::create_directories(dir);
    _putenv_s("LILA_CONFIG_DIR", dir.string().c_str());
    lila::mainq::init();
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    // The renderer the Vulkan backend started stops before its statics go.
    lila::render::Renderer::get().stop();
    return result;
}
