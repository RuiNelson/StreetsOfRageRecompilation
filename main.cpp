#include "SoRCheats.hpp"
#include "SoRControlsConfig.hpp"
#include "SoRInput.hpp"
#include "SoRPause.hpp"
#include "SoRPlatform.hpp"
#include "SoRPython.hpp"
#include "SoRRemoteInput.hpp"
#include "SoRSoundtrack.hpp"
#include "SoRVideoConfig.hpp"
#include "SoR.hpp"
#include <CLI/CLI.hpp>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <memory>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __PROSPERO__
#include <fstream>
#include <vector>

extern "C" void ps5_notify(const char *message); // prospero/src/startup.c
#endif

namespace {

#ifdef __PROSPERO__
// A title gets no command line: <data>/args.txt holds options for development, one a line.
std::vector<std::string> consoleArguments(const char *program) {
    std::vector<std::string> arguments{program};
    std::ifstream            file(std::string(SoRPlatform::kDataDir) + "/args.txt");
    for (std::string line; std::getline(file, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            arguments.push_back(line);
    }
    return arguments;
}
#endif

// On Windows `sor` is a GUI program (no console window). A console exists only when a debug option or
// --help/--version asks for output: the parent terminal's if there is one, else a new window.
void openConsoleIfWanted(int argc, char *argv[]) {
#ifdef _WIN32
    bool wanted = false;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (std::strcmp(a, "--debug-utils") == 0 || std::strcmp(a, "--debug-log") == 0 ||
            std::strcmp(a, "--call-log") == 0 || std::strcmp(a, "--aux-addr-file") == 0 ||
            std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0 || std::strcmp(a, "-V") == 0 ||
            std::strcmp(a, "--version") == 0)
            wanted = true;
    }
    if (!wanted)
        return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole())
        return;
    std::freopen("CONOUT$", "w", stdout);
    std::freopen("CONOUT$", "w", stderr);
#else
    (void)argc;
    (void)argv;
#endif
}

} // namespace

int main(int argc, char *argv[]) {
#ifdef __PROSPERO__
    std::vector<std::string> consoleOptions = consoleArguments(argc > 0 ? argv[0] : "sor");
    std::vector<char *>      consoleArgv;
    for (std::string &option : consoleOptions)
        consoleArgv.push_back(option.data());
    argc = static_cast<int>(consoleArgv.size());
    argv = consoleArgv.data();
#endif
    openConsoleIfWanted(argc, argv);
    CLI::App app{"Streets of Rage"};
    app.set_version_flag("-V,--version", "0.3.1");

    bool          sorDebugFlag       = false;
    bool          debugUtilsFlag     = false;
    bool          silentFlag         = false;
    bool          fullscreenFlag     = false;
    bool          widescreenFlag     = false;
    bool          integerScalingFlag = false;
    bool          defaultVideoFlag   = false;
    bool          aiAssistFlag       = false;
    int           remoteAccessPort   = 6969;
    std::uint32_t turboMultiplier    = 0;
#ifdef __PROSPERO__
    std::string   sorRomPath         = std::string(SoRPlatform::kDataDir) + "/SOR.bin";
#else
    std::string   sorRomPath         = "rom/SOR.bin";
#endif
    std::string   sorAuxAddrFile; // if set, record unknown dispatch targets here
    std::string   sorCallLogFile;
    std::string   frameSync;
    std::string   languagePin = "en";
    std::string   videoHz     = "60";
    std::string   soundMode   = "prerendered";
    std::string   soundtrackDir; // default: soundtrack/ next to the ROM
#ifdef __PROSPERO__
    std::string   aiPath = std::string(SoRPlatform::kDataDir) + "/ai-code";
#else
    std::string   aiPath = "ai-code";
#endif

    app.add_option("--rom", sorRomPath, "Streets of Rage ROM path")->capture_default_str()->group("Game");
    app.add_option("--lang", languagePin, "Console language: jp=Japanese, en=overseas")
        ->capture_default_str()
        ->check(CLI::IsMember({"jp", "en"}))
        ->group("Game");
    app.add_option("--hz", videoHz, "Console video frequency: 60=NTSC, 50=PAL")
        ->capture_default_str()
        ->check(CLI::IsMember({"60", "50"}))
        ->group("Game");

    // Each wins over video.yaml for this run; OPTIONS > VIDEO can still change it, and saves what it shows.
    const auto *fullscreenOption = app.add_flag("--fullscreen,!--windowed", fullscreenFlag, "Start in fullscreen, or in a window")
                                       ->group("Video");
    const auto *widescreenOption =
        app.add_flag("--widescreen,!--no-widescreen",
                     widescreenFlag,
                     "384x216 (VDP Mode 48, which the hardware lacks) instead of 320x224, or not")
            ->group("Video");
    const auto *integerScalingOption =
        app.add_flag("--integer-scaling,!--no-integer-scaling", integerScalingFlag, "Scale the picture by whole multiples only, or not")
            ->group("Video");
    const auto *frameSyncOption = app.add_option("--frame-sync",
                                                 frameSync,
                                                 "What sets the game's speed. auto: the --hz timer, no tearing, any monitor. "
                                                 "vsync, vsync-half, vsync-third: the monitor, every 1, 2 or 3 refreshes")
                                      ->check(CLI::IsMember({"auto", "vsync", "vsync-half", "vsync-third"}))
                                      ->group("Video");
    app.add_flag("--default-video",
                 defaultVideoFlag,
                 "Ignore video.yaml for this run: the defaults, and any other video option given. For scripts and measurements")
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
    app.add_flag("--console-menus",
                 SoRPlatform::consoleMenusFlag(),
                 "The PS5's menus (no BIND KEYBOARD, no DISPLAY) and always fullscreen, to test them on the desktop")
        ->group("Development");

    CLI11_PARSE(app, argc, argv);

    const auto soundtrackDirectory = [&]() -> std::filesystem::path {
        return soundtrackDir.empty() ? std::filesystem::path(sorRomPath).parent_path() / "soundtrack"
                                     : std::filesystem::path(soundtrackDir);
    };

#ifdef __PROSPERO__
    // Nothing here is optional on the console, whatever the options say. Standard error is klog
    // there, out of the player's sight, so the player is told in a notification.
    const auto missing = [](const std::filesystem::path &file) {
        std::error_code error;
        return !std::filesystem::is_regular_file(file, error);
    };
    const char *const kAdvice = " /data needs ShadowMountPlus 1.7beta4 or newer.";
    if (missing(sorRomPath)) {
        ps5_notify(("Streets of Rage: copy your ROM to " + sorRomPath + "." + kAdvice).c_str());
        return 0;
    }
    if (missing(soundtrackDirectory() / "manifest.tsv")) {
        ps5_notify(("Streets of Rage: copy the pre-rendered soundtrack to " + soundtrackDirectory().string() + "." + kAdvice).c_str());
        return 0;
    }
    if (missing(std::filesystem::path(aiPath) / "sor_ai" / "embedded.py")) {
        ps5_notify(("Streets of Rage: copy the AI's files to " + aiPath + "." + kAdvice).c_str());
        return 0;
    }
    if (missing(SoRPython::standardLibrary() / "os.py")) {
        ps5_notify(("Streets of Rage: copy Python's standard library to " + SoRPython::standardLibrary().string() + "." + kAdvice).c_str());
        return 0;
    }
#else
    // No log output unless a debug option asks for it (--help/--version and parse errors already printed).
    if (!debugUtilsFlag && !sorDebugFlag && sorCallLogFile.empty() && sorAuxAddrFile.empty()) {
#ifdef _WIN32
        constexpr const char *kNull = "NUL";
#else
        constexpr const char *kNull = "/dev/null";
#endif
        std::freopen(kNull, "w", stdout);
        std::freopen(kNull, "w", stderr);
    }
#endif

    SoRVideoConfig::VideoConfig video;
    if (!defaultVideoFlag)
        video = SoRVideoConfig::loadOrCreate(SoRVideoConfig::defaultPath());
    if (fullscreenOption->count() > 0)
        video.fullscreen = fullscreenFlag;
    if (widescreenOption->count() > 0)
        video.widescreen = widescreenFlag;
    if (integerScalingOption->count() > 0)
        video.integerScaling = integerScalingFlag;
    if (frameSyncOption->count() > 0)
        video.frameSync = *SoRVideoConfig::frameSyncFromKey(frameSync);
    else if (turboMultiplier > 0)
        video.frameSync = VDP::InternalTimer; // only the timer can run faster than the monitor
    if (SoRPlatform::consoleMenus())
        video.fullscreen = true;

    bool soundtrackFailed = false;
    auto configureEnvironment = [&](MegaDriveEnvironment &env) {
        env.setLanguagePin(languagePin == "en" ? MegaDriveEnvironment::LanguagePin::Overseas
                                               : MegaDriveEnvironment::LanguagePin::Japanese);
        env.setVideoStandard(videoHz == "50" ? MegaDriveEnvironment::VideoStandard::Hz50
                                             : MegaDriveEnvironment::VideoStandard::Hz60);
        env.setVDPTurboMultiplier(turboMultiplier);
        env.setWidescreen(video.widescreen);
        env.setIntegerScaling(video.integerScaling);
        if (silentFlag) {
            env.sound().disable();
        } else if (soundMode == "prerendered") {
            const std::filesystem::path directory = soundtrackDirectory();
            // The engine still runs; only the chip output is replaced.
            if (SoRSoundtrack::open(directory)) {
                env.sound().disable();
            } else {
                soundtrackFailed = true;
                std::fprintf(stderr,
                             "No pre-rendered soundtrack in %s (run scripts/render_soundtrack.py); "
                             "emulating the sound chips\n",
                             directory.string().c_str());
            }
        }
    };

    {
        // On the heap: it holds the whole emulation environment, which overflowed a 1 MiB main stack on Windows.
        const auto game = std::make_unique<StreetsOfRage>(sorRomPath,
                                                          video.frameSync,
                                                          VDP::Integer,
                                                          debugUtilsFlag ? static_cast<std::uint16_t>(remoteAccessPort) : 0);
        StreetsOfRage &sor = *game;
        configureEnvironment(sor);
#ifdef __PROSPERO__
        if (soundtrackFailed) { // the manifest is there, but unreadable or the audio output is not
            ps5_notify("Streets of Rage: the pre-rendered soundtrack could not be opened. Render it and copy it again.");
            return 0;
        }
#endif
        // The game reads the keyboard and gamepads itself (controls.yaml);
        // MegaDriveEnvironment keeps only the remote (autoplay) buttons.
        sor.controllers().setPhysicalInputEnabled(false);
        SoRInput::start(SoRControlsConfig::defaultPath());
        // Host mode flags for GET_FLAGS; the indices are mirrored by autoplay/src/sor_autoplay/host_flags.py.
        // (Flag 1 was --altControls; each player's scheme is now in the game data, SoRRemoteInput.)
        constexpr std::size_t kFlagWidescreen = 0;
        sor.remoteAccess().setFlag(kFlagWidescreen, video.widescreen);
        sor.setDebugLog(sorDebugFlag);
        sor.setDebugUtilities(debugUtilsFlag);
        sor.setStartFullscreen(video.fullscreen);
        if (!sorAuxAddrFile.empty()) {
            sor.setAuxAddrFile(sorAuxAddrFile);
        }
        if (!sorCallLogFile.empty()) {
            sor.setCallLog(sorCallLogFile);
        }
        SoRPython::configure(aiPath, video.widescreen, turboMultiplier);
        SoRPython::setVideo(video.widescreen, video.frameSync == VDP::InternalTimer);
        if (aiAssistFlag) {
            SoRRemoteInput::forceAiAssist();
            SoRPython::start(sor.memory());
        }
        sor.vdp().setFrameOverlay([&sor](Framebuffer &framebuffer, int width, int height) {
            SoRPause::drawOverlay(sor.memory(), framebuffer, width, height);
        });
        sor.boot();
        SoRPython::stop();
        SoRInput::stop();
        SoRSoundtrack::close();
    }

    return 0;
}
