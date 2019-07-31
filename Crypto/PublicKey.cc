//
// PublicKey.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
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

#include "PublicKey.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "mbedUtils.hh"
#include "pk.h"

namespace litecore { namespace crypto {
    using namespace std;
    using namespace fleece;


    Key::Key()                     {_pk = new mbedtls_pk_context; mbedtls_pk_init(_pk);}
    Key::~Key()                    {if (!_owner) {mbedtls_pk_free(_pk); delete _pk;}}

    string Key::description() {
        return format("%zd-bit %s %s key", mbedtls_pk_get_bitlen(_pk), mbedtls_pk_get_name(_pk),
                      (isPrivate() ? "private" : "public"));
    }


    alloc_slice Key::publicKeyDERData() {
        return allocDER(4096, [&](uint8_t *buf, size_t size) {
            return mbedtls_pk_write_pubkey_der(_pk, buf, size);
        });
    }

    alloc_slice Key::publicKeyRawData() {
        return allocDER(4096, [&](uint8_t *buf, size_t size) {
            auto pos = buf + size;
            return mbedtls_pk_write_pubkey(&pos, buf, _pk);
        });
    }

    alloc_slice Key::publicKeyData(KeyFormat format) {
        switch (format) {
        case KeyFormat::DER:
        case KeyFormat::PEM: {
            auto result = publicKeyDERData();
            if (format == KeyFormat::PEM)
                result = convertToPEM(result, "PUBLIC KEY");
            return result;
        }
        case KeyFormat::Raw:
            return publicKeyRawData();
        }
    }


    PublicKey::PublicKey(slice data) {
        TRY( mbedtls_pk_parse_public_key(context(), (const uint8_t*)data.buf, data.size) );
    }


    Retained<PrivateKey> PrivateKey::generateTemporaryRSA(unsigned keySizeInBits) {
        Retained<PrivateKey> key = new PrivateKey();
        auto ctx = key->context();
        TRY( mbedtls_pk_setup(ctx, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) );
        Log("Generating %u-bit RSA key-pair...", keySizeInBits);
        TRY( mbedtls_rsa_gen_key(mbedtls_pk_rsa(*ctx),
                                 mbedtls_ctr_drbg_random, RandomNumberContext(),
                                 keySizeInBits, 65537) );
        return key;
    }


    PersistentPrivateKey::PersistentPrivateKey(unsigned keySizeInBits, const string &label)
    :_keyLength( (keySizeInBits + 7) / 8)
    ,_label(label)
    {
        auto decryptFunc = [](void *ctx, int mode, size_t *olen,
                              const unsigned char *input, unsigned char *output,
                              size_t output_max_len ) -> int {
            return ((PersistentPrivateKey*)ctx)->_decrypt(input, output, output_max_len, olen);
        };

        auto signFunc = [](void *ctx,
                           int (*f_rng)(void *, unsigned char *, size_t), void *p_rng,
                           int mode, mbedtls_md_type_t md_alg, unsigned int hashlen,
                           const unsigned char *hash, unsigned char *sig ) -> int {
            return ((PersistentPrivateKey*)ctx)->_sign(md_alg, slice(hash, hashlen), sig);
        };

        auto keyLengthFunc = []( void *ctx ) -> size_t {
            return ((PersistentPrivateKey*)ctx)->_keyLength;
        };

        TRY( mbedtls_pk_setup_rsa_alt(context(), this, decryptFunc, signFunc, keyLengthFunc) );
    }


#if 0
    // NOTE: These factory functions are implemented in a per-platform source file such as
    // PublicKey+Apple.mm, because they need to call platform-specific APIs.

    Retained<KeyPair> PersistentPrivateKey::generateRSA(unsigned keySizeInBits, const string &label) {
        ... platform specific code...
    }

    Retained<KeyPair> PersistentPrivateKey::load(const string &label) {
        ... platform specific code...
    }

    bool PersistentPrivateKey::remove(const string &label) {
        ... platform specific code...
    }
#endif

} }
