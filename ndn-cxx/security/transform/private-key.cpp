/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2013-2022 Regents of the University of California.
 *
 * This file is part of ndn-cxx library (NDN C++ library with eXperimental eXtensions).
 *
 * ndn-cxx library is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later version.
 *
 * ndn-cxx library is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.
 *
 * You should have received copies of the GNU General Public License and GNU Lesser
 * General Public License along with ndn-cxx, e.g., in COPYING.md file.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * See AUTHORS.md for complete list of ndn-cxx authors and contributors.
 */

#include "ndn-cxx/security/transform/private-key.hpp"
#include "ndn-cxx/security/transform/base64-decode.hpp"
#include "ndn-cxx/security/transform/base64-encode.hpp"
#include "ndn-cxx/security/transform/buffer-source.hpp"
#include "ndn-cxx/security/transform/digest-filter.hpp"
#include "ndn-cxx/security/transform/stream-sink.hpp"
#include "ndn-cxx/security/transform/stream-source.hpp"
#include "ndn-cxx/security/impl/openssl-helper.hpp"
#include "ndn-cxx/security/key-params.hpp"
#include "ndn-cxx/encoding/buffer-stream.hpp"
#include "ndn-cxx/util/random.hpp"
#include "ndn-cxx/util/scope.hpp"

#include <boost/lexical_cast.hpp>
#include <cstring>
//added_GM, by liupenghui
#if 1
#include "ndn-cxx/util/string-helper.hpp"
#include <iostream>
#endif

#define ENSURE_PRIVATE_KEY_LOADED(key) \
  do { \
    if ((key) == nullptr) \
      NDN_THROW(Error("Private key has not been loaded yet")); \
  } while (false)

#define ENSURE_PRIVATE_KEY_NOT_LOADED(key) \
  do { \
    if ((key) != nullptr) \
      NDN_THROW(Error("Private key has already been loaded")); \
  } while (false)

namespace ndn {
namespace security {
namespace transform {

static void
opensslInitAlgorithms()
{
#if OPENSSL_VERSION_NUMBER < 0x1010000fL
  static bool isInitialized = false;
  if (!isInitialized) {
    OpenSSL_add_all_algorithms();
    isInitialized = true;
  }
#endif // OPENSSL_VERSION_NUMBER < 0x1010000fL
}

class PrivateKey::Impl : noncopyable
{
public:
  ~Impl()
  {
    EVP_PKEY_free(key);
  }

public:
  EVP_PKEY* key = nullptr;

#if OPENSSL_VERSION_NUMBER < 0x1010100fL
  size_t keySize = 0; // in bits, used only for HMAC
#endif
};

PrivateKey::PrivateKey()
  : m_impl(make_unique<Impl>())
{
}

PrivateKey::~PrivateKey() = default;

KeyType
PrivateKey::getKeyType() const
{
  if (!m_impl->key)
    return KeyType::NONE;

  switch (detail::getEvpPkeyType(m_impl->key)) {
  case EVP_PKEY_RSA:
    return KeyType::RSA;
  case EVP_PKEY_EC:
    return KeyType::EC;
  case EVP_PKEY_HMAC:
    return KeyType::HMAC;
//added_GM, by liupenghui
#if 1
  case EVP_PKEY_SM2:
	return KeyType::SM2;
#endif
  default:
    return KeyType::NONE;
  }
}

size_t
PrivateKey::getKeySize() const
{
  switch (getKeyType()) {
    case KeyType::RSA:
    case KeyType::EC:
      return static_cast<size_t>(EVP_PKEY_bits(m_impl->key));
//added_GM, by liupenghui
#if 1
	case KeyType::SM2:
#endif	
    case KeyType::HMAC: {
#if OPENSSL_VERSION_NUMBER >= 0x1010100fL
      size_t nBytes = 0;
      EVP_PKEY_get_raw_private_key(m_impl->key, nullptr, &nBytes);
      return nBytes * 8;
#else
      return m_impl->keySize;
#endif
    }
    default:
      return 0;
  }
}

ConstBufferPtr
PrivateKey::getKeyDigest(DigestAlgorithm algo) const
{
  if (getKeyType() != KeyType::HMAC)
    NDN_THROW(Error("Digest is not supported for key type " +
                    boost::lexical_cast<std::string>(getKeyType())));

  const uint8_t* buf = nullptr;
  size_t len = 0;
#if OPENSSL_VERSION_NUMBER >= 0x1010000fL
  buf = EVP_PKEY_get0_hmac(m_impl->key, &len);
#else
  const auto* octstr = reinterpret_cast<ASN1_OCTET_STRING*>(EVP_PKEY_get0(m_impl->key));
  buf = octstr->data;
  len = octstr->length;
#endif
  if (buf == nullptr)
    NDN_THROW(Error("Failed to obtain raw key pointer"));
  if (len * 8 != getKeySize())
    NDN_THROW(Error("Key length mismatch"));

  OBufferStream os;
  bufferSource(make_span(buf, len)) >> digestFilter(algo) >> streamSink(os);
  return os.buf();
}

void
PrivateKey::loadRaw(KeyType type, span<const uint8_t> buf)
{
  ENSURE_PRIVATE_KEY_NOT_LOADED(m_impl->key);

  int pkeyType;
  switch (type) {
  case KeyType::HMAC:
    pkeyType = EVP_PKEY_HMAC;
    break;
  default:
    NDN_THROW(std::invalid_argument("Unsupported key type " + boost::lexical_cast<std::string>(type)));
  }

  m_impl->key =
#if OPENSSL_VERSION_NUMBER >= 0x1010100fL
      EVP_PKEY_new_raw_private_key(pkeyType, nullptr, buf.data(), buf.size());
#else
      EVP_PKEY_new_mac_key(pkeyType, nullptr, buf.data(), static_cast<int>(buf.size()));
#endif
  if (m_impl->key == nullptr)
    NDN_THROW(Error("Failed to load private key"));

#if OPENSSL_VERSION_NUMBER < 0x1010100fL
  m_impl->keySize = buf.size() * 8;
#endif
}

//added_HMAC, by liupenghui
#if 1 
void
PrivateKey::loadHamcPkcs1Base64(std::istream& is)
{
  OBufferStream os;
  streamSource(is) >> base64Decode() >> streamSink(os);
  loadRaw(KeyType::HMAC, *os.buf());
}
#endif

void
PrivateKey::loadPkcs1(span<const uint8_t> buf)
{
  ENSURE_PRIVATE_KEY_NOT_LOADED(m_impl->key);
  opensslInitAlgorithms();

  auto ptr = buf.data();
  if (d2i_AutoPrivateKey(&m_impl->key, &ptr, static_cast<long>(buf.size())) == nullptr)
    NDN_THROW(Error("Failed to load private key"));
}

void
PrivateKey::loadPkcs1(std::istream& is)
{
  OBufferStream os;
  streamSource(is) >> streamSink(os);
  loadPkcs1(*os.buf());
}

void
PrivateKey::loadPkcs1Base64(span<const uint8_t> buf)
{
  OBufferStream os;
  bufferSource(buf) >> base64Decode() >> streamSink(os);
  loadPkcs1(*os.buf());
}

void
PrivateKey::loadPkcs1Base64(std::istream& is)
{
  OBufferStream os;
  streamSource(is) >> base64Decode() >> streamSink(os);
  loadPkcs1(*os.buf());
}

void
PrivateKey::loadPkcs8(span<const uint8_t> buf, const char* pw, size_t pwLen)
{
  BOOST_ASSERT(std::strlen(pw) == pwLen);
  ENSURE_PRIVATE_KEY_NOT_LOADED(m_impl->key);
  opensslInitAlgorithms();

  detail::Bio membio(BIO_s_mem());
  if (!membio.write(buf))
    NDN_THROW(Error("Failed to copy buffer"));

  if (d2i_PKCS8PrivateKey_bio(membio, &m_impl->key, nullptr, const_cast<char*>(pw)) == nullptr)
    NDN_THROW(Error("Failed to load private key"));
}

static inline int
passwordCallbackWrapper(char* buf, int size, int rwflag, void* u)
{
  BOOST_ASSERT(size >= 0);
  auto cb = reinterpret_cast<PrivateKey::PasswordCallback*>(u);
  return (*cb)(buf, static_cast<size_t>(size), rwflag);
}

void
PrivateKey::loadPkcs8(span<const uint8_t> buf, PasswordCallback pwCallback)
{
  ENSURE_PRIVATE_KEY_NOT_LOADED(m_impl->key);
  opensslInitAlgorithms();

  detail::Bio membio(BIO_s_mem());
  if (!membio.write(buf))
    NDN_THROW(Error("Failed to copy buffer"));

  if (pwCallback)
    m_impl->key = d2i_PKCS8PrivateKey_bio(membio, nullptr, &passwordCallbackWrapper, &pwCallback);
  else
    m_impl->key = d2i_PKCS8PrivateKey_bio(membio, nullptr, nullptr, nullptr);

  if (m_impl->key == nullptr)
    NDN_THROW(Error("Failed to load private key"));
}

void
PrivateKey::loadPkcs8(std::istream& is, const char* pw, size_t pwLen)
{
  OBufferStream os;
  streamSource(is) >> streamSink(os);
  loadPkcs8(*os.buf(), pw, pwLen);
}

void
PrivateKey::loadPkcs8(std::istream& is, PasswordCallback pwCallback)
{
  OBufferStream os;
  streamSource(is) >> streamSink(os);
  loadPkcs8(*os.buf(), std::move(pwCallback));
}

void
PrivateKey::loadPkcs8Base64(span<const uint8_t> buf, const char* pw, size_t pwLen)
{
  OBufferStream os;
  bufferSource(buf) >> base64Decode() >> streamSink(os);
  loadPkcs8(*os.buf(), pw, pwLen);
}

void
PrivateKey::loadPkcs8Base64(span<const uint8_t> buf, PasswordCallback pwCallback)
{
  OBufferStream os;
  bufferSource(buf) >> base64Decode() >> streamSink(os);
  loadPkcs8(*os.buf(), std::move(pwCallback));
}

void
PrivateKey::loadPkcs8Base64(std::istream& is, const char* pw, size_t pwLen)
{
  OBufferStream os;
  streamSource(is) >> base64Decode() >> streamSink(os);
  loadPkcs8(*os.buf(), pw, pwLen);
}

void
PrivateKey::loadPkcs8Base64(std::istream& is, PasswordCallback pwCallback)
{
  OBufferStream os;
  streamSource(is) >> base64Decode() >> streamSink(os);
  loadPkcs8(*os.buf(), std::move(pwCallback));
}

void
PrivateKey::savePkcs1(std::ostream& os) const
{
  bufferSource(*toPkcs1()) >> streamSink(os);
}

void
PrivateKey::savePkcs1Base64(std::ostream& os) const
{
  bufferSource(*toPkcs1()) >> base64Encode() >> streamSink(os);
}

void
PrivateKey::savePkcs8(std::ostream& os, const char* pw, size_t pwLen) const
{
  bufferSource(*toPkcs8(pw, pwLen)) >> streamSink(os);
}

void
PrivateKey::savePkcs8(std::ostream& os, PasswordCallback pwCallback) const
{
  bufferSource(*toPkcs8(std::move(pwCallback))) >> streamSink(os);
}

void
PrivateKey::savePkcs8Base64(std::ostream& os, const char* pw, size_t pwLen) const
{
  bufferSource(*toPkcs8(pw, pwLen)) >> base64Encode() >> streamSink(os);
}

void
PrivateKey::savePkcs8Base64(std::ostream& os, PasswordCallback pwCallback) const
{
  bufferSource(*toPkcs8(std::move(pwCallback))) >> base64Encode() >> streamSink(os);
}

ConstBufferPtr
PrivateKey::derivePublicKey() const
{
  ENSURE_PRIVATE_KEY_LOADED(m_impl->key);

  uint8_t* pkcs8 = nullptr;
  int len = i2d_PUBKEY(m_impl->key, &pkcs8);
  if (len < 0)
    NDN_THROW(Error("Failed to derive public key"));

  auto result = make_shared<Buffer>(pkcs8, len);
  OPENSSL_free(pkcs8);

  return result;
}


//added_GM, by liupenghui
//if the PrivateKey is imported from outside, detail::getEvpPkeyType(m_impl->key)) can't differ SM2 from ECDSA, thus, we add a parameter keyType.
#if 1
ConstBufferPtr
PrivateKey::decrypt(span<const uint8_t> cipherText, KeyType keyType) const
{
  ENSURE_PRIVATE_KEY_LOADED(m_impl->key);

 // int keyType = detail::getEvpPkeyType(m_impl->key);
  switch (keyType) {
    case KeyType::NONE:
      NDN_THROW(Error("Failed to determine key type"));
    case KeyType::RSA:
      return rsaDecrypt(cipherText);
    case KeyType::SM2:
  	  return sm2Decrypt(cipherText);
    default:
      NDN_THROW(Error("Decryption is not supported for key type " + to_string((int)keyType)));
  }
}

#else
ConstBufferPtr
PrivateKey::decrypt(span<const uint8_t> cipherText) const
{
  ENSURE_PRIVATE_KEY_LOADED(m_impl->key);

  int keyType = detail::getEvpPkeyType(m_impl->key);
  switch (keyType) {
    case EVP_PKEY_NONE:
      NDN_THROW(Error("Failed to determine key type"));
    case EVP_PKEY_RSA:
      return rsaDecrypt(cipherText);
    default:
      NDN_THROW(Error("Decryption is not supported for key type " + to_string(keyType)));
  }
}
#endif

void*
PrivateKey::getEvpPkey() const
{
  return m_impl->key;
}

ConstBufferPtr
PrivateKey::toPkcs1() const
{
  ENSURE_PRIVATE_KEY_LOADED(m_impl->key);
  opensslInitAlgorithms();
  
//added_HMAC, by liupenghui
#if 1 
  
#if OPENSSL_VERSION_NUMBER >= 0x1010100fL
  if (getKeyType() == KeyType::HMAC) {
    size_t outlen = 0;
    if (1 != EVP_PKEY_get_raw_private_key(m_impl->key, nullptr, &outlen))
  	  NDN_THROW(Error("Cannot EVP_PKEY_get_raw_private_key"));
    auto tmpbuffer = make_shared<Buffer>(outlen);
    if (1 != EVP_PKEY_get_raw_private_key(m_impl->key, tmpbuffer->data(), &outlen))
  	  NDN_THROW(Error("Cannot EVP_PKEY_get_raw_private_key"));
    return tmpbuffer;
  }
#endif
  
#endif

  detail::Bio membio(BIO_s_mem());
  if (!i2d_PrivateKey_bio(membio, m_impl->key))
    NDN_THROW(Error("Cannot convert key to PKCS #1 format"));

  auto buffer = make_shared<Buffer>(BIO_pending(membio));
  if (!membio.read(*buffer))
    NDN_THROW(Error("Read error during PKCS #1 conversion"));

  return buffer;
}

ConstBufferPtr
PrivateKey::toPkcs8(const char* pw, size_t pwLen) const
{
  BOOST_ASSERT(std::strlen(pw) == pwLen);
  ENSURE_PRIVATE_KEY_LOADED(m_impl->key);
  opensslInitAlgorithms();

  detail::Bio membio(BIO_s_mem());
  if (!i2d_PKCS8PrivateKey_bio(membio, m_impl->key, EVP_aes_256_cbc(), nullptr, 0,
                               nullptr, const_cast<char*>(pw)))
    NDN_THROW(Error("Cannot convert key to PKCS #8 format"));

  auto buffer = make_shared<Buffer>(BIO_pending(membio));
  if (!membio.read(*buffer))
    NDN_THROW(Error("Read error during PKCS #8 conversion"));

  return buffer;
}

ConstBufferPtr
PrivateKey::toPkcs8(PasswordCallback pwCallback) const
{
  ENSURE_PRIVATE_KEY_LOADED(m_impl->key);
  opensslInitAlgorithms();

  detail::Bio membio(BIO_s_mem());
  if (!i2d_PKCS8PrivateKey_bio(membio, m_impl->key, EVP_aes_256_cbc(), nullptr, 0,
                               &passwordCallbackWrapper, &pwCallback))
    NDN_THROW(Error("Cannot convert key to PKCS #8 format"));

  auto buffer = make_shared<Buffer>(BIO_pending(membio));
  if (!membio.read(*buffer))
    NDN_THROW(Error("Read error during PKCS #8 conversion"));

  return buffer;
}

ConstBufferPtr
PrivateKey::rsaDecrypt(span<const uint8_t> cipherText) const
{
  detail::EvpPkeyCtx ctx(m_impl->key);

  if (EVP_PKEY_decrypt_init(ctx) <= 0)
    NDN_THROW(Error("Failed to initialize decryption context"));

  if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0)
    NDN_THROW(Error("Failed to set padding"));

  size_t outlen = 0;
  // Determine buffer length
  if (EVP_PKEY_decrypt(ctx, nullptr, &outlen, cipherText.data(), cipherText.size()) <= 0)
    NDN_THROW(Error("Failed to estimate output length"));

  auto out = make_shared<Buffer>(outlen);
  if (EVP_PKEY_decrypt(ctx, out->data(), &outlen, cipherText.data(), cipherText.size()) <= 0)
    NDN_THROW(Error("Failed to decrypt ciphertext"));

  out->resize(outlen);
  return out;
}

//added_GM, by liupenghui
#if 1
ConstBufferPtr
PrivateKey::sm2Decrypt(span<const uint8_t> cipherText) const
{
  size_t outlen=0;

  if ((EVP_PKEY_set_alias_type(m_impl->key, EVP_PKEY_SM2)) != 1) {
	  NDN_THROW(Error("Failed to EVP_PKEY_set_alias_type"));
  }
  
  detail::EvpPkeyCtx ectx(m_impl->key);

  if ((EVP_PKEY_decrypt_init(ectx)) != 1) {
	  NDN_THROW(Error("Failed to initialize decryption context"));
  }
 
  if ((EVP_PKEY_decrypt(ectx, nullptr, &outlen, cipherText.data(), cipherText.size())) != 1) {
	  NDN_THROW(Error("Failed to estimate buffer length"));
  }
 
  auto out = make_shared<Buffer>(outlen);
 
  if ((EVP_PKEY_decrypt(ectx, out->data(), &outlen, cipherText.data(), cipherText.size())) != 1) {
	  NDN_THROW(Error("Failed to decrypt ciphertext"));
  }
 
  out->resize(outlen);

  return out;
}

#endif


unique_ptr<PrivateKey>
PrivateKey::generateRsaKey(uint32_t keySize)
{
  detail::EvpPkeyCtx kctx(EVP_PKEY_RSA);

  if (EVP_PKEY_keygen_init(kctx) <= 0)
    NDN_THROW(PrivateKey::Error("Failed to initialize RSA keygen context"));

  if (EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, static_cast<int>(keySize)) <= 0)
    NDN_THROW(PrivateKey::Error("Failed to set RSA key length"));

  auto privateKey = make_unique<PrivateKey>();
  if (EVP_PKEY_keygen(kctx, &privateKey->m_impl->key) <= 0)
    NDN_THROW(PrivateKey::Error("Failed to generate RSA key"));

  return privateKey;
}

unique_ptr<PrivateKey>
PrivateKey::generateEcKey(uint32_t keySize)
{
  EC_KEY* eckey = nullptr;
  switch (keySize) {
  case 224:
    eckey = EC_KEY_new_by_curve_name(NID_secp224r1);
    break;
  case 256:
    eckey = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1); // same as secp256r1
    break;
  case 384:
    eckey = EC_KEY_new_by_curve_name(NID_secp384r1);
    break;
  case 521:
    eckey = EC_KEY_new_by_curve_name(NID_secp521r1);
    break;
  default:
    NDN_THROW(std::invalid_argument("Unsupported EC key length " + to_string(keySize)));
  }
  if (eckey == nullptr) {
    NDN_THROW(Error("Failed to set EC curve"));
  }

  auto guard = make_scope_exit([eckey] { EC_KEY_free(eckey); });

#if OPENSSL_VERSION_NUMBER < 0x1010000fL
  EC_KEY_set_asn1_flag(eckey, OPENSSL_EC_NAMED_CURVE);
#endif // OPENSSL_VERSION_NUMBER < 0x1010000fL

  if (EC_KEY_generate_key(eckey) != 1) {
    NDN_THROW(Error("Failed to generate EC key"));
  }

  auto privateKey = make_unique<PrivateKey>();
  privateKey->m_impl->key = EVP_PKEY_new();
  if (privateKey->m_impl->key == nullptr)
    NDN_THROW(Error("Failed to create EVP_PKEY"));
  if (EVP_PKEY_set1_EC_KEY(privateKey->m_impl->key, eckey) != 1)
    NDN_THROW(Error("Failed to assign EC key"));

  return privateKey;
}

//added_GM, by liupenghui
#if 1
unique_ptr<PrivateKey>
PrivateKey::generateSM2Key(uint32_t keySize)
{
  EVP_PKEY_CTX* pctx = nullptr;
  EVP_PKEY_CTX *kctx = nullptr;
  EVP_PKEY *params = nullptr;
  EC_KEY *key_pair = nullptr;
  const BIGNUM *priv_key = nullptr;
  char *priv_key_str = nullptr;
  const EC_GROUP *group = nullptr;
  const EC_POINT *pub_key = nullptr;
  BN_CTX *ctx = nullptr;
  BIGNUM *x_coordinate = nullptr, *y_coordinate = nullptr;
  char *x_coordinate_str = nullptr, *y_coordinate_str = nullptr;

	/* create SM2 Ellipse Curve parameters and key pair */
  if (keySize != 256) {
    NDN_THROW(std::invalid_argument("Unsupported EC key length " + to_string(keySize)));
  }
   if (!(pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr))) {
    NDN_THROW(Error("Failed to set EVP_PKEY_CTX_new_id"));
  }
 
  if ((EVP_PKEY_paramgen_init(pctx)) != 1) {
    EVP_PKEY_CTX_free(pctx);
    NDN_THROW(Error("Failed to set param init"));
  }
 
  if ((EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_sm2)) <= 0 ) {
	EVP_PKEY_CTX_free(pctx);
    NDN_THROW(Error("Failed to set EC curve nid"));
  }
  
  if ((EVP_PKEY_paramgen(pctx, &params)) != 1) {
    EVP_PKEY_CTX_free(pctx);
    NDN_THROW(Error("Failed to setEVP_PKEY_paramgen"));
  }
  
  kctx = EVP_PKEY_CTX_new(params, nullptr);
  if (!kctx) {
    EVP_PKEY_CTX_free(pctx);
    NDN_THROW(Error("Failed to setEVP_PKEY_paramgen"));
  }
  
  if ((EVP_PKEY_keygen_init(kctx)) != 1) {
    EVP_PKEY_CTX_free(pctx);
    EVP_PKEY_CTX_free(kctx);
    NDN_THROW(Error("Failed to EVP_PKEY_keygen_init"));
  }
 
  auto privateKey = make_unique<PrivateKey>();
  if ((EVP_PKEY_keygen(kctx, &(privateKey->m_impl->key))) != 1) {
	EVP_PKEY_CTX_free(pctx);
    EVP_PKEY_CTX_free(kctx);
    NDN_THROW(Error("Failed to EVP_PKEY_keygen"));
  }
	
  if (privateKey->m_impl->key == nullptr) {
	EVP_PKEY_CTX_free(pctx);
    EVP_PKEY_CTX_free(kctx);
    NDN_THROW(Error("Failed to create EVP_PKEY"));
  }
  
  /*########## start to print SM2 key pair, for debug ######### */
  /* print SM2 key pair */
  if (!(key_pair = EVP_PKEY_get0_EC_KEY(privateKey->m_impl->key))) {
	  EVP_PKEY_CTX_free(pctx);
	  EVP_PKEY_CTX_free(kctx);
	  EVP_PKEY_free(privateKey->m_impl->key);
	  NDN_THROW(Error("Failed to EVP_PKEY_get0_EC_KEY"));
  }
  
  if (!(priv_key = EC_KEY_get0_private_key(key_pair))) {
	  EVP_PKEY_CTX_free(pctx);
	  EVP_PKEY_CTX_free(kctx);
	  EVP_PKEY_free(privateKey->m_impl->key);
	  NDN_THROW(Error("Failed to EC_KEY_get0_private_key"));
  }
  
  if (!(priv_key_str = BN_bn2hex(priv_key))) {
	  EVP_PKEY_CTX_free(pctx);
	  EVP_PKEY_CTX_free(kctx);
	  EVP_PKEY_free(privateKey->m_impl->key);
	  NDN_THROW(Error("Failed to BN_bn2hex"));
  }
  //for debug, don't delete it
  #if 0 
  std::cout << "SM2 private key (in hex form):"<<std::endl;
  std::cout << priv_key_str <<std::endl;
  #endif
  
  OPENSSL_free(priv_key_str);
  
  if (!(pub_key = EC_KEY_get0_public_key(key_pair))) {
	  EVP_PKEY_CTX_free(pctx);
	  EVP_PKEY_CTX_free(kctx);
	  EVP_PKEY_free(privateKey->m_impl->key);
	  NDN_THROW(Error("Failed to EC_KEY_get0_public_key"));
  }
  
  if (!(group = EC_KEY_get0_group(key_pair))) {
	  EVP_PKEY_CTX_free(pctx);
	  EVP_PKEY_CTX_free(kctx);
	  EVP_PKEY_free(privateKey->m_impl->key);
	  NDN_THROW(Error("Failed to EC_KEY_get0_group"));
  }
  
  if (!(ctx = BN_CTX_new())) {
	  EVP_PKEY_CTX_free(pctx);
	  EVP_PKEY_CTX_free(kctx);
	  EVP_PKEY_free(privateKey->m_impl->key);
	  NDN_THROW(Error("Failed to BN_CTX_new"));
  }
  
  BN_CTX_start(ctx);
  x_coordinate = BN_CTX_get(ctx);
  y_coordinate = BN_CTX_get(ctx);
  if (!(y_coordinate)) {
	  EVP_PKEY_CTX_free(pctx);
	  EVP_PKEY_CTX_free(kctx);
	  EVP_PKEY_free(privateKey->m_impl->key);
	  BN_CTX_end(ctx);
	  BN_CTX_free(ctx);
	  NDN_THROW(Error("Failed to get y_coordinate"));
  }
  
  if (!(EC_POINT_get_affine_coordinates(group, pub_key, x_coordinate, y_coordinate, ctx))) {
	  EVP_PKEY_CTX_free(pctx);
	  EVP_PKEY_CTX_free(kctx);
	  EVP_PKEY_free(privateKey->m_impl->key);
	  BN_CTX_end(ctx);
	  BN_CTX_free(ctx);
	  NDN_THROW(Error("Failed to EC_POINT_get_affine_coordinates"));
  }
  
  if (!(x_coordinate_str = BN_bn2hex(x_coordinate))) {
	  EVP_PKEY_CTX_free(pctx);
	  EVP_PKEY_CTX_free(kctx);
	  EVP_PKEY_free(privateKey->m_impl->key);
	  BN_CTX_end(ctx);
	  BN_CTX_free(ctx);
	  NDN_THROW(Error("Failed to BN_bn2hex"));
  }
  //for debug, don't delete it
  #if 0 
  std::cout << "x coordinate in SM2 public key (in hex form):"<<std::endl;
  std::cout << x_coordinate_str<<std::endl;
  #endif

  if (!(y_coordinate_str = BN_bn2hex(y_coordinate))) {
	  EVP_PKEY_CTX_free(pctx);
	  EVP_PKEY_CTX_free(kctx);
	  EVP_PKEY_free(privateKey->m_impl->key);
	  BN_CTX_end(ctx);
	  BN_CTX_free(ctx);
	  OPENSSL_free(x_coordinate_str);
	  NDN_THROW(Error("Failed to BN_bn2hex"));
  }
  
  //for debug, don't delete it
  #if 0 
  std::cout << "y coordinate in SM2 public key (in hex form):"<<std::endl;
  std::cout <<  y_coordinate_str<< "\n" <<std::endl;
  #endif
  
  /*########## end to print SM2 key pair######### */
  OPENSSL_free(x_coordinate_str);
  OPENSSL_free(y_coordinate_str);
  BN_CTX_end(ctx);
  BN_CTX_free(ctx);
  
  if ((EVP_PKEY_set_alias_type(privateKey->m_impl->key, EVP_PKEY_SM2)) != 1) {
	  EVP_PKEY_CTX_free(pctx);
	  EVP_PKEY_CTX_free(kctx);
	  EVP_PKEY_free(privateKey->m_impl->key);
	  NDN_THROW(Error("Failed to EVP_PKEY_set_alias_type"));
  }
  
  EVP_PKEY_CTX_free(pctx);
  EVP_PKEY_CTX_free(kctx);

  return privateKey;
}

#endif


unique_ptr<PrivateKey>
PrivateKey::generateHmacKey(uint32_t keySize)
{
  std::vector<uint8_t> rawKey(keySize / 8);
  random::generateSecureBytes(rawKey);

  auto privateKey = make_unique<PrivateKey>();
  try {
    privateKey->loadRaw(KeyType::HMAC, rawKey);
  }
  catch (const PrivateKey::Error&) {
    NDN_THROW(PrivateKey::Error("Failed to generate HMAC key"));
  }

  return privateKey;
}

unique_ptr<PrivateKey>
generatePrivateKey(const KeyParams& keyParams)
{
  switch (keyParams.getKeyType()) {
    case KeyType::RSA: {
      const auto& rsaParams = static_cast<const RsaKeyParams&>(keyParams);
      return PrivateKey::generateRsaKey(rsaParams.getKeySize());
    }
    case KeyType::EC: {
      const auto& ecParams = static_cast<const EcKeyParams&>(keyParams);
      return PrivateKey::generateEcKey(ecParams.getKeySize());
    }
    case KeyType::HMAC: {
      const auto& hmacParams = static_cast<const HmacKeyParams&>(keyParams);
      return PrivateKey::generateHmacKey(hmacParams.getKeySize());
    }
//added_GM, by liupenghui
#if 1
	case KeyType::SM2: {
	  const auto& sm2Params = static_cast<const sm2KeyParams&>(keyParams);
	  return PrivateKey::generateSM2Key(sm2Params.getKeySize());
	}
#endif
    default:
      NDN_THROW(std::invalid_argument("Unsupported key type " +
                                      boost::lexical_cast<std::string>(keyParams.getKeyType())));
  }
}

} // namespace transform
} // namespace security
} // namespace ndn
