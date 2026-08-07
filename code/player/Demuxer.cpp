// =============================================================================
//  Demuxer.cpp —— 解封装实现
// =============================================================================
#include "Demuxer.h"
#include <iostream>

// -----------------------------------------------------------------------------
// avio 回调: 读. 签名由 FFmpeg 规定, opaque 是我们 avio_alloc_context 时传的指针.
// 这里把 opaque 转回 IOSource*, 转发它的 read(). 返回值约定一致 (0=EOF, <0=错).
// -----------------------------------------------------------------------------
static int avioReadCallback(void* opaque, uint8_t* buf, int bufSize) {
    IOSource* io = static_cast<IOSource*>(opaque);
    return io->read(buf, bufSize);
}

// -----------------------------------------------------------------------------
// avio 回调: 定位. whence 可能是 SEEK_SET/CUR/END 或 AVSEEK_SIZE (问大小).
// 转发给 IOSource->seek().
// -----------------------------------------------------------------------------
static int64_t avioSeekCallback(void* opaque, int64_t offset, int whence) {
    IOSource* io = static_cast<IOSource*>(opaque);
    return io->seek(offset, whence);
}

// =============================================================================
Demuxer::Demuxer()
    : m_fmt(nullptr), m_avio(nullptr), m_avioBuf(nullptr),
      m_io(nullptr), m_videoIdx(-1), m_audioIdx(-1) {}

Demuxer::~Demuxer() { close(); }

bool Demuxer::open(IOSource* io) {
    m_io = io;
    if (!m_io || !m_io->isOpen()) {
        std::cerr << "[Demuxer] IOSource 未打开\n";
        return false;
    }

    // ---- [1] 分配 avio 缓冲 + 创建自定义 AVIOContext ----
    // 缓冲大小: 32KB (网络用 32-64KB 合适; 文件小点也行, 但太小会频繁回调)
    const int AVIO_BUF_SIZE = 32 * 1024;
    m_avioBuf = (uint8_t*)av_malloc(AVIO_BUF_SIZE);
    if (!m_avioBuf) return false;

    // avio_alloc_context 参数:
    //   buffer, buffer_size, write_flag(0=读), opaque(传给回调的指针),
    //   read_packet, write_packet, seek
    m_avio = avio_alloc_context(m_avioBuf, AVIO_BUF_SIZE, 0, m_io,
                                avioReadCallback, nullptr, avioSeekCallback);
    if (!m_avio) {
        av_free(m_avioBuf);
        m_avioBuf = nullptr;
        return false;
    }

    // ---- [2] 创建 AVFormatContext 并挂上自定义 IO ----
    m_fmt = avformat_alloc_context();
    if (!m_fmt) return false;
    m_fmt->pb = m_avio;     // ★ 关键: 把自定义 IO 接到 FFmpeg 上
    m_fmt->flags |= AVFMT_FLAG_CUSTOM_IO;  // 告诉 FFmpeg: pb 是我自己的, 你别乱动

    // 限制 find_stream_info 的分析时长, 防止某些容器(如 FLV)在网络/自定义IO场景下
    // 因为探测不到足够信息而长时间阻塞. 5 秒足够绝大多数情况.
    m_fmt->max_analyze_duration = 5 * AV_TIME_BASE;   // AV_TIME_BASE=1000000, 即 5 秒

    // ---- [3] 打开容器 (url 传 nullptr, 因为数据从 pb 来) ----
    // 注意: avformat_open_input 内部会调 avio 的回调去读字节探测格式.
    //       这一步你的 IOSource.read() 已经在被调用了!
    int ret = avformat_open_input(&m_fmt, nullptr, nullptr, nullptr);
    if (ret < 0) {
        char err[128]; av_strerror(ret, err, sizeof(err));
        std::cerr << "[Demuxer] avformat_open_input 失败: " << err << "\n";
        return false;
    }

    // ---- [4] 探测流信息 (解析流的编码参数) ----
    ret = avformat_find_stream_info(m_fmt, nullptr);
    if (ret < 0) {
        std::cerr << "[Demuxer] avformat_find_stream_info 失败\n";
        return false;
    }

    // ---- [5] 记录视频/音频流 index ----
    if (!findStreams()) {
        std::cerr << "[Demuxer] 没找到任何可用的视频/音频流\n";
        return false;
    }

    return true;
}

bool Demuxer::findStreams() {
    for (unsigned i = 0; i < m_fmt->nb_streams; ++i) {
        AVMediaType type = m_fmt->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && m_videoIdx < 0) {
            m_videoIdx = (int)i;
        } else if (type == AVMEDIA_TYPE_AUDIO && m_audioIdx < 0) {
            m_audioIdx = (int)i;
        }
    }
    return m_videoIdx >= 0;   // 至少要有视频流
}

bool Demuxer::readPacket(AVPacket* pkt) {
    if (!m_fmt) return false;
    int ret = av_read_frame(m_fmt, pkt);
    if (ret < 0) {
        // EOF 或错误都返回 false (Step3 多线程时再区分)
        return false;
    }
    return true;
}

AVCodecParameters* Demuxer::streamParameters(int streamIndex) const {
    if (!m_fmt || streamIndex < 0 || streamIndex >= (int)m_fmt->nb_streams) return nullptr;
    return m_fmt->streams[streamIndex]->codecpar;
}

AVStream* Demuxer::stream(int streamIndex) const {
    if (!m_fmt || streamIndex < 0 || streamIndex >= (int)m_fmt->nb_streams) return nullptr;
    return m_fmt->streams[streamIndex];
}

void Demuxer::close() {
    // ★ 释放顺序很重要 ★
    // 1. 先关 fmt (它内部引用了 avio 的缓冲)
    if (m_fmt) {
        // avformat_close_input 会判断 flags&AVFMT_FLAG_CUSTOM_IO,
        // 如果是自定义 IO 就不释放 pb (pb 是我们 avio_alloc 的, 要自己 avio_context_free)
        avformat_close_input(&m_fmt);
    }
    // 2. 释放 avio (它持有 m_avioBuf 的引用)
    if (m_avio) {
        avio_context_free(&m_avio);
        m_avio = nullptr;
    }
    // 注意: avio_context_free 会顺便 free 掉 buffer, 所以 m_avioBuf 不用再单独 free.
    //       (但如果 FFmpeg 内部把 buffer realloc 过, avio_context_free 处理的是 realloc 后的指针,
    //        这时我们存的 m_avioBuf 已失效 —— 不过 avio_context_free 内部记的是真实指针, 没问题)
    m_avioBuf = nullptr;
    m_io = nullptr;
    m_videoIdx = -1;
    m_audioIdx = -1;
}
