#include "AuthorityRegistry.hpp"
#include "PhysicsEvents.hpp"

#include <cassert>
#include <cstdint>

int main()
{
    using namespace stratum;
    AuthorityRegistry registry;

    const StateKey box{42, 1, StateDomain::PhysicsBody};
    assert(registry.Acquire(box, StateOwner::Ode) == AuthorityResult::Ok);
    assert(registry.Acquire(box, StateOwner::Ode) == AuthorityResult::AlreadyOwned);
    assert(registry.Acquire(box, StateOwner::PhysX) == AuthorityResult::Conflict);
    assert(registry.Get(box) == StateOwner::Ode);

    // Wrong current owner: no state mutation.
    assert(registry.Transfer(box, StateOwner::PhysX, StateOwner::Ode) ==
        AuthorityResult::Conflict);
    assert(registry.Get(box) == StateOwner::Ode);
    assert(registry.Transfer(box, StateOwner::Ode, StateOwner::PhysX) ==
        AuthorityResult::Ok);
    assert(registry.Get(box) == StateOwner::PhysX);

    // The engine must use the new object generation on entity ID reuse.
    const StateKey respawn{42, 2, StateDomain::PhysicsBody};
    assert(registry.Acquire(respawn, StateOwner::Ode) == AuthorityResult::Ok);
    assert(registry.Get(box) == StateOwner::PhysX);

    const StateKey voice{123, 1, StateDomain::AudioVoice};
    assert(registry.Acquire(voice, StateOwner::Ode) ==
        AuthorityResult::InvalidOwner);
    assert(registry.Acquire(voice, StateOwner::XrSound) ==
        AuthorityResult::Ok);
    assert(registry.Transfer(voice, StateOwner::XrSound, StateOwner::PhysX) ==
        AuthorityResult::InvalidOwner);

    const StateKey pose{123, 1, StateDomain::ActorFinalPose};
    assert(registry.Acquire(pose, StateOwner::StratumAnimation) ==
        AuthorityResult::Ok);

    assert(registry.Release(box, StateOwner::Ode) ==
        AuthorityResult::Conflict);
    assert(registry.Release(box, StateOwner::PhysX) == AuthorityResult::Ok);
    assert(!registry.Get(box).has_value());
    assert(registry.Size() == 3);

    // The schema distinguishes unknown material IDs and unmeasured impulses.
    PhysicsContactEventV1 contact;
    assert(contact.version == kContactEventVersion);
    assert(contact.material_a_id == kUnknownMaterialId);
    assert(!contact.impulse_measured);
    contact.event_id = 9;
    contact.a = {123, 1};
    contact.b = {42, 2};
    contact.time = {5, 5.0 / 120.0};
    contact.position_m = {1.0f, 2.0f, 3.0f};
    assert(contact.event_id == 9 && contact.a.generation == 1);

    return 0;
}
