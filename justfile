web_build_dir := "build/web"
native_build_dir := "build/native"

# Build the native (X11) binary and launch it directly (default).
default: run

# Configure and build the wasm target via emscripten.
build-web:
    emcmake cmake -S . -B {{web_build_dir}} -DCMAKE_BUILD_TYPE=Release
    cmake --build {{web_build_dir}} -j

# Configure and build a native desktop binary.
build-native:
    cmake -S . -B {{native_build_dir}} -DCMAKE_BUILD_TYPE=Release
    cmake --build {{native_build_dir}} -j

# Build and launch the native (X11) binary directly -- no wasm/Emscripten,
# WebKitGTK, or deno anywhere in the loop, for isolating whether a given
# issue is specific to the wasm+webview path (`just run-wasm`) or present
# natively too.
run: build-native
    ./{{native_build_dir}}/mep

# Build the wasm target and open it in a native window via deno + webview.
run-wasm: build-web
    LD_LIBRARY_PATH="${MEP_WEBVIEW_LD_LIBRARY_PATH:-}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" deno task launch

# Remove all build output.
clean:
    rm -rf build
