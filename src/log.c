#include "log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mem.h"

const char* LogLevelStrings[] = {"DEBUG", "INFO", "WARNING", "ERROR", NULL};

static FILE* logStream;
static bool closeLog;
LogLevel _logThreshold;

void logSetStream(FILE* output) {
	logStream = output;
	closeLog = false;
}

bool logSetFile(const char* filename) {
	FILE* fout = fopen(filename, "a");
	if (!fout) return false;
	logStream = fout;
	closeLog = true;
	return true;
}

void logCleanup(void) {
	if (closeLog) {
		fclose(logStream);
		logStream = NULL;
		closeLog = false;
	}
}

void logSetThreshold(LogLevel level) {
	_logThreshold = level;
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

void _lprintln(LogLevel level, const char* str) {
	_lprintHead(level);
	fprintf(logStream, "%s\n", str);
}

void _lprintf(LogLevel level, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	_lvprintf(level, fmt, args);
	va_end(args);
}

void _lvprintf(LogLevel level, const char* fmt, va_list args) {
	_lprintHead(level);
	vfprintf(logStream, fmt, args);
}

void _lprintHead(LogLevel level) {
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
	fprintf(logStream, "[%s] %s: ", timeStr, LogLevelStrings[level]);
}

void _lprintDirectf(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	_lvprintDirectf(fmt, args);
	va_end(args);
}

void _lvprintDirectf(const char* fmt, va_list args) {
	vfprintf(logStream, fmt, args);
}
