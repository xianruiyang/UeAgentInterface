#include "Core/HjsonWorkspace/UeAgentSemanticSurfaceComposition.h"

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		ESemanticUnitKind ItemUnitKind(EUeAgentSemanticSurfaceKind Kind)
		{
			switch (Kind)
			{
			case EUeAgentSemanticSurfaceKind::PropertyCollection:
				return ESemanticUnitKind::PropertySet;
			case EUeAgentSemanticSurfaceKind::Spatial:
				return ESemanticUnitKind::LayoutState;
			case EUeAgentSemanticSurfaceKind::ExternalProvenance:
				return ESemanticUnitKind::ExternalPayloadState;
			case EUeAgentSemanticSurfaceKind::KeyedTable:
				return ESemanticUnitKind::CollectionState;
			default:
				return ESemanticUnitKind::ItemState;
			}
		}

		bool TryStringValue(
			const FString& Text,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			OutValue.Reset();
			return FUeAgentCanonicalValue::TryString(
					Text,
					Resources,
					OutValue,
					OutError)
				&& OutValue.IsValid();
		}

		bool TryIntegerValue(
			int64 Number,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			OutValue.Reset();
			return FUeAgentCanonicalValue::TryInteger(
					FString::Printf(TEXT("%lld"), Number),
					Resources,
					OutValue,
					OutError)
				&& OutValue.IsValid();
		}
	}

	bool TryBuildSemanticSurfaceAuthoringModel(
		FUeAgentSemanticSurfaceRef Surface,
		const FString& BaseRevision,
		FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError)
	{
		OutModel.Reset();
		OutError.Reset();
		const FString TargetId = Surface->GetSurfaceId();
		const FString OrderingId = TargetId + TEXT(":ordering");
		const TArray<FString> Visibility{TEXT("state"), TEXT("edit_bundle")};
		TArray<FSemanticUnit> Units;
		TArray<FString> ItemRefs;
		TArray<FString> RelationRefs;
		TMap<FString, FUeAgentCanonicalValueRef> OrderingValues;
		FUeAgentStateSemanticError SemanticError;

		for (const FUeAgentSemanticSurfaceItem& Item : Surface->GetItems())
		{
			FUeAgentCanonicalValuePtr OrderValue;
			if (!TryIntegerValue(Item.Order, Resources, OrderValue, SemanticError))
			{
				OutError.SetFailure(SemanticError, Item.Identity, TEXT("/order"));
				return false;
			}
			OrderingValues.Add(Item.Identity, OrderValue.ToSharedRef());
			TArray<FString> Children;
			for (const FUeAgentSemanticSurfaceItem& Candidate
				: Surface->GetItems())
			{
				if (Candidate.OwnerIdentity == Item.Identity)
				{
					Children.Add(Candidate.Identity);
				}
			}
			Units.Emplace(
				Item.Identity,
				ItemUnitKind(Surface->GetKind()),
				Item.OwnerIdentity,
				Item.SemanticType,
				Item.Slot,
				Item.StableKey,
				Item.Values,
				MoveTemp(Children),
				TArray<FString>{},
				OrderingId,
				FString(),
				TArray<FString>{},
				Visibility,
				Surface->GetSourceProvenance(),
				Item.StableKey);
			ItemRefs.Add(Item.Identity);
		}
		for (const FUeAgentSemanticSurfaceEndpoint& Endpoint
			: Surface->GetEndpoints())
		{
			TMap<FString, FUeAgentCanonicalValueRef> Values;
			FUeAgentCanonicalValuePtr RoleValue;
			if (!TryStringValue(
					Endpoint.Role,
					Resources,
					RoleValue,
					SemanticError))
			{
				OutError.SetFailure(SemanticError, Endpoint.Identity, TEXT("/role"));
				return false;
			}
			Values.Add(TEXT("role"), RoleValue.ToSharedRef());
			Units.Emplace(
				Endpoint.Identity,
				ESemanticUnitKind::ItemState,
				Endpoint.ItemIdentity,
				TEXT("uai.semantic_surface.endpoint/1"),
				TEXT("endpoint"),
				Endpoint.Identity,
				MoveTemp(Values),
				TArray<FString>{},
				TArray<FString>{},
				OrderingId,
				FString(),
				TArray<FString>{},
				Visibility,
				Surface->GetSourceProvenance(),
				Endpoint.Identity);
			ItemRefs.Add(Endpoint.Identity);
		}
		for (const FUeAgentSemanticSurfaceRelation& Relation
			: Surface->GetRelations())
		{
			TMap<FString, FUeAgentCanonicalValueRef> Values;
			FUeAgentCanonicalValuePtr TypeValue;
			if (!TryStringValue(
					Relation.RelationType,
					Resources,
					TypeValue,
					SemanticError))
			{
				OutError.SetFailure(SemanticError, Relation.Identity, TEXT("/relation_type"));
				return false;
			}
			Values.Add(TEXT("relation_type"), TypeValue.ToSharedRef());
			Units.Emplace(
				Relation.Identity,
				ESemanticUnitKind::RelationSet,
				TargetId,
				TEXT("uai.semantic_surface.relation/1"),
				TEXT("relation"),
				Relation.Identity,
				MoveTemp(Values),
				TArray<FString>{},
				TArray<FString>{},
				OrderingId,
				FString(),
				TArray<FString>{
					Relation.FromEndpointIdentity,
					Relation.ToEndpointIdentity},
				Visibility,
				Surface->GetSourceProvenance(),
				Relation.Identity);
			RelationRefs.Add(Relation.Identity);
		}

		Units.Emplace(
			OrderingId,
			ESemanticUnitKind::OrderingState,
			TargetId,
			TEXT("uai.semantic_surface.ordering/1"),
			TEXT("stable_order"),
			TEXT("order"),
			MoveTemp(OrderingValues),
			TArray<FString>{},
			TArray<FString>{},
			FString(),
			FString(),
			TArray<FString>{},
			Visibility,
			Surface->GetSourceProvenance(),
			OrderingId);

		TMap<FString, FUeAgentCanonicalValueRef> TargetValues;
		FUeAgentCanonicalValuePtr KindValue;
		if (!TryStringValue(
				LexToString(Surface->GetKind()),
				Resources,
				KindValue,
				SemanticError))
		{
			OutError.SetFailure(SemanticError, TargetId, TEXT("/kind"));
			return false;
		}
		TargetValues.Add(TEXT("kind"), KindValue.ToSharedRef());
		ItemRefs.Add(OrderingId);
		Units.Emplace(
			TargetId,
			ESemanticUnitKind::TargetState,
			FString(),
			TEXT("uai.semantic_surface.target/1"),
			LexToString(Surface->GetKind()),
			FString(),
			MoveTemp(TargetValues),
			MoveTemp(ItemRefs),
			MoveTemp(RelationRefs),
			OrderingId,
			FString(),
			TArray<FString>{},
			Visibility,
			Surface->GetSourceProvenance(),
			TargetId);

		return TryCompileAuthoringModel(
			TEXT("uai.semantic_surface.authoring/1"),
			1,
			TEXT("state"),
			BaseRevision,
			MoveTemp(Units),
			{TargetId},
			Resources,
			OutModel,
			OutError);
	}
}
