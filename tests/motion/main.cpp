#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "core/MainQueue.h"
#include "fbx/Fbx.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "retarget/SceneRetarget.h"
#include "scene/Node.h"
#include "scene/Rig.h"
#include "scene/SceneCommands.h"

#include <cstdlib>
#include <filesystem>

int main(int argc, char** argv)
{
    // Keep tests out of the real user prefs.
    auto dir = std::filesystem::temp_directory_path() / "lila_motion_tests";
    std::filesystem::create_directories(dir);
#ifdef _WIN32
    _putenv_s("LILA_CONFIG_DIR", dir.string().c_str());
#else
    setenv("LILA_CONFIG_DIR", dir.string().c_str(), 1);
#endif
    lila::mainq::init();
    lila::ops::registerBuiltins();
    lila::scene::types::registerBuiltins();
    lila::scene::registerRigTypes();
    lila::scene::registerCommands();
    lila::scene::registerRigCommands();
    lila::io::registerCommands();
    lila::fbx::registerCommands();
    lila::retarget::registerCommands();

    doctest::Context ctx(argc, argv);
    return ctx.run();
}
