// =============================================================================
//  FrameQueue.cpp —— 实现
// =============================================================================
#include "FrameQueue.h"
#include <cassert>

FrameQueue::FrameQueue(int capacity) : m_capacity(capacity) {
    // 预分配 capacity 个 AVFrame (用 av_frame_alloc, 它返回空引用的 frame).
    // 这些 frame 在整个队列生命周期内复用, 不再 free (析构时统一 free).
    m_frames.reserve(capacity);
    for (int i = 0; i < capacity; ++i) {
        m_frames.push_back(av_frame_alloc());
    }
}

FrameQueue::~FrameQueue() {
    flush();
    for (AVFrame* f : m_frames) {
        if (f->data[0]) av_frame_unref(f);   // 确保引用归零
        av_frame_free(&f);
    }
}

bool FrameQueue::put(AVFrame* frame) {
    std::unique_lock<std::mutex> lk(m_mutex);
    m_notFull.wait(lk, [this] {
        return m_count < m_capacity - 1 || m_quit.load();   // 留一个槽位区分满/空
    });
    if (m_quit.load()) return false;

    // 把 frame 的数据 move 到预分配的槽位 (零拷贝, 只搬 AVBufferRef 引用)
    AVFrame* slot = m_frames[m_wIdx];
    av_frame_unref(slot);                 // 槽位之前的引用先清掉 (应该已经是空的)
    av_frame_move_ref(slot, frame);       // move: frame 的所有权转到 slot, frame 变空

    m_wIdx = (m_wIdx + 1) % m_capacity;
    ++m_count;

    lk.unlock();
    m_notEmpty.notify_one();
    return true;
}

AVFrame* FrameQueue::acquire() {
    std::unique_lock<std::mutex> lk(m_mutex);
    m_notEmpty.wait(lk, [this] {
        return m_count > 0 || m_quit.load();
    });
    if (m_count == 0 && m_quit.load()) return nullptr;

    // 返回当前读位置的指针. 调用方持有期间, 这个槽位被"借出".
    // 用 m_acquired 标记, release() 时才真正推进 rIdx.
    // (简化设计: 一次只能借出一个. 对单消费者渲染线程足够.)
    AVFrame* f = m_frames[m_rIdx];
    m_rIdx = (m_rIdx + 1) % m_capacity;
    --m_count;
    return f;
}

void FrameQueue::release() {
    // acquire 时已经推进了 rIdx 和 count, 这里只是通知生产者有了空位.
    // (借出的那个槽位的数据由调用方在下次循环 unref, 或者我们这里不处理
    //  —— 实际上 slot 在下次 put 到它时会被 av_frame_unref 清掉, 安全.)
    std::lock_guard<std::mutex> lk(m_mutex);
    m_notFull.notify_one();
}

void FrameQueue::flush() {
    std::lock_guard<std::mutex> lk(m_mutex);
    // 把每个有数据的槽位的引用清掉
    for (AVFrame* f : m_frames) {
        if (f && f->data[0]) {
            av_frame_unref(f);
        }
    }
    m_rIdx = m_wIdx = 0;
    m_count = 0;
    m_notFull.notify_all();
}
