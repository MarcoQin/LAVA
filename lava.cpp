#include "lava.h"

using namespace LAVA;

Core *Core::_instance = nullptr;

Core *Core::instance()
{
    return _instance ? _instance : new Core();
}

Core::Core()
{
    _instance = this;
    init_audio();
    is = new AudioState();
    cbkData = new CallbackData();
    m_deviceName = SDL_GetAudioDeviceName(0, 0);
    m_defaultDeviceName = m_deviceName;
}

std::vector<std::string> Core::getAudioDevices()
{
    std::vector<std::string> devices;
    int i, count = SDL_GetNumAudioDevices(0);
    for (i = 0; i < count; ++i) {
        SDL_Log("Audio device %d: %s", i, SDL_GetAudioDeviceName(i, 0));
        devices.push_back(SDL_GetAudioDeviceName(i, 0));
    }
    return devices;
}

void Core::setAudioDevice(std::string &deviceName)
{
    if (m_deviceName != deviceName) {
        m_deviceName = deviceName;

        audio_close();
        useDefaultDevice = m_deviceName == m_defaultDeviceName ? true : false;
        is->useDefaultDevice = useDefaultDevice;
        audio_open();

        if (!is_stopping()) {
        }
    }
}

std::string Core::currentAudioDevice()
{
    return m_deviceName;
}

Core::~Core()
{
    printf("destory core\n");
    stream_close();
}

void Core::init_audio()
{
    // Register all formats and codecs
    av_register_all();

    // Init SDL
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }

    av_init_packet(&flush_pkt);
    flush_pkt.data = (unsigned char *)"FLUSH";
}

void Core::load_file(const std::string &filename)
{
    if (!is->read_tid) {
        this->stream_open(filename);
    } else {
        this->audio_close();
        no_more_data_in_the_queue = false;
        input_filename = filename;
        is->read_thread_abord = true;
    }
    is->stopped = false;
}


static int extern_callback_thread(void *data)
{
    Core *core = (Core *)data;
    AudioState *is = core->getState();
    CallbackData *cbk = core->cbkData;
    for (;;) {
        if (is->quit) {
            fprintf(stderr, "extern_callback_thread EXIT \n");
            break;
        }
        SDL_LockMutex(cbk->mutex);
        SDL_CondWait(cbk->cond, cbk->mutex);
        memcpy(cbk->tmp_buffer, cbk->buffer, cbk->buffer_size);
        SDL_UnlockMutex(cbk->mutex);
        core->audioCallbackUpdate(cbk->tmp_buffer, cbk->buffer_size);
    }
    return 0;
}

static int read_thread(void *data)
{
    Core *core = (Core *) data;
    AudioState *is = core->getState();
    AVPacket packet;

start:
    // prepare clean stuff
    is->audio_buf_index = 0;
    is->audio_buf_size = 0;
    is->audio_pkt_size = 0;
    // clean work
    if (is->audio_codec_ctx_orig) {
        avcodec_close(is->audio_codec_ctx_orig);
        is->audio_codec_ctx_orig = NULL;
    }
    if (is->audio_codec_ctx) {
        avcodec_close(is->audio_codec_ctx);
        is->audio_codec_ctx = NULL;
    }
    if (is->format_ctx) {
        avformat_close_input(&is->format_ctx);
        is->format_ctx = NULL;
    }

    // Open audio file
    if (avformat_open_input(&is->format_ctx, core->getInputFileName().c_str(), NULL, NULL) != 0) {
        printf("avformat_open_input Failed: %s\n", core->getInputFileName().c_str());
        return -1;  // Error
    }

    // Retrieve stream information
    if (avformat_find_stream_info(is->format_ctx, NULL) < 0) {
        printf("avformat find_stream Failed\n");
        return -1;
    }

    is->audio_stream_index = -1;
    unsigned int i;
    for (i = 0; i < is->format_ctx->nb_streams; i++) {
        if (is->format_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO && is->audio_stream_index < 0) {
            is->audio_stream_index = i;
        }
    }
    if (is->audio_stream_index == -1) {
        printf("audio_stream_index == -1, return -1;\n");
        return -1;
    }

    if (is->format_ctx->duration != AV_NOPTS_VALUE) {
        int secs;
        int64_t duration = is->format_ctx->duration + (is->format_ctx->duration <= INT64_MAX - 5000 ? 5000 : 0);
        secs  = duration / AV_TIME_BASE;
        is->duration = secs;
    }

    is->audio_codec_ctx_orig = is->format_ctx->streams[is->audio_stream_index]->codec;
    is->audio_codec = avcodec_find_decoder(is->audio_codec_ctx_orig->codec_id);
    if (!is->audio_codec) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }

    // Copy context
    is->audio_codec_ctx = avcodec_alloc_context3(is->audio_codec);
    if (avcodec_copy_context(is->audio_codec_ctx, is->audio_codec_ctx_orig) != 0) {
        fprintf(stderr, "Couldn't copy codec context\n");
        return -1;
    }


    // Open audio device
    core->audio_open();

    if(avcodec_open2(is->audio_codec_ctx, is->audio_codec, NULL)<0) {
        printf("avcodec_open2 failed\n");
        return -1;
    }

    // Read frames and put to audio_queue
    for (;;) {
        if (is->quit) {
            break;
        }
        if (is->read_thread_abord) {
            is->read_thread_abord = false;
            core->packet_queue_flush();
            goto start;
        }
        // handle seek stuff
        if (is->seek_req) {
            int stream_index = -1;
            int64_t seek_target = is->seek_pos;

            if (is->audio_stream_index >= 0) {
                stream_index = is->audio_stream_index;
            }
            if (stream_index >= 0) {
                seek_target = av_rescale_q(seek_target, AV_TIME_BASE_Q,
                                           is->format_ctx->streams[stream_index]->time_base);
            }
            if (av_seek_frame(is->format_ctx, stream_index, seek_target,
                              is->seek_flags) < 0) {
                fprintf(stderr, "seek error\n");
            } else {
                if (is->audio_stream_index >= 0) {
                    core->packet_queue_flush();
                    core->packet_queue_put(&core->flush_pkt);
                }
            }
            is->seek_req = 0;
        }

        if (is->audio_queue.size > MAX_AUDIOQ_SIZE) {
            SDL_Delay(10);
            continue;
        }

        if (av_read_frame(is->format_ctx, &packet) < 0) {
            if (!is->read_thread_abord) {
                SDL_Delay(100); /* no error; wait for user input */
                continue;
            } else {
                is->read_thread_abord = false;
                core->packet_queue_flush();
                goto start;
            }
        }

        if (packet.stream_index == is->audio_stream_index) {
            core->packet_queue_put(&packet);
        } else {
            av_packet_unref(&packet); // Free the packet
        }
    }
    return 0;
}


int Core::stream_open(const std::string &filename)
{
    input_filename = filename;
    memset(&is->audio_queue, 0, sizeof(PacketQueue));
    is->audio_queue.mutex = SDL_CreateMutex();
    is->audio_queue.cond = SDL_CreateCond();
    is->audio_volume = SDL_MIX_MAXVOLUME;
    is->muted = false;

    is->read_tid = SDL_CreateThread(read_thread, "read_thread", this);
    SDL_CreateThread(extern_callback_thread, "extern_callback_thread", this);
    if (!is->read_tid) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
        stream_close();
        return -1;
    }
    return 0;
}

void Core::stream_close()
{
    is->quit = true;
    av_log(NULL, AV_LOG_WARNING, "wait for read_tid\n");
    SDL_WaitThread(is->read_tid, NULL);

    audio_close();
    if (is->audio_codec_ctx_orig) {
        avcodec_close(is->audio_codec_ctx_orig);
        is->audio_codec_ctx_orig = NULL;
    }
    if (is->audio_codec_ctx) {
        avcodec_close(is->audio_codec_ctx);
        is->audio_codec_ctx = NULL;
    }
    if (is->format_ctx) {
        avformat_close_input(&is->format_ctx);
        is->format_ctx = NULL;
    }

    // destroy audio_queue
    packet_queue_flush();
    if (is->audio_queue.mutex)
        SDL_DestroyMutex(is->audio_queue.mutex);
    if (is->audio_queue.cond)
        SDL_DestroyCond(is->audio_queue.cond);
}

void Core::audio_close()
{
    if (useDefaultDevice) {
        SDL_PauseAudio(1);
    } else {
        SDL_PauseAudioDevice(dev, 1);
    }
}

void Core::packet_queue_flush() {
    AVPacketList *pkt, *pkt1;
    PacketQueue *q = &is->audio_queue;
    SDL_LockMutex(q->mutex);
    for (pkt = q->first_pkt; pkt != NULL; pkt=pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_free(pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    SDL_UnlockMutex(q->mutex);
}


AudioState *Core::getState()
{
    return is;
}

std::string &Core::getInputFileName()
{
    return input_filename;
}


int Core:: packet_queue_get(AVPacket *pkt, int block)
{
    PacketQueue *q = &is->audio_queue;
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {

        if (is->quit) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            no_more_data_in_the_queue = false;
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            if (q->nb_packets <= 0) {
                no_more_data_in_the_queue = true;
                ret = -1;
                break;
            }
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

int Core::packet_queue_put(AVPacket *pkt)
{
    PacketQueue *q = &is->audio_queue;
    AVPacketList *pkt1;
    pkt1 = (AVPacketList *)av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;

    SDL_LockMutex(q->mutex);

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    no_more_data_in_the_queue = false;
    // Send signal to queue get function
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    return 0;
}

void audio_callback(void *userdata, Uint8 *stream, int len)
{
    Core *core = (Core *) userdata;
    CallbackData *cbk = core->cbkData;
    AudioState *is = core->getState();
    int len1, audio_size;
    int len_copy = len;
    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
            // We have already sent all our data; get more */
            audio_size = core->audio_decode_frame(is->audio_buf, sizeof(is->audio_buf));
            if (audio_size < 0) {
                // If error, output silence
                is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE; // eh...
                memset(is->audio_buf, 0, is->audio_buf_size);
            } else {
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        SDL_LockMutex(cbk->mutex);
        if (len1 > cbk->buffer_size) {
            memcpy(cbk->buffer, (uint8_t*)is->audio_buf + is->audio_buf_index, cbk->buffer_size);
            cbk->coppyed_len = cbk->buffer_size;
            SDL_CondSignal(cbk->cond);
        }
        SDL_UnlockMutex(cbk->mutex);
        if(len1 > len)
            len1 = len;
        if (!is->muted && is->audio_volume == SDL_MIX_MAXVOLUME || !is->useDefaultDevice) {
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        } else {
            memset(stream, is->silence_buf[0], len1);
            if (!is->muted) {
                SDL_MixAudio(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1, is->audio_volume);
            }
        }
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
}

int Core::audio_open()
{
    SDL_AudioSpec wanted_spec, spec;

    // Set audio settings from codec info
    wanted_spec.freq = is->audio_codec_ctx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = is->audio_codec_ctx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = this;

    if (is->audio_opend) {
        SDL_CloseAudio();
        if (dev) {
            SDL_CloseAudioDevice(dev);
        }
    }
    fprintf(stderr, "Current Audio Device: %s\n", m_deviceName.c_str());

    if (useDefaultDevice) {
        if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
            fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
            return -1;
        }
    } else {
        dev = SDL_OpenAudioDevice(m_deviceName.c_str(), 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FORMAT_CHANGE);
        if (dev == 0) {
            fprintf(stderr, "Failed to open audio: %s", SDL_GetError());
            return -1;
        }
    }
    is->audio_opend = true;

    if (useDefaultDevice) {
        SDL_PauseAudio(0);
    } else {
        SDL_PauseAudioDevice(dev, 0);
    }

    return 0;
}


int Core:: audio_decode_frame(uint8_t *audio_buf, int buf_size)
{
    AVPacket *pkt = &is->audio_pkt;

    int len1, data_size = 0;

    for (;;) {
        while (is->audio_pkt_size > 0) {
            int got_frame = 0;
            len1 = avcodec_decode_audio4(is->audio_codec_ctx, &is->audio_frame, &got_frame, pkt);
            if (len1 < 0) {
                // error, skip frame
                is->audio_pkt_size = 0;
                break;
            }
            is->audio_pkt_data += len1;
            is->audio_pkt_size -= len1;
            data_size = 0;
            if (got_frame) {
                // resamplling and copy buf to audio_buf
                data_size = audio_resampling(is->audio_codec_ctx, &is->audio_frame, AV_SAMPLE_FMT_S16, is->audio_frame.channels, is->audio_frame.sample_rate, audio_buf);
                assert(data_size <= buf_size);
            }
            if (data_size <= 0) {
                // No data yet, get more frames
                continue;
            }
            int n = 2 * is->audio_frame.channels;
            is->audio_clock += (double)data_size / (double)(n * is->audio_frame.sample_rate);
            // We have data, return it and come back for more later
            return data_size;
        }

        if (pkt->data) {
            av_packet_unref(pkt);
        }

        if (is->quit) {
            return -1;
        }

        if (packet_queue_get(pkt, 1) < 0) {
            return -1;
        }

        if (pkt->data == flush_pkt.data) {
            avcodec_flush_buffers(is->audio_codec_ctx);
            continue;
        }

        is->audio_pkt_data = pkt->data;
        is->audio_pkt_size = pkt->size;
        // if update, update the audio clock w/pts
        if (pkt->pts != AV_NOPTS_VALUE) {
            is->audio_clock = av_q2d(is->audio_codec_ctx->pkt_timebase) * pkt->pts;
        }
    }

}


int Core:: audio_resampling(AVCodecContext *audio_decode_ctx,
                            AVFrame *audio_decode_frame,
                            enum AVSampleFormat out_sample_fmt,
                            int out_channels,
                            int out_sample_rate,
                            uint8_t *out_buf)
{
    SwrContext *swr_ctx = NULL;
    int ret = 0;
    int64_t in_channel_layout = audio_decode_ctx->channel_layout;
    int64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
    int out_nb_channels = 0;
    int out_linesize = 0;
    int in_nb_samples = 0;
    int out_nb_samples = 0;
    int max_out_nb_samples = 0;
    uint8_t **resampled_data = NULL;
    int resampled_data_size = 0;

    swr_ctx = swr_alloc();
    if (!swr_ctx) {
        printf("swr_alloc error\n");
        return -1;
    }

    in_channel_layout = (audio_decode_ctx->channels ==
                     av_get_channel_layout_nb_channels(audio_decode_ctx->channel_layout)) ?
                     audio_decode_ctx->channel_layout :
                     av_get_default_channel_layout(audio_decode_ctx->channels);
    if (in_channel_layout <=0) {
        printf("in_channel_layout error\n");
        return -1;
    }

    if (out_channels == 1) {
        out_channel_layout = AV_CH_LAYOUT_MONO;
    } else if (out_channels == 2) {
        out_channel_layout = AV_CH_LAYOUT_STEREO;
    } else {
        out_channel_layout = AV_CH_LAYOUT_SURROUND;
    }

    in_nb_samples = audio_decode_frame->nb_samples;
    if (in_nb_samples <=0) {
        printf("in_nb_samples error\n");
        return -1;
    }

    av_opt_set_int(swr_ctx, "in_channel_layout", in_channel_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate", audio_decode_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", audio_decode_ctx->sample_fmt, 0);

    av_opt_set_int(swr_ctx, "out_channel_layout", out_channel_layout, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", out_sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", out_sample_fmt, 0);

    if ((ret = swr_init(swr_ctx)) < 0) {
        printf("Failed to initialize the resampling context\n");
        return -1;
    }

    max_out_nb_samples = out_nb_samples = av_rescale_rnd(in_nb_samples,
                                                         out_sample_rate,
                                                         audio_decode_ctx->sample_rate,
                                                         AV_ROUND_UP);

    if (max_out_nb_samples <= 0) {
        printf("av_rescale_rnd error\n");
        return -1;
    }

    out_nb_channels = av_get_channel_layout_nb_channels(out_channel_layout);

    ret = av_samples_alloc_array_and_samples(&resampled_data, &out_linesize, out_nb_channels, out_nb_samples, out_sample_fmt, 0);
    if (ret < 0) {
        printf("av_samples_alloc_array_and_samples error\n");
        return -1;
    }

    out_nb_samples = av_rescale_rnd(swr_get_delay(swr_ctx, audio_decode_ctx->sample_rate) + in_nb_samples,
                                    out_sample_rate, audio_decode_ctx->sample_rate, AV_ROUND_UP);
    if (out_nb_samples <= 0) {
        printf("av_rescale_rnd error\n");
        return -1;
    }

    if (out_nb_samples > max_out_nb_samples) {
        av_free(resampled_data[0]);
        ret = av_samples_alloc(resampled_data, &out_linesize, out_nb_channels, out_nb_samples, out_sample_fmt, 1);
        max_out_nb_samples = out_nb_samples;
    }

    if (swr_ctx) {
        ret = swr_convert(swr_ctx, resampled_data, out_nb_samples,
                          (const uint8_t **)audio_decode_frame->data, audio_decode_frame->nb_samples);
        if (ret < 0) {
            printf("swr_convert_error\n");
            return -1;
        }

        resampled_data_size = av_samples_get_buffer_size(&out_linesize, out_nb_channels, ret, out_sample_fmt, 1);
        if (resampled_data_size < 0) {
            printf("av_samples_get_buffer_size error\n");
            return -1;
        }
    } else {
        printf("swr_ctx null error\n");
        return -1;
    }

    memcpy(out_buf, resampled_data[0], resampled_data_size);

    if (resampled_data) {
        av_freep(&resampled_data[0]);
    }
    av_freep(&resampled_data);
    resampled_data = NULL;

    if (swr_ctx) {
        swr_free(&swr_ctx);
    }
    return resampled_data_size;

}

void Core::pause()
{
    is->paused = !is->paused;
    if (useDefaultDevice) {
        SDL_PauseAudio(is->paused);
    } else {
        SDL_PauseAudioDevice(dev, is->paused);
    }
}

void Core::stop()
{
    if (!is->stopped) {
        is->stopped = !is->stopped;
        audio_close();
        packet_queue_flush();
        is->duration = 0;
        is->audio_clock = 0;
    }
}

void Core::set_volume(int volume)
{
    if (volume < 0 || volume > 100)
        return;
    fprintf(stderr, "set_volume: %d\n", volume);
    volume = SDL_MIX_MAXVOLUME * (volume / 100.0f);
    fprintf(stderr, "set_volume1: %d\n", volume);
    is->audio_volume = volume;
}

void Core:: stream_seek(int64_t pos, int flag)
{
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_flags = flag < 0 ? AVSEEK_FLAG_BACKWARD : 0;
        is->seek_req = 1;
    }
}

void Core::seek(double percent)
{
    if (percent < 0 || percent > 100){
        return;
    }
    int incr = 0;
    double seek_target = (double)is->duration * percent / 100.0;
    double current_pos = is->audio_clock;
    incr = seek_target > current_pos ? 1 : -1;
    stream_seek((int64_t)(seek_target * AV_TIME_BASE), incr);
}

void Core:: seek_by_sec(int sec)
{
    int incr = 0;
    incr = sec > 0 ? 1 : -1;
    double pos = is->audio_clock;
    pos += sec;
    int duration = is->duration;
    if (duration < pos || pos < 0) {
        return;
    }
    stream_seek((int64_t)(pos * AV_TIME_BASE), incr);
}

void Core::seek_by_absolute_pos(double pos)
{
    int incr = 1;
    stream_seek((int64_t)(pos * AV_TIME_BASE), incr);
}

int Core::audio_duration()
{
    return is->duration;
}

bool Core::is_stopping()
{
    return no_more_data_in_the_queue;
}

double Core::time_position()
{
    return is->audio_clock;
}

void Core::audioCallbackUpdate(uint8_t *stream, int len)
{
    if (inject) {
        inject->update(stream, len);
    }
}

void Core::setAudioCallbackInject(AudioCallbackInject *inst)
{
    inject = inst;
}
