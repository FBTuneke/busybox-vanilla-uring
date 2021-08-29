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

 // #define ENTRIES 16
 // #define ENTRIES 128
//#define ENTRIES 1024
#define ENTRIES 4096
// #define ENTRIES 8192


// #define BLOCK_SZ 1024
//#define BLOCK_SZ 65536 //Normales GNU cat benutzt auf ext4 scheinbar 128kb blocksize, bei höherer blocksize als diese hier aber kaum einsparung bei unserem use case.. vllt use case ändern? man bräuchte den durchschnittlichen use case
//#define BLOCK_SZ 131072
//#define BLOCK_SZ 128000
#define BLOCK_SZ 4096
// #define BLOCK_SZ 8192
// #define BLOCK_SZ 16384
//#define BLOCK_SZ 131072
// #define BLOCK_SZ 128

struct file_info {
    int file_sz;
    struct iovec iovecs[];
};

off_t get_file_size(int fd) 
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
	off_t writtenBytes = 0;

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

			int ret = read_write_with_uring(fd, writtenBytes);
			
			//Optimierungspotential: Wird auch in read_write_with_uring schon benutzt.
			writtenBytes += get_file_size(fd);

			if (ret)
			{
				fprintf(stderr, "Error reading/writing file: %s\n", *argv);

				return 1;
			}

			if (fd != STDIN_FILENO) close(fd);
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

int read_write_with_uring(int fd, off_t interFilesOffset)
{
	struct io_uring ring;
	struct io_uring_sqe* sqe;
	struct io_uring_cqe** cqes;
	struct iovec rw_iovec;
	void* buf;
	// int ENTRIES = 2;
	//void* buf2;
	
	int fdArray[2];
	off_t nr_of_bytes;
	int ret;
	off_t file_sz = get_file_size(fd);
	off_t bytes_remaining = file_sz;
	off_t writeOffset;
	off_t readOffset;
	off_t processed_blocks = 0;
	off_t total_nr_blocks = file_sz / (off_t) BLOCK_SZ;
	off_t nr_of_blocks_current_batch = 0;
	off_t blocks_remaining;
	//int temp;
	//int fd2;
	
	bool first = true;
	
	writeOffset = interFilesOffset;
	readOffset = 0;

	//Zusätzlichen Block für Rest
	if (file_sz % (off_t) BLOCK_SZ) total_nr_blocks++;

	//struct file_info *fi = malloc(sizeof(*fi) + sizeof(struct iovec));

	// if (posix_memalign(&buf, BLOCK_SZ, BLOCK_SZ))
	// {
		// perror("Error on posix_memalign");
		// return 1;
	// }
	
	if ((buf = malloc(BLOCK_SZ)) == NULL )
	{
		perror("Error on malloc for read/write buffer");
		return 1;
	}

	/*if (posix_memalign(&buf2, BLOCK_SZ, BLOCK_SZ))
	{
		perror("Error on posix_memalign2");
		return 1;
	}*/

	blocks_remaining = total_nr_blocks;

	//Einfach in Schleife reusen, nicht jedes mal neu initialisieren
	//"As soon as an sqe is consumed by the kernel, the application is free to reuse that sqe entry."
	ret = io_uring_queue_init(ENTRIES, &ring, 0);
	if (ret != 0)
	{
		perror("Error on io_uring_queue_init");
		return 1;
	}
	
	//--------------------------------TODO------------------
	//Glaube das bringt nix, wenn man nur wenig files/buffer benutzt. geschwindigkeit ist um faktor 4 langsamer als vorher...
	//Evtl. mal alle files öffnen und übergeben. Dann immer den gleichen Ring benutzen..
	//----------------------------------------------------------
	
	// fdArray[0] = fd;
	// fdArray[1] = STDOUT_FILENO;
	
	// //register file deskriptoren fuer mehr performance
	// ret = io_uring_register_files(&ring, fdArray, 2);
	// if (ret != 0)
	// {
		// perror("Error on io_uring_register");
		// return 1;
	// }
	
	// rw_iovec.iov_base = buf;
	// rw_iovec.iov_len = BLOCK_SZ;
	
	// //register buffer fuer mehr performance
	// ret = io_uring_register_buffers(&ring, &rw_iovec, 1);
	// if (ret != 0)
	// {
		// perror("Error on io_uring_register_buffers");
		// return 1;
	// }
	
	//22.11.2020 - geaendert, weil eigtl. unnoetig gross.
	// cqes = malloc(2 * total_nr_blocks * sizeof(struct io_uring_cqe*));
	cqes = malloc(ENTRIES * sizeof(struct io_uring_cqe*));

	while (blocks_remaining)
	{
		nr_of_blocks_current_batch = blocks_remaining;
		if (nr_of_blocks_current_batch > ENTRIES / 2) nr_of_blocks_current_batch = ENTRIES / 2;

		//TODO: Aus Schleife raus?!
		// iovecs = malloc(total_nr_blocks * sizeof(struct iovec));

		/*ret = io_uring_queue_init(nr_of_blocks_current_batch * 2, &ring, 0);

		if (ret != 0)
		{
			perror("Error on io_uring_queue_init");
			return 1;
		}*/

		processed_blocks = 0;

		while (processed_blocks < nr_of_blocks_current_batch)
		{
			nr_of_bytes = bytes_remaining;
			if (nr_of_bytes > BLOCK_SZ) nr_of_bytes = BLOCK_SZ;

			//read submission queue entry
			sqe = io_uring_get_sqe(&ring);
			io_uring_prep_read(sqe, fd, buf, nr_of_bytes, readOffset);
			// io_uring_prep_read(sqe, 0, buf, nr_of_bytes, readOffset);
			// io_uring_prep_read_fixed(sqe, 0, rw_iovec.iov_base, nr_of_bytes, readOffset, 0);
			//io_uring_prep_readv(sqe, fd, &iovecs[processed_blocks], 1, readOffset);
			//io_uring_sqe_set_data(sqe, NULL);
			io_uring_sqe_set_flags(sqe, IOSQE_IO_DRAIN);
			// io_uring_sqe_set_flags(sqe, IOSQE_IO_DRAIN | IOSQE_FIXED_FILE);

			//write submission queue entry
			sqe = io_uring_get_sqe(&ring);
			io_uring_prep_write(sqe, STDOUT_FILENO, buf, nr_of_bytes, writeOffset);
			// io_uring_prep_write(sqe, 1, buf, nr_of_bytes, writeOffset);
			// io_uring_prep_write_fixed(sqe, 1, rw_iovec.iov_base, nr_of_bytes, writeOffset, 0);
			//io_uring_prep_writev(sqe, STDOUT_FILENO, &iovecs[processed_blocks], 1, writeOffset);
			// io_uring_sqe_set_data(sqe, NULL);
			io_uring_sqe_set_flags(sqe, IOSQE_IO_DRAIN);
			// io_uring_sqe_set_flags(sqe, IOSQE_IO_DRAIN | IOSQE_FIXED_FILE);

			writeOffset += nr_of_bytes;
			readOffset += nr_of_bytes;

			processed_blocks++;
			bytes_remaining -= nr_of_bytes;
		}

		ret = io_uring_submit_and_wait(&ring, nr_of_blocks_current_batch * 2);
		if (ret != nr_of_blocks_current_batch * 2)
		{		
			//sollte hier eigtl. nie hinkommen
			perror("io_uring_submit_and_wait did not wait for the number of entries submitted");
			return 1;
		}
		
		// printf("ret: %i\n", ret);
		// printf("Entries: %i\n", ENTRIES);

		//Hole alle cqes
		ret = io_uring_peek_batch_cqe(&ring, cqes, nr_of_blocks_current_batch * 2);
		if (ret != nr_of_blocks_current_batch * 2)
		{
			//Sollte hier eigtl. nie hinkommen
			perror("io_uring_peek_batch_cqe did not return the same number of cqes as entries were submitted");
			return 1;
		}

		//markiere alle cqes als gesehen
		for (int i = 0; i < ret; i++)
		{
			//Vllt in user data mitgeben ob es read oder write war und hier mit ausgeben..
			if (cqes[i]->res < 0)
			{
				perror("read/write syscall mit rückgabewert < 0");
				return 1;
			}

			io_uring_cqe_seen(&ring, cqes[i]);
		}

		blocks_remaining -= nr_of_blocks_current_batch;
	}

	io_uring_queue_exit(&ring);
	free(cqes);

	return 0;

	
	//Bei riesen Datei Würde Queue auch seeehr viele Einträge benötigen. Daher aufteilen sinnvoll, auch wenn wir zwischendurch immer wieder zurückkehren?!
	//Maximal ENTRIES Einträge. Wenn Datei Größer als ENTRIES/2 * BLOCK_SZ ("/2" weil pro Block zwei Entries (read und write)), dann Queue mehrmals benutzen.
	//if (total_nr_blocks <= ENTRIES / 2)
	//{
	//	ret = io_uring_queue_init(total_nr_blocks * 2, &ring, 0);

	//	if (ret != 0)
	//	{
	//		perror("Error on io_uring_queue_init");
	//		return 1;
	//	}

	//	//1 iovec würde auch reichen!
	//	iovecs = malloc(total_nr_blocks * sizeof(struct iovec));
	//	writeIovecs = malloc(total_nr_blocks * sizeof(struct iovec));

	//	//iov.iov_base = buf;
	//	//iov2.iov_base = buf2;

	//	while (bytes_remaining)
	//	{
	//		nr_of_bytes = bytes_remaining;
	//		if (nr_of_bytes > BLOCK_SZ) nr_of_bytes = BLOCK_SZ;

	//		//1 Iovec reicht auch, bis auf letzten rest. 
	//		iovecs[processed_blocks].iov_base = buf;
	//		iovecs[processed_blocks].iov_len = nr_of_bytes;

	//		writeIovec.iov_base = buf;


	//		writeIovec.iov_len = ((struct io_uring_cqe) ring.cq.ktail)->res;

	//		//iov2.iov_len = nr_of_bytes;

	//		/*temp = preadv(fd, &iov2, 1, offset);
	//		fd2 = open("/fs/students/tuneke/cat.txt", O_CREAT | O_RDWR, S_IRWXU);
	//		temp = pwritev(fd2, &iov2, 1, offset);*/

	//		//read submission queue entry
	//		sqe = io_uring_get_sqe(&ring);
	//		//io_uring_prep_read(sqe, fd, buf, nr_of_bytes, offset);
	//		io_uring_prep_readv(sqe, fd, &iovecs[processed_blocks], 1, offset);
	//		//io_uring_sqe_set_data(sqe, NULL);
	//		io_uring_sqe_set_flags(sqe, IOSQE_IO_DRAIN);

	//		//write submission queue entry
	//		sqe = io_uring_get_sqe(&ring);
	//		//io_uring_prep_write(sqe, STDOUT_FILENO, buf, nr_of_bytes, offset);
	//		io_uring_prep_writev(sqe, STDOUT_FILENO, &writeIovec, 1, offset);
	//		//io_uring_sqe_set_data(sqe, NULL);
	//		io_uring_sqe_set_flags(sqe, IOSQE_IO_DRAIN);

	//		offset += nr_of_bytes;

	//		processed_blocks++;
	//		bytes_remaining -= nr_of_bytes;
	//	}

	//	//close(fd2);

	//	ret = io_uring_submit_and_wait(&ring, total_nr_blocks * 2);

	//	//Hier CQEs noch verarbeiten. Rückgabewert checken und als "gesehen" markieren (löscht dann eintrag aus completion queue?!)

	//	if (ret != total_nr_blocks * 2)
	//	{
	//		//Sollte hier eigtl. nie hinkommen
	//		perror("io_uring_submit_and_wait did not wait for the number of entries submitted");
	//		return 1;
	//	}

	//	io_uring_queue_exit(&ring);
	//}
	//else
	//{
	//	blocks_remaining = total_nr_blocks;

	//	//Einfach in Schleife reusen, nicht jedes mal neu initialisieren
	//	//"As soon as an sqe is consumed by the kernel, the application is free to reuse that sqe entry."
	//	ret = io_uring_queue_init(ENTRIES, &ring, 0);

	//	if (ret != 0)
	//	{
	//		perror("Error on io_uring_queue_init");
	//		return 1;
	//	}

	//	while (blocks_remaining)
	//	{		
	//		nr_of_blocks_current_batch = blocks_remaining;
	//		if (nr_of_blocks_current_batch > ENTRIES / 2) nr_of_blocks_current_batch = ENTRIES / 2;

	//		iovecs = malloc(total_nr_blocks * sizeof(struct iovec));

	//		/*ret = io_uring_queue_init(nr_of_blocks_current_batch * 2, &ring, 0);

	//		if (ret != 0)
	//		{
	//			perror("Error on io_uring_queue_init");
	//			return 1;
	//		}*/

	//		processed_blocks = 0;

	//		//TODO: Code-Duplikat auslagern, wenn alles funktioniert
	//		while (processed_blocks < nr_of_blocks_current_batch)
	//		{
	//			nr_of_bytes = bytes_remaining;
	//			if (nr_of_bytes > BLOCK_SZ) nr_of_bytes = BLOCK_SZ;

	//			iovecs[processed_blocks].iov_base = buf;
	//			iovecs[processed_blocks].iov_len = nr_of_bytes;

	//			//read submission queue entry
	//			sqe = io_uring_get_sqe(&ring);
	//			//io_uring_prep_read(sqe, fd, buf, nr_of_bytes, offset);
	//			io_uring_prep_readv(sqe, fd, &iovecs[processed_blocks], 1, offset);
	//			//io_uring_sqe_set_data(sqe, NULL);
	//			io_uring_sqe_set_flags(sqe, IOSQE_IO_DRAIN);

	//			//write submission queue entry
	//			sqe = io_uring_get_sqe(&ring);
	//			//io_uring_prep_write(sqe, STDOUT_FILENO, buf, nr_of_bytes, offset);
	//			io_uring_prep_writev(sqe, STDOUT_FILENO, &iovecs[processed_blocks], 1, offset);
	//			//io_uring_sqe_set_data(sqe, NULL);
	//			io_uring_sqe_set_flags(sqe, IOSQE_IO_DRAIN);

	//			offset += nr_of_bytes;

	//			processed_blocks++;
	//			bytes_remaining -= nr_of_bytes;
	//		}

	//		ret = io_uring_submit_and_wait(&ring, nr_of_blocks_current_batch * 2);

	//		//Hier CQEs noch verarbeiten. Rückgabewert checken und als "gesehen" markieren (löscht dann eintrag aus completion queue?!)

	//		if (ret != nr_of_blocks_current_batch * 2)
	//		{
	//			//Sollte hier eigtl. nie hinkommen
	//			perror("io_uring_submit_and_wait did not wait for the number of entries submitted");
	//			return 1;
	//		}

	//		//io_uring_queue_exit(&ring);

	//		blocks_remaining -= nr_of_blocks_current_batch;
	//	}

	//	io_uring_queue_exit(&ring);
	//}


}

