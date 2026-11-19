cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(vitassh C)

include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VitaSSH")
set(VITA_TITLEID  "VSSH00004" CACHE STRING "Nine-character title id")
# VitaSDK's vita_create_vpk only accepts the Vita package format ##.##.
# The project's release version lives in the git tag / release asset name
# (v1.0.0, vitassh-1.0.0.vpk); this is the version shown by VitaShell.
set(VITA_VERSION  "01.00")

# Overridable so a probe build can borrow an asset set that is already known to
# install. When the real installer rejects a vpk but Vita3K accepts it, the
# LiveArea resources are the only thing left that differs from a known-good
# build -- param.sfo and the SELF header can both be verified equal by
# inspection, the artwork cannot:
#
#   cmake -DVITASSH_ASSETS="$VITASDK/share/gcc-arm-vita-eabi/samples/hello_world/sce_sys" \
#         -DVITA_TITLEID=VSSH00003 -B build/probe -G Ninja ...
set(VITASSH_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/sce_sys" CACHE PATH "sce_sys directory to package")

# Matches the SDK's hello_world sample exactly. It is not optional in the sense
# that it changes behaviour -- 0 and 1 both mean "no restriction" -- but when an
# install is being rejected the goal is to differ from a known-good artifact in
# as few fields as possible, and this is the one field the sample sets by hand.
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME}
  src/main.c
  src/ui.c
  src/ssh.c
  src/net.c
  src/render.c
  src/input.c
  src/touch.c
  src/keyboard.c
  src/ime.c
  src/clock.c
  src/log.c
  src/config.c
  src/fileio.c
  src/vault.c
  src/vault_platform_ossl.c
  src/term.c
  src/vt.c
  src/font8x8.c
)

target_include_directories(${PROJECT_NAME} PRIVATE src)
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-unused-parameter)

# libssh2 links crypto and zlib privately, which a static link does not carry
# over, so they have to be named here too -- and before the libraries that use
# them. The Sce*_stub archives only supply the syscall trampolines.
target_link_libraries(${PROJECT_NAME}
  ssh2 crypto ssl z pthread
  vita2d png16 jpeg freetype bz2 m
  SceDisplay_stub SceCtrl_stub SceGxm_stub SceSysmodule_stub
  SceNet_stub SceNetCtl_stub SceIme_stub SceAppUtil_stub
  SceAppMgr_stub SceCommonDialog_stub ScePgf_stub SceTouch_stub
)

# The terminal grid and the scrollback ring are static, so they live in the
# module's own segment -- which is why no MEMSIZE is set here. vita-make-fself
# documents the budget as "Normal app: 0, System mode app: 0x1000 - 0x12800",
# so declaring one on a normal app produces an eboot.bin the installer rejects.
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${VITASSH_ASSETS}/icon0.png sce_sys/icon0.png
  FILE ${VITASSH_ASSETS}/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE ${VITASSH_ASSETS}/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE ${VITASSH_ASSETS}/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
