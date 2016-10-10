/*
 * predict-refine.c
 *
 * Prediction refinement
 *
 * Copyright © 2012-2016 Deutsches Elektronen-Synchrotron DESY,
 *                       a research centre of the Helmholtz Association.
 *
 * Authors:
 *   2010-2016 Thomas White <taw@physics.org>
 *   2016      Valerio Mariani
 *
 * This file is part of CrystFEL.
 *
 * CrystFEL is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CrystFEL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CrystFEL.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif


#include <stdlib.h>
#include <assert.h>
#include <gsl/gsl_matrix.h>
#include <gsl/gsl_vector.h>

#include "image.h"
#include "geometry.h"
#include "cell-utils.h"


/* Maximum number of iterations of NLSq to do for each image per macrocycle. */
#define MAX_CYCLES (10)

/* Weighting of excitation error term (m^-1) compared to position term (m) */
#define EXC_WEIGHT (4e-20)

/* Parameters to refine */
static const enum gparam rv[] =
{
	GPARAM_ASX,
	GPARAM_ASY,
	GPARAM_ASZ,
	GPARAM_BSX,
	GPARAM_BSY,
	GPARAM_BSZ,
	GPARAM_CSX,
	GPARAM_CSY,
	GPARAM_CSZ,
	GPARAM_DETX,
	GPARAM_DETY,
};

static const int num_params = 11;

struct reflpeak {
	Reflection *refl;
	struct imagefeature *peak;
	double Ih;   /* normalised */
	struct panel *panel;  /* panel the reflection appears on
                               * (we assume this never changes) */
};


static void twod_mapping(double fs, double ss, double *px, double *py,
                         struct panel *p)
{
	double xs, ys;

	xs = fs*p->fsx + ss*p->ssx;
	ys = fs*p->fsy + ss*p->ssy;

	*px = (xs + p->cnx) / p->res;
	*py = (ys + p->cny) / p->res;
}


static double r_dev(struct reflpeak *rp)
{
	/* Excitation error term */
	double rlow, rhigh, p;
	get_partial(rp->refl, &rlow, &rhigh, &p);
	return (rlow+rhigh)/2.0;
}


static double x_dev(struct reflpeak *rp, struct detector *det)
{
	/* Peak position term */
	double xpk, ypk, xh, yh;
	double fsh, ssh;
	twod_mapping(rp->peak->fs, rp->peak->ss, &xpk, &ypk, rp->panel);
	get_detector_pos(rp->refl, &fsh, &ssh);
	twod_mapping(fsh, ssh, &xh, &yh, rp->panel);
	return xh-xpk;
}


static double y_dev(struct reflpeak *rp, struct detector *det)
{
	/* Peak position term */
	double xpk, ypk, xh, yh;
	double fsh, ssh;
	twod_mapping(rp->peak->fs, rp->peak->ss, &xpk, &ypk, rp->panel);
	get_detector_pos(rp->refl, &fsh, &ssh);
	twod_mapping(fsh, ssh, &xh, &yh, rp->panel);
	return yh-ypk;
}


static void UNUSED write_pairs(const char *filename, struct reflpeak *rps,
                               int n, struct detector *det)
{
	int i;
	FILE *fh;

	fh = fopen(filename, "w");
	if ( fh == NULL ) {
		ERROR("Failed to open '%s'\n", filename);
		return;
	}

	for ( i=0; i<n; i++ ) {

		double write_fs, write_ss;
		double fs, ss;
		struct panel *p;

		fs = rps[i].peak->fs;
		ss = rps[i].peak->ss;
		p = rps[i].panel;

		write_fs = fs + p->orig_min_fs;
		write_ss = ss + p->orig_min_ss;

		fprintf(fh, "%7.2f %7.2f dev r,x,y: %9f %9f %9f %9f\n",
		        write_fs, write_ss,
		        r_dev(&rps[i])/1e9, fabs(r_dev(&rps[i])/1e9),
		        x_dev(&rps[i], det),
		        y_dev(&rps[i], det));

	}

	fclose(fh);

	STATUS("Wrote %i pairs to %s\n", n, filename);
}


static int cmpd2(const void *av, const void *bv)
{
	struct reflpeak *a, *b;

	a = (struct reflpeak *)av;
	b = (struct reflpeak *)bv;

	if ( fabs(r_dev(a)) < fabs(r_dev(b)) ) return -1;
	return 1;
}


static int check_outlier_transition(struct reflpeak *rps, int n,
                                    struct detector *det)
{
	int i;

	if ( n < 3 ) return n;

	qsort(rps, n, sizeof(struct reflpeak), cmpd2);
	//write_pairs("pairs-before-outlier.lst", rps, n, det);

	for ( i=1; i<n-1; i++ ) {

		int j;
		double grad = fabs(r_dev(&rps[i])) / i;

		for ( j=i+1; j<n; j++ ) {
			if ( fabs(r_dev(&rps[j])) < 0.001e9+grad*j ) {
				break;
			}
		}
		if ( j == n ) {
			//STATUS("Outlier transition found at position %i / %i\n",
			//       i, n);
			return i;
		}
	}

	//STATUS("No outlier transition found.\n");
	return n;
}


/* Associate a Reflection with each peak in "image" which is close to Bragg.
 * Reflections will be added to "reflist", which can be NULL if this is not
 * needed.  "rps" must be an array of sufficient size for all the peaks */
static int pair_peaks(struct image *image, Crystal *cr,
                      RefList *reflist, struct reflpeak *rps)
{
	int i;
	int n_acc = 0;
	int n_final;
	int n = 0;
	double ax, ay, az;
	double bx, by, bz;
	double cx, cy, cz;
	RefList *all_reflist;

	all_reflist = reflist_new();
	cell_get_cartesian(crystal_get_cell(cr),
	                   &ax, &ay, &az, &bx, &by, &bz, &cx, &cy, &cz);

	/* First, create a RefList containing the most likely indices for each
	 * peak, with no exclusion criteria */
	for ( i=0; i<image_feature_count(image->features); i++ ) {

		struct imagefeature *f;
		double h, k, l, hd, kd, ld;
		Reflection *refl;

		/* Assume all image "features" are genuine peaks */
		f = image_get_feature(image->features, i);
		if ( f == NULL ) continue;

		/* Decimal and fractional Miller indices of nearest reciprocal
		 * lattice point */
		hd = f->rx * ax + f->ry * ay + f->rz * az;
		kd = f->rx * bx + f->ry * by + f->rz * bz;
		ld = f->rx * cx + f->ry * cy + f->rz * cz;
		h = lrint(hd);
		k = lrint(kd);
		l = lrint(ld);

		refl = reflection_new(h, k, l);
		if ( refl == NULL ) {
			ERROR("Failed to create reflection\n");
			return 0;
		}

		add_refl_to_list(refl, all_reflist);
		set_symmetric_indices(refl, h, k, l);

		/* It doesn't matter if the actual predicted location
		 * doesn't fall on this panel.  We're only interested
		 * in how far away it is from the peak location.
		 * The predicted position and excitation errors will be
		 * filled in by update_partialities(). */
		set_panel(refl, f->p);

		rps[n].refl = refl;
		rps[n].peak = f;
		rps[n].panel = f->p;
		n++;

	}

	/* Get the excitation errors and detector positions for the candidate
	 * reflections */
	crystal_set_reflections(cr, all_reflist);
	update_partialities(cr, PMODEL_SCSPHERE);

	/* Pass over the peaks again, keeping only the ones which look like
	 * good pairings */
	for ( i=0; i<n; i++ ) {

		double fs, ss, pd;
		signed int h, k, l;
		Reflection *refl = rps[i].refl;

		get_indices(refl, &h, &k, &l);

		/* Is the supposed reflection anywhere near the peak? */
		get_detector_pos(refl, &fs, &ss);
		pd = pow(fs - rps[i].peak->fs, 2.0)
		   + pow(ss - rps[i].peak->ss, 2.0);
		if ( pd > 10.0 * 10.0 ) continue; /* FIXME Hardcoded distance */

		rps[n_acc] = rps[i];
		rps[n_acc].refl = reflection_new(h, k, l);
		copy_data(rps[n_acc].refl, refl);
		n_acc++;

	}
	reflist_free(all_reflist);

	/* Sort the pairings by excitation error and look for a transition
	 * between good pairings and outliers */
	n_final = check_outlier_transition(rps, n_acc, image->det);

	/* Add the final accepted reflections to the caller's list */
	if ( reflist != NULL ) {
		for ( i=0; i<n_final; i++ ) {
			add_refl_to_list(rps[i].refl, reflist);
		}
	}

	/* Free the reflections beyond the outlier cutoff */
	for ( i=n_final; i<n_acc; i++ ) {
		reflection_free(rps[i].refl);
	}

	return n_final;
}


void refine_radius(Crystal *cr, struct image *image)
{
	int n, n_acc;
	struct reflpeak *rps;
	RefList *reflist;

	/* Maximum possible size */
	rps = malloc(image_feature_count(image->features)
	                  * sizeof(struct reflpeak));
	if ( rps == NULL ) return;

	reflist = reflist_new();
	n_acc = pair_peaks(image, cr, reflist, rps);
	if ( n_acc < 3 ) {
		free(rps);
		return;
	}
	crystal_set_reflections(cr, reflist);
	update_partialities(cr, PMODEL_SCSPHERE);
	crystal_set_reflections(cr, NULL);

	qsort(rps, n_acc, sizeof(struct reflpeak), cmpd2);
	n = (n_acc-1) - n_acc/50;
	if ( n < 2 ) n = 2; /* n_acc is always >= 2 */
	crystal_set_profile_radius(cr, fabs(r_dev(&rps[n])));

	reflist_free(reflist);
	free(rps);
}


static void update_detector(struct detector *det, double xoffs, double yoffs,
                            double coffs)
{
	int i;

	for ( i=0; i<det->n_panels; i++ ) {
		struct panel *p = &det->panels[i];
		p->cnx += xoffs * p->res;
		p->cny += yoffs * p->res;
		p->clen += coffs;
	}
}


static int iterate(struct reflpeak *rps, int n, UnitCell *cell,
                   struct image *image,
                   double *total_x, double *total_y, double *total_z)
{
	int i;
	gsl_matrix *M;
	gsl_vector *v;
	gsl_vector *shifts;
	double asx, asy, asz;
	double bsx, bsy, bsz;
	double csx, csy, csz;

	/* Number of parameters to refine */
	M = gsl_matrix_calloc(num_params, num_params);
	v = gsl_vector_calloc(num_params);

	for ( i=0; i<n; i++ ) {

		int k;
		double gradients[num_params];
		double w;

		/* Excitation error terms */
		w = EXC_WEIGHT * rps[i].Ih;

		for ( k=0; k<num_params; k++ ) {
			gradients[k] = r_gradient(cell, rv[k], rps[i].refl,
			                          image);
		}

		for ( k=0; k<num_params; k++ ) {

			int g;
			double v_c, v_curr;

			for ( g=0; g<num_params; g++ ) {

				double M_c, M_curr;

				/* Matrix is symmetric */
				if ( g > k ) continue;

				M_c = w * gradients[g] * gradients[k];
				M_curr = gsl_matrix_get(M, k, g);
				gsl_matrix_set(M, k, g, M_curr + M_c);
				gsl_matrix_set(M, g, k, M_curr + M_c);

			}

			v_c = w * r_dev(&rps[i]);
			v_c *= -gradients[k];
			v_curr = gsl_vector_get(v, k);
			gsl_vector_set(v, k, v_curr + v_c);

		}

		/* Positional x terms */
		for ( k=0; k<num_params; k++ ) {
			gradients[k] = x_gradient(rv[k], rps[i].refl, cell,
			                          rps[i].panel, image->lambda);
		}

		for ( k=0; k<num_params; k++ ) {

			int g;
			double v_c, v_curr;

			for ( g=0; g<num_params; g++ ) {

				double M_c, M_curr;

				/* Matrix is symmetric */
				if ( g > k ) continue;

				M_c = gradients[g] * gradients[k];
				M_curr = gsl_matrix_get(M, k, g);
				gsl_matrix_set(M, k, g, M_curr + M_c);
				gsl_matrix_set(M, g, k, M_curr + M_c);

			}

			v_c = x_dev(&rps[i], image->det);
			v_c *= -gradients[k];
			v_curr = gsl_vector_get(v, k);
			gsl_vector_set(v, k, v_curr + v_c);

		}

		/* Positional y terms */
		for ( k=0; k<num_params; k++ ) {
			gradients[k] = y_gradient(rv[k], rps[i].refl, cell,
			                          rps[i].panel, image->lambda);
		}

		for ( k=0; k<num_params; k++ ) {

			int g;
			double v_c, v_curr;

			for ( g=0; g<num_params; g++ ) {

				double M_c, M_curr;

				/* Matrix is symmetric */
				if ( g > k ) continue;

				M_c = gradients[g] * gradients[k];
				M_curr = gsl_matrix_get(M, k, g);
				gsl_matrix_set(M, k, g, M_curr + M_c);
				gsl_matrix_set(M, g, k, M_curr + M_c);

			}

			v_c = y_dev(&rps[i], image->det);
			v_c *= -gradients[k];
			v_curr = gsl_vector_get(v, k);
			gsl_vector_set(v, k, v_curr + v_c);

		}

	}

	//show_matrix_eqn(M, v);
	shifts = solve_svd(v, M, NULL, 0);
	if ( shifts == NULL ) {
		ERROR("Failed to solve equations.\n");
		gsl_matrix_free(M);
		gsl_vector_free(v);
		return 1;
	}

	for ( i=0; i<num_params; i++ ) {
	//	STATUS("Shift %i = %e\n", i, gsl_vector_get(shifts, i));
		if ( isnan(gsl_vector_get(shifts, i)) ) {
			gsl_vector_set(shifts, i, 0.0);
		}
	}

	/* Apply shifts */
	cell_get_reciprocal(cell, &asx, &asy, &asz,
	                          &bsx, &bsy, &bsz,
	                          &csx, &csy, &csz);

	/* Ensure the order here matches the order in rv[] */
	asx += gsl_vector_get(shifts, 0);
	asy += gsl_vector_get(shifts, 1);
	asz += gsl_vector_get(shifts, 2);
	bsx += gsl_vector_get(shifts, 3);
	bsy += gsl_vector_get(shifts, 4);
	bsz += gsl_vector_get(shifts, 5);
	csx += gsl_vector_get(shifts, 6);
	csy += gsl_vector_get(shifts, 7);
	csz += gsl_vector_get(shifts, 8);
	update_detector(image->det, gsl_vector_get(shifts, 9),
	                            gsl_vector_get(shifts, 10),
	                            gsl_vector_get(shifts, 11));
	*total_x += gsl_vector_get(shifts, 9);
	*total_y += gsl_vector_get(shifts, 10);
	*total_z += gsl_vector_get(shifts, 11);

	cell_set_reciprocal(cell, asx, asy, asz, bsx, bsy, bsz, csx, csy, csz);

	gsl_vector_free(shifts);
	gsl_matrix_free(M);
	gsl_vector_free(v);

	return 0;
}


static double UNUSED residual(struct reflpeak *rps, int n, struct detector *det)
{
	int i;
	double res = 0.0;
	double r;

	r = 0.0;
	for ( i=0; i<n; i++ ) {
		r += EXC_WEIGHT * rps[i].Ih * pow(r_dev(&rps[i]), 2.0);
	}
	printf("%e ", r);
	res += r;

	r = 0.0;
	for ( i=0; i<n; i++ ) {
		r += pow(x_dev(&rps[i], det), 2.0);
	}
	printf("%e ", r);
	res += r;

	r = 0.0;
	for ( i=0; i<n; i++ ) {
		r += pow(y_dev(&rps[i], det), 2.0);
	}
	printf("%e\n", r);
	res += r;

	return res;
}


/* NB Only for use when the list of reflpeaks was created without a RefList.
 * If a RefList was used, then reflist_free the list then just free() the rps */
static void free_rps_noreflist(struct reflpeak *rps, int n)
{
	int i;

	for ( i=0; i<n; i++ ) {
		reflection_free(rps[i].refl);
	}
	free(rps);
}


int refine_prediction(struct image *image, Crystal *cr)
{
	int n;
	int i;
	struct reflpeak *rps;
	double max_I;
	RefList *reflist;
	double total_x = 0.0;
	double total_y = 0.0;
	double total_z = 0.0;

	rps = malloc(image_feature_count(image->features)
	                       * sizeof(struct reflpeak));
	if ( rps == NULL ) return 1;

	reflist = reflist_new();
	n = pair_peaks(image, cr, reflist, rps);
	if ( n < 10 ) {
		free(rps);
		reflist_free(reflist);
		return 1;
	}
	crystal_set_reflections(cr, reflist);

	/* Normalise the intensities to max 1 */
	max_I = -INFINITY;
	for ( i=0; i<n; i++ ) {
		double cur_I = rps[i].peak->intensity;
		if ( cur_I > max_I ) max_I = cur_I;
	}
	if ( max_I <= 0.0 ) {
		ERROR("All peaks negative?\n");
		free(rps);
		return 1;
	}
	for ( i=0; i<n; i++ ) {
		rps[i].Ih = rps[i].peak->intensity / max_I;
	}

	//STATUS("Initial residual = %e\n", residual(rps, n, image->det));

	/* Refine */
	for ( i=0; i<MAX_CYCLES; i++ ) {
		update_partialities(cr, PMODEL_SCSPHERE);
		if ( iterate(rps, n, crystal_get_cell(cr), image,
		             &total_x, &total_y, &total_z) ) return 1;
		//STATUS("Residual after %i = %e\n", i,
		//       residual(rps, n, image->det));
	}
	//STATUS("Final residual = %e\n", residual(rps, n, image->det));

	crystal_set_det_shift(cr, total_x, total_y);

	crystal_set_reflections(cr, NULL);
	reflist_free(reflist);

	n = pair_peaks(image, cr, NULL, rps);
	free_rps_noreflist(rps, n);
	if ( n < 10 ) {
		return 1;
	}

	return 0;
}
