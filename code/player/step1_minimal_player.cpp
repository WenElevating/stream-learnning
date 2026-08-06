// =============================================================================
//  step1_minimal_player.cpp —— 最小可视播放器 (Step 1)
// =============================================================================
//  目标: 用 FFmpeg 解码视频 + SDL 显示画面, 让画面动起来。
//
//  刻意简化 (这些在后面 Step 会逐个解决):
//    - 单线程 (主线程里: 解码一帧 -> 显示一帧, 循环)
//    - 固定延时 SDL_Delay(33) 硬假设 30fps (不看 PTS, 不同步)
//    - 只放视频, 无音频
//    - 无队列, 无背压, 无丢帧
//    - 用文件 URL 直接给 FFmpeg, 还没接 IOSource 抽象 (Step2 才接)
//
//  核心学到的知识点 (本文件 4 个关键点):
//    [A] SDL 初始化三件套: Init -> CreateWindow -> CreateRenderer
//    [B] SDL 纹理用 IYUV 格式 == FFmpeg 的 YUV420P, 数据零拷贝
//        (不用转 RGB, GPU 帮你做色彩转换, 省 CPU)
//    [C] SDL_UpdateYUVTexture 三平面分别喂 Y/U/V (不是 UpdateTexture!)
//    [D] FFmpeg 7.x 解码 API: send_packet / receive_frame 循环
//
//  编译 (注意 SDL2 在 msys2 ucrt64, FFmpeg 在 D:\FFmpeg):
//  > cmd.exe //C "set PATH=D:\msys64\ucrt64\bin;D:\FFmpeg\...\bin;%PATH% && ^
//      g++ step1_minimal_player.cpp -o ..\build\step1_player.exe ^
//        -ID:/msys64/ucrt64/include -ID:/FFmpeg/.../include ^
//        -LD:/msys64/ucrt64/lib -LD:/FFmpeg/.../lib ^
//        -lSDL2 -lavformat -lavcodec -lavutil -lswscale -std=c++17"
//
//  运行:
//  > step1_player.exe <视频文件>
//  > step1_player.exe ..\labs\w1_sample.mp4
// =============================================================================

#include <iostream>
#include <string>
#include <chrono>
#include <thread>

// ---- SDL (C 库, extern "C") ----
// [关键] SDL2 在 Windows 上会把 main 宏重定义成 SDL_main, 导致链接器找不到入口.
// 解决: 在 #include <SDL.h> 之前定义 SDL_MAIN_HANDLED, 禁用这个重定义,
//       让我们的 main() 保持标准 int main(int, char**), 不依赖 SDL2main 库.
#define SDL_MAIN_HANDLED
extern "C" {
#include <SDL.h>
}

// ---- FFmpeg (C 库, extern "C") ----
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
}

// 在 Windows 控制台正确显示中文 (你之前踩过的坑)
#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char* argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    if (argc < 2) {
        std::cerr << "用法: " << argv[0] << " <视频文件>\n";
        std::cerr << "例如: " << argv[0] << " ..\\labs\\w1_sample.mp4\n";
        return 1;
    }
    const char* filename = argv[1];

    // =========================================================================
    //  第一阶段: FFmpeg 打开输入 + 找到视频流 + 打开解码器
    //  (这5步是 FFmpeg 解码侧的固定仪式, 后面每个播放器都会重复)
    // =========================================================================
    AVFormatContext* fmtCtx = nullptr;

    // [1] 打开输入文件 + 读容器头
    //     avformat_open_input 会自动探测容器格式 (MP4/FLV/TS...)
    int ret = avformat_open_input(&fmtCtx, filename, nullptr, nullptr);
    if (ret < 0) {
        char err[128];
        av_strerror(ret, err, sizeof(err));
        std::cerr << "[错误] 打不开文件: " << err << "\n";
        return 1;
    }

    // [2] 探测流信息 (解析容器里的音视频流参数: 编码、分辨率、帧率等)
    //     没这步, fmtCtx->streams 里很多字段是空的
    ret = avformat_find_stream_info(fmtCtx, nullptr);
    if (ret < 0) {
        std::cerr << "[错误] 无法获取流信息\n";
        avformat_close_input(&fmtCtx);
        return 1;
    }

    // [3] 找到视频流的 index (容器里可能有多路流: 视频0/音频1/字幕2...)
    //     注意: fmtCtx->streams 是 C 风格数组, 元素个数在 nb_streams 字段里
    int videoIdx = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoIdx = (int)i;
            break;
        }
    }
    if (videoIdx < 0) {
        std::cerr << "[错误] 没找到视频流\n";
        avformat_close_input(&fmtCtx);
        return 1;
    }
    AVStream* vstream = fmtCtx->streams[videoIdx];
    int width  = vstream->codecpar->width;
    int height = vstream->codecpar->height;

    std::cout << "[信息] 视频流: " << width << "x" << height
              << "  编码: " << avcodec_get_name(vstream->codecpar->codec_id) << "\n";

    // [4] 找解码器 + 创建解码器上下文 + 把流参数拷进去
    const AVCodec* codec = avcodec_find_decoder(vstream->codecpar->codec_id);
    if (!codec) {
        std::cerr << "[错误] 找不到解码器\n";
        avformat_close_input(&fmtCtx);
        return 1;
    }
    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codecCtx, vstream->codecpar);

    // [5] 打开解码器 (这步才真正初始化解码器内部状态)
    ret = avcodec_open2(codecCtx, codec, nullptr);
    if (ret < 0) {
        std::cerr << "[错误] 解码器打不开\n";
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return 1;
    }

    // =========================================================================
    //  第二阶段: SDL 初始化 + 创建窗口/渲染器/纹理
    //  (Step1 最重要的知识点在纹理格式这里 ↓)
    // =========================================================================

    // [关键] 所有后续用到的变量在这里"前置声明" (只声明, 不初始化).
    // 原因: C++ 规定 goto 不能跨越带初始值的变量声明. 我们要用 goto 做错误跳转,
    //       所以把 SDL/Frame/Packet 句柄全部前置到 goto 之前, 初始化在后面.
    SDL_Window*   window   = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture*  texture  = nullptr;
    AVPacket*     pkt      = nullptr;
    AVFrame*      frame    = nullptr;
    bool          quitting = false;

    // [A] SDL 初始化: 只需要 VIDEO 子系统 (Step4 加音频时再加 AUDIO)
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "[错误] SDL_Init 失败: " << SDL_GetError() << "\n";
        goto cleanup_ffmpeg;   // 跳到统一的资源释放
    }

    // [B] 创建窗口: 标题 + 位置(居中) + 宽高 + flags
    window = SDL_CreateWindow(
        "Step1 Player",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_SHOWN);
    if (!window) {
        std::cerr << "[错误] 创建窗口失败: " << SDL_GetError() << "\n";
        goto cleanup_sdl;
    }

    // [C] 创建渲染器: 绑定到窗口, 用硬件加速 (-1 = 自动选驱动)
    renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        // vsync 不支持就退到软件渲染
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        std::cerr << "[错误] 创建渲染器失败: " << SDL_GetError() << "\n";
        goto cleanup_sdl;
    }

    // [D] 创建纹理 ★★★ Step1 最重要的知识点 ★★★
    //
    //   为什么用 SDL_PIXELFORMAT_IYUV?
    //     - IYUV == I420, 和 FFmpeg 的 AV_PIX_FMT_YUV420P 内存布局完全一致
    //     - 这样解码出来的 YUV 数据可以直接喂, 不用 sws_scale 转 RGB
    //     - 色彩转换由 GPU 做, 省 CPU 且质量好
    //   为什么用 STREAMING 访问?
    //     - 因为每帧都要更新像素数据 (STATIC 是只设一次的纹理)
    texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_IYUV,
        SDL_TEXTUREACCESS_STREAMING,
        width, height);
    if (!texture) {
        std::cerr << "[错误] 创建纹理失败: " << SDL_GetError() << "\n";
        goto cleanup_sdl;
    }

    // =========================================================================
    //  第三阶段: 主循环 —— 解码一帧 -> 显示一帧
    //  (单线程, 同步阻塞. Step3 会拆成多线程)
    // =========================================================================

    pkt   = av_packet_alloc();
    frame = av_frame_alloc();

    std::cout << "[信息] 开始播放, 按 ESC 或关窗口退出\n";

    while (!quitting) {
        // ---- 3.1 读一个包 ----
        ret = av_read_frame(fmtCtx, pkt);
        if (ret < 0) {
            // ret < 0 通常是 EOF. 单线程播放器到这就结束了.
            // (Step3 多线程版本会把 EOF 发送给解码器来刷缓冲, 这里先简单处理)
            std::cout << "[信息] 播放完毕\n";
            break;
        }

        // 只处理视频包 (音频包直接丢掉, Step4 才处理)
        if (pkt->stream_index != videoIdx) {
            av_packet_unref(pkt);
            continue;
        }

        // ---- 3.2 送进解码器 ----
        // send_packet 把包塞进解码器输入端. 返回 EAGAIN 表示解码器满,
        // 需要先 receive_frame 把缓存的帧取出来. 这里循环到送进去为止.
        while ((ret = avcodec_send_packet(codecCtx, pkt)) == AVERROR(EAGAIN)) {
            // 解码器满, 先 drain
            while (avcodec_receive_frame(codecCtx, frame) == 0) {
                // 这里的帧先丢弃, 因为当前包还没进去
                av_frame_unref(frame);
            }
        }
        av_packet_unref(pkt);   // 送完立即释放包内存

        if (ret < 0 && ret != AVERROR_EOF) {
            char err[128];
            av_strerror(ret, err, sizeof(err));
            std::cerr << "[警告] send_packet 错误: " << err << "\n";
            continue;
        }

        // ---- 3.3 取解码后的帧 ----
        // 一个 packet 可能产生 0 或多帧 (B帧重排, 见你笔记 W1)
        // 这里取所有能取出来的帧, 每帧显示一次
        while (avcodec_receive_frame(codecCtx, frame) == 0) {
            // 确认是 YUV420P. 不是的话 Step1 先报警告 (理论上 H.264 默认输出 YUV420P)
            if (frame->format != AV_PIX_FMT_YUV420P) {
                std::cerr << "[警告] 像素格式不是 YUV420P (是 "
                          << av_get_pix_fmt_name((AVPixelFormat)frame->format)
                          << "), Step1 不处理\n";
                av_frame_unref(frame);
                continue;
            }

            // ---- 3.4 把 YUV 三平面上传到纹理 ----
            // SDL_UpdateYUVTexture: 分别传 Y/U/V 三个平面的数据和行字节数
            // (frame->data[0]=Y, [1]=U, [2]=V; linesize 是每行字节数)
            SDL_UpdateYUVTexture(texture, nullptr,
                frame->data[0], frame->linesize[0],   // Y 平面
                frame->data[1], frame->linesize[1],   // U 平面
                frame->data[2], frame->linesize[2]);  // V 平面

            // ---- 3.5 渲染呈现 ----
            SDL_RenderClear(renderer);           // 清屏(背景)
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);  // 拷贝纹理到渲染目标
            SDL_RenderPresent(renderer);         // 把后台缓冲翻到屏幕

            av_frame_unref(frame);   // ★ 用完必须 unref, 否则内存泄漏 (#2 常见坑)

            // ---- 3.6 延时到下一帧 (固定 33ms, 假设 30fps) ----
            // ★ 这是 Step1 最大的简化 ★
            // 真实播放器按 PTS 延时 (Step4 同步时做), 这里硬编码 33ms
            // → 后果: 视频可能播放速度不准 (源不是 30fps 就偏)
            SDL_Delay(33);
        }

        // ---- 3.7 处理 SDL 事件 (ESC/关窗口) ----
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                quitting = true;
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                quitting = true;
    }
        }
    }

    // =========================================================================
    //  第四阶段: 资源释放 (注意顺序, 后申请的先释放)
    // =========================================================================
    av_frame_free(&frame);
    av_packet_free(&pkt);

cleanup_sdl:
    if (texture)  SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window)   SDL_DestroyWindow(window);
    SDL_Quit();

cleanup_ffmpeg:
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    return 0;
}
