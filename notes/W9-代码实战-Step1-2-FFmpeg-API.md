# W9 代码实战 · Step 1-2 (FFmpeg API 探测 + 解码)

> 阶段：方案A / 阶段4 FFmpeg 框架实战（提前到这里练手）
> 这是从"会用 ffprobe/ffmpeg 命令"升级到"会用 C++ API 写程序"的关键一步
> 状态：✅ Step1 探测、Step2 解码 均已跑通

---

## 📌 本节一句话总结

FFmpeg API 编程三件套：`avformat_open_input`(打开) → `avcodec_send/receive`(解码) → `av_read_frame`(读包)；编译用 `cmd.exe` 调 g++（Git Bash 会吞 g++ 子进程 stderr），运行时要把 FFmpeg 的 bin 加进 PATH 找 DLL。

---

## 1. 开发环境（Windows / MinGW）

### 工具链
- 编译器：`g++ 15.2.0` (MSYS2 UCRT64, `D:\msys64\ucrt64\bin`)
- FFmpeg：`D:\FFmpeg\ffmpeg-n7.1-latest-win64-gpl-shared-7.1`
  - `include\` 头文件
  - `lib\libavcodec.dll.a` 等（g++ 链接用 `lib*.dll.a`，不是 MSVC 的 `.lib`）
  - `bin\avcodec-61.dll` 等（运行时需要，要加进 PATH）

### ⚠️ 踩坑记录（重要）

| 坑 | 现象 | 根因 | 解决 |
|---|---|---|---|
| **g++ 静默失败** | EXIT=1 无任何输出 | Git Bash 和 MinGW g++ 的子进程 stderr 冲突 | **所有编译走 `cmd.exe //C`** |
| DLL 找不到 | 运行 exe 报缺 dll | exe 依赖运行时 DLL | 运行时 PATH 加 FFmpeg bin |
| `av_get_pix_fmt_name` 未定义 | 编译报错 | FFmpeg 7.x API 变动 | 用 `av_pix_fmt_desc_get()->name` |
| .bat 中文乱码 | build.bat 执行报错 | BOM + bash 解析 | 改用直接 g++ 命令 |

### 编译命令模板（务必用 cmd.exe）
```bat
set PATH=D:\msys64\ucrt64\bin;%PATH%
set FF=D:\FFmpeg\ffmpeg-n7.1-latest-win64-gpl-shared-7.1
g++ 源文件.cpp -o build\输出.exe ^
    -I%FF%\include -L%FF%\lib ^
    -lavformat -lavcodec -lavutil ^
    -std=c++17
```
运行时：
```bat
set PATH=%FF%\bin;%PATH%
build\输出.exe 参数
```

---

## 2. Step 1：打开视频打印流信息（`01_probe.cpp`）

### 核心 API 流程

```
avformat_open_input()      ← 打开文件，识别容器格式（MP4/FLV/TS）
        ↓
avformat_find_stream_info() ← 预读补全信息（FLV 等需要）
        ↓
遍历 fmt_ctx->streams[]
        ↓
读 stream->codecpar (AVCodecParameters)
  → codec_type (视频/音频)
  → codec_id   (h264/aac)
  → width/height, pix_fmt, profile/level
  → bit_rate, time_base
```

### 关键数据结构

| 结构 | 作用 | 对应 ffprobe |
|---|---|---|
| `AVFormatContext` | 容器（快递箱） | FORMAT 块 |
| `AVStream` | 一路流（物品） | STREAM 块 |
| `AVCodecParameters` | 编码参数（压缩袋） | codec_name/width/pix_fmt 等 |

### 实测验证
用 `01_probe.exe ..\labs\w1_av.mp4` 输出与 W1 的 ffprobe 完全一致：
- 容器 mov,mp4 + 时长 3.0s + 2 路流
- 视频 h264 640x360 yuv420p High@3.0
- 视频 time_base 1/15360，音频 1/44100

---

## 3. ⭐ Step 2：解码一帧（`02_decode.cpp`）

### 解码的本质

```
AVPacket (压缩包)              AVFrame (原始画面)
┌──────────────┐   解码器     ┌──────────────────┐
│ H.264 NALU    │ ──────→     │ YUV 像素           │
│ ~300字节(P帧) │             │ 345600字节(640×360)│
└──────────────┘             └──────────────────┘
   有 pts/dts/flags              有 pts/format/data[3]
```

### 解码循环套路（FFmpeg 5.0+ API）

```
while (av_read_frame(fmt_ctx, pkt) >= 0) {     // 读压缩包
    if (pkt->stream_index != video_idx) continue;

    avcodec_send_packet(codec_ctx, pkt);        // 喂给解码器(可能不解码,先缓存)
    av_packet_unref(pkt);

    while (true) {                              // ⭐ receive 必须循环!
        ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == EAGAIN) break;               // 解码器要更多包
        if (ret < 0) break;
        // 拿到一帧! 处理它
        av_frame_unref(frame);
    }
}
```

### ⚠️ 新手必踩的坑：send 和 receive 不是 1:1

```
因为有 B 帧重排（W3）:
  解码器内部有缓冲区
  可能 send 好几个包才 receive 出一帧
  所以 receive 必须放 while 循环, 直到 EAGAIN
```

### 解码器初始化（三步）

```c
const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);  // 找解码器
AVCodecContext* ctx = avcodec_alloc_context3(codec);               // 分配上下文
avcodec_parameters_to_context(ctx, codecpar);  // ⭐ 把SPS/PPS喂给解码器!
avcodec_open2(ctx, codec, nullptr);           // 真正打开
```
> `avcodec_parameters_to_context` 这一步会把 W4 讲的 SPS/PPS 传给解码器，没有它解不开。

### AVFrame 的 YUV 数据布局（YUV420P）

```
frame->data[0]     → Y 平面指针 (width × height)
frame->data[1]     → U 平面指针 (width/2 × height/2)
frame->data[2]     → V 平面指针 (width/2 × height/2)
frame->linesize[0] → Y 每行字节数(可能 > width, 有 padding)
frame->linesize[1] → U 每行字节数
frame->linesize[2] → V 每行字节数
```
写 YUV 文件要按 plane 写，注意 linesize 可能 ≠ width！

### 实测验证（`w1_av.mp4` 解码）

```
解出帧序: I → B → B  ← 印证 W3: 第一帧必是I, B帧在缓冲后才出
每帧YUV: 345600 字节 = 640×360×1.5 ✅ 对应W1公式
文件大小: 1036800 = 345600 × 3 ✅

压缩对比:
  H.264 P帧: ~300字节 → YUV: 345600字节, 解压膨胀 ~1152×
```

---

## 4. 已掌握的 API 清单

### Step 1（探测）
| API | 作用 |
|---|---|
| `avformat_open_input` | 打开文件/URL，识别容器 |
| `avformat_find_stream_info` | 预读补全流信息 |
| `av_strerror` | 错误码转文字 |
| `avio_size` | 文件大小 |

### Step 2（解码）
| API | 作用 |
|---|---|
| `av_find_best_stream` | 自动找指定类型的最佳流 |
| `avcodec_find_decoder` | 根据 codec_id 找解码器 |
| `avcodec_alloc_context3` | 分配解码器上下文 |
| `avcodec_parameters_to_context` | 把编码参数喂给解码器 |
| `avcodec_open2` | 打开解码器 |
| `av_read_frame` | 从文件读一个压缩包 |
| `avcodec_send_packet` | 把包喂给解码器 |
| `avcodec_receive_frame` | 取出解码后的帧 |
| `av_packet_alloc/free` | 包的分配/释放 |
| `av_frame_alloc/free` | 帧的分配/释放 |
| `av_image_get_buffer_size` | 算 YUV 帧字节数 |
| `av_packet_unref/frame_unref` | 释放引用(复用结构) |

---

## 5. FFmpeg 编程的"内存管理心法"

```
所有 alloc 都要配对 free:
  av_packet_alloc  ↔ av_packet_free
  av_frame_alloc   ↔ av_frame_free
  avcodec_alloc_context3 ↔ avcodec_free_context
  avformat_open_input    ↔ avformat_close_input

中间复用要 unref(不释放结构, 只清内容):
  av_packet_unref(pkt)   ← 喂完包就调用, 复用 pkt 结构
  av_frame_unref(frame)  ← 处理完帧就调用, 复用 frame 结构

顺序: 先打开的最后关 (栈式)
  open: fmt_ctx → codec_ctx
  close: codec_ctx → fmt_ctx
```

---

## ✅ 自检问题

1. FFmpeg 是 C 库，C++ 引入头文件要注意什么？
2. 为什么 `avcodec_send_packet` 和 `avcodec_receive_frame` 不是 1:1 调用？
3. `avcodec_parameters_to_context` 这一步做了什么？为什么不能省？
4. YUV420P 一帧的 `frame->data[0/1/2]` 各是什么？为什么 U/V 比 Y 小？
5. 640×360 的 YUV420P 一帧多少字节？怎么算的？
6. 为什么编译必须用 `cmd.exe` 而不能直接在 Git Bash 里 `g++`？

<details>
<summary>🔑 参考答案</summary>

1. 必须用 `extern "C" {}` 包裹，否则 C++ 的 name mangling 会导致链接报 `undefined reference`。
2. B 帧重排。解码器内部有缓冲区，可能 send 好几个包才 receive 出一帧（要等参考帧就位）。所以 receive 要放 while 循环直到 EAGAIN。
3. 把流里的编码参数（SPS/PPS/profile/分辨率）拷进解码器上下文。没有 SPS/PPS 解码器根本解不开 I 帧。
4. data[0]=Y 平面（全分辨率），data[1]=U，data[2]=V。YUV420 色度降采样，U/V 横竖各减半，所以是 Y 的 1/4。
5. 640×360×1.5 = 345600 字节。Y=640×360，U=V=320×180，合计 230400 + 57600 + 57600 = 345600。
6. Git Bash（基于 MSYS2）和 MinGW UCRT64 的 g++ 子进程 stderr 处理冲突，导致 g++ 误判失败、静默退出码 1。用 cmd.exe 调用则正常。

</details>

---

## 📚 下一步

- ✅ Step 1 探测
- ✅ Step 2 解码 ← 本节
- ⏭️ Step 3：最小推流器（读文件 → 推 RTSP，对标无人机载荷推流）
