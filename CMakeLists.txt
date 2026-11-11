cmake_minimum_required(VERSION 3.16)
project(taiji_desktop VERSION 1.0.0 LANGUAGES C)

set(TAIJI_BUILD_NUMBER "" CACHE STRING "Build number suffix")
if(NOT TAIJI_BUILD_NUMBER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/BUILD_NUMBER")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/BUILD_NUMBER" TAIJI_BUILD_NUMBER)
  string(STRIP "${TAIJI_BUILD_NUMBER}" TAIJI_BUILD_NUMBER)
endif()
if(TAIJI_BUILD_NUMBER)
  set(TAIJI_VERSION "${PROJECT_VERSION}.${TAIJI_BUILD_NUMBER}")
else()
  set(TAIJI_VERSION "${PROJECT_VERSION}")
endif()
message(STATUS "Taiji version: ${TAIJI_VERSION}")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(TAIJI_INSTALL_SDDM_THEME "Install the Taiji SDDM theme" ON)
option(TAIJI_INSTALL_WAYLAND_SESSION "Install the Wayland session" ON)
option(TAIJI_INSTALL_X11_SESSION "Install the X11 fallback session" ON)
option(TAIJI_BUILD_HOTKEYD "Build the controller hotkey daemon on Linux" ON)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(PNG REQUIRED libpng)
if(UNIX AND NOT APPLE)
  pkg_check_modules(GIO_MOUNT REQUIRED gio-2.0 glib-2.0)
endif()

# ── LVGL (vendored, trimmed to the sources we need) ──────────────────────────
file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lvgl/src/*.c"
)
# macOS/Finder creates duplicate "name 2.c" files; never compile those.
list(FILTER LVGL_SOURCES EXCLUDE REGEX "[ ]2\\.c$")
add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lvgl"
  "${CMAKE_CURRENT_SOURCE_DIR}"
  ${PNG_INCLUDE_DIRS}
)
target_link_libraries(lvgl PUBLIC ${PNG_LIBRARIES})
target_compile_definitions(lvgl PUBLIC
  LV_CONF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h"
)
target_compile_options(lvgl PRIVATE -w)

# ── Application ──────────────────────────────────────────────────────────────
add_executable(taiji
  src/main/main.c
  src/main/app.c
  src/main/apps.c
  src/main/tasks.c
  src/files/files_fs.c
  src/files/smb.c
  src/files/star_icon.c
  src/files/ui_files.c
  src/files/smb_share.c
  src/common/backend.c
  src/common/util.c
  src/common/catalog.c
  src/common/libretro.c
  src/common/config.c
  src/main/desktop.c
  src/main/desktop_watch.c
  src/main/library.c
  src/main/launcher.c
  src/main/system_status.c
  src/main/system_volume.c
  src/main/system_brightness.c
  src/main/perf.c
  src/main/gpu.c
  src/main/network.c
  src/main/bluetooth.c
  src/main/audio.c
  src/common/theme.c
  src/common/theme_auto.c
  src/common/i18n.c
  src/common/system_locale.c
  src/main/external_osk.c
  src/store/store.c
  src/store/store_cache.c
  src/store/ui_store.c
  src/store/icon_index.c
  src/store/appstream_index.c
  src/store/mirror.c
  src/store/backend_pacman.c
  src/store/backend_flatpak.c
  src/main/ui.c
  src/main/ui_home.c
  src/main/ui_games.c
  src/main/ui_apps.c
  src/main/ui_control.c
  src/main/ui_settings.c
  src/main/ui_modal.c
  src/main/ui_keyboard.c
  src/main/ui_gamepad.c
  src/main/ui_overview.c
  src/main/editor.c
  src/main/ui_editor.c
  src/main/oobe.c
  src/main/ui_oobe.c
)

target_include_directories(taiji PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/main"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/files"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/common"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/store"
  ${SDL2_INCLUDE_DIRS}
)
target_link_directories(taiji PRIVATE ${SDL2_LIBRARY_DIRS})
target_link_libraries(taiji PRIVATE lvgl ${SDL2_LIBRARIES} m Threads::Threads z)
target_compile_definitions(taiji PRIVATE
  TAIJI_BUILD_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
  TAIJI_VERSION="${TAIJI_VERSION}"
)
target_compile_options(taiji PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)
if(UNIX AND NOT APPLE)
  target_include_directories(taiji PRIVATE ${GIO_MOUNT_INCLUDE_DIRS})
  target_link_libraries(taiji PRIVATE ${GIO_MOUNT_LIBRARIES})
endif()
if(APPLE)
  target_link_libraries(taiji PRIVATE "-framework CoreFoundation")
endif()

# Copy assets next to the binary for a portable build tree.
add_custom_command(TARGET taiji POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:taiji>/assets"
)

install(TARGETS taiji RUNTIME DESTINATION bin)
install(DIRECTORY assets DESTINATION share/taiji)
install(FILES packaging/taiji.desktop DESTINATION share/applications)
install(FILES assets/images/app_icon.png
        DESTINATION share/icons/hicolor/256x256/apps RENAME taiji.png)

# Session helpers. They are intentionally not on PATH: session entries and the
# hotkey daemon use absolute paths so a user cannot shadow them.
install(PROGRAMS
  scripts/taiji-wayland-session
  scripts/taiji-session-loop
  scripts/taiji-run-app
  scripts/taiji-run-background
  scripts/taiji-return
  scripts/taiji-resume
  scripts/taiji-power
  scripts/taiji-tasks
  scripts/taiji-overview
  scripts/taiji-capture
  scripts/taiji-close-task
  scripts/taiji-task-lib
  scripts/taiji-scale
  DESTINATION libexec/taiji
)
install(FILES packaging/wayland/sway.conf DESTINATION share/taiji)
if(TAIJI_INSTALL_X11_SESSION)
  install(PROGRAMS scripts/taiji-session DESTINATION libexec/taiji)
  install(FILES packaging/x11/openbox-rc.xml DESTINATION share/taiji)
  install(FILES packaging/xsessions/taiji.desktop
          DESTINATION share/xsessions RENAME taiji-x11.desktop)
endif()
if(TAIJI_INSTALL_WAYLAND_SESSION)
  install(FILES packaging/wayland-sessions/taiji.desktop
          DESTINATION share/wayland-sessions RENAME taiji-wayland.desktop)
endif()

if(TAIJI_INSTALL_SDDM_THEME)
  install(DIRECTORY packaging/sddm/themes/taiji/
          DESTINATION share/sddm/themes/taiji)
  install(FILES assets/fonts/TaijiCJK.ttf
          DESTINATION share/sddm/themes/taiji/assets)
  install(FILES assets/images/splash.jpg
          DESTINATION share/sddm/themes/taiji/assets)
endif()

if(UNIX AND NOT APPLE)
  pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
  find_package(X11 REQUIRED)
  add_executable(taiji-osd
    src/main/osd.c
    src/protocol/wlr-layer-shell-unstable-v1-protocol.c
    src/protocol/xdg-shell-protocol.c
    src/common/theme.c
    src/common/i18n.c
    src/common/config.c
    src/common/catalog.c
    src/common/libretro.c
    src/common/util.c
  )
  target_include_directories(taiji-osd PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common"
    ${SDL2_INCLUDE_DIRS}
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${X11_INCLUDE_DIR}
  )
  target_link_libraries(taiji-osd PRIVATE lvgl ${SDL2_LIBRARIES}
    ${WAYLAND_CLIENT_LIBRARIES} ${X11_LIBRARIES} Xext m Threads::Threads)
  target_compile_definitions(taiji-osd PRIVATE
    TAIJI_BUILD_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
    TAIJI_VERSION="${TAIJI_VERSION}"
  )
  target_compile_options(taiji-osd PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)
  install(TARGETS taiji-osd RUNTIME DESTINATION libexec/taiji)

  add_executable(taiji-files
    src/files/files_main.c
    src/files/files_ui.c
    src/files/files_fs.c
    src/files/star_icon.c
    src/common/backend.c
    src/common/theme.c
    src/common/i18n.c
    src/common/config.c
    src/common/catalog.c
    src/common/libretro.c
    src/common/util.c
  )
  target_include_directories(taiji-files PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/files"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common"
    ${SDL2_INCLUDE_DIRS}
  )
  target_link_libraries(taiji-files PRIVATE lvgl ${SDL2_LIBRARIES} m Threads::Threads)
  target_compile_definitions(taiji-files PRIVATE
    TAIJI_BUILD_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
    TAIJI_VERSION="${TAIJI_VERSION}"
  )
  target_compile_options(taiji-files PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)
  install(TARGETS taiji-files RUNTIME DESTINATION libexec/taiji)

  pkg_check_modules(GIO REQUIRED gio-2.0 glib-2.0)
  add_executable(taiji-store
    src/store/store_main.c
    src/store/store_ui.c
    src/store/store.c
    src/store/store_cache.c
    src/store/backend_pacman.c
    src/store/backend_flatpak.c
    src/common/backend.c
    src/common/theme.c
    src/common/theme_auto.c
    src/common/i18n.c
    src/common/config.c
    src/common/catalog.c
    src/common/libretro.c
    src/common/util.c
  )
  target_include_directories(taiji-store PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/store"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common"
    ${SDL2_INCLUDE_DIRS}
  )
  target_link_libraries(taiji-store PRIVATE lvgl ${SDL2_LIBRARIES} m Threads::Threads)
  target_compile_definitions(taiji-store PRIVATE
    TAIJI_BUILD_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
    TAIJI_VERSION="${TAIJI_VERSION}"
  )
  target_compile_options(taiji-store PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)

  add_executable(taiji-smb-helper src/files/smb_helper.c)
  target_compile_options(taiji-smb-helper PRIVATE -Wall -Wextra)
  install(TARGETS taiji-smb-helper RUNTIME DESTINATION libexec/taiji)

  add_executable(taiji-store-helper src/store/pkg_helper.c)
  target_compile_options(taiji-store-helper PRIVATE -Wall -Wextra)
  install(TARGETS taiji-store-helper RUNTIME DESTINATION libexec/taiji)

  add_executable(taiji-files-portal src/files/files_portal.c)
  target_include_directories(taiji-files-portal PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/files"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common"
    ${GIO_INCLUDE_DIRS}
  )
  target_link_libraries(taiji-files-portal PRIVATE ${GIO_LIBRARIES})
  target_compile_options(taiji-files-portal PRIVATE -Wall -Wextra)
  install(TARGETS taiji-files-portal RUNTIME DESTINATION libexec/taiji)
  install(FILES packaging/portal/taiji.portals
          DESTINATION share/xdg-desktop-portal/portals)
  install(FILES packaging/portal/org.freedesktop.impl.portal.desktop.taiji.service
          DESTINATION share/dbus-1/services)

  add_executable(taiji-fbcap src/main/fbcap.c)
  target_compile_options(taiji-fbcap PRIVATE -Wall -Wextra)
  install(TARGETS taiji-fbcap RUNTIME DESTINATION libexec/taiji)
endif()

if(UNIX AND NOT APPLE AND TAIJI_BUILD_HOTKEYD)
  add_executable(taiji-hotkeyd src/main/hotkeyd.c)
  target_compile_options(taiji-hotkeyd PRIVATE -Wall -Wextra)
  install(TARGETS taiji-hotkeyd RUNTIME DESTINATION libexec/taiji)
  install(FILES packaging/systemd/user/taiji-hotkeyd.service
          DESTINATION lib/systemd/user)
  install(FILES packaging/udev/99-taiji-input.rules
          DESTINATION lib/udev/rules.d)
endif()

# ── USB MTP ──────────────────────────────────────────────────────────────────
# taiji-mtprd 是 vendored 的 uMTP-Responder（GPLv3，独立可执行文件，
# 不链接进 taiji 主程序）；taiji-mtpd 是生成配置并定位当前用户主目录的薄封装。
if(UNIX AND NOT APPLE)
  file(GLOB_RECURSE UMTP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/umtp-responder/src/*.c")
  add_executable(taiji-mtprd ${UMTP_SOURCES})
  target_include_directories(taiji-mtprd PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/umtp-responder/inc")
  target_compile_options(taiji-mtprd PRIVATE -w)
  target_link_libraries(taiji-mtprd PRIVATE pthread rt)
  install(TARGETS taiji-mtprd RUNTIME DESTINATION libexec/taiji)

  add_executable(taiji-mtpd src/mtp/taiji_mtpd.c)
  target_compile_options(taiji-mtpd PRIVATE -Wall -Wextra)
  target_link_libraries(taiji-mtpd PRIVATE pthread)
  install(TARGETS taiji-mtpd RUNTIME DESTINATION libexec/taiji)
  install(FILES packaging/systemd/system/taiji-mtpd.service
          DESTINATION lib/systemd/system)
endif()
