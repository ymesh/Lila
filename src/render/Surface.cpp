#include "render/Surface.h"

#include "core/Log.h"

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#else
#include <xcb/xcb.h>
#include <vulkan/vulkan_xcb.h>
#endif

namespace lila::render
{
VkSurfaceKHR createSurface(VkInstance instance, uintptr_t nativeWindow)
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
#ifdef _WIN32
    auto create = PFN_vkCreateWin32SurfaceKHR(vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
    VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    ci.hinstance = GetModuleHandleW(nullptr);
    ci.hwnd = HWND(nativeWindow);
    if (!create || create(instance, &ci, nullptr, &surface) != VK_SUCCESS)
        log::error("vulkan: failed to create a window surface");
#else
    // Qt's xcb platform: winId() is an X window, which any connection to that server can target.
    static xcb_connection_t* connection = [] {
        xcb_connection_t* c = xcb_connect(nullptr, nullptr);
        if (!xcb_connection_has_error(c))
            return c;
        xcb_disconnect(c);
        return static_cast<xcb_connection_t*>(nullptr);
    }();
    auto create = PFN_vkCreateXcbSurfaceKHR(vkGetInstanceProcAddr(instance, "vkCreateXcbSurfaceKHR"));
    VkXcbSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR};
    ci.connection = connection;
    ci.window = xcb_window_t(nativeWindow);
    if (!connection)
        log::error("vulkan: no X server connection (run with QT_QPA_PLATFORM=xcb)");
    else if (!create || create(instance, &ci, nullptr, &surface) != VK_SUCCESS)
        log::error("vulkan: failed to create a window surface");
#endif
    return surface;
}

void fillWindow(uintptr_t nativeWindow, float r, float g, float b)
{
#ifdef _WIN32
    auto byte = [](float v) { return int(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    const HWND hwnd = HWND(nativeWindow);
    if (HDC dc = GetDC(hwnd))
    {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        HBRUSH brush = CreateSolidBrush(RGB(byte(r), byte(g), byte(b)));
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
        ReleaseDC(hwnd, dc);
    }
#else
    (void)nativeWindow;
    (void)r;
    (void)g;
    (void)b;
#endif
}

double refreshHz(uintptr_t nativeWindow)
{
#ifdef _WIN32
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (HMONITOR monitor = MonitorFromWindow(HWND(nativeWindow), MONITOR_DEFAULTTONEAREST);
        monitor && GetMonitorInfoW(monitor, &info) && EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
        mode.dmDisplayFrequency > 1)
        return double(mode.dmDisplayFrequency);
#else
    (void)nativeWindow;
#endif
    return 60.0;
}
} // namespace lila::render
