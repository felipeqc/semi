#include <linux/threads.h>

/*
 * Definition of the scheduler plugin interface.
 *
 */
#ifndef _LINUX_RT_PARAM_H_
#define _LINUX_RT_PARAM_H_

/* Litmus time type. */
typedef unsigned long long lt_t;

static inline int lt_after(lt_t a, lt_t b)
{
	return ((long long) b) - ((long long) a) < 0;
}
#define lt_before(a, b) lt_after(b, a)

static inline int lt_after_eq(lt_t a, lt_t b)
{
	return ((long long) a) - ((long long) b) >= 0;
}
#define lt_before_eq(a, b) lt_after_eq(b, a)

/* different types of clients */
typedef enum {
	RT_CLASS_HARD,
	RT_CLASS_SOFT,
	RT_CLASS_BEST_EFFORT
} task_class_t;

typedef enum {
	NO_ENFORCEMENT,      /* job may overrun unhindered */
	QUANTUM_ENFORCEMENT, /* budgets are only checked on quantum boundaries */
	PRECISE_ENFORCEMENT  /* NOT IMPLEMENTED - enforced with hrtimers */
} budget_policy_t;


/* The parameters for EDF-Fm scheduling algorithm.
 * Each task may be fixed or migratory. Migratory tasks may
 * migrate on 2 (contiguous) CPU only. NR_CPUS_EDF_FM = 2.
 */
#define NR_CPUS_EDF_FM	2

struct edffm_params {
	/* EDF-fm where can a migratory task execute? */
	unsigned int	cpus[NR_CPUS_EDF_FM];
	/* how many cpus are used by this task?
	 * fixed = 0, migratory = (NR_CPUS_EDF_FM - 1)
	 * Efficient way to allow writing cpus[nr_cpus].
	 */
	unsigned int	nr_cpus;
	/* Fraction of this task exec_cost that each CPU should handle.
	 * We keep the fraction divided in num/denom : a matrix of
	 * (NR_CPUS_EDF_FM rows) x (2 columns).
	 * The first column is the numerator of the fraction.
	 * The second column is the denominator.
	 * In EDF-fm this is a 2*2 matrix
	 */
	lt_t		fraction[2][NR_CPUS_EDF_FM];
};

/* Parameters for NPS-F semi-partitioned scheduling algorithm.
 * Each (cpu, budget) entry defines the share ('budget' in ns, a % of
 * the slot_length) of the notional processor on the CPU 'cpu'.
 * This structure is used by the library - syscall interface in order
 * to go through the overhead of a syscall only once per server.
 */
struct npsf_budgets {
	int	cpu;
	lt_t	budget;
};

/* The parameters for the EDF-WM semi-partitioned scheduler.
 * Each task may be split across multiple cpus. Each per-cpu allocation
 * is called a 'slice'.
 */
#define MAX_EDF_WM_SLICES 24
#define MIN_EDF_WM_SLICE_SIZE 50000 /* .05 millisecond = 50us */

struct edf_wm_slice {
	/* on which CPU is this slice allocated */
	unsigned int cpu;
	/* relative deadline from job release (not from slice release!) */
	lt_t deadline;
	/* budget of this slice; must be precisely enforced */
	lt_t budget;
	/* offset of this slice relative to the job release */
	lt_t offset;
};

/* If a job is not sliced across multiple CPUs, then
 * count is set to zero and none of the slices is used.
 * This implies that count == 1 is illegal.
 */
struct edf_wm_params {
	/* enumeration of all slices */
	struct edf_wm_slice slices[MAX_EDF_WM_SLICES];

	/* how many slices are defined? */
	unsigned int count;
};

struct rt_task {
	lt_t 		exec_cost;
	lt_t 		period;
	lt_t		phase;
	unsigned int	cpu;
	task_class_t	cls;
	budget_policy_t budget_policy; /* ignored by pfair */

	/* parameters used by the semi-partitioned algorithms */
	union {
		/* EDF-Fm; defined in sched_edf_fm.c */
		struct edffm_params fm;

		/* NPS-F; defined in sched_npsf.c
		 * id for the server (notional processor) that holds
		 * this task; the same npfs_id can be assigned to "the same"
		 * server split on different cpus
		 */
		int npsf_id;

		/* EDF-WM; defined in sched_edf_wm.c */
		struct edf_wm_params wm;
	} semi_part;
};

/* The definition of the data that is shared between the kernel and real-time
 * tasks via a shared page (see litmus/ctrldev.c).
 *
 * WARNING: User space can write to this, so don't trust
 * the correctness of the fields!
 *
 * This servees two purposes: to enable efficient signaling
 * of non-preemptive sections (user->kernel) and
 * delayed preemptions (kernel->user), and to export
 * some real-time relevant statistics such as preemption and
 * migration data to user space. We can't use a device to export
 * statistics because we want to avoid system call overhead when
 * determining preemption/migration overheads).
 */
struct control_page {
	/* Is the task currently in a non-preemptive section? */
	int np_flag;
	/* Should the task call into the kernel when it leaves
	 * its non-preemptive section? */
	int delayed_preemption;

	/* to be extended */
};

/* don't export internal data structures to user space (liblitmus) */
#ifdef __KERNEL__

struct _rt_domain;
struct bheap_node;
struct release_heap;

struct rt_job {
	/* Time instant the the job was or will be released.  */
	lt_t	release;
	/* What is the current deadline? */
	lt_t   	deadline;

	/* How much service has this job received so far? */
	lt_t	exec_time;

	/* Which job is this. This is used to let user space
	 * specify which job to wait for, which is important if jobs
	 * overrun. If we just call sys_sleep_next_period() then we
	 * will unintentionally miss jobs after an overrun.
	 *
	 * Increase this sequence number when a job is released.
	 */
	unsigned int    job_no;
};

struct pfair_param;

/*	RT task parameters for scheduling extensions
 *	These parameters are inherited during clone and therefore must
 *	be explicitly set up before the task set is launched.
 */
struct rt_param {
	/* is the task sleeping? */
	unsigned int 		flags:8;

	/* do we need to check for srp blocking? */
	unsigned int		srp_non_recurse:1;

	/* is the task present? (true if it can be scheduled) */
	unsigned int		present:1;

	/* user controlled parameters */
	struct rt_task 		task_params;

	/* timing parameters */
	struct rt_job 		job_params;

	/* task representing the current "inherited" task
	 * priority, assigned by inherit_priority and
	 * return priority in the scheduler plugins.
	 * could point to self if PI does not result in
	 * an increased task priority.
	 */
	 struct task_struct*	inh_task;

#ifdef CONFIG_NP_SECTION
	/* For the FMLP under PSN-EDF, it is required to make the task
	 * non-preemptive from kernel space. In order not to interfere with
	 * user space, this counter indicates the kernel space np setting.
	 * kernel_np > 0 => task is non-preemptive
	 */
	unsigned int	kernel_np;
#endif

	/* This field can be used by plugins to store where the task
	 * is currently scheduled. It is the responsibility of the
	 * plugin to avoid race conditions.
	 *
	 * This used by GSN-EDF and PFAIR.
	 */
	volatile int		scheduled_on;

	/* Is the stack of the task currently in use? This is updated by
	 * the LITMUS core.
	 *
	 * Be careful to avoid deadlocks!
	 */
	volatile int		stack_in_use;

	/* This field can be used by plugins to store where the task
	 * is currently linked. It is the responsibility of the plugin
	 * to avoid race conditions.
	 *
	 * Used by GSN-EDF.
	 */
	volatile int		linked_on;

	/* PFAIR/PD^2 state. Allocated on demand. */
	struct pfair_param*	pfair;

	/* Fields saved before BE->RT transition.
	 */
	int old_policy;
	int old_prio;

	/* ready queue for this task */
	struct _rt_domain* domain;

	/* heap element for this task
	 *
	 * Warning: Don't statically allocate this node. The heap
	 *          implementation swaps these between tasks, thus after
	 *          dequeuing from a heap you may end up with a different node
	 *          then the one you had when enqueuing the task.  For the same
	 *          reason, don't obtain and store references to this node
	 *          other than this pointer (which is updated by the heap
	 *          implementation).
	 */
	struct bheap_node*	heap_node;
	struct release_heap*	rel_heap;

	/* Used by rt_domain to queue task in release list.
	 */
	struct list_head list;

	/* Pointer to the page shared between userspace and kernel. */
	struct control_page * ctrl_page;

	/* runtime info for the semi-part plugins */
	union {
		/* EDF-Fm runtime information
		 * number of jobs handled by this cpu
		 * (to determine next cpu for a migrating task)
		 */
		unsigned int	cpu_job_no[NR_CPUS_EDF_FM];

		/* EDF-WM runtime information */
		struct {
			/* at which exec time did the current slice start? */
			lt_t exec_time;
			/* when did the job suspend? */
			lt_t suspend_time;
			/* cached job parameters */
			lt_t job_release, job_deadline;
			/* pointer to the current slice */
			struct edf_wm_slice* slice;
		} wm;
	} semi_part;
};

/*	Possible RT flags	*/
#define RT_F_RUNNING		0x00000000
#define RT_F_SLEEP		0x00000001
#define RT_F_EXIT_SEM		0x00000008

#endif

#endif
