@echo off
REM =============================================================================
REM  build_player.bat —— 编译 Step2 模块化播放器
REM  用法 (在 Git Bash 里): cmd.exe //C build_player.bat
REM =============================================================================
setlocal

set PATH=D:\msys64\ucrt64\bin;D:\FFmpeg\ffmpeg-n7.1-latest-win64-gpl-shared-7.1\bin;%PATH%
set FF=D:\FFmpeg\ffmpeg-n7.1-latest-win64-gpl-shared-7.1

set SRC=step2_modular_player.cpp Demuxer.cpp Decoder.cpp VideoRenderer.cpp
set OUT=..\build\step2_player.exe
set INC=-ID:/msys64/ucrt64/include/SDL2 -I%FF%/include
set LIB=-LD:/msys64/ucrt64/lib -L%FF%/lib -lSDL2 -lavformat -lavcodec -lavutil -lswscale

if not exist ..\build mkdir ..\build

echo [编译] g++ %SRC%
g++ %SRC% -o %OUT% %INC% %LIB% -std=c++17
if %errorlevel% neq 0 (
    echo [失败] 编译出错
    exit /b 1
)
echo [成功] 已生成 %OUT%
endlocal
