#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::ContentDiscovery
{
	/** Explicitly owned by one read-only normalization pass. Never pass across mutation. */
	class FReadOnlyCandidateBatch final
	{
		struct FEntry
		{
			virtual ~FEntry() = default;
		};

		template <typename T> struct TEntry final : FEntry
		{
			TSharedPtr<T> Value;
		};

		TMap<const void*, TMap<const void*, TMap<FString, TSharedPtr<FEntry>>>> Entries;

	public:
		FReadOnlyCandidateBatch() = default;
		FReadOnlyCandidateBatch(const FReadOnlyCandidateBatch&) = delete;
		FReadOnlyCandidateBatch& operator=(const FReadOnlyCandidateBatch&) = delete;

		/** Loading objects can change catalogs even without an asset mutation. */
		void Invalidate() { Entries.Reset(); }

		/** Type, exact target and complete domain context identify a prepared catalog. */
		template <typename T, typename FBuild>
		static TSharedPtr<T> Prepare(FReadOnlyCandidateBatch* Batch, const void* Target,
			const FString& Context, FBuild&& Build)
		{
			static const uint8 TypeKey = 0;
			if (Batch)
			{
				if (const auto* Targets = Batch->Entries.Find(&TypeKey))
					if (const auto* Contexts = Targets->Find(Target))
						if (const auto* Existing = Contexts->Find(Context))
							return StaticCastSharedPtr<TEntry<T>>(*Existing)->Value;
			}
			auto Value = MakeShared<T>();
			if (!Build(*Value)) return nullptr;
			if (Batch)
			{
				auto Entry = MakeShared<TEntry<T>>();
				Entry->Value = Value;
				Batch->Entries.FindOrAdd(&TypeKey).FindOrAdd(Target).Add(Context, Entry);
			}
			return Value;
		}
	};
}
