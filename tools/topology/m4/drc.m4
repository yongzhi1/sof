divert(-1)

dnl Define macro for Dyanamic Range Compressor widget
DECLARE_SOF_RT_UUID("drc", drc_uuid, 0xb809efaf, 0x5681, 0x42b1,
		    0x9e, 0xd6, 0x04, 0xbb, 0x01, 0x2d, 0xd3, 0x84)

dnl N_DRC(name)
define(`N_DRC', `DRC'PIPELINE_ID`.'$1)

dnl W_DRC(name, format, periods_sink, periods_source, kcontrol_list)
define(`W_DRC',
`SectionVendorTuples."'N_DRC($1)`_tuples_uuid" {'
`	tokens "sof_comp_tokens"'
`	tuples."uuid" {'
`		SOF_TKN_COMP_UUID'		STR(drc_uuid)
`	}'
`}'
`SectionData."'N_DRC($1)`_data_uuid" {'
`	tuples "'N_DRC($1)`_tuples_uuid"'
`}'
`SectionVendorTuples."'N_DRC($1)`_tuples_w" {'
`	tokens "sof_comp_tokens"'
`	tuples."word" {'
`		SOF_TKN_COMP_PERIOD_SINK_COUNT'		STR($3)
`		SOF_TKN_COMP_PERIOD_SOURCE_COUNT'	STR($4)
`	}'
`}'
`SectionData."'N_DRC($1)`_data_w" {'
`	tuples "'N_DRC($1)`_tuples_w"'
`}'
`SectionVendorTuples."'N_DRC($1)`_tuples_str" {'
`	tokens "sof_comp_tokens"'
`	tuples."string" {'
`		SOF_TKN_COMP_FORMAT'	STR($2)
`	}'
`}'
`SectionData."'N_DRC($1)`_data_str" {'
`	tuples "'N_DRC($1)`_tuples_str"'
`}'
`SectionVendorTuples."'N_DRC($1)`_tuples_str_type" {'
`	tokens "sof_process_tokens"'
`	tuples."string" {'
`		SOF_TKN_PROCESS_TYPE'	"DRC"
`	}'
`}'
`SectionData."'N_DRC($1)`_data_str_type" {'
`	tuples "'N_DRC($1)`_tuples_str_type"'
`}'
`SectionWidget."'N_DRC($1)`" {'
`	index "'PIPELINE_ID`"'
`	type "effect"'
`	no_pm "true"'
`	data ['
`		"'N_DRC($1)`_data_uuid"'
`		"'N_DRC($1)`_data_w"'
`		"'N_DRC($1)`_data_str"'
`		"'N_DRC($1)`_data_str_type"'
`	]'
`	bytes ['
		$5
`	]'
`}')

divert(0)dnl
