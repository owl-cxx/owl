#pragma once

// A certificate made when the test runs, so no key material lives in the
// repository and none can expire there. It is self-signed for localhost
// and 127.0.0.1, which lets a test client trust it as its own root and
// verify the name as a real client would. Beside the matching key it
// writes two that must be refused: one that belongs to no certificate,
// and the right one behind a passphrase.

#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

#include <stdlib.h>
#include <unistd.h>

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <owl/core/config.h>

namespace owl_test {
    struct TestCert final {
        std::filesystem::path dir;
        std::string cert;
        std::string key;
        std::string unrelated_key;
        std::string locked_key;

        TestCert() {
            std::string pattern = (std::filesystem::temp_directory_path() / "owl-tls-XXXXXX").string();
            if (::mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("TestCert: mkdtemp");
            dir = pattern;
            cert = (dir / "cert.pem").string();
            key = (dir / "key.pem").string();
            unrelated_key = (dir / "unrelated.pem").string();
            locked_key = (dir / "locked.pem").string();

            const pkey_ptr pkey{EVP_EC_gen("P-256")};
            const pkey_ptr other{EVP_EC_gen("P-256")};
            if (!pkey || !other) throw std::runtime_error("TestCert: keygen");

            const std::unique_ptr<X509, decltype(&X509_free)> x509{X509_new(), &X509_free};
            X509_set_version(x509.get(), 2);
            ASN1_INTEGER_set(X509_get_serialNumber(x509.get()), 1);
            X509_gmtime_adj(X509_getm_notBefore(x509.get()), -60);
            X509_gmtime_adj(X509_getm_notAfter(x509.get()), 3600);
            X509_set_pubkey(x509.get(), pkey.get());
            X509_NAME* const name = X509_get_subject_name(x509.get());
            X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
            X509_set_issuer_name(x509.get(), name);
            X509V3_CTX v3{};
            X509V3_set_ctx_nodb(&v3);
            X509V3_set_ctx(&v3, x509.get(), x509.get(), nullptr, nullptr, 0);
            X509_EXTENSION* const san = X509V3_EXT_conf_nid(nullptr, &v3, NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1");
            if (san == nullptr) throw std::runtime_error("TestCert: subjectAltName");
            X509_add_ext(x509.get(), san, -1);
            X509_EXTENSION_free(san);
            if (X509_sign(x509.get(), pkey.get(), EVP_sha256()) == 0) throw std::runtime_error("TestCert: sign");

            const file_ptr cert_file = open(cert);
            if (PEM_write_X509(cert_file.get(), x509.get()) != 1) throw std::runtime_error("TestCert: write certificate");
            write_key(key, pkey.get(), nullptr);
            write_key(unrelated_key, other.get(), nullptr);
            write_key(locked_key, pkey.get(), "secret");
        }

        ~TestCert() {
            std::error_code ignored;
            std::filesystem::remove_all(dir, ignored);
        }

        TestCert(const TestCert&) = delete;
        TestCert& operator=(const TestCert&) = delete;

        [[nodiscard]] owl::Tls tls() const {
            return {.cert = cert, .key = key};
        }

    private:
        struct pkey_free final {
            void operator()(EVP_PKEY* const p) const noexcept {
                EVP_PKEY_free(p);
            }
        };
        using pkey_ptr = std::unique_ptr<EVP_PKEY, pkey_free>;

        struct file_close final {
            void operator()(std::FILE* const f) const noexcept {
                std::fclose(f);
            }
        };
        using file_ptr = std::unique_ptr<std::FILE, file_close>;

        [[nodiscard]] static file_ptr open(const std::string& path) {
            file_ptr file{std::fopen(path.c_str(), "w")};
            if (!file) throw std::runtime_error("TestCert: cannot write " + path);
            return file;
        }

        static void write_key(const std::string& path, EVP_PKEY* const pkey, const char* const passphrase) {
            const file_ptr file = open(path);
            const std::string secret = passphrase != nullptr ? passphrase : "";
            const int ok = passphrase != nullptr
                               ? PEM_write_PrivateKey(file.get(), pkey, EVP_aes_256_cbc(),
                                                      reinterpret_cast<const unsigned char*>(secret.data()),
                                                      static_cast<int>(secret.size()), nullptr, nullptr)
                               : PEM_write_PrivateKey(file.get(), pkey, nullptr, nullptr, 0, nullptr, nullptr);
            if (ok != 1) throw std::runtime_error("TestCert: write key " + path);
        }
    };
}
