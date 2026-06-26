// ============================================================
// Step 3b: H.264 裸流 → RTMP 推流器
// 对应理论: W3 (NALU/I-P-B帧) + W4 (SPS/PPS) + W2 (FLV)
//
// ⭐ 这是真实无人机载荷端推流的核心场景!
//   硬件编码器输出的就是 H.264 裸流(只有 NALU, 没有容器)
//   你需要: 接收裸流 → 自己打时间戳 → 封装 FLV → 推 RTMP
//
// 和 Step3 (MP4 remux) 的本质区别:
//   Step3: 容器(MP4)帮你做好了时间戳/SPS-PPS索引/包边界
//   Step3b: 裸流啥都没有, 你必须自己造!
//
// 关键难点 (3个):
//   1. SPS/PPS 提取: 裸流里 SPS/PPS 散在前面, 要喂给 extradata
//   2. 时间戳生成: 裸流没 PTS/DTS, 按帧率自己算 (PTS = 帧号 × tick)
//   3. FLV 要求: 必须先发 AVC sequence header (含 SPS/PPS) 才能发视频
//      好在 FFmpeg 帮忙: 你只要把 SPS/PPS 塞进 extradata, 它自动生成
//
// 用法:
//   04_h264_to_rtmp.exe <输入.h264> <RTMP地址> [帧率]
//   04_h264_to_rtmp.exe ..\labs\raw_h264.h264 rtsp://localhost:8554/live2 30
//   也支持 RTMP:
//   04_h264_to_rtmp.exe ..\labs\raw_h264.h264 rtmp://localhost:1935/live2 30
// ============================================================

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>         // AVBitStreamFilter (SPS/PPS 注入神器)
#include <libavutil/avutil.h>
#include <libavutil/time.h>
}

#include <cstdio>
#include <cstring>
#include <windows.h>

// ---------- 工具函数 ----------
static void log_err(const char* msg, int err) {
    char errbuf[128];
    av_strerror(err, errbuf, sizeof(errbuf));
    fprintf(stderr, "[错误] %s: %s\n", msg, errbuf);
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 3) {
        fprintf(stderr, "用法: %s <输入.h264> <RTMP/RTSP地址> [帧率, 默认30]\n", argv[0]);
        fprintf(stderr, "示例: %s ..\\labs\\raw_h264.h264 rtmp://localhost:1935/live2 30\n", argv[0]);
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

    // ========== 第1步: 打开 H.264 裸流 ==========
    // ⭐ 关键: 用 avformat_open_input 打开裸流时, FFmpeg 用 h264 demuxer
    //   它会自动: 用起始码 00 00 00 01 切分 NALU, 每个包一个 NALU(或一帧)
    //   但是! 裸流的 codecpar->extradata 是空的, SPS/PPS 没被提取!
    //   我们待会用 bitstream filter 解决
    AVFormatContext* in_fmt = nullptr;
    AVDictionary* opts = nullptr;
    // 告诉 h264 demuxer 帧率 (裸流没帧率信息, 必须显式给)
    char fps_str[16];
    snprintf(fps_str, sizeof(fps_str), "%d", fps);
    av_dict_set(&opts, "framerate", fps_str, 0);

    int ret = avformat_open_input(&in_fmt, in_path, nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) { log_err("打开裸流失败", ret); return 1; }
    avformat_find_stream_info(in_fmt, nullptr);

    int vidx = av_find_best_stream(in_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidx < 0) {
        fprintf(stderr, "[错误] 裸流里找不到视频流\n");
        avformat_close_input(&in_fmt);
        return 1;
    }
    AVStream* in_stream = in_fmt->streams[vidx];
    AVCodecParameters* in_codecpar = in_stream->codecpar;
    printf("[OK] 打开裸流: %s %dx%d\n",
           avcodec_get_name(in_codecpar->codec_id),
           in_codecpar->width, in_codecpar->height);

    // 检查 extradata (裸流通常为空或只有部分)
    printf("     extradata 大小: %d 字节 %s\n\n",
           in_codecpar->extradata_size,
           in_codecpar->extradata_size > 0 ? "(已有SPS/PPS)" : "(空! 需要提取)");

    // ========== 第2步: ⭐ 用 bitstream filter 提取/整理 SPS/PPS ==========
    // 这是裸流推流的核心技巧!
    //
    // h264_mp4toannexb / annexb 相关的 filter 做的事:
    //   - 把 SPS/PPS 从 extradata 提取出来, 内联到每个 IDR 帧前面
    //   - 保证每个关键帧都带 SPS/PPS (FLV/RTMP 强烈推荐)
    //
    // 但我们的输入是 annexb 格式裸流(已有起始码), SPS/PPS 在流里
    // 我们需要的是: 让 FFmpeg 把 SPS/PPS 收集起来放进 extradata
    // 这里用 "trace_headers" 或直接用 av_bsf_alloc
    //
    // 实际工程更简单可靠的做法:
    //   用 h264_metadata 或直接让 FFmpeg 在 write_header 时自动处理
    //   只要 extradata 不为空, FFmpeg 会自动生成 FLV sequence header
    //
    // 我们这里用最稳妥的方案: 如果 extradata 为空, 用 BSF 提取
    if (in_codecpar->extradata_size == 0) {
        printf("[!] extradata 为空, 用 BSF 提取 SPS/PPS...\n");
        // 用 extract_extradata BSF: 从 annexb 流提取 SPS/PPS 到 extradata
        const AVBitStreamFilter* bsf = av_bsf_get_by_name("extract_extradata");
        if (!bsf) {
            fprintf(stderr, "[错误] 找不到 extract_extradata BSF\n");
            avformat_close_input(&in_fmt);
            return 1;
        }
        AVBSFContext* bsf_ctx = nullptr;
        ret = av_bsf_alloc(bsf, &bsf_ctx);
        if (ret < 0) { log_err("BSF alloc 失败", ret); avformat_close_input(&in_fmt); return 1; }
        // 复制输入参数给 BSF
        avcodec_parameters_copy(bsf_ctx->par_in, in_codecpar);
        ret = av_bsf_init(bsf_ctx);
        if (ret < 0) { log_err("BSF init 失败", ret); av_bsf_free(&bsf_ctx); avformat_close_input(&in_fmt); return 1; }

        // 读前几包喂给 BSF, 让它提取出 SPS/PPS
        AVPacket* tmp_pkt = av_packet_alloc();
        bool got_extradata = false;
        while (av_read_frame(in_fmt, tmp_pkt) >= 0 && !got_extradata) {
            ret = av_bsf_send_packet(bsf_ctx, tmp_pkt);
            if (ret < 0) { av_packet_unref(tmp_pkt); continue; }
            AVPacket* out_pkt = av_packet_alloc();
            while (av_bsf_receive_packet(bsf_ctx, out_pkt) >= 0) {
                if (bsf_ctx->par_out->extradata_size > 0) {
                    // 提取到了! 拷回 in_codecpar
                    avcodec_parameters_copy(in_codecpar, bsf_ctx->par_out);
                    printf("[OK] BSF 提取到 SPS/PPS: %d 字节\n\n",
                           in_codecpar->extradata_size);
                    got_extradata = true;
                }
                av_packet_unref(out_pkt);
            }
            av_packet_free(&out_pkt);
            av_packet_unref(tmp_pkt);
        }
        av_packet_free(&tmp_pkt);
        av_bsf_free(&bsf_ctx);

        if (!got_extradata) {
            fprintf(stderr, "[警告] 未能提取 SPS/PPS, 继续尝试(可能拉流端解不开)\n");
        }
        // 重新定位到文件开头, 准备真正推流
        av_seek_frame(in_fmt, vidx, 0, AVSEEK_FLAG_BACKWARD);
    }

    // ========== 第3步: 创建输出容器 (FLV/RTMP) ==========
    AVFormatContext* out_fmt = nullptr;
    // ⚠️ RTMP 必须用 FLV! RTMP 底层就是 FLV over TCP
    //   rtmp:// URL 没有后缀, 必须显式指定 "flv"
    //   rtsp:// 则指定 "rtsp"
    const char* fmt_name = (strncmp(out_url, "rtmp", 4) == 0) ? "flv" : "rtsp";
    ret = avformat_alloc_output_context2(&out_fmt, nullptr, fmt_name, out_url);
    if (ret < 0) { log_err("创建输出容器失败", ret); avformat_close_input(&in_fmt); return 1; }
    printf("[OK] 输出格式: %s\n", out_fmt->oformat->name);

    // 创建输出流, 复制编码参数(含已提取的 SPS/PPS!)
    AVStream* out_stream = avformat_new_stream(out_fmt, nullptr);
    avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
    out_stream->codecpar->codec_tag = 0;
    // ⭐ 设置 time_base
    //   - RTSP 底层是 RTP, RTP 标准时钟是 90kHz → time_base = 1/90000
    //   - FLV/RTMP 用毫秒 → time_base = 1/1000
    //   设错了会导致 RTSP 服务器拒绝或时间戳错误
    if (strncmp(out_url, "rtsp", 4) == 0) {
        out_stream->time_base = (AVRational){1, 90000};  // RTP 90kHz
    } else {
        out_stream->time_base = (AVRational){1, 1000};   // FLV 毫秒
    }

    printf("[OK] 输出流 #%d: %s %dx%d (extradata=%d字节)\n\n",
           out_stream->index, avcodec_get_name(out_stream->codecpar->codec_id),
           out_stream->codecpar->width, out_stream->codecpar->height,
           out_stream->codecpar->extradata_size);

    // ========== 第4步: 打开网络 + 写容器头 ==========
    // 提前声明所有循环用变量, 避免 goto 跨过初始化
    AVPacket* pkt = nullptr;
    int64_t start_time = 0;
    int64_t frame_pts = 0;      // ⭐ 自己维护的 PTS (裸流没有, 按帧号累加)
    int pkt_count = 0;
    AVDictionary* out_opts = nullptr;
    int64_t pts_step = 0;       // 每帧 PTS 增量

    if (!(out_fmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open2(&out_fmt->pb, out_url, AVIO_FLAG_WRITE, nullptr, nullptr);
        if (ret < 0) { log_err("连接服务器失败", ret); goto cleanup; }
        printf("[OK] 已连接服务器\n");
    }

    // avformat_write_header 做的关键事(对 FLV):
    //   - 写 FLV Header
    //   - ⭐ 用 extradata 里的 SPS/PPS 生成 AVC sequence header Tag!
    //   没有这一步, RTMP 拉流端根本解不开
    //
    // RTMP 推流的关键选项:
    //   - 对 FLV muxer 设置一些标志, 让它按"流"模式工作(不回写文件头)
    // 这两个选项让 FLV muxer 知道是网络流, 不尝试回写(那两个警告就没了)
    av_dict_set(&out_opts, "flvflags", "no_duration_filesize", 0);
    ret = avformat_write_header(out_fmt, &out_opts);
    av_dict_free(&out_opts);
    if (ret < 0) { log_err("写容器头失败", ret); avio_closep(&out_fmt->pb); goto cleanup; }
    printf("[OK] 容器头已写 (FLV sequence header 含 SPS/PPS 已生成)\n\n");

    // ========== 第5步: 推流主循环 ==========
    printf("━━━━━━ 开始推流 ━━━━━━\n");
    printf("%-8s %-10s %-12s %-10s %-8s\n", "包序号", "类型", "PTS(ms)", "大小", "关键帧");

    pkt = av_packet_alloc();
    start_time = av_gettime_relative();
    // 每帧的 PTS 增量(按输出 time_base 计算)
    // RTSP: 90000/30 = 3000 刻度/帧;  FLV: 1000/30 ≈ 33 刻度/帧
    pts_step = out_stream->time_base.den / (out_stream->time_base.num * fps);

    while (av_read_frame(in_fmt, pkt) >= 0) {
        if (pkt->stream_index != vidx) { av_packet_unref(pkt); continue; }

        // ----- 5.1 ⭐ 给裸流包打时间戳 (最关键!) -----
        // 裸流的 pkt->pts/dts 通常是 AV_NOPTS_VALUE (没有时间戳)
        // 我们按帧率自己算: pts = 帧号 × pts_step
        // RTSP: 帧号 × 3000 (90kHz/30fps)
        // FLV:  帧号 × 33   (1000/30fps)
        pkt->pts = frame_pts * pts_step;
        pkt->dts = frame_pts * pts_step;
        pkt->duration = pts_step;
        frame_pts++;

        // 注意: 不再调用 av_packet_rescale_ts!
        // 因为我们已经直接按 out_stream->time_base (1/fps) 生成时间戳了
        // 如果再 rescale, 会把已经正确的值又除一次 → 出错
        pkt->stream_index = out_stream->index;
        pkt->pos = -1;

        // 限速: 按 PTS 控制发送节奏
        double pts_sec = pkt->pts * av_q2d(out_stream->time_base);
        int64_t target_us = (int64_t)(pts_sec * 1000000);
        int64_t elapsed_us = av_gettime_relative() - start_time;
        if (target_us > elapsed_us) {
            av_usleep((unsigned)(target_us - elapsed_us));
        }

        // 判断帧类型 (从 flags 看)
        bool is_keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;

        // ----- 5.2 推出去 -----
        ret = av_interleaved_write_frame(out_fmt, pkt);
        if (ret < 0) {
            // 静默大部分错误, 只在第一个包失败时详细报告
            if (pkt_count < 3) log_err("推包失败", ret);
        }

        pkt_count++;
        if (pkt_count % 60 == 0 || pkt_count <= 5) {
            printf("%-8d %-10s %-12.0f %-10d %s\n",
                   pkt_count,
                   is_keyframe ? "I" : "P/B",
                   pts_sec * 1000,
                   pkt->size,
                   is_keyframe ? "★" : "");
        }
        av_packet_unref(pkt);
    }

    printf("\n━━━━━━ 推流结束 ━━━━━━\n");
    printf("  共推送 %d 个包\n", pkt_count);

    // ========== 第6步: 收尾 ==========
    av_write_trailer(out_fmt);
    printf("\n[OK] 完成。拉流验证:\n");
    printf("  ffplay %s\n", out_url);
    printf("  (RTMP 用: ffplay -fflags nobuffer rtmp://...)\n");

cleanup:
    if (pkt) av_packet_free(&pkt);
    if (out_fmt) {
        if (!(out_fmt->oformat->flags & AVFMT_NOFILE) && out_fmt->pb) {
            avio_closep(&out_fmt->pb);
        }
        avformat_free_context(out_fmt);
    }
    avformat_close_input(&in_fmt);
    return 0;

    // 补充: 上面用了 goto cleanup, 但所有变量都已提前声明, 编译OK
}

// ============================================================
// 编译 (cmd.exe):
//   set PATH=D:\msys64\ucrt64\bin;%PATH%
//   set FF=D:\FFmpeg\ffmpeg-n7.1-latest-win64-gpl-shared-7.1
//   g++ 04_h264_to_rtmp.cpp -o build\04_h264_to_rtmp.exe ^
//       -I%FF%\include -L%FF%\lib ^
//       -lavformat -lavcodec -lavutil ^
//       -std=c++17
//
// 运行 (MediaMTX 已启动):
//   RTMP 推流:
//     build\04_h264_to_rtmp.exe ..\labs\raw_h264.h264 rtmp://localhost:1935/live2 30
//   RTSP 推流 (也支持):
//     build\04_h264_to_rtmp.exe ..\labs\raw_h264.h264 rtsp://localhost:8554/live2 30
//
// 拉流验证:
//   ffplay rtmp://localhost:1935/live2
//   ffplay -rtsp_transport tcp rtsp://localhost:8554/live2
// ============================================================
