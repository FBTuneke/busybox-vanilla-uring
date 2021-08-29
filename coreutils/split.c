/* vi: set sw=4 ts=4: */
/*
 * split - split a file into pieces
 * Copyright (c) 2007 Bernhard Reutner-Fischer
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */
//config:config SPLIT
//config:	bool "split (5 kb)"
//config:	default y
//config:	help
//config:	Split a file into pieces.
//config:
//config:config FEATURE_SPLIT_FANCY
//config:	bool "Fancy extensions"
//config:	default y
//config:	depends on SPLIT
//config:	help
//config:	Add support for features not required by SUSv3.
//config:	Supports additional suffixes 'b' for 512 bytes,
//config:	'g' for 1GiB for the -b option.

//applet:IF_SPLIT(APPLET(split, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_SPLIT) += split.o

/* BB_AUDIT: SUSv3 compliant
 * SUSv3 requirements:
 * http://www.opengroup.org/onlinepubs/009695399/utilities/split.html
 */

//usage:#define split_trivial_usage
//usage:       "[OPTIONS] [INPUT [PREFIX]]"
//usage:#define split_full_usage "\n\n"
//usage:       "	-b N[k|m]	Split by N (kilo|mega)bytes"
//usage:     "\n	-l N		Split by N lines"
//usage:     "\n	-a N		Use N letters as suffix"
//usage:
//usage:#define split_example_usage
//usage:       "$ split TODO foo\n"
//usage:       "$ cat TODO | split -a 2 -l 2 TODO_\n"

#include "libbb.h"
#include "common_bufsiz.h"
#include "../liburing/include/liburing.h"

#if ENABLE_FEATURE_SPLIT_FANCY
static const struct suffix_mult split_suffixes[] = {
	{ "b", 512 },
	{ "k", 1024 },
	{ "m", 1024*1024 },
	{ "g", 1024*1024*1024 },
	{ "", 0 }
};
#endif

/* Increment the suffix part of the filename.
 * Returns NULL if we are out of filenames.
 */
static char *next_file(char *old, unsigned suffix_len)
{
	size_t end = strlen(old);
	unsigned i = 1;
	char *curr;

	while (1) {
		curr = old + end - i;
		if (*curr < 'z') {
			*curr += 1;
			break;
		}
		i++;
		if (i > suffix_len) {
			return NULL;
		}
		*curr = 'a';
	}

	return old;
}

// #define read_buffer bb_common_bufsiz1
// enum { READ_BUFFER_SIZE = COMMON_BUFSIZE - 1 };
// enum { READ_BUFFER_SIZE = 1024 };
// enum { READ_BUFFER_SIZE = 2048 };
// enum { READ_BUFFER_SIZE = 3072 };
// enum { READ_BUFFER_SIZE = 4096 };
// enum { READ_BUFFER_SIZE = 5120 };
// enum { READ_BUFFER_SIZE = 6144 };
// enum { READ_BUFFER_SIZE = 10240 };
// enum { READ_BUFFER_SIZE = 20480 };
// enum { READ_BUFFER_SIZE = 40960 };
// enum { READ_BUFFER_SIZE = 51200 };
// enum { READ_BUFFER_SIZE = 61440 };
// enum { READ_BUFFER_SIZE = 102400 };
// enum { READ_BUFFER_SIZE = 614400 };
 enum { READ_BUFFER_SIZE = 1228800 };

#define SPLIT_OPT_l (1<<0)
#define SPLIT_OPT_b (1<<1)
#define SPLIT_OPT_a (1<<2)

int split_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int split_main(int argc UNUSED_PARAM, char **argv)
{
	unsigned suffix_len = 2;
	char *pfx;
	char *count_p;
	const char *sfx;
	off_t cnt = 1000;
	off_t remaining = 0;
	unsigned opt;
	ssize_t bytes_read, to_write;
	char *src;

	struct io_uring ring;
	struct io_uring_sqe* sqe;
	struct io_uring_cqe** cqes;
	int ret;
	int fd = 0;
	int oldFd = 1;
	int offsetWrite = 0;
	int offsetRead = 0;
	int nrOfEntries = 0;
	bool firstLoop = true;
	int nrOfOpenFiles = 0;
	int nrOfOpenedFiles = 0;
	int nrOfCloses = 0;
	int nrOfCurrentEntries = 0;
	char* read_buffer;

	read_buffer = malloc(READ_BUFFER_SIZE * sizeof(char));

	setup_common_bufsiz();

	opt = getopt32(argv, "^"
			"l:b:a:+" /* -a N */
			"\0" "?2"/*max 2 args*/,
			&count_p, &count_p, &suffix_len
	);

	if (opt & SPLIT_OPT_l)
		cnt = XATOOFF(count_p);
	if (opt & SPLIT_OPT_b) // FIXME: also needs XATOOFF
		cnt = xatoull_sfx(count_p,
				IF_FEATURE_SPLIT_FANCY(split_suffixes)
				IF_NOT_FEATURE_SPLIT_FANCY(km_suffixes)
		);
	sfx = "x";

	argv += optind;
	if (argv[0]) 
	{
		int fd;
		if (argv[1])
			sfx = argv[1];
		fd = xopen_stdin(argv[0]);
		xmove_fd(fd, STDIN_FILENO);
	} else {
		argv[0] = (char *) bb_msg_standard_input;
	}

	if (NAME_MAX < strlen(sfx) + suffix_len)
		bb_error_msg_and_die("suffix too long");

	{
		char *char_p = xzalloc(suffix_len + 1);
		memset(char_p, 'a', suffix_len);
		pfx = xasprintf("%s%s", sfx, char_p);
		if (ENABLE_FEATURE_CLEAN_UP)
			free(char_p);
	}

	//uring setup. Maximal READ_BUFFER_SIZE * 2 Einträge, da im schlechtesten fall ein byte pro file = 1 write + 1 close pro byte.
	//TODO: Glaube cnt * 2 reicht auch für den worst case und ist meistens kleiner
	// ret = io_uring_queue_init(READ_BUFFER_SIZE * 2, &ring, 0);
	// ret = io_uring_queue_init(READ_BUFFER_SIZE, &ring, 0);
	ret = io_uring_queue_init(10240, &ring, 0);

	if (ret != 0)
	{
		perror("Error on io_uring_queue_init #1");
		return 1;
	}

	bytes_read = safe_read(STDIN_FILENO, read_buffer, READ_BUFFER_SIZE);
	
	if (!bytes_read)
	{
		perror("Did read zero bytes on first safe_read");
		return 1;
	}
	if (bytes_read < 0) bb_simple_perror_msg_and_die(argv[0]);
	
	src = read_buffer;

	offsetRead += bytes_read;

	while (1) 
	{	
		nrOfEntries = 0;
		if(!remaining) firstLoop = true;
		nrOfOpenFiles = 0;
		nrOfCloses = 0;
		
		do 
		{
			if (!remaining) 
			{
				if (!pfx) bb_error_msg_and_die("suffixes exhausted");
				//xmove_fd(xopen(pfx, O_WRONLY | O_CREAT | O_TRUNC), 1);

				fd = xopen(pfx, O_WRONLY | O_CREAT | O_TRUNC);
				nrOfOpenFiles++;
				nrOfOpenedFiles++;
				
				// printf("Filedeskriptor: %i\n", fd);
				
				if (firstLoop)
				{
					firstLoop = false;
					oldFd = fd;
				}

				pfx = next_file(pfx, suffix_len);
				remaining = cnt;
				offsetWrite = 0;
			}

			if (opt & SPLIT_OPT_b) 
			{
				/* split by bytes */
				to_write = (bytes_read < remaining) ? bytes_read : remaining;
				remaining -= to_write;
			} 
			else 
			{
				/* split by lines */
				/* can be sped up by using _memrchr_
				 * and writing many lines at once... */
				char *end = memchr(src, '\n', bytes_read);
				if (end) 
				{
					--remaining;
					to_write = end - src + 1;
				} 
				else to_write = bytes_read;
			}

			//File kann geschlossen werden, wenn neues geöffnet
			if (oldFd != fd)
			{
				sqe = io_uring_get_sqe(&ring);
				io_uring_prep_close(sqe, oldFd);
				io_uring_sqe_set_flags(sqe, IOSQE_IO_DRAIN);
				
				// close(oldFd); //geht natürlich nicht, geht erst nach dem gelesen/geschrieben wurde..
				// printf("closed file\n");
				nrOfCloses++;
				oldFd = fd;
				nrOfEntries++;
			}
		
			//Man könnte verschiedene Ketten machen, die parallel ablaufen. für jede Datei eine eigene KEtte. Weiß aber nicht ob das hier viel bringt, der buffer oben ist nur 1024 byte groß...
			sqe = io_uring_get_sqe(&ring);
			io_uring_prep_write(sqe, fd, src, to_write, offsetWrite);
			io_uring_sqe_set_flags(sqe, IOSQE_IO_DRAIN);

			//xwrite(STDOUT_FILENO, src, to_write);
			bytes_read -= to_write;
			src += to_write;
			offsetWrite += to_write;
			nrOfEntries++;

		} while (bytes_read);
		
		
		// Fall: Buffer zu Ende gelesen und Datei zu Ende geschrieben. Tritt dies auf, dann greift die obere Abfrage (olFd != fd) nicht, da fd erst in dem nächsten
		// Schleifendurchlauf geändert werden würde, den es aber nicht mehr gibt. Also muss hier noch mal geclosed werden.
		if(!remaining) 
		{
				// printf("---------Adding Close sqe fuer Fall 2---------");
				sqe = io_uring_get_sqe(&ring);
				io_uring_prep_close(sqe, fd);
				io_uring_sqe_set_flags(sqe, IOSQE_IO_DRAIN);
				nrOfCloses++;
				nrOfEntries++;
		}
		
		// printf("Anzahl gleichzeitig offener Files: %i\n", nrOfOpenFiles);
		// printf("Anzahl insgesamt geoeffnter Files: %i\n", nrOfOpenedFiles);
		// printf("Anzahl von Close-SQES in uring: %i\n", nrOfCloses);
		
		// printf("Anzahl an SQEs: %i\n", nrOfEntries);
		
		//Hier müsste man eigtl. abfragen ob offsetRead > Filegröße ist. Wenn ja, keinen read mehr in uring einreihen
		//Man könnte auch schauen ob bytes_read % READ_BUFFER_SIZE != 0 ist, das müsste dann nämlich der etzte "Rest" an Bytes der Datei sein, der gelesen wurde
		//Scheint zu funktionieren, read-SQE scheint wohl, wenn der file-pointer durch den offset-Parameter auf eine Stelle außerhalb des Files zeigen würde mit 0 zurückzukehren.

		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_read(sqe, STDIN_FILENO, read_buffer, READ_BUFFER_SIZE, offsetRead);
		io_uring_sqe_set_flags(sqe, IOSQE_IO_DRAIN);
		nrOfEntries++;
		
		
		//VERBRAUCHT DAS HIER SO VIEL ZEIT?! - denke eher nicht angeblich laut stackoverflow "unmeasurable difference"
		cqes = malloc(nrOfEntries * sizeof(struct io_uring_cqe*));

		ret = io_uring_submit_and_wait(&ring, nrOfEntries);
		if (ret != nrOfEntries)
		{
			perror("io_uring_submit_and_wait did not wait for the number of entries submitted");
			return 1;
		}
		//Hole alle cqes
		ret = io_uring_peek_batch_cqe(&ring, cqes, nrOfEntries);
		if (ret != nrOfEntries)
		{
			//Sollte hier eigtl. nie hinkommen
			perror("io_uring_peek_batch_cqe did not return the same number of cqes as entries were submitted");
			return 1;
		}			

		//markiere alle cqes als gesehen
		for (int i = 0; i < ret-1; i++)
		{
			if (cqes[i]->res < 0)
			{
				perror("write/close syscall mit rückgabewert < 0");
				printf("Error on syscall nummer %i\n", i);
				return 1;
			}

			io_uring_cqe_seen(&ring, cqes[i]);
		}

		//Letzer CQE gehört zum read-SQE, da Serialisierung
		//Rückgabewert holen und für nächsten Schleifendurchlauf vorbereiten, oder Error oder fertig (alles von Datei gelesen)
		bytes_read = cqes[ret - 1]->res;
		
		// printf("Bytes read: %i\n", bytes_read);
		
		io_uring_cqe_seen(&ring, cqes[ret - 1]);

		free(cqes);
		
		//finished
		if (!bytes_read) break;

		if (bytes_read < 0)
		{
			perror("Error - bytes_read from read sqe < 0");
			bb_simple_perror_msg_and_die(argv[0]);
		}

		src = read_buffer;
		offsetRead += bytes_read;
	}

	io_uring_queue_exit(&ring);
	// printf("Reabuffersize: %i\n", READ_BUFFER_SIZE);

	return EXIT_SUCCESS;
}
