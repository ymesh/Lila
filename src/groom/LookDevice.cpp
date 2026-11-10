#include "groom/LookDevice.h"

#include <mutex>

namespace lila::groom
{
namespace
{
std::mutex gMutex;
LookDeviceFactory gFactory;
} // namespace

void setLookDeviceFactory(LookDeviceFactory factory)
{
    std::scoped_lock lock(gMutex);
    gFactory = std::move(factory);
}

std::unique_ptr<LookDevice> makeLookDevice()
{
    std::scoped_lock lock(gMutex);
    return gFactory ? gFactory() : nullptr;
}
} // namespace lila::groom
