#include <obs-module.h>
#include <util/threading.h>
#include <util/dstr.h>
#include <util/circlebuf.h>
#include "plugin-macros.generated.h"
#include "audio-hook-buffer.h"

#define MAX_DIFF_SAMPLES 24000
#define PLOT_HEIGHT 128

struct comparator_s
{
	gs_effect_t *effect;
	bool effect_failed;

	struct audio_hook_buffer src1, src2;

	// data in graphics thread
	float *buf1, *buf2;
	size_t buf_size;   // [samples] for each buf
	size_t buf_stride; // [bytes]

	gs_texture_t *audio_tex;
	size_t audio_tex_width;

	gs_texture_t *prev_correlation;
	gs_texrender_t *correlation_texrender;
	float video_s;

	// properties
	char *source1_name;
	char *source2_name;
	size_t window;
	size_t range;
	float decay_s;
	bool add_sync_offset;
};

static const char *get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("Source.Name");
}

struct source_prop_info
{
	obs_property_t *prop;
};

static bool add_sources(void *data, obs_source_t *source)
{
	struct source_prop_info *info = data;

	uint32_t caps = obs_source_get_output_flags(source);
	if (!(caps & OBS_SOURCE_AUDIO))
		return true;

	const char *name = obs_source_get_name(source);
	obs_property_list_add_string(info->prop, name, name);

	return true;
}

static obs_properties_t *get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop;

	struct source_prop_info info;

	prop = obs_properties_add_list(props, "source1_name", obs_module_text("Properties.Source1"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	info.prop = prop;
	obs_enum_sources(add_sources, &info);

	prop = obs_properties_add_list(props, "source2_name", obs_module_text("Properties.Source2"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	info.prop = prop;
	obs_enum_sources(add_sources, &info);

	obs_properties_add_bool(props, "add_sync_offset", obs_module_text("Properties.AddSyncOffset"));

	prop = obs_properties_add_int(props, "window_ms", obs_module_text("Properties.Window"), 1, 1000, 1);
	obs_property_int_set_suffix(prop, " ms");
	prop = obs_properties_add_int(props, "range_ms", obs_module_text("Properties.Range"), 1, 1000, 1);
	obs_property_int_set_suffix(prop, " ms");

	prop = obs_properties_add_float(props, "decay_s", obs_module_text("Properties.Decay"), 0.1, 100.0, 0.1);
	obs_property_float_set_suffix(prop, " s");

	return props;
}

static void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "add_sync_offset", true);
	obs_data_set_default_int(settings, "window_ms", 33);
	obs_data_set_default_int(settings, "range_ms", 200);
	obs_data_set_default_double(settings, "decay_s", 1.0);
}

static size_t ms_to_samples(long long ms, size_t min, size_t max)
{
	uint32_t sample_rate = audio_output_get_sample_rate(obs_get_audio());
	size_t ret = (size_t)(ms * sample_rate / 1000);
	if (ret < min)
		return min;
	if (ret > max)
		return max;
	return ret;
}

static void update(void *data, obs_data_t *settings)
{
	struct comparator_s *s = data;

	const char *source1_name = obs_data_get_string(settings, "source1_name");
	if (source1_name && (!s->source1_name || strcmp(source1_name, s->source1_name))) {
		bfree(s->source1_name);
		s->source1_name = bstrdup(source1_name);
		ahb_set_source(&s->src1, source1_name);
	}

	const char *source2_name = obs_data_get_string(settings, "source2_name");
	if (source2_name && (!s->source2_name || strcmp(source2_name, s->source2_name))) {
		bfree(s->source2_name);
		s->source2_name = bstrdup(source2_name);
		ahb_set_source(&s->src2, source2_name);
	}

	s->add_sync_offset = obs_data_get_bool(settings, "add_sync_offset");

	s->window = ms_to_samples(obs_data_get_int(settings, "window_ms"), 1, 48000);
	s->range = ms_to_samples(obs_data_get_int(settings, "range_ms"), 1, 48000);
	s->decay_s = (float)obs_data_get_double(settings, "decay_s");
}

static void *create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(source);
	struct comparator_s *s = bzalloc(sizeof(struct comparator_s));

	ahb_init(&s->src1);
	ahb_init(&s->src2);

	update(s, settings);

	return s;
}

static void destroy(void *data)
{
	struct comparator_s *s = data;

	ahb_free(&s->src2);
	ahb_free(&s->src1);
	bfree(s->buf1);

	obs_enter_graphics();
	gs_texture_destroy(s->audio_tex);
	gs_texrender_destroy(s->correlation_texrender);
	gs_texture_destroy(s->prev_correlation);
	obs_leave_graphics();

	bfree(s->source1_name);
	bfree(s->source2_name);

	bfree(s);
}

static void video_tick(void *data, float seconds)
{
	struct comparator_s *s = data;

	s->video_s += seconds;

	/* When the source is created at the beginning, it might fail to find the source. */
	if (s->source1_name && !s->src1.weak)
		ahb_set_source(&s->src1, s->source1_name);
	if (s->source2_name && !s->src2.weak)
		ahb_set_source(&s->src2, s->source2_name);

	ahb_release_old_buffer(&s->src1, s->window + s->range + MAX_DIFF_SAMPLES);
	ahb_release_old_buffer(&s->src2, s->window + s->range + MAX_DIFF_SAMPLES);
}

static inline void prepare_buffer(struct comparator_s *s)
{
	size_t req = s->window + s->range;
	if (s->buf_size < req) {
		bfree(s->buf1);
		s->buf1 = bmalloc(sizeof(float) * req * 2);
		s->buf_size = req;
	}
	s->buf2 = s->buf1 + req;
}

static inline int64_t mul_div_s64(int64_t num, uint64_t mul, uint64_t div)
{
	if (num > 0)
		return util_mul_div64((uint64_t)num, mul, div);
	else if (num < 0)
		return -(int64_t)util_mul_div64((uint64_t)-num, mul, div);
	else
		return 0;
}

static inline void copy_to_buffer(struct comparator_s *s)
{
	uint32_t sample_rate = audio_output_get_sample_rate(obs_get_audio());

	pthread_mutex_lock(&s->src1.mutex);
	pthread_mutex_lock(&s->src2.mutex);

	int64_t diff_ts = (int64_t)s->src1.last_ts - (int64_t)s->src2.last_ts;
	if (s->add_sync_offset) {
		diff_ts += s->src1.sync_offset;
		diff_ts -= s->src2.sync_offset;
	}
	int64_t diff_sample = mul_div_s64(diff_ts, sample_rate, 1000000000);

	ahb_get_buffer_locked(&s->src1, s->buf1, s->window + s->range, diff_sample > 0 ? diff_sample : 0);
	ahb_get_buffer_locked(&s->src2, s->buf2, s->window + s->range, diff_sample < 0 ? -diff_sample : 0);

	pthread_mutex_unlock(&s->src2.mutex);
	pthread_mutex_unlock(&s->src1.mutex);
}

static inline void send_audio_buffer(struct comparator_s *s)
{
	size_t width = s->window + s->range;

	if (width != s->audio_tex_width) {
		gs_texture_destroy(s->audio_tex);
		s->audio_tex = NULL;
	}

	if (!s->audio_tex) {
		s->audio_tex = gs_texture_create(width, 2, GS_R32F, 1, (const uint8_t **)&s->buf1, GS_DYNAMIC);
		s->audio_tex_width = width;
	}
	else {
		gs_texture_set_image(s->audio_tex, (const uint8_t *)s->buf1, width * sizeof(float), false);
	}
}

static inline void calculate_correlation(struct comparator_s *s)
{
	if (!s->effect) {
		char *name = obs_module_file("correlation.effect");

		s->effect = gs_effect_create_from_file(name, NULL);
		if (!s->effect && !s->effect_failed) {
			s->effect_failed = true;
			blog(LOG_ERROR, "Cannot open '%s'", name);
		}
		bfree(name);

		if (!s->effect)
			return;
	}

	if (s->range < 1)
		return;
	size_t width = s->range * 2;
	bool first = false;

	if (s->prev_correlation && gs_texture_get_width(s->prev_correlation) != width) {
		gs_texture_destroy(s->prev_correlation);
		s->prev_correlation = NULL;
		first = true;
	}
	if (!s->prev_correlation)
		s->prev_correlation = gs_texture_create(width, 1, GS_RGBA32F, 1, NULL, GS_RENDER_TARGET);

	if (!s->correlation_texrender) {
		s->correlation_texrender = gs_texrender_create(GS_RGBA32F, GS_ZS_NONE);
		first = true;
	}
	else {
		gs_copy_texture(s->prev_correlation, gs_texrender_get_texture(s->correlation_texrender));
		gs_texrender_reset(s->correlation_texrender);
	}

	if (!gs_texrender_begin(s->correlation_texrender, width, 1))
		return;

	struct vec4 background = {{{0.0f, 0.0f, 0.0f, 0.0f}}};
	gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);

	gs_ortho(0.0f, (float)width, 0.0f, (float)1, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	gs_effect_set_texture(gs_effect_get_param_by_name(s->effect, "image"), s->audio_tex);
	gs_effect_set_texture(gs_effect_get_param_by_name(s->effect, "image_prev"), s->prev_correlation);
	gs_effect_set_int(gs_effect_get_param_by_name(s->effect, "range"), s->range);
	gs_effect_set_float(gs_effect_get_param_by_name(s->effect, "decay"),
			    first ? 2.0f : s->video_s / (s->decay_s + s->video_s));
	s->video_s = 0.0f;
	while (gs_effect_loop(s->effect, "DrawCorrelation")) {
		gs_draw_sprite(s->audio_tex, 0, width, 1);
	}

	gs_blend_state_pop();

	gs_texrender_end(s->correlation_texrender);
}

static inline void render_audio_buffer(struct comparator_s *s)
{
	size_t width = s->range * 2;

	gs_texture_t *tex = gs_texrender_get_texture(s->correlation_texrender);
	gs_effect_set_int(gs_effect_get_param_by_name(s->effect, "range"), s->range);
	gs_effect_set_texture(gs_effect_get_param_by_name(s->effect, "image"), tex);
	gs_effect_set_texture(gs_effect_get_param_by_name(s->effect, "image_prev"), tex);
	gs_effect_set_float(gs_effect_get_param_by_name(s->effect, "decay"), 0.0f);

	while (gs_effect_loop(s->effect, "DrawWaveform")) {
		gs_draw_sprite(tex, 0, width, PLOT_HEIGHT);
	}
}

static void video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct comparator_s *s = data;

	prepare_buffer(s);
	copy_to_buffer(s);
	send_audio_buffer(s);
	calculate_correlation(s);
	render_audio_buffer(s);
}

static uint32_t get_width(void *data)
{
	struct comparator_s *s = data;
	return s->range * 2;
}

static uint32_t get_height(void *data)
{
	UNUSED_PARAMETER(data);
	return PLOT_HEIGHT;
}

const struct obs_source_info audio_latency_comparator = {
	.id = ID_PREFIX "source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = get_name,
	.create = create,
	.destroy = destroy,
	.get_properties = get_properties,
	.get_defaults = get_defaults,
	.update = update,
	.video_tick = video_tick,
	.video_render = video_render,
	.get_width = get_width,
	.get_height = get_height,
};
