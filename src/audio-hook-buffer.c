#include <obs-module.h>
#ifdef _WIN32
#include <malloc.h>
#define alloca _alloca
#else
#include <alloca.h>
#endif
#include "plugin-macros.generated.h"
#include "audio-hook-buffer.h"

static void audio_callback(void *param, obs_source_t *source, const struct audio_data *audio_data, bool muted);

static inline void hook_source(struct audio_hook_buffer *ahb, obs_source_t *src)
{
	obs_source_add_audio_capture_callback(src, audio_callback, ahb);
}

static inline void unhook_source(struct audio_hook_buffer *ahb, obs_source_t *src)
{
	obs_source_remove_audio_capture_callback(src, audio_callback, ahb);
}

void ahb_set_source(struct audio_hook_buffer *ahb, const char *name)
{
	if (ahb->weak) {
		obs_source_t *src = obs_weak_source_get_source(ahb->weak);
		if (src)
			unhook_source(ahb, src);
		obs_source_release(src);
		obs_weak_source_release(ahb->weak);
		ahb->weak = NULL;
	}

	if (!name)
		return;

	obs_source_t *src = obs_get_source_by_name(name);
	if (src) {
		ahb->weak = obs_source_get_weak_source(src);

		hook_source(ahb, src);

		obs_source_release(src);
	}
}

static void audio_callback(void *param, obs_source_t *source, const struct audio_data *audio_data, bool muted)
{
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(muted);
	struct audio_hook_buffer *ahb = param;

	uint32_t sample_rate = audio_output_get_sample_rate(obs_get_audio());
	uint64_t frames_ns = util_mul_div64(audio_data->frames, 1000000000, sample_rate);

	float *mono = alloca(sizeof(float) * audio_data->frames);

	size_t channels = 1;
	memcpy(mono, audio_data->data[0], sizeof(float) * audio_data->frames);

	for (size_t ch = 1; ch < MAX_AV_PLANES; ch++) {
		if (!audio_data->data[ch])
			break;
		const float *data = (const float *)(audio_data->data[ch]);
		for (size_t i = 0; i < audio_data->frames; i++)
			mono[i] += data[i];
		channels++;
	}

	float k = 1.0f / channels;
	for (size_t i = 0; i < audio_data->frames; i++)
		mono[i] *= k;

	pthread_mutex_lock(&ahb->mutex);
	circlebuf_push_back(&ahb->buffer, mono, sizeof(float) * audio_data->frames);
	ahb->last_ts = audio_data->timestamp + frames_ns;
	pthread_mutex_unlock(&ahb->mutex);
}

void ahb_get_buffer_locked(struct audio_hook_buffer *ahb, float *buffer, size_t size, size_t offset)
{
	/* Delete the front unnecessary data. */
	ahb_release_old_buffer(ahb, size + offset);

	/* Fill zero if circle buffer is smaller. */
	while (ahb->buffer.size / sizeof(float) < size + offset) {
		*buffer++ = 0.0f;
		size -= 1;

		if (!size)
			return;
	}

	circlebuf_peek_front(&ahb->buffer, buffer, sizeof(float) * size);
}

void ahb_release_old_buffer(struct audio_hook_buffer *ahb, size_t size)
{
	size_t size_byte = sizeof(float) * size;

	if (ahb->buffer.size > size_byte) {
		circlebuf_pop_front(&ahb->buffer, NULL, ahb->buffer.size - size_byte);
	}
}
