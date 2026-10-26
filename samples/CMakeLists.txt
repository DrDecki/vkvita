# VkVita samples. Built as part of the main project (VKV_BUILD_SAMPLES=ON).
# For your own project, copy samples/standalone_template instead.

include("${VITASDK}/share/vita.cmake" REQUIRED)

add_library(vkv_sample_common STATIC common/vkv_sample.c)
target_include_directories(vkv_sample_common PUBLIC common)
target_link_libraries(vkv_sample_common PUBLIC VkVita::vkvita SceCtrl_stub)

# vkv_add_sample(<name> <LiveArea title> <TITLEID> [USE_COMMON])
function(vkv_add_sample name title titleid)
  cmake_parse_arguments(ARG "USE_COMMON" "" "" ${ARGN})
  add_executable(${name} ${name}/main.c)
  target_compile_options(${name} PRIVATE -O2 -Wall)
  target_link_libraries(${name} PRIVATE VkVita::vkvita SceCtrl_stub)
  if(ARG_USE_COMMON)
    target_link_libraries(${name} PRIVATE vkv_sample_common)
  endif()
  vita_create_self(${name}.self ${name})
  vita_create_vpk(${name}.vpk ${titleid} ${name}.self
    VERSION 01.00
    NAME "${title}"
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  )
endfunction()

vkv_add_sample(hello_triangle "VkVita Triangle" VKVT00001)
vkv_add_sample(textured_cube  "VkVita Cube"     VKVT00002 USE_COMMON)
vkv_add_sample(fixed_function "VkVita FixedFn"  VKVT00003 USE_COMMON)
