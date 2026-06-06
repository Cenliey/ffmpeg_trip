// tutorial03.cpp — FFmpeg + SDL2 带音频的视频播放器（未做音画同步）
//
// 基于 tutorial03.c（Martin Bohme 的经典 FFmpeg 教程）。
// 现代化改造：C++17、RAII、SDL2、FFmpeg 4+ 解码 API、std::thread、
// std::mutex / std::condition_variable。
//
// 构建：
//   mkdir build && cd build && cmake .. && cmake --build .
//   ./ffmpeg_trip_03 <视频文件>

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
// RAII 资源释放器 — FFmpeg 系列
//
// 每个 Deleter 对应一个 FFmpeg 释放函数，用于 unique_ptr 的自定义删除器。
// 当 unique_ptr 离开作用域或 reset 时，自动调用对应的 free/close 函数，
// 无需手动管理资源，避免内存泄漏。
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

// AVPacket 的释放器：av_packet_free 接受 AVPacket**，需要 lambda 包装
static auto pkt_deleter = [](AVPacket* p) { av_packet_free(&p); };
using AVPacketPtr = std::unique_ptr<AVPacket, decltype(pkt_deleter)>;

// ============================================================================
// RAII 资源释放器 — SDL 系列
// ============================================================================

struct SDLWinDeleter { void operator()(SDL_Window* p)   const { SDL_DestroyWindow(p);   } };
struct SDLRdrDeleter { void operator()(SDL_Renderer* p)  const { SDL_DestroyRenderer(p);  } };
struct SDLTexDeleter { void operator()(SDL_Texture* p)   const { SDL_DestroyTexture(p);   } };

using SDLWinPtr   = std::unique_ptr<SDL_Window,   SDLWinDeleter>;
using SDLRdrPtr   = std::unique_ptr<SDL_Renderer,  SDLRdrDeleter>;
using SDLTexPtr   = std::unique_ptr<SDL_Texture,   SDLTexDeleter>;

// ============================================================================
// 线程安全的包队列（生产者-消费者模式）
//
// 核心作用：在"解复用线程"和"解码线程"之间传递 AVPacket
//
//                    ┌─────────────────────────────────────┐
//                    │           PacketQueue               │
//                    │                                     │
//  主线程 ──put──▶   │  first → [pkt1]→[pkt2]→[pkt3]       │  ◀──get── 解码线程
//  av_read_frame     │  last  ─────────────────────┘       │   avcodec_send_packet
//  (生产者)          │  count=3, size=xxx bytes            │   (消费者)
//                    │                                     │
//                    │  mtx + cv 保证线程安全                │
//                    │  abort 控制退出                      │
//                    └─────────────────────────────────────┘
//
// 为什么需要队列？
//   1. av_read_frame() 读取速度 >> 解码速度，需要缓冲
//   2. 解码线程需要稳定输入，不能因为网络/磁盘慢而饿死
//   3. seek（快进/快退）时可以通过 flush() 清空过期数据
//
// 为什么用单向链表不用 std::queue？
//   FFmpeg 提供的 AVPacketList 就是单向链表节点，直接复用，
//   避免额外拷贝和适配器。
// ============================================================================

struct PacketQueue {
    // ---- 链表结构 ----
    AVPacketList* first = nullptr;   // 链表头 —— 下一个 get() 取出的包
    AVPacketList* last  = nullptr;   // 链表尾 —— 下一个 put() 插入的位置
    int count = 0;                   // 当前队列中包的数量
    int size  = 0;                   // 当前队列中所有包的总字节数

    // ---- 同步原语 ----
    std::mutex              mtx;     // 保护 first/last/count/size 的互斥锁
    std::condition_variable cv;      // 条件变量：put() 入队后唤醒等待 get() 的消费者
    std::atomic<bool>       abort{false}; // 原子标志，让阻塞的 get() 安全退出

    // ==========================================================================
    // put() — 入队（由主线程调用，生产者）
    //
    // 时序：
    //   主线程 av_read_frame() → 得到 packet
    //     └─ put(&packet)
    //          ├─ clone packet（深拷贝，因为下次 av_read_frame 会覆盖同一块内存）
    //          ├─ lock mutex
    //          ├─ 挂到链表尾部
    //          ├─ notify_one ← 如果解码线程正在 wait()，此刻被唤醒
    //          └─ unlock mutex
    // ==========================================================================
    void put(AVPacket* pkt) {
        // 深拷贝 packet：av_packet_clone = av_packet_alloc + av_packet_ref
        //   - 分配一个新的 AVPacket 结构体
        //   - 把 pkt 的 data/buffer 的引用计数 +1（零拷贝数据共享）
        //   - 如果克隆失败（OOM），直接返回
        AVPacket* copy = av_packet_clone(pkt);
        if (!copy) return;

        // 创建链表节点，把克隆的 packet 搬进节点
        auto* node = new AVPacketList{ *copy, nullptr };
        av_packet_free(&copy);  // copy 指针本身已复制到 node 中，释放局部变量

        // 加锁，挂到链表尾部（线程安全区域开始）
        std::lock_guard<std::mutex> lock(mtx);
        if (!last) first = node;           // 空队列：头尾都指向新节点
        else       last->next = node;      // 非空队列：旧尾节点的 next 指向新节点
        last = node;                        // 尾指针移到新节点
        count++;                            // 包计数 +1
        size += node->pkt.size;             // 累计字节数

        // 唤醒一个正在 wait() 的消费者线程
        //   如果此时没有线程在等待，notify_one() 是无害的（通知会丢失，但
        //   下次 get() 进来时 first 非空，直接返回，不需要 wait）
        cv.notify_one();
        // lock 在作用域结束时自动释放（RAII）
    }

    // ==========================================================================
    // get() — 出队（由解码线程调用，消费者）
    //
    // 参数：
    //   pkt   — 输出参数，取出的包通过 unique_ptr 返回给调用者
    //   block — true: 队列为空时阻塞等待；false: 立即返回 false
    //
    // 返回值：
    //   true  — 成功取出一个包（pkt 有效）
    //   false — abort=true 或 非阻塞模式下队列为空
    //
    // 关键：cv.wait(lock) 做了什么？
    //   1. 释放 mtx（让 put() 能进来入队）
    //   2. 线程进入睡眠（不消耗 CPU）
    //   3. 等 put() 调用 cv.notify_one() 时唤醒
    //   4. 唤醒后自动重新获得 mtx（保证后续操作线程安全）
    //   5. 回到 while 开头，再次检查 first 是否为空（防止虚假唤醒）
    // ==========================================================================
    bool get(AVPacketPtr& pkt, bool block = true) {
        std::unique_lock<std::mutex> lock(mtx);  // unique_lock 支持 wait 期间临时释放锁
        while (true) {
            // 情况 1：收到停止信号，不再等待，立刻退出
            if (abort) return false;

            // 情况 2：队列非空，取出头部的包
            if (first) {
                auto* node = first;              // 记住当前头节点
                first = node->next;              // 头指针后移一位
                if (!first) last = nullptr;      // 如果取完了（队列变空），尾指针归零
                count--;                         // 包计数 -1
                size -= node->pkt.size;          // 总字节数减少

                // 把节点中的 packet 转移给调用者
                //   AVPacket 是浅拷贝安全的 —— data 的引用计数已经由 clone 管理
                AVPacket* raw = av_packet_alloc();
                *raw = node->pkt;                // 逐成员拷贝（浅拷贝，data 指针共享）
                pkt.reset(raw);                  // 交给 unique_ptr 管理生命周期
                delete node;                     // 链表节点不再需要，释放
                return true;                     // 成功返回

            // 情况 3：队列为空
            } else if (!block) {
                return false;                    // 非阻塞模式：没货就直接返回
            } else {
                cv.wait(lock);                   // 阻塞等待，直到 put() 通知"有货了"
                // 唤醒后自动回到 while 开头重新检查
            }
        }
    }

    // ==========================================================================
    // flush() — 清空队列（seek 快进/快退时调用）
    //
    // 作用：丢弃队列中所有积压的包，因为 seek 后这些包的时间戳已经过期
    //       不清空的话，解码器会先处理旧数据，导致画面/声音跳跃
    // ==========================================================================
    void flush() {
        std::lock_guard<std::mutex> lock(mtx);
        while (first) {
            auto* node = first;
            first = node->next;
            av_packet_unref(&node->pkt);   // 释放 packet 内部数据（引用计数 -1）
            delete node;                    // 删除链表节点
        }
        last = nullptr;                     // 收尾归零
        count = 0;
        size = 0;
    }
};

// ============================================================================
// PCM 样本缓冲区（解码后的音频数据，供 SDL 音频回调消费）
//
// 结构：环形缓冲区（ring buffer），固定 1 MB 容量
//
//   ┌───────────────────────────────────────────┐
//   │  data[0] ... data[CAPACITY-1]             │
//   │         ↑read          ↑write             │
//   │         └── used 字节 ──┘                 │
//   └───────────────────────────────────────────┘
//
// 写入端：音频解码线程，把 resample 后的 PCM 数据写入
// 读取端：SDL 音频回调线程，从缓冲区读取数据送到声卡
// 双条件变量：cv_read（有数据可读时通知）、cv_write（有空间可写时通知）
// ============================================================================

struct PcmBuffer {
    static constexpr int CAPACITY = 1024 * 1024; // 环形缓冲区容量 1 MB
    uint8_t data[CAPACITY]{};
    int read_pos  = 0;   // 读指针：下一次 read() 的起始位置
    int write_pos = 0;   // 写指针：下一次 write() 的起始位置
    int used      = 0;   // 当前已使用的字节数

    std::mutex              mtx;
    std::condition_variable cv_read;   // 有数据到达时通知 SDL 回调线程
    std::condition_variable cv_write;  // 有空间释放时通知解码线程

    // ==========================================================================
    // read() — 读取最多 len 字节到 out（SDL 音频回调调用）
    //
    // 返回值：实际读取的字节数。无数据时返回 0（非阻塞）。
    //
    // 环形缓冲区核心逻辑：
    //   如果读指针 + 长度不超过 CAPACITY，直接线性拷贝
    //   否则需要分两段：从读指针到缓冲区末尾 + 从缓冲区开头到剩余数据
    // ==========================================================================
    int read(uint8_t* out, int len) {
        std::lock_guard<std::mutex> lock(mtx);
        if (used == 0) return 0;           // 没有数据可读，直接返回
        int n = std::min(len, used);       // 最多读取 used 字节

        // 处理环形缓冲区的回绕（wrap-around）
        if (read_pos + n <= CAPACITY) {
            // 未跨越边界：一次线性拷贝
            std::memcpy(out, data + read_pos, n);
        } else {
            // 跨越边界：分两段拷贝
            int tail = CAPACITY - read_pos;
            std::memcpy(out, data + read_pos, tail);
            std::memcpy(out + tail, data, n - tail);
        }
        read_pos = (read_pos + n) % CAPACITY; // 读指针回绕
        used -= n;                            // 已用空间减少
        cv_write.notify_one();                // 通知写入端：有空间了
        return n;
    }

    // ==========================================================================
    // write() — 写入 len 字节（音频解码线程调用）
    //
    // 如果缓冲区满了，会阻塞等待 SDL 回调线程读取数据释放空间。
    // 同样需要处理环形回绕。
    // ==========================================================================
    void write(const uint8_t* in, int len) {
        std::unique_lock<std::mutex> lock(mtx);
        // 缓冲区满了就等待 SDL 回调读取数据
        while (used + len > CAPACITY) {
            cv_write.wait(lock);
            // 等待后仍然太满，则强制写入（丢弃旧数据的风险）
            if (used + len > CAPACITY * 8 / 10) {
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

    bool empty() const { return used == 0; }
};

// ============================================================================
// 音频状态 — 封装音频相关的所有资源
// ============================================================================

struct AudioState {
    AVCodecCtx*     codec_ctx   = nullptr;   // 非拥有指针（生命周期由 VideoContext 管理）
    PacketQueue     pkt_queue;               // 音频包队列（主线程写入，解码线程读取）
    PcmBuffer       pcm_buf;                 // PCM 样本缓冲区（解码线程写入，SDL 回调读取）
    std::thread     decode_thread;           // 音频解码线程
    bool            thread_running = false;  // 线程是否正在运行

    // 重采样器状态
    SwrPtr          swr_ctx;                 // libswresample 上下文
    AVSampleFormat  out_fmt     = AV_SAMPLE_FMT_S16;  // 输出格式：16 位有符号整数
    int64_t         out_ch_layout = AV_CH_LAYOUT_STEREO; // 输出声道布局：立体声
    int             out_channels  = 2;       // 输出声道数
    int             out_sample_rate = 44100; // 输出采样率

    ~AudioState() { stop(); }

    // 停止解码线程并等待其退出
    void stop() {
        if (thread_running) {
            pkt_queue.abort = true;         // 设置退出标志
            pkt_queue.cv.notify_all();      // 唤醒所有等待的线程
            decode_thread.join();           // 等待线程结束
            thread_running = false;
        }
    }
};

// ============================================================================
// 音频解码线程 — 从队列取包 → 解码 → 重采样 → 写入 PCM 缓冲区
//
// 线程模型：
//   PacketQueue.get() ← 主线程放入的音频包
//     → avcodec_send/receive → 解码后的原始音频帧
//       → swr_convert → 重采样为 SDL 要求的格式（S16, 立体声, 44100Hz）
//         → PcmBuffer.write() → 供 SDL 音频回调消费
// ============================================================================

static void audio_decode_thread(AudioState* as) {
    AVPacketPtr pkt(nullptr, pkt_deleter);
    AVFramePtr frame(av_frame_alloc());

    // 持续从队列中获取音频包，直到 abort 被设为 true
    while (as->pkt_queue.get(pkt)) {
        if (!pkt) continue;

        // 发送音频包到解码器
        if (avcodec_send_packet(as->codec_ctx, pkt.get()) < 0) {
            continue;
        }

        // 接收解码后的音频帧
        while (avcodec_receive_frame(as->codec_ctx, frame.get()) == 0) {
            // ---- 第一次解码时，初始化重采样器 ----
            if (!as->swr_ctx) {
                SwrContext* swr = swr_alloc();
                // 设置输入参数（从音频文件中读取的原始格式）
                av_opt_set_channel_layout(swr, "in_channel_layout",
                                          as->codec_ctx->channel_layout, 0);
                av_opt_set_int(swr, "in_sample_rate",  as->codec_ctx->sample_rate,  0);
                av_opt_set_sample_fmt(swr, "in_sample_fmt",  as->codec_ctx->sample_fmt,  0);
                // 设置输出参数（SDL 要求的格式：立体声、S16、44100Hz）
                av_opt_set_channel_layout(swr, "out_channel_layout",
                                          as->out_ch_layout, 0);
                av_opt_set_int(swr, "out_sample_rate", as->out_sample_rate,          0);
                av_opt_set_sample_fmt(swr, "out_sample_fmt", as->out_fmt,               0);
                swr_init(swr);
                as->swr_ctx.reset(swr);
            }

            // 计算需要的输出样本数（考虑重采样延迟）
            int out_count = av_rescale_rnd(swr_get_delay(as->swr_ctx.get(),
                                         as->codec_ctx->sample_rate) +
                                         frame->nb_samples,
                                         as->out_sample_rate,
                                         as->codec_ctx->sample_rate, AV_ROUND_UP);
            // 分配输出缓冲区
            uint8_t* out_buf = nullptr;
            int out_bytes = av_samples_alloc(&out_buf, nullptr,
                                             as->out_channels, out_count,
                                             as->out_fmt, 1);

            // 执行重采样转换
            int converted = swr_convert(as->swr_ctx.get(), &out_buf, out_count,
                                        (const uint8_t**)frame->extended_data,
                                        frame->nb_samples);
            int out_size = av_samples_get_buffer_size(nullptr, as->out_channels,
                                                      converted, as->out_fmt, 1);
            // 将重采样后的 PCM 数据写入环形缓冲区
            as->pcm_buf.write(out_buf, out_size);
            av_freep(&out_buf);
        }
    }
}

// ============================================================================
// SDL 音频回调 — 从 PCM 缓冲区读取数据，填充到 SDL 的输出流中
//
// 此函数由 SDL 内部线程调用，不在你的控制之下。
// 要求：必须快速返回，不能阻塞。如果缓冲区没数据，就填充静音。
// ============================================================================

static void sdl_audio_callback(void* userdata, Uint8* stream, int len) {
    auto* as = static_cast<AudioState*>(userdata);
    int got = as->pcm_buf.read(stream, len);
    if (got < len) {
        // 数据不够（缓冲区欠载），剩余部分填充静音（全零 = 无声）
        std::memset(stream + got, 0, len - got);
    }
}

// ============================================================================
// 视频上下文 — 封装视频解码 + 音频相关的全部资源
// ============================================================================

struct VideoContext {
    AVFormatPtr   fmt_ctx;       // 格式上下文（管理整个媒体文件）
    AVCodecPtr    codec_ctx;     // 视频解码器上下文
    AVFramePtr    frame;         // 解码后的视频帧
    SwsPtr        sws_ctx;       // 像素格式转换上下文（任意格式 → YUV420P）
    int           video_idx = -1; // 视频流在文件中的索引
    int           audio_idx = -1; // 音频流在文件中的索引
    int           width  = 0;
    int           height = 0;
    AudioState    audio;         // 音频子系统
};

// ============================================================================
// open_file() — 打开媒体文件，获取格式上下文和流信息
// ============================================================================

static AVFormatPtr open_file(const char* path) {
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, path, nullptr, nullptr) < 0) {
        fprintf(stderr, "无法打开文件: %s\n", path);
        return nullptr;
    }
    if (avformat_find_stream_info(raw, nullptr) < 0) {
        avformat_close_input(&raw);
        fprintf(stderr, "无法获取流信息\n");
        return nullptr;
    }
    return AVFormatPtr(raw);
}

// ============================================================================
// open_codec() — 为指定流打开解码器
//
// 步骤：
//   1. 从流的 codecpar 获取编解码器 ID
//   2. 查找对应的解码器
//   3. 分配解码器上下文
//   4. 将流的参数复制到解码器上下文
//   5. 打开解码器
// ============================================================================

static AVCodecPtr open_codec(AVFormatContext* fmt, int stream_idx) {
    AVCodecParameters* par = fmt->streams[stream_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) { fprintf(stderr, "不支持的编解码器\n"); return nullptr; }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) return nullptr;

    if (avcodec_parameters_to_context(ctx, par) < 0 ||
        avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return AVCodecPtr(ctx);
}

// ============================================================================
// build_context() — 从文件路径构建完整的 VideoContext
//
// 完整的初始化流程：
//   1. 打开文件 → 2. 查找视频/音频流 → 3. 打开解码器
//   4. 创建像素转换上下文 → 5. 初始化 SDL 音频
//   6. 启动音频解码线程 → 7. 开始音频播放
// ============================================================================

static std::unique_ptr<VideoContext> build_context(const char* path) {
    // 打开文件，获取格式上下文
    auto fmt = open_file(path);
    if (!fmt) return nullptr;

    // 打印文件信息到 stderr（调试用）
    av_dump_format(fmt.get(), 0, path, 0);

    // 遍历所有流，找到第一个视频流和第一个音频流
    int video_idx = -1, audio_idx = -1;
    for (int i = 0; i < (int)fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_idx < 0)
            video_idx = i;
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_idx < 0)
            audio_idx = i;
    }
    if (video_idx < 0) { fprintf(stderr, "未找到视频流\n"); return nullptr; }
    if (audio_idx < 0) { fprintf(stderr, "未找到音频流\n"); return nullptr; }

    // 打开视频和音频解码器
    auto vcodec = open_codec(fmt.get(), video_idx);
    auto acodec = open_codec(fmt.get(), audio_idx);
    if (!vcodec || !acodec) return nullptr;

    // 分配视频帧
    auto frame = AVFramePtr(av_frame_alloc());
    if (!frame) return nullptr;

    int w = vcodec->width, h = vcodec->height;

    // 创建像素格式转换上下文：任意输入格式 → YUV420P（SDL 渲染所需格式）
    SwsContext* sws = sws_getContext(
        w, h, vcodec->pix_fmt,
        w, h, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) return nullptr;

    // 组装 VideoContext
    auto vc = std::make_unique<VideoContext>();
    vc->fmt_ctx   = std::move(fmt);
    vc->codec_ctx = std::move(vcodec);
    vc->frame     = std::move(frame);
    vc->sws_ctx.reset(sws);
    vc->video_idx = video_idx;
    vc->audio_idx = audio_idx;
    vc->width     = w;
    vc->height    = h;

    // 设置音频子系统：传递非拥有引用
    vc->audio.codec_ctx = acodec.get();

    // 打开 SDL 音频设备
    SDL_AudioSpec wanted{}, spec{};
    wanted.freq     = 44100;         // 期望采样率
    wanted.format   = AUDIO_S16SYS;  // 期望格式：16 位有符号整数，系统字节序
    wanted.channels = 2;             // 期望声道数：立体声
    wanted.samples  = 1024;          // 期望每次回调的样本数
    wanted.callback = sdl_audio_callback;
    wanted.userdata = &vc->audio;

    if (SDL_OpenAudio(&wanted, &spec) < 0) {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        return nullptr;
    }
    // 使用 SDL 实际返回的参数
    vc->audio.out_sample_rate = spec.freq;
    vc->audio.out_channels    = spec.channels;

    // 启动音频解码线程
    vc->audio.decode_thread = std::thread(audio_decode_thread, &vc->audio);
    vc->audio.thread_running = true;

    // 开始音频播放（SDL_PauseAudio(0) = 取消暂停 = 开始播放）
    SDL_PauseAudio(0);

    return vc;
}

// ============================================================================
// SDL 辅助函数
// ============================================================================

// 创建窗口
static SDLWinPtr create_window(int w, int h) {
    SDL_Window* p = SDL_CreateWindow("FFmpeg 播放器（视频+音频）",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, 0);
    return SDLWinPtr(p);
}

// 创建渲染器（启用硬件加速和垂直同步）
static SDLRdrPtr create_renderer(SDL_Window* w) {
    SDL_Renderer* p = SDL_CreateRenderer(w, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    return SDLRdrPtr(p);
}

// 创建 YUV 纹理（SDL_PIXELFORMAT_IYUV = IYUV420P）
static SDLTexPtr create_texture(SDL_Renderer* r, int w, int h) {
    SDL_Texture* p = SDL_CreateTexture(r, SDL_PIXELFORMAT_IYUV,
        SDL_TEXTUREACCESS_STREAMING, w, h);
    return SDLTexPtr(p);
}

// ============================================================================
// play() — 主播放循环
//
// 整体流程：
//   av_read_frame() 读取包
//     ├─ 音频包 → 放入 audio.pkt_queue（音频解码线程会取走）
//     └─ 视频包 → 解码 → sws_scale 转换 → 渲染到纹理 → 显示
//
// 每一帧视频渲染后根据 PTS 计算延迟时间，控制播放速度
// ============================================================================

static int play(VideoContext& vc) {
    auto win   = create_window(vc.width, vc.height);
    auto rdr   = create_renderer(win.get());
    auto tex   = create_texture(rdr.get(), vc.width, vc.height);
    if (!win || !rdr || !tex) return 1;

    AVPacketPtr pkt(av_packet_alloc(), pkt_deleter);
    AVRational tb = vc.fmt_ctx->streams[vc.video_idx]->time_base;
    Uint32 last_pts = 0;

    while (av_read_frame(vc.fmt_ctx.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == vc.audio_idx) {
            // 音频包：放入队列，由音频解码线程处理
            vc.audio.pkt_queue.put(pkt.get());
        } else if (pkt->stream_index == vc.video_idx) {
            // 视频包：发送到解码器
            if (avcodec_send_packet(vc.codec_ctx.get(), pkt.get()) < 0) {
                av_packet_unref(pkt.get());
                continue;
            }

            // 接收解码后的帧并渲染
            while (avcodec_receive_frame(vc.codec_ctx.get(), vc.frame.get()) == 0) {
                // 锁定纹理，直接写入纹理内存
                void* pixels = nullptr;
                int pitch = 0;
                SDL_LockTexture(tex.get(), nullptr, &pixels, &pitch);

                // 计算 Y、U、V 三个平面在纹理内存中的起始地址
                uint8_t* dst[4];
                dst[0] = static_cast<uint8_t*>(pixels);         // Y 平面
                dst[1] = dst[0] + pitch * vc.height;            // U 平面
                dst[2] = dst[1] + (pitch / 2) * (vc.height / 2); // V 平面
                dst[3] = nullptr;

                int linesize[4];
                linesize[0] = pitch;
                linesize[1] = pitch / 2;
                linesize[2] = pitch / 2;
                linesize[3] = 0;

                // 像素格式转换：解码器输出格式 → YUV420P
                sws_scale(vc.sws_ctx.get(),
                          vc.frame->data, vc.frame->linesize, 0, vc.height,
                          dst, linesize);
                SDL_UnlockTexture(tex.get());

                // 渲染到屏幕
                SDL_RenderClear(rdr.get());
                SDL_RenderCopy(rdr.get(), tex.get(), nullptr, nullptr);
                SDL_RenderPresent(rdr.get());

                // 根据 PTS 计算帧间隔，控制播放速度
                Uint32 frame_ms = (Uint32)(vc.frame->pts - last_pts) *
                                  1000 * tb.num / tb.den;
                last_pts = (Uint32)vc.frame->pts;
                if (frame_ms > 0 && frame_ms < 5000)
                    SDL_Delay(frame_ms);
            }
        }
        av_packet_unref(pkt.get());

        // 处理 SDL 事件（窗口关闭、ESC 退出等）
        SDL_Event evt;
        while (SDL_PollEvent(&evt)) {
            if (evt.type == SDL_QUIT ||
                (evt.type == SDL_KEYDOWN && evt.key.keysym.sym == SDLK_ESCAPE))
                return 0;
        }
    }
    return 0;
}

// ============================================================================
// main() — 程序入口
// ============================================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <视频文件>\n", argv[0]);
        return 1;
    }

    // 初始化 SDL（视频 + 音频 + 定时器）
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
        fprintf(stderr, "SDL 初始化失败: %s\n", SDL_GetError());
        return 1;
    }

    // 构建播放上下文（打开文件、解码器、SDL 设备等）
    auto vc = build_context(argv[1]);
    if (!vc) { SDL_Quit(); return 1; }

    // 进入播放循环
    int ret = play(*vc);

    // 清理资源
    vc.audio.stop();  // 通知音频解码线程退出并等待其结束
    SDL_CloseAudio();
    SDL_Quit();
    return ret;
}
