#pragma once

// Canonical boundary types for native physics -> game/acoustics/AI.
// This file is a standalone contract. It does NOT attach itself to game events.

#include <array>
#include <cstdint>
#include <limits>

namespace stratum
{
constexpr std::uint32_t kContactEventVersion = 1;
constexpr std::uint32_t kUnknownMaterialId =
    std::numeric_limits<std::uint32_t>::max();

struct EntityRef
{
    std::uint64_t id = 0;
    std::uint32_t generation = 0;
};

struct SimulationStamp
{
    std::uint64_t tick = 0;       // monotonically increasing physics tick
    double seconds = 0.0;         // timestamp on the one shared timeline
};

enum class ContactKind : std::uint8_t
{
    Impact,
    Sliding,
    Breaking,
    Footstep
};

struct PhysicsContactEventV1
{
    std::uint32_t version = kContactEventVersion;
    std::uint64_t event_id = 0;
    SimulationStamp time{};

    EntityRef a{};
    EntityRef b{};

    // PERSISTENT GameMtlLib IDs, never implicit indices into a runtime array.
    std::uint32_t material_a_id = kUnknownMaterialId;
    std::uint32_t material_b_id = kUnknownMaterialId;

    // IX-Ray world coordinates; metres, m/s, N*s. Converter at SDK boundary.
    std::array<float, 3> position_m{};
    std::array<float, 3> contact_normal{};
    std::array<float, 3> relative_velocity_mps{};

    ContactKind kind = ContactKind::Impact;

    // If false, the impulse numbers are NOT source-verified measurements.
    bool impulse_measured = false;
    float normal_impulse_ns = 0.0f;
    float tangential_impulse_ns = 0.0f;
};

struct PhysicsStep
{
    SimulationStamp time{};
    float dt_seconds = 0.0f;
};

class IPhysicsBackend
{
public:
    virtual ~IPhysicsBackend() = default;
    virtual void Step(const PhysicsStep& step) = 0;
    virtual void SubmitImpact(const PhysicsContactEventV1& event) = 0;
};

class IContactEventSink
{
public:
    virtual ~IContactEventSink() = default;
    virtual void PublishContact(const PhysicsContactEventV1& event) = 0;
};
} // namespace stratum
