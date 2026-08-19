// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringDomainRegistry.h"

#include "Misc/ScopeRWLock.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"

namespace UeAgentAuthoringDomainRegistry
{
	namespace
	{
		FString NormalizeId(FString Value)
		{
			Value.TrimStartAndEndInline();
			Value.ToLowerInline();
			return Value;
		}

		void NormalizeList(TArray<FString>& Values)
		{
			for (FString& Value : Values) Value = NormalizeId(MoveTemp(Value));
			Values.RemoveAll([](const FString& Value) { return Value.IsEmpty(); });
			Values.Sort();
			for (int32 Index = Values.Num() - 1; Index > 0; --Index)
			{
				if (Values[Index] == Values[Index - 1]) Values.RemoveAt(Index);
			}
		}

		void NormalizeMapKeys(TMap<FString, FString>& Values)
		{
			TMap<FString, FString> Canonical;
			for (TPair<FString, FString>& Pair : Values)
			{
				FString Key = NormalizeId(MoveTemp(Pair.Key));
				Pair.Value.TrimStartAndEndInline();
				if (!Key.IsEmpty() && !Pair.Value.IsEmpty()) Canonical.Add(MoveTemp(Key), MoveTemp(Pair.Value));
			}
			Values = MoveTemp(Canonical);
		}

		bool StringMapsEqual(const TMap<FString, FString>& Left, const TMap<FString, FString>& Right)
		{
			if (Left.Num() != Right.Num()) return false;
			for (const TPair<FString, FString>& Pair : Left)
			{
				const FString* Value = Right.Find(Pair.Key);
				if (!Value || *Value != Pair.Value) return false;
			}
			return true;
		}

		FCatalogBinding Normalize(FCatalogBinding Binding, const FString& DomainId)
		{
			Binding.Kind = NormalizeId(MoveTemp(Binding.Kind));
			NormalizeList(Binding.CatalogTypeAliases);
			Binding.DomainId = NormalizeId(DomainId);
			Binding.WorkspaceContractId = NormalizeId(MoveTemp(Binding.WorkspaceContractId));
			Binding.InputContractId = NormalizeId(MoveTemp(Binding.InputContractId));
			Binding.UnitId = NormalizeId(MoveTemp(Binding.UnitId));
			Binding.PathPattern.TrimStartAndEndInline();
			Binding.RelativePathTemplate.TrimStartAndEndInline();
			NormalizeList(Binding.ExactIdentityFields);
			Binding.PointerTemplate.TrimStartAndEndInline();
			Binding.MergeMode = NormalizeId(MoveTemp(Binding.MergeMode));
			NormalizeMapKeys(Binding.OperationMap);
			NormalizeMapKeys(Binding.SeedBindings);
			NormalizeList(Binding.RequiredContext);
			NormalizeMapKeys(Binding.ContextBindings);
			NormalizeMapKeys(Binding.ContextValueAliases);
			NormalizeMapKeys(Binding.ConstantBindings);
			NormalizeList(Binding.RequiredSeedFields);
			NormalizeList(Binding.OptionalSeedFields);
			return Binding;
		}

		FDomainRegistration Normalize(FDomainRegistration Registration)
		{
			Registration.DomainId = NormalizeId(MoveTemp(Registration.DomainId));
			NormalizeList(Registration.WorkspaceContractIds);
			NormalizeList(Registration.InputContractIds);
			NormalizeList(Registration.ExportContractIds);
			for (FCatalogBinding& Binding : Registration.CatalogBindings)
			{
				Binding = Normalize(MoveTemp(Binding), Registration.DomainId);
			}
			Registration.CatalogBindings.Sort([](const FCatalogBinding& Left, const FCatalogBinding& Right)
			{
				return Left.Kind < Right.Kind;
			});
			return Registration;
		}

		bool Validate(const FDomainRegistration& Registration, FString& OutError)
		{
			UeAgentAuthoringSemanticRegistry::FDomainDescriptor SemanticDomain;
			if (Registration.DomainId.IsEmpty())
			{
				OutError = TEXT("authoring_domain_id_missing");
				return false;
			}
			if (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().FindDomain(
				Registration.DomainId, SemanticDomain))
			{
				OutError = TEXT("authoring_semantic_domain_missing:") + Registration.DomainId;
				return false;
			}
			for (const FString& ContractId : Registration.WorkspaceContractIds)
			{
				UeAgentApplyWorkspaceContract::FContract Contract;
				if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(ContractId, Contract))
				{
					OutError = TEXT("authoring_workspace_contract_missing:") + ContractId;
					return false;
				}
			}
			for (const FString& ContractId : Registration.InputContractIds)
			{
				UeAgentApplyInputContract::FContract Contract;
				if (!UeAgentApplyInputContract::FRegistry::Get().FindContract(ContractId, Contract))
				{
					OutError = TEXT("authoring_input_contract_missing:") + ContractId;
					return false;
				}
			}
			for (const FString& ContractId : Registration.ExportContractIds)
			{
				UeAgentExportContract::FContract Contract;
				if (!UeAgentExportContract::FRegistry::Get().FindContract(ContractId, Contract))
				{
					OutError = TEXT("authoring_export_contract_missing:") + ContractId;
					return false;
				}
			}
			TSet<FString> LocalKinds;
			for (const FCatalogBinding& Binding : Registration.CatalogBindings)
			{
				if (Binding.Kind.IsEmpty() || Binding.WorkspaceContractId.IsEmpty()
					|| Binding.InputContractId.IsEmpty() || Binding.UnitId.IsEmpty()
					|| Binding.PathPattern.IsEmpty() || Binding.RelativePathTemplate.IsEmpty()
					|| Binding.ExactIdentityFields.IsEmpty() || !Binding.PointerTemplate.StartsWith(TEXT("/"))
					|| Binding.MergeMode.IsEmpty())
				{
					OutError = TEXT("authoring_catalog_binding_invalid:") + Binding.Kind;
					return false;
				}
				if (LocalKinds.Contains(Binding.Kind))
				{
					OutError = TEXT("authoring_catalog_binding_duplicate:") + Binding.Kind;
					return false;
				}
				LocalKinds.Add(Binding.Kind);
				if (!Registration.WorkspaceContractIds.Contains(Binding.WorkspaceContractId)
					|| !Registration.InputContractIds.Contains(Binding.InputContractId))
				{
					OutError = TEXT("authoring_catalog_binding_contract_outside_domain:") + Binding.Kind;
					return false;
				}
				for (const FString& Field : Binding.RequiredSeedFields)
				{
					if (Binding.OptionalSeedFields.Contains(Field))
					{
						OutError = TEXT("authoring_catalog_seed_field_overlap:") + Binding.Kind + TEXT(":") + Field;
						return false;
					}
					if (!Binding.SeedBindings.Contains(Field))
					{
						OutError = TEXT("authoring_catalog_seed_binding_missing:") + Binding.Kind + TEXT(":") + Field;
						return false;
					}
				}
				for (const FString& Field : Binding.OptionalSeedFields)
				{
					if (!Binding.SeedBindings.Contains(Field))
					{
						OutError = TEXT("authoring_catalog_seed_binding_missing:") + Binding.Kind + TEXT(":") + Field;
						return false;
					}
				}
				for (const FString& Field : Binding.RequiredContext)
				{
					if (!Binding.ContextBindings.Contains(Field))
					{
						OutError = TEXT("authoring_catalog_context_binding_missing:") + Binding.Kind + TEXT(":") + Field;
						return false;
					}
				}
			}
			OutError.Reset();
			return true;
		}

		#include "Utilities/AuthoringCore/UeAgentAuthoringRegistryGenerated.inl"
	}

	bool FCatalogBinding::operator==(const FCatalogBinding& Other) const
	{
		return Kind == Other.Kind && CatalogTypeAliases == Other.CatalogTypeAliases && DomainId == Other.DomainId
			&& WorkspaceContractId == Other.WorkspaceContractId
			&& InputContractId == Other.InputContractId && UnitId == Other.UnitId
			&& PathPattern == Other.PathPattern && RelativePathTemplate == Other.RelativePathTemplate
			&& ExactIdentityFields == Other.ExactIdentityFields
			&& PointerTemplate == Other.PointerTemplate && MergeMode == Other.MergeMode
			&& StringMapsEqual(OperationMap, Other.OperationMap)
			&& StringMapsEqual(SeedBindings, Other.SeedBindings) && RequiredContext == Other.RequiredContext
			&& StringMapsEqual(ContextBindings, Other.ContextBindings)
			&& StringMapsEqual(ContextValueAliases, Other.ContextValueAliases)
			&& StringMapsEqual(ConstantBindings, Other.ConstantBindings)
			&& RequiredSeedFields == Other.RequiredSeedFields
			&& OptionalSeedFields == Other.OptionalSeedFields;
	}

	bool FDomainRegistration::operator==(const FDomainRegistration& Other) const
	{
		return DomainId == Other.DomainId
			&& WorkspaceContractIds == Other.WorkspaceContractIds
			&& InputContractIds == Other.InputContractIds
			&& ExportContractIds == Other.ExportContractIds
			&& CatalogBindings == Other.CatalogBindings;
	}

	FRegistry& FRegistry::Get()
	{
		static FRegistry Registry;
		return Registry;
	}

	bool FRegistry::RegisterDomain(
		const FDomainRegistration& Registration,
		ERegistrationResult& OutResult,
		FString& OutError)
	{
		const FDomainRegistration Canonical = Normalize(Registration);
		if (!Validate(Canonical, OutError)) return false;
		FWriteScopeLock Lock(RegistryLock);
		if (const FDomainRegistration* Existing = Domains.Find(Canonical.DomainId))
		{
			if (*Existing != Canonical)
			{
				OutError = TEXT("conflicting_authoring_domain_registration:") + Canonical.DomainId;
				return false;
			}
			OutResult = ERegistrationResult::AlreadyRegistered;
			OutError.Reset();
			return true;
		}
		TSet<FString> PendingCatalogNames;
		for (const FCatalogBinding& Binding : Canonical.CatalogBindings)
		{
			if (PendingCatalogNames.Contains(Binding.Kind))
			{
				OutError = TEXT("conflicting_authoring_catalog_name:") + Binding.Kind;
				return false;
			}
			PendingCatalogNames.Add(Binding.Kind);
			if (const FCatalogBinding* Existing = CatalogBindings.Find(Binding.Kind))
			{
				OutError = *Existing == Binding
					? TEXT("authoring_catalog_binding_owned_by_another_root:") + Binding.Kind
					: TEXT("conflicting_authoring_catalog_binding:") + Binding.Kind;
				return false;
			}
			if (CatalogAliases.Contains(Binding.Kind))
			{
				OutError = TEXT("authoring_catalog_kind_conflicts_with_alias:") + Binding.Kind;
				return false;
			}
			for (const FString& Alias : Binding.CatalogTypeAliases)
			{
				if (PendingCatalogNames.Contains(Alias) || CatalogBindings.Contains(Alias) || CatalogAliases.Contains(Alias))
				{
					OutError = TEXT("conflicting_authoring_catalog_alias:") + Alias;
					return false;
				}
				PendingCatalogNames.Add(Alias);
			}
		}
		Domains.Add(Canonical.DomainId, Canonical);
		for (const FCatalogBinding& Binding : Canonical.CatalogBindings)
		{
			CatalogBindings.Add(Binding.Kind, Binding);
			for (const FString& Alias : Binding.CatalogTypeAliases)
			{
				CatalogAliases.Add(Alias, Binding.Kind);
			}
		}
		OutResult = ERegistrationResult::Registered;
		OutError.Reset();
		return true;
	}

	bool FRegistry::FindDomain(const FString& DomainId, FDomainRegistration& OutRegistration) const
	{
		FReadScopeLock Lock(RegistryLock);
		if (const FDomainRegistration* Found = Domains.Find(NormalizeId(DomainId)))
		{
			OutRegistration = *Found;
			return true;
		}
		return false;
	}

	bool FRegistry::FindCatalogBinding(const FString& Kind, FCatalogBinding& OutBinding) const
	{
		FReadScopeLock Lock(RegistryLock);
		if (const FCatalogBinding* Found = CatalogBindings.Find(NormalizeId(Kind)))
		{
			OutBinding = *Found;
			return true;
		}
		const FString CanonicalKind = CatalogAliases.FindRef(NormalizeId(Kind));
		if (const FCatalogBinding* Found = CatalogBindings.Find(CanonicalKind))
		{
			OutBinding = *Found;
			return true;
		}
		return false;
	}

	TArray<FDomainRegistration> FRegistry::ListDomains() const
	{
		FReadScopeLock Lock(RegistryLock);
		TArray<FDomainRegistration> Result;
		Domains.GenerateValueArray(Result);
		Result.Sort([](const FDomainRegistration& Left, const FDomainRegistration& Right)
		{
			return Left.DomainId < Right.DomainId;
		});
		return Result;
	}

	TArray<FCatalogBinding> FRegistry::ListCatalogBindings() const
	{
		FReadScopeLock Lock(RegistryLock);
		TArray<FCatalogBinding> Result;
		CatalogBindings.GenerateValueArray(Result);
		Result.Sort([](const FCatalogBinding& Left, const FCatalogBinding& Right)
		{
			return Left.Kind < Right.Kind;
		});
		return Result;
	}

	bool RegisterGeneratedDomain(const FString& DomainId, FString& OutError)
	{
		FDomainRegistration Registration;
		if (!BuildGeneratedDomainRegistration(NormalizeId(DomainId), Registration))
		{
			OutError = TEXT("generated_authoring_domain_missing:") + DomainId;
			return false;
		}
		ERegistrationResult Result;
		return FRegistry::Get().RegisterDomain(Registration, Result, OutError);
	}

	const TArray<FString>& GetGeneratedSeedFields()
	{
		return GeneratedSeedFields();
	}
}
