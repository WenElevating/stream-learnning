@echo off
REM ============================================================
REM FFmpeg C++ 程序一键编译脚本 (MinGW g++ 版)
REM 用法: build.bat 程序序号
REM   build.bat 1      编译 01_probe.cpp
REM   build.bat 2      编译 02_decode.cpp
REM   build.bat 3      编译 03_streamer.cpp
REM ============================================================

setlocal

REM --- 配置 FFmpeg 路径 ---
set FF=D:\FFmpeg\ffmpeg-n7.1-latest-win64-gpl-shared-7.1

REM --- 把 FFmpeg 的 bin 加到 PATH(运行时需要 DLL) ---
set PATH=%FF%\bin;%PATH%

REM --- 切到本脚本所在目录 ---
cd /d "%~dp0"

REM --- 解析参数, 默认编译 1 ---
set NUM=%1
if "%NUM%"=="" set NUM=1

REM --- 补零成两位 (1 -> 01) ---
if "%NUM%"=="1" set SRC=01_probe.cpp
if "%NUM%"=="2" set SRC=02_decode.cpp
if "%NUM%"=="3" set SRC=03_streamer.cpp

if "%SRC%"=="" (
    echo [错误] 未知序号: %NUM%
    echo 可用: 1=probe, 2=decode, 3=streamer
    exit /b 1
)

if not exist "%SRC%" (
    echo [错误] 源文件不存在: %SRC%
    exit /b 1
)

echo ========================================
echo 编译: %SRC
echo ========================================
echo.

REM --- 核心编译命令 ---
REM -I: 头文件搜索路径
REM -L: 库文件搜索路径
REM -l: 链接的库(顺序有讲究: 依赖者在前, 被依赖者在后)
REM     avformat 依赖 avcodec, avcodec 依赖 avutil
g++ %SRC% -o build\%SRC:~0,2%.exe ^
    -I%FF%\include ^
    -L%FF%\lib ^
    -lavformat -lavcodec -lavutil ^
    -std=c++17 ^
    -Wall

if %errorlevel% equ 0 (
    echo.
    echo [成功] 输出: build\%SRC:~0,2%.exe
    echo.
    echo 运行示例:
    echo   build\%SRC:~0,2%.exe ..\labs\w1_sample.mp4
) else (
    echo.
    echo [失败] 编译出错, 请看上面的错误信息
)

endlocal
