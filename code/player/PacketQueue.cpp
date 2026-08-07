// =============================================================================
//  PacketQueue.cpp —— 实现
// =============================================================================
//
//  ★ 重要: AVPacket 是 C 结构体, 内部有引用计数的 buf (AVBufferRef).
//          绝对不能用 C++ 的 std::move 搬它 (会破坏引用计数, 导致 double-free
//          或悬空指针). 必须用 FFmpeg 的 av_packet_move_ref / av_packet_ref.
// =============================================================================
#include "PacketQueue.h"

bool PacketQueue::push(AVPacket* pkt) {
    std::unique_lock<std::mutex> lk(m_mutex);

    // ---- 等待队列不满 ----
    m_notFull.wait(lk, [this] {
        return m_bytes < m_maxBytes || m_quit.load();
    });
    if (m_quit.load()) {
        return false;
    }

    // 入队. 用 av_packet_move_ref 转移所有权 (正确管理 buf 引用计数).
    // 先 push 一个空 packet 占位, 再 move_ref 把数据搬进去.
    AVPacket slot;
    av_packet_move_ref(&slot, pkt);   // pkt → slot (pkt 变空, 调用方无需再 unref)
    m_queue.push(std::move(slot));    // std::queue 用移动构造存入 (slot 内部已无 buf, 安全)
    m_bytes += m_queue.back().size;   // 用队列里那份的 size 统计

    lk.unlock();
    m_notEmpty.notify_one();
    return true;
}

bool PacketQueue::pop(AVPacket* outPkt) {
    std::unique_lock<std::mutex> lk(m_mutex);

    m_notEmpty.wait(lk, [this] {
        return !m_queue.empty() || m_quit.load();
    });
    if (m_queue.empty() && m_quit.load()) {
        return false;
    }

    // 出队. 同样用 av_packet_move_ref 把数据搬给调用方.
    AVPacket& front = m_queue.front();
    m_bytes -= front.size;
    av_packet_move_ref(outPkt, &front);   // front → outPkt
    m_queue.pop();

    lk.unlock();
    m_notFull.notify_one();
    return true;
}

void PacketQueue::flush() {
    std::lock_guard<std::mutex> lk(m_mutex);
    while (!m_queue.empty()) {
        AVPacket& front = m_queue.front();
        av_packet_unref(&front);
        m_queue.pop();
    }
    m_bytes = 0;
    m_notFull.notify_all();
}
