/* radcom.c
 *
 * $Id: radcom.c,v 1.12 1999/09/24 05:49:52 guy Exp $
 *
 * Wiretap Library
 * Copyright (c) 1998 by Gilbert Ramirez <gram@verdict.uthscsa.edu>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include "wtap.h"
#include "file.h"
#include "buffer.h"
#include "radcom.h"

struct frame_date {
	guint16	year;
	guint8	month;
	guint8	day;
	guint32	sec;		/* seconds since midnight */
	guint32	usec;
};

struct unaligned_frame_date {
	char	year[2];
	char	month;
	char	day;
	char	sec[4];		/* seconds since midnight */
	char	usec[4];
};

static char radcom_magic[8] = {
	0x42, 0xD2, 0x00, 0x34, 0x12, 0x66, 0x22, 0x88
};

/* RADCOM record header - followed by frame data (perhaps including FCS).
   The first two bytes of "xxz" appear to equal "length", as do the
   second two bytes; if a RADCOM box can be told not to save all of
   the captured packet, might one or the other of those be the
   captured length of the packet? */
struct radcomrec_hdr {
	char	xxx[4];		/* unknown */
	char	length[2];	/* packet length */
	char	xxy[5];		/* unknown */
	struct unaligned_frame_date date; /* date/time stamp of packet */
	char	xxz[6];		/* unknown */
	char	dce;		/* DCE/DTE flag (and other flags?) */
	char	xxw[9];		/* unknown */
};

static int radcom_read(wtap *wth, int *err);

int radcom_open(wtap *wth, int *err)
{
	int bytes_read;
	char magic[8];
	struct frame_date start_date;
	guint32 sec;
	struct tm tm;
	char byte;
	char encap_magic[7] = {0x54, 0x43, 0x50, 0x00, 0x42, 0x43, 0x09};
	char search_encap[7];

	/* Read in the string that should be at the start of a RADCOM file */
	file_seek(wth->fh, 0, SEEK_SET);
	errno = WTAP_ERR_CANT_READ;
	bytes_read = file_read(magic, 1, 8, wth->fh);
	if (bytes_read != 8) {
		if (file_error(wth->fh)) {
			*err = errno;
			return -1;
		}
		return 0;
	}

	if (memcmp(magic, radcom_magic, 8)) {
		return 0;
	}

	file_seek(wth->fh, 0x8B, SEEK_SET);
	wth->data_offset = 0x8B;
	errno = WTAP_ERR_CANT_READ;
	bytes_read = file_read(&byte, 1, 1, wth->fh);
	if (bytes_read != 1) {
		if (file_error(wth->fh)) {
			*err = errno;
			return -1;
		}
		return 0;
	}
	wth->data_offset += 1;
	while (byte) {
		errno = WTAP_ERR_CANT_READ;
		bytes_read = file_read(&byte, 1, 1, wth->fh);
		if (bytes_read != 1) {
			if (file_error(wth->fh)) {
				*err = errno;
				return -1;
			}
			return 0;
		}
		wth->data_offset += 1;
	}
	file_seek(wth->fh, 1, SEEK_CUR);
	wth->data_offset += 1;

	/* Get capture start time */
	errno = WTAP_ERR_CANT_READ;
	bytes_read = file_read(&start_date, 1, sizeof(struct frame_date), wth->fh);
	if (bytes_read != sizeof(struct frame_date)) {
		if (file_error(wth->fh)) {
			*err = errno;
			return -1;
		}
		return 0;
	}
	wth->data_offset += sizeof(struct frame_date);

	/* This is a radcom file */
	wth->file_type = WTAP_FILE_RADCOM;
	wth->capture.radcom = g_malloc(sizeof(radcom_t));
	wth->subtype_read = radcom_read;
	wth->snapshot_length = 16384;	/* not available in header, only in frame */

	tm.tm_year = pletohs(&start_date.year)-1900;
	tm.tm_mon = start_date.month-1;
	tm.tm_mday = start_date.day;
	sec = pletohl(&start_date.sec);
	tm.tm_hour = sec/3600;
	tm.tm_min = (sec%3600)/60;
	tm.tm_sec = sec%60;
	tm.tm_isdst = -1;
	wth->capture.radcom->start = mktime(&tm);

	file_seek(wth->fh, sizeof(struct frame_date), SEEK_CUR);
	wth->data_offset += sizeof(struct frame_date);

	errno = WTAP_ERR_CANT_READ;
	bytes_read = file_read(search_encap, 1, 7, wth->fh);
	if (bytes_read != 7) {
		goto read_error;
	}
	wth->data_offset += 7;
	while (memcmp(encap_magic, search_encap, 7)) {
		file_seek(wth->fh, -6, SEEK_CUR);
		wth->data_offset -= 6;
		errno = WTAP_ERR_CANT_READ;
		bytes_read = file_read(search_encap, 1, 7, wth->fh);
		if (bytes_read != 7) {
			goto read_error;
		}
		wth->data_offset += 7;
	}
	file_seek(wth->fh, 12, SEEK_CUR);
	wth->data_offset += 12;
	errno = WTAP_ERR_CANT_READ;
	bytes_read = file_read(search_encap, 1, 4, wth->fh);
	if (bytes_read != 4) {
		goto read_error;
	}
	wth->data_offset += 4;
	if (!memcmp(search_encap, "LAPB", 4))
		wth->file_encap = WTAP_ENCAP_LAPB;
	else if (!memcmp(search_encap, "Ethe", 4))
		wth->file_encap = WTAP_ENCAP_ETHERNET;
	else {
		g_message("pcap: network type \"%.4s\" unknown", search_encap);
		*err = WTAP_ERR_UNSUPPORTED;
		return -1;
	}

	/*bytes_read = file_read(&next_date, 1, sizeof(struct frame_date), wth->fh);
	errno = WTAP_ERR_CANT_READ;
	if (bytes_read != sizeof(struct frame_date)) {
		goto read_error;
	}

	while (memcmp(&start_date, &next_date, 4)) {
		file_seek(wth->fh, 1-sizeof(struct frame_date), SEEK_CUR);
		errno = WTAP_ERR_CANT_READ;
		bytes_read = file_read(&next_date, 1, sizeof(struct frame_date),
				   wth->fh);
		if (bytes_read != sizeof(struct frame_date)) {
			goto read_error;
		}
	}*/

	if (wth->file_encap == WTAP_ENCAP_ETHERNET) {
		file_seek(wth->fh, 294, SEEK_CUR);
		wth->data_offset += 294;
	} else if (wth->file_encap == WTAP_ENCAP_LAPB) {
		file_seek(wth->fh, 297, SEEK_CUR);
		wth->data_offset += 297;
	}

	return 1;

read_error:
	if (file_error(wth->fh)) {
		*err = errno;
		free(wth->capture.radcom);
		return -1;
	}
	free(wth->capture.radcom);
	return 0;
}

/* Read the next packet */
static int radcom_read(wtap *wth, int *err)
{
	int	bytes_read;
	struct radcomrec_hdr hdr;
	guint16 length;
	guint32 sec;
	struct tm tm;
	int	data_offset;
	char	fcs[2];

	/* Read record header. */
	errno = WTAP_ERR_CANT_READ;
	bytes_read = file_read(&hdr, 1, sizeof hdr, wth->fh);
	if (bytes_read != sizeof hdr) {
		if (file_error(wth->fh)) {
			*err = errno;
			return -1;
		}
		if (bytes_read != 0) {
			*err = WTAP_ERR_SHORT_READ;
			return -1;
		}
		return 0;
	}
	wth->data_offset += sizeof hdr;
	length = pletohs(&hdr.length);
	if (length == 0) return 0;

	if (wth->file_encap == WTAP_ENCAP_LAPB)
		length -= 2; /* FCS */

	wth->phdr.len = length;
	wth->phdr.caplen = length;

	tm.tm_year = pletohs(&hdr.date.year)-1900;
	tm.tm_mon = hdr.date.month-1;
	tm.tm_mday = hdr.date.day;
	sec = pletohl(&hdr.date.sec);
	tm.tm_hour = sec/3600;
	tm.tm_min = (sec%3600)/60;
	tm.tm_sec = sec%60;
	tm.tm_isdst = -1;
	wth->phdr.ts.tv_sec = mktime(&tm);
	wth->phdr.ts.tv_usec = pletohl(&hdr.date.usec);
	wth->phdr.pseudo_header.x25.flags = (hdr.dce & 0x1) ? 0x00 : 0x80;

	/*
	 * Read the packet data.
	 */
	buffer_assure_space(wth->frame_buffer, length);
	data_offset = wth->data_offset;
	errno = WTAP_ERR_CANT_READ;
	bytes_read = file_read(buffer_start_ptr(wth->frame_buffer), 1,
			length, wth->fh);

	if (bytes_read != length) {
		if (file_error(wth->fh))
			*err = errno;
		else
			*err = WTAP_ERR_SHORT_READ;
		return -1;
	}
	wth->data_offset += length;

	wth->phdr.pkt_encap = wth->file_encap;

	if (wth->file_encap == WTAP_ENCAP_LAPB) {
		/* Read the FCS.
		   XXX - should we put it in the pseudo-header? */
		errno = WTAP_ERR_CANT_READ;
		bytes_read = file_read(&fcs, 1, sizeof fcs, wth->fh);
		if (bytes_read != sizeof fcs) {
			if (file_error(wth->fh))
				*err = errno;
			else
				*err = WTAP_ERR_SHORT_READ;
			return -1;
		}
		wth->data_offset += sizeof fcs;
	}

	return data_offset;
}
