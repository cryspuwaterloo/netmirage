#include "log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

const char* LogLevelStrings[] = {"DEBUG", "INFO", "WARNING", "ERROR", NULL};

static FILE* logStream;
static bool closeLog;
LogLevel _logThreshold;

void setLogStream(FILE* output) {
	logStream = output;
	closeLog = false;
}

bool setLogFile(const char* filename) {
	FILE* fout = fopen(filename, "a");
	if (!fout) return false;
	logStream = fout;
	closeLog = true;
	return true;
}

void cleanupLog() {
	if (closeLog) {
		fclose(logStream);
		logStream = NULL;
		closeLog = false;
	}
}

void setLogThreshold(LogLevel level) {
	_logThreshold = level;
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
