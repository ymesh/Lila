// weatheringUnreal: builds the material functions and materials weathering results need in the linked Unreal project.

#include "Weathering.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/UnrealLink.h>

#include <algorithm>

using namespace lila;

namespace weathering
{
namespace unrealSend
{
struct MaterialRpc
{
    const char* key;     // what the result is listed under
    const char* feature; // LilaLink feature that has the method
    const char* method;
};

/** Every weathering material set LilaLink can build, by the feature that says it can. */
const std::vector<MaterialRpc>& materialRpcs()
{
    static const std::vector<MaterialRpc> list = {
        {"groundTint", "groundTint", "groundTintMaterials"},
    };
    return list;
}
} // namespace unrealSend

void registerUnreal(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "weatheringUnreal",
        .doc = "Build the weathering material functions in the linked Unreal project (MF_LilaGroundTint and the others "
               "weathering ships), each with a generated material built on it, so meshes sent with weathering colour sets "
               "shade the same there. Made when missing or older; regenerate=True rebuilds. Anything this LilaLink can't "
               "build is listed under skipped with the reason. Returns built and skipped.",
        .flags = {{"regenerate", "rg", ArgType::Bool, false, "Rebuild them even when they are current."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            try
            {
                link::Link::get().ensureConnected();
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("weatheringUnreal: Unreal is not connected (") + e.what() + ")");
            }
            auto client = link::Link::get().client();
            const json features = link::Link::get().status().value("features", json::array());
            json built = json::object(), skipped = json::object();
            for (const unrealSend::MaterialRpc& r : unrealSend::materialRpcs())
            {
                if (!features.is_array() || std::find(features.begin(), features.end(), json(r.feature)) == features.end())
                {
                    skipped[r.key] = std::string("this LilaLink has no ") + r.feature + " feature (update the plugin)";
                    continue;
                }
                link::RpcResult res = client->call(r.method, {{"regenerate", a.get<bool>("regenerate", false)}}, link::importCallOptions());
                if (!res.ok)
                    skipped[r.key] = res.error;
                else
                    built[r.key] = res.result;
            }
            return {{"built", built}, {"skipped", skipped}};
        },
        .examples = {"weatheringUnreal()", "weatheringUnreal(regenerate=True)"},
    });
}
} // namespace weathering
