#ifndef COMMON_H
#define COMMON_H

#include "litmus.h"

void bail_out(const char* msg);

/* Helper functions to parse task parameters from file */
int parse_edfwm_ts_file(FILE *ts, struct rt_task *rt);
int parse_hime_ts_file(FILE *ts, struct rt_task *rt);
int parse_cd_ts_file(FILE *ts, struct rt_task *rt);

#endif
