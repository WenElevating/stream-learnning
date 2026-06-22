# W3/W4/W5 Lab · 编解码原理实验

> 配套笔记：`notes/W3-W5-编解码原理.md`
> 本 lab 包含 4 类实操：(A) I/P/B 帧大小 (B) GOP 调优 (C) NALU 解析 (D) H.264 vs H.265

---

## A. 观察 I/P/B 帧大小差异

### A1. 生成无 B 帧 vs 有 B 帧视频

```bash
# 无B帧
ffmpeg -y -f lavfi -i testsrc=duration=3:size=640x360:rate=30 \
  -c:v libx264 -bf 0 -g 30 -pix_fmt yuv420p w3_no_bf.mp4

# 有B帧
ffmpeg -y -f lavfi -i testsrc=duration=3:size=640x360:rate=30 \
  -c:v libx264 -bf 2 -g 30 -pix_fmt yuv420p w3_with_bf.mp4
```

### A2. 逐帧大小

```bash
ffprobe -v error -select_streams v -show_entries packet=size,flags -of csv w3_no_bf.mp4 | head -10
ffprobe -v error -select_streams v -show_entries packet=size,flags -of csv w3_with_bf.mp4 | head -10
```

**期望观察**：
- I 帧（K__ 标志）≈ 5000 字节
- P 帧 ≈ 300 字节
- B 帧 ≈ 50 字节
- 比例 I:P:B ≈ 100:10:1

---

## B. GOP 长度调优实验

### B1. 同源不同 GOP 码率对比

```bash
for gop in 1 10 30 60 300; do
  ffmpeg -y -f lavfi -i testsrc=duration=10:size=640x360:rate=30 \
    -c:v libx264 -g $gop -bf 0 -pix_fmt yuv420p -an w3_gop${gop}.mp4 2>&1 | grep "kb/s"
done
```

**期望**：GOP=1 约 945kbps, GOP=30 约 111kbps, GOP=300 约 79kbps

### B2. 思考题

为什么 GOP=1→30 码率降 8.5 倍，但 GOP=30→300 只降 30%？
（答：30 帧内大部分是 P 帧，I 帧只占 1/30；GOP=1 全是 I 帧，全是 I 帧）

---

## C. ⭐ NALU 解析（最重要的实验）

### C1. 生成 H.264 裸流

```bash
ffmpeg -y -f lavfi -i testsrc=duration=2:size=640x360:rate=30 \
  -c:v libx264 -g 30 -bf 0 -pix_fmt yuv420p -f h264 w3_raw.h264
```

### C2. Python 解析 NALU

```python
import re
with open('w3_raw.h264','rb') as f:
    data = f.read()

# 同时匹配 3字节和4字节起始码
matches = []
i = 0
while i < len(data) - 3:
    if data[i:i+3] == b'\x00\x00\x01':
        start = i-1 if (i > 0 and data[i-1] == 0) else i
        matches.append(start)
        i += 3
    else:
        i += 1
matches = sorted(set(matches))
matches.append(len(data))

type_names = {1:'P/B帧', 5:'IDR(I帧)', 6:'SEI', 7:'SPS', 8:'PPS', 9:'AUD'}
print(f'共 {len(matches)-1} 个 NALU')
print(f'{"#":<5}{"偏移":<8}{"大小":<8}{"首字节":<10}{"Type":<6}{"名称":<15}')
for i in range(min(10, len(matches)-1)):
    start, end = matches[i], matches[i+1]
    sc_len = 4 if data[start:start+4]==b'\x00\x00\x00\x01' else 3
    nalu = data[start+sc_len:end]
    if not nalu: continue
    b0 = nalu[0]
    nal_type, ref = b0 & 0x1F, (b0>>5)&3
    print(f'{i:<5}{start:<8}{end-start:<8}0x{b0:02x}      {nal_type:<6}{type_names.get(nal_type,"?"):<15}')
```

### C3. SPS 字节解码

```python
sps = bytes.fromhex('67 64 00 1e'.replace(' ',''))
profiles = {66:'Baseline', 77:'Main', 100:'High'}
print(f'profile_idc = {sps[1]} = {profiles.get(sps[1], "?")}')
print(f'level_idc   = {sps[3]} = Level {sps[3]/10}')
```

---

## D. H.264 vs H.265 压缩效率

```bash
# 同 CRF=23 同源
ffmpeg -y -f lavfi -i testsrc=duration=5:size=1280x720:rate=30 \
  -c:v libx264 -crf 23 -pix_fmt yuv420p w5_h264.mp4
ffmpeg -y -f lavfi -i testsrc=duration=5:size=1280x720:rate=30 \
  -c:v libx265 -crf 23 -pix_fmt yuv420p w5_h265.mp4
```

**期望**：H.265 比 H.264 省 30% 左右。

---

## E. 码率控制对比

```bash
# CBR
ffmpeg -y -f lavfi -i mandelbrot=s=640x360:r=30:end_pts=300 -t 10 \
  -c:v libx264 -b:v 1M -minrate 1M -maxrate 1M -bufsize 2M -bf 0 -pix_fmt yuv420p w3_cbr.mp4

# CRF 18 vs 28
ffmpeg -y -f lavfi -i mandelbrot=s=640x360:r=30:end_pts=300 -t 10 \
  -c:v libx264 -crf 18 -bf 0 -pix_fmt yuv420p w3_crf18.mp4
ffmpeg -y -f lavfi -i mandelbrot=s=640x360:r=30:end_pts=300 -t 10 \
  -c:v libx264 -crf 28 -bf 0 -pix_fmt yuv420p w3_crf28.mp4
```

**期望**：CBR 约 1Mbps，CRF18 约 3.4Mbps，CRF28 约 1Mbps

---

## 🎯 练习任务

1. 用 C2 的 Python 脚本解析 `w3_raw.h264`，统计每种 NALU 类型的数量和总字节占比
2. 修改 `-bf 0` 为 `-bf 2`，重新生成裸流，对比 NALU 首字节分布（B 帧首字节多为 0x01）
3. 用 `-crf 18/23/28/35` 编码同一段视频，画出 CRF vs 文件大小曲线
4. 找一个你以前见过的 .h264/.h265 文件，用脚本解析它的 SPS，反推 profile/level/分辨率

## ✅ 验收标准

1. 能从一段 H.264 裸流的二进制里手动找出 SPS/PPS/IDR
2. 能解释为什么无人机用 `-bf 0 -g 30`，每个参数对应哪个原理
3. 能用 ffprobe + Python 验证 I:P:B 帧大小比例
4. 能解释 H.264 vs H.265 的核心差异和无人机选型考虑
