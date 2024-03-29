/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright(c) 2022 P Quentin Armitage <quentin@armitage.org.uk>
 */

/*
** tfo_printf.h for tcp flow optimizer
**
** Author: P Quentin Armitage <quentin@armitage.org.uk>
**
*/


#ifndef TFO_PRINTF_H_
#define TFO_PRINTF_H_

#include "tfo_config.h"

#include <stdbool.h>
#include <stddef.h>


#ifdef PER_THREAD_LOGS
extern void open_thread_log(const char *);
#endif
extern int tfo_printf(const char *, ...) __attribute__((format (printf, 1, 2)));
extern int tfo_fprintf(FILE *fp, const char *, ...) __attribute__((format (printf, 2, 3)));
extern int tfo_fflush(FILE *);
#ifdef DEBUG_PRINT_TO_BUF
extern void tfo_printf_init(size_t, bool);
extern void tfo_fflush_buf(const char *);
#endif


/* The definitions needs to follow the declarations above */
#define printf(...)		tfo_printf(__VA_ARGS__)
#define fprintf(_FP, ...)	tfo_fprintf(_FP, ##__VA_ARGS__)
#define fflush(_FP)		tfo_fflush(_FP)

#define PRINTF_DEFINED


#endif
