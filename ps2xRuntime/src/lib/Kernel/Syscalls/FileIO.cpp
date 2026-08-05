#include "Common.h"
#include "FileIO.h"

namespace ps2_syscalls
{
    static int allocatePs2Fd(FILE *file)
    {
        if (!file)
            return -1;

        std::lock_guard<std::mutex> lock(g_fd_mutex);
        int fd = g_nextFd++;
        g_fileDescriptors[fd] = file;
        return fd;
    }

    static FILE *getHostFile(int ps2Fd)
    {
        std::lock_guard<std::mutex> lock(g_fd_mutex);
        auto it = g_fileDescriptors.find(ps2Fd);
        if (it != g_fileDescriptors.end())
        {
            return it->second;
        }
        return nullptr;
    }

    static void releasePs2Fd(int ps2Fd)
    {
        std::lock_guard<std::mutex> lock(g_fd_mutex);
        g_fileDescriptors.erase(ps2Fd);
    }

    struct VagAccumEntry
    {
        std::vector<uint8_t> data;
        uint32_t firstBufAddr = 0;
    };
    static std::unordered_map<int, VagAccumEntry> g_vagAccum;
    static std::mutex g_vagAccumMutex;
    static constexpr size_t kVagAccumMaxBytes = 16 * 1024 * 1024;

    static const char *translateFioMode(int ps2Flags)
    {
        // O_RDWR is O_RDONLY|O_WRONLY, so each direction is a single bit test.
        // Masking against O_RDWR would make a plain O_RDONLY look writable and
        // open read-only game data as "r+b".
        bool read = (ps2Flags & PS2_FIO_O_RDONLY) != 0;
        bool write = (ps2Flags & PS2_FIO_O_WRONLY) != 0;
        bool append = (ps2Flags & PS2_FIO_O_APPEND);
        bool create = (ps2Flags & PS2_FIO_O_CREAT);
        bool truncate = (ps2Flags & PS2_FIO_O_TRUNC);

        if (read && write)
        {
            if (create && truncate)
                return "w+b";
            if (create)
                return "a+b";
            return "r+b";
        }
        else if (write)
        {
            if (append)
                return "ab";
            if (create && truncate)
                return "wb";
            if (create)
                return "wx";
            return "r+b";
        }
        else if (read)
        {
            return "rb";
        }
        return "rb";
    }

    void fioOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t pathAddr = getRegU32(ctx, 4); // $a0
        int flags = (int)getRegU32(ctx, 5);    // $a1 (PS2 FIO flags)

        const char *ps2Path = reinterpret_cast<const char *>(getConstMemPtr(rdram, pathAddr));
        if (!ps2Path)
        {
            std::cerr << "fioOpen error: Invalid path address" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        std::string hostPath = translatePs2Path(ps2Path);
        if (hostPath.empty())
        {
            std::cerr << "fioOpen error: Failed to translate path '" << ps2Path << "'" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        const char *mode = translateFioMode(flags);
        RUNTIME_LOG("fioOpen: '" << hostPath << "' flags=0x" << std::hex << flags << std::dec << " mode='" << mode << "'");

        FILE *fp = ::fopen(hostPath.c_str(), mode);
        if (!fp)
        {
            std::cerr << "fioOpen error: fopen failed for '" << hostPath << "': " << strerror(errno) << std::endl;
            setReturnS32(ctx, -1); // e.g., -ENOENT, -EACCES
            return;
        }

        int ps2Fd = allocatePs2Fd(fp);
        if (ps2Fd < 0)
        {
            std::cerr << "fioOpen error: Failed to allocate PS2 file descriptor" << std::endl;
            ::fclose(fp);
            setReturnS32(ctx, -1); // e.g., -EMFILE
            return;
        }

        // returns the PS2 file descriptor
        setReturnS32(ctx, ps2Fd);
    }

    void fioClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int ps2Fd = (int)getRegU32(ctx, 4);

        FILE *fp = getHostFile(ps2Fd);
        if (!fp)
        {
            std::cerr << "fioClose warning: Invalid PS2 file descriptor " << ps2Fd << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        int ret = ::fclose(fp);
        releasePs2Fd(ps2Fd);

        {
            std::lock_guard<std::mutex> lock(g_vagAccumMutex);
            auto it = g_vagAccum.find(ps2Fd);
            if (it != g_vagAccum.end())
            {
                VagAccumEntry &e = it->second;
                if (e.data.size() >= 48)
                {
                    const uint32_t magic = (static_cast<uint32_t>(e.data[0]) << 24) |
                                           (static_cast<uint32_t>(e.data[1]) << 16) |
                                           (static_cast<uint32_t>(e.data[2]) << 8) |
                                           static_cast<uint32_t>(e.data[3]);
                    const uint32_t magicLE = (static_cast<uint32_t>(e.data[3]) << 24) |
                                             (static_cast<uint32_t>(e.data[2]) << 16) |
                                             (static_cast<uint32_t>(e.data[1]) << 8) |
                                             static_cast<uint32_t>(e.data[0]);
                    if (magic == 0x56414770u || magicLE == 0x56414770u)
                    {
                        if (runtime)
                            runtime->audioBackend().onVagTransferFromBuffer(
                                e.data.data(), static_cast<uint32_t>(e.data.size()),
                                e.firstBufAddr ? e.firstBufAddr : 0u);
                    }
                }
                g_vagAccum.erase(it);
            }
        }

        setReturnS32(ctx, ret == 0 ? 0 : -1);
    }

    void fioRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int ps2Fd = (int)getRegU32(ctx, 4);   // $a0
        uint32_t bufAddr = getRegU32(ctx, 5); // $a1
        size_t size = getRegU32(ctx, 6);      // $a2

        uint8_t *hostBuf = getMemPtr(rdram, bufAddr);
        FILE *fp = getHostFile(ps2Fd);

        if (!hostBuf)
        {
            std::cerr << "fioRead error: Invalid buffer address for fd " << ps2Fd << std::endl;
            setReturnS32(ctx, -1); // -EFAULT
            return;
        }
        if (!fp)
        {
            std::cerr << "fioRead error: Invalid file descriptor " << ps2Fd << std::endl;
            setReturnS32(ctx, -1); // -EBADF
            return;
        }
        if (size == 0)
        {
            setReturnS32(ctx, 0); // Read 0 bytes
            return;
        }

        size_t bytesRead = 0;
        {
            std::lock_guard<std::mutex> lock(g_sys_fd_mutex);
            bytesRead = fread(hostBuf, 1, size, fp);
        }
        if (bytesRead > 0)
        {
            ps2TraceGuestRangeWrite(rdram, bufAddr, static_cast<uint32_t>(bytesRead), "fioRead", ctx);
        }

        if (bytesRead < size && ferror(fp))
        {
            std::cerr << "fioRead error: fread failed for fd " << ps2Fd << ": " << strerror(errno) << std::endl;
            clearerr(fp);
            setReturnS32(ctx, -1);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_vagAccumMutex);
            auto it = g_vagAccum.find(ps2Fd);
            if (it != g_vagAccum.end())
            {
                VagAccumEntry &e = it->second;
                if (e.data.size() + bytesRead <= kVagAccumMaxBytes)
                    e.data.insert(e.data.end(), hostBuf, hostBuf + bytesRead);
            }
            else if (bytesRead >= 4)
            {
                const uint32_t magic = (static_cast<uint32_t>(hostBuf[0]) << 24) |
                                       (static_cast<uint32_t>(hostBuf[1]) << 16) |
                                       (static_cast<uint32_t>(hostBuf[2]) << 8) |
                                       static_cast<uint32_t>(hostBuf[3]);
                const uint32_t magicLE = (static_cast<uint32_t>(hostBuf[3]) << 24) |
                                         (static_cast<uint32_t>(hostBuf[2]) << 16) |
                                         (static_cast<uint32_t>(hostBuf[1]) << 8) |
                                         static_cast<uint32_t>(hostBuf[0]);
                if (magic == 0x56414770u || magicLE == 0x56414770u)
                {
                    VagAccumEntry &e = g_vagAccum[ps2Fd];
                    e.firstBufAddr = bufAddr;
                    if (bytesRead <= kVagAccumMaxBytes)
                        e.data.assign(hostBuf, hostBuf + bytesRead);
                }
            }
        }

        setReturnS32(ctx, (int32_t)bytesRead);
    }

    void fioWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int ps2Fd = (int)getRegU32(ctx, 4);   // $a0
        uint32_t bufAddr = getRegU32(ctx, 5); // $a1
        size_t size = getRegU32(ctx, 6);      // $a2

        const uint8_t *hostBuf = getConstMemPtr(rdram, bufAddr);
        if (!hostBuf)
        {
            setReturnS32(ctx, -1);
            return;
        }

        FILE *fp = getHostFile(ps2Fd);
        if (!fp)
        {
            setReturnS32(ctx, -1); // -EFAULT
            return;
        }

        if (size == 0)
        {
            setReturnS32(ctx, 0); // Wrote 0 bytes
            return;
        }

        size_t bytesWritten = 0;
        {
            std::lock_guard<std::mutex> lock(g_sys_fd_mutex);
            bytesWritten = ::fwrite(hostBuf, 1, size, fp);
            if (bytesWritten < size && ferror(fp))
            {
                clearerr(fp);
                setReturnS32(ctx, -1); // -EIO, -ENOSPC etc.
                return;
            }
        }

        // returns number of bytes written
        setReturnS32(ctx, (int32_t)bytesWritten);
    }

    void fioLseek(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int ps2Fd = (int)getRegU32(ctx, 4);  // $a0
        int32_t offset = getRegU32(ctx, 5);  // $a1 (PS2 seems to use 32-bit offset here commonly)
        int whence = (int)getRegU32(ctx, 6); // $a2 (PS2 FIO_SEEK constants)

        FILE *fp = getHostFile(ps2Fd);
        if (!fp)
        {
            std::cerr << "fioLseek error: Invalid file descriptor " << ps2Fd << std::endl;
            setReturnS32(ctx, -1); // -EBADF
            return;
        }

        int hostWhence;
        switch (whence)
        {
        case PS2_FIO_SEEK_SET:
            hostWhence = SEEK_SET;
            break;
        case PS2_FIO_SEEK_CUR:
            hostWhence = SEEK_CUR;
            break;
        case PS2_FIO_SEEK_END:
            hostWhence = SEEK_END;
            break;
        default:
            std::cerr << "fioLseek error: Invalid whence value " << whence << " for fd " << ps2Fd << std::endl;
            setReturnS32(ctx, -1); // -EINVAL
            return;
        }

        if (::fseek(fp, static_cast<long>(offset), hostWhence) != 0)
        {
            std::cerr << "fioLseek error: fseek failed for fd " << ps2Fd << ": " << strerror(errno) << std::endl;
            setReturnS32(ctx, -1); // Return error code
            return;
        }

        long newPos = ::ftell(fp);
        if (newPos < 0)
        {
            std::cerr << "fioLseek error: ftell failed after fseek for fd " << ps2Fd << ": " << strerror(errno) << std::endl;
            setReturnS32(ctx, -1);
        }
        else
        {
            if (newPos > 0xFFFFFFFFL)
            {
                std::cerr << "fioLseek warning: New position exceeds 32-bit for fd " << ps2Fd << std::endl;
                setReturnS32(ctx, -1);
            }
            else
            {
                setReturnS32(ctx, (int32_t)newPos);
            }
        }
    }

    void fioMkdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t pathAddr = getRegU32(ctx, 4); // $a0
        // int mode = (int)getRegU32(ctx, 5);  // $a1 - ignored on host

        const char *ps2Path = reinterpret_cast<const char *>(getConstMemPtr(rdram, pathAddr));
        if (!ps2Path)
        {
            std::cerr << "fioMkdir error: Invalid path address" << std::endl;
            setReturnS32(ctx, -1); // -EFAULT
            return;
        }
        std::string hostPath = translatePs2Path(ps2Path);
        if (hostPath.empty())
        {
            std::cerr << "fioMkdir error: Failed to translate path '" << ps2Path << "'" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }
        std::error_code ec;
        bool success = std::filesystem::create_directory(hostPath, ec);

        if (!success && ec)
        {
            std::cerr << "fioMkdir error: create_directory failed for '" << hostPath
                      << "': " << ec.message() << std::endl;
            setReturnS32(ctx, -1);
        }
        else
        {
            RUNTIME_LOG("fioMkdir: Created directory '" << hostPath << "'");
            setReturnS32(ctx, 0); // Success
        }
    }

    void fioChdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t pathAddr = getRegU32(ctx, 4); // $a0
        const char *ps2Path = reinterpret_cast<const char *>(getConstMemPtr(rdram, pathAddr));
        if (!ps2Path)
        {
            std::cerr << "fioChdir error: Invalid path address" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        std::string hostPath = translatePs2Path(ps2Path);
        if (hostPath.empty())
        {
            std::cerr << "fioChdir error: Failed to translate path '" << ps2Path << "'" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        std::error_code ec;
        std::filesystem::current_path(hostPath, ec);

        if (ec)
        {
            std::cerr << "fioChdir error: current_path failed for '" << hostPath
                      << "': " << ec.message() << std::endl;
            setReturnS32(ctx, -1);
        }
        else
        {
            RUNTIME_LOG("fioChdir: Changed directory to '" << hostPath << "'");
            setReturnS32(ctx, 0); // Success
        }
    }

    void fioRmdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t pathAddr = getRegU32(ctx, 4); // $a0
        const char *ps2Path = reinterpret_cast<const char *>(getConstMemPtr(rdram, pathAddr));
        if (!ps2Path)
        {
            std::cerr << "fioRmdir error: Invalid path address" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }
        std::string hostPath = translatePs2Path(ps2Path);
        if (hostPath.empty())
        {
            std::cerr << "fioRmdir error: Failed to translate path '" << ps2Path << "'" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        std::error_code ec;
        bool success = std::filesystem::remove(hostPath, ec);

        if (!success || ec)
        {
            std::cerr << "fioRmdir error: remove failed for '" << hostPath
                      << "': " << ec.message() << std::endl;
            setReturnS32(ctx, -1);
        }
        else
        {
            RUNTIME_LOG("fioRmdir: Removed directory '" << hostPath << "'");
            setReturnS32(ctx, 0); // Success
        }
    }

    void fioGetstat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // we wont implement this for now.
        uint32_t pathAddr = getRegU32(ctx, 4);    // $a0
        uint32_t statBufAddr = getRegU32(ctx, 5); // $a1

        const char *ps2Path = reinterpret_cast<const char *>(getConstMemPtr(rdram, pathAddr));
        uint8_t *ps2StatBuf = getMemPtr(rdram, statBufAddr);

        if (!ps2Path)
        {
            std::cerr << "fioGetstat error: Invalid path addr" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }
        if (!ps2StatBuf)
        {
            std::cerr << "fioGetstat error: Invalid buffer addr" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        std::string hostPath = translatePs2Path(ps2Path);
        if (hostPath.empty())
        {
            std::cerr << "fioGetstat error: Bad path translate" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        setReturnS32(ctx, -1);
    }

    void fioRemove(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t pathAddr = getRegU32(ctx, 4); // $a0
        const char *ps2Path = reinterpret_cast<const char *>(getConstMemPtr(rdram, pathAddr));
        if (!ps2Path)
        {
            std::cerr << "fioRemove error: Invalid path" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        std::string hostPath = translatePs2Path(ps2Path);
        if (hostPath.empty())
        {
            std::cerr << "fioRemove error: Path translate fail" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        std::error_code ec;
        bool success = std::filesystem::remove(hostPath, ec);

        if (!success || ec)
        {
            std::cerr << "fioRemove error: remove failed for '" << hostPath
                      << "': " << ec.message() << std::endl;
            setReturnS32(ctx, -1);
        }
        else
        {
            RUNTIME_LOG("fioRemove: Removed file '" << hostPath << "'");
            setReturnS32(ctx, 0); // Success
        }
    }

    // Directory enumeration.
    //
    // The listing is snapshotted at open time so a guest that keeps a dirent
    // handle open across frames sees a stable sequence, and so host directory
    // changes cannot invalidate an iterator we are holding.
    namespace
    {
        struct Ps2DirStream
        {
            std::vector<std::filesystem::directory_entry> entries;
            std::size_t next = 0;
        };

        std::mutex g_dirMutex;
        std::unordered_map<int, Ps2DirStream> g_dirStreams;
        int g_nextDirFd = 1;

        // sce_dirent is a 64-byte sce_stat (the 40 documented bytes plus six
        // words of private data) followed by char d_name[256] and a private
        // pointer. Guests read the type bits from d_stat.st_mode and the name
        // at +0x40.
        constexpr uint32_t kDirentNameOffset = 0x40u;
        constexpr uint32_t kDirentSize = 0x144u;
        constexpr uint32_t kDirentNameCapacity = 256u;
        constexpr uint32_t kFioSIFDIR = 0x1000u;
        constexpr uint32_t kFioSIFREG = 0x2000u;

        void storeDirentU32(uint8_t *base, uint32_t offset, uint32_t value)
        {
            std::memcpy(base + offset, &value, sizeof(value));
        }
    }

    void fioDopen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t pathAddr = getRegU32(ctx, 4); // $a0

        const char *ps2Path = reinterpret_cast<const char *>(getConstMemPtr(rdram, pathAddr));
        if (!ps2Path)
        {
            std::cerr << "fioDopen error: Invalid path address" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        std::string hostPath = translatePs2Path(ps2Path);
        if (hostPath.empty())
        {
            std::cerr << "fioDopen error: Failed to translate path '" << ps2Path << "'" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        std::error_code ec;
        std::filesystem::directory_iterator it(hostPath, ec);
        if (ec)
        {
            std::cerr << "fioDopen error: cannot open directory '" << hostPath
                      << "': " << ec.message() << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        // "." and ".." are deliberately not reported: the disc filesystem does
        // not list them either, and guests that walk a tree would recurse.
        Ps2DirStream stream;
        for (const auto &entry : it)
        {
            stream.entries.push_back(entry);
        }

        const std::size_t entryCount = stream.entries.size();
        int dirFd = 0;
        {
            std::lock_guard<std::mutex> lock(g_dirMutex);
            dirFd = g_nextDirFd++;
            g_dirStreams[dirFd] = std::move(stream);
        }

        RUNTIME_LOG("fioDopen: '" << hostPath << "' fd=" << dirFd
                                  << " entries=" << entryCount);
        setReturnS32(ctx, dirFd);
    }

    void fioDread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int dirFd = (int)getRegU32(ctx, 4);   // $a0
        uint32_t bufAddr = getRegU32(ctx, 5); // $a1

        uint8_t *dirent = getMemPtr(rdram, bufAddr);
        if (!dirent)
        {
            std::cerr << "fioDread error: Invalid dirent address 0x" << std::hex << bufAddr
                      << std::dec << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        std::lock_guard<std::mutex> lock(g_dirMutex);
        auto streamIt = g_dirStreams.find(dirFd);
        if (streamIt == g_dirStreams.end())
        {
            std::cerr << "fioDread error: Invalid directory descriptor " << dirFd << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        Ps2DirStream &stream = streamIt->second;
        if (stream.next >= stream.entries.size())
        {
            setReturnS32(ctx, 0); // end of directory
            return;
        }

        const std::filesystem::directory_entry &entry = stream.entries[stream.next++];

        std::error_code ec;
        const bool isDirectory = entry.is_directory(ec);
        uint64_t fileSize = 0;
        if (!ec && !isDirectory)
        {
            fileSize = static_cast<uint64_t>(entry.file_size(ec));
            if (ec)
            {
                fileSize = 0;
            }
        }

        std::memset(dirent, 0, kDirentSize);
        storeDirentU32(dirent, 0, (isDirectory ? kFioSIFDIR : kFioSIFREG) | 0x1FFu); // st_mode
        storeDirentU32(dirent, 8, static_cast<uint32_t>(fileSize));                  // st_size
        storeDirentU32(dirent, 36, static_cast<uint32_t>(fileSize >> 32));           // st_hisize

        const std::string name = entry.path().filename().string();
        const std::size_t nameLength = std::min<std::size_t>(name.size(), kDirentNameCapacity - 1);
        std::memcpy(dirent + kDirentNameOffset, name.data(), nameLength);

        setReturnS32(ctx, 1); // an entry was produced
    }

    void fioDclose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int dirFd = (int)getRegU32(ctx, 4); // $a0

        std::lock_guard<std::mutex> lock(g_dirMutex);
        if (g_dirStreams.erase(dirFd) == 0)
        {
            std::cerr << "fioDclose warning: Invalid directory descriptor " << dirFd << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        setReturnS32(ctx, 0);
    }
}
