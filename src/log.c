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
LogLevel logThreshold;

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

#undef lprintln
void lprintln(LogLevel level, const char* str) {
	lprintHead(level);
	fprintf(logStream, "%s\n", str);
}

#undef lprintf
void lprintf(LogLevel level, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	lvprintf(level, fmt, args);
	va_end(args);
}

#undef lvprintf
void lvprintf(LogLevel level, const char* fmt, va_list args) {
	lprintHead(level);
	vfprintf(logStream, fmt, args);
}

#undef lprintHead
void lprintHead(LogLevel level) {
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

#undef lprintDirectf
void lprintDirectf(LogLevel level, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	lvprintDirectf(level, fmt, args);
	va_end(args);
}

#undef lvprintDirectf
void lvprintDirectf(LogLevel level, const char* fmt, va_list args) {
	vfprintf(logStream, fmt, args);
}
