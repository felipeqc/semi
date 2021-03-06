#include <sys/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include "litmus.h"
#include "common.h"

void usage(char *error) {
	fprintf(stderr, "Error: %s\n", error);
	fprintf(stderr,
		"Usage: rt_spin [-w] [-p PARTITION] [-c CLASS] WCET PERIOD DURATION fracnum1 fracden1 cpu1 fracnum2 fracden2 cpu2\n"
		"       rt_spin -l\n");
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

#define OPTSTR "p:c:wld:v"

int main(int argc, char** argv)
{
	int ret;
	lt_t wcet;
	lt_t period;
	/* [num,den] */
	lt_t frac1[2], frac2[2];
	int cpu1, cpu2;
	double wcet_ms, period_ms;
	int migrate = 0;
	int cpu = 0;
	int opt;
	int wait = 0;
	int test_loop = 0;
	int skip_config = 0;
	int verbose = 0;
	double duration, start;
	task_class_t class = RT_CLASS_HARD;

	progname = argv[0];

	while ((opt = getopt(argc, argv, OPTSTR)) != -1) {
		switch (opt) {
		case 'w':
			wait = 1;
			break;
		case 'p':
			cpu = atoi(optarg);
			migrate = 1;
			break;
		case 'c':
			class = str2class(optarg);
			if (class == -1)
				usage("Unknown task class.");
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

	if (argc - optind < 9)
		usage("Arguments missing.");
	wcet_ms   = atof(argv[optind + 0]);
	period_ms = atof(argv[optind + 1]);
	duration  = atof(argv[optind + 2]);
	/* frac num, den = 0 means fixed task */
	frac1[0]  = atoi(argv[optind + 3]);
	frac1[1]  = atoi(argv[optind + 4]);
	cpu1 	  = atoi(argv[optind + 5]);
	frac2[0]  = atoi(argv[optind + 6]);
	frac2[1]  = atoi(argv[optind + 7]);
	cpu2	  = atoi(argv[optind + 8]);
	wcet   = wcet_ms * __NS_PER_MS;
	period = period_ms * __NS_PER_MS;
	if (wcet <= 0)
		usage("The worst-case execution time must be a "
		      "positive number.");
	if (period <= 0)
		usage("The period must be a positive number.");
	if (wcet > period) {
		usage("The worst-case execution time must not "
		      "exceed the period.");
	}

	if (migrate) {
		ret = be_migrate_to(cpu);
		if (ret < 0)
			bail_out("could not migrate to target partition");
	}

	ret = sporadic_task_ns_edffm(wcet, period, 0, cpu,
			frac1, frac2, cpu1, cpu2,
			class, NO_ENFORCEMENT, migrate);

	if (ret < 0)
		bail_out("could not setup rt task params");

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

	start = wctime();

	while (start + duration > wctime()) {
		job(wcet_ms * 0.0009); /* 90% wcet, in seconds */
	}

	return 0;
}
