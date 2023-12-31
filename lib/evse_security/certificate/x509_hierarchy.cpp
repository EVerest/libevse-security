// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#include <evse_security/certificate/x509_hierarchy.hpp>

#include <algorithm>

namespace evse_security {

bool X509CertificateHierarchy::is_root(const X509Wrapper& certificate) const {
    if (certificate.is_selfsigned()) {
        return (std::find_if(hierarchy.begin(), hierarchy.end(), [&certificate](const X509Node& node) {
                    return node.certificate == certificate;
                }) != hierarchy.end());
    }

    return false;
}

std::vector<X509Wrapper> X509CertificateHierarchy::collect_descendants(const X509Wrapper& top) {
    std::vector<X509Wrapper> descendants;

    for_each([&](const X509Node& node) {
        // If we found the certificate
        if (node.certificate == top) {
            // Collect all descendants
            if (node.children.size()) {
                for_each_descendant(
                    [&](const X509Node& descendant, int depth) { descendants.push_back(descendant.certificate); },
                    node);
            }

            return false;
        }

        return true;
    });

    return descendants;
}

CertificateHashData X509CertificateHierarchy::get_certificate_hash(const X509Wrapper& certificate) {
    if (certificate.is_selfsigned()) {
        return certificate.get_certificate_hash_data();
    }

    // Search for certificate in the hierarchy and return the hash
    CertificateHashData hash;
    bool found;

    for_each([&](const X509Node& node) {
        if (node.certificate == certificate) {
            hash = node.hash;
            found = true;

            return false;
        }

        return true;
    });

    if (found)
        return hash;

    throw NoCertificateFound("Could not find owner for certificate: " + certificate.get_common_name());
}

bool X509CertificateHierarchy::contains_certificate_hash(const CertificateHashData& hash) {
    bool contains = false;

    for_each([&](const X509Node& node) {
        if (node.hash == hash) {
            contains = true;
            return false;
        }

        return true;
    });

    return contains;
}

X509Wrapper X509CertificateHierarchy::find_certificate(const CertificateHashData& hash) {
    X509Wrapper* certificate = nullptr;

    for_each([&](X509Node& node) {
        if (node.hash == hash) {
            certificate = &node.certificate;
            return false;
        }

        return true;
    });

    if (certificate)
        return *certificate;

    throw NoCertificateFound("Could not find a certificate for hash: " + hash.issuer_name_hash);
}

std::string X509CertificateHierarchy::to_debug_string() {
    std::stringstream str;

    for (const auto& root : hierarchy) {
        if (root.certificate.is_selfsigned())
            str << "* [ROOT]";
        else
            str << "+ [ORPH]";

        str << ' ' << root.certificate.get_common_name() << std::endl;

        for_each_descendant(
            [&](const X509Node& node, int depth) {
                while (depth-- > 0)
                    str << "---";

                str << ' ' << node.certificate.get_common_name() << std::endl;
            },
            root, 1);
    }

    return str.str();
}

bool X509CertificateHierarchy::try_add_to_hierarchy(X509Wrapper&& certificate) {
    bool added = false;

    for_each([&](X509Node& top) {
        if (certificate.is_child(top.certificate)) {
            auto hash = certificate.get_certificate_hash_data(top.certificate);
            top.children.push_back({std::move(certificate), hash, top.certificate, {}});
            added = true;

            return false;
        }

        return true;
    });

    return added;
}

X509CertificateHierarchy X509CertificateHierarchy::build_hierarchy(std::vector<X509Wrapper>& certificates) {
    X509CertificateHierarchy ordered;

    // Search for all self-signed certificates and add them as the roots, and also search for
    // all owner-less certificates and also add them as root, since they are  not self-signed
    // but we can't find any owner for them anyway
    std::for_each(certificates.begin(), certificates.end(), [&](const X509Wrapper& certif) {
        if (certif.is_selfsigned()) {
            ordered.hierarchy.push_back({certif, certif.get_certificate_hash_data(), certif, {}});
        } else {
            // Search for a possible owner
            bool has_owner = std::find_if(certificates.begin(), certificates.end(), [&](const X509Wrapper& owner) {
                                 return certif.is_child(owner);
                             }) != certificates.end();

            if (!has_owner) {
                // If we don't have an owner we can't determine the proper hash, use invalid
                // We can identify the invalid hash data by the fact that its hash strings are empty
                CertificateHashData invalid;
                // Set the hash algorithm, leaving it undefined leads to UB
                invalid.hash_algorithm = HashAlgorithm::SHA256;
                ordered.hierarchy.push_back({certif, invalid, certif, {}});
            }
        }
    });

    // Remove all root certificates from the provided certificate list
    auto remove_roots = std::remove_if(certificates.begin(), certificates.end(), [&](const X509Wrapper& certif) {
        return std::find_if(ordered.hierarchy.begin(), ordered.hierarchy.end(), [&](const X509Node& node) {
                   return (certif == node.certificate);
               }) != ordered.hierarchy.end();
    });

    certificates.erase(remove_roots, certificates.end());

    // Try build the full hierarchy, we are not assuming any order
    // This will not get stuck in a loop, because we removed the orphaned certificates already
    // Note: the logic is fairly simple here, but has worst-case cubic runtime in the number of certs
    while (certificates.size()) {
        // The current certificate that we're testing for
        auto current = std::move(certificates.back());
        certificates.pop_back();

        // If we have any roots try and search in the hierarchy the owner
        if (!ordered.try_add_to_hierarchy(std::move(current))) {
            // If we couldn't add to the hierarchy move it down in the queue and try again later
            certificates.insert(certificates.begin(), std::move(current));
        }
    }

    return ordered;
}

} // namespace evse_security
