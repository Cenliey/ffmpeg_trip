

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <stdio.h>
}

#include <cassert>
#include <memory>

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif


// 保存一帧为 PPM 格式图片文件
void saveFrame(AVFrame* frame, int width, int height, int iFrame) {
    FILE *pfile;
    char szFileName[32];
    int y;
    sprintf(szFileName, "frame%d.ppm", iFrame);
    pfile = fopen(szFileName, "wb");
    assert(pfile != NULL);
    if(pfile==NULL) {
        return;
    }
    // Write header
    fprintf(pfile, "P6\n%d %d\n255\n", width, height);

    // Write pixel data
    for(y=0; y<height; y++) {
        fwrite(frame->data[0]+y*frame->linesize[0], 1, width*3, pfile);
    }

    // Close file
    fclose(pfile);
}

/*
* AVFormatContext 是 FFmpeg 中最顶层的容器上下文，你可以把它理解成**"整个媒体文件的总管"**。
  打开一个视频文件后，FFmpeg 会把文件的所有信息都塞到这个结构体里：
  AVFormatContext (整个文件的信息)
  ├── nb_streams          → 有几条流 (比如: 1个视频 + 1个音频 = 2)
  ├── streams[0]          → 视频流
  ├── streams[1]          → 音频流
  ├── duration            → 总时长
  ├── bit_rate            → 码率
  ├── format_name         → 格式名 (比如 "mp4", "avi")
  └── filename            → 文件名
 *
 */
// 查找第一个视频流，返回流索引，未找到返回 -1
int find_video_stream(AVFormatContext *pFormatCtx) {
    for (int i = 0;i<pFormatCtx->nb_streams;i++) {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            return i;
        }
    }
    fprintf(stderr, "No video stream found\n");
    return -1;
}
// 解码并保存帧的逻辑
int extract_frames(AVFormatContext *pFormatCtx,
    int videoStreamIndex,
    AVCodecContext  *pCodecContext,
    SwsContext *pSwsContext,
    AVFrame *pFrame,
    AVFrame *pFrameRGB,
    int targetFrames) {
    AVPacket *pPacket = av_packet_alloc();
    if (!pPacket) {
        fprintf(stderr, "无法分配数据包\n");
        return 1;
    }
    int frameCount = 0;
    int packetCount = 0;

    while (av_read_frame(pFormatCtx,pPacket) >= 0) {
        if (pPacket->stream_index != videoStreamIndex) {
            av_packet_unref(pPacket);
            continue;
        }

        packetCount++;
        fprintf(stderr, "处理 packet #%d, size=%d\n", packetCount, pPacket->size);

        int send_ret = avcodec_send_packet(pCodecContext, pPacket);
        if (send_ret < 0) {
            char errbuf[128];
            av_strerror(send_ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "发送数据包出错: %s\n", errbuf);
            av_packet_unref(pPacket);
            continue;
        }

        // 接收解码后的帧（一个数据包可能产生多帧）
        int recv_ret;
        while ((recv_ret = avcodec_receive_frame(pCodecContext, pFrame)) == 0) {
            fprintf(stderr, "成功获取帧 #%d\n", frameCount + 1);
            // 转换为 RGB 格式
            sws_scale(pSwsContext,
                (uint8_t const* const *)pFrame->data,
                pFrame->linesize,
                0,pCodecContext->height,
                pFrameRGB->data,pFrameRGB->linesize);

            //save to disk
            if (++frameCount <= targetFrames) {
                saveFrame(pFrameRGB, pCodecContext->width, pCodecContext->height,
                    frameCount);
            }
        }

        if (recv_ret != AVERROR(EAGAIN) && recv_ret < 0) {
            char errbuf[128];
            av_strerror(recv_ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "接收帧出错: %s\n", errbuf);
        }

        if (frameCount >= targetFrames) {
            av_packet_unref(pPacket);
            break;
        }

        av_packet_unref(pPacket);
    }
    av_packet_free(&pPacket);
    printf("已提取 %d 帧 (处理了 %d 个 packet)\n", frameCount, packetCount);
    return 0;


}

int run(const char *filename,
               AVFormatContext *&pFormatCtx,
               AVCodecContext  *&pCodecCtx,
               AVFrame         *&pFrame,
               AVFrame         *&pFrameRGB,
               SwsContext      *&sws_ctx,
               uint8_t         *&buffer) {
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
