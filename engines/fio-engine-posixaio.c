/*
 * posix aio io engine
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include "fio.h"
#include "os.h"

struct posixaio_data {
	struct io_u **aio_events;
};

static int fill_timespec(struct timespec *ts)
{
#ifdef _POSIX_TIMERS
	if (!clock_gettime(CLOCK_MONOTONIC, ts))
		return 0;

	perror("clock_gettime");
#endif
	return 1;
}

static unsigned long long ts_utime_since_now(struct timespec *t)
{
	long long sec, nsec;
	struct timespec now;

	if (fill_timespec(&now))
		return 0;
	
	sec = now.tv_sec - t->tv_sec;
	nsec = now.tv_nsec - t->tv_nsec;
	if (sec > 0 && nsec < 0) {
		sec--;
		nsec += 1000000000;
	}

	sec *= 1000000;
	nsec /= 1000;
	return sec + nsec;
}

static int fio_posixaio_sync(struct thread_data *td)
{
	return fsync(td->fd);
}

static int fio_posixaio_cancel(struct thread_data *td, struct io_u *io_u)
{
	int r = aio_cancel(td->fd, &io_u->aiocb);

	if (r == 1 || r == AIO_CANCELED)
		return 0;

	return 1;
}

static int fio_posixaio_prep(struct thread_data *td, struct io_u *io_u)
{
	struct aiocb *aiocb = &io_u->aiocb;

	aiocb->aio_fildes = td->fd;
	aiocb->aio_buf = io_u->buf;
	aiocb->aio_nbytes = io_u->buflen;
	aiocb->aio_offset = io_u->offset;

	io_u->seen = 0;
	return 0;
}

static int fio_posixaio_getevents(struct thread_data *td, int min, int max,
				  struct timespec *t)
{
	struct posixaio_data *pd = td->io_ops->data;
	struct list_head *entry;
	struct timespec start;
	int r, have_timeout = 0;

	if (t && !fill_timespec(&start))
		have_timeout = 1;

	r = 0;
restart:
	list_for_each(entry, &td->io_u_busylist) {
		struct io_u *io_u = list_entry(entry, struct io_u, list);
		int err;

		if (io_u->seen)
			continue;

		err = aio_error(&io_u->aiocb);
		switch (err) {
			default:
				io_u->error = err;
			case ECANCELED:
			case 0:
				pd->aio_events[r++] = io_u;
				io_u->seen = 1;
				break;
			case EINPROGRESS:
				break;
		}

		if (r >= max)
			break;
	}

	if (r >= min)
		return r;

	if (have_timeout) {
		unsigned long long usec;

		usec = (t->tv_sec * 1000000) + (t->tv_nsec / 1000);
		if (ts_utime_since_now(&start) > usec)
			return r;
	}

	/*
	 * hrmpf, we need to wait for more. we should use aio_suspend, for
	 * now just sleep a little and recheck status of busy-and-not-seen
	 */
	usleep(1000);
	goto restart;
}

static struct io_u *fio_posixaio_event(struct thread_data *td, int event)
{
	struct posixaio_data *pd = td->io_ops->data;

	return pd->aio_events[event];
}

static int fio_posixaio_queue(struct thread_data fio_unused *td,
			      struct io_u *io_u)
{
	struct aiocb *aiocb = &io_u->aiocb;
	int ret;

	if (io_u->ddir == DDIR_READ)
		ret = aio_read(aiocb);
	else
		ret = aio_write(aiocb);

	if (ret)
		io_u->error = errno;
		
	return io_u->error;
}

static void fio_posixaio_cleanup(struct thread_data *td)
{
	struct posixaio_data *pd = td->io_ops->data;

	if (pd) {
		free(pd->aio_events);
		free(pd);
		td->io_ops->data = NULL;
	}
}

static int fio_posixaio_init(struct thread_data *td)
{
	struct posixaio_data *pd = malloc(sizeof(*pd));

	pd->aio_events = malloc(td->iodepth * sizeof(struct io_u *));

	td->io_ops->data = pd;
	return 0;
}

struct ioengine_ops ioengine = {
	.name		= "posixaio",
	.version	= FIO_IOOPS_VERSION,
	.init		= fio_posixaio_init,
	.prep		= fio_posixaio_prep,
	.queue		= fio_posixaio_queue,
	.cancel		= fio_posixaio_cancel,
	.getevents	= fio_posixaio_getevents,
	.event		= fio_posixaio_event,
	.cleanup	= fio_posixaio_cleanup,
	.sync		= fio_posixaio_sync,
};
