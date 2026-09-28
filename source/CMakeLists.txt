# =============================================================================
#  AIwrite —— AI 小说/剧本创作软件（MVP）
#  里程碑 M1：技术验证 + 项目骨架 / M2：节点系统 P1+P2（见 docs/actionPlan/）
#
#  工具链：系统 CMake 4.4.3（生成器 "Visual Studio 18 2026"）+ MSVC 14.50（VS 2026 Insiders）
#          · vcpkg 端口构建使用工程自带的 third_party/cmake-3.31.6（旧 ports 与 CMake 4 不兼容）
#  依赖：vcpkg manifest 模式（vcpkg.json）；依赖安装树在 <AIWRITE_ROOT>/vcpkg-installed
#
#  用法：在 source/ 下执行  .\build.ps1             （自动加载 dev.ps1）
# =============================================================================
#  版本区间写法 <min>...<policy_max>（CMake 3.12+）：
#    * <min>        = 3.25 —— 运行中的 CMake 低于它直接报错；
#    * <policy_max> = 4.6  —— **策略版本**取 4.6（而不是 3.25），于是 3.25 之后引入的策略
#                             全部按 NEW 行为生效，新版 CMake 不会再因「策略未设置」报警；
#                             区间只声明"已适配到哪个版本"，**并不禁止**更高的 CMake 版本。
#    这样即便以后换用 CMake 5.x 也能正常配置（相当于放宽了 CMake 的容忍度）。
cmake_minimum_required(VERSION 3.25...4.6)

#  CMake 4.0 起移除了对「策略版本 < 3.5」的兼容：任何仍写老版本号的 CMake 代码
#  （add_subdirectory 进来的老库 / 手写子工程）都会直接报错：
#      Compatibility with CMake < 3.5 has been removed from CMake.
#  CMake 3.31 起还会对「策略版本 < 3.10」给出 deprecation 警告 —— vcpkg 自带的
#  scripts/buildsystems/vcpkg.cmake 就调用了 cmake_policy(VERSION 3.7.2)，
#  因此这里把策略下限设成 3.10：既让老代码被容忍配置，又消除该 deprecation 警告。
#    （文档依据：策略版本 = max(调用处声明值, CMAKE_POLICY_VERSION_MINIMUM)）
#  命令行也可临时覆盖：cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ...
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM "3.10" CACHE STRING
        "策略版本下限：容忍仍声明老策略版本的第三方 / 端口代码（CMake 4+ 必需）")
    mark_as_advanced(CMAKE_POLICY_VERSION_MINIMUM)
endif()

#  CMake 4.4 实测：vcpkg 自带的 toolchain（C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake）
#  内部调用 cmake_policy(VERSION 3.7.2)（第 40 / 878 行），因此在**每次处理该 toolchain**时
#  都会打印一串看起来像报错的弃用警告（含 cmake 调用栈）：
#      CMake Warning (deprecated) at .../vcpkg.cmake:40 (cmake_policy):
#        Compatibility with CMake < 3.10 will be removed from a future version of CMake.
#  这不是本项目的问题，也不影响配置/构建（Configure 仍正常完成），但会淹没真正的错误。
#  处理方式：
#    * 首选在 **preset** 里关掉（见 CMakePresets.json：CMAKE_WARN_DEPRECATED=OFF +
#      CMAKE_SUPPRESS_DEVELOPER_WARNINGS=ON）——toolchain 在本文件之前执行，
#      只有 preset / 命令行传入的缓存变量才能影响到它；
#    * 本文件里的同样两行只是**兜底**（供不用 preset 的 `cmake -S source -B build` 场景）。
#      注意：CMAKE_SUPPRESS_DEVELOPER_WARNINGS（-Wno-dev）**不要**写进 preset ——
#      它由 CMake 自身消费，作为「手工指定变量」会被 unused-cli 反过来警告一次。
if(NOT DEFINED CMAKE_WARN_DEPRECATED)
    set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL
        "关闭弃用警告：屏蔽 vcpkg toolchain 的老策略版本噪音（CMake 3.31+/4.x）")
endif()
if(NOT DEFINED CMAKE_SUPPRESS_DEVELOPER_WARNINGS)
    set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS ON CACHE BOOL
        "等价 -Wno-dev：屏蔽作者级警告（如 unused-cli）")
endif()

project(aiwrite
    VERSION 0.2.0
    DESCRIPTION "AI 小说/剧本创作软件 —— 节点工作流 + DeepSeek 后端"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# 所有可执行文件输出到 <build>/bin
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach()

message(STATUS "AIwrite: 构建类型 = ${CMAKE_BUILD_TYPE}")
message(STATUS "AIwrite: 源码目录 = ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "AIwrite: 构建目录 = ${CMAKE_BINARY_DIR}")

# -----------------------------------------------------------------------------
# vcpkg 安装树的搜索路径修正
# Windows 上 CMake 的 config 模式不会去搜 <prefix>/share/<pkg>/，而 vcpkg 的
# xxx-config.cmake 全部位于 <installed>/<triplet>/share/<pkg>/ 下，因此这里
# 显式把 share 目录加入搜索路径（vcpkg 工具链只会加 <installed>/<triplet>）。
# -----------------------------------------------------------------------------
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    list(APPEND CMAKE_PREFIX_PATH "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
    message(STATUS "AIwrite: vcpkg 依赖目录 = ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
else()
    message(WARNING "AIwrite: 未检测到 vcpkg 环境，请通过 CMakePresets 配置（见 dev.ps1）")
endif()

# -----------------------------------------------------------------------------
# 依赖（全部来自 vcpkg manifest，见 vcpkg.json）
# -----------------------------------------------------------------------------
find_package(imgui CONFIG REQUIRED)             # imgui::imgui（已含 glfw / opengl3 后端）
find_package(glfw3 CONFIG REQUIRED)             # glfw
find_package(OpenGL REQUIRED)                   # OpenGL::GL
find_package(spdlog CONFIG REQUIRED)            # spdlog::spdlog
find_package(nlohmann_json CONFIG REQUIRED)     # nlohmann_json::nlohmann_json
find_package(httplib CONFIG REQUIRED)           # httplib::httplib
find_package(OpenSSL REQUIRED)                  # OpenSSL::SSL / OpenSSL::Crypto
# toml++ 的 vcpkg 端口只提供库文件与 pkg-config（无 CMake config），这里手工建立导入目标
set(_aiwrite_toml_root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
find_path(AIWRITE_TOMLPLUSPLUS_INCLUDE_DIR NAMES "toml++/toml.h"
    PATHS "${_aiwrite_toml_root}/include" NO_DEFAULT_PATH REQUIRED)
find_library(AIWRITE_TOMLPLUSPLUS_LIB_RELEASE NAMES tomlplusplus
    PATHS "${_aiwrite_toml_root}/lib" NO_DEFAULT_PATH)
find_library(AIWRITE_TOMLPLUSPLUS_LIB_DEBUG NAMES tomlplusplus
    PATHS "${_aiwrite_toml_root}/debug/lib" NO_DEFAULT_PATH)
if(NOT AIWRITE_TOMLPLUSPLUS_LIB_RELEASE AND NOT AIWRITE_TOMLPLUSPLUS_LIB_DEBUG)
    message(FATAL_ERROR "未找到 tomlplusplus 库，请先执行 source/install-deps.cmd")
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND AIWRITE_TOMLPLUSPLUS_LIB_DEBUG)
    set(_aiwrite_toml_lib "${AIWRITE_TOMLPLUSPLUS_LIB_DEBUG}")
else()
    set(_aiwrite_toml_lib "${AIWRITE_TOMLPLUSPLUS_LIB_RELEASE}")
endif()
add_library(aiwrite_tomlplusplus UNKNOWN IMPORTED)
set_target_properties(aiwrite_tomlplusplus PROPERTIES
    IMPORTED_LOCATION "${_aiwrite_toml_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${AIWRITE_TOMLPLUSPLUS_INCLUDE_DIR}")
message(STATUS "AIwrite: toml++ = ${_aiwrite_toml_lib}")

# stb（图片解码，M5-03/M5-08）：vcpkg 端口只装头文件（无 CMake config），同样手工取 include 目录
find_path(AIWRITE_STB_INCLUDE_DIR NAMES "stb_image.h"
    PATHS "${_aiwrite_toml_root}/include" NO_DEFAULT_PATH REQUIRED)
message(STATUS "AIwrite: stb = ${AIWRITE_STB_INCLUDE_DIR}")

find_package(unofficial-webview2 CONFIG REQUIRED)
find_package(nfd CONFIG REQUIRED)               # nativefiledialog-extended（overlay port）

if(TARGET unofficial::webview2::webview2)
    set(AIWRITE_WEBVIEW2 unofficial::webview2::webview2)
elseif(TARGET unofficial::webview2::WebView2)
    set(AIWRITE_WEBVIEW2 unofficial::webview2::WebView2)
elseif(TARGET WebView2::WebView2)
    set(AIWRITE_WEBVIEW2 WebView2::WebView2)
elseif(TARGET webview2::webview2)
    set(AIWRITE_WEBVIEW2 webview2::webview2)
else()
    message(FATAL_ERROR "未找到 WebView2 导入目标（unofficial-webview2）")
endif()
message(STATUS "AIwrite: WebView2 目标 = ${AIWRITE_WEBVIEW2}")

# -----------------------------------------------------------------------------
# 通用编译选项
# -----------------------------------------------------------------------------
add_library(aiwrite_build_options INTERFACE)
target_compile_definitions(aiwrite_build_options INTERFACE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE)
if(MSVC)
    # /utf-8：源码为 UTF-8 无 BOM（含中文字面量）
    target_compile_options(aiwrite_build_options INTERFACE /utf-8 /permissive- /Zc:__cplusplus)
endif()

# 把 exe 依赖的 vcpkg 动态库拷到 exe 同级目录，便于直接运行
function(aiwrite_copy_runtime_dlls target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                "-DDEST=$<TARGET_FILE_DIR:${target}>"
                "-DDLLS=$<TARGET_RUNTIME_DLLS:${target}>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        COMMENT "拷贝运行时依赖 DLL -> $<TARGET_FILE_DIR:${target}>"
        VERBATIM)
endfunction()

# 兜底：把 vcpkg 安装树 bin 下的所有 DLL 同步到 exe 同级目录
# （M1 开发期确保 exe 可独立运行；M6 打包时会改成精确收集）
function(aiwrite_sync_vcpkg_bin target)
    if(NOT DEFINED VCPKG_INSTALLED_DIR OR NOT DEFINED VCPKG_TARGET_TRIPLET)
        return()
    endif()
    set(_vcpkg_bin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    if(NOT EXISTS "${_vcpkg_bin}")
        return()
    endif()
    file(GLOB _vcpkg_dlls "${_vcpkg_bin}/*.dll")
    if(NOT _vcpkg_dlls)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                "-DDEST=$<TARGET_FILE_DIR:${target}>"
                "-DSYNC_DIR=${_vcpkg_bin}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        COMMENT "同步 vcpkg 运行时 DLL -> $<TARGET_FILE_DIR:${target}>"
        VERBATIM)
endfunction()

# -----------------------------------------------------------------------------
# aiwrite_copy_assets：把随程序发布的配置表（assets/providers.json 等）拷到输出目录
#   M_patchB L1 / PB2-02：配置表的第 ① 层来源 = <exe>/assets/providers.json
# -----------------------------------------------------------------------------
function(aiwrite_copy_assets target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${target}>/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/providers.json"
                "$<TARGET_FILE_DIR:${target}>/assets/providers.json"
        COMMENT "copy_assets: providers.json -> $<TARGET_FILE_DIR:${target}>/assets"
        VERBATIM)
endfunction()

# -----------------------------------------------------------------------------
# aiwrite_core：公共基础设施（路径 / 日志 / 配置 / 加密）
# 后续 M2 起把 engine / nodes / ai 也并入本库
# -----------------------------------------------------------------------------
add_library(aiwrite_core STATIC
    src/utils/paths.cpp
    src/utils/log.cpp
    src/utils/config.cpp
    src/utils/window_geometry.cpp
    src/utils/credential.cpp
    src/utils/text_export.cpp
    src/utils/crypto.cpp
    src/utils/output_archive.cpp
    src/utils/diagnostics.cpp
    src/utils/image_decode.cpp     # M7-04：图片格式嗅探 + stb/WIC 解码（texture_cache 与自检共用）
    src/engine/graph.cpp
    src/engine/node_registry.cpp
    src/engine/undo_stack.cpp
    src/engine/workflow_io.cpp
    src/engine/validate.cpp
src/engine/provider_resolve.cpp
    src/engine/executor.cpp
    src/engine/recent_files.cpp
    src/ai/web_pow.cpp
    src/ai/deepseek_web_client.cpp
    src/ai/dom_web_client.cpp        # L3（PB2-13/15）：通用 DOM 站点适配器 + 选择器探测
    src/ai/deepseek_official_provider.cpp
    src/ai/provider_spec.cpp          # M_patchB L1：Provider 配置表（API + 网页版同表）
    src/nodes/local_nodes.cpp
    src/nodes/register_executors.cpp
    src/web/session_store.cpp
    src/web/webview_host.cpp)   # 网页版登录 + 会话 + 协议探测 + 页面内 PoW 求解
target_include_directories(aiwrite_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(aiwrite_core
    PUBLIC aiwrite_build_options
    PUBLIC spdlog::spdlog
    PUBLIC nlohmann_json::nlohmann_json
    PUBLIC aiwrite_tomlplusplus
    PUBLIC OpenSSL::Crypto
    PUBLIC httplib::httplib   # ai/：网页版 PoW + /api/v0/chat/completion（M4-06）
    ${AIWRITE_WEBVIEW2}       # web/：内嵌 WebView2 登录窗口 + 页面内 PoW 求解（M4-06/M4-08）
    PRIVATE psapi)   # 诊断模块：GetProcessMemoryInfo
# M7-04 图片解码：stb_image 只有头文件（端口无 CMake config）→ 手工挂 include；
#   Windows 系统解码器 WIC（WebP/TIFF/ICO/HEIF 等）+ COM 初始化（ole32）
target_include_directories(aiwrite_core PRIVATE "${AIWRITE_STB_INCLUDE_DIR}")
target_link_libraries(aiwrite_core PRIVATE windowscodecs ole32 uuid)
if(MSVC)
    target_compile_options(aiwrite_core PRIVATE /W4)
endif()

# -----------------------------------------------------------------------------
# imgui-node-editor：第三方源码集成（固定 tag v0.9.3）
# 与 vcpkg 的 imgui 1.90.7-docking 配对
# -----------------------------------------------------------------------------
add_library(imgui_node_editor STATIC
    third_party/imgui-node-editor/imgui_node_editor.cpp
    third_party/imgui-node-editor/imgui_node_editor_api.cpp
    third_party/imgui-node-editor/imgui_canvas.cpp
    third_party/imgui-node-editor/crude_json.cpp)
target_include_directories(imgui_node_editor
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui-node-editor")
target_link_libraries(imgui_node_editor PUBLIC imgui::imgui aiwrite_build_options)
target_compile_definitions(imgui_node_editor PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
if(MSVC)
    # 第三方代码：关闭警告
    target_compile_options(imgui_node_editor PRIVATE /W0)
endif()

# -----------------------------------------------------------------------------
# aiwrite：主程序（GUI，Dear ImGui + GLFW + OpenGL3）
# -----------------------------------------------------------------------------
add_executable(aiwrite WIN32
    src/main.cpp
    src/ui/app.cpp
    src/ui/console_panel.cpp
    src/ui/property_panel.cpp
    src/ui/editor_state.cpp
    src/ui/node_canvas.cpp
    src/ui/node_library.cpp
    src/ui/toolbar.cpp
    src/utils/file_dialog.cpp
    src/ui/output_panel.cpp
    src/ui/error_bar.cpp
    src/ui/text_view.cpp
    src/ui/texture_cache.cpp)
target_link_libraries(aiwrite PRIVATE
    aiwrite_core
    imgui_node_editor
    glfw
    OpenGL::GL
    nfd::nfd
    shell32)   # M5-03：ShellExecuteW（在资源管理器中定位图片）
# 图片解码的 stb include 与 WIC 链接已随 src/utils/image_decode.cpp 归到 aiwrite_core（M7-04）
# 源码目录（自检用：默认示例图片路径可复现）
target_compile_definitions(aiwrite PRIVATE AIWRITE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(aiwrite PRIVATE /W4)
    # GUI 程序但保留 main() 入口（便于命令行参数解析）
    target_link_options(aiwrite PRIVATE /ENTRY:mainCRTStartup /SUBSYSTEM:WINDOWS)
endif()
aiwrite_copy_runtime_dlls(aiwrite)
aiwrite_sync_vcpkg_bin(aiwrite)
aiwrite_copy_assets(aiwrite)   # M_patchB L1 / PB2-02：providers.json → <exe>/assets

# -----------------------------------------------------------------------------
# api_probe：M1 技术验证命令行工具（V-04 HTTP / V-05 SHA3 / V-06 DeepSeek API）
# -----------------------------------------------------------------------------
add_executable(api_probe tools/api_probe.cpp)
target_link_libraries(api_probe PRIVATE
    aiwrite_core
    httplib::httplib
    OpenSSL::SSL)
if(MSVC)
    target_compile_options(api_probe PRIVATE /W4)
endif()
# 源码目录（自检用：加载仓库内的示例工作流，路径可复现）
target_compile_definitions(api_probe PRIVATE AIWRITE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
aiwrite_copy_runtime_dlls(api_probe)
aiwrite_sync_vcpkg_bin(api_probe)
aiwrite_copy_assets(api_probe) # 自检也要读表（与 aiwrite 同一份资产）

# -----------------------------------------------------------------------------
# webview2_login：M1 技术验证命令行工具（V-03 WebView2 登录 + Cookie 提取）
# -----------------------------------------------------------------------------
add_executable(webview2_login tools/webview2_login.cpp)
target_link_libraries(webview2_login PRIVATE
    aiwrite_core
    ${AIWRITE_WEBVIEW2})
if(MSVC)
    target_compile_options(webview2_login PRIVATE /W4)
endif()
aiwrite_copy_runtime_dlls(webview2_login)
aiwrite_sync_vcpkg_bin(webview2_login)

message(STATUS "AIwrite: 目标 = aiwrite (GUI) / api_probe (CLI) / webview2_login (CLI)")
