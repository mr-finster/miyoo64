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

#include "mbi.h"
#include "gu.h"

#if defined(PORT) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
/* Port: the tables are written as {high word, low word} of each double -- the
 * N64's big-endian memory order. Keep them in that form and assemble the double
 * in host order on use (read through DU_D), so sinf/cosf work on little-endian
 * hosts (as a union they decode to garbage: sinf returned inf). */
typedef struct
{
	unsigned int hi;
	unsigned int lo;
} du;
static inline double du_to_double(du x) {
	union { unsigned long long u; double d; } c;
	c.u = ((unsigned long long) x.hi << 32) | x.lo;
	return c.d;
}
#define DU_D(x) du_to_double(x)
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
#define DU_D(x) ((x).d)
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
