#include "Core/HjsonWorkspace/UeAgentExportFieldContract.h"
#include "Dom/JsonValue.h"

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

		Contract::FDiagnostic FieldDiagnostic(const FString& Code, const FString& Reason,
			const FString& Field = FString(), const FString& Pointer = TEXT("/"),
			TSharedPtr<FJsonValue> Actual = nullptr, TSharedPtr<FJsonValue> Expected = nullptr)
		{
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("export_field_contract"));
			Error.PropertyPath = Field;
			Error.SemanticPointer = Pointer;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		bool Fail(FWorkspaceError& OutError, const FString& Code, const FString& Reason,
			const FString& Field = FString(), const FString& Pointer = TEXT("/"),
			TSharedPtr<FJsonValue> Actual = nullptr, TSharedPtr<FJsonValue> Expected = nullptr)
		{
			OutError.SetFailure(FieldDiagnostic(Code, Reason, Field, Pointer, MoveTemp(Actual), MoveTemp(Expected)));
			return false;
		}

		const Contract::FDiagnostic& ValidationOrEmpty(const TSharedPtr<const Contract::FDiagnostic>& Error)
		{
			static const Contract::FDiagnostic Empty;
			return Error ? *Error : Empty;
		}

		FString DefaultSourceName(EUeAgentDefaultSourceKind Kind)
		{
			switch (Kind)
			{
			case EUeAgentDefaultSourceKind::Factory: return TEXT("factory");
			case EUeAgentDefaultSourceKind::ClassDefaultObject: return TEXT("class_default_object");
			case EUeAgentDefaultSourceKind::Parent: return TEXT("parent");
			case EUeAgentDefaultSourceKind::Constructor: return TEXT("constructor");
			case EUeAgentDefaultSourceKind::StructInitializer: return TEXT("struct_initializer");
			case EUeAgentDefaultSourceKind::Schema: return TEXT("schema");
			case EUeAgentDefaultSourceKind::DomainNative: return TEXT("domain_native");
			case EUeAgentDefaultSourceKind::PostCreateReadback: return TEXT("post_create_readback");
			default: return FString::FromInt(static_cast<int32>(Kind));
			}
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
					TEXT("field_default_source_mismatch"), TEXT("The captured default source does not match the field contract."),
					Field.GetFieldId(), TEXT("/default/source_kind"),
					MakeShared<FJsonValueString>(DefaultSourceName(Entry.GetDefault().GetSourceKind())),
					MakeShared<FJsonValueString>(DefaultSourceName(Field.GetDefaultSource())));
			}
			if (ValueKindName(Entry.GetType().GetValueKind())
				!= Field.GetValueKind())
			{
				return Fail(
					OutError,
					TEXT("field_value_kind_mismatch"), TEXT("The captured value kind does not match the field contract."),
					Field.GetFieldId(), TEXT("/type/kind"),
					MakeShared<FJsonValueString>(ValueKindName(Entry.GetType().GetValueKind())),
					MakeShared<FJsonValueString>(Field.GetValueKind()));
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
		const auto Invalid = [&](const FString& Reason, const FString& Path, TSharedPtr<FJsonValue> Actual = nullptr)
		{
			if (!ValidationDiagnostic) ValidationDiagnostic = MakeShared<const Contract::FDiagnostic>(
				FieldDiagnostic(TEXT("field_contract_invalid"), Reason, FieldId, Path, MoveTemp(Actual)));
		};
		const TArray<TPair<FString, FString>> RequiredFields = {
			{TEXT("contract_id"), ContractId}, {TEXT("field_id"), FieldId}, {TEXT("external_name"), ExternalName},
			{TEXT("owner_semantic_id"), OwnerSemanticId}, {TEXT("value_contract_ref"), ValueContractRef},
			{TEXT("semantic_role"), SemanticRole}, {TEXT("round_trip_requirement"), RoundTripRequirement},
			{TEXT("field_set_id"), FieldSetId}, {TEXT("exposure_reason"), ExposureReason}, {TEXT("display_name"), DisplayName}};
		for (const auto& Field : RequiredFields)
			if (Field.Value.IsEmpty()) { Invalid(FString::Printf(TEXT("Required field contract member '%s' is empty."), *Field.Key), TEXT("/") + Field.Key); break; }
		if (ContractVersion <= 0) Invalid(TEXT("The field contract version must be greater than zero."), TEXT("/contract_version"), MakeShared<FJsonValueNumber>(ContractVersion));
		if (ProfileVisibility.IsEmpty()) Invalid(TEXT("The field contract must declare at least one visible profile."), TEXT("/profile_visibility"));
		if (!IsValueKindValid(ValueKind)) Invalid(TEXT("The field contract declares an unsupported value kind."), TEXT("/value_kind"), MakeShared<FJsonValueString>(ValueKind));
		for (int32 Index = 1; Index < ProfileVisibility.Num(); ++Index)
			if (ProfileVisibility[Index - 1] == ProfileVisibility[Index])
			{ Invalid(TEXT("The field contract repeats a visible profile."), FString::Printf(TEXT("/profile_visibility/%d"), Index), MakeShared<FJsonValueString>(ProfileVisibility[Index])); break; }
		bValid = !ValidationDiagnostic;
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
	const Contract::FDiagnostic& FExportFieldContract::GetValidationDiagnostic() const { return ValidationOrEmpty(ValidationDiagnostic); }

	FExportFieldRegistration::FExportFieldRegistration(
		FExportFieldContract InField,
		EExportFieldOwnerSource InProviderSource,
		FString InProviderId)
		: Field(MoveTemp(InField))
		, ProviderSource(InProviderSource)
		, ProviderId(NormalizeRequired(MoveTemp(InProviderId)))
	{
		if (!Field.IsValid()) ValidationDiagnostic = MakeShared<const Contract::FDiagnostic>(Field.GetValidationDiagnostic());
		else if (ProviderId.IsEmpty()) ValidationDiagnostic = MakeShared<const Contract::FDiagnostic>(FieldDiagnostic(
			TEXT("field_registration_invalid"), TEXT("The field registration has no provider identifier."), Field.GetFieldId(), TEXT("/provider_id")));
		bValid = !ValidationDiagnostic;
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
	const Contract::FDiagnostic& FExportFieldRegistration::GetValidationDiagnostic() const { return ValidationOrEmpty(ValidationDiagnostic); }

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
		bValid = !ContractId.IsEmpty() && ContractVersion > 0;
		if (!bValid) ValidationDiagnostic = MakeShared<const Contract::FDiagnostic>(FieldDiagnostic(TEXT("field_registry_contract_invalid"),
			ContractId.IsEmpty() ? TEXT("The export registry contract identifier is empty.") : TEXT("The export registry contract version must be greater than zero."),
			FString(), ContractId.IsEmpty() ? TEXT("/contract_id") : TEXT("/contract_version"),
			ContractId.IsEmpty() ? TSharedPtr<FJsonValue>() : MakeShared<FJsonValueNumber>(ContractVersion)));
		for (int32 Index = 0; Index < Registrations.Num(); ++Index)
		{
			const FExportFieldRegistration& Registration =
				Registrations[Index];
			if (!Registration.IsValid()
				|| IndexByFieldId.Contains(
					Registration.GetField().GetFieldId()))
			{
				bValid = false;
				if (!ValidationDiagnostic) ValidationDiagnostic = MakeShared<const Contract::FDiagnostic>(
					!Registration.IsValid() ? Registration.GetValidationDiagnostic()
					: FieldDiagnostic(TEXT("field_owner_conflict"), TEXT("More than one provider is registered for this field."), Registration.GetField().GetFieldId()));
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
	const Contract::FDiagnostic& FExportFieldRegistry::GetValidationDiagnostic() const { return ValidationOrEmpty(ValidationDiagnostic); }

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
				TEXT("field_registry_contract_invalid"), NormalizedContractId.IsEmpty() ? TEXT("The export registry contract identifier is empty.") : FString::Printf(TEXT("The export registry contract version must be greater than zero; received %d."), ContractVersion),
				NormalizedContractId);
		}

		TMap<FString, const FExportFieldRegistration*> ByFieldId;
		TMap<FString, const FExportFieldRegistration*> ByExternalKey;
		for (const FExportFieldRegistration& Registration : Registrations)
		{
			if (!Registration.IsValid())
			{
				OutError.SetFailure(Registration.GetValidationDiagnostic());
				return false;
			}
			const FExportFieldContract& Field = Registration.GetField();
			if (const FExportFieldRegistration* const* Existing =
				ByFieldId.Find(Field.GetFieldId()))
			{
				return Fail(OutError, TEXT("field_owner_conflict"),
					FString::Printf(TEXT("Field '%s' is registered by both %s provider '%s' and %s provider '%s'."),
						*Field.GetFieldId(), *OwnerSourceName((*Existing)->GetProviderSource()), *(*Existing)->GetProviderId(),
						*OwnerSourceName(Registration.GetProviderSource()), *Registration.GetProviderId()), Field.GetFieldId());
			}
			const FString ExternalKey = FString::Printf(
				TEXT("%s|%s|%s"),
				*Field.GetOwnerSemanticId(),
				*Field.GetFieldSetId(),
				*Field.GetExternalName());
			if (const FExportFieldRegistration* const* Existing =
				ByExternalKey.Find(ExternalKey))
			{
				return Fail(OutError, TEXT("field_external_name_conflict"),
					FString::Printf(TEXT("Fields '%s' and '%s' both export the name '%s' in field set '%s'."),
						*(*Existing)->GetField().GetFieldId(), *Field.GetFieldId(), *Field.GetExternalName(), *Field.GetFieldSetId()), Field.GetFieldId());
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
			OutError.SetFailure(Candidate->GetValidationDiagnostic());
			return false;
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
		if (!Registry->IsValid()) { OutError.SetFailure(Registry->GetValidationDiagnostic()); return false; }
		if (NormalizedStateOwner.IsEmpty()
			|| NormalizedOwner.IsEmpty()
			|| NormalizedFieldSet.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("property_projection_contract_invalid"), NormalizedStateOwner.IsEmpty() ? TEXT("Property projection requires a captured state owner.") : NormalizedOwner.IsEmpty() ? TEXT("Property projection requires a semantic owner.") : TEXT("Property projection requires a field set."),
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
					TEXT("field_state_duplicate"), TEXT("More than one captured state entry resolves to this export field."),
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
					OutError.SetFailure(StateError, FString(), TEXT("/property_projection/default"));
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
					TEXT("property_map_key_conflict"), TEXT("More than one field exports the same property name."),
					Field.GetExternalName());
			}
			Values.Add(Field.GetExternalName(), Entry.GetValue());
			EmittedFieldIds.Add(FieldId);
		}

		FUeAgentResourceChargeError ChargeError;
		if (!Resources.TryChargeMany(
			{
				{EUeAgentResourceDimension::Nodes, 1},
				{EUeAgentResourceDimension::Items, Values.Num()},
				{EUeAgentResourceDimension::WorkUnits, Values.Num() + 1},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					64 + Values.Num() * 48},
			},
			TEXT("/property_projection"),
			ChargeError))
		{
			OutError.SetFailure(ChargeError, FString(), TEXT("/property_projection"));
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
				TEXT("property_projection_invalid"), FString::Printf(TEXT("The projection contains %d emitted fields but %d property values."), Candidate->GetEmittedFieldIds().Num(), Candidate->GetValues().Num()),
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
				TEXT("field_contract_not_found"), TEXT("No export contract is registered for this field."),
				NormalizedFieldId);
		}
		if (!Registration->IsValid()) { OutError.SetFailure(Registration->GetValidationDiagnostic()); return false; }
		OutDescription = FExportFieldDescription(*Registration);
		return true;
	}
}
