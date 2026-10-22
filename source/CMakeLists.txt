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
# aiwrite_copy_python：把 Python 侧守护进程包拷到输出目录（M7B 批 2 · §6.2 · 风险 P6）
#   * 源 = <仓库根>/source/python/brain_ai_browser（**排除 `.venv`** —— 那是本机环境、不入产物）
#   * 目标 = <exe>/python/brain_ai_browser（`pydoll_channel.cpp` 的包目录候选之一）
#   * `file(GLOB CONFIGURE_DEPENDS)`：新增 / 删除 `.py` 时自动重配（避免漏拷）
# -----------------------------------------------------------------------------
function(aiwrite_copy_python target)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/python/brain_ai_browser")
    set(_dst "$<TARGET_FILE_DIR:${target}>/python/brain_ai_browser")
    file(GLOB _py_files CONFIGURE_DEPENDS "${_src}/*.py")
    if(NOT _py_files)
        message(WARNING "copy_python: 未找到 ${_src}/*.py（跳过）—— 开发态请确认 source/python 就位")
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dst}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_py_files} "${_dst}"
        COMMENT "copy_python: brain_ai_browser/*.py -> $<TARGET_FILE_DIR:${target}>/python"
        VERBATIM)
endfunction()

# -----------------------------------------------------------------------------
# aiwrite_core：公共基础设施（路径 / 日志 / 配置 / 加密）
# 后续 M2 起把 engine / nodes / ai 也并入本库
# -----------------------------------------------------------------------------
add_library(aiwrite_core STATIC
    utils/src/string_util.cpp  # CLN-05：字符串通用工具单一来源（findCi/trim/nowString/toLower/narrow）
    utils/src/paths.cpp
    utils/src/log.cpp
    utils/src/config.cpp
    utils/src/window_geometry.cpp
    utils/src/credential.cpp
    utils/src/text_export.cpp
    utils/src/crypto.cpp
    utils/src/output_archive.cpp
    utils/src/diagnostics.cpp
    utils/src/image_decode.cpp     # M7-04：图片格式嗅探 + stb/WIC 解码（texture_cache 与自检共用）
    utils/src/asset_store.cpp      # P7a-04：统一资源目录（内容寻址归档 + 令牌解析）
    utils/src/diagnostics_report.cpp  # M6（PM-04）：诊断报告（只读采集；api_probe --diag-assert）
    utils/src/settings_validation.cpp # M6（M6-01）：设置面板校验纯函数（api_probe --settings-assert）
    utils/src/webview2_runtime.cpp    # M6（PM-01）：WebView2 运行时依赖探测（首启引导 + 诊断）
    engine/src/graph.cpp
    engine/src/node_registry.cpp
    engine/src/undo_stack.cpp
    engine/src/workflow_io.cpp
    engine/src/validate.cpp
    engine/src/failure_summary.cpp     # DEBT-L8（M6-04）：运行结束失败汇总（纯函数）
    engine/src/provider_resolve.cpp
    engine/src/executor.cpp
    engine/src/composite.cpp        # CN：上级节点（模型/注册表/独立文件存储/展开执行）
    engine/src/recent_files.cpp
    engine/src/autosave.cpp          # M_patchAB §4（PC-01/02）：自动保存 + 崩溃恢复（api_probe --autosave-assert）
    engine/src/migration.cpp         # M_patchAB §4（PC-04）：版本迁移框架（workflow + config；未来版本明确拒绝）
    ai/src/web_pow.cpp
    ai/src/cancel_token.cpp          # M_patchAB §2.1（PB-02）：协作式取消 + 取消监视（纯逻辑 + 线程）
    ai/src/retry_policy.cpp          # M_patchAB §2.6（PB-08 残项）：可重试状态 / 退避 / 次数钳制
    ai/src/deepseek/deepseek_web_client.cpp
    ai/src/dom_web_client.cpp        # L3（PB2-13/15）：通用 DOM 站点适配器 + 选择器探测
    ai/src/upload_supply_probe.cpp  # M8 Phase 2：C++ 供料探针（WebView2 会话供料 + 站点只读 GET）
    ai/src/upload/image_convert.cpp        # M8-14：图片准备（站点白名单 / 限额 → 缩放 + 转码）
    ai/src/upload/image_preprocess.cpp     # M_8nodes_extension L1-c（§4.2）：图片预处理（用户参数 → 裁剪/缩放）
    ai/src/upload/upload_evidence.cpp      # M8-14：上传双证据（页面 + 网络；I18）纯函数
    ai/src/upload/upload_registry.cpp      # M8-14：站点上传注册表 + 唯一执行入口 upload_images
    ai/src/doubao/site_upload_doubao.cpp # M8-14：豆包站点上传单元（B1/B2 实测）
    ai/src/kimi/site_upload_kimi.cpp   # M8-14：Kimi 站点上传单元（模板性验证站）
    ai/src/deepseek/deepseek_official_provider.cpp
    ai/src/provider_spec.cpp          # M_patchB L1：Provider 配置表（API + 网页版同表）
    ai/src/http_json.cpp              # L2（B2-b）：通用 JSON POST（取消 / 超时 / 错误归因三协议共用）
    ai/src/http_net.cpp               # M_patchAB §2.3（PB-06 剩余）：代理 / 自签证书策略（纯逻辑 + 应用点）
    ai/src/inference_provider.cpp     # L2（PB2-08/12）：统一推理接口 + 能力门控 + openai/网页版实现
    ai/src/anthropic_provider.cpp     # L2（PB2-10）：Anthropic Messages API 适配器
    ai/src/gemini_provider.cpp        # L2（PB2-11）：Gemini generateContent 适配器
    ai/src/provider_factory.cpp       # L2（PB2-09）：按表 protocol 分派（未知 ⇒ 明确报错，R9）
    ai/src/site_template.cpp          # M_patchAB §3.5（PB2-24）：自建网页版站点模板 + 落盘
    ai/src/official_template.cpp      # 自定义 AI 收口：自建官方 API 提供商模板 + 落盘
    nodes/src/local_nodes.cpp
    nodes/src/register_executors.cpp
    nodes/src/text_split.cpp          # M_node_expansion L2（§4.3.5.1）：文本分块
    nodes/src/text_extract.cpp        # M_node_expansion L2（§4.3.5.2）：文本提取
    nodes/src/text_clean.cpp          # M_node_expansion L2（§4.3.5.3）：文本清理
    nodes/src/text_replace.cpp        # M_node_expansion L2（§4.3.5.4）：文本替换
    nodes/src/text_stats.cpp          # M_node_expansion L2（§4.3.5.5）：文本统计
    nodes/src/text_diff.cpp           # M_node_expansion L2（§4.3.5.6）：文本对比
    web/src/channel_frames.cpp   # M7B 批 1：管道协议 v1 帧（纯函数；VB2-29 / VB2-32）
    web/src/pipe_client.cpp      # M7B 批 1 step 2：命名管道客户端（M7B-02；读线程 + id 配对）
    web/src/pydoll_channel.cpp   # M7B 批 2：新通道（Pydoll 守护进程）—— 本批**不接线**（只 --pydoll-login / --pydoll-selftest）
    web/src/session_snapshot.cpp # M7B 批 2：L2 快照只读视图（MB-D0-8；**只碰元数据**，不读内容）
    web/src/session_store.cpp
    web/src/probe_verdict.cpp    # M_patchAB §2.2/§3.3（PB-07 / PB2-25）：会话诊断纯函数（判据收紧 + 错误串反查）
    web/src/webview_host.cpp)    # 网页版登录 + 会话 + 协议探测 + 页面内 PoW 求解
# 头文件根 = **每个模块自己的 `include/`**（模块自带走 src/ + include/）—— 见 AI_README.md §11「代码组织规范」
#  * 实现（.cpp）在 source/<模块>/src/**；头文件在 source/<模块>/include/**（模块内**拍平**，不再镜像子目录）
#  * 引用写法 = **裸文件名**（例：#include "graph.h"）⇒ 每个模块的 include/ 都要挂成头文件根，故逐个列出
#  * AI 专属实现仍按站点分目录：ai/src/deepseek/**、ai/src/doubao/**、ai/src/kimi/**
#  * 新增模块 = 建 source/<模块>/{src,include} + 下面加一行 + 上方源文件列表加 .cpp（三步都要做）
target_include_directories(aiwrite_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/ai/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/nodes/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/utils/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/include")
# 源码目录宏（PS-D9 内置上级节点源：paths::builtin_composites_dir() 需解析 <source>/workflows/compositeNodes）
target_compile_definitions(aiwrite_core PRIVATE AIWRITE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aiwrite_core
    PUBLIC aiwrite_build_options
    PUBLIC spdlog::spdlog
    PUBLIC nlohmann_json::nlohmann_json
    PUBLIC aiwrite_tomlplusplus
    PUBLIC OpenSSL::Crypto
    PUBLIC httplib::httplib   # ai/：网页版 PoW + /api/v0/chat/completion（M4-06）
    ${AIWRITE_WEBVIEW2}       # web/：内嵌 WebView2 登录窗口 + 页面内 PoW 求解（M4-06/M4-08）
    PRIVATE psapi version)   # 诊断模块：GetProcessMemoryInfo ｜ webview2_runtime：GetFileVersionInfoW/VerQueryValueW
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
#
# ⚠ ODR 红线（见 third_party/imgui-node-editor/AIWRITE_PATCHES.md）：
#   本库自带 external/imgui（1.84 WIP），与 vcpkg 的 1.90.7 同名结构体布局不同。
#   绝不可把 third_party/imgui-node-editor/external 加入任何 target 的
#   include 路径——否则链接器静默选错实现，运行期才以内存错乱暴露。
#   imgui_node_editor.h 内已有 IMGUI_VERSION_NUM==19070 静态断言兜底。
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
    app/src/main.cpp
    app/src/cli_selftest.cpp  # CLN-14 拆分（CLI 自检域：run_selftest / 各 selftest）
    app/src/cli_diag.cpp      # CLN-14 拆分（CLI 诊断域：帮助 / pipe / 会话清单 / 表导出）
    ui/src/app.cpp
    ui/src/console_panel.cpp
    ui/src/property_panel.cpp
    ui/src/panel_widgets.cpp  # CLN-12 拆分（参数控件域）
    ui/src/panel_web.cpp      # CLN-12 拆分（网页会话 / 提供商域）
    ui/src/editor_state.cpp
    ui/src/run_session.cpp    # ARCH-R1 拆分（运行会话：Executor + 快照从 editor() 拆出）
    ui/src/node_canvas.cpp
    ui/src/canvas_view.cpp
    ui/src/node_library.cpp
    ui/src/toolbar.cpp
    utils/src/file_dialog.cpp
    ui/src/output_panel.cpp
    ui/src/error_bar.cpp
    ui/src/text_view.cpp
    ui/src/texture_cache.cpp
    ui/src/settings_panel.cpp
    ui/src/composite_editor.cpp  # CN：上级节点编辑器（CLN-13 拆分：画布/上下文/改名域）
    ui/src/ce_state.cpp          # CLN-13 拆分（会话状态 + 撤销域）
    ui/src/ce_session.cpp        # CLN-13 拆分（会话操作域）
    ui/src/ce_run.cpp            # CLN-13 拆分（试运行「执行连线」域）
    ui/src/ce_pack.cpp           # CLN-13 拆分（打包选区为上级节点域）
    ui/src/ce_draw.cpp           # CLN-13 拆分（编辑器窗口 + 新建/打包对话框域）
    ui/src/commands.cpp)        # ARCH-R4 拆分（命令路由层：AppCommand + 快捷键表 + dispatchCommand）
target_sources(aiwrite PRIVATE
    ui/src/event_bus.cpp       # ARCH-E1（事件总线：EventBus + EditEvent 落地）
    ui/src/workflow_model.cpp) # ARCH-E3（文档级工作流模型：Graph + Undo + EventBus 拆出）

# M_patchAB §3.2 / §3.4（PB2-07 / PB2-30②）：面板诊断按钮 + 错误条「重新登录」（PT-07）
# M_8nodes_extension L1-d（§4.3）：PromptTemplate「生成提示词」异步后端
target_sources(aiwrite PRIVATE
    ui/src/connection_test.cpp
    ui/src/prompt_generator.cpp)
target_link_libraries(aiwrite PRIVATE
    aiwrite_core
    imgui_node_editor
    glfw
    OpenGL::GL
    nfd::nfd
    shell32)   # M5-03：ShellExecuteW（在资源管理器中定位图片）
# 图片解码的 stb include 与 WIC 链接已随 utils/src/image_decode.cpp 归到 aiwrite_core（M7-04）
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
aiwrite_copy_python(aiwrite)   # M7B 批 2：brain_ai_browser/*.py → <exe>/python/brain_ai_browser

# -----------------------------------------------------------------------------
# api_probe：M1 技术验证命令行工具（V-04 HTTP / V-05 SHA3 / V-06 DeepSeek API）
# -----------------------------------------------------------------------------
add_executable(api_probe tools/api_probe.cpp)
target_sources(api_probe PRIVATE
    tools/api_probe_selftest.cpp        # CLN-15：协议/网络/CLI 自检段（原匿名块1）
    tools/api_graph_selftest.cpp        # CLN-15：图模型自检段
    tools/api_executor_selftest.cpp     # CLN-15：执行器自检段（P3-1）
    tools/api_execution_selftest.cpp    # CLN-15：执行核心自检段（P3-2）
    tools/api_diag.cpp                  # CLN-15：诊断/断言段
    tools/composite_selftest.cpp)       # CN：上级节点自检（展开执行/规格/注册表/JSON）
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
aiwrite_copy_python(api_probe) # M7B 批 2：同上（自检也要能起新通道守护进程）

# -----------------------------------------------------------------------------
# ce_repro：临时复现「新建上级节点 → 编辑器首帧」ImGui 窗口栈断言（headless）
# 验证完即可移除本目标
# -----------------------------------------------------------------------------
add_executable(ce_repro tools/ce_repro.cpp)
target_link_libraries(ce_repro PRIVATE
    imgui_node_editor
    imgui::imgui)
target_compile_definitions(ce_repro PRIVATE IMGUI_DEFINE_MATH_OPERATORS)

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
