// =============================================================================
//  VideoRenderer.h —— 视频渲染模块 (AVFrame → 屏幕)
// =============================================================================
//
//  【职责】
//    只负责: 把一帧 YUV 数据显示到窗口
//    绝不碰: 帧从哪解出来的 (Decoder 的事), 什么时候显示 (Step4 时钟的事)
//
//  【设计】
//    封装 SDL 的 Window/Renderer/Texture 三件套.
//    外部调用 show(frame) 即可显示一帧, 内部处理纹理更新和翻页.
//    不关心 YUV 从哪来, 也不关心播放节奏.
//
//  【为什么单独抽出来?】
//    渲染后端可替换: SDL → OpenGL → D3D → 自绘 framebuffer.
//    抽象成接口后, 换后端 = 换一个 VideoRenderer 子类, 上层不变.
//    (本 Step 只实现 SDL 版, 但结构已留出替换余地)
// =============================================================================
#pragma once

#define SDL_MAIN_HANDLED
extern "C" {
#include <SDL.h>
#include <libavutil/frame.h>   // AVFrame (子头, 需在 extern "C" 内)
}

class VideoRenderer {
public:
    VideoRenderer();
    ~VideoRenderer();

    // 初始化 SDL + 创建窗口/渲染器/纹理. width/height 是视频原始尺寸.
    bool open(int width, int height);

    // 显示一帧 YUV420P. 内部做: 更新纹理 → 清屏 → 拷贝 → 翻页.
    // 返回 false 表示收到退出事件 (窗口被关/ESC).
    bool show(const AVFrame* frame);

    // 延时 (Step1/2 用固定延时, Step4 改成按 PTS)
    void delay(int ms) { SDL_Delay(ms); }

    void close();

private:
    SDL_Window*   m_window;
    SDL_Renderer* m_renderer;
    SDL_Texture*  m_texture;
    int m_width;
    int m_height;
};
