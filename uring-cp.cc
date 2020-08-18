#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <liburing.h>

#include <string>
#include <iostream>
using namespace std;

#define QUEUE_DEPTH	64
#define BLOCK_SIZE	8192

#define handle_error(m) \
	do { perror(m); exit(-1); } while (0);

// Normal read/write.
int cp0(int ifd, int ofd, off_t sz) {
	uint8_t *buffer = (uint8_t *)malloc(BLOCK_SIZE);
	ssize_t nsz = BLOCK_SIZE;
	
	while (sz) {
		ssize_t n = read(ifd, &buffer[BLOCK_SIZE - nsz], nsz);
		// Retry.
		if (-EAGAIN == n)
			continue;

		// Partial read (signal?), complete the block.
		if (n < nsz) {
			nsz -= n;
			continue;
		}

		// Complete read.
		if (n == nsz) {
			ssize_t wn = BLOCK_SIZE, wnsz = BLOCK_SIZE;
			while (wnsz) {
				ssize_t wn = write(ofd, &buffer[BLOCK_SIZE - wnsz], wnsz);
				// Retry.
				if (-EAGAIN == wn)
					continue;

				// Partial write (signal?), complete the block.
				if (wn < wnsz) {
					wnsz -= wn;
					continue;
				}

				// Complete write.
				if (wn == wnsz)
					wnsz -= wn;
			}
			nsz -= n;
			assert(0 == nsz);
			nsz = BLOCK_SIZE;
		}
		sz -= n;
	}

	free(buffer);
	return 0;
}

// Vectored read/write.
int cp1(int ifd, int ofd, off_t sz) {
	return 0;
}

// Vectored read/write with io_uring.
int cp2(int ifd, int ofd, off_t sz) {
	return 0;
}

int main(int argc, char *argv[]) {
	if (argc < 3) {
		puts("uring-cp a b");
		exit(-1);
	}

	int ifd, ofd;
	ifd = open(argv[1], O_RDONLY);
	ofd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (ifd < 0) { puts("open ifd failed"); exit(-1); }
	if (ofd < 0) { puts("open ofd failed"); exit(-1); }

	// Get file size.
	struct stat st;
	off_t sz;
	if (fstat(ifd, &st) < 0)
		handle_error("fstat");
	sz = st.st_size;
	printf("input is %ld bytes\n", sz);

	// Setup.
	int ent = 64;
	struct io_uring ring;
	if (io_uring_queue_init(ent, &ring, 0) < 0)
		handle_error("io_uring_queue_init");

	// Copy file.
	off_t offset;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;


	cp0(ifd, ofd, sz);


	io_uring_queue_exit(&ring);
	close(ifd);
	close(ofd);
}
