@echo off
REM VideoEye Windows 构建脚本
REM 用法: build.bat [debug|release]

setlocal

if "%1"=="" (
    set BUILD_TYPE=Release
) else (
    if /i "%1"=="debug" (
        set BUILD_TYPE=Debug
    ) else (
        set BUILD_TYPE=Release
    )
)

set BUILD_DIR=build-%BUILD_TYPE%

echo =====================================
echo VideoEye 构建脚本 (Windows)
echo =====================================
echo 构建类型: %BUILD_TYPE%
echo 构建目录: %BUILD_DIR%
echo =====================================

REM 创建构建目录
if not exist %BUILD_DIR% mkdir %BUILD_DIR%

REM 保存项目根目录（cd 前）
set "PROJECT_ROOT=%CD%"
cd %BUILD_DIR%

REM 配置
echo 配置项目...

REM 检测 vcpkg 安装路径
set VCPKG_ARGS=
if defined VCPKG_ROOT (
    if exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
        echo 使用 vcpkg toolchain: %VCPKG_ROOT%
        set "VCPKG_ARGS=-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"
    )
)
if "%VCPKG_ARGS%"=="" if exist "%PROJECT_ROOT%\vcpkg_installed\x64-windows\share\opencv4" (
    echo 使用 vcpkg_installed: %PROJECT_ROOT%\vcpkg_installed\x64-windows
    set "VCPKG_ARGS=-DCMAKE_PREFIX_PATH=%PROJECT_ROOT%/vcpkg_installed/x64-windows"
)
if "%VCPKG_ARGS%"=="" (
    echo 警告: 未找到 vcpkg，OpenCV/Qt6/SDL2 可能找不到！
    echo   请安装 vcpkg 或设置 CMAKE_PREFIX_PATH 环境变量
)

cmake .. ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -G "Visual Studio 17 2022" ^
    -A x64 ^
    %VCPKG_ARGS% ^
    -DBUILD_TESTING=OFF ^
    %*

if %errorlevel% neq 0 (
    echo 配置失败!
    pause
    exit /b %errorlevel%
)

REM 编译
echo 开始编译...
cmake --build . --config %BUILD_TYPE%

if %errorlevel% neq 0 (
    echo 编译失败!
    pause
    exit /b %errorlevel%
)

echo =====================================
echo 构建完成!
echo 可执行文件位置: %BUILD_DIR%\bin\VideoEye.exe
echo =====================================
pause
