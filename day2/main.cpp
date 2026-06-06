extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <SDL2/SDL.h>
}

#include <cassert>
#include <memory>
// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55, 28, 1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

struct AVFormatContextDeleter {
    void operator()(AVFormatContext *ctx) { avformat_free_context(ctx); }
};

struct AVCodecContextDeleter {
    void operator()(AVCodecContext *ctx) { avcodec_free_context(&ctx); }
};

struct AVFrameDeleter {
    void operator()(AVFrame *ctx) { av_frame_free(&ctx); }
};

struct SwsContextDeleter {
    void operator()(SwsContext *ctx) { sws_freeContext(ctx); }
};

using AVFormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;
using AVCodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;
using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;


// SDL deleters
struct SDLWindowDeleter {
    void operator()(SDL_Window *p) const { SDL_DestroyWindow(p); }
};

struct SDLRendererDeleter {
    void operator()(SDL_Renderer *p) const { SDL_DestroyRenderer(p); }
};

struct SDLTextureDeleter {
    void operator()(SDL_Texture *p) const { SDL_DestroyTexture(p); }
};

using SDLWindowPtr = std::unique_ptr<SDL_Window, SDLWindowDeleter>;
using SDLRendererPtr = std::unique_ptr<SDL_Renderer, SDLRendererDeleter>;
using SDLTexturePtr = std::unique_ptr<SDL_Texture, SDLTextureDeleter>;


struct VideoContext {
    AVFormatContextPtr fmt_ctx;
    AVCodecContextPtr codec_ctx;
    AVFramePtr frame;
    SwsContextPtr sws_ctx;
    int video_idx;
    int width;
    int height;
};

namespace {
    int find_video_stream(AVFormatContext *pFormatCtx) {
        for (int i = 0; i < pFormatCtx->nb_streams; i++) {
            if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                return i;
            }
        }
        return -1;
    }


    AVFormatContextPtr open_file(const char *filename) {
        AVFormatContext *format_ctx = nullptr;
        if (avformat_open_input(&format_ctx, filename, nullptr, nullptr) < 0) {
            fprintf(stderr, "Cannot open file: %s\n", filename);
            return nullptr;
        }
        if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
            avformat_close_input(&format_ctx);
            fprintf(stderr, "Cannot find stream info\n");
            return nullptr;
        }
        return AVFormatContextPtr(format_ctx);
    }

    AVCodecContextPtr open_codec(AVFormatContext *fmt, int video_idx) {
        AVCodecParameters *codecpar = fmt->streams[video_idx]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
        if (!codec) {
            fprintf(stderr, "Unsupported codec\n");
            return nullptr;
        }

        AVCodecContext *ctx = avcodec_alloc_context3(codec);
        if (!ctx) {
            return nullptr;
        }

        if ((avcodec_parameters_to_context(ctx, codecpar) < 0) ||
            (avcodec_open2(ctx, codec, nullptr) < 0)) {
            avcodec_free_context(&ctx);
            fprintf(stderr, "Cannot open codec\n");
            return nullptr;
        }
        return AVCodecContextPtr(ctx);
    }

    SDLWindowPtr create_window(int w, int h) {
        SDL_Window *window = SDL_CreateWindow("FFmpeg Player",
                                              SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED, w, h, 0);
        if (!window) {
            fprintf(stderr, "SDL window failed: %s\n", SDL_GetError());
        }
        return SDLWindowPtr(window);
    }

    SDLRendererPtr create_renderer(SDL_Window *window) {
        SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
                                                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) {
            fprintf(stderr, "SDL renderer failed: %s\n", SDL_GetError());
        }
        return SDLRendererPtr(renderer);
    }

    SDLTexturePtr create_texture(SDL_Renderer *renderer, int w, int h) {
        SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                                                 SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!texture) {
            fprintf(stderr, "SDL texture failed: %s\n", SDL_GetError());
        }
        return SDLTexturePtr(texture);
    }

    void render_frame(SDL_Renderer *renderer, SDL_Texture *texture) {
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    }

    bool poll_events() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT ||
                (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) {
                return false;
            }
        }
        return true;
    }

    std::unique_ptr<VideoContext> build_context(const char *filename) {
        auto fmt_ctx = open_file(filename);
        if (!fmt_ctx) {
            return nullptr;
        }

        av_dump_format(fmt_ctx.get(), 0, filename, 0);

        int video_idx = find_video_stream(fmt_ctx.get());
        if (video_idx < 0) {
            return nullptr;
        }

        auto codec_ctx = open_codec(fmt_ctx.get(), video_idx);
        if (!codec_ctx) {
            return nullptr;
        }

        auto frame = AVFramePtr(av_frame_alloc());
        if (!frame) {
            fprintf(stderr, "Cannot allocate frame\n");
            return nullptr;
        }

        int w = codec_ctx->width;
        int h = codec_ctx->height;

        SwsContext *sws_ctx = sws_getContext(
            w, h, codec_ctx->pix_fmt,
            w, h, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx) {
            fprintf(stderr, "Cannot create sws context\n");
            return nullptr;
        }

        auto ctx = std::make_unique<VideoContext>();
        ctx->fmt_ctx = std::move(fmt_ctx);
        ctx->codec_ctx = std::move(codec_ctx);
        ctx->frame = std::move(frame);
        ctx->sws_ctx = SwsContextPtr(sws_ctx);
        ctx->video_idx = video_idx;
        ctx->width = w;
        ctx->height = h;
        return ctx;
    }

    int play(VideoContext &ctx) {
        auto window = create_window(ctx.width, ctx.height);
        auto renderer = create_renderer(window.get());
        auto texture = create_texture(renderer.get(), ctx.width, ctx.height);
        if (!texture || !renderer || !window) {
            return 1;
        }

        AVPacket *packet = av_packet_alloc();
        std::unique_ptr<AVPacket, void(*)(AVPacket *)> pkt(packet,
                                                           [](AVPacket *p) { av_packet_free(&p); });
        uint32_t last_pts = 0;
        AVRational tb = ctx.fmt_ctx->streams[ctx.video_idx]->time_base;

        while (av_read_frame(ctx.fmt_ctx.get(), pkt.get()) >= 0) {
            if (pkt->stream_index != ctx.video_idx) {
                av_packet_unref(pkt.get());
                continue;
            }
            if (avcodec_send_packet(ctx.codec_ctx.get(), pkt.get()) < 0) {
                fprintf(stderr, "Error sending packet\n");
                av_packet_unref(pkt.get());
                continue;
            }
            av_packet_unref(pkt.get());//unref为了及时释放packet的数据，防止内存泄漏

            while (avcodec_receive_frame(ctx.codec_ctx.get(), ctx.frame.get()) == 0) {
                // convert decoder output -> YUV420P directly on texture memory

                uint8_t *dst_data[4];
                void *pixels = nullptr;
                int pitch = 0;
                SDL_LockTexture(texture.get(), nullptr, &pixels, &pitch);

                // uint8_t* dst_data[4];
                dst_data[0] = static_cast<uint8_t *>(pixels);
                dst_data[1] = dst_data[0] + pitch * ctx.height;
                dst_data[2] = dst_data[1] + (pitch / 2) * (ctx.height / 2);
                dst_data[3] = nullptr;

                int dst_linesize[4];
                dst_linesize[0] = pitch;
                dst_linesize[1] = pitch / 2;
                dst_linesize[2] = pitch / 2;
                dst_linesize[3] = 0;

                sws_scale(ctx.sws_ctx.get(),
                          ctx.frame->data, ctx.frame->linesize, 0, ctx.height,
                          dst_data, dst_linesize);
                SDL_UnlockTexture(texture.get());

                render_frame(renderer.get(), texture.get());
                // frame-rate pacing via PTS
                uint32_t frame_time_ms = (uint32_t) (ctx.frame->pts -last_pts) * 1000 * tb.num / tb.den;
                last_pts = (uint32_t) ctx.frame->pts;
                if (frame_time_ms > 0 && frame_time_ms < 5000) {
                    SDL_Delay(frame_time_ms);
                }
                if (!poll_events()) {
                    return 0;
                }
            }
        }

        return 0;
    }
};


int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <video_file>\n", argv[0]);
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        return 1;
    }

    auto vc = build_context(argv[1]);
    if (!vc) {
        return 1;
    }
    return play(*vc);
}
