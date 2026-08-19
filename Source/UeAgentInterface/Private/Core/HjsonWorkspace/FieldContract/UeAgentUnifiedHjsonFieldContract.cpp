#include "Core/HjsonWorkspace/FieldContract/UeAgentUnifiedHjsonFieldContract.h"

namespace UE::UAI::HjsonWorkspace
{
	namespace
	{
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
			OutError.Set(TEXT("hjson_field_base_registry_invalid"));
			return false;
		}

		TSet<FString> Seen;
		for (const FUnifiedHjsonFieldContract& Contract : Contracts)
		{
			if (!Contract.IsValid())
			{
				OutError.Set(
					TEXT("hjson_field_contract_invalid"),
					Contract.GetFieldId());
				return false;
			}
			if (Seen.Contains(Contract.GetFieldId()))
			{
				OutError.Set(
					TEXT("hjson_field_contract_duplicate"),
					Contract.GetFieldId());
				return false;
			}
			Seen.Add(Contract.GetFieldId());
			const FExportFieldRegistration* Registration =
				BaseRegistry->FindField(Contract.GetFieldId());
			if (!Registration)
			{
				OutError.Set(
					TEXT("hjson_field_contract_field_missing"),
					Contract.GetFieldId());
				return false;
			}
			if (!Contract.Allows(EUnifiedHjsonFieldIntent::Preserve))
			{
				OutError.Set(
					TEXT("hjson_field_preserve_required"),
					Contract.GetFieldId());
				return false;
			}
			if (Contract.Allows(EUnifiedHjsonFieldIntent::SetNull)
				&& !Contract.IsNullable())
			{
				OutError.Set(
					TEXT("hjson_field_nullability_conflict"),
					Contract.GetFieldId());
				return false;
			}
			const FExportFieldContract& Field = Registration->GetField();
			if (Contract.Allows(EUnifiedHjsonFieldIntent::RestoreDefault)
				&& !Field.IsDefaultReproducible())
			{
				OutError.Set(
					TEXT("hjson_field_default_not_reproducible"),
					Contract.GetFieldId());
				return false;
			}
			if (Contract.Allows(EUnifiedHjsonFieldIntent::Remove)
				&& !Field.IsPresenceSensitive())
			{
				OutError.Set(
					TEXT("hjson_field_remove_not_supported"),
					Contract.GetFieldId());
				return false;
			}
		}

		for (const FExportFieldRegistration& Registration :
			BaseRegistry->GetRegistrations())
		{
			if (!Seen.Contains(Registration.GetField().GetFieldId()))
			{
				OutError.Set(
					TEXT("hjson_field_contract_missing"),
					Registration.GetField().GetFieldId());
				return false;
			}
		}

		FUnifiedHjsonFieldRegistryPtr Candidate =
			MakeShareable(new FUnifiedHjsonFieldRegistry(
				MoveTemp(BaseRegistry),
				MoveTemp(Contracts)));
		if (!Candidate->IsValid())
		{
			OutError.Set(TEXT("hjson_field_registry_invalid"));
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
			OutError.Set(TEXT("hjson_field_contract_missing"), FieldId);
			return false;
		}
		const EUnifiedHjsonFieldIntent Intent = ExplicitIntent.IsSet()
			? ExplicitIntent.GetValue()
			: EUnifiedHjsonFieldIntent::Preserve;
		if (!Contract->Allows(Intent))
		{
			OutError.Set(
				TEXT("hjson_field_intent_not_allowed"),
				FieldId,
				TEXT("/intent"));
			return false;
		}
		OutIntent = Intent;
		return true;
	}
}
