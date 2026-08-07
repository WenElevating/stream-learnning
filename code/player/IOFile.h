// =============================================================================
//  IOFile.h —— IOSource 的"本地文件"实现
// =============================================================================
//
//  用标准 C 的 FILE* + fopen/fread/fseek/ftell 实现.
//  为什么不用 Windows API (CreateFile/ReadFile)? 因为标准 C 跨平台且足够快
//  (FILE* 自带用户态缓冲, 默认 4KB, 够用了).
//
//  这个类是 IOSource 的"参考实现", 其它后端 (内存/网络) 照着它的返回值约定写就行.
// =============================================================================
#pragma once

#include "IOSource.h"
#include <cstdio>   // FILE*, fopen...
#include <string>

// AVSEEK_SIZE 定义在 avio.h (值 0x10000, FFmpeg 用来询问总大小)
// 同样需要 extern "C" (子头文件无守卫)
extern "C" {
#include <libavformat/avio.h>
}

class IOFile : public IOSource {
public:
    IOFile() : m_fp(nullptr), m_size(-1) {}
    ~IOFile() override { close(); }

    bool open(const std::string& path) override {
        // "rb": 二进制读 (Windows 上不加 b 会有 \r\n 转换问题, 破坏视频字节!)
        m_fp = fopen(path.c_str(), "rb");
        if (!m_fp) return false;
        // 预先算好总大小, size() 和 seek(AVSEEK_SIZE) 都要用
        fseek(m_fp, 0, SEEK_END);
        m_size = ftell(m_fp);
        fseek(m_fp, 0, SEEK_SET);
        return true;
    }

    int read(uint8_t* buf, int bufSize) override {
        if (!m_fp) return AVERROR(EINVAL);
        size_t n = fread(buf, 1, (size_t)bufSize, m_fp);
        if (n == 0 && feof(m_fp)) {
            // ★ 关键: 给 FFmpeg 的 AVIO 回调, EOF 必须返回 AVERROR_EOF (FFEOF),
            //   不能返回 0. 返回 0 会让某些 demuxer (FLV 尤其敏感) 误以为
            //   "流还在继续, 只是暂时没数据", 从而在 find_stream_info 里死循环重试.
            return AVERROR_EOF;   // AVERROR_EOF == AVERROR(E'EOF') 内部常量
        }
        if (n == 0 && ferror(m_fp)) {
            clearerr(m_fp);
            return AVERROR(EIO);
        }
        return (int)n;
    }

    int64_t seek(int64_t offset, int whence) override {
        if (!m_fp) return AVERROR(EINVAL);
        // ★ AVSEEK_SIZE 是 FFmpeg 特殊值: 询问总大小, 不是真的定位
        if (whence == AVSEEK_SIZE) {
            return m_size;   // -1 表示未知, FFmpeg 会按"不可定位"处理
        }
        // 其余 whence 直接转发给 fseek (0/1/2 = SEEK_SET/CUR/END)
        if (fseek(m_fp, (long)offset, whence) != 0) {
            return AVERROR(errno);
        }
        return ftell(m_fp);
    }

    void close() override {
        if (m_fp) {
            fclose(m_fp);
            m_fp = nullptr;
        }
        m_size = -1;
    }

    int64_t size() const override { return m_size; }
    bool isOpen() const override { return m_fp != nullptr; }

private:
    FILE*   m_fp;
    int64_t m_size;
};
