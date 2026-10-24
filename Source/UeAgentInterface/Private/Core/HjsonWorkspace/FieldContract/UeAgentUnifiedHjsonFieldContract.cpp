#include "Core/HjsonWorkspace/FieldContract/UeAgentUnifiedHjsonFieldContract.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::HjsonWorkspace
{
	namespace
	{
		void FieldFailure(FWorkspaceError& OutError, const FString& Code, const FString& Reason,
			const FString& Field = FString(), TSharedPtr<FJsonValue> Actual = nullptr,
			TSharedPtr<FJsonValue> Expected = nullptr)
		{
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("hjson_field_contract"));
			Error.PropertyPath = Field;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			OutError.SetFailure(MoveTemp(Error));
		}

		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		bool IsIntentValueValid(const EUnifiedHjsonFieldIntent Intent)
		{
			switch (Intent)
			{
			case EUnifiedHjsonFieldIntent::Preserve:
			case EUnifiedHjsonFieldIntent::SetValue:
			case EUnifiedHjsonFieldIntent::SetNull:
			case EUnifiedHjsonFieldIntent::RestoreDefault:
			case EUnifiedHjsonFieldIntent::Remove:
				return true;
			default:
				return false;
			}
		}
	}

	FString LexToString(const EUnifiedHjsonFieldIntent Intent)
	{
		switch (Intent)
		{
		case EUnifiedHjsonFieldIntent::Preserve:
			return TEXT("preserve");
		case EUnifiedHjsonFieldIntent::SetValue:
			return TEXT("set_value");
		case EUnifiedHjsonFieldIntent::SetNull:
			return TEXT("set_null");
		case EUnifiedHjsonFieldIntent::RestoreDefault:
			return TEXT("restore_default");
		case EUnifiedHjsonFieldIntent::Remove:
			return TEXT("remove");
		default:
			return FString();
		}
	}

	FUnifiedHjsonFieldContract::FUnifiedHjsonFieldContract(
		FString InFieldId,
		const bool bInNullable,
		TArray<EUnifiedHjsonFieldIntent> InAllowedIntents)
		: FieldId(Normalize(MoveTemp(InFieldId)))
		, bNullable(bInNullable)
		, AllowedIntents(MoveTemp(InAllowedIntents))
	{
		AllowedIntents.Sort(
			[](const EUnifiedHjsonFieldIntent Left,
				const EUnifiedHjsonFieldIntent Right)
			{
				return static_cast<uint8>(Left) < static_cast<uint8>(Right);
			});
		bValid = !FieldId.IsEmpty() && !AllowedIntents.IsEmpty();
		for (int32 Index = 0; Index < AllowedIntents.Num(); ++Index)
		{
			if (!IsIntentValueValid(AllowedIntents[Index])
				|| (Index > 0
					&& AllowedIntents[Index - 1] == AllowedIntents[Index]))
			{
				bValid = false;
				break;
			}
		}
	}

	const FString& FUnifiedHjsonFieldContract::GetFieldId() const
	{
		return FieldId;
	}

	bool FUnifiedHjsonFieldContract::IsNullable() const
	{
		return bNullable;
	}

	const TArray<EUnifiedHjsonFieldIntent>&
	FUnifiedHjsonFieldContract::GetAllowedIntents() const
	{
		return AllowedIntents;
	}

	bool FUnifiedHjsonFieldContract::Allows(
		const EUnifiedHjsonFieldIntent Intent) const
	{
		return AllowedIntents.Contains(Intent);
	}

	bool FUnifiedHjsonFieldContract::IsValid() const
	{
		return bValid;
	}

	FUnifiedHjsonFieldRegistry::FUnifiedHjsonFieldRegistry(
		FExportFieldRegistryRef InBaseRegistry,
		TArray<FUnifiedHjsonFieldContract> InContracts)
		: BaseRegistry(MoveTemp(InBaseRegistry))
		, Contracts(MoveTemp(InContracts))
	{
		for (int32 Index = 0; Index < Contracts.Num(); ++Index)
		{
			ContractIndexByFieldId.Add(Contracts[Index].GetFieldId(), Index);
		}
		bValid = BaseRegistry->IsValid()
			&& Contracts.Num() == BaseRegistry->GetRegistrations().Num()
			&& ContractIndexByFieldId.Num() == Contracts.Num();
	}

	const FExportFieldRegistryRef&
	FUnifiedHjsonFieldRegistry::GetBaseRegistry() const
	{
		return BaseRegistry;
	}

	const TArray<FUnifiedHjsonFieldContract>&
	FUnifiedHjsonFieldRegistry::GetContracts() const
	{
		return Contracts;
	}

	const FUnifiedHjsonFieldContract*
	FUnifiedHjsonFieldRegistry::FindContract(const FString& FieldId) const
	{
		const int32* Index = ContractIndexByFieldId.Find(FieldId);
		return Index ? &Contracts[*Index] : nullptr;
	}

	bool FUnifiedHjsonFieldRegistry::IsValid() const
	{
		return bValid;
	}

	bool TryCompileUnifiedHjsonFieldRegistry(
		FExportFieldRegistryRef BaseRegistry,
		TArray<FUnifiedHjsonFieldContract> Contracts,
		FUnifiedHjsonFieldRegistryPtr& OutRegistry,
		FWorkspaceError& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		if (!BaseRegistry->IsValid())
		{
			OutError.SetFailure(BaseRegistry->GetValidationDiagnostic());
			return false;
		}

		TSet<FString> Seen;
		for (const FUnifiedHjsonFieldContract& Contract : Contracts)
		{
			if (!Contract.IsValid())
			{
				FString Reason = Contract.GetFieldId().IsEmpty()
					? TEXT("The HJSON field contract has an empty field identifier.")
					: TEXT("The HJSON field contract declares no allowed intents.");
				TSet<EUnifiedHjsonFieldIntent> SeenIntents;
				for (const auto Intent : Contract.GetAllowedIntents())
				{
					if (!IsIntentValueValid(Intent)) { Reason = FString::Printf(TEXT("The HJSON field contract declares invalid intent value %d."), static_cast<int32>(Intent)); break; }
					if (SeenIntents.Contains(Intent)) { Reason = FString::Printf(TEXT("The HJSON field contract repeats intent '%s'."), *LexToString(Intent)); break; }
					SeenIntents.Add(Intent);
				}
				FieldFailure(OutError, TEXT("hjson_field_contract_invalid"), Reason, Contract.GetFieldId());
				return false;
			}
			if (Seen.Contains(Contract.GetFieldId()))
			{
				FieldFailure(OutError, TEXT("hjson_field_contract_duplicate"), TEXT("More than one HJSON contract is registered for this field."), Contract.GetFieldId());
				return false;
			}
			Seen.Add(Contract.GetFieldId());
			const FExportFieldRegistration* Registration =
				BaseRegistry->FindField(Contract.GetFieldId());
			if (!Registration)
			{
				FieldFailure(OutError, TEXT("hjson_field_contract_field_missing"), TEXT("The HJSON contract refers to a field absent from the export registry."), Contract.GetFieldId());
				return false;
			}
			if (!Contract.Allows(EUnifiedHjsonFieldIntent::Preserve))
			{
				FieldFailure(OutError, TEXT("hjson_field_preserve_required"), TEXT("Every HJSON field contract must allow the preserve intent."), Contract.GetFieldId());
				return false;
			}
			if (Contract.Allows(EUnifiedHjsonFieldIntent::SetNull)
				&& !Contract.IsNullable())
			{
				FieldFailure(OutError, TEXT("hjson_field_nullability_conflict"), TEXT("The contract allows set_null for a non-nullable field."), Contract.GetFieldId());
				return false;
			}
			const FExportFieldContract& Field = Registration->GetField();
			if (Contract.Allows(EUnifiedHjsonFieldIntent::RestoreDefault)
				&& !Field.IsDefaultReproducible())
			{
				FieldFailure(OutError, TEXT("hjson_field_default_not_reproducible"), TEXT("The contract allows restore_default, but the field has no reproducible default."), Contract.GetFieldId());
				return false;
			}
			if (Contract.Allows(EUnifiedHjsonFieldIntent::Remove)
				&& !Field.IsPresenceSensitive())
			{
				FieldFailure(OutError, TEXT("hjson_field_remove_not_supported"), TEXT("The contract allows remove, but this field does not distinguish absence from a value."), Contract.GetFieldId());
				return false;
			}
		}

		for (const FExportFieldRegistration& Registration :
			BaseRegistry->GetRegistrations())
		{
			if (!Seen.Contains(Registration.GetField().GetFieldId()))
			{
				FieldFailure(OutError, TEXT("hjson_field_contract_missing"), TEXT("An exported field has no HJSON intent contract."), Registration.GetField().GetFieldId());
				return false;
			}
		}

		FUnifiedHjsonFieldRegistryPtr Candidate =
			MakeShareable(new FUnifiedHjsonFieldRegistry(
				MoveTemp(BaseRegistry),
				MoveTemp(Contracts)));
		if (!Candidate->IsValid())
		{
			FieldFailure(OutError, TEXT("hjson_field_registry_invalid"), TEXT("The compiled HJSON registry does not contain exactly one contract for each exported field."));
			return false;
		}
		OutRegistry = MoveTemp(Candidate);
		return true;
	}

	bool TryResolveUnifiedHjsonFieldIntent(
		FUnifiedHjsonFieldRegistryRef Registry,
		const FString& FieldId,
		const TOptional<EUnifiedHjsonFieldIntent>& ExplicitIntent,
		EUnifiedHjsonFieldIntent& OutIntent,
		FWorkspaceError& OutError)
	{
		OutError.Reset();
		const FUnifiedHjsonFieldContract* Contract =
			Registry->FindContract(FieldId);
		if (!Contract)
		{
			FieldFailure(OutError, TEXT("hjson_field_contract_missing"), TEXT("No HJSON intent contract is registered for this field."), FieldId);
			return false;
		}
		const EUnifiedHjsonFieldIntent Intent = ExplicitIntent.IsSet()
			? ExplicitIntent.GetValue()
			: EUnifiedHjsonFieldIntent::Preserve;
		if (!Contract->Allows(Intent))
		{
			TArray<TSharedPtr<FJsonValue>> Allowed;
			for (const auto AllowedIntent : Contract->GetAllowedIntents()) Allowed.Add(MakeShared<FJsonValueString>(LexToString(AllowedIntent)));
			const FString IntentName = IsIntentValueValid(Intent) ? LexToString(Intent) : FString::FromInt(static_cast<int32>(Intent));
			FieldFailure(OutError, TEXT("hjson_field_intent_not_allowed"), TEXT("The requested intent is not allowed for this field."), FieldId,
				MakeShared<FJsonValueString>(IntentName), MakeShared<FJsonValueArray>(Allowed));
			return false;
		}
		OutIntent = Intent;
		return true;
	}
}
