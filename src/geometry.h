/*
 * geometry.h
 *
 * Geometry of diffraction
 *
 * (c) 2006-2010 Thomas White <taw@physics.org>
 *
 * Part of CrystFEL - crystallography with a FEL
 *
 */

#ifndef GEOMETRY_H
#define GEOMETRY_H


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "reflist.h"

extern RefList *find_intersections(struct image *image, UnitCell *cell,
                                   int output);

extern double integrate_all(struct image *image, RefList *reflections);

extern void update_partialities_and_asymm(struct image *image, const char *sym,
                                          ReflItemList *obs,
                                          int *n_expected, int *n_found,
                                          int *n_notfound);
#endif	/* GEOMETRY_H */
