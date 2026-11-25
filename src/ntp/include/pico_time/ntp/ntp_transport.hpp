// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

/// \file ntp_transport.hpp
/// Network, timing and locking operations used by the NTP attempt loop.

namespace pico_time {

/// Capacity of the transport address representation, in bytes.
inline constexpr std::size_t NTP_ADDRESS_BYTES = 20;

/// Equal endpoints must give equal bytes on the resolve and receive paths,
/// because the attempt loop compares addresses byte for byte. The address is
/// opaque to the attempt loop.
struct NtpAddress {
    /// The address, in the transport's own representation.
    alignas(std::uint32_t) std::array<std::uint8_t, NTP_ADDRESS_BYTES> opaque{};

    /// Whether resolve() has filled this in.
    bool valid = false;
};

/// What a resolution request did.
enum class ResolveStatus : std::uint8_t {
    /// The address is available immediately. No callback will fire.
    RESOLVED,
    /// The observer will be called exactly once. The lookup cannot be
    /// cancelled.
    IN_PROGRESS,
    /// The query failed. No callback will fire.
    FAILED,
};

/// Receives resolution results in callback context, holding the transport lock.
/// An implementation records the result and returns (VR-8).
class IResolveObserver {
   public:
    IResolveObserver() = default;

    /// Non-copyable and non-movable to prevent interface slicing.
    IResolveObserver(const IResolveObserver&) = delete;
    IResolveObserver& operator=(const IResolveObserver&) = delete;
    IResolveObserver(IResolveObserver&&) = delete;
    IResolveObserver& operator=(IResolveObserver&&) = delete;

    /// \param address the resolved address, or an invalid one on failure.
    virtual void on_resolved(const NtpAddress& address) = 0;

   protected:
    // The owning attempt slot destroys its observer directly.
    ~IResolveObserver() = default;
};

/// Notified for each datagram arriving on the open endpoint.
class IDatagramObserver {
   public:
    IDatagramObserver() = default;

    /// Non-copyable and non-movable to prevent interface slicing.
    IDatagramObserver(const IDatagramObserver&) = delete;
    IDatagramObserver& operator=(const IDatagramObserver&) = delete;
    IDatagramObserver(IDatagramObserver&&) = delete;
    IDatagramObserver& operator=(IDatagramObserver&&) = delete;

    /// \param sender the address it came from, for the policy to match.
    /// \param datagram the payload; valid only for the duration of the call.
    virtual void on_datagram(const NtpAddress& sender,
                             std::span<const std::uint8_t> datagram) = 0;

   protected:
    ~IDatagramObserver() = default;
};

/// Operations implemented by the lwIP adapter and the test transport.
class INtpTransport {
   public:
    INtpTransport() = default;

    /// Non-copyable and non-movable to prevent interface slicing.
    INtpTransport(const INtpTransport&) = delete;
    INtpTransport& operator=(const INtpTransport&) = delete;
    INtpTransport(INtpTransport&&) = delete;
    INtpTransport& operator=(INtpTransport&&) = delete;

    /// Hold while accessing callback state to serialise slot claims and
    /// releases.
    virtual void lock() = 0;
    /// Release the lock taken by lock().
    virtual void unlock() = 0;

    /// Begin resolving `host`. Call with the lock held.
    ///
    /// \param host a NUL-terminated server name.
    /// \param address written when the result is RESOLVED.
    /// \param observer called back exactly once when the result is IN_PROGRESS.
    [[nodiscard]] virtual ResolveStatus resolve(const char* host,
                                                NtpAddress& address,
                                                IResolveObserver& observer) = 0;

    /// Open a UDP endpoint for one exchange. \return whether it opened.
    [[nodiscard]] virtual bool open(IDatagramObserver& observer) = 0;

    /// Stop delivering datagrams and close the endpoint opened by open().
    /// Safe to call when open() failed.
    virtual void close() = 0;

    /// Send `payload` to `server`. \return whether it reached the wire.
    [[nodiscard]] virtual bool send(const NtpAddress& server,
                                    std::span<const std::uint8_t> payload) = 0;

    /// Service the background context and yield. Called from the poll loop.
    virtual void poll() = 0;

    /// Monotonic milliseconds. Only differences are used, so any epoch will do.
    [[nodiscard]] virtual std::uint64_t now_ms() const = 0;

    /// Block for `duration`, between attempts.
    virtual void wait(std::chrono::milliseconds duration) = 0;

    /// A fresh 64-bit anti-replay nonce (VR-4).
    [[nodiscard]] virtual std::uint64_t nonce() = 0;

   protected:
    // The constructor caller owns the transport.
    // Deletion through this type does not compile.
    ~INtpTransport() = default;
};

}  // namespace pico_time
