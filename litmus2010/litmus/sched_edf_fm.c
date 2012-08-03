/*
 * litmus/sched_edf_fm.c
 *
 * Implementation of the EDF-fm scheduling algorithm.
 */

#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#include <linux/module.h>

#include <litmus/litmus.h>
#include <litmus/jobs.h>
#include <litmus/sched_plugin.h>
#include <litmus/edf_common.h>

typedef struct {
	rt_domain_t 		domain;
	int          		cpu;
	struct task_struct* 	scheduled; /* only RT tasks */
/* domain lock */
#define slock domain.ready_lock
} edffm_domain_t;

DEFINE_PER_CPU(edffm_domain_t, edffm_domains);

#define local_edffm		(&__get_cpu_var(edffm_domains))
#define remote_edf(cpu)		(&per_cpu(edffm_domains, cpu).domain)
#define remote_edffm(cpu)	(&per_cpu(edffm_domains, cpu))
#define task_edf(task)		remote_edf(get_partition(task))
#define task_edffm(task)	remote_edffm(get_partition(task))

#define edffm_params(t)		(t->rt_param.task_params.semi_part.fm)

/* Is the task a migratory task? */
#define is_migrat_task(task)	(edffm_params(task).nr_cpus)
/* t is on the wrong CPU (it should be requeued properly) */
#define wrong_cpu(t)	is_migrat_task((t)) && task_cpu((t)) != get_partition((t))
/* Get next CPU */
#define migrat_next_cpu(t)	\
	((tsk_rt(t)->task_params.cpu == edffm_params(t).cpus[0]) ? \
		edffm_params(t).cpus[1] : \
		edffm_params(t).cpus[0])
/* Get current cpu */
#define migrat_cur_cpu(t)	\
	((tsk_rt(t)->task_params.cpu == edffm_params(t).cpus[0]) ? \
		edffm_params(t).cpus[0] : \
		edffm_params(t).cpus[1])
/* Manipulate share for current cpu */
#define cur_cpu_fract_num(t)	\
	((tsk_rt(t)->task_params.cpu == edffm_params(t).cpus[0]) ? \
		edffm_params(t).fraction[0][0] : \
		edffm_params(t).fraction[0][1])
#define cur_cpu_fract_den(t)	\
	((tsk_rt(t)->task_params.cpu == edffm_params(t).cpus[0]) ? \
		edffm_params(t).fraction[1][0] : \
		edffm_params(t).fraction[1][1])
/* Get job number for current cpu */
#define cur_cpu_job_no(t)	\
	((tsk_rt(t)->task_params.cpu == edffm_params(t).cpus[0]) ? \
		tsk_rt(t)->semi_part.cpu_job_no[0] : \
		tsk_rt(t)->semi_part.cpu_job_no[1])
/* What is the current cpu position in the array? */
#define edffm_cpu_pos(cpu,t)    \
	((cpu == edffm_params(t).cpus[0]) ? \
	 0 : 1)

/*
 * EDF-fm: migratory tasks have higher prio than fixed, EDF in both classes.
 * (Both first and second may be NULL).
 */
int edffm_higher_prio(struct task_struct* first, struct task_struct* second)
{
	if ((first && edffm_params(first).nr_cpus) ||
			(second && edffm_params(second).nr_cpus)) {
		if ((first && edffm_params(first).nr_cpus) &&
			(second && edffm_params(second).nr_cpus))
			/* both are migrating */
			return edf_higher_prio(first, second);

		if (first && edffm_params(first).nr_cpus)
			/* first is migrating */
			return 1;
		else
			/* second is migrating */
			return 0;
	}

	/* both are fixed or not real time */
	return edf_higher_prio(first, second);
}

/* need_to_preempt - check whether the task t needs to be preempted
 *                   call only with irqs disabled and with ready_lock acquired
 */
int edffm_preemption_needed(rt_domain_t* rt, struct task_struct *t)
{
	/* we need the read lock for edf_ready_queue */
	/* no need to preempt if there is nothing pending */
	if (!__jobs_pending(rt))
		return 0;
	/* we need to reschedule if t doesn't exist */
	if (!t)
		return 1;

	/* make sure to get non-rt stuff out of the way */
	return !is_realtime(t) || edffm_higher_prio(__next_ready(rt), t);
}

/* we assume the lock is being held */
static void preempt(edffm_domain_t *edffm)
{
	preempt_if_preemptable(edffm->scheduled, edffm->cpu);
}

static void edffm_release_jobs(rt_domain_t* rt, struct bheap* tasks)
{
	unsigned long flags;
	edffm_domain_t *edffm = container_of(rt, edffm_domain_t, domain);

	raw_spin_lock_irqsave(&edffm->slock, flags);

	__merge_ready(rt, tasks);

	if (edffm_preemption_needed(rt, edffm->scheduled))
		preempt(edffm);

	raw_spin_unlock_irqrestore(&edffm->slock, flags);
}

/* EDF-fm uses the "release_master" field to force the next release for
 * the task 'task' to happen on a remote CPU. The remote cpu for task is
 * previously set up during job_completion() taking into consideration
 * whether a task is a migratory task or not.
 */
static inline void
edffm_add_release_remote(struct task_struct *task)
{
	unsigned long flags;
	rt_domain_t *rt = task_edf(task);

	raw_spin_lock_irqsave(&rt->tobe_lock, flags);

	/* "modify" destination cpu */
	rt->release_master = get_partition(task);

	TRACE_TASK(task, "Add remote release: smp_proc_id = %d, cpu = %d, remote = %d\n",
			smp_processor_id(), task_cpu(task), rt->release_master);

	/* trigger future release */
	__add_release(rt, task);

	/* reset proper release_master and unlock */
	rt->release_master = NO_CPU;
	raw_spin_unlock_irqrestore(&rt->tobe_lock, flags);
}

/* perform double ready_queue locking in an orderwise fashion
 * this is called with: interrupt disabled and rq->lock held (from
 * schedule())
 */
static noinline void double_domain_lock(edffm_domain_t *dom1, edffm_domain_t *dom2)
{
	if (dom1 == dom2) {
		/* fake */
		raw_spin_lock(&dom1->slock);
	} else {
		if (dom1 < dom2) {
			raw_spin_lock(&dom1->slock);
			raw_spin_lock(&dom2->slock);
			TRACE("acquired %d and %d\n", dom1->cpu, dom2->cpu);
		} else {
			raw_spin_lock(&dom2->slock);
			raw_spin_lock(&dom1->slock);
			TRACE("acquired %d and %d\n", dom2->cpu, dom1->cpu);
		}
	}
}

/* Directly insert a task in a remote ready queue. This function
 * should only be called if this task is a migrating task and its
 * last job for this CPU just completed (a new one is released for
 * a remote CPU), but the new job is already tardy.
 */
static noinline void insert_task_in_remote_ready(struct task_struct *task)
{
	edffm_domain_t *this = remote_edffm(task_cpu(task));
	edffm_domain_t *remote = remote_edffm(get_partition(task));

	BUG_ON(get_partition(task) != remote->cpu);

	TRACE_TASK(task, "Migrate From P%d -> To P%d\n",
			this->cpu, remote->cpu);
	TRACE_TASK(task, "Inserting in remote ready queue\n");

	WARN_ON(!irqs_disabled());

	raw_spin_unlock(&this->slock);
	mb();
	TRACE_TASK(task,"edffm_lock %d released\n", this->cpu);

	/* lock both ready queues */
	double_domain_lock(this, remote);
	mb();

	__add_ready(&remote->domain, task);

	/* release remote but keep ours */
	raw_spin_unlock(&remote->slock);
	TRACE_TASK(task,"edffm_lock %d released\n", remote->cpu);

	/* ask remote cpu to reschedule, we are already rescheduling on this */
	preempt(remote);
}

static void requeue(struct task_struct* t, rt_domain_t *edf)
{
	if (t->state != TASK_RUNNING)
		TRACE_TASK(t, "requeue: !TASK_RUNNING\n");

	set_rt_flags(t, RT_F_RUNNING);
	if (is_released(t, litmus_clock())) {
		if (wrong_cpu(t)) {
			/* this should only happen if t just completed, but
			 * its next release is already tardy, so it should be
			 * migrated and inserted in the remote ready queue
			 */
			TRACE_TASK(t, "Migrating task already released, "
				       "move from P%d to P%d\n",
					task_cpu(t), get_partition(t));

			insert_task_in_remote_ready(t);
		} else {
			/* not a migrat task or the job is on the right CPU */
			__add_ready(edf, t);
		}
	} else {
		if (wrong_cpu(t)) {

			TRACE_TASK(t, "Migrating task, adding remote release\n");
			edffm_add_release_remote(t);
		} else {
			TRACE_TASK(t, "Adding local release\n");
			add_release(edf, t);
		}
	}
}

/* Update statistics for the _current_ job.
 * 	- job_no was incremented _before_ starting this job
 * 	(release_at / prepare_for_next_period)
 * 	- cpu_job_no is incremented when the job completes
 */
static void update_job_counter(struct task_struct *t)
{
	int cpu_pos;

	/* Which CPU counter should be incremented? */
	cpu_pos = edffm_cpu_pos(t->rt_param.task_params.cpu, t);
	t->rt_param.semi_part.cpu_job_no[cpu_pos]++;

	TRACE_TASK(t, "job_no = %d, cpu_job_no(pos %d) = %d, cpu %d\n",
			t->rt_param.job_params.job_no, cpu_pos, cur_cpu_job_no(t),
			t->rt_param.task_params.cpu);
}

/* What is the next cpu for this job? (eq. 8, in EDF-Fm paper) */
static int next_cpu_for_job(struct task_struct *t)
{
	BUG_ON(!is_migrat_task(t));

	TRACE_TASK(t, "%u = %u * %u / %u\n",
			t->rt_param.job_params.job_no, cur_cpu_job_no(t),
			cur_cpu_fract_den(t), cur_cpu_fract_num(t));
	if ((t->rt_param.job_params.job_no) ==
			(((lt_t) cur_cpu_job_no(t) * cur_cpu_fract_den(t)) /
			cur_cpu_fract_num(t)))
		return edffm_params(t).cpus[0];

	return edffm_params(t).cpus[1];
}

/* If needed (the share for task t on this CPU is exhausted), updates
 * the task_params.cpu for the _migrating_ task t
 */
static void change_migrat_cpu_if_needed(struct task_struct *t)
{
	BUG_ON(!is_migrat_task(t));
	/* EDF-fm: if it is a migrating task and it has already executed
	 * the required number of jobs on this CPU, we need to move it
	 * on its next CPU; changing the cpu here will affect the requeue
	 * and the next release
	 */
	if (unlikely(next_cpu_for_job(t) != migrat_cur_cpu(t))) {

		tsk_rt(t)->task_params.cpu = migrat_next_cpu(t);
		TRACE_TASK(t, "EDF-fm: will migrate job %d -> %d\n",
			task_cpu(t), tsk_rt(t)->task_params.cpu);
		return;
	}

	TRACE_TASK(t, "EDF-fm: job will stay on %d -> %d\n",
			task_cpu(t), tsk_rt(t)->task_params.cpu);
}

static void job_completion(struct task_struct* t, int forced)
{
	sched_trace_task_completion(t,forced);
	TRACE_TASK(t, "job_completion().\n");

	if (unlikely(is_migrat_task(t))) {
		update_job_counter(t);
		change_migrat_cpu_if_needed(t);
	}

	set_rt_flags(t, RT_F_SLEEP);
	prepare_for_next_period(t);
}

static void edffm_tick(struct task_struct *t)
{
	edffm_domain_t *edffm = local_edffm;

	BUG_ON(is_realtime(t) && t != edffm->scheduled);

	if (is_realtime(t) && budget_enforced(t) && budget_exhausted(t)) {
		set_tsk_need_resched(t);
		TRACE("edffm_scheduler_tick: "
			"%d is preemptable "
			" => FORCE_RESCHED\n", t->pid);
	}
}

static struct task_struct* edffm_schedule(struct task_struct * prev)
{
	edffm_domain_t* 	edffm = local_edffm;
	rt_domain_t*		edf  = &edffm->domain;
	struct task_struct*	next;

	int out_of_time, sleep, preempt, exists, blocks, change_cpu, resched;

	raw_spin_lock(&edffm->slock);

	BUG_ON(edffm->scheduled && edffm->scheduled != prev);
	BUG_ON(edffm->scheduled && !is_realtime(prev));

	/* (0) Determine state */
	exists      = edffm->scheduled != NULL;
	blocks      = exists && !is_running(edffm->scheduled);
	out_of_time = exists &&
				  budget_enforced(edffm->scheduled) &&
				  budget_exhausted(edffm->scheduled);
	sleep	    = exists && get_rt_flags(edffm->scheduled) == RT_F_SLEEP;
	change_cpu  = exists && wrong_cpu(edffm->scheduled);
	preempt     = edffm_preemption_needed(edf, prev);

	BUG_ON(blocks && change_cpu);

	if (exists)
		TRACE_TASK(prev,
			   "blocks:%d out_of_time:%d sleep:%d preempt:%d "
			   "wrong_cpu:%d state:%d sig:%d\n",
			   blocks, out_of_time, sleep, preempt,
			   change_cpu, prev->state, signal_pending(prev));

	/* If we need to preempt do so. */
	resched = preempt;

	/* If a task blocks we have no choice but to reschedule. */
	if (blocks)
		resched = 1;

	/* If a task has just woken up, it was tardy and the wake up
	 * raced with this schedule, a new job has already been released,
	 * but scheduled should be enqueued on a remote ready queue, and a
	 * new task should be selected for the current queue.
	 */
	if (change_cpu)
		resched = 1;

	/* Any task that is preemptable and either exhausts its execution
	 * budget or wants to sleep completes. We may have to reschedule after
	 * this.
	 */
	if ((out_of_time || sleep) && !blocks) {
		job_completion(edffm->scheduled, !sleep);
		resched = 1;
	}

	/* The final scheduling decision. Do we need to switch for some reason?
	 * Switch if we are in RT mode and have no task or if we need to
	 * resched.
	 */
	next = NULL;
	if (resched || !exists) {

		if (edffm->scheduled && !blocks)
			requeue(edffm->scheduled, edf);
		next = __take_ready(edf);
	} else
		/* Only override Linux scheduler if we have a real-time task
		 * scheduled that needs to continue.
		 */
		if (exists)
			next = prev;

	if (next) {
		TRACE_TASK(next, "scheduled at %llu\n", litmus_clock());
		set_rt_flags(next, RT_F_RUNNING);
	} else {
		TRACE("becoming idle at %llu\n", litmus_clock());
	}

	edffm->scheduled = next;
	raw_spin_unlock(&edffm->slock);

	return next;
}

/*	Prepare a task for running in RT mode
 */
static void edffm_task_new(struct task_struct * t, int on_rq, int running)
{
	rt_domain_t* 		edf  = task_edf(t);
	edffm_domain_t* 	edffm = task_edffm(t);
	unsigned long		flags;

	TRACE_TASK(t, "EDF-fm: task new, cpu = %d\n",
		   t->rt_param.task_params.cpu);

	release_at(t, litmus_clock());
	update_job_counter(t);

	/* The task should be running in the queue, otherwise signal
	 * code will try to wake it up with fatal consequences.
	 */
	raw_spin_lock_irqsave(&edffm->slock, flags);
	if (running) {
		/* there shouldn't be anything else running at the time */
		BUG_ON(edffm->scheduled);
		edffm->scheduled = t;
	} else {
		requeue(t, edf);
		/* maybe we have to reschedule */
		preempt(edffm);
	}
	raw_spin_unlock_irqrestore(&edffm->slock, flags);
}

static void edffm_task_wake_up(struct task_struct *task)
{
	unsigned long		flags;
	edffm_domain_t* 	edffm = task_edffm(task);
	rt_domain_t* 		edf  = task_edf(task);
	lt_t			now;

	TRACE_TASK(task, "wake_up at %llu\n", litmus_clock());

	TRACE_TASK(task, "acquire edffm %d\n", edffm->cpu);
	raw_spin_lock_irqsave(&edffm->slock, flags);

	BUG_ON(edffm != task_edffm(task));
	BUG_ON(is_queued(task));

	now = litmus_clock();
	if (is_tardy(task, now)) {
		if (unlikely(is_migrat_task(task))) {
			/* a new job will be released.
			 * Update current job counter */
			update_job_counter(task);
			/* Switch CPU if needed */
			change_migrat_cpu_if_needed(task);
		}
		/* new sporadic release */
		TRACE_TASK(task, "release new\n");
		release_at(task, now);
		sched_trace_task_release(task);
	}

	/* Only add to ready queue if it is not the currently-scheduled
	 * task. This could be the case if a task was woken up concurrently
	 * on a remote CPU before the executing CPU got around to actually
	 * de-scheduling the task, i.e., wake_up() raced with schedule()
	 * and won.
	 */
	if (edffm->scheduled != task)
		requeue(task, edf);

	raw_spin_unlock_irqrestore(&edffm->slock, flags);
	TRACE_TASK(task, "release edffm %d\n", edffm->cpu);
	TRACE_TASK(task, "wake up done\n");
}

static void edffm_task_block(struct task_struct *t)
{
	TRACE_TASK(t, "block at %llu, state=%d\n", litmus_clock(), t->state);

	BUG_ON(!is_realtime(t));
	if (is_queued(t)) {
		edffm_domain_t *edffm = local_edffm;
		TRACE_TASK(t, "task blocked, race with wakeup, "
				"remove from queue %d\n", edffm->cpu);
		remove(&edffm->domain, t);
	}
}

static void edffm_task_exit(struct task_struct * t)
{
	unsigned long flags;
	edffm_domain_t* 	edffm = task_edffm(t);
	rt_domain_t*		edf;

	raw_spin_lock_irqsave(&edffm->slock, flags);
	if (is_queued(t)) {
		/* dequeue */
		edf  = task_edf(t);
		remove(edf, t);
	}
	if (edffm->scheduled == t)
		edffm->scheduled = NULL;

	TRACE_TASK(t, "RIP\n");

	preempt(edffm);
	raw_spin_unlock_irqrestore(&edffm->slock, flags);
}

static long edffm_admit_task(struct task_struct* tsk)
{
	return task_cpu(tsk) == tsk->rt_param.task_params.cpu ? 0 : -EINVAL;
}

/*	Plugin object	*/
static struct sched_plugin edffm_plugin __cacheline_aligned_in_smp = {
	.plugin_name		= "EDF-fm",
	.tick			= edffm_tick,
	.task_new		= edffm_task_new,
	.complete_job		= complete_job,
	.task_exit		= edffm_task_exit,
	.schedule		= edffm_schedule,
	.task_wake_up		= edffm_task_wake_up,
	.task_block		= edffm_task_block,
	.admit_task		= edffm_admit_task
};

static int __init init_edffm(void)
{
	int i;
	edffm_domain_t *edffm;

	/* Note, broken if num_online_cpus() may change */
	for (i = 0; i < num_online_cpus(); i++) {
		edffm = remote_edffm(i);
		edffm->cpu = i;
		edffm->scheduled = NULL;
		edf_domain_init(&edffm->domain, NULL, edffm_release_jobs);
	}

	return register_sched_plugin(&edffm_plugin);
}

module_init(init_edffm);

