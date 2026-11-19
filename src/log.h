/*
 * File logging to ux0:data/vitassh/log.txt.
 *
 * A vita2d application has no console, so this is the only way to see what
 * happened on real hardware. Every write is flushed immediately: a log that
 * vanishes along with the crash is worthless.
 */
#ifndef VITASSH_LOG_H
#define VITASSH_LOG_H

void log_open(void);
void log_close(void);
void log_printf(const char *fmt, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 1, 2)))
#endif
	;

#endif /* VITASSH_LOG_H */
