# The PS5 title's toolchain: the payload SDK, RADV and the title converter, at pinned revisions.
# Built and used by scripts/build_ps5.py. Runs natively on amd64 and arm64: everything is a cross build.
FROM ubuntu:26.04@sha256:6a06111c3b0a8600feadb270aec771dc239bf407b25625f93f44ab00e11f4d3c

# PS5_Vulkan's HEAD of 2026-10-13, the day after griches' PS5_DevilutionX, whose title recipe we adapt.
# It pins PS5_Mesa and its own SDK; RADV is built against that one. A title links the newer SDK
# PS5_DevilutionX pins, as that port does: its platform layer has the C-locale localeconv.
ARG PS5_VULKAN_REVISION=f410e67a0241b3e8edcdbcd2a9b285dfc4a412ed
ARG PS5_MESA_REVISION=1ba9874f478657ccf6da72ce55f0260164cc0bd0
ARG RADV_SDK_REVISION=b83202be73e930050e00de0f1b4d9ec46c0391bf
ARG TITLE_SDK_REVISION=611893fc25ef3e138f5bc2c90d829d8b58174cd2

# LLVM 21 is the newest the SDK's prospero-* wrappers accept; its development files and libclc are
# for mesa_clc and vtn_bindgen2, RADV's host tools. bsdextrautils has the rev those wrappers use.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates git curl wget unzip zip xz-utils file make build-essential pkgconf bsdextrautils patch \
        cmake ninja-build meson rsync bison flex glslang-tools spirv-tools \
        python3 python3-mako python3-yaml python3-packaging python3-ply \
        clang-21 lld-21 llvm-21 libclang-rt-21-dev \
        llvm-21-dev libclang-21-dev libclang-cpp21-dev libclc-21 libclc-21-dev libllvmspirvlib-21-dev \
        spirv-tools-dev spirv-headers libexpat1-dev zlib1g-dev \
    && for tool in clang clang++ llvm-ar llvm-ranlib llvm-nm; do ln -s "/usr/bin/$tool-21" "/usr/local/bin/$tool"; done

# A title links libclang_rt.builtins-x86_64.a, which an arm64 Ubuntu's clang does not have.
RUN if [ "$(dpkg --print-architecture)" != amd64 ]; then \
        set -e; \
        version=$(dpkg-query -W -f='${Version}' libclang-rt-21-dev); \
        escaped=$(printf '%s' "$version" | sed 's/:/%3a/; s/+/%2b/g; s/~/%7e/g'); \
        upstream=${version#*:}; \
        source_package=llvm-toolchain-21; \
        cd /tmp; \
        curl -fsSLO "http://archive.ubuntu.com/ubuntu/pool/universe/l/$source_package/libclang-rt-21-dev_${upstream}_amd64.deb" \
            || curl -fsSLO "http://archive.ubuntu.com/ubuntu/pool/main/l/$source_package/libclang-rt-21-dev_${upstream}_amd64.deb"; \
        dpkg-deb -x libclang-rt-21-dev_*_amd64.deb extracted; \
        builtins=$(find extracted -name 'libclang_rt.builtins-x86_64.a' -o -name 'libclang_rt.builtins.a' | head -1); \
        install -D "$builtins" "$(clang-21 --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"; \
        rm -rf /tmp/extracted /tmp/*.deb; \
    fi

ENV PS5_ROOT=/opt/ps5 \
    PS5_CLANG=/usr/bin/clang-21 \
    PS5_VULKAN_DIR=/opt/ps5/PS5_Vulkan \
    PS5_PAYLOAD_SDK=/opt/ps5/title-sdk

# Only the revisions asked for, not the forks' histories.
RUN set -e; \
    fetch() { git init -q "$2"; git -C "$2" remote add origin "https://github.com/$1.git"; shift 2; }; \
    fetch mihawk-99/PS5_Vulkan "$PS5_VULKAN_DIR"; \
    git -C "$PS5_VULKAN_DIR" fetch -q --depth 1 origin "$PS5_VULKAN_REVISION"; \
    git -C "$PS5_VULKAN_DIR" checkout -q FETCH_HEAD; \
    fetch mihawk-99/PS5_Mesa "$PS5_ROOT/PS5_Mesa"; \
    git -C "$PS5_ROOT/PS5_Mesa" fetch -q --depth 1 origin "$PS5_MESA_REVISION"; \
    fetch mihawk-99/PS5_PayloadSDK "$PS5_ROOT/PS5_PayloadSDK"; \
    git -C "$PS5_ROOT/PS5_PayloadSDK" fetch -q --depth 1 origin "$RADV_SDK_REVISION" "$TITLE_SDK_REVISION"

# The SDK RADV builds against, and zlib for the converter.
RUN cd "$PS5_VULKAN_DIR" && bash tools/setup-native-dependencies.sh

# RADV's release archive: no assertions, as titles ship. A title's link reads it from here.
RUN cd "$PS5_VULKAN_DIR" \
    && PS5_MESA_REVISION="$PS5_MESA_REVISION" bash tools/build-radv.sh release \
    || { tail -40 .deps/work/radv-build-ps5-release.log 2>/dev/null; exit 1; }

# The converter: ELF to the console's fake SELF.
RUN set -e; cd "$PS5_VULKAN_DIR"; mkdir -p build/host; \
    native=tooling/native; zlib=.deps/native/zlib/root/usr; \
    clang++ -std=c++20 -O2 -I "$zlib/include" \
        "$native/native_app_builder.cpp" "$native/self_container.cpp" \
        "$native/elf_object.cpp" "$native/sce_module_writer.cpp" \
        "$(find .deps/native/zlib/root -name libz.a -print -quit)" -o build/host/ps5-native-tool

# The clean-room libc.prx a title folder needs, reproduced against its recorded digest.
RUN cd "$PS5_VULKAN_DIR" && bash tools/rebuild-libc.sh

# The SDK a title links.
RUN set -e; tree=$(mktemp -d); \
    git -C "$PS5_ROOT/PS5_PayloadSDK" archive "$TITLE_SDK_REVISION" | tar -x -C "$tree"; \
    bash "$tree/platform/tools/setup-sdk.sh" "$PS5_PAYLOAD_SDK" "$TITLE_SDK_REVISION" "$PS5_ROOT/.sdk-cache"; \
    rm -rf "$tree" "$PS5_ROOT/.sdk-cache"

# Only what a title's build reads stays in the image.
RUN rm -rf "$PS5_VULKAN_DIR/.deps/work" "$PS5_ROOT/PS5_Mesa" "$PS5_ROOT/PS5_PayloadSDK" "$PS5_VULKAN_DIR/.git"

COPY check-toolchain.sh /opt/ps5/check-toolchain.sh
WORKDIR /workspace
