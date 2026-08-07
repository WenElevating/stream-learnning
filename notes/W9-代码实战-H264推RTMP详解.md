# H.264 裸流推 RTMP 详解 —— 重点剖析 Bitstream Filter（BSF）

> 对应代码：`code/04_h264_to_rtmp.cpp`（Step3b）
> 相关代码：`code/H264Streamer.h`（Step3c，pushFrame 接口版）、`code-ref/RtmpStreamer.h`（生产代码）
> 本文重点：**第 97-158 行的 BSF 码流过滤器**，以及裸流推流的整体流程。

---

## 一、为什么这一步难 —— 裸流 vs 容器的本质差别

先建立一个关键认知，这是理解整个推流器的钥匙：

```
Step3 (MP4 remux):     输入 MP4 文件 → extradata 已塞好 SPS/PPS
                       (MP4 的 moov box 专门存了参数集, FFmpeg 打开就拿到了)
                       
Step3b (裸流推流):     输入 .h264 裸流 → extradata 是空的!
                       (裸流只是 NALU 串, SPS/PPS 散在前面的独立 NALU 里)
```

**FLV/RTMP 推流的硬约束**：`avformat_write_header` 在写 FLV 头时，**必须**用 `extradata` 里的 SPS/PPS 生成 **AVC sequence header Tag**。没有 sequence header，拉流端（ffplay/浏览器/手机）拿到包也解不开画面。

→ 所以裸流推流的核心矛盾是：**怎么从散在流里的 SPS/PPS NALU 把参数集收集起来，填进 `extradata`？**

答案就是 **Bitstream Filter**。

---

## 二、什么是 Bitstream Filter（BSF）

### 定义

**Bitstream Filter = 码流过滤器**。作用在**编码后的码流层**（不解码，不重编码），只做字节级的解析/改写。可以理解成"码流的管道滤镜"。

它和"滤镜"（filter，如 `-vf`）的区别：

| | Bitstream Filter (BSF) | Filter (`-vf`/`-af`) |
|---|---|---|
| 作用层 | 编码后的码流（H.264 NALU） | 解码后的像素/采样（YUV/PCM） |
| 是否解码 | ❌ 不解码 | ✅ 先解码 |
| 是否重编码 | ❌ 不重编码 | 通常后接编码 |
| 开销 | 极小（字节操作） | 大（解码+编码） |
| 典型用途 | SPS/PPS 提取、格式转换 | 缩放、加水印、调色 |

### FFmpeg 内置常用 BSF

| BSF 名字 | 作用 | 典型场景 |
|---------|------|---------|
| **`extract_extradata`** | 从码流提取 SPS/PPS 到 extradata | **本文场景**：裸流 → 有 extradata |
| `h264_mp4toannexb` | AVCC 格式 → AnnexB（加起始码，内联 SPS/PPS） | MP4 → TS/FLV |
| `h264_metadata` | 修改 SPS 里的参数（level、码率等） | 转码后调整 |
| `trace_headers` | 打印每帧的 SPS/PPS/SEI（调试用） | 排查问题 |
| `noise` | 给码流加噪声（测试容错） | 压力测试 |
| `remove_extra` | 删除冗余的 SPS/PPS/SEI | 码流瘦身 |

> 用 `ffmpeg -hide_banner -bsfs` 可以列出你的 FFmpeg 编译进的所有 BSF。

---

## 三、先理解 H.264 两种码流格式（最容易混淆的点）

这是理解 BSF 的前置知识，也是很多人搞混 `extract_extradata` 和 `h264_mp4toannexb` 的根源。

### AnnexB 格式（裸流/TS/FLV-over-RTMP 用）

```
[00 00 00 01] [SPS NALU] [00 00 00 01] [PPS NALU] [00 00 00 01] [IDR NALU] ...
 ↑起始码        ↑type=7    ↑起始码        ↑type=8    ↑起始码        ↑type=5
```

- NALU 之间用**起始码** `00 00 00 01`（或 3 字节 `00 00 01`）分隔
- SPS/PPS 是**独立的 NALU**，散在码流前面（通常每个 IDR 前都有）
- `.h264` / `.ts` 文件、RTMP 推流都是这种格式

### AVCC 格式（MP4 用）

```
extradata 里:  [AVCDecoderConfigurationRecord]  ← SPS/PPS 存这里
流里每个包:    [4字节大端长度] [NALU] [4字节长度] [NALU] ...
```

- NALU 之间用**长度前缀**（4 字节大端）分隔，没有起始码
- SPS/PPS **不散在流里**，统一存在 `extradata`（AVCDecoderConfigurationRecord 结构）
- MP4 文件用这种格式

### 两种 BSF 的方向

```
       AVCC 格式                          AnnexB 格式
  (MP4, 长度前缀,                    (裸流/TS/RTMP,
   SPS/PPS 在 extradata)              起始码, SPS/PPS 散在流里)
         │                                  │
         │  h264_mp4toannexb                │  extract_extradata
         │  (AVCC→AnnexB: 加起始码,         │  (AnnexB→有extradata:
         │   内联 SPS/PPS 到每个 IDR 前)     │   把流里的 SPS/PPS 抽出来)
         ▼                                  ▼
       AnnexB                           AVCC-like
                                         (有 extradata)
```

**你的场景**（`.h264` 裸流 → RTMP）：
- 输入：AnnexB 格式（裸流天然是）
- 需要：`extract_extradata` 把 SPS/PPS 抽到 `extradata`
- **不需要** `h264_mp4toannexb`（那是 MP4→TS 用的，方向不同）

---

## 四、BSF 的工作机制 —— send/receive（和编解码器一模一样）

这是理解代码的关键。BSF 采用**和 `avcodec_send/receive` 完全相同的模式**：

```
你的代码                BSF 内部                    输出
─────────              ─────────                   ─────
av_bsf_send_packet  ──→  [包进来, 解析 NALU]
                         ↓
                       [识别出 SPS/PPS? → 填到 par_out->extradata]
                         ↓
av_bsf_receive_packet ←── [输出处理后的包]
```

**三个关键特性**：

1. **send 一个包不一定立即 receive 一个包**（解耦的，和编解码器同理）
2. **extradata 不是从包里出来的，是从 `bsf_ctx->par_out` 里读的** —— 这是个**独立通道**，和包流并行
3. **send/receive 返回 EAGAIN 是正常的**，表示"暂时给不了，继续送"

### BSF 完整 API 调用序列

```cpp
// ① 找到 BSF (按名字)
const AVBitStreamFilter* bsf = av_bsf_get_by_name("extract_extradata");

// ② 分配上下文
AVBSFContext* bsf_ctx = nullptr;
av_bsf_alloc(bsf, &bsf_ctx);

// ③ 设置输入参数 (告诉 BSF 输入是什么 codec/格式)
avcodec_parameters_copy(bsf_ctx->par_in, in_codecpar);

// ④ 初始化 (这步后 par_out 可用)
av_bsf_init(bsf_ctx);

// ⑤ 处理循环 (send/receive)
while (还有包) {
    av_bsf_send_packet(bsf_ctx, pkt);          // 送
    while (av_bsf_receive_packet(bsf_ctx, out) == 0) {  // 取
        // 检查 par_out->extradata 是否已填充
        if (bsf_ctx->par_out->extradata_size > 0) {
            // 拿到 SPS/PPS 了!
        }
    }
}

// ⑥ flush (可选): 送 NULL 把内部缓冲的包都取出来
av_bsf_send_packet(bsf_ctx, nullptr);
while (av_bsf_receive_packet(bsf_ctx, out) == 0) { ... }

// ⑦ 释放
av_bsf_free(&bsf_ctx);
```

---

## 五、逐行精讲 `04_h264_to_rtmp.cpp` 的 BSF 段（第 113-158 行

这是本文的核心。我把它拆成 8 个步骤：

### 步骤 ① 检测是否需要提取（第 113 行）

```cpp
if (in_codecpar->extradata_size == 0) {
```

打开裸流后检查 `extradata`。`.h264` 裸流通常 `extradata_size == 0`（SPS/PPS 还散在流里）。如果是 MP4，这里不为 0，整个 BSF 段跳过。

### 步骤 ② 找到 BSF（第 116 行）

```cpp
const AVBitStreamFilter* bsf = av_bsf_get_by_name("extract_extradata");
```

`extract_extradata` 是 FFmpeg 内置 BSF，专门做"从码流提取参数集到 extradata"。支持 H.264/H.265/AV1 等多种 codec。

### 步骤 ③ 分配上下文（第 122-123 行）

```cpp
AVBSFContext* bsf_ctx = nullptr;
ret = av_bsf_alloc(bsf, &bsf_ctx);
```

`av_bsf_alloc` 创建一个 `AVBSFContext`，它内部会分配 `par_in`（输入参数）和 `par_out`（输出参数）两个 `AVCodecParameters`。此时它们都是空的。

### 步骤 ④ 设置输入参数（第 126 行）

```cpp
avcodec_parameters_copy(bsf_ctx->par_in, in_codecpar);
```

把输入流的 codec 信息（codec_id=H.264、分辨率、当前 extradata 等）拷给 BSF 的 `par_in`。**BSF 需要知道输入是什么 codec 才能正确解析 NALU**（H.264 和 H.265 的 NALU 头格式不同）。

### 步骤 ⑤ 初始化（第 127 行）

```cpp
ret = av_bsf_init(bsf_ctx);
```

初始化 BSF。**这步之后 `par_out` 才可读**。`extract_extradata` 在 init 时会复制 `par_in` 到 `par_out`，准备好接收提取出的 extradata。

### 步骤 ⑥ 喂包循环提取（第 131-149 行）★ 核心 ★

```cpp
AVPacket* tmp_pkt = av_packet_alloc();
bool got_extradata = false;
while (av_read_frame(in_fmt, tmp_pkt) >= 0 && !got_extradata) {
    // 送一个包给 BSF
    ret = av_bsf_send_packet(bsf_ctx, tmp_pkt);
    if (ret < 0) { av_packet_unref(tmp_pkt); continue; }
    
    // 尝试取出处理后的包
    AVPacket* out_pkt = av_packet_alloc();
    while (av_bsf_receive_packet(bsf_ctx, out_pkt) >= 0) {
        // ★ 关键: 检查 par_out 的 extradata 是否被填充
        if (bsf_ctx->par_out->extradata_size > 0) {
            // 提取到了! 拷回 in_codecpar
            avcodec_parameters_copy(in_codecpar, bsf_ctx->par_out);
            printf("[OK] BSF 提取到 SPS/PPS: %d 字节\n",
                   in_codecpar->extradata_size);
            got_extradata = true;
        }
        av_packet_unref(out_pkt);
    }
    av_packet_free(&out_pkt);
    av_packet_unref(tmp_pkt);
}
```

**逻辑解读**：
- 从裸流读包，一个个喂给 BSF
- BSF 解析每个包的 NALU，**一旦遇到 SPS（type=7）或 PPS（type=8）**，就把它们累加到 `par_out->extradata`
- 我们循环检查 `par_out->extradata_size`，一旦非 0 就说明提取成功，拷回 `in_codecpar`
- 通常读前 1-2 个包（含 IDR 前的 SPS/PPS NALU）就能提取到

**为什么用 `while` 而不是 `if`？** 因为 send/receive 是解耦的，一个包进去可能出 0 个或多个包。

### 步骤 ⑦ 释放 BSF（第 151 行）

```cpp
av_bsf_free(&bsf_ctx);
```

BSF 已经完成使命（提取 SPS/PPS），可以释放了。**注意**：释放前我们已经把 `par_out` 拷走了，所以释放不影响 `in_codecpar`。

### 步骤 ⑧ 重新定位到文件开头（第 157 行）

```cpp
av_seek_frame(in_fmt, vidx, 0, AVSEEK_FLAG_BACKWARD);
```

因为前面读了几包做提取，文件指针不在开头了。后面要**完整推流**，所以 seek 回 0。

---

## 六、提取完之后怎么用（BSF 段之后的流程）

BSF 段结束后，`in_codecpar->extradata` 已经塞好了 SPS/PPS。后面的流程就和 Step3 的 MP4 remux 几乎一样了：

### 第 3 步：创建输出容器（第 161-187 行）

```cpp
// RTMP 必须用 FLV (RTMP 底层就是 FLV over TCP)
const char* fmt_name = (strncmp(out_url, "rtmp", 4) == 0) ? "flv" : "rtsp";
avformat_alloc_output_context2(&out_fmt, nullptr, fmt_name, out_url);

// 创建输出流, 复制编码参数 (★ 含刚提取的 SPS/PPS!)
AVStream* out_stream = avformat_new_stream(out_fmt, nullptr);
avcodec_parameters_copy(out_stream->codecpar, in_codecpar);  // extradata 一起拷过去

// 设置 time_base (RTMP=1/1000, RTSP=1/90000)
out_stream->time_base = (strncmp(out_url, "rtsp", 4) == 0) 
    ? (AVRational){1, 90000}   // RTP 90kHz
    : (AVRational){1, 1000};   // FLV 毫秒
```

### 第 4 步：写容器头（第 213 行）★ SPS/PPS 在这里被用掉

```cpp
ret = avformat_write_header(out_fmt, &out_opts);
```

**这一步 FLV muxer 内部做的事**（黑盒，但你要知道）：
1. 写 FLV 文件头（`FLV\x01` 魔数 + 版本 + 流标志）
2. **读 `out_stream->codecpar->extradata`，解析出 SPS/PPS**
3. **生成 AVCDecoderConfigurationRecord**（FLV 的参数集结构）
4. **写成 AVC sequence header Tag**（FLV 的第一个 Tag，tag type=0x09，frame type=0x12）

→ 这就是为什么前面非要用 BSF 提取 SPS/PPS：**没有 extradata，这一步就生成不了 sequence header，拉流端解不开**。

### 第 5 步：推流主循环（第 228-275 行）—— 给裸流打时间戳

这是裸流推流的**第二个难点**：裸流没有 PTS/DTS，必须自己造。

```cpp
int64_t frame_pts = 0;
// 每帧 PTS 增量 (按输出 time_base)
pts_step = out_stream->time_base.den / (out_stream->time_base.num * fps);
// RTMP: 1000/30 ≈ 33 刻度/帧;  RTSP: 90000/30 = 3000 刻度/帧

while (av_read_frame(in_fmt, pkt) >= 0) {
    // ★ 给裸流包打时间戳 (裸流原本 pts=AV_NOPTS_VALUE)
    pkt->pts = frame_pts * pts_step;
    pkt->dts = frame_pts * pts_step;
    pkt->duration = pts_step;
    frame_pts++;
    
    // ★ 注意: 不调用 av_packet_rescale_ts!
    //   因为时间戳已经是按 out_stream->time_base 生成的, 再 rescale 会出错
    
    // 限速: 按 PTS 控制发送节奏 (避免一口气推完)
    double pts_sec = pkt->pts * av_q2d(out_stream->time_base);
    int64_t target_us = (int64_t)(pts_sec * 1000000);
    int64_t elapsed_us = av_gettime_relative() - start_time;
    if (target_us > elapsed_us) {
        av_usleep((unsigned)(target_us - elapsed_us));
    }
    
    av_interleaved_write_frame(out_fmt, pkt);  // 推出去
}
```

**两个关键点**：
1. **时间戳是按 `out_stream->time_base` 直接生成的**，所以**不能再 `av_packet_rescale_ts`**（那会把已经正确的值再除一次）。这是 Step3 里踩过的坑。
2. **限速**很重要：裸流读取极快（几十 MB/秒），不限速会把服务器缓冲撑爆。按 PTS 节奏 sleep 是正确做法。

---

## 七、对比三种实现方式（学习价值）

同样的"H.264 → RTMP"问题，有三套代码，思路完全不同：

### 方式 A：BSF 提取（本文件 `04_h264_to_rtmp.cpp`）

```
文件 → av_read_frame → BSF提取SPS/PPS → extradata → FLV muxer → RTMP
                       ↑ 让 FFmpeg 自动处理格式转换
```

- **依赖 FFmpeg**：用 BSF + FLV muxer 自动处理 SPS/PPS 和格式
- **简单**：~100 行核心代码
- **不够灵活**：BSF 是黑盒，你想精细控制（比如动态改 SPS）不方便

### 方式 B：手动 NALU 分割（你的 `H264Streamer.h`，Step3c）

```
字节流 → 自己 splitNalus() → 缓存 SPS/PPS → 自己 buildExtradata (AVCC) → FLV muxer → RTMP
                                  ↑ 手动管理一切
```

- **自己实现 NALU 分割**（处理 3/4 字节起始码）
- **自己构造 extradata**（`buildExtradata` 手写 AVCDecoderConfigurationRecord）
- **更灵活**：可以精细控制每个 NALU
- **更复杂**：要理解 AVCC 格式的每个字节

### 方式 C：parser 分割（生产代码 `RtmpStreamer.h`）

```
字节流 → av_parser_parse2 切帧 → ExtractSpsPps → ConvertAnnexBToAvcc 
       → BuildAvcDecoderConfigurationRecord → FLV muxer → RTMP
```

- 用 FFmpeg 的 **`av_parser_parse2`** 切帧（比手动 splitNalus 更鲁棒）
- 但 extradata 是**手动构造**的（`BuildAvcDecoderConfigurationRecord`）
- **生产级**：带背压、丢帧、重试、线程安全

### 三种方式对比

| | 方式 A (BSF) | 方式 B (手动 NALU) | 方式 C (parser) |
|---|---|---|---|
| SPS/PPS 提取 | BSF 自动 | 手动 splitNalus | av_parser_parse2 |
| extradata 构造 | FFmpeg 自动 | 手动 buildExtradata | 手动 BuildAvc... |
| 代码量 | 少 (~100行) | 中 (~300行) | 多 (~700行) |
| 灵活性 | 低 | 中 | 高 |
| 鲁棒性 | 高 (FFmpeg 处理) | 中 (自己处理边界) | 高 (FFmpeg+手动) |
| 适用场景 | 学习/快速原型 | 嵌入式/固定格式 | 生产环境 |

**学习路径建议**：A → B → C。先理解 BSF（让 FFmpeg 帮你做），再理解手动 NALU 分割（自己掌握细节），最后看生产代码（学工程化）。

---

## 八、关键 API 速查表

### BSF 相关

| API | 作用 |
|-----|------|
| `av_bsf_get_by_name(name)` | 按名字查找 BSF |
| `av_bsf_alloc(bsf, &ctx)` | 分配 BSF 上下文 |
| `avcodec_parameters_copy(ctx->par_in, src)` | 设置 BSF 输入参数 |
| `av_bsf_init(ctx)` | 初始化（之后 par_out 可用） |
| `av_bsf_send_packet(ctx, pkt)` | 送一个包进 BSF |
| `av_bsf_receive_packet(ctx, out)` | 取一个处理后的包 |
| `av_bsf_send_packet(ctx, NULL)` | flush（冲洗内部缓冲） |
| `av_bsf_free(&ctx)` | 释放 |

### 裸流推流相关

| API | 作用 |
|-----|------|
| `avformat_open_input(&fmt, path, NULL, opts)` | 打开裸流（FFmpeg 自动用 h264 demuxer） |
| `av_dict_set(&opts, "framerate", "30", 0)` | 告诉裸流 demuxer 帧率（裸流本身没有） |
| `avformat_write_header(fmt, opts)` | 写容器头（FLV 在这生成 sequence header） |
| `av_interleaved_write_frame(fmt, pkt)` | 写一个包（推流） |
| `av_usleep(us)` | 微秒级休眠（限速用） |

---

## 九、常见错误与排查

### 错误 1：拉流端黑屏 / 解不开

**原因**：`extradata` 为空，FLV 没生成 sequence header。

**排查**：
```cpp
printf("extradata_size = %d\n", out_stream->codecpar->extradata_size);
// 如果是 0, 说明 BSF 没提取到, 或 extradata 没拷到 out_stream
```

**修复**：确认 BSF 段执行了，且 `avcodec_parameters_copy(out_stream->codecpar, in_codecpar)` 这一步把含 SPS/PPS 的 codecpar 拷过去了。

### 错误 2：`Non-monotonically increasing DTS`

**原因**：时间戳不单调递增（FFmpeg 硬约束）。

**排查**：打印每帧的 `pkt->dts`，看是否递增。

**修复**：用 `frame_pts * pts_step` 生成，保证单调。生产代码 `RtmpStreamer.h` 里的 `NormalizePacketPts` 专门处理这个（强制 `pts > last_pts`）。

### 错误 3：推流速度过快，服务器断开

**原因**：没有限速，裸流一口气读完推完。

**修复**：按 PTS 节奏 `av_usleep`（见第 248-253 行）。

### 错误 4：RTSP 服务器拒绝推流

**原因**：`time_base` 设错了。RTSP 底层是 RTP，时钟必须是 90kHz（1/90000）。

**修复**：
```cpp
if (rtsp) out_stream->time_base = (AVRational){1, 90000};
```

### 错误 5：BSF 提取不到 SPS/PPS

**原因**：
- 输入根本不是标准 H.264（NALU 起始码不对）
- `par_in` 没正确设置（BSF 不知道 codec_id）

**排查**：用 `ffmpeg -i input.h264` 看 FFmpeg 能否识别。用 `trace_headers` BSF 打印 NALU 结构。

---

## 十、自测题

1. 为什么裸流推流需要 BSF，而 MP4 推流不需要？
   → MP4 的 moov box 已经存了 SPS/PPS 到 extradata；裸流的 SPS/PPS 散在流里，extradata 为空，需要 BSF 提取。

2. `extract_extradata` 和 `h264_mp4toannexb` 有什么区别？
   → 前者从 AnnexB 流提取 SPS/PPS 到 extradata（AnnexB → 有 extradata）；后者把 AVCC 转 AnnexB（MP4 → TS）。方向相反。

3. BSF 的 extradata 从哪里读？为什么不是从 receive_packet 的包里读？
   → 从 `bsf_ctx->par_out->extradata` 读。因为 extradata 是流级别的元数据（不是单个包的），BSF 用独立通道（par_out）输出。

4. 为什么裸流推流时不能调用 `av_packet_rescale_ts`？
   → 因为时间戳已经是按 `out_stream->time_base` 直接生成的（`frame_pts * pts_step`），再 rescale 会除两次导致错误。

5. BSF 的 send/receive 为什么是解耦的（送一个不一定立即取一个）？
   → 因为 BSF 内部可能需要缓存多个包才能处理（比如合并 NALU），和编解码器的 send/receive 同理。

6. FLV 的 AVC sequence header 是什么时候、由谁生成的？
   → 在 `avformat_write_header` 时，由 FLV muxer 从 `codecpar->extradata` 读取 SPS/PPS 自动生成。这就是为什么必须有 extradata。

7. 裸流推流为什么要限速（`av_usleep`）？
   → 裸流读取极快（几十 MB/秒），不限速会一口气推完，服务器缓冲撑爆或时间戳全挤在一起。按 PTS 节奏推是正确做法。

---

## 十一、延伸：你的无人机场景对应哪种方式？

你的无人机硬件编码器回调给的 H.264 字节流，本质就是**内存里的 AnnexB 裸流**。对应到上面三种方式：

- **方式 A (BSF)**：从内存读字节喂 BSF —— 可以，但 BSF 需要包的概念，要先切帧
- **方式 B (手动 NALU)**：你的 `H264Streamer.h` 就是这个 —— **最贴合**，因为硬件回调给的就是任意大小的字节块，要自己切 NALU
- **方式 C (parser)**：生产代码 `RtmpStreamer.h` —— 用 `av_parser_parse2` 切帧，最鲁棒

**生产代码为什么用方式 C 而不是方式 A？**
1. 方式 A 依赖 `av_read_frame`（文件 IO 抽象），硬件回调是内存字节流，要绕一层
2. 方式 C 的 `av_parser_parse2` 可以接受**任意大小的字节块**（不需要预先切好帧），完美匹配硬件回调的"来了多少字节就喂多少"
3. 生产环境要精细控制 SPS/PPS（动态分辨率变化），BSF 黑盒不够灵活

→ 所以你的 `H264Streamer.h`（方式 B）是走向生产代码（方式 C）的**中间学习步骤**。
