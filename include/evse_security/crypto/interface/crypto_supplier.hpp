// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#pragma once

#include <optional>
#include <stdexcept>
#include <vector>

#include <evse_security/crypto/interface/crypto_types.hpp>
#include <evse_security/evse_types.hpp>
#include <evse_security/utils/evse_filesystem_types.hpp>

namespace evse_security {

/// @brief All cryptography suppliers must conform to this class. Do not
/// use this class directly, just include the evse_crypto.hpp
class AbstractCryptoSupplier {
public:
    /// @brief Loads all certificates from the string data that can contain multiple cetifs
    static std::vector<X509Handle_ptr> load_certificates(const std::string& data, const EncodingFormat encoding);

public: // X509 certificate utilities
    static std::string x509_to_string(X509Handle* handle);
    static std::string x509_get_responder_url(X509Handle* handle);
    static std::string x509_get_key_hash(X509Handle* handle);
    static std::string x509_get_serial_number(X509Handle* handle);
    static std::string x509_get_issuer_name_hash(X509Handle* handle);
    static std::string x509_get_common_name(X509Handle* handle);

    /// @brief Returns the time validity for a certificate
    /// @param out_valid_in Valid in amount of seconds. A negative value is in the past, a positive one is in the future
    /// (not yet valid)
    /// @param out_valid_to Valid amount of seconds. A negative value is in the past (expired), a positive one is in the
    /// future
    static void x509_get_validity(X509Handle* handle, std::int64_t& out_valid_in, std::int64_t& out_valid_to);

    static bool x509_is_selfsigned(X509Handle* handle);
    static bool x509_is_child(X509Handle* child, X509Handle* parent);
    static bool x509_is_equal(X509Handle* a, X509Handle* b);

    static X509Handle_ptr x509_duplicate_unique(X509Handle* handle);

    /// @brief Verifies the provided target against the certificate chain
    /// @param target       Target to verify
    /// @param parents      Parents chain, until the root
    /// @param dir_path     Optional directory path that can be used for certificate store lookup
    /// @param file_path    Optional certificate file path that can be used for certificate store lookup
    /// @return
    static CertificateValidationError x509_verify_certificate_chain(X509Handle* target,
                                                                    const std::vector<X509Handle*>& parents,
                                                                    const std::optional<fs::path> dir_path,
                                                                    const std::optional<fs::path> file_path);

    /// @brief Checks if the private key is consistent with the provided handle
    static bool x509_check_private_key(X509Handle* handle, std::string private_key,
                                       std::optional<std::string> password);

    /// @brief Verifies the signature with the certificate handle public key against the data
    static bool x509_verify_signature(X509Handle* handle, const std::vector<std::byte>& signature,
                                      const std::vector<std::byte>& data);

    /// @brief Generates a certificate signing request with the provided parameters
    static bool x509_generate_csr(const CertificateSigningRequestInfo& generation_info, std::string& out_csr);

public: // Digesting/decoding utils
    static bool digest_file_sha256(const fs::path& path, std::vector<std::byte>& out_digest);
    static bool decode_base64_signature(const std::string& signature, std::vector<std::byte>& out_decoded);
};

} // namespace evse_security