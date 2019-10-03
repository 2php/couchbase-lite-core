//
// c4Certificate.h
//
// Copyright (c) 2019 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once

#include "c4Base.h"

#ifdef COUCHBASE_ENTERPRISE

#ifdef __cplusplus
extern "C" {
#endif

    /** \defgroup certificates Certificates
        @{ */

    /** Certificate types. A certificate may be of one or more of these.*/
    typedef C4_OPTIONS(uint8_t, C4CertUsage) { // Note: Same values as `MBEDTLS_X509_NS_CERT_TYPE_*`
        kC4CertUsage_NotSpecified      = 0x00, ///< No specified usage (not generally useful)
        kC4CertUsage_TLSClient         = 0x80, ///< TLS (SSL) client cert
        kC4CertUsage_TLSServer         = 0x40, ///< TLS (SSL) server cert
        kC4CertUsage_Email             = 0x20, ///< Email signing and encryption
        kC4CertUsage_ObjectSigning     = 0x10, ///< Signing arbitrary data
        kC4CertUsage_TLS_CA            = 0x04, ///< CA for signing TLS cert requests
        kC4CertUsage_Email_CA          = 0x02, ///< CA for signing email cert requests
        kC4CertUsage_ObjectSigning_CA  = 0x01  ///< CA for signing object-signing cert requests
    };


    /** Parameters for signing a certificate. These will be used by the Certificate Authority
        (CA), which might be the same as the subject if self-signing. */
    typedef struct {
        unsigned validityInSeconds;         ///< seconds from signing till expiration (default 1 year)
        C4String serialNumber;              ///< serial number string (default "1")
        int maxPathLen;                     ///< maximum CA path length (default -1, meaning none)
        bool isCA;                          ///< will this be a CA certificate? (default false)
        bool addAuthorityIdentifier;        ///< add authority identifier to cert? (default true)
        bool addSubjectIdentifier;          ///< add subject identifier to cert? (default true)
        bool addBasicConstraints;           ///< add basic constraints extension? (default true)
    } C4CertIssuerParameters;

    /** Default issuer parameters. Every C4CertIssuerParameters should be initialized from this. */
    extern const C4CertIssuerParameters kDefaultCertIssuerParameters;


    /** Standard Distinguished Name attributes, from RFC 4519.
        Only the CommonName is required; it's used as the visible name of the certificate.
        If the cert is to be used for a TLS server, the CommonName must match its DNS name. */
    typedef C4_ENUM(uint8_t, C4CertNameAttributeID) {
        kC4Cert_CommonName,        // "Jane Doe", (or "jane.example.com")
        kC4Cert_Pseudonym,         // "plainjane837"
        kC4Cert_GivenName,         // "Jane"
        kC4Cert_Surname,           // "Doe"
        kC4Cert_EmailAddress,      // "jane@example.com"
        kC4Cert_Organization,      // "Example Corp."
        kC4Cert_OrganizationUnit,  // "Marketing"
        kC4Cert_PostalAddress,     // "123 Example Blvd #2A"
        kC4Cert_Locality,          // "Boston"
        kC4Cert_PostalCode,        // "02134"
        kC4Cert_StateOrProvince,   // "Massachusetts" (or "Quebec", ...)
        kC4Cert_Country,           // "us" (2-letter ISO country code)
    };

    /** A component of an X.509 "Relative Distinguished Name". */
    typedef struct {
        C4CertNameAttributeID attributeID; ///< Attribute ID; use one of the `kC4Cert_...` symbols defined
                                ///< above. Arbitrary strings are not supported!
        C4String value;         ///< Value of the attribute
    } C4CertNameComponent;


    /** \name Certificate and CSR Functions
     @{ */

    /** Creates a Certificate Signing Request, i.e. an unsigned certificate.
        \note You are responsible for releasing the returned reference.
        @param nameComponents  Pointer to an array of one or more \ref C4CertNameComponent structs.
        @param nameCount  Number of items in the \p nameComponents array.
        @param certUsages  Flags giving intended usage. (The certificate will be rejected by peers
                if you try to use it for something not specified in its certUsages!)
        @param subjectKey  The owner's private key that this certificate will attest to.
        @param outError  On failure, the error info will be stored here.
        @return  The new certificate request, or NULL on failure. */
    C4Cert* c4cert_createRequest(const C4CertNameComponent *nameComponents C4NONNULL,
                                 size_t nameCount,
                                 C4CertUsage certUsages,
                                 C4KeyPair *subjectKey C4NONNULL,
                                 C4Error *outError) C4API;

    /** Instantiates a C4Cert from X.509 certificate data in DER or PEM form.
        \note PEM data might consist of a series of certificates. If so, the returned C4Cert
              will represent only the first, and you can iterate over the next by calling
              \ref c4cert_nextInChain.
        \note You are responsible for releasing the returned reference. */
    C4Cert* c4cert_fromData(C4Slice certData,
                            C4Error *outError) C4API;

    /** Returns the encoded X.509 data in DER (binary) or PEM (ASCII) form.
        \warning DER format can only encode a _single_ certificate, so if this C4Cert includes
            multiple certificates, use PEM format to preserve them.
        \note You are responsible for releasing the returned data. */
    C4SliceResult c4cert_copyData(C4Cert* C4NONNULL,
                                  bool pemEncoded) C4API;

    /** Returns the cert's Subject Name, which identifies the cert's owner.
        This is an X.509 structured string consisting of "KEY=VALUE" pairs separated by commas,
        where the keys are attribute names. (Commas in values are backslash-escaped.)
        \note Rather than parsing this yourself, use \ref c4cert_subjectNameComponent.
        \note You are responsible for releasing the returned data. */
    C4StringResult c4cert_subjectName(C4Cert* C4NONNULL) C4API;

    /** Returns one component of a cert's subject name, given the attribute name.
        \note You are responsible for releasing the returned data. */
    C4StringResult c4cert_subjectNameComponent(C4Cert* C4NONNULL, C4CertNameAttributeID) C4API;

    /** Returns a human-readable, multi-line string describing the certificate in detail.
        \note You are responsible for releasing the returned data. */
    C4StringResult c4cert_summary(C4Cert* C4NONNULL) C4API;

    /** Returns true if this is a signed certificate, false if it's a signing request (CSR). */
    bool c4cert_isSigned(C4Cert* C4NONNULL) C4API;

    /** Signs an unsigned certificate (a CSR) and returns the new signed certificate.
        \note You are responsible for releasing the returned reference.
        @param cert  The unsigned certificate to be signed.
        @param params  Capabilities to store in the cert; if NULL, uses defaults.
        @param issuerPrivateKey  The Certificate Authority's private key. (If self-signing a
                    cert, this should be the same as the `subjectKey` it was created with.)
        @param issuerCert  The Certificate Authority's certificate (which must match
                    \p issuerPrivateKey), or NULL if self-signing.
        @param outError  On failure, the error info will be stored here.
        @return  The signed certificate, or NULL on failure. */
    C4Cert* c4cert_signRequest(C4Cert *cert C4NONNULL,
                               const C4CertIssuerParameters *params,
                               C4KeyPair *issuerPrivateKey C4NONNULL,
                               C4Cert *issuerCert,
                               C4Error *outError) C4API;

    /** Returns a certificate's public key.
        \note You are responsible for releasing the returned key reference. */
    C4KeyPair* c4cert_getPublicKey(C4Cert* C4NONNULL) C4API;

    /** Loads a certificate's matching private key from the OS's persistent store, if it exists,
        and returns the key-pair with both private and public key.
        \note You are responsible for releasing the returned key reference. */
    C4KeyPair* c4cert_loadPersistentPrivateKey(C4Cert* C4NONNULL,
                                           C4Error *outError) C4API;

    /** @} */



    /** \name Certificate Chains
     @{ */

    /** Returns the next certificate in the chain after this one, if any.
        \note You are responsible for releasing the returned reference. */
    C4Cert* c4cert_nextInChain(C4Cert* C4NONNULL) C4API;

    /** Returns the encoded data of this cert and the following ones in the chain, in PEM form.
        \note You are responsible for releasing the returned data.*/
    C4SliceResult c4cert_copyChainData(C4Cert* C4NONNULL) C4API;

    /** @} */



    /** \name Certificate Persistence
     @{ */

    /** Saves a certificate to a database for easy lookup by name, or deletes a saved cert.
        \note The certificate is saved as a "raw document", and will _not_ be replicated.
        @param cert  The certificate to store, or NULL to delete any saved cert with that name.
        @param entireChain  True if the entire cert chain should be saved.
        @param db  The database in which to store the certificate.
        @param name  The name to save as.
        @param outError  On failure, the error info will be stored here.
        @return  True on success, false on failure. */
    bool c4cert_save(C4Cert *cert,
                     bool entireChain,
                     C4Database *db C4NONNULL,
                     C4String name,
                     C4Error *outError);

    /** Loads a certificate from a database given the name it was saved under.
        \note You are responsible for releasing the returned key reference.
        @param db  The database the certificate was saved in.
        @param name  The name the certificate was saved with.
        @param outError  On failure, the error info will be stored here.
        @return  The certificate, or NULL if missing or if it failed to parse. */
    C4Cert* c4cert_load(C4Database *db C4NONNULL,
                        C4String name,
                        C4Error *outError);

    /** @} */



    /** \name Key-Pairs
     @{ */

    /** Supported key-pair algorithms. */
    typedef C4_ENUM(uint8_t, C4KeyPairAlgorithm) {
        kC4RSA,
    };


    /** Creates a new key-pair.
        \note You are responsible for releasing the returned reference.
        \warning Key-pairs should usually be persistent. This is more secure because the private
                 key data is extremely difficult to access. A non-persistent key-pair's private
                 key data lives in the process's heap, and if you store it yourself it's difficult
                 to do so securely.
        @param algorithm  The type of key to create, e.g. RSA.
        @param sizeInBits  The size (length) of the key in bits. Larger sizes are more secure.
                            Available key sizes depend on the key type.
        @param persistent  True if the key should be managed by the OS's persistent store.
        @param outError  On failure, the error info will be stored here.
        @return  The new key, or NULL on failure. */
    C4KeyPair* c4keypair_generate(C4KeyPairAlgorithm algorithm,
                                  unsigned sizeInBits,
                                  bool persistent,
                                  C4Error *outError) C4API;

    /** Loads a public key from its data.
        The resulting C4KeyPair will not have a private key.
        \note You are responsible for releasing the returned reference. */
    C4KeyPair* c4keypair_fromPublicKeyData(C4Slice publicKeyData) C4API;

    /** Loads a private key from its data.
        The resulting C4KeyPair will have both a public and private key.
        \note You are responsible for releasing the returned reference. */
    C4KeyPair* c4keypair_fromPrivateKeyData(C4Slice privateKeyData) C4API;

    /** Returns true if the C4KeyPair has a private as well as a public key. */
    bool c4keypair_hasPrivateKey(C4KeyPair* C4NONNULL) C4API;

    /** Returns a hex digest of the public key.
        \note You are responsible for releasing the returned data. */
    C4SliceResult c4keypair_publicKeyDigest(C4KeyPair* C4NONNULL) C4API;

    /** Returns the public key data.
        \note You are responsible for releasing the returned data. */
    C4SliceResult c4keypair_publicKeyData(C4KeyPair* C4NONNULL) C4API;

    /** Returns the private key data, if the private key is known and its data is accessible.
        \note Persistent private keys generally don't have accessible data.
        \note You are responsible for releasing the returned data. */
    C4SliceResult c4keypair_privateKeyData(C4KeyPair* C4NONNULL) C4API;

    /** Returns true if the C4KeyPair is stored int the OS's persistent store. */
    bool c4keypair_isPersistent(C4KeyPair* C4NONNULL) C4API;

    /** Attempts to find & load the persistent key-pair matching this public key.
        \note If there is no matching persistent key, returns NULL but sets no error.
        \note You are responsible for releasing the returned reference. */
    C4KeyPair* c4keypair_persistentWithPublicKey(C4KeyPair* C4NONNULL,
                                                 C4Error *outError) C4API;

    /** Removes a private key from persistent storage. */
    bool c4keypair_removePersistent(C4KeyPair* C4NONNULL,
                                C4Error *outError) C4API;

    /** @} */

#ifdef __cplusplus
}
#endif

#endif // COUCHBASE_ENTERPRISE
