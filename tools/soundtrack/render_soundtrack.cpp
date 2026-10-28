// Pre-renders every Streets of Rage music track, sound effect and PCM voice
// with Genesis Plus GX (Nuked OPN2) into FLAC files plus a manifest that the
// `sor` host plays instead of emulating the sound chips in real time.
//
// The game's own 68000 sound engine produces the audio: the renderer boots the
// ROM, parks the main loop (the VBlank handler keeps ticking `sound_engine`),
// and posts each sound ID into `play_se` exactly as game code does. A looping
// track is detected when the music sequencer's work RAM repeats a previous
// frame byte for byte, which gives frame-exact loop points.

#include "libretro.h"

#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
unsigned sor_gpgx_read_ram8(unsigned addr);
void     sor_gpgx_write_ram8(unsigned addr, unsigned value);
unsigned sor_gpgx_read_zram8(unsigned addr);
unsigned sor_gpgx_read_rom8(unsigned addr);
void     sor_gpgx_write_rom8(unsigned addr, unsigned value);
void     sor_gpgx_set_audio_rate(int samplerate, double framerate);
void     sor_gpgx_clear_audio(void);
}

namespace fs = std::filesystem;

namespace {

constexpr int    kSampleRate      = 48000;
// The host runs the console at exactly 60 Hz, so the renders are paced the
// same way: 800 samples per frame makes every frame boundary, and so every
// loop point, an exact sample index.
constexpr double kFrameRate       = 60.0;
constexpr int    kFramesPerSecond = 60;

// Work RAM (low 16 bits of $FFxxxx), from code-analysis/addresses.csv.
constexpr unsigned kPlaySe          = 0xF00A; // play_se
constexpr unsigned kPendingId       = 0xF009; // sound_pending_id
constexpr unsigned kEngineGate      = 0xF007; // sound_engine_gate
constexpr unsigned kMusicFirst      = 0xF040; // sound_music_fm0 .. psg2: 9 blocks
constexpr unsigned kMusicEnd        = 0xF1F0;
constexpr unsigned kSfxFirst        = 0xF1F0; // sound_sfx_fm0 .. sfx_fm_extra
constexpr unsigned kSfxEnd          = 0xF370;
constexpr unsigned kChannelSize     = 0x30;
// Z80 RAM: bit 7 while the DAC driver streams a sample (z80_dac_busy).
constexpr unsigned kZ80DacBusy      = 0x1FFD;
// ROM: game_infinite_loop, sound test name rows.
constexpr unsigned kMainLoop        = 0x0003A2;
constexpr unsigned kSoundTestIds    = 0x00128A;
constexpr unsigned kSoundTestNames  = 0x01C684;
constexpr unsigned kSoundTestCount  = 0x49;

enum class Kind { Music, Sfx, Pcm };

const char *kindName(Kind kind) {
    switch (kind) {
        case Kind::Music: return "music";
        case Kind::Sfx: return "sfx";
        case Kind::Pcm: return "pcm";
    }
    return "?";
}

Kind kindOf(unsigned id) {
    if (id < 0xA0)
        return Kind::Music;
    if (id < 0xD0)
        return Kind::Sfx;
    return Kind::Pcm;
}

// --- libretro frontend ------------------------------------------------------

std::string          g_systemDir;
std::string          g_chipOption = "nuked (ym3438)";
std::vector<int16_t> g_audio; // interleaved stereo for the current render

const char *optionValue(const std::string &key) {
    if (key == "genesis_plus_gx_ym2612")
        return g_chipOption.c_str();
    if (key == "genesis_plus_gx_audio_filter")
        return "disabled";
    if (key == "genesis_plus_gx_sound_output")
        return "stereo";
    if (key == "genesis_plus_gx_region_detect")
        return "ntsc-u";
    if (key == "genesis_plus_gx_psg_preamp")
        return "150";
    if (key == "genesis_plus_gx_fm_preamp")
        return "100";
    if (key == "genesis_plus_gx_frameskip")
        return "disabled";
    if (key == "genesis_plus_gx_overclock")
        return "100%";
    return nullptr;
}

bool environment(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            auto *variable  = static_cast<retro_variable *>(data);
            variable->value = optionValue(variable->key);
            return variable->value != nullptr;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *static_cast<bool *>(data) = false;
            return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *static_cast<const char **>(data) = g_systemDir.c_str();
            return true;
        default:
            return false;
    }
}

void   videoRefresh(const void *, unsigned, unsigned, size_t) {}
void   audioSample(int16_t left, int16_t right) {
    g_audio.push_back(left);
    g_audio.push_back(right);
}
size_t audioBatch(const int16_t *data, size_t frames) {
    g_audio.insert(g_audio.end(), data, data + frames * 2);
    return frames;
}
void    inputPoll() {}
int16_t inputState(unsigned, unsigned, unsigned, unsigned) {
    return 0;
}

// --- helpers ------------------------------------------------------------------

std::vector<uint8_t> readRam(unsigned first, unsigned end) {
    std::vector<uint8_t> bytes(end - first);
    for (unsigned i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>(sor_gpgx_read_ram8(first + i));
    return bytes;
}

bool anyChannelActive(unsigned first, unsigned end) {
    for (unsigned block = first; block < end; block += kChannelSize) {
        if (sor_gpgx_read_ram8(block) & 0x80u)
            return true;
    }
    return false;
}

size_t stereoFrames() {
    return g_audio.size() / 2;
}

// The sound test's 12-byte name rows: 0 is a space, 1-26 are A-Z, $1B-$24
// are 0-9.
std::string soundTestName(unsigned id) {
    for (unsigned index = 0; index < kSoundTestCount; ++index) {
        if (sor_gpgx_read_rom8(kSoundTestIds + index) != id)
            continue;
        std::string name;
        for (unsigned i = 0; i < 12; ++i) {
            const unsigned c = sor_gpgx_read_rom8(kSoundTestNames + index * 12 + i);
            if (c == 0)
                name += ' ';
            else if (c <= 26)
                name += static_cast<char>('A' + c - 1);
            else if (c >= 0x1B && c <= 0x24)
                name += static_cast<char>('0' + c - 0x1B);
            else
                name += '?';
        }
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        return name;
    }
    return {};
}

struct Render {
    unsigned             id   = 0;
    Kind                 kind = Kind::Music;
    std::vector<int16_t> samples; // interleaved stereo
    std::optional<size_t> loopStart;
    std::optional<size_t> loopEnd;
    int                  framesRun   = 0;
    bool                 gateTouched = false;
    bool                 truncated   = false;
};

bool writeFlac(const fs::path &path, const Render &render, const std::string &name) {
    FLAC__StreamEncoder *encoder = FLAC__stream_encoder_new();
    if (encoder == nullptr)
        return false;
    FLAC__stream_encoder_set_channels(encoder, 2);
    FLAC__stream_encoder_set_bits_per_sample(encoder, 16);
    FLAC__stream_encoder_set_sample_rate(encoder, kSampleRate);
    FLAC__stream_encoder_set_compression_level(encoder, 8);
    FLAC__stream_encoder_set_total_samples_estimate(encoder, render.samples.size() / 2);

    FLAC__StreamMetadata *tags = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    auto addTag = [tags](const std::string &key, const std::string &value) {
        FLAC__StreamMetadata_VorbisComment_Entry entry;
        FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&entry, key.c_str(), value.c_str());
        FLAC__metadata_object_vorbiscomment_append_comment(tags, entry, false);
    };
    char idText[8];
    std::snprintf(idText, sizeof idText, "$%02X", render.id);
    addTag("TITLE", name.empty() ? std::string(idText) : name + " (" + idText + ")");
    addTag("ALBUM", "Streets of Rage (pre-rendered)");
    addTag("SOR_SOUND_ID", idText + 1);
    addTag("ENCODER_SETTINGS", "Genesis Plus GX " SOR_GPGX_COMMIT ", " + g_chipOption + ", no filter, 48 kHz, 60 Hz");
    if (render.loopStart) {
        addTag("LOOPSTART", std::to_string(*render.loopStart));
        addTag("LOOPLENGTH", std::to_string(*render.loopEnd - *render.loopStart));
    }
    FLAC__StreamMetadata *metadata[] = {tags};
    FLAC__stream_encoder_set_metadata(encoder, metadata, 1);

    bool ok = FLAC__stream_encoder_init_file(encoder, path.string().c_str(), nullptr, nullptr) ==
              FLAC__STREAM_ENCODER_INIT_STATUS_OK;
    if (ok) {
        std::vector<FLAC__int32> wide(render.samples.begin(), render.samples.end());
        ok = FLAC__stream_encoder_process_interleaved(encoder, wide.data(), static_cast<unsigned>(wide.size() / 2));
    }
    ok = FLAC__stream_encoder_finish(encoder) && ok;
    FLAC__stream_encoder_delete(encoder);
    FLAC__metadata_object_delete(tags);
    return ok;
}

// --- rendering ---------------------------------------------------------------

struct Options {
    fs::path              rom;
    fs::path              out;
    std::vector<unsigned> ids;
    int                   bootFrames = 600;
    int                   maxMinutes = 10;
};

constexpr size_t kSamplesPerFrame = static_cast<size_t>(kSampleRate / kFrameRate);

// The base state is digital silence (every sample 0), so a render starts at
// its first non-zero sample, at most one frame in: the sound engine's first
// chip writes land 20-400 samples into the first frame.
size_t leadIn() {
    const size_t limit = std::min(stereoFrames(), kSamplesPerFrame);
    for (size_t i = 0; i < limit; ++i) {
        if (g_audio[i * 2] != 0 || g_audio[i * 2 + 1] != 0)
            return i;
    }
    return limit;
}

// The sequencer's RAM repeats exactly but the chips' free-running phases (PSG
// tone counters, LFO) do not, so the samples before the loop end differ from
// those before the loop start. The last kSeamFrames before the loop end fade
// into the audio that precedes the loop start, so the jump back is continuous.
constexpr size_t kSeamFrames = kSampleRate / 40; // 25 ms

void blendLoopSeam(std::vector<int16_t> &samples, size_t loopStart, size_t loopEnd) {
    const size_t length = std::min({kSeamFrames, loopStart, loopEnd - loopStart});
    for (size_t i = 0; i < length; ++i) {
        const double weight = 0.5 - 0.5 * std::cos(3.14159265358979323846 * (i + 1) / length);
        const size_t into   = loopEnd - length + i;
        const size_t from   = loopStart - length + i;
        for (size_t channel = 0; channel < 2; ++channel) {
            const double mixed = (1.0 - weight) * samples[into * 2 + channel] + weight * samples[from * 2 + channel];
            samples[into * 2 + channel] = static_cast<int16_t>(std::lround(std::clamp(mixed, -32768.0, 32767.0)));
        }
    }
}

// A tail is quiet when every sample of the last `window` frames stays within
// `tolerance` of the final sample (the DAC can leave a constant offset).
bool quietTail(size_t window, int tolerance) {
    if (stereoFrames() < window)
        return false;
    const int16_t lastL = g_audio[g_audio.size() - 2];
    const int16_t lastR = g_audio[g_audio.size() - 1];
    for (size_t i = g_audio.size() - window * 2; i < g_audio.size(); i += 2) {
        if (std::abs(g_audio[i] - lastL) > tolerance || std::abs(g_audio[i + 1] - lastR) > tolerance)
            return false;
    }
    return true;
}

Render renderSound(unsigned id, const std::vector<uint8_t> &baseState, const Options &options) {
    Render render;
    render.id   = id;
    render.kind = kindOf(id);

    retro_unserialize(baseState.data(), baseState.size());
    sor_gpgx_clear_audio();
    g_audio.clear();

    const unsigned stateFirst = render.kind == Kind::Music ? kMusicFirst : kSfxFirst;
    const unsigned stateEnd   = render.kind == Kind::Music ? kMusicEnd : kSfxEnd;
    const int      maxFrames  = options.maxMinutes * 60 * kFramesPerSecond;
    constexpr int  kTailFrames   = 5 * kFramesPerSecond; // loop margin past release tails
    constexpr int  kSettleFrames = 4 * kFramesPerSecond; // max release after the last channel ends

    sor_gpgx_write_ram8(kPlaySe, id);

    std::vector<size_t>               frameEnd{0}; // stereo frames at each frame boundary
    std::vector<std::vector<uint8_t>> states;      // sequencer RAM after each frame
    std::unordered_map<uint64_t, std::vector<int>> seen;
    std::optional<int>                loopFrom;    // frame M whose state frame N repeats
    std::optional<int>                loopTo;
    int                               endedAt = -1;

    for (int frame = 1;; ++frame) {
        retro_run();
        frameEnd.push_back(stereoFrames());
        render.framesRun = frame;
        if (sor_gpgx_read_ram8(kEngineGate) != 0)
            render.gateTouched = true;

        if (loopTo) {
            if (frame >= *loopTo + kTailFrames)
                break;
            continue;
        }

        const bool channels = anyChannelActive(stateFirst, stateEnd);
        const bool dacBusy  = (sor_gpgx_read_zram8(kZ80DacBusy) & 0x80u) != 0;
        if (endedAt < 0 && !channels && (render.kind != Kind::Pcm || !dacBusy) && frame > 2)
            endedAt = frame;
        if (endedAt >= 0) {
            if (quietTail(static_cast<size_t>(kSampleRate / 10), 1) || frame >= endedAt + kSettleFrames)
                break;
            continue;
        }

        if (render.kind != Kind::Pcm) {
            std::vector<uint8_t> state = readRam(stateFirst, stateEnd);
            uint64_t             hash  = 14695981039346656037ull; // FNV-1a
            for (const uint8_t byte : state)
                hash = (hash ^ byte) * 1099511628211ull;
            for (const int previous : seen[hash]) {
                if (states[previous] == state) {
                    loopFrom = previous + 1; // states[k] is the state after frame k + 1
                    loopTo   = frame;
                    break;
                }
            }
            seen[hash].push_back(static_cast<int>(states.size()));
            states.push_back(std::move(state));
        }
        if (frame >= maxFrames) {
            render.truncated = true;
            break;
        }
    }

    const size_t start  = leadIn();
    size_t       length = stereoFrames();
    if (loopTo) {
        // Loop one tail margin into the periodic part, so that the release
        // tails of the intro have died out at the loop start.
        const int margin = std::min(kTailFrames, *loopTo - *loopFrom);
        length           = frameEnd[*loopTo + margin];
        render.loopStart = frameEnd[*loopFrom + margin] - start;
        render.loopEnd   = length - start;
    } else {
        // Drop the settled tail: keep everything up to the last change.
        const int16_t lastL = g_audio[g_audio.size() - 2];
        const int16_t lastR = g_audio[g_audio.size() - 1];
        size_t        end   = stereoFrames();
        while (end > start + 1 && std::abs(g_audio[(end - 1) * 2] - lastL) <= 1 &&
               std::abs(g_audio[(end - 1) * 2 + 1] - lastR) <= 1)
            --end;
        length = std::min(stereoFrames(), end + 1);
    }
    render.samples.assign(g_audio.begin() + static_cast<std::ptrdiff_t>(start * 2),
                          g_audio.begin() + static_cast<std::ptrdiff_t>(length * 2));
    if (render.loopStart)
        blendLoopSeam(render.samples, *render.loopStart, *render.loopEnd);
    return render;
}

bool parseIds(const std::string &text, std::vector<unsigned> &ids) {
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        const std::string item = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        const size_t dash = item.find('-');
        char *end = nullptr;
        const unsigned first = static_cast<unsigned>(std::strtoul(item.c_str(), &end, 16));
        unsigned last = first;
        if (dash != std::string::npos)
            last = static_cast<unsigned>(std::strtoul(item.c_str() + dash + 1, nullptr, 16));
        if (item.empty() || first < 0x81 || last > 0xDF || last < first)
            return false;
        for (unsigned id = first; id <= last; ++id)
            ids.push_back(id);
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return true;
}

void usage(const char *program) {
    std::fprintf(stderr,
                 "Usage: %s --rom SOR.bin --out DIR [--ids 81-91,A0-CF,D0-DB] [--chip ym3438|ym2612]\n"
                 "          [--boot-frames N] [--max-minutes N]\n",
                 program);
}

} // namespace

int main(int argc, char **argv) {
    Options     options;
    std::string idText = "81-91,A0-CF,D0-DB";
    for (int i = 1; i < argc; ++i) {
        const std::string arg  = argv[i];
        auto              next = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage(argv[0]);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--rom")
            options.rom = next();
        else if (arg == "--out")
            options.out = next();
        else if (arg == "--ids")
            idText = next();
        else if (arg == "--chip") {
            const std::string chip = next();
            if (chip != "ym3438" && chip != "ym2612") {
                usage(argv[0]);
                return 2;
            }
            g_chipOption = "nuked (" + chip + ")";
        } else if (arg == "--boot-frames")
            options.bootFrames = std::stoi(next());
        else if (arg == "--max-minutes")
            options.maxMinutes = std::stoi(next());
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if (options.rom.empty() || options.out.empty() || !parseIds(idText, options.ids)) {
        usage(argv[0]);
        return 2;
    }

    fs::create_directories(options.out);
    g_systemDir = fs::absolute(options.out).string();

    retro_set_environment(environment);
    retro_set_video_refresh(videoRefresh);
    retro_set_audio_sample(audioSample);
    retro_set_audio_sample_batch(audioBatch);
    retro_set_input_poll(inputPoll);
    retro_set_input_state(inputState);
    retro_init();

    const std::string romPath = fs::absolute(options.rom).string();
    retro_game_info   game{};
    game.path = romPath.c_str();
    if (!retro_load_game(&game)) {
        std::fprintf(stderr, "Cannot load %s\n", romPath.c_str());
        return 1;
    }
    sor_gpgx_set_audio_rate(kSampleRate, kFrameRate);

    // game_infinite_loop starts with moveq #0,d0 / move.w (game_state).w,d0.
    const unsigned expected[] = {0x70, 0x00, 0x30, 0x38};
    for (unsigned i = 0; i < 4; ++i) {
        if (sor_gpgx_read_rom8(kMainLoop + i) != expected[i]) {
            std::fprintf(stderr, "Unexpected ROM at $%06X: not a compatible Streets of Rage dump\n", kMainLoop);
            return 1;
        }
    }

    // Boot to the title, then park the main loop on itself (bra.s *). VBlank
    // still runs sound_engine every frame, and nothing else posts sounds.
    for (int frame = 0; frame < options.bootFrames; ++frame)
        retro_run();
    sor_gpgx_write_rom8(kMainLoop, 0x60);
    sor_gpgx_write_rom8(kMainLoop + 1, 0xFE);
    for (int frame = 0; frame < 30; ++frame)
        retro_run();
    sor_gpgx_write_ram8(kPlaySe, 0xE1); // stop everything the attract mode left
    for (int frame = 0; frame < 120; ++frame)
        retro_run();
    if (anyChannelActive(kMusicFirst, kSfxEnd) || sor_gpgx_read_ram8(kPendingId) != 0x80) {
        std::fprintf(stderr, "The sound engine did not settle after the stop command\n");
        return 1;
    }

    std::vector<uint8_t> baseState(retro_serialize_size());
    if (!retro_serialize(baseState.data(), baseState.size())) {
        std::fprintf(stderr, "Cannot save the base state\n");
        return 1;
    }

    std::ofstream manifest(options.out / "manifest.tsv");
    manifest << "# Streets of Rage pre-rendered soundtrack. Generated by sor_soundtrack; do not edit.\n"
             << "# sample_rate=" << kSampleRate << " frame_rate=" << kFramesPerSecond
             << " chip=" << g_chipOption << " gpgx=" << SOR_GPGX_COMMIT << "\n"
             << "# id\tkind\tfile\tframes\tloop_start\tloop_end\tname\n";

    int failures = 0;
    for (const unsigned id : options.ids) {
        const Render render = renderSound(id, baseState, options);
        const std::string name = soundTestName(id);
        int peak = 0;
        for (const int16_t sample : render.samples)
            peak = std::max(peak, std::abs(static_cast<int>(sample)));

        char file[32];
        std::snprintf(file, sizeof file, "%s_%02X.flac", kindName(render.kind), id);
        const size_t frames = render.samples.size() / 2;
        std::printf("$%02X %-5s %-12s %7.2f s", id, kindName(render.kind), name.c_str(),
                    static_cast<double>(frames) / kSampleRate);
        if (render.loopStart)
            std::printf("  loop %.2f-%.2f s", static_cast<double>(*render.loopStart) / kSampleRate,
                        static_cast<double>(*render.loopEnd) / kSampleRate);
        std::printf("  peak %d%s%s\n", peak, render.gateTouched ? "  [engine gate used]" : "",
                    render.truncated ? "  [TRUNCATED: no loop found]" : "");

        if (peak == 0) {
            std::printf("    silent: no file written\n");
            continue;
        }
        if (!writeFlac(options.out / file, render, name)) {
            std::fprintf(stderr, "    cannot write %s\n", file);
            ++failures;
            continue;
        }
        manifest << std::hex << std::uppercase << id << std::dec << '\t' << kindName(render.kind) << '\t' << file
                 << '\t' << frames << '\t' << (render.loopStart ? static_cast<long long>(*render.loopStart) : -1)
                 << '\t' << (render.loopEnd ? static_cast<long long>(*render.loopEnd) : -1) << '\t' << name << '\n';
        if (render.truncated)
            ++failures;
    }

    retro_unload_game();
    retro_deinit();
    return failures == 0 ? 0 : 1;
}
