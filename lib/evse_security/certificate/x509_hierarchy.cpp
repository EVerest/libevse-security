// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#include <evse_security/certificate/x509_hierarchy.hpp>

#include <algorithm>

#include <everest/logging.hpp>
namespace evse_security {

bool X509CertificateHierarchy::is_internal_root(const X509Wrapper& certificate) const {
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

std::vector<X509Wrapper> X509CertificateHierarchy::collect_top(const X509Wrapper& leaf) {
    auto root_node = find_certificate_root_node(leaf);

    if (root_node.has_value()) {
        std::vector<X509Wrapper> top_nodes;
        auto tuple = root_node.value();

        const X509Node* root = std::get<0>(tuple);
        int found_depth = std::get<1>(tuple);

        // Iterate all the descendants of the root until we find the leaf level
        for_each_descendant(
            [&](const X509Node& node, int depth) {
                if (depth < found_depth)
                    // Collect all owned
                    top_nodes.push_back(node.certificate);
            },
            *root, 1);

        return top_nodes;
    }

    return {};
}

bool X509CertificateHierarchy::get_certificate_hash(const X509Wrapper& certificate, CertificateHashData& out_hash) {
    if (certificate.is_selfsigned()) {
        out_hash = certificate.get_certificate_hash_data();
        return true;
    }

    // Search for certificate in the hierarchy and return the hash
    CertificateHashData hash;
    bool found;

    for_each([&](const X509Node& node) {
        if (node.certificate == certificate && node.hash.has_value()) {
            hash = node.hash.value();
            found = true;

            return false;
        }

        return true;
    });

    if (found) {
        out_hash = std::move(hash);
        return true;
    }

    EVLOG_warning << "Could not find owner for certificate: " << certificate.get_common_name();
    return false;
}

bool X509CertificateHierarchy::contains_certificate_hash(const CertificateHashData& hash,
                                                         bool case_insensitive_comparison) {
    bool contains = false;

    for_each([&](const X509Node& node) {
        if (node.hash.has_value()) {
            bool matches = false;

            if (case_insensitive_comparison) {
                matches = (node.hash.value().case_insensitive_comparison(hash));
            } else {
                matches = (node.hash.value() == hash);
            }

            if (matches) {
                contains = true;
                return false;
            }
        }

        return true;
    });

    return contains;
}

std::optional<X509Wrapper> X509CertificateHierarchy::find_certificate_root(const X509Wrapper& leaf) {
    auto root = find_certificate_root_node(leaf);

    if (root.has_value()) {
        auto root_ptr = std::get<0>(root.value());
        return root_ptr->certificate;
    }

    return std::nullopt;
}

std::optional<std::pair<const X509Node*, int>>
X509CertificateHierarchy::find_certificate_root_node(const X509Wrapper& leaf) {
    const X509Node* root_ptr = nullptr;
    int found_depth;

    for (const auto& root : hierarchy) {
        if (root.state.is_selfsigned) {
            for_each_descendant(
                [&](const X509Node& node, int depth) {
                    // If we found our matching certificate, we also found the root
                    if (node.certificate == leaf) {
                        root_ptr = &root;
                        found_depth = depth;
                    }
                },
                root, 1);
        }
    }

    if (root_ptr)
        return std::make_pair(root_ptr, found_depth);

    return std::nullopt;
}

std::optional<X509Wrapper> X509CertificateHierarchy::find_certificate(const CertificateHashData& hash,
                                                                      bool case_insensitive_comparison) {
    X509Wrapper* certificate = nullptr;

    for_each([&](X509Node& node) {
        if (node.hash.has_value()) {
            bool matches = false;

            if (case_insensitive_comparison) {
                matches = (node.hash.value().case_insensitive_comparison(hash));
            } else {
                matches = (node.hash.value() == hash);
            }

            if (matches) {
                certificate = &node.certificate;
                return false;
            }
        }

        return true;
    });

    if (certificate)
        return *certificate;

    return std::nullopt;
}

std::vector<X509Wrapper> X509CertificateHierarchy::find_certificates_multi(const CertificateHashData& hash) {
    std::vector<X509Wrapper> certificates;

    for_each([&](X509Node& node) {
        if (node.hash == hash) {
            certificates.push_back(node.certificate);
        }

        return true;
    });

    return certificates;
}

std::string X509CertificateHierarchy::to_debug_string() {
    std::stringstream str;

    for (const auto& root : hierarchy) {
        if (root.state.is_selfsigned)
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

void X509CertificateHierarchy::insert(X509Wrapper&& inserted_certificate) {
    if (false == inserted_certificate.is_selfsigned()) {
        // If this certif has any link to any of the existing certificates
        bool hierarchy_found = false;

        // Create a new node, is not self-signed and is not a permanent orphan
        X509Node new_node = {{0, 0}, inserted_certificate, std::nullopt, inserted_certificate, {}};

        // Search through all the list for a link
        for_each([&](X509Node& top) {
            if (top.certificate.is_child(inserted_certificate)) {
                // Some sanity checks
                if (top.state.is_selfsigned) {
                    throw InvalidStateException(
                        "Newly added certificate can't be parent of a self-signed certificate!");
                }

                if (top.hash.has_value()) {
                    throw InvalidStateException("Existing non-root top certificate can't have a valid hash!");
                }

                // If the top certificate is a descendant of the certificate we're adding

                // Cache top node
                auto temp_top = std::move(top);

                // Set the new state of the top node
                temp_top.state = {0, 0};
                temp_top.hash = temp_top.certificate.get_certificate_hash_data(new_node.certificate);
                temp_top.issuer = X509Wrapper(new_node.certificate);

                // Set the top as a child of the new_node
                new_node.children.push_back(std::move(temp_top));

                // Set the new top
                top = std::move(new_node);
                hierarchy_found = true; // Found a link
            } else if (inserted_certificate.is_child(top.certificate)) {
                // If the certificate is the descendant of top certificate

                // Calculate hash and set issuer
                new_node.state = {0, 0};
                new_node.hash = inserted_certificate.get_certificate_hash_data(top.certificate);
                new_node.issuer = X509Wrapper(top.certificate); // Set the new issuer

                // Add it to the top's descendant list
                top.children.push_back(new_node);
                hierarchy_found = true; // Found a link
            }

            // Keep iterating while we did not find a link
            return (false == hierarchy_found);
        });

        // Else insert it in the roots as a potentially orphan certificate
        if (hierarchy_found == false) {
            hierarchy.push_back(new_node);
        }
    } else {
        // If it is self-signed insert it in the roots, with the state set as a self-signed and a properly computed hash
        hierarchy.push_back(
            {{1, 0}, inserted_certificate, inserted_certificate.get_certificate_hash_data(), inserted_certificate, {}});

        // Attempt a partial prune, by searching through all the contained temporary orphan certificates
        // and trying to add them to the newly inserted root certificate, if that is possible

        // Only iterate until last (not including) since the last is the new node
        for (int i = 0; i < (hierarchy.size() - 1); ++i) {
            auto& node = hierarchy[i];
            auto& state = node.state;

            // If we have a temporary orphan, that is if we are in the roots
            // and we are not self-signed then it means that we are an orphan
            if (state.is_selfsigned == 0) {
                // Some sanity checks
                if (node.hash.has_value())
                    throw InvalidStateException("Orphan certificate can't have a proper hash!");

                // If it is a child of the new root certificate insert it to it's list and break
                if (node.certificate.is_child(inserted_certificate)) {
                    auto& new_root = hierarchy.back();

                    // Hash is properly computed now
                    node.hash = node.certificate.get_certificate_hash_data(new_root.certificate);
                    node.state.is_orphan = 0;                        // Not an orphan any more
                    node.issuer = X509Wrapper(inserted_certificate); // Set the new valid issuer

                    // Add to the newly inserted root child list
                    new_root.children.push_back(std::move(node));

                    // Erase element since it was added to the root's descendants
                    hierarchy.erase(hierarchy.begin() + i);
                    // Decrement i again since it was erased
                    i--;
                }
            }
        }
    }
} // End insert

void X509CertificateHierarchy::prune() {
    if (hierarchy.size() <= 1)
        return;

    for (int i = 0; i < hierarchy.size(); ++i) {
        // Possible orphan
        auto& orphan = hierarchy[i];

        bool is_orphan = (orphan.state.is_selfsigned) == 0 && (orphan.state.is_orphan == 0);
        if (is_orphan == false)
            continue;

        // Found a non-permanent orphan, search for a issuer
        bool found_issuer = false;

        for_each([&](X509Node& top) {
            if (orphan.certificate.is_child(top.certificate)) {
                orphan.hash = orphan.certificate.get_certificate_hash_data(top.certificate);
                orphan.state.is_orphan = 0;                   // Not an orphan any more
                orphan.issuer = X509Wrapper(top.certificate); // Set the new valid issuer

                top.children.push_back(std::move(orphan));
                found_issuer = true;
            }

            return (false == found_issuer);
        });

        if (false == found_issuer) {
            // Mark as permanent orphan
            orphan.state.is_orphan = 1; // Permanent orphan
        } else {
            // Erase from hierarchy list and decrement iterator
            hierarchy.erase(std::begin(hierarchy) + i);
            i--;
        }
    }
}

X509CertificateHierarchy X509CertificateHierarchy::build_hierarchy(std::vector<X509Wrapper>& certificates) {
    X509CertificateHierarchy ordered;

    while (certificates.size()) {
        ordered.insert(std::move(certificates.back()));
        certificates.pop_back();
    }

    // Prune the tree
    ordered.prune();

    return ordered;
}

} // namespace evse_security
