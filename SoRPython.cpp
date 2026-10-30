#include "SoRPython.hpp"

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <SDL3/SDL.h>

#include "system/memory/SystemMemory.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

namespace {

// Python's own threads on macOS get 16 MiB: the 512 KiB of a secondary thread
// there is not enough for what the interpreter expects to recurse.
constexpr std::size_t kThreadStackBytes = 16u * 1024u * 1024u;

constexpr auto kStopTimeout = std::chrono::seconds(3);

// One AI tick per two game frames, as `autoplay --lockstep` and its 33 ms poll
// at normal speed. In game frames, so it follows --turbo by itself.
constexpr std::uint64_t kFramesPerTick = 2;

constexpr m_long      kWorkRamBase = 0xFF0000u;
constexpr std::size_t kWorkRamSize = 0x10000u;

// An owned reference, so that no path here has to decrement one by hand.
class PyRef {
public:
    explicit PyRef(PyObject *object = nullptr) : object_(object) {}
    ~PyRef() { Py_XDECREF(object_); }
    PyRef(const PyRef &)            = delete;
    PyRef &operator=(const PyRef &) = delete;

    [[nodiscard]] PyObject *get() const { return object_; }
    explicit                operator bool() const { return object_ != nullptr; }

private:
    PyObject *object_;
};

struct FrameImage {
    std::uint64_t                     frame           = 0;
    bool                              player2Enhanced = false;
    std::array<m_byte, kWorkRamSize> *ram             = new std::array<m_byte, kWorkRamSize>;
};

// Outlives everything: an AI that never ends is abandoned still using it.
struct State {
    std::filesystem::path   aiPath;
    SystemMemory           *memory     = nullptr;
    bool                    widescreen = false;
    SDL_Thread             *thread     = nullptr;
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    stopRequested = false;
    bool                    finished      = false;

    // The CPU thread fills `filling` with no lock and swaps it with `latest`;
    // the AI swaps `latest` with `reading`. The lock only covers the swaps, so
    // neither thread ever holds the other up for longer than that.
    FrameImage filling;
    FrameImage latest;
    FrameImage reading;
    bool       latestIsNew = false;

    std::atomic<std::uint64_t> currentFrame{0};
    std::atomic<bool>          running{false};

    std::vector<std::uint8_t> input; // the AI's newest document, under the lock
    bool                      inputIsNew = false;
};

State &state() {
    static State *instance = new State;
    return *instance;
}

// -- the sor_host module, what the AI sees of the game

// stop_requested() -> True once the game asks the AI to end.
PyObject *hostStopRequested(PyObject * /*module*/, PyObject * /*unused*/) {
    State                &shared = state();
    const std::lock_guard lock(shared.mutex);
    return PyBool_FromLong(shared.stopRequested);
}

// wait_frame(timeout_seconds) -> (frame, work RAM bytes, P2 on ENHANCED 6): the
// newest image the AI has not had yet, or None after the timeout or on a stop.
PyObject *hostWaitFrame(PyObject * /*module*/, PyObject *argument) {
    const double seconds = PyFloat_AsDouble(argument);
    if (seconds == -1.0 && PyErr_Occurred())
        return nullptr;
    State &shared = state();
    bool   ready  = false;
    Py_BEGIN_ALLOW_THREADS
    std::unique_lock lock(shared.mutex);
    shared.changed.wait_for(lock, std::chrono::duration<double>(seconds), [&shared] {
        return shared.latestIsNew || shared.stopRequested;
    });
    if (shared.latestIsNew && !shared.stopRequested) {
        std::swap(shared.latest, shared.reading);
        shared.latestIsNew = false;
        ready              = true;
    }
    Py_END_ALLOW_THREADS
    if (!ready)
        Py_RETURN_NONE;
    return Py_BuildValue("(Ky#O)",
                         static_cast<unsigned long long>(shared.reading.frame),
                         reinterpret_cast<const char *>(shared.reading.ram->data()),
                         static_cast<Py_ssize_t>(kWorkRamSize),
                         shared.reading.player2Enhanced ? Py_True : Py_False);
}

// current_frame() -> the game's uptime in frames, now.
PyObject *hostCurrentFrame(PyObject * /*module*/, PyObject * /*unused*/) {
    return PyLong_FromUnsignedLongLong(state().currentFrame.load(std::memory_order_acquire));
}

// read_memory(address, length) -> bytes, from the live bus.
PyObject *hostReadMemory(PyObject * /*module*/, PyObject *arguments) {
    unsigned long address = 0;
    int           length  = 0;
    if (!PyArg_ParseTuple(arguments, "ki", &address, &length))
        return nullptr;
    if (length < 0) {
        PyErr_SetString(PyExc_ValueError, "negative length");
        return nullptr;
    }
    const PyRef bytes(PyBytes_FromStringAndSize(nullptr, length));
    if (!bytes)
        return nullptr;
    state().memory->copyToBuffer(static_cast<m_long>(address), PyBytes_AS_STRING(bytes.get()), length);
    return Py_NewRef(bytes.get());
}

// set_input(document): an input document (SoRRemoteInput.hpp's layout) for the game.
PyObject *hostSetInput(PyObject * /*module*/, PyObject *argument) {
    char      *data   = nullptr;
    Py_ssize_t length = 0;
    if (PyBytes_AsStringAndSize(argument, &data, &length) == -1)
        return nullptr;
    State                &shared = state();
    const std::lock_guard lock(shared.mutex);
    shared.input.assign(reinterpret_cast<const std::uint8_t *>(data),
                        reinterpret_cast<const std::uint8_t *>(data) + length);
    shared.inputIsNew = true;
    Py_RETURN_NONE;
}

// widescreen() -> whether the game runs in --widescreen.
PyObject *hostWidescreen(PyObject * /*module*/, PyObject * /*unused*/) {
    return PyBool_FromLong(state().widescreen);
}

// log(text): a line on the game's stderr.
PyObject *hostLog(PyObject * /*module*/, PyObject *argument) {
    const char *text = PyUnicode_AsUTF8(argument);
    if (text == nullptr)
        return nullptr;
    std::fprintf(stderr, "[ai] %s\n", text);
    Py_RETURN_NONE;
}

PyMethodDef kHostMethods[] = {
    {"stop_requested", hostStopRequested, METH_NOARGS, "True once the game asks the AI to end."},
    {"wait_frame", hostWaitFrame, METH_O, "The newest (frame, work RAM, P2 enhanced) not yet had, or None."},
    {"current_frame", hostCurrentFrame, METH_NOARGS, "The game's uptime in frames, now."},
    {"read_memory", hostReadMemory, METH_VARARGS, "Bytes from the live bus: read_memory(address, length)."},
    {"set_input", hostSetInput, METH_O, "An input document for the game."},
    {"widescreen", hostWidescreen, METH_NOARGS, "Whether the game runs in --widescreen."},
    {"log", hostLog, METH_O, "Write a line on the game's stderr."},
    {nullptr, nullptr, 0, nullptr},
};

PyModuleDef kHostModule = {
    PyModuleDef_HEAD_INIT, "sor_host", "The game, as the embedded AI sees it.", -1, kHostMethods,
};

PyObject *initHostModule() {
    return PyModule_Create(&kHostModule);
}

// -- the interpreter

// A Python failure must not reach the game: PyErr_Print would end the process
// on a SystemExit.
void reportPythonError() {
    if (PyErr_ExceptionMatches(PyExc_SystemExit)) {
        PyErr_Clear();
        std::fprintf(stderr, "[ai] the Python AI asked to exit; the game goes on without it\n");
        return;
    }
    PyErr_Print();
    std::fprintf(stderr, "[ai] the Python AI failed; the game goes on without it\n");
}

bool initializeInterpreter(const std::filesystem::path &aiPath) {
    const char *basePath = SDL_GetBasePath();
    if (basePath == nullptr) {
        std::fprintf(stderr, "[ai] cannot find the executable's folder: %s\n", SDL_GetError());
        return false;
    }
    const std::filesystem::path home = std::filesystem::path(basePath) / "python";
#ifdef _WIN32
    const std::filesystem::path stdlib     = home / "Lib";
    const std::filesystem::path extensions = home / "DLLs";
#else
    const std::filesystem::path stdlib =
        home / "lib" / ("python" + std::to_string(PY_MAJOR_VERSION) + "." + std::to_string(PY_MINOR_VERSION));
    const std::filesystem::path extensions = stdlib / "lib-dynload";
#endif

    if (PyImport_AppendInittab("sor_host", initHostModule) == -1) {
        std::fprintf(stderr, "[ai] cannot register the sor_host module\n");
        return false;
    }

    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.site_import              = 0;
    config.module_search_paths_set  = 1;
    const auto succeeded            = [&config](PyStatus status) {
        if (!PyStatus_Exception(status))
            return true;
        std::fprintf(stderr, "[ai] cannot start Python: %s\n", status.err_msg != nullptr ? status.err_msg : "unknown");
        PyConfig_Clear(&config);
        return false;
    };
    if (!succeeded(PyConfig_SetString(&config, &config.home, home.wstring().c_str())))
        return false;
    for (const std::filesystem::path &path : {aiPath, stdlib, extensions}) {
        if (!succeeded(PyWideStringList_Append(&config.module_search_paths, path.wstring().c_str())))
            return false;
    }
    if (!succeeded(Py_InitializeFromConfig(&config)))
        return false;
    PyConfig_Clear(&config);
    return true;
}

void runEmbeddedAi() {
    const PyRef host(PyImport_ImportModule("sor_host"));
    const PyRef embedded(host ? PyImport_ImportModule("sor_ai.embedded") : nullptr);
    const PyRef run(embedded ? PyObject_GetAttrString(embedded.get(), "run") : nullptr);
    const PyRef result(run ? PyObject_CallOneArg(run.get(), host.get()) : nullptr);
    if (!result)
        reportPythonError();
}

int interpreterThread(void * /*unused*/) {
    State &shared = state();
    if (initializeInterpreter(shared.aiPath)) {
        runEmbeddedAi();
        if (Py_FinalizeEx() < 0)
            std::fprintf(stderr, "[ai] Python did not shut down cleanly\n");
    }
    {
        const std::lock_guard lock(shared.mutex);
        shared.finished = true;
    }
    shared.changed.notify_all();
    return 0;
}

} // namespace

namespace SoRPython {

void configure(const std::filesystem::path &aiPath, bool widescreen) {
    State &shared = state();
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(aiPath, error);
    shared.aiPath     = error ? aiPath : absolute;
    shared.widescreen = widescreen;
    if (!available())
        std::fprintf(stderr, "[ai] no Python AI in '%s' (see --ai-path): 1 PLAYER AI ASSIST is hidden\n",
                     shared.aiPath.string().c_str());
}

bool available() {
    std::error_code error;
    return std::filesystem::is_regular_file(state().aiPath / "sor_ai" / "embedded.py", error);
}

void start(SystemMemory &memory) {
    State &shared = state();
    if (shared.thread != nullptr || !available())
        return;
    shared.memory = &memory;
    const SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetPointerProperty(properties, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER,
                           reinterpret_cast<void *>(interpreterThread));
    SDL_SetStringProperty(properties, SDL_PROP_THREAD_CREATE_NAME_STRING, "sor-python");
    SDL_SetNumberProperty(properties, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, static_cast<Sint64>(kThreadStackBytes));
    shared.thread = SDL_CreateThreadWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (shared.thread == nullptr) {
        std::fprintf(stderr, "[ai] cannot start the Python thread: %s\n", SDL_GetError());
        return;
    }
    shared.running.store(true, std::memory_order_release);
}

void publishFrame(SystemMemory &memory, std::uint64_t frame, bool player2Enhanced) {
    State &shared = state();
    if (!shared.running.load(std::memory_order_acquire))
        return;
    shared.currentFrame.store(frame, std::memory_order_release);
    if (frame % kFramesPerTick != 0)
        return;
    memory.copyToBuffer(kWorkRamBase, shared.filling.ram->data(), static_cast<int>(kWorkRamSize));
    shared.filling.frame           = frame;
    shared.filling.player2Enhanced = player2Enhanced;
    {
        const std::lock_guard lock(shared.mutex);
        std::swap(shared.filling, shared.latest);
        shared.latestIsNew = true;
    }
    shared.changed.notify_all();
}

bool takeInput(std::vector<std::uint8_t> &document) {
    State                &shared = state();
    const std::lock_guard lock(shared.mutex);
    if (!shared.inputIsNew)
        return false;
    document.swap(shared.input);
    shared.inputIsNew = false;
    return true;
}

void stop() {
    State &shared = state();
    if (shared.thread == nullptr)
        return;
    shared.running.store(false, std::memory_order_release);
    bool finished = false;
    {
        std::unique_lock lock(shared.mutex);
        shared.stopRequested = true;
        shared.changed.notify_all();
        finished = shared.changed.wait_for(lock, kStopTimeout, [&shared] { return shared.finished; });
    }
    if (finished) {
        SDL_WaitThread(shared.thread, nullptr);
    } else {
        std::fprintf(stderr, "[ai] the Python AI did not end in time; abandoning it\n");
        SDL_DetachThread(shared.thread);
    }
    shared.thread = nullptr;
}

} // namespace SoRPython
