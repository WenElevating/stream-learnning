# W1 Lab 1 · 用 ffprobe 拆解一个视频文件

> 目标：掌握"生成测试视频 → ffprobe 探测 → 逐字段解读"的标准流程。
> 这是以后分析任何码流（包括你手头的推流输出）的通用动作。

## 环境

- FFmpeg 7.1（已确认：`D:\FFmpeg\ffmpeg-n7.1-latest-win64-gpl-shared-7.1\bin\`）

## 步骤 1：生成测试视频

```bash
ffmpeg -y -f lavfi -i testsrc=duration=3:size=640x360:rate=30 \
  -c:v libx264 -pix_fmt yuv420p w1_sample.mp4
```

参数解释：
- `-f lavfi -i testsrc=...`：用 FFmpeg 内置的测试图源生成画面（彩色条纹+计时器）
- `duration=3`：3 秒
- `size=640x360`：分辨率
- `rate=30`：30 fps
- `-c:v libx264`：视频编码器用 x264（H.264 的开源实现）
- `-pix_fmt yuv420p`：⭐ 强制像素格式为 YUV420P（编码器默认就是它，显式写更清楚）

## 步骤 2：探测结构

```bash
ffprobe -v error -show_format -show_streams w1_sample.mp4
```

- `-v error`：只显示错误，不要冗余日志
- `-show_format`：显示容器信息
- `-show_streams`：显示每一路流

## 步骤 3：逐字段对照（核心）

### 容器层 [FORMAT]
| 字段 | 值 | 含义 |
|---|---|---|
| format_name | mov,mp4,... | 封装格式 MP4 |
| nb_streams | 1 | 1 路流（无音频） |
| bit_rate | 61373 bps | 容器码率（含封装开销） |

### 流层 [STREAM]
| 字段 | 值 | 含义 |
|---|---|---|
| codec_name | h264 | 编码格式 |
| width × height | 640×360 | 分辨率 |
| pix_fmt | yuv420p | ⭐ 像素格式 |
| r_frame_rate | 30/1 | 帧率 |
| nb_frames | 90 | 总帧数 |
| bit_rate | 56160 bps | 流码率 |
| has_b_frames | 2 | 有 B 帧 → 需要 DTS/PTS |

## 步骤 4：三组验证

### ① 码率两个值
```
流码率 56160  <  容器码率 61373
差值 ≈ 封装开销
```

### ② YUV420P 的压缩效果
```
未压缩 3 秒（YUV420）：640×360×1.5×30×3 ≈ 93.3 MB
编码后：              23 KB
压缩比：              ≈ 4000:1
```

### ③ B 帧印证时间戳
```
has_b_frames=2 → 解码顺序 ≠ 显示顺序 → 需要 DTS 和 PTS
```

---

## 🎯 你的练习任务（必做）

把以下命令依次跑一遍，**把每个输出截图或抄到 notes/W1笔记.md**：

1. 改变分辨率和码率，观察 bit_rate 变化：
   ```bash
   ffmpeg -y -f lavfi -i testsrc=duration=3:size=1280x720:rate=30 \
     -c:v libx264 -b:v 2M w1_720p.mp4
   ```
   - `-b:v 2M`：强制视频码率 2Mbps
   - 探测后对比 w1_sample.mp4，bit_rate 应该接近 2Mbps

2. 改变像素格式，看体积差异：
   ```bash
   ffmpeg -y -f lavfi -i testsrc=duration=3:size=640x360:rate=30 \
     -c:v libx264 -pix_fmt yuv444p w1_yuv444.mp4
   ```
   - 对比 w1_sample.mp4（yuv420p）的 size 和 bit_rate
   - 思考：为什么无人机编码默认用 yuv420p 而不是 yuv444p？

3. 只看关键摘要（更清爽的输出）：
   ```bash
   ffprobe -v error -show_entries stream=codec_name,width,height,pix_fmt,r_frame_rate,bit_rate \
     -of default=noprint_wrappers=1 w1_sample.mp4
   ```

## ✅ 验收标准

能不看笔记，口头回答：
1. 容器、流、编码 三者的关系（用快递箱类比）
2. 为什么摄像头出 YUV 不出 RGB？YUV420 比 RGB 省多少？
3. ffprobe 输出里哪三个字段分别对应：分辨率、帧率、码率？
4. has_b_frames=2 意味着什么？为什么需要 DTS 和 PTS？

能答上来 → W1 第一部分过关，进入 PTS/DTS 与音视频同步（W2 内容）。
