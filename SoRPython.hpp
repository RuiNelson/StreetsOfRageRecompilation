#pragma once

#include <filesystem>

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

/// Starts the thread, once; the code in @p aiPath runs only from this call on.
void start(const std::filesystem::path &aiPath);

/// Asks the AI to end and waits for the interpreter to finish, for a few
/// seconds: an AI that does not end is abandoned, never waited on forever.
void stop();

} // namespace SoRPython
