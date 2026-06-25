// ============================================================
// Step 1: 用 FFmpeg API 打开视频文件并打印流信息
// 对应理论: W1 容器/流/编码三层结构
// 这就是 ffprobe 背后的核心代码原理
// ============================================================
//
// 编译(在项目根目录运行):
//   见 build/compile.bat 或文末注释
//
// 用法:
//   01_probe.exe <视频文件路径>
//   01_probe.exe ..\labs\w1_sample.mp4
//

extern "C" {
// ⚠️ FFmpeg 是 C 库, C++ 引入必须用 extern "C"
// 否则链接报错 "undefined reference"(C++ 会改名 name mangling)
#include <libavformat/avformat.h>   // AVFormatContext, avformat_open_input
#include <libavcodec/avcodec.h>     // AVCodecParameters, AVMediaType
#include <libavutil/avutil.h>       // av_version_info
#include <libavutil/pixdesc.h>      // av_get_pix_fmt_name, AV_PIX_FMT_*
}

#include <cstdio>
#include <cstring>

// 小工具: 把 AVMediaType 枚举转成中文名
const char* media_type_name(AVMediaType type) {
    switch (type) {
        case AVMEDIA_TYPE_VIDEO: return "视频";
        case AVMEDIA_TYPE_AUDIO: return "音频";
        case AVMEDIA_TYPE_SUBTITLE: return "字幕";
        case AVMEDIA_TYPE_DATA: return "数据";
        default: return "未知";
    }
}

// 小工具: 把 AVPixelFormat 转人话(YUV420P 等)
const char* pixfmt_name(AVPixelFormat fmt) {
    if (fmt == AV_PIX_FMT_YUV420P) return "yuv420p ⭐(W1讲过,无人机默认)";
    if (fmt == AV_PIX_FMT_YUV444P) return "yuv444p";
    if (fmt == AV_PIX_FMT_NV12)    return "nv12";
    // 通用方法: 从 pixdesc 取名字
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    return desc ? desc->name : "unknown";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <视频文件>\n", argv[0]);
        fprintf(stderr, "示例: %s ..\\labs\\w1_sample.mp4\n", argv[0]);
        return 1;
    }
    const char* filepath = argv[1];

    // 打印 FFmpeg 版本, 确认库链接成功
    printf("========================================\n");
    printf("FFmpeg 版本: %s\n", av_version_info());
    printf("========================================\n\n");

    // ----------------------------------------------------------
    // 核心数据结构: AVFormatContext
    // 它就是 W1 讲的"容器(快递箱)"在内存里的表示
    // 里面装着 nb_streams 路流(物品), 每路流有编码参数(压缩袋)
    // ----------------------------------------------------------
    AVFormatContext* fmt_ctx = nullptr;

    // ----------------------------------------------------------
    // avformat_open_input(): 打开文件并读取容器头部
    // 这一步做的事:
    //   1. 探测文件格式(MP4? FLV? TS?) → 对应 W2 容器识别
    //   2. 解析容器头部(MP4 的 ftyp/moov, FLV 的 header)
    //   3. 填充 fmt_ctx->iformat, fmt_ctx->streams[]
    //
    // 第2个参数: NULL 表示自动探测格式
    // 第4个参数: NULL 表示无额外选项
    // 返回值: 0=成功, 负数=失败(用 av_err2str 看错误)
    // ----------------------------------------------------------
    int ret = avformat_open_input(
        &fmt_ctx,    // 输出: 填充好的上下文
        filepath,    // 输入: 文件路径或URL
        nullptr,     // 强制指定格式(NULL=自动探测)
        nullptr      // 额外选项(如 rtsp_transport=tcp)
    );

    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[错误] 打开文件失败: %s\n", errbuf);
        return 1;
    }
    printf("[OK] 成功打开: %s\n", filepath);
    printf("     容器格式: %s (%s)\n\n",
           fmt_ctx->iformat->name,        // 如 "mov,mp4,m4a..."
           fmt_ctx->iformat->long_name);

    // ----------------------------------------------------------
    // ⚠️ 重要: avformat_open_input 只读了容器头部
    // 有些格式(如 FLV)头部信息不全, 需要再读几包才能知道流细节
    // avformat_find_stream_info() 会预读几帧来补全信息
    // 这就是为什么 ffprobe 对某些文件要转一会才出结果
    // ----------------------------------------------------------
    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) {
        fprintf(stderr, "[警告] stream info 不完整, 结果可能不准\n");
    }

    // ----------------------------------------------------------
    // 容器层信息 (对应 W1/W2 的 FORMAT 块)
    // ----------------------------------------------------------
    printf("━━━━━━ 容器信息 (FORMAT) ━━━━━━\n");
    printf("  流的数量: %d\n", fmt_ctx->nb_streams);
    double duration_sec = fmt_ctx->duration > 0 ?
        fmt_ctx->duration / (double)AV_TIME_BASE : 0;
    printf("  时长:     %.3f 秒\n", duration_sec);
    printf("  文件大小: %lld 字节 (%.2f KB)\n",
           fmt_ctx->pb ? avio_size(fmt_ctx->pb) : 0,
           fmt_ctx->pb ? avio_size(fmt_ctx->pb) / 1024.0 : 0);
    // bit_rate: 整个容器的码率(含封装开销)
    if (fmt_ctx->bit_rate > 0) {
        printf("  容器码率: %.1f kbps\n", fmt_ctx->bit_rate / 1000.0);
    }
    printf("\n");

    // ----------------------------------------------------------
    // 遍历每一路流 (对应 W1/W2 的 STREAM 块)
    // fmt_ctx->streams[] 是指针数组, 每个元素是 AVStream*
    // ----------------------------------------------------------
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream* stream = fmt_ctx->streams[i];
        // ⭐ AVCodecParameters: 编码参数(对应 ffprobe 的 STREAM 信息)
        // 注意: 老版本 FFmpeg 用 stream->codec (AVCodecContext*)
        //       新版本(5.0+)改用 stream->codecpar (AVCodecParameters*)
        //       编码参数用 codecpar, 解码要用 codecpar 去找 AVCodec 再开 AVCodecContext
        AVCodecParameters* codecpar = stream->codecpar;

        printf("━━━━━━ 流 #%d (%s) ━━━━━━\n", i, media_type_name(codecpar->codec_type));

        // codec_id → 编码器名(如 h264/aac)
        // avcodec_get_name() 会返回 "h264"/"aac" 这种短名
        const char* codec_name = avcodec_get_name(codecpar->codec_id);
        printf("  编码格式:   %s\n", codec_name);

        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            // ----- 视频流特有信息 (对应 W1/W3/W4) -----
            printf("  分辨率:     %dx%d\n", codecpar->width, codecpar->height);
            printf("  像素格式:   %s\n", pixfmt_name((AVPixelFormat)codecpar->format));

            // AVRational 是分数(分子/分母), 避免 float 精度问题
            // frame_rate = num/den, 如 30/1 = 30fps
            AVRational fr = stream->r_frame_rate;
            if (fr.den > 0) {
                printf("  帧率:       %.2f fps (%d/%d)\n",
                       av_q2d(fr), fr.num, fr.den);
            }

            // profile/level (对应 W4 SPS 解析!)
            if (codecpar->profile != FF_PROFILE_UNKNOWN) {
                printf("  Profile:    %s\n",
                       avcodec_profile_name(codecpar->codec_id, codecpar->profile));
            }
            if (codecpar->level >= 0) {
                printf("  Level:      %.1f\n", codecpar->level / 10.0);
            }

            // 色彩范围/色彩空间 (对应 W1-3)
            if (codecpar->color_range != AVCOL_RANGE_UNSPECIFIED) {
                printf("  色彩范围:   %s\n",
                       codecpar->color_range == AVCOL_RANGE_JPEG ? "full(PC)" : "limited(TV)");
            }
            if (codecpar->color_space != AVCOL_SPC_UNSPECIFIED) {
                // FFmpeg 没有直接的 av_color_space_name, 用 av_color_range_name 等的姊妹函数
                // 这里手动映射常见的几种
                const char* spc = "other";
                switch (codecpar->color_space) {
                    case AVCOL_SPC_BT709:     spc = "bt709"; break;
                    case AVCOL_SPC_BT470BG:   spc = "bt601"; break;
                    case AVCOL_SPC_BT2020_NCL:spc = "bt2020nc"; break;
                    case AVCOL_SPC_SMPTE170M: spc = "smpte170m"; break;
                    default: break;
                }
                printf("  色彩空间:   %s\n", spc);
            }

            // 码率 (可能为0, 某些容器不存)
            if (codecpar->bit_rate > 0) {
                printf("  视频码率:   %.1f kbps\n", codecpar->bit_rate / 1000.0);
            }
        }
        else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            // ----- 音频流信息 -----
            printf("  采样率:     %d Hz\n", codecpar->sample_rate);
            printf("  声道数:     %d\n", codecpar->ch_layout.nb_channels);
            if (codecpar->bit_rate > 0) {
                printf("  音频码率:   %.1f kbps\n", codecpar->bit_rate / 1000.0);
            }
        }

        // time_base: 这一路流的时间基 (对应 W1-2)
        // 实际秒 = pts × time_base
        printf("  time_base:  %d/%d (1秒=%d刻度)\n",
               stream->time_base.num, stream->time_base.den,
               stream->time_base.den);

        printf("\n");
    }

    // ----------------------------------------------------------
    // 收尾: 必须释放, 否则内存泄漏
    // avformat_free_context 会同时释放 streams[] 和 codecpar
    // ----------------------------------------------------------
    avformat_close_input(&fmt_ctx);  // 关闭文件并释放 fmt_ctx
    printf("[OK] 完成。按回车退出...\n");
    return 0;
}

// ============================================================
// 编译命令 (MinGW g++):
//
// set FF=D:\FFmpeg\ffmpeg-n7.1-latest-win64-gpl-shared-7.1
// g++ 01_probe.cpp -o build\01_probe.exe ^
//     -I%FF%\include ^
//     -L%FF%\lib ^
//     -lavformat -lavcodec -lavutil ^
//     -std=c++17
//
// 运行前要把 %FF%\bin 加到 PATH(否则找不到 avformat-61.dll)
// ============================================================
