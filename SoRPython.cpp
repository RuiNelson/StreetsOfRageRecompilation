#include "SoRPython.hpp"

#include <cstdio>

#ifdef SOR_EMBED_PYTHON

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <SDL3/SDL.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

namespace {

// Python's own threads on macOS get 16 MiB: the 512 KiB of a secondary thread
// there is not enough for what the interpreter expects to recurse.
constexpr std::size_t kThreadStackBytes = 16u * 1024u * 1024u;

constexpr auto kStopTimeout = std::chrono::seconds(3);

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

// Outlives everything: an AI that never ends is abandoned still using it.
struct State {
    std::filesystem::path   aiPath;
    SDL_Thread             *thread = nullptr;
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    stopRequested = false;
    bool                    finished      = false;
};

State &state() {
    static State *instance = new State;
    return *instance;
}

// -- the sor_host module, what the AI sees of the game

// wait_stop(timeout_seconds) -> True once the game asks the AI to end.
PyObject *hostWaitStop(PyObject * /*module*/, PyObject *argument) {
    const double seconds = PyFloat_AsDouble(argument);
    if (seconds == -1.0 && PyErr_Occurred())
        return nullptr;
    bool stop = false;
    Py_BEGIN_ALLOW_THREADS
    State           &shared = state();
    std::unique_lock lock(shared.mutex);
    shared.changed.wait_for(lock, std::chrono::duration<double>(seconds), [&shared] { return shared.stopRequested; });
    stop = shared.stopRequested;
    Py_END_ALLOW_THREADS
    return PyBool_FromLong(stop);
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
    {"wait_stop", hostWaitStop, METH_O, "Wait up to timeout seconds; True once the game asks the AI to end."},
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
    const std::filesystem::path stdlib =
        home / "lib" / ("python" + std::to_string(PY_MAJOR_VERSION) + "." + std::to_string(PY_MINOR_VERSION));

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
    for (const std::filesystem::path &path : {aiPath, stdlib, stdlib / "lib-dynload"}) {
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

void start(const std::filesystem::path &aiPath) {
    State &shared = state();
    if (shared.thread != nullptr)
        return;
    std::error_code error;
    shared.aiPath = std::filesystem::absolute(aiPath, error);
    if (error || !std::filesystem::is_directory(shared.aiPath, error)) {
        std::fprintf(stderr, "[ai] no Python AI: '%s' is not a folder (see --ai-path)\n", aiPath.string().c_str());
        return;
    }
    const SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetPointerProperty(properties, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER,
                           reinterpret_cast<void *>(interpreterThread));
    SDL_SetStringProperty(properties, SDL_PROP_THREAD_CREATE_NAME_STRING, "sor-python");
    SDL_SetNumberProperty(properties, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, static_cast<Sint64>(kThreadStackBytes));
    shared.thread = SDL_CreateThreadWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (shared.thread == nullptr)
        std::fprintf(stderr, "[ai] cannot start the Python thread: %s\n", SDL_GetError());
}

void stop() {
    State &shared = state();
    if (shared.thread == nullptr)
        return;
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

#else // SOR_EMBED_PYTHON

namespace SoRPython {

void start(const std::filesystem::path & /*aiPath*/) {
    std::fprintf(stderr, "[ai] the Python AI is not available in this build yet\n");
}

void stop() {
}

} // namespace SoRPython

#endif // SOR_EMBED_PYTHON
