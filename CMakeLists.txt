cmake_minimum_required(VERSION 3.16)

# Use the VitaSDK toolchain unless one was given explicitly.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK is not set. Install VitaSDK and export VITASDK=/usr/local/vitasdk")
  endif()
endif()

project(VkVita VERSION 0.1.0 LANGUAGES C CXX)

if(NOT DEFINED VITASDK)
  set(VITASDK "$ENV{VITASDK}")
endif()

# Install straight into the SDK by default, like other vdpm libraries.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND VITASDK)
  set(CMAKE_INSTALL_PREFIX "${VITASDK}/arm-vita-eabi" CACHE PATH "install prefix" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(VKV_BUILD_SAMPLES "Build the sample VPKs" ON)
option(VKV_ENABLE_DEBUG "Compile in logging, validation and profiling (toggled at runtime)" ON)
option(VKV_INSTALL_VULKAN_HEADERS "Install the bundled Khronos Vulkan headers" ON)
option(VKV_WITH_SPIRV "Translate SPIR-V shaders to Cg at runtime (bundled SPIRV-Cross)" ON)
set(VKV_WITH_SHARK "AUTO" CACHE STRING "Runtime Cg compiler via vitaShaRK: AUTO, ON or OFF")
set_property(CACHE VKV_WITH_SHARK PROPERTY STRINGS AUTO ON OFF)
set(VKV_DEFAULT_DEBUG_FLAGS "" CACHE STRING "Default VkvDebugFlags bits (number); empty = library default")

include(GNUInstallDirs)

# vitaShaRK detection --------------------------------------------------------
find_library(VKV_VITASHARK_LIB vitashark)
find_library(VKV_SHACCCGEXT_LIB SceShaccCgExt)
find_path(VKV_VITASHARK_INCLUDE vitashark.h)
if(VKV_WITH_SHARK STREQUAL "AUTO")
  if(VKV_VITASHARK_LIB AND VKV_SHACCCGEXT_LIB AND VKV_VITASHARK_INCLUDE)
    set(VKV_SHARK ON)
  else()
    set(VKV_SHARK OFF)
    message(STATUS "VkVita: vitaShaRK not found, runtime Cg compilation disabled (vdpm vitashark to enable)")
  endif()
elseif(VKV_WITH_SHARK)
  set(VKV_SHARK ON)
  if(NOT VKV_VITASHARK_LIB OR NOT VKV_SHACCCGEXT_LIB)
    message(FATAL_ERROR "VKV_WITH_SHARK=ON but libvitashark.a / libSceShaccCgExt.a were not found")
  endif()
else()
  set(VKV_SHARK OFF)
endif()
message(STATUS "VkVita: runtime Cg compiler (vitaShaRK): ${VKV_SHARK}")
message(STATUS "VkVita: debug/validation/profiling support: ${VKV_ENABLE_DEBUG}")
message(STATUS "VkVita: SPIR-V shaders: ${VKV_WITH_SPIRV}")
if(VKV_WITH_SPIRV AND NOT VKV_SHARK)
  message(STATUS "VkVita: SPIR-V shaders only load from the shader cache without the runtime Cg compiler")
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(vkvita STATIC
  src/vkv_config.c
  src/vkv_formats.c
  src/vkv_instance.c
  src/vkv_device.c
  src/vkv_memory.c
  src/vkv_image.c
  src/vkv_shader.c
  src/vkv_pipeline.c
  src/vkv_cmd.c
  src/vkv_queue.c
  src/vkv_clear.c
  src/vkv_draw.c
  src/vkv_transfer.c
  src/vkv_swapchain.c
  src/vkv_ff.c
  src/vkv_query.c
  src/vkv_dispatch.c
)
add_library(VkVita::vkvita ALIAS vkvita)

if(VKV_WITH_SPIRV)
  set(VKV_SPIRV_CROSS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/SPIRV-Cross)
  set(VKV_SPIRV_CROSS_SOURCES
    ${VKV_SPIRV_CROSS_DIR}/spirv_cfg.cpp
    ${VKV_SPIRV_CROSS_DIR}/spirv_cross.cpp
    ${VKV_SPIRV_CROSS_DIR}/spirv_cross_c.cpp
    ${VKV_SPIRV_CROSS_DIR}/spirv_cross_parsed_ir.cpp
    ${VKV_SPIRV_CROSS_DIR}/spirv_glsl.cpp
    ${VKV_SPIRV_CROSS_DIR}/spirv_hlsl.cpp
    ${VKV_SPIRV_CROSS_DIR}/spirv_parser.cpp
  )
  target_sources(vkvita PRIVATE src/vkv_spirv.c src/vkv_spirv_lower.c ${VKV_SPIRV_CROSS_SOURCES})
  set_source_files_properties(${VKV_SPIRV_CROSS_SOURCES} PROPERTIES COMPILE_OPTIONS "-Os;-w")
  set_source_files_properties(src/vkv_spirv.c src/vkv_spirv_lower.c PROPERTIES COMPILE_OPTIONS "-Os")
  target_include_directories(vkvita PRIVATE ${VKV_SPIRV_CROSS_DIR})
  target_compile_definitions(vkvita PRIVATE SPIRV_CROSS_C_API_GLSL=1 SPIRV_CROSS_C_API_HLSL=1)
  target_compile_features(vkvita PRIVATE cxx_std_11)
endif()

target_include_directories(vkvita PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/Vulkan-Headers/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(vkvita PRIVATE c_std_11)
target_compile_options(vkvita PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
target_compile_definitions(vkvita PRIVATE
  VKV_ENABLE_DEBUG=$<BOOL:${VKV_ENABLE_DEBUG}>
  VKV_WITH_SHARK=$<BOOL:${VKV_SHARK}>
  VKV_WITH_SPIRV=$<BOOL:${VKV_WITH_SPIRV}>
)
if(NOT VKV_DEFAULT_DEBUG_FLAGS STREQUAL "")
  target_compile_definitions(vkvita PRIVATE VKV_DEFAULT_DEBUG_FLAGS=${VKV_DEFAULT_DEBUG_FLAGS})
endif()
if(VKV_SHARK)
  target_include_directories(vkvita PRIVATE ${VKV_VITASHARK_INCLUDE})
endif()

# Everything a program linking libvkvita.a needs (order matters for static libs).
set(VKV_LINK_LIBS)
if(VKV_SHARK)
  list(APPEND VKV_LINK_LIBS vitashark SceShaccCgExt taihen_stub SceShaccCg_stub)
endif()
if(VKV_WITH_SPIRV)
  list(APPEND VKV_LINK_LIBS stdc++ pthread)
endif()
list(APPEND VKV_LINK_LIBS
  SceGxm_stub SceDisplay_stub SceSysmem_stub SceIofilemgr_stub SceKernelThreadMgr_stub SceLibKernel_stub m)
target_link_libraries(vkvita INTERFACE ${VKV_LINK_LIBS})

# ---------------------------------------------------------------------------
# Install: lib, headers, CMake package, pkg-config
# ---------------------------------------------------------------------------
install(TARGETS vkvita EXPORT VkVitaTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/vkvita DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(VKV_INSTALL_VULKAN_HEADERS)
  install(DIRECTORY third_party/Vulkan-Headers/include/vulkan third_party/Vulkan-Headers/include/vk_video
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

include(CMakePackageConfigHelpers)
install(EXPORT VkVitaTargets NAMESPACE VkVita:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VkVita)
configure_package_config_file(cmake/VkVitaConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/VkVitaConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VkVita)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/VkVitaConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/VkVitaConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/VkVitaConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VkVita)

list(TRANSFORM VKV_LINK_LIBS PREPEND "-l" OUTPUT_VARIABLE VKV_PC_LIBS)
string(JOIN " " VKV_PC_LIBS ${VKV_PC_LIBS})
configure_file(cmake/vkvita.pc.in ${CMAKE_CURRENT_BINARY_DIR}/vkvita.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vkvita.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# ---------------------------------------------------------------------------
# Samples
# ---------------------------------------------------------------------------
if(VKV_BUILD_SAMPLES)
  add_subdirectory(samples)
endif()
