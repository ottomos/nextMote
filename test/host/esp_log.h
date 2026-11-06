#pragma once
#include <stddef.h>
void host_test_log(const char *tag, const char *format, ...);
#define ESP_LOGI(...) host_test_log(__VA_ARGS__)
#define ESP_LOGW(...) host_test_log(__VA_ARGS__)
#define ESP_LOGD(...) host_test_log(__VA_ARGS__)
#define ESP_LOGE(...) host_test_log(__VA_ARGS__)
#define ESP_LOG_BUFFER_HEX(tag, data, length) ((void)(tag), (void)(data), (void)(length))
