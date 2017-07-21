/*
 * taketwo.c
 *
 * Rewrite of TakeTwo algorithm (Acta D72 (8) 956-965) for CrystFEL
 *
 * Copyright © 2016 Helen Ginn
 * Copyright © 2016 Deutsches Elektronen-Synchrotron DESY,
 *                  a research centre of the Helmholtz Association.
 *
 * Authors:
 *   2016 Helen Ginn <helen@strubi.ox.ac.uk>
 *   2016 Thomas White <taw@physics.org>
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

#include <gsl/gsl_matrix.h>
#include <gsl/gsl_blas.h>
#include <float.h>
#include <math.h>
#include <assert.h>
#include <time.h>

#include "cell-utils.h"
#include "index.h"
#include "taketwo.h"
#include "peaks.h"
#include "symmetry.h"

/**
 * spotvec
 * @obsvec: an observed vector between two spots
 * @matches: array of matching theoretical vectors from unit cell
 * @match_num: number of matches
 * @distance: length of obsvec (do I need this?)
 * @her_rlp: pointer to first rlp position for difference vec
 * @his_rlp: pointer to second rlp position for difference vec
 *
 * Structure representing 3D vector between two potential Bragg peaks
 * in reciprocal space, and an array of potential matching theoretical
 * vectors from unit cell/centering considerations.
 **/
struct SpotVec
{
	struct rvec obsvec;
	struct rvec *matches;
	int match_num;
	struct rvec *asym_matches;
	int asym_match_num;
	double distance;
	struct rvec *her_rlp;
	struct rvec *his_rlp;
};


struct taketwo_private
{
	IndexingMethod indm;
	float          *ltl;
	UnitCell       *cell;
};

// These rotation symmetry operators
struct TakeTwoCell
{
	UnitCell       *cell;
	gsl_matrix     **rotSymOps;
	unsigned int   numOps;
	struct SpotVec **obs_vecs; // Pointer to an array
	int obs_vec_count;
	struct taketwo_options *options;
	gsl_matrix *twiz1Tmp;
	gsl_matrix *twiz2Tmp;
	gsl_vector *vec1Tmp;
	gsl_vector *vec2Tmp;
};


/* Maximum distance between two rlp sizes to consider info for indexing */
#define MAX_RECIP_DISTANCE (0.15*1e10)

/* Tolerance for two lengths in reciprocal space to be considered the same */
#define RECIP_TOLERANCE (0.0010*1e10)

/* Threshold for network members to consider a potential solution */
#define NETWORK_MEMBER_THRESHOLD (20)

/* Maximum network members (obviously a solution so should stop) */
#define MAX_NETWORK_MEMBERS (NETWORK_MEMBER_THRESHOLD + 3)

/* Maximum dead ends for a single branch extension during indexing */
#define MAX_DEAD_ENDS (10)

/* Maximum observed vectors before TakeTwo gives up and deals with
 * what is already there. */
#define MAX_OBS_VECTORS 100000

/* Tolerance for two angles to be considered the same */
#define ANGLE_TOLERANCE (deg2rad(0.6))

/* Tolerance for rot_mats_are_similar */
#define TRACE_TOLERANCE (deg2rad(3.0))

/* TODO: Multiple lattices */


/* ------------------------------------------------------------------------
 * apologetic function
 * ------------------------------------------------------------------------*/

void apologise()
{
	printf("Error - could not allocate memory for indexing.\n");
}

/* ------------------------------------------------------------------------
 * functions concerning aspects of rvec which are very likely to be
 * incorporated somewhere else in CrystFEL and therefore may need to be
 * deleted and references connected to a pre-existing function. (Lowest level)
 * ------------------------------------------------------------------------*/

static struct rvec new_rvec(double new_u, double new_v, double new_w)
{
	struct rvec new_rvector;
	new_rvector.u = new_u;
	new_rvector.v = new_v;
	new_rvector.w = new_w;

	return new_rvector;
}


static struct rvec diff_vec(struct rvec from, struct rvec to)
{
	struct rvec diff = new_rvec(to.u - from.u,
				    to.v - from.v,
				    to.w - from.w);

	return diff;
}

static double sq_length(struct rvec vec)
{
	double sqlength = (vec.u * vec.u + vec.v * vec.v + vec.w * vec.w);

	return sqlength;
}


static double rvec_length(struct rvec vec)
{
	return sqrt(sq_length(vec));
}


static void normalise_rvec(struct rvec *vec)
{
	double length = rvec_length(*vec);
	vec->u /= length;
	vec->v /= length;
	vec->w /= length;
}


static double rvec_cosine(struct rvec v1, struct rvec v2)
{
	double dot_prod = v1.u * v2.u + v1.v * v2.v + v1.w * v2.w;
	double v1_length = rvec_length(v1);
	double v2_length = rvec_length(v2);

	double cos_theta = dot_prod / (v1_length * v2_length);

	return cos_theta;
}


static double rvec_angle(struct rvec v1, struct rvec v2)
{
	double cos_theta = rvec_cosine(v1, v2);
	double angle = acos(cos_theta);

	return angle;
}


static struct rvec rvec_cross(struct rvec a, struct rvec b)
{
	struct rvec c;

	c.u = a.v*b.w - a.w*b.v;
	c.v = -(a.u*b.w - a.w*b.u);
	c.w = a.u*b.v - a.v*b.u;

	return c;
}

/*
static void show_rvec(struct rvec r2)
{
	struct rvec r = r2;
	normalise_rvec(&r);
	STATUS("[ %.3f %.3f %.3f ]\n", r.u, r.v, r.w);
}
*/


/* ------------------------------------------------------------------------
 * functions called under the core functions, still specialised (Level 3)
 * ------------------------------------------------------------------------*/

static void rotation_around_axis(struct rvec c, double th,
				 gsl_matrix *res)
{
	double omc = 1.0 - cos(th);
	double s = sin(th);
	gsl_matrix_set(res, 0, 0, cos(th) + c.u*c.u*omc);
	gsl_matrix_set(res, 0, 1, c.u*c.v*omc - c.w*s);
	gsl_matrix_set(res, 0, 2, c.u*c.w*omc + c.v*s);
	gsl_matrix_set(res, 1, 0, c.u*c.v*omc + c.w*s);
	gsl_matrix_set(res, 1, 1, cos(th) + c.v*c.v*omc);
	gsl_matrix_set(res, 1, 2, c.v*c.w*omc - c.u*s);
	gsl_matrix_set(res, 2, 0, c.w*c.u*omc - c.v*s);
	gsl_matrix_set(res, 2, 1, c.w*c.v*omc + c.u*s);
	gsl_matrix_set(res, 2, 2, cos(th) + c.w*c.w*omc);
}


/* Rotate vector (vec1) around axis (axis) by angle theta. Find value of
 * theta for which the angle between (vec1) and (vec2) is minimised. */
static void closest_rot_mat(struct rvec vec1, struct rvec vec2,
			    struct rvec axis, gsl_matrix *twizzle)
{
	/* Let's have unit vectors */
	normalise_rvec(&vec1);
	normalise_rvec(&vec2);
	normalise_rvec(&axis);

	/* Redeclaring these to try and maintain readability and
	 * check-ability against the maths I wrote down */
	double a = vec2.u; double b = vec2.v; double c = vec2.w;
	double p = vec1.u; double q = vec1.v; double r = vec1.w;
	double x = axis.u; double y = axis.v; double z = axis.w;

	/* Components in handwritten maths online when I upload it */
	double A = a*(p*x*x - p + x*y*q + x*z*r) +
	b*(p*x*y + q*y*y - q + r*y*z) +
	c*(p*x*z + q*y*z + r*z*z - r);

	double B = a*(y*r - z*q) + b*(p*z - r*x) + c*(q*x - p*y);

	double tan_theta = - B / A;
	double theta = atan(tan_theta);

	/* Now we have two possible solutions, theta or theta+pi
	 * and we need to work out which one. This could potentially be
	 * simplified - do we really need so many cos/sins? maybe check
	 * the 2nd derivative instead? */
	double cc = cos(theta);
	double C = 1 - cc;
	double s = sin(theta);
	double occ = -cc;
	double oC = 1 - occ;
	double os = -s;

	double pPrime = (x*x*C+cc)*p + (x*y*C-z*s)*q + (x*z*C+y*s)*r;
	double qPrime = (y*x*C+z*s)*p + (y*y*C+cc)*q + (y*z*C-x*s)*r;
	double rPrime = (z*x*C-y*s)*p + (z*y*C+x*s)*q + (z*z*C+cc)*r;

	double pDbPrime = (x*x*oC+occ)*p + (x*y*oC-z*os)*q + (x*z*oC+y*os)*r;
	double qDbPrime = (y*x*oC+z*os)*p + (y*y*oC+occ)*q + (y*z*oC-x*os)*r;
	double rDbPrime = (z*x*oC-y*os)*p + (z*y*oC+x*os)*q + (z*z*oC+occ)*r;

	double cosAlpha = pPrime * a + qPrime * b + rPrime * c;
	double cosAlphaOther = pDbPrime * a + qDbPrime * b + rDbPrime * c;

	int addPi = (cosAlphaOther > cosAlpha);
	double bestAngle = theta + addPi * M_PI;

	/* Don't return an identity matrix which has been rotated by
	 * theta around "axis", but do assign it to twizzle. */
	rotation_around_axis(axis, bestAngle, twizzle);
}


static double matrix_trace(gsl_matrix *a)
{
	int i;
	double tr = 0.0;

	assert(a->size1 == a->size2);
	for ( i=0; i<a->size1; i++ ) {
		tr += gsl_matrix_get(a, i, i);
	}
	return tr;
}

static char *add_ua(const char *inp, char ua)
{
	char *pg = malloc(64);
	if ( pg == NULL ) return NULL;
	snprintf(pg, 63, "%s_ua%c", inp, ua);
	return pg;
}


static char *get_chiral_holohedry(UnitCell *cell)
{
	LatticeType lattice = cell_get_lattice_type(cell);
	char *pg = malloc(64);
	char *pgout = 0;

	if ( pg == NULL ) return NULL;

	switch (lattice)
	{
		case L_TRICLINIC:
		pg = "1";
		break;

		case L_MONOCLINIC:
		pg = "2";
		break;

		case L_ORTHORHOMBIC:
		pg = "222";
		break;

		case L_TETRAGONAL:
		pg = "422";
		break;

		case L_RHOMBOHEDRAL:
		pg = "3_R";
		break;

		case L_HEXAGONAL:
		if ( cell_get_centering(cell) == 'H' ) {
			pg = "3_H";
		} else {
			pg = "622";
		}
		break;

		case L_CUBIC:
		pg = "432";
		break;

		default:
		pg = "error";
		break;
	}

	switch (lattice)
	{
		case L_TRICLINIC:
		case L_ORTHORHOMBIC:
		case L_RHOMBOHEDRAL:
		case L_CUBIC:
		pgout = strdup(pg);
		break;

		case L_MONOCLINIC:
		case L_TETRAGONAL:
		case L_HEXAGONAL:
		pgout = add_ua(pg, cell_get_unique_axis(cell));
		break;

		default:
		break;
	}
	
	return pgout;
}


static SymOpList *sym_ops_for_cell(UnitCell *cell)
{
	SymOpList *rawList;

	char *pg = get_chiral_holohedry(cell);
	rawList = get_pointgroup(pg);
	free(pg);
	
	return rawList;
}

static int rot_mats_are_similar(gsl_matrix *rot1, gsl_matrix *rot2,
  				gsl_matrix *sub, gsl_matrix *mul,
				double *score, struct TakeTwoCell *cell)
{
	double tr;

	gsl_matrix_memcpy(sub, rot1);
	gsl_matrix_sub(sub, rot2);  /* sub = rot1 - rot2 */

	gsl_blas_dgemm(CblasNoTrans, CblasTrans, 1.0, sub, sub, 0.0, mul);

	tr = matrix_trace(mul);
	if (score != NULL) *score = tr;

	return (tr < cell->options->trace_tol);
}

static int symm_rot_mats_are_similar(gsl_matrix *rot1, gsl_matrix *rot2,
                                     struct TakeTwoCell *cell)
{
	int i;
	
	gsl_matrix *sub = gsl_matrix_calloc(3, 3);
	gsl_matrix *mul = gsl_matrix_calloc(3, 3);

	for (i = 0; i < cell->numOps; i++) {
		gsl_matrix *testRot = gsl_matrix_alloc(3, 3);
		gsl_matrix *symOp = cell->rotSymOps[i];
		
		gsl_blas_dgemm(CblasNoTrans, CblasNoTrans, 1.0, rot1, symOp,
		0.0, testRot);
		
		if (rot_mats_are_similar(testRot, rot2, sub, mul, NULL, cell)) {
			gsl_matrix_free(testRot);
			gsl_matrix_free(sub);
			gsl_matrix_free(mul);
			return 1;
		}
	
		gsl_matrix_free(testRot);	
	}
	
	gsl_matrix_free(sub);
	gsl_matrix_free(mul);
	
	return 0;
}

static void rotation_between_vectors(struct rvec a, struct rvec b,
				     gsl_matrix *twizzle)
{
	double th = rvec_angle(a, b);
	struct rvec c = rvec_cross(a, b);
	normalise_rvec(&c);
	rotation_around_axis(c, th, twizzle);
}


static void rvec_to_gsl(gsl_vector *newVec, struct rvec v)
{
	gsl_vector_set(newVec, 0, v.u);
	gsl_vector_set(newVec, 1, v.v);
	gsl_vector_set(newVec, 2, v.w);
}


struct rvec gsl_to_rvec(gsl_vector *a)
{
	struct rvec v;
	v.u = gsl_vector_get(a, 0);
	v.v = gsl_vector_get(a, 1);
	v.w = gsl_vector_get(a, 2);
	return v;
}


/* Code me in gsl_matrix language to copy the contents of the function
 * in cppxfel (IndexingSolution::createSolution). This function is quite
 * intensive on the number crunching side so simple angle checks are used
 * to 'pre-scan' vectors beforehand. */
static gsl_matrix *generate_rot_mat(struct rvec obs1, struct rvec obs2,
				    struct rvec cell1, struct rvec cell2,
				    struct TakeTwoCell *cell)
{
	gsl_matrix *fullMat;
	rvec_to_gsl(cell->vec1Tmp, cell2);

//	gsl_vector *cell2v = rvec_to_gsl(cell2);
//	gsl_vector *cell2vr = gsl_vector_calloc(3);

	normalise_rvec(&obs1);
	normalise_rvec(&obs2);
	normalise_rvec(&cell1);
	normalise_rvec(&cell2);

	/* Rotate reciprocal space so that the first simulated vector lines up
	 * with the observed vector. */
	rotation_between_vectors(cell1, obs1, cell->twiz1Tmp);

	normalise_rvec(&obs1);

	/* Multiply cell2 by rotateSpotDiffMatrix --> cell2vr */
	gsl_blas_dgemv(CblasNoTrans, 1.0, cell->twiz1Tmp, cell->vec1Tmp,
		       0.0, cell->vec2Tmp);

	/* Now we twirl around the firstAxisUnit until the rotated simulated
	 * vector matches the second observed vector as closely as possible. */

	closest_rot_mat(gsl_to_rvec(cell->vec2Tmp), obs2, obs1, cell->twiz2Tmp);

	/* We want to apply the first matrix and then the second matrix,
	 * so we multiply these. */
	fullMat = gsl_matrix_calloc(3, 3);
	gsl_blas_dgemm(CblasTrans, CblasTrans, 1.0,
		       cell->twiz1Tmp, cell->twiz2Tmp, 0.0, fullMat);
	gsl_matrix_transpose(fullMat);

	return fullMat;
}


static int obs_vecs_share_spot(struct SpotVec *her_obs, struct SpotVec *his_obs)
{
	if ( (her_obs->her_rlp == his_obs->her_rlp) ||
	    (her_obs->her_rlp == his_obs->his_rlp) ||
	    (her_obs->his_rlp == his_obs->her_rlp) ||
	    (her_obs->his_rlp == his_obs->his_rlp) ) {
		return 1;
	}

	return 0;
}


static int obs_shares_spot_w_array(struct SpotVec *obs_vecs, int test_idx,
				   int *members, int num)
{
	int i;
	
	struct SpotVec *her_obs = &obs_vecs[test_idx];

	for ( i=0; i<num; i++ ) {
		struct SpotVec *his_obs = &obs_vecs[members[i]];

		int shares = obs_vecs_share_spot(her_obs, his_obs);

		if ( shares ) return 1;
	}

	return 0;
}


static int obs_vecs_match_angles(struct SpotVec *her_obs,
                                 struct SpotVec *his_obs,
                                 int **her_match_idxs, int **his_match_idxs,
                                 int *match_count, struct TakeTwoCell *cell)
{
        int i, j;
        *match_count = 0;

        double min_angle = deg2rad(2.5);
        double max_angle = deg2rad(187.5);

        /* calculate angle between observed vectors */
        double obs_angle = rvec_angle(her_obs->obsvec, his_obs->obsvec);

        /* calculate angle between all potential theoretical vectors */

        for ( i=0; i<her_obs->match_num; i++ ) {
        for ( j=0; j<his_obs->match_num; j++ ) {

                struct rvec *her_match = &her_obs->matches[i];
                struct rvec *his_match = &his_obs->matches[j];

                double theory_angle = rvec_angle(*her_match,
                                                 *his_match);

                /* is this angle a match? */

                double angle_diff = fabs(theory_angle - obs_angle);

                if ( angle_diff < cell->options->angle_tol ) {
                        // in the case of a brief check only            
                        if (!her_match_idxs || !his_match_idxs) {
                                return 1;
                        }
                
                        /* If the angles are too close to 0
                           or 180, one axis ill-determined */
                        if (theory_angle < min_angle ||
                            theory_angle > max_angle) {
                                continue;
                        }

                        // check the third vector
                        
                        struct rvec theory_diff = diff_vec(*his_match, *her_match);
                        struct rvec obs_diff = diff_vec(his_obs->obsvec,
                                                  her_obs->obsvec);
                        
                        theory_angle = rvec_angle(*her_match,
                                                 theory_diff);
                        obs_angle = rvec_angle(her_obs->obsvec, obs_diff);

                        if (fabs(obs_angle - theory_angle) > ANGLE_TOLERANCE) {
                                continue;
                        }
                        
                        theory_angle = rvec_angle(*his_match,
                                                 theory_diff);
                        obs_angle = rvec_angle(his_obs->obsvec, obs_diff);
                
                        if (fabs(obs_angle - theory_angle) > ANGLE_TOLERANCE) {
                                continue;
                        }
                        
                        size_t new_size = (*match_count + 1) *
                        sizeof(int);
                        if (her_match_idxs && his_match_idxs)
                        {
                                /* Reallocate the array to fit in another match */
                                int *temp_hers;
                                int *temp_his;
                                temp_hers = realloc(*her_match_idxs, new_size);
                                temp_his = realloc(*his_match_idxs, new_size);

                                if ( temp_hers == NULL || temp_his == NULL ) {
                                        apologise();
                                }

                                (*her_match_idxs) = temp_hers;
                                (*his_match_idxs) = temp_his;

                                (*her_match_idxs)[*match_count] = i;
                                (*his_match_idxs)[*match_count] = j;
                        }

                        (*match_count)++;
                }
        }
        }

        return (*match_count > 0);
}

/* ------------------------------------------------------------------------
 * core functions regarding the meat of the TakeTwo algorithm (Level 2)
 * ------------------------------------------------------------------------*/

static signed int finish_solution(gsl_matrix *rot, struct SpotVec *obs_vecs,
                                    int *obs_members, int *match_members,
                                    int member_num, struct TakeTwoCell *cell)
{
	gsl_matrix *sub = gsl_matrix_calloc(3, 3);
	gsl_matrix *mul = gsl_matrix_calloc(3, 3);
	
	gsl_matrix **rotations = malloc(sizeof(*rotations)* pow(member_num, 2)
	                                - member_num);

	int i, j, count;

	count = 0;
	for ( i=0; i<1; i++ ) {
		for ( j=0; j<member_num; j++ ) {
			if (i == j) continue;
			struct SpotVec i_vec = obs_vecs[obs_members[i]];
			struct SpotVec j_vec = obs_vecs[obs_members[j]];

			struct rvec i_obsvec = i_vec.obsvec;
			struct rvec j_obsvec = j_vec.obsvec;
			struct rvec i_cellvec = i_vec.matches[match_members[i]];
			struct rvec j_cellvec = j_vec.matches[match_members[j]];

			rotations[count] = generate_rot_mat(i_obsvec, j_obsvec,
							    i_cellvec, j_cellvec,
							    cell);

			count++;
		}
	}

	double min_score = FLT_MAX;
	int min_rot_index = 0;

	for (i=0; i<count; i++) {
		double current_score = 0;
		for (j=0; j<count; j++) {
			double addition;
			rot_mats_are_similar(rotations[i], rotations[j],
					     sub, mul,
					     &addition, cell);

			current_score += addition;
		}

		if (current_score < min_score) {
			min_score = current_score;
			min_rot_index = i;
		}
	}

	gsl_matrix_memcpy(rot, rotations[min_rot_index]);

	for (i=0; i<count; i++) {
		gsl_matrix_free(rotations[i]);
	}

	free(rotations);
	gsl_matrix_free(sub);
	gsl_matrix_free(mul);

	return 1;
}

static int weed_duplicate_matches(struct SpotVec *her_obs,
                                   struct SpotVec *his_obs,
                                   int **her_match_idxs, int **his_match_idxs,
                                   int *match_count, struct TakeTwoCell *cell)
{
	int num_occupied = 0;
	gsl_matrix **old_mats = calloc(*match_count, sizeof(gsl_matrix *));
	
	if (old_mats == NULL)
	{
		apologise();
		return 0;
	}

	signed int i, j;
	int duplicates = 0;
	
	for (i = *match_count - 1; i >= 0; i--) {
		int her_match = (*her_match_idxs)[i];
		int his_match = (*his_match_idxs)[i];
	
		struct rvec i_obsvec = her_obs->obsvec;
		struct rvec j_obsvec = his_obs->obsvec;
		struct rvec i_cellvec = her_obs->matches[her_match];
		struct rvec j_cellvec = his_obs->matches[his_match];

		gsl_matrix *mat = generate_rot_mat(i_obsvec, j_obsvec,
		                                   i_cellvec, j_cellvec, cell);

		int found = 0;
		
		for (j = 0; j < num_occupied; j++) {
			if (old_mats[j] && mat &&
			    symm_rot_mats_are_similar(old_mats[j], mat, cell))
			{
				// we have found a duplicate, so flag as bad.
				(*her_match_idxs)[i] = -1;
				(*his_match_idxs)[i] = -1;
				found = 1;
	
				duplicates++;

				gsl_matrix_free(mat);
				break;
			}
		}
		
		if (!found) {
			// we have not found a duplicate, add to list.
			old_mats[num_occupied] = mat;
			num_occupied++;
		}
	}
	
	for (i = 0; i < num_occupied; i++) {
		if (old_mats[i]) {
			gsl_matrix_free(old_mats[i]);
		}
	}

	free(old_mats);
	
	return 1;
}

static signed int find_next_index(gsl_matrix *rot, int *obs_members,
				  int *match_members, int start, int member_num,
				  int *match_found, struct TakeTwoCell *cell)
{
	struct SpotVec *obs_vecs = *(cell->obs_vecs);
	int obs_vec_count = cell->obs_vec_count;
	gsl_matrix *sub = gsl_matrix_calloc(3, 3);
	gsl_matrix *mul = gsl_matrix_calloc(3, 3);
	
	int i, j, k;
	
	for ( i=start; i<obs_vec_count; i++ ) {

		/* first we check for a shared spot - harshest condition */
		int shared = obs_shares_spot_w_array(obs_vecs, i, obs_members,
						     member_num);

		if ( !shared ) continue;
		
		int skip = 0;
		for ( j=0; j<member_num && skip == 0; j++ ) {
			if (i == obs_members[j]) {
				skip = 1;
			}
		}
		
		if (skip) {
			continue;
		}

		int all_ok = 1;
		int matched = -1;
	
		for ( j=0; j<member_num && all_ok; j++ ) {
		
			struct SpotVec *me = &obs_vecs[i];
			struct SpotVec *you = &obs_vecs[obs_members[j]];
			struct rvec you_cell = you->matches[match_members[j]];

			struct rvec me_obs = me->obsvec;
			struct rvec you_obs = you->obsvec;

			int one_is_okay = 0;

			for ( k=0; k<me->match_num; k++ ) {
				
				gsl_matrix *test_rot;
		
				struct rvec me_cell = me->matches[k];								

				test_rot = generate_rot_mat(me_obs,
					    you_obs, me_cell, you_cell,
					    cell);			

				double trace = 0;
				int ok = rot_mats_are_similar(rot, test_rot,
						       sub, mul, &trace, cell);
				
				gsl_matrix_free(test_rot);

				if (ok) {
					one_is_okay = 1;
					
					if (matched >= 0 && k == matched) {
						*match_found = k;
					} else if (matched < 0) {
						matched = k;
					} else {
						one_is_okay = 0;
						break;
					}					
				}
			}
			
			if (!one_is_okay) {
				all_ok = 0;
				break;
			}
		}


		if (all_ok) {
		
			for ( j=0; j<member_num; j++ ) {
			//	STATUS("%i ", obs_members[j]);
			}
			//STATUS("%i\n", i);
			
			return i;
		}
	}

	/* give up. */

	return -1;
}


static int grow_network(gsl_matrix *rot, int obs_idx1, int obs_idx2,
			int match_idx1, int match_idx2, int *max_members,
			struct TakeTwoCell *cell)
{
	struct SpotVec *obs_vecs = *(cell->obs_vecs);
	int obs_vec_count = cell->obs_vec_count;

	/* indices of members of the self-consistent network of vectors */
	int obs_members[MAX_NETWORK_MEMBERS];
	int match_members[MAX_NETWORK_MEMBERS];

	/* initialise the ones we know already */
	obs_members[0] = obs_idx1;
	obs_members[1] = obs_idx2;
	match_members[0] = match_idx1;
	match_members[1] = match_idx2;
	int member_num = 2;
	*max_members = 2;

	/* counter for dead ends which must not exceed MAX_DEAD_ENDS
	 * before it is reset in an additional branch */
	int dead_ends = 0;

	/* we start from 0 */
	int start = 0;

	while ( 1 ) {

		if (start > obs_vec_count) {
			return 0;
		}

		int match_found = -1;

		signed int next_index = find_next_index(rot, obs_members,
							match_members,
							start, member_num,
							&match_found, cell);

		if ( member_num < 2 ) {
			return 0;
		}

		if ( next_index < 0 ) {
			/* If there have been too many dead ends, give up
			 * on indexing altogether.
			 **/
			if ( dead_ends > MAX_DEAD_ENDS ) {
				break;
			}

			/* We have not had too many dead ends. Try removing
			 the last member and continue. */
			start = obs_members[member_num - 1] + 1;
			member_num--;
			dead_ends++;

			continue;
		}

		/* we have elongated membership - so reset dead_ends counter */
	//	dead_ends = 0;

		obs_members[member_num] = next_index;
		match_members[member_num] = match_found;

		member_num++;
		
		if (member_num > *max_members) {
			*max_members = member_num;
		}

		/* If member_num is high enough, we want to return a yes */
		if ( member_num > cell->options->member_thresh ) break;
	}

	finish_solution(rot, obs_vecs, obs_members,
			  match_members, member_num, cell);
	
	return ( member_num > cell->options->member_thresh );
}


static int start_seed(int i, int j, int i_match, int j_match,
		      gsl_matrix **rotation, int *max_members,
		      struct TakeTwoCell *cell)
{
	struct SpotVec *obs_vecs = *(cell->obs_vecs);

	gsl_matrix *rot_mat;
	
	rot_mat = generate_rot_mat(obs_vecs[i].obsvec,
				   obs_vecs[j].obsvec,
				   obs_vecs[i].matches[i_match],
				   obs_vecs[j].matches[j_match],
				   cell);

	/* Try to expand this rotation matrix to a larger network */

	int success = grow_network(rot_mat, i, j, i_match, j_match, max_members,
				   cell);

	/* return this matrix and if it was immediately successful */
	*rotation = rot_mat;

	return success;
}

static int find_seed(gsl_matrix **rotation, struct TakeTwoCell *cell)
{
	struct SpotVec *obs_vecs = *(cell->obs_vecs);
	int obs_vec_count = cell->obs_vec_count;
	
	/* META: Maximum achieved maximum network membership */
	int max_max_members = 0;
	gsl_matrix *best_rotation = NULL;

//	unsigned long start_time = time(NULL);

	/* loop round pairs of vectors to try and find a suitable
	 * seed to start building a self-consistent network of vectors
	 */
	int i, j;

	for ( i=0; i<obs_vec_count; i++ ) {
		for ( j=0; j<i; j++ ) {

			/** Check to see if there is a shared spot - opportunity
			 * for optimisation by generating a look-up table
			 * by spot instead of by vector.
			 */
			int shared = obs_vecs_share_spot(&obs_vecs[i], &obs_vecs[j]);

			if ( !shared ) continue;

			/* cell vector index matches stored in i, j and total
			 * number stored in int matches.
			 */
			int *i_idx = 0;
			int *j_idx = 0;
			int matches;
				
			/* Check to see if any angles match from the cell vectors */
			obs_vecs_match_angles(&obs_vecs[i], &obs_vecs[j],
					      &i_idx, &j_idx, &matches, cell);
			if ( matches == 0 ) {
				free(i_idx);
				free(j_idx);
				continue;
			}

			/* Weed out the duplicate seeds (from symmetric
			 * reflection pairs)
			 */

			weed_duplicate_matches(&obs_vecs[i], &obs_vecs[j],
					       &i_idx, &j_idx, &matches, cell);

			/* We have seeds! Pass each of them through the seed-starter  */
			/* If a seed has the highest achieved membership, make note...*/
			int k;
			for ( k=0; k<matches; k++ ) {
				if (i_idx[k] < 0 || j_idx[k] < 0) {
					continue;
				}

				int max_members = 0;

				int success = start_seed(i, j,
							 i_idx[k], j_idx[k],
							 rotation, &max_members,
							 cell);

				if (success) {
					free(i_idx); free(j_idx);
					gsl_matrix_free(best_rotation);
					return success;
				} else {
					if (max_members > max_max_members) {
						max_max_members = max_members;
						gsl_matrix_free(best_rotation);
						best_rotation = *rotation;
						*rotation = NULL;
					} else {
						gsl_matrix_free(*rotation);
						*rotation = NULL;
					}
				}
			}

			free(i_idx);
			free(j_idx);
		}
	} /* yes this } is meant to be here */

	*rotation = best_rotation;
	return (best_rotation != NULL);
}

static void set_gsl_matrix(gsl_matrix *mat, double asx, double asy, double asz,
                           double bsx, double bsy, double bsz,
                           double csx, double csy, double csz)
{
	gsl_matrix_set(mat, 0, 0, asx);
	gsl_matrix_set(mat, 0, 1, asy);
	gsl_matrix_set(mat, 0, 2, asz);
	gsl_matrix_set(mat, 1, 0, bsx);
	gsl_matrix_set(mat, 1, 1, bsy);
	gsl_matrix_set(mat, 1, 2, bsz);
	gsl_matrix_set(mat, 2, 0, csx);
	gsl_matrix_set(mat, 2, 1, csy);
	gsl_matrix_set(mat, 2, 2, csz);
}

static int generate_rotation_sym_ops(struct TakeTwoCell *ttCell)
{
	SymOpList *rawList = sym_ops_for_cell(ttCell->cell);

	/* Now we must convert these into rotation matrices rather than hkl
	 * transformations (affects triclinic, monoclinic, rhombohedral and
	 * hexagonal space groups only) */
	 
	double asx, asy, asz;
	double bsx, bsy, bsz;
	double csx, csy, csz;

	gsl_matrix *recip = gsl_matrix_alloc(3, 3);
	gsl_matrix *cart = gsl_matrix_alloc(3, 3);
	cell_get_reciprocal(ttCell->cell, &asx, &asy, &asz, &bsx, &bsy,
						&bsz, &csx, &csy, &csz);

	set_gsl_matrix(recip, asx, asy, asz, bsx, bsy, bsz, csx, csy, csz);

	cell_get_cartesian(ttCell->cell, &asx, &asy, &asz, &bsx, &bsy,
						&bsz, &csx, &csy, &csz);
	
	set_gsl_matrix(cart, asx, asy, asz, bsx, bsy, bsz, csx, csy, csz);

	int i, j, k;
	int numOps = num_equivs(rawList, NULL);
	
	ttCell->rotSymOps = malloc(numOps * sizeof(gsl_matrix *));
	ttCell->numOps = numOps;
	
	if (ttCell->rotSymOps == NULL) {
		apologise();
		return 0;
	}
	
	for (i = 0; i < numOps; i++)
	{
		gsl_matrix *symOp = gsl_matrix_alloc(3, 3);
		IntegerMatrix *op = get_symop(rawList, NULL, i);
		
		for (j = 0; j < 3; j++) {
		for (k = 0; k < 3; k++) {
			gsl_matrix_set(symOp, j, k, intmat_get(op, j, k));
		}
		}
		
		gsl_matrix *first = gsl_matrix_calloc(3, 3);
		gsl_matrix *second = gsl_matrix_calloc(3, 3);
		
		/* Each equivalence is of the form:
	 	   cartesian * symOp * reciprocal.
	 	   First multiplication: symOp * reciprocal */
	 	   
		gsl_blas_dgemm(CblasNoTrans, CblasNoTrans,
		               1.0, symOp, recip,
		               0.0, first);
		
		/* Second multiplication: cartesian * first */
		
		gsl_blas_dgemm(CblasNoTrans, CblasNoTrans,
		               1.0, cart, first,
		               0.0, second);
		
		ttCell->rotSymOps[i] = second;
		
		gsl_matrix_free(symOp);
		gsl_matrix_free(first);
	}

	gsl_matrix_free(cart);
	gsl_matrix_free(recip);
	
	free_symoplist(rawList);
	
	return 1;
}


struct sortme
{
	struct rvec v;
	double dist;
};

static int sort_func(const void *av, const void *bv)
{
	struct sortme *a = (struct sortme *)av;
	struct sortme *b = (struct sortme *)bv;
	return a->dist > b->dist;
}


static int match_obs_to_cell_vecs(struct rvec *cell_vecs, int cell_vec_count,
				  struct SpotVec *obs_vecs, int obs_vec_count,
				  int is_asymmetric, struct TakeTwoCell *cell)
{
	int i, j;

	for ( i=0; i<obs_vec_count; i++ ) {

		int count = 0;
		struct sortme *for_sort = NULL;
		
		for ( j=0; j<cell_vec_count; j++ ) {
			/* get distance for unit cell vector */
			double cell_length = rvec_length(cell_vecs[j]);
			double obs_length = obs_vecs[i].distance;

			/* check if this matches the observed length */
			double dist_diff = fabs(cell_length - obs_length);

			if ( dist_diff > cell->options->len_tol ) continue;

			/* we have a match, add to array! */

			size_t new_size = (count+1)*sizeof(struct sortme);
			for_sort = realloc(for_sort, new_size);

			if ( for_sort == NULL ) return 0;

			for_sort[count].v = cell_vecs[j];
			for_sort[count].dist = dist_diff;
			count++;

		}

		/* Pointers to relevant things */
		
		struct rvec **match_array;
		int *match_count;
		
		if (!is_asymmetric) {
			match_array = &(obs_vecs[i].matches);
			match_count = &(obs_vecs[i].match_num);
		} else {
			match_array = &(obs_vecs[i].asym_matches);
			match_count = &(obs_vecs[i].asym_match_num);		
		}

		/* Sort in order to get most agreeable matches first */
		qsort(for_sort, count, sizeof(struct sortme), sort_func);
		*match_array = malloc(count*sizeof(struct rvec));
		*match_count = count;
		for ( j=0; j<count; j++ ) {
			(*match_array)[j] = for_sort[j].v;
			
		}
		free(for_sort);
	}

	return 1;
}

static int compare_spot_vecs(const void *av, const void *bv)
{
	struct SpotVec *a = (struct SpotVec *)av;
	struct SpotVec *b = (struct SpotVec *)bv;
	return a->distance > b->distance;
}

static int gen_observed_vecs(struct rvec *rlps, int rlp_count,
			     struct SpotVec **obs_vecs, int *obs_vec_count)
{
	int i, j;
	int count = 0;

	/* maximum distance squared for comparisons */
	double max_sq_length = pow(MAX_RECIP_DISTANCE, 2);

	for ( i=0; i<rlp_count-1 && count < MAX_OBS_VECTORS; i++ ) {
		for ( j=i+1; j<rlp_count; j++ ) {


			/* calculate difference vector between rlps */
			struct rvec diff = diff_vec(rlps[i], rlps[j]);

			/* are these two far from each other? */
			double sqlength = sq_length(diff);

			if ( sqlength > max_sq_length ) continue;

			count++;

			struct SpotVec *temp_obs_vecs;
			temp_obs_vecs = realloc(*obs_vecs,
						count*sizeof(struct SpotVec));

			if ( temp_obs_vecs == NULL ) {
				return 0;
			} else {
				*obs_vecs = temp_obs_vecs;

				/* initialise all SpotVec struct members */

				struct SpotVec spot_vec;
				spot_vec.obsvec = diff;
				spot_vec.distance = sqrt(sqlength);
				spot_vec.matches = NULL;
				spot_vec.match_num = 0;
				spot_vec.her_rlp = &rlps[i];
				spot_vec.his_rlp = &rlps[j];

				(*obs_vecs)[count - 1] = spot_vec;
			}
		}
	}

	/* Sort such that the shortest and least error-prone distances
	   are searched first.
	 */
	qsort(*obs_vecs, count, sizeof(struct SpotVec), compare_spot_vecs);

	*obs_vec_count = count;

	return 1;
}


static int gen_theoretical_vecs(UnitCell *cell, struct rvec **cell_vecs,
				struct rvec **asym_vecs, int *vec_count,
				int *asym_vec_count)
{
	double a, b, c, alpha, beta, gamma;
	int h_max, k_max, l_max;
	double asx, asy, asz;
	double bsx, bsy, bsz;
	double csx, csy, csz;

	cell_get_reciprocal(cell, &asx, &asy, &asz,
			    &bsx, &bsy, &bsz,
			    &csx, &csy, &csz);

	SymOpList *rawList = sym_ops_for_cell(cell);

	cell_get_parameters(cell, &a, &b, &c, &alpha, &beta, &gamma);

	/* find maximum Miller (h, k, l) indices for a given resolution */
	h_max = MAX_RECIP_DISTANCE * a;
	k_max = MAX_RECIP_DISTANCE * b;
	l_max = MAX_RECIP_DISTANCE * c;

	int h, k, l;
	int _h, _k, _l;
	int count = 0;
	int asym_count = 0;

	for ( h=-h_max; h<=+h_max; h++ ) {
	for ( k=-k_max; k<=+k_max; k++ ) {
	for ( l=-l_max; l<=+l_max; l++ ) {

		struct rvec cell_vec;

		/* Exclude systematic absences from centering concerns */
		if ( forbidden_reflection(cell, h, k, l) ) continue;

		int asymmetric = 0;
		get_asymm(rawList, h, k, l, &_h, &_k, &_l);
		
		if (h == _h && k == _k && l == _l) {
			asymmetric = 1;
			asym_count++;
		}
		
		cell_vec.u = h*asx + k*bsx + l*csx;
		cell_vec.v = h*asy + k*bsy + l*csy;
		cell_vec.w = h*asz + k*bsz + l*csz;

		/* add this to our array - which may require expanding */
		count++;

		struct rvec *temp_cell_vecs;
		temp_cell_vecs = realloc(*cell_vecs, count*sizeof(struct rvec));
		struct rvec *temp_asym_vecs = NULL;

		if (asymmetric) {
			temp_asym_vecs = realloc(*asym_vecs,
			                         count*sizeof(struct rvec));
		}

		if ( temp_cell_vecs == NULL ) {
			return 0;
		} else if (asymmetric && temp_asym_vecs == NULL) {
			return 0;
		} else {
			*cell_vecs = temp_cell_vecs;
			(*cell_vecs)[count - 1] = cell_vec;
			
			if (!asymmetric) {
				continue;
			}
			
			*asym_vecs = temp_asym_vecs;
			(*asym_vecs)[asym_count - 1] = cell_vec;
		}
	}
	}
	}

	*vec_count = count;
	*asym_vec_count = asym_count;
	
	free_symoplist(rawList);


	return 1;
}


/* ------------------------------------------------------------------------
 * cleanup functions - called from run_taketwo().
 * ------------------------------------------------------------------------*/
 
static void cleanup_taketwo_obs_vecs(struct SpotVec *obs_vecs,
				     int obs_vec_count)
{
	int i;
	for ( i=0; i<obs_vec_count; i++ ) {
		free(obs_vecs[i].matches);
		free(obs_vecs[i].asym_matches);
	}

	free(obs_vecs);
}

static void cleanup_taketwo_cell(struct TakeTwoCell *ttCell)
{
	int i;
	for ( i=0; i<ttCell->numOps; i++ ) {
		gsl_matrix_free(ttCell->rotSymOps[i]);
	}

	free(ttCell->vec1Tmp);
	free(ttCell->vec2Tmp);
	free(ttCell->twiz1Tmp);
	free(ttCell->twiz2Tmp);
	free(ttCell->rotSymOps);
}


/* ------------------------------------------------------------------------
 * external functions - top level functions (Level 1)
 * ------------------------------------------------------------------------*/

/**
 * @cell: target unit cell
 * @rlps: spot positions on detector back-projected into recripocal
 * space depending on detector geometry etc.
 * @rlp_count: number of rlps in rlps array.
 * @rot: pointer to be given an assignment (hopefully) if indexing is
 * successful.
 **/
static UnitCell *run_taketwo(UnitCell *cell, struct taketwo_options *opts,
			     struct rvec *rlps, int rlp_count)
{
	int cell_vec_count = 0;
	int asym_vec_count = 0;
	struct rvec *cell_vecs = NULL;
	struct rvec *asym_vecs = NULL;
	UnitCell *result;
	int obs_vec_count = 0;
	struct SpotVec *obs_vecs = NULL;
	int success = 0;
	gsl_matrix *solution = NULL;
	
	struct TakeTwoCell ttCell;
	ttCell.cell = cell;
	ttCell.options = opts;
	ttCell.rotSymOps = NULL;
	ttCell.twiz1Tmp = gsl_matrix_calloc(3, 3);
	ttCell.twiz2Tmp = gsl_matrix_calloc(3, 3);
	ttCell.vec1Tmp = gsl_vector_calloc(3);
	ttCell.vec2Tmp = gsl_vector_calloc(3);
	ttCell.numOps = 0;

	success = generate_rotation_sym_ops(&ttCell);

	success = gen_theoretical_vecs(cell, &cell_vecs, &asym_vecs,
	                               &cell_vec_count, &asym_vec_count);
	if ( !success ) return NULL;

	success = gen_observed_vecs(rlps, rlp_count, &obs_vecs, &obs_vec_count);
	if ( !success ) return NULL;

	ttCell.obs_vecs = &obs_vecs;
	ttCell.obs_vec_count = obs_vec_count;

	success = match_obs_to_cell_vecs(asym_vecs, asym_vec_count,
					 obs_vecs, obs_vec_count, 1, &ttCell);

	success = match_obs_to_cell_vecs(cell_vecs, cell_vec_count,
					 obs_vecs, obs_vec_count, 0, &ttCell);

	free(cell_vecs);
	free(asym_vecs);

	if ( !success ) return NULL;

	find_seed(&solution, &ttCell);

	if ( solution == NULL ) {
		cleanup_taketwo_obs_vecs(obs_vecs, obs_vec_count);
		return NULL;
	}

	result = transform_cell_gsl(cell, solution);
	gsl_matrix_free(solution);
	cleanup_taketwo_obs_vecs(obs_vecs, obs_vec_count);
	cleanup_taketwo_cell(&ttCell);
	
	return result;
}


/* CrystFEL interface hooks */

int taketwo_index(struct image *image, struct taketwo_options *opts, void *priv)
{
	Crystal *cr;
	UnitCell *cell;
	struct rvec *rlps;
	int n_rlps = 0;
	int i;
	struct taketwo_private *tp = (struct taketwo_private *)priv;

	/* Replace negative values in options with defaults */

	if (opts->member_thresh < 0) {
		opts->member_thresh = NETWORK_MEMBER_THRESHOLD;
	}

	if (opts->len_tol < 0) {
		opts->len_tol = RECIP_TOLERANCE;
	}
	
	if (opts->angle_tol < 0) {
		opts->angle_tol = ANGLE_TOLERANCE;
	}

	if (opts->trace_tol < 0) {
		opts->trace_tol = TRACE_TOLERANCE;
	}
	
	opts->trace_tol = sqrt(4.0*(1.0-cos(opts->trace_tol)));

	rlps = malloc((image_feature_count(image->features)+1)*sizeof(struct rvec));
	for ( i=0; i<image_feature_count(image->features); i++ ) {
		struct imagefeature *pk = image_get_feature(image->features, i);
		if ( pk == NULL ) continue;
		rlps[n_rlps].u = pk->rx;
		rlps[n_rlps].v = pk->ry;
		rlps[n_rlps].w = pk->rz;
		n_rlps++;
	}
	rlps[n_rlps].u = 0.0;
	rlps[n_rlps].v = 0.0;
	rlps[n_rlps++].w = 0.0;

	cell = run_taketwo(tp->cell, opts, rlps, n_rlps);
	free(rlps);
	if ( cell == NULL ) return 0;

	cr = crystal_new();
	if ( cr == NULL ) {
		ERROR("Failed to allocate crystal.\n");
		return 0;
	}

	crystal_set_cell(cr, cell);

	if ( tp->indm & INDEXING_CHECK_PEAKS ) {
		if ( !peak_sanity_check(image, &cr, 1) ) {
			cell_free(cell);
			crystal_free(cr);
		//	STATUS("Rubbish!!\n");

			return 0;
		} else {
		//	STATUS("That's good!\n");
		}
	}

	image_add_crystal(image, cr);

	return 1;
}


void *taketwo_prepare(IndexingMethod *indm, UnitCell *cell,
                      struct detector *det, float *ltl)
{
	struct taketwo_private *tp;

	/* Flags that TakeTwo knows about */
	*indm &= INDEXING_METHOD_MASK | INDEXING_CHECK_PEAKS
	| INDEXING_USE_LATTICE_TYPE | INDEXING_USE_CELL_PARAMETERS
	| INDEXING_CONTROL_FLAGS;

	if ( !( (*indm & INDEXING_USE_LATTICE_TYPE)
	       && (*indm & INDEXING_USE_CELL_PARAMETERS)) )
	{
		ERROR("TakeTwo indexing requires cell and lattice type "
		      "information.\n");
		return NULL;
	}

	if ( cell == NULL ) {
		ERROR("TakeTwo indexing requires a unit cell.\n");
		return NULL;
	}

	STATUS("*******************************************************************\n");
	STATUS("*****                    Welcome to TakeTwo                   *****\n");
	STATUS("*******************************************************************\n");
	STATUS("      If you use these indexing results, please keep a roof\n");
	STATUS("           over the author's head by citing this paper.\n\n");

	STATUS("o     o     o     o     o     o     o     o     o     o     o     o\n");
	STATUS("   o     o     o     o     o     o     o     o     o     o     o   \n");
	STATUS("o                                                                 o\n");
	STATUS("   o                      The citation is:                     o   \n");
	STATUS("o           Ginn et al., Acta Cryst. (2016). D72, 956-965         o\n");
	STATUS("   o                         Thank you!                        o   \n");
	STATUS("o                                                                 o\n");
	STATUS("   o     o     o     o     o     o     o     o     o     o     o   \n");
	STATUS("o     o     o     o     o     o     o     o     o     o     o     o\n");


	STATUS("\n");

	tp = malloc(sizeof(struct taketwo_private));
	if ( tp == NULL ) return NULL;

	tp->ltl = ltl;
	tp->cell = cell;
	tp->indm = *indm;

	return tp;
}


void taketwo_cleanup(IndexingPrivate *pp)
{
	struct taketwo_private *tp = (struct taketwo_private *)pp;
	free(tp);
}
