#ifndef LOG_H
#define LOG_H

#include <syslog.h>
#include <cstdarg>

void set_loglevel( int log_level = LOG_DEBUG );
void log( int log_level, const char* file_name, int line_num, const char* format, ... );

#endif
