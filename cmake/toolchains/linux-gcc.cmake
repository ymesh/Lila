# Linux compiler choice, chainloaded by vcpkg's toolchain (the linux preset sets VCPKG_CHAINLOAD_TOOLCHAIN_FILE).
# C++20 <format> needs GCC 13+. EL9's system GCC is 11, so use the newest /opt/rh/gcc-toolset-* when the
# compiler isn't given explicitly (CC/CXX or -DCMAKE_CXX_COMPILER).
if(NOT CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    execute_process(COMMAND g++ -dumpversion OUTPUT_VARIABLE _sys_gcc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT _sys_gcc OR _sys_gcc VERSION_LESS 13)
        file(GLOB _toolsets LIST_DIRECTORIES true /opt/rh/gcc-toolset-*)
        list(SORT _toolsets COMPARE NATURAL ORDER DESCENDING)
        foreach(_ts IN LISTS _toolsets)
            if(EXISTS ${_ts}/root/usr/bin/g++)
                set(CMAKE_C_COMPILER ${_ts}/root/usr/bin/gcc)
                set(CMAKE_CXX_COMPILER ${_ts}/root/usr/bin/g++)
                break()
            endif()
        endforeach()
    endif()
endif()
