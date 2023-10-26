// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2023 Pionix GmbH and Contributors to EVerest

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <regex>
#include <sstream>

#include "evse_security.hpp"

#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <x509_bundle.hpp>
#include <x509_wrapper.hpp>

std::string read_file_to_string(const std::filesystem::path filepath) {
    std::ifstream t(filepath.string());
    std::stringstream buffer;
    buffer << t.rdbuf();
    return buffer.str();
}

bool equal_certificate_strings(const std::string& cert1, const std::string& cert2) {
    for (int i = 0; i < cert1.length(); ++i) {
        if (i < cert1.length() && i < cert2.length()) {
            if (isalnum(cert1[i]) && isalnum(cert2[i]) && cert1[i] != cert2[i])
                return false;
        }
    }

    return true;
}

void install_certs() {
    std::system("./generate_test_certs.sh");
}

namespace evse_security {

class EvseSecurityTests : public ::testing::Test {
protected:
    std::unique_ptr<EvseSecurity> evse_security;

    void SetUp() override {
        install_certs();

        FilePaths file_paths;
        file_paths.csms_ca_bundle = std::filesystem::path("certs/ca/v2g/V2G_CA_BUNDLE.pem");
        file_paths.mf_ca_bundle = std::filesystem::path("certs/ca/v2g/V2G_CA_BUNDLE.pem");
        file_paths.mo_ca_bundle = std::filesystem::path("certs/ca/mo/MO_CA_BUNDLE.pem");
        file_paths.v2g_ca_bundle = std::filesystem::path("certs/ca/v2g/V2G_CA_BUNDLE.pem");
        file_paths.directories.csms_leaf_cert_directory = std::filesystem::path("certs/client/csms/");
        file_paths.directories.csms_leaf_key_directory = std::filesystem::path("certs/client/csms/");
        file_paths.directories.secc_leaf_cert_directory = std::filesystem::path("certs/client/cso/");
        file_paths.directories.secc_leaf_key_directory = std::filesystem::path("certs/client/cso/");

        this->evse_security = std::make_unique<EvseSecurity>(file_paths, "123456");
    }

    void TearDown() override {
        std::filesystem::remove_all("certs");
    }
};

TEST_F(EvseSecurityTests, verify_basics) {
    const char* bundle_path = "certs/ca/v2g/V2G_CA_BUNDLE.pem";

    std::ifstream file(bundle_path, std::ios::binary);
    std::string certificate_file((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    std::vector<std::string> certificate_strings;

    static const std::regex cert_regex("-----BEGIN CERTIFICATE-----[\\s\\S]*?-----END CERTIFICATE-----");
    std::string::const_iterator search_start(certificate_file.begin());

    std::smatch match;
    while (std::regex_search(search_start, certificate_file.cend(), match, cert_regex)) {
        std::string cert_data = match.str();
        try {
            certificate_strings.emplace_back(cert_data);
        } catch (const CertificateLoadException& e) {
            std::cout << "Could not load single certificate while splitting CA bundle: " << e.what() << std::endl;
        }
        search_start = match.suffix().first;
    }

    ASSERT_TRUE(certificate_strings.size() == 3);

    X509CertificateBundle bundle(std::filesystem::path(bundle_path), EncodingFormat::PEM);
    ASSERT_TRUE(bundle.is_using_bundle_file());

    auto certificates = bundle.split();
    ASSERT_TRUE(certificates.size() == 3);

    for (int i = 0; i < certificate_strings.size() - 1; ++i) {
        X509Wrapper cert(certificate_strings[i], EncodingFormat::PEM);
        X509Wrapper parent(certificate_strings[i + 1],EncodingFormat::PEM);

        ASSERT_TRUE(certificates[i].get_certificate_hash_data(parent) == cert.get_certificate_hash_data(parent));
        ASSERT_TRUE(equal_certificate_strings(cert.get_export_string(), certificate_strings[i]));
    }

    auto root_cert_idx = certificate_strings.size() - 1;
    X509Wrapper root_cert(certificate_strings[root_cert_idx], EncodingFormat::PEM);
    ASSERT_TRUE(certificates[root_cert_idx].get_certificate_hash_data() == root_cert.get_certificate_hash_data());
    ASSERT_TRUE(equal_certificate_strings(root_cert.get_export_string(), certificate_strings[root_cert_idx]));
}

TEST_F(EvseSecurityTests, verify_bundle_management) {
    const char* directory_path = "certs/ca/csms/";
    X509CertificateBundle bundle(std::filesystem::path(directory_path), EncodingFormat::PEM);
    ASSERT_TRUE(bundle.split().size() == 2);

    bundle.delete_certificate(bundle.split()[0].get_certificate_hash_data(bundle.split()[1]));
    bundle.sync_to_certificate_store();

    int items = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory_path)) {
        if (X509CertificateBundle::is_certificate_file(entry)) {
            items++;
        }
    }
    ASSERT_TRUE(items == 1);
}

/// \brief test verifyChargepointCertificate with valid cert
TEST_F(EvseSecurityTests, verify_chargepoint_cert_01) {
    const auto client_certificate = read_file_to_string(std::filesystem::path("certs/client/csms/CSMS_LEAF.pem"));
    std::cout << client_certificate << std::endl;
    const auto result = this->evse_security->update_leaf_certificate(client_certificate, LeafCertificateType::CSMS);
    ASSERT_TRUE(result == InstallCertificateResult::Accepted);
}

/// \brief test verifyChargepointCertificate with invalid cert
TEST_F(EvseSecurityTests, verify_chargepoint_cert_02) {
    const auto result = this->evse_security->update_leaf_certificate("InvalidCertificate", LeafCertificateType::CSMS);
    ASSERT_TRUE(result == InstallCertificateResult::InvalidFormat);
}

/// \brief test verifyV2GChargingStationCertificate with valid cert
TEST_F(EvseSecurityTests, verify_v2g_cert_01) {
    const auto client_certificate = read_file_to_string(std::filesystem::path("certs/client/cso/SECC_LEAF.pem"));
    const auto result = this->evse_security->update_leaf_certificate(client_certificate, LeafCertificateType::V2G);
    ASSERT_TRUE(result == InstallCertificateResult::Accepted);
}

/// \brief test verifyV2GChargingStationCertificate with invalid cert
TEST_F(EvseSecurityTests, verify_v2g_cert_02) {
    const auto invalid_certificate =
        read_file_to_string(std::filesystem::path("certs/client/invalid/INVALID_CSMS.pem"));
    const auto result = this->evse_security->update_leaf_certificate(invalid_certificate, LeafCertificateType::V2G);
    ASSERT_TRUE(result == InstallCertificateResult::InvalidCertificateChain);
}

TEST_F(EvseSecurityTests, install_root_ca_01) {
    const auto v2g_root_ca = read_file_to_string(std::filesystem::path("certs/ca/v2g/V2G_ROOT_CA_NEW.pem"));
    const auto result = this->evse_security->install_ca_certificate(v2g_root_ca, CaCertificateType::V2G);
    ASSERT_TRUE(result == InstallCertificateResult::Accepted);
}

TEST_F(EvseSecurityTests, install_root_ca_02) {
    const auto invalid_csms_ca = "InvalidCertificate";
    const auto result = this->evse_security->install_ca_certificate(invalid_csms_ca, CaCertificateType::CSMS);
    ASSERT_EQ(result, InstallCertificateResult::InvalidFormat);
}

/// \brief test install two new root certificates
TEST_F(EvseSecurityTests, install_root_ca_03) {
    const auto pre_installed_certificates = this->evse_security->get_installed_certificates({ CertificateType::CSMSRootCertificate });

    const auto new_root_ca_1 = read_file_to_string(std::filesystem::path("certs/to_be_installed/INSTALL_TEST_ROOT_CA1.pem"));
    const auto result = this->evse_security->install_ca_certificate(new_root_ca_1, CaCertificateType::CSMS);
    ASSERT_TRUE(result == InstallCertificateResult::Accepted);

    const auto new_root_ca_2 = read_file_to_string(std::filesystem::path("certs/to_be_installed/INSTALL_TEST_ROOT_CA2.pem"));
    const auto result2 = this->evse_security->install_ca_certificate(new_root_ca_2, CaCertificateType::CSMS);
    ASSERT_TRUE(result2 == InstallCertificateResult::Accepted);

    const auto post_installed_certificates = this->evse_security->get_installed_certificates({ CertificateType::CSMSRootCertificate });
    ASSERT_EQ(post_installed_certificates.certificate_hash_data_chain.size(), pre_installed_certificates.certificate_hash_data_chain.size() + 2);

    // todo: validate installed certificates
}

/// \brief test install new root certificates + two child certificates
TEST_F(EvseSecurityTests, install_root_ca_04) {
    const auto pre_installed_certificates = this->evse_security->get_installed_certificates({ CertificateType::CSMSRootCertificate });

    const auto new_root_ca_1 = read_file_to_string(std::filesystem::path("certs/to_be_installed/INSTALL_TEST_ROOT_CA3.pem"));
    const auto result = this->evse_security->install_ca_certificate(new_root_ca_1, CaCertificateType::CSMS);
    ASSERT_TRUE(result == InstallCertificateResult::Accepted);

    const auto new_root_sub_ca_1 = read_file_to_string(std::filesystem::path("certs/to_be_installed/INSTALL_TEST_ROOT_CA3_SUBCA1.pem"));
    const auto result2 = this->evse_security->install_ca_certificate(new_root_sub_ca_1, CaCertificateType::CSMS);
    ASSERT_TRUE(result2 == InstallCertificateResult::Accepted);

    const auto new_root_sub_ca_2 = read_file_to_string(std::filesystem::path("certs/to_be_installed/INSTALL_TEST_ROOT_CA3_SUBCA2.pem"));
    const auto result3 = this->evse_security->install_ca_certificate(new_root_sub_ca_2, CaCertificateType::CSMS);
    ASSERT_TRUE(result3 == InstallCertificateResult::Accepted);

    const auto post_installed_certificates = this->evse_security->get_installed_certificates({ CertificateType::CSMSRootCertificate });
    ASSERT_EQ(post_installed_certificates.certificate_hash_data_chain.size(), pre_installed_certificates.certificate_hash_data_chain.size() + 1);

    // todo: clarify order of newly installed, to be corrected once assertions before pass!
    ASSERT_EQ(post_installed_certificates.certificate_hash_data_chain[0].child_certificate_hash_data.size(), 2);
}

/// \brief test install expired certificate must be rejected
TEST_F(EvseSecurityTests, install_root_ca_05) {
    const auto expired_cert =
        std::string("-----BEGIN CERTIFICATE-----\n")
        + "MIICsjCCAZqgAwIBAgICMDkwDQYJKoZIhvcNAQELBQAwHDEaMBgGA1UEAwwRT0NU\n"
        + "VEV4cGlyZWRSb290Q0EwHhcNMjAwMTAxMDAwMDAwWhcNMjEwMTAxMDAwMDAwWjAc\n"
        + "MRowGAYDVQQDDBFPQ1RURXhwaXJlZFJvb3RDQTCCASIwDQYJKoZIhvcNAQEBBQAD\n"
        + "ggEPADCCAQoCggEBALA3xfKUgMaFfRHabFy27PhWvaeVDL6yd4qv4w4pe0NMJ0pE\n"
        + "gr9ynzvXleVlOHF09rabgH99bW/ohLx3l7OliOjMk82e/77oGf0O8ZxViFrppA+z\n"
        + "6WVhvRn7opso8KkrTCNUYyuzTH9u/n3EU9uFfueu+ifzD2qke7YJqTz7GY7aEqSb\n"
        + "x7+3GDKhZV8lOw68T+WKkJxfuuafzczewHhu623ztc0bo5fTr3FSqWkuJXhB4Zg/\n"
        + "GBMt1hS+O4IZeho8Ik9uu5zW39HQQNcJKN6dYDTIZdtQ8vNp6hYdOaRd05v77Ye0\n"
        + "ywqqYVyUTgdfmqE5u7YeWUfO9vab3Qxq1IeHVd8CAwEAATANBgkqhkiG9w0BAQsF\n"
        + "AAOCAQEAfDeemUzKXtqfCfuaGwTKTsj+Ld3A6VRiT/CSx1rh6BNAZZrve8OV2ckr\n"
        + "2Ia+fol9mEkZPCBNLDzgxs5LLiJIOy4prjSTX4HJS5iqJBO8UJGakqXOAz0qBG1V\n"
        + "8xWCJLeLGni9vi+dLVVFWpSfzTA/4iomtJPuvoXLdYzMvjLcGFT9RsE9q0oEbGHq\n"
        + "ezKIzFaOdpCOtAt+FgW1lqqGHef2wNz15iWQLAU1juip+lgowI5YdhVJVPyqJTNz\n"
        + "RUletvBeY2rFUKFWhj8QRPBwBlEDZqxRJSyIwQCe9t7Nhvbd9eyCFvRm9z3a8FDf\n"
        + "FRmmZMWQkhBDQt15vxoDyyWn3hdwRA==\n"
        + "-----END CERTIFICATE-----";

    const auto result = this->evse_security->install_ca_certificate(expired_cert, CaCertificateType::CSMS);
    ASSERT_EQ(result, InstallCertificateResult::Expired);
}


TEST_F(EvseSecurityTests, delete_root_ca_01) {

    std::vector<CertificateType> certificate_types;
    certificate_types.push_back(CertificateType::V2GRootCertificate);
    certificate_types.push_back(CertificateType::MORootCertificate);
    certificate_types.push_back(CertificateType::CSMSRootCertificate);
    certificate_types.push_back(CertificateType::V2GCertificateChain);
    certificate_types.push_back(CertificateType::MFRootCertificate);

    const auto root_certs = this->evse_security->get_installed_certificates(certificate_types);

    CertificateHashData certificate_hash_data;
    certificate_hash_data.hash_algorithm = HashAlgorithm::SHA256;
    certificate_hash_data.issuer_key_hash =
        root_certs.certificate_hash_data_chain.at(0).certificate_hash_data.issuer_key_hash;
    certificate_hash_data.issuer_name_hash =
        root_certs.certificate_hash_data_chain.at(0).certificate_hash_data.issuer_name_hash;
    certificate_hash_data.serial_number =
        root_certs.certificate_hash_data_chain.at(0).certificate_hash_data.serial_number;
    const auto result = this->evse_security->delete_certificate(certificate_hash_data);

    ASSERT_EQ(result, DeleteCertificateResult::Accepted);
}

TEST_F(EvseSecurityTests, delete_root_ca_02) {
    CertificateHashData certificate_hash_data;
    certificate_hash_data.hash_algorithm = HashAlgorithm::SHA256;
    certificate_hash_data.issuer_key_hash = "UnknownKeyHash";
    certificate_hash_data.issuer_name_hash = "7da88c3366c19488ee810c5408f612db98164a34e05a0b15c93914fbed228c0f";
    certificate_hash_data.serial_number = "3046";
    const auto result = this->evse_security->delete_certificate(certificate_hash_data);

    ASSERT_EQ(result, DeleteCertificateResult::NotFound);
}

TEST_F(EvseSecurityTests, get_installed_certificates_and_delete_secc_leaf) {
    std::vector<CertificateType> certificate_types;
    certificate_types.push_back(CertificateType::V2GRootCertificate);
    certificate_types.push_back(CertificateType::MORootCertificate);
    certificate_types.push_back(CertificateType::CSMSRootCertificate);
    certificate_types.push_back(CertificateType::V2GCertificateChain);
    certificate_types.push_back(CertificateType::MFRootCertificate);

    const auto r = this->evse_security->get_installed_certificates(certificate_types);

    ASSERT_EQ(r.status, GetInstalledCertificatesStatus::Accepted);
    ASSERT_EQ(r.certificate_hash_data_chain.size(), 4);
    bool found_v2g_chain = false;

    CertificateHashData secc_leaf_data;

    for (const auto& certificate_hash_data_chain : r.certificate_hash_data_chain) {
        if (certificate_hash_data_chain.certificate_type == CertificateType::V2GCertificateChain) {
            found_v2g_chain = true;
            secc_leaf_data = certificate_hash_data_chain.certificate_hash_data;
            ASSERT_EQ(2, certificate_hash_data_chain.child_certificate_hash_data.size());
        }
    }
    ASSERT_TRUE(found_v2g_chain);

    auto delete_response = this->evse_security->delete_certificate(secc_leaf_data);
    ASSERT_EQ(delete_response, DeleteCertificateResult::Accepted);

    const auto get_certs_response = this->evse_security->get_installed_certificates(certificate_types);
    // ASSERT_EQ(r.status, GetInstalledCertificatesStatus::Accepted);
    ASSERT_EQ(get_certs_response.certificate_hash_data_chain.size(), 3);

    delete_response = this->evse_security->delete_certificate(secc_leaf_data);
    ASSERT_EQ(delete_response, DeleteCertificateResult::NotFound);
}

} // namespace evse_security

// FIXME(piet): Add more tests for getRootCertificateHashData (incl. V2GCertificateChain etc.)
