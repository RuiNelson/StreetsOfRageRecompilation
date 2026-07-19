#include "Sor.hpp"

#include <cstdint>

namespace {

constexpr m_long kVdpControl = 0x00C00004u;

constexpr m_long kGameState  = 0xFFFFFF00u;
constexpr m_long kLevel      = 0xFFFFFF02u;
constexpr m_long kPlayerMode = 0xFFFFFF18u;

constexpr m_long kP1Continues   = 0xFFFFFF1Au;
constexpr m_long kP2Continues   = 0xFFFFFF1Cu;
constexpr m_long kP1CharacterId = 0xFFFFFF1Eu;
constexpr m_long kP2CharacterId = 0xFFFFFF1Fu;
constexpr m_long kP1Lives       = 0xFFFFFF20u;
constexpr m_long kP1Specials    = 0xFFFFFF21u;
constexpr m_long kP1AttackFlag  = 0xFFFFFF22u;
constexpr m_long kP2Lives       = 0xFFFFFF23u;
constexpr m_long kP2Specials    = 0xFFFFFF24u;
constexpr m_long kP2AttackFlag  = 0xFFFFFF25u;

constexpr m_long kSoundTestIndex = 0xFFFFFFC4u;
constexpr m_long kDifficulty     = 0xFFFFFFC6u;
constexpr m_long kControlScheme  = 0xFFFFFFC8u;
constexpr m_long kLivesSetting   = 0xFFFFFFCAu;

constexpr m_long kP1Object    = 0xFFFFB800u;
constexpr m_long kP2Object    = 0xFFFFB880u;
constexpr m_long kObjectTable = 0xFFFFB900u;

constexpr m_long kCheatFlag             = 0xFFFFFA7Bu;
constexpr m_long kSelectMenuOptionCount = 0xFFFFFA09u;
constexpr m_long kPaletteFadeCounter    = 0xFFFFFB0Cu;
constexpr m_long kSelectScreenSubstate  = 0xFFFFFB0Eu;
constexpr m_long kSelectMenuCursor      = 0xFFFFB840u;

constexpr m_long kCharSelectIdleTimer    = 0xFFFFF900u;
constexpr m_long kCharSelectSubstate     = 0xFFFFF904u;
constexpr m_long kCharSelectConfirmCount = 0xFFFFF908u;
constexpr m_long kCharSelectExitDelay    = 0xFFFFF90Au;

constexpr m_long kP1ButtonHeld  = 0xFFFFFC04u;
constexpr m_long kP1ButtonPress = 0xFFFFFC05u;
constexpr m_long kP2ButtonHeld  = 0xFFFFFC08u;
constexpr m_long kP2ButtonPress = 0xFFFFFC09u;
constexpr m_long kP2PadMissing  = 0xFFFFFC0Au;

constexpr m_long kSelectScreenJumpTable = 0x000010C4u;
constexpr m_long kPlayerStateJumpTable  = 0x00001578u;
constexpr m_long kCharSelectJumpTable   = 0x0000171Au;
constexpr m_long kObjectTypeJumpTable   = 0x0000B236u;

constexpr m_long kOptionsSoundIds      = 0x0000128Au;
constexpr m_long kCharNavRight         = 0x000019E4u;
constexpr m_long kCharNavLeft          = 0x000019EAu;
constexpr m_long kCharOldPalette       = 0x000019F0u;
constexpr m_long kCharNewPalette       = 0x000019F6u;
constexpr m_long kCharCursorX          = 0x000019FCu;
constexpr m_long kCharPortraitPointers = 0x00001A02u;
constexpr m_long kCharacterIdFromSlot  = 0x00001A0Eu;

constexpr m_long signExtendWord(m_word value) {
    return static_cast<m_long>(static_cast<std::int32_t>(static_cast<std::int16_t>(value)));
}

constexpr m_word wrappedStep(m_word value, m_word maximum, bool increment) {
    if (increment)
        return value >= maximum ? 0 : static_cast<m_word>(value + 1);
    return value == 0 ? maximum : static_cast<m_word>(value - 1);
}

} // namespace

// A hand-written equivalent of generated/Sor.cpp's CALL/CALL_DISPATCH.  The
// expression is a 68000 subroutine call: push the synthetic return PC, invoke
// it, and stop this body if the callee unwound beyond our frame.
#define SOR_CALL_68K(expression, returnPc)                                                                             \
    do {                                                                                                               \
        const m_long sorCallSp = cpu().ssp;                                                                            \
        cpu().ssp -= 4;                                                                                                \
        memory().writeLong(cpu().ssp, static_cast<m_long>(returnPc));                                                  \
        expression;                                                                                                    \
        if ((cpu().ssp & 0x00FFFFFFu) > (sorCallSp & 0x00FFFFFFu))                                                     \
            return;                                                                                                    \
    } while (false)

// ---------------------------------------------------------------------------
// Mode-select / OPTIONS ($0FE8-$1564)
// ---------------------------------------------------------------------------

void Sor::init_game_start_screen(m_long entry_) {
    if (entry_ == 0x1050u || entry_ == 0x1058u) {
        traceEnter(entry_);
        memory().writeByte(kSelectMenuOptionCount, entry_ == 0x1050u ? 3 : 1);
        cpu().ssp += 4;
        return;
    }

    traceEnter(0x0FE8u);

    memory().writeByte(kCheatFlag, 0);
    memory().writeWord(0xFFFFFF06u, 1);
    memory().writeWord(0xFFFFE000u, 3);
    cpu().d[0] = 2;
    SOR_CALL_68K(sub_00a63a(), 0x1000u);

    memory().writeWord(kSelectScreenSubstate, 0);
    cpu().d[1] = 0;
    cpu().a[1] = kP1Object;
    SOR_CALL_68K(memfill_long_128(), 0x1010u);

    cpu().d[4] = 2;
    cpu().d[5] = 0;
    cpu().a[6] = 0x00001060u;
    cpu().a[5] = 0x0000108Au;
    for (int row = 0; row < 3; ++row) {
        cpu().d[6] = 9;
        cpu().d[7] = memory().readLong(cpu().a[6]);
        cpu().a[6] += 4;
        SOR_CALL_68K(vdp_write_menu_string(), 0x1026u);
        cpu().setDw(5, memory().readWord(cpu().a[5]));
        cpu().a[5] += 2;
        cpu().setDw(4, static_cast<m_word>(cpu().dw(4) - 1));
    }

    memory().writeWord(kPaletteFadeCounter, 0x40);
    cpu().a[6] = memory().readByte(kP2PadMissing) == 0 ? 0x0007267Cu : 0x00072684u;
    SOR_CALL_68K(sub_010538(), 0x104Au);

    memory().writeByte(kSelectMenuOptionCount, memory().readByte(kP2PadMissing) == 0 ? 3 : 1);
    cpu().ssp += 4;
}

void Sor::game_start_screen_update(m_long /*entry_*/) {
    traceEnter(0x108Eu);

    const m_word substate = memory().readWord(kSelectScreenSubstate);
    if (substate >= 0x10 && substate < 0x28 && (memory().readByte(kP1ButtonPress) & 0x80u) != 0) {
        cpu().d[7] = 0xFFFFFFE1u;
        SOR_CALL_68K(queue_sound_id(), 0x10AEu);
        memory().writeWord(kSelectScreenSubstate, 0x28);
    }

    cpu().a[1] = kSelectScreenJumpTable;
    cpu().setDw(0, memory().readWord(kSelectScreenSubstate));
    const m_word relative = memory().readWord(cpu().a[1] + signExtendWord(cpu().dw(0)));
    cpu().setDw(0, relative);
    cpu().setNZClearVC(relative, 0x8000u);
    dispatch(cpu().a[1] + signExtendWord(relative));
}

void Sor::select_menu_wait_fade_in(m_long /*entry_*/) {
    traceEnter(0x10F2u);
    SOR_CALL_68K(player_state_dispatcher(), 0x10F6u);
    select_menu_wait_fade(0x10F6u);
}

void Sor::select_menu_input(m_long /*entry_*/) {
    traceEnter(0x1104u);

    const m_byte confirm = static_cast<m_byte>(memory().readByte(kP1ButtonPress) & 0xF0u);
    cpu().setDb(0, confirm);
    cpu().setNZClearVC(confirm, 0x80u);
    if (confirm == 0) {
        SOR_CALL_68K(select_menu_sync_pad_count(), 0x1112u);
        player_state_dispatcher();
        return;
    }

    cpu().d[7] = 0xFFFFFFBAu;
    SOR_CALL_68K(queue_sound_id(), 0x111Eu);
    memory().writeWord(kSelectScreenSubstate, static_cast<m_word>(memory().readWord(kSelectScreenSubstate) + 2));
    cpu().ssp += 4;
}

void Sor::select_menu_resolve_choice(m_long /*entry_*/) {
    traceEnter(0x114Au);

    const m_word cursor = memory().readWord(kSelectMenuCursor);
    if (cursor < 2) {
        cpu().setDw(0, cursor);
        memory().writeByte(kPlayerMode, memory().readByte(0x0000117Au + signExtendWord(cursor)));
        memory().writeWord(kGameState, 0x20);
        cpu().setNZClearVC(0x20u, 0x8000u);
        cpu().ssp += 4;
        return;
    }

    const m_byte cheatChord = static_cast<m_byte>(memory().readByte(kP2ButtonHeld) ^ 0x78u);
    cpu().setDb(1, cheatChord);
    cpu().setFlag(CPU68K::FlagZ, cheatChord == 0);
    if (cheatChord == 0)
        memory().writeByte(kCheatFlag, 1);

    memory().writeWord(kSelectScreenSubstate, static_cast<m_word>(memory().readWord(kSelectScreenSubstate) + 2));
    cpu().ssp += 4;
}

void Sor::options_menu_build(m_long /*entry_*/) {
    traceEnter(0x117Cu);

    SOR_CALL_68K(sub_007f00(), 0x1180u);
    memory().writeLong(0xFFFFDA00u, 0);

    cpu().d[0] = 0x0D0C0504u;
    SOR_CALL_68K(sub_00a8b8(), 0x1190u);
    cpu().d[0] = 0x10u;
    SOR_CALL_68K(sub_00a8b8(), 0x1198u);
    SOR_CALL_68K(options_draw_sound_name(), 0x119Cu);

    cpu().setDw(4, 0x2000u);
    SOR_CALL_68K(options_draw_difficulty(), 0x11A4u);
    SOR_CALL_68K(options_draw_controls(), 0x11A8u);

    if (memory().readByte(kCheatFlag) != 0) {
        SOR_CALL_68K(options_draw_lives_digit(), 0x11B2u);
        SOR_CALL_68K(options_draw_level_digit(), 0x11B6u);
        cpu().d[0] = 0x00000F0Eu;
        SOR_CALL_68K(sub_00a8b8(), 0x11C2u);
    }

    memory().writeWord(kSelectScreenSubstate, static_cast<m_word>(memory().readWord(kSelectScreenSubstate) + 2));
    cpu().ssp += 4;
}

void Sor::options_input_sound_test(m_long /*entry_*/) {
    traceEnter(0x1218u);

    cpu().setDw(0, memory().readWord(kSoundTestIndex));
    cpu().setFlag(CPU68K::FlagZ, cpu().dw(0) == 0);
    SOR_CALL_68K(options_row_nav(), 0x1220u);
    if (cpu().ne()) {
        cpu().d[7] = 0xFFFFFFE1u;
        SOR_CALL_68K(queue_sound_id(), 0x1262u);
        cpu().d[0] = 0x0Bu;
        SOR_CALL_68K(sub_00a8b8(), 0x126Au);
        cpu().setDw(4, 0x2000u);
        cpu().setNZClearVC(0x2000u, 0x8000u);
        options_draw_sound_name(0x11D6u);
        return;
    }

    const m_byte press = memory().readByte(kP1ButtonPress);
    cpu().setDb(1, static_cast<m_byte>(press & 0x0Cu));
    cpu().setDb(2, press);
    if ((press & 0x0Cu) != 0) {
        cpu().d[7] = 0xFFFFFFE1u;
        SOR_CALL_68K(queue_sound_id(), 0x123Au);
        const bool   right = (press & 0x08u) != 0;
        const m_word value = wrappedStep(cpu().dw(0), 0x48, right);
        cpu().setDw(0, value);
        memory().writeWord(kSoundTestIndex, value);
        cpu().setNZClearVC(value, 0x8000u);
        options_draw_sound_name();
        return;
    }

    const m_byte face = static_cast<m_byte>(press & 0xF0u);
    cpu().setDb(2, face);
    cpu().setNZClearVC(face, 0x80u);
    if (face != 0) {
        cpu().d[7] = 0;
        cpu().setDw(7, memory().readWord(kSoundTestIndex));
        const m_byte soundId = memory().readByte(kOptionsSoundIds + signExtendWord(cpu().dw(7)));
        cpu().setDb(7, soundId);
        cpu().setNZClearVC(soundId, 0x80u);
        queue_sound_id();
        return;
    }

    cpu().ssp += 4;
}

void Sor::options_input_difficulty(m_long /*entry_*/) {
    traceEnter(0x131Au);

    cpu().setDw(0, memory().readWord(kDifficulty));
    cpu().setFlag(CPU68K::FlagZ, cpu().dw(0) == 0);
    SOR_CALL_68K(options_row_nav(), 0x1322u);
    if (cpu().ne()) {
        cpu().d[0] = 0x0Cu;
        SOR_CALL_68K(sub_00a8b8(), 0x1358u);
        cpu().setDw(4, 0x2000u);
        cpu().setNZClearVC(0x2000u, 0x8000u);
        options_draw_difficulty();
        return;
    }

    const m_byte horizontal = static_cast<m_byte>(memory().readByte(kP1ButtonPress) & 0x0Cu);
    cpu().setDb(1, horizontal);
    cpu().setNZClearVC(horizontal, 0x80u);
    if (horizontal == 0) {
        cpu().ssp += 4;
        return;
    }

    const m_word value = wrappedStep(cpu().dw(0), 3, (horizontal & 0x08u) != 0);
    cpu().setDw(0, value);
    memory().writeWord(kDifficulty, value);
    cpu().setNZClearVC(value, 0x8000u);
    options_highlight_difficulty(0x12E0u);
}

void Sor::options_input_controls(m_long /*entry_*/) {
    traceEnter(0x1390u);

    cpu().setDw(0, memory().readWord(kControlScheme));
    cpu().setFlag(CPU68K::FlagZ, cpu().dw(0) == 0);
    SOR_CALL_68K(options_row_nav(), 0x1398u);
    if (cpu().ne()) {
        cpu().d[0] = 0x0Du;
        SOR_CALL_68K(sub_00a8b8(), 0x13CEu);
        cpu().setDw(4, 0x2000u);
        cpu().setNZClearVC(0x2000u, 0x8000u);
        options_draw_controls();
        return;
    }

    const m_byte horizontal = static_cast<m_byte>(memory().readByte(kP1ButtonPress) & 0x0Cu);
    cpu().setDb(1, horizontal);
    cpu().setNZClearVC(horizontal, 0x80u);
    if (horizontal == 0) {
        cpu().ssp += 4;
        return;
    }

    const m_word value = wrappedStep(cpu().dw(0), 2, (horizontal & 0x08u) != 0);
    cpu().setDw(0, value);
    memory().writeWord(kControlScheme, value);
    cpu().setNZClearVC(value, 0x8000u);
    options_highlight_controls(0x136Cu);
}

void Sor::options_input_lives(m_long /*entry_*/) {
    traceEnter(0x1404u);

    cpu().setDw(0, memory().readWord(kLivesSetting));
    cpu().setFlag(CPU68K::FlagZ, cpu().dw(0) == 0);
    SOR_CALL_68K(options_row_nav(), 0x140Cu);
    if (cpu().ne()) {
        cpu().d[0] = 0x0Eu;
        SOR_CALL_68K(sub_00a8b8(), 0x1442u);
        cpu().setDw(4, 0x2000u);
        options_draw_lives_digit();
        return;
    }

    const m_byte horizontal = static_cast<m_byte>(memory().readByte(kP1ButtonPress) & 0x0Cu);
    cpu().setDb(1, horizontal);
    cpu().setNZClearVC(horizontal, 0x80u);
    if (horizontal == 0) {
        cpu().ssp += 4;
        return;
    }

    const m_word value = wrappedStep(cpu().dw(0), 3, (horizontal & 0x08u) != 0);
    cpu().setDw(0, value);
    memory().writeWord(kLivesSetting, value);
    options_draw_lives_digit();
}

void Sor::options_input_level(m_long /*entry_*/) {
    traceEnter(0x1476u);

    cpu().setDw(0, memory().readWord(kLevel));
    cpu().setFlag(CPU68K::FlagZ, cpu().dw(0) == 0);
    SOR_CALL_68K(options_row_nav(), 0x147Eu);
    if (cpu().ne()) {
        cpu().d[0] = 0x0Fu;
        SOR_CALL_68K(sub_00a8b8(), 0x14B4u);
        cpu().setDw(4, 0x2000u);
        options_draw_level_digit();
        return;
    }

    const m_byte horizontal = static_cast<m_byte>(memory().readByte(kP1ButtonPress) & 0x0Cu);
    cpu().setDb(1, horizontal);
    cpu().setNZClearVC(horizontal, 0x80u);
    if (horizontal == 0) {
        cpu().ssp += 4;
        return;
    }

    const m_word value = wrappedStep(cpu().dw(0), 7, (horizontal & 0x08u) != 0);
    cpu().setDw(0, value);
    memory().writeWord(kLevel, value);
    options_draw_level_digit();
}

void Sor::options_input_exit(m_long /*entry_*/) {
    traceEnter(0x14CAu);

    SOR_CALL_68K(options_row_nav(), 0x14CEu);
    if (cpu().ne()) {
        cpu().d[0] = 0x10u;
        cpu().setNZClearVC(0x10u, 0x80000000u);
        SOR_CALL_68K(sub_00a8b8(), 0x14E8u);
        cpu().ssp += 4;
        return;
    }

    const m_byte confirm = static_cast<m_byte>(memory().readByte(kP1ButtonPress) & 0xF0u);
    cpu().setDb(1, confirm);
    cpu().setNZClearVC(confirm, 0x80u);
    if (confirm != 0)
        memory().writeWord(kSelectScreenSubstate, static_cast<m_word>(memory().readWord(kSelectScreenSubstate) + 2));
    cpu().ssp += 4;
}

void Sor::options_menu_return(m_long /*entry_*/) {
    traceEnter(0x14EAu);
    memory().writeWord(kGameState, 0x10);
    cpu().setNZClearVC(0x10u, 0x8000u);
    cpu().ssp += 4;
}

void Sor::options_row_nav(m_long /*entry_*/) {
    traceEnter(0x14F2u);

    const m_byte vertical = static_cast<m_byte>(memory().readByte(kP1ButtonPress) & 0x03u);
    cpu().setDb(1, vertical);
    cpu().setNZClearVC(vertical, 0x80u);
    if (vertical == 0) {
        cpu().ssp += 4;
        return;
    }

    m_word     next = memory().readWord(kSelectScreenSubstate);
    const bool up   = (vertical & 0x01u) != 0;
    cpu().setDb(1, static_cast<m_byte>(vertical & 1u));
    cpu().setFlag(CPU68K::FlagZ, !up);

    if (up) {
        next = static_cast<m_word>(next - 6);
        if (memory().readByte(kCheatFlag) == 0 && static_cast<m_byte>(next) == 0x20)
            next = 0x18;
        if (static_cast<m_byte>(next) < 0x10)
            next = 0x24;
    } else {
        next = static_cast<m_word>(next + 2);
        if (memory().readByte(kCheatFlag) == 0 && static_cast<m_byte>(next) == 0x1C)
            next = 0x24;
        if (static_cast<m_byte>(next) >= 0x26)
            next = 0x10;
    }

    cpu().setDw(2, next);
    memory().writeWord(kSelectScreenSubstate, next);
    cpu().d[7] = 0xFFFFFFB9u;
    cpu().setNZClearVC(0xFFFFFFB9u, 0x80000000u);
    queue_sound_id();
}

void Sor::select_menu_sync_pad_count(m_long /*entry_*/) {
    traceEnter(0x1546u);

    const bool p2Missing = memory().readByte(kP2PadMissing) != 0;
    if (memory().readByte(kSelectMenuOptionCount) == 1) {
        if (!p2Missing) {
            init_game_start_screen(0x1050u);
            return;
        }
    } else if (p2Missing) {
        init_game_start_screen(0x1058u);
        return;
    }

    cpu().setNZClearVC(p2Missing ? 1u : 0u, 0x80u);
    cpu().ssp += 4;
}

void Sor::player_state_dispatcher(m_long /*entry_*/) {
    traceEnter(0x1564u);
    cpu().a[0] = kP1Object;
    cpu().a[1] = kPlayerStateJumpTable;
    cpu().setDw(0, memory().readWord(cpu().a[0] + 0x30));
    const m_word relative = memory().readWord(cpu().a[1] + signExtendWord(cpu().dw(0)));
    cpu().setDw(0, relative);
    cpu().setNZClearVC(relative, 0x8000u);
    dispatch(cpu().a[1] + signExtendWord(relative));
}

// ---------------------------------------------------------------------------
// Character select ($1634-$1916)
// ---------------------------------------------------------------------------

void Sor::init_character_select_screen(m_long /*entry_*/) {
    traceEnter(0x1634u);

    cpu().a[1] = 0x00FF0000u;
    cpu().setDw(7, 0x01F9u);
    cpu().d[1] = 0;
    while (cpu().dw(7) != 0xFFFFu) {
        SOR_CALL_68K(memfill_long_128(), 0x1646u);
        cpu().setDw(7, static_cast<m_word>(cpu().dw(7) - 1));
    }

    memory().writeWord(kCharSelectIdleTimer, 0x012C);
    memory().writeWord(0xFFFFFF06u, 1);
    memory().writeWord(kPaletteFadeCounter, 0x40);
    cpu().a[6] = 0x00071F30u;
    SOR_CALL_68K(sub_010538(), 0x1668u);

    memory().writeByte(kP1Object, 6);
    memory().writeWord(kP1Object + 0x58, 0);
    memory().writeWord(kP1Object + 0x10, 0x20);
    cpu().a[6] = 0x00001708u;
    if (memory().readByte(kPlayerMode) == 3) {
        memory().writeByte(kP2Object, 6);
        memory().writeWord(kP2Object + 0x58, 2);
        memory().writeWord(kP2Object + 0x10, 0xE0);
        cpu().a[6] = 0x00001706u;
    }
    SOR_CALL_68K(sub_010538(), 0x16A2u);

    memory().writeByte(kObjectTable, 7);
    memory().writeByte(kObjectTable + 0x50, 1);
    memory().writeByte(kObjectTable + 0x80, 7);
    memory().writeByte(kObjectTable + 0xD0, 0);
    memory().writeByte(kObjectTable + 0x100, 7);
    memory().writeByte(kObjectTable + 0x150, 2);

    cpu().d[0] = 0x00352003u;
    SOR_CALL_68K(sub_00a63a(), 0x16D2u);
    cpu().a[0] = 0x00071C6Cu;
    cpu().a[1] = 0x00FF8000u;
    SOR_CALL_68K(kosinskidec(), 0x16E4u);
    cpu().d[0] = 4;
    SOR_CALL_68K(sub_00a5f4(), 0x16ECu);
    cpu().d[0] = 0x14131211u;
    SOR_CALL_68K(sub_00a8b8(), 0x16F8u);
    cpu().d[0] = 0x00001615u;
    cpu().setNZClearVC(cpu().d[0], 0x80000000u);
    SOR_CALL_68K(sub_00a8b8(), 0x1704u);
    cpu().ssp += 4;
}

void Sor::screen_state_dispatcher(m_long /*entry_*/) {
    traceEnter(0x170Au);
    cpu().a[1] = kCharSelectJumpTable;
    cpu().setDw(0, memory().readWord(kCharSelectSubstate));
    const m_word relative = memory().readWord(cpu().a[1] + signExtendWord(cpu().dw(0)));
    cpu().setDw(0, relative);
    cpu().setNZClearVC(relative, 0x8000u);
    dispatch(cpu().a[1] + signExtendWord(relative));
}

void Sor::char_select_play_music(m_long /*entry_*/) {
    traceEnter(0x1726u);
    cpu().d[7] = 0xFFFFFF8Au;
    SOR_CALL_68K(queue_sound_id(), 0x172Eu);
    memory().writeWord(kCharSelectSubstate, static_cast<m_word>(memory().readWord(kCharSelectSubstate) + 2));
    update_select_objects();
}

void Sor::char_select_interactive(m_long /*entry_*/) {
    traceEnter(0x175Au);
    SOR_CALL_68K(char_select_idle_tick(), 0x175Eu);
    SOR_CALL_68K(update_select_objects(), 0x1764u);

    const m_word required = memory().readByte(kPlayerMode) == 1 ? 1 : 2;
    cpu().d[0]            = required;
    if (memory().readWord(kCharSelectConfirmCount) == required) {
        memory().writeWord(kCharSelectSubstate, static_cast<m_word>(memory().readWord(kCharSelectSubstate) + 2));
        memory().writeWord(kPaletteFadeCounter, 0x40);
        memory().writeWord(kCharSelectExitDelay, 0x12);
        cpu().setNZClearVC(0x12u, 0x8000u);
    }
    cpu().ssp += 4;
}

void Sor::initialize_player_continues(m_long /*entry_*/) {
    traceEnter(0x17A2u);

    cpu().d[0] = 3;
    memory().writeWord(kP1Continues, 3);
    memory().writeWord(kP2Continues, 3);

    const m_word lives = static_cast<m_word>(memory().readWord(kLivesSetting) * 2u + 1u);
    cpu().setDw(0, lives);
    memory().writeByte(kP1Lives, static_cast<m_byte>(lives));
    memory().writeByte(kP2Lives, static_cast<m_byte>(lives));
    memory().writeByte(kP1AttackFlag, 0);
    memory().writeByte(kP2AttackFlag, 0);
    memory().writeByte(kP1Specials, 0);
    memory().writeByte(kP2Specials, 0);
    memory().writeWord(kGameState, 0x28);

    cpu().d[7] = 0xFFFFFFE1u;
    cpu().setNZClearVC(cpu().d[7], 0x80000000u);
    queue_sound_id();
}

void Sor::char_select_player_input(m_long /*entry_*/) {
    traceEnter(0x1916u);

    const m_long object = cpu().a[0];
    const m_byte locked = memory().readByte(object + 0x5A);
    cpu().setDb(0, locked);
    cpu().setNZClearVC(locked, 0x80u);
    if (locked != 0) {
        cpu().ssp += 4;
        return;
    }

    const m_byte press = memory().readByte(object + 0x55);
    cpu().setDb(6, press);
    cpu().setNZClearVC(press, 0x80u);
    if (press == 0) {
        cpu().ssp += 4;
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
    cpu().a[1] = kCharPortraitPointers;
    cpu().a[1] = memory().readLong(cpu().a[1] + signExtendWord(cpu().dw(2)));
    memory().writeByte(cpu().a[1] + 0x5C, 0);

    cpu().setDw(1, static_cast<m_word>(newSlot * 4u));
    cpu().a[1] = kCharPortraitPointers;
    cpu().a[1] = memory().readLong(cpu().a[1] + signExtendWord(cpu().dw(1)));
    memory().writeByte(cpu().a[1] + 0x5C, 1);

    cpu().a[6] = kCharOldPalette + signExtendWord(cpu().dw(0));
    SOR_CALL_68K(sub_01053e(), 0x199Cu);
    cpu().setDw(0, static_cast<m_word>(newSlot * 2u));
    memory().writeWord(object + 0x10, memory().readWord(kCharCursorX + signExtendWord(cpu().dw(0))));
    cpu().a[6] = kCharNewPalette + signExtendWord(cpu().dw(0));
    SOR_CALL_68K(sub_01053e(), 0x19B2u);

    cpu().d[7] = 0xFFFFFFB9u;
    cpu().setNZClearVC(cpu().d[7], 0x80000000u);
    queue_sound_id();
}

// ---------------------------------------------------------------------------
// Mode wrappers ($9170-$92A8) and shared select-screen object pass ($AD8E)
// ---------------------------------------------------------------------------

void Sor::init_selectscreenmode(m_long /*entry_*/) {
    traceEnter(0x9170u);
    memory().writeWord(kVdpControl, memory().readWord(0xFFFFFF48u));
    cpu().setStatus(0x2500u);
    SOR_CALL_68K(init_z80(), 0x9180u);
    SOR_CALL_68K(init_game_start_screen(), 0x9186u);
    SOR_CALL_68K(clear_player_input(), 0x918Au);
    memory().writeWord(kVdpControl, memory().readWord(0xFFFFFF46u));
    cpu().setStatus(0x2700u);
    memory().writeWord(kGameState, static_cast<m_word>(memory().readWord(kGameState) + 2));
    game_mode_selectscreenmode();
}

void Sor::game_mode_selectscreenmode(m_long /*entry_*/) {
    traceEnter(0x919Au);
    game_start_screen_update();
}

void Sor::init_characterselectscreen(m_long /*entry_*/) {
    traceEnter(0x927Cu);
    memory().writeWord(kVdpControl, memory().readWord(0xFFFFFF48u));
    cpu().setStatus(0x2500u);
    SOR_CALL_68K(init_z80(), 0x928Cu);
    SOR_CALL_68K(init_character_select_screen(), 0x9290u);
    SOR_CALL_68K(load_z80_dac_driver(), 0x9294u);
    SOR_CALL_68K(clear_player_input(), 0x9298u);
    memory().writeWord(kVdpControl, memory().readWord(0xFFFFFF46u));
    cpu().setStatus(0x2700u);
    memory().writeWord(kGameState, static_cast<m_word>(memory().readWord(kGameState) + 2));
    game_mode_characterselectscreen();
}

void Sor::game_mode_characterselectscreen(m_long /*entry_*/) {
    traceEnter(0x92A8u);
    screen_state_dispatcher();
}

void Sor::update_select_objects(m_long entry_) {
    traceEnter(entry_ == 0xAE10u ? 0xAE10u : 0xAD8Eu);

    if (entry_ != 0xAE10u) {
        cpu().a[0] = kP1Object;
        memory().writeByte(0xFFFFFA75u, 0);
        memory().writeWord(0xFFFFFB56u, 0xFB72u);
        cpu().d[7] = 1;

        for (int player = 0; player < 2; ++player) {
            const m_byte type = memory().readByte(cpu().a[0]);
            cpu().d[0]        = type;
            cpu().setFlag(CPU68K::FlagZ, type == 0);
            if (type != 0) {
                cpu().ssp -= 2;
                memory().writeWord(cpu().ssp, cpu().dw(7));
                cpu().setDw(0, static_cast<m_word>(type * 2u));
                cpu().a[1] = kObjectTypeJumpTable;
                cpu().setDw(0, memory().readWord(cpu().a[1] + signExtendWord(cpu().dw(0))));
                cpu().a[1] = cpu().d[0];
                SOR_CALL_68K(dispatch(cpu().a[1]), 0xADB4u);
                SOR_CALL_68K(sub_00b132(), 0xADB8u);
                cpu().setDw(7, memory().readWord(cpu().ssp));
                cpu().ssp += 2;
            }
            cpu().a[0] += 0x80;
            cpu().setDw(7, static_cast<m_word>(cpu().dw(7) - 1));
        }

        const m_word state = memory().readWord(kGameState);
        if (state == 0x14 || state == 0x16) {
            SOR_CALL_68K(sub_004478(), 0xADD6u);
            SOR_CALL_68K(sub_018af8(), 0xADDCu);
        }
        SOR_CALL_68K(sub_0051cc(), 0xADE0u);
        SOR_CALL_68K(sub_0043aa(), 0xADE4u);
        SOR_CALL_68K(sync_z80_2(), 0xADE8u);

        memory().writeWord(kP1Object + 0x54, memory().readWord(kP1ButtonHeld));
        memory().writeWord(kP2Object + 0x54, memory().readWord(kP2ButtonHeld));
        memory().writeByte(0xFFFFFA47u, memory().readByte(kP1ButtonPress));
        memory().writeByte(0xFFFFFA48u, memory().readByte(kP2ButtonPress));

        cpu().a[0] = kP1Object;
        SOR_CALL_68K(sub_00ae4c(), 0xAE06u);
        cpu().a[0] = kP2Object;
        SOR_CALL_68K(sub_00ae4c(), 0xAE0Cu);
        cpu().a[0] = kObjectTable;
    }

    cpu().d[7] = 0x41u;
    for (int objectIndex = 0; objectIndex < 0x42; ++objectIndex) {
        const m_byte type = memory().readByte(cpu().a[0]);
        cpu().d[0]        = type;
        cpu().setNZClearVC(type, 0x80u);
        if (type != 0) {
            cpu().ssp -= 2;
            memory().writeWord(cpu().ssp, cpu().dw(7));

            const m_word tableOffset = static_cast<m_word>(type * 2u);
            cpu().d[0]               = tableOffset >= 0x120u ? 0x00010000u : 0;
            cpu().setDw(0, tableOffset);
            cpu().a[1] = kObjectTypeJumpTable;
            cpu().setDw(0, memory().readWord(cpu().a[1] + signExtendWord(tableOffset)));
            cpu().a[1] = cpu().d[0];
            SOR_CALL_68K(dispatch(cpu().a[1]), 0xAE32u);
            SOR_CALL_68K(sub_00b132(), 0xAE36u);
            SOR_CALL_68K(sub_00ae4c(), 0xAE38u);

            cpu().setDw(7, memory().readWord(cpu().ssp));
            cpu().ssp += 2;
            cpu().setFlag(CPU68K::FlagN, (cpu().dw(7) & 0x8000u) != 0);
            cpu().setFlag(CPU68K::FlagV, false);
            cpu().setFlag(CPU68K::FlagC, false);
        }
        cpu().a[0] += 0x80;
        cpu().setDw(7, static_cast<m_word>(cpu().dw(7) - 1));
    }

    const m_byte flags = memory().readByte(0xFFFFFA1Bu);
    cpu().setFlag(CPU68K::FlagZ, (flags & 1u) == 0);
    memory().writeByte(0xFFFFFA1Bu, static_cast<m_byte>(flags & ~1u));
    sub_00ae4c(0xAE96u);
}

#undef SOR_CALL_68K
