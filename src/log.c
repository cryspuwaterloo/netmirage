#define _GNU_SOURCE

#include "log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <unistd.h>

#include "mem.h"

const char* LogLevelStrings[] = {"DEBUG", "INFO", "WARNING", "ERROR", NULL};

#define COLOR_RESET "\x1b[0m"
#define COLOR_TIMESTAMP "\x1b[32m"
static const char* LogLabelColors[] = { "\x1b[0m", "\x1b[36;1m", "\x1b[31;1m", "\x1b[33;41;1m" };
static const char* LogTextColors[] = { "\x1b[0m", "\x1b[36;22m", "\x1b[33;22m", "\x1b[37;41;1m" };

static FILE* logStream;
static bool closeLog;
static bool useColors;
LogLevel _logThreshold;

void logSetStream(FILE* output) {
	logStream = output;
	int fd = fileno(output);
	logSetColorize(fd != -1 && isatty(fd));
	closeLog = false;
}

bool logSetFile(const char* filename) {
	FILE* fout = fopen(filename, "a");
	if (!fout) return false;
	logStream = fout;
	logSetColorize(false);
	closeLog = true;
	return true;
}

void logSetColorize(bool enabled) {
	useColors = enabled;
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

static void colorVfprintf(LogLevel level, bool resetOnlyLast, const char* fmt, va_list args) {
	// We need to emit COLOR_RESET before every newline. Otherwise, things like
	// the background color and intensity will cross line boundaries.
	char* buf;
	int bufLen = newVsprintf(&buf, fmt, args);
	if (resetOnlyLast) {
		if (buf[bufLen - 1] == '\n') {
			buf[bufLen - 1] = '\0';
			fprintf(logStream, "%s", buf);
			fprintf(logStream, COLOR_RESET "\n");
		} else {
			fprintf(logStream, "%s", buf);
		}
	} else {
		// This is basically strtok, but without the associated problems
		char* p = buf;
		while (true) {
			char* nl = strchr(p, '\n');
			if (nl == NULL) break;
			*nl = '\0';
			fprintf(logStream, "%s", p);
			fprintf(logStream, COLOR_RESET "\n");
			p = nl+1;
		}
		fprintf(logStream, "%s", p);
	}
	free(buf);
}

void _lprintln(LogLevel level, const char* str) {
	_lprintHead(level);
	if (useColors) {
		fprintf(logStream, "%s" COLOR_RESET "\n", str);
	} else {
		fprintf(logStream, "%s\n", str);
	}
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
		vfprintf(logStream, fmt, args);
	}
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
	if (useColors) {
		fprintf(logStream, COLOR_TIMESTAMP "[%s] %s%s:%s ", timeStr, LogLabelColors[level], LogLevelStrings[level], LogTextColors[level]);
	} else {
		fprintf(logStream, "[%s] %s: ", timeStr, LogLevelStrings[level]);
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
		vfprintf(logStream, fmt, args);
	}
}
