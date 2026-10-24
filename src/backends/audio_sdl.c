// Copyright 2021 Jonne Kokkonen
// Released under the MIT licence, https://opensource.org/licenses/MIT
#ifndef USE_LIBUSB
#include "audio.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

SDL_AudioStream *audio_stream_in, *audio_stream_out;

// Direct ALSA output via aplay (bypass broken SDL3 ALSA)
static FILE *aplay_pipe = NULL;
static int use_aplay = 1;  // Enable aplay by default for TCP audio

static unsigned int audio_paused = 0;
static unsigned int audio_initialized = 0;
static SDL_AudioSpec audio_spec_in = {SDL_AUDIO_S16LE, 2, 44100};

static int cb_call_count = 0;
static Uint64 cb_last_log = 0;

static void SDLCALL audio_cb_out(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
  // suppress compiler warnings
  (void)userdata;

  // Log callback activity
  cb_call_count++;
  Uint64 now = SDL_GetTicks();
  if (now - cb_last_log > 5000) {
    SDL_Log("audio_cb_out: %d calls, additional=%d, total=%d",
            cb_call_count, additional_amount, total_amount);
    cb_call_count = 0;
    cb_last_log = now;
  }

  if (additional_amount <= 0) {
    return;
  }

  const int bytes_available = SDL_GetAudioStreamAvailable(audio_stream_in);
  if (bytes_available == -1) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO,
                 "Error getting available audio stream bytes: %s, destroying audio",
                 SDL_GetError());
    audio_close();
    return;
  }

  // Decide how much to feed this time.
  int to_write_goal = additional_amount;
  if (total_amount > 0) {
    const int prefill_cap = additional_amount * 2;
    if (to_write_goal < total_amount) {
      to_write_goal = SDL_min(total_amount, prefill_cap);
    }
  }

  int to_write = to_write_goal;
  Uint8 temp[4096];

  // Debug: check available before loop
  int avail_before = SDL_GetAudioStreamAvailable(audio_stream_in);
  static int debug_count = 0;
  if (debug_count++ < 20) {
    SDL_Log("CB: avail_before=%d, to_write=%d", avail_before, to_write);
  }

  while (to_write > 0) {
    int still_avail = SDL_GetAudioStreamAvailable(audio_stream_in);
    if (still_avail <= 0) {
      break; // nothing more to pull now
    }

    int chunk = still_avail;
    if (chunk > (int)sizeof(temp)) chunk = (int)sizeof(temp);
    if (chunk > to_write) chunk = to_write;

    const int got = SDL_GetAudioStreamData(audio_stream_in, temp, chunk);
    if (got == -1) {
      SDL_LogError(SDL_LOG_CATEGORY_AUDIO,
                   "Error reading audio stream data: %s, destroying audio",
                   SDL_GetError());
      audio_close();
      return;
    }
    if (got == 0) {
      break; // no data currently available
    }

    if (!SDL_PutAudioStreamData(stream, temp, got)) {
      SDL_LogError(SDL_LOG_CATEGORY_AUDIO,
                   "Error putting audio stream data: %s, destroying audio",
                   SDL_GetError());
      audio_close();
      return;
    }

    to_write -= got;
  }

  // Log how much data we actually wrote
  static int total_written = 0;
  static Uint64 write_log_time = 0;
  total_written += (to_write_goal - to_write);
  if (now - write_log_time > 5000) {
    SDL_Log("audio_cb_out: wrote %d bytes to output stream this period", total_written);
    total_written = 0;
    write_log_time = now;
  }
}

void audio_toggle(const char *output_device_name, unsigned int audio_buffer_size) {
  if (!audio_initialized) {
    audio_initialize(output_device_name, audio_buffer_size);
    return;
  }
  if (audio_paused) {
    SDL_ResumeAudioStreamDevice(audio_stream_out);
    SDL_ResumeAudioStreamDevice(audio_stream_in);
  } else {
    SDL_PauseAudioStreamDevice(audio_stream_in);
    SDL_PauseAudioStreamDevice(audio_stream_out);
  }
  audio_paused = !audio_paused;
  SDL_Log(audio_paused ? "Audio paused" : "Audio resumed");
}

int audio_initialize(const char *output_device_name, const unsigned int audio_buffer_size) {

  int num_devices_in, num_devices_out;
  SDL_AudioDeviceID m8_device_id = 0;
  SDL_AudioDeviceID output_device_id = 0;

  if (SDL_Init(SDL_INIT_AUDIO) == false) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "SDL Audio init failed, SDL Error: %s", SDL_GetError());
    return 0;
  }

  SDL_AudioDeviceID *devices_in = SDL_GetAudioRecordingDevices(&num_devices_in);
  if (!devices_in) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "No audio capture devices, SDL Error: %s", SDL_GetError());
    return 0;
  }

  SDL_AudioDeviceID *devices_out = SDL_GetAudioPlaybackDevices(&num_devices_out);
  if (!devices_out) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "No audio playback devices, SDL Error: %s",
                 SDL_GetError());
    return 0;
  }

  SDL_LogDebug(SDL_LOG_CATEGORY_AUDIO, "Audio input devices:");
  for (int i = 0; i < num_devices_in; i++) {
    const SDL_AudioDeviceID instance_id = devices_in[i];
    const char *device_name = SDL_GetAudioDeviceName(instance_id);
    SDL_LogDebug(SDL_LOG_CATEGORY_AUDIO, "%s", device_name);
    if (SDL_strstr(device_name, "M8") != NULL) {
      SDL_LogInfo(SDL_LOG_CATEGORY_AUDIO, "M8 Audio Input device found: %s", device_name);
      m8_device_id = instance_id;
    }
  }

  if (output_device_name != NULL) {
    for (int i = 0; i < num_devices_out; i++) {
      const SDL_AudioDeviceID instance_id = devices_out[i];
      const char *device_name = SDL_GetAudioDeviceName(instance_id);
      SDL_LogDebug(SDL_LOG_CATEGORY_AUDIO, "%s", device_name);
      if (SDL_strcasestr(device_name, output_device_name) != NULL) {
        SDL_Log("Requested output device found: %s", device_name);
        output_device_id = instance_id;
      }
    }
  }

  SDL_free(devices_in);
  SDL_free(devices_out);

  if (!output_device_id) {
    output_device_id = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
  }

  if (!m8_device_id) {
    // forget about it
    SDL_Log("Cannot find M8 audio input device");
    return 0;
  }

  char audio_buffer_size_str[256];
  SDL_snprintf(audio_buffer_size_str, sizeof(audio_buffer_size_str), "%d", audio_buffer_size);
  if (audio_buffer_size > 0) {
    SDL_LogInfo(SDL_LOG_CATEGORY_AUDIO, "Setting requested audio device sample frames to %d",
                audio_buffer_size);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, audio_buffer_size_str);
  }

  audio_stream_out = SDL_OpenAudioDeviceStream(output_device_id, NULL, audio_cb_out, NULL);

  SDL_AudioSpec audio_spec_out;
  int audio_out_buffer_size_real, audio_in_buffer_size_real = 0;

  SDL_GetAudioDeviceFormat(output_device_id, &audio_spec_out, &audio_out_buffer_size_real);

  if (!audio_stream_out) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Error opening audio output device: %s", SDL_GetError());
    return 0;
  }
  SDL_LogInfo(SDL_LOG_CATEGORY_AUDIO,
              "Opening audio output: rate %dhz, buffer size: %d frames", audio_spec_out.freq,
              audio_out_buffer_size_real);

  audio_stream_in = SDL_OpenAudioDeviceStream(m8_device_id, &audio_spec_in, NULL, NULL);
  if (!audio_stream_in) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Error opening audio input device: %s", SDL_GetError());
    SDL_DestroyAudioStream(audio_stream_out);
    return 0;
  }

  SDL_SetAudioStreamFormat(audio_stream_in, &audio_spec_in, &audio_spec_out);
  SDL_GetAudioDeviceFormat(m8_device_id, &audio_spec_in, &audio_in_buffer_size_real);
  SDL_LogDebug(SDL_LOG_CATEGORY_AUDIO, "Audiospec In: format %d, channels %d, rate %d, buffer size %d frames",
               audio_spec_in.format, audio_spec_in.channels, audio_spec_in.freq, audio_in_buffer_size_real);



  SDL_ResumeAudioStreamDevice(audio_stream_out);
  SDL_ResumeAudioStreamDevice(audio_stream_in);

  audio_paused = 0;
  audio_initialized = 1;

  return 1;
}

void audio_close(void) {
  if (!audio_initialized)
    return;
  SDL_Log("Closing audio devices");

  // Close aplay pipe if used
  if (aplay_pipe) {
    pclose(aplay_pipe);
    aplay_pipe = NULL;
  }

  if (audio_stream_in && audio_stream_in != audio_stream_out) {
    SDL_DestroyAudioStream(audio_stream_in);
  }
  if (audio_stream_out) {
    SDL_DestroyAudioStream(audio_stream_out);
  }
  audio_stream_in = NULL;
  audio_stream_out = NULL;

  SDL_QuitSubSystem(SDL_INIT_AUDIO);
  audio_initialized = 0;
}

// TCP audio support - initialize output only (no M8 input device needed)
int audio_init_tcp(const char *output_device_name, const unsigned int audio_buffer_size) {
  if (audio_initialized) {
    return 1;
  }

  int num_devices_out;
  SDL_AudioDeviceID output_device_id = 0;

  if (SDL_Init(SDL_INIT_AUDIO) == false) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "SDL Audio init failed: %s", SDL_GetError());
    return 0;
  }

  SDL_AudioDeviceID *devices_out = SDL_GetAudioPlaybackDevices(&num_devices_out);
  if (!devices_out) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "No audio playback devices: %s", SDL_GetError());
    return 0;
  }

  if (output_device_name != NULL) {
    for (int i = 0; i < num_devices_out; i++) {
      const SDL_AudioDeviceID instance_id = devices_out[i];
      const char *device_name = SDL_GetAudioDeviceName(instance_id);
      if (SDL_strcasestr(device_name, output_device_name) != NULL) {
        SDL_Log("TCP Audio: Using output device: %s", device_name);
        output_device_id = instance_id;
        break;
      }
    }
  }
  SDL_free(devices_out);

  if (!output_device_id) {
    output_device_id = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
  }

  char audio_buffer_size_str[256];
  SDL_snprintf(audio_buffer_size_str, sizeof(audio_buffer_size_str), "%d", audio_buffer_size);
  if (audio_buffer_size > 0) {
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, audio_buffer_size_str);
  }

  // Use aplay for direct ALSA output (bypass SDL3 ALSA which is broken on ARM)
  if (use_aplay) {
    // Start aplay as a subprocess, reading from stdin
    // Format: S16_LE, 44100Hz, stereo
    // Low-latency: buffer=4096 (~23ms), period=512 (~3ms)
    // For unstable networks, increase to buffer=8192, period=1024
    // Use 'default' ALSA device (goes through dmix, allows device sharing)
    // 'hw:0,0' would fail with "Device or resource busy" if alsaloop or other process has it open
    aplay_pipe = popen("aplay -D default -f S16_LE -r 44100 -c 2 -t raw --buffer-size=4096 --period-size=512 - 2>/dev/null", "w");
    if (!aplay_pipe) {
      SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Failed to start aplay: %s", strerror(errno));
      // Fall back to SDL3
      use_aplay = 0;
    } else {
      SDL_Log("TCP Audio using aplay (direct ALSA, S16_LE 44100Hz stereo)");
      audio_stream_in = NULL;  // Not used in aplay mode
      audio_stream_out = NULL;
      audio_paused = 0;
      audio_initialized = 1;
      return 1;
    }
  }

  // Fallback: Use SDL_OpenAudioDeviceStream in push mode (no callback)
  audio_stream_out = SDL_OpenAudioDeviceStream(output_device_id, &audio_spec_in, NULL, NULL);
  if (!audio_stream_out) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Error opening audio output stream: %s", SDL_GetError());
    return 0;
  }

  // Get the ACTUAL output device format
  SDL_AudioSpec audio_spec_out;
  int out_buffer_size;
  SDL_AudioDeviceID actual_device = SDL_GetAudioStreamDevice(audio_stream_out);
  SDL_GetAudioDeviceFormat(actual_device, &audio_spec_out, &out_buffer_size);
  SDL_Log("TCP Audio output (actual): %d Hz, %d channels, format %d, buffer %d",
          audio_spec_out.freq, audio_spec_out.channels, audio_spec_out.format, out_buffer_size);

  // Use the same stream for input (data pushed directly to device stream)
  audio_stream_in = audio_stream_out;

  SDL_ResumeAudioStreamDevice(audio_stream_out);
  SDL_Log("TCP Audio initialized in push mode (input: S16LE 44100Hz stereo)");

  audio_paused = 0;
  audio_initialized = 1;
  return 1;
}

// Feed PCM data from TCP to audio output
static int tcp_audio_packet_count = 0;
static int tcp_audio_bytes_written = 0;
static Uint64 tcp_audio_last_log = 0;

void audio_feed_tcp(const uint8_t *data, int length) {
  if (!audio_initialized || audio_paused) {
    return;
  }

  // Use aplay pipe if available
  if (aplay_pipe) {
    size_t written = fwrite(data, 1, length, aplay_pipe);
    fflush(aplay_pipe);

    // Log every 5 seconds
    tcp_audio_packet_count++;
    tcp_audio_bytes_written += written;
    Uint64 now = SDL_GetTicks();
    if (now - tcp_audio_last_log > 5000) {
      SDL_Log("TCP Audio (aplay): %d pkts, %d bytes written",
              tcp_audio_packet_count, tcp_audio_bytes_written);
      tcp_audio_packet_count = 0;
      tcp_audio_bytes_written = 0;
      tcp_audio_last_log = now;
    }
    return;
  }

  // Fallback to SDL3 stream
  if (!audio_stream_in) {
    return;
  }

  int result = SDL_PutAudioStreamData(audio_stream_in, data, length);

  // Log every 5 seconds
  tcp_audio_packet_count++;
  Uint64 now = SDL_GetTicks();
  if (now - tcp_audio_last_log > 5000) {
    int available = SDL_GetAudioStreamAvailable(audio_stream_in);
    SDL_Log("TCP Audio: %d pkts, last=%d bytes, stream=%d bytes available, put result=%d",
            tcp_audio_packet_count, length, available, result);
    tcp_audio_packet_count = 0;
    tcp_audio_last_log = now;
  }
}

#endif
