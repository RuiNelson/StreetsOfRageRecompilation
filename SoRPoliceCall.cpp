#include "SoRCpuOps.hpp"
#include "SoRQolConfig.hpp"

using namespace sorops;

// ---------------------------------------------------------------------------
// The police call under `--qol police`; the ROM's routines otherwise.
//
// The camera goes back $200 instead of $300: the vehicle's spawn test ($18CD4) fires at exactly -$200,
// and at that distance nothing of the street is left on any screen. The vehicle starts $100 nearer, where
// the ROM's camera would have it once stopped (further out the game culls it, and the call never ends).
// The vehicle, the fire or shells and the enemy sweep then run twice an update, so their timers, moves and
// child objects keep their ROM values and stay in step with one another. What is timed from outside is set
// where it starts: the screen shake, and how long a later boss waits for the hit.
//
// The ROM brings the enemies' art and palette back at -$220 on the way home, just as its camera leaves the car
// behind. At -$200 that would be with the car on screen, so here it waits for the game to cull the car, which
// backs out on the way home to be gone before the street comes in.
// ---------------------------------------------------------------------------
namespace {

constexpr m_long kCamX              = 0xFFFFE002u;
constexpr m_long kCameraPrevX       = 0xFFFFE006u;
constexpr m_long kCameraY           = 0xFFFFE00Eu;
constexpr m_long kPrimaryCamera     = 0xFFFFE000u;
constexpr m_long kCameraXMin        = 0xFFFFE01Eu;
constexpr m_long kLevel             = 0xFFFFFF02u;
constexpr m_long kPoliceStartPulse  = 0xFFFFFA1Bu;
constexpr m_long kScreenShakeActive = 0xFFFFFA44u;
constexpr m_long kScreenShakeTimer  = 0xFFFFFA45u;

constexpr m_word kRound7Level = 6u;
constexpr m_long kRomBack     = 0xFD000000u; // -$300, 16.16
constexpr m_long kShortBack   = 0xFE000000u; // -$200
constexpr m_byte kRomShake    = 0x74u;       // p2_police_shell_impact; even, as apply_screen_shake alternates on it

constexpr m_long kPoliceCaller   = 0xFFFFFA1Cu;
constexpr m_long kObjectTable    = 0xFFFFB900u;
constexpr int    kObjectSlots    = 32;
constexpr m_word kVehicleCloser  = 0x0100u;
constexpr m_word kVehicleBackOut = 0x0030u; // px an update, on top of the camera's 16
constexpr m_word kCameraHomeFrom = 8u;      // primary_camera states 8-10, the way home
constexpr m_long kObjectPoolD100 = 0xFFFFD100u;
constexpr int    kObjectPoolSlots = 18;
constexpr m_byte kVehicleType    = 0x05u;
// Updates a later boss in state $0A still has on +$54 ($16B4A) when the fire starts, in the ROM: measured,
// 300 and 390 less the 178 and 220 updates it takes to get there. Half, as the fire runs at twice the speed.
constexpr m_word kBossWaitFromFireP1 = 61u;
constexpr m_word kBossWaitFromFireP2 = 85u;

bool shortCall() {
    return SoRQolConfig::current().shortPoliceCall;
}

// The first object of its kind, on its first update: state 0, not created by another (+$0B).
bool startingRootObject(CPU68K &c, SystemMemory &mem) {
    return mem.readByte(c.a[0] + 0x30u) == 0u && mem.readByte(c.a[0] + 0x0Bu) == 0u;
}

bool vehicleIsAlive(SystemMemory &mem) {
    for (int slot = 0; slot < kObjectPoolSlots; ++slot)
        if (mem.readByte(kObjectPoolD100 + static_cast<m_long>(slot) * 0x80u) == kVehicleType)
            return true;
    return false;
}

void setLaterBossWait(SystemMemory &mem) {
    const m_word wait = mem.readByte(kPoliceCaller) == 0u ? kBossWaitFromFireP1 : kBossWaitFromFireP2;
    for (int slot = 0; slot < kObjectSlots; ++slot) {
        const m_long object = kObjectTable + static_cast<m_long>(slot) * 0x80u;
        const m_byte type   = mem.readByte(object);
        if (type >= 0x55u && type <= 0x58u && mem.readByte(object + 0x30u) == 0x0Au && mem.readWord(object + 0x54u) != 0u)
            mem.writeWord(object + 0x54u, wait);
    }
}

// A police object's update under `--qol police`: the routine that @p target names, again while the slot
// still holds the same object. Like the ROM's `jmp`, the routine's rts ends in our caller only when it is
// last, so each run gets a return address of its own. False when one unwound the stack past us.
template <typename Dispatch, typename Target>
bool runTwice(CPU68K &c, SystemMemory &mem, Dispatch &&dispatch, Target &&target) {
    const m_long object = c.a[0];
    const m_byte type   = mem.readByte(object);
    for (int tick = 0; tick < 2 && mem.readByte(object) == type; ++tick) {
        c.a[0] = object;
        const m_long spBefore = c.ssp;
        c.ssp -= 4;
        mem.writeLong(c.ssp, 0x0000B19Au); // never read back: a generated rts only moves the stack
        dispatch(target());
        if ((c.ssp & 0x00FFFFFFu) > (spBefore & 0x00FFFFFFu))
            return false;
    }
    c.a[0] = object;
    if (mem.readByte(kScreenShakeActive) != 0u && mem.readByte(kScreenShakeTimer) == kRomShake)
        mem.writeByte(kScreenShakeTimer, static_cast<m_byte>(kRomShake / 2u));
    return true;
}

// $00B186 (dispatch_object_primary_state_table) with the table in a1: the address of the object's state.
m_long objectStateTarget(CPU68K &c, SystemMemory &mem, m_long table) {
    c.a[1] = table;
    c.d[0] = static_cast<m_long>(mem.readByte(c.a[0] + 0x30u)) * 2u;
    c.d[0] = mem.readWord(table + c.d[0]);
    c.a[1] = c.d[0];
    return c.d[0];
}

} // namespace

// $0192A4 (begin_police_special_camera_excursion)
void StreetsOfRage::begin_police_special_camera_excursion(m_long /*entry_*/) {
    traceEnter(0x000192A4u);
    auto &c   = cpu();
    auto &mem = memory();

    if ((mem.readByte(kPoliceStartPulse) & 0x01u) != 0u) {
        const m_long camera = c.a[1];
        mem.writeLong(camera + 0x96u, mem.readLong(camera + 0x1Au));
        mem.writeLong(camera + 0x9Au, mem.readLong(camera + 0x1Eu));
        mem.writeLong(camera + 0x9Eu, mem.readLong(camera + 0x22u));
        mem.writeLong(camera + 0xA2u, mem.readLong(camera + 0x26u));
        c.d[0] = mem.readLong(camera + 0x02u);
        mem.writeLong(camera + 0x8Cu, c.d[0]);
        mem.writeLong(camera + 0x90u, mem.readLong(camera + 0x0Eu));
        mem.writeWord(camera + 0x94u, mem.readWord(camera + 0x00u));

        if (mem.readWord(kLevel) == kRound7Level) {
            mem.writeWord(camera + 0x00u, static_cast<m_word>(mem.readWord(camera + 0x00u) + 1u));
            mem.writeLong(camera + 0x22u, 0x05000000u);
            mem.writeLong(camera + 0x1Eu, 0u);
            c.setDw(0, static_cast<m_word>(0x0500u - mem.readWord(camera + 0x90u)));
            mem.writeWord(0xFFFFE022u, c.dw(0));
            mem.writeLong(kCameraXMin, 0u);
            mem.writeWord(kPrimaryCamera, static_cast<m_word>(mem.readWord(kPrimaryCamera) + 4u));
            mem.writeLong(0xFFFFE09Eu, mem.readLong(0xFFFFE022u));
            mem.writeLong(0xFFFFE0A2u, mem.readLong(0xFFFFE026u));
            mem.writeLong(0xFFFFE08Cu, mem.readLong(kCamX));
            mem.writeLong(0xFFFFE090u, mem.readLong(kCameraY));
        } else {
            mem.writeWord(camera + 0x00u, static_cast<m_word>(mem.readWord(camera + 0x00u) + 4u));
            c.d[0] += shortCall() ? kShortBack : kRomBack;
            if (static_cast<std::int32_t>(c.d[0]) < 0)
                c.d[0] = 0u;
            mem.writeLong(camera + 0x1Eu, c.d[0]);
        }
    }
    c.ssp += 4;
}

#define SOR_RUN_POLICE_STATES(table)                                                                    \
    do {                                                                                                \
        auto &c_   = cpu();                                                                             \
        auto &mem_ = memory();                                                                          \
        if (!shortCall()) {                                                                             \
            dispatch(objectStateTarget(c_, mem_, (table)));                                             \
        } else if (runTwice(c_, mem_, [this](m_long a) { dispatch(a); }, [&] { return objectStateTarget(c_, mem_, (table)); })) { \
            c_.ssp += 4;                                                                                \
        }                                                                                               \
    } while (false)

// $018E3C (restore_police_special_palette_and_art)
void StreetsOfRage::restore_police_special_palette_and_art(m_long /*entry_*/) {
    traceEnter(0x00018E3Cu);
    auto &c   = cpu();
    auto &mem = memory();

    constexpr m_long kArtArrayCue      = 0xFFFFDCD0u;
    constexpr m_long kPaletteDirty     = 0xFFFFFA01u;
    constexpr m_long kVehicleSpawned   = 0xFFFFFA1Du;
    constexpr m_long kLevelSpawnFlow   = 0xFFFFFA05u;
    constexpr m_long kPaletteSnapshot  = 0xFFFFDD80u;
    constexpr m_long kPalette          = 0xFFFFF400u;

    const auto restorePalette = [&] {
        for (m_long offset = 0; offset < 0x80u; offset += 4u)
            mem.writeLong(kPalette + offset, mem.readLong(kPaletteSnapshot + offset));
    };

    if (mem.readLong(kArtArrayCue) != 0u) {
        c.ssp += 4;
        return;
    }
    mem.writeByte(kPaletteDirty, 1);
    c.setDw(0, static_cast<m_word>(mem.readWord(c.a[1] + 0x8Cu) - 0x0220u));
    const bool held = shortCall() && mem.readWord(kLevel) != kRound7Level && vehicleIsAlive(mem);
    if (c.dw(0) > mem.readWord(c.a[1] + 0x02u) || held) {
        c.ssp += 4;
        return;
    }
    mem.writeByte(kVehicleSpawned, 0);

    constexpr m_long kArtCueFA63  = 0xFFFFFA63u;
    constexpr m_long kArtCueFA61 = 0xFFFFFA61u;
    constexpr m_long kFlagFA5A  = 0xFFFFFA5Au;
    restorePalette();
    if ((mem.readByte(kLevelSpawnFlow) & 0x40u) != 0u) {
        c.setDw(0, mem.readWord(kLevel));
        if (c.dw(0) == 4u) {
            mem.writeByte(kFlagFA5A, 1);
            c.ssp += 4;
            return;
        }
    } else if (mem.readByte(kArtCueFA63) == 0u) {
        const m_byte level = mem.readByte(kArtCueFA61);
        c.d[0] = level;
        if (level == 0u) {
            c.ssp += 4;
            return;
        }
        c.setDw(0, static_cast<m_word>((level - 1u) * 3u + 0x0Cu));
        queue_nemesis_art_cues();
        return;
    }
    c.d[0] = mem.readByte(kArtCueFA63);
    queue_nemesis_art_cues();
}

// $008454 (queue_nemesis_art_cues): d0's list of cues, appended to the queue of 6-byte entries at $FFDCD0.
void StreetsOfRage::queue_nemesis_art_cues(m_long /*entry_*/) {
    traceEnter(0x00008454u);
    auto &c   = cpu();
    auto &mem = memory();

    constexpr m_long kNemesisArtCueOffsetTable = 0x00008672u;
    constexpr m_long kArtArrayCue              = 0xFFFFDCD0u;
    c.setDw(0, static_cast<m_word>(c.dw(0) * 2u));
    c.setDw(0, mem.readWord(kNemesisArtCueOffsetTable + sext16(c.dw(0))));
    m_long source = kNemesisArtCueOffsetTable + sext16(c.dw(0));
    m_long queue  = kArtArrayCue;
    while (mem.readLong(queue) != 0u)
        queue += 6u;
    c.setDw(0, mem.readWord(source));
    source += 2u;
    if ((c.dw(0) & 0x8000u) == 0u) {
        for (int count = c.dw(0); count >= 0; --count) {
            mem.writeLong(queue, mem.readLong(source));
            mem.writeWord(queue + 4u, mem.readWord(source + 4u));
            queue += 6u;
            source += 6u;
        }
        c.setDw(0, 0xFFFFu);
    }
    c.ssp += 4;
}

// $00597C (police_vehicle_dispatcher)
void StreetsOfRage::police_vehicle_dispatcher(m_long /*entry_*/) {
    traceEnter(0x0000597Cu);
    if (shortCall() && startingRootObject(cpu(), memory()) && memory().readWord(kLevel) != kRound7Level) {
        const m_long x = cpu().a[0] + 0x10u;
        memory().writeWord(x, static_cast<m_word>(memory().readWord(x) + kVehicleCloser));
    }
    if (shortCall() && memory().readWord(kLevel) != kRound7Level && memory().readWord(kPrimaryCamera) >= kCameraHomeFrom) {
        const m_long x = cpu().a[0] + 0x10u;
        memory().writeWord(x, static_cast<m_word>(memory().readWord(x) - kVehicleBackOut));
    }
    SOR_RUN_POLICE_STATES(0x00005984u);
}

// $0063F6 and $00654E: the objects of the fire keep their place on the street as the camera moves.
void StreetsOfRage::p2_police_bombardment_dispatcher(m_long /*entry_*/) {
    traceEnter(0x000063F6u);
    auto &mem = memory();
    const m_word moved = static_cast<m_word>(mem.readWord(kCamX) - mem.readWord(kCameraPrevX));
    mem.writeWord(cpu().a[0] + 0x10u, static_cast<m_word>(mem.readWord(cpu().a[0] + 0x10u) + moved));
    if (shortCall() && startingRootObject(cpu(), mem))
        setLaterBossWait(mem);
    SOR_RUN_POLICE_STATES(0x0000640Au);
}

void StreetsOfRage::p1_police_blast_dispatcher(m_long /*entry_*/) {
    traceEnter(0x0000654Eu);
    auto &mem = memory();
    const m_word moved = static_cast<m_word>(mem.readWord(kCamX) - mem.readWord(kCameraPrevX));
    mem.writeWord(cpu().a[0] + 0x10u, static_cast<m_word>(mem.readWord(cpu().a[0] + 0x10u) + moved));
    if (shortCall() && startingRootObject(cpu(), mem))
        setLaterBossWait(mem);
    SOR_RUN_POLICE_STATES(0x00006562u);
}

// $00B3C0 (police_special_enemy_sweep_trampoline)
void StreetsOfRage::police_special_enemy_sweep_trampoline(m_long /*entry_*/) {
    traceEnter(0x0000B3C0u);
    constexpr m_long kSweepUpdate = 0x000100B6u;
    if (!shortCall())
        dispatch(kSweepUpdate);
    else if (runTwice(cpu(), memory(), [this](m_long a) { dispatch(a); }, [] { return kSweepUpdate; }))
        cpu().ssp += 4;
}
