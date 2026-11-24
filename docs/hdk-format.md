# Formato HDK

Esta nota descreve a estrutura observada em `A0.HDK`, `B0.HDK` e
`A0_A.HDK`, em `original-data/SYSTEM/`.

## Estrutura confirmada

Cada arquivo começa no deslocamento zero e contém dez blocos consecutivos.
Os quatro primeiros bytes de cada bloco guardam seu comprimento como um
inteiro de 32 bits em little-endian (byte menos significativo primeiro).
O comprimento inclui esses quatro bytes. Assim, o deslocamento inicial
somado ao comprimento aponta para o início do próximo bloco ou para o fim
do arquivo. Em seguida vêm o rótulo da seção e o nome da classe, cada qual
terminado por um byte zero.

Por exemplo, em `A0.HDK`, `ROCK_0` começa em `0x0059`. Os bytes
`70 06 00 00` indicam comprimento `0x0670`, e a soma dá `0x06C9`,
onde começa `VISTA_0`.

## Seções e deslocamentos

Os deslocamentos são hexadecimais e apontam para o campo de comprimento.

| Seção | Classe | A0.HDK | B0.HDK | A0_A.HDK |
|---|---|---:|---:|---:|
| `FLUG_0` | `CFListStage` | `0x0000` | `0x0000` | `0x0000` |
| `ROCK_0` | `CRock` | `0x0059` | `0x005B` | `0x0055` |
| `VISTA_0` | `CGameRoot` | `0x06C9` | `0x0711` | `0x093E` |
| `STAGE_0` | `CGameRoot` | `0x0921` | `0x08ED` | `0x0A3D` |
| `FRONT_0` | `CGameRoot` | `0x1E41` | `0x1B9C` | `0x1FEC` |
| `GIMK_0` | `CGimkRoot` | `0x334F` | `0x3030` | `0x3624` |
| `EXIT_0` | `CGimkRoot` | `0x350C` | `0x3195` | `0x3703` |
| `ITEM_0` | `CGimkRoot` | `0x35F7` | `0x3231` | `0x379D` |
| `ENEMY_0` | `CMarkRoot` | `0x3615` | `0x324F` | `0x37BB` |
| `OUT_0` | `CMarkRoot` | `0x3B8C` | `0x3503` | `0x3CF7` |

## Validação dos limites

Cada rótulo terminado em zero foi localizado independentemente do campo de
comprimento e apareceu uma única vez por arquivo. Para cada bloco, o fim
calculado coincidiu com o início do seguinte. O fim calculado de
`OUT_0` coincidiu com o tamanho real do arquivo nas três amostras.

| Arquivo | Blocos validados | Fim exclusivo | Tamanho em bytes |
|---|---:|---:|---:|
| `A0.HDK` | 10 | `0x3C43` | 15.427 |
| `B0.HDK` | 10 | `0x357C` | 13.692 |
| `A0_A.HDK` | 10 | `0x3D52` | 15.698 |

Os 30 blocos cobrem integralmente os três arquivos, sem sobreposições,
lacunas ou bytes restantes.

## Validação do conjunto de arquivos

O conjunto em `original-data/` contém 108 arquivos `.HDK`. Os 99 arquivos
com conteúdo passaram na validação estrutural externa com
`tools/inspect_hdk.py`.

Os nove arquivos de `original-data/USER/USER02.HDK` até
`original-data/USER/USER10.HDK` têm zero bytes e foram corretamente
rejeitados pelo leitor com a mensagem `arquivo vazio`.

A validação cobre os limites dos blocos e os nomes das seções. A
integridade dos registros internos e a função dos arquivos `USER`
continuam sem confirmação.

## Hipóteses sobre a serialização interna

- **Contagem e marcadores de registros.** Em `OUT_0`, os dois bytes
  antes do primeiro `FF FF` valem `05 00`, `03 00` e `02 00`, na
  ordem da tabela. Esses valores coincidem com as ocorrências de `COut`.
  O campo pode ser uma contagem de objetos de 16 bits em little-endian.
  `FF FF` também antecede nomes como `CCastMK` e pode marcar o início
  de um registro tipado. Comparar `OUT_0` e `EXIT_0` ajuda a testar se
  a contagem e os limites explicam todo o conteúdo dos blocos.
- **Organização de `CRock`.** Há repetições como `27 D7 F0 E0` em
  `A0.HDK`, `27 D7 EB E0` em `B0.HDK` e `27 C7 E4 E0` em
  `A0_A.HDK`. A associação com células, linhas ou sequências
  compactadas ainda precisa explicar os campos iniciais e o tamanho dos
  dados nas três amostras.
- **Campos e relações entre classes.** Os nomes de atores e controladores
  próximos a `CCastMK`, como `CAPistol` e `CCtrlChase`, podem ser
  sub-registros ou referências. A comparação de registros repetidos,
  começando por `COut`, `CExit` e `CPlate`, pode revelar campos
  fixos, textos variáveis e possíveis coordenadas.
- **Conteúdo de `FLUG_0`.** O bloco termina com `EOF` seguido de byte
  zero nos três arquivos. O significado de nomes como `skey80a`,
  `w40a`, `skey160a` e `def` segue em aberto.

A identificação da engine e a viabilidade do port para PS Vita seguem
em aberto.

## A0.HDK, STAGE_0 (issue #6)

A investigação da Etapa 82 usa a cópia local de `A0.HDK` e a revisão
da Etapa 81 informada pelo coordenador. A janela de referência é
`[820, 884) × [508, 572)`, escolhida com 32 unidades em cada direção
a partir do ponto candidato `(852, 540)`. A janela serve à análise
gráfica. A posição inicial, a colisão e a física seguem em aberto.

### Limites e classes

`STAGE_0` ocupa `[0x0921, 0x1E41)` (comprimento `0x1520`).
O campo de contagem em `0x093D` vale 60. A busca delimitada por
marcadores de classe encontrou 60 ocorrências, sendo 51 `CEarth`,
um `CEarthFluct`, sete `CPlate` e um `CNumPlate`. Nos 51 registros
`CEarth`, os controles são `0B 0B`. Cada lista começa após os campos
`<IiiIH`, contém elementos `<Iii` de 12 bytes e termina com contagem de
filhos zero imediatamente antes do marcador seguinte. As bases de
referência dos 51 registros são zero.

Dos 284 elementos `CEarth`, 281 apontam para o banco 2 antes do ajuste,
dois para o banco 3 e um para o banco 1. Os três últimos têm posições
de destino com X ≥ 3936, à direita da janela. Os índices brutos cabem
nas respectivas tabelas (245, 715 e 98 entradas para os bancos 1, 2 e
3). No banco 2, todos os recortes brutos cabem no BMP de 640 × 1128.
A tabela do banco 3 começa em `0x48CE08`, endereço passado ao
registrador do descritor `0x4ABDAC`, conforme o
operando imediato no offset `0x50D8C` do PE.

### Recortes que cruzam a janela antes do ajuste

Os offsets são absolutos em `A0.HDK`. O destino foi calculado como
posição do objeto mais deslocamento local; a origem de destino vem
desses campos, enquanto seu tamanho depende da referência efetiva.
`Src` usa `(x, y, largura, altura)`; `Dest` usa
`(esquerda, topo, direita, fundo)` com bordas finais exclusivas.
Os seis resultados usam a tabela bruta do banco 2, equivalente a
simular ajuste zero. Um recorte só entra na lista se houver área de
interseção positiva em ambos os eixos.

| Objeto | Elemento | Referência bruta | Banco/índice | Seletor | Src bruto | Dest com tamanho bruto |
|---|---|---|---|---:|---|---|
| `0x0F13` | `0x0F3C` | `0x08198229` | 2/553 | 25 | (480, 240, 160, 120) | (680, 432, 840, 552) |
| `0x0F13` | `0x0F48` | `0x08198229` | 2/553 | 25 | (480, 240, 160, 120) | (868, 428, 1028, 548) |
| `0x1041` | `0x105E` | `0x08198225` | 2/549 | 25 | (480, 0, 160, 120) | (764, 420, 924, 540) |
| `0x1041` | `0x1076` | `0x081A820E` | 2/526 | 26 | (0, 784, 80, 40) | (816, 516, 896, 556) |
| `0x15E9` | `0x165A` | `0x081A81F6` | 2/502 | 26 | (0, 672, 80, 16) | (776, 532, 856, 548) |
| `0x15E9` | `0x1666` | `0x081A81F6` | 2/502 | 26 | (0, 672, 80, 16) | (856, 532, 936, 548) |

As origens dos objetos são `(956, 488)` em `0x0F13`,
`(860, 472)` em `0x1041` e `(588, 580)` em `0x15E9`.
Em particular, `0x1076` guarda o deslocamento `(-44, 44)`,
resultando na origem de destino `(816, 516)`.

O [mapa do motor](engine-map.md#associação-com-recursos-gráficos)
documenta dois caminhos. O cálculo de limites consulta banco e índice
após somar a base. O desenho soma também o valor selecionado na tabela
`0x4ABED4`. A leitura indexada seguida de soma pode ser conferida no
PE no offset `0x89E7` (endereço virtual `0x4095E7` no caminho de desenho de
`CEarth`). O offset `0x8489` corresponde a outra leitura da mesma tabela. Os slots 25 e 26 ficam em `0x4ABF38` e
`0x4ABF3C`, na área de `.data` sem bytes gravados no PE
(a parte gravada termina em `0x4AA200`, com base de imagem `0x400000`).
A leitura local do arquivo deixa em aberto o valor no momento do desenho.
Esse valor pode alterar banco, índice, dimensões e chave de cor. A
lista é um resultado pré-ajuste e a cobertura visual efetiva permanece
indeterminada. Por isso a entrega é documental.

### Sensibilidade dos seis recortes ao ajuste

A tabela abaixo compara três valores hipotéticos do ajuste, somados à
referência antes da escolha do recorte. As células mostram a origem no
BMP e o tamanho em pixels. A ordem das linhas é por seletor e índice.

| Seletor / índice bruto | Ajuste 0 | Ajuste +1 | Ajuste +2 |
|---|---|---|---|
| 25 / 549 | (480,0) 160×120 | (480,120) 160×120 | (440,480) 100×144 |
| 25 / 553 | (480,240) 160×120 | (480,360) 160×120 | (240,624) 216×88 |
| 26 / 502 | (0,672) 80×16 | (80,672) 80×16 | (160,672) 80×16 |
| 26 / 526 | (0,784) 80×40 | (80,784) 80×40 | (160,784) 80×40 |

Os seis elementos usam quatro índices distintos. O ajuste +1 preserva
todos os tamanhos e muda todas as origens no BMP. Portanto, conferir
apenas dimensões ou interseção com a janela deixa ambígua a seleção de
pixels. Com +2, o elemento em `0x0F3C` passaria de (680,432,840,552)
para (680,432,896,520). Esses exemplos medem o efeito da soma. O valor
efetivo e a ordem temporal dos ajustes continuam em aberto.

Para reproduzir na raiz, lendo a cópia local:

```sh
PYTHONPATH=tools python3 -B - <<'PYBANK'
from pathlib import Path
from render_cearth import read_bank

bank = read_bank(Path("original-data/HDKWIN.EXE").read_bytes())
for index in (549, 553, 502, 526):
    print(index, bank[index:index + 3])
PYBANK
```

A menor evidência que permitiria avançar é localizar quem escreve nos
slots 25 e 26 durante a preparação ou atualização da fase e identificar
o valor gravado. Na falta dessa evidência, a rodada usa a vegetação
validada e mantém sua geometria provisória.

### Registro CEarthFluct

O marcador em `0x093F` antecede um trecho com controles `0B 0B`
em `0x096C`. Interpretado provisoriamente pela estrutura de
`CEarth`, ele daria posição `(5960, 360)`, base `0x40000000` e
seis elementos em `[0x0980, 0x09C8)`, seguidos de dois bytes zero até
o próximo marcador em `0x09CA`. O primeiro elemento tem referência
bruta `0x081A8202` (banco 2, índice bruto 514, seletor 26).
A estrutura anterior aos controles e o tratamento da posição e da base
pela classe ainda precisam de confirmação. Esse trecho ficou fora da
seleção geométrica de `CEarth`.

### Reprodução local

Na raiz do projeto, o trecho abaixo lê os arquivos originais e imprime
os recortes brutos que cruzam a janela. Os limites das listas são
conferidos usando os marcadores como verificação independente.

```sh
PYTHONPATH=tools python3 -B - <<'MAPA'
from pathlib import Path
import re
import struct

from inspect_hdk import iter_sections
from render_cearth import read_bank, read_bitmap

original = Path("original-data")
data = (original / "SYSTEM/A0.HDK").read_bytes()
sections = {name: offset for offset, name in iter_sections(data)}
start, end = sections["STAGE_0"], sections["FRONT_0"]
assert (start, end) == (0x0921, 0x1E41)
records = list(re.compile(rb"\xff\xff(C[A-Za-z0-9]+)\0").finditer(data, start, end))
assert len(records) == struct.unpack_from("<H", data, start + 28)[0] == 60
exe = (original / "HDKWIN.EXE").read_bytes()
addresses = (0x48AFF8, 0x48B7A8, 0x48CE08, 0x48D120)
banks = {i: read_bank(exe, a, b) for i, (a, b) in
         enumerate(zip(addresses, addresses[1:]), 1)}
_, rows = read_bitmap((original / "BITMAP/HDKMAPA0.BMP").read_bytes())
for record, following in zip(records, records[1:]):
    if record[1] != b"CEarth":
        continue
    fields = record.end() + 2
    assert data[record.end():fields] == b"\x0b\x0b"
    _, x, y, base, count = struct.unpack_from("<IiiIH", data, fields)
    first = fields + 18
    stop = first + 12 * count
    assert base == 0 and stop + 2 == following.start()
    assert data[stop:stop + 2] == b"\0\0"
    for offset in range(first, stop, 12):
        ref, dx, dy = struct.unpack_from("<Iii", data, offset)
        bank, index = (ref >> 26) & 15, ref & 0x7FFF
        assert 0 < ref < 0x40000000 and index < len(banks[bank])
        sx, sy, w, h = banks[bank][index]
        left, top = x + dx, y + dy
        if bank == 2:
            assert 0 <= sx < sx + w <= len(rows[0])
            assert 0 <= sy < sy + h <= len(rows)
        if left < 884 and top < 572 and left + w > 820 and top + h > 508:
            print(f"{record.start():04X} {offset:04X} {ref:08X}",
                  bank, index, (ref >> 16) & 63, (sx, sy, w, h),
                  (left, top, left + w, top + h))
MAPA
```

### Decodificação de ROCK_0

Em `0x40B938`, o leitor consulta o nome `CRock`. No ramo de
leitura, `0x40B993` e `0x40B99A` leem dois inteiros de 32 bits
para os campos `+0x0C` e `+0x10`. O produto define o tamanho
da grade de bytes. A chamada em `0x40B9F3` chega a `0x40A73E`
por `0x401B04`. Esse caminho lê um marcador zero e chama o
decodificador em `0x40A775`. A instrução em `0x40C55C` avança
pela largura ao percorrer uma coluna, confirmando a ordem por linhas.

Em `0x40A824–0x40A8D5`, os cinco bits inferiores de cada comando
guardam a contagem menos um. Os três superiores selecionam um
literal, um valor já disponível na tabela local ou a repetição
com multiplicador de 32. Repetições consecutivas multiplicam
novamente por 32. O leitor local valida tamanho, truncamento e
consumo integral da seção para a variante observada.

```sh
python3 -B tools/inspect_rock.py original-data/SYSTEM/A0.HDK
```

A0 decodificou em 1600 × 160 células, B0 em 1440 × 200 e A0_A em
1200 × 240. Em A0, foram encontrados os valores 0, 1, 4, 8 e 16.
O código em `0x40C531–0x40C578` examina colunas e registra uma
linha a partir de uma sequência de cinco células diferentes de
zero. Isso fornece um ponto de entrada para comparar o perfil
com as inclinações visíveis na gravação de gameplay.
A escala na tela, o significado das marcas e a ligação com a
resposta de colisão do personagem seguem em aberto.

#### Perfil por coluna e consulta vertical de CRock

O corpo x86 de `0x40C4F2–0x40C57D` recebeu as grades reais de
A0, B0 e A0_A em Unicorn. Somente o alocador em `0x4029F0` foi
modelado. A região de saída foi preenchida antes da execução
com um valor sentinela. A comparação independente procurou,
em cada coluna, a primeira sequência de cinco células não nulas.
O resultado coincidiu em todas as 4240 colunas das três grades.
Em uma grade sintética de três colunas, as duas com uma sequência
válida receberam a coordenada inicial e a terceira preservou o
sentinela. O perfil de A0 tem valores de 0 a 150 em unidades de
célula.

Há uma consulta independente em `0x40BB19–0x40BB77` que divide
X e Y por quatro via deslocamento aritmético e lê a grade em
`CRock+0x08`. O quarto argumento fornece uma máscara de bits.
Se a célula inicial atende à máscara, a função procura para
cima até a primeira célula que não atende e retorna a borda.
Se a célula inicial não atende, procura para baixo a primeira
célula correspondente. Sem transição, preserva Y. O resultado
sempre preserva X. A prova em Unicorn, com os dados de A0,
coincidiu com o modelo independente nas 432 combinações de
nove valores X, oito valores Y e seis máscaras.

O chamador `0x42D856` lê a máscara em `[objeto+0x54]+0x04`.
Em `0x42D86B`, obtém a grade de `[[objeto+0x48]+0x3C]+0x6C`
e chama a consulta pelo salto `0x404089`. A prova confirma uma
consulta de geometria, em coordenadas de quatro unidades por
célula. A associação com o protagonista, a máscara de piso em
uso e a consequência durante a atualização de Y ainda precisam
ser rastreadas. A gravação de gameplay serve para escolher
trechos com transições e inclinações para essa comparação.

#### Chamadores e efeito do ponto retornado

A consulta `0x42D856` é alcançada pelo salto `0x403B43`.
Na rotina que começa em `0x419C4F`, EBX recebe o objeto em
`0x419C59`. Em `0x41A469–0x41A477`, um despacho pelo
salto `0x4045F7 → 0x41C877` recebe o retângulo
`[objeto+0x30]` e grava o objeto retornado em `+0x110`.
Esse endereço de despacho já foi validado no Vita com uma
estrutura sintética, conforme o plano de recompilação.

Quando o retorno do despacho é diferente de zero,
`0x41A485–0x41A496` passa `[EBX+0x18]` e `[EBX+0x1C]`
como X e Y à consulta de terreno. O ponto retornado vai para
`[EBP-0x8C]` e `[EBP-0x88]`. `0x41A49E` carrega o Y
temporário e `0x41A4A4` compara com `[EBX+0x34]`.
Em ramos como `0x41A4FE–0x41A50A`, o ponto é passado à
rotina de inicialização `0x40259F → 0x415DA2`. O caminho que
chega a `0x41A84A–0x41A857` encaminha o objeto produzido à
inserção `0x402275`. O segundo chamador direto, em `0x42D268`,
também usa o ponto como entrada para verificações e criação
condicional de outro objeto.

A mesma rotina `0x419C4F` contém um caminho distinto em
`0x41A0B0–0x41A0DF`: copia o par `[ESI]` e `[ESI+0x04]`
para `[EBX+0x18]` e `[EBX+0x1C]` e chama
`0x401D7F → 0x410E48` para recalcular os limites.
O ramo anterior calcula diferenças entre esse registro e
a posição corrente. A origem do registro em ESI e a classe
concreta do receptor precisam ser rastreadas. O Y temporário
da consulta em `0x41A496` ainda não foi ligado à escrita
da origem do protagonista.

Reproduzir com `objdump -d -Mintel` no `HDKWIN.EXE` local,
nos intervalos `0x419C4F–0x419C65`, `0x41A0B0–0x41A0E4`,
`0x41A469–0x41A50F`, `0x41A84A–0x41A85C` e
`0x42D268–0x42D31D`.

O rastreio posterior identificou o registro de origem das escritas
em `0x41A0D8` e `0x41A0DD`. Em `0x419FBE–0x419FE9`, EDI
recebe o primeiro argumento da rotina e ESI aponta para
`[EDI+0xDC] + 20 × [EDI+0xE8]`. O chamador em `0x418EA5`
passa EDI a `0x4043C7 → 0x419C4F`. O teste do bit 1 em
`[ESI+0x08]` em `0x419FFE` pode selecionar a cópia direta.
Falta determinar quem preenche esse vetor e confirmar o tipo
em execução. Esse caminho evidencia uma cópia de um registro
selecionado, com origem independente do retorno da consulta CRock.

#### Origem dos registros de posição de 20 bytes

A inicialização em `0x4180CC–0x4180ED` multiplica a contagem
em `[descritor+0x10]` por 20, aloca o vetor, guarda o ponteiro
em `[objeto+0xDC]` e chama `0x403E18 → 0x41D290`.
A rotina percorre as entradas com passo `0x14` e chama
`0x402D7E → 0x417E46` para preencher cada uma. O trecho
`0x418FED–0x419016` usa o mesmo escritor com um índice que
volta a zero ao atingir o limite.

Em `0x417E46`, o primeiro argumento fornece X/Y nos offsets
`+0x18/+0x1C`, um valor em `+0x68` e um byte em `+0x70`.
Esses dados chegam aos offsets `0`, `4`, `0x0C` e `0x10`
do registro. O campo `+0x08` recebe bits derivados de
`[origem+0x100]` e `[origem+0x108]`. Se o bit `0x80` de
`+0x108` estiver presente e houver um segundo objeto com vetor,
o escritor substitui X/Y pelo par de sua entrada selecionada
em `[vetor+20×índice]`.

Três vetores sintéticos no x86 isolado em Unicorn confirmaram
a escrita básica, o caso com os bits ativos sem segundo
objeto e o caso que copia de uma entrada do outro vetor.
A prova identifica um registro de estado já existente.
A rotina que integra a posição inicial do personagem continua
em investigação. O próximo ponto é seguir o despacho em
`0x419028` e verificar se seu receptor atualiza X/Y antes
de a posição ser registrada no ciclo seguinte.
