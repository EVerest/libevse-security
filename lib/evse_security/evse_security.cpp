// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <everest/logging.hpp>

#include <evse_security/evse_security.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdio.h>

#include <cert_rehash/c_rehash.hpp>

#include <evse_security/certificate/x509_bundle.hpp>
#include <evse_security/certificate/x509_hierarchy.hpp>
#include <evse_security/certificate/x509_wrapper.hpp>
#include <evse_security/utils/evse_filesystem.hpp>

namespace evse_security {

static InstallCertificateResult to_install_certificate_result(CertificateValidationResult error) {
    switch (error) {
    case CertificateValidationResult::Valid:
        EVLOG_info << "Certificate accepted";
        return InstallCertificateResult::Accepted;
    case CertificateValidationResult::Expired:
        EVLOG_warning << "Certificate has expired";
        return InstallCertificateResult::Expired;
    case CertificateValidationResult::InvalidSignature:
        EVLOG_warning << "Invalid signature";
        return InstallCertificateResult::InvalidSignature;
    case CertificateValidationResult::InvalidChain:
        EVLOG_warning << "Invalid certificate chain";
        return InstallCertificateResult::InvalidCertificateChain;
    case CertificateValidationResult::InvalidLeafSignature:
        EVLOG_warning << "Unable to verify leaf signature";
        return InstallCertificateResult::InvalidSignature;
    case CertificateValidationResult::IssuerNotFound:
        EVLOG_warning << "Issuer not found";
        return InstallCertificateResult::NoRootCertificateInstalled;
    default:
        return InstallCertificateResult::InvalidFormat;
    }
}

static std::vector<CaCertificateType> get_ca_certificate_types(const std::vector<CertificateType> certificate_types) {
    std::vector<CaCertificateType> ca_certificate_types;
    for (const auto& certificate_type : certificate_types) {
        if (certificate_type == CertificateType::V2GRootCertificate) {
            ca_certificate_types.push_back(CaCertificateType::V2G);
        }
        if (certificate_type == CertificateType::MORootCertificate) {
            ca_certificate_types.push_back(CaCertificateType::MO);
        }
        if (certificate_type == CertificateType::CSMSRootCertificate) {
            ca_certificate_types.push_back(CaCertificateType::CSMS);
        }
        if (certificate_type == CertificateType::MFRootCertificate) {
            ca_certificate_types.push_back(CaCertificateType::MF);
        }
    }
    return ca_certificate_types;
}

static CertificateType get_certificate_type(const CaCertificateType ca_certificate_type) {
    switch (ca_certificate_type) {
    case CaCertificateType::V2G:
        return CertificateType::V2GRootCertificate;
    case CaCertificateType::MO:
        return CertificateType::MORootCertificate;
    case CaCertificateType::CSMS:
        return CertificateType::CSMSRootCertificate;
    case CaCertificateType::MF:
        return CertificateType::MFRootCertificate;
    default:
        throw std::runtime_error("Could not convert CaCertificateType to CertificateType");
    }
}

static bool is_keyfile(const fs::path& file_path) {
    if (fs::is_regular_file(file_path)) {
        if (file_path.has_extension()) {
            auto extension = file_path.extension();
            if (extension == KEY_EXTENSION || extension == CUSTOM_KEY_EXTENSION) {
                return true;
            }
        }
    }

    return false;
}

/// @brief Searches for the private key linked to the provided certificate or nullopt if none was found
static std::optional<fs::path> get_private_key_path_of_certificate(const X509Wrapper& certificate,
                                                                   const fs::path& key_path_directory,
                                                                   const std::optional<std::string> password) {
    // Before iterating the whole dir check by the filename first 'key_path'.key/.tkey
    if (certificate.get_file().has_value()) {
        // Check normal keyfile & tpm filename
        for (const auto& extension : {KEY_EXTENSION, CUSTOM_KEY_EXTENSION}) {
            fs::path potential_keyfile = certificate.get_file().value();
            potential_keyfile.replace_extension(extension);

            if (fs::exists(potential_keyfile)) {
                try {
                    std::string private_key;

                    if (filesystem_utils::read_from_file(potential_keyfile, private_key)) {
                        if (KeyValidationResult::Valid ==
                            CryptoSupplier::x509_check_private_key(certificate.get(), private_key, password)) {
                            EVLOG_debug << "Key found for certificate (" << certificate.get_common_name()
                                        << ") at path: " << potential_keyfile;
                            return potential_keyfile;
                        }
                    }
                } catch (const std::exception& e) {
                    EVLOG_debug << "Could not load or verify private key at: " << potential_keyfile << ": " << e.what();
                }
            }
        }
    }

    for (const auto& entry : fs::recursive_directory_iterator(key_path_directory)) {
        if (false == fs::is_regular_file(entry)) {
            continue;
        }

        auto key_file_path = entry.path();
        if (is_keyfile(key_file_path)) {
            try {
                std::string private_key;

                if (filesystem_utils::read_from_file(key_file_path, private_key)) {
                    if (KeyValidationResult::Valid ==
                        CryptoSupplier::x509_check_private_key(certificate.get(), private_key, password)) {
                        EVLOG_debug << "Key found for certificate (" << certificate.get_common_name()
                                    << ") at path: " << key_file_path;
                        return key_file_path;
                    }
                }
            } catch (const std::exception& e) {
                EVLOG_debug << "Could not load or verify private key at: " << key_file_path << ": " << e.what();
            }
        }
    }

    EVLOG_error << "Could not find private key for given certificate: " << certificate.get_file().value_or("N/A") << "("
                << certificate.get_common_name() << ")"
                << " key path: " << key_path_directory;

    return std::nullopt;
}

/// @brief Searches for the certificate linked to the provided key
/// @return The files where the certificates were found, more than one can be returned in case it is
/// present in a bundle too
static std::set<fs::path> get_certificate_path_of_key(const fs::path& key, const fs::path& certificate_path_directory,
                                                      const std::optional<std::string> password) {
    std::string private_key;

    if (false == filesystem_utils::read_from_file(key, private_key)) {
        throw NoPrivateKeyException("Could not read private key from path: " + private_key);
    }

    // Before iterating all bundles, check by certificates from key filename
    fs::path cert_filename = key;
    cert_filename.replace_extension(PEM_EXTENSION);

    if (fs::exists(cert_filename)) {
        try {
            std::set<fs::path> bundles;
            X509CertificateBundle certificate_bundles(cert_filename, EncodingFormat::PEM);

            certificate_bundles.for_each_chain(
                [&](const fs::path& bundle, const std::vector<X509Wrapper>& certificates) {
                    for (const auto& certificate : certificates) {
                        if (KeyValidationResult::Valid ==
                            CryptoSupplier::x509_check_private_key(certificate.get(), private_key, password)) {
                            bundles.emplace(bundle);
                        }
                    }

                    // Continue iterating
                    return true;
                });

            if (bundles.empty() == false) {
                return bundles;
            }
        } catch (const CertificateLoadException& e) {
            EVLOG_debug << "Could not load certificate bundle at: " << certificate_path_directory << ": " << e.what();
        }
    }

    try {
        std::set<fs::path> bundles;
        X509CertificateBundle certificate_bundles(certificate_path_directory, EncodingFormat::PEM);

        certificate_bundles.for_each_chain([&](const fs::path& bundle, const std::vector<X509Wrapper>& certificates) {
            for (const auto& certificate : certificates) {
                if (KeyValidationResult::Valid ==
                    CryptoSupplier::x509_check_private_key(certificate.get(), private_key, password)) {
                    bundles.emplace(bundle);
                }
            }

            // Continue iterating
            return true;
        });

        if (bundles.empty() == false) {
            return bundles;
        }
    } catch (const CertificateLoadException& e) {
        EVLOG_debug << "Could not load certificate bundle at: " << certificate_path_directory << ": " << e.what();
    }

    std::string error = "Could not find certificate for given private key: ";
    error += key;
    error += " certificates path: ";
    error += certificate_path_directory;

    throw NoCertificateValidException(error);
}

/// @brief Searches for the ocsp data and hash related to the specified certificate and hash
/// @return True if the files were found, false otherwise
static bool get_oscp_data_of_certificate(const X509Wrapper& certificate, const CertificateHashData& hash,
                                         fs::path& out_path_hash, fs::path& out_path_data) {
    if (false == certificate.get_file().has_value()) {
        return false;
    }

    const auto ocsp_path = certificate.get_file().value().parent_path() / "ocsp";

    if (false == fs::exists(ocsp_path)) {
        return false;
    }

    try {
        // Iterate existing hashes
        for (const auto& hash_entry : fs::directory_iterator(ocsp_path)) {
            if (false == hash_entry.is_regular_file()) {
                continue;
            }

            CertificateHashData read_hash;

            if (filesystem_utils::read_hash_from_file(hash_entry.path(), read_hash) && read_hash == hash) {
                EVLOG_debug << "OCSP certificate hash found for certificate: " << certificate.get_common_name();

                // Get paths
                fs::path ocsp_data_path = hash_entry.path();
                ocsp_data_path.replace_extension(DER_EXTENSION);

                if (false == fs::exists(ocsp_data_path)) {
                    EVLOG_error << "OCSP certificate hash found at path:" << hash_entry.path()
                                << " but no data named: " << ocsp_data_path << " present!";
                    return false;
                }

                out_path_hash = hash_entry.path();
                out_path_data = ocsp_data_path;

                return true;
            }
        } // End for
    } catch (const fs::filesystem_error& e) {
        EVLOG_error << "Could not iterate over ocsp cache: " << e.what();
    }

    return false;
}

static OCSPRequestDataList
generate_ocsp_request_data_internal(const std::map<CaCertificateType, fs::path>& ca_bundle_path_map,
                                    const std::set<CaCertificateType>& possible_roots,
                                    const std::vector<X509Wrapper>& leaf_chain);

std::mutex EvseSecurity::security_mutex;

EvseSecurity::EvseSecurity(const FilePaths& file_paths, const std::optional<std::string>& private_key_password,
                           const std::optional<std::uintmax_t>& max_fs_usage_bytes,
                           const std::optional<std::uintmax_t>& max_fs_certificate_store_entries,
                           const std::optional<std::chrono::seconds>& csr_expiry,
                           const std::optional<std::chrono::seconds>& garbage_collect_time) :
    private_key_password(private_key_password) {
    static_assert(sizeof(std::uint8_t) == 1, "uint8_t not equal to 1 byte!");

    std::vector<fs::path> dirs = {
        file_paths.directories.csms_leaf_cert_directory,
        file_paths.directories.csms_leaf_key_directory,
        file_paths.directories.secc_leaf_cert_directory,
        file_paths.directories.secc_leaf_key_directory,
    };

    for (const auto& path : dirs) {
        if (!fs::exists(path)) {
            EVLOG_warning << "Could not find configured leaf directory at: " << path.string()
                          << " creating default dir!";
            if (!fs::create_directories(path)) {
                EVLOG_error << "Could not create default dir for path: " << path.string();
            }
        } else if (!fs::is_directory(path)) {
            throw std::runtime_error(path.string() + " is not a directory.");
        }
    }

    this->ca_bundle_path_map[CaCertificateType::CSMS] = file_paths.csms_ca_bundle;
    this->ca_bundle_path_map[CaCertificateType::MF] = file_paths.mf_ca_bundle;
    this->ca_bundle_path_map[CaCertificateType::MO] = file_paths.mo_ca_bundle;
    this->ca_bundle_path_map[CaCertificateType::V2G] = file_paths.v2g_ca_bundle;

    for (const auto& pair : this->ca_bundle_path_map) {
        if (!fs::exists(pair.second)) {
            EVLOG_warning << "Could not find configured " << conversions::ca_certificate_type_to_string(pair.first)
                          << " bundle file at: " + pair.second.string() << ", creating default!";
            if (!filesystem_utils::create_file_if_nonexistent(pair.second)) {
                EVLOG_error << "Could not create default bundle for path: " << pair.second;
            }
        }
    }

    // Check that the leafs directory is not related to the bundle directory because
    // on garbage collect that can delete relevant CA certificates instead of leaf ones
    for (const auto& leaf_dir : dirs) {
        for (auto const& [certificate_type, ca_bundle_path] : ca_bundle_path_map) {
            if (ca_bundle_path == leaf_dir) {
                throw std::runtime_error(leaf_dir.string() +
                                         " leaf directory can not overlap CA directory: " + ca_bundle_path.string());
            }
        }
    }

    this->directories = file_paths.directories;
    this->links = file_paths.links;

    this->max_fs_usage_bytes = max_fs_usage_bytes.value_or(DEFAULT_MAX_FILESYSTEM_SIZE);
    this->max_fs_certificate_store_entries = max_fs_certificate_store_entries.value_or(DEFAULT_MAX_CERTIFICATE_ENTRIES);
    this->csr_expiry = csr_expiry.value_or(DEFAULT_CSR_EXPIRY);
    this->garbage_collect_time = garbage_collect_time.value_or(DEFAULT_GARBAGE_COLLECT_TIME);

    // Start GC timer
    garbage_collect_timer.interval([this]() { this->garbage_collect(); }, this->garbage_collect_time);
}

EvseSecurity::~EvseSecurity() {
}

InstallCertificateResult EvseSecurity::install_ca_certificate(const std::string& certificate,
                                                              CaCertificateType certificate_type) {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    EVLOG_info << "Installing ca certificate: " << conversions::ca_certificate_type_to_string(certificate_type);

    if (is_filesystem_full()) {
        EVLOG_error << "Filesystem full, can't install new CA certificate!";
        return InstallCertificateResult::CertificateStoreMaxLengthExceeded;
    }

    try {
        X509Wrapper new_cert(certificate, EncodingFormat::PEM);

        if (!new_cert.is_valid()) {
            return InstallCertificateResult::Expired;
        }

        // Load existing
        const auto ca_bundle_path = this->ca_bundle_path_map.at(certificate_type);

        if (!fs::is_directory(ca_bundle_path)) {
            // Ensure file exists
            filesystem_utils::create_file_if_nonexistent(ca_bundle_path);
        }

        X509CertificateBundle existing_certs(ca_bundle_path, EncodingFormat::PEM);

        if (existing_certs.is_using_directory()) {
            std::string filename = conversions::ca_certificate_type_to_string(certificate_type) + "_ROOT_" +
                                   filesystem_utils::get_random_file_name(PEM_EXTENSION.string());
            fs::path new_path = ca_bundle_path / filename;

            // Sets the path of the new certificate
            new_cert.set_file(new_path);
        }

        // Check if cert is already installed
        if (existing_certs.contains_certificate(new_cert) == false) {
            existing_certs.add_certificate(std::move(new_cert));

            if (existing_certs.export_certificates()) {
                return InstallCertificateResult::Accepted;
            } else {
                return InstallCertificateResult::WriteError;
            }
        } else {
            // Else, simply update it
            if (existing_certs.update_certificate(std::move(new_cert))) {
                if (existing_certs.export_certificates()) {
                    return InstallCertificateResult::Accepted;
                } else {
                    return InstallCertificateResult::WriteError;
                }
            } else {
                return InstallCertificateResult::WriteError;
            }
        }
    } catch (const CertificateLoadException& e) {
        EVLOG_error << "Certificate load error: " << e.what();
        return InstallCertificateResult::InvalidFormat;
    }
}

DeleteResult EvseSecurity::delete_certificate(const CertificateHashData& certificate_hash_data) {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    EVLOG_info << "Deleteing certificate: " << certificate_hash_data.serial_number;

    DeleteResult response;
    response.result = DeleteCertificateResult::NotFound;

    bool found_certificate = false;
    bool failed_to_write = false;
    bool deleted_root_certificate = false;

    std::vector<X509Wrapper> deleted_roots;

    // After we delete the roots, we collect them and use them in building the hierarchy to delete the leafs too
    for (auto const& [certificate_type, ca_bundle_path] : ca_bundle_path_map) {
        try {
            X509CertificateBundle ca_bundle(ca_bundle_path, EncodingFormat::PEM);
            auto deleted = ca_bundle.delete_certificate(certificate_hash_data, true, false);

            if (false == deleted.empty()) {
                found_certificate = true;
                response.ca_certificate_type = certificate_type;

                if (!ca_bundle.export_certificates()) {
                    failed_to_write = true;
                } else {
                    // Append to deleted roots
                    deleted_roots.insert(deleted_roots.end(), std::make_move_iterator(deleted.begin()),
                                         std::make_move_iterator(deleted.end()));
                    deleted_root_certificate = true;
                }
            }
        } catch (const CertificateLoadException& e) {
            EVLOG_warning << "Could not load ca bundle from file: " << ca_bundle_path;
        }
    }

    // We failed to write out the delete operation, early return
    if (deleted_root_certificate) {
        if (failed_to_write) {
            EVLOG_error << "Could not delete CA root certificate!";
            response.result = DeleteCertificateResult::Failed;
            return response;
        } else {
            // TODO(ioan): we have an early return here since we do not delete the
            // intermediates/leafs that are issued by this root. Remove this code
            // to also delete the leafs that are issued by this root
            EVLOG_info << "Deleted CA root certificate successfully!";
            response.result = DeleteCertificateResult::Accepted;
            return response;
        }
    }

    // Collect all the leaf chains
    for (const auto& leaf_certificate_type : {LeafCertificateType::V2G, LeafCertificateType::CSMS}) {
        fs::path leaf_certificate_path;
        fs::path leaf_certificate_key;

        if (leaf_certificate_type == LeafCertificateType::CSMS) {
            leaf_certificate_path = this->directories.csms_leaf_cert_directory;
            leaf_certificate_key = this->directories.csms_leaf_key_directory;
        } else if (leaf_certificate_type == LeafCertificateType::V2G) {
            leaf_certificate_path = this->directories.secc_leaf_cert_directory;
            leaf_certificate_key = this->directories.secc_leaf_key_directory;
        }

        if (leaf_certificate_path.empty() || leaf_certificate_key.empty()) {
            EVLOG_error << "Could not find leaf certificate key/cert directory!";
            continue;
        }

        bool secc = (leaf_certificate_path == directories.secc_leaf_cert_directory);
        bool csms = (leaf_certificate_path == directories.csms_leaf_cert_directory) ||
                    // for when we have shared directories for secc/csms
                    (directories.csms_leaf_cert_directory == directories.secc_leaf_cert_directory);

        // The leaf bundle contains many chain/single certificates in separate files
        X509CertificateBundle leaf_bundle(leaf_certificate_path, EncodingFormat::PEM);

        CaCertificateType root_load;
        if (secc) {
            root_load = CaCertificateType::V2G;
        } else if (csms) {
            root_load = CaCertificateType::CSMS;
        } else {
            throw std::runtime_error("Leaf root type load invalid, should never happen!");
        }

        // If we deleted roots then those are the base, else load the root bundle that is intact
        std::vector<X509Wrapper> base_roots;

        try {
            base_roots =
                std::move(deleted_roots.empty()
                              ? X509CertificateBundle(ca_bundle_path_map[root_load], EncodingFormat::PEM).split()
                              : deleted_roots);
        } catch (const CertificateLoadException& e) {
            EVLOG_warning << "Could not load base roots: " << ca_bundle_path_map[root_load];
        }

        X509CertificateHierarchy hierarchy =
            std::move(X509CertificateHierarchy::build_hierarchy(base_roots, leaf_bundle.split()));

        // Collect all the leafs that we have to delete
        auto leafs_to_delete = hierarchy.find_certificates_multi(certificate_hash_data);

        leaf_bundle.for_each_chain([&](const fs::path& path, const std::vector<X509Wrapper>& chain) {
            // If any chain element is contained in the leafs to delete, then delete the whole chain
            bool delete_whole_chain = false;

            for (const auto& deleted_leaf : chain) {
                if (std::find(leafs_to_delete.begin(), leafs_to_delete.end(), deleted_leaf) != leafs_to_delete.end()) {
                    delete_whole_chain = true;
                    break;
                }
            }

            // We do not delete anything, move to next chain
            if (false == delete_whole_chain) {
                return true;
            }

            bool deleted_full_file = false;
            for (const auto& deleted_leaf : chain) {
                // Update the response
                found_certificate = true;
                response.leaf_certificate_type = leaf_certificate_type;

                if (csms) {
                    // Per M04.FR.06 we are not allowed to delete the CSMS (ChargingStationCertificate), we
                    // should return 'Failed' if we try to delete one via DeleteCertificateRequest
                    if (deleted_root_certificate) {
                        EVLOG_error << "Root ChargingStationCertificate was deleted, however the leaf will be kept";
                    } else {
                        failed_to_write = true;
                        EVLOG_error << "Error, not allowed to delete ChargingStationCertificate: "
                                    << deleted_leaf.get_common_name();
                    }
                } else {
                    // Delete the whole chain file for leafs only once since in the deletes we might have
                    // two or more certificates (subca1->subca2->leaf) that point to the same bundle file
                    if (false == deleted_full_file) {
                        if (false == filesystem_utils::delete_file(path)) {
                            failed_to_write = true;
                            EVLOG_error << "Error removing leaf chain file: " << deleted_leaf.get_common_name();
                        } else {
                            deleted_full_file = true;
                        }
                    }

                    // Clear certificate additional data, key and OCSP
                    if (false == failed_to_write) {
                        // Collect keys of deleted certificates and OCSP data
                        auto key_path = get_private_key_path_of_certificate(deleted_leaf, leaf_certificate_key,
                                                                            this->private_key_password);

                        if (key_path.has_value()) {
                            EVLOG_info << "Deleted key of leaf certificate: " << deleted_leaf.get_common_name();
                            filesystem_utils::delete_file(key_path.value());
                        }

                        // NOTE: Deletes the OCSP only of the leaf since the intermediates being used by
                        // multiple chains can still require their OCSP data
                        fs::path path_ocsp_hash, path_ocsp_data;
                        if (get_oscp_data_of_certificate(deleted_leaf, certificate_hash_data, path_ocsp_hash,
                                                         path_ocsp_data)) {
                            EVLOG_info << "Deleted ocsp data of certificate: " << deleted_leaf.get_common_name();
                            filesystem_utils::delete_file(path_ocsp_hash);
                            filesystem_utils::delete_file(path_ocsp_data);
                        }
                    }
                }
            } // End deleted certificates for

            return true;
        }); // End for each chain
    }       // End for each leaf directory

    if (!found_certificate) {
        response.result = DeleteCertificateResult::NotFound;
        return response;
    }
    if (failed_to_write) {
        // at least one certificate could not be deleted from the bundle
        response.result = DeleteCertificateResult::Failed;
        return response;
    }

    response.result = DeleteCertificateResult::Accepted;
    return response;
}

InstallCertificateResult EvseSecurity::update_leaf_certificate(const std::string& certificate_chain,
                                                               LeafCertificateType certificate_type) {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    if (is_filesystem_full()) {
        EVLOG_error << "Filesystem full, can't install new CA certificate!";
        return InstallCertificateResult::CertificateStoreMaxLengthExceeded;
    }

    EVLOG_info << "Updating leaf certificate: " << conversions::leaf_certificate_type_to_string(certificate_type);

    fs::path cert_path;
    fs::path key_path;

    if (certificate_type == LeafCertificateType::CSMS) {
        cert_path = this->directories.csms_leaf_cert_directory;
        key_path = this->directories.csms_leaf_key_directory;
    } else if (certificate_type == LeafCertificateType::V2G) {
        cert_path = this->directories.secc_leaf_cert_directory;
        key_path = this->directories.secc_leaf_key_directory;
    } else {
        EVLOG_error << "Attempt to update leaf certificate for non CSMS/V2G certificate!";
        return InstallCertificateResult::WriteError;
    }

    try {
        X509CertificateBundle chain_certificate(certificate_chain, EncodingFormat::PEM);
        std::vector<X509Wrapper> _certificate_chain = chain_certificate.split();
        if (_certificate_chain.empty()) {
            return InstallCertificateResult::InvalidFormat;
        }

        // Internal since we already acquired the lock
        const auto result = this->verify_certificate_internal(certificate_chain, {certificate_type});
        if (result != CertificateValidationResult::Valid) {
            return to_install_certificate_result(result);
        }

        // First certificate is always the leaf as per the spec
        const auto& leaf_certificate = _certificate_chain[0];

        // Check if a private key belongs to the provided certificate
        std::optional<fs::path> private_key_path_opt =
            get_private_key_path_of_certificate(leaf_certificate, key_path, this->private_key_password);

        if (false == private_key_path_opt.has_value()) {
            EVLOG_warning << "Provided certificate does not belong to any private key";
            return InstallCertificateResult::WriteError;
        }

        const auto& private_key_path = private_key_path_opt.value();

        // Write certificate to file
        std::string extra_filename = filesystem_utils::get_random_file_name(PEM_EXTENSION.string());
        std::string file_name = conversions::leaf_certificate_type_to_filename(certificate_type) + extra_filename;

        const auto file_path = cert_path / file_name;
        std::string str_cert = leaf_certificate.get_export_string();

        if (filesystem_utils::write_to_file(file_path, str_cert, std::ios::out)) {

            // Remove from managed certificate keys, the CSR is fulfilled, no need to delete the key
            // since it is not orphaned any more
            auto it = managed_csr.find(private_key_path);
            if (it != managed_csr.end()) {
                managed_csr.erase(it);
            }

            // We presume that we might receive a leaf chain that also contains intermediate certificates
            if (_certificate_chain.size() > 1) {
                // Attempt to write the chain to file
                const auto chain_file_name = std::string("CPO_CERT_") +
                                             conversions::leaf_certificate_type_to_filename(certificate_type) +
                                             "CHAIN_" + extra_filename;

                const auto chain_file_path = cert_path / chain_file_name;
                std::string str_chain_cert = chain_certificate.to_export_string();

                if (false == filesystem_utils::write_to_file(chain_file_path, str_chain_cert, std::ios::out)) {
                    // This is an error, since if we contain SUBCAs those are required for a connection
                    EVLOG_error << "Could not write leaf certificate chain to file!";
                    return InstallCertificateResult::WriteError;
                }
            }

            // TODO(ioan): properly rename key path here for fast retrieval
            // @see 'get_private_key_path_of_certificate' and 'get_certificate_path_of_key'

            return InstallCertificateResult::Accepted;
        } else {
            return InstallCertificateResult::WriteError;
        }

    } catch (const CertificateLoadException& e) {
        EVLOG_warning << "Could not load update leaf certificate because of invalid format";
        return InstallCertificateResult::InvalidFormat;
    }

    return InstallCertificateResult::Accepted;
}

GetInstalledCertificatesResult EvseSecurity::get_installed_certificate(CertificateType certificate_type) {
    return get_installed_certificates({certificate_type});
}

GetInstalledCertificatesResult
EvseSecurity::get_installed_certificates(const std::vector<CertificateType>& certificate_types) {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    GetInstalledCertificatesResult result;
    std::vector<CertificateHashDataChain> certificate_chains;
    const auto ca_certificate_types = get_ca_certificate_types(certificate_types);

    // retrieve ca certificates and chains
    for (const auto& ca_certificate_type : ca_certificate_types) {
        auto ca_bundle_path = this->ca_bundle_path_map.at(ca_certificate_type);
        try {
            X509CertificateBundle ca_bundle(ca_bundle_path, EncodingFormat::PEM);
            X509CertificateHierarchy& hierarchy = ca_bundle.get_certificate_hierarchy();

            EVLOG_debug << "Hierarchy:(" << conversions::ca_certificate_type_to_string(ca_certificate_type) << ")\n"
                        << hierarchy.to_debug_string();

            // Iterate the hierarchy and add all the certificates to their respective locations
            for (auto& root : hierarchy.get_hierarchy()) {
                // Ignore non self-signed
                if (false == root.state.is_selfsigned) {
                    continue;
                }

                CertificateHashDataChain certificate_hash_data_chain;

                certificate_hash_data_chain.certificate_type =
                    get_certificate_type(ca_certificate_type); // We always know type
                certificate_hash_data_chain.certificate_hash_data = root.hash.value();

                // Add all owned children/certificates in order
                X509CertificateHierarchy::for_each_descendant(
                    [&certificate_hash_data_chain](const X509Node& child, int depth) {
                        if (child.hash.has_value()) {
                            certificate_hash_data_chain.child_certificate_hash_data.push_back(child.hash.value());
                        }
                    },
                    root);

                // Add to our chains
                certificate_chains.push_back(certificate_hash_data_chain);
            }
        } catch (const CertificateLoadException& e) {
            EVLOG_warning << "Could not load CA bundle file at: " << ca_bundle_path << " error: " << e.what();
        }
    }

    // retrieve v2g certificate chain
    if (std::find(certificate_types.begin(), certificate_types.end(), CertificateType::V2GCertificateChain) !=
        certificate_types.end()) {
        // Retrieve all valid leaf certificates, we will return
        // multiple chains for each valid leaf that we find
        CertificateQueryParams params;
        params.certificate_type = LeafCertificateType::V2G;
        params.include_all_valid = true;
        params.remove_duplicates = true;

        GetCertificateFullInfoResult secc_key_pairs = get_full_leaf_certificate_info_internal(params);

        if (secc_key_pairs.status == GetCertificateInfoStatus::Accepted) {
            for (const auto& secc_key_pair : secc_key_pairs.info) {
                fs::path certificate_path;

                if (secc_key_pair.certificate.has_value()) {
                    certificate_path = secc_key_pair.certificate.value();
                } else if (secc_key_pair.certificate_single.has_value()) {
                    certificate_path = secc_key_pair.certificate_single.value();
                } else {
                    throw std::runtime_error("Leaf certificate single/bundle not present, should never happen!");
                }

                try {
                    // Leaf V2G chain, containing (SECCLeaf->SubCA2->SubCA1) or (SECCLeaf)
                    X509CertificateBundle leaf_bundle(certificate_path, EncodingFormat::PEM);

                    // V2G chain, containing the certs from the V2G bundle/folder,
                    // containing (SubCA2->SubCA1->V2GRoot) or (V2GRoot)
                    const auto ca_bundle_path = this->ca_bundle_path_map.at(CaCertificateType::V2G);
                    X509CertificateBundle ca_bundle(ca_bundle_path, EncodingFormat::PEM);

                    // Merge the bundles, adding only uniques for full chain
                    // (SubCA2->SubCA1->V2GRoot->SECCLeaf) in any order
                    for (auto& certif : leaf_bundle.split()) {
                        ca_bundle.add_certificate_unique(std::move(certif));
                    }

                    // Create the proper certificate hierarchy since the bundle is not ordered
                    X509CertificateHierarchy& hierarchy = ca_bundle.get_certificate_hierarchy();
                    EVLOG_debug << "Hierarchy:(V2GCertificateChain)\n" << hierarchy.to_debug_string();

                    for (auto& root : hierarchy.get_hierarchy()) {
                        CertificateHashDataChain certificate_hash_data_chain;
                        certificate_hash_data_chain.certificate_type = CertificateType::V2GCertificateChain;

                        // Since the hierarchy starts with V2G (Root) -> SubCa1->SubCa2 we have to reorder:
                        // them with the leaf first when returning to:
                        // * Leaf           [index 0]
                        // --- SubCa2       [index 1]
                        // --- SubCa1       [index 2]
                        // --- --- V2GRoot  [index 3]
                        std::vector<CertificateHashData> hierarchy_hash_data;

                        // For each root's descendant, excluding the root
                        X509CertificateHierarchy::for_each_descendant(
                            [&](const X509Node& child, int depth) {
                                if (child.hash.has_value()) {
                                    hierarchy_hash_data.push_back(child.hash.value());
                                }
                            },
                            root);

                        // Now the hierarchy_hash_data contains SubCA1->SubCA2->SECCLeaf,
                        // reverse order iteration to conform to the required leaf-first order
                        if (hierarchy_hash_data.size()) {
                            bool first_leaf = true;

                            // Reverse iteration
                            for (auto it = hierarchy_hash_data.rbegin(); it != hierarchy_hash_data.rend(); ++it) {
                                if (first_leaf) {
                                    // Leaf is the last
                                    certificate_hash_data_chain.certificate_hash_data = *it;
                                    first_leaf = false;
                                } else {
                                    certificate_hash_data_chain.child_certificate_hash_data.push_back(*it);
                                }
                            }

                            // Add to our chains
                            certificate_chains.push_back(certificate_hash_data_chain);
                        }
                    }
                } catch (const CertificateLoadException& e) {
                    EVLOG_error << "Could not load installed leaf certificates: " << e.what();
                }
            }
        }
    }

    if (certificate_chains.empty()) {
        result.status = GetInstalledCertificatesStatus::NotFound;
    } else {
        result.status = GetInstalledCertificatesStatus::Accepted;
    }

    result.certificate_hash_data_chain = certificate_chains;
    return result;
}

int EvseSecurity::get_count_of_installed_certificates(const std::vector<CertificateType>& certificate_types) {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    int count = 0;

    std::set<fs::path> directories;
    const auto ca_certificate_types = get_ca_certificate_types(certificate_types);

    // Collect unique directories
    for (const auto& ca_certificate_type : ca_certificate_types) {
        directories.emplace(this->ca_bundle_path_map.at(ca_certificate_type));
    }

    for (const auto& unique_dir : directories) {
        try {
            X509CertificateBundle ca_bundle(unique_dir, EncodingFormat::PEM);
            count += ca_bundle.get_certificate_count();
        } catch (const CertificateLoadException& e) {
            EVLOG_error << "Could not load bundle for certificate count: " << e.what();
        }
    }

    // V2G Chain
    if (std::find(certificate_types.begin(), certificate_types.end(), CertificateType::V2GCertificateChain) !=
        certificate_types.end()) {
        auto leaf_dir = this->directories.secc_leaf_cert_directory;

        // Load all from chain, including expired/unused
        try {
            X509CertificateBundle leaf_bundle(leaf_dir, EncodingFormat::PEM);
            count += leaf_bundle.get_certificate_count();
        } catch (const CertificateLoadException& e) {
            EVLOG_error << "Could not load bundle for certificate count: " << e.what();
        }
    }

    return count;
}

OCSPRequestDataList EvseSecurity::get_v2g_ocsp_request_data() {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    CertificateQueryParams params;
    params.certificate_type = LeafCertificateType::V2G;
    params.encoding = EncodingFormat::PEM;
    params.include_all_valid = true;
    params.include_future_valid = true;
    params.include_ocsp = false;
    params.include_root = false;
    params.remove_duplicates = true;

    GetCertificateFullInfoResult result = get_full_leaf_certificate_info_internal(params);

    if (result.status != GetCertificateInfoStatus::Accepted or !result.info.size()) {
        EVLOG_error << "Could not get key pair, for v2g ocsp request!";
        return OCSPRequestDataList();
    }

    OCSPRequestDataList full_oscp_list;

    for (const auto secc_key_pair : result.info) {
        std::string chain{};

        if (secc_key_pair.certificate.has_value()) {
            filesystem_utils::read_from_file(secc_key_pair.certificate.value(), chain);
        } else if (secc_key_pair.certificate_single.has_value()) {
            filesystem_utils::read_from_file(secc_key_pair.certificate_single.value(), chain);
        } else {
            EVLOG_error << "Could not load v2g ocsp cache leaf chain!";
        }

        std::vector<X509Wrapper> leaf_chain{};

        if (!chain.empty()) {
            leaf_chain = std::move(X509CertificateBundle(chain, EncodingFormat::PEM).split());
        }

        if (!leaf_chain.empty()) {
            OCSPRequestDataList partial_ocsp_request =
                generate_ocsp_request_data_internal(this->ca_bundle_path_map, {CaCertificateType::V2G}, leaf_chain);

            for (OCSPRequestData& ocsp_data : partial_ocsp_request.ocsp_request_data_list) {
                // Add the ones that we don't already contain
                bool not_contained =
                    std::find_if(full_oscp_list.ocsp_request_data_list.begin(),
                                 full_oscp_list.ocsp_request_data_list.end(), [&ocsp_data](auto& existing) {
                                     return (ocsp_data.certificate_hash_data == existing.certificate_hash_data);
                                 }) == full_oscp_list.ocsp_request_data_list.end();

                if (not_contained) {
                    full_oscp_list.ocsp_request_data_list.emplace_back(std::move(ocsp_data));
                }
            }
        }
    }

    return full_oscp_list;
}

OCSPRequestDataList EvseSecurity::get_mo_ocsp_request_data(const std::string& certificate_chain) {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    try {
        std::vector<X509Wrapper> leaf_chain = X509CertificateBundle(certificate_chain, EncodingFormat::PEM).split();

        // Test for both MO and V2G roots
        return generate_ocsp_request_data_internal(this->ca_bundle_path_map,
                                                   {CaCertificateType::V2G, CaCertificateType::MO}, leaf_chain);
    } catch (const CertificateLoadException& e) {
        EVLOG_error << "Could not load mo ocsp cache leaf chain!";
    }

    return {};
}

static OCSPRequestDataList
generate_ocsp_request_data_internal(const std::map<CaCertificateType, fs::path>& ca_bundle_path_map,
                                    const std::set<CaCertificateType>& possible_roots,
                                    const std::vector<X509Wrapper>& leaf_chain) {
    OCSPRequestDataList response;

    if (leaf_chain.empty()) {
        return response;
    }

    std::vector<X509Wrapper> full_root_hierarchy;
    for (const CaCertificateType& root_type : possible_roots) {
        const fs::path& root_path = ca_bundle_path_map.at(root_type);
        std::vector<X509Wrapper> root_hierarchy = X509CertificateBundle(root_path, EncodingFormat::PEM).split();

        full_root_hierarchy.insert(full_root_hierarchy.end(), std::make_move_iterator(root_hierarchy.begin()),
                                   std::make_move_iterator(root_hierarchy.end()));
    }

    std::vector<OCSPRequestData> ocsp_request_data_list;
    try {
        // Build the full hierarchy
        auto hierarchy = std::move(X509CertificateHierarchy::build_hierarchy(full_root_hierarchy, leaf_chain));

        // Search for the first valid root, and collect all the chain
        for (auto& root : hierarchy.get_hierarchy()) {
            // Ignore non-roots and ignore invalid ones
            if (root.certificate.is_selfsigned() == false || root.certificate.is_valid() == false) {
                continue;
            }

            // Collect the chain, from root->subca1->subca2->leaf
            std::vector<X509Wrapper> descendants = hierarchy.collect_descendants(root.certificate);

            if (descendants.size() > 0) {
                // We must make sure that the full received 'leaf_chain' is present in the descendants
                bool missing_link = false;

                for (const X509Wrapper& received_chain_link : leaf_chain) {
                    bool descendants_contain =
                        std::find(descendants.begin(), descendants.end(), received_chain_link) != descendants.end();

                    // A link is missing from the chain, break
                    if (!descendants_contain) {
                        missing_link = true;
                        break;
                    }
                }

                // Search next root
                if (missing_link) {
                    continue;
                }
            } else {
                // Search next root
                continue;
            }

            // Reverse iteration, since we need to move from the leaf to the root, not the other way around
            for (auto rit = descendants.rbegin(); rit != descendants.rend(); ++rit) {
                auto& certificate = *rit;

                std::string responder_url = certificate.get_responder_url();

                if (!responder_url.empty()) {
                    CertificateHashData certificate_hash_data;
                    if (hierarchy.get_certificate_hash(certificate, certificate_hash_data)) {
                        // Do not insert duplicate hashes, in case we have multiple SUBCAs in different bundles
                        auto it = std::find_if(std::begin(ocsp_request_data_list), std::end(ocsp_request_data_list),
                                               [&certificate_hash_data](const OCSPRequestData& existing_data) {
                                                   return existing_data.certificate_hash_data == certificate_hash_data;
                                               });

                        if (it == ocsp_request_data_list.end()) {
                            OCSPRequestData ocsp_request_data = {certificate_hash_data, responder_url};
                            ocsp_request_data_list.push_back(ocsp_request_data);
                        }
                    } else {
                        EVLOG_error << "Could not find hash for certificate: " << certificate.get_common_name();
                    }
                } else {
                    EVLOG_warning << "When generating an OCSP request, could not find responder URL for certificate: "
                                  << certificate.get_common_name();
                }
            }
        }

        response.ocsp_request_data_list = ocsp_request_data_list;
    } catch (const CertificateLoadException& e) {
        EVLOG_error << "Could not get ocsp cache, certificate load failure: " << e.what();
    } catch (const NoCertificateFound& e) {
        EVLOG_error << "Could not find proper root: " << e.what();
    }

    return response;
}

void EvseSecurity::update_ocsp_cache(const CertificateHashData& certificate_hash_data,
                                     const std::string& ocsp_response) {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    EVLOG_info << "Updating OCSP cache";

    // TODO(ioan): shouldn't we also do this for the MO?
    const auto ca_bundle_path = this->ca_bundle_path_map.at(CaCertificateType::V2G);
    auto leaf_cert_dir = this->directories.secc_leaf_cert_directory; // V2G leafs

    try {
        X509CertificateBundle ca_bundle(ca_bundle_path, EncodingFormat::PEM);
        X509CertificateBundle leaf_bundle(leaf_cert_dir, EncodingFormat::PEM);

        auto certificate_hierarchy =
            std::move(X509CertificateHierarchy::build_hierarchy(ca_bundle.split(), leaf_bundle.split()));

        // If we already have the hash, over-write, else create a new one
        try {
            // Find the certificates, can me multiple if we have SUBcas in multiple bundles
            std::vector<X509Wrapper> certs = certificate_hierarchy.find_certificates_multi(certificate_hash_data);

            for (auto& cert : certs) {
                EVLOG_debug << "Writing OCSP Response to filesystem";
                if (false == cert.get_file().has_value()) {
                    EVLOG_error << "Could not find OCSP cache patch directory!";
                    continue;
                }

                const auto ocsp_path = cert.get_file().value().parent_path() / "ocsp";
                bool updated_hash = false;

                if (false == fs::exists(ocsp_path)) {
                    filesystem_utils::create_file_or_dir_if_nonexistent(ocsp_path);
                }

                fs::path out_path_hash;
                fs::path out_path_data;

                // If we already have OCSP data, update it
                if (get_oscp_data_of_certificate(cert, certificate_hash_data, out_path_hash, out_path_data)) {
                    EVLOG_debug << "OCSP certificate hash already found, over-writing!";

                    // Discard previous content
                    std::ofstream fs(out_path_data.c_str(), std::ios::trunc);
                    fs << ocsp_response;
                    fs.close();

                    updated_hash = true;
                }

                if (!updated_hash) {
                    // Randomize filename, since multiple certificates can be stored in same bundle
                    const auto name = filesystem_utils::get_random_file_name("_ocsp");

                    const auto ocsp_file_path = (ocsp_path / name) += DER_EXTENSION;
                    const auto hash_file_path = (ocsp_path / name) += CERT_HASH_EXTENSION;

                    // Write out OCSP data
                    try {
                        std::ofstream fs(ocsp_file_path.c_str());
                        fs << ocsp_response;
                        fs.close();
                    } catch (const std::exception& e) {
                        EVLOG_error << "Could not write OCSP certificate data!";
                    }

                    if (false == filesystem_utils::write_hash_to_file(hash_file_path, certificate_hash_data)) {
                        EVLOG_error << "Could not write OCSP certificate hash!";
                    }

                    EVLOG_debug << "OCSP certificate hash not found, written at path: " << ocsp_file_path;
                }
            }
        } catch (const NoCertificateFound& e) {
            EVLOG_error << "Could not find any certificate for ocsp cache update: " << e.what();
        }
    } catch (const CertificateLoadException& e) {
        EVLOG_error << "Could not update ocsp cache, certificate load failure: " << e.what();
    }
}

std::optional<fs::path> EvseSecurity::retrieve_ocsp_cache(const CertificateHashData& certificate_hash_data) {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    return retrieve_ocsp_cache_internal(certificate_hash_data);
}

std::optional<fs::path> EvseSecurity::retrieve_ocsp_cache_internal(const CertificateHashData& certificate_hash_data) {
    // TODO(ioan): shouldn't we also do this for the MO?
    const auto ca_bundle_path = this->ca_bundle_path_map.at(CaCertificateType::V2G);
    const auto leaf_path = this->directories.secc_leaf_key_directory;

    try {
        X509CertificateBundle ca_bundle(ca_bundle_path, EncodingFormat::PEM);
        X509CertificateBundle leaf_bundle(leaf_path, EncodingFormat::PEM);

        auto certificate_hierarchy =
            std::move(X509CertificateHierarchy::build_hierarchy(ca_bundle.split(), leaf_bundle.split()));

        // Find the certificate
        std::optional<X509Wrapper> cert = certificate_hierarchy.find_certificate(certificate_hash_data);

        if (false == cert.has_value()) {
            EVLOG_error << "Could not find any certificate for ocsp cache retrieve!";
            return std::nullopt;
        }

        EVLOG_debug << "Reading OCSP Response from filesystem";

        fs::path path_hash, path_data;
        if (get_oscp_data_of_certificate(cert.value(), certificate_hash_data, path_hash, path_data)) {
            return path_data;
        }
    } catch (const CertificateLoadException& e) {
        EVLOG_error << "Could not retrieve ocsp cache, certificate load failure: " << e.what();
    }

    return std::nullopt;
}

bool EvseSecurity::is_ca_certificate_installed(CaCertificateType certificate_type) {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    return is_ca_certificate_installed_internal(certificate_type);
}

bool EvseSecurity::is_ca_certificate_installed_internal(CaCertificateType certificate_type) {
    try {
        X509CertificateBundle bundle(this->ca_bundle_path_map.at(certificate_type), EncodingFormat::PEM);

        // Search for a valid self-signed root
        auto& hierarchy = bundle.get_certificate_hierarchy();

        // Get all roots and search for a valid self-signed
        for (auto& root : hierarchy.get_hierarchy()) {
            if (root.certificate.is_selfsigned() && root.certificate.is_valid()) {
                return true;
            }
        }
    } catch (const CertificateLoadException& e) {
        EVLOG_error << "Could not load ca certificate type:"
                    << conversions::ca_certificate_type_to_string(certificate_type);
        return false;
    }

    return false;
}

void EvseSecurity::certificate_signing_request_failed(const std::string& csr, LeafCertificateType certificate_type) {
    // TODO(ioan): delete the pairing key of the CSR
}

GetCertificateSignRequestResult
EvseSecurity::generate_certificate_signing_request_internal(LeafCertificateType certificate_type,
                                                            const CertificateSigningRequestInfo& info) {
    GetCertificateSignRequestResult result{};

    EVLOG_info << "Generating CSR for leaf: " << conversions::leaf_certificate_type_to_string(certificate_type);

    std::string csr;
    CertificateSignRequestResult csr_result = CryptoSupplier::x509_generate_csr(info, csr);

    if (csr_result == CertificateSignRequestResult::Valid) {
        result.status = GetCertificateSignRequestStatus::Accepted;
        result.csr = std::move(csr);

        EVLOG_debug << "Generated CSR end. CSR: " << csr;

        // Add the key to the managed CRS that we will delete if we can't find a certificate pair within the time
        if (info.key_info.private_key_file.has_value()) {
            managed_csr.emplace(info.key_info.private_key_file.value(), std::chrono::steady_clock::now());
        }
    } else {
        EVLOG_error << "CSR leaf generation error: "
                    << conversions::get_certificate_sign_request_result_to_string(csr_result);

        if (csr_result == CertificateSignRequestResult::KeyGenerationError) {
            result.status = GetCertificateSignRequestStatus::KeyGenError;
        } else {
            result.status = GetCertificateSignRequestStatus::GenerationError;
        }
    }

    return result;
}

GetCertificateSignRequestResult EvseSecurity::generate_certificate_signing_request(LeafCertificateType certificate_type,
                                                                                   const std::string& country,
                                                                                   const std::string& organization,
                                                                                   const std::string& common,
                                                                                   bool use_custom_provider) {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    // Make a difference between normal and tpm keys for identification
    const auto file_name = conversions::leaf_certificate_type_to_filename(certificate_type) +
                           filesystem_utils::get_random_file_name(use_custom_provider ? CUSTOM_KEY_EXTENSION.string()
                                                                                      : KEY_EXTENSION.string());

    fs::path key_path;
    if (certificate_type == LeafCertificateType::CSMS) {
        key_path = this->directories.csms_leaf_key_directory / file_name;
    } else if (certificate_type == LeafCertificateType::V2G) {
        key_path = this->directories.secc_leaf_key_directory / file_name;
    } else {
        EVLOG_error << "Generate CSR for non CSMS/V2G leafs!";

        GetCertificateSignRequestResult result{};
        result.status = GetCertificateSignRequestStatus::InvalidRequestedType;
        return result;
    }

    std::string csr;
    CertificateSigningRequestInfo info;

    info.n_version = 0;
    info.commonName = common;
    info.country = country;
    info.organization = organization;
#ifdef CSR_DNS_NAME
    info.dns_name = CSR_DNS_NAME;
#else
    info.dns_name = std::nullopt;
#endif
#ifdef CSR_IP_ADDRESS
    info.ip_address = CSR_IP_ADDRESS;
#else
    info.ip_address = std::nullopt;
#endif

    info.key_info.key_type = CryptoKeyType::EC_prime256v1;
    info.key_info.generate_on_custom = use_custom_provider;
    info.key_info.private_key_file = key_path;

    if ((use_custom_provider == false) && private_key_password.has_value()) {
        info.key_info.private_key_pass = private_key_password;
    }

    return generate_certificate_signing_request_internal(certificate_type, info);
}

GetCertificateSignRequestResult EvseSecurity::generate_certificate_signing_request(LeafCertificateType certificate_type,
                                                                                   const std::string& country,
                                                                                   const std::string& organization,
                                                                                   const std::string& common) {
    return generate_certificate_signing_request(certificate_type, country, organization, common, false);
}

GetCertificateFullInfoResult EvseSecurity::get_all_valid_certificates_info(LeafCertificateType certificate_type,
                                                                           EncodingFormat encoding, bool include_ocsp) {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    GetCertificateFullInfoResult result =
        get_full_leaf_certificate_info_internal({certificate_type, encoding, include_ocsp, true, true});

    // If we failed, simply return the result
    if (result.status != GetCertificateInfoStatus::Accepted) {
        return result;
    }

    GetCertificateFullInfoResult filtered_results;
    filtered_results.status = result.status;

    // Filter the certificates to return only the ones that have a unique
    // root, and from those that have a unique root, return only the newest
    std::set<std::string> unique_roots;

    // The newest are the first, that's how 'get_leaf_certificate_info_internal'
    // returns them
    for (const auto& chain : result.info) {
        // Ignore non-root items
        if (!chain.certificate_root.has_value()) {
            continue;
        }

        const std::string& root = chain.certificate_root.value();

        // If we don't contain the unique root yet, it is the newest leaf for that root
        if (unique_roots.find(root) == unique_roots.end()) {
            filtered_results.info.push_back(chain);

            // Add it to the roots list, adding only unique roots
            unique_roots.insert(root);
        }
    }

    return filtered_results;
}

GetCertificateInfoResult EvseSecurity::get_leaf_certificate_info(LeafCertificateType certificate_type,
                                                                 EncodingFormat encoding, bool include_ocsp) {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    return get_leaf_certificate_info_internal(certificate_type, encoding, include_ocsp);
}

GetCertificateInfoResult EvseSecurity::get_leaf_certificate_info_internal(LeafCertificateType certificate_type,
                                                                          EncodingFormat encoding, bool include_ocsp) {
    GetCertificateFullInfoResult result =
        get_full_leaf_certificate_info_internal({certificate_type, encoding, include_ocsp, false, false, false});
    GetCertificateInfoResult internal_result;

    internal_result.status = result.status;
    if (!result.info.empty()) {
        internal_result.info = std::move(result.info.at(0));
    }

    return internal_result;
}

GetCertificateFullInfoResult
EvseSecurity::get_full_leaf_certificate_info_internal(const CertificateQueryParams& params) {
    auto certificate_type = params.certificate_type;

    EVLOG_info << "Requesting leaf certificate info: "
               << conversions::leaf_certificate_type_to_string(certificate_type);

    GetCertificateFullInfoResult result;

    fs::path key_dir;
    fs::path cert_dir;
    CaCertificateType root_type;

    if (certificate_type == LeafCertificateType::CSMS) {
        key_dir = this->directories.csms_leaf_key_directory;
        cert_dir = this->directories.csms_leaf_cert_directory;
        root_type = CaCertificateType::CSMS;
    } else if (certificate_type == LeafCertificateType::V2G) {
        key_dir = this->directories.secc_leaf_key_directory;
        cert_dir = this->directories.secc_leaf_cert_directory;
        root_type = CaCertificateType::V2G;
    } else {
        EVLOG_warning << "Rejected attempt to retrieve non CSMS/V2G key pair";
        result.status = GetCertificateInfoStatus::Rejected;
        return result;
    }

    fs::path root_dir = ca_bundle_path_map[root_type];

    // choose appropriate cert (valid_from / valid_to)
    try {
        auto leaf_certificates = X509CertificateBundle(cert_dir, EncodingFormat::PEM);

        if (leaf_certificates.empty()) {
            EVLOG_warning << "Could not find any key pair";
            result.status = GetCertificateInfoStatus::NotFound;
            return result;
        }

        struct KeyPairInternal {
            X509Wrapper certificate;
            fs::path certificate_key;
        };

        std::vector<KeyPairInternal> valid_leafs;

        bool any_valid_certificate = false;
        bool any_valid_key = false;

        // Iterate all certificates from newest to the oldest
        leaf_certificates.for_each_chain_ordered(
            [&](const fs::path& file, const std::vector<X509Wrapper>& chain) {
                bool is_valid = false;

                if (not chain.empty()) {
                    is_valid |= chain.at(0).is_valid();

                    if (params.include_future_valid) {
                        is_valid |= chain.at(0).is_valid_in_future();
                    }
                }

                if (is_valid) {
                    any_valid_certificate = true;

                    // Search for the private key
                    auto priv_key_path =
                        get_private_key_path_of_certificate(chain.at(0), key_dir, this->private_key_password);

                    if (priv_key_path.has_value()) {
                        // Found at least one valid key
                        any_valid_key = true;

                        KeyPairInternal key_pair{chain.at(0), priv_key_path.value()};

                        if (params.remove_duplicates) {
                            // Filter the already added certificates, since we can have a case
                            // when a leaf is present in 2 files (single/chain) that causes it
                            // to be added to the list twice by the bundle parser
                            auto it = std::find_if(valid_leafs.begin(), valid_leafs.end(),
                                                   [&key_pair](const auto& in_key_pair) {
                                                       return in_key_pair.certificate == key_pair.certificate;
                                                   });

                            // None found
                            if (it == valid_leafs.end()) {
                                valid_leafs.emplace_back(std::move(key_pair));
                            }
                        } else {
                            // Copy to latest valid
                            valid_leafs.emplace_back(std::move(key_pair));
                        }

                        // We found, break
                        EVLOG_info << "Found valid leaf: [" << chain.at(0).get_file().value() << "]";

                        // Collect all if we don't include valid only
                        if (params.include_all_valid == false) {
                            EVLOG_info << "Not requiring all valid leafs, returning";
                            return false;
                        }
                    }
                }

                return true;
            },
            [](const std::vector<X509Wrapper>& a, const std::vector<X509Wrapper>& b) {
                // Order from newest to oldest
                if (not a.empty() && not b.empty()) {
                    return a.at(0).get_valid_to() > b.at(0).get_valid_to();
                } else {
                    return false;
                }
            });

        if (!any_valid_certificate) {
            EVLOG_warning << "Could not find valid certificate";
            result.status = GetCertificateInfoStatus::NotFoundValid;
            return result;
        }

        if (!any_valid_key) {
            EVLOG_warning << "Could not find private key for the valid certificate";
            result.status = GetCertificateInfoStatus::PrivateKeyNotFound;
            return result;
        }

        for (const auto& valid_leaf : valid_leafs) {
            // Key path doesn't change
            fs::path key_file = valid_leaf.certificate_key;
            auto& certificate = valid_leaf.certificate;

            // Paths to search
            std::optional<fs::path> certificate_file;
            std::optional<fs::path> chain_file;

            X509CertificateBundle leaf_directory(cert_dir, EncodingFormat::PEM);

            const std::vector<X509Wrapper>* leaf_fullchain = nullptr;
            const std::vector<X509Wrapper>* leaf_single = nullptr;
            int chain_len = 1; // Defaults to 1, single certificate

            // We are searching for both the full leaf bundle, containing the leaf and the cso1/2 and the single
            // leaf without the cso1/2
            leaf_directory.for_each_chain(
                [&](const std::filesystem::path& path, const std::vector<X509Wrapper>& chain) {
                    // If we contain the latest valid, we found our generated bundle
                    bool leaf_found = (std::find(chain.begin(), chain.end(), certificate) != chain.end());

                    if (leaf_found) {
                        if (chain.size() > 1) {
                            leaf_fullchain = &chain;
                            chain_len = chain.size();
                        } else if (chain.size() == 1) {
                            leaf_single = &chain;
                        }
                    }

                    // Found both, break
                    if (leaf_fullchain != nullptr && leaf_single != nullptr)
                        return false;

                    return true;
                });

            std::vector<CertificateOCSP> certificate_ocsp{};
            std::optional<std::string> leafs_root = std::nullopt;

            // None were found
            if (leaf_single == nullptr && leaf_fullchain == nullptr) {
                EVLOG_error << "Could not find any leaf certificate for:"
                            << conversions::leaf_certificate_type_to_string(certificate_type);
                // Move onto next valid leaf, and attempt a search there
                continue;
            }

            if (leaf_fullchain != nullptr) {
                chain_file = leaf_fullchain->at(0).get_file();
                EVLOG_debug << "Leaf fullchain: [" << chain_file.value_or("INVALID") << "]";
            } else {
                EVLOG_debug << conversions::leaf_certificate_type_to_string(certificate_type)
                            << " leaf requires full bundle, but full bundle not found at path: " << cert_dir;
            }

            if (leaf_single != nullptr) {
                certificate_file = leaf_single->at(0).get_file();
                EVLOG_debug << "Leaf single: [" << certificate_file.value_or("INVALID") << "]";
            } else {
                EVLOG_debug << conversions::leaf_certificate_type_to_string(certificate_type)
                            << " single leaf not found at path: " << cert_dir;
            }

            // Both require the hierarchy build
            if (params.include_ocsp || params.include_root) {
                X509CertificateBundle root_bundle(root_dir, EncodingFormat::PEM); // Required for hierarchy

                // The hierarchy is required for both roots and the OCSP cache
                auto hierarchy = X509CertificateHierarchy::build_hierarchy(root_bundle.split(), leaf_directory.split());
                EVLOG_debug << "Hierarchy for root/OCSP data: \n" << hierarchy.to_debug_string();

                // Include OCSP data if possible
                if (params.include_ocsp) {
                    // Search for OCSP data for each certificate
                    if (leaf_fullchain != nullptr) {
                        for (const auto& chain_certif : *leaf_fullchain) {
                            CertificateHashData hash;
                            if (hierarchy.get_certificate_hash(chain_certif, hash)) {
                                std::optional<fs::path> data = retrieve_ocsp_cache_internal(hash);
                                certificate_ocsp.push_back({hash, data});
                            } else {
                                // Always add to preserve file order
                                certificate_ocsp.push_back({{}, std::nullopt});
                            }
                        }
                    } else {
                        CertificateHashData hash;
                        if (hierarchy.get_certificate_hash(leaf_single->at(0), hash)) {
                            certificate_ocsp.push_back({hash, retrieve_ocsp_cache_internal(hash)});
                        }
                    }
                }

                // Include root data if possible
                if (params.include_root) {
                    // Search for the root of any of the leafs
                    // present either in the chain or single
                    std::optional<X509Wrapper> leafs_root_cert = hierarchy.find_certificate_root(
                        (leaf_fullchain != nullptr) ? leaf_fullchain->at(0) : leaf_single->at(0));

                    if (leafs_root_cert.has_value()) {
                        // Append the root
                        leafs_root = leafs_root_cert.value().get_export_string();
                    } else {
                        EVLOG_warning << "Root required for ["
                                      << conversions::leaf_certificate_type_to_string(certificate_type)
                                      << "] leaf certificate, but no root could be found";
                    }
                }
            }

            CertificateInfo info;

            info.key = key_file;
            info.certificate = chain_file;
            info.certificate_single = certificate_file;
            info.certificate_count = chain_len;
            info.password = this->private_key_password;

            if (params.include_ocsp) {
                info.ocsp = certificate_ocsp;
            }

            if (params.include_root && leafs_root.has_value()) {
                info.certificate_root = leafs_root.value();
            }

            // Add it to the returned result list
            result.info.push_back(info);
            result.status = GetCertificateInfoStatus::Accepted;
        } // End valid leaf iteration

        return result;
    } catch (const CertificateLoadException& e) {
        EVLOG_warning << "Leaf certificate load exception";
        result.status = GetCertificateInfoStatus::NotFound;
        return result;
    }

    result.status = GetCertificateInfoStatus::NotFound;
    return result;
}

bool EvseSecurity::update_certificate_links(LeafCertificateType certificate_type) {
    bool changed = false;

    if (certificate_type != LeafCertificateType::V2G) {
        throw std::runtime_error("Link updating only supported for V2G certificates");
    }

    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    fs::path cert_link_path = this->links.secc_leaf_cert_link;
    fs::path key_link_path = this->links.secc_leaf_key_link;
    fs::path chain_link_path = this->links.cpo_cert_chain_link;

    // Get the most recent valid certificate (internal since we already locked mutex)
    const auto key_pair = this->get_leaf_certificate_info_internal(certificate_type, EncodingFormat::PEM);
    if ((key_pair.status == GetCertificateInfoStatus::Accepted) && key_pair.info.has_value()) {

        // Create or update symlinks to SECC leaf cert
        if (!cert_link_path.empty()) {
            std::optional<fs::path> cert_path = key_pair.info.value().certificate_single;

            if (cert_path.has_value()) {
                if (fs::is_symlink(cert_link_path)) {
                    if (fs::read_symlink(cert_link_path) != cert_path.value()) {
                        fs::remove(cert_link_path);
                        changed = true;
                    }
                }
                if (!fs::exists(cert_link_path)) {
                    EVLOG_debug << "SECC cert link: " << cert_link_path << " -> " << cert_path.value();
                    fs::create_symlink(cert_path.value(), cert_link_path);
                    changed = true;
                }
            }
        }

        // Create or update symlinks to SECC leaf key
        if (!key_link_path.empty()) {
            fs::path key_path = key_pair.info.value().key;
            if (fs::is_symlink(key_link_path)) {
                if (fs::read_symlink(key_link_path) != key_path) {
                    fs::remove(key_link_path);
                    changed = true;
                }
            }
            if (!fs::exists(key_link_path)) {
                EVLOG_debug << "SECC key link: " << key_link_path << " -> " << key_path;
                fs::create_symlink(key_path, key_link_path);
                changed = true;
            }
        }

        // Create or update symlinks to CPO chain
        if (key_pair.info.value().certificate.has_value()) {
            fs::path chain_path = key_pair.info.value().certificate.value();
            if (!chain_link_path.empty()) {
                if (fs::is_symlink(chain_link_path)) {
                    if (fs::read_symlink(chain_link_path) != chain_path) {
                        fs::remove(chain_link_path);
                        changed = true;
                    }
                }
                if (!fs::exists(chain_link_path)) {
                    EVLOG_debug << "CPO cert chain link: " << chain_link_path << " -> " << chain_path;
                    fs::create_symlink(chain_path, chain_link_path);
                    changed = true;
                }
            }
        }
    } else {
        // Remove existing symlinks if no valid certificate is found
        if (!cert_link_path.empty() && fs::is_symlink(cert_link_path)) {
            fs::remove(cert_link_path);
            changed = true;
        }
        if (!key_link_path.empty() && fs::is_symlink(key_link_path)) {
            fs::remove(key_link_path);
            changed = true;
        }
        if (!chain_link_path.empty() && fs::is_symlink(chain_link_path)) {
            fs::remove(chain_link_path);
            changed = true;
        }
    }

    return changed;
}

GetCertificateInfoResult EvseSecurity::get_ca_certificate_info_internal(CaCertificateType certificate_type) {
    GetCertificateInfoResult result{};

    try {
        // Support bundle files, in case the certificates contain
        // multiple entries (should be 3) as per the specification
        X509CertificateBundle verify_file(this->ca_bundle_path_map.at(certificate_type), EncodingFormat::PEM);

        EVLOG_info << "Requesting certificate file: [" << conversions::ca_certificate_type_to_string(certificate_type)
                   << "] file:" << verify_file.get_path();

        // If we are using a directory, search for the first valid root file
        if (verify_file.is_using_directory()) {
            auto& hierarchy = verify_file.get_certificate_hierarchy();

            // Get all roots and search for a valid self-signed
            for (auto& root : hierarchy.get_hierarchy()) {
                if (root.certificate.is_selfsigned() && root.certificate.is_valid()) {
                    CertificateInfo info;
                    info.certificate = root.certificate.get_file().value();
                    info.certificate_single = root.certificate.get_file().value();

                    result.info = info;
                    result.status = GetCertificateInfoStatus::Accepted;
                    return result;
                }
            }
        } else {
            CertificateInfo info;
            info.certificate = verify_file.get_path();
            info.certificate_single = verify_file.get_path();

            result.info = info;
            result.status = GetCertificateInfoStatus::Accepted;
            return result;
        }
    } catch (const CertificateLoadException& e) {
        EVLOG_error << "Could not obtain verify file, wrong format for certificate: "
                    << this->ca_bundle_path_map.at(certificate_type) << " with error: " << e.what();
    }

    EVLOG_error << "Could not find any CA certificate for: "
                << conversions::ca_certificate_type_to_string(certificate_type);

    result.status = GetCertificateInfoStatus::NotFound;
    return result;
}

GetCertificateInfoResult EvseSecurity::get_ca_certificate_info(CaCertificateType certificate_type) {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    return get_ca_certificate_info_internal(certificate_type);
}

std::string EvseSecurity::get_verify_file(CaCertificateType certificate_type) {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    auto result = get_ca_certificate_info_internal(certificate_type);

    if (result.status == GetCertificateInfoStatus::Accepted && result.info.has_value()) {
        if (result.info.value().certificate.has_value()) {
            return result.info.value().certificate.value().string();
        }
    }

    return {};
}

std::string EvseSecurity::get_verify_location(CaCertificateType certificate_type) {

    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    try {
        // Support bundle files, in case the certificates contain
        // multiple entries (should be 3) as per the specification
        X509CertificateBundle verify_location(this->ca_bundle_path_map.at(certificate_type), EncodingFormat::PEM);

        const auto location_path = verify_location.get_path();

        EVLOG_info << "Requesting certificate location: ["
                   << conversions::ca_certificate_type_to_string(certificate_type) << "] location:" << location_path;

        if (!verify_location.empty() &&
            (!verify_location.is_using_directory() || hash_dir(location_path.c_str()) == 0)) {
            return location_path;
        }

    } catch (const CertificateLoadException& e) {
        EVLOG_error << "Could not obtain verify location, wrong format for certificate: "
                    << this->ca_bundle_path_map.at(certificate_type) << " with error: " << e.what();
    }

    EVLOG_error << "Could not find any CA certificate for: "
                << conversions::ca_certificate_type_to_string(certificate_type);

    return {};
}

int EvseSecurity::get_leaf_expiry_days_count(LeafCertificateType certificate_type) {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    EVLOG_info << "Requesting certificate expiry: " << conversions::leaf_certificate_type_to_string(certificate_type);

    // Internal since we already locked mutex
    const auto key_pair = this->get_leaf_certificate_info_internal(certificate_type, EncodingFormat::PEM, false);
    if (key_pair.status == GetCertificateInfoStatus::Accepted) {
        try {
            fs::path certificate_path;

            if (key_pair.info.has_value()) {
                if (key_pair.info.value().certificate.has_value()) {
                    certificate_path = key_pair.info.value().certificate.value();
                } else {
                    certificate_path = key_pair.info.value().certificate_single.value();
                }
            }

            if (certificate_path.empty() == false) {
                // In case it is a bundle, we know the leaf is always the first
                X509CertificateBundle cert(certificate_path, EncodingFormat::PEM);

                int64_t seconds = cert.split().at(0).get_valid_to();
                return std::chrono::duration_cast<days_to_seconds>(std::chrono::seconds(seconds)).count();
            }
        } catch (const CertificateLoadException& e) {
            EVLOG_error << "Could not obtain leaf expiry certificate: " << e.what();
        }
    }

    return 0;
}

bool EvseSecurity::verify_file_signature(const fs::path& path, const std::string& signing_certificate,
                                         const std::string signature) {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    EVLOG_info << "Verifying file signature for " << path.string();

    std::vector<std::uint8_t> sha256_digest;

    if (false == CryptoSupplier::digest_file_sha256(path, sha256_digest)) {
        EVLOG_error << "Error during digesting file: " << path;
        return false;
    }

    std::vector<std::uint8_t> signature_decoded;

    if (false == CryptoSupplier::base64_decode_to_bytes(signature, signature_decoded)) {
        EVLOG_error << "Error during decoding signature: " << signature;
        return false;
    }

    try {
        X509Wrapper x509_signing_cerificate(signing_certificate, EncodingFormat::PEM);

        if (CryptoSupplier::x509_verify_signature(x509_signing_cerificate.get(), signature_decoded, sha256_digest)) {
            EVLOG_debug << "Signature successful verification";
            return true;
        } else {
            EVLOG_error << "Failure to verify signature";
            return false;
        }
    } catch (const CertificateLoadException& e) {
        EVLOG_error << "Could not parse signing certificate: " << e.what();
        return false;
    }

    return false;
}

std::vector<std::uint8_t> EvseSecurity::base64_decode_to_bytes(const std::string& base64_string) {
    std::vector<std::uint8_t> decoded_bytes;

    if (false == CryptoSupplier::base64_decode_to_bytes(base64_string, decoded_bytes)) {
        return {};
    }

    return decoded_bytes;
}

std::string EvseSecurity::base64_decode_to_string(const std::string& base64_string) {
    std::string decoded_string;

    if (false == CryptoSupplier::base64_decode_to_string(base64_string, decoded_string)) {
        return {};
    }

    return decoded_string;
}

std::string EvseSecurity::base64_encode_from_bytes(const std::vector<std::uint8_t>& bytes) {
    std::string encoded_string;

    if (false == CryptoSupplier::base64_encode_from_bytes(bytes, encoded_string)) {
        return {};
    }

    return encoded_string;
}

std::string EvseSecurity::base64_encode_from_string(const std::string& string) {
    std::string encoded_string;

    if (false == CryptoSupplier::base64_encode_from_string(string, encoded_string)) {
        return {};
    }

    return encoded_string;
}

CertificateValidationResult EvseSecurity::verify_certificate(const std::string& certificate_chain,
                                                             LeafCertificateType certificate_type) {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    return verify_certificate_internal(certificate_chain, {certificate_type});
}

CertificateValidationResult
EvseSecurity::verify_certificate(const std::string& certificate_chain,
                                 const std::vector<LeafCertificateType>& certificate_types) {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);
    return verify_certificate_internal(certificate_chain, certificate_types);
}

CertificateValidationResult
EvseSecurity::verify_certificate_internal(const std::string& certificate_chain,
                                          const std::vector<LeafCertificateType>& certificate_types) {

    EVLOG_info << "Verifying leaf certificate";

    std::set<CaCertificateType> ca_certificate_types;

    for (const auto& cert_type : certificate_types) {
        EVLOG_info << "Including trust anchor for leaf certificate: "
                   << conversions::leaf_certificate_type_to_string(cert_type);

        switch (cert_type) {
        case LeafCertificateType::CSMS:
            ca_certificate_types.insert(CaCertificateType::CSMS);
            break;
        case LeafCertificateType::V2G:
            ca_certificate_types.insert(CaCertificateType::V2G);
            break;
        case LeafCertificateType::MF:
            ca_certificate_types.insert(CaCertificateType::MF);
            break;
        case LeafCertificateType::MO:
            ca_certificate_types.insert(CaCertificateType::MO);
            break;
        default:
            EVLOG_warning << "Unknown LeafCertificateType provided. Skipping.";
            break;
        }
    }

    if (ca_certificate_types.empty()) {
        EVLOG_warning << "No valid CA certificate types could be determined from leaf types.";
        return CertificateValidationResult::IssuerNotFound;
    }

    try {
        X509CertificateBundle certificate(certificate_chain, EncodingFormat::PEM);

        std::vector<X509Wrapper> _certificate_chain = certificate.split();
        if (_certificate_chain.empty()) {
            return CertificateValidationResult::Unknown;
        }

        // The leaf is to be verified
        const auto leaf_certificate = _certificate_chain.at(0);

        // Retrieve the hierarchy in order to check if the chain contains a root certificate
        X509CertificateHierarchy& hierarchy = certificate.get_certificate_hierarchy();

        // Build all untrusted intermediary certificates, and exclude any root
        std::vector<X509Handle*> untrusted_subcas;

        if (_certificate_chain.size() > 1) {
            // Iterate ignoring the received leaf (that is the first certificate)
            for (size_t i = 1; i < _certificate_chain.size(); i++) {
                const auto& cert = _certificate_chain[i];
                // Ignore the received certificate is somehow self-signed
                if (cert.is_selfsigned()) {
                    EVLOG_warning << "Ignore root certificate: " << cert.get_common_name();
                } else {
                    untrusted_subcas.emplace_back(cert.get());
                }
            }
        }

        // Build the trusted parent certificates from our internal store
        std::vector<X509Wrapper> trusted_wrappers;
        std::vector<X509Handle*> trusted_parent_certificates;

        for (const auto& ca_type : ca_certificate_types) {
            if (!is_ca_certificate_installed_internal(ca_type)) {
                continue;
            }

            const auto root_store = this->ca_bundle_path_map.at(ca_type);
            if (fs::is_directory(root_store)) {
                // In case of a directory load the certificates manually and add them
                // to the parent certificates
                X509CertificateBundle roots(root_store, EncodingFormat::PEM);

                // We use a root chain instead of relying on OpenSSL since that requires to have
                // the name of the certificates in the format "hash.0", hash being the subject hash
                // or to have symlinks in the mentioned format to the certificates in the directory
                std::vector<X509Wrapper> root_chain = roots.split();

                for (auto& root_cert : root_chain) {
                    trusted_parent_certificates.emplace_back(root_cert.get());
                    trusted_wrappers.emplace_back(std::move(root_cert)); // Keep wrappers alive
                }
            } else {
                X509CertificateBundle roots(root_store, EncodingFormat::PEM);
                std::vector<X509Wrapper> root_chain = roots.split();

                for (auto& root_cert : root_chain) {
                    trusted_parent_certificates.emplace_back(root_cert.get());
                    trusted_wrappers.emplace_back(std::move(root_cert));
                }
            }
        }

        if (trusted_parent_certificates.empty()) {
            return CertificateValidationResult::IssuerNotFound;
        }

        return CryptoSupplier::x509_verify_certificate_chain(leaf_certificate.get(), trusted_parent_certificates,
                                                             untrusted_subcas, true, std::nullopt, std::nullopt);

    } catch (const CertificateLoadException& e) {
        EVLOG_warning << "Could not validate certificate chain because of invalid format";
        return CertificateValidationResult::Unknown;
    }
}

void EvseSecurity::garbage_collect() {
    std::lock_guard<std::mutex> guard(EvseSecurity::security_mutex);

    // Only garbage collect if we are full
    if (is_filesystem_full() == false) {
        EVLOG_debug << "Garbage collect postponed, filesystem is not full";
        return;
    }

    EVLOG_info << "Starting garbage collect!";

    std::vector<std::tuple<fs::path, fs::path, CaCertificateType>> leaf_paths;

    leaf_paths.push_back(std::make_tuple(this->directories.csms_leaf_cert_directory,
                                         this->directories.csms_leaf_key_directory, CaCertificateType::CSMS));
    leaf_paths.push_back(std::make_tuple(this->directories.secc_leaf_cert_directory,
                                         this->directories.secc_leaf_key_directory, CaCertificateType::V2G));

    // Delete certificates first, give the option to cleanup the dangling keys afterwards
    std::set<fs::path> invalid_certificate_files;

    // Private keys that are linked to the skipped certificates and that will not be deleted regardless
    std::set<fs::path> protected_private_keys;

    // Order by latest valid, and keep newest with a safety limit
    for (auto const& [cert_dir, key_dir, ca_type] : leaf_paths) {
        // Root bundle required for hash of OCSP cache
        try {
            X509CertificateBundle root_bundle(ca_bundle_path_map[ca_type], EncodingFormat::PEM);
            X509CertificateBundle expired_certs(cert_dir, EncodingFormat::PEM);

            // Only handle if we have more than the minimum certificates entry
            if (expired_certs.get_certificate_chains_count() > DEFAULT_MINIMUM_CERTIFICATE_ENTRIES) {
                fs::path key_directory = key_dir;
                int skipped = 0;

                // Order by expiry date, and keep even expired certificates with a minimum of 10 certificates
                expired_certs.for_each_chain_ordered(
                    [this, &invalid_certificate_files, &skipped, &key_directory, &protected_private_keys,
                     &root_bundle](const fs::path& file, const std::vector<X509Wrapper>& chain) {
                        // By default delete all empty
                        if (chain.size() <= 0) {
                            invalid_certificate_files.emplace(file);
                            return true;
                        }

                        if (++skipped > DEFAULT_MINIMUM_CERTIFICATE_ENTRIES) {
                            // If the chain contains the first expired (leafs are the first)
                            if (chain[0].is_expired()) {
                                invalid_certificate_files.emplace(file);

                                // Also attempt to add the key for deletion
                                auto key_file = get_private_key_path_of_certificate(chain[0], key_directory,
                                                                                    this->private_key_password);
                                if (key_file.has_value()) {
                                    // Found key, delete it too
                                    invalid_certificate_files.emplace(key_file.value());
                                }

                                auto leaf_chain = chain;
                                X509CertificateHierarchy hierarchy = std::move(
                                    X509CertificateHierarchy::build_hierarchy(root_bundle.split(), leaf_chain));

                                CertificateHashData ocsp_hash;
                                fs::path path_ocsp_hash, patch_ocsp_data;

                                if (hierarchy.get_certificate_hash(chain[0], ocsp_hash) &&
                                    get_oscp_data_of_certificate(chain[0], ocsp_hash, path_ocsp_hash,
                                                                 patch_ocsp_data)) {

                                    // OCSP cache found, delete the files
                                    invalid_certificate_files.emplace(path_ocsp_hash);
                                    invalid_certificate_files.emplace(patch_ocsp_data);
                                }
                            }
                        } else {
                            // Add to protected certificate list
                            auto key_file = get_private_key_path_of_certificate(chain[0], key_directory,
                                                                                this->private_key_password);

                            if (key_file.has_value()) {
                                protected_private_keys.emplace(key_file.value());

                                // Erase all protected keys from the managed CRSs
                                auto it = managed_csr.find(key_file.value());
                                if (it != managed_csr.end()) {
                                    managed_csr.erase(it);
                                }
                            }
                        }

                        return true;
                    },
                    [](const std::vector<X509Wrapper>& a, const std::vector<X509Wrapper>& b) {
                        // Order from newest to oldest (newest DEFAULT_MINIMUM_CERTIFICATE_ENTRIES) are kept
                        // even if they are expired
                        if (a.size() && b.size()) {
                            return a.at(0).get_valid_to() > b.at(0).get_valid_to();
                        } else {
                            return false;
                        }
                    });
            }
        } catch (const CertificateLoadException& e) {
            EVLOG_warning << "Could not load bundle from file: " << e.what();
        }
    } // End leaf for iteration

    for (const auto& expired_certificate_file : invalid_certificate_files) {
        if (filesystem_utils::delete_file(expired_certificate_file))
            EVLOG_info << "Deleted expired certificate file: " << expired_certificate_file;
        else
            EVLOG_warning << "Error deleting expired certificate file: " << expired_certificate_file;
    }

    // In case of a reset, the managed CSRs can be lost. In that case add them back to the list
    // to give the change of a CSR to be fulfilled. Eventually the GC will delete those CSRs
    // at a further invocation after the GC timer will elapse a few times. This behavior
    // was added so that if we have a reset and the CSMS sends us a CSR response while we were
    // down it should still be processed when we boot up and NOT delete the CSRs
    for (auto const& [cert_dir, keys_dir, ca_type] : leaf_paths) {
        fs::path cert_path = cert_dir;
        fs::path key_path = keys_dir;

        for (const auto& key_entry : fs::recursive_directory_iterator(key_path)) {
            auto key_file_path = key_entry.path();

            // Skip protected keys
            if (protected_private_keys.find(key_file_path) != protected_private_keys.end()) {
                continue;
            }

            if (is_keyfile(key_file_path)) {
                bool error = false;

                try {
                    // Check if we have found any matching certificate
                    auto key_path = get_certificate_path_of_key(key_file_path, keys_dir, this->private_key_password);
                } catch (const NoCertificateValidException& e) {
                    // If we did not found, add to the potential delete list
                    EVLOG_debug << "Could not find matching certificate for key: " << key_file_path
                                << " adding to potential deletes";
                    error = true;
                } catch (const NoPrivateKeyException& e) {
                    EVLOG_debug << "Could not load private key: " << key_file_path << " adding to potential deletes";
                    error = true;
                }

                if (error) {
                    // Give a chance to be fulfilled by the CSMS
                    if (managed_csr.find(key_file_path) == managed_csr.end()) {
                        managed_csr.emplace(key_file_path, std::chrono::steady_clock::now());
                    }
                }
            }
        }
    }

    // Delete all managed private keys of a CSR that we did not had a response to
    auto now_timepoint = std::chrono::steady_clock::now();

    // The update_leaf_certificate function is responsible for removing responded CSRs from this managed list
    for (auto it = managed_csr.begin(); it != managed_csr.end();) {
        std::chrono::seconds elapsed = std::chrono::duration_cast<std::chrono::seconds>(now_timepoint - it->second);

        if (elapsed > csr_expiry) {
            EVLOG_debug << "Found expired csr key, deleting: " << it->first;
            filesystem_utils::delete_file(it->first);

            it = managed_csr.erase(it);
        } else {
            ++it;
        }
    }

    std::set<fs::path> invalid_ocsp_files;

    // Delete all non-owned OCSP data
    for (const auto& leaf_certificate_path :
         {directories.secc_leaf_cert_directory, directories.csms_leaf_cert_directory}) {
        try {
            bool secc = (leaf_certificate_path == directories.secc_leaf_cert_directory);
            bool csms = (leaf_certificate_path == directories.csms_leaf_cert_directory) ||
                        (directories.csms_leaf_cert_directory == directories.secc_leaf_cert_directory);

            CaCertificateType load;

            if (secc)
                load = CaCertificateType::V2G;
            else if (csms)
                load = CaCertificateType::CSMS;

            // Also load the roots since we need to build the hierarchy for correct certificate hashes
            X509CertificateBundle root_bundle(ca_bundle_path_map[load], EncodingFormat::PEM);
            X509CertificateBundle leaf_bundle(leaf_certificate_path, EncodingFormat::PEM);

            fs::path leaf_ocsp;
            fs::path root_ocsp;

            if (root_bundle.is_using_bundle_file()) {
                root_ocsp = root_bundle.get_path().parent_path() / "ocsp";
            } else {
                root_ocsp = root_bundle.get_path() / "ocsp";
            }

            if (leaf_bundle.is_using_bundle_file()) {
                leaf_ocsp = leaf_bundle.get_path().parent_path() / "ocsp";
            } else {
                leaf_ocsp = leaf_bundle.get_path() / "ocsp";
            }

            X509CertificateHierarchy hierarchy =
                std::move(X509CertificateHierarchy::build_hierarchy(root_bundle.split(), leaf_bundle.split()));

            // Iterate all hashes folders and see if any are missing
            for (auto& ocsp_dir : {leaf_ocsp, root_ocsp}) {
                if (fs::exists(ocsp_dir)) {
                    for (auto& ocsp_entry : fs::directory_iterator(ocsp_dir)) {
                        if (ocsp_entry.is_regular_file() == false) {
                            continue;
                        }

                        // Attempt hash read
                        CertificateHashData read_hash;

                        if (filesystem_utils::read_hash_from_file(ocsp_entry.path(), read_hash)) {
                            // If we can't find the has, it means it was deleted somehow, add to delete list
                            if (hierarchy.contains_certificate_hash(read_hash, true) == false) {
                                auto oscp_data_path = ocsp_entry.path();
                                oscp_data_path.replace_extension(DER_EXTENSION);

                                invalid_ocsp_files.emplace(ocsp_entry.path());
                                invalid_ocsp_files.emplace(oscp_data_path);
                            }
                        }
                    }
                }
            }
        } catch (const CertificateLoadException& e) {
            EVLOG_warning << "Could not load ca bundle from file: " << leaf_certificate_path;
        }
    }

    for (const auto& invalid_ocsp : invalid_ocsp_files) {
        if (filesystem_utils::delete_file(invalid_ocsp))
            EVLOG_info << "Deleted invalid ocsp file: " << invalid_ocsp;
        else
            EVLOG_warning << "Error deleting invalid ocsp file: " << invalid_ocsp;
    }
}

bool EvseSecurity::is_filesystem_full() {
    std::set<fs::path> unique_paths;

    // Collect all bundles
    for (auto const& [certificate_type, ca_bundle_path] : ca_bundle_path_map) {
        if (fs::is_regular_file(ca_bundle_path)) {
            unique_paths.emplace(ca_bundle_path);
        } else if (fs::is_directory(ca_bundle_path)) {
            for (const auto& entry : fs::recursive_directory_iterator(ca_bundle_path)) {
                if (fs::is_regular_file(entry)) {
                    unique_paths.emplace(entry);
                }
            }
        }
    }

    // Collect all key/leafs
    std::vector<fs::path> key_pairs;

    key_pairs.push_back(directories.csms_leaf_cert_directory);
    key_pairs.push_back(directories.csms_leaf_key_directory);
    key_pairs.push_back(directories.secc_leaf_cert_directory);
    key_pairs.push_back(directories.secc_leaf_key_directory);

    for (auto const& directory : key_pairs) {
        if (fs::is_regular_file(directory)) {
            unique_paths.emplace(directory);
        } else if (fs::is_directory(directory)) {
            for (const auto& entry : fs::recursive_directory_iterator(directory)) {
                if (fs::is_regular_file(entry)) {
                    unique_paths.emplace(entry);
                }
            }
        }
    }

    uintmax_t total_entries = unique_paths.size();
    EVLOG_debug << "Total entries used: " << total_entries;

    if (total_entries > max_fs_certificate_store_entries) {
        EVLOG_warning << "Exceeded maximum entries: " << max_fs_certificate_store_entries << " with :" << total_entries
                      << " total entries";
        return true;
    }

    uintmax_t total_size_bytes = 0;
    for (const auto& path : unique_paths) {
        total_size_bytes = fs::file_size(path);
    }

    EVLOG_debug << "Total bytes used: " << total_size_bytes;
    if (total_size_bytes >= max_fs_usage_bytes) {
        EVLOG_warning << "Exceeded maximum byte size: " << total_size_bytes;
        return true;
    }

    return false;
}

} // namespace evse_security
