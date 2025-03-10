/*
 *  PSA PAKE layer on top of Mbed TLS software crypto
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

#include "common.h"

#if defined(MBEDTLS_PSA_CRYPTO_C)

#include <psa/crypto.h>
#include "psa_crypto_core.h"
#include "psa_crypto_slot_management.h"

#include <mbedtls/ecjpake.h>
#include <mbedtls/psa_util.h>

#include <mbedtls/platform.h>
#include <mbedtls/error.h>
#include <string.h>

/*
 * State sequence:
 *
 *   psa_pake_setup()
 *   |
 *   |-- In any order:
 *   |   | psa_pake_set_password_key()
 *   |   | psa_pake_set_user()
 *   |   | psa_pake_set_peer()
 *   |   | psa_pake_set_role()
 *   |
 *   |--- In any order: (First round input before or after first round output)
 *   |   |
 *   |   |------ In Order
 *   |   |       | psa_pake_output(PSA_PAKE_STEP_KEY_SHARE)
 *   |   |       | psa_pake_output(PSA_PAKE_STEP_ZK_PUBLIC)
 *   |   |       | psa_pake_output(PSA_PAKE_STEP_ZK_PROOF)
 *   |   |       | psa_pake_output(PSA_PAKE_STEP_KEY_SHARE)
 *   |   |       | psa_pake_output(PSA_PAKE_STEP_ZK_PUBLIC)
 *   |   |       | psa_pake_output(PSA_PAKE_STEP_ZK_PROOF)
 *   |   |
 *   |   |------ In Order:
 *   |           | psa_pake_input(PSA_PAKE_STEP_KEY_SHARE)
 *   |           | psa_pake_input(PSA_PAKE_STEP_ZK_PUBLIC)
 *   |           | psa_pake_input(PSA_PAKE_STEP_ZK_PROOF)
 *   |           | psa_pake_input(PSA_PAKE_STEP_KEY_SHARE)
 *   |           | psa_pake_input(PSA_PAKE_STEP_ZK_PUBLIC)
 *   |           | psa_pake_input(PSA_PAKE_STEP_ZK_PROOF)
 *   |
 *   |--- In any order: (Second round input before or after second round output)
 *   |   |
 *   |   |------ In Order
 *   |   |       | psa_pake_output(PSA_PAKE_STEP_KEY_SHARE)
 *   |   |       | psa_pake_output(PSA_PAKE_STEP_ZK_PUBLIC)
 *   |   |       | psa_pake_output(PSA_PAKE_STEP_ZK_PROOF)
 *   |   |
 *   |   |------ In Order:
 *   |           | psa_pake_input(PSA_PAKE_STEP_KEY_SHARE)
 *   |           | psa_pake_input(PSA_PAKE_STEP_ZK_PUBLIC)
 *   |           | psa_pake_input(PSA_PAKE_STEP_ZK_PROOF)
 *   |
 *   psa_pake_get_implicit_key()
 *   psa_pake_abort()
 */

enum psa_pake_step
{
    PSA_PAKE_STEP_INVALID       = 0,
    PSA_PAKE_STEP_X1_X2         = 1,
    PSA_PAKE_STEP_X2S           = 2,
    PSA_PAKE_STEP_DERIVE        = 3,
};

enum psa_pake_state
{
    PSA_PAKE_STATE_INVALID      = 0,
    PSA_PAKE_STATE_SETUP        = 1,
    PSA_PAKE_STATE_READY        = 2,
    PSA_PAKE_OUTPUT_X1_X2       = 3,
    PSA_PAKE_OUTPUT_X2S         = 4,
    PSA_PAKE_INPUT_X1_X2        = 5,
    PSA_PAKE_INPUT_X4S          = 6,
};

/*
 * The first PAKE step shares the same sequences of the second PAKE step
 * but with a second set of KEY_SHARE/ZK_PUBLIC/ZK_PROOF outputs/inputs.
 * It's simpler to share the same sequences numbers of the first
 * set of KEY_SHARE/ZK_PUBLIC/ZK_PROOF outputs/inputs in both PAKE steps.
 *
 * State sequence with step, state & sequence enums:
 *   => Input & Output Step = PSA_PAKE_STEP_INVALID
 *   => state = PSA_PAKE_STATE_INVALID
 *   psa_pake_setup()
 *   => Input & Output Step = PSA_PAKE_STEP_X1_X2
 *   => state = PSA_PAKE_STATE_SETUP
 *   => sequence = PSA_PAKE_SEQ_INVALID
 *   |
 *   |--- In any order: (First round input before or after first round output)
 *   |   | First call of psa_pake_output() or psa_pake_input() sets
 *   |   | state = PSA_PAKE_STATE_READY
 *   |   |
 *   |   |------ In Order: => state = PSA_PAKE_OUTPUT_X1_X2
 *   |   |       | psa_pake_output() => sequence = PSA_PAKE_X1_STEP_KEY_SHARE
 *   |   |       | psa_pake_output() => sequence = PSA_PAKE_X1_STEP_ZK_PUBLIC
 *   |   |       | psa_pake_output() => sequence = PSA_PAKE_X1_STEP_ZK_PROOF
 *   |   |       | psa_pake_output() => sequence = PSA_PAKE_X2_STEP_KEY_SHARE
 *   |   |       | psa_pake_output() => sequence = PSA_PAKE_X2_STEP_ZK_PUBLIC
 *   |   |       | psa_pake_output() => sequence = PSA_PAKE_X2_STEP_ZK_PROOF
 *   |   |       | => state = PSA_PAKE_STATE_READY
 *   |   |       | => sequence = PSA_PAKE_SEQ_INVALID
 *   |   |       | => Output Step = PSA_PAKE_STEP_X2S
 *   |   |
 *   |   |------ In Order: => state = PSA_PAKE_INPUT_X1_X2
 *   |   |       | psa_pake_input() => sequence = PSA_PAKE_X1_STEP_KEY_SHARE
 *   |   |       | psa_pake_input() => sequence = PSA_PAKE_X1_STEP_ZK_PUBLIC
 *   |   |       | psa_pake_input() => sequence = PSA_PAKE_X1_STEP_ZK_PROOF
 *   |   |       | psa_pake_input() => sequence = PSA_PAKE_X2_STEP_KEY_SHARE
 *   |   |       | psa_pake_input() => sequence = PSA_PAKE_X2_STEP_ZK_PUBLIC
 *   |   |       | psa_pake_input() => sequence = PSA_PAKE_X2_STEP_ZK_PROOF
 *   |   |       | => state = PSA_PAKE_STATE_READY
 *   |   |       | => sequence = PSA_PAKE_SEQ_INVALID
 *   |   |       | => Output Step = PSA_PAKE_INPUT_X4S
 *   |
 *   |--- In any order: (Second round input before or after second round output)
 *   |   |
 *   |   |------ In Order: => state = PSA_PAKE_OUTPUT_X2S
 *   |   |       | psa_pake_output() => sequence = PSA_PAKE_X1_STEP_KEY_SHARE
 *   |   |       | psa_pake_output() => sequence = PSA_PAKE_X1_STEP_ZK_PUBLIC
 *   |   |       | psa_pake_output() => sequence = PSA_PAKE_X1_STEP_ZK_PROOF
 *   |   |       | => state = PSA_PAKE_STATE_READY
 *   |   |       | => sequence = PSA_PAKE_SEQ_INVALID
 *   |   |       | => Output Step = PSA_PAKE_STEP_DERIVE
 *   |   |
 *   |   |------ In Order: => state = PSA_PAKE_INPUT_X4S
 *   |   |       | psa_pake_input() => sequence = PSA_PAKE_X1_STEP_KEY_SHARE
 *   |   |       | psa_pake_input() => sequence = PSA_PAKE_X1_STEP_ZK_PUBLIC
 *   |   |       | psa_pake_input() => sequence = PSA_PAKE_X1_STEP_ZK_PROOF
 *   |   |       | => state = PSA_PAKE_STATE_READY
 *   |   |       | => sequence = PSA_PAKE_SEQ_INVALID
 *   |   |       | => Output Step = PSA_PAKE_STEP_DERIVE
 *   |
 *   psa_pake_get_implicit_key()
 *   => Input & Output Step = PSA_PAKE_STEP_INVALID
 */
enum psa_pake_sequence
{
    PSA_PAKE_SEQ_INVALID        = 0,
    PSA_PAKE_X1_STEP_KEY_SHARE  = 1,    /* also X2S & X4S KEY_SHARE */
    PSA_PAKE_X1_STEP_ZK_PUBLIC  = 2,    /* also X2S & X4S ZK_PUBLIC */
    PSA_PAKE_X1_STEP_ZK_PROOF   = 3,    /* also X2S & X4S ZK_PROOF */
    PSA_PAKE_X2_STEP_KEY_SHARE  = 4,
    PSA_PAKE_X2_STEP_ZK_PUBLIC  = 5,
    PSA_PAKE_X2_STEP_ZK_PROOF   = 6,
    PSA_PAKE_SEQ_END            = 7,
};

#if defined(MBEDTLS_PSA_BUILTIN_ALG_JPAKE)
static psa_status_t mbedtls_ecjpake_to_psa_error( int ret )
{
    switch( ret )
    {
        case MBEDTLS_ERR_MPI_BAD_INPUT_DATA:
        case MBEDTLS_ERR_ECP_BAD_INPUT_DATA:
        case MBEDTLS_ERR_ECP_INVALID_KEY:
        case MBEDTLS_ERR_ECP_VERIFY_FAILED:
            return( PSA_ERROR_DATA_INVALID );
        case MBEDTLS_ERR_MPI_BUFFER_TOO_SMALL:
        case MBEDTLS_ERR_ECP_BUFFER_TOO_SMALL:
            return( PSA_ERROR_BUFFER_TOO_SMALL );
        case MBEDTLS_ERR_MD_FEATURE_UNAVAILABLE:
            return( PSA_ERROR_NOT_SUPPORTED );
        case MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED:
            return( PSA_ERROR_CORRUPTION_DETECTED );
        default:
            return( PSA_ERROR_GENERIC_ERROR );
    }
}
#endif

#if defined(MBEDTLS_PSA_BUILTIN_PAKE)
psa_status_t psa_pake_setup( psa_pake_operation_t *operation,
                             const psa_pake_cipher_suite_t *cipher_suite)
{
    /* A context must be freshly initialized before it can be set up. */
    if( operation->alg != PSA_ALG_NONE )
        return( PSA_ERROR_BAD_STATE );

    if( cipher_suite == NULL ||
        PSA_ALG_IS_PAKE(cipher_suite->algorithm ) == 0 ||
        ( cipher_suite->type != PSA_PAKE_PRIMITIVE_TYPE_ECC &&
          cipher_suite->type != PSA_PAKE_PRIMITIVE_TYPE_DH ) ||
        PSA_ALG_IS_HASH( cipher_suite->hash ) == 0 )
    {
        return( PSA_ERROR_INVALID_ARGUMENT );
    }

#if defined(MBEDTLS_PSA_BUILTIN_ALG_JPAKE)
    if( cipher_suite->algorithm == PSA_ALG_JPAKE )
    {
        if( cipher_suite->type != PSA_PAKE_PRIMITIVE_TYPE_ECC ||
            cipher_suite->family != PSA_ECC_FAMILY_SECP_R1 ||
            cipher_suite->bits != 256 ||
            cipher_suite->hash != PSA_ALG_SHA_256 )
        {
            return( PSA_ERROR_NOT_SUPPORTED );
        }

        operation->alg = cipher_suite->algorithm;

        mbedtls_ecjpake_init( &operation->ctx.ecjpake );

        operation->state = PSA_PAKE_STATE_SETUP;
        operation->sequence = PSA_PAKE_SEQ_INVALID;
        operation->input_step = PSA_PAKE_STEP_X1_X2;
        operation->output_step = PSA_PAKE_STEP_X1_X2;

        mbedtls_platform_zeroize( operation->buffer, PSA_PAKE_BUFFER_SIZE );
        operation->buffer_length = 0;
        operation->buffer_offset = 0;

        return( PSA_SUCCESS );
    }
    else
#endif
    return( PSA_ERROR_NOT_SUPPORTED );
}

psa_status_t psa_pake_set_password_key( psa_pake_operation_t *operation,
                                        mbedtls_svc_key_id_t password )
{
    psa_status_t status = PSA_ERROR_CORRUPTION_DETECTED;
    psa_key_attributes_t attributes = psa_key_attributes_init();
    psa_key_type_t type;
    psa_key_usage_t usage;

    if( operation->alg == PSA_ALG_NONE ||
        operation->state != PSA_PAKE_STATE_SETUP )
    {
        return( PSA_ERROR_BAD_STATE );
    }

    status = psa_get_key_attributes( password, &attributes );
    if( status != PSA_SUCCESS )
        return( status );

    type = psa_get_key_type( &attributes );
    usage = psa_get_key_usage_flags( &attributes );

    psa_reset_key_attributes( &attributes );

    if( type != PSA_KEY_TYPE_PASSWORD &&
        type != PSA_KEY_TYPE_PASSWORD_HASH )
    {
        return( PSA_ERROR_INVALID_ARGUMENT );
    }

    if( ( usage & PSA_KEY_USAGE_DERIVE ) == 0 )
        return( PSA_ERROR_NOT_PERMITTED );

    operation->password = password;

    return( PSA_SUCCESS );
}

psa_status_t psa_pake_set_user( psa_pake_operation_t *operation,
                                const uint8_t *user_id,
                                size_t user_id_len )
{
    if( operation->alg == PSA_ALG_NONE ||
        operation->state != PSA_PAKE_STATE_SETUP )
    {
        return( PSA_ERROR_BAD_STATE );
    }

    if( user_id_len == 0 || user_id == NULL )
        return( PSA_ERROR_INVALID_ARGUMENT );

    return( PSA_ERROR_NOT_SUPPORTED );
}

psa_status_t psa_pake_set_peer( psa_pake_operation_t *operation,
                                const uint8_t *peer_id,
                                size_t peer_id_len )
{
    if( operation->alg == PSA_ALG_NONE ||
        operation->state != PSA_PAKE_STATE_SETUP )
    {
        return( PSA_ERROR_BAD_STATE );
    }

    if( peer_id_len == 0 || peer_id == NULL )
        return( PSA_ERROR_INVALID_ARGUMENT );

    return( PSA_ERROR_NOT_SUPPORTED );
}

psa_status_t psa_pake_set_role( psa_pake_operation_t *operation,
                                psa_pake_role_t role )
{
    if( operation->alg == PSA_ALG_NONE ||
        operation->state != PSA_PAKE_STATE_SETUP )
    {
        return( PSA_ERROR_BAD_STATE );
    }

    if( role != PSA_PAKE_ROLE_NONE &&
        role != PSA_PAKE_ROLE_FIRST &&
        role != PSA_PAKE_ROLE_SECOND &&
        role != PSA_PAKE_ROLE_CLIENT &&
        role != PSA_PAKE_ROLE_SERVER )
    {
        return( PSA_ERROR_INVALID_ARGUMENT );
    }

#if defined(MBEDTLS_PSA_BUILTIN_ALG_JPAKE)
    if( operation->alg == PSA_ALG_JPAKE )
    {
        if( role != PSA_PAKE_ROLE_CLIENT &&
            role != PSA_PAKE_ROLE_SERVER )
            return( PSA_ERROR_NOT_SUPPORTED );

        operation->role = role;

        return( PSA_SUCCESS );
    }
    else
#endif
        return( PSA_ERROR_NOT_SUPPORTED );
}

#if defined(MBEDTLS_PSA_BUILTIN_ALG_JPAKE)
static psa_status_t psa_pake_ecjpake_setup( psa_pake_operation_t *operation )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    psa_status_t status = PSA_ERROR_CORRUPTION_DETECTED;
    mbedtls_ecjpake_role role;
    psa_key_slot_t *slot = NULL;

    if( operation->role == PSA_PAKE_ROLE_CLIENT )
        role = MBEDTLS_ECJPAKE_CLIENT;
    else if( operation->role == PSA_PAKE_ROLE_SERVER )
        role = MBEDTLS_ECJPAKE_SERVER;
    else
        return( PSA_ERROR_BAD_STATE );

    if( psa_is_valid_key_id( operation->password, 1 ) == 0 )
        return( PSA_ERROR_BAD_STATE );

    status = psa_get_and_lock_key_slot( operation->password, &slot );
    if( status != PSA_SUCCESS )
        return( status );


    ret = mbedtls_ecjpake_setup( &operation->ctx.ecjpake,
                                 role,
                                 MBEDTLS_MD_SHA256,
                                 MBEDTLS_ECP_DP_SECP256R1,
                                 slot->key.data, slot->key.bytes );

    psa_unlock_key_slot( slot );
    slot = NULL;

    if( ret != 0 )
        return( mbedtls_ecjpake_to_psa_error( ret ) );

    operation->state = PSA_PAKE_STATE_READY;

    return( PSA_SUCCESS );
}
#endif

psa_status_t psa_pake_output( psa_pake_operation_t *operation,
                              psa_pake_step_t step,
                              uint8_t *output,
                              size_t output_size,
                              size_t *output_length )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    psa_status_t status = PSA_ERROR_CORRUPTION_DETECTED;
    size_t length;

    if( operation->alg == PSA_ALG_NONE ||
        operation->state == PSA_PAKE_STATE_INVALID )
        return( PSA_ERROR_BAD_STATE );

    if( output == NULL || output_size == 0 || output_length == NULL )
        return( PSA_ERROR_INVALID_ARGUMENT );

#if defined(MBEDTLS_PSA_BUILTIN_ALG_JPAKE)
    /*
     * The PSA CRYPTO PAKE and MbedTLS JPAKE API have a different
     * handling of output sequencing.
     *
     * The MbedTLS JPAKE API outputs the whole X1+X2 and X2S steps data
     * at once, on the other side the PSA CRYPTO PAKE api requires
     * the KEY_SHARE/ZP_PUBLIC/ZK_PROOF parts of X1, X2 & X2S to be
     * retrieved in sequence.
     *
     * In order to achieve API compatibility, the whole X1+X2 or X2S steps
     * data is stored in an intermediate buffer at first step output call,
     * and data is sliced down by parsing the ECPoint records in order
     * to return the right parts on each step.
     */
    if( operation->alg == PSA_ALG_JPAKE )
    {
        if( step != PSA_PAKE_STEP_KEY_SHARE &&
            step != PSA_PAKE_STEP_ZK_PUBLIC &&
            step != PSA_PAKE_STEP_ZK_PROOF )
            return( PSA_ERROR_INVALID_ARGUMENT );

        if( operation->state == PSA_PAKE_STATE_SETUP ) {
            status = psa_pake_ecjpake_setup( operation );
            if( status != PSA_SUCCESS )
            {
                psa_pake_abort( operation );
                return( status );
            }
        }

        if( operation->state != PSA_PAKE_STATE_READY &&
            operation->state != PSA_PAKE_OUTPUT_X1_X2 &&
            operation->state != PSA_PAKE_OUTPUT_X2S )
        {
            return( PSA_ERROR_BAD_STATE );
        }

        if( operation->state == PSA_PAKE_STATE_READY )
        {
            if( step != PSA_PAKE_STEP_KEY_SHARE )
                return( PSA_ERROR_BAD_STATE );

            switch( operation->output_step )
            {
                case PSA_PAKE_STEP_X1_X2:
                    operation->state = PSA_PAKE_OUTPUT_X1_X2;
                    break;
                case PSA_PAKE_STEP_X2S:
                    operation->state = PSA_PAKE_OUTPUT_X2S;
                    break;
                default:
                    return( PSA_ERROR_BAD_STATE );
            }

            operation->sequence = PSA_PAKE_X1_STEP_KEY_SHARE;
        }

        /* Check if step matches current sequence */
        switch( operation->sequence )
        {
            case PSA_PAKE_X1_STEP_KEY_SHARE:
            case PSA_PAKE_X2_STEP_KEY_SHARE:
                if( step != PSA_PAKE_STEP_KEY_SHARE )
                    return( PSA_ERROR_BAD_STATE );
                break;

            case PSA_PAKE_X1_STEP_ZK_PUBLIC:
            case PSA_PAKE_X2_STEP_ZK_PUBLIC:
                if( step != PSA_PAKE_STEP_ZK_PUBLIC )
                    return( PSA_ERROR_BAD_STATE );
                break;

            case PSA_PAKE_X1_STEP_ZK_PROOF:
            case PSA_PAKE_X2_STEP_ZK_PROOF:
                if( step != PSA_PAKE_STEP_ZK_PROOF )
                    return( PSA_ERROR_BAD_STATE );
                break;

            default:
                return( PSA_ERROR_BAD_STATE );
        }

        /* Initialize & write round on KEY_SHARE sequences */
        if( operation->state == PSA_PAKE_OUTPUT_X1_X2 &&
            operation->sequence == PSA_PAKE_X1_STEP_KEY_SHARE )
        {
            ret = mbedtls_ecjpake_write_round_one( &operation->ctx.ecjpake,
                                                   operation->buffer,
                                                   PSA_PAKE_BUFFER_SIZE,
                                                   &operation->buffer_length,
                                                   mbedtls_psa_get_random,
                                                   MBEDTLS_PSA_RANDOM_STATE );
            if( ret != 0 )
            {
                psa_pake_abort( operation );
                return( mbedtls_ecjpake_to_psa_error( ret ) );
            }

            operation->buffer_offset = 0;
        }
        else if( operation->state == PSA_PAKE_OUTPUT_X2S &&
                 operation->sequence == PSA_PAKE_X1_STEP_KEY_SHARE )
        {
            ret = mbedtls_ecjpake_write_round_two( &operation->ctx.ecjpake,
                                                   operation->buffer,
                                                   PSA_PAKE_BUFFER_SIZE,
                                                   &operation->buffer_length,
                                                   mbedtls_psa_get_random,
                                                   MBEDTLS_PSA_RANDOM_STATE );
            if( ret != 0 )
            {
                psa_pake_abort( operation );
                return( mbedtls_ecjpake_to_psa_error( ret ) );
            }

            operation->buffer_offset = 0;
        }

        /*
         * Steps sequences are stored as:
         * struct {
         *     opaque point <1..2^8-1>;
         * } ECPoint;
         *
         * Where byte 0 stores the ECPoint curve point length.
         *
         * The sequence length is equal to:
         * - data length extracted from byte 0
         * - byte 0 size (1)
         */
        if( operation->state == PSA_PAKE_OUTPUT_X2S &&
            operation->sequence == PSA_PAKE_X1_STEP_KEY_SHARE )
        {
            if( operation->role == PSA_PAKE_ROLE_SERVER )
                /*
                 * The X2S KEY SHARE Server steps sequence is stored as:
                 * struct {
                 *     ECPoint X;
                 *    opaque r <1..2^8-1>;
                 * } ECSchnorrZKP;
                 *
                 * And MbedTLS uses a 3 bytes Ephemeral public key ECPoint,
                 * so byte 3 stores the r Schnorr signature length.
                 *
                 * The sequence length is equal to:
                 * - curve storage size (3)
                 * - data length extracted from byte 3
                 * - byte 3 size (1)
                 */
                length = 3 + operation->buffer[3] + 1;
            else
                length = operation->buffer[0] + 1;
        }
        else
            length = operation->buffer[operation->buffer_offset] + 1;

        if( length > operation->buffer_length )
            return( PSA_ERROR_DATA_CORRUPT );

        if( output_size < length )
        {
            psa_pake_abort( operation );
            return( PSA_ERROR_BUFFER_TOO_SMALL );
        }

        memcpy( output,
                operation->buffer +  operation->buffer_offset,
                length );
        *output_length = length;

        operation->buffer_offset += length;

        /* Reset buffer after ZK_PROOF sequence */
        if( ( operation->state == PSA_PAKE_OUTPUT_X1_X2 &&
              operation->sequence == PSA_PAKE_X2_STEP_ZK_PROOF ) ||
            ( operation->state == PSA_PAKE_OUTPUT_X2S &&
              operation->sequence == PSA_PAKE_X1_STEP_ZK_PROOF ) )
        {
            mbedtls_platform_zeroize( operation->buffer, PSA_PAKE_BUFFER_SIZE );
            operation->buffer_length = 0;
            operation->buffer_offset = 0;

            operation->state = PSA_PAKE_STATE_READY;
            operation->output_step++;
            operation->sequence = PSA_PAKE_SEQ_INVALID;
        }
        else
            operation->sequence++;

        return( PSA_SUCCESS );
    }
    else
#endif
    return( PSA_ERROR_NOT_SUPPORTED );
}

psa_status_t psa_pake_input( psa_pake_operation_t *operation,
                             psa_pake_step_t step,
                             const uint8_t *input,
                             size_t input_length )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    psa_status_t status = PSA_ERROR_CORRUPTION_DETECTED;
    size_t buffer_remain;

    if( operation->alg == PSA_ALG_NONE ||
        operation->state == PSA_PAKE_STATE_INVALID )
        return( PSA_ERROR_BAD_STATE );

    if( input == NULL || input_length == 0 )
        return( PSA_ERROR_INVALID_ARGUMENT );

#if defined(MBEDTLS_PSA_BUILTIN_ALG_JPAKE)
    /*
     * The PSA CRYPTO PAKE and MbedTLS JPAKE API have a different
     * handling of input sequencing.
     *
     * The MbedTLS JPAKE API takes the whole X1+X2 or X4S steps data
     * at once as input, on the other side the PSA CRYPTO PAKE api requires
     * the KEY_SHARE/ZP_PUBLIC/ZK_PROOF parts of X1, X2 & X4S to be
     * given in sequence.
     *
     * In order to achieve API compatibility, each X1+X2 or X4S step data
     * is stored sequentially in an intermediate buffer and given to the
     * MbedTLS JPAKE API on the last step.
     *
     * This causes any input error to be only detected on the last step.
     */
    if( operation->alg == PSA_ALG_JPAKE )
    {
        if( step != PSA_PAKE_STEP_KEY_SHARE &&
            step != PSA_PAKE_STEP_ZK_PUBLIC &&
            step != PSA_PAKE_STEP_ZK_PROOF )
            return( PSA_ERROR_INVALID_ARGUMENT );

        if( operation->state == PSA_PAKE_STATE_SETUP )
        {
            status = psa_pake_ecjpake_setup( operation );
            if( status != PSA_SUCCESS )
            {
                psa_pake_abort( operation );
                return( status );
            }
        }

        if( operation->state != PSA_PAKE_STATE_READY &&
            operation->state != PSA_PAKE_INPUT_X1_X2 &&
            operation->state != PSA_PAKE_INPUT_X4S )
        {
            return( PSA_ERROR_BAD_STATE );
        }

        if( operation->state == PSA_PAKE_STATE_READY )
        {
            if( step != PSA_PAKE_STEP_KEY_SHARE )
                return( PSA_ERROR_BAD_STATE );

            switch( operation->input_step )
            {
                case PSA_PAKE_STEP_X1_X2:
                    operation->state = PSA_PAKE_INPUT_X1_X2;
                    break;
                case PSA_PAKE_STEP_X2S:
                    operation->state = PSA_PAKE_INPUT_X4S;
                    break;
                default:
                    return( PSA_ERROR_BAD_STATE );
            }

            operation->sequence = PSA_PAKE_X1_STEP_KEY_SHARE;
        }

        buffer_remain = PSA_PAKE_BUFFER_SIZE - operation->buffer_length;

        if( input_length == 0 ||
            input_length > buffer_remain )
        {
            psa_pake_abort( operation );
            return( PSA_ERROR_INSUFFICIENT_MEMORY );
        }

        /* Check if step matches current sequence */
        switch( operation->sequence )
        {
            case PSA_PAKE_X1_STEP_KEY_SHARE:
            case PSA_PAKE_X2_STEP_KEY_SHARE:
                if( step != PSA_PAKE_STEP_KEY_SHARE )
                    return( PSA_ERROR_BAD_STATE );
                break;

            case PSA_PAKE_X1_STEP_ZK_PUBLIC:
            case PSA_PAKE_X2_STEP_ZK_PUBLIC:
                if( step != PSA_PAKE_STEP_ZK_PUBLIC )
                    return( PSA_ERROR_BAD_STATE );
                break;

            case PSA_PAKE_X1_STEP_ZK_PROOF:
            case PSA_PAKE_X2_STEP_ZK_PROOF:
                if( step != PSA_PAKE_STEP_ZK_PROOF )
                    return( PSA_ERROR_BAD_STATE );
                break;

            default:
                return( PSA_ERROR_BAD_STATE );
        }

        /* Copy input to local buffer */
        memcpy( operation->buffer + operation->buffer_length,
                input, input_length );
        operation->buffer_length += input_length;

        /* Load buffer at each last round ZK_PROOF */
        if( operation->state == PSA_PAKE_INPUT_X1_X2 &&
            operation->sequence == PSA_PAKE_X2_STEP_ZK_PROOF )
        {
            ret = mbedtls_ecjpake_read_round_one( &operation->ctx.ecjpake,
                                                  operation->buffer,
                                                  operation->buffer_length );

            mbedtls_platform_zeroize( operation->buffer, PSA_PAKE_BUFFER_SIZE );
            operation->buffer_length = 0;

            if( ret != 0 )
            {
                psa_pake_abort( operation );
                return( mbedtls_ecjpake_to_psa_error( ret ) );
            }
        }
        else if( operation->state == PSA_PAKE_INPUT_X4S &&
                 operation->sequence == PSA_PAKE_X1_STEP_ZK_PROOF )
        {
            ret = mbedtls_ecjpake_read_round_two( &operation->ctx.ecjpake,
                                                  operation->buffer,
                                                  operation->buffer_length );

            mbedtls_platform_zeroize( operation->buffer, PSA_PAKE_BUFFER_SIZE );
            operation->buffer_length = 0;

            if( ret != 0 )
            {
                psa_pake_abort( operation );
                return( mbedtls_ecjpake_to_psa_error( ret ) );
            }
        }

        if( ( operation->state == PSA_PAKE_INPUT_X1_X2 &&
              operation->sequence == PSA_PAKE_X2_STEP_ZK_PROOF ) ||
            ( operation->state == PSA_PAKE_INPUT_X4S &&
              operation->sequence == PSA_PAKE_X1_STEP_ZK_PROOF ) )
        {
            operation->state = PSA_PAKE_STATE_READY;
            operation->input_step++;
            operation->sequence = PSA_PAKE_SEQ_INVALID;
        }
        else
            operation->sequence++;

        return( PSA_SUCCESS );
    }
    else
#endif
    return( PSA_ERROR_NOT_SUPPORTED );
}

psa_status_t psa_pake_get_implicit_key(psa_pake_operation_t *operation,
                                       psa_key_derivation_operation_t *output)
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    psa_status_t status = PSA_ERROR_CORRUPTION_DETECTED;

    if( operation->alg == PSA_ALG_NONE ||
        operation->state != PSA_PAKE_STATE_READY ||
        operation->input_step != PSA_PAKE_STEP_DERIVE ||
        operation->output_step != PSA_PAKE_STEP_DERIVE )
        return( PSA_ERROR_BAD_STATE );

#if defined(MBEDTLS_PSA_BUILTIN_ALG_JPAKE)
    if( operation->alg == PSA_ALG_JPAKE )
    {
        ret = mbedtls_ecjpake_write_shared_key( &operation->ctx.ecjpake,
                                                operation->buffer,
                                                PSA_PAKE_BUFFER_SIZE,
                                                &operation->buffer_length,
                                                mbedtls_psa_get_random,
                                                MBEDTLS_PSA_RANDOM_STATE );
        if( ret != 0)
        {
            psa_pake_abort( operation );
            return( mbedtls_ecjpake_to_psa_error( ret ) );
        }

        status = psa_key_derivation_input_bytes( output,
                                                 PSA_KEY_DERIVATION_INPUT_SECRET,
                                                 operation->buffer,
                                                 operation->buffer_length );

        mbedtls_platform_zeroize( operation->buffer, PSA_PAKE_BUFFER_SIZE );

        psa_pake_abort( operation );

        return( status );
    }
    else
#endif
    return( PSA_ERROR_NOT_SUPPORTED );
}

psa_status_t psa_pake_abort(psa_pake_operation_t * operation)
{
    if( operation->alg == PSA_ALG_NONE )
    {
        return( PSA_SUCCESS );
    }

#if defined(MBEDTLS_PSA_BUILTIN_ALG_JPAKE)
    if( operation->alg == PSA_ALG_JPAKE )
    {
        operation->input_step = PSA_PAKE_STEP_INVALID;
        operation->output_step = PSA_PAKE_STEP_INVALID;
        operation->password = MBEDTLS_SVC_KEY_ID_INIT;
        operation->role = PSA_PAKE_ROLE_NONE;
        mbedtls_platform_zeroize( operation->buffer, PSA_PAKE_BUFFER_SIZE );
        operation->buffer_length = 0;
        operation->buffer_offset = 0;
        mbedtls_ecjpake_free( &operation->ctx.ecjpake );
    }
#endif

    operation->alg = PSA_ALG_NONE;
    operation->state = PSA_PAKE_STATE_INVALID;
    operation->sequence = PSA_PAKE_SEQ_INVALID;

    return( PSA_SUCCESS );
}

#endif /* MBEDTLS_PSA_BUILTIN_PAKE */

#endif /* MBEDTLS_PSA_CRYPTO_C */
