# 播放器 Step 1 —— 最小可视播放器 (SDL 显示 YUV)

> 代码：`code/player/step1_minimal_player.cpp`
> 测试片：`labs/w1_sample.mp4` (640×360, H.264, YUV420P, 30fps, 3s)
> 已验证：MP4 / FLV(1080p) / TS 三种容器均播放成功

---

## 一、本步目标

**用最少的代码让画面动起来**。刻意省略：
- ❌ 多线程（Step3）
- ❌ 音频（Step4）
- ❌ A/V 同步（Step4）
- ❌ IO 抽象（Step2）
- ❌ 丢帧/背压（Step3）

保留的核心：**FFmpeg 解码 → SDL 显示 YUV**。

---

## 二、整体流程（单线程流水线）

```
avformat_open_input      ┐
avformat_find_stream_info│ FFmpeg 打开输入(5步仪式)
找 videoIdx              │
avcodec_find_decoder     │
avcodec_alloc/open2      ┘
        │
        ▼
┌─── 主循环 (每帧一圈) ──────────────────────┐
│ av_read_frame        读一个包              │
│ avcodec_send_packet  送进解码器            │
│ avcodec_receive_frame 取出解码后的YUV帧    │
│ SDL_UpdateYUVTexture 上传到纹理            │
│ SDL_RenderCopy/Present 显示                │
│ SDL_Delay(33)        固定延时(假设30fps)   │
│ SDL_PollEvent        处理ESC/关窗口        │
└────────────────────────────────────────────┘
```

---

## 三、四个核心知识点

### 知识点 1：SDL 的三件套 —— Window / Renderer / Texture

SDL2 的显示模型是三层：

```
SDL_Window      一个操作系统窗口 (有标题、位置、大小)
   │
   └─ SDL_Renderer   渲染器 (绑定到窗口, 负责实际绘制, 可硬件加速)
          │
          └─ SDL_Texture   纹理 (存放一帧图像的像素数据, GPU上传)
```

显示一帧的标准四步：
```cpp
SDL_RenderClear(renderer);                              // 1. 清屏
SDL_RenderCopy(renderer, texture, NULL, NULL);          // 2. 把纹理拷到渲染目标
SDL_RenderPresent(renderer);                            // 3. 翻页 (后台缓冲→屏幕)
// texture 在这之前用 SDL_UpdateYUVTexture 更新过像素   // 4. 数据来源
```

> **为什么有 RenderPresent（翻页）？**
> SDL 用**双缓冲**：你画到后台缓冲，Present 时一次性翻到屏幕。
> 这样用户看不到"画一半"的中间状态，避免闪烁。

### 知识点 2 ★：纹理格式 IYUV == FFmpeg 的 YUV420P（零拷贝）

这是 Step1 最重要的决策：

```cpp
texture = SDL_CreateTexture(renderer,
    SDL_PIXELFORMAT_IYUV,          // ★ 关键
    SDL_TEXTUREACCESS_STREAMING,   // 每帧更新
    width, height);
```

**为什么用 `SDL_PIXELFORMAT_IYUV`？**

| 格式 | 内存布局 | 对应 FFmpeg |
|------|---------|------------|
| `SDL_PIXELFORMAT_IYUV` | I420: Y/U/V 三平面分开存 | `AV_PIX_FMT_YUV420P` ✅ |
| `SDL_PIXELFORMAT_RGB24` | RGB 打包存 | 需 `sws_scale` 转 |

**IYUV 和 YUV420P 内存布局完全一致**：
- Y 平面：`width × height` 字节
- U 平面：`(width/2) × (height/2)` 字节
- V 平面：`(width/2) × (height/2)` 字节
- 总计：`width × height × 1.5` 字节

→ 解码出来的 `AVFrame->data[0/1/2]` 可以**直接喂给 SDL**，**跳过 `sws_scale`**。
- 省一次 CPU 拷贝和色彩转换
- 色彩转换由 GPU 在显示时做（免费）
- 这是你 W1 学的 YUV420P 理论的直接应用

### 知识点 3：`SDL_UpdateYUVTexture` —— 三平面分别喂

```cpp
SDL_UpdateYUVTexture(texture, nullptr,
    frame->data[0], frame->linesize[0],   // Y 平面数据 + 每行字节数
    frame->data[1], frame->linesize[1],   // U 平面
    frame->data[2], frame->linesize[2]);  // V 平面
```

**踩坑提醒**：
- ❌ 不要用 `SDL_UpdateTexture` —— 它只处理**打包格式**（RGB/BGR），
  对 planar YUV 不工作（虽然有些文档暗示可以，实际不行）
- ✅ 必须用 `SDL_UpdateYUVTexture`，分别传三个平面

**为什么需要 `linesize`？**
FFmpeg 解码出来的行可能有**填充字节**（为了内存对齐，比如宽 640 但 linesize 可能是 672）。
所以传"每行实际字节数"（linesize），而不是简单的 `width`。SDL 据此正确步进。

### 知识点 4：FFmpeg 7.x 解码 API —— send/receive 循环

```cpp
// 送包到解码器
ret = avcodec_send_packet(codecCtx, pkt);
// 返回值:
//   0        = 成功接收, 可以去取帧了
//   EAGAIN   = 解码器内部满, 先 receive_frame 把帧取出来
//   AVERROR_EOF = 解码器已冲洗, 不再接收
//   其他负值 = 真错误

// 取解码后的帧
while (avcodec_receive_frame(codecCtx, frame) == 0) {
    // 一个 packet 可能产生 0 或多帧:
    //   - 0帧: B帧重排序还没凑齐, 这次的包"欠"了帧
    //   - 1帧: 普通情况
    //   - 多帧: 之前的B帧凑齐了一起重排出来
    // 显示这一帧...
    av_frame_unref(frame);   // ★ 必须 unref, 否则内存泄漏
}
```

**关键概念**：send/receive 是**解耦**的 —— 你送进去的包和取出来的帧**不是一一对应**的。
这和 B 帧重排序有关（W1 笔记）：解码顺序 ≠ 显示顺序，解码器内部会缓存帧。

**EAGAIN 不是错误** —— 它只表示"我现在给不了你帧，再送个包来"。

---

## 四、踩到的坑（都已解决）

### 坑 1：`fmtCtx->streams.size()` 编译错误
- **现象**：`request for member 'size' which is of non-class type 'AVStream**'`
- **原因**：FFmpeg 是 C 库，`streams` 是 C 风格指针数组，没有 `.size()`
- **修复**：用 `fmtCtx->nb_streams` 获取元素个数

### 坑 2：`goto` 跨越变量初始化
- **现象**：`jump crosses initialization of 'AVFrame* frame'`
- **原因**：C++ 规定 `goto` 不能跳过带初始值的变量声明
- **修复**：把所有 SDL/FFmpeg 句柄**前置声明**到 goto 之前（`SDL_Window* window = nullptr;`），
  初始化放在后面。这是 C++ 错误处理的经典套路（当你想用 goto 做统一 cleanup 时）

### 坑 3：`undefined reference to 'WinMain'`
- **现象**：链接器找不到程序入口
- **原因**：SDL2 在 Windows 上把 `main` 宏重定义成了 `SDL_main`，
  导致你的 `int main(...)` 实际变成了 `int SDL_main(...)`，链接器找不到真正的 `main`
- **修复**：在 `#include <SDL.h>` **之前**定义 `SDL_MAIN_HANDLED`：
  ```cpp
  #define SDL_MAIN_HANDLED   // 禁用 SDL 的 main 宏重定义
  #include <SDL.h>
  ```
  这样 `main` 保持标准签名，不需要链接 `libSDL2main`，也不需要 `SDLmain.lib`。
  这是最干净的方式（适合控制台程序嵌入 SDL）。

---

## 五、本步的简化（后面要解决的）

| 简化点 | 后果 | 哪一步解决 |
|--------|------|----------|
| `SDL_Delay(33)` 固定延时 | 非 30fps 视频播放速度不准 | Step4（按 PTS） |
| 单线程 | 4K/高码率会卡顿 | Step3（多线程） |
| 无音频 | 一半体验缺失 | Step4 |
| `av_read_frame` 阻塞 EOF 直接退出 | 最后几帧可能丢 | Step3（送 NULL 包冲洗解码器） |
| 内存直接用文件 URL | IO 没抽象 | Step2（IOSource） |

---

## 六、关键代码索引（方便复习时定位）

| 功能 | 代码位置 |
|------|---------|
| FFmpeg 打开输入 5 步 | `step1_minimal_player.cpp` 第一阶段 |
| SDL 三件套创建 | 第二阶段 [A][B][C] |
| IYUV 纹理创建 | 第二阶段 [D] |
| YUV 上传 | 第三阶段 3.4 `SDL_UpdateYUVTexture` |
| send/receive 解码循环 | 第三阶段 3.2/3.3 |
| 固定延时 | 第三阶段 3.6 `SDL_Delay(33)` |

---

## 七、自测题（读完应该能答）

1. 为什么纹理用 `SDL_PIXELFORMAT_IYUV` 而不是 `RGB24`？
   → IYUV 内存布局和 FFmpeg 的 YUV420P 完全一致，可直接喂，跳过 sws_scale，GPU 做色彩转换，省 CPU。
2. `SDL_UpdateTexture` 和 `SDL_UpdateYUVTexture` 有什么区别？
   → 前者处理打包格式（RGB），后者处理 planar YUV 的三个平面，必须用后者。
3. `avcodec_send_packet` 返回 `EAGAIN` 表示什么？该怎么处理？
   → 解码器内部缓冲满，需要先 `avcodec_receive_frame` 把帧取出来再重试。EAGAIN 不是错误。
4. 为什么一个 packet 可能解出多帧？
   → B 帧重排序：解码器内部缓存帧，凑齐后一起输出，所以 send/receive 不是一一对应。
5. `av_frame_unref(frame)` 必须在什么时候调用？不调会怎样？
   → 每次用完一帧后立即调。不调会导致 `av_frame_alloc` 的引用计数不归零，内存泄漏。
6. 为什么用 `SDL_MAIN_HANDLED`？
   → 禁用 SDL 在 Windows 上对 `main` 的宏重定义，避免链接器找不到 `WinMain`。

---

## 八、下一步（Step 2 预告）

Step1 的代码是"面条代码"——所有逻辑挤在 main 里。Step2 会把它拆成模块：

```
IOSource (抽象接口)  ← 新增, 这是"IO 可替换"的关键
   ├─ open/read/seek/close 四个虚函数
   └─ Demuxer 通过 avio_alloc_context 接它
Demuxer     ← 持有 IOSource + AVFormatContext
Decoder     ← 一个类, 封装 send/receive
Renderer    ← 封装 SDL 三件套 + UpdateYUVTexture
```

拆完后，**换 IO 后端（文件↔内存↔网络）不用动 Demuxer/Decoder/Renderer 一行代码**。
