#pragma once

#include <string>
#include <vector>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/opensslv.h>

// Generate a self-signed cert at runtime, different from the hardcoded test cert.
// Used in negative tests where a valid-but-wrong CA is needed.
inline std::string generate_different_self_signed_cert() {
#if OPENSSL_VERSION_NUMBER >= 0x30000000LL
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
#else
    EVP_PKEY* pkey = EVP_PKEY_new();
    auto rsa = RSA_new();
    auto bn = BN_new();
    BN_set_word(bn, RSA_F4);
    RSA_generate_key_ex(rsa, 2048, bn, nullptr);
    EVP_PKEY_assign_RSA(pkey, rsa);
    BN_free(bn);
#endif

    X509* x509 = X509_new();
    X509_set_version(x509, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 99999);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 365 * 24 * 3600);
    X509_set_pubkey(x509, pkey);

    auto name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
        (unsigned char*)"WrongCA", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        (unsigned char*)"WrongCA", -1, -1, 0);
    X509_set_issuer_name(x509, name);
    X509_sign(x509, pkey, EVP_sha256());

    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, x509);
    char* data = nullptr;
    long len = BIO_get_mem_data(bio, &data);
    std::string result(data, len);
    BIO_free(bio);
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return result;
}

// A CA plus a leaf certificate signed by it, all PEM encoded. The client loads
// `ca_pem` as its trust anchor; the server is configured with `cert_pem` and
// `key_pem`.
struct TestCertChain {
    std::string ca_pem;
    std::string cert_pem;
    std::string key_pem;
};

inline EVP_PKEY* test_generate_rsa_key() {
#if OPENSSL_VERSION_NUMBER >= 0x30000000LL
    return EVP_RSA_gen(2048);
#else
    EVP_PKEY* pkey = EVP_PKEY_new();
    auto rsa = RSA_new();
    auto bn = BN_new();
    BN_set_word(bn, RSA_F4);
    RSA_generate_key_ex(rsa, 2048, bn, nullptr);
    EVP_PKEY_assign_RSA(pkey, rsa);
    BN_free(bn);
    return pkey;
#endif
}

inline std::string test_pem_of_cert(X509* cert) {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, cert);
    char* data = nullptr;
    long len = BIO_get_mem_data(bio, &data);
    std::string result(data, len);
    BIO_free(bio);
    return result;
}

inline std::string test_pem_of_key(EVP_PKEY* pkey) {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    long len = BIO_get_mem_data(bio, &data);
    std::string result(data, len);
    BIO_free(bio);
    return result;
}

inline void test_set_cert_name(X509* cert, const char* cn) {
    auto name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
        (unsigned char*)"PhotonTest", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        (unsigned char*)cn, -1, -1, 0);
}

// Generate a CA and a leaf certificate signed by it, with `sans` as the leaf's
// subjectAltName. Entries use OpenSSL syntax: "DNS:example.com", "IP:127.0.0.1".
// An empty list produces a leaf with no SAN extension at all, so that name
// matching falls back to the common name.
//
// Each scenario gets a purpose-built certificate rather than sharing one, the
// same way curl's tests/certs are laid out; the shared cert in
// net/test/cert-key.cpp carries no SAN and cannot exercise name checking.
inline TestCertChain generate_ca_signed_cert(const std::vector<std::string>& sans,
                                             const char* cn = "photon-test",
                                             const char* ca_cn = "PhotonTestCA") {
    TestCertChain out;

    EVP_PKEY* ca_key = test_generate_rsa_key();
    X509* ca = X509_new();
    X509_set_version(ca, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(ca), 1);
    X509_gmtime_adj(X509_get_notBefore(ca), 0);
    X509_gmtime_adj(X509_get_notAfter(ca), 365 * 24 * 3600);
    X509_set_pubkey(ca, ca_key);
    test_set_cert_name(ca, ca_cn);
    X509_set_issuer_name(ca, X509_get_subject_name(ca));
    {
        // CA:TRUE is mandatory: without basicConstraints the chain is rejected
        // before any name check happens, which would mask what these tests mean
        // to exercise.
        X509V3_CTX v3ctx;
        X509V3_set_ctx_nodb(&v3ctx);
        X509V3_set_ctx(&v3ctx, ca, ca, nullptr, nullptr, 0);
        auto ext = X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_basic_constraints,
                                       (char*)"critical,CA:TRUE");
        X509_add_ext(ca, ext, -1);
        X509_EXTENSION_free(ext);
    }
    X509_sign(ca, ca_key, EVP_sha256());

    EVP_PKEY* leaf_key = test_generate_rsa_key();
    X509* leaf = X509_new();
    X509_set_version(leaf, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(leaf), 2);
    X509_gmtime_adj(X509_get_notBefore(leaf), 0);
    X509_gmtime_adj(X509_get_notAfter(leaf), 365 * 24 * 3600);
    X509_set_pubkey(leaf, leaf_key);
    test_set_cert_name(leaf, cn);
    X509_set_issuer_name(leaf, X509_get_subject_name(ca));
    {
        X509V3_CTX v3ctx;
        X509V3_set_ctx_nodb(&v3ctx);
        X509V3_set_ctx(&v3ctx, ca, leaf, nullptr, nullptr, 0);
        auto ext = X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_basic_constraints,
                                       (char*)"critical,CA:FALSE");
        X509_add_ext(leaf, ext, -1);
        X509_EXTENSION_free(ext);
        if (!sans.empty()) {
            std::string value;
            for (auto& s : sans) {
                if (!value.empty()) value += ",";
                value += s;
            }
            auto san = X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_subject_alt_name,
                                           (char*)value.c_str());
            X509_add_ext(leaf, san, -1);
            X509_EXTENSION_free(san);
        }
    }
    X509_sign(leaf, ca_key, EVP_sha256());

    out.ca_pem = test_pem_of_cert(ca);
    out.cert_pem = test_pem_of_cert(leaf);
    out.key_pem = test_pem_of_key(leaf_key);

    X509_free(leaf);
    EVP_PKEY_free(leaf_key);
    X509_free(ca);
    EVP_PKEY_free(ca_key);
    return out;
}
