#!/usr/bin/env python3
"""Записывает в NACP японский языковой слот.

ВАЖНО: массив языковых записей NACP НЕ индексируется по SetLanguage!
Настоящий порядок (libnx, nx/source/runtime/nacp.c, g_nacpLanguageTable;
совпадает с SwitchBrew «Control.nacp Language»):

    слот 0  = AmericanEnglish      (setMakeLanguage(SetLanguage_ENUS) → 0)
    слот 1  = BritishEnglish
    слот 2  = Japanese             ← сюда!
    слот 3  = French
    слот 4  = German
    слот 5  = LatinAmericanSpanish
    слот 6  = Spanish
    слот 7  = Italian
    слот 8  = Dutch
    слот 9  = CanadianFrench
    слот 10 = Portuguese
    слот 11 = Russian
    слот 12 = Korean
    слот 13 = TraditionalChinese
    слот 14 = SimplifiedChinese
    слот 15 = BrazilianPortuguese

Раскладка записи: имя 0x200 UTF-8 NUL-terminated + автор 0x100, всего 0x300,
16 записей с offset 0. nacptool заполняет ВСЕ слоты одинаковыми (английскими)
строками, поэтому достаточно перезаписать слот 2 японскими названием/автором.

Использование: nacp_lang.py <вход.nacp> <выход.nacp>
"""

import sys

ENTRY_SIZE = 0x300
NAME_SIZE = 0x200
AUTHOR_SIZE = 0x100

# (индекс слота, имя, автор)
SLOTS = [
    (2, "Japanese", "東方永夜抄　～ Imperishable Night", "上海アリス幻樂団"),
]

def put(dest: bytearray, offset: int, size: int, text: str) -> None:
    raw = text.encode("utf-8")
    if len(raw) + 1 > size:
        raise SystemExit(f"строка не помещается в слот: {text!r}")
    dest[offset:offset + size] = raw + b"\x00" * (size - len(raw))

def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    with open(sys.argv[1], "rb") as handle:
        nacp = bytearray(handle.read())
    if len(nacp) < 16 * ENTRY_SIZE:
        raise SystemExit(f"файл NACP подозрительно мал: {len(nacp)} байт")

    for slot, language, name, author in SLOTS:
        base = slot * ENTRY_SIZE
        put(nacp, base, NAME_SIZE, name)
        put(nacp, base + NAME_SIZE, AUTHOR_SIZE, author)
        print(f"nacp_lang: слот {slot} ({language}) <- '{name}' / '{author}'")

    with open(sys.argv[2], "wb") as handle:
        handle.write(nacp)

if __name__ == "__main__":
    main()
