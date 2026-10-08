# SPDX-License-Identifier: GPL-3.0-or-later
# Meltype の fcitx5 版 (アドオン)。本体 (libMeltypeNative.so) は実行時に dlopen で読む。
cmake_minimum_required(VERSION 3.16)
project(meltype-fcitx5 CXX)

find_package(Fcitx5Core REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)

add_library(meltype MODULE meltype.cpp)
set_target_properties(meltype PROPERTIES PREFIX "" CXX_STANDARD 20 CXX_VISIBILITY_PRESET hidden)
target_compile_options(meltype PRIVATE -Wall -Wextra)
target_link_libraries(meltype PRIVATE Fcitx5::Core PkgConfig::JSONC ${CMAKE_DL_LIBS})
# 環境変数 MELTYPE_DIR が無いときに本体を探す所。既定は IBus 版の install.sh と同じ /opt/meltype
set(MELTYPE_DEFAULT_DIR "/opt/meltype" CACHE PATH "Directory of libMeltypeNative.so when MELTYPE_DIR is unset")
target_compile_definitions(meltype PRIVATE MELTYPE_DEFAULT_DIR="${MELTYPE_DEFAULT_DIR}")

# インストール先は fcitx5 の CMake の設定に任せる (Arch は /usr/lib/fcitx5、Ubuntu は /usr/lib/x86_64-linux-gnu/fcitx5)。
# fcitx5 と同じ所にインストールするには -DCMAKE_INSTALL_PREFIX=/usr を付ける。
install(TARGETS meltype DESTINATION "${FCITX_INSTALL_ADDONDIR}")
install(FILES meltype-addon.conf DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon" RENAME meltype.conf)
# トレイのアイコン (Icon=) は、本体と同じ所に置いた icon.png を絶対パスで指す
# 出力の名前を変えるのは、ソースの中でビルドしても (cmake .) 元の meltype.conf を上書きしないため
configure_file(meltype.conf meltype-im.conf @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/meltype-im.conf" DESTINATION "${FCITX_INSTALL_PKGDATADIR}/inputmethod" RENAME meltype.conf)
