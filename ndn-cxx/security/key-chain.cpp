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

#include "ndn-cxx/security/key-chain.hpp"

#include "ndn-cxx/encoding/buffer-stream.hpp"
#include "ndn-cxx/util/config-file.hpp"
#include "ndn-cxx/util/logger.hpp"

#include "ndn-cxx/security/pib/impl/pib-memory.hpp"
#include "ndn-cxx/security/pib/impl/pib-sqlite3.hpp"

#include "ndn-cxx/security/tpm/impl/back-end-file.hpp"
#include "ndn-cxx/security/tpm/impl/back-end-mem.hpp"
#ifdef NDN_CXX_HAVE_OSX_FRAMEWORKS
#include "ndn-cxx/security/tpm/impl/back-end-osx.hpp"
#endif // NDN_CXX_HAVE_OSX_FRAMEWORKS

#include "ndn-cxx/security/transform/bool-sink.hpp"
#include "ndn-cxx/security/transform/buffer-source.hpp"
#include "ndn-cxx/security/transform/digest-filter.hpp"
#include "ndn-cxx/security/transform/private-key.hpp"
#include "ndn-cxx/security/transform/public-key.hpp"
#include "ndn-cxx/security/transform/stream-sink.hpp"
#include "ndn-cxx/security/transform/verifier-filter.hpp"

#include <boost/lexical_cast.hpp>

namespace ndn {
namespace security {

// When static library is used, not everything is compiled into the resulting binary.
// Therefore, the following standard PIB and TPMs need to be registered here.
// http://stackoverflow.com/q/9459980/2150331

namespace pib {
NDN_CXX_KEYCHAIN_REGISTER_PIB_BACKEND(PibSqlite3);
NDN_CXX_KEYCHAIN_REGISTER_PIB_BACKEND(PibMemory);
} // namespace pib

namespace tpm {
#if defined(NDN_CXX_HAVE_OSX_FRAMEWORKS) && defined(NDN_CXX_WITH_OSX_KEYCHAIN)
NDN_CXX_KEYCHAIN_REGISTER_TPM_BACKEND(BackEndOsx);
#endif // defined(NDN_CXX_HAVE_OSX_FRAMEWORKS) && defined(NDN_CXX_WITH_OSX_KEYCHAIN)
NDN_CXX_KEYCHAIN_REGISTER_TPM_BACKEND(BackEndFile);
NDN_CXX_KEYCHAIN_REGISTER_TPM_BACKEND(BackEndMem);
} // namespace tpm

inline namespace v2 {

NDN_LOG_INIT(ndn.security.KeyChain);

std::string KeyChain::s_defaultPibLocator;
std::string KeyChain::s_defaultTpmLocator;

KeyChain::PibFactories&
KeyChain::getPibFactories()
{
  static PibFactories pibFactories;
  return pibFactories;
}

KeyChain::TpmFactories&
KeyChain::getTpmFactories()
{
  static TpmFactories tpmFactories;
  return tpmFactories;
}

const std::string&
KeyChain::getDefaultPibScheme()
{
  return pib::PibSqlite3::getScheme();
}

const std::string&
KeyChain::getDefaultTpmScheme()
{
#if defined(NDN_CXX_HAVE_OSX_FRAMEWORKS) && defined(NDN_CXX_WITH_OSX_KEYCHAIN)
  return tpm::BackEndOsx::getScheme();
#else
  return tpm::BackEndFile::getScheme();
#endif // defined(NDN_CXX_HAVE_OSX_FRAMEWORKS) && defined(NDN_CXX_WITH_OSX_KEYCHAIN)
}

const std::string&
KeyChain::getDefaultPibLocator()
{
  if (!s_defaultPibLocator.empty())
    return s_defaultPibLocator;

  if (getenv("NDN_CLIENT_PIB") != nullptr) {
    s_defaultPibLocator = getenv("NDN_CLIENT_PIB");
  }
  else {
    ConfigFile config;
    s_defaultPibLocator = config.getParsedConfiguration().get<std::string>("pib", getDefaultPibScheme() + ":");
  }

  std::string pibScheme, pibLocation;
  std::tie(pibScheme, pibLocation) = parseAndCheckPibLocator(s_defaultPibLocator);
  s_defaultPibLocator = pibScheme + ":" + pibLocation;

  return s_defaultPibLocator;
}

const std::string&
KeyChain::getDefaultTpmLocator()
{
  if (!s_defaultTpmLocator.empty())
    return s_defaultTpmLocator;

  if (getenv("NDN_CLIENT_TPM") != nullptr) {
    s_defaultTpmLocator = getenv("NDN_CLIENT_TPM");
  }
  else {
    ConfigFile config;
    s_defaultTpmLocator = config.getParsedConfiguration().get<std::string>("tpm", getDefaultTpmScheme() + ":");
  }

  std::string tpmScheme, tpmLocation;
  std::tie(tpmScheme, tpmLocation) = parseAndCheckTpmLocator(s_defaultTpmLocator);
  s_defaultTpmLocator = tpmScheme + ":" + tpmLocation;

  return s_defaultTpmLocator;
}

const KeyParams&
KeyChain::getDefaultKeyParams()
{
  static EcKeyParams keyParams;
  return keyParams;
}

//

KeyChain::KeyChain()
  : KeyChain(getDefaultPibLocator(), getDefaultTpmLocator(), true)
{
}

KeyChain::KeyChain(const std::string& pibLocator, const std::string& tpmLocator, bool allowReset)
{
  // PIB Locator
  std::string pibScheme, pibLocation;
  std::tie(pibScheme, pibLocation) = parseAndCheckPibLocator(pibLocator);
  std::string canonicalPibLocator = pibScheme + ":" + pibLocation;

  // Create PIB
  m_pib = createPib(canonicalPibLocator);
  std::string oldTpmLocator;
  try {
    oldTpmLocator = m_pib->getTpmLocator();
  }
  catch (const Pib::Error&) {
    // TPM locator is not set in PIB yet.
  }

  // TPM Locator
  std::string tpmScheme, tpmLocation;
  std::tie(tpmScheme, tpmLocation) = parseAndCheckTpmLocator(tpmLocator);
  std::string canonicalTpmLocator = tpmScheme + ":" + tpmLocation;

  if (canonicalPibLocator == getDefaultPibLocator()) {
    // Default PIB must use default TPM
    if (!oldTpmLocator.empty() && oldTpmLocator != getDefaultTpmLocator()) {
      m_pib->reset();
      canonicalTpmLocator = getDefaultTpmLocator();
    }
  }
  else {
    // non-default PIB check consistency
    if (!oldTpmLocator.empty() && oldTpmLocator != canonicalTpmLocator) {
      if (allowReset)
        m_pib->reset();
      else
        NDN_THROW(LocatorMismatchError("TPM locator supplied does not match TPM locator in PIB: " +
                                       oldTpmLocator + " != " + canonicalTpmLocator));
    }
  }

  // note that key mismatch may still happen if the TPM locator is initially set to a
  // wrong one or if the PIB was shared by more than one TPMs before.  This is due to the
  // old PIB does not have TPM info, new pib should not have this problem.
  m_tpm = createTpm(canonicalTpmLocator);
  m_pib->setTpmLocator(canonicalTpmLocator);
}

KeyChain::~KeyChain() = default;

// public: management

Identity
KeyChain::createIdentity(const Name& identityName, const KeyParams& params)
{
  Identity id = m_pib->addIdentity(identityName);

  Key key;
  try {
    key = id.getDefaultKey();
  }
  catch (const Pib::Error&) {
    key = createKey(id, params);
  }

  try {
    key.getDefaultCertificate();
  }
  catch (const Pib::Error&) {
    NDN_LOG_DEBUG("No default cert for " << key.getName() << ", requesting self-signing");
    selfSign(key);
  }

  return id;
}

void
KeyChain::deleteIdentity(const Identity& identity)
{
  BOOST_ASSERT(static_cast<bool>(identity));

  Name identityName = identity.getName();

  for (const auto& key : identity.getKeys()) {
    m_tpm->deleteKey(key.getName());
  }

  m_pib->removeIdentity(identityName);
}

void
KeyChain::setDefaultIdentity(const Identity& identity)
{
  BOOST_ASSERT(static_cast<bool>(identity));

  m_pib->setDefaultIdentity(identity.getName());
}

Key
KeyChain::createKey(const Identity& identity, const KeyParams& params)
{
  BOOST_ASSERT(static_cast<bool>(identity));

  // create key in TPM
  Name keyName = m_tpm->createKey(identity.getName(), params);

  // set up key info in PIB
//added_GM, by liupenghui
#if 1  
  Key key = identity.addKey(*m_tpm->getPublicKey(keyName), keyName, params.getKeyType());
#else
  Key key = identity.addKey(*m_tpm->getPublicKey(keyName), keyName);
#endif

  NDN_LOG_DEBUG("Requesting self-signing for newly created key " << key.getName());
  selfSign(key);

  return key;
}

Name
KeyChain::createHmacKey(const Name& prefix, const HmacKeyParams& params)
{
  return m_tpm->createKey(prefix, params);
}

void
KeyChain::deleteKey(const Identity& identity, const Key& key)
{
  BOOST_ASSERT(static_cast<bool>(identity));
  BOOST_ASSERT(static_cast<bool>(key));

  Name keyName = key.getName();
  if (identity.getName() != key.getIdentity()) {
    NDN_THROW(std::invalid_argument("Identity `" + identity.getName().toUri() + "` "
                                    "does not match key `" + keyName.toUri() + "`"));
  }

  identity.removeKey(keyName);
  m_tpm->deleteKey(keyName);
}

void
KeyChain::setDefaultKey(const Identity& identity, const Key& key)
{
  BOOST_ASSERT(static_cast<bool>(identity));
  BOOST_ASSERT(static_cast<bool>(key));

  if (identity.getName() != key.getIdentity())
    NDN_THROW(std::invalid_argument("Identity `" + identity.getName().toUri() + "` "
                                    "does not match key `" + key.getName().toUri() + "`"));

  identity.setDefaultKey(key.getName());
}

void
KeyChain::addCertificate(const Key& key, const Certificate& certificate)
{
  BOOST_ASSERT(static_cast<bool>(key));

  const auto& certContent = certificate.getContent();
  if (certContent.value_size() == 0) {
    NDN_THROW(std::invalid_argument("Certificate `" + certificate.getName().toUri() + "` is empty"));
  }

  if (key.getName() != certificate.getKeyName() ||
      !std::equal(certContent.value_begin(), certContent.value_end(), key.getPublicKey().begin())) {
    NDN_THROW(std::invalid_argument("Key `" + key.getName().toUri() + "` "
                                    "does not match certificate `" + certificate.getName().toUri() + "`"));
  }

  key.addCertificate(certificate);
}

void
KeyChain::deleteCertificate(const Key& key, const Name& certificateName)
{
  BOOST_ASSERT(static_cast<bool>(key));

  if (!Certificate::isValidName(certificateName)) {
    NDN_THROW(std::invalid_argument("Wrong certificate name `" + certificateName.toUri() + "`"));
  }

  key.removeCertificate(certificateName);
}

void
KeyChain::setDefaultCertificate(const Key& key, const Certificate& cert)
{
  BOOST_ASSERT(static_cast<bool>(key));

  addCertificate(key, cert);
  key.setDefaultCertificate(cert.getName());
}

shared_ptr<SafeBag>
KeyChain::exportSafeBag(const Certificate& certificate, const char* pw, size_t pwLen)
{
  Name identity = certificate.getIdentity();
  Name keyName = certificate.getKeyName();

  ConstBufferPtr encryptedKey;
  try {
    encryptedKey = m_tpm->exportPrivateKey(keyName, pw, pwLen);
  }
  catch (const Tpm::Error&) {
    NDN_THROW_NESTED(Error("Failed to export private key `" + keyName.toUri() + "`"));
  }

  return make_shared<SafeBag>(certificate, *encryptedKey);
}

void
KeyChain::importSafeBag(const SafeBag& safeBag, const char* pw, size_t pwLen)
{
  Data certData = safeBag.getCertificate();
  Certificate cert(std::move(certData));
  Name identity = cert.getIdentity();
  Name keyName = cert.getKeyName();
  const auto publicKeyBits = cert.getPublicKey();

//added_GM, by liupenghui
// PublicKey.getKeyType() can't differ SM2 from ECDSA.
#if 1
  KeyType keyType = KeyType::NONE;
  KeyType keyTypefromSig = KeyType::NONE;
  
  int32_t Signature_type = cert.getSignatureType();
  if (Signature_type == tlv::SignatureSha256WithRsa)
    keyTypefromSig =	KeyType::RSA;
  else if (Signature_type == tlv::SignatureSha256WithEcdsa)
    keyTypefromSig =	KeyType::EC;
  else if (Signature_type == tlv::SignatureHmacWithSha256)
    keyTypefromSig =	KeyType::HMAC;
  else if (Signature_type == tlv::SignatureSm3WithSm2)
    keyTypefromSig =	KeyType::SM2;
  else
    keyTypefromSig = KeyType::NONE;
  
  transform::PrivateKey pkey;
  pkey.loadPkcs8(safeBag.getEncryptedKey(), pw, pwLen);
  keyType = pkey.getKeyType();
  // After loading Pkcs8 key from outside file, pkey.getKeyType() can't differ SM2 from ECDSA,
  // Thus, we use the key type from signature, in general only SM2 public key certificate will use SignatureSm3WithSm2.
  if (keyTypefromSig != keyType) {
    keyType = keyTypefromSig;
  }
#endif

  if (m_tpm->hasKey(keyName)) {
    NDN_THROW(Error("Private key `" + keyName.toUri() + "` already exists"));
  }

  try {
    Identity existingId = m_pib->getIdentity(identity);
    existingId.getKey(keyName);
    NDN_THROW(Error("Public key `" + keyName.toUri() + "` already exists"));
  }
  catch (const Pib::Error&) {
    // Either identity or key doesn't exist. OK to import.
  }

  try {
    m_tpm->importPrivateKey(keyName, safeBag.getEncryptedKey(), pw, pwLen);
  }
  catch (const Tpm::Error&) {
    NDN_THROW_NESTED(Error("Failed to import private key `" + keyName.toUri() + "`"));
  }

  // check the consistency of private key and certificate
  const uint8_t content[] = {0x01, 0x02, 0x03, 0x04};
  ConstBufferPtr sigBits;
  try {
    //added_GM, by liupenghui
    #if 1
    if (keyType == KeyType::SM2)
      sigBits = m_tpm->sign({content}, keyName, keyType, DigestAlgorithm::SM3);
    else
      sigBits = m_tpm->sign({content}, keyName, keyType, DigestAlgorithm::SHA256);
    #else 
    sigBits = m_tpm->sign({content}, keyName, DigestAlgorithm::SHA256);
    #endif
  }
  catch (const std::runtime_error&) {
    m_tpm->deleteKey(keyName);
    NDN_THROW(Error("Invalid private key `" + keyName.toUri() + "`"));
  }
  bool isVerified = false;
  {
    using namespace transform;
    PublicKey publicKey;
    publicKey.loadPkcs8(publicKeyBits);

//added_GM, by liupenghui
#if 1
	if (keyType == KeyType::SM2) {
	  bufferSource(content) >> verifierFilter(DigestAlgorithm::SM3, publicKey, keyType, *sigBits)
					  >> boolSink(isVerified);
	}
	else {
		bufferSource(content) >> verifierFilter(DigestAlgorithm::SHA256, publicKey, keyType, *sigBits)
						>> boolSink(isVerified);
	}
#else	
    bufferSource(content) >> verifierFilter(DigestAlgorithm::SHA256, publicKey, *sigBits)
                          >> boolSink(isVerified);
#endif	

  }
  if (!isVerified) {
    m_tpm->deleteKey(keyName);
    NDN_THROW(Error("Certificate `" + cert.getName().toUri() + "` "
                    "and private key `" + keyName.toUri() + "` do not match"));
  }

  Identity id = m_pib->addIdentity(identity);
//added_GM, by liupenghui
#if 1  
  Key key = id.addKey(cert.getPublicKey(), keyName, keyType);
#else
  Key key = id.addKey(cert.getPublicKey(), keyName);
#endif
  key.addCertificate(cert);
}

void
KeyChain::importPrivateKey(const Name& keyName, shared_ptr<transform::PrivateKey> key)
{
  if (m_tpm->hasKey(keyName)) {
    NDN_THROW(Error("Private key `" + keyName.toUri() + "` already exists"));
  }

  try {
    m_tpm->importPrivateKey(keyName, std::move(key));
  }
  catch (const Tpm::Error&) {
    NDN_THROW_NESTED(Error("Failed to import private key `" + keyName.toUri() + "`"));
  }
}

// public: signing

void
KeyChain::sign(Data& data, const SigningInfo& params)
{
  Name keyName;
  SignatureInfo sigInfo;
  std::tie(keyName, sigInfo) = prepareSignatureInfo(params);

//added_GM, by liupenghui
#if 1
  KeyType keyTypefromSig = KeyType::NONE;
  int32_t Signature_type = sigInfo.getSignatureType();
  if (Signature_type == tlv::SignatureSha256WithRsa)
    keyTypefromSig =	KeyType::RSA;
  else if (Signature_type == tlv::SignatureSha256WithEcdsa)
    keyTypefromSig =	KeyType::EC;
  else if (Signature_type == tlv::SignatureHmacWithSha256)
    keyTypefromSig =	KeyType::HMAC;
  else if (Signature_type == tlv::SignatureSm3WithSm2)
    keyTypefromSig =	KeyType::SM2;
  else
    keyTypefromSig = KeyType::NONE;
#endif

  data.setSignatureInfo(sigInfo);

  EncodingBuffer encoder;
  data.wireEncode(encoder, true);

//added_GM, by liupenghui
#if 1	 
  auto sigValue = sign({encoder}, keyName, keyTypefromSig, params.getDigestAlgorithm());
  
#else
  auto sigValue = sign({encoder}, keyName, params.getDigestAlgorithm());
#endif

  data.wireEncode(encoder, *sigValue);
}

void
KeyChain::sign(Interest& interest, const SigningInfo& params)
{
  Name keyName;
  SignatureInfo sigInfo;
  std::tie(keyName, sigInfo) = prepareSignatureInfo(params);
  
//added_GM, by liupenghui
#if 1
  KeyType keyTypefromSig = KeyType::NONE;
  int32_t Signature_type = sigInfo.getSignatureType();
  if (Signature_type == tlv::SignatureSha256WithRsa)
	keyTypefromSig =  KeyType::RSA;
  else if (Signature_type == tlv::SignatureSha256WithEcdsa)
	keyTypefromSig =  KeyType::EC;
  else if (Signature_type == tlv::SignatureHmacWithSha256)
	keyTypefromSig =  KeyType::HMAC;
  else if (Signature_type == tlv::SignatureSm3WithSm2)
	keyTypefromSig =  KeyType::SM2;
  else
	keyTypefromSig = KeyType::NONE;
#endif

  if (params.getSignedInterestFormat() == SignedInterestFormat::V03) {
    interest.setSignatureInfo(sigInfo);

    // Extract function will throw if not all necessary elements are present in Interest
    //added_GM, by liupenghui
#if 1	 
    auto sigValue = sign(interest.extractSignedRanges(), keyName, keyTypefromSig, params.getDigestAlgorithm());
#else
    auto sigValue = sign(interest.extractSignedRanges(), keyName, params.getDigestAlgorithm());
#endif
    interest.setSignatureValue(std::move(sigValue));
  }
  else {
    Name signedName = interest.getName();

    // We encode in Data format because this is the format used prior to Packet Specification v0.3
    const auto& sigInfoBlock = sigInfo.wireEncode(SignatureInfo::Type::Data);
    signedName.append(sigInfoBlock.begin(), sigInfoBlock.end()); // SignatureInfo
	//added_GM, by liupenghui
#if 1	 
    Block sigValue(tlv::SignatureValue,
			   sign({{signedName.wireEncode().value(), signedName.wireEncode().value_size()}},
					keyName, keyTypefromSig, params.getDigestAlgorithm()));

#else
    Block sigValue(tlv::SignatureValue,
                   sign({{signedName.wireEncode().value(), signedName.wireEncode().value_size()}},
                        keyName, params.getDigestAlgorithm()));
#endif

    sigValue.encode();
    signedName.append(sigValue.begin(), sigValue.end()); // SignatureValue

    interest.setName(signedName);
  }
}

// public: PIB/TPM creation helpers

static inline std::tuple<std::string/*type*/, std::string/*location*/>
parseLocatorUri(const std::string& uri)
{
  size_t pos = uri.find(':');
  if (pos != std::string::npos) {
    return std::make_tuple(uri.substr(0, pos), uri.substr(pos + 1));
  }
  else {
    return std::make_tuple(uri, "");
  }
}

std::tuple<std::string/*type*/, std::string/*location*/>
KeyChain::parseAndCheckPibLocator(const std::string& pibLocator)
{
  std::string pibScheme, pibLocation;
  std::tie(pibScheme, pibLocation) = parseLocatorUri(pibLocator);

  if (pibScheme.empty()) {
    pibScheme = getDefaultPibScheme();
  }

  auto pibFactory = getPibFactories().find(pibScheme);
  if (pibFactory == getPibFactories().end()) {
    NDN_THROW(Error("PIB scheme `" + pibScheme + "` is not supported"));
  }

  return std::make_tuple(pibScheme, pibLocation);
}

unique_ptr<Pib>
KeyChain::createPib(const std::string& pibLocator)
{
  std::string pibScheme, pibLocation;
  std::tie(pibScheme, pibLocation) = parseAndCheckPibLocator(pibLocator);
  auto pibFactory = getPibFactories().find(pibScheme);
  BOOST_ASSERT(pibFactory != getPibFactories().end());
  return unique_ptr<Pib>(new Pib(pibScheme, pibLocation, pibFactory->second(pibLocation)));
}

std::tuple<std::string/*type*/, std::string/*location*/>
KeyChain::parseAndCheckTpmLocator(const std::string& tpmLocator)
{
  std::string tpmScheme, tpmLocation;
  std::tie(tpmScheme, tpmLocation) = parseLocatorUri(tpmLocator);

  if (tpmScheme.empty()) {
    tpmScheme = getDefaultTpmScheme();
  }

  auto tpmFactory = getTpmFactories().find(tpmScheme);
  if (tpmFactory == getTpmFactories().end()) {
    NDN_THROW(Error("TPM scheme `" + tpmScheme + "` is not supported"));
  }

  return std::make_tuple(tpmScheme, tpmLocation);
}

unique_ptr<Tpm>
KeyChain::createTpm(const std::string& tpmLocator)
{
  std::string tpmScheme, tpmLocation;
  std::tie(tpmScheme, tpmLocation) = parseAndCheckTpmLocator(tpmLocator);
  auto tpmFactory = getTpmFactories().find(tpmScheme);
  BOOST_ASSERT(tpmFactory != getTpmFactories().end());
  return unique_ptr<Tpm>(new Tpm(tpmScheme, tpmLocation, tpmFactory->second(tpmLocation)));
}

// private: signing

Certificate
KeyChain::selfSign(Key& key)
{
  Certificate certificate;

  // set name
  Name certificateName = key.getName();
  certificateName
    .append("self")
    .appendVersion();
  certificate.setName(certificateName);

  // set metainfo
  certificate.setContentType(tlv::ContentType_Key);
  certificate.setFreshnessPeriod(1_h);

  // set content
  certificate.setContent(key.getPublicKey());

  // set signature-info
  SignatureInfo signatureInfo;
  // Note time::system_clock::max() or other NotAfter date results in incorrect encoded value
  // because of overflow during conversion to boost::posix_time::ptime (bug #3915).
  signatureInfo.setValidityPeriod(ValidityPeriod(time::system_clock::TimePoint(),
                                                 time::system_clock::now() + 20 * 365_days));

  sign(certificate, SigningInfo(key).setSignatureInfo(signatureInfo));

  key.addCertificate(certificate);
  return certificate;
}

std::tuple<Name, SignatureInfo>
KeyChain::prepareSignatureInfo(const SigningInfo& params)
{
  SignatureInfo sigInfo = params.getSignatureInfo();
  pib::Identity identity;
  pib::Key key;

  switch (params.getSignerType()) {
    case SigningInfo::SIGNER_TYPE_NULL: {
      try {
        identity = m_pib->getDefaultIdentity();
      }
      catch (const Pib::Error&) { // no default identity, use sha256 for signing.
        sigInfo.setSignatureType(tlv::DigestSha256);
        NDN_LOG_TRACE("Prepared signature info: " << sigInfo);
        return std::make_tuple(SigningInfo::getDigestSha256Identity(), sigInfo);
      }
      break;
    }
    case SigningInfo::SIGNER_TYPE_ID: {
      identity = params.getPibIdentity();
      if (!identity) {
        try {
          identity = m_pib->getIdentity(params.getSignerName());
        }
        catch (const Pib::Error&) {
          NDN_THROW_NESTED(InvalidSigningInfoError("Signing identity `" +
                                                   params.getSignerName().toUri() + "` does not exist"));
        }
      }
      break;
    }
    case SigningInfo::SIGNER_TYPE_KEY: {
      key = params.getPibKey();
      if (!key) {
        Name identityName = extractIdentityFromKeyName(params.getSignerName());
        try {
          key = m_pib->getIdentity(identityName).getKey(params.getSignerName());
        }
        catch (const Pib::Error&) {
          NDN_THROW_NESTED(InvalidSigningInfoError("Signing key `" +
                                                   params.getSignerName().toUri() + "` does not exist"));
        }
      }
      break;
    }
    case SigningInfo::SIGNER_TYPE_CERT: {
      Name identityName = extractIdentityFromCertName(params.getSignerName());
      Name keyName = extractKeyNameFromCertName(params.getSignerName());
      try {
        identity = m_pib->getIdentity(identityName);
        key = identity.getKey(keyName);
      }
      catch (const Pib::Error&) {
        NDN_THROW_NESTED(InvalidSigningInfoError("Signing certificate `" +
                                                 params.getSignerName().toUri() + "` does not exist"));
      }
      break;
    }
    case SigningInfo::SIGNER_TYPE_SHA256: {
      sigInfo.setSignatureType(tlv::DigestSha256);
      NDN_LOG_TRACE("Prepared signature info: " << sigInfo);
      return std::make_tuple(SigningInfo::getDigestSha256Identity(), sigInfo);
    }
    case SigningInfo::SIGNER_TYPE_HMAC: {
      const Name& keyName = params.getSignerName();
      if (!m_tpm->hasKey(keyName)) {
        m_tpm->importPrivateKey(keyName, params.getHmacKey());
      }
      sigInfo.setSignatureType(getSignatureType(KeyType::HMAC, params.getDigestAlgorithm()));
      sigInfo.setKeyLocator(keyName);
      NDN_LOG_TRACE("Prepared signature info: " << sigInfo);
      return std::make_tuple(keyName, sigInfo);
    }
    default: {
      NDN_THROW(InvalidSigningInfoError("Unrecognized signer type " +
                                        boost::lexical_cast<std::string>(params.getSignerType())));
    }
  }

  if (!key) {
    if (!identity) {
      NDN_THROW(InvalidSigningInfoError("Cannot determine signing parameters"));
    }
    try {
      key = identity.getDefaultKey();
    }
    catch (const Pib::Error&) {
      NDN_THROW_NESTED(InvalidSigningInfoError("Signing identity `" + identity.getName().toUri() +
                                               "` does not have a default certificate"));
    }
  }

  BOOST_ASSERT(key);

  sigInfo.setSignatureType(getSignatureType(key.getKeyType(), params.getDigestAlgorithm()));
  sigInfo.setKeyLocator(key.getName());

  NDN_LOG_TRACE("Prepared signature info: " << sigInfo);
  return std::make_tuple(key.getName(), sigInfo);
}


//added_GM, by liupenghui
#if 1
ConstBufferPtr
KeyChain::sign(const InputBuffers& bufs, const Name& keyName, KeyType keyType, DigestAlgorithm digestAlgorithm) const
{
  using namespace transform;

  if (keyName == SigningInfo::getDigestSha256Identity()) {
    OBufferStream os;
    bufferSource(bufs) >> digestFilter(DigestAlgorithm::SHA256) >> streamSink(os);
    return os.buf();
  }
	
  if (keyType == KeyType::SM2)
  	digestAlgorithm = DigestAlgorithm::SM3;

  auto signature = m_tpm->sign(bufs, keyName, keyType, digestAlgorithm);
  if (!signature) {
    NDN_THROW(InvalidSigningInfoError("TPM signing failed for key `" + keyName.toUri() + "` "
                                      "(e.g., PIB contains info about the key, but TPM is missing "
                                      "the corresponding private key)"));
  }

  return signature;
}
#else
ConstBufferPtr
KeyChain::sign(const InputBuffers& bufs, const Name& keyName, DigestAlgorithm digestAlgorithm) const
{
  using namespace transform;

  if (keyName == SigningInfo::getDigestSha256Identity()) {
    OBufferStream os;
    bufferSource(bufs) >> digestFilter(DigestAlgorithm::SHA256) >> streamSink(os);
    return os.buf();
  }

  auto signature = m_tpm->sign(bufs, keyName, digestAlgorithm);
  if (!signature) {
    NDN_THROW(InvalidSigningInfoError("TPM signing failed for key `" + keyName.toUri() + "` "
                                      "(e.g., PIB contains info about the key, but TPM is missing "
                                      "the corresponding private key)"));
  }

  return signature;
}

#endif


tlv::SignatureTypeValue
KeyChain::getSignatureType(KeyType keyType, DigestAlgorithm)
{
  switch (keyType) {
  case KeyType::RSA:
    return tlv::SignatureSha256WithRsa;
  case KeyType::EC:
    return tlv::SignatureSha256WithEcdsa;
//added_GM, by liupenghui
#if 1
  case KeyType::SM2:
	return tlv::SignatureSm3WithSm2;
#endif	
  case KeyType::HMAC:
    return tlv::SignatureHmacWithSha256;
  default:
    NDN_THROW(Error("Unsupported key type " + boost::lexical_cast<std::string>(keyType)));
  }
}

} // inline namespace v2
} // namespace security
} // namespace ndn
