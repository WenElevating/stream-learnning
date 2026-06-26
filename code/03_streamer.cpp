// ============================================================
// Step 3: 最小推流器 — 读视频文件, 转 RTSP 推到服务器
// 对应理论: W3-W5 (编码参数) + W1-2 (时间戳换算) + W6 RTSP
//
// ⭐ 这是无人机载荷端推流的核心逻辑缩影!
//   真实载荷: 摄像头→编码器→H.264包→RTSP推出
//   我们这里: 文件(已有H.264包)→读出→RTSP推出
//   本质一样: 都是"搬运压缩包到网络"
//
// 关键概念: 这是 remux(重封装), 不解码不编码!
//   MP4 和 RTSP 装的 H.264 包是一样的, 只是容器不同
//   所以速度极快, 数据量不变
//
// 用法:
//   03_streamer.exe <输入文件> <RTSP地址>
//   03_streamer.exe ..\labs\w1_av.mp4 rtsp://localhost:8554/mystream
//
// 拉流验证(另开窗口):
//   ffplay -rtsp_transport tcp rtsp://localhost:8554/mystream
// ============================================================

extern "C" {
#include <libavformat/avformat.h>   // 输入输出容器
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>         // av_gettime_relative (限速用)
}

#include <cstdio>
#include <cstring>
#include <windows.h>   // SetConsoleOutputCP

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);  // 修复中文乱码

    if (argc < 3) {
        fprintf(stderr, "用法: %s <输入文件> <RTSP地址>\n", argv[0]);
        fprintf(stderr, "示例: %s ..\\labs\\w1_av.mp4 rtsp://localhost:8554/mystream\n", argv[0]);
        return 1;
    }
    const char* in_path  = argv[1];
    const char* out_url  = argv[2];

    printf("========================================\n");
    printf("输入: %s\n", in_path);
    printf("输出: %s\n", out_url);
    printf("========================================\n\n");

    // ========== 第1步: 打开输入文件 (复用 Step1 逻辑) ==========
    AVFormatContext* in_fmt = nullptr;
    if (avformat_open_input(&in_fmt, in_path, nullptr, nullptr) < 0) {
        fprintf(stderr, "[错误] 打不开输入文件\n");
        return 1;
    }
    avformat_find_stream_info(in_fmt, nullptr);
    printf("[OK] 打开输入: %d 路流\n", in_fmt->nb_streams);

    // ========== 第2步: 创建输出容器 (RTSP) ==========
    // avformat_alloc_output_context2: 根据 url 推断输出格式
    //   rtsp:// → RTSP 协议
    //   xxx.flv → FLV 格式
    //   xxx.mp4 → MP4 格式
    // ⚠️ 坑: 函数靠"文件后缀"推断格式, 但 rtsp:// 没有后缀!
    //   必须显式传第3个参数 format_name="rtsp"
    //   否则报错 "Unable to choose an output format"
    AVFormatContext* out_fmt = nullptr;
    if (avformat_alloc_output_context2(&out_fmt, nullptr, "rtsp", out_url) < 0) {
        fprintf(stderr, "[错误] 创建输出容器失败\n");
        avformat_close_input(&in_fmt);
        return 1;
    }
    printf("[OK] 输出格式: %s\n", out_fmt->oformat->name);

    // 提前声明所有变量, 避免 goto 跨过初始化(C++ 规则)
    int* stream_map = new int[in_fmt->nb_streams];
    AVDictionary* opts = nullptr;
    AVPacket* pkt = nullptr;
    int64_t start_time = 0;
    int64_t first_pkt_time = -1;
    int pkt_count = 0;
    int out_stream_idx = 0;
    bool header_written = false;

    // ========== 第3步: 复制所有流的编码参数 ==========
    // ⭐ 核心思想: 不重新编码, 直接搬运!
    // 对每路输入流, 在输出里建一路对应的流, 复制编码参数
    // 用一个数组记录"输入流索引 → 输出流索引"的映射

    for (unsigned i = 0; i < in_fmt->nb_streams; i++) {
        AVStream* in_stream = in_fmt->streams[i];
        AVCodecParameters* in_codecpar = in_stream->codecpar;

        // RTSP/RTMP 通常只支持音视频, 跳过字幕/数据流
        if (in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            stream_map[i] = -1;
            continue;
        }

        // 在输出容器里创建一路新流
        AVStream* out_stream = avformat_new_stream(out_fmt, nullptr);
        if (!out_stream) {
            fprintf(stderr, "[错误] 创建输出流失败\n");
            continue;
        }

        // ⭐ 复制编码参数 (SPS/PPS/profile/分辨率 都复制过去)
        // 这样接收端才知道这路流是什么编码
        if (avcodec_parameters_copy(out_stream->codecpar, in_codecpar) < 0) {
            fprintf(stderr, "[错误] 复制编码参数失败\n");
            continue;
        }
        out_stream->codecpar->codec_tag = 0;  // 让输出端自动选 codec_tag

        stream_map[i] = out_stream_idx++;
        printf("[OK] 流 #%d (%s) → 输出流 #%d\n",
               i, avcodec_get_name(in_codecpar->codec_id), stream_map[i]);
    }

    // ========== 第4步: 打开网络连接 + 写容器头 ==========
    // RTSP 是网络协议, 需要 avio_open 建立连接
    // (AVIOContext 是 FFmpeg 的 I/O 抽象层, 能读写文件/网络/内存)
    if (!(out_fmt->oformat->flags & AVFMT_NOFILE)) {
        // AVIO_FLAG_WRITE 表示写模式
        if (avio_open2(&out_fmt->pb, out_url, AVIO_FLAG_WRITE, nullptr, nullptr) < 0) {
            fprintf(stderr, "[错误] 无法连接到 %s\n", out_url);
            goto cleanup;
        }
        printf("[OK] 已连接到服务器\n");
    }

    // avformat_write_header: 写容器头
    //   - 对 RTSP: 触发 OPTIONS/DESCRIBE/SETUP/ANNOUNCE 握手 (W6-W7)
    //   - 对 MP4: 写 ftyp + 开始写 moov
    // AVDictionary 存放额外选项, 这里设置 RTSP 用 TCP 传 RTP(更可靠)
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);  // RTSP 走 TCP
    if (avformat_write_header(out_fmt, &opts) < 0) {
        fprintf(stderr, "[错误] 写容器头失败\n");
        avio_closep(&out_fmt->pb);
        goto cleanup;
    }
    av_dict_free(&opts);
    header_written = true;
    printf("[OK] 容器头已写入 (RTSP 握手完成)\n\n");

    // ========== 第5步: 推流主循环 ⭐ 核心 ==========
    printf("━━━━━━ 开始推流 ━━━━━━\n");
    printf("%-8s %-8s %-12s %-10s\n", "包序号", "流", "PTS(秒)", "大小");

    pkt = av_packet_alloc();
    start_time = av_gettime_relative();  // 用于限速

    // 限速原理 (重要!):
    //   视频文件里的包是"密集"存的, 读完只要几毫秒
    //   但真实推流必须按时间节奏发! (1秒的视频要花1秒推)
    //   否则服务器/播放器缓冲区会爆, 或拉流端看到"快进"
    //   所以我们要: 等到该发这包的时刻再发

    while (av_read_frame(in_fmt, pkt) >= 0) {
        // 跳过被过滤的流(字幕等)
        if (stream_map[pkt->stream_index] < 0) {
            av_packet_unref(pkt);
            continue;
        }

        AVStream* in_stream  = in_fmt->streams[pkt->stream_index];
        AVStream* out_stream = out_fmt->streams[stream_map[pkt->stream_index]];

        // ----- 5.1 限速: 等到该发这包的时刻 -----
        // 把包的 PTS 换算成微秒 (统一时间单位)
        // 公式: 微秒 = PTS × time_base × 1000000
        int64_t pkt_time_us = 0;
        if (pkt->pts != AV_NOPTS_VALUE) {
            pkt_time_us = av_rescale_q(pkt->pts, in_stream->time_base,
                                       {1, AV_TIME_BASE});  // 换成 AV_TIME_BASE(微秒)
            if (first_pkt_time < 0) first_pkt_time = pkt_time_us;
        }

        // 计算应该等待的时间 = 这包相对首包的时间差 - 已经过去的时间
        int64_t elapsed_us = av_gettime_relative() - start_time;
        int64_t target_us  = pkt_time_us - first_pkt_time;
        if (target_us > elapsed_us) {
            // 比该发的时刻早, 睡一会
            av_usleep((unsigned)(target_us - elapsed_us));
        }

        // ----- 5.2 时间戳换算 (W1-2 的核心!) -----
        // 输入流和输出流的 time_base 可能不同!
        //   MP4 视频流 time_base = 1/15360
        //   RTSP/RTP time_base   = 1/90000
        // 必须用 av_rescale_q 换算, 否则拉流端时间戳全乱 → 音画不同步
        av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
        // 重新设置流索引 (包现在属于输出流了)
        pkt->stream_index = stream_map[pkt->stream_index];
        // pos = -1 表示位置未知(网络推流没有文件偏移概念)
        pkt->pos = -1;

        // ----- 5.3 把包写入输出 (推出去!) -----
        // av_interleaved_frame: 自动处理交错和缓冲
        //   (让音视频包按时间顺序交错, 对 RTSP/MP4 很重要)
        // 不是直接 write_frame! interleaved 会缓冲重排保证顺序正确
        int ret = av_interleaved_write_frame(out_fmt, pkt);
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "[警告] 推包失败: %s\n", errbuf);
        }

        pkt_count++;
        // 每 30 包打印一次 (避免刷屏)
        if (pkt_count % 30 == 0 || pkt_count <= 3) {
            double pts_sec = (pkt->pts != AV_NOPTS_VALUE)
                ? pkt->pts * av_q2d(out_stream->time_base) : -1;
            printf("%-8d %-8d %-12.3f %-10d\n",
                   pkt_count, pkt->stream_index, pts_sec, pkt->size);
        }

        av_packet_unref(pkt);
    }

    printf("\n━━━━━━ 推流结束 ━━━━━━\n");
    printf("  共推送 %d 个包\n", pkt_count);

    // ========== 第6步: 写容器尾 + 清理 ==========
cleanup:
    if (header_written) {
        av_write_trailer(out_fmt);   // 写容器尾(RTSP 的 TEARDOWN)
    }
    if (pkt) av_packet_free(&pkt);
    delete[] stream_map;
    av_dict_free(&opts);
    if (out_fmt) {
        if (!(out_fmt->oformat->flags & AVFMT_NOFILE) && out_fmt->pb) {
            avio_closep(&out_fmt->pb);  // 关闭网络连接
        }
        avformat_free_context(out_fmt);
    }
    avformat_close_input(&in_fmt);

    printf("\n[OK] 完成。拉流验证:\n");
    printf("  ffplay -rtsp_transport tcp %s\n", out_url);
    return 0;
}

// ============================================================
// 编译 (用 cmd.exe):
//   set PATH=D:\msys64\ucrt64\bin;%PATH%
//   set FF=D:\FFmpeg\ffmpeg-n7.1-latest-win64-gpl-shared-7.1
//   g++ 03_streamer.cpp -o build\03_streamer.exe ^
//       -I%FF%\include -L%FF%\lib ^
//       -lavformat -lavcodec -lavutil ^
//       -std=c++17
//
// 运行 (先确保 MediaMTX 在跑):
//   set PATH=%FF%\bin;%PATH%
//   build\03_streamer.exe ..\labs\w1_av.mp4 rtsp://localhost:8554/mystream
//
// 拉流验证 (另开窗口):
//   ffplay -rtsp_transport tcp rtsp://localhost:8554/mystream
// ============================================================
