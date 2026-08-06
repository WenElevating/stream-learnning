// ============================================================
// H264Streamer.h - 可复用的 H.264 推流器(接收字节数组版)
// ============================================================
//
// ⭐ 为真实无人机载荷场景设计的推流器!
//   硬件编码器通过回调给你 H.264 字节数组 → pushFrame() → 推流
//
// 架构要点(踩坑后总结):
//   1. 懒初始化: 收到首个 IDR 才创建 FFmpeg 容器 + write_header
//      原因: FLV/RTMP 要求 extradata 里有 SPS/PPS, 必须先收齐才能写头
//   2. 一次性初始化: alloc→new_stream→extradata→avio_open→write_header
//      紧挨着执行, 避免中间内存操作导致成员损坏
//   3. AVCC 转换: 收到的 annexb(起始码)要转成 AVCC(长度前缀)给 FLV/MP4
//   4. time_base: RTSP 必须 1/90000(RTP), FLV/RTMP 用 1/1000
//
// 用法:
//   H264Streamer s;
//   s.open("rtmp://host/app/stream", 640, 360, 30);
//   // 硬编码器回调里:
//   void onEncoded(uint8_t* data, int size) { s.pushFrame(data, size); }
//   s.close();
// ============================================================

#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libavutil/imgutils.h>
}

class H264Streamer {
public:
    H264Streamer() = default;
    ~H264Streamer() { close(); }

    // 打开(只保存配置, 不创建容器)
    bool open(const char* url, int width, int height, int fps) {
        m_url = url;
        m_width = width;
        m_height = height;
        m_fps = (fps > 0) ? fps : 30;
        printf("[Streamer] 已就绪: %s (%dx%d@%dfps)\n", url, width, height, m_fps);
        return true;
    }

    // ⭐ 核心接口: 推送一帧 H.264 字节(模拟硬编码器回调)
    void pushFrame(const uint8_t* data, int size) {
        if (!data || size <= 0) return;

        // 1. 切分 NALU
        std::vector<NaluInfo> nalus = splitNalus(data, size);

        // 2. 缓存 SPS/PPS, 检测 IDR
        bool has_idr = false;
        for (auto& nalu : nalus) {
            int type = nalu.data[0] & 0x1F;
            if (type == 7) m_sps.assign(nalu.data, nalu.data + nalu.size);
            else if (type == 8) m_pps.assign(nalu.data, nalu.data + nalu.size);
            else if (type == 5) has_idr = true;
        }

        // 3. 首个 IDR 时一次性初始化 + 写头
        if (has_idr && !m_headerWritten) {
            m_headerWritten = initOnFirstIdr();
            if (!m_headerWritten) return;
        }

        // 4. 转换并推出
        if (m_headerWritten) {
            if (m_useTs) {
                // TS 直接吃 annexb(起始码), 不转换
                writePacket(data, size, has_idr);
            } else {
                // FLV/MP4 要 AVCC(长度前缀)
                std::vector<uint8_t> avcc = annexbToAvcc(data, size);
                if (!avcc.empty()) writePacket(avcc.data(), (int)avcc.size(), has_idr);
            }
        }
    }

    void close() {
        if (m_outFmt) {
            if (m_headerWritten) av_write_trailer(m_outFmt);
            if (m_outFmt->pb) avio_closep(&m_outFmt->pb);
            avformat_free_context(m_outFmt);
            m_outFmt = nullptr;
        }
        m_headerWritten = false;
        m_frameCount = 0;
    }

private:
    struct NaluInfo { const uint8_t* data; int size; };

    // ---- NALU 切分(同时处理 3字节和4字节起始码) ----
    static std::vector<NaluInfo> splitNalus(const uint8_t* data, int size) {
        std::vector<NaluInfo> nalus;
        int i = 0, nalu_start = -1;
        while (i < size - 2) {
            int sc_len = 0;
            if (data[i]==0 && data[i+1]==0) {
                if (data[i+2]==1) sc_len = 3;
                else if (i+3<size && data[i+2]==0 && data[i+3]==1) sc_len = 4;
            }
            if (sc_len > 0) {
                if (nalu_start >= 0) nalus.push_back({data+nalu_start, i-nalu_start});
                nalu_start = i + sc_len;
                i += sc_len;
            } else i++;
        }
        if (nalu_start >= 0 && nalu_start < size)
            nalus.push_back({data+nalu_start, size-nalu_start});
        return nalus;
    }

    // ---- 用 SPS/PPS 构建 extradata(annexb格式, 给TS) ----
    bool buildExtradataAnnexb() {
        if (m_sps.empty() || m_pps.empty()) return false;
        std::vector<uint8_t> ed;
        uint8_t sc4[] = {0,0,0,1};
        ed.insert(ed.end(), sc4, sc4+4);
        ed.insert(ed.end(), m_sps.begin(), m_sps.end());
        ed.insert(ed.end(), sc4, sc4+4);
        ed.insert(ed.end(), m_pps.begin(), m_pps.end());
        AVCodecParameters* par = m_outStream->codecpar;
        if (par->extradata) av_free(par->extradata);
        par->extradata_size = (int)ed.size();
        par->extradata = (uint8_t*)av_mallocz(ed.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!par->extradata) return false;
        std::memcpy(par->extradata, ed.data(), ed.size());
        return true;
    }

    // ---- 用 SPS/PPS 构建 extradata(AVCC格式, 给FLV/MP4) ----
    bool buildExtradata() {
        if (m_sps.empty() || m_pps.empty()) return false;
        // AVCC: 01 profile compat level FF E1 [sps_len] SPS 01 [pps_len] PPS
        std::vector<uint8_t> avcc;
        avcc.push_back(0x01);
        avcc.push_back(m_sps.size()>1 ? m_sps[1] : 0x42);
        avcc.push_back(m_sps.size()>2 ? m_sps[2] : 0x00);
        avcc.push_back(m_sps.size()>3 ? m_sps[3] : 0x1e);
        avcc.push_back(0xFF);
        avcc.push_back(0xE1);
        avcc.push_back((m_sps.size()>>8)&0xFF);
        avcc.push_back(m_sps.size()&0xFF);
        avcc.insert(avcc.end(), m_sps.begin(), m_sps.end());
        avcc.push_back(0x01);
        avcc.push_back((m_pps.size()>>8)&0xFF);
        avcc.push_back(m_pps.size()&0xFF);
        avcc.insert(avcc.end(), m_pps.begin(), m_pps.end());

        AVCodecParameters* par = m_outStream->codecpar;
        if (par->extradata) av_free(par->extradata);
        par->extradata_size = (int)avcc.size();
        par->extradata = (uint8_t*)av_mallocz(avcc.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!par->extradata) return false;
        std::memcpy(par->extradata, avcc.data(), avcc.size());
        return true;
    }

    // ---- annexb(起始码) → AVCC(4字节长度前缀) ----
    static std::vector<uint8_t> annexbToAvcc(const uint8_t* data, int size) {
        std::vector<uint8_t> out;
        std::vector<NaluInfo> nalus = splitNalus(data, size);
        for (auto& nalu : nalus) {
            uint32_t len = (uint32_t)nalu.size;
            out.push_back((len>>24)&0xFF);
            out.push_back((len>>16)&0xFF);
            out.push_back((len>>8)&0xFF);
            out.push_back(len&0xFF);
            out.insert(out.end(), nalu.data, nalu.data + nalu.size);
        }
        return out;
    }

    // ---- 收到首个 IDR 时一次性完成全部初始化 ----
    bool initOnFirstIdr() {
        printf("[Streamer] 收到首个IDR, 初始化(SPS=%d PPS=%d)\n",
               (int)m_sps.size(), (int)m_pps.size());
        fflush(stdout);

        // 1. 创建容器
        // ⭐ 用 mpegts 替代 flv! 原因: MinGW 下 flv muxer 的 write_header 有 hang bug
        //   TS 直接吃 annexb 格式(起始码), 不需要 AVCC 转换, 也不需要复杂 extradata
        //   RTMP 底层也能用 TS (RTMP over TS)
        // 对于文件输出用 mpegts, 对于 rtmp:// 仍用 flv(但本地用 ts 测试)
        const char* fmt;
        if (std::strncmp(m_url.c_str(), "rtsp", 4) == 0) {
            fmt = "rtsp";
        } else if (std::strncmp(m_url.c_str(), "rtmp", 4) == 0) {
            fmt = "flv";  // RTMP 协议要求 FLV
        } else {
            fmt = "mpegts";  // 本地文件用 TS, 避免 FLV hang bug
        }
        if (avformat_alloc_output_context2(&m_outFmt, nullptr, fmt, m_url.c_str()) < 0 || !m_outFmt) {
            fprintf(stderr, "[Streamer] 创建容器失败\n"); return false;
        }
        m_useTs = (std::string(m_outFmt->oformat->name) == "mpegts");
        // 2. 创建流
        m_outStream = avformat_new_stream(m_outFmt, nullptr);
        AVCodecParameters* par = m_outStream->codecpar;
        par->codec_type = AVMEDIA_TYPE_VIDEO;
        par->codec_id = AV_CODEC_ID_H264;
        par->width = m_width;
        par->height = m_height;
        par->format = AV_PIX_FMT_YUV420P;
        par->codec_tag = 0;
        m_outStream->time_base = (std::strncmp(m_url.c_str(),"rtsp",4)==0)
            ? (AVRational){1,90000} : (AVRational){1,1000};
        m_ptsStep = m_outStream->time_base.den / m_fps;
        // 3. extradata
        if (m_useTs) {
            // TS 的 extradata 用 annexb 格式: 起始码+SPS+起始码+PPS
            if (!buildExtradataAnnexb()) fprintf(stderr, "[Streamer] 警告: extradata构建失败\n");
        } else {
            // FLV/MP4 用 AVCC 格式
            if (!buildExtradata()) fprintf(stderr, "[Streamer] 警告: extradata构建失败\n");
        }
        // 4. 打开IO
        printf("[Streamer] 调用 avio_open...\n"); fflush(stdout);
        int aior = avio_open(&m_outFmt->pb, m_url.c_str(), AVIO_FLAG_WRITE);
        printf("[Streamer] avio_open ret=%d\n", aior); fflush(stdout);
        if (aior < 0) {
            fprintf(stderr, "[Streamer] avio_open失败\n"); return false;
        }
        // 5. 写头
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "flvflags", "no_duration_filesize", 0);
        printf("[Streamer] 调用 write_header...\n"); fflush(stdout);
        int ret = avformat_write_header(m_outFmt, &opts);
        av_dict_free(&opts);
        printf("[Streamer] write_header ret=%d\n", ret); fflush(stdout);
        if (ret < 0) return false;
        printf("[Streamer] 初始化完成, 开始推流\n");
        return true;
    }

    // ---- 写一个包 ----
    void writePacket(const uint8_t* data, int size, bool is_keyframe) {
        AVPacket* pkt = av_packet_alloc();
        av_new_packet(pkt, size);
        std::memcpy(pkt->data, data, size);
        pkt->pts = m_frameCount * m_ptsStep;
        pkt->dts = pkt->pts;
        pkt->duration = m_ptsStep;
        pkt->stream_index = m_outStream->index;
        pkt->pos = -1;
        if (is_keyframe) pkt->flags |= AV_PKT_FLAG_KEY;
        int ret = av_interleaved_write_frame(m_outFmt, pkt);
        if (ret < 0 && m_frameCount < 3) {
            char eb[128]; av_strerror(ret, eb, sizeof(eb));
            fprintf(stderr, "[Streamer] 推包失败: %s\n", eb);
        }
        m_frameCount++;
        av_packet_free(&pkt);
        // 限速
        if (m_frameCount == 1) m_startTime = av_gettime_relative();
        else {
            int64_t target = (int64_t)(m_frameCount*1000000.0/m_fps);
            int64_t elapsed = av_gettime_relative() - m_startTime;
            if (target > elapsed) av_usleep((unsigned)(target-elapsed));
        }
    }

    // ---- 成员 ----
    std::string m_url;
    int m_width = 0, m_height = 0, m_fps = 30;
    bool m_useTs = false;  // 是否用 mpegts(影响 extradata 和包格式)
    AVFormatContext* m_outFmt = nullptr;
    AVStream* m_outStream = nullptr;
    int64_t m_ptsStep = 0;
    int64_t m_frameCount = 0;
    int64_t m_startTime = 0;
    bool m_headerWritten = false;
    std::vector<uint8_t> m_sps;
    std::vector<uint8_t> m_pps;
};
