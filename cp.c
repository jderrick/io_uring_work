/*
 * This is file synchronous because it waits to get all read blocks before
 * starting the write. This is the correct flow because you wouldn't want to
 * write without having the full file read. However the submission queues are
 * allowed to be asynchronous, so the I/O vectors can be non-sequential.
 *
 * Note: This doesn't work on large files. IOV_MAX limits to 1024 * 4KB = 4MB.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <unistd.h>
#include <math.h>
#include <fcntl.h>
#include <liburing.h>
#include <liburing/io_uring.h>

#define BLOCK_SZ 4096

struct cp_data {
	int infd;
	int outfd;
	int size;
	int blocks;
	struct iovec iovecs[];
};

off_t get_file_size(int fd) {
	struct stat st;

	if (fstat(fd, &st) < 0) {
		perror("fstat");
		return -1;
	}

	if (S_ISBLK(st.st_mode)) {
		unsigned long long bytes;

		if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
			perror("ioctl");
			return -1;
		}

		return bytes;
	} else if (S_ISREG(st.st_mode))
		return st.st_size;

	return -1;
}

int read_file(int infd, int outfd, struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	struct cp_data *cp_data;
	off_t size, blocks, block;

	size = get_file_size(infd);
	blocks = size / BLOCK_SZ;
	if (size % BLOCK_SZ)
		blocks++;

	cp_data = malloc(sizeof(*cp_data) + blocks * sizeof(struct iovec));
	if (!cp_data) {
		perror("cp_data");
		return -1;
	}

	cp_data->infd = infd;
	cp_data->outfd = outfd;
	cp_data->size = size;
	cp_data->blocks = blocks;

	block = 0;
	while (size > 0) {
		void *buf;

		buf = aligned_alloc(BLOCK_SZ, BLOCK_SZ);
		if (!buf) {
			perror("aligned_alloc");
			return -1;
		}

		cp_data->iovecs[block].iov_len = MIN(size, BLOCK_SZ);
		cp_data->iovecs[block].iov_base = buf;
		block++;
		size -= BLOCK_SZ;
	}

	sqe = io_uring_get_sqe(ring);
	sqe->flags = IOSQE_ASYNC;
	io_uring_prep_readv(sqe, infd, cp_data->iovecs, blocks, 0);
	io_uring_sqe_set_data(sqe, cp_data);
	io_uring_submit(ring);
	return 0;
}

int write_file(struct cp_data *cp_data, struct io_uring *ring)
{
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(ring);
	sqe->flags = IOSQE_ASYNC;
	io_uring_prep_writev(sqe, cp_data->outfd, cp_data->iovecs,
			     cp_data->blocks, 0);
	io_uring_sqe_set_data(sqe, cp_data);
	io_uring_submit(ring);
}

int handle_read_cqes(struct io_uring *ring, struct io_uring *write_ring)
{
	struct io_uring_cqe *cqe;
	struct cp_data *cp_data;
	int ret, i;

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret) {
		perror("io_uring_wait_cqe");
		return -1;
	}
	if (cqe->res < 0) {
		fprintf(stderr, "cqe->res: %d\n", cqe->res);
		return -1;
	}

	cp_data = io_uring_cqe_get_data(cqe);
	io_uring_cqe_seen(ring, cqe);

	write_file(cp_data, write_ring);
	return 0;
}

int handle_write_cqes(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct cp_data *cp_data;
	int ret, i;

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret) {
		perror("io_uring_wait_cqe");
		return -1;
	}

	cp_data = io_uring_cqe_get_data(cqe);
	for (i = 0; i < cp_data->blocks; i++)
		free(cp_data->iovecs[i].iov_base);

	io_uring_cqe_seen(ring, cqe);
	return 0;
}

int main(int argc, char *argv[])
{
	struct io_uring read_ring, write_ring;
	struct stat st;
	int infd, outfd, ret;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <file1> <file2>\n", argv[0]);
		return -1;
	}

	ret = io_uring_queue_init(256, &read_ring, 0);
	if (ret < 0) {
		perror("io_uring_queue_init");
		return -1;
	}
	ret = io_uring_queue_init(256, &write_ring, 0);
	if (ret < 0) {
		perror("io_uring_queue_init");
		return -1;
	}

	infd = open(argv[1], O_RDONLY);
	if (infd < 0) {
		perror("open");
		return -1;
	}
	if (fstat(infd, &st) < 0) {
		perror("stat");
		return -1;
	}

	outfd = open(argv[2], O_RDWR | O_CREAT | O_ASYNC | O_DIRECT, st.st_mode);
	if (outfd < 0) {
		perror("open");
		return -1;
	}

	ret = read_file(infd, outfd, &read_ring);
	if (ret)
		return ret;
	ret = handle_read_cqes(&read_ring, &write_ring);
	if (ret)
		return ret;
	ret = handle_write_cqes(&write_ring);
	if (ret)
		return ret;

	close(infd);
	close(outfd);
}
