#include "AuthorityRegistry.hpp"
#include "PhysicsEvents.hpp"

#include <cstdint>
#include <iostream>

static int Fail(const char* expression, const char* file, int line)
{
    std::cerr << file << ':' << line << ": CHECK failed: "
              << expression << '\n';
    return 1;
}

// Do not use assert(): Release / NDEBUG would silently disable the tests.
#define CHECK(condition) do { if (!(condition)) return Fail(#condition, __FILE__, __LINE__); } while (false)

int main()
{
    using namespace stratum;
    AuthorityRegistry registry;

    const StateKey invalid{0, 1, StateDomain::PhysicsBody};
    CHECK(registry.Acquire(invalid, StateOwner::Ode) ==
        AuthorityResult::InvalidKey);

    const StateKey body{42, 1, StateDomain::PhysicsBody};
    CHECK(registry.Acquire(body, StateOwner::Ode) == AuthorityResult::Ok);
    CHECK(registry.Acquire(body, StateOwner::Ode) == AuthorityResult::AlreadyOwned);
    CHECK(registry.Acquire(body, StateOwner::PhysX) == AuthorityResult::Conflict);
    CHECK(registry.Get(body) == StateOwner::Ode);

    // Unauthorized transfer cannot change the body state.
    CHECK(registry.Transfer(body, StateOwner::PhysX, StateOwner::Ode) ==
        AuthorityResult::Conflict);
    CHECK(registry.Get(body) == StateOwner::Ode);
    CHECK(registry.Transfer(body, StateOwner::Ode, StateOwner::PhysX) ==
        AuthorityResult::Ok);
    CHECK(registry.Get(body) == StateOwner::PhysX);

    // Reused entity IDs use a new generation, not the stale former owner.
    const StateKey respawn{42, 2, StateDomain::PhysicsBody};
    CHECK(registry.Acquire(respawn, StateOwner::Ode) == AuthorityResult::Ok);
    CHECK(registry.Get(body) == StateOwner::PhysX);

    const StateKey voice{123, 1, StateDomain::AudioVoice};
    CHECK(registry.Acquire(voice, StateOwner::Ode) ==
        AuthorityResult::InvalidOwner);
    CHECK(registry.Acquire(voice, StateOwner::XrSound) ==
        AuthorityResult::Ok);
    CHECK(registry.Transfer(voice, StateOwner::XrSound, StateOwner::PhysX) ==
        AuthorityResult::InvalidOwner);
    CHECK(registry.Get(voice) == StateOwner::XrSound);

    const StateKey pose{123, 1, StateDomain::ActorFinalPose};
    CHECK(registry.Acquire(pose, StateOwner::StratumAnimation) ==
        AuthorityResult::Ok);

    CHECK(registry.Release(body, StateOwner::Ode) == AuthorityResult::Conflict);
    CHECK(registry.Release(body, StateOwner::PhysX) == AuthorityResult::Ok);
    CHECK(!registry.Get(body).has_value());
    CHECK(registry.Size() == 3);

    // Distinguish unknown persistent IDs and genuinely measured impulses.
    PhysicsContactEventV1 contact{};
    CHECK(contact.version == kContactEventVersion);
    CHECK(contact.material_a_id == kUnknownMaterialId);
    CHECK(contact.material_b_id == kUnknownMaterialId);
    CHECK(!contact.impulse_measured);
    contact.event_id = 9;
    contact.a = {123, 1};
    contact.b = {42, 2};
    contact.time = {5, 5.0 / 120.0};
    contact.position_m = {1.0f, 2.0f, 3.0f};
    CHECK(contact.event_id == 9 && contact.a.generation == 1);

    std::cout << "STRATUM ownership/event contracts PASS\n";
    return 0;
}
