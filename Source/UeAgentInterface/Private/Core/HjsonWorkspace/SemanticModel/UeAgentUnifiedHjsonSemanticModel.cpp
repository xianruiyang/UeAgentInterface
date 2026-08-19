#include "Core/HjsonWorkspace/SemanticModel/UeAgentUnifiedHjsonSemanticModel.h"

#include "Core/ContextResource/UeAgentContextResource.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ContextResource;

	namespace
	{
		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		FString Sha256(const FString& Material)
		{
			const FTCHARToUTF8 Utf8(*Material, Material.Len());
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const unsigned char*>(Utf8.Get()),
				static_cast<size_t>(Utf8.Length()),
				Digest);
			FString Result(TEXT("sha256:"));
			for (const uint8 Byte : Digest)
			{
				Result += FString::Printf(TEXT("%02x"), Byte);
			}
			return Result;
		}

		bool IsIntentValid(const EUnifiedHjsonAuthoringIntent Intent)
		{
			switch (Intent)
			{
			case EUnifiedHjsonAuthoringIntent::FullState:
			case EUnifiedHjsonAuthoringIntent::MinimalCreate:
			case EUnifiedHjsonAuthoringIntent::LocalEdit:
				return true;
			default:
				return false;
			}
		}

		bool IsUnitKindAllowed(
			const EUnifiedHjsonCompositionKind Composition,
			const ESemanticUnitKind UnitKind)
		{
			switch (Composition)
			{
			case EUnifiedHjsonCompositionKind::Target:
				return UnitKind == ESemanticUnitKind::TargetState;
			case EUnifiedHjsonCompositionKind::PropertySet:
			case EUnifiedHjsonCompositionKind::TemporalContract:
			case EUnifiedHjsonCompositionKind::SpatialContract:
				return UnitKind == ESemanticUnitKind::PropertySet;
			case EUnifiedHjsonCompositionKind::MemberSet:
				return UnitKind == ESemanticUnitKind::EntityState
					|| UnitKind == ESemanticUnitKind::CollectionState;
			case EUnifiedHjsonCompositionKind::Structure:
				return UnitKind == ESemanticUnitKind::StructureState;
			case EUnifiedHjsonCompositionKind::Item:
				return UnitKind == ESemanticUnitKind::ItemState;
			case EUnifiedHjsonCompositionKind::Endpoint:
				return UnitKind == ESemanticUnitKind::EntityState;
			case EUnifiedHjsonCompositionKind::Relation:
				return UnitKind == ESemanticUnitKind::RelationSet;
			case EUnifiedHjsonCompositionKind::Order:
				return UnitKind == ESemanticUnitKind::OrderingState;
			case EUnifiedHjsonCompositionKind::Layout:
				return UnitKind == ESemanticUnitKind::LayoutState;
			case EUnifiedHjsonCompositionKind::ExternalInput:
				return UnitKind == ESemanticUnitKind::ExternalPayloadState;
			default:
				return false;
			}
		}

		bool FailResource(
			FWorkspaceError& OutError,
			const FUeAgentResourceBudgetExceeded& Error)
		{
			OutError.Set(
				Error.Code,
				Error.Path,
				TEXT("/"),
				Error.Observed,
				Error.bStoppedEarly);
			return false;
		}
	}

	FString LexToString(const EUnifiedHjsonCompositionKind Kind)
	{
		switch (Kind)
		{
		case EUnifiedHjsonCompositionKind::Target:
			return TEXT("target");
		case EUnifiedHjsonCompositionKind::PropertySet:
			return TEXT("property_set");
		case EUnifiedHjsonCompositionKind::MemberSet:
			return TEXT("member_set");
		case EUnifiedHjsonCompositionKind::Structure:
			return TEXT("structure");
		case EUnifiedHjsonCompositionKind::Item:
			return TEXT("item");
		case EUnifiedHjsonCompositionKind::Endpoint:
			return TEXT("endpoint");
		case EUnifiedHjsonCompositionKind::Relation:
			return TEXT("relation");
		case EUnifiedHjsonCompositionKind::Order:
			return TEXT("order");
		case EUnifiedHjsonCompositionKind::Layout:
			return TEXT("layout");
		case EUnifiedHjsonCompositionKind::TemporalContract:
			return TEXT("temporal_contract");
		case EUnifiedHjsonCompositionKind::SpatialContract:
			return TEXT("spatial_contract");
		case EUnifiedHjsonCompositionKind::ExternalInput:
			return TEXT("external_input");
		default:
			return FString();
		}
	}

	FString LexToString(const EUnifiedHjsonAuthoringIntent Intent)
	{
		switch (Intent)
		{
		case EUnifiedHjsonAuthoringIntent::FullState:
			return TEXT("full_state");
		case EUnifiedHjsonAuthoringIntent::MinimalCreate:
			return TEXT("minimal_create");
		case EUnifiedHjsonAuthoringIntent::LocalEdit:
			return TEXT("local_edit");
		default:
			return FString();
		}
	}

	TArray<EUnifiedHjsonCompositionKind>
	GetAllUnifiedHjsonCompositionKinds()
	{
		return {
			EUnifiedHjsonCompositionKind::Target,
			EUnifiedHjsonCompositionKind::PropertySet,
			EUnifiedHjsonCompositionKind::MemberSet,
			EUnifiedHjsonCompositionKind::Structure,
			EUnifiedHjsonCompositionKind::Item,
			EUnifiedHjsonCompositionKind::Endpoint,
			EUnifiedHjsonCompositionKind::Relation,
			EUnifiedHjsonCompositionKind::Order,
			EUnifiedHjsonCompositionKind::Layout,
			EUnifiedHjsonCompositionKind::TemporalContract,
			EUnifiedHjsonCompositionKind::SpatialContract,
			EUnifiedHjsonCompositionKind::ExternalInput,
		};
	}

	FUnifiedHjsonSemanticModel::FUnifiedHjsonSemanticModel(
		FAuthoringModelRef InCoreModel,
		const EUnifiedHjsonAuthoringIntent InAuthoringIntent,
		TMap<EUnifiedHjsonCompositionKind, TArray<FString>> InCompositions,
		const int32 InBoundUnitCount,
		FString InModelFingerprint)
		: CoreModel(MoveTemp(InCoreModel))
		, AuthoringIntent(InAuthoringIntent)
		, Compositions(MoveTemp(InCompositions))
		, BoundUnitCount(InBoundUnitCount)
		, ModelFingerprint(Normalize(MoveTemp(InModelFingerprint)))
		, bValid(
			IsIntentValid(AuthoringIntent)
			&& Compositions.Num() == 12
			&& BoundUnitCount == CoreModel->GetUnits().Num()
			&& ModelFingerprint.StartsWith(TEXT("sha256:")))
	{
	}

	const FAuthoringModelRef&
	FUnifiedHjsonSemanticModel::GetCoreModel() const
	{
		return CoreModel;
	}

	EUnifiedHjsonAuthoringIntent
	FUnifiedHjsonSemanticModel::GetAuthoringIntent() const
	{
		return AuthoringIntent;
	}

	const TMap<EUnifiedHjsonCompositionKind, TArray<FString>>&
	FUnifiedHjsonSemanticModel::GetCompositions() const
	{
		return Compositions;
	}

	const TArray<FString>*
	FUnifiedHjsonSemanticModel::FindCompositionUnitIds(
		const EUnifiedHjsonCompositionKind Kind) const
	{
		return Compositions.Find(Kind);
	}

	int32 FUnifiedHjsonSemanticModel::GetBoundUnitCount() const
	{
		return BoundUnitCount;
	}

	const FString&
	FUnifiedHjsonSemanticModel::GetModelFingerprint() const
	{
		return ModelFingerprint;
	}

	bool FUnifiedHjsonSemanticModel::IsValid() const
	{
		return bValid;
	}

	bool TryCreateUnifiedHjsonSemanticModel(
		FAuthoringModelRef CoreModel,
		const EUnifiedHjsonAuthoringIntent AuthoringIntent,
		TMap<EUnifiedHjsonCompositionKind, TArray<FString>> Compositions,
		FUeAgentResourceTracker& Resources,
		FUnifiedHjsonSemanticModelPtr& OutModel,
		FWorkspaceError& OutError)
	{
		OutModel.Reset();
		OutError.Reset();
		if (!IsIntentValid(AuthoringIntent))
		{
			OutError.Set(TEXT("hjson_authoring_intent_invalid"));
			return false;
		}

		const TArray<EUnifiedHjsonCompositionKind> Kinds =
			GetAllUnifiedHjsonCompositionKinds();
		if (Compositions.Num() != Kinds.Num())
		{
			OutError.Set(
				TEXT("hjson_composition_set_incomplete"),
				FString::FromInt(Compositions.Num()));
			return false;
		}

		TSet<FString> BoundUnitIds;
		FString FingerprintMaterial =
			CoreModel->GetSemanticFingerprint()
			+ TEXT("|uai.unified_hjson_model/1|");
		int64 ReferenceCount = 0;
		for (const EUnifiedHjsonCompositionKind Kind : Kinds)
		{
			TArray<FString>* UnitIds = Compositions.Find(Kind);
			if (!UnitIds)
			{
				OutError.Set(
					TEXT("hjson_composition_missing"),
					LexToString(Kind));
				return false;
			}
			for (FString& UnitId : *UnitIds)
			{
				UnitId = Normalize(MoveTemp(UnitId));
			}
			UnitIds->Sort();
			for (int32 Index = 0; Index < UnitIds->Num(); ++Index)
			{
				const FString& UnitId = (*UnitIds)[Index];
				if (UnitId.IsEmpty()
					|| (Index > 0 && (*UnitIds)[Index - 1] == UnitId)
					|| BoundUnitIds.Contains(UnitId))
				{
					OutError.Set(
						TEXT("hjson_composition_unit_duplicate"),
						UnitId);
					return false;
				}
				const FSemanticUnit* Unit = CoreModel->FindUnit(UnitId);
				if (!Unit)
				{
					OutError.Set(
						TEXT("hjson_composition_unit_missing"),
						UnitId);
					return false;
				}
				if (!IsUnitKindAllowed(Kind, Unit->GetUnitKind()))
				{
					OutError.Set(
						TEXT("hjson_composition_unit_kind_mismatch"),
						UnitId);
					return false;
				}
				BoundUnitIds.Add(UnitId);
				FingerprintMaterial += LexToString(Kind)
					+ TEXT(":") + UnitId + TEXT(";");
				++ReferenceCount;
			}
		}

		if (BoundUnitIds.Num() != CoreModel->GetUnits().Num())
		{
			OutError.Set(
				TEXT("hjson_composition_unit_unbound"),
				FString::FromInt(
					CoreModel->GetUnits().Num() - BoundUnitIds.Num()));
			return false;
		}
		const TArray<FString>& TargetBindings =
			Compositions.FindChecked(EUnifiedHjsonCompositionKind::Target);
		for (const FString& TargetId : CoreModel->GetTargetUnitIds())
		{
			if (!TargetBindings.Contains(TargetId))
			{
				OutError.Set(
					TEXT("hjson_target_composition_mismatch"),
					TargetId);
				return false;
			}
		}

		FUeAgentResourceBudgetExceeded ResourceError;
		if (!Resources.TryChargeMany(
			{
				{EUeAgentResourceDimension::Nodes,
					CoreModel->GetUnits().Num()},
				{EUeAgentResourceDimension::ReferenceEdges, ReferenceCount},
				{EUeAgentResourceDimension::WorkUnits,
					CoreModel->GetUnits().Num() + Kinds.Num()},
			},
			TEXT("hjson.unified_model"),
			ResourceError))
		{
			return FailResource(OutError, ResourceError);
		}

		FUnifiedHjsonSemanticModelPtr Candidate =
			MakeShareable(new FUnifiedHjsonSemanticModel(
				MoveTemp(CoreModel),
				AuthoringIntent,
				MoveTemp(Compositions),
				BoundUnitIds.Num(),
				Sha256(FingerprintMaterial)));
		if (!Candidate->IsValid())
		{
			OutError.Set(TEXT("hjson_unified_model_invalid"));
			return false;
		}
		OutModel = MoveTemp(Candidate);
		return true;
	}
}
