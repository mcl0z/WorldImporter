cmake_minimum_required(VERSION 3.30.0)
project(WorldImporter VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(zlib REQUIRED)
find_package(libzip CONFIG REQUIRED)

file(GLOB SOURCE_FILES "WorldImporter/*.cpp")
add_executable(WorldImporter ${SOURCE_FILES})

target_link_libraries(WorldImporter PRIVATE libzip::zip ZLIB::ZLIB)

# Windows 正式发行使用 MSVC：其线程运行时在多模型线程下稳定，且 /EHsc
# 保证 try/catch 能正确展开。/utf-8 避免中文注释源文件按本地代码页误解析。
if (MSVC)
    target_compile_options(WorldImporter PRIVATE /EHsc /utf-8 /Zc:preprocessor)
endif()

# Optional standalone CTM regression harness; no world/configuration required.
option(WI_BUILD_CTM_TESTS "Build CTM texture baking regression tests" OFF)
if (WI_BUILD_CTM_TESTS)
    enable_testing()
    add_executable(ctm_regression tests/ctm_regression.cpp)
    target_link_libraries(ctm_regression PRIVATE libzip::zip ZLIB::ZLIB)
    if (MSVC)
        target_compile_options(ctm_regression PRIVATE /EHsc /utf-8 /Zc:preprocessor)
    else()
        target_compile_definitions(ctm_regression PRIVATE _GLIBCXX_ASSERTIONS)
    endif()
    foreach(case plain padded tiny odd mixed mcmeta lowercase mcmeta_tiny mcmeta_odd invalid matching identity)
        add_test(NAME ctm_${case} COMMAND ctm_regression ${case})
        set_tests_properties(ctm_${case} PROPERTIES RESOURCE_LOCK ctm_texture_output)
    endforeach()
endif()
