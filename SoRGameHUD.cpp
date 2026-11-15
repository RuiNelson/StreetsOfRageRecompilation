#include "SoRCpuOps.hpp"
#include "SoRRemoteInput.hpp"

#include <cstdint>
#include <memory>

using namespace sorops;

// ---------------------------------------------------------------------------
// The gameplay HUD, written by the game into the window-plane block at $FF6000 (two rows of elements from
// $FF600A and $FF605A, a bottom edge behind them) and uploaded by vdp_copy_stride every VBlank:
//
//   $4D1E  hud_write_tile_list                  count, base and tile bytes from a3 as words at a2
//   $4D60  update_score_hud_and_check_extra_life the six score digits, and a life at each threshold
//   $4E14  draw_player_lives_and_specials       the lives and special counters (two tiles each)
//   $5620  hud_clear_p1_status_rows / $563E p2  blank tile $6D7 over a player's side of both rows
//   $10CA8 hud_tick_round_timer / $10CCC        the round timer's BCD step and its two digits
//   $10DCA add_bcd_resource_value               one BCD table byte added through two predecrements
//   $11090 hud_write_plate_row                  one tile row of a bar: three plate longs, five of d1
//   $115CC update_join_and_continue_hud         P2's JOIN prompt and each player's out/continue rows
//   $116DC hud_blink_join_prompt                the prompt's blink, every 8 frames
//   $11708 hud_write_tile_string                tile bytes from a3 as words at a2, up to a negative byte
//   $11774 / $1177A hud_draw_p1/p2_score_digits  the player's six score digits as tiles from $7DB
//   $117BC hud_draw_player_name_tiles           the player's name tiles on their side of the row
//   $11876 hud_draw_object_health_bar           one boss bar row from +$32 of an object
//   $1852C / $18532 round_clear_build_row(_highlighted)  a row of the round-clear tally into the scratch buffer
//
// Each is the ROM routine as it is: the same RAM at the same addresses, written on the same frame (the
// block at $FF6000 and the counters are read by the game and by autoplay), and the same registers.
// Tables are read from the ROM. A routine here calls only routines that are also manual, so
// `$103FE (memfill_long_36)`, which `$5620` and `$563E` call, is inlined (the call map no longer shows it).
// The condition codes at the exit are the ROM instruction's, except in `$115CC`; the recompiler drops dead
// flags and none of these callers reads them.
//
// The window-plane upload `$8064 (vdp_copy_stride)` and the bar tile rows `$10F80
// (vblank_update_gameplay_health_bars)` are at the end of the file. They are what make the HUD 4:3 (the ROM's)
// or 16:9 (`--widescreen`), and the routines above know nothing of it: the block in work RAM is the ROM's.
//
// Not here: the routines that decide when the HUD is drawn (`$10C88 (update_game_clock)`, `$117FC
// (stage_clear_monitor)`) and the level-start loader `$110AC (load_round_hud_resources)`.
// ---------------------------------------------------------------------------

namespace {

constexpr m_long kHudRows = 0x00FF600Au; // first element row of the block, window row 1 of the HUD

constexpr m_long kP1Object = 0xFFFFB800u;
constexpr m_long kP2Object = 0xFFFFB880u;

constexpr m_long kScoreP1             = 0xFFFFFF09u; // three packed-BCD bytes
constexpr m_long kScoreP2             = 0xFFFFFF11u;
constexpr m_long kLivesViaPointsP1    = 0xFFFFFF26u; // index of the next extra-life threshold
constexpr m_long kLivesViaPointsP2    = 0xFFFFFF27u;
constexpr m_long kP1Lives             = 0xFFFFFF20u; // lives, then the special counter
constexpr m_long kP2Lives             = 0xFFFFFF23u;
constexpr m_long kP1SpecialAttacks    = 0xFFFFFF21u;
constexpr m_long kP2SpecialAttacks    = 0xFFFFFF24u;
constexpr m_long kP1OutOrContinueFlag = 0xFFFFFF22u;
constexpr m_long kP2OutOrContinueFlag = 0xFFFFFF25u;
constexpr m_long kDemoMode            = 0xFFFFFF2Au;
constexpr m_long kPlayerMode          = 0xFFFFFF18u;
constexpr m_long kLevel               = 0xFFFFFF02u;
constexpr m_long kP1CharacterId       = 0xFFFFFF1Eu;
constexpr m_long kP2CharacterId       = 0xFFFFFF1Fu;
constexpr m_long kP2CharacterIngame   = 0xFFFFB8D0u;
constexpr m_long kP2ObjectFlags       = 0xFFFFB8D9u; // object +$59
constexpr m_long kP2ObjectFlags1      = 0xFFFFB881u; // object +$01

constexpr m_long kRoundTimerBcd     = 0xFFFFFB02u; // the timer byte is the one before it
constexpr m_long kTimeOverFlag      = 0xFFFFFA52u;
constexpr m_long kTimeOverSequence  = 0xFFFFFA49u;
constexpr m_long kTimeOverTimer     = 0xFFFFFA4Au;
constexpr m_long kPoliceSpecial     = 0xFFFFFA1Au;
constexpr m_long kJoinBlinkP2       = 0xFFFFFA21u; // frame counter, then its alternation bit, per prompt
constexpr m_long kJoinAlternate     = 0xFFFFFA22u;
constexpr m_long kJoinBlinkOutP1    = 0xFFFFFA23u;
constexpr m_long kJoinBlinkOutP2    = 0xFFFFFA25u;
constexpr m_long kP2JoypadAbsent    = 0xFFFFFC0Au; // nonzero: no pad on port 2
constexpr m_long kP2ButtonsHeld     = 0xFFFFFC08u;

constexpr m_long kExtraLifeThresholds = 0x00004DECu; // longs, one a step
constexpr m_long kBcdTable            = 0x00010DDEu; // longs, read backwards one byte at a time
constexpr m_long kJoinCharacterTable  = 0x0001167Eu; // P1's character -> P2's
constexpr m_long kBarSpecialShapes    = 0x00011930u; // one long per eighth of a bar tile

// 1 PLAYER AI ASSIST (user): P2's label reads "COM-", not "2UP-". The two tile lists that hold the label (the
// gameplay HUD's, `$4D38`, written by `$4D1E`, and the continue prompt's, `$117F4`, by `$117BC`) are the ROM's
// count, base and four tile bytes; the bytes are the HUD font's A = 1 ... Z = 26, `-` = $26.
constexpr m_long kP2NameList         = 0x00004D38u;
constexpr m_long kP2ContinueNameList = 0x000117F4u;
constexpr m_byte kComputerLabel[4]   = {0x03, 0x0F, 0x0D, 0x26}; // C O M -

// True when the tile list at `list` (before its count word is read) is P2's label and the computer plays P2.
bool computerLabel(m_long list) {
    return (list == kP2NameList || list == kP2ContinueNameList) && SoRRemoteInput::aiAssist();
}

// The round-clear tally's P2 row (`$185F0`): a VDP command long, then tile bytes; at byte 8 the label "2UP".
constexpr m_long kP2TallyRow           = 0x000185F0u;
constexpr m_byte kComputerTallyLabel[3] = {0x03, 0x0F, 0x0D}; // C O M

constexpr m_long kScratchBuffer          = 0x00FF8000u; // decompression_scratch_buffer
constexpr m_long kRoundClearVblankRequest = 0xFFFFFA4Fu;
constexpr m_long kRoundClearSubstate     = 0xFFFFFB4Cu;

constexpr m_word kBlankTile = 0x06D7u;
constexpr m_word kDigitBase = 0x07DBu; // score digits
constexpr m_word kTimerBase = 0x06C0u; // the timer's and the counters' digits, two tiles tall

// move.w dN, (aN)+ and the flags of that move.
void writeWordPostInc(CPU68K &cpu, SystemMemory &mem, int addressReg, m_word value) {
    mem.writeWord(cpu.a[addressReg], value);
    cpu.a[addressReg] += 2;
    cpu.setNZClearVC(value, 0x8000u);
}

// dbf dN: the low word counts down; true while it has not reached $FFFF.
bool dbf(CPU68K &cpu, int reg) {
    const m_word next = static_cast<m_word>(cpu.dw(reg) - 1u);
    cpu.setDw(reg, next);
    return next != 0xFFFFu;
}

// add.w src, dN
void addWord(CPU68K &cpu, int reg, m_word src) {
    cpu.setDw(reg, static_cast<m_word>(addValue(cpu, cpu.dw(reg), src, 16)));
}

// The three digit pairs of a score as tiles from $7DB at a5 (the shared tail of $4D60 and $11774):
// d1.w is the player's offset into the row, a4 the score.
void writeScoreTiles(CPU68K &cpu, SystemMemory &mem) {
    cpu.a[5] = kHudRows + sext16(cpu.dw(1));
    cpu.setDw(1, kDigitBase);
    cpu.d[7] = 2;
    do {
        cpu.d[0] = 0;
        cpu.setDb(0, mem.readByte(cpu.a[4]));
        cpu.setDb(0, static_cast<m_byte>(lsrValue(cpu, cpu.db(0), 4, 8)));
        addWord(cpu, 0, cpu.dw(1));
        writeWordPostInc(cpu, mem, 5, cpu.dw(0));
        cpu.d[0] = 0;
        cpu.setDb(0, mem.readByte(cpu.a[4]));
        cpu.a[4] += 1;
        cpu.setDb(0, static_cast<m_byte>(cpu.db(0) & 0x0Fu));
        addWord(cpu, 0, cpu.dw(1));
        writeWordPostInc(cpu, mem, 5, cpu.dw(0));
    } while (dbf(cpu, 7));
}

// $10CE6: one BCD digit (d0.w, 0-9) as two stacked tiles from base d6 at a1, the lower one a row further down.
void writeDigitTiles(CPU68K &cpu, SystemMemory &mem) {
    addWord(cpu, 0, cpu.dw(0));
    addWord(cpu, 0, cpu.dw(6));
    writeWordPostInc(cpu, mem, 1, cpu.dw(0));
    addWord(cpu, 0, 1);
    mem.writeWord(cpu.a[1] + 0x4Eu, cpu.dw(0));
    cpu.setNZClearVC(cpu.dw(0), 0x8000u);
}

// $10CCC's body: the two digits of the timer byte at a6, tens then units. `entered` is told each time the ROM
// would enter `$10CE6` (true for the `bsr`, false for the fall-through) so the call log keeps both.
template <typename Entered>
void writeTimerDigits(CPU68K &cpu, SystemMemory &mem, Entered &&entered) {
    cpu.a[1] = 0x00FF602Eu;
    cpu.setDw(6, kTimerBase);
    cpu.setDb(0, mem.readByte(cpu.a[6]));
    cpu.setDw(0, static_cast<m_word>(cpu.dw(0) & 0x00F0u));
    cpu.setDb(0, static_cast<m_byte>(lsrValue(cpu, cpu.db(0), 4, 8)));
    entered(true);
    writeDigitTiles(cpu, mem);

    cpu.setDb(0, mem.readByte(cpu.a[6]));
    cpu.setDw(0, static_cast<m_word>(cpu.dw(0) & 0x000Fu));
    entered(false);
    writeDigitTiles(cpu, mem);
}

// The ROM's `$103FE (memfill_long_36)` cascade: `count` longs of d1 at a1 (9 from its first entry, 5 from `loc_10406`).
void fillLongs(CPU68K &cpu, SystemMemory &mem, int count) {
    for (int i = 0; i < count; ++i) {
        mem.writeLong(cpu.a[1], cpu.d[1]);
        cpu.a[1] += 4;
    }
    cpu.setNZClearVC(cpu.d[1], 0x80000000u);
}


constexpr m_long kGameState = 0xFFFFFF00u;

// VDP ports and the window-plane HUD block uploaded by vdp_copy_stride.
constexpr m_long kVdpData    = 0x00C00000u;
constexpr m_long kVdpControl = 0x00C00004u;
constexpr m_long kRowStride  = 0x00800000u; // 64-cell plane row, in VDP command units

// The gameplay HUD lives in window rows 0-3 (VRAM $D000, 40 columns). Level start uploads all four rows
// once from $FF8000: the frame's top edge with the TIME label, the plate and side edges, the bottom edge.
// Every VBlank then re-uploads rows 1-3 from the $FF6000 block (two rows of elements, then the bottom edge).
constexpr m_long kHudStaticCommand = 0x50000003u;
constexpr m_word kHudStaticRows    = 0x0003u;
constexpr m_long kHudBlockCommand  = 0x50800003u;
constexpr m_word kHudBlockRows     = 0x0002u;
// While a boss is registered, bundle 9 ($FFF510, every VBlank) puts the boss bar row at window row 3 and
// the bottom edge under it at row 4 (the window is then 5 rows high).
constexpr m_long kHudBossCommand   = 0x51800003u;
constexpr m_word kHudBossRows      = 0x0001u;
constexpr m_word kHudColumns       = 0x0027u;
constexpr int    kHudCells         = 40;

// Window tiles of the HUD frame: the yellow border (corners, edges) and the plate between elements.
// Glyph and bar tiles carry the plate as palette 0 colour 2; main.cpp keys that colour out.
constexpr m_word kHudFrameFirstTile = 0x06D4u;
constexpr m_word kHudFrameLastTile  = 0x06D7u;

// HUD tiles in VRAM (tile numbers) and the plate colour inside them (palette 0 colour 2). The glyph, digit and
// bar tiles all paint the plate into their own background.
// The two runs skip the sprite table and the horizontal scroll table that sit between them in VRAM ($F000-$F7FF).
struct TileRange {
    m_word first;
    m_word last;
};
constexpr TileRange kHudArtRanges[] = {{0x06BEu, 0x06FFu}, {0x07C0u, 0x07F3u}};
constexpr m_byte    kHudPlateColor  = 2;

// The level-start HUD upload sets this; the first per-VBlank HUD upload after it, when all the level's HUD art
// is in VRAM, clears the plate from that art.
thread_local bool hudPlatePending = false;

// Health-bar upload state ($110A8 holds the plate rows of a bar tile).
constexpr m_long kHealthBarPlateRow       = 0x000110A8u;
constexpr m_long kMrXOfferFlag            = 0xFFFFDE00u;
constexpr m_long kFinalBossPresentation   = 0xFFFFFA77u;
constexpr m_long kHudClearRequest         = 0xFFFFFA7Au;
constexpr m_long kBossHealthSwitch        = 0xFFFFF500u;
constexpr m_long kHealthBarPointers       = 0xFFFFFC0Cu;
constexpr m_long kBossHealthPrimary       = 0xFFFFF502u;
constexpr m_long kBossHealthSecondary     = 0xFFFFF508u;

// VDP command for a VRAM access at a byte address: bits 15-14 move to the low command word.
m_long vramCommand(m_word byteAddress, bool write) {
    return (write ? 0x40000000u : 0u) | (static_cast<m_long>(byteAddress & 0x3FFFu) << 16) |
           static_cast<m_long>((byteAddress & 0xC000u) >> 14);
}

// Turns the HUD plate colour of every tile in the HUD art ranges into transparent colour 0. The art is read
// from a VDP state snapshot (the data port's read path does not prefetch like the hardware) and written back
// through the VDP ports, so this must run where the game itself may touch them (the VDP stalls the renderer
// during the VBlank handler, which is where the HUD is uploaded).
void removeHudPlate(VDP &vdp) {
    const auto snapshot = std::make_unique<VDP::StateSnapshot>(vdp.stateSnapshot());

    for (const TileRange &range : kHudArtRanges) {
        const std::size_t firstByte = static_cast<std::size_t>(range.first) * 32u;
        const std::size_t byteCount = (static_cast<std::size_t>(range.last) - range.first + 1u) * 32u;

        const m_long writeCommand = vramCommand(static_cast<m_word>(firstByte), true);
        vdp.writeControlPort(static_cast<m_word>(writeCommand >> 16));
        vdp.writeControlPort(static_cast<m_word>(writeCommand));
        for (std::size_t offset = 0; offset < byteCount; offset += 2) {
            m_word word = static_cast<m_word>((snapshot->vram[firstByte + offset] << 8) |
                                              snapshot->vram[firstByte + offset + 1]);
            for (int shift = 0; shift < 16; shift += 4) {
                if (((word >> shift) & 0x0Fu) == kHudPlateColor)
                    word = static_cast<m_word>(word & ~(0x0Fu << shift));
            }
            vdp.writeDataPort(word);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// $004D1E (hud_write_tile_list) — a3 -> count.w, base.w, then count+1 tile bytes; each becomes the word
// base + byte at a2. Leaves d7.w = $FFFF, d1.w = base, d0 = the last word.
// ---------------------------------------------------------------------------
void StreetsOfRage::hud_write_tile_list(m_long /*entry_*/) {
    traceEnter(0x00004D1Eu);
    auto &mem = memory();

    const bool computer = computerLabel(cpu().a[3]);
    cpu().setDw(7, mem.readWord(cpu().a[3]));
    cpu().a[3] += 2;
    cpu().setDw(1, mem.readWord(cpu().a[3]));
    cpu().a[3] += 2;
    unsigned index = 0;
    do {
        cpu().d[0] = 0;
        const m_byte tile = mem.readByte(cpu().a[3]);
        cpu().setDb(0, computer && index < 4 ? kComputerLabel[index] : tile);
        ++index;
        cpu().a[3] += 1;
        addWord(cpu(), 0, cpu().dw(1));
        writeWordPostInc(cpu(), mem, 2, cpu().dw(0));
    } while (dbf(cpu(), 7));
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $011708 (hud_write_tile_string) — as $4D1E, but the list stops at a negative byte (which is left in d0,
// N set, and consumed).
// ---------------------------------------------------------------------------
void StreetsOfRage::hud_write_tile_string(m_long /*entry_*/) {
    traceEnter(0x00011708u);
    auto &mem = memory();

    cpu().setDw(7, mem.readWord(cpu().a[3]));
    cpu().a[3] += 2;
    cpu().setDw(1, mem.readWord(cpu().a[3]));
    cpu().a[3] += 2;
    do {
        cpu().d[0] = 0;
        const m_byte tile = mem.readByte(cpu().a[3]);
        cpu().a[3] += 1;
        cpu().setDb(0, tile);
        cpu().setNZClearVC(tile, 0x80u);
        if ((tile & 0x80u) != 0) {
            cpu().ssp += 4;
            return;
        }
        addWord(cpu(), 0, cpu().dw(1));
        writeWordPostInc(cpu(), mem, 2, cpu().dw(0));
    } while (dbf(cpu(), 7));
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $004E14 (draw_player_lives_and_specials) — a0 = a player's object. Writes the lives counter, then the
// special counter, each as the tiles of one digit (two stacked, $6C0 + 2n and the next, 9 for 16 and more):
// P1's from $FF6020, P2's from $FF6054, the second 10 cells right of the first (P2: 26 cells left).
// ---------------------------------------------------------------------------
void StreetsOfRage::draw_player_lives_and_specials(m_long /*entry_*/) {
    traceEnter(0x00004E14u);
    auto &mem = memory();

    cpu().setDw(2, kTimerBase);
    cpu().d[0] = 0;
    cpu().d[1] = 0x0A;
    cpu().a[4] = kP1Lives;
    if (cpu().a[0] != kP1Object) {
        cpu().d[0] = 0x34;
        cpu().d[1] = 0xFFFFFFE6u;
        cpu().a[4] = kP2Lives;
    }
    cpu().a[5] = 0x00FF6020u + sext16(cpu().dw(0));

    const auto counterTiles = [&](m_long target) {
        cpu().d[0] = 0;
        cpu().setDb(0, mem.readByte(cpu().a[4]));
        cpu().a[4] += 1;
        cmpValue(cpu(), cpu().db(0), 0x10, 8);
        if (!cpu().cs())
            cpu().d[0] = 9;
        addWord(cpu(), 0, cpu().dw(0));
        addWord(cpu(), 0, cpu().dw(2));
        mem.writeWord(target, cpu().dw(0));
        addWord(cpu(), 0, 1);
        mem.writeWord(target + 0x50u, cpu().dw(0));
        cpu().setNZClearVC(cpu().dw(0), 0x8000u);
    };
    counterTiles(cpu().a[5]);
    counterTiles(cpu().a[5] + sext16(cpu().dw(1)));
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $004D60 (update_score_hud_and_check_extra_life) — a0 = a player's object. The score's last digit is cleared
// (the table holds the thresholds as ten thousands); at or above the next threshold the player gets one
// more life (BCD table entry 0, added to the byte before the player's specials), the sound $BE and the
// counters drawn again. Then the score.
// ---------------------------------------------------------------------------
void StreetsOfRage::update_score_hud_and_check_extra_life(m_long /*entry_*/) {
    traceEnter(0x00004D60u);
    auto &mem = memory();

    cpu().d[1] = 0x08;
    cpu().a[4] = kScoreP1;
    cpu().a[1] = kLivesViaPointsP1;
    if (cpu().a[0] != kP1Object) {
        cpu().d[1] = 0x3C;
        cpu().a[4] = kScoreP2;
        cpu().a[1] = kLivesViaPointsP2;
    }

    cpu().d[0] = 0;
    cpu().setDb(0, mem.readByte(cpu().a[1]));
    addWord(cpu(), 0, cpu().dw(0));
    addWord(cpu(), 0, cpu().dw(0));
    cpu().d[0] = mem.readLong(kExtraLifeThresholds + sext16(cpu().dw(0)));

    const m_long scoreAddress = cpu().a[4] - 1u;
    mem.writeLong(scoreAddress, mem.readLong(scoreAddress) & 0xFFFFFFF0u);
    cpu().d[2] = mem.readLong(scoreAddress);
    cmpValue(cpu(), cpu().d[2], cpu().d[0], 32);

    if (!cpu().cs()) {
        mem.writeByte(cpu().a[1], static_cast<m_byte>(addValue(cpu(), mem.readByte(cpu().a[1]), 1, 8)));
        cpu().d[7] = 0xFFFFFFBEu;
        SOR_CALL([this] { player_queue_sound_trampoline(); }, 0x00004D9Eu, 0x000035D6u);

        // movem.l d1/a4, -(sp)
        cpu().ssp -= 8;
        mem.writeLong(cpu().ssp, cpu().d[1]);
        mem.writeLong(cpu().ssp + 4, cpu().a[4]);

        cpu().a[6] = kP1SpecialAttacks;
        if (cpu().a[0] != kP1Object)
            cpu().a[6] = kP2SpecialAttacks;
        cpu().d[7] = 0;
        SOR_CALL([this] { add_bcd_resource_value(); }, 0x00004DB8u, 0x00010DCAu);
        SOR_CALL([this] { draw_player_lives_and_specials(); }, 0x00004DBCu, 0x00004E14u);

        // movem.l (sp)+, d1/a4
        cpu().d[1] = mem.readLong(cpu().ssp);
        cpu().a[4] = mem.readLong(cpu().ssp + 4);
        cpu().ssp += 8;
    }

    writeScoreTiles(cpu(), mem);
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $005620 / $00563E (hud_clear_p1_status_rows / hud_clear_p2_status_rows) — blank tile $6D7 over 9 longs of
// each of the two element rows on a player's side of the block (P1: $FF600A and $FF605A; P2: $FF6032 and
// $FF6082). Leaves d1 = $06D706D7 and a1 after the second fill.
// ---------------------------------------------------------------------------
void StreetsOfRage::hud_clear_p1_status_rows(m_long /*entry_*/) {
    traceEnter(0x00005620u);
    auto &mem = memory();

    cpu().d[1] = 0x06D706D7u;
    cpu().a[1] = kHudRows;
    fillLongs(cpu(), mem, 9);
    cpu().a[1] = 0x00FF605Au;
    fillLongs(cpu(), mem, 9);
    cpu().ssp += 4;
}

void StreetsOfRage::hud_clear_p2_status_rows(m_long /*entry_*/) {
    traceEnter(0x0000563Eu);
    auto &mem = memory();

    cpu().d[1] = 0x06D706D7u;
    cpu().a[1] = 0x00FF6032u;
    fillLongs(cpu(), mem, 9);
    cpu().a[1] = 0x00FF6082u;
    fillLongs(cpu(), mem, 9);
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $010DCA (add_bcd_resource_value) — d7 (a table index, times 4) selects a byte of the table at $10DDE, taken
// from the end of its long; it is added in BCD to the byte before a6, and both pointers move back one.
// X is cleared first; the result is in the BCD flags (C = X on a carry, Z cleared by a nonzero result).
// Callers: a life or special added (a6 = a counter + 1), the life taken, the round timer (d7 = $0C, one a tick).
// ---------------------------------------------------------------------------
void StreetsOfRage::add_bcd_resource_value(m_long /*entry_*/) {
    traceEnter(0x00010DCAu);
    auto &mem = memory();

    addWord(cpu(), 7, cpu().dw(7));
    addWord(cpu(), 7, cpu().dw(7));
    cpu().a[5] = kBcdTable + sext16(cpu().dw(7));
    cpu().setCCR(0x04u);

    cpu().a[5] -= 1;
    const m_byte source = mem.readByte(cpu().a[5]);
    cpu().a[6] -= 1;
    const m_long target = cpu().a[6];
    m_byte       dest   = mem.readByte(target);

    int low  = (dest & 0x0F) + (source & 0x0F) + (cpu().flagX() ? 1 : 0);
    int high = (dest >> 4) + (source >> 4);
    int carry = 0;
    if (low > 9) {
        low -= 10;
        ++high;
    }
    if (high > 9) {
        high -= 10;
        carry = 1;
    }
    dest = static_cast<m_byte>(((high & 0x0F) << 4) | (low & 0x0F));
    cpu().setFlag(CPU68K::FlagC, carry != 0);
    cpu().setFlag(CPU68K::FlagX, carry != 0);
    if (dest != 0)
        cpu().setFlag(CPU68K::FlagZ, false);
    cpu().setFlag(CPU68K::FlagN, (dest & 0x80u) != 0);
    mem.writeByte(target, dest);
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $010CA8 (hud_tick_round_timer) — d7 selects the step (a table index). Takes it off the timer ($FFFB01);
// at zero it raises the time-over flag, sets the sequence flags and posts sound $E1. Then, as $10CCC does,
// draws the two digits. Entered by update_game_clock each second (d7 = $0C) and by the level loader (d7 = $10).
// $10CCC (hud_draw_round_timer_digits) is its second half: the digits of the byte at a6 to the window plane,
// $FF602E (tens) and the cell after it (units), each digit two tiles tall.
// ---------------------------------------------------------------------------
void StreetsOfRage::hud_tick_round_timer(m_long /*entry_*/) {
    traceEnter(0x00010CA8u);
    auto &mem = memory();

    cpu().a[6] = kRoundTimerBcd;
    SOR_CALL([this] { add_bcd_resource_value(); }, 0x00010CB0u, 0x00010DCAu);
    if (mem.readByte(cpu().a[6]) == 0) {
        mem.writeByte(kTimeOverFlag, 0x01);
        mem.writeByte(kTimeOverSequence, 0x03);
        mem.writeByte(kTimeOverTimer, 0xFF);
        cpu().d[7] = 0xFFFFFFE1u;
        SOR_CALL([this] { queue_sound_id(); }, 0x00010CCCu, 0x0001069Eu);
    }
    // Falls through into $10CCC: the same frame, so no entry of its own.
    writeTimerDigits(cpu(), mem, [this](bool call) {
        if (call)
            logCallFromReturn(0x00010CE0u, 0x00010CE6u);
        traceEnter(0x00010CE6u);
    });
    cpu().ssp += 4;
}

void StreetsOfRage::hud_draw_round_timer_digits(m_long /*entry_*/) {
    traceEnter(0x00010CCCu);
    writeTimerDigits(cpu(), memory(), [this](bool call) {
        if (call)
            logCallFromReturn(0x00010CE0u, 0x00010CE6u);
        traceEnter(0x00010CE6u);
    });
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $011090 (hud_write_plate_row) — one tile row of a bar at the VDP data port a2: the plate (three longs of
// $110A8), then d1 five times. $1108E's `move.l (a1), d1` in front is the boss bar's, inlined in $10F80.
// ---------------------------------------------------------------------------
void StreetsOfRage::hud_write_plate_row(m_long /*entry_*/) {
    traceEnter(0x00011090u);
    auto &mem = memory();

    const m_long plate = mem.readLong(kHealthBarPlateRow);
    for (int i = 0; i < 3; ++i)
        mem.writeLong(cpu().a[2], plate);
    for (int i = 0; i < 5; ++i)
        mem.writeLong(cpu().a[2], cpu().d[1]);
    cpu().setNZClearVC(cpu().d[1], 0x80000000u);
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $011774 / $01177A (hud_draw_p1_score_digits / hud_draw_p2_score_digits) — the six digits of the player's
// score as tiles from $7DB at their side of the first row (P1: $FF6012, P2: $FF6046). Leaves a0 = the player.
// ---------------------------------------------------------------------------
namespace {

void drawScoreDigits(CPU68K &cpu, SystemMemory &mem) {
    cpu.d[1] = 0x08;
    cpu.a[4] = kScoreP1;
    if (cpu.a[0] != kP1Object) {
        cpu.d[1] = 0x3C;
        cpu.a[4] = kScoreP2;
    }
    writeScoreTiles(cpu, mem);
}

} // namespace

void StreetsOfRage::hud_draw_p1_score_digits(m_long entry_) {
    traceEnter(entry_);
    cpu().a[0] = kP1Object;
    drawScoreDigits(cpu(), memory());
    cpu().ssp += 4;
}

void StreetsOfRage::hud_draw_p2_score_digits(m_long entry_) {
    traceEnter(entry_);
    cpu().a[0] = kP2Object;
    drawScoreDigits(cpu(), memory());
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $0117BC (hud_draw_player_name_tiles) — a0 = a player's object. The tile list $117EC (P1) or $117F4 (P2)
// at their side of the first row: a count, a base and the tile bytes.
// ---------------------------------------------------------------------------
void StreetsOfRage::hud_draw_player_name_tiles(m_long /*entry_*/) {
    traceEnter(0x000117BCu);
    auto &mem = memory();

    if (cpu().a[0] == kP1Object) {
        cpu().d[0] = 0;
        cpu().a[3] = 0x000117ECu;
    } else {
        cpu().d[0] = 0x34;
        cpu().a[3] = 0x000117F4u;
    }
    cpu().a[4] = kHudRows + sext16(cpu().dw(0));
    const bool computer = computerLabel(cpu().a[3]);
    cpu().setDw(7, mem.readWord(cpu().a[3]));
    cpu().a[3] += 2;
    cpu().setDw(1, mem.readWord(cpu().a[3]));
    cpu().a[3] += 2;
    unsigned index = 0;
    do {
        cpu().d[0] = 0;
        const m_byte tile = mem.readByte(cpu().a[3]);
        cpu().setDb(0, computer && index < 4 ? kComputerLabel[index] : tile);
        ++index;
        cpu().a[3] += 1;
        addWord(cpu(), 0, cpu().dw(1));
        writeWordPostInc(cpu(), mem, 4, cpu().dw(0));
    } while (dbf(cpu(), 7));
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $0116DC (hud_blink_join_prompt) — a1 = the prompt's counter (the byte after it flips every 8 frames),
// a2 = its cell: blank tiles round the string $11754, or on the other beat $11764 (blanks).
// ---------------------------------------------------------------------------
void StreetsOfRage::hud_blink_join_prompt(m_long /*entry_*/) {
    traceEnter(0x000116DCu);
    auto &mem = memory();

    const m_byte count = static_cast<m_byte>(subValue(cpu(), mem.readByte(cpu().a[1]), 1, 8));
    mem.writeByte(cpu().a[1], count);
    if (count != 0) {
        cpu().ssp += 4;
        return;
    }
    mem.writeByte(cpu().a[1], 0x08);
    cpu().a[1] += 1;
    cpu().a[3] = 0x00011754u;
    if (testBitMem(cpu(), mem, cpu().a[1], 0)) {
        // bchg: the bit was set, so the other string
        mem.writeByte(cpu().a[1], static_cast<m_byte>(mem.readByte(cpu().a[1]) & ~1u));
        cpu().a[3] = 0x00011764u;
    } else {
        mem.writeByte(cpu().a[1], static_cast<m_byte>(mem.readByte(cpu().a[1]) | 1u));
    }

    cpu().setDw(2, kBlankTile);
    mem.writeWord(cpu().a[2] - 0x3Au, cpu().dw(2));
    SOR_CALL([this] { hud_write_tile_string(); }, 0x000116FCu, 0x00011708u);
    writeWordPostInc(cpu(), mem, 2, cpu().dw(2));
    SOR_CALL([this] { hud_write_tile_string(); }, 0x00011700u, 0x00011708u);
    writeWordPostInc(cpu(), mem, 2, cpu().dw(2));
    writeWordPostInc(cpu(), mem, 2, cpu().dw(2));
    writeWordPostInc(cpu(), mem, 2, cpu().dw(2));
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $0115CC (update_join_and_continue_hud) — each frame of the round: the JOIN prompt of an absent P2 (a
// blinking "PUSH START"; Start from P2's pad brings P2 in with the character P1 did not take, with two
// specials), P2's side blanked when both players are out, and the score, name and blinking prompt of a
// player waiting on the continue screen.
// ---------------------------------------------------------------------------
void StreetsOfRage::update_join_and_continue_hud(m_long /*entry_*/) {
    traceEnter(0x000115CCu);
    auto &mem = memory();

    if (mem.readWord(kPoliceSpecial) != 0)
        goto outP1;
    if (mem.readByte(kP2OutOrContinueFlag) != 0)
        goto outP1;
    if (mem.readWord(kLevel) == 0x0007u)
        goto outP1;
    if (mem.readByte(kDemoMode) == 0 && mem.readByte(kP2JoypadAbsent) != 0) {
        // A pad that is not there: only P2 already in (object type nonzero) leaves without drawing.
        if (mem.readByte(kP2Object) == 0)
            goto joinedPrompt;
        cpu().ssp += 4;
        return;
    }

    if ((mem.readByte(kPlayerMode) & 0x02u) != 0)
        goto outP1;
    if (mem.readByte(kP1Object) == 0x0Fu) {
        if (mem.readByte(kP2Object) == 0x0Fu)
            goto outP1;
        // Both are out: three blank cells at the join prompt, and P2's rows.
        cpu().setDw(1, kBlankTile);
        cpu().a[1] = 0x00FF6038u;
        for (int cell = 0; cell < 3; ++cell)
            writeWordPostInc(cpu(), mem, 1, cpu().dw(1));
        SOR_CALL([this] { hud_clear_p2_status_rows(); }, 0x00011626u, 0x0000563Eu);
        goto outP1;
    }

    {
        if (mem.readByte(kP2Object) == 0x0Fu)
            goto outP1;
        const m_byte count = static_cast<m_byte>(mem.readByte(kJoinBlinkP2) - 1u);
        mem.writeByte(kJoinBlinkP2, count);
        if (count != 0)
            goto outP1;
        mem.writeByte(kJoinBlinkP2, 0x08);

        if ((mem.readByte(kP2ButtonsHeld) & 0x80u) != 0 && mem.readByte(kPoliceSpecial) == 0) {
            // P2 joins: the character P1 did not take, two specials, the object started.
            mem.writeByte(kPlayerMode, static_cast<m_byte>(mem.readByte(kPlayerMode) | 0x02u));
            cpu().d[0] = 0;
            cpu().setDb(0, mem.readByte(kP1CharacterId));
            const m_byte partner = mem.readByte(kJoinCharacterTable + sext16(cpu().dw(0)));
            mem.writeByte(kP2CharacterIngame, partner);
            mem.writeByte(kP2CharacterId, partner);
            mem.writeByte(kP2SpecialAttacks, 0x02);
            mem.writeByte(kP2Object, 0x01);
            mem.writeByte(kP2ObjectFlags, static_cast<m_byte>(mem.readByte(kP2ObjectFlags) | 0x04u));
            mem.writeByte(kP2ObjectFlags1, static_cast<m_byte>(mem.readByte(kP2ObjectFlags1) | 0x01u));
            goto joinedPrompt;
        }
    }

    // The prompt alternates between its two strings.
    {
        const m_byte alternate = mem.readByte(kJoinAlternate);
        mem.writeByte(kJoinAlternate, static_cast<m_byte>(alternate ^ 0x01u));
        cpu().a[3] = 0x0001171Cu;
        if ((alternate & 0x01u) == 0)
            goto promptRows;
    }
joinedPrompt:
    cpu().a[3] = 0x00011738u;
promptRows:
    cpu().a[2] = 0x00FF6038u;
    SOR_CALL([this] { hud_write_tile_string(); }, 0x000116A2u, 0x00011708u);
    cpu().a[2] += 2;
    SOR_CALL([this] { hud_write_tile_string(); }, 0x000116A6u, 0x00011708u);
    cpu().a[2] += 2;
    SOR_CALL([this] { hud_write_tile_string(); }, 0x000116AAu, 0x00011708u);

outP1:
    if (mem.readByte(kP1OutOrContinueFlag) != 0) {
        cpu().a[1] = kJoinBlinkOutP1;
        cpu().a[2] = 0x00FF605Au;
        SOR_CALL([this] { hud_draw_p1_score_digits(); }, 0x000116BEu, 0x00011774u);
        SOR_CALL([this] { hud_draw_player_name_tiles(); }, 0x000116C2u, 0x000117BCu);
        SOR_CALL([this] { hud_blink_join_prompt(); }, 0x000116C4u, 0x000116DCu);
    }
    if (mem.readByte(kP2OutOrContinueFlag) == 0) {
        cpu().ssp += 4;
        return;
    }
    cpu().a[1] = kJoinBlinkOutP2;
    cpu().a[2] = 0x00FF608Eu;
    SOR_CALL([this] { hud_draw_p2_score_digits(); }, 0x000116D8u, 0x0001177Au);
    SOR_CALL([this] { hud_draw_player_name_tiles(); }, 0x000116DCu, 0x000117BCu);
    hud_blink_join_prompt(); // falls through into $116DC
}

// ---------------------------------------------------------------------------
// $011876 (hud_draw_object_health_bar, with the entry $1196A) — one boss bar. a4 -> a word pointer to the object, then the long
// where the bar's tile row is handed to the VBlank ($10F80); a6 -> the table row (+1: the object's type
// byte); a2 -> the ten cells of a full bar, a1 -> the ten cells to write in the bundle at $FFF500.
// Health is +$32 times 4 (nothing for another type or a negative health); above 80 a special bar, five
// longs of $86FA (above 96: $86FB); else the cells from a2 offset by the full eighths of 10 and the
// tile row for the remainder from $11930.
// ---------------------------------------------------------------------------
void StreetsOfRage::hud_draw_object_health_bar(m_long entry_) {
    auto &mem = memory();

    if (entry_ == 0x0001196Au) {
        // The end of the boss bars (`stage_clear_monitor` jumps here): the bundle's cap, five longs of $1197E
        // after the twelve bytes that follow the bar's ten cells.
        traceEnter(0x0001196Au);
        cpu().a[1] += 0x0C;
        cpu().a[5] = 0x0001197Eu;
        for (int i = 0; i < 5; ++i) {
            mem.writeLong(cpu().a[1], mem.readLong(cpu().a[5]));
            cpu().a[5] += 4;
            cpu().a[1] += 4;
        }
        cpu().setNZClearVC(mem.readLong(cpu().a[5] - 4u), 0x80000000u);
        cpu().ssp += 4;
        return;
    }
    traceEnter(0x00011876u);

    cpu().a[3] = sext16(mem.readWord(cpu().a[4]));
    cpu().a[4] += 2;
    cpu().setDb(0, mem.readByte(cpu().a[6] + 1u));
    cmpValue(cpu(), cpu().db(0), mem.readByte(cpu().a[3]), 8);
    bool health = cpu().eq();
    if (health) {
        cpu().setDw(0, mem.readWord(cpu().a[3] + 0x32u));
        health = (cpu().dw(0) & 0x8000u) == 0;
    }
    if (!health)
        cpu().d[0] = 0;

    cpu().setDw(0, static_cast<m_word>(lslValue(cpu(), cpu().dw(0), 2, 16)));
    cmpValue(cpu(), cpu().dw(0), 0x0051u, 16);
    if (!cpu().cs()) {
        cpu().d[1] = 0x86FA86FAu;
        cmpValue(cpu(), cpu().dw(0), 0x0061u, 16);
        if (!cpu().cs())
            cpu().d[1] = 0x86FB86FBu;
        fillLongs(cpu(), mem, 5);
        cpu().ssp += 4;
        return;
    }

    cpu().d[0] &= 0x0000007Fu;
    const m_long quotient  = cpu().d[0] / 8u;
    const m_long remainder = cpu().d[0] % 8u;
    cpu().d[0] = (remainder << 16) | quotient;
    cpu().d[1] = remainder;
    cpu().d[2] = 0x0A;
    cpu().d[3] = 0x0A;
    cpu().setDw(2, static_cast<m_word>(cpu().dw(2) - cpu().dw(0)));
    addWord(cpu(), 2, cpu().dw(2));
    cpu().a[2] += sext16(cpu().dw(2));
    cpu().setDw(3, 9);
    do {
        mem.writeWord(cpu().a[1], mem.readWord(cpu().a[2]));
        cpu().a[1] += 2;
        cpu().a[2] += 2;
    } while (dbf(cpu(), 3));

    cpu().setDw(1, static_cast<m_word>(lslValue(cpu(), cpu().dw(1), 2, 16)));
    cpu().a[5] = kBarSpecialShapes;
    const m_long shape = mem.readLong(cpu().a[5] + sext16(cpu().dw(1)));
    mem.writeLong(cpu().a[4], shape);
    cpu().setNZClearVC(shape, 0x80000000u);
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $008064 — copy a headered rectangle of tile words to the VDP
//
// a1 -> long VDP command, word columns-1, word rows-1, then the words. Each row
// is written at the command plus a 64-cell row stride. Registers and flags are
// left as the ROM leaves them: a1 past the data, d0 = command after the last row
// stride, d1 = columns-1, d2/d3 low words $FFFF, d4 = stride, a2/a3 the VDP ports.
//
// With --widescreen, the gameplay HUD block is reshaped on its way to the
// window plane: the yellow frame and the plate between the elements are
// dropped, and the two element rows move one tile down, out of the first
// tile row that Mode 48 does not display, and the plate colour is cleared from the HUD tiles once the level's
// HUD art is in VRAM. Work RAM still holds the original data.
// ---------------------------------------------------------------------------
void StreetsOfRage::vdp_copy_stride(m_long /*entry_*/) {
    traceEnter(0x8064u);

    auto &mem = memory();
    m_long source = cpu().a[1];

    const m_long command = mem.readLong(source);
    const m_word columns = mem.readWord(source + 4);
    const m_word rows    = mem.readWord(source + 6);
    source += 8;

    const bool widescreenHud = vdp().widescreen() && columns == kHudColumns &&
                               ((command == kHudStaticCommand && rows == kHudStaticRows) ||
                                (command == kHudBlockCommand && rows == kHudBlockRows) ||
                                (command == kHudBossCommand && rows == kHudBossRows));

    m_long rowCommand = command;
    if (widescreenHud) {
        // Every row but the last (the bottom edge) is an element row: it moves down one window row and loses
        // the frame. The static upload also blanks window row 0, which Mode 48 does not display.
        if (command == kHudStaticCommand) {
            hudPlatePending = true;
            mem.writeLong(kVdpControl, command);
            for (int cell = 0; cell < kHudCells; ++cell)
                mem.writeWord(kVdpData, 0);
        }
        if (hudPlatePending) {
            hudPlatePending = false;
            removeHudPlate(vdp());
        }
        for (m_word row = 0; row < rows; ++row) {
            mem.writeLong(kVdpControl, command + (static_cast<m_long>(row) + 1u) * kRowStride);
            for (int cell = 0; cell < kHudCells; ++cell) {
                m_word word = mem.readWord(source);
                source += 2;
                const m_word tile = word & 0x07FFu;
                if (tile >= kHudFrameFirstTile && tile <= kHudFrameLastTile)
                    word = 0;
                mem.writeWord(kVdpData, word);
            }
        }
        source += kHudCells * 2; // bottom edge row
        // Leave d0 where the ROM does: the command after the stride add of its last row.
        rowCommand = command + (static_cast<m_long>(rows) + 1u) * kRowStride;
    } else {
        for (m_word row = 0; row <= rows; ++row) {
            mem.writeLong(kVdpControl, rowCommand);
            for (m_word column = 0; column <= columns; ++column) {
                mem.writeWord(kVdpData, mem.readWord(source));
                source += 2;
            }
            rowCommand += kRowStride;
        }
    }

    const m_long previous = rowCommand - kRowStride;
    const bool   carry    = rowCommand < previous;
    const bool   overflow = (((previous ^ rowCommand) & (kRowStride ^ rowCommand)) & 0x80000000u) != 0;
    cpu().a[1] = source;
    cpu().a[2] = kVdpControl;
    cpu().a[3] = kVdpData;
    cpu().d[0] = rowCommand;
    cpu().setDw(1, columns);
    cpu().setDw(2, 0xFFFFu);
    cpu().setDw(3, 0xFFFFu);
    cpu().d[4] = kRowStride;
    cpu().setNZVCX(rowCommand, 0x80000000u, overflow, carry, carry);
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $010F80 — gameplay VBlank HUD update: tilemap bundles and health-bar tile rows
//
// The routine's own bar uploads ($11072 player, $1108E boss) are inlined; nothing
// else calls them. A bar tile is eight longs of 4bpp rows: plate rows from $110A8
// around five rows of the bar colour. With --widescreen the plate rows are
// transparent, as the rest of the HUD plate is (see vdp_copy_stride).
// ---------------------------------------------------------------------------
void StreetsOfRage::vblank_update_gameplay_health_bars(m_long /*entry_*/) {
    traceEnter(0x00010F80u);

    auto &mem = memory();

    const m_long plate = vdp().widescreen() ? 0u : mem.readLong(kHealthBarPlateRow);
    const auto   push  = [&](m_long value) {
        mem.writeLong(kVdpData, value);
    };
    const auto playerBar = [&](m_long pointerSlot, m_long source) {
        mem.writeLong(pointerSlot, 0);
        const m_long bar = mem.readLong(source);
        push(plate);
        for (int row = 0; row < 5; ++row)
            push(bar);
        push(plate);
        push(plate);
        cpu().d[1] = bar;
    };
    const auto bossBar = [&](m_long source) {
        const m_long bar = mem.readLong(source);
        push(plate);
        push(plate);
        push(plate);
        for (int row = 0; row < 5; ++row)
            push(bar);
        cpu().d[1] = bar;
    };
    // Soft jsr into generated code, as in game_infinite_loop.
    const auto call68k = [this](auto &&fn, m_long retPc, m_long target) {
        logCallFromReturn(retPc, target);
        cpu().ssp -= 4;
        memory().writeLong(cpu().ssp, retPc);
        fn();
    };
    const auto bundle = [&](m_byte id, m_long retPc) {
        cpu().d[0] = id;
        call68k([this] { load_vdp_tilemap_bundle(); }, retPc, 0x0000A5F4u);
    };

    const m_word state = mem.readWord(kGameState);
    const bool   inGameplay = state == 0x28u || state == 0x2Au || (state >= 0x14u && state < 0x1Cu);
    if (!inGameplay) {
        cpu().ssp += 4;
        return;
    }

    bundle(3, 0x00010FAAu);

    // Request bit 0 of $FFFA7A: blank the boss bar tiles at VRAM $D240 (11 x 49 longs).
    const m_byte clearRequest = mem.readByte(kHudClearRequest);
    if ((clearRequest & 1u) != 0) {
        mem.writeByte(kHudClearRequest, static_cast<m_byte>(clearRequest & ~1u));
        mem.writeLong(kVdpControl, 0x52400003u);
        for (int block = 0; block < 11; ++block) {
            cpu().d[1] = 0;
            cpu().a[1] = kVdpData;
            call68k([this] { vdp_fill_long_49(); }, 0x00010FCCu, 0x00010412u);
        }
    }

    const bool offer = mem.readByte(kMrXOfferFlag) != 0;
    if (!offer)
        mem.writeWord(kVdpControl, 0x9204u);

    if (state != 0x18u && state != 0x1Au) {
        if (mem.readByte(kFinalBossPresentation) != 0)
            mem.writeWord(kVdpControl, 0x9205u);
        if (mem.readByte(kBossHealthSwitch) != 0) {
            if (!offer)
                mem.writeWord(kVdpControl, 0x9205u);
            bundle(9, 0x00011018u);
        }
    }

    cpu().a[2] = kVdpData;
    const m_long playerOne = mem.readLong(kHealthBarPointers);
    if (playerOne != 0) {
        mem.writeLong(kVdpControl, 0x5B400003u);
        playerBar(kHealthBarPointers, playerOne);
    }
    const m_long playerTwo = mem.readLong(kHealthBarPointers + 4);
    if (playerTwo != 0) {
        mem.writeLong(kVdpControl, 0x5B600003u);
        playerBar(kHealthBarPointers + 4, playerTwo);
        cpu().ssp += 4;
        return;
    }

    if (mem.readWord(kBossHealthPrimary) != 0) {
        mem.writeLong(kVdpControl, 0x5F800003u);
        bossBar(kBossHealthPrimary + 2);
    }
    if (mem.readWord(kBossHealthSecondary) != 0) {
        mem.writeLong(kVdpControl, 0x5FA00003u);
        bossBar(kBossHealthSecondary + 2);
    }
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $01852C / $018532 (round_clear_build_row_highlighted / round_clear_build_row) — a1 -> a row of the round-clear
// tally: a VDP command long, then tile bytes up to $FF. It goes to the scratch buffer as the command, a long
// $00160000 and the words $7C0 + byte + d7 (palette line 2 for the highlighted row, 1 for the other); the upload
// is requested and the substate moves on. Under 1 PLAYER AI ASSIST the P2 row's "2UP" reads "COM" (user).
// ---------------------------------------------------------------------------
namespace {

void buildTallyRow(CPU68K &cpu, SystemMemory &mem) {
    const bool computer = cpu.a[1] == kP2TallyRow && SoRRemoteInput::aiAssist();
    cpu.a[4] = kScratchBuffer;
    mem.writeLong(cpu.a[4], mem.readLong(cpu.a[1]));
    cpu.a[1] += 4;
    cpu.a[4] += 4;
    mem.writeLong(cpu.a[4], 0x00160000u);
    cpu.a[4] += 4;
    unsigned index = 0;
    while (true) {
        cpu.d[0] = 0;
        const m_byte tile = mem.readByte(cpu.a[1]);
        cpu.setDb(0, tile);
        cpu.a[1] += 1;
        if (tile == 0xFFu)
            break;
        if (computer && index >= 8 && index < 11)
            cpu.setDb(0, kComputerTallyLabel[index - 8]);
        ++index;
        addWord(cpu, 0, 0x07C0u);
        addWord(cpu, 0, cpu.dw(7));
        writeWordPostInc(cpu, mem, 4, cpu.dw(0));
    }
    mem.writeByte(kRoundClearVblankRequest, 0x01);
    mem.writeWord(kRoundClearSubstate, static_cast<m_word>(mem.readWord(kRoundClearSubstate) + 2u));
    cpu.ssp += 4;
}

} // namespace

void StreetsOfRage::round_clear_build_row_highlighted(m_long /*entry_*/) {
    traceEnter(0x0001852Cu);
    cpu().setDw(7, 0x4000u);
    buildTallyRow(cpu(), memory());
}

void StreetsOfRage::round_clear_build_row(m_long /*entry_*/) {
    traceEnter(0x00018532u);
    cpu().setDw(7, 0x2000u);
    buildTallyRow(cpu(), memory());
}
