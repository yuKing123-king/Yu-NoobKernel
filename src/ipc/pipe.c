#include <ipc/pipe.h>
#include <fs/file.h>
#include <fs/fd_table.h>
#include <sync/wait.h>
#include <mm/kalloc.h>
#include <mm/vm.h>
#include <task/proc.h>
#include <sync/barrier.h>
#include <misc/errno.h>

#define PIPE_SIZE 4096

struct pipe {
	char buf[PIPE_SIZE];
	int rd;
	int wr;
	int count;
	int nreaders;
	int nwriters;
	struct wait_queue rq;
	struct wait_queue wq;
	spinlock_t lock;
};

static struct file_operations pipe_fops;

static struct pipe *get_pipe(struct file *f)
{
	return (struct pipe *)f->f_private;
}

static int pipe_is_read_end(struct file *f)
{
	return f->f_flags & O_RDONLY;
}

ssize_t pipe_read(struct file *f, void *ubuf, size_t count, loff_t *pos)
{
	struct pipe *p = get_pipe(f);
	struct proc *cur = thiscpu()->proc;
	int total = 0;

	(void)pos;

	if (count == 0)
		return 0;

	for (;;) {
		spinlock_acquire(&p->lock);
		if (p->count > 0)
			break;
		if (p->nwriters == 0) {
			spinlock_release(&p->lock);
			return 0;
		}
		wait_queue_sleep_locked(&p->rq, cur, &p->lock);
	}

	while (total < (int)count) {
		if (p->count == 0) {
			if (p->nwriters == 0)
				break;
			break;
		}

		char c = p->buf[p->rd];
		p->rd = (p->rd + 1) % PIPE_SIZE;
		p->count--;

		((char *)ubuf)[total] = c;
		total++;
	}

	spinlock_release(&p->lock);

	if (total > 0)
		wait_queue_wakeup_one(&p->wq);

	return total;
}
ssize_t pipe_write(struct file *f, const void *ubuf, size_t count, loff_t *pos)
{
	struct pipe *p = get_pipe(f);
	struct proc *cur = thiscpu()->proc;
	int total = 0;

	(void)pos;

	if (count == 0)
		return 0;

	if (p->nreaders == 0) {
		return -EPIPE;
	}

	while (total < (int)count) {
		spinlock_acquire(&p->lock);

		if (p->count == PIPE_SIZE) {
			if (p->nreaders == 0) {
				spinlock_release(&p->lock);
				return -EPIPE;
			}
			wait_queue_sleep_locked(&p->wq, cur, &p->lock);
			spinlock_acquire(&p->lock);
			if (p->count == PIPE_SIZE && p->nreaders == 0) {
				spinlock_release(&p->lock);
				return total > 0 ? total : -EPIPE;
			}
		}

		char c = ((const char *)ubuf)[total];

		p->buf[p->wr] = c;
		p->wr = (p->wr + 1) % PIPE_SIZE;
		p->count++;
		total++;

		spinlock_release(&p->lock);
		wait_queue_wakeup_one(&p->rq);
	}

	return total;
}

int pipe_release(struct inode *inode, struct file *f)
{
	(void)inode;

	struct pipe *p = get_pipe(f);
	if (!p)
		return 0;

	spinlock_acquire(&p->lock);

	if (pipe_is_read_end(f)) {
		p->nreaders--;
		wait_queue_wakeup_all(&p->wq);
	} else {
		p->nwriters--;
		wait_queue_wakeup_all(&p->rq);
	}

	if (p->nreaders == 0 && p->nwriters == 0) {
		kfree(p);
		f->f_private = NULL;
	}

	spinlock_release(&p->lock);
	return 0;
}

int pipe_create(struct file *rf, struct file *wf)
{
	struct pipe *p = kzalloc(sizeof(*p));
	if (!p)
		return -ENOMEM;

	p->rd = 0;
	p->wr = 0;
	p->count = 0;
	p->nreaders = 1;
	p->nwriters = 1;
	wait_queue_init(&p->rq);
	wait_queue_init(&p->wq);
	p->lock = SPINLOCK_INITIALIZER("pipe");

	rf->f_op = &pipe_fops;
	rf->f_private = p;
	rf->f_mode = S_IFIFO;
	rf->f_flags = O_RDONLY;

	wf->f_op = &pipe_fops;
	wf->f_private = p;
	wf->f_mode = S_IFIFO;
	wf->f_flags = O_WRONLY;

	return 0;
}

static struct file_operations pipe_fops = {
	.read = pipe_read,
	.write = pipe_write,
	.release = pipe_release,
};
