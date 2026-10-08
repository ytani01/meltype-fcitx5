#!/usr/bin/env python3
"""Mozc のユーザー辞書を Meltype のユーザー辞書（userdict.txt）へ変換する。

Mozc の ~/.config/mozc/user_dictionary.db（protobuf の UserDictionaryStorage）を読み、
Meltype の ~/.local/share/Meltype/userdict.txt にまだ無い「読み[Tab]単語」だけを末尾に足す。
既存の行は消さない。何度実行しても重複しない。

Meltype が取り込みで飛ばすもの（UserDictionaryFile.cs）はここでも飛ばす:
読みが 2 文字未満、読みにひらがな・英数字・「ー」「・」以外がある、単語が空。
読みのカタカナはひらがなにする。

protobuf のライブラリは使わず、wire format を直接読む（依存を増やさないため）。
"""

import argparse
import sys
from pathlib import Path

SRC = Path.home() / ".config/mozc/user_dictionary.db"
DEST = Path.home() / ".local/share/Meltype/userdict.txt"
MIN_READING_LENGTH = 2


def _varint(b, i):
    n = shift = 0
    while True:
        c = b[i]
        i += 1
        n |= (c & 0x7F) << shift
        shift += 7
        if c < 0x80:
            return n, i


def _fields(b):
    """protobuf のメッセージを (フィールド番号, 値) の列にする。値は varint なら int、それ以外は bytes。"""
    i = 0
    while i < len(b):
        tag, i = _varint(b, i)
        f, t = tag >> 3, tag & 7
        if t == 0:
            v, i = _varint(b, i)
        elif t == 1:
            v, i = b[i : i + 8], i + 8
        elif t == 2:
            n, i = _varint(b, i)
            v, i = b[i : i + n], i + n
        elif t == 5:
            v, i = b[i : i + 4], i + 4
        else:
            raise ValueError(f"wire type {t} は読めない（フィールド {f}）")
        yield f, v


def mozc_entries(storage):
    """UserDictionaryStorage のバイト列から (読み, 単語) を順に返す。

    UserDictionaryStorage.dictionaries = 2、UserDictionary.entries = 4、Entry.key = 1、Entry.value = 2、Entry.pos = 5。
    抑制単語（pos = 44。その変換を出さないための登録）は、Meltype では最優先で出す語になって意味が逆なので飛ばす。
    """
    for f, dic in _fields(storage):
        if f != 2:
            continue
        for g, entry in _fields(dic):
            if g != 4:
                continue
            kv = dict(_fields(entry))
            if kv.get(5) == 44:
                continue
            yield kv.get(1, b"").decode("utf-8"), kv.get(2, b"").decode("utf-8")


def to_hiragana(text):
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in text)


def is_reading_char(c):
    return "ぁ" <= c <= "ゖ" or c in "ーゔ・" or c.isascii() and c.isalnum()


def normalize(reading, word):
    """Meltype に登録できる形なら (読み, 単語)、できなければ None。"""
    reading = to_hiragana(reading.strip())
    word = word.strip()
    if len(reading) < MIN_READING_LENGTH or not word or not all(map(is_reading_char, reading)):
        return None
    if any(c in word for c in "\t\r\n"):
        return None
    return reading, word


def existing_words(dest):
    """userdict.txt にある (読み, 単語)。Meltype と同じく、各欄を strip して比べる。"""
    words = set()
    if not dest.exists():
        return words
    for line in dest.read_text(encoding="utf-8-sig").split("\n"):  # Windows 版の Meltype は BOM 付きで書く
        if line.startswith("#"):
            continue
        parts = line.rstrip("\r").split("\t")
        if len(parts) >= 2:
            words.add((parts[0].strip(), parts[1].strip()))
    return words


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--src", type=Path, default=SRC, help=f"Mozc のユーザー辞書（既定: {SRC}）")
    ap.add_argument("--dest", type=Path, default=DEST, help=f"Meltype のユーザー辞書（既定: {DEST}）")
    ap.add_argument("-n", "--dry-run", action="store_true", help="足す行を標準出力に出すだけで、書かない")
    ap.add_argument("--self-test", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args()
    if args.self_test:
        return _self_test()

    have = existing_words(args.dest)
    added, skipped_rule, skipped_dup = [], 0, 0
    for reading, word in mozc_entries(args.src.read_bytes()):
        pair = normalize(reading, word)
        if pair is None:
            skipped_rule += 1
        elif pair in have:
            skipped_dup += 1
        else:
            have.add(pair)
            added.append(pair)

    lines = "".join(f"{r}\t{w}\n" for r, w in added)
    if args.dry_run:
        sys.stdout.write(lines)
    elif added:
        args.dest.parent.mkdir(parents=True, exist_ok=True)
        old = args.dest.read_bytes() if args.dest.exists() else b""
        sep = "" if not old or old.endswith(b"\n") else "\n"
        with args.dest.open("a", encoding="utf-8") as f:
            f.write(sep + lines)
    print(
        f"足した: {len(added)} 件、飛ばした: 既にある {skipped_dup} 件、Meltype に登録できない形 {skipped_rule} 件"
        + ("（--dry-run なので書いていない）" if args.dry_run else ""),
        file=sys.stderr,
    )
    return 0


def _self_test():
    """wire format の読みと、読みの規則を小さな辞書で確かめる。"""

    def ld(f, b):  # length-delimited（長さは varint）
        n, size = len(b), b""
        while n >= 0x80:
            size, n = size + bytes([n & 0x7F | 0x80]), n >> 7
        return bytes([f << 3 | 2]) + size + bytes([n]) + b

    def entry(k, v, pos=6):
        return ld(4, ld(1, k.encode()) + ld(2, v.encode()) + bytes([5 << 3, pos]))

    dic = ld(2, bytes([1 << 3, 0x85, 0x01]) + ld(3, "辞書".encode()) + entry("", "", 1) + entry("たにばやし", "谷林")
             + entry("チェック", "□", 14) + entry("ー", "x") + entry("**", "**") + entry(" あい ", " 愛 ")
             + entry("だめ", "駄目", 44))
    got = [normalize(r, w) for r, w in mozc_entries(dic)]
    assert got == [None, ("たにばやし", "谷林"), ("ちぇっく", "□"), None, None, ("あい", "愛")], got
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
