@echo off
rem ============================================================================
rem  install-deps.cmd —— 一次性预装 vcpkg 依赖（全部写到 F: 盘，避免占用 C 盘）
rem
rem  说明：
rem    * vcpkg 工具复用 C:\dev\vcpkg（2024-04-23 版）
rem    * --x-install-root / --x-buildtrees-root / --x-packages-root / --downloads-root
rem      全部重定向到 F:\GameDao\Tools\AIwrite\ 下
rem    * 之后 CMake 配置时命中二进制缓存，不会在 C:\dev\vcpkg 下重新编译
rem ============================================================================
setlocal
for %%I in ("%~dp0..") do set "AIWRITE_ROOT=%%~fI"
pushd "%~dp0"

set "VCPKG_ROOT=C:\dev\vcpkg"
set "VCPKG_DOWNLOADS=%AIWRITE_ROOT%\vcpkg-cache\downloads"
set "VCPKG_DEFAULT_BINARY_CACHE=%AIWRITE_ROOT%\vcpkg-cache\binary"
set "VCPKG_DEFAULT_TRIPLET=x64-windows"

rem ---------------------------------------------------------------------------
rem  CMake 版本策略（避免 "Compatibility with CMake < 3.5 has been removed"）：
rem
rem  主保险：端口构建固定用工程自带的 CMake 3.31.6。
rem    旧 ports 的源码里仍有 cmake_minimum_required(VERSION 2.8/3.1/3.4…)（例：glfw3 3.4、
rem    nlohmann-json 3.1、imgui 示例 2.8），CMake 4.x 会直接拒绝配置它们；
rem    系统安装的 CMake 4.x 只用于构建本项目（本项目已用 3.25...4.6 区间写法 + 策略下限）。
rem
rem  如果将来必须用 CMake 4.x 构建端口，可在 triplet 文件里加：
rem    set(VCPKG_CMAKE_CONFIGURE_OPTIONS "-DCMAKE_POLICY_VERSION_MINIMUM=3.5")
rem  （注意：triplet/该变量参与 ABI 哈希，改动会触发依赖重编；vcpkg 端口构建默认
rem    不传递父进程环境变量，因此用环境变量设置该值无效）
rem ---------------------------------------------------------------------------
set "PATH=%AIWRITE_ROOT%\third_party\cmake-3.31.6\bin;%PATH%"

echo [install-deps] AIWRITE_ROOT=%AIWRITE_ROOT%
echo [install-deps] 端口构建使用的 CMake（应为工程自带的 3.31.6）:
where cmake 2>nul | findstr /n "." | findstr "^1:"
cmake --version | findstr /r "^cmake version"

if not exist "%AIWRITE_ROOT%\third_party\cmake-3.31.6\bin\cmake.exe" (
  echo [install-deps] 警告: 未找到 third_party\cmake-3.31.6
  echo [install-deps]       若系统 CMake 为 4.0 或更高，老端口可能报错，
  echo [install-deps]       解决办法见上面的注释（triplet 注入 CMAKE_POLICY_VERSION_MINIMUM）。
)

echo [install-deps] 导入 MSVC 环境...
call "D:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 echo [install-deps] 警告: vcvars64 失败，继续尝试

echo [install-deps] 开始安装 manifest 依赖 (x64-windows) ...
"%VCPKG_ROOT%\vcpkg.exe" install ^
  --triplet x64-windows ^
  --x-install-root="%AIWRITE_ROOT%\vcpkg-installed" ^
  --x-buildtrees-root="%AIWRITE_ROOT%\vcpkg-cache\buildtrees" ^
  --x-packages-root="%AIWRITE_ROOT%\vcpkg-cache\packages" ^
  --downloads-root="%VCPKG_DOWNLOADS%"

set "RC=%ERRORLEVEL%"
popd
echo [install-deps] exit=%RC%
exit /b %RC%
