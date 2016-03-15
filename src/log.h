#pragma once

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>

// Terminates the logging system
void logCleanup();

typedef enum {
	LogDebug,
	LogInfo,
	LogWarning,
	LogError,
} LogLevel;

// NULL-terminated list corresponding to LogLevel enum
extern const char* LogLevelStrings[];

// Configures the log to write to a file
bool logSetFile(const char* filename);

// Configures the log to write to a stream
void logSetStream(FILE* output);

// Configures the log verbosity threshold
void logSetThreshold(LogLevel level);

// The logging system declares its print statements using macros. These macros
// check the logging threshold in the caller before calling the actual printing
// functions. This prevents logging arguments from being evaluated at all if the
// logging threshold is not met (much like a call to assert in a release build).
// This results in significant performance gains when the logging threshold is
// raised. However, callers should make sure that they do not change program
// state as part of the evaluation of logging function arguments.

// Internal log threshold comparisons. Callers should use setLogThreshold.
extern LogLevel _logThreshold;
#define PASSES_LOG_THRESHOLD(x) ((x) >= _logThreshold)

// Prints a line to the log
#define lprintln(level, str) if (PASSES_LOG_THRESHOLD(level)) { _lprintln((level), (str)); }

// Prints a formatted string to the log
#define lprintf(level, fmt, ...) if (PASSES_LOG_THRESHOLD(level)) { _lprintf((level), (fmt), ##__VA_ARGS__); }
#define lvprintf(level, fmt, args) if (PASSES_LOG_THRESHOLD(level)) { _lvprintf((level), (fmt), (args)); }

// Logging functions with control over the line header. Avoid when possible.
#define lprintHead(level) if (PASSES_LOG_THRESHOLD(level)) { _lprintHead(level); }
#define lprintDirectf(level, fmt, ...) if (PASSES_LOG_THRESHOLD(level)) { _lprintDirectf((fmt), ##__VA_ARGS__); }
#define lvprintDirectf(level, fmt, args) if (PASSES_LOG_THRESHOLD(level)) { _lvprintDirectf((fmt), (args)); }

// Internal implementations. These functions do not check the logging level.
// Callers must not call these directly; they should use the macros instead.
void _lprintln(LogLevel level, const char* str);
void _lprintf(LogLevel level, const char* fmt, ...);
void _lvprintf(LogLevel level, const char* fmt, va_list args);
void _lprintHead(LogLevel level);
// The macros require a logging level for these, but internal functions do not:
void _lprintDirectf(/*LogLevel level*/ const char* fmt, ...);
void _lvprintDirectf(/*LogLevel level*/ const char* fmt, va_list args);
