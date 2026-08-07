// =============================================================================
//  FrameQueue.h —— 线程安全的有界帧环形队列 (Decoder线程 → Renderer线程)
// =============================================================================
//
//  【职责】
//    在"解码线程"(生产者) 和 "渲染线程"(消费者) 之间缓冲已解码的 AVFrame.
//
//  【为什么用环形缓冲, 不像 PacketQueue 用 std::queue?】
//    两个原因:
//    1. 渲染线程取出帧后, 可能还要持有它一段时间 (Step4 同步: 当前帧显示完
//       才换下一帧). 如果用 std::queue pop 出去, 就只能拷贝 AVFrame, 开销大.
//       环形缓冲给出"槽位指针", 渲染线程持有指针, 数据不动.
//    2. 预分配 AVFrame 数组, 避免运行时反复 alloc/free.
//
//  【环形缓冲的工作方式】(容量 N, 实际可用 N-1)
//    [0][1][2][3][4][5][6][7]    (容量=8)
//          ↑           ↑
//       rIdx(读)    wIdx(写)
//    写: 写到 wIdx, wIdx=(wIdx+1)%N. 追上 rIdx 时表示满.
//    读: 读 rIdx,   rIdx=(rIdx+1)%N. 追上 wIdx 时表示空.
//    为了区分"满"和"空", 牺牲一个槽位: (wIdx+1)%N==rIdx 视为满.
//
//  【并发模型】
//    和 PacketQueue 一样: 一个 mutex + 两个条件变量.
//
//  【quit 语义】(和 PacketQueue 一致)
//    signalQuit 后, 阻塞的 acquire/put 立刻返回 false, 已入队的数据仍可被消费.
// =============================================================================
#pragma once

#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

class FrameQueue {
public:
    // capacity: 槽位数 (实际可用 capacity-1). 默认 8, 和 ffplay 一致.
    FrameQueue(int capacity = 8);
    ~FrameQueue();

    // ---- 生产者 (Decoder): 放入一帧 ----
    // 队列满时阻塞. 返回 false 表示 quit.
    // frame 的内容会被拷贝到内部预分配的槽位 (av_frame_move_ref).
    // 调用后 frame 会被 unref, 调用方不再持有数据.
    bool put(AVFrame* frame);

    // ---- 消费者 (Renderer): 获取一个可读槽位 ----
    // 队列空时阻塞. 返回 nullptr 表示 quit 且队列已空.
    // 返回的 AVFrame* 由队列拥有, 调用方用完后必须调 release() 归还.
    AVFrame* acquire();

    // 归还 acquire() 拿到的槽位, 释放该帧的引用, 槽位变可写.
    void release();

    // ---- 控制 ----
    void signalQuit() {
        m_quit.store(true);
        m_notFull.notify_all();
        m_notEmpty.notify_all();
    }
    bool isQuit() const { return m_quit.load(); }

    void flush();    // 清空 (清引用计数, 槽位可复用)

    int count() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_count;
    }

private:
    mutable std::mutex      m_mutex;
    std::condition_variable m_notFull;
    std::condition_variable m_notEmpty;
    std::vector<AVFrame*>   m_frames;     // 预分配的 AVFrame 指针数组
    int                     m_capacity;
    int                     m_rIdx = 0;   // 下一个要读的位置
    int                     m_wIdx = 0;   // 下一个要写的位置
    int                     m_count = 0;  // 当前元素数
    std::atomic<bool>       m_quit{false};
};
