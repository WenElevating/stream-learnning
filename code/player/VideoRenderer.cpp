// =============================================================================
//  VideoRenderer.cpp —— SDL 视频渲染实现
// =============================================================================
#include "VideoRenderer.h"
#include <iostream>

VideoRenderer::VideoRenderer()
    : m_window(nullptr), m_renderer(nullptr), m_texture(nullptr),
      m_width(0), m_height(0) {}

VideoRenderer::~VideoRenderer() { close(); }

bool VideoRenderer::open(int width, int height) {
    m_width = width;
    m_height = height;

    // [1] SDL 初始化 (只 VIDEO. Step4 加音频时改 SDL_INIT_VIDEO|AUDIO)
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "[Renderer] SDL_Init 失败: " << SDL_GetError() << "\n";
        return false;
    }

    // [2] 窗口
    m_window = SDL_CreateWindow("Step2 Player",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height, SDL_WINDOW_SHOWN);
    if (!m_window) {
        std::cerr << "[Renderer] 创建窗口失败: " << SDL_GetError() << "\n";
        return false;
    }

    // [3] 渲染器 (硬件加速 + vsync)
    m_renderer = SDL_CreateRenderer(m_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!m_renderer) {
        m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!m_renderer) {
        std::cerr << "[Renderer] 创建渲染器失败: " << SDL_GetError() << "\n";
        return false;
    }

    // [4] 纹理: IYUV == YUV420P, 零拷贝 (知识点见 Step1 笔记)
    m_texture = SDL_CreateTexture(m_renderer,
        SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!m_texture) {
        std::cerr << "[Renderer] 创建纹理失败: " << SDL_GetError() << "\n";
        return false;
    }

    return true;
}

bool VideoRenderer::show(const AVFrame* frame) {
    // [1] 上传 YUV 三平面到纹理
    //     必须用 UpdateYUVTexture (不是 UpdateTexture), 原因见 Step1 笔记
    SDL_UpdateYUVTexture(m_texture, nullptr,
        frame->data[0], frame->linesize[0],   // Y
        frame->data[1], frame->linesize[1],   // U
        frame->data[2], frame->linesize[2]);  // V

    // [2] 清屏 + 拷贝 + 翻页
    SDL_RenderClear(m_renderer);
    SDL_RenderCopy(m_renderer, m_texture, nullptr, nullptr);
    SDL_RenderPresent(m_renderer);

    // [3] 顺便处理 SDL 事件 (ESC/关窗口). 返回 false 通知主循环退出.
    //     (Step3 会把事件处理移到专门的主线程, 这里 Step2 先简化)
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return false;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) return false;
    }
    return true;
}

void VideoRenderer::close() {
    if (m_texture)  { SDL_DestroyTexture(m_texture);  m_texture = nullptr; }
    if (m_renderer) { SDL_DestroyRenderer(m_renderer); m_renderer = nullptr; }
    if (m_window)   { SDL_DestroyWindow(m_window);    m_window = nullptr; }
    SDL_Quit();
    m_width = m_height = 0;
}
