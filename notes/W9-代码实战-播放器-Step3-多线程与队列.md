# 播放器 Step 3 —— 多线程 + PacketQueue/FrameQueue

> 代码：`code/player/` 新增 5 个文件
> - `PacketQueue.h/.cpp` —— 包队列（Demuxer→Decoder）
> - `FrameQueue.h/.cpp` —— 帧环形队列（Decoder→Renderer）
> - `Player.h/.cpp` —— 协调器（三线程 + EOF 传播）
> - `step3_threaded_player.cpp` —— main
>
> 验证：MP4/FLV(1080p)/TS 全过；**4K@60fps 不卡**（多线程的核心价值）

---

## 一、为什么需要多线程（Step2 的问题）

Step2 单线程：`读包 → 解码 → 显示` **串行**。一帧解码耗时 30ms，那 30ms 内画面完全静止 —— 解 4K 时尤其严重。

Step3 把流水线拆成 **3 个并发线程**，中间用队列缓冲：

```
┌──────────┐      ┌─────────────┐      ┌──────────┐      ┌─────────────┐      ┌──────────┐
│ read线程  │────▶│ PacketQueue │───▶ │ decode线程│───▶│ FrameQueue  │───▶│ main线程  │
│ (解封装)  │      │ (有界,背压)  │      │ (解码)    │      │ (环形,缓冲)  │      │ (SDL渲染) │
└──────────┘      └─────────────┘      └──────────┘      └─────────────┘      └──────────┘
```

**多线程如何吸收抖动**：
- 解码某一帧特别慢（复杂场景）→ FrameQueue 里还囤着之前解好的帧，main 线程照样显示，画面不卡
- 解封装比解码快 → PacketQueue 满，read 线程阻塞（背压），内存不爆
- 解码比解封装快 → PacketQueue 空，decode 线程阻塞等待，不空转

---

## 二、两个队列的设计

### PacketQueue（有界，背压）

```
std::queue<AVPacket> + mutex + 2个条件变量(not_full / not_empty)
容量限制: 16MB (按字节, 不是按包数)
```

**为什么有界？** 无界时解封装比解码快会无限堆积，内存爆。有界 + 满则阻塞生产者 = 自然背压。

### FrameQueue（环形缓冲，预分配）

```
预分配的 AVFrame[8] 数组 + mutex + 2个条件变量
环形: rIdx/wIdx/count, 容量N实际可用N-1 (区分满/空)
```

**为什么用环形不用 std::queue？** 渲染线程取出帧后要持有它一段时间（显示完才换），环形给出"槽位指针"，数据不动，零拷贝。

---

## 三、核心知识点

### 知识点 1：生产者-消费者模式（mutex + 条件变量）

两个队列都用同一个套路。以 PacketQueue 的 push 为例：

```cpp
bool push(AVPacket* pkt) {
    std::unique_lock<std::mutex> lk(m_mutex);
    // ★ 条件变量 wait: 等到 "队列不满 或 quit" 才继续
    m_notFull.wait(lk, [this] { return m_bytes < m_maxBytes || m_quit.load(); });
    if (m_quit.load()) return false;
    // ... 入队 ...
    lk.unlock();
    m_notEmpty.notify_one();   // ★ 通知可能在等数据的消费者
}
```

**关键三步**：加锁 → wait(谓词) → 改状态后 notify。谓词 `wait(lk, 条件)` 是个语法糖，等价于：
```cpp
while (!条件) { cv.wait(lk); }   // 防止虚假唤醒
```

### 知识点 2 ★★：AVPacket/AVFrame 必须用 FFmpeg 的 ref API，不能用 std::move

这是本步**最深的坑**。最初我写：

```cpp
// ❌ 错误! 会内存损坏
m_queue.push(std::move(*pkt));
m_bytes += pkt->size;           // move 后 pkt 状态不确定
av_packet_unref(pkt);           // 对已 move 的 pkt unref → 灾难
```

**运行表现**：`Invalid NAL unit size (1100673097 > 5300)` —— 完全乱码的包大小，典型的内存损坏。

**根因**：`AVPacket` 是 C 结构体，内部有 `buf` 指针指向引用计数的 `AVBufferRef`。C++ 的 `std::move` 只做成员级移动，**不更新引用计数**。结果：
- move 后两个地方以为自己是 buf 的 owner
- `av_packet_unref` 释放了一次
- 另一个 owner 持有悬空指针，下次访问就破坏内存

**正确做法**：用 FFmpeg 专门的引用管理 API：
```cpp
// ✅ 正确
AVPacket slot;
av_packet_move_ref(&slot, pkt);   // pkt → slot, 正确转移 buf 引用, pkt 变空
m_queue.push(std::move(slot));    // 现在 slot 内部无 buf, std::move 安全
```

| 操作 | API | 语义 |
|------|-----|------|
| 转移所有权（move） | `av_packet_move_ref` | src 变空，dst 接管 |
| 共享所有权（copy） | `av_packet_ref` | 引用计数+1，两个都有效 |
| 释放引用 | `av_packet_unref` | 引用计数-1 |

**铁律**：**FFmpeg 的 C 对象，永远用 FFmpeg 自己的管理 API，不要用 C++ 的 move/copy**。AVFrame 同理用 `av_frame_move_ref`。

### 知识点 3：EOF 传播（最精巧的设计）

三线程下，EOF 是个麻烦事。read 线程到 EOF 时**不能直接退出** —— 否则 decode 线程还在 PacketQueue.pop() 阻塞等数据，死锁。

**解决方案：EOF 标记包**

```
read 线程读到 EOF:
  → 向 PacketQueue 推一个 "空包" (data==nullptr && size==0)
  → 这个空包像普通包一样排队
  → 自己退出

decode 线程 pop 到空包:
  → 知道是 EOF 标记
  → 向解码器 send NULL (avcodec_send_packet(ctx, nullptr)) 冲洗管线
  → 把 H.264 重排缓冲里残留的最后几帧 receive 出来入 FrameQueue
  → signalQuit FrameQueue
  → 退出

main 线程:
  → FrameQueue.acquire() 返回 nullptr (quit 且空)
  → runFrame 返回 false
  → 退出
```

**为什么要 send NULL 冲洗？** H.264 有 B 帧重排序，解码器内部缓存了几帧没输出。直接结束会**丢最后 5 帧左右**。send NULL 告诉解码器"没更多输入了，把缓存的都吐出来"。

### 知识点 4：quit 信号的广播

用户按 ESC 时，需要让**所有阻塞的线程**都醒来退出：

```cpp
void signalQuit() {
    m_quit.store(true);
    m_notFull.notify_all();    // 唤醒所有 push 阻塞
    m_notEmpty.notify_all();   // 唤醒所有 pop 阻塞
}
```

线程醒来后重新检查 `m_quit`，返回 false，循环退出。

### 知识点 5：Player 协调器模式

main 现在极其简单：
```cpp
Player player;
player.open(url);
while (player.runFrame()) { }   // 主线程只管渲染
player.close();
```

main **完全不知道**有线程、队列、EOF 传播的存在。这是协调器（Facade）模式的价值 —— 把复杂的并发协调封装起来，对外只暴露 open/runFrame/close 三个方法。

---

## 四、验证结果

| 测试片 | 分辨率 | 帧率 | 结果 |
|--------|--------|------|------|
| w1_sample.mp4 | 640×360 | 30 | ✅ EOF 传播正确，线程有序退出 |
| w1_av.mp4 | 640×360 | 30 | ✅（音频正确忽略） |
| w1.flv | 1920×1080 | - | ✅ |
| w1_bf.ts | 640×360 | - | ✅ |
| **66.mp4** | **3840×2160** | **60** | ✅ **持续播放 8 秒不卡不崩** |

4K@60fps 能持续播放是多线程的核心证明 —— Step2 单线程版本在 4K 下会严重卡顿。

---

## 五、本步踩的坑

| 坑 | 现象 | 根因 | 修复 |
|----|------|------|------|
| **std::move AVPacket** | `Invalid NAL unit size` + 退出码 127 | C++ move 不管理 buf 引用计数，内存损坏 | 用 `av_packet_move_ref` |
| `av_init_packet` deprecated | 编译警告 | FFmpeg 7.x 不再需要 | 用 `av_packet_alloc` 出来的空包当 EOF 标记 |
| `[decode] 线程退出` 打印两次 | 日志冗余 | EOF 分支 break + 循环退出都打印 | 小问题，不影响功能 |

---

## 六、线程退出顺序（理解整体流程）

正常播放完毕时：
```
1. read 线程: av_read_frame 返回 EOF
   → 推空包到 PacketQueue
   → 打印 [read] EOF
   → 退出

2. decode 线程: pop 到空包
   → send NULL 冲洗解码器
   → receive 出最后几帧入 FrameQueue
   → FrameQueue.signalQuit
   → 退出

3. main 线程: FrameQueue.acquire() 返回 nullptr
   → runFrame 返回 false
   → while 循环退出
   → player.close()

4. close: join read 线程 + decode 线程 → 释放资源
```

用户按 ESC 时：
```
1. main: SDL 事件检测到 ESC
   → runFrame 返回 false
   → player.close()

2. close: 设 m_quit + PacketQueue/FrameQueue.signalQuit
   → 唤醒所有阻塞的 push/pop

3. read 线程: push 返回 false → 退出
4. decode 线程: pop 返回 false → 退出
5. close: join 两个线程 → 释放资源
```

---

## 七、自测题

1. 为什么 PacketQueue 必须有界？无界会怎样？
   → 无界时解封装比解码快会无限堆积，内存爆。有界 + 满则阻塞 = 自然背压。

2. 为什么 FrameQueue 用环形缓冲，PacketQueue 用 std::queue？
   → 渲染线程要持有帧一段时间（显示完才换），环形给槽位指针零拷贝；包不需要持有，queue 够用。

3. `std::move(*pkt)` 一个 AVPacket 会出什么问题？
   → C++ move 不更新 buf 的引用计数，导致 double-free 或悬空指针。必须用 `av_packet_move_ref`。

4. read 线程到 EOF 时为什么不能直接退出？
   → decode 线程还在 PacketQueue.pop() 阻塞，直接退出会死锁。要推空包通知它。

5. decode 线程收到 EOF 标记后，为什么要 `send NULL`？
   → 冲洗 H.264 重排缓冲，把最后几帧挤出来。不送会丢末尾几帧。

6. `signalQuit` 为什么要 notify_all 而不是 notify_one？
   → 可能有多个线程在阻塞（push 和 pop 各一个），要全部唤醒。

7. 三线程模型下，4K@60fps 为什么不卡？
   → 解码慢的帧只影响 decode 线程，FrameQueue 里囤的旧帧让 main 线程照样 30fps 显示，队列吸收了抖动。

---

## 八、当前简化（Step4 要解决的）

| 简化 | 后果 | Step4 怎么改 |
|------|------|------------|
| 固定 `delay(33)` | 4K@60fps 会以 30fps 显示（浪费一半帧） | 按 PTS 延时 |
| 无音频 | 一半体验缺失 | 加 AudioPlayer + 音频回调 |
| 无 A/V 同步 | 加音频后会音画不同步 | audio master clock |

---

## 九、下一步（Step 4 预告）

Step4 是**最难的一步**：加音频 + A/V 同步。
- `AudioPlayer`：SDL 音频回调 + `swr_convert` 重采样
- `Clock`：audio master，`pts_drift` 外推
- 视频按 PTS 决策：早了 Delay、晚了丢帧、准点显示
- FFmpeg 7.x 的 `AVChannelLayout` 新写法（老 `channel_layout` 字段已删）

完成后就是真正的"能用的播放器"了。
