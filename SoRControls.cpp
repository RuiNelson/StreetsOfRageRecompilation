#include "SoRCheats.hpp"
#include "SoRControlsMenu.hpp"
#include "SoRInput.hpp"
#include "SoRPython.hpp"
#include "SoRRemoteInput.hpp"
#include "SoR.hpp"

#include <cstdint>

namespace {

constexpr m_long kP1Object        = 0xFFFFB800u;
constexpr m_long kP2Object        = 0xFFFFB880u;
constexpr m_long kObjState        = 0x30u;
constexpr m_long kPlayerInputWord = 0x54u;
constexpr m_long kPlayerPressByte = 0x55u;

constexpr m_long kIoPlayer1DataPort = 0x00A10003u;
constexpr m_long kIoPlayer2DataPort = 0x00A10005u;
constexpr m_long kZ80BusRequest     = 0x00A11100u;

constexpr m_long kStopMusic           = 0xFFFFF002u;
constexpr m_long kPlaySe              = 0xFFFFF00Au;
constexpr m_long kPauseTextFlag       = 0xFFFFFA46u;
constexpr m_long kP1StartButtonBuffer = 0xFFFFFA47u;
constexpr m_long kP2StartButtonBuffer = 0xFFFFFA48u;
constexpr m_long kFadeOutFlag         = 0xFFFFFA71u;
constexpr m_long kPaletteFadeCounter  = 0xFFFFFB0Cu;

constexpr m_long kCharSelectConfirmCount = 0xFFFFF908u;
constexpr m_long kSelectScreenSubstate   = 0xFFFFFB0Eu;
constexpr m_long kCheatFlag              = 0xFFFFFA7Bu;

constexpr m_long kP1ButtonHeld  = 0xFFFFFC04u;
constexpr m_long kP1ButtonPress = 0xFFFFFC05u;
constexpr m_long kP2ButtonHeld  = 0xFFFFFC08u;
constexpr m_long kP2ButtonPress = 0xFFFFFC09u;

constexpr m_long kPlayerMode    = 0xFFFFFF18u;
constexpr m_long kP1CharacterId = 0xFFFFFF1Eu;
constexpr m_long kP2CharacterId = 0xFFFFFF1Fu;
constexpr m_long kDemoMode      = 0xFFFFFF2Au;
constexpr m_long kControlScheme = 0xFFFFFFC8u;

constexpr m_long kDemoAiInputP1 = 0xFFFFFF2Cu;
constexpr m_long kDemoAiInputP2 = 0xFFFFFF30u;

constexpr m_long kCharSelectCursorJt  = 0x000018F6u;
constexpr m_long kCharNavRight        = 0x000019E4u;
constexpr m_long kCharNavLeft         = 0x000019EAu;
constexpr m_long kCharOldPalette      = 0x000019F0u;
constexpr m_long kCharNewPalette      = 0x000019F6u;
constexpr m_long kCharCursorX         = 0x000019FCu;
constexpr m_long kCharPreviewPointers = 0x00001A02u;
constexpr m_long kCharacterIdFromSlot = 0x00001A0Eu;

constexpr m_byte kButtonStart = 0x80u;
constexpr m_byte kButtonX     = 0x40u;

constexpr m_long signExtendWord(m_word value) {
    return static_cast<m_long>(static_cast<std::int32_t>(static_cast<std::int16_t>(value)));
}

bool bit(m_byte value, unsigned index) {
    return (value & static_cast<m_byte>(1u << index)) != 0;
}

void setBit(SystemMemory &memory, m_long address, unsigned index) {
    memory.writeByte(address, static_cast<m_byte>(memory.readByte(address) | (1u << index)));
}

void return68k(CPU68K &cpu) {
    cpu.ssp += 4;
}

void setMoveByteFlags(CPU68K &cpu, m_byte value) {
    cpu.setNZClearVC(value, 0x80u);
}

void setMoveWordFlags(CPU68K &cpu, m_word value) {
    cpu.setNZClearVC(value, 0x8000u);
}

void setCmpByteFlags(CPU68K &cpu, m_byte destination, m_byte source) {
    const m_word difference = static_cast<m_word>(destination) - static_cast<m_word>(source);
    const m_byte result     = static_cast<m_byte>(difference);
    const bool   carry      = (difference & 0x100u) != 0u;
    const bool   overflow   = (((destination ^ source) & (destination ^ result)) & 0x80u) != 0u;
    cpu.setNZVC(result, 0x80u, overflow, carry);
}

void setActiveHighButton(m_byte &buttons, bool pressed, m_byte mask) {
    if (pressed)
        buttons = static_cast<m_byte>(buttons | mask);
}

int altPickupPlayerIndexForBuffer(m_long bufferAddress) {
    const m_long normalized = bufferAddress & 0x00FFFFFFu;
    if (normalized == (kP1ButtonHeld & 0x00FFFFFFu))
        return 0;
    if (normalized == (kP2ButtonHeld & 0x00FFFFFFu))
        return 1;
    return -1;
}

void setShiftLeftByteX(CPU68K &cpu, m_byte value, int count) {
    bool carry = false;
    for (int i = 0; i < count; ++i) {
        carry = (value & 0x80u) != 0u;
        value = static_cast<m_byte>(value << 1);
    }
    cpu.setFlag(CPU68K::FlagX, carry);
}

void setShiftLeftWordX(CPU68K &cpu, m_word value, int count) {
    bool carry = false;
    for (int i = 0; i < count; ++i) {
        carry = (value & 0x8000u) != 0u;
        value = static_cast<m_word>(value << 1);
    }
    cpu.setFlag(CPU68K::FlagX, carry);
}

void setShiftRightWordX(CPU68K &cpu, m_word value, int count) {
    bool carry = false;
    for (int i = 0; i < count; ++i) {
        carry = (value & 1u) != 0u;
        value = static_cast<m_word>(value >> 1);
    }
    cpu.setFlag(CPU68K::FlagX, carry);
}

using SoRControlsConfig::Action;
using SoRControlsConfig::actionBit;

// One player's input for the sampler: their actions (keyboard and gamepad
// through controls.yaml, ORed with a remote client's, SoRRemoteInput) and
// whether they use the ENHANCED 6 scheme.
struct SampledPlayer {
    SoRInput::ActionMask actions  = 0;
    bool                 enhanced = false;
};

SampledPlayer samplePlayer(int playerIndex) {
    const SoRInput::Scheme scheme  = SoRInput::scheme(playerIndex);
    // With AI ASSIST on, P2 is the AI's alone: no physical device reaches it.
    const bool             aiPlays = playerIndex == 1 && SoRRemoteInput::aiAssist();
    SoRInput::ActionMask   actions = static_cast<SoRInput::ActionMask>(
        (aiPlays ? SoRInput::ActionMask{0} : SoRInput::physicalActions(playerIndex)) |
        SoRRemoteInput::actions(playerIndex));
    actions = static_cast<SoRInput::ActionMask>(actions & SoRControlsConfig::schemeActions(scheme));

    // Opposite directions cancel each other, as a real pad cannot report both.
    const auto cancel = [&actions](Action a, Action b) {
        const SoRInput::ActionMask pair = actionBit(a) | actionBit(b);
        if ((actions & pair) == pair)
            actions = static_cast<SoRInput::ActionMask>(actions & ~pair);
    };
    cancel(Action::Up, Action::Down);
    cancel(Action::Left, Action::Right);
    return {actions, scheme == SoRInput::Scheme::Enhanced6};
}

bool hasAction(SoRInput::ActionMask actions, Action action) {
    return (actions & actionBit(action)) != 0;
}

void sampleOneJoypadBody(CPU68K &cpu, SystemMemory &memory, const SampledPlayer *player) {
    const int    altPickupPlayerIndex = altPickupPlayerIndexForBuffer(cpu.a[0]);
    const m_byte oldHeld              = memory.readByte(cpu.a[0]);

    memory.writeByte(cpu.a[1], 0x00u);
    cpu.setDb(0, memory.readByte(cpu.a[1]));
    cpu.setDb(3, cpu.db(0));

    setShiftLeftByteX(cpu, cpu.db(0), 2);
    cpu.setDb(0, static_cast<m_byte>((cpu.db(0) << 2) & 0xC0u));

    memory.writeByte(cpu.a[1], 0x40u);
    cpu.setDb(1, static_cast<m_byte>(memory.readByte(cpu.a[1]) & 0x3Fu));
    cpu.setDb(4, cpu.db(1));

    m_byte held = static_cast<m_byte>(~static_cast<m_byte>(cpu.db(0) | cpu.db(1)));

    // The game reads SDL itself and binds actions (SoRInput, controls.yaml).
    //
    // The port reads above still run exactly as in the ROM: they set the
    // pad-absent byte below. MegaDriveEnvironment no longer reads SDL, so the
    // held byte is replaced by the one the player's actions make, in the
    // active-high format the original player routines expect: Punch $10,
    // Jump $20, Special $40 (the police call), Start $80. ENHANCED 6 adds
    // Back Attack as Punch+Jump ($30, the ROM's own back attack through
    // $322A) and Pick Item / Throw Knife, which are not ROM bits: they are
    // native per-player presses, like the native Punch edge, read by the
    // $3028 / $3084 ports. In the attract demo only Start is kept, below.
    if (player != nullptr) {
        const SoRInput::ActionMask actions = player->actions;
        m_byte                     sampled = 0;
        setActiveHighButton(sampled, hasAction(actions, Action::Up), 0x01u);
        setActiveHighButton(sampled, hasAction(actions, Action::Down), 0x02u);
        setActiveHighButton(sampled, hasAction(actions, Action::Left), 0x04u);
        setActiveHighButton(sampled, hasAction(actions, Action::Right), 0x08u);
        setActiveHighButton(sampled, hasAction(actions, Action::Punch), 0x10u);
        setActiveHighButton(sampled, hasAction(actions, Action::Jump), 0x20u);
        setActiveHighButton(sampled, hasAction(actions, Action::BackAttack), 0x30u);
        setActiveHighButton(sampled, hasAction(actions, Action::Special), kButtonX);
        setActiveHighButton(sampled, hasAction(actions, Action::Start), kButtonStart);
        held = sampled;
    }

    if (player != nullptr && player->enhanced && memory.readByte(kDemoMode) == 0u) {
        SoRCheats::updateAltAttackButton(altPickupPlayerIndex, hasAction(player->actions, Action::Punch));
        SoRCheats::updateAltPickupButton(altPickupPlayerIndex, hasAction(player->actions, Action::Pickup));
        SoRCheats::updateAltThrowButton(altPickupPlayerIndex, hasAction(player->actions, Action::Throw));
    } else {
        SoRCheats::updateAltAttackButton(altPickupPlayerIndex, false);
        SoRCheats::updateAltPickupButton(altPickupPlayerIndex, false);
        SoRCheats::updateAltThrowButton(altPickupPlayerIndex, false);
    }

    cpu.setDb(0, held);
    cpu.setDb(1, cpu.db(0));
    cpu.setDb(2, oldHeld);
    cpu.setDb(0, static_cast<m_byte>(cpu.db(0) ^ cpu.db(2)));

    if (memory.readByte(kDemoMode) != 0u) {
        cpu.a[3] = memory.readLong(cpu.a[2]);
        cpu.setDb(1, static_cast<m_byte>((cpu.db(1) & 0x80u) | memory.readByte(cpu.a[3])));
        cpu.a[3] += 1;
        cpu.setDb(0, static_cast<m_byte>((cpu.db(0) & 0x80u) | memory.readByte(cpu.a[3])));
        cpu.a[3] += 1;
        memory.writeLong(cpu.a[2], cpu.a[3]);
    }

    memory.writeByte(cpu.a[0], cpu.db(1));
    cpu.a[0] += 1;
    cpu.setDb(0, static_cast<m_byte>(cpu.db(0) & cpu.db(1)));
    memory.writeByte(cpu.a[0], cpu.db(0));
    cpu.a[0] += 1;

    memory.writeByte(cpu.a[0], 0);
    setCmpByteFlags(cpu, cpu.db(4), cpu.db(3));
    if (cpu.eq()) {
        memory.writeByte(cpu.a[0], 1);
        setMoveByteFlags(cpu, 1);
    }
}

} // namespace

#define SOR_CONTROLS_CALL_68K(expression, returnPc, target)                                                            \
    do {                                                                                                               \
        const m_long sorCallSp = cpu().ssp;                                                                            \
        logCallFromReturn(static_cast<m_long>(returnPc), static_cast<m_long>(target));                                 \
        cpu().ssp -= 4;                                                                                                \
        memory().writeLong(cpu().ssp, static_cast<m_long>(returnPc));                                                  \
        expression;                                                                                                    \
        if ((cpu().ssp & 0x00FFFFFFu) > (sorCallSp & 0x00FFFFFFu))                                                     \
            return;                                                                                                    \
    } while (false)

// $001360 -- OPTIONS sub-state $18: highlight the CONTROLS row (the ROM's
// CONTROL row, whose A/B/C layouts the bindings replace).
void StreetsOfRage::options_highlight_controls(m_long /*entry_*/) {
    traceEnter(0x1360u);
    memory().writeWord(kSelectScreenSubstate, static_cast<m_word>(memory().readWord(kSelectScreenSubstate) + 2));
    SoRControlsMenu::drawOptionsRow(memory(), true);
    return68k(cpu());
}

// $001390 -- OPTIONS sub-state $1A: the CONTROLS row. Up/Down leave it as
// every row does; Punch, Jump or Special opens the CONTROLS menu, which then
// runs here one frame per call until it closes ($FFFFC8 is never changed).
void StreetsOfRage::options_input_controls(m_long /*entry_*/) {
    traceEnter(0x1390u);

    if (SoRControlsMenu::isOpen()) {
        const auto remote = static_cast<SoRInput::ActionMask>(SoRRemoteInput::actions(0) | SoRRemoteInput::actions(1));
        if (SoRControlsMenu::frame(memory(), remote) == SoRControlsMenu::Result::Closed) {
            // Redraw the OPTIONS screen as options_menu_build does, with
            // this row selected.
            cpu().d[0] = 0x100C0B04u;
            SOR_CONTROLS_CALL_68K(load_encoded_vdp_tilemap_bundle(), 0x1190u, 0x00A8B8u);
            cpu().setDw(4, 0x2000u);
            SOR_CONTROLS_CALL_68K(options_draw_sound_name(0x11D6u), 0x119Cu, 0x0011D6u);
            cpu().setDw(4, 0x2000u);
            SOR_CONTROLS_CALL_68K(options_draw_difficulty(), 0x11A4u, 0x0012E4u);
            if (memory().readByte(kCheatFlag) != 0u) {
                SOR_CONTROLS_CALL_68K(options_draw_lives_digit(), 0x11B2u, 0x0013E6u);
                SOR_CONTROLS_CALL_68K(options_draw_level_digit(), 0x11B6u, 0x00145Au);
                cpu().d[0] = 0x00000F0Eu;
                SOR_CONTROLS_CALL_68K(load_encoded_vdp_tilemap_bundle(), 0x11C2u, 0x00A8B8u);
            }
            SoRControlsMenu::drawOptionsRow(memory(), true);
        }
        return68k(cpu());
        return;
    }

    cpu().setDw(0, memory().readWord(kControlScheme));
    cpu().setFlag(CPU68K::FlagZ, cpu().dw(0) == 0);
    SOR_CONTROLS_CALL_68K(options_row_nav(), 0x1398u, 0x0014F2u);
    if (cpu().ne()) {
        SoRControlsMenu::drawOptionsRow(memory(), false);
        return68k(cpu());
        return;
    }

    if ((memory().readByte(kP1ButtonPress) & 0x70u) != 0u) {
        memory().writeByte(kPlaySe, 0xBAu);
        SoRControlsMenu::open();
    }
    return68k(cpu());
}

void StreetsOfRage::char_select_cursor_dispatcher(m_long /*entry_*/) {
    traceEnter(0x18D4u);

    cpu().a[1] = cpu().a[0] == kP1Object ? kP1ButtonHeld : kP2ButtonHeld;
    cpu().setDw(0,
                static_cast<m_word>(memory().readWord(cpu().a[0] + kPlayerInputWord) | memory().readWord(cpu().a[1])));
    memory().writeWord(cpu().a[0] + kPlayerInputWord, cpu().dw(0));

    cpu().a[1] = kCharSelectCursorJt;
    cpu().d[0] = 0;
    cpu().setDb(0, memory().readByte(cpu().a[0] + kObjState));
    cpu().setDw(0, static_cast<m_word>(cpu().dw(0) + cpu().dw(0)));
    cpu().setDw(0, memory().readWord(cpu().a[1] + signExtendWord(cpu().dw(0))));
    cpu().a[1] = cpu().d[0];
    dispatch(cpu().a[1]);
}

void StreetsOfRage::char_select_player_input(m_long /*entry_*/) {
    traceEnter(0x1916u);

    const m_long object = cpu().a[0];
    const m_byte locked = memory().readByte(object + 0x5A);
    cpu().setDb(0, locked);
    setMoveByteFlags(cpu(), locked);
    if (locked != 0) {
        return68k(cpu());
        return;
    }

    const m_byte press = memory().readByte(object + kPlayerPressByte);
    cpu().setDb(6, press);
    setMoveByteFlags(cpu(), press);
    if (press == 0) {
        return68k(cpu());
        return;
    }

    if ((press & 0xF0u) != 0) {
        memory().writeByte(object + 0x5A, 1);
        memory().writeWord(kCharSelectConfirmCount,
                           static_cast<m_word>(memory().readWord(kCharSelectConfirmCount) + 1));
        const m_long characterIdAddress = object == kP1Object ? kP1CharacterId : kP2CharacterId;
        cpu().a[2]                      = characterIdAddress;
        const m_word slot               = memory().readWord(object + 0x58);
        cpu().setDw(0, slot);
        memory().writeByte(characterIdAddress, memory().readByte(kCharacterIdFromSlot + signExtendWord(slot)));
        cpu().d[7] = 0xFFFFFFBAu;
        cpu().setNZClearVC(cpu().d[7], 0x80000000u);
        queue_sound_id();
        return;
    }

    const bool right = (press & 0x08u) != 0;
    cpu().setDb(6, static_cast<m_byte>(press & 0x08u));
    cpu().a[2] = right ? kCharNavRight : kCharNavLeft;

    const m_word oldSlot = memory().readWord(object + 0x58);
    cpu().setDw(0, static_cast<m_word>(oldSlot * 2u));
    m_word newSlot = memory().readWord(cpu().a[2] + signExtendWord(cpu().dw(0)));
    cpu().setDw(1, newSlot);

    if (memory().readByte(kPlayerMode) != 1) {
        const m_long otherObject = object == kP1Object ? kP2Object : kP1Object;
        cpu().a[3]               = otherObject;
        cpu().setDw(2, memory().readWord(otherObject + 0x58));
        cpu().setDw(2, static_cast<m_word>(cpu().dw(2) ^ newSlot));
        if (cpu().dw(2) == 0) {
            cpu().setDw(1, static_cast<m_word>(newSlot * 2u));
            newSlot = memory().readWord(cpu().a[2] + signExtendWord(cpu().dw(1)));
            cpu().setDw(1, newSlot);
        }
    }

    memory().writeWord(object + 0x58, newSlot);

    cpu().setDw(2, static_cast<m_word>(cpu().dw(0) * 2u));
    cpu().a[1] = kCharPreviewPointers;
    cpu().a[1] = memory().readLong(cpu().a[1] + signExtendWord(cpu().dw(2)));
    memory().writeByte(cpu().a[1] + 0x5C, 0);

    cpu().setDw(1, static_cast<m_word>(newSlot * 4u));
    cpu().a[1] = kCharPreviewPointers;
    cpu().a[1] = memory().readLong(cpu().a[1] + signExtendWord(cpu().dw(1)));
    memory().writeByte(cpu().a[1] + 0x5C, 1);

    cpu().a[6] = kCharOldPalette + signExtendWord(cpu().dw(0));
    SOR_CONTROLS_CALL_68K(load_palette_list_to_active(), 0x199Cu, 0x01053Eu);
    cpu().setDw(0, static_cast<m_word>(newSlot * 2u));
    memory().writeWord(object + 0x10, memory().readWord(kCharCursorX + signExtendWord(cpu().dw(0))));
    cpu().a[6] = kCharNewPalette + signExtendWord(cpu().dw(0));
    SOR_CONTROLS_CALL_68K(load_palette_list_to_active(), 0x19B2u, 0x01053Eu);

    cpu().d[7] = 0xFFFFFFB9u;
    cpu().setNZClearVC(cpu().d[7], 0x80000000u);
    queue_sound_id();
}

void StreetsOfRage::remap_player_gameplay_input(m_long /*entry_*/) {
    traceEnter(0x568Au);

    cpu().a[1] = cpu().a[0] == kP1Object ? kP1ButtonHeld : kP2ButtonHeld;

    cpu().setDw(0, static_cast<m_word>(memory().readWord(cpu().a[0] + kPlayerInputWord) & 0xF0F0u));
    cpu().setDw(0, static_cast<m_word>(cpu().dw(0) | memory().readWord(cpu().a[1])));

    // The ROM rotates A/B/C here by the OPTIONS button layout
    // ($FFFFC8 control_scheme). Players now bind actions instead
    // (controls.yaml), so the sampled byte is already in the action layout and
    // is never rotated; the rotation is kept below, disabled, for reference.
    constexpr bool kRotateByControlScheme = false;
    const m_byte   demo                   = memory().readByte(kDemoMode);
    cpu().setFlag(CPU68K::FlagZ, demo == 0);
    if (kRotateByControlScheme && demo == 0) {
        const m_word scheme = memory().readWord(kControlScheme);
        cpu().setDw(7, scheme);
        cpu().setFlag(CPU68K::FlagZ, scheme == 0);
        if (scheme != 0) {
            cpu().setDw(1, cpu().dw(0));
            cpu().setDw(0, static_cast<m_word>(cpu().dw(0) & 0x7070u));
            cpu().setDw(1, static_cast<m_word>(cpu().dw(1) & 0x8F0Fu));

            if (cpu().db(7) == 1u) {
                setShiftLeftWordX(cpu(), cpu().dw(0), 1);
                cpu().setDw(0, static_cast<m_word>(cpu().dw(0) << 1));
                cpu().setDw(2, cpu().dw(0));
                cpu().setDw(0, static_cast<m_word>(cpu().dw(0) & 0x7070u));
                cpu().setDw(2, static_cast<m_word>(cpu().dw(2) & 0x8080u));
                setShiftRightWordX(cpu(), cpu().dw(2), 3);
                cpu().setDw(2, static_cast<m_word>(cpu().dw(2) >> 3));
            } else {
                setShiftRightWordX(cpu(), cpu().dw(0), 1);
                cpu().setDw(0, static_cast<m_word>(cpu().dw(0) >> 1));
                cpu().setDw(2, cpu().dw(0));
                cpu().setDw(0, static_cast<m_word>(cpu().dw(0) & 0x7070u));
                cpu().setDw(2, static_cast<m_word>(cpu().dw(2) & 0x0808u));
                setShiftLeftWordX(cpu(), cpu().dw(2), 3);
                cpu().setDw(2, static_cast<m_word>(cpu().dw(2) << 3));
            }

            cpu().setDw(0, static_cast<m_word>(cpu().dw(0) | cpu().dw(2)));
            cpu().setDw(0, static_cast<m_word>(cpu().dw(0) | cpu().dw(1)));
        }
    }

    memory().writeWord(cpu().a[0] + kPlayerInputWord, cpu().dw(0));
    setMoveWordFlags(cpu(), cpu().dw(0));
    return68k(cpu());
}

// ---------------------------------------------------------------------------
// $0056E6 — high_score_name_entry_dispatcher
//
// When resolve_player_death marks object+$4B bit7 (score qualifies vs the
// current top-10 table), the type-$0F object takes this path instead of the
// continue table at $5236. It is a two-instruction tail into the shared state
// dispatcher with the absolute word table at $56EE (name-entry UI). Completing
// name entry clears bit7 and state so the next frame can run continue Yes/No.
//
// Must not share a body with remap_player_gameplay_input ($568A): the generator
// previously collapsed this nearby root into that manual and broke both the
// name-entry UI and the subsequent continue dialog.
// ---------------------------------------------------------------------------
void StreetsOfRage::high_score_name_entry_dispatcher(m_long /*entry_*/) {
    traceEnter(0x56E6u);
    cpu().a[1] = 0x56EEu;
    // jmp $B186 — state handler returns through the caller's soft stack frame.
    dispatch_object_primary_state_table(0xB186u);
}

void StreetsOfRage::sample_all_joypads(m_long entry_) {
    traceEnter(entry_);

    // Once a frame: the remote client's input document (execution data).
    SoRRemoteInput::update(remoteAccess(), gameUptimeFrames());
    const SampledPlayer player1 = samplePlayer(0);
    const SampledPlayer player2 = samplePlayer(1);
    if (SoRRemoteInput::aiAssist())
        SoRPython::publishFrame(memory(), gameUptimeFrames(), player2.enhanced);

    memory().writeWord(kZ80BusRequest, 0x0100u);
    cpu().a[1] = kIoPlayer1DataPort;
    cpu().a[0] = kP1ButtonHeld;
    cpu().a[2] = kDemoAiInputP1;
    sampleOneJoypadBody(cpu(), memory(), &player1);

    cpu().a[1] = kIoPlayer2DataPort;
    cpu().a[0] += signExtendWord(0x0002u);
    cpu().a[2] = kDemoAiInputP2;
    sampleOneJoypadBody(cpu(), memory(), &player2);

    memory().writeWord(kZ80BusRequest, 0);
    setMoveWordFlags(cpu(), 0);
    return68k(cpu());
}

void StreetsOfRage::sample_one_joypad(m_long entry_) {
    traceEnter(entry_);
    const int playerIndex = altPickupPlayerIndexForBuffer(cpu().a[0]);
    if (playerIndex < 0) {
        sampleOneJoypadBody(cpu(), memory(), nullptr);
    } else {
        const SampledPlayer player = samplePlayer(playerIndex);
        sampleOneJoypadBody(cpu(), memory(), &player);
    }
    return68k(cpu());
}

void StreetsOfRage::clear_player_input(m_long entry_) {
    traceEnter(entry_);
    memory().writeWord(kP1ButtonHeld, 0xFF00u);
    memory().writeWord(kP2ButtonHeld, 0xFF00u);
    SoRCheats::clearAltPickupButtons();
    setMoveWordFlags(cpu(), 0xFF00u);
    return68k(cpu());
}

void StreetsOfRage::handle_pause_start_input(m_long entry_) {
    traceEnter(entry_);

    const auto finishWithPauseFlag = [this] {
        const m_byte pause = memory().readByte(kPauseTextFlag);
        setMoveByteFlags(cpu(), pause);
        return68k(cpu());
    };

    if (memory().readByte(kStopMusic) != 0u) {
        finishWithPauseFlag();
        return;
    }

    cpu().d[7] = memory().readByte(kPauseTextFlag) == 0u ? 3u : 0u;

    bool         pauseRequested = false;
    const m_byte playerMode     = memory().readByte(kPlayerMode);

    if ((playerMode & 0x01u) != 0u) {
        const m_byte buffered =
            static_cast<m_byte>(memory().readByte(kP1StartButtonBuffer) | memory().readByte(kP1ButtonPress));
        memory().writeByte(kP1StartButtonBuffer, buffered);
        if ((buffered & kButtonStart) != 0u)
            pauseRequested = true;
    }

    if (!pauseRequested && (playerMode & 0x02u) != 0u) {
        const m_byte buffered =
            static_cast<m_byte>(memory().readByte(kP2StartButtonBuffer) | memory().readByte(kP2ButtonPress));
        memory().writeByte(kP2StartButtonBuffer, buffered);
        if ((buffered & kButtonStart) != 0u)
            pauseRequested = true;
    }

    if (pauseRequested) {
        if (memory().readByte(kDemoMode) != 0u) {
            const m_byte fade = memory().readByte(kFadeOutFlag);
            if (fade != 0u) {
                setMoveByteFlags(cpu(), fade);
                return68k(cpu());
                return;
            }

            setBit(memory(), kDemoMode, 7);
            memory().writeByte(kFadeOutFlag, 1u);
            memory().writeWord(kPaletteFadeCounter, 0x0040u);
            setMoveWordFlags(cpu(), 0x0040u);
            return68k(cpu());
            return;
        }

        memory().writeByte(kPauseTextFlag, cpu().db(7));
        memory().writeByte(kPlaySe, 0xBDu);
    }

    memory().writeByte(kP1StartButtonBuffer, 0u);
    memory().writeByte(kP2StartButtonBuffer, 0u);
    finishWithPauseFlag();
}
