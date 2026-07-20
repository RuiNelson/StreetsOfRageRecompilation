#include "Sor.hpp"
#include "SorCheats.hpp"

#include <cstdint>

namespace {

// VBlank self-mailbox: game code posts 1 or 2; vblank_handler clears it to 0.
constexpr m_long kVBlankMailbox = 0xFFFFFA00u;
constexpr m_long kHalfDamage    = 0xFFFFFA43u;

// Main game-state word and the long jump table at $3BA (ROM).
constexpr m_long kGameState      = 0xFFFFFF00u;
constexpr m_long kLevel          = 0xFFFFFF02u;
constexpr m_long kStateJumpTable = 0x000003BAu;

constexpr m_long kP1SpecialAttacks          = 0xFFFFFF21u;
constexpr m_long kP2SpecialAttacks          = 0xFFFFFF24u;
constexpr m_long kPoliceSpecialActive       = 0xFFFFFA1Au;
constexpr m_long kPoliceSpecialCaller       = 0xFFFFFA1Cu;
constexpr m_long kLevelIntroActive          = 0xFFFFFA1Fu;
constexpr m_long kPoliceSpecialBlastActive  = 0xFFFFFA41u;
constexpr m_long kScreenShakeActive         = 0xFFFFFA44u;
constexpr m_long kPoliceSpecialImpactFlags  = 0xFFFFFA51u;
constexpr m_long kWaveAdvancePending        = 0xFFFFFA0Du;
constexpr m_long kRound7CameraBlocker1      = 0xFFFFFA14u;
constexpr m_long kRound7CameraBlocker2      = 0xFFFFFA15u;
constexpr m_long kPoliceSpecialBlastLane    = 0xFFFFFB40u;
constexpr m_long kPalette                   = 0xFFFFF400u;
constexpr m_long kPoliceSpecialPalette      = 0xFFFFDD80u;

// Attack-strength lookup table in ROM (word offsets from the table base).
constexpr m_long kAttackStrengthTable = 0x0000423Cu;

// Object (a0) field offsets used by the attack-strength routine.
constexpr m_long kObjTableKey     = 0x08; // word → index into kAttackStrengthTable
constexpr m_long kObjStrengthBias = 0x0A; // byte added into the per-move row
constexpr m_long kObjDamage       = 0x34; // byte: low nibble = applied damage
constexpr m_long kObjHitFlags     = 0x42; // byte: high nibble of raw strength → reaction
constexpr m_long kObjMoveIndex    = 0x50; // byte: row selector inside the move table

// Supervisor mode, interrupt mask 2 — allows VBlank (level 6) while waiting.
constexpr m_word kStatusIrqEnabled = 0x2500u;

} // namespace

// ---------------------------------------------------------------------------
// $0003A2 — game_infinite_loop
//
// Core frame loop: index the $3BA jump table by game_state, jsr the handler,
// then sync_z80_1 (VBlank mailbox). IRQ service is not needed between those
// few setup opcodes — handlers and sync_z80_1 already deliver IRQs. Also owns
// the mid-entry at $412 (boot checksum-fail: clear CRAM and hang).
// ---------------------------------------------------------------------------
void Sor::game_infinite_loop(m_long entry_) {
    // Soft 68000 jsr/rts. Mirrors CALL / CALL_DISPATCH in generated code: push
    // return PC, call, abort if the callee unwound past this frame.
    const auto call68k = [this](auto &&fn, m_long retPc) -> bool {
        const m_long spBefore = cpu().ssp;
        cpu().ssp -= 4;
        memory().writeLong(cpu().ssp, retPc);
        fn();
        return (cpu().ssp & 0x00FFFFFFu) <= (spBefore & 0x00FFFFFFu);
    };

    if (entry_ == 0x0412u) {
        // Checksum mismatch path from init ($348 bne.w $412).
        traceEnter(0x0412u);

        if (!call68k([this] { init_joypad(); }, 0x0418u))
            return;

        cpu().a[1] = 0x00C00000u; // vdp_data
        cpu().d[1] = 0x000E000Eu;
        memory().writeLong(0x00C00004u, 0xC0000000u); // CRAM write address
        if (!call68k([this] { memfill_long_64(); }, 0x0434u))
            return;

        // Host-friendly stand-in for `bra.s *`: sleep on IRQs/quit, do not spin.
        while (!shouldQuit()) {
            waitForInterrupt();
            if (irqLevel() > cpu().interruptMask())
                serviceIRQ();
        }
        return;
    }

    traceEnter(0x03A2u);

    while (!shouldQuit()) {
        // moveq #0,d0 / move.w (game_state).w,d0 / add.w d0,d0
        const m_word state       = memory().readWord(kGameState);
        const m_word tableOffset = static_cast<m_word>(state + state);

        // move.l $3BA(pc,d0.w),d0 / movea.l d0,a0  (SEX_W on the word offset)
        const m_long handler =
            memory().readLong(kStateJumpTable + static_cast<m_long>(static_cast<std::int16_t>(tableOffset)));
        cpu().d[0] = handler;
        cpu().a[0] = handler;

        // jsr (a0) — state init/update handler
        if (!call68k([this, handler] { dispatch(handler); }, 0x03B2u))
            return;

        // jsr sync_z80_1 — waits on VBlank mailbox; services IRQs there
        if (!call68k([this] { sync_z80_1(); }, 0x03B8u))
            return;
    }
}

// ---------------------------------------------------------------------------
// $003FCC — activate a police special
//
// Preserves the cartridge gate and sequence. Alt/Option+W supplies a one-shot
// caller request from the SDL thread; that request replaces the button/resource
// checks and decrement, so the normal event runs without consuming a special.
// ---------------------------------------------------------------------------
void Sor::try_activate_police_special(m_long /*entry_*/) {
    traceEnter(0x3FCCu);

    const auto call68k = [this](auto &&fn, m_long retPc) -> bool {
        const m_long spBefore = cpu().ssp;
        cpu().ssp -= 4;
        memory().writeLong(cpu().ssp, retPc);
        fn();
        return (cpu().ssp & 0x00FFFFFFu) <= (spBefore & 0x00FFFFFFu);
    };

    const m_long object   = cpu().a[0];
    const bool freeCall   = SorCheats::consumeFreePoliceCall(object);
    const m_word level    = memory().readWord(kLevel);
    const bool round7Busy = level == 6u &&
                            (memory().readByte(kWaveAdvancePending) != 0u ||
                             memory().readByte(kRound7CameraBlocker1) != 0u ||
                             memory().readByte(kRound7CameraBlocker2) != 0u);

    if ((!freeCall && (memory().readByte(object + 0x55u) & 0x40u) == 0u) ||
        memory().readWord(object + 0x32u) == 0u ||
        memory().readByte(kLevelIntroActive) != 0u ||
        (memory().readByte(object + 0x4Bu) & 0x01u) != 0u ||
        round7Busy || memory().readByte(kPoliceSpecialActive) != 0u) {
        cpu().ssp += 4;
        return;
    }

    const bool p1 = (object & 0x00FFFFFFu) == SorCheats::kP1Object;
    const m_long specialCounter = p1 ? kP1SpecialAttacks : kP2SpecialAttacks;
    if (!freeCall && memory().readByte(specialCounter) == 0u) {
        cpu().ssp += 4;
        return;
    }

    cpu().d[0] = p1 ? 0u : 1u;
    cpu().a[1] = specialCounter;
    if (!freeCall)
        memory().writeByte(specialCounter, static_cast<m_byte>(memory().readByte(specialCounter) - 1u));
    memory().writeByte(kPoliceSpecialCaller, cpu().db(0));

    if (!call68k([this] { draw_player_lives_and_specials(); }, 0x4038u))
        return;

    memory().writeByte(kPoliceSpecialImpactFlags, 0u);
    memory().writeByte(kPoliceSpecialBlastActive, 0u);
    memory().writeWord(kPoliceSpecialBlastLane, 0u);
    memory().writeByte(kScreenShakeActive, 0u);

    if (!call68k([this] { prepare_ordinary_enemies_for_police_special(); }, 0x404Cu))
        return;

    memory().writeWord(kPoliceSpecialActive, 0x0101u);
    cpu().a[1] = kPalette;
    cpu().a[2] = kPoliceSpecialPalette;
    if (!call68k([this] { memcopy_128(); }, 0x4060u))
        return;

    if (level == 2u)
        memory().writeWord(0xFFFFDDDAu, 0x0668u);

    if (level != 6u) {
        memory().writeLong(0xFFFFE00Eu, 0u);
        memory().writeLong(0xFFFFE10Eu, 0u);
        memory().writeLong(0xFFFFE012u, 0u);
        memory().writeLong(0xFFFFE112u, 0u);
        if (level == 4u) {
            memory().writeLong(0xFFFFE1AAu, 0xFFFFF800u);
            memory().writeWord(0xFFFFE1A8u, 0x1428u);
            memory().writeLong(0xFFFFE1AEu, 0u);
        }
    }

    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $0041EA — compute attack strength for the object in a0
//
// Looks up a raw strength byte from ROM table $423C (indexed by object+$08 and
// object+$50, biased by object+$0A). Optionally triples the low nibble when
// half_damage is set (P1-vs-P2). Stores damage in object+$34 and packs the
// original high nibble into object+$42. Host cheat may boost P1 damage only.
// ---------------------------------------------------------------------------
void Sor::compute_player_attack_descriptor(m_long /*entry_*/) {
    traceEnter(0x41EAu);

    const m_long object = cpu().a[0];

    // table_index = (object.tableKey >> 1) & ~1 — even word offset into the table
    const m_word tableKey   = memory().readWord(object + kObjTableKey);
    const m_word tableIndex = static_cast<m_word>((tableKey >> 1) & 0xFFFEu);
    const m_word rowOffset  = memory().readWord(kAttackStrengthTable + tableIndex);
    const m_long rowBase =
        kAttackStrengthTable + static_cast<m_long>(static_cast<std::int16_t>(rowOffset));

    const m_byte moveIndex = memory().readByte(object + kObjMoveIndex);
    const m_byte bias      = memory().readByte(object + kObjStrengthBias);

    // First byte at row[move] is a secondary index; final strength at row[3 + index + bias].
    const m_byte secondary = memory().readByte(rowBase + moveIndex);
    const m_byte lookup    = static_cast<m_byte>(secondary + bias);
    m_byte       strength  = memory().readByte(rowBase + 3 + lookup);

    // half_damage: keep high nibble, replace low nibble with 3× low nibble
    // (add.b d0,d0; add.b d2,d0; add.b d1,d0 with d0=low, d1=high).
    if (memory().readByte(kHalfDamage) != 0) {
        const m_byte high = static_cast<m_byte>(strength & 0xF0u);
        const m_byte low  = static_cast<m_byte>(strength & 0x0Fu);
        strength          = static_cast<m_byte>(static_cast<unsigned>(low) * 3u + high);
    }

    // Damage nibble only at +$34; reaction bits from the *original* high nibble
    // (before the memory mask) are merged into +$42 with bit 0 cleared.
    const m_byte reactionNibble = static_cast<m_byte>(strength >> 4);
    m_byte       damage         = static_cast<m_byte>(strength & 0x0Fu);
    damage = SorCheats::adjustP1PunchDamage(object, damage, SorCheats::p1PunchPowerEnabled());

    memory().writeByte(object + kObjDamage, damage);

    const m_byte hitFlags =
        static_cast<m_byte>((memory().readByte(object + kObjHitFlags) & 0xFEu) | reactionNibble);
    memory().writeByte(object + kObjHitFlags, hitFlags);

    // Same as RETURN_68K() in generated code.
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $010502 / $010514 — wait for VBlank via the $FFFA00 mailbox
//
// Posts a command byte; vblank_handler consumes it (and runs DMA/sound work)
// then clears the mailbox. Host waits on interrupt instead of a tight spin.
// ---------------------------------------------------------------------------
void Sor::sync_z80_1(m_long /*entry_*/) {
    traceEnter(0x00010502u);

    memory().writeByte(kVBlankMailbox, 1);
    cpu().setStatus(kStatusIrqEnabled);

    memory().waitForByteValue(kVBlankMailbox, 0, [this] {
        waitForInterrupt();
        if (irqLevel() > cpu().interruptMask())
            serviceIRQ();
        return !shouldQuit();
    });

    cpu().ssp += 4;
}

void Sor::sync_z80_2(m_long /*entry_*/) {
    traceEnter(0x00010514u);

    memory().writeByte(kVBlankMailbox, 2);
    cpu().setStatus(kStatusIrqEnabled);

    memory().waitForByteValue(kVBlankMailbox, 0, [this] {
        waitForInterrupt();
        if (irqLevel() > cpu().interruptMask())
            serviceIRQ();
        return !shouldQuit();
    });

    cpu().ssp += 4;
}
