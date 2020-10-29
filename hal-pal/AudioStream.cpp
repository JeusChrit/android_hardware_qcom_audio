/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of The Linux Foundation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_TAG "AHAL: AudioStream"
#define ATRACE_TAG (ATRACE_TAG_AUDIO | ATRACE_TAG_HAL)
/*#define LOG_NDEBUG 0*/
/*#define VERY_VERY_VERBOSE_LOGGING*/
#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif

#include "AudioDevice.h"
#include "AudioStream.h"

#include <log/log.h>
#include <utils/Trace.h>
#include <cutils/properties.h>

#include <chrono>
#include <thread>

#include "PalApi.h"
#include <audio_effects/effect_aec.h>
#include <audio_effects/effect_ns.h>
#include "audio_extn.h"
#include <audio_utils/format.h>

#define COMPRESS_OFFLOAD_FRAGMENT_SIZE (32 * 1024)
#define FLAC_COMPRESS_OFFLOAD_FRAGMENT_SIZE (256 * 1024)

#define MAX_READ_RETRY_COUNT 25

void StreamOutPrimary::GetStreamHandle(audio_stream_out** stream) {
  *stream = (audio_stream_out*)stream_.get();
}

void StreamInPrimary::GetStreamHandle(audio_stream_in** stream) {
  *stream = (audio_stream_in*)stream_.get();
}

uint32_t StreamPrimary::GetSampleRate() {
    return config_.sample_rate;
}

audio_format_t StreamPrimary::GetFormat() {
    return config_.format;
}

uint32_t StreamPrimary::GetChannelMask() {
    return config_.channel_mask;
}

audio_io_handle_t StreamPrimary::GetHandle()
{
    return handle_;
}

int StreamPrimary::GetUseCase()
{
    return usecase_;
}

bool StreamPrimary::AcquirePerfLock()
{
    if (!pal_stream_handle_ || !stream_started_) { // use-case is being setup
        std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
        AudioExtn::audio_extn_perf_lock_acquire(&adevice->perf_lock_handle, 0,
                adevice->perf_lock_opts, adevice->perf_lock_opts_size);
        return true;
    }

    return false;
}

void StreamPrimary::ReleasePerfLock()
{
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    AudioExtn::audio_extn_perf_lock_release(&adevice->perf_lock_handle);
}

#if 0
static pal_stream_type_t GetPalStreamType(audio_output_flags_t flags) {
    std::ignore = flags;
    return PAL_STREAM_LOW_LATENCY;
}
#endif
//audio_hw_device_t* AudioDevice::device_ = NULL;
std::shared_ptr<AudioDevice> AudioDevice::adev_ = nullptr;
std::shared_ptr<audio_hw_device_t> AudioDevice::device_ = nullptr;

static int32_t pal_callback(pal_stream_handle_t *stream_handle,
                            uint32_t event_id, uint32_t *event_data,
                            uint32_t event_size, void *cookie)
{
    stream_callback_event_t event;
    StreamOutPrimary *astream_out = static_cast<StreamOutPrimary *> (cookie);

    ALOGV("%s: stream_handle (%p), event_id (%x), event_data (%p), cookie (%p)"
          "event_size (%d)", __func__, stream_handle, event_id, event_data,
           cookie, event_size);

    switch (event_id)
    {
        case PAL_STREAM_CBK_EVENT_WRITE_READY:
        {
            std::lock_guard<std::mutex> write_guard (astream_out->write_wait_mutex_);
            astream_out->write_ready_ = true;
            ALOGV("%s: received WRITE_READY event", __func__);
            (astream_out->write_condition_).notify_all();
            event = STREAM_CBK_EVENT_WRITE_READY;
        }
        break;

    case PAL_STREAM_CBK_EVENT_DRAIN_READY:
        {
            std::lock_guard<std::mutex> drain_guard (astream_out->drain_wait_mutex_);
            astream_out->drain_ready_ = true;
            astream_out->sendGaplessMetadata = false;
            ALOGD("%s: received DRAIN_READY event", __func__);
            (astream_out->drain_condition_).notify_all();
            event = STREAM_CBK_EVENT_DRAIN_READY;
        }
        break;
    case PAL_STREAM_CBK_EVENT_PARTIAL_DRAIN_READY:
        {
            std::lock_guard<std::mutex> drain_guard (astream_out->drain_wait_mutex_);
            astream_out->drain_ready_ = true;
            astream_out->sendGaplessMetadata = true;
            ALOGD("%s: received PARTIAL DRAIN_READY event", __func__);
            (astream_out->drain_condition_).notify_all();
            event = STREAM_CBK_EVENT_DRAIN_READY;
        }
        break;
    case PAL_STREAM_CBK_EVENT_ERROR:
        event = STREAM_CBK_EVENT_ERROR;
        break;
    default:
        ALOGE("%s: Invalid event id:%d", __func__, event_id);
        return -EINVAL;
    }

    if (astream_out && astream_out->client_callback)
        astream_out->client_callback(event, NULL, astream_out->client_cookie);

    return 0;
}


static int astream_out_mmap_noirq_start(const struct audio_stream_out *stream)
{
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out;

    if (!adevice) {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    if (!astream_out) {
        ALOGE("%s: unable to get audio OutStream", __func__);
        return -EINVAL;
    }

    return astream_out->Start();
}

static int astream_out_mmap_noirq_stop(const struct audio_stream_out *stream)
{
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out;

    if (!adevice) {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    if (!astream_out) {
        ALOGE("%s: unable to get audio OutStream", __func__);
        return -EINVAL;
    }

    return astream_out->Stop();
}

static int astream_out_create_mmap_buffer(const struct audio_stream_out *stream,
        int32_t min_size_frames, struct audio_mmap_buffer_info *info)
{
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out;
    int ret = 0;

    if (!adevice) {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    if (!astream_out) {
        ALOGE("%s: unable to get audio OutStream", __func__);
        return -EINVAL;
    }

    if (info == NULL || !(min_size_frames > 0 && min_size_frames < INT32_MAX)) {
        ALOGE("%s: info = %p, min_size_frames = %d", __func__, info, min_size_frames);
        return -EINVAL;
    }
    if (astream_out->GetUseCase() != USECASE_AUDIO_PLAYBACK_MMAP) {
         ALOGE("%s: usecase = %d", __func__, astream_out->GetUseCase());
         return -ENOSYS;
    }

    ret = astream_out->CreateMmapBuffer(min_size_frames, info);
    if (ret)
        ALOGE("%s: failed %d\n", __func__, ret);

    return ret;
}

static int astream_out_get_mmap_position(const struct audio_stream_out *stream,
        struct audio_mmap_position *position)
{
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out;

    if (!adevice) {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    if (!astream_out) {
        ALOGE("%s: unable to get audio OutStream", __func__);
        return -EINVAL;
    }
    if (astream_out->GetUseCase() != USECASE_AUDIO_PLAYBACK_MMAP) {
         ALOGE("%s: usecase = %d", __func__, astream_out->GetUseCase());
         return -ENOSYS;
    }

    return astream_out->GetMmapPosition(position);
}

static uint32_t astream_out_get_sample_rate(const struct audio_stream *stream) {
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out;

    if (adevice) {
        astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    } else {
        ALOGE("%s: unable to get audio device", __func__);
        return 0;
    }

    if (astream_out)
        return astream_out->GetSampleRate();
    else
        return 0;
}

static int astream_set_sample_rate(struct audio_stream *stream __unused,
                                   uint32_t rate __unused) {
    return -ENOSYS;
}

static audio_format_t astream_out_get_format(
                                const struct audio_stream *stream) {
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out;

    if (adevice)
        astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    else
        ALOGE("%s: unable to get audio device", __func__);

    if (astream_out)
        return astream_out->GetFormat();
    else
        return AUDIO_FORMAT_DEFAULT;
}

static int astream_out_get_next_write_timestamp(
                                const struct audio_stream_out *stream __unused,
                                int64_t *timestamp __unused) {
    return -ENOSYS;
}

static int astream_set_format(struct audio_stream *stream __unused,
                              audio_format_t format __unused) {
    return -ENOSYS;
}

static size_t astream_out_get_buffer_size(const struct audio_stream *stream) {
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out =
                                    adevice->OutGetStream((audio_stream_t*)stream);

    if (astream_out)
        return astream_out->GetBufferSize();
    else
        return 0;
}

static uint32_t astream_out_get_channels(const struct audio_stream *stream) {

    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out;

    ALOGV("%s: stream_out(%p)", __func__, stream);
    if (adevice != nullptr) {
        astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    } else {
        ALOGE("%s: unable to get audio device", __func__);
        return 0;
    }

    if (astream_out != nullptr) {
        return astream_out->GetChannelMask();
    } else {
        ALOGE("%s: unable to get audio stream", __func__);
        return 0;
    }
}

static int astream_pause(struct audio_stream_out *stream)
{
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out;

    if (!adevice) {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    if (!astream_out) {
        ALOGE("%s: unable to get audio stream", __func__);
        return -EINVAL;
    }

    ALOGD("%s: pause", __func__);
    return astream_out->Pause();
}

static int astream_resume(struct audio_stream_out *stream)
{
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out;

    if (!adevice) {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    if (!astream_out) {
        ALOGE("%s: unable to get audio stream", __func__);
        return -EINVAL;
    }

    return astream_out->Resume();
}

static int astream_flush(struct audio_stream_out *stream)
{
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out;

    if (!adevice) {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    if (!astream_out) {
        ALOGE("%s: unable to get audio stream", __func__);
        return -EINVAL;
    }

    return astream_out->Flush();
}

static int astream_drain(struct audio_stream_out *stream, audio_drain_type_t type)
{
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out;

    if (!adevice) {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    if (!astream_out) {
        ALOGE("%s: unable to get audio stream", __func__);
        return -EINVAL;
    }

    return astream_out->Drain(type);
}

static int astream_set_callback(struct audio_stream_out *stream, stream_callback_t callback, void *cookie)
{
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out;

    if (!callback) {
        ALOGE("%s: NULL Callback passed", __func__);
        return -EINVAL;
    }

    if (!adevice) {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    if (!astream_out) {
        ALOGE("%s: unable to get audio stream", __func__);
        return -EINVAL;
    }

    astream_out->client_callback = callback;
    astream_out->client_cookie = cookie;

    return 0;
}

static int astream_out_standby(struct audio_stream *stream) {
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out;

    if (adevice) {
        astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    } else {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    ALOGD("%s: enter: stream (%p), usecase(%d: %s)", __func__, astream_out.get(),
          astream_out->GetUseCase(), use_case_table[astream_out->GetUseCase()]);

    if (astream_out) {
        return astream_out->Standby();
    } else {
        ALOGE("%s: unable to get audio stream", __func__);
        return -EINVAL;
    }
}

static int astream_dump(const struct audio_stream *stream, int fd) {
    std::ignore = stream;
    std::ignore = fd;
    ALOGD("%s: dump function not implemented", __func__);
    return 0;
}

static uint32_t astream_get_latency(const struct audio_stream_out *stream) {
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out;
    uint32_t period_ms, latency = 0;

    if (adevice) {
        astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    } else {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    switch (astream_out->GetUseCase()) {
    case USECASE_AUDIO_PLAYBACK_OFFLOAD:
        //TODO: get dsp latency for compress usecase
        latency = COMPRESS_OFFLOAD_PLAYBACK_LATENCY;
        break;
    case USECASE_AUDIO_PLAYBACK_ULL:
    case USECASE_AUDIO_PLAYBACK_MMAP:
        period_ms = (ULL_PERIOD_MULTIPLIER * ULL_PERIOD_SIZE *
                1000) / DEFAULT_OUTPUT_SAMPLING_RATE;
        latency = period_ms +
            StreamOutPrimary::GetRenderLatency(astream_out->flags_) / 1000;
        break;
    case USECASE_AUDIO_PLAYBACK_OFFLOAD2:
        latency = PCM_OFFLOAD_OUTPUT_PERIOD_DURATION;
        latency += StreamOutPrimary::GetRenderLatency(astream_out->flags_) / 1000;
        break;
    case USECASE_AUDIO_PLAYBACK_DEEP_BUFFER:
        latency = DEEP_BUFFER_OUTPUT_PERIOD_DURATION;
        latency += StreamOutPrimary::GetRenderLatency(astream_out->flags_) / 1000;
        break;
    case USECASE_AUDIO_PLAYBACK_LOW_LATENCY:
        latency = LOW_LATENCY_OUTPUT_PERIOD_DURATION;
        latency += StreamOutPrimary::GetRenderLatency(astream_out->flags_) / 1000;
        break;
    case USECASE_AUDIO_PLAYBACK_VOIP:
        latency += (VOIP_PERIOD_COUNT_DEFAULT * DEFAULT_VOIP_BUF_DURATION_MS * DEFAULT_VOIP_BIT_DEPTH_BYTE)/2;
        break;
    default:
        latency += StreamOutPrimary::GetRenderLatency(astream_out->flags_) / 1000;
        break;
    }

    // accounts for A2DP encoding and sink latency
    pal_param_bta2dp_t *param_bt_a2dp = NULL;
    size_t size = 0;
    int32_t ret;

    if (astream_out->isDeviceAvailable(PAL_DEVICE_OUT_BLUETOOTH_A2DP)) {
        ret = pal_get_param(PAL_PARAM_ID_BT_A2DP_ENCODER_LATENCY,
                            (void **)&param_bt_a2dp, &size, nullptr);
        if (!ret && param_bt_a2dp)
            latency += param_bt_a2dp->latency;
    }

    ALOGV("%s: Latency %d", __func__, latency);
    return latency;
}

static int astream_out_get_presentation_position(
                               const struct audio_stream_out *stream,
                               uint64_t *frames, struct timespec *timestamp) {
    std::ignore = stream;
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out;
    int ret = 0;
    if (adevice) {
        astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    } else {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }
    if (!timestamp) {
       ALOGE("%s: timestamp NULL", __func__);
       return -EINVAL;
    }
    if (astream_out) {
       switch (astream_out->GetPalStreamType(astream_out->flags_)) {
       case PAL_STREAM_COMPRESSED:
          ret = astream_out->GetFrames(frames);
          if (ret != 0) {
             ALOGE("%s: GetTimestamp failed %d", __func__, ret);
             return ret;
          }
          clock_gettime(CLOCK_MONOTONIC, timestamp);
          break;
       default:
          *frames = astream_out->GetFramesWritten(timestamp);
          break;
       }
    } else {
        //ALOGE("%s: unable to get audio stream", __func__);
        return -EINVAL;
    }
    ALOGV("%s: frames %lld played at %lld ", __func__, ((long long) *frames), timestamp->tv_sec * 1000000LL + timestamp->tv_nsec / 1000);

    return ret;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames) {
    std::ignore = stream;
    std::ignore = dsp_frames;
    ALOGD("%s: enter", __func__);
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out;
    int ret = 0;
    uint64_t frames = 0;

    if (adevice) {
        astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    } else {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }
    if (astream_out) {
        switch (astream_out->GetPalStreamType(astream_out->flags_)) {
        case PAL_STREAM_COMPRESSED:
           ret = astream_out->GetFrames(&frames);
           if (ret != 0) {
              ALOGE("%s: Get DSP Frames failed %d", __func__, ret);
              return ret;
           }
           *dsp_frames = (uint32_t) frames;
           break;
        default:
           break;
        }
    }

    //Temporary fix for Compressed offload SSR
    return -EINVAL;
}

static int astream_out_set_parameters(struct audio_stream *stream,
                                      const char *kvpairs) {
    int ret = 0;
    struct str_parms *parms = (str_parms *)NULL;
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out;
    if (adevice) {
        astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    } else {
        ret = -EINVAL;
        ALOGE("%s: unable to get audio device", __func__);
        goto exit;
    }

    ALOGD("%s: enter: usecase(%d: %s) kvpairs: %s",
          __func__, astream_out->GetUseCase(), use_case_table[astream_out->GetUseCase()], kvpairs);

    parms = str_parms_create_str(kvpairs);
    if (!parms) {
       ret = -EINVAL;
       goto exit;
    }


    ret = astream_out->SetParameters(parms);
    if (ret) {
        ALOGE("Stream SetParameters Error (%x)", ret);
        goto exit;
    }
exit:
    if (parms)
        str_parms_destroy(parms);
    return ret;
}

static char* astream_out_get_parameters(const struct audio_stream *stream,
                                        const char *keys) {
    int ret = 0;
    struct str_parms *query = str_parms_create_str(keys);
    char value[256];
    char *str = (char*) nullptr;
    std::shared_ptr<StreamOutPrimary> astream_out;
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    struct str_parms *reply = str_parms_create();
    //int index = 0;
    //int table_size = 0;

    if (adevice) {
        astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    } else {
        ret = -EINVAL;
        ALOGE("%s: unable to get audio device", __func__);
        goto exit;
    }

    if (!query || !reply) {
        if (reply)
            str_parms_destroy(reply);
        if (query)
            str_parms_destroy(query);
        ALOGE("out_get_parameters: failed to allocate mem for query or reply");
        return nullptr;
    }
    ALOGD("%s: keys: %s", __func__, keys);

    ret = str_parms_get_str(query, "is_direct_pcm_track", value, sizeof(value));
    if (ret >= 0) {
        value[0] = '\0';

        if (astream_out->flags_ & AUDIO_OUTPUT_FLAG_DIRECT &&
             !(astream_out->flags_ & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)) {
            ALOGV("in direct_pcm");
            strlcat(value, "true", sizeof(value));
        } else {
            ALOGV("not in direct_pcm");
            strlcat(value, "false", sizeof(value));
        }
        str_parms_add_str(reply, "is_direct_pcm_track", value);
        if (str)
            free(str);
        str = str_parms_to_str(reply);
    }

#if 0
    ret = str_parms_get_str(query, AUDIO_PARAMETER_STREAM_SUP_FORMATS, value, sizeof(value));
    if (ret >= 0) {
        value[0] = '\0';
        int stream_format = astream_out->GetFormat();
        table_size = sizeof(formats_name_to_enum_table) / sizeof(struct string_to_enum);
        index = astream_out->GetLookupTableIndex(formats_name_to_enum_table,
                                    table_size, stream_format);
        strlcat(value, formats_name_to_enum_table[index].name, sizeof(value));
        str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_SUP_FORMATS, value);
    }

    ret = str_parms_get_str(query, AUDIO_PARAMETER_STREAM_SUP_CHANNELS, value, sizeof(value));
    if (ret >= 0) {
        int stream_chn_mask = astream_out->GetChannelMask();

        table_size = sizeof(channels_name_to_enum_table) / sizeof(struct string_to_enum);
        index = astream_out->GetLookupTableIndex(channels_name_to_enum_table,
                                    table_size, stream_chn_mask);
        value[0] = '\0';
        strlcat(value, "AUDIO_CHANNEL_OUT_STEREO", sizeof(value));
        str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_SUP_CHANNELS, value);
    }

    ret = str_parms_get_str(query, AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES, value, sizeof(value));
    if (ret >= 0) {
        value[0] = '\0';
        int stream_sample_rate = astream_out->GetSampleRate();
        int cursor = 0;
        int avail = sizeof(value) - cursor;
        ret = snprintf(value + cursor, avail, "%s%d",
                       cursor > 0 ? "|" : "",
                       stream_sample_rate);
        str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES,
                          value);
    }

exit:
    /* do we need new hooks inside pal? */
    str = str_parms_to_str(reply);
    return str;
#endif
exit:
    return 0;
}

static int astream_out_set_volume(struct audio_stream_out *stream,
                                  float left, float right) {
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out;

    if (adevice) {
        astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    } else {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    if (astream_out) {
        return astream_out->SetVolume(left, right);
    } else {
        ALOGE("%s: unable to get audio stream", __func__);
        return -EINVAL;
    }
}

static int astream_out_add_audio_effect(
                                const struct audio_stream *stream __unused,
                                effect_handle_t effect __unused) {
    return 0;
}

static int astream_out_remove_audio_effect(
                                const struct audio_stream *stream __unused,
                                effect_handle_t effect __unused) {
    return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void *buffer,
                       size_t bytes) {

    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamInPrimary> astream_in;

    if (adevice) {
        astream_in = adevice->InGetStream((audio_stream_t*)stream);
    } else {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    if (astream_in) {
        return astream_in->Read(buffer, bytes);
    } else {
        ALOGE("%s: unable to get audio stream", __func__);
        return -EINVAL;
    }

    return 0;
}

static ssize_t out_write(struct audio_stream_out *stream, const void *buffer,
                         size_t bytes) {

    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamOutPrimary> astream_out;

    if (adevice) {
        astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    } else {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    if (astream_out) {
        return astream_out->Write(buffer, bytes);
    } else {
        ALOGE("%s: unable to get audio stream", __func__);
        return -EINVAL;
    }

    return 0;
}

static int astream_in_mmap_noirq_start(const struct audio_stream_in *stream)
{
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamInPrimary> astream_in;

    if (!adevice) {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    astream_in = adevice->InGetStream((audio_stream_t*)stream);
    if (!astream_in) {
        ALOGE("%s: unable to get audio InStream", __func__);
        return -EINVAL;
    }

    return astream_in->Start();
}

static int astream_in_mmap_noirq_stop(const struct audio_stream_in *stream)
{
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamInPrimary> astream_in;

    if (!adevice) {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    astream_in = adevice->InGetStream((audio_stream_t*)stream);
    if (!astream_in) {
        ALOGE("%s: unable to get audio InStream", __func__);
        return -EINVAL;
    }

    return astream_in->Stop();
}

static int astream_in_create_mmap_buffer(const struct audio_stream_in *stream,
        int32_t min_size_frames, struct audio_mmap_buffer_info *info)
{
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamInPrimary> astream_in;

    if (!adevice) {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    astream_in = adevice->InGetStream((audio_stream_t*)stream);
    if (!astream_in) {
        ALOGE("%s: unable to get audio InStream", __func__);
        return -EINVAL;
    }

    if (info == NULL || !(min_size_frames > 0 && min_size_frames < INT32_MAX)) {
        ALOGE("%s: info = %p, min_size_frames = %d", __func__, info, min_size_frames);
        return -EINVAL;
    }
    if (astream_in->GetUseCase() != USECASE_AUDIO_RECORD_MMAP) {
         ALOGE("%s: usecase = %d", __func__, astream_in->GetUseCase());
         return -ENOSYS;
    }

    return astream_in->CreateMmapBuffer(min_size_frames, info);
}

static int astream_in_get_mmap_position(const struct audio_stream_in *stream,
        struct audio_mmap_position *position)
{
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamInPrimary> astream_in;

    if (!adevice) {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    astream_in = adevice->InGetStream((audio_stream_t*)stream);
    if (!astream_in) {
        ALOGE("%s: unable to get audio InStream", __func__);
        return -EINVAL;
    }
    if (astream_in->GetUseCase() != USECASE_AUDIO_RECORD_MMAP) {
         ALOGE("%s: usecase = %d", __func__, astream_in->GetUseCase());
         return -ENOSYS;
    }

    return astream_in->GetMmapPosition(position);
}

static int astream_in_set_microphone_direction(
                        const struct audio_stream_in *stream,
                        audio_microphone_direction_t dir) {
    std::ignore = stream;
    std::ignore = dir;
    ALOGV("%s: function not implemented", __func__);
    //No plans to implement audiozoom
    return -1;
}

static int in_set_microphone_field_dimension(
                        const struct audio_stream_in *stream,
                        float zoom) {
    std::ignore = stream;
    std::ignore = zoom;
    ALOGV("%s: function not implemented", __func__);
    //No plans to implement audiozoom
    return -1;
}

static int astream_in_add_audio_effect(
                                const struct audio_stream *stream,
                                effect_handle_t effect)
{
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamInPrimary> astream_in;
    ALOGD("%s: Enter ", __func__);
    if (adevice) {
        astream_in = adevice->InGetStream((audio_stream_t*)stream);
    } else {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }
    if (astream_in) {
        return astream_in->addRemoveAudioEffect(stream, effect, true);
    } else {
        ALOGE("%s: unable to get audio stream", __func__);
        return -EINVAL;
    }
}

static int astream_in_remove_audio_effect(const struct audio_stream *stream,
                                          effect_handle_t effect)
{
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamInPrimary> astream_in;
    ALOGD("%s: Enter ", __func__);
    if (adevice) {
        astream_in = adevice->InGetStream((audio_stream_t*)stream);
    } else {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }
    if (astream_in) {
        return astream_in->addRemoveAudioEffect(stream, effect, false);
    } else {
        ALOGE("%s: unable to get audio stream", __func__);
        return -EINVAL;
    }
}

static int astream_in_get_capture_position(const struct audio_stream_in *stream,
                                           int64_t *frames, int64_t *time) {
    std::ignore = stream;
    std::ignore = frames;
    std::ignore = time;
    //TODO: get timestamp for input streams
    ALOGV("%s: position not implemented currently supported in pal", __func__);
    return 0;
}

static uint32_t astream_in_get_input_frames_lost(
                                struct audio_stream_in *stream __unused) {
    return 0;
}

static void in_update_sink_metadata(
                                struct audio_stream_in *stream,
                                const struct sink_metadata *sink_metadata) {
    std::ignore = stream;
    std::ignore = sink_metadata;

    ALOGV("%s: sink meta data update not  supported in pal", __func__);
}

static int astream_in_get_active_microphones(
                        const struct audio_stream_in *stream,
                        struct audio_microphone_characteristic_t *mic_array,
                        size_t *mic_count) {
    std::ignore = stream;
    std::ignore = mic_array;
    std::ignore = mic_count;
    ALOGV("%s: get active mics not currently supported in pal", __func__);
    return 0;
}

static uint32_t astream_in_get_sample_rate(const struct audio_stream *stream) {
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamInPrimary> astream_in;
    ALOGE("%s: Inside", __func__);

    if (adevice) {
        astream_in = adevice->InGetStream((audio_stream_t*)stream);
    } else {
        ALOGE("%s: unable to get audio device", __func__);
        return 0;
    }

    if (astream_in)
        return astream_in->GetSampleRate();
    else
        return 0;
}

static uint32_t astream_in_get_channels(const struct audio_stream *stream) {

    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamInPrimary> astream_in;

    if (adevice) {
        astream_in = adevice->InGetStream((audio_stream_t*)stream);
    } else {
        ALOGE("%s: unable to get audio device", __func__);
        return 0;
    }

    if (astream_in) {
        return astream_in->GetChannelMask();
    } else {
        ALOGE("%s: unable to get audio stream", __func__);
        return 0;
    }
}

static audio_format_t astream_in_get_format(const struct audio_stream *stream) {
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamInPrimary> astream_in;

    if (adevice)
        astream_in = adevice->InGetStream((audio_stream_t*)stream);
    else
        ALOGE("%s: unable to get audio device", __func__);

    if (astream_in)
        return astream_in->GetFormat();
    else
        return AUDIO_FORMAT_DEFAULT;
}

static int astream_in_standby(struct audio_stream *stream) {
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamInPrimary> astream_in;

    if (adevice) {
        astream_in = adevice->InGetStream((audio_stream_t*)stream);
    } else {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    ALOGD("%s: enter: stream (%p) usecase(%d: %s)", __func__, astream_in.get(),
          astream_in->GetUseCase(), use_case_table[astream_in->GetUseCase()]);

    if (astream_in) {
        return astream_in->Standby();
    } else {
        ALOGE("%s: unable to get audio stream", __func__);
        return -EINVAL;
    }
}

static int astream_in_set_parameters(struct audio_stream *stream, const char *kvpairs) {
    int ret = -EINVAL;

    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamInPrimary> astream_in;


    if (!stream || !kvpairs) {
        ret = 0;
        goto error;
    }

    if (adevice) {
        astream_in = adevice->InGetStream((audio_stream_t*)stream);
    } else {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    if (astream_in) {
        return astream_in->SetParameters(kvpairs);
    }

error:
    return ret;
}

static char* astream_in_get_parameters(const struct audio_stream *stream,
                                       const char *keys) {
    std::ignore = stream;
    std::ignore = keys;
    ALOGD("%s: function not implemented", __func__);
    return 0;
}

static int astream_in_set_gain(struct audio_stream_in *stream, float gain) {
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamInPrimary> astream_in;

    if (adevice) {
        astream_in = adevice->InGetStream((audio_stream_t*)stream);
    } else {
        ALOGE("%s: unable to get audio device", __func__);
        return -EINVAL;
    }

    if (astream_in) {
        return astream_in->SetGain(gain);
    } else {
        ALOGE("%s: unable to get audio stream", __func__);
        return -EINVAL;
    }
}

static size_t astream_in_get_buffer_size(const struct audio_stream *stream) {
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    std::shared_ptr<StreamInPrimary> astream_in =
                            adevice->InGetStream((audio_stream_t*)stream);

    if (astream_in)
        return astream_in->GetBufferSize();
    else
        return 0;
}

int StreamPrimary::getPalDeviceIds(const std::set<audio_devices_t>& halDeviceIds,
                                   pal_device_id_t* qualIds) {
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    return adevice->GetPalDeviceIds(halDeviceIds, qualIds);
}

int StreamPrimary::GetDeviceAddress(struct str_parms *parms, int *card_id,
                                      int *device_num) {
    int ret = -EINVAL;
    char value[64];

    ret = str_parms_get_str(parms, "card", value, sizeof(value));
    if (ret >= 0) {
        *card_id = atoi(value);
        ret = str_parms_get_str(parms, "device", value, sizeof(value));
        if (ret >= 0) {
            *device_num = atoi(value);
        }
    }

    return ret;
}

int StreamPrimary::GetLookupTableIndex(const struct string_to_enum *table,
                                    int table_size, int value) {
    int index = -EINVAL;
    int i = 0;

    for (i = 0; i < table_size; i++) {
        if (value == table[i].value) {
            index = i;
            break;
        }
    }

    return index;
}

pal_stream_type_t StreamInPrimary::GetPalStreamType(
                                        audio_input_flags_t halStreamFlags,
                                        uint32_t sample_rate) {
    pal_stream_type_t palStreamType = PAL_STREAM_LOW_LATENCY;
    if ((halStreamFlags & AUDIO_INPUT_FLAG_VOIP_TX)!=0) {
         palStreamType = PAL_STREAM_VOIP_TX;
         return palStreamType;
    }

    if (sample_rate == LOW_LATENCY_CAPTURE_SAMPLE_RATE &&
            (halStreamFlags & AUDIO_INPUT_FLAG_TIMESTAMP) == 0 &&
            (halStreamFlags & AUDIO_INPUT_FLAG_COMPRESS) == 0 &&
            (halStreamFlags & AUDIO_INPUT_FLAG_FAST) != 0) {
        if (isDeviceAvailable(PAL_DEVICE_IN_PROXY))
            palStreamType = PAL_STREAM_PROXY;
        else
            palStreamType = PAL_STREAM_ULTRA_LOW_LATENCY;

        return palStreamType;
    }
    switch (halStreamFlags) {
        case AUDIO_INPUT_FLAG_FAST:
            palStreamType = PAL_STREAM_LOW_LATENCY;
            break;
        case AUDIO_INPUT_FLAG_RAW:
        case AUDIO_INPUT_FLAG_DIRECT:
            palStreamType = PAL_STREAM_RAW;
            break;
        case AUDIO_INPUT_FLAG_VOIP_TX:
            palStreamType = PAL_STREAM_VOIP_TX;
            break;
        case AUDIO_INPUT_FLAG_MMAP_NOIRQ:
            palStreamType = PAL_STREAM_ULTRA_LOW_LATENCY;
            break;
        case AUDIO_INPUT_FLAG_NONE:
            palStreamType = PAL_STREAM_DEEP_BUFFER;
            break;
        default:
            /*
            unsupported from PAL
            AUDIO_INPUT_FLAG_HW_HOTWORD = 0x2,
            AUDIO_INPUT_FLAG_SYNC        = 0x8,
            AUDIO_INPUT_FLAG_HW_AV_SYNC = 0x40,
            */
            ALOGE("%s: flag %#x is not supported from PAL." ,
                      __func__, halStreamFlags);
            break;
    }

    return palStreamType;
}

pal_stream_type_t StreamOutPrimary::GetPalStreamType(
                                    audio_output_flags_t halStreamFlags) {
    pal_stream_type_t palStreamType = PAL_STREAM_LOW_LATENCY;
    if ((halStreamFlags & AUDIO_OUTPUT_FLAG_VOIP_RX)!=0) {
        palStreamType = PAL_STREAM_VOIP_RX;
        return palStreamType;
    }
    if ((halStreamFlags & AUDIO_OUTPUT_FLAG_RAW) != 0) {
        palStreamType = PAL_STREAM_ULTRA_LOW_LATENCY;
    } else if ((halStreamFlags & AUDIO_OUTPUT_FLAG_FAST) != 0) {
        palStreamType = PAL_STREAM_LOW_LATENCY;
    } else if (halStreamFlags ==
                    (AUDIO_OUTPUT_FLAG_FAST|AUDIO_OUTPUT_FLAG_RAW)) {
        palStreamType = PAL_STREAM_RAW;
    } else if (halStreamFlags == AUDIO_OUTPUT_FLAG_DEEP_BUFFER) {
        palStreamType = PAL_STREAM_DEEP_BUFFER;
    } else if (halStreamFlags ==
                    (AUDIO_OUTPUT_FLAG_DIRECT|AUDIO_OUTPUT_FLAG_MMAP_NOIRQ)) {
        // mmap_no_irq_out: to be confirmed
        palStreamType = PAL_STREAM_ULTRA_LOW_LATENCY;
    } else if (halStreamFlags & AUDIO_OUTPUT_FLAG_MMAP_NOIRQ) {
        palStreamType = PAL_STREAM_ULTRA_LOW_LATENCY;
    } else if (halStreamFlags & AUDIO_OUTPUT_FLAG_RAW) {
        palStreamType = PAL_STREAM_ULTRA_LOW_LATENCY;
    } else if (halStreamFlags == (AUDIO_OUTPUT_FLAG_DIRECT|
                                      AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD|
                                  AUDIO_OUTPUT_FLAG_NON_BLOCKING)) {
        // hifi: to be confirmed
        palStreamType = PAL_STREAM_COMPRESSED;
    } else if (halStreamFlags == AUDIO_OUTPUT_FLAG_DIRECT) {
        palStreamType = PAL_STREAM_PCM_OFFLOAD;
    } else if (halStreamFlags == (AUDIO_OUTPUT_FLAG_DIRECT|
                                      AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD|
                                  AUDIO_OUTPUT_FLAG_NON_BLOCKING)) {
        palStreamType = PAL_STREAM_COMPRESSED;
    } else if (halStreamFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) {
        // dsd_compress_passthrough
        palStreamType = PAL_STREAM_COMPRESSED;
    } else if (halStreamFlags == (AUDIO_OUTPUT_FLAG_DIRECT|
                                      AUDIO_OUTPUT_FLAG_VOIP_RX)) {
        // voice rx
        palStreamType = PAL_STREAM_VOIP_RX;
    } else if (halStreamFlags == AUDIO_OUTPUT_FLAG_VOIP_RX) {
        palStreamType = PAL_STREAM_VOIP_RX;
    } else if (halStreamFlags == AUDIO_OUTPUT_FLAG_INCALL_MUSIC) {
        // incall_music_uplink
        palStreamType = PAL_STREAM_VOICE_CALL_MUSIC;
    } else {
        palStreamType = PAL_STREAM_GENERIC;
    }
    return palStreamType;
}

int StreamOutPrimary::FillHalFnPtrs() {
    int ret = 0;

    stream_.get()->common.get_sample_rate = astream_out_get_sample_rate;
    stream_.get()->common.set_sample_rate = astream_set_sample_rate;
    stream_.get()->common.get_buffer_size = astream_out_get_buffer_size;
    stream_.get()->common.get_channels = astream_out_get_channels;
    stream_.get()->common.get_format = astream_out_get_format;
    stream_.get()->common.set_format = astream_set_format;
    stream_.get()->common.standby = astream_out_standby;
    stream_.get()->common.dump = astream_dump;
    stream_.get()->common.set_parameters = astream_out_set_parameters;
    stream_.get()->common.get_parameters = astream_out_get_parameters;
    stream_.get()->common.add_audio_effect = astream_out_add_audio_effect;
    stream_.get()->common.remove_audio_effect =
                                            astream_out_remove_audio_effect;
    stream_.get()->get_latency = astream_get_latency;
    stream_.get()->set_volume = astream_out_set_volume;
    stream_.get()->write = out_write;
    stream_.get()->get_render_position = out_get_render_position;
    stream_.get()->get_next_write_timestamp =
                                            astream_out_get_next_write_timestamp;
    stream_.get()->get_presentation_position =
                                            astream_out_get_presentation_position;
    stream_.get()->update_source_metadata = NULL;
    stream_.get()->pause = astream_pause;
    stream_.get()->resume = astream_resume;
    stream_.get()->drain = astream_drain;
    stream_.get()->flush = astream_flush;
    stream_.get()->set_callback = astream_set_callback;
    return ret;
}

int StreamOutPrimary::GetMmapPosition(struct audio_mmap_position *position)
{
    struct pal_mmap_position pal_mmap_pos;
    int32_t ret = 0;

    if (pal_stream_handle_ == nullptr) {
        ALOGE("%s: pal handle is null\n", __func__);
        return -EINVAL;
    }

    ret = pal_stream_get_mmap_position(pal_stream_handle_, &pal_mmap_pos);
    if (ret) {
        ALOGE("%s: failed to get mmap position %d\n", __func__, ret);
        return ret;
    }
    position->position_frames = pal_mmap_pos.position_frames;
    position->time_nanoseconds = pal_mmap_pos.time_nanoseconds;

#if 0
    /** Check if persist vendor property is available */
    const int32_t kDefaultOffsetMicros = 0;
    int32_t mmap_time_offset_micros = property_get_int32(
            "persist.vendor.audio.out_mmap_delay_micros", kDefaultOffsetMicros);

    position->time_nanoseconds += mmap_time_offset_micros * (int64_t)1000;
#endif

    return 0;
}

bool StreamOutPrimary::isDeviceAvailable(pal_device_id_t deviceId)
{
    for (int i = 0; i < mAndroidOutDevices.size(); i++) {
        if (mPalOutDevice[i].id == deviceId)
            return true;
    }

    return false;
}

int StreamOutPrimary::CreateMmapBuffer(int32_t min_size_frames,
        struct audio_mmap_buffer_info *info)
{
    int ret;
    struct pal_mmap_buffer palMmapBuf;

    if (pal_stream_handle_) {
        ALOGE("%s: pal handle already created\n", __func__);
        return -EINVAL;
    }

    ret = Open();
    if (ret) {
        ALOGE("%s: failed to open stream.", __func__);
        return ret;
    }
    ret = pal_stream_create_mmap_buffer(pal_stream_handle_,
            min_size_frames, &palMmapBuf);
    if (ret) {
        ALOGE("%s: failed to create mmap buffer: %d", __func__, ret);
        Standby();
        return ret;
    }
    info->shared_memory_address = palMmapBuf.buffer;
    info->shared_memory_fd = palMmapBuf.fd;
    info->buffer_size_frames = palMmapBuf.buffer_size_frames;
    info->burst_size_frames = palMmapBuf.burst_size_frames;
    info->flags = (audio_mmap_buffer_flag) AUDIO_MMAP_APPLICATION_SHAREABLE;

    return ret;
}

int StreamOutPrimary::Stop() {
    int ret = -ENOSYS;

    if (usecase_ == USECASE_AUDIO_PLAYBACK_MMAP &&
            pal_stream_handle_ && stream_started_) {

        ret = pal_stream_stop(pal_stream_handle_);
        if (ret == 0) {
            stream_started_ = false;
            stream_paused_ = false;
        }
    }
    return ret;
}

int StreamOutPrimary::Start() {
    int ret = -ENOSYS;

    if (usecase_ == USECASE_AUDIO_PLAYBACK_MMAP &&
            pal_stream_handle_ && !stream_started_) {

        ret = pal_stream_start(pal_stream_handle_);
        if (ret == 0)
            stream_started_ = true;
    }
    return ret;
}

int StreamOutPrimary::Pause() {
    int ret = 0;

    if (pal_stream_handle_) {
        ret = pal_stream_pause(pal_stream_handle_);
    }
    if (ret)
        return -EINVAL;
    else
    {
        stream_paused_ = true;
        return ret;
    }
}

int StreamOutPrimary::Resume() {
    int ret = 0;

    if (pal_stream_handle_) {
        ret = pal_stream_resume(pal_stream_handle_);
    }
    if (ret)
        return -EINVAL;
    else {
        stream_paused_ = false;
        return ret;
    }
}

int StreamOutPrimary::Flush() {
    int ret = 0;
    ALOGD("%s: Enter", __func__);
    if (pal_stream_handle_) {
        if(stream_paused_ == true)
        {
            ret = pal_stream_flush(pal_stream_handle_);
            if (!ret) {
                ret = pal_stream_resume(pal_stream_handle_);
                if (!ret)
                    stream_paused_ = false;
            }
        } else {
            ALOGI("%s: called in invalid state (stream not paused)", __func__ );
        }
        total_bytes_written_ = 0;
    }

    if (ret)
        return -EINVAL;
    else {
        return ret;
    }
}

int StreamOutPrimary::Drain(audio_drain_type_t type) {
    int ret = 0;
    pal_drain_type_t palDrainType;

    switch (type) {
      case AUDIO_DRAIN_ALL:
           palDrainType = PAL_DRAIN;
           break;
      case AUDIO_DRAIN_EARLY_NOTIFY:
           palDrainType = PAL_DRAIN_PARTIAL;
           break;
    default:
           ALOGE("%s: Invalid drain type:%d", __func__, type);
           return -EINVAL;
    }

    if (pal_stream_handle_)
        ret = pal_stream_drain(pal_stream_handle_, palDrainType);

    if (ret) {
        ALOGE("%s: Invalid drain type:%d", __func__, type);
    }

    return ret;
}

int StreamOutPrimary::Standby() {
    int ret = 0;

    if (pal_stream_handle_) {
        ret = pal_stream_stop(pal_stream_handle_);
        if (ret) {
            ALOGE("%s: failed to stop stream.", __func__);
            return -EINVAL;
        }
    }

    stream_started_ = false;
    stream_paused_ = false;
    if (CheckOffloadEffectsType(streamAttributes_.type)) {
        ret = StopOffloadEffects(handle_, pal_stream_handle_);
    }

    if (pal_stream_handle_) {
        ret = pal_stream_close(pal_stream_handle_);
        pal_stream_handle_ = NULL;
    }

    if (ret)
        return -EINVAL;
    else
        return ret;
}

int StreamOutPrimary::RouteStream(const std::set<audio_devices_t>& new_devices) {
    int ret = 0, noPalDevices = 0;
    pal_device_id_t * deviceId;
    struct pal_device* deviceIdConfigs;
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();

    ALOGD("StreamOutPrimary::%s: enter ", __func__);

    if (!mInitialized){
        ALOGE("%s: Not initialized, returning error", __func__);
        ret = -EINVAL;
        goto done;
    }

    ALOGD("%s: mAndroidOutDevices %d, mNoOfOutDevices %zu, new_devices %d, num new_devices: %zu",
          __func__, AudioExtn::get_device_types(mAndroidOutDevices),
          mAndroidOutDevices.size(), AudioExtn::get_device_types(new_devices), new_devices.size());

    /* If its the same device as what was already routed to, dont bother */
    if (!AudioExtn::audio_devices_empty(new_devices) && mAndroidOutDevices != new_devices) {
        //re-allocate mPalOutDevice and mPalOutDeviceIds
        if (new_devices.size() != mAndroidOutDevices.size()) {
            deviceId = (pal_device_id_t*) realloc(mPalOutDeviceIds,
                    new_devices.size() * sizeof(pal_device_id_t));
            deviceIdConfigs = (struct pal_device*) realloc(mPalOutDevice,
                    new_devices.size() * sizeof(struct pal_device));
            if (!deviceId || !deviceIdConfigs) {
                ALOGE("%s: Failed to allocate PalOutDeviceIds!", __func__);
                ret = -ENOMEM;
                goto done;
            }
            mPalOutDeviceIds = deviceId;
            mPalOutDevice = deviceIdConfigs;
        }

        noPalDevices = getPalDeviceIds(new_devices, mPalOutDeviceIds);
        ALOGD("%s: noPalDevices: %d , new_devices: %zu", __func__,
                noPalDevices, new_devices.size());

        if (noPalDevices != new_devices.size()) {
            ALOGE("%s: Device count mismatch! Expected: %zu Got: %d", __func__,
                    new_devices.size(), noPalDevices);
            ret = -EINVAL;
            goto done;
        }

        for (int i = 0; i < noPalDevices; i++) {
            mPalOutDevice[i].id = mPalOutDeviceIds[i];
            mPalOutDevice[i].config.sample_rate = mPalOutDevice[0].config.sample_rate;
            mPalOutDevice[i].config.bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
            mPalOutDevice[i].config.ch_info = {0, {0}};
            mPalOutDevice[i].config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM; 
            if ((mPalOutDeviceIds[i] == PAL_DEVICE_OUT_USB_DEVICE) ||
               (mPalOutDeviceIds[i] == PAL_DEVICE_OUT_USB_HEADSET)) {
                mPalOutDevice[i].address.card_id = adevice->usb_card_id_;
                mPalOutDevice[i].address.device_num = adevice->usb_dev_num_;
            }
        }

        mAndroidOutDevices = new_devices;

        if (pal_stream_handle_){
            ret = pal_stream_set_device(pal_stream_handle_, noPalDevices, mPalOutDevice);
            if (!ret)
                for (const auto &dev : mAndroidOutDevices)
                    audio_extn_gef_notify_device_config(dev,
                            config_.channel_mask, config_.sample_rate);
            else
                ALOGE("%s: failed to set device. Error %d", __func__ ,ret);
        }
    }

done:
    ALOGD("StreamOutPrimary::%s: exit %d", __func__, ret);
    return ret;
}

int StreamOutPrimary::SetParameters(struct str_parms *parms) {
    char value[64];
    int ret =  -EINVAL, controller = -1, stream = -1;
    int ret1 = 0;

    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();

    ALOGD("%s: enter ", __func__);

    if (!mInitialized)
        goto error;

    ret = AudioExtn::get_controller_stream_from_params(parms, &controller, &stream);
    if (ret >= 0) {
        adevice->dp_controller = controller;
        adevice->dp_stream = stream;
        ALOGE("%s: plugin device cont %d stream %d",__func__, controller, stream);
    }

    //TBD: check if its offload and check call the following
    ret = AudioExtn::audio_extn_parse_compress_metadata(&config_, &palSndDec, parms, &msample_rate, &mchannels);
    if (ret) {
        ALOGE("parse_compress_metadata Error (%x)", ret);
        goto error;
    }

    ret1 = str_parms_get_str(parms, AUDIO_OFFLOAD_CODEC_DELAY_SAMPLES, value, sizeof(value));
    if (ret1 >= 0 ) {
        gaplessMeta.encoderDelay = atoi(value);
        ALOGD("%s new encoder delay %u", __func__, gaplessMeta.encoderDelay);
    }
    ret1 = str_parms_get_str(parms, AUDIO_OFFLOAD_CODEC_PADDING_SAMPLES, value, sizeof(value));
    if (ret1 >= 0) {
        gaplessMeta.encoderPadding = atoi(value);
        ALOGD("%s padding %u", __func__, gaplessMeta.encoderPadding);
    }
error:
    ALOGE("%s: exit %d", __func__, ret);
    return ret;
}

int StreamOutPrimary::SetVolume(float left , float right) {
    int ret = 0;
    ALOGD("%s: left %f, right %f", __func__, left, right);

    /* free previously cached volume if any */
    if (volume_) {
        free(volume_);
        volume_ = NULL;
    }

    if (left == right) {
        volume_ = (struct pal_volume_data *)malloc(sizeof(struct pal_volume_data)
                    +sizeof(struct pal_channel_vol_kv));
        volume_->no_of_volpair = 1;
        volume_->volume_pair[0].channel_mask = 0x03;
        volume_->volume_pair[0].vol = left;
    } else {
        volume_ = (struct pal_volume_data *)malloc(sizeof(struct pal_volume_data)
                    +sizeof(struct pal_channel_vol_kv) * 2);
        volume_->no_of_volpair = 2;
        volume_->volume_pair[0].channel_mask = 0x01;
        volume_->volume_pair[0].vol = left;
        volume_->volume_pair[1].channel_mask = 0x10;
        volume_->volume_pair[1].vol = right;
    }

    /* if stream is not opened already cache the volume and set on open */
    if (pal_stream_handle_) {
        ret = pal_stream_set_volume(pal_stream_handle_, volume_);
        if (ret) {
            ALOGE("Pal Stream volume Error (%x)", ret);
        }
    }
    return ret;
}

/* Delay in Us */
/* Delay in Us, only to be used for PCM formats */
int64_t StreamOutPrimary::GetRenderLatency(audio_output_flags_t halStreamFlags)
{
    struct pal_stream_attributes streamAttributes_;
    streamAttributes_.type = StreamOutPrimary::GetPalStreamType(halStreamFlags);
    ALOGV("%s:%d type %d", __func__, __LINE__, streamAttributes_.type);
    switch (streamAttributes_.type) {
         case PAL_STREAM_DEEP_BUFFER:
             return DEEP_BUFFER_PLATFORM_DELAY;
         case PAL_STREAM_LOW_LATENCY:
             return LOW_LATENCY_PLATFORM_DELAY;
         case PAL_STREAM_COMPRESSED:
         case PAL_STREAM_PCM_OFFLOAD:
              return PCM_OFFLOAD_PLATFORM_DELAY;
         case PAL_STREAM_ULTRA_LOW_LATENCY:
              return ULL_PLATFORM_DELAY;
         //TODO: Add more usecases/type as in current hal, once they are available in pal
         default:
             return 0;
     }
}

uint64_t StreamOutPrimary::GetFramesWritten(struct timespec *timestamp)
{
    uint64_t signed_frames = 0;
    uint64_t written_frames = 0;
    uint64_t kernel_frames = 0;
    uint64_t dsp_frames = 0;
    uint64_t bt_extra_frames = 0;
    pal_param_bta2dp_t *param_bt_a2dp = NULL;
    size_t size = 0, kernel_buffer_size = 0;
    int32_t ret;

    /* This adjustment accounts for buffering after app processor
     * It is based on estimated DSP latency per use case, rather than exact.
     */
    dsp_frames = StreamOutPrimary::GetRenderLatency(flags_) *
        (streamAttributes_.out_media_config.sample_rate) / 1000000LL;

    if (!timestamp) {
       ALOGE("%s: timestamp NULL", __func__);
       return 0;
    }
    written_frames = total_bytes_written_ / audio_bytes_per_frame(
        audio_channel_count_from_out_mask(config_.channel_mask),
        config_.format);

    /* not querying actual state of buffering in kernel as it would involve an ioctl call
     * which then needs protection, this causes delay in TS query for pcm_offload usecase
     * hence only estimate.
     */
    kernel_buffer_size = fragment_size_ * fragments_;
    kernel_frames = kernel_buffer_size /
        audio_bytes_per_frame(
        audio_channel_count_from_out_mask(config_.channel_mask),
        config_.format);


    // kernel_frames = (kernel_buffer_size - avail) / (bitwidth * channel count);
    if (written_frames >= (kernel_frames + dsp_frames))
        signed_frames = written_frames - (kernel_frames + dsp_frames);

    // Adjustment accounts for A2dp encoder latency with non offload usecases
    // Note: Encoder latency is returned in ms, while platform_render_latency in us.
    if (isDeviceAvailable(PAL_DEVICE_OUT_BLUETOOTH_A2DP)) {
        ret = pal_get_param(PAL_PARAM_ID_BT_A2DP_ENCODER_LATENCY,
                            (void **)&param_bt_a2dp, &size, nullptr);
        if (!ret && param_bt_a2dp) {
            bt_extra_frames = param_bt_a2dp->latency *
                (streamAttributes_.out_media_config.sample_rate) / 1000;
            if (signed_frames >= bt_extra_frames)
                signed_frames -= bt_extra_frames;

        }
    }

    if (signed_frames <= 0) {
       clock_gettime(CLOCK_MONOTONIC, timestamp);
       signed_frames = 0;
    } else {
       *timestamp = writeAt;
    }

    ALOGV("%s signed frames %lld written frames %lld kernel frames %lld dsp frames %lld, bt extra frames %lld",
            __func__, (long long)signed_frames, (long long)written_frames, (long long)kernel_frames,
            (long long)dsp_frames, (long long)bt_extra_frames);

    return signed_frames;
}

int StreamOutPrimary::get_compressed_buffer_size()
{
    int fragment_size = COMPRESS_OFFLOAD_FRAGMENT_SIZE;
    ALOGD("%s:%d config_ %x", __func__, __LINE__, config_.format);
    if(config_.format ==  AUDIO_FORMAT_FLAC ) {
        fragment_size = FLAC_COMPRESS_OFFLOAD_FRAGMENT_SIZE;
        ALOGD("%s:%d aud_fmt_id: 0x%x  FLAC buffer size:%d", __func__, __LINE__,
            streamAttributes_.out_media_config.aud_fmt_id,
            fragment_size);
    } else {
        fragment_size =  COMPRESS_OFFLOAD_FRAGMENT_SIZE;
    }
    return fragment_size;
}

int StreamOutPrimary::get_pcm_buffer_size()
{
    uint8_t channels = audio_channel_count_from_out_mask(config_.channel_mask);
    uint8_t bytes_per_sample = audio_bytes_per_sample(config_.format);
    uint32_t fragment_size = 0;

    ALOGD("%s:%d config_ format:%x, SR %d ch_mask 0x%x",
            __func__, __LINE__, config_.format, config_.sample_rate,
            config_.channel_mask);
    fragment_size = PCM_OFFLOAD_OUTPUT_PERIOD_DURATION *
        config_.sample_rate * bytes_per_sample * channels;
    fragment_size /= 1000;

    if (fragment_size < MIN_PCM_FRAGMENT_SIZE)
        fragment_size = MIN_PCM_FRAGMENT_SIZE;
    else if (fragment_size > MAX_PCM_FRAGMENT_SIZE)
        fragment_size = MAX_PCM_FRAGMENT_SIZE;

    fragment_size = ALIGN(fragment_size, (bytes_per_sample * channels * 32));

    ALOGD("%s: fragment size: %d", __func__, fragment_size);
    return fragment_size;
}

static int voip_get_buffer_size(uint32_t sample_rate)
{
    if (sample_rate == 48000)
        return COMPRESS_VOIP_IO_BUF_SIZE_FB;
    else if (sample_rate == 32000)
        return COMPRESS_VOIP_IO_BUF_SIZE_SWB;
    else if (sample_rate == 16000)
        return COMPRESS_VOIP_IO_BUF_SIZE_WB;
    else
        return COMPRESS_VOIP_IO_BUF_SIZE_NB;

}

uint32_t StreamOutPrimary::GetBufferSize() {
    struct pal_stream_attributes streamAttributes_;

    streamAttributes_.type = StreamOutPrimary::GetPalStreamType(flags_);
    ALOGD("%s:%d type %d", __func__, __LINE__, streamAttributes_.type);
    if (streamAttributes_.type == PAL_STREAM_VOIP_RX) {
        return voip_get_buffer_size(config_.sample_rate);
    } else if (streamAttributes_.type == PAL_STREAM_COMPRESSED) {
        return get_compressed_buffer_size();
    } else if (streamAttributes_.type == PAL_STREAM_PCM_OFFLOAD
              || streamAttributes_.type == PAL_STREAM_DEEP_BUFFER) {
        return get_pcm_buffer_size();
    } else if (streamAttributes_.type == PAL_STREAM_LOW_LATENCY) {
        return LOW_LATENCY_PLAYBACK_PERIOD_SIZE *
            audio_bytes_per_frame(
                    audio_channel_count_from_out_mask(config_.channel_mask),
                    config_.format);
    } else if (streamAttributes_.type == PAL_STREAM_ULTRA_LOW_LATENCY) {
        return ULL_PERIOD_SIZE * ULL_PERIOD_MULTIPLIER *
            audio_bytes_per_frame(
                    audio_channel_count_from_out_mask(config_.channel_mask),
                    config_.format);
    } else {
       return BUF_SIZE_PLAYBACK * NO_OF_BUF;
    }
}

int StreamOutPrimary::Open() {
    int ret = -EINVAL;
    uint8_t channels = 0;
    struct pal_channel_info ch_info = {0, {0}};
    uint32_t inBufSize = 0;
    uint32_t outBufSize = 0;
    uint32_t inBufCount = NO_OF_BUF;
    uint32_t outBufCount = NO_OF_BUF;

    if (!mInitialized) {
        ALOGE("%s: Not initialized, returning error", __func__);
        goto error_open;
    }
    ALOGD("%s: no_of_devices %zu", __func__, mAndroidOutDevices.size());
    //need to convert channel mask to pal channel mask
    // Stream channel mask
    channels = audio_channel_count_from_out_mask(config_.channel_mask);

    ch_info.channels = channels;
    ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;
    if (ch_info.channels > 1)
        ch_info.ch_map[1] = PAL_CHMAP_CHANNEL_FR;

    streamAttributes_.type = StreamOutPrimary::GetPalStreamType(flags_);
    streamAttributes_.flags = (pal_stream_flags_t)0;
    streamAttributes_.direction = PAL_AUDIO_OUTPUT;
    streamAttributes_.out_media_config.sample_rate = config_.sample_rate;
    streamAttributes_.out_media_config.bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
    streamAttributes_.out_media_config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
    streamAttributes_.out_media_config.ch_info = ch_info;

    if (streamAttributes_.type == PAL_STREAM_COMPRESSED) {
        streamAttributes_.flags = (pal_stream_flags_t)(PAL_STREAM_FLAG_NON_BLOCKING);
        if (config_.offload_info.format == 0)
            config_.offload_info.format = config_.format;
        if (config_.offload_info.sample_rate == 0)
            config_.offload_info.sample_rate = config_.sample_rate;
        streamAttributes_.out_media_config.sample_rate = config_.offload_info.sample_rate;
        if (msample_rate)
            streamAttributes_.out_media_config.sample_rate = msample_rate;
        if (mchannels)
            streamAttributes_.out_media_config.ch_info.channels = mchannels;
        streamAttributes_.out_media_config.aud_fmt_id = getFormatId.at(config_.format & AUDIO_FORMAT_MAIN_MASK);
    } else if (streamAttributes_.type == PAL_STREAM_PCM_OFFLOAD ||
               streamAttributes_.type == PAL_STREAM_DEEP_BUFFER) {
        halInputFormat = config_.format;
        halOutputFormat = (audio_format_t)(getAlsaSupportedFmt.at(halInputFormat));
        ALOGD("halInputFormat %d halOutputFormat %d", halInputFormat, halOutputFormat);
        streamAttributes_.out_media_config.bit_width = format_to_bitwidth_table[halOutputFormat];
        if (streamAttributes_.out_media_config.bit_width == 0)
            streamAttributes_.out_media_config.bit_width = 16;
    } else if ((streamAttributes_.type == PAL_STREAM_ULTRA_LOW_LATENCY) &&
            (usecase_ == USECASE_AUDIO_PLAYBACK_MMAP)) {
        streamAttributes_.flags = (pal_stream_flags_t)(PAL_STREAM_FLAG_MMAP_NO_IRQ);
    } else if ((streamAttributes_.type == PAL_STREAM_ULTRA_LOW_LATENCY) &&
            (usecase_ == USECASE_AUDIO_PLAYBACK_ULL)) {
        streamAttributes_.flags = (pal_stream_flags_t)(PAL_STREAM_FLAG_MMAP);
    }

    ALOGD("channels %d samplerate %d format id %d, stream type %d  stream bitwidth %d",
           streamAttributes_.out_media_config.ch_info.channels, streamAttributes_.out_media_config.sample_rate,
           streamAttributes_.out_media_config.aud_fmt_id, streamAttributes_.type,
           streamAttributes_.out_media_config.bit_width);
    ALOGD("msample_rate %d mchannels %d", msample_rate, mchannels);
    ALOGD("mNoOfOutDevices %zu", mAndroidOutDevices.size());
    ret = pal_stream_open (&streamAttributes_,
                          mAndroidOutDevices.size(),
                          mPalOutDevice,
                          0,
                          NULL,
                          &pal_callback,
                          (void *)this,
                          &pal_stream_handle_);

    ALOGD("%s:(%x:ret)%d",__func__,ret, __LINE__);
    if (ret) {
        ALOGE("Pal Stream Open Error (%x)", ret);
        ret = -EINVAL;
        goto error_open;
    }

    //TODO: Remove below code, once pal_stream_open is moved to
    //adev_open_output_stream
    if (streamAttributes_.type == PAL_STREAM_COMPRESSED) {
        pal_param_payload *param_payload = nullptr;
        param_payload = (pal_param_payload *) calloc (1,
                                              sizeof(pal_param_payload) +
                                              sizeof(pal_snd_dec_t));

        if (!param_payload) {
            ALOGE("%s:%d calloc failed for size %zu", __func__, __LINE__,
                   sizeof(pal_param_payload) + sizeof(pal_snd_dec_t));
        } else {
            param_payload->payload_size = sizeof(pal_snd_dec_t);
            memcpy(param_payload->payload, &palSndDec, param_payload->payload_size);

            ret = pal_stream_set_param(pal_stream_handle_,
                                       PAL_PARAM_ID_CODEC_CONFIGURATION,
                                       param_payload);
            if (ret)
                ALOGE("Pal Set Param Error (%x)", ret);
            free(param_payload);

        }
    }

    if (usecase_ == USECASE_AUDIO_PLAYBACK_MMAP) {
        outBufSize = MMAP_PERIOD_SIZE * audio_bytes_per_frame(
                    audio_channel_count_from_out_mask(config_.channel_mask),
                    config_.format);
        outBufCount = MMAP_PERIOD_COUNT_DEFAULT;
    } else if (usecase_ == USECASE_AUDIO_PLAYBACK_ULL) {
        outBufSize = ULL_PERIOD_SIZE * audio_bytes_per_frame(
                    audio_channel_count_from_out_mask(config_.channel_mask),
                    config_.format);
        outBufCount = ULL_PERIOD_COUNT_DEFAULT;
    } else
        outBufSize = StreamOutPrimary::GetBufferSize();

    if (usecase_ == USECASE_AUDIO_PLAYBACK_LOW_LATENCY)
        outBufCount = LOW_LATENCY_PLAYBACK_PERIOD_COUNT;
    else if (usecase_ == USECASE_AUDIO_PLAYBACK_OFFLOAD2)
         outBufCount = PCM_OFFLOAD_PLAYBACK_PERIOD_COUNT;
    else if (usecase_ == USECASE_AUDIO_PLAYBACK_DEEP_BUFFER)
         outBufCount = DEEP_BUFFER_PLAYBACK_PERIOD_COUNT;

    if (halInputFormat != halOutputFormat) {
        convertBufSize =  PCM_OFFLOAD_OUTPUT_PERIOD_DURATION *
                         config_.sample_rate * audio_bytes_per_frame(
                         audio_channel_count_from_out_mask(config_.channel_mask),
                         halOutputFormat);
        convertBufSize /= 1000;
        convertBuffer = realloc(convertBuffer, convertBufSize);
        if (!convertBuffer) {
            ret = -ENOMEM;
            ALOGE("convert Buffer allocation failed. ret %d", ret);
            goto error_open;
        }
        outBufSize = convertBufSize;
        ALOGD("convert buffer allocated for size %d", convertBufSize);
    }

    fragment_size_ = outBufSize;
    fragments_ = outBufCount;

    ALOGD("%s:fragment_size_ %d fragments_ %d",__func__, fragment_size_, fragments_);

    ret = pal_stream_set_buffer_size(pal_stream_handle_, (size_t*)&inBufSize,
            inBufCount, (size_t*)&outBufSize, outBufCount);
    if (ret) {
        ALOGE("Pal Stream set buffer size Error  (%x)", ret);
    }

error_open:
    return ret;
}


int StreamOutPrimary::GetFrames(uint64_t *frames) {
    int ret = 0;
    if (!pal_stream_handle_) {
        ALOGV("%s: pal_stream_handle_ NULL", __func__);
        *frames = 0;
        return 0;
    }
    pal_session_time tstamp;
    uint64_t timestamp = 0;
    ret = pal_get_timestamp(pal_stream_handle_, &tstamp);
    if (ret != 0) {
       ALOGE("%s: pal_get_timestamp failed %d", __func__, ret);
       goto exit;
    }
    timestamp = (uint64_t)tstamp.session_time.value_msw;
    timestamp = timestamp  << 32 | tstamp.session_time.value_lsw;
    ALOGV("%s: session msw %u", __func__, tstamp.session_time.value_msw);
    ALOGV("%s: session lsw %u", __func__, tstamp.session_time.value_lsw);
    ALOGV("%s: session timespec %lld", __func__, ((long long) timestamp));
    timestamp *= (streamAttributes_.out_media_config.sample_rate);
    ALOGV("%s: timestamp %lld", __func__, ((long long) timestamp));
    *frames = timestamp/1000000;
exit:
    return ret;
}

int StreamOutPrimary::GetOutputUseCase(audio_output_flags_t halStreamFlags)
{
    // TODO: just covered current supported usecases in PAL
    // need to update other usecases in future
    int usecase = USECASE_AUDIO_PLAYBACK_LOW_LATENCY;
    if (halStreamFlags & AUDIO_OUTPUT_FLAG_VOIP_RX)
        usecase = USECASE_AUDIO_PLAYBACK_VOIP;
    else if ((halStreamFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) ||
             (halStreamFlags == AUDIO_OUTPUT_FLAG_DIRECT)) {
        if (halStreamFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
            usecase = USECASE_AUDIO_PLAYBACK_OFFLOAD;
        else
            usecase = USECASE_AUDIO_PLAYBACK_OFFLOAD2;
    } else if (halStreamFlags & AUDIO_OUTPUT_FLAG_RAW)
        usecase = USECASE_AUDIO_PLAYBACK_ULL;
    else if (halStreamFlags & AUDIO_OUTPUT_FLAG_FAST)
        usecase = USECASE_AUDIO_PLAYBACK_LOW_LATENCY;
    else if (halStreamFlags & AUDIO_OUTPUT_FLAG_DEEP_BUFFER)
        usecase = USECASE_AUDIO_PLAYBACK_DEEP_BUFFER;
    else if (halStreamFlags & AUDIO_OUTPUT_FLAG_MMAP_NOIRQ)
        usecase = USECASE_AUDIO_PLAYBACK_MMAP;

    return usecase;
}

ssize_t StreamOutPrimary::Write(const void *buffer, size_t bytes) {
    int ret = 0;
    struct pal_buffer palBuffer;
    int local_bytes_written = 0;
    uint32_t frames;
    bool is_perf_lock_acquired = false;

    palBuffer.buffer = (void*)buffer;
    palBuffer.size = bytes;
    palBuffer.offset = 0;

    ALOGV("%s: handle_ %x Bytes:(%zu)", __func__, handle_, bytes);
    is_perf_lock_acquired = AcquirePerfLock();

    if (!pal_stream_handle_) {
        ATRACE_BEGIN("hal: pal_stream_open");
        ret = Open();
        ATRACE_END();
        if (ret) {
            ALOGE("%s: failed to open stream.", __func__);
            return -EINVAL;
        }
    }

    if (!stream_started_) {
        /* set cached volume if any, dont return failure back up */
        if (volume_) {
            ret = pal_stream_set_volume(pal_stream_handle_, volume_);
            if (ret) {
                ALOGE("Pal Stream volume Error (%x)", ret);
            }
        }

        ATRACE_BEGIN("hal: pal_stream_start");
        ret = pal_stream_start(pal_stream_handle_);
        if (ret) {
            ALOGE("%s:failed to start stream. ret=%d", __func__, ret);
            pal_stream_close(pal_stream_handle_);
            pal_stream_handle_ = NULL;
            ATRACE_END();
            goto exit;
        }

        stream_started_ = true;

        if (CheckOffloadEffectsType(streamAttributes_.type)) {
            ret = StartOffloadEffects(handle_, pal_stream_handle_);
        }

        if (CheckOffloadEffectsType(streamAttributes_.type)) {
            ret = StartOffloadVisualizer(handle_, pal_stream_handle_);
        }

        ATRACE_END();
    }
    if (streamAttributes_.type == PAL_STREAM_COMPRESSED &&
                                      sendGaplessMetadata) {
        pal_param_payload *param_payload = nullptr;
        param_payload = (pal_param_payload *) calloc (1,
                                              sizeof(pal_param_payload) +
                                              sizeof(struct pal_compr_gapless_mdata));
        if (!param_payload) {
            ALOGE("%s:%d calloc failed for size %zu", __func__, __LINE__,
                   sizeof(pal_param_payload) + sizeof(struct pal_compr_gapless_mdata));
        } else {
            ALOGD("%s sending gapless metadata", __func__);
            param_payload->payload_size = sizeof(struct pal_compr_gapless_mdata);
            memcpy(param_payload->payload, &gaplessMeta, param_payload->payload_size);

            ret = pal_stream_set_param(pal_stream_handle_,
                                       PAL_PARAM_ID_GAPLESS_MDATA,
                                       param_payload);
            if (ret)
                ALOGE("PAL set param for gapless failed, error (%x)", ret);
            free(param_payload);
        }
        sendGaplessMetadata = false;
    }

    ATRACE_BEGIN("hal: pal_stream_write");
    if (halInputFormat != halOutputFormat && convertBuffer != NULL) {
        frames = bytes / (format_to_bitwidth_table[halInputFormat]/8);
        memcpy_by_audio_format(convertBuffer, halOutputFormat, buffer, halInputFormat,
                               frames);
        palBuffer.buffer = convertBuffer;
        palBuffer.size = frames * (format_to_bitwidth_table[halOutputFormat]/8);
        local_bytes_written = pal_stream_write(pal_stream_handle_, &palBuffer);
        local_bytes_written = (local_bytes_written * (format_to_bitwidth_table[halInputFormat]/8)) /
                               (format_to_bitwidth_table[halOutputFormat]/8);
    } else {
        local_bytes_written = pal_stream_write(pal_stream_handle_, &palBuffer);
    }
    total_bytes_written_ += local_bytes_written;
    clock_gettime(CLOCK_MONOTONIC, &writeAt);
    ATRACE_END();

exit:
    if (is_perf_lock_acquired)
        ReleasePerfLock();
    return local_bytes_written;
}

bool StreamOutPrimary::CheckOffloadEffectsType(pal_stream_type_t pal_stream_type) {
    if (pal_stream_type == PAL_STREAM_COMPRESSED  ||
        pal_stream_type == PAL_STREAM_PCM_OFFLOAD) {
        return true;
    }

    return false;
}

int StreamOutPrimary::StartOffloadEffects(
                                    audio_io_handle_t ioHandle,
                                    pal_stream_handle_t* pal_stream_handle) {
    int ret  = 0;
    if (fnp_offload_effect_start_output_) {
        ret = fnp_offload_effect_start_output_(ioHandle, pal_stream_handle);
        if (ret) {
            ALOGE("%s: failed to start offload effect.", __func__);
        }
    } else {
        ALOGE("%s: function pointer is null.", __func__);
        return -EINVAL;
    }

    return ret;
}

int StreamOutPrimary::StopOffloadEffects(
                                    audio_io_handle_t ioHandle,
                                    pal_stream_handle_t* pal_stream_handle) {
    int ret  = 0;
    if (fnp_offload_effect_stop_output_) {
        ret = fnp_offload_effect_stop_output_(ioHandle, pal_stream_handle);
        if (ret) {
            ALOGE("%s: failed to stop offload effect.\n", __func__);
        }
    } else {
        ALOGE("%s: function pointer is null.", __func__);
        return -EINVAL;
    }

    return ret;
}


int StreamOutPrimary::StartOffloadVisualizer(
                                    audio_io_handle_t ioHandle,
                                    pal_stream_handle_t* pal_stream_handle) {
    int ret  = 0;
    if (fnp_visualizer_start_output_) {
        ret = fnp_visualizer_start_output_(ioHandle, pal_stream_handle);
        if (ret) {
            ALOGE("%s: failed to visualizer_start.", __func__);
        }
    } else {
        ALOGE("%s: function pointer is null.", __func__);
        return -EINVAL;
    }

    return ret;
}

int StreamOutPrimary::StopOffloadVisualizer(
                                    audio_io_handle_t ioHandle,
                                    pal_stream_handle_t* pal_stream_handle) {
    int ret  = 0;
    if (fnp_visualizer_stop_output_) {
        ret = fnp_visualizer_stop_output_(ioHandle, pal_stream_handle);
        if (ret) {
            ALOGE("%s: failed to visualizer_stop.\n", __func__);
        }
    } else {
        ALOGE("%s: function pointer is null.", __func__);
        return -EINVAL;
    }

    return ret;
}

StreamOutPrimary::StreamOutPrimary(
                        audio_io_handle_t handle,
                        const std::set<audio_devices_t> &devices,
                        audio_output_flags_t flags,
                        struct audio_config *config,
                        const char *address __unused,
                        offload_effects_start_output start_offload_effect,
                        offload_effects_stop_output stop_offload_effect,
                        visualizer_hal_start_output visualizer_start_output,
                        visualizer_hal_stop_output visualizer_stop_output):
    StreamPrimary(handle, devices, config),
    mAndroidOutDevices(devices),
    flags_(flags)
{
    stream_ = std::shared_ptr<audio_stream_out> (new audio_stream_out());
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    mInitialized = false;
    int noPalDevices = 0;
    int ret = 0;

    if (!stream_) {
        ALOGE("%s: No memory allocated for stream_", __func__);
        goto error;
    }
    ALOGE("%s: enter: handle (%x) format(%#x) sample_rate(%d) channel_mask(%#x) devices(%zu) flags(%#x)\
          address(%s)", __func__, handle, config->format, config->sample_rate, config->channel_mask,
          mAndroidOutDevices.size(), flags, address);

    //TODO: check if USB device is connected or not
    if (AudioExtn::audio_devices_cmp(mAndroidOutDevices, audio_is_usb_out_device)){
        if (!config->sample_rate) {
            // get capability from device of USB
            pal_param_device_capability_t *device_cap_query = new pal_param_device_capability_t();
            dynamic_media_config_t dynamic_media_config;
            size_t payload_size = 0;
            device_cap_query->id = PAL_DEVICE_OUT_USB_DEVICE;
            device_cap_query->addr.card_id = adevice->usb_card_id_;
            device_cap_query->addr.device_num = adevice->usb_dev_num_;
            device_cap_query->config = &dynamic_media_config;
            device_cap_query->is_playback = true;
            ret = pal_get_param(PAL_PARAM_ID_DEVICE_CAPABILITY,
                                (void **)&device_cap_query,
                                &payload_size, nullptr);
            delete device_cap_query;

            config->sample_rate = dynamic_media_config.sample_rate;
            config->channel_mask = dynamic_media_config.mask;
            config->format = (audio_format_t)dynamic_media_config.format;
            memcpy(&config_, config, sizeof(struct audio_config));
            ALOGI("%s: sample rate = %#x channel_mask=%#x fmt=%#x %d",
                  __func__, config->sample_rate, config->channel_mask,
                  config->format, __LINE__);

        }
    }

    if (AudioExtn::audio_devices_cmp(mAndroidOutDevices, AUDIO_DEVICE_OUT_AUX_DIGITAL)){
        ALOGD("AUDIO_DEVICE_OUT_AUX_DIGITAL and DIRECT | OFFLOAD, check hdmi caps");
        if (config->sample_rate == 0) {
            config->sample_rate = DEFAULT_OUTPUT_SAMPLING_RATE;
            config_.sample_rate = DEFAULT_OUTPUT_SAMPLING_RATE;
        }
        if (config->channel_mask == AUDIO_CHANNEL_NONE) {
            config->channel_mask = AUDIO_CHANNEL_OUT_5POINT1;
            config_.channel_mask = AUDIO_CHANNEL_OUT_5POINT1;
        }
        if (config->format == AUDIO_FORMAT_DEFAULT) {
            config->format = AUDIO_FORMAT_PCM_16_BIT;
            config_.format = AUDIO_FORMAT_PCM_16_BIT;
        }
    }

    usecase_ = GetOutputUseCase(flags);
    if (address) {
        strlcpy((char *)&address_, address, AUDIO_DEVICE_MAX_ADDRESS_LEN);
    } else {
        ALOGD("%s: invalid address", __func__);
    }

    fnp_offload_effect_start_output_ = start_offload_effect;
    fnp_offload_effect_stop_output_ = stop_offload_effect;

    fnp_visualizer_start_output_ = visualizer_start_output;
    fnp_visualizer_stop_output_ = visualizer_stop_output;

    writeAt.tv_sec = 0;
    writeAt.tv_nsec = 0;
    total_bytes_written_ = 0;
    convertBuffer = NULL;

    if (mAndroidOutDevices.empty())
        mAndroidOutDevices.insert(AUDIO_DEVICE_OUT_DEFAULT);
    ALOGD("%s: No of Android devices %zu", __func__, mAndroidOutDevices.size());

    mPalOutDeviceIds = new pal_device_id_t[mAndroidOutDevices.size()];
    if (!mPalOutDeviceIds) {
           goto error;
    }

    noPalDevices = getPalDeviceIds(mAndroidOutDevices, mPalOutDeviceIds);
    if (noPalDevices != mAndroidOutDevices.size()) {
        ALOGE("%s: mismatched pal no of devices %d and hal devices %zu", __func__, noPalDevices, mAndroidOutDevices.size());
        goto error;
    }

    mPalOutDevice = new pal_device[mAndroidOutDevices.size()];
    if (!mPalOutDevice) {
        goto error;
    }

    /* TODO: how to update based on stream parameters and see if device is supported */
    for (int i = 0; i < mAndroidOutDevices.size(); i++) {
        mPalOutDevice[i].id = mPalOutDeviceIds[i];
        if (AudioExtn::audio_devices_cmp(mAndroidOutDevices, audio_is_usb_out_device))
            mPalOutDevice[i].config.sample_rate = config_.sample_rate;
        else
            mPalOutDevice[i].config.sample_rate = DEFAULT_OUTPUT_SAMPLING_RATE;
        mPalOutDevice[i].config.bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
        mPalOutDevice[i].config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM; // TODO: need to convert this from output format
        ALOGI("%s: device rate = %#x width=%#x fmt=%#x",
            __func__, mPalOutDevice[i].config.sample_rate,
            mPalOutDevice[i].config.bit_width,
            mPalOutDevice[i].config.aud_fmt_id);
            mPalOutDevice[i].config.ch_info = {0, {0}};
        if ((mPalOutDeviceIds[i] == PAL_DEVICE_OUT_USB_DEVICE) ||
           (mPalOutDeviceIds[i] == PAL_DEVICE_OUT_USB_HEADSET)) {
            mPalOutDevice[i].address.card_id = adevice->usb_card_id_;
            mPalOutDevice[i].address.device_num = adevice->usb_dev_num_;
        }
    }

    if (flags & AUDIO_OUTPUT_FLAG_MMAP_NOIRQ) {
        stream_.get()->start = astream_out_mmap_noirq_start;
        stream_.get()->stop = astream_out_mmap_noirq_stop;
        stream_.get()->create_mmap_buffer = astream_out_create_mmap_buffer;
        stream_.get()->get_mmap_position = astream_out_get_mmap_position;
    }
    (void)FillHalFnPtrs();
    mInitialized = true;
    for(auto dev : mAndroidOutDevices)
        audio_extn_gef_notify_device_config(dev, config_.channel_mask, config_.sample_rate);

error:
    return;
}

StreamOutPrimary::~StreamOutPrimary() {
    ALOGD("%s: close stream, handle(%x), pal_stream_handle (%p)", __func__,
          handle_, pal_stream_handle_);

    if (pal_stream_handle_) {
        if (CheckOffloadEffectsType(streamAttributes_.type)) {
            StopOffloadEffects(handle_, pal_stream_handle_);
        }

        if (CheckOffloadEffectsType(streamAttributes_.type)) {
            StopOffloadVisualizer(handle_, pal_stream_handle_);
        }

        pal_stream_close(pal_stream_handle_);
        pal_stream_handle_ = nullptr;
    }
    if (convertBuffer)
        free(convertBuffer);
}

bool StreamInPrimary::isDeviceAvailable(pal_device_id_t deviceId)
{
    for (int i = 0; i < mAndroidInDevices.size(); i++) {
        if (mPalInDevice[i].id == deviceId)
            return true;
    }

    return false;
}

int StreamInPrimary::Stop() {
    int ret = -ENOSYS;

    if (usecase_ == USECASE_AUDIO_RECORD_MMAP &&
            pal_stream_handle_ && stream_started_) {

        ret = pal_stream_stop(pal_stream_handle_);
        if (ret == 0)
            stream_started_ = false;
    }
    return ret;
}

int StreamInPrimary::Start() {
    int ret = -ENOSYS;

    if (usecase_ == USECASE_AUDIO_RECORD_MMAP &&
            pal_stream_handle_ && !stream_started_) {

        ret = pal_stream_start(pal_stream_handle_);
        if (ret == 0)
            stream_started_ = true;
    }
    return ret;
}

int StreamInPrimary::CreateMmapBuffer(int32_t min_size_frames,
        struct audio_mmap_buffer_info *info)
{
    int ret;
    struct pal_mmap_buffer palMmapBuf;

    if (pal_stream_handle_) {
        ALOGE("%s: pal handle already created\n", __func__);
        return -EINVAL;
    }

    ret = Open();
    if (ret) {
        ALOGE("%s: failed to open stream.", __func__);
        return ret;
    }
    ret = pal_stream_create_mmap_buffer(pal_stream_handle_,
            min_size_frames, &palMmapBuf);
    if (ret) {
        ALOGE("%s: failed to create mmap buffer: %d", __func__, ret);
        Standby();
        return ret;
    }
    info->shared_memory_address = palMmapBuf.buffer;
    info->shared_memory_fd = palMmapBuf.fd;
    info->buffer_size_frames = palMmapBuf.buffer_size_frames;
    info->burst_size_frames = palMmapBuf.burst_size_frames;
    info->flags = (audio_mmap_buffer_flag)palMmapBuf.flags;

    return ret;
}

int StreamInPrimary::GetMmapPosition(struct audio_mmap_position *position)
{
    struct pal_mmap_position pal_mmap_pos;
    int32_t ret = 0;

    if (pal_stream_handle_ == nullptr) {
        ALOGE("%s: pal handle is null\n", __func__);
        return -EINVAL;
    }

    ret = pal_stream_get_mmap_position(pal_stream_handle_, &pal_mmap_pos);
    if (ret) {
        ALOGE("%s: failed to get mmap position %d\n", __func__, ret);
        return ret;
    }
    position->position_frames = pal_mmap_pos.position_frames;
    position->time_nanoseconds = pal_mmap_pos.time_nanoseconds;

    return 0;
}

int StreamInPrimary::Standby() {
    int ret = 0;
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();

    if (pal_stream_handle_) {
        if (!is_st_session) {
            ret = pal_stream_stop(pal_stream_handle_);
        } else {
            ret = pal_stream_set_param(pal_stream_handle_,
                PAL_PARAM_ID_STOP_BUFFERING, nullptr);
            if (adevice->num_va_sessions_ > 0) {
                adevice->num_va_sessions_--;
            }
        }
    }
    effects_applied_ = true;
    stream_started_ = false;

    if (pal_stream_handle_ && !is_st_session) {
        ret = pal_stream_close(pal_stream_handle_);
        pal_stream_handle_ = NULL;
    }

    if (ret)
        return -EINVAL;
    else
        return ret;
}

int StreamInPrimary::addRemoveAudioEffect(const struct audio_stream *stream __unused,
                                   effect_handle_t effect,
                                   bool enable)
{
    int status = 0;
    effect_descriptor_t desc;

    status = (*effect)->get_descriptor(effect, &desc);
    if (status != 0)
        return status;


    if (source_ == AUDIO_SOURCE_VOICE_COMMUNICATION) {
        if (memcmp(&desc.type, FX_IID_AEC, sizeof(effect_uuid_t)) == 0) {
            if (enable) {
                if (isECEnabled) {
                    ALOGE("%s: EC already enabled", __func__);
                    goto exit;
                } else if (isNSEnabled) {
                    ALOGV("%s: Got EC enable and NS is already active. Enabling ECNS", __func__);
                    status = pal_add_remove_effect(pal_stream_handle_,PAL_AUDIO_EFFECT_ECNS,true);
                    isECEnabled = true;
                    goto exit;
                } else {
                    ALOGV("%s: Got EC enable. Enabling EC", __func__);
                    status = pal_add_remove_effect(pal_stream_handle_,PAL_AUDIO_EFFECT_EC,true);
                    isECEnabled = true;
                    goto exit;
               }
            } else {
                if (isECEnabled) {
                    if (isNSEnabled) {
                        ALOGV("%s: ECNS is running. Disabling EC and enabling NS alone", __func__);
                        status = pal_add_remove_effect(pal_stream_handle_,PAL_AUDIO_EFFECT_NS,true);
                        isECEnabled = false;
                        goto exit;
                    } else {
                        ALOGV("%s: EC is running. Disabling it", __func__);

                        status = pal_add_remove_effect(pal_stream_handle_,PAL_AUDIO_EFFECT_ECNS,false);

                        isECEnabled = false;
                        goto exit;
                    }
                } else {
                    ALOGE("%s: EC is not enabled", __func__);
                    goto exit;
               }
            }
        }

        if (memcmp(&desc.type, FX_IID_NS, sizeof(effect_uuid_t)) == 0) {
            if (enable) {
                if (isNSEnabled) {
                    ALOGE("%s: NS already enabled", __func__);
                    goto exit;
                } else if (isECEnabled) {
                    ALOGV("%s: Got NS enable and EC is already active. Enabling ECNS", __func__);
                    status = pal_add_remove_effect(pal_stream_handle_,PAL_AUDIO_EFFECT_ECNS,true);
                    isNSEnabled = true;
                    goto exit;
                } else {
                    ALOGV("%s: Got NS enable. Enabling NS", __func__);
                    status = pal_add_remove_effect(pal_stream_handle_,PAL_AUDIO_EFFECT_NS,true);
                    isNSEnabled = true;
                    goto exit;
               }
            } else {
                if (isNSEnabled) {
                    if (isECEnabled) {
                        ALOGV("%s: ECNS is running. Disabling NS and enabling EC alone", __func__);
                        status = pal_add_remove_effect(pal_stream_handle_,PAL_AUDIO_EFFECT_EC,true);
                        isNSEnabled = false;
                        goto exit;
                    } else {
                        ALOGV("%s: NS is running. Disabling it", __func__);

                        status = pal_add_remove_effect(pal_stream_handle_,PAL_AUDIO_EFFECT_ECNS,false);

                        isNSEnabled = false;
                        goto exit;
                    }
                } else {
                    ALOGE("%s: NS is not enabled", __func__);
                    goto exit;
               }
            }
        }
    }
exit:
    if (status) {
       effects_applied_ = false;
    } else
       effects_applied_ = true;

    return 0;
}


int StreamInPrimary::SetGain(float gain) {
    struct pal_volume_data* volume;
    int ret = 0;

    volume = (struct pal_volume_data*)malloc(sizeof(uint32_t)
                +sizeof(struct pal_channel_vol_kv));
    volume->no_of_volpair = 1;
    volume->volume_pair[0].channel_mask = 0x03;
    volume->volume_pair[0].vol = gain;
    ret = pal_stream_set_volume(pal_stream_handle_, volume);

    free(volume);
    if (ret) {
        ALOGE("Pal Stream volume Error (%x)", ret);
    }

    return ret;
}

int StreamInPrimary::RouteStream(const std::set<audio_devices_t>& new_devices) {
    bool is_empty, is_input;
    int ret = 0, noPalDevices = 0;
    pal_device_id_t * deviceId;
    struct pal_device* deviceIdConfigs;
    struct pal_channel_info ch_info = {0, {0}};
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();

    ALOGD("StreamInPrimary::%s: enter ", __func__);

    if (!mInitialized){
        ALOGE("%s: Not initialized, returning error", __func__);
        ret = -EINVAL;
        goto done;
    }

    ALOGD("%s: mAndroidInDevices %d, mNoOfInDevices %zu, new_devices %d, num new_devices: %zu",
          __func__, AudioExtn::get_device_types(mAndroidInDevices),
          mAndroidInDevices.size(), AudioExtn::get_device_types(new_devices), new_devices.size());

    // TBD: Hard code number of channels to 2 for now.
    // channels = audio_channel_count_from_out_mask(config_.channel_mask);
    // need to convert channel mask to pal channel mask
    ch_info.channels = 2;
    ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;
    if (ch_info.channels > 1 )
        ch_info.ch_map[1] = PAL_CHMAP_CHANNEL_FR;

    is_empty = AudioExtn::audio_devices_empty(new_devices);
    is_input = AudioExtn::audio_devices_cmp(new_devices, audio_is_input_device);

    /* If its the same device as what was already routed to, dont bother */
    if (!is_empty && is_input && mAndroidInDevices != new_devices) {
        //re-allocate mPalOutDevice and mPalOutDeviceIds
        if (new_devices.size() != mAndroidInDevices.size()) {
            deviceId = (pal_device_id_t*) realloc(mPalInDeviceIds,
                    new_devices.size() * sizeof(pal_device_id_t));
            deviceIdConfigs = (struct pal_device*) realloc(mPalInDevice,
                    new_devices.size() * sizeof(struct pal_device));
            if (!deviceId || !deviceIdConfigs) {
                ret = -ENOMEM;
                goto done;
            }
            mPalInDeviceIds = deviceId;
            mPalInDevice = deviceIdConfigs;
        }
        noPalDevices = getPalDeviceIds(new_devices, mPalInDeviceIds);
        ALOGD("%s: noPalDevices: %d , new_devices: %zu", __func__,
                noPalDevices, new_devices.size());
        if (noPalDevices != new_devices.size()) {
            ALOGE("%s: Device count mismatch! Expected: %d Got: %zu", __func__, noPalDevices, new_devices.size());
            ret = -EINVAL;
            goto done;
        }

        for (int i = 0; i < noPalDevices; i++) {
            mPalInDevice[i].id = mPalInDeviceIds[i];
            mPalInDevice[i].config.sample_rate = mPalInDevice[0].config.sample_rate;
            mPalInDevice[i].config.bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
            mPalInDevice[i].config.ch_info = ch_info;
            mPalInDevice[i].config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
            if ((mPalInDeviceIds[i] == PAL_DEVICE_IN_USB_DEVICE) ||
               (mPalInDeviceIds[i] == PAL_DEVICE_IN_USB_HEADSET)) {
                mPalInDevice[i].address.card_id = adevice->usb_card_id_;
                mPalInDevice[i].address.device_num = adevice->usb_dev_num_;
            }
        }

        mAndroidInDevices = new_devices;

        if(pal_stream_handle_)
            ret = pal_stream_set_device(pal_stream_handle_, noPalDevices, mPalInDevice);
    }

done:
   ALOGD("StreamInPrimary::%s: exit %d", __func__, ret);
   return ret;
}

int StreamInPrimary::SetParameters(const char* kvpairs) {
    struct str_parms *parms = (str_parms *)NULL;
    int ret = -EINVAL;

    ALOGD("%s: enter: kvpairs=%s", __func__, kvpairs);
    if(!mInitialized)
        goto exit;

    parms = str_parms_create_str(kvpairs);
    if (!parms)
        goto exit;

exit:
   ALOGE("%s: exit %d", __func__, ret);
   return ret;
}

int StreamInPrimary::Open() {
    int ret = -EINVAL;
    uint8_t channels = 0;
    struct pal_channel_info ch_info = {0, {0}};
    uint32_t inBufSize = 0;
    uint32_t outBufSize = 0;
    uint32_t inBufCount = NO_OF_BUF;
    uint32_t outBufCount = NO_OF_BUF;
    void *handle = nullptr;

    if (!mInitialized) {
        ALOGE("%s: Not initialized, returning error", __func__);
        goto error_open;
    }

    handle = audio_extn_sound_trigger_check_and_get_session(this);
    if (handle) {
        ALOGV("Found existing pal stream handle associated with capture handle");
        pal_stream_handle_ = (pal_stream_handle_t *)handle;
        goto set_buff_size;
    }

    channels = audio_channel_count_from_in_mask(config_.channel_mask);
    if (channels == 0) {
       ALOGE("invalid channel count");
       return -EINVAL;
    }
    //need to convert channel mask to pal channel mask
    if (channels == 8) {
      ch_info.channels = 8;
      ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;
      ch_info.ch_map[1] = PAL_CHMAP_CHANNEL_FR;
      ch_info.ch_map[2] = PAL_CHMAP_CHANNEL_C;
      ch_info.ch_map[3] = PAL_CHMAP_CHANNEL_LFE;
      ch_info.ch_map[4] = PAL_CHMAP_CHANNEL_LB;
      ch_info.ch_map[5] = PAL_CHMAP_CHANNEL_RB;
      ch_info.ch_map[6] = PAL_CHMAP_CHANNEL_LS;
      ch_info.ch_map[6] = PAL_CHMAP_CHANNEL_RS;
    } else if (channels == 7) {
      ch_info.channels = 7;
      ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;
      ch_info.ch_map[1] = PAL_CHMAP_CHANNEL_FR;
      ch_info.ch_map[2] = PAL_CHMAP_CHANNEL_C;
      ch_info.ch_map[3] = PAL_CHMAP_CHANNEL_LFE;
      ch_info.ch_map[4] = PAL_CHMAP_CHANNEL_LB;
      ch_info.ch_map[5] = PAL_CHMAP_CHANNEL_RB;
      ch_info.ch_map[6] = PAL_CHMAP_CHANNEL_LS;
    } else if (channels == 6) {
      ch_info.channels = 6;
      ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;
      ch_info.ch_map[1] = PAL_CHMAP_CHANNEL_FR;
      ch_info.ch_map[2] = PAL_CHMAP_CHANNEL_C;
      ch_info.ch_map[3] = PAL_CHMAP_CHANNEL_LFE;
      ch_info.ch_map[4] = PAL_CHMAP_CHANNEL_LB;
      ch_info.ch_map[5] = PAL_CHMAP_CHANNEL_RB;
    } else if (channels == 5) {
      ch_info.channels = 5;
      ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;
      ch_info.ch_map[1] = PAL_CHMAP_CHANNEL_FR;
      ch_info.ch_map[2] = PAL_CHMAP_CHANNEL_C;
      ch_info.ch_map[3] = PAL_CHMAP_CHANNEL_LFE;
      ch_info.ch_map[4] = PAL_CHMAP_CHANNEL_RC;
    } else if (channels == 4) {
      ch_info.channels = 4;
      ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;
      ch_info.ch_map[1] = PAL_CHMAP_CHANNEL_FR;
      ch_info.ch_map[2] = PAL_CHMAP_CHANNEL_C;
      ch_info.ch_map[3] = PAL_CHMAP_CHANNEL_LFE;
    } else if (channels == 3) {
      ch_info.channels = 3;
      ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;
      ch_info.ch_map[1] = PAL_CHMAP_CHANNEL_FR;
      ch_info.ch_map[2] = PAL_CHMAP_CHANNEL_C;
    } else if (channels == 2) {
      ch_info.channels = 2;
      ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;
      ch_info.ch_map[1] = PAL_CHMAP_CHANNEL_FR;
    } else {
      ch_info.channels = 1;
      ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;
    }

    streamAttributes_.type = StreamInPrimary::GetPalStreamType(flags_,
            config_.sample_rate);
    if (source_ == AUDIO_SOURCE_VOICE_UPLINK) {
        streamAttributes_.type = PAL_STREAM_VOICE_CALL_RECORD;
        streamAttributes_.info.voice_rec_info.record_direction = INCALL_RECORD_VOICE_UPLINK;
    } else if (source_ == AUDIO_SOURCE_VOICE_DOWNLINK) {
        streamAttributes_.type = PAL_STREAM_VOICE_CALL_RECORD;
        streamAttributes_.info.voice_rec_info.record_direction = INCALL_RECORD_VOICE_DOWNLINK;
    } else if (source_ == AUDIO_SOURCE_VOICE_CALL) {
        streamAttributes_.type = PAL_STREAM_VOICE_CALL_RECORD;
        streamAttributes_.info.voice_rec_info.record_direction = INCALL_RECORD_VOICE_UPLINK_DOWNLINK;
    }
    streamAttributes_.flags = (pal_stream_flags_t)0;
    streamAttributes_.direction = PAL_AUDIO_INPUT;
    streamAttributes_.in_media_config.sample_rate = config_.sample_rate;
    streamAttributes_.in_media_config.bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
    streamAttributes_.in_media_config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM; // TODO: need to convert this from output format
    streamAttributes_.in_media_config.ch_info = ch_info;

    if (streamAttributes_.type == PAL_STREAM_ULTRA_LOW_LATENCY) {
            if (usecase_ == USECASE_AUDIO_RECORD_MMAP)
                streamAttributes_.flags = (pal_stream_flags_t)
                    (PAL_STREAM_FLAG_MMAP_NO_IRQ);
            else if (usecase_ == USECASE_AUDIO_RECORD_LOW_LATENCY)
                streamAttributes_.flags = (pal_stream_flags_t)
                    (PAL_STREAM_FLAG_MMAP);
    }

    if (streamAttributes_.type == PAL_STREAM_PROXY &&
            (isDeviceAvailable(PAL_DEVICE_IN_PROXY)))
        streamAttributes_.info.opt_stream_info.tx_proxy_type = PAL_STREAM_PROXY_TX_WFD;

    ALOGD("%s:(%x:ret)%d", __func__, ret, __LINE__);

    ret = pal_stream_open(&streamAttributes_,
                         mAndroidInDevices.size(),
                         mPalInDevice,
                         0,
                         NULL,
                         &pal_callback,
                         (void *)this,
                         &pal_stream_handle_);

    ALOGD("%s:(%x:ret)%d", __func__, ret, __LINE__);

    if (ret) {
        ALOGE("Pal Stream Open Error (%x)", ret);
        ret = -EINVAL;
        goto error_open;
    }

set_buff_size:
    if (usecase_ == USECASE_AUDIO_RECORD_MMAP) {
        inBufSize = MMAP_PERIOD_SIZE * audio_bytes_per_frame(
                    audio_channel_count_from_in_mask(config_.channel_mask),
                    config_.format);
        inBufCount = MMAP_PERIOD_COUNT_DEFAULT;
    } else if (usecase_ == USECASE_AUDIO_RECORD_LOW_LATENCY) {
        inBufSize = ULL_PERIOD_SIZE * audio_bytes_per_frame(
                    audio_channel_count_from_in_mask(config_.channel_mask),
                    config_.format);
        inBufCount = ULL_PERIOD_COUNT_DEFAULT;
    } else
        inBufSize = StreamInPrimary::GetBufferSize();
    if (!handle) {
        ret = pal_stream_set_buffer_size(pal_stream_handle_,(size_t*)&inBufSize,inBufCount,(size_t*)&outBufSize,outBufCount);
        if (ret) {
            ALOGE("Pal Stream set buffer size Error  (%x)", ret);
        }
    }

    total_bytes_read_ = 0; // reset at each open

error_open:
    return ret;
}


/* in bytes */
uint32_t StreamInPrimary::GetBufferSize() {
    struct pal_stream_attributes streamAttributes_;

    streamAttributes_.type = StreamInPrimary::GetPalStreamType(flags_,
            config_.sample_rate);
    if (streamAttributes_.type == PAL_STREAM_VOIP_TX) {
        return voip_get_buffer_size(config_.sample_rate);
    } else if (streamAttributes_.type == PAL_STREAM_LOW_LATENCY) {
        return LOW_LATENCY_CAPTURE_PERIOD_SIZE *
            audio_bytes_per_frame(
                    audio_channel_count_from_in_mask(config_.channel_mask),
                    config_.format);
    } else if (streamAttributes_.type == PAL_STREAM_ULTRA_LOW_LATENCY) {
        return ULL_PERIOD_SIZE * ULL_PERIOD_MULTIPLIER *
            audio_bytes_per_frame(
                    audio_channel_count_from_in_mask(config_.channel_mask),
                    config_.format);
    } else if (streamAttributes_.type == PAL_STREAM_DEEP_BUFFER) {
        return (config_.sample_rate * AUDIO_CAPTURE_PERIOD_DURATION_MSEC/ 1000) *
            audio_bytes_per_frame(
                    audio_channel_count_from_in_mask(config_.channel_mask),
                    config_.format);
    } else if (streamAttributes_.type == PAL_STREAM_PROXY) {
        return config_.frame_count *
            audio_bytes_per_frame(
                    audio_channel_count_from_in_mask(config_.channel_mask),
                    config_.format);
    } else {
        return BUF_SIZE_CAPTURE * NO_OF_BUF;
    }
}

int StreamInPrimary::GetInputUseCase(audio_input_flags_t halStreamFlags, audio_source_t source)
{
    // TODO: cover other usecases
    int usecase = USECASE_AUDIO_RECORD;
    if (config_.sample_rate == LOW_LATENCY_CAPTURE_SAMPLE_RATE &&
        (halStreamFlags & AUDIO_INPUT_FLAG_TIMESTAMP) == 0 &&
        (halStreamFlags & AUDIO_INPUT_FLAG_COMPRESS) == 0 &&
        (halStreamFlags & AUDIO_INPUT_FLAG_FAST) != 0 &&
        (!(isDeviceAvailable(PAL_DEVICE_IN_PROXY))))
        usecase = USECASE_AUDIO_RECORD_LOW_LATENCY;

    if ((halStreamFlags & AUDIO_INPUT_FLAG_MMAP_NOIRQ) != 0)
        usecase = USECASE_AUDIO_RECORD_MMAP;
    else if (source == AUDIO_SOURCE_VOICE_COMMUNICATION &&
             halStreamFlags & AUDIO_INPUT_FLAG_VOIP_TX)
        usecase = USECASE_AUDIO_RECORD_VOIP;

    return usecase;
}

ssize_t StreamInPrimary::Read(const void *buffer, size_t bytes) {
    int ret = 0;
    int retry_count = MAX_READ_RETRY_COUNT;
    ssize_t size = 0;
    struct pal_buffer palBuffer;
    uint32_t local_bytes_read = 0;
    bool is_perf_lock_acquired = false;

    palBuffer.buffer = (void*)buffer;
    palBuffer.size = bytes;
    palBuffer.offset = 0;
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();

    ALOGV("%s: Bytes:(%zu)", __func__, bytes);
    is_perf_lock_acquired = AcquirePerfLock();
    if (!pal_stream_handle_) {
        ret = Open();
    }

    if (is_st_session) {
        ATRACE_BEGIN("hal: lab read");
        if (!stream_started_) {
            adevice->num_va_sessions_++;
            stream_started_ = true;
        }
        while (retry_count--) {
            size = pal_stream_read(pal_stream_handle_, &palBuffer);
            if (size < 0) {
                memset(palBuffer.buffer, 0, palBuffer.size);
                local_bytes_read = palBuffer.size;
                total_bytes_read_ += local_bytes_read;
                ALOGE("%s: error, failed to read data from PAL", __func__);
                ATRACE_END();
                goto exit;
            } else if (size == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            } else {
                break;
            }
        }
        local_bytes_read = size;
        total_bytes_read_ += local_bytes_read;
        ATRACE_END();
        goto exit;
    }

    if (!stream_started_) {
        ret = pal_stream_start(pal_stream_handle_);
        if (ret) {
            ALOGE("%s:failed to start stream. ret=%d", __func__, ret);
            pal_stream_close(pal_stream_handle_);
            pal_stream_handle_ = NULL;
            goto exit;
        }
        stream_started_ = true;
        /* set cached volume if any, dont return failure back up */
        if (volume_) {
            ret = pal_stream_set_volume(pal_stream_handle_, volume_);
            if (ret) {
                ALOGE("Pal Stream volume Error (%x)", ret);
            }
        }
    }

    if (!effects_applied_) {
       if (isECEnabled && isNSEnabled) {
          ret = pal_add_remove_effect(pal_stream_handle_,PAL_AUDIO_EFFECT_ECNS,true);
       } else if (isECEnabled) {
          ret = pal_add_remove_effect(pal_stream_handle_,PAL_AUDIO_EFFECT_EC,true);
       } else if (isNSEnabled) {
          ret = pal_add_remove_effect(pal_stream_handle_,PAL_AUDIO_EFFECT_NS,true);
       } else {
          ret = pal_add_remove_effect(pal_stream_handle_,PAL_AUDIO_EFFECT_ECNS,false);
       }
       effects_applied_ = true;
    }

    local_bytes_read = pal_stream_read(pal_stream_handle_, &palBuffer);

    // mute pcm data if sva client is reading lab data
    if (adevice->num_va_sessions_ > 0 &&
        source_ != AUDIO_SOURCE_VOICE_RECOGNITION &&
        property_get_bool("persist.vendor.audio.va_concurrency_mute_enabled",
        false)) {
        memset(palBuffer.buffer, 0, palBuffer.size);
        local_bytes_read = palBuffer.size;
    }

    total_bytes_read_ += local_bytes_read;

exit:
    ALOGV("%s: Exit, bytes read %u", __func__, local_bytes_read);

    if (is_perf_lock_acquired)
        ReleasePerfLock();
    return local_bytes_read;
}

int StreamInPrimary::FillHalFnPtrs() {
    int ret = 0;

    stream_.get()->common.get_sample_rate = astream_in_get_sample_rate;
    stream_.get()->common.set_sample_rate = astream_set_sample_rate;
    stream_.get()->common.get_buffer_size = astream_in_get_buffer_size;
    stream_.get()->common.get_channels = astream_in_get_channels;
    stream_.get()->common.get_format = astream_in_get_format;
    stream_.get()->common.set_format = astream_set_format;
    stream_.get()->common.standby = astream_in_standby;
    stream_.get()->common.dump = astream_dump;
    stream_.get()->common.set_parameters = astream_in_set_parameters;
    stream_.get()->common.get_parameters = astream_in_get_parameters;
    stream_.get()->common.add_audio_effect = astream_in_add_audio_effect;
    stream_.get()->common.remove_audio_effect = astream_in_remove_audio_effect;
    stream_.get()->set_gain = astream_in_set_gain;
    stream_.get()->read = in_read;
    stream_.get()->get_input_frames_lost = astream_in_get_input_frames_lost;
    stream_.get()->get_capture_position = astream_in_get_capture_position;
    stream_.get()->get_active_microphones = astream_in_get_active_microphones;
    stream_.get()->set_microphone_direction =
                                            astream_in_set_microphone_direction;
    stream_.get()->set_microphone_field_dimension =
                                            in_set_microphone_field_dimension;
    stream_.get()->update_sink_metadata = in_update_sink_metadata;

    return ret;
}

StreamInPrimary::StreamInPrimary(audio_io_handle_t handle,
    const std::set<audio_devices_t> &devices,
    audio_input_flags_t flags,
    struct audio_config *config,
    const char *address __unused,
    audio_source_t source) :
    StreamPrimary(handle, devices, config),
    mAndroidInDevices(devices),
    flags_(flags)
{
    stream_ = std::shared_ptr<audio_stream_in> (new audio_stream_in());
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    pal_stream_handle_ = NULL;
    mInitialized = false;
    int noPalDevices = 0;
    int ret = 0;

    ALOGD("%s: enter: handle (%x) format(%#x) sample_rate(%d) channel_mask(%#x) devices(%zu) flags(%#x)"\
          , __func__, handle, config->format, config->sample_rate, config->channel_mask,
          mAndroidInDevices.size(), flags);
    if (AudioExtn::audio_devices_cmp(mAndroidInDevices, audio_is_usb_in_device)) {
        if (!config->sample_rate) {
            // get capability from device of USB
            pal_param_device_capability_t *device_cap_query = new pal_param_device_capability_t();
            dynamic_media_config_t dynamic_media_config;
            size_t payload_size = 0;
            device_cap_query->id = PAL_DEVICE_IN_USB_HEADSET;
            device_cap_query->addr.card_id = adevice->usb_card_id_;
            device_cap_query->addr.device_num = adevice->usb_dev_num_;
            device_cap_query->config = &dynamic_media_config;
            device_cap_query->is_playback = false;
            ret = pal_get_param(PAL_PARAM_ID_DEVICE_CAPABILITY,
                                (void **)&device_cap_query,
                                &payload_size, nullptr);
            ALOGD("%s: usb fs=%d format=%d mask=%x", __func__,
                dynamic_media_config.sample_rate,
                dynamic_media_config.format, dynamic_media_config.mask);
            delete device_cap_query;
            config->sample_rate = dynamic_media_config.sample_rate;
            config->channel_mask = dynamic_media_config.mask;
            config->format = (audio_format_t)dynamic_media_config.format;
            memcpy(&config_, config, sizeof(struct audio_config));
        }
    }

            /* this is required for USB otherwise adev_open_input_stream is failed */
    if (!config_.sample_rate)
        config_.sample_rate = DEFAULT_OUTPUT_SAMPLING_RATE;
    if (!config_.channel_mask)
        config_.channel_mask = AUDIO_CHANNEL_IN_MONO;
    if (!config_.format)
        config_.format = AUDIO_FORMAT_PCM_16_BIT;

    ALOGD("%s: local : handle (%x) format(%#x) sample_rate(%d) channel_mask(%#x) devices(%#x) flags(%#x)"\
          , __func__, handle, config_.format, config_.sample_rate, config_.channel_mask,
          AudioExtn::get_device_types(devices), flags);


    source_ = source;

    mAndroidInDevices = devices;
    if(mAndroidInDevices.empty())
        mAndroidInDevices.insert(AUDIO_DEVICE_IN_DEFAULT);

    ALOGD("%s: No of devices %zu", __func__, mAndroidInDevices.size());
    mPalInDeviceIds = new pal_device_id_t[mAndroidInDevices.size()];
    if (!mPalInDeviceIds) {
        goto error;
    }

    noPalDevices = getPalDeviceIds(devices, mPalInDeviceIds);
    if (noPalDevices != mAndroidInDevices.size()) {
        ALOGE("%s: mismatched pal %d and hal devices %zu", __func__, noPalDevices, mAndroidInDevices.size());
        goto error;
    }
    mPalInDevice = new pal_device [mAndroidInDevices.size()];
    if (!mPalInDevice) {
        goto error;
    }

    for (int i = 0; i < mAndroidInDevices.size(); i++) {
        mPalInDevice[i].id = mPalInDeviceIds[i];
        mPalInDevice[i].config.sample_rate = config->sample_rate;
        mPalInDevice[i].config.bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
        // ch_info memory is allocated at resource manager:getdeviceconfig
        mPalInDevice[i].config.ch_info = {0, {0}};
        mPalInDevice[i].config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM; // TODO: need to convert this from output format
        if ((mPalInDeviceIds[i] == PAL_DEVICE_IN_USB_DEVICE) ||
           (mPalInDeviceIds[i] == PAL_DEVICE_IN_USB_HEADSET)) {
            mPalInDevice[i].address.card_id = adevice->usb_card_id_;
            mPalInDevice[i].address.device_num = adevice->usb_dev_num_;
        }
    }

    usecase_ = GetInputUseCase(flags, source);
    if (flags & AUDIO_INPUT_FLAG_MMAP_NOIRQ) {
        stream_.get()->start = astream_in_mmap_noirq_start;
        stream_.get()->stop = astream_in_mmap_noirq_stop;
        stream_.get()->create_mmap_buffer = astream_in_create_mmap_buffer;
        stream_.get()->get_mmap_position = astream_in_get_mmap_position;
    }
    (void)FillHalFnPtrs();
    mInitialized = true;
error:
    return;
}

StreamInPrimary::~StreamInPrimary() {
    if (pal_stream_handle_ && !is_st_session) {
        ALOGD("%s: close stream, pal_stream_handle (%p)", __func__,
             pal_stream_handle_);
        pal_stream_close(pal_stream_handle_);
        pal_stream_handle_ = NULL;
    }
}

StreamPrimary::StreamPrimary(audio_io_handle_t handle,
    const std::set<audio_devices_t> &devices __unused, struct audio_config *config):
    pal_stream_handle_(NULL),
    handle_(handle),
    config_(*config),
    volume_(NULL)
{
    memset(&streamAttributes_, 0, sizeof(streamAttributes_));
    memset(&address_, 0, sizeof(address_));
    ALOGE("%s: handle: %d channel_mask: %d ", __func__, handle_, config_.channel_mask);
}

StreamPrimary::~StreamPrimary(void)
{
    if (volume_) {
        free(volume_);
        volume_ = NULL;
    }
}

