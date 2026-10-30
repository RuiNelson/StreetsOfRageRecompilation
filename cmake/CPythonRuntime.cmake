# Our own CPython: the shared library and stdlib `sor` embeds to run the Python
# AI (SoRPython.cpp), the same on every machine and independent of any Python
# installed there. Built once from the pinned source into .cpython/ (ignored by
# Git and outside build/, so --clean and --release do not rebuild it: it takes
# minutes) and copied next to `sor` by sor_bundle_cpython().
#
# Defines the imported target SoR::CPython.

set(SOR_CPYTHON_VERSION 3.15.2)
set(SOR_CPYTHON_SHA256 8b5536d280142ecf6cdfe15d12a21ba1c65e300bbf7b28a8d0cdb849575797de)
string(REGEX MATCH "^[0-9]+\\.[0-9]+" SOR_CPYTHON_ABI "${SOR_CPYTHON_VERSION}")

set(SOR_CPYTHON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.cpython/${SOR_CPYTHON_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
set(SOR_CPYTHON_PREFIX "${SOR_CPYTHON_ROOT}/install")
set(SOR_CPYTHON_STAMP "${SOR_CPYTHON_ROOT}/install.complete")
set(SOR_CPYTHON_STDLIB "${SOR_CPYTHON_PREFIX}/lib/python${SOR_CPYTHON_ABI}")
if(APPLE)
    set(SOR_CPYTHON_LIBRARY "${SOR_CPYTHON_PREFIX}/lib/libpython${SOR_CPYTHON_ABI}.dylib")
else()
    set(SOR_CPYTHON_LIBRARY "${SOR_CPYTHON_PREFIX}/lib/libpython${SOR_CPYTHON_ABI}.so.1.0")
endif()

# A step of the CPython build; any failure stops the configure.
function(sor_cpython_run working_directory)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env --unset=PYTHONPATH --unset=PYTHONHOME ${ARGN}
        WORKING_DIRECTORY "${working_directory}"
        OUTPUT_FILE "${SOR_CPYTHON_ROOT}/build.log"
        ERROR_FILE "${SOR_CPYTHON_ROOT}/build.log"
        RESULT_VARIABLE result
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "CPython build failed at '${ARGN}': see ${SOR_CPYTHON_ROOT}/build.log")
    endif()
endfunction()

if(NOT EXISTS "${SOR_CPYTHON_STAMP}")
    message(STATUS "Building CPython ${SOR_CPYTHON_VERSION} into .cpython/ (first time only, takes some minutes)")
    file(REMOVE_RECURSE "${SOR_CPYTHON_ROOT}")
    file(MAKE_DIRECTORY "${SOR_CPYTHON_ROOT}")

    set(archive "${SOR_CPYTHON_ROOT}/Python-${SOR_CPYTHON_VERSION}.tar.xz")
    file(DOWNLOAD
        "https://www.python.org/ftp/python/${SOR_CPYTHON_VERSION}/Python-${SOR_CPYTHON_VERSION}.tar.xz"
        "${archive}"
        EXPECTED_HASH SHA256=${SOR_CPYTHON_SHA256}
        STATUS download_status
    )
    list(GET download_status 0 download_code)
    if(NOT download_code EQUAL 0)
        message(FATAL_ERROR "Could not download CPython ${SOR_CPYTHON_VERSION}: ${download_status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${SOR_CPYTHON_ROOT}")
    set(source "${SOR_CPYTHON_ROOT}/Python-${SOR_CPYTHON_VERSION}")

    # Nothing the AI uses that would tie the runtime to libraries of this
    # machine (Homebrew's OpenSSL, say). Without _decimal, `decimal` falls back
    # to its Python version.
    set(disabled_modules
        _bz2 _ctypes _curses _curses_panel _dbm _decimal _gdbm _hashlib _lzma _remote_debugging _sqlite3 _ssl _tkinter
        _uuid _zstd readline)
    # PGO: 7% off the AI's mean tick and 6% off its p99 (tools/tick_bench.py), for
    # about 40 s more once. Its training run needs the test package, removed below.
    set(configure_options --prefix=${SOR_CPYTHON_PREFIX} --enable-shared --enable-optimizations --without-ensurepip
        --without-static-libpython)
    foreach(module IN LISTS disabled_modules)
        list(APPEND configure_options py_cv_module_${module}=n/a)
    endforeach()

    cmake_host_system_information(RESULT jobs QUERY NUMBER_OF_LOGICAL_CORES)
    sor_cpython_run("${source}" ./configure ${configure_options})
    sor_cpython_run("${source}" make -j${jobs})
    sor_cpython_run("${source}" make altinstall)

    # What the game never loads.
    foreach(directory idlelib test tkinter turtledemo ensurepip venv pydoc_data)
        file(REMOVE_RECURSE "${SOR_CPYTHON_STDLIB}/${directory}")
    endforeach()
    file(GLOB test_modules "${SOR_CPYTHON_STDLIB}/lib-dynload/*test*")
    if(test_modules)
        file(REMOVE ${test_modules})
    endif()
    file(GLOB_RECURSE optimized_bytecode "${SOR_CPYTHON_STDLIB}/*.opt-1.pyc" "${SOR_CPYTHON_STDLIB}/*.opt-2.pyc")
    if(optimized_bytecode)
        file(REMOVE ${optimized_bytecode})
    endif()
    file(GLOB static_config "${SOR_CPYTHON_STDLIB}/config-*")
    if(static_config)
        file(REMOVE_RECURSE ${static_config})
    endif()
    if(APPLE)
        sor_cpython_run("${SOR_CPYTHON_PREFIX}"
            install_name_tool -id "@rpath/libpython${SOR_CPYTHON_ABI}.dylib" "${SOR_CPYTHON_LIBRARY}")
    endif()

    file(REMOVE_RECURSE "${source}")
    file(REMOVE "${archive}")
    file(WRITE "${SOR_CPYTHON_STAMP}" "${SOR_CPYTHON_VERSION}\n")
endif()

add_library(SoR::CPython SHARED IMPORTED)
set_target_properties(SoR::CPython PROPERTIES
    IMPORTED_LOCATION "${SOR_CPYTHON_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SOR_CPYTHON_PREFIX}/include/python${SOR_CPYTHON_ABI}"
)

# Puts the library next to @p target and the stdlib in python/ beside it, where
# SoRPython.cpp looks for them.
function(sor_bundle_cpython target)
    target_link_libraries(${target} PRIVATE SoR::CPython)
    if(APPLE)
        set_property(TARGET ${target} PROPERTY BUILD_RPATH "@loader_path")
    else()
        set_property(TARGET ${target} PROPERTY BUILD_RPATH "$ORIGIN")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SOR_CPYTHON_LIBRARY}" "$<TARGET_FILE_DIR:${target}>"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${SOR_CPYTHON_STDLIB}"
            "$<TARGET_FILE_DIR:${target}>/python/lib/python${SOR_CPYTHON_ABI}"
        COMMENT "Copying CPython next to ${target}"
    )
endfunction()
