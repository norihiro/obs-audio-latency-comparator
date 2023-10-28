#pragma once

#include <util/threading.h>
#include <util/circlebuf.h>

struct audio_hook_buffer
{
	obs_weak_source_t *weak;

	// context data, need to lock `mutex`
	struct circlebuf buffer;
	uint64_t last_ts;

	pthread_mutex_t mutex;
};

/**
 * Set the source to hook the audio by name.
 *
 * If `name` is NULL, it reset hook state.
 *
 * @param ahb   Context
 * @param name  Name of the source
 */
void ahb_set_source(struct audio_hook_buffer *ahb, const char *name);

/**
 * Get the audio data.
 *
 * Copy [end_pos - offset - size, end_pos - offset - 1] in the circle buffer.
 * If the circle buffer has insufficient samples, 0.0f will be filled.
 * This function expects `mutex` has already been locked.
 *
 * @param ahb     Context
 * @param buffer  Destination to copy the audio
 * @param size    Size of the destination in samples
 * @param offset  Offset in samples from the end of the circle buffer
 */
void ahb_get_buffer_locked(struct audio_hook_buffer *ahb, float *buffer, size_t size, size_t offset);

/**
 * Release old audio data.
 *
 * @param ahb   Context
 * @param size  Audio samples that can still stay.
 */
void ahb_release_old_buffer(struct audio_hook_buffer *ahb, size_t size);

static inline void ahb_init(struct audio_hook_buffer *ahb)
{
	memset(ahb, 0, sizeof(struct audio_hook_buffer));
	pthread_mutex_init(&ahb->mutex, 0);
}

static inline void ahb_free(struct audio_hook_buffer *ahb)
{
	ahb_set_source(ahb, NULL);
	circlebuf_free(&ahb->buffer);
	pthread_mutex_destroy(&ahb->mutex);
}
