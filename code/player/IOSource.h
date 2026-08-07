// =============================================================================
//  IOSource.h —— IO 抽象接口 (Step2 的核心: "IO 可替换"的接缝)
// =============================================================================
//
//  【为什么要有这一层?】
//  FFmpeg 的 avformat_open_input 默认吃一个 URL 字符串或文件路径,
//  但真实场景里数据可能来自:
//    - 本地文件      → 用 fopen/fread
//    - 内存 buffer   → 硬件编码器回调给的裸字节流 (你的无人机场景!)
//    - 自定义网络协议 → 自己实现的 QUIC/私有协议
//    - RTMP/RTSP     → FFmpeg 协议层 (这种情况其实不用走 IOSource, 直接传 URL)
//
//  把"字节从哪来"抽象成统一接口后, 上层的 Demuxer/Decoder/Renderer
//  完全不需要知道数据来源, 换 IO 后端 = 换一个 IOSource 子类, 一行代码的事.
//
//  【FFmpeg 怎么接住这个抽象?】
//  通过 avio_alloc_context 注册一组 C 回调函数, FFmpeg 内部读字节时
//  会回调我们提供的 read/seek, 我们在里面转发给 IOSource 的虚函数.
//  这样 FFmpeg 就从"只能读文件"变成"能读任意字节源".
//
//  【接口设计】
//  四个操作, 对应文件 IO 的原语:
//    open()  - 打开/准备数据源
//    read()  - 读字节 (核心, 必须实现)
//    seek()  - 定位 (可选, 直播流不需要)
//    close() - 关闭
//  再加两个辅助: size() 给总长度 (seek 用), isOpen() 状态查询.
//
//  【返回值约定 (和 FFmpeg AVIOCallback 对齐)】
//    read: >0 实际读取字节数;
//          AVERROR_EOF (负值) 表示已到末尾 —— ★ 必须用这个, 不能返回 0!
//          (返回 0 会让 FLV 等 demuxer 在 find_stream_info 里死循环重试)
//          其他负值 = 错误 (用 AVERROR(errno) 或 AVERROR_xxx)
//    seek: 成功返回新位置; 失败返回负数
//  这套约定是为了让 Demuxer 里能直接把返回值喂给 FFmpeg, 零转换.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

// FFmpeg 错误码宏 (read 返回负值时用)
// 注意: FFmpeg 子头文件(error.h)本身没有 extern "C" 守卫, 单独 include 时必须手动加,
//       否则 C++ 会把 AVERROR 等(它们是内联函数/宏)用到的符号做 name mangling, 链接失败.
extern "C" {
#include <libavutil/error.h>
}

class IOSource {
public:
    virtual ~IOSource() = default;

    // 打开数据源. path 对文件是路径, 对内存源可忽略. 成功返回 true.
    virtual bool open(const std::string& path) = 0;

    // 读字节到 buf, 最多 bufSize 字节.
    // 返回: >0 实际读取数; 0=EOF; <0 错误 (用 AVERROR(errno) 或 AVERROR_xxx)
    // 注意: 允许返回少于 bufSize (部分读), FFmpeg 能处理.
    virtual int read(uint8_t* buf, int bufSize) = 0;

    // 定位. offset/whence 语义同 lseek:
    //   SEEK_SET(0): 绝对位置
    //   SEEK_CUR(1): 相对当前位置
    //   SEEK_END(2): 相对末尾
    //   AVSEEK_SIZE(0x10000): 特殊值, 询问总大小 (不真的定位), 返回 size 或 <0
    // 不可定位的源 (如直播流) 返回 AVERROR(ENOSYS) 即可.
    virtual int64_t seek(int64_t offset, int whence) = 0;

    // 关闭数据源, 释放资源
    virtual void close() = 0;

    // 数据源总字节数 (未知返回 -1). 用于 FFmpeg 计算时长/支持拖动.
    virtual int64_t size() const = 0;

    // 当前是否已打开
    virtual bool isOpen() const = 0;
};
