cmake_minimum_required(VERSION 3.10)
project(tier0_goldsrc C CXX)

# Valve-style port: _LINUX / POSIX branch in public/tier0/platform.h (common/port.h)
# Windows -> tier0.dll (build.bat), Linux -> tier0.so (-fPIC -shared)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(UNIX AND NOT APPLE)
    add_definitions(-D_LINUX -DPOSIX -DLINUX -D_POSIX)
endif()
add_definitions(-DTIER0_DLL_EXPORT -DNDEBUG -D_CRT_SECURE_NO_WARNINGS)

include_directories(public public/tier0 tier0)

# Full tier0 sources — all tier0/*.cpp (100% 1:1, включая testthread)
file(GLOB SRCS "tier0/*.cpp")

if(UNIX)
    add_library(tier0 SHARED ${SRCS})
    set_target_properties(tier0 PROPERTIES
        OUTPUT_NAME tier0
        PREFIX ""
        SUFFIX ".so"
        POSITION_INDEPENDENT_CODE ON
    )
    target_compile_options(tier0 PRIVATE -fPIC -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(tier0 dl pthread m rt)
    # ensure -shared -fPIC
    set_target_properties(tier0 PROPERTIES LINK_FLAGS "-shared -fPIC")
else()
    # Windows fallback: build as DLL matching build.bat (/BASE:0x10000000 handled by tier0.def on MSVC)
    add_library(tier0 SHARED ${SRCS})
endif()
