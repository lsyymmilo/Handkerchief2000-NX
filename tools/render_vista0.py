"""Gera prévias dos objetos CEarth da seção VISTA_0 de A0.HDK."""

import struct
import sys

from inspect_hdk import iter_sections
from render_cearth import ROOT, encode_png, read_bank, read_bitmap, render_pieces, resolve_pieces


def read_vista_objects(data):
    """Percorre os registros pela contagem e pelo tamanho das listas."""
    for offset, name in iter_sections(data):
        if name == "VISTA_0":
            length, = struct.unpack_from("<I", data, offset)
            section = data[offset + 4:offset + length]
            break
    else:
        raise ValueError("A0.HDK: seção VISTA_0 ausente")

    # Este é o cabeçalho CGameRoot verificado na amostra, até a contagem.
    header = b"VISTA_0\0CGameRoot\0\0\x01\x01\0\0\x01"
    if not section.startswith(header) or len(section) < len(header) + 2:
        raise ValueError("VISTA_0: cabeçalho CGameRoot truncado ou sem suporte")
    count, = struct.unpack_from("<H", section, len(header))
    cursor = len(header) + 2
    fields_size = struct.calcsize("<IiiIH")
    element_size = struct.calcsize("<Iii")
    # Marcador, nome com terminador, dois bytes de controle, campos fixos
    # e contagem de filhos. As listas podem acrescentar bytes a esse mínimo.
    minimum_size = len(b"\xff\xffCEarth\0\x0b\x0b") + fields_size + 2
    if count == 0 or count > (len(section) - cursor) // minimum_size:
        raise ValueError("VISTA_0: contagem de objetos incompatível com o tamanho da seção")

    objects = []
    for number in range(1, count + 1):
        record = f"VISTA_0: objeto {number} em 0x{offset + 4 + cursor:04X}"
        if section[cursor:cursor + 2] != b"\xff\xff":
            raise ValueError(f"{record}: marcador de classe ausente ou truncado")
        name_start = cursor + 2
        name_end = section.find(b"\0", name_start)
        if name_end == -1:
            raise ValueError(f"{record}: nome de classe truncado")
        class_name = section[name_start:name_end]
        if class_name != b"CEarth":
            raise ValueError(f"{record}: classe sem suporte {class_name!r}")
        flags = name_end + 1
        if section[flags:flags + 2] != b"\x0b\x0b":
            raise ValueError(f"{record}: variante de CEarth truncada ou sem suporte")

        fields = flags + 2
        start = fields + fields_size
        if start > len(section):
            raise ValueError(f"{record}: campos de CEarth truncados")
        _, x, y, reference_base, elements = struct.unpack_from("<IiiIH", section, fields)
        end = start + elements * element_size
        # Reserva a contagem de filhos e o tamanho mínimo dos objetos
        # restantes para que a lista não avance sobre seus cabeçalhos.
        remaining = count - number
        if end + 2 + remaining * minimum_size > len(section):
            raise ValueError(f"{record}: lista de elementos excede o espaço disponível")
        children, = struct.unpack_from("<H", section, end)
        if children:
            raise ValueError(f"{record}: {children} objetos aninhados sem suporte")
        objects.append(((x, y, reference_base), list(struct.iter_unpack("<Iii", section[start:end]))))
        cursor = end + 2

    if cursor != len(section):
        raise ValueError("VISTA_0: a contagem de objetos não cobre toda a seção")
    return objects


def load_vista_scene():
    """Carrega os objetos e recortes da amostra."""
    original = ROOT / "original-data"
    objects = read_vista_objects((original / "SYSTEM" / "A0.HDK").read_bytes())
    bank = read_bank((original / "HDKWIN.EXE").read_bytes())
    palette, rows = read_bitmap((original / "BITMAP" / "HDKMAPA0.BMP").read_bytes())
    pieces = []
    for number, (origin, elements) in enumerate(objects, 1):
        try:
            pieces.extend(resolve_pieces(origin, elements, bank, rows))
        except ValueError as error:
            raise ValueError(f"VISTA_0: objeto {number}: {error}") from error
    return len(objects), pieces, palette, rows


def main():
    output = ROOT / "build"
    try:
        object_count, pieces, palette, rows = load_vista_scene()
        (left, top), width, height, pixels = render_pieces(pieces, palette, rows)
        preview = encode_png(width, height, pixels)
        # Substitui só o fundo transparente para preservar os pixels desenhados.
        for offset in range(0, len(pixels), 4):
            if pixels[offset + 3] == 0:
                pixels[offset:offset + 4] = b"\xff\x00\xff\xff"
        debug = encode_png(width, height, pixels)
        output.mkdir(exist_ok=True)
        (output / "vista0-preview.png").write_bytes(preview)
        (output / "vista0-debug.png").write_bytes(debug)
    except (OSError, ValueError, struct.error) as error:
        print(f"Erro ao gerar a prévia: {error}", file=sys.stderr)
        return 1

    print("Prévia em ordem de arquivo, com ajuste gráfico zero e paleta original do BMP.")
    print(f"{object_count} objetos CEarth, 0 sem suporte e {len(pieces)} elementos.")
    print(f"Imagens RGBA de {width} × {height}, origem ({left}, {top}), em {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
