# Building Lila on Linux

Experimental, but it builds and runs: the Lila window, the Vulkan viewport and every extension work on X11. Windows instructions are in the [README](README.md).

| Setup | Compiler | C++ library | glibc |
|---|---|---|---|
| Rocky Linux 9.8 | GCC 15.2 (gcc-toolset-15) | libstdc++ 15 | 2.34 |
| Rocky Linux 9.8 | Clang 21.1.8 (`linux-clang` preset) | libstdc++ 15 | 2.34 |
| Ubuntu 22.04 (distrobox container) | GCC 13.4 (ubuntu-toolchain-r PPA) | libstdc++ 13 | 2.35 |

USD is off on Linux (`build.sh` passes `-DLILA_USD=OFF`): the `usd` port only knows the Windows Python layout.

## What you need

- **A C++ library with `<format>`:** GCC 13 or newer, or Clang using such a libstdc++. GCC 11 and 12 can't build Lila. Enterprise Linux 9 ships GCC 11; `cmake/toolchains/linux-gcc.cmake` then picks the newest `/opt/rh/gcc-toolset-*` by itself.
- **CMake 3.28+ and Ninja.** Older distro CMakes (Ubuntu 22.04 has 3.22) can use the portable CMake and Ninja that vcpkg downloads into `$VCPKG_ROOT/downloads/tools/`, or `pip install cmake ninja`.
- **vcpkg:** a clone of github.com/microsoft/vcpkg after `./bootstrap-vcpkg.sh`, in `VCPKG_ROOT`.
- **Tools some ports build with:** autoconf, automake, libtool, bison, flex, gperf, pkg-config, nasm (FFmpeg).
- **For Qt's X11 (xcb) platform:** the X11, xcb-util and xkbcommon-x11 development packages, plus GL/EGL headers (lists below).
- **A Vulkan 1.3 driver.** Python 3.14 is downloaded into `.deps` by `scripts/fetch-python.sh`; the distro's Python and Qt aren't used.

### Rocky / RHEL 9

```bash
sudo dnf install gcc-toolset-15-gcc-c++ autoconf automake libtool bison flex gperf nasm \
  libxcb-devel xcb-util-devel xcb-util-image-devel xcb-util-keysyms-devel xcb-util-renderutil-devel \
  xcb-util-wm-devel xcb-util-cursor-devel libxkbcommon-devel libxkbcommon-x11-devel \
  libX11-devel libXrender-devel libXi-devel libSM-devel libICE-devel mesa-libGL-devel mesa-libEGL-devel \
  vulkan-loader-devel fontconfig-devel
```

Some of these come from the CRB repository (`sudo dnf config-manager --set-enabled crb`). Ninja: `ninja-build` from EPEL, or `pip install ninja`. For the Clang build also `clang` and `llvm` (`llvm-ar` and `llvm-ranlib`, which ThinLTO ports such as Tracy's tools need). The distro Clang already uses gcc-toolset's libstdc++, since `clang-libs` depends on it.

### Ubuntu 22.04

```bash
sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
sudo rm -f /var/cache/apt/pkgcache.bin /var/cache/apt/srcpkgcache.bin   # add-apt-repository can leave it stale
sudo apt-get update
sudo apt-get install -y gcc-13 g++-13 build-essential pkg-config autoconf autoconf-archive automake libtool \
  bison flex gperf nasm python3 curl zip unzip tar git \
  libx11-dev libx11-xcb-dev libxext-dev libxrender-dev libxi-dev libxkbcommon-dev libxkbcommon-x11-dev \
  libxcb1-dev libxcb-cursor-dev libxcb-glx0-dev libxcb-icccm4-dev libxcb-image0-dev libxcb-keysyms1-dev \
  libxcb-randr0-dev libxcb-render-util0-dev libxcb-shape0-dev libxcb-shm0-dev libxcb-sync-dev libxcb-util-dev \
  libxcb-xfixes0-dev libxcb-xinerama0-dev libxcb-xinput-dev libxcb-xkb-dev libxcb-res0-dev \
  libsm-dev libice-dev libgl-dev libegl-dev libglu1-mesa-dev libvulkan-dev libltdl-dev
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 150 --slave /usr/bin/g++ g++ /usr/bin/g++-13
sudo update-alternatives --install /usr/bin/cc cc /usr/bin/gcc 150
sudo update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++ 150
```

Make GCC 13 the system default, not just `CXX`: vcpkg builds its ports with the system compiler. Don't use the PPA's GCC 14.3 (`14.3.0-12ubuntu1~22~ppa2`): about 1.4% of compiles that include the x86 intrinsic headers crash inside the compiler (`internal compiler error: Segmentation fault` in `ix86_valid_target_attribute_p`), at random, so a full build keeps failing. GCC 13.4 from the same PPA doesn't.

## Build

```bash
export VCPKG_ROOT=<vcpkg clone>
./build.sh                      # configures on first run (all vcpkg ports, Qt included: hours), then builds into build/
build/bin/RelWithDebInfo/lila
```

`./build.sh <dir> <config>` builds into another folder or configuration. Two optional variables pick a second, independent build on the same checkout:

| Variable | Effect |
|---|---|
| `LILA_PRESET=linux-clang` | Clang for Lila and every vcpkg port (triplet `x64-linux-clang`, `cmake/toolchains/linux-clang.cmake`), packages in `vcpkg_installed-clang/`. Use with `./build.sh build-clang`. |
| `LILA_VCPKG_INSTALLED_DIR=<dir>` | Packages in a folder of their own, for another distro or compiler using the same `x64-linux` triplet name, e.g. a container: `./build.sh build-ubuntu` with `vcpkg_installed-ubuntu`. |

Each build needs its own build folder and `vcpkg_installed` folder: packages built with another compiler or glibc must not overwrite each other's. Don't run two builds at the same time either: they share vcpkg's `buildtrees` and `packages` folders under `VCPKG_ROOT`.

## Run

- **X11:** `QT_QPA_PLATFORM=xcb`. The Vulkan surface is created through xcb, so a Wayland session needs XWayland.
- **Headless:** `QT_QPA_PLATFORM=offscreen ./lilabatch` starts Lila without a window. Set `LILA_CONFIG_DIR=<scratch dir>` to keep a test run out of your real preferences.
- **Motion model:** MotionBricks' `mmm_v6_soma_fp16.onnx` isn't in git, so a build without it warns at startup and only the motion features are missing. Supply its folder with `-DLILA_MOTIONBRICKS_MODEL_DIR=<dir>` at configure time, or the file with `LILA_MOTION_MODEL=<file>` at run time. NVIDIA's original file runs on the CPU, which is all Linux uses.

## Building inside a distrobox container with an NVIDIA GPU

A container is an easy way to build on a second distribution without touching the host. It shares your home folder and X11 display, and with the NVIDIA Container Toolkit it also gets the GPU.

1. On the host, install the [NVIDIA Container Toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html) and [distrobox](https://distrobox.it).
2. Create and enter the container: `distrobox create --name ubuntu22 --image ubuntu:22.04 --nvidia`, then `distrobox enter ubuntu22`.
3. Check the GPU from inside: `nvidia-smi -L` lists it, and `/etc/vulkan/icd.d/nvidia_icd.json` exists.
4. Install the Ubuntu packages above, and build with its own folders:
   ```bash
   export VCPKG_ROOT=<vcpkg clone> LILA_VCPKG_INSTALLED_DIR=$PWD/vcpkg_installed-ubuntu
   ./build.sh build-ubuntu
   build-ubuntu/bin/RelWithDebInfo/lila
   ```

The host's files appear under `/run/host` inside the container, but its `/opt` doesn't: a host gcc-toolset isn't usable there, and shouldn't be, since its libstdc++ is made for that distro. Use the container's own compiler.

## Troubleshooting

- **`fatal error: format: No such file or directory`:** the compiler is GCC 11 or 12. Install GCC 13+ (gcc-toolset on Enterprise Linux, the PPA on Ubuntu). For a vcpkg port, the system default compiler is what counts.
- **Configure fails with `CMAKE_C_COMPILER not set` or `Ninja Multi-Config` not found after a port failed:** the failed run cached NOTFOUND values. Fix the port, delete the build folder (not `vcpkg_installed`; finished ports are kept) and run `./build.sh` again. If Ninja really is missing, put it on `PATH`.
- **A port fails with `Could not find nasm`, or with `CMAKE_C_COMPILER_AR-NOTFOUND` under Clang:** install `nasm`, or the `llvm` tools package matching your Clang.
- **`could not load ... undefined symbol` when an extension loads:** Linux links shared libraries even with symbols still missing, so a missing dependency only shows when Lila loads the extension. Link that library into the extension.
- **A crash at startup or exit:** `coredumpctl debug` shows the backtrace. For heap corruption (`corrupted double-linked list`, `double free`), run `valgrind ./lilabatch`, which reports the first bad read or write.
