#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "ops/Command.h"
#include "core/MainQueue.h"
#include "gn/Gn.h"
#include "io/SceneFile.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"

#include <cstdlib>
#include <filesystem>

int main(int argc, char** argv)
{
    // Keep tests out of the real user prefs.
    auto dir = std::filesystem::temp_directory_path() / "lila_slots_tests";
    std::filesystem::create_directories(dir);
#ifdef _WIN32
    _putenv_s("LILA_CONFIG_DIR", dir.string().c_str());
#else
    setenv("LILA_CONFIG_DIR", dir.string().c_str(), 1);
#endif
    lila::mainq::init();
    lila::ops::registerBuiltins();
    lila::scene::types::registerBuiltins();
    lila::gn::registerTypes();
    lila::scene::registerCommands();
    lila::io::registerCommands();
    lila::gn::registerCommands();

    doctest::Context ctx(argc, argv);
    return ctx.run();
}
