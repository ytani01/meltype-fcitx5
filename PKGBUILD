# SPDX-License-Identifier: GPL-3.0-or-later
# Meltype の fcitx5 版。アドオンはこのディレクトリのソースからビルドし、
# 本体 (libMeltypeNative.so) と変換ヘルパーは配布 zip のものを /usr/lib/meltype-fcitx5 にインストールし、アドオンもそこを見るようにビルドする。
# IBus 版の install.sh が使う /opt/meltype とは分ける。
pkgname=meltype-fcitx5
pkgver=1.0.4
pkgrel=2
pkgdesc='Meltype input method for fcitx5'
arch=('x86_64')
url='https://github.com/ytani01/meltype-fcitx5'
# 本体に .NET ランタイム (MIT)・辞書 (CC-BY-SA-4.0, Unicode-3.0)、ヘルパーに Mozc (BSD-3-Clause)・Abseil (Apache-2.0)。
# Mozc の辞書 (IPAdic など) は MOZC-CREDITS.html にある
license=('GPL-3.0-or-later' 'MIT' 'BSD-3-Clause' 'Apache-2.0' 'CC-BY-SA-4.0' 'Unicode-3.0')
depends=('fcitx5' 'json-c' 'libstdc++' 'libgcc' 'glibc')
makedepends=('cmake')
# 配布 zip の本体とヘルパーは、sha256 で照合したバイト列のままインストールする
options=('!strip' '!debug')
source=("https://github.com/yksr-melt/Meltype/releases/download/v$pkgver/Meltype-$pkgver-linux.zip"
        'CMakeLists.txt' 'meltype.cpp' 'meltype-addon.conf' 'meltype.conf')
sha256sums=('62b438477d8b982a3a09fe6f5b236df569ff7abd6781c5d13b02395f4789cf4f'
            'SKIP' 'SKIP' 'SKIP' 'SKIP')

# makepkg は source のファイルを src/ にシンボリックリンクで置き、cmake --install はリンクのままインストールするので、実体にする
prepare() {
  local f
  for f in meltype-addon.conf meltype.conf; do
    cp --remove-destination "$(readlink -f "$f")" "$f"
  done
}

build() {
  cmake -B build -S . -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=None \
    -DMELTYPE_DEFAULT_DIR=/usr/lib/meltype-fcitx5
  cmake --build build
}

package() {
  DESTDIR="$pkgdir" cmake --install build

  cd Meltype-linux
  install -Dm755 libMeltypeNative.so "$pkgdir/usr/lib/meltype-fcitx5/libMeltypeNative.so"
  install -Dm755 mozc/meltype_mozc_helper "$pkgdir/usr/lib/meltype-fcitx5/mozc/meltype_mozc_helper"
  install -Dm644 -t "$pkgdir/usr/share/licenses/$pkgname" LICENSE THIRD-PARTY-NOTICES.md \
    mozc/MOZC-LICENSE.txt mozc/MOZC-CREDITS.html
}
