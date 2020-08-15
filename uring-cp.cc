#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <liburing.h>

#include <string>
#include <iostream>
using namespace std;

#define handle_error(m) \
	do { perror(m); exit(-1); } while (0);

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

	// Setup.
	int ent = 64;
	struct io_uring ring;
	if (io_uring_queue_init(ent, &ring, 0) < 0)
		handle_error("io_uring_queue_init");

	// Get file size.
	struct stat st;
	off_t sz;
	if (fstat(ifd, &st) < 0)
		handle_error("fstat");
	sz = st.st_size;
	printf("input is %ld bytes\n", sz);

	// Copy file.
	off_t offset;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;



	io_uring_queue_exit(&ring);
	close(ifd);
	close(ofd);
}
