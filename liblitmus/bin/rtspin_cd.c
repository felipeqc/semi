#include <sys/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

#include "litmus.h"
#include "common.h"

void usage(char *error) {
	fprintf(stderr, "Error: %s\n", error);
	fprintf(stderr,
		"Usage: rtspin_cd [-w] task_parameters_file duration\n"
		"       rtspin_cd -l\n");
	exit(1);
}

#define NUMS 4096
static int num[NUMS];
static double loop_length = 1.0;
static char* progname;

static int loop_once(void)
{
	int i, j = 0;
	for (i = 0; i < NUMS; i++)
		j += num[i]++;
	return j;
}

static int loop_for(double exec_time)
{
	double t = 0;
	int tmp = 0;
/*	while (t + loop_length < exec_time) {
		tmp += loop_once();
		t += loop_length;
	}
*/
	double start = cputime();
	double now = cputime();
	while (now + loop_length < start + exec_time) {
		tmp += loop_once();
		t += loop_length;
		now = cputime();
	}

	return tmp;
}

static void fine_tune(double interval)
{
	double start, end, delta;

	start = wctime();
	loop_for(interval);
	end = wctime();
	delta = (end - start - interval) / interval;
	if (delta != 0)
		loop_length = loop_length / (1 - delta);
}

static void configure_loop(void)
{
	double start;

	/* prime cache */
	loop_once();
	loop_once();
	loop_once();

	/* measure */
	start = wctime();
	loop_once(); /* hope we didn't get preempted  */
	loop_length = wctime();
	loop_length -= start;

	/* fine tune */
	fine_tune(0.1);
	fine_tune(0.1);
	fine_tune(0.1);
}

static void show_loop_length(void)
{
	printf("%s/%d: loop_length=%f (%ldus)\n",
	       progname, getpid(), loop_length,
	       (long) (loop_length * 1000000));
}

static void debug_delay_loop(void)
{
	double start, end, delay;
	show_loop_length();
	while (1) {
		for (delay = 0.5; delay > 0.01; delay -= 0.01) {
			start = wctime();
			loop_for(delay);
			end = wctime();
			printf("%6.4fs: looped for %10.8fs, delta=%11.8fs, error=%7.4f%%\n",
			       delay,
			       end - start,
			       end - start - delay,
			       100 * (end - start - delay) / delay);
		}
	}
}

static int job(double exec_time)
{
	loop_for(exec_time);
	sleep_next_period();
	return 0;
}

#define OPTSTR "wld:v"

int main(int argc, char** argv)
{
	int ret;

	int opt;
	int wait = 0;
	int test_loop = 0;
	int skip_config = 0;
	int verbose = 0;
	double wcet_ms;
	double duration, start;

	struct rt_task rt;
	FILE *file;

	progname = argv[0];

	while ((opt = getopt(argc, argv, OPTSTR)) != -1) {
		switch (opt) {
		case 'w':
			wait = 1;
			break;
		case 'l':
			test_loop = 1;
			break;
		case 'd':
			/* manually configure delay per loop iteration
			 * unit: microseconds */
			loop_length = atof(optarg) / 1000000;
			skip_config = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case ':':
			usage("Argument missing.");
			break;
		case '?':
		default:
			usage("Bad argument.");
			break;
		}
	}


	if (!skip_config)
		configure_loop();

	if (test_loop) {
		debug_delay_loop();
		return 0;
	}

	if (argc - optind < 2)
		usage("Arguments missing.");

	if ((file = fopen(argv[optind + 0], "r")) == NULL) {
		fprintf(stderr, "Cannot open %s\n", argv[1]);
		return -1;
	}
	duration = atof(argv[optind + 1]);

	memset(&rt, 0, sizeof(struct rt_task));

	if (parse_cd_ts_file(file, &rt) < 0)
		bail_out("Could not parse file\n");

	if (sporadic_task_ns_semi(&rt) < 0)
		bail_out("could not setup rt task params");

	fclose(file);

	if (verbose)
		show_loop_length();

	init_litmus();

	ret = task_mode(LITMUS_RT_TASK);
	if (ret != 0)
		bail_out("could not become RT task");

	if (wait) {
		ret = wait_for_ts_release();
		if (ret != 0)
			bail_out("wait_for_ts_release()");
	}

	wcet_ms = ((double) rt.exec_cost ) / __NS_PER_MS;
	start = wctime();

	while (start + duration > wctime()) {
		job(wcet_ms * 0.0009); /* 90% wcet, in seconds */
	}

	return 0;
}
