# Our own CPython: the shared library and stdlib `sor` embeds to run the Python
# AI (SoRPython.cpp), the same on every machine and independent of any Python
# installed there. Kept in .cpython/ (ignored by Git and outside build/, so --clean
# and --release do not redo it) and copied next to `sor` by sor_bundle_cpython().
# macOS and Linux build it once from the pinned source, which takes minutes;
# CPython does not support building with llvm-mingw, so Windows takes the official
# build of the same version from NuGet, which also has the headers and import library.
#
# Defines the imported target SoR::CPython.

set(SOR_CPYTHON_VERSION 3.15.2)
set(SOR_CPYTHON_SHA256 8b5536d280142ecf6cdfe15d12a21ba1c65e300bbf7b28a8d0cdb849575797de)
string(REGEX MATCH "^[0-9]+\\.[0-9]+" SOR_CPYTHON_ABI "${SOR_CPYTHON_VERSION}")

set(SOR_CPYTHON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.cpython/${SOR_CPYTHON_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
set(SOR_CPYTHON_STAMP "${SOR_CPYTHON_ROOT}/install.complete")
if(WIN32)
    string(REPLACE "." "" SOR_CPYTHON_DLL_ABI "${SOR_CPYTHON_ABI}")
    set(SOR_CPYTHON_PREFIX "${SOR_CPYTHON_ROOT}/tools")
    set(SOR_CPYTHON_STDLIB "${SOR_CPYTHON_PREFIX}/Lib")
    set(SOR_CPYTHON_INCLUDE "${SOR_CPYTHON_PREFIX}/include")
    set(SOR_CPYTHON_LIBRARY "${SOR_CPYTHON_PREFIX}/python${SOR_CPYTHON_DLL_ABI}.dll")
    set(SOR_CPYTHON_IMPORT_LIBRARY "${SOR_CPYTHON_PREFIX}/libs/python${SOR_CPYTHON_DLL_ABI}.lib")
else()
    set(SOR_CPYTHON_PREFIX "${SOR_CPYTHON_ROOT}/install")
    set(SOR_CPYTHON_STDLIB "${SOR_CPYTHON_PREFIX}/lib/python${SOR_CPYTHON_ABI}")
    set(SOR_CPYTHON_INCLUDE "${SOR_CPYTHON_PREFIX}/include/python${SOR_CPYTHON_ABI}")
    if(APPLE)
        set(SOR_CPYTHON_LIBRARY "${SOR_CPYTHON_PREFIX}/lib/libpython${SOR_CPYTHON_ABI}.dylib")
    else()
        set(SOR_CPYTHON_LIBRARY "${SOR_CPYTHON_PREFIX}/lib/libpython${SOR_CPYTHON_ABI}.so.1.0")
    endif()
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

if(WIN32 AND NOT EXISTS "${SOR_CPYTHON_STAMP}")
    message(STATUS "Downloading CPython ${SOR_CPYTHON_VERSION} into .cpython/ (first time only)")
    file(REMOVE_RECURSE "${SOR_CPYTHON_ROOT}")
    file(MAKE_DIRECTORY "${SOR_CPYTHON_ROOT}")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(package pythonarm64)
        set(package_sha256 aeb58f2bb6ba9a4cefacae6c23f720133f82bbdc91c8c9dc9a467d483a1a3534)
    else()
        set(package python)
        set(package_sha256 294f5ed7ab97148fcbb57b8c06ba428f6d282c2bbaa266dbe3137284497998f0)
    endif()
    set(archive "${SOR_CPYTHON_ROOT}/${package}.zip")
    file(DOWNLOAD
        "https://api.nuget.org/v3-flatcontainer/${package}/${SOR_CPYTHON_VERSION}/${package}.${SOR_CPYTHON_VERSION}.nupkg"
        "${archive}"
        EXPECTED_HASH SHA256=${package_sha256}
        STATUS download_status
    )
    list(GET download_status 0 download_code)
    if(NOT download_code EQUAL 0)
        message(FATAL_ERROR "Could not download CPython ${SOR_CPYTHON_VERSION}: ${download_status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${SOR_CPYTHON_ROOT}")
    foreach(directory ensurepip venv pydoc_data)
        file(REMOVE_RECURSE "${SOR_CPYTHON_STDLIB}/${directory}")
    endforeach()
    file(REMOVE "${archive}")
    file(WRITE "${SOR_CPYTHON_STAMP}" "${SOR_CPYTHON_VERSION}\n")
elseif(NOT EXISTS "${SOR_CPYTHON_STAMP}")
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
    # about 30 s more once. Its training run needs the test package, removed below.
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
    INTERFACE_INCLUDE_DIRECTORIES "${SOR_CPYTHON_INCLUDE}"
)
if(WIN32)
    set_target_properties(SoR::CPython PROPERTIES IMPORTED_IMPLIB "${SOR_CPYTHON_IMPORT_LIBRARY}")
endif()

# Puts the library next to @p target and the stdlib in python/ beside it, where
# SoRPython.cpp looks for them.
function(sor_bundle_cpython target)
    target_link_libraries(${target} PRIVATE SoR::CPython)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SOR_CPYTHON_LIBRARY}"
                "${SOR_CPYTHON_PREFIX}/python3.dll"
                "${SOR_CPYTHON_PREFIX}/vcruntime140.dll"
                "${SOR_CPYTHON_PREFIX}/vcruntime140_1.dll"
                "$<TARGET_FILE_DIR:${target}>"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${SOR_CPYTHON_STDLIB}" "$<TARGET_FILE_DIR:${target}>/python/Lib"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${SOR_CPYTHON_PREFIX}/DLLs" "$<TARGET_FILE_DIR:${target}>/python/DLLs"
            COMMENT "Copying CPython next to ${target}"
        )
        return()
    endif()
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
