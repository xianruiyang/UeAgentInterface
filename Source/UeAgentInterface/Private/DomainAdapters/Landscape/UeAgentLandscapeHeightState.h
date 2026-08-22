#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class ALandscape;
class FJsonObject;
class UWorld;

namespace UE::UAI::DomainAdapters::LandscapeGeometryEffect
{
	class IUeAgentLandscapeGeometryEffectReferenceResolver;
}

namespace UE::UAI::DomainAdapters::Landscape
{
	struct UEAGENTINTERFACE_API FLandscapeHeightStateError final
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	struct UEAGENTINTERFACE_API FLandscapeHeightState final
	{
		FString ActorPath;
		FString ManagedId;
		FString ManagedFolder;
		bool bManagedTagPresent = false;
		FIntPoint ExtentMinInclusive = FIntPoint::ZeroValue;
		FIntPoint ExtentMaxInclusive = FIntPoint::ZeroValue;
		FIntPoint RegionMinInclusive = FIntPoint::ZeroValue;
		FIntPoint RegionMaxInclusive = FIntPoint::ZeroValue;
		FVector Scale = FVector::OneVector;
		TArray<double> Heights;

		int64 GetSampleCount() const;
	};

	/**
	 * Resolves one exact Landscape identity. Managed UAI identities are checked
	 * first and fail closed on duplicate or corrupt ownership. The optional
	 * operation resolver preserves existing exact domain bindings; otherwise
	 * only an unchanged UE object path is accepted.
	 */
	UEAGENTINTERFACE_API bool TryResolveLandscapeHeightTarget(
		UWorld& World,
		const FString& ExactTarget,
		LandscapeGeometryEffect::
			IUeAgentLandscapeGeometryEffectReferenceResolver*
				OperationReferenceResolver,
		ALandscape*& OutLandscape,
		FLandscapeHeightStateError& OutError);

	/** Reads one bounded row-major, x-fastest local-height region. */
	UEAGENTINTERFACE_API bool TryReadLandscapeHeightState(
		ALandscape& Landscape,
		const FIntPoint& RegionMinInclusive,
		const FIntPoint& RegionMaxInclusive,
		int64 MaxSamples,
		FLandscapeHeightState& OutState,
		FLandscapeHeightStateError& OutError);

	/** Projects the shared native state without rereading or reinterpreting it. */
	UEAGENTINTERFACE_API TSharedRef<FJsonObject>
	BuildLandscapeHeightStateObject(
		const FString& ExactTarget,
		const FLandscapeHeightState& State);
}
