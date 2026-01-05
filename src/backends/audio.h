// Copyright 2021 Jonne Kokkonen
// Released under the MIT licence, https://opensource.org/licenses/MIT
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

int audio_initialize(const char *output_device_name, unsigned int audio_buffer_size);
void audio_toggle(const char *output_device_name, unsigned int audio_buffer_size);
void audio_process(void);
void audio_close(void);

// TCP audio support - feed PCM data from network
int audio_init_tcp(const char *output_device_name, unsigned int audio_buffer_size);
void audio_feed_tcp(const uint8_t *data, int length);

#endif
