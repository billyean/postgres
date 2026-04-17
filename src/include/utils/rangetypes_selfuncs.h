/*-------------------------------------------------------------------------
 *
 * rangetypes_selfuncs.h
 *	  Internal helper declarations shared between rangetypes_selfuncs.c
 *	  and multirangetypes_selfuncs.c.
 *
 *	  These functions are not part of any general-purpose planner API.
 *	  They exist solely to avoid code duplication between the range and
 *	  multirange selectivity estimators, which share the same underlying
 *	  histogram model.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/rangetypes_selfuncs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RANGETYPES_SELFUNCS_H
#define RANGETYPES_SELFUNCS_H

#include "utils/rangetypes.h"
#include "utils/typcache.h"

/* Histogram lookup: fraction of bounds less than (or equal to) a constant */
extern double calc_hist_selectivity_scalar(TypeCacheEntry *typcache,
										   const RangeBound *constbound,
										   const RangeBound *hist,
										   int hist_nvalues, bool equal);

/* Binary search on an array of range bounds */
extern int	rbound_bsearch(TypeCacheEntry *typcache,
						   const RangeBound *value,
						   const RangeBound *hist,
						   int hist_length, bool equal);

/* Relative position of a value within a histogram bin */
extern float8 get_position(TypeCacheEntry *typcache,
						   const RangeBound *value,
						   const RangeBound *hist1,
						   const RangeBound *hist2);

/* Relative position of a value within a length histogram bin */
extern double get_len_position(double value, double hist1, double hist2);

/* Distance between two range bounds */
extern float8 get_distance(TypeCacheEntry *typcache,
						   const RangeBound *bound1,
						   const RangeBound *bound2);

/* Binary search on a length histogram */
extern int	length_hist_bsearch(const Datum *length_hist_values,
								int length_hist_nvalues,
								double value, bool equal);

/* Average fraction from a length histogram over an interval */
extern double calc_length_hist_frac(const Datum *length_hist_values,
									int length_hist_nvalues,
									double length1, double length2,
									bool equal);

/* Selectivity of "var <@ const" using bounds and length histograms */
extern double calc_hist_selectivity_contained(TypeCacheEntry *typcache,
											  const RangeBound *lower,
											  RangeBound *upper,
											  const RangeBound *hist_lower,
											  int hist_nvalues,
											  const Datum *length_hist_values,
											  int length_hist_nvalues);

/* Selectivity of "var @> const" using bounds and length histograms */
extern double calc_hist_selectivity_contains(TypeCacheEntry *typcache,
											 const RangeBound *lower,
											 const RangeBound *upper,
											 const RangeBound *hist_lower,
											 int hist_nvalues,
											 const Datum *length_hist_values,
											 int length_hist_nvalues);

/* Estimate P(X < Y) from two bound histograms (join selectivity helper) */
extern double calc_hist_join_selectivity(TypeCacheEntry *typcache,
										 const RangeBound *hist1, int nhist1,
										 const RangeBound *hist2, int nhist2);

#endif							/* RANGETYPES_SELFUNCS_H */
