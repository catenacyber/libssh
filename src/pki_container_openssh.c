/*
 * pki_container_openssh.c
 * This file is part of the SSH Library
 *
 * Copyright (c) 2013,2014 Aris Adamantiadis <aris@badcode.be>
 *
 * The SSH Library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * The SSH Library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with the SSH Library; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA.
 */

/**
 * @ingroup libssh_pki
 * *
 * @{
 */

#include "config.h"

#include <ctype.h>
#include <string.h>
#include <stdbool.h>

#include "libssh/libssh.h"
#include "libssh/priv.h"
#include "libssh/pki.h"
#include "libssh/pki_priv.h"
#include "libssh/buffer.h"


/**
 * @internal
 *
 * @brief Import a private key from a ssh buffer.
 *
 * @param[in] key_blob_buffer The key blob to import as specified in
 *                            key.c:key_private_serialize in OpenSSH source
 *                            code.
 *
 * @param[out] pkey     A pointer where the allocated key can be stored. You
 *                      need to free the memory using ssh_key_free().
 *
 * @return              SSH_OK on success, SSH_ERROR on error.
 *
 * @see ssh_key_free()
 */
static int pki_openssh_import_privkey_blob(ssh_buffer key_blob_buffer,
                                           ssh_key *pkey)
{
    enum ssh_keytypes_e type;
    char *type_s = NULL;
    ssh_key key = NULL;
    int rc;

    if (pkey == NULL) {
        return SSH_ERROR;
    }

    rc = ssh_buffer_unpack(key_blob_buffer, "s", &type_s);
    if (rc == SSH_ERROR){
        SSH_LOG(SSH_LOG_TRACE, "Unpack error");
        return SSH_ERROR;
    }

    type = ssh_key_type_from_name(type_s);
    if (type == SSH_KEYTYPE_UNKNOWN) {
        SSH_LOG(SSH_LOG_TRACE, "Unknown key type '%s' found!", type_s);
        return SSH_ERROR;
    }
    SAFE_FREE(type_s);

    rc = pki_import_privkey_buffer(type, key_blob_buffer, &key);
    if (rc != SSH_OK) {
        SSH_LOG(SSH_LOG_TRACE, "Failed to read key in OpenSSH format");
        goto fail;
    }

    *pkey = key;
    return SSH_OK;
fail:
    ssh_key_free(key);

    return SSH_ERROR;
}

/**
 * @brief decrypts an encrypted private key blob in OpenSSH format.
 *
 */
static int pki_private_key_decrypt(ssh_string blob,
                                   const char* passphrase,
                                   const char *ciphername,
                                   const char *kdfname,
                                   ssh_string kdfoptions,
                                   ssh_auth_callback auth_fn,
                                   void *auth_data)
{
    struct ssh_cipher_struct *ciphers = ssh_get_ciphertab();
    struct ssh_cipher_struct cipher;
    uint8_t key_material[128] = {0};
    char passphrase_buffer[128] = {0};
    size_t key_material_len;
    ssh_buffer buffer = NULL;
    ssh_string salt = NULL;
    uint32_t rounds;
    int cmp;
    int rc;
    int i;

    cmp = strcmp(ciphername, "none");
    if (cmp == 0){
        /* no decryption required */
        return SSH_OK;
    }

    for (i = 0; ciphers[i].name != NULL; i++) {
        cmp = strcmp(ciphername, ciphers[i].name);
        if (cmp == 0){
            memcpy(&cipher, &ciphers[i], sizeof(cipher));
            break;
        }
    }

    if (ciphers[i].name == NULL){
        SSH_LOG(SSH_LOG_TRACE, "Unsupported cipher %s", ciphername);
        return SSH_ERROR;
    }

    cmp = strcmp(kdfname, "bcrypt");
    if (cmp != 0) {
        SSH_LOG(SSH_LOG_TRACE, "Unsupported KDF %s", kdfname);
        return SSH_ERROR;
    }
    if (ssh_string_len(blob) % cipher.blocksize != 0) {
        SSH_LOG(SSH_LOG_TRACE,
                "Encrypted string not multiple of blocksize: %zu",
                ssh_string_len(blob));
        return SSH_ERROR;
    }

    buffer = ssh_buffer_new();
    if (buffer == NULL){
        return SSH_ERROR;
    }
    rc = ssh_buffer_add_data(buffer,
                             ssh_string_data(kdfoptions),
                             (uint32_t)ssh_string_len(kdfoptions));
    if (rc != SSH_ERROR){
        rc = ssh_buffer_unpack(buffer, "Sd", &salt, &rounds);
    }
    SSH_BUFFER_FREE(buffer);
    if (rc == SSH_ERROR){
        return SSH_ERROR;
    }

    /* We need material for key (keysize bits / 8) and IV (blocksize)  */
    key_material_len =  cipher.keysize/8 + cipher.blocksize;
    if (key_material_len > sizeof(key_material)) {
        SSH_LOG(SSH_LOG_TRACE, "Key material too big");
        return SSH_ERROR;
    }

    SSH_LOG(SSH_LOG_DEBUG,
            "Decryption: %d key, %d IV, %" PRIu32 " rounds, %zu bytes salt",
            cipher.keysize/8,
            cipher.blocksize,
            rounds,
            ssh_string_len(salt));

    if (passphrase == NULL) {
        if (auth_fn == NULL) {
            SAFE_FREE(salt);
            SSH_LOG(SSH_LOG_TRACE, "No passphrase provided");
            return SSH_ERROR;
        }
        rc = auth_fn("Passphrase",
                     passphrase_buffer,
                     sizeof(passphrase_buffer),
                     0,
                     0,
                     auth_data);
        if (rc != SSH_OK) {
            SAFE_FREE(salt);
            return SSH_ERROR;
        }
        passphrase = passphrase_buffer;
    }

    rc = bcrypt_pbkdf(passphrase,
                      strlen(passphrase),
                      ssh_string_data(salt),
                      ssh_string_len(salt),
                      key_material,
                      key_material_len,
                      rounds);
    SAFE_FREE(salt);
    if (rc < 0){
        return SSH_ERROR;
    }
    explicit_bzero(passphrase_buffer, sizeof(passphrase_buffer));

    cipher.set_decrypt_key(&cipher,
                           key_material,
                           key_material + cipher.keysize/8);
    cipher.decrypt(&cipher,
                   ssh_string_data(blob),
                   ssh_string_data(blob),
                   ssh_string_len(blob));
    ssh_cipher_clear(&cipher);
    return SSH_OK;
}


/** @internal
 * @brief Import a private key in OpenSSH (new) format. This format is
 * typically used with ed25519 keys but can be used for others.
 */
static ssh_key
ssh_pki_openssh_import(const char *text_key,
                       const char *passphrase,
                       ssh_auth_callback auth_fn,
                       void *auth_data,
                       bool private)
{
    const char *ptr = text_key;
    const char *end = NULL;
    char *base64 = NULL;
    int cmp;
    int rc;
    int i;
    ssh_buffer buffer = NULL, privkey_buffer = NULL;
    char *magic = NULL, *ciphername = NULL, *kdfname = NULL;
    uint32_t nkeys = 0, checkint1 = 0, checkint2 = 0xFFFF;
    ssh_string kdfoptions = NULL;
    ssh_string pubkey0 = NULL;
    ssh_string privkeys = NULL;
    ssh_string comment = NULL;
    ssh_key key = NULL;
    uint8_t padding;

    cmp = strncmp(ptr, OPENSSH_HEADER_BEGIN, strlen(OPENSSH_HEADER_BEGIN));
    if (cmp != 0) {
        SSH_LOG(SSH_LOG_TRACE, "Not an OpenSSH private key (no header)");
        goto out;
    }
    ptr += strlen(OPENSSH_HEADER_BEGIN);
    while(ptr[0] != '\0' && !isspace((int)ptr[0])) {
        ptr++;
    }
    end = strstr(ptr, OPENSSH_HEADER_END);
    if (end == NULL) {
        SSH_LOG(SSH_LOG_TRACE, "Not an OpenSSH private key (no footer)");
        goto out;
    }
    base64 = malloc(end - ptr + 1);
    if (base64 == NULL) {
        goto out;
    }
    for (i = 0; ptr < end; ptr++) {
        if (!isspace((int)ptr[0])) {
            base64[i] = ptr[0];
            i++;
        }
    }
    base64[i] = '\0';
    buffer = base64_to_bin(base64);
    SAFE_FREE(base64);
    if (buffer == NULL) {
        SSH_LOG(SSH_LOG_TRACE, "Not an OpenSSH private key (base64 error)");
        goto out;
    }
    rc = ssh_buffer_unpack(buffer, "PssSdSS",
                           strlen(OPENSSH_AUTH_MAGIC) + 1,
                           &magic,
                           &ciphername,
                           &kdfname,
                           &kdfoptions,
                           &nkeys,
                           &pubkey0,
                           &privkeys);
    if (rc == SSH_ERROR) {
        SSH_LOG(SSH_LOG_TRACE, "Not an OpenSSH private key (unpack error)");
        goto out;
    }
    cmp = strncmp(magic, OPENSSH_AUTH_MAGIC, strlen(OPENSSH_AUTH_MAGIC));
    if (cmp != 0) {
        SSH_LOG(SSH_LOG_TRACE, "Not an OpenSSH private key (bad magic)");
        goto out;
    }
    SSH_LOG(SSH_LOG_DEBUG,
            "Opening OpenSSH private key: ciphername: %s, kdf: %s, nkeys: %" PRIu32,
            ciphername,
            kdfname,
            nkeys);
    if (nkeys != 1) {
        SSH_LOG(SSH_LOG_TRACE, "Opening OpenSSH private key: only 1 key supported (%" PRIu32 " available)", nkeys);
        goto out;
    }

    /* If we are interested only in public key do not progress
     * to the key decryption later
     */
    if (!private) {
        rc = ssh_pki_import_pubkey_blob(pubkey0, &key);
        if (rc != SSH_OK) {
            SSH_LOG(SSH_LOG_TRACE, "Failed to import public key blob");
        }
        /* in either case we clean up here */
        goto out;
    }

    rc = pki_private_key_decrypt(privkeys,
                                 passphrase,
                                 ciphername,
                                 kdfname,
                                 kdfoptions,
                                 auth_fn,
                                 auth_data);
    if (rc == SSH_ERROR) {
        goto out;
    }

    privkey_buffer = ssh_buffer_new();
    if (privkey_buffer == NULL) {
        goto out;
    }

    ssh_buffer_set_secure(privkey_buffer);
    ssh_buffer_add_data(privkey_buffer,
                        ssh_string_data(privkeys),
                        (uint32_t)ssh_string_len(privkeys));

    rc = ssh_buffer_unpack(privkey_buffer, "dd", &checkint1, &checkint2);
    if (rc == SSH_ERROR || checkint1 != checkint2) {
        SSH_LOG(SSH_LOG_TRACE, "OpenSSH private key unpack error (correct password?)");
        goto out;
    }
    rc = pki_openssh_import_privkey_blob(privkey_buffer, &key);
    if (rc == SSH_ERROR) {
        goto out;
    }
    comment = ssh_buffer_get_ssh_string(privkey_buffer);
    SAFE_FREE(comment);
    /* verify that the remaining data is correct padding */
    for (i = 1; ssh_buffer_get_len(privkey_buffer) > 0; ++i) {
        ssh_buffer_get_u8(privkey_buffer, &padding);
        if (padding != i) {
            ssh_key_free(key);
            key = NULL;
            SSH_LOG(SSH_LOG_TRACE, "Invalid padding");
            goto out;
        }
    }
out:
    if (buffer != NULL) {
        SSH_BUFFER_FREE(buffer);
        buffer = NULL;
    }
    if (privkey_buffer != NULL) {
        SSH_BUFFER_FREE(privkey_buffer);
        privkey_buffer = NULL;
    }
    SAFE_FREE(magic);
    SAFE_FREE(ciphername);
    SAFE_FREE(kdfname);
    SAFE_FREE(kdfoptions);
    SAFE_FREE(pubkey0);
    SAFE_FREE(privkeys);
    return key;
}

ssh_key ssh_pki_openssh_privkey_import(const char *text_key,
                                       const char *passphrase,
                                       ssh_auth_callback auth_fn,
                                       void *auth_data)
{
    return ssh_pki_openssh_import(text_key, passphrase, auth_fn, auth_data, true);
}

ssh_key ssh_pki_openssh_pubkey_import(const char *text_key)
{
    return ssh_pki_openssh_import(text_key, NULL, NULL, NULL, false);
}


/** @internal
 * @brief encrypts an ed25519 private key blob
 *
 */
static int pki_private_key_encrypt(ssh_buffer privkey_buffer,
                                   const char* passphrase,
                                   const char *ciphername,
                                   const char *kdfname,
                                   ssh_auth_callback auth_fn,
                                   void *auth_data,
                                   uint32_t rounds,
                                   ssh_string salt)
{
    struct ssh_cipher_struct *ciphers = ssh_get_ciphertab();
    struct ssh_cipher_struct cipher;
    uint8_t key_material[128] = {0};
    size_t key_material_len;
    char passphrase_buffer[128] = {0};
    int rc;
    int i;
    int cmp;

    cmp = strcmp(ciphername, "none");
    if (cmp == 0){
        /* no encryption required */
        return SSH_OK;
    }

    for (i = 0; ciphers[i].name != NULL; i++) {
        cmp = strcmp(ciphername, ciphers[i].name);
        if (cmp == 0){
            memcpy(&cipher, &ciphers[i], sizeof(cipher));
            break;
        }
    }

    if (ciphers[i].name == NULL){
        SSH_LOG(SSH_LOG_TRACE, "Unsupported cipher %s", ciphername);
        return SSH_ERROR;
    }

    cmp = strcmp(kdfname, "bcrypt");
    if (cmp != 0){
        SSH_LOG(SSH_LOG_TRACE, "Unsupported KDF %s", kdfname);
        return SSH_ERROR;
    }
    /* We need material for key (keysize bits / 8) and IV (blocksize)  */
    key_material_len =  cipher.keysize/8 + cipher.blocksize;
    if (key_material_len > sizeof(key_material)){
        SSH_LOG(SSH_LOG_TRACE, "Key material too big");
        return SSH_ERROR;
    }

    SSH_LOG(SSH_LOG_DEBUG, "Encryption: %d key, %d IV, %" PRIu32 " rounds, %zu bytes salt",
                cipher.keysize/8,
                cipher.blocksize, rounds, ssh_string_len(salt));

    if (passphrase == NULL){
        if (auth_fn == NULL){
            SSH_LOG(SSH_LOG_TRACE, "No passphrase provided");
            return SSH_ERROR;
        }
        rc = auth_fn("Passphrase",
                     passphrase_buffer,
                     sizeof(passphrase_buffer),
                     0,
                     0,
                     auth_data);
        if (rc != SSH_OK){
            return SSH_ERROR;
        }
        passphrase = passphrase_buffer;
    }

    rc = bcrypt_pbkdf(passphrase,
                      strlen(passphrase),
                      ssh_string_data(salt),
                      ssh_string_len(salt),
                      key_material,
                      key_material_len,
                      rounds);
    if (rc < 0){
        return SSH_ERROR;
    }

    cipher.set_encrypt_key(&cipher,
                           key_material,
                           key_material + cipher.keysize/8);
    cipher.encrypt(&cipher,
                   ssh_buffer_get(privkey_buffer),
                   ssh_buffer_get(privkey_buffer),
                   ssh_buffer_get_len(privkey_buffer));
    ssh_cipher_clear(&cipher);
    explicit_bzero(passphrase_buffer, sizeof(passphrase_buffer));

    return SSH_OK;
}


/** @internal
 * generate an OpenSSH private key (defined in PROTOCOL.key) and output it in text format.
 * @param privkey[in] private key to export
 * @returns an SSH string containing the text representation of the exported key.
 * @warning currently only supports ED25519 key types.
 */

ssh_string ssh_pki_openssh_privkey_export(const ssh_key privkey,
                                          const char *passphrase,
                                          ssh_auth_callback auth_fn,
                                          void *auth_data)
{
    ssh_buffer buffer = NULL;
    ssh_string str = NULL, blob = NULL;
    ssh_string pubkey_s = NULL;
    ssh_buffer privkey_buffer = NULL;
    uint32_t rnd;
    uint32_t rounds = 16;
    ssh_string salt = NULL;
    ssh_string kdf_options = NULL;
    int to_encrypt=0;
    unsigned char *b64 = NULL;
    uint32_t str_len, len;
    uint8_t padding = 1;
    int ok;
    int rc;

    if (privkey == NULL) {
        return NULL;
    }
    if (passphrase != NULL || auth_fn != NULL){
        SSH_LOG(SSH_LOG_DEBUG, "Enabling encryption for private key export");
        to_encrypt = 1;
    }
    buffer = ssh_buffer_new();
    rc = ssh_pki_export_pubkey_blob(privkey, &pubkey_s);
    if (buffer == NULL || rc != SSH_OK) {
        goto error;
    }

    ok = ssh_get_random(&rnd, sizeof(rnd), 0);
    if (!ok) {
        goto error;
    }

    privkey_buffer = ssh_buffer_new();
    if (privkey_buffer == NULL) {
        goto error;
    }

    rc = ssh_pki_export_privkey_blob(privkey, &blob);
    if (rc != SSH_OK) {
        goto error;
    }

    rc = ssh_buffer_pack(privkey_buffer,
                         "ddPs",
                         rnd, /* checkint 1 & 2 */
                         rnd,
                         ssh_string_len(blob),
                         ssh_string_data(blob),
                         "" /* comment */);
    if (rc == SSH_ERROR){
        goto error;
    }

    /* Add padding regardless encryption because it is expected
     * by OpenSSH tools.
     * XXX Using 16 B as we use only AES cipher below anyway.
     */
    while (ssh_buffer_get_len(privkey_buffer) % 16 != 0) {
        rc = ssh_buffer_add_u8(privkey_buffer, padding);
        if (rc < 0) {
            goto error;
        }
        padding++;
    }

    if (to_encrypt){
        ssh_buffer kdf_buf;

        kdf_buf = ssh_buffer_new();
        if (kdf_buf == NULL) {
            goto error;
        }

        salt = ssh_string_new(16);
        if (salt == NULL){
            SSH_BUFFER_FREE(kdf_buf);
            goto error;
        }

        ok = ssh_get_random(ssh_string_data(salt), 16, 0);
        if (!ok) {
            SSH_BUFFER_FREE(kdf_buf);
            goto error;
        }

        rc = ssh_buffer_pack(kdf_buf, "Sd", salt, rounds);
        if (rc != SSH_OK) {
            SSH_BUFFER_FREE(kdf_buf);
            goto error;
        }
        kdf_options = ssh_string_new(ssh_buffer_get_len(kdf_buf));
        if (kdf_options == NULL){
            SSH_BUFFER_FREE(kdf_buf);
            goto error;
        }
        memcpy(ssh_string_data(kdf_options),
               ssh_buffer_get(kdf_buf),
               ssh_buffer_get_len(kdf_buf));
        SSH_BUFFER_FREE(kdf_buf);
        rc = pki_private_key_encrypt(privkey_buffer,
                                     passphrase,
                                     "aes128-cbc",
                                     "bcrypt",
                                     auth_fn,
                                     auth_data,
                                     rounds,
                                     salt);
        if (rc != SSH_OK){
            goto error;
        }
    } else {
        kdf_options = ssh_string_new(0);
    }

    rc = ssh_buffer_pack(buffer,
                         "PssSdSdP",
                         strlen(OPENSSH_AUTH_MAGIC) + 1,
                         OPENSSH_AUTH_MAGIC,
                         to_encrypt ? "aes128-cbc" : "none", /* ciphername */
                         to_encrypt ? "bcrypt" : "none",     /* kdfname */
                         kdf_options,                        /* kdfoptions */
                         (uint32_t)1,                        /* nkeys */
                         pubkey_s,
                         ssh_buffer_get_len(privkey_buffer),
                         /* rest of buffer is a string */
                         (size_t)ssh_buffer_get_len(privkey_buffer),
                         ssh_buffer_get(privkey_buffer));
    if (rc != SSH_OK) {
        goto error;
    }

    b64 = bin_to_base64(ssh_buffer_get(buffer),
                        ssh_buffer_get_len(buffer));
    if (b64 == NULL){
        goto error;
    }

    /* we can reuse the buffer */
    ssh_buffer_reinit(buffer);
    rc = ssh_buffer_pack(buffer,
                         "tttttt",
                         OPENSSH_HEADER_BEGIN,
                         "\n",
                         b64,
                         "\n",
                         OPENSSH_HEADER_END,
                         "\n");
    explicit_bzero(b64, strlen((char *)b64));
    SAFE_FREE(b64);

    if (rc != SSH_OK){
        goto error;
    }

    str = ssh_string_new(ssh_buffer_get_len(buffer));
    if (str == NULL){
        goto error;
    }

    str_len = ssh_buffer_get_len(buffer);
    len = ssh_buffer_get_data(buffer, ssh_string_data(str), str_len);
    if (str_len != len) {
        SSH_STRING_FREE(str);
        str = NULL;
    }

error:
    ssh_string_burn(blob);
    ssh_string_free(blob);
    if (privkey_buffer != NULL) {
        void *bufptr = ssh_buffer_get(privkey_buffer);
        explicit_bzero(bufptr, ssh_buffer_get_len(privkey_buffer));
        SSH_BUFFER_FREE(privkey_buffer);
    }
    SAFE_FREE(pubkey_s);
    SAFE_FREE(kdf_options);
    SAFE_FREE(salt);
    if (buffer != NULL) {
        SSH_BUFFER_FREE(buffer);
    }

    return str;
}


/**
 * @}
 */
