cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(batteryconsumption C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "BatteryConsumption")
set(VITA_TITLEID  "BATC00001")
set(VITA_VERSION  "01.01")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu11 -O2")

add_executable(${PROJECT_NAME}
  src/main.c
)

target_link_libraries(${PROJECT_NAME}
  vita2d
  png
  jpeg
  z
  m
  c
  SceAppMgr_stub
  SceCommonDialog_stub
  SceCtrl_stub
  SceDisplay_stub
  SceGxm_stub
  SceKernelThreadMgr_stub
  SceNet_stub
  SceNetCtl_stub
  ScePgf_stub
  ScePower_stub
  SceProcessmgr_stub
  SceRtc_stub
  SceSysmodule_stub
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugin/BatteryConsumptionKernel.skprx" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugin/BatteryConsumptionKernel.suprx")
  vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE sce_sys/icon0.png sce_sys/icon0.png
    FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
    FILE plugin/BatteryConsumptionKernel.skprx plugin/BatteryConsumptionKernel.skprx
    FILE plugin/BatteryConsumptionKernel.suprx plugin/BatteryConsumptionKernel.suprx
  )
else()
  message(WARNING "plugin/BatteryConsumptionKernel.skprx or .suprx not found; VPK will be built without plugin payload")
  vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE sce_sys/icon0.png sce_sys/icon0.png
    FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  )
endif()
