/*
 * litmus/sched_npsf.c
 *
 * Implementation of the NPS-F scheduling algorithm.
 *
 * A _server_ may span on multiple _reserves_ on different CPUs.
 *
 *                      *                      1
 * +--------------+  +--> +--------------+  +--> +--------------+
 * | cpu_entry_t  |  |    | npsf_reserve |  |    | npsf_server  |
 * +--------------+  |    +--------------+  |    +--------------+
 * |              |1 |    |              |1 |    |              |
 * | cpu_reserve  |--+   1|       server |--+   1|              |
 * |              |   +---| cpu          |   +---| curr_reserve |
 * +--------------+ <-+   +--------------+ <-+   +--------------+
 *                  1                      *
 */

#include <asm/uaccess.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/slab.h>

#include <linux/module.h>

#include <litmus/litmus.h>
#include <litmus/jobs.h>
#include <litmus/sched_plugin.h>
#include <litmus/edf_common.h>

/* Be extra verbose (log spam) */
#define NPSF_VERBOSE

#ifdef NPSF_VERBOSE
#define npsf_printk(fmt, arg...) printk(KERN_INFO fmt, ##arg)
#else
#define npsf_printk(fmt, arg...)
#endif

struct npsf_reserve;

/* cpu_entry_t
 *
 * Each cpu has a list of reserves assigned on the cpu.
 * Each reserve has a pointer to its server (Notional processor)
 * that may be shared among multiple reserves.
 */
typedef struct  {
	/* lock to protect cpu_reserve and list changes */
	raw_spinlock_t		cpu_res_lock;
	/* the reserve currently executing on this cpu */
	struct npsf_reserve	*cpu_reserve;
	/* list of reserves on this cpu */
	struct list_head	npsf_reserves;
	/* cpu ID */
	int 			cpu;
	/* timer to control reserve switching */
	struct hrtimer		timer;
	/* virtual timer expiring (wrt time_origin) */
	lt_t			should_expire;
	/* delegate timer firing to proper cpu */
	struct hrtimer_start_on_info	info;
	/* FIXME: the ids for servers should be an increasing int >=0 */
	int			last_seen_npsf_id;
} cpu_entry_t;

/* one cpu_entry_t per CPU */
DEFINE_PER_CPU(cpu_entry_t, npsf_cpu_entries);

/* This is the "notional processor" (i.e., simple server) abstraction. */
typedef struct npsf_server {
	/* shared among reserves */
	rt_domain_t		dom;
	/* the real-time task that this server *SHOULD* be scheduling */
	struct task_struct	*highest_prio;
	/* current reserve where this dom is executing */
	struct npsf_reserve	*curr_reserve;
	/* The "first" reserve for this server in a time slot.
	 * For non-migrating servers this will always be the same as curr_reserve. */
	struct npsf_reserve *first_reserve;
	/* Prevent a race between the last CPU in a reserve chain an the first. */
	int first_cpu_wants_ipi;
	/* rt_domain_t lock + npsf_server_t lock */
#define lock dom.ready_lock
} npsf_server_t;

typedef struct npsf_reserve {
	/* Pointer to the server for this reserve: a server may be shared among
	 * multiple cpus with different budget per cpu, but same npsf_id. */
	npsf_server_t		*server;
	/* we queue here in npsf_reserves */
	struct list_head	node;
	/* budget of this npsf_id on this cpu */
	lt_t			budget;
	/* cpu for this (portion of) server */
	cpu_entry_t		*cpu;
	/* id of this server, it is the same for the
	 * same server on different cpus */
	int 			npsf_id;
	/* Can be used to identify if a reserve continues
	 * next npsf in the chain, needed for proper server deletion */
	struct npsf_reserve 	*next_npsf;
	/* flag that is true if the reserve is currently scheduled */
	int			is_currently_scheduled;
} npsf_reserve_t;

/* synchronization point to start moving and switching servers only
 * when all servers have been properly set up by the user.
 */
static atomic_t all_servers_added;
static atomic_t timers_activated = ATOMIC_INIT(0);

/* Virtual time starts here */
static lt_t time_origin;

/* save number of online cpus seen at init time */
static unsigned int _online_cpus = 1;

#define no_reserves(entry)	(list_empty(&((entry)->npsf_reserves)))
#define local_entry		(&__get_cpu_var(npsf_cpu_entries))
#define remote_entry(cpu)	(&per_cpu(npsf_cpu_entries, (cpu)))

#define server_from_dom(domain)	(container_of((domain), npsf_server_t, dom))

/* task_entry uses get_partition() therefore we must take care of
 * updating correclty the task_params.cpu whenever we switch task,
 * otherwise we'll deadlock.
 */
#define task_entry(task)	remote_entry(get_partition(task))
#define domain_edf(npsf)	(&((npsf)->server->dom))

#define task_npsfid(task)	((task)->rt_param.task_params.semi_part.npsf_id)

static inline int owns_server(npsf_reserve_t *npsf)
{
	return (npsf->server->curr_reserve == npsf);
}

/* utility functions to get next and prev domains; must hold entry lock */
static inline npsf_reserve_t* local_next_reserve(npsf_reserve_t *curr,
		cpu_entry_t *entry)
{
	return (list_is_last(&curr->node, &entry->npsf_reserves)) ?
		list_entry(entry->npsf_reserves.next, npsf_reserve_t, node) :
		list_entry(curr->node.next, npsf_reserve_t, node);

}

static inline npsf_reserve_t* local_prev_reserve(npsf_reserve_t *curr,
		cpu_entry_t *entry)
{
	return ((curr->node.prev == &entry->npsf_reserves) ?
		list_entry(entry->npsf_reserves.prev, npsf_reserve_t, node) :
		list_entry(curr->node.prev, npsf_reserve_t, node));
}
static void requeue(struct task_struct* t, rt_domain_t *edf)
{
	if (t->state != TASK_RUNNING)
		TRACE_TASK(t, "requeue: !TASK_RUNNING\n");

	BUG_ON(is_queued(t));

	set_rt_flags(t, RT_F_RUNNING);
	if (is_released(t, litmus_clock()))
		__add_ready(edf, t);
	else
		add_release(edf, t); /* it has got to wait */
}

/* we assume the lock is being held */
static void preempt(npsf_reserve_t *npsf)
{
	/* Since we do not support non-preemptable sections,
	 * we don't need to pass in a task. If we call this,
	 * we want the remote CPU to reschedule, no matter what.
	 */
	preempt_if_preemptable(NULL, npsf->cpu->cpu);
}


static void npsf_preempt_if_server_is_scheduled(npsf_server_t* srv)
{
	npsf_reserve_t *reserve = srv->curr_reserve;
	if (reserve->is_currently_scheduled) {
		preempt(reserve);
	}
}

/* assumes lock is held by caller */
static void npsf_reschedule_server(npsf_server_t* srv)
{
	struct task_struct* hp = srv->highest_prio;
	rt_domain_t* edf = &srv->dom;

	if (edf_preemption_needed(edf, hp)) {
		srv->highest_prio = __take_ready(edf);
		if (hp) {
			TRACE_TASK(hp, "requeue: no longer highest prio\n");
			requeue(hp, edf);
		}
		npsf_preempt_if_server_is_scheduled(srv);
	}
}

static void npsf_release_jobs(rt_domain_t* rt, struct bheap* tasks)
{
	npsf_server_t *srv = server_from_dom(rt);
	unsigned long flags;

	raw_spin_lock_irqsave(&srv->lock, flags);

	__merge_ready(rt, tasks);
	npsf_reschedule_server(srv);

	raw_spin_unlock_irqrestore(&srv->lock, flags);
}

static void job_completion(struct task_struct* t, int forced)
{
	sched_trace_task_completion(t, forced);
	TRACE_TASK(t, "job_completion().\n");

	set_rt_flags(t, RT_F_SLEEP);
	prepare_for_next_period(t);
}

/* When did this slot start ? */
static inline lt_t slot_begin(lt_t now)
{
	return (((now - time_origin) / npsf_slot_length)
			* npsf_slot_length + time_origin);
}

/* Compute the delta from the beginning of the current slot. */
static inline lt_t delta_from_slot_begin(lt_t now)
{
	return (now - slot_begin(now));
}

/* Given an offset into a slot, return the corresponding eligible reserve.
 * The output param reservation_end is used to return the (relative) time at which
 * the returned reserve ends.
 */
static npsf_reserve_t* get_reserve_for_offset(cpu_entry_t *entry, lt_t offset,
                                              lt_t *reservation_end)
{
	npsf_reserve_t *tmp;

	*reservation_end = 0;

	/* linear search through all reserves, figure out which one is the last one
	 * to become eligible before delta */
	list_for_each_entry(tmp, &entry->npsf_reserves, node) {
		*reservation_end += tmp->budget;

		/* We are always "late". Found tmp is the right one */
		if ((*reservation_end > offset))
			return tmp;
	}

	/* error: we should never fall of the reserve list */
	BUG();
	return NULL;
}

/* Determine which reserve is eligible based on the current time.
 */
static npsf_reserve_t* get_current_reserve(cpu_entry_t *entry)
{
	lt_t reservation_end;
	lt_t offset = delta_from_slot_begin(litmus_clock());
	return get_reserve_for_offset(entry, offset, &reservation_end);
}

/* This is used to ensure that we are "always" late, i.e., to make
 * sure that the timer jitter is always positive. This should
 * only trigger in KVM (or in real machines with bad TSC drift after
 * an IPI).
 *
 * ATM proper tracing for this event is done in reserve_switch_tick().
 */
static noinline ktime_t catchup_time(lt_t from, lt_t target)
{
	while(lt_before(from, target)) {
		from = litmus_clock();

		mb();
		cpu_relax();
	}

	return ns_to_ktime(from);
}


/* compute the next ABSOLUTE timer value */
static lt_t get_next_reserve_switch_time(void)
{
	cpu_entry_t *entry = local_entry;
	lt_t now        = litmus_clock();
	lt_t slot_start = slot_begin(now);
	lt_t offset     = now - slot_start;
	lt_t next_time;
	npsf_reserve_t* reserve;

	/* compute the absolute litmus time of the next reserve switch */
	reserve = get_reserve_for_offset(entry, offset, &next_time);
	/* get_reserve_for_offset returns a relative start time; let's make it
	   absolute */
	next_time += slot_start;

	/* Let's see if we need to skip the next timer. */
	reserve = local_next_reserve(reserve, entry);
	/* if the next reserve is a continuing reserve
	 * (i.e., if it belongs to a migrating server),
	 * then we skip the timer event because we will
	 * receive an IPI from the previous processor instead. */
	if (reserve->server->first_reserve != reserve) {
		/* it is indeed not the first reserve */
		next_time += reserve->budget;
	}

	return next_time;
}

/* This is the callback for reserve-switching interrupts.
 * The timer is reprogrammed to expire at the beginning of every logical
 * reserve (i.e., a continuing reserve may be split among different CPUs
 * but is a _single_ logical reserve). get_next_reserve_switch_time()
 * will return the right next_expire time.
 */
static enum hrtimer_restart reserve_switch_tick(struct hrtimer *timer)
{
	unsigned long flags;
	cpu_entry_t *entry;
	/* we are using CLOCK_MONOTONIC */
	ktime_t now = ktime_get();
	ktime_t delta;
	int late;

	entry = container_of(timer, cpu_entry_t, timer);
	raw_spin_lock_irqsave(&entry->cpu_res_lock, flags);

	/* jitter wrt virtual time */
	delta = ktime_sub(now, ns_to_ktime(entry->should_expire));
	late = (ktime_to_ns(delta) >= 0) ? 1 : 0;

#ifdef NPSF_VERBOSE
	if (entry->cpu_reserve && atomic_read(&all_servers_added))
		TRACE("(npsf_id: %d) tick starts at %Ld, "
		      "now - should_expire: %Ld\n",
		      entry->cpu_reserve->npsf_id,
		      ktime_to_ns(now), ktime_to_ns(delta));
#endif
	/* if the timer expires earlier than the should_expire time,
	 * we delay the switching until time it's synchronized with
	 * the switch boundary. Otherwise next reserve will execute
	 * longer (wrong).
	 */
	if (!late) {
		TRACE("+++ Timer fired early, waiting...\n");
		now = catchup_time(ktime_to_ns(now), entry->should_expire);

		delta = ktime_sub(now, ns_to_ktime(entry->should_expire));
		TRACE("+++ done, tick restarts at %Ld, "
		      "now - should_expire: %Ld\n",
		      ktime_to_ns(now), ktime_to_ns(delta));
	}

	BUG_ON(!atomic_read(&all_servers_added));
	BUG_ON(no_reserves(entry));

	/* Compute the next time that we need to be notified. */
	entry->should_expire = get_next_reserve_switch_time();

	/* kindly ask the Penguin to let us know... */
	hrtimer_set_expires(timer, ns_to_ktime(entry->should_expire));

	/* set resched flag to reschedule local cpu */
	set_need_resched();

	raw_spin_unlock_irqrestore(&entry->cpu_res_lock, flags);
#ifdef NPSF_VERBOSE
	if (atomic_read(&all_servers_added))
		TRACE("(npsf_id: %d) tick ends at %Ld, should_expire: %llu\n",
		      entry->cpu_reserve->npsf_id, ktime_to_ns(ktime_get()),
		      entry->should_expire);
#endif

	return HRTIMER_RESTART;
}

static void npsf_scheduler_tick(struct task_struct *t)
{
	if (is_realtime(t) && budget_enforced(t) && budget_exhausted(t)) {
		set_tsk_need_resched(t);
		TRACE("npsf_tick: %d is preemptable "
				" => FORCE_RESCHED\n", t->pid);
	}
}

/* Assumption: caller holds srv lock and prev belongs to
 * the currently-scheduled reservation.
 */
static void npsf_schedule_server(struct task_struct* prev,
				 cpu_entry_t *entry)
{
	npsf_server_t* srv = entry->cpu_reserve->server;

	int out_of_time, sleep, exists, blocks;

	exists      = is_realtime(prev);
	blocks      = exists && !is_running(prev);
	out_of_time = exists &&
		budget_enforced(prev) &&
		budget_exhausted(prev);
	sleep	    = exists && get_rt_flags(prev) == RT_F_SLEEP;

	if (exists)
		TRACE_TASK(prev, "(npsf_id %d) blocks:%d "
			   "out_of_time:%d sleep:%d state:%d sig:%d\n",
			   task_npsfid(prev),
			   blocks, out_of_time, sleep,
			   prev->state,
			   signal_pending(prev));

	/* Any task that is preemptable and either exhausts its
	 * execution budget or wants to sleep completes. We may have
	 * to reschedule after this.
	 */
	if ((out_of_time || sleep) && !blocks) {
		job_completion(prev, !sleep);

		if (srv->highest_prio != prev) {
			BUG_ON(!is_queued(prev));
			remove(&srv->dom, prev);
		}

		requeue(prev, &srv->dom);

		if (srv->highest_prio == prev)
			srv->highest_prio = __take_ready(&srv->dom);
	}

	BUG_ON(blocks && prev == srv->highest_prio);
//	BUG_ON(!srv->highest_prio && jobs_pending(&srv->dom));
}

static void npsf_notify_next_cpu(npsf_reserve_t *npsf_prev)
{
	npsf_server_t *srv;

	if (unlikely(npsf_prev->next_npsf != npsf_prev)) {
		/* This reserve is actually shared. Let's update its 'owner'
		 * and notify the next CPU. */
		srv = npsf_prev->server;
		raw_spin_lock(&srv->lock);
		srv->curr_reserve = npsf_prev->next_npsf;
		if (srv->first_reserve != srv->curr_reserve ||
		    srv->first_cpu_wants_ipi) {
			/* send an IPI to notify next CPU in chain */
			srv->first_cpu_wants_ipi = 0;
			TRACE("sending IPI\n");
			preempt(srv->curr_reserve);
		}
		raw_spin_unlock(&srv->lock);
	}
}

static struct task_struct* npsf_schedule(struct task_struct * prev)
{
	npsf_reserve_t *npsf_prev, *npsf_next;
	npsf_server_t *srv_prev, *srv_next;
	cpu_entry_t *entry = local_entry;
	struct task_struct *next;

	int reserve_switch;

	/* servers not ready yet, yield to linux */
	if (!atomic_read(&all_servers_added))
		return NULL;

#ifdef NPSF_VERBOSE
	TRACE_TASK(prev, "schedule\n");
#endif
	raw_spin_lock(&entry->cpu_res_lock);

	BUG_ON(no_reserves(entry));

	/* step 1: what are we currently serving? */
	npsf_prev = entry->cpu_reserve;
	srv_prev  = npsf_prev->server;

	/* step 2: what SHOULD we be currently serving? */
	npsf_next = get_current_reserve(entry);
	srv_next  = npsf_next->server;

	/* TODO second measuring point for IPI receiving
	 * if (!srv_next->measure_wait_IPI) --- the remote reset
	 * 	trace_time_end.
	 */
	raw_spin_lock(&srv_prev->lock);


	/* step 3: update prev server */
	if (is_realtime(prev) && task_npsfid(prev) == entry->cpu_reserve->npsf_id)
		npsf_schedule_server(prev, entry);
	else if (is_realtime(prev))
		TRACE_TASK(prev, "npsf_id %d != cpu_reserve npsf_id %d\n",
				task_npsfid(prev), entry->cpu_reserve->npsf_id);

	/* step 4: determine if we need to switch to another reserve */
	reserve_switch = npsf_prev != npsf_next;

	if (!reserve_switch) {
		/* easy case: just enact what the server scheduler decided */
		next = srv_prev->highest_prio;

		/* Unlock AFTER observing highest_prio to avoid races with
		 * remote rescheduling activity. */
		raw_spin_unlock(&srv_prev->lock);
	} else {
		/* In this case we have a reserve switch.  We are done with the
		 * previous server, so release its lock. */
		TRACE("switch reserve npsf_id %d -> npsf_id %d\n",
				npsf_prev->npsf_id, npsf_next->npsf_id);
		npsf_prev->is_currently_scheduled = 0;
		raw_spin_unlock(&srv_prev->lock);

		/* Move on to the next server. */

		raw_spin_lock(&srv_next->lock);
		npsf_next->is_currently_scheduled = 1;

		/* make sure we are owner of a  server (if it is shared) */
		if (unlikely(srv_next->curr_reserve != npsf_next)) {
			/* We raced with the previous owner.  Let's schedule
			 * the previous reserve for now. The previous owner
			 * will send us an IPI when the server has been pushed
			 * to us.
			 */
			TRACE("(npsf_id %d) raced with previous server owner\n",
			      npsf_next->npsf_id);

			/* check if we are the first CPU, in which case we need
			 * to request a notification explicitly */
			if (srv_next->first_reserve == npsf_next)
				srv_next->first_cpu_wants_ipi = 1;

			npsf_next->is_currently_scheduled = 0;
			raw_spin_unlock(&srv_next->lock);

			/* just keep the previous reserve one more time */
			raw_spin_lock(&srv_prev->lock);

			npsf_prev->is_currently_scheduled = 1;
			/* Note that there is not a race condition here.
			 * Since curr_reserve didn't point yet to this reserve,
			 * so no processor would have observed the one in npsf_next.
			 * A processor might have observed the flag being zero
			 * in npsf_prev and decided not to send an IPI, which
			 * doesn't matter since we are going to reschedule
			 * below anyay. */

			next = srv_prev->highest_prio;

			raw_spin_unlock(&srv_prev->lock);

			/* TODO first measuring point for '0'-switching time
			 * remote is not ready yet and will send us an IPI
			 * when it's done.
			 * local:
			 * 	srv_next->measure_wait_IPI = 1;
			 * remote before sending IPI:
			 * 	if (srv_next->measure_wait_IPI) reset;
			 */
		} else {
			/* invariant: srv->highest_prio is always the
			 * highest-priority job in the server, and it is always
			 * runnable. Any update to the server must maintain
			 * this invariant. */
			next = srv_next->highest_prio;

			entry->cpu_reserve = npsf_next;
			raw_spin_unlock(&srv_next->lock);

			/* send an IPI (if necessary) */
			npsf_notify_next_cpu(npsf_prev);
		}

	}

	if (next) {
		TRACE_TASK(next, "(npsf_id %d) scheduled at %llu\n",
			   task_npsfid(next), litmus_clock());
		set_rt_flags(next, RT_F_RUNNING);
		/* The TASK_RUNNING flag is set by the Penguin _way_ after
		 * activating a task. This dosn't matter much to Linux as
		 * the rq lock will prevent any changes, but it matters to
		 * us. It is possible for a remote cpu waking up this task
		 * to requeue the task before it's runnable, send an IPI here,
		 * we schedule that task (still "not-runnable"), and only
		 * before the real execution of next, the running flag is set.
		 */
		if (!is_running(next))
			TRACE_TASK(next, "BAD: !TASK_RUNNING\n");
	} else {
		/* FIXME npsf_id is wrong if reserve switch but "switching back"
		 * if we race */
		TRACE("(npsf_id %d) becoming idle at %llu\n",
		      reserve_switch ? npsf_next->npsf_id : npsf_prev->npsf_id,
		      litmus_clock());
	}

	raw_spin_unlock(&entry->cpu_res_lock);

	return next;
}

/*	Prepare a task for running in RT mode
 *
 *	We can only be sure that the cpu is a right one (admit checks
 *	against tasks released on a cpu that doesn't host the right npsf_id)
 *	but we _cannot_ be sure that:
 *	1) the found npsf is the reserve currently running on this cpu.
 *	2) the current reserve (the one in charge of scheduling) is not
 *	running on a different cpu.
 */
static void npsf_task_new(struct task_struct * t, int on_rq, int running)
{
	npsf_reserve_t *npsf;
	npsf_server_t *srv;
	cpu_entry_t *entry = task_entry(t);
	rt_domain_t *edf;
	unsigned long flags;

	BUG_ON(no_reserves(entry));

	/* search the proper npsf_server where to add the new task */
	list_for_each_entry(npsf, &entry->npsf_reserves, node) {
		if (npsf->npsf_id == task_npsfid(t))
			break;
	}


	srv = npsf->server;

	/* The task should be running in the queue, otherwise signal
	 * code will try to wake it up with fatal consequences.
	 */
	raw_spin_lock_irqsave(&entry->cpu_res_lock, flags);
	raw_spin_lock(&srv->lock);

	edf = domain_edf(npsf);
	tsk_rt(t)->domain = edf;

	TRACE_TASK(t, "task_new: P%d, task_npsfid %d, "
			"npsf->npsf_id %d, entry->cpu %d\n",
			t->rt_param.task_params.cpu, task_npsfid(t),
			npsf->npsf_id, entry->cpu);

	/* setup job parameters */
	release_at(t, litmus_clock());

	/* There are four basic scenarios that could happen:
	 *  1) the server is on another cpu and scheduled;
	 *  2) the server is on another cpu and not scheduled;
	 *  3) the server is on this cpu and scheduled; and
	 *  4) the server is on this cpu and not scheduled.
	 *
	 * Whatever scenario we're in, it cannot change while we are
	 * holding the server lock.
	 *
	 * If the new task does not have a high priority, then
	 * we can just queue it and be done.
	 *
	 * In theory, the requeue() and reschedule_server() code
	 * take care of all that.
	 */

	requeue(t, edf);
	/* reschedule will cause a remote preemption, if required */
	npsf_reschedule_server(srv);
	/* always preempt to make sure we don't
	 * use the stack if it needs to migrate */
	set_tsk_need_resched(t);

	raw_spin_unlock(&srv->lock);
	raw_spin_unlock_irqrestore(&entry->cpu_res_lock, flags);
}

static void npsf_task_wake_up(struct task_struct *t)
{
	rt_domain_t *edf;
	npsf_server_t* srv;
	unsigned long flags;
	lt_t now;

	BUG_ON(!is_realtime(t));

	edf = tsk_rt(t)->domain;
	srv = server_from_dom(edf);

	raw_spin_lock_irqsave(&srv->lock, flags);

	BUG_ON(is_queued(t));

	now = litmus_clock();
	/* FIXME: this should be a configurable policy... */
	if (is_tardy(t, now)) {
		/* new sporadic release */
		release_at(t, now);
		sched_trace_task_release(t);
	}

	/* Only add to ready queue if it is not the
	 * currently-scheduled task.
	 */
	if (srv->highest_prio != t) {
		requeue(t, edf);
		npsf_reschedule_server(srv);
	}
#ifdef NPSF_VERBOSE
	else
		TRACE_TASK(t, "wake_up, is curr_sched, not requeued\n");
#endif

	raw_spin_unlock_irqrestore(&srv->lock, flags);

	TRACE_TASK(t, "wake up done\n");
}

static void remove_from_server(struct task_struct *t, npsf_server_t* srv)
{
	if (srv->highest_prio == t) {
		TRACE_TASK(t, "remove from server: is highest-prio task\n");
		srv->highest_prio = NULL;
		npsf_reschedule_server(srv);
	} else if (is_queued(t)) {
		TRACE_TASK(t, "remove from server:  removed from queue\n");
		remove(&srv->dom, t);
	}
#ifdef NPSF_VERBOSE
	else
		TRACE_TASK(t, "WARN: where is this task?\n");
#endif
}

static void npsf_task_block(struct task_struct *t)
{
	rt_domain_t *edf;
	npsf_server_t* srv;
	unsigned long flags;

	TRACE_TASK(t, "(npsf_id %d) block at %llu, state=%d\n",
			task_npsfid(t), litmus_clock(), t->state);

	BUG_ON(!is_realtime(t));

	edf = tsk_rt(t)->domain;
	srv = server_from_dom(edf);

	raw_spin_lock_irqsave(&srv->lock, flags);

	remove_from_server(t, srv);

	raw_spin_unlock_irqrestore(&srv->lock, flags);
}

static void npsf_task_exit(struct task_struct * t)
{
	rt_domain_t *edf;
	npsf_server_t* srv;
	unsigned long flags;

	BUG_ON(!is_realtime(t));

	edf = tsk_rt(t)->domain;
	srv = server_from_dom(edf);

	raw_spin_lock_irqsave(&srv->lock, flags);

	remove_from_server(t, srv);

	raw_spin_unlock_irqrestore(&srv->lock, flags);

	TRACE_TASK(t, "RIP, now reschedule\n");
}

static long npsf_admit_task(struct task_struct* tsk)
{
	npsf_reserve_t *npsf;
	cpu_entry_t *entry = task_entry(tsk);
	int id_ok = 0;

	if (!atomic_read(&all_servers_added)) {
		printk(KERN_DEBUG "not all servers added\n");
		return -ENODEV;
	}

	/* check to be on the right cpu and on the right server */
	if (task_cpu(tsk) != tsk->rt_param.task_params.cpu) {
		printk(KERN_DEBUG "wrong CPU(%d, %d, %d) for npsf_id %d\n",
			task_cpu(tsk), tsk->rt_param.task_params.cpu,
			entry->cpu, task_npsfid(tsk));
		return -EINVAL;
	}

	/* 1) this cpu should have the proper npsf_id in the list
	 * 2) the rt_domain for the proper npsf_id is not null
	 */
	list_for_each_entry(npsf, &entry->npsf_reserves, node) {
		if (npsf->npsf_id == task_npsfid(tsk)) {
			id_ok = 1;
			break;
		}
	}
	if (!id_ok)
		printk(KERN_DEBUG "wrong npsf_id (%d) for entry %d\n",
				task_npsfid(tsk), entry->cpu);

	return id_ok ? 0 : -EINVAL;
}

/* in litmus.c */
extern atomic_t rt_task_count;

/* initialization status control */
static int reserves_allocated = 0;

#ifdef NPSF_VERBOSE
static void print_reserve(cpu_entry_t *cpu)
{
	npsf_reserve_t *tmp;

	printk(KERN_INFO "NPS-F: reserves on CPU %d:\n", cpu->cpu);
	list_for_each_entry(tmp, &cpu->npsf_reserves, node) {
		BUG_ON(!tmp->server);
		BUG_ON(!&(tmp->server->dom));
		BUG_ON(tmp->server->highest_prio);
		printk(KERN_INFO "%d: %d us\n", tmp->npsf_id,
				(int)(tmp->budget / 1000));
	}
}
#endif
/*
 * do_add_reserve:	add a reserve(cpu, id, budget)
 *
 * Callback for syscall add_server(); it allows to add the reserve "id"
 * to the CPU "cpu". "budget" is the length of the reserve for the
 * notional processor (server) id on the cpu cpu.
 */
static long do_add_reserve(npsf_reserve_t **new, cpu_entry_t *cpu,
		npsf_server_t *the_dom, int npsf_id, lt_t budget)
{
	unsigned long flags;

	/* npsf_id for each cpu should be given in increasing order,
	 * it doesn't make sense the same np on the same cpu.
	 * The last_seen_npsf_id is reset upon plugin insertion.
	 */
	if (cpu->last_seen_npsf_id >= npsf_id)
		return -EINVAL;

	/* don't allow server changes if there are tasks in the system */
	if (atomic_read(&rt_task_count))
		return -EACCES;

	if ((*new = kmalloc(sizeof(npsf_reserve_t), GFP_ATOMIC)) == NULL)
		return -ENOMEM;

	(*new)->server = the_dom;
	(*new)->npsf_id = npsf_id;
	(*new)->budget = budget;
	(*new)->cpu = cpu;

	npsf_printk("Add npsf_id %d on P%d with budget %llu\n", (*new)->npsf_id,
			(*new)->cpu->cpu, (*new)->budget);

	raw_spin_lock_irqsave(&cpu->cpu_res_lock, flags);

	list_add_tail(&(*new)->node, &cpu->npsf_reserves);
	cpu->last_seen_npsf_id = npsf_id;
	cpu->cpu_reserve = list_first_entry(&cpu->npsf_reserves, npsf_reserve_t, node);

	raw_spin_unlock_irqrestore(&cpu->cpu_res_lock, flags);

	return 0;
}

static void kickoff_timers(void)
{
	int cpu;
	cpu_entry_t *entry;
	lt_t kickoff;

	kickoff = slot_begin(litmus_clock() + npsf_slot_length * 2);

	for_each_online_cpu(cpu) {
		entry = &per_cpu(npsf_cpu_entries, cpu);
		hrtimer_start_on(cpu, &entry->info, &entry->timer,
				 ns_to_ktime(kickoff),
				 HRTIMER_MODE_ABS_PINNED);
		entry->should_expire = kickoff;
	}
	atomic_set(&timers_activated, 1);
}

/* We offer to library a budgets array interface (so we go through the
 * syscall path only once) and we internally cycle on do_add_reserve.
 *
 * last == 1 means that the user is adding the last server and after
 * the insertion the plugin is properly set up. (FIXME it should be
 * done in a better way, but I doubt this plugin will ever go
 * to the master branch).
 */
asmlinkage long sys_add_server(int __user *__id,
		struct npsf_budgets __user *__budgets, int last)
{
	int id, i;
	int ret = -EFAULT;
	struct npsf_budgets *budgets;
	cpu_entry_t *entry;
	npsf_server_t *npsfserver;
	npsf_reserve_t *npsf_reserve_array[NR_CPUS];
	npsf_reserve_t *first_reserve;

	if (_online_cpus != num_online_cpus())
		return ret;

	if (copy_from_user(&id, __id, sizeof(id)))
		return ret;

	budgets = kmalloc(_online_cpus * sizeof(struct npsf_budgets),
			GFP_ATOMIC);

	for (i = 0; i < _online_cpus; i++) {
		budgets[i].cpu = NO_CPU;
		budgets[i].budget = 0;
	}

	if (copy_from_user(budgets, __budgets,
				sizeof(budgets) * _online_cpus))
		goto err;

	/* initialize the npsf_server_t for this npsf_server series */
	npsfserver = kmalloc(sizeof(npsf_server_t), GFP_ATOMIC);
	if (!npsfserver) {
		ret = -ENOMEM;
		goto err;
	}
	edf_domain_init(&npsfserver->dom, NULL, npsf_release_jobs);
	npsfserver->highest_prio = NULL;

	/* initialize all npsf_reserve_t for this server */
	for (i = 0; budgets[i].cpu != NO_CPU && i < _online_cpus; i++) {
		entry = &per_cpu(npsf_cpu_entries, budgets[i].cpu);
		if ((ret = do_add_reserve(&npsf_reserve_array[i], entry,
					npsfserver,
					id, budgets[i].budget)) < 0)
			goto err;
	}
	/* set the current reserve to the first (and possibly unique)
	 * slice for this npsf_id */
	npsfserver->curr_reserve  = npsf_reserve_array[0];
	npsfserver->first_reserve = npsf_reserve_array[0];
	npsfserver->first_cpu_wants_ipi = 0;
	for (i = 0; budgets[i].cpu != NO_CPU && i < _online_cpus; i++) {

		if (i == 0 && budgets[i+1].cpu == NO_CPU) {
			/* Fixed reserve always has itself as next */
			npsf_reserve_array[i]->next_npsf = npsf_reserve_array[i];
		} else if (((i+1) < _online_cpus) &&
				(i > 0 && budgets[i+1].cpu == NO_CPU)) {
			/* Last reserve in the chain has the first reserve as next */
			npsf_reserve_array[i]->next_npsf = npsf_reserve_array[0];
		} else {
			/* Normal continuing reserve */
			npsf_reserve_array[i]->next_npsf = npsf_reserve_array[i+1];
		}
	}
#ifdef NPSF_VERBOSE
	for (i = 0; budgets[i].cpu != NO_CPU && i < _online_cpus; i++) {
		entry = &per_cpu(npsf_cpu_entries, budgets[i].cpu);
		print_reserve(entry);
	}
#endif

	if (last) {
		/* force the first slot switching by setting the
		 * current_reserve to the last server for each cpu.
		 *
		 * FIXME:don't assume there exists at least one reserve per CPU
		 */
		for_each_online_cpu(i) {
			entry = &per_cpu(npsf_cpu_entries, i);
			first_reserve = list_entry(entry->npsf_reserves.next,
						   npsf_reserve_t, node);

			first_reserve->server->curr_reserve = first_reserve;
			entry->cpu_reserve = first_reserve;
			npsf_printk("npsf_id %d is the current reserve "
				    "and server on CPU %d\n",
				    first_reserve->npsf_id, entry->cpu);

		}

		kickoff_timers();

		/* real plugin enable */
		atomic_set(&all_servers_added, 1);
		mb();
	}

	/* at least one server was initialized and may need deletion */
	reserves_allocated = 1;
err:
	kfree(budgets);
	return ret;
}


/* Cancel server_reschedule_tick() hrtimers. Wait for all callbacks
 * to complete. The function is triggered writing 0 as npsf_slot_length.
 */
void npsf_hrtimers_cleanup(void)
{
	int cpu;
	cpu_entry_t *entry;
	int redo;

	if (!atomic_read(&timers_activated))
		return;

	atomic_set(&timers_activated, 0);

	/* prevent the firing of the timer on this cpu */
	do {
		redo = 0;
		for_each_online_cpu(cpu) {
			entry = &per_cpu(npsf_cpu_entries, cpu);

			/* if callback active, skip it for now and redo later */
			if (hrtimer_try_to_cancel(&entry->timer) == -1) {
				redo = 1;
#ifdef NPSF_VERBOSE
				printk(KERN_INFO "(P%d) hrtimer on P%d was "
						"active, try to delete again\n",
						get_cpu(), cpu);
				put_cpu();
#endif
			}
		}
	} while (redo);

	printk(KERN_INFO "npsf hrtimers deleted\n");
}

static void cleanup_npsf(void)
{
	int cpu;
	cpu_entry_t *entry;
	struct list_head *nd, *next;
	npsf_reserve_t *tmp, *tmp_save;

	for_each_online_cpu(cpu) {
		entry = &per_cpu(npsf_cpu_entries, cpu);

		/* FIXME probably not needed as we should be the only cpu
		 * doing the removal */
		raw_spin_lock(&entry->cpu_res_lock);

		list_for_each_safe(nd, next, &entry->npsf_reserves) {
			tmp = list_entry(nd, npsf_reserve_t, node);
			npsf_printk("Del. (id, cpu):(%d, %d)\n",
					tmp->npsf_id,
					tmp->cpu->cpu);
			if (tmp->server) {
				npsf_printk("Del. reserves for npsf_id %d\n",
						tmp->npsf_id);
				tmp_save = tmp;
				while (tmp_save->next_npsf &&
						tmp_save->next_npsf != tmp) {
					tmp_save = tmp_save->next_npsf;
					tmp_save->server = NULL;
				}
				npsf_printk("Freeing server 0x%p\n", tmp->server);
				kfree(tmp->server);
			}
			npsf_printk("Freeing npsf_reserve_t 0x%p\n", tmp);
			kfree(tmp);
		}
		list_del(&entry->npsf_reserves);
		raw_spin_unlock(&entry->cpu_res_lock);
	}
}

/* prevent plugin deactivation if timers are still active */
static long npsf_deactivate_plugin(void)
{
	return (atomic_read(&timers_activated)) ? -1 : 0;
}

static long npsf_activate_plugin(void)
{
	int cpu;
	cpu_entry_t *entry;
	ktime_t now = ktime_get();

	/* prevent plugin switching if timers are active */
	if (atomic_read(&timers_activated))
		return -1;

	atomic_set(&all_servers_added, 0);

	/* de-allocate old servers (if any) */
	if (reserves_allocated)
		cleanup_npsf();

	_online_cpus = num_online_cpus();

	for_each_online_cpu(cpu) {
		entry = &per_cpu(npsf_cpu_entries, cpu);

		raw_spin_lock_init(&entry->cpu_res_lock);

		entry->cpu_reserve = NULL;
		INIT_LIST_HEAD(&entry->npsf_reserves);

		entry->cpu = cpu;
		hrtimer_init(&entry->timer, CLOCK_MONOTONIC,
				HRTIMER_MODE_ABS_PINNED);

		/* initialize (reinitialize) pull timers */
		hrtimer_start_on_info_init(&entry->info);

		entry->timer.function = reserve_switch_tick;
		entry->last_seen_npsf_id = -1;
	}

	printk(KERN_INFO "NPS-F activated: slot length = %lld ns\n",
			npsf_slot_length);

	/* time starts now! */
	time_origin = (lt_t) ktime_to_ns(now);
	TRACE("Time_origin = %llu\n", time_origin);
	return 0;
}

/*	Plugin object	*/
static struct sched_plugin npsf_plugin __cacheline_aligned_in_smp = {
	.plugin_name		= "NPS-F",

	.tick			= npsf_scheduler_tick,
	.task_new		= npsf_task_new,
	.complete_job		= complete_job,
	.task_exit		= npsf_task_exit,
	.schedule		= npsf_schedule,
	.task_wake_up		= npsf_task_wake_up,
	.task_block		= npsf_task_block,
	.admit_task		= npsf_admit_task,
	.activate_plugin	= npsf_activate_plugin,
	.deactivate_plugin	= npsf_deactivate_plugin,
};

static int __init init_npsf(void)
{
	return register_sched_plugin(&npsf_plugin);
}

static void __exit exit_npsf(void)
{
	if (atomic_read(&timers_activated)) {
		atomic_set(&timers_activated, 0);
		return;
	}

	if (reserves_allocated)
		cleanup_npsf();
}

module_init(init_npsf);
module_exit(exit_npsf);

