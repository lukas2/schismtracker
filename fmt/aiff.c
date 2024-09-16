/*
 * Schism Tracker - a cross-platform Impulse Tracker clone
 * copyright (c) 2003-2005 Storlek <storlek@rigelseven.com>
 * copyright (c) 2005-2008 Mrs. Brisby <mrs.brisby@nimh.org>
 * copyright (c) 2009 Storlek & Mrs. Brisby
 * copyright (c) 2010-2012 Storlek
 * URL: http://schismtracker.org/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* --------------------------------------------------------------------- */

#include "headers.h"
#include "bswap.h"
#include "log.h"
#include "fmt.h"

#include <stdint.h>
#include <unistd.h> /* swab */
#include <math.h> /* for ldexp/frexp */

#ifdef SCHISM_WIN32
# define swab(a,b,c) swab((const char*)(a),(char*)(b),(size_t)(c))
#endif

static void ConvertToIeeeExtended(double num, unsigned char *bytes);
static double ConvertFromIeeeExtended(const unsigned char *bytes);

/* --------------------------------------------------------------------- */

#pragma pack(push, 1)

struct aiff_chunk_vhdr {
	uint32_t smp_highoct_1shot;
	uint32_t smp_highoct_repeat;
	uint32_t smp_cycle_highoct;
	uint16_t smp_per_sec;
	uint8_t num_octaves;
	uint8_t compression; // 0 = none, 1 = fibonacci-delta
	uint32_t volume; // fixed point, 65536 = 1.0
};

SCHISM_BINARY_STRUCT(struct aiff_chunk_vhdr, 20);

struct aiff_chunk_comm {
	uint16_t num_channels;
	uint32_t num_frames;
	uint16_t sample_size;
	uint8_t sample_rate[80]; // IEEE-extended
};

SCHISM_BINARY_STRUCT(struct aiff_chunk_comm, 88);

#pragma pack(pop)

// other chunks that might exist: "NAME", "AUTH", "ANNO", "(c) "

// Wish I could do this:
//#define ID(x) ((0[#x] << 24) | (1[#x] << 16) | (2[#x] << 8) | (3[#x]))
// It works great, but gcc doesn't think it's a constant, so it can't be used in a switch.
// (although with -O, it definitely does optimize it into a constant...)

#define ID_FORM 0x464F524D
#define ID_8SVX 0x38535658
#define ID_VHDR 0x56484452
#define ID_BODY 0x424F4459
#define ID_NAME 0x4E414D45
#define ID_AUTH 0x41555448
#define ID_ANNO 0x414E4E4F
#define ID__C__ 0x28632920 /* "(c) " */
#define ID_AIFF 0x41494646
#define ID_COMM 0x434F4D4D
#define ID_SSND 0x53534E44

/* --------------------------------------------------------------------- */

static int read_iff_(dmoz_file_t *file, song_sample_t *smp, slurp_t *fp)
{
	uint32_t filetype = 0;
	iff_chunk_t chunk = {0};
	iff_chunk_t vhdr = {0}, body = {0}, name = {0}, auth = {0}, anno = {0}, ssnd = {0}, comm = {0};

	if (!iff_chunk_peek(&chunk, fp))
		return 0;

	if (chunk.id != ID_FORM)
		return 0;

	if (iff_chunk_read(&chunk, fp, &filetype, sizeof(filetype)) != sizeof(filetype))
		return 0;

	// jump "into" the FORM chunk
	slurp_seek(fp, chunk.offset + sizeof(filetype), SEEK_SET);

	switch (bswapBE32(filetype)) {
	case ID_8SVX: {
		struct aiff_chunk_vhdr chunk_vhdr;

		while (iff_chunk_peek(&chunk, fp)) {
			switch (chunk.id) {
				case ID_VHDR: vhdr = chunk; break;
				case ID_BODY: body = chunk; break;
				case ID_NAME: name = chunk; break;
				case ID_AUTH: auth = chunk; break;
				case ID_ANNO: anno = chunk; break;
				default: break;
			}
		}
		if (!(vhdr.id && body.id))
			return 0;

		iff_chunk_read(&vhdr, fp, &chunk_vhdr, sizeof(chunk_vhdr));

		if (chunk_vhdr.compression) {
			log_appendf(4, "error: compressed 8SVX files are unsupported");
			return 0;
		}
		if (chunk_vhdr.num_octaves != 1) {
			log_appendf(4, "warning: 8SVX file contains %d octaves",
				chunk_vhdr.num_octaves);
		}

		if (file) {
			file->smp_speed = bswapBE16(chunk_vhdr.smp_per_sec);
			file->smp_length = body.size;

			file->description = "8SVX sample";
			file->type = TYPE_SAMPLE_PLAIN;
		}
		if (!name.id) name = auth;
		if (!name.id) name = anno;
		if (name.id) {
			unsigned char title[name.size];

			iff_chunk_read(&name, fp, title, sizeof(title));

			if (file) file->title = strn_dup(title, sizeof(title));
			if (smp) {
				size_t len = MIN(sizeof(smp->name), sizeof(title));
				memcpy(smp->name, title, len);
				smp->name[len] = 0;
			}
		}

		if (smp) {
			smp->c5speed = bswapBE16(chunk_vhdr.smp_per_sec);
			smp->length = body.size;

			iff_read_sample(&body, fp, smp, SF_BE | SF_PCMS | SF_8 | SF_M, 0);

			smp->volume = 64*4;
			smp->global_volume = 64;

			// this is done kinda weird
			smp->loop_end = bswapBE32(chunk_vhdr.smp_highoct_repeat);
			if (smp->loop_end) {
				smp->loop_start = bswapBE32(chunk_vhdr.smp_highoct_1shot);
				smp->loop_end += smp->loop_start;
				if (smp->loop_start > smp->length)
					smp->loop_start = 0;
				if (smp->loop_end > smp->length)
					smp->loop_end = smp->length;
				if (smp->loop_start + 2 < smp->loop_end)
					smp->flags |= CHN_LOOP;
			}
		}

		return 1;
	}
	case ID_AIFF: {
		struct aiff_chunk_comm chunk_comm;

		while (iff_chunk_peek(&chunk, fp)) {
			switch (chunk.id) {
				case ID_COMM: comm = chunk; break;
				case ID_SSND: ssnd = chunk; break;
				case ID_NAME: name = chunk; break;
				default: break;
			}
		}
		if (!(comm.id && ssnd.id))
			return 0;

		iff_chunk_read(&comm, fp, &chunk_comm, sizeof(chunk_comm));

		if (file) {
			file->smp_speed = ConvertFromIeeeExtended(chunk_comm.sample_rate);
			file->smp_length = bswapBE32(chunk_comm.num_frames);

			file->description = "Audio IFF sample";
			file->type = TYPE_SAMPLE_PLAIN;
		}

		if (name.id) {
			uint8_t title[name.size];

			iff_chunk_read(&chunk, fp, title, sizeof(title));

			if (file) file->title = strn_dup(title, sizeof(title));
			if (smp) {
				int len = MIN(sizeof(smp->name), sizeof(title));
				memcpy(smp->name, title, len);
				smp->name[len] = 0;
			}
		}

		/* TODO loop points */

		if (smp) {
			uint32_t flags = SF_BE | SF_PCMS;

			switch (bswapBE16(chunk_comm.num_channels)) {
			default:
				log_appendf(4, "warning: multichannel AIFF is unsupported");
			case 1:
				flags |= SF_M;
				break;
			case 2:
				flags |= SF_SI;
				break;
			}

			switch ((bswapBE16(chunk_comm.sample_size) + 7) & ~7) {
			default:
				log_appendf(4, "warning: AIFF has unsupported bit-width");
			case 8:
				flags |= SF_8;
				break;
			case 16:
				flags |= SF_16;
				break;
			case 24:
				flags |= SF_24;
				break;
			case 32:
				flags |= SF_32;
				break;
			}

			// TODO: data checking; make sure sample count and byte size agree
			// (and if not, cut to shorter of the two)

			smp->c5speed = ConvertFromIeeeExtended(chunk_comm.sample_rate);
			smp->length = bswapBE32(chunk_comm.num_frames);
			smp->volume = 64*4;
			smp->global_volume = 64;

			// the audio data starts 8 bytes into the chunk
			// (don't care about the block alignment stuff)
			iff_read_sample(&ssnd, fp, smp, flags, 8);
		}

		return 1;
	}
	default:
		break;
	}

	return 0;
}

/* --------------------------------------------------------------------- */

int fmt_aiff_read_info(dmoz_file_t *file, slurp_t *fp)
{
	return read_iff_(file, NULL, fp);
}

int fmt_aiff_load_sample(slurp_t *fp, song_sample_t *smp)
{
	return read_iff_(NULL, smp, fp);
}

/* --------------------------------------------------------------------- */

struct aiff_writedata {
	long comm_frames, ssnd_size; // seek positions for writing header data
	size_t numbytes; // how many bytes have been written
	int bps; // bytes per sample
	int swap; // should be byteswapped?
};

static int aiff_header(disko_t *fp, int bits, int channels, int rate,
	const char *name, size_t length, struct aiff_writedata *awd /* out */)
{
	int16_t s;
	uint32_t ul;
	int tlen, bps = 1;
	uint8_t b[10];

	bps *= ((bits + 7) / 8);
	/* note: channel multiply is done below -- need single-channel value for the COMM chunk */

	/* write a very large size for now */
	disko_write(fp, "FORM\377\377\377\377AIFF", 12);

	if (name && *name) {
		disko_write(fp, "NAME", 4);
		tlen = strlen(name);
		ul = (tlen + 1) & ~1; /* must be even */
		ul = bswapBE32(ul);
		disko_write(fp, &ul, 4);
		disko_write(fp, name, tlen);
		if (tlen & 1)
			disko_putc(fp, '\0');
	}

	/* Common Chunk
		The Common Chunk describes fundamental parameters of the sampled sound.
	typedef struct {
		ID              ckID;           // 'COMM'
		long            ckSize;         // 18
		short           numChannels;
		unsigned long   numSampleFrames;
		short           sampleSize;
		extended        sampleRate;
	} CommonChunk; */
	disko_write(fp, "COMM", 4);
	ul = bswapBE32(18); /* chunk size -- won't change */
	disko_write(fp, &ul, 4);
	s = bswapBE16(channels);
	disko_write(fp, &s, 2);
	if (awd)
		awd->comm_frames = disko_tell(fp);
	ul = bswapBE32(length); /* num sample frames */
	disko_write(fp, &ul, 4);
	s = bswapBE16(bits);
	disko_write(fp, &s, 2);
	ConvertToIeeeExtended(rate, b);
	disko_write(fp, b, 10);

	/* NOW do this (sample size in AIFF is indicated per channel, not per frame) */
	bps *= channels; /* == number of bytes per (stereo) sample */

	/* Sound Data Chunk
		The Sound Data Chunk contains the actual sample frames.
	typedef struct {
		ID              ckID;           // 'SSND'
		long            ckSize;         // data size in bytes, *PLUS EIGHT* (for offset and blockSize)
		unsigned long   offset;         // just set this to 0...
		unsigned long   blockSize;      // likewise
		unsigned char   soundData[];
	} SoundDataChunk; */
	disko_write(fp, "SSND", 4);
	if (awd)
		awd->ssnd_size = disko_tell(fp);
	ul = bswapBE32(length * bps + 8);
	disko_write(fp, &ul, 4);
	ul = bswapBE32(0);
	disko_write(fp, &ul, 4);
	disko_write(fp, &ul, 4);

	return bps;
}

/* --------------------------------------------------------------------- */

int fmt_aiff_save_sample(disko_t *fp, song_sample_t *smp)
{
	int bps;
	uint32_t ul;
	uint32_t flags = SF_BE | SF_PCMS;
	flags |= (smp->flags & CHN_16BIT) ? SF_16 : SF_8;
	flags |= (smp->flags & CHN_STEREO) ? SF_SI : SF_M;

	bps = aiff_header(fp, (smp->flags & CHN_16BIT) ? 16 : 8, (smp->flags & CHN_STEREO) ? 2 : 1,
		smp->c5speed, smp->name, smp->length, NULL);

	if (csf_write_sample(fp, smp, flags, UINT32_MAX) != smp->length * bps) {
		log_appendf(4, "AIFF: unexpected data size written");
		return SAVE_INTERNAL_ERROR;
	}

	/* TODO: loop data */

	/* fix the length in the file header */
	ul = disko_tell(fp) - 8;
	ul = bswapBE32(ul);
	disko_seek(fp, 4, SEEK_SET);
	disko_write(fp, &ul, 4);

	return SAVE_SUCCESS;
}


int fmt_aiff_export_head(disko_t *fp, int bits, int channels, int rate)
{
	struct aiff_writedata *awd = malloc(sizeof(struct aiff_writedata));
	if (!awd)
		return DW_ERROR;
	fp->userdata = awd;
	awd->bps = aiff_header(fp, bits, channels, rate, NULL, ~0, awd);
	awd->numbytes = 0;
#if WORDS_BIGENDIAN
	awd->swap = 0;
#else
	awd->swap = (bits > 8);
#endif

	return DW_OK;
}

int fmt_aiff_export_body(disko_t *fp, const uint8_t *data, size_t length)
{
	struct aiff_writedata *awd = fp->userdata;

	if (length % awd->bps) {
		log_appendf(4, "AIFF export: received uneven length");
		return DW_ERROR;
	}

	awd->numbytes += length;

	if (awd->swap) {
		const int16_t *ptr = (const int16_t *) data;
		uint16_t v;

		length /= 2;
		while (length--) {
			v = *ptr;
			v = bswapBE16(v);
			disko_write(fp, &v, 2);
			ptr++;
		}
	} else {
		disko_write(fp, data, length);
	}

	return DW_OK;
}

int fmt_aiff_export_silence(disko_t *fp, long bytes)
{
	struct aiff_writedata *awd = fp->userdata;
	awd->numbytes += bytes;

	disko_seek(fp, bytes, SEEK_CUR);
	return DW_OK;
}

int fmt_aiff_export_tail(disko_t *fp)
{
	struct aiff_writedata *awd = fp->userdata;
	uint32_t ul;

	/* fix the length in the file header */
	ul = disko_tell(fp) - 8;
	ul = bswapBE32(ul);
	disko_seek(fp, 4, SEEK_SET);
	disko_write(fp, &ul, 4);

	/* write the other lengths */
	disko_seek(fp, awd->comm_frames, SEEK_SET);
	ul = bswapBE32(awd->numbytes / awd->bps);
	disko_write(fp, &ul, 4);
	disko_seek(fp, awd->ssnd_size, SEEK_SET);
	ul = bswapBE32(awd->numbytes + 8);
	disko_write(fp, &ul, 4);

	free(awd);

	return DW_OK;
}

/* --------------------------------------------------------------------- */
/* Copyright (C) 1988-1991 Apple Computer, Inc.
 * All rights reserved.
 *
 * Machine-independent I/O routines for IEEE floating-point numbers.
 *
 * NaN's and infinities are converted to HUGE_VAL or HUGE, which
 * happens to be infinity on IEEE machines.  Unfortunately, it is
 * impossible to preserve NaN's in a machine-independent way.
 * Infinities are, however, preserved on IEEE machines.
 *
 * These routines have been tested on the following machines:
 *    Apple Macintosh, MPW 3.1 C compiler
 *    Apple Macintosh, THINK C compiler
 *    Silicon Graphics IRIS, MIPS compiler
 *    Cray X/MP and Y/MP
 *    Digital Equipment VAX
 *
 *
 * Implemented by Malcolm Slaney and Ken Turkowski.
 *
 * Malcolm Slaney contributions during 1988-1990 include big- and little-
 * endian file I/O, conversion to and from Motorola's extended 80-bit
 * floating-point format, and conversions to and from IEEE single-
 * precision floating-point format.
 *
 * In 1991, Ken Turkowski implemented the conversions to and from
 * IEEE double-precision format, added more precision to the extended
 * conversions, and accommodated conversions involving +/- infinity,
 * NaN's, and denormalized numbers.
 */

/* XXX this crap should be moved into util.c */

#ifndef HUGE_VAL
# define HUGE_VAL HUGE
#endif /* HUGE_VAL */

#define FloatToUnsigned(f) ((uint32_t) (((int32_t) (f - 2147483648.0)) + 2147483647L + 1))
#define UnsignedToFloat(u) (((double) ((int32_t) (u - 2147483647L - 1))) + 2147483648.0)

static void ConvertToIeeeExtended(double num, unsigned char *bytes)
{
	int sign, expon;
	double fMant, fsMant;
	uint32_t hiMant, loMant;

	if (num < 0) {
		sign = 0x8000;
		num *= -1;
	} else {
		sign = 0;
	}

	if (num == 0) {
		expon = 0;
		hiMant = 0;
		loMant = 0;
	} else {
		fMant = frexp(num, &expon);
		if ((expon > 16384) || !(fMant < 1)) {
			/* Infinity or NaN */
			expon = sign | 0x7FFF;
			hiMant = 0;
			loMant = 0; /* infinity */
		} else {
			/* Finite */
			expon += 16382;
			if (expon < 0) {
				/* denormalized */
				fMant = ldexp(fMant, expon);
				expon = 0;
			}
			expon |= sign;
			fMant = ldexp(fMant, 32);
			fsMant = floor(fMant);
			hiMant = FloatToUnsigned(fsMant);
			fMant = ldexp(fMant - fsMant, 32);
			fsMant = floor(fMant);
			loMant = FloatToUnsigned(fsMant);
		}
	}

	bytes[0] = expon >> 8;
	bytes[1] = expon;
	bytes[2] = hiMant >> 24;
	bytes[3] = hiMant >> 16;
	bytes[4] = hiMant >> 8;
	bytes[5] = hiMant;
	bytes[6] = loMant >> 24;
	bytes[7] = loMant >> 16;
	bytes[8] = loMant >> 8;
	bytes[9] = loMant;
}

static double ConvertFromIeeeExtended(const unsigned char *bytes)
{
	double f;
	int expon;
	uint32_t hiMant, loMant;

	expon = ((bytes[0] & 0x7F) << 8) | (bytes[1] & 0xFF);
	hiMant = ((uint32_t) (bytes[2] & 0xFF) << 24)
		| ((uint32_t) (bytes[3] & 0xFF) << 16)
		| ((uint32_t) (bytes[4] & 0xFF) << 8)
		| ((uint32_t) (bytes[5] & 0xFF));
	loMant = ((uint32_t) (bytes[6] & 0xFF) << 24)
		| ((uint32_t) (bytes[7] & 0xFF) << 16)
		| ((uint32_t) (bytes[8] & 0xFF) << 8)
		| ((uint32_t) (bytes[9] & 0xFF));

	if (expon == 0 && hiMant == 0 && loMant == 0) {
		f = 0;
	} else if (expon == 0x7FFF) {
		/* Infinity or NaN */
		f = HUGE_VAL;
	} else {
		expon -= 16383;
		f = ldexp(UnsignedToFloat(hiMant), expon -= 31);
		f += ldexp(UnsignedToFloat(loMant), expon -= 32);
	}

	if (bytes[0] & 0x80)
		return -f;
	else
		return f;
}
