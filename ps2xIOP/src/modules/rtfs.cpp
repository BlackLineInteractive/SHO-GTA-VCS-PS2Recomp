// RTFS — the RenderWare Studio / Climax file system served by RTFSSIOP.IRX.
//
// Unlike CLFILE (Lord of the Rings) this module is sector oriented and its reads
// are asynchronous: the read RPC carries no receive buffer, and the IOP reports
// completion through a SIF command that runs an EE handler. The command id, the
// handler and its argument all come from the guest's own sceSifAddCmdHandler
// registration, so nothing here is hard-coded to one game build.
//
// Wire format, recovered from the EE client (TkSkyIOPFSystem*):
//   fn open  : send = NUL-terminated name      -> recv[0] = handle, recv[4] = size
//   fn seek  : send[0] = handle, send[4] = position in sectors
//   fn read  : send[0] = handle, send[4] = sectors, send[8] = EE destination
//   fn close : send[0] = handle
// Handles are ours to choose; the EE treats them as opaque tokens.

#include "module_factories.h"

#include <algorithm>
#include <cstdio>
#include <array>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        std::string toHex(uint32_t value)
        {
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "%08x", value);
            return buffer;
        }

        class RtfsService final : public IopService
        {
        public:
            RtfsService(IopHost &host, RtfsBindings bindings)
                : m_host(host), m_bindings(std::move(bindings)), m_sids{m_bindings.sid}
            {
            }

            [[nodiscard]] std::string_view name() const override
            {
                return m_bindings.serviceName;
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return m_sids;
            }

            void reset() override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (auto &entry : m_files)
                {
                    if (entry.second.file != 0u)
                    {
                        m_host.closeHostFile(entry.second.file);
                    }
                }
                m_files.clear();
                m_nextHandle = m_bindings.firstHandle;
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                if (request.sid != m_bindings.sid)
                {
                    return {};
                }

                m_host.log(LogLevel::Info,
                           "RTFS rpc fn=" + std::to_string(request.function) +
                               " send=0x" + toHex(request.send.address) + "/" +
                               std::to_string(request.send.size) +
                               " recv=0x" + toHex(request.receive.address) + "/" +
                               std::to_string(request.receive.size));

                RpcResult result;
                result.handled = true;
                result.resultAddress = request.receive.address;
                // The generic unhandled-RPC path used to signal the client's
                // completion semaphore. Claiming the sid without doing the same
                // leaves callers of the reply-less commands waiting forever.
                result.signalCompletion = true;

                const RtfsRpcLayout &rpc = m_bindings.rpc;
                if (request.function == rpc.openFunction)
                {
                    handleOpen(request);
                }
                else if (request.function == rpc.readFunction)
                {
                    handleRead(request);
                }
                else if (request.function == rpc.seekFunction)
                {
                    handleSeek(request);
                }
                else if (request.function == rpc.closeFunction)
                {
                    handleClose(request);
                }
                else if (request.function == rpc.initFunction)
                {
                    // RtSkyIOPFSystemInit treats a zero first word as "no file
                    // system" and tears the object down (0x274AFC), so the reply
                    // has to carry a non-zero handle for the mounted volume.
                    clearReceive(request);
                    if (request.receive.address != 0u)
                    {
                        const uint32_t mounted = m_bindings.rpc.initSuccessValue;
                        (void)m_host.writeGuest(request.receive.address, &mounted, sizeof(mounted));
                    }
                }
                else
                {
                    // prepare / configure / filesystem-close: nothing to model,
                    // but the EE still reads the reply buffer on some of them.
                    clearReceive(request);
                }

                return result;
            }

        private:
            struct OpenFile
            {
                uint64_t file = 0u;
                uint64_t position = 0u;
                uint64_t size = 0u;
            };

            void clearReceive(const RpcRequest &request)
            {
                if (request.receive.address != 0u && request.receive.size != 0u)
                {
                    (void)m_host.zeroGuest(request.receive.address, request.receive.size);
                }
            }

            void writeOpenReply(const RpcRequest &request, int32_t handle, uint32_t size)
            {
                clearReceive(request);
                if (request.receive.address == 0u)
                {
                    return;
                }
                const uint32_t status = static_cast<uint32_t>(handle);
                (void)m_host.writeGuest(request.receive.address + m_bindings.rpc.replyHandleOffset,
                                        &status, sizeof(status));
                (void)m_host.writeGuest(request.receive.address + m_bindings.rpc.replySizeOffset,
                                        &size, sizeof(size));
            }

            bool readSendWord(const RpcRequest &request, uint32_t offset, uint32_t &value)
            {
                return m_host.readGuest(request.send.address + offset, &value, sizeof(value));
            }

            void handleOpen(const RpcRequest &request)
            {
                const uint32_t nameBytes = request.send.size != 0u ? request.send.size
                                                                   : m_bindings.rpc.pathBytes;
                std::vector<char> raw(nameBytes + 1u, '\0');
                if (!m_host.readGuest(request.send.address, raw.data(), nameBytes))
                {
                    writeOpenReply(request, -1, 0u);
                    return;
                }
                const std::string guestName(raw.data());
                const std::string hostPath = m_host.translateGuestPath(guestName);
                if (hostPath.empty())
                {
                    m_host.log(LogLevel::Warning, "RTFS open: cannot resolve " + guestName);
                    writeOpenReply(request, -1, 0u);
                    return;
                }

                const uint64_t file = m_host.openHostFile(hostPath);
                uint64_t size = 0u;
                if (file == 0u || !m_host.hostFileSize(file, size))
                {
                    if (file != 0u)
                    {
                        m_host.closeHostFile(file);
                    }
                    m_host.log(LogLevel::Warning, "RTFS open failed: " + guestName);
                    writeOpenReply(request, -1, 0u);
                    return;
                }

                int32_t handle = 0;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    handle = static_cast<int32_t>(m_nextHandle++);
                    m_files[static_cast<uint32_t>(handle)] = OpenFile{file, 0u, size};
                }
                m_host.log(LogLevel::Info,
                           "RTFS open " + guestName + " -> handle " + std::to_string(handle) +
                               " size " + std::to_string(size));
                writeOpenReply(request, handle,
                               static_cast<uint32_t>(std::min<uint64_t>(size, 0xFFFFFFFFull)));
            }

            void handleSeek(const RpcRequest &request)
            {
                uint32_t handle = 0u;
                uint32_t sectors = 0u;
                if (!readSendWord(request, 0u, handle) || !readSendWord(request, 4u, sectors))
                {
                    return;
                }
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto it = m_files.find(handle);
                if (it != m_files.end())
                {
                    it->second.position = static_cast<uint64_t>(sectors) * m_bindings.rpc.sectorBytes;
                }
            }

            void handleClose(const RpcRequest &request)
            {
                uint32_t handle = 0u;
                if (!readSendWord(request, 0u, handle))
                {
                    return;
                }
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto it = m_files.find(handle);
                if (it == m_files.end())
                {
                    return;
                }
                if (it->second.file != 0u)
                {
                    m_host.closeHostFile(it->second.file);
                }
                m_files.erase(it);
            }

            void handleRead(const RpcRequest &request)
            {
                uint32_t handle = 0u;
                uint32_t sectors = 0u;
                uint32_t destination = 0u;
                if (!readSendWord(request, 0u, handle) ||
                    !readSendWord(request, 4u, sectors) ||
                    !readSendWord(request, 8u, destination) ||
                    destination == 0u)
                {
                    return;
                }

                uint64_t file = 0u;
                uint64_t position = 0u;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const auto it = m_files.find(handle);
                    if (it == m_files.end())
                    {
                        return;
                    }
                    file = it->second.file;
                    position = it->second.position;
                }

                const uint64_t requested =
                    static_cast<uint64_t>(sectors) * m_bindings.rpc.sectorBytes;
                const size_t wanted = static_cast<size_t>(
                    std::min<uint64_t>(requested, m_bindings.rpc.maximumReadBytes));
                std::vector<uint8_t> bytes(wanted, 0u);
                size_t got = 0u;
                if (wanted != 0u &&
                    !m_host.readHostFile(file, position, bytes.data(), bytes.size(), got))
                {
                    got = 0u;
                }
                if (got != 0u)
                {
                    (void)m_host.writeGuest(destination, bytes.data(), got);
                }

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const auto it = m_files.find(handle);
                    if (it != m_files.end())
                    {
                        it->second.position = position + got;
                    }
                }

                m_host.log(LogLevel::Info,
                           "RTFS read handle " + std::to_string(handle) + " sectors " +
                               std::to_string(sectors) + " dest 0x" + toHex(destination) +
                               " got " + std::to_string(got));
                signalReadComplete(request, handle);
            }

            // The EE handler locates its file entry from a byte in the command
            // packet, which indexes the guest's own table — not our handle. The
            // table hangs off the handler argument, so walk it and match on the
            // handle the guest stored when the file was opened.
            bool findGuestFileIndex(uint32_t handlerArgument, uint32_t handle, uint32_t &index)
            {
                uint32_t tableAddress = 0u;
                if (!m_host.readGuest(handlerArgument + m_bindings.rpc.fileTablePointerOffset,
                                      &tableAddress, sizeof(tableAddress)) ||
                    tableAddress == 0u)
                {
                    return false;
                }
                for (uint32_t i = 0; i < m_bindings.rpc.fileTableEntries; ++i)
                {
                    uint32_t stored = 0u;
                    const uint32_t entry = tableAddress + i * m_bindings.rpc.fileTableStride;
                    if (!m_host.readGuest(entry + m_bindings.rpc.fileHandleOffset,
                                          &stored, sizeof(stored)))
                    {
                        return false;
                    }
                    if (stored == handle)
                    {
                        index = i;
                        return true;
                    }
                }
                return false;
            }

            void signalReadComplete(const RpcRequest &request, uint32_t handle)
            {
                uint32_t function = 0u;
                uint32_t argument = 0u;
                if (!m_host.sifCommandHandler(m_bindings.rpc.readCompleteCommandId,
                                              function, argument))
                {
                    m_host.log(LogLevel::Warning, "RTFS: no EE handler for read completion");
                    return;
                }

                uint32_t index = 0u;
                if (!findGuestFileIndex(argument, handle, index))
                {
                    m_host.log(LogLevel::Warning,
                               "RTFS: no guest file entry for handle " + std::to_string(handle));
                    return;
                }
                m_host.log(LogLevel::Info,
                           "RTFS complete handle " + std::to_string(handle) + " index " +
                               std::to_string(index) + " -> 0x" + toHex(function));

                if (m_packet == 0u)
                {
                    m_packet = m_host.allocateGuest(m_bindings.rpc.packetBytes, 16u);
                    if (m_packet == 0u)
                    {
                        return;
                    }
                }
                (void)m_host.zeroGuest(m_packet, m_bindings.rpc.packetBytes);
                const uint8_t indexByte = static_cast<uint8_t>(index);
                (void)m_host.writeGuest(m_packet + m_bindings.rpc.packetIndexOffset,
                                        &indexByte, sizeof(indexByte));

                (void)m_host.invokeGuestFunction(request.callToken, function,
                                                 m_packet, argument, 0u, 0u, nullptr);
            }

            IopHost &m_host;
            RtfsBindings m_bindings;
            std::array<uint32_t, 1> m_sids;
            std::mutex m_mutex;
            std::unordered_map<uint32_t, OpenFile> m_files;
            uint32_t m_nextHandle = 0u;
            uint32_t m_packet = 0u;
        };
    }

    std::unique_ptr<IopService> createRtfsService(IopHost &host, RtfsBindings bindings)
    {
        if (bindings.serviceName.empty() || bindings.sid == 0u ||
            bindings.rpc.sectorBytes == 0u || bindings.rpc.maximumReadBytes == 0u ||
            bindings.rpc.fileTableStride == 0u || bindings.firstHandle == 0u)
        {
            throw std::invalid_argument("invalid RTFS bindings");
        }
        return std::make_unique<RtfsService>(host, std::move(bindings));
    }
}
