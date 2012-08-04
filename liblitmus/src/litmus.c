#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>

#include <sched.h> /* for cpu sets */

#include "litmus.h"
#include "internal.h"

void show_rt_param(struct rt_task* tp)
{
	printf("rt params:\n\t"
	       "exec_cost:\t%llu\n\tperiod:\t\t%llu\n\tcpu:\t%d\n",
	       tp->exec_cost, tp->period, tp->cpu);
}

task_class_t str2class(const char* str)
{
	if      (!strcmp(str, "hrt"))
		return RT_CLASS_HARD;
	else if (!strcmp(str, "srt"))
		return RT_CLASS_SOFT;
	else if (!strcmp(str, "be"))
		return RT_CLASS_BEST_EFFORT;
	else
		return -1;
}

/* only for best-effort execution: migrate to target_cpu */
int be_migrate_to(int target_cpu)
{
	cpu_set_t cpu_set;

	CPU_ZERO(&cpu_set);
	CPU_SET(target_cpu, &cpu_set);
	return sched_setaffinity(0, sizeof(cpu_set_t), &cpu_set);
}

int sporadic_task(lt_t e, lt_t p, lt_t phase,
		  int cpu, task_class_t cls,
		  budget_policy_t budget_policy, int set_cpu_set)
{
	return sporadic_task_ns(e * __NS_PER_MS, p * __NS_PER_MS, phase * __NS_PER_MS,
				cpu, cls, budget_policy, set_cpu_set);
}

int sporadic_task_ns(lt_t e, lt_t p, lt_t phase,
			int cpu, task_class_t cls,
			budget_policy_t budget_policy, int set_cpu_set)
{
	struct rt_task param;
	int ret;

	/* Zero out first --- this is helpful when we add plugin-specific
	 * parameters during development.
	 */
	memset(&param, 0, sizeof(param));

	param.exec_cost = e;
	param.period    = p;
	param.cpu       = cpu;
	param.cls       = cls;
	param.phase	= phase;
	param.budget_policy = budget_policy;

	if (set_cpu_set) {
		ret = be_migrate_to(cpu);
		check("migrate to cpu");
	}
	return set_rt_task_param(gettid(), &param);
}

int sporadic_task_ns_edffm(lt_t e, lt_t p, lt_t phase, int cpu,
		lt_t *frac1, lt_t *frac2, int cpu1, int cpu2,
		task_class_t cls, budget_policy_t budget_policy,
		int set_cpu_set)
{
	struct rt_task param;
	struct edffm_params fm;
	int ret;
	param.exec_cost = e;
	param.period    = p;
	param.cpu       = cpu;
	/* check on denominators */
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
	param.semi_part.fm = fm;
	param.cls       = cls;
	param.phase	= phase;
	param.budget_policy = budget_policy;

	if (set_cpu_set) {
		ret = be_migrate_to(cpu);
		check("migrate to cpu");
	}
	return set_rt_task_param(gettid(), &param);
}

int sporadic_task_ns_npsf(lt_t e, lt_t p, lt_t phase,
		int cpu, task_class_t cls, int npsf_id,
		budget_policy_t budget_policy, int set_cpu_set)
{
	struct rt_task param;
	int ret;
	param.exec_cost = e;
	param.period    = p;
	param.cpu       = cpu;
	param.cls       = cls;
	param.phase	= phase;
	param.budget_policy = budget_policy;
	param.semi_part.npsf_id = (int) npsf_id;

	if (set_cpu_set) {
		ret = be_migrate_to(cpu);
		check("migrate to cpu");
	}
	return set_rt_task_param(gettid(), &param);
}

/* Sporadic task helper function for Semi-Partitioned algorithms. */
int sporadic_task_ns_semi(struct rt_task *param)
{
	int ret;

	ret = be_migrate_to(param->cpu);
	check("migrate to cpu");

	return set_rt_task_param(gettid(), param);
}

int init_kernel_iface(void);

int init_litmus(void)
{
	int ret, ret2;

	ret = mlockall(MCL_CURRENT | MCL_FUTURE);
	check("mlockall()");
	ret2 = init_rt_thread();
	return (ret == 0) && (ret2 == 0) ? 0 : -1;
}

int init_rt_thread(void)
{
	int ret;

        ret = init_kernel_iface();
	check("kernel <-> user space interface initialization");
	return ret;
}

void exit_litmus(void)
{
	/* nothing to do in current version */
}
