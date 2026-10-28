#include "SoRSoundtrack.hpp"

#include "Logger.hpp"
#include "system/memory/SystemMemory.hpp"

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace SoRSoundtrack {
namespace {

// Sound workspace and ROM tables (code-analysis/addresses.csv).
constexpr m_long kSoundActivePriority = 0x00FFF000u;
constexpr m_long kStopMusic           = 0x00FFF002u;
constexpr m_long kEngineGate          = 0x00FFF007u;
constexpr m_long kPendingId           = 0x00FFF009u;
constexpr m_long kPlaySe              = 0x00FFF00Au;
constexpr m_long kSfxFm0              = 0x00FFF1F0u;
constexpr m_long kSfxFm1              = 0x00FFF220u;
constexpr m_long kPriorityTable       = 0x00072824u; // sound_priority_table, index ID - $81
constexpr m_long kSfxPointerTable     = 0x00073B56u; // sfx_pointer_table, index ID - $A0
constexpr m_long kSfxChannelTable     = 0x00072E74u; // SFX channel block per header byte 3

constexpr m_long kProcessQueueEntry = 0x00072D08u;
constexpr m_long kResetAllEntry     = 0x00072F8Au;
constexpr m_long kFadeMusicEntry    = 0x00073066u;

// One fade step raises every music FM channel's attenuation by one total-level
// unit (0.75 dB); stop_music counts $10 down to 0, one step every 5 frames.
constexpr m_byte kFadeSteps   = 0x10u;
constexpr double kFadeStepDb  = 0.75;
constexpr int    kSampleRate  = 48000;

struct Sound {
    bool                  present   = false;
    std::filesystem::path file;
    long long             loopStart = -1;
    MIX_Audio            *audio     = nullptr; // effects and voices: predecoded
};

bool                         initialized = false; // SDL audio + MIX_Init held by open()
MIX_Mixer                   *mixer      = nullptr;
std::array<Sound, 0x100>     sounds;
MIX_Track                   *musicTrack = nullptr;
MIX_Track                   *pcmTrack   = nullptr;
std::map<m_long, MIX_Track *> sfxTracks; // keyed by the SFX channel block it arms

// CPU-thread state between the queue entry and the next fade-music entry.
m_byte picked      = 0;
bool   musicPaused = false;
bool   trace       = false; // SOR_SOUNDTRACK_TRACE=1: log every engine event
float  musicGain   = 1.0f;

bool play(MIX_Track *track, const Sound &sound) {
    const SDL_PropertiesID options = SDL_CreateProperties();
    if (sound.loopStart >= 0) {
        SDL_SetNumberProperty(options, MIX_PROP_PLAY_LOOPS_NUMBER, -1);
        SDL_SetNumberProperty(options, MIX_PROP_PLAY_LOOP_START_FRAME_NUMBER, sound.loopStart);
    }
    const bool ok = MIX_PlayTrack(track, options);
    SDL_DestroyProperties(options);
    return ok;
}

void playMusic(m_byte id) {
    const Sound &sound = sounds[id];
    MIX_StopTrack(musicTrack, 0);
    musicPaused = false;
    musicGain   = 1.0f;
    MIX_SetTrackGain(musicTrack, musicGain);
    if (!sound.present)
        return;
    // Music is streamed from disk: the renders hold minutes of audio each.
    SDL_IOStream *io = SDL_IOFromFile(sound.file.string().c_str(), "rb");
    if (io == nullptr || !MIX_SetTrackIOStream(musicTrack, io, true) || !play(musicTrack, sound))
        Logger::log("[soundtrack] cannot play music $%02X: %s", static_cast<unsigned>(id), SDL_GetError());
    else
        Logger::log("[soundtrack] music $%02X", static_cast<unsigned>(id));
}

// The engine arms one channel block per effect, chosen by header byte 3; a new
// effect on the same block replaces the one playing there.
m_long sfxChannelBlock(m_byte id, SystemMemory &memory) {
    const m_long header = memory.readLong(kSfxPointerTable + (id - 0xA0u) * 4u);
    const m_byte offset = static_cast<m_byte>((memory.readByte(header + 3u) - 2u) << 2);
    return memory.readLong(kSfxChannelTable + offset);
}

void playEffect(m_byte id, SystemMemory &memory) {
    const Sound &sound = sounds[id];
    if (!sound.present)
        return;
    const m_long block = sfxChannelBlock(id, memory);
    MIX_Track  *&track = sfxTracks[block];
    if (track == nullptr)
        track = MIX_CreateTrack(mixer);
    if (track == nullptr || !MIX_SetTrackAudio(track, sound.audio) || !play(track, sound))
        Logger::log("[soundtrack] cannot play sound $%02X: %s", static_cast<unsigned>(id), SDL_GetError());
}

void playVoice(m_byte id) {
    const Sound &sound = sounds[id];
    if (!sound.present)
        return;
    // The Z80 driver streams one sample at a time; a new command replaces it.
    if (!MIX_SetTrackAudio(pcmTrack, sound.audio) || !play(pcmTrack, sound))
        Logger::log("[soundtrack] cannot play voice $%02X: %s", static_cast<unsigned>(id), SDL_GetError());
}

void stopEffects(bool onlyFmBlocks) {
    for (auto &[block, track] : sfxTracks) {
        if (!onlyFmBlocks || block == kSfxFm0 || block == kSfxFm1)
            MIX_StopTrack(track, 0);
    }
}

// $72D08: the same pick as the ROM. Each queued ID with bit 7 set and a
// priority (from ID - $81) at least the running one replaces the pending ID.
void pickFromQueue(SystemMemory &memory) {
    m_byte active  = memory.readByte(kSoundActivePriority);
    m_byte pending = memory.readByte(kPendingId);
    for (m_long slot = 0; slot < 3; ++slot) {
        const m_byte id = memory.readByte(kPlaySe + slot);
        if ((id & 0x80u) == 0 || id < 0x81u)
            continue;
        const m_byte priority = memory.readByte(kPriorityTable + (id - 0x81u));
        if (priority >= active) {
            active  = priority;
            pending = id;
        }
    }
    // Bit 7 clear sends the ROM to sound_reset_all, and $80 is idle.
    picked = (pending & 0x80u) != 0 && pending != 0x80u ? pending : 0;
}

void startPicked(SystemMemory &memory) {
    const m_byte id = picked;
    picked          = 0;
    if (id == 0)
        return;
    if (trace)
        Logger::log("[soundtrack] start $%02X", static_cast<unsigned>(id));
    if (id < 0xA0u)
        playMusic(id);
    else if (id < 0xD0u)
        playEffect(id, memory);
    else if (id < 0xE0u)
        playVoice(id);
    else if (id == 0xE2u)
        stopEffects(true); // $72FE4: key off the two FM effect channels only
    // $E0 fades through stop_music below; $E1, $E3 and $E4+ reset the engine.
}

void followEngine(SystemMemory &memory) {
    const m_byte stop = memory.readByte(kStopMusic);
    const int    step = stop == 0 ? 0 : kFadeSteps - std::min(stop, kFadeSteps);
    const float  gain = static_cast<float>(std::pow(10.0, -kFadeStepDb * step / 20.0));
    if (gain != musicGain) {
        musicGain = gain;
        if (trace)
            Logger::log("[soundtrack] music gain %.3f", static_cast<double>(gain));
        MIX_SetTrackGain(musicTrack, gain);
    }
    // A positive gate (sequence command $F9) keys the music off and freezes
    // its channels while effects keep running; bit 7 then releases it.
    const m_byte gate   = memory.readByte(kEngineGate);
    const bool   paused = gate != 0 && (gate & 0x80u) == 0;
    if (paused != musicPaused) {
        musicPaused = paused;
        if (trace)
            Logger::log("[soundtrack] music %s", paused ? "paused" : "resumed");
        if (paused)
            MIX_PauseTrack(musicTrack);
        else
            MIX_ResumeTrack(musicTrack);
    }
}

bool loadManifest(const std::filesystem::path &directory) {
    std::ifstream manifest(directory / "manifest.tsv");
    if (!manifest)
        return false;
    std::string line;
    int         count = 0;
    while (std::getline(manifest, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream fields(line);
        std::string        idText, kind, file;
        long long          frames = 0, loopStart = -1, loopEnd = -1;
        if (!(fields >> idText >> kind >> file >> frames >> loopStart >> loopEnd))
            continue;
        const unsigned long id = std::stoul(idText, nullptr, 16);
        if (id < 0x81 || id > 0xFF)
            continue;
        Sound &sound    = sounds[id];
        sound.file      = directory / file;
        sound.loopStart = loopStart;
        if (kind != "music") {
            sound.audio = MIX_LoadAudio(mixer, sound.file.string().c_str(), true);
            if (sound.audio == nullptr) {
                Logger::log("[soundtrack] cannot load %s: %s", file.c_str(), SDL_GetError());
                continue;
            }
        }
        sound.present = true;
        ++count;
    }
    Logger::log("[soundtrack] %d pre-rendered sounds from %s", count, directory.string().c_str());
    return count > 0;
}

} // namespace

bool open(const std::filesystem::path &directory) {
    if (!std::filesystem::is_regular_file(directory / "manifest.tsv"))
        return false;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        Logger::log("[soundtrack] cannot start SDL audio: %s", SDL_GetError());
        return false;
    }
    if (!MIX_Init()) {
        Logger::log("[soundtrack] cannot start SDL_mixer: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    initialized = true;
    const char *traceValue = SDL_getenv("SOR_SOUNDTRACK_TRACE");
    trace                  = traceValue != nullptr && *traceValue != '\0' && *traceValue != '0';
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kSampleRate};
    mixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (mixer != nullptr) {
        musicTrack = MIX_CreateTrack(mixer);
        pcmTrack   = MIX_CreateTrack(mixer);
    }
    if (mixer == nullptr || musicTrack == nullptr || pcmTrack == nullptr || !loadManifest(directory)) {
        Logger::log("[soundtrack] unavailable: %s", SDL_GetError());
        close();
        return false;
    }
    return true;
}

void close() {
    for (Sound &sound : sounds) {
        if (sound.audio != nullptr)
            MIX_DestroyAudio(sound.audio); // reference-counted: playing tracks keep theirs
        sound = Sound{};
    }
    if (mixer != nullptr)
        MIX_DestroyMixer(mixer); // also destroys its tracks
    mixer      = nullptr;
    musicTrack = nullptr;
    pcmTrack   = nullptr;
    sfxTracks.clear();
    if (initialized) {
        MIX_Quit();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        initialized = false;
    }
}

bool isOpen() {
    return mixer != nullptr;
}

void onEntry(m_long entry, SystemMemory &memory) {
    switch (entry) {
        case kProcessQueueEntry:
            pickFromQueue(memory);
            break;
        case kResetAllEntry:
            if (trace)
                Logger::log("[soundtrack] reset");
            MIX_StopTrack(musicTrack, 0);
            stopEffects(false);
            musicPaused = false;
            break;
        case kFadeMusicEntry:
            startPicked(memory);
            followEngine(memory);
            break;
        default:
            break;
    }
}

} // namespace SoRSoundtrack
