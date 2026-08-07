// =============================================================================
//  Player.h —— 播放器协调器 (Step3 的核心)
// =============================================================================
//
//  【职责】
//    组装 Step2 的四个模块, 加上 PacketQueue/FrameQueue, 拆成三线程并发.
//
//  【为什么需要这个协调器?】
//    Step2 是单线程: 读包→解码→显示 串行. 解码耗时会让画面卡顿.
//    Step3 把流水线拆成并发线程, 中间用队列缓冲:
//
//      ┌──────────┐      ┌─────────────┐      ┌──────────┐      ┌─────────────┐
//      │ read线程  │────▶│ PacketQueue │───▶ │ decode线程│───▶│ FrameQueue  │
//      │ (解封装)  │      │  (有界, 背压)│      │ (解码)    │      │ (环形, 缓冲) │
//      └──────────┘      └─────────────┘      └──────────┘      └──────┬──────┘
//                                                                             │
//                                                                             ▼
//                                                                    ┌──────────────┐
//                                                                    │ main线程渲染  │
//                                                                    │ (SDL显示)     │
//                                                                    └──────────────┘
//
//    三线程并发后, 解码慢时 FrameQueue 里还囤着已解好的帧, 画面不卡;
//    解封装快时 PacketQueue 满了让 read 线程停一停, 内存不爆.
//
//  【EOF 传播】(关键设计)
//    read 线程读到 EOF 时, 不能直接退出 —— 否则 decode 线程还在等数据, 死锁.
//    正确做法: read 线程向 PacketQueue 推一个 nullptr 包表示 EOF,
//              decode 线程收到 nullptr 后向解码器 send NULL 冲洗管线,
//              把缓冲里的最后几帧取出来入 FrameQueue, 然后退出.
//    这样最末几帧不会丢 (H.264 的重排缓冲).
//
//  【quit 信号】
//    用户按 ESC 或关窗口时, signalQuit() 唤醒所有阻塞的线程, 让它们有序退出.
//    退出顺序: main 设 quit → PacketQueue/FrameQueue.signalQuit()
//              → read/decode 线程从阻塞返回 false → join.
//
//  【本 Step 的简化】
//    - 仍只视频, 无音频 (Step4 加)
//    - 渲染仍固定延时 (Step4 改成按 PTS 同步)
//    - 无 seek (后续)
// =============================================================================
#pragma once

#include <thread>
#include <atomic>

#include "IOSource.h"
#include "Demuxer.h"
#include "Decoder.h"
#include "VideoRenderer.h"
#include "PacketQueue.h"
#include "FrameQueue.h"

class Player {
public:
    Player();
    ~Player();

    // 打开并启动播放. url 是文件路径 (Step5 会支持任意 IOSource).
    // 成功后三线程已启动, 窗口已显示. 返回 false 表示初始化失败.
    bool open(const std::string& url);

    // 主线程在 SDL 事件循环里调这个, 渲染一帧 + 处理事件.
    // 返回 false 表示应该退出 (收到 ESC/关窗口).
    bool runFrame();

    // 关闭: 停线程 + 释放资源. open 失败时也要调 (清理已分配的).
    void close();

private:
    // ---- 三个线程函数 ----
    void readThread();      // 解封装: demuxer.readPacket → PacketQueue
    void decodeThread();    // 解码: PacketQueue → decoder → FrameQueue

    // ---- 模块 (Step2 的四个) ----
    Demuxer         m_demuxer;
    Decoder         m_decoder;
    VideoRenderer   m_renderer;
    IOSource*       m_io = nullptr;     // 外部拥有 (open 时创建, close 时释放)
    bool            m_ownIO = false;    // 是否由 Player 自己拥有 IO (用于 IOFile)

    // ---- 队列 ----
    PacketQueue     m_packetQueue;
    FrameQueue      m_frameQueue;

    // ---- 线程 ----
    std::thread     m_readThread;
    std::thread     m_decodeThread;

    // ---- 状态 ----
    std::atomic<bool> m_quit{false};      // 全局退出信号
    std::atomic<bool> m_eofReached{false};// read 线程是否已到 EOF
    int               m_videoIdx = -1;    // 视频流 index
    bool              m_opened = false;
};
