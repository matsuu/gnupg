/* mpihelp-sub.c  -  MPI helper functions
 *	Copyright (C) 1998 Free Software Foundation, Inc.
 *	Copyright (C) 1994, 1996 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 *
 * Note: This code is heavily based on the GNU MP Library.
 *	 Actually it's the same code with only minor changes in the
 *	 way the data is stored; this is to support the abstraction
 *	 of an optional secure memory allocation which may be used
 *	 to avoid revealing of sensitive data due to paging etc.
 *	 The GNU MP Library itself is published under the LGPL;
 *	 however I decided to publish this code under the plain GPL.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>

#include "mpi-internal.h"

#ifndef __GNUC__
mpi_limb_t
mpihelp_sub_1(mpi_ptr_t res_ptr,  mpi_ptr_t s1_ptr,
	      mpi_size_t s1_size, mpi_limb_t s2_limb )
{
    mpi_limb_t x;

    x = *s1_ptr++;
    s2_limb = x - s2_limb;
    *res_ptr++ = s2_limb;
    if( s2_limb > x ) {
	while( --s1_size ) {
	    x = *s1_ptr++;
	    *res_ptr++ = x - 1;
	    if( x )
		goto leave;
	}
	return 1;
    }

  leave:
    if( res_ptr != s1_ptr ) {
	mpi_size_t i;
	for( i=0; i < s1_size-1; i++ )
	    res_ptr[i] = s1_ptr[i];
    }
    return 0;
}


mpi_limb_t
mpihelp_sub( mpi_ptr_t res_ptr, mpi_ptr_t s1_ptr, mpi_size_t s1_size,
				mpi_ptr_t s2_ptr, mpi_size_t s2_size)
{
    mpi_limb_t cy = 0;

    if( s2_size )
	cy = mpihelp_sub_n(res_ptr, s1_ptr, s2_ptr, s2_size);

    if( s1_size - s2_size )
	cy = mpihelp_sub_1(res_ptr + s2_size, s1_ptr + s2_size,
				      s1_size - s2_size, cy);
    return cy;
}
#endif

