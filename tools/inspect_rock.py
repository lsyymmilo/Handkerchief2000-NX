"""Inspeciona a grade compactada de CRock em um HDK local."""

import argparse
from collections import Counter
from pathlib import Path
import struct

from inspect_hdk import iter_sections


def read_rock(data):
    """Retorna largura, altura e bytes da grade ROCK_0 em ordem de linhas."""
    for offset, name in iter_sections(data):
        if name == "ROCK_0":
            length, = struct.unpack_from("<I", data, offset)
            section = data[offset + 4:offset + length]
            break
    else:
        raise ValueError("seção ROCK_0 ausente")

    header = b"ROCK_0\0CRock\0\0"
    if not section.startswith(header) or len(section) < len(header) + 9:
        raise ValueError("cabeçalho CRock inválido ou truncado")
    width, height, marker = struct.unpack_from("<IIB", section, len(header))
    size = width * height
    if not 0 < size <= 16_000_000 or marker != 0:
        raise ValueError("dimensões ou marcador CRock sem suporte")

    cursor = len(header) + 9
    cells = bytearray()
    values = [1, 1, 2, 4, 8, 16, 0]
    multiplier = 32
    while len(cells) < size:
        if cursor == len(section):
            raise ValueError("fluxo CRock truncado")
        command = section[cursor]
        cursor += 1
        operation, count = command >> 5, (command & 31) + 1
        if operation == 0:
            if cursor == len(section):
                raise ValueError("valor literal CRock truncado")
            value = section[cursor]
            cursor += 1
            values[:6] = [value, *values[:5]]
            multiplier = 32
        elif operation == 7:
            value = values[0]
            count *= multiplier
            multiplier *= 32
        else:
            value = values[operation]
            values[0] = value
            multiplier = 32
        if count > size - len(cells):
            raise ValueError("repetição CRock ultrapassa a grade")
        cells.extend(bytes([value]) * count)

    if cursor != len(section):
        raise ValueError("dados extras após a grade CRock")
    return width, height, bytes(cells)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("arquivo", type=Path)
    args = parser.parse_args()
    try:
        width, height, cells = read_rock(args.arquivo.read_bytes())
    except (OSError, ValueError) as error:
        parser.exit(1, f"Erro ao inspecionar CRock: {error}\n")
    print(f"{width} × {height} células")
    for value, count in sorted(Counter(cells).items()):
        print(f"0x{value:02X} {count}")


if __name__ == "__main__":
    main()
