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

#include <realm/util/features.h>

#include "file_mapper.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <realm/util/errno.hpp>
#include <realm/util/to_string.hpp>
#include <realm/exceptions.hpp>

#if REALM_ENABLE_ENCRYPTION

#include "encrypted_file_mapping.hpp"
#include "aes_cryptor.hpp"

#include <memory>
#include <csignal>
#include <sys/stat.h>
#include <cstring>
#include <atomic>

#include <realm/util/file.hpp>
#include <realm/util/errno.hpp>
#include <realm/util/shared_ptr.hpp>
#include <realm/util/terminate.hpp>
#include <realm/util/thread.hpp>
#include <cstring> // for memset

#if REALM_PLATFORM_APPLE
#include <mach/mach.h>
#include <mach/exc.h>
#endif

#if REALM_ANDROID
#include <linux/unistd.h>
#include <sys/syscall.h>
#endif

#endif // enable encryption

namespace {

inline bool is_mmap_memory_error(int err)
{
    return (err == EAGAIN || err == EMFILE || err == ENOMEM);
}

} // Unnamed namespace

using namespace realm;
using namespace realm::util;

namespace realm {
namespace util {

#if REALM_ENABLE_ENCRYPTION

// A list of all of the active encrypted mappings for a single file
struct mappings_for_file {
#ifdef _WIN32
	HANDLE handle;
#else
	dev_t device;
    ino_t inode;
#endif
	SharedPtr<SharedFileInfo> info;
};

// Group the information we need to map a SIGSEGV address to an
// EncryptedFileMapping for the sake of cache-friendliness with 3+ active
// mappings (and no worse with only two
struct mapping_and_addr {
    SharedPtr<EncryptedFileMapping> mapping;
    void* addr;
    size_t size;
};

// prevent destruction at exit (which can lead to races if other threads are still running)
util::Mutex& mapping_mutex = *new Mutex;
std::vector<mapping_and_addr>& mappings_by_addr = *new std::vector<mapping_and_addr>;
std::vector<mappings_for_file>& mappings_by_file = *new std::vector<mappings_for_file>;


mapping_and_addr* find_mapping_for_addr(void* addr, size_t size)
{
    for (size_t i = 0; i < mappings_by_addr.size(); ++i) {
        mapping_and_addr& m = mappings_by_addr[i];
        if (m.addr == addr && m.size == size)
            return &m;
    }

    return 0;
}

EncryptedFileMapping* add_mapping(void* addr, size_t size, File fd, size_t file_offset, File::AccessMode access,
                                  const char* encryption_key)
{
//	struct stat st;
/*
    if (_fstat(fd, &st)) {
        int err = errno; // Eliminate any risk of clobbering
        throw std::runtime_error(get_errno_msg("fstat() failed: ", err));
    }
*/
    if (fd.get_size() > 0 && static_cast<size_t>(fd.get_size()) < page_size())
        throw DecryptionFailed();
		
    LockGuard lock(mapping_mutex);

    std::vector<mappings_for_file>::iterator it;
    for (it = mappings_by_file.begin(); it != mappings_by_file.end(); ++it) {
#ifdef _WIN32
        if (it->handle == it->handle)
			break;
#else
		if (it->inode == st.st_ino && it->device == st.st_dev)
			break;
#endif
    }

    // Get the potential memory allocation out of the way so that mappings_by_addr.push_back can't throw
    mappings_by_addr.reserve(mappings_by_addr.size() + 1);

    if (it == mappings_by_file.end()) {
        mappings_by_file.reserve(mappings_by_file.size() + 1);

      //  fd = dup(fd);
      //  if (fd == -1) {
      //      int err = errno; // Eliminate any risk of clobbering
      //      throw std::runtime_error(get_errno_msg("dup() failed: ", err));
      //  }

        mappings_for_file f;

#ifdef _WIN32
		f.handle = fd.m_handle;
#else
		f.device = st.st_dev;
        f.inode = st.st_ino;
#endif        
		
		try {
            f.info = new SharedFileInfo(reinterpret_cast<const uint8_t*>(encryption_key), fd);

        }
        catch (...) {
			fd.close(); //::close(fd);
            throw;
        }

        mappings_by_file.push_back(f); // can't throw due to reserve() above
        it = mappings_by_file.end() - 1;
    }

    try {
        mapping_and_addr m;
        m.addr = addr;
        m.size = size;
        EncryptedFileMapping* m_ptr = new EncryptedFileMapping(*it->info, file_offset, addr, size, access);
        m.mapping = m_ptr;
        mappings_by_addr.push_back(m); // can't throw due to reserve() above
        return m_ptr;
    }
    catch (...) {
        if (it->info->mappings.empty()) {
			it->info->fd.close();//  ::close(it->info->fd);
            mappings_by_file.erase(it);
        }
        throw;
    }
}

void remove_mapping(void* addr, size_t size)
{
    size = round_up_to_page_size(size);
    LockGuard lock(mapping_mutex);
    mapping_and_addr* m = find_mapping_for_addr(addr, size);
    if (!m)
        return;

    mappings_by_addr.erase(mappings_by_addr.begin() + (m - &mappings_by_addr[0]));

    for (std::vector<mappings_for_file>::iterator it = mappings_by_file.begin(); it != mappings_by_file.end(); ++it) {
        if (it->info->mappings.empty()) {
			it->info->fd.close();

			/*
            if (::close(it->info->fd) != 0) {
                int err = errno;                // Eliminate any risk of clobbering
                if (err == EBADF || err == EIO) // FIXME: how do we handle EINTR?
                    throw std::runtime_error(get_errno_msg("close() failed: ", err));
            }
			*/
            mappings_by_file.erase(it);
            break;
        }
    }
}

void* mmap_anon(size_t size)
{
#ifdef _WIN32
	TCHAR szName[] = TEXT("Global\\MyFileMappingObject");

	HANDLE hMapFile;
	LPCTSTR pBuf;

	hMapFile = CreateFileMapping(
		INVALID_HANDLE_VALUE,    // use paging file
		NULL,                    // default security
		PAGE_READWRITE,          // read/write access
		0,                       // maximum object size (high-order DWORD)
		size,                // maximum object size (low-order DWORD)
		szName);                 // name of mapping object

	pBuf = (LPTSTR)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, size);

	return (void*)pBuf;
#else
    void* addr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (addr == MAP_FAILED) {
        int err = errno; // Eliminate any risk of clobbering
        if (is_mmap_memory_error(err)) {
            throw AddressSpaceExhausted(get_errno_msg("mmap() failed: ", err) + " size: " + util::to_string(size));
        }
        throw std::runtime_error(get_errno_msg("mmap() failed: ", err) + "size: " + util::to_string(size) +
                                 "offset is 0");
    }
    return addr;
#endif
}

size_t round_up_to_page_size(size_t size) noexcept
{
    return (size + page_size() - 1) & ~(page_size() - 1);
}

void* mmap(File fd, size_t size, File::AccessMode access, size_t offset, const char* encryption_key,
           EncryptedFileMapping*& mapping)
{
    if (encryption_key) {
        size = round_up_to_page_size(size);
        void* addr = mmap_anon(size);
        mapping = add_mapping(addr, size, fd, offset, access, encryption_key);
        return addr;
    }
    else {
        mapping = nullptr;
        return mmap(fd, size, access, offset, nullptr);
    }
}

#endif // enable encryption


void* mmap(File fd, size_t size, File::AccessMode access, size_t offset, const char* encryption_key)
{
#if REALM_ENABLE_ENCRYPTION
    if (encryption_key) {
        size = round_up_to_page_size(size);
        void* addr = mmap_anon(size);
        add_mapping(addr, size, fd, offset, access, encryption_key);
        return addr;
    }
    else
#else
    REALM_ASSERT(!encryption_key);
#endif
    {

		/*
        int prot = PROT_READ;
        switch (access) {
            case File::access_ReadWrite:
                prot |= PROT_WRITE;
                break;
            case File::access_ReadOnly:
                break;
        }

        void* addr = ::mmap(nullptr, size, prot, MAP_SHARED, fd, offset);
        if (addr != MAP_FAILED)
            return addr;


*/

		void* addr = fd.map(File::access_ReadWrite, size, PAGE_READWRITE, offset);
		return addr;


    }

    int err = errno; // Eliminate any risk of clobbering
    if (is_mmap_memory_error(err)) {
        throw AddressSpaceExhausted(get_errno_msg("mmap() failed: ", err) + " size: " + util::to_string(size) +
                                    " offset: " + util::to_string(offset));
    }
    throw std::runtime_error(get_errno_msg("mmap() failed: ", err) + "size: " + util::to_string(size) + "offset: " +
                             util::to_string(offset));
}

void munmap(void* addr, size_t size) noexcept
{
#if REALM_ENABLE_ENCRYPTION
    remove_mapping(addr, size);
#endif

#ifdef _WIN32
	UnmapViewOfFile(addr);
#else
    if (::munmap(addr, size) != 0) {
        int err = errno;
        throw std::runtime_error(get_errno_msg("munmap() failed: ", err));
    }
#endif
}

void* mremap(File fd, size_t file_offset, void* old_addr, size_t old_size, File::AccessMode a, size_t new_size)
{
#if REALM_ENABLE_ENCRYPTION
    {
        LockGuard lock(mapping_mutex);
        size_t rounded_old_size = round_up_to_page_size(old_size);
        if (mapping_and_addr* m = find_mapping_for_addr(old_addr, rounded_old_size)) {
            size_t rounded_new_size = round_up_to_page_size(new_size);
            if (rounded_old_size == rounded_new_size)
                return old_addr;

            void* new_addr = mmap_anon(rounded_new_size);
            m->mapping->set(new_addr, rounded_new_size, file_offset);

#ifdef _WIN32
			UnmapViewOfFile(old_addr);
#else
			int i = ::munmap(old_addr, rounded_old_size);

			m->addr = new_addr;
            m->size = rounded_new_size;
            if (i != 0) {
                int err = errno;
                throw std::runtime_error(get_errno_msg("munmap() failed: ", err));
            }
#endif
            return new_addr;
        }
    }
#endif

#ifdef _GNU_SOURCE
    {
        void* new_addr = ::mremap(old_addr, old_size, new_size, MREMAP_MAYMOVE);
        if (new_addr != MAP_FAILED)
            return new_addr;
        int err = errno; // Eliminate any risk of clobbering
        // Do not throw here if mremap is declared as "not supported" by the
        // platform Eg. When compiling with GNU libc on OSX, iOS.
        // In this case fall through to no-mremap case below.
        if (err != ENOTSUP && err != ENOSYS) {
            if (is_mmap_memory_error(err)) {
                throw AddressSpaceExhausted(get_errno_msg("mremap() failed: ", err) + " old size: " +
                                            util::to_string(old_size) + " new size: " + util::to_string(new_size));
            }
            throw std::runtime_error(get_errno_msg("_gnu_src mmap() failed: ", err) + " old size: " +
                                     util::to_string(old_size) + " new_size: " + util::to_string(new_size));
        }
    }
#endif

    void* new_addr = mmap(fd, new_size, a, file_offset, nullptr);

#ifdef _WIN32
	UnmapViewOfFile(old_addr);
#else
    if (::munmap(old_addr, old_size) != 0) {
        int err = errno;
        throw std::runtime_error(get_errno_msg("munmap() failed: ", err));
    }
#endif
	
	return new_addr;
}

void msync(void* addr, size_t size)
{
#if REALM_ENABLE_ENCRYPTION
    {
        // first check the encrypted mappings
        LockGuard lock(mapping_mutex);
        if (mapping_and_addr* m = find_mapping_for_addr(addr, round_up_to_page_size(size))) {
            m->mapping->flush();
            m->mapping->sync();
            return;
        }
    }
#endif

    // not an encrypted mapping

    // FIXME: on iOS/OSX fsync may not be enough to ensure crash safety.
    // Consider adding fcntl(F_FULLFSYNC). This most likely also applies to msync.
    //
    // See description of fsync on iOS here:
    // https://developer.apple.com/library/ios/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html
    //
    // See also
    // https://developer.apple.com/library/ios/documentation/Cocoa/Conceptual/CoreData/Articles/cdPersistentStores.html
    // for a discussion of this related to core data.

	/*
	if (::msync(addr, size, MS_SYNC) != 0) {
        int err = errno; // Eliminate any risk of clobbering
        throw std::runtime_error(get_errno_msg("msync() failed: ", err));
    }
*/
}
}
}
