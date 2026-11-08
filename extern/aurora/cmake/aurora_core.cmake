add_library(aurora_core STATIC
        lib/aurora.cpp
        lib/device.cpp
        lib/device.hpp
        lib/input.cpp
        lib/io.cpp
        lib/io.hpp
        lib/logging.cpp
        lib/system_info.cpp
        lib/system_info.hpp
        lib/thread.cpp
        lib/thread.hpp
        lib/time.cpp
        lib/time_internal.hpp
        lib/window.cpp
)
add_library(aurora::core ALIAS aurora_core)
set_target_properties(aurora_core PROPERTIES FOLDER "aurora")

target_compile_definitions(aurora_core PUBLIC AURORA TARGET_PC)
target_include_directories(aurora_core PUBLIC include)
target_link_libraries(aurora_core PUBLIC fmt::fmt ${AURORA_SDL3_TARGET} xxHash::xxhash)
target_link_libraries(aurora_core PRIVATE absl::btree absl::flat_hash_map sqlite3 Tracy::TracyClient)
if (AURORA_ENABLE_GX AND AURORA_CACHE_USE_ZSTD)
    target_compile_definitions(aurora_core PRIVATE AURORA_CACHE_USE_ZSTD)
    target_link_libraries(aurora_core PRIVATE zstd::libzstd)
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL Windows)
    # stuff for fetching system info.
    target_link_libraries(aurora_core PRIVATE wbemuuid.lib comsuppw.lib ntdll.lib DXGI.lib)
elseif (APPLE)
    target_sources(aurora_core PRIVATE lib/system_info_mac.mm)
endif ()

# visionOS too: SDL defines SDL_PLATFORM_IOS there, so device.cpp leaves rumble to this file.
if (IOS OR CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    find_library(COREHAPTICS_FRAMEWORK CoreHaptics REQUIRED)
    target_sources(aurora_core PRIVATE lib/device_ios.mm)
    set_source_files_properties(lib/device_ios.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)
    target_link_libraries(aurora_core PUBLIC ${COREHAPTICS_FRAMEWORK})
endif ()

if (AURORA_ENABLE_GX)
    target_sources(aurora_core PRIVATE lib/imgui.cpp)
    target_link_libraries(aurora_core PUBLIC imgui)
endif ()

if(AURORA_ENABLE_RMLUI)
    target_compile_definitions(aurora_core PUBLIC AURORA_ENABLE_RMLUI)

    target_sources(aurora_core PRIVATE
            lib/rmlui.cpp
            lib/rmlui/RuntimeTextureProvider.cpp
            lib/rmlui/RmlUi_Backend_Aurora.cpp
            lib/rmlui/WebGPURenderInterface.cpp
            lib/rmlui/SystemInterface_Aurora.cpp
            lib/rmlui/FileInterface_SDL.cpp
            lib/rmlui/GlassFilter.cpp
    )
    target_link_libraries(aurora_core PUBLIC rmlui)

    target_link_libraries(aurora_core PUBLIC rmlui_backends)
endif ()

if (AURORA_ENABLE_GX)
    target_compile_definitions(aurora_core PUBLIC AURORA_ENABLE_GX WEBGPU_DAWN)
    target_sources(aurora_core PRIVATE
            lib/webgpu/gpu.cpp
            lib/webgpu/gpu_cache.cpp
            lib/webgpu/gpu_prof.cpp
            lib/webgpu/memory_budget.cpp
            lib/dawn/BackendBinding.cpp
            lib/dawn/TracyPlatform.cpp
            lib/gfx/stereo_eyes.cpp
            lib/gfx/stereo_foveation.cpp
            lib/gfx/stereo_multiview.cpp
            lib/gfx/stereo_shadow.cpp
            lib/stereo_host.cpp
            lib/stereo_overlay.cpp
    )
    if (CMAKE_SYSTEM_NAME STREQUAL Windows)
        # The OpenXR D3D12 stereo bridge (zero-readback eye copies on Dawn's queue).
        target_sources(aurora_core PRIVATE lib/webgpu/d3d12_interop.cpp)
        target_link_libraries(aurora_core PRIVATE d3d12 dxgi)
    endif ()
    # The OpenXR Vulkan stereo bridge for the Quest (eyes shared through
    # AHardwareBuffers, ordered by sync fds). It compiles to stubs elsewhere.
    target_sources(aurora_core PRIVATE lib/webgpu/vulkan_interop.cpp)
    # Direct presentation with PrimedGun's patched Dawn (the eyes copied straight into the
    # runtime's swapchain images on Dawn's queue): the Quest, and PC VR on Vulkan where the
    # embedding project names its Vulkan headers target in AURORA_VULKAN_DIRECT_HEADERS (on
    # Windows the patched entry points are looked up in webgpu_dawn.dll at run time, so a stock
    # Dawn still starts). Stubs elsewhere.
    target_sources(aurora_core PRIVATE lib/webgpu/vulkan_direct_interop.cpp)
    if (AURORA_VULKAN_DIRECT_HEADERS AND NOT ANDROID)
        target_link_libraries(aurora_core PRIVATE ${AURORA_VULKAN_DIRECT_HEADERS})
        target_compile_definitions(aurora_core PRIVATE AURORA_VULKAN_DIRECT_DESKTOP=1)
    endif ()
    # Fragment density maps for foveated eye rendering, through the same patched Dawn. Stubs elsewhere.
    target_sources(aurora_core PRIVATE lib/webgpu/fdm.cpp)
    # The Apple Vision Pro stereo bridge (eyes shared with the visionOS OpenXR provider through
    # IOSurfaces, ordered by MTLSharedEvents; aurora/metal_interop.h). Stubs elsewhere.
    target_sources(aurora_core PRIVATE lib/webgpu/metal_interop.cpp)
    if (ANDROID)
        target_link_libraries(aurora_core PRIVATE android)
    endif ()
    if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
        set_source_files_properties(lib/dawn/TracyPlatform.cpp PROPERTIES COMPILE_FLAGS -fno-rtti)
    endif ()
    target_link_libraries(aurora_core PRIVATE dawn::webgpu_dawn)
    if (DAWN_ENABLE_VULKAN)
        target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_VULKAN)
    endif ()
    if (DAWN_ENABLE_METAL)
        target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_METAL)
        target_sources(aurora_core PRIVATE lib/dawn/MetalBinding.mm)
        set_source_files_properties(lib/dawn/MetalBinding.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)
        target_link_options(aurora_core PUBLIC "LINKER:-weak_framework,Metal")
        # metal_interop.cpp retains IOSurfaces and MTLSharedEvents through CoreFoundation.
        target_link_libraries(aurora_core PUBLIC "-framework IOSurface" "-framework CoreFoundation"
                                                 "-framework QuartzCore")
    endif ()
    if (APPLE AND CMAKE_SYSTEM_NAME STREQUAL "visionOS")
        # MetalBinding.mm makes its own CAMetalLayer there (no UIKit window exists).
        target_compile_definitions(aurora_core PRIVATE AURORA_VISIONOS=1)
    endif ()
    if (DAWN_ENABLE_D3D11)
        target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_D3D11)
    endif ()
    if (DAWN_ENABLE_D3D12)
        target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_D3D12)
    endif ()
    if (DAWN_ENABLE_DESKTOP_GL OR DAWN_ENABLE_OPENGLES)
        target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_OPENGL)
        if (DAWN_ENABLE_DESKTOP_GL)
            target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_DESKTOP_GL)
        endif ()
        if (DAWN_ENABLE_OPENGLES)
            target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_OPENGLES)
        endif ()
    endif ()
    if (DAWN_ENABLE_NULL)
        target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_NULL)
    endif ()
endif ()
