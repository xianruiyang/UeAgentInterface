#include "Core/HjsonWorkspace/UeAgentExportFieldContract.h"

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		FString NormalizeRequired(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		FString OwnerSourceName(EExportFieldOwnerSource Source)
		{
			switch (Source)
			{
			case EExportFieldOwnerSource::Handwritten:
				return TEXT("handwritten");
			case EExportFieldOwnerSource::Reflection:
				return TEXT("reflection");
			default:
				return TEXT("unknown");
			}
		}

		bool IsValueKindValid(const FString& ValueKind)
		{
			return ValueKind == TEXT("null")
				|| ValueKind == TEXT("boolean")
				|| ValueKind == TEXT("string")
				|| ValueKind == TEXT("integer")
				|| ValueKind == TEXT("decimal")
				|| ValueKind == TEXT("array")
				|| ValueKind == TEXT("object")
				|| ValueKind == TEXT("temporal")
				|| ValueKind == TEXT("spatial");
		}

		FString ValueKindName(EUeAgentValueKind ValueKind)
		{
			switch (ValueKind)
			{
			case EUeAgentValueKind::Null: return TEXT("null");
			case EUeAgentValueKind::Boolean: return TEXT("boolean");
			case EUeAgentValueKind::String: return TEXT("string");
			case EUeAgentValueKind::Integer: return TEXT("integer");
			case EUeAgentValueKind::Decimal: return TEXT("decimal");
			case EUeAgentValueKind::Array: return TEXT("array");
			case EUeAgentValueKind::Object: return TEXT("object");
			case EUeAgentValueKind::Temporal: return TEXT("temporal");
			case EUeAgentValueKind::Spatial: return TEXT("spatial");
			default: return FString();
			}
		}

		bool Fail(
			FWorkspaceError& OutError,
			const FString& Code,
			const FString& SourceUnit = FString(),
			const FString& JsonPointer = TEXT("/"))
		{
			OutError.Set(Code, SourceUnit, JsonPointer);
			return false;
		}

		bool ShouldEmitWithoutComparison(
			const FUeAgentStateEntry& Entry,
			const FExportFieldContract& Field,
			bool& bOutRequiresDefaultComparison)
		{
			bOutRequiresDefaultComparison = false;
			const bool bAuthored =
				Entry.GetAuthoredPresence()
				== EUeAgentAuthoredPresence::Present;
			const bool bOverridden =
				Entry.GetOverrideState()
				== EUeAgentOverrideState::Overridden;
			switch (Field.GetEmissionPolicy())
			{
			case EExportFieldEmissionPolicy::Always:
				return true;
			case EExportFieldEmissionPolicy::WhenPresent:
			case EExportFieldEmissionPolicy::WhenAuthored:
				return bAuthored;
			case EExportFieldEmissionPolicy::WhenOverridden:
				return bOverridden;
			case EExportFieldEmissionPolicy::OnSelection:
				return false;
			case EExportFieldEmissionPolicy::WhenNonDefault:
				if (!Field.IsDefaultReproducible()
					|| bOverridden
					|| (Field.IsPresenceSensitive() && bAuthored))
				{
					return true;
				}
				bOutRequiresDefaultComparison = true;
				return false;
			default:
				return false;
			}
		}

		bool IsEntryCompatible(
			const FUeAgentStateEntry& Entry,
			const FExportFieldContract& Field,
			FWorkspaceError& OutError)
		{
			if (Entry.GetDefault().GetSourceKind()
				!= Field.GetDefaultSource())
			{
				return Fail(
					OutError,
					TEXT("field_default_source_mismatch"),
					Field.GetFieldId(),
					TEXT("/default/source_kind"));
			}
			if (ValueKindName(Entry.GetType().GetValueKind())
				!= Field.GetValueKind())
			{
				return Fail(
					OutError,
					TEXT("field_value_kind_mismatch"),
					Field.GetFieldId(),
					TEXT("/type/value_kind"));
			}
			return true;
		}
	}

	FExportFieldContract::FExportFieldContract(
		FString InContractId,
		int32 InContractVersion,
		FString InFieldId,
		FString InExternalName,
		FString InOwnerSemanticId,
		FString InValueContractRef,
		FString InSemanticRole,
		FString InRoundTripRequirement,
		TArray<FString> InProfileVisibility,
		bool bInPresenceSensitive,
		EExportFieldEmissionPolicy InEmissionPolicy,
		EUeAgentDefaultSourceKind InDefaultSource,
		bool bInDefaultReproducible,
		FString InFieldSetId,
		FString InExposureReason,
		FString InDisplayName,
		FString InTooltip,
		FString InValueKind)
		: ContractId(NormalizeRequired(MoveTemp(InContractId)))
		, ContractVersion(InContractVersion)
		, FieldId(NormalizeRequired(MoveTemp(InFieldId)))
		, ExternalName(NormalizeRequired(MoveTemp(InExternalName)))
		, OwnerSemanticId(NormalizeRequired(MoveTemp(InOwnerSemanticId)))
		, ValueContractRef(NormalizeRequired(MoveTemp(InValueContractRef)))
		, SemanticRole(NormalizeRequired(MoveTemp(InSemanticRole)))
		, RoundTripRequirement(
			NormalizeRequired(MoveTemp(InRoundTripRequirement)))
		, ProfileVisibility(MoveTemp(InProfileVisibility))
		, bPresenceSensitive(bInPresenceSensitive)
		, EmissionPolicy(InEmissionPolicy)
		, DefaultSource(InDefaultSource)
		, bDefaultReproducible(bInDefaultReproducible)
		, FieldSetId(NormalizeRequired(MoveTemp(InFieldSetId)))
		, ExposureReason(NormalizeRequired(MoveTemp(InExposureReason)))
		, DisplayName(NormalizeRequired(MoveTemp(InDisplayName)))
		, Tooltip(MoveTemp(InTooltip))
		, ValueKind(NormalizeRequired(MoveTemp(InValueKind)))
	{
		Tooltip.TrimStartAndEndInline();
		for (FString& Visibility : ProfileVisibility)
		{
			Visibility = NormalizeRequired(MoveTemp(Visibility));
		}
		ProfileVisibility.Sort();
		bool bUniqueVisibility = true;
		for (int32 Index = 1; Index < ProfileVisibility.Num(); ++Index)
		{
			if (ProfileVisibility[Index - 1] == ProfileVisibility[Index])
			{
				bUniqueVisibility = false;
				break;
			}
		}
		bValid = !ContractId.IsEmpty()
			&& ContractVersion > 0
			&& !FieldId.IsEmpty()
			&& !ExternalName.IsEmpty()
			&& !OwnerSemanticId.IsEmpty()
			&& !ValueContractRef.IsEmpty()
			&& !SemanticRole.IsEmpty()
			&& !RoundTripRequirement.IsEmpty()
			&& !ProfileVisibility.IsEmpty()
			&& !FieldSetId.IsEmpty()
			&& !ExposureReason.IsEmpty()
			&& !DisplayName.IsEmpty()
			&& IsValueKindValid(ValueKind)
			&& bUniqueVisibility;
	}

	const FString& FExportFieldContract::GetContractId() const { return ContractId; }
	int32 FExportFieldContract::GetContractVersion() const { return ContractVersion; }
	const FString& FExportFieldContract::GetFieldId() const { return FieldId; }
	const FString& FExportFieldContract::GetExternalName() const { return ExternalName; }
	const FString& FExportFieldContract::GetOwnerSemanticId() const
	{
		return OwnerSemanticId;
	}
	const FString& FExportFieldContract::GetValueContractRef() const
	{
		return ValueContractRef;
	}
	const FString& FExportFieldContract::GetSemanticRole() const
	{
		return SemanticRole;
	}
	const FString& FExportFieldContract::GetRoundTripRequirement() const
	{
		return RoundTripRequirement;
	}
	const TArray<FString>& FExportFieldContract::GetProfileVisibility() const
	{
		return ProfileVisibility;
	}
	bool FExportFieldContract::IsPresenceSensitive() const
	{
		return bPresenceSensitive;
	}
	EExportFieldEmissionPolicy FExportFieldContract::GetEmissionPolicy() const
	{
		return EmissionPolicy;
	}
	EUeAgentDefaultSourceKind FExportFieldContract::GetDefaultSource() const
	{
		return DefaultSource;
	}
	bool FExportFieldContract::IsDefaultReproducible() const
	{
		return bDefaultReproducible;
	}
	const FString& FExportFieldContract::GetFieldSetId() const { return FieldSetId; }
	const FString& FExportFieldContract::GetExposureReason() const
	{
		return ExposureReason;
	}
	const FString& FExportFieldContract::GetDisplayName() const { return DisplayName; }
	const FString& FExportFieldContract::GetTooltip() const { return Tooltip; }
	const FString& FExportFieldContract::GetValueKind() const { return ValueKind; }
	bool FExportFieldContract::IsValid() const { return bValid; }

	FExportFieldRegistration::FExportFieldRegistration(
		FExportFieldContract InField,
		EExportFieldOwnerSource InProviderSource,
		FString InProviderId)
		: Field(MoveTemp(InField))
		, ProviderSource(InProviderSource)
		, ProviderId(NormalizeRequired(MoveTemp(InProviderId)))
	{
		bValid = Field.IsValid() && !ProviderId.IsEmpty();
	}

	const FExportFieldContract& FExportFieldRegistration::GetField() const
	{
		return Field;
	}
	EExportFieldOwnerSource FExportFieldRegistration::GetProviderSource() const
	{
		return ProviderSource;
	}
	const FString& FExportFieldRegistration::GetProviderId() const
	{
		return ProviderId;
	}
	bool FExportFieldRegistration::IsValid() const { return bValid; }

	FExportFieldRegistry::FExportFieldRegistry(
		FString InContractId,
		int32 InContractVersion,
		TArray<FExportFieldRegistration> InRegistrations)
		: ContractId(NormalizeRequired(MoveTemp(InContractId)))
		, ContractVersion(InContractVersion)
		, Registrations(MoveTemp(InRegistrations))
	{
		Registrations.Sort(
			[](const FExportFieldRegistration& Left,
				const FExportFieldRegistration& Right)
			{
				return Left.GetField().GetFieldId()
					< Right.GetField().GetFieldId();
			});
		bValid = !ContractId.IsEmpty()
			&& ContractVersion > 0;
		for (int32 Index = 0; Index < Registrations.Num(); ++Index)
		{
			const FExportFieldRegistration& Registration =
				Registrations[Index];
			if (!Registration.IsValid()
				|| IndexByFieldId.Contains(
					Registration.GetField().GetFieldId()))
			{
				bValid = false;
				break;
			}
			IndexByFieldId.Add(
				Registration.GetField().GetFieldId(),
				Index);
		}
	}

	const FString& FExportFieldRegistry::GetContractId() const { return ContractId; }
	int32 FExportFieldRegistry::GetContractVersion() const { return ContractVersion; }
	const TArray<FExportFieldRegistration>&
	FExportFieldRegistry::GetRegistrations() const
	{
		return Registrations;
	}
	const FExportFieldRegistration* FExportFieldRegistry::FindField(
		const FString& FieldId) const
	{
		const int32* Index = IndexByFieldId.Find(FieldId);
		return Index == nullptr ? nullptr : &Registrations[*Index];
	}
	const FExportFieldRegistration*
	FExportFieldRegistry::FindFieldByExternalName(
		const FString& OwnerSemanticId,
		const FString& FieldSetId,
		const FString& ExternalName) const
	{
		const FString NormalizedOwner =
			NormalizeRequired(OwnerSemanticId);
		const FString NormalizedFieldSet =
			NormalizeRequired(FieldSetId);
		const FString NormalizedExternalName =
			NormalizeRequired(ExternalName);
		if (NormalizedOwner.IsEmpty()
			|| NormalizedFieldSet.IsEmpty()
			|| NormalizedExternalName.IsEmpty())
		{
			return nullptr;
		}
		for (const FExportFieldRegistration& Registration : Registrations)
		{
			const FExportFieldContract& Field = Registration.GetField();
			if (Field.GetOwnerSemanticId() == NormalizedOwner
				&& Field.GetFieldSetId() == NormalizedFieldSet
				&& Field.GetExternalName() == NormalizedExternalName)
			{
				return &Registration;
			}
		}
		return nullptr;
	}
	bool FExportFieldRegistry::IsValid() const { return bValid; }

	FExportFieldDescription::FExportFieldDescription(
		const FExportFieldRegistration& Registration)
		: FieldId(Registration.GetField().GetFieldId())
		, ExternalName(Registration.GetField().GetExternalName())
		, ValueContractRef(Registration.GetField().GetValueContractRef())
		, DisplayName(Registration.GetField().GetDisplayName())
		, Tooltip(Registration.GetField().GetTooltip())
		, ValueKind(Registration.GetField().GetValueKind())
		, ProviderSource(Registration.GetProviderSource())
		, ProviderId(Registration.GetProviderId())
		, bValid(Registration.IsValid())
	{
	}

	const FString& FExportFieldDescription::GetFieldId() const { return FieldId; }
	const FString& FExportFieldDescription::GetExternalName() const
	{
		return ExternalName;
	}
	const FString& FExportFieldDescription::GetValueContractRef() const
	{
		return ValueContractRef;
	}
	const FString& FExportFieldDescription::GetDisplayName() const
	{
		return DisplayName;
	}
	const FString& FExportFieldDescription::GetTooltip() const { return Tooltip; }
	const FString& FExportFieldDescription::GetValueKind() const { return ValueKind; }
	EExportFieldOwnerSource FExportFieldDescription::GetProviderSource() const
	{
		return ProviderSource;
	}
	const FString& FExportFieldDescription::GetProviderId() const
	{
		return ProviderId;
	}
	bool FExportFieldDescription::IsValid() const { return bValid; }

	FPropertySetProjection::FPropertySetProjection(
		FString InOwnerSemanticId,
		FString InFieldSetId,
		TMap<FString, FUeAgentCanonicalValueRef> InValues,
		TArray<FString> InEmittedFieldIds)
		: OwnerSemanticId(NormalizeRequired(MoveTemp(InOwnerSemanticId)))
		, FieldSetId(NormalizeRequired(MoveTemp(InFieldSetId)))
		, Values(MoveTemp(InValues))
		, EmittedFieldIds(MoveTemp(InEmittedFieldIds))
	{
		EmittedFieldIds.Sort();
		bValid = !OwnerSemanticId.IsEmpty()
			&& !FieldSetId.IsEmpty()
			&& EmittedFieldIds.Num() == Values.Num();
	}

	const FString& FPropertySetProjection::GetOwnerSemanticId() const
	{
		return OwnerSemanticId;
	}
	const FString& FPropertySetProjection::GetFieldSetId() const
	{
		return FieldSetId;
	}
	const TMap<FString, FUeAgentCanonicalValueRef>&
	FPropertySetProjection::GetValues() const
	{
		return Values;
	}
	const TArray<FString>& FPropertySetProjection::GetEmittedFieldIds() const
	{
		return EmittedFieldIds;
	}
	const TMap<FString, FExportFieldDescription>&
	FPropertySetProjection::GetDescriptions() const
	{
		return Descriptions;
	}
	bool FPropertySetProjection::IsValid() const { return bValid; }

	bool TryCompileExportFieldRegistry(
		const TArray<FExportFieldRegistration>& Registrations,
		const FString& ContractId,
		int32 ContractVersion,
		FExportFieldRegistryPtr& OutRegistry,
		FWorkspaceError& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		const FString NormalizedContractId =
			NormalizeRequired(ContractId);
		if (NormalizedContractId.IsEmpty()
			|| ContractVersion <= 0)
		{
			return Fail(
				OutError,
				TEXT("field_registry_contract_invalid"),
				NormalizedContractId);
		}

		TMap<FString, const FExportFieldRegistration*> ByFieldId;
		TMap<FString, const FExportFieldRegistration*> ByExternalKey;
		for (const FExportFieldRegistration& Registration : Registrations)
		{
			if (!Registration.IsValid())
			{
				return Fail(
					OutError,
					TEXT("field_registration_invalid"),
					Registration.GetProviderId());
			}
			const FExportFieldContract& Field = Registration.GetField();
			if (const FExportFieldRegistration* const* Existing =
				ByFieldId.Find(Field.GetFieldId()))
			{
				const FString SourceUnit = FString::Printf(
					TEXT("%s|%s:%s|%s:%s"),
					*Field.GetFieldId(),
					*OwnerSourceName((*Existing)->GetProviderSource()),
					*(*Existing)->GetProviderId(),
					*OwnerSourceName(Registration.GetProviderSource()),
					*Registration.GetProviderId());
				return Fail(
					OutError,
					TEXT("field_owner_conflict"),
					SourceUnit);
			}
			const FString ExternalKey = FString::Printf(
				TEXT("%s|%s|%s"),
				*Field.GetOwnerSemanticId(),
				*Field.GetFieldSetId(),
				*Field.GetExternalName());
			if (const FExportFieldRegistration* const* Existing =
				ByExternalKey.Find(ExternalKey))
			{
				return Fail(
					OutError,
					TEXT("field_external_name_conflict"),
					FString::Printf(
						TEXT("%s|%s|%s"),
						*ExternalKey,
						*(*Existing)->GetField().GetFieldId(),
						*Field.GetFieldId()));
			}
			ByFieldId.Add(Field.GetFieldId(), &Registration);
			ByExternalKey.Add(ExternalKey, &Registration);
		}

		FExportFieldRegistryPtr Candidate =
			MakeShared<const FExportFieldRegistry>(
				NormalizedContractId,
				ContractVersion,
				Registrations);
		if (!Candidate->IsValid())
		{
			return Fail(
				OutError,
				TEXT("field_registry_invalid"),
				NormalizedContractId);
		}
		OutRegistry = MoveTemp(Candidate);
		return true;
	}

	bool TryProjectPropertySet(
		const FExportFieldRegistryRef& Registry,
		const FUeAgentCanonicalSnapshotRef& Snapshot,
		const FString& OwnerSemanticId,
		const FString& FieldSetId,
		FUeAgentResourceTracker& Resources,
		FPropertySetProjectionPtr& OutProjection,
		FWorkspaceError& OutError)
	{
		return TryProjectPropertySet(
			Registry,
			Snapshot,
			OwnerSemanticId,
			OwnerSemanticId,
			FieldSetId,
			Resources,
			OutProjection,
			OutError);
	}

	bool TryProjectPropertySet(
		const FExportFieldRegistryRef& Registry,
		const FUeAgentCanonicalSnapshotRef& Snapshot,
		const FString& StateOwnerId,
		const FString& OwnerSemanticId,
		const FString& FieldSetId,
		FUeAgentResourceTracker& Resources,
		FPropertySetProjectionPtr& OutProjection,
		FWorkspaceError& OutError)
	{
		OutProjection.Reset();
		OutError.Reset();
		const FString NormalizedStateOwner =
			NormalizeRequired(StateOwnerId);
		const FString NormalizedOwner =
			NormalizeRequired(OwnerSemanticId);
		const FString NormalizedFieldSet =
			NormalizeRequired(FieldSetId);
		if (!Registry->IsValid()
			|| NormalizedStateOwner.IsEmpty()
			|| NormalizedOwner.IsEmpty()
			|| NormalizedFieldSet.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("property_projection_contract_invalid"),
				NormalizedOwner);
		}

		TMap<FString, FUeAgentCanonicalValueRef> Values;
		TArray<FString> EmittedFieldIds;
		TSet<FString> SeenFieldIds;
		for (const FUeAgentStateEntry& Entry : Snapshot->GetEntries())
		{
			if (Entry.GetIdentity().GetOwnerId()
				!= NormalizedStateOwner)
			{
				continue;
			}
			const FString& FieldId =
				Entry.GetIdentity().GetSemanticId();
			const FExportFieldRegistration* Registration =
				Registry->FindField(FieldId);
			if (Registration == nullptr)
			{
				continue;
			}
			const FExportFieldContract& Field =
				Registration->GetField();
			if (Field.GetOwnerSemanticId() != NormalizedOwner
				|| Field.GetFieldSetId() != NormalizedFieldSet)
			{
				continue;
			}
			if (SeenFieldIds.Contains(FieldId))
			{
				return Fail(
					OutError,
					TEXT("field_state_duplicate"),
					FieldId);
			}
			SeenFieldIds.Add(FieldId);
			if (!IsEntryCompatible(Entry, Field, OutError))
			{
				return false;
			}

			bool bRequiresDefaultComparison = false;
			bool bEmit = ShouldEmitWithoutComparison(
				Entry,
				Field,
				bRequiresDefaultComparison);
			if (bRequiresDefaultComparison)
			{
				FUeAgentSemanticComparison Comparison;
				FUeAgentStateSemanticError StateError;
				if (!TryCompareValues(
					Entry.GetValue(),
					Entry.GetDefault().GetValue(),
					TEXT("/property_projection/default"),
					Resources,
					Comparison,
					StateError))
				{
					OutError.Set(
						StateError.Code,
						StateError.Path,
						TEXT("/property_projection/default"),
						StateError.ResourceObserved,
						true);
					return false;
				}
				bEmit = !Comparison.IsEquivalent();
			}
			if (!bEmit)
			{
				continue;
			}
			if (Values.Contains(Field.GetExternalName()))
			{
				return Fail(
					OutError,
					TEXT("property_map_key_conflict"),
					Field.GetExternalName());
			}
			Values.Add(Field.GetExternalName(), Entry.GetValue());
			EmittedFieldIds.Add(FieldId);
		}

		FUeAgentResourceBudgetExceeded BudgetError;
		if (!Resources.TryChargeMany(
			{
				{EUeAgentResourceDimension::Nodes, 1},
				{EUeAgentResourceDimension::Items, Values.Num()},
				{EUeAgentResourceDimension::WorkUnits, Values.Num() + 1},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					64 + Values.Num() * 48},
			},
			TEXT("/property_projection"),
			BudgetError))
		{
			OutError.Set(
				BudgetError.Code,
				BudgetError.Path,
				TEXT("/property_projection"),
				BudgetError.Observed,
				true);
			return false;
		}

		FPropertySetProjectionPtr Candidate =
			MakeShared<const FPropertySetProjection>(
				NormalizedOwner,
				NormalizedFieldSet,
				MoveTemp(Values),
				MoveTemp(EmittedFieldIds));
		if (!Candidate->IsValid())
		{
			return Fail(
				OutError,
				TEXT("property_projection_invalid"),
				NormalizedOwner);
		}
		OutProjection = MoveTemp(Candidate);
		return true;
	}

	bool TryDescribeExportField(
		const FExportFieldRegistryRef& Registry,
		const FString& FieldId,
		FExportFieldDescription& OutDescription,
		FWorkspaceError& OutError)
	{
		OutDescription = FExportFieldDescription();
		OutError.Reset();
		const FString NormalizedFieldId =
			NormalizeRequired(FieldId);
		const FExportFieldRegistration* Registration =
			Registry->FindField(NormalizedFieldId);
		if (Registration == nullptr)
		{
			return Fail(
				OutError,
				TEXT("field_contract_not_found"),
				NormalizedFieldId);
		}
		OutDescription = FExportFieldDescription(*Registration);
		return OutDescription.IsValid();
	}
}
