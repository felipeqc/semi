#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>

#include "litmus.h"
#include "common.h"

typedef struct {
	int  wait;
	char * exec_path;
	char ** argv;
} startup_info_t;


int launch(void *task_info_p) {
	startup_info_t *info = (startup_info_t*) task_info_p;
	int ret;
	if (info->wait) {
		ret = wait_for_ts_release();
		if (ret != 0)
			perror("wait_for_ts_release()");
	}
	ret = execvp(info->exec_path, info->argv);
	perror("execv failed");
	return ret;
}

void usage(char *error) {
	fprintf(stderr, "%s\nUsage: rt_launch [-w] task_parameters_file "
			"program [arg1 arg2 ...]\n"
			"\t-w\tSynchronous release\n"
			"\tprogram to be launched\n",
			error);
	exit(1);
}

#define OPTSTR "w"

int main(int argc, char** argv)
{
	int ret;
	int wait = 0;
	int opt;
	startup_info_t info;

	struct rt_task rt;
	FILE *file;

	while ((opt = getopt(argc, argv, OPTSTR)) != -1) {
		switch (opt) {
		case 'w':
			wait = 1;
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

	signal(SIGUSR1, SIG_IGN);

	if (argc - optind < 2)
		usage("Arguments missing.");

	if ((file = fopen(argv[optind + 0], "r")) == NULL) {
		fprintf(stderr, "Cannot open %s\n", argv[1]);
		return -1;
	}

	memset(&rt, 0, sizeof(struct rt_task));

	if (parse_edfwm_ts_file(file, &rt) < 0)
		bail_out("Could not parse file\n");

	if (sporadic_task_ns_semi(&rt) < 0)
		bail_out("could not setup rt task params");

	fclose(file);

	info.exec_path = argv[optind + 1];
	info.argv      = argv + optind + 1;
	info.wait      = wait;

	ret = create_rt_task_semi(launch, &info, &rt);

	if (ret < 0)
		bail_out("could not create rt child process");

	return 0;
}

