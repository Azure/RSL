/* (c) Microsoft Corporation.  All rights reserved. */
#pragma once

namespace RSLibImpl
{

typedef unsigned __int64 msn_fprint_t;
#pragma warning(disable:4127)
#include <stddef.h>

typedef struct msn_fprint_data_s *msn_fprint_data_t;
/* an opaque type used to keep the data structures need to compute
   fingerprints.  */

msn_fprint_data_t msn_fprint_new ();
/* Computes the tables needed for fingerprint manipulations. */

msn_fprint_data_t msn_fprint_new (msn_fprint_t poly);
/* Computes the tables needed for fingerprint manipulations. */

msn_fprint_t msn_fprint_of (msn_fprint_data_t fp,
                void *data, size_t len);

msn_fprint_t
msn_fprint_of (msn_fprint_data_t fp,
               msn_fprint_t init,
               void *data,
               size_t len );

/* if fp was generated with polynomial P, and bytes
   "data[0, ..., len-1]" contain string B,
   return the fingerprint under P of the concatenation of B.
   Strings are treated as polynomials.  The low-order bit in the first
   byte is the highest degree coefficient in the polynomial.*/

void msn_fprint_destroy (msn_fprint_data_t fp);
/* discard the data associated with "fp" */

} // namespace RSLibImpl
