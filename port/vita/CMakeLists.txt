# PS Vita build of the recompiled Handkerchief. From the repository root:
#   cmake -S port/vita -B build/hk-vita -DCMAKE_BUILD_TYPE=Release && cmake --build build/hk-vita -j8
# Needs build/hk-recomp/gen (recomp/x86rec.py + recomp/gen_import_stubs.py) and SDL2 in VitaSDK.
# The VPK contains code derived from HDKWIN.EXE: keep it local, never publish it.
cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()
project(handkerchief C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Handkerchief")
set(VITA_TITLEID  "HKRC00010")
set(VITA_VERSION  "00.01")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")   # 365 MB of main memory

get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(GEN "${ROOT}/build/hk-recomp/gen")
file(GLOB GEN_SRC CONFIGURE_DEPENDS "${GEN}/*.c")
file(GLOB PORT_SRC CONFIGURE_DEPENDS "${ROOT}/port/rt/*.c" "${ROOT}/port/win32/*.c" "${ROOT}/port/hk/*.c")
list(APPEND PORT_SRC "${ROOT}/port/third_party/tlsf/tlsf.c" "${ROOT}/port/vita/main.c")
if(NOT GEN_SRC)
  message(FATAL_ERROR "No generated code in ${GEN}: run recomp/x86rec.py first")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu11 -g -fsigned-char -mcpu=cortex-a9 -mfpu=neon -mthumb")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set_source_files_properties(${GEN_SRC} PROPERTIES COMPILE_OPTIONS "-w")

add_executable(handkerchief ${GEN_SRC} ${PORT_SRC})
target_include_directories(handkerchief PRIVATE "${ROOT}/port" "${ROOT}/port/rt" "${ROOT}/port/win32"
                           "${ROOT}/port/third_party/tlsf" "${GEN}")
target_compile_definitions(handkerchief PRIVATE SH3_LOG_PATH="ux0:data/handkerchief/hklog.txt")
find_package(SDL2 REQUIRED)
target_link_libraries(handkerchief SDL2::SDL2 SceDisplay_stub SceCtrl_stub SceTouch_stub SceMotion_stub
  SceAudio_stub SceGxm_stub ScePower_stub SceAppUtil_stub SceCommonDialog_stub SceAppMgr_stub
  SceSysmodule_stub SceHid_stub SceIofilemgr_stub SceProcessmgr_stub m -pthread)

vita_create_self(handkerchief.self handkerchief)
vita_create_vpk(handkerchief.vpk ${VITA_TITLEID} handkerchief.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
  FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
