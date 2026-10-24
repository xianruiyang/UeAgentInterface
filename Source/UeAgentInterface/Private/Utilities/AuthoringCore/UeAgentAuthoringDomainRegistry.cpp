// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringDomainRegistry.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

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

		using UE::UAI::Contract::FDiagnostic;

		FString PointerToken(FString Value)
		{
			return Value.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}

		FDiagnostic RegistryFailure(const TCHAR* Code, const TCHAR* Reason, const FString& Name, const FString& Path)
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("authoring_domain_registry"));
			Error.CandidateName = Name;
			Error.PropertyPath = Path;
			Error.SourceKind = TEXT("native_result");
			return Error;
		}

		FDiagnostic InvalidValue(const TCHAR* Code, const TCHAR* Reason, const FString& Name,
			const FString& Path, const FString& Expected, const FString& Actual)
		{
			auto Error = RegistryFailure(Code, Reason, Name, Path);
			Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			Error.ActualValue = MakeShared<FJsonValueString>(Actual);
			return Error;
		}

		TSharedPtr<FJsonValue> ComparisonValue(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
		TSharedPtr<FJsonValue> ComparisonValue(int32 Value) { return MakeShared<FJsonValueNumber>(Value); }

		template<typename T>
		bool RegistrationValuesEqual(const T& Expected, const T& Actual, const FString& Path, FDiagnostic* Error)
		{
			if (Expected == Actual) return true;
			if (Error)
			{
				Error->PropertyPath = Path;
				Error->ExpectedValue = ComparisonValue(Expected);
				Error->ActualValue = ComparisonValue(Actual);
			}
			return false;
		}
		bool RegistrationValuesEqual(const FCatalogBinding& Expected, const FCatalogBinding& Actual, const FString& Path, FDiagnostic* Error);
		bool RegistrationValuesEqual(const FDomainRegistration& Expected, const FDomainRegistration& Actual, const FString& Path, FDiagnostic* Error);
		template<typename T>
		bool RegistrationValuesEqual(const TArray<T>& Expected, const TArray<T>& Actual, const FString& Path, FDiagnostic* Error)
		{
			if (!RegistrationValuesEqual(Expected.Num(), Actual.Num(), Path + TEXT("/count"), Error)) return false;
			for (int32 Index = 0; Index < Expected.Num(); ++Index)
				if (!RegistrationValuesEqual(Expected[Index], Actual[Index], Path + TEXT("/") + FString::FromInt(Index), Error)) return false;
			return true;
		}
		bool RegistrationValuesEqual(const TMap<FString, FString>& Expected, const TMap<FString, FString>& Actual, const FString& Path, FDiagnostic* Error)
		{
			TArray<FString> Keys;
			Expected.GetKeys(Keys);
			for (const auto& Pair : Actual) Keys.AddUnique(Pair.Key);
			Keys.Sort();
			for (const FString& Key : Keys)
			{
				const auto* Left = Expected.Find(Key);
				const auto* Right = Actual.Find(Key);
				const FString FieldPath = Path + TEXT("/") + PointerToken(Key);
				if (!Left || !Right)
				{
					if (Error)
					{
						Error->PropertyPath = FieldPath;
						Error->ExpectedValuePresent = Left != nullptr;
						Error->ActualValuePresent = Right != nullptr;
						if (Left) Error->ExpectedValue = ComparisonValue(*Left);
						if (Right) Error->ActualValue = ComparisonValue(*Right);
					}
					return false;
				}
				if (!RegistrationValuesEqual(*Left, *Right, FieldPath, Error)) return false;
			}
			return true;
		}
		bool RegistrationValuesEqual(const FCatalogBinding& Expected, const FCatalogBinding& Actual, const FString& Path, FDiagnostic* Error)
		{
			return RegistrationValuesEqual(Expected.Kind, Actual.Kind, Path + TEXT("/kind"), Error)
				&& RegistrationValuesEqual(Expected.CatalogTypeAliases, Actual.CatalogTypeAliases, Path + TEXT("/catalog_type_aliases"), Error)
				&& RegistrationValuesEqual(Expected.DomainId, Actual.DomainId, Path + TEXT("/domain_id"), Error)
				&& RegistrationValuesEqual(Expected.WorkspaceContractId, Actual.WorkspaceContractId, Path + TEXT("/workspace_contract_id"), Error)
				&& RegistrationValuesEqual(Expected.InputContractId, Actual.InputContractId, Path + TEXT("/input_contract_id"), Error)
				&& RegistrationValuesEqual(Expected.UnitId, Actual.UnitId, Path + TEXT("/unit_id"), Error)
				&& RegistrationValuesEqual(Expected.PathPattern, Actual.PathPattern, Path + TEXT("/path_pattern"), Error)
				&& RegistrationValuesEqual(Expected.RelativePathTemplate, Actual.RelativePathTemplate, Path + TEXT("/relative_path_template"), Error)
				&& RegistrationValuesEqual(Expected.ExactIdentityFields, Actual.ExactIdentityFields, Path + TEXT("/exact_identity_fields"), Error)
				&& RegistrationValuesEqual(Expected.PointerTemplate, Actual.PointerTemplate, Path + TEXT("/pointer_template"), Error)
				&& RegistrationValuesEqual(Expected.MergeMode, Actual.MergeMode, Path + TEXT("/merge_mode"), Error)
				&& RegistrationValuesEqual(Expected.OperationMap, Actual.OperationMap, Path + TEXT("/operation_map"), Error)
				&& RegistrationValuesEqual(Expected.SeedBindings, Actual.SeedBindings, Path + TEXT("/seed_bindings"), Error)
				&& RegistrationValuesEqual(Expected.RequiredContext, Actual.RequiredContext, Path + TEXT("/required_context"), Error)
				&& RegistrationValuesEqual(Expected.ContextBindings, Actual.ContextBindings, Path + TEXT("/context_bindings"), Error)
				&& RegistrationValuesEqual(Expected.ContextValueAliases, Actual.ContextValueAliases, Path + TEXT("/context_value_aliases"), Error)
				&& RegistrationValuesEqual(Expected.ConstantBindings, Actual.ConstantBindings, Path + TEXT("/constant_bindings"), Error)
				&& RegistrationValuesEqual(Expected.RequiredSeedFields, Actual.RequiredSeedFields, Path + TEXT("/required_seed_fields"), Error)
				&& RegistrationValuesEqual(Expected.OptionalSeedFields, Actual.OptionalSeedFields, Path + TEXT("/optional_seed_fields"), Error);
		}
		bool RegistrationValuesEqual(const FDomainRegistration& Expected, const FDomainRegistration& Actual, const FString& Path, FDiagnostic* Error)
		{
			return RegistrationValuesEqual(Expected.DomainId, Actual.DomainId, Path + TEXT("/domain_id"), Error)
				&& RegistrationValuesEqual(Expected.WorkspaceContractIds, Actual.WorkspaceContractIds, Path + TEXT("/workspace_contract_ids"), Error)
				&& RegistrationValuesEqual(Expected.InputContractIds, Actual.InputContractIds, Path + TEXT("/input_contract_ids"), Error)
				&& RegistrationValuesEqual(Expected.ExportContractIds, Actual.ExportContractIds, Path + TEXT("/export_contract_ids"), Error)
				&& RegistrationValuesEqual(Expected.CatalogBindings, Actual.CatalogBindings, Path + TEXT("/catalog_bindings"), Error);
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

		bool Validate(const FDomainRegistration& Registration, UE::UAI::Contract::FDiagnostic& OutError)
		{
			UeAgentAuthoringSemanticRegistry::FDomainDescriptor SemanticDomain;
			if (Registration.DomainId.IsEmpty())
			{
				OutError = InvalidValue(TEXT("authoring_domain_id_missing"), TEXT("The authoring domain identifier is empty."), Registration.DomainId, TEXT("/domain_id"), TEXT("nonempty domain identifier"), Registration.DomainId);
				return false;
			}
			if (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().FindDomain(
				Registration.DomainId, SemanticDomain))
			{
				OutError = InvalidValue(TEXT("authoring_semantic_domain_missing"), TEXT("The authoring domain has no registered semantic domain."), Registration.DomainId, TEXT("/domain_id"), TEXT("registered semantic domain"), Registration.DomainId);
				return false;
			}
			for (const FString& ContractId : Registration.WorkspaceContractIds)
			{
				UeAgentApplyWorkspaceContract::FContract Contract;
				if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(ContractId, Contract))
				{
					OutError = InvalidValue(TEXT("authoring_workspace_contract_missing"), TEXT("The domain references an unregistered workspace contract."), Registration.DomainId, TEXT("/workspace_contract_ids"), TEXT("registered workspace contract"), ContractId);
					return false;
				}
			}
			for (const FString& ContractId : Registration.InputContractIds)
			{
				UeAgentApplyInputContract::FContract Contract;
				if (!UeAgentApplyInputContract::FRegistry::Get().FindContract(ContractId, Contract))
				{
					OutError = InvalidValue(TEXT("authoring_input_contract_missing"), TEXT("The domain references an unregistered input contract."), Registration.DomainId, TEXT("/input_contract_ids"), TEXT("registered input contract"), ContractId);
					return false;
				}
			}
			for (const FString& ContractId : Registration.ExportContractIds)
			{
				UeAgentExportContract::FContract Contract;
				if (!UeAgentExportContract::FRegistry::Get().FindContract(ContractId, Contract))
				{
					OutError = InvalidValue(TEXT("authoring_export_contract_missing"), TEXT("The domain references an unregistered export contract."), Registration.DomainId, TEXT("/export_contract_ids"), TEXT("registered export contract"), ContractId);
					return false;
				}
			}
			TSet<FString> LocalKinds;
			for (const FCatalogBinding& Binding : Registration.CatalogBindings)
			{
				const FString BindingPath = TEXT("/catalog_bindings/") + FString::FromInt(&Binding - Registration.CatalogBindings.GetData());
				const auto RequireText = [&](const TCHAR* Field, const FString& Value)
				{
					if (!Value.IsEmpty()) return true;
					OutError = InvalidValue(TEXT("authoring_catalog_binding_invalid"), TEXT("A required catalog binding field is empty."), Binding.Kind, BindingPath + TEXT("/") + Field, TEXT("nonempty string"), Value);
					return false;
				};
				if (!RequireText(TEXT("kind"), Binding.Kind) || !RequireText(TEXT("workspace_contract_id"), Binding.WorkspaceContractId)
					|| !RequireText(TEXT("input_contract_id"), Binding.InputContractId) || !RequireText(TEXT("unit_id"), Binding.UnitId)
					|| !RequireText(TEXT("path_pattern"), Binding.PathPattern) || !RequireText(TEXT("relative_path_template"), Binding.RelativePathTemplate)) return false;
				if (Binding.ExactIdentityFields.IsEmpty())
				{
					OutError = RegistryFailure(TEXT("authoring_catalog_binding_invalid"), TEXT("A catalog binding requires at least one exact identity field."), Binding.Kind, BindingPath + TEXT("/exact_identity_fields/count"));
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least 1"));
					OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
					return false;
				}
				if (!Binding.PointerTemplate.StartsWith(TEXT("/")))
				{
					OutError = InvalidValue(TEXT("authoring_catalog_binding_invalid"), TEXT("A catalog binding pointer template must start with '/'."), Binding.Kind, BindingPath + TEXT("/pointer_template"), TEXT("JSON pointer starting with /"), Binding.PointerTemplate);
					return false;
				}
				if (!RequireText(TEXT("merge_mode"), Binding.MergeMode)) return false;
				if (LocalKinds.Contains(Binding.Kind))
				{
					OutError = InvalidValue(TEXT("authoring_catalog_binding_duplicate"), TEXT("The domain declares the same catalog kind more than once."), Registration.DomainId, BindingPath + TEXT("/kind"), TEXT("unique catalog kind"), Binding.Kind);
					return false;
				}
				LocalKinds.Add(Binding.Kind);
				const auto RequireDomainContract = [&](const TCHAR* Field, const FString& Value, const TArray<FString>& Allowed)
				{
					if (Allowed.Contains(Value)) return true;
					OutError = InvalidValue(TEXT("authoring_catalog_binding_contract_outside_domain"), TEXT("The catalog binding references a contract not declared by its domain."), Binding.Kind, BindingPath + TEXT("/") + Field, TEXT("contract declared by this domain"), Value);
					TArray<TSharedPtr<FJsonValue>> Choices;
					for (const FString& ContractId : Allowed) Choices.Add(MakeShared<FJsonValueString>(ContractId));
					OutError.CandidateChoices = MakeShared<FJsonValueArray>(MoveTemp(Choices));
					return false;
				};
				if (!RequireDomainContract(TEXT("workspace_contract_id"), Binding.WorkspaceContractId, Registration.WorkspaceContractIds)
					|| !RequireDomainContract(TEXT("input_contract_id"), Binding.InputContractId, Registration.InputContractIds)) return false;
				for (const FString& Field : Binding.RequiredSeedFields)
				{
					if (Binding.OptionalSeedFields.Contains(Field))
					{
						OutError = InvalidValue(TEXT("authoring_catalog_seed_field_overlap"), TEXT("A seed field is declared both required and optional."), Binding.Kind, BindingPath + TEXT("/required_seed_fields"), TEXT("disjoint required and optional fields"), Field);
						return false;
					}
					if (!Binding.SeedBindings.Contains(Field))
					{
						OutError = RegistryFailure(TEXT("authoring_catalog_seed_binding_missing"), TEXT("A required seed field has no binding."), Binding.Kind, BindingPath + TEXT("/seed_bindings/") + PointerToken(Field));
						OutError.ExpectedValuePresent = true;
						OutError.ActualValuePresent = false;
						return false;
					}
				}
				for (const FString& Field : Binding.OptionalSeedFields)
				{
					if (!Binding.SeedBindings.Contains(Field))
					{
						OutError = RegistryFailure(TEXT("authoring_catalog_seed_binding_missing"), TEXT("An optional seed field has no binding."), Binding.Kind, BindingPath + TEXT("/seed_bindings/") + PointerToken(Field));
						OutError.ExpectedValuePresent = true;
						OutError.ActualValuePresent = false;
						return false;
					}
				}
				for (const FString& Field : Binding.RequiredContext)
				{
					if (!Binding.ContextBindings.Contains(Field))
					{
						OutError = RegistryFailure(TEXT("authoring_catalog_context_binding_missing"), TEXT("A required context field has no binding."), Binding.Kind, BindingPath + TEXT("/context_bindings/") + PointerToken(Field));
						OutError.ExpectedValuePresent = true;
						OutError.ActualValuePresent = false;
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
		return RegistrationValuesEqual(*this, Other, FString(), nullptr);
	}

	bool FDomainRegistration::operator==(const FDomainRegistration& Other) const
	{
		return RegistrationValuesEqual(*this, Other, FString(), nullptr);
	}

	FRegistry& FRegistry::Get()
	{
		static FRegistry Registry;
		return Registry;
	}

	bool FRegistry::RegisterDomain(
		const FDomainRegistration& Registration,
		ERegistrationResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const FDomainRegistration Canonical = Normalize(Registration);
		if (!Validate(Canonical, OutError)) return false;
		FWriteScopeLock Lock(RegistryLock);
		if (const FDomainRegistration* Existing = Domains.Find(Canonical.DomainId))
		{
			if (*Existing != Canonical)
			{
				OutError = RegistryFailure(TEXT("conflicting_authoring_domain_registration"), TEXT("The domain registration differs from the existing registration."), Canonical.DomainId, FString());
				RegistrationValuesEqual(*Existing, Canonical, FString(), &OutError);
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
				OutError = InvalidValue(TEXT("conflicting_authoring_catalog_name"), TEXT("The catalog kind conflicts with another kind or alias in this registration."), Canonical.DomainId, TEXT("/catalog_bindings/kind"), TEXT("unused catalog name"), Binding.Kind);
				return false;
			}
			PendingCatalogNames.Add(Binding.Kind);
			if (const FCatalogBinding* Existing = CatalogBindings.Find(Binding.Kind))
			{
				OutError = RegistryFailure(*Existing == Binding
                    ? TEXT("authoring_catalog_binding_owned_by_another_root") : TEXT("conflicting_authoring_catalog_binding"),
                    TEXT("The catalog kind already has an incompatible owner or binding."), Binding.Kind, TEXT("/catalog_bindings"));
				if (RegistrationValuesEqual(*Existing, Binding, TEXT("/catalog_bindings"), &OutError))
				{
					OutError.ExpectedValuePresent = false;
					OutError.ActualValuePresent = true;
				}
				return false;
			}
			if (CatalogAliases.Contains(Binding.Kind))
			{
				OutError = InvalidValue(TEXT("authoring_catalog_kind_conflicts_with_alias"), TEXT("The catalog kind is already registered as an alias for another kind."), Binding.Kind, TEXT("/catalog_bindings/kind"), TEXT("unused catalog name"), CatalogAliases.FindChecked(Binding.Kind));
				return false;
			}
			for (const FString& Alias : Binding.CatalogTypeAliases)
			{
				if (PendingCatalogNames.Contains(Alias) || CatalogBindings.Contains(Alias) || CatalogAliases.Contains(Alias))
				{
					OutError = InvalidValue(TEXT("conflicting_authoring_catalog_alias"), TEXT("The catalog alias conflicts with an existing kind or alias."), Binding.Kind, TEXT("/catalog_bindings/catalog_type_aliases"), TEXT("unused catalog alias"), Alias);
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

	bool RegisterGeneratedDomain(const FString& DomainId, UE::UAI::Contract::FDiagnostic& OutError)
	{
		FDomainRegistration Registration;
		if (!BuildGeneratedDomainRegistration(NormalizeId(DomainId), Registration))
		{
			OutError = InvalidValue(TEXT("generated_authoring_domain_missing"), TEXT("No generated authoring registration exists for this domain."), DomainId, TEXT("/domain_id"), TEXT("generated authoring domain"), DomainId);
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
