/* Copyright (C) 2007 Board of Trustees, Leland Stanford Jr. University.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "util.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *program_name;

static void
out_of_memory(void) 
{
    fatal(0, "virtual memory exhausted");
}

void *
xcalloc(size_t count, size_t size) 
{
    void *p = count && size ? calloc(count, size) : malloc(1);
    if (p == NULL) {
        out_of_memory();
    }
    return p;
}

void *
xmalloc(size_t size) 
{
    void *p = malloc(size ? size : 1);
    if (p == NULL) {
        out_of_memory();
    }
    return p;
}

void *
xrealloc(void *p, size_t size) 
{
    p = realloc(p, size ? size : 1);
    if (p == NULL) {
        out_of_memory();
    }
    return p;
}

char *
xstrdup(const char *s_) 
{
    size_t size = strlen(s_) + 1;
    char *s = xmalloc(size);
    memcpy(s, s_, size);
    return s;
}

char *
xasprintf(const char *format, ...)
{
    va_list args;
    size_t needed;
    char *s;

    va_start(args, format);
    needed = vsnprintf(NULL, 0, format, args);
    va_end(args);

    s = xmalloc(needed + 1);

    va_start(args, format);
    vsnprintf(s, needed + 1, format, args);
    va_end(args);

    return s;
}

void fatal(int err_no, const char *format, ...)
{
    va_list args;

    fprintf(stderr, "%s: ", program_name);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    if (err_no != 0)
        fprintf(stderr, " (%s)", strerror(err_no));
    putc('\n', stderr);

    exit(EXIT_FAILURE);
}

void error(int err_no, const char *format, ...)
{
    va_list args;

    fprintf(stderr, "%s: ", program_name);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    if (err_no != 0)
        fprintf(stderr, " (%s)", strerror(err_no));
    putc('\n', stderr);
}

void debug(int err_no, const char *format, ...)
{
    va_list args;

    fprintf(stderr, "%s: ", program_name);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    if (err_no != 0)
        fprintf(stderr, " (%s)", strerror(err_no));
    putc('\n', stderr);
}

/* Sets program_name based on 'argv0'.  Should be called at the beginning of
 * main(), as "set_program_name(argv[0]);".  */
void set_program_name(const char *argv0)
{
    const char *slash = strrchr(argv0, '/');
    program_name = slash ? slash + 1 : argv0;
}

/* Writes the 'size' bytes in 'buf' to 'stream' as hex bytes arranged 16 per
 * line.  Numeric offsets are also included, starting at 'ofs' for the first
 * byte in 'buf'.  If 'ascii' is true then the corresponding ASCII characters
 * are also rendered alongside. */
void
hex_dump(FILE *stream, const void *buf_, size_t size,
         uintptr_t ofs, bool ascii)
{
  const uint8_t *buf = buf_;
  const size_t per_line = 16; /* Maximum bytes per line. */

  while (size > 0)
    {
      size_t start, end, n;
      size_t i;

      /* Number of bytes on this line. */
      start = ofs % per_line;
      end = per_line;
      if (end - start > size)
        end = start + size;
      n = end - start;

      /* Print line. */
      fprintf(stream, "%08jx  ", (uintmax_t) ROUND_DOWN(ofs, per_line));
      for (i = 0; i < start; i++)
        fprintf(stream, "   ");
      for (; i < end; i++)
        fprintf(stream, "%02hhx%c",
                buf[i - start], i == per_line / 2 - 1? '-' : ' ');
      if (ascii)
        {
          for (; i < per_line; i++)
            fprintf(stream, "   ");
          fprintf(stream, "|");
          for (i = 0; i < start; i++)
            fprintf(stream, " ");
          for (; i < end; i++) {
              int c = buf[i - start];
              putc(c >= 32 && c < 127 ? c : '.', stream);
          }
          for (; i < per_line; i++)
            fprintf(stream, " ");
          fprintf(stream, "|");
        }
      fprintf(stream, "\n");

      ofs += n;
      buf += n;
      size -= n;
    }
}