// =============================================================================
//  step2_modular_player.cpp —— 模块化播放器 (Step 2)
// =============================================================================
//
//  【对比 Step1】
//    功能完全一样: 单线程, 固定 30fps 延时, 只放视频.
//    区别在代码结构: 拆成 4 个职责隔离的模块.
//
//  【架构】(对比 Step1 的"面条代码")
//
//    main 组装这 4 个模块, 模块之间通过明确的接口通信:
//
//      IOFile (IOSource 实现)
//        │ 字节
//        ▼
//      Demuxer ── avio_alloc_context 桥接 IOSource
//        │ AVPacket
//        ▼
//      Decoder ── send/receive
//        │ AVFrame (YUV420P)
//        ▼
//      VideoRenderer ── SDL 显示
//
//  【IO 可替换的证明】
//    main 里只要把 IOFile 换成另一个 IOSource 子类 (比如 IOMemory),
//    其余代码一行都不用改 —— 这就是 Step5 要验证的事.
//
//  编译: 见 build_player.bat
//  运行: step2_player.exe <视频文件>
// =============================================================================
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "IOFile.h"
#include "Demuxer.h"
#include "Decoder.h"
#include "VideoRenderer.h"

// av_pix_fmt_desc_get (FFmpeg 7.x 用来替代 av_get_pix_fmt_name)
extern "C" {
#include <libavutil/pixdesc.h>
}

int main(int argc, char* argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    if (argc < 2) {
        std::cerr << "用法: " << argv[0] << " <视频文件>\n";
        return 1;
    }
    const std::string filename = argv[1];

    // =========================================================================
    //  组装阶段: 创建 4 个模块, 按顺序 open
    //  注意每个模块只 open 一次, 失败就清理已 open 的, 干净退出.
    // =========================================================================

    // [1] IO —— 创建文件 IO 源并打开
    IOFile io;
    if (!io.open(filename)) {
        std::cerr << "[错误] 打不开文件: " << filename << "\n";
        return 1;
    }
    std::cout << "[IO] 文件已打开, 大小: " << io.size() << " 字节\n";

    // [2] Demuxer —— 接 IOSource, 解封装
    Demuxer demuxer;
    if (!demuxer.open(&io)) {
        std::cerr << "[错误] 解封装失败\n";
        io.close();
        return 1;
    }
    int vIdx = demuxer.videoStreamIndex();
    auto* vParams = demuxer.streamParameters(vIdx);
    std::cout << "[Demuxer] 视频流#" << vIdx << ": "
              << vParams->width << "x" << vParams->height << " "
              << avcodec_get_name(vParams->codec_id) << "\n";

    // [3] Decoder —— 用视频流参数初始化
    Decoder decoder;
    if (!decoder.open(vParams)) {
        std::cerr << "[错误] 解码器初始化失败\n";
        demuxer.close();
        io.close();
        return 1;
    }
    // FFmpeg 7.x 移除了 av_get_pix_fmt_name, 用 av_pix_fmt_desc_get()->name 代替
    const char* pixFmtName = av_pix_fmt_desc_get(decoder.pixelFormat())->name;
    std::cout << "[Decoder] 就绪, 像素格式: " << pixFmtName << "\n";

    // [4] Renderer —— 用解码后的分辨率初始化
    VideoRenderer renderer;
    if (!renderer.open(decoder.width(), decoder.height())) {
        std::cerr << "[错误] 渲染器初始化失败\n";
        decoder.close();
        demuxer.close();
        io.close();
        return 1;
    }

    // =========================================================================
    //  主循环: Demuxer 读包 → Decoder 解码 → Renderer 显示
    //  对比 Step1: 逻辑完全一样, 但现在每一步都是模块的方法调用,
    //              读起来就像"流水线作业", 而不是一大坨 FFmpeg API.
    // =========================================================================
    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();

    std::cout << "[信息] 开始播放, 按 ESC 或关窗口退出\n";

    bool quitting = false;
    while (!quitting) {
        // ---- 1. 读一个包 ----
        if (!demuxer.readPacket(pkt)) {
            std::cout << "[信息] 播放完毕\n";
            break;
        }

        // 只处理视频包 (音频包 Step4 才处理, 这里跳过)
        if (pkt->stream_index != vIdx) {
            av_packet_unref(pkt);
            continue;
        }

        // ---- 2. 送包 + 取所有帧 ----
        int ret = decoder.sendPacket(pkt);
        av_packet_unref(pkt);

        // send 返回 EAGAIN: 解码器满, 先 drain
        if (ret == AVERROR(EAGAIN)) {
            while (decoder.receiveFrame(frame) == 0) {
                av_frame_unref(frame);
            }
            continue;
        }
        if (ret < 0) continue;

        // ---- 3. 取出每一帧并显示 ----
        while (decoder.receiveFrame(frame) == 0) {
            // 像素格式检查 (Step2 仍假设 YUV420P, 非此格式跳过)
            if (frame->format != AV_PIX_FMT_YUV420P) {
                const char* fmtName = av_pix_fmt_desc_get((AVPixelFormat)frame->format)->name;
                std::cerr << "[警告] 非预期像素格式: " << fmtName << "\n";
                av_frame_unref(frame);
                continue;
            }

            // 显示这一帧. 返回 false = 收到退出事件
            if (!renderer.show(frame)) {
                quitting = true;
            }
            av_frame_unref(frame);

            // 固定延时 33ms (假设 30fps. Step4 改成按 PTS)
            renderer.delay(33);
        }
    }

    // =========================================================================
    //  释放: 反序关闭 (后 open 的先 close)
    // =========================================================================
    av_frame_free(&frame);
    av_packet_free(&pkt);
    renderer.close();
    decoder.close();
    demuxer.close();
    io.close();

    std::cout << "[信息] 已退出\n";
    return 0;
}
