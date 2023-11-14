// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <everest/logging.hpp>
#include <evse_utilities.hpp>
#include <x509_bundle.hpp>

#include <algorithm>
#include <openssl/err.h>
#include <openssl/x509v3.h>

namespace evse_security {

X509Wrapper X509CertificateBundle::get_latest_valid_certificate(const std::vector<X509Wrapper>& certificates) {
    // Filter certificates with valid_in > 0
    std::vector<X509Wrapper> valid_certificates;
    for (const auto& cert : certificates) {
        if (cert.is_valid()) {
            valid_certificates.push_back(cert);
        }
    }

    if (valid_certificates.empty()) {
        // No valid certificates found
        throw NoCertificateValidException("No valid certificates available.");
    }

    // Find the certificate with the latest valid_in
    auto latest_certificate = std::max_element(
        valid_certificates.begin(), valid_certificates.end(),
        [](const X509Wrapper& cert1, const X509Wrapper& cert2) { return cert1.get_valid_in() < cert2.get_valid_in(); });

    return *latest_certificate;
}

std::vector<X509_ptr> X509CertificateBundle::load_certificates(const std::string& data, const EncodingFormat encoding) {
    BIO_ptr bio(BIO_new_mem_buf(data.data(), static_cast<int>(data.size())));

    if (!bio) {
        throw CertificateLoadException("Failed to create BIO from data");
    }

    std::vector<X509_ptr> certificates;

    if (encoding == EncodingFormat::PEM) {
        STACK_OF(X509_INFO)* allcerts = PEM_X509_INFO_read_bio(bio.get(), nullptr, nullptr, nullptr);

        if (allcerts) {
            for (int i = 0; i < sk_X509_INFO_num(allcerts); i++) {
                X509_INFO* xi = sk_X509_INFO_value(allcerts, i);

                if (xi && xi->x509) {
                    // Transfer ownership
                    certificates.emplace_back(xi->x509);
                    xi->x509 = nullptr;
                }
            }

            sk_X509_INFO_pop_free(allcerts, X509_INFO_free);
        } else {
            throw CertificateLoadException("Certificate (PEM) parsing error");
        }
    } else if (encoding == EncodingFormat::DER) {
        X509* x509 = d2i_X509_bio(bio.get(), nullptr);

        if (x509) {
            certificates.emplace_back(x509);
        } else {
            throw CertificateLoadException("Certificate (DER) parsing error");
        }
    } else {
        throw CertificateLoadException("Unsupported encoding format");
    }

    return certificates;
}

X509CertificateBundle::X509CertificateBundle(const std::string& certificate, const EncodingFormat encoding) :
    hierarchy_invalidated(true), source(X509CertificateSource::STRING) {
    add_certifcates(certificate, encoding, std::nullopt);
}

X509CertificateBundle::X509CertificateBundle(const fs::path& path, const EncodingFormat encoding) :
    hierarchy_invalidated(true) {
    this->path = path;

    if (fs::is_directory(path)) {
        source = X509CertificateSource::DIRECTORY;

        // Iterate directory
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
            if (is_certificate_file(entry)) {
                std::string certificate;
                if (EvseUtils::read_from_file(entry.path(), certificate))
                    add_certifcates(certificate, encoding, entry.path());
            }
        }
    } else if (is_certificate_file(path)) {
        source = X509CertificateSource::FILE;

        std::string certificate;
        if (EvseUtils::read_from_file(path, certificate))
            add_certifcates(certificate, encoding, path);
    } else {
        throw CertificateLoadException("Failed to create certificate info from path: " + path.string());
    }
}

void X509CertificateBundle::add_certifcates(const std::string& data, const EncodingFormat encoding,
                                            const std::optional<fs::path>& path) {
    auto loaded = load_certificates(data, encoding);
    auto& list = certificates[path.value_or(std::filesystem::path())];

    for (auto& x509 : loaded) {
        if (path.has_value())
            list.emplace_back(std::move(x509), path.value());
        else
            list.emplace_back(std::move(x509));
    }
}

std::vector<X509Wrapper> X509CertificateBundle::split() {
    std::vector<X509Wrapper> full_certificates;

    // Append all chains
    for (const auto& chains : certificates) {
        for (const auto& cert : chains.second)
            full_certificates.push_back(cert);
    }

    return full_certificates;
}

bool X509CertificateBundle::contains_certificate(const X509Wrapper& certificate) {
    // Search through all the chains
    for (const auto& chain : certificates) {
        for (const auto& certif : chain.second) {
            if (certif == certificate)
                return true;
        }
    }

    return false;
}

bool X509CertificateBundle::contains_certificate(const CertificateHashData& certificate_hash) {
    // Try an initial search for root certificates, else a hierarchy build will be required
    for (const auto& chain : certificates) {
        bool found = std::find_if(std::begin(chain.second), std::end(chain.second), [&](const X509Wrapper& cert) {
                         return cert.is_selfsigned() && cert == certificate_hash;
                     }) != std::end(chain.second);

        if (found)
            return true;
    }

    // Nothing found, build the hierarchy and search by the issued hash
    X509CertificateHierarchy& hierarchy = get_certficate_hierarchy();
    return hierarchy.contains_certificate_hash(certificate_hash);
}

X509Wrapper X509CertificateBundle::find_certificate(const CertificateHashData& certificate_hash) {
    // Try an initial search for root certificates, else a hierarchy build will be required
    for (const auto& chain : certificates) {
        for (const auto& certif : chain.second) {
            if (certif.is_selfsigned() && certif == certificate_hash) {
                return certif;
            }
        }
    }

    // Nothing found, build the hierarchy and search by the issued hash
    X509CertificateHierarchy& hierarchy = get_certficate_hierarchy();
    return hierarchy.find_certificate(certificate_hash);
}

int X509CertificateBundle::delete_certificate(const X509Wrapper& certificate, bool include_issued) {
    std::vector<X509Wrapper> to_delete;

    if (include_issued) {
        // Include all descendants in the delete list
        auto& hierarchy = get_certficate_hierarchy();
        to_delete = hierarchy.collect_descendants(certificate);
    }

    // Include default delete
    to_delete.push_back(certificate);
    int deleted = 0;

    for (auto& chains : certificates) {
        auto& certifs = chains.second;

        certifs.erase(std::remove_if(certifs.begin(), certifs.end(),
                                     [&](const auto& certif) {
                                         bool found =
                                             std::find(to_delete.begin(), to_delete.end(), certif) != to_delete.end();

                                         if (found)
                                             deleted++;

                                         return found;
                                     }),
                      certifs.end());
    }

    // If we deleted any, invalidate the built hierarchy
    if (deleted) {
        invalidate_hierarchy();
    }

    return deleted;
}

int X509CertificateBundle::delete_certificate(const CertificateHashData& data, bool include_issued) {
    auto& hierarchy = get_certficate_hierarchy();

    try {
        // Try to find the certificate by correct hierarchy hash
        X509Wrapper to_delete = hierarchy.find_certificate(data);
        return delete_certificate(to_delete, include_issued);
    } catch (NoCertificateFound& e) {
    }

    return 0;
}

void X509CertificateBundle::delete_all_certificates() {
    certificates.clear();
}

void X509CertificateBundle::add_certificate(X509Wrapper&& certificate) {
    if (source == X509CertificateSource::DIRECTORY) {
        // If it is in directory mode only allow sub-directories of that directory
        std::filesystem::path certif_path = certificate.get_file().value_or(std::filesystem::path());

        if (EvseUtils::is_subdirectory(path, certif_path)) {
            certificates[certif_path].push_back(std::move(certificate));
            invalidate_hierarchy();
        } else {
            throw InvalidOperationException(
                "Added certificate with directory bundle, must be subdir of the main directory: " + path.string());
        }
    } else {
        // The bundle came from a file, so there is only one file we could add the certificate to
        certificates.begin()->second.push_back(certificate);
        invalidate_hierarchy();
    }
}

void X509CertificateBundle::add_certificate_unique(X509Wrapper&& certificate) {
    if (!contains_certificate(certificate)) {
        return add_certificate(std::move(certificate));
        invalidate_hierarchy();
    }
}

bool X509CertificateBundle::update_certificate(X509Wrapper&& certificate) {
    for (auto& chain : certificates) {
        for (auto& certif : chain.second) {
            if (certif == certificate) {
                certif = std::move(certificate);
                invalidate_hierarchy();

                return true;
            }
        }
    }

    return false;
}

bool X509CertificateBundle::export_certificates() {
    if (source == X509CertificateSource::STRING) {
        EVLOG_error << "Export for string is invalid!";
        return false;
    }

    // Add/delete certifs
    if (!sync_to_certificate_store()) {
        EVLOG_error << "Sync to certificate store failed!";
        return false;
    }

    if (source == X509CertificateSource::DIRECTORY) {
        bool exported_all = true;

        // Write updated certificates
        for (auto& chains : certificates) {
            // Ignore empty chains (the file was deleted)
            if (chains.second.empty())
                continue;

            // Each chain is a single file
            if (!EvseUtils::write_to_file(chains.first, to_export_string(chains.first), std::ios::trunc)) {
                exported_all = false;
            }
        }

        return exported_all;
    } else if (source == X509CertificateSource::FILE) {
        // We're using a single file, no need to check for deleted certificates
        return EvseUtils::write_to_file(path, to_export_string(), std::ios::trunc);
    }

    return false;
}

bool X509CertificateBundle::sync_to_certificate_store() {
    if (source == X509CertificateSource::STRING) {
        EVLOG_error << "Sync for string is invalid!";
        return false;
    }

    if (source == X509CertificateSource::DIRECTORY) {
        // Get existing certificates from filesystem
        X509CertificateBundle fs_certificates(path, EncodingFormat::PEM);
        bool success = true;

        // Delete filesystem certificate chains missing from our map
        for (const auto& fs_chain : fs_certificates.certificates) {
            if (certificates.find(fs_chain.first) == certificates.end()) {
                // fs certif chain not existing in our certificate list, delete
                if (!EvseUtils::delete_file(fs_chain.first))
                    success = false;
            }
        }

        // Add the certificates that are not existing in the filesystem. Each chain represents a single file
        for (const auto& chain : certificates) {
            if (chain.second.empty()) {
                // If it's an empty chain, delete
                if (!EvseUtils::delete_file(chain.first))
                    success = false;
            } else if (fs_certificates.certificates.find(chain.first) == fs_certificates.certificates.end()) {
                // Certif not existing in fs certificates write it out
                if (!EvseUtils::write_to_file(chain.first, to_export_string(chain.first), std::ios::trunc))
                    success = false;
            }
        }

        // After fs deletion erase all empty files from our certificate list, so that we don't write them out
        for (auto first = certificates.begin(); first != certificates.end();) {
            if (first->second.empty())
                first = certificates.erase(first);
            else
                ++first;
        }

        return success;
    } else if (source == X509CertificateSource::FILE) {
        // Delete source file if we're empty
        if (certificates.empty()) {
            return EvseUtils::delete_file(path);
        }

        return true;
    }

    return false;
}

X509Wrapper X509CertificateBundle::get_latest_valid_certificate() {
    return get_latest_valid_certificate(split());
}

void X509CertificateBundle::invalidate_hierarchy() {
    hierarchy_invalidated = true;
}

X509CertificateHierarchy& X509CertificateBundle::get_certficate_hierarchy() {
    if (hierarchy_invalidated) {
        hierarchy_invalidated = false;

        auto certificates = split();
        hierarchy = X509CertificateHierarchy::build_hierarchy(certificates);
    }

    return hierarchy;
}

std::string X509CertificateBundle::to_export_string() const {
    std::string export_string;

    for (auto& chain : certificates) {
        for (auto& certificate : chain.second) {
            export_string += certificate.get_export_string();
        }
    }

    return export_string;
}

std::string X509CertificateBundle::to_export_string(const std::filesystem::path& chain) const {
    std::string export_string;

    auto found = certificates.find(chain);
    if (found != certificates.end()) {
        for (auto& certificate : found->second)
            export_string += certificate.get_export_string();
    }

    return export_string;
}

} // namespace evse_security
