// =============================================================================
//  step3_threaded_player.cpp —— 多线程播放器 (Step 3)
// =============================================================================
//
//  【对比 Step2】
//    Step2: 单线程, 读包/解码/显示串行 → 解码耗时直接卡画面
//    Step3: 三线程并发, 队列缓冲 → 解码慢时 FrameQueue 有囤货, 画面不卡
//
//  【main 的职责】(被 Player 协调器大大简化)
//    1. 创建 Player, open(url) 启动三线程
//    2. 在主线程循环调 runFrame() 渲染
//    3. runFrame 返回 false 就退出
//
//  main 本身不知道有任何线程、队列的存在 —— 这就是协调器模式的好处.
//
//  编译: 见下面的命令 (注意新增了 PacketQueue.cpp FrameQueue.cpp Player.cpp)
//  运行: step3_player.exe <视频文件>
// =============================================================================
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "Player.h"

int main(int argc, char* argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    if (argc < 2) {
        std::cerr << "用法: " << argv[0] << " <视频文件>\n";
        return 1;
    }
    const std::string filename = argv[1];

    Player player;
    if (!player.open(filename)) {
        std::cerr << "[错误] 打开失败\n";
        player.close();
        return 1;
    }

    std::cout << "[信息] 开始播放, 按 ESC 或关窗口退出\n";

    // 主线程渲染循环: runFrame 一次 = 取一帧 + 显示 + 延时 + 处理事件
    while (player.runFrame()) {
        // 空循环, 所有工作由 runFrame 内部完成
    }

    player.close();
    std::cout << "[信息] 已退出\n";
    return 0;
}
