#pragma once

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>

// Terminates the logging system
void logCleanup(void);

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

typedef void (*logCallback)(const char* msg);

// Configures the log to redirect strings to a callback function. These strings
// are only valid for the duration of the call. A single print may result in
// multiple calls tbool logColorized(void)o the callback. However, each print "line" will end with a
// call having NULL as the argument.
void logSetCallback(logCallback callback);

// Turns colors on or off. Note that calls to logSetFile and logSetStream
// implicitly modify the color mode, so this should be called after them.
void logSetColorize(bool enabled);
bool logColorized(void);

// Adds a prefix to the head of all messages. The string must be valid until
// logSetPrefix is called again with a NULL parameter.
void logSetPrefix(const char* prefix);
const char* logPrefix(void);

// Configures the log verbosity threshold
void logSetThreshold(LogLevel level);
LogLevel logThreshold(void);

#define PASSES_LOG_THRESHOLD(x) ((x) >= _logThreshold)

// The logging system declares its print statements using macros. These macros
// check the logging threshold in the caller before calling the actual printing
// functions. This prevents logging arguments from being evaluated at all if the
// logging threshold is not met (much like a call to assert in a release build).
// This results in significant performance gains when the logging threshold is
// raised. However, callers should make sure that they do not change program
// state as part of the evaluation of logging function arguments.

// Internal log threshold storage. Callers should use setLogThreshold.
extern LogLevel _logThreshold;

// Prints a line to the log. Do not include newlines in the string.
#define lprintln(level, str) do{ if (PASSES_LOG_THRESHOLD(level)) { _lprintln((level), (str)); } }while(0)

// Prints a formatted string to the log
#define lprintf(level, fmt, ...) do{ if (PASSES_LOG_THRESHOLD(level)) { _lprintf((level), (fmt), ##__VA_ARGS__); } }while(0)
#define lvprintf(level, fmt, args) do{ if (PASSES_LOG_THRESHOLD(level)) { _lvprintf((level), (fmt), (args)); } }while(0)

// Logging functions with control over the line header. Avoid when possible.
// When used, the log will be locked until lprintDirectFinish is called. This
// can lead to deadlocks if the caller does not release the lock after calling
// lprintHead. lprintRaw prints a raw message without applying any log settings.
#define lprintHead(level) do{ if (PASSES_LOG_THRESHOLD(level)) { _lprintHead(level); } }while(0)
#define lprintDirectf(level,  fmt, ...) do{ if (PASSES_LOG_THRESHOLD(level)) { _lprintDirectf((fmt), ##__VA_ARGS__); } }while(0)
#define lvprintDirectf(level, fmt, args) do{ if (PASSES_LOG_THRESHOLD(level)) { _lvprintDirectf((fmt), (args)); } }while(0)
#define lprintDirectFinish(level) do{ if (PASSES_LOG_THRESHOLD(level)) { _lprintDirectFinish(); } }while(0)
#define lprintRaw(str) do{ _lprintRaw(str); }while(0)

// Convenience functions that are often useful for logging operations. Similar
// to the behavior of Linux's (v)asprintf.
int newSprintf(char** buf, const char* fmt, ...);
int newVsprintf(char** buf, const char* fmt, va_list args);

// Internal implementations. These functions do not check the logging level.
// Callers must not call these directly; they should use the macros instead.
void _lprintln(LogLevel level, const char* str);
void _lprintf(LogLevel level, const char* fmt, ...);
void _lvprintf(LogLevel level, const char* fmt, va_list args);
void _lprintHead(LogLevel level);
// The macros require a logging level for these, but internal functions do not:
void _lprintDirectf(/*LogLevel level*/ const char* fmt, ...);
void _lvprintDirectf(/*LogLevel level*/ const char* fmt, va_list args);
void _lprintDirectFinish(/*LogLevel level*/ void);

void _lprintRaw(const char* str);
