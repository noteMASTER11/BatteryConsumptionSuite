cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(BatteryConsumptionUserPrx C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

add_executable(${PROJECT_NAME} src/main.c)

target_link_libraries(${PROJECT_NAME}
  SceLibKernel_stub
)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -nostdlib")
vita_create_self(BatteryConsumptionKernel.suprx ${PROJECT_NAME} UNSAFE CONFIG ${CMAKE_SOURCE_DIR}/main.yml)
