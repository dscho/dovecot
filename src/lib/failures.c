/* Copyright (c) 2002-2007 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "str.h"
#include "backtrace-string.h"
#include "printf-format-fix.h"
#include "write-full.h"
#include "fd-close-on-exec.h"

#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <time.h>

static void failure_exit(int status) ATTR_NORETURN;

static void default_panic_handler(const char *format, va_list args)
	ATTR_NORETURN ATTR_FORMAT(1, 0);
static void default_fatal_handler(int status, const char *format, va_list args)
	ATTR_NORETURN ATTR_FORMAT(2, 0);

static void default_error_handler(const char *format, va_list args)
	ATTR_FORMAT(1, 0);
static void default_warning_handler(const char *format, va_list args)
	ATTR_FORMAT(1, 0);
static void default_info_handler(const char *format, va_list args)
	ATTR_FORMAT(1, 0);

/* Initialize working defaults */
static failure_callback_t *panic_handler ATTR_NORETURN =
	default_panic_handler;
static fatal_failure_callback_t *fatal_handler ATTR_NORETURN =
	default_fatal_handler;
static failure_callback_t *error_handler = default_error_handler;
static failure_callback_t *warning_handler = default_warning_handler;
static failure_callback_t *info_handler = default_info_handler;
static void (*failure_exit_callback)(int *) = NULL;

static FILE *log_fd = NULL, *log_info_fd = NULL;
static char *log_prefix = NULL, *log_stamp_format = NULL;

/* kludgy .. we want to trust log_stamp_format with -Wformat-nonliteral */
static const char *get_log_stamp_format(const char *unused)
	ATTR_FORMAT_ARG(1);

static const char *get_log_stamp_format(const char *unused ATTR_UNUSED)
{
	return log_stamp_format;
}

static void failure_exit(int status)
{
	if (failure_exit_callback != NULL)
		failure_exit_callback(&status);
	exit(status);
}

static void write_prefix(FILE *f)
{
	struct tm *tm;
	char str[256];
	time_t now;

	if (log_prefix != NULL)
		fputs(log_prefix, f);

	if (log_stamp_format != NULL) {
		now = time(NULL);
		tm = localtime(&now);

		if (strftime(str, sizeof(str),
			     get_log_stamp_format("unused"), tm) > 0)
			fputs(str, f);
	}
}

static int ATTR_FORMAT(3, 0)
default_handler(const char *prefix, FILE *f, const char *format, va_list args)
{
	static int recursed = 0;
	va_list args2;
	int old_errno = errno;

	if (recursed == 2) {
		/* we're being called from some signal handler, or
		   printf_format_fix_unsafe() killed us again */
		return -1;
	}

	recursed++;

	if (f == NULL) {
		f = stderr;

		if (log_fd == NULL)
			log_fd = stderr;
	}

	VA_COPY(args2, args);

	if (recursed == 2) {
		/* printf_format_fix_unsafe() probably killed us last time,
		   just write the format now. */

		fputs("recursed: ", f);
		fputs(format, f);
	} else {
		write_prefix(f);
		fputs(prefix, f);

		/* write may have failed, restore errno so %m works. although
		   it probably can't write the error then anyway. */
		errno = old_errno;

		/* make sure there's no %n in there and fix %m */
		vfprintf(f, printf_format_fix_unsafe(format), args2);
	}

	fputc('\n', f);

	errno = old_errno;
	recursed--;

	return 0;
}

static void ATTR_FORMAT(1, 0)
default_panic_handler(const char *format, va_list args)
{
	const char *backtrace;

	(void)default_handler("Panic: ", log_fd, format, args);
	if (backtrace_get(&backtrace) == 0)
		i_error("Raw backtrace: %s", backtrace);
	abort();
}

static void log_fd_flush_stop(struct ioloop *ioloop)
{
	io_loop_stop(ioloop);
}

static int log_fd_flush(FILE *fd)
{
	struct ioloop *ioloop;
	struct io *io;

	while (fflush(fd) < 0) {
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN)
			return -1;

		/* wait until we can write more. this can happen at least
		   when writing to terminal, even if fd is blocking. */
		ioloop = io_loop_create();
		io = io_add(IO_WRITE, fileno(log_fd),
			    log_fd_flush_stop, ioloop);
		io_loop_run(ioloop);
		io_remove(&io);
		io_loop_destroy(&ioloop);
	}
	return 0;
}

static void ATTR_FORMAT(2, 0)
default_fatal_handler(int status, const char *format, va_list args)
{
	if (default_handler("Fatal: ", log_fd, format, args) < 0 &&
	    status == FATAL_DEFAULT)
		status = FATAL_LOGERROR;

	if (log_fd_flush(log_fd) < 0 && status == FATAL_DEFAULT)
		status = FATAL_LOGWRITE;

	failure_exit(status);
}

static void ATTR_FORMAT(1, 0)
default_error_handler(const char *format, va_list args)
{
	if (default_handler("Error: ", log_fd, format, args) < 0)
		failure_exit(FATAL_LOGERROR);

	if (log_fd_flush(log_fd) < 0)
		failure_exit(FATAL_LOGWRITE);
}

static void ATTR_FORMAT(1, 0)
default_warning_handler(const char *format, va_list args)
{
	(void)default_handler("Warning: ", log_fd, format, args);

	if (log_fd_flush(log_fd) < 0)
		failure_exit(FATAL_LOGWRITE);
}

static void ATTR_FORMAT(1, 0)
default_info_handler(const char *format, va_list args)
{
	(void)default_handler("Info: ", log_info_fd, format, args);

	if (log_fd_flush(log_info_fd) < 0)
		failure_exit(FATAL_LOGWRITE);
}

void i_panic(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	panic_handler(format, args);
	va_end(args);
}

void i_fatal(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	fatal_handler(FATAL_DEFAULT, format, args);
	va_end(args);
}

void i_fatal_status(int status, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	fatal_handler(status, format, args);
	va_end(args);
}

void i_error(const char *format, ...)
{
	int old_errno = errno;
	va_list args;

	va_start(args, format);
	error_handler(format, args);
	va_end(args);

	errno = old_errno;
}

void i_warning(const char *format, ...)
{
	int old_errno = errno;
	va_list args;

	va_start(args, format);
	warning_handler(format, args);
	va_end(args);

	errno = old_errno;
}

void i_info(const char *format, ...)
{
	int old_errno = errno;
	va_list args;

	va_start(args, format);
	info_handler(format, args);
	va_end(args);

	errno = old_errno;
}

void i_set_panic_handler(failure_callback_t *callback ATTR_NORETURN)
{
	if (callback == NULL)
		callback = default_panic_handler;
        panic_handler = callback;
}

void i_set_fatal_handler(fatal_failure_callback_t *callback ATTR_NORETURN)
{
	if (callback == NULL)
		callback = default_fatal_handler;
        fatal_handler = callback;
}

void i_set_error_handler(failure_callback_t *callback)
{
	if (callback == NULL)
		callback = default_error_handler;
        error_handler = callback;
}

void i_set_warning_handler(failure_callback_t *callback)
{
	if (callback == NULL)
		callback = default_warning_handler;
        warning_handler = callback;
}

void i_set_info_handler(failure_callback_t *callback)
{
	if (callback == NULL)
		callback = default_info_handler;
        info_handler = callback;
}

static int ATTR_FORMAT(2, 0)
syslog_handler(int level, const char *format, va_list args)
{
	static int recursed = 0;

	if (recursed != 0)
		return -1;

	recursed++;

	/* make sure there's no %n in there. vsyslog() supports %m, but since
	   we'll convert it ourself anyway, we might as well it */
	vsyslog(level, printf_format_fix_unsafe(format), args);
	recursed--;

	return 0;
}

void i_syslog_panic_handler(const char *fmt, va_list args)
{
	const char *backtrace;

	(void)syslog_handler(LOG_CRIT, fmt, args);
	if (backtrace_get(&backtrace) == 0)
		i_error("Raw backtrace: %s", backtrace);
	abort();
}

void i_syslog_fatal_handler(int status, const char *fmt, va_list args)
{
	if (syslog_handler(LOG_CRIT, fmt, args) < 0 && status == FATAL_DEFAULT)
		status = FATAL_LOGERROR;
	failure_exit(status);
}

void i_syslog_error_handler(const char *fmt, va_list args)
{
	if (syslog_handler(LOG_ERR, fmt, args) < 0)
		failure_exit(FATAL_LOGERROR);
}

void i_syslog_warning_handler(const char *fmt, va_list args)
{
	(void)syslog_handler(LOG_WARNING, fmt, args);
}

void i_syslog_info_handler(const char *fmt, va_list args)
{
	(void)syslog_handler(LOG_INFO, fmt, args);
}

void i_set_failure_syslog(const char *ident, int options, int facility)
{
	openlog(ident, options, facility);

	i_set_panic_handler(i_syslog_panic_handler);
	i_set_fatal_handler(i_syslog_fatal_handler);
	i_set_error_handler(i_syslog_error_handler);
	i_set_warning_handler(i_syslog_warning_handler);
	i_set_info_handler(i_syslog_info_handler);
}

static void open_log_file(FILE **file, const char *path)
{
	if (*file != NULL && *file != stderr)
		(void)fclose(*file);

	if (path == NULL || strcmp(path, "/dev/stderr") == 0)
		*file = stderr;
	else {
		*file = fopen(path, "a");
		if (*file == NULL) {
			fprintf(stderr, "Can't open log file %s: %s\n",
				path, strerror(errno));
			failure_exit(FATAL_LOGOPEN);
		}
		fd_close_on_exec(fileno(*file), TRUE);
	}
}

void i_set_failure_file(const char *path, const char *prefix)
{
	i_set_failure_prefix(prefix);

	if (log_info_fd != NULL && log_info_fd != log_fd &&
	    log_info_fd != stderr)
		(void)fclose(log_info_fd);

	open_log_file(&log_fd, path);
	log_info_fd = log_fd;

	i_set_panic_handler(NULL);
	i_set_fatal_handler(NULL);
	i_set_error_handler(NULL);
	i_set_warning_handler(NULL);
}

void i_set_failure_prefix(const char *prefix)
{
	i_free(log_prefix);
	log_prefix = i_strdup(prefix);
}

static int ATTR_FORMAT(2, 0)
internal_handler(char log_type, const char *format, va_list args)
{
	string_t *str;
	int ret;

	t_push();
	str = t_str_new(512);
	str_append_c(str, 1);
	str_append_c(str, log_type);
	str_vprintfa(str, format, args);
	str_append_c(str, '\n');
	ret = write_full(2, str_data(str), str_len(str));
	t_pop();

	return ret;
}

static void ATTR_NORETURN ATTR_FORMAT(1, 0)
i_internal_panic_handler(const char *fmt, va_list args)
{
	const char *backtrace;

	(void)internal_handler('F', fmt, args);
	if (backtrace_get(&backtrace) == 0)
		i_error("Raw backtrace: %s", backtrace);
        abort();
}

static void ATTR_NORETURN ATTR_FORMAT(2, 0)
i_internal_fatal_handler(int status, const char *fmt, va_list args)
{
	if (internal_handler('F', fmt, args) < 0 && status == FATAL_DEFAULT)
		status = FATAL_LOGERROR;
	failure_exit(status);
}

static void ATTR_FORMAT(1, 0)
i_internal_error_handler(const char *fmt, va_list args)
{
	if (internal_handler('E', fmt, args) < 0)
		failure_exit(FATAL_LOGERROR);
}

static void ATTR_FORMAT(1, 0)
i_internal_warning_handler(const char *fmt, va_list args)
{
	(void)internal_handler('W', fmt, args);
}

static void ATTR_FORMAT(1, 0)
i_internal_info_handler(const char *fmt, va_list args)
{
	(void)internal_handler('I', fmt, args);
}

void i_set_failure_internal(void)
{
	i_set_panic_handler(i_internal_panic_handler);
	i_set_fatal_handler(i_internal_fatal_handler);
	i_set_error_handler(i_internal_error_handler);
	i_set_warning_handler(i_internal_warning_handler);
	i_set_info_handler(i_internal_info_handler);
}

void i_set_info_file(const char *path)
{
	if (log_info_fd == log_fd)
		log_info_fd = NULL;

	open_log_file(&log_info_fd, path);
        info_handler = default_info_handler;
}

void i_set_failure_timestamp_format(const char *fmt)
{
	i_free(log_stamp_format);
        log_stamp_format = i_strdup(fmt);
}

void i_set_failure_exit_callback(void (*callback)(int *status))
{
	failure_exit_callback = callback;
}

void failures_deinit(void)
{
	if (log_info_fd == log_fd)
		log_info_fd = NULL;

	if (log_fd != NULL && log_fd != stderr) {
		(void)fclose(log_fd);
		log_fd = stderr;
	}

	if (log_info_fd != NULL && log_info_fd != stderr) {
		(void)fclose(log_info_fd);
		log_info_fd = stderr;
	}

	i_free_and_null(log_prefix);
	i_free_and_null(log_stamp_format);
}
