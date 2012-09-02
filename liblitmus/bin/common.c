#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "common.h"

void bail_out(const char* msg)
{
	perror(msg);
	exit(-1 * errno);
}

/* EDF-WM helper functions to parse a custom text file format to "easily"
 * launch tests with rtspin and rt_launch:
 *
 * Format for task:
 *
 * <task_id execution_cost period phase cpu slices_number> .
 *
 * If the task is split on multiple slices, slices_number is non 0
 * and we scan a list of slice parameters up to slices_number:
 *
 * Format for slices:
 *
 * <task_id cpu deadline(from job release) budget offset> .
 *
 * The offset is the start time for the slice relative to the job release.
 *
 * Example:
 * 14 2.26245771754 10 0 5 2
 * 14 5 5.000000 1.497306 0.000000
 * 14 7 10.000000 0.765152 5.000000
 */

#define fms_to_ns(x)	(lt_t)(((x) * __NS_PER_MS))
/*
 * <task_id, cpu, deadline (from job release), budget, offset> .
 */
int parse_edfwm_slice(FILE *ts, int slices_no, int task_id,
		      struct rt_task *rt)
{
	int i, tid;
	unsigned int cpu;
	double deadline, budget, offset;

	lt_t total_budget = 0;

	struct edf_wm_params* wm = (struct edf_wm_params*) &rt->semi_part;

	for (i = 0; i < slices_no; i++) {

		if (fscanf(ts, "%d %u %lf %lf %lf\n", &tid, &cpu,
				&deadline, &budget, &offset) != EOF) {

			if (task_id != tid) {
				fprintf(stderr, "task_id %d != tid %d\n",
						task_id, tid);
				return -1;
			}

			wm->slices[i].deadline = fms_to_ns(deadline);
			wm->slices[i].budget = fms_to_ns(budget);
			wm->slices[i].offset = fms_to_ns(offset);
			wm->slices[i].cpu    = cpu;

			printf("slice(tid, cpu, d, e, ph) = (%d, %u, %llu, %llu, %llu)\n",
					tid, cpu, wm->slices[i].deadline,
					wm->slices[i].budget, wm->slices[i].offset);

			total_budget += wm->slices[i].budget;
			if (wm->slices[i].budget < MIN_EDF_WM_SLICE_SIZE) {

				fprintf(stderr, "Slice %llu is too small\n",
						wm->slices[i].budget);
				return -1;
			}
		}

		if (ferror(ts)) {
			fprintf(stderr, "Cannot read file\n");
			return -1;
		}
	}
	wm->count = slices_no;
	rt->exec_cost = total_budget;
	printf("--- total %u slices ---\n", wm->count);
	return 0;
}

/*
 * <task_id, execution_cost, period, phase, cpu, slices_number> .
 */
int parse_edfwm_ts_file(FILE *ts, struct rt_task *rt)
{
	int task_id, ret = 1;
	unsigned int cpu, sliceno;
	double fwcet, fperiod, fphase;

	ret = fscanf(ts, "%d %lf %lf %lf %d %d\n",
			&task_id, &fwcet, &fperiod, &fphase, &cpu, &sliceno);

	if (ferror(ts))
		goto err;

	rt->exec_cost = fms_to_ns(fwcet);
	rt->period = fms_to_ns(fperiod);
	rt->phase = fms_to_ns(fphase);
	rt->cpu = cpu;
	rt->cls = RT_CLASS_HARD;
	rt->budget_policy = PRECISE_ENFORCEMENT;

	printf("(tid, wcet, period, ph, cpu, slices) = "
			"(%d, %llu, %llu, %llu, %u, %u)\n",
			task_id, rt->exec_cost, rt->period, rt->phase, cpu, sliceno);
	if (sliceno > 0) {
		memset(&rt->semi_part, 0, sizeof(struct edf_wm_params));
		ret = parse_edfwm_slice(ts, sliceno, task_id, rt);
		if (ret < 0)
			goto err;
	}

	return 0;

err:
	fprintf(stderr, "Error parsing file\n");
	return -1;
}

/*
 * <task_id, cpu, budget> .
 */
int parse_hime_slice(FILE *ts, int slices_no, int task_id,
		      struct rt_task *rt)
{
	int i, tid;
	unsigned int cpu;
	double budget;

	lt_t total_budget = 0;

	struct hime_params* hime = (struct hime_params*) &rt->semi_part;

	for (i = 0; i < slices_no; i++) {

		if (fscanf(ts, "%d %u %lf\n", &tid, &cpu, &budget) != EOF) {

			if (task_id != tid) {
				fprintf(stderr, "task_id %d != tid %d\n",
						task_id, tid);
				return -1;
			}

			hime->slices[i].budget = fms_to_ns(budget);
			hime->slices[i].cpu    = cpu;

			total_budget += hime->slices[i].budget;
			if (hime->slices[i].budget < MIN_HIME_SLICE_SIZE) {

				fprintf(stderr, "Slice %llu is too small\n",
						hime->slices[i].budget);
				return -1;
			}
		}

		if (ferror(ts)) {
			fprintf(stderr, "Cannot read file\n");
			return -1;
		}
	}

	for(i = 0; i < slices_no; i++) {
		printf("slice(tid, cpu, budget) = (%d, %u, %llu)\n",
					tid, hime->slices[i].cpu, hime->slices[i].budget);
	}

	hime->count = slices_no;
	rt->exec_cost = total_budget;
	printf("--- total %u slices ---\n", hime->count);
	return 0;
}

/*
 * <task_id, execution_cost, period, phase, cpu, slices_number> .
 */
int parse_hime_ts_file(FILE *ts, struct rt_task *rt)
{
	int task_id, ret = 1;
	unsigned int cpu, sliceno;
	double fwcet, fperiod, fphase;

	ret = fscanf(ts, "%d %lf %lf %lf %d %d\n",
			&task_id, &fwcet, &fperiod, &fphase, &cpu, &sliceno);

	if (ferror(ts))
		goto err;

	rt->exec_cost = fms_to_ns(fwcet);
	rt->period = fms_to_ns(fperiod);
	rt->phase = fms_to_ns(fphase);
	rt->cpu = cpu;
	rt->cls = RT_CLASS_HARD;
	rt->budget_policy = PRECISE_ENFORCEMENT;

	printf("(tid, wcet, period, ph, cpu, slices) = "
			"(%d, %llu, %llu, %llu, %u, %u)\n",
			task_id, rt->exec_cost, rt->period, rt->phase, cpu, sliceno);
	if (sliceno > 0) {
		memset(&rt->semi_part, 0, sizeof(struct edf_wm_params));
		ret = parse_hime_slice(ts, sliceno, task_id, rt);
		if (ret < 0)
			goto err;
	}

	return 0;

err:
	fprintf(stderr, "Error parsing file\n");
	return -1;
}


/*
 * <task_id, cpu, budget, deadline (from job release)> .
 */
int parse_cd_slice(FILE *ts, int slices_no, int task_id,
		      struct rt_task *rt)
{
	int i, tid;
	unsigned int cpu;
	double budget;
	double deadline;

	lt_t total_budget = 0;

	struct cd_params* cd = (struct cd_params*) &rt->semi_part;

	for (i = 0; i < slices_no; i++) {

		if (fscanf(ts, "%d %u %lf %lf\n", &tid, &cpu, &budget, &deadline) != EOF) {

			if (task_id != tid) {
				fprintf(stderr, "task_id %d != tid %d\n",
						task_id, tid);
				return -1;
			}

			cd->slices[i].budget = fms_to_ns(budget);
			cd->slices[i].cpu    = cpu;
			cd->slices[i].deadline = fms_to_ns(deadline);

			total_budget += cd->slices[i].budget;
			if (cd->slices[i].budget < MIN_CD_SLICE_SIZE) {

				fprintf(stderr, "Slice %llu is too small\n",
						cd->slices[i].budget);
				return -1;
			}
		}

		if (ferror(ts)) {
			fprintf(stderr, "Cannot read file\n");
			return -1;
		}
	}

	for(i = 0; i < slices_no; i++) {
		printf("slice(tid, cpu, budget, deadline) = (%d, %u, %llu, %llu)\n",
					tid, cd->slices[i].cpu, cd->slices[i].budget, cd->slices[i].deadline);
	}

	cd->count = slices_no;
	rt->exec_cost = total_budget;
	printf("--- total %u slices ---\n", cd->count);
	return 0;
}

/*
 * <task_id, execution_cost, period, phase, cpu, slices_number> .
 */
int parse_cd_ts_file(FILE *ts, struct rt_task *rt)
{
	int task_id, ret = 1;
	unsigned int cpu, sliceno;
	double fwcet, fperiod, fphase;

	ret = fscanf(ts, "%d %lf %lf %lf %d %d\n",
			&task_id, &fwcet, &fperiod, &fphase, &cpu, &sliceno);

	if (ferror(ts))
		goto err;

	rt->exec_cost = fms_to_ns(fwcet);
	rt->period = fms_to_ns(fperiod);
	rt->phase = fms_to_ns(fphase);
	rt->cpu = cpu;
	rt->cls = RT_CLASS_HARD;
	rt->budget_policy = PRECISE_ENFORCEMENT;

	printf("(tid, wcet, period, ph, cpu, slices) = "
			"(%d, %llu, %llu, %llu, %u, %u)\n",
			task_id, rt->exec_cost, rt->period, rt->phase, cpu, sliceno);
	if (sliceno > 0) {
		memset(&rt->semi_part, 0, sizeof(struct edf_wm_params));
		ret = parse_cd_slice(ts, sliceno, task_id, rt);
		if (ret < 0)
			goto err;
	}

	return 0;

err:
	fprintf(stderr, "Error parsing file\n");
	return -1;
}



