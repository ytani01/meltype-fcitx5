# meltype-fcitx5

> [!WARNING]
> **このアドオンの開発はやめました。このリポジトリはアーカイブしてあります。**
> Meltype 1.1.0 から、配布元が fcitx5 のアドオン (`linux/fcitx5/`) を同梱しています。
> fcitx5 で使うときは、[配布元のリリース](https://github.com/yksr-melt/Meltype/releases) の手順で入れてください。
> このアドオンを入れている場合は、先に `sudo pacman -R meltype-fcitx5` で外してください (同じ場所に同じ名前で入るため)。

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

- **Meltype の配布元とは別に作ったものです。** 配布元はこのアドオンに関わっていません
- **本体とヘルパーは、配布元のリリースの zip (`Meltype-<版>-linux.zip`) のものを使います。**
  このリポジトリには入っていません。.NET や Mozc のビルドは要りません
- **このアドオンを入れた PC では、配布元の zip の `install.sh`・`uninstall.sh` を実行しないでください。**
  Meltype 1.1.0 から、`install.sh` は fcitx5 があると配布元の fcitx5 のアドオンを入れます。
  このアドオンと同じ場所に同じ名前で入るので、このアドオンが上書きされます (`uninstall.sh` は消します)。
  IBus 版だけを使うつもりでも同じです

## 動作を確認した環境

- CachyOS (Arch Linux 系)
- fcitx5 5.1.23
- Meltype 1.1.0

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
| `/usr/lib/meltype-fcitx5/libMeltypeNative.so`、`/usr/lib/meltype-fcitx5/mozc/meltype_mozc_helper`、`/usr/lib/meltype-fcitx5/icon.png` | 本体と変換ヘルパー、パネル (トレイ) のアイコン (zip の中身をそのまま) |
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
sudo cp -R libMeltypeNative.so mozc icon.png LICENSE THIRD-PARTY-NOTICES.md /opt/meltype/
```

`icon.png` はパネル (トレイ) に出すアイコンです。

アドオンは本体を `/opt/meltype` から読み込みます。別の場所に置くときは、ビルドするときに `-DMELTYPE_DEFAULT_DIR=<その場所>` を付けます。
環境変数 `MELTYPE_DIR` でも本体の場所を変えられますが、アイコンはビルドしたときの場所を指したままです。

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

## 変換中のキーの割り当てを変える

変換中 (未確定の文字があるとき) に押したキーを、別のキーに置き換えて Meltype に渡せます。
割り当ては `~/.config/meltype-fcitx5/keymap.conf` (`$XDG_CONFIG_HOME` があればその下) に、1 行 1 組で書きます。

```ini
# 押すキー = 置き換え先のキー
Control+n = Down
Control+p = Up
Control+g = Escape
Control+m = Return
```

- キーの書き方は fcitx5 の設定と同じです (`Control+n`、`Alt+v`、`Page_Up`)。英字の大文字と小文字はどちらで書いても同じです
- 置き換え先として意味があるのは、変換中に Meltype が扱う次のキーです。Meltype が扱わないキーに置き換えると、
  未確定の文字を確定してから、押したキーがアプリに渡ります

  | 置き換え先 | 変換中にすること |
  | --- | --- |
  | `space` | 変換・次の候補 |
  | `Shift+space` | ローマ字として変換・次の候補 |
  | `Down` / `Up` | 次の候補 / 前の候補 |
  | `Right` / `Left` | 次の文節 / 前の文節 |
  | `Shift+Right` / `Shift+Left` | 文節を伸ばす / 縮める |
  | `Return` | 確定 |
  | `BackSpace` | 1 文字消す・変換の取り消し |
  | `Escape` | 取り消し |
  | `Tab` | もしかして (書き間違いを直す) |
  | `F6` / `F7` / `F9` / `F10` | ひらがな / カタカナ / 全角英数 / 半角英数 |

  変換する前に矢印キーを押すと、変換してから文節を選びます。`Escape` は、変換する前なら打った文字を消し、
  変換したあとならかなに戻します
- 割り当てを足しても、元のキーの働きは残ります (`Control+n = Down` と書いても `Down` は次の候補のままです)。
  元のキーを Meltype に使わせないときは、置き換え先を空にします。そのキーは変換中は Meltype に渡らず、
  未確定の文字を残したままアプリに渡ります

  ```ini
  Down =
  ```
- ファイルが無いときは何も置き換えません。変換中でないときも置き換えません (アプリのキー操作はそのまま使えます)
- `#` で始まる行と空行は無視します。読めない行は飛ばし、fcitx5 のログに行番号を出します
- 書き換えたら fcitx5 を再起動 (`fcitx5 -r`) して読み直します。`fcitx5-remote -r` では読み直しません
- fcitx5 の設定 (`fcitx5-configtool`) の Meltype の設定画面でも変えられます。画面には上の表の置き換え先が
  「次の候補（Down）」のように並び、それぞれに押すキーを登録します。置き換え先を空にするキーは
  「本体に渡さずアプリに渡す」の欄に登録します。画面とファイルは同じ内容で、
  画面から保存するとファイルを書き直します。そのとき、ファイルのコメントと空行は消えます。
  表に無い置き換え先の行は画面に出ませんが、保存しても消えずに残ります

## Mozc のユーザー辞書を変換する

Meltype のユーザー辞書 (`~/.local/share/Meltype/userdict.txt`) は、fcitx5-mozc のユーザー辞書とは別です。
fcitx5-mozc に登録してある語は、`tools/mozc-userdict-to-meltype.py` で Meltype のユーザー辞書へ変換できます (Python 3 だけで動きます)。
このリポジトリのトップディレクトリで実行します。

```bash
python3 tools/mozc-userdict-to-meltype.py -n   # 足す行を表示するだけ
python3 tools/mozc-userdict-to-meltype.py      # userdict.txt の末尾に足す
```

- `~/.config/mozc/user_dictionary.db` を読みます。別の場所にあるときは `--src` / `--dest` で指定します
- `userdict.txt` に**まだ無い語だけ**を足します。既存の行は消さないので、何度実行しても重複しません
- Meltype が取り込まない語 (読みが 1 文字、読みにひらがな・英数字・「ー」「・」以外がある) と、Mozc の抑制単語は変換しません
- 足した語は、そのあとに開いた入力欄から使えます。開いたままの入力欄では、アプリを開き直してください

## ライセンス

GPL-3.0-or-later ([LICENSE](LICENSE))。`meltype.cpp` は Meltype の IBus 版を書き直したものなので、
Meltype の著作権表示を残しています。

本体とヘルパー (配布元の zip) のライセンスは、zip の `LICENSE`、`THIRD-PARTY-NOTICES.md`、
`mozc/MOZC-LICENSE.txt` を見てください。パッケージでインストールしたときは `/usr/share/licenses/meltype-fcitx5/` にあります。
