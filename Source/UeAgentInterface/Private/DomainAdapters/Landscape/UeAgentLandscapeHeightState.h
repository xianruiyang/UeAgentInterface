#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

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
	using FLandscapeHeightStateError = UE::UAI::Contract::FDiagnostic;

	struct UEAGENTINTERFACE_API FLandscapeHeightState final
	{
		FString ActorPath;
		FIntPoint ExtentMinInclusive = FIntPoint::ZeroValue;
		FIntPoint ExtentMaxInclusive = FIntPoint::ZeroValue;
		FVector Scale = FVector::OneVector;
		TArray<double> Heights;

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
		FLandscapeHeightState& OutState,
		FLandscapeHeightStateError& OutError);

	/** Builds the normalized Query value consumed by the public projection. */
	UEAGENTINTERFACE_API TSharedRef<FJsonObject>
	BuildLandscapeHeightQueryObject(
		const FLandscapeHeightState& State);
}
