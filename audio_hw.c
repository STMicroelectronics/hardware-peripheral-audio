/*
 * Copyright (C) 2012-13 Wolfson Microelectronics plc
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (C) 2019 STMicroelectronics
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "tinyhal"

//#define LOG_NDEBUG 0
// #define TEST_32BITS 0

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>

#include <audio_utils/resampler.h>
#include <cutils/properties.h>
#include <cutils/str_parms.h>
#include <hardware/audio.h>
#include <hardware/audio_alsaops.h>
#include <hardware/hardware.h>
#include <log/log.h>
#include <sound/compress_params.h>
#include <sound/compress_offload.h>
#include <system/audio.h>
#include <tinyalsa/asoundlib.h>
#include <tinycompress/tinycompress.h>
#include <utils/Timers.h>

#include "audio_config.h"

/* These values are defined in _frames_ (not bytes) to match the ALSA API */
#define OUT_PERIOD_SIZE_DEFAULT 256
#define OUT_PERIOD_COUNT_DEFAULT 4
#define OUT_PERIOD_START_THRESHOLD 2
#define OUT_CHANNEL_MASK_DEFAULT AUDIO_CHANNEL_OUT_STEREO
#define OUT_CHANNEL_COUNT_DEFAULT 2
#define OUT_SAMPLE_RATE_DEFAULT 48000

#define IN_PERIOD_SIZE_DEFAULT 256
#define IN_PERIOD_COUNT_DEFAULT 4
#define IN_CHANNEL_MASK_DEFAULT AUDIO_CHANNEL_IN_MONO
#define IN_CHANNEL_COUNT_DEFAULT 1
#define IN_SAMPLE_RATE_DEFAULT 48000

/* AudioFlinger does not re-read the buffer size after
 * issuing a routing or input_source change so the
 * default buffer size must be suitable for both PCM
 * and compressed inputs
 */
#define IN_COMPRESS_BUFFER_SIZE_DEFAULT 1024

/* Maximum time we'll wait for data from a compress_pcm input */
#define MAX_COMPRESS_PCM_TIMEOUT_MS     2100

/* Voice trigger and voice recognition stream names */
const char kVoiceTriggerStreamName[] = "voice trigger";
const char kVoiceRecogStreamName[] = "voice recognition";

/* States for voice trigger / voice recognition state machine */
enum voice_state {
  eVoiceNone,             /* no voice recognition hardware */
  eVoiceTriggerIdle,      /* Trigger-only mode idle */
  eVoiceTriggerArmed,     /* Trigger-only mode armed */
  eVoiceTriggerFired,     /* Trigger-only mode received trigger */
  eVoiceRecogIdle,        /* Full trigger+audio mode idle */
  eVoiceRecogArmed,       /* Full trigger+audio mode armed */
  eVoiceRecogFired,       /* Full trigger+audio mode received trigger */
  eVoiceRecogAudio,       /* Full trigger+audio mode opened for audio */
  eVoiceRecogReArm        /* Re-arm after audio */
};

struct audio_device {
  struct audio_hw_device hw_device;

  bool disable_audio;

  pthread_mutex_t lock;
  bool mic_mute;
  struct config_mgr *cm;

  struct stream_in_pcm *active_voice_control;

  enum voice_state voice_st;
  audio_devices_t voice_trig_mic;

  const struct hw_stream* global_stream;

  union {
    /* config stream for trigger-only operation */
    const struct hw_stream* voice_trig_stream;

    /* config stream for trigger+voice operation */
    const struct hw_stream* voice_recog_stream;
  };
};


typedef void(*close_fn)(struct audio_stream *);

/* Fields common to all types of output stream */
struct stream_out_common {
  struct audio_stream_out stream;

  close_fn    close;
  struct audio_device *dev;
  const struct hw_stream* hw;

  pthread_mutex_t lock;
  pthread_mutex_t pre_lock;

  bool standby;

  /* Stream parameters as seen by AudioFlinger
   * If stream is resampling AudioFlinger buffers before
   * passing them to hardware, these members refer to the
   * _input_ data from AudioFlinger
   */
  audio_format_t format;
  uint32_t channel_mask;
  int channel_count;
  uint32_t sample_rate;
  size_t frame_size;
  uint32_t buffer_size;

  uint32_t latency;
};

struct stream_out_pcm {
  struct stream_out_common common;

  struct pcm *pcm;

  unsigned int hw_sample_rate;    /* actual sample rate of hardware */
  unsigned int hw_channel_count;  /* actual number of output channels */
  unsigned int hw_period_size;  /* actual number of output period size */
  unsigned int hw_period_count;  /* actual number of output period count */

  uint64_t hw_frames_written;  /* actual number of written frames */
  uint64_t hw_frames_rendered;  /* actual number of written frames */
};

struct in_resampler {
  struct resampler_itfe *resampler;
  struct resampler_buffer_provider buf_provider;
  int16_t *buffer;
  size_t in_buffer_size;
  int in_buffer_frames;
  size_t frames_in;
  int read_status;
};

/* Fields common to all types of input stream */
struct stream_in_common {
  struct audio_stream_in stream;

  close_fn    close;
  struct audio_device *dev;
  const struct hw_stream* hw;

  pthread_mutex_t lock;

  bool standby;

  /* Stream parameters as seen by AudioFlinger
   * If stream is resampling AudioFlinger buffers before
   * passing them to hardware, these members refer to the
   * _input_ data from AudioFlinger
   */
  audio_devices_t devices;
  audio_format_t format;
  uint32_t channel_mask;
  int channel_count;
  uint32_t sample_rate;
  size_t frame_size;
  size_t buffer_size;

  int input_source;

  nsecs_t last_read_ns;
};

struct stream_in_pcm {
  struct stream_in_common common;

  union {
    struct pcm *pcm;
    struct compress *compress;
  };

  unsigned int hw_sample_rate;    /* actual sample rate of hardware */
  unsigned int hw_channel_count;  /* actual number of input channels */
  unsigned int hw_period_size;  /* actual number of input period size */
  unsigned int hw_period_count;  /* actual number of input period count */

  struct in_resampler resampler;
};

static uint32_t out_get_sample_rate(const struct audio_stream *stream);
static uint32_t in_get_sample_rate(const struct audio_stream *stream);
static void voice_trigger_audio_started_locked(struct audio_device *adev);
static void voice_trigger_audio_ended_locked(struct audio_device *adev);
static const char *voice_trigger_audio_stream_name(struct audio_device *adev);

/*********************************************************************
 * Stream common functions
 *********************************************************************/

static int stream_invoke_usecases(const struct hw_stream *stream,
                                  const char *kvpairs)
{
  char *parms = strdup(kvpairs);
  char *p = NULL;
  char *temp = NULL;
  char *pval = NULL;
  char value[32] = { 0 };
  int ret = 0;

  ALOGV("+stream_invoke_usecases(%p) '%s'", stream, kvpairs);

  if (!parms) {
    return -ENOMEM;
  }

  /* It's not obvious what we should do if multiple parameters
   * are given and we only understand some. The action taken
   * here is to process all that we understand and only return
   * and error if we don't understand any
   */
  ret = -ENOTSUP;

  if (stream != NULL) {
    p = strtok_r(parms, ";", &temp);
    while(p) {
      pval = strchr(p, '=');
      if (pval && (pval[1] != '\0')) {
        *pval = '\0';
        if (apply_use_case(stream, p, pval+1) >= 0) {
          ret = 0;
        }
        *pval = '=';
      }
      p = strtok_r(NULL, ";", &temp);
    }
  }

  return ret;
}

static int common_get_routing_param(uint32_t *vout, const char *kvpairs)
{
  struct str_parms *parms = NULL;
  char value[32] = { 0 };
  int ret = 0;

  parms = str_parms_create_str(kvpairs);

  ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING,
                          value, sizeof(value));
  if (ret >= 0) {
    *vout = atoi(value);
  }
  str_parms_destroy(parms);
  return ret;
}

/*********************************************************************
 * Output stream common functions
 *********************************************************************/

static void lock_output_stream(struct stream_out_pcm *out)
{
  pthread_mutex_lock(&out->common.pre_lock);
  pthread_mutex_lock(&out->common.lock);
  pthread_mutex_unlock(&out->common.pre_lock);
}

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
  struct stream_out_common *out = (struct stream_out_common *)stream;
  uint32_t rate = 0;

  if (out->sample_rate != 0) {
    rate = out->sample_rate;
  } else {
    rate = out->hw->rate;
  }

  ALOGV("out_get_sample_rate=%u", rate);
  return rate;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
  ALOGV("out_set_sample_rate called but not implemented");
  UNUSED(stream);
  UNUSED(rate);
  return -ENOSYS;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
  struct stream_out_common *out = (struct stream_out_common *)stream;
  ALOGV("out_get_buffer_size: %u", out->buffer_size);
  return out->buffer_size;
}

static audio_channel_mask_t out_get_channels(const struct audio_stream *stream)
{
  struct stream_out_common *out = (struct stream_out_common *)stream;
  audio_channel_mask_t mask = OUT_CHANNEL_MASK_DEFAULT;

  if (out->channel_mask != 0) {
    mask = out->channel_mask;
  }
  ALOGV("out_get_channels=%x", mask);
  return mask;
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
  struct stream_out_common *out = (struct stream_out_common *)stream;
  ALOGV("out_get_format(%p): 0x%x", stream, out->format);
  return out->format;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
  ALOGV("out_dump called but not implemented");
  UNUSED(stream);
  UNUSED(fd);
  return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
  ALOGV("+out_set_parameters(%p) '%s'", stream, kvpairs);

  struct stream_out_common *out = (struct stream_out_common *)stream;
  struct audio_device *adev = out->dev;
  uint32_t v = 0;
  int ret = common_get_routing_param(&v, kvpairs);

  pthread_mutex_lock(&adev->lock);

  if (ret >= 0) {
    apply_route(out->hw, v);
  }

  stream_invoke_usecases(out->hw, kvpairs);

  pthread_mutex_unlock(&adev->lock);

  ALOGV("-out_set_parameters(%p)", out);

  /* Its meaningless to return an error here - it's not an error if
   * we were sent a parameter we aren't interested in
   */
  return 0;
}

static char * out_get_parameters(const struct audio_stream *stream,
                                 const char *keys)
{
  char *inputKeys, *currentKey, *saveptr1;
  char outputBuffer[256] = {0};

  inputKeys = strdup(keys);

  currentKey = strtok_r(inputKeys, ";", &saveptr1); //Each keys are separate by ';'
  while(currentKey != NULL)
  {
    ALOGE("Extract key : %s", currentKey);
    if (strcmp(currentKey,AUDIO_PARAMETER_STREAM_SUP_FORMATS) == 0) {
      audio_format_t format = out_get_format(stream);
      ALOGE("get format value : %d", format);
      strncat(outputBuffer, AUDIO_PARAMETER_STREAM_SUP_FORMATS, 256);
      switch(format){
        case AUDIO_FORMAT_PCM_16_BIT:
          strncat(outputBuffer, "=AUDIO_FORMAT_PCM_16_BIT", 256);
          break;
        case AUDIO_FORMAT_PCM_8_BIT:
          strncat(outputBuffer, "=AUDIO_FORMAT_PCM_8_BIT", 256);
          break;
        case AUDIO_FORMAT_PCM_32_BIT:
          strncat(outputBuffer, "=AUDIO_FORMAT_PCM_32_BIT", 256);
          break;
        case AUDIO_FORMAT_PCM_8_24_BIT:
          strncat(outputBuffer, "=AUDIO_FORMAT_PCM_8_24_BIT", 256);
          break;
        case AUDIO_FORMAT_PCM_FLOAT:
          strncat(outputBuffer, "=AUDIO_FORMAT_PCM_FLOAT", 256);
          break;
        case AUDIO_FORMAT_PCM_24_BIT_PACKED:
          strncat(outputBuffer, "=AUDIO_FORMAT_PCM_24_BIT_PACKED", 256);
          break;
        default:
          strncat(outputBuffer, "=AUDIO_FORMAT_INVALID", 256);
          break;
      }
    }
    currentKey = strtok_r(NULL, ";", &saveptr1);
  }

  return strdup(outputBuffer);
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
  struct stream_out_common *out = (struct stream_out_common *)stream;
  uint32_t ret = 0;

  if (out->latency > 0) {
    ret = out->latency;
  } else {
    ret = (OUT_PERIOD_SIZE_DEFAULT * OUT_PERIOD_COUNT_DEFAULT * 1000);
    ret /= OUT_SAMPLE_RATE_DEFAULT;
  }

  ALOGV("out_get_latency(%p): %d", stream, ret);
  return ret;
}

static int volume_to_percent(float volume)
{
  float decibels = 0;
  float percent = 0;

  /* Converting back to a decibel scale */
  if(volume > 0) {
    decibels = log(volume) / 0.115129f;
  } else {
    /* Use the maximum attenuation value 58 */
    decibels = -58;
  }

  /* decibels range is -58..0, rescale to range 0..100 */
  percent = ((decibels + 58.0) * (100.0/58.0));
  return (int)percent;
}

static int out_set_volume(struct audio_stream_out *stream,
                          float left, float right)
{
  struct stream_out_common *out = (struct stream_out_common *)stream;
  int l_pc = volume_to_percent(left);
  int r_pc = volume_to_percent(right);

  ALOGV("out_set_volume (%f,%f) -> (%d%%,%d%%)", left, right, l_pc, r_pc);

  return set_hw_volume(out->hw, l_pc, r_pc);
}

static int out_add_audio_effect(const struct audio_stream *stream,
                                effect_handle_t effect)
{
  ALOGV("out_add_audio_effect called but not implemented");
  UNUSED(stream);
  UNUSED(effect);
  return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream,
                                   effect_handle_t effect)
{
  ALOGV("out_remove_audio_effect called but not implemented");
  UNUSED(stream);
  UNUSED(effect);
  return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
                                        int64_t *timestamp)
{
  ALOGV("out_get_next_write_timestamp called but not implemented");
  *timestamp = 0;
  UNUSED(stream);
  return -ENOSYS;
}

static int out_get_presentation_position(const struct audio_stream_out *stream,
                                         uint64_t *frames,
                                         struct timespec *timestamp)
{
  struct stream_out_pcm *out = (struct stream_out_pcm *)stream;
  int ret = -EINVAL;

  lock_output_stream(out);

  if (out->pcm) {
    unsigned int avail;
    if (pcm_get_htimestamp(out->pcm, &avail, timestamp) == 0) {
      size_t kernel_buffer_size = out->hw_period_size * out->hw_period_count;
      int64_t presented_frames = out->hw_frames_written - kernel_buffer_size +
                                 avail;
      if (presented_frames >= 0) {
        *frames = presented_frames;
        ret = 0;
      }
    }
  } else {
    size_t kernel_buffer_size = out->hw_period_size * out->hw_period_count;
    int64_t presented_frames = out->hw_frames_written - kernel_buffer_size;
    if (presented_frames >= 0) {
      *frames = presented_frames;
      ret = 0;
    }
  }

  pthread_mutex_unlock(&out->common.lock);

  ALOGV("out_get_presentation_position returned %llu frames", *frames);

  return ret;
}

static void do_close_out_common(struct audio_stream *stream)
{
  struct stream_out_common *out = (struct stream_out_common *)stream;
  release_stream(out->hw);
  free(stream);
}

static int do_init_out_common(struct stream_out_common *out,
                              const struct audio_config *config,
                              audio_devices_t devices)
{
  int ret = 0;

  ALOGV("do_init_out_common rate=%u channels=%x",
      config->sample_rate,
      config->channel_mask);

  out->standby = true;

  out->stream.common.get_sample_rate = out_get_sample_rate;
  out->stream.common.set_sample_rate = out_set_sample_rate;
  out->stream.common.get_buffer_size = out_get_buffer_size;
  out->stream.common.get_channels = out_get_channels;
  out->stream.common.get_format = out_get_format;
  out->stream.common.set_format = NULL;
  out->stream.common.standby = NULL;
  out->stream.common.dump = out_dump;
  out->stream.common.get_device = NULL;
  out->stream.common.set_device = NULL;
  out->stream.common.set_parameters = out_set_parameters;
  out->stream.common.get_parameters = out_get_parameters;
  out->stream.common.add_audio_effect = out_add_audio_effect;
  out->stream.common.remove_audio_effect = out_remove_audio_effect;

  out->stream.get_latency = out_get_latency;
  out->stream.set_volume = out_set_volume;
  out->stream.write = NULL;
  out->stream.get_render_position = NULL;
  out->stream.get_next_write_timestamp = out_get_next_write_timestamp;
  out->stream.set_callback = NULL;
  out->stream.pause = NULL;
  out->stream.resume = NULL;
  out->stream.drain = NULL;
  out->stream.flush = NULL;
  out->stream.get_presentation_position = out_get_presentation_position;
  out->stream.start = NULL;
  out->stream.stop = NULL;
  out->stream.create_mmap_buffer = NULL;
  out->stream.get_mmap_position = NULL;
  out->stream.update_source_metadata = NULL;

  /* init requested stream config */
  out->format = config->format;
  out->sample_rate = config->sample_rate;
  out->channel_mask = config->channel_mask;
  out->channel_count = popcount(out->channel_mask);

  /* Default settings */
  out->frame_size = audio_stream_out_frame_size(&out->stream);
  ALOGV("frame_size initialize to %d", out->frame_size);

  /* Apply initial route */
  apply_route(out->hw, devices);

  return 0;
}

/*********************************************************************
 * PCM output stream
 *********************************************************************/

static unsigned int out_pcm_cfg_period_count(struct stream_out_pcm *out)
{
  uint32_t ret = OUT_PERIOD_COUNT_DEFAULT;
  if (out->common.hw->period_count != 0) {
    ret = out->common.hw->period_count;
  }
  ALOGV("out_pcm_cfg_period_count = %d", ret);
  return ret;
}

static unsigned int out_pcm_cfg_period_size(struct stream_out_pcm *out)
{
  uint32_t ret = OUT_PERIOD_SIZE_DEFAULT;
  if (out->common.hw->period_size != 0) {
    ret = out->common.hw->period_size;
  }
  ALOGV("out_pcm_cfg_period_size = %d", ret);
  return ret;
}

static unsigned int out_pcm_cfg_rate(struct stream_out_pcm *out)
{
  uint32_t ret = OUT_SAMPLE_RATE_DEFAULT;
  if (out->common.hw->rate != 0) {
    ret = out->common.hw->rate;
  } else if (out->common.sample_rate) {
    ret = out->common.sample_rate;
  }
  ALOGV("out_pcm_cfg_rate = %d", ret);
  return ret;
}

static unsigned int out_pcm_cfg_channel_count(struct stream_out_pcm *out)
{
  uint32_t ret = OUT_CHANNEL_COUNT_DEFAULT;
  if (out->common.channel_count != 0) {
    ret = out->common.channel_count;
  }
  ALOGV("out_pcm_cfg_channel_count = %d", ret);
  return ret;
}

/* must be called with hw device and output stream mutexes locked */
static void do_out_pcm_standby(struct stream_out_pcm *out)
{
  struct audio_device *adev = out->common.dev;

  ALOGV("+do_out_pcm_standby(%p)", out);

  if ((!out->common.standby) && (out->pcm)){
    pthread_mutex_lock(&adev->lock);
    out->common.standby = true;
    pcm_close(out->pcm);
    out->pcm = NULL;
    pthread_mutex_unlock(&adev->lock);
  }
  ALOGV("-do_out_pcm_standby(%p)", out);
}

static void out_pcm_fill_params(struct stream_out_pcm *out,
                                const struct pcm_config *config )
{
  out->hw_sample_rate = config->rate;
  out->hw_channel_count = config->channels;
  out->hw_period_size = config->period_size;
  out->hw_period_count = config->period_count;

  out->common.buffer_size = pcm_frames_to_bytes(out->pcm, config->period_size);
  out->common.latency = (config->period_size * config->period_count * 1000);
  out->common.latency /= config->rate;
}

/* must be called with hw device and output stream mutexes locked */
static int start_output_pcm(struct stream_out_pcm *out)
{
  struct audio_device *adev = out->common.dev;
  int ret = 0;

  struct pcm_config config = {
    .channels = out_pcm_cfg_channel_count(out),
    .rate = out_pcm_cfg_rate(out),
    .period_size = out_pcm_cfg_period_size(out),
    .period_count = out_pcm_cfg_period_count(out),
#ifdef TEST_32BITS
    .format =  PCM_FORMAT_S32_LE,
#else
    .format =  PCM_FORMAT_S16_LE,
#endif
    .start_threshold = 0,
    .stop_threshold = 0,
    .silence_threshold = 0
  };

  ALOGV("+start_output_pcm(%p)", out);

  ALOGV("Requested configuration : channels %d, rate %d, format %d",
      config.channels, config.rate, pcm_format_to_bits(config.format));

  ALOGV("Extended configuration : period_size %d, period_count %d, "
        "start_threshold=%d", config.period_size, config.period_count,
        config.start_threshold);

  if (!adev->disable_audio) {

    out->pcm = pcm_open(out->common.hw->card_number,
                        out->common.hw->device_number,
                        PCM_OUT | PCM_MONOTONIC, &config);

    if (out->pcm && !pcm_is_ready(out->pcm)) {
      ALOGE("pcm_open(out) failed: %s", pcm_get_error(out->pcm));
      pcm_close(out->pcm);
      return -ENOMEM;
    }
  }

#ifdef TEST_32BITS
  // Hide 16 to 32bits conversion
  config.format = PCM_FORMAT_S16_LE;
#endif

  out_pcm_fill_params(out, &config);

  ALOGV("-start_output_pcm(%p)", out);
  return 0;
}

static int out_pcm_standby(struct audio_stream *stream)
{
  struct stream_out_pcm *out = (struct stream_out_pcm *)stream;

  lock_output_stream(out);
  do_out_pcm_standby(out);
  pthread_mutex_unlock(&out->common.lock);

  return 0;
}

#ifdef TEST_32BITS
void out_pcm_memcpy_to_i32_from_i16(int32_t *dst, const int16_t *src,
                                    size_t count)
{
  while (count--) {
    *dst++ = (int32_t)*src++ << 16;
  }
}
#endif

static ssize_t out_pcm_write(struct audio_stream_out *stream,
                             const void* buffer,
                             size_t bytes)
{
  ALOGV("+out_pcm_write(%p) l=%u", stream, bytes);

  int ret = 0;
  struct stream_out_pcm *out = (struct stream_out_pcm *)stream;
  struct audio_device *adev = out->common.dev;

#ifdef TEST_32BITS
  size_t outBufferSize = 0;
  void* outBuffer = NULL;
  int16_t* tmp16Buffer = NULL;
  int32_t* tmp32Buffer = NULL;
#endif

  /* Check that we are routed to something. Android can send routing
   * commands that tell us to disconnect from everything and in that
   * state we shouldn't issue any write commands because we can't be
   * sure that the driver will accept a write to nowhere
   */
  if (get_current_routes(out->common.hw) == 0) {
    ALOGV("-out_pcm_write(%p) 0 (no routes)", stream);
    return 0;
  }

  pthread_mutex_lock(&adev->lock);
  lock_output_stream(out);
  if (out->common.standby) {
    ret = start_output_pcm(out);
    if (ret != 0) {
      pthread_mutex_unlock(&adev->lock);
      goto exit;
    }
    out->common.standby = false;
    out->hw_frames_rendered = 0;
  }
  pthread_mutex_unlock(&adev->lock);

#ifdef TEST_32BITS
  if (!adev->disable_audio) {
    outBufferSize = bytes * 2;
    outBuffer = malloc(outBufferSize);
    memset(outBuffer,0,outBufferSize);
    out_pcm_memcpy_to_i32_from_i16((int32_t*)outBuffer,
                                   (const int16_t*)buffer, bytes >> 1);

    /*
       ALOGV("NLO - buffer 16bits (size = %d): ", bytes);
       tmp16Buffer = (int16_t*)buffer;
       for (int i = 0; i<10; i++) {
       ALOGV("sample %d: 0x%x",i,*tmp16Buffer++);
       }

       ALOGV("NLO - outBuffer 32bits (size = %d): ", outBufferSize);
       tmp32Buffer = (int32_t*)outBuffer;
       for (int i = 0; i<10; i++) {
       ALOGV("outBuffer %d: Ox%x",i,*tmp32Buffer++);
       }
       */

    // case 32bits
    if (outBufferSize > 0) {
      ALOGV(" Write %d bytes (from buffer %p)", (int)outBufferSize, outBuffer);
      ret = pcm_write(out->pcm, outBuffer, outBufferSize);
      if (ret >= 0) {
        ret = bytes;
        out->hw_frames_written += bytes / out->common.frame_size;
        ALOGV(" - Write OK (%llu frames)", out->hw_frames_written);
      }
    }

    free(outBuffer);
  } else {
    int64_t sleep_time = (int64_t)bytes * 1000000;
    sleep_time /= out->common.frame_size / out->common.sample_rate;
    usleep(sleep_time);
    ret = bytes;
    out->hw_frames_written += bytes / out->common.frame_size;
  }

#else
  if (!adev->disable_audio) {
    // case 16bits
    ALOGV(" Write %d bytes (from buffer %p)", (int)bytes, buffer);
    ret = pcm_write(out->pcm, buffer, bytes);
    if (ret >= 0) {
      ret = bytes;
      out->hw_frames_written += bytes / out->common.frame_size;
      out->hw_frames_rendered += bytes / out->common.frame_size;
      ALOGV(" - Write OK (%llu frames)", out->hw_frames_written);
    }
  } else {
    int64_t sleep_time = (int64_t)bytes * 1000000;
    sleep_time /= out->common.frame_size / out->common.sample_rate;
    usleep(sleep_time);
    ret = bytes;
    out->hw_frames_written += bytes / out->common.frame_size;
    out->hw_frames_rendered += bytes / out->common.frame_size;
  }
#endif

exit:
  pthread_mutex_unlock(&out->common.lock);

  ALOGV("-out_pcm_write(%p) r=%u", stream, ret);

  return ret;
}

static int out_pcm_get_render_position(const struct audio_stream_out *stream,
    uint32_t *dsp_frames)
{
  if (stream == NULL || dsp_frames == NULL) {
    return -EINVAL;
  }
  struct stream_out_pcm *out = (struct stream_out_pcm *)stream;
  lock_output_stream(out);
  *dsp_frames = out->hw_frames_rendered;
  pthread_mutex_unlock(&out->common.lock);
  ALOGV("out_get_render_position: dsp_frames: %p", dsp_frames);
  return 0;
}

static void do_close_out_pcm(struct audio_stream *stream)
{
  ALOGV("do_close_out_pcm (%p)", stream);
  out_pcm_standby(stream);
  do_close_out_common(stream);
}

static int do_init_out_pcm(struct stream_out_pcm *out,
                           const struct audio_config *config)
{
  UNUSED(config);
  out->common.close = do_close_out_pcm;
  out->common.stream.common.standby = out_pcm_standby;
  out->common.stream.write = out_pcm_write;
  out->common.stream.get_render_position = out_pcm_get_render_position;
  out->common.buffer_size = out_pcm_cfg_period_size(out) *
                            out->common.frame_size;

  out->hw_frames_rendered = 0;
  out->hw_frames_written = 0;

  return 0;
}

/*********************************************************************
 * Input stream common functions
 *********************************************************************/
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
  const struct stream_in_common *in = (struct stream_in_common *)stream;
  uint32_t rate = 0;

  if (in->sample_rate != 0) {
    rate = in->sample_rate;
  } else {
    rate = in->hw->rate;
  }

  ALOGV("in_get_sample_rate=%u", rate);
  return rate;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
  const struct stream_in_common *in = (struct stream_in_common *)stream;

  if (rate == in->sample_rate) {
    return 0;
  } else {
    return -ENOTSUP;
  }
}

static audio_channel_mask_t in_get_channels(const struct audio_stream *stream)
{
  const struct stream_in_common *in = (struct stream_in_common *)stream;
  audio_channel_mask_t mask = IN_CHANNEL_MASK_DEFAULT;

  if (in->channel_mask != 0) {
    mask = in->channel_mask;
  }

  ALOGV("in_get_channels=0x%x", mask);
  return mask;
}

static audio_format_t in_get_format(const struct audio_stream *stream)
{
  const struct stream_in_common *in = (struct stream_in_common *)stream;

  return in->format;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
  const struct stream_in_common *in = (struct stream_in_common *)stream;
  ALOGV("in_get_buffer_size(%p): %u", stream, in->buffer_size);
  return in->buffer_size;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
  UNUSED(stream);
  UNUSED(fd);
  return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
  UNUSED(stream);
  UNUSED(kvpairs);
  return 0;
}

static char * in_get_parameters(const struct audio_stream *stream,
    const char *keys)
{
  char *inputKeys, *currentKey, *saveptr1;
  char outputBuffer[256] = {0};

  inputKeys = strdup(keys);

  currentKey = strtok_r(inputKeys, ";", &saveptr1); //Each keys are separate by ';'
  while(currentKey != NULL)
  {
    ALOGE("Extract key : %s", currentKey);
    if (strcmp(currentKey,AUDIO_PARAMETER_STREAM_SUP_FORMATS) == 0) {
      audio_format_t format = in_get_format(stream);
      ALOGE("get format value : %d", format);
      strncat(outputBuffer, AUDIO_PARAMETER_STREAM_SUP_FORMATS, 256);
      switch(format){
        case AUDIO_FORMAT_PCM_16_BIT:
          strncat(outputBuffer, "=AUDIO_FORMAT_PCM_16_BIT", 256);
          break;
        case AUDIO_FORMAT_PCM_8_BIT:
          strncat(outputBuffer, "=AUDIO_FORMAT_PCM_8_BIT", 256);
          break;
        case AUDIO_FORMAT_PCM_32_BIT:
          strncat(outputBuffer, "=AUDIO_FORMAT_PCM_32_BIT", 256);
          break;
        case AUDIO_FORMAT_PCM_8_24_BIT:
          strncat(outputBuffer, "=AUDIO_FORMAT_PCM_8_24_BIT", 256);
          break;
        case AUDIO_FORMAT_PCM_FLOAT:
          strncat(outputBuffer, "=AUDIO_FORMAT_PCM_FLOAT", 256);
          break;
        case AUDIO_FORMAT_PCM_24_BIT_PACKED:
          strncat(outputBuffer, "=AUDIO_FORMAT_PCM_24_BIT_PACKED", 256);
          break;
        default:
          strncat(outputBuffer, "=AUDIO_FORMAT_INVALID", 256);
          break;
      }
    }
    currentKey = strtok_r(NULL, ";", &saveptr1);
  }

  return strdup(outputBuffer);
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
  UNUSED(stream);
  UNUSED(gain);
  return 0;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
  UNUSED(stream);
  return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream,
    effect_handle_t effect)
{
  UNUSED(stream);
  UNUSED(effect);
  return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream,
    effect_handle_t effect)
{
  UNUSED(stream);
  UNUSED(effect);
  return 0;
}

static void do_in_set_read_timestamp(struct stream_in_common *in)
{
  nsecs_t ns = systemTime(SYSTEM_TIME_MONOTONIC);

  /* 0 is used to mean we don't have a timestamp, so if */
  /* time count wraps to zero change it to 1 */
  if (ns == 0) {
    ns = 1;
  }

  in->last_read_ns = ns;
}

/*
 * Delay for the time it would have taken to read <bytes> since the last
 * read at the stream sample rate
 */
static void do_in_realtime_delay(struct stream_in_common *in, size_t bytes)
{
  size_t required_interval = 0;
  nsecs_t required_ns = 0;
  nsecs_t elapsed_ns = 0;
  struct timespec ts = { 0 };

  if (in->last_read_ns != 0) {
    /* required interval is calculated so that a left shift 19 places
     * converts approximately to nanoseconds. This avoids the overhead
     * of having to do a 64-bit division if we worked entirely in
     * nanoseconds, and of a large multiply by 1000000 to convert
     * milliseconds to 64-bit nanoseconds.
     * (1907 << 19) = 999817216
     */
    required_interval = (1907 * bytes) / (in->frame_size * in->sample_rate);
    required_ns = (nsecs_t)required_interval << 19;
    elapsed_ns = systemTime(SYSTEM_TIME_MONOTONIC) - in->last_read_ns;

    /* use ~millisecond accurace to ignore trivial nanosecond differences */
    if (required_interval > (elapsed_ns >> 19)) {
      ts.tv_sec = 0;
      ts.tv_nsec = required_ns - elapsed_ns;
      nanosleep(&ts, NULL);
    }
  }
}

static void do_close_in_common(struct audio_stream *stream)
{
  struct stream_in_common *in = (struct stream_in_common *)stream;
  struct audio_device *adev = in->dev;

  in->stream.common.standby(stream);

  /* active_voice_control is not cleared by standby so we must
   * clear it here when stream is closed
   */
  pthread_mutex_lock(&adev->lock);
  if ((struct stream_in_common *)in->dev->active_voice_control == in) {
    in->dev->active_voice_control = NULL;
    voice_trigger_audio_ended_locked(adev);
  }
  pthread_mutex_unlock(&adev->lock);

  if (in->hw != NULL) {
    release_stream(in->hw);
  }

  free(stream);
}

static int do_init_in_common(struct stream_in_common *in,
                             const struct audio_config *config,
                             audio_devices_t devices)
{
  in->standby = true;

  in->close = do_close_in_common;
  in->stream.common.get_sample_rate = in_get_sample_rate;
  in->stream.common.set_sample_rate = in_set_sample_rate;
  in->stream.common.get_buffer_size = in_get_buffer_size;
  in->stream.common.get_channels = in_get_channels;
  in->stream.common.get_format = in_get_format;
  in->stream.common.set_format = NULL;
  in->stream.common.standby = NULL;
  in->stream.common.dump = in_dump;
  in->stream.common.get_device = NULL;
  in->stream.common.set_device = NULL;
  in->stream.common.set_parameters = in_set_parameters;
  in->stream.common.get_parameters = in_get_parameters;
  in->stream.common.add_audio_effect = in_add_audio_effect;
  in->stream.common.remove_audio_effect = in_remove_audio_effect;

  in->stream.set_gain = in_set_gain;
  in->stream.read = NULL;
  in->stream.get_input_frames_lost = in_get_input_frames_lost;
  in->stream.get_capture_position = NULL;
  in->stream.start = NULL;
  in->stream.stop = NULL;
  in->stream.create_mmap_buffer = NULL;
  in->stream.get_mmap_position = NULL;
  in->stream.get_active_microphones = NULL;
  in->stream.update_sink_metadata = NULL;

  /* init requested stream config */
  in->format = config->format;
  in->sample_rate = config->sample_rate;
  in->channel_mask = config->channel_mask;
  in->channel_count = popcount(in->channel_mask);

  in->frame_size = audio_stream_in_frame_size(&in->stream);

  /* Save devices so we can apply initial routing after we've
   * been told the input_source and opened the stream
   */
  in->devices = devices;

  return 0;
}

/*********************************************************************
 * PCM input resampler handling
 *********************************************************************/
static int get_next_buffer(struct resampler_buffer_provider *buffer_provider,
                           struct resampler_buffer* buffer)
{
  struct in_resampler *rsp = NULL;
  struct stream_in_pcm *in = NULL;

  if (buffer_provider == NULL || buffer == NULL) {
    return -EINVAL;
  }

  rsp = (struct in_resampler *)((char *)buffer_provider -
            offsetof(struct in_resampler, buf_provider));
  in = (struct stream_in_pcm *)((char *)rsp -
            offsetof(struct stream_in_pcm, resampler));

  if (in->pcm == NULL) {
    buffer->raw = NULL;
    buffer->frame_count = 0;
    rsp->read_status = -ENODEV;
    return -ENODEV;
  }

  if (rsp->frames_in == 0) {
    rsp->read_status = pcm_read(in->pcm, (void*)rsp->buffer,
                                rsp->in_buffer_size);
    if (rsp->read_status != 0) {
      ALOGE("get_next_buffer() pcm_read error %d", errno);
      buffer->raw = NULL;
      buffer->frame_count = 0;
      return rsp->read_status;
    }
    rsp->frames_in = rsp->in_buffer_frames;
    if ((in->common.channel_count == 1) && (in->hw_channel_count == 2)) {
      unsigned int i;

      /* Discard right channel */
      for (i = 1; i < rsp->frames_in; i++) {
        rsp->buffer[i] = rsp->buffer[i * 2];
      }
    }
  }

  buffer->frame_count = (buffer->frame_count > rsp->frames_in) ?
                        rsp->frames_in : buffer->frame_count;
  buffer->i16 = (int16_t *)rsp->buffer;
  buffer->i16 += (rsp->in_buffer_frames - rsp->frames_in);

  return rsp->read_status;
}

static void release_buffer(struct resampler_buffer_provider *buffer_provider,
                           struct resampler_buffer* buffer)
{
  struct in_resampler *rsp = NULL;
  struct stream_in_pcm *in = NULL;

  if (buffer_provider == NULL || buffer == NULL)
    return;

  rsp = (struct in_resampler *)((char *)buffer_provider -
            offsetof(struct in_resampler, buf_provider));
  in = (struct stream_in_pcm *)((char *)rsp -
            offsetof(struct stream_in_pcm, resampler));

  rsp->frames_in -= buffer->frame_count;
}

static ssize_t read_resampled_frames(struct stream_in_pcm *in,
                                     void *buffer, ssize_t frames)
{
  struct in_resampler *rsp = &in->resampler;
  ssize_t frames_wr = 0;

  while (frames_wr < frames) {
    size_t frames_rd = frames - frames_wr;
    rsp->resampler->resample_from_provider(rsp->resampler,
        (int16_t *)((char *)buffer +
          (frames_wr * in->common.frame_size)),
        &frames_rd);
    if (rsp->read_status != 0) {
      return rsp->read_status;
    }

    frames_wr += frames_rd;
  }
  return frames_wr;
}

static int in_resampler_init(struct stream_in_pcm *in, int hw_rate,
                             int channels, size_t hw_fragment)
{
  struct in_resampler *rsp = &in->resampler;
  int ret = 0;

  rsp->in_buffer_size = hw_fragment * channels * in->common.frame_size;
  rsp->in_buffer_frames = rsp->in_buffer_size /
                          (channels * in->common.frame_size);
  rsp->buffer = malloc(rsp->in_buffer_size);

  if (!rsp->buffer) {
    ret = -ENOMEM;
  } else {
    rsp->buf_provider.get_next_buffer = get_next_buffer;
    rsp->buf_provider.release_buffer = release_buffer;

    ret = create_resampler(hw_rate,
                           in->common.sample_rate,
                           in->common.channel_count,
                           RESAMPLER_QUALITY_DEFAULT,
                           &rsp->buf_provider,
                           &rsp->resampler);
  }

  if (ret < 0) {
    free(rsp->buffer);
    rsp->buffer = NULL;
  }

  return ret;
}

static void in_resampler_free(struct stream_in_pcm *in)
{
  if (in->resampler.resampler) {
    release_resampler(in->resampler.resampler);
    in->resampler.resampler = NULL;
  }

  free(in->resampler.buffer);
  in->resampler.buffer = NULL;
}


/*********************************************************************
 * PCM input stream via compressed channel
 *********************************************************************/

/* must be called with hw device and input stream mutexes locked */
static int do_open_compress_pcm_in(struct stream_in_pcm *in)
{
  struct snd_codec codec = { 0 };
  struct compress *compress = NULL;
  int ret = 0;

  ALOGV("+do_open_compress_pcm_in");

  if (in->common.hw == NULL) {
    ALOGW("input_source not set");
    ret = -EINVAL;
    goto exit;
  }

  memset(&codec, 0, sizeof(codec));
  codec.id = SND_AUDIOCODEC_PCM;
  codec.ch_in = in->common.channel_count;
  codec.sample_rate = in->common.sample_rate;
  codec.format = SNDRV_PCM_FORMAT_S16_LE;

  /* Fragment and buffer sizes should be configurable or auto-detected
   * but are currently just hardcoded
   */
  struct compr_config config = {
    .fragment_size = 4096,
    .fragments = 1,
    .codec = &codec
  };

  compress = compress_open(in->common.hw->card_number,
                           in->common.hw->device_number,
                           COMPRESS_OUT,
                           &config);

  if (!compress || !is_compress_ready(compress)) {
    ret = errno;
    ALOGE_IF(compress,"compress_open(in) failed: %s",
             compress_get_error(compress));
    ALOGE_IF(!compress,"compress_open(in) failed");
    compress_close(compress);
    goto exit;
  }

  in->compress = compress;
  in->common.buffer_size = config.fragment_size * config.fragments *
                           in->common.frame_size;
  compress_start(in->compress);

exit:
  ALOGV("-do_open_compress_pcm_in (%d)", ret);
  return ret;
}

/* must be called with hw device and input stream mutexes locked */
static int start_compress_pcm_input_stream(struct stream_in_pcm *in)
{
  struct audio_device *adev = in->common.dev;
  int ms = 0;
  int ret = 0;

  ALOGV("start_compress_pcm_input_stream");

  if (in->common.standby) {
    ret = do_open_compress_pcm_in(in);
    if (ret < 0) {
      return ret;
    }

    /*
     * We must not block AudioFlinger so limit the time that tinycompress
     * will block for data to around twice the time it would take to fetch
     * a buffer of data at the configured sample rate
     */
    ms = (1000LL * in->common.buffer_size) / (in->common.frame_size *
            in->common.sample_rate);

    compress_set_max_poll_wait(in->compress, ms * 2);

    in->common.standby = 0;
  }

  return 0;
}

/* must be called with hw device and input stream mutexes locked */
static void do_in_compress_pcm_standby(struct stream_in_pcm *in)
{
  struct compress *c;
  int ret = 0;

  ALOGV("+do_in_compress_pcm_standby");

  if (!in->common.standby) {
    c = in->compress;
    in->compress = NULL;
    compress_stop(c);
    compress_close(c);
  }
  in->common.standby = true;

  ALOGV("-do_in_compress_pcm_standby");
}

static ssize_t do_in_compress_pcm_read(struct audio_stream_in *stream,
                                       void *buffer,
                                       size_t bytes)
{
  struct stream_in_pcm *in = (struct stream_in_pcm *)stream;
  struct audio_device *adev = in->common.dev;
  int ret = 0;

  ALOGV("+do_in_compress_pcm_read %d", bytes);

  pthread_mutex_lock(&in->common.lock);
  ret = start_compress_pcm_input_stream(in);

  if (ret < 0) {
    goto exit;
  }

  ret = compress_read(in->compress, buffer, bytes);

  ALOGV_IF(ret == 0, "no data");

  if (ret > 0) {
    /*
     * The interface between AudioFlinger and AudioRecord cannot cope
     * with bursty or high-speed data and will lockup for periods if
     * the data arrives faster than the app reads it. So we must limit
     * the rate that we deliver PCM buffers to avoid triggering this
     * condition. Allow data to be returned up to 4x realtime
     */
    do_in_realtime_delay(&in->common, bytes / 4);
  }

exit:
  pthread_mutex_unlock(&in->common.lock);

  ALOGV("-do_in_compress_pcm_read (%d)", ret);
  return ret;
}

static void do_in_compress_pcm_close(struct stream_in_pcm *in)
{
  ALOGV("+do_in_compress_pcm_close");

  if (in->compress != NULL) {
    compress_stop(in->compress);
    compress_close(in->compress);
  }

  ALOGV("-do_in_compress_pcm_close");
}

/*********************************************************************
 * PCM input stream
 *********************************************************************/

static unsigned int in_pcm_cfg_period_count(struct stream_in_pcm *in)
{
  if (in->common.hw->period_count != 0) {
    return in->common.hw->period_count;
  } else {
    return IN_PERIOD_COUNT_DEFAULT;
  }
}

static unsigned int in_pcm_cfg_period_size(struct stream_in_pcm *in)
{
  if (in->common.hw->period_size != 0) {
    return in->common.hw->period_size;
  } else {
    return IN_PERIOD_SIZE_DEFAULT;
  }
}

static unsigned int in_pcm_cfg_rate(struct stream_in_pcm *in)
{
  if (in->common.hw->rate != 0) {
    return in->common.hw->rate;
  } else {
    return IN_SAMPLE_RATE_DEFAULT;
  }
}

static unsigned int in_pcm_cfg_channel_count(struct stream_in_pcm *in)
{
  if (in->common.channel_count != 0) {
    return in->common.channel_count;
  } else {
    return IN_CHANNEL_COUNT_DEFAULT;
  }
}

/* must be called with hw device and input stream mutexes locked */
static void do_in_pcm_standby(struct stream_in_pcm *in)
{
  struct audio_device *adev = in->common.dev;

  ALOGV("+do_in_pcm_standby");

  if (!in->common.standby) {
    pcm_close(in->pcm);
    in->pcm = NULL;
  }

  in_resampler_free(in);
  in->common.standby = true;

  ALOGV("-do_in_pcm_standby");
}

static void in_pcm_fill_params(struct stream_in_pcm *in,
                               const struct pcm_config *config)
{
  size_t size = 0;

  in->hw_sample_rate = config->rate;
  in->hw_channel_count = config->channels;
  in->hw_period_size = config->period_size;
  in->hw_period_count = config->period_count;

  /*
   * take resampling into account and return the closest majoring
   * multiple of 16 frames, as audioflinger expects audio buffers to
   * be a multiple of 16 frames
   */
  size = (config->period_size * in->common.sample_rate) / config->rate;
  size = ((size + 15) / 16) * 16;
  in->common.buffer_size = size * in->common.frame_size;

}

/* must be called with hw device and input stream mutexes locked */
static int do_open_pcm_input(struct stream_in_pcm *in)
{
  struct pcm_config config = { 0 };
  struct audio_device *adev = in->common.dev;
  int ret = 0;

  ALOGV("+do_open_pcm_input");

  if (in->common.hw == NULL) {
    ALOGW("input_source not set");
    ret = -EINVAL;
    goto exit;
  }

  memset(&config, 0, sizeof(config));
  config.channels = in_pcm_cfg_channel_count(in);
  config.rate = in_pcm_cfg_rate(in);
  config.period_size = in_pcm_cfg_period_size(in);
  config.period_count = in_pcm_cfg_period_count(in);
  config.format = pcm_format_from_audio_format(in->common.format);
  config.start_threshold = 0;

  ALOGV("do_open_pcm_input: open PCM config: channels = %d, rate = %d, "
        "period_size = %d, period_count = %d, format = %d", config.channels,
        config.rate, config.period_size, config.period_count, config.format);

  if (!adev->disable_audio) {
    in->pcm = pcm_open(in->common.hw->card_number,
                       in->common.hw->device_number,
                       PCM_IN, &config);

    if (!in->pcm || !pcm_is_ready(in->pcm)) {
      ALOGV_IF(in->pcm,"pcm_open(in) failed: %s", pcm_get_error(in->pcm));
      ALOGV_IF(!in->pcm,"pcm_open(in) failed");
      ret = -ENOMEM;
      goto fail;
    }
  }

  in_pcm_fill_params(in, &config);

  ALOGV("input buffer size=0x%x", in->common.buffer_size);

  /*
   * If the stream rate differs from the PCM rate, we need to
   * create a resampler.
   */
  if (!adev->disable_audio) {
    if (in_get_sample_rate(&in->common.stream.common) != config.rate) {
      ret = in_resampler_init(in, config.rate, config.channels,
          pcm_frames_to_bytes(in->pcm, config.period_size));
      if (ret < 0) {
        goto fail;
      }
    }
  }
  ALOGV("-do_open_pcm_input");
  return 0;

fail:
  pcm_close(in->pcm);
  in->pcm = NULL;
exit:
  ALOGV("-do_open_pcm_input error:%d", ret);
  return ret;
}

/* must be called with hw device and input stream mutexes locked */
static int start_pcm_input_stream(struct stream_in_pcm *in)
{
  int ret = 0;

  if (in->common.standby) {
    ret = do_open_pcm_input(in);
    if (ret == 0) {
      in->common.standby = 0;
    }
  }

  return ret;
}

static int change_input_source_locked(struct stream_in_pcm *in,
                                      const char *value,
                                      uint32_t devices, bool *was_changed)
{
  struct audio_device *adev = in->common.dev;
  struct audio_config config = { 0 };
  const char *stream_name = NULL;
  const struct hw_stream *hw = NULL;
  bool voice_control = false;
  const int new_source = atoi(value);

  *was_changed = false;

  if (!in->common.standby) {
    ALOGE("attempt to change input source while active");
    return -EINVAL;
  }

  if (in->common.input_source == new_source) {
    ALOGV("input source not changed");
    return 0;
  }

  /* Special input sources are obtained from the configuration
   * by opening a named stream
   */
  switch (new_source) {
    case AUDIO_SOURCE_VOICE_RECOGNITION:
      /* We should verify here that current frame size, sample rate and
       * channels are compatible
       */

      /* depends on voice recognition type and state whether we open
       * the voice recognition stream or generic PCM stream
       */
      stream_name = voice_trigger_audio_stream_name(adev);
      voice_control = true;
      break;

    default:
      stream_name = NULL;
      break;
  }

  if (stream_name) {
    /* try to open a stream specific to the chosen input source */
    hw = get_named_stream(in->common.dev->cm, stream_name);
    ALOGV_IF(hw != NULL, "Changing input source to %s", stream_name);
  }

  if (!hw) {
    /* open generic PCM input stream */
    memset(&config, 0, sizeof(config));
    config.sample_rate = in->common.sample_rate;
    config.channel_mask = in->common.channel_mask;
    config.format = in->common.format;
    hw = get_stream(in->common.dev->cm, devices, 0, &config);
    ALOGV_IF(hw != NULL, "Changing to default input source for devices 0x%x",
             devices);
  }

  if (hw != NULL) {
    /* A normal stream will be in standby and therefore device node */
    /* is closed when we get here. */

    if (in->common.hw != NULL) {
      release_stream(in->common.hw);
    }

    in->common.hw = hw;

    pthread_mutex_lock(&adev->lock);
    if (voice_control) {
      adev->active_voice_control = in;
      voice_trigger_audio_started_locked(adev);
    } else if (adev->active_voice_control == in) {
      adev->active_voice_control = NULL;
      voice_trigger_audio_ended_locked(adev);
    }
    pthread_mutex_unlock(&adev->lock);

    in->common.input_source = new_source;
    *was_changed = true;
    return 0;
  } else {
    ALOGV("Could not open new input stream");
    return -EINVAL;
  }
}

static ssize_t do_in_pcm_read(struct audio_stream_in *stream, void* buffer,
                              size_t bytes)
{
  int ret = 0;
  struct stream_in_pcm *in = (struct stream_in_pcm *)stream;
  struct audio_device *adev = in->common.dev;
  size_t frames_rq = bytes / in->common.frame_size;

  ALOGV("+do_in_pcm_read %d", bytes);

  pthread_mutex_lock(&in->common.lock);
  ret = start_pcm_input_stream(in);

  if (ret < 0) {
    goto exit;
  }

  if (!adev->disable_audio) {
    if (in->resampler.resampler != NULL) {
      ret = read_resampled_frames(in, buffer, frames_rq);
    } else {
      ret = pcm_read(in->pcm, buffer, bytes);
    }

    /* Assume any non-negative return is a successful read */
    if (ret >= 0) {
      ret = bytes;
    }
  } else {
    int64_t sleep_time = (int64_t)bytes * 1000000;
    sleep_time /= in->common.frame_size / in->common.sample_rate;
    usleep(sleep_time);
    memset(buffer, 0, bytes);
  }

exit:
  pthread_mutex_unlock(&in->common.lock);

  ALOGV("-do_in_pcm_read (%d)", ret);
  return ret;
}

static int in_pcm_standby(struct audio_stream *stream)
{
  struct stream_in_pcm *in = (struct stream_in_pcm *)stream;

  pthread_mutex_lock(&in->common.lock);

  if (in->common.hw != NULL) {
    if (stream_is_compressed_in(in->common.hw)) {
      do_in_compress_pcm_standby(in);
    } else {
      do_in_pcm_standby(in);
    }
  }

  pthread_mutex_unlock(&in->common.lock);

  return 0;
}

static ssize_t in_pcm_read(struct audio_stream_in *stream, void* buffer,
                           size_t bytes)
{
  struct stream_in_pcm *in = (struct stream_in_pcm *)stream;
  struct audio_device *adev = in->common.dev;
  int ret = 0;

  if (in->common.hw == NULL) {
    ALOGW("in_pcm_read(%p): no input source for stream", stream);
    ret = -EINVAL;
  } else if (get_current_routes(in->common.hw) == 0) {
    ALOGV("in_pcm_read(%p) (no routes)", stream);
    ret = -EINVAL;
  } else {
    if (stream_is_compressed_in(in->common.hw)) {
      ret = do_in_compress_pcm_read(stream, buffer, bytes);
    } else {
      ret = do_in_pcm_read(stream, buffer, bytes);
    }
  }

  /* If error, no data or muted, return a buffer of zeros and delay
   * for the time it would take to capture that much audio at the
   * current sample rate. AudioFlinger can't do anything useful with
   * read errors so convert errors into a read of silence
   */
  if ((ret <= 0) || adev->mic_mute) {
    memset(buffer, 0, bytes);

    /* Only delay if we failed to capture any audio */
    if (ret <= 0) {
      do_in_realtime_delay(&in->common, bytes);
    }

    ret = bytes;
  }

  do_in_set_read_timestamp(&in->common);

  return ret;
}

static int in_pcm_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
  struct stream_in_pcm *in = (struct stream_in_pcm *)stream;
  struct audio_device *adev = in->common.dev;
  struct str_parms *parms = NULL;
  char value[32] = { 0 };
  uint32_t new_routing = 0;
  bool routing_changed = false;
  uint32_t devices = 0;
  bool input_was_changed = false;
  int ret = 0;

  ALOGV("+in_pcm_set_parameters(%p) '%s'", stream, kvpairs);

  ret = common_get_routing_param(&new_routing, kvpairs);
  routing_changed = (ret >= 0);
  parms = str_parms_create_str(kvpairs);

  pthread_mutex_lock(&in->common.lock);

  if(str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_INPUT_SOURCE,
                       value, sizeof(value)) >= 0) {

    if (routing_changed) {
      devices = new_routing;
    } else if (in->common.hw != NULL) {
      /* Route new stream to same devices as current stream */
      devices = get_routed_devices(in->common.hw);
    } else {
      devices = 0;
    }

    ret = change_input_source_locked(in, value, devices, &input_was_changed);
    if (ret < 0) {
      goto out;
    }

    /* We must apply any existing routing to the new stream */
    new_routing = devices;
    routing_changed = true;
  }

  if (routing_changed) {
    in->common.devices = new_routing;

    if (in->common.hw) {
      ALOGV("Apply routing=0x%x to input stream", new_routing);
      apply_route(in->common.hw, new_routing);
    }
    ret = 0;
  }

  stream_invoke_usecases(in->common.hw, kvpairs);

out:
  pthread_mutex_unlock(&in->common.lock);
  str_parms_destroy(parms);

  ALOGV("-in_pcm_set_parameters(%p):%d", stream, ret);

  /* Its meaningless to return an error here - it's not an error if
   * we were sent a parameter we aren't interested in
   */
  return 0;
}

static void do_close_in_pcm(struct audio_stream *stream)
{
  struct stream_in_pcm *in = (struct stream_in_pcm *)stream;

  if (in->common.hw != NULL) {
    if (stream_is_compressed(in->common.hw)) {
      do_in_compress_pcm_close(in);
    }
  }

  do_close_in_common(stream);
}

static int do_init_in_pcm(struct stream_in_pcm *in,
                          struct audio_config *config)
{
  UNUSED(config);
  in->common.close = do_close_in_pcm;
  in->common.stream.common.standby = in_pcm_standby;
  in->common.stream.common.set_parameters = in_pcm_set_parameters;
  in->common.stream.read = in_pcm_read;

  /* Although AudioFlinger has not yet told us the input_source for
   * this stream, it expects us to already know the buffer size.
   * We just have to hardcode something that might work
   */
  in->common.buffer_size = IN_COMPRESS_BUFFER_SIZE_DEFAULT;

  return 0;
}

/*********************************************************************
 * Stream open and close
 *********************************************************************/
static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out,
                                   const char *address)
{
  struct audio_device *adev = (struct audio_device *)dev;
  union {
    struct stream_out_common *common;
    struct stream_out_pcm *pcm;
  } out;
  int ret = 0;

  UNUSED(handle);
  UNUSED(address);

  ALOGV("+adev_open_output_stream");

  devices &= AUDIO_DEVICE_OUT_ALL;
  const struct hw_stream *hw = get_stream(adev->cm, devices, flags, config);
  if (!hw) {
    ALOGE("No suitable output stream for devices=0x%x flags=0x%x format=0x%x",
              devices, flags, config->format);
    ret = -EINVAL;
    goto err_fail;
  }

  out.common = calloc(1, sizeof(struct stream_out_pcm));
  if (!out.common) {
    ret = -ENOMEM;
    goto err_fail;
  }

  out.common->dev = adev;
  out.common->hw = hw;
  ret = do_init_out_common( out.common, config, devices );
  if (ret < 0) {
    goto err_open;
  }

  pthread_mutex_init(&out.common->lock, (const pthread_mutexattr_t *) NULL);
  pthread_mutex_init(&out.common->pre_lock, (const pthread_mutexattr_t *) NULL);

  ret = do_init_out_pcm( out.pcm, config );
  if (ret < 0) {
    goto err_open;
  }


  /* Update config with initial stream settings */
  config->format = out.common->format;
  config->channel_mask = out.common->channel_mask;
  config->sample_rate = out.common->sample_rate;

  *stream_out = &out.common->stream;
  ALOGV("-adev_open_output_stream(%p) with format = 0x%x, channel mask = 0x%x,"
        "sample rate = %d", *stream_out, config->format, config->channel_mask,
        config->sample_rate);
  return 0;

err_open:
  free(out.common);
  *stream_out = NULL;
err_fail:
  ALOGV("-adev_open_output_stream (%d)", ret);
  return ret;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
  UNUSED(dev);
  struct stream_out_common *out = (struct stream_out_common *)stream;
  ALOGV("adev_close_output_stream(%p)", stream);

  (out->close)(&stream->common);

  pthread_mutex_destroy(&out->pre_lock);
  pthread_mutex_destroy(&out->lock);
}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in,
                                  audio_input_flags_t flags,
                                  const char *address,
                                  audio_source_t source)
{
  struct audio_device *adev = (struct audio_device *)dev;
  struct stream_in_pcm *in = NULL;
  int ret = 0;

  UNUSED(handle);
  UNUSED(flags);
  UNUSED(address);
  UNUSED(source);

  ALOGV("+adev_open_input_stream");

  *stream_in = NULL;

  /* We don't open a config manager stream here because we don't yet
   * know what input_source to use. Defer until Android sends us an
   * input_source set_parameter()
   */

  in = (struct stream_in_pcm *)calloc(1, sizeof(struct stream_in_pcm));
  if (!in) {
    ret = -ENOMEM;
    goto fail;
  }

  in->common.dev = adev;

  devices &= AUDIO_DEVICE_IN_ALL;
  ret = do_init_in_common(&in->common, config, devices);
  if (ret < 0) {
    goto fail;
  }

  ret = do_init_in_pcm(in, config);
  if (ret < 0) {
    goto fail;
  }

  *stream_in = &in->common.stream;
  return 0;

fail:
  free(in);
  ALOGV("-adev_open_input_stream (%d)", ret);
  return ret;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
                                    struct audio_stream_in *stream)
{
  UNUSED(dev);
  struct stream_in_common *in = (struct stream_in_common *)stream;
  ALOGV("adev_close_input_stream(%p)", stream);
  (in->close)(&stream->common);
}

/*********************************************************************
 * Voice trigger state machine
 *********************************************************************/

static void do_voice_trigger_open_stream(struct audio_device *adev,
                                         const char *streamName)
{
  audio_devices_t mic_device = AUDIO_DEVICE_IN_BUILTIN_MIC;

  adev->voice_trig_stream = get_named_stream(adev->cm, streamName);

  if (adev->voice_trig_stream != NULL) {
    if (adev->voice_trig_mic != 0) {
      mic_device = adev->voice_trig_mic;
    }

    apply_route(adev->voice_trig_stream, mic_device);
  }
}

static void do_voice_trigger_close_stream(struct audio_device *adev)
{
  if (adev->voice_trig_stream != NULL) {
    apply_route(adev->voice_trig_stream, 0);
    release_stream(adev->voice_trig_stream);
    adev->voice_trig_stream == NULL;
  }
}

static void voice_trigger_enable(struct audio_device *adev)
{
  pthread_mutex_lock(&adev->lock);

  ALOGV("+voice_trigger_enable (%u)", adev->voice_st);

  switch (adev->voice_st) {
    case eVoiceNone:
      break;

    case eVoiceTriggerIdle:
    case eVoiceTriggerFired:
      do_voice_trigger_open_stream(adev, kVoiceTriggerStreamName);
      adev->voice_st = eVoiceTriggerArmed;
      break;

    case eVoiceTriggerArmed:
      break;

    case eVoiceRecogIdle:
      do_voice_trigger_open_stream(adev, kVoiceRecogStreamName);
      adev->voice_st = eVoiceRecogArmed;
      break;

    case eVoiceRecogArmed:
    case eVoiceRecogFired:
    case eVoiceRecogReArm:
      break;

    case eVoiceRecogAudio:
      adev->voice_st = eVoiceRecogReArm;
      break;
  }

  ALOGV("-voice_trigger_enable (%u)", adev->voice_st);

  pthread_mutex_unlock(&adev->lock);
}

static void voice_trigger_disable(struct audio_device *adev)
{
  pthread_mutex_lock(&adev->lock);

  ALOGV("+voice_trigger_disable (%u)", adev->voice_st);

  switch (adev->voice_st) {
    case eVoiceNone:
      break;

    case eVoiceTriggerIdle:
      break;

    case eVoiceTriggerFired:
    case eVoiceTriggerArmed:
      do_voice_trigger_close_stream(adev);
      adev->voice_st = eVoiceTriggerIdle;
      break;

    case eVoiceRecogIdle:
      break;

    case eVoiceRecogArmed:
      do_voice_trigger_close_stream(adev);
      adev->voice_st = eVoiceRecogIdle;
      break;

    case eVoiceRecogFired:
    case eVoiceRecogAudio:
      /* If a full trigger+audio stream has fired we must wait for the */
      /* audio capture stage to end before disabling it                */
      break;

    case eVoiceRecogReArm:
      /* See note on previous case */
      adev->voice_st = eVoiceRecogAudio;
      break;
  }

  ALOGV("-voice_trigger_disable (%u)", adev->voice_st);

  pthread_mutex_unlock(&adev->lock);
}

static void voice_trigger_triggered(struct audio_device *adev)
{
  pthread_mutex_lock(&adev->lock);

  ALOGV("+voice_trigger_triggered (%u)", adev->voice_st);

  switch (adev->voice_st) {
    case eVoiceNone:
    case eVoiceTriggerIdle:
    case eVoiceTriggerFired:
      break;

    case eVoiceTriggerArmed:
      adev->voice_st = eVoiceTriggerFired;
      break;

    case eVoiceRecogIdle:
    case eVoiceRecogFired:
    case eVoiceRecogAudio:
    case eVoiceRecogReArm:
      break;

    case eVoiceRecogArmed:
      adev->voice_st = eVoiceRecogFired;
      break;
  }

  ALOGV("-voice_trigger_triggered (%u)", adev->voice_st);

  pthread_mutex_unlock(&adev->lock);
}

static void voice_trigger_audio_started_locked(struct audio_device *adev)
{
  ALOGV("+voice_trigger_audio_started (%u)", adev->voice_st);

  switch (adev->voice_st) {
    case eVoiceNone:
    case eVoiceTriggerIdle:
    case eVoiceTriggerArmed:
    case eVoiceTriggerFired:
      break;

    case eVoiceRecogIdle:
    case eVoiceRecogArmed:
      break;

    case eVoiceRecogFired:
      adev->voice_st = eVoiceRecogAudio;
      break;

    case eVoiceRecogAudio:
    case eVoiceRecogReArm:
      break;
  }

  ALOGV("-voice_trigger_audio_started (%d)", adev->voice_st);
}

static void voice_trigger_audio_ended_locked(struct audio_device *adev)
{
  ALOGV("+voice_trigger_audio_ended (%u)", adev->voice_st);

  switch (adev->voice_st) {
    case eVoiceNone:
    case eVoiceTriggerIdle:
    case eVoiceTriggerArmed:
    case eVoiceTriggerFired:
      break;

    case eVoiceRecogIdle:
    case eVoiceRecogArmed:
    case eVoiceRecogFired:
      break;

    case eVoiceRecogAudio:
      do_voice_trigger_close_stream(adev);
      adev->voice_st = eVoiceRecogIdle;
      break;

    case eVoiceRecogReArm:
      adev->voice_st = eVoiceRecogArmed;
      break;
  }

  ALOGV("-voice_trigger_audio_ended (%d)", adev->voice_st);
}

static void voice_trigger_set_params(struct audio_device *adev,
                                     struct str_parms *parms)
{
  char value[32];
  int ret = str_parms_get_str(parms, "voice_trigger_mic", value, sizeof(value));

  if (ret >= 0) {
    adev->voice_trig_mic = atoi(value);
  }

  ret = str_parms_get_str(parms, "voice_trigger", value, sizeof(value));
  if (ret >= 0) {
    if (strcmp(value, "2") == 0) {
      voice_trigger_triggered(adev);
    } else if (strcmp(value, "1") == 0) {
      voice_trigger_enable(adev);
    } else if (strcmp(value, "0") == 0) {
      voice_trigger_disable(adev);
    }
  }
}

static const char *voice_trigger_audio_stream_name(struct audio_device *adev)
{
  /* no need to lock adev because we only need the instantaneous state */
  switch (adev->voice_st) {
    case eVoiceNone:
    case eVoiceTriggerIdle:
    case eVoiceTriggerArmed:
    case eVoiceTriggerFired:
      /* trigger-only hardware, so attempt to open a specific
       * voice recognition stream
       */
      return kVoiceRecogStreamName;

    case eVoiceRecogIdle:
    case eVoiceRecogArmed:
    case eVoiceRecogReArm:
      /* recognizer has not fired, do not open the dedicated audio stream
       * because there will not be any audio available from it. Fall
       * back to just opening the normal recording path
       */
      return NULL;

    case eVoiceRecogFired:
      /* recognizer has fired so audio will be available from it */
      return kVoiceRecogStreamName;

    case eVoiceRecogAudio:
      /* should never get here, state says audio stream is already open */
      return NULL;

    default:
      /* stops compiler warning */
      return NULL;
  }
}

static void voice_trigger_init(struct audio_device *adev)
{
  if (is_named_stream_defined(adev->cm, kVoiceRecogStreamName)) {
    ALOGV("Voice recognition mode");
    adev->voice_st = eVoiceRecogIdle;
  } else if (is_named_stream_defined(adev->cm, kVoiceTriggerStreamName)) {
    ALOGV("Voice trigger mode");
    adev->voice_st = eVoiceTriggerIdle;
  } else {
    ALOGV("no voice recognition available");
    adev->voice_st = eVoiceNone;
  }
}

/*********************************************************************
 * Global API functions
 *********************************************************************/
static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
  struct audio_device *adev = (struct audio_device *)dev;
  struct str_parms *parms = NULL;
  char *str = NULL;
  char value[32];

  ALOGW("adev_set_parameters '%s'", kvpairs);

  parms = str_parms_create_str(kvpairs);
  if (parms) {
    voice_trigger_set_params(adev, parms);
    str_parms_destroy(parms);
  }

  if (adev->global_stream != NULL)
    stream_invoke_usecases(adev->global_stream, kvpairs);

  return 0;
}

static char * adev_get_parameters(const struct audio_hw_device *dev,
                                  const char *keys)
{
  UNUSED(dev);
  UNUSED(keys);
  return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev)
{
  UNUSED(dev);
  return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
  UNUSED(dev);
  UNUSED(volume);
  return 0; //Stub
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
  UNUSED(dev);
  UNUSED(volume);
  return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
  UNUSED(dev);
  switch (mode) {
    case AUDIO_MODE_NORMAL:
      break;
    case AUDIO_MODE_RINGTONE:
      break;
    case AUDIO_MODE_IN_CALL:
      break;
    case AUDIO_MODE_IN_COMMUNICATION:
      break;
    default:
      break;
  };

  return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
  struct audio_device *adev = (struct audio_device *)dev;

  adev->mic_mute = state;

  return 0;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
  struct audio_device *adev = (struct audio_device *)dev;

  *state = adev->mic_mute;

  return 0;
}

static int adev_set_master_mute(struct audio_hw_device *dev, bool mute)
{
  UNUSED(dev);
  UNUSED(mute);
  return -ENOSYS;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
    const struct audio_config *config)
{
  size_t s = IN_PERIOD_SIZE_DEFAULT *
             audio_bytes_per_sample(config->format) *
             popcount(config->channel_mask);
  UNUSED(dev);

  if (s > IN_COMPRESS_BUFFER_SIZE_DEFAULT) {
    s = IN_COMPRESS_BUFFER_SIZE_DEFAULT;
  }

  return s;
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
  UNUSED(device);
  UNUSED(fd);
  return 0;
}

static int adev_close(hw_device_t *device)
{
  struct audio_device *adev = (struct audio_device *)device;

  free_audio_config(adev->cm);

  free(device);
  return 0;
}

static int adev_get_microphones(const struct audio_hw_device *dev,
                                struct audio_microphone_characteristic_t *mic_array,
                                size_t *mic_count) {
    UNUSED(dev);
    UNUSED(mic_array);
    UNUSED(mic_count);
    *mic_count = 0;
    return 0; //Stub
}

static int adev_open(const hw_module_t *module, const char *name,
                     hw_device_t **device)
{
  struct audio_device *adev = NULL;
  int ret = 0;
  char prop_value[PROPERTY_VALUE_MAX] = { 0 };

  if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
    return -EINVAL;

  adev = calloc(1, sizeof(struct audio_device));
  if (!adev)
    return -ENOMEM;

  adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
  adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
  adev->hw_device.common.module = (struct hw_module_t *) module;
  adev->hw_device.common.close = adev_close;

  adev->hw_device.get_supported_devices = NULL;
  adev->hw_device.init_check = adev_init_check;
  adev->hw_device.set_voice_volume = adev_set_voice_volume;
  adev->hw_device.set_master_volume = adev_set_master_volume;
  adev->hw_device.get_master_volume = NULL;
  adev->hw_device.set_mode = adev_set_mode;
  adev->hw_device.set_mic_mute = adev_set_mic_mute;
  adev->hw_device.get_mic_mute = adev_get_mic_mute;
  adev->hw_device.set_parameters = adev_set_parameters;
  adev->hw_device.get_parameters = adev_get_parameters;
  adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
  adev->hw_device.open_output_stream = adev_open_output_stream;
  adev->hw_device.close_output_stream = adev_close_output_stream;
  adev->hw_device.open_input_stream = adev_open_input_stream;
  adev->hw_device.close_input_stream = adev_close_input_stream;
  adev->hw_device.get_microphones = adev_get_microphones; //Stub
  adev->hw_device.dump = adev_dump;
  adev->hw_device.set_master_mute = adev_set_master_mute;
  adev->hw_device.get_master_mute = NULL;
  adev->hw_device.create_audio_patch = NULL;
  adev->hw_device.release_audio_patch = NULL;
  adev->hw_device.get_audio_port = NULL;
  adev->hw_device.set_audio_port_config = NULL;

  adev->cm = init_audio_config();
  if (!adev->cm) {
    ret = -EINVAL;
    goto fail;
  }

  adev->global_stream = get_named_stream(adev->cm, "global");
  voice_trigger_init(adev);

  property_get(PROP_AUDIO_CONFIG, prop_value, "false");
  if (strcmp(prop_value, "true") == 0) {
    // If audio is disabled, use fake device instead of ALSA pcm device
    adev->disable_audio = true;
  } else {
    adev->disable_audio = false;
  }

  *device = &adev->hw_device.common;
  return 0;

fail:
  if (adev->cm) {
    /*free_audio_config(adev->cm);*/ /* Currently broken */
  }

  free(adev);
  return ret;
}

static struct hw_module_methods_t hal_module_methods = {
  .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
  .common = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
    .hal_api_version = HARDWARE_HAL_API_VERSION,
    .id = AUDIO_HARDWARE_MODULE_ID,
    .name = "TinyHAL",
    .author = "Richard Fitzgerald <rf@opensource.wolfsonmicro.com>",
    .methods = &hal_module_methods,
  },
};
