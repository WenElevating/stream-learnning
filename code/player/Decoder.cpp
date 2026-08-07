// =============================================================================
//  Decoder.cpp —— 解码实现
// =============================================================================
#include "Decoder.h"
#include <iostream>

Decoder::Decoder() : m_codecCtx(nullptr) {}
Decoder::~Decoder() { close(); }

bool Decoder::open(AVCodecParameters* params) {
    if (!params) return false;

    // [1] 按 codec_id 找解码器 (H.264 → 找 h264 解码器, AAC → 找 aac 解码器)
    const AVCodec* codec = avcodec_find_decoder(params->codec_id);
    if (!codec) {
        std::cerr << "[Decoder] 找不到解码器: codec_id=" << params->codec_id << "\n";
        return false;
    }

    // [2] 分配解码器上下文
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) return false;

    // [3] 把流参数 (分辨率/像素格式/extradata 等) 拷到解码器上下文
    //     这步是必须的, 解码器需要从 extradata 里拿 SPS/PPS 才能解 H.264
    if (avcodec_parameters_to_context(m_codecCtx, params) < 0) {
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    // [4] 打开解码器 (初始化内部状态, 分配解码缓冲等)
    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        std::cerr << "[Decoder] avcodec_open2 失败\n";
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    return true;
}

int Decoder::sendPacket(AVPacket* pkt) {
    if (!m_codecCtx) return AVERROR(EINVAL);
    return avcodec_send_packet(m_codecCtx, pkt);
}

int Decoder::receiveFrame(AVFrame* frame) {
    if (!m_codecCtx) return AVERROR(EINVAL);
    return avcodec_receive_frame(m_codecCtx, frame);
}

void Decoder::close() {
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
    }
}
