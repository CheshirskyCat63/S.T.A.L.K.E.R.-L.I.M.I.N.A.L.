#pragma once

#include <algorithm>
#include <cmath>

// Small, engine-independent operations shared by the native adapter and its
// numerical tests. Power is ANM amplitude, never a slerp blend coefficient.
namespace BodycamMotion
{
template <class Quaternion>
Quaternion ScaleRotation(const Quaternion& Input, float Power)
{
    Quaternion Result = Input;
    Result.x = Result.y = Result.z = 0.f;
    Result.w = 1.f;
    if (!std::isfinite(Power)) { return Result; }
    Power = std::clamp(Power, 0.f, 3.f);

    const float Length = std::sqrt(Input.x*Input.x + Input.y*Input.y + Input.z*Input.z + Input.w*Input.w);
    if (!std::isfinite(Length) || Length <= 1e-12f || Power == 0.f) { return Result; }
    // q and -q describe the same rotation. Keep the shortest arc from rest.
    const float InvLength = (Input.w < 0.f ? -1.f : 1.f) / Length;
    const float X = Input.x * InvLength;
    const float Y = Input.y * InvLength;
    const float Z = Input.z * InvLength;
    const float W = Input.w * InvLength;
    const float SinHalf = std::sqrt(X*X + Y*Y + Z*Z);
    const float HalfAngle = std::atan2(SinHalf, W);
    const float Scale = SinHalf > 1e-8f ? std::sin(HalfAngle * Power) / SinHalf : Power;
    Result.x = X * Scale;
    Result.y = Y * Scale;
    Result.z = Z * Scale;
    Result.w = std::cos(HalfAngle * Power);
    return Result;
}

inline float TransitionWeight(float Elapsed, float Duration)
{
    if (Duration <= 0.f) { return 1.f; }
    const float T = std::clamp(Elapsed / Duration, 0.f, 1.f);
    return T*T*(3.f - 2.f*T);
}
}
