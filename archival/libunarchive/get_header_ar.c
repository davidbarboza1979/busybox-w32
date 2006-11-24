/* vi: set sw=4 ts=4: */
/* Copyright 2001 Glenn McGrath.
 *
 * Licensed under GPLv2 or later, see file LICENSE in this tarball for details.
 */

#include "libbb.h"
#include "unarchive.h"

char get_header_ar(archive_handle_t *archive_handle)
{
	file_header_t *typed = archive_handle->file_header;
	union {
		char raw[60];
		struct {
			char name[16];
			char date[12];
			char uid[6];
			char gid[6];
			char mode[8];
			char size[10];
			char magic[2];
		} formatted;
	} ar;
#ifdef CONFIG_FEATURE_AR_LONG_FILENAMES
	static char *ar_long_names;
	static unsigned int ar_long_name_size;
#endif

	/* dont use xread as we want to handle the error ourself */
	if (read(archive_handle->src_fd, ar.raw, 60) != 60) {
		/* End Of File */
		return(EXIT_FAILURE);
	}

	/* ar header starts on an even byte (2 byte aligned)
	 * '\n' is used for padding
	 */
	if (ar.raw[0] == '\n') {
		/* fix up the header, we started reading 1 byte too early */
		memmove(ar.raw, &ar.raw[1], 59);
		ar.raw[59] = xread_char(archive_handle->src_fd);
		archive_handle->offset++;
	}
	archive_handle->offset += 60;

	/* align the headers based on the header magic */
	if ((ar.formatted.magic[0] != '`') || (ar.formatted.magic[1] != '\n')) {
		bb_error_msg_and_die("invalid ar header");
	}

	typed->mode = xstrtoul(ar.formatted.mode, 8);
	typed->mtime = xatou(ar.formatted.date);
	typed->uid = xatou(ar.formatted.uid);
	typed->gid = xatou(ar.formatted.gid);
	typed->size = xatoul(ar.formatted.size);

	/* long filenames have '/' as the first character */
	if (ar.formatted.name[0] == '/') {
#ifdef CONFIG_FEATURE_AR_LONG_FILENAMES
		if (ar.formatted.name[1] == '/') {
			/* If the second char is a '/' then this entries data section
			 * stores long filename for multiple entries, they are stored
			 * in static variable long_names for use in future entries */
			ar_long_name_size = typed->size;
			ar_long_names = xmalloc(ar_long_name_size);
			xread(archive_handle->src_fd, ar_long_names, ar_long_name_size);
			archive_handle->offset += ar_long_name_size;
			/* This ar entries data section only contained filenames for other records
			 * they are stored in the static ar_long_names for future reference */
			return (get_header_ar(archive_handle)); /* Return next header */
		} else if (ar.formatted.name[1] == ' ') {
			/* This is the index of symbols in the file for compilers */
			data_skip(archive_handle);
			archive_handle->offset += typed->size;
			return (get_header_ar(archive_handle)); /* Return next header */
		} else {
			/* The number after the '/' indicates the offset in the ar data section
			(saved in variable long_name) that conatains the real filename */
			const unsigned int long_offset = atoi(&ar.formatted.name[1]);
			if (long_offset >= ar_long_name_size) {
				bb_error_msg_and_die("can't resolve long filename");
			}
			typed->name = xstrdup(ar_long_names + long_offset);
		}
#else
		bb_error_msg_and_die("long filenames not supported");
#endif
	} else {
		/* short filenames */
		typed->name = xstrndup(ar.formatted.name, 16);
	}

	typed->name[strcspn(typed->name, " /")] = '\0';

	if (archive_handle->filter(archive_handle) == EXIT_SUCCESS) {
		archive_handle->action_header(typed);
		if (archive_handle->sub_archive) {
			while (archive_handle->action_data_subarchive(archive_handle->sub_archive) == EXIT_SUCCESS);
		} else {
			archive_handle->action_data(archive_handle);
		}
	} else {
		data_skip(archive_handle);
	}

	archive_handle->offset += typed->size;
	/* Set the file pointer to the correct spot, we may have been reading a compressed file */
	lseek(archive_handle->src_fd, archive_handle->offset, SEEK_SET);

	return(EXIT_SUCCESS);
}
