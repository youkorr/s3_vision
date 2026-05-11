// Weak no-op stubs for the mbedtls AES API used by ESP-DL's fbs_loader to
// decrypt encrypted model blobs. We only ship plain (unencrypted) .espdl
// files, so the AES path is never reached at runtime - we just need the
// symbols to resolve so the linker is happy.
//
// If you ever need to load an encrypted model, remove this file and link
// the real mbedtls (it ships with ESP-IDF).

#include <stddef.h>
#include <stdint.h>

typedef struct mbedtls_aes_context_dummy {
    uint8_t _opaque[288];
} mbedtls_aes_context_dummy;

__attribute__((weak)) void mbedtls_aes_init(void *ctx) {
    (void) ctx;
}

__attribute__((weak)) void mbedtls_aes_free(void *ctx) {
    (void) ctx;
}

__attribute__((weak)) int mbedtls_aes_setkey_enc(void *ctx, const unsigned char *key, unsigned int keybits) {
    (void) ctx; (void) key; (void) keybits;
    return 0;
}

__attribute__((weak)) int mbedtls_aes_setkey_dec(void *ctx, const unsigned char *key, unsigned int keybits) {
    (void) ctx; (void) key; (void) keybits;
    return 0;
}

__attribute__((weak)) int mbedtls_aes_crypt_ctr(void *ctx,
                                                size_t length,
                                                size_t *nc_off,
                                                unsigned char nonce_counter[16],
                                                unsigned char stream_block[16],
                                                const unsigned char *input,
                                                unsigned char *output) {
    (void) ctx; (void) nc_off; (void) nonce_counter; (void) stream_block;
    // Caller will receive cleartext == ciphertext copy (which is fine because
    // we never actually feed encrypted data through this stub).
    if (input != output && input && output) {
        for (size_t i = 0; i < length; i++) output[i] = input[i];
    }
    return 0;
}
