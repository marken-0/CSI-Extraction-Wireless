#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"
#include "lwip/err.h"
#include "lwip/sys.h"

// Include our redesigned components
#include "../../_components/storage_manager.h"
#include "../../_components/csi_handler.h"
#include "../../_components/timestamp_manager.h"
#include "../../_components/command_processor.h"

// Network and device configuration constants
#define WIFI_ACCESS_POINT_SSID      "ESP32-AP"
#define WIFI_ACCESS_POINT_PASSWORD  "esp32-ap"
#define WIFI_CHANNEL_NUMBER         6
#define MAX_STATION_CONNECTIONS     10
#define CSI_DATA_QUEUE_SIZE         64
#define HOST_COMMUNICATION_PORT     9999
#define DEVICE_HOSTNAME_PREFIX      "ESP32_CSI_Collector"

// Application configuration
#define CSI_PROCESSING_TASK_PRIORITY    5
#define CSI_PROCESSING_STACK_SIZE       8192
#define UDP_PAYLOAD_BUFFER_SIZE         4096
#define MDNS_SERVICE_NAME              "csi-collector"
#define MDNS_PROTOCOL                  "_udp"

static const char *APPLICATION_TAG = "CSI_Collector_AP";

// Structure to manage application state
typedef struct {
    xQueueHandle csi_data_queue;
    char* target_host_address;
    int udp_socket_descriptor;
    bool network_ready;
    uint32_t processed_packets;
    char discovered_host_ip[16];
    bool host_discovered;
} application_state_t;

static application_state_t app_state = {0};

// Known research device MAC addresses for filtering
static const char authorized_devices[][20] = {
    "a0:b7:65:5a:08:a5",  // Research ESP32 Device A
    "24:0a:c4:c9:25:d8",  // Research ESP32 Device B  
};
static const uint8_t NUM_AUTHORIZED_DEVICES = sizeof(authorized_devices) / sizeof(authorized_devices[0]);

// Task function declarations
static void csi_data_processing_task(void *parameters);
static void network_event_handler(void* arg, esp_event_base_t event_base, 
                                 int32_t event_id, void* event_data);
static void mdns_discovery_task(void *parameters);
static esp_err_t setup_mdns_service(void);
static esp_err_t discover_host_computer(void);

// Enhanced MAC address validation function
bool is_authorized_research_device(uint8_t device_mac[6]) {
    char mac_address_string[20];
    snprintf(mac_address_string, sizeof(mac_address_string), 
             "%02x:%02x:%02x:%02x:%02x:%02x", 
             device_mac[0], device_mac[1], device_mac[2], 
             device_mac[3], device_mac[4], device_mac[5]);
    
    // Check against authorized device list
    for (int device_index = 0; device_index < NUM_AUTHORIZED_DEVICES; device_index++) {
        if (strcmp(mac_address_string, authorized_devices[device_index]) == 0) {
            ESP_LOGI(APPLICATION_TAG, "Authorized device detected: %s", mac_address_string);
            return true;
        }
    }
    
    ESP_LOGD(APPLICATION_TAG, "Unauthorized device filtered: %s", mac_address_string);
    return false;
}

// CSI callback that implements filtering
void research_csi_data_callback(void *context, wifi_csi_info_t *csi_info) {
    if (!csi_info || !csi_info->buf) {
        ESP_LOGW(APPLICATION_TAG, "Invalid CSI data received");
        return;
    }
    
    // Apply MAC address filtering for given devices only
    if (!is_authorized_research_device(csi_info->mac)) {
        return; 
    }
    
    // Create a copy of CSI data for queue processing
    wifi_csi_info_t *csi_copy = malloc(sizeof(wifi_csi_info_t));
    if (!csi_copy) {
        ESP_LOGE(APPLICATION_TAG, "Failed to allocate memory for CSI data");
        return;
    }
    
    // Deep copy the CSI information
    memcpy(csi_copy, csi_info, sizeof(wifi_csi_info_t));
    
    // Allocate and copy the buffer data
    csi_copy->buf = malloc(csi_info->len);
    if (!csi_copy->buf) {
        free(csi_copy);
        ESP_LOGE(APPLICATION_TAG, "Failed to allocate memory for CSI buffer");
        return;
    }
    memcpy(csi_copy->buf, csi_info->buf, csi_info->len);
    
    // Queue the data for processing (non-blocking)
    if (xQueueSend(app_state.csi_data_queue, &csi_copy, 0) != pdTRUE) {
        // Queue is full, free the allocated memory
        free(csi_copy->buf);
        free(csi_copy);
        ESP_LOGW(APPLICATION_TAG, "CSI data queue overflow - dropping packet");
    }
}

// Network event handler for WiFi and IP events
static void network_event_handler(void* arg, esp_event_base_t event_base,
                                 int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_AP_START:
                ESP_LOGI(APPLICATION_TAG, "Access Point started successfully");
                app_state.network_ready = true;
                break;
                
            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(APPLICATION_TAG, "Access Point stopped");
                app_state.network_ready = false;
                break;
                
            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
                ESP_LOGI(APPLICATION_TAG, "Station connected - MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                         event->mac[0], event->mac[1], event->mac[2],
                         event->mac[3], event->mac[4], event->mac[5]);
                break;
            }
            
            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
                ESP_LOGI(APPLICATION_TAG, "Station disconnected - MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                         event->mac[0], event->mac[1], event->mac[2],
                         event->mac[3], event->mac[4], event->mac[5]);
                break;
            }
            
            default:
                ESP_LOGD(APPLICATION_TAG, "Unhandled WiFi event: %ld", event_id);
                break;
        }
    }
}

// Initialize and configure WiFi Access Point
static esp_err_t configure_wifi_access_point(void) {
    ESP_LOGI(APPLICATION_TAG, "Configuring WiFi Access Point...");
    
    // Initialize network interface
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create default AP network interface
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) {
        ESP_LOGE(APPLICATION_TAG, "Failed to create AP network interface");
        return ESP_FAIL;
    }
    
    // Configure custom IP settings for the AP
    esp_netif_ip_info_t ip_config = {0};
    inet_pton(AF_INET, "192.168.4.1", &ip_config.ip.addr);
    inet_pton(AF_INET, "192.168.4.1", &ip_config.gw.addr);
    inet_pton(AF_INET, "255.255.255.0", &ip_config.netmask.addr);
    
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_config));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));
    
    // Initialize WiFi with default configuration
    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
    
    // Register event handler for network events
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &network_event_handler, NULL));
    
    // Configure Access Point settings
    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_ACCESS_POINT_SSID,
            .ssid_len = strlen(WIFI_ACCESS_POINT_SSID),
            .password = WIFI_ACCESS_POINT_PASSWORD,
            .channel = WIFI_CHANNEL_NUMBER,
            .max_connection = MAX_STATION_CONNECTIONS,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .beacon_interval = 100,
        },
    };
    
    // Set WiFi mode and configuration
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(APPLICATION_TAG, "WiFi AP configured: SSID=%s, Channel=%d", 
             WIFI_ACCESS_POINT_SSID, WIFI_CHANNEL_NUMBER);
    
    return ESP_OK;
}

// Setup mDNS service for host discovery
static esp_err_t setup_mdns_service(void) {
    ESP_LOGI(APPLICATION_TAG, "Setting up mDNS service...");
    
    // Initialize mDNS
    esp_err_t result = mdns_init();
    if (result != ESP_OK) {
        ESP_LOGE(APPLICATION_TAG, "mDNS initialization failed: %s", esp_err_to_name(result));
        return result;
    }
    
    // Set mDNS hostname
    char hostname[32];
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(hostname, sizeof(hostname), "%s_%02x%02x", 
             DEVICE_HOSTNAME_PREFIX, mac[4], mac[5]);
    
    result = mdns_hostname_set(hostname);
    if (result != ESP_OK) {
        ESP_LOGE(APPLICATION_TAG, "Failed to set mDNS hostname: %s", esp_err_to_name(result));
        return result;
    }
    
    // Set mDNS instance name
    result = mdns_instance_name_set("ESP32 CSI Data Collector");
    if (result != ESP_OK) {
        ESP_LOGE(APPLICATION_TAG, "Failed to set mDNS instance name: %s", esp_err_to_name(result));
        return result;
    }
    
    // Add mDNS service for CSI data collection
    result = mdns_service_add(NULL, MDNS_SERVICE_NAME, MDNS_PROTOCOL, 
                              HOST_COMMUNICATION_PORT, NULL, 0);
    if (result != ESP_OK) {
        ESP_LOGE(APPLICATION_TAG, "Failed to add mDNS service: %s", esp_err_to_name(result));
        return result;
    }
    
    ESP_LOGI(APPLICATION_TAG, "mDNS service configured: hostname=%s", hostname);
    return ESP_OK;
}

// Discover host computer using mDNS
static esp_err_t discover_host_computer(void) {
    ESP_LOGI(APPLICATION_TAG, "Searching for host computer via mDNS...");
    
    // Query for research host computers
    mdns_result_t *results = NULL;
    esp_err_t result = mdns_query_ptr("_ssh", "_tcp", 3000, 20, &results);
    
    if (result != ESP_OK) {
        ESP_LOGW(APPLICATION_TAG, "mDNS query failed, using broadcast discovery");
        return ESP_FAIL;
    }
    
    // Process mDNS results
    mdns_result_t *current = results;
    while (current) {
        if (current->addr && current->hostname) {
            ESP_LOGI(APPLICATION_TAG, "Found host: %s at %s", 
                     current->hostname, inet_ntoa(current->addr->addr.u_addr.ip4));
            
            // Store the first discovered host
            if (!app_state.host_discovered) {
                strcpy(app_state.discovered_host_ip, inet_ntoa(current->addr->addr.u_addr.ip4));
                app_state.host_discovered = true;
                ESP_LOGI(APPLICATION_TAG, "Selected host: %s", app_state.discovered_host_ip);
            }
        }
        current = current->next;
    }
    
    // Free mDNS results
    mdns_query_results_free(results);
    
    if (!app_state.host_discovered) {
        ESP_LOGW(APPLICATION_TAG, "No host computer discovered via mDNS");
        // Fallback to broadcast or default gateway
        strcpy(app_state.discovered_host_ip, "192.168.4.255"); // Broadcast address
        app_state.host_discovered = true;
        ESP_LOGI(APPLICATION_TAG, "Using broadcast address: %s", app_state.discovered_host_ip);
    }
    
    return ESP_OK;
}

// mDNS discovery task
static void mdns_discovery_task(void *parameters) {
    ESP_LOGI(APPLICATION_TAG, "mDNS discovery task started");
    
    // Wait for network to be ready
    while (!app_state.network_ready) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // Setup mDNS service
    if (setup_mdns_service() != ESP_OK) {
        ESP_LOGE(APPLICATION_TAG, "Failed to setup mDNS service");
        vTaskDelete(NULL);
        return;
    }
    
    // Periodic host discovery
    while (true) {
        if (!app_state.host_discovered) {
            discover_host_computer();
        }
        
        // Re-discover every 30 seconds if no host found
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

// Initialize UDP socket for data transmission
static int initialize_udp_socket(void) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(APPLICATION_TAG, "Failed to create UDP socket: %s", strerror(errno));
        return -1;
    }
    
    // Enable broadcast for the socket
    int broadcast_enable = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable)) < 0) {
        ESP_LOGW(APPLICATION_TAG, "Failed to enable broadcast: %s", strerror(errno));
    }
    
    ESP_LOGI(APPLICATION_TAG, "UDP socket initialized successfully");
    return sock;
}

// Send CSI data via UDP
static esp_err_t transmit_csi_data(wifi_csi_info_t *csi_data, const char *formatted_data) {
    if (!app_state.host_discovered || app_state.udp_socket_descriptor < 0) {
        return ESP_FAIL;
    }
    
    struct sockaddr_in dest_addr = {0};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(HOST_COMMUNICATION_PORT);
    dest_addr.sin_addr.s_addr = inet_addr(app_state.discovered_host_ip);
    
    int bytes_sent = sendto(app_state.udp_socket_descriptor, formatted_data, 
                           strlen(formatted_data), 0, 
                           (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    
    if (bytes_sent < 0) {
        ESP_LOGE(APPLICATION_TAG, "UDP transmission failed: %s", strerror(errno));
        return ESP_FAIL;
    }
    
    app_state.processed_packets++;
    return ESP_OK;
}

// CSI data processing task
static void csi_data_processing_task(void *parameters) {
    ESP_LOGI(APPLICATION_TAG, "CSI data processing task started");
    
    wifi_csi_info_t *csi_data = NULL;
    char *output_buffer = malloc(UDP_PAYLOAD_BUFFER_SIZE);
    
    if (!output_buffer) {
        ESP_LOGE(APPLICATION_TAG, "Failed to allocate output buffer");
        vTaskDelete(NULL);
        return;
    }
    
    while (true) {
        // Wait for CSI data from queue
        if (xQueueReceive(app_state.csi_data_queue, &csi_data, portMAX_DELAY) == pdTRUE) {
            if (!csi_data) {
                continue;
            }
            
            // Format CSI data for transmission
            memset(output_buffer, 0, UDP_PAYLOAD_BUFFER_SIZE);
            
            // Create formatted output using the enhanced CSI callback logic
            char mac_string[20] = {0};
            snprintf(mac_string, sizeof(mac_string), "%02X:%02X:%02X:%02X:%02X:%02X",
                     csi_data->mac[0], csi_data->mac[1], csi_data->mac[2],
                     csi_data->mac[3], csi_data->mac[4], csi_data->mac[5]);
            
            // Get current timestamp
            char *timestamp = get_formatted_timestamp();
            
            // Build the formatted CSI data string
            int offset = snprintf(output_buffer, UDP_PAYLOAD_BUFFER_SIZE,
                                 "CSI_Data,AP,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,%d,[",
                                 mac_string,
                                 csi_data->rx_ctrl.rssi, csi_data->rx_ctrl.rate, csi_data->rx_ctrl.sig_mode,
                                 csi_data->rx_ctrl.mcs, csi_data->rx_ctrl.cwb, csi_data->rx_ctrl.smoothing,
                                 csi_data->rx_ctrl.not_sounding, csi_data->rx_ctrl.aggregation, csi_data->rx_ctrl.stbc,
                                 csi_data->rx_ctrl.fec_coding, csi_data->rx_ctrl.sgi, csi_data->rx_ctrl.noise_floor,
                                 csi_data->rx_ctrl.ampdu_cnt, csi_data->rx_ctrl.channel, csi_data->rx_ctrl.secondary_channel,
                                 csi_data->rx_ctrl.timestamp, csi_data->rx_ctrl.ant, csi_data->rx_ctrl.sig_len,
                                 csi_data->rx_ctrl.rx_state, (int)is_time_synchronized(),
                                 timestamp ? timestamp : "0.0", csi_data->len);
            
            int8_t *data_ptr = csi_data->buf;
            for (int i = 0; i < 64 && (i * 2 + 1) < csi_data->len && offset < (UDP_PAYLOAD_BUFFER_SIZE - 50); i++) {
                double real_part = data_ptr[i * 2];
                double imag_part = data_ptr[i * 2 + 1];
                double amplitude = sqrt(real_part * real_part + imag_part * imag_part);
                offset += snprintf(output_buffer + offset, UDP_PAYLOAD_BUFFER_SIZE - offset, "%.4f ", amplitude);
            }
            
            // Close the data array
            snprintf(output_buffer + offset, UDP_PAYLOAD_BUFFER_SIZE - offset, "]\n");
            
            // Transmit the formatted data
            transmit_csi_data(csi_data, output_buffer);
            
            // Print to console as well
            printf("%s", output_buffer);
            
            // Free allocated memory
            if (timestamp) {
                free(timestamp);
            }
            free(csi_data->buf);
            free(csi_data);
            csi_data = NULL;
        }
    }
    
    free(output_buffer);
    vTaskDelete(NULL);
}

void app_main(void) {
    ESP_LOGI(APPLICATION_TAG, "ESP32 CSI Data Collector - Access Point Mode");
    ESP_LOGI(APPLICATION_TAG, "Firmware Version: 1.2.0");
    
    // Initialize non-volatile storage
    storage_init_result_t storage_result = initialize_non_volatile_storage();
    if (!storage_result.success) {
        ESP_LOGE(APPLICATION_TAG, "Storage initialization failed");
        return;
    }
    
    // Initialize timestamp manager
    reset_time_sync_status();
    ESP_LOGI(APPLICATION_TAG, "Timestamp manager initialized");
    
    // Initialize command processor
    initialize_command_processor(true);
    
    // Create CSI data queue
    app_state.csi_data_queue = xQueueCreate(CSI_DATA_QUEUE_SIZE, sizeof(wifi_csi_info_t*));
    if (!app_state.csi_data_queue) {
        ESP_LOGE(APPLICATION_TAG, "Failed to create CSI data queue");
        return;
    }
    
    // Configure WiFi Access Point
    esp_err_t wifi_result = configure_wifi_access_point();
    if (wifi_result != ESP_OK) {
        ESP_LOGE(APPLICATION_TAG, "WiFi AP configuration failed: %s", esp_err_to_name(wifi_result));
        return;
    }
    
    // Initialize UDP socket
    app_state.udp_socket_descriptor = initialize_udp_socket();
    if (app_state.udp_socket_descriptor < 0) {
        ESP_LOGE(APPLICATION_TAG, "UDP socket initialization failed");
        return;
    }
    
    // Initialize CSI collection with callback
    esp_err_t csi_result = initialize_csi_collection("collector", CSI_MODE_AMPLITUDE, 
                                                     research_csi_data_callback);
    if (csi_result != ESP_OK) {
        ESP_LOGE(APPLICATION_TAG, "CSI initialization failed: %s", esp_err_to_name(csi_result));
        return;
    }
    
    // Create processing tasks
    xTaskCreate(csi_data_processing_task, "csi_processor", 
                CSI_PROCESSING_STACK_SIZE, NULL, CSI_PROCESSING_TASK_PRIORITY, NULL);
    
    xTaskCreate(mdns_discovery_task, "mdns_discovery", 
                4096, NULL, 3, NULL);
    
    // Start command monitoring in main task
    ESP_LOGI(APPLICATION_TAG, "System initialization complete");
    ESP_LOGI(APPLICATION_TAG, "Access Point SSID: %s", WIFI_ACCESS_POINT_SSID);
    ESP_LOGI(APPLICATION_TAG, "Data transmission port: %d", HOST_COMMUNICATION_PORT);
    
    // Main command processing loop
    start_command_monitoring_loop();

}