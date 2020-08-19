#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <liburing.h>

#include <string>
#include <iostream>
using namespace std;

#define QUEUE_DEPTH	64
#define BLOCK_SIZE	8192
#define BUFFER_SIZE	8192

// N.B. this makes cp0 appreciably faster than cp1, and
// even when set to block size cp0 is still faster (with
// size tested).
//#define BUFFER_SIZE	(BLOCK_SIZE * QUEUE_DEPTH)

#define handle_error(m) \
	do { perror(m); exit(-1); } while (0);

// Normal read/write.
int cp0(int ifd, int ofd, off_t sz) {
	uint8_t *buffer = (uint8_t *)malloc(BUFFER_SIZE);
	ssize_t nsz = BUFFER_SIZE;
	
	while (sz) {
		ssize_t n = read(ifd, &buffer[BUFFER_SIZE - nsz], nsz);
		
		// Partial read (signal?), complete the block.
		if (n < nsz) {
			nsz -= n;
			continue;
		}

		// Complete read.
		if (n == nsz) {
			ssize_t wnsz = BLOCK_SIZE;
			while (wnsz) {
				ssize_t wn = write(ofd, &buffer[BUFFER_SIZE - wnsz], wnsz);
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
			nsz = BUFFER_SIZE;
		}
		sz -= n;
	}

	free(buffer);
	return 0;
}

struct io_buffer {
	void *buffer[QUEUE_DEPTH];
	ssize_t length[QUEUE_DEPTH];
	struct iovec iovec[QUEUE_DEPTH];
};

// Vectored read/write.
// Three options:
//   1. Redo window on interrupt;
//   2. Force returned data through, potentially aligning to blocksize;
//   2. Force rest of current window through with retry.
// For full throughput would need two threads.
// In its current form this is not any different than just using a large
// read buffer.
int cp1(int ifd, int ofd, off_t sz) {
	// Setup IO vectors.
	struct iovec vect[QUEUE_DEPTH];
	for (int i = 0; i < QUEUE_DEPTH; i++) {
		vect[i].iov_base = malloc(BLOCK_SIZE);
		vect[i].iov_len = BLOCK_SIZE;
	}

	ssize_t nsz = BLOCK_SIZE * QUEUE_DEPTH;
	bool dirty = 0;

	// Do copy.
	// TODO: Test with signal/fault injection.
	while (sz) {
		// Enqueue as many read vectors as possible.
		ssize_t n = readv(ifd, vect, QUEUE_DEPTH);
		ssize_t wnsz = n;
		
		// Calculate quantity of blocks read and remainder.
		// TODO: This will mess up if we inherit a partial block from the
		// last loop. A partial read will always be in vect[0].
		ssize_t nblocks = n / BLOCK_SIZE;
		ssize_t nshort = n % BLOCK_SIZE;

		// Set the lengths on the vectors for writev.
		if (nblocks < QUEUE_DEPTH) {
			for (int i = 0; i < QUEUE_DEPTH; i++) {
				if (i <= nblocks)
					{ /* Do nothing, full block. */ }
				else if (i == nblocks + 1)
					{ /* The partial block. */ vect[i].iov_len = nshort; }
				else if (i > nblocks)
					{ /* Unfilled blocks. */ vect[i].iov_len = 0; }
			}
			dirty = true;
		}

		int npartial = -1;
		void *basepartial = NULL;
		while (wnsz) {
			// Restore vect's iov_base if necessary.
			if (npartial > 0) {
				vect[npartial].iov_base = basepartial;
				vect[npartial].iov_len = BLOCK_SIZE;
				npartial = -1;
			}

			// Enqueue as many write vectors as possible.
			// TODO: Ensure doesn't blow up.
			ssize_t wn = writev(ofd, vect, QUEUE_DEPTH);

			// Calculate quantity of blocks written and remainder.
			// TODO: This will mess up if we inherit a partial block from
			// the last loop. A partial write will walk upwards from the first
			// iovec.
			ssize_t wnblocks = wn / BLOCK_SIZE;
			ssize_t wnshort = wn % BLOCK_SIZE;

			// Check wn. If less than n, attempt to change vect containing
			// next byte's iov_base. iov_base must be saved and restored.
			if (wn < n) {
				npartial = wnblocks + 1;
				basepartial = vect[npartial].iov_base;

				vect[npartial].iov_base += BLOCK_SIZE - wnshort;
				vect[npartial].iov_len = wnshort;
			}

			// Restart skipped bytes on next iteration.
			wnsz -= wn;
		}
	
		// Partial read (signal?), set the first iovec to the remainder of
		// the last block to ensure all reads are block-aligned. The resumed
		// read *should* read out the last block, already in kernel memory.
		if (n < nsz) {
			vect[0].iov_len = nshort;
		}
		// Full read, ensure everything is restored.
		// TODO: Ensure this doesn't run unless necessary.
		else if (dirty && n == nsz) {
			for (int i = 0; i < QUEUE_DEPTH; i++)
				vect[i].iov_len = BLOCK_SIZE;
			dirty = false;
		}

		sz -= n;
	}


	for (int i = 0; i < QUEUE_DEPTH; i++)
		free(vect[i].iov_base);
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


	//cp0(ifd, ofd, sz);
	cp1(ifd, ofd, sz);


	io_uring_queue_exit(&ring);
	close(ifd);
	close(ofd);
}
