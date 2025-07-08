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
# if __has_include(<SDL3/SDL.h>)
#   include <SDL3/SDL_audio.h>
#   include <SDL3/SDL_version.h>
#   include <SDL3/SDL_iostream.h>
#   include <SDL3/SDL_init.h>
# elif __has_include(<SDL2/SDL.h>)
#   include <SDL2/SDL_audio.h>
#   include <SDL2/SDL_version.h>
#   include <SDL2/SDL_rwops.h>
#   include <SDL2/SDL.h>
# else
#   error "Requires SDL2 or later"
# endif

#include <signal.h>
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/version.h>

#define PROGRAM_NAME "vidtty"
#define VERSION "2.0.0a"
#define COPYRIGHT "Copyright (C) 2025"
#define LICENSE "MIT"
#define AUTHOR "Revnoplex"
#define VIDTXT_HEADER_SIZE 64
#define VID_METADATA_START 8

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
    stdcall associated_call;
    char *description;
    char **aliases;
    int32_t alias_count;
    char *usage;
    void *associated_option;
    int8_t associated_typedef;
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
    }

    vidtxt_info->columns = htobe32(vidtxt_info->columns);
    vidtxt_info->lines = htobe32(vidtxt_info->lines);
    uint64_t raw_fps;
    double new_fps;
    memmove(&raw_fps, &vidtxt_info->fps, sizeof(raw_fps));
    raw_fps = htobe64(raw_fps);
    memmove(&new_fps, &raw_fps, sizeof(new_fps));
    vidtxt_info->audio_size = htobe64(vidtxt_info->audio_size);

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
    FILE *fp = fopen(filename, "r");

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
    (void)(options);
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
# if SDL_VERSION_ATLEAST(3, 0, 0)
    SDL_AudioStream *stream = NULL;
# else
    SDL_AudioDeviceID *stream = NULL;
# endif
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
# if LIBAVUTIL_VERSION_MAJOR >= 57
        if ((status = av_channel_layout_copy(&encoder_ctx->ch_layout, &decoder_ctx->ch_layout)) < 0) {
            fprintf(stderr, "Failed to copy channel layout: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto ffmpeg_cleanup;
        }
# else 
        encoder_ctx->channel_layout = decoder_ctx->channel_layout;
        encoder_ctx->channels = decoder_ctx->channels;
# endif
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

# if LIBAVUTIL_VERSION_MAJOR >= 57
        status = swr_alloc_set_opts2(
            &swr_ctx,
            &encoder_ctx->ch_layout, encoder_ctx->sample_fmt, encoder_ctx->sample_rate,
            &decoder_ctx->ch_layout, decoder_ctx->sample_fmt, decoder_ctx->sample_rate,
            0, NULL
        );
# else
        swr_ctx = swr_alloc_set_opts(
            NULL,
            encoder_ctx->channel_layout, encoder_ctx->sample_fmt, encoder_ctx->sample_rate,
            decoder_ctx->channel_layout, decoder_ctx->sample_fmt, decoder_ctx->sample_rate,
            0, NULL
        );
        status = (swr_ctx == NULL) ? AVERROR(ENOMEM) : 0;
# endif
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

# if LIBAVUTIL_VERSION_MAJOR >= 57
        if ((status = av_channel_layout_copy(&converted->ch_layout, &encoder_ctx->ch_layout)) < 0) {
            fprintf(stderr, "Failed to copy channel layout: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto ffmpeg_cleanup;
        }
# else 
        converted->channel_layout = encoder_ctx->channel_layout;
        converted->channels = encoder_ctx->channels;
# endif

        int64_t next_pts = 0;
        uint64_t frame_count = 0;
        printf("writing frames...\r");
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

# if LIBAVUTIL_VERSION_MAJOR >= 57
                if ((status = av_channel_layout_copy(&converted->ch_layout, &encoder_ctx->ch_layout)) < 0) {
                    fprintf(stderr, "Failed to copy channel layout: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                    goto ffmpeg_cleanup;
                }
# else 
                converted->channel_layout = encoder_ctx->channel_layout;
                converted->channels = encoder_ctx->channels;
# endif
                
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
                        fprintf(stderr, "Error writing frame: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
                        goto ffmpeg_cleanup;
                    }
                    av_packet_unref(pkt);
                }

                av_frame_unref(decoded);
                av_frame_unref(converted);
            }

            av_packet_unref(pkt);
            frame_count++;
            if (frame_count % 1024 == 0) {
                printf("written %lu frames\r", frame_count);
                fflush(stdout);
            }
        }
        printf("written %lu frames\n", frame_count);
        fflush(stdout);

        if ((status = av_write_trailer(out_fmt_ctx)) < 0) {
            fprintf(stderr, "Error writing trailer: FFmpeg error 0x%02x: %s\n", status, av_err2str(status));
            goto ffmpeg_cleanup;
        }
ffmpeg_cleanup:
        av_packet_free(&pkt);
        av_frame_free(&decoded);
        if (converted) {
# if LIBAVUTIL_VERSION_MAJOR >= 57
            av_channel_layout_uninit(&converted->ch_layout);
# endif
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
        
# if SDL_VERSION_ATLEAST(3, 0, 0)
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

# endif

        // SDL takes over signal handling for SIGINT and SIGTERM. We dont't want that so we change it back.

        // save current handlers
        struct sigaction int_action, term_action;
        sigaction(SIGINT, NULL, &int_action);
        sigaction(SIGTERM, NULL, &term_action);
# if SDL_VERSION_ATLEAST(3, 0, 0)
        if (!SDL_Init(SDL_INIT_AUDIO)) {
# else 
        if (SDL_Init(SDL_INIT_AUDIO) < 0 ) {
# endif
            status = -1;
            fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
            goto main_cleanup;
        }
        
        // set the saved hanlers back
        sigaction(SIGINT, &int_action, NULL);
        sigaction(SIGTERM, &term_action, NULL);

# if SDL_VERSION_ATLEAST(3, 0, 0)
        SDL_IOStream *wav_stream = SDL_IOFromMem(wav_buffer, wav_size);
        int32_t load_result = SDL_LoadWAV_IO(wav_stream, 1, &spec, &wav_data, &wav_data_len);
# else
        SDL_RWops *wav_stream = SDL_RWFromMem(wav_buffer, wav_size);
        SDL_AudioSpec *spec_result;
        spec_result = SDL_LoadWAV_RW(wav_stream, 1, &spec, &wav_data, &wav_data_len);
        int32_t load_result = (spec_result != NULL);
        spec = *spec_result;
# endif
        if (!load_result) {
            status = -1;
            fprintf(stderr, "Couldn't load .wav file: %s\n", SDL_GetError());
            goto main_cleanup;
        }

# if SDL_VERSION_ATLEAST(3, 0, 0)
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
# else
        stream = malloc(sizeof(SDL_AudioDeviceID));
        SDL_AudioDeviceID device_id = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
        if (device_id) {
            *stream = device_id;
        }
# endif
        if (!stream) {
            status = -1;
            fprintf(stderr, "Couldn't create audio stream: %s\n", SDL_GetError());
            goto main_cleanup;
        }
# if SDL_VERSION_ATLEAST(3, 0, 0)
        if (SDL_GetAudioStreamQueued(stream) < (int)wav_data_len) {
            SDL_PutAudioStreamData(stream, wav_data, wav_data_len);
        }

        SDL_ResumeAudioStreamDevice(stream);
# else
        if(SDL_QueueAudio(*stream, wav_data, wav_data_len) < 0) {
            status = -1;
            fprintf(stderr, "Audio could not be queued: %s\n", SDL_GetError());
            goto main_cleanup;
        }

        SDL_PauseAudioDevice(*stream, 0);
# endif
 
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

    struct winsize term_size;

    int32_t curses_fd = 1;
    char *curses_term = getenv("TERM");
    FILE *curses_stdin = stdin;
    FILE *curses_stdout = stdout;
    FILE *curses_stderr = stderr;
    if (options->tty) {
        curses_stdin = fopen(options->tty, "r+"); 
        curses_stdout = fopen(options->tty, "w+");
        if (!curses_stdin || !curses_stdout) {
            status = -1;
            fprintf(stderr, "Couldn't open %s: %s\n", options->tty, strerror(errno));
            goto main_cleanup;
        }
        curses_stderr = curses_stdout;
        curses_fd = fileno(curses_stdout);
        // todo: write method to get the TERM value of another tty
        /*
        currently assuming the tty is a virtual console 
        which seems to display fine with most terminal emulators even 
        though they have a different TERM value eg. xterm.
        while not setting "linux" on a virtual console results in a messy output.
        */
        curses_term = "linux";
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

    # define DRAW_ERROR_TOLERANCE 256

    int32_t draw_successful;
    int32_t draw_errors = 0;
    line_contents = malloc(vidtxt_info->print_columns);
    
    uint64_t pre_draw;
    struct timespec draw_spec;

    if (clock_gettime(CLOCK_MONOTONIC, &draw_spec) == ERR) {
        status = -1;
        asprintf(&queued_err_msg, "Couldn't get timestamp: errno %d: %s\n", errno, strerror(errno));
        goto main_cleanup;
    }
    pre_draw = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000;

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
        if (draw_successful == ERR) {
            draw_successful = 0;
            draw_errors++;
            continue;
        }
        if (draw_errors >= DRAW_ERROR_TOLERANCE) {
            status = -1;
            asprintf(&queued_err_msg, "Too many draw errors: errno %d: %s. Stopping...\n", errno, strerror(errno));
            goto main_cleanup;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &draw_spec) == ERR) {
            status = -1;
            asprintf(&queued_err_msg, "Couldn't get timestamp: errno %d: %s\n", errno, strerror(errno));
            goto main_cleanup;
        }
        uint64_t draw_time = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000 - pre_draw;
        if (draw_time < interval * 1000000) {
            int32_t sleep_interval = (int32_t) (interval * 1000000 - draw_time);
            pre_draw = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000 + sleep_interval;
            usleep(sleep_interval);
        } else {
            if (options->debug_mode && draw_time > interval) {
                fprintf(curses_stderr, "Falling behind");
            }
            pre_draw = draw_spec.tv_sec * 1000000 + draw_spec.tv_nsec / 1000;
        }
    }
main_cleanup:
    free(line_contents);
    free(wav_data);
# if !SDL_VERSION_ATLEAST(3, 0, 0)
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
        "Duration: %02u:%02u:%02lf \n"
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
    (void)(filename);
    (void)(options);
    printf("Not implemented!\n");
    return 2;
}

int32_t print_help(char *filename, VIDTTYOptions *options){
    (void)(filename);
    VIDTTYArguments *arguments = options->arguments;
    if (options->columns) {
        printf("%u\n", options->columns);
    }
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
    if ((failed = add_new_vidtty_argument(
            arguments, "size", 1, NULL, 
            "The output size of the video to convert", 
            "VIDEO_SIZE [filename]", aliases, 2, NULL, 2
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
    VIDTTYOptions *options = malloc(sizeof(VIDTTYOptions));
    options->debug_mode = 0;
    options->no_audio = 0;
    options->tty = 0;
    options->lines = 0;
    options->columns = 0;
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
                            printf("video-size argument not implemented!\n");
                            free(options);
                            free_vidtty_arguments(arguments);
                            return 2;
                        }
                        char **associated_option = (char **)(matching_argument->associated_option);
                        *associated_option = argv[argi+1];
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
    status = default_call(filename, options);
    free(options);
    free_vidtty_arguments(arguments);
    return status;
}