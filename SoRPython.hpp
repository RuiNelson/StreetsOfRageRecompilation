#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

class SystemMemory;

// The Python AI (the sor_ai package) in this process, on the CPython that
// cmake/CPythonRuntime.cmake puts next to the executable.
//
// One thread owns the interpreter's whole lifetime: it initializes it, runs
// sor_ai.embedded.run and finalizes it. It keeps its own time: on its own timer
// it asks for the memory, waits for it, ticks and sleeps, as autoplay does over
// its socket. It lives from choosing the mode until the mode menu is back. No
// other thread ever touches Python or waits for the GIL, so however long the AI
// takes, the game does not slow down.
// The interpreter is isolated: its only search paths are the bundled stdlib and
// the AI's directory, and the system Python and the PYTHON* variables play no
// part. An AI that fails is reported on stderr and the game goes on without it.
namespace SoRPython {

/// Says where the AI's code is. Nothing in it runs yet.
void configure(const std::filesystem::path &aiPath, bool widescreen, std::uint32_t turbo);

/// Whether the configured folder holds the AI (the mode's menu row depends on it).
bool available();

/// Starts the thread unless it is running; the configured code runs only from
/// this call on. A fresh interpreter each time.
void start(SystemMemory &memory);

/// CPU thread, once per sampled frame: when the AI has asked for the memory,
/// copies the work RAM and answers. Otherwise it costs one atomic read.
void serveFrame(SystemMemory &memory, std::uint64_t frame, bool player2Enhanced);

/// CPU thread: the input document the AI sent since the last call, if any.
bool takeInput(std::vector<std::uint8_t> &document);

/// Asks the AI to end and waits for the interpreter to finish, for a few
/// seconds: an AI that does not end is abandoned, never waited on forever.
void stop();

} // namespace SoRPython
