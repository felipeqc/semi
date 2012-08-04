#ifndef LITMUS_H
#define LITMUS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Include kernel header.
 * This is required for the rt_param
 * and control_page structures.
 */
#include <linux/threads.h>
#include <litmus/rt_param.h>

#include <sys/types.h>

#include "cycles.h" /* for null_call() */

typedef int pid_t;	 /* PID of a task */

/* obtain the PID of a thread */
pid_t gettid(void);

/* migrate to partition */
int be_migrate_to(int target_cpu);

int set_rt_task_param(pid_t pid, struct rt_task* param);
int get_rt_task_param(pid_t pid, struct rt_task* param);

/* setup helper */

/* times are given in ms */
int sporadic_task(
		lt_t e, lt_t p, lt_t phase,
		int partition, task_class_t cls,
		budget_policy_t budget_policy, int set_cpu_set);

/* times are given in ns */
int sporadic_task_ns(
		lt_t e, lt_t p, lt_t phase,
		int cpu, task_class_t cls,
		budget_policy_t budget_policy, int set_cpu_set);

int sporadic_task_ns_edffm(lt_t e, lt_t p, lt_t phase, int cpu,
		lt_t *frac1, lt_t *frac2, int cpu1, int cpu2,
		task_class_t cls, budget_policy_t budget_policy,
		int set_cpu_set);

int sporadic_task_ns_npsf(
		lt_t e, lt_t p, lt_t phase,
		int cpu, task_class_t cls, int npsf_id,
		budget_policy_t budget_policy, int set_cpu_set);

/* times are in ns, specific helper for semi-partitioned algos */
int sporadic_task_ns_semi(struct rt_task *rt);

/* budget enforcement off by default in these macros */
#define sporadic_global(e, p) \
	sporadic_task(e, p, 0, 0, RT_CLASS_SOFT, NO_ENFORCEMENT, 0)
#define sporadic_partitioned(e, p, cpu) \
	sporadic_task(e, p, 0, cpu, RT_CLASS_SOFT, NO_ENFORCEMENT, 1)

/* file descriptor attached shared objects support */
typedef enum  {
	FMLP_SEM	= 0,
	SRP_SEM		= 1,
} obj_type_t;

int od_openx(int fd, obj_type_t type, int obj_id, void* config);
int od_close(int od);

static inline int od_open(int fd, obj_type_t type, int obj_id)
{
	return od_openx(fd, type, obj_id, 0);
}

/* FMLP binary semaphore support */
int fmlp_down(int od);
int fmlp_up(int od);

/* SRP binary semaphore support */
int srp_down(int od);
int srp_up(int od);

/* job control*/
int get_job_no(unsigned int* job_no);
int wait_for_job_release(unsigned int job_no);
int sleep_next_period(void);

/*  library functions */
int  init_litmus(void);
int  init_rt_thread(void);
void exit_litmus(void);

/* A real-time program. */
typedef int (*rt_fn_t)(void*);

/* These two functions configure the RT task to use enforced exe budgets */
int create_rt_task(rt_fn_t rt_prog, void *arg, int cpu, int wcet, int period);
int __create_rt_task(rt_fn_t rt_prog, void *arg, int cpu, int wcet,
		     int period, task_class_t cls);
int __create_rt_task_edffm(rt_fn_t rt_prog, void *arg, int cpu, int wcet,
		int period, lt_t *frac1, lt_t *frac2,
		int cpu1, int cpu2, task_class_t class);
int __create_rt_task_npsf(rt_fn_t rt_prog, void *arg, int cpu, int wcet,
		int period, int npsf_id, task_class_t class);

/* wrapper to mask __launch_rt_task() for semi-partitioned algorithms
 * (it can be extended to cover all algorithms that directly submit
 * an rt_task structure instead of a set of values).
 */
int create_rt_task_semi(rt_fn_t rt_prog, void *arg, struct rt_task *params);

/*	per-task modes */
enum rt_task_mode_t {
	BACKGROUND_TASK = 0,
	LITMUS_RT_TASK  = 1
};
int task_mode(int target_mode);

void show_rt_param(struct rt_task* tp);
task_class_t str2class(const char* str);

/* non-preemptive section support */
void enter_np(void);
void exit_np(void);

/* task system support */
int wait_for_ts_release(void);
int release_ts(lt_t *delay);

#define __NS_PER_MS 1000000

static inline lt_t ms2lt(unsigned long milliseconds)
{
	return __NS_PER_MS * milliseconds;
}

/* CPU time consumed so far in seconds */
double cputime(void);

/* wall-clock time in seconds */
double wctime(void);

/* semaphore allocation */

static inline int open_fmlp_sem(int fd, int name)
{
	return od_open(fd, FMLP_SEM, name);
}

static inline int open_srp_sem(int fd, int name)
{
	return od_open(fd, SRP_SEM, name);
}


/* syscall overhead measuring */
int null_call(cycles_t *timestamp);

/*
 * get control page:
 * atm it is used only by preemption migration overhead code
 * but it is very general and can be used for different purposes
 */
struct control_page* get_ctrl_page(void);

/* NPS-F syscall to add a notional processor (a server) to a cpu.
 * A notional processor may span across multiple cpu.
 *
 * @npsf_id:	a "unique" identifier for the notional processor.
 * @budgets:	array of (cpu, budget-ns) that describes this np.
 * 		on possibly more than one cpu.
 * @last:	marks the end of servers initialization and trigger
 * 		the switching of servers in the plugin.
 * 		Should be set to 1 only once at the end of the sequence
 * 		of add_server() calls
 *
 * Currently implemented on x86_64 only.
 */
int add_server(int *npsf_id, struct npsf_budgets *budgets, int last);

#ifdef __cplusplus
}
#endif
#endif
