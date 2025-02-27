/*
 *  Public Key layer for parsing key files and structures
 *
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

#if defined(MBEDTLS_PK_PARSE_C)

#include "mbedtls/pk.h"
#include "mbedtls/asn1.h"
#include "mbedtls/oid.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/platform.h"
#include "mbedtls/error.h"

#include <string.h>

#if defined(MBEDTLS_USE_PSA_CRYPTO)
#include "mbedtls/psa_util.h"
#include "psa/crypto.h"
#endif

/* Key types */
#if defined(MBEDTLS_RSA_C)
#include "mbedtls/rsa.h"
#endif
#if defined(MBEDTLS_PK_HAVE_ECC_KEYS)
#include "mbedtls/ecp.h"
#include "pk_internal.h"
#endif

/* Extended formats */
#if defined(MBEDTLS_PEM_PARSE_C)
#include "mbedtls/pem.h"
#endif
#if defined(MBEDTLS_PKCS5_C)
#include "mbedtls/pkcs5.h"
#endif
#if defined(MBEDTLS_PKCS12_C)
#include "mbedtls/pkcs12.h"
#endif

#if defined(MBEDTLS_PK_HAVE_ECC_KEYS)

/***********************************************************************
 *
 *      ECC setters
 *
 * 1. This is an abstraction layer around MBEDTLS_PK_USE_PSA_EC_DATA:
 *    this macro will not appear outside this section.
 * 2. All inputs are raw: no metadata, no ASN.1 until the next section.
 *
 **********************************************************************/

/*
 * Set the group used by this key.
 *
 * [in/out] pk: in: must have been pk_setup() to an ECC type
 *              out: will have group (curve) information set
 * [in] grp_in: a supported group ID (not NONE)
 */
static int pk_ecc_set_group(mbedtls_pk_context *pk, mbedtls_ecp_group_id grp_id)
{
#if defined(MBEDTLS_PK_USE_PSA_EC_DATA)
    size_t ec_bits;
    psa_ecc_family_t ec_family = mbedtls_ecc_group_to_psa(grp_id, &ec_bits);

    /* group may already be initialized; if so, make sure IDs match */
    if ((pk->ec_family != 0 && pk->ec_family != ec_family) ||
        (pk->ec_bits != 0 && pk->ec_bits != ec_bits)) {
        return MBEDTLS_ERR_PK_KEY_INVALID_FORMAT;
    }

    /* set group */
    pk->ec_family = ec_family;
    pk->ec_bits = ec_bits;

    return 0;
#else /* MBEDTLS_PK_USE_PSA_EC_DATA */
    mbedtls_ecp_keypair *ecp = mbedtls_pk_ec_rw(*pk);

    /* grp may already be initialized; if so, make sure IDs match */
    if (mbedtls_pk_ec_ro(*pk)->grp.id != MBEDTLS_ECP_DP_NONE &&
        mbedtls_pk_ec_ro(*pk)->grp.id != grp_id) {
        return MBEDTLS_ERR_PK_KEY_INVALID_FORMAT;
    }

    /* set group */
    return mbedtls_ecp_group_load(&(ecp->grp), grp_id);
#endif /* MBEDTLS_PK_USE_PSA_EC_DATA */
}

/*
 * Set the private key material
 *
 * [in/out] pk: in: must have the group set already, see pk_ecc_set_group().
 *              out: will have the private key set.
 * [in] key, key_len: the raw private key (no ASN.1 wrapping).
 */
static int pk_ecc_set_key(mbedtls_pk_context *pk,
                          unsigned char *key, size_t key_len)
{
#if defined(MBEDTLS_PK_USE_PSA_EC_DATA)
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_status_t status;

    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(pk->ec_family));
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);
    psa_key_usage_t flags = PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_DERIVE;
    /* Montgomery allows only ECDH, others ECDSA too */
    if (pk->ec_family != PSA_ECC_FAMILY_MONTGOMERY) {
        flags |= PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_SIGN_MESSAGE;
        psa_set_key_enrollment_algorithm(&attributes,
                                         MBEDTLS_PK_PSA_ALG_ECDSA_MAYBE_DET(PSA_ALG_ANY_HASH));
    }
    psa_set_key_usage_flags(&attributes, flags);

    status = psa_import_key(&attributes, key, key_len, &pk->priv_id);
    return psa_pk_status_to_mbedtls(status);

#else /* MBEDTLS_PK_USE_PSA_EC_DATA */

    mbedtls_ecp_keypair *eck = mbedtls_pk_ec_rw(*pk);
    int ret = mbedtls_ecp_read_key(eck->grp.id, eck, key, key_len);
    if (ret != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }
    return 0;
#endif /* MBEDTLS_PK_USE_PSA_EC_DATA */
}

/*
 * Derive a public key from its private counterpart.
 * Computationally intensive, only use when public key is not available.
 *
 * [in/out] pk: in: must have the private key set, see pk_ecc_set_key().
 *              out: will have the public key set.
 * [in] prv, prv_len: the raw private key (see note below).
 * [in] f_rng, p_rng: RNG function and context.
 *
 * Note: the private key information is always available from pk,
 * however for convenience the serialized version is also passed,
 * as it's available at each calling site, and useful in some configs
 * (as otherwise we would have to re-serialize it from the pk context).
 *
 * There are three implementations of this function:
 * 1. MBEDTLS_PK_USE_PSA_EC_DATA,
 * 2. MBEDTLS_USE_PSA_CRYPTO but not MBEDTLS_PK_USE_PSA_EC_DATA,
 * 3. not MBEDTLS_USE_PSA_CRYPTO.
 */
static int pk_ecc_set_pubkey_from_prv(mbedtls_pk_context *pk,
                                      const unsigned char *prv, size_t prv_len,
                                      int (*f_rng)(void *, unsigned char *, size_t), void *p_rng)
{
#if defined(MBEDTLS_PK_USE_PSA_EC_DATA)

    (void) f_rng;
    (void) p_rng;
    (void) prv;
    (void) prv_len;
    psa_status_t status;

    status = psa_export_public_key(pk->priv_id, pk->pub_raw, sizeof(pk->pub_raw),
                                   &pk->pub_raw_len);
    return psa_pk_status_to_mbedtls(status);

#elif defined(MBEDTLS_USE_PSA_CRYPTO) /* && !MBEDTLS_PK_USE_PSA_EC_DATA */

    (void) f_rng;
    (void) p_rng;
    psa_status_t status;

    mbedtls_ecp_keypair *eck = (mbedtls_ecp_keypair *) pk->pk_ctx;
    size_t curve_bits;
    psa_ecc_family_t curve = mbedtls_ecc_group_to_psa(eck->grp.id, &curve_bits);

    /* Import private key into PSA, from serialized input */
    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_key_attributes_t key_attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&key_attr, PSA_KEY_TYPE_ECC_KEY_PAIR(curve));
    psa_set_key_usage_flags(&key_attr, PSA_KEY_USAGE_EXPORT);
    status = psa_import_key(&key_attr, prv, prv_len, &key_id);
    if (status != PSA_SUCCESS) {
        return psa_pk_status_to_mbedtls(status);
    }

    /* Export public key from PSA */
    unsigned char pub[MBEDTLS_PSA_MAX_EC_PUBKEY_LENGTH];
    size_t pub_len;
    status = psa_export_public_key(key_id, pub, sizeof(pub), &pub_len);
    psa_status_t destruction_status = psa_destroy_key(key_id);
    if (status != PSA_SUCCESS) {
        return psa_pk_status_to_mbedtls(status);
    } else if (destruction_status != PSA_SUCCESS) {
        return psa_pk_status_to_mbedtls(destruction_status);
    }

    /* Load serialized public key into ecp_keypair structure */
    return mbedtls_ecp_point_read_binary(&eck->grp, &eck->Q, pub, pub_len);

#else /* MBEDTLS_USE_PSA_CRYPTO */

    (void) prv;
    (void) prv_len;

    mbedtls_ecp_keypair *eck = (mbedtls_ecp_keypair *) pk->pk_ctx;
    return mbedtls_ecp_mul(&eck->grp, &eck->Q, &eck->d, &eck->grp.G, f_rng, p_rng);

#endif /* MBEDTLS_USE_PSA_CRYPTO */
}

#if defined(MBEDTLS_PK_USE_PSA_EC_DATA)
/*
 * Set the public key: fallback using ECP_LIGHT in the USE_PSA_EC_DATA case.
 *
 * Normally, when MBEDTLS_PK_USE_PSA_EC_DATA is enabled, we only use PSA
 * functions to handle keys. However, currently psa_import_key() does not
 * support compressed points. In case that support was explicitly requested,
 * this fallback uses ECP functions to get the job done. This is the reason
 * why MBEDTLS_PK_PARSE_EC_COMPRESSED auto-enables MBEDTLS_ECP_LIGHT.
 *
 * [in/out] pk: in: must have the group set, see pk_ecc_set_group().
 *              out: will have the public key set.
 * [in] pub, pub_len: the public key as an ECPoint,
 *                    in any format supported by ECP.
 *
 * Return:
 * - 0 on success;
 * - MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE if the format is potentially valid
 *   but not supported;
 * - another error code otherwise.
 */
static int pk_ecc_set_pubkey_psa_ecp_fallback(mbedtls_pk_context *pk,
                                              const unsigned char *pub,
                                              size_t pub_len)
{
#if !defined(MBEDTLS_PK_PARSE_EC_COMPRESSED)
    (void) pk;
    (void) pub;
    (void) pub_len;
    return MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE;
#else /* MBEDTLS_PK_PARSE_EC_COMPRESSED */
    mbedtls_ecp_keypair ecp_key;
    mbedtls_ecp_group_id ecp_group_id;
    int ret;

    ecp_group_id = mbedtls_ecc_group_of_psa(pk->ec_family, pk->ec_bits, 0);

    mbedtls_ecp_keypair_init(&ecp_key);
    ret = mbedtls_ecp_group_load(&(ecp_key.grp), ecp_group_id);
    if (ret != 0) {
        goto exit;
    }
    ret = mbedtls_ecp_point_read_binary(&(ecp_key.grp), &ecp_key.Q,
                                        pub, pub_len);
    if (ret != 0) {
        goto exit;
    }
    ret = mbedtls_ecp_point_write_binary(&(ecp_key.grp), &ecp_key.Q,
                                         MBEDTLS_ECP_PF_UNCOMPRESSED,
                                         &pk->pub_raw_len, pk->pub_raw,
                                         sizeof(pk->pub_raw));

exit:
    mbedtls_ecp_keypair_free(&ecp_key);
    return ret;
#endif /* MBEDTLS_PK_PARSE_EC_COMPRESSED */
}
#endif /* MBEDTLS_PK_USE_PSA_EC_DATA */

/*
 * Set the public key.
 *
 * [in/out] pk: in: must have its group set, see pk_ecc_set_group().
 *              out: will have the public key set.
 * [in] pub, pub_len: the raw public key (an ECPoint).
 *
 * Return:
 * - 0 on success;
 * - MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE if the format is potentially valid
 *   but not supported;
 * - another error code otherwise.
 */
static int pk_ecc_set_pubkey(mbedtls_pk_context *pk,
                             const unsigned char *pub, size_t pub_len)
{
#if defined(MBEDTLS_PK_USE_PSA_EC_DATA)

    /* Load the key */
    if (!PSA_ECC_FAMILY_IS_WEIERSTRASS(pk->ec_family) || *pub == 0x04) {
        /* Format directly supported by PSA:
         * - non-Weierstrass curves that only have one format;
         * - uncompressed format for Weierstrass curves. */
        if (pub_len > sizeof(pk->pub_raw)) {
            return MBEDTLS_ERR_PK_BUFFER_TOO_SMALL;
        }
        memcpy(pk->pub_raw, pub, pub_len);
        pk->pub_raw_len = pub_len;
    } else {
        /* Other format, try the fallback */
        int ret = pk_ecc_set_pubkey_psa_ecp_fallback(pk, pub, pub_len);
        if (ret != 0) {
            return ret;
        }
    }

    /* Validate the key by trying to import it */
    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_key_attributes_t key_attrs = PSA_KEY_ATTRIBUTES_INIT;

    psa_set_key_usage_flags(&key_attrs, 0);
    psa_set_key_type(&key_attrs, PSA_KEY_TYPE_ECC_PUBLIC_KEY(pk->ec_family));
    psa_set_key_bits(&key_attrs, pk->ec_bits);

    if ((psa_import_key(&key_attrs, pk->pub_raw, pk->pub_raw_len,
                        &key_id) != PSA_SUCCESS) ||
        (psa_destroy_key(key_id) != PSA_SUCCESS)) {
        return MBEDTLS_ERR_PK_INVALID_PUBKEY;
    }

    return 0;

#else /* MBEDTLS_PK_USE_PSA_EC_DATA */

    int ret;
    mbedtls_ecp_keypair *ec_key = (mbedtls_ecp_keypair *) pk->pk_ctx;
    ret = mbedtls_ecp_point_read_binary(&ec_key->grp, &ec_key->Q, pub, pub_len);
    if (ret != 0) {
        return ret;
    }
    return mbedtls_ecp_check_pubkey(&ec_key->grp, &ec_key->Q);

#endif /* MBEDTLS_PK_USE_PSA_EC_DATA */
}

/***********************************************************************
 *
 *      Low-level ECC parsing: optional support for SpecifiedECDomain
 *
 * There are two functions here that are used by the rest of the code:
 * - pk_ecc_tag_is_speficied_ec_domain()
 * - pk_ecc_group_id_from_specified()
 *
 * All the other functions are internal to this section.
 *
 * The two "public" functions have a dummy variant provided
 * in configs without MBEDTLS_PK_PARSE_EC_EXTENDED. This acts as an
 * abstraction layer for this macro, which should not appear outside
 * this section.
 *
 **********************************************************************/

#if !defined(MBEDTLS_PK_PARSE_EC_EXTENDED)
/* See the "real" version for documentation */
static int pk_ecc_tag_is_specified_ec_domain(int tag)
{
    (void) tag;
    return 0;
}

/* See the "real" version for documentation */
static int pk_ecc_group_id_from_specified(const mbedtls_asn1_buf *params,
                                          mbedtls_ecp_group_id *grp_id)
{
    (void) params;
    (void) grp_id;
    return MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE;
}
#else /* MBEDTLS_PK_PARSE_EC_EXTENDED */
/*
 * Tell if the passed tag might be the start of SpecifiedECDomain
 * (that is, a sequence).
 */
static int pk_ecc_tag_is_specified_ec_domain(int tag)
{
    return tag == (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
}

/*
 * Parse a SpecifiedECDomain (SEC 1 C.2) and (mostly) fill the group with it.
 * WARNING: the resulting group should only be used with
 * pk_ecc_group_id_from_specified(), since its base point may not be set correctly
 * if it was encoded compressed.
 *
 *  SpecifiedECDomain ::= SEQUENCE {
 *      version SpecifiedECDomainVersion(ecdpVer1 | ecdpVer2 | ecdpVer3, ...),
 *      fieldID FieldID {{FieldTypes}},
 *      curve Curve,
 *      base ECPoint,
 *      order INTEGER,
 *      cofactor INTEGER OPTIONAL,
 *      hash HashAlgorithm OPTIONAL,
 *      ...
 *  }
 *
 * We only support prime-field as field type, and ignore hash and cofactor.
 */
static int pk_group_from_specified(const mbedtls_asn1_buf *params, mbedtls_ecp_group *grp)
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    unsigned char *p = params->p;
    const unsigned char *const end = params->p + params->len;
    const unsigned char *end_field, *end_curve;
    size_t len;
    int ver;

    /* SpecifiedECDomainVersion ::= INTEGER { 1, 2, 3 } */
    if ((ret = mbedtls_asn1_get_int(&p, end, &ver)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    if (ver < 1 || ver > 3) {
        return MBEDTLS_ERR_PK_KEY_INVALID_FORMAT;
    }

    /*
     * FieldID { FIELD-ID:IOSet } ::= SEQUENCE { -- Finite field
     *       fieldType FIELD-ID.&id({IOSet}),
     *       parameters FIELD-ID.&Type({IOSet}{@fieldType})
     * }
     */
    if ((ret = mbedtls_asn1_get_tag(&p, end, &len,
                                    MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) != 0) {
        return ret;
    }

    end_field = p + len;

    /*
     * FIELD-ID ::= TYPE-IDENTIFIER
     * FieldTypes FIELD-ID ::= {
     *       { Prime-p IDENTIFIED BY prime-field } |
     *       { Characteristic-two IDENTIFIED BY characteristic-two-field }
     * }
     * prime-field OBJECT IDENTIFIER ::= { id-fieldType 1 }
     */
    if ((ret = mbedtls_asn1_get_tag(&p, end_field, &len, MBEDTLS_ASN1_OID)) != 0) {
        return ret;
    }

    if (len != MBEDTLS_OID_SIZE(MBEDTLS_OID_ANSI_X9_62_PRIME_FIELD) ||
        memcmp(p, MBEDTLS_OID_ANSI_X9_62_PRIME_FIELD, len) != 0) {
        return MBEDTLS_ERR_PK_FEATURE_UNAVAILABLE;
    }

    p += len;

    /* Prime-p ::= INTEGER -- Field of size p. */
    if ((ret = mbedtls_asn1_get_mpi(&p, end_field, &grp->P)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    grp->pbits = mbedtls_mpi_bitlen(&grp->P);

    if (p != end_field) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT,
                                 MBEDTLS_ERR_ASN1_LENGTH_MISMATCH);
    }

    /*
     * Curve ::= SEQUENCE {
     *       a FieldElement,
     *       b FieldElement,
     *       seed BIT STRING OPTIONAL
     *       -- Shall be present if used in SpecifiedECDomain
     *       -- with version equal to ecdpVer2 or ecdpVer3
     * }
     */
    if ((ret = mbedtls_asn1_get_tag(&p, end, &len,
                                    MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) != 0) {
        return ret;
    }

    end_curve = p + len;

    /*
     * FieldElement ::= OCTET STRING
     * containing an integer in the case of a prime field
     */
    if ((ret = mbedtls_asn1_get_tag(&p, end_curve, &len, MBEDTLS_ASN1_OCTET_STRING)) != 0 ||
        (ret = mbedtls_mpi_read_binary(&grp->A, p, len)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    p += len;

    if ((ret = mbedtls_asn1_get_tag(&p, end_curve, &len, MBEDTLS_ASN1_OCTET_STRING)) != 0 ||
        (ret = mbedtls_mpi_read_binary(&grp->B, p, len)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    p += len;

    /* Ignore seed BIT STRING OPTIONAL */
    if ((ret = mbedtls_asn1_get_tag(&p, end_curve, &len, MBEDTLS_ASN1_BIT_STRING)) == 0) {
        p += len;
    }

    if (p != end_curve) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT,
                                 MBEDTLS_ERR_ASN1_LENGTH_MISMATCH);
    }

    /*
     * ECPoint ::= OCTET STRING
     */
    if ((ret = mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_OCTET_STRING)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    if ((ret = mbedtls_ecp_point_read_binary(grp, &grp->G,
                                             (const unsigned char *) p, len)) != 0) {
        /*
         * If we can't read the point because it's compressed, cheat by
         * reading only the X coordinate and the parity bit of Y.
         */
        if (ret != MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE ||
            (p[0] != 0x02 && p[0] != 0x03) ||
            len != mbedtls_mpi_size(&grp->P) + 1 ||
            mbedtls_mpi_read_binary(&grp->G.X, p + 1, len - 1) != 0 ||
            mbedtls_mpi_lset(&grp->G.Y, p[0] - 2) != 0 ||
            mbedtls_mpi_lset(&grp->G.Z, 1) != 0) {
            return MBEDTLS_ERR_PK_KEY_INVALID_FORMAT;
        }
    }

    p += len;

    /*
     * order INTEGER
     */
    if ((ret = mbedtls_asn1_get_mpi(&p, end, &grp->N)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    grp->nbits = mbedtls_mpi_bitlen(&grp->N);

    /*
     * Allow optional elements by purposefully not enforcing p == end here.
     */

    return 0;
}

/*
 * Find the group id associated with an (almost filled) group as generated by
 * pk_group_from_specified(), or return an error if unknown.
 */
static int pk_group_id_from_group(const mbedtls_ecp_group *grp, mbedtls_ecp_group_id *grp_id)
{
    int ret = 0;
    mbedtls_ecp_group ref;
    const mbedtls_ecp_group_id *id;

    mbedtls_ecp_group_init(&ref);

    for (id = mbedtls_ecp_grp_id_list(); *id != MBEDTLS_ECP_DP_NONE; id++) {
        /* Load the group associated to that id */
        mbedtls_ecp_group_free(&ref);
        MBEDTLS_MPI_CHK(mbedtls_ecp_group_load(&ref, *id));

        /* Compare to the group we were given, starting with easy tests */
        if (grp->pbits == ref.pbits && grp->nbits == ref.nbits &&
            mbedtls_mpi_cmp_mpi(&grp->P, &ref.P) == 0 &&
            mbedtls_mpi_cmp_mpi(&grp->A, &ref.A) == 0 &&
            mbedtls_mpi_cmp_mpi(&grp->B, &ref.B) == 0 &&
            mbedtls_mpi_cmp_mpi(&grp->N, &ref.N) == 0 &&
            mbedtls_mpi_cmp_mpi(&grp->G.X, &ref.G.X) == 0 &&
            mbedtls_mpi_cmp_mpi(&grp->G.Z, &ref.G.Z) == 0 &&
            /* For Y we may only know the parity bit, so compare only that */
            mbedtls_mpi_get_bit(&grp->G.Y, 0) == mbedtls_mpi_get_bit(&ref.G.Y, 0)) {
            break;
        }
    }

cleanup:
    mbedtls_ecp_group_free(&ref);

    *grp_id = *id;

    if (ret == 0 && *id == MBEDTLS_ECP_DP_NONE) {
        ret = MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE;
    }

    return ret;
}

/*
 * Parse a SpecifiedECDomain (SEC 1 C.2) and find the associated group ID
 */
static int pk_ecc_group_id_from_specified(const mbedtls_asn1_buf *params,
                                          mbedtls_ecp_group_id *grp_id)
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    mbedtls_ecp_group grp;

    mbedtls_ecp_group_init(&grp);

    if ((ret = pk_group_from_specified(params, &grp)) != 0) {
        goto cleanup;
    }

    ret = pk_group_id_from_group(&grp, grp_id);

cleanup:
    /* The API respecting lifecycle for mbedtls_ecp_group struct is
     * _init(), _load() and _free(). In pk_ecc_group_id_from_specified() the
     * temporary grp breaks that flow and it's members are populated
     * by pk_group_id_from_group(). As such mbedtls_ecp_group_free()
     * which is assuming a group populated by _setup() may not clean-up
     * properly -> Manually free it's members.
     */
    mbedtls_mpi_free(&grp.N);
    mbedtls_mpi_free(&grp.P);
    mbedtls_mpi_free(&grp.A);
    mbedtls_mpi_free(&grp.B);
    mbedtls_ecp_point_free(&grp.G);

    return ret;
}
#endif /* MBEDTLS_PK_PARSE_EC_EXTENDED */

/***********************************************************************
 *
 * Unsorted (yet!) from this point on until the next section header
 *
 **********************************************************************/

/* Minimally parse an ECParameters buffer to and mbedtls_asn1_buf
 *
 * ECParameters ::= CHOICE {
 *   namedCurve         OBJECT IDENTIFIER
 *   specifiedCurve     SpecifiedECDomain -- = SEQUENCE { ... }
 *   -- implicitCurve   NULL
 * }
 */
static int pk_get_ecparams(unsigned char **p, const unsigned char *end,
                           mbedtls_asn1_buf *params)
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;

    if (end - *p < 1) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT,
                                 MBEDTLS_ERR_ASN1_OUT_OF_DATA);
    }

    /* Acceptable tags: OID for namedCurve, or specifiedECDomain */
    params->tag = **p;
    if (params->tag != MBEDTLS_ASN1_OID &&
        !pk_ecc_tag_is_specified_ec_domain(params->tag)) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT,
                                 MBEDTLS_ERR_ASN1_UNEXPECTED_TAG);
    }

    if ((ret = mbedtls_asn1_get_tag(p, end, &params->len, params->tag)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    params->p = *p;
    *p += params->len;

    if (*p != end) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT,
                                 MBEDTLS_ERR_ASN1_LENGTH_MISMATCH);
    }

    return 0;
}

/*
 * Use EC parameters to initialise an EC group
 *
 * ECParameters ::= CHOICE {
 *   namedCurve         OBJECT IDENTIFIER
 *   specifiedCurve     SpecifiedECDomain -- = SEQUENCE { ... }
 *   -- implicitCurve   NULL
 */
static int pk_use_ecparams(const mbedtls_asn1_buf *params, mbedtls_pk_context *pk)
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    mbedtls_ecp_group_id grp_id;

    if (params->tag == MBEDTLS_ASN1_OID) {
        if (mbedtls_oid_get_ec_grp(params, &grp_id) != 0) {
            return MBEDTLS_ERR_PK_UNKNOWN_NAMED_CURVE;
        }
    } else {
        ret = pk_ecc_group_id_from_specified(params, &grp_id);
        if (ret != 0) {
            return ret;
        }
    }

    return pk_ecc_set_group(pk, grp_id);
}

#if defined(MBEDTLS_PK_HAVE_RFC8410_CURVES)

/*
 * Load an RFC8410 EC key, which doesn't have any parameters
 */
static int pk_use_ecparams_rfc8410(const mbedtls_asn1_buf *params,
                                   mbedtls_ecp_group_id grp_id,
                                   mbedtls_pk_context *pk)
{
    if (params->tag != 0 || params->len != 0) {
        return MBEDTLS_ERR_PK_KEY_INVALID_FORMAT;
    }

    return pk_ecc_set_group(pk, grp_id);
}

/*
 * Parse an RFC 8410 encoded private EC key
 *
 * CurvePrivateKey ::= OCTET STRING
 */
static int pk_parse_key_rfc8410_der(mbedtls_pk_context *pk,
                                    unsigned char *key, size_t keylen, const unsigned char *end,
                                    int (*f_rng)(void *, unsigned char *, size_t), void *p_rng)
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    size_t len;

    if ((ret = mbedtls_asn1_get_tag(&key, (key + keylen), &len, MBEDTLS_ASN1_OCTET_STRING)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    if (key + len != end) {
        return MBEDTLS_ERR_PK_KEY_INVALID_FORMAT;
    }

    /*
     * Load the private key
     */
    ret = pk_ecc_set_key(pk, key, len);
    if (ret != 0) {
        return ret;
    }

    /* pk_parse_key_pkcs8_unencrypted_der() only supports version 1 PKCS8 keys,
     * which never contain a public key. As such, derive the public key
     * unconditionally. */
    if ((ret = pk_ecc_set_pubkey_from_prv(pk, key, len, f_rng, p_rng)) != 0) {
        return ret;
    }

    return 0;
}
#endif /* MBEDTLS_PK_HAVE_RFC8410_CURVES */

#endif /* MBEDTLS_PK_HAVE_ECC_KEYS */

#if defined(MBEDTLS_RSA_C)
/*
 *  RSAPublicKey ::= SEQUENCE {
 *      modulus           INTEGER,  -- n
 *      publicExponent    INTEGER   -- e
 *  }
 */
static int pk_get_rsapubkey(unsigned char **p,
                            const unsigned char *end,
                            mbedtls_rsa_context *rsa)
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    size_t len;

    if ((ret = mbedtls_asn1_get_tag(p, end, &len,
                                    MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_INVALID_PUBKEY, ret);
    }

    if (*p + len != end) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_INVALID_PUBKEY,
                                 MBEDTLS_ERR_ASN1_LENGTH_MISMATCH);
    }

    /* Import N */
    if ((ret = mbedtls_asn1_get_tag(p, end, &len, MBEDTLS_ASN1_INTEGER)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_INVALID_PUBKEY, ret);
    }

    if ((ret = mbedtls_rsa_import_raw(rsa, *p, len, NULL, 0, NULL, 0,
                                      NULL, 0, NULL, 0)) != 0) {
        return MBEDTLS_ERR_PK_INVALID_PUBKEY;
    }

    *p += len;

    /* Import E */
    if ((ret = mbedtls_asn1_get_tag(p, end, &len, MBEDTLS_ASN1_INTEGER)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_INVALID_PUBKEY, ret);
    }

    if ((ret = mbedtls_rsa_import_raw(rsa, NULL, 0, NULL, 0, NULL, 0,
                                      NULL, 0, *p, len)) != 0) {
        return MBEDTLS_ERR_PK_INVALID_PUBKEY;
    }

    *p += len;

    if (mbedtls_rsa_complete(rsa) != 0 ||
        mbedtls_rsa_check_pubkey(rsa) != 0) {
        return MBEDTLS_ERR_PK_INVALID_PUBKEY;
    }

    if (*p != end) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_INVALID_PUBKEY,
                                 MBEDTLS_ERR_ASN1_LENGTH_MISMATCH);
    }

    return 0;
}
#endif /* MBEDTLS_RSA_C */

/* Get a PK algorithm identifier
 *
 *  AlgorithmIdentifier  ::=  SEQUENCE  {
 *       algorithm               OBJECT IDENTIFIER,
 *       parameters              ANY DEFINED BY algorithm OPTIONAL  }
 */
static int pk_get_pk_alg(unsigned char **p,
                         const unsigned char *end,
                         mbedtls_pk_type_t *pk_alg, mbedtls_asn1_buf *params,
                         mbedtls_ecp_group_id *ec_grp_id)
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    mbedtls_asn1_buf alg_oid;

    memset(params, 0, sizeof(mbedtls_asn1_buf));

    if ((ret = mbedtls_asn1_get_alg(p, end, &alg_oid, params)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_INVALID_ALG, ret);
    }

    ret = mbedtls_oid_get_pk_alg(&alg_oid, pk_alg);
#if defined(MBEDTLS_PK_HAVE_ECC_KEYS)
    if (ret == MBEDTLS_ERR_OID_NOT_FOUND) {
        ret = mbedtls_oid_get_ec_grp_algid(&alg_oid, ec_grp_id);
        if (ret == 0) {
            *pk_alg = MBEDTLS_PK_ECKEY;
        }
    }
#else
    (void) ec_grp_id;
#endif
    if (ret != 0) {
        return MBEDTLS_ERR_PK_UNKNOWN_PK_ALG;
    }

    /*
     * No parameters with RSA (only for EC)
     */
    if (*pk_alg == MBEDTLS_PK_RSA &&
        ((params->tag != MBEDTLS_ASN1_NULL && params->tag != 0) ||
         params->len != 0)) {
        return MBEDTLS_ERR_PK_INVALID_ALG;
    }

    return 0;
}

/* Helper for Montgomery curves */
#if defined(MBEDTLS_PK_HAVE_RFC8410_CURVES)
#define MBEDTLS_PK_IS_RFC8410_GROUP_ID(id)  \
    ((id == MBEDTLS_ECP_DP_CURVE25519) || (id == MBEDTLS_ECP_DP_CURVE448))
#endif /* MBEDTLS_PK_HAVE_RFC8410_CURVES */

/*
 *  SubjectPublicKeyInfo  ::=  SEQUENCE  {
 *       algorithm            AlgorithmIdentifier,
 *       subjectPublicKey     BIT STRING }
 */
int mbedtls_pk_parse_subpubkey(unsigned char **p, const unsigned char *end,
                               mbedtls_pk_context *pk)
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    size_t len;
    mbedtls_asn1_buf alg_params;
    mbedtls_pk_type_t pk_alg = MBEDTLS_PK_NONE;
    mbedtls_ecp_group_id ec_grp_id = MBEDTLS_ECP_DP_NONE;
    const mbedtls_pk_info_t *pk_info;

    if ((ret = mbedtls_asn1_get_tag(p, end, &len,
                                    MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    end = *p + len;

    if ((ret = pk_get_pk_alg(p, end, &pk_alg, &alg_params, &ec_grp_id)) != 0) {
        return ret;
    }

    if ((ret = mbedtls_asn1_get_bitstring_null(p, end, &len)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_INVALID_PUBKEY, ret);
    }

    if (*p + len != end) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_INVALID_PUBKEY,
                                 MBEDTLS_ERR_ASN1_LENGTH_MISMATCH);
    }

    if ((pk_info = mbedtls_pk_info_from_type(pk_alg)) == NULL) {
        return MBEDTLS_ERR_PK_UNKNOWN_PK_ALG;
    }

    if ((ret = mbedtls_pk_setup(pk, pk_info)) != 0) {
        return ret;
    }

#if defined(MBEDTLS_RSA_C)
    if (pk_alg == MBEDTLS_PK_RSA) {
        ret = pk_get_rsapubkey(p, end, mbedtls_pk_rsa(*pk));
    } else
#endif /* MBEDTLS_RSA_C */
#if defined(MBEDTLS_PK_HAVE_ECC_KEYS)
    if (pk_alg == MBEDTLS_PK_ECKEY_DH || pk_alg == MBEDTLS_PK_ECKEY) {
#if defined(MBEDTLS_PK_HAVE_RFC8410_CURVES)
        if (MBEDTLS_PK_IS_RFC8410_GROUP_ID(ec_grp_id)) {
            ret = pk_use_ecparams_rfc8410(&alg_params, ec_grp_id, pk);
        } else
#endif
        {
            ret = pk_use_ecparams(&alg_params, pk);
        }
        if (ret == 0) {
            ret = pk_ecc_set_pubkey(pk, *p, end - *p);
            *p += end - *p;
        }
    } else
#endif /* MBEDTLS_PK_HAVE_ECC_KEYS */
    ret = MBEDTLS_ERR_PK_UNKNOWN_PK_ALG;

    if (ret == 0 && *p != end) {
        ret = MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_INVALID_PUBKEY,
                                MBEDTLS_ERR_ASN1_LENGTH_MISMATCH);
    }

    if (ret != 0) {
        mbedtls_pk_free(pk);
    }

    return ret;
}

#if defined(MBEDTLS_RSA_C)
/*
 * Wrapper around mbedtls_asn1_get_mpi() that rejects zero.
 *
 * The value zero is:
 * - never a valid value for an RSA parameter
 * - interpreted as "omitted, please reconstruct" by mbedtls_rsa_complete().
 *
 * Since values can't be omitted in PKCS#1, passing a zero value to
 * rsa_complete() would be incorrect, so reject zero values early.
 */
static int asn1_get_nonzero_mpi(unsigned char **p,
                                const unsigned char *end,
                                mbedtls_mpi *X)
{
    int ret;

    ret = mbedtls_asn1_get_mpi(p, end, X);
    if (ret != 0) {
        return ret;
    }

    if (mbedtls_mpi_cmp_int(X, 0) == 0) {
        return MBEDTLS_ERR_PK_KEY_INVALID_FORMAT;
    }

    return 0;
}

/*
 * Parse a PKCS#1 encoded private RSA key
 */
static int pk_parse_key_pkcs1_der(mbedtls_rsa_context *rsa,
                                  const unsigned char *key,
                                  size_t keylen)
{
    int ret, version;
    size_t len;
    unsigned char *p, *end;

    mbedtls_mpi T;
    mbedtls_mpi_init(&T);

    p = (unsigned char *) key;
    end = p + keylen;

    /*
     * This function parses the RSAPrivateKey (PKCS#1)
     *
     *  RSAPrivateKey ::= SEQUENCE {
     *      version           Version,
     *      modulus           INTEGER,  -- n
     *      publicExponent    INTEGER,  -- e
     *      privateExponent   INTEGER,  -- d
     *      prime1            INTEGER,  -- p
     *      prime2            INTEGER,  -- q
     *      exponent1         INTEGER,  -- d mod (p-1)
     *      exponent2         INTEGER,  -- d mod (q-1)
     *      coefficient       INTEGER,  -- (inverse of q) mod p
     *      otherPrimeInfos   OtherPrimeInfos OPTIONAL
     *  }
     */
    if ((ret = mbedtls_asn1_get_tag(&p, end, &len,
                                    MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    end = p + len;

    if ((ret = mbedtls_asn1_get_int(&p, end, &version)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    if (version != 0) {
        return MBEDTLS_ERR_PK_KEY_INVALID_VERSION;
    }

    /* Import N */
    if ((ret = asn1_get_nonzero_mpi(&p, end, &T)) != 0 ||
        (ret = mbedtls_rsa_import(rsa, &T, NULL, NULL,
                                  NULL, NULL)) != 0) {
        goto cleanup;
    }

    /* Import E */
    if ((ret = asn1_get_nonzero_mpi(&p, end, &T)) != 0 ||
        (ret = mbedtls_rsa_import(rsa, NULL, NULL, NULL,
                                  NULL, &T)) != 0) {
        goto cleanup;
    }

    /* Import D */
    if ((ret = asn1_get_nonzero_mpi(&p, end, &T)) != 0 ||
        (ret = mbedtls_rsa_import(rsa, NULL, NULL, NULL,
                                  &T, NULL)) != 0) {
        goto cleanup;
    }

    /* Import P */
    if ((ret = asn1_get_nonzero_mpi(&p, end, &T)) != 0 ||
        (ret = mbedtls_rsa_import(rsa, NULL, &T, NULL,
                                  NULL, NULL)) != 0) {
        goto cleanup;
    }

    /* Import Q */
    if ((ret = asn1_get_nonzero_mpi(&p, end, &T)) != 0 ||
        (ret = mbedtls_rsa_import(rsa, NULL, NULL, &T,
                                  NULL, NULL)) != 0) {
        goto cleanup;
    }

#if !defined(MBEDTLS_RSA_NO_CRT) && !defined(MBEDTLS_RSA_ALT)
    /*
     * The RSA CRT parameters DP, DQ and QP are nominally redundant, in
     * that they can be easily recomputed from D, P and Q. However by
     * parsing them from the PKCS1 structure it is possible to avoid
     * recalculating them which both reduces the overhead of loading
     * RSA private keys into memory and also avoids side channels which
     * can arise when computing those values, since all of D, P, and Q
     * are secret. See https://eprint.iacr.org/2020/055 for a
     * description of one such attack.
     */

    /* Import DP */
    if ((ret = asn1_get_nonzero_mpi(&p, end, &T)) != 0 ||
        (ret = mbedtls_mpi_copy(&rsa->DP, &T)) != 0) {
        goto cleanup;
    }

    /* Import DQ */
    if ((ret = asn1_get_nonzero_mpi(&p, end, &T)) != 0 ||
        (ret = mbedtls_mpi_copy(&rsa->DQ, &T)) != 0) {
        goto cleanup;
    }

    /* Import QP */
    if ((ret = asn1_get_nonzero_mpi(&p, end, &T)) != 0 ||
        (ret = mbedtls_mpi_copy(&rsa->QP, &T)) != 0) {
        goto cleanup;
    }

#else
    /* Verify existence of the CRT params */
    if ((ret = asn1_get_nonzero_mpi(&p, end, &T)) != 0 ||
        (ret = asn1_get_nonzero_mpi(&p, end, &T)) != 0 ||
        (ret = asn1_get_nonzero_mpi(&p, end, &T)) != 0) {
        goto cleanup;
    }
#endif

    /* rsa_complete() doesn't complete anything with the default
     * implementation but is still called:
     * - for the benefit of alternative implementation that may want to
     *   pre-compute stuff beyond what's provided (eg Montgomery factors)
     * - as is also sanity-checks the key
     *
     * Furthermore, we also check the public part for consistency with
     * mbedtls_pk_parse_pubkey(), as it includes size minima for example.
     */
    if ((ret = mbedtls_rsa_complete(rsa)) != 0 ||
        (ret = mbedtls_rsa_check_pubkey(rsa)) != 0) {
        goto cleanup;
    }

    if (p != end) {
        ret = MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT,
                                MBEDTLS_ERR_ASN1_LENGTH_MISMATCH);
    }

cleanup:

    mbedtls_mpi_free(&T);

    if (ret != 0) {
        /* Wrap error code if it's coming from a lower level */
        if ((ret & 0xff80) == 0) {
            ret = MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
        } else {
            ret = MBEDTLS_ERR_PK_KEY_INVALID_FORMAT;
        }

        mbedtls_rsa_free(rsa);
    }

    return ret;
}
#endif /* MBEDTLS_RSA_C */

#if defined(MBEDTLS_PK_HAVE_ECC_KEYS)
/*
 * Parse a SEC1 encoded private EC key
 */
static int pk_parse_key_sec1_der(mbedtls_pk_context *pk,
                                 const unsigned char *key, size_t keylen,
                                 int (*f_rng)(void *, unsigned char *, size_t), void *p_rng)
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    int version, pubkey_done;
    size_t len, d_len;
    mbedtls_asn1_buf params = { 0, 0, NULL };
    unsigned char *p = (unsigned char *) key;
    unsigned char *d;
    unsigned char *end = p + keylen;
    unsigned char *end2;

    /*
     * RFC 5915, or SEC1 Appendix C.4
     *
     * ECPrivateKey ::= SEQUENCE {
     *      version        INTEGER { ecPrivkeyVer1(1) } (ecPrivkeyVer1),
     *      privateKey     OCTET STRING,
     *      parameters [0] ECParameters {{ NamedCurve }} OPTIONAL,
     *      publicKey  [1] BIT STRING OPTIONAL
     *    }
     */
    if ((ret = mbedtls_asn1_get_tag(&p, end, &len,
                                    MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    end = p + len;

    if ((ret = mbedtls_asn1_get_int(&p, end, &version)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    if (version != 1) {
        return MBEDTLS_ERR_PK_KEY_INVALID_VERSION;
    }

    if ((ret = mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_OCTET_STRING)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    /* Keep a reference to the position fo the private key. It will be used
     * later in this function. */
    d = p;
    d_len = len;

    p += len;

    pubkey_done = 0;
    if (p != end) {
        /*
         * Is 'parameters' present?
         */
        if ((ret = mbedtls_asn1_get_tag(&p, end, &len,
                                        MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED |
                                        0)) == 0) {
            if ((ret = pk_get_ecparams(&p, p + len, &params)) != 0 ||
                (ret = pk_use_ecparams(&params, pk)) != 0) {
                return ret;
            }
        } else if (ret != MBEDTLS_ERR_ASN1_UNEXPECTED_TAG) {
            return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
        }
    }

    /*
     * Load the private key
     */
    ret = pk_ecc_set_key(pk, d, d_len);
    if (ret != 0) {
        return ret;
    }

    if (p != end) {
        /*
         * Is 'publickey' present? If not, or if we can't read it (eg because it
         * is compressed), create it from the private key.
         */
        if ((ret = mbedtls_asn1_get_tag(&p, end, &len,
                                        MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED |
                                        1)) == 0) {
            end2 = p + len;

            if ((ret = mbedtls_asn1_get_bitstring_null(&p, end2, &len)) != 0) {
                return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
            }

            if (p + len != end2) {
                return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT,
                                         MBEDTLS_ERR_ASN1_LENGTH_MISMATCH);
            }

            if ((ret = pk_ecc_set_pubkey(pk, p, end2 - p)) == 0) {
                pubkey_done = 1;
            } else {
                /*
                 * The only acceptable failure mode of pk_ecc_set_pubkey() above
                 * is if the point format is not recognized.
                 */
                if (ret != MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE) {
                    return MBEDTLS_ERR_PK_KEY_INVALID_FORMAT;
                }
            }
        } else if (ret != MBEDTLS_ERR_ASN1_UNEXPECTED_TAG) {
            return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
        }
    }

    if (!pubkey_done) {
        if ((ret = pk_ecc_set_pubkey_from_prv(pk, d, d_len, f_rng, p_rng)) != 0) {
            return ret;
        }
    }

    return 0;
}
#endif /* MBEDTLS_PK_HAVE_ECC_KEYS */

/***********************************************************************
 *
 *      PKCS#8 parsing functions
 *
 **********************************************************************/

/*
 * Parse an unencrypted PKCS#8 encoded private key
 *
 * Notes:
 *
 * - This function does not own the key buffer. It is the
 *   responsibility of the caller to take care of zeroizing
 *   and freeing it after use.
 *
 * - The function is responsible for freeing the provided
 *   PK context on failure.
 *
 */
static int pk_parse_key_pkcs8_unencrypted_der(
    mbedtls_pk_context *pk,
    const unsigned char *key, size_t keylen,
    int (*f_rng)(void *, unsigned char *, size_t), void *p_rng)
{
    int ret, version;
    size_t len;
    mbedtls_asn1_buf params;
    unsigned char *p = (unsigned char *) key;
    unsigned char *end = p + keylen;
    mbedtls_pk_type_t pk_alg = MBEDTLS_PK_NONE;
    mbedtls_ecp_group_id ec_grp_id = MBEDTLS_ECP_DP_NONE;
    const mbedtls_pk_info_t *pk_info;

#if !defined(MBEDTLS_PK_HAVE_ECC_KEYS)
    (void) f_rng;
    (void) p_rng;
#endif

    /*
     * This function parses the PrivateKeyInfo object (PKCS#8 v1.2 = RFC 5208)
     *
     *    PrivateKeyInfo ::= SEQUENCE {
     *      version                   Version,
     *      privateKeyAlgorithm       PrivateKeyAlgorithmIdentifier,
     *      privateKey                PrivateKey,
     *      attributes           [0]  IMPLICIT Attributes OPTIONAL }
     *
     *    Version ::= INTEGER
     *    PrivateKeyAlgorithmIdentifier ::= AlgorithmIdentifier
     *    PrivateKey ::= OCTET STRING
     *
     *  The PrivateKey OCTET STRING is a SEC1 ECPrivateKey
     */

    if ((ret = mbedtls_asn1_get_tag(&p, end, &len,
                                    MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    end = p + len;

    if ((ret = mbedtls_asn1_get_int(&p, end, &version)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    if (version != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_VERSION, ret);
    }

    if ((ret = pk_get_pk_alg(&p, end, &pk_alg, &params, &ec_grp_id)) != 0) {
        return ret;
    }

    if ((ret = mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_OCTET_STRING)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    if (len < 1) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT,
                                 MBEDTLS_ERR_ASN1_OUT_OF_DATA);
    }

    if ((pk_info = mbedtls_pk_info_from_type(pk_alg)) == NULL) {
        return MBEDTLS_ERR_PK_UNKNOWN_PK_ALG;
    }

    if ((ret = mbedtls_pk_setup(pk, pk_info)) != 0) {
        return ret;
    }

#if defined(MBEDTLS_RSA_C)
    if (pk_alg == MBEDTLS_PK_RSA) {
        if ((ret = pk_parse_key_pkcs1_der(mbedtls_pk_rsa(*pk), p, len)) != 0) {
            mbedtls_pk_free(pk);
            return ret;
        }
    } else
#endif /* MBEDTLS_RSA_C */
#if defined(MBEDTLS_PK_HAVE_ECC_KEYS)
    if (pk_alg == MBEDTLS_PK_ECKEY || pk_alg == MBEDTLS_PK_ECKEY_DH) {
#if defined(MBEDTLS_PK_HAVE_RFC8410_CURVES)
        if (MBEDTLS_PK_IS_RFC8410_GROUP_ID(ec_grp_id)) {
            if ((ret =
                     pk_use_ecparams_rfc8410(&params, ec_grp_id, pk)) != 0 ||
                (ret =
                     pk_parse_key_rfc8410_der(pk, p, len, end, f_rng,
                                              p_rng)) != 0) {
                mbedtls_pk_free(pk);
                return ret;
            }
        } else
#endif
        {
            if ((ret = pk_use_ecparams(&params, pk)) != 0 ||
                (ret = pk_parse_key_sec1_der(pk, p, len, f_rng, p_rng)) != 0) {
                mbedtls_pk_free(pk);
                return ret;
            }
        }
    } else
#endif /* MBEDTLS_PK_HAVE_ECC_KEYS */
    return MBEDTLS_ERR_PK_UNKNOWN_PK_ALG;

    end = p + len;
    if (end != (key + keylen)) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT,
                                 MBEDTLS_ERR_ASN1_LENGTH_MISMATCH);
    }

    return 0;
}

/*
 * Parse an encrypted PKCS#8 encoded private key
 *
 * To save space, the decryption happens in-place on the given key buffer.
 * Also, while this function may modify the keybuffer, it doesn't own it,
 * and instead it is the responsibility of the caller to zeroize and properly
 * free it after use.
 *
 */
#if defined(MBEDTLS_PKCS12_C) || defined(MBEDTLS_PKCS5_C)
MBEDTLS_STATIC_TESTABLE int mbedtls_pk_parse_key_pkcs8_encrypted_der(
    mbedtls_pk_context *pk,
    unsigned char *key, size_t keylen,
    const unsigned char *pwd, size_t pwdlen,
    int (*f_rng)(void *, unsigned char *, size_t), void *p_rng)
{
    int ret, decrypted = 0;
    size_t len;
    unsigned char *buf;
    unsigned char *p, *end;
    mbedtls_asn1_buf pbe_alg_oid, pbe_params;
#if defined(MBEDTLS_PKCS12_C)
    mbedtls_cipher_type_t cipher_alg;
    mbedtls_md_type_t md_alg;
#endif
    size_t outlen = 0;

    p = key;
    end = p + keylen;

    if (pwdlen == 0) {
        return MBEDTLS_ERR_PK_PASSWORD_REQUIRED;
    }

    /*
     * This function parses the EncryptedPrivateKeyInfo object (PKCS#8)
     *
     *  EncryptedPrivateKeyInfo ::= SEQUENCE {
     *    encryptionAlgorithm  EncryptionAlgorithmIdentifier,
     *    encryptedData        EncryptedData
     *  }
     *
     *  EncryptionAlgorithmIdentifier ::= AlgorithmIdentifier
     *
     *  EncryptedData ::= OCTET STRING
     *
     *  The EncryptedData OCTET STRING is a PKCS#8 PrivateKeyInfo
     *
     */
    if ((ret = mbedtls_asn1_get_tag(&p, end, &len,
                                    MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    end = p + len;

    if ((ret = mbedtls_asn1_get_alg(&p, end, &pbe_alg_oid, &pbe_params)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    if ((ret = mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_OCTET_STRING)) != 0) {
        return MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_KEY_INVALID_FORMAT, ret);
    }

    buf = p;

    /*
     * Decrypt EncryptedData with appropriate PBE
     */
#if defined(MBEDTLS_PKCS12_C)
    if (mbedtls_oid_get_pkcs12_pbe_alg(&pbe_alg_oid, &md_alg, &cipher_alg) == 0) {
        if ((ret = mbedtls_pkcs12_pbe_ext(&pbe_params, MBEDTLS_PKCS12_PBE_DECRYPT,
                                          cipher_alg, md_alg,
                                          pwd, pwdlen, p, len, buf, len, &outlen)) != 0) {
            if (ret == MBEDTLS_ERR_PKCS12_PASSWORD_MISMATCH) {
                return MBEDTLS_ERR_PK_PASSWORD_MISMATCH;
            }

            return ret;
        }

        decrypted = 1;
    } else
#endif /* MBEDTLS_PKCS12_C */
#if defined(MBEDTLS_PKCS5_C)
    if (MBEDTLS_OID_CMP(MBEDTLS_OID_PKCS5_PBES2, &pbe_alg_oid) == 0) {
        if ((ret = mbedtls_pkcs5_pbes2_ext(&pbe_params, MBEDTLS_PKCS5_DECRYPT, pwd, pwdlen,
                                           p, len, buf, len, &outlen)) != 0) {
            if (ret == MBEDTLS_ERR_PKCS5_PASSWORD_MISMATCH) {
                return MBEDTLS_ERR_PK_PASSWORD_MISMATCH;
            }

            return ret;
        }

        decrypted = 1;
    } else
#endif /* MBEDTLS_PKCS5_C */
    {
        ((void) pwd);
    }

    if (decrypted == 0) {
        return MBEDTLS_ERR_PK_FEATURE_UNAVAILABLE;
    }
    return pk_parse_key_pkcs8_unencrypted_der(pk, buf, outlen, f_rng, p_rng);
}
#endif /* MBEDTLS_PKCS12_C || MBEDTLS_PKCS5_C */

/***********************************************************************
 *
 *      Top-level functions, with format auto-discovery
 *
 **********************************************************************/

/*
 * Parse a private key
 */
int mbedtls_pk_parse_key(mbedtls_pk_context *pk,
                         const unsigned char *key, size_t keylen,
                         const unsigned char *pwd, size_t pwdlen,
                         int (*f_rng)(void *, unsigned char *, size_t), void *p_rng)
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    const mbedtls_pk_info_t *pk_info;
#if defined(MBEDTLS_PEM_PARSE_C)
    size_t len;
    mbedtls_pem_context pem;
#endif

    if (keylen == 0) {
        return MBEDTLS_ERR_PK_KEY_INVALID_FORMAT;
    }

#if defined(MBEDTLS_PEM_PARSE_C)
    mbedtls_pem_init(&pem);

#if defined(MBEDTLS_RSA_C)
    /* Avoid calling mbedtls_pem_read_buffer() on non-null-terminated string */
    if (key[keylen - 1] != '\0') {
        ret = MBEDTLS_ERR_PEM_NO_HEADER_FOOTER_PRESENT;
    } else {
        ret = mbedtls_pem_read_buffer(&pem,
                                      "-----BEGIN RSA PRIVATE KEY-----",
                                      "-----END RSA PRIVATE KEY-----",
                                      key, pwd, pwdlen, &len);
    }

    if (ret == 0) {
        pk_info = mbedtls_pk_info_from_type(MBEDTLS_PK_RSA);
        if ((ret = mbedtls_pk_setup(pk, pk_info)) != 0 ||
            (ret = pk_parse_key_pkcs1_der(mbedtls_pk_rsa(*pk),
                                          pem.buf, pem.buflen)) != 0) {
            mbedtls_pk_free(pk);
        }

        mbedtls_pem_free(&pem);
        return ret;
    } else if (ret == MBEDTLS_ERR_PEM_PASSWORD_MISMATCH) {
        return MBEDTLS_ERR_PK_PASSWORD_MISMATCH;
    } else if (ret == MBEDTLS_ERR_PEM_PASSWORD_REQUIRED) {
        return MBEDTLS_ERR_PK_PASSWORD_REQUIRED;
    } else if (ret != MBEDTLS_ERR_PEM_NO_HEADER_FOOTER_PRESENT) {
        return ret;
    }
#endif /* MBEDTLS_RSA_C */

#if defined(MBEDTLS_PK_HAVE_ECC_KEYS)
    /* Avoid calling mbedtls_pem_read_buffer() on non-null-terminated string */
    if (key[keylen - 1] != '\0') {
        ret = MBEDTLS_ERR_PEM_NO_HEADER_FOOTER_PRESENT;
    } else {
        ret = mbedtls_pem_read_buffer(&pem,
                                      "-----BEGIN EC PRIVATE KEY-----",
                                      "-----END EC PRIVATE KEY-----",
                                      key, pwd, pwdlen, &len);
    }
    if (ret == 0) {
        pk_info = mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY);

        if ((ret = mbedtls_pk_setup(pk, pk_info)) != 0 ||
            (ret = pk_parse_key_sec1_der(pk,
                                         pem.buf, pem.buflen,
                                         f_rng, p_rng)) != 0) {
            mbedtls_pk_free(pk);
        }

        mbedtls_pem_free(&pem);
        return ret;
    } else if (ret == MBEDTLS_ERR_PEM_PASSWORD_MISMATCH) {
        return MBEDTLS_ERR_PK_PASSWORD_MISMATCH;
    } else if (ret == MBEDTLS_ERR_PEM_PASSWORD_REQUIRED) {
        return MBEDTLS_ERR_PK_PASSWORD_REQUIRED;
    } else if (ret != MBEDTLS_ERR_PEM_NO_HEADER_FOOTER_PRESENT) {
        return ret;
    }
#endif /* MBEDTLS_PK_HAVE_ECC_KEYS */

    /* Avoid calling mbedtls_pem_read_buffer() on non-null-terminated string */
    if (key[keylen - 1] != '\0') {
        ret = MBEDTLS_ERR_PEM_NO_HEADER_FOOTER_PRESENT;
    } else {
        ret = mbedtls_pem_read_buffer(&pem,
                                      "-----BEGIN PRIVATE KEY-----",
                                      "-----END PRIVATE KEY-----",
                                      key, NULL, 0, &len);
    }
    if (ret == 0) {
        if ((ret = pk_parse_key_pkcs8_unencrypted_der(pk,
                                                      pem.buf, pem.buflen, f_rng, p_rng)) != 0) {
            mbedtls_pk_free(pk);
        }

        mbedtls_pem_free(&pem);
        return ret;
    } else if (ret != MBEDTLS_ERR_PEM_NO_HEADER_FOOTER_PRESENT) {
        return ret;
    }

#if defined(MBEDTLS_PKCS12_C) || defined(MBEDTLS_PKCS5_C)
    /* Avoid calling mbedtls_pem_read_buffer() on non-null-terminated string */
    if (key[keylen - 1] != '\0') {
        ret = MBEDTLS_ERR_PEM_NO_HEADER_FOOTER_PRESENT;
    } else {
        ret = mbedtls_pem_read_buffer(&pem,
                                      "-----BEGIN ENCRYPTED PRIVATE KEY-----",
                                      "-----END ENCRYPTED PRIVATE KEY-----",
                                      key, NULL, 0, &len);
    }
    if (ret == 0) {
        if ((ret = mbedtls_pk_parse_key_pkcs8_encrypted_der(pk, pem.buf, pem.buflen,
                                                            pwd, pwdlen, f_rng, p_rng)) != 0) {
            mbedtls_pk_free(pk);
        }

        mbedtls_pem_free(&pem);
        return ret;
    } else if (ret != MBEDTLS_ERR_PEM_NO_HEADER_FOOTER_PRESENT) {
        return ret;
    }
#endif /* MBEDTLS_PKCS12_C || MBEDTLS_PKCS5_C */
#else
    ((void) pwd);
    ((void) pwdlen);
#endif /* MBEDTLS_PEM_PARSE_C */

    /*
     * At this point we only know it's not a PEM formatted key. Could be any
     * of the known DER encoded private key formats
     *
     * We try the different DER format parsers to see if one passes without
     * error
     */
#if defined(MBEDTLS_PKCS12_C) || defined(MBEDTLS_PKCS5_C)
    if (pwdlen != 0) {
        unsigned char *key_copy;

        if ((key_copy = mbedtls_calloc(1, keylen)) == NULL) {
            return MBEDTLS_ERR_PK_ALLOC_FAILED;
        }

        memcpy(key_copy, key, keylen);

        ret = mbedtls_pk_parse_key_pkcs8_encrypted_der(pk, key_copy, keylen,
                                                       pwd, pwdlen, f_rng, p_rng);

        mbedtls_zeroize_and_free(key_copy, keylen);
    }

    if (ret == 0) {
        return 0;
    }

    mbedtls_pk_free(pk);
    mbedtls_pk_init(pk);

    if (ret == MBEDTLS_ERR_PK_PASSWORD_MISMATCH) {
        return ret;
    }
#endif /* MBEDTLS_PKCS12_C || MBEDTLS_PKCS5_C */

    ret = pk_parse_key_pkcs8_unencrypted_der(pk, key, keylen, f_rng, p_rng);
    if (ret == 0) {
        return 0;
    }

    mbedtls_pk_free(pk);
    mbedtls_pk_init(pk);

#if defined(MBEDTLS_RSA_C)

    pk_info = mbedtls_pk_info_from_type(MBEDTLS_PK_RSA);
    if (mbedtls_pk_setup(pk, pk_info) == 0 &&
        pk_parse_key_pkcs1_der(mbedtls_pk_rsa(*pk), key, keylen) == 0) {
        return 0;
    }

    mbedtls_pk_free(pk);
    mbedtls_pk_init(pk);
#endif /* MBEDTLS_RSA_C */

#if defined(MBEDTLS_PK_HAVE_ECC_KEYS)
    pk_info = mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY);
    if (mbedtls_pk_setup(pk, pk_info) == 0 &&
        pk_parse_key_sec1_der(pk,
                              key, keylen, f_rng, p_rng) == 0) {
        return 0;
    }
    mbedtls_pk_free(pk);
#endif /* MBEDTLS_PK_HAVE_ECC_KEYS */

    /* If MBEDTLS_RSA_C is defined but MBEDTLS_PK_HAVE_ECC_KEYS isn't,
     * it is ok to leave the PK context initialized but not
     * freed: It is the caller's responsibility to call pk_init()
     * before calling this function, and to call pk_free()
     * when it fails. If MBEDTLS_PK_HAVE_ECC_KEYS is defined but MBEDTLS_RSA_C
     * isn't, this leads to mbedtls_pk_free() being called
     * twice, once here and once by the caller, but this is
     * also ok and in line with the mbedtls_pk_free() calls
     * on failed PEM parsing attempts. */

    return MBEDTLS_ERR_PK_KEY_INVALID_FORMAT;
}

/*
 * Parse a public key
 */
int mbedtls_pk_parse_public_key(mbedtls_pk_context *ctx,
                                const unsigned char *key, size_t keylen)
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    unsigned char *p;
#if defined(MBEDTLS_RSA_C)
    const mbedtls_pk_info_t *pk_info;
#endif
#if defined(MBEDTLS_PEM_PARSE_C)
    size_t len;
    mbedtls_pem_context pem;
#endif

    if (keylen == 0) {
        return MBEDTLS_ERR_PK_KEY_INVALID_FORMAT;
    }

#if defined(MBEDTLS_PEM_PARSE_C)
    mbedtls_pem_init(&pem);
#if defined(MBEDTLS_RSA_C)
    /* Avoid calling mbedtls_pem_read_buffer() on non-null-terminated string */
    if (key[keylen - 1] != '\0') {
        ret = MBEDTLS_ERR_PEM_NO_HEADER_FOOTER_PRESENT;
    } else {
        ret = mbedtls_pem_read_buffer(&pem,
                                      "-----BEGIN RSA PUBLIC KEY-----",
                                      "-----END RSA PUBLIC KEY-----",
                                      key, NULL, 0, &len);
    }

    if (ret == 0) {
        p = pem.buf;
        if ((pk_info = mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) == NULL) {
            mbedtls_pem_free(&pem);
            return MBEDTLS_ERR_PK_UNKNOWN_PK_ALG;
        }

        if ((ret = mbedtls_pk_setup(ctx, pk_info)) != 0) {
            mbedtls_pem_free(&pem);
            return ret;
        }

        if ((ret = pk_get_rsapubkey(&p, p + pem.buflen, mbedtls_pk_rsa(*ctx))) != 0) {
            mbedtls_pk_free(ctx);
        }

        mbedtls_pem_free(&pem);
        return ret;
    } else if (ret != MBEDTLS_ERR_PEM_NO_HEADER_FOOTER_PRESENT) {
        mbedtls_pem_free(&pem);
        return ret;
    }
#endif /* MBEDTLS_RSA_C */

    /* Avoid calling mbedtls_pem_read_buffer() on non-null-terminated string */
    if (key[keylen - 1] != '\0') {
        ret = MBEDTLS_ERR_PEM_NO_HEADER_FOOTER_PRESENT;
    } else {
        ret = mbedtls_pem_read_buffer(&pem,
                                      "-----BEGIN PUBLIC KEY-----",
                                      "-----END PUBLIC KEY-----",
                                      key, NULL, 0, &len);
    }

    if (ret == 0) {
        /*
         * Was PEM encoded
         */
        p = pem.buf;

        ret = mbedtls_pk_parse_subpubkey(&p, p + pem.buflen, ctx);
        mbedtls_pem_free(&pem);
        return ret;
    } else if (ret != MBEDTLS_ERR_PEM_NO_HEADER_FOOTER_PRESENT) {
        mbedtls_pem_free(&pem);
        return ret;
    }
    mbedtls_pem_free(&pem);
#endif /* MBEDTLS_PEM_PARSE_C */

#if defined(MBEDTLS_RSA_C)
    if ((pk_info = mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) == NULL) {
        return MBEDTLS_ERR_PK_UNKNOWN_PK_ALG;
    }

    if ((ret = mbedtls_pk_setup(ctx, pk_info)) != 0) {
        return ret;
    }

    p = (unsigned char *) key;
    ret = pk_get_rsapubkey(&p, p + keylen, mbedtls_pk_rsa(*ctx));
    if (ret == 0) {
        return ret;
    }
    mbedtls_pk_free(ctx);
    if (ret != (MBEDTLS_ERROR_ADD(MBEDTLS_ERR_PK_INVALID_PUBKEY,
                                  MBEDTLS_ERR_ASN1_UNEXPECTED_TAG))) {
        return ret;
    }
#endif /* MBEDTLS_RSA_C */
    p = (unsigned char *) key;

    ret = mbedtls_pk_parse_subpubkey(&p, p + keylen, ctx);

    return ret;
}

/***********************************************************************
 *
 *      Top-level functions, with filesystem support
 *
 **********************************************************************/

#if defined(MBEDTLS_FS_IO)
/*
 * Load all data from a file into a given buffer.
 *
 * The file is expected to contain either PEM or DER encoded data.
 * A terminating null byte is always appended. It is included in the announced
 * length only if the data looks like it is PEM encoded.
 */
int mbedtls_pk_load_file(const char *path, unsigned char **buf, size_t *n)
{
    FILE *f;
    long size;

    if ((f = fopen(path, "rb")) == NULL) {
        return MBEDTLS_ERR_PK_FILE_IO_ERROR;
    }

    /* Ensure no stdio buffering of secrets, as such buffers cannot be wiped. */
    mbedtls_setbuf(f, NULL);

    fseek(f, 0, SEEK_END);
    if ((size = ftell(f)) == -1) {
        fclose(f);
        return MBEDTLS_ERR_PK_FILE_IO_ERROR;
    }
    fseek(f, 0, SEEK_SET);

    *n = (size_t) size;

    if (*n + 1 == 0 ||
        (*buf = mbedtls_calloc(1, *n + 1)) == NULL) {
        fclose(f);
        return MBEDTLS_ERR_PK_ALLOC_FAILED;
    }

    if (fread(*buf, 1, *n, f) != *n) {
        fclose(f);

        mbedtls_zeroize_and_free(*buf, *n);

        return MBEDTLS_ERR_PK_FILE_IO_ERROR;
    }

    fclose(f);

    (*buf)[*n] = '\0';

    if (strstr((const char *) *buf, "-----BEGIN ") != NULL) {
        ++*n;
    }

    return 0;
}

/*
 * Load and parse a private key
 */
int mbedtls_pk_parse_keyfile(mbedtls_pk_context *ctx,
                             const char *path, const char *pwd,
                             int (*f_rng)(void *, unsigned char *, size_t), void *p_rng)
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    size_t n;
    unsigned char *buf;

    if ((ret = mbedtls_pk_load_file(path, &buf, &n)) != 0) {
        return ret;
    }

    if (pwd == NULL) {
        ret = mbedtls_pk_parse_key(ctx, buf, n, NULL, 0, f_rng, p_rng);
    } else {
        ret = mbedtls_pk_parse_key(ctx, buf, n,
                                   (const unsigned char *) pwd, strlen(pwd), f_rng, p_rng);
    }

    mbedtls_zeroize_and_free(buf, n);

    return ret;
}

/*
 * Load and parse a public key
 */
int mbedtls_pk_parse_public_keyfile(mbedtls_pk_context *ctx, const char *path)
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    size_t n;
    unsigned char *buf;

    if ((ret = mbedtls_pk_load_file(path, &buf, &n)) != 0) {
        return ret;
    }

    ret = mbedtls_pk_parse_public_key(ctx, buf, n);

    mbedtls_zeroize_and_free(buf, n);

    return ret;
}
#endif /* MBEDTLS_FS_IO */

#endif /* MBEDTLS_PK_PARSE_C */
