#include "SorRuntime.hpp"
#include "config/controls/ControlsConfigUI.hpp"
#include <CLI/CLI.hpp>
#include <cstdint>
#include <string>

int main(int argc, char *argv[]) {
    CLI::App app{"MegaDrive Environment"};
    app.set_version_flag("-V,--version", "0.1.0");

    bool        configControlsFlag    = false;
    bool        runSorFlag            = false;
    bool        sorDebugFlag          = false;
    bool        debugUtilsFlag        = false;
    bool        fullScreenFlag        = false;
    bool        silentFlag            = false;
    int         sorVSyncMode          = 0; // 0 = internal timer (default); 1/2/3 = VSync/VSync2/VSync3
    int         remoteAccessPort      = 6969;
    std::uint32_t turboMultiplier     = 0;
    std::string sorRomPath            = "rom/SOR.bin";
    std::string sorAuxAddrFile; // if set, record unknown dispatch targets here
    std::string languagePin = "jp";
    std::string videoHz     = "60";

    app.add_flag("--silent", silentFlag, "Disable audio output entirely (chip writes are dropped)");
    app.add_flag("--configControls", configControlsFlag, "Open controller configuration UI");
    app.add_option("--lang", languagePin, "Console language pin: jp=low/Japanese, en=high/overseas")
        ->capture_default_str()
        ->check(CLI::IsMember({"jp", "en"}));
    app.add_option("--hz", videoHz, "Console video frequency pin: 60=low/NTSC, 50=high/PAL")
        ->capture_default_str()
        ->check(CLI::IsMember({"60", "50"}));
    app.add_flag("--runSor", runSorFlag, "Run the recompiled Streets of Rage cartridge");
    app.add_flag("--debug", sorDebugFlag, "With --runSor: log CPU/VDP state once per second");
    app.add_flag("--debugUtils",
                 debugUtilsFlag,
                 "With --runSor: enable debug hotkeys, cheats and remote access");
    app.add_flag("--fullScreen", fullScreenFlag, "With --runSor: start in fullscreen");
    app.add_option("--vsync",
                   sorVSyncMode,
                   "With --runSor: frame sync — 0=internal timer from --hz (default), "
                   "1=VSync, 2=VSync2 (½ rate), 3=VSync3 (⅓ rate)")
        ->capture_default_str()
        ->check(CLI::Range(0, 3));
    app.add_option("--rom", sorRomPath, "ROM path for --runSor")->capture_default_str();
    app.add_option("--port",
                   remoteAccessPort,
                   "With --runSor --debugUtils: remote access TCP port (0 disables)")
        ->capture_default_str()
        ->check(CLI::Range(0, 65535));
    app.add_option("--turbo",
                   turboMultiplier,
                   "With --runSor and --vsync 0: run the internal VDP at 60 Hz times this multiplier")
        ->check(CLI::Range(1u, 4'294'967'295u));
    app.add_option("--auxAddrFile",
                   sorAuxAddrFile,
                   "With --runSor: on an indirect dispatch to an unknown address, append it to "
                   "this aux file and exit (42) instead of aborting — for the discovery loop");

    CLI11_PARSE(app, argc, argv);

    auto configureEnvironment = [&](MegaDriveEnvironment &env) {
        env.setLanguagePin(languagePin == "en" ? MegaDriveEnvironment::LanguagePin::Overseas
                                               : MegaDriveEnvironment::LanguagePin::Japanese);
        env.setVideoStandard(videoHz == "50" ? MegaDriveEnvironment::VideoStandard::Hz50
                                             : MegaDriveEnvironment::VideoStandard::Hz60);
        env.setVDPTurboMultiplier(turboMultiplier);
        if (silentFlag)
            env.sound().disable();
    };

    if (configControlsFlag) {
        runControlsConfig();
    }
    if (runSorFlag) {
        SorRuntime sor(sorRomPath,
                       static_cast<VDP::Synchronization>(sorVSyncMode),
                       VDP::Integer,
                       debugUtilsFlag ? static_cast<std::uint16_t>(remoteAccessPort) : 0);
        configureEnvironment(sor);
        sor.setDebugLog(sorDebugFlag);
        sor.setDebugUtilities(debugUtilsFlag);
        sor.setStartFullscreen(fullScreenFlag);
        if (!sorAuxAddrFile.empty()) {
            sor.setAuxAddrFile(sorAuxAddrFile);
        }
        sor.boot();
    }

    return 0;
}
