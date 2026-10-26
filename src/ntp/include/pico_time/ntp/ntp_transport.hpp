// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

/// \file ntp_transport.hpp
/// The environment an NTP exchange needs, as one interface (EN-04.1).
///
/// Everything on the far side of this seam is lwIP and the Pico SDK: name
/// resolution, a UDP endpoint, the monotonic clock, the background poll and the
/// lock those callbacks run under. Everything on the near side -- the attempt
/// loop, the slot pool that outlives an uncancellable lookup, and the decision
/// about what a failed attempt means -- is policy, and is host-tested against a
/// fake.
///
/// The split is drawn here and not lower because the slot pool exists *because*
/// of a property of this interface: resolve() cannot be cancelled, so a lookup
/// fires after the attempt that started it has been torn down. That protocol is
/// the subtlest thing in this library and it belongs where a test can drive it.
///
/// Requirement IDs cited here are defined in docs/technical-specification.md.

namespace pico_time {

/// Bytes an address occupies. Large enough for an lwIP ip_addr_t in its
/// dual-stack configuration; the adapter static_asserts it against the real
/// type, which is the only place the two spellings meet.
inline constexpr std::size_t NTP_ADDRESS_BYTES = 20;

/// A resolved server address, opaque to the policy above the seam.
///
/// The policy does not interpret one: it receives an address from resolve()
/// and hands the same value back to send(). Holding it as an
/// implementation-defined blob keeps ip_addr_t -- and so lwIP -- out of every
/// header the policy includes, which is what lets that policy compile on a
/// host at all.
///
/// It does compare two for equality, to tell a reply from the server it asked
/// from a stray datagram. A transport must therefore write the same bytes for
/// the same endpoint on both the resolve and the receive path, padding
/// included; the adapter's size assert is what holds that, because the lwIP
/// configuration whose representation stops being canonical is also the one
/// too large to fit.
struct NtpAddress {
    /// The address, in the transport's own representation.
    alignas(std::uint32_t) std::array<std::uint8_t, NTP_ADDRESS_BYTES> opaque{};

    /// Whether resolve() has filled this in.
    bool valid = false;
};

/// What a resolution request did.
enum class ResolveStatus : std::uint8_t {
    /// Answered immediately, from cache. The address is written and no
    /// callback will fire.
    RESOLVED,
    /// The observer will be called back exactly once, and cannot be cancelled.
    /// This is the case the slot pool exists for.
    IN_PROGRESS,
    /// The query was refused. No callback will fire.
    FAILED,
};

/// Notified when a resolution started with IN_PROGRESS lands.
///
/// Called in the transport's callback context, holding the transport's lock.
/// An implementation stores and returns (VR-8).
class IResolveObserver {
   public:
    IResolveObserver() = default;

    /// An interface is never copied: assigning one implementation to another
    /// through this type would slice it. The destructor below is protected and
    /// non-virtual on purpose -- see there.
    IResolveObserver(const IResolveObserver&) = delete;
    IResolveObserver& operator=(const IResolveObserver&) = delete;
    IResolveObserver(IResolveObserver&&) = delete;
    IResolveObserver& operator=(IResolveObserver&&) = delete;

    /// \param address the resolved address, or an invalid one on failure.
    virtual void on_resolved(const NtpAddress& address) = 0;

   protected:
    // Not deleted through this interface: an observer is a member of the
    // attempt slot that owns it, so a virtual destructor would buy nothing and
    // cost a vtable entry (CXX-05.7).
    ~IResolveObserver() = default;
};

/// Notified for each datagram arriving on the open endpoint.
class IDatagramObserver {
   public:
    IDatagramObserver() = default;

    /// An interface is never copied: assigning one implementation to another
    /// through this type would slice it. The destructor below is protected and
    /// non-virtual on purpose -- see there.
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

/// The seam. One implementation over lwIP, one fake.
class INtpTransport {
   public:
    INtpTransport() = default;

    /// An interface is never copied: assigning one implementation to another
    /// through this type would slice it. The destructor below is protected and
    /// non-virtual on purpose -- see there.
    INtpTransport(const INtpTransport&) = delete;
    INtpTransport& operator=(const INtpTransport&) = delete;
    INtpTransport(INtpTransport&&) = delete;
    INtpTransport& operator=(INtpTransport&&) = delete;

    /// Hold while touching state a callback also touches.
    ///
    /// On the real transport this is cyw43_arch_lwip_begin/end. It is on the
    /// interface rather than hidden inside it because the slot protocol's
    /// correctness rests on it: claiming and releasing a slot is serialized
    /// against the resolution callback by this lock and by nothing else.
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
    // Protected and non-virtual: a transport is owned by whoever constructed
    // it, never deleted through this interface, and this makes that a compile
    // error rather than a convention (CXX-05.7).
    ~INtpTransport() = default;
};

}  // namespace pico_time
