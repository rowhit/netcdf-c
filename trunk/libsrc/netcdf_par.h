/*
 * Copyright 2010 University Corporation for Atmospheric
 * Research/Unidata. See COPYRIGHT file for more info.
 *
 * This header file is for the parallel I/O functions of netCDF.
 * 
 */
/* "$Id: netcdf_par.h,v 2.2 2010/05/26 20:13:31 dmh Exp $" */

#ifndef NETCDF_PAR_H
#define NETCDF_PAR_H 1

#include <mpi.h>

/* Keep C++ compilers from getting confused */
#if defined(__cplusplus)
extern "C" {
#endif

/* Use these with nc_var_par_access(). */
#define NC_INDEPENDENT 0
#define NC_COLLECTIVE 1

/* Create a file and enable parallel I/O. */
extern int
nc_create_par(const char *path, int cmode, MPI_Comm comm, MPI_Info info,
	      int *ncidp);

/* Open a file and enable parallel I/O. */
extern int
nc_open_par(const char *path, int mode, MPI_Comm comm, MPI_Info info,
	    int *ncidp);

/* Change a variable from independent (the default) to collective
 * access. */
extern int
nc_var_par_access(int ncid, int varid, int par_access);

extern int
nc_create_par_fortran(const char *path, int cmode, int comm, 
		      int info, int *ncidp);
extern int
nc_open_par_fortran(const char *path, int mode, int comm, 
		    int info, int *ncidp);

#if defined(__cplusplus)
}

#endif /* NETCDF_PAR_H */
