// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringRegistrySnapshot.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "String/BytesToHex.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDomainRegistry.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

#include <openssl/sha.h>

namespace UeAgentAuthoringRegistrySnapshot
{
	namespace
	{
		TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const FString& Value : Values) Result.Add(MakeShared<FJsonValueString>(Value));
			return Result;
		}

		TSharedRef<FJsonObject> StringMap(const TMap<FString, FString>& Values)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<FString> Keys;
			Values.GetKeys(Keys);
			Keys.Sort();
			for (const FString& Key : Keys) Result->SetStringField(Key, Values.FindChecked(Key));
			return Result;
		}

		TSharedRef<FJsonObject> ApplyContract(
			const UeAgentAuthoringDomainRegistry::FDomainRegistration* Registration)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetArrayField(
				TEXT("workspace_contract_ids"),
				StringArray(Registration ? Registration->WorkspaceContractIds : TArray<FString>()));
			Result->SetArrayField(
				TEXT("input_contract_ids"),
				StringArray(Registration ? Registration->InputContractIds : TArray<FString>()));
			return Result;
		}

		TSharedRef<FJsonObject> ExportContract(
			const UeAgentAuthoringDomainRegistry::FDomainRegistration* Registration)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetArrayField(
				TEXT("contract_ids"),
				StringArray(Registration ? Registration->ExportContractIds : TArray<FString>()));
			return Result;
		}

		FString CanonicalHash(const TSharedRef<FJsonObject>& Object, UE::UAI::Contract::FDiagnostic& OutError)
		{
			FString Text;
			if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Object, Text, OutError)) return FString();
			FTCHARToUTF8 Utf8(*Text);
			uint8 Digest[SHA256_DIGEST_LENGTH];
			if (!SHA256(reinterpret_cast<const uint8*>(Utf8.Get()), static_cast<size_t>(Utf8.Length()), Digest))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("authoring_registry_sha256_failed"), TEXT("SHA-256 could not produce a digest for the authoring registry snapshot."), TEXT("authoring_registry_snapshot"));
				OutError.SourceKind = TEXT("native_result");
				return FString();
			}
			TStringBuilder<SHA256_DIGEST_LENGTH * 2> Result;
			UE::String::BytesToHexLower(MakeArrayView(Digest), Result);
			return Result.ToString();
		}
	}

	bool BuildSnapshot(TSharedPtr<FJsonObject>& OutSnapshot, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutSnapshot.Reset();
		OutError.Reset();
		TArray<TSharedPtr<FJsonValue>> Domains;
		TArray<TSharedPtr<FJsonValue>> ApplyDomains;
		TArray<TSharedPtr<FJsonValue>> ExportDomains;
		for (const UeAgentAuthoringSemanticRegistry::FDomainDescriptor& SemanticDomain :
			UeAgentAuthoringSemanticRegistry::FRegistry::Get().ListDomains())
		{
			UeAgentAuthoringDomainRegistry::FDomainRegistration Registration;
			const bool bHasRegistration = UeAgentAuthoringDomainRegistry::FRegistry::Get().FindDomain(
				SemanticDomain.DomainId, Registration);
			TSharedRef<FJsonObject> Domain = MakeShared<FJsonObject>();
			Domain->SetStringField(TEXT("domain_id"), SemanticDomain.DomainId);
			Domain->SetObjectField(TEXT("apply_contract"), ApplyContract(bHasRegistration ? &Registration : nullptr));
			Domain->SetObjectField(TEXT("export_contract"), ExportContract(bHasRegistration ? &Registration : nullptr));
			Domains.Add(MakeShared<FJsonValueObject>(Domain));

			TSharedRef<FJsonObject> ApplyDomain = MakeShared<FJsonObject>();
			ApplyDomain->SetStringField(TEXT("domain_id"), SemanticDomain.DomainId);
			ApplyDomain->SetObjectField(TEXT("apply_contract"), ApplyContract(bHasRegistration ? &Registration : nullptr));
			ApplyDomains.Add(MakeShared<FJsonValueObject>(ApplyDomain));

			TSharedRef<FJsonObject> ExportDomain = MakeShared<FJsonObject>();
			ExportDomain->SetStringField(TEXT("domain_id"), SemanticDomain.DomainId);
			ExportDomain->SetObjectField(TEXT("export_contract"), ExportContract(bHasRegistration ? &Registration : nullptr));
			ExportDomains.Add(MakeShared<FJsonValueObject>(ExportDomain));
		}

		TArray<TSharedPtr<FJsonValue>> Bindings;
		for (const UeAgentAuthoringDomainRegistry::FCatalogBinding& Binding :
			UeAgentAuthoringDomainRegistry::FRegistry::Get().ListCatalogBindings())
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("kind"), Binding.Kind);
			Object->SetArrayField(TEXT("catalog_type_aliases"), StringArray(Binding.CatalogTypeAliases));
			Object->SetStringField(TEXT("domain_id"), Binding.DomainId);
			Object->SetStringField(TEXT("workspace_contract_id"), Binding.WorkspaceContractId);
			Object->SetStringField(TEXT("input_contract_id"), Binding.InputContractId);
			Object->SetStringField(TEXT("unit_id"), Binding.UnitId);
			Object->SetStringField(TEXT("path_pattern"), Binding.PathPattern);
			Object->SetStringField(TEXT("relative_path_template"), Binding.RelativePathTemplate);
			Object->SetArrayField(TEXT("exact_identity_fields"), StringArray(Binding.ExactIdentityFields));
			Object->SetStringField(TEXT("pointer_template"), Binding.PointerTemplate);
			Object->SetStringField(TEXT("merge_mode"), Binding.MergeMode);
			Object->SetObjectField(TEXT("operation_map"), StringMap(Binding.OperationMap));
			Object->SetObjectField(TEXT("seed_bindings"), StringMap(Binding.SeedBindings));
			Object->SetArrayField(TEXT("required_context"), StringArray(Binding.RequiredContext));
			Object->SetObjectField(TEXT("context_bindings"), StringMap(Binding.ContextBindings));
			Object->SetObjectField(TEXT("context_value_aliases"), StringMap(Binding.ContextValueAliases));
			Object->SetObjectField(TEXT("constant_bindings"), StringMap(Binding.ConstantBindings));
			Object->SetArrayField(TEXT("required_seed_fields"), StringArray(Binding.RequiredSeedFields));
			Object->SetArrayField(TEXT("optional_seed_fields"), StringArray(Binding.OptionalSeedFields));
			Bindings.Add(MakeShared<FJsonValueObject>(Object));
		}

		TSharedRef<FJsonObject> SeedContract = MakeShared<FJsonObject>();
		SeedContract->SetArrayField(
			TEXT("allowed_fields"),
			StringArray(UeAgentAuthoringDomainRegistry::GetGeneratedSeedFields()));
		TSharedRef<FJsonObject> Registry = MakeShared<FJsonObject>();
		Registry->SetArrayField(TEXT("domains"), Domains);
		Registry->SetArrayField(TEXT("catalog_bindings"), Bindings);
		Registry->SetObjectField(TEXT("seed_contract"), SeedContract);

		TSharedRef<FJsonObject> ApplyProjection = MakeShared<FJsonObject>();
		ApplyProjection->SetArrayField(TEXT("domains"), ApplyDomains);
		TSharedRef<FJsonObject> ExportProjection = MakeShared<FJsonObject>();
		ExportProjection->SetArrayField(TEXT("domains"), ExportDomains);
		const FString RegistryHash = CanonicalHash(Registry, OutError);
		if (RegistryHash.IsEmpty()) return false;
		const FString ApplyHash = CanonicalHash(ApplyProjection, OutError);
		if (ApplyHash.IsEmpty()) return false;
		const FString ExportHash = CanonicalHash(ExportProjection, OutError);
		if (ExportHash.IsEmpty()) return false;

		TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
		Snapshot->SetStringField(TEXT("schema"), TEXT("uai.authoring_registry_snapshot.v1"));
		Snapshot->SetNumberField(TEXT("schema_version"), 1);
		Snapshot->SetStringField(TEXT("dialect_id"), FUeAgentStructuredHjsonFormatter::DialectId());
		Snapshot->SetStringField(TEXT("registry_hash"), RegistryHash);
		Snapshot->SetStringField(TEXT("apply_contract_hash"), ApplyHash);
		Snapshot->SetStringField(TEXT("export_contract_hash"), ExportHash);
		Snapshot->SetObjectField(TEXT("registry"), Registry);
		OutSnapshot = Snapshot;
		return true;
	}

	bool SerializeSnapshot(FString& OutHjson, UE::UAI::Contract::FDiagnostic& OutError)
	{
		TSharedPtr<FJsonObject> Snapshot;
		if (!BuildSnapshot(Snapshot, OutError) || !Snapshot.IsValid()) return false;
		return FUeAgentStructuredHjsonFormatter::TryWriteObject(Snapshot.ToSharedRef(), OutHjson, OutError);
	}
}
