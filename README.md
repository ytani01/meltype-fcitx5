# meltype-fcitx5

> [!IMPORTANT]
> **このリポジトリのコードと文書は、[Claude Code](https://claude.com/claude-code) で作りました。**
> 人間の目でも確認していますが、正直、細部まですべては把握しきれていません。

日本語入力 [Meltype](https://github.com/yksr-melt/Meltype) を fcitx5 で使うためのアドオンです。

Meltype の Linux 版は IBus のエンジンなので、fcitx5 では使えません。
このアドオンは、IBus 版で IBus とやり取りする部分 (Meltype の `linux/ibus-engine-meltype`) を fcitx5 のアドオン (C++) に書き直したものです。
日本語と英語の判定・変換は、IBus 版と同じ本体 (`libMeltypeNative.so`) と Mozc の変換ヘルパー (`mozc/meltype_mozc_helper`) が行います。

```
アプリ ⇄ fcitx5 ⇄ meltype.so (このアドオン)
                      ⇣ dlopen
                  libMeltypeNative.so (本体) → mozc/meltype_mozc_helper (漢字変換)
```

- **Meltype の配布元とは別に作っているものです。** 配布元はこのアドオンに関わっていません。
  不具合はこのリポジトリの Issue に報告してください
- **本体とヘルパーは、配布元のリリースの zip (`Meltype-<版>-linux.zip`) のものを使います。**
  このリポジトリには入っていません。.NET や Mozc のビルドは要りません

## 動作を確認した環境

- CachyOS (Arch Linux 系)
- fcitx5 5.1.23
- Meltype 1.0.4

Ubuntu などではまだ確認していません。

## Arch Linux 系: パッケージでインストールする

`PKGBUILD` で、アドオンと本体をまとめたパッケージ `meltype-fcitx5` を作れます。
本体の zip は `makepkg` が配布元のリリースからダウンロードし、sha256 で照合します。

```bash
git clone https://github.com/ytani01/meltype-fcitx5.git
cd meltype-fcitx5
makepkg -si
```

| インストール先 | 中身 |
| --- | --- |
| `/usr/lib/fcitx5/meltype.so` | アドオン |
| `/usr/share/fcitx5/addon/meltype.conf`、`/usr/share/fcitx5/inputmethod/meltype.conf` | アドオンと入力メソッドの定義 |
| `/usr/lib/meltype-fcitx5/libMeltypeNative.so`、`/usr/lib/meltype-fcitx5/mozc/meltype_mozc_helper` | 本体と変換ヘルパー (zip の中身をそのまま) |
| `/usr/share/licenses/meltype-fcitx5/` | 本体のライセンス |

アンインストールするときは `sudo pacman -R meltype-fcitx5` を実行します。

本体は IBus 版の `install.sh` (`/opt/meltype`) とは別の場所に置くので、IBus 版と並べてインストールできます。

## ほかの環境: ビルドしてインストールする

### 必要なもの

- fcitx5 の開発用ファイル (ヘッダーと CMake の設定)、json-c、cmake、C++20 のコンパイラー
  - Arch Linux: `sudo pacman -S --needed fcitx5 json-c cmake gcc pkgconf`
  - Ubuntu: `sudo apt install libfcitx5core-dev libjson-c-dev cmake g++ pkg-config` (未確認)
- 配布元のリリースの zip (`Meltype-<版>-linux.zip`) を展開したもの

### インストール

このリポジトリのトップディレクトリで実行します。

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

- アドオンのインストール先は fcitx5 の CMake の設定に従います (Arch Linux なら `/usr/lib/fcitx5/meltype.so` と `/usr/share/fcitx5/` の下の 2 つの `meltype.conf`)。
  `-DCMAKE_INSTALL_PREFIX=/usr` を付けないと `/usr/local` の下にインストールされ、fcitx5 から見つかりません
- インストールしたファイルの一覧は `build/install_manifest.txt` に残ります

次に、本体とヘルパーを `/opt/meltype` に置きます。zip を展開したディレクトリ (`Meltype-linux/`) で実行します。

```bash
sudo mkdir -p /opt/meltype
sudo cp -R libMeltypeNative.so mozc LICENSE THIRD-PARTY-NOTICES.md /opt/meltype/
```

アドオンは本体を `/opt/meltype` から読み込みます。別の場所に置くときは、fcitx5 を起動する環境で、環境変数 `MELTYPE_DIR` にそのディレクトリを指定します。

### アンインストール

```bash
sudo xargs rm -v < build/install_manifest.txt
sudo rm -rf /opt/meltype
```

- `/opt/meltype` は IBus 版 (`install.sh`) と同じ場所です。IBus 版も使っているときは、2 行目を実行しないでください
- `build/` を削除していたら、上の「インストール」をもう一度実行してからアンインストールしてください

## 使い方

1. fcitx5 を再起動する (`fcitx5 -r`)
2. fcitx5 の設定 (`fcitx5-configtool`) で、入力メソッドに「Meltype」を追加する (パネルの表示は `M`)

設定と学習データは、IBus 版と同じ `~/.local/share/Meltype/` に作られます。

アンインストールしたあとは fcitx5 を再起動し、設定の入力メソッドから「Meltype」を削除してください。

## ライセンス

GPL-3.0-or-later ([LICENSE](LICENSE))。`meltype.cpp` は Meltype の IBus 版を書き直したものなので、
Meltype の著作権表示を残しています。

本体とヘルパー (配布元の zip) のライセンスは、zip の `LICENSE`、`THIRD-PARTY-NOTICES.md`、
`mozc/MOZC-LICENSE.txt` を見てください。パッケージでインストールしたときは `/usr/share/licenses/meltype-fcitx5/` にあります。
