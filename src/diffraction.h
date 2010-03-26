/*
 * diffraction.h
 *
 * Calculate diffraction patterns by Fourier methods
 *
 * (c) 2006-2010 Thomas White <taw@physics.org>
 *
 * Part of CrystFEL - crystallography with a FEL
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef DIFFRACTION_H
#define DIFFRACTION_H

#include "image.h"
#include "cell.h"

extern void get_diffraction(struct image *image, int na, int nb, int nc,
                            double *intensities, int do_water);
extern struct rvec get_q(struct image *image, unsigned int xs, unsigned int ys,
                         unsigned int sampling, float *ttp, float k);

#endif	/* DIFFRACTION_H */
