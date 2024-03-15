// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#pragma once

#include <evse_security/crypto/interface/crypto_supplier.hpp>

namespace evse_security {

class OpenSSLSupplier : public AbstractCryptoSupplier {
public:
    static const char* get_supplier_name();

    static bool supports_tpm();
    static bool supports_tpm_key_creation();

public:
    static bool generate_key(const KeyGenerationInfo& key_info, KeyHandle_ptr& out_key);

public:
    static std::vector<X509Handle_ptr> load_certificates(const std::string& data, const EncodingFormat encoding);

public:
    static std::string x509_to_string(X509Handle* handle);
    static std::string x509_get_responder_url(X509Handle* handle);
    static std::string x509_get_key_hash(X509Handle* handle);
    static std::string x509_get_serial_number(X509Handle* handle);
    static std::string x509_get_issuer_name_hash(X509Handle* handle);
    static std::string x509_get_common_name(X509Handle* handle);
    static void x509_get_validity(X509Handle* handle, std::int64_t& out_valid_in, std::int64_t& out_valid_to);
    static bool x509_is_selfsigned(X509Handle* handle);
    static bool x509_is_child(X509Handle* child, X509Handle* parent);
    static bool x509_is_equal(X509Handle* a, X509Handle* b);
    static X509Handle_ptr x509_duplicate_unique(X509Handle* handle);
    static CertificateValidationResult x509_verify_certificate_chain(X509Handle* target,
                                                                     const std::vector<X509Handle*>& parents,
                                                                     bool allow_future_certificates,
                                                                     const std::optional<fs::path> dir_path,
                                                                     const std::optional<fs::path> file_path);
    static bool x509_check_private_key(X509Handle* handle, std::string private_key,
                                       std::optional<std::string> password);
    static bool x509_verify_signature(X509Handle* handle, const std::vector<std::byte>& signature,
                                      const std::vector<std::byte>& data);

    static bool x509_generate_csr(const CertificateSigningRequestInfo& csr_info, std::string& out_csr);

public:
    static bool digest_file_sha256(const fs::path& path, std::vector<std::byte>& out_digest);
    static bool decode_base64_signature(const std::string& signature, std::vector<std::byte>& out_decoded);
};

} // namespace evse_security