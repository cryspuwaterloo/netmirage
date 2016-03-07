#pragma once

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>

typedef enum {
	LogDebug,
	LogInfo,
	LogWarning,
	LogError,
} LogLevel;

extern const char* LogLevelStrings[];

// Prints a line to the log
void lprintln(LogLevel level, const char* str);

// Prints a formatted string to the log
void lprintf(LogLevel level, const char* fmt, ...);
void lvprintf(LogLevel level, const char* fmt, va_list args);

// Logging functions with control over the line header. Avoid when possible.
void lprintHead(LogLevel level);
void lprintDirectf(LogLevel level, const char* fmt, ...);
void lvprintDirectf(LogLevel level, const char* fmt, va_list args);

// Configures the log to write to a file
bool setLogFile(const char* filename);

// Configures the log to write to a stream
void setLogStream(FILE* output);

// Configures the log verbosity threshold
void setLogThreshold(LogLevel level);

// Terminates the logging subsystem
void cleanupLog();
