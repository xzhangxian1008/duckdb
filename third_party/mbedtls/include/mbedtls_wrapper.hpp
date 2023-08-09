//===----------------------------------------------------------------------===//
//                         DuckDB
//
// mbedtls_wrapper.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/optional_ptr.hpp"

#include <string>

namespace duckdb_mbedtls {

// class MbedTlsAesContext {
// public:
//	static MbedTlsAesContext CreateEncryptionContext(const std::string &key);
//	static MbedTlsAesContext CreateDecryptionContext(const std::string &key);
//
//	~MbedTlsAesContext();
//
// public:
//	void *context_ptr;
// };

class MbedTlsWrapper {
public:
	static void ComputeSha256Hash(const char *in, size_t in_len, char *out);
	static std::string ComputeSha256Hash(const std::string &file_content);
	static bool IsValidSha256Signature(const std::string &pubkey, const std::string &signature,
	                                   const std::string &sha256_hash);
	static void Hmac256(const char *key, size_t key_len, const char *message, size_t message_len, char *out);

	static constexpr size_t SHA256_HASH_BYTES = 32;

	// public:
	//	static void Encrypt(MbedTlsAesContext &context, unsigned char iv[16], unsigned char *in, size_t in_len);
	//	static void Decrypt(MbedTlsAesContext &context, unsigned char iv[16], unsigned char *in, size_t in_len);
};

class MbedTlsGcmContext {
public:
	// trying to follow https://gist.github.com/unprovable/892a677d672990f46bca97194ae549bc
	MbedTlsGcmContext();
	MbedTlsGcmContext(const std::string &key);
	void Initialize(const std::string &key);
	~MbedTlsGcmContext();

public:
	void InitializeDecryption(duckdb::const_data_ptr_t iv, duckdb::idx_t iv_len);
	size_t Process(duckdb::const_data_ptr_t in, duckdb::idx_t in_len, duckdb::data_ptr_t out, duckdb::idx_t out_len);

private:
	duckdb::optional_ptr<char> context_ptr;
};

} // namespace duckdb_mbedtls
