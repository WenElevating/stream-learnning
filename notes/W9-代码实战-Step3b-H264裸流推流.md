# W9 代码实战 · Step 3b (H.264 裸流 → RTMP/RTSP 推流器)

> 阶段：方案A / 阶段4 FFmpeg 框架实战
> 配套代码：`code/04_h264_to_rtmp.cpp`
> 状态：✅ RTMP + RTSP 双协议完整链路打通

---

## 📌 本节一句话总结

真实载荷给的是 **H.264 裸流**（只有 NALU，没有容器），你必须自己处理 3 件事：**提取 SPS/PPS 到 extradata**、**按帧率生成 PTS/DTS**、**设置正确的 time_base**（RTSP 必须 1/90000，FLV/RTMP 用 1/1000）；这三个坑踩透，裸流推流就通了。

---

## 1. ⭐ 裸流 vs 容器：为什么裸流推流难

```
Step3 (MP4 remux, 简单):
  MP4文件 → 读包 → 推流
  容器帮你做好: 时间戳/SPS-PPS索引/包边界

Step3b (裸流, 真实载荷场景):
  H.264裸流 → ??? → 推流
  裸流啥都没有, 你必须自己造!
```

| 容器帮你做的 | 裸流没有 | 你要怎么办 |
|---|---|---|
| 时间戳 PTS/DTS | ❌ 全是 NOPTS | ⭐ 按帧率自己生成 |
| SPS/PPS 索引 | ❌ 散在流里 | 提取到 extradata |
| 包边界 | ❌ 只有起始码 | h264 demuxer 用起始码切 |
| time_base | ❌ 不确定 | ⭐ 必须按协议设 |

---

## 2. 三个核心难点 + 解决方案

### 难点1：SPS/PPS 提取

**问题**：FLV/RTMP 推流要求先发"AVC sequence header"（含 SPS/PPS），裸流的 SPS/PPS 在流里没单独索引。

**解决**：FFmpeg 的 h264 demuxer 打开裸流时，**会自动把 SPS/PPS 收集到 `codecpar->extradata`**（实测 39 字节）。只要 extradata 不为空，`avformat_write_header` 会自动生成 FLV sequence header。

```c
// 检查 extradata
if (in_codecpar->extradata_size > 0) {
    // 已有 SPS/PPS, FFmpeg 会自动处理
}
// 兜底: 如果为空, 用 extract_extradata BSF 提取
```

### 难点2：⭐ 时间戳生成（最关键）

**问题**：裸流的 `pkt->pts/dts` 都是 `AV_NOPTS_VALUE`，必须自己按帧率算。

**解决**：
```c
// 每帧 PTS 增量 = time_base.den / (time_base.num × fps)
int64_t pts_step = out_stream->time_base.den / (out_stream->time_base.num * fps);

// 每个包
pkt->pts = frame_pts * pts_step;
pkt->dts = frame_pts * pts_step;
pkt->duration = pts_step;
frame_pts++;
```

### 难点3：⭐⭐ time_base 必须按协议设（踩了最久的坑）

**问题**：设成 `1/fps` 时，RTSP 服务器**拒绝 publish**（连接建立但流无效）。

**根因**：RTSP 底层是 RTP，**RTP 标准时钟是 90kHz**，time_base 必须是 `1/90000`。设错了服务器识别不了。

**解决**：
```c
if (strncmp(out_url, "rtsp", 4) == 0) {
    out_stream->time_base = (AVRational){1, 90000};  // RTP 90kHz ⭐
} else {
    out_stream->time_base = (AVRational){1, 1000};   // FLV 毫秒
}
```

---

## 3. ⚠️ 踩坑全记录（5个，都是真实工程坑）

| # | 坑 | 现象 | 根因 | 解决 |
|---|---|---|---|---|
| 1 | **time_base 设错** | RTSP 服务器收到连接但不 publish（404/no stream） | RTSP 必须 1/90000 (RTP 90kHz)，不能用 1/fps | 按协议设 time_base |
| 2 | **时间戳重复** | `non monotonically increasing dts ... 1 >= 1` | 用 1/fps time_base 时 rescale 把所有 pts 压成相同值 | 直接用帧号×pts_step，不再 rescale |
| 3 | **RTMP URL 格式** | `no stream is available` | RTMP URL 必须两段：`rtmp://host/app/stream` | 用 `rtmp://localhost:1935/live/cam01` |
| 4 | **format_name 推断** | `Unable to choose output format` | URL 没后缀，FFmpeg 推断不出格式 | 显式传 `"flv"` 或 `"rtsp"` |
| 5 | **C++ goto 跨初始化** | 编译报 `crosses initialization` | goto 跳过变量定义 | 所有变量提前声明 |

---

## 4. 实测验证（双协议都打通）

### RTSP 链路 ✅
```
推流器 → rtsp://localhost:8554/cam01
MediaMTX: is publishing to path 'cam01', 1 track (H264)
MediaMTX: is reading from path 'cam01', with TCP, 1 track (H264)
ffprobe: stream,h264,video,640,360  ← 拉流成功!
```

### RTMP 链路 ✅
```
推流器 → rtmp://localhost:1935/live/cam01
MediaMTX: is publishing to path 'live/cam01', 1 track (H264)
MediaMTX: is reading from path 'live/cam01', 1 track (H264)
ffprobe: stream,h264,video,640,360  ← 拉流成功!
```

---

## 5. 关键 API 清单

| API | 作用 | 备注 |
|---|---|---|
| `avformat_open_input` | 打开裸流 | 用 h264 demuxer，自动切 NALU |
| `avcodec_parameters_copy` | 复制编码参数 | 含 extradata(SPS/PPS) |
| `avformat_alloc_output_context2` | 创建输出容器 | ⭐ 显式传 format_name |
| `avio_open2` | 建立网络连接 | RTMP/RTSP |
| `avformat_write_header` | 写容器头 | ⭐ 自动生成 FLV sequence header |
| `av_interleaved_write_frame` | 推包 | 核心 |
| `av_bsf_get_by_name("extract_extradata")` | BSF 提取 SPS/PPS | 兜底方案 |

---

## 6. 调试技巧（以后必用）

### 如何判断推流是否真的成功
看 MediaMTX 日志的关键行：
- ❌ `[conn] opened` 后 `closed: no stream is available` → 推流没成功
- ✅ `[session] is publishing to path 'xxx'` → 推流成功
- ✅ `[session] is reading from path 'xxx'` → 拉流成功

### RTSP time_base 验证
```c
printf("time_base = %d/%d\n", out_stream->time_base.num, out_stream->time_base.den);
// RTSP 应该是 1/90000
```

---

## ✅ 自检问题

1. 裸流推流比 MP4 remux 难在哪？要自己处理哪 3 件事？
2. 为什么 RTSP 的 time_base 必须是 1/90000？设成 1/fps 会怎样？
3. FLV/RTMP 推流为什么必须先发 SPS/PPS？FFmpeg 怎么自动处理这个？
4. `non monotonically increasing dts` 错误是什么意思？怎么产生的？
5. RTMP 的 URL 格式有什么要求？为什么 `rtmp://host:port/live` 不行？
6. extradata 里装的是什么？为什么它对推流至关重要？

<details>
<summary>🔑 参考答案</summary>

1. 裸流没有容器，缺时间戳、SPS/PPS 索引、包边界、time_base。要自己：①按帧率生成PTS/DTS ②提取SPS/PPS到extradata ③按协议设time_base。
2. RTSP底层是RTP，RTP标准时钟90kHz。设成1/fps服务器识别不了时间戳，会拒绝publish（连接建立但流无效，拉流端404）。
3. FLV要求第一个视频Tag是AVC sequence header（含SPS/PPS），解码器没有SPS/PPS解不开IDR。FFmpeg在avformat_write_header时自动用extradata生成这个header。
4. 时间戳不单调递增（后面的DTS≤前面）。我用1/fps time_base时，av_packet_rescale_ts把所有帧的pts压成相同值，导致DTS重复。
5. RTMP URL必须两段：rtmp://host:port/<app>/<stream>。只有一段时stream为空，服务器找不到流。
6. extradata装SPS/PPS（编码参数集）。它是解码器初始化的必需品，也是FLV sequence header的数据源，没有它推流和拉流都解不开。

</details>

---

## 📚 下一步

- ✅ Step 1 探测
- ✅ Step 2 解码
- ✅ Step 3 MP4推流
- ✅ Step 3b **H.264裸流推流**（RTMP+RTSP双协议）← 本节
- ⏭️ Step 4 拉流器（地面端接收）
