"""Gera uma prévia RGBA do primeiro CEarth de VISTA_0 em A0.HDK.

A prévia usa a paleta do BMP e assume ajuste gráfico zero. A câmera e as
alterações de paleta do jogo ficam fora deste experimento.
"""

from pathlib import Path
import struct
import sys
import zlib

from inspect_hdk import iter_sections


ROOT = Path(__file__).resolve().parents[1]
BANK_2_ADDRESS = 0x48B7A8


def read_cearth(data):
    """Lê o cabeçalho e a lista do primeiro objeto na amostra."""
    for offset, name in iter_sections(data):
        if name == "VISTA_0":
            length = int.from_bytes(data[offset:offset + 4], "little")
            section = data[offset + 4:offset + length]
            break
    else:
        raise ValueError("A0.HDK: seção VISTA_0 ausente")

    # Nesta amostra, há 8 bytes entre CGameRoot e o primeiro objeto.
    root_header = b"VISTA_0\0CGameRoot\0"
    record_start = len(root_header) + 8
    marker = b"\xff\xffCEarth\0\x0b\x0b"
    if not section.startswith(root_header) or not section.startswith(marker, record_start):
        raise ValueError("A0.HDK: cabeçalho do primeiro CEarth inesperado em VISTA_0")

    fields = record_start + len(marker)
    _, base_x, base_y, reference_base, count = struct.unpack_from("<IiiIH", section, fields)
    if count != 5:
        raise ValueError(f"A0.HDK: esperados 5 elementos em CEarth, encontrados {count}")
    start = fields + struct.calcsize("<IiiIH")
    end = start + count * 12
    if section[end:end + 4] != b"\0\0\xff\xff":
        raise ValueError("A0.HDK: lista truncada ou fim do primeiro CEarth inesperado")
    return (base_x, base_y, reference_base), list(struct.iter_unpack("<Iii", section[start:end]))


def read_bank(data, address=BANK_2_ADDRESS, next_address=None):
    """Lê uma tabela de retângulos em .data, até o marcador X = -1."""
    if len(data) < 64 or not data.startswith(b"MZ"):
        raise ValueError("HDKWIN.EXE: cabeçalho DOS inválido ou truncado")
    pe, = struct.unpack_from("<I", data, 0x3C)
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("HDKWIN.EXE: cabeçalho PE inválido ou truncado")
    machine, section_count = struct.unpack_from("<HH", data, pe + 4)
    optional_size, = struct.unpack_from("<H", data, pe + 20)
    optional = pe + 24
    if optional_size < 32 or optional + optional_size > len(data):
        raise ValueError("HDKWIN.EXE: cabeçalho opcional inválido ou truncado")
    magic, = struct.unpack_from("<H", data, optional)
    if machine != 0x14C or magic != 0x10B:
        raise ValueError("HDKWIN.EXE: esperado um PE32 x86")

    image_base, = struct.unpack_from("<I", data, optional + 28)
    bank_rva = address - image_base
    section_table = optional + optional_size
    if section_table + section_count * 40 > len(data):
        raise ValueError("HDKWIN.EXE: tabela de seções truncada")

    for number in range(section_count):
        section = section_table + number * 40
        virtual_size, rva, raw_size, raw_offset = struct.unpack_from("<IIII", data, section + 8)
        if data[section:section + 8].rstrip(b"\0") != b".data":
            continue
        relative = bank_rva - rva
        if not 0 <= relative < min(virtual_size, raw_size) or raw_offset + raw_size > len(data):
            raise ValueError(f"HDKWIN.EXE: tabela 0x{address:X} fora dos dados gravados de .data")
        entries = []
        end = raw_offset + min(virtual_size, raw_size)
        start = raw_offset + relative
        if next_address is not None:
            span = next_address - address
            if span < 8 or span % 8 or start + span > end:
                raise ValueError(f"HDKWIN.EXE: limite inválido para a tabela 0x{address:X}")
            end = start + span
        for offset in range(start, end - 7, 8):
            entry = struct.unpack_from("<hhhh", data, offset)
            if entry[0] == -1:
                return entries
            entries.append(entry)
        raise ValueError(f"HDKWIN.EXE: terminador ausente na tabela 0x{address:X}")
    raise ValueError("HDKWIN.EXE: seção .data ausente")


def read_bitmap(data):
    """Lê a paleta e os índices de um BMP de 8 bits sem compressão."""
    if not data.startswith(b"BM"):
        raise ValueError("BMP: assinatura BMP ausente")
    pixel_offset, dib_size, width, height, planes, depth, compression = struct.unpack_from(
        "<IIiiHHI", data, 10
    )
    if dib_size != 40 or planes != 1 or depth != 8 or compression != 0 or width <= 0 or height == 0:
        raise ValueError("BMP: esperado um BMP indexado de 8 bits sem compressão")
    colors, = struct.unpack_from("<I", data, 46)
    colors = colors or 256
    stride = (width + 3) & ~3
    if colors > 256 or 54 + colors * 4 > pixel_offset or pixel_offset + stride * abs(height) > len(data):
        raise ValueError("BMP: paleta ou pixels fora dos limites do arquivo")

    # A quarta componente da paleta BMP é reservada. A chave de cor é
    # aplicada por elemento, comparando o índice antes de consultar a paleta.
    palette = [bytes((r, g, b, 255)) for b, g, r, _ in struct.iter_unpack("<BBBB", data[54:54 + colors * 4])]
    rows = [
        data[start:start + width]
        for start in range(pixel_offset, pixel_offset + stride * abs(height), stride)
    ]
    if height > 0:
        rows.reverse()
    if any(max(row) >= colors for row in rows):
        raise ValueError("BMP: pixel aponta para fora da paleta")
    return palette, rows


def resolve_pieces(origin, elements, bank, rows):
    """Resolve os recortes do banco 2 e suas posições de destino."""
    base_x, base_y, reference_base = origin
    pieces = []
    for reference, x, y in elements:
        group = reference & 0xC0000000
        if reference == 0 or (group and group != (reference_base & 0xC0000000)):
            raise ValueError(f"CEarth: referência inesperada 0x{reference:08X}")
        # O ajuste consultado pelo jogo é assumido como zero nesta prévia.
        reference = (reference + reference_base) & 0xFFFFFFFF
        index = reference & 0x7FFF
        bank_number = (reference >> 26) & 0xF
        if bank_number != 2 or index >= len(bank):
            raise ValueError(f"CEarth: referência 0x{reference:08X} fora do banco 2")
        source_x, source_y, width, height = bank[index]
        if (width <= 0 or height <= 0 or source_x < 0 or source_y < 0
                or source_x + width > len(rows[0]) or source_y + height > len(rows)):
            raise ValueError(f"CEarth: recorte inválido para o índice {index} no BMP")
        pieces.append((index, base_x + x, base_y + y, source_x, source_y, width, height, bool(reference & 0x8000)))
    return pieces


def render_pieces(pieces, palette, rows, expected_size=None):
    """Compõe os recortes na ordem recebida e calcula os limites da imagem."""
    if not pieces:
        raise ValueError("nenhum elemento para desenhar")
    left = min(piece[1] for piece in pieces)
    top = min(piece[2] for piece in pieces)
    width = max(piece[1] + piece[5] for piece in pieces) - left
    height = max(piece[2] + piece[6] for piece in pieces) - top
    if expected_size is not None and (width, height) != expected_size:
        raise ValueError(f"CEarth: limites inesperados para a amostra ({width} × {height})")
    # Coordenadas danificadas podem produzir uma área enorme com poucos elementos.
    if width * height > 16_000_000:
        raise ValueError(f"CEarth: imagem excede o limite da prévia ({width} × {height})")
    pixels = bytearray(width * height * 4)
    for _, x, y, sx, sy, w, h, color_key in pieces:
        for row in range(h):
            target = ((y - top + row) * width + x - left) * 4
            for pixel_index in rows[sy + row][sx:sx + w]:
                if pixel_index != 0 or not color_key:
                    pixels[target:target + 4] = palette[pixel_index]
                target += 4
    return (left, top), width, height, pixels


def compose(origin, elements, bank, palette, rows):
    """Compõe a amostra original de um único CEarth."""
    pieces = resolve_pieces(origin, elements, bank, rows)
    return pieces, *render_pieces(pieces, palette, rows, expected_size=(760, 256))


def encode_png(width, height, pixels):
    """Codifica um PNG RGBA de 8 bits, com filtro zero em cada linha."""
    def chunk(kind, payload):
        body = kind + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))

    stride = width * 4
    scanlines = b"".join(b"\0" + pixels[start:start + stride] for start in range(0, len(pixels), stride))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b"")


def main():
    original = ROOT / "original-data"
    output = ROOT / "build" / "cearth-preview.png"
    try:
        origin, elements = read_cearth((original / "SYSTEM" / "A0.HDK").read_bytes())
        bank = read_bank((original / "HDKWIN.EXE").read_bytes())
        palette, rows = read_bitmap((original / "BITMAP" / "HDKMAPA0.BMP").read_bytes())
        pieces, (left, top), width, height, pixels = compose(origin, elements, bank, palette, rows)
        output.parent.mkdir(exist_ok=True)
        output.write_bytes(encode_png(width, height, pixels))
    except (OSError, ValueError, struct.error) as error:
        print(f"Erro ao gerar a prévia: {error}", file=sys.stderr)
        return 1

    print("Prévia experimental com ajuste gráfico zero e paleta do BMP.")
    for number, (index, x, y, _, _, w, h, _) in enumerate(pieces, 1):
        print(f"{number}. Índice {index}, posição ({x}, {y}), dimensões {w} × {h}")
    print(f"PNG RGBA de {width} × {height}, origem ({left}, {top}), salvo em {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
