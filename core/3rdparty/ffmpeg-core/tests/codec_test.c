#include <stdio.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

static int receive_frames(AVCodecContext *codec, AVFrame *frame, int *count)
{
    int result;
    while ((result = avcodec_receive_frame(codec, frame)) >= 0) {
        if (codec->codec_type == AVMEDIA_TYPE_VIDEO && (frame->width != 64 || frame->height != 48))
            return -1;
        if (codec->codec_type == AVMEDIA_TYPE_AUDIO && frame->nb_samples <= 0)
            return -1;
        ++*count;
        av_frame_unref(frame);
    }
    return result == AVERROR(EAGAIN) || result == AVERROR_EOF ? 0 : -1;
}

static int decode(const char *path)
{
    AVFormatContext *format = NULL;
    AVCodecContext *codecs[2] = {NULL, NULL};
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    int indices[2], counts[2] = {0, 0};
    int result = 1;
    if (!frame || !packet || avformat_open_input(&format, path, NULL, NULL) < 0 ||
        avformat_find_stream_info(format, NULL) < 0)
        goto done;
    for (int i = 0; i < 2; ++i) {
        const AVCodec *decoder = NULL;
        indices[i] = av_find_best_stream(format, i ? AVMEDIA_TYPE_AUDIO : AVMEDIA_TYPE_VIDEO,
                                        -1, -1, &decoder, 0);
        if (indices[i] < 0 || !(codecs[i] = avcodec_alloc_context3(decoder)) ||
            avcodec_parameters_to_context(codecs[i], format->streams[indices[i]]->codecpar) < 0 ||
            avcodec_open2(codecs[i], decoder, NULL) < 0)
            goto done;
    }
    int read_result;
    while ((read_result = av_read_frame(format, packet)) >= 0) {
        for (int i = 0; i < 2; ++i) {
            if (packet->stream_index == indices[i] &&
                (avcodec_send_packet(codecs[i], packet) < 0 ||
                 receive_frames(codecs[i], frame, &counts[i]) < 0))
                goto done;
        }
        av_packet_unref(packet);
    }
    if (read_result != AVERROR_EOF)
        goto done;
    for (int i = 0; i < 2; ++i) {
        if (avcodec_send_packet(codecs[i], NULL) < 0 ||
            receive_frames(codecs[i], frame, &counts[i]) < 0)
            goto done;
    }
    printf("%s: %d video frames, %d audio frames\n", path, counts[0], counts[1]);
    result = counts[0] == 5 && counts[1] > 0 ? 0 : 1;
done:
    avcodec_free_context(&codecs[0]);
    avcodec_free_context(&codecs[1]);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avformat_close_input(&format);
    return result;
}

int main(int argc, char **argv)
{
    const enum AVCodecID required[] = {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_AAC,
        AV_CODEC_ID_ATRAC9, AV_CODEC_ID_VP8, AV_CODEC_ID_VP9, AV_CODEC_ID_OPUS, AV_CODEC_ID_VORBIS};
    for (unsigned i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        const AVCodec *codec = avcodec_find_decoder(required[i]);
        if (!codec) {
            fprintf(stderr, "Missing decoder: %s\n", avcodec_get_name(required[i]));
            return 1;
        }
        printf("Decoder: %s\n", codec->name);
    }
    if (!av_find_input_format("matroska") ||
        strcmp(avcodec_license(), "LGPL version 2.1 or later") != 0)
        return 1;
    return argc > 1 ? decode(argv[1]) : 0;
}
