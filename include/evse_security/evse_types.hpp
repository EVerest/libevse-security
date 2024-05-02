// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <evse_security/utils/evse_filesystem_types.hpp>

namespace evse_security {

enum class EncodingFormat {
    DER,
    PEM,
};

enum class CaCertificateType {
    V2G,
    MO,
    CSMS,
    MF,
};

enum class LeafCertificateType {
    CSMS,
    V2G,
    MF,
    MO,
};

enum class CertificateType {
    V2GRootCertificate,
    MORootCertificate,
    CSMSRootCertificate,
    V2GCertificateChain,
    MFRootCertificate,
};

enum class HashAlgorithm {
    SHA256,
    SHA384,
    SHA512,
};

enum class CertificateValidationResult {
    Valid,
    Expired,
    InvalidSignature,
    IssuerNotFound,
    InvalidLeafSignature,
    InvalidChain,
    Unknown,
};

enum class InstallCertificateResult {
    InvalidSignature,
    InvalidCertificateChain,
    InvalidFormat,
    InvalidCommonName,
    NoRootCertificateInstalled,
    Expired,
    CertificateStoreMaxLengthExceeded,
    WriteError,
    Accepted,
};

enum class DeleteCertificateResult {
    Accepted,
    Failed,
    NotFound,
};

enum class GetInstalledCertificatesStatus {
    Accepted,
    NotFound,
};

enum class GetCertificateInfoStatus {
    Accepted,
    Rejected,
    NotFound,
    NotFoundValid,
    PrivateKeyNotFound,
};

// types of evse_security

struct CertificateHashData {
    HashAlgorithm hash_algorithm; ///< Algorithm used for the hashes provided
    std::string issuer_name_hash; ///< The hash of the issuer's distinguished name (DN), calculated over the DER
                                  ///< encoding of the issuer's name field.
    std::string issuer_key_hash;  ///< The hash of the DER encoded public key: the value (excluding tag and length) of
                                  ///< the  subject public key field
    std::string serial_number; ///< The string representation of the hexadecimal value of the serial number without the
                               ///< prefix "0x" and without leading zeroes.

    bool operator==(const CertificateHashData& Other) const {
        return hash_algorithm == Other.hash_algorithm && issuer_name_hash == Other.issuer_name_hash &&
               issuer_key_hash == Other.issuer_key_hash && serial_number == Other.serial_number;
    }

    bool is_valid() {
        return (false == issuer_name_hash.empty()) && (false == issuer_key_hash.empty()) &&
               (false == serial_number.empty());
    }
};
struct CertificateHashDataChain {
    CertificateType certificate_type; ///< Indicates the type of the certificate for which the hash data is provided
    CertificateHashData certificate_hash_data; ///< Contains the hash data of the certificate
    std::vector<CertificateHashData>
        child_certificate_hash_data; ///< Contains the hash data of the child's certificates
};
struct GetInstalledCertificatesResult {
    GetInstalledCertificatesStatus status; ///< Indicates the status of the request
    std::vector<CertificateHashDataChain>
        certificate_hash_data_chain; ///< the hashed certificate data for each requested certificates
};
struct OCSPRequestData {
    std::optional<CertificateHashData> certificate_hash_data; ///< Contains the hash data of the certificate
    std::optional<std::string> responder_url;                 ///< Contains the responder URL
};
struct OCSPRequestDataList {
    std::vector<OCSPRequestData> ocsp_request_data_list; ///< A list of OCSP request data
};

struct CertificateOCSP {
    CertificateHashData hash;
    std::optional<fs::path> oscsp_data;
};

struct CertificateInfo {
    fs::path key;                               ///< The path of the PEM or DER encoded private key
    std::optional<fs::path> certificate;        ///< The path of the PEM or DER encoded certificate chain if found
    std::optional<fs::path> certificate_single; ///< The path of the PEM or DER encoded certificate if found
    int certificate_count; ///< The count of certificates in the chain, if the chain is available, or if single 1
    std::optional<std::string> password; ///< Specifies the password for the private key if encrypted
    std::vector<CertificateOCSP>
        oscsp; ///< Contains the ordered list of OCSP certificate data based on the chain file order
};

struct GetCertificateInfoResult {
    GetCertificateInfoStatus status;
    std::optional<CertificateInfo> info;
};

const fs::path PEM_EXTENSION = ".pem";
const fs::path DER_EXTENSION = ".der";
const fs::path KEY_EXTENSION = ".key";
const fs::path TPM_KEY_EXTENSION = ".tkey";
const fs::path CERT_HASH_EXTENSION = ".hash";

namespace conversions {
std::string encoding_format_to_string(EncodingFormat e);
std::string ca_certificate_type_to_string(CaCertificateType e);
std::string leaf_certificate_type_to_string(LeafCertificateType e);
std::string leaf_certificate_type_to_filename(LeafCertificateType e);
std::string certificate_type_to_string(CertificateType e);
std::string hash_algorithm_to_string(HashAlgorithm e);
std::string install_certificate_result_to_string(InstallCertificateResult e);
std::string delete_certificate_result_to_string(DeleteCertificateResult e);
std::string get_installed_certificates_status_to_string(GetInstalledCertificatesStatus e);
std::string get_certificate_info_status_to_string(GetCertificateInfoStatus e);
} // namespace conversions

} // namespace evse_security
