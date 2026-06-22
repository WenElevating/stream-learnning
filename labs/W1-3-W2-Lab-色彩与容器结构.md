# W1-3 / W2 Lab · 色彩元数据探测 + 容器结构解析

> 配套笔记：`notes/W1-3-色彩空间与W2-容器格式.md`
> 本 lab 包含两类实操：(A) 色彩元数据探测 (B) 容器二进制结构解析

---

## A. 色彩元数据探测

### A1. 生成带明确色彩元数据的视频

```bash
ffmpeg -y -f lavfi -i testsrc=duration=3:size=1920x1080:rate=30 \
  -c:v libx264 -pix_fmt yuv420p \
  -color_range tv -colorspace bt709 \
  -color_trc bt709 -color_primaries bt709 \
  w1_color.mp4
```

参数：
- `-color_range tv`：Limited Range（16-235）
- `-colorspace bt709`：HD 色彩矩阵
- `-color_trc bt709`：传输函数
- `-color_primaries bt709`：色域

### A2. 探测所有色彩字段

```bash
ffprobe -v error -select_streams v -show_entries \
  stream=color_range,color_space,color_transfer,color_primaries,pix_fmt \
  -of default=noprint_wrappers=1 w1_color.mp4
```

⚠️ **注意坑**：x264 默认不写 transfer/primaries 进 MP4，可能显示 unknown。

### A3. 验证 Y 分量范围（确认 Limited/Full）

```bash
ffmpeg -y -i w1_color.mp4 -vf "signalstats,metadata=print:file=-" -f null - 2>&1 | grep -iE "YMIN|YMAX|YAVG" | head -3
```

期望：
- Limited Range：YMIN≈6~16，YMAX≈235~245
- Full Range：YMIN≈0，YMAX≈255

---

## B. MP4 容器结构解析

### B1. 解析顶层 box

```python
import struct
with open('w1_color.mp4','rb') as f:
    data = f.read()
offset = 0
while offset + 8 <= len(data):
    size, box_type = struct.unpack('>I4s', data[offset:offset+8])
    box_name = box_type.decode('ascii', errors='replace')
    if size == 0: size = len(data) - offset
    elif size == 1: size = struct.unpack('>Q', data[offset+8:offset+16])[0]
    if size < 8: break
    print(f'offset={offset:6d} size={size:6d} type={box_name!r}')
    offset += size
```

期望输出：
```
offset=     0 size=    32 type='ftyp'
offset=    32 size=     8 type='free'
offset=    40 size=69661 type='mdat'
offset=69701 size=  1911 type='moov'
```

### B2. 验证 moov 位置

看 moov 是在开头还是末尾。末尾 = 未优化（默认）。开头 = 用了 `-movflags +faststart`。

试一下对比：
```bash
# 未优化
ffmpeg -y -i w1_color.mp4 -c copy w1_nofast.mp4
# 优化（moov 移到开头）
ffmpeg -y -i w1_color.mp4 -c copy -movflags +faststart w1_fast.mp4
```
对比两个文件的 box 顺序，能看到 moov 位置不同。

---

## C. MPEG-TS 包结构解析

### C1. 验证 188 字节对齐

```python
with open('w1_bf.ts','rb') as f:
    data = f.read()
print(f'文件大小: {len(data)}, /188 = {len(data)/188}, 整除: {len(data)%188==0}')
```

### C2. 解析包头部

```python
import struct
with open('w1_bf.ts','rb') as f:
    data = f.read()
pkt = data[:188]
sync_byte = pkt[0]
pid = ((pkt[1] & 0x1F) << 8) | pkt[2]
print(f'同步字节: 0x{sync_byte:02x} ({"对" if sync_byte==0x47 else "错"})')
print(f'PID: {pid} (0x{pid:04x})')
print(f'PUSI(单元起始): {(pkt[1] >> 6) & 1}')
print(f'连续计数器 CC: {pkt[3] & 0xF}')
```

### C3. 看不同 PID（PAT/PMT/视频流）

```python
for i in range(5):
    pkt = data[i*188:(i+1)*188]
    pid = ((pkt[1] & 0x1F) << 8) | pkt[2]
    pusi = (pkt[1] >> 6) & 1
    print(f'packet[{i}]: PID={pid}, PUSI={pusi}')
```

期望看到 PID=0 (PAT)、PID=17 (PMT)、PID=256 (视频流) 等不同值。

---

## D. FLV tag 结构解析

### D1. 生成 FLV

```bash
ffmpeg -y -i w1_color.mp4 -c copy -f flv w1.flv
```

### D2. 解析 FLV Header + 前几个 Tag

```python
import struct
with open('w1.flv','rb') as f:
    data = f.read()
print(f'签名: {data[0:3].decode()}')
print(f'版本: {data[3]}')
print(f'音频标志: {(data[4]>>2)&1}, 视频标志: {data[4]&1}')

offset = 9
offset += 4  # PreviousTagSize0
for i in range(3):
    tag_type = data[offset]
    data_size = struct.unpack('>I', b'\x00' + data[offset+1:offset+4])[0]
    ts_low = struct.unpack('>I', b'\x00' + data[offset+4:offset+7])[0]
    ts_ext = data[offset+7]
    timestamp = (ts_ext << 24) | ts_low
    type_name = {8:'音频', 9:'视频', 18:'脚本'}.get(tag_type, f'未知({tag_type})')
    print(f'Tag[{i}]: {type_name} size={data_size} ts={timestamp}ms')
    offset += 11 + data_size + 4
```

期望：
```
Tag[0]: 脚本 size=262 ts=0ms     ← onMetaData
Tag[1]: 视频 size=56  ts=0ms     ← SPS/PPS
Tag[2]: 视频 size=12221 ts=0ms   ← 关键帧
```

---

## 🎯 练习任务（必做）

1. 用 Python 解析 `w1_color.mp4`，确认 moov 在末尾
2. 再生成一个 `-movflags +faststart` 版本，对比 moov 位置变化
3. 用 signalstats 看任意一个视频的 Y 分量范围，判断它是 Limited 还是 Full
4. 用 Python 数 `w1_bf.ts` 里有几个不同 PID 的包（提示：用字典统计）

## ✅ 验收标准

不看笔记能回答：
1. 怎么用一条 ffprobe 命令查清视频的所有色彩参数？
2. 给你一个 MP4，怎么用 Python 判断 moov 在头还是在尾？
3. TS 文件怎么验证它是不是真的 TS（看哪个字节）？
4. FLV 第一个 Tag 通常是脚本类型，它装的是什么？
