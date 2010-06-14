/*-
 * Copyright 2003-2005 Colin Percival
 * All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted providing that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#if 0
__FBSDID("$FreeBSD: src/usr.bin/bsdiff/bspatch/bspatch.c,v 1.1 2005/08/06 01:59:06 cperciva Exp $");
#endif

#include <bzlib.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <err.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>    // android

#define JOIN(a, b) __JOIN(a, b)
#define __JOIN(a, b) a ## b
#define COMPILE_ASSERT(expr, message) \
	typedef char JOIN(message, JOIN(_, __LINE__)) [(expr) ? 1 : -1]

COMPILE_ASSERT(sizeof(int64_t) == 8, int64_t_64_bit);

#define MIN(a, b) \
	((a) < (b) ? (a) : (b))

// Reads next int from *ints. The int should be terminated with a comma
// or NULL char. *ints will be updated to the space right after the comma
// or set to NULL if this was the last number. This assumes the input is
// a valid string, as validated with PositionsStringIsValid().
// Returns 1 on success.
int NextInt64(const char** ints, int64_t *out) {
	if (!ints[0])
		return 0;
	int r = sscanf(*ints, "%" PRIi64, out);
	if (r == 1) {
		const char* next_comma = strchr(*ints, ',');
		const char* next_colon = strchr(*ints, ':');
		if (!next_comma && !next_colon)
			*ints = NULL;
		else if (!next_comma)
			*ints = next_colon + 1;
		else if (!next_colon)
			*ints = next_comma + 1;
		else
			*ints = MIN(next_comma, next_colon) + 1;
		return 1;
	}
	return 0;
}

COMPILE_ASSERT(sizeof(intmax_t) == 8, intmax_t_not_64_bit);

// Returns 1 if str can be converted to int64_t without over/underflowing.
// str is assumed to point to an optional negative sign followed by numbers,
// optionally followed by non-numeric characters, followed by '\0'.
int IsValidInt64(const char* str) {
	const char* end_ptr;
	errno = 0;
	intmax_t result = strtoimax(str, &end_ptr, /* base: */ 10);
	return errno == 0;
}

// Input validator. Make sure the positions string is well formatted.
// All numerical values are checked to make sure they don't over/underflow
// int64_t. Returns 1 if valid.
int PositionsStringIsValid(const char* positions) {
	if (positions == NULL)
		errx(1, "bad string");

	// Special case: empty string is valid
	if (!positions[0])
		return 1;

	// Use a state machine to determine if the string is valid.
	// Key: (s): state, ((s)) valid end state.
	// n (negative_valid) is a boolean that starts out as true.
	// If n is true, ':' is the delimiter, otherwise ','.
	//
	//         .--------------------------.
	//         |                          | n ? ':' : ',' ; n = !n
	//         V  '-'&&n           0-9    |
	// start->(0)------------->(1)----->((2))---.
	//           `--------------------->     <--' 0-9
	//              0-9
	int state = 0;
	int negative_valid = 1;
	const char* number_start = positions;
	for (;; positions++) {
		char c = *positions;
		switch (state) {
			case 0:
				if (c == '-' && negative_valid) {
					state = 1;
					continue;
				}
				if (isdigit(c)) {
					state = 2;
					continue;
				}
				return 0;
			case 1:
				if (isdigit(c)) {
					state = 2;
					continue;
				}
				return 0;
			case 2:
				if (isdigit(c))
					continue;
				// number_start must point to a valid number
				if (!IsValidInt64(number_start)) {
					return 0;
				}
				if ((negative_valid && c == ':') ||
				    (!negative_valid && c == ',')) {
					state = 0;
					number_start = positions + 1;
					negative_valid = !negative_valid;
					continue;
				}
				return (c == '\0');
		}
	}
}

// Reads into a buffer a series of byte ranges from filename.
// Each range is a pair of comma-separated ints from positions.
// -1 as an offset means a sparse-hole.
// E.g. If positions were "1,5:23,4:-1,8:3,7", then we would return a buffer
// consisting of 5 bytes from offset 1 of the file, followed by
// 4 bytes from offset 23, then 8 bytes of all zeros, then 7 bytes from
// offset 3 in the file.
// Returns NULL on error.
static char* PositionedRead(const char* filename,
                            const char* positions,
                            ssize_t* old_size) {
	if (!PositionsStringIsValid(positions)) {
		errx(1, "invalid positions string for read\n");
	}

	// Get length
	const char* p = positions;
	int64_t length = 0;
	for (;;) {
		int64_t value;
		if (0 == NextInt64(&p, &value)) {
			break;
		}
		int r = NextInt64(&p, &value);
		if (r == 0) {
			errx(1, "bad length parse\n");
		}
		if (value < 0) {
			errx(1, "length can't be negative\n");
		}
		length += value;
	}

	// Malloc
	if (length > 0x40000000) {  // 1 GiB; sanity check
		errx(1, "Read length too long (exceeds 1 GiB)");
	}
	// Following bsdiff convention, allocate length + 1 to avoid malloc(0)
	char* buf = malloc(length + 1);
	if (buf == NULL) {
		errx(1, "malloc failed\n");
	}
	char* buf_tail = buf;

	int fd = open(filename, O_RDONLY);
	if (fd < 0) {
		errx(1, "open failed for read\n");
	}

	// Read bytes
	p = positions;
	for (;;) {
		int64_t offset, read_length;
		if (NextInt64(&p, &offset) == 0) {
			break;
		}
		if (offset < 0) {
			errx(1, "no support for sparse positions "
			     "yet during read\n");
		}
		if (NextInt64(&p, &read_length) == 0) {
			errx(1, "bad length parse (should never happen)\n");
		}
		if (read_length < 0) {
			errx(1, "length can't be negative "
			     "(should never happen)\n");
		}
		ssize_t rc = pread(fd, buf_tail, read_length, offset);
		if (rc != read_length) {
			errx(1, "read failed\n");
		}
		buf_tail += rc;
	}
	close(fd);
	*old_size = length;
	return buf;
}

static void PositionedWrite(const char* filename,
                            const char* positions,
                            const char* buf,
                            ssize_t new_size) {
	if (!PositionsStringIsValid(positions)) {
		errx(1, "invalid positions string for write\n");
	}
	int fd = open(filename, O_WRONLY | O_CREAT, 0666);
	if (fd < 0) {
		errx(1, "open failed for write\n");
	}

	for (;;) {
		int64_t offset, length;
		if (NextInt64(&positions, &offset) == 0) {
			break;
		}
		if (NextInt64(&positions, &length) == 0) {
			errx(1, "bad length parse for write\n");
		}
		if (length < 0) {
			errx(1, "length can't be negative for write\n");
		}

		if (offset < 0) {
			// Sparse hole. Skip.
		} else {
			ssize_t rc = pwrite(fd, buf, length, offset);
			if (rc != length) {
				errx(1, "write failed\n");
			}
		}
		buf += length;
		new_size -= length;
	}
	if (new_size != 0) {
		errx(1, "output position length doesn't match new size\n");
	}
	close(fd);
}

static off_t offtin(u_char *buf)
{
	off_t y;

	y=buf[7]&0x7F;
	y=y*256;y+=buf[6];
	y=y*256;y+=buf[5];
	y=y*256;y+=buf[4];
	y=y*256;y+=buf[3];
	y=y*256;y+=buf[2];
	y=y*256;y+=buf[1];
	y=y*256;y+=buf[0];

	if(buf[7]&0x80) y=-y;

	return y;
}

int main(int argc,char * argv[])
{
	FILE * f, * cpf, * dpf, * epf;
	BZFILE * cpfbz2, * dpfbz2, * epfbz2;
	int cbz2err, dbz2err, ebz2err;
	int fd;
	ssize_t oldsize,newsize;
	ssize_t bzctrllen,bzdatalen;
	u_char header[32],buf[8];
	u_char *old, *new;
	off_t oldpos,newpos;
	off_t ctrl[3];
	off_t lenread;
	off_t i;

	if ((argc != 6) && (argc != 4)) {
		errx(1,"usage: %s oldfile newfile patchfile \\\n"
			"  [in_offset,in_length,in_offset,in_length,... \\\n"
			"  out_offset,out_length,"
			"out_offset,out_length,...]\n",argv[0]);
	}
	int using_positioning = (argc == 6);

	/* Open patch file */
	if ((f = fopen(argv[3], "r")) == NULL)
		err(1, "fopen(%s)", argv[3]);

	/*
	File format:
		0	8	"BSDIFF40"
		8	8	X
		16	8	Y
		24	8	sizeof(newfile)
		32	X	bzip2(control block)
		32+X	Y	bzip2(diff block)
		32+X+Y	???	bzip2(extra block)
	with control block a set of triples (x,y,z) meaning "add x bytes
	from oldfile to x bytes from the diff block; copy y bytes from the
	extra block; seek forwards in oldfile by z bytes".
	*/

	/* Read header */
	if (fread(header, 1, 32, f) < 32) {
		if (feof(f))
			errx(1, "Corrupt patch\n");
		err(1, "fread(%s)", argv[3]);
	}

	/* Check for appropriate magic */
	if (memcmp(header, "BSDIFF40", 8) != 0)
		errx(1, "Corrupt patch\n");

	/* Read lengths from header */
	bzctrllen=offtin(header+8);
	bzdatalen=offtin(header+16);
	newsize=offtin(header+24);
	if((bzctrllen<0) || (bzdatalen<0) || (newsize<0))
		errx(1,"Corrupt patch\n");

	/* Close patch file and re-open it via libbzip2 at the right places */
	if (fclose(f))
		err(1, "fclose(%s)", argv[3]);
	if ((cpf = fopen(argv[3], "r")) == NULL)
		err(1, "fopen(%s)", argv[3]);
	if (fseeko(cpf, 32, SEEK_SET))
		err(1, "fseeko(%s, %lld)", argv[3],
		    (long long)32);
	if ((cpfbz2 = BZ2_bzReadOpen(&cbz2err, cpf, 0, 0, NULL, 0)) == NULL)
		errx(1, "BZ2_bzReadOpen, bz2err = %d", cbz2err);
	if ((dpf = fopen(argv[3], "r")) == NULL)
		err(1, "fopen(%s)", argv[3]);
	if (fseeko(dpf, 32 + bzctrllen, SEEK_SET))
		err(1, "fseeko(%s, %lld)", argv[3],
		    (long long)(32 + bzctrllen));
	if ((dpfbz2 = BZ2_bzReadOpen(&dbz2err, dpf, 0, 0, NULL, 0)) == NULL)
		errx(1, "BZ2_bzReadOpen, bz2err = %d", dbz2err);
	if ((epf = fopen(argv[3], "r")) == NULL)
		err(1, "fopen(%s)", argv[3]);
	if (fseeko(epf, 32 + bzctrllen + bzdatalen, SEEK_SET))
		err(1, "fseeko(%s, %lld)", argv[3],
		    (long long)(32 + bzctrllen + bzdatalen));
	if ((epfbz2 = BZ2_bzReadOpen(&ebz2err, epf, 0, 0, NULL, 0)) == NULL)
		errx(1, "BZ2_bzReadOpen, bz2err = %d", ebz2err);

	// Read

	if (!using_positioning) {
		if(((fd=open(argv[1],O_RDONLY,0))<0) ||
			((oldsize=lseek(fd,0,SEEK_END))==-1) ||
			((old=malloc(oldsize+1))==NULL) ||
			(lseek(fd,0,SEEK_SET)!=0) ||
			(read(fd,old,oldsize)!=oldsize) ||
			(close(fd)==-1)) err(1,"%s",argv[1]);
	} else {
		old = PositionedRead(argv[1], argv[4], &oldsize);
	}
	if((new=malloc(newsize+1))==NULL) err(1,NULL);

	oldpos=0;newpos=0;
	while(newpos<newsize) {
		/* Read control data */
		for(i=0;i<=2;i++) {
			lenread = BZ2_bzRead(&cbz2err, cpfbz2, buf, 8);
			if ((lenread < 8) || ((cbz2err != BZ_OK) &&
			    (cbz2err != BZ_STREAM_END)))
				errx(1, "Corrupt patch\n");
			ctrl[i]=offtin(buf);
		};

		// android local change (start)
		if (ctrl[0]<0||ctrl[1]<0)
			errx(1,"Corrupt patch\n");
		// android local change (end)

		/* Sanity-check */
		if(newpos+ctrl[0]>newsize)
			errx(1,"Corrupt patch\n");

		/* Read diff string */
		lenread = BZ2_bzRead(&dbz2err, dpfbz2, new + newpos, ctrl[0]);
		if ((lenread < ctrl[0]) ||
		    ((dbz2err != BZ_OK) && (dbz2err != BZ_STREAM_END)))
			errx(1, "Corrupt patch\n");

		/* Add old data to diff string */
		for(i=0;i<ctrl[0];i++)
			if((oldpos+i>=0) && (oldpos+i<oldsize))
				new[newpos+i]+=old[oldpos+i];

		/* Adjust pointers */
		newpos+=ctrl[0];
		oldpos+=ctrl[0];

		/* Sanity-check */
		if(newpos+ctrl[1]>newsize)
			errx(1,"Corrupt patch\n");

		/* Read extra string */
		lenread = BZ2_bzRead(&ebz2err, epfbz2, new + newpos, ctrl[1]);
		if ((lenread < ctrl[1]) ||
		    ((ebz2err != BZ_OK) && (ebz2err != BZ_STREAM_END)))
			errx(1, "Corrupt patch\n");

		/* Adjust pointers */
		newpos+=ctrl[1];
		oldpos+=ctrl[2];
	};

	/* Clean up the bzip2 reads */
	BZ2_bzReadClose(&cbz2err, cpfbz2);
	BZ2_bzReadClose(&dbz2err, dpfbz2);
	BZ2_bzReadClose(&ebz2err, epfbz2);
	if (fclose(cpf) || fclose(dpf) || fclose(epf))
		err(1, "fclose(%s)", argv[3]);

	/* Write the new file */
	if (!using_positioning) {
		if(((fd=open(argv[2],O_CREAT|O_TRUNC|O_WRONLY,0666))<0) ||
			(write(fd,new,newsize)!=newsize) || (close(fd)==-1))
			err(1,"%s",argv[2]);
	} else {
		PositionedWrite(argv[2], argv[5], new, newsize);
	}

	free(new);
	free(old);

	return 0;
}
