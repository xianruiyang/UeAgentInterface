#include "DomainAdapters/HjsonExternalInput/UeAgentHjsonExternalInputDomain.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/HjsonExternalInput/UeAgentHjsonExternalInputWorkspace.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UE::UAI::DomainAdapters::HjsonExternalInput
{
	using namespace HjsonWorkspace;
	using namespace CommandContract;

	namespace
	{
		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		bool IsSha256(const FString& Value)
		{
			if (!Value.StartsWith(TEXT("sha256:")) || Value.Len() != 71)
			{
				return false;
			}
			for (int32 Index = 7; Index < Value.Len(); ++Index)
			{
				if (!FChar::IsHexDigit(Value[Index]))
				{
					return false;
				}
			}
			return true;
		}

		bool StringField(
			const FJsonObject& Object,
			const FString& Key,
			FString& OutValue)
		{
			if (!Object.TryGetStringField(Key, OutValue))
			{
				OutValue.Reset();
				return false;
			}
			OutValue = Normalize(OutValue);
			return !OutValue.IsEmpty();
		}
	}

	bool FHjsonExternalInputFamilyContract::IsValid() const
	{
		UE::UAI::Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FHjsonExternalInputFamilyContract::TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		auto Reject = [&](const FString& Reason, const FString& Field)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_external_input_contract_invalid"),
				Reason, TEXT("domain_contract"));
			OutError.PropertyPath = Field;
			OutError.SourceKind = TEXT("native_result");
			return false;
		};
		if (RootTypeIdentity.IsEmpty()) return Reject(TEXT("The external-input family contract has no root type."), TEXT("root_type"));
		if (InitializationContractId.IsEmpty()) return Reject(TEXT("The external-input family contract has no initialization contract."), TEXT("initialization_contract"));
		if (ImporterCommandIds.IsEmpty()) return Reject(TEXT("The external-input family contract has no importer commands."), TEXT("importer_commands"));
		TSet<FString> Unique;
		for (int32 Index = 0; Index < ImporterCommandIds.Num(); ++Index)
		{
			const FString& CommandId = ImporterCommandIds[Index];
			if (CommandId.IsEmpty() || Unique.Contains(CommandId))
			{
				Reject(CommandId.IsEmpty() ? TEXT("An external-input importer command is empty.")
					: TEXT("An external-input importer command is duplicated."), FString::Printf(TEXT("importer_commands/%d"), Index));
				OutError.ActualValue = MakeShared<FJsonValueString>(CommandId);
				return false;
			}
			Unique.Add(CommandId);
		}
		return true;
	}

	bool FHjsonExternalInputDelegation::IsValid() const
	{
		return !CommandId.IsEmpty() && !CanonicalParamsJson.IsEmpty();
	}

	void FHjsonExternalInputDelegation::LocateFailure(FWorkspaceError& Error) const
	{
		if (SourceDocument) SourceDocument->LocateFailure(Error, true);
	}

	const TArray<FHjsonExternalInputFamilyContract>&
	GetHjsonExternalInputFamilyContracts()
	{
		static const TArray<FHjsonExternalInputFamilyContract> Contracts = {
			{EHjsonExternalInputFamily::Factory,
				TEXT("uai.external_input.factory"),
				TEXT("uai.init.hjson_external_input.factory_importer/1"),
				{
					TEXT("asset.reimport_sound_wave"),
					TEXT("asset.reimport_sparse_volume_texture"),
					TEXT("asset.reimport_static_mesh"),
					TEXT("asset.reimport_texture"),
					TEXT("external.fbx.animation.import"),
					TEXT("external.fbx.skeletal_mesh.import"),
					TEXT("external.fbx.static_mesh.import"),
					TEXT("external.geometry_cache.import"),
					TEXT("external.light_profile.import"),
					TEXT("external.sound_wave.import"),
					TEXT("external.sparse_volume_texture.import"),
					TEXT("external.texture.import"),
				}},
			{EHjsonExternalInputFamily::SkinWeightProfile,
				TEXT("uai.external_input.skin_weight_profile"),
				TEXT("uai.init.hjson_external_input.skin_weight_importer/1"),
				{TEXT("external.skin_weight_profile.import")}},
			{EHjsonExternalInputFamily::Localization,
				TEXT("uai.external_input.localization"),
				TEXT("uai.init.hjson_external_input.localization_importer/1"),
				{TEXT("external.localization.po.import")}},
		};
		return Contracts;
	}

	const FHjsonExternalInputFamilyContract*
	FindHjsonExternalInputFamilyContract(const FString& RootTypeIdentity)
	{
		return GetHjsonExternalInputFamilyContracts().FindByPredicate(
			[&RootTypeIdentity](const FHjsonExternalInputFamilyContract& Contract)
			{
				return Contract.RootTypeIdentity == RootTypeIdentity;
			});
	}

	bool TryBuildHjsonExternalInputDelegation(
		const FString& CanonicalContentGraphParamsJson,
		const FHjsonExternalInputFamilyContract& Contract,
		FHjsonExternalInputDelegation& OutDelegation,
		FWorkspaceError& OutError)
	{
		OutDelegation = {};
		OutError.Reset();
		UE::UAI::Contract::FDiagnostic ContractError;
		if (!Contract.TryValidate(ContractError))
		{
			OutError.SetFailure(MoveTemp(ContractError));
			return false;
		}
		FString PublicTarget;
		const auto Document = MakeShared<FResolvedExternalInputDocument>();
		if (!TryResolveExternalInputDocument(CanonicalContentGraphParamsJson,
			Contract.RootTypeIdentity, *Document, PublicTarget, OutError)) return false;
		ON_SCOPE_EXIT { Document->LocateFailure(OutError); };
		UE::UAI::Contract::FDiagnostic JsonError;
		auto InvalidValue = [&](const FString& Code, const FString& Pointer,
			const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual,
			const FString& Reason)
		{
			OutError.SetFailure(UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Pointer,
				Expected, Actual, TEXT("input_decode"), Reason));
			return false;
		};
		auto ReadObject = [&](const FJsonObject& Parent, const FString& Field, const FString& Pointer,
			TSharedPtr<FJsonObject>& Object)
		{
			const TSharedPtr<FJsonObject>* Found = nullptr;
			if (Parent.TryGetObjectField(Field, Found) && Found && Found->IsValid())
			{ Object = *Found; return true; }
			return InvalidValue(Field == TEXT("parameters") ? TEXT("hjson_external_input_target_parameters_invalid")
				: TEXT("hjson_external_input_document_invalid"), Pointer,
				MakeShared<FJsonValueString>(TEXT("object")), Parent.TryGetField(Field), TEXT("This external-input field requires an object."));
		};
		auto ReadString = [&](const FJsonObject& Parent, const FString& Field, const FString& Pointer, FString& Value)
		{
			if (StringField(Parent, Field, Value)) return true;
			return InvalidValue(TEXT("hjson_external_input_document_invalid"), Pointer,
				MakeShared<FJsonValueString>(TEXT("non-empty string")), Parent.TryGetField(Field), TEXT("This external-input field requires a non-empty string."));
		};
		const FString Base = TEXT("/root/external_input");
		TSharedPtr<FJsonObject> External, Source, Options, Policy, PolicyParams;
		if (!ReadObject(*Document->Root, TEXT("external_input"), Base, External)
			|| !ReadObject(*External, TEXT("source"), Base + TEXT("/source"), Source)
			|| !ReadObject(*External, TEXT("options"), Base + TEXT("/options"), Options)
			|| !ReadObject(*External, TEXT("target_policy"), Base + TEXT("/target_policy"), Policy)) return false;
		FString SourceIdentity;
		FString Format;
		FString ContentSha256;
		FString CommandId;
		if (!ReadString(*Source, TEXT("identity"), Base + TEXT("/source/identity"), SourceIdentity)
			|| !ReadString(*Source, TEXT("format"), Base + TEXT("/source/format"), Format)
			|| !ReadString(*Policy, TEXT("command_id"), Base + TEXT("/target_policy/command_id"), CommandId)) return false;
		if (!StringField(*Source, TEXT("content_sha256"), ContentSha256) || !IsSha256(ContentSha256))
		{
			auto Failure = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_external_input_source_digest_invalid"),
				TEXT("Source preparation did not produce a valid SHA-256 digest."), TEXT("input_decode"));
			Failure.SemanticPointer = Base + TEXT("/source/identity");
			Failure.SourceKind = TEXT("related_input");
			OutError.SetFailure(MoveTemp(Failure));
			return false;
		}
		if (!Contract.ImporterCommandIds.Contains(CommandId))
		{
			TArray<TSharedPtr<FJsonValue>> Commands;
			for (const auto& Allowed : Contract.ImporterCommandIds) Commands.Add(MakeShared<FJsonValueString>(Allowed));
			return InvalidValue(TEXT("hjson_external_input_importer_unregistered"), Base + TEXT("/target_policy/command_id"),
				MakeShared<FJsonValueArray>(MoveTemp(Commands)), Policy->TryGetField(TEXT("command_id")),
				TEXT("The importer command is not registered for this external-input family."));
		}
		if (!ReadObject(*Policy, TEXT("parameters"), Base + TEXT("/target_policy/parameters"), PolicyParams)) return false;
		const FString TargetField = CommandId.StartsWith(TEXT("external."))
			&& CommandId != TEXT("external.skin_weight_profile.import")
			&& CommandId != TEXT("external.localization.po.import")
			? FString(TEXT("destination_ref"))
			: FString(TEXT("target_ref"));
		FString PolicyTarget;
		if (!StringField(*PolicyParams, TargetField, PolicyTarget)
			|| PolicyTarget != PublicTarget)
		{
			return InvalidValue(TEXT("hjson_external_input_target_policy_mismatch"),
				Base + TEXT("/target_policy/parameters/") + TargetField,
				MakeShared<FJsonValueString>(PublicTarget), PolicyParams->TryGetField(TargetField),
				TEXT("The importer target must match the workspace target."));
		}
		const auto DelegatedParams = MakeShared<FJsonObject>();
		DelegatedParams->Values = PolicyParams->Values;
		DelegatedParams->SetStringField(TEXT("source_ref"), SourceIdentity);
		DelegatedParams->SetStringField(
			TEXT("_hjson_expected_source_sha256"),
			ContentSha256.ToLower());
		DelegatedParams->SetStringField(TEXT("_hjson_source_format"), Format.ToLower());
		if (!Options->Values.IsEmpty())
		{
			DelegatedParams->SetObjectField(TEXT("options"), Options);
		}
		FString Canonical;
		if (!Json::TryCanonicalize(DelegatedParams, Canonical, JsonError))
		{
			OutError.SetFailure(JsonError, FString());
			return false;
		}
		OutDelegation.CommandId = MoveTemp(CommandId);
		OutDelegation.CanonicalParamsJson = MoveTemp(Canonical);
		OutDelegation.SourceDocument = Document;
		return OutDelegation.IsValid();
	}
}
