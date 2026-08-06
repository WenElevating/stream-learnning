// ============================================================
// Step 3c: 用 H264Streamer 类推流(字节数组版)
//
// 这个程序演示真实无人机载荷场景:
//   硬件编码器回调 → 给一段 H.264 字节 → pushFrame() → 推流
//
// 因为没有真实硬件编码器, 我们用文件"假装":
//   从 .h264 文件按帧读出字节 → 调 pushFrame()
//   这和真实硬编码器回调的逻辑【完全一样】, 只是数据来源不同
//
// 真实项目里, 你只要把 main() 里的"读文件"换成"硬编码器回调":
//   void onEncodedFrame(uint8_t* data, int size, bool isKey) {
//       streamer.pushFrame(data, size);
//   }
//
// 用法:
//   05_h264_streamer.exe <输入.h264> <RTMP/RTSP地址> [fps]
//   05_h264_streamer.exe ..\labs\raw_h264_long.h264 rtmp://localhost:1935/live/cam01 30
// ============================================================

#include "H264Streamer.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <windows.h>   // SetConsoleOutputCP

// --------------------------------------------------------
// 从 H.264 裸流文件里按 NALU 读出字节(模拟硬编码器回调)
// 返回: 每个 NALU 的完整字节(含起始码 00 00 00 01)
//
// ⭐ 真实硬编码器通常是"一帧一个回调", 一帧可能含多个 NALU
//   但为了简单和稳健, 这里按单个 NALU 喂给 pushFrame
//   pushFrame 内部会自动: 缓存SPS/PPS + 识别IDR + 合并发送
// --------------------------------------------------------
std::vector<std::vector<uint8_t>> readFramesFromFile(const char* path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        fprintf(stderr, "[错误] 打不开文件: %s\n", path);
        return {};
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(file_size);
    fread(buf.data(), 1, file_size, f);
    fclose(f);
    printf("[OK] 读取文件: %s (%ld 字节)\n", path, file_size);

    // 扫描起始码切 NALU (同时处理 3字节 00 00 01 和 4字节 00 00 00 01)
    std::vector<std::vector<uint8_t>> nalus;
    int i = 0;
    int cur_start = -1;  // 当前 NALU 的起始码起点
    while (i < (int)buf.size() - 2) {
        bool is_sc = false;
        int sc_len = 0;
        if (buf[i]==0 && buf[i+1]==0) {
            if (i+2 < (int)buf.size() && buf[i+2]==1) {
                is_sc = true; sc_len = 3;
            } else if (i+3 < (int)buf.size() && buf[i+2]==0 && buf[i+3]==1) {
                is_sc = true; sc_len = 4;
            }
        }
        if (is_sc) {
            // 遇到新起始码, 保存上一个 NALU
            if (cur_start >= 0) {
                nalus.emplace_back(buf.begin() + cur_start, buf.begin() + i);
            }
            cur_start = i;
            i += sc_len;
        } else {
            i++;
        }
    }
    // 最后一个 NALU
    if (cur_start >= 0) {
        nalus.emplace_back(buf.begin() + cur_start, buf.end());
    }

    printf("[OK] 切分出 %zu 个 NALU\n\n", nalus.size());
    return nalus;
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    // 禁用 stdout 缓冲, 让 printf 实时输出(后台运行时不被吞)
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    if (argc < 3) {
        fprintf(stderr, "用法: %s <输入.h264> <RTMP/RTSP地址> [fps]\n", argv[0]);
        fprintf(stderr, "示例: %s ..\\labs\\raw_h264_long.h264 rtmp://localhost:1935/live/cam01 30\n", argv[0]);
        return 1;
    }
    const char* in_path = argv[1];
    const char* out_url = argv[2];
    int fps = (argc >= 4) ? atoi(argv[3]) : 30;

    printf("========================================\n");
    printf("输入(裸流): %s\n", in_path);
    printf("输出:       %s\n", out_url);
    printf("帧率:       %d fps\n", fps);
    printf("========================================\n\n");

    // ===== 1. 模拟硬编码器: 从文件读出每个 NALU 字节 =====
    std::vector<std::vector<uint8_t>> nalus = readFramesFromFile(in_path);
    if (nalus.empty()) {
        fprintf(stderr, "[错误] 没读到任何 NALU\n");
        return 1;
    }

    // ===== 2. 用 H264Streamer 推流 =====
    H264Streamer streamer;
    if (!streamer.open(out_url, 640, 360, fps)) {
        fprintf(stderr, "[错误] 推流器打开失败\n");
        return 1;
    }

    // ===== 3. 逐个 NALU 推送(模拟硬编码器回调) =====
    printf("━━━━━━ 开始推流(模拟硬编码器回调) ━━━━━━\n");
    int nalu_idx = 0;
    int frame_count = 0;
    for (auto& nalu_data : nalus) {
        // ⭐ 这就是真实硬编码器回调里你要写的一行!
        streamer.pushFrame(nalu_data.data(), (int)nalu_data.size());

        // 判断 NALU 类型(跳过起始码后的第一个字节)
        uint8_t first_byte = 0;
        for (size_t k = 0; k < nalu_data.size(); k++) {
            if (nalu_data[k] != 0 && !(k > 0 && nalu_data[k-1] == 0 && k > 1 && nalu_data[k-2] == 0)) {
                // 找到第一个非0非起始码字节
                if (nalu_data[k] == 1) { first_byte = nalu_data[k+1]; break; }
            }
        }
        // 更简单: 找 00 00 00 01 或 00 00 01 后的字节
        for (size_t k = 0; k + 3 < nalu_data.size(); k++) {
            if (nalu_data[k]==0 && nalu_data[k+1]==0 &&
                ((nalu_data[k+2]==0 && nalu_data[k+3]==1) || nalu_data[k+2]==1)) {
                int off = (nalu_data[k+2]==1) ? (k+3) : (k+4);
                if (off < (int)nalu_data.size()) first_byte = nalu_data[off];
                break;
            }
        }
        int nal_type = first_byte & 0x1F;
        const char* type_name = "?";
        switch (nal_type) {
            case 7: type_name = "SPS"; break;
            case 8: type_name = "PPS"; break;
            case 5: type_name = "IDR"; frame_count++; break;
            case 1: type_name = "P/B"; frame_count++; break;
            case 6: type_name = "SEI"; break;
        }

        nalu_idx++;
        if (nalu_idx <= 5 || nalu_idx % 90 == 0) {
            printf("  NALU %4d: %5d 字节  类型=%s\n",
                   nalu_idx, (int)nalu_data.size(), type_name);
        }
    }

    printf("\n━━━━━━ 推流结束 ━━━━━━\n");
    printf("  共推送 %d 个 NALU (%d 帧)\n", nalu_idx, frame_count);

    streamer.close();
    printf("\n[OK] 完成。拉流验证:\n");
    printf("  ffplay %s\n", out_url);
    return 0;
}

// ============================================================
// 编译 (cmd.exe):
//   set PATH=D:\msys64\ucrt64\bin;%PATH%
//   set FF=D:\FFmpeg\ffmpeg-n7.1-latest-win64-gpl-shared-7.1
//   g++ 05_h264_streamer.cpp -o build\05_h264_streamer.exe ^
//       -I%FF%\include -L%FF%\lib ^
//       -lavformat -lavcodec -lavutil ^
//       -std=c++17
//
// 运行 (MediaMTX 已启动):
//   RTMP:
//     build\05_h264_streamer.exe ..\labs\raw_h264_long.h264 rtmp://localhost:1935/live/cam01 30
//   RTSP:
//     build\05_h264_streamer.exe ..\labs\raw_h264_long.h264 rtsp://localhost:8554/cam01 30
//
// 拉流验证:
//   ffplay rtmp://localhost:1935/live/cam01
//   ffplay -rtsp_transport tcp rtsp://localhost:8554/cam01
// ============================================================
