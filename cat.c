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

struct cat_data {
	int fd;
	int size;
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

int read_file(int fd, struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	struct cat_data *cat_data;
	off_t size, blocks, block;

	size = get_file_size(fd);
	blocks = size / BLOCK_SZ;
	if (size % BLOCK_SZ)
		blocks++;

	cat_data = malloc(sizeof(*cat_data) + blocks * sizeof(struct iovec));
	if (!cat_data) {
		perror("cat_data");
		return -1;
	}

	cat_data->fd = fd;
	cat_data->size = size;

	block = 0;
	while (size > 0) {
		void *buf;

		buf = aligned_alloc(BLOCK_SZ, BLOCK_SZ);
		if (!buf) {
			perror("aligned_alloc");
			return -1;
		}

		cat_data->iovecs[block].iov_len = MIN(size, BLOCK_SZ);
		cat_data->iovecs[block].iov_base = buf;
		block++;
		size -= BLOCK_SZ;
	}

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_readv(sqe, fd, cat_data->iovecs, blocks, 0);
	io_uring_sqe_set_data(sqe, cat_data);
	io_uring_submit(ring);
	return 0;
}

int handle_cqes(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct cat_data *cat_data;
	int blocks;
	int ret, i;

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret) {
		perror("io_uring_wait_cqe");
		return -1;
	}

	cat_data = io_uring_cqe_get_data(cqe);
	blocks = cat_data->size / BLOCK_SZ;
	if (cat_data->size % BLOCK_SZ)
		blocks++;
	for (i = 0; i < blocks; i++) {
		char *p = cat_data->iovecs[i].iov_base;
		while (cat_data->iovecs[i].iov_len--)
			fputc(*p++, stdout);
		free(cat_data->iovecs[i].iov_base);
	}

	io_uring_cqe_seen(ring, cqe);
	return 0;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	int fd, ret, i;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <file[s]>\n", argv[0]);
		return -1;
	}

	io_uring_queue_init(256, &ring, 0);
	for (i = 1; i < argc; i++) {
		fd = open(argv[i], O_RDONLY);
		if (fd < 0) {
			perror("open");
			return -1;
		}

		ret = read_file(fd, &ring);
		if (ret)
			return ret;
		ret = handle_cqes(&ring);
		if (ret)
			return ret;

		close(fd);
	}
}
