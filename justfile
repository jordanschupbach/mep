web_build_dir := "build/web"
native_build_dir := "build/native"

# Build to wasm and launch it in a window (default).
default: run

# Configure and build the wasm target via emscripten.
build-web:
    emcmake cmake -S . -B {{web_build_dir}} -DCMAKE_BUILD_TYPE=Release
    cmake --build {{web_build_dir}} -j

# Configure and build a native desktop binary (not used by `just run`).
build-native:
    cmake -S . -B {{native_build_dir}} -DCMAKE_BUILD_TYPE=Release
    cmake --build {{native_build_dir}} -j

# Build the wasm target and open it in a native window via deno + webview.
run: build-web
    LD_LIBRARY_PATH="${MEP_WEBVIEW_LD_LIBRARY_PATH:-}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" deno task launch

# Remove all build output.
clean:
    rm -rf build
