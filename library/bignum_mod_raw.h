/**
 *  Low-level modular bignum functions
 *
 *  This interface should only be used by the higher-level modular bignum
 *  module (bignum_mod.c) and the ECP module (ecp.c, ecp_curves.c). All other
 *  modules should use the high-level modular bignum interface (bignum_mod.h)
 *  or the legacy bignum interface (bignum.h).
 *
 * This is a low-level interface to operations on integers modulo which
 * has no protection against passing invalid arguments such as arrays of
 * the wrong size. The functions in bignum_mod.h provide a higher-level
 * interface that includes protections against accidental misuse, at the
 * expense of code size and sometimes more cumbersome memory management.
 */

/*
 *  Copyright The Mbed TLS Contributors
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may
 *  not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#ifndef MBEDTLS_BIGNUM_MOD_RAW_H
#define MBEDTLS_BIGNUM_MOD_RAW_H

#include "common.h"

#if defined(MBEDTLS_BIGNUM_C)
#include "mbedtls/bignum.h"
#endif

#include "bignum_mod.h"

/**
 * \brief   Perform a safe conditional copy of an MPI which doesn't reveal
 *          whether the assignment was done or not.
 *
 * The size to copy is determined by \p N.
 *
 * \param[out] X        The address of the destination MPI.
 *                      This must be initialized. Must have enough limbs to
 *                      store the full value of \p A.
 * \param[in]  A        The address of the source MPI. This must be initialized.
 * \param[in]  N        The address of the modulus related to \p X and \p A.
 * \param      assign   The condition deciding whether to perform the
 *                      assignment or not. Must be either 0 or 1:
 *                      * \c 1: Perform the assignment `X = A`.
 *                      * \c 0: Keep the original value of \p X.
 *
 * \note           This function avoids leaking any information about whether
 *                 the assignment was done or not.
 *
 * \warning        If \p assign is neither 0 nor 1, the result of this function
 *                 is indeterminate, and the resulting value in \p X might be
 *                 neither its original value nor the value in \p A.
 */
void mbedtls_mpi_mod_raw_cond_assign( mbedtls_mpi_uint *X,
                                      const mbedtls_mpi_uint *A,
                                      const mbedtls_mpi_mod_modulus *N,
                                      unsigned char assign );

/**
 * \brief   Perform a safe conditional swap of two MPIs which doesn't reveal
 *          whether the swap was done or not.
 *
 * The size to swap is determined by \p N.
 *
 * \param[in,out] X     The address of the first MPI. This must be initialized.
 * \param[in,out] Y     The address of the second MPI. This must be initialized.
 * \param[in]     N     The address of the modulus related to \p X and \p Y.
 * \param         swap  The condition deciding whether to perform
 *                      the swap or not. Must be either 0 or 1:
 *                      * \c 1: Swap the values of \p X and \p Y.
 *                      * \c 0: Keep the original values of \p X and \p Y.
 *
 * \note           This function avoids leaking any information about whether
 *                 the swap was done or not.
 *
 * \warning        If \p swap is neither 0 nor 1, the result of this function
 *                 is indeterminate, and both \p X and \p Y might end up with
 *                 values different to either of the original ones.
 */
void mbedtls_mpi_mod_raw_cond_swap( mbedtls_mpi_uint *X,
                                    mbedtls_mpi_uint *Y,
                                    const mbedtls_mpi_mod_modulus *N,
                                    unsigned char swap );

/** Import X from unsigned binary data.
 *
 * The MPI needs to have enough limbs to store the full value (including any
 * most significant zero bytes in the input).
 *
 * \param[out] X        The address of the MPI. The size is determined by \p m.
 *                      (In particular, it must have at least as many limbs as
 *                      the modulus \p m.)
 * \param[in] m         The address of the modulus related to \p X.
 * \param[in] input     The input buffer to import from.
 * \param input_length  The length in bytes of \p input.
 *
 * \return       \c 0 if successful.
 * \return       #MBEDTLS_ERR_MPI_BUFFER_TOO_SMALL if \p X isn't
 *               large enough to hold the value in \p input.
 * \return       #MBEDTLS_ERR_MPI_BAD_INPUT_DATA if the external representation
 *               of \p m is invalid or \p X is not less than \p m.
 */
int mbedtls_mpi_mod_raw_read( mbedtls_mpi_uint *X,
                              const mbedtls_mpi_mod_modulus *m,
                              const unsigned char *input,
                              size_t input_length );

/** Export A into unsigned binary data.
 *
 * \param[in] A         The address of the MPI. The size is determined by \p m.
 *                      (In particular, it must have at least as many limbs as
 *                      the modulus \p m.)
 * \param[in] m         The address of the modulus related to \p A.
 * \param[out] output   The output buffer to export to.
 * \param output_length The length in bytes of \p output.
 *
 * \return       \c 0 if successful.
 * \return       #MBEDTLS_ERR_MPI_BUFFER_TOO_SMALL if \p output isn't
 *               large enough to hold the value of \p A.
 * \return       #MBEDTLS_ERR_MPI_BAD_INPUT_DATA if the external representation
 *               of \p m is invalid.
 */
int mbedtls_mpi_mod_raw_write( const mbedtls_mpi_uint *A,
                               const mbedtls_mpi_mod_modulus *m,
                               unsigned char *output,
                               size_t output_length );

#endif /* MBEDTLS_BIGNUM_MOD_RAW_H */
