/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#include <realm/util/aes_cryptor.hpp>
#include <realm/util/file_mapper.hpp>
#include <realm/utilities.hpp>

#if REALM_ENABLE_ENCRYPTION
#include <cstdlib>
#include <algorithm>

#ifdef REALM_DEBUG
#include <cstdio>
#include <iostream>
#endif

#include <cstring>
#include <pthread.h>

#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <realm/util/encrypted_file_mapping.hpp>
#include <realm/util/terminate.hpp>

namespace realm {
namespace util {

SharedFileInfo::SharedFileInfo(const uint8_t* key, File file_descriptor)
    : fd(file_descriptor)
    , cryptor(key)
{
}

// We have the following constraints here:
//
// 1. When writing, we only know which 4k page is dirty, and not what bytes
//    within the page are dirty, so we always have to write in 4k blocks.
// 2. Pages being written need to be entirely within an 8k-aligned block to
//    ensure that they're written to the hardware in atomic blocks.
// 3. We need to store the IV used for each 4k page somewhere, so that we can
//    ensure that we never reuse an IV (and still be decryptable).
//
// Because pages need to be aligned, we can't just prepend the IV to each page,
// or we'd have to double the size of the file (as the rest of the 4k block
// containing the IV would not be usable). Writing the IVs to a different part
// of the file from the data results in them not being in the same 8k block, and
// so it is possible that only the IV or only the data actually gets updated on
// disk. We deal with this by storing four pieces of data about each page: the
// hash of the encrypted data, the current IV, the hash of the previous encrypted
// data, and the previous IV. To write, we encrypt the data, hash the ciphertext,
// then write the new IV/ciphertext hash, fsync(), and then write the new
// ciphertext. This ensures that if an error occurs between writing the IV and
// the ciphertext, we can still determine that we should use the old IV, since
// the ciphertext's hash will match the old ciphertext.

struct iv_table {
    uint32_t iv1;
    uint8_t hmac1[28];
    uint32_t iv2;
    uint8_t hmac2[28];
};

namespace {
const int aes_block_size = 16;
const size_t block_size = 4096;

const size_t metadata_size = sizeof(iv_table);
const size_t blocks_per_metadata_block = block_size / metadata_size;

// map an offset in the data to the actual location in the file
template <typename Int>
Int real_offset(Int pos)
{
    REALM_ASSERT(pos >= 0);
    const size_t index = static_cast<size_t>(pos) / block_size;
    const size_t metadata_page_count = index / blocks_per_metadata_block + 1;
    return pos + metadata_page_count * block_size;
}

// map a location in the file to the offset in the data
template <typename Int>
Int fake_offset(Int pos)
{
    REALM_ASSERT(pos >= 0);
    const size_t index = static_cast<size_t>(pos) / block_size;
    const size_t metadata_page_count = (index + blocks_per_metadata_block) / (blocks_per_metadata_block + 1);
    return pos - metadata_page_count * block_size;
}

// get the location of the iv_table for the given data (not file) position
off_t iv_table_pos(off_t pos)
{
    REALM_ASSERT(pos >= 0);
    const size_t index = static_cast<size_t>(pos) / block_size;
    const size_t metadata_block = index / blocks_per_metadata_block;
    const size_t metadata_index = index & (blocks_per_metadata_block - 1);
    return metadata_block * (blocks_per_metadata_block + 1) * block_size + metadata_index * metadata_size;
}

void check_write(File fd, off_t pos, const void* data, size_t len)
{
	fd.seek(pos);
	fd.write((char*)data, len);
    //REALM_ASSERT(ret >= 0 && static_cast<size_t>(ret) == len);
}

size_t check_read(File fd, off_t pos, void* dst, size_t len)
{
	fd.seek(pos);
	fd.read((char*)dst, len);

//	ssize_t ret = pread(fd, dst, len, pos);
//    REALM_ASSERT(ret >= 0);
	return len;
//    return ret < 0 ? 0 : static_cast<size_t>(ret);
}

} // anonymous namespace

AESCryptor::AESCryptor(const uint8_t* key)
    : m_rw_buffer(new char[block_size])
{
#if REALM_PLATFORM_APPLE
    CCCryptorCreate(kCCEncrypt, kCCAlgorithmAES, 0 /* options */, key, kCCKeySizeAES256, 0 /* IV */, &m_encr);
    CCCryptorCreate(kCCDecrypt, kCCAlgorithmAES, 0 /* options */, key, kCCKeySizeAES256, 0 /* IV */, &m_decr);
#else
    AES_set_encrypt_key(key, 256 /* key size in bits */, &m_ectx);
    AES_set_decrypt_key(key, 256 /* key size in bits */, &m_dctx);
#endif
    memcpy(m_hmacKey, key + 32, 32);
}

AESCryptor::~AESCryptor() noexcept
{
#if REALM_PLATFORM_APPLE
    CCCryptorRelease(m_encr);
    CCCryptorRelease(m_decr);
#endif
}

void AESCryptor::set_file_size(off_t new_size)
{
    REALM_ASSERT(new_size >= 0 && !int_cast_has_overflow<size_t>(new_size));
    size_t new_size_casted = size_t(new_size);
    size_t block_count = (new_size_casted + block_size - 1) / block_size;
    m_iv_buffer.reserve((block_count + blocks_per_metadata_block - 1) & ~(blocks_per_metadata_block - 1));
}

iv_table& AESCryptor::get_iv_table(File fd, off_t data_pos) noexcept
{
    REALM_ASSERT(!int_cast_has_overflow<size_t>(data_pos));
    size_t data_pos_casted = size_t(data_pos);
    size_t idx = data_pos_casted / block_size;
    if (idx < m_iv_buffer.size())
        return m_iv_buffer[idx];

    size_t old_size = m_iv_buffer.size();
    size_t new_block_count = 1 + idx / blocks_per_metadata_block;
    REALM_ASSERT(new_block_count * blocks_per_metadata_block <= m_iv_buffer.capacity()); // not safe to allocate here
    m_iv_buffer.resize(new_block_count * blocks_per_metadata_block);

    for (size_t i = old_size; i < new_block_count * blocks_per_metadata_block; i += blocks_per_metadata_block) {
        size_t bytes = check_read(fd, iv_table_pos(i * block_size), &m_iv_buffer[i], block_size);
        if (bytes < block_size)
            break; // rest is zero-filled by resize()
    }

    return m_iv_buffer[idx];
}

bool AESCryptor::check_hmac(const void* src, size_t len, const uint8_t* hmac) const
{
    uint8_t buffer[224 / 8];
    calc_hmac(src, len, buffer, m_hmacKey);

    // Constant-time memcmp to avoid timing attacks
    uint8_t result = 0;
    for (size_t i = 0; i < 224 / 8; ++i)
        result |= buffer[i] ^ hmac[i];
    return result == 0;
}

bool AESCryptor::read(File fd, off_t pos, char* dst, size_t size)
{
    REALM_ASSERT(size % block_size == 0);
    while (size > 0) {
        ssize_t bytes_read = check_read(fd, real_offset(pos), m_rw_buffer.get(), block_size);

        if (bytes_read == 0)
            return false;

        iv_table& iv = get_iv_table(fd, pos);
        if (iv.iv1 == 0) {
            // This block has never been written to, so we've just read pre-allocated
            // space. No memset() since the code using this doesn't rely on
            // pre-allocated space being zeroed.
            return false;
        }

        if (!check_hmac(m_rw_buffer.get(), bytes_read, iv.hmac1)) {
            // Either the DB is corrupted or we were interrupted between writing the
            // new IV and writing the data
            if (iv.iv2 == 0) {
                // Very first write was interrupted
                return false;
            }

            if (check_hmac(m_rw_buffer.get(), bytes_read, iv.hmac2)) {
                // Un-bump the IV since the write with the bumped IV never actually
                // happened
                memcpy(&iv.iv1, &iv.iv2, 32);
            }
            else {
                // If the file has been shrunk and then re-expanded, we may have
                // old hmacs that don't go with this data. ftruncate() is
                // required to fill any added space with zeroes, so assume that's
                // what happened if the buffer is all zeroes
                for (ssize_t i = 0; i < bytes_read; ++i) {
                    if (m_rw_buffer[i] != 0)
                        throw DecryptionFailed();
                }
                return false;
            }
        }

        crypt(mode_Decrypt, pos, dst, m_rw_buffer.get(), reinterpret_cast<const char*>(&iv.iv1));

        pos += block_size;
        dst += block_size;
        size -= block_size;
    }
    return true;
}

void AESCryptor::write(File fd, off_t pos, const char* src, size_t size) noexcept
{
    REALM_ASSERT(size % block_size == 0);
    while (size > 0) {
        iv_table& iv = get_iv_table(fd, pos);

        memcpy(&iv.iv2, &iv.iv1, 32);
        do {
            ++iv.iv1;
            // 0 is reserved for never-been-used, so bump if we just wrapped around
            if (iv.iv1 == 0)
                ++iv.iv1;

            crypt(mode_Encrypt, pos, m_rw_buffer.get(), src, reinterpret_cast<const char*>(&iv.iv1));
            calc_hmac(m_rw_buffer.get(), block_size, iv.hmac1, m_hmacKey);
            // In the extremely unlikely case that both the old and new versions have
            // the same hash we won't know which IV to use, so bump the IV until
            // they're different.
        } while (REALM_UNLIKELY(memcmp(iv.hmac1, iv.hmac2, 4) == 0));

        check_write(fd, iv_table_pos(pos), &iv, sizeof(iv));
        check_write(fd, real_offset(pos), m_rw_buffer.get(), block_size);

        pos += block_size;
        src += block_size;
        size -= block_size;
    }
}

void AESCryptor::crypt(EncryptionMode mode, off_t pos, char* dst, const char* src, const char* stored_iv) noexcept
{
    uint8_t iv[aes_block_size] = {0};
    memcpy(iv, stored_iv, 4);
    memcpy(iv + 4, &pos, sizeof(pos));

#if REALM_PLATFORM_APPLE
    CCCryptorRef cryptor = mode == mode_Encrypt ? m_encr : m_decr;
    CCCryptorReset(cryptor, iv);

    size_t bytesEncrypted = 0;
    CCCryptorStatus err = CCCryptorUpdate(cryptor, src, block_size, dst, block_size, &bytesEncrypted);
    REALM_ASSERT(err == kCCSuccess);
    REALM_ASSERT(bytesEncrypted == block_size);
#else
    AES_cbc_encrypt(reinterpret_cast<const uint8_t*>(src), reinterpret_cast<uint8_t*>(dst), block_size,
                    mode == mode_Encrypt ? &m_ectx : &m_dctx, iv, mode);
#endif
}

void AESCryptor::calc_hmac(const void* src, size_t len, uint8_t* dst, const uint8_t* key) const
{
#if REALM_PLATFORM_APPLE
    CCHmac(kCCHmacAlgSHA224, key, 32, src, len, dst);
#else
    SHA256_CTX ctx;

    uint8_t ipad[64];
    for (size_t i = 0; i < 32; ++i)
        ipad[i] = key[i] ^ 0x36;
    memset(ipad + 32, 0x36, 32);

    uint8_t opad[64] = {0};
    for (size_t i = 0; i < 32; ++i)
        opad[i] = key[i] ^ 0x5C;
    memset(opad + 32, 0x5C, 32);

    // Full hmac operation is sha224(opad + sha224(ipad + data))
    SHA224_Init(&ctx);
    SHA256_Update(&ctx, ipad, 64);
    SHA256_Update(&ctx, static_cast<const uint8_t*>(src), len);
    SHA256_Final(dst, &ctx);

    SHA224_Init(&ctx);
    SHA256_Update(&ctx, opad, 64);
    SHA256_Update(&ctx, dst, SHA224_DIGEST_LENGTH);
    SHA256_Final(dst, &ctx);
#endif
}

EncryptedFileMapping::EncryptedFileMapping(SharedFileInfo& file, size_t file_offset, void* addr, size_t size,
                                           File::AccessMode access)
    : m_file(file)
    , m_page_shift(log2(realm::util::page_size()))
    , m_blocks_per_page((1 << m_page_shift) / block_size)
    , m_access(access)
#ifdef REALM_DEBUG
    , m_validate_buffer(new char[1 << m_page_shift])
#endif
{
    REALM_ASSERT(m_blocks_per_page * block_size == (1ULL << m_page_shift));
    set(addr, size, file_offset); // throws
    file.mappings.push_back(this);
}

EncryptedFileMapping::~EncryptedFileMapping()
{
    flush();
    sync();
    m_file.mappings.erase(remove(m_file.mappings.begin(), m_file.mappings.end(), this));
}

char* EncryptedFileMapping::page_addr(size_t i) const noexcept
{
    return reinterpret_cast<char*>((m_first_page + i) << m_page_shift);
}

void EncryptedFileMapping::mark_outdated(size_t i) noexcept
{
    if (i >= m_page_count)
        return;

    if (m_dirty_pages[i])
        flush();

    if (m_up_to_date_pages[i]) {
        m_up_to_date_pages[i] = false;
    }
}

void EncryptedFileMapping::mark_up_to_date(size_t i) noexcept
{
    if (i >= m_up_to_date_pages.size() || m_up_to_date_pages[i])
        return;

    m_up_to_date_pages[i] = true;
}

bool EncryptedFileMapping::copy_up_to_date_page(size_t page) noexcept
{
    for (size_t i = 0; i < m_file.mappings.size(); ++i) {
        EncryptedFileMapping* m = m_file.mappings[i];
        if (m == this || page >= m->m_page_count)
            continue;

        if (m->m_up_to_date_pages[page]) {
            memcpy(page_addr(page), m->page_addr(page), 1 << m_page_shift);
            return true;
        }
    }
    return false;
}

void EncryptedFileMapping::refresh_page(size_t i)
{
    char* addr = page_addr(i);

    if (!copy_up_to_date_page(i))
        m_file.cryptor.read(m_file.fd, i << m_page_shift, addr, 1 << m_page_shift);

    m_up_to_date_pages[i] = true;
}

void EncryptedFileMapping::write_page(size_t page) noexcept
{
    // Go through all other mappings of this file and mark
    // the page outdated in those mappings:
    for (size_t i = 0; i < m_file.mappings.size(); ++i) {
        EncryptedFileMapping* m = m_file.mappings[i];
        if (m != this && page < m->m_page_count) {
            m->mark_outdated(page);
            // if (m->m_up_to_date_pages[page])
            //    memcpy(m->page_addr(page), page_addr(page), m_page_size);
        }
    }

    m_dirty_pages[page] = true;
}

void EncryptedFileMapping::validate_page(size_t page) noexcept
{
	/*

#ifdef REALM_DEBUG
    if (!m_up_to_date_pages[page])
        return;

    if (!m_file.cryptor.read(m_file.fd, page << m_page_shift, m_validate_buffer.get(), 1 << m_page_shift))
        return;

    for (size_t i = 0; i < m_file.mappings.size(); ++i) {
        EncryptedFileMapping* m = m_file.mappings[i];
        if (m != this && page < m->m_page_count && m->m_dirty_pages[page]) {
            memcpy(m_validate_buffer.get(), m->page_addr(page), 1 << m_page_shift);
            break;
        }
    }

    if (memcmp(m_validate_buffer.get(), page_addr(page), 1 << m_page_shift)) {
        std::cerr << "mismatch " << this << ": fd(" << m_file.fd << ") page(" << page << "/" << m_page_count << ") "
                  << m_validate_buffer.get() << " " << page_addr(page) << std::endl;
        REALM_TERMINATE("");
    }
#else
    static_cast<void>(page);
#endif
*/
}

void EncryptedFileMapping::validate() noexcept
{
#ifdef REALM_DEBUG
    for (size_t i = 0; i < m_page_count; ++i)
        validate_page(i);
#endif
}

void EncryptedFileMapping::flush() noexcept
{
    for (size_t i = 0; i < m_page_count; ++i) {
        if (!m_dirty_pages[i]) {
            validate_page(i);
            continue;
        }

        m_file.cryptor.write(m_file.fd, i << m_page_shift, page_addr(i), 1 << m_page_shift);
        m_dirty_pages[i] = false;
    }

    validate();
}

void EncryptedFileMapping::sync() noexcept
{
#ifdef _WIN32
#else
	fsync(m_file.fd);
    // FIXME: on iOS/OSX fsync may not be enough to ensure crash safety.
    // Consider adding fcntl(F_FULLFSYNC). This most likely also applies to msync.
    //
    // See description of fsync on iOS here:
    // https://developer.apple.com/library/ios/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html
    //
    // See also
    // https://developer.apple.com/library/ios/documentation/Cocoa/Conceptual/CoreData/Articles/cdPersistentStores.html
    // for a discussion of this related to core data.
#endif
}


void EncryptedFileMapping::write_barrier(const void* addr, size_t size) noexcept
{
    REALM_ASSERT(m_access == File::access_ReadWrite);

    size_t first_accessed_page = reinterpret_cast<uintptr_t>(addr) >> m_page_shift;
    size_t last_accessed_page = (reinterpret_cast<uintptr_t>(addr) + size - 1) >> m_page_shift;

    size_t first_idx = first_accessed_page - m_first_page;
    size_t last_idx = last_accessed_page - m_first_page;

    for (size_t idx = first_idx; idx <= last_idx; ++idx) {
        // Pages written must earlier on have been decrypted
        // by a call to read_barrier().
        REALM_ASSERT(m_up_to_date_pages[idx]);
        write_page(idx);
    }
}

void EncryptedFileMapping::set(void* new_addr, size_t new_size, size_t new_file_offset)
{
    REALM_ASSERT(new_file_offset % (1 << m_page_shift) == 0);
    REALM_ASSERT(new_size % (1 << m_page_shift) == 0);
    REALM_ASSERT(new_size > 0);

    m_file.cryptor.set_file_size(new_size + new_file_offset);

    flush();
    m_addr = new_addr;
    m_file_offset = new_file_offset;

    m_first_page = (reinterpret_cast<uintptr_t>(m_addr) - m_file_offset) >> m_page_shift;
    m_page_count = (new_size + m_file_offset) >> m_page_shift;

    m_up_to_date_pages.clear();
    m_dirty_pages.clear();

    m_up_to_date_pages.resize(m_page_count, false);
    m_dirty_pages.resize(m_page_count, false);
}

File::SizeType encrypted_size_to_data_size(File::SizeType size) noexcept
{
    if (size == 0)
        return 0;
    return fake_offset(size);
}

File::SizeType data_size_to_encrypted_size(File::SizeType size) noexcept
{
    size_t ps = page_size();
    return real_offset((size + ps - 1) & ~(ps - 1));
}

#else

namespace realm {
namespace util {

File::SizeType encrypted_size_to_data_size(File::SizeType size) noexcept
{
    return size;
}

File::SizeType data_size_to_encrypted_size(File::SizeType size) noexcept
{
    return size;
}

#endif // REALM_ENABLE_ENCRYPTION

} // namespace util {
} // namespace realm {
