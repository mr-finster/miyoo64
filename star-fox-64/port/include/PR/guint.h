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

#include "PR/mbi.h"
#include "PR/gu.h"

/* A double as two words. The N64 is big-endian (high word first); on a
 * little-endian port (handheld/desktop) the low word comes first in memory, so
 * the field order flips and DU_INIT -- the libm constant tables, written high
 * word first -- swaps its arguments. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
typedef union
{
	struct
	{
		unsigned int lo;
		unsigned int hi;
	} word;

	double	d;
} du;
#define DU_INIT(hi, lo) { { (lo), (hi) } }
#else
typedef union
{
	struct
	{
		unsigned int hi;
		unsigned int lo;
	} word;

	double	d;
} du;
#define DU_INIT(hi, lo) { { (hi), (lo) } }
#endif

typedef union
{
	unsigned int	i;
	float		f;
} fu;

#ifndef __GL_GL_H__

typedef	float	Matrix[4][4];

#endif

#define ROUND(d)	(int)(((d) >= 0.0) ? ((d) + 0.5) : ((d) - 0.5))
#define	ABS(d)		((d) > 0) ? (d) : -(d)

extern float	__libm_qnan_f;
