#pragma once

namespace XRay::Sound::RoomChecksum
{
	// The v1 room sidecar uses CRC-32/ISO-HDLC (IEEE), as does its exporter.
	// xrCore::crc32 dispatches to a different polynomial on SSE4.2 CPUs.
	struct IeeeTable
	{
		u32 Values[256] = {};

		constexpr IeeeTable() noexcept
		{
			for (u32 Index = 0; Index < 256; ++Index)
			{
				u32 Value = Index;
				for (u32 Bit = 0; Bit < 8; ++Bit)
				{
					Value = (Value >> 1) ^ ((Value & 1u) != 0 ? 0xEDB88320u : 0u);
				}
				Values[Index] = Value;
			}
		}
	};

	inline u32 IeeeCrc32(const void* Data, size_t Size) noexcept
	{
		static constexpr IeeeTable Table;
		const u8* Bytes = static_cast<const u8*>(Data);
		u32 Crc = 0xFFFFFFFFu;
		for (size_t Index = 0; Index < Size; ++Index)
		{
			Crc = (Crc >> 8) ^ Table.Values[(Crc ^ Bytes[Index]) & 0xFFu];
		}
		return Crc ^ 0xFFFFFFFFu;
	}
}
