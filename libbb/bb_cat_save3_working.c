/* vi: set sw=4 ts=4: */
/*
 * Copyright (C) 2003  Manuel Novoa III  <mjn3@codepoet.org>
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */
//kbuild:lib-y += bb_cat.o

#include "libbb.h"
#include <sys/stat.h>
#include "../liburing/include/liburing.h"

#define ENTRIES 1024
#define BLOCK_SZ 1024

struct file_info {
    int file_sz;
    struct iovec iovecs[];
};

int get_file_size(int fd) 
{
    struct stat st;
    if(fstat(fd, &st) < 0) 
	{
        perror("fstat");
        return -1;
    }

    if (S_ISBLK(st.st_mode)) 
	{
        unsigned long long bytes;
        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) 
		{
            perror("ioctl");
            return -1;
        }
        return bytes;
    } 
	else if (S_ISREG(st.st_mode)) return st.st_size;

    return -1;
}

int FAST_FUNC bb_cat(char **argv)
{
	int fd;
	int retval = EXIT_SUCCESS;

	int nrOfFiles = 0;

	//maximal Anzahl (ENTRIES) von files die konkateniert werden können, wenn wir darauf warten, dass alle befehle fertig werden ?!
	//Mal sequentiell (IOSQE_IO_LINK) versuchen. Files sollen ja auch in angegebener Reihenfolge konkateniert werden.

	if (!*argv)
		argv = (char**) &bb_argv_dash;

	//Alle an cat übergebenen files öffnen und als sqe in uring-Struktur übergeben
	do 
	{
		fd = open_or_warn_stdin(*argv);
		if (fd >= 0)
		{
			nrOfFiles++;

			int ret = read_write_with_uring(fd);

			if (ret)
			{
				fprintf(stderr, "Error reading/writing file: %s\n", *argv);

				return 1;
			}

			close(fd);
		}
		else
		{
			retval = EXIT_FAILURE;
		}
	}while (*++argv);

	//* 2, weil pro file zwei sqe (lesen und schreiben)


	//do {
	//	fd = open_or_warn_stdin(*argv);
	//	if (fd >= 0) {
	//		/* This is not a xfunc - never exits */
	//		off_t r = bb_copyfd_eof(fd, STDOUT_FILENO);
	//		if (fd != STDIN_FILENO)
	//			close(fd);
	//		if (r >= 0)
	//			continue;
	//	}
	//	retval = EXIT_FAILURE;
	//} while (*++argv);

	//free memory for uring structure

	return retval;
}

int read_write_with_uring(int fd)
{
	struct io_uring ring;
	struct io_uring_sqe* sqe;
	struct iovec* iovecs;
	struct iovec iov2;
	void* buf;
	void* buf2;
	
	int nr_of_bytes;
	int ret;
	int file_sz = get_file_size(fd);
	int bytes_remaining = file_sz;
	int offset = 0;
	int current_block = 0;
	int total_nr_blocks = (int)file_sz / BLOCK_SZ;
	int nr_of_blocks = 0;
	int blocks_remaining;
	int temp;
	int fd2;

	bool first = true;

	//Zusätzlichen Block für Rest
	if (file_sz % BLOCK_SZ) total_nr_blocks++;

	struct file_info *fi = malloc(sizeof(*fi) + sizeof(struct iovec));
	


	if (posix_memalign(&buf, BLOCK_SZ, BLOCK_SZ))
	{
		perror("Error on posix_memalign");
		return 1;
	}

	if (posix_memalign(&buf2, BLOCK_SZ, BLOCK_SZ))
	{
		perror("Error on posix_memalign2");
		return 1;
	}

	//Bei riesen Datei Würde Queue auch seeehr viele Einträge benötigen. Daher aufteilen sinnvoll, auch wenn wir zwischendurch immer wieder zurückkehren?!
	//Maximal ENTRIES Einträge. Wenn Datei Größer als ENTRIES/2 * BLOCK_SZ ("/2" weil pro Block zwei Entries (read und write)), dann Queue mehrmals benutzen.
	if (total_nr_blocks <= ENTRIES / 2)
	{
		ret = io_uring_queue_init(total_nr_blocks * 2, &ring, 0);

		if (ret != 0)
		{
			perror("Error on io_uring_queue_init");
			return 1;
		}

		iovecs = malloc(total_nr_blocks * sizeof(struct iovec));

		//iov.iov_base = buf;
		//iov2.iov_base = buf2;

		while (bytes_remaining)
		{
			nr_of_bytes = bytes_remaining;
			if (nr_of_bytes > BLOCK_SZ) nr_of_bytes = BLOCK_SZ;

			iovecs[current_block].iov_base = buf;
			iovecs[current_block].iov_len = nr_of_bytes;

			//iov2.iov_len = nr_of_bytes;

			/*temp = preadv(fd, &iov2, 1, offset);
			fd2 = open("/fs/students/tuneke/cat.txt", O_CREAT | O_RDWR, S_IRWXU);
			temp = pwritev(fd2, &iov2, 1, offset);*/

			//read submission queue entry
			sqe = io_uring_get_sqe(&ring);
			//io_uring_prep_read(sqe, fd, buf, nr_of_bytes, offset);
			io_uring_prep_readv(sqe, fd, &iovecs[current_block], 1, offset);
			//io_uring_sqe_set_data(sqe, NULL);
			io_uring_sqe_set_flags(sqe, IOSQE_IO_DRAIN);

			//write submission queue entry
			sqe = io_uring_get_sqe(&ring);
			//io_uring_prep_write(sqe, STDOUT_FILENO, buf, nr_of_bytes, offset);
			io_uring_prep_writev(sqe, STDOUT_FILENO, &iovecs[current_block], 1, offset);
			//io_uring_sqe_set_data(sqe, NULL);
			io_uring_sqe_set_flags(sqe, IOSQE_IO_DRAIN);

			offset += nr_of_bytes;

			current_block++;
			bytes_remaining -= nr_of_bytes;
		}

		//close(fd2);

		ret = io_uring_submit_and_wait(&ring, total_nr_blocks * 2);

		//Ich _glaube_ io_uring_cqe_seen() muss nicht aufgerufen werden, da wir auf die Fertigstellung aller Entries gewartet haben und wir die Queue danach nicht mehr brauchen und auch keine Userdaten haben

		if (ret != total_nr_blocks * 2)
		{
			//Sollte hier eigtl. nie hinkommen
			perror("io_uring_submit_and_wait did not wait for the number of entries submitted");
			return 1;
		}

		io_uring_queue_exit(&ring);
	}
	else
	{
		blocks_remaining = total_nr_blocks;

		//printf("In else-Block");

		ret = io_uring_queue_init(ENTRIES, &ring, 0);

		if (ret != 0)
		{
			perror("Error on io_uring_queue_init");
			return 1;
		}

		//Queue mit Größe ENTRIES anlegen
		while (blocks_remaining)
		{		
			nr_of_blocks = blocks_remaining;
			if (nr_of_blocks > ENTRIES / 2) nr_of_blocks = ENTRIES / 2;

			iovecs = malloc(total_nr_blocks * sizeof(struct iovec));

			/*ret = io_uring_queue_init(nr_of_blocks * 2, &ring, 0);

			if (ret != 0)
			{
				perror("Error on io_uring_queue_init");
				return 1;
			}*/

			current_block = 0;

			//TODO: Code-Duplikat auslagern, wenn alles funktioniert
			while (current_block < nr_of_blocks)
			{
				nr_of_bytes = bytes_remaining;
				if (nr_of_bytes > BLOCK_SZ) nr_of_bytes = BLOCK_SZ;

				iovecs[current_block].iov_base = buf;
				iovecs[current_block].iov_len = nr_of_bytes;

				//read submission queue entry
				sqe = io_uring_get_sqe(&ring);
				//io_uring_prep_read(sqe, fd, buf, nr_of_bytes, offset);
				io_uring_prep_readv(sqe, fd, &iovecs[current_block], 1, offset);
				//io_uring_sqe_set_data(sqe, NULL);
				io_uring_sqe_set_flags(sqe, IOSQE_IO_DRAIN);

				//write submission queue entry
				sqe = io_uring_get_sqe(&ring);
				//io_uring_prep_write(sqe, STDOUT_FILENO, buf, nr_of_bytes, offset);
				io_uring_prep_writev(sqe, STDOUT_FILENO, &iovecs[current_block], 1, offset);
				//io_uring_sqe_set_data(sqe, NULL);
				io_uring_sqe_set_flags(sqe, IOSQE_IO_DRAIN);

				offset += nr_of_bytes;

				current_block++;
				bytes_remaining -= nr_of_bytes;
			}

			ret = io_uring_submit_and_wait(&ring, nr_of_blocks * 2);

			//Ich _glaube_ io_uring_cqe_seen() muss nicht aufgerufen werden, da wir auf die Fertigstellung aller Entries gewartet haben und wir die Queue danach nicht mehr brauchen und auch keine Userdaten haben

			if (ret != nr_of_blocks * 2)
			{
				//Sollte hier eigtl. nie hinkommen
				perror("io_uring_submit_and_wait did not wait for the number of entries submitted");
				return 1;
			}

			//io_uring_queue_exit(&ring);

			blocks_remaining -= nr_of_blocks;
		}

		io_uring_queue_exit(&ring);
	}

	return 0;
}

