#ifndef COMMON_H
#define COMMON_H

#include "litmus.h"

void bail_out(const char* msg);

/* EDF-WM helper functions to parse task parameters from file */
int parse_edfwm_ts_file(FILE *ts, struct rt_task *rt);

#endif
