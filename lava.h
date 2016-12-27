#ifndef _lava_H
#define _lava_H

#include <string>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avstring.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>
}

#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main() */
#endif

#include <stdio.h>
#include <assert.h>

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55, 28, 1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

#define SDL_AUDIO_MIN_BUFFER_SIZE 512
#define SDL_AUDIO_BUFFER_SIZE 4800
#define MAX_AUDIO_FRAME_SIZE 192000
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE_MAX, SUBPICTURE_QUEUE_SIZE))
#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)

namespace LAVA {
    struct PacketQueue {
        AVPacketList *first_pkt, *last_pkt;
        int nb_packets;
        int size;
        SDL_mutex *mutex;
        SDL_cond *cond;
    };

    struct AudioState {
        SDL_Thread *read_tid;
        AVFormatContext *format_ctx;
        AVCodecContext *audio_codec_ctx_orig;
        AVCodecContext *audio_codec_ctx;
        AVCodec *audio_codec;
        bool audio_opend;
        PacketQueue audio_queue;
        uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
        unsigned int audio_buf_size;
        unsigned int audio_buf_index;
        AVFrame audio_frame;
        AVPacket audio_pkt;
        uint8_t *audio_pkt_data;
        int audio_pkt_size;
        int audio_stream_index;
        int duration;  // total secs
        double audio_clock;  // current playing clock
        uint8_t silence_buf[SDL_AUDIO_MIN_BUFFER_SIZE];
        int audio_volume;
        bool muted;
        bool paused;
        bool stopped;
        bool quit;
        int seek_req;
        int64_t seek_pos;
        int seek_flags;
        bool read_thread_abord;
        AudioState() {
            audio_opend = false;
            read_thread_abord = false;
            muted = false;
            paused = false;
            stopped = false;
            quit = false;
        };
    };

    class AudioCallbackInject {
    public:
        virtual void update(uint8_t *stream, int len)=0;
    };

    class Core {
    public:
        static Core *instance();
        void load_file(const std::string &filename);
        void pause();
        void stop();
        void set_volume(int volume);
        void seek(double percent);
        void seek_by_sec(int sec);
        int audio_duration();
        double time_position();
        bool is_stopping();
        ~Core();

        AudioState *getState();
        std::string &getInputFileName();
        int audio_open();
        void packet_queue_flush();
        int packet_queue_put(AVPacket *pkt);
        AVPacket flush_pkt;
        int audio_decode_frame(uint8_t *audio_buf, int buf_size);
        void setAudioCallbackInject(AudioCallbackInject *inst);
        void audioCallbackUpdate(uint8_t *stream, int len);
    private:
        static Core* _instance;
        explicit Core();
        void init_audio();
        AudioState *is;
        int stream_open(const std::string &input_filename);
        void stream_close();
        void stream_seek(int64_t pos, int flag);
        std::string input_filename;
        void audio_close();
        int packet_queue_get(AVPacket *pkt, int block);
        bool no_more_data_in_the_queue = false;
        int audio_resampling(AVCodecContext *audio_decode_ctx,
                            AVFrame *audio_decode_frame,
                            enum AVSampleFormat out_sample_fmt,
                            int out_channels,
                            int out_sample_rate,
                            uint8_t *out_buf);
        AudioCallbackInject *inject = nullptr;
    };
}

#endif // _lava_H
