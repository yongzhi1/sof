// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2020 Google LLC. All rights reserved.
//
// Author: Pin-chih Lin <johnylin@google.com>

#include <sof/audio/component.h>
#include <sof/audio/drc/drc.h>
#include <sof/audio/drc/drc_math.h>
#include <sof/audio/format.h>
#include <stdint.h>

/* This is the knee part of the compression curve. Returns the output level
 * given the input level x. */
static float knee_curveK(struct sof_drc_params *p, float x)
{
	const float knee_alpha = Q_CONVERT_QTOF(p->knee_alpha, 24);
	const float knee_beta = Q_CONVERT_QTOF(p->knee_beta, 24);
	const float K = Q_CONVERT_QTOF(p->K, 20);

	/* The formula in knee_curveK is linear_threshold +
	 * (1 - expf(-k * (x - linear_threshold))) / k
	 * which simplifies to (alpha + beta * expf(gamma))
	 * where alpha = linear_threshold + 1 / k
	 *	 beta = -expf(k * linear_threshold) / k
	 *	 gamma = -k * x
	 */
	return knee_alpha + knee_beta * knee_expf(-K * x);
}

/* Full compression curve with constant ratio after knee. Returns the ratio of
 * output and input signal. */
static float volume_gain(struct sof_drc_params *p, float x)
{
	const float knee_threshold = Q_CONVERT_QTOF(p->knee_threshold, 24);
	const float linear_threshold = Q_CONVERT_QTOF(p->linear_threshold, 30);
	const float ratio_base = Q_CONVERT_QTOF(p->ratio_base, 30);
	const float slope = Q_CONVERT_QTOF(p->slope, 30);
	float y;

	if (x < knee_threshold) {
		if (x < linear_threshold)
			return 1;
		y = knee_curveK(p, x) / x;
	} else {
		/* Constant ratio after knee.
		 * log(y/y0) = s * log(x/x0)
		 * => y = y0 * (x/x0)^s
		 * => y = [y0 * (1/x0)^s] * x^s
		 * => y = ratio_base * x^s
		 * => y/x = ratio_base * x^(s - 1)
		 * => y/x = ratio_base * e^(log(x) * (s - 1))
		 */
		y = ratio_base * knee_expf(logf(x) * (slope - 1));
	}

	return y;
}

/* Update detector_average from the last input division. */
static void drc_update_detector_average(struct drc_state *state, struct sof_drc_params *p, int nch)
{
	float abs_input_array[DRC_DIVISION_FRAMES];
	const float sat_release_frames_inv_neg = Q_CONVERT_QTOF(p->sat_release_frames_inv_neg, 30);
	const float sat_release_rate_at_neg_two_db = Q_CONVERT_QTOF(p->sat_release_rate_at_neg_two_db, 30);
	float detector_average = Q_CONVERT_QTOF(state->detector_average, 30);
	int div_start, i, ch;
	float sample;

	/* Calculate the start index of the last input division */
	if (state->pre_delay_write_index == 0) {
		div_start = DRC_MAX_PRE_DELAY_FRAMES - DRC_DIVISION_FRAMES;
	} else {
		div_start = state->pre_delay_write_index - DRC_DIVISION_FRAMES;
	}

	/* The max abs value across all channels for this frame */
	for (i = 0; i < DRC_DIVISION_FRAMES; i++) {
		abs_input_array[i] = 0.0f;
		for (ch = 0; ch < nch; ch++) {
			sample = Q_CONVERT_QTOF(state->pre_delay_buffers[ch][div_start + i], 31);
			abs_input_array[i] = fmaxf(abs_input_array[i], fabsf(sample));
		}
	}

	for (i = 0; i < DRC_DIVISION_FRAMES; i++) {
		/* Compute compression amount from un-delayed signal */
		float abs_input = abs_input_array[i];

		/* Calculate shaped power on undelayed input.  Put through
		 * shaping curve. This is linear up to the threshold, then
		 * enters a "knee" portion followed by the "ratio" portion. The
		 * transition from the threshold to the knee is smooth (1st
		 * derivative matched). The transition from the knee to the
		 * ratio portion is smooth (1st derivative matched).
		 */
		float gain = volume_gain(p, abs_input);
		int is_release = (gain > detector_average);
		if (is_release) {
			if (gain > DRC_NEG_TWO_DB) {
				detector_average +=
					(gain - detector_average) *
					sat_release_rate_at_neg_two_db;
			} else {
				float gain_db = linear_to_decibels(gain);
				float db_per_frame =
					gain_db * sat_release_frames_inv_neg;
				float sat_release_rate =
					decibels_to_linear(db_per_frame) - 1;
				detector_average += (gain - detector_average) *
						    sat_release_rate;
			}
		} else {
			detector_average = gain;
		}

		/* Fix gremlins. */
		if (isbadf(detector_average))
			detector_average = 1.0f;
		else
			detector_average = min(detector_average, 1.0f);
	}

	state->detector_average = Q_CONVERT_FLOAT(detector_average, 30);
}

/* Updates the envelope_rate used for the next division */
static void drc_update_envelope(struct drc_state *state, struct sof_drc_params *p)
{
	const float kA = Q_CONVERT_QTOF(p->kA, 12);
	const float kB = Q_CONVERT_QTOF(p->kB, 12);
	const float kC = Q_CONVERT_QTOF(p->kC, 12);
	const float kD = Q_CONVERT_QTOF(p->kD, 12);
	const float kE = Q_CONVERT_QTOF(p->kE, 12);
	const float attack_frames = Q_CONVERT_QTOF(p->attack_frames, 20);

	/* Calculate desired gain */
	float desired_gain = Q_CONVERT_QTOF(state->detector_average, 30);

	/* Pre-warp so we get desired_gain after sin() warp below. */
	float scaled_desired_gain = warp_asinf(desired_gain);

	/* Deal with envelopes */

	/* envelope_rate is the rate we slew from current compressor level to
	 * the desired level.  The exact rate depends on if we're attacking or
	 * releasing and by how much.
	 */
	float envelope_rate;

	int is_releasing = scaled_desired_gain > Q_CONVERT_QTOF(state->compressor_gain, 30);

	/* compression_diff_db is the difference between current compression
	 * level and the desired level. */
	float compression_diff_db = linear_to_decibels(
		Q_CONVERT_QTOF(state->compressor_gain, 30) / scaled_desired_gain);

	if (is_releasing) {
		/* Release mode - compression_diff_db should be negative dB */
		state->max_attack_compression_diff_db = INT32_MIN;

		/* Fix gremlins. */
		if (isbadf(compression_diff_db))
			compression_diff_db = -1;

		/* Adaptive release - higher compression (lower
		 * compression_diff_db) releases faster. Contain within range:
		 * -12 -> 0 then scale to go from 0 -> 3
		 */
		float x = compression_diff_db;
		x = max(-12.0f, x);
		x = min(0.0f, x);
		x = 0.25f * (x + 12);

		/* Compute adaptive release curve using 4th order polynomial.
		 * Normal values for the polynomial coefficients would create a
		 * monotonically increasing function.
		 */
		float x2 = x * x;
		float x3 = x2 * x;
		float x4 = x2 * x2;
		float release_frames = kA + kB * x + kC * x2 + kD * x3 + kE * x4;

#define kSpacingDb 5
		float db_per_frame = kSpacingDb / release_frames;
		envelope_rate = decibels_to_linear(db_per_frame);
	} else {
		/* Attack mode - compression_diff_db should be positive dB */

		/* Fix gremlins. */
		if (isbadf(compression_diff_db))
			compression_diff_db = 1;

		/* As long as we're still in attack mode, use a rate based off
		 * the largest compression_diff_db we've encountered so far.
		 */
		state->max_attack_compression_diff_db =
			max(state->max_attack_compression_diff_db,
			    Q_CONVERT_FLOAT(compression_diff_db, 20));

		float eff_atten_diff_db =
			max(0.5f, Q_CONVERT_QTOF(state->max_attack_compression_diff_db, 20));

		float x = 0.25f / eff_atten_diff_db;
		envelope_rate = 1 - powf(x, 1 / attack_frames);
	}

	state->envelope_rate = Q_CONVERT_FLOAT(envelope_rate, 30);
	state->scaled_desired_gain = Q_CONVERT_FLOAT(scaled_desired_gain, 30);
}

/* Calculate compress_gain from the envelope and apply total_gain to compress
 * the next output division. */
static void drc_compress_output(struct drc_state *state, struct sof_drc_params *p, int nch)
{
	const float master_linear_gain = Q_CONVERT_QTOF(p->master_linear_gain, 24);
	const float envelope_rate = Q_CONVERT_QTOF(state->envelope_rate, 30);
	const float scaled_desired_gain = Q_CONVERT_QTOF(state->scaled_desired_gain, 30);
	const float compressor_gain = Q_CONVERT_QTOF(state->compressor_gain, 30);
	const int div_start = state->pre_delay_read_index;
	int count = DRC_DIVISION_FRAMES / 4;

	int i, j, ch, inc;
	float sample;

	/* Exponential approach to desired gain. */
	if (envelope_rate < 1) {
		/* Attack - reduce gain to desired. */
		float c = compressor_gain - scaled_desired_gain;
		float base = scaled_desired_gain;
		float r = 1 - envelope_rate;
		float x[4] = { c * r, c * r * r, c * r * r * r,
			       c * r * r * r * r };
		float r4 = r * r * r * r;

		i = 0;
		inc = 0;
		while (1) {
			for (j = 0; j < 4; j++) {
				/* Warp pre-compression gain to smooth out sharp
				 * exponential transition points.
				 */
				float post_warp_compressor_gain = warp_sinf(x[j] + base);

				/* Calculate total gain using master gain. */
				float total_gain = master_linear_gain * post_warp_compressor_gain;

				/* Apply final gain. */
				for (ch = 0; ch < nch; ch++) {
					sample = Q_CONVERT_QTOF(state->pre_delay_buffers[ch][div_start + inc], 31);
					state->pre_delay_buffers[ch][div_start + inc] = Q_CONVERT_FLOAT(sample * total_gain, 31);
				}
				inc++;
			}

			if (++i == count)
				break;

			for (j = 0; j < 4; j++)
				x[j] = x[j] * r4;
		}

		state->compressor_gain = Q_CONVERT_FLOAT(x[3] + base, 30);
	} else {
		/* Release - exponentially increase gain to 1.0 */
		float c = compressor_gain;
		float r = envelope_rate;
		float x[4] = { c * r, c * r * r, c * r * r * r,
			       c * r * r * r * r };
		float r4 = r * r * r * r;

		i = 0;
		inc = 0;
		while (1) {
			for (j = 0; j < 4; j++) {
				/* Warp pre-compression gain to smooth out sharp
				 * exponential transition points.
				 */
				float post_warp_compressor_gain = warp_sinf(x[j]);

				/* Calculate total gain using master gain. */
				float total_gain = master_linear_gain * post_warp_compressor_gain;

				/* Apply final gain. */
				for (ch = 0; ch < nch; ch++) {
					sample = Q_CONVERT_QTOF(state->pre_delay_buffers[ch][div_start + inc], 31);
					state->pre_delay_buffers[ch][div_start + inc] = Q_CONVERT_FLOAT(sample * total_gain, 31);
				}
				inc++;
			}

			if (++i == count)
				break;

			for (j = 0; j < 4; j++)
				x[j] = min(1.0f, x[j] * r4);
		}

		state->compressor_gain = Q_CONVERT_FLOAT(x[3], 30);
	}
}

/* After one complete divison of samples have been received (and one divison of
 * samples have been output), we calculate shaped power average
 * (detector_average) from the input division, update envelope parameters from
 * detector_average, then prepare the next output division by applying the
 * envelope to compress the samples.
 */
static void drc_process_one_division(struct drc_state *state, struct sof_drc_params *p, int nch)
{
	drc_update_detector_average(state, p, nch);
	drc_update_envelope(state, p);
	drc_compress_output(state, p, nch);
}

#if CONFIG_FORMAT_S16LE
static void drc_s16_default_pass(const struct comp_dev *dev,
				 const struct audio_stream *source,
				 struct audio_stream *sink,
				 uint32_t frames)
{
	int16_t *x;
	int16_t *y;
	int i;
	int n = source->channels * frames;

	for (i = 0; i < n; i++) {
		x = audio_stream_read_frag_s16(source, i);
		y = audio_stream_read_frag_s16(sink, i);
		*y = *x;
	}
}
#endif /* CONFIG_FORMAT_S16LE */

#if CONFIG_FORMAT_S16LE
static void drc_s16_default(const struct comp_dev *dev,
			    const struct audio_stream *source,
			    struct audio_stream *sink,
			    uint32_t frames)
{
	int16_t *x;
	int16_t *y;
	int16_t *pd_write;
	int16_t *pd_read;
	int offset;
	int i = 0;
	int ch;
	int idx;
	int f;
	int fragment;
	int pd_write_index;
	int pd_read_index;
	int nch = source->channels;

	struct comp_data *cd = comp_get_drvdata(dev);
	struct drc_state *state = &cd->state;
	struct sof_drc_params *p = &cd->config->params; /* Read-only */

	if (!p->enabled) {
		// TODO
		//drc_s16_process_delay_only(state, source, sink, frames);
		return;
	}

	if (!state->processed) {
		drc_update_envelope(state, p);
		drc_compress_output(state, p, nch);
		state->processed = 1;
	}

	offset = state->pre_delay_write_index & DRC_DIVISION_FRAMES_MASK;
	while (i < frames) {
		/* Copy fragment data from source to pre-delay buffers, and copy the output fragment
		 * to sink */
		fragment = min(DRC_DIVISION_FRAMES - offset, frames - i);
		pd_write_index = state->pre_delay_write_index;
		pd_read_index = state->pre_delay_read_index;
		for (ch = 0; ch < nch; ++ch) {
			pd_write = (int16_t *)state->pre_delay_buffers[ch] + pd_write_index;
			pd_read = (int16_t *)state->pre_delay_buffers[ch] + pd_read_index;
			idx = i * nch + ch;
			for (f = 0; f < fragment; ++f) {
				x = audio_stream_read_frag_s16(source, idx);
				y = audio_stream_read_frag_s16(sink, idx);
				*pd_write = *x;
				*y = *pd_read;
				pd_write++;
				pd_read++;
				idx += nch;
			}
		}
		state->pre_delay_write_index = (pd_write_index + fragment) & DRC_MAX_PRE_DELAY_FRAMES_MASK;
		state->pre_delay_read_index = (pd_read_index + fragment) & DRC_MAX_PRE_DELAY_FRAMES_MASK;

		i += fragment;
		offset = (offset + fragment) & DRC_DIVISION_FRAMES_MASK;

		/* Process the input division (32 frames). */
		if (offset == 0)
			drc_process_one_division(state, p, nch);
	}
}
#endif /* CONFIG_FORMAT_S16LE */

const struct drc_proc_fnmap drc_proc_fnmap[] = {
/* { SOURCE_FORMAT , PROCESSING FUNCTION } */
#if CONFIG_FORMAT_S16LE
	{ SOF_IPC_FRAME_S16_LE, drc_s16_default },
#endif /* CONFIG_FORMAT_S16LE */

#if CONFIG_FORMAT_S24LE
	//{ SOF_IPC_FRAME_S24_4LE, drc_s24_default },
#endif /* CONFIG_FORMAT_S24LE */

#if CONFIG_FORMAT_S32LE
	//{ SOF_IPC_FRAME_S32_LE, drc_s32_default },
#endif /* CONFIG_FORMAT_S32LE */
};

const struct drc_proc_fnmap drc_proc_fnmap_pass[] = {
/* { SOURCE_FORMAT , PROCESSING FUNCTION } */
#if CONFIG_FORMAT_S16LE
	{ SOF_IPC_FRAME_S16_LE, drc_s16_default_pass },
#endif /* CONFIG_FORMAT_S16LE */

#if CONFIG_FORMAT_S24LE
	//{ SOF_IPC_FRAME_S24_4LE, drc_s32_default_pass },
#endif /* CONFIG_FORMAT_S24LE */

#if CONFIG_FORMAT_S32LE
	//{ SOF_IPC_FRAME_S32_LE, drc_s32_default_pass },
#endif /* CONFIG_FORMAT_S32LE */
};

const size_t drc_proc_fncount = ARRAY_SIZE(drc_proc_fnmap);
