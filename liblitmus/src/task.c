#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <sched.h>

#include "litmus.h"
#include "internal.h"

static void tperrorx(char* msg)
{
	fprintf(stderr,
		"Task %d: %s: %m",
		gettid(), msg);
	exit(-1);
}

/* common launch routine */
int __launch_rt_task(rt_fn_t rt_prog, void *rt_arg, rt_setup_fn_t setup,
		     void* setup_arg)
{
	int ret;
	int rt_task = fork();

	if (rt_task == 0) {
		/* we are the real-time task
		 * launch task and die when it is done
		 */
		rt_task = gettid();
		ret = setup(rt_task, setup_arg);
		if (ret < 0)
			tperrorx("could not setup task parameters");
		ret = task_mode(LITMUS_RT_TASK);
		if (ret < 0)
			tperrorx("could not become real-time task");
		exit(rt_prog(rt_arg));
	}

	return rt_task;
}

int __create_rt_task_edffm(rt_fn_t rt_prog, void *arg, int cpu, int wcet,
			int period, lt_t *frac1, lt_t *frac2,
			int cpu1, int cpu2, task_class_t class)
{
	struct rt_task params;
	struct edffm_params fm;
	params.cpu       = cpu;
	params.period    = period;
	params.exec_cost = wcet;
	params.cls       = class;
	params.phase     = 0;
	/* enforce budget for tasks that might not use sleep_next_period() */
	params.budget_policy = QUANTUM_ENFORCEMENT;

	/* edf-fm check on denominators for migratory tasks */
	if (frac1[1] != 0 && frac2[1] != 0) {
		/* edf-fm migrat task */
		fm.nr_cpus = 1;
		fm.cpus[0] = cpu1;
		fm.cpus[1] = cpu2;
		fm.fraction[0][0] = frac1[0];
		fm.fraction[1][0] = frac1[1];
		fm.fraction[0][1] = frac2[0];
		fm.fraction[1][1] = frac2[1];
	}
	params.semi_part.fm = fm;

	return __launch_rt_task(rt_prog, arg,
				(rt_setup_fn_t) set_rt_task_param, &params);
}

int __create_rt_task_npsf(rt_fn_t rt_prog, void *arg, int cpu, int wcet,
		int period, int npsf_id, task_class_t class)
{
	struct rt_task params;
	params.cpu       = cpu;
	params.period    = period;
	params.exec_cost = wcet;
	params.cls       = class;
	params.phase     = 0;
	/* enforce budget for tasks that might not use sleep_next_period() */
	params.budget_policy = QUANTUM_ENFORCEMENT;
	params.semi_part.npsf_id = (int) npsf_id;

	return __launch_rt_task(rt_prog, arg,
				(rt_setup_fn_t) set_rt_task_param, &params);
}

int __create_rt_task(rt_fn_t rt_prog, void *arg, int cpu, int wcet, int period,
		     task_class_t class)
{
	struct rt_task params;
	params.cpu       = cpu;
	params.period    = period;
	params.exec_cost = wcet;
	params.cls       = class;
	params.phase     = 0;
	/* enforce budget for tasks that might not use sleep_next_period() */
	params.budget_policy = QUANTUM_ENFORCEMENT;

	return __launch_rt_task(rt_prog, arg,
				(rt_setup_fn_t) set_rt_task_param, &params);
}

int create_rt_task(rt_fn_t rt_prog, void *arg, int cpu, int wcet, int period) {
	return __create_rt_task(rt_prog, arg, cpu, wcet, period, RT_CLASS_HARD);
}

int create_rt_task_semi(rt_fn_t rt_prog, void *arg, struct rt_task *params)
{
	return __launch_rt_task(rt_prog, arg,
			(rt_setup_fn_t) set_rt_task_param, params);
}

#define SCHED_NORMAL 0
#define SCHED_LITMUS 6

int task_mode(int mode)
{
	struct sched_param param;
	int me     = gettid();
	int policy = sched_getscheduler(gettid());
	int old_mode = policy == SCHED_LITMUS ? LITMUS_RT_TASK : BACKGROUND_TASK;

	param.sched_priority = 0;
	if (old_mode == LITMUS_RT_TASK && mode == BACKGROUND_TASK) {
		/* transition to normal task */
		return sched_setscheduler(me, SCHED_NORMAL, &param);
	} else if (old_mode == BACKGROUND_TASK && mode == LITMUS_RT_TASK) {
		/* transition to RT task */
		return sched_setscheduler(me, SCHED_LITMUS, &param);
	} else {
		errno = -EINVAL;
		return -1;
	}
}
