# components.sh
#
# Copyright The Mbed TLS Contributors
# SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later

# This file contains the test components that are executed by all.sh

# The functions below are named as follows:
#  * component_XXX: independent components. They can be run in any order.
#      * component_check_XXX: quick tests that aren't worth parallelizing.
#      * component_build_XXX: build things but don't run them.
#      * component_test_XXX: build and test.
#      * component_release_XXX: tests that the CI should skip during PR testing.
#  * support_XXX: if support_XXX exists and returns false then
#    component_XXX is not run by default.

# Each component must start by invoking `msg` with a short informative message.
#
# Warning: due to the way bash detects errors, the failure of a command
# inside 'if' or '!' is not detected. Use the 'not' function instead of '!'.
#
# Each component is executed in a separate shell process. The component
# fails if any command in it returns a non-zero status.
#
# The framework in all.sh performs some cleanup tasks after each component.
# This means that components can assume that the working directory is in a
# cleaned-up state, and don't need to perform the cleanup themselves.
# * Run `make clean`.
# * Restore `include/mbedtls/mbedtls_config.h` from a backup made before running
#   the component.
# * Check out `Makefile`, `library/Makefile`, `programs/Makefile`,
#   `tests/Makefile` and `programs/fuzz/Makefile` from git.
#   This cleans up after an in-tree use of CMake.
#
# The tests are roughly in order from fastest to slowest. This doesn't
# have to be exact, but in general you should add slower tests towards
# the end and fast checks near the beginning.


################################################################
#### Build and test many configurations and targets
################################################################

component_test_no_rsa_key_pair_generation() {
    msg "build: default config minus PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_GENERATE"
    scripts/config.py set MBEDTLS_PSA_CRYPTO_CONFIG
    scripts/config.py unset MBEDTLS_GENPRIME
    scripts/config.py -f $CRYPTO_CONFIG_H unset PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_GENERATE
    make

    msg "test: default config minus PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_GENERATE"
    make test
}

component_test_full_no_cipher () {
    msg "build: full no CIPHER"

    scripts/config.py full
    scripts/config.py unset MBEDTLS_CIPHER_C

    # The built-in implementation of the following algs/key-types depends
    # on CIPHER_C so we disable them.
    # This does not hold for KEY_TYPE_CHACHA20 and ALG_CHACHA20_POLY1305
    # so we keep them enabled.
    scripts/config.py -f $CRYPTO_CONFIG_H unset PSA_WANT_ALG_CCM_STAR_NO_TAG
    scripts/config.py -f $CRYPTO_CONFIG_H unset PSA_WANT_ALG_CMAC
    scripts/config.py -f $CRYPTO_CONFIG_H unset PSA_WANT_ALG_CBC_NO_PADDING
    scripts/config.py -f $CRYPTO_CONFIG_H unset PSA_WANT_ALG_CBC_PKCS7
    scripts/config.py -f $CRYPTO_CONFIG_H unset PSA_WANT_ALG_CFB
    scripts/config.py -f $CRYPTO_CONFIG_H unset PSA_WANT_ALG_CTR
    scripts/config.py -f $CRYPTO_CONFIG_H unset PSA_WANT_ALG_ECB_NO_PADDING
    scripts/config.py -f $CRYPTO_CONFIG_H unset PSA_WANT_ALG_OFB
    scripts/config.py -f $CRYPTO_CONFIG_H unset PSA_WANT_ALG_PBKDF2_AES_CMAC_PRF_128
    scripts/config.py -f $CRYPTO_CONFIG_H unset PSA_WANT_ALG_STREAM_CIPHER
    scripts/config.py -f $CRYPTO_CONFIG_H unset PSA_WANT_KEY_TYPE_DES

    # The following modules directly depends on CIPHER_C
    scripts/config.py unset MBEDTLS_CMAC_C
    scripts/config.py unset MBEDTLS_NIST_KW_C

    make

    # Ensure that CIPHER_C was not re-enabled
    not grep mbedtls_cipher_init ${BUILTIN_SRC_PATH}/cipher.o

    msg "test: full no CIPHER"
    make test
}
