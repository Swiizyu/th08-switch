#!/usr/bin/env python3
"""Генерирует src/modern/switch/cp932_table.h: односимвольные маппинги
CP932 -> Unicode (мульти-символьные CP932-маппинги выбрасываются — в игре
они не встречаются). Запуск из корня дерева порта."""
import codecs
import os


def main():
    table = []
    # двухбайтовые последовательности CP932
    for lead in range(0x81, 0x100):
        trails = list(range(0x40, 0x100))
        raws = [bytes([lead, t]) for t in trails if t != 0x7F]
        for raw in raws:
            try:
                s = raw.decode('cp932')
            except UnicodeDecodeError:
                continue
            if len(s) == 1:
                table.append(((raw[0] << 8) | raw[1], ord(s)))
    # однобайтовые JIS X 0201 (0xA1-0xDF)
    for b in range(0xA1, 0xE0):
        try:
            s = bytes([b]).decode('cp932')
            if len(s) == 1:
                table.append((b, ord(s)))
        except UnicodeDecodeError:
            pass
    table.sort()

    out = os.path.join(os.path.dirname(__file__), '..', 'src', 'modern', 'switch', 'cp932_table.h')
    with open(out, 'w', encoding='utf-8') as f:
        f.write("// Сгенерировано scripts/gen_cp932.py — не редактировать вручную.\n")
        f.write("// Односимвольные маппинги CP932 -> Unicode, отсортированы по коду CP932.\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("namespace th08_switch {\n")
        f.write("struct Cp932Entry { uint16_t code; uint16_t unicode; };\n")
        f.write("extern const Cp932Entry g_Cp932Table[];\n")
        f.write("const int g_Cp932TableSize = %d;\n" % len(table))
        f.write("const Cp932Entry g_Cp932Table[] = {\n")
        for i in range(0, len(table), 6):
            f.write("    " + ", ".join("{0x%04x, 0x%04x}" % e for e in table[i:i + 6]) + ",\n")
        f.write("};\n}\n")
    print("записей:", len(table))


main()
