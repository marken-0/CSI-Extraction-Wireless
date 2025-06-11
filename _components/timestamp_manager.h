#ifndef TIMESTAMP_MANAGER_H
#define TIMESTAMP_MANAGER_H

#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static bool time_sync_established = false;

#define TIMESTAMP_FORMAT_EXTENDED "SYNC_TIME: %lld.%ld"
#define TIMESTAMP_FORMAT_SIMPLE   "%lld.%ld"
#define TIMESTAMP_FORMAT_READABLE "%lld.%06ld"

typedef struct {
    long long seconds;
    long microseconds;
    bool parse_success;
} timestamp_parse_result_t;

// Enhanced timestamp validation function
bool validate_timestamp_format(const char* input_string) {
    if (!input_string) {
        return false;
    }
    
    long long sec_val;
    long usec_val;
    int fields_parsed = sscanf(input_string, TIMESTAMP_FORMAT_EXTENDED, &sec_val, &usec_val);
    
    if (fields_parsed > 0) {
        return true;
    }
    
    fields_parsed = sscanf(input_string, TIMESTAMP_FORMAT_SIMPLE, &sec_val, &usec_val);
    return (fields_parsed > 0);
}

timestamp_parse_result_t parse_timestamp_string(const char* timestamp_str) {
    timestamp_parse_result_t result = {0, 0, false};
    
    if (!timestamp_str) {
        return result;
    }
    
    int parsed_fields = sscanf(timestamp_str, TIMESTAMP_FORMAT_EXTENDED, 
                               &result.seconds, &result.microseconds);
    
    if (parsed_fields <= 0) {
        parsed_fields = sscanf(timestamp_str, TIMESTAMP_FORMAT_SIMPLE, 
                               &result.seconds, &result.microseconds);
    }
    
    if (parsed_fields > 0) {
        result.parse_success = true;
        if (result.microseconds < 0 || result.microseconds >= 1000000) {
            result.microseconds = 0;
        }
    }
    
    return result;
}

// System time synchronization function with validation
bool synchronize_system_time(const char* timestamp_input) {
    timestamp_parse_result_t parsed_time = parse_timestamp_string(timestamp_input);
    
    if (!parsed_time.parse_success) {
        printf("Warning: Failed to parse timestamp format\n");
        return false;
    }
    
    struct timeval new_time = {
        .tv_sec = (time_t)parsed_time.seconds,  // Cast to time_t
        .tv_usec = parsed_time.microseconds
    };
    
    if (settimeofday(&new_time, NULL) == 0) {
        time_sync_established = true;
        printf("System time synchronized successfully\n");
        return true;
    } else {
        printf("Error: Failed to set system time\n");
        return false;
    }
}

// Get current timestamp as formatted string with memory management
char* get_formatted_timestamp() {
    struct timeval current_time;
    if (gettimeofday(&current_time, NULL) != 0) {
        char* error_timestamp = malloc(20);
        if (error_timestamp) {
            strcpy(error_timestamp, "0.0");
        }
        return error_timestamp;
    }
    
    // Calculate required buffer size
    int buffer_size = snprintf(NULL, 0, TIMESTAMP_FORMAT_READABLE, 
                               (long long)current_time.tv_sec, current_time.tv_usec) + 1;
    
    char* formatted_time = malloc(buffer_size);
    if (!formatted_time) {
        return NULL;
    }
    
    snprintf(formatted_time, buffer_size, TIMESTAMP_FORMAT_READABLE, 
             (long long)current_time.tv_sec, current_time.tv_usec);
    
    return formatted_time;
}

bool is_time_synchronized() {
    return time_sync_established;
}

void get_current_time_components(long long* seconds, long* microseconds) {  // Changed seconds to long long*
    struct timeval current_time;
    if (gettimeofday(&current_time, NULL) == 0) {
        *seconds = (long long)current_time.tv_sec;  // Cast to long long
        *microseconds = current_time.tv_usec;
    } else {
        *seconds = 0;
        *microseconds = 0;
    }
}

void reset_time_sync_status() {
    time_sync_established = false;
}

long get_time_difference_ms(struct timeval* start_time) {
    struct timeval current_time;
    if (gettimeofday(&current_time, NULL) != 0) {
        return -1;
    }
    
    long long seconds_diff = (long long)current_time.tv_sec - (long long)start_time->tv_sec;  // Cast to long long
    long useconds_diff = current_time.tv_usec - start_time->tv_usec;
    
    return (long)(seconds_diff * 1000) + (useconds_diff / 1000);  // Cast result to long
}

#endif // TIMESTAMP_MANAGER_H