extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <SDL2/SDL.h>

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

// ============================================================================
// RAII deleters — FFmpeg
// ============================================================================
struct AVFormatDeleter {
    void operator()(AVFormatContext* p) const { avformat_close_input(&p); }
};
struct AVCodecDeleter {
    void operator()(AVCodecContext* p) const { avcodec_free_context(&p); }
};
struct AVFrameDeleter {
    void operator()(AVFrame* p) const { av_frame_free(&p); }
};
struct SwsDeleter {
    void operator()(SwsContext* p) const { sws_freeContext(p); }
};
struct SwrDeleter {
    void operator()(SwrContext* p) const { swr_free(&p); }
};
using AVFormatPtr = std::unique_ptr<AVFormatContext, AVFormatDeleter>;
using AVCodecPtr  = std::unique_ptr<AVCodecContext,  AVCodecDeleter>;
using AVFramePtr  = std::unique_ptr<AVFrame,         AVFrameDeleter>;
using SwsPtr      = std::unique_ptr<SwsContext,      SwsDeleter>;
using SwrPtr      = std::unique_ptr<SwrContext,      SwrDeleter>;
// Packet deleter (av_packet_free takes AVPacket**)
static auto pkt_deleter = [](AVPacket* p) { av_packet_free(&p); };
using AVPacketPtr = std::unique_ptr<AVPacket, decltype(pkt_deleter)>;
// ============================================================================
// RAII deleters — SDL
// ============================================================================
struct SDLWinDeleter { void operator()(SDL_Window* p)   const { SDL_DestroyWindow(p);   } };
struct SDLRdrDeleter { void operator()(SDL_Renderer* p)  const { SDL_DestroyRenderer(p);  } };
struct SDLTexDeleter { void operator()(SDL_Texture* p)   const { SDL_DestroyTexture(p);   } };

using SDLWinPtr   = std::unique_ptr<SDL_Window,   SDLWinDeleter>;
using SDLRdrPtr   = std::unique_ptr<SDL_Renderer,  SDLRdrDeleter>;
using SDLTexPtr   = std::unique_ptr<SDL_Texture,   SDLTexDeleter>;

//QUEUE
struct PacketQueue {
    AVPacketList *first = nullptr;
    AVPacketList *last = nullptr;
    int count = 0;
    int size = 0;

    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> abort{false};

    void put(AVPacket *packet) {
        AVPacket *copy = av_packet_clone(packet);
        if (!copy) {
            return;
        }
        auto *node = new AVPacketList{*copy,nullptr};
        av_packet_free(&copy);

        std::lock_guard<std::mutex> lock(mutex);
        if (!last) {
            first = node;
        } else {
            last->next = node;
        }
        last = node;
        count++;
        size+=node->pkt.size;
        cv.notify_one();
    }

    bool get(AVPacketPtr &pkt,bool block = true) {
        std::unique_lock<std::mutex> lock(mutex);
        while (true) {
            if (abort) {
                return false;
            }
            if (first) {
                auto *node = first;
                first = node->next;
                if (!first) {
                    last = nullptr;
                }
                count--;
                size-=node->pkt.size;

                AVPacket *packet = av_packet_alloc();
                *packet = node->pkt;
                pkt.reset(packet);
                delete node;
                return true;
            }
            if (!block) {
                return false;
            }
            cv.wait(lock);
        }
    }

    void flush() {
        std::lock_guard<std::mutex> lock(mutex);
        while (first) {
            auto *node = first;
            first = node->next;
            av_packet_unref(&node->pkt);
            delete node;
        }
        last = nullptr;
        count = 0;
        size = 0;
    }

};

struct PcmBuffer {
    static constexpr int CAPACITY = 1024*1024;
    uint8_t data[CAPACITY]{};
    int read_pos = 0;
    int write_pos = 0;
    int used = 0;

    std::mutex mutex;
    std::condition_variable cv_read;
    std::condition_variable cv_write;

    int read(uint8_t *out,int len) {
        std::lock_guard<std::mutex> lock(mutex);
        if (0 == used) {
            return 0;
        }
        int n = std::min(len,used);
        if (read_pos +n <=CAPACITY) {
            std::memcpy(out,data+read_pos,n);
        }else {
            int tail = CAPACITY - read_pos;
            std::memcpy(out,data+read_pos,tail);
            std::memcpy(out+tail,data,n-tail);
        }
        read_pos = (read_pos + n) % CAPACITY;
        used -=n;
        cv_write.notify_one();
        return n;

    }
    void write(const uint8_t *in,int len) {
        std::unique_lock<std::mutex> lock(mutex);

        while (used+len>CAPACITY) {
            cv_write.wait(lock);
            if (used+len>CAPACITY*8/10) {
                break;
            }
        }

        // 处理环形回绕
        if (write_pos + len <= CAPACITY) {
            std::memcpy(data + write_pos, in, len);
        } else {
            int tail = CAPACITY - write_pos;
            std::memcpy(data + write_pos, in, tail);
            std::memcpy(data, in + tail, len - tail);
        }
        write_pos = (write_pos + len) % CAPACITY; // 写指针回绕
        used += len;                              // 已用空间增加
        cv_read.notify_one();                     // 通知读取端：有数据了
    }
     bool empty() const {
        return used==0;
    }

};

struct AudioState {
    AVCodecContext *ctx = nullptr;
    // static auto ctxx=avcodec_alloc_context3(codec);
    PacketQueue pkt_queue;
    PcmBuffer pcm;
    std::thread decode_thread;
    bool thread_running = false;

    SwrPtr swr_ctx;
    AVSampleFormat out_fmt = AV_SAMPLE_FMT_S16;// 输出格式：16 位有符号整数
    int64_t out_channel_layout = AV_CH_LAYOUT_STEREO;// 输出声道布局：立体声
    int             out_channels  = 2;       // 输出声道数
    int             out_sample_rate = 44100; // 输出采样率
    ~AudioState() {
        stop();
    }
    void stop() {
        if (thread_running) {
            pkt_queue.abort = true;
            pkt_queue.cv.notify_all();
            decode_thread.join();
            thread_running = false;
        }
    }


};

namespace {
    void audio_decode_thread(AudioState *state) {
        AVPacketPtr pkt(nullptr,pkt_deleter);
        AVFramePtr frame(av_frame_alloc());

        while (state->pkt_queue.get(pkt)) {
            if (!pkt) {
                continue;
            }

            if (avcodec_send_packet(state->ctx,frame.get()) == 0) {
                if (!state->swr_ctx) {

                }
            }
        }

    }
}