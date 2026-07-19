# Modo história e progressão da campanha em Streets of Rage

**Manuscrito:** análise estática do ROM original e da recompilação C++  
**Âmbito:** abertura narrativa, attract mode, início de campanha, progressão dos oito rounds, ecrã de round clear, oferta de Mr. X e seleção do final  
**Fontes primárias:** `output/sor.asm`, `generated/Sor.cpp`, `SorManualFunctions.cpp`, `code-analysis/addresses.csv`, `code-analysis/labels.csv` e `rom/SOR.bin`

Os nomes usados abaixo correspondem aos símbolos acrescentados aos CSVs. Quando um significado é uma inferência e não uma consequência direta de uma leitura/escrita inequívoca, isso é assinalado.

---

## 1. Resultado principal

O jogo não possui uma classe, estrutura ou função única chamada “story mode”. A campanha é uma composição de máquinas de estados que comunicam quase exclusivamente através da RAM do Mega Drive:

1. `game_state` (`$FFFF00`) seleciona o modo global;
2. cada modo tem normalmente um handler de inicialização e outro de atualização;
3. os modos mais complexos têm um segundo índice de subestado e uma jump table própria;
4. `level` (`$FFFF02`) é o contador persistente da campanha, de `0` a `7`;
5. depois de cada round, o ecrã de resultados incrementa `level` e volta ao estado de introdução do round;
6. depois do round 8, `bad_ending_selected` (`$FFDE10`) decide entre o final bom e o final mau.

Fluxo normal de uma campanha:

```text
Sega ($00/$02)
  -> introdução narrativa ($04/$06)
  -> título ($08/$0A)
  -> menu ($10/$12)
  -> seleção de personagem ($20/$22)
  -> introdução do round ($28/$2A)
  -> gameplay ($14/$16)
  -> round clear ($18/$1A)
       | level < 7: level++ e volta a $28
       ` level = 7: $24 final bom ou $1C final mau
```

Há uma distinção importante entre dois sentidos de “história”:

- `init_intro` / `game_mode_intro` são apenas a sequência narrativa antes do título;
- a campanha jogável é o ciclo `level start -> in-game -> round clear`, governado por `game_state`, `level`, flags de conclusão e a oferta final.

---

## 2. Dispatcher global

### 2.1 Assembly

O núcleo em `game_infinite_loop` (`$3A2`) lê `game_state`, duplica-o para obter um offset de quatro bytes e consulta `game_state_handler_table` (`$3BA`):

```asm
moveq  #0,d0
move.w game_state,d0
add.w  d0,d0
move.l $3BA(pc,d0.w),d0
movea.l d0,a0
jsr    (a0)
jsr    sync_z80_1
bra.s  game_infinite_loop
```

Como os valores de `game_state` são pares, cada valor seleciona um longword da tabela. Há onze pares inicialização/atualização:

| `game_state` | Handler | Função |
|---:|---|---|
| `$00` / `$02` | `init_segascreen` / `game_mode_segascreen` | logótipo Sega |
| `$04` / `$06` | `init_intro` / `game_mode_intro` | prólogo narrativo |
| `$08` / `$0A` | `init_titlescreen` / `game_mode_titlescreen` | título |
| `$0C` / `$0E` | `init_top10score` / `game_mode_top10score` | top 10 |
| `$10` / `$12` | `init_selectscreenmode` / `game_mode_selectscreenmode` | menu principal / OPTIONS |
| `$14` / `$16` | `init_ingame` / `game_mode_ingame` | gameplay ou attract mode |
| `$18` / `$1A` | `init_roundclear` / `game_mode_roundclear` | bónus e resultados do round |
| `$1C` / `$1E` | `init_ending_bad` / `game_mode_ending_bad` | final mau |
| `$20` / `$22` | `init_characterselectscreen` / `game_mode_characterselectscreen` | seleção de personagem |
| `$24` / `$26` | `init_ending_good` / `game_mode_ending_good` | final bom |
| `$28` / `$2A` | `init_levelstart` / `game_mode_levelstart` | apresentação do round |

O padrão é consistente: o handler de inicialização termina normalmente com `addq.w #2,game_state`; a partir daí, o loop chama o handler de atualização em todos os frames.

### 2.2 C++ recompilado

`SorManualFunctions.cpp` preserva este mecanismo numa implementação manual de `Sor::game_infinite_loop`. As constantes mais relevantes são:

```cpp
constexpr m_long kGameState      = 0xFFFFFF00u;
constexpr m_long kStateJumpTable = 0x000003BAu;
```

O C++ lê o word de estado, calcula `state + state`, lê o ponteiro longword da ROM e chama `dispatch(handler)`. Portanto, a recompilação não substitui a campanha por uma abstração moderna; ela conserva o modelo de controlo do cartucho. As rotinas restantes em `generated/Sor.cpp` são traduções quase literais das instruções 68000, com endereços de RAM explícitos.

---

## 3. Prólogo, título e attract mode

### 3.1 Nova sessão

`init_intro` (`$8FD0`) não se limita a desenhar o prólogo. Também reinicializa os dados persistentes de uma nova sessão:

- `level = 0`;
- `wave = 0`;
- scores de P1 e P2 a zero;
- flags de morte/estado dos jogadores a zero;
- ponteiros de vidas por pontuação a zero;
- `player_mode_copy` a zero;
- música do prólogo (`$83`).

Isto torna o prólogo a fronteira lógica de “nova campanha”. O menu e a seleção de personagem que aparecem depois operam sobre esta sessão já limpa.

### 3.2 Saltar a abertura

Em `game_mode_intro` (`$904E`), Start pode enviar a execução para:

- título (`game_state = $08`) quando a sequência ainda está numa fase inicial;
- menu principal (`game_state = $10`) quando já passou o limiar interno da cena.

Sem input, a timeline partilhada `story_scene_timeline_update` (`$B6DE`) cria objetos narrativos segundo uma lista temporizada. Quando a lista acaba, escreve em `game_state` o próximo estado configurado pela cena. `story_scene_select_script` (`$3F65E`) lê esta configuração em `$3F680`: a entrada do prólogo termina com estado `$00`, que `game_mode_intro` converte na entrada em attract mode.

### 3.3 A campanha de demonstração não é uma campanha normal

Se o título expira, `$90AA` prepara o attract mode:

```asm
move.w #$0014,game_state
move.b #1,demo_mode
move.l #$00FF7000,demo_ai_input_p1
move.l #$00FF8000,demo_ai_input_p2
```

Com `demo_mode != 0`:

- a leitura de joypad em `$813C` mistura bytes dos streams de input artificial;
- `init_ingame` chama internamente `init_levelstart`, evitando o fluxo interativo normal;
- são impostos personagens, vidas e duração da demo;
- Start coloca o bit 7 de `demo_mode`, começa o fade e aborta a demonstração;
- no fim do fade, `game_mode_ingame` regressa ao logótipo (`$00`) ou ao top 10 (`$0C`), em vez de entrar em round clear.

Assim, os estados `$14/$16` são partilhados entre campanha e demo, mas a flag `demo_mode` muda as entradas, o HUD e a rota de saída.

---

## 4. Início da campanha e introdução de cada round

Depois da confirmação de personagem, `initialize_player_continues` (`$17A2`) configura continues/vidas e escreve:

```asm
move.w #$0028,game_state
```

### 4.1 `init_levelstart` (`$106EA`)

O handler de inicialização do round:

- limpa quase toda a RAM de trabalho;
- repõe `wave = 0`;
- marca `level_intro_active = 1`;
- inicia o fade em `$40`;
- carrega arte, tilemaps, HUD e dados dependentes de `level`;
- chama `start_round_setup`;
- recria os objetos P1/P2 com os IDs escolhidos;
- avança `game_state` de `$28` para `$2A`.

O carregamento de dados é indexado pelo número do round. Por exemplo, `load_level_data` (`$576`) multiplica `level` por seis e usa uma entrada de seis bytes na tabela ROM `$1C378`.

### 4.2 Máquina de estados da apresentação

`game_mode_levelstart` entra em `level_intro_dispatcher` (`$11A50`). O índice `level_intro_substate` (`$FB48`) seleciona uma de seis entradas em `level_intro_jt` (`$11A5C`):

1. espera pelo fade-in;
2. move as duas metades do banner “ROUND n” para o centro;
3. espera `$60` frames;
4. move o banner para fora;
5. espera `$30` frames;
6. `level_intro_finish` escreve `game_state = $14`.

Ao chegar a `$14`, `init_ingame` configura os contadores de fade/tempo e avança imediatamente para `$16`.

---

## 5. Gameplay e progressão dentro do round

### 5.1 Atualização por frame

No caminho normal de `game_mode_ingame` (`$1087A`), quando não há fade nem pausa, o jogo executa sucessivamente:

- relógio e limites do cenário;
- pausa/entrada de segundo jogador;
- máquina da oferta de Mr. X;
- HUD e objetos;
- lógica de waves;
- arte pendente;
- dispatcher secundário de fluxo de nível.

`level_flow_handler` (`$464`) usa `level_flow_flags` (`$FFFA72`) para impedir que fases de carregamento, música e setup sejam repetidas. `wave` (`$FFFF04`) seleciona os blocos de inimigos do round; `level` seleciona a tabela principal.

### 5.2 Conclusão do round

`end_of_level_flag` (`$FFFA73`) é a indicação explícita de que a parte jogável terminou. Ela pode ser levantada por caminhos dependentes do round, por exemplo quando acabam certas waves ou quando se satisfaz a condição especial do nível 6.

Enquanto a flag está ativa, `end_level_player_exit_update` (`$502C`) força a animação/posição de saída dos jogadores. Quando a saída termina, escreve:

```asm
move.b #1,fade_out_flag
move.w #$40,palette_fade_counter
```

Quando o fade acaba, o caminho de campanha em `ingame_finish_fade` (`$108CC`) escolhe normalmente:

```asm
move.w #$0018,game_state
```

Ou seja, a conclusão física do nível não incrementa diretamente `level`; transfere o controlo para o ecrã de round clear.

Há rotas laterais no mesmo ponto:

- attract mode volta ao circuito de apresentação;
- flags de game over/continue podem enviar ao top 10 ou reiniciar outra sequência;
- uma ramificação especial da narrativa a dois jogadores força `level = 5` e reentra em `$28`.

Esta última ramificação existe no código, mas o nome funcional da flag que a ativa ainda não foi promovido para os CSVs, porque a sua semântica completa depende de todos os resultados do duelo entre jogadores.

---

## 6. Round clear é o gestor da campanha

### 6.1 Inicialização

`init_roundclear` (`$91A0`) chama `round_clear_sequence_init` (`$181EA`). Esta rotina:

- normaliza jogadores mortos/inativos;
- carrega o ecrã de resultados;
- prepara bónus de tempo, dificuldade, vidas e especiais;
- no round final, inclui valores adicionais ligados às vidas restantes;
- limpa estado transitório, incluindo `mr_x_offer_flag`;
- põe `round_clear_substate = 0`.

### 6.2 Tally e avanço

`round_clear_sequence_update` (`$1833C`) usa `round_clear_substate` (`$FB4C`) como offset em `round_clear_jt` (`$18350`). A tabela conduz animações, conversão dos bónus em score, esperas e fade.

O ponto decisivo é `round_clear_advance_campaign` (`$183B0`):

```asm
cmpi.w #7,level
beq.s   final_round
addq.w  #1,level
move.w  #$28,game_state
rts
```

Logo, os oito rounds são representados por `level = 0..7`. Para `0..6`, o round clear é a única rotina observada que incrementa o contador normal da campanha e inicia a apresentação seguinte.

### 6.3 Seleção do final

No round 8 (`level == 7`), a execução cai em `round_clear_select_ending` (`$183C4`):

```asm
moveq #$24,d0
tst.b  bad_ending_selected
beq.s  set_state
moveq #$1C,d0
set_state:
move.w d0,game_state
```

Esta é a prova direta da semântica de `$FFDE10`:

- zero: `game_state = $24`, final bom;
- não zero: `game_state = $1C`, final mau.

---

## 7. Oferta de Mr. X e bifurcação narrativa

### 7.1 Ativação

Na secção final do round 8, a lógica do objeto de jogador em `$50A6` deteta que os jogadores chegaram à posição da cena. Depois de todos os jogadores ativos entrarem na zona, ela:

```asm
move.b #1,mr_x_offer_flag
move.b #1,stop_clock
```

A partir daí, `mr_x_offer_update` (`$11B4C`), chamado em todos os frames de gameplay, deixa de retornar imediatamente e começa a processar `mr_x_offer_state` (`$FFDE04`).

### 7.2 Estrutura da máquina

O estado é usado duas vezes:

- como índice byte em `mr_x_offer_control_table` (`$120AA`), que decide se o controlo está bloqueado, libertado ou numa fase de escolha;
- multiplicado por dois como índice em `mr_x_offer_jt` (`$11B94`).

As fases observáveis incluem:

- parar os jogadores e o relógio;
- carregar arte e texto;
- abrir/fechar a área visível da cena via registo VDP `$92xx`;
- desenhar diálogo letra a letra;
- permitir escolha esquerda/direita e confirmação;
- em 2P, comparar as escolhas dos dois jogadores;
- ativar `half_damage` durante um duelo P1 contra P2;
- regressar ao combate normal ou marcar o resultado narrativo.

As escolhas ficam nos campos de estado dos objetos dos jogadores (em particular bits de `object+$59`). `$FFDE0E` não guarda a resposta: é `mr_x_dialogue_clear_flags`. A rotina `$12576` consome o bit 0 para limpar a área principal de diálogo e o bit 1 para limpar as duas áreas de escolha. A ligação entre aceitação e final mau é, por sua vez, inequívoca.

### 7.3 Um jogador

`mr_x_offer_choice_init` (`$11CCA`) começa por limpar `bad_ending_selected`. A resposta do jogador escolhe depois um dos ramos:

- recusar: a cena termina, o controlo regressa e o combate contra Mr. X pode concluir normalmente;
- aceitar: `mr_x_offer_mark_bad_ending` (`$12074`) escreve `1` em `bad_ending_selected` e avança o diálogo.

O combate/saída termina ainda pelo mecanismo normal de round clear. A diferença só é consumida em `$183C4`, depois do tally.

### 7.4 Dois jogadores

Em 2P a mesma máquina admite mais casos:

- respostas compatíveis podem seguir diretamente para o ramo correspondente;
- respostas incompatíveis ativam o confronto P1 contra P2;
- `half_damage` altera a força aplicada durante esse confronto;
- a máscara `player_mode` pode ser temporariamente alterada enquanto a máquina decide qual jogador continua;
- existe uma rota que volta ao round 6 (`level = 5`) antes de reentrar no ciclo normal.

O código mostra claramente esta topologia, embora atribuir um nome narrativo definitivo a todas as combinações exija uma matriz de testes de input no jogo. Por isso, os CSVs agora identificam apenas os estados e resultados confirmados estaticamente.

---

## 8. Finais

### 8.1 Final bom: estados `$24/$26`

`init_ending_good`:

- limpa a RAM de objetos;
- carrega os assets da sequência através de `good_ending_sequence_init` (`$B3C6`);
- toca o tema `$91`;
- avança para `$26`.

`game_mode_ending_good` usa `story_scene_timeline_update`, a mesma infraestrutura temporal do prólogo. Depois de a cena estar suficientemente avançada, Start permite saltar para o top 10 (`$0C`). Sem input, a configuração 1 em `story_scene_config_table` também termina em `$0C` quando o índice da timeline ultrapassa `$12`.

### 8.2 Final mau: estados `$1C/$1E`

`bad_ending_sequence_init` (`$87C6`):

- limpa os objetos;
- toca o tema `$8F`;
- escolhe o retrato com base na personagem/jogador sobrevivente;
- inicializa a máquina em `$F910`;
- cria o primeiro objeto da cena.

`bad_ending_sequence_update` (`$8890`) despacha essa máquina através da tabela relativa `$88A0`, atualiza objetos e permite Start nas fases finais. O último fade escreve `game_state = 0`, regressando ao ciclo do logótipo Sega.

O final mau tem, portanto, uma implementação separada da timeline genérica usada pelo prólogo e pelo final bom.

---

## 9. Pseudocódigo reconstruído

```cpp
for (;;) {
    dispatch(gameStateHandlerTable[game_state / 2]);
    waitForVBlank();
}

void finishGameplayFade() {
    if (demo_mode) {
        game_state = (demo_mode & 0x80) ? SEGA_INIT : TOP10_INIT;
        demo_mode = 0;
        return;
    }

    if (specialStoryRestart) {
        specialStoryRestart = 0;
        level = 5;
        game_state = LEVEL_INTRO_INIT;
        return;
    }

    game_state = ROUND_CLEAR_INIT;
}

void advanceCampaignAfterTally() {
    if (level != 7) {
        ++level;
        game_state = LEVEL_INTRO_INIT;
        return;
    }

    game_state = bad_ending_selected
        ? BAD_ENDING_INIT
        : GOOD_ENDING_INIT;
}
```

---

## 10. Mapa de dados essenciais

| Endereço | Símbolo | Papel |
|---:|---|---|
| `$FFFF00` | `game_state` | modo global |
| `$FFFF02` | `level` | round atual, `0..7` |
| `$FFFF04` | `wave` | grupo atual dentro do round |
| `$FFFF18` | `player_mode` | máscara de jogadores ativos |
| `$FFFF2A` | `demo_mode` | separa attract mode da campanha |
| `$FFFA1F` | `level_intro_active` | bloqueia gameplay durante o fade inicial |
| `$FFFA71` | `fade_out_flag` | transição para fora do gameplay |
| `$FFFA72` | `level_flow_flags` | gates internos do carregamento/fluxo |
| `$FFFA73` | `end_of_level_flag` | round jogável concluído |
| `$FFFA30/$31/$33` | `story_scene_step/last_step/next_state` | timeline do prólogo/final bom |
| `$FFFB06` | `story_scene_timer` | espera entre entradas da timeline |
| `$FFFB48` | `level_intro_substate` | apresentação “ROUND n” |
| `$FFFB4A` | `level_intro_timer` | temporização da apresentação |
| `$FFFB4C` | `round_clear_substate` | tally e avanço da campanha |
| `$FFFB4E` | `round_clear_timer` | espera curta do tally |
| `$FFDE00` | `mr_x_offer_flag` | ativa a cena final |
| `$FFDE04` | `mr_x_offer_state` | estado da oferta |
| `$FFDE0E` | `mr_x_dialogue_clear_flags` | pedidos de limpeza das áreas de diálogo |
| `$FFDE10` | `bad_ending_selected` | resultado consumido após o round 8 |
| `$FFF910` | `bad_ending_substate` | máquina exclusiva do final mau |

---

## 11. Conclusões

- A unidade persistente da campanha é `level`; `wave` só descreve progresso interno do round.
- A progressão normal entre rounds pertence ao ecrã de round clear, não à lógica que derrota o boss.
- A introdução do round e o tally têm máquinas de estados independentes (`$FB48` e `$FB4C`).
- O attract mode reutiliza o gameplay, mas é isolado por `demo_mode` e nunca segue a progressão normal de campanha.
- A oferta de Mr. X é executada dentro de `game_mode_ingame`, não num `game_state` global separado.
- A decisão narrativa final é reduzida a um byte, `bad_ending_selected`, lido num único ponto de routing depois do round 8.
- O C++ recompilado mantém deliberadamente esta arquitetura de ROM/RAM; compreender o modo história continua a exigir seguir os endereços e jump tables originais.

## 12. Trabalho futuro

Uma análise dinâmica pode completar a matriz 2P da oferta de Mr. X. O teste ideal registaria, para cada combinação de respostas, os valores por frame de `$FFDE04`, `$FFDE10`, os bytes `object+$59` de P1/P2, `$FFFF18`, `$FFFF34`, `$FFFF36` e `game_state`. Isso permitiria nomear com 100% de confiança as duas flags ainda não promovidas e documentar exatamente quando a rota especial regressa ao round 6.
