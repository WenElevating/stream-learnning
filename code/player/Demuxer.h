// =============================================================================
//  Demuxer.h —— 解封装模块 (持有 IOSource, 字节流 → AVPacket)
// =============================================================================
//
//  【职责】(严格隔离)
//    只负责: 打开容器 + 读包 + 暴露流信息 (分辨率/编码格式等)
//    绝不碰: 解码 (那是 Decoder 的事), 显示 (那是 Renderer 的事)
//
//  【关键设计: IOSource → FFmpeg 的桥接】
//  Demuxer 不直接 fopen 文件, 而是持有外部传进来的 IOSource*.
//  然后用 avio_alloc_context 把 IOSource 的 read/seek 包成 C 回调,
//  挂到 AVFormatContext->pb 上. 这样 FFmpeg 的解封装逻辑完全不知道
//  字节是从文件、内存还是网络来的 —— 这就是"IO 无关".
//
//  【数据流】
//    IOSource.read()  ←── 字节来自哪里由具体子类决定 (文件/内存/网络)
//         │
//         ▼
//    avio 回调 (C 函数, 转发给 IOSource)
//         │
//         ▼
//    AVFormatContext (FFmpeg 解析容器, 拆包)
//         │
//         ▼
//    Demuxer.readPacket() 返回 AVPacket (带 stream_index, 标明属于哪路流)
// =============================================================================
#pragma once

#include "IOSource.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>   // AVCodecParameters 在这里
}

class Demuxer {
public:
    Demuxer();
    ~Demuxer();

    // 打开数据源. io 由外部创建并拥有 (Demuxer 只借用, 不负责释放).
    // 成功后可通过 streamInfo() 拿到流参数.
    bool open(IOSource* io);

    // 读一个包. 返回 false 表示 EOF 或错误.
    // 注意: 调用方拿到 pkt 后, 用完必须 av_packet_unref(pkt).
    bool readPacket(AVPacket* pkt);

    // ---- 流信息查询 (open 成功后才可用) ----
    // 找指定类型的流 index (video/audio). 找不到返回 -1.
    int videoStreamIndex() const { return m_videoIdx; }
    int audioStreamIndex() const { return m_audioIdx; }

    // 拿某路流的参数 (分辨率/编码等). 用于 Decoder 初始化.
    AVCodecParameters* streamParameters(int streamIndex) const;

    // 拿流对象本身 (Decoder 需要它的 time_base 等做时间戳换算)
    AVStream* stream(int streamIndex) const;

    void close();

private:
    AVFormatContext* m_fmt;          // FFmpeg 解封装上下文
    AVIOContext*     m_avio;         // 自定义 IO 上下文 (桥接 IOSource)
    uint8_t*         m_avioBuf;      // avio 内部缓冲 (av_malloc 分配)
    IOSource*        m_io;           // 外部传入的字节源 (不拥有)
    int              m_videoIdx;     // 视频流 index
    int              m_audioIdx;     // 音频流 index

    bool findStreams();              // 扫描容器, 记录视频/音频流 index
};
