#
# Topology for Icelake with rt700 codec.
#

# Include topology builder
include(`utils.m4')
include(`dai.m4')
include(`pipeline.m4')
include(`alh.m4')
include(`hda.m4')

# Include TLV library
include(`common/tlv.m4')

# Include Token library
include(`sof/tokens.m4')

# Include Icelake DSP configuration
include(`platform/intel/tgl.m4')
include(`platform/intel/dmic.m4')

define(LAST_PIPELINE_ID, `8')

DEBUG_START

#
# Define the pipelines
#
# PCM0 ---> volume ----> ALH 2 BE dailink 0
# PCM1 <--- volume <---- ALH 3 BE dailink 1
# PCM2 <---------------- DMIC01 (dmic0 capture, BE dailink 2)
# PCM3 <---------------- DMIC16k (dmic16k, BE dailink 3)
# PCM4 ----> volume -----> iDisp1 (HDMI/DP playback, BE link 4)
# PCM5 ----> volume -----> iDisp2 (HDMI/DP playback, BE link 5)
# PCM6 ----> volume -----> iDisp3 (HDMI/DP playback, BE link 6)
# PCM7 ----> volume -----> iDisp4 (HDMI/DP playback, BE link 7)
#

dnl PIPELINE_PCM_ADD(pipeline,
dnl     pipe id, pcm, max channels, format,
dnl     period, priority, core,
dnl     pcm_min_rate, pcm_max_rate, pipeline_rate,
dnl     time_domain, sched_comp)

# Low Latency playback pipeline 1 on PCM 0 using max 2 channels of s32le.
# Schedule 48 frames per 1000us deadline on core 0 with priority 0
PIPELINE_PCM_ADD(sof/pipe-volume-playback.m4,
	1, 0, 2, s24le,
	1000, 0, 0,
	48000, 48000, 48000)

# Low Latency capture pipeline 2 on PCM 1 using max 2 channels of s32le.
# Schedule 48 frames per 1000us deadline on core 0 with priority 0
PIPELINE_PCM_ADD(sof/pipe-volume-capture.m4,
	2, 1, 2, s24le,
	1000, 0, 0,
	48000, 48000, 48000)

# Passthrough capture pipeline 3 on PCM 2 using max 4 channels.
# Schedule 48 frames per 1000us deadline on core 0 with priority 0
PIPELINE_PCM_ADD(sof/pipe-passthrough-capture.m4,
	3, 2, 4, s32le,
	1000, 0, 0,
	48000, 48000, 48000)

# Passthrough capture pipeline 4 on PCM 3 using max 2 channels.
# Schedule 16 frames per 1000us deadline on core 0 with priority 0
PIPELINE_PCM_ADD(sof/pipe-passthrough-capture.m4,
	4, 3, 2, s16le,
	1000, 0, 0,
	16000, 16000, 16000)

# Low Latency playback pipeline 5 on PCM 4 using max 2 channels of s32le.
# Schedule 48 frames per 1000us deadline on core 0 with priority 0
PIPELINE_PCM_ADD(sof/pipe-volume-playback.m4,
        5, 4, 2, s32le,
        1000, 0, 0,
        48000, 48000, 48000)

# Low Latency playback pipeline 6 on PCM 5 using max 2 channels of s32le.
# Schedule 48 frames per 1000us deadline on core 0 with priority 0
PIPELINE_PCM_ADD(sof/pipe-volume-playback.m4,
        6, 5, 2, s32le,
        1000, 0, 0,
        48000, 48000, 48000)

# Low Latency playback pipeline 7 on PCM 6 using max 2 channels of s32le.
# Schedule 48 frames per 1000us deadline on core 0 with priority 0
PIPELINE_PCM_ADD(sof/pipe-volume-playback.m4,
        7, 6, 2, s32le,
        1000, 0, 0,
        48000, 48000, 48000)

# Low Latency playback pipeline 7 on PCM 6 using max 2 channels of s32le.
# Schedule 48 frames per 1000us deadline on core 0 with priority 0
PIPELINE_PCM_ADD(sof/pipe-volume-playback.m4,
        8, 7, 2, s32le,
        1000, 0, 0,
        48000, 48000, 48000)

# BT audio offload
define(`CHANNELS_MIN', 1)

PIPELINE_PCM_ADD(sof/pipe-passthrough-playback.m4,
	eval(LAST_PIPELINE_ID + 1), 50, 2, s16le,
	1000, 0, 0,
	8000, 16000, 48000)

PIPELINE_PCM_ADD(sof/pipe-passthrough-capture.m4,
	eval(LAST_PIPELINE_ID + 2), 50, 2, s16le,
	1000, 0, 0,
	8000, 16000, 48000)

undefine(`CHANNELS_MIN')

#
# DAIs configuration
#

dnl DAI_ADD(pipeline,
dnl     pipe id, dai type, dai_index, dai_be,
dnl     buffer, periods, format,
dnl     deadline, priority, core, time_domain)

# playback DAI is ALH(SDW0 PIN2) using 2 periods
# Buffers use s32le format, with 48 frame per 1000us on core 0 with priority 0
DAI_ADD(sof/pipe-dai-playback.m4,
	1, ALH, 2, SDW0-Playback,
	PIPELINE_SOURCE_1, 2, s24le,
	1000, 0, 0, SCHEDULE_TIME_DOMAIN_TIMER)

# capture DAI is ALH(SDW0 PIN2) using 2 periods
# Buffers use s32le format, with 48 frame per 1000us on core 0 with priority 0
DAI_ADD(sof/pipe-dai-capture.m4,
	2, ALH, 3, SDW0-Capture,
	PIPELINE_SINK_2, 2, s24le,
	1000, 0, 0, SCHEDULE_TIME_DOMAIN_TIMER)

# capture DAI is DMIC01 using 2 periods
# Buffers use s32le format, with 48 frame per 1000us on core 0 with priority 0
DAI_ADD(sof/pipe-dai-capture.m4,
	3, DMIC, 0, dmic01,
	PIPELINE_SINK_3, 2, s32le,
	1000, 0, 0, SCHEDULE_TIME_DOMAIN_TIMER)

# capture DAI is DMIC16k using 2 periods
# Buffers use s16le format, with 16 frame per 1000us on core 0 with priority 0
DAI_ADD(sof/pipe-dai-capture.m4,
	4, DMIC, 1, dmic16k,
	PIPELINE_SINK_4, 2, s16le,
	1000, 0, 0, SCHEDULE_TIME_DOMAIN_TIMER)
# playback DAI is iDisp1 using 2 periods
# Buffers use s32le format, with 48 frame per 1000us on core 0 with priority 0
DAI_ADD(sof/pipe-dai-playback.m4,
        5, HDA, 0, iDisp1,
        PIPELINE_SOURCE_5, 2, s32le,
        1000, 0, 0, SCHEDULE_TIME_DOMAIN_TIMER)

# playback DAI is iDisp2 using 2 periods
# Buffers use s32le format, with 48 frame per 1000us on core 0 with priority 0
DAI_ADD(sof/pipe-dai-playback.m4,
        6, HDA, 1, iDisp2,
        PIPELINE_SOURCE_6, 2, s32le,
        1000, 0, 0, SCHEDULE_TIME_DOMAIN_TIMER)

# playback DAI is iDisp3 using 2 periods
# Buffers use s32le format, with 48 frame per 1000us on core 0 with priority 0
DAI_ADD(sof/pipe-dai-playback.m4,
        7, HDA, 2, iDisp3,
        PIPELINE_SOURCE_7, 2, s32le,
        1000, 0, 0, SCHEDULE_TIME_DOMAIN_TIMER)

# playback DAI is iDisp3 using 2 periods
# Buffers use s32le format, with 48 frame per 1000us on core 0 with priority 0
DAI_ADD(sof/pipe-dai-playback.m4,
        8, HDA, 3, iDisp4,
        PIPELINE_SOURCE_8, 3, s32le,
        1000, 0, 0, SCHEDULE_TIME_DOMAIN_TIMER)

# BT offload
DAI_ADD(sof/pipe-dai-playback.m4,
	eval(LAST_PIPELINE_ID + 1), SSP, 2, SSP2-NoCodec,
	PIPELINE_SOURCE_9, 2, s16le,
	1000, 0, 0, SCHEDULE_TIME_DOMAIN_TIMER)

DAI_ADD(sof/pipe-dai-capture.m4,
	eval(LAST_PIPELINE_ID + 2), SSP, 2, SSP2-NoCodec,
	PIPELINE_SINK_10, 2, s16le,
	1000, 0, 0, SCHEDULE_TIME_DOMAIN_TIMER)

# PCM Low Latency, id 0
dnl PCM_PLAYBACK_ADD(name, pcm_id, playback)
PCM_PLAYBACK_ADD(SDW0-speakers, 0, PIPELINE_PCM_1)
PCM_CAPTURE_ADD(SDW0-mics, 1, PIPELINE_PCM_2)
PCM_CAPTURE_ADD(DMIC, 2, PIPELINE_PCM_3)
PCM_CAPTURE_ADD(DMIC16kHz, 3, PIPELINE_PCM_4)
PCM_PLAYBACK_ADD(HDMI1, 4, PIPELINE_PCM_5)
PCM_PLAYBACK_ADD(HDMI2, 5, PIPELINE_PCM_6)
PCM_PLAYBACK_ADD(HDMI3, 6, PIPELINE_PCM_7)
PCM_PLAYBACK_ADD(HDMI4, 7, PIPELINE_PCM_8)
PCM_DUPLEX_ADD(BT-PCM, 50, PIPELINE_PCM_9, PIPELINE_PCM_10)

#
# BE configurations - overrides config in ACPI if present
#

#ALH dai index = ((link_id << 8) | PDI id)
#ALH SDW0 Pin2 (ID: 0)
DAI_CONFIG(ALH, 2, 0, SDW0-Playback,
	ALH_CONFIG(ALH_CONFIG_DATA(ALH, 2, 48000, 2)))

#ALH SDW0 Pin3 (ID: 1)
DAI_CONFIG(ALH, 3, 1, SDW0-Capture,
	ALH_CONFIG(ALH_CONFIG_DATA(ALH, 3, 48000, 2)))

# dmic01 (ID: 2)
DAI_CONFIG(DMIC, 0, 2, dmic01,
	   DMIC_CONFIG(1, 500000, 4800000, 40, 60, 48000,
		DMIC_WORD_LENGTH(s32le), 400, DMIC, 0,
		PDM_CONFIG(DMIC, 0, FOUR_CH_PDM0_PDM1)))

# dmic16k (ID: 3)
DAI_CONFIG(DMIC, 1, 3, dmic16k,
	   DMIC_CONFIG(1, 500000, 4800000, 40, 60, 16000,
		DMIC_WORD_LENGTH(s16le), 400, DMIC, 1,
		PDM_CONFIG(DMIC, 1, STEREO_PDM0)))

# 3 HDMI/DP outputs (ID: 4,5,6,7)
DAI_CONFIG(HDA, 0, 4, iDisp1,
        HDA_CONFIG(HDA_CONFIG_DATA(HDA, 0, 48000, 2)))
DAI_CONFIG(HDA, 1, 5, iDisp2,
        HDA_CONFIG(HDA_CONFIG_DATA(HDA, 1, 48000, 2)))
DAI_CONFIG(HDA, 2, 6, iDisp3,
        HDA_CONFIG(HDA_CONFIG_DATA(HDA, 2, 48000, 2)))
DAI_CONFIG(HDA, 3, 7, iDisp4,
        HDA_CONFIG(HDA_CONFIG_DATA(HDA, 3, 48000, 2)))

# BT offload on SSP2 (ID: 8)
include(`ssp.m4')
define(`SSP_MCLK', 19200000)

define(`hwconfig_names', HW_CONFIG_NAMES(LIST(`     ', "hw_config1", "hw_config2", "hw_config3")))
define(`data_names', DAI_DATA_NAMES(LIST(`     ', "ssp_data1", "ssp_data2", "ssp_data3")))

define(`ssp_config_list_1', LIST(`',
	`MULTI_SSP_CONFIG(hw_config1, 8, DSP_A, SSP_CLOCK(mclk, SSP_MCLK, codec_mclk_in),'
		`SSP_CLOCK(bclk, 128000, codec_master, inverted),'
		`SSP_CLOCK(fsync, 8000, codec_master),'
		`SSP_TDM(1, 16, 1, 1),'
		`SSP_MULTI_CONFIG_DATA(ssp_data1, 16))',
	`MULTI_SSP_CONFIG(hw_config2, 9, DSP_A, SSP_CLOCK(mclk, SSP_MCLK, codec_mclk_in),'
		`SSP_CLOCK(bclk, 256000, codec_master, inverted),'
		`SSP_CLOCK(fsync, 16000, codec_master),'
		`SSP_TDM(1, 16, 1, 1),'
		`SSP_MULTI_CONFIG_DATA(ssp_data2, 16))',
	`MULTI_SSP_CONFIG(hw_config3, 10, DSP_A, SSP_CLOCK(mclk, eval(SSP_MCLK * 2), codec_mclk_in),'
		`SSP_CLOCK(bclk, 1536000, codec_master),'
		`SSP_CLOCK(fsync, 48000, codec_master),'
		`SSP_TDM(2, 16, 3, 3),'
		`SSP_MULTI_CONFIG_DATA(ssp_data3, 16))'))

MULTI_DAI_CONFIG(SSP, 2, 8, SSP2-NoCodec, ssp_config_list_1, hwconfig_names, data_names)

DEBUG_END
