// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/dif/dif_otbn.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/lib/testing/otbn_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

/**
 * End-to-end RSA encryption and decryption test using OTBN.
 *
 * IMPORTANT: This test is not a secure, complete, or reusable implementation of
 * RSA; it is not even close to being production-ready. It is only meant as an
 * end-to-end test for OTBN during the bringup phase.
 *
 * This test loads the RSA application into OTBN, sets all required input
 * arguments, and performs the encryption and decryption operations.
 *
 * To keep the test execution time reasonable some parts of the test can be
 * disabled with the `kTestDecrypt` and `kTestRsaGreater1k` constants.
 *
 * The test contains constants and expected output, which can be independently
 * and conveniently verified using a Python script.
 *
 * <code>
 * # Optional: generate a new key
 * $ openssl genpkey -algorithm RSA -out otbn_rsa_test_private_key_1024.pem \
 *     -pkeyopt rsa_keygen_bits:1024
 *
 * # Create all constants/variables
 * $ ./otbn_test_params.py rsa otbn_rsa_test_private_key_1024.pem \
 *     "Hello OTBN, can you encrypt and decrypt this for me?"
 * </code>
 */

/**
 * Performs encrypt and decrypt operations. (Otherwise, only encrypt is done.)
 *
 * Encryption is reasonably fast in RSA, taking between 50 thousand instructions
 * for RSA 512, up to 17 million instructions for RSA 4096.
 * Assuming a typical Verilator-based simulation of Earl Grey at 5 kHz, this
 * results in simulation times between a couple of seconds and an hour for RSA
 * encrypt.
 *
 * Decryption, on the other hand, is much more computationally expensive.
 * Decryption takes around 0.5 million instructions for RSA 512, up to
 * 128 million instructions for RSA 4096. Assuming the same simulation at 5 kHz,
 * this translates to a RSA 512 decode taking around two minutes to execute,
 * and a RSA 4096 decode taking over seven hours.
 */
static const bool kTestDecrypt = true;

/**
 * Tests RSA 2k, 3k, and 4k.
 *
 * If disabled, only RSA 512 and RSA 1k tests are executed. Longer keys take
 * significantly longer to test, especially the decryption parts.
 *
 * See the description of `kTestDecrypt` for more details on execution times.
 */
static const bool kTestRsaGreater1k = false;

OTBN_DECLARE_APP_SYMBOLS(rsa);
OTBN_DECLARE_SYMBOL_ADDR(rsa, mode);
OTBN_DECLARE_SYMBOL_ADDR(rsa, n_limbs);
OTBN_DECLARE_SYMBOL_ADDR(rsa, inout);
OTBN_DECLARE_SYMBOL_ADDR(rsa, modulus);
OTBN_DECLARE_SYMBOL_ADDR(rsa, exp);

static const otbn_app_t kOtbnAppRsa = OTBN_APP_T_INIT(rsa);
static const otbn_addr_t kOtbnVarRsaMode = OTBN_ADDR_T_INIT(rsa, mode);
static const otbn_addr_t kOtbnVarRsaNLimbs = OTBN_ADDR_T_INIT(rsa, n_limbs);
static const otbn_addr_t kOtbnVarRsaInOut = OTBN_ADDR_T_INIT(rsa, inout);
static const otbn_addr_t kOtbnVarRsaModulus = OTBN_ADDR_T_INIT(rsa, modulus);
static const otbn_addr_t kOtbnVarRsaExp = OTBN_ADDR_T_INIT(rsa, exp);

enum {
  kRsa512SizeBytes = 512 / 8,
  kRsa1024SizeBytes = 1024 / 8,
  kRsa2048SizeBytes = 2048 / 8,
  kRsa3072SizeBytes = 3072 / 8,
  kRsa4096SizeBytes = 4096 / 8,
};

OTTF_DEFINE_TEST_CONFIG();

/**
 * Encrypts a message with RSA.
 *
 * @param otbn The OTBN context object.
 * @param modulus The modulus (n).
 * @param in The plaintext message.
 * @param out The encrypted message.
 * @param size_bytes The size of all buffers in bytes, i.e. the key/modulus
 *                   length (i.e. 128 for RSA 1024). Valid range: 32..512 in
 *                   32 byte-steps (i.e. RSA 256 to RSA 4096).
 */
static void rsa_encrypt(dif_otbn_t *otbn, const uint8_t *modulus,
                        const uint8_t *in, uint8_t *out, size_t size_bytes) {
  CHECK(otbn != NULL);
  CHECK(size_bytes % 32 == 0);

  uint32_t n_limbs = size_bytes / 32;
  CHECK(n_limbs != 0 && n_limbs <= 16);

  // Write input arguments.
  uint32_t mode = 1;  // mode 1 => encrypt
  CHECK_STATUS_OK(otbn_testutils_write_data(otbn, sizeof(uint32_t), &mode,
                                            kOtbnVarRsaMode));
  CHECK_STATUS_OK(otbn_testutils_write_data(otbn, sizeof(uint32_t), &n_limbs,
                                            kOtbnVarRsaNLimbs));
  CHECK_STATUS_OK(
      otbn_testutils_write_data(otbn, size_bytes, modulus, kOtbnVarRsaModulus));
  CHECK_STATUS_OK(
      otbn_testutils_write_data(otbn, size_bytes, in, kOtbnVarRsaInOut));

  // Call OTBN to perform operation, and wait for it to complete.
  CHECK_STATUS_OK(otbn_testutils_execute(otbn));
  CHECK_STATUS_OK(otbn_testutils_wait_for_done(otbn, kDifOtbnErrBitsNoError));

  // Read back results.
  CHECK_STATUS_OK(
      otbn_testutils_read_data(otbn, size_bytes, kOtbnVarRsaInOut, out));
}

/**
 * Decrypts a message with RSA.
 *
 * @param otbn The OTBN context object.
 * @param modulus The modulus (n).
 * @param private_exponent The private exponent (d).
 * @param in The encrypted message.
 * @param out The decrypted (plaintext) message.
 * @param size_bytes The size of all buffers in bytes, i.e. the key/modulus
 *                   length (i.e. 128 for RSA 1024). Valid range: 32..512 in
 *                   32 byte-steps (i.e. RSA 256 to RSA 4096).
 */
static void rsa_decrypt(dif_otbn_t *otbn, const uint8_t *modulus,
                        const uint8_t *private_exponent, const uint8_t *in,
                        uint8_t *out, size_t size_bytes) {
  CHECK(otbn != NULL);
  CHECK(size_bytes % 32 == 0);

  // Limbs are 256b words.
  uint32_t n_limbs = size_bytes / 32;
  CHECK(n_limbs != 0 && n_limbs <= 16);

  // Write input arguments.
  uint32_t mode = 2;  // mode 2 => decrypt
  CHECK_STATUS_OK(
      otbn_testutils_write_data(otbn, sizeof(mode), &mode, kOtbnVarRsaMode));
  CHECK_STATUS_OK(otbn_testutils_write_data(otbn, sizeof(n_limbs), &n_limbs,
                                            kOtbnVarRsaNLimbs));
  CHECK_STATUS_OK(
      otbn_testutils_write_data(otbn, size_bytes, modulus, kOtbnVarRsaModulus));
  CHECK_STATUS_OK(otbn_testutils_write_data(otbn, size_bytes, private_exponent,
                                            kOtbnVarRsaExp));
  CHECK_STATUS_OK(
      otbn_testutils_write_data(otbn, size_bytes, in, kOtbnVarRsaInOut));

  // Call OTBN to perform operation
  CHECK_STATUS_OK(otbn_testutils_execute(otbn));
  CHECK_STATUS_OK(otbn_testutils_wait_for_done(otbn, kDifOtbnErrBitsNoError));

  // Read back results.
  CHECK_STATUS_OK(
      otbn_testutils_read_data(otbn, size_bytes, kOtbnVarRsaInOut, out));
}

/**
 * CHECK()s that the actual data matches the expected data.
 *
 * @param actual The actual data.
 * @param expected The expected data.
 * @param size_bytes The size of the actual/expected data.
 */
static void check_data(const uint8_t *actual, const uint8_t *expected,
                       size_t size_bytes) {
  for (int i = 0; i < size_bytes; ++i) {
    CHECK(actual[i] == expected[i],
          "Data mismatch at byte %d: 0x%x (actual) != 0x%x (expected)", i,
          actual[i], expected[i]);
  }
}

static uint64_t t_start;
static uint64_t t_end;

/**
 * Starts a profiling section.
 *
 * Call this function at the start of a section that should be profiled, and
 * call `profile_end()` at the end of it to display the results.
 *
 * Profiling section may not overlap, i.e. `profile_start()`/`profile_end()`
 * pairs may not be nested.
 */
static void profile_start(void) { t_start = ibex_mcycle_read(); }

/**
 * Ends a profiling section.
 *
 * The time since `profile_start()` is printed as log message.
 *
 * @param msg Name of the operation (for logging purposes).
 */
static void profile_end(const char *msg) {
  t_end = ibex_mcycle_read();
  uint32_t cycles = t_end - t_start;
  uint32_t time_us = cycles / 100;
  LOG_INFO("%s took %u cycles or %u us @ 100 MHz.", msg, cycles, time_us);
}

/**
 * Performs a RSA roundtrip test.
 *
 * A roundtrip consists of three steps:
 * - Initialize OTBN.
 * - Encrypt data. Check that the OTBN-produced data matches
 *   `encrypted_expected`.
 * - If `kTestDecrypt` is set: Decrypt the encrypted data. Check that the OTBN-
 *   produced plaintext matches the original plaintext in `in`.
 *
 * @param size_bytes Size of all data arguments/buffers, in bytes.
 * @param modulus The modulus (n).
 * @param private_exponent The private exponent (d).
 * @param in The input data of size `size_bytes`.
 * @param encrypted_expected The encrypted version of `in`.
 * @param out_encrypted Buffer to hold the encrypted data, as produced by OTBN.
 * @param out_decrypted Buffer to hold the decrypted data, as produced by OTBN.
 */
static void rsa_roundtrip(uint32_t size_bytes, const uint8_t *modulus,
                          const uint8_t *private_exponent, const uint8_t *in,
                          const uint8_t *encrypted_expected,
                          uint8_t *out_encrypted, uint8_t *out_decrypted) {
  dif_otbn_t otbn;

  // Initialize
  profile_start();
  CHECK_DIF_OK(
      dif_otbn_init(mmio_region_from_addr(TOP_EARLGREY_OTBN_BASE_ADDR), &otbn));
  CHECK_STATUS_OK(otbn_testutils_load_app(&otbn, kOtbnAppRsa));
  profile_end("Initialization");

  // Encrypt
  LOG_INFO("Encrypting");
  profile_start();
  rsa_encrypt(&otbn, modulus, in, out_encrypted, size_bytes);
  check_data(out_encrypted, encrypted_expected, size_bytes);
  profile_end("Encryption");

  if (kTestDecrypt) {
    // Decrypt
    LOG_INFO("Decrypting");
    profile_start();
    rsa_decrypt(&otbn, modulus, private_exponent, encrypted_expected,
                out_decrypted, size_bytes);
    check_data(out_decrypted, in, size_bytes);
    profile_end("Decryption");
  }
}

static void test_rsa512_roundtrip(void) {
  static const uint8_t kModulus[kRsa512SizeBytes] = {
      0xf3, 0xb7, 0x91, 0xce, 0x6e, 0xc0, 0x57, 0xcd, 0x19, 0x63, 0xb9,
      0x6b, 0x81, 0x97, 0x96, 0x40, 0x81, 0xd8, 0x89, 0x27, 0xec, 0x0a,
      0xb1, 0xf2, 0x4a, 0xda, 0x2e, 0x68, 0xe1, 0x80, 0xa4, 0x4f, 0xe0,
      0x82, 0x87, 0x1f, 0x98, 0xdc, 0x42, 0xc7, 0xc2, 0xce, 0xa2, 0xb2,
      0x1a, 0x3f, 0x77, 0xdc, 0xc6, 0x27, 0x6d, 0x83, 0x5c, 0xcd, 0x1d,
      0xdf, 0xe5, 0xf3, 0x98, 0xe6, 0x8b, 0xe6, 0x5b, 0xd4};

  static const uint8_t kPrivateExponent[kRsa512SizeBytes] = {
      0xc1, 0xf3, 0x5d, 0x18, 0x12, 0x7e, 0xe7, 0x0c, 0xbf, 0x33, 0xd0,
      0x1c, 0xd8, 0x5d, 0x91, 0x26, 0xb6, 0xc5, 0xae, 0x78, 0xda, 0x4c,
      0xae, 0x43, 0xa1, 0x57, 0xab, 0x32, 0xcf, 0xa4, 0xd4, 0x72, 0x20,
      0x53, 0x30, 0x55, 0x7a, 0x93, 0xd9, 0xae, 0x75, 0x32, 0x9d, 0x09,
      0x18, 0x06, 0xc8, 0x26, 0x64, 0x28, 0xcf, 0x2c, 0x3b, 0x6e, 0x6b,
      0x5c, 0x28, 0xde, 0x76, 0x6c, 0x2f, 0xcc, 0xf3, 0x31};

  static const uint8_t kEncryptedExpected[kRsa512SizeBytes] = {
      0xb7, 0x02, 0x28, 0xcb, 0x63, 0x5e, 0xa6, 0xfd, 0x55, 0x4a, 0x85,
      0x43, 0x1d, 0x26, 0x13, 0xb3, 0x78, 0x66, 0xd9, 0xe2, 0xe1, 0xbf,
      0x29, 0xc6, 0xc6, 0xdd, 0x90, 0x76, 0x3f, 0x1d, 0x43, 0xc0, 0x76,
      0x51, 0x75, 0x10, 0x66, 0x61, 0x8c, 0x3c, 0x99, 0xd9, 0x90, 0xd2,
      0x59, 0x45, 0x0a, 0x7a, 0x6d, 0x58, 0xaa, 0x75, 0xf2, 0x63, 0xb3,
      0xe1, 0x06, 0x4b, 0x82, 0x0a, 0xdd, 0x07, 0x44, 0x2a};

  static const uint8_t kIn[kRsa512SizeBytes] = {
      "Hello OTBN, can you encrypt and decrypt this for me?"};
  uint8_t out_encrypted[kRsa512SizeBytes] = {0};
  uint8_t out_decrypted[kRsa512SizeBytes] = {0};

  LOG_INFO("Running RSA512 test");
  rsa_roundtrip(kRsa512SizeBytes, kModulus, kPrivateExponent, kIn,
                kEncryptedExpected, out_encrypted, out_decrypted);
}

static void test_rsa1024_roundtrip(void) {
  static const uint8_t kModulus[kRsa1024SizeBytes] = {
      0x69, 0xef, 0x70, 0x5d, 0xcd, 0xf5, 0x15, 0xb5, 0x6b, 0xa2, 0xcd, 0x2b,
      0x76, 0x3c, 0x6e, 0xdc, 0x13, 0x7,  0x6a, 0x9,  0x80, 0xe2, 0x2a, 0x24,
      0xc2, 0xb0, 0x32, 0x36, 0x67, 0x1b, 0x1d, 0xf2, 0xaa, 0xf9, 0xd4, 0xeb,
      0xc6, 0xf0, 0x3c, 0xe5, 0x94, 0x85, 0xd9, 0xc8, 0xa4, 0x79, 0x35, 0x77,
      0x38, 0x10, 0x1,  0x74, 0xc3, 0xd7, 0x6b, 0x10, 0xc2, 0xc1, 0x5d, 0xa0,
      0x57, 0x11, 0xd8, 0xc7, 0xd9, 0xdf, 0x78, 0xc5, 0xc3, 0x9,  0x84, 0x4d,
      0x28, 0x6c, 0xea, 0x55, 0x87, 0x35, 0x44, 0x85, 0xde, 0x70, 0xa8, 0xec,
      0x60, 0x3b, 0x7c, 0x5,  0x12, 0xb5, 0xb3, 0xbd, 0x75, 0x4,  0x40, 0x2b,
      0x6a, 0x35, 0x4,  0x21, 0x73, 0x5,  0x94, 0x8,  0x8,  0x2c, 0xe9, 0xb4,
      0x8c, 0xd,  0x7c, 0x76, 0xc5, 0x85, 0xa7, 0xa,  0xa1, 0x91, 0xe0, 0xad,
      0xae, 0xfa, 0xb,  0xc5, 0xc4, 0x88, 0x7e, 0xbe};

  static const uint8_t kPrivateExponent[kRsa1024SizeBytes] = {
      0x1,  0x66, 0x7f, 0x2,  0xdb, 0x27, 0x92, 0x7d, 0xd6, 0x41, 0x4e, 0xbf,
      0x47, 0x31, 0x95, 0x8e, 0xfb, 0x5d, 0xee, 0xa1, 0xdf, 0x6d, 0x31, 0xd2,
      0xeb, 0xee, 0xe2, 0xf4, 0xa4, 0x21, 0xa9, 0xb9, 0xd2, 0xcf, 0x94, 0xfe,
      0x13, 0x74, 0xc8, 0xc8, 0xc1, 0x38, 0x6f, 0xb0, 0x84, 0x9c, 0x57, 0x1,
      0x58, 0x91, 0xd6, 0x4,  0x4b, 0x9d, 0x49, 0x3,  0x6,  0x2e, 0x5c, 0xb1,
      0xe2, 0xb7, 0x66, 0x0,  0xf7, 0xad, 0xbb, 0xce, 0xc,  0x46, 0xa5, 0xeb,
      0xd9, 0x32, 0xc2, 0xf8, 0xca, 0xe7, 0xf1, 0xae, 0x8,  0x77, 0xce, 0xc4,
      0xa0, 0xa0, 0xdc, 0xef, 0x6d, 0x4c, 0xd7, 0xf0, 0x7a, 0x66, 0xf3, 0x2f,
      0xd5, 0x54, 0xde, 0xa8, 0xe5, 0xfb, 0xa9, 0xa2, 0x36, 0x21, 0xae, 0xff,
      0xd,  0xfa, 0xba, 0x6b, 0xfd, 0xa3, 0x6a, 0x84, 0xa4, 0x8b, 0x95, 0xd6,
      0xac, 0x5d, 0x4e, 0x2e, 0x7b, 0x14, 0x1f, 0x3d};

  static const uint8_t kEncryptedExpected[kRsa1024SizeBytes] = {
      0x76, 0x71, 0x99, 0x16, 0x38, 0x3a, 0xe0, 0xca, 0x9e, 0xc4, 0x5e, 0x9b,
      0x68, 0xb6, 0x3f, 0x78, 0x0d, 0x6e, 0x43, 0x7c, 0xaf, 0x24, 0xcc, 0x3e,
      0x4a, 0xd0, 0x3c, 0x15, 0xc6, 0x10, 0xf8, 0x3a, 0x1a, 0x6e, 0xe8, 0x8f,
      0x9e, 0x6b, 0xdb, 0x3d, 0xd3, 0x48, 0x51, 0x20, 0x8a, 0xb9, 0x36, 0xfb,
      0x9c, 0x2a, 0xd9, 0xef, 0xfc, 0x24, 0x7f, 0xb7, 0x81, 0x7d, 0x81, 0xb2,
      0x6f, 0xd0, 0x1e, 0xdd, 0x5c, 0x70, 0x1b, 0x79, 0x3b, 0x67, 0xe5, 0xfa,
      0xaf, 0x2e, 0xf3, 0xb2, 0xc6, 0xb1, 0xb9, 0x6d, 0x18, 0x79, 0x1a, 0xed,
      0x29, 0xfd, 0xf5, 0x27, 0x8c, 0xf2, 0x6e, 0xe4, 0x48, 0x88, 0xaf, 0x75,
      0xf5, 0xed, 0x09, 0xe7, 0x92, 0xbb, 0x30, 0x97, 0x1e, 0x45, 0x68, 0x81,
      0x6d, 0x69, 0x75, 0xcb, 0xbb, 0xbc, 0xc2, 0x51, 0x6e, 0xb8, 0xc9, 0x46,
      0x57, 0xe5, 0x27, 0xf7, 0x21, 0xb8, 0xd7, 0x2f};

  static const uint8_t kIn[kRsa1024SizeBytes] = {
      "Hello OTBN, can you encrypt and decrypt this for me?"};
  uint8_t out_encrypted[kRsa1024SizeBytes] = {0};
  uint8_t out_decrypted[kRsa1024SizeBytes] = {0};

  LOG_INFO("Running RSA1024 test");
  rsa_roundtrip(kRsa1024SizeBytes, kModulus, kPrivateExponent, kIn,
                kEncryptedExpected, out_encrypted, out_decrypted);
}

static void test_rsa2048_roundtrip(void) {
  static const uint8_t kModulus[kRsa2048SizeBytes] = {
      0xf9, 0x90, 0xc7, 0x94, 0xcf, 0x96, 0xd3, 0x12, 0x6f, 0x16, 0xa6, 0x50,
      0x5d, 0xcb, 0xe9, 0x29, 0x53, 0xc8, 0x44, 0x04, 0xda, 0x69, 0x2d, 0x1a,
      0xc1, 0xb8, 0xa8, 0x70, 0x97, 0xb5, 0x96, 0xd8, 0x07, 0xef, 0x2c, 0x3a,
      0x66, 0x90, 0x16, 0xf9, 0x27, 0x1e, 0xf9, 0x82, 0x2b, 0x32, 0x31, 0x17,
      0x9d, 0x3b, 0x2a, 0x86, 0x0f, 0xb8, 0x2b, 0x51, 0xab, 0xd8, 0x79, 0x99,
      0x1e, 0xfe, 0x94, 0x86, 0x68, 0x12, 0xae, 0x20, 0x03, 0x07, 0xc3, 0xb3,
      0x84, 0x23, 0x36, 0x91, 0xe9, 0x26, 0xc8, 0xff, 0xc7, 0xb9, 0x8c, 0x35,
      0xfb, 0xec, 0xd0, 0xb5, 0xde, 0x60, 0xb2, 0xd4, 0x64, 0x3c, 0x60, 0x94,
      0x22, 0x6f, 0xc9, 0x6c, 0x5b, 0x61, 0x13, 0x6e, 0x45, 0x26, 0x4f, 0x48,
      0xc2, 0x1e, 0xe0, 0x16, 0x58, 0x1a, 0x31, 0x69, 0x22, 0x93, 0x10, 0xa0,
      0x3d, 0x26, 0xc3, 0x92, 0xa3, 0xc3, 0x40, 0xd3, 0x33, 0x1d, 0xa3, 0x31,
      0xc7, 0xe1, 0x61, 0xc5, 0xf4, 0xb5, 0x66, 0xc1, 0x31, 0xc6, 0x4f, 0xf6,
      0xa5, 0x2d, 0x1a, 0x73, 0xf4, 0x67, 0x75, 0x88, 0xf4, 0xc8, 0xc4, 0xa1,
      0x3b, 0xab, 0x47, 0xc7, 0x18, 0x5b, 0x8c, 0x47, 0x28, 0x82, 0xba, 0xad,
      0x7f, 0x39, 0x80, 0x04, 0xf5, 0x77, 0x07, 0x08, 0xe5, 0x39, 0xff, 0x8c,
      0x7f, 0xfc, 0x72, 0x41, 0x1a, 0x99, 0x5a, 0x4d, 0xf7, 0xe9, 0x71, 0xf2,
      0x74, 0x6c, 0xc9, 0x11, 0xb1, 0xb8, 0x13, 0x3f, 0x9f, 0x8e, 0x08, 0x12,
      0xa7, 0x5a, 0x40, 0xd0, 0xe3, 0xaa, 0x26, 0x48, 0xb2, 0x6e, 0xa7, 0x39,
      0x08, 0x06, 0x8e, 0x43, 0x74, 0xce, 0x8d, 0xfa, 0x49, 0x10, 0xf9, 0x7b,
      0xd2, 0x4a, 0xa4, 0x2f, 0x93, 0x24, 0x9d, 0x0f, 0xda, 0xd9, 0x2c, 0xd5,
      0x21, 0xc0, 0xc9, 0x61, 0xc3, 0xc6, 0x1f, 0xaf, 0xf4, 0x47, 0x1a, 0xa5,
      0x2d, 0xa9, 0xc5, 0xbd};

  static const uint8_t kPrivateExponent[kRsa2048SizeBytes] = {
      0xfd, 0x82, 0x16, 0x63, 0x74, 0xc0, 0x2a, 0xde, 0x83, 0x69, 0x77, 0x3d,
      0x1a, 0x5c, 0x07, 0xbb, 0x97, 0x34, 0xd4, 0xcb, 0x28, 0xc3, 0x28, 0xc0,
      0x6a, 0x28, 0xaf, 0x05, 0xfd, 0x24, 0x84, 0x76, 0xd6, 0xc5, 0xfa, 0x3f,
      0xc6, 0x02, 0x56, 0x08, 0x2f, 0x75, 0xbe, 0x9c, 0x92, 0x0e, 0x37, 0x8b,
      0x5a, 0x67, 0x4e, 0xe3, 0x90, 0xb3, 0x16, 0x13, 0x4c, 0x75, 0xa2, 0xa4,
      0xeb, 0x10, 0x03, 0x46, 0xc2, 0x09, 0xa9, 0xb5, 0x7a, 0x1e, 0x53, 0xaf,
      0x5c, 0x61, 0xe7, 0x94, 0x6b, 0x1d, 0x06, 0x7d, 0x55, 0xc6, 0xb4, 0xe7,
      0xa6, 0x56, 0x81, 0x59, 0x47, 0xee, 0xdc, 0x30, 0x7d, 0xc6, 0x49, 0xfd,
      0xb8, 0x05, 0xa1, 0xc2, 0x55, 0xb1, 0xf1, 0x68, 0x47, 0xc5, 0x97, 0x9a,
      0x31, 0xf3, 0x03, 0x3e, 0xc7, 0xf3, 0xe8, 0x70, 0xae, 0x80, 0xbd, 0x79,
      0xfd, 0xe6, 0x15, 0x31, 0x12, 0xc6, 0xcb, 0xaa, 0xd1, 0x84, 0x6d, 0x5a,
      0xed, 0x31, 0x4f, 0x17, 0x6b, 0x1b, 0x95, 0x89, 0x91, 0x3f, 0x9f, 0xa7,
      0x2c, 0xec, 0x4a, 0xbe, 0xc2, 0x15, 0x28, 0xc0, 0xb8, 0xf0, 0xec, 0xd8,
      0x80, 0x25, 0xd7, 0x98, 0x84, 0xde, 0x07, 0xb4, 0x67, 0xc0, 0x78, 0x79,
      0x5c, 0x2a, 0x21, 0x87, 0x47, 0x76, 0x84, 0x1d, 0x26, 0x1e, 0x81, 0x6b,
      0x02, 0x5e, 0xe2, 0x05, 0x1c, 0x17, 0xcb, 0xf5, 0xc1, 0x15, 0x00, 0xa4,
      0xd6, 0x1b, 0xf2, 0x5c, 0x21, 0x55, 0xb6, 0x35, 0x24, 0x8e, 0x3d, 0x9c,
      0x5c, 0xf3, 0x0c, 0xb3, 0x64, 0xff, 0x9e, 0x5e, 0x7f, 0xe8, 0x71, 0xe9,
      0x04, 0xf6, 0xdd, 0x54, 0x00, 0x4f, 0x7f, 0x0a, 0x63, 0x64, 0x88, 0xa5,
      0x36, 0x76, 0xc8, 0x0e, 0x2c, 0xd6, 0x39, 0x57, 0x6c, 0x9c, 0xcb, 0x19,
      0x2c, 0x63, 0x73, 0xcb, 0x6b, 0x89, 0x2e, 0xe1, 0xf7, 0xc4, 0x74, 0x61,
      0x67, 0x9a, 0x63, 0x9e};

  static const uint8_t kEncryptedExpected[kRsa2048SizeBytes] = {
      0x54, 0x83, 0x7c, 0xb0, 0xd9, 0x77, 0x76, 0xb5, 0xf5, 0xc8, 0x51, 0x02,
      0x41, 0xab, 0xeb, 0xa6, 0x8e, 0x01, 0x15, 0x54, 0x30, 0x9b, 0x05, 0xb6,
      0xbf, 0x40, 0x3d, 0xd2, 0x95, 0x62, 0xf7, 0x42, 0x4d, 0xf8, 0x3b, 0xd6,
      0x0b, 0x9e, 0xef, 0x27, 0x2f, 0x95, 0x8e, 0x8a, 0xaf, 0x07, 0xe9, 0x54,
      0x66, 0xc0, 0xe9, 0x1c, 0xdd, 0x1b, 0xfb, 0x91, 0xe3, 0xa6, 0x83, 0x6f,
      0xa4, 0x74, 0x49, 0x75, 0x7f, 0x35, 0x8e, 0x40, 0x04, 0x72, 0xb9, 0xe2,
      0x78, 0x4c, 0x4a, 0x3e, 0x37, 0xe9, 0x19, 0xe8, 0x61, 0xf4, 0xaa, 0x7d,
      0x27, 0xd1, 0x55, 0x40, 0x59, 0x5b, 0x3c, 0x88, 0x70, 0x76, 0x09, 0x49,
      0x8c, 0x3c, 0x66, 0xe1, 0x85, 0x8e, 0xe9, 0x79, 0xfe, 0x8f, 0xc0, 0xfd,
      0x40, 0xbf, 0xf3, 0x87, 0xa9, 0x45, 0xb1, 0xce, 0xb2, 0xb8, 0x4b, 0xc2,
      0x60, 0xcd, 0xda, 0xe5, 0x30, 0xf3, 0xd2, 0x38, 0xfd, 0x9d, 0x6e, 0x15,
      0x5f, 0xa3, 0x24, 0x22, 0x90, 0x08, 0x09, 0x2b, 0x2d, 0x6e, 0x15, 0xe0,
      0x97, 0x31, 0x1f, 0x85, 0x47, 0x72, 0x69, 0xf9, 0xd2, 0x5a, 0xcc, 0xe4,
      0x9d, 0x17, 0xf2, 0x81, 0x73, 0x8c, 0x40, 0x61, 0x56, 0x6f, 0xbf, 0xd0,
      0xa5, 0x20, 0xed, 0x37, 0x22, 0x5a, 0xab, 0xb6, 0x8e, 0x12, 0x87, 0x1b,
      0xcd, 0x34, 0xda, 0x79, 0x0d, 0x35, 0x7c, 0xa4, 0xd1, 0xfa, 0x44, 0x09,
      0xb9, 0xf0, 0x0b, 0xb2, 0xfb, 0xd3, 0xf1, 0xfd, 0xd8, 0x2f, 0x30, 0x15,
      0xe2, 0x75, 0x18, 0x90, 0x3b, 0x33, 0xc5, 0x4a, 0x3d, 0x19, 0xd1, 0xb9,
      0x35, 0x59, 0x2d, 0x2a, 0x0a, 0x51, 0xfe, 0xad, 0x03, 0xcd, 0x05, 0x8c,
      0xb6, 0xeb, 0x5f, 0x66, 0xb9, 0x40, 0x1e, 0xd0, 0xce, 0xa5, 0xe1, 0x8e,
      0x47, 0xb7, 0xb7, 0x55, 0x06, 0x92, 0xe5, 0x6f, 0xc9, 0x92, 0xc7, 0x80,
      0x26, 0x2d, 0x3f, 0x2d};

  static const uint8_t kIn[kRsa2048SizeBytes] = {"OTBN is great!"};
  uint8_t out_encrypted[kRsa2048SizeBytes] = {0};
  uint8_t out_decrypted[kRsa2048SizeBytes] = {0};

  LOG_INFO("Running RSA2048 test");
  rsa_roundtrip(kRsa2048SizeBytes, kModulus, kPrivateExponent, kIn,
                kEncryptedExpected, out_encrypted, out_decrypted);
}

static void test_rsa3072_roundtrip(void) {
  static const uint8_t kModulus[kRsa3072SizeBytes] = {
      0x4f, 0x2b, 0xc7, 0xac, 0x81, 0x3a, 0xe8, 0x3e, 0xa4, 0x6c, 0x47, 0x9c,
      0x9e, 0xdc, 0x5b, 0x94, 0x45, 0x0f, 0x14, 0x69, 0x53, 0x20, 0xc2, 0x47,
      0x81, 0x4d, 0x3b, 0x96, 0xb9, 0x34, 0x9f, 0x7b, 0x47, 0x02, 0x8c, 0xe6,
      0x35, 0x47, 0x04, 0x68, 0x9d, 0x07, 0xeb, 0x5e, 0xcb, 0x80, 0x22, 0x75,
      0xd3, 0x42, 0xa1, 0xd8, 0xa2, 0xfe, 0x82, 0x4a, 0xb8, 0x01, 0x50, 0x8e,
      0x06, 0xd4, 0x0f, 0xc4, 0x9a, 0xb9, 0x9d, 0xd6, 0xe8, 0x60, 0x71, 0xfb,
      0xe4, 0xb4, 0x91, 0xd1, 0x4c, 0xf6, 0x00, 0xa1, 0x86, 0x41, 0x14, 0x73,
      0xa3, 0xf5, 0x0f, 0xfb, 0x54, 0x81, 0x63, 0x2b, 0x61, 0xd1, 0x5d, 0x53,
      0xfd, 0x22, 0x71, 0x3f, 0x17, 0xe5, 0x86, 0xd1, 0x9a, 0x13, 0x3d, 0x64,
      0xbb, 0xb5, 0xc2, 0xc8, 0x08, 0xac, 0xa1, 0x68, 0xd6, 0x1c, 0x4d, 0x5a,
      0x24, 0x21, 0x53, 0x03, 0xa2, 0x33, 0xef, 0xb6, 0x94, 0x91, 0x25, 0x11,
      0x72, 0xe2, 0x21, 0x43, 0x9a, 0xee, 0x9f, 0x00, 0xca, 0x67, 0x99, 0x86,
      0x04, 0x0a, 0x4a, 0x05, 0x2c, 0xbe, 0x1b, 0x08, 0xb5, 0x08, 0x8a, 0x43,
      0xfa, 0xcd, 0xb2, 0xac, 0xac, 0x87, 0x32, 0xa7, 0x40, 0x76, 0xea, 0x44,
      0x91, 0x46, 0x7a, 0x9a, 0x21, 0x2e, 0xb6, 0x96, 0x3b, 0xa0, 0xfb, 0x5e,
      0xfd, 0x65, 0xbf, 0x5a, 0x10, 0xbf, 0x4a, 0xd2, 0x1c, 0xff, 0x06, 0x31,
      0xb4, 0xe2, 0x4e, 0x73, 0xc7, 0x3d, 0xe0, 0xf1, 0x99, 0x64, 0x89, 0xca,
      0xf7, 0x02, 0x28, 0x8a, 0x91, 0xfa, 0x68, 0x22, 0x34, 0xe4, 0x89, 0x22,
      0xbd, 0x33, 0xb3, 0xdb, 0xb0, 0xe8, 0x52, 0x98, 0xdb, 0x41, 0x20, 0xb6,
      0xc3, 0xf2, 0xf0, 0x81, 0x2d, 0x3e, 0x30, 0xa3, 0xbf, 0x09, 0xfb, 0xdc,
      0x3f, 0x83, 0xc5, 0x8a, 0x58, 0x3b, 0xf3, 0x5e, 0x52, 0x40, 0x7b, 0xab,
      0x57, 0xd5, 0x8e, 0x0f, 0x46, 0x43, 0xa3, 0x8a, 0x07, 0x6e, 0xed, 0x9c,
      0x60, 0x08, 0x2d, 0x8a, 0x3c, 0xa9, 0x25, 0x2a, 0x78, 0xb1, 0xd5, 0x3a,
      0x85, 0x2d, 0xb2, 0xe6, 0xcc, 0x4d, 0xa1, 0x62, 0xd3, 0x97, 0xe7, 0x86,
      0x04, 0x7a, 0xb8, 0xe5, 0x60, 0xea, 0xb5, 0x5f, 0x6f, 0xb7, 0x15, 0x3c,
      0x4b, 0x54, 0x8d, 0xb1, 0x77, 0x05, 0x51, 0x66, 0x54, 0x4e, 0xbd, 0x6f,
      0xb7, 0x74, 0x98, 0x4d, 0x88, 0xbe, 0xf4, 0xbb, 0x14, 0x13, 0xcc, 0x7b,
      0x4b, 0xe3, 0xf1, 0x52, 0x57, 0x2a, 0x18, 0x35, 0x87, 0x40, 0x28, 0xd5,
      0xc4, 0xb6, 0x62, 0x54, 0xa9, 0xa1, 0xca, 0xd4, 0x87, 0x42, 0x62, 0x5d,
      0x96, 0x58, 0x41, 0x1e, 0x2e, 0x44, 0x34, 0xde, 0x89, 0x04, 0xf3, 0xd6,
      0x89, 0x89, 0x92, 0xc3, 0x67, 0x97, 0x97, 0x0c, 0x26, 0xcd, 0xec, 0x6b,
      0xbd, 0x2e, 0xc3, 0xa4, 0xa5, 0x86, 0xf1, 0xd5, 0x56, 0x9b, 0xb2, 0xac};

  static const uint8_t kPrivateExponent[kRsa3072SizeBytes] = {
      0x01, 0xd7, 0x1b, 0x43, 0xb7, 0x32, 0x49, 0x3b, 0xc0, 0x5c, 0x93, 0x33,
      0xfa, 0x2d, 0xd6, 0x33, 0x70, 0x6f, 0xeb, 0x5b, 0x53, 0xdb, 0x2c, 0x0f,
      0x72, 0x26, 0x6a, 0x7c, 0x10, 0x0a, 0x02, 0x1f, 0xed, 0xf0, 0x5f, 0x8d,
      0x7e, 0xb5, 0xd7, 0xa2, 0x3a, 0x8e, 0xa7, 0xa1, 0x6b, 0x20, 0xe6, 0xde,
      0x5e, 0xcf, 0x53, 0xd0, 0xf4, 0x30, 0x7e, 0xf4, 0xa8, 0xa3, 0xcf, 0x3e,
      0x47, 0x23, 0x8f, 0x73, 0x5d, 0x23, 0x2c, 0x03, 0x8a, 0x21, 0xe6, 0x34,
      0xd3, 0x69, 0x80, 0x62, 0x3b, 0x25, 0x08, 0x99, 0x7e, 0xc9, 0xf0, 0xf0,
      0x57, 0xd5, 0x48, 0xe6, 0xd6, 0x6b, 0xeb, 0x80, 0x68, 0x7a, 0xa9, 0xdc,
      0xa2, 0x9b, 0x73, 0x8a, 0xb2, 0x79, 0x1a, 0xb7, 0x61, 0x25, 0x86, 0x4c,
      0x19, 0x25, 0xc3, 0x77, 0xde, 0x9b, 0xed, 0xf5, 0xab, 0xe3, 0x83, 0x05,
      0x24, 0x40, 0x34, 0xbd, 0xcf, 0xe9, 0x41, 0xc1, 0x27, 0x69, 0x45, 0x4d,
      0x1b, 0x94, 0x7e, 0xee, 0x91, 0x75, 0x9c, 0x6e, 0x37, 0x5d, 0xdc, 0xee,
      0x38, 0xbe, 0x8d, 0x89, 0xd1, 0x36, 0x01, 0x5b, 0xd6, 0x4f, 0xb2, 0x8c,
      0x87, 0x95, 0xad, 0x96, 0x0a, 0x6c, 0xd2, 0x41, 0x1d, 0x2a, 0xeb, 0x40,
      0x8f, 0xbf, 0xb3, 0x59, 0x6b, 0xdc, 0x0d, 0xef, 0x9b, 0x23, 0xfb, 0xd3,
      0x3d, 0x48, 0xbf, 0x42, 0x77, 0x2d, 0xed, 0x92, 0x21, 0xff, 0x4c, 0x92,
      0xdf, 0x40, 0xea, 0xb7, 0x0e, 0x77, 0x2d, 0x49, 0x47, 0x67, 0x7a, 0xc6,
      0x3d, 0x7d, 0x22, 0xf2, 0x34, 0xa3, 0x40, 0x79, 0x27, 0x2a, 0x44, 0xa9,
      0x3d, 0xda, 0xb7, 0x3a, 0x21, 0x79, 0xad, 0x7f, 0x78, 0xe1, 0xf7, 0x03,
      0x7a, 0xe0, 0x2d, 0xce, 0x7c, 0x80, 0xdd, 0x6c, 0xbd, 0x33, 0x53, 0xa5,
      0x56, 0xe4, 0x36, 0x08, 0x3f, 0x4a, 0xf5, 0x60, 0x31, 0xef, 0xfa, 0xa1,
      0x92, 0x0e, 0x46, 0x8a, 0xb1, 0xd3, 0xd9, 0xfc, 0x01, 0x1d, 0xc8, 0xb7,
      0x89, 0x4c, 0x3f, 0x9e, 0x49, 0x50, 0x7a, 0x6d, 0x76, 0x9a, 0xbb, 0x18,
      0x72, 0x8a, 0x1d, 0x6d, 0xba, 0x5d, 0xcc, 0x5e, 0x00, 0xf6, 0xbf, 0xc5,
      0x51, 0x30, 0x32, 0x13, 0x9c, 0x26, 0x62, 0xfb, 0x4d, 0xd4, 0xa6, 0x54,
      0x45, 0x20, 0xec, 0x26, 0x6c, 0xfd, 0xc0, 0x4c, 0xb2, 0x7c, 0xbc, 0xd3,
      0x2e, 0x91, 0xc4, 0xbe, 0xa2, 0x5c, 0xda, 0x10, 0xe0, 0xbd, 0x81, 0xae,
      0x38, 0x49, 0x6e, 0x3c, 0xfd, 0x29, 0x02, 0xc8, 0x99, 0x20, 0xf2, 0x90,
      0x38, 0xe8, 0xd9, 0x7a, 0x1e, 0x10, 0x39, 0xff, 0x85, 0x38, 0x3d, 0x18,
      0xb3, 0xc9, 0x02, 0x60, 0xcb, 0x7c, 0x94, 0xd0, 0x11, 0x21, 0xf5, 0xf9,
      0x8c, 0xc5, 0x2b, 0x2b, 0x7e, 0x65, 0x22, 0x19, 0xf5, 0x71, 0x58, 0xa3,
      0xd2, 0x25, 0xa9, 0xb6, 0xc8, 0x8d, 0x85, 0x8c, 0x00, 0x45, 0xf9, 0x32};

  static const uint8_t kEncryptedExpected[kRsa3072SizeBytes] = {
      0x45, 0x91, 0x09, 0xad, 0x6a, 0xbd, 0xc1, 0x7f, 0x68, 0xea, 0x1e, 0xad,
      0xb7, 0x59, 0xfa, 0x98, 0xa6, 0x13, 0x03, 0x35, 0x45, 0x45, 0x64, 0xca,
      0x10, 0xa3, 0x47, 0x08, 0x45, 0xc9, 0x25, 0x7a, 0xd4, 0x62, 0x77, 0xe7,
      0xfa, 0xb8, 0x03, 0x96, 0xfc, 0xb7, 0x0c, 0x20, 0x0c, 0x1f, 0xd6, 0xcc,
      0x13, 0xe0, 0x0c, 0x47, 0x11, 0x8b, 0x82, 0x77, 0x3f, 0x9b, 0x65, 0x90,
      0x54, 0x8a, 0xff, 0x83, 0xbe, 0xf4, 0xa1, 0x2a, 0xf0, 0x3c, 0xb4, 0x1b,
      0xdd, 0x03, 0xd4, 0x87, 0x20, 0xad, 0x5d, 0x1e, 0xb8, 0x74, 0xc7, 0x91,
      0x0c, 0x14, 0x56, 0x92, 0xda, 0xc4, 0x99, 0x43, 0x2d, 0x27, 0x80, 0x72,
      0x5f, 0x20, 0xfe, 0xf8, 0xcb, 0x2e, 0xd4, 0xe4, 0x47, 0x19, 0x64, 0x72,
      0xb8, 0x45, 0xd8, 0x58, 0x45, 0x60, 0x53, 0xf3, 0x81, 0xa9, 0xd9, 0xad,
      0xac, 0x02, 0xc1, 0x1a, 0xf0, 0x6d, 0x93, 0xcc, 0x42, 0xda, 0x74, 0xd9,
      0x48, 0x05, 0xde, 0xd7, 0x4e, 0x23, 0x79, 0x63, 0x81, 0x3e, 0xc9, 0x23,
      0xff, 0x24, 0x61, 0x84, 0x01, 0x65, 0xea, 0x10, 0xa9, 0xc9, 0xc1, 0x77,
      0xcd, 0x06, 0x79, 0xb9, 0xeb, 0xeb, 0x2f, 0xd6, 0x43, 0xe1, 0xa3, 0x6d,
      0xfc, 0xb7, 0x1f, 0x6d, 0x05, 0x9d, 0x8c, 0x9f, 0xd5, 0xb7, 0x73, 0xdc,
      0xf0, 0x52, 0xe9, 0x18, 0x17, 0x1d, 0x12, 0x95, 0xa8, 0xc1, 0xff, 0x99,
      0xa7, 0x93, 0x6e, 0x3b, 0x93, 0x0a, 0x9d, 0x15, 0x21, 0x19, 0xe2, 0xa8,
      0x42, 0x1c, 0x3b, 0xf5, 0x5f, 0x33, 0xa9, 0x0e, 0x74, 0x84, 0xd6, 0x4c,
      0x37, 0x92, 0x84, 0x87, 0xdb, 0xa4, 0x80, 0x65, 0x39, 0x3f, 0xf8, 0x0e,
      0xb7, 0x77, 0xd9, 0x1d, 0x7c, 0x0a, 0x7f, 0x57, 0x7e, 0xa6, 0xce, 0xc3,
      0x61, 0x3d, 0x34, 0x8c, 0x48, 0x12, 0x4c, 0x80, 0x85, 0x14, 0xa5, 0x85,
      0x6c, 0x95, 0x3e, 0xaf, 0xca, 0xd3, 0x51, 0x4b, 0xf8, 0xc9, 0x16, 0x1f,
      0xfa, 0x5c, 0x4a, 0xe5, 0x9d, 0xab, 0xc4, 0xa3, 0xf4, 0xc3, 0xe9, 0x71,
      0x81, 0x1f, 0x2e, 0x40, 0x35, 0x35, 0x24, 0xf2, 0x6d, 0x26, 0xb9, 0x0d,
      0x9e, 0x23, 0x4f, 0xfb, 0xc2, 0x9e, 0x32, 0x4e, 0x05, 0xa0, 0xee, 0x6c,
      0xe6, 0x6e, 0x94, 0x1d, 0x1c, 0xf0, 0xb8, 0x53, 0x56, 0x88, 0x36, 0x0b,
      0x78, 0x96, 0x6e, 0x3f, 0x94, 0x00, 0xe8, 0x93, 0x4a, 0x37, 0xa5, 0xf8,
      0xe6, 0x20, 0xa4, 0x56, 0x4b, 0xf2, 0x41, 0x34, 0x4e, 0x20, 0x5e, 0x65,
      0xa4, 0x0b, 0xd6, 0xe9, 0x6f, 0x1b, 0x9f, 0x4b, 0x09, 0xd7, 0x47, 0x97,
      0x99, 0xb4, 0x03, 0xbc, 0x84, 0x82, 0xc0, 0xc0, 0x34, 0x53, 0xcc, 0x29,
      0xb3, 0xe7, 0x2d, 0xf8, 0xd8, 0x4d, 0x94, 0x47, 0xb2, 0xc5, 0xef, 0x36,
      0xef, 0x65, 0x15, 0x32, 0x4f, 0x91, 0x99, 0x24, 0x15, 0x69, 0x65, 0x0c};

  static const uint8_t kIn[kRsa3072SizeBytes] = {
      "OpenTitan proudly presents: OTBN"};
  uint8_t out_encrypted[kRsa3072SizeBytes] = {0};
  uint8_t out_decrypted[kRsa3072SizeBytes] = {0};

  LOG_INFO("Running RSA3072 test");
  rsa_roundtrip(kRsa3072SizeBytes, kModulus, kPrivateExponent, kIn,
                kEncryptedExpected, out_encrypted, out_decrypted);
}

static void test_rsa4096_roundtrip(void) {
  static const uint8_t kModulus[kRsa4096SizeBytes] = {
      0xd1, 0xde, 0xe6, 0xd0, 0x09, 0x86, 0xfd, 0xd2, 0xb8, 0xcf, 0xf0, 0x57,
      0x46, 0xac, 0x71, 0xda, 0x1d, 0x02, 0xcc, 0xf8, 0xf4, 0x66, 0x86, 0x3a,
      0x92, 0x98, 0x5a, 0x89, 0x48, 0xca, 0x48, 0x9e, 0xab, 0x48, 0x53, 0xbf,
      0xf1, 0x8c, 0xb5, 0x2f, 0xb1, 0xe0, 0x10, 0xd0, 0x44, 0x31, 0x16, 0x52,
      0xce, 0x18, 0xb1, 0xe0, 0x2c, 0x2f, 0xf8, 0x95, 0xdd, 0xe5, 0xa0, 0x2f,
      0x11, 0xa0, 0xba, 0xec, 0xfb, 0xa9, 0x77, 0x95, 0x72, 0xfe, 0xe2, 0x5f,
      0x00, 0xc7, 0x38, 0x4b, 0x5b, 0xe9, 0x78, 0xe5, 0x87, 0x1b, 0xcc, 0xdc,
      0xa6, 0x09, 0xc2, 0xfd, 0x0d, 0xd9, 0x03, 0xc9, 0xbc, 0x35, 0xac, 0x30,
      0xcc, 0x0b, 0x79, 0xef, 0x6b, 0x6b, 0xf1, 0x85, 0x4a, 0x58, 0x9b, 0xe0,
      0x0a, 0xff, 0x7d, 0x60, 0x7a, 0x39, 0xfe, 0xd9, 0xeb, 0x75, 0xcb, 0x18,
      0x18, 0x8e, 0x89, 0x7d, 0x81, 0xd2, 0x24, 0x40, 0x11, 0xf4, 0xe9, 0x6f,
      0x28, 0x1f, 0xb9, 0xc8, 0x5a, 0xaf, 0x9c, 0x97, 0x51, 0xf8, 0xbe, 0xef,
      0x03, 0x64, 0x5b, 0xbb, 0x77, 0xda, 0xfc, 0xa8, 0xc9, 0xd5, 0x32, 0x04,
      0x2e, 0x75, 0x60, 0x99, 0x4a, 0x20, 0x64, 0x5c, 0x6d, 0x5b, 0xf3, 0x02,
      0xf7, 0xe5, 0x66, 0x83, 0x6b, 0xfb, 0x3f, 0x88, 0x9d, 0x9b, 0x73, 0xf7,
      0x5c, 0x43, 0x43, 0xfd, 0x0d, 0xb4, 0x34, 0x18, 0xd8, 0x10, 0xb9, 0x3b,
      0x30, 0x37, 0x31, 0x61, 0x83, 0x6c, 0x24, 0x44, 0x28, 0x7f, 0x76, 0x95,
      0xd7, 0x2f, 0xdb, 0x0b, 0xc9, 0x7a, 0x72, 0xc7, 0x93, 0xb9, 0x51, 0xd4,
      0xb0, 0xb7, 0x4f, 0xd6, 0x8c, 0xe6, 0xb2, 0x7a, 0x12, 0xae, 0xd2, 0x24,
      0x54, 0x74, 0x36, 0x75, 0xbb, 0x6e, 0xc7, 0x3a, 0xc8, 0xb4, 0x82, 0xd1,
      0x8d, 0x6f, 0x60, 0x08, 0xc7, 0x2a, 0xf8, 0xc1, 0x74, 0x1d, 0x3c, 0x05,
      0x26, 0xd0, 0x31, 0x20, 0xf5, 0xe5, 0x14, 0xbc, 0xe6, 0x83, 0x77, 0xae,
      0xbb, 0xf3, 0x9c, 0x93, 0x61, 0x8b, 0xb0, 0xa0, 0xfa, 0xc7, 0x13, 0xc8,
      0x8d, 0xca, 0x36, 0xd9, 0x5e, 0xec, 0x37, 0xe8, 0x71, 0x3a, 0x39, 0x78,
      0x26, 0xdd, 0x1e, 0x03, 0xba, 0x9d, 0xea, 0xb2, 0x99, 0x3b, 0x89, 0xb9,
      0xc5, 0x6e, 0x5d, 0x38, 0x06, 0xf7, 0x7a, 0x89, 0x1a, 0xfa, 0x49, 0xff,
      0x05, 0x75, 0x56, 0xd0, 0xbd, 0x8e, 0xb5, 0x1e, 0xf3, 0xc2, 0x4d, 0x4e,
      0x24, 0x61, 0x8a, 0x5b, 0xfe, 0x62, 0xbb, 0x43, 0x25, 0xd0, 0x63, 0xf7,
      0xa2, 0x2a, 0x6d, 0x09, 0x0f, 0xe2, 0x4d, 0x81, 0x54, 0x9a, 0xfc, 0x89,
      0x66, 0x00, 0x51, 0xf3, 0x9c, 0x13, 0xa7, 0x62, 0x63, 0x59, 0x71, 0x51,
      0xbc, 0x22, 0xba, 0xa3, 0xa5, 0x6a, 0x34, 0xa2, 0x67, 0xfd, 0xe1, 0xd3,
      0x6c, 0xfa, 0x64, 0xa4, 0x3b, 0x54, 0x8b, 0x50, 0x7d, 0x54, 0x1a, 0x11,
      0x74, 0xd5, 0x35, 0x25, 0x6d, 0x8b, 0x14, 0xf1, 0xa0, 0xfc, 0xce, 0x8d,
      0x5f, 0xa5, 0xe9, 0x20, 0xc2, 0x95, 0xa9, 0xb2, 0x93, 0xb7, 0xfa, 0x41,
      0x95, 0x0a, 0xdc, 0xa7, 0xe9, 0xe9, 0x87, 0xb5, 0x35, 0xab, 0x4d, 0xa0,
      0x15, 0x9a, 0xb3, 0xba, 0x64, 0x50, 0x25, 0xf9, 0xf5, 0xb1, 0x1c, 0x9f,
      0xa1, 0x34, 0x18, 0xd3, 0xab, 0x8b, 0xe9, 0x35, 0x77, 0xca, 0x45, 0x77,
      0x10, 0xe8, 0x41, 0xfb, 0x04, 0x12, 0x00, 0x04, 0x70, 0x95, 0xa9, 0x70,
      0xcc, 0xa6, 0xae, 0xe3, 0xe2, 0x18, 0xca, 0x8a, 0x29, 0x57, 0x17, 0x1c,
      0x48, 0x67, 0xbd, 0x1e, 0x54, 0x6d, 0x45, 0x5e, 0x45, 0x2c, 0x34, 0x64,
      0x71, 0xc0, 0xdb, 0x61, 0xe1, 0x73, 0x98, 0x25, 0x41, 0xf2, 0x18, 0xd9,
      0x28, 0xfb, 0xef, 0x16, 0xa5, 0xf9, 0x9d, 0xe0, 0x54, 0x13, 0xf8, 0x28,
      0x98, 0xb7, 0x1a, 0x4a, 0x7f, 0xa2, 0x74, 0xf0};

  static const uint8_t kPrivateExponent[kRsa4096SizeBytes] = {
      0xc1, 0x91, 0x67, 0x67, 0xb2, 0xe7, 0xd2, 0xba, 0x16, 0xf1, 0x70, 0xec,
      0xbf, 0x6e, 0x73, 0x6f, 0x3b, 0x4f, 0x09, 0xed, 0x09, 0x91, 0xc1, 0xc3,
      0x73, 0x8c, 0x19, 0x33, 0x93, 0xcd, 0xd2, 0xb2, 0x9d, 0x53, 0x49, 0xa4,
      0xf7, 0xce, 0xde, 0x09, 0xbf, 0x58, 0x56, 0xd6, 0xd7, 0x10, 0x0c, 0xcc,
      0x08, 0x68, 0xe4, 0xdd, 0xfb, 0x31, 0xc2, 0x9b, 0x0e, 0xc6, 0xf4, 0x21,
      0x59, 0x13, 0x83, 0xbd, 0xb8, 0x04, 0x68, 0xaf, 0xd9, 0x97, 0xf9, 0x0e,
      0xf9, 0x5d, 0x90, 0x76, 0xdb, 0xae, 0x9e, 0xb4, 0x16, 0xb0, 0x14, 0xcf,
      0xeb, 0x2c, 0x2b, 0xde, 0x2a, 0xb9, 0x93, 0xc1, 0xb7, 0x5d, 0x6d, 0xec,
      0x46, 0x09, 0xe2, 0x12, 0xf1, 0x38, 0x45, 0x61, 0x4d, 0x17, 0x54, 0x17,
      0xb1, 0x06, 0x29, 0x53, 0x85, 0x56, 0xc9, 0x3b, 0xb4, 0xf3, 0x28, 0xeb,
      0xc1, 0x0a, 0x3f, 0x05, 0xbc, 0xed, 0x7b, 0x64, 0x09, 0xab, 0xbd, 0x99,
      0x22, 0x71, 0xbf, 0x40, 0x27, 0x3d, 0x8a, 0xcf, 0xb3, 0x31, 0xfd, 0x6b,
      0xdf, 0x85, 0x46, 0x39, 0x3a, 0x7c, 0x6f, 0xbd, 0x04, 0xb0, 0x4e, 0xf4,
      0xe2, 0x89, 0xc0, 0x86, 0xb4, 0xa8, 0xf8, 0x29, 0x85, 0x99, 0x1f, 0x80,
      0x09, 0xa8, 0x77, 0xcc, 0xdf, 0x1f, 0x4d, 0x1b, 0xe9, 0x6e, 0x2d, 0xe3,
      0x15, 0x43, 0x7e, 0x07, 0x6c, 0x4d, 0x97, 0xb0, 0x07, 0x23, 0xe1, 0x07,
      0xf7, 0xcc, 0x6d, 0x9f, 0x8e, 0xd9, 0xd3, 0xa3, 0xa3, 0x7e, 0xbd, 0xa5,
      0x23, 0xb7, 0xab, 0x54, 0xe5, 0xc3, 0xa9, 0xb4, 0xe4, 0xa2, 0xf4, 0x33,
      0x40, 0xa9, 0x5f, 0xa2, 0x60, 0x65, 0xaa, 0xaa, 0x2c, 0x44, 0x59, 0xca,
      0x2e, 0x4e, 0xd5, 0x8a, 0x48, 0xd6, 0x65, 0xfd, 0x43, 0x51, 0x11, 0x2d,
      0x64, 0x07, 0x99, 0x63, 0x8d, 0x17, 0xe4, 0xb2, 0xe0, 0xa3, 0x9a, 0xc1,
      0xb8, 0xb6, 0x8f, 0x2c, 0x84, 0x95, 0xa5, 0xb3, 0x54, 0xca, 0xd0, 0x1f,
      0xce, 0x11, 0x57, 0x06, 0x1c, 0x54, 0xa5, 0x93, 0x7a, 0x0b, 0xa8, 0x9d,
      0xa9, 0x77, 0x9d, 0xe1, 0x63, 0xd9, 0xfd, 0x6c, 0x62, 0x9b, 0x7c, 0x48,
      0x80, 0x0f, 0x9b, 0x7f, 0x6c, 0x22, 0x9f, 0x8d, 0x76, 0x41, 0xfb, 0xa9,
      0x24, 0x23, 0x6f, 0xe2, 0x2e, 0x17, 0x1a, 0x16, 0x07, 0x7f, 0xba, 0x90,
      0xea, 0x10, 0xed, 0x60, 0x74, 0xb5, 0x71, 0x51, 0xd2, 0xff, 0x0f, 0xbb,
      0xe9, 0x91, 0xad, 0x73, 0xa8, 0x82, 0xd7, 0xd9, 0xd0, 0xd3, 0xda, 0x56,
      0x28, 0x2c, 0x9a, 0x24, 0xa5, 0xd8, 0xf1, 0x96, 0x0d, 0xb7, 0x92, 0xbd,
      0x62, 0xf4, 0x9d, 0x62, 0x69, 0xb5, 0x0f, 0xd4, 0xf7, 0xff, 0x6f, 0x9c,
      0xca, 0xde, 0x15, 0xa4, 0x6f, 0x9e, 0xb8, 0xd4, 0xa7, 0x3f, 0x66, 0x2a,
      0x15, 0x72, 0x0f, 0xe0, 0xbd, 0xa0, 0xe0, 0x9c, 0xbd, 0x44, 0x8f, 0xfd,
      0x89, 0xf1, 0x25, 0xd3, 0x17, 0xc4, 0xbc, 0x5f, 0x3a, 0x1a, 0x70, 0x26,
      0x1b, 0x4b, 0x70, 0x97, 0x0d, 0x30, 0x1f, 0x4d, 0x0c, 0x44, 0x7b, 0x89,
      0x6f, 0xa3, 0x76, 0xec, 0xc6, 0xba, 0x2a, 0x42, 0x96, 0x51, 0xa7, 0xea,
      0xa8, 0xca, 0x21, 0xa0, 0x2d, 0xd0, 0x05, 0x25, 0xa7, 0x14, 0x16, 0xcf,
      0x94, 0xbb, 0x85, 0x36, 0x19, 0xeb, 0x40, 0x64, 0x11, 0x60, 0xcb, 0x63,
      0xd5, 0xd7, 0xbf, 0x65, 0x20, 0x87, 0xea, 0x0d, 0xf4, 0xbe, 0x21, 0x76,
      0x15, 0x18, 0x10, 0x34, 0x6f, 0xaa, 0xef, 0x4b, 0x2b, 0x10, 0xf5, 0x67,
      0x07, 0xc2, 0xf3, 0x76, 0x76, 0xe7, 0x55, 0xbe, 0x3f, 0xdb, 0x73, 0xce,
      0xfa, 0xec, 0x2b, 0xb6, 0x5a, 0x08, 0x73, 0x55, 0xb6, 0xed, 0x74, 0x11,
      0xec, 0x28, 0x79, 0x17, 0xa6, 0x9f, 0xca, 0x15, 0xe1, 0xd6, 0x67, 0x35,
      0x6f, 0x9b, 0x99, 0x46, 0x7f, 0x20, 0x4a, 0x43};

  static const uint8_t kEncryptedExpected[kRsa4096SizeBytes] = {
      0x59, 0x13, 0x5c, 0x73, 0xb2, 0xee, 0xe6, 0x48, 0x24, 0x95, 0x80, 0xe3,
      0x5b, 0x54, 0x08, 0x7d, 0x81, 0x98, 0x4a, 0x64, 0xb0, 0xf4, 0x06, 0x29,
      0x6d, 0x4a, 0x51, 0x9c, 0x12, 0xcd, 0xe3, 0x4a, 0x5b, 0x48, 0x7a, 0x84,
      0x8c, 0x0c, 0x76, 0x77, 0x28, 0x23, 0xf2, 0x77, 0x62, 0x32, 0xfa, 0x03,
      0x31, 0x62, 0x83, 0x2f, 0x04, 0x97, 0x94, 0x28, 0x56, 0xa9, 0x0d, 0xaf,
      0x25, 0x85, 0xb4, 0x55, 0x87, 0x50, 0xde, 0xed, 0x1f, 0x37, 0xd4, 0xc8,
      0x39, 0x51, 0x70, 0x7a, 0x1e, 0x36, 0xd3, 0x24, 0x04, 0x94, 0x5f, 0xa3,
      0xc1, 0xf9, 0x14, 0x62, 0x1e, 0x03, 0xb6, 0x3c, 0xd6, 0x7b, 0x55, 0x1d,
      0x23, 0x64, 0x9d, 0x7e, 0xcc, 0x74, 0xba, 0x9a, 0x57, 0x29, 0xcd, 0xea,
      0x13, 0xf9, 0x41, 0xca, 0xe7, 0x31, 0x95, 0xf3, 0x78, 0x2f, 0x8f, 0x91,
      0x59, 0x36, 0x11, 0x28, 0xae, 0x01, 0x82, 0x05, 0x78, 0x68, 0xc9, 0x6a,
      0xee, 0x1c, 0x7b, 0x48, 0x4d, 0x55, 0xa5, 0x64, 0x43, 0xe2, 0x90, 0x4e,
      0x1e, 0x12, 0x4b, 0x5c, 0xa6, 0xb9, 0x12, 0x57, 0xe0, 0x9a, 0x19, 0x88,
      0xa8, 0x10, 0x8b, 0x92, 0x26, 0xf8, 0x62, 0x9e, 0x46, 0xcf, 0x65, 0xcf,
      0xdd, 0xcc, 0xb1, 0x23, 0xaf, 0x55, 0x3a, 0x8f, 0x47, 0xa6, 0x8a, 0xd9,
      0x1b, 0x19, 0x5b, 0x5e, 0x5f, 0x6c, 0xa6, 0x4e, 0xb8, 0xa7, 0x6d, 0x2f,
      0x6b, 0x0e, 0xd0, 0x0c, 0xf2, 0x01, 0xa3, 0xfd, 0x2a, 0x67, 0x5b, 0x81,
      0x21, 0x32, 0x25, 0x79, 0x95, 0x8f, 0x78, 0x05, 0x92, 0x45, 0x4a, 0x67,
      0x26, 0xd6, 0xc6, 0x5e, 0x17, 0xd3, 0xbb, 0x00, 0x14, 0x13, 0xb9, 0xa4,
      0x0d, 0x0a, 0xd4, 0x98, 0xff, 0x8f, 0x57, 0x13, 0xc2, 0x16, 0x0c, 0xc0,
      0x70, 0x67, 0x1a, 0x5d, 0xd0, 0xc7, 0xa1, 0x58, 0x28, 0x74, 0x67, 0x9c,
      0x19, 0xee, 0xef, 0x94, 0x79, 0xc4, 0x60, 0xb8, 0x6e, 0x47, 0x45, 0xe1,
      0x51, 0xd9, 0x57, 0x53, 0x24, 0x4d, 0x44, 0xc5, 0xf3, 0xbc, 0x15, 0x7e,
      0xfe, 0x7d, 0x2b, 0xb0, 0x51, 0xa3, 0x77, 0x76, 0xd4, 0x85, 0x51, 0x6d,
      0xeb, 0x6f, 0x31, 0x45, 0x63, 0x1e, 0x64, 0xac, 0x11, 0x2f, 0xbc, 0x2c,
      0xc0, 0xe8, 0x39, 0x8d, 0x8c, 0x40, 0x01, 0x41, 0x03, 0xd1, 0xce, 0xfb,
      0x68, 0x42, 0x0e, 0x63, 0xf3, 0xd0, 0x63, 0xf6, 0xc9, 0xc9, 0x84, 0xfc,
      0x90, 0x59, 0x2d, 0x7a, 0x85, 0x1a, 0x7d, 0xd6, 0x11, 0x73, 0xe2, 0x45,
      0x40, 0x72, 0x82, 0x08, 0xcd, 0x3b, 0x19, 0x26, 0x20, 0x7c, 0x86, 0x1d,
      0xde, 0xbf, 0x4f, 0xb7, 0x49, 0x2b, 0xc3, 0x2f, 0x1a, 0x8e, 0x5b, 0xeb,
      0x1e, 0xf5, 0xa9, 0xb6, 0x59, 0xdf, 0xca, 0x5a, 0x07, 0x95, 0xaf, 0x1a,
      0xbf, 0x2c, 0x2a, 0x18, 0x02, 0xeb, 0x76, 0xa3, 0xad, 0x53, 0x4e, 0xf3,
      0x18, 0xe4, 0xb9, 0xac, 0x76, 0x80, 0x8a, 0xe0, 0x37, 0x36, 0x29, 0x34,
      0x20, 0x52, 0x45, 0x81, 0x80, 0xb0, 0x1c, 0xd4, 0xac, 0x56, 0x50, 0xc6,
      0x1b, 0xe3, 0xbf, 0xa4, 0xd8, 0x55, 0xd8, 0xdd, 0xbe, 0x3a, 0x9e, 0x5f,
      0x65, 0xc7, 0xa0, 0x14, 0xb8, 0xa8, 0x75, 0x61, 0x5f, 0x50, 0xba, 0x2b,
      0x41, 0x6c, 0xf5, 0x4b, 0x9e, 0xf1, 0x66, 0xc4, 0x2a, 0xbb, 0xc5, 0xaf,
      0x10, 0x92, 0x70, 0xc9, 0x1b, 0xcd, 0x59, 0x3a, 0x17, 0xaa, 0x5e, 0xf3,
      0x6c, 0x48, 0x1b, 0xe8, 0xee, 0x8f, 0x00, 0xbb, 0xcf, 0xa6, 0x90, 0xa1,
      0xfd, 0x61, 0x19, 0x5d, 0xba, 0x55, 0xbe, 0x50, 0x77, 0xd5, 0xcc, 0xea,
      0xe8, 0x0a, 0x98, 0x48, 0x15, 0xb5, 0xee, 0x22, 0xc4, 0xab, 0x04, 0xfb,
      0xcb, 0x2c, 0x05, 0x1f, 0xef, 0x72, 0x4d, 0xf9, 0x5b, 0x29, 0x1c, 0x07,
      0x9e, 0xd4, 0x63, 0xe2, 0x9f, 0xfd, 0x04, 0x39};

  static const uint8_t kIn[kRsa4096SizeBytes] = {"OTBN is doing RSA here"};
  uint8_t out_encrypted[kRsa4096SizeBytes] = {0};
  uint8_t out_decrypted[kRsa4096SizeBytes] = {0};

  LOG_INFO("Running RSA4096 test");
  rsa_roundtrip(kRsa4096SizeBytes, kModulus, kPrivateExponent, kIn,
                kEncryptedExpected, out_encrypted, out_decrypted);
}

bool test_main(void) {
  CHECK_STATUS_OK(entropy_testutils_auto_mode_init());

  test_rsa512_roundtrip();
  test_rsa1024_roundtrip();

  if (kTestRsaGreater1k) {
    test_rsa2048_roundtrip();
    test_rsa3072_roundtrip();
    test_rsa4096_roundtrip();
  }

  return true;
}
