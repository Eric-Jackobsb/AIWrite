@echo off
rem ============================================================================
rem  install-deps.cmd —— 一次性预装 vcpkg 依赖（缓存/安装树写在仓库根目录，避免污染 C:\dev\vcpkg）
rem
rem  说明：
rem    * vcpkg 工具复用 C:\dev\vcpkg（本地 clone；依赖版本由 vcpkg.json 的 builtin-baseline 决定）
rem    * --x-install-root / --x-buildtrees-root / --x-packages-root / --downloads-root
rem      全部重定向到 %AIWRITE_ROOT%\ 下（vcpkg-cache\ / vcpkg-installed\）
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
rem  目录预建：全新 clone 时这些目录并不存在，而 vcpkg 会校验
rem  VCPKG_DEFAULT_BINARY_CACHE 必须是已存在的目录（否则直接报错退出）。
rem  dev.ps1 会建同样这几个目录，但 install-deps.cmd 是独立入口，必须自建。
rem ---------------------------------------------------------------------------
if not exist "%VCPKG_DOWNLOADS%" mkdir "%VCPKG_DOWNLOADS%"
if not exist "%VCPKG_DEFAULT_BINARY_CACHE%" mkdir "%VCPKG_DEFAULT_BINARY_CACHE%"
if not exist "%AIWRITE_ROOT%\vcpkg-cache\buildtrees" mkdir "%AIWRITE_ROOT%\vcpkg-cache\buildtrees"
if not exist "%AIWRITE_ROOT%\vcpkg-cache\packages" mkdir "%AIWRITE_ROOT%\vcpkg-cache\packages"
if not exist "%AIWRITE_ROOT%\vcpkg-installed" mkdir "%AIWRITE_ROOT%\vcpkg-installed"

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

echo [install-deps] 定位 MSVC 环境（vswhere 动态查找，不写死 VS 版本/盘符）...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSTMP=%TEMP%\aiwrite_vswhere.txt"
if exist "%VSTMP%" del "%VSTMP%" >nul
set "VSROOT="
if exist "%VSWHERE%" "%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSTMP%"
if exist "%VSTMP%" for /f "usebackq delims=" %%i in ("%VSTMP%") do set "VSROOT=%%i"
if not defined VSROOT set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
echo [install-deps] MSVC: %VCVARS%
if exist "%VCVARS%" (
  call "%VCVARS%" >nul
) else (
  echo [install-deps] 警告: 未找到 vcvars64.bat，继续尝试（端口构建可能失败）
)

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
