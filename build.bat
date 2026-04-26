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
cd %BUILD_DIR%

REM 配置
echo 配置项目...
cmake .. ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -G "Visual Studio 17 2022" ^
    -A x64 ^
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
