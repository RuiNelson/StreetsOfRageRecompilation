#include "SoRPython.hpp"

// Until CPython is built for the console: no AI, so the mode menu hides AI ASSIST.
namespace SoRPython {

void configure(const std::filesystem::path & /*aiPath*/, bool /*widescreen*/, std::uint32_t /*turbo*/) {}

void setVideo(bool /*widescreen*/, bool /*timerFrameSync*/) {}

bool available() {
    return false;
}

void start(SystemMemory & /*memory*/) {}

void serveFrame(SystemMemory & /*memory*/, std::uint64_t /*frame*/, bool /*player2Enhanced*/) {}

bool takeInput(std::vector<std::uint8_t> & /*document*/) {
    return false;
}

void stop() {}

} // namespace SoRPython
