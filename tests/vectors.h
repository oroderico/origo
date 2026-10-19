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

/* m/84'/0'/0'/0/9999, the last index the viewer derives, from
 * tools/origo_verify.py: a pure-Python derivation sharing no code with
 * libwally. */
static const char expected_address_9999[] = "bc1qhr6g4qhtaqlu8jvfex80gexwmxca2p65ujuwt8";

static const char expected_xpub84[]
    = "xpub6CatWdiZiodmUeTDp8LT5or8nmbKNcuyvz7WyksVFkKB4RHwCD3XyuvPEbvqAQY3rAPshWcMLoP2fMFMKHPJ4ZeZXYVUhLv1VMrjPC7PW6V";

static const char expected_xpub86[]
    = "xpub6BgBgsespWvERF3LHQu6CnqdvfEvtMcQjYrcRzx53QJjSxarj2afYWcLteoGVky7D3UKDP9QyrLprQ3VCECoY49yfdDEHGCtMMj92pReUsQ";

/* The first BIP44 and BIP49 receive addresses for the same mnemonic, as every
 * BIP39 reference wallet derives them, and the ypub of the BIP49 account
 * (SLIP-132: the xpub's 78 bytes under ypub's version). The account xpubs are
 * from tools/origo_verify.py, which shares no code with libwally. */
static const char expected_address44[] = "1LqBGSKuX5yYUonjxT5qGfpUsXKYYWeabA";
static const char expected_address49[] = "37VucYSaXLCAsxYyAPfbSi9eh4iEcbShgf";
static const char expected_xpub44[]
    = "xpub6BosfCnifzxcFwrSzQiqu2DBVTshkCXacvNsWGYJVVhhawA7d4R5WSWGFNbi8Aw6ZRc1brxMyWMzG3DSSSSoekkudhUd9yLb6qx39T9nMdj";
static const char expected_xpub49[]
    = "xpub6C6nQwHaWbSrzs5tZ1q7m5R9cPK9eYpNMFesiXsYrgc1P8bvLLAet9JfHjYXKjToD8cBRswJXXbbFpXgwsswVPAZzKMa1jUp2kVkGVUaJa7";
static const char expected_ypub49[]
    = "ypub6Ww3ibxVfGzLrAH1PNcjyAWenMTbbAosGNB6VvmSEgytSER9azLDWCxoJwW7Ke7icmizBMXrzBx9979FfaHxHcrArf3zbeJJJUZPf663zsP";

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
