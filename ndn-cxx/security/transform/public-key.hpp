/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2013-2021 Regents of the University of California.
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

#ifndef NDN_CXX_SECURITY_TRANSFORM_PUBLIC_KEY_HPP
#define NDN_CXX_SECURITY_TRANSFORM_PUBLIC_KEY_HPP

#include "ndn-cxx/security/security-common.hpp"
#include "ndn-cxx/encoding/buffer.hpp"

namespace ndn {
namespace security {
namespace transform {

/**
 * @brief Abstraction of public key in crypto transformation
 */
class PublicKey : noncopyable
{
public:
  class Error : public std::runtime_error
  {
  public:
    using std::runtime_error::runtime_error;
  };

public:
  /**
   * @brief Create an empty public key instance
   *
   * One must call loadXXXX(...) to load a public key.
   */
  PublicKey();

  ~PublicKey();

  /**
   * @brief Get the type of the public key
   */
  KeyType
  getKeyType() const;

  /**
   * @brief Load the public key in PKCS#8 format from a buffer @p buf
   */
  void
  loadPkcs8(span<const uint8_t> buf);

  /**
   * @brief Load the public key in PKCS#8 format from a buffer @p buf
   * @deprecated
   */
  [[deprecated("use the overload that takes a span<>")]]
  void
  loadPkcs8(const uint8_t* buf, size_t size)
  {
    loadPkcs8({buf, size});
  }

  /**
   * @brief Load the public key in PKCS#8 format from a stream @p is
   */
  void
  loadPkcs8(std::istream& is);

  /**
   * @brief Load the public key in base64-encoded PKCS#8 format from a buffer @p buf
   */
  void
  loadPkcs8Base64(span<const uint8_t> buf);

  /**
   * @brief Load the public key in base64-encoded PKCS#8 format from a buffer @p buf
   * @deprecated
   */
  [[deprecated("use the overload that takes a span<>")]]
  void
  loadPkcs8Base64(const uint8_t* buf, size_t size)
  {
    loadPkcs8Base64({buf, size});
  }

  /**
   * @brief Load the public key in base64-encoded PKCS#8 format from a stream @p is
   */
  void
  loadPkcs8Base64(std::istream& is);

  /**
   * @brief Save the public key in PKCS#8 format into a stream @p os
   */
  void
  savePkcs8(std::ostream& os) const;

  /**
   * @brief Save the public key in base64-encoded PKCS#8 format into a stream @p os
   */
  void
  savePkcs8Base64(std::ostream& os) const;

  /**
   * @return Cipher text of @p plainText encrypted using this public key.
   *
   * Only RSA encryption is supported for now.
   */
//added_GM, by liupenghui
#if 1
  ConstBufferPtr
  encrypt(span<const uint8_t> plainText, KeyType keyType) const;
  /**
   * @return Cipher text of @p plainText encrypted using this public key.
   * @deprecated
   *
   * Only RSA encryption is supported for now.
   */
  [[deprecated("use the overload that takes a span<>")]]
  ConstBufferPtr
  encrypt(const uint8_t* plainText, size_t plainLen, KeyType keyType) const
  {
    return encrypt({plainText, plainLen}, keyType);
  }
#else
  ConstBufferPtr
  encrypt(span<const uint8_t> plainText) const;

  /**
   * @return Cipher text of @p plainText encrypted using this public key.
   * @deprecated
   *
   * Only RSA encryption is supported for now.
   */
  [[deprecated("use the overload that takes a span<>")]]
  ConstBufferPtr
  encrypt(const uint8_t* plainText, size_t plainLen) const
  {
    return encrypt({plainText, plainLen});
  }
#endif 

private:
  friend class VerifierFilter;

  /**
   * @return A pointer to an OpenSSL EVP_PKEY instance.
   *
   * The caller needs to explicitly cast the return value to `EVP_PKEY*`.
   */
  void*
  getEvpPkey() const;

private:
  ConstBufferPtr
  toPkcs8() const;

  ConstBufferPtr
  rsaEncrypt(span<const uint8_t> plainText) const;
//added_GM, by liupenghui
#if 1
  ConstBufferPtr
  sm2Encrypt(span<const uint8_t> plainText) const;
  
#endif

private:
  class Impl;
  const unique_ptr<Impl> m_impl;
};

} // namespace transform
} // namespace security
} // namespace ndn

#endif // NDN_CXX_SECURITY_TRANSFORM_PUBLIC_KEY_HPP
