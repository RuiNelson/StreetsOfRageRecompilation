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
#include <string>

int main(int argc, char *argv[]) {
    CLI::App app{"MegaDrive Environment"};
    app.set_version_flag("-V,--version", "0.1.0");

    bool          sorDebugFlag       = false;
    bool          debugUtilsFlag     = false;
    bool          fullScreenFlag     = false;
    bool          silentFlag         = false;
    bool          widescreenFlag     = false;
    bool          integerScalingFlag = false;
    bool          aiAssistFlag       = false;
    int           sorVSyncMode       = 0; // 0 = internal timer (default); 1/2/3 = VSync/VSync2/VSync3
    int           remoteAccessPort   = 6969;
    std::uint32_t turboMultiplier    = 0;
    std::string   sorRomPath         = "rom/SOR.bin";
    std::string   sorAuxAddrFile; // if set, record unknown dispatch targets here
    std::string   sorCallLogFile;
    std::string   languagePin = "jp";
    std::string   videoHz     = "60";
    std::string   soundMode   = "prerendered";
    std::string   soundtrackDir; // default: soundtrack/ next to the ROM
    std::string   aiPath = "ai-code";

    app.add_flag("--silent", silentFlag, "Disable audio output entirely (chip writes are dropped)");
    app.add_option("--sound",
                   soundMode,
                   "Audio source: prerendered (the tools/soundtrack FLAC renders; emulated when they are "
                   "missing) or emulated (YM2612, PSG and Z80 DAC in real time)")
        ->capture_default_str()
        ->check(CLI::IsMember({"prerendered", "emulated"}));
    app.add_option("--soundtrack", soundtrackDir, "Pre-rendered soundtrack directory (default: soundtrack next to the ROM)");
    app.add_option("--lang", languagePin, "Console language pin: jp=low/Japanese, en=high/overseas")
        ->capture_default_str()
        ->check(CLI::IsMember({"jp", "en"}));
    app.add_option("--hz", videoHz, "Console video frequency pin: 60=low/NTSC, 50=high/PAL")
        ->capture_default_str()
        ->check(CLI::IsMember({"60", "50"}));
    app.add_flag("--debug", sorDebugFlag, "Log CPU/VDP state once per second");
    app.add_flag("--debugUtils", debugUtilsFlag, "Enable debug hotkeys, cheats and remote access");
    app.add_flag("--fullScreen", fullScreenFlag, "Start the game in fullscreen");
    app.add_flag("--widescreen",
                 widescreenFlag,
                 "Use VDP Mode 48 (48x27 tiles, 384x216) instead of Mode 40; a host-only mode the hardware lacks");
    app.add_flag("--integerScaling",
                 integerScalingFlag,
                 "Scale the picture by whole multiples only (320x224 in 1920x1080 is drawn 4x at 1280x896)");
    app.add_option("--vsync",
                   sorVSyncMode,
                   "Frame sync — 0=automatic: internal timer from --hz, no tearing, any monitor (default), "
                   "1=VSync, 2=VSync2 (½ rate), 3=VSync3 (⅓ rate)")
        ->capture_default_str()
        ->check(CLI::Range(0, 3));
    app.add_option("--rom", sorRomPath, "Streets of Rage ROM path")->capture_default_str();
    app.add_option("--port", remoteAccessPort, "With --debugUtils: remote access TCP port (0 disables)")
        ->capture_default_str()
        ->check(CLI::Range(0, 65535));
    app.add_option("--turbo", turboMultiplier, "With --vsync 0: run the internal VDP at 60 Hz times this multiplier")
        ->check(CLI::Range(1u, 4'294'967'295u));
    app.add_option("--auxAddrFile",
                   sorAuxAddrFile,
                   "On an indirect dispatch to an unknown address, append it to "
                   "this aux file and exit (42) instead of aborting — for the discovery loop");
    app.add_option("--callLog",
                   sorCallLogFile,
                   "Write every 68000 subroutine entry and call to this typed CSV file "
                   "(event, source, callsite, target)");

    app.add_option("--ai-path", aiPath, "Directory holding the Python AI's code (the sor_ai package)")
        ->capture_default_str();
    app.add_flag("--aiAssist", aiAssistFlag, "Debug: P2 is the Python AI from launch, whichever mode is chosen");

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

    {
        StreetsOfRage sor(sorRomPath,
                          static_cast<VDP::Synchronization>(sorVSyncMode),
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
