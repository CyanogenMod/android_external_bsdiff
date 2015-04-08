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
#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>    // android

#include "exfile.h"
#include "extents.h"


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

/* Parses an extent string ex_str, returning a pointer to a newly allocated
 * array of extents. The number of extents is stored in ex_count_p (if
 * provided). */
static ex_t *parse_extent_str(const char *ex_str, size_t *ex_count_p)
{
	size_t ex_count = (size_t)-1;
	ex_t *ex_arr = extents_parse(ex_str, NULL, &ex_count);
	if (!ex_arr)
		errx(1, (ex_count == (size_t)-1 ?
			 "error parsing extents" :
			 "error allocating extent array"));
	if (ex_count_p)
		*ex_count_p = ex_count;
	return ex_arr;
}

int bspatch(
    const char* old_filename, const char* new_filename,
    const char* patch_filename,
    const char* old_extents, const char* new_extents) {
	FILE * f, * cpf, * dpf, * epf;
	BZFILE * cpfbz2, * dpfbz2, * epfbz2;
	int cbz2err, dbz2err, ebz2err;
	FILE *old_file = NULL, *new_file = NULL;
	ssize_t oldsize,newsize;
	ssize_t bzctrllen,bzdatalen;
	u_char header[32],buf[8];
	u_char *new;
	off_t oldpos,newpos;
	off_t ctrl[3];
	off_t lenread;
	off_t i, j;

	int using_extents = (old_extents != NULL || new_extents != NULL);

	/* Open patch file */
	if ((f = fopen(patch_filename, "r")) == NULL)
		err(1, "fopen(%s)", patch_filename);

	/*
	File format:
		0	8	"BSDIFF40"
		8	8	X
		16	8	Y
		24	8	sizeof(new_filename)
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
		err(1, "fread(%s)", patch_filename);
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
		err(1, "fclose(%s)", patch_filename);
	if ((cpf = fopen(patch_filename, "r")) == NULL)
		err(1, "fopen(%s)", patch_filename);
	if (fseeko(cpf, 32, SEEK_SET))
		err(1, "fseeko(%s, %lld)", patch_filename,
		    (long long)32);
	if ((cpfbz2 = BZ2_bzReadOpen(&cbz2err, cpf, 0, 0, NULL, 0)) == NULL)
		errx(1, "BZ2_bzReadOpen, bz2err = %d", cbz2err);
	if ((dpf = fopen(patch_filename, "r")) == NULL)
		err(1, "fopen(%s)", patch_filename);
	if (fseeko(dpf, 32 + bzctrllen, SEEK_SET))
		err(1, "fseeko(%s, %lld)", patch_filename,
		    (long long)(32 + bzctrllen));
	if ((dpfbz2 = BZ2_bzReadOpen(&dbz2err, dpf, 0, 0, NULL, 0)) == NULL)
		errx(1, "BZ2_bzReadOpen, bz2err = %d", dbz2err);
	if ((epf = fopen(patch_filename, "r")) == NULL)
		err(1, "fopen(%s)", patch_filename);
	if (fseeko(epf, 32 + bzctrllen + bzdatalen, SEEK_SET))
		err(1, "fseeko(%s, %lld)", patch_filename,
		    (long long)(32 + bzctrllen + bzdatalen));
	if ((epfbz2 = BZ2_bzReadOpen(&ebz2err, epf, 0, 0, NULL, 0)) == NULL)
		errx(1, "BZ2_bzReadOpen, bz2err = %d", ebz2err);

	/* Open input file for reading. */
	if (using_extents) {
		size_t ex_count = 0;
		ex_t *ex_arr = parse_extent_str(old_extents, &ex_count);
		old_file = exfile_fopen(new_filename, "r", ex_arr, ex_count,
		                        free);
	} else {
		old_file = fopen(new_filename, "r");
	}
	if (!old_file ||
	    fseek(old_file, 0, SEEK_END) != 0 ||
	    (oldsize = ftell(old_file)) < 0 ||
	    fseek(old_file, 0, SEEK_SET) != 0)
		err(1, "cannot obtain the size of %s", new_filename);
	off_t old_file_pos = 0;

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

		/* Add old data to diff string. It is enough to fseek once, at
		 * the beginning of the sequence, to avoid unnecessary
		 * overhead. */
		j = newpos;
		if ((i = oldpos) < 0) {
			j -= i;
			i = 0;
		}
		if (i != old_file_pos && fseek(old_file, i, SEEK_SET) < 0)
			err(1, "error seeking input file to offset %" PRIdMAX,
			    (intmax_t)i);
		if ((old_file_pos = oldpos + ctrl[0]) > oldsize)
			old_file_pos = oldsize;
		while (i++ < old_file_pos) {
			u_char c;
			if (fread_unlocked(&c, 1, 1, old_file) != 1)
				err(1, "error reading from input file");
			new[j++] += c;
		}

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

	/* Close input file. */
	fclose(old_file);

	/* Clean up the bzip2 reads */
	BZ2_bzReadClose(&cbz2err, cpfbz2);
	BZ2_bzReadClose(&dbz2err, dpfbz2);
	BZ2_bzReadClose(&ebz2err, epfbz2);
	if (fclose(cpf) || fclose(dpf) || fclose(epf))
		err(1, "fclose(%s)", patch_filename);

	/* Write the new file */
	if (using_extents) {
		size_t ex_count = 0;
		ex_t *ex_arr = parse_extent_str(new_extents, &ex_count);
		new_file = exfile_fopen(new_filename, "w", ex_arr, ex_count,
		                        free);
	} else {
		new_file = fopen(new_filename, "w");
	}
	if (!new_file ||
	    fwrite_unlocked(new, 1, newsize, new_file) != newsize ||
	    fclose(new_file) == EOF)
		err(1,"%s",new_filename);

	free(new);

	return 0;
}
