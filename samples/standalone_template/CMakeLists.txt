# Template for your own homebrew project using an installed VkVita.
#   cmake -S . -B build && cmake --build build
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(my_vulkan_game C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "My Vulkan Game")
set(VITA_TITLEID  "MYGAME001")   # exactly 9 characters
set(VITA_VERSION  "01.00")

# Brings in headers (vulkan/vulkan.h, vkvita/vkvita.h) and every stub lib needed.
find_package(VkVita 0.1 REQUIRED)

add_executable(${PROJECT_NAME} main.c)
target_link_libraries(${PROJECT_NAME} PRIVATE VkVita::vkvita SceCtrl_stub)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
)
