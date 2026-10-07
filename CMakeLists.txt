cmake_minimum_required(VERSION 3.14)

if (NOT DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Please define VITASDK env var to point to your SDK path or set CMAKE_TOOLCHAIN_FILE.")
endif ()

set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Toolchain file")

project(pockettanks C CXX)

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Pocket Tanks")
set(VITA_TITLEID "PCKT00001")
set(VITA_VPKNAME "pockettanks")
set(VITA_VERSION "01.00")

# ATTRIBUTE2=12: request the extended memory budget from the OS.
# Without this, the 256MB newlib heap allocation in _init_vita_heap fails,
# the first malloc returns NULL, and the process dies in _malloc_r during
# C++ static init (vitaGL's expression::ptok constructor). Same flag as HCR.
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

set(DATA_PATH "ux0:data/pockettanks/" CACHE STRING "Path to data (with trailing /)")
add_definitions(-DDATA_PATH="${DATA_PATH}")

# Silence FalsoJNI debug logging for release; uncomment for bring-up
# add_definitions(-DFALSOJNI_DEBUGLEVEL=0)

include_directories(
  ${CMAKE_SOURCE_DIR}/source
  ${CMAKE_SOURCE_DIR}/lib
)

add_executable(${PROJECT_NAME}
  source/main.c
  source/ptree_fake.cpp
  source/utils.c
  source/bootlog.c
  source/egl_graphics.c
  source/jni_bridge.c
  source/fake_motionevent.c
  source/asset_mgr.c
  source/sound.c
  source/input.c
  source/dynlib.c
  source/android_log.c
  source/bionic_compat.c
  source/bionic_ctype.c
  source/bionic_pthread.c
  source/gl_wrappers.c
  lib/falso_jni/FalsoJNI.c
  lib/falso_jni/FalsoJNI_ImplBridge.c
  lib/falso_jni/FalsoJNI_Logger.c
  lib/falso_jni/converter.c
  lib/so_util/so_util.c
)

target_link_libraries(${PROJECT_NAME}
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  vitaGL
  vitashark
  mathneon
  SceShaccCgExt
  kubridge_stub
  taihen_stub
  SceShaccCg_stub
  z
  m
  stdc++
  SceAppMgr_stub
  SceAppUtil_stub
  SceAudio_stub
  SceCommonDialog_stub
  SceCtrl_stub
  SceDisplay_stub
  SceGxm_stub
  SceKernelDmacMgr_stub
  ScePower_stub
  SceSysmem_stub
  SceRtc_stub
  SceTouch_stub
  SceMotion_stub
)

target_link_options(${PROJECT_NAME} PRIVATE -Wl,--allow-multiple-definition)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE NOASLR)
vita_create_vpk(${VITA_VPKNAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_SOURCE_DIR}/extras/livearea/icon0.png sce_sys/icon0.png
  ${CMAKE_SOURCE_DIR}/extras/livearea/pic0.png sce_sys/pic0.png
  ${CMAKE_SOURCE_DIR}/extras/livearea/bg.png sce_sys/livearea/contents/bg.png
  ${CMAKE_SOURCE_DIR}/extras/livearea/startup.png sce_sys/livearea/contents/startup.png
  ${CMAKE_SOURCE_DIR}/extras/livearea/template.xml sce_sys/livearea/contents/template.xml
)
