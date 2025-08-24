#include <libavutil/rational.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <endian.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>
#include <curses.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/version.h>
#include <libavformat/version.h>
#include <libavcodec/version.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>

#if __has_include(<SDL3/SDL.h>)

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_version.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_init.h>

#elif __has_include(<SDL2/SDL.h>)

#include <SDL2/SDL_audio.h>
#include <SDL2/SDL_version.h>
#include <SDL2/SDL_rwops.h>
#include <SDL2/SDL.h>

#else

#error "Requires SDL2 or later"

#endif

#define PROGRAM_NAME "vidtty"
#define VERSION "2.0.0a"
#define COPYRIGHT "Copyright (C) 2025"
#define LICENSE "MIT"
#define AUTHOR "Revnoplex"
#define VIDTXT_HEADER_SIZE 64
#define VID_METADATA_START 8
#define VIDTXT_EXT ".vidtxt"
#define SUFFIX_MAX_SIZE sizeof("[ 100% ]")

typedef struct _VIDTTYOptions VIDTTYOptions;
typedef struct _VIDTTYArguments VIDTTYArguments;

typedef struct {
    FILE *fp;
    uint64_t file_size;
    uint32_t columns;
    uint32_t lines;
    double fps;
    uint64_t audio_size;
    uint32_t print_columns;
    uint32_t print_lines;
    uint64_t total_frames;
    double duration;
} VIDTXTInfo;

typedef struct _VIDTTYOptions {
    int32_t debug_mode;
    int32_t no_audio;
    char *tty;
    uint32_t columns;
    uint32_t lines;
    VIDTTYArguments *arguments;
} VIDTTYOptions;

typedef int32_t (*stdcall)(char *, VIDTTYOptions*);

typedef struct {
    char *name;
    int8_t type;
    /*
    Types:
    0. Toggle
    1. Value
    2. Function
    */
    stdcall associated_call;
    // the accociated function if type 2.
    char *description;
    char **aliases;
    int32_t alias_count;
    char *usage;
    void *associated_option;
    // The VIDTTYOptions associated with the argument.
    int8_t associated_typedef;
     /*
    associated_typedef is the value type for a type 2 option.
    Types:
    0. Signed Integer
    1. Unsigned Inteter
    2. String
    3. String containing Unsigned Integers
    */
} VIDTTYArgument;

typedef struct _VIDTTYArguments {
    VIDTTYArgument **argumentv;
    uint32_t argumentc;
} VIDTTYArguments;

VIDTXTInfo *new_vidtxt_info(FILE *fp, char *filename) {
    VIDTXTInfo *vidtxt_info = malloc(sizeof(VIDTXTInfo));

    if (ftell(fp) != 0) {
        fprintf(stderr, "File pointer not seeked to start\n");
        free(vidtxt_info);
        return NULL;
    }

    vidtxt_info->fp = fp;

    uint64_t sig_value = 0;
    fread(&sig_value, 6, 1, vidtxt_info->fp);

    if (be64toh(sig_value) != 0x5649445458540000) {
        if (filename == NULL) {
            fprintf(stderr, "The file is not vidtxt format!\n");
        } else {
            fprintf(stderr, "%s is not vidtxt format!\n", filename);
        }
        free(vidtxt_info);
        return NULL;
    }

    if (fseek(vidtxt_info->fp, VID_METADATA_START,0)) {
        fprintf(stderr, "Error seeking to position %d: Seek error %d: %s\n", VID_METADATA_START, errno, strerror(errno));
        free(vidtxt_info);
        return NULL;
    }
    if (ftell(vidtxt_info->fp) != VID_METADATA_START) {
        fprintf(stderr, "Unable to seek to position %d\n", VID_METADATA_START);
        free(vidtxt_info);
        return NULL;
    }

    int32_t total_reads = 0;

    total_reads += fread(&vidtxt_info->columns, sizeof(vidtxt_info->columns), 1, vidtxt_info->fp);
    total_reads += fread(&vidtxt_info->lines, sizeof(vidtxt_info->lines), 1, vidtxt_info->fp);
    total_reads += fread(&vidtxt_info->fps, sizeof(vidtxt_info->fps), 1, vidtxt_info->fp);
    total_reads += fread(&vidtxt_info->audio_size, sizeof(vidtxt_info->audio_size), 1, vidtxt_info->fp);

    if (total_reads != 4) {
        fprintf(stderr, "Error reading header\n");
        free(vidtxt_info);
        return NULL;
    }

    vidtxt_info->columns = be32toh(vidtxt_info->columns);
    vidtxt_info->lines = be32toh(vidtxt_info->lines);
    uint64_t raw_fps;
    double new_fps;
    memmove(&raw_fps, &vidtxt_info->fps, sizeof(raw_fps));
    raw_fps = be64toh(raw_fps);
    memmove(&new_fps, &raw_fps, sizeof(new_fps));
    vidtxt_info->audio_size = be64toh(vidtxt_info->audio_size);

    if (new_fps >= 0 && 1/new_fps != INFINITY) {
        vidtxt_info->fps = new_fps;
    } else {
        fprintf(stderr, "Warning: Error interpreting fps value in big endian. Trying in little endian...\n");
    }
    if (vidtxt_info->fps < 0) {
        fprintf(stderr, "Error interpreting fps value. Possibly wrong endian value\n");
        free(vidtxt_info);
        return NULL;
    }

    struct stat file_stat;
    if (fstat(fileno(vidtxt_info->fp), &file_stat)) {
        if (filename == NULL) {
            fprintf(stderr, "Couldn't stat vidtxt file to get size: Stat error %d: %s\n", errno, strerror(errno));
        } else {
            fprintf(stderr, "Couldn't stat %s to get size: Stat error %d: %s\n", filename, errno, strerror(errno));
        }
        free(vidtxt_info);
        return NULL;
    }
    vidtxt_info->file_size = file_stat.st_size;
    if (vidtxt_info->columns <= 1 || vidtxt_info->lines <= 1) {
        fprintf(stderr, "Invalid vidtxt resolution! Must be greater than 1x1\n");
        free(vidtxt_info);
        return NULL;
    }
    vidtxt_info->print_columns = vidtxt_info->columns - 1;
    vidtxt_info->print_lines = vidtxt_info->lines - 1;
    vidtxt_info->total_frames = (vidtxt_info->file_size - VIDTXT_HEADER_SIZE - vidtxt_info->audio_size) / (vidtxt_info->print_columns*vidtxt_info->print_lines);
    vidtxt_info->duration = floor(vidtxt_info->total_frames / vidtxt_info->fps) + fmod(vidtxt_info->total_frames,  vidtxt_info->fps) / vidtxt_info->fps;

    if (fseek(vidtxt_info->fp, VIDTXT_HEADER_SIZE,0)) {
        fprintf(stderr, "Error seeking to position %d: Seek error %d: %s\n", VIDTXT_HEADER_SIZE, errno, strerror(errno));
        free(vidtxt_info);
        return NULL;
    }
    if (ftell(vidtxt_info->fp) != VIDTXT_HEADER_SIZE) {
        fprintf(stderr, "Unable to seek to position %d\n", VIDTXT_HEADER_SIZE);
        free(vidtxt_info);
        return NULL;
    }

    return vidtxt_info;
}

VIDTXTInfo *open_vidtxt(char *filename) {
    FILE *fp = fopen(filename, "rb");

    if (fp == NULL) {
        fprintf(stderr, "Couldn't open %s: %s\n", filename, strerror(errno));
        return NULL;  
    }

    VIDTXTInfo *vidtxt_info = new_vidtxt_info(fp, filename);
    if (vidtxt_info == NULL) {
        fclose(fp);
    }
    return vidtxt_info;
}

int32_t int_str_asprintf(char **restrict ptr, const char *restrict fmt, int32_t d, char *s) {
    // format string must contain exactly one 32-bit integer followed by exactly char ptr 
    // array or undefined behavior may occur
    int32_t calculated_size = 0;
    int32_t fmt_size;
    int32_t str_size;

    for (fmt_size = 0; fmt[fmt_size] != '\0'; fmt_size++);
    calculated_size+= fmt_size;

    // note: null character space gets included here.
    calculated_size += snprintf(NULL, 0, "%d", d);

    for(str_size = 0; s[str_size] != '\0'; str_size++);
    calculated_size+= str_size;

    *ptr = malloc(calculated_size);

    snprintf(*ptr, calculated_size, fmt, d, s);

    return calculated_size;
}

int32_t includes_match(char *string, char *match) {
    int32_t sizeof_match;
    int32_t matches = 0;
    for (sizeof_match = 0; match[sizeof_match] != '\0'; sizeof_match++);
    for (int32_t idx = 0; string[idx] != '\0'; idx++) {
        if (idx+1 >= sizeof_match) {
            for (int32_t sub_idx = 0; sub_idx < sizeof_match; sub_idx++) {
                if (string[idx-sizeof_match+1+sub_idx] != match[sub_idx]) {
                    break;
                }
                if (sub_idx+1 == sizeof_match) {
                    matches = 1;
                }
            }
        }
    }
    return matches;
}

char *extract_filename_from_url(char *url, int32_t include_ext, char *add_str, uint32_t add_strc) {
    uint32_t add_strc_check = add_strc;
    if (add_str != NULL && add_strc == 0) {
        for (;add_str[add_strc] != '\0'; add_strc++);
    }
    if (!add_strc_check) {
        add_strc++;
    }
    int32_t sizeof_url;
    for(sizeof_url = 0; url[sizeof_url] != '\0'; sizeof_url++);
    char *output_buffer = malloc(sizeof_url+add_strc);
    int32_t buffer_idx = 0;
    int32_t stop_adding = 0;
    for (int32_t idx = 0; url[idx] != '\0'; idx++) {
        if (url[idx] == '?') {
            break;
        }
        if (!include_ext && url[idx]== '.') {
            stop_adding = 1;
        }
        if (!stop_adding) {
            output_buffer[buffer_idx] = url[idx];
        }
        if (url[idx] == '/') {
            stop_adding = 0;
            buffer_idx = 0;
        } else if (!stop_adding){
            buffer_idx++;
        }
    }
    if (add_str == NULL) {
        output_buffer[buffer_idx] = '\0';
    } else {
        for (uint32_t idx = 0; idx < add_strc; idx++) {
            output_buffer[buffer_idx+idx] = add_str[idx];
        }
    }
    return output_buffer;
}

char *progress_bar(uint16_t columns, char *prefix, int32_t prefix_size, char *suffix, int32_t suffix_size, uint64_t numerator, uint64_t denominator) {
    for (int32_t idx = prefix_size; idx < columns+1; idx++) {
        prefix[idx] = ' ';
    }
    for (int32_t idx = 0; idx < suffix_size; idx++) {
        prefix[columns-suffix_size+idx] = suffix[idx];
    }
    prefix[columns] = '\0';
    char *full_bar = malloc(columns+9);
    char escape_prefix[] = "\x1b[7m";
    snprintf(full_bar, sizeof(escape_prefix), "%s", escape_prefix);
    char insert[] = "\x1b[0m";
    int32_t insert_offset = columns*numerator / denominator;
    for (int32_t idx = 4; idx < columns+9; idx++) {
        if (idx >= insert_offset+4 && idx < insert_offset+4+4) {
            full_bar[idx] = insert[idx-insert_offset-4];
        } else {
            if (idx <= columns+8) {
                int32_t data_offset = 4;
                if (idx >= insert_offset+4) {
                    data_offset = 8;
                }
                full_bar[idx] = prefix[idx-data_offset];
            } else {
                full_bar[idx] = 'E';
            }
        }
    }
    full_bar[columns+9-1] = '\0';
    return full_bar;
}

int32_t avio_custom_read(void *opaque, uint8_t *buffer, int buffer_size) {
    // cast opaque to file pointer struct
    VIDTXTInfo *vidtxt_info = (VIDTXTInfo*)opaque;

    /* 
    failsafe in case the file pointer is not seeked past VIDTXT_HEADER_SIZE which 
    would cause uint underflow leading to unexpected behaviour with the audio_size comparison statements.
    */
    int32_t current_position = ftell(vidtxt_info->fp)-VIDTXT_HEADER_SIZE;
    if (current_position < 0) {
        fprintf(stderr, "Got unexpected negative value when comparing audio_size!\n");
        return AVERROR_UNKNOWN;
    }
    if ((uint64_t)current_position+buffer_size > vidtxt_info->audio_size) {
        buffer_size = vidtxt_info->audio_size - current_position;
    }

    size_t len = fread(buffer, 1, buffer_size, vidtxt_info->fp);
    if (len == 0 || (uint64_t)current_position >= vidtxt_info->audio_size) {
        return AVERROR_EOF;
    }
    return (int) len;
}

int32_t file_print_frames(char *filename, VIDTTYOptions *options) {
    VIDTXTInfo *vidtxt_info = open_vidtxt(filename);
    if (vidtxt_info == NULL) {
        return 1;
    }
    
    int32_t curses_init = 0;
    char *queued_err_msg = NULL;
    int32_t status = 0;
    char *line_contents = NULL;
    uint8_t *wav_data = NULL;
    uint32_t wav_data_len = 0;
    uint8_t *wav_buffer = NULL;
#if SDL_VERSION_ATLEAST(3, 0, 0)
    SDL_AudioStream *stream = NULL;
#else
    SDL_AudioDeviceID *stream = NULL;
#endif
    AVIOContext *avio_ctx = NULL;
    AVPacket *pkt = NULL;
    AVFrame *decoded = NULL;
    AVFrame *converted = NULL;
    SwrContext *swr_ctx = NULL;
    size_t wav_size = 0;
    AVIOContext *out_avio_ctx = NULL;
    AVCodecContext *encoder_ctx = NULL;
    AVCodecContext *decoder_ctx = NULL;
    AVFormatContext *out_fmt_ctx = NULL;
    AVFormatContext *avfmt_ctx = NULL;
    struct winsize term_size;
    struct timespec draw_spec;
    uint64_t pre_duration;
    double numerator;
    double denominator;
    
    if (vidtxt_info->audio_size > 0 && options->no_audio == 0) {

        #define AVIO_BUFFER_SIZE 4096
        uint8_t *avio_buffer = malloc(AVIO_BUFFER_SIZE);
        avio_ctx = avio_alloc_context(avio_buffer, AVIO_BUFFER_SIZE, 0, vidtxt_info, avio_custom_read, NULL, NULL);
        if (avio_ctx == NULL) {
            status = -1;
            fprintf(stderr,"Error allocating avio context\n");
            goto ffmpeg_cleanup;
        }

        avfmt_ctx = avformat_alloc_context();
        avfmt_ctx->pb = avio_ctx;
        if ((status = avformat_open_input(&avfmt_ctx, NULL, NULL, NULL)) < 0) {
            fprintf(stderr, "Could not read audio data: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto ffmpeg_cleanup;
        }
        if ((status = avformat_find_stream_info(avfmt_ctx, NULL)) < 0) {
            fprintf(stderr, "Could not find stream information: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto ffmpeg_cleanup;
        }

        int32_t stream_idx = av_find_best_stream(avfmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
        if (stream_idx < 0) {
            status = stream_idx;
            fprintf(stderr, "Could not find audio stream: FFmpeg error 0x%02x: %s\n", stream_idx, av_err2str(stream_idx));
            goto ffmpeg_cleanup;
        }

        AVStream *input_audio_stream = avfmt_ctx->streams[stream_idx];
        const AVCodec *decoder = avcodec_find_decoder(input_audio_stream->codecpar->codec_id);
        decoder_ctx = avcodec_alloc_context3(decoder);
        avcodec_parameters_to_context(decoder_ctx, input_audio_stream->codecpar);
        avcodec_open2(decoder_ctx, decoder, NULL);

        if ((status = avformat_alloc_output_context2(&out_fmt_ctx, NULL, "wav", NULL)) < 0) {
            fprintf(stderr, "Could not create output format context: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto ffmpeg_cleanup;
        }

        if ((status = avio_open_dyn_buf(&out_avio_ctx)) < 0) {
            fprintf(stderr, "Could not create output buffer: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto ffmpeg_cleanup;
        }

        out_fmt_ctx->pb = out_avio_ctx;

        const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
        AVStream *output_audio_stream = avformat_new_stream(out_fmt_ctx, encoder);
        encoder_ctx = avcodec_alloc_context3(encoder);
        encoder_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
        encoder_ctx->sample_rate = decoder_ctx->sample_rate;
        encoder_ctx->time_base = (AVRational){1, decoder_ctx->sample_rate};
#if LIBAVUTIL_VERSION_MAJOR >= 57
        if ((status = av_channel_layout_copy(&encoder_ctx->ch_layout, &decoder_ctx->ch_layout)) < 0) {
            fprintf(stderr, "Failed to copy channel layout: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto ffmpeg_cleanup;
        }
#else 
        encoder_ctx->channel_layout = decoder_ctx->channel_layout;
        encoder_ctx->channels = decoder_ctx->channels;
#endif
        if ((status = avcodec_open2(encoder_ctx, encoder, NULL)) < 0) {
            fprintf(stderr, "Could not open encoder: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto ffmpeg_cleanup;
        }
        if ((status = avcodec_parameters_from_context(output_audio_stream->codecpar, encoder_ctx)) < 0) {
            fprintf(stderr, "Could not transfer codec paramaters: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto ffmpeg_cleanup;
        }
        if ((status = avformat_write_header(out_fmt_ctx, NULL)) < 0) {
            fprintf(stderr, "Could not write header: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto ffmpeg_cleanup;
        }

#if LIBAVUTIL_VERSION_MAJOR >= 57
        status = swr_alloc_set_opts2(
            &swr_ctx,
            &encoder_ctx->ch_layout, encoder_ctx->sample_fmt, encoder_ctx->sample_rate,
            &decoder_ctx->ch_layout, decoder_ctx->sample_fmt, decoder_ctx->sample_rate,
            0, NULL
        );
#else
        swr_ctx = swr_alloc_set_opts(
            NULL,
            encoder_ctx->channel_layout, encoder_ctx->sample_fmt, encoder_ctx->sample_rate,
            decoder_ctx->channel_layout, decoder_ctx->sample_fmt, decoder_ctx->sample_rate,
            0, NULL
        );
        status = (swr_ctx == NULL) ? AVERROR(ENOMEM) : 0;
#endif
        if (status < 0) {
            fprintf(stderr, "Failed to allocate SwrContext: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto ffmpeg_cleanup;
        }
        if ((status = swr_init(swr_ctx)) < 0) {
            fprintf(stderr, "Failed to initialize SwrContext: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto ffmpeg_cleanup;
        }

        pkt = av_packet_alloc();
        decoded = av_frame_alloc();
        converted = av_frame_alloc();
        converted->format = encoder_ctx->sample_fmt;
        converted->sample_rate = encoder_ctx->sample_rate;

#if LIBAVUTIL_VERSION_MAJOR >= 57
        if ((status = av_channel_layout_copy(&converted->ch_layout, &encoder_ctx->ch_layout)) < 0) {
            fprintf(stderr, "Failed to copy channel layout: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto ffmpeg_cleanup;
        }
#else 
        converted->channel_layout = encoder_ctx->channel_layout;
        converted->channels = encoder_ctx->channels;
#endif
        if (clock_gettime(CLOCK_MONOTONIC, &draw_spec) == ERR) {
            status = -1;
            fprintf(stderr, "Couldn't get timestamp: errno %d: %s\n", errno, strerror(errno));
            goto ffmpeg_cleanup;
        }
        pre_duration = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000;
        numerator = 0;
        denominator = 1;
        int64_t next_pts = 0;
        uint64_t frame_count = 0;
        int64_t nb_frames = input_audio_stream->nb_frames;
        if (nb_frames <= 0) {
            fprintf(stderr, "Warning: No frame count metadata! Estimating from bitrate, sample rate and frame size (this may be inaccurate)...\n");
            double total_samples = ((vidtxt_info->audio_size * 8.0 / input_audio_stream->codecpar->bit_rate) * input_audio_stream->codecpar->sample_rate);
            nb_frames = floor(total_samples / input_audio_stream->codecpar->frame_size)-1;
        }
        printf("Writing Audio Frames...\r");
        fflush(stdout);
        while ((status = av_read_frame(avfmt_ctx, pkt)) >= 0) {
            if (pkt->stream_index != stream_idx) {
                av_packet_unref(pkt);
                continue;
            }

            if ((status = avcodec_send_packet(decoder_ctx, pkt)) < 0) {
                fprintf(stderr, "Warning: Error sending packet to decoder: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                break;
            }
            while ((status = avcodec_receive_frame(decoder_ctx, decoded)) == 0) {
                av_frame_unref(converted);  
                converted->nb_samples = decoded->nb_samples;
                
                converted->format       = encoder_ctx->sample_fmt;
                converted->sample_rate  = encoder_ctx->sample_rate;

#if LIBAVUTIL_VERSION_MAJOR >= 57
                if ((status = av_channel_layout_copy(&converted->ch_layout, &encoder_ctx->ch_layout)) < 0) {
                    fprintf(stderr, "Failed to copy channel layout: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                    goto ffmpeg_cleanup;
                }
#else 
                converted->channel_layout = encoder_ctx->channel_layout;
                converted->channels = encoder_ctx->channels;
#endif
                
                if ((status = av_frame_get_buffer(converted, 0)) < 0) {
                    fprintf(stderr, "Failed to allocate converted frame buffer: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                    goto ffmpeg_cleanup;
                }

                int out_samples = swr_convert(
                    swr_ctx,
                    converted->data, converted->nb_samples,
                    (const uint8_t **)decoded->data, decoded->nb_samples);

                if (out_samples < 0) {
                    status = out_samples;
                    fprintf(stderr, "Error during resampling: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                    goto ffmpeg_cleanup;
                }

                converted->pts = next_pts;
                next_pts += out_samples;

                if ((status = avcodec_send_frame(encoder_ctx, converted)) < 0) {
                    fprintf(stderr, "Error sending frame to encoder: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                    goto ffmpeg_cleanup;
                }

                while ((status = avcodec_receive_packet(encoder_ctx, pkt)) == 0) {
                    pkt->stream_index = output_audio_stream->index;
                    if ((status = av_interleaved_write_frame(out_fmt_ctx, pkt)) < 0) {
                        fprintf(stderr, "Error writing audio frame: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                        goto ffmpeg_cleanup;
                    }
                    av_packet_unref(pkt);
                }

                av_frame_unref(decoded);
                av_frame_unref(converted);
            }

            av_packet_unref(pkt);
            frame_count++;
            if ((int64_t) frame_count > nb_frames) {
                    nb_frames = frame_count;
                }
            if (clock_gettime(CLOCK_MONOTONIC, &draw_spec) == ERR) {
                status = -1;
                fprintf(stderr, "Couldn't get timestamp: errno %d: %s\n", errno, strerror(errno));
                goto ffmpeg_cleanup;
            }
            uint64_t frame_duration = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000 - pre_duration;
            pre_duration = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000;
            double rate = (double)1000000 / frame_duration;
            numerator+=rate;
            double time_left = (nb_frames-frame_count) / (numerator/denominator);
            if (frame_count % 64 == 0) {
                if (ioctl(1, TIOCGWINSZ, &term_size) == -1) {
                    fprintf(stderr, "Could't get terminal size: ioctl error %d: %s\n", errno, strerror(errno));
                    goto ffmpeg_cleanup;
                }
                char *prefix = malloc(term_size.ws_col+1);
                int32_t prefix_size;
                int32_t suffix_size;
                char *suffix;
                char *full_bar;
                if (nb_frames > 0) {
                    prefix_size = snprintf(
                        prefix, term_size.ws_col+1,
                        "Writing Audio Frame: %lu/%ld Rate: %.1lf/s Time Left: %02u:%02u:%06.3lf", 
                        frame_count, nb_frames, numerator/denominator,
                        (uint32_t) floor(time_left / 3600), (uint32_t) floor(time_left / 60), fmod(time_left, 60)
                    );
                    suffix = malloc(SUFFIX_MAX_SIZE);
                    suffix_size = snprintf(suffix, SUFFIX_MAX_SIZE, "[ %lu%% ]", 100*(frame_count) / nb_frames);
                    full_bar = progress_bar(term_size.ws_col, prefix, prefix_size, suffix, suffix_size, frame_count, nb_frames);
                } else {
                    prefix_size = snprintf(
                        prefix, term_size.ws_col+1,
                        "Writing Audio Frame: %lu Rate: %.1lf/s", 
                        frame_count, numerator/denominator
                    );
                    suffix = malloc(SUFFIX_MAX_SIZE);
                    suffix_size = snprintf(suffix, SUFFIX_MAX_SIZE, "[ ???%% ]");
                    full_bar = progress_bar(term_size.ws_col, prefix, prefix_size, suffix, suffix_size, 0, 1);
                }
                free(suffix);
                free(prefix);
                printf("%s\r", full_bar);
                free(full_bar);
                fflush(stdout);
            }
            denominator++;
        }
        if (ioctl(1, TIOCGWINSZ, &term_size) == -1) {
            fprintf(stderr, "Could't get terminal size: ioctl error %d: %s\n", errno, strerror(errno));
            goto ffmpeg_cleanup;
        }
        char *prefix = malloc(term_size.ws_col+9);
        if (nb_frames <= 0) {
            nb_frames = frame_count;
        }
        int32_t amount = snprintf(
            prefix, term_size.ws_col+1,
            "\x1b[7mWriting Audio Frame: %lu/%ld Rate: %.1lf/s Time Left: %02u:%02u:%06.3lf", 
            frame_count, nb_frames, numerator/denominator,
            0, 0, 0.0
        );
        for (int32_t idx = amount; idx < term_size.ws_col+9; idx++) {
            prefix[idx] = ' ';
        }
        char *suffix = malloc(SUFFIX_MAX_SIZE+4);
        int32_t suffix_size = snprintf(suffix, SUFFIX_MAX_SIZE+4, "[ %lu%% ]\x1b[0m", 100*(frame_count) / nb_frames);
        for (int32_t idx = 0; idx < suffix_size; idx++) {
            prefix[term_size.ws_col+8-suffix_size+idx] = suffix[idx];
        }
        free(suffix);
        prefix[term_size.ws_col+8] = '\0';
        printf("%s\n", prefix);
        free(prefix);

        if ((status = av_write_trailer(out_fmt_ctx)) < 0) {
            fprintf(stderr, "Error writing trailer: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto ffmpeg_cleanup;
        }
ffmpeg_cleanup:
        av_packet_free(&pkt);
        av_frame_free(&decoded);
        if (converted) {
#if LIBAVUTIL_VERSION_MAJOR >= 57
            av_channel_layout_uninit(&converted->ch_layout);
#endif
            av_frame_free(&converted);
        }
        swr_free(&swr_ctx);
        wav_size = avio_close_dyn_buf(out_avio_ctx, &wav_buffer);
        out_avio_ctx = NULL;
        avcodec_free_context(&encoder_ctx);
        avcodec_free_context(&decoder_ctx);
        avformat_free_context(out_fmt_ctx);
        avformat_close_input(&avfmt_ctx);
        if (avio_ctx) {
            av_freep(&avio_ctx->buffer);
        }
        avio_context_free(&avio_ctx);
        if (status < 0) {
            goto main_cleanup;
        }
        

        SDL_AudioSpec spec;
        
#if SDL_VERSION_ATLEAST(3, 0, 0)
        if (!SDL_SetAppMetadata(PROGRAM_NAME, VERSION, PROGRAM_NAME)) {
            status = -1;
            fprintf(stderr, "Error setting mixer metadata: %s\n", SDL_GetError());
            goto main_cleanup;
        }
        if (!SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_TYPE_STRING, "mediaplayer")) {
            status = -1;
            fprintf(stderr, "Error setting mixer metadata: %s\n", SDL_GetError());
            goto main_cleanup;
        }

#endif

        // SDL takes over signal handling for SIGINT and SIGTERM. We dont't want that so we change it back.

        // save current handlers
        struct sigaction int_action, term_action;
        sigaction(SIGINT, NULL, &int_action);
        sigaction(SIGTERM, NULL, &term_action);
#if SDL_VERSION_ATLEAST(3, 0, 0)
        if (!SDL_Init(SDL_INIT_AUDIO)) {
#else 
        if (SDL_Init(SDL_INIT_AUDIO) < 0 ) {
#endif
            status = -1;
            fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
            goto main_cleanup;
        }
        
        // set the saved hanlers back
        sigaction(SIGINT, &int_action, NULL);
        sigaction(SIGTERM, &term_action, NULL);

#if SDL_VERSION_ATLEAST(3, 0, 0)
        SDL_IOStream *wav_stream = SDL_IOFromMem(wav_buffer, wav_size);
        int32_t load_result = SDL_LoadWAV_IO(wav_stream, 1, &spec, &wav_data, &wav_data_len);
#else
        SDL_RWops *wav_stream = SDL_RWFromMem(wav_buffer, wav_size);
        SDL_AudioSpec *spec_result;
        spec_result = SDL_LoadWAV_RW(wav_stream, 1, &spec, &wav_data, &wav_data_len);
        int32_t load_result = (spec_result != NULL);
        spec = *spec_result;
#endif
        if (!load_result) {
            status = -1;
            fprintf(stderr, "Couldn't load .wav file: %s\n", SDL_GetError());
            goto main_cleanup;
        }

#if SDL_VERSION_ATLEAST(3, 0, 0)
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
#else
        stream = malloc(sizeof(SDL_AudioDeviceID));
        SDL_AudioDeviceID device_id = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
        if (device_id) {
            *stream = device_id;
        }
#endif
        if (!stream) {
            status = -1;
            fprintf(stderr, "Couldn't create audio stream: %s\n", SDL_GetError());
            goto main_cleanup;
        }
    }

    if (fseek(vidtxt_info->fp, VIDTXT_HEADER_SIZE + vidtxt_info->audio_size, 0)) {
        status = -1;
        fprintf(stderr, "Error seeking to position %lu: Seek error %d: %s\n", VIDTXT_HEADER_SIZE + vidtxt_info->audio_size, errno, strerror(errno));
        goto main_cleanup;
    }
    if ((uint64_t) ftell(vidtxt_info->fp) != VIDTXT_HEADER_SIZE + vidtxt_info->audio_size) {
        status = -1;
        fprintf(stderr, "Unable to seek to position %lu\n", VIDTXT_HEADER_SIZE + vidtxt_info->audio_size);
        goto main_cleanup;
    }
    
    double interval = 1 / vidtxt_info->fps;

    int32_t curses_fd = 1;
    char *curses_term = getenv("TERM");
    FILE *curses_stdin = stdin;
    FILE *curses_stdout = stdout;
    if (options->tty) {
        curses_stdin = fopen(options->tty, "r+"); 
        curses_stdout = fopen(options->tty, "w+");
        if (!curses_stdin || !curses_stdout) {
            if (errno == EACCES) {
                printf("Need permission to write to \x1b[1m%s\x1b[0m\nRunning sudo...\n", options->tty);
                uint64_t max_cb_size = snprintf(NULL, 0, "sudo chown %u %s", UINT32_MAX, options->tty);
                char *chown_buffer = malloc(max_cb_size+1);
                int32_t buffer_written = snprintf(chown_buffer, max_cb_size+1, "sudo chown %u %s", getuid(), options->tty);
                if (buffer_written <= 0) {
                    status = -1;
                    fprintf(stderr, "Error concentrating chown command\n");
                    goto main_cleanup;
                }
                int32_t chown_status = system(chown_buffer);
                free(chown_buffer);
                int32_t chown_failed = WEXITSTATUS(chown_status);
                if (WIFSIGNALED(chown_status)) {
                    if (WTERMSIG(chown_status) == 2) {
                        fprintf(stderr, "Sudo aborted by user\n");
                    } else {
                        fprintf(
                            stderr, "Sudo exited due to signal %d: %s\n", 
                            WTERMSIG(chown_status), strsignal(WTERMSIG(chown_status)));
                    }
                    status = 128+WTERMSIG(chown_status);
                    goto main_cleanup;
                }
                if (chown_failed) {
                    fprintf(stderr, "Changing ownership of %s failed with exit code %d!\n", options->tty, chown_failed);
                    status = -1;
                    goto main_cleanup;
                }
                if (chmod(options->tty, 0600)) {
                    fprintf(stderr, "Couldn't change permissions of %s: %s\n", options->tty, strerror(errno));
                    status = -1;
                    goto main_cleanup;
                }
                curses_stdin = fopen(options->tty, "r+"); 
                curses_stdout = fopen(options->tty, "w+");
                if (!curses_stdin || !curses_stdout) {
                    status = -1;
                    fprintf(stderr, "Couldn't open %s: %s\n", options->tty, strerror(errno));
                    goto main_cleanup;
                }
            } else {
                status = -1;
                fprintf(stderr, "Couldn't open %s: %s\n", options->tty, strerror(errno));
                goto main_cleanup;
            }
        }
        curses_fd = fileno(curses_stdout);
        // todo: write method to get the TERM value of another tty
        /*
        currently assuming the tty is a virtual console 
        which seems to display fine with most terminal emulators even 
        though they have a different TERM value eg. xterm.
        while not setting "linux" on a virtual console results in a messy output.
        */
        curses_term = "linux";
        printf("Running on another terminal session...\n");
    }

    if (ioctl(curses_fd, TIOCGWINSZ, &term_size) == -1) {
        status = -1;
        fprintf(stderr, "Could't get terminal size: ioctl error %d: %s\n", errno, strerror(errno));
        goto main_cleanup;
    }

    uint16_t curr_term_lines = term_size.ws_row;
    uint16_t curr_term_cols = term_size.ws_col;
    size_t ch_read = 1;

    SCREEN *screen = newterm(curses_term, curses_stdout, curses_stdin);
    if (screen == NULL) {
        status = -1;
        fprintf(stderr, "Error opening screen: errno %d: %s", errno, strerror(errno));
        goto main_cleanup;
    }
    noecho();
    cbreak();
    curses_init = 1;

    #define DRAW_ERROR_TOLERANCE 256

    int32_t draw_successful = 0;
    int32_t draw_errors = 0;
    line_contents = malloc(vidtxt_info->print_columns);
    
    uint64_t pre_draw;

    if (clock_gettime(CLOCK_MONOTONIC, &draw_spec) == ERR) {
        status = -1;
        int_str_asprintf(&queued_err_msg, "Couldn't get timestamp: errno %d: %s\n", errno, strerror(errno));
        goto main_cleanup;
    }
    pre_draw = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000;
    uint64_t frame_num = 0;
    if (vidtxt_info->audio_size > 0 && options->no_audio == 0) {
#if SDL_VERSION_ATLEAST(3, 0, 0)
        if (SDL_GetAudioStreamQueued(stream) < (int)wav_data_len) {
            SDL_PutAudioStreamData(stream, wav_data, wav_data_len);
        }

        SDL_ResumeAudioStreamDevice(stream);
#else
        if(SDL_QueueAudio(*stream, wav_data, wav_data_len) < 0) {
            status = -1;
            fprintf(stderr, "Audio could not be queued: %s\n", SDL_GetError());
            goto main_cleanup;
        }

        SDL_PauseAudioDevice(*stream, 0);
#endif
    }
    while (ch_read) {
        refresh();

        for (uint32_t line = 0; line < vidtxt_info->print_lines; line++) {
            ch_read = fread(line_contents, sizeof(char), vidtxt_info->print_columns, vidtxt_info->fp);
            if (line < (uint16_t) (curr_term_lines))  {
                if (vidtxt_info->print_columns > curr_term_cols) {
                    chtype *ch_array = malloc((curr_term_cols)*sizeof(chtype));
                    for (uint32_t ch = 0; ch < curr_term_cols; ch++) {
                        ch_array[ch] = line_contents[ch] | A_NORMAL;
                    }
                    draw_successful = mvaddchnstr(line, 0, ch_array, vidtxt_info->print_columns);
                    free(ch_array);
                } else {
                    chtype *ch_array = malloc((vidtxt_info->print_columns)*sizeof(chtype));
                    for (uint32_t ch = 0; ch < vidtxt_info->print_columns; ch++) {
                        ch_array[ch] = line_contents[ch] | A_NORMAL;
                    }
                    draw_successful = mvaddchnstr(line, 0, ch_array, vidtxt_info->print_columns);
                    free(ch_array);
                }
                
            }
            if (draw_successful == ERR) {
                break;
            } else {
                draw_errors = 0;
                
            }
        }
        frame_num++;
        if (options->debug_mode) {
            char *prefix = malloc(curr_term_cols+1);
            double time_position = floor(frame_num / vidtxt_info->fps) + fmod(frame_num,  vidtxt_info->fps) / vidtxt_info->fps;
            int32_t prefix_size = snprintf(
                prefix, curr_term_cols+1,
                "[Frame: %lu, %02u:%02u:%06.3lf]", frame_num, 
                (uint32_t) floor(time_position / 3600), (uint32_t) floor(time_position / 60), fmod(time_position, 60)
            );
            char *suffix = malloc(curr_term_cols);
            int32_t suffix_size = snprintf(suffix, curr_term_cols, "[%02u:%02u:%06.3lf, %lu Frames, %lu%%]", 
                (uint32_t) floor(vidtxt_info->duration / 3600), (uint32_t) floor(vidtxt_info->duration / 60), fmod(vidtxt_info->duration, 60),
                vidtxt_info->total_frames, 100*frame_num / vidtxt_info->total_frames
            );
            char *full_bar = progress_bar(curr_term_cols-1, prefix, prefix_size, suffix, suffix_size, frame_num, vidtxt_info->total_frames);
            free(suffix);
            free(prefix);
            uint32_t full_bar_size;
            for (full_bar_size = 0; full_bar[full_bar_size] != '\0'; full_bar_size++);
            chtype *ch_array = malloc(full_bar_size*sizeof(chtype));
            uint32_t current_style = A_NORMAL;
            int32_t read_esc_seq = 0;
            char *ansi_esc_buffer = malloc(4);
            uint8_t aeb_size = 0;
            uint32_t offset = 0;
            for (uint32_t ch = 0; ch < full_bar_size; ch++) {
                if (full_bar[ch] == 0x1b) {
                    read_esc_seq = 1;
                    ch++;
                    offset+=2;
                    continue;
                }
                if (read_esc_seq) {
                    offset++;
                    if (full_bar[ch] == 'm') {
                        int32_t esc_code = atoi(ansi_esc_buffer);
                        aeb_size = 0;
                        if (esc_code == 7) {
                            current_style = A_STANDOUT;
                        }
                        if (esc_code == 0) {
                            current_style = A_NORMAL;
                        }
                        read_esc_seq = 0;
                        continue;
                    }
                    if (aeb_size < 4) {
                        ansi_esc_buffer[aeb_size] = full_bar[ch];
                        aeb_size++;
                    }
                    continue;
                }
                ch_array[ch-offset] = full_bar[ch] | current_style;
            }
            free(ansi_esc_buffer);
            free(full_bar);
            draw_successful = mvaddchnstr(curr_term_lines-1, 0, ch_array, vidtxt_info->print_columns);
            free(ch_array);
        }
        if (draw_successful == ERR) {
            draw_successful = 0;
            draw_errors++;
            continue;
        }
        if (draw_errors >= DRAW_ERROR_TOLERANCE) {
            status = -1;
            int_str_asprintf(&queued_err_msg, "Too many draw errors: errno %d: %s. Stopping...\n", errno, strerror(errno));
            goto main_cleanup;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &draw_spec) == ERR) {
            status = -1;
            int_str_asprintf(&queued_err_msg, "Couldn't get timestamp: errno %d: %s\n", errno, strerror(errno));
            goto main_cleanup;
        }
        uint64_t draw_time = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000 - pre_draw;
        if (draw_time < interval * 1000000) {
            int32_t sleep_interval = (int32_t) (interval * 1000000 - draw_time);
            pre_draw = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000 + sleep_interval;
            usleep(sleep_interval);
        } else {
            if (options->debug_mode && draw_time > interval) {
            }
            pre_draw = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000;
        }
    }
main_cleanup:
    free(line_contents);
    free(wav_data);
#if !SDL_VERSION_ATLEAST(3, 0, 0)
    if (stream) {
        SDL_CloseAudioDevice(*stream);
        free(stream);
    }
#endif
    av_free(wav_buffer);
    fclose(vidtxt_info->fp);
    free(vidtxt_info);
    if (curses_init) {
        echo();
        nocbreak();
        endwin();
    }
    if (queued_err_msg) {
        fprintf(stderr, "%s", queued_err_msg);
    }
    free(queued_err_msg);
    if (status < 0){
        return 1;
    }
    if (status >= 128) {
        return status;
    }
    return 0;
}

int32_t vidtxt_info(char *filename, VIDTTYOptions *options) {
    (void)(options);
    VIDTXTInfo *vidtxt_info = open_vidtxt(filename);
    if (vidtxt_info == NULL) {
        return 1;
    }

    printf(
        "\x1b[1mVIDTXT Video Information for %s:\x1b[0m\n"
        "Dimensions (columns x lines): %ux%u characters\n"
        "Framerate: %lf \n"
        "Total Frames: %lu \n"
        "Duration: %02u:%02u:%06.3lf \n"
        "Audio Size: %lu bytes\n", 
        filename, vidtxt_info->columns, vidtxt_info->lines, vidtxt_info->fps, vidtxt_info->total_frames, 
        (uint32_t) floor(vidtxt_info->duration / 3600), (uint32_t) floor(vidtxt_info->duration / 60), fmod(vidtxt_info->duration, 60),
        vidtxt_info->audio_size
    );
    fclose(vidtxt_info->fp);
    free(vidtxt_info);
    return 0;
}

int32_t dump_frames(char *filename, VIDTTYOptions *options) {
    char *output_filename;
    struct timespec draw_spec;
    uint64_t pre_duration;
    double numerator;
    double denominator;
    FILE *output_fp;
    int32_t status = 0;
    AVPacket *video_pkt = NULL;
    AVFrame *video_decoded = NULL;
    AVFrame *video_converted = NULL;
    AVCodecContext *vd_ctx = NULL;
    AVFormatContext *avfmt_ctx = NULL;
    struct SwsContext *sws_ctx = NULL;
    uint8_t *rgb_buffer = NULL;
    struct winsize term_size;

    AVPacket *audio_pkt = NULL;
    AVFrame *audio_decoded = NULL;
    SwrContext *swr_ctx = NULL;
    AVAudioFifo *fifo = NULL;
    AVCodecContext *encoder_ctx = NULL;
    AVCodecContext *ad_ctx = NULL;
    AVFormatContext *out_fmt_ctx = NULL;
    uint8_t *audio_buffer = NULL;
    int64_t samples_pts = 0;
    uint8_t **resampled_data = NULL;
    
    if (includes_match(filename, "://")) {
        output_filename = extract_filename_from_url(filename, 0, VIDTXT_EXT, sizeof(VIDTXT_EXT));
    } else {
        int32_t fname_size;
        for (fname_size = 0; filename[fname_size] != '\0'; fname_size++);
        output_filename = malloc(fname_size+sizeof(VIDTXT_EXT));
        int32_t prefix_size;
        for (prefix_size = 0; filename[prefix_size] != '.' && filename[prefix_size] != '\0'; prefix_size++) {
            output_filename[prefix_size] = filename[prefix_size];
        }
        for (uint64_t idx = 0; idx < sizeof(VIDTXT_EXT); idx++) {
            output_filename[prefix_size+idx] = VIDTXT_EXT[idx];
        }
    }
    if (!access(output_filename, F_OK)) {
        printf("A file called \x1b[1m%s\x1b[0m already exists\nOverwrite? [y/N]: ", output_filename);
        char prompt[6];
        char *input_successful = fgets(prompt, sizeof(prompt), stdin);
        if (!(input_successful && prompt[0] != '\n' && (prompt[0] | 32) == 'y')) {
            uint32_t duplicate_number = 1;
            char *tmp_buffer = NULL;
            uint32_t rep_size;
            for (uint32_t idx = 0; output_filename[idx] != '\0'; idx++) {
                if (output_filename[idx] == '.') {
                    output_filename[idx] = '\0';
                    break;
                }
            }
            while (1) {
                rep_size = snprintf(NULL, 0, "%s.%u%s", output_filename, duplicate_number, VIDTXT_EXT);
                tmp_buffer = malloc(rep_size+1);
                snprintf(tmp_buffer, rep_size+1, "%s.%u%s", output_filename, duplicate_number, VIDTXT_EXT);
                if (access(tmp_buffer, F_OK)) {
                    break;
                }
                free(tmp_buffer);
                duplicate_number++;
            }
            output_filename = realloc(output_filename, rep_size+1);
            snprintf(output_filename, rep_size+1, "%s", tmp_buffer);
            free(tmp_buffer);
        }
    }

    if (ioctl(1, TIOCGWINSZ, &term_size) == -1) {
        fprintf(stderr, "Could't get terminal size: ioctl error %d: %s\n", errno, strerror(errno));
        free(output_filename);
        return 1;
    }
    
    if (options->columns < 2) {
        options->columns = term_size.ws_col;
    }
    if (options->lines < 2) {
        options->lines = term_size.ws_row;
    }
    if (options->columns < 2 || options->lines < 2) {
        printf("Invalid terminal resolution! Must be 2x2 or greater");
        free(output_filename);
        return 1;
    }
    printf("Setting output resolution to %ux%u\n", options->columns, options->lines);
    printf("Writing to \x1b[1m%s\x1b[0m\n", output_filename);
    output_fp = fopen(output_filename, "wb");

    if (output_fp == NULL) {
        fprintf(stderr, "Couldn't open %s: %s\n", output_filename, strerror(errno));
        free(output_filename);
        return 1;  
    }

    char signature[] = "VIDTXT\0";
    fwrite(signature, sizeof(char), sizeof(signature), output_fp);
    uint32_t be_columns = htobe32(options->columns);
    uint32_t be_lines = htobe32(options->lines);
    fwrite(&be_columns, sizeof(be_columns), 1, output_fp);
    fwrite(&be_lines, sizeof(be_lines), 1, output_fp);

    avfmt_ctx = avformat_alloc_context();
    if ((status = avformat_open_input(&avfmt_ctx, filename, NULL, NULL)) < 0) {
        fprintf(stderr, "Could not read video file: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
        goto cleanup;
    }
    if ((status = avformat_find_stream_info(avfmt_ctx, NULL)) < 0) {
        fprintf(stderr, "Could not find stream information: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
        goto cleanup;
    }

    int32_t video_idx = av_find_best_stream(avfmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_idx < 0) {
        status = video_idx;
        fprintf(stderr, "Could not find video stream: FFmpeg error 0x%02x: %s\n", video_idx, av_err2str(video_idx));
        goto cleanup;
    }

    AVStream *video_stream = avfmt_ctx->streams[video_idx];

    AVRational r_frame_rate = video_stream->r_frame_rate;
    if (r_frame_rate.num <= 0 || r_frame_rate.den <= 0) {
        status = -1;
        fprintf(stderr, "Error getting frame rate!\n");
        goto cleanup;
    }
    double fps = av_q2d(r_frame_rate);
    uint64_t raw_fps;
    double be_fps;
    memmove(&raw_fps, &fps, sizeof(raw_fps));
    raw_fps = htobe64(raw_fps);
    memmove(&be_fps, &raw_fps, sizeof(be_fps));
    fwrite(&be_fps, sizeof(be_fps), 1, output_fp);

    uint64_t audio_size = 0;
    int32_t audio_idx = 0;
    if (!options->no_audio) {
        if ((audio_idx = av_find_best_stream(avfmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0)) < 0) {
            printf( "No audio stream found. Writing without audio...\n");
            options->no_audio = 1;
        }
    }
    if (!options->no_audio) {
        AVStream *audio_stream = avfmt_ctx->streams[audio_idx];
        #if LIBAVCODEC_VERSION_MAJOR >= 57
            const AVCodec *decoder = avcodec_find_decoder(audio_stream->codecpar->codec_id);
        #else
            VCodec *decoder = avcodec_find_decoder(audio_stream->codecpar->codec_id);
        #endif
        
        ad_ctx = avcodec_alloc_context3(decoder);
        avcodec_parameters_to_context(ad_ctx, audio_stream->codecpar);
        avcodec_open2(ad_ctx, decoder, NULL);

        if ((status = avformat_alloc_output_context2(&out_fmt_ctx, NULL, "mp3", NULL)) < 0) {
            fprintf(stderr, "Could not create output format context: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }

#if LIBAVCODEC_VERSION_MAJOR >= 57
        const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_MP3);
#else 
        AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_MP3);
#endif
        AVStream *output_audio_stream = avformat_new_stream(out_fmt_ctx, encoder);
        encoder_ctx = avcodec_alloc_context3(encoder);
        
       
        // output_audio_stream->codecpar->sample_rate = encoder_ctx->sample_rate;
        encoder_ctx->sample_rate = ad_ctx->sample_rate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        if (ad_ctx->ch_layout.nb_channels == 0) {
            // If decoder didn't fill it, synthesize from channels
            AVChannelLayout tmp;
            av_channel_layout_default(&tmp, ad_ctx->ch_layout.nb_channels);
            av_channel_layout_copy(&((AVCodecContext *)ad_ctx)->ch_layout, &tmp);
        }
        if ((status = av_channel_layout_copy(&encoder_ctx->ch_layout, &ad_ctx->ch_layout)) < 0) {
            fprintf(stderr, "Failed to copy channel layout: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }
#else 
        encoder_ctx->channel_layout = ad_ctx->channel_layout ?
                          ad_ctx->channel_layout :
                          av_get_default_channel_layout(ad_ctx->channels);
        encoder_ctx->channels = av_get_channel_layout_nb_channels(encoder_ctx->channel_layout);
#endif
        encoder_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        encoder_ctx->bit_rate = 192000;
        encoder_ctx->time_base = (AVRational){1, ad_ctx->sample_rate};

        if ((status = avcodec_open2(encoder_ctx, encoder, NULL)) < 0) {
            fprintf(stderr, "Could not open encoder: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }
        if ((status = avcodec_parameters_from_context(output_audio_stream->codecpar, encoder_ctx)) < 0) {
            fprintf(stderr, "Could not transfer codec paramaters: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }
        output_audio_stream->time_base = encoder_ctx->time_base;

        if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            if ((status = avio_open_dyn_buf(&out_fmt_ctx->pb)) < 0) {
                fprintf(stderr, "Could not create output buffer: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                goto cleanup;
            }
        }
    
        if ((status = avformat_write_header(out_fmt_ctx, NULL)) < 0) {
            fprintf(stderr, "Could not write header: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }

#if LIBAVUTIL_VERSION_MAJOR >= 57
        if (ad_ctx->ch_layout.nb_channels == 0) {
            AVChannelLayout tmp; av_channel_layout_default(&tmp, ad_ctx->ch_layout.nb_channels);
            av_channel_layout_copy(&ad_ctx->ch_layout, &tmp);
        }
        status = swr_alloc_set_opts2(
            &swr_ctx,
            &encoder_ctx->ch_layout, encoder_ctx->sample_fmt, encoder_ctx->sample_rate,
            &ad_ctx->ch_layout, ad_ctx->sample_fmt, ad_ctx->sample_rate,
            0, NULL
        );
#else
        if (!ad_ctx->channel_layout) {
            ad_ctx->channel_layout = av_get_default_channel_layout(ad_ctx->channels);
        }
        swr_ctx = swr_alloc_set_opts(
            NULL,
            encoder_ctx->channel_layout, encoder_ctx->sample_fmt, encoder_ctx->sample_rate,
            ad_ctx->channel_layout, ad_ctx->sample_fmt, ad_ctx->sample_rate,
            0, NULL
        );
        status = (swr_ctx == NULL) ? AVERROR(ENOMEM) : 0;
#endif
        if (status < 0) {
            fprintf(stderr, "Failed to allocate SwrContext: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }
        if ((status = swr_init(swr_ctx)) < 0) {
            fprintf(stderr, "Failed to initialize SwrContext: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }

#if LIBAVUTIL_VERSION_MAJOR >= 57
        fifo = av_audio_fifo_alloc(encoder_ctx->sample_fmt, encoder_ctx->ch_layout.nb_channels, 1024);
#else
        fifo = av_audio_fifo_alloc(encoder_ctx->sample_fmt, encoder_ctx->channels, 1024);
#endif

        audio_pkt = av_packet_alloc();
        audio_decoded = av_frame_alloc();

        int resampled_linesize = 0;
        int max_dst_nb_samples = 0;

        // Frame size we will enforce (1152 for MP3 at most rates; the encoder tells us)
        int enc_frame_size = encoder_ctx->frame_size;
        if (enc_frame_size <= 0) enc_frame_size = 1152; // conservative fallback

        // --- Demux/Decode/Resample/Buffer ---
        if (clock_gettime(CLOCK_MONOTONIC, &draw_spec) == ERR) {
            status = -1;
            fprintf(stderr, "Couldn't get timestamp: errno %d: %s\n", errno, strerror(errno));
            goto cleanup;
        }
        pre_duration = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000;
        numerator = 0;
        denominator = 1;
        uint64_t frame_count = 0;
        int64_t nb_frames = audio_stream->nb_frames;
        if (nb_frames <= 0) {
            int64_t decoder_frames = (int64_t)((double) avfmt_ctx->duration / 1000000 * audio_stream->codecpar->sample_rate + 0.5);
            fprintf(stderr, "Warning: No frame count metadata! Estimating from sample rate and frame size (this may be inaccurate)...\n");
            nb_frames = (decoder_frames + encoder_ctx->frame_size - 1) / encoder_ctx->frame_size;
        }
        printf("Writing Audio Frames...\r");
        fflush(stdout);
        while ((status = av_read_frame(avfmt_ctx, audio_pkt)) >= 0) {
            if (audio_pkt->stream_index != audio_idx) { av_packet_unref(audio_pkt); continue; }

            if ((status = avcodec_send_packet(ad_ctx, audio_pkt)) < 0) {
                av_packet_unref(audio_pkt);
                fprintf(stderr, "Error sending packet to decoder: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                goto cleanup;
            }
            av_packet_unref(audio_pkt);

            while ((status = avcodec_receive_frame(ad_ctx, audio_decoded)) >= 0) {
                int64_t delay = swr_get_delay(swr_ctx, ad_ctx->sample_rate);
                int dst_nb_samples = (int)av_rescale_rnd(delay + audio_decoded->nb_samples,
                                                        encoder_ctx->sample_rate,
                                                        ad_ctx->sample_rate,
                                                        AV_ROUND_UP);
                if (dst_nb_samples <= 0) dst_nb_samples = enc_frame_size;

                if (dst_nb_samples > max_dst_nb_samples) {
                    if (resampled_data) { av_freep(&resampled_data[0]); av_freep(&resampled_data); }
                    if ((
                            status = av_samples_alloc_array_and_samples(&resampled_data, &resampled_linesize,
#if LIBAVUTIL_VERSION_MAJOR >= 57
                            encoder_ctx->ch_layout.nb_channels
#else
                            encoder_ctx->channels
#endif
                            ,
                            dst_nb_samples, encoder_ctx->sample_fmt, 0)) < 0
                    ) {
                        fprintf(stderr, "Error allocating array and samples: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                        goto cleanup;
                    }
                    max_dst_nb_samples = dst_nb_samples;
                }

                int converted = swr_convert(swr_ctx,
                                            resampled_data, dst_nb_samples,
                                            (const uint8_t **)audio_decoded->data, audio_decoded->nb_samples);
                if (converted < 0) {
                    status = converted;
                    fprintf(stderr, "Error during resampling: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                    goto cleanup;
                }
                if ((status = av_audio_fifo_write(fifo, (void **)resampled_data, converted)) < 0) {
                    fprintf(stderr, "Error writing to audio fifo: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                    goto cleanup;
                }

                // While we have enough for a full encoder frame, encode it
                while (av_audio_fifo_size(fifo) >= enc_frame_size) {
                    AVFrame *audio_converted = av_frame_alloc();
                    if (!audio_converted) { 
                        status = AVERROR(ENOMEM); 
                        fprintf(stderr, "Error allocating conversion frames: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                        goto cleanup; 
                    }
                    audio_converted->nb_samples  = enc_frame_size;
                    audio_converted->format      = encoder_ctx->sample_fmt;
                    audio_converted->sample_rate = encoder_ctx->sample_rate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
                    audio_converted->ch_layout = encoder_ctx->ch_layout;
#else
                    audio_converted->channel_layout = encoder_ctx->channel_layout;
                    audio_converted->channels = encoder_ctx->channels;
#endif
                    if ((status = av_frame_get_buffer(audio_converted, 0)) < 0) {
                        fprintf(stderr, "Failed to allocate converted frame buffer: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                        av_frame_free(&audio_converted);
                        goto cleanup;
                    }

                    if ((status = av_audio_fifo_read(fifo, (void **)audio_converted->data, enc_frame_size)) < 0) { 
                        fprintf(stderr, "Error reading from audio fifo: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                        av_frame_free(&audio_converted); 
                        goto cleanup; 
                    }

                    // PTS in samples (time_base = 1/sample_rate)
                    audio_converted->pts = samples_pts;
                    samples_pts += enc_frame_size;
                    if ((status = avcodec_send_frame(encoder_ctx, audio_converted)) < 0) {
                        fprintf(stderr, "Error sending frame to encoder: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                        av_frame_free(&audio_converted);
                        goto cleanup;
                    }
                    av_frame_free(&audio_converted);

                    AVPacket *opkt = av_packet_alloc();
                    if (!opkt) { 
                        status = AVERROR(ENOMEM); 
                        fprintf(stderr, "Error allocating pkt: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                        goto cleanup; 
                    }
                    while ((status = avcodec_receive_packet(encoder_ctx, opkt)) >= 0) {
                        opkt->stream_index = output_audio_stream->index;
                        av_packet_rescale_ts(opkt, encoder_ctx->time_base, output_audio_stream->time_base);
                        if ((status = av_interleaved_write_frame(out_fmt_ctx, opkt)) < 0) {
                            fprintf(stderr, "Error writing audio frame: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                            av_packet_free(&opkt);
                            goto cleanup;
                        }
                        av_packet_unref(opkt);
                    }
                    // AVERROR(EAGAIN) or AVERROR_EOF is fine here
                    av_packet_free(&opkt);
                    if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) status = 0;
                }
                av_frame_unref(audio_decoded);
            }
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {status = 0;}
            frame_count++;
            if ((int64_t) frame_count > nb_frames) {
                    nb_frames = frame_count;
            }
            if (clock_gettime(CLOCK_MONOTONIC, &draw_spec) == ERR) {
                status = -1;
                fprintf(stderr, "Couldn't get timestamp: errno %d: %s\n", errno, strerror(errno));
                goto cleanup;
            }
            uint64_t frame_duration = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000 - pre_duration;
            pre_duration = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000;
            double rate = (double)1000000 / frame_duration;
            numerator+=rate;
            double time_left = (nb_frames-frame_count) / (numerator/denominator);
            if (frame_count % 64 == 0) {
                if (ioctl(1, TIOCGWINSZ, &term_size) == -1) {
                    fprintf(stderr, "Could't get terminal size: ioctl error %d: %s\n", errno, strerror(errno));
                    goto cleanup;
                }
                char *prefix = malloc(term_size.ws_col+1);
                int32_t prefix_size;
                int32_t suffix_size;
                char *suffix;
                char *full_bar;
                if (nb_frames > 0) {
                    prefix_size = snprintf(
                        prefix, term_size.ws_col+1,
                        "Writing Audio Frame: %lu/%ld Rate: %.1lf/s Time Left: %02u:%02u:%06.3lf", 
                        frame_count, nb_frames, numerator/denominator,
                        (uint32_t) floor(time_left / 3600), (uint32_t) floor(time_left / 60), fmod(time_left, 60)
                    );
                    suffix = malloc(SUFFIX_MAX_SIZE);
                    suffix_size = snprintf(suffix, SUFFIX_MAX_SIZE, "[ %lu%% ]", 100*(frame_count) / nb_frames);
                    full_bar = progress_bar(term_size.ws_col, prefix, prefix_size, suffix, suffix_size, frame_count, nb_frames);
                } else {
                    prefix_size = snprintf(
                        prefix, term_size.ws_col+1,
                        "Writing Audio Frame: %lu Rate: %.1lf/s", 
                        frame_count, numerator/denominator
                    );
                    suffix = malloc(SUFFIX_MAX_SIZE);
                    suffix_size = snprintf(suffix, SUFFIX_MAX_SIZE, "[ ???%% ]");
                    full_bar = progress_bar(term_size.ws_col, prefix, prefix_size, suffix, suffix_size, 0, 1);
                }
                free(suffix);
                free(prefix);
                printf("%s\r", full_bar);
                free(full_bar);
                fflush(stdout);
            }
            denominator++;
        }
        if (status == AVERROR_EOF) {status = 0;}
        if (status < 0) {
            fprintf(stderr, "Error converting frames: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }

        // Flush decoder
        if ((status = avcodec_send_packet(ad_ctx, NULL)) < 0) {
            fprintf(stderr, "Warning: Error sending packet to decoder: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }
        while ((status = avcodec_receive_frame(ad_ctx, audio_decoded)) >= 0) {
            int64_t delay = swr_get_delay(swr_ctx, ad_ctx->sample_rate);
            int dst_nb_samples = (int)av_rescale_rnd(delay + audio_decoded->nb_samples,
                                                    encoder_ctx->sample_rate,
                                                    ad_ctx->sample_rate, AV_ROUND_UP);
            if (dst_nb_samples <= 0) dst_nb_samples = enc_frame_size;

            if (dst_nb_samples > max_dst_nb_samples) {
                if (resampled_data) { av_freep(&resampled_data[0]); av_freep(&resampled_data); }
                if ((
                        status = av_samples_alloc_array_and_samples(&resampled_data, &resampled_linesize,
#if LIBAVUTIL_VERSION_MAJOR >= 57
                        encoder_ctx->ch_layout.nb_channels
#else
                        encoder_ctx->channels
#endif
                        ,
                        dst_nb_samples, encoder_ctx->sample_fmt, 0)) < 0
                    ) {
                    fprintf(stderr, "Error allocating array and samples: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                    goto cleanup;

                }
                max_dst_nb_samples = dst_nb_samples;
            }

            int converted = swr_convert(swr_ctx, resampled_data, dst_nb_samples,
                                        (const uint8_t **)audio_decoded->data, audio_decoded->nb_samples);
            if (converted < 0) { status = converted; goto cleanup; }
            if ((status = av_audio_fifo_write(fifo, (void **)resampled_data, converted)) < 0) {
                fprintf(stderr, "Error writing to audio fifo: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                goto cleanup;
            }
            av_frame_unref(audio_decoded);

            // Encode any complete frames now available
            while (av_audio_fifo_size(fifo) >= enc_frame_size) {
                AVFrame *audio_converted = av_frame_alloc();
                if (!audio_converted) { status = AVERROR(ENOMEM); goto cleanup; }
                audio_converted->nb_samples = enc_frame_size;
                audio_converted->format = encoder_ctx->sample_fmt;
                audio_converted->sample_rate = encoder_ctx->sample_rate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
                audio_converted->ch_layout = encoder_ctx->ch_layout;
#else
                audio_converted->channel_layout = encoder_ctx->channel_layout;
                audio_converted->channels = encoder_ctx->channels;
#endif
                if ((status = av_frame_get_buffer(audio_converted, 0)) < 0) {
                    fprintf(stderr, "Failed to allocate converted frame buffer: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                    goto cleanup;
                }
                if ((status = av_audio_fifo_read(fifo, (void **)audio_converted->data, enc_frame_size)) < 0) { av_frame_free(&audio_converted); goto cleanup; }
                audio_converted->pts = samples_pts; samples_pts += enc_frame_size;
                if ((status = avcodec_send_frame(encoder_ctx, audio_converted)) < 0) {
                    fprintf(stderr, "Error sending frame to encoder: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                    av_frame_free(&audio_converted);
                    goto cleanup;
                }
                av_frame_free(&audio_converted);

                AVPacket *opkt = av_packet_alloc();
                if (!opkt) { status = AVERROR(ENOMEM); goto cleanup; }
                while ((status = avcodec_receive_packet(encoder_ctx, opkt)) >= 0) {
                    opkt->stream_index = output_audio_stream->index;
                    av_packet_rescale_ts(opkt, encoder_ctx->time_base, output_audio_stream->time_base);
                    if ((status = av_interleaved_write_frame(out_fmt_ctx, opkt)) < 0) {
                        fprintf(stderr, "Error writing audio frame: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                        av_packet_free(&opkt);
                        goto cleanup;
                    }
                    av_packet_unref(opkt);
                }
                av_packet_free(&opkt);
                if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) status = 0;
            }
        }
        if (status == AVERROR_EOF || status == AVERROR(EAGAIN)) status = 0;

        // Flush the resampler (pull remaining delayed samples)
        for (;;) {
            int dst_nb_samples = enc_frame_size;
            if (dst_nb_samples > max_dst_nb_samples) {
                if (resampled_data) { av_freep(&resampled_data[0]); av_freep(&resampled_data); }
                if ((status = av_samples_alloc_array_and_samples(&resampled_data, &resampled_linesize,
#if LIBAVUTIL_VERSION_MAJOR >= 57
                        encoder_ctx->ch_layout.nb_channels
#else
                        encoder_ctx->channels
#endif
                        ,
                        dst_nb_samples, encoder_ctx->sample_fmt, 0)) < 0) goto cleanup;
                max_dst_nb_samples = dst_nb_samples;
            }
            int converted = swr_convert(swr_ctx, resampled_data, dst_nb_samples, NULL, 0);
            if (converted <= 0) break; // 0 means drained
            if ((status = av_audio_fifo_write(fifo, (void **)resampled_data, converted)) < 0) goto cleanup;

            while (av_audio_fifo_size(fifo) >= enc_frame_size) {
                AVFrame *audio_converted = av_frame_alloc();
                if (!audio_converted) { status = AVERROR(ENOMEM); goto cleanup; }
                audio_converted->nb_samples = enc_frame_size;
                audio_converted->format = encoder_ctx->sample_fmt;
                audio_converted->sample_rate = encoder_ctx->sample_rate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
                audio_converted->ch_layout = encoder_ctx->ch_layout;
#else
                audio_converted->channel_layout = encoder_ctx->channel_layout;
                audio_converted->channels = encoder_ctx->channels;
#endif
                if ((status = av_frame_get_buffer(audio_converted, 0)) < 0) {
                    fprintf(stderr, "Failed to allocate converted frame buffer: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                    goto cleanup;
                }
                if ((status = av_audio_fifo_read(fifo, (void **)audio_converted->data, enc_frame_size)) < 0) { av_frame_free(&audio_converted); goto cleanup; }
                audio_converted->pts = samples_pts; samples_pts += enc_frame_size;
                if ((status = avcodec_send_frame(encoder_ctx, audio_converted)) < 0) {
                    fprintf(stderr, "Error sending frame to encoder: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                    av_frame_free(&audio_converted);
                    goto cleanup;
                }
                av_frame_free(&audio_converted);

                AVPacket *opkt = av_packet_alloc();
                if (!opkt) { status = AVERROR(ENOMEM); goto cleanup; }
                while ((status = avcodec_receive_packet(encoder_ctx, opkt)) >= 0) {
                    opkt->stream_index = output_audio_stream->index;
                    av_packet_rescale_ts(opkt, encoder_ctx->time_base, output_audio_stream->time_base);
                    if ((status = av_interleaved_write_frame(out_fmt_ctx, opkt)) < 0) {
                        fprintf(stderr, "Error writing audio frame: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                        av_packet_free(&opkt);
                        goto cleanup;
                    }
                    av_packet_unref(opkt);
                }
                av_packet_free(&opkt);
                if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) status = 0;
            }
        }

        // Drain FIFO: if leftover < frame_size, pad with silence so the final frame is full-size
        {
            int leftover = av_audio_fifo_size(fifo);
            if (leftover > 0) {
                if (leftover < enc_frame_size) {
                    int to_pad = enc_frame_size - leftover;
                    uint8_t **silence = NULL;
                    int linesize = 0;
                    status = av_samples_alloc_array_and_samples(
                        &silence, &linesize,
#if LIBAVUTIL_VERSION_MAJOR >= 57
                        encoder_ctx->ch_layout.nb_channels
#else
                        encoder_ctx->channels
#endif
                        , 
                        to_pad, encoder_ctx->sample_fmt, 0
                    );
                    if (status < 0) goto cleanup;
                    av_samples_set_silence(
                        silence, 0, to_pad, 
#if LIBAVUTIL_VERSION_MAJOR >= 57
                        encoder_ctx->ch_layout.nb_channels
#else
                        encoder_ctx->channels
#endif
                        , 
                        encoder_ctx->sample_fmt
                    );
                    status = av_audio_fifo_write(fifo, (void **)silence, to_pad);
                    av_freep(&silence[0]);
                    av_freep(&silence);
                    if (status < 0) goto cleanup;
                }
                // Now exactly one (or more) full-size frames remain
                while (av_audio_fifo_size(fifo) >= enc_frame_size) {
                    AVFrame *audio_converted = av_frame_alloc();
                    if (!audio_converted) { status = AVERROR(ENOMEM); goto cleanup; }
                    audio_converted->nb_samples = enc_frame_size;
                    audio_converted->format = encoder_ctx->sample_fmt;
                    audio_converted->sample_rate = encoder_ctx->sample_rate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
                    audio_converted->ch_layout = encoder_ctx->ch_layout;
#else
                    audio_converted->channel_layout = encoder_ctx->channel_layout;
                    audio_converted->channels = encoder_ctx->channels;
#endif
                    if ((status = av_frame_get_buffer(audio_converted, 0)) < 0) {
                        fprintf(stderr, "Failed to allocate converted frame buffer: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                        goto cleanup;
                    }
                    if ((status = av_audio_fifo_read(fifo, (void **)audio_converted->data, enc_frame_size)) < 0) { av_frame_free(&audio_converted); goto cleanup; }
                    audio_converted->pts = samples_pts; samples_pts += enc_frame_size;
                    if ((status = avcodec_send_frame(encoder_ctx, audio_converted)) < 0) {
                        fprintf(stderr, "Error sending frame to encoder: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                        av_frame_free(&audio_converted);
                        goto cleanup;
                    }
                    av_frame_free(&audio_converted);

                    AVPacket *opkt = av_packet_alloc();
                    if (!opkt) { status = AVERROR(ENOMEM); goto cleanup; }
                    while ((status = avcodec_receive_packet(encoder_ctx, opkt)) >= 0) {
                        opkt->stream_index = output_audio_stream->index;
                        av_packet_rescale_ts(opkt, encoder_ctx->time_base, output_audio_stream->time_base);
                        if ((status = av_interleaved_write_frame(out_fmt_ctx, opkt)) < 0) {
                            fprintf(stderr, "Error writing audio frame: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                            av_packet_free(&opkt);
                            goto cleanup;
                        }
                        av_packet_unref(opkt);
                    }
                    av_packet_free(&opkt);
                    if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) status = 0;
                }
            }
        }

        // Flush encoder
        if ((status = avcodec_send_frame(encoder_ctx, NULL)) < 0) {
                        fprintf(stderr, "Error sending frame to encoder: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }
        {
            AVPacket *opkt = av_packet_alloc();
            if (!opkt) { status = AVERROR(ENOMEM); goto cleanup; }
            while ((status = avcodec_receive_packet(encoder_ctx, opkt)) >= 0) {
                opkt->stream_index = output_audio_stream->index;
                av_packet_rescale_ts(opkt, encoder_ctx->time_base, output_audio_stream->time_base);
                if ((status = av_interleaved_write_frame(out_fmt_ctx, opkt)) < 0) {
                    fprintf(stderr, "Error writing audio frame: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                    av_packet_free(&opkt);
                    goto cleanup;
                }
                av_packet_unref(opkt);
            }
            av_packet_free(&opkt);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) status = 0;
        }
        if (ioctl(1, TIOCGWINSZ, &term_size) == -1) {
            fprintf(stderr, "Could't get terminal size: ioctl error %d: %s\n", errno, strerror(errno));
            goto cleanup;
        }
        char *prefix = malloc(term_size.ws_col+9);
        if (nb_frames <= 0) {
            nb_frames = frame_count;
        }
        int32_t amount = snprintf(
            prefix, term_size.ws_col+1,
            "\x1b[7mWriting Audio Frame: %lu/%ld Rate: %.1lf/s Time Left: %02u:%02u:%06.3lf", 
            frame_count, nb_frames, numerator/denominator,
            0, 0, 0.0
        );
        for (int32_t idx = amount; idx < term_size.ws_col+9; idx++) {
            prefix[idx] = ' ';
        }
        char *suffix = malloc(SUFFIX_MAX_SIZE+4);
        int32_t suffix_size = snprintf(suffix, SUFFIX_MAX_SIZE+4, "[ %lu%% ]\x1b[0m", 100*(frame_count) / nb_frames);
        for (int32_t idx = 0; idx < suffix_size; idx++) {
            prefix[term_size.ws_col+8-suffix_size+idx] = suffix[idx];
        }
        free(suffix);
        prefix[term_size.ws_col+8] = '\0';
        printf("%s\n", prefix);
        free(prefix);

        if ((status = av_write_trailer(out_fmt_ctx)) < 0) {
            fprintf(stderr, "Error writing trailer: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }

        audio_size = avio_close_dyn_buf(out_fmt_ctx->pb, &audio_buffer);
    }
    
    uint64_t be_audio_size = htobe64(audio_size);
    fwrite(&be_audio_size, sizeof(be_audio_size), 1, output_fp);;

    for (int32_t idx = 0; idx < 32; idx++) {
        if ((status = fputc('\0', output_fp)) < 0) {
            fprintf(stderr, "Error writing null byte %d to header: fputc error %d: %s\n", idx, errno, strerror(errno));
            goto cleanup;
        }
    }

    if (!options->no_audio) {
        fwrite(audio_buffer, sizeof(uint8_t), audio_size, output_fp);
    }

    const AVCodec *decoder = avcodec_find_decoder(video_stream->codecpar->codec_id);
    vd_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(vd_ctx, video_stream->codecpar);
    avcodec_open2(vd_ctx, decoder, NULL);

    sws_ctx = sws_getContext(
        vd_ctx->width, vd_ctx->height, vd_ctx->pix_fmt,
        options->columns-1, options->lines-1, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, NULL, NULL, NULL);

    if (!sws_ctx) {
        fprintf(stderr, "Failed to create sws context!\n");
        status = AVERROR(EINVAL);
        goto cleanup;
    }

    video_pkt = av_packet_alloc();
    video_decoded = av_frame_alloc();
    video_converted = av_frame_alloc();

    int buffer_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, options->columns-1, options->lines-1, 1);
    rgb_buffer = av_malloc(buffer_size);
    av_image_fill_arrays(video_converted->data, video_converted->linesize, rgb_buffer, AV_PIX_FMT_RGB24,
                         options->columns-1, options->lines-1, 1);

    static const char ascii_gradients[] = " .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";
    const uint64_t ascii_size = sizeof(ascii_gradients) - 1;
    uint64_t frame_count = 0;
    printf("Writing Video Frames...\r");
    fflush(stdout);
    int32_t loop_status = 0;
    av_seek_frame(avfmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);
    if (clock_gettime(CLOCK_MONOTONIC, &draw_spec) == ERR) {
        status = -1;
        fprintf(stderr, "Couldn't get timestamp: errno %d: %s\n", errno, strerror(errno));
        goto cleanup;
    }
    pre_duration = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000;
    numerator = 0;
    denominator = 1;
    int64_t nb_frames = video_stream->nb_frames;
    if (nb_frames <= 0) {
        fprintf(stderr, "Warning: No frame count metadata! Estimating from duration and fps...\n");
        nb_frames = (int64_t)((double) avfmt_ctx->duration / 1000000 * fps + 0.5);
    }
    while (av_read_frame(avfmt_ctx, video_pkt) >= 0) {
        if (video_pkt->stream_index == video_idx) {
            if ((loop_status = avcodec_send_packet(vd_ctx, video_pkt)) < 0) {
                fprintf(stderr, "Failed to send packet: %s\n", av_err2str(status));
                break;
            }

            while ((loop_status = avcodec_receive_frame(vd_ctx, video_decoded)) >= 0) {
                sws_scale(sws_ctx, (const uint8_t *const *)video_decoded->data, video_decoded->linesize,
                          0, vd_ctx->height, video_converted->data, video_converted->linesize);
                char *ascii_fb = malloc(buffer_size / 3);
                int32_t ascii_fb_size = 0;
                for (int32_t idx = 0; idx+2 < buffer_size; idx+=3) {
                    uint8_t gradient = 0.299 * rgb_buffer[idx] + 0.587 * rgb_buffer[idx+1] + 0.114 * rgb_buffer[idx+2];
                    uint64_t gradient_idx = (gradient * (ascii_size - 1)) / 255;
                    if (gradient_idx > ascii_size) {
                        fprintf(stderr, "Fatal: index greater than gradient list. Aborting to prevent oob array access...");
                        status = -1;
                        goto cleanup;
                    }
                    if (ascii_fb_size >= buffer_size / 3) {
                        fprintf(stderr, "Fatal: ascii_fb_size greater than what was calculated. Aborting to prevent oob array access...");
                        status = -1;
                        goto cleanup;
                    }
                    ascii_fb[ascii_fb_size] = ascii_gradients[gradient_idx];
                    ascii_fb_size++;
                }
                fwrite(ascii_fb, sizeof(char), ascii_fb_size, output_fp);
                free(ascii_fb);
                frame_count++;
                if ((int64_t) frame_count > nb_frames) {
                    nb_frames = frame_count;
                }
                if (clock_gettime(CLOCK_MONOTONIC, &draw_spec) == ERR) {
                    status = -1;
                    fprintf(stderr, "Couldn't get timestamp: errno %d: %s\n", errno, strerror(errno));
                    goto cleanup;
                }
                uint64_t frame_duration = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000 - pre_duration;
                pre_duration = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000;
                double rate = (double)1000000 / frame_duration;
                numerator+=rate;
                double time_left = (nb_frames-frame_count-1) / (numerator/denominator);
                if (ioctl(1, TIOCGWINSZ, &term_size) == -1) {
                    fprintf(stderr, "Could't get terminal size: ioctl error %d: %s\n", errno, strerror(errno));
                    goto cleanup;
                }
                char *prefix = malloc(term_size.ws_col+1);
                int32_t prefix_size;
                int32_t suffix_size;
                char *suffix;
                char *full_bar;
                if (nb_frames > 0) {
                    prefix_size = snprintf(
                        prefix, term_size.ws_col+1,
                        "Writing Video Frame: %lu/%ld Rate: %.1lf/s Time Left: %02u:%02u:%06.3lf", 
                        frame_count+1, nb_frames, numerator/denominator,
                        (uint32_t) floor(time_left / 3600), (uint32_t) floor(time_left / 60), fmod(time_left, 60)
                    );
                    suffix = malloc(SUFFIX_MAX_SIZE);
                    suffix_size = snprintf(suffix, SUFFIX_MAX_SIZE, "[ %lu%% ]", 100*(frame_count+1) / nb_frames);
                    full_bar = progress_bar(term_size.ws_col, prefix, prefix_size, suffix, suffix_size, frame_count+1, nb_frames);
                } else {
                    prefix_size = snprintf(
                        prefix, term_size.ws_col+1,
                        "Writing Video Frame: %lu Rate: %.1lf/s", 
                        frame_count+1, numerator/denominator
                    );
                    suffix = malloc(SUFFIX_MAX_SIZE);
                    suffix_size = snprintf(suffix, SUFFIX_MAX_SIZE, "[ ???%% ]");
                    full_bar = progress_bar(term_size.ws_col, prefix, prefix_size, suffix, suffix_size, 0, 1);
                }
                free(suffix);
                free(prefix);
                printf("%s\r", full_bar);
                free(full_bar);
                denominator++;
                fflush(stdout);
            }
        }
        av_packet_unref(video_pkt);
    }
    printf("\n");

    
cleanup:
    sws_freeContext(sws_ctx);
    av_packet_free(&audio_pkt);
    av_packet_free(&video_pkt);
    av_frame_free(&audio_decoded);  
    av_frame_free(&video_decoded);   
    if (status < 0 && out_fmt_ctx) {
        audio_size = avio_close_dyn_buf(out_fmt_ctx->pb, &audio_buffer);
    }
    avcodec_free_context(&encoder_ctx);
    avformat_free_context(out_fmt_ctx);
    av_frame_free(&video_converted);
    if (resampled_data) { 
        av_freep(&resampled_data[0]); 
        av_freep(&resampled_data); 
    }
    av_audio_fifo_free(fifo);
    swr_free(&swr_ctx);
    avcodec_free_context(&ad_ctx);
    avcodec_free_context(&vd_ctx);
    avformat_close_input(&avfmt_ctx);
    av_free(rgb_buffer);
    av_free(audio_buffer);
    fclose(output_fp);
    free(output_filename);
    if (status < 0) {
        return 1;
    }
    return 0;
}

int32_t render_frames(char *filename, VIDTTYOptions *options) {
    struct timespec draw_spec;
    int32_t status = 0;
    AVPacket *video_pkt = NULL;
    AVFrame *video_decoded = NULL;
    AVFrame *video_converted = NULL;
    AVCodecContext *vd_ctx = NULL;
    AVFormatContext *avfmt_ctx = NULL;
    struct SwsContext *sws_ctx = NULL;
    uint8_t *rgb_buffer = NULL;
    struct winsize term_size;
    int32_t curses_init = 0;
    char *queued_err_msg = NULL;

    AVPacket *audio_pkt = NULL;
    AVFrame *audio_decoded = NULL;
    SwrContext *swr_ctx = NULL;
    AVAudioFifo *fifo = NULL;
    AVCodecContext *encoder_ctx = NULL;
    AVCodecContext *ad_ctx = NULL;
    AVFormatContext *out_fmt_ctx = NULL;
    uint8_t *audio_buffer = NULL;
    uint8_t **resampled_data = NULL;
    AVIOContext *out_avio_ctx = NULL;
    AVFrame *audio_converted = NULL;
    uint8_t *wav_data = NULL;
    uint32_t wav_data_len = 0;
    uint64_t audio_size = 0;
    uint64_t pre_duration;
    double numerator;
    double denominator;
#if SDL_VERSION_ATLEAST(3, 0, 0)
    SDL_AudioStream *stream = NULL;
#else
    SDL_AudioDeviceID *stream = NULL;
#endif

    avfmt_ctx = avformat_alloc_context();
    if ((status = avformat_open_input(&avfmt_ctx, filename, NULL, NULL)) < 0) {
        fprintf(stderr, "Could not read video file: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
        goto cleanup;
    }
    if ((status = avformat_find_stream_info(avfmt_ctx, NULL)) < 0) {
        fprintf(stderr, "Could not find stream information: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
        goto cleanup;
    }

    int32_t video_idx = av_find_best_stream(avfmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_idx < 0) {
        status = video_idx;
        fprintf(stderr, "Could not find video stream: FFmpeg error 0x%02x: %s\n", video_idx, av_err2str(video_idx));
        goto cleanup;
    }

    AVStream *video_stream = avfmt_ctx->streams[video_idx];

    AVRational r_frame_rate = video_stream->r_frame_rate;
    if (r_frame_rate.num <= 0 || r_frame_rate.den <= 0) {
        status = -1;
        fprintf(stderr, "Error getting frame rate!\n");
        goto cleanup;
    }
    double fps = av_q2d(r_frame_rate);
    double interval = av_q2d(av_inv_q(r_frame_rate));

    int32_t audio_idx = 0;
    if (!options->no_audio) {
        if ((audio_idx = av_find_best_stream(avfmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0)) < 0) {
            printf( "No audio stream found. Writing without audio...\n");
            options->no_audio = 1;
        }
    }
    if (!options->no_audio) {
        AVStream *audio_stream = avfmt_ctx->streams[audio_idx];
        const AVCodec *decoder = avcodec_find_decoder(audio_stream->codecpar->codec_id);
        ad_ctx = avcodec_alloc_context3(decoder);
        avcodec_parameters_to_context(ad_ctx, audio_stream->codecpar);
        avcodec_open2(ad_ctx, decoder, NULL);

        if ((status = avformat_alloc_output_context2(&out_fmt_ctx, NULL, "wav", NULL)) < 0) {
            fprintf(stderr, "Could not create output format context: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }

        if ((status = avio_open_dyn_buf(&out_avio_ctx)) < 0) {
            fprintf(stderr, "Could not create output buffer: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }

        out_fmt_ctx->pb = out_avio_ctx;

        const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
        AVStream *output_audio_stream = avformat_new_stream(out_fmt_ctx, encoder);
        encoder_ctx = avcodec_alloc_context3(encoder);
        encoder_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
        encoder_ctx->sample_rate = ad_ctx->sample_rate;
        encoder_ctx->time_base = (AVRational){1, ad_ctx->sample_rate};
#if LIBAVUTIL_VERSION_MAJOR >= 57
        if ((status = av_channel_layout_copy(&encoder_ctx->ch_layout, &ad_ctx->ch_layout)) < 0) {
            fprintf(stderr, "Failed to copy channel layout: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }
#else 
        encoder_ctx->channel_layout = decoder_ctx->channel_layout;
        encoder_ctx->channels = decoder_ctx->channels;
#endif
        if ((status = avcodec_open2(encoder_ctx, encoder, NULL)) < 0) {
            fprintf(stderr, "Could not open encoder: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }
        if ((status = avcodec_parameters_from_context(output_audio_stream->codecpar, encoder_ctx)) < 0) {
            fprintf(stderr, "Could not transfer codec paramaters: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }
        if ((status = avformat_write_header(out_fmt_ctx, NULL)) < 0) {
            fprintf(stderr, "Could not write header: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }

#if LIBAVUTIL_VERSION_MAJOR >= 57
        status = swr_alloc_set_opts2(
            &swr_ctx,
            &encoder_ctx->ch_layout, encoder_ctx->sample_fmt, encoder_ctx->sample_rate,
            &ad_ctx->ch_layout, ad_ctx->sample_fmt, ad_ctx->sample_rate,
            0, NULL
        );
#else
        swr_ctx = swr_alloc_set_opts(
            NULL,
            encoder_ctx->channel_layout, encoder_ctx->sample_fmt, encoder_ctx->sample_rate,
            decoder_ctx->channel_layout, decoder_ctx->sample_fmt, decoder_ctx->sample_rate,
            0, NULL
        );
        status = (swr_ctx == NULL) ? AVERROR(ENOMEM) : 0;
#endif
        if (status < 0) {
            fprintf(stderr, "Failed to allocate SwrContext: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }
        if ((status = swr_init(swr_ctx)) < 0) {
            fprintf(stderr, "Failed to initialize SwrContext: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }

        audio_pkt = av_packet_alloc();
        audio_decoded = av_frame_alloc();
        audio_converted = av_frame_alloc();
        audio_converted->format = encoder_ctx->sample_fmt;
        audio_converted->sample_rate = encoder_ctx->sample_rate;

#if LIBAVUTIL_VERSION_MAJOR >= 57
        if ((status = av_channel_layout_copy(&audio_converted->ch_layout, &encoder_ctx->ch_layout)) < 0) {
            fprintf(stderr, "Failed to copy channel layout: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }
#else 
        converted->channel_layout = encoder_ctx->channel_layout;
        converted->channels = encoder_ctx->channels;
#endif
        if (clock_gettime(CLOCK_MONOTONIC, &draw_spec) == ERR) {
            status = -1;
            fprintf(stderr, "Couldn't get timestamp: errno %d: %s\n", errno, strerror(errno));
            goto cleanup;
        }
        pre_duration = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000;
        numerator = 0;
        denominator = 1;
        int64_t next_pts = 0;
        uint64_t frame_count = 0;
        int64_t nb_frames = audio_stream->nb_frames;
        if (nb_frames <= 0) {
            fprintf(stderr, "Warning: No frame count metadata! Estimating from bitrate, sample rate and frame size (this may be inaccurate)...\n");
            double total_samples = (double)avfmt_ctx->duration / AV_TIME_BASE * audio_stream->codecpar->sample_rate;
            nb_frames = floor(total_samples / audio_stream->codecpar->frame_size)-1;
        }
        printf("Writing Audio Frames...\r");
        fflush(stdout);
        while ((status = av_read_frame(avfmt_ctx, audio_pkt)) >= 0) {
            if (audio_pkt->stream_index != audio_idx) {
                av_packet_unref(audio_pkt);
                continue;
            }

            if ((status = avcodec_send_packet(ad_ctx, audio_pkt)) < 0) {
                fprintf(stderr, "Warning: Error sending packet to decoder: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                break;
            }
            while ((status = avcodec_receive_frame(ad_ctx, audio_decoded)) == 0) {
                av_frame_unref(audio_converted);  
                audio_converted->nb_samples = audio_decoded->nb_samples;
                
                audio_converted->format       = encoder_ctx->sample_fmt;
                audio_converted->sample_rate  = encoder_ctx->sample_rate;

#if LIBAVUTIL_VERSION_MAJOR >= 57
                if ((status = av_channel_layout_copy(&audio_converted->ch_layout, &encoder_ctx->ch_layout)) < 0) {
                    fprintf(stderr, "Failed to copy channel layout: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                    goto cleanup;
                }
#else 
                converted->channel_layout = encoder_ctx->channel_layout;
                converted->channels = encoder_ctx->channels;
#endif
                
                if ((status = av_frame_get_buffer(audio_converted, 0)) < 0) {
                    fprintf(stderr, "Failed to allocate converted frame buffer: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                    goto cleanup;
                }

                int out_samples = swr_convert(
                    swr_ctx,
                    audio_converted->data, audio_converted->nb_samples,
                    (const uint8_t **)audio_decoded->data, audio_decoded->nb_samples);

                if (out_samples < 0) {
                    status = out_samples;
                    fprintf(stderr, "Error during resampling: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                    goto cleanup;
                }

                audio_converted->pts = next_pts;
                next_pts += out_samples;

                if ((status = avcodec_send_frame(encoder_ctx, audio_converted)) < 0) {
                    fprintf(stderr, "Error sending frame to encoder: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                    goto cleanup;
                }

                while ((status = avcodec_receive_packet(encoder_ctx, audio_pkt)) == 0) {
                    audio_pkt->stream_index = output_audio_stream->index;
                    if ((status = av_interleaved_write_frame(out_fmt_ctx, audio_pkt)) < 0) {
                        fprintf(stderr, "Error writing audio frame: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                        goto cleanup;
                    }
                    av_packet_unref(audio_pkt);
                }

                av_frame_unref(audio_decoded);
                av_frame_unref(audio_converted);
            }

            av_packet_unref(audio_pkt);
            frame_count++;
            if ((int64_t) frame_count > nb_frames) {
                    nb_frames = frame_count;
                }
            if (clock_gettime(CLOCK_MONOTONIC, &draw_spec) == ERR) {
                status = -1;
                fprintf(stderr, "Couldn't get timestamp: errno %d: %s\n", errno, strerror(errno));
                goto cleanup;
            }
            uint64_t frame_duration = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000 - pre_duration;
            pre_duration = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000;
            double rate = (double)1000000 / frame_duration;
            numerator+=rate;
            double time_left = (nb_frames-frame_count) / (numerator/denominator);
            if (frame_count % 64 == 0) {
                if (ioctl(1, TIOCGWINSZ, &term_size) == -1) {
                    fprintf(stderr, "Could't get terminal size: ioctl error %d: %s\n", errno, strerror(errno));
                    goto cleanup;
                }
                char *prefix = malloc(term_size.ws_col+1);
                int32_t prefix_size;
                int32_t suffix_size;
                char *suffix;
                char *full_bar;
                if (nb_frames > 0) {
                    prefix_size = snprintf(
                        prefix, term_size.ws_col+1,
                        "Writing Audio Frame: %lu/%ld Rate: %.1lf/s Time Left: %02u:%02u:%06.3lf", 
                        frame_count, nb_frames, numerator/denominator,
                        (uint32_t) floor(time_left / 3600), (uint32_t) floor(time_left / 60), fmod(time_left, 60)
                    );
                    suffix = malloc(SUFFIX_MAX_SIZE);
                    suffix_size = snprintf(suffix, SUFFIX_MAX_SIZE, "[ %lu%% ]", 100*(frame_count) / nb_frames);
                    full_bar = progress_bar(term_size.ws_col, prefix, prefix_size, suffix, suffix_size, frame_count, nb_frames);
                } else {
                    prefix_size = snprintf(
                        prefix, term_size.ws_col+1,
                        "Writing Audio Frame: %lu Rate: %.1lf/s", 
                        frame_count, numerator/denominator
                    );
                    suffix = malloc(SUFFIX_MAX_SIZE);
                    suffix_size = snprintf(suffix, SUFFIX_MAX_SIZE, "[ ???%% ]");
                    full_bar = progress_bar(term_size.ws_col, prefix, prefix_size, suffix, suffix_size, 0, 1);
                }
                free(suffix);
                free(prefix);
                printf("%s\r", full_bar);
                free(full_bar);
                fflush(stdout);
            }
            denominator++;
        }
        if (ioctl(1, TIOCGWINSZ, &term_size) == -1) {
            fprintf(stderr, "Could't get terminal size: ioctl error %d: %s\n", errno, strerror(errno));
            goto cleanup;
        }
        char *prefix = malloc(term_size.ws_col+9);
        if (nb_frames <= 0) {
            nb_frames = frame_count;
        }
        int32_t amount = snprintf(
            prefix, term_size.ws_col+1,
            "\x1b[7mWriting Audio Frame: %lu/%ld Rate: %.1lf/s Time Left: %02u:%02u:%06.3lf", 
            frame_count, nb_frames, numerator/denominator,
            0, 0, 0.0
        );
        for (int32_t idx = amount; idx < term_size.ws_col+9; idx++) {
            prefix[idx] = ' ';
        }
        char *suffix = malloc(SUFFIX_MAX_SIZE+4);
        int32_t suffix_size = snprintf(suffix, SUFFIX_MAX_SIZE+4, "[ %lu%% ]\x1b[0m", 100*(frame_count) / nb_frames);
        for (int32_t idx = 0; idx < suffix_size; idx++) {
            prefix[term_size.ws_col+8-suffix_size+idx] = suffix[idx];
        }
        free(suffix);
        prefix[term_size.ws_col+8] = '\0';
        printf("%s\n", prefix);
        free(prefix);

        if ((status = av_write_trailer(out_fmt_ctx)) < 0) {
            fprintf(stderr, "Error writing trailer: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto cleanup;
        }

        audio_size = avio_close_dyn_buf(out_avio_ctx, &audio_buffer);
        SDL_AudioSpec spec;
        
#if SDL_VERSION_ATLEAST(3, 0, 0)
        if (!SDL_SetAppMetadata(PROGRAM_NAME, VERSION, PROGRAM_NAME)) {
            status = -1;
            fprintf(stderr, "Error setting mixer metadata: %s\n", SDL_GetError());
            goto cleanup;
        }
        if (!SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_TYPE_STRING, "mediaplayer")) {
            status = -1;
            fprintf(stderr, "Error setting mixer metadata: %s\n", SDL_GetError());
            goto cleanup;
        }

#endif

        // SDL takes over signal handling for SIGINT and SIGTERM. We dont't want that so we change it back.

        // save current handlers
        struct sigaction int_action, term_action;
        sigaction(SIGINT, NULL, &int_action);
        sigaction(SIGTERM, NULL, &term_action);
#if SDL_VERSION_ATLEAST(3, 0, 0)
        if (!SDL_Init(SDL_INIT_AUDIO)) {
#else 
        if (SDL_Init(SDL_INIT_AUDIO) < 0 ) {
#endif
            status = -1;
            fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
            goto cleanup;
        }
        
        // set the saved hanlers back
        sigaction(SIGINT, &int_action, NULL);
        sigaction(SIGTERM, &term_action, NULL);

#if SDL_VERSION_ATLEAST(3, 0, 0)
        SDL_IOStream *wav_stream = SDL_IOFromMem(audio_buffer, audio_size);
        int32_t load_result = SDL_LoadWAV_IO(wav_stream, 1, &spec, &wav_data, &wav_data_len);
#else
        SDL_RWops *wav_stream = SDL_RWFromMem(wav_buffer, wav_size);
        SDL_AudioSpec *spec_result;
        spec_result = SDL_LoadWAV_RW(wav_stream, 1, &spec, &wav_data, &wav_data_len);
        int32_t load_result = (spec_result != NULL);
        spec = *spec_result;
#endif
        if (!load_result) {
            status = -1;
            fprintf(stderr, "Couldn't load .wav file: %s\n", SDL_GetError());
            goto cleanup;
        }

#if SDL_VERSION_ATLEAST(3, 0, 0)
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
#else
        stream = malloc(sizeof(SDL_AudioDeviceID));
        SDL_AudioDeviceID device_id = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
        if (device_id) {
            *stream = device_id;
        }
#endif
        if (!stream) {
            status = -1;
            fprintf(stderr, "Couldn't create audio stream: %s\n", SDL_GetError());
            goto cleanup;
        }
    }

    const AVCodec *decoder = avcodec_find_decoder(video_stream->codecpar->codec_id);
    vd_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(vd_ctx, video_stream->codecpar);
    avcodec_open2(vd_ctx, decoder, NULL);

    static const char ascii_gradients[] = " .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";
    const uint64_t ascii_size = sizeof(ascii_gradients) - 1;
    uint64_t frame_count = 0;
    int32_t loop_status = 0;
    av_seek_frame(avfmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);

    int32_t curses_fd = 1;
    char *curses_term = getenv("TERM");
    FILE *curses_stdin = stdin;
    FILE *curses_stdout = stdout;
    if (options->tty) {
        curses_stdin = fopen(options->tty, "r+"); 
        curses_stdout = fopen(options->tty, "w+");
        if (!curses_stdin || !curses_stdout) {
            if (errno == EACCES) {
                printf("Need permission to write to \x1b[1m%s\x1b[0m\nRunning sudo...\n", options->tty);
                uint64_t max_cb_size = snprintf(NULL, 0, "sudo chown %u %s", UINT32_MAX, options->tty);
                char *chown_buffer = malloc(max_cb_size+1);
                int32_t buffer_written = snprintf(chown_buffer, max_cb_size+1, "sudo chown %u %s", getuid(), options->tty);
                if (buffer_written <= 0) {
                    status = -1;
                    fprintf(stderr, "Error concentrating chown command\n");
                    goto cleanup;
                }
                int32_t chown_status = system(chown_buffer);
                free(chown_buffer);
                int32_t chown_failed = WEXITSTATUS(chown_status);
                if (WIFSIGNALED(chown_status)) {
                    if (WTERMSIG(chown_status) == 2) {
                        fprintf(stderr, "Sudo aborted by user\n");
                    } else {
                        fprintf(
                            stderr, "Sudo exited due to signal %d: %s\n", 
                            WTERMSIG(chown_status), strsignal(WTERMSIG(chown_status)));
                    }
                    status = 128+WTERMSIG(chown_status);
                    goto cleanup;
                }
                if (chown_failed) {
                    fprintf(stderr, "Changing ownership of %s failed with exit code %d!\n", options->tty, chown_failed);
                    status = -1;
                    goto cleanup;
                }
                if (chmod(options->tty, 0600)) {
                    fprintf(stderr, "Couldn't change permissions of %s: %s\n", options->tty, strerror(errno));
                    status = -1;
                    goto cleanup;
                }
                curses_stdin = fopen(options->tty, "r+"); 
                curses_stdout = fopen(options->tty, "w+");
                if (!curses_stdin || !curses_stdout) {
                    status = -1;
                    fprintf(stderr, "Couldn't open %s: %s\n", options->tty, strerror(errno));
                    goto cleanup;
                }
            } else {
                status = -1;
                fprintf(stderr, "Couldn't open %s: %s\n", options->tty, strerror(errno));
                goto cleanup;
            }
        }
        curses_fd = fileno(curses_stdout);
        // todo: write method to get the TERM value of another tty
        /*
        currently assuming the tty is a virtual console 
        which seems to display fine with most terminal emulators even 
        though they have a different TERM value eg. xterm.
        while not setting "linux" on a virtual console results in a messy output.
        */
        curses_term = "linux";
        printf("Running on another terminal session...\n");
    }

    SCREEN *screen = newterm(curses_term, curses_stdout, curses_stdin);
    if (screen == NULL) {
        status = -1;
        fprintf(stderr, "Error opening screen: errno %d: %s", errno, strerror(errno));
        goto cleanup;
    }
    noecho();
    cbreak();
    curses_init = 1;

    #define DRAW_ERROR_TOLERANCE 256

    int32_t draw_successful = 0;
    int32_t draw_errors = 0;

    uint64_t pre_draw;

    if (clock_gettime(CLOCK_MONOTONIC, &draw_spec) == ERR) {
        status = -1;
        int_str_asprintf(&queued_err_msg, "Couldn't get timestamp: errno %d: %s\n", errno, strerror(errno));
        goto cleanup;
    }
    pre_draw = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000;
    video_pkt = av_packet_alloc();
    video_decoded = av_frame_alloc();
    video_converted = av_frame_alloc();
    int32_t break_condition = 0;
    int64_t nb_frames = video_stream->nb_frames;
        if (nb_frames <= 0) {
            fprintf(stderr, "Warning: No frame count metadata! Estimating from duration and fps...\n");
            nb_frames = (int64_t)((double) avfmt_ctx->duration / 1000000 * fps + 0.5);
        }
    double duration = floor((nb_frames-1) / fps) + fmod(nb_frames-1,  fps) / fps;
    if (!options->no_audio) {
#if SDL_VERSION_ATLEAST(3, 0, 0)
        if (SDL_GetAudioStreamQueued(stream) < (int)wav_data_len) {
            SDL_PutAudioStreamData(stream, wav_data, wav_data_len);
        }

        SDL_ResumeAudioStreamDevice(stream);
#else
        if(SDL_QueueAudio(*stream, wav_data, wav_data_len) < 0) {
            status = -1;
            fprintf(stderr, "Audio could not be queued: %s\n", SDL_GetError());
            goto main_cleanup;
        }

        SDL_PauseAudioDevice(*stream, 0);
#endif
    }
    while (av_read_frame(avfmt_ctx, video_pkt) >= 0) {
        if (ioctl(curses_fd, TIOCGWINSZ, &term_size) == -1) {
            status = -1;
            int_str_asprintf(&queued_err_msg, "Could't get terminal size: ioctl error %d: %s\n", errno, strerror(errno));
            goto cleanup;
        }
            if (term_size.ws_col < 2 || term_size.ws_col < 2) {
            int_str_asprintf(&queued_err_msg, "Invalid terminal resolution! Must be 2x2 or greater. %d %s\n", 0, "Placeholder");
            return 1;
        }
        sws_ctx = sws_getContext(
        vd_ctx->width, vd_ctx->height, vd_ctx->pix_fmt,
        term_size.ws_col-1, term_size.ws_row-1, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, NULL, NULL, NULL);

        if (!sws_ctx) {
            fprintf(stderr, "Failed to create sws context!\n");
            status = AVERROR(EINVAL);
            goto cleanup;
        }
        
        int buffer_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, term_size.ws_col-1, term_size.ws_row-1, 1);
        rgb_buffer = av_malloc(buffer_size);
        av_image_fill_arrays(video_converted->data, video_converted->linesize, rgb_buffer, AV_PIX_FMT_RGB24,
                            term_size.ws_col-1, term_size.ws_row-1, 1);
        refresh();

        if (video_pkt->stream_index == video_idx) {
            if ((loop_status = avcodec_send_packet(vd_ctx, video_pkt)) < 0) {
                int_str_asprintf(&queued_err_msg, "Failed to send packet: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                break_condition = 1;
                goto loop_cleanup;
            }

            while ((loop_status = avcodec_receive_frame(vd_ctx, video_decoded)) >= 0) {
                sws_scale(sws_ctx, (const uint8_t *const *)video_decoded->data, video_decoded->linesize,
                          0, vd_ctx->height, video_converted->data, video_converted->linesize);
                char *ascii_fb = malloc(buffer_size / 3);
                int32_t ascii_fb_size = 0;
                int32_t line = 0;
                for (int32_t idx = 0; idx+2 < buffer_size; idx+=3) {
                    uint8_t gradient = 0.299 * rgb_buffer[idx] + 0.587 * rgb_buffer[idx+1] + 0.114 * rgb_buffer[idx+2];
                    uint64_t gradient_idx = (gradient * (ascii_size - 1)) / 255;
                    if (gradient_idx > ascii_size) {
                        int_str_asprintf(&queued_err_msg, "Fatal: index greater than gradient list. Aborting to prevent oob array access... %d: %s\n", 0, "Placeholder");
                        status = -1;
                        goto loop_cleanup;
                    }
                    if (ascii_fb_size >= buffer_size / 3) {
                        int_str_asprintf(&queued_err_msg, "Fatal: ascii_fb_size greater than what was calculated. Aborting to prevent oob array access... %d: %s\n", 0, "Placeholder");
                        status = -1;
                        goto loop_cleanup;
                    }
                    ascii_fb[ascii_fb_size] = ascii_gradients[gradient_idx];
                    ascii_fb_size++;
                    if (ascii_fb_size == term_size.ws_col-1) {
                        if (line < (uint16_t) (term_size.ws_row))  {
                            chtype *ch_array = malloc(ascii_fb_size*sizeof(chtype));
                            for (int32_t ch = 0; ch < ascii_fb_size; ch++) {
                                ch_array[ch] = ascii_fb[ch] | A_NORMAL;
                            }
                            draw_successful = mvaddchnstr(line, 0, ch_array, ascii_fb_size);
                            free(ch_array);
                        }
                        line++;
                        ascii_fb_size = 0;
                    }
                }
                free(ascii_fb);
                frame_count++;
                if ((int64_t) frame_count > nb_frames) {
                    nb_frames = frame_count;
                }
                if (options->debug_mode) {
                    char *prefix = malloc(term_size.ws_col+1);
                    double time_position = floor(frame_count / fps) + fmod(frame_count,  fps) / fps;
                    int32_t prefix_size = snprintf(
                        prefix, term_size.ws_col+1,
                        "[Frame: %lu, %02u:%02u:%06.3lf]", frame_count, 
                        (uint32_t) floor(time_position / 3600), (uint32_t) floor(time_position / 60), fmod(time_position, 60)
                    );
                    char *suffix = malloc(term_size.ws_col);
                    int32_t suffix_size = snprintf(suffix, term_size.ws_col, "[%02u:%02u:%06.3lf, %lu Frames, %lu%%]", 
                        (uint32_t) floor(duration / 3600), (uint32_t) floor(duration / 60), fmod(duration, 60),
                        nb_frames-1, 100*frame_count / (nb_frames-1)
                    );
                    char *full_bar = progress_bar(term_size.ws_col-1, prefix, prefix_size, suffix, suffix_size, frame_count, nb_frames-1);
                    free(suffix);
                    free(prefix);
                    uint32_t full_bar_size;
                    for (full_bar_size = 0; full_bar[full_bar_size] != '\0'; full_bar_size++);
                    chtype *ch_array = malloc(full_bar_size*sizeof(chtype));
                    uint32_t current_style = A_NORMAL;
                    int32_t read_esc_seq = 0;
                    char *ansi_esc_buffer = malloc(4);
                    uint8_t aeb_size = 0;
                    uint32_t offset = 0;
                    for (uint32_t ch = 0; ch < full_bar_size; ch++) {
                        if (full_bar[ch] == 0x1b) {
                            read_esc_seq = 1;
                            ch++;
                            offset+=2;
                            continue;
                        }
                        if (read_esc_seq) {
                            offset++;
                            if (full_bar[ch] == 'm') {
                                int32_t esc_code = atoi(ansi_esc_buffer);
                                aeb_size = 0;
                                if (esc_code == 7) {
                                    current_style = A_STANDOUT;
                                }
                                if (esc_code == 0) {
                                    current_style = A_NORMAL;
                                }
                                read_esc_seq = 0;
                                continue;
                            }
                            if (aeb_size < 4) {
                                ansi_esc_buffer[aeb_size] = full_bar[ch];
                                aeb_size++;
                            }
                            continue;
                        }
                        ch_array[ch-offset] = full_bar[ch] | current_style;
                    }
                    free(ansi_esc_buffer);
                    free(full_bar);
                    draw_successful = mvaddchnstr(term_size.ws_row-1, 0, ch_array, term_size.ws_col-1);
                    free(ch_array);
                }
                if (draw_successful == ERR) {
                    draw_successful = 0;
                    draw_errors++;
                    continue;
                }
                if (draw_errors >= DRAW_ERROR_TOLERANCE) {
                    status = -1;
                    int_str_asprintf(&queued_err_msg, "Too many draw errors: errno %d: %s. Stopping...\n", errno, strerror(errno));
                    goto loop_cleanup;
                }
                if (clock_gettime(CLOCK_MONOTONIC, &draw_spec) == ERR) {
                    status = -1;
                    int_str_asprintf(&queued_err_msg, "Couldn't get timestamp: errno %d: %s\n", errno, strerror(errno));
                    goto loop_cleanup;
                }
                uint64_t draw_time = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000 - pre_draw;
                if (draw_time < interval * 1000000) {
                    int32_t sleep_interval = (int32_t) (interval * 1000000 - draw_time);
                    pre_draw = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000 + sleep_interval;
                    usleep(sleep_interval);
                } else {
                    if (options->debug_mode && draw_time > interval) {
                        
                    }
                    pre_draw = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000;
                }
            }
        }
loop_cleanup:
        av_packet_unref(video_pkt);
        sws_freeContext(sws_ctx);
        av_free(rgb_buffer);
        if (break_condition) {
            break;
        }
        if (status < 0) {
            goto cleanup;
        }
    }

    
cleanup:
    if (curses_init) {
        echo();
        nocbreak();
        endwin();
    }
    free(wav_data);
#if !SDL_VERSION_ATLEAST(3, 0, 0)
    if (stream) {
        SDL_CloseAudioDevice(*stream);
        free(stream);
    }
#endif
    av_packet_free(&audio_pkt);
    av_packet_free(&video_pkt);
    av_frame_free(&audio_decoded);  
    av_frame_free(&video_decoded);   
    if (status < 0 && out_avio_ctx && !audio_size) {
        audio_size = avio_close_dyn_buf(out_avio_ctx, &audio_buffer);
        out_avio_ctx = NULL;
    }
    avcodec_free_context(&encoder_ctx);
    avformat_free_context(out_fmt_ctx);
    av_frame_free(&video_converted);
    av_frame_free(&audio_converted);
    if (resampled_data) { 
        av_freep(&resampled_data[0]); 
        av_freep(&resampled_data); 
    }
    av_audio_fifo_free(fifo);
    swr_free(&swr_ctx);
    avcodec_free_context(&ad_ctx);
    avcodec_free_context(&vd_ctx);
    avformat_close_input(&avfmt_ctx);
    av_free(audio_buffer);
    
    if (queued_err_msg) {
        fprintf(stderr, "%s", queued_err_msg);
    }
    free(queued_err_msg);
    if (status < 0) {
        return 1;
    }
    if (status >= 128) {
        return status;
    }
    return 0;
}

int32_t print_help(char *filename, VIDTTYOptions *options){
    (void)(filename);
    VIDTTYArguments *arguments = options->arguments;

    printf("Usage: %s [OPTIONS] FILE\n", PROGRAM_NAME);
    printf("Options:\n");
    for (uint32_t argument = 0; argument < arguments->argumentc; argument++) {
        printf(" --%s", arguments->argumentv[argument]->name);
        if (arguments->argumentv[argument]->alias_count) {
            for (int32_t alias = 0; alias < arguments->argumentv[argument]->alias_count; alias++) {
                char *prefix;
                uint32_t alias_length;
                for(alias_length = 0; arguments->argumentv[argument]->aliases[alias][alias_length] != '\0'; alias_length++);
                if (alias_length == 1) {
                    prefix = "-\0";
                } else {
                    prefix = "--";
                }
                printf(", %s%s", prefix, arguments->argumentv[argument]->aliases[alias]);
            }
        }
        printf(" %s", arguments->argumentv[argument]->usage);
        printf("\t\t%s", arguments->argumentv[argument]->description); 
        printf("\n");
    }
    return 0;
}

VIDTTYArgument *new_vidtty_argument(
    char *name, int8_t type, stdcall associated_call, char *description, 
    char *usage, char *aliases[], uint32_t alias_count, void *associated_option, 
    int8_t associated_typedef
) {
    VIDTTYArgument *argument = malloc(sizeof(VIDTTYArgument));
    argument->name = name;
    argument->type = type;
    argument->associated_call = associated_call;
    argument->description = description;
    argument->usage = usage;
    argument->aliases = aliases;
    argument->alias_count = alias_count;
    argument->associated_option = associated_option;
    argument->associated_typedef = associated_typedef;
    return argument;
}

void free_vidtty_argument(VIDTTYArgument *argument) {
    if (argument) {
        if (argument->associated_typedef == 3) {
            free(argument->associated_option);
        }
        free(argument->aliases);
    }
    free(argument);
}

int32_t add_vidtty_argument(VIDTTYArguments *arguments, VIDTTYArgument *argument) {
    VIDTTYArgument **resized = realloc(arguments->argumentv, arguments->argumentc*sizeof(void *) + sizeof(void *));
    if (resized == NULL) {
        return -1;
    }
    arguments->argumentv = resized;
    arguments->argumentv[arguments->argumentc] = argument;
    arguments->argumentc++;
    return 0;
}

int32_t add_new_vidtty_argument(
    VIDTTYArguments *arguments, char *name, int8_t type, stdcall associated_call, 
    char *description, char *usage, char *aliases[], uint32_t alias_count, 
    void *associated_option, int8_t associated_typedef
) {
    VIDTTYArgument *argument = new_vidtty_argument(name, type, associated_call, description, usage, aliases, alias_count, associated_option, associated_typedef);
    return add_vidtty_argument(arguments, argument);
}

VIDTTYArguments *new_vidtty_arguments() {
    VIDTTYArguments *arguments = malloc(sizeof(VIDTTYArguments));
    arguments->argumentv = NULL;
    arguments->argumentc =  0;
    return arguments;
}

void free_vidtty_arguments(VIDTTYArguments *arguments) {
    if (arguments && arguments->argumentv) {
        for (uint32_t argument = 0; argument < arguments->argumentc; argument++) {
            free_vidtty_argument(arguments->argumentv[argument]);
        }
        free(arguments->argumentv);
    }
    free(arguments);
}

VIDTTYArguments *initialise_arguments(VIDTTYOptions *options) {
    VIDTTYArguments *arguments = new_vidtty_arguments();
    char **aliases = malloc(sizeof(void *));
    aliases[0] = "b";
    int32_t failed = add_new_vidtty_argument(
        arguments, "debug-mode", 0, NULL, 
        "Extra information will show at the bottom of the screen when playing", 
        "[filename]", aliases, 1, &options->debug_mode, 0
    );
    if (failed) {
        fprintf(stderr, "Failed to initialise arguments\n");
        free_vidtty_arguments(arguments);
        return NULL;
    }
    aliases = malloc(sizeof(void *));
    aliases[0] = "m";
    if ((failed = add_new_vidtty_argument(
            arguments, "no-audio", 0, NULL, 
            "Play or save video without any audio. Avoids loading up any audio modules", 
            "[filename]", aliases, 1, &options->no_audio, 0
    ))) {
        fprintf(stderr, "Failed to initialise arguments\n");
        free_vidtty_arguments(arguments);
        return NULL;
    }
    aliases = malloc(sizeof(void *));
    aliases[0] = "d";
    if ((failed = add_new_vidtty_argument(
            arguments, "dump", 2, dump_frames, 
            "Convert the video to a instantly playable vidtxt file", 
            "[filename]", aliases, 1, NULL, 0
    ))) {
        fprintf(stderr, "Failed to initialise arguments\n");
        free_vidtty_arguments(arguments);
        return NULL;
    }
    aliases = malloc(sizeof(void *));
    aliases[0] = "t";
    if ((failed = add_new_vidtty_argument(
            arguments, "tty", 1, NULL, 
            "Send output to another file or tty instead of the default stdout", 
            "TTY [filename]", aliases, 1, &options->tty, 2
    ))) {
        fprintf(stderr, "Failed to initialise arguments\n");
        free_vidtty_arguments(arguments);
        return NULL;
    }
    aliases = malloc(2*sizeof(void *));
    aliases[0] = "s";
    aliases[1] = "video-size";
    uint32_t **options_size = malloc(2*sizeof(void *));
    options_size[0] = &options->columns;
    options_size[1] = &options->lines;
    if ((failed = add_new_vidtty_argument(
            arguments, "size", 1, NULL, 
            "The output size of the video to convert", 
            "VIDEO_SIZE [filename]", aliases, 2, options_size, 3
    ))) {
        fprintf(stderr, "Failed to initialise arguments\n");
        free_vidtty_arguments(arguments);
        return NULL;
    }
    aliases = malloc(sizeof(void *));
    aliases[0] = "width";
    if ((failed = add_new_vidtty_argument(
            arguments, "columns", 1, NULL, 
            "The width or columns the converted video should be", 
            "COLUMNS [filename]", aliases, 1, &options->columns,  1
    ))) {
        fprintf(stderr, "Failed to initialise arguments\n");
        free_vidtty_arguments(arguments);
        return NULL;
    }
    aliases = malloc(sizeof(void *));
    aliases[0] = "height";
    if ((failed = add_new_vidtty_argument(
            arguments, "lines", 1, NULL, 
            "The height or lines the converted video should be", 
            "LINES [filename]", aliases, 1, &options->lines, 1
    ))) {
        fprintf(stderr, "Failed to initialise arguments\n");
        free_vidtty_arguments(arguments);
        return NULL;
    }
    aliases = malloc(sizeof(void *));
    aliases[0] = "i";
    if ((failed = add_new_vidtty_argument(
            arguments, "info", 2, vidtxt_info, 
            "Get information about a vidtxt file", 
            "[filename]", aliases, 1, NULL, 0
    ))) {
        fprintf(stderr, "Failed to initialise arguments\n");
        free_vidtty_arguments(arguments);
        return NULL;
    }
    aliases = malloc(sizeof(void *));
    aliases[0] = "h";
    if ((failed = add_new_vidtty_argument(
            arguments, "help", 2, print_help, 
            "Displays this message", 
            "[argument]", aliases, 1, NULL, 0
    ))) {
        fprintf(stderr, "Failed to initialise arguments\n");
        free_vidtty_arguments(arguments);
        return NULL;
    }

    return arguments;
}

int32_t main(int32_t argc, char *argv[]) {
    printf("%s %s\n%s %s\n", PROGRAM_NAME, VERSION, COPYRIGHT, AUTHOR);
    if (argc < 2) {
        printf("Not enough arguments! Try %s --help for usage.\n", PROGRAM_NAME);
    }
    VIDTTYOptions *options = calloc(1, sizeof(VIDTTYOptions));
    VIDTTYArguments *arguments = initialise_arguments(options);
    if (arguments == NULL) {
        return 1;
    }
    options->arguments = arguments;
    int32_t status = 0;
    char *filename = NULL;
    stdcall default_call = file_print_frames;
    char *argument_name;
    int32_t comp_offset;
    VIDTTYArgument *matching_argument = NULL;
    int32_t argument_match = 0;
    int32_t prev_arg_value = 0;
    char *curr_arg;
    for (int32_t argi = 1; argi < argc; argi++) {
        curr_arg = argv[argi];
        if (prev_arg_value) {
            prev_arg_value = 0;
            continue;
        }
        if (!(curr_arg && curr_arg[0] == '-' && curr_arg[1] != '\0')) {
            filename = curr_arg;
            continue;
        }
        for (uint32_t argument = 0; argument < arguments->argumentc; argument++) {
            for (int32_t alias = -1; alias < arguments->argumentv[argument]->alias_count; alias++) {
                if (alias < 0) {
                    argument_name = arguments->argumentv[argument]->name;
                } else {
                    argument_name = arguments->argumentv[argument]->aliases[alias];
                }
                comp_offset = 1;
                if (curr_arg[1] == '-') {
                    comp_offset = 2; 
                }
                for (uint32_t ch = 0; curr_arg[ch+comp_offset] == argument_name[ch]; ch++) {
                    if (curr_arg[ch+comp_offset] == '\0') {
                        argument_match = 1;
                        matching_argument = arguments->argumentv[argument];
                        break;
                    }
                }
            }
            if (argument_match) {
                argument_match = 0;
                if (matching_argument->type == 0) {
                    int32_t *associated_option = (int32_t *)(matching_argument->associated_option);
                    *associated_option = 1;
                }
                if (matching_argument->type == 1) {
                    prev_arg_value = 1;
                    if (!(argi+1 < argc)) {
                        printf("Bad argument usage. Try %s --help for usage.\n", PROGRAM_NAME);
                        free(options);
                        free_vidtty_arguments(arguments);
                        return 1;
                    }
                    if (matching_argument->associated_typedef == 0) {
                        int32_t *associated_option = (int32_t *)(matching_argument->associated_option);
                        char *checkval;
                        errno = 0;
                        int64_t converted_value = strtol(argv[argi+1], &checkval, 10);
                        if (errno || *checkval != '\0' || converted_value > INT32_MAX) {
                            printf("Invalid argument value. Try %s --help for usage.\n", PROGRAM_NAME);
                            free(options);
                            free_vidtty_arguments(arguments);
                            return 1;
                        }
                        *associated_option = (int32_t) converted_value;
                    }
                    if (matching_argument->associated_typedef == 1) {
                        uint32_t *associated_option = (uint32_t *)(matching_argument->associated_option);
                        char *checkval;
                        errno = 0;
                        uint64_t converted_value = strtoul(argv[argi+1], &checkval, 10);
                        if (errno || *checkval != '\0' || converted_value > UINT32_MAX) {
                            printf("Invalid argument value. Try %s --help for usage.\n", PROGRAM_NAME);
                            free(options);
                            free_vidtty_arguments(arguments);
                            return 1;
                        }
                        *associated_option = (uint32_t) converted_value;
                    }
                    if (matching_argument->associated_typedef == 2) {
                        if (matching_argument->associated_option == NULL) {
                            printf("%s\n", argv[argi+1]);
                            printf("video-size argument not implemented!\n");
                            free(options);
                            free_vidtty_arguments(arguments);
                            return 2;
                        }
                        char **associated_option = (char **)(matching_argument->associated_option);
                        *associated_option = argv[argi+1];
                    }
                    if (matching_argument->associated_typedef == 3) {
                        uint32_t **associated_option = (uint32_t **)(matching_argument->associated_option);
                        char *checkval;
                        errno = 0;
                        uint64_t converted_value = 0;
                        int32_t option_idx = 0;
                        uint32_t option_size;
                        for (option_size = 0; argv[argi+1][option_size] != '\0'; option_size++);
                        char *option_buffer = malloc(option_size);
                        int32_t arg_offset = 0;
                        for (int32_t idx = 0; argv[argi+1][idx] != '\0'; idx++) {
                            if ((argv[argi+1][idx] | 32) == 'x') {
                                option_buffer[idx-arg_offset] = '\0';
                                converted_value = strtoul(option_buffer, &checkval, 10);
                                if (errno || *checkval != '\0' || converted_value > UINT32_MAX) {
                                    printf("Invalid argument value. Try %s --help for usage.\n", PROGRAM_NAME);
                                    free(options);
                                    free_vidtty_arguments(arguments);
                                    return 1;
                                }
                                *associated_option[option_idx] = (uint32_t) converted_value;
                                arg_offset = idx+1;
                                option_idx++;
                                continue;
                            }
                            option_buffer[idx-arg_offset] = argv[argi+1][idx];
                        }
                        option_buffer[option_size-arg_offset] = '\0';
                        converted_value = strtoul(option_buffer, &checkval, 10);
                        if (errno || *checkval != '\0' || converted_value > UINT32_MAX) {
                            printf("Invalid argument value. Try %s --help for usage.\n", PROGRAM_NAME);
                            free(options);
                            free_vidtty_arguments(arguments);
                            return 1;
                        }
                        *associated_option[option_idx] = (uint32_t) converted_value;
                        free(option_buffer);
                    }
                }
                if (matching_argument->type == 2) {
                    default_call = matching_argument->associated_call;
                }
            }
        }
    }
    if (filename == NULL && default_call != print_help) {
        printf("Missing filename! Try %s --help for usage.\n", PROGRAM_NAME);
        free(options);
        free_vidtty_arguments(arguments);
        return 1;
    }
    if (default_call == file_print_frames) {
        uint64_t sig_value = 0;
        int32_t is_url = includes_match(filename, "://");
        if (!is_url) {
            FILE *fp = fopen(filename, "rb");
            if (fp == NULL) {
                fprintf(stderr, "Couldn't open %s: %s\n", filename, strerror(errno));
                free(options);
                free_vidtty_arguments(arguments);
                return 1;
            }
            fread(&sig_value, 6, 1, fp);
            fclose(fp);
        }
        if (is_url || be64toh(sig_value) != 0x5649445458540000) {
            default_call = render_frames;
        }
    }
    status = default_call(filename, options);
    free(options);
    free_vidtty_arguments(arguments);
    return status;
}