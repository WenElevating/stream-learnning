# W9 代码实战 · Step 3 (FFmpeg 最小推流器)

> 阶段：方案A / 阶段4 FFmpeg 框架实战
> 配套代码：`code/03_streamer.cpp`
> 状态：✅ 已跑通完整链路（推流器 → MediaMTX → ffprobe 拉流验证）

---

## 📌 本节一句话总结

推流 = **remux（重封装）**，不解码不编码，只把压缩包从一种容器搬到另一种容器（MP4→RTSP）；核心是 mux API 三件套：`alloc_output_context2` → `write_header` → 循环 `interleaved_write_frame` → `write_trailer`；关键细节是 **限速**（按时间节奏推）和 **时间戳换算**（`av_rescale_q`）。

---

## 1. ⭐ 核心概念：解码 vs 推流（remux）

这是 FFmpeg 最易混淆的两个概念：

```
解码 (Step2, Demux+Decode):
  文件 → 读包 → 解码成YUV像素
  压缩→原始, 膨胀1000倍, 用于看画面/处理

推流 (Step3, Demux→Mux):  ⭐ 不解码!
  文件A(MP4) → 读包 → 直接写包 → 文件B或网络流(RTSP)
  包还是那个包, 只换包装, 数据量不变, 速度极快
```

**为什么 remux 这么快**：MP4 和 RTSP 装的 H.264 包是一样的，只是容器不同。所以无人机载荷端一旦编码出 H.264 包，推流就是纯搬运。

---

## 2. 推流器架构（6 步）

```
第1步  avformat_open_input            ← 打开输入文件
第2步  avformat_alloc_output_context2 ← 创建输出容器(RTSP)
第3步  avformat_new_stream +          ← 复制每路流的编码参数
       avcodec_parameters_copy          (SPS/PPS/分辨率都复制)
第4步  avio_open2 +                   ← 建立网络连接
       avformat_write_header            (RTSP握手: OPTIONS/DESCRIBE/SETUP/ANNOUNCE)
第5步  循环 {                          ⭐ 推流主循环
         av_read_frame                 ← 读输入包
         限速(等到该发的时刻)
         av_packet_rescale_ts          ← 时间戳换算(关键!)
         av_interleaved_write_frame    ← 推出去
       }
第6步  av_write_trailer               ← 写容器尾(RTSP TEARDOWN)
```

---

## 3. 推流核心 API

| API | 作用 | 对应原理 |
|---|---|---|
| `avformat_alloc_output_context2` | 创建输出容器 | W2 容器 |
| `avformat_new_stream` | 在输出建一路流 | W1 流 |
| `avcodec_parameters_copy` | 复制编码参数(SPS/PPS) | W4 SPS/PPS |
| `avio_open2` | 建立网络连接 | — |
| `avformat_write_header` | 写容器头(RTSP握手) | W6 RTSP |
| `av_packet_rescale_ts` | **时间戳换算** | W1-2 time_base |
| `av_interleaved_write_frame` | **推一个包(核心)** | — |
| `av_write_trailer` | 写容器尾(TEARDOWN) | W6 RTSP |

---

## 4. ⚠️ 踩坑记录（4个，都是真实工程坑）

### 坑1：`avformat_alloc_output_context2` 推不出 RTSP

**现象**：报错 `Unable to choose an output format for 'rtsp://...'`
**根因**：这个函数靠**URL 文件后缀**推断格式，但 `rtsp://...` 没有后缀
**解决**：显式传第3个参数 format_name：
```c
avformat_alloc_output_context2(&out_fmt, nullptr, "rtsp", out_url);
                                                          ↑ 必须显式
```

### 坑2：不限速导致拉流端"快进"或缓冲区爆

**现象**：文件几毫秒推完，拉流端看到快进或卡顿
**根因**：视频文件包是密集存储的，但推流必须按时间节奏发
**解决**：限速逻辑
```c
// 该包应该发送的时刻(微秒)
int64_t target_us = pkt_time_us - first_pkt_time;
// 已经过去的时间
int64_t elapsed_us = av_gettime_relative() - start_time;
if (target_us > elapsed_us) {
    av_usleep(target_us - elapsed_us);  // 等到该发的时刻
}
```

### 坑3：时间戳不换算导致音画不同步

**现象**：拉流端 PTS 全错，音画不同步
**根因**：输入流和输出流的 time_base 不同（MP4视频1/15360 vs RTSP/RTP 1/90000）
**解决**：每次推包前换算
```c
av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
```

### 坑4：C++ goto 不能跨过变量初始化

**现象**：编译报错 `jump to label crosses initialization`
**根因**：C++ 规定 goto 不能跳过带初始化的变量定义
**解决**：把所有变量提前声明在 goto 之前，或改用 RAII/函数封装

---

## 5. ⭐ 限速原理（重要）

```
问题: 视频文件里 30 秒的包, 读完只要几毫秒
      如果不限速, 瞬间全推给服务器
      → 服务器缓冲区爆
      → 或拉流端看到"快进30秒"
      
解决: 按"应该发送的时刻"控制节奏
      1秒的视频要花1秒推, 实时性才有意义

伪代码:
  start_time = 当前时间
  对每个包:
    target_time = 这个包的PTS(换算成微秒) - 第一个包的PTS
    elapsed = 当前时间 - start_time
    if target_time > elapsed:
      睡眠 (target_time - elapsed)
    推这个包
```

真实无人机载荷端不需要限速（摄像头按帧率实时产包），但**文件推流**必须限速。

---

## 6. 实测验证（完整链路打通）

### 推流端（我们的 03_streamer.exe）
```
[OK] 打开输入: 2 路流
[OK] 输出格式: rtsp
[OK] 流 #0 (h264) → 输出流 #0
[OK] 流 #1 (aac)  → 输出流 #1
[OK] 容器头已写入 (RTSP 握手完成)
━━━━━━ 开始推流 ━━━━━━
  共推送 221 个包
━━━━━━ 推流结束 ━━━━━━
```

### MediaMTX 服务器收到
```
[session 79fc506a] is publishing to path 'mystream', 2 tracks (H264, MPEG-4 Audio)
```

### 拉流端（ffprobe 模拟地面端）
```
stream,h264,video,640,360    ← 视频流成功接收
stream,aac,audio             ← 音频流成功接收
```

**完整闭环**：
```
w1_av.mp4 → 03_streamer.exe → RTSP握手 → MediaMTX → ffprobe拉流 ✅
(载荷源)    (我们写的推流器)   (W6/W7)    (服务器)    (地面端)
```

---

## 7. 三个 Step 的完整 API 对照

| Step | 套路 | 核心 API |
|---|---|---|
| Step1 探测 | 打开+遍历 | `avformat_open_input` → 遍历 `streams` |
| Step2 解码 | 读+解 | `av_read_frame` → `avcodec_send/receive` |
| Step3 推流 | 读+写 | `av_read_frame` → `av_interleaved_write_frame` |

**对比解码和推流**：都用 `av_read_frame` 读包，但：
- 解码：把包送 `avcodec_send_packet` 解成 YUV
- 推流：把包直接送 `av_interleaved_write_frame` 推出去（不解码！）

---

## ✅ 自检问题

1. 推流（remux）和解码（decode）的本质区别是什么？为什么 remux 这么快？
2. `avformat_alloc_output_context2` 推 RTSP 时为什么要显式传 `"rtsp"`？
3. 为什么文件推流必须限速？真实无人机载荷端需要限速吗？
4. 输入流和输出流的 time_base 为什么可能不同？不换算会怎样？
5. `av_interleaved_write_frame` 和 `av_write_frame` 的区别？（提示：interleaved 会缓冲重排）
6. RTSP 推流的完整生命周期对应哪几个 API？（握手→推流→结束）

<details>
<summary>🔑 参考答案</summary>

1. 解码把压缩包解成原始YUV（膨胀1000倍）；推流只搬运压缩包换容器，不解码不编码，数据量不变所以极快。
2. 函数靠URL文件后缀推断格式，`rtsp://...` 没有后缀，推断不出。必须显式指定 format_name="rtsp"。
3. 文件包密集存储，几毫秒读完，但推流要按时间节奏。否则服务器缓冲爆/拉流端快进。真实载荷端按帧率实时产包，不需要限速。
4. MP4视频 tb=1/15360，RTSP/RTP tb=1/90000，不同。不换算拉流端时间戳全错，音画不同步。用 `av_packet_rescale_ts`。
5. interleaved 会内部缓冲重排，保证音视频包按时间顺序交错输出（对RTSP/MP4重要）。普通 write_frame 不缓冲直接写。
6. write_header（握手 OPTIONS/DESCRIBE/SETUP/ANNOUNCE）→ interleaved_write_frame（推流）→ write_trailer（TEARDOWN）。

</details>

---

## 📚 下一步

- ✅ Step 1 探测
- ✅ Step 2 解码
- ✅ Step 3 推流 ← 本节，**完整链路已打通**
- ⏭️ 进阶：转码（解码+编码）、滤镜、多协议（SRT/WebRTC）、低延迟调优
