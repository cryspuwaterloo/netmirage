#include "log.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

const char* LogLevelStrings[] = {"DEBUG", "INFO", "WARNING", "ERROR", NULL};

static FILE* logStream;
static bool closeLog;
static LogLevel logThreshold;

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
	logThreshold = level;
}

void lprintln(LogLevel level, const char* str) {
	lprintf(level, "%s\n", str);
}

void lprintf(LogLevel level, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	lvprintf(level, fmt, args);
	va_end(args);
}

void lvprintf(LogLevel level, const char* fmt, va_list args) {
	if (!logStream) return;
	if (level < logThreshold) return;

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
	vfprintf(logStream, fmt, args);
}
