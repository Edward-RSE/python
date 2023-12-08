
/***********************************************************/
/** @file  para_update.c
 * @author ksl, jm
 * @date   January, 2018
 *
 * @brief  routines for communicating MC estimators and spectra between MPI ranks.
 *
 * @TODO: as much as this as possible should use non-blocking communication
 *
 ***********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "atomic.h"
#include "python.h"

/**********************************************************/
/**
 * @brief helper routine for splitting up tasks in MPI
 * @param   [in]      int   rank       processor rank (typically set from rank_global)
 * @param   [in]      int   ntotal     total number of tasks, e.g. NPLASMA
 * @param   [in]       int   nproc      total number of MPI processors
 * @param   [in,out]  int   *my_nmax   pointer to integer value of first task
 * @param   [in,out]  int   *my_nmax   pointer to integer value of final task
 * @return            int   ndo        number of tasks this thread is working on

 * @details  For a process with ntotal tasks,
 * this routine calculates which thread will be given each task.
 * typically ntotal is NPLASMA and the thread is splitting up wind cells.
 * The routine deals with remainders by distributing the remainder over the
 * threads if the cells do not divide evenly by thread
 **********************************************************/

int
get_parallel_nrange (int rank, int ntotal, int nproc, int *my_nmin, int *my_nmax)
{
  /* divide the cells between the threads */
  int ndo;
  int num_mpi_cells = floor ((double) ntotal / nproc);

  /* the remainder from the above division */
  int num_mpi_extra = ntotal - (nproc * num_mpi_cells);

  /* this section distributes the remainder over the threads if the cells
     do not divide evenly by thread */
  if (rank < num_mpi_extra)
  {
    *my_nmin = rank_global * (num_mpi_cells + 1);
    *my_nmax = (rank_global + 1) * (num_mpi_cells + 1);
  }
  else
  {
    *my_nmin = num_mpi_extra * (num_mpi_cells + 1) + (rank - num_mpi_extra) * (num_mpi_cells);
    *my_nmax = num_mpi_extra * (num_mpi_cells + 1) + (rank - num_mpi_extra + 1) * (num_mpi_cells);
  }
  ndo = *my_nmax - *my_nmin;

  return ndo;
}

/**********************************************************/
/**
 * @brief  Get the max cells a rank will operate on
 *
 * @param [in]  int n_total  The number of cells to split
 *
 * @return  int  The largest number of cells a rank will work on
 *
 * @details
 *
 * This should be used to determine how big a communication buffer should be
 * when each rank is working on a different number of cells. This is required
 * in case of some ranks having a smaller number of cells to work on than the
 * rest. If value from this function is not used, then a MPI_TRUNCATE will
 * likely occur or a segmentation fault.
 *
 **********************************************************/

int
get_max_cells_per_rank (const int n_total)
{
  return ceil ((double) n_total / np_mpi_global);
}

/**********************************************************/
/**
 * @brief Calculate the minimum size for MPI_PACKED comm buffer for ints and doubles
 *
 * @param [in] int num_ints     The number of ints going into the comm buffer
 * @param [in] int num_doubles  The number of doubles going into the comm buffer
 *
 * @return  int  the size of the comm buffer
 *
 * @details
 *
 * This makes use of MPI_Pack_size which takes into account any data alignment
 * or system dependent properties which would affect the size of the
 * communication buffer required. This function was designed to be used for
 * packed buffers. However, there is no reason why it won't work for regular
 * communication buffered communication buffers.
 *
 * As of original writing, only ints and doubles were ever communicated. As more
 * types are introduced, this function should be extended or refactored.
 *
 **********************************************************/

int
calculate_comm_buffer_size (const int num_ints, const int num_doubles)
{
#ifdef MPI_ON
  int int_bytes;
  int double_bytes;

  MPI_Pack_size (num_ints, MPI_INT, MPI_COMM_WORLD, &int_bytes);
  MPI_Pack_size (num_doubles, MPI_DOUBLE, MPI_COMM_WORLD, &double_bytes);

  return int_bytes + double_bytes;
#else
  return 0;
#endif
}

/**********************************************************/
/**
 * @brief sum up the synthetic and cell spectra between threads.
 *
 * @details
 * sum up the synthetic spectra between threads. Does an
 * MPI_Reduce then an MPI_Bcast for each element of the
 * linear and log spectra arrays (xxspec)
 *
 **********************************************************/

int
gather_extracted_spectrum (void)
{
#ifdef MPI_ON
  double *redhelper, *redhelper2;
  int mpi_i, mpi_j;
  int size_of_commbuffer, nspec;

  if (geo.ioniz_or_extract == CYCLE_EXTRACT)
  {
    nspec = MSPEC + geo.nangles;
  }
  else
  {
    nspec = MSPEC;
  }

  size_of_commbuffer = 4 * nspec * NWAVE_MAX;   //we need space for all 4 separate spectra we are normalizing

  redhelper = calloc (sizeof (double), size_of_commbuffer);
  redhelper2 = calloc (sizeof (double), size_of_commbuffer);

  for (mpi_i = 0; mpi_i < NWAVE_MAX; mpi_i++)
  {
    for (mpi_j = 0; mpi_j < nspec; mpi_j++)
    {
      redhelper[mpi_i * nspec + mpi_j] = xxspec[mpi_j].f[mpi_i] / np_mpi_global;
      redhelper[mpi_i * nspec + mpi_j + (NWAVE_MAX * nspec)] = xxspec[mpi_j].lf[mpi_i] / np_mpi_global;
      redhelper[mpi_i * nspec + mpi_j + (2 * NWAVE_MAX * nspec)] = xxspec[mpi_j].f_wind[mpi_i] / np_mpi_global;
      redhelper[mpi_i * nspec + mpi_j + (3 * NWAVE_MAX * nspec)] = xxspec[mpi_j].lf_wind[mpi_i] / np_mpi_global;
    }
  }

  MPI_Reduce (redhelper, redhelper2, size_of_commbuffer, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  MPI_Bcast (redhelper2, size_of_commbuffer, MPI_DOUBLE, 0, MPI_COMM_WORLD);

  for (mpi_i = 0; mpi_i < NWAVE_MAX; mpi_i++)
  {
    for (mpi_j = 0; mpi_j < nspec; mpi_j++)
    {
      xxspec[mpi_j].f[mpi_i] = redhelper2[mpi_i * nspec + mpi_j];
      xxspec[mpi_j].lf[mpi_i] = redhelper2[mpi_i * nspec + mpi_j + (NWAVE_MAX * nspec)];
      xxspec[mpi_j].f_wind[mpi_i] = redhelper2[mpi_i * nspec + mpi_j + (2 * NWAVE_MAX * nspec)];
      xxspec[mpi_j].lf_wind[mpi_i] = redhelper2[mpi_i * nspec + mpi_j + (3 * NWAVE_MAX * nspec)];
    }
  }

  free (redhelper);
  free (redhelper2);
#endif

  return (0);
}
