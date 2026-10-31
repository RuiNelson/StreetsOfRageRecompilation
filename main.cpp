#include "SoRCheats.hpp"
#include "SoRControlsConfig.hpp"
#include "SoRInput.hpp"
#include "SoRPython.hpp"
#include "SoRRemoteInput.hpp"
#include "SoRSoundtrack.hpp"
#include "SoR.hpp"
#include <CLI/CLI.hpp>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>

int main(int argc, char *argv[]) {
    CLI::App app{"Streets of Rage"};
    app.set_version_flag("-V,--version", "0.1.0");

    bool          sorDebugFlag       = false;
    bool          debugUtilsFlag     = false;
    bool          fullScreenFlag     = false;
    bool          silentFlag         = false;
    bool          widescreenFlag     = false;
    bool          integerScalingFlag = false;
    bool          aiAssistFlag       = false;
    int           remoteAccessPort   = 6969;
    std::uint32_t turboMultiplier    = 0;
    std::string   sorRomPath         = "rom/SOR.bin";
    std::string   sorAuxAddrFile; // if set, record unknown dispatch targets here
    std::string   sorCallLogFile;
    std::string   frameSync   = "auto";
    std::string   languagePin = "jp";
    std::string   videoHz     = "60";
    std::string   soundMode   = "prerendered";
    std::string   soundtrackDir; // default: soundtrack/ next to the ROM
    std::string   aiPath = "ai-code";

    app.add_option("--rom", sorRomPath, "Streets of Rage ROM path")->capture_default_str()->group("Game");
    app.add_option("--lang", languagePin, "Console language: jp=Japanese, en=overseas")
        ->capture_default_str()
        ->check(CLI::IsMember({"jp", "en"}))
        ->group("Game");
    app.add_option("--hz", videoHz, "Console video frequency: 60=NTSC, 50=PAL")
        ->capture_default_str()
        ->check(CLI::IsMember({"60", "50"}))
        ->group("Game");

    app.add_flag("--fullscreen", fullScreenFlag, "Start in fullscreen")->group("Video");
    app.add_flag("--widescreen", widescreenFlag, "384x216 (VDP Mode 48, which the hardware lacks) instead of 320x224")
        ->group("Video");
    app.add_flag("--integer-scaling", integerScalingFlag, "Scale the picture by whole multiples only")->group("Video");
    app.add_option("--frame-sync",
                   frameSync,
                   "What sets the game's speed. auto: the --hz timer, no tearing, any monitor. "
                   "vsync, vsync-half, vsync-third: the monitor, every 1, 2 or 3 refreshes")
        ->capture_default_str()
        ->check(CLI::IsMember({"auto", "vsync", "vsync-half", "vsync-third"}))
        ->group("Video");

    app.add_flag("--silent", silentFlag, "No audio at all (chip writes are dropped)")->group("Audio");
    app.add_option("--sound",
                   soundMode,
                   "Audio source. prerendered: the soundtrack's FLAC renders, emulated when they are missing. "
                   "emulated: YM2612, PSG and Z80 DAC in real time")
        ->capture_default_str()
        ->check(CLI::IsMember({"prerendered", "emulated"}))
        ->group("Audio");
    app.add_option("--soundtrack", soundtrackDir, "Pre-rendered soundtrack folder (default: soundtrack next to the ROM)")
        ->group("Audio");

    app.add_option("--ai-path", aiPath, "Folder holding the Python AI's code (the sor_ai package)")
        ->capture_default_str()
        ->group("AI");
    app.add_flag("--ai-assist", aiAssistFlag, "P2 is the Python AI from launch, whichever mode is chosen")->group("AI");

    app.add_flag("--debug-utils", debugUtilsFlag, "Debug hotkeys, cheats and remote access")->group("Development");
    app.add_option("--port", remoteAccessPort, "With --debug-utils: remote access TCP port (0 disables)")
        ->capture_default_str()
        ->check(CLI::Range(0, 65535))
        ->group("Development");
    app.add_option("--turbo", turboMultiplier, "With --frame-sync auto: run at 60 Hz times this multiplier")
        ->check(CLI::Range(1u, 4'294'967'295u))
        ->group("Development");
    app.add_flag("--debug-log", sorDebugFlag, "Log CPU and VDP state once per second")->group("Development");
    app.add_option("--call-log", sorCallLogFile, "Write every 68000 subroutine entry and call to this CSV file")
        ->group("Development");
    app.add_option("--aux-addr-file",
                   sorAuxAddrFile,
                   "On an indirect dispatch to an unknown address, append it to this file and exit (42) "
                   "instead of aborting: the discovery loop")
        ->group("Development");

    CLI11_PARSE(app, argc, argv);

    auto configureEnvironment = [&](MegaDriveEnvironment &env) {
        env.setLanguagePin(languagePin == "en" ? MegaDriveEnvironment::LanguagePin::Overseas
                                               : MegaDriveEnvironment::LanguagePin::Japanese);
        env.setVideoStandard(videoHz == "50" ? MegaDriveEnvironment::VideoStandard::Hz50
                                             : MegaDriveEnvironment::VideoStandard::Hz60);
        env.setVDPTurboMultiplier(turboMultiplier);
        env.setWidescreen(widescreenFlag);
        env.setIntegerScaling(integerScalingFlag);
        if (silentFlag) {
            env.sound().disable();
        } else if (soundMode == "prerendered") {
            const std::filesystem::path directory = soundtrackDir.empty()
                                                        ? std::filesystem::path(sorRomPath).parent_path() / "soundtrack"
                                                        : std::filesystem::path(soundtrackDir);
            // The engine still runs; only the chip output is replaced.
            if (SoRSoundtrack::open(directory))
                env.sound().disable();
            else
                std::fprintf(stderr,
                             "No pre-rendered soundtrack in %s (run scripts/render_soundtrack.py); "
                             "emulating the sound chips\n",
                             directory.string().c_str());
        }
    };

    const std::map<std::string, VDP::Synchronization> frameSyncModes{
        {"auto", VDP::InternalTimer}, {"vsync", VDP::VSync}, {"vsync-half", VDP::VSync2}, {"vsync-third", VDP::VSync3}};

    {
        StreetsOfRage sor(sorRomPath,
                          frameSyncModes.at(frameSync),
                          VDP::Integer,
                          debugUtilsFlag ? static_cast<std::uint16_t>(remoteAccessPort) : 0);
        configureEnvironment(sor);
        // The game reads the keyboard and gamepads itself (controls.yaml);
        // MegaDriveEnvironment keeps only the remote (autoplay) buttons.
        sor.controllers().setPhysicalInputEnabled(false);
        SoRInput::start(SoRControlsConfig::defaultPath());
        // Host mode flags for GET_FLAGS; the indices are mirrored by autoplay/src/sor_autoplay/host_flags.py.
        // (Flag 1 was --altControls; each player's scheme is now in the game data, SoRRemoteInput.)
        constexpr std::size_t kFlagWidescreen = 0;
        sor.remoteAccess().setFlag(kFlagWidescreen, widescreenFlag);
        sor.setDebugLog(sorDebugFlag);
        sor.setDebugUtilities(debugUtilsFlag);
        sor.setStartFullscreen(fullScreenFlag);
        if (!sorAuxAddrFile.empty()) {
            sor.setAuxAddrFile(sorAuxAddrFile);
        }
        if (!sorCallLogFile.empty()) {
            sor.setCallLog(sorCallLogFile);
        }
        SoRPython::configure(aiPath, widescreenFlag);
        if (aiAssistFlag) {
            SoRRemoteInput::forceAiAssist();
            SoRPython::start(sor.memory());
        }
        sor.boot();
        SoRPython::stop();
        SoRInput::stop();
        SoRSoundtrack::close();
    }

    return 0;
}
