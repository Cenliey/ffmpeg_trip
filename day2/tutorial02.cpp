// tutorial02.cpp — A modern C++ FFmpeg video player using SDL2
//
// Based on the classic FFmpeg tutorial by Martin Bohme.
// Modernized: RAII, SDL2, FFmpeg 4+ decode API, unique_ptr.
//
// Run:
//   cmake --build build
//   ./tutorial02 myvideofile.mp4

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <SDL2/SDL.h>
#include <cstdio>
#include <memory>

// ============================================================================
// RAII helpers
// ============================================================================

struct AVFormatContextDeleter {
    void operator()(AVFormatContext* p) const { avformat_close_input(&p); }
};
struct AVCodecContextDeleter {
    void operator()(AVCodecContext* p) const { avcodec_free_context(&p); }
};
struct AVFrameDeleter {
    void operator()(AVFrame* p) const { av_frame_free(&p); }
};
struct SwsContextDeleter {
    void operator()(SwsContext* p) const { sws_freeContext(p); }
};

using AVFormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;
using AVCodecContextPtr  = std::unique_ptr<AVCodecContext,  AVCodecContextDeleter>;
using AVFramePtr         = std::unique_ptr<AVFrame,         AVFrameDeleter>;
using SwsContextPtr      = std::unique_ptr<SwsContext,      SwsContextDeleter>;

// SDL deleters
struct SDLWindowDeleter {
    void operator()(SDL_Window* p) const { SDL_DestroyWindow(p); }
};
struct SDLRendererDeleter {
    void operator()(SDL_Renderer* p) const { SDL_DestroyRenderer(p); }
};
struct SDLTextureDeleter {
    void operator()(SDL_Texture* p) const { SDL_DestroyTexture(p); }
};

using SDLWindowPtr   = std::unique_ptr<SDL_Window,   SDLWindowDeleter>;
using SDLRendererPtr = std::unique_ptr<SDL_Renderer,  SDLRendererDeleter>;
using SDLTexturePtr  = std::unique_ptr<SDL_Texture,   SDLTextureDeleter>;

// ============================================================================
// Video context — bundles everything needed for decode + render
// ============================================================================
struct VideoContext {
    AVFormatContextPtr fmt_ctx;
    AVCodecContextPtr  codec_ctx;
    AVFramePtr         frame;
    SwsContextPtr      sws_ctx;
    int                video_idx;
    int                width;
    int                height;
};

// ============================================================================
// Step 1: open file, find video stream
// ============================================================================

static int find_video_stream(AVFormatContext* fmt) {
    for (int i = 0; i < (int)fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            return i;
    }
    fprintf(stderr, "No video stream found\n");
    return -1;
}

static AVFormatContextPtr open_file(const char* filename) {
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, filename, nullptr, nullptr) < 0) {
        fprintf(stderr, "Cannot open file: %s\n", filename);
        return nullptr;
    }
    if (avformat_find_stream_info(raw, nullptr) < 0) {
        avformat_close_input(&raw);
        fprintf(stderr, "Cannot find stream info\n");
        return nullptr;
    }
    return AVFormatContextPtr(raw);
}

// ============================================================================
// Step 2: open codec
// ============================================================================

static AVCodecContextPtr open_codec(AVFormatContext* fmt, int video_idx) {
    AVCodecParameters* codecpar = fmt->streams[video_idx]->codecpar;

    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "Unsupported codec\n");
        return nullptr;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) return nullptr;

    if (avcodec_parameters_to_context(ctx, codecpar) < 0 ||
        avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        fprintf(stderr, "Cannot open codec\n");
        return nullptr;
    }
    return AVCodecContextPtr(ctx);
}

// ============================================================================
// Step 3: init SDL2 — window, renderer, texture
// ============================================================================

static SDLWindowPtr create_window(int w, int h) {
    SDL_Window* win = SDL_CreateWindow("FFmpeg Player",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       w, h, 0);
    if (!win) fprintf(stderr, "SDL window failed: %s\n", SDL_GetError());
    return SDLWindowPtr(win);
}

static SDLRendererPtr create_renderer(SDL_Window* win) {
    SDL_Renderer* rdr = SDL_CreateRenderer(win, -1,
                                           SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!rdr) fprintf(stderr, "SDL renderer failed: %s\n", SDL_GetError());
    return SDLRendererPtr(rdr);
}

static SDLTexturePtr create_texture(SDL_Renderer* rdr, int w, int h) {
    SDL_Texture* tex = SDL_CreateTexture(rdr, SDL_PIXELFORMAT_IYUV,
                                         SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!tex) fprintf(stderr, "SDL texture failed: %s\n", SDL_GetError());
    return SDLTexturePtr(tex);
}

// ============================================================================
// Step 4: decode + render loop
// ============================================================================

static void render_frame(SDL_Renderer* rdr, SDL_Texture* tex) {
    SDL_RenderClear(rdr);
    SDL_RenderCopy(rdr, tex, nullptr, nullptr);
    SDL_RenderPresent(rdr);
}

static bool poll_events() {
    SDL_Event evt;
    while (SDL_PollEvent(&evt)) {
        if (evt.type == SDL_QUIT ||
            (evt.type == SDL_KEYDOWN && evt.key.keysym.sym == SDLK_ESCAPE))
            return false;
    }
    return true;
}

// ============================================================================
// Build VideoContext from filename
// ============================================================================

static std::unique_ptr<VideoContext> build_context(const char* filename) {
    auto fmt_ctx = open_file(filename);
    if (!fmt_ctx) return nullptr;

    av_dump_format(fmt_ctx.get(), 0, filename, 0);

    int video_idx = find_video_stream(fmt_ctx.get());
    if (video_idx < 0) return nullptr;

    auto codec_ctx = open_codec(fmt_ctx.get(), video_idx);
    if (!codec_ctx) return nullptr;

    auto frame = AVFramePtr(av_frame_alloc());
    if (!frame) {
        fprintf(stderr, "Cannot allocate frame\n");
        return nullptr;
    }

    int w = codec_ctx->width;
    int h = codec_ctx->height;

    SwsContext* raw_sws = sws_getContext(
        w, h, codec_ctx->pix_fmt,
        w, h, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!raw_sws) {
        fprintf(stderr, "Cannot create sws context\n");
        return nullptr;
    }

    auto ctx = std::make_unique<VideoContext>();
    ctx->fmt_ctx   = std::move(fmt_ctx);
    ctx->codec_ctx = std::move(codec_ctx);
    ctx->frame     = std::move(frame);
    ctx->sws_ctx   = SwsContextPtr(raw_sws);
    ctx->video_idx = video_idx;
    ctx->width     = w;
    ctx->height    = h;
    return ctx;
}

// ============================================================================
// Main
// ============================================================================

static int play(VideoContext& vc) {
    SDLWindowPtr   window   = create_window(vc.width, vc.height);
    SDLRendererPtr renderer = create_renderer(window.get());
    SDLTexturePtr  texture  = create_texture(renderer.get(), vc.width, vc.height);
    if (!window || !renderer || !texture) return 1;

    AVPacket* raw_pkt = av_packet_alloc();
    std::unique_ptr<AVPacket, void(*)(AVPacket*)> pkt(raw_pkt,
        [](AVPacket* p) { av_packet_free(&p); });

    Uint32 last_pts = 0;
    AVRational tb = vc.fmt_ctx->streams[vc.video_idx]->time_base;

    while (av_read_frame(vc.fmt_ctx.get(), pkt.get()) >= 0) {
        if (pkt->stream_index != vc.video_idx) {
            av_packet_unref(pkt.get());
            continue;
        }

        if (avcodec_send_packet(vc.codec_ctx.get(), pkt.get()) < 0) {
            fprintf(stderr, "Error sending packet\n");
            av_packet_unref(pkt.get());
            continue;
        }
        av_packet_unref(pkt.get());

        while (avcodec_receive_frame(vc.codec_ctx.get(), vc.frame.get()) == 0) {
            // convert decoder output -> YUV420P directly on texture memory
            void* pixels = nullptr;
            int pitch = 0;
            SDL_LockTexture(texture.get(), nullptr, &pixels, &pitch);

            uint8_t* dst_data[4];
            dst_data[0] = static_cast<uint8_t*>(pixels);
            dst_data[1] = dst_data[0] + pitch * vc.height;
            dst_data[2] = dst_data[1] + (pitch / 2) * (vc.height / 2);
            dst_data[3] = nullptr;

            int dst_linesize[4];
            dst_linesize[0] = pitch;
            dst_linesize[1] = pitch / 2;
            dst_linesize[2] = pitch / 2;
            dst_linesize[3] = 0;

            sws_scale(vc.sws_ctx.get(),
                      vc.frame->data, vc.frame->linesize, 0, vc.height,
                      dst_data, dst_linesize);
            SDL_UnlockTexture(texture.get());

            render_frame(renderer.get(), texture.get());

            // frame-rate pacing via PTS
            Uint32 frame_time_ms = (Uint32)(vc.frame->pts - last_pts) *
                                   1000 * tb.num / tb.den;
            last_pts = (Uint32)vc.frame->pts;
            if (frame_time_ms > 0 && frame_time_ms < 5000)
                SDL_Delay(frame_time_ms);

            if (!poll_events()) return 0;
        }
    }
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <video_file>\n", argv[0]);
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        return 1;
    }

    auto vc = build_context(argv[1]);
    if (!vc) return 1;

    return play(*vc);
}
