// =============================================================================
//  Player.cpp —— 协调器实现
// =============================================================================
#include "Player.h"
#include "IOFile.h"
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

Player::Player() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
}

Player::~Player() { close(); }

bool Player::open(const std::string& url) {
    // =========================================================================
    //  阶段 1: 初始化各模块 (复用 Step2 的代码, 顺序一致)
    // =========================================================================

    // [IO] 创建文件 IO 源 (Step5 会换成可注入的 IOSource)
    m_io = new IOFile();
    m_ownIO = true;
    if (!m_io->open(url)) {
        std::cerr << "[Player] 打不开: " << url << "\n";
        return false;
    }

    // [Demuxer] 解封装
    if (!m_demuxer.open(m_io)) {
        std::cerr << "[Player] 解封装失败\n";
        return false;
    }
    m_videoIdx = m_demuxer.videoStreamIndex();

    // [Decoder] 视频解码器
    if (!m_decoder.open(m_demuxer.streamParameters(m_videoIdx))) {
        std::cerr << "[Player] 解码器初始化失败\n";
        return false;
    }

    // [Renderer] SDL 窗口
    if (!m_renderer.open(m_decoder.width(), m_decoder.height())) {
        std::cerr << "[Player] 渲染器初始化失败\n";
        return false;
    }

    // [队列] 配置容量
    m_packetQueue.setMaxBytes(16 * 1024 * 1024);  // 16MB (Step3 只视频, 给大点)

    std::cout << "[Player] 就绪 " << m_decoder.width() << "x" << m_decoder.height()
              << "  视频流#" << m_videoIdx << "\n";

    // =========================================================================
    //  阶段 2: 启动工作线程
    // =========================================================================
    m_quit.store(false);
    m_eofReached.store(false);
    m_opened = true;

    m_readThread   = std::thread(&Player::readThread, this);
    m_decodeThread = std::thread(&Player::decodeThread, this);

    return true;
}

// =============================================================================
//  read 线程: 解封装 → PacketQueue
//  职责单一: 只负责读包扔进队列. 不关心后面怎么解码.
// =============================================================================
void Player::readThread() {
    AVPacket* pkt = av_packet_alloc();
    while (!m_quit.load()) {
        // ---- 读一个包 ----
        if (!m_demuxer.readPacket(pkt)) {
            // EOF 或错误: 推一个"空包"作为 EOF 标记给 decode 线程
            // (decode 线程见到 data==nullptr && size==0 就 send NULL 冲洗解码器)
            // FFmpeg 7.x 不再用 av_init_packet, 直接用 alloc 出来的空包即可.
            AVPacket* eof = av_packet_alloc();   // alloc 出来 data=nullptr,size=0,正好当 EOF 标记
            m_packetQueue.push(eof);
            av_packet_free(&eof);
            m_eofReached.store(true);
            std::cout << "[read] EOF, 已通知 decode 线程\n";
            break;
        }

        // 只把视频包入队 (音频包 Step4 处理, 这里丢弃)
        if (pkt->stream_index != m_videoIdx) {
            av_packet_unref(pkt);
            continue;
        }

        // ---- 入队 (满了会阻塞, 形成背压) ----
        if (!m_packetQueue.push(pkt)) {
            break;   // quit 被设置
        }
    }
    av_packet_free(&pkt);
    std::cout << "[read] 线程退出\n";
}

// =============================================================================
//  decode 线程: PacketQueue → 解码 → FrameQueue
// =============================================================================
void Player::decodeThread() {
    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();

    while (!m_quit.load()) {
        // ---- 从队列取一个包 ----
        if (!m_packetQueue.pop(pkt)) {
            break;   // quit 且队列空
        }

        // ---- 处理 EOF 标记包 ----
        // read 线程读到 EOF 时会推一个空包. 这里检测到就送 NULL 冲洗解码器,
        // 把 H.264 重排缓冲里的最后几帧挤出来.
        if (pkt->data == nullptr && pkt->size == 0) {
            avcodec_send_packet(m_decoder.codecCtx(), nullptr);   // 冲洗
            // 取出所有剩余帧
            while (avcodec_receive_frame(m_decoder.codecCtx(), frame) == 0) {
                if (!m_frameQueue.put(frame)) break;
                av_frame_unref(frame);
            }
            av_packet_unref(pkt);
            m_frameQueue.signalQuit();   // 通知 FrameQueue 没有更多数据了
            std::cout << "[decode] 处理完 EOF, 线程退出\n";
            break;
        }

        // ---- 正常包: 送解码器 ----
        int ret = m_decoder.sendPacket(pkt);
        av_packet_unref(pkt);
        if (ret == AVERROR(EAGAIN)) {
            // 解码器满, 先 drain. 这里 drain 出来的帧也要入队.
            while (m_decoder.receiveFrame(frame) == 0) {
                if (!m_frameQueue.put(frame)) break;
                av_frame_unref(frame);
            }
            continue;
        }
        if (ret < 0) continue;

        // ---- 取所有解出的帧, 入 FrameQueue ----
        while (m_decoder.receiveFrame(frame) == 0) {
            if (!m_frameQueue.put(frame)) {
                av_frame_unref(frame);
                break;
            }
            av_frame_unref(frame);
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    std::cout << "[decode] 线程退出\n";
}

// =============================================================================
//  主线程渲染: 从 FrameQueue 取一帧 → 显示 + 处理事件
// =============================================================================
bool Player::runFrame() {
    if (!m_opened) return false;

    // ---- 从 FrameQueue 取一帧 ----
    AVFrame* frame = m_frameQueue.acquire();
    if (frame == nullptr) {
        // quit 且队列空, 没帧了
        return false;
    }

    // ---- 显示 ----
    bool ok = m_renderer.show(frame);
    m_frameQueue.release();

    if (!ok) {
        // SDL 事件说该退出了
        return false;
    }

    // 固定延时 (Step4 改成按 PTS 同步)
    m_renderer.delay(33);

    return true;
}

// =============================================================================
//  关闭: 通知退出 → join 线程 → 释放资源 (反序)
// =============================================================================
void Player::close() {
    if (!m_opened) {
        // open 失败的兜底清理 (可能部分模块已初始化)
        m_renderer.close();
        m_decoder.close();
        m_demuxer.close();
        if (m_io) { m_io->close(); if (m_ownIO) delete m_io; m_io = nullptr; }
        return;
    }

    // 1. 设退出信号
    m_quit.store(true);
    m_packetQueue.signalQuit();
    m_frameQueue.signalQuit();

    // 2. 等工作线程结束 (它们在队列阻塞, signalQuit 会唤醒它们)
    if (m_readThread.joinable())   m_readThread.join();
    if (m_decodeThread.joinable()) m_decodeThread.join();

    // 3. 释放资源 (反序: 后 open 的先 close)
    m_renderer.close();
    m_decoder.close();
    m_demuxer.close();
    if (m_io) {
        m_io->close();
        if (m_ownIO) delete m_io;
        m_io = nullptr;
    }
    m_opened = false;
    std::cout << "[Player] 已关闭\n";
}
