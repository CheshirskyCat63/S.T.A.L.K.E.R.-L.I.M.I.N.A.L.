#pragma once

// STRATUM state ownership contract — portable C++17, no IX-Ray or SDK types.
// The integration layer must call this on the simulation thread at explicit
// handover boundaries. Protect concurrent readers via immutable snapshots;
// this class by itself is not a multi-threaded physics scheduler.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace stratum
{
enum class StateDomain : std::uint8_t
{
    PhysicsBody,
    AudioVoice,
    ActorFinalPose,
    StaticWorldGeometry,
    DynamicWorldGeometry,
    Navigation
};

enum class StateOwner : std::uint8_t
{
    Ode,
    PhysX,
    XrSound,
    StratumAnimation,
    IxRayWorld,
    StratumCollisionOverlay,
    IxRayNavigation,
    StratumNavigation
};

struct StateKey
{
    std::uint64_t entity_id = 0;
    std::uint32_t generation = 0;
    StateDomain domain = StateDomain::PhysicsBody;

    bool operator==(const StateKey& other) const noexcept
    {
        return entity_id == other.entity_id &&
            generation == other.generation &&
            domain == other.domain;
    }
};

struct StateKeyHash
{
    std::size_t operator()(const StateKey& key) const noexcept
    {
        std::uint64_t x = key.entity_id;
        x ^= static_cast<std::uint64_t>(key.generation) << 32;
        x ^= static_cast<std::uint64_t>(key.domain) << 56;
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        return static_cast<std::size_t>(x);
    }
};

enum class AuthorityResult : std::uint8_t
{
    Ok,
    AlreadyOwned,
    Conflict,
    Missing,
    InvalidKey,
    InvalidOwner
};

constexpr bool IsAllowedOwner(StateDomain domain, StateOwner owner) noexcept
{
    switch (domain)
    {
    case StateDomain::PhysicsBody:
        return owner == StateOwner::Ode || owner == StateOwner::PhysX;
    case StateDomain::AudioVoice:
        return owner == StateOwner::XrSound;
    case StateDomain::ActorFinalPose:
        return owner == StateOwner::StratumAnimation;
    case StateDomain::StaticWorldGeometry:
        return owner == StateOwner::IxRayWorld;
    case StateDomain::DynamicWorldGeometry:
        return owner == StateOwner::StratumCollisionOverlay;
    case StateDomain::Navigation:
        return owner == StateOwner::IxRayNavigation ||
            owner == StateOwner::StratumNavigation;
    }
    return false;
}

class AuthorityRegistry
{
public:
    AuthorityResult Acquire(const StateKey key, const StateOwner owner)
    {
        if (!ValidKey(key))
            return AuthorityResult::InvalidKey;
        if (!IsAllowedOwner(key.domain, owner))
            return AuthorityResult::InvalidOwner;

        const auto result = owners_.emplace(key, owner);
        if (result.second)
            return AuthorityResult::Ok;
        return result.first->second == owner ?
            AuthorityResult::AlreadyOwned : AuthorityResult::Conflict;
    }

    // Must be called only at a synchronized solver/animation ownership barrier.
    // A mismatch NEVER changes the current owner.
    AuthorityResult Transfer(
        const StateKey key,
        const StateOwner expected,
        const StateOwner next)
    {
        if (!ValidKey(key))
            return AuthorityResult::InvalidKey;
        if (!IsAllowedOwner(key.domain, next))
            return AuthorityResult::InvalidOwner;

        const auto it = owners_.find(key);
        if (it == owners_.end())
            return AuthorityResult::Missing;
        if (it->second != expected)
            return AuthorityResult::Conflict;
        if (expected == next)
            return AuthorityResult::AlreadyOwned;
        it->second = next;
        return AuthorityResult::Ok;
    }

    AuthorityResult Release(const StateKey key, const StateOwner expected)
    {
        if (!ValidKey(key))
            return AuthorityResult::InvalidKey;
        const auto it = owners_.find(key);
        if (it == owners_.end())
            return AuthorityResult::Missing;
        if (it->second != expected)
            return AuthorityResult::Conflict;
        owners_.erase(it);
        return AuthorityResult::Ok;
    }

    std::optional<StateOwner> Get(const StateKey key) const
    {
        const auto it = owners_.find(key);
        if (it == owners_.end())
            return std::nullopt;
        return it->second;
    }

    std::size_t Size() const noexcept { return owners_.size(); }

private:
    static bool ValidKey(const StateKey& key) noexcept
    {
        return key.entity_id != 0 && key.generation != 0;
    }

    std::unordered_map<StateKey, StateOwner, StateKeyHash> owners_;
};
} // namespace stratum
