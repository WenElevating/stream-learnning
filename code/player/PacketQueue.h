// =============================================================================
//  PacketQueue.h —— 线程安全的有界包队列 (Demuxer线程 → Decoder线程)
// =============================================================================
//
//  【职责】
//    在"解封装线程"(生产者) 和 "解码线程"(消费者) 之间缓冲 AVPacket.
//    解封装快、解码慢时, 队列吸收这个速度差; 队列满时让生产者停一停(背压).
//
//  【为什么必须有界?】
//    如果无界, 解封装比解码快时会无限堆积, 内存爆掉.
//    有界 + 满时阻塞生产者, 形成自然的"背压"(backpressure), 内存可控.
//
//  【并发模型】
//    一个 mutex 保护内部链表 + 两个条件变量:
//      not_full_cond  : 队列从满变非满时唤醒 (叫醒被 push 阻塞的生产者)
//      not_empty_cond : 队列从空变非空时唤醒 (叫醒被 pop 阻塞的消费者)
//    这就是教科书上的"生产者-消费者"模式.
//
//  【AVPacket 的所有权】
//    push 时调用方 transfer 所有权给队列 (队列内部 av_packet_ref 增引用).
//    pop 时 transfer 给消费者 (消费者用完要 av_packet_unref).
//    队列销毁/flush 时释放剩余包.
//
//  【为什么不直接存 AVPacket 值?】
//    AVPacket 内部有动态分配的 buf 字段 (AVBufferRef, 引用计数).
//    av_packet_ref/move 正确管理这个引用计数, 避免深拷贝大数据.
// =============================================================================
#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

extern "C" {
#include <libavcodec/avcodec.h>
}

class PacketQueue {
public:
    PacketQueue() = default;
    ~PacketQueue() { flush(); }

    // 设置容量上限 (字节数). 达到上限后 push 会阻塞.
    void setMaxBytes(size_t maxBytes) { m_maxBytes = maxBytes; }

    // ---- 生产者: push 一个包 (transfer 所有权) ----
    // 如果队列已满, 阻塞直到有空间 (或 quit 被设置).
    // 返回 false 表示 quit 被设置, 应该退出 push 循环.
    // 注意: pkt 调用后会 av_packet_unref, 调用方不要再使用它.
    bool push(AVPacket* pkt);

    // ---- 消费者: pop 一个包 ----
    // 如果队列空, 阻塞直到有包 (或 quit 被设置).
    // 返回 false 表示 quit 被设置且队列已空, 应该退出消费循环.
    // 成功时 pkt 包含一个引用计数+1 的包, 消费者用完要 av_packet_unref.
    bool pop(AVPacket* outPkt);

    // ---- 控制 ----
    // 请求退出: 唤醒所有阻塞的 push/pop, 让它们赶紧返回 false.
    // 这个函数线程安全, 可以从任意线程调用 (通常从 main 在退出时调).
    void signalQuit() {
        m_quit.store(true);
        // 唤醒所有人, 让它们重新检查 m_quit 标志
        m_notFull.notify_all();
        m_notEmpty.notify_all();
    }

    // 判断是否已请求退出
    bool isQuit() const { return m_quit.load(); }

    // 清空队列 (释放所有包). 用于 seek 后丢弃过期数据.
    void flush();

    // 当前包数 (调试用)
    int size() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return (int)m_queue.size();
    }

    // 当前字节数 (调试用)
    size_t bytes() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_bytes;
    }

private:
    mutable std::mutex      m_mutex;
    std::condition_variable m_notFull;     // 队列非满 (push 可继续)
    std::condition_variable m_notEmpty;    // 队列非空 (pop 可继续)
    std::queue<AVPacket>    m_queue;       // 包队列 (AVPacket 内含引用计数)
    size_t                  m_bytes = 0;   // 当前队列总字节数 (用于容量限制)
    size_t                  m_maxBytes = 8 * 1024 * 1024;  // 默认 8MB
    std::atomic<bool>       m_quit{false}; // 退出信号
};
