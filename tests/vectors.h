/* Published test vectors shared by the test files. */
#ifndef ORIGO_TEST_VECTORS_H_
#define ORIGO_TEST_VECTORS_H_

/* Published BIP84/BIP86 vectors for the all-zero entropy mnemonic. */
static const char mnemonic[]
    = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";

static const char expected_address[] = "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu";

/* BIP84's own published change vector, m/84'/0'/0'/1/0 for the same mnemonic -
 * the spec publishes both branches, so the change side is pinned to a vector
 * this project did not invent either. */
static const char expected_change_address[] = "bc1q8c6fshw2dlwun7ekn9qwf37cu2rn755upcp6el";

static const char expected_xpub84[]
    = "xpub6CatWdiZiodmUeTDp8LT5or8nmbKNcuyvz7WyksVFkKB4RHwCD3XyuvPEbvqAQY3rAPshWcMLoP2fMFMKHPJ4ZeZXYVUhLv1VMrjPC7PW6V";

static const char expected_xpub86[]
    = "xpub6BgBgsespWvERF3LHQu6CnqdvfEvtMcQjYrcRzx53QJjSxarj2afYWcLteoGVky7D3UKDP9QyrLprQ3VCECoY49yfdDEHGCtMMj92pReUsQ";

/* SLIP-132 zpub for the same account: the identical key, xpub's version bytes
 * swapped for zpub's, published in the SLIP-132 test vectors. */
static const char expected_zpub84[]
    = "zpub6rFR7y4Q2AijBEqTUquhVz398htDFrtymD9xYYfG1m4wAcvPhXNfE3EfH1r1ADqtfSdVCToUG868RvUUkgDKf31mGDtKsAYz2oz2AGutZYs";

/* Published BIP39 vector for the all-zero 256-bit entropy. */
static const char mnemonic24[]
    = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon "
      "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon art";

/* Same words as `mnemonic`, checksum deliberately broken. */
static const char bad_checksum_mnemonic[]
    = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon";

#endif
