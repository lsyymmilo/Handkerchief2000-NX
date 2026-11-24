# Mapa técnico do motor de Handkerchief

O objetivo do projeto é desenvolver um port nativo de Handkerchief para
PlayStation Vita. Este mapa reúne resultados da análise estática de
`HDKWIN.EXE` e dos arquivos originais. A viabilidade do port segue em aberto.

## Arquivos HDK

Os HDK contêm blocos consecutivos. Cada bloco começa com um comprimento de
32 bits em little-endian que inclui o próprio campo, seguido pelo rótulo
da seção e pelo nome de classe, terminados em zero. A
[documentação do formato](hdk-format.md) descreve as dez seções das amostras
e a validação estrutural dos 99 arquivos com conteúdo. A estrutura dos
registros internos foi investigada apenas em parte.

## Funções identificadas em HDKWIN.EXE

Os endereços são virtuais e se referem à cópia analisada do executável
PE32 x86. Os nomes `fread` e `fseek` descrevem o comportamento observado
na desmontagem. São identificações funcionais de rotinas sem símbolos.

| Endereço | Responsabilidade observada |
|---|---|
| `0x408159` | Busca uma seção pelo rótulo. Lê o comprimento do bloco e o soma ao deslocamento atual para avançar. |
| `0x45D930` | Leitura do fluxo em elementos de tamanho e quantidade informados, compatível com `fread`. |
| `0x45DB00` | Reposicionamento do fluxo, compatível com `fseek`. |
| `0x42B439` | Método de serialização de `CEarth`. Passa o nome da classe ao leitor comum e delega o conteúdo à rotina compartilhada. |
| `0x4125E2` | Serialização compartilhada. O ramo de leitura em `0x41271C` consome os campos conforme a máscara presente no fluxo. |
| `0x40AC63` | Lê a lista com 12 bytes por elemento e acrescenta uma entrada zerada em memória. |
| `0x412F27` | Passa a lista e a base da referência a `0x40963E` e desloca o retângulo obtido pela posição do objeto. |
| `0x40963E` | Consulta as dimensões e calcula a união dos retângulos dos elementos. |
| `0x451629` | Inicializa os bancos globais com endereços de tabelas embutidas no executável. |
| `0x4120F9` | Grava o ponteiro da tabela em `descritor+0x04`. |
| `0x42B4B2` | Método de desenho de `CEarth`. Encaminha a lista em `objeto+0x44` para o desenho. |
| `0x40953C` | Prepara os elementos da lista, ajustando a posição e a referência antes de chamar a rotina de desenho. |
| `0x408C75` | Seleciona a superfície e recorta a região de origem antes de uma chamada compatível com DirectDraw `BltFast`. |

## Estrutura parcial de CEarth

### Campos verificados

Os deslocamentos abaixo são relativos ao objeto em memória. No arquivo,
os grupos são escritos em sequência quando seus bits estão presentes.
A rotina também testa uma máscara do chamador para decidir se guarda ou
apenas consome os dados.

| Condição no fluxo | Dados consumidos | Destino ou efeito observado |
|---|---|---|
| Campo inicial | Um valor de 32 bits | `+0x14`. |
| Bit `0x02` | Dois valores de 32 bits | `+0x18` e `+0x1C` quando armazenados. O par é usado para transladar o retângulo do objeto. |
| Bit `0x04` | Quatro valores de 32 bits | `+0x20` a `+0x2C` quando armazenados. O cálculo em `0x410E48` usa esses valores como dimensões e deslocamentos locais. |
| Bit `0x08` | Base de 32 bits, contagem de 16 bits e elementos de 12 bytes | Base em `+0x40` e ponteiro da lista em `+0x44` no caminho que mantém os dados. |
| Bit `0x01` | Contagem de 16 bits seguida da leitura de objetos | Controla um laço que pode anexar os objetos lidos à coleção. |

Em `A0.HDK`, o primeiro `CEarth` de `VISTA_0` tem posição `(524, 556)`,
base de referência zero e cinco elementos entre `0x0704` e `0x0740`
(fim exclusivo). Esses offsets são posições no arquivo HDK.

Em `VISTA_0` de `A0.HDK`, a contagem de `CGameRoot` é 6. Os seis
registros usam os bytes de controle `0B 0B` e têm contagem zero de
objetos aninhados. Suas listas somam 32 elementos do banco 2. O cursor
após o último registro coincide com o fim da seção em `0x0921`.
`tools/render_vista0.py` compõe esses elementos em ordem de arquivo com
ajuste gráfico zero e a paleta original do BMP. A prévia mede 4464 × 384
e tem origem de cenário em `(52, 232)`.

Os limites usados pela prévia são a união dos retângulos de destino.
Para esta amostra, vão de `(52, 232)` a `(4516, 616)`, com fim
exclusivo. O leitor valida se a contagem cabe na seção e reserva espaço
para a contagem de filhos e os cabeçalhos restantes antes de aceitar
cada lista. A leitura termina no limite declarado de `VISTA_0`.

### Interpretações e lacunas

O uso na aritmética de retângulos confirma o papel geométrico dos pares
de posição e deslocamento. As unidades e o papel desses limites na
colisão continuam em aberto. O significado do campo `+0x14`, a semântica
completa das máscaras e a função da coleção de objetos ainda precisam
ser estabelecidos.

## Lista de elementos e bancos de dimensões

Cada elemento tem três valores de 32 bits em little-endian.

| Offset no elemento | Uso observado |
|---|---|
| `+0x00` | Referência compactada a uma entrada de um banco de dimensões. O consumidor encerra a lista em memória quando encontra uma referência bruta zero. |
| `+0x04` | Deslocamento local X, interpretado com sinal. |
| `+0x08` | Deslocamento local Y, interpretado com sinal. |

`0x40963E` soma a base de `CEarth+0x40` à referência antes de extrair
o **banco nos bits 26–29** e o **índice nos bits 0–14**. A consulta usa
o ponteiro em `0x4ABD74 + banco × 0x14`. Cada entrada gráfica ocupa
8 bytes e contém quatro inteiros de 16 bits com sinal em little-endian.
O desenho usa X e Y de origem em `+0x00` e `+0x02`, e largura e altura
em `+0x04` e `+0x06`.

| Banco | Endereço da tabela em `.data` | Endereço do ponteiro global |
|---:|---|---|
| 0 | `0x48A3D8` | `0x4ABD74` |
| 1 | `0x48AFF8` | `0x4ABD88` |
| 2 | `0x48B7A8` | `0x4ABD9C` |

As dimensões abaixo foram lidas diretamente do banco 2 no executável.
O índice 556 é usado duas vezes em posições diferentes no primeiro
`CEarth` de `A0.HDK`.
O índice 564 tem origem `(272, 800)` e dimensões `184 × 40`, compondo
o retângulo de origem até `(456, 840)`, com limites finais exclusivos,
antes do recorte.

| Índice | Largura | Altura |
|---:|---:|---:|
| 564 | 184 | 40 |
| 556 | 216 | 176 |
| 555 | 216 | 88 |
| 558 | 184 | 176 |

### Associação com recursos gráficos

O método `0x42B4B2` chama `0x40953C` via `0x402095`. A preparação
percorre a lista até a referência zero, soma os X/Y locais à posição de
desenho e chama `0x408C75` via `0x401230`. A rotina final seleciona a
superfície de origem pelo ponteiro em `0x4AA3E4 + banco × 4` e a região
pelo índice na tabela de entradas. A chamada indireta em `0x408D59`
usa a entrada virtual `+0x1C` da superfície de destino com argumentos
compatíveis com `BltFast`.

| Bits | Uso observado no desenho |
|---|---|
| 30–31 | Antes de somar a base, `0x40953C` aceita a referência bruta se esses bits forem zero ou coincidirem com os da base em `objeto+0x40`. A finalidade do filtro segue em aberto. |
| 16–21 | Após somar a base, `0x40953C` usa esses bits para selecionar um valor de ajuste na tabela em `0x4ABED4` e soma esse valor à referência. O significado dos ajustes ainda precisa ser determinado. |
| 15 | Após o ajuste, `0x408C75` passa o bit como opção da cópia. O valor 1 seleciona a chave de cor da origem. |

### Carregamento da superfície do banco 2

O caminho configurado para o banco 2 é `./bitmap/HdkMAPa0.BMP`. Na cópia
local, corresponde a `original-data/BITMAP/HDKMAPA0.BMP`, um BMP indexado
de 8 bits com 640 × 1128 pixels. As entradas de regiões e dimensões
permanecem na tabela embutida em `0x48B7A8`.

| Endereço | Evidência |
|---|---|
| `0x4517DE` | Carrega o endereço da string em `0x48A0F8`. A chamada em `0x4517E8` configura o caminho no slot `0x4ABC78`. |
| `0x40E859` | Percorre os bancos e chama o carregador, inclusive para o índice 2. |
| `0x40DEF1` | Lê o caminho do banco. Tenta `LoadImageA` com o módulo e recorre a `LR_LOADFROMFILE` se a primeira tentativa falhar. Usa `GetObjectA` para obter as dimensões do bitmap. As chamadas em `0x40E044` e `0x40E06C` passam o slot `0x4AA3EC` como saída da criação da superfície do banco 2. |
| `0x40E25F` e `0x40E294` | Encaminham e copiam o bitmap para a superfície. O código tem um ramo de cópia por linhas em modo indexado de 8 bits e outro com `StretchBlt` e `SRCCOPY`. |
| `0x40FAC9` | Chamado em `0x40E100` após o preenchimento. Configura a chave de cor da origem com valor zero. |

A tabela de recursos examinada não mostrou um bitmap embutido
correspondente ao caminho. Ainda falta reproduzir a paleta efetiva,
explicar os ajustes gráficos, mapear os outros bancos a seus recursos e
verificar quais modos de desenho e caminhos de cópia são usados durante
uma partida.

### Catálogo dos bancos 5 a 8

As chamadas em `0x45180B–0x451842` configuram os caminhos dos quatro
BMPs. As chamadas em `0x4519A9–0x4519E0` registram as tabelas. Os quatro
bancos passam pelo mesmo inicializador, e `0x41229A` avança 8 bytes por
entrada até encontrar X igual a −1. Na amostra, os quatro campos do
terminador valem −1. O catálogo limita a leitura à seção `.data` gravada
no PE32 e ao início da tabela seguinte, em `0x4913B8` para o banco 8.

| Banco | BMP original | Tabela | Entradas antes do terminador | Válidas | Inválidas | Possíveis duplicatas |
|---:|---|---|---:|---:|---:|---:|
| 5 | HDKCHRA0.BMP | `0x48E070` | 721 | 721 | 0 | 1 |
| 6 | HDKCHRB0.BMP | `0x48F700` | 661 | 661 | 0 | 56 |
| 7 | HDKCHRC0.BMP | `0x490BB0` | 195 | 195 | 0 | 0 |
| 8 | HDKCHRD0.BMP | `0x4911D0` | 60 | 59 | 1 | 0 |

Os quatro BMPs são indexados de 8 bits e sem compressão. A entrada 0
do banco 8 declara `(0, 0, 640, 480)`, mas o BMP mede 640 × 440.
O catálogo sinaliza essa entrada e pula o recorte inteiro. A contagem
de possíveis duplicatas considera cada ocorrência posterior à primeira
com mesmas dimensões e mesmo hash SHA-256 dos índices de paleta dentro
do banco. Cada entrada válida mantém seu índice e sua posição no catálogo.

`python3 -B tools/preview_characters.py` gera páginas RGBA em
`build/characters-bank-*-opaco-*.png`, com os recortes rotulados, em
escala 1:1 e com a paleta do BMP. As páginas têm até 1024 × 1024 pixels.
`--transparent-zero` grava uma variante `chave-zero` que simula a
transparência do índice de paleta zero. No motor, a chave depende do bit
15 da referência efetiva. Uma entrada da tabela fornece apenas o
retângulo, então a transparência dessa variante é uma hipótese visual.
O catálogo ainda deixa em aberto ajustes de paleta em jogo, identidades
dos personagens e associação dos recortes às poses.

## Câmera e parallax horizontal

| Endereço | Comportamento observado |
|---|---|
| `0x4119A2` | Inicializa X em `contexto+0x00` e L em `+0x10` com a borda esquerda do retângulo recebido. O contexto global inicia com o retângulo `(0, 0, 640, 480)`. |
| `0x411C60` | Translada o retângulo da entidade-alvo por X e Y, corrige a posição quando ela ultrapassa a janela de acompanhamento e limita X a `[R − W, L]` para limites consistentes. R vem de `+0x18`, e W de `+0x40`. |
| `0x411D9D` | Caminho alternativo que acompanha um ponto, limita a correção a ±8 pixels por eixo por chamada e aplica os limites do contexto. |

X é somado à coordenada de mundo no desenho de `STAGE_0` e `FRONT_0`.
No passe condicional de `VISTA_0`, `0x42309E` usa
`X_vista = X − trunc((X − L) / 3)`, com truncamento em direção a zero.
A posição horizontal na tela é `x_mundo + X_vista`. Para `CEarth`,
`x_mundo` inclui a posição do objeto e o deslocamento local do elemento.
Com L fixo, X mais negativo desloca a cena para a esquerda. O passe
reaproveita a origem vertical do contexto principal.

`python3 -B tools/preview_camera.py` gera três PNGs RGBA transparentes
de 640 × 480 em `build/`, usando os seis objetos e 32 elementos de
`VISTA_0`. Para L igual a zero, X de 0, −300 e −600 resulta em
`X_vista` de 0, −200 e −400, respectivamente. O recorte preserva a
origem de mundo `(52, 232)`. Os valores de X são amostras experimentais.
A prévia mantém a translação vertical em zero, usa ajuste gráfico zero
e a paleta original do BMP. A aparência durante uma partida segue
sem verificação.

A análise ainda não determina qual entidade é selecionada em todos os
modos, quando o caminho de passo limitado é escolhido nem como os limites
do contexto se relacionam com os de cada fase HDK.

## Próximos marcos técnicos

1. Conferir a composição de `VISTA_0` de `A0.HDK` em desktop. As
   prévias já cobrem os 32 elementos. Resta comparar posições, limites,
   paleta e transparência com o desenho original antes da integração
   ao PS Vita.
2. Validar de ponta a ponta a leitura de registros `CEarth` em mais de um
   HDK, cobrindo o grupo opcional `0x04`, listas de elementos e objetos
   aninhados. Usar os limites das seções como verificação independente.
3. Usar o resultado da prova de conceito e a dependência das rotinas
   x86 de Win32 para decidir
   entre reaproveitar código autorizado, traduzir rotinas ou
   reimplementar o motor.
4. Com a rota técnica definida, validar uma cena mínima no PS Vita
   e medir memória e tempo por quadro no aparelho antes de ampliar o
   escopo para jogabilidade.
