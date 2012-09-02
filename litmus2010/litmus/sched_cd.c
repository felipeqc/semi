/* C=D: based on EDF-WM code (LITMUS-RT semi-partitioned patch by Andrea Bastoni)
 * The only change is that we don't need to program release timers between migrations,
 * only for the release of the next job. So slice offset is not needed anymore.
 *
 * Comments and function names were updated from *wm* to *cd*.
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

/*
 * scheduling lock slock
 * protects the domain and serializes scheduling decisions
 */
#define slock domain.ready_lock

} cd_domain_t;

DEFINE_PER_CPU(cd_domain_t, cd_domains);

#define TRACE_DOM(dom, fmt, args...) \
	TRACE("(cd_domains[%d]) " fmt, (dom)->cpu, ##args)


#define local_domain         (&__get_cpu_var(cd_domains))
#define remote_domain(cpu)   (&per_cpu(cd_domains, cpu))
#define domain_of_task(task) (remote_domain(get_partition(task)))

static int is_sliced_task(struct task_struct* t)
{
	return tsk_rt(t)->task_params.semi_part.cd.count;
}

static struct cd_slice* get_last_slice(struct task_struct* t)
{
	int idx = tsk_rt(t)->task_params.semi_part.cd.count - 1;
	return tsk_rt(t)->task_params.semi_part.cd.slices + idx;
}

static lt_t slice_budget(struct task_struct* t)
{
	return tsk_rt(t)->semi_part.cd.slice->budget;
}

static void compute_slice_params(struct task_struct* t)
{
	struct rt_param* p = tsk_rt(t);
	/* Here we do a little trick to make the generic EDF code
	 * play well with job slices. We keep the release of the job and
         * overwrite only the absolute deadline, to force C=D.
	 * So we need to change the deadline, the cpu and the budget the slice has to execute.
         * The actual values are cached in the semi_part.cd structure.
         */

        /* As the release is not changed, we can use the original value for obtaining the absolute deadline. */      
	p->job_params.deadline = p->job_params.release + p->semi_part.cd.slice->deadline;

	/* DIRTY HACK for making BUDGET_ENFORCEMENT use slice cost instead of task cost:
         * task_params.exec_cost must be the cummulative cost of all slices
         * until now (including the current one) because it's going to be compared
	 * with the entire job exec_time in budget_remaining(). */
	p->task_params.exec_cost += slice_budget(t);

	/* Update cpu field. */
	p->task_params.cpu = p->semi_part.cd.slice->cpu;

	/* Update the per-slice budget reference */
	p->semi_part.cd.exec_time = p->job_params.exec_time;
}

static void complete_sliced_job(struct task_struct* t, int completion_signaled)
{
	struct rt_param* p = tsk_rt(t);

	/* We need to undo our trickery to the
	 * job parameters (see above) */
	p->job_params.deadline = p->semi_part.cd.job_deadline;
        p->task_params.exec_cost = p->semi_part.cd.job_exec_cost;

        sched_trace_task_completion(t, completion_signaled);

	/* Ok, now let generic code do the actual work. */
	prepare_for_next_period(t);

	/* And finally cache the updated parameters. */
	p->semi_part.cd.job_deadline = p->job_params.deadline;
}

static lt_t slice_exec_time(struct task_struct* t)
{
	struct rt_param* p = tsk_rt(t);

	/* Compute how much execution time has been consumed
	 * since last slice advancement. */
	return p->job_params.exec_time - p->semi_part.cd.exec_time;
}

static int slice_budget_exhausted(struct task_struct* t)
{
	return slice_exec_time(t) >= slice_budget(t);
}

/* assumes positive remainder; overflows otherwise */
static lt_t slice_budget_remaining(struct task_struct* t)
{
	return slice_budget(t) - slice_exec_time(t);
}

static int cd_budget_exhausted(struct task_struct* t)
{
	if (is_sliced_task(t))
		return slice_budget_exhausted(t);
	else
		return budget_exhausted(t);
}

static void advance_next_slice(struct task_struct* t, int completion_signaled)
{
	int idx;
	struct rt_param* p = tsk_rt(t);

	/* make sure this is actually a sliced job */
	BUG_ON(!is_sliced_task(t));
	BUG_ON(is_queued(t));

	/* determine index of current slice */
	idx = p->semi_part.cd.slice -
		p->task_params.semi_part.cd.slices;

	TRACE_TASK(t, "advancing slice %d; excess=%lluns; "
		   "completion_signaled=%d.\n",
		   idx, slice_exec_time(t) - slice_budget(t),
		   completion_signaled);

	if (completion_signaled)
		idx = 0;
	else
		/* increment and wrap around, if necessary */
		idx = (idx + 1) % p->task_params.semi_part.cd.count;

	/* point to next slice */
	p->semi_part.cd.slice =
		p->task_params.semi_part.cd.slices + idx;

	/* Check if we need to update essential job parameters. */
	if (!idx) {
		/* job completion */
		TRACE_TASK(t, "completed sliced job"
			   "(signaled:%d)\n", completion_signaled);
		complete_sliced_job(t, completion_signaled);

		/* Cummulative exec_cost starts at 0 (see compute_slice_params(t)) */
		p->task_params.exec_cost = 0;
	}

	/* Update job parameters for new slice. */
	compute_slice_params(t);
}

/* assumes time_passed does not advance past the last slice */
static void fast_forward_slices(struct task_struct* t, lt_t time_passed)
{
	TRACE_TASK(t, "fast forwarding %lluns\n", time_passed);

	/* this is NOT the slice version */
	BUG_ON(budget_remaining(t) <= time_passed);

	if (cd_budget_exhausted(t)) {
		/* This can happen if a suspension raced
		 * with a normal slice advancement. cd_schedule()
		 * does not process out_of_time when a task blocks. */
		TRACE_TASK(t, "block raced with out_of_time?\n");
		advance_next_slice(t, 0);
	}

	while (time_passed &&
	       time_passed >= slice_budget_remaining(t)) {
		/* slice completely exhausted */
		time_passed -= slice_budget_remaining(t);
		tsk_rt(t)->job_params.exec_time +=
			slice_budget_remaining(t);

		BUG_ON(!slice_budget_exhausted(t));
		BUG_ON(slice_budget_remaining(t) != 0);
		BUG_ON(tsk_rt(t)->semi_part.cd.slice == get_last_slice(t));

		advance_next_slice(t, 0);
	}
	/* add remainder to exec cost */
	tsk_rt(t)->job_params.exec_time += time_passed;
}

/* we assume the lock is being held */
static void preempt(cd_domain_t *dom)
{
	TRACE_DOM(dom, "will be preempted.\n");
	/* We pass NULL as the task since non-preemptive sections are not
	 * supported in this plugin, so per-task checks are not needed. */
	preempt_if_preemptable(NULL, dom->cpu);
}

static void cd_domain_init(cd_domain_t* dom,
			   check_resched_needed_t check,
			   release_jobs_t release,
			   int cpu)
{
	edf_domain_init(&dom->domain, check, release);
	dom->cpu      		= cpu;
	dom->scheduled		= NULL;
}

static void cd_requeue_remote(struct task_struct *t)
{
	cd_domain_t *dom = domain_of_task(t);

	/* C=D slices keep the release of the original task, so
	 * is_released() is always true and there is no release
	 * timer, except when we move to the next job.
	 * In that case, the release is in the future, so the timer
	 * is properly armed. */

	set_rt_flags(t, RT_F_RUNNING);
	if (is_released(t, litmus_clock()))
		/* acquires necessary lock */
		add_ready(&dom->domain, t);
	else
		/* force timer on remote CPU */
		add_release_on(&dom->domain, t, get_partition(t));
}

static void cd_requeue_local(struct task_struct* t, rt_domain_t *edf)
{
	if (t->state != TASK_RUNNING)
		TRACE_TASK(t, "requeue: !TASK_RUNNING\n");

	set_rt_flags(t, RT_F_RUNNING);
	if (is_released(t, litmus_clock()))
		__add_ready(edf, t);
	else
		add_release(edf, t); /* it has got to wait */
}

static int cd_check_resched(rt_domain_t *edf)
{
	cd_domain_t *dom = container_of(edf, cd_domain_t, domain);

	/* because this is a callback from rt_domain_t we already hold
	 * the necessary lock for the ready queue
	 */
	if (edf_preemption_needed(edf, dom->scheduled)) {
		preempt(dom);
		return 1;
	} else
		return 0;
}

static void regular_job_completion(struct task_struct* t, int forced)
{
	sched_trace_task_completion(t, forced);
	TRACE_TASK(t, "job_completion().\n");

	set_rt_flags(t, RT_F_SLEEP);
	prepare_for_next_period(t);
}

static void cd_job_or_slice_completion(struct task_struct* t,
				       int completion_signaled)
{
	if (is_sliced_task(t))
		advance_next_slice(t, completion_signaled);
	else
		regular_job_completion(t, !completion_signaled);
}

static void cd_tick(struct task_struct *t)
{
	cd_domain_t *dom = local_domain;

	/* Check for inconsistency. We don't need the lock for this since
	 * ->scheduled is only changed in schedule, which obviously is not
	 *  executing in parallel on this CPU
	 */
	BUG_ON(is_realtime(t) && t != dom->scheduled);

	if (is_realtime(t) && budget_enforced(t) && cd_budget_exhausted(t)) {
		set_tsk_need_resched(t);
		TRACE_DOM(dom, "budget of %d exhausted in tick\n",
			  t->pid);
	}
}

static struct task_struct* cd_schedule(struct task_struct * prev)
{
	cd_domain_t		*dom = local_domain;
	rt_domain_t		*edf = &dom->domain;
	struct task_struct	*next, *migrate = NULL;

	int out_of_time, sleep, preempt, wrong_cpu, exists, blocks, resched;

	raw_spin_lock(&dom->slock);

	/* Sanity checking:
	 * When a task exits (dead) dom->schedule may be null
	 * and prev _is_ realtime. */
	BUG_ON(dom->scheduled && dom->scheduled != prev);
	BUG_ON(dom->scheduled && !is_realtime(prev));

	/* (0) Determine state */
	exists      = dom->scheduled != NULL;
	wrong_cpu   = exists && get_partition(dom->scheduled) != dom->cpu;
	blocks      = exists && !is_running(dom->scheduled);
	out_of_time = exists
		&& budget_enforced(dom->scheduled)
		&& cd_budget_exhausted(dom->scheduled);
	sleep	    = exists && get_rt_flags(dom->scheduled) == RT_F_SLEEP;
	preempt     = edf_preemption_needed(edf, prev);

	/* If we need to preempt do so.
	 * The following checks set resched to 1 in case of special
	 * circumstances.
	 */
	resched = preempt;


	if (exists)
		TRACE_TASK(prev,
			   "blocks:%d out_of_time:%d sleep:%d preempt:%d "
			   "wrong_cpu:%d state:%d sig:%d\n",
			   blocks, out_of_time, sleep, preempt, wrong_cpu,
			   prev->state, signal_pending(prev));

	/* If a task blocks we have no choice but to reschedule.
	 */
	if (blocks)
		resched = 1;

	/* This can happen if sliced task was moved to the next slice
	 * by the wake_up() code path while still being scheduled.
	 */
	if (wrong_cpu)
		resched = 1;

	/* Any task that is preemptable and either exhausts its execution
	 * budget or wants to sleep completes. We may have to reschedule after
	 * this.
	 */
	if ((out_of_time || sleep) && !blocks) {
		cd_job_or_slice_completion(dom->scheduled, sleep);
		resched = 1;
	}

	/* The final scheduling decision. Do we need to switch for some reason?
	 * Switch if we are in RT mode and have no task or if we need to
	 * resched.
	 */
	next = NULL;
	if (resched || !exists) {
		if (dom->scheduled && !blocks) {
			if (get_partition(dom->scheduled) == dom->cpu)
				/* local task */
				cd_requeue_local(dom->scheduled, edf);
			else
				/* not local anymore; wait until we drop the
				 * ready queue lock */
				migrate = dom->scheduled;
		}
		next = __take_ready(edf);
	} else
		/* Only override Linux scheduler if we have a real-time task
		 * scheduled that needs to continue. */
		if (exists)
			next = prev;

	if (next) {
		TRACE_TASK(next, "scheduled at %llu (state:%d/%d)\n", litmus_clock(),
			   next->state, is_running(next));
		set_rt_flags(next, RT_F_RUNNING);
	} else if (exists) {
		TRACE("becoming idle at %llu\n", litmus_clock());
	}

	dom->scheduled = next;
	raw_spin_unlock(&dom->slock);

	/* check if we need to push the previous task onto another queue */
	if (migrate) {
		TRACE_TASK(migrate, "schedule-initiated migration to %d\n",
			   get_partition(migrate));
		cd_requeue_remote(migrate);
	}

	return next;
}


/*	Prepare a task for running in RT mode
 */
static void cd_task_new(struct task_struct * t, int on_rq, int running)
{
	cd_domain_t* dom = domain_of_task(t);
	rt_domain_t* edf = &dom->domain;
	unsigned long flags;

	TRACE_TASK(t, "C=D: task new, cpu = %d\n",
		   t->rt_param.task_params.cpu);

	/* setup job parameters */
	release_at(t, litmus_clock());

	/* The task should be running in the queue, otherwise signal
	 * code will try to wake it up with fatal consequences.
	 */
	raw_spin_lock_irqsave(&dom->slock, flags);

	if (is_sliced_task(t)) {
		/* make sure parameters are initialized consistently */
		tsk_rt(t)->semi_part.cd.exec_time = 0;
		tsk_rt(t)->semi_part.cd.job_deadline = get_deadline(t);
                tsk_rt(t)->semi_part.cd.job_exec_cost = get_exec_cost(t);
		tsk_rt(t)->semi_part.cd.slice = tsk_rt(t)->task_params.semi_part.cd.slices;
		tsk_rt(t)->job_params.exec_time = 0;
	}

	if (running) {
		/* there shouldn't be anything else running at the time */
		BUG_ON(dom->scheduled);
		dom->scheduled = t;
	} else {
		cd_requeue_local(t, edf);
		/* maybe we have to reschedule */
		preempt(dom);
	}
	raw_spin_unlock_irqrestore(&dom->slock, flags);
}

static void cd_release_at(struct task_struct *t, lt_t start)
{
	struct rt_param* p = tsk_rt(t);

	if (is_sliced_task(t)) {
		/* simulate wrapping to the first slice */
		p->semi_part.cd.job_deadline = start;
		p->semi_part.cd.slice = get_last_slice(t);
		/* FIXME: creates bogus completion event... */
		advance_next_slice(t, 0);
		set_rt_flags(t, RT_F_RUNNING);
	} else
		/* generic code handles it */
		release_at(t, start);
}

static lt_t cd_earliest_release(struct task_struct *t, lt_t now)
{
	lt_t deadline;
	if (is_sliced_task(t))
		deadline = tsk_rt(t)->semi_part.cd.job_deadline;
	else
		deadline = get_deadline(t);
	if (lt_before(deadline, now))
		return now;
	else
		return deadline;
}

static void cd_task_wake_up(struct task_struct *t)
{
	unsigned long flags;
	cd_domain_t* dom = domain_of_task(t);
	rt_domain_t* edf = &dom->domain;
	struct rt_param* p = tsk_rt(t);
	lt_t now, sleep_time;
	int migrate = 0;

	raw_spin_lock_irqsave(&dom->slock, flags);
	BUG_ON(is_queued(t));

	now = litmus_clock();

	sleep_time = now - p->semi_part.cd.suspend_time;

	TRACE_TASK(t, "wake_up at %llu after %llu, still-scheduled:%d\n",
		   now, sleep_time, dom->scheduled == t);

	/* account sleep time as execution time */
	if (get_exec_time(t) + sleep_time >= get_exec_cost(t)) {
		/* first job is not really tardy in this sense */
		if (t->rt_param.job_params.job_no != 2 &&
				get_exec_time(t) == 0) {
			/* new sporadic release */
			TRACE_TASK(t, "new sporadic release\n");
			cd_release_at(t, cd_earliest_release(t, now));
			sched_trace_task_release(t);
		}
	} else if (is_sliced_task(t)) {
		/* figure out which slice we should be executing on */
		fast_forward_slices(t, sleep_time);
		/* can't be exhausted now */
		BUG_ON(cd_budget_exhausted(t));
	} else {
		/* simply add to the execution time */
		tsk_rt(t)->job_params.exec_time += sleep_time;
	}


	/* Only add to ready queue if it is not the currently-scheduled
	 * task. This could be the case if a task was woken up concurrently
	 * on a remote CPU before the executing CPU got around to actually
	 * de-scheduling the task, i.e., wake_up() raced with schedule()
	 * and won.
	 */
	if (dom->scheduled != t) {
		if (get_partition(t) == dom->cpu)
			cd_requeue_local(t, edf);
		else
			/* post-pone migration until after unlocking */
			migrate = 1;
	}

	raw_spin_unlock_irqrestore(&dom->slock, flags);

	if (migrate) {
		TRACE_TASK(t, "wake_up-initiated migration to %d\n",
			   get_partition(t));
		cd_requeue_remote(t);
	}

	TRACE_TASK(t, "wake up done\n");
}

static void cd_task_block(struct task_struct *t)
{
	cd_domain_t* dom = domain_of_task(t);
	unsigned long flags;
	lt_t now = litmus_clock();

	TRACE_TASK(t, "block at %llu, state=%d\n", now, t->state);

	tsk_rt(t)->semi_part.cd.suspend_time = now;

	raw_spin_lock_irqsave(&dom->slock, flags);
	if (is_queued(t)) {
		TRACE_TASK(t, "still queued; migration invariant failed?\n");
		remove(&dom->domain, t);
	}
	raw_spin_unlock_irqrestore(&dom->slock, flags);

	BUG_ON(!is_realtime(t));
}

static void cd_task_exit(struct task_struct * t)
{
	unsigned long flags;
	cd_domain_t* dom = domain_of_task(t);
	rt_domain_t* edf = &dom->domain;

	raw_spin_lock_irqsave(&dom->slock, flags);
	if (is_queued(t)) {
		/* dequeue */
		remove(edf, t);
	}
	if (dom->scheduled == t)
		dom->scheduled = NULL;

	TRACE_TASK(t, "RIP, now reschedule\n");

	preempt(dom);
	raw_spin_unlock_irqrestore(&dom->slock, flags);
}

static long cd_check_params(struct task_struct *t)
{
	struct rt_param* p = tsk_rt(t);
	struct cd_params* cd = &p->task_params.semi_part.cd;
	int i;
	lt_t tmp;

	if (!is_sliced_task(t)) {
		/* regular task; nothing to check */
		TRACE_TASK(t, "accepted regular (non-sliced) task with "
			   "%d slices\n",
			   cd->count);
		return 0;
	}

	/* (1) Either not sliced, or more than 1 slice. */
	if (cd->count == 1 || cd->count > MAX_CD_SLICES) {
		TRACE_TASK(t, "bad number of slices (%u) \n",
			   cd->count);
		return -EINVAL;
	}

	/* (2) The partition has to agree with the first slice. */
	if (get_partition(t) != cd->slices[0].cpu) {
		TRACE_TASK(t, "partition and first slice CPU differ "
			   "(%d != %d)\n", get_partition(t), cd->slices[0].cpu);
		return -EINVAL;
	}

	/* (3) The total budget must agree. */
	for (i = 0, tmp = 0; i < cd->count; i++)
		tmp += cd->slices[i].budget;
	if (get_exec_cost(t) != tmp) {
		TRACE_TASK(t, "total budget and sum of slice budgets differ\n");
		return -EINVAL;
	}

	/* (4) The budget of each slice must exceed the minimum budget size. */
	for (i = 0; i < cd->count; i++)
		if (cd->slices[i].budget < MIN_CD_SLICE_SIZE) {
			TRACE_TASK(t, "slice %d is too short\n", i);
			return -EINVAL;
		}

	/* (5) The CPU of each slice must be different from the previous CPU. */
	for (i = 0; i < cd->count - 1; i++)
		if (cd->slices[i].cpu == cd->slices[i + 1].cpu) {
			TRACE_TASK(t, "slice %d does not migrate\n", i);
			return -EINVAL;
		}

	/* (6) The CPU of each slice must be online. */
	for (i = 0; i < cd->count; i++)
		if (!cpu_online(cd->slices[i].cpu)) {
			TRACE_TASK(t, "slice %d is allocated on offline CPU\n",
				   i);
			return -EINVAL;
		}

	/* (7) A sliced task's budget must be precisely enforced. */
	if (!budget_precisely_enforced(t)) {
		TRACE_TASK(t, "budget is not precisely enforced "
			   "(policy: %d).\n",
			   tsk_rt(t)->task_params.budget_policy);
		return -EINVAL;
	}

	TRACE_TASK(t, "accepted sliced task with %d slices\n",
		   cd->count);

	return 0;
}

static long cd_admit_task(struct task_struct* t)
{
	return task_cpu(t) == get_partition(t) ? cd_check_params(t) : -EINVAL;
}

/*	Plugin object	*/
static struct sched_plugin cd_plugin __cacheline_aligned_in_smp = {
	.plugin_name		= "C=D",
	.tick			= cd_tick,
	.task_new		= cd_task_new,
	.complete_job		= complete_job,
	.task_exit		= cd_task_exit,
	.schedule		= cd_schedule,
	.release_at		= cd_release_at,
	.task_wake_up		= cd_task_wake_up,
	.task_block		= cd_task_block,
	.admit_task		= cd_admit_task
};


static int __init init_cd(void)
{
	int i;

	/* FIXME: breaks with CPU hotplug
	 */
	for (i = 0; i < num_online_cpus(); i++) {
		cd_domain_init(remote_domain(i),
			       cd_check_resched,
			       NULL, i);
	}
	return register_sched_plugin(&cd_plugin);
}

module_init(init_cd);

