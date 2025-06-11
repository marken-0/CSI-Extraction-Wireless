#ifndef COMMAND_PROCESSOR_H
#define COMMAND_PROCESSOR_H

#include "csi_handler.h"
#include "timestamp_manager.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

// Command buffer configuration
#define MAX_COMMAND_LENGTH 512
#define COMMAND_HISTORY_SIZE 5

// Command processing structure
typedef struct
{
    char command_buffer[MAX_COMMAND_LENGTH];
    int buffer_position;
    int commands_processed;
    bool echo_enabled;
} command_processor_t;

// Global command processor instance
static command_processor_t g_cmd_processor = {0};

// Command type enumeration for better organization
typedef enum
{
    CMD_TYPE_UNKNOWN = 0,
    CMD_TYPE_TIME_SYNC,
    CMD_TYPE_CSI_CONFIG,
    CMD_TYPE_SYSTEM_INFO,
    CMD_TYPE_HELP
} command_type_t;

// Function to trim whitespace from strings
static void trim_whitespace(char *str)
{
    if (!str)
        return;

    // Trim leading whitespace
    char *start = str;
    while (isspace((unsigned char)*start))
        start++;

    // Move string to beginning if needed
    if (start != str)
    {
        memmove(str, start, strlen(start) + 1);
    }

    // Trim trailing whitespace
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
        end--;
    end[1] = '\0';
}

// Enhanced command classification function
command_type_t classify_command(const char *command_text)
{
    if (!command_text)
    {
        return CMD_TYPE_UNKNOWN;
    }

    char temp_cmd[MAX_COMMAND_LENGTH];
    strncpy(temp_cmd, command_text, sizeof(temp_cmd) - 1);
    temp_cmd[sizeof(temp_cmd) - 1] = '\0';
    trim_whitespace(temp_cmd);

    // Check for time synchronization commands
    if (validate_timestamp_format(temp_cmd))
    {
        return CMD_TYPE_TIME_SYNC;
    }

    // Check for CSI configuration commands (could be extended)
    if (strncmp(temp_cmd, "CSI_", 4) == 0)
    {
        return CMD_TYPE_CSI_CONFIG;
    }

    // Check for help commands
    if (strcasecmp(temp_cmd, "help") == 0 || strcasecmp(temp_cmd, "?") == 0)
    {
        return CMD_TYPE_HELP;
    }

    // Check for system info commands
    if (strcasecmp(temp_cmd, "status") == 0 || strcasecmp(temp_cmd, "info") == 0)
    {
        return CMD_TYPE_SYSTEM_INFO;
    }

    return CMD_TYPE_UNKNOWN;
}

// Display help information
void display_help_information()
{
    printf("\n=== Available Commands ===\n");
    printf("Time Sync: SYNC_TIME: <seconds>.<microseconds>\n");
    printf("Simple Time: <seconds>.<microseconds>\n");
    printf("System Info: status, info\n");
    printf("Help: help, ?\n");
    printf("CSI Config: CSI_* commands (future expansion)\n");
    printf("===========================\n\n");
}

// Display system status
void display_system_status()
{
    printf("\n=== System Status ===\n");
    printf("Time Synchronized: %s\n", is_time_synchronized() ? "Yes" : "No");
    printf("Commands Processed: %d\n", g_cmd_processor.commands_processed);

    char *current_time = get_formatted_timestamp();
    if (current_time)
    {
        printf("Current Timestamp: %s\n", current_time);
        free(current_time);
    }

    csi_config_t csi_config = get_csi_configuration();
    printf("CSI Mode: %d\n", csi_config.mode);
    printf("Device Role: %s\n", csi_config.device_role);
    printf("====================\n\n");
}

// Process different types of commands
bool execute_classified_command(const char *command_text, command_type_t cmd_type)
{
    bool command_handled = false;

    switch (cmd_type)
    {
    case CMD_TYPE_TIME_SYNC:
        printf("Processing time synchronization: %s\n", command_text);
        command_handled = synchronize_system_time(command_text);
        break;

    case CMD_TYPE_HELP:
        display_help_information();
        command_handled = true;
        break;

    case CMD_TYPE_SYSTEM_INFO:
        display_system_status();
        command_handled = true;
        break;

    case CMD_TYPE_CSI_CONFIG:
        printf("CSI configuration commands not yet implemented: %s\n", command_text);
        command_handled = true;
        break;

    case CMD_TYPE_UNKNOWN:
    default:
        printf("Unrecognized command: %s\n", command_text);
        printf("Type 'help' for available commands\n");
        break;
    }

    if (command_handled)
    {
        g_cmd_processor.commands_processed++;
    }

    return command_handled;
}

// Main command processing function
void process_received_command()
{
    if (strlen(g_cmd_processor.command_buffer) == 0)
    {
        return;
    }

    // Classify and execute the command
    command_type_t cmd_type = classify_command(g_cmd_processor.command_buffer);
    execute_classified_command(g_cmd_processor.command_buffer, cmd_type);
}

// Read and buffer input characters
void scan_for_input_data()
{
    int input_char = fgetc(stdin);

    // Process all available characters
    while (input_char != 0xFF && input_char != EOF)
    {
        if (input_char == '\n' || input_char == '\r')
        {
            // End of command - process it
            g_cmd_processor.command_buffer[g_cmd_processor.buffer_position] = '\0';
            process_received_command();

            // Reset buffer for next command
            memset(g_cmd_processor.command_buffer, 0, sizeof(g_cmd_processor.command_buffer));
            g_cmd_processor.buffer_position = 0;
        }
        else if (g_cmd_processor.buffer_position < (MAX_COMMAND_LENGTH - 1))
        {
            // Add character to buffer if there's space
            g_cmd_processor.command_buffer[g_cmd_processor.buffer_position] = (char)input_char;
            g_cmd_processor.buffer_position++;
        }
        else
        {
            // Buffer overflow protection
            printf("Warning: Command too long, buffer reset\n");
            memset(g_cmd_processor.command_buffer, 0, sizeof(g_cmd_processor.command_buffer));
            g_cmd_processor.buffer_position = 0;
        }

        input_char = fgetc(stdin);
    }
}

// Continuous input monitoring loop
void start_command_monitoring_loop()
{
    printf("Command processor started. Type 'help' for commands.\n");

    while (true)
    {
        scan_for_input_data();
        vTaskDelay(pdMS_TO_TICKS(25)); // Check every 25ms for better responsiveness
    }
}

// Initialize command processor
void initialize_command_processor(bool enable_echo)
{
    memset(&g_cmd_processor, 0, sizeof(g_cmd_processor));
    g_cmd_processor.echo_enabled = enable_echo;
    printf("Command processor initialized\n");
}

// Get processor statistics
void get_processor_stats(int *commands_processed, int *buffer_usage)
{
    *commands_processed = g_cmd_processor.commands_processed;
    *buffer_usage = g_cmd_processor.buffer_position;
}

#endif // COMMAND_PROCESSOR_H