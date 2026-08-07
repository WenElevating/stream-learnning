# 播放器 Step 2 —— 模块化拆分 + IO 抽象接口

> 代码：`code/player/` 下 7 个文件
> - `IOSource.h` / `IOFile.h` —— IO 抽象 + 文件实现
> - `Demuxer.h/.cpp` / `Decoder.h/.cpp` / `VideoRenderer.h/.cpp` —— 三大模块
> - `step2_modular_player.cpp` —— main 组装
>
> 验证：MP4 / MP4带音轨 / FLV(1080p) / TS 全部播放成功，行为与 Step1 一致

---

## 一、本步做了什么（对比 Step1）

**功能完全没变**（单线程、固定 30fps、只视频），但代码从 Step1 的"面条代码"重构为 **4 个职责隔离的模块**：

```
Step1: 全部逻辑挤在 main() 里 200+ 行
Step2: 拆成 IOSource / Demuxer / Decoder / VideoRenderer, main 只负责组装
```

核心新增：**`IOSource` 抽象接口** —— 这就是你说的"IO 可替换"的接缝。Demuxer 不再直接 fopen 文件，而是通过 `avio_alloc_context` 接住任意 `IOSource` 实现。

---

## 二、架构图（Step2 的核心成果）

```
      ┌──────────────────────────────────────────┐
      │        IOSource (抽象接口)                 │  ← "IO 可替换"的接缝
      │   open / read / seek / close / size       │
      └──────────────────┬───────────────────────┘
                         │ (多态: 文件/内存/网络 任意实现)
                         ▼
                   IOFile (本步实现)
                         │ 字节流
            ┌────────────┴────────────┐
            │  avio_alloc_context     │  ← C 回调桥接层
            │  (注册 read/seek 回调)   │
            └────────────┬────────────┘
                         ▼
   ┌─────────────── Demuxer ───────────────┐
   │ 持有 IOSource* + AVFormatContext       │
   │ open() → readPacket() → streamInfo()  │
   └───────────────────┬───────────────────┘
                       │ AVPacket
                       ▼
   ┌─────────────── Decoder ───────────────┐
   │ open(params) → sendPacket/receiveFrame│
   └───────────────────┬───────────────────┘
                       │ AVFrame (YUV420P)
                       ▼
   ┌──────────── VideoRenderer ────────────┐
   │ SDL 三件套 + UpdateYUVTexture         │
   └───────────────────────────────────────┘
```

**职责边界（每个模块的"合同"）：**

| 模块 | 只知道 | 绝不碰 |
|------|--------|--------|
| IOSource | 字节从哪来 | 容器、编码、显示 |
| Demuxer | 容器格式 + IOSource 接口 | 解码、显示 |
| Decoder | 一种编码格式 | 容器、显示、时钟 |
| VideoRenderer | SDL + AVFrame | 解封装、解码 |

→ **换 IO 不动 Demuxer，换显示后端不动 Decoder**。Step5 会验证这一点。

---

## 三、核心知识点

### 知识点 1 ★：`avio_alloc_context` —— FFmpeg 的 IO 抽象点

这是整个 Step2 最关键的 API。FFmpeg 默认通过 URL 或文件路径读数据，但 `avio_alloc_context` 让你能**注入自定义字节流**：

```cpp
// 1. 分配缓冲区 (FFmpeg 对齐)
uint8_t* avioBuf = (uint8_t*)av_malloc(32 * 1024);

// 2. 创建 AVIOContext, 注册 C 回调
m_avio = avio_alloc_context(
    avioBuf,            // 缓冲
    32 * 1024,          // 缓冲大小
    0,                  // write_flag=0 (只读)
    m_io,               // opaque (传给回调的指针, 我们传 IOSource*)
    avioReadCallback,   // 读回调
    nullptr,            // 写回调 (player 不需要)
    avioSeekCallback);  // 定位回调

// 3. 挂到 FormatContext 上
m_fmt->pb = m_avio;
m_fmt->flags |= AVFMT_FLAG_CUSTOM_IO;   // 告诉 FFmpeg: pb 是我管的

// 4. open_input 时 url 传 nullptr (数据从 pb 来)
avformat_open_input(&m_fmt, nullptr, nullptr, nullptr);
```

**回调签名（FFmpeg 规定）**：
```cpp
int     read_callback(void* opaque, uint8_t* buf, int bufSize);   // 返回读到的字节数
int64_t seek_callback(void* opaque, int64_t offset, int whence);   // 返回新位置
```

回调里把 `opaque` 转回 `IOSource*`，转发它的虚函数。这样 FFmpeg 就在**完全不知情**的情况下，从我们的 IOSource 读字节。

### 知识点 2：`AVSEEK_SIZE` —— seek 回调的特殊询问

```cpp
int64_t seek(int64_t offset, int whence) {
    if (whence == AVSEEK_SIZE) {   // 0x10000, 特殊值
        return m_size;              // 不真的定位, 只是问"你多大"
    }
    // 其余 whence (0/1/2 = SEEK_SET/CUR/END) 才是真的定位
    fseek(m_fp, offset, whence);
    return ftell(m_fp);
}
```

**为什么重要**：MP4 的 `moov` atom 定位、FLV 的时长计算都需要知道总大小。如果 seek 回调对 `AVSEEK_SIZE` 返回错误，这些容器会退化甚至失败。

直播流（RTMP/RTSP）不可定位，seek 回调可以返回 `AVERROR(ENOSYS)` 或干脆传 `nullptr`。

### 知识点 3 ★★：EOF 必须返回 `AVERROR_EOF`，不能返回 0（本步最大的坑）

这是调试最久的点。最初 IOSource.read() 在 EOF 时返回 0（按 fread 的约定），结果：

```
[avio][read] #3  want=32768 got=0       ← EOF
[avio][read] #4  want=32768 got=0       ← FFmpeg 重试
[avio][read] #5  want=32768 got=0
... (1300+ 次全是 0, 死循环)
```

FLV demuxer 在 `find_stream_info` 时，把 read 返回 0 理解成"流还在继续，只是暂时没数据"，于是疯狂重试。MP4/TS 没这个问题是因为它们内部有 EOF 检测机制，但 FLV 尤其敏感。

**修复**：EOF 返回 `AVERROR_EOF`（一个负数常量）：

```cpp
if (n == 0 && feof(m_fp)) {
    return AVERROR_EOF;   // ★ 不是 0!
}
```

修复后 trace 立刻正常：
```
[avio][read] #3  got=-541478725   ← AVERROR_EOF
[avio][seek] SIZE -> 71834        ← FFmpeg 知道结束了, 改去问大小
find_stream_info 返回 0            ← 成功
```

**教训**：FFmpeg 的 AVIO 回调有自己的一套错误码约定（负值），和标准 C 的 fread（0=EOF）不一样。给 FFmpeg 写回调必须按 FFmpeg 的约定来。

### 知识点 4：FFmpeg 子头文件没有 `extern "C"` 守卫

Step1 单文件时，所有 FFmpeg include 在一个大 `extern "C"` 块里，没问题。
Step2 拆多文件后，某些 `.h` 单独 include FFmpeg 的**子头文件**（如 `error.h`、`avio.h`、`pixdesc.h`、`frame.h`），导致链接错误：

```
undefined reference to `av_strerror(int, char*, unsigned long long)'
                              ↑ C++ name mangling 后的签名
```

**根因**：FFmpeg 只有**顶层头文件**（`avformat.h`/`avcodec.h`/`avutil.h`）有 `extern "C"` 守卫，**子头文件没有**。C++ 单独 include 子头时，会把 C 函数当 C++ 符号 mangle，链接时在 `.dll.a`（导出的是无修饰 C 符号）里找不到。

**修复**：任何单独 include FFmpeg 子头的地方，手动包 `extern "C"`：
```cpp
extern "C" {
#include <libavutil/error.h>
}
```

### 知识点 5：FFmpeg 7.x 的 `av_get_pix_fmt_name` 被移除

```cpp
// ❌ 7.x 编不过
av_get_pix_fmt_name(fmt)

// ✅ 7.x 正确写法
av_pix_fmt_desc_get(fmt)->name
```

需要 include `<libavutil/pixdesc.h>`（同样要 `extern "C"`）。

---

## 四、本步踩的坑汇总（都在笔记里了）

| 坑 | 现象 | 根因 | 修复 |
|----|------|------|------|
| `AVSEEK_SIZE` 未定义 | 编译错误 | `avio.h` 没 include | 加 include（带 extern "C"） |
| `av_get_pix_fmt_name` 未定义 | 编译错误 | 7.x 移除了它 | 用 `av_pix_fmt_desc_get()->name` |
| `av_strerror` undefined reference | 链接错误 | 子头文件无 extern "C" 守卫 | 手动包 extern "C" |
| FLV 在 find_stream_info 死循环 | 运行卡死 | read 返回 0 而非 AVERROR_EOF | EOF 返回 AVERROR_EOF |
| .bat 中文乱码 | cmd 执行失败 | UTF-8 文件 vs GBK 控制台 | 不用 .bat, 直接 cmd //C 内联 |

---

## 五、main 组装代码为何这么干净

对比 Step1 的 main（200 行 FFmpeg API 调用），Step2 的 main 核心只有：

```cpp
IOFile io;              io.open(filename);        // 1. IO
Demuxer demuxer;        demuxer.open(&io);        // 2. 解封装 (接 IOSource!)
Decoder decoder;        decoder.open(params);     // 3. 解码
VideoRenderer renderer; renderer.open(w, h);      // 4. 显示

// 主循环
while (demuxer.readPacket(pkt)) {
    decoder.sendPacket(pkt);
    while (decoder.receiveFrame(frame)) {
        renderer.show(frame);
    }
}
```

这就是**职责隔离的价值**：main 只描述"流水线怎么连"，每个模块内部怎么实现 main 不关心。

---

## 六、验证结果

| 测试片 | 格式 | 分辨率 | 结果 |
|--------|------|--------|------|
| w1_sample.mp4 | MP4 | 640×360 | ✅ 播放完毕 |
| w1_av.mp4 | MP4(带音轨) | 640×360 | ✅ 播放完毕（音频正确忽略） |
| w1.flv | FLV | **1920×1080** | ✅ 播放完毕（FLV 坑已修） |
| w1_bf.ts | TS | 640×360 | ✅ 播放完毕 |

注意：这些全部走的是 **IOSource → avio_alloc_context → FFmpeg** 这条自定义 IO 路径（不是 Step1 的直接 URL）。所以这个成功本身就证明了 **IO 抽象层工作正常**。

---

## 七、自测题

1. 为什么 Demuxer 不直接 `fopen` 文件，而要绕一层 IOSource？
   → 为了 IO 可替换。Demuxer 只依赖 IOSource 接口，换文件/内存/网络后端不用改 Demuxer。
2. `avio_alloc_context` 的 `opaque` 参数有什么用？
   → 它是传给 read/seek 回调的"上下文指针"。回调里把它转回 IOSource*，从而调用具体实现。这是 C 风格的"this 指针"。
3. read 回调在 EOF 时应该返回什么？返回 0 会怎样？
   → 必须返回 `AVERROR_EOF`（负值）。返回 0 会让 FLV 等 demuxer 误以为"暂时没数据"而死循环重试。
4. `AVSEEK_SIZE` 是什么？seek 回调为什么要处理它？
   → FFmpeg 用来询问总大小的特殊 whence 值（0x10000）。MP4/FLV 等容器需要知道总大小才能定位关键 box/算时长。
5. 为什么 FFmpeg 子头文件单独 include 时会链接失败？
   → FFmpeg 只有顶层头有 `extern "C"`，子头没有。C++ 会把 C 函数 mangle，导致在 `.dll.a` 里找不到符号。手动包 `extern "C"` 解决。
6. Step2 比 Step1 多了一层 IOSource 抽象，性能会变差吗？
   → 不会。虚函数调用 + 回调转发开销是纳秒级，相对磁盘 IO 和解码的微秒/毫秒级可忽略。抽象的价值远大于这点开销。

---

## 八、下一步（Step 3 预告）

Step2 仍是单线程：解码时画面停、显示时解码停。Step3 引入多线程：

```
read线程 → PacketQueue → decode线程 → FrameQueue → main线程渲染
```

这是所有真实播放器（含 ffplay）的骨架。Step3 完成后，4K@60fps 的视频也不会卡（多线程吸收了"解码耗时不均"的抖动）。
