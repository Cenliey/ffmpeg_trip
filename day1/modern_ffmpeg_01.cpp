// modern_ffmpeg_01.cpp
// 现代 FFmpeg 视频帧提取程序 (FFmpeg 5.0+)
// 从视频中提取前 5 帧并保存为 PPM 图片。
//
// 编译 (CMake):
//   cmake -S . -B build && cmake --build build
//
// 运行:
//   ./build/modern_ffmpeg_01 <视频文件>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// 保存一帧为 PPM 格式图片文件
static void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame) {
    char szFilename[32];
    snprintf(szFilename, sizeof(szFilename), "frame%d.ppm", iFrame);

    FILE *pFile = fopen(szFilename, "wb");
    if (!pFile) {
        fprintf(stderr, "无法打开文件 %s\n", szFilename);
        return;
    }

    // PPM 文件头 (二进制 RGB 格式)
    fprintf(pFile, "P6\n%d %d\n255\n", width, height);

    // 写入像素数据
    for (int y = 0; y < height; y++) {
        fwrite(pFrame->data[0] + y * pFrame->linesize[0],
               1, width * 3, pFile);
    }

    fclose(pFile);
    printf("已保存 %s\n", szFilename);
}

// 查找第一个视频流，返回流索引，未找到返回 -1
static int find_video_stream(AVFormatContext *pFormatCtx) {
    for (unsigned i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            return static_cast<int>(i);
        }
    }
    fprintf(stderr, "未找到视频流\n");
    return -1;
}

// 解码并保存帧的逻辑
static int extract_frames(AVFormatContext *pFormatCtx,
                          int videoStream,
                          AVCodecContext *pCodecCtx,
                          SwsContext *sws_ctx,
                          AVFrame *pFrame,
                          AVFrame *pFrameRGB,
                          int targetFrames) {
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        fprintf(stderr, "无法分配数据包\n");
        return 1;
    }

    int frameCount = 0;

    while (av_read_frame(pFormatCtx, packet) >= 0) {
        if (packet->stream_index != videoStream) {
            av_packet_unref(packet);
            continue;
        }

        // 发送数据包给解码器
        int send_ret = avcodec_send_packet(pCodecCtx, packet);
        if (send_ret < 0) {
            fprintf(stderr, "发送数据包出错\n");
            break;
        }

        // 接收解码后的帧（一个数据包可能产生多帧）
        while (avcodec_receive_frame(pCodecCtx, pFrame) >= 0) {
            // 转换为 RGB 格式
            sws_scale(sws_ctx,
                      (uint8_t const *const *)pFrame->data,
                      pFrame->linesize,
                      0, pCodecCtx->height,
                      pFrameRGB->data, pFrameRGB->linesize);

            // 保存到磁盘
            if (++frameCount <= targetFrames) {
                SaveFrame(pFrameRGB, pCodecCtx->width, pCodecCtx->height,
                          frameCount);
            }
        }

        if (frameCount >= targetFrames) {
            av_packet_unref(packet);
            break;
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    printf("已提取 %d 帧\n", frameCount);
    return 0;
}

// 主处理逻辑，成功返回 0，失败返回 1 并打印错误信息
static int run(const char *filename,
               AVFormatContext *&pFormatCtx,
               AVCodecContext  *&pCodecCtx,
               AVFrame         *&pFrame,
               AVFrame         *&pFrameRGB,
               SwsContext      *&sws_ctx,
               uint8_t         *&buffer) {

    // FFmpeg 4.0+ 自动注册所有编解码器和格式，无需手动调用。
    // av_register_all() 已在 FFmpeg 5.0 中移除。

    // 打开输入文件
    if (avformat_open_input(&pFormatCtx, filename, nullptr, nullptr) < 0) {
        fprintf(stderr, "无法打开文件: %s\n", filename);
        return 1;
    }

    // 获取流信息
    if (avformat_find_stream_info(pFormatCtx, nullptr) < 0) {
        fprintf(stderr, "找不到流信息\n");
        return 1;
    }

    // 打印文件信息到 stderr
    av_dump_format(pFormatCtx, 0, filename, 0);

    // 查找第一个视频流
    int videoStream = find_video_stream(pFormatCtx);
    if (videoStream == -1) {
        return 1;
    }

    // 获取编解码参数并找到对应的解码器
    AVCodecParameters *codecpar = pFormatCtx->streams[videoStream]->codecpar;
    const AVCodec *pCodec = avcodec_find_decoder(codecpar->codec_id);
    if (!pCodec) {
        fprintf(stderr, "不支持的编解码器\n");
        return 1;
    }

    // 分配解码器上下文并填充参数
    pCodecCtx = avcodec_alloc_context3(pCodec);
    if (!pCodecCtx) {
        fprintf(stderr, "无法分配解码器上下文\n");
        return 1;
    }
    if (avcodec_parameters_to_context(pCodecCtx, codecpar) < 0) {
        fprintf(stderr, "无法拷贝编解码参数\n");
        return 1;
    }

    // 打开解码器
    if (avcodec_open2(pCodecCtx, pCodec, nullptr) < 0) {
        fprintf(stderr, "无法打开解码器\n");
        return 1;
    }

    // 分配帧
    pFrame    = av_frame_alloc();
    pFrameRGB = av_frame_alloc();
    if (!pFrame || !pFrameRGB) {
        fprintf(stderr, "无法分配帧\n");
        return 1;
    }

    // 为 RGB 图像分配缓冲区
    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24,
                                            pCodecCtx->width,
                                            pCodecCtx->height, 1);
    if (numBytes < 0) {
        fprintf(stderr, "无法计算缓冲区大小\n");
        return 1;
    }
    buffer = (uint8_t *)av_malloc(static_cast<size_t>(numBytes));

    // 将缓冲区绑定到 pFrameRGB
    int img_ret = av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize,
                                       buffer, AV_PIX_FMT_RGB24,
                                       pCodecCtx->width, pCodecCtx->height, 1);
    if (img_ret < 0) {
        fprintf(stderr, "无法填充图像数组\n");
        return 1;
    }

    // 创建格式转换上下文 (原始像素格式 -> RGB24)
    sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height,
                             pCodecCtx->pix_fmt,
                             pCodecCtx->width, pCodecCtx->height,
                             AV_PIX_FMT_RGB24, SWS_BILINEAR,
                             nullptr, nullptr, nullptr);
    if (!sws_ctx) {
        fprintf(stderr, "无法初始化 sws 上下文\n");
        return 1;
    }

    // 解码并保存前 5 帧
    return extract_frames(pFormatCtx, videoStream, pCodecCtx, sws_ctx,
                          pFrame, pFrameRGB, 5);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("用法: %s <视频文件>\n", argv[0]);
        return 1;
    }

    AVFormatContext *pFormatCtx = nullptr;
    AVCodecContext  *pCodecCtx  = nullptr;
    AVFrame         *pFrame     = nullptr;
    AVFrame         *pFrameRGB  = nullptr;
    SwsContext      *sws_ctx    = nullptr;
    uint8_t         *buffer     = nullptr;

    int ret = run(argv[1], pFormatCtx, pCodecCtx, pFrame, pFrameRGB,
                  sws_ctx, buffer);

    // 统一释放所有资源
    av_free(buffer);
    sws_freeContext(sws_ctx);
    av_frame_free(&pFrameRGB);
    av_frame_free(&pFrame);
    avcodec_free_context(&pCodecCtx);
    avformat_close_input(&pFormatCtx);

    return ret;
}
