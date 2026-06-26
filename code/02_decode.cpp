// ============================================================
// Step 2: 用 FFmpeg API 解码视频, 把压缩帧变成原始 YUV
// 对应理论: W3 (I/P/B帧) + W4 (NALU)
//
// 这个程序做的事:
//   1. 打开视频文件 (复用 Step1 的逻辑)
//   2. 找到视频流, 初始化解码器 AVCodecContext
//   3. 循环: 读包 → 喂解码器 → 取帧
//   4. 把第一帧 YUV 存成 .yuv 文件 (可用 YUV播放器查看)
//
// 编译:
//   见文末, 或用 build.bat 2
//
// 用法:
//   02_decode.exe <视频文件> [输出.yuv]
//   02_decode.exe ..\labs\w1_sample.mp4 out.yuv
// ============================================================

extern "C" {
#include <libavformat/avformat.h>   // avformat_open_input, av_read_frame
#include <libavcodec/avcodec.h>     // avcodec_find_decoder, avcodec_send/receive
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>     // av_image_get_buffer_size
}

#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>   // SetConsoleOutputCP (修复中文乱码)

// 帧类型转中文 (对应 W3)
const char* picture_type_char(int pict_type) {
    switch (pict_type) {
        case AV_PICTURE_TYPE_I: return "I";  // 帧内预测
        case AV_PICTURE_TYPE_P: return "P";  // 前向预测
        case AV_PICTURE_TYPE_B: return "B";  // 双向预测
        case AV_PICTURE_TYPE_S: return "S";  // 可跳过
        case AV_PICTURE_TYPE_SI:return "SI";
        case AV_PICTURE_TYPE_SP:return "SP";
        default: return "?";
    }
}

int main(int argc, char* argv[]) {
    // 修复 Windows 控制台中文乱码(UTF-8 源 vs GBK 控制台)
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 2) {
        fprintf(stderr, "用法: %s <视频文件> [输出.yuv]\n", argv[0]);
        fprintf(stderr, "示例: %s ..\\labs\\w1_sample.mp4 out.yuv\n", argv[0]);
        return 1;
    }
    const char* filepath = argv[1];
    const char* outpath = (argc >= 3) ? argv[2] : "decoded_frame.yuv";

    // ========== 第1步: 打开文件 (和 Step1 一样) ==========
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, filepath, nullptr, nullptr) < 0) {
        fprintf(stderr, "[错误] 打不开文件: %s\n", filepath);
        return 1;
    }
    avformat_find_stream_info(fmt_ctx, nullptr);
    printf("[OK] 打开文件: %s (共 %d 路流)\n", filepath, fmt_ctx->nb_streams);

    // ========== 第2步: 找到视频流的索引 ==========
    // av_find_best_stream: 自动找最合适的视频流
    // (有些文件有多路视频, 这个函数帮你挑主路)
    int video_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_idx < 0) {
        fprintf(stderr, "[错误] 找不到视频流\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }
    AVStream* vstream = fmt_ctx->streams[video_idx];
    AVCodecParameters* codecpar = vstream->codecpar;
    printf("[OK] 视频流 #%d: %s %dx%d\n\n",
           video_idx, avcodec_get_name(codecpar->codec_id),
           codecpar->width, codecpar->height);

    // ========== 第3步: 初始化解码器 (关键!) ==========
    // 这一步把 "编码参数 codecpar" 转成 "可工作的解码器上下文"

    // 3.1 根据 codec_id 找到解码器 (如 h264 → 内置 h264 解码器)
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "[错误] 找不到解码器: %s\n", avcodec_get_name(codecpar->codec_id));
        avformat_close_input(&fmt_ctx);
        return 1;
    }
    printf("[OK] 解码器: %s\n", codec->name);

    // 3.2 分配解码器上下文
    // ⚠️ 注意 FFmpeg 版本差异:
    //   老版本: 直接用 stream->codec (已废弃)
    //   新版本: 自己 alloc, 再把 codecpar 拷进去
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    // 把流里的编码参数(SPS/PPS/profile/分辨率等)拷到解码器上下文
    // 这一步会把 W4 讲的 SPS/PPS 喂给解码器!
    avcodec_parameters_to_context(codec_ctx, codecpar);

    // 3.3 打开解码器 (真正初始化硬件/分配内存)
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        fprintf(stderr, "[错误] 解码器打开失败\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }
    printf("[OK] 解码器已打开 (profile=%d level=%d)\n\n",
           codec_ctx->profile, codec_ctx->level);

    // ========== 第4步: 准备输出文件 ==========
    FILE* fout = nullptr;
    bool save_yuv = true;
    if (fopen_s(&fout, outpath, "wb") != 0 || !fout) {
        fprintf(stderr, "[警告] 无法写 %s, 将只打印不解码到文件\n", outpath);
        save_yuv = false;
    }

    // YUV420P 一帧的字节数 = width × height × 1.5
    // (Y全分辨率 + U,V各1/4)
    int yuv_frame_size = av_image_get_buffer_size(
        codec_ctx->pix_fmt, codec_ctx->width, codec_ctx->height, 1);

    // ========== 第5步: 解码主循环 ⭐ 核心 ==========
    AVPacket* pkt = av_packet_alloc();    // 压缩包
    AVFrame*  frame = av_frame_alloc();   // 解码后的原始帧
    int frame_count = 0;
    int packet_count = 0;
    int max_frames = 3;  // 只解码前3帧用于演示

    printf("━━━━━━ 开始解码 ━━━━━━\n");
    printf("%-8s %-10s %-12s %-10s %-10s\n",
           "包序号", "帧类型", "DTS", "PTS", "包大小");

    // av_read_frame: 从文件读一个包(可能含多帧的某个分片)
    // 返回 0=成功读到, AVERROR_EOF=文件结束, 负数=错误
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        // 只处理视频流(跳过音频等)
        if (pkt->stream_index != video_idx) {
            av_packet_unref(pkt);  // 释放包内存
            continue;
        }
        packet_count++;

        // ----- 5.1 把压缩包喂给解码器 -----
        // avcodec_send_packet: 把包丢进解码器内部队列
        // 它不会立即解码! 解码器会根据需要缓冲(W3的B帧重排)
        int ret = avcodec_send_packet(codec_ctx, pkt);
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "[警告] send_packet 失败: %s\n", errbuf);
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);  // 喂完就释放包(内部已拷贝)

        // ----- 5.2 循环取出解码后的帧 -----
        // ⚠️ 关键: send 和 receive 不是 1:1!
        // B帧存在时, 可能 send 好几个包才 receive 出一帧
        // 所以 receive 必须在 while 循环里, 直到返回 EAGAIN
        while (ret >= 0) {
            ret = avcodec_receive_frame(codec_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;  // 解码器还要更多包, 或流结束了
            }
            if (ret < 0) {
                fprintf(stderr, "[错误] 解码失败\n");
                break;
            }

            // ----- 成功拿到一帧! 打印信息 -----
            // 把 PTS/DTS 从 time_base 刻度换算成秒 (对应 W1-2)
            double pts_sec = (frame->pts != AV_NOPTS_VALUE)
                ? frame->pts * av_q2d(vstream->time_base) : -1;

            printf("%-8d %-10s %-12.3f %-10.3f %-10d\n",
                   packet_count,
                   picture_type_char(frame->pict_type),
                   pts_sec,
                   pts_sec,  // 简化, 用 PTS
                   pkt->size);

            // ----- 存 YUV -----
            if (save_yuv && frame_count < max_frames) {
                // frame->data[0/1/2] = Y/U/V 三个平面的指针
                // frame->linesize[0/1/2] = 每行字节数(可能有padding)
                // 写 YUV420P: Y全写, U和V各1/4
                int w = codec_ctx->width;
                int h = codec_ctx->height;
                // Y 平面
                for (int y = 0; y < h; y++) {
                    fwrite(frame->data[0] + y * frame->linesize[0],
                           1, w, fout);
                }
                // U 平面 (宽高各减半)
                for (int y = 0; y < h / 2; y++) {
                    fwrite(frame->data[1] + y * frame->linesize[1],
                           1, w / 2, fout);
                }
                // V 平面
                for (int y = 0; y < h / 2; y++) {
                    fwrite(frame->data[2] + y * frame->linesize[2],
                           1, w / 2, fout);
                }
                printf("         → 已保存第 %d 帧到 %s (%d 字节)\n",
                       frame_count + 1, outpath, yuv_frame_size);
            }

            frame_count++;
            av_frame_unref(frame);  // 用完释放, 复用同一结构

            if (frame_count >= max_frames) {
                goto done;  // 演示用, 解码3帧就够
            }
        }
    }

done:
    printf("\n━━━━━━ 解码完成 ━━━━━━\n");
    printf("  读取的包数: %d\n", packet_count);
    printf("  解出的帧数: %d\n", frame_count);
    printf("  每帧 YUV 大小: %d 字节 (= %dx%d × 1.5)\n",
           yuv_frame_size, codec_ctx->width, codec_ctx->height);

    if (save_yuv && frame_count > 0) {
        printf("\n[OK] YUV 已存到: %s\n", outpath);
        printf("     可用 YUV 播放器(如 ffplay)查看:\n");
        printf("     ffplay -f rawvideo -pixel_format yuv420p ");
        printf("-video_size %dx%d %s\n",
               codec_ctx->width, codec_ctx->height, outpath);
    }

    // ========== 第6步: 清理 (必须按相反顺序) ==========
    if (fout) fclose(fout);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);

    return 0;
}

// ============================================================
// 编译命令 (用 cmd.exe, 不要直接在 Git Bash 编):
//
// set PATH=D:\msys64\ucrt64\bin;%PATH%
// set FF=D:\FFmpeg\ffmpeg-n7.1-latest-win64-gpl-shared-7.1
// g++ 02_decode.cpp -o build\02_decode.exe ^
//     -I%FF%\include -L%FF%\lib ^
//     -lavformat -lavcodec -lavutil -lswscale ^
//     -std=c++17
// ============================================================
