/* Copyright 2026 CSE220 Final Project Group
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction. Provided "AS IS" without warranty.
 */

#ifndef __PREF_SPP_PARAM_H__
#define __PREF_SPP_PARAM_H__

#include "globals/global_types.h"

/**************************************************************************************/
/* extern all of the variables defined in pref_spp.param.def */

#define DEF_PARAM(name, variable, type, func, def, const) extern const type variable;
#include "pref_spp.param.def"
#undef DEF_PARAM

#endif
