// Built-in fabric presets, tuned for game garments rather than measured from swatches.

#include "TailorCore.h"

#include <cctype>

namespace tailor
{
namespace
{
Fabric makeFabricPreset(const char* name, float warp, float weft, float shear, float bendWarp, float bendWeft,
                        float bendBias, float strainLimit, float density, float thickness, float friction, float damping)
{
    Fabric f;
    f.name = name;
    f.stretchWarp = warp;
    f.stretchWeft = weft;
    f.shear = shear;
    f.bendWarp = bendWarp;
    f.bendWeft = bendWeft;
    f.bendBias = bendBias;
    f.strainLimit = strainLimit;
    f.density = density;
    f.thickness = thickness;
    f.friction = friction;
    f.damping = damping;
    return f;
}

bool fabricNameEquals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}
} // namespace

const std::vector<Fabric>& fabricPresets()
{
    // name, stretch warp/weft/shear, bend warp/weft/bias, strain limit, density, thickness, friction, damping
    static const std::vector<Fabric> presets{
        makeFabricPreset("Cotton", 4000.f, 3000.f, 300.f, 2e-5f, 2e-5f, 1.5e-5f, 0.10f, 0.15f, 0.0010f, 0.30f, 0.010f),
        makeFabricPreset("Linen", 6000.f, 5000.f, 250.f, 4e-5f, 3.5e-5f, 2.5e-5f, 0.06f, 0.18f, 0.0008f, 0.35f, 0.010f),
        makeFabricPreset("Denim", 12000.f, 9000.f, 800.f, 1.5e-4f, 1.2e-4f, 1e-4f, 0.05f, 0.40f, 0.0012f, 0.40f, 0.015f),
        makeFabricPreset("Canvas", 15000.f, 14000.f, 1500.f, 3e-4f, 3e-4f, 2.5e-4f, 0.03f, 0.45f, 0.0015f, 0.40f, 0.015f),
        makeFabricPreset("Wool", 3000.f, 2500.f, 250.f, 6e-5f, 5e-5f, 4e-5f, 0.15f, 0.30f, 0.0020f, 0.45f, 0.020f),
        makeFabricPreset("Leather", 20000.f, 20000.f, 4000.f, 8e-4f, 8e-4f, 8e-4f, 0.03f, 0.80f, 0.0015f, 0.50f, 0.020f),
        makeFabricPreset("Suede", 14000.f, 14000.f, 3000.f, 4e-4f, 4e-4f, 4e-4f, 0.05f, 0.70f, 0.0015f, 0.60f, 0.020f),
        makeFabricPreset("Silk", 2500.f, 2000.f, 100.f, 3e-6f, 3e-6f, 2e-6f, 0.08f, 0.06f, 0.0003f, 0.20f, 0.005f),
        makeFabricPreset("Chiffon", 800.f, 700.f, 40.f, 5e-7f, 5e-7f, 4e-7f, 0.12f, 0.03f, 0.0002f, 0.25f, 0.010f),
        makeFabricPreset("Rubber", 500.f, 500.f, 500.f, 1e-4f, 1e-4f, 1e-4f, 0.f, 1.00f, 0.0020f, 0.80f, 0.020f),
        makeFabricPreset("Burlap", 9000.f, 8000.f, 500.f, 1.5e-4f, 1.5e-4f, 1e-4f, 0.04f, 0.35f, 0.0015f, 0.60f, 0.020f),
        makeFabricPreset("Heavy", 30000.f, 30000.f, 8000.f, 2e-3f, 2e-3f, 2e-3f, 0.02f, 3.00f, 0.0040f, 0.40f, 0.050f),
        makeFabricPreset("Fleece", 2000.f, 1500.f, 180.f, 6e-5f, 5e-5f, 4e-5f, 0.20f, 0.28f, 0.0030f, 0.60f, 0.030f),
        makeFabricPreset("Plush", 1500.f, 900.f, 120.f, 1.2e-4f, 1e-4f, 9e-5f, 0.25f, 0.35f, 0.0040f, 0.70f, 0.040f),
        makeFabricPreset("Banner", 7000.f, 6000.f, 400.f, 3e-5f, 3e-5f, 2e-5f, 0.05f, 0.20f, 0.0006f, 0.30f, 0.005f),
        // Stretch knits: soft enough to stretch over the body from a pattern cut smaller than it.
        makeFabricPreset("Spandex", 700.f, 550.f, 60.f, 6e-6f, 6e-6f, 5e-6f, 0.60f, 0.20f, 0.0008f, 0.45f, 0.020f),
        makeFabricPreset("Neoprene", 1600.f, 1500.f, 300.f, 5e-4f, 5e-4f, 4e-4f, 0.40f, 0.55f, 0.0030f, 0.70f, 0.030f),
        makeFabricPreset("Nylon", 9000.f, 8000.f, 350.f, 1e-5f, 1e-5f, 8e-6f, 0.04f, 0.09f, 0.0004f, 0.20f, 0.008f),
        makeFabricPreset("Twill", 10000.f, 8000.f, 600.f, 1e-4f, 9e-5f, 7e-5f, 0.05f, 0.30f, 0.0011f, 0.40f, 0.015f),
    };
    return presets;
}

const Fabric* findFabric(const std::string& name)
{
    for (const Fabric& f : fabricPresets())
        if (fabricNameEquals(f.name, name))
            return &f;
    return nullptr;
}

} // namespace tailor
