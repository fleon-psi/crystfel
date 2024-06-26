/*
 * crystal.c
 *
 * A class representing a single crystal
 *
 * Copyright © 2013-2024 Deutsches Elektronen-Synchrotron DESY,
 *                       a research centre of the Helmholtz Association.
 *
 * Authors:
 *   2013-2024 Thomas White <taw@physics.org>
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

#include <libcrystfel-config.h>

#include "crystal.h"
#include "utils.h"


/**
 * \file crystal.h
 */


struct _crystal
{
	UnitCell               *cell;
	int                     owns_cell;
	double                  m;     /* Mosaicity in radians */
	double                  osf;
	double                  Bfac;
	double                  profile_radius;
	double                  resolution_limit;
	long long int           n_saturated;   /* Number of overloads */
	long long int           n_implausible; /* Number of implausibly negative reflectionss */
	int                     user_flag;
	char                   *notes; /* Text notes, which go in the stream */
	double			det_shift_x; /* Detector x-shift in metres */
	double			det_shift_y; /* Detector y-shift in metres */
};


/************************** Constructor / Destructor **************************/


/**
 * Create a new \ref Crystal.
 *
 * \returns The new unit cell, or NULL on failure.
 *
 */
Crystal *crystal_new()
{
	Crystal *cryst;

	cryst = cfmalloc(sizeof(Crystal));
	if ( cryst == NULL ) return NULL;

	cryst->cell = NULL;
	cryst->owns_cell = 1;
	cryst->m = 0.0;
	cryst->osf = 1.0;
	cryst->Bfac = 0.0;
	cryst->profile_radius = 0.0;
	cryst->resolution_limit = INFINITY;
	cryst->n_saturated = 0;
	cryst->n_implausible = 0;
	cryst->user_flag = 0;
	cryst->notes = NULL;
	cryst->det_shift_x = 0;
	cryst->det_shift_y = 0;

	return cryst;
}


/**
 * \param cryst: A \ref Crystal to copy.
 *
 * Creates a new \ref Crystal which is a copy of \p cryst.  The copy is a "deep
 * copy", which means that copies ARE made of the data structures which
 * \p cryst contains references to, for example its \ref UnitCell.
 *
 * \returns A (deep) copy of \p cryst, or NULL on failure.
 *
 */
Crystal *crystal_copy(const Crystal *cryst)
{
	Crystal *c;

	c = crystal_new();
	if ( c == NULL ) return NULL;

	memcpy(c, cryst, sizeof(Crystal));
	if ( c->notes != NULL ) c->notes = cfstrdup(c->notes);

	if ( cryst->cell != NULL ) {
		UnitCell *cell;
		cell = cell_new_from_cell(cryst->cell);
		if ( cell == NULL ) return NULL;
		c->cell = cell;
	}

	return c;
}


/**
 * \param cryst: A \ref Crystal to free.
 *
 * Frees a \ref Crystal, and all internal resources concerning that crystal.
 *
 */
void crystal_free(Crystal *cryst)
{
	if ( cryst == NULL ) return;
	if ( cryst->owns_cell ) cell_free(cryst->cell);
	cffree(cryst->notes);
	cffree(cryst);
}


/********************************** Getters ***********************************/


UnitCell *crystal_get_cell(Crystal *cryst)
{
	return cryst->cell;
}


UnitCell *crystal_relinquish_cell(Crystal *cryst)
{
	cryst->owns_cell = 0;
	return cryst->cell;
}


const UnitCell *crystal_get_cell_const(const Crystal *cryst)
{
	return cryst->cell;
}


double crystal_get_profile_radius(const Crystal *cryst)
{
	return cryst->profile_radius;
}


double crystal_get_resolution_limit(Crystal *cryst)
{
	return cryst->resolution_limit;
}


long long int crystal_get_num_saturated_reflections(Crystal *cryst)
{
	return cryst->n_saturated;
}


long long int crystal_get_num_implausible_reflections(Crystal *cryst)
{
	return cryst->n_implausible;
}


double crystal_get_osf(Crystal *cryst)
{
	return cryst->osf;
}


double crystal_get_Bfac(Crystal *cryst)
{
	return cryst->Bfac;
}


int crystal_get_user_flag(Crystal *cryst)
{
	return cryst->user_flag;
}


double crystal_get_mosaicity(Crystal *cryst)
{
	return cryst->m;
}


const char *crystal_get_notes(Crystal *cryst)
{
	return cryst->notes;
}


void crystal_get_det_shift(Crystal *cryst, double* shift_x, double *shift_y)
{
	*shift_x = cryst->det_shift_x;
	*shift_y = cryst->det_shift_y;
}



/********************************** Setters ***********************************/


void crystal_set_cell(Crystal *cryst, UnitCell *cell)
{
	if ( cryst->owns_cell ) cell_free(cryst->cell);
	cryst->cell = cell;
	cryst->owns_cell = 1;
}


void crystal_set_profile_radius(Crystal *cryst, double r)
{
	cryst->profile_radius = r;
}


void crystal_set_resolution_limit(Crystal *cryst, double res)
{
	cryst->resolution_limit = res;
}


void crystal_set_num_saturated_reflections(Crystal *cryst, long long int n)
{
	cryst->n_saturated = n;
}


void crystal_set_num_implausible_reflections(Crystal *cryst, long long int n)
{
	cryst->n_implausible = n;
}


void crystal_set_osf(Crystal *cryst, double osf)
{
	cryst->osf = osf;
}


void crystal_set_Bfac(Crystal *cryst, double Bfac)
{
	cryst->Bfac = Bfac;
}


void crystal_set_user_flag(Crystal *cryst, int user_flag)
{
	cryst->user_flag = user_flag;
}


void crystal_set_mosaicity(Crystal *cryst, double m)
{
	cryst->m = m;
}


void crystal_set_notes(Crystal *cryst, const char *notes)
{
	cffree(cryst->notes);  /* free(NULL) is OK */
	cryst->notes = cfstrdup(notes);
}


void crystal_add_notes(Crystal *cryst, const char *notes_add)
{
	size_t len;
	char *nnotes;

	if ( cryst->notes == NULL ) {
		crystal_set_notes(cryst, notes_add);
		return;
	}

	len = strlen(notes_add) + strlen(cryst->notes) + 2;
	nnotes = cfmalloc(len);
	if ( nnotes == NULL ) {
		ERROR("Failed to add notes to crystal.\n");
		return;
	}

	strcpy(nnotes, cryst->notes);
	strcat(nnotes, "\n");
	strcat(nnotes, notes_add);
	cffree(cryst->notes);
	cryst->notes = nnotes;
}


void crystal_set_det_shift(Crystal *cryst, double shift_x, double shift_y)
{
	cryst->det_shift_x = shift_x;
	cryst->det_shift_y = shift_y;
}
