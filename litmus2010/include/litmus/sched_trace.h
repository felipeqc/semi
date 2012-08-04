/*
 * sched_trace.h -- record scheduler events to a byte stream for offline analysis.
 */
#ifndef _LINUX_SCHED_TRACE_H_
#define _LINUX_SCHED_TRACE_H_

/* all times in nanoseconds */

struct st_trace_header {
	u8	type;		/* Of what type is this record?  */
	u8	cpu;		/* On which CPU was it recorded? */
	u16	pid;		/* PID of the task.              */
	u32	job;		/* The job sequence number.      */
};

#define ST_NAME_LEN 16
struct st_name_data {
	char	cmd[ST_NAME_LEN];/* The name of the executable of this process. */
};

struct st_param_data {		/* regular params */
	u32	wcet;
	u32	period;
	u32	phase;
	u8	partition;
	u8	__unused[3];
};

struct st_release_data {	/* A job is was/is going to be released. */
	u64	release;	/* What's the release time?              */
	u64	deadline;	/* By when must it finish?		 */
};

struct st_assigned_data {	/* A job was asigned to a CPU. 		 */
	u64	when;
	u8	target;		/* Where should it execute?	         */
	u8	__unused[3];
};

struct st_switch_to_data {	/* A process was switched to on a given CPU.   */
	u64	when;		/* When did this occur?                        */
	u32	exec_time;	/* Time the current job has executed.          */

};

struct st_switch_away_data {	/* A process was switched away from on a given CPU. */
	u64	when;
	u64	exec_time;
};

struct st_completion_data {	/* A job completed. */
	u64	when;
	u8	forced:1; 	/* Set to 1 if job overran and kernel advanced to the
				 * next task automatically; set to 0 otherwise.
				 */
	u8	__uflags:7;
	u8	__unused[3];
};

struct st_block_data {		/* A task blocks. */
	u64	when;
	u64	__unused;
};

struct st_resume_data {		/* A task resumes. */
	u64	when;
	u64	__unused;
};

struct st_sys_release_data {
	u64	when;
	u64	release;
};

#define DATA(x) struct st_ ## x ## _data x;

typedef enum {
        ST_NAME = 1,		/* Start at one, so that we can spot
				 * uninitialized records. */
	ST_PARAM,
	ST_RELEASE,
	ST_ASSIGNED,
	ST_SWITCH_TO,
	ST_SWITCH_AWAY,
	ST_COMPLETION,
	ST_BLOCK,
	ST_RESUME,
	ST_SYS_RELEASE,
} st_event_record_type_t;

struct st_event_record {
	struct st_trace_header hdr;
	union {
		u64 raw[2];

		DATA(name);
		DATA(param);
		DATA(release);
		DATA(assigned);
		DATA(switch_to);
		DATA(switch_away);
		DATA(completion);
		DATA(block);
		DATA(resume);
		DATA(sys_release);

	} data;
};

#undef DATA

#ifdef __KERNEL__

#include <linux/sched.h>
#include <litmus/feather_trace.h>

#ifdef CONFIG_SCHED_TASK_TRACE

#define SCHED_TRACE(id, callback, task) \
	ft_event1(id, callback, task)
#define SCHED_TRACE2(id, callback, task, xtra) \
	ft_event2(id, callback, task, xtra)

/* provide prototypes; needed on sparc64 */
#ifndef NO_TASK_TRACE_DECLS
feather_callback void do_sched_trace_task_name(unsigned long id,
					       struct task_struct* task);
feather_callback void do_sched_trace_task_param(unsigned long id,
						struct task_struct* task);
feather_callback void do_sched_trace_task_release(unsigned long id,
						  struct task_struct* task);
feather_callback void do_sched_trace_task_switch_to(unsigned long id,
						    struct task_struct* task);
feather_callback void do_sched_trace_task_switch_away(unsigned long id,
						      struct task_struct* task);
feather_callback void do_sched_trace_task_completion(unsigned long id,
						     struct task_struct* task,
						     unsigned long forced);
feather_callback void do_sched_trace_task_block(unsigned long id,
						struct task_struct* task);
feather_callback void do_sched_trace_task_resume(unsigned long id,
						 struct task_struct* task);
feather_callback void do_sched_trace_sys_release(unsigned long id,
						 lt_t* start);
#endif

#else

#define SCHED_TRACE(id, callback, task)        /* no tracing */
#define SCHED_TRACE2(id, callback, task, xtra) /* no tracing */

#endif


#define SCHED_TRACE_BASE_ID 500


#define sched_trace_task_name(t) \
	SCHED_TRACE(SCHED_TRACE_BASE_ID + 1, do_sched_trace_task_name, t)
#define sched_trace_task_param(t) \
	SCHED_TRACE(SCHED_TRACE_BASE_ID + 2, do_sched_trace_task_param, t)
#define sched_trace_task_release(t) \
	SCHED_TRACE(SCHED_TRACE_BASE_ID + 3, do_sched_trace_task_release, t)
#define sched_trace_task_switch_to(t) \
	SCHED_TRACE(SCHED_TRACE_BASE_ID + 4, do_sched_trace_task_switch_to, t)
#define sched_trace_task_switch_away(t) \
	SCHED_TRACE(SCHED_TRACE_BASE_ID + 5, do_sched_trace_task_switch_away, t)
#define sched_trace_task_completion(t, forced) \
	SCHED_TRACE2(SCHED_TRACE_BASE_ID + 6, do_sched_trace_task_completion, t, \
		     (unsigned long) forced)
#define sched_trace_task_block(t) \
	SCHED_TRACE(SCHED_TRACE_BASE_ID + 7, do_sched_trace_task_block, t)
#define sched_trace_task_resume(t) \
	SCHED_TRACE(SCHED_TRACE_BASE_ID + 8, do_sched_trace_task_resume, t)
/* when is a pointer, it does not need an explicit cast to unsigned long */
#define sched_trace_sys_release(when) \
	SCHED_TRACE(SCHED_TRACE_BASE_ID + 9, do_sched_trace_sys_release, when)

#define sched_trace_quantum_boundary() /* NOT IMPLEMENTED */

#ifdef CONFIG_SCHED_DEBUG_TRACE
void sched_trace_log_message(const char* fmt, ...);
void dump_trace_buffer(int max);
#else

#define sched_trace_log_message(fmt, ...)

#endif

#endif /* __KERNEL__ */

#endif
