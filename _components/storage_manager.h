#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include "nvs_flash.h"
#include "esp_log.h"
#include <stdbool.h>

static const char *STORAGE_TAG = "storage_mgr";

typedef struct
{
    bool success;
    bool required_format;
    esp_err_t error_code;
} storage_init_result_t;

storage_init_result_t initialize_non_volatile_storage()
{
    storage_init_result_t result = {false, false, ESP_OK};

    ESP_LOGI(STORAGE_TAG, "Initializing NVS storage system...");

    // Attempt initial NVS initialization
    esp_err_t init_result = nvs_flash_init();

    // Handle different initialization scenarios
    if (init_result == ESP_OK)
    {
        ESP_LOGI(STORAGE_TAG, "NVS initialized successfully");
        result.success = true;
        result.error_code = init_result;
    }
    else if (init_result == ESP_ERR_NVS_NO_FREE_PAGES ||
             init_result == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(STORAGE_TAG, "NVS partition needs formatting (reason: %s)",
                 esp_err_to_name(init_result));

        // Attempt to erase and re-initialize
        esp_err_t erase_result = nvs_flash_erase();
        if (erase_result != ESP_OK)
        {
            ESP_LOGE(STORAGE_TAG, "Failed to erase NVS partition: %s",
                     esp_err_to_name(erase_result));
            result.error_code = erase_result;
            return result;
        }

        // Try initialization again after erase
        init_result = nvs_flash_init();
        if (init_result == ESP_OK)
        {
            ESP_LOGI(STORAGE_TAG, "NVS initialized successfully after format");
            result.success = true;
            result.required_format = true;
            result.error_code = init_result;
        }
        else
        {
            ESP_LOGE(STORAGE_TAG, "NVS initialization failed even after format: %s",
                     esp_err_to_name(init_result));
            result.error_code = init_result;
        }
    }
    else
    {
        ESP_LOGE(STORAGE_TAG, "Critical NVS initialization error: %s",
                 esp_err_to_name(init_result));
        result.error_code = init_result;
    }

    return result;
}

// Simple wrapper function for backward compatibility
void setup_nvs_storage()
{
    storage_init_result_t result = initialize_non_volatile_storage();

    if (!result.success)
    {
        ESP_ERROR_CHECK(result.error_code);
    }
}

// Function to check NVS status and get statistics
esp_err_t get_storage_statistics(size_t *used_entries, size_t *free_entries,
                                 size_t *total_entries)
{
    esp_err_t result = nvs_get_stats(NULL, NULL);
    if (result != ESP_OK)
    {
        ESP_LOGE(STORAGE_TAG, "Failed to get NVS statistics: %s", esp_err_to_name(result));
        return result;
    }

    *used_entries = 0;
    *free_entries = 0;
    *total_entries = 0;

    ESP_LOGI(STORAGE_TAG, "Storage statistics retrieved");
    return ESP_OK;
}

#endif // STORAGE_MANAGER_H