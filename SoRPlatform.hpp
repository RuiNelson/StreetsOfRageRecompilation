#pragma once

// What differs on a console, which has one fullscreen output and is played with gamepads.
namespace SoRPlatform {

// The console's ROM, soundtrack, Python and settings: a title's own folder is read-only.
inline constexpr const char *kDataDir = "/data/sor";

// On the PS5 always; on the desktop with --console-menus, to test those screens without one.
inline bool &consoleMenusFlag() {
#ifdef __PROSPERO__
    static bool enabled = true;
#else
    static bool enabled = false;
#endif
    return enabled;
}

// The menus leave out what a console has no use for: BIND KEYBOARD, and fullscreen or window.
inline bool consoleMenus() {
    return consoleMenusFlag();
}

} // namespace SoRPlatform
