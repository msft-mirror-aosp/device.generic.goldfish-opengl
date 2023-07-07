// Generated Code - DO NOT EDIT !!
// generated by 'emugen'
#ifndef __magma_client_ftable_t_h
#define __magma_client_ftable_t_h


static const struct _magma_funcs_by_name {
	const char *name;
	void *proc;
} magma_funcs_by_name[] = {
	{"magma_device_import", (void*)magma_device_import},
	{"magma_device_release", (void*)magma_device_release},
	{"magma_device_query", (void*)magma_device_query},
	{"magma_device_create_connection", (void*)magma_device_create_connection},
	{"magma_connection_release", (void*)magma_connection_release},
	{"magma_connection_get_error", (void*)magma_connection_get_error},
	{"magma_connection_create_context", (void*)magma_connection_create_context},
	{"magma_connection_release_context", (void*)magma_connection_release_context},
	{"magma_connection_create_buffer", (void*)magma_connection_create_buffer},
	{"magma_connection_release_buffer", (void*)magma_connection_release_buffer},
	{"magma_connection_import_buffer", (void*)magma_connection_import_buffer},
	{"magma_connection_create_semaphore", (void*)magma_connection_create_semaphore},
	{"magma_connection_release_semaphore", (void*)magma_connection_release_semaphore},
	{"magma_connection_import_semaphore", (void*)magma_connection_import_semaphore},
	{"magma_connection_perform_buffer_op", (void*)magma_connection_perform_buffer_op},
	{"magma_connection_map_buffer", (void*)magma_connection_map_buffer},
	{"magma_connection_unmap_buffer", (void*)magma_connection_unmap_buffer},
	{"magma_connection_execute_command", (void*)magma_connection_execute_command},
	{"magma_connection_execute_immediate_commands", (void*)magma_connection_execute_immediate_commands},
	{"magma_connection_flush", (void*)magma_connection_flush},
	{"magma_connection_get_notification_channel_handle", (void*)magma_connection_get_notification_channel_handle},
	{"magma_connection_read_notification_channel", (void*)magma_connection_read_notification_channel},
	{"magma_buffer_clean_cache", (void*)magma_buffer_clean_cache},
	{"magma_buffer_set_cache_policy", (void*)magma_buffer_set_cache_policy},
	{"magma_buffer_get_cache_policy", (void*)magma_buffer_get_cache_policy},
	{"magma_buffer_set_name", (void*)magma_buffer_set_name},
	{"magma_buffer_get_info", (void*)magma_buffer_get_info},
	{"magma_buffer_get_handle", (void*)magma_buffer_get_handle},
	{"magma_buffer_export", (void*)magma_buffer_export},
	{"magma_semaphore_signal", (void*)magma_semaphore_signal},
	{"magma_semaphore_reset", (void*)magma_semaphore_reset},
	{"magma_semaphore_export", (void*)magma_semaphore_export},
	{"magma_poll", (void*)magma_poll},
	{"magma_initialize_tracing", (void*)magma_initialize_tracing},
	{"magma_initialize_logging", (void*)magma_initialize_logging},
	{"magma_connection_enable_performance_counter_access", (void*)magma_connection_enable_performance_counter_access},
	{"magma_connection_enable_performance_counters", (void*)magma_connection_enable_performance_counters},
	{"magma_connection_create_performance_counter_buffer_pool", (void*)magma_connection_create_performance_counter_buffer_pool},
	{"magma_connection_release_performance_counter_buffer_pool", (void*)magma_connection_release_performance_counter_buffer_pool},
	{"magma_connection_add_performance_counter_buffer_offsets_to_pool", (void*)magma_connection_add_performance_counter_buffer_offsets_to_pool},
	{"magma_connection_remove_performance_counter_buffer_from_pool", (void*)magma_connection_remove_performance_counter_buffer_from_pool},
	{"magma_connection_dump_performance_counters", (void*)magma_connection_dump_performance_counters},
	{"magma_connection_clear_performance_counters", (void*)magma_connection_clear_performance_counters},
	{"magma_connection_read_performance_counter_completion", (void*)magma_connection_read_performance_counter_completion},
	{"magma_virt_connection_create_image", (void*)magma_virt_connection_create_image},
	{"magma_virt_connection_get_image_info", (void*)magma_virt_connection_get_image_info},
};
static const int magma_num_funcs = sizeof(magma_funcs_by_name) / sizeof(struct _magma_funcs_by_name);


#endif
