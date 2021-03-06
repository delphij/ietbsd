/*
 * Worker thread.
 * (C) 2004 - 2005 FUJITA Tomonori <tomof@acm.org>
 * This code is licenced under the GPL.
 */

#include "iscsi.h"
#include "iscsi_dbg.h"

struct worker_thread_info *worker_thread_pool;

void wthread_queue(struct iscsi_cmnd *cmnd)
{
	struct worker_thread_info *info = cmnd->conn->session->target->wthread_info;

	if (!list_empty(&cmnd->list)) {
		struct iscsi_scsi_cmd_hdr *req = cmnd_hdr(cmnd);
		eprintk("%x %p %x %x %x %x %lx %x\n",
			cmnd_itt(cmnd), req, req->opcode, req->scb[0], cmnd->pdu.datasize,
			be32_to_cpu(req->data_length), cmnd->flags, req->flags);

		if (cmnd->lun)
			eprintk("%u\n", cmnd->lun->lun);
		assert(list_empty(&cmnd->list));
	}

	spin_lock(&info->wthread_lock);
	list_add_tail(&cmnd->list, &info->work_queue);
#ifdef FREEBSD
	set_bit(D_DATA_READY, &info->flags);
#endif
	spin_unlock(&info->wthread_lock);

	atomic_inc(&cmnd->conn->nr_busy_cmnds);

	wake_up(&info->wthread_sleep);
}

static struct iscsi_cmnd * get_ready_cmnd(struct worker_thread_info *info)
{
	struct iscsi_cmnd *cmnd = NULL;

	spin_lock(&info->wthread_lock);
	if (!list_empty(&info->work_queue)) {
		cmnd = list_entry(info->work_queue.next, struct iscsi_cmnd, list);
		list_del_init(&cmnd->list);

		assert(cmnd->conn);
	}
	spin_unlock(&info->wthread_lock);

	return cmnd;
}

static int cmnd_execute(struct iscsi_cmnd *cmnd)
{
	int type = cmnd->conn->session->target->trgt_param.target_type;

	assert(target_type_array[type]->execute_cmnd);
	return target_type_array[type]->execute_cmnd(cmnd);
}

#ifdef LINUX
static int worker_thread(void *arg)
{
	struct worker_thread *wt = (struct worker_thread *) arg;
	struct worker_thread_info *info = wt->w_info;
	struct iscsi_cmnd *cmnd;
	struct iscsi_conn *conn;

	DECLARE_WAITQUEUE(wait, current);

	if (current->io_context)
		put_io_context(current->io_context);

	if (!info->wthread_ioc)
		info->wthread_ioc = get_task_io_context(current, GFP_KERNEL, -1);
	ioc_task_link(info->wthread_ioc);
	current->io_context = info->wthread_ioc;

	add_wait_queue(&info->wthread_sleep, &wait);

	__set_current_state(TASK_RUNNING);
	do {
		while (!list_empty(&info->work_queue) &&
		       (cmnd = get_ready_cmnd(info))) {
			conn = cmnd->conn;
			if (cmnd_tmfabort(cmnd))
				cmnd_release(cmnd, 1);
			else
				cmnd_execute(cmnd);
			assert(conn);
			atomic_dec(&conn->nr_busy_cmnds);
		}

		set_current_state(TASK_INTERRUPTIBLE);
		if (list_empty(&info->work_queue))
			schedule();

		__set_current_state(TASK_RUNNING);
	} while (!kthread_should_stop());

	remove_wait_queue(&info->wthread_sleep, &wait);

	return 0;
}
#else
static void worker_thread(void *arg)
{
	struct worker_thread *wt = (struct worker_thread *) arg;
	struct worker_thread_info *info = wt->w_info;
	struct iscsi_cmnd *cmnd;
	struct iscsi_conn *conn;

	for (;;) {
		wait_event_interruptible(info->wthread_sleep, test_and_clear_bit(D_DATA_READY, &info->flags) || test_bit(D_THR_EXIT, &wt->w_flags));
		while (!list_empty(&info->work_queue) &&
		       (cmnd = get_ready_cmnd(info))) {
			conn = cmnd->conn;
			if (cmnd_tmfabort(cmnd))
				cmnd_release(cmnd, 1);
			else
				cmnd_execute(cmnd);
			assert(conn);
			atomic_dec(&conn->nr_busy_cmnds);
		}

		if (test_bit(D_THR_EXIT, &wt->w_flags))
			break;
	}
	kproc_exit(0);
}
#endif

static int start_one_worker_thread(struct worker_thread_info *info, u32 tid)
{
	struct worker_thread *wt;
	int err;

	if (!(wt = kmalloc(sizeof(struct worker_thread), GFP_KERNEL)))
		return -ENOMEM;

	wt->w_info = info;
	wt->w_flags = 0;
	err = kernel_thread_create(worker_thread, wt, wt->w_task, "istiod%d", tid);
	if (err != 0)
		return err;

	list_add(&wt->w_list, &info->wthread_list);
	info->nr_running_wthreads++;

#ifdef LINUX
	wake_up_process(wt->w_task);
#endif

	return 0;
}

static int stop_one_worker_thread(struct worker_thread *wt)
{
	struct worker_thread_info *info = wt->w_info;
	int err;

	assert(wt->w_task);
#ifdef LINUX
	err = kthread_stop(wt->w_task);
#else
	mtx_lock(&info->wthread_sleep.chan_lock);
	set_bit(D_THR_EXIT, &wt->w_flags);
	wakeup(&info->wthread_sleep);
	msleep(wt->w_task, &info->wthread_sleep.chan_lock, 0, "wait for wthread exit", 0);
	mtx_unlock(&info->wthread_sleep.chan_lock);
	err = 0;
#endif

	if (err < 0 && err != -EINTR)
		return err;

	list_del(&wt->w_list);
	kfree(wt);
	info->nr_running_wthreads--;

	return 0;
}

int wthread_init(struct worker_thread_info *info)
{
	spin_lock_init(&info->wthread_lock);

	info->nr_running_wthreads = 0;
	info->wthread_ioc = NULL;
	info->flags = 0;

	INIT_LIST_HEAD(&info->work_queue);
	INIT_LIST_HEAD(&info->wthread_list);

	init_waitqueue_head(&info->wthread_sleep);

	return 0;
}

int wthread_start(struct worker_thread_info *info, int wthreads, u32 tid)
{
	int err = 0;

	while (info->nr_running_wthreads < wthreads) {
		if ((err = start_one_worker_thread(info, tid)) < 0) {
			eprintk("Fail to create a worker thread %d\n", err);
			goto out;
		}
	}

	while (info->nr_running_wthreads > wthreads) {
		struct worker_thread *wt;
		wt = list_entry(info->wthread_list.next, struct worker_thread, w_list);
		if ((err = stop_one_worker_thread(wt)) < 0) {
			eprintk("Fail to stop a worker thread %d\n", err);
			break;
		}
	}
out:
	return err;
}

int wthread_stop(struct worker_thread_info *info)
{
	struct worker_thread *wt, *tmp;
	int err = 0;

	list_for_each_entry_safe(wt, tmp, &info->wthread_list, w_list) {
		if ((err = stop_one_worker_thread(wt)) < 0) {
			eprintk("Fail to stop a worker thread %d\n", err);
			return err;
		}
	}

	return err;
}

int wthread_module_init()
{
	int err;

	if (!worker_thread_pool_size)
		return 0;

	worker_thread_pool = kmalloc(sizeof(struct worker_thread_info),
				     GFP_KERNEL);
	if (!worker_thread_pool)
		return -ENOMEM;

	wthread_init(worker_thread_pool);

	err = wthread_start(worker_thread_pool, worker_thread_pool_size, 0);
	if (err) {
		kfree(worker_thread_pool);
		worker_thread_pool = NULL;
		return err;
	}

	iprintk("iscsi_trgt using worker thread pool; size = %ld\n",
		worker_thread_pool_size);

	return 0;
}

void wthread_module_exit()
{
	if (!worker_thread_pool_size)
		return;

	wthread_stop(worker_thread_pool);
	kfree(worker_thread_pool);
}
