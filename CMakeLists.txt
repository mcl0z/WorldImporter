cmake_minimum_required(VERSION 3.30.0)
project(WorldImporter VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(ZLIB REQUIRED)
find_package(libzip CONFIG REQUIRED)

file(GLOB SOURCE_FILES CONFIGURE_DEPENDS "WorldImporter/*.cpp")
list(REMOVE_ITEM SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/main.cpp")
add_library(WorldImporterCore STATIC ${SOURCE_FILES})
target_include_directories(WorldImporterCore PUBLIC WorldImporter)
target_link_libraries(WorldImporterCore PUBLIC libzip::zip ZLIB::ZLIB)
add_executable(WorldImporter WorldImporter/main.cpp)
target_link_libraries(WorldImporter PRIVATE WorldImporterCore)

# Windows 正式发行使用 MSVC：其线程运行时在多模型线程下稳定，且 /EHsc
# 保证 try/catch 能正确展开。/utf-8 避免中文注释源文件按本地代码页误解析。
function(wi_apply_defaults target)
    if (MSVC)
        target_compile_options(${target} PRIVATE /EHsc /utf-8 /Zc:preprocessor)
    endif()
endfunction()

wi_apply_defaults(WorldImporter)
wi_apply_defaults(WorldImporterCore)

include(CTest)
option(WI_BUILD_CTM_TESTS "Build CTM texture baking regression tests" ON)

# tests/ 是可选目录：删掉它（例如只发布主程序源码）时必须仍能正常编译主程序。
set(WI_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")
if (BUILD_TESTING AND NOT EXISTS "${WI_TESTS_DIR}")
    message(STATUS "tests/ 不存在: 跳过测试目标（要跑测试请恢复 tests/，或显式传 -DBUILD_TESTING=OFF）")
endif()

if (BUILD_TESTING AND EXISTS "${WI_TESTS_DIR}")
    # 跑法（构建目录内）：
    #   ctest                        全部用例
    #   ctest -L ctm                 只跑 CTM 烘焙回归
    #   ctest -R chunk_io            只跑单个用例
    #   ctest --output-on-failure    失败时打印输出

    add_executable(chunk_io_tests
        tests/chunk_io_tests.cpp
        WorldImporter/chunk.cpp
        WorldImporter/decompressor.cpp
        WorldImporter/locutil.cpp
        WorldImporter/nbtutils.cpp)
    target_include_directories(chunk_io_tests PRIVATE WorldImporter)
    target_link_libraries(chunk_io_tests PRIVATE ZLIB::ZLIB)
    wi_apply_defaults(chunk_io_tests)
    add_test(NAME chunk_io COMMAND chunk_io_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(chunk_io PROPERTIES TIMEOUT 30)

    add_executable(import_geometry_tests tests/import_geometry_tests.cpp)
    target_link_libraries(import_geometry_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(import_geometry_tests)
    add_test(NAME import_geometry COMMAND import_geometry_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(import_geometry PROPERTIES TIMEOUT 30)

    add_executable(slab_regression_test tests/slab_regression_test.cpp)
    target_link_libraries(slab_regression_test PRIVATE WorldImporterCore)
    wi_apply_defaults(slab_regression_test)
    add_test(NAME slab_regression COMMAND slab_regression_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(slab_regression PROPERTIES TIMEOUT 30)

    add_executable(foliage_model_tests tests/foliage_model_tests.cpp)
    target_link_libraries(foliage_model_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(foliage_model_tests)
    add_test(NAME foliage_model COMMAND foliage_model_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(foliage_model PROPERTIES TIMEOUT 30)

    # 独立 CTM 回归（自包含：直接编译 CTM.cpp）。烘焙产物写在测试可执行文件旁边，
    # 用例之间用 RESOURCE_LOCK 串行化，避免互相覆盖。
    if (WI_BUILD_CTM_TESTS)
        add_executable(ctm_regression tests/ctm_regression.cpp)
        target_link_libraries(ctm_regression PRIVATE libzip::zip ZLIB::ZLIB)
        wi_apply_defaults(ctm_regression)
        if (NOT MSVC)
            target_compile_definitions(ctm_regression PRIVATE _GLIBCXX_ASSERTIONS)
        endif()
        foreach(case plain padded tiny odd mixed mcmeta lowercase matching identity invalid prefix_tile prefix_scope proxy_pattern grid_random pillar)
            add_test(NAME ctm_${case} COMMAND ctm_regression ${case}
                     WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
            set_tests_properties(ctm_${case} PROPERTIES
                RESOURCE_LOCK ctm_texture_output
                LABELS ctm)
        endforeach()
    endif()
endif()
