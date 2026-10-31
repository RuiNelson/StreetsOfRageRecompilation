#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

class SystemMemory;

// The Python AI (the sor_ai package) in this process, on the CPython that
// cmake/CPythonRuntime.cmake puts next to the executable.
//
// One thread owns the interpreter's whole lifetime: it initializes it, runs
// sor_ai.embedded.run and finalizes it. No other thread ever touches Python or
// waits for the GIL, so however long the AI takes, the game does not slow down.
// The interpreter is isolated: its only search paths are the bundled stdlib and
// the AI's directory, and the system Python and the PYTHON* variables play no
// part. An AI that fails is reported on stderr and the game goes on without it.
namespace SoRPython {

/// Says where the AI's code is. Nothing in it runs yet.
void configure(const std::filesystem::path &aiPath, bool widescreen);

/// Whether the configured folder holds the AI (the mode's menu row depends on it).
bool available();

/// Starts the thread, once; the configured code runs only from this call on.
void start(SystemMemory &memory);

/// CPU thread, once per sampled frame: the game as the AI's next tick sees it.
/// Once kFramesPerTick frames have passed it copies the work RAM; the AI takes the newest
/// copy whenever it is ready, so a slow tick skips frames and delays nothing.
void publishFrame(SystemMemory &memory, std::uint64_t frame, bool player2Enhanced);

/// CPU thread: the input document the AI sent since the last call, if any.
bool takeInput(std::vector<std::uint8_t> &document);

/// Asks the AI to end and waits for the interpreter to finish, for a few
/// seconds: an AI that does not end is abandoned, never waited on forever.
void stop();

} // namespace SoRPython
