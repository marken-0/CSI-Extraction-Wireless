#ifndef CSI_HANDLER_H
#define CSI_HANDLER_H

#include "timestamp_manager.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum
{
    CSI_MODE_RAW_DATA = 1,
    CSI_MODE_AMPLITUDE = 2,
    CSI_MODE_PHASE_INFO = 3
} csi_processing_mode_t;

// Configuration structure for CSI collection
typedef struct
{
    csi_processing_mode_t mode;
    char device_role[16];
    bool enable_filtering;
    int buffer_size;
} csi_config_t;

// Global configuration
static csi_config_t g_csi_config;

static inline void format_mac_address(uint8_t *mac_bytes, char *output_buffer)
{
    snprintf(output_buffer, 20, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac_bytes[0], mac_bytes[1], mac_bytes[2],
             mac_bytes[3], mac_bytes[4], mac_bytes[5]);
}

// CSI callback function
void enhanced_csi_callback(void *context, wifi_csi_info_t *csi_data)
{
    if (!csi_data || !csi_data->buf)
    {
        return;
    }

    wifi_csi_info_t local_data = *csi_data;
    char mac_string[20] = {0};
    format_mac_address(local_data.mac, mac_string);

    printf("CSI_DATA,");
    printf("%s,", g_csi_config.device_role);
    printf("%s,", mac_string);

    // Output RX control information with enhanced formatting
    wifi_pkt_rx_ctrl_t *rx_info = &local_data.rx_ctrl;
    printf("%d,%d,%d,%d,%d,", rx_info->rssi, rx_info->rate, rx_info->sig_mode,
           rx_info->mcs, rx_info->cwb);
    printf("%d,%d,%d,%d,%d,", rx_info->smoothing, rx_info->not_sounding,
           rx_info->aggregation, rx_info->stbc, rx_info->fec_coding);
    printf("%d,%d,%d,%d,%d,", rx_info->sgi, rx_info->noise_floor, rx_info->ampdu_cnt,
           rx_info->channel, rx_info->secondary_channel);
    printf("%d,%d,%d,%d,", rx_info->timestamp, rx_info->ant, rx_info->sig_len, rx_info->rx_state);

    // Add timestamp information
    char *current_time = get_formatted_timestamp();
    printf("%d,%s,", is_time_synchronized(), current_time);
    free(current_time);

    // Process CSI data based on configured mode
    int8_t *data_ptr = csi_data->buf;
    printf("%d,[", csi_data->len);

    switch (g_csi_config.mode)
    {
    case CSI_MODE_RAW_DATA:
        for (int idx = 0; idx < 128 && idx < csi_data->len; idx++)
        {
            printf("%d ", data_ptr[idx]);
        }
        break;

    case CSI_MODE_AMPLITUDE:
        for (int idx = 0; idx < 64 && (idx * 2 + 1) < csi_data->len; idx++)
        {
            double real_part = data_ptr[idx * 2];
            double imag_part = data_ptr[idx * 2 + 1];
            double amplitude = sqrt(real_part * real_part + imag_part * imag_part);
            printf("%.4f ", amplitude);
        }
        break;

    case CSI_MODE_PHASE_INFO:
        for (int idx = 0; idx < 64 && (idx * 2 + 1) < csi_data->len; idx++)
        {
            double real_part = data_ptr[idx * 2];
            double imag_part = data_ptr[idx * 2 + 1];
            double phase = atan2(imag_part, real_part);
            printf("%.4f ", phase);
        }
        break;
    }

    printf("]\n");
    vTaskDelay(pdMS_TO_TICKS(1)); // Small delay for stability
}

// Function to print CSV header with different format
void output_csi_header()
{
    const char *header_format = "data_type,node_role,source_mac,rssi,data_rate,signal_mode,"
                                "mcs_index,channel_width,smoothing_enabled,not_sounding,"
                                "aggregation_flag,stbc_enabled,fec_type,short_gi,noise_level,"
                                "ampdu_count,primary_channel,secondary_channel,local_time,"
                                "antenna_id,signal_length,rx_status,time_sync_flag,"
                                "timestamp_value,data_length,csi_measurements\n";
    printf("%s", header_format);
}

// Main initialization function with different parameter structure
esp_err_t initialize_csi_collection(const char *role_name, csi_processing_mode_t mode,
                                    wifi_csi_cb_t custom_callback)
{
    // Store configuration
    strncpy(g_csi_config.device_role, role_name, sizeof(g_csi_config.device_role) - 1);
    g_csi_config.mode = mode;
    g_csi_config.enable_filtering = true;
    g_csi_config.buffer_size = 128;

    // Enable CSI functionality
    esp_err_t result = esp_wifi_set_csi(true);
    if (result != ESP_OK)
    {
        return result;
    }

    // Configure CSI parameters with different structure
    wifi_csi_config_t csi_settings = {
        .lltf_en = false,
        .htltf_en = true,
        .stbc_htltf2_en = false,
        .ltf_merge_en = false,
        .channel_filter_en = false,
        .manu_scale = 0};

    result = esp_wifi_set_csi_config(&csi_settings);
    if (result != ESP_OK)
    {
        return result;
    }

    // Set callback function
    wifi_csi_cb_t callback_func = (custom_callback != NULL) ? custom_callback : enhanced_csi_callback;
    result = esp_wifi_set_csi_rx_cb(callback_func, NULL);
    if (result != ESP_OK)
    {
        return result;
    }

    // Output header
    output_csi_header();

    return ESP_OK;
}

// Utility function to change CSI processing mode at runtime
void set_csi_processing_mode(csi_processing_mode_t new_mode)
{
    g_csi_config.mode = new_mode;
}

// Function to get current configuration
csi_config_t get_csi_configuration()
{
    return g_csi_config;
}

#endif // CSI_HANDLER_H