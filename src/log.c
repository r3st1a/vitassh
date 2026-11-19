/*
 * File logging to ux0:data/vitassh/log.txt.
 *
 * A vita2d application has no console, so this is the only way to see what
 * happened on real hardware -- and the interesting case is the crash, where
 * nothing gets a chance to flush a buffer or close a handle. So every line is
 * its own fopen("ab") / fwrite / fclose: the line is on the device before
 * log_printf() returns, whatever happens next. At a handful of lines per
 * session the syscalls do not matter, and it keeps the log honest.
 */
#include "log.h"

#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "paths.h"

/* Start a fresh file rather than appending across sessions without bound. */
#define LOG_MAX_BYTES (256 * 1024)

static int log_ready;

/* Seconds since boot, so lines can be ordered without a wall clock. */
static double log_stamp(void)
{
	return (double)sceKernelGetSystemTimeWide() / 1000000.0;
}

void log_open(void)
{
	SceIoStat st;

	sceIoMkdir(VITASSH_DIR, 0777);

	if (sceIoGetstat(VITASSH_LOG, &st) >= 0 && st.st_size > LOG_MAX_BYTES)
		remove(VITASSH_LOG);

	log_ready = 1;
	log_printf("--- vitassh start ---");
}

void log_close(void)
{
	if (!log_ready) return;
	log_printf("--- vitassh exit ---");
	log_ready = 0;
}

void log_printf(const char *fmt, ...)
{
	char line[256];
	size_t n;
	va_list ap;
	FILE *f;

	if (!log_ready) return;

	n = (size_t)snprintf(line, sizeof(line), "[%8.3f] ", log_stamp());
	if (n > sizeof(line) - 2) n = sizeof(line) - 2;

	va_start(ap, fmt);
	vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
	va_end(ap);

	n = strlen(line);
	line[n++] = '\n';

	f = fopen(VITASSH_LOG, "ab");
	if (f == NULL) return;

	fwrite(line, 1, n, f);
	fclose(f);
}
