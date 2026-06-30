cmake_minimum_required(VERSION 3.16)
project(r3d C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 14)

option(R3D_BACKEND_VGLITE "Build VGLite backend" OFF)
option(R3D_BACKEND_OPENGL "Build OpenGL ES backend (Linux)" OFF)
option(R3D_BUILD_TOOLS    "Build offline tools" ON)

# ---- core 引擎库 ----
set(R3D_CORE_SRC
    core/r3d_math.c
    core/r3d_model.c
    core/r3d_anim.c
    core/r3d_deform.c
    core/r3d_skin.c
    core/r3d_backend.c
    backend/null/backend_null.c
)
if(R3D_BACKEND_VGLITE)
    list(APPEND R3D_CORE_SRC backend/vglite/backend_vglite.c)
endif()
if(R3D_BACKEND_OPENGL)
    list(APPEND R3D_CORE_SRC backend/opengl/backend_opengl.c)
endif()

add_library(r3d STATIC ${R3D_CORE_SRC})
target_include_directories(r3d PUBLIC include)
target_compile_options(r3d PRIVATE -Wall -Wextra)

find_library(M_LIB m)
if(M_LIB)
    target_link_libraries(r3d PUBLIC ${M_LIB})
endif()

if(R3D_BACKEND_VGLITE)
    target_compile_definitions(r3d PUBLIC R3D_BACKEND_VGLITE)
    if(NOT R3D_VGLITE_INC)
        set(R3D_VGLITE_INC "/media/pengxiaolei/workspace/code/vela/dev-graphic/vendor/bes/framework/services/platform/drivers/graphic/gpu/drv/vglite/inc"
            CACHE PATH "Path to VGLite headers (vg_lite.h)")
    endif()
    target_include_directories(r3d PUBLIC ${R3D_VGLITE_INC})
    if(R3D_VGLITE_LIB)
        # 目标平台提供的真实 VGLite 库
        target_link_libraries(r3d PUBLIC ${R3D_VGLITE_LIB})
    else()
        # 本机无硬件：编入 CPU 软件实现(softsim)用于验证后端逻辑
        target_sources(r3d PRIVATE backend/vglite/vg_lite_softsim.c)
    endif()
endif()
if(R3D_BACKEND_OPENGL)
    target_compile_definitions(r3d PUBLIC R3D_BACKEND_OPENGL)
    target_link_libraries(r3d PUBLIC glfw GLESv2)
endif()

# ---- 测试（后端无关，用 null）----
enable_testing()
add_executable(test_m1 tests/test_m1.c)
target_link_libraries(test_m1 PRIVATE r3d)
add_test(NAME test_m1 COMMAND test_m1)

add_executable(test_m2 tests/test_m2.c)
target_link_libraries(test_m2 PRIVATE r3d)
add_test(NAME test_m2 COMMAND test_m2 ${CMAKE_SOURCE_DIR}/tests/assets/tri.b3dm)

# ---- demo（仅 OpenGL 后端）----
if(R3D_BACKEND_OPENGL)
    add_executable(demo_viewer tools/demo/demo_viewer.c)
    target_link_libraries(demo_viewer PRIVATE r3d)
endif()

# ---- VGLite 无头 demo（渲染到 PPM，验证 VGLite 后端）----
if(R3D_BACKEND_VGLITE)
    add_executable(demo_vglite tools/demo/demo_vglite.c)
    target_link_libraries(demo_vglite PRIVATE r3d)
endif()

# ---- 离线工具 gltf2b3dm ----
if(R3D_BUILD_TOOLS)
    add_executable(gltf2b3dm
        tools/gltf2b3dm/main.c
        tools/gltf2b3dm/third_party/tp_impl.c
        tools/gltf2b3dm/third_party/meshopt/simplifier.cpp
        tools/gltf2b3dm/third_party/meshopt/indexgenerator.cpp
        tools/gltf2b3dm/third_party/meshopt/vcacheoptimizer.cpp
        tools/gltf2b3dm/third_party/meshopt/vfetchoptimizer.cpp
        tools/gltf2b3dm/third_party/meshopt/quantization.cpp
        tools/gltf2b3dm/third_party/meshopt/vertexcodec.cpp
        tools/gltf2b3dm/third_party/meshopt/indexcodec.cpp
        tools/gltf2b3dm/third_party/meshopt/vertexfilter.cpp
        tools/gltf2b3dm/g2b_draco.cpp
    )
    target_include_directories(gltf2b3dm PRIVATE
        include tools/gltf2b3dm tools/gltf2b3dm/third_party
        tools/gltf2b3dm/third_party/meshopt)
    find_library(M_LIB_TOOL m)
    if(M_LIB_TOOL)
        target_link_libraries(gltf2b3dm PRIVATE ${M_LIB_TOOL})
    endif()
    target_compile_options(gltf2b3dm PRIVATE -w)

    # ---- 可选 Draco 解码支持（KHR_draco_mesh_compression）----
    # 需先拉取源码: third_party/draco_src（见 tools/gltf2b3dm/README）
    # git clone --depth 1 -b 1.5.7 https://github.com/google/draco.git \
    #     tools/gltf2b3dm/third_party/draco_src
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gltf2b3dm/third_party/draco_src/CMakeLists.txt")
        set(DRACO_TRANSCODER_SUPPORTED OFF CACHE BOOL "" FORCE)
        set(DRACO_TESTS OFF CACHE BOOL "" FORCE)
        add_subdirectory(tools/gltf2b3dm/third_party/draco_src draco_build EXCLUDE_FROM_ALL)
        target_compile_definitions(gltf2b3dm PRIVATE G2B_HAVE_DRACO)
        target_include_directories(gltf2b3dm PRIVATE
            tools/gltf2b3dm/third_party/draco_src/src
            ${CMAKE_CURRENT_BINARY_DIR})
        target_link_libraries(gltf2b3dm PRIVATE draco_static)
        message(STATUS "gltf2b3dm: Draco 解码已启用")
    else()
        message(STATUS "gltf2b3dm: 未找到 draco_src，KHR_draco_mesh_compression 模型将被跳过")
    endif()

    add_executable(b3dm2gltf tools/b3dm2gltf/b3dm2gltf.c)
    target_include_directories(b3dm2gltf PRIVATE include tools/gltf2b3dm/third_party)
    find_library(M_LIB_B2G m)
    if(M_LIB_B2G)
        target_link_libraries(b3dm2gltf PRIVATE ${M_LIB_B2G})
    endif()
    target_compile_options(b3dm2gltf PRIVATE -w)
endif()
