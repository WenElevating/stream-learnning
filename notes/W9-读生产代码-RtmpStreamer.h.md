# 读生产代码：RtmpStreamer.h (C++/CLI RTMP 推流器)

> 文件位置：`code-ref/RtmpStreamer.h`（已经我加注释的版本，原逻辑一字未改）
> 这是用户工作里的真实生产代码，是学习「理论如何落地」最好的样本。

---

## 一、一句话定位

**把 C# 业务层拿到的 H.264 裸字节流，封装成 FLV/RTMP 包，通过 FFmpeg 推到 RTMP 服务器。**

---

## 二、为什么是 C++/CLI（不是纯 C++ 也不是纯 C#）

注意 `.h` 但语法是 `ref class`、`String^`、`array<Byte>^`、`Thread^` —— 这是 **C++/CLI**，微软的"托管 C++"。

| 语法 | 含义 | 类比 |
|------|------|------|
| `ref class` | 托管类，活在 .NET 堆 | C# 的 `class` |
| `^` 句柄 | 托管对象引用 | C# 的引用变量 |
| `*` 指针 | 原生裸指针 | C++ 的指针 |
| `~T()` | Dispose（确定性） | C# 的 `IDisposable.Dispose` |
| `!T()` | Finalize（GC 调） | C# 的 `~T()` 析构/finalizer |
| `pin_ptr` | 钉住托管数组，让 native 拿地址 | 没有 C# 对等物（必须用固定块） |
| `int% x` | ref 参数 | C# 的 `ref/out` |

**核心价值**：它能同时写 .NET 托管代码和原生 C/FFmpeg 代码，天然是 C# ↔ FFmpeg 的桥梁。
这是无人机/桌面端推流器常见的工程选择：UI 用 C#（WPF/WinForms），底层用 C++（FFmpeg）。

---

## 三、整体架构 —— 经典生产者/消费者

```
   ┌───────────────────────────┐         ┌─────────────────────────────┐
   │  C# 业务线程 (生产者)       │         │  原生 StreamLoop 线程(消费者) │
   │  摄像头/硬编码器回调         │         │  后台线程,独立运行            │
   │                           │         │                             │
   │  streamer.InputData(bytes) │ ──写入──▶ StreamBufferContext (环形缓冲)│
   │                           │         │        │                    │
   └───────────────────────────┘         │        ▼ 消费               │
                                         │  av_parser_parse2 拆 NALU    │
                                         │  提取 SPS/PPS                │
                                         │  AnnexB→AVCC                 │
                                         │  打 PTS                      │
                                         │  av_interleaved_write_frame  │
                                         │        │                    │
                                         │        ▼                    │
                                         │   RTMP 服务器 (flv)          │
                                         └─────────────────────────────┘
```

### 为什么中间要套一个环形缓冲，而不是 C# 直接调 FFmpeg？

1. **解耦**：摄像头回调可能很急（60fps），FFmpeg 推流可能卡（网络），用缓冲吸收抖动。
2. **线程隔离**：C# 回调线程和 FFmpeg 的 native 上下文不能混用线程，用队列隔离。
3. **背压**：缓冲满了就**丢帧到下一个关键帧**。这是无人机/直播场景的铁律：
   **实时性 > 完整性**。宁可画面闪一下，也不能越积越卡。

---

## 四、按代码顺序梳理九大模块

### 模块 1：私有字段（状态）

分三组看：
- **线程&缓冲**：`bufferContext`(原生环形缓冲指针)、`streamThread`(消费者线程)、`rtmpUrl`
- **控制标志（全 volatile）**：`isRunning`、`isDisposing`、`writerCount`
- **统计**：`statTotalBytes/statWindowStart/statFrames` → 滚动算 fps/bps
- **配置**：`configuredFps`、`maxBufferBytes`(8MB)、`maxLatencyMs`(500ms)
- **时间戳队列**：`Queue<Int64>^ inputTimestamps` + `latestInputTimestampMillis`

> 关键设计：**时间戳随数据走**。每次 `InputData` 可带一个 `timestampMillis`，必须和字节流严格 1:1 入队出队。所以用 `Queue<Int64>` 配合缓冲字节流，消费者每消费一帧调一次 `PopNextTimestampToken()`。

### 模块 2：统计（UpdateStats）

**滑动窗口法**：攒满 1 秒就结算一次 fps/bps，再清零重来。
- fps = 帧数 × 1000 / 毫秒数
- bps = 字节数 × 8 × 1000 / 毫秒数
- 用 `Interlocked::Exchange` 原子地"取走并清零"，避免并发竞争。

### 模块 3：错误管理（SetLastError / SetAvError）

`lastError` 可能被 StreamLoop 和 C# 查询线程并发访问，所以**所有读写都加 `syncLock`**。
`SetAvError` 用 `av_strerror` 把 FFmpeg 的 int 错误码翻译成人话。

### 模块 4：时间戳管理（最精巧的部分）

#### IsTimestampTooStale
判断一帧是否"积压太久"。`ageMs = 最新时间戳 - 这帧时间戳`，超过 `maxLatencyMs` 就判 stale。
→ 触发丢帧模式，从下一个关键帧重开。**网络卡顿时主动止损**。

#### GetFallbackPts
当没有真实时间戳时，用 `frameCount` × `frameDuration` 兜底估算 PTS。
`(frameCount*1000 + fps/2)/fps` 是**四舍五入整数除法**，避免截断误差。
单位 ms，因为 FLV/RTMP 的 time_base = 1/1000。

#### NormalizePacketPts（重点）
四个职责，全靠引用传参保留跨帧状态：
1. 有真实时间戳 → 用它，但减去**首帧基准** `timestampBase`，让时间从 0 起算
2. 没有 → 用兜底 PTS
3. 首帧记录基准（`hasTimestampBase`）
4. **强制单调递增**：`if (pts <= lastPts) pts = lastPts + step;`
   - FFmpeg 对非单调 DTS 会报错甚至拒写，这是硬约束。

### 模块 5：StreamLoop（类的灵魂，≈250 行）

这是整个推流器的核心。它一个函数做了 6 件事：

```
① 初始化 FLV 输出上下文
   avformat_alloc_output_context2(&ctx, nullptr, "flv", url)
   - 必须显式 "flv"，因为 RTMP URL 推不出格式，FFmpeg 猜不到
   - 注册 interrupt_callback，Stop 时能打断阻塞操作

② 建视频流
   time_base = {1, 1000}  ← FLV/RTMP 铁律：毫秒时间基
   codec_id  = AV_CODEC_ID_H264

③ 准备 H.264 parser
   - 注意：不是真解码！只借 parser 把字节流切成"完整一帧"
   - parser 还会顺带吐出 width/height/SPS/PPS，一举多得

④ 主循环
   while (isRunning) {
     read_packet(ctx, inbuf)           // 从缓冲读最多 64KB
     while (有剩余字节) {
       av_parser_parse2(...)            // 喂给 parser
       if (out_size <= 0) continue;     // 这一口没凑成完整帧，继续喂
       ExtractSpsPps()                  // 抽 SPS/PPS（每帧都试，防中途换分辨率）
       ConvertAnnexBToAvcc()            // AnnexB → AVCC（FLV 强制要求 AVCC）
       ...进入分支 A 或 B
     }
   }

⑤ 分支 A：头还没写
   - 必须等 "SPS + PPS + 关键帧" 齐了才能 write_header
   - 期间把帧缓存到 pending_packets
   - 条件齐了：
     · 扔掉关键帧之前的非关键帧（参考帧没了）
     · BuildAvcDecoderConfigurationRecord → 填 codecpar->extradata
     · avio_open2（RTMP 握手在这里发生，3 秒超时）
     · avformat_write_header（FLV 头 + RTMP publish begin）
     · 把缓存的帧从关键帧起补写

⑥ 分支 B：头已写
   - 常规路径：打 PTS → WriteOwnedRtmpPacket
   - 失败处理分级：
     · EAGAIN 或 10 次失败 → Sleep(5) 退避
     · IsFatalRtmpWriteError → 立即放弃
     · 200 次连续失败 → 放弃
     · 每 50 次打一行日志（不刷屏）
```

#### 两类"丢帧模式"触发点（重要！）

| 触发条件 | 行为 |
|---------|------|
| `ctx->consumeOverflowDropped()`（缓冲溢出） | 置 dropUntilKeyframe，清空 pending |
| `IsTimestampTooStale`（延迟超 500ms） | 置 dropUntilKeyframe，清空 pending |

丢帧模式下：**不是关键帧就扔，关键帧也要等 SPS/PPS 齐**才恢复。
这正是无人机/直播的工程哲学：**花一帧的代价换流畅**。

### 模块 6：构造/析构（C++/CLI 双析构）

```
~RtmpStreamer()  = Dispose()  → 用户手动 using 调
!RtmpStreamer()  = Finalize() → GC 兜底调
```

析构里跳了一段"安全退出之舞"，非常小心：

```
1. isDisposing = true            ← 阻止新 InputData
2. Stop()                        ← 软+硬通知 StreamLoop 退出
3. streamThread->Join(10000)     ← 等线程死
   ├ 若还活着 → 故意泄漏缓冲，至少不崩 (return)
4. 取出 bufferContext 指针，置空
5. 自旋等 writerCount==0          ← 等所有正在 InputData 的写者退出
6. 双重检查（线程死 + 无写者）才 SafeDeleteBufferContext
```

**为什么这么啰嗦？** 因为这里有三线程并发：
- C# 析构线程（调 `!RtmpStreamer`）
- StreamLoop 线程（用 `bufferContext`）
- C# InputData 线程（写 `bufferContext`）

任何一个删早了都是**野指针/UAF 崩溃**。这里的写法是"宁可泄漏不可崩溃"的典型工程权衡。

### 模块 7：Start

参数归一化 → 全部状态重置 → 启动后台线程。
注意：**线程创建在锁外**，避免线程还没真跑起来就被误判。

### 模块 8：InputData（生产者入口，从 C# 进 native 的关口）

```
Interlocked::Increment(writerCount)   ← 必须在持锁前自增
Monitor::Enter(syncLock)
  ├ if (!isRunning || isDisposing) return
  ├ pin_ptr<Byte> p = &data[0]        ← 钉住托管数组防 GC 移动
  ├ ctx->write(p, len, &dropped)      ← 写环形缓冲
  ├ if (dropped) inputTimestamps->Clear()   ← 字节被丢时间戳也清
  └ if (has_ts) inputTimestamps->Enqueue(ts) ← 时间戳严格跟字节 1:1
Monitor::Exit
Interlocked::Decrement(writerCount)   ← 通知析构线程"我写完了"
```

`writerCount` 自增/自减**必须在锁外**，否则析构线程持锁时无法等到它归零 → 死锁。

`pin_ptr` 是 C++/CLI 特性：把托管数组钉死在内存里，让 native `ctx->write` 能安全取地址。否则 GC 移动数组就会野指针。

### 模块 9：状态查询
`GetLastError()` / `GetStatus(fps, bps)` 都加锁读，供 C# UI 显示。

---

## 五、关键 FFmpeg API 清单（对照学习）

| API | 作用 | 在本代码哪里 |
|-----|------|-------------|
| `avformat_network_init` | 初始化网络模块（RTMP 必需） | StreamLoop 开头 |
| `avformat_alloc_output_context2` | 创建输出上下文，指定 "flv" | StreamLoop |
| `avformat_new_stream` | 建一条流（视频） | StreamLoop |
| `av_parser_init` / `av_parser_parse2` | 字节流 → 完整 NALU | 主循环 |
| `avcodec_alloc_context3` | 给 parser 配一个解码器上下文 | StreamLoop |
| `BuildAvcDecoderConfigurationRecord` | 构造 FLV 必需的 extradata | write_header 前 |
| `avio_open2` | 打开 RTMP 连接（握手在这） | write_header 前 |
| `avformat_write_header` | 写 FLV 头 | write_header 前 |
| `WriteOwnedRtmpPacket` | 写一帧（封装 AVPacket） | 分支 A/B |
| `interrupt_callback` | 让 Stop 能打断阻塞 | StreamLoop |

---

## 六、对照 Step3c 自己写的 H264Streamer，能学到什么

| 自己写的 H264Streamer | 生产代码 RtmpStreamer |
|---------------------|---------------------|
| 单线程，pushFrame 直接写 | 生产者/消费者，环形缓冲解耦 |
| 假设 SPS/PPS 第一帧就有 | **每帧都重新抽 SPS/PPS**，应对变分辨率 |
| PTS 用固定 step | 双轨：真实时间戳优先 + 帧号兜底 + 单调保证 |
| 写失败直接报错退出 | 分级重试：EAGAIN 退避、200 次放弃、致命立即退 |
| 无背压 | dropUntilKeyframe 机制，溢出/超延迟都触发丢帧 |
| 析构只 close | 三线程安全退出舞蹈，宁泄漏不崩溃 |
| 直接用 `data[0]` | `pin_ptr` 钉住防 GC 移动 |

**最大收获**：生产代码把"实时性优先 + 线程安全 + 错误容忍"三件事做到了极致。理论（SPS/PPS/AVCC/PTS）是基础，但**工程化才是难点**。

---

## 七、自测问题（读完应该能答）

1. 为什么 time_base 必须是 `{1, 1000}`？
   → FLV/RTMP 容器规定 PTS 单位是毫秒。
2. 为什么要等"关键帧 + SPS + PPS"齐了才 write_header？
   → FLV 头里要带 extradata（参数集），且第一帧必须是 IDR，否则播放器解不出。
3. `dropUntilKeyframe` 触发后，如何恢复？
   → 遇到下一个关键帧且 SPS/PPS 齐了才清标志继续。
4. `writerCount` 为什么必须在持锁外自增？
   → 否则析构线程持锁时无法等到它归零，会死锁。
5. `pin_ptr` 在 `InputData` 里起什么作用？
   → 钉住托管数组防 GC 移动，让 native 的 `ctx->write` 能安全用裸指针。
6. `NormalizePacketPts` 为什么要强制 `pts > lastPts`？
   → FFmpeg 要求 DTS 单调递增，非单调会报错/拒写。
7. 为什么 avio_open2 要设 `rw_timeout=3000000`？
   → RTMP 握手可能很慢，3 秒超时避免无限阻塞，配合 interrupt_callback 可被 Stop 打断。

---

## 八、延伸：依赖的外部类型（本文件未定义）

这些是工程其它文件提供的，读代码时先当黑盒：

- **StreamBufferContext**：原生环形缓冲 + 条件唤醒，生产者/消费者的纽带
- **FFmpegResourcesGuard**：RAII，析构自动 `avformat_free` / `avio_close`
- **check_interrupt**：FFmpeg 阻塞操作的中断回调
- **ExtractSpsPps**：从 NALU 流抠 SPS/PPS
- **ConvertAnnexBToAvcc**：H.264 AnnexB（起始码）→ AVCC（长度前缀）
- **BuildAvcDecoderConfigurationRecord**：构造 FLV 的参数集头
- **WriteOwnedRtmpPacket**：封装 AVPacket 并 `av_interleaved_write_frame`
- **IsFatalRtmpWriteError**：判定错误码是否不可恢复
- **SafeWriteTrailer / PrepareRtmpOutputForClose / SafeAbortBufferContext / SafeDeleteBufferContext**：各种异常路径安全清理

> 下一步如果要看，建议先看 `StreamBufferContext`（环形缓冲的实现）和 `ConvertAnnexBToAvcc` / `BuildAvcDecoderConfigurationRecord`（这俩最能补完 NALU 格式理论）。
