#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::StateSemantic
{
	enum class EUeAgentSemanticSurfaceKind : uint8
	{
		PropertyCollection,
		Graph,
		Tree,
		Timeline,
		KeyedTable,
		Spatial,
		ExternalProvenance,
	};

	enum class EUeAgentSemanticSurfaceIntent : uint8
	{
		FullState,
		MinimalCreate,
		LocalEdit,
	};

	enum class EUeAgentSemanticSurfaceCompleteness : uint8
	{
		Complete,
		Partial,
	};

	UEAGENTINTERFACE_API FString LexToString(
		EUeAgentSemanticSurfaceKind Kind);
	UEAGENTINTERFACE_API FString LexToString(
		EUeAgentSemanticSurfaceIntent Intent);
	UEAGENTINTERFACE_API FString LexToString(
		EUeAgentSemanticSurfaceCompleteness Completeness);

	struct UEAGENTINTERFACE_API FUeAgentSemanticSurfaceItem
	{
		FString Identity;
		FString SemanticType;
		FString OwnerIdentity;
		FString StableKey;
		FString Slot;
		int64 Order = 0;
		TMap<FString, FUeAgentCanonicalValueRef> Values;
	};

	struct UEAGENTINTERFACE_API FUeAgentSemanticSurfaceEndpoint
	{
		FString Identity;
		FString ItemIdentity;
		FString Role;
		int64 Order = 0;
	};

	struct UEAGENTINTERFACE_API FUeAgentSemanticSurfaceRelation
	{
		FString Identity;
		FString FromEndpointIdentity;
		FString ToEndpointIdentity;
		FString RelationType;
		int64 Order = 0;
	};

	class FUeAgentSemanticSurface;
	using FUeAgentSemanticSurfacePtr =
		TSharedPtr<const FUeAgentSemanticSurface>;
	using FUeAgentSemanticSurfaceRef =
		TSharedRef<const FUeAgentSemanticSurface>;

	class UEAGENTINTERFACE_API FUeAgentSemanticSurface final
	{
	public:
		const FString& GetSurfaceId() const;
		EUeAgentSemanticSurfaceKind GetKind() const;
		EUeAgentSemanticSurfaceIntent GetIntent() const;
		EUeAgentSemanticSurfaceCompleteness GetCompleteness() const;
		const FString& GetSourceProvenance() const;
		const TArray<FUeAgentSemanticSurfaceItem>& GetItems() const;
		const TArray<FUeAgentSemanticSurfaceEndpoint>& GetEndpoints() const;
		const TArray<FUeAgentSemanticSurfaceRelation>& GetRelations() const;
		const FUeAgentSemanticSurfaceItem* FindItem(
			const FString& Identity) const;

	private:
		FUeAgentSemanticSurface(
			FString InSurfaceId,
			EUeAgentSemanticSurfaceKind InKind,
			EUeAgentSemanticSurfaceIntent InIntent,
			EUeAgentSemanticSurfaceCompleteness InCompleteness,
			FString InSourceProvenance,
			TArray<FUeAgentSemanticSurfaceItem> InItems,
			TArray<FUeAgentSemanticSurfaceEndpoint> InEndpoints,
			TArray<FUeAgentSemanticSurfaceRelation> InRelations);

		FString SurfaceId;
		EUeAgentSemanticSurfaceKind Kind =
			EUeAgentSemanticSurfaceKind::PropertyCollection;
		EUeAgentSemanticSurfaceIntent Intent =
			EUeAgentSemanticSurfaceIntent::FullState;
		EUeAgentSemanticSurfaceCompleteness Completeness =
			EUeAgentSemanticSurfaceCompleteness::Complete;
		FString SourceProvenance;
		TArray<FUeAgentSemanticSurfaceItem> Items;
		TArray<FUeAgentSemanticSurfaceEndpoint> Endpoints;
		TArray<FUeAgentSemanticSurfaceRelation> Relations;
		TMap<FString, int32> ItemIndex;

		friend class FUeAgentSemanticSurfaceBuilder;
	};

	class UEAGENTINTERFACE_API FUeAgentSemanticSurfaceBuilder final
	{
	public:
		FUeAgentSemanticSurfaceBuilder(
			FString InSurfaceId,
			EUeAgentSemanticSurfaceKind InKind,
			EUeAgentSemanticSurfaceIntent InIntent,
			EUeAgentSemanticSurfaceCompleteness InCompleteness,
			FString InSourceProvenance);

		bool TryAddItem(
			FUeAgentSemanticSurfaceItem Item,
			UE::UAI::Contract::FDiagnostic& OutError);
		bool TryAddEndpoint(
			FUeAgentSemanticSurfaceEndpoint Endpoint,
			UE::UAI::Contract::FDiagnostic& OutError);
		bool TryAddRelation(
			FUeAgentSemanticSurfaceRelation Relation,
			UE::UAI::Contract::FDiagnostic& OutError);
		bool TryBuild(
			ContextResource::FUeAgentResourceTracker& Resources,
			FUeAgentSemanticSurfacePtr& OutSurface,
			UE::UAI::Contract::FDiagnostic& OutError) const;

	private:
		FString SurfaceId;
		EUeAgentSemanticSurfaceKind Kind =
			EUeAgentSemanticSurfaceKind::PropertyCollection;
		EUeAgentSemanticSurfaceIntent Intent =
			EUeAgentSemanticSurfaceIntent::FullState;
		EUeAgentSemanticSurfaceCompleteness Completeness =
			EUeAgentSemanticSurfaceCompleteness::Complete;
		FString SourceProvenance;
		TArray<FUeAgentSemanticSurfaceItem> Items;
		TArray<FUeAgentSemanticSurfaceEndpoint> Endpoints;
		TArray<FUeAgentSemanticSurfaceRelation> Relations;
	};

	UEAGENTINTERFACE_API bool TrySerializeSemanticSurface(
		FUeAgentSemanticSurfaceRef Surface,
		TArray<uint8>& OutCanonicalUtf8,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryParseSemanticSurface(
		const TArray<uint8>& CanonicalUtf8,
		ContextResource::FUeAgentResourceTracker& Resources,
		FUeAgentSemanticSurfacePtr& OutSurface,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryBuildSemanticSurfaceSnapshot(
		FUeAgentSemanticSurfaceRef Surface,
		const FString& SnapshotId,
		Contract::EUeAgentStatePlane StatePlane,
		const FUeAgentStateRevision& Revision,
		const FUeAgentTemporalPoint& CapturedAt,
		const FUeAgentStateProvenance& Provenance,
		ContextResource::FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalSnapshotPtr& OutSnapshot,
		FUeAgentStateSemanticError& OutError);

	UEAGENTINTERFACE_API bool TryCompareSemanticSurfaces(
		FUeAgentSemanticSurfaceRef Left,
		FUeAgentSemanticSurfaceRef Right,
		ContextResource::FUeAgentResourceTracker& Resources,
		FUeAgentSemanticComparison& OutComparison,
		FUeAgentStateSemanticError& OutError);

}
