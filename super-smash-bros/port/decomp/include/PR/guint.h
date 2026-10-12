/**************************************************************************
 *									  *
 *		 Copyright (C) 1994, Silicon Graphics, Inc.		  *
 *									  *
 *  These coded instructions, statements, and computer programs  contain  *
 *  unpublished  proprietary  information of Silicon Graphics, Inc., and  *
 *  are protected by Federal copyright law.  They  may  not be disclosed  *
 *  to  third  parties  or copied or duplicated in any form, in whole or  *
 *  in part, without the prior written consent of Silicon Graphics, Inc.  *
 *									  *
 **************************************************************************/

#include <PR/mbi.h>
#include <PR/gu.h>

/* The du constant tables in sinf.c/cosf.c assemble doubles from {hi, lo}
 * 32-bit word pairs laid out in the N64's big-endian order. On little-endian
 * hosts (every PC/mobile port target) both the member order and the
 * positional initializers must flip so .d reassembles the same double —
 * initializers fill ascending addresses no matter what the members are
 * named, so the swap has to happen in the initializer too (DU_INIT below).
 * IDO and any other big-endian build keeps the original order via the
 * #else branches and preprocesses to the original source, byte-identical. */
#if defined(_MSC_VER) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define DU_LITTLE_ENDIAN 1
#endif

typedef union
{
	struct
	{
#ifdef DU_LITTLE_ENDIAN
		unsigned int lo;
		unsigned int hi;
#else
		unsigned int hi;
		unsigned int lo;
#endif
	} word;

	double d;
} du;

#ifdef DU_LITTLE_ENDIAN
#define DU_INIT(hi_, lo_) { lo_, hi_ }
#else
#define DU_INIT(hi_, lo_) { hi_, lo_ }
#endif

typedef union
{
	unsigned int i;
	float f;
} fu;

#ifndef __GL_GL_H__

typedef float Matrix[4][4];

#endif

#define ROUND(d) (int)(((d) >= 0.0) ? ((d) + 0.5) : ((d)-0.5))
#define ABS(d) ((d) > 0) ? (d) : -(d)

extern float __libm_qnan_f;
