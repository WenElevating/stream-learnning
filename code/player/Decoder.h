// =============================================================================
//  Decoder.h —— 解码模块 (AVPacket → AVFrame)
// =============================================================================
//
//  【职责】
//    只负责: 把某一路流的编码包 (H.264/AAC...) 解成原始帧 (YUV/PCM)
//    绝不碰: 包从哪来 (Demuxer 的事), 帧到哪去显示 (Renderer 的事)
//
//  【设计】
//    一个 Decoder 实例对应一路流 (视频流一个实例, 音频流另一个实例).
//    open() 时从 Demuxer 拿该流的 AVCodecParameters, 自动找解码器并初始化.
//    外部循环: sendPacket() → while(receiveFrame()) 处理每一帧.
//
//  【为什么 send/receive 解耦】
//    H.264 有 B 帧重排序, 解码器内部要缓存若干帧. 所以送一个包不一定立即出一帧,
//    可能出 0 帧也可能出多帧. send/receive 分离让这个语义清晰:
//      send: 我给你包, 你先存着
//      receive: 有没有解好的帧? (EAGAIN=没有, 再送包来; 0=有, 拿去)
// =============================================================================
#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
}

class Decoder {
public:
    Decoder();
    ~Decoder();

    // 从 Demuxer 的流参数初始化解码器. 成功后 width/height/format 可用.
    bool open(AVCodecParameters* params);

    // 送一个包进解码器. 返回值:
    //   0       = 成功, 可以 receiveFrame
    //   EAGAIN  = 解码器满, 先 drain (循环 receiveFrame 直到 EAGAIN)
    //   <0 其他 = 真错误
    int sendPacket(AVPacket* pkt);

    // 取一帧. 返回 0=成功, AVERROR(EAGAIN)=没帧了, 其他负值=错误.
    int receiveFrame(AVFrame* frame);

    // ---- 解码后的帧信息 (open 后可用) ----
    int width()  const { return m_codecCtx ? m_codecCtx->width  : 0; }
    int height() const { return m_codecCtx ? m_codecCtx->height : 0; }
    AVPixelFormat pixelFormat() const {
        return m_codecCtx ? (AVPixelFormat)m_codecCtx->pix_fmt : AV_PIX_FMT_NONE;
    }

    // 暴露内部 codecCtx 给协调器用 (比如 EOF 时需要 send NULL 冲洗解码器).
    // 这是受控的"逃生舱口", 仅限内部协调逻辑使用.
    AVCodecContext* codecCtx() { return m_codecCtx; }

    void close();

private:
    AVCodecContext* m_codecCtx;
};
