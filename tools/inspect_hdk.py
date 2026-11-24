"""Lista os cabeçalhos de seção de um arquivo HDK."""

import argparse
from pathlib import Path


def iter_sections(data):
    """Encontra as seções seguindo os comprimentos gravados no arquivo."""
    if not data:
        raise ValueError("arquivo vazio")

    offset = 0
    while offset < len(data):
        header = f"bloco em 0x{offset:04X}"
        name_start = offset + 4
        if name_start > len(data):
            raise ValueError(f"{header}: campo de comprimento truncado")

        # O comprimento inclui os quatro bytes do próprio campo.
        length = int.from_bytes(data[offset:name_start], "little")
        if length < 6:
            raise ValueError(f"{header}: comprimento inválido ({length} bytes)")

        end = offset + length
        if end > len(data):
            raise ValueError(f"{header}: bloco truncado (fim previsto em 0x{end:04X})")

        # Procura o byte zero só dentro deste bloco.
        name_end = data.find(b"\x00", name_start, end)
        if name_end == -1:
            raise ValueError(f"{header}: nome da seção sem terminador")

        name = data[name_start:name_end]
        if not name or not all(0x21 <= byte <= 0x7E for byte in name):
            raise ValueError(f"{header}: nome da seção inválido")

        yield offset, name.decode("ascii")
        offset = end


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("arquivo", type=Path, help="caminho do arquivo HDK")
    args = parser.parse_args()

    try:
        sections = list(iter_sections(args.arquivo.read_bytes()))
    except (OSError, ValueError) as error:
        parser.exit(1, f"Erro ao inspecionar {args.arquivo}: {error}\n")

    for offset, name in sections:
        print(f"0x{offset:04X} {name}")


if __name__ == "__main__":
    main()
