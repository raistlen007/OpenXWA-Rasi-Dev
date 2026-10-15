/* State-derived HD rendering for XWA's three hyperspace phases. */

#include "xwa_remaster/hyperspace.h"

#include "aeron/aeron.h"
#include "aeron/asset/opt_model.h"
#include "xwa_runtime/timing/host_clock.h"
#include "xwa_remaster/color.h"
#include "xwa_remaster/flight.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct HyperStreakVertex {
	float position[3];
	float color[4];
	float center_position[3];
	float shape_uv[3];
} HyperStreakVertex;

/* A viewport-aware projection allows thin quads to cover the AA fringe. */
typedef struct HyperStreakUniform {
	float view_proj[16];
	float viewport[4];
} HyperStreakUniform;
typedef char HyperStreakUniformSizeCheck[sizeof(HyperStreakUniform) == 80 ? 1 : -1];

typedef struct HyperTunnelUniform {
	float view[4];
	float projection[4];
	float motion[4];
	float appearance[4];
	float geometry[4];
	float tunnel_right[4];
	float tunnel_up[4];
	float tunnel_forward[4];
	float dark_color[4];
	float body_color[4];
	float highlight_color[4];
	float cap_color[4];
} HyperTunnelUniform;

typedef struct HyperEnvironmentUniform {
	float roughness;
	float _pad[3];
} HyperEnvironmentUniform;
typedef char HyperEnvironmentUniformSizeCheck[sizeof(HyperEnvironmentUniform) == 16 ? 1 : -1];

enum {
	HYPER_ENVIRONMENT_FACE_SIZE = 32,
	HYPER_ENVIRONMENT_ATLAS_WIDTH = HYPER_ENVIRONMENT_FACE_SIZE * 6,
};

struct XwaRemasterHyperspace {
	AeronShader* streak_vs;
	AeronShader* streak_fs;
	AeronShader* tunnel_vs;
	AeronShader* tunnel_fs;
	AeronGraphicsPipeline* streak_pipeline;
	AeronGraphicsPipeline* tunnel_pipeline;
	AeronComputePipeline* environment_pipeline;
	AeronTexture* environment_atlas;
	AeronTexture* environment_cube;
	AeronSampler* environment_sampler;
	AeronSampleCount pipeline_samples;
	AeronBuffer* streak_vb;
	uint32_t streak_vb_capacity;
	uint32_t streak_vertex_count;
	uint8_t draw_background;
	HyperStreakUniform streak_uniform;
	XwaFlightHyperspaceTunnelParams params;
	XwaFlightHyperspaceStreakParams streak_tuning;
	int streak_preview;
	/* Keep one white share for the entire real outbound hyperspace entry. */
	float entry_white_share;
	uint32_t entry_sequence;
	uint32_t last_phase_ticks;
	uint8_t last_phase;
	int last_was_streak_preview;
	int entry_white_share_valid;
	HyperTunnelUniform tunnel_uniform;
	XwaRemasterHyperspaceLighting lighting;
};

static AeronBlendStateDesc hyper_blend_additive(void) {
	return (AeronBlendStateDesc) {
		.enabled = 1,
		.src_color = AERON_BLEND_ONE,
		.dst_color = AERON_BLEND_ONE,
		.color_op = AERON_BLEND_OP_ADD,
		.src_alpha = AERON_BLEND_ZERO,
		.dst_alpha = AERON_BLEND_ONE,
		.alpha_op = AERON_BLEND_OP_ADD,
	};
}

static AeronBlendStateDesc hyper_blend_pma(void) {
	return (AeronBlendStateDesc) {
		.enabled = 1,
		.src_color = AERON_BLEND_ONE,
		.dst_color = AERON_BLEND_ONE_MINUS_SRC_ALPHA,
		.color_op = AERON_BLEND_OP_ADD,
		.src_alpha = AERON_BLEND_ONE,
		.dst_alpha = AERON_BLEND_ONE_MINUS_SRC_ALPHA,
		.alpha_op = AERON_BLEND_OP_ADD,
	};
}

static AeronGraphicsPipeline* hyper_create_pipeline(AeronShader* vs, AeronShader* fs, uint32_t stride,
													const AeronVertexAttributeDesc* attrs,
													uint32_t attr_count, AeronBlendStateDesc blend,
													AeronSampleCount sample_count) {
	const AeronVertexBufferLayoutDesc layout = { .slot = 0, .stride = stride };
	AeronColorTargetStateDesc targets[1] = { 0 };
	targets[0].format = AERON_TEXTURE_FORMAT_RGBA16_FLOAT;
	targets[0].blend = blend;
	return Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
		.vertex_shader = vs,
		.fragment_shader = fs,
		.primitive_type = AERON_PRIMITIVE_TRIANGLES,
		.cull_mode = AERON_CULL_NONE,
		.vertex_buffers = &layout,
		.vertex_buffer_count = 1,
		.attributes = attrs,
		.attribute_count = attr_count,
		.depth_format = AERON_TEXTURE_FORMAT_D32_FLOAT,
		.depth = { .depth_test = 0, .depth_write = 0, .compare = AERON_COMPARE_ALWAYS },
		.color_target_count = 1,
		.color_targets = targets,
		.sample_count = sample_count,
	});
}

static AeronGraphicsPipeline* hyper_create_fullscreen_pipeline(AeronShader* vs, AeronShader* fs,
															   AeronSampleCount sample_count) {
	AeronColorTargetStateDesc target = {
		.format = AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
		.blend = hyper_blend_pma(),
	};
	return Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
		.vertex_shader = vs,
		.fragment_shader = fs,
		.primitive_type = AERON_PRIMITIVE_TRIANGLES,
		.cull_mode = AERON_CULL_NONE,
		.depth_format = AERON_TEXTURE_FORMAT_D32_FLOAT,
		.depth = { .depth_test = 0, .depth_write = 0, .compare = AERON_COMPARE_ALWAYS },
		.color_target_count = 1,
		.color_targets = &target,
		.sample_count = sample_count,
	});
}

static void hyper_destroy_pipelines(XwaRemasterHyperspace* h) {
	if (h->streak_pipeline)
		Aeron_DestroyGraphicsPipeline(h->streak_pipeline);
	if (h->tunnel_pipeline)
		Aeron_DestroyGraphicsPipeline(h->tunnel_pipeline);
	h->streak_pipeline = NULL;
	h->tunnel_pipeline = NULL;
	h->pipeline_samples = 0;
}

static int hyper_ensure_pipelines(XwaRemasterHyperspace* h, AeronSampleCount sample_count) {
	static const AeronVertexAttributeDesc streak_attrs[] = {
		{ .location = 0,
		  .buffer_slot = 0,
		  .format = AERON_VERTEX_FORMAT_FLOAT3,
		  .offset = (uint32_t)offsetof(HyperStreakVertex, position) },
		{ .location = 1,
		  .buffer_slot = 0,
		  .format = AERON_VERTEX_FORMAT_FLOAT4,
		  .offset = (uint32_t)offsetof(HyperStreakVertex, color) },
		{ .location = 2,
		  .buffer_slot = 0,
		  .format = AERON_VERTEX_FORMAT_FLOAT3,
		  .offset = (uint32_t)offsetof(HyperStreakVertex, center_position) },
		{ .location = 3,
		  .buffer_slot = 0,
		  .format = AERON_VERTEX_FORMAT_FLOAT3,
		  .offset = (uint32_t)offsetof(HyperStreakVertex, shape_uv) },
	};
	if (h->pipeline_samples == sample_count && h->streak_pipeline && h->tunnel_pipeline) {
		return 1;
	}
	hyper_destroy_pipelines(h);
	h->streak_pipeline =
		hyper_create_pipeline(h->streak_vs, h->streak_fs, (uint32_t)sizeof(HyperStreakVertex), streak_attrs,
							  4, hyper_blend_additive(), sample_count);
	h->tunnel_pipeline = hyper_create_fullscreen_pipeline(h->tunnel_vs, h->tunnel_fs, sample_count);
	if (!h->streak_pipeline || !h->tunnel_pipeline) {
		hyper_destroy_pipelines(h);
		return 0;
	}
	h->pipeline_samples = sample_count;
	return 1;
}

XwaRemasterHyperspace* XwaRemasterHyperspace_Create(const XwaFlightHyperspaceTunnelParams* params) {
	if (!params) {
		return NULL;
	}
	XwaRemasterHyperspace* h = (XwaRemasterHyperspace*)calloc(1, sizeof *h);
	if (!h) {
		return NULL;
	}
	h->streak_vs = Aeron_CreateShader(&(AeronShaderDesc) {
		.name = "hyperspace_streak.vert", .stage = AERON_SHADER_STAGE_VERTEX, .uniform_buffer_count = 1 });
	h->streak_fs = Aeron_CreateShader(
		&(AeronShaderDesc) { .name = "hyperspace_streak.frag", .stage = AERON_SHADER_STAGE_FRAGMENT });
	h->tunnel_vs = Aeron_CreateShader(
		&(AeronShaderDesc) { .name = "hyperspace_tunnel.vert", .stage = AERON_SHADER_STAGE_VERTEX });
	h->tunnel_fs = Aeron_CreateShader(&(AeronShaderDesc) {
		.name = "hyperspace_tunnel.frag", .stage = AERON_SHADER_STAGE_FRAGMENT, .uniform_buffer_count = 1 });
	h->environment_pipeline = Aeron_CreateComputePipeline(&(AeronComputePipelineDesc) {
		.name = "hyperspace_environment.comp",
		.readwrite_storage_texture_count = 1,
		.uniform_buffer_count = 2,
		.thread_count_x = 8,
		.thread_count_y = 8,
		.thread_count_z = 1,
	});
	h->environment_atlas = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width = HYPER_ENVIRONMENT_ATLAS_WIDTH,
		.height = HYPER_ENVIRONMENT_FACE_SIZE,
		.format = AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
		.usage = AERON_TEXTURE_USAGE_COMPUTE_STORAGE_WRITE | AERON_TEXTURE_USAGE_TRANSFER_SRC,
		.debug_name = "xwa.hyperspace.environment_atlas",
	});
	h->environment_cube = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width = HYPER_ENVIRONMENT_FACE_SIZE,
		.height = HYPER_ENVIRONMENT_FACE_SIZE,
		.format = AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
		.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.cube = 1,
		.debug_name = "xwa.hyperspace.environment",
	});
	h->environment_sampler = Aeron_CreateSampler(&(AeronSamplerDesc) {
		.min_filter = AERON_FILTER_LINEAR,
		.mag_filter = AERON_FILTER_LINEAR,
		.mip_filter = AERON_FILTER_LINEAR,
		.address_u = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_v = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_w = AERON_ADDRESS_CLAMP_TO_EDGE,
	});
	if (!h->streak_vs || !h->streak_fs || !h->tunnel_vs || !h->tunnel_fs ||
		!h->environment_pipeline || !h->environment_atlas || !h->environment_cube ||
		!h->environment_sampler) {
		Aeron_LogError("xwa.remaster", "hyperspace: GPU resource creation failed");
		XwaRemasterHyperspace_Destroy(h);
		return NULL;
	}
	h->params = *params;
	h->streak_tuning.white_share = 0.6540f;
	h->streak_tuning.white_share_min = 0.42f;
	h->streak_tuning.white_share_max = 0.88f;
	h->streak_tuning.white_brightness = 2.4400f;
	h->streak_tuning.blue_brightness = 2.5900f;
	h->streak_tuning.blue_red = 0.3620f;
	h->streak_tuning.blue_green = 0.4920f;
	h->streak_tuning.density_scale = 2.1800f;
	h->streak_tuning.width_scale = 0.9500f;
	h->streak_tuning.length_scale = 1.7700f;
	h->streak_tuning.preview_seed = 3041712727u;
	h->streak_tuning.preview_ticks = 462;
	return h;
}

void XwaRemasterHyperspace_SetParams(XwaRemasterHyperspace* h,
									 const XwaFlightHyperspaceTunnelParams* params) {
	if (h && params) {
		h->params = *params;
	}
}

void XwaRemasterHyperspace_SetStreakTuning(XwaRemasterHyperspace* h,
									const XwaFlightHyperspaceStreakParams* params, int preview) {
	if (!h || !params) return;
	h->streak_tuning = *params;
	h->streak_preview = preview ? 1 : 0;
}

void XwaRemasterHyperspace_Destroy(XwaRemasterHyperspace* h) {
	if (!h) {
		return;
	}
	if (h->streak_vb)
		Aeron_DestroyBuffer(h->streak_vb);
	hyper_destroy_pipelines(h);
	if (h->streak_vs)
		Aeron_DestroyShader(h->streak_vs);
	if (h->streak_fs)
		Aeron_DestroyShader(h->streak_fs);
	if (h->tunnel_vs)
		Aeron_DestroyShader(h->tunnel_vs);
	if (h->tunnel_fs)
		Aeron_DestroyShader(h->tunnel_fs);
	if (h->environment_pipeline)
		Aeron_DestroyComputePipeline(h->environment_pipeline);
	if (h->environment_atlas)
		Aeron_DestroyTexture(h->environment_atlas);
	if (h->environment_cube)
		Aeron_DestroyTexture(h->environment_cube);
	if (h->environment_sampler)
		Aeron_DestroySampler(h->environment_sampler);
	free(h);
}

static int hyper_ensure_streak_buffer(XwaRemasterHyperspace* h, uint32_t bytes) {
	if (bytes == 0) {
		return 1;
	}
	if (h->streak_vb && h->streak_vb_capacity >= bytes) {
		return 1;
	}
	uint32_t capacity = h->streak_vb_capacity ? h->streak_vb_capacity : 64u * 1024u;
	while (capacity < bytes) {
		capacity *= 2u;
	}
	if (h->streak_vb) {
		Aeron_DestroyBuffer(h->streak_vb);
	}
	h->streak_vb = Aeron_CreateBuffer(&(AeronBufferDesc) { .size = capacity,
														   .usage = AERON_BUFFER_USAGE_VERTEX,
														   .debug_name = "xwa.hyperspace.streak_vertices" });
	h->streak_vb_capacity = h->streak_vb ? capacity : 0;
	return h->streak_vb != NULL;
}

static void hyper_widescreen_remap(float p[3], const float camera_rows[9], float x_scale) {
	if (x_scale <= 1.0f) {
		return;
	}
	float eye[3];
	for (int r = 0; r < 3; r++) {
		eye[r] =
			camera_rows[r * 3 + 0] * p[0] + camera_rows[r * 3 + 1] * p[1] + camera_rows[r * 3 + 2] * p[2];
	}
	eye[0] *= x_scale;
	for (int c = 0; c < 3; c++) {
		p[c] = camera_rows[0 * 3 + c] * eye[0] + camera_rows[1 * 3 + c] * eye[1] +
			   camera_rows[2 * 3 + c] * eye[2];
	}
}

/* Derive stable visual variation from existing streak seeds without touching game RNG. */
static uint32_t hyper_streak_visual_seed(const XwaHyperspaceStreak* streak) {
	uint32_t hash = 2166136261u;
	for (int axis = 0; axis < 3; axis++) {
		hash = (hash ^ (uint32_t)streak->offset[axis]) * 16777619u;
	}
	return (hash ^ (uint32_t)streak->roll) * 16777619u;
}

static void hyper_emit_streak(HyperStreakVertex* out, const XwaHyperspaceStreak* streak, float extent,
							  float transition_y, const float camera_rows[9], float x_scale,
							  int supplemental, const XwaFlightHyperspaceStreakParams* tuning) {
	XwaFlightObject synthetic;
	memset(&synthetic, 0, sizeof synthetic);
	synthetic.orient_dirty = 1;
	synthetic.pitch = 0x4000u;
	synthetic.roll = streak->roll;
	float model[16];
	XwaRemasterFlight_ObjectModelMatrixForCameraDelta(&synthetic, NULL, 0, model);
	/* The shared model matrix expects meter-space geometry; streak seeds retain XWA model units. */
	const float model_units_to_meters = AERON_OPT_METERS_PER_UNIT;
	/* The 1977 film uses much finer, unevenly spaced blue-white light trails. */
	const uint32_t variation = hyper_streak_visual_seed(streak);
	const float width_jitter = (float)(variation & 255u) / 255.0f;
	const float brightness_jitter = (float)((variation >> 16) & 255u) / 255.0f;
	/* Shader normalizes the apparent width after projection. */
	const float half = (float)streak->half_width * model_units_to_meters;
	const float film_half_width_px = (0.66f + 0.52f * width_jitter) *
		(tuning ? tuning->width_scale : 1.0f);

	/* Treat each existing radial streak as a bounded part of a zoom exposure.
	 * Its perspective projection already makes trails farther from the axis
	 * appear longer. A modest radius dependence reinforces that relationship
	 * without squaring it or changing the original transition timing.
	 *
	 * Stable shutter intervals create black gaps between separate light trails.
	 * The supplemental inner streaks start closer to the vanishing point. */
	uint32_t shutter_hash = variation ^ (variation >> 16);
	shutter_hash *= 0x7feb352du;
	shutter_hash ^= shutter_hash >> 15;
	shutter_hash *= 0x846ca68bu;
	shutter_hash ^= shutter_hash >> 16;
	const float start_jitter = (float)(shutter_hash & 65535u) / 65535.0f;
	const float duration_jitter = (float)(shutter_hash >> 16) / 65535.0f;
	const float offset_x = (float)streak->offset[0];
	const float offset_z = (float)streak->offset[2];
	const float radius = sqrtf(offset_x * offset_x + offset_z * offset_z);
	const float radial_weight = radius / (radius + 4096.0f);
	/* Film exposure marks: shorter near the convergence, longer outward. */
	const float shutter_start = (supplemental ? 0.025f : 0.055f) +
								(supplemental ? 0.41f : 0.55f) * start_jitter;
	const float shutter_duration = (0.17f + 0.34f * duration_jitter) *
									(0.54f + 0.88f * radial_weight) *
									(tuning ? tuning->length_scale : 1.0f);
	const float shutter_end = fminf(0.97f, shutter_start + shutter_duration);
	const float visible_span = shutter_end - shutter_start;
	const float exposure_start = extent * model_units_to_meters * shutter_start;
	const float exposure_end = extent * model_units_to_meters * shutter_end;

	/* A long optical smear spreads a star's energy over more pixels. */
	const float exposure_brightness =
		fminf(1.12f, fmaxf(0.72f, sqrtf(0.38f / fmaxf(visible_span, 0.12f))));
	/* ROTJ-inspired blue-white, with a few lavender and brilliant highlights.
	 * Lower additive intensity compensates for the increased line population. */
	static const float film_tints[8][3] = {
		{ 0.95f, 0.97f, 1.00f }, { 0.75f, 0.87f, 1.00f },
		{ 0.52f, 0.75f, 1.00f }, { 0.42f, 0.62f, 0.99f },
		{ 0.85f, 0.92f, 1.00f }, { 0.66f, 0.73f, 0.99f },
		{ 0.91f, 0.95f, 1.00f }, { 0.59f, 0.81f, 1.00f },
	};
	const float* tint = film_tints[(variation >> 24) & 7u];
	float intensity = (0.48f + 0.22f * brightness_jitter) * exposure_brightness;
	float live_tint[3];
	if (tuning) {
		/* Keep these as actual RGB values; do not infer blue hue from a
		 * cyan/violet formula. Existing additive blending handles overlaps. */
		uint32_t color_hash = hyper_streak_visual_seed(streak) ^ tuning->preview_seed;
		color_hash ^= color_hash >> 16;
		color_hash *= 0x7feb352du;
		color_hash ^= color_hash >> 15;
		const float color_jitter = (float)((color_hash >> 8) & 255u) / 255.0f;
		const int white = color_jitter < tuning->white_share;
		live_tint[0] = white ? 0.965f : tuning->blue_red;
		live_tint[1] = white ? 0.980f : tuning->blue_green;
		live_tint[2] = 1.0f;
		tint = live_tint;
		intensity *= white ? tuning->white_brightness : tuning->blue_brightness;
	}
	const float corners[4][3] = {
		{ half, exposure_start, 0.0f },
		{ half, exposure_end, 0.0f },
		{ -half, exposure_end, 0.0f },
		{ -half, exposure_start, 0.0f },
	};
	static const float shape_coords[4][2] = {
		{ 1.0f, 0.0f }, { 1.0f, 1.0f },
		{ -1.0f, 1.0f }, { -1.0f, 0.0f },
	};
	static const uint8_t indices[6] = { 0, 1, 2, 0, 2, 3 };
	for (int v = 0; v < 6; v++) {
		const uint8_t corner = indices[v];
		const float* q = corners[corner];
		float p[3] = {
			model[0] * q[0] + model[1] * q[1] + model[2] * q[2] + (float)streak->offset[0],
			model[4] * q[0] + model[5] * q[1] + model[6] * q[2] + (float)streak->offset[1] - transition_y,
			model[8] * q[0] + model[9] * q[1] + model[10] * q[2] + (float)streak->offset[2],
		};
		/* An unexpanded centerline accompanies each edge vertex. The vertex
		 * shader adds a one-pixel fringe around the actual line width. */
		float center[3] = {
			model[1] * q[1] + (float)streak->offset[0],
			model[5] * q[1] + (float)streak->offset[1] - transition_y,
			model[9] * q[1] + (float)streak->offset[2],
		};
		hyper_widescreen_remap(p, camera_rows, x_scale);
		hyper_widescreen_remap(center, camera_rows, x_scale);
		memcpy(out[v].position, p, sizeof p);
		memcpy(out[v].center_position, center, sizeof center);
		out[v].shape_uv[0] = shape_coords[corner][0];
		out[v].shape_uv[1] = shape_coords[corner][1];
		out[v].shape_uv[2] = film_half_width_px;
		out[v].color[0] = tint[0] * intensity;
		out[v].color[1] = tint[1] * intensity;
		out[v].color[2] = tint[2] * intensity;
		out[v].color[3] = 1.0f;
	}
}

/* Renderer-only companion exposure marks. Preserve game RNG, star population,
 * 3D depth movement and the distinct outbound/inbound timings. */
static void hyper_derive_film_streak(const XwaHyperspaceStreak* source,
								 XwaHyperspaceStreak* derived, uint32_t variant) {
	*derived = *source;
	uint32_t hash = hyper_streak_visual_seed(source) ^ (0x9e3779b9u * (variant + 1u));
	hash ^= hash >> 16;
	hash *= 0x7feb352du;
	hash ^= hash >> 15;
	hash *= 0x846ca68bu;
	hash ^= hash >> 16;
	const float r = (float)(hash & 65535u) / 65535.0f;
	const float a = (float)(hash >> 16) / 65535.0f;
	const float radial_scale = variant == 0u ? 0.18f + 0.34f * r :
		(variant == 1u ? 0.56f + 0.41f * r : 0.16f + 0.79f * r);
	const float angle = (a - 0.5f) * (variant == 0u ? 0.36f :
		(variant == 1u ? 0.27f : 0.30f + 0.06f * (float)(variant % 3u)));
	const float sine = sinf(angle), cosine = cosf(angle);
	const float x = (float)source->offset[0] * radial_scale;
	const float z = (float)source->offset[2] * radial_scale;
	derived->offset[0] = (int32_t)(x * cosine - z * sine);
	derived->offset[2] = (int32_t)(x * sine + z * cosine);
	derived->half_width = (int32_t)fmaxf(1.0f, (float)source->half_width * radial_scale);
	derived->roll = (uint16_t)(source->roll + (int32_t)(angle * (65536.0f / 6.28318530718f)));
}

static float hyper_transition_center_y(const XwaFlightCamera* camera) {
	if (!camera || camera->vp_h == 0) {
		return 0.5f;
	}

	/* The recovered transition sprite uses bottom-origin screen coordinates. */
	return 1.0f - ((float)camera->vp_center_y - (float)camera->proj_offset_y) / (float)camera->vp_h;
}

static float hyper_dot3(const float a[3], const float b[3]) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static int hyper_normalize(float value[3]) {
	const float length = sqrtf(hyper_dot3(value, value));
	if (!isfinite(length) || length <= 0.0001f) {
		return 0;
	}
	value[0] /= length;
	value[1] /= length;
	value[2] /= length;
	return 1;
}

static void hyper_view_to_world(const float camera_rows[9], const float view[3], float world[3]) {
	for (int component = 0; component < 3; component++) {
		world[component] = camera_rows[0 * 3 + component] * view[0] +
						   camera_rows[1 * 3 + component] * view[1] +
						   camera_rows[2 * 3 + component] * view[2];
	}
}

static void hyper_add_uniform_ambient(XwaShipAmbientCube* cube, const float color[3]) {
	float* const lobes[6] = { cube->pos_x, cube->neg_x, cube->pos_y, cube->neg_y, cube->pos_z, cube->neg_z };
	for (int lobe = 0; lobe < 6; lobe++) {
		memcpy(lobes[lobe], color, 3 * sizeof(float));
	}
}

static void hyper_build_transition_lighting(XwaRemasterHyperspace* h, float flash_alpha) {
	float color[3];
	const float scale = h->params.brightness * h->params.highlight_strength *
						h->params.mesh_ambient_strength * flash_alpha;
	for (int channel = 0; channel < 3; channel++) {
		color[channel] = h->params.cap_color[channel] * scale;
	}
	hyper_add_uniform_ambient(&h->lighting.ambient_add, color);
	h->lighting.active = 1;
}

static int hyper_generate_environment(XwaRemasterHyperspace* h, AeronCommandBuffer* cmd) {
	const AeronComputeTextureBinding output = { .texture = h->environment_atlas };
	AeronComputePass* pass = Aeron_BeginComputePass(&(AeronComputePassDesc) {
		.command_buffer = cmd,
		.write_textures = &output,
		.write_texture_count = 1,
		.debug_label = "Hyperspace diffuse environment",
	});
	if (!pass) {
		return 0;
	}
	const HyperEnvironmentUniform environment = {
		.roughness = h->params.mesh_environment_roughness,
	};
	Aeron_BindComputePipeline(pass, h->environment_pipeline);
	Aeron_BindComputeUniformData(pass, 0, &h->tunnel_uniform, sizeof h->tunnel_uniform);
	Aeron_BindComputeUniformData(pass, 1, &environment, sizeof environment);
	Aeron_DispatchCompute(pass, HYPER_ENVIRONMENT_ATLAS_WIDTH / 8, HYPER_ENVIRONMENT_FACE_SIZE / 8, 1);
	Aeron_EndComputePass(pass);

	/* Metal cannot expose one face of a cube as a writable storage view.
	 * Generate into a 2D atlas and copy each completed face into the sampled cube. */
	for (uint32_t face = 0; face < 6; face++) {
		if (!Aeron_CopyTextureCmd(cmd, &(AeronTextureCopyDesc) {
				.source = h->environment_atlas,
				.source_x = face * HYPER_ENVIRONMENT_FACE_SIZE,
				.destination = h->environment_cube,
				.destination_layer = face,
				.width = HYPER_ENVIRONMENT_FACE_SIZE,
				.height = HYPER_ENVIRONMENT_FACE_SIZE,
			})) {
			return 0;
		}
	}
	return 1;
}

static void hyper_build_tunnel_lighting(XwaRemasterHyperspace* h, const float camera_rows[9]) {
	float* const world_basis[3] = {
		h->lighting.environment.right,
		h->lighting.environment.up,
		h->lighting.environment.forward,
	};
	const float* const view_basis[3] = {
		h->tunnel_uniform.tunnel_right,
		h->tunnel_uniform.tunnel_up,
		h->tunnel_uniform.tunnel_forward,
	};
	for (int basis = 0; basis < 3; basis++) {
		hyper_view_to_world(camera_rows, view_basis[basis], world_basis[basis]);
		if (!hyper_normalize(world_basis[basis])) {
			h->lighting.active = 1;
			return;
		}
	}
	h->lighting.environment.texture = h->environment_cube;
	h->lighting.environment.sampler = h->environment_sampler;
	h->lighting.environment.strength = h->params.mesh_ambient_strength;

	float key_direction[3] = { h->tunnel_uniform.tunnel_forward[0], h->tunnel_uniform.tunnel_forward[1],
							   h->tunnel_uniform.tunnel_forward[2] };
	hyper_view_to_world(camera_rows, key_direction, h->lighting.key.world_dir);
	float linear_key[3];
	float intensity = 0.0f;
	const float key_scale = h->params.brightness * h->params.highlight_strength * h->params.mesh_key_strength;
	for (int channel = 0; channel < 3; channel++) {
		linear_key[channel] = h->params.cap_color[channel] * key_scale;
		intensity = fmaxf(intensity, linear_key[channel]);
	}
	if (hyper_normalize(h->lighting.key.world_dir) && isfinite(intensity) && intensity > 0.0f) {
		h->lighting.key.intensity = intensity;
		for (int channel = 0; channel < 3; channel++) {
			h->lighting.key.color[channel] = XwaRemaster_LinearToSrgb(linear_key[channel] / intensity);
		}
		h->lighting.key.source_backdrop_model_type = 0;
		h->lighting.key_count = 1;
	}
	h->lighting.active = 1;
}

int XwaRemasterHyperspace_GetPreparedLighting(const XwaRemasterHyperspace* h,
											  XwaRemasterHyperspaceLighting* out) {
	if (!h || !out || !h->lighting.active) {
		return 0;
	}
	*out = h->lighting;
	return 1;
}

int XwaRemasterHyperspace_Prepare(XwaRemasterHyperspace* h, AeronCommandBuffer* cmd, const XwaSnapshot* snap,
								  const float view_proj[16], const float camera_rows[9], int rt_w, int rt_h, int force_tunnel,
								  float tunnel_time_seconds,
								  const XwaRemasterHyperspaceTunnelView* tunnel_view) {
	if (!h || !cmd || !snap || !view_proj || !camera_rows) {
		return 0;
	}
	h->streak_vertex_count = 0;
	h->draw_background = 0;
	memset(&h->lighting, 0, sizeof h->lighting);
	memset(&h->tunnel_uniform, 0, sizeof h->tunnel_uniform);
	h->tunnel_uniform.view[0] = (float)rt_w;
	h->tunnel_uniform.view[1] = (float)rt_h;
	h->tunnel_uniform.view[2] = 0.5f;
	h->tunnel_uniform.view[3] = hyper_transition_center_y(&snap->flight_camera);
	h->tunnel_uniform.appearance[0] = h->params.brightness;
	h->tunnel_uniform.appearance[1] = h->params.highlight_strength;
	memcpy(h->tunnel_uniform.cap_color, h->params.cap_color, 3 * sizeof(float));
	memcpy(h->streak_uniform.view_proj, view_proj, sizeof h->streak_uniform.view_proj);
	h->streak_uniform.viewport[0] = (float)rt_w;
	h->streak_uniform.viewport[1] = (float)rt_h;
	/* Streak preview freezes the rendered outbound frame only. */
	const int streak_preview = h->streak_preview && !force_tunnel;
	const uint8_t phase = force_tunnel ? XWA_HYPERSPACE_TUNNEL :
		(streak_preview ? XWA_HYPERSPACE_OUTBOUND : snap->hyperspace.phase);
	const uint32_t ticks = force_tunnel ? 0u :
		(streak_preview ? (uint32_t)h->streak_tuning.preview_ticks : snap->hyperspace.phase_elapsed_ticks);
	/* Each actual outbound entry gets a fresh, uniformly sampled white
	 * fraction. It stays fixed through every frame of the same entry.
	 * The frozen inspector preview keeps its exact manual white share.
	 * Only host clock and renderer-local sequence state are consumed. */
	if (phase == XWA_HYPERSPACE_OUTBOUND && !streak_preview &&
		(!h->entry_white_share_valid ||
		 h->last_phase != XWA_HYPERSPACE_OUTBOUND || h->last_was_streak_preview ||
		 ticks < h->last_phase_ticks)) {
		const uint64_t host_us = XwaTime_GetElapsedUs();
		uint32_t hash = (uint32_t)host_us ^ (uint32_t)(host_us >> 32) ^
			(++h->entry_sequence * 0x9e3779b9u);
		hash ^= hash >> 16;
		hash *= 0x7feb352du;
		hash ^= hash >> 15;
		hash *= 0x846ca68bu;
		hash ^= hash >> 16;
		const float unit = (float)(hash >> 8) / 16777215.0f;
		h->entry_white_share = h->streak_tuning.white_share_min +
			(h->streak_tuning.white_share_max - h->streak_tuning.white_share_min) * unit;
		h->entry_white_share_valid = 1;
	}
	h->last_phase = phase;
	h->last_phase_ticks = ticks;
	h->last_was_streak_preview = streak_preview;
	if (phase == XWA_HYPERSPACE_TUNNEL) {
		if (tunnel_view) {
			h->tunnel_uniform.projection[0] = tunnel_view->tan_half_fov_x;
			h->tunnel_uniform.projection[1] = tunnel_view->tan_half_fov_y;
			h->tunnel_uniform.projection[2] = tunnel_view->proj_offset_x;
			h->tunnel_uniform.projection[3] = tunnel_view->proj_offset_y;
			/* XWA's simulation clock has 236 timing ticks per second. */
			h->tunnel_uniform.motion[0] = force_tunnel ? tunnel_time_seconds : (float)ticks / 236.0f;
			h->tunnel_uniform.motion[1] = h->params.travel_speed;
			h->tunnel_uniform.motion[2] = h->params.rotation_speed;
			h->tunnel_uniform.motion[3] = h->params.noise_scale;
			h->tunnel_uniform.geometry[0] = h->params.focal_length;
			h->tunnel_uniform.geometry[1] = h->params.twist;
			h->tunnel_uniform.geometry[2] = h->params.cap_radius;
			h->tunnel_uniform.geometry[3] = h->params.cap_falloff;
			memcpy(h->tunnel_uniform.tunnel_right, tunnel_view->right, 3 * sizeof(float));
			memcpy(h->tunnel_uniform.tunnel_up, tunnel_view->up, 3 * sizeof(float));
			memcpy(h->tunnel_uniform.tunnel_forward, tunnel_view->forward, 3 * sizeof(float));
			memcpy(h->tunnel_uniform.dark_color, h->params.dark_color, 3 * sizeof(float));
			memcpy(h->tunnel_uniform.body_color, h->params.body_color, 3 * sizeof(float));
			memcpy(h->tunnel_uniform.highlight_color, h->params.highlight_color, 3 * sizeof(float));
			if (h->params.mesh_ambient_strength > 0.0f && !hyper_generate_environment(h, cmd)) {
				return 0;
			}
			hyper_build_tunnel_lighting(h, camera_rows);
			h->draw_background = 1;
			return 1;
		}
		return 0;
	}
	if (phase != XWA_HYPERSPACE_OUTBOUND && phase != XWA_HYPERSPACE_INBOUND) {
		return 0;
	}

	float extent = 0.0f;
	float transition_y = 0.0f;
	float flash_alpha = 0.0f;
	if (phase == XWA_HYPERSPACE_OUTBOUND) {
		if (ticks < 472u) {
			const float scaled = (float)(ticks >> 2);
			extent = scaled * scaled;
		} else {
			extent = 20000.0f;
			const float t = (float)(ticks * 2u - 944u);
			transition_y = (float)(int)(t * t);
			const int alpha = (int)(((float)(ticks - 472u) / 118.0f) * 255.0f);
			flash_alpha = (float)(uint16_t)alpha / 255.0f;
		}
	} else {
		int remaining = 236 - (int)ticks;
		if (remaining < 0)
			remaining = 0;
		if (remaining < 118) {
			extent = 10000.0f - (1.0f - (float)remaining / 118.0f) * 5000.0f;
		} else {
			extent = 10000.0f;
			const int alpha = (int)((1.0f - (float)ticks / 118.0f) * 255.0f);
			flash_alpha = (float)(uint16_t)alpha / 255.0f;
		}
		transition_y = (float)(int)(-0.1f * (float)(8 * remaining) * (float)(4 * remaining));
	}
	if (flash_alpha < 0.0f)
		flash_alpha = 0.0f;
	if (flash_alpha > 1.0f)
		flash_alpha = 1.0f;
	if (flash_alpha > 0.0f) {
		h->tunnel_uniform.appearance[2] = flash_alpha;
		h->tunnel_uniform.appearance[3] = 1.0f;
		h->draw_background = 1;
	}
	hyper_build_transition_lighting(h, flash_alpha);

	/* Reproduce a seed field outside real hyperspace. This is private render
	 * data, never written to simulation state or the game's RNG streams. */
	XwaHyperspaceStreak synthetic[512];
	const XwaHyperspaceStreak* seeds = snap->hyperspace_streaks;
	uint32_t count = snap->hyperspace_streak_count;
	if (streak_preview) {
		count = 512u;
		for (uint32_t i = 0; i < count; i++) {
			uint32_t h1 = (i + 1u) * 0x9e3779b9u ^ h->streak_tuning.preview_seed;
			h1 ^= h1 >> 16; h1 *= 0x7feb352du;
			h1 ^= h1 >> 15; h1 *= 0x846ca68bu; h1 ^= h1 >> 16;
			uint32_t h2 = h1 ^ 0xa58f3421u;
			h2 ^= h2 >> 16; h2 *= 0x7feb352du;
			h2 ^= h2 >> 15; h2 *= 0x846ca68bu; h2 ^= h2 >> 16;
			const float x = ((float)(h1 & 65535u) / 65535.0f - 0.5f) * 16000.0f;
			const float z = ((float)(h2 & 65535u) / 65535.0f - 0.5f) * 11000.0f;
			synthetic[i].offset[0] = (int32_t)x;
			synthetic[i].offset[1] = 0x4000;
			synthetic[i].offset[2] = (int32_t)z;
			synthetic[i].half_width = 1;
			synthetic[i].roll =
				(uint16_t)((int32_t)(atan2f(z, x) * (65536.0f / 6.28318530718f)) + 0x4000);
		}
		seeds = synthetic;
	}
	if (count > XWA_SNAP_MAX_HYPERSPACE_STREAKS)
		count = XWA_SNAP_MAX_HYPERSPACE_STREAKS;
	/* Smooth density adjustment with stable partial companion populations.
	 * Three complete marks per seed at density 1.0 (original baseline). */
	XwaFlightHyperspaceStreakParams entry_tuning = h->streak_tuning;
	/* Real entry uses the fixed per-jump draw; preview stays exactly as set. */
	if (phase == XWA_HYPERSPACE_OUTBOUND && !streak_preview)
		entry_tuning.white_share = h->entry_white_share;
	const XwaFlightHyperspaceStreakParams* tuning =
		phase == XWA_HYPERSPACE_OUTBOUND ? &entry_tuning : NULL;
	const float marks_per_seed = tuning ? 3.0f * tuning->density_scale : 3.0f;
	const uint32_t full_marks = (uint32_t)floorf(marks_per_seed);
	const uint32_t supplement_count = (uint32_t)ceilf(marks_per_seed) - 1u;
	const float fractional = marks_per_seed - (float)full_marks;
	const uint32_t vertex_capacity = count * (1u + supplement_count) * 6u;
	const uint32_t bytes = vertex_capacity * (uint32_t)sizeof(HyperStreakVertex);
	if (vertex_capacity) {
		if (!hyper_ensure_streak_buffer(h, bytes)) {
			return 0;
		}
		HyperStreakVertex* verts = (HyperStreakVertex*)malloc(bytes);
		if (!verts) {
			return 0;
		}
		const float target_aspect = rt_h > 0 ? (float)rt_w / (float)rt_h : 4.0f / 3.0f;
		const float x_scale = target_aspect > 4.0f / 3.0f ? target_aspect / (4.0f / 3.0f) : 1.0f;
		uint32_t written = 0;
		for (uint32_t i = 0; i < count; i++) {
			hyper_emit_streak(&verts[written++ * 6u], &seeds[i], extent, transition_y, camera_rows,
							  x_scale, 0, tuning);
		}
		for (uint32_t variant = 0; variant < supplement_count; variant++) {
			for (uint32_t i = 0; i < count; i++) {
				if (variant + 2u > full_marks) {
					uint32_t jitter = (i + 17u) * 0x9e3779b9u ^
						((variant + 1u) * 0x85ebca6bu) ^
						(tuning ? tuning->preview_seed : 0u);
					jitter ^= jitter >> 16; jitter *= 0x7feb352du;
					jitter ^= jitter >> 15;
					if ((float)(jitter & 65535u) / 65535.0f >= fractional) continue;
				}
				XwaHyperspaceStreak derived;
				hyper_derive_film_streak(&seeds[i], &derived, variant);
				hyper_emit_streak(&verts[written++ * 6u],
									  &derived, extent, transition_y, camera_rows, x_scale, 1, tuning);
			}
		}
		/* Only upload initialized quads; the capacity includes partial-ring slack. */
		const uint32_t used_bytes = written * 6u * (uint32_t)sizeof(HyperStreakVertex);
		if (!Aeron_UploadBufferDataCmd(cmd, h->streak_vb, 0, verts, used_bytes)) {
			free(verts);
			return 0;
		}
		free(verts);
		h->streak_vertex_count = written * 6u;
	}
	return h->draw_background || h->streak_vertex_count != 0;
}

void XwaRemasterHyperspace_Draw(AeronCommandBuffer* command_buffer, AeronRenderPass* pass, int rt_w, int rt_h,
								void* user) {
	XwaRemasterHyperspace* h = (XwaRemasterHyperspace*)user;
	if (!h || !pass) {
		return;
	}
	if (!hyper_ensure_pipelines(h, Aeron_RenderPassGetSampleCount(pass))) {
		Aeron_CommandBufferSetFailure(command_buffer, "Hyperspace pipeline preparation failed");
		return;
	}
	Aeron_SetViewport(pass, &(AeronRectI) { 0, 0, rt_w, rt_h });
	if (h->draw_background) {
		Aeron_BindGraphicsPipeline(pass, h->tunnel_pipeline);
		Aeron_BindUniformData(pass, AERON_SHADER_STAGE_FRAGMENT, 0, &h->tunnel_uniform,
							  (uint32_t)sizeof h->tunnel_uniform);
		Aeron_Draw(pass, 3, 0);
	}
	if (h->streak_vertex_count && h->streak_vb) {
		Aeron_BindGraphicsPipeline(pass, h->streak_pipeline);
		Aeron_BindVertexBuffer(pass, 0, h->streak_vb, 0);
		Aeron_BindUniformData(pass, AERON_SHADER_STAGE_VERTEX, 0, &h->streak_uniform,
								  (uint32_t)sizeof h->streak_uniform);
		Aeron_Draw(pass, h->streak_vertex_count, 0);
	}
}
