#define _GNU_SOURCE

#include "log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>
#include <unistd.h>

#include "mem.h"

const char* LogLevelStrings[] = {"DEBUG", "INFO", "WARNING", "ERROR", NULL};

#define COLOR_RESET "\x1b[0m"
#define COLOR_TIMESTAMP "\x1b[32m"
static const char* LogLabelColors[] = { "\x1b[0m", "\x1b[36;1m", "\x1b[31;1m", "\x1b[33;41;1m" };
static const char* LogTextColors[] = { "\x1b[0m", "\x1b[36;22m", "\x1b[33;22m", "\x1b[37;41;1m" };

static GMutex logLock;

static logCallback logFunc;
static FILE* logStream;
static bool closeLog;
static bool useColors;
static const char* _logPrefix;
LogLevel _logThreshold;

void logSetStream(FILE* output) {
	logStream = output;
	int fd = fileno(output);
	logSetColorize(fd != -1 && isatty(fd));
	closeLog = false;
	logFunc = NULL;
}

bool logSetFile(const char* filename) {
	FILE* fout = fopen(filename, "a");
	if (!fout) return false;
	logStream = fout;
	logSetColorize(false);
	closeLog = true;
	logFunc = NULL;
	return true;
}

void logSetCallback(logCallback callback) {
	logStream = NULL;
	logSetColorize(false);
	closeLog = false;
	logFunc = callback;
}

void logSetColorize(bool enabled) {
	useColors = enabled;
}

bool logColorized(void) {
	return useColors;
}

void logSetPrefix(const char* prefix) {
	_logPrefix = prefix;
}

const char* logPrefix(void) {
	return _logPrefix;
}

void logCleanup(void) {
	if (logStream != NULL && closeLog) {
		fclose(logStream);
		logStream = NULL;
		closeLog = false;
	}
}

void logSetThreshold(LogLevel level) {
	_logThreshold = level;
}

LogLevel logThreshold(void) {
	return _logThreshold;
}

static void logVprintf(const char* fmt, va_list args) {
	if (logStream != NULL) {
		vfprintf(logStream, fmt, args);
	} else {
		char* msg;
		if (newVsprintf(&msg, fmt, args) < 0) return;
		logFunc(msg);
		free(msg);
	}
}

static void logPrintf(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	logVprintf(fmt, args);
	va_end(args);
}

static void logPrint(const char* msg) {
	if (logStream != NULL) {
		fprintf(logStream, "%s", msg);
	} else {
		logFunc(msg);
	}
}

int newSprintf(char** buf, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	int res = newVsprintf(buf, fmt, args);
	va_end(args);
	return res;
}

int newVsprintf(char** buf, const char* fmt, va_list args) {
	const size_t defaultBufferSize = 255;
	*buf = emalloc(defaultBufferSize);
	int neededChars = vsnprintf(*buf, defaultBufferSize, fmt, args);
	if (neededChars < 0) {
		free(*buf);
	} else if ((size_t)neededChars >= defaultBufferSize) {
		*buf = earealloc(*buf, (size_t)neededChars, 1, 1); // Extra byte for NUL
		neededChars = vsnprintf(*buf, defaultBufferSize, fmt, args);
	}
	return neededChars;
}

static void colorVfprintf(LogLevel level, bool resetOnlyLast, const char* fmt, va_list args) {
	// We need to emit COLOR_RESET before every newline. Otherwise, things like
	// the background color and intensity will cross line boundaries.
	char* buf;
	int bufLen = newVsprintf(&buf, fmt, args);
	if (resetOnlyLast) {
		if (buf[bufLen - 1] == '\n') {
			buf[bufLen - 1] = '\0';
			logPrint(buf);
			logPrint(COLOR_RESET "\n");
		} else {
			logPrint(buf);
		}
	} else {
		// This is basically strtok, but without the associated problems
		char* p = buf;
		while (true) {
			char* nl = strchr(p, '\n');
			if (nl == NULL) break;
			*nl = '\0';
			logPrint(p);
			logPrint(COLOR_RESET "\n");
			p = nl+1;
		}
		logPrint(p);
	}
	free(buf);
}

void _lprintln(LogLevel level, const char* str) {
	_lprintHead(level);
	logPrint(str);
	if (useColors) {
		logPrint(COLOR_RESET);
	}
	logPrint("\n");
	_lprintDirectFinish();
}

void _lprintf(LogLevel level, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	_lvprintf(level, fmt, args);
	va_end(args);
}

void _lvprintf(LogLevel level, const char* fmt, va_list args) {
	_lprintHead(level);
	if (useColors) {
		colorVfprintf(level, false, fmt, args);
	} else {
		logVprintf(fmt, args);
	}
	_lprintDirectFinish();
}

void _lprintHead(LogLevel level) {
	g_mutex_lock(&logLock);

	// Generate timestamp
	time_t epochTime;
	time(&epochTime);
	struct tm* localTimeInfo = localtime(&epochTime);
	const size_t timeLen = 30;
	char timeStr[timeLen];
	if (!strftime(timeStr, timeLen, "%F %T %Z", localTimeInfo)) {
		strcpy(timeStr, "(Unknown time)");
	}

	// Print prepended args
	const char* prefix = (_logPrefix == NULL ? "" : _logPrefix);
	if (useColors) {
		logPrintf(COLOR_TIMESTAMP "[%s] %s%s%s:%s ", timeStr, LogLabelColors[level], LogLevelStrings[level], prefix, LogTextColors[level]);
	} else {
		logPrintf("[%s] %s%s: ", timeStr, LogLevelStrings[level], prefix);
	}
}

void _lprintDirectf(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	_lvprintDirectf(fmt, args);
	va_end(args);
}

void _lvprintDirectf(const char* fmt, va_list args) {
	if (useColors) {
		colorVfprintf(0, true, fmt, args);
	} else {
		logVprintf(fmt, args);
	}
}

void _lprintDirectFinish(void) {
	// Note that this is not an appropriate place to emit COLOR_RESET codes for
	// colored logs, since some direct print calls might contain embedded
	// newlines.
	g_mutex_unlock(&logLock);
	if (logStream == NULL) {
		logFunc(NULL);
	}
}

void _lprintRaw(const char* str) {
	g_mutex_lock(&logLock);
	logPrint(str);
	g_mutex_unlock(&logLock);
}
