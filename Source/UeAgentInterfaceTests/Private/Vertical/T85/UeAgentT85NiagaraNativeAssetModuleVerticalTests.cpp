#include "Misc/AutomationTest.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Module/CommandRegistration/Query/UeAgentQueryRegistration.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "PropertyEditorEditConstPolicy.h"
#include "UObject/UObjectGlobals.h"
#include "PropertyEditorModule.h"
#include "PropertyCustomizationHelpers.h"
#include "PropertyHandle.h"
#include "IPropertyRowGenerator.h"
#include "IDetailTreeNode.h"
#include "SClassViewer.h"
#include "ISinglePropertyView.h"
#include "UObject/UObjectIterator.h"
#include "EditorClassUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Vertical/T167/UeAgentT167AssetReferenceTestTypes.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringRegistration.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringStateAccessors.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringExport.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringApplyProvider.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraStageModuleAuthoring.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Module/CommandRegistration/Niagara/UeAgentNiagaraDomainRegistration.h"
#include "Module/CommandRegistration/UObjectDataAsset/UeAgentUObjectDataAssetRegistration.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptFactoryNew.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraValidationRules.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "UObject/UObjectHash.h"
#include "OperationOwners/UObjectDataAsset/UeAgentUObjectDataAssetCommandService.h"
#include "QueryOwners/Niagara/UeAgentNiagaraModuleCandidateQuery.h"
#include "UObject/Package.h"
#include "UObject/PackageReload.h"
#include "UObject/SavePackage.h"
#include "NiagaraScriptSource.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
	using namespace UE::UAI;
	using namespace UeAgentNiagaraStageModuleAuthoring;

	bool MatchesPropertyName(FString Actual, FString Expected)
	{
		for (const TCHAR* Separator : {TEXT(" "), TEXT("_"), TEXT("-")})
		{
			Actual.ReplaceInline(Separator, TEXT(""));
			Expected.ReplaceInline(Separator, TEXT(""));
		}
		return Actual.Equals(Expected, ESearchCase::IgnoreCase);
	}

	class FNeverCancelled final
		: public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	CommandEdge::FUeAgentCommandResult SubmitPublicCommand(
		const TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			Registry,
		const TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const FString& ParamsJson)
	{
		const auto* Contract = Registry.IsValid()
			? Registry->FindExact(CommandId)
			: nullptr;
		if (!Contract || !Dispatch.IsValid())
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t85-niagara"), TEXT("req-t85-niagara"), CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("niagara_public_import_contract_missing"),
				TEXT("the exact public import contract is unavailable"));
		}
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t85-niagara\",")
			TEXT("\"request_id\":\"req-t85-niagara\",")
			TEXT("\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*CommandId, *Contract->GetContractHash(), *ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		TOptional<CommandEdge::FUeAgentCommandRequest> Request;
		if (!CommandEdge::FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes, Request, Diagnostic)
			|| !Request.IsSet())
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t85-niagara"), TEXT("req-t85-niagara"), CommandId,
				Contract->GetContractHash(),
				TEXT("niagara_public_import_request_invalid"),
				TEXT("the exact public request could not be decoded"));
		}
		return Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("t85-niagara"),
				{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")},
				TEXT("corr-t85-niagara")),
			MakeShared<FNeverCancelled>());
	}

	bool TrySubmitPublicCommand(
		const TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId, const FString& ParamsJson, FString& OutError)
	{
		const CommandEdge::FUeAgentCommandResult Result = SubmitPublicCommand(Registry, Dispatch, CommandId, ParamsJson);
		if (!Result.IsOk()) OutError = CommandId + TEXT(":") + Result.GetErrorJson();
		return Result.IsOk();
	}

	bool TryCreateNiagaraPublicSurface(
		TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			OutRegistry,
		TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& OutDispatch,
		FString& OutError, bool bIncludeQuery = false)
	{
		TSharedPtr<OperationOwners::UObjectDataAsset::
			IUeAgentUObjectDataAssetCommandService> ContentGraphService;
		TSharedPtr<Module::CommandRegistration::Active::
			IUeAgentActiveCommandContribution> ContentGraphContribution;
		TSharedPtr<Module::CommandRegistration::Active::
			IUeAgentActiveCommandContribution> NiagaraContribution;
		TSharedPtr<Module::CommandRegistration::Active::IUeAgentActiveCommandContribution> QueryContribution;
		if (bIncludeQuery && !Module::CommandRegistration::Query::TryCreateProductionQueryContribution(QueryContribution, OutError)) return false;
		if (!OperationOwners::UObjectDataAsset::
				TryCreateProductionUObjectDataAssetCommandService(
					ContentGraphService, OutError)
			|| !ContentGraphService.IsValid()
			|| !Module::CommandRegistration::UObjectDataAsset::
				TryCreateUObjectDataAssetContribution(
					ContentGraphService.ToSharedRef(),
					ContentGraphContribution, OutError)
			|| !ContentGraphContribution.IsValid()
			|| !Module::CommandRegistration::Niagara::
				TryCreateProductionNiagaraCandidateContribution(
					NiagaraContribution, OutError)
			|| !NiagaraContribution.IsValid())
		{
			return false;
		}
		TArray<TSharedRef<Module::CommandRegistration::Active::IUeAgentActiveCommandContribution>> Contributions{
			ContentGraphContribution.ToSharedRef(), NiagaraContribution.ToSharedRef()};
		if (QueryContribution) Contributions.Add(QueryContribution.ToSharedRef());
		return Module::CommandRegistration::Active::TryComposeActiveCommandContributions(Contributions, OutRegistry, OutDispatch, OutError);
	}

	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
			UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	bool SaveSystem(
		UPackage& Package,
		UNiagaraSystem& System,
		const FString& Filename)
	{
		System.RequestCompile(false);
		Package.MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(
			&Package, &System, *Filename, Args);
		if (bSaved)
		{
			Package.SetDirtyFlag(false);
		}
		return bSaved;
	}

	bool SaveEmitter(
		UPackage& Package,
		UNiagaraEmitter& Emitter,
		const FString& Filename)
	{
		Package.MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(
			&Package, &Emitter, *Filename, Args);
		if (bSaved)
		{
			Package.SetDirtyFlag(false);
		}
		return bSaved;
	}

	const FResolvedStage* FindSystemUpdateStage(
		UObject& Target,
		const FString& UsageId,
		TArray<FResolvedStage>& OutStages,
		FString& OutError)
	{
		if (!EnumerateStages(Target, OutStages, OutError))
		{
			return nullptr;
		}
		FGuid ParsedUsageId;
		if (!FGuid::Parse(UsageId, ParsedUsageId))
		{
			OutError = TEXT("t85_niagara_usage_id_invalid");
			return nullptr;
		}
		return OutStages.FindByPredicate(
			[&ParsedUsageId](const FResolvedStage& Stage)
			{
				return Stage.Usage == ENiagaraScriptUsage::SystemUpdateScript
					&& Stage.UsageId == ParsedUsageId
					&& !Stage.EmitterHandleId.IsValid();
			});
	}

	const FResolvedStage* FindEmitterParticleUpdateStage(
		UObject& Target,
		TArray<FResolvedStage>& OutStages,
		FString& OutError)
	{
		if (!EnumerateStages(Target, OutStages, OutError))
		{
			return nullptr;
		}
		const FResolvedStage* Match = nullptr;
		for (const FResolvedStage& Stage : OutStages)
		{
			if (Stage.Usage != ENiagaraScriptUsage::ParticleUpdateScript
				|| Stage.EmitterHandleId.IsValid())
			{
				continue;
			}
			if (Match)
			{
				OutError = TEXT("t162_niagara_particle_update_ambiguous");
				return nullptr;
			}
			Match = &Stage;
		}
		if (!Match)
		{
			OutError = TEXT("t162_niagara_particle_update_missing");
		}
		return Match;
	}

	bool TryBuildNiagaraExportParams(
		const FString& Target,
		const FString& WorkspacePath,
		FString& OutParams,
		FString& OutError)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetStringField(TEXT("workspace_path"), WorkspacePath);
		Params->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		return CommandContract::Json::TryCanonicalize(
			Params, OutParams, OutError);
	}

	bool TryBuildNiagaraDirectoryImportParams(
		const FString& Target,
		const FString& WorkspacePath,
		FString& OutParams,
		FString& OutError)
	{
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("directory"));
		Source->SetStringField(TEXT("path"), WorkspacePath);
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetObjectField(TEXT("source"), Source);
		return CommandContract::Json::TryCanonicalize(Params, OutParams, OutError);
	}

	bool TryAddVisibleModuleCandidate(
		const FString& WorkspacePath,
		const FString& StageUsage,
		const FString& CandidateName,
		FString& OutError)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(
			Files, *WorkspacePath, TEXT("*.hjson"), true, false);
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/"))
				|| File.Contains(TEXT("\\.uai\\"))
				|| File.EndsWith(TEXT(".interfaces.hjson")))
			{
				continue;
			}
			FString Text;
			TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File)
				|| !FUeAgentStructuredHjsonParser::ParseObject(
					Text, File, Document, OutError)
				|| !Document.IsValid())
			{
				continue;
			}
			TArray<TSharedPtr<FJsonObject>> Pending{Document};
			while (!Pending.IsEmpty())
			{
				const TSharedPtr<FJsonObject> Current = Pending.Pop();
				FString Usage;
				const TSharedPtr<FJsonObject>* Modules = nullptr;
				if (Current->TryGetStringField(TEXT("usage"), Usage) && Usage == StageUsage
					&& Current->TryGetObjectField(TEXT("modules"), Modules))
				{
					const TSharedRef<FJsonObject> NewModule = MakeShared<FJsonObject>();
					NewModule->SetStringField(TEXT("name"), CandidateName);
					(*Modules)->SetObjectField(TEXT("AddedNativeModule"), NewModule);
					FString Updated;
					return CommandContract::Json::TryCanonicalize(
						Document.ToSharedRef(), Updated, OutError)
						&& FFileHelper::SaveStringToFile(
							Updated, *File,
							FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Current->Values)
				{
					if (Field.Value.IsValid() && Field.Value->Type == EJson::Object)
					{
						Pending.Add(Field.Value->AsObject());
					}
				}
			}
		}
		OutError = TEXT("niagara_visible_stage_not_found");
		return false;
	}

	bool TrySetNamedNumericField(
		const TSharedPtr<FJsonObject>& Object,
		const FString& FieldName,
		double Desired)
	{
		if (!Object.IsValid()) return false;
		FString EntryName;
		if (Object->TryGetStringField(TEXT("name"), EntryName)
			&& MatchesPropertyName(EntryName, FieldName))
		{
			const TSharedPtr<FJsonValue>* EntryValue = Object->Values.Find(TEXT("value"));
			if (EntryValue && EntryValue->IsValid() && (*EntryValue)->Type == EJson::Number)
			{
				Object->SetNumberField(TEXT("value"), Desired);
				return true;
			}
			if (EntryValue && EntryValue->IsValid() && (*EntryValue)->Type == EJson::Object)
			{
				double Literal = 0.0;
				const TSharedPtr<FJsonObject> ValueObject = (*EntryValue)->AsObject();
				if (ValueObject->TryGetNumberField(TEXT("literal"), Literal))
				{
					ValueObject->SetNumberField(TEXT("literal"), Desired);
					return true;
				}
			}
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
		{
			if (MatchesPropertyName(Field.Key, FieldName))
			{
				if (Field.Value.IsValid() && Field.Value->Type == EJson::Number)
				{
					Object->SetNumberField(Field.Key, Desired);
					return true;
				}
				if (Field.Value.IsValid() && Field.Value->Type == EJson::Object)
				{
					const TSharedPtr<FJsonObject> ValueObject = Field.Value->AsObject();
					double Literal = 0.0;
					if (ValueObject.IsValid() && ValueObject->TryGetNumberField(TEXT("literal"), Literal))
					{
						ValueObject->SetNumberField(TEXT("literal"), Desired);
						return true;
					}
				}
			}
			if (Field.Value.IsValid() && Field.Value->Type == EJson::Object
				&& TrySetNamedNumericField(Field.Value->AsObject(), FieldName, Desired))
			{
				return true;
			}
			if (Field.Value.IsValid() && Field.Value->Type == EJson::Array)
			{
				for (const TSharedPtr<FJsonValue>& Item : Field.Value->AsArray())
					if (Item.IsValid() && Item->Type == EJson::Object
						&& TrySetNamedNumericField(Item->AsObject(), FieldName, Desired)) return true;
			}
		}
		return false;
	}

	bool TrySetFixedBoundsMinX(
		const TSharedPtr<FJsonObject>& Object,
		double Desired)
	{
		if (!Object.IsValid()) return false;
		FString EntryName;
		const TSharedPtr<FJsonObject>* EntryValue = nullptr;
		if (Object->TryGetStringField(TEXT("name"), EntryName)
			&& MatchesPropertyName(EntryName, TEXT("Min"))
			&& Object->TryGetObjectField(TEXT("value"), EntryValue) && EntryValue
			&& TrySetNamedNumericField(*EntryValue, TEXT("X"), Desired)) return true;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
		{
			if (MatchesPropertyName(Field.Key, TEXT("Min"))
				&& Field.Value.IsValid() && Field.Value->Type == EJson::Array)
			{
				TArray<TSharedPtr<FJsonValue>> Values = Field.Value->AsArray();
				if (!Values.IsEmpty() && Values[0].IsValid() && Values[0]->Type == EJson::Number)
				{
					Values[0] = MakeShared<FJsonValueNumber>(Desired);
					Object->SetArrayField(Field.Key, Values);
					return true;
				}
				if (!Values.IsEmpty() && Values[0].IsValid() && Values[0]->Type == EJson::Object)
				{
					const TSharedPtr<FJsonObject> First = Values[0]->AsObject();
					double Existing = 0.0;
					if (First->TryGetNumberField(TEXT("value"), Existing))
					{
						First->SetNumberField(TEXT("value"), Desired);
						return true;
					}
					if (First->TryGetNumberField(TEXT("literal"), Existing))
					{
						First->SetNumberField(TEXT("literal"), Desired);
						return true;
					}
				}
			}
			if (MatchesPropertyName(Field.Key, TEXT("Min"))
				&& Field.Value.IsValid() && Field.Value->Type == EJson::Object
				&& [&]()
				{
					const TSharedPtr<FJsonObject> Minimum = Field.Value->AsObject();
					const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
					if (Minimum->TryGetArrayField(TEXT("components"), Components)
						&& Components && !Components->IsEmpty())
					{
						TArray<TSharedPtr<FJsonValue>> Values = *Components;
						Values[0] = MakeShared<FJsonValueNumber>(Desired);
						Minimum->SetArrayField(TEXT("components"), Values);
						return true;
					}
					return TrySetNamedNumericField(Minimum, TEXT("X"), Desired);
				}())
			{
				return true;
			}
			if (Field.Value.IsValid() && Field.Value->Type == EJson::Object
				&& TrySetFixedBoundsMinX(Field.Value->AsObject(), Desired))
			{
				return true;
			}
			if (Field.Value.IsValid() && Field.Value->Type == EJson::Array)
			{
				for (const TSharedPtr<FJsonValue>& Item : Field.Value->AsArray())
					if (Item.IsValid() && Item->Type == EJson::Object
						&& TrySetFixedBoundsMinX(Item->AsObject(), Desired)) return true;
			}
		}
		return false;
	}

	bool TryGetNamedNumericField(
		const TSharedPtr<FJsonObject>& Object,
		const FString& FieldName,
		double& OutValue)
	{
		if (!Object.IsValid()) return false;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
		{
			if (MatchesPropertyName(Field.Key, FieldName))
			{
				if (Field.Value.IsValid() && Field.Value->TryGetNumber(OutValue)) return true;
				if (Field.Value.IsValid() && Field.Value->Type == EJson::Object
					&& Field.Value->AsObject()->TryGetNumberField(TEXT("literal"), OutValue)) return true;
			}
			if (Field.Value.IsValid() && Field.Value->Type == EJson::Object
				&& TryGetNamedNumericField(Field.Value->AsObject(), FieldName, OutValue)) return true;
		}
		return false;
	}

	bool TryGetFixedBoundsMinX(const TSharedPtr<FJsonObject>& Object, double& OutValue)
	{
		if (!Object.IsValid()) return false;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
		{
			if (MatchesPropertyName(Field.Key, TEXT("Min"))
				&& Field.Value.IsValid() && Field.Value->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& Values = Field.Value->AsArray();
				if (!Values.IsEmpty() && Values[0].IsValid()
					&& Values[0]->TryGetNumber(OutValue)) return true;
			}
			if (MatchesPropertyName(Field.Key, TEXT("Min"))
				&& Field.Value.IsValid() && Field.Value->Type == EJson::Object
				&& TryGetNamedNumericField(Field.Value->AsObject(), TEXT("X"), OutValue)) return true;
			if (Field.Value.IsValid() && Field.Value->Type == EJson::Object
				&& TryGetFixedBoundsMinX(Field.Value->AsObject(), OutValue)) return true;
		}
		return false;
	}

	bool TryReadVisibleEmitterFixedBoundsMinX(
		const FString& WorkspacePath,
		double& OutValue,
		FString& OutError)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *WorkspacePath, TEXT("*.hjson"), true, false);
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))
				|| File.EndsWith(TEXT(".interfaces.hjson"))) continue;
			FString Text;
			TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File)
				|| !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, OutError)
				|| !Document.IsValid()) continue;
			TArray<TSharedPtr<FJsonObject>> Pending{Document};
			while (!Pending.IsEmpty())
			{
				const TSharedPtr<FJsonObject> Current = Pending.Pop();
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Current->Values)
				{
					if (MatchesPropertyName(Field.Key, TEXT("FixedBounds")) && Field.Value.IsValid()
						&& Field.Value->Type == EJson::Object
						&& TryGetFixedBoundsMinX(Field.Value->AsObject(), OutValue)) return true;
					if (Field.Value.IsValid() && Field.Value->Type == EJson::Object)
						Pending.Add(Field.Value->AsObject());
				}
			}
		}
		OutError = TEXT("niagara_visible_fixed_bounds_not_found");
		return false;
	}

	bool TryEditVisibleEmitterFixedBoundsMinX(
		const FString& WorkspacePath,
		double Desired,
		FString& OutError)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(
			Files, *WorkspacePath, TEXT("*.hjson"), true, false);
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))
				|| File.EndsWith(TEXT(".interfaces.hjson"))) continue;
			FString Text;
			TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File)
				|| !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, OutError)
				|| !Document.IsValid()) continue;
			TArray<TSharedPtr<FJsonObject>> Pending{Document};
			while (!Pending.IsEmpty())
			{
				const TSharedPtr<FJsonObject> Current = Pending.Pop();
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Current->Values)
				{
					if (MatchesPropertyName(Field.Key, TEXT("FixedBounds")) && Field.Value.IsValid()
						&& Field.Value->Type == EJson::Object
						&& TrySetFixedBoundsMinX(Field.Value->AsObject(), Desired))
					{
						FString Updated;
						return CommandContract::Json::TryCanonicalize(
							Document.ToSharedRef(), Updated, OutError)
							&& FFileHelper::SaveStringToFile(
								Updated, *File,
								FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
					}
					if (Field.Value.IsValid() && Field.Value->Type == EJson::Object)
						Pending.Add(Field.Value->AsObject());
				}
			}
		}
		OutError = TEXT("niagara_visible_fixed_bounds_not_found");
		return false;
	}

	// Snapshot native persistent content, including every owned object and every
	// module's reflected identity, pins and inputs. Bounds are asserted separately.
	TMap<FString, FString> CaptureEmitterContent(UNiagaraEmitter& Emitter, bool bAuthoringEquivalence = false,
		bool bIncludeEmitterRoot = false)
	{
		TMap<FString, FString> Result;
		TFunction<void(UStruct*, void*, UObject*, const FString&)> Capture;
		Capture = [&](UStruct* Type, void* Data, UObject* Owner, const FString& Prefix)
		{
			for (TFieldIterator<FProperty> It(Type); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
				// VersionData is captured field by field below, with Bounds checked
				// independently. Embedded fixtures assert that there is one version.
				if (Owner == &Emitter && Type == Emitter.GetClass()
					&& It->GetFName() == TEXT("VersionData")) continue;
				// RebuildCachedCompileIds regenerates these four derived fields
				// (NiagaraGraph.cpp). Ignore them only across successful compilation;
				// dirty-memory rollback comparisons include them without exception.
				if (bAuthoringEquivalence && Owner->IsA<UNiagaraGraph>()
					&& (It->GetFName() == TEXT("LastBuiltTraversalDataChangeId")
						|| It->GetFName() == TEXT("LastBuiltScriptVersionId")
						|| It->GetFName() == TEXT("CachedUsageInfo")
						|| It->GetFName() == TEXT("bHasValidLastBuiltScriptVersionId"))) continue;
				// System RequestCompile(true) calls ForceGraphToRecompileOnNextCheck,
				// regenerating graph revision/DDC salts and script compilation IDs.
				// These are success-phase equivalence exceptions, never rollback
				// exclusions. VersionData itself (including version identity and all
				// authoring fields) remains captured; only its generated ID differs.
				if (bAuthoringEquivalence && Owner->IsA<UNiagaraGraph>()
					&& (It->GetFName() == TEXT("ChangeId") || It->GetFName() == TEXT("ForceRebuildId"))) continue;
				if (bAuthoringEquivalence && Owner->IsA<UNiagaraScript>()
					&& ((Type == Owner->GetClass() && It->GetFName() == TEXT("CachedScriptVMId"))
						|| (Type == FVersionedNiagaraScriptData::StaticStruct()
							&& It->GetFName() == TEXT("LastGeneratedVMId")))) continue;
				if (Type == FVersionedNiagaraEmitterData::StaticStruct() && It->GetFName() == TEXT("FixedBounds")) continue;
				// Script VersionData mixes authored fields and compilation identity.
				// Keep every field visible to the oracle rather than comparing or
				// excluding the whole aggregate as an opaque string.
				if (Owner->IsA<UNiagaraScript>() && It->GetFName() == TEXT("VersionData"))
				{
					const FArrayProperty* Array = CastFieldChecked<FArrayProperty>(*It);
					FScriptArrayHelper Items(Array, Array->ContainerPtrToValuePtr<void>(Data));
					Result.Add(Prefix + TEXT("/VersionData/@count"), FString::FromInt(Items.Num()));
					for (int32 Index = 0; Index < Items.Num(); ++Index)
						Capture(FVersionedNiagaraScriptData::StaticStruct(), Items.GetRawPtr(Index), Owner,
							Prefix + FString::Printf(TEXT("/VersionData/%d"), Index));
					continue;
				}
				FString Value;
				if (bAuthoringEquivalence && Owner->IsA<UNiagaraRendererProperties>()
					&& It->GetFName() == TEXT("RendererEnabledBinding"))
				{
					FNiagaraVariableAttributeBinding Binding = *It->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(Data);
					const FProperty* Root = FindFProperty<FProperty>(FNiagaraVariableAttributeBinding::StaticStruct(), TEXT("RootVariable"));
					if (Root && Root->ContainerPtrToValuePtr<FNiagaraVariableBase>(&Binding)->GetName().IsNone()
						&& Binding.GetParamMapBindableVariable().GetName().IsNone() && !Binding.IsValid())
					{
						// PostInitProperties initializes this unbound binding for Emitter;
						// RenameEmitter refreshes its cached particle bit for the renderer
						// source mode. Only this derived leaf of an invalid, nameless
						// binding is normalized across success/reload. Valid bindings and
						// all dirty-source/rollback captures remain fully exact.
						FBoolProperty* Cached = FindFProperty<FBoolProperty>(FNiagaraVariableAttributeBinding::StaticStruct(), TEXT("bIsCachedParticleValue"));
						check(Cached);
						Cached->SetPropertyValue_InContainer(&Binding, false);
						It->ExportTextItem_Direct(Value, &Binding, nullptr, Owner, PPF_Delimited);
						Result.Add(Prefix + TEXT("/") + It->GetName(), MoveTemp(Value));
						continue;
					}
				}
				It->ExportTextItem_Direct(Value, It->ContainerPtrToValuePtr<void>(Data), nullptr, Owner, PPF_Delimited);
				Result.Add(Prefix + TEXT("/") + It->GetName(), MoveTemp(Value));
			}
		};
		Capture(FVersionedNiagaraEmitterData::StaticStruct(), Emitter.GetLatestEmitterData(), &Emitter, TEXT("data"));
		if (bIncludeEmitterRoot) Capture(Emitter.GetClass(), &Emitter, &Emitter, TEXT("emitter"));
		TArray<UObject*> Owned;
		GetObjectsWithOuter(&Emitter, Owned, true);
		for (UObject* Object : Owned)
		{
			const FString Path = Object->GetPathName(&Emitter);
			Result.Add(Path + TEXT("/@class"), Object->GetClass()->GetPathName());
			Capture(Object->GetClass(), Object, Object, Path);
			// Pins are custom-serialized, not UPROPERTY members of UEdGraphNode.
			if (const UEdGraphNode* Node = Cast<UEdGraphNode>(Object))
			{
				for (int32 Index = 0; Index < Node->Pins.Num(); ++Index)
				{
					FString PinText;
					if (Node->Pins[Index]) Node->Pins[Index]->ExportTextItem(PinText, PPF_Delimited);
					Result.Add(Path + FString::Printf(TEXT("/@pin/%d"), Index), MoveTemp(PinText));
				}
			}
		}
		return Result;
	}

	bool SameEmitterContent(FAutomationTestBase& Test, const TMap<FString, FString>& Expected,
		UNiagaraEmitter& Actual, const TCHAR* Phase, bool bAuthoringEquivalence = false,
		bool bIncludeEmitterRoot = false)
	{
		const TMap<FString, FString> Values = CaptureEmitterContent(Actual, bAuthoringEquivalence, bIncludeEmitterRoot);
		bool bSame = Test.TestEqual(Phase, Values.Num(), Expected.Num());
		for (const auto& Pair : Expected)
		{
			const FString* Value = Values.Find(Pair.Key);
			if (!Value || *Value != Pair.Value)
			{
				Test.AddError(FString(Phase) + TEXT(": native content differs at ") + Pair.Key);
				bSame = false;
			}
		}
		return bSame;
	}

	bool HasExactBounds(const UNiagaraEmitter* Emitter, double MinX)
	{
		const auto* Data = Emitter ? Emitter->GetLatestEmitterData() : nullptr;
		return Data && Data->CalculateBoundsMode == ENiagaraEmitterCalculateBoundMode::Fixed
			&& Data->FixedBounds.Min == FVector(MinX, -20.0, -30.0)
			&& Data->FixedBounds.Max == FVector(10.0, 20.0, 30.0)
			&& Data->FixedBounds.IsValid == 1;
	}

	class FEmitterPropertyStageObserver final
		: public UeAgentAuthoringOperationApply::IDurablePackageStager
	{
	public:
		FEmitterPropertyStageObserver(
			FString InObjectPath,
			FString InFilename,
			TArray<uint8> InDiskBefore,
			double InDesiredMinX)
			: ObjectPath(MoveTemp(InObjectPath))
			, Filename(MoveTemp(InFilename))
			, DiskBefore(MoveTemp(InDiskBefore))
			, DesiredMinX(InDesiredMinX)
		{
		}

		bool BuildArtifactInventory(
			const TArray<UeAgentAuthoringOperationApply::FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts,
			UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) override
		{
			return Inner->BuildArtifactInventory(Plans, Artifacts, Failure);
		}

		bool StagePackages(
			const TArray<UeAgentAuthoringOperationApply::FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts,
			UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) override
		{
			if (!Inner->StagePackages(Plans, Artifacts, Failure)) return false;
			UNiagaraEmitter* const Actual = LoadObject<UNiagaraEmitter>(nullptr, *ObjectPath);
			const FVersionedNiagaraEmitterData* const Data = Actual ? Actual->GetLatestEmitterData() : nullptr;
			bObservedDesiredInMemory = HasExactBounds(Actual, DesiredMinX);
			TArray<uint8> DiskNow;
			bObservedOriginalDisk = FFileHelper::LoadFileToArray(DiskNow, *Filename)
				&& DiskNow == DiskBefore;
			return true;
		}

		bool bObservedDesiredInMemory = false;
		bool bObservedOriginalDisk = false;

	private:
		TSharedRef<UeAgentAuthoringOperationApply::IDurablePackageStager> Inner =
			UeAgentAuthoringOperationApply::CreateDefaultDurablePackageStager();
		FString ObjectPath;
		FString Filename;
		TArray<uint8> DiskBefore;
		double DesiredMinX = 0.0;
	};

	class FCreateTextStageObserver final : public UeAgentAuthoringOperationApply::IDurablePackageStager
	{
	public:
		TFunction<bool(UeAgentAuthoringWorkspaceTransaction::FOperationFailure&)> BeforeStage;
		TFunction<bool(UeAgentAuthoringWorkspaceTransaction::FOperationFailure&)> BeforeCheckpoint;
		bool BuildArtifactInventory(const TArray<UeAgentAuthoringOperationApply::FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts,
			UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) override
		{ return Inner->BuildArtifactInventory(Plans, Artifacts, Failure); }
		bool StagePackages(const TArray<UeAgentAuthoringOperationApply::FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts,
			UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) override
		{
			// The same stager also captures the absent before checkpoint as Delete.
			// Only observe/refuse forward Save, after native creation has happened.
			const bool bForwardSave = Plans.ContainsByPredicate([](const auto& Plan) { return Plan.Intent == UeAgentAuthoringOperationApply::EPackageDurableIntent::Save; });
			return (bForwardSave ? BeforeStage(Failure) : (!BeforeCheckpoint || BeforeCheckpoint(Failure)))
				&& Inner->StagePackages(Plans, Artifacts, Failure);
		}
	private:
		TSharedRef<UeAgentAuthoringOperationApply::IDurablePackageStager> Inner = UeAgentAuthoringOperationApply::CreateDefaultDurablePackageStager();
	};

	class FNextTickObservation final : public IAutomationLatentCommand
	{
	public:
		explicit FNextTickObservation(TFunction<void()> InObserve) : Observe(MoveTemp(InObserve)) {}
		bool Update() override { if (!bWaited) { bWaited = true; return false; } Observe(); return true; }
	private:
		bool bWaited = false;
		TFunction<void()> Observe;
	};

	bool HasMachineIdentity(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid()) return false;
		if (Value->Type == EJson::String)
		{
			FGuid Guid;
			return Value->AsString().StartsWith(TEXT("sha256:"))
				|| (FGuid::Parse(Value->AsString(), Guid) && Guid.IsValid());
		}
		if (Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
				if (HasMachineIdentity(Item)) return true;
			return false;
		}
		if (Value->Type != EJson::Object) return false;
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
		{
			if (Field.Key == TEXT("usage_id")
				|| Field.Key == TEXT("module_guid")
				|| Field.Key == TEXT("dynamic_input_guid")
				|| Field.Key == TEXT("emitter_handle_id")
				|| Field.Key == TEXT("hash")
				|| Field.Key.EndsWith(TEXT("_hash"))
				|| HasMachineIdentity(Field.Value)) return true;
		}
		return false;
	}

	bool VisibleWorkspaceHasMachineIdentity(const FString& WorkspacePath)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(
			Files, *WorkspacePath, TEXT("*.hjson"), true, false);
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/"))
				|| File.Contains(TEXT("\\.uai\\"))
				|| File.EndsWith(TEXT(".interfaces.hjson"))) continue;
			FString Text;
			TSharedPtr<FJsonObject> Document;
			FString Error;
			if (FFileHelper::LoadFileToString(Text, *File)
				&& FUeAgentStructuredHjsonParser::ParseObject(
					Text, File, Document, Error)
				&& Document.IsValid()
				&& HasMachineIdentity(MakeShared<FJsonValueObject>(Document)))
			{
				return true;
			}
		}
		return false;
	}

	bool TryEditVisibleEmitterFixedBoundsMinXByName(
		const FString& WorkspacePath, const FString& EmitterName,
		double Desired, FString& OutError)
	{
		TArray<FString> Files;
		const FString OwnerToken = EmitterName.ToLower();
		IFileManager::Get().FindFilesRecursive(Files, *WorkspacePath, TEXT("*.hjson"), true, false);
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))
				|| File.EndsWith(TEXT(".interfaces.hjson"))) continue;
			FString Text;
			TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File)
				|| !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, OutError)
				|| !Document.IsValid()) continue;
			if (File.ToLower().Contains(OwnerToken)
				&& TrySetFixedBoundsMinX(Document, Desired))
			{
				FString Updated;
				return CommandContract::Json::TryCanonicalize(Document.ToSharedRef(), Updated, OutError)
					&& FFileHelper::SaveStringToFile(Updated, *File,
						FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			}
			TArray<TSharedPtr<FJsonObject>> Pending{Document};
			while (!Pending.IsEmpty())
			{
				const TSharedPtr<FJsonObject> Current = Pending.Pop();
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Current->Values)
				{
					const bool bNamedOwner = MatchesPropertyName(Field.Key, EmitterName)
						&& Field.Value.IsValid() && Field.Value->Type == EJson::Object;
					FString ReadableName;
					const bool bReadableOwner = Field.Value.IsValid() && Field.Value->Type == EJson::String
						&& MatchesPropertyName(Field.Key, TEXT("emitter_name"))
						&& Field.Value->TryGetString(ReadableName)
						&& ReadableName == EmitterName;
					if ((bNamedOwner && TrySetFixedBoundsMinX(Field.Value->AsObject(), Desired))
						|| (bReadableOwner && TrySetFixedBoundsMinX(Current, Desired)))
					{
						FString Updated;
						return CommandContract::Json::TryCanonicalize(Document.ToSharedRef(), Updated, OutError)
							&& FFileHelper::SaveStringToFile(Updated, *File,
								FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
					}
					if (Field.Value.IsValid() && Field.Value->Type == EJson::Object)
						Pending.Add(Field.Value->AsObject());
				}
			}
		}
		if (OutError.IsEmpty())
			OutError = TEXT("niagara_visible_named_emitter_fixed_bounds_not_found:") + EmitterName;
		return false;
	}

	bool TryReadVisibleEmitterFixedBoundsMinXByName(
		const FString& WorkspacePath, const FString& EmitterName,
		double& OutValue, FString& OutError)
	{
		TArray<FString> Files;
		FString OwnerToken = EmitterName.ToLower();
		IFileManager::Get().FindFilesRecursive(Files, *WorkspacePath, TEXT("*.hjson"), true, false);
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))
				|| File.EndsWith(TEXT(".interfaces.hjson"))) continue;
			FString Text;
			TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File)
				|| !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, OutError)
				|| !Document.IsValid()) continue;
			if (File.ToLower().Contains(OwnerToken)
				&& TryGetFixedBoundsMinX(Document, OutValue)) return true;
			TArray<TSharedPtr<FJsonObject>> Pending{Document};
			while (!Pending.IsEmpty())
			{
				const TSharedPtr<FJsonObject> Current = Pending.Pop();
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Current->Values)
				{
					FString ReadableName;
					if ((MatchesPropertyName(Field.Key, EmitterName)
							&& Field.Value.IsValid() && Field.Value->Type == EJson::Object
							&& TryGetFixedBoundsMinX(Field.Value->AsObject(), OutValue))
						|| (Field.Value.IsValid() && Field.Value->Type == EJson::String
							&& MatchesPropertyName(Field.Key, TEXT("emitter_name"))
							&& Field.Value->TryGetString(ReadableName) && ReadableName == EmitterName
							&& TryGetFixedBoundsMinX(Current, OutValue))) return true;
					if (Field.Value.IsValid() && Field.Value->Type == EJson::Object)
						Pending.Add(Field.Value->AsObject());
				}
			}
		}
		OutError = TEXT("niagara_visible_named_emitter_fixed_bounds_not_found:") + EmitterName;
		return false;
	}

	TMap<FString, FString> CaptureVisibleOwnerFiles(
		const FString& WorkspacePath, const FString& EmitterName,
		bool bIncludeInterfaceFiles = false)
	{
		TMap<FString, FString> Result;
		const FString OwnerToken = EmitterName.ToLower();
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *WorkspacePath, TEXT("*.hjson"), true, false);
		for (const FString& File : Files)
		{
			if (!File.ToLower().Contains(OwnerToken) || File.Contains(TEXT("/.uai/"))
				|| File.Contains(TEXT("\\.uai\\"))
				|| (!bIncludeInterfaceFiles && File.EndsWith(TEXT(".interfaces.hjson")))) continue;
			FString Text;
			TSharedPtr<FJsonObject> Document;
			FString Error;
			FString Canonical;
			if (FFileHelper::LoadFileToString(Text, *File)
				&& FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)
				&& Document.IsValid()
				&& CommandContract::Json::TryCanonicalize(Document.ToSharedRef(), Canonical, Error))
			{
				FString Relative = File;
				FPaths::MakePathRelativeTo(Relative, *(WorkspacePath / TEXT("")));
				Result.Add(Relative, MoveTemp(Canonical));
			}
		}
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85NiagaraNativeAssetModuleVerticalTest,
	"UAI.Root.T85.Niagara.NativeAssetModuleVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85NiagaraNativeAssetModuleVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::Niagara;

	const FString PackageName =
		TEXT("/Game/UAI/Transient/T85NiagaraNativeAssetModule/NS_NativeAssetModule");
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	DeleteAssetIfPresent(ObjectPath);
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
			*this, PackageName);
	};

	UPackage* const Package = CreatePackage(*PackageName);
	UNiagaraSystem* const System = Package
		? NewObject<UNiagaraSystem>(
			Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!TestNotNull(
		TEXT("a persistent Niagara System fixture is created"), System))
	{
		return false;
	}
	UNiagaraSystemFactoryNew::InitializeSystem(System, true);
	FAssetRegistryModule::AssetCreated(System);
	if (!TestTrue(
		TEXT("the initialized Niagara System fixture is saved"),
		SaveSystem(*Package, *System, Filename)))
	{
		return false;
	}

	FNiagaraModuleCandidateContext SearchContext;
	SearchContext.TargetUsage = TEXT("SystemUpdateScript");
	SearchContext.TargetUsageId = System->GetSystemUpdateScript()
		->GetUsageId().ToString(EGuidFormats::DigitsWithHyphensLower);
	TArray<FResolvedStage> InitialStages;
	FString StageError;
	const FResolvedStage* const InitialStage = FindSystemUpdateStage(
		*System, SearchContext.TargetUsageId, InitialStages, StageError);
	if (!TestNotNull(
		TEXT("the fixture exposes its real System Update output"), InitialStage))
	{
		AddError(StageError);
		return false;
	}
	TArray<UNiagaraNodeFunctionCall*> InitialModules;
	GetModules(*InitialStage->OutputNode, InitialModules);
	TMap<FString, int32> InitialScriptCounts;
	for (UNiagaraNodeFunctionCall* ModuleNode : InitialModules)
	{
		if (ModuleNode)
		{
			++InitialScriptCounts.FindOrAdd(GetModuleScriptPath(*ModuleNode));
		}
	}

	FNiagaraModuleCandidate Selected;
	FNiagaraModuleCandidateQueryResult CandidateResult;
	FNiagaraModuleCandidateQueryError QueryError;
	CandidateResult = {};
	QueryError = {};
	if (!TestTrue(
		TEXT("search enumerates native asset-module actions in the exact output context"),
		TryQueryNiagaraModuleCandidates(
			*System,
			FString(),
			SearchContext,
			CandidateResult,
			QueryError)))
	{
		AddError(QueryError.Code + TEXT(":") + QueryError.PublicMessage);
		return false;
	}
	for (const FNiagaraModuleCandidate& Candidate
		: CandidateResult.Candidates)
	{
		if (Candidate.bSupported
			&& !InitialScriptCounts.Contains(Candidate.ScriptAssetPath))
		{
			Selected = Candidate;
			break;
		}
	}
	if (!TestFalse(
		TEXT("a native System Update asset-module action is available"),
		Selected.Name.IsEmpty()))
	{
		return false;
	}
	TestFalse(TEXT("the public selector name is non-empty"),
		Selected.Name.IsEmpty());
	TestFalse(TEXT("the public name is not a script path"),
		Selected.Name.StartsWith(TEXT("/")));
	TestFalse(TEXT("native candidate search leaves the persisted asset clean"),
		Package->IsDirty());

	FNiagaraModuleCandidate RejectedCandidate;
	FNiagaraModuleCandidateContext RejectedContext;
	UNiagaraNodeFunctionCall* RejectedNode = nullptr;
	FNiagaraModuleCandidateQueryError RejectedError;
	if (!TestFalse(
		TEXT("a script path is not accepted as a candidate name selector"),
		TryPerformNiagaraModuleCandidate(
			*System,
			Selected.ScriptAssetPath,
			CandidateResult.Context,
			INDEX_NONE,
			RejectedCandidate,
			RejectedContext,
			RejectedNode,
			RejectedError)))
	{
		return false;
	}
	TestEqual(
		TEXT("the rejected path-as-name selector reports unresolved"),
		RejectedError.Code,
		FString(TEXT("niagara_candidate_selector_unresolved")));
	TestNull(
		TEXT("the rejected path-as-name selector creates no module"),
		RejectedNode);
	TArray<UNiagaraNodeFunctionCall*> ModulesAfterRejectedSelector;
	GetModules(*InitialStage->OutputNode, ModulesAfterRejectedSelector);
	TestEqual(
		TEXT("the rejected path-as-name selector does not mutate the stack"),
		ModulesAfterRejectedSelector.Num(),
		InitialModules.Num());
	TestFalse(
		TEXT("the rejected path-as-name selector leaves the package clean"),
		Package->IsDirty());

	TSharedPtr<OperationOwners::UObjectDataAsset::
		IUeAgentUObjectDataAssetCommandService> Service;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	UE::UAI::Contract::FDiagnostic RegistrationError;
	if (!TestTrue(
		TEXT("the production public import service composes"),
		OperationOwners::UObjectDataAsset::
			TryCreateProductionUObjectDataAssetCommandService(
				Service, RegistrationError))
		|| !Service.IsValid()
		|| !TestTrue(
			TEXT("production registration creates exact public dispatch"),
			Module::CommandRegistration::UObjectDataAsset::
				TryCreateUObjectDataAssetRegistration(
					Service.ToSharedRef(), Registry, Dispatch,
					RegistrationError)))
	{
		AddError(RegistrationError.ToPublicDetail());
		return false;
	}
	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / (TEXT("Automation/UAI/T85/NiagaraSystemExport_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	const FString ReadbackWorkspacePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / (TEXT("Automation/UAI/T85/NiagaraSystemReadback_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*WorkspacePath, false, true);
		IFileManager::Get().DeleteDirectory(*ReadbackWorkspacePath, false, true);
	};
	FString CanonicalParams;
	FString ParamsError;
	if (!TestTrue(TEXT("the Niagara System export request canonicalizes"),
		TryBuildNiagaraExportParams(
			ObjectPath, WorkspacePath, CanonicalParams, ParamsError))
		|| !TestTrue(TEXT("content_graph.export_workspace publishes the Niagara System"),
			TrySubmitPublicCommand(
				Registry, Dispatch, TEXT("content_graph.export_workspace"),
				CanonicalParams, ParamsError))
		|| !TestFalse(TEXT("the visible Niagara workspace contains no machine identity"),
			VisibleWorkspaceHasMachineIdentity(WorkspacePath))
		|| !TestTrue(TEXT("the model-visible Niagara stage adds a module by readable alias and candidate name"),
			TryAddVisibleModuleCandidate(
				WorkspacePath, TEXT("SystemUpdateScript"), Selected.Name, ParamsError))
		|| !TestTrue(TEXT("the edited Niagara directory import request canonicalizes"),
			TryBuildNiagaraDirectoryImportParams(
				ObjectPath, WorkspacePath, CanonicalParams, ParamsError)))
	{
		AddError(ParamsError);
		return false;
	}
	const CommandEdge::FUeAgentCommandResult ImportResult =
		SubmitPublicCommand(
			Registry, Dispatch, TEXT("content_graph.import_workspace"),
			CanonicalParams);
	if (!TestTrue(
		TEXT("exact public dispatch invokes the Niagara HJSON provider"),
		ImportResult.IsOk()))
	{
		AddError(ImportResult.GetErrorJson());
		return false;
	}
	const FString ImportPayloadJson = ImportResult.GetPayloadJson();
	if (!TestTrue(TEXT("the persisted Niagara System independently re-exports"),
		TryBuildNiagaraExportParams(
			ObjectPath, ReadbackWorkspacePath, CanonicalParams, ParamsError))
		|| !TestTrue(TEXT("the Niagara System readback export succeeds"),
			TrySubmitPublicCommand(
				Registry, Dispatch, TEXT("content_graph.export_workspace"),
				CanonicalParams, ParamsError)))
	{
		AddError(ParamsError);
		return false;
	}
	TestFalse(TEXT("the independent Niagara readback remains identity-free"),
		VisibleWorkspaceHasMachineIdentity(ReadbackWorkspacePath));

	UeAgentExportContract::FContract NiagaraExportContract;
	TestTrue(
		TEXT("the Niagara export contract is registered and valid"),
		UeAgentExportContract::FRegistry::Get().FindContract(
			UeAgentNiagaraAuthoring::ExportContractId,
			NiagaraExportContract)
			&& NiagaraExportContract.IsValid());

	FString CompositionRegistrationError;
	if (!TestTrue(
			TEXT("the shared non-graph target compositions register"),
			HjsonWorkspace::RegisterOtherTargetCompositionContracts(
				CompositionRegistrationError)))
	{
		AddError(CompositionRegistrationError);
		return false;
	}

	const TCHAR* NiagaraTargetKinds[] = {
		TEXT("ue.niagara_system.asset"),
		TEXT("ue.niagara_emitter.asset"),
		TEXT("ue.niagara_script.asset"),
	};
	for (const TCHAR* TargetKind : NiagaraTargetKinds)
	{
		HjsonWorkspace::FTargetCompositionContract TargetComposition;
		TestTrue(
			FString::Printf(
				TEXT("target composition %s is registered"),
				TargetKind),
			HjsonWorkspace::FTargetCompositionRegistry::Get().FindContract(
				TargetKind,
				TargetComposition));
	}

	TSharedPtr<FJsonObject> ImportPayload;
	FString PayloadError;
	if (!TestTrue(
		TEXT("the Niagara import payload is canonical JSON"),
		CommandContract::Json::TryParseObject(
			ImportPayloadJson, ImportPayload, PayloadError)))
	{
		AddError(PayloadError);
		return false;
	}
	FString TerminalStatus;
	FString ProviderId;
	bool bSaved = false;
	bool bReloaded = false;
	TestTrue(TEXT("the import exposes only its compact success status"),
		ImportPayload.IsValid()
			&& ImportPayload->Values.Num() == 1
			&& ImportPayload->GetStringField(TEXT("status")) == TEXT("succeeded"));

	UNiagaraSystem* const Reloaded = LoadObject<UNiagaraSystem>(
		nullptr, *ObjectPath);
	if (!TestNotNull(
		TEXT("the persisted Niagara System independently reloads"), Reloaded))
	{
		return false;
	}
	TArray<FResolvedStage> ReloadedStages;
	StageError.Reset();
	const FResolvedStage* const ReloadedStage = FindSystemUpdateStage(
		*Reloaded,
		CandidateResult.Context.TargetUsageId,
		ReloadedStages,
		StageError);
	if (!TestNotNull(
		TEXT("the exact System Update output survives reload"), ReloadedStage))
	{
		AddError(StageError);
		return false;
	}
	TArray<UNiagaraNodeFunctionCall*> ReloadedModules;
	GetModules(*ReloadedStage->OutputNode, ReloadedModules);
	TArray<UNiagaraNodeFunctionCall*> MatchingModules;
	for (UNiagaraNodeFunctionCall* ModuleNode : ReloadedModules)
	{
		if (ModuleNode && GetModuleScriptPath(*ModuleNode)
				.Equals(Selected.ScriptAssetPath, ESearchCase::CaseSensitive))
		{
			MatchingModules.Add(ModuleNode);
		}
	}
	const int32 InitialCount = InitialScriptCounts.FindRef(
		Selected.ScriptAssetPath);
	TestEqual(
		TEXT("one searched native module persists in the target stack"),
		MatchingModules.Num(),
		InitialCount + 1);
	for (const auto& Required : InitialScriptCounts)
	{
		int32 Count = 0;
		for (const UNiagaraNodeFunctionCall* Module : ReloadedModules)
			if (Module && GetModuleScriptPath(*Module) == Required.Key) ++Count;
		TestEqual(TEXT("the factory-required module survives public addition"), Count, Required.Value);
	}
	if (!MatchingModules.IsEmpty())
	{
		UNiagaraNodeFunctionCall* const Created = MatchingModules.Last();
		TestNotNull(TEXT("the persisted node resolves its module script"),
			Created->FunctionScript.Get());
		if (Created->FunctionScript
			&& Created->FunctionScript->IsVersioningEnabled())
		{
			TestEqual(
				TEXT("the native action selects the exposed script version"),
				Created->SelectedScriptVersion,
				Created->FunctionScript->GetExposedVersion().VersionGuid);
		}
	}

	TArray<uint8> DiskBefore;
	if (!TestTrue(TEXT("the Niagara disk before image is readable"),
		FFileHelper::LoadFileToArray(DiskBefore, *Filename))
		|| !TestTrue(TEXT("the before stack contains the added optional module"), !MatchingModules.IsEmpty())) return false;
	UNiagaraNodeFunctionCall* DirtyModule = MatchingModules.Last();
	const FGuid DirtyModuleGuid = DirtyModule->NodeGuid;
	FStrProperty* FunctionName = FindFProperty<FStrProperty>(DirtyModule->GetClass(), TEXT("FunctionDisplayName"));
	if (!TestNotNull(TEXT("the native module exposes its serialized name"), FunctionName)) return false;
	FunctionName->SetPropertyValue_InContainer(DirtyModule, TEXT("AtomicDirtyModule"));
	const FString DirtyModuleName = DirtyModule->GetFunctionName();
	Reloaded->MarkPackageDirty();
	const int32 BeforeModuleCount = ReloadedModules.Num();
	UeAgentNiagaraAuthoring::FAssetApplyRequest FailedRequest;
	FailedRequest.AssetPath = PackageName;
	FailedRequest.AssetKind = TEXT("system");
	FailedRequest.Stages = MakeShared<FJsonObject>();
	const TSharedRef<FJsonObject> RemovalStage = MakeShared<FJsonObject>();
	const TSharedRef<FJsonObject> StageTarget = MakeShared<FJsonObject>();
	StageTarget->SetStringField(TEXT("usage"), TEXT("SystemUpdateScript"));
	StageTarget->SetStringField(TEXT("usage_id"), CandidateResult.Context.TargetUsageId);
	RemovalStage->SetObjectField(TEXT("target"), StageTarget);
	RemovalStage->SetStringField(TEXT("operation"), TEXT("update"));
	const TSharedRef<FJsonObject> RemovedModule = MakeShared<FJsonObject>();
	RemovedModule->SetStringField(TEXT("module_guid"), DirtyModuleGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	RemovedModule->SetBoolField(TEXT("remove"), true);
	const TSharedRef<FJsonObject> RemovedModules = MakeShared<FJsonObject>();
	RemovedModules->SetObjectField(TEXT("optional_module"), RemovedModule);
	RemovalStage->SetObjectField(TEXT("modules"), RemovedModules);
	FailedRequest.Stages->SetObjectField(TEXT("system_update"), RemovalStage);
	bool bReachedFinalReadback = false;
	FailedRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
	{
		UNiagaraSystem* Actual = LoadObject<UNiagaraSystem>(nullptr, *ObjectPath);
		TArray<FResolvedStage> ActualStages;
		FString Error;
		const FResolvedStage* ActualStage = Actual ? FindSystemUpdateStage(
			*Actual, CandidateResult.Context.TargetUsageId, ActualStages, Error) : nullptr;
		TArray<UNiagaraNodeFunctionCall*> ActualModules;
		if (ActualStage) GetModules(*ActualStage->OutputNode, ActualModules);
		bReachedFinalReadback = ActualStage && ActualModules.Num() == BeforeModuleCount - 1
			&& !ActualModules.ContainsByPredicate([&](const UNiagaraNodeFunctionCall* Module)
				{ return Module && Module->NodeGuid == DirtyModuleGuid; });
		for (const auto& Required : InitialScriptCounts)
			bReachedFinalReadback &= ActualModules.ContainsByPredicate([&](const UNiagaraNodeFunctionCall* Module)
				{ return Module && GetModuleScriptPath(*Module) == Required.Key; });
		Failure.Diagnostic.Code = TEXT("t85_niagara_final_readback_rejected");
		Failure.Diagnostic.PublicMessage = TEXT("intentional final acceptance failure after persisted stack reload");
		return false;
	};
	UeAgentNiagaraAuthoring::FAssetApplyResult FailedResult;
	TestFalse(TEXT("the final Niagara readback rejection fails the same transaction"),
		UeAgentNiagaraAuthoring::ApplyAsset(FailedRequest, FailedResult,
			TEXT("t85-niagara-final-readback-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TestTrue(TEXT("the rejection observes optional deletion and the required module after real reload"), bReachedFinalReadback);
	TestTrue(TEXT("the Niagara late failure reports semantically confirmed recovery"),
		FailedResult.bRollbackAttempted && FailedResult.bRollbackSucceeded);
	if (!bReachedFinalReadback || !FailedResult.bRollbackSucceeded)
		AddError(FailedResult.Phase + TEXT(":") + FailedResult.Diagnostic.Code + TEXT(":") + FailedResult.Diagnostic.InternalContext);
	UNiagaraSystem* Restored = LoadObject<UNiagaraSystem>(nullptr, *ObjectPath);
	TArray<FResolvedStage> RestoredStages;
	FString RestoreError;
	const FResolvedStage* RestoredStage = Restored ? FindSystemUpdateStage(
		*Restored, CandidateResult.Context.TargetUsageId, RestoredStages, RestoreError) : nullptr;
	TArray<UNiagaraNodeFunctionCall*> RestoredModules;
	if (RestoredStage) GetModules(*RestoredStage->OutputNode, RestoredModules);
	const UNiagaraNodeFunctionCall* RestoredDirty = nullptr;
	for (const UNiagaraNodeFunctionCall* Module : RestoredModules)
		if (Module && Module->NodeGuid == DirtyModuleGuid) RestoredDirty = Module;
	TestTrue(TEXT("checkpoint restores the linked stack and unsaved module name"),
		RestoredStage && RestoredModules.Num() == BeforeModuleCount && RestoredDirty
			&& RestoredDirty->GetFunctionName() == DirtyModuleName);
	TestTrue(TEXT("Niagara restores the original dirty flag"), Restored && Restored->GetOutermost()->IsDirty());
	TArray<uint8> DiskAfter;
	TestTrue(TEXT("Niagara keeps the older disk independently of the dirty module graph"),
		FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskBefore == DiskAfter);

	DeleteAssetIfPresent(ObjectPath);
	UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
		*this, PackageName);
	TestFalse(TEXT("the task-owned Niagara fixture leaves no asset file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT162NiagaraEmitterNativeAssetModuleVerticalTest,
	"UAI.Root.T162.Niagara.EmitterNativeAssetModuleVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT162NiagaraEmitterNativeAssetModuleVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::Niagara;

	const FString PackageName =
		TEXT("/Game/UAI/Transient/T85NiagaraEmitterNativeAssetModule/NE_NativeAssetModule");
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	DeleteAssetIfPresent(ObjectPath);
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
			*this, PackageName);
	};

	UPackage* const Package = CreatePackage(*PackageName);
	UNiagaraEmitter* const Emitter = Package
		? NewObject<UNiagaraEmitter>(
			Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!TestNotNull(
		TEXT("a persistent standalone Niagara Emitter fixture is created"),
		Emitter))
	{
		return false;
	}
	UNiagaraEmitterFactoryNew::InitializeEmitter(Emitter, false);
	FAssetRegistryModule::AssetCreated(Emitter);
	if (!TestTrue(
		TEXT("the initialized standalone Emitter fixture is saved"),
		SaveEmitter(*Package, *Emitter, Filename)))
	{
		return false;
	}

	TArray<FResolvedStage> InitialStages;
	FString StageError;
	const FResolvedStage* const InitialStage = FindEmitterParticleUpdateStage(
		*Emitter, InitialStages, StageError);
	if (!TestNotNull(
		TEXT("the standalone Emitter exposes one Particle Update output"),
		InitialStage))
	{
		AddError(StageError);
		return false;
	}
	TArray<UNiagaraNodeFunctionCall*> InitialModules;
	GetModules(*InitialStage->OutputNode, InitialModules);
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	UE::UAI::Contract::FDiagnostic RegistrationError;
	if (!TestTrue(
		TEXT("the Niagara query and HJSON import public surface composes"),
		TryCreateNiagaraPublicSurface(Registry, Dispatch, RegistrationError)))
	{
		AddError(RegistrationError.ToPublicDetail());
		return false;
	}

	const TSharedRef<FJsonObject> QueryParams = MakeShared<FJsonObject>();
	QueryParams->SetStringField(TEXT("target"), ObjectPath);
	QueryParams->SetStringField(
		TEXT("target_usage"), TEXT("ParticleUpdateScript"));
	QueryParams->SetStringField(TEXT("query"), TEXT("asset_module"));
	QueryParams->SetNumberField(TEXT("limit"), 20);
	FString QueryParamsJson;
	FString JsonError;
	if (!TestTrue(
		TEXT("the bounded keyword query parameters canonicalize"),
		CommandContract::Json::TryCanonicalize(
			QueryParams, QueryParamsJson, JsonError)))
	{
		AddError(JsonError);
		return false;
	}

	const CommandEdge::FUeAgentCommandResult QueryResult = SubmitPublicCommand(
		Registry, Dispatch, TEXT("niagara.module.candidates"), QueryParamsJson);
	if (!TestTrue(
		TEXT("the public keyword search accepts a standalone Emitter"),
		QueryResult.IsOk()))
	{
		AddError(QueryResult.GetErrorJson());
		return false;
	}
	TSharedPtr<FJsonObject> QueryPayload;
	if (!TestTrue(
		TEXT("the public Emitter candidate payload is canonical JSON"),
		CommandContract::Json::TryParseObject(
			QueryResult.GetPayloadJson(), QueryPayload, JsonError))
		|| !QueryPayload.IsValid())
	{
		AddError(JsonError);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* NameValues = nullptr;
	if (!TestTrue(
		TEXT("the public result returns names only"),
		QueryPayload->TryGetArrayField(TEXT("names"), NameValues)
			&& NameValues))
	{
		return false;
	}

	FString CandidateName;
	for (const TSharedPtr<FJsonValue>& Value : *NameValues)
	{
		if (Value.IsValid() && Value->Type == EJson::String
			&& !Value->AsString().IsEmpty())
		{
			CandidateName = Value->AsString();
			break;
		}
	}
	if (!TestFalse(
		TEXT("the keyword search yields a native asset module name"),
		CandidateName.IsEmpty()))
	{
		return false;
	}
	TestFalse(
		TEXT("the public candidate query leaves the standalone Emitter clean"),
		Package->IsDirty());

	FNiagaraModuleCandidateContext PublicContext;
	PublicContext.TargetUsage = TEXT("ParticleUpdateScript");

	FNiagaraModuleCandidate Selected;
	FNiagaraModuleCandidateContext ResolvedContext;
	FNiagaraModuleCandidateQueryError ResolveError;
	if (!TestTrue(
		TEXT("the public name resolves back to one server-side native action"),
		TryResolveNiagaraModuleCandidate(
			*Emitter,
			CandidateName,
			PublicContext,
			Selected,
			ResolvedContext,
			ResolveError)))
	{
		AddError(ResolveError.Code + TEXT(":") + ResolveError.PublicMessage);
		return false;
	}
	if (!TestFalse(
		TEXT("the resolved action has a private script asset payload"),
		Selected.ScriptAssetPath.IsEmpty()))
	{
		return false;
	}
	int32 InitialSelectedCount = 0;
	for (const UNiagaraNodeFunctionCall* Module : InitialModules)
	{
		if (Module
			&& GetModuleScriptPath(*Module).Equals(
				Selected.ScriptAssetPath, ESearchCase::CaseSensitive))
		{
			++InitialSelectedCount;
		}
	}

	FNiagaraModuleCandidate RejectedCandidate;
	FNiagaraModuleCandidateContext RejectedContext;
	UNiagaraNodeFunctionCall* RejectedNode = nullptr;
	FNiagaraModuleCandidateQueryError RejectedError;
	if (!TestFalse(
		TEXT("a private script path remains invalid as a public name selector"),
		TryPerformNiagaraModuleCandidate(
			*Emitter,
			Selected.ScriptAssetPath,
			PublicContext,
			INDEX_NONE,
			RejectedCandidate,
			RejectedContext,
			RejectedNode,
			RejectedError)))
	{
		return false;
	}
	TestEqual(
		TEXT("the invalid selector fails before mutation"),
		RejectedError.Code,
		FString(TEXT("niagara_candidate_selector_unresolved")));
	TestNull(TEXT("the invalid selector creates no node"), RejectedNode);
	TArray<UNiagaraNodeFunctionCall*> AfterRejectedModules;
	GetModules(*InitialStage->OutputNode, AfterRejectedModules);
	TestEqual(
		TEXT("the invalid selector leaves the real Emitter stack unchanged"),
		AfterRejectedModules.Num(), InitialModules.Num());
	TestFalse(
		TEXT("the invalid selector leaves the package clean"),
		Package->IsDirty());

	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / (TEXT("Automation/UAI/T85/NiagaraEmitterExport_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	const FString ReadbackWorkspacePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / (TEXT("Automation/UAI/T85/NiagaraEmitterReadback_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*WorkspacePath, false, true);
		IFileManager::Get().DeleteDirectory(*ReadbackWorkspacePath, false, true);
	};
	FString ImportParams;
	FString ImportParamsError;
	if (!TestTrue(TEXT("the standalone Emitter export request canonicalizes"),
		TryBuildNiagaraExportParams(
			ObjectPath, WorkspacePath, ImportParams, ImportParamsError))
		|| !TestTrue(TEXT("content_graph.export_workspace publishes the standalone Emitter"),
			TrySubmitPublicCommand(
				Registry, Dispatch, TEXT("content_graph.export_workspace"),
				ImportParams, ImportParamsError))
		|| !TestFalse(TEXT("the visible Emitter workspace contains no machine identity"),
			VisibleWorkspaceHasMachineIdentity(WorkspacePath))
		|| !TestTrue(TEXT("the model-visible Emitter module edits by readable alias and candidate name"),
			TryAddVisibleModuleCandidate(
				WorkspacePath, TEXT("ParticleUpdateScript"), CandidateName, ImportParamsError))
		|| !TestTrue(TEXT("the edited Emitter directory import request canonicalizes"),
			TryBuildNiagaraDirectoryImportParams(
				ObjectPath, WorkspacePath, ImportParams, ImportParamsError)))
	{
		AddError(ImportParamsError);
		return false;
	}
	const CommandEdge::FUeAgentCommandResult ImportResult = SubmitPublicCommand(
		Registry,
		Dispatch,
		TEXT("content_graph.import_workspace"),
		ImportParams);
	if (!TestTrue(
		TEXT("the public HJSON import applies the native Emitter action"),
		ImportResult.IsOk()))
	{
		AddError(ImportResult.GetErrorJson());
		return false;
	}
	if (!TestTrue(TEXT("the persisted standalone Emitter independently re-exports"),
		TryBuildNiagaraExportParams(
			ObjectPath, ReadbackWorkspacePath, ImportParams, ImportParamsError))
		|| !TestTrue(TEXT("the standalone Emitter readback export succeeds"),
			TrySubmitPublicCommand(
				Registry, Dispatch, TEXT("content_graph.export_workspace"),
				ImportParams, ImportParamsError)))
	{
		AddError(ImportParamsError);
		return false;
	}
	TestFalse(TEXT("the independent Emitter readback remains identity-free"),
		VisibleWorkspaceHasMachineIdentity(ReadbackWorkspacePath));
	TSharedPtr<FJsonObject> ImportPayload;
	if (!TestTrue(
		TEXT("the standalone Emitter import payload is canonical JSON"),
		CommandContract::Json::TryParseObject(
			ImportResult.GetPayloadJson(), ImportPayload, JsonError))
		|| !ImportPayload.IsValid())
	{
		AddError(JsonError);
		return false;
	}
	FString TerminalStatus;
	FString ProviderId;
	bool bSaved = false;
	bool bReloaded = false;
	TestTrue(
		TEXT("the Emitter import exposes only its compact success status"),
		ImportPayload->Values.Num() == 1
			&& ImportPayload->GetStringField(TEXT("status")) == TEXT("succeeded"));

	UNiagaraEmitter* const Reloaded = LoadObject<UNiagaraEmitter>(
		nullptr, *ObjectPath);
	if (!TestNotNull(
		TEXT("the persisted standalone Emitter reloads"), Reloaded))
	{
		return false;
	}
	TArray<FResolvedStage> ReloadedStages;
	StageError.Reset();
	const FResolvedStage* const ReloadedStage = FindEmitterParticleUpdateStage(
		*Reloaded, ReloadedStages, StageError);
	if (!TestNotNull(
		TEXT("the Emitter Particle Update output survives reload"),
		ReloadedStage))
	{
		AddError(StageError);
		return false;
	}
	TArray<UNiagaraNodeFunctionCall*> ReloadedModules;
	GetModules(*ReloadedStage->OutputNode, ReloadedModules);
	int32 ReloadedSelectedCount = 0;
	for (UNiagaraNodeFunctionCall* Module : ReloadedModules)
	{
		if (Module
			&& GetModuleScriptPath(*Module).Equals(
				Selected.ScriptAssetPath, ESearchCase::CaseSensitive))
		{
			++ReloadedSelectedCount;
			if (Module->FunctionScript
				&& Module->FunctionScript->IsVersioningEnabled())
			{
				TestEqual(
					TEXT("the Emitter action selects the exposed script version"),
					Module->SelectedScriptVersion,
					Module->FunctionScript->GetExposedVersion().VersionGuid);
			}
		}
	}
	TestEqual(
		TEXT("one searched native module persists in the Emitter stack"),
		ReloadedSelectedCount,
		InitialSelectedCount + 1);

	DeleteAssetIfPresent(ObjectPath);
	UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
		*this, PackageName);
	TestFalse(
		TEXT("the task-owned Emitter fixture leaves no asset file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT208NiagaraEmitterScopedPropertyVerticalTest,
	"UAI.Root.T208.Niagara.EmitterScopedPropertyVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
int32 SetVisibleEmitterRootText(const TSharedPtr<FJsonObject>& Document, const FString& Desired)
{
	if (!Document.IsValid()) return 0;
	int32 Changed = 0;
	for (auto& Pair : Document->Values)
	{
		if (MatchesPropertyName(Pair.Key, TEXT("TemplateAssetDescription"))
			|| MatchesPropertyName(Pair.Key, TEXT("AssetDescription")))
		{
			// Follow the public FText literal form; leave its type and enclosing
			// property metadata intact. This fixture starts with an empty literal.
			if (Pair.Value->Type == EJson::String)
			{
				Pair.Value = MakeShared<FJsonValueString>(Desired);
				++Changed;
				continue;
			}
			TSharedPtr<FJsonObject> Value = Pair.Value->Type == EJson::Object ? Pair.Value->AsObject() : nullptr;
			if (Value.IsValid() && Value->HasTypedField<EJson::String>(TEXT("value")))
			{
				Value->SetStringField(TEXT("value"), Desired);
				++Changed;
				continue;
			}
			if (Value.IsValid() && Value->HasTypedField<EJson::Object>(TEXT("value")))
				Value = Value->GetObjectField(TEXT("value"));
			if (Value.IsValid() && Value->HasTypedField<EJson::String>(TEXT("literal")))
			{
				Value->SetStringField(TEXT("literal"), Desired);
				++Changed;
			}
		}
		else if (Pair.Value->Type == EJson::Object)
			Changed += SetVisibleEmitterRootText(Pair.Value->AsObject(), Desired);
	}
	return Changed;
}

bool RunStandaloneEmitterPropertyTest(FAutomationTestBase& Test, const bool bRootText)
{
	using namespace UE::UAI;
	const FString PackageName =
		bRootText ? TEXT("/Game/UAI/Transient/T85NiagaraEmitterRootProperty/NE_RootProperty")
		: TEXT("/Game/UAI/Transient/T85NiagaraEmitterScopedProperty/NE_ScopedProperty");
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	DeleteAssetIfPresent(ObjectPath);
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test, PackageName);
	};

	UPackage* const Package = CreatePackage(*PackageName);
	UNiagaraEmitter* const Emitter = Package
		? NewObject<UNiagaraEmitter>(
			Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!Test.TestNotNull(TEXT("a persistent scoped property Emitter is created"), Emitter)) return false;
	UNiagaraEmitterFactoryNew::InitializeEmitter(Emitter, false);
	FVersionedNiagaraEmitterData* const InitialData = Emitter->GetLatestEmitterData();
	if (!Test.TestNotNull(TEXT("the scoped property Emitter exposes versioned data"), InitialData)) return false;
	InitialData->CalculateBoundsMode = ENiagaraEmitterCalculateBoundMode::Fixed;
	InitialData->FixedBounds = FBox(
		FVector(-10.0, -20.0, -30.0), FVector(10.0, 20.0, 30.0));
	InitialData->bLocalSpace = true;
	const FString DesiredDescription = TEXT("T208 emitter description: preserved native module / 根属性文本");
	if (bRootText)
	{
		const FTextProperty* Description = FindFProperty<FTextProperty>(Emitter->GetClass(), TEXT("TemplateAssetDescription"));
		if (!Test.TestTrue(TEXT("the root description is a persistent editable native FText"),
			Description && Description->HasAnyPropertyFlags(CPF_Edit)
			&& !Description->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))) return false;
		Emitter->TemplateAssetDescription = FText::GetEmpty();
		Emitter->bIsInheritable = false;
	}
	using namespace QueryOwners::Niagara;
	FNiagaraModuleCandidateContext SeedContext;
	SeedContext.TargetUsage = TEXT("ParticleUpdateScript");
	FNiagaraModuleCandidateQueryResult SeedCandidates;
	FNiagaraModuleCandidateQueryError SeedError;
	if (!Test.TestTrue(TEXT("fixture searches the existing native module action owner"),
		TryQueryNiagaraModuleCandidates(*Emitter, TEXT("asset_module"), SeedContext,
			SeedCandidates, SeedError)))
	{
		Test.AddError(SeedError.Code + TEXT(":") + SeedError.PublicMessage);
		return false;
	}
	const auto* Seed = SeedCandidates.Candidates.FindByPredicate(
		[](const FNiagaraModuleCandidate& Candidate) { return Candidate.bSupported && !Candidate.Name.IsEmpty(); });
	if (!Test.TestNotNull(TEXT("fixture has a supported native module candidate"), Seed)) return false;
	FNiagaraModuleCandidate SeedSelected;
	FNiagaraModuleCandidateContext SeedResolved;
	UNiagaraNodeFunctionCall* SeedModule = nullptr;
	if (!Test.TestTrue(TEXT("fixture contains a real native module before property editing"),
		TryPerformNiagaraModuleCandidate(*Emitter, Seed->Name, SeedContext, INDEX_NONE,
			SeedSelected, SeedResolved, SeedModule, SeedError))
		|| !Test.TestNotNull(TEXT("native fixture module exists"), SeedModule))
	{
		Test.AddError(SeedError.Code + TEXT(":") + SeedError.PublicMessage);
		return false;
	}
	SeedModule->NodeComment = TEXT("scoped-property non-target module content");
	const FGuid SeedModuleGuid = SeedModule->NodeGuid;
	FAssetRegistryModule::AssetCreated(Emitter);
	FString CompileError;
	if (!Test.TestTrue(TEXT("the baseline is compiled before preservation capture"),
		UeAgentNiagaraAuthoring::CompileAsset(Emitter, CompileError)))
	{
		Test.AddError(CompileError);
		return false;
	}
	if (!Test.TestTrue(TEXT("the scoped property Emitter fixture is saved"),
		SaveEmitter(*Package, *Emitter, Filename))) return false;
	const auto InitialContent = CaptureEmitterContent(*Emitter, true);
	const FGuid InitialVersion = InitialData->Version.VersionGuid;
	const FGuid InitialExposedVersion = Emitter->GetExposedVersion().VersionGuid;
	const FGuid InitialEmitterChangeId = Emitter->GetChangeId();
	const bool bInitialVersioning = Emitter->IsVersioningEnabled();
	TMap<FName, FString> InitialOtherRoot;
	if (bRootText)
	{
		if (!Test.TestTrue(TEXT("the standalone root fixture has exactly one selected native version"),
			Emitter->GetAllAvailableVersions().Num() == 1
			&& Emitter->GetEmitterData(InitialExposedVersion) == InitialData)) return false;
		for (TFieldIterator<FProperty> It(Emitter->GetClass()); It; ++It)
		{
			// VersionData is compared field by field by CaptureEmitterContent;
			// only the description is edited and bounds are checked independently.
			if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)
				|| It->GetFName() == TEXT("VersionData") || It->GetFName() == TEXT("TemplateAssetDescription")) continue;
			FString Value;
			It->ExportTextItem_Direct(Value, It->ContainerPtrToValuePtr<void>(Emitter), nullptr, Emitter, PPF_Delimited);
			InitialOtherRoot.Add(It->GetFName(), MoveTemp(Value));
		}
	}
	TArray<FResolvedStage> InitialStages;
	FString StageError;
	const FResolvedStage* const InitialStage = FindEmitterParticleUpdateStage(
		*Emitter, InitialStages, StageError);
	TArray<UNiagaraNodeFunctionCall*> InitialModules;
	if (InitialStage) GetModules(*InitialStage->OutputNode, InitialModules);
	if (!Test.TestNotNull(TEXT("the initial Emitter stack is readable"), InitialStage))
	{
		Test.AddError(StageError);
		return false;
	}
	if (!Test.TestTrue(TEXT("the preserved stack contains the exact seeded module"),
		InitialModules.ContainsByPredicate([&](const UNiagaraNodeFunctionCall* Module)
			{ return Module && Module->NodeGuid == SeedModuleGuid; }))) return false;

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!Test.TestTrue(TEXT("the public Niagara HJSON surface composes"),
		TryCreateNiagaraPublicSurface(Registry, Dispatch, Error)))
	{
		Test.AddError(Error);
		return false;
	}
	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / (TEXT("Automation/UAI/T208/NiagaraEmitterScopedExport_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	const FString ReadbackWorkspacePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / (TEXT("Automation/UAI/T208/NiagaraEmitterScopedReadback_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*WorkspacePath, false, true);
		IFileManager::Get().DeleteDirectory(*ReadbackWorkspacePath, false, true);
	};
	FString ParamsJson;
	if (!Test.TestTrue(TEXT("the scoped Emitter export request canonicalizes"),
		TryBuildNiagaraExportParams(ObjectPath, WorkspacePath, ParamsJson, Error))
		|| !Test.TestTrue(TEXT("the public scoped Emitter export succeeds"),
			TrySubmitPublicCommand(
				Registry, Dispatch, TEXT("content_graph.export_workspace"), ParamsJson, Error))
		|| !Test.TestFalse(TEXT("the visible scoped Emitter export contains no machine identity"),
			VisibleWorkspaceHasMachineIdentity(WorkspacePath)))
	{
		Test.AddError(Error);
		return false;
	}
	TMap<FString, FString> VisibleExpected;
	if (bRootText)
	{
		VisibleExpected = CaptureVisibleOwnerFiles(WorkspacePath, FString());
		int32 ChangedLeaves = 0;
		for (auto& Expected : VisibleExpected)
		{
			TSharedPtr<FJsonObject> Document;
			if (!FUeAgentStructuredHjsonParser::ParseObject(Expected.Value, Expected.Key, Document, Error)) return false;
			const int32 Changed = SetVisibleEmitterRootText(Document, DesiredDescription);
			if (Changed == 0) continue;
			ChangedLeaves += Changed;
			if (!CommandContract::Json::TryCanonicalize(Document.ToSharedRef(), Expected.Value, Error)
				|| !FFileHelper::SaveStringToFile(Expected.Value, *(WorkspacePath / Expected.Key))) return false;
		}
		if (!Test.TestEqual(TEXT("the ordinary workspace edits exactly one root description leaf"), ChangedLeaves, 1))
		{
			for (const auto& Entry : VisibleExpected)
			{
				const int32 Position = Entry.Value.Find(TEXT("Description"));
				if (Position != INDEX_NONE) Test.AddError(Entry.Key + TEXT(": ")
					+ Entry.Value.Mid(FMath::Max(0, Position - 60), 350));
			}
			return false;
		}
	}
	else if (!Test.TestTrue(TEXT("the visible scoped Emitter accepts a compact nested struct edit"),
		TryEditVisibleEmitterFixedBoundsMinX(WorkspacePath, -42.5, Error))) return false;
	if (!Test.TestTrue(TEXT("the Emitter import request canonicalizes"),
		TryBuildNiagaraDirectoryImportParams(ObjectPath, WorkspacePath, ParamsJson, Error))) return false;
	const CommandEdge::FUeAgentCommandResult ImportResult = SubmitPublicCommand(
		Registry, Dispatch, TEXT("content_graph.import_workspace"), ParamsJson);
	if (!Test.TestTrue(TEXT("the public import applies the compact scoped struct edit"),
		ImportResult.IsOk()))
	{
		Test.AddError(ImportResult.GetErrorJson());
		return false;
	}
	UNiagaraEmitter* Published = FindObject<UNiagaraEmitter>(nullptr, *ObjectPath);
	if (!Test.TestNotNull(TEXT("public import leaves a published emitter"), Published)) return false;
	TWeakObjectPtr<UNiagaraEmitter> PublishedWeak(Published);
	UPackage* PublishedPackage = Published->GetOutermost();
	Test.TestFalse(TEXT("public import completed with a clean package"), PublishedPackage->IsDirty());
	Published = nullptr;
	FText UnloadError;
	if (!Test.TestTrue(TEXT("independent package unload succeeds"),
		UPackageTools::UnloadPackages({PublishedPackage}, UnloadError, true)))
	{
		Test.AddError(UnloadError.ToString());
		return false;
	}
	if (!Test.TestFalse(TEXT("the independently unloaded emitter no longer exists"), PublishedWeak.IsValid())) return false;
	UNiagaraEmitter* const Reloaded = LoadObject<UNiagaraEmitter>(nullptr, *ObjectPath);
	FVersionedNiagaraEmitterData* const ReloadedData =
		Reloaded ? Reloaded->GetLatestEmitterData() : nullptr;
	TArray<FResolvedStage> ReloadedStages;
	StageError.Reset();
	const FResolvedStage* const ReloadedStage = Reloaded
		? FindEmitterParticleUpdateStage(*Reloaded, ReloadedStages, StageError)
		: nullptr;
	TArray<UNiagaraNodeFunctionCall*> ReloadedModules;
	if (ReloadedStage) GetModules(*ReloadedStage->OutputNode, ReloadedModules);
	if (!Test.TestTrue(TEXT("all bounds components and validity survive independent disk reload"),
		HasExactBounds(Reloaded, bRootText ? -10.0 : -42.5))) return false;
	if (!SameEmitterContent(Test, InitialContent, *Reloaded, TEXT("public reload owned preservation"), true)) return false;
	if (!Test.TestTrue(TEXT("the persisted scoped Emitter independently re-exports"),
		TryBuildNiagaraExportParams(ObjectPath, ReadbackWorkspacePath, ParamsJson, Error))
		|| !Test.TestTrue(TEXT("the public scoped Emitter readback export succeeds"),
			TrySubmitPublicCommand(
				Registry, Dispatch, TEXT("content_graph.export_workspace"), ParamsJson, Error)))
	{
		Test.AddError(Error);
		return false;
	}
	double ReexportedMinX = 0.0;
	Test.TestTrue(TEXT("the public re-export contains the persisted compact struct value"),
		TryReadVisibleEmitterFixedBoundsMinX(
			ReadbackWorkspacePath, ReexportedMinX, Error)
			&& FMath::IsNearlyEqual(ReexportedMinX, bRootText ? -10.0 : -42.5));

	if (bRootText)
	{
		Test.TestEqual(TEXT("the exact nondefault FText survives independent disk reload"),
			Reloaded->TemplateAssetDescription.ToString(), DesiredDescription);
		Test.TestTrue(TEXT("the standalone selected version and exposed version retain their native owner"),
			ReloadedData && ReloadedData->Version.VersionGuid == InitialVersion
			&& Reloaded->GetAllAvailableVersions().Num() == 1
			&& Reloaded->IsVersioningEnabled() == bInitialVersioning
			&& Reloaded->GetExposedVersion().VersionGuid == InitialExposedVersion
			&& Reloaded->GetEmitterData(InitialExposedVersion) == ReloadedData);
		// Native PostEditChangeProperty calls UpdateChangeId; this revision is
		// expected to advance on a successful root edit, not an authoring field
		// to preserve. Dirty rollback comparisons retain their exact before ID.
		Test.TestTrue(TEXT("native emitter revision advances for the successful root edit"),
			Reloaded->GetChangeId().IsValid() && Reloaded->GetChangeId() != InitialEmitterChangeId);
		for (const auto& Expected : InitialOtherRoot)
		{
			if (Expected.Key == TEXT("ChangeId")) continue;
			const FProperty* Property = FindFProperty<FProperty>(Reloaded->GetClass(), Expected.Key);
			FString Actual;
			if (Property) Property->ExportTextItem_Direct(Actual,
				Property->ContainerPtrToValuePtr<void>(Reloaded), nullptr, Reloaded, PPF_Delimited);
			Test.TestTrue(*FString::Printf(TEXT("other native emitter root field is unchanged: %s"), *Expected.Key.ToString()),
				Property && Actual == Expected.Value);
		}
		const auto VisibleActual = CaptureVisibleOwnerFiles(ReadbackWorkspacePath, FString());
		if (!Test.TestTrue(TEXT("all public documents match the predetermined description-only edit"),
			VisibleActual.OrderIndependentCompareEqual(VisibleExpected)))
		{
			TFunction<TSharedPtr<FJsonValue>(const TSharedPtr<FJsonValue>&)> RedactIdentity = [&](const TSharedPtr<FJsonValue>& Value) -> TSharedPtr<FJsonValue>
			{
				if (Value->Type != EJson::Object) return Value;
				auto Safe = MakeShared<FJsonObject>();
				for (const auto& Field : Value->AsObject()->Values)
				{
					if (Field.Key == TEXT("namespace") || Field.Key == TEXT("key"))
						Safe->SetBoolField(Field.Key + TEXT("_present"), Field.Value->Type == EJson::String && !Field.Value->AsString().IsEmpty());
					else Safe->SetField(Field.Key, RedactIdentity(Field.Value));
				}
				return MakeShared<FJsonValueObject>(Safe);
			};
			TFunction<void(const TSharedPtr<FJsonObject>&, const FString&)> Describe = [&](const TSharedPtr<FJsonObject>& Document, const FString& Label)
			{
				for (const auto& Field : Document->Values)
				{
					if (MatchesPropertyName(Field.Key, TEXT("TemplateAssetDescription")) || MatchesPropertyName(Field.Key, TEXT("AssetDescription")))
						Test.AddInfo(Label + TEXT(": ") + UeAgentAuthoringState::MakeStableCanonicalValueKey(RedactIdentity(Field.Value)));
					else if (Field.Value->Type == EJson::Object) Describe(Field.Value->AsObject(), Label);
				}
			};
			for (const auto& Entry : VisibleActual)
			{
				const FString* Before = VisibleExpected.Find(Entry.Key);
				if (Before && *Before == Entry.Value) continue;
				Test.AddInfo(TEXT("changed public document: ") + Entry.Key);
				TSharedPtr<FJsonObject> Document;
				if (Before && FUeAgentStructuredHjsonParser::ParseObject(*Before, Entry.Key, Document, Error)) Describe(Document, TEXT("expected description"));
				if (FUeAgentStructuredHjsonParser::ParseObject(Entry.Value, Entry.Key, Document, Error)) Describe(Document, TEXT("actual description"));
			}
		}
		if (Test.HasAnyErrors()) return false;
		const FText FirstText = Reloaded->TemplateAssetDescription;
		const FGuid FirstRevision = Reloaded->GetChangeId();
		TArray<uint8> FirstDisk, UnchangedDisk;
		if (!FFileHelper::LoadFileToArray(FirstDisk, *Filename)
			|| !TryBuildNiagaraDirectoryImportParams(ObjectPath, ReadbackWorkspacePath, ParamsJson, Error)) return false;
		const auto UnchangedResult = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ParamsJson);
		TSharedPtr<FJsonObject> UnchangedError;
		FString UnchangedCode;
		FUeAgentStructuredHjsonParser::ParseObject(UnchangedResult.GetErrorJson(), TEXT("unchanged result"), UnchangedError, Error);
		if (UnchangedError) UnchangedError->TryGetStringField(TEXT("code"), UnchangedCode);
		if (!Test.TestTrue(TEXT("unchanged ordinary text returns the public no-change outcome"),
			!UnchangedResult.IsOk() && UnchangedCode == TEXT("hjson_import_no_explicit_changes")))
		{ Test.AddError(UnchangedResult.GetErrorJson()); return false; }
		UNiagaraEmitter* Unchanged = FindObject<UNiagaraEmitter>(nullptr, *ObjectPath);
		if (!Test.TestNotNull(TEXT("unchanged import retains the emitter"), Unchanged)) return false;
		Test.TestTrue(TEXT("unchanged ordinary source retains exact text identity and native revision"),
			FTextInspector::GetNamespace(Unchanged->TemplateAssetDescription) == FTextInspector::GetNamespace(FirstText)
			&& FTextInspector::GetKey(Unchanged->TemplateAssetDescription) == FTextInspector::GetKey(FirstText)
			&& Unchanged->TemplateAssetDescription.ToString() == DesiredDescription
			&& Unchanged->GetChangeId() == FirstRevision);
		Test.TestTrue(TEXT("unchanged import leaves disk bytes untouched"),
			FFileHelper::LoadFileToArray(UnchangedDisk, *Filename) && UnchangedDisk == FirstDisk);
		const FString SecondDescription = TEXT("Second ordinary emitter source / 第二次文本编辑");
		auto SecondExpected = VisibleExpected;
		int32 SecondLeaves = 0;
		for (auto& Expected : SecondExpected)
		{
			TSharedPtr<FJsonObject> Document;
			if (!FUeAgentStructuredHjsonParser::ParseObject(Expected.Value, Expected.Key, Document, Error)) return false;
			const int32 Changed = SetVisibleEmitterRootText(Document, SecondDescription);
			if (!Changed) continue;
			SecondLeaves += Changed;
			if (!CommandContract::Json::TryCanonicalize(Document.ToSharedRef(), Expected.Value, Error)
				|| !FFileHelper::SaveStringToFile(Expected.Value, *(ReadbackWorkspacePath / Expected.Key))) return false;
		}
		if (!Test.TestEqual(TEXT("second ordinary source edits exactly the predetermined leaf"), SecondLeaves, 1)) return false;
		const auto SecondResult = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ParamsJson);
		if (!Test.TestTrue(TEXT("second ordinary source edit applies through public import"), SecondResult.IsOk()))
		{ Test.AddError(SecondResult.GetErrorJson()); return false; }
		UNiagaraEmitter* SecondPublished = FindObject<UNiagaraEmitter>(nullptr, *ObjectPath);
		if (!SecondPublished) return false;
		const TWeakObjectPtr<UNiagaraEmitter> SecondWeak(SecondPublished);
		UPackage* SecondPackage = SecondPublished->GetPackage();
		SecondPublished = nullptr;
		Unchanged = nullptr;
		if (!Test.TestTrue(TEXT("second edit independently unloads"), UPackageTools::UnloadPackages({SecondPackage}, UnloadError, true))
			|| !Test.TestFalse(TEXT("second published object is gone before reload"), SecondWeak.IsValid())) return false;
		UNiagaraEmitter* SecondReloaded = LoadObject<UNiagaraEmitter>(nullptr, *ObjectPath);
		if (!Test.TestNotNull(TEXT("second edit reloads a fresh emitter"), SecondReloaded)) return false;
		Test.TestEqual(TEXT("second source persists exactly"), SecondReloaded->TemplateAssetDescription.ToString(), SecondDescription);
		Test.TestFalse(TEXT("second reloaded package is clean"), SecondReloaded->GetPackage()->IsDirty());
		if (!SameEmitterContent(Test, InitialContent, *SecondReloaded, TEXT("second source edit preserves all native owned content"), true)) return false;
		const FString SecondReadback = ReadbackWorkspacePath + TEXT("-second");
		if (!TryBuildNiagaraExportParams(ObjectPath, SecondReadback, ParamsJson, Error)
			|| !TrySubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), ParamsJson, Error))
		{ Test.AddError(Error); return false; }
		Test.TestTrue(TEXT("second public export matches only the predetermined source change"),
			CaptureVisibleOwnerFiles(SecondReadback, FString()).OrderIndependentCompareEqual(SecondExpected));
		return !Test.HasAnyErrors();
	}

	TArray<uint8> DiskBeforeFailure;
	if (!Test.TestTrue(TEXT("the scoped property disk before image is readable"),
		FFileHelper::LoadFileToArray(DiskBeforeFailure, *Filename))
		|| !Test.TestNotNull(TEXT("the reloaded Emitter data remains available"), ReloadedData)) return false;
	const double DirtyMinX = -111.25;
	const double DesiredLateMinX = -222.5;
	Reloaded->Modify();
	ReloadedData->FixedBounds.Min.X = DirtyMinX;
	ReloadedData->bLocalSpace = false;
	UNiagaraNodeFunctionCall** DirtyModule = ReloadedModules.FindByPredicate(
		[&](const UNiagaraNodeFunctionCall* Module) { return Module && Module->NodeGuid == SeedModuleGuid; });
	if (!Test.TestNotNull(TEXT("the reloaded stack retains the original module identity"), DirtyModule)) return false;
	(*DirtyModule)->NodeComment = TEXT("dirty module content distinct from disk");
	Reloaded->MarkPackageDirty();
	const auto DirtyContent = CaptureEmitterContent(*Reloaded);
	const auto DirtyAuthoringContent = CaptureEmitterContent(*Reloaded, true);
	const TWeakObjectPtr<UNiagaraEmitter> DirtyWeak(Reloaded);
	// Partial Struct with an unwrapped compact Vector: common completion must
	// retain untouched Max and IsValid from current native data.
	const auto BoundsFields = MakeShared<FJsonObject>();
	BoundsFields->SetArrayField(TEXT("Min"), {MakeShared<FJsonValueNumber>(DesiredLateMinX),
		MakeShared<FJsonValueNumber>(-20.0), MakeShared<FJsonValueNumber>(-30.0)});
	const auto Bounds = MakeShared<FJsonObject>();
	Bounds->SetObjectField(TEXT("fields"), BoundsFields);
	const auto Descriptor = MakeShared<FJsonObject>();
	Descriptor->SetStringField(TEXT("property_path"), TEXT("FixedBounds"));
	Descriptor->SetObjectField(TEXT("value"), Bounds);
	const auto LateProperties = MakeShared<FJsonObject>();
	LateProperties->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
	LateProperties->SetArrayField(TEXT("properties"), {MakeShared<FJsonValueObject>(Descriptor)});
	UeAgentNiagaraAuthoring::FAssetApplyRequest FailedRequest;
	FailedRequest.bCompactValues = true;
	FailedRequest.AssetPath = PackageName;
	FailedRequest.AssetKind = TEXT("emitter");
	FailedRequest.Emitters = MakeShared<FJsonObject>();
	const TSharedRef<FJsonObject> EmitterPatch = MakeShared<FJsonObject>();
	const TSharedRef<FJsonObject> EmitterTarget = MakeShared<FJsonObject>();
	EmitterPatch->SetObjectField(TEXT("target"), EmitterTarget);
	EmitterPatch->SetObjectField(TEXT("properties"), LateProperties);
	FailedRequest.Emitters->SetObjectField(TEXT("standalone"), EmitterPatch);
	const TSharedRef<FEmitterPropertyStageObserver> StageObserver =
		MakeShared<FEmitterPropertyStageObserver>(
			ObjectPath, Filename, DiskBeforeFailure, DesiredLateMinX);
	FailedRequest.PackageStager = StageObserver;
	bool bReachedFinalReadback = false;
	FailedRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
	{
		UNiagaraEmitter* const Actual = LoadObject<UNiagaraEmitter>(nullptr, *ObjectPath);
		const FVersionedNiagaraEmitterData* const ActualData =
			Actual ? Actual->GetLatestEmitterData() : nullptr;
		TArray<uint8> PublishedDisk;
		bReachedFinalReadback = HasExactBounds(Actual, DesiredLateMinX)
			&& !DirtyWeak.IsValid() && Actual && !Actual->GetOutermost()->IsDirty()
			&& FFileHelper::LoadFileToArray(PublishedDisk, *Filename)
			&& PublishedDisk != DiskBeforeFailure;
		if (Actual) bReachedFinalReadback &= SameEmitterContent(
			Test, DirtyAuthoringContent, *Actual, TEXT("late new reload owned preservation"), true);
		Failure.Diagnostic.Code = TEXT("t208_niagara_emitter_property_final_readback_rejected");
		Failure.Diagnostic.PublicMessage = TEXT("intentional rejection after persisted scoped property reload");
		return false;
	};
	UeAgentNiagaraAuthoring::FAssetApplyResult FailedResult;
	Test.TestFalse(TEXT("the intentional late scoped property rejection fails"),
		UeAgentNiagaraAuthoring::ApplyAsset(
			FailedRequest, FailedResult, TEXT("t208-niagara-emitter-property-late")));
	Test.TestEqual(TEXT("failure reached the intended final callback"), FailedResult.Diagnostic.Code,
		FString(TEXT("t208_niagara_emitter_property_final_readback_rejected")));
	Test.TestTrue(TEXT("the stager observes desired memory before original disk publication"),
		StageObserver->bObservedDesiredInMemory && StageObserver->bObservedOriginalDisk);
	Test.TestTrue(TEXT("the final callback observes the clean persisted scoped value"), bReachedFinalReadback);
	Test.TestTrue(TEXT("the late scoped failure reports successful rollback"),
		FailedResult.bRollbackAttempted && FailedResult.bRollbackSucceeded);
	UNiagaraEmitter* const Restored = LoadObject<UNiagaraEmitter>(nullptr, *ObjectPath);
	FVersionedNiagaraEmitterData* const RestoredData =
		Restored ? Restored->GetLatestEmitterData() : nullptr;
	Test.TestTrue(TEXT("rollback restores the original dirty scoped Emitter state"),
		HasExactBounds(Restored, DirtyMinX)
			&& Restored && Restored->GetOutermost()->IsDirty());
	if (Restored) SameEmitterContent(Test, DirtyContent, *Restored, TEXT("rollback complete owned content"));
	TArray<uint8> DiskAfterFailure;
	Test.TestTrue(TEXT("rollback restores the different original Emitter disk"),
		FFileHelper::LoadFileToArray(DiskAfterFailure, *Filename)
			&& DiskAfterFailure == DiskBeforeFailure);
	return !Test.HasAnyErrors();
}

} // namespace

bool FUeAgentT208NiagaraEmitterScopedPropertyVerticalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	return RunStandaloneEmitterPropertyTest(*this, false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT208NiagaraEmitterRootPropertyVerticalTest,
	"UAI.Root.T208.Niagara.EmitterRootPropertyVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208NiagaraEmitterRootPropertyVerticalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	return RunStandaloneEmitterPropertyTest(*this, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208NiagaraEmitterCreateTextVerticalTest,
	"UAI.Root.T208.Niagara.EmitterCreateTextVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208NiagaraEmitterCreateTextVerticalTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	(void)Parameters;
	const FString PackageName = TEXT("/Game/UAI/Transient/T85NiagaraEmitterCreateText/NE_CreateText");
	const FString ObjectPath = PackageName + TEXT(".NE_CreateText");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	auto& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	if (!TestNull(TEXT("create starts without a target package"), FindPackage(nullptr, *PackageName))
		|| !TestNull(TEXT("create starts without a target object"), FindObject<UObject>(nullptr, *ObjectPath))
		|| !TestFalse(TEXT("create starts without a registered asset"), AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid())
		|| !TestFalse(TEXT("create starts without a disk package"), IFileManager::Get().FileExists(*Filename))) return false;
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TryCreateNiagaraPublicSurface(Registry, Dispatch, Error)) { AddError(Error); return false; }
	const FString DesiredSource = TEXT("Created emitter source / 创建文本");
	auto Literal = MakeShared<FJsonObject>(); Literal->SetStringField(TEXT("literal"), DesiredSource);
	auto Property = MakeShared<FJsonObject>();
	Property->SetStringField(TEXT("property_path"), TEXT("TemplateAssetDescription"));
	Property->SetObjectField(TEXT("value"), Literal);
	auto Target = MakeShared<FJsonObject>();
	Target->SetStringField(TEXT("asset_path"), PackageName);
	Target->SetStringField(TEXT("asset_kind"), TEXT("emitter"));
	auto Document = MakeShared<FJsonObject>();
	Document->SetStringField(TEXT("operation"), TEXT("create"));
	Document->SetObjectField(TEXT("target"), Target);
	Document->SetArrayField(TEXT("properties"), {MakeShared<FJsonValueObject>(Property)});
	auto Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("type"), TEXT("/Script/Niagara.NiagaraEmitter"));
	Root->SetObjectField(TEXT("document"), Document);
	auto Hjson = MakeShared<FJsonObject>();
	Hjson->SetStringField(TEXT("schema"), TEXT("uai.niagara.edit/1"));
	Hjson->SetObjectField(TEXT("root"), Root);
	FString DocumentText;
	if (!CommandContract::Json::TryCanonicalize(Hjson, DocumentText, Error)) { AddError(Error); return false; }
	auto Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("inline"));
	Source->SetStringField(TEXT("document"), DocumentText);
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("target"), ObjectPath);
	Params->SetObjectField(TEXT("source"), Source);
	FString ParamsJson;
	if (!CommandContract::Json::TryCanonicalize(Params, ParamsJson, Error)) { AddError(Error); return false; }
	const auto Result = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ParamsJson);
	// Observe the public return before test cleanup or another load can change the terminal state.
	UObject* ReturnedObject = FindObject<UObject>(nullptr, *ObjectPath);
	UPackage* ReturnedPackage = FindPackage(nullptr, *PackageName);
	const bool bRegistered = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
	const bool bDisk = IFileManager::Get().FileExists(*Filename);
	AddInfo(FString::Printf(TEXT("create terminal before cleanup: ok=%d object=%d package=%d registered=%d disk=%d dirty=%d"),
		Result.IsOk(), ReturnedObject != nullptr, ReturnedPackage != nullptr, bRegistered, bDisk, ReturnedPackage && ReturnedPackage->IsDirty()));
	if (!Result.IsOk())
	{
		AddError(Result.GetErrorJson());
		TestNull(TEXT("failed create leaves no target object"), ReturnedObject);
		TestFalse(TEXT("failed create leaves no registry asset"), bRegistered);
		TestFalse(TEXT("failed create leaves no published disk"), bDisk);
		return false;
	}
	auto* Emitter = Cast<UNiagaraEmitter>(ReturnedObject);
	if (!TestNotNull(TEXT("public create returns a native emitter"), Emitter)) return false;
	TestEqual(TEXT("public create initializes exact text source"), Emitter->TemplateAssetDescription.ToString(), DesiredSource);
	const auto CreatedContent = CaptureEmitterContent(*Emitter, true);
	TArray<FResolvedStage> CreatedStages;
	const auto* Stage = FindEmitterParticleUpdateStage(*Emitter, CreatedStages, Error);
	TArray<UNiagaraNodeFunctionCall*> CreatedModules;
	if (Stage) GetModules(*Stage->OutputNode, CreatedModules);
	if (!TestTrue(TEXT("native factory creates a nonempty module stack"), !CreatedModules.IsEmpty())) return false;
	TMap<FName, FString> CreatedRoot;
	for (TFieldIterator<FProperty> It(Emitter->GetClass()); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_TextExportTransient)) continue;
		FString Value;
		It->ExportTextItem_Direct(Value, It->ContainerPtrToValuePtr<void>(Emitter), nullptr, Emitter, PPF_Delimited);
		CreatedRoot.Add(It->GetFName(), Value);
	}
	const TWeakObjectPtr<UNiagaraEmitter> PublishedWeak(Emitter);
	UPackage* PublishedPackage = Emitter->GetPackage();
	Emitter = nullptr;
	ReturnedObject = nullptr;
	FText UnloadError;
	if (!TestTrue(TEXT("public created emitter independently unloads"), UPackageTools::UnloadPackages({PublishedPackage}, UnloadError, true))
		|| !TestFalse(TEXT("old created emitter is gone before independent reload"), PublishedWeak.IsValid())) return false;
	UeAgentNiagaraAuthoring::FAssetApplyResult ExistingDiskResult;
	TestFalse(TEXT("existing unloaded disk emitter cannot be created twice"), UeAgentNiagaraAuthoring::ApplyAssetInlineDocument(
		Document, TEXT("emitter"), PackageName, ExistingDiskResult, FString(), true));
	TestEqual(TEXT("native disk existence lookup resolves existing emitter"), ExistingDiskResult.Diagnostic.Code, FString(TEXT("asset_already_exists")));
	auto* Reloaded = FindObject<UNiagaraEmitter>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("created emitter independently reloads"), Reloaded)) return false;
	TestFalse(TEXT("created reloaded package is clean"), Reloaded->GetPackage()->IsDirty());
	if (!SameEmitterContent(*this, CreatedContent, *Reloaded, TEXT("created complete owned content survives independent reload"), true)) return false;
	for (const auto& Expected : CreatedRoot)
	{
		const auto* PropertyField = FindFProperty<FProperty>(Reloaded->GetClass(), Expected.Key);
		FString ActualValue;
		if (PropertyField) PropertyField->ExportTextItem_Direct(ActualValue, PropertyField->ContainerPtrToValuePtr<void>(Reloaded), nullptr, Reloaded, PPF_Delimited);
		TestTrue(*FString::Printf(TEXT("created root field survives reload: %s"), *Expected.Key.ToString()), PropertyField && ActualValue == Expected.Value);
	}
	const FString ReadbackPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T85/NiagaraCreateTextReadback"));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*ReadbackPath, false, true); };
	if (!TryBuildNiagaraExportParams(ObjectPath, ReadbackPath, ParamsJson, Error)
		|| !TrySubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), ParamsJson, Error)) { AddError(Error); return false; }
	int32 TextLeaves = 0;
	for (const auto& File : CaptureVisibleOwnerFiles(ReadbackPath, FString()))
	{
		TSharedPtr<FJsonObject> Visible;
		if (!FUeAgentStructuredHjsonParser::ParseObject(File.Value, File.Key, Visible, Error)) return false;
		const FString BeforeText = UeAgentAuthoringState::MakeStableCanonicalValueKey(MakeShared<FJsonValueObject>(Visible));
		TextLeaves += SetVisibleEmitterRootText(Visible, DesiredSource);
		TestEqual(TEXT("created ordinary description is exactly the source string"), UeAgentAuthoringState::MakeStableCanonicalValueKey(MakeShared<FJsonValueObject>(Visible)), BeforeText);
	}
	TestEqual(TEXT("created ordinary description is editable as one public leaf"), TextLeaves, 1);
	if (HasAnyErrors()) return false;
	TArray<FString> FailureObjectPaths;
	for (const int32 RejectionStage : {0, 1, 2})
	{
		const bool bRejectCheckpoint = RejectionStage == 0;
		const bool bRejectBeforeStage = RejectionStage == 1;
		const FString FailedPackage = FPackageName::GetLongPackagePath(PackageName) + (bRejectCheckpoint ? TEXT("/NE_CheckpointRejected") : bRejectBeforeStage ? TEXT("/NE_EarlyRejected") : TEXT("/NE_LateRejected"));
		const FString FailedObject = FailedPackage + TEXT(".") + FPackageName::GetLongPackageAssetName(FailedPackage);
		const FString FailedFilename = FPackageName::LongPackageNameToFilename(FailedPackage, FPackageName::GetAssetPackageExtension());
		FailureObjectPaths.Add(FailedObject);
		if (!TestNull(TEXT("rejected creation starts with no package"), FindPackage(nullptr, *FailedPackage))
			|| !TestNull(TEXT("rejected creation starts with no object"), FindObject<UObject>(nullptr, *FailedObject))
			|| !TestFalse(TEXT("rejected creation starts with no registered asset"), AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(FailedObject)).IsValid())
			|| !TestFalse(TEXT("rejected creation starts with no disk"), IFileManager::Get().FileExists(*FailedFilename))) return false;
		auto FailedDocument = MakeShared<FJsonObject>(*Document);
		auto FailedTarget = MakeShared<FJsonObject>(*Target);
		FailedTarget->SetStringField(TEXT("asset_path"), FailedPackage);
		FailedDocument->SetObjectField(TEXT("target"), FailedTarget);
		auto Extension = MakeShared<FJsonObject>();
		Extension->SetBoolField(TEXT("compile"), true);
		Extension->SetBoolField(TEXT("save"), true);
		FailedDocument->SetObjectField(TEXT("provider_extension"), Extension);
		const auto Observer = MakeShared<FCreateTextStageObserver>();
		bool bObservedBeforePublish = false;
		bool bObservedFinalReload = false;
		bool bObservedAbsentCheckpoint = false;
		Observer->BeforeCheckpoint = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
		{
			bObservedAbsentCheckpoint = !FindPackage(nullptr, *FailedPackage) && !FindObject<UObject>(nullptr, *FailedObject)
				&& !AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(FailedObject)).IsValid()
				&& !IFileManager::Get().FileExists(*FailedFilename);
			if (bRejectCheckpoint) { Failure.Diagnostic.Code = TEXT("t208_create_checkpoint_rejected"); return false; }
			return true;
		};
		TWeakObjectPtr<UNiagaraEmitter> StagedWeak;
		TMap<FString, FString> StagedContent;
		Observer->BeforeStage = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
		{
			auto* Staged = FindObject<UNiagaraEmitter>(nullptr, *FailedObject);
			bObservedBeforePublish = Staged && Staged->TemplateAssetDescription.ToString() == DesiredSource
				&& !FTextInspector::GetTextId(Staged->TemplateAssetDescription).GetKey().IsEmpty()
				&& !IFileManager::Get().FileExists(*FailedFilename);
			if (Staged) { StagedWeak = Staged; StagedContent = CaptureEmitterContent(*Staged, true, true); }
			if (bRejectBeforeStage && Staged)
			{
				UeAgentNiagaraAuthoring::FAssetApplyResult ExistingMemoryResult;
				TestFalse(TEXT("unsaved in-memory emitter cannot be created twice"), UeAgentNiagaraAuthoring::ApplyAssetInlineDocument(
					FailedDocument, TEXT("emitter"), FailedPackage, ExistingMemoryResult, FString(), true));
				TestEqual(TEXT("memory existence lookup retains unsaved emitter"), ExistingMemoryResult.Diagnostic.Code, FString(TEXT("asset_already_exists")));
				TestTrue(TEXT("existence lookup retains the same native object"), FindObject<UObject>(nullptr, *FailedObject) == Staged);
			}
			if (bRejectBeforeStage) { Failure.Diagnostic.Code = TEXT("t208_create_before_stage_rejected"); return false; }
			return true;
		};
		auto FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
		{
			auto* Fresh = FindObject<UNiagaraEmitter>(nullptr, *FailedObject);
			bObservedFinalReload = Fresh && !StagedWeak.IsValid() && !Fresh->GetPackage()->IsDirty()
				&& Fresh->TemplateAssetDescription.ToString() == DesiredSource && IFileManager::Get().FileExists(*FailedFilename);
			if (Fresh) bObservedFinalReload &= SameEmitterContent(*this, StagedContent, *Fresh, TEXT("created final native readback"), true, true);
			Failure.Diagnostic.Code = TEXT("t208_create_final_readback_rejected");
			return false;
		};
		UeAgentNiagaraAuthoring::FAssetApplyResult FailedResult;
		const bool bApplied = UeAgentNiagaraAuthoring::ApplyAssetInlineDocument(FailedDocument, TEXT("emitter"), FailedPackage,
			FailedResult, TEXT("t208-create-rejection-") + FGuid::NewGuid().ToString(EGuidFormats::Digits), true, FinalReadback, Observer);
		TestFalse(TEXT("injected create refusal returns failure"), bApplied);
		AddInfo(FString::Printf(TEXT("create refusal stage=%s phase=%s cause=%s observed_prepublication=%d observed_fresh_reload=%d rollback=%d package_present=%d"),
			bRejectCheckpoint ? TEXT("checkpoint") : bRejectBeforeStage ? TEXT("before_save") : TEXT("final_readback"), *FailedResult.Phase, *FailedResult.Diagnostic.Code,
			bObservedBeforePublish, bObservedFinalReload, FailedResult.bRollbackSucceeded, FindPackage(nullptr, *FailedPackage) != nullptr));
		TestTrue(TEXT("checkpoint precedes target package and text namespace creation"), bObservedAbsentCheckpoint);
		TestEqual(TEXT("forward source application follows checkpoint success"), bObservedBeforePublish, !bRejectCheckpoint);
		TestEqual(TEXT("final readback is reached only for late refusal"), bObservedFinalReload, RejectionStage == 2);
		TestTrue(TEXT("create refusal reports confirmed rollback"), FailedResult.bRollbackAttempted && FailedResult.bRollbackSucceeded);
		TestNull(TEXT("refused create has no object before fixture cleanup"), FindObject<UObject>(nullptr, *FailedObject));
		TestNull(TEXT("refused create has no package before fixture cleanup"), FindPackage(nullptr, *FailedPackage));
		TestFalse(TEXT("refused create has no registry entry before fixture cleanup"), AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(FailedObject)).IsValid());
		TestFalse(TEXT("refused create has no disk before fixture cleanup"), IFileManager::Get().FileExists(*FailedFilename));
		if (!FailedResult.bRollbackSucceeded) AddError(UeAgentNiagaraAuthoring::GetApplyFailureMessage(FailedResult));
	}
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FNextTickObservation>([this, FailureObjectPaths]()
	{
		auto& RegistryAfterTick = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		for (const auto& FailedObject : FailureObjectPaths)
		{
			const FString FailedPackage = FPackageName::ObjectPathToPackageName(FailedObject);
			const FString FailedFilename = FPackageName::LongPackageNameToFilename(FailedPackage, FPackageName::GetAssetPackageExtension());
			TestNull(TEXT("refused create remains absent after editor tick"), FindObject<UObject>(nullptr, *FailedObject));
			TestFalse(TEXT("refused create registry remains absent after editor tick"), RegistryAfterTick.GetAssetByObjectPath(FSoftObjectPath(FailedObject)).IsValid());
			TestFalse(TEXT("refused create disk remains absent after editor tick"), IFileManager::Get().FileExists(*FailedFilename));
			DeleteAssetIfPresent(FailedObject);
			T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, FailedPackage);
		}
	}));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208NiagaraEmitterCloneTextVerticalTest,
	"UAI.Root.T208.Niagara.EmitterCloneTextVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

static bool RunEmitterCloneTextVertical(FAutomationTestBase& Test, bool bFactoryControl)
{
	using namespace UE::UAI;
	const FString Base = TEXT("/Game/UAI/Transient/T85NiagaraEmitterCloneText/");
	const FString SourcePackage = Base + TEXT("NE_Source");
	const FString SourceObject = SourcePackage + TEXT(".NE_Source");
	const FString SourceFilename = FPackageName::LongPackageNameToFilename(SourcePackage, FPackageName::GetAssetPackageExtension());
	const FString TargetPackage = Base + TEXT("NE_Clone");
	const FString TargetObject = TargetPackage + TEXT(".NE_Clone");
	const FString FailedPackage = Base + TEXT("NE_Rejected");
	const FString FailedObject = FailedPackage + TEXT(".NE_Rejected");
	const FString NativePackage = Base + TEXT("NE_NativeControl");
	const FString NativeObject = NativePackage + TEXT(".NE_Clone");
	auto& Assets = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	auto Absent = [&](const FString& Package, const FString& Object, const TCHAR* Phase)
	{
		const bool bAbsent = !FindPackage(nullptr, *Package) && !FindObject<UObject>(nullptr, *Object)
			&& !Assets.GetAssetByObjectPath(FSoftObjectPath(Object)).IsValid()
			&& !FPackageName::DoesPackageExist(Package);
		return Test.TestTrue(Phase, bAbsent);
	};
	if (!Absent(SourcePackage, SourceObject, TEXT("source fixture initially absent"))
		|| !Absent(TargetPackage, TargetObject, TEXT("clone target initially absent"))
		|| !Absent(FailedPackage, FailedObject, TEXT("rejected target initially absent"))
		|| (bFactoryControl && !Absent(NativePackage, NativeObject, TEXT("native control initially absent")))) return false;
	// Cleanup is deferred until after the following Editor tick's restoration checks.
	ON_SCOPE_EXIT
	{
		ADD_LATENT_AUTOMATION_COMMAND(FNextTickObservation([&Test, SourceObject, SourcePackage, TargetObject, FailedObject, NativeObject, bFactoryControl]()
		{
			if (bFactoryControl) DeleteAssetIfPresent(NativeObject);
			DeleteAssetIfPresent(TargetObject);
			DeleteAssetIfPresent(FailedObject);
			DeleteAssetIfPresent(SourceObject);
			T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test, SourcePackage);
		}));
	};
	UPackage* Package = CreatePackage(*SourcePackage);
	auto* SourceEmitter = NewObject<UNiagaraEmitter>(Package, TEXT("NE_Source"), RF_Public | RF_Standalone | RF_Transactional);
	UNiagaraEmitterFactoryNew::InitializeEmitter(SourceEmitter, true);
	SourceEmitter->TemplateAssetDescription = FText::FromString(TEXT("Persisted source description"));
	FAssetRegistryModule::AssetCreated(SourceEmitter);
	FString Error;
	if (!UeAgentNiagaraAuthoring::CompileAsset(SourceEmitter, Error) || !UeAgentNiagaraAuthoring::SaveAsset(SourceEmitter, Error)) { Test.AddError(Error); return false; }
	if (!bFactoryControl)
	{
	// Establish a normally loaded source before fixing the clone oracle. Script
	// PostLoad synchronizes parameter-definition subscriptions and source graphs.
	const TWeakObjectPtr<UNiagaraEmitter> FactorySource(SourceEmitter);
	SourceEmitter = nullptr;
	FText SourceUnloadError;
	if (!Test.TestTrue(TEXT("factory source baseline unloads"), UPackageTools::UnloadPackages({Package}, SourceUnloadError, true))
		|| !Test.TestFalse(TEXT("factory source is gone before baseline reload"), FactorySource.IsValid())) return false;
	SourceEmitter = LoadObject<UNiagaraEmitter>(nullptr, *SourceObject);
	if (!Test.TestNotNull(TEXT("source baseline is native reloaded"), SourceEmitter)
		|| !UeAgentNiagaraAuthoring::SaveAsset(SourceEmitter, Error)) { Test.AddError(Error); return false; }
	}
	TArray<uint8> SourceDisk;
	if (!Test.TestTrue(TEXT("capture distinct source disk"), FFileHelper::LoadFileToArray(SourceDisk, *SourceFilename))) return false;
	SourceEmitter->TemplateAssetDescription = FText::FromString(TEXT("Dirty source description"));
	auto* SourceData = SourceEmitter->GetLatestEmitterData();
	SourceData->CalculateBoundsMode = ENiagaraEmitterCalculateBoundMode::Fixed;
	SourceData->FixedBounds = FBox(FVector(-17, -20, -30), FVector(10, 20, 30));
	SourceEmitter->MarkPackageDirty();
	TArray<FResolvedStage> Stages;
	const auto* Stage = FindEmitterParticleUpdateStage(*SourceEmitter, Stages, Error);
	TArray<UNiagaraNodeFunctionCall*> Modules;
	if (Stage) GetModules(*Stage->OutputNode, Modules);
	if (!Test.TestTrue(TEXT("clone source has real native modules"), !Modules.IsEmpty())) return false;
	const auto SourceBefore = CaptureEmitterContent(*SourceEmitter, false, true);
	const auto SourceAuthoring = CaptureEmitterContent(*SourceEmitter, true, true);
	const TWeakObjectPtr<UNiagaraEmitter> SourceWeak(SourceEmitter);
	auto SourcePreserved = [&Test, SourceWeak, SourceBefore, SourceDisk, SourceFilename, SourceObject]()
	{
		auto* Current = FindObject<UNiagaraEmitter>(nullptr, *SourceObject);
		bool bOk = Test.TestTrue(TEXT("clone preserves original source object and dirty flag"), Current && SourceWeak.Get() == Current && Current->GetPackage()->IsDirty());
		if (Current) bOk &= SameEmitterContent(Test, SourceBefore, *Current, TEXT("clone source full native content"), false, true)
			&& HasExactBounds(Current, -17);
		TArray<uint8> Disk;
		bOk &= Test.TestTrue(TEXT("clone preserves original source disk bytes"), FFileHelper::LoadFileToArray(Disk, *SourceFilename) && Disk == SourceDisk);
		bOk &= Test.TestTrue(TEXT("clone preserves source registration"), FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetByObjectPath(FSoftObjectPath(SourceObject)).IsValid());
		return bOk;
	};
	const FString Desired = TEXT("Explicit clone description / 克隆文本");
	ON_SCOPE_EXIT
	{
		if (bFactoryControl && SourceWeak.IsValid())
		{
			SourcePreserved();
		}
	};
	auto ExpectedCopy = SourceAuthoring;
	FString ExpectedRootObject = SourceObject;
	if (bFactoryControl)
	{
		if (!Absent(NativePackage, NativeObject, TEXT("native control initially absent"))) return false;
		// A single normal native duplicate is independent of UAI's scratch route.
		auto* Native = Cast<UNiagaraEmitter>(StaticDuplicateObject(SourceEmitter, CreatePackage(*NativePackage), TEXT("NE_Clone")));
		if (!Test.TestNotNull(TEXT("single native duplicate"), Native)) return false;
		FAssetRegistryModule::AssetCreated(Native);
		Native->TemplateAssetDescription = FText::FromString(Desired);
		if (!UeAgentNiagaraAuthoring::CompileAsset(Native, Error)) { Test.AddError(Error); return false; }
		if (!UeAgentNiagaraAuthoring::SaveAsset(Native, Error)) { Test.AddError(Error); return false; }
		UPackage* NativeLoadedPackage = Native->GetPackage();
		const TWeakObjectPtr<UNiagaraEmitter> NativeWeak(Native); Native = nullptr;
		FText UnloadError;
		if (!Test.TestTrue(TEXT("native control unloads"), UPackageTools::UnloadPackages({NativeLoadedPackage}, UnloadError, true))
			|| !Test.TestFalse(TEXT("native control old object gone"), NativeWeak.IsValid())) return false;
		Native = LoadObject<UNiagaraEmitter>(nullptr, *NativeObject);
		if (!Test.TestNotNull(TEXT("native control reloads"), Native)) return false;
		ExpectedCopy = CaptureEmitterContent(*Native, true, true);
		ExpectedRootObject = NativeObject;
		if (!SourcePreserved()) return false;
	}
	auto MakeDocument = [&](const FString& Destination)
	{
		auto Target = MakeShared<FJsonObject>(); Target->SetStringField(TEXT("asset_path"), Destination);
		Target->SetStringField(TEXT("source_asset_path"), SourcePackage);
		auto Value = MakeShared<FJsonObject>(); Value->SetStringField(TEXT("literal"), Desired);
		auto Property = MakeShared<FJsonObject>(); Property->SetStringField(TEXT("property_path"), TEXT("TemplateAssetDescription")); Property->SetObjectField(TEXT("value"), Value);
		auto Document = MakeShared<FJsonObject>(); Document->SetStringField(TEXT("operation"), TEXT("clone"));
		Document->SetObjectField(TEXT("target"), Target); Document->SetArrayField(TEXT("properties"), {MakeShared<FJsonValueObject>(Property)});
		return Document;
	};
	auto CheckClone = [&](UNiagaraEmitter& Actual, const FString& Destination)
	{
		auto Expected = ExpectedCopy;
		FString ExpectedObject = ExpectedRootObject;
		if (!bFactoryControl)
		{
			// Build the published alias expectation through the native owner on an
			// independent copy, never by mutating the dirty source or rewriting names.
			TStrongObjectPtr<UNiagaraEmitter> ExpectedEmitter(Cast<UNiagaraEmitter>(StaticDuplicateObject(SourceEmitter, GetTransientPackage(), NAME_None, RF_Transactional)));
			if (!Test.TestNotNull(TEXT("native loaded-source clone expectation"), ExpectedEmitter.Get())) return false;
			ExpectedEmitter->SetUniqueEmitterName(Actual.GetFName().GetPlainNameString());
			Expected = CaptureEmitterContent(*ExpectedEmitter, true, true);
			ExpectedObject = ExpectedEmitter->GetPathName();
		}
		auto Observed = CaptureEmitterContent(Actual, true, true);
		// Native duplication relocates owned references to the destination root;
		// all other authored values, node/pin identities and external references remain exact.
		for (auto& Pair : Expected) Pair.Value.ReplaceInline(*ExpectedObject, *Actual.GetPathName(), ESearchCase::CaseSensitive);
		Expected.Remove(TEXT("emitter/TemplateAssetDescription")); Observed.Remove(TEXT("emitter/TemplateAssetDescription"));
		Expected.Remove(TEXT("emitter/ChangeId")); Observed.Remove(TEXT("emitter/ChangeId"));
		bool bOk = Test.TestEqual(TEXT("clone native content cardinality"), Observed.Num(), Expected.Num());
		TSet<FGuid> NewPinIdentities;
		for (const auto& Pair : Expected)
		{
			const FString* Value = Observed.Find(Pair.Key);
			if (Value && *Value == Pair.Value) continue;
			bool bNativeNewIdentity = false;
			const FString* Original = SourceBefore.Find(Pair.Key);
			if (bFactoryControl && Value && Original && Pair.Key.Contains(TEXT("/@pin/")))
			{
				auto ReadIdentity = [](const FString& Pin, FGuid& Guid, int32& Offset)
				{
					Offset = Pin.Find(TEXT("PersistentGuid="));
					if (Offset == INDEX_NONE) return false;
					Offset += 15;
					return FGuid::ParseExact(Pin.Mid(Offset, 32), EGuidFormats::Digits, Guid);
				};
				FGuid OriginalId, ExpectedId, ActualId; int32 OriginalOffset, ExpectedOffset, ActualOffset;
				if (ReadIdentity(*Original, OriginalId, OriginalOffset) && !OriginalId.IsValid()
					&& ReadIdentity(Pair.Value, ExpectedId, ExpectedOffset) && ExpectedId.IsValid()
					&& ReadIdentity(*Value, ActualId, ActualOffset) && ActualId.IsValid() && !NewPinIdentities.Contains(ActualId))
				{
					FString WithExpectedNewIdentity = *Value;
					WithExpectedNewIdentity.RemoveAt(ActualOffset, 32);
					WithExpectedNewIdentity.InsertAt(ActualOffset, Pair.Value.Mid(ExpectedOffset, 32));
					bNativeNewIdentity = WithExpectedNewIdentity == Pair.Value;
					NewPinIdentities.Add(ActualId);
				}
			}
			if (!bNativeNewIdentity)
			{
				Test.AddError(TEXT("clone native copy differs: ") + Pair.Key); bOk = false;
			}
		}
		bOk &= Test.TestTrue(TEXT("clone has exact dirty-source bounds and destination ownership"), HasExactBounds(&Actual, -17) && Actual.GetPackage()->GetName() == Destination);
		bOk &= Test.TestEqual(TEXT("clone explicit source text"), Actual.TemplateAssetDescription.ToString(), Desired);
		bOk &= Test.TestTrue(TEXT("clone native text identity exists"), !FTextInspector::GetTextId(Actual.TemplateAssetDescription).GetKey().IsEmpty());
		bOk &= Test.TestTrue(TEXT("clone success revision is valid and new"), Actual.GetChangeId().IsValid() && Actual.GetChangeId() != SourceEmitter->GetChangeId());
		return bOk;
	};
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!TryCreateNiagaraPublicSurface(Registry, Dispatch, Error)) { Test.AddError(Error); return false; }
	auto Root = MakeShared<FJsonObject>(); Root->SetStringField(TEXT("type"), TEXT("/Script/Niagara.NiagaraEmitter")); Root->SetObjectField(TEXT("document"), MakeDocument(TargetPackage));
	auto Hjson = MakeShared<FJsonObject>(); Hjson->SetStringField(TEXT("schema"), TEXT("uai.niagara.edit/1")); Hjson->SetObjectField(TEXT("root"), Root);
	FString Text; if (!CommandContract::Json::TryCanonicalize(Hjson, Text, Error)) return false;
	auto Input = MakeShared<FJsonObject>(); Input->SetStringField(TEXT("kind"), TEXT("inline")); Input->SetStringField(TEXT("document"), Text);
	auto Params = MakeShared<FJsonObject>(); Params->SetStringField(TEXT("target"), TargetObject); Params->SetObjectField(TEXT("source"), Input);
	if (!CommandContract::Json::TryCanonicalize(Params, Text, Error)) return false;
	const auto Result = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Text);
	if (!Result.IsOk()) { Test.AddError(Result.GetErrorJson()); SourcePreserved(); return false; }
	auto* Created = FindObject<UNiagaraEmitter>(nullptr, *TargetObject);
	if (!Test.TestNotNull(TEXT("public clone exists"), Created) || !CheckClone(*Created, TargetPackage) || !SourcePreserved()) return false;
	const auto CreatedContent = CaptureEmitterContent(*Created, true, true);
	TWeakObjectPtr<UNiagaraEmitter> OldTarget(Created);
	UPackage* CreatedPackage = Created->GetPackage(); Created = nullptr;
	FText UnloadError;
	if (!Test.TestTrue(TEXT("clone independently unloads"), UPackageTools::UnloadPackages({CreatedPackage}, UnloadError, true))
		|| !Test.TestFalse(TEXT("old clone invalid before reload"), OldTarget.IsValid())) return false;
	auto* Reloaded = LoadObject<UNiagaraEmitter>(nullptr, *TargetObject);
	if (!Test.TestNotNull(TEXT("clone independently reloads"), Reloaded) || !CheckClone(*Reloaded, TargetPackage)
		|| !SameEmitterContent(Test, CreatedContent, *Reloaded, TEXT("clone full persisted native identity"), true, true) || !SourcePreserved()) return false;
	Test.TestFalse(TEXT("reloaded clone is clean"), Reloaded->GetPackage()->IsDirty());
	const FString Readback = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T85/NiagaraCloneTextReadback"));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Readback, false, true); };
	if (!TryBuildNiagaraExportParams(TargetObject, Readback, Text, Error) || !TrySubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), Text, Error)) { Test.AddError(Error); return false; }
	int32 Leaves = 0;
	for (const auto& File : CaptureVisibleOwnerFiles(Readback, FString()))
	{
		TSharedPtr<FJsonObject> Visible;
		if (!FUeAgentStructuredHjsonParser::ParseObject(File.Value, File.Key, Visible, Error)) return false;
		const FString Before = UeAgentAuthoringState::MakeStableCanonicalValueKey(MakeShared<FJsonValueObject>(Visible));
		Leaves += SetVisibleEmitterRootText(Visible, Desired);
		Test.TestEqual(TEXT("clone ordinary text is exact editable source"), UeAgentAuthoringState::MakeStableCanonicalValueKey(MakeShared<FJsonValueObject>(Visible)), Before);
	}
	Test.TestEqual(TEXT("clone has one ordinary text leaf"), Leaves, 1);
	if (Test.HasAnyErrors()) return false;
	if (bFactoryControl)
	{
		return SourcePreserved() && !Test.HasAnyErrors();
	}
	const auto Observer = MakeShared<FCreateTextStageObserver>();
	TWeakObjectPtr<UNiagaraEmitter> StagedWeak;
	TMap<FString, FString> StagedContent;
	Observer->BeforeStage = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure&)
	{
		auto* Staged = FindObject<UNiagaraEmitter>(nullptr, *FailedObject);
		if (!Staged) return false;
		StagedWeak = Staged; StagedContent = CaptureEmitterContent(*Staged, true, true);
		return CheckClone(*Staged, FailedPackage) && SourcePreserved();
	};
	bool bFreshPublished = false;
	auto Final = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
	{
		auto* Fresh = FindObject<UNiagaraEmitter>(nullptr, *FailedObject);
		bFreshPublished = Fresh && !StagedWeak.IsValid() && !Fresh->GetPackage()->IsDirty() && FPackageName::DoesPackageExist(FailedPackage);
		if (Fresh) bFreshPublished &= CheckClone(*Fresh, FailedPackage) && SameEmitterContent(Test, StagedContent, *Fresh, TEXT("clone late full native identity"), true, true);
		SourcePreserved();
		Failure.Diagnostic.Code = TEXT("t208_clone_final_readback_rejected"); return false;
	};
	UeAgentNiagaraAuthoring::FAssetApplyResult FailedResult;
	// Match the public PrepareDocument output consumed by &Test same inline seam.
	const auto FailedDocument = MakeDocument(FailedPackage);
	FailedDocument->GetObjectField(TEXT("target"))->SetStringField(TEXT("asset_kind"), TEXT("emitter"));
	auto FailedExtension = MakeShared<FJsonObject>();
	FailedExtension->SetBoolField(TEXT("compile"), true); FailedExtension->SetBoolField(TEXT("save"), true);
	FailedDocument->SetObjectField(TEXT("provider_extension"), FailedExtension);
	Test.TestFalse(TEXT("late clone refusal returns failure"), UeAgentNiagaraAuthoring::ApplyAssetInlineDocument(FailedDocument, TEXT("emitter"), FailedPackage,
		FailedResult, TEXT("t208-clone-") + FGuid::NewGuid().ToString(EGuidFormats::Digits), true, Final, Observer));
	Test.TestTrue(TEXT("clone refusal follows fresh clean persisted target"), bFreshPublished);
	Test.TestTrue(TEXT("clone refusal restores absence successfully"), FailedResult.bRollbackAttempted && FailedResult.bRollbackSucceeded);
	Absent(FailedPackage, FailedObject, TEXT("rejected clone absent before cleanup")); SourcePreserved();
	Test.AddInfo(FString::Printf(TEXT("clone late refusal phase=%s cause=%s fresh_published=%d rollback=%d"), *FailedResult.Phase, *FailedResult.Diagnostic.Code, bFreshPublished, FailedResult.bRollbackSucceeded));
	ADD_LATENT_AUTOMATION_COMMAND(FNextTickObservation([&Test, FailedPackage, FailedObject, SourcePreserved]()
	{
		Test.TestTrue(TEXT("rejected clone remains absent after tick"), !FindPackage(nullptr, *FailedPackage) && !FindObject<UObject>(nullptr, *FailedObject)
			&& !FPackageName::DoesPackageExist(FailedPackage) && !FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetByObjectPath(FSoftObjectPath(FailedObject)).IsValid());
		SourcePreserved();
	}));
	return !Test.HasAnyErrors();
}

bool FUeAgentT208NiagaraEmitterCloneTextVerticalTest::RunTest(const FString&)
{ return RunEmitterCloneTextVertical(*this, false); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208NiagaraEmitterCloneFactoryControlTest,
	"UAI.Root.T208.Niagara.EmitterCloneFactoryControl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208NiagaraEmitterCloneFactoryControlTest::RunTest(const FString&)
{ return RunEmitterCloneTextVertical(*this, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT208NiagaraEmbeddedEmitterScopedPropertyVerticalTest,
	"UAI.Root.T208.Niagara.EmbeddedEmitterScopedPropertyVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
int32 SetVisibleSystemRootBool(const TSharedPtr<FJsonObject>& Document, bool Desired)
{
	int32 Count = 0;
	for (const auto& Field : Document->Values)
	{
		if ((MatchesPropertyName(Field.Key, TEXT("bRequireCurrentFrameData"))
			|| MatchesPropertyName(Field.Key, TEXT("RequireCurrentFrameData"))) && Field.Value.IsValid())
		{
			if (Field.Value->Type == EJson::Boolean)
			{
				Document->SetBoolField(Field.Key, Desired);
				++Count;
			}
			else if (Field.Value->Type == EJson::Object)
			{
				const auto Value = Field.Value->AsObject();
				FString Type;
				bool Previous;
				if (Value->TryGetStringField(TEXT("type"), Type) && Type == TEXT("bool")
					&& Value->TryGetBoolField(TEXT("value"), Previous))
				{
					Value->SetBoolField(TEXT("value"), Desired);
					++Count;
				}
			}
		}
		else if (Field.Value.IsValid() && Field.Value->Type == EJson::Object)
			Count += SetVisibleSystemRootBool(Field.Value->AsObject(), Desired);
	}
	return Count;
}

TMap<FName, FString> CaptureOtherSystemRootAuthoring(UNiagaraSystem& System)
{
	TMap<FName, FString> Values;
	for (TFieldIterator<FProperty> It(System.GetClass()); It; ++It)
	{
		if (!It->HasAnyPropertyFlags(CPF_Edit) || It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)
			|| It->GetFName() == TEXT("bRequireCurrentFrameData")) continue;
		FString Value;
		It->ExportTextItem_Direct(Value, It->ContainerPtrToValuePtr<void>(&System), nullptr, &System, PPF_Delimited);
		Values.Add(It->GetFName(), MoveTemp(Value));
	}
	return Values;
}

bool RunSystemPropertyWorkspaceTest(FAutomationTestBase& Test, const bool bRootProperty)
{
	const FString PackageName = bRootProperty
		? TEXT("/Game/UAI/Transient/T85NiagaraSystemRootProperty/NS_RootProperty")
		: TEXT("/Game/UAI/Transient/T85NiagaraEmbeddedEmitterScopedProperty/NS_ScopedProperty");
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	DeleteAssetIfPresent(ObjectPath);
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test, PackageName);
	};

	UPackage* const Package = CreatePackage(*PackageName);
	UNiagaraSystem* const System = Package ? NewObject<UNiagaraSystem>(
		Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional) : nullptr;
	if (!Test.TestNotNull(TEXT("the embedded scoped System fixture is created"), System)) return false;
	UNiagaraSystemFactoryNew::InitializeSystem(System, true);
	if (bRootProperty)
	{
		const FBoolProperty* Property = FindFProperty<FBoolProperty>(System->GetClass(), TEXT("bRequireCurrentFrameData"));
		if (!Test.TestTrue(TEXT("the root target is a native editable persistent bool field"),
			Property && Property->HasAnyPropertyFlags(CPF_Edit)
			&& !Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))) return false;
		System->bRequireCurrentFrameData = false;
	}
	UNiagaraEmitter* const SourceA = NewObject<UNiagaraEmitter>(GetTransientPackage());
	UNiagaraEmitter* const SourceB = NewObject<UNiagaraEmitter>(GetTransientPackage());
	if (!Test.TestNotNull(TEXT("the target source Emitter is created"), SourceA)
		|| !Test.TestNotNull(TEXT("the control source Emitter is created"), SourceB)) return false;
	UNiagaraEmitterFactoryNew::InitializeEmitter(SourceA, true);
	UNiagaraEmitterFactoryNew::InitializeEmitter(SourceB, true);
	const FString TargetName = TEXT("ScopedTargetA");
	const FString ControlName = TEXT("ScopedControlB");
	const FNiagaraEmitterHandle AddedA = System->AddEmitterHandle(*SourceA, FName(*TargetName), FGuid());
	const FNiagaraEmitterHandle AddedB = System->AddEmitterHandle(*SourceB, FName(*ControlName), FGuid());
	const FGuid TargetHandleId = AddedA.GetId();
	const FGuid ControlHandleId = AddedB.GetId();
	auto FindHandle = [](UNiagaraSystem& Owner, const FGuid& Id) -> FNiagaraEmitterHandle*
	{
		return Owner.GetEmitterHandles().FindByPredicate(
			[&](const FNiagaraEmitterHandle& Handle) { return Handle.GetId() == Id; });
	};
	FNiagaraEmitterHandle* const TargetHandle = FindHandle(*System, TargetHandleId);
	FNiagaraEmitterHandle* const ControlHandle = FindHandle(*System, ControlHandleId);
	UNiagaraEmitter* const TargetEmitter = TargetHandle ? TargetHandle->GetInstance().Emitter : nullptr;
	UNiagaraEmitter* const ControlEmitter = ControlHandle ? ControlHandle->GetInstance().Emitter : nullptr;
	FVersionedNiagaraEmitterData* const TargetData = TargetHandle
		? TargetHandle->GetInstance().GetEmitterData() : nullptr;
	FVersionedNiagaraEmitterData* const ControlData = ControlHandle
		? ControlHandle->GetInstance().GetEmitterData() : nullptr;
	if (!Test.TestNotNull(TEXT("the target embedded Emitter resolves"), TargetEmitter)
		|| !Test.TestNotNull(TEXT("the control embedded Emitter resolves"), ControlEmitter)
		|| !Test.TestNotNull(TEXT("the target embedded version data resolves"), TargetData)
		|| !Test.TestNotNull(TEXT("the control embedded version data resolves"), ControlData)) return false;
	if (!Test.TestTrue(TEXT("both initialized handles use valid unversioned Emitter ownership"),
		!TargetHandle->GetInstance().Version.IsValid()
		&& !ControlHandle->GetInstance().Version.IsValid()
		&& !TargetEmitter->IsVersioningEnabled()
		&& !ControlEmitter->IsVersioningEnabled()
		&& TargetHandle->GetInstance().GetEmitterData() == TargetData
		&& ControlHandle->GetInstance().GetEmitterData() == ControlData)) return false;
	TargetData->CalculateBoundsMode = ENiagaraEmitterCalculateBoundMode::Fixed;
	TargetData->FixedBounds = FBox(FVector(-10.0, -20.0, -30.0), FVector(10.0, 20.0, 30.0));
	ControlData->CalculateBoundsMode = ENiagaraEmitterCalculateBoundMode::Fixed;
	ControlData->FixedBounds = FBox(FVector(-70.0, -80.0, -90.0), FVector(70.0, 80.0, 90.0));
	TArray<FResolvedStage> TargetStages;
	TArray<FResolvedStage> ControlStages;
	FString StageError;
	const FResolvedStage* const TargetStage = FindEmitterParticleUpdateStage(
		*TargetEmitter, TargetStages, StageError);
	const FResolvedStage* const ControlStage = FindEmitterParticleUpdateStage(
		*ControlEmitter, ControlStages, StageError);
	TArray<UNiagaraNodeFunctionCall*> TargetModules;
	TArray<UNiagaraNodeFunctionCall*> ControlModules;
	if (TargetStage) GetModules(*TargetStage->OutputNode, TargetModules);
	if (ControlStage) GetModules(*ControlStage->OutputNode, ControlModules);
	if (!Test.TestTrue(TEXT("both embedded Emitters contain native module content"),
		TargetStage && ControlStage && !TargetModules.IsEmpty() && !ControlModules.IsEmpty())) return false;
	TargetModules[0]->NodeComment = TEXT("target module content before scoped property edit");
	ControlModules[0]->NodeComment = TEXT("control module content must remain unchanged");
	TargetData->bLocalSpace = true;
	ControlData->bLocalSpace = false;
	FAssetRegistryModule::AssetCreated(System);
	FString BaselineCompileError;
	if (!Test.TestTrue(TEXT("the native System baseline compiles before preservation capture"),
		UeAgentNiagaraAuthoring::CompileAsset(System, BaselineCompileError)))
	{
		Test.AddError(BaselineCompileError);
		return false;
	}
	if (!Test.TestTrue(TEXT("the embedded scoped System fixture is saved"),
		SaveSystem(*Package, *System, Filename))) return false;
	const TWeakObjectPtr<UNiagaraSystem> InitialSystemWeak(System);
	FText InitialUnloadError;
	if (!Test.TestTrue(TEXT("the initial embedded fixture independently unloads"),
		UPackageTools::UnloadPackages({Package}, InitialUnloadError, true)))
	{
		Test.AddError(InitialUnloadError.ToString());
		return false;
	}
	if (!Test.TestFalse(TEXT("the initial embedded fixture object is gone"), InitialSystemWeak.IsValid())) return false;
	UNiagaraSystem* const Baseline = LoadObject<UNiagaraSystem>(nullptr, *ObjectPath);
	FNiagaraEmitterHandle* const BaselineA = Baseline ? FindHandle(*Baseline, TargetHandleId) : nullptr;
	FNiagaraEmitterHandle* const BaselineB = Baseline ? FindHandle(*Baseline, ControlHandleId) : nullptr;
	UNiagaraEmitter* const BaselineEmitterA = BaselineA ? BaselineA->GetInstance().Emitter : nullptr;
	UNiagaraEmitter* const BaselineEmitterB = BaselineB ? BaselineB->GetInstance().Emitter : nullptr;
	if (!Test.TestNotNull(TEXT("the persisted target owner is readable"), BaselineEmitterA)
		|| !Test.TestNotNull(TEXT("the persisted control owner is readable"), BaselineEmitterB)) return false;
	if (!Test.TestTrue(TEXT("the baseline has exactly two distinct System-owned single-version Emitters"),
		Baseline->GetEmitterHandles().Num() == 2 && BaselineEmitterA != BaselineEmitterB
		&& BaselineEmitterA->GetOuter() == Baseline && BaselineEmitterB->GetOuter() == Baseline
		&& !BaselineEmitterA->IsVersioningEnabled() && !BaselineEmitterB->IsVersioningEnabled()
		&& BaselineEmitterA->GetAllAvailableVersions().Num() == 1
		&& BaselineEmitterB->GetAllAvailableVersions().Num() == 1
		&& !BaselineA->GetInstance().Version.IsValid() && !BaselineB->GetInstance().Version.IsValid()
		&& BaselineA->GetInstance().GetEmitterData() == BaselineEmitterA->GetLatestEmitterData()
		&& BaselineB->GetInstance().GetEmitterData() == BaselineEmitterB->GetLatestEmitterData())) return false;
	const auto BaselineContentA = CaptureEmitterContent(*BaselineEmitterA, true, true);
	const auto BaselineContentB = CaptureEmitterContent(*BaselineEmitterB, true, true);
	const auto BaselineOtherRoot = CaptureOtherSystemRootAuthoring(*Baseline);
	if (bRootProperty && !Test.TestFalse(TEXT("the persisted root baseline is the preset false value"),
		Baseline->bRequireCurrentFrameData)) return false;
	const FGuid BaselineRawVersionA = BaselineA->GetInstance().Version;
	const FGuid BaselineRawVersionB = BaselineB->GetInstance().Version;
	const FGuid BaselineDataVersionA = BaselineA->GetInstance().GetEmitterData()->Version.VersionGuid;
	const FGuid BaselineDataVersionB = BaselineB->GetInstance().GetEmitterData()->Version.VersionGuid;
	const FGuid BaselineExposedVersionA = BaselineEmitterA->GetExposedVersion().VersionGuid;
	const FGuid BaselineExposedVersionB = BaselineEmitterB->GetExposedVersion().VersionGuid;
	const FString BaselinePathA = BaselineEmitterA->GetPathName(Baseline);
	const FString BaselinePathB = BaselineEmitterB->GetPathName(Baseline);
	const bool BaselineEnabledA = BaselineA->GetIsEnabled();
	const bool BaselineEnabledB = BaselineB->GetIsEnabled();
	const ENiagaraEmitterMode BaselineModeA = BaselineA->GetEmitterMode();
	const ENiagaraEmitterMode BaselineModeB = BaselineB->GetEmitterMode();

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!Test.TestTrue(TEXT("the public Niagara HJSON surface composes"),
		TryCreateNiagaraPublicSurface(Registry, Dispatch, Error))) return false;
	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / (TEXT("Automation/UAI/T208/NiagaraEmbeddedScopedExport_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	const FString ReadbackPath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / (TEXT("Automation/UAI/T208/NiagaraEmbeddedScopedReadback_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*WorkspacePath, false, true);
		IFileManager::Get().DeleteDirectory(*ReadbackPath, false, true);
	};
	FString ParamsJson;
	if (!Test.TestTrue(TEXT("the embedded scoped export request canonicalizes"),
		TryBuildNiagaraExportParams(ObjectPath, WorkspacePath, ParamsJson, Error))
		|| !Test.TestTrue(TEXT("the public embedded scoped export succeeds"),
			TrySubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), ParamsJson, Error)))
	{
		Test.AddError(Error);
		return false;
	}
	const TMap<FString, FString> ControlVisibleBefore =
		CaptureVisibleOwnerFiles(WorkspacePath, ControlName);
	const TMap<FString, FString> TargetVisibleBefore =
		CaptureVisibleOwnerFiles(WorkspacePath, TargetName);
	TMap<FString, FString> RootVisibleExpected;
	if (bRootProperty)
	{
		RootVisibleExpected = CaptureVisibleOwnerFiles(WorkspacePath, FString());
		int32 EditedRootLeaves = 0;
		for (auto& Expected : RootVisibleExpected)
		{
			TSharedPtr<FJsonObject> Document;
			if (!CommandContract::Json::TryParseObject(Expected.Value, Document, Error)) return false;
			const int32 Count = SetVisibleSystemRootBool(Document, true);
			EditedRootLeaves += Count;
			if (Count && (!CommandContract::Json::TryCanonicalize(Document.ToSharedRef(), Expected.Value, Error)
				|| !FFileHelper::SaveStringToFile(Expected.Value, *(WorkspacePath / Expected.Key),
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
		}
		if (!Test.TestEqual(TEXT("the ordinary directory edits exactly one root bool leaf"), EditedRootLeaves, 1))
		{
			for (const auto& Entry : RootVisibleExpected)
			{
				const int32 Offset = Entry.Value.Find(TEXT("Current"));
				if (Offset != INDEX_NONE || Entry.Key.Contains(TEXT("current")))
					Test.AddInfo(Entry.Key + TEXT(": ") + Entry.Value.Mid(FMath::Max(0, Offset - 80), 450));
			}
			return false;
		}
	}
	// Freeze the whole public expectation before import. Exactly one fixed leaf
	// differs; a later arbitrary change elsewhere in that document cannot pass.
	TMap<FString, FString> TargetVisibleExpected = TargetVisibleBefore;
	int32 ExpectedChangedDocuments = 0;
	for (auto& Expected : TargetVisibleExpected)
	{
		TSharedPtr<FJsonObject> Document;
		if (!CommandContract::Json::TryParseObject(Expected.Value, Document, Error)) return false;
		double BeforeMinX = 0.0;
		if (!bRootProperty && Expected.Value.Contains(TEXT("FixedBounds")) && TryGetFixedBoundsMinX(Document, BeforeMinX))
		{
			if (!Test.TestEqual(TEXT("the selected public leaf has its fixed baseline value"), BeforeMinX, -10.0)
				|| !TrySetFixedBoundsMinX(Document, -42.5)
				|| !CommandContract::Json::TryCanonicalize(Document.ToSharedRef(), Expected.Value, Error)) return false;
			++ExpectedChangedDocuments;
		}
	}
	if (!Test.TestEqual(TEXT("the public expectation changes the selected scope only"), ExpectedChangedDocuments, bRootProperty ? 0 : 1)) return false;
	if (!Test.TestTrue(TEXT("the control owner has nonempty visible content"), !ControlVisibleBefore.IsEmpty())
		|| (!bRootProperty && !Test.TestTrue(TEXT("the readable target owner accepts the compact Struct edit"),
			TryEditVisibleEmitterFixedBoundsMinXByName(WorkspacePath, TargetName, -42.5, Error)))
		|| !Test.TestTrue(TEXT("the embedded scoped import request canonicalizes"),
			TryBuildNiagaraDirectoryImportParams(ObjectPath, WorkspacePath, ParamsJson, Error)))
	{
		Test.AddError(Error);
		return false;
	}
	if (!Test.TestTrue(TEXT("the ordinary workspace edit matches only the predetermined leaf change"),
		CaptureVisibleOwnerFiles(WorkspacePath, TargetName).OrderIndependentCompareEqual(TargetVisibleExpected))) return false;
	const auto ImportResult = SubmitPublicCommand(
		Registry, Dispatch, TEXT("content_graph.import_workspace"), ParamsJson);
	if (!Test.TestTrue(TEXT("public import applies only the selected embedded owner"), ImportResult.IsOk()))
	{
		Test.AddError(ImportResult.GetErrorJson());
		return false;
	}
	UNiagaraSystem* Published = FindObject<UNiagaraSystem>(nullptr, *ObjectPath);
	if (!Test.TestNotNull(TEXT("public import leaves a published System"), Published)) return false;
	TWeakObjectPtr<UNiagaraSystem> PublishedWeak(Published);
	FNiagaraEmitterHandle* PublishedA = FindHandle(*Published, TargetHandleId);
	FNiagaraEmitterHandle* PublishedB = FindHandle(*Published, ControlHandleId);
	if (!Test.TestNotNull(TEXT("published A owner exists before unload"), PublishedA)
		|| !Test.TestNotNull(TEXT("published B owner exists before unload"), PublishedB)) return false;
	const TWeakObjectPtr<UNiagaraEmitter> PublishedEmitterWeakA(PublishedA->GetInstance().Emitter);
	const TWeakObjectPtr<UNiagaraEmitter> PublishedEmitterWeakB(PublishedB->GetInstance().Emitter);
	UPackage* PublishedPackage = Published->GetOutermost();
	Test.TestFalse(TEXT("public embedded import completes with a clean package"), PublishedPackage->IsDirty());
	Published = nullptr;
	FText UnloadError;
	if (!Test.TestTrue(TEXT("independent embedded System unload succeeds"),
		UPackageTools::UnloadPackages({PublishedPackage}, UnloadError, true)))
	{
		Test.AddError(UnloadError.ToString());
		return false;
	}
	if (!Test.TestFalse(TEXT("the independently unloaded System no longer exists"), PublishedWeak.IsValid())) return false;
	if (!Test.TestTrue(TEXT("both embedded objects are gone before independent disk reload"),
		!PublishedEmitterWeakA.IsValid() && !PublishedEmitterWeakB.IsValid())) return false;
	UNiagaraSystem* const Reloaded = LoadObject<UNiagaraSystem>(nullptr, *ObjectPath);
	FNiagaraEmitterHandle* const ReloadedA = Reloaded ? FindHandle(*Reloaded, TargetHandleId) : nullptr;
	FNiagaraEmitterHandle* const ReloadedB = Reloaded ? FindHandle(*Reloaded, ControlHandleId) : nullptr;
	UNiagaraEmitter* const ReloadedEmitterA = ReloadedA ? ReloadedA->GetInstance().Emitter : nullptr;
	UNiagaraEmitter* const ReloadedEmitterB = ReloadedB ? ReloadedB->GetInstance().Emitter : nullptr;
	if (!Test.TestNotNull(TEXT("the original target handle survives reload"), ReloadedA)
		|| !Test.TestNotNull(TEXT("the original control handle survives reload"), ReloadedB)
		|| !Test.TestNotNull(TEXT("the target handle retains an Emitter instance"), ReloadedEmitterA)
		|| !Test.TestNotNull(TEXT("the control handle retains an Emitter instance"), ReloadedEmitterB)) return false;
	Test.TestEqual(TEXT("the target readable owner name survives reload"),
		ReloadedA->GetName().ToString(), TargetName);
	Test.TestEqual(TEXT("the control readable owner name survives reload"),
		ReloadedB->GetName().ToString(), ControlName);
	// Existing provider canonicalizes an unversioned empty token to ExposedVersion.
	// UE GetEmitterData selects VersionData[0] in either case. Preserve that data
	// identity and exact owner binding; arbitrary nonempty version changes fail.
	auto VersionStillSelectsBaseline = [](const FNiagaraEmitterHandle& Handle,
		const FGuid& RawBefore, const FGuid& DataBefore, const FGuid& ExposedBefore)
	{
		const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
		const auto* Data = Instance.GetEmitterData();
		return Instance.Emitter && !Instance.Emitter->IsVersioningEnabled()
			&& Instance.Emitter->GetAllAvailableVersions().Num() == 1
			&& Data && Data == Instance.Emitter->GetLatestEmitterData()
			&& Data->Version.VersionGuid == DataBefore
			&& Instance.Emitter->GetExposedVersion().VersionGuid == ExposedBefore
			&& (Instance.Version == RawBefore || (!RawBefore.IsValid() && Instance.Version == ExposedBefore));
	};
	if (!Test.TestTrue(TEXT("both handles retain exact owner/data/version association"),
		Reloaded->GetEmitterHandles().Num() == 2 && ReloadedEmitterA != ReloadedEmitterB
		&& ReloadedEmitterA->GetOuter() == Reloaded && ReloadedEmitterB->GetOuter() == Reloaded
		&& ReloadedEmitterA->GetPathName(Reloaded) == BaselinePathA
		&& ReloadedEmitterB->GetPathName(Reloaded) == BaselinePathB
		&& ReloadedA->GetIsEnabled() == BaselineEnabledA && ReloadedB->GetIsEnabled() == BaselineEnabledB
		&& ReloadedA->GetEmitterMode() == BaselineModeA && ReloadedB->GetEmitterMode() == BaselineModeB
		&& VersionStillSelectsBaseline(*ReloadedA, BaselineRawVersionA, BaselineDataVersionA, BaselineExposedVersionA)
		&& VersionStillSelectsBaseline(*ReloadedB, BaselineRawVersionB, BaselineDataVersionB, BaselineExposedVersionB))) return false;
	if (bRootProperty)
	{
		if (!Test.TestTrue(TEXT("the root bool persists through independent disk reload"), Reloaded->bRequireCurrentFrameData)
			|| !Test.TestTrue(TEXT("all other native editable System root fields are unchanged"),
				CaptureOtherSystemRootAuthoring(*Reloaded).OrderIndependentCompareEqual(BaselineOtherRoot))) return false;
	}
	if (!Test.TestTrue(TEXT("the target embedded Struct is complete after reload"),
		HasExactBounds(ReloadedEmitterA, bRootProperty ? -10.0 : -42.5))) return false;
	const FVersionedNiagaraEmitterData* const ReloadedControlData = ReloadedB->GetInstance().GetEmitterData();
	if (!Test.TestTrue(TEXT("the control embedded bounds remain completely unchanged"),
		ReloadedControlData && ReloadedControlData->CalculateBoundsMode == ENiagaraEmitterCalculateBoundMode::Fixed
		&& ReloadedControlData->FixedBounds.Min == FVector(-70.0, -80.0, -90.0)
		&& ReloadedControlData->FixedBounds.Max == FVector(70.0, 80.0, 90.0)
		&& ReloadedControlData->FixedBounds.IsValid == 1)) return false;
	const bool bTargetPreserved = SameEmitterContent(Test, BaselineContentA,
		*ReloadedEmitterA, TEXT("target complete native owned/pin content"), true, true);
	const bool bControlPreserved = SameEmitterContent(Test, BaselineContentB,
		*ReloadedEmitterB, TEXT("control complete native owned/pin content"), true, true);
	if (!bTargetPreserved || !bControlPreserved) return false;
	if (!Test.TestTrue(TEXT("the persisted embedded System independently re-exports"),
		TryBuildNiagaraExportParams(ObjectPath, ReadbackPath, ParamsJson, Error))
		|| !Test.TestTrue(TEXT("the public embedded readback export succeeds"),
			TrySubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), ParamsJson, Error)))
	{
		Test.AddError(Error);
		return false;
	}
	double ReexportedMinX = 0.0;
	Test.TestTrue(TEXT("the public re-export retains the selected owner value"),
		TryReadVisibleEmitterFixedBoundsMinXByName(
			ReadbackPath, TargetName, ReexportedMinX, Error)
		&& FMath::IsNearlyEqual(ReexportedMinX, bRootProperty ? -10.0 : -42.5));
	Test.TestTrue(TEXT("every visible control-owner document remains unchanged"),
		CaptureVisibleOwnerFiles(ReadbackPath, ControlName).OrderIndependentCompareEqual(
			ControlVisibleBefore));
	Test.TestTrue(TEXT("public target documents match the predetermined single-leaf expectation"),
		CaptureVisibleOwnerFiles(ReadbackPath, TargetName).OrderIndependentCompareEqual(TargetVisibleExpected));
	if (bRootProperty) Test.TestTrue(TEXT("all public documents match the predetermined root-only edit"),
		CaptureVisibleOwnerFiles(ReadbackPath, FString()).OrderIndependentCompareEqual(RootVisibleExpected));
	return !Test.HasAnyErrors();
}

} // namespace

bool FUeAgentT208NiagaraEmbeddedEmitterScopedPropertyVerticalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	return RunSystemPropertyWorkspaceTest(*this, false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT208NiagaraSystemRootPropertyVerticalTest,
	"UAI.Root.T208.Niagara.SystemRootPropertyVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208NiagaraSystemRootPropertyVerticalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	return RunSystemPropertyWorkspaceTest(*this, true);
}

namespace
{
	bool SaveScript(UPackage& Package, UNiagaraScript& Script, const FString& Filename)
	{
		Package.MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(&Package, &Script, *Filename, Args);
		if (bSaved)
		{
			Package.SetDirtyFlag(false);
		}
		return bSaved;
	}

	int32 SetVisibleStringLeaf(
		const TSharedPtr<FJsonValue>& Value,
		const FString& FieldName,
		const FString& Before,
		const FString& After)
	{
		if (!Value.IsValid()) return 0;
		if (Value->Type == EJson::Array)
		{
			int32 Changed = 0;
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				Changed += SetVisibleStringLeaf(Item, FieldName, Before, After);
			}
			return Changed;
		}
		if (Value->Type != EJson::Object) return 0;

		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		int32 Changed = 0;
		FString EntryName;
		FString EntryValue;
		const bool bNamedEntry =
			(Object->TryGetStringField(TEXT("name"), EntryName)
				|| Object->TryGetStringField(TEXT("property_path"), EntryName))
			&& MatchesPropertyName(EntryName, FieldName);
		if (bNamedEntry
			&& Object->TryGetStringField(TEXT("value"), EntryValue)
			&& EntryValue == Before)
		{
			Object->SetStringField(TEXT("value"), After);
			++Changed;
		}
		const TSharedPtr<FJsonObject>* EntryValueObject = nullptr;
		if (bNamedEntry
			&& Object->TryGetObjectField(TEXT("value"), EntryValueObject)
			&& EntryValueObject && (*EntryValueObject)->TryGetStringField(
				TEXT("selected"), EntryValue)
			&& EntryValue == Before)
		{
			(*EntryValueObject)->SetStringField(TEXT("selected"), After);
			++Changed;
		}
		// TMap::Add may allocate before replacing an existing entry. Keep the
		// traversal stable while SetStringField updates the original object.
		const auto Fields = Object->Values;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Fields)
		{
			FString DirectValue;
			if (MatchesPropertyName(Field.Key, FieldName)
				&& Field.Value.IsValid()
				&& Field.Value->TryGetString(DirectValue)
				&& DirectValue == Before)
			{
				Object->SetStringField(Field.Key, After);
				++Changed;
				continue;
			}
			if (MatchesPropertyName(Field.Key, FieldName)
				&& Field.Value.IsValid()
				&& Field.Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> EnumValue = Field.Value->AsObject();
				if (EnumValue->HasField(TEXT("type")) && EnumValue->TryGetStringField(TEXT("value"), DirectValue)
					&& DirectValue == Before)
				{
					EnumValue->SetStringField(TEXT("value"), After);
					++Changed;
					continue;
				}
				if (EnumValue->TryGetStringField(TEXT("selected"), DirectValue)
					&& DirectValue == Before)
				{
					EnumValue->SetStringField(TEXT("selected"), After);
					++Changed;
					continue;
				}
			}
			Changed += SetVisibleStringLeaf(Field.Value, FieldName, Before, After);
		}
		return Changed;
	}

	TMap<FString, FString> CaptureScriptNativeStructure(UNiagaraScript& Script, bool bAuthoringEquivalence = true)
	{
		TMap<FString, FString> Result;
		TFunction<void(UStruct*, void*, UObject*, const FString&)> Capture;
		Capture = [&](UStruct* Type, void* Data, UObject* Owner, const FString& Prefix)
		{
			for (TFieldIterator<FProperty> It(Type); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
				// The same native compilation caches as the Emitter oracle above.
				// Only successful compile/reload equivalence excludes them; rollback
				// captures every non-transient leaf, including generated identities.
				if (bAuthoringEquivalence && Owner->IsA<UNiagaraGraph>()
					&& (It->GetFName() == TEXT("LastBuiltTraversalDataChangeId")
						|| It->GetFName() == TEXT("LastBuiltScriptVersionId")
						|| It->GetFName() == TEXT("CachedUsageInfo")
						|| It->GetFName() == TEXT("bHasValidLastBuiltScriptVersionId")
						|| It->GetFName() == TEXT("ChangeId")
						|| It->GetFName() == TEXT("ForceRebuildId"))) continue;
				if (bAuthoringEquivalence && Owner->IsA<UNiagaraScript>()
					&& ((Type == Owner->GetClass() && It->GetFName() == TEXT("CachedScriptVMId"))
						|| (Type == FVersionedNiagaraScriptData::StaticStruct()
							&& It->GetFName() == TEXT("LastGeneratedVMId")))) continue;
				if (Owner->IsA<UNiagaraScript>() && Type == Owner->GetClass()
					&& It->GetFName() == TEXT("VersionData"))
				{
					const auto* Array = CastFieldChecked<FArrayProperty>(*It);
					FScriptArrayHelper Items(Array, Array->ContainerPtrToValuePtr<void>(Data));
					Result.Add(Prefix + TEXT("/VersionData/@count"), FString::FromInt(Items.Num()));
					for (int32 Index = 0; Index < Items.Num(); ++Index)
						Capture(FVersionedNiagaraScriptData::StaticStruct(), Items.GetRawPtr(Index), Owner,
							Prefix + FString::Printf(TEXT("/VersionData/%d"), Index));
					continue;
				}
				FString Value;
				It->ExportTextItem_Direct(Value, It->ContainerPtrToValuePtr<void>(Data), nullptr, Owner, PPF_Delimited);
				Result.Add(Prefix + TEXT("/") + It->GetName(), MoveTemp(Value));
			}
		};
		Capture(Script.GetClass(), &Script, &Script, TEXT("root"));
		TArray<UObject*> Owned;
		GetObjectsWithOuter(&Script, Owned, true);
		for (UObject* Object : Owned)
		{
			const FString Prefix = TEXT("owned/") + Object->GetPathName(&Script);
			Result.Add(Prefix + TEXT("/@class"), Object->GetClass()->GetPathName());
			Capture(Object->GetClass(), Object, Object, Prefix);
			if (const auto* Node = Cast<UEdGraphNode>(Object))
				for (int32 Index = 0; Index < Node->Pins.Num(); ++Index)
				{
					FString Value;
					if (Node->Pins[Index]) Node->Pins[Index]->ExportTextItem(Value, PPF_Delimited);
					Result.Add(Prefix + FString::Printf(TEXT("/@pin/%d"), Index), MoveTemp(Value));
				}
		}
		return Result;
	}

	bool SameScriptNativeStructure(FAutomationTestBase& Test, const TMap<FString, FString>& Expected,
		UNiagaraScript& Actual, const TCHAR* Phase, bool bAuthoringEquivalence = true)
	{
		const auto Values = CaptureScriptNativeStructure(Actual, bAuthoringEquivalence);
		bool bSame = Test.TestEqual(Phase, Values.Num(), Expected.Num());
		for (const auto& Pair : Expected)
			if (!Values.Contains(Pair.Key) || Values[Pair.Key] != Pair.Value)
			{
				Test.AddError(FString(Phase) + TEXT(": native content differs at ") + Pair.Key);
				bSame = false;
			}
		return bSame;
	}

	FString ExportPlatforms(const UNiagaraValidationRule_NoEvents& Rule)
	{
		const FProperty* const Property = FindFProperty<FProperty>(
			Rule.GetClass(), TEXT("Platforms"));
		FString Value;
		if (Property)
		{
			Property->ExportTextItem_Direct(
				Value, Property->ContainerPtrToValuePtr<void>(&Rule), nullptr,
				const_cast<UNiagaraValidationRule_NoEvents*>(&Rule), PPF_Delimited);
		}
		return Value;
	}

	int32 RemoveFirstVisibleValidationRule(const TSharedPtr<FJsonValue>& Value, FAutomationTestBase& Test, bool bSameClass = false)
	{
		if (!Value) return 0;
		int32 Changed = 0;
		if (Value->Type == EJson::Array)
		{
			for (const auto& Item : Value->AsArray()) Changed += RemoveFirstVisibleValidationRule(Item, Test, bSameClass);
			return Changed;
		}
		if (Value->Type != EJson::Object) return 0;
		for (auto& Field : Value->AsObject()->Values)
		{
			if (MatchesPropertyName(Field.Key, TEXT("ValidationRules")))
			{
				if (!Test.TestTrue(TEXT("the ordinary rules property carries its exported type and value"), Field.Value->Type == EJson::Object)) return 0;
				const auto Row = Field.Value->AsObject();
				FString Type;
				const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
				if (!Test.TestTrue(TEXT("the exported owned rules value is an object_ref array"),
					Row->TryGetStringField(TEXT("type"), Type) && Type == TEXT("object_ref[]")
					&& Row->TryGetArrayField(TEXT("value"), Values))) return 0;
				auto Items = *Values;
				if (!Test.TestEqual(TEXT("the exported rules array contains both native rules"), Items.Num(), 2)) return 0;
				if (!Test.TestFalse(TEXT("existing rules do not repeat a class selector"),
					Items[0]->AsObject()->GetObjectField(TEXT("owned_object"))->HasField(TEXT("class_path")))
					|| !Test.TestFalse(TEXT("the retained rule also has no class selector"),
						Items[1]->AsObject()->GetObjectField(TEXT("owned_object"))->HasField(TEXT("class_path")))) return 0;
				const FString FirstKey = Items[0]->AsObject()->GetObjectField(TEXT("owned_object"))->GetStringField(TEXT("instance"));
				const FString SecondKey = Items[1]->AsObject()->GetObjectField(TEXT("owned_object"))->GetStringField(TEXT("instance"));
				if (!Test.TestTrue(TEXT("existing ordered instances have distinct readable keys"),
					!FirstKey.IsEmpty() && !SecondKey.IsEmpty() && FirstKey != SecondKey)) return 0;
				Items.RemoveAt(0);
				Row->SetArrayField(TEXT("value"), MoveTemp(Items));
				return 1;
			}
			Changed += RemoveFirstVisibleValidationRule(Field.Value, Test, bSameClass);
		}
		return Changed;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT208NiagaraScriptValidationRulesOwnedPropertyVerticalTest,
	"UAI.Root.T208.Niagara.ScriptValidationRulesOwnedPropertyVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT208NiagaraScriptOwnedArrayProjectionTest,
	"UAI.Root.T208.Niagara.ScriptOwnedArrayProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208NiagaraScriptOwnedArrayProjectionTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Composition::HjsonImport;
	// Reflection-only component: no asset lifecycle, compiler, or persistent package.
	TStrongObjectPtr<UNiagaraScript> Script(NewObject<UNiagaraScript>());
	auto* First = NewObject<UNiagaraValidationRule_NoEvents>(Script.Get(), TEXT("LegacyRule91"));
	auto* Second = NewObject<UNiagaraValidationRule_NoEvents>(Script.Get(), TEXT("LegacyRule7"));
	First->Severity = ENiagaraValidationSeverity::Warning;
	Second->Severity = ENiagaraValidationSeverity::Error;
	Script->ValidationRules = {First, nullptr, Second};
	const FString ExpectedPath = Second->GetPathName(Script.Get());
	const FString SemanticName = Second->GetClass()->GetDisplayNameText().ToString();
	UeAgentAuthoringState::FEditablePropertyReadOptions Options;
	Options.IncludeProperty = [](const FProperty* Property, const void*, const UObject*) { return Property->GetFName() == TEXT("ValidationRules"); };
	TArray<UeAgentAuthoringState::FReadRequest> Reads;
	UeAgentAuthoringState::FCaptureResult Capture;
	FString Error;
	if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(Script.Get(), Options, Reads, Error)
		|| !UeAgentAuthoringState::CaptureSnapshot(Reads, Capture)) { AddError(Error + Capture.Diagnostic.Code); return false; }
	const auto Entries = Capture.Snapshot.CopyEntries();
	if (!TestEqual(TEXT("the native owned property is captured once"), Entries.Num(), 1)) return false;
	const auto Definition = Entries[0].ValueDefinition->AsObject();
	TestEqual(TEXT("nullable arrays retain their native owned element contract"), Definition->GetStringField(TEXT("element_kind")), FString(TEXT("owned_object")));
	TestEqual(TEXT("the null position retains its typed definition"), Definition->GetArrayField(TEXT("items"))[1]->AsObject()->GetStringField(TEXT("builtin")), FString(TEXT("owned_object")));
	if (!RegisterGraphTreeTargetCompositionContracts(Error) || !RegisterOtherTargetCompositionContracts(Error)) { AddError(Error); return false; }
	auto Resources = MakeSemanticWorkspaceResourceTracker();
	FUeAgentCanonicalSnapshotPtr Snapshot;
	FAuthoringModelPtr Model;
	FWorkspaceError WorkspaceError;
	if (!TryBuildCanonicalAuthoringCapture(TEXT("ue.niagara_script.asset"), TEXT("llm_authoring"), Capture.Snapshot,
		Resources, Snapshot, Model, WorkspaceError)) { AddError(WorkspaceError.GetCode()); return false; }
	FExportWorkspacePlanPtr Plan;
	if (!TryBuildCompositionExportPlan(Model.ToSharedRef(),
		FCompositionSnapshotBinding(Snapshot->GetSnapshotId(), Snapshot->GetRevision(), Snapshot->GetProvenance().GetEvidenceId()),
		{}, Resources, Plan, WorkspaceError)) { AddError(WorkspaceError.GetCode()); return false; }
	const FString WorkspacePath = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T208/OwnedArrayComponent_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*WorkspacePath, false, true); };
	FString PropertyFile;
	TSharedPtr<FJsonObject> PropertyDocument;
	TSharedPtr<FJsonObject> PropertyRow;
	for (const auto& File : Plan->GetFiles())
	{
		const FString Path = WorkspacePath / File.GetRelativePath();
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		if (!FFileHelper::SaveArrayToFile(File.GetCanonicalUtf8(), *Path)) return false;
		if (PropertyRow || !File.GetRelativePath().EndsWith(TEXT("properties.hjson"))) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path) || !FUeAgentStructuredHjsonParser::ParseObject(Text, Path, PropertyDocument, Error)) return false;
		TFunction<void(const TSharedPtr<FJsonValue>&)> FindProperty;
		FindProperty = [&](const TSharedPtr<FJsonValue>& Value) {
			if (Value->Type != EJson::Object) return;
			for (const auto& Field : Value->AsObject()->Values)
				if (MatchesPropertyName(Field.Key, TEXT("ValidationRules"))) PropertyRow = Field.Value->AsObject();
				else FindProperty(Field.Value);
		};
		FindProperty(MakeShared<FJsonValueObject>(PropertyDocument));
		if (PropertyRow) PropertyFile = Path;
	}
	if (!TestTrue(TEXT("ordinary owned array row is projected"), PropertyRow.IsValid())) return false;
	auto Items = PropertyRow->GetArrayField(TEXT("value"));
	TestEqual(TEXT("semantic alias ignores native historic numbering"), Items[0]->AsObject()->GetObjectField(TEXT("owned_object"))->GetStringField(TEXT("instance")), SemanticName);
	TestEqual(TEXT("same-class semantic occurrences are consecutive despite null"), Items[2]->AsObject()->GetObjectField(TEXT("owned_object"))->GetStringField(TEXT("instance")), SemanticName + TEXT("2"));
	auto BuildDraft = [&](TOptional<UE::UAI::PlanningPreflight::FChangeSetDraft>& Draft, FSemanticWorkspaceImportError& ImportError) {
		FResolvedSemanticWorkspacePtr Workspace;
		if (!TryLoadHjsonImportSource(FWorkspaceSource::Directory(WorkspacePath), ESemanticWorkspaceMode::State, Resources, Workspace, WorkspaceError))
		{ AddError(WorkspaceError.GetCode()); return false; }
		return TryBuildSemanticChangeSetDraft(Workspace.ToSharedRef(), Snapshot.ToSharedRef(),
			Contract::FUeAgentDescriptorIdentity(TEXT("owned-array-component"), 1, Model->GetSemanticFingerprint()),
			TEXT("owned-array-component"), TEXT("owned-array-component:1"), Resources, Draft, ImportError, Model);
	};
	TOptional<UE::UAI::PlanningPreflight::FChangeSetDraft> Draft;
	FSemanticWorkspaceImportError ImportError;
	TestFalse(TEXT("nullable unchanged workspace has no mutation"), BuildDraft(Draft, ImportError));
	if (!TestEqual(TEXT("unchanged workspace reaches the semantic no-change result"), ImportError.Code, FString(TEXT("hjson_import_no_explicit_changes")))) return false;
	Items.RemoveAt(0);
	PropertyRow->SetArrayField(TEXT("value"), Items);
	FString Edited;
	if (!CommandContract::Json::TryCanonicalize(PropertyDocument.ToSharedRef(), Edited, Error)
		|| !FFileHelper::SaveStringToFile(Edited, *PropertyFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	if (!TestTrue(TEXT("ordinary nullable deletion reaches the semantic overlay"), BuildDraft(Draft, ImportError))) { AddError(ImportError.Code); return false; }
	if (!TestEqual(TEXT("one owned array change is prepared"), Draft->GetChanges().Num(), 1)) return false;
	TSharedPtr<FJsonValue> Desired;
	if (!CanonicalJson::TryToJsonValue(*Draft->GetChanges()[0].GetProposedAfter(), Desired, Error)) return false;
	const auto DesiredItems = Desired->AsObject()->GetArrayField(TEXT("items"));
	TestEqual(TEXT("deletion preserves the explicit null and retained rule"), DesiredItems.Num(), 2);
	TestEqual(TEXT("the moved same-class rule binds its original native owner path"), DesiredItems[1]->AsObject()->GetObjectField(TEXT("owned_object"))->GetStringField(TEXT("owner_relative_path")), ExpectedPath);
	TestEqual(TEXT("the reflection-only pipeline has not mutated native order"), Script->ValidationRules.Num(), 3);
	Script->ValidationRules.Empty();
	UeAgentAuthoringState::FCaptureResult EmptyCapture;
	if (!UeAgentAuthoringState::CaptureSnapshot(Reads, EmptyCapture)) return false;
	TestEqual(TEXT("an empty native array retains the owned element contract"), EmptyCapture.Snapshot.CopyEntries()[0].ValueDefinition->AsObject()->GetStringField(TEXT("element_kind")), FString(TEXT("owned_object")));
	return !HasAnyErrors();
}

namespace
{
bool TryDefinitionSemantics(const TSharedPtr<FJsonObject>& Document, FString& OutCanonical, FString& OutError)
{
	const TSharedPtr<FJsonObject>* Definitions = nullptr;
	if (!Document || !Document->TryGetObjectField(TEXT("definitions"), Definitions)) { OutError = TEXT("definition_catalog_missing"); return false; }
	TMap<FString, TSharedPtr<FJsonObject>> Bindings;
	for (const auto& Row : (*Definitions)->Values) {
		FString Ref;
		if (!Row.Value || Row.Value->Type != EJson::Object || !Row.Value->AsObject()->TryGetStringField(TEXT("ref"), Ref)
			|| !Ref.StartsWith(TEXT("collection:")) || Bindings.Contains(Ref)) { OutError = TEXT("definition_public_binding_invalid:") + Row.Key; return false; }
		Bindings.Add(Ref, Row.Value->AsObject());
	}
	TSet<FString> Stack;
	TFunction<TSharedPtr<FJsonValue>(const TSharedPtr<FJsonValue>&, bool, bool)> Expand;
	Expand = [&](const TSharedPtr<FJsonValue>& Value, bool bDefinitionRoot, bool bReferenceSlot) -> TSharedPtr<FJsonValue> {
		if (!Value) return nullptr;
		if (bReferenceSlot && Value->Type == EJson::String && Value->AsString().StartsWith(TEXT("collection:"))) {
			const FString Ref = Value->AsString(); const auto* Bound = Bindings.Find(Ref);
			if (!Bound || Stack.Contains(Ref)) { OutError = TEXT("definition_public_reference_unresolved:") + Ref; return nullptr; }
			Stack.Add(Ref); auto Expanded = Expand(MakeShared<FJsonValueObject>(*Bound), true, false); Stack.Remove(Ref); return Expanded;
		}
		if (Value->Type == EJson::Array) {
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const auto& Item : Value->AsArray()) { auto Expanded = Expand(Item, false, bReferenceSlot); if (!Expanded) return nullptr; Items.Add(Expanded); }
			return MakeShared<FJsonValueArray>(Items);
		}
		if (Value->Type != EJson::Object) return Value;
		const auto Result = MakeShared<FJsonObject>();
		for (const auto& Field : Value->AsObject()->Values) {
			if (bDefinitionRoot && Field.Key == TEXT("ref")) continue;
			TSharedPtr<FJsonValue> Expanded;
			if (Field.Key == TEXT("fields") && Field.Value->Type == EJson::Object) {
				const auto Fields = MakeShared<FJsonObject>();
				for (const auto& Member : Field.Value->AsObject()->Values) { auto Child = Expand(Member.Value, false, true); if (!Child) return nullptr; Fields->SetField(Member.Key, Child); }
				Expanded = MakeShared<FJsonValueObject>(Fields);
			} else Expanded = Expand(Field.Value, false, Field.Key == TEXT("value_definition") || Field.Key == TEXT("items"));
			if (!Expanded) return nullptr; Result->SetField(Field.Key, Expanded);
		}
		return MakeShared<FJsonValueObject>(Result);
	};
	TArray<FString> Contents;
	for (const auto& Row : (*Definitions)->Values) {
		auto Expanded = Expand(Row.Value, true, false); FString Canonical;
		if (!Expanded || !CommandContract::Json::TryCanonicalize(Expanded->AsObject().ToSharedRef(), Canonical, OutError)) return false;
		Contents.Add(MoveTemp(Canonical));
	}
	Contents.Sort();
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const auto& Content : Contents) Values.Add(MakeShared<FJsonValueString>(Content));
	const auto Result = MakeShared<FJsonObject>();
	for (const auto& Field : Document->Values) if (Field.Key != TEXT("definitions")) Result->SetField(Field.Key, Field.Value);
	Result->SetArrayField(TEXT("definitions"), Values);
	return CommandContract::Json::TryCanonicalize(Result, OutCanonical, OutError);
}

bool TryScriptDefinitionDocument(UNiagaraScript& Script, TSharedPtr<FJsonObject>& OutDocument, FString& OutError)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;
	TArray<UeAgentAuthoringState::FReadRequest> Requests;
	UeAgentAuthoringState::FCaptureResult Capture;
	if (!UeAgentNiagaraAuthoring::BuildAssetExportSnapshotReadRequests(Script, Requests, OutError)
		|| !UeAgentAuthoringState::CaptureSnapshot(Requests, Capture)) { OutError += Capture.Diagnostic.Code + Capture.Diagnostic.InternalContext; return false; }
	auto Resources = MakeSemanticWorkspaceResourceTracker();
	FUeAgentCanonicalSnapshotPtr Snapshot; FAuthoringModelPtr Model; FWorkspaceError Error;
	if (!TryBuildCanonicalAuthoringCapture(TEXT("ue.niagara_script.asset"), TEXT("llm_authoring"), Capture.Snapshot,
		Resources, Snapshot, Model, Error)) { OutError = Error.GetCode(); return false; }
	FExportWorkspacePlanPtr Plan;
	if (!TryBuildCompositionExportPlan(Model.ToSharedRef(),
		FCompositionSnapshotBinding(Snapshot->GetSnapshotId(), Snapshot->GetRevision(), Snapshot->GetProvenance().GetEvidenceId()),
		{}, Resources, Plan, Error)) { OutError = Error.GetCode(); return false; }
	for (const auto& File : Plan->GetFiles()) if (File.GetRelativePath().EndsWith(TEXT("definitions.hjson"))) {
		if (OutDocument) { OutError = TEXT("expected_definition_catalog_ambiguous"); return false; }
		FString Text; FFileHelper::BufferToString(Text, File.GetCanonicalUtf8().GetData(), File.GetCanonicalUtf8().Num());
		if (!FUeAgentStructuredHjsonParser::ParseObject(Text, File.GetRelativePath(), OutDocument, OutError)) return false;
	}
	return OutDocument.IsValid();
}

bool SameScriptVisibleFiles(FAutomationTestBase& Test, const TMap<FString, FString>& Expected,
	const TMap<FString, FString>& Actual, const TSharedPtr<FJsonObject>& ExpectedDefinitions)
{
	bool bSame = Test.TestEqual(TEXT("all public files remain accounted for"), Actual.Num(), Expected.Num());
	for (const auto& File : Expected) {
		const auto* Found = Actual.Find(File.Key);
		if (!Found) { Test.AddError(TEXT("missing public file:") + File.Key); bSame = false; continue; }
		FString Before = File.Value, After = *Found, Error;
		if (File.Key.EndsWith(TEXT("definitions.hjson"))) {
			TSharedPtr<FJsonObject> Captured, Readback;
			if (!FUeAgentStructuredHjsonParser::ParseObject(Before, File.Key, Captured, Error)
				|| !FUeAgentStructuredHjsonParser::ParseObject(After, File.Key, Readback, Error)
				|| !TryDefinitionSemantics(ExpectedDefinitions ? ExpectedDefinitions : Captured, Before, Error)
				|| !TryDefinitionSemantics(Readback, After, Error)) { Test.AddError(Error); return false; }
		}
		if (Before == After) continue;
		bSame = false; int32 Offset = 0;
		while (Offset < Before.Len() && Offset < After.Len() && Before[Offset] == After[Offset]) ++Offset;
		const int32 Start = FMath::Max(0, Offset - 80);
		Test.AddError(File.Key + FString::Printf(TEXT(" semantic difference at %d expected="), Offset) + Before.Mid(Start, 320) + TEXT(" actual=") + After.Mid(Start, 320));
	}
	return bSame;
}

bool RunScriptValidationRulesTest(FAutomationTestBase& Test, bool bRemoveFirst, bool bSameClass = false, bool bLateRemoval = false, bool bAddNew = false, bool bClear = false, bool bNativeName = false, bool bRejectName = false, bool bContextChanges = false)
{
	// Each independently registered scenario owns its package. An earlier
	// scenario's unreferenced objects must not enter this scenario's baseline.
	const FString PackageName = FString::Printf(
		TEXT("/Game/UAI/Transient/T85NiagaraScriptValidationRules/NS_ValidationRules_%d%d%d%d%d%d%d%d"),
		bRemoveFirst, bSameClass, bLateRemoval, bAddNew, bClear, bNativeName, bRejectName, bContextChanges);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	DeleteAssetIfPresent(ObjectPath);
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test, PackageName);
	};

	UPackage* const Package = CreatePackage(*PackageName);
	UNiagaraModuleScriptFactory* const Factory =
		NewObject<UNiagaraModuleScriptFactory>();
	UNiagaraScript* Script = Package && Factory
		? Cast<UNiagaraScript>(Factory->FactoryCreateNew(
			UNiagaraScript::StaticClass(), Package, *AssetName,
			RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn))
		: nullptr;
	if (!Test.TestNotNull(TEXT("the native Niagara factory creates a standalone Script"), Script))
	{
		return false;
	}
	const auto* NativeSource = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
	if (!Test.TestNotNull(TEXT("the native Script owns a valid compiler source"), NativeSource)
		|| !Test.TestNotNull(TEXT("the native compiler source owns a graph"), NativeSource->NodeGraph.Get())) return false;
	UNiagaraValidationRule_NoEvents* NoEvents =
		NewObject<UNiagaraValidationRule_NoEvents>(Script, NAME_None, RF_Transactional);
	UNiagaraValidationRule* ControlRule = bSameClass
		? static_cast<UNiagaraValidationRule*>(NewObject<UNiagaraValidationRule_NoEvents>(Script, NAME_None, RF_Transactional))
		: static_cast<UNiagaraValidationRule*>(NewObject<UNiagaraValidationRule_NoWarmupTime>(Script, NAME_None, RF_Transactional));
	if (bSameClass)
	{
		auto* SameClassControl = CastChecked<UNiagaraValidationRule_NoEvents>(ControlRule);
		SameClassControl->Severity = ENiagaraValidationSeverity::Error;
		SameClassControl->Platforms.QualityLevelMask = 9;
	}
	if (!Test.TestNotNull(TEXT("the Script owns a native NoEvents rule"), NoEvents)
		|| !Test.TestNotNull(TEXT("the Script owns an unedited control rule"), ControlRule))
	{
		return false;
	}
	NoEvents->Severity = ENiagaraValidationSeverity::Warning;
	NoEvents->Platforms.QualityLevelMask = 5;
	Script->ValidationRules = {NoEvents, ControlRule};
	FAssetRegistryModule::AssetCreated(Script);
	FString BaselineCompileError;
	if (!Test.TestTrue(TEXT("the valid native Script baseline compiles before content is fixed"),
		UeAgentNiagaraAuthoring::CompileAsset(Script, BaselineCompileError)))
	{
		Test.AddError(BaselineCompileError);
		return false;
	}
	if (!Test.TestTrue(TEXT("the standalone Script fixture saves"),
		SaveScript(*Package, *Script, Filename)))
	{
		return false;
	}

	const FString BaselinePlatforms = ExportPlatforms(*NoEvents);
	if (bLateRemoval)
	{
		UE::UAI::Contract::FDiagnostic RegistrationError;
		if (!UeAgentNiagaraAuthoring::RegisterAuthoring(RegistrationError)) { Test.AddError(RegistrationError.ToPublicDetail()); return false; }
		TArray<uint8> OriginalDisk;
		if (!FFileHelper::LoadFileToArray(OriginalDisk, *Filename)) return false;
		const FString RemovedPath = NoEvents->GetPathName(Script);
		const FString RetainedPath = ControlRule->GetPathName(Script);
		const FString RemovedPrefix = TEXT("owned/") + RemovedPath + TEXT("/");
		NoEvents->Severity = ENiagaraValidationSeverity::Info;
		NoEvents->Platforms.QualityLevelMask = 3;
		auto* Source = CastChecked<UNiagaraScriptSource>(Script->GetLatestSource());
		if (!Test.TestTrue(TEXT("late deletion retains a real native graph"), Source->NodeGraph && !Source->NodeGraph->Nodes.IsEmpty())) return false;
		Source->NodeGraph->Nodes[0]->NodeComment = TEXT("dirty structure before owned deletion");
		Script->MarkPackageDirty();
		const auto DirtyNative = CaptureScriptNativeStructure(*Script, false);
		auto DesiredNative = CaptureScriptNativeStructure(*Script);
		auto DesiredRules = Script->ValidationRules;
		if (!bNativeName) DesiredRules.RemoveAt(0);
		FString DesiredReferences;
		FindFProperty<FArrayProperty>(Script->GetClass(), TEXT("ValidationRules"))->ExportTextItem_Direct(
			DesiredReferences, &DesiredRules, nullptr, Script, PPF_Delimited);
		DesiredNative[TEXT("root/ValidationRules")] = DesiredReferences;
		if (!bNativeName)
			for (auto It = DesiredNative.CreateIterator(); It; ++It) if (It.Key().StartsWith(RemovedPrefix)) It.RemoveCurrent();
		auto MatchesNativeCreation = [&](UNiagaraScript& Current)
		{
			if (Current.ValidationRules.Num() != 3 || !Current.ValidationRules[0] || !Current.ValidationRules[1]) return false;
			auto* Added = Cast<UNiagaraValidationRule_EmitterCount>(Current.ValidationRules[2]);
			if (!Added || Added->GetOuter() != &Current || Added->Severity != ENiagaraValidationSeverity::Warning
				|| !Added->EmitterCountLimits.IsEmpty() || !Added->IsEnabled()
				|| Current.ValidationRules[0]->GetPathName(&Current) != RemovedPath
				|| Current.ValidationRules[1]->GetPathName(&Current) != RetainedPath
				|| Current.ValidationRules[0]->GetOuter() != &Current || Current.ValidationRules[1]->GetOuter() != &Current) return false;
			auto Actual = CaptureScriptNativeStructure(Current);
			const FString Prefix = TEXT("owned/") + Added->GetPathName(&Current) + TEXT("/");
			TMap<FString, FString> AddedValues;
			for (auto It = Actual.CreateIterator(); It; ++It)
				if (It.Key().StartsWith(Prefix)) { AddedValues.Add(It.Key().RightChop(Prefix.Len()), It.Value()); It.RemoveCurrent(); }
			const TMap<FString, FString> DefaultValues{{TEXT("@class"), UNiagaraValidationRule_EmitterCount::StaticClass()->GetPathName()},
				{TEXT("Severity"), TEXT("Warning")}, {TEXT("EmitterCountLimits"), TEXT("")}, {TEXT("bIsConfigDisabled"), TEXT("False")}};
			auto Existing = DesiredNative;
			Actual.Remove(TEXT("root/ValidationRules")); Existing.Remove(TEXT("root/ValidationRules"));
			return Actual.OrderIndependentCompareEqual(Existing) && AddedValues.OrderIndependentCompareEqual(DefaultValues);
		};
		TStrongObjectPtr<UUeAgentT167AssetReferenceData> External(NewObject<UUeAgentT167AssetReferenceData>());
		External->HardReference = ControlRule;
		const TWeakObjectPtr<UNiagaraScript> DirtyWeak(Script);
		const TWeakObjectPtr<UPackage> DirtyPackageWeak(Script->GetOutermost());
		const TWeakObjectPtr<UNiagaraValidationRule> RemovedWeak(NoEvents);
		auto Owned = MakeShared<FJsonObject>();
		Owned->SetStringField(TEXT("owner_relative_path"), RetainedPath);
		Owned->SetStringField(TEXT("class_path"), ControlRule->GetClass()->GetPathName());
		auto Item = MakeShared<FJsonObject>(); Item->SetObjectField(TEXT("owned_object"), Owned);
		auto Descriptor = MakeShared<FJsonObject>(); Descriptor->SetStringField(TEXT("property_path"), TEXT("ValidationRules"));
		Descriptor->SetArrayField(TEXT("value"), {MakeShared<FJsonValueObject>(Item)});
		if (bNativeName)
		{
			auto FirstOwned = MakeShared<FJsonObject>(); FirstOwned->SetStringField(TEXT("owner_relative_path"), RemovedPath);
			FirstOwned->SetStringField(TEXT("class_path"), NoEvents->GetClass()->GetPathName());
			auto First = MakeShared<FJsonObject>(); First->SetObjectField(TEXT("owned_object"), FirstOwned);
			auto NewOwned = MakeShared<FJsonObject>(); NewOwned->SetStringField(TEXT("name"), UNiagaraValidationRule_EmitterCount::StaticClass()->GetDisplayNameText().ToString());
			auto New = MakeShared<FJsonObject>(); New->SetObjectField(TEXT("owned_object"), NewOwned);
			Descriptor->SetArrayField(TEXT("value"), {MakeShared<FJsonValueObject>(First), MakeShared<FJsonValueObject>(Item), MakeShared<FJsonValueObject>(New)});
		}
		UeAgentNiagaraAuthoring::FAssetApplyRequest Request;
		Request.AssetPath = PackageName; Request.AssetKind = TEXT("script"); Request.bCompactValues = true;
		Request.Properties = {MakeShared<FJsonValueObject>(Descriptor)};
		if (bContextChanges)
		{
			UeAgentAuthoringObjectApply::FPreparedApply Prepared;
			UeAgentAuthoringObjectApply::FFailure Failure;
			UeAgentAuthoringObjectApply::FOptions Options;
			Options.bCompactValues = true; Options.bAllowDomainAuthorizedOwners = true;
			if (!Test.TestTrue(TEXT("native creation prepares in the original editable context"),
				UeAgentAuthoringObjectApply::PrepareObject(Script, Request.Properties, Options, Prepared, Failure)))
			{ Test.AddError(Failure.Diagnostic.Code + TEXT(":") + Failure.Diagnostic.InternalContext); return false; }
			SameScriptNativeStructure(Test, DirtyNative, *Script, TEXT("prepare has no native mutation"), false);
			struct FFixtureReadOnlyPolicy final : PropertyEditorPolicy::IEditConstPolicy
			{
				const UObject* FixtureOwner;
				bool Allows(const UObject* Object) const { return Object != FixtureOwner; }
				bool CanEditProperty(const FEditPropertyChain&, const UObject* Object) const override { return Allows(Object); }
				bool CanEditProperty(const FProperty*, const UObject* Object) const override { return Allows(Object); }
			} Policy;
			Policy.FixtureOwner = Script;
			PropertyEditorPolicy::RegisterEditConstPolicy(&Policy);
			int32 RealModifyCount = 0;
			const bool bWasReported = FCoreUObjectDelegates::ObjectsModifiedThisFrame.Remove(Script) != 0;
			const auto Modified = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object) { if (Object == Script) ++RealModifyCount; });
			ON_SCOPE_EXIT {
				FCoreUObjectDelegates::OnObjectModified.Remove(Modified);
				if (bWasReported) FCoreUObjectDelegates::ObjectsModifiedThisFrame.Add(Script);
				else FCoreUObjectDelegates::ObjectsModifiedThisFrame.Remove(Script);
				PropertyEditorPolicy::UnregisterEditConstPolicy(&Policy);
			};
			Test.TestTrue(TEXT("the current native editor policy makes this fixture read-only"), PropertyEditorPolicy::IsPropertyEditConst(
				FindFProperty<FArrayProperty>(Script->GetClass(), TEXT("ValidationRules")), Script));
			UeAgentAuthoringObjectApply::FApplyResult ApplyResult;
			Test.TestFalse(TEXT("apply rechecks the current context before mutation"), UeAgentAuthoringObjectApply::ApplyPrepared(Prepared, ApplyResult));
			Test.TestEqual(TEXT("the real native handle rejects the newly read-only context"), ApplyResult.Diagnostic.Code, FString(TEXT("owned_creation_handle_not_editable")));
			Test.TestEqual(TEXT("context rejection occurs before real Modify, not only after rollback"), RealModifyCount, 0);
			Test.TestTrue(TEXT("original slots and references remain before any rollback"), Script->ValidationRules.Num() == 2
				&& Script->ValidationRules[0] == NoEvents && Script->ValidationRules[1] == ControlRule && External->HardReference == ControlRule);
			SameScriptNativeStructure(Test, DirtyNative, *Script, TEXT("context rejection is observed before explicit rollback"), false);
			Test.TestTrue(TEXT("write-before rejection preserves original dirty state"), Script->GetOutermost()->IsDirty());
			UeAgentAuthoringObjectApply::FApplyResult Rollback;
			Test.TestTrue(TEXT("the unmodified prepared plan can finish rollback"), UeAgentAuthoringObjectApply::RollbackPrepared(Prepared, Rollback));
			return !Test.HasAnyErrors();
		}
		const auto Observer = MakeShared<FCreateTextStageObserver>();
		bool bCheckpoint = false, bStaged = false, bFinal = false;
		TWeakObjectPtr<UNiagaraScript> StagedWeak, FinalWeak;
		Observer->BeforeStage = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure&)
		{
			auto* Current = FindObject<UNiagaraScript>(nullptr, *ObjectPath);
			TArray<uint8> Disk;
			if (!Current || !FFileHelper::LoadFileToArray(Disk, *Filename) || Disk != OriginalDisk) return false;
			if (Current->ValidationRules.Num() == 2)
				bCheckpoint = Current->GetOutermost()->IsDirty()
					&& SameScriptNativeStructure(Test, DirtyNative, *Current, TEXT("deletion checkpoint keeps all dirty native content"), false);
			else if (bNativeName && Current->ValidationRules.Num() == 3)
			{
				StagedWeak = Current;
				bStaged = MatchesNativeCreation(*Current);
			}
			else if (!bNativeName && Current->ValidationRules.Num() == 1)
			{
				StagedWeak = Current;
				auto Actual = CaptureScriptNativeStructure(*Current);
				// Unreferenced deleted subobjects may await native GC during staging.
				// The array must already drop them; fresh persistent readback below
				// requires their complete absence, and rollback remains full/exact.
				for (auto It = Actual.CreateIterator(); It; ++It) if (It.Key().StartsWith(RemovedPrefix)) It.RemoveCurrent();
				bStaged = Current->ValidationRules[0]->GetPathName(Current) == RetainedPath
					&& Actual.OrderIndependentCompareEqual(DesiredNative);
			}
			return true;
		};
		Request.PackageStager = Observer;
		Request.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
		{
			auto* Current = FindObject<UNiagaraScript>(nullptr, *ObjectPath);
			FinalWeak = Current;
			TArray<uint8> Disk;
			bFinal = Current && !Current->GetOutermost()->IsDirty() && !DirtyWeak.IsValid() && !DirtyPackageWeak.IsValid()
				&& !StagedWeak.IsValid() && !RemovedWeak.IsValid() && Current->ValidationRules.Num() == (bNativeName ? 3 : 1)
				&& External->HardReference == Current->ValidationRules[bNativeName ? 1 : 0]
				&& FFileHelper::LoadFileToArray(Disk, *Filename) && Disk != OriginalDisk
				&& (bNativeName ? MatchesNativeCreation(*Current) : SameScriptNativeStructure(Test, DesiredNative, *Current, TEXT("late deletion sees complete fresh persistent desired state")));
			Failure.Diagnostic.Code = bNativeName ? TEXT("t208_script_rule_creation_final_rejected") : TEXT("t208_script_rule_removal_final_rejected");
			Failure.Diagnostic.PublicMessage = TEXT("reject after persistent owned deletion readback");
			return false;
		};
		UeAgentNiagaraAuthoring::FAssetApplyResult Result;
		Test.TestFalse(TEXT("late structure rejection returns failure"), UeAgentNiagaraAuthoring::ApplyAsset(Request, Result, TEXT("t208-rule-removal-late")));
		Test.TestEqual(TEXT("structure rejection reaches the intended callback"), Result.Diagnostic.Code,
			FString(bNativeName ? TEXT("t208_script_rule_creation_final_rejected") : TEXT("t208_script_rule_removal_final_rejected")));
		Test.TestTrue(TEXT("dirty full checkpoint precedes structural application"), bCheckpoint);
		Test.TestTrue(TEXT("desired structure exists before original disk is published"), bStaged);
		Test.TestTrue(TEXT("rejection follows clean fresh published deletion and reference rebinding"), bFinal);
		Test.TestTrue(TEXT("late structural failure reports complete recovery"), Result.bRollbackAttempted && Result.bRollbackSucceeded);
		auto* Restored = FindObject<UNiagaraScript>(nullptr, *ObjectPath);
		if (!Test.TestNotNull(TEXT("the original dirty Script is restored before cleanup"), Restored)) return false;
		Test.TestFalse(TEXT("the rejected new persistent object is released"), FinalWeak.IsValid());
		Test.TestTrue(TEXT("restored Script retains its original dirty state"), Restored->GetOutermost()->IsDirty());
		SameScriptNativeStructure(Test, DirtyNative, *Restored, TEXT("rollback restores full deleted rule, order, owned state, graph and pins"), false);
		Test.TestTrue(TEXT("external reference returns to the original retained identity"), Restored->ValidationRules.Num() == 2
			&& External->HardReference == Restored->ValidationRules[1] && Restored->ValidationRules[1]->GetPathName(Restored) == RetainedPath);
		TArray<uint8> RestoredDisk;
		Test.TestTrue(TEXT("rollback independently restores different original disk bytes"), FFileHelper::LoadFileToArray(RestoredDisk, *Filename) && RestoredDisk == OriginalDisk);
		return !Test.HasAnyErrors();
	}
	TMap<FString, FString> ExpectedNative = CaptureScriptNativeStructure(*Script);
	const FString BaselineNoEventsPath = NoEvents->GetPathName(Script);
	const FString BaselineControlPath = ControlRule->GetPathName(Script);
	const FString SeverityKey = TEXT("owned/") + BaselineNoEventsPath + TEXT("/Severity");
	if (!Test.TestEqual(TEXT("the initial native rule severity is the fixed Warning baseline"),
		ExpectedNative.FindRef(SeverityKey), FString(TEXT("Warning")))) return false;
	// Package replacement rewrites serialized external references. A strong
	// pointer directly to the rule instead pins its old native identity.
	TStrongObjectPtr<UUeAgentT167AssetReferenceData> RetainedRuleReference(
		NewObject<UUeAgentT167AssetReferenceData>());
	if (RetainedRuleReference.IsValid()) RetainedRuleReference->HardReference = ControlRule;
	TStrongObjectPtr<UUeAgentT167AssetReferenceData> FirstRuleReference((bAddNew || bClear || !bRemoveFirst) ? NewObject<UUeAgentT167AssetReferenceData>() : nullptr);
	if (FirstRuleReference.IsValid()) FirstRuleReference->HardReference = NoEvents;
	const TWeakObjectPtr<UNiagaraValidationRule_NoEvents> RemovedRuleWeak(NoEvents);
	const TWeakObjectPtr<UNiagaraValidationRule> ControlRuleWeak(ControlRule);
	if (bRemoveFirst || bClear)
	{
		auto ExpectedRules = Script->ValidationRules;
		if (bClear) ExpectedRules.Empty(); else ExpectedRules.RemoveAt(0);
		FString ExpectedReferences;
		const auto* RulesProperty = FindFProperty<FArrayProperty>(Script->GetClass(), TEXT("ValidationRules"));
		RulesProperty->ExportTextItem_Direct(ExpectedReferences, &ExpectedRules, nullptr, Script, PPF_Delimited);
		ExpectedNative[TEXT("root/ValidationRules")] = ExpectedReferences;
		const FString RemovedPrefix = TEXT("owned/") + BaselineNoEventsPath + TEXT("/");
		for (auto It = ExpectedNative.CreateIterator(); It; ++It)
			if (It.Key().StartsWith(RemovedPrefix) || (bClear && It.Key().StartsWith(TEXT("owned/") + BaselineControlPath + TEXT("/")))) It.RemoveCurrent();
	}
	else if (!bAddNew) ExpectedNative[SeverityKey] = TEXT("Error");
	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / (TEXT("Automation/UAI/T208/NiagaraScriptRulesExport_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	const FString ReadbackPath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / (TEXT("Automation/UAI/T208/NiagaraScriptRulesReadback_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*WorkspacePath, false, true);
		IFileManager::Get().DeleteDirectory(*ReadbackPath, false, true);
	};

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!Test.TestTrue(TEXT("the production Niagara public surface composes"),
		TryCreateNiagaraPublicSurface(Registry, Dispatch, Error, bAddNew)))
	{
		Test.AddError(Error);
		return false;
	}
	FString ParamsJson;
	FString NewRuleName;
	if (bAddNew)
	{
		const auto QueryBefore = CaptureScriptNativeStructure(*Script, false);
		const bool bQueryDirtyBefore = Script->GetOutermost()->IsDirty();
		auto Query = MakeShared<FJsonObject>(); auto Input = MakeShared<FJsonObject>();
		Query->SetStringField(TEXT("target"), ObjectPath);
		Input->SetStringField(TEXT("property_path"), TEXT("ValidationRules"));
		Input->SetStringField(TEXT("query"), bNativeName ? TEXT("Emitter Count") : NoEvents->GetClass()->GetDisplayNameText().ToString()); Query->SetObjectField(TEXT("input"), Input);
		if (!CommandContract::Json::TryCanonicalize(Query, ParamsJson, Error)) { Test.AddError(Error); return false; }
		const auto Result = SubmitPublicCommand(Registry, Dispatch, TEXT("asset.owned_candidates"), ParamsJson);
		if (!Test.TestTrue(TEXT("public property-context query discovers native rule names"), Result.IsOk())) { Test.AddError(Result.GetErrorJson()); return false; }
		TSharedPtr<FJsonObject> Payload;
		if (!CommandContract::Json::TryParseObject(Result.GetPayloadJson(), Payload, Error)) { Test.AddError(Error); return false; }
		const auto& Names = Payload->GetArrayField(TEXT("names"));
		if (!Test.TestEqual(TEXT("focused public query returns one native name"), Names.Num(), 1)) return false;
		NewRuleName = Names[0]->AsString();
		Test.TestEqual(TEXT("the name comes from the native class display label"), NewRuleName, bNativeName ? UNiagaraValidationRule_EmitterCount::StaticClass()->GetDisplayNameText().ToString() : NoEvents->GetClass()->GetDisplayNameText().ToString());
		SameScriptNativeStructure(Test, QueryBefore, *Script, TEXT("candidate query preserves the complete original Script"), false);
		Test.TestTrue(TEXT("query retains original owned instances and dirty state"), Script->ValidationRules[0] == NoEvents
			&& Script->ValidationRules[1] == ControlRule && Script->GetOutermost()->IsDirty() == bQueryDirtyBefore);
		Test.TestTrue(TEXT("the selected native rule default is enabled in the current config"), GetDefault<UNiagaraValidationRule_EmitterCount>()->IsEnabled());
	}
	if (!Test.TestTrue(TEXT("the standalone Script export request canonicalizes"),
		TryBuildNiagaraExportParams(ObjectPath, WorkspacePath, ParamsJson, Error))
		|| !Test.TestTrue(TEXT("content_graph.export_workspace exports the standalone Script"),
			TrySubmitPublicCommand(Registry, Dispatch,
				TEXT("content_graph.export_workspace"), ParamsJson, Error))
		|| !Test.TestFalse(TEXT("the visible Script workspace contains no machine identity"),
			VisibleWorkspaceHasMachineIdentity(WorkspacePath)))
	{
		Test.AddError(Error);
		return false;
	}

	TSharedPtr<FJsonObject> ExpectedDefinitions;
	if ((bAddNew && !bRejectName && !bContextChanges) || bRemoveFirst || bClear)
	{
		// Fix desired schema before import from an independent native graph.
		// Emitter Count uses its initialized class, never the result under test.
		TStrongObjectPtr<UNiagaraScript> ExpectedScript(DuplicateObject<UNiagaraScript>(Script, GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UNiagaraScript::StaticClass(), TEXT("ExpectedRules"))));
		if (!ExpectedScript.IsValid()) return false;
		if (bClear) ExpectedScript->ValidationRules.Empty();
		else if (bRemoveFirst) ExpectedScript->ValidationRules.RemoveAt(0);
		else if (bNativeName) ExpectedScript->ValidationRules.Add(NewObject<UNiagaraValidationRule_EmitterCount>(ExpectedScript.Get()));
		else {
			auto* Added = DuplicateObject<UNiagaraValidationRule_NoEvents>(NoEvents, ExpectedScript.Get(),
				MakeUniqueObjectName(ExpectedScript.Get(), NoEvents->GetClass(), TEXT("ExpectedAdditionalRule")));
			Added->Severity = ENiagaraValidationSeverity::Error; ExpectedScript->ValidationRules.Add(Added);
		}
		if (!TryScriptDefinitionDocument(*ExpectedScript.Get(), ExpectedDefinitions, Error)) { Test.AddError(Error); return false; }
		FString DefinitionContent;
		if (!TryDefinitionSemantics(ExpectedDefinitions, DefinitionContent, Error)) { Test.AddError(Error); return false; }
	}
	TMap<FString, FString> ExpectedVisible =
		CaptureVisibleOwnerFiles(WorkspacePath, FString(), true);
	if (!bRemoveFirst && !bAddNew && !bClear)
	{
		const auto Before = CaptureScriptNativeStructure(*Script, false);
		const bool bDirtyBefore = Script->GetOutermost()->IsDirty();
		TArray<uint8> DiskBefore, DiskAfter;
		if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)
			|| !TryBuildNiagaraDirectoryImportParams(ObjectPath, WorkspacePath, ParamsJson, Error)) return false;
		int32 RealModifications = 0;
		const bool bWasModifiedThisFrame = FCoreUObjectDelegates::ObjectsModifiedThisFrame.Remove(Script) > 0;
		const auto ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object) { if (Object == Script) ++RealModifications; });
		ON_SCOPE_EXIT {
			FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);
			if (bWasModifiedThisFrame) FCoreUObjectDelegates::ObjectsModifiedThisFrame.Add(Script);
		};
		const auto Unchanged = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ParamsJson);
		if (!Test.TestTrue(TEXT("omitted ordinary class fields preserve the no-change outcome"), !Unchanged.IsOk()
			&& Unchanged.GetErrorJson().Contains(TEXT("hjson_import_no_explicit_changes")))) return false;
		int32 RejectedShapes = 0;
		for (const auto& File : ExpectedVisible)
		{
			for (bool bKeepInstance : {true, false})
			{
				TSharedPtr<FJsonObject> Document;
				if (!FUeAgentStructuredHjsonParser::ParseObject(File.Value, File.Key, Document, Error)) return false;
				bool bChanged = false;
				TFunction<void(const TSharedPtr<FJsonValue>&)> AddLegacySelector;
				AddLegacySelector = [&](const TSharedPtr<FJsonValue>& Value) {
					if (Value->Type != EJson::Object) return;
					for (const auto& Field : Value->AsObject()->Values)
					{
						if (!MatchesPropertyName(Field.Key, TEXT("ValidationRules"))) { AddLegacySelector(Field.Value); continue; }
						const auto Owned = Field.Value->AsObject()->GetArrayField(TEXT("value"))[0]->AsObject()->GetObjectField(TEXT("owned_object"));
						Test.TestFalse(TEXT("ordinary existing object exposes no class path"), Owned->HasField(TEXT("class_path")));
						Test.TestFalse(TEXT("ordinary existing object exposes no candidate name"), Owned->HasField(TEXT("name")));
						Owned->SetStringField(TEXT("class_path"), NoEvents->GetClass()->GetPathName());
						if (!bKeepInstance) Owned->RemoveField(TEXT("instance"));
						bChanged = true;
					}
				};
				AddLegacySelector(MakeShared<FJsonValueObject>(Document));
				if (!bChanged) continue;
				FString LegacyText;
				if (!CommandContract::Json::TryCanonicalize(Document.ToSharedRef(), LegacyText, Error)
					|| !FFileHelper::SaveStringToFile(LegacyText, *(WorkspacePath / File.Key), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
				const auto Rejected = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ParamsJson);
				Test.TestTrue(TEXT("ordinary class selection is rejected with and without an existing key"), !Rejected.IsOk()
					&& Rejected.GetErrorJson().Contains(TEXT("owned_item_visible_field_forbidden")));
				if (!FFileHelper::SaveStringToFile(File.Value, *(WorkspacePath / File.Key), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
				++RejectedShapes;
			}
		}
		Test.TestEqual(TEXT("both legacy selector shapes were exercised"), RejectedShapes, 2);
		Test.TestEqual(TEXT("no-change and legacy rejection perform no real Script Modify"), RealModifications, 0);
		SameScriptNativeStructure(Test, Before, *Script, TEXT("ordinary boundary preserves full native state"), false);
		Test.TestTrue(TEXT("ordinary boundary preserves actual identities, references and dirty state"),
			FindObject<UNiagaraScript>(nullptr, *ObjectPath) == Script && Script->ValidationRules[0] == NoEvents
			&& Script->ValidationRules[1] == ControlRule && FirstRuleReference->HardReference == NoEvents
			&& RetainedRuleReference->HardReference == ControlRule && Script->GetOutermost()->IsDirty() == bDirtyBefore);
		Test.TestTrue(TEXT("ordinary boundary does not publish"), FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
		if (Test.HasAnyErrors()) return false;
	}
	int32 ChangedLeaves = 0;
	for (TPair<FString, FString>& File : ExpectedVisible)
	{
		TSharedPtr<FJsonObject> Document;
		if (!FUeAgentStructuredHjsonParser::ParseObject(
			File.Value, File.Key, Document, Error))
		{
			Test.AddError(Error);
			return false;
		}
		int32 Changed = bRemoveFirst
			? RemoveFirstVisibleValidationRule(MakeShared<FJsonValueObject>(Document), Test, bSameClass)
			: ((bAddNew || bClear) ? 0 : SetVisibleStringLeaf(MakeShared<FJsonValueObject>(Document), TEXT("Severity"), TEXT("Warning"), TEXT("Error")));
		if (bClear)
		{
			TFunction<void(const TSharedPtr<FJsonValue>&)> ClearRules;
			ClearRules = [&](const TSharedPtr<FJsonValue>& Value) {
				if (Value->Type != EJson::Object) return;
				for (const auto& Field : Value->AsObject()->Values)
				{
					if (!MatchesPropertyName(Field.Key, TEXT("ValidationRules"))) { ClearRules(Field.Value); continue; }
					auto Row = Field.Value->AsObject();
					if (!Test.TestEqual(TEXT("explicit clear starts with both owned rules"), Row->GetArrayField(TEXT("value")).Num(), 2)) return;
					Row->SetArrayField(TEXT("value"), TArray<TSharedPtr<FJsonValue>>()); ++Changed;
				}
			};
			ClearRules(MakeShared<FJsonValueObject>(Document));
		}
		TSharedPtr<FJsonObject> AddedOwned;
		if (bAddNew)
		{
			TFunction<void(const TSharedPtr<FJsonValue>&)> AddRule;
			AddRule = [&](const TSharedPtr<FJsonValue>& Value) {
				if (Value->Type != EJson::Object) return;
				for (const auto& Field : Value->AsObject()->Values)
				{
					if (!MatchesPropertyName(Field.Key, TEXT("ValidationRules"))) { AddRule(Field.Value); continue; }
					auto Row = Field.Value->AsObject(); auto Items = Row->GetArrayField(TEXT("value"));
					if (!Test.TestEqual(TEXT("new instance starts from the two-rule ordinary array"), Items.Num(), 2)) return;
					FString NewText; TSharedPtr<FJsonObject> NewItem;
					if (bNativeName)
					{
						NewItem = MakeShared<FJsonObject>(); AddedOwned = MakeShared<FJsonObject>();
						AddedOwned->SetStringField(TEXT("name"), bRejectName ? TEXT("Missing Native Validation Rule") : NewRuleName);
						NewItem->SetObjectField(TEXT("owned_object"), AddedOwned);
						Items.Add(MakeShared<FJsonValueObject>(NewItem)); Row->SetArrayField(TEXT("value"), Items); ++Changed;
						continue;
					}
					if (!CommandContract::Json::TryCanonicalize(Items[0]->AsObject().ToSharedRef(), NewText, Error)
						|| !FUeAgentStructuredHjsonParser::ParseObject(NewText, TEXT("new-rule"), NewItem, Error)) return;
					AddedOwned = NewItem->GetObjectField(TEXT("owned_object"));
					AddedOwned->RemoveField(TEXT("instance"));
					AddedOwned->SetStringField(TEXT("name"), NewRuleName);
					if (!Test.TestEqual(TEXT("new ordinary rule has the predetermined non-default severity"),
						SetVisibleStringLeaf(MakeShared<FJsonValueObject>(NewItem), TEXT("Severity"), TEXT("Warning"), TEXT("Error")), 1)) return;
					Items.Add(MakeShared<FJsonValueObject>(NewItem)); Row->SetArrayField(TEXT("value"), Items); ++Changed;
				}
			};
			AddRule(MakeShared<FJsonValueObject>(Document));
		}
		if (Changed == 0) continue;
		ChangedLeaves += Changed;
		if (!CommandContract::Json::TryCanonicalize(
			Document.ToSharedRef(), File.Value, Error)
			|| !FFileHelper::SaveStringToFile(
				File.Value, *(WorkspacePath / File.Key),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Test.AddError(Error);
			return false;
		}
		if (bRemoveFirst && bSameClass)
		{
			// Input retains the original second key. A new export numbers the
			// remaining semantic occurrence from the beginning of its collection.
			const FString Name = ControlRule->GetClass()->GetDisplayNameText().ToString();
			if (!Test.TestEqual(TEXT("the re-export expectation renumbers only the retained semantic instance"),
				SetVisibleStringLeaf(MakeShared<FJsonValueObject>(Document), TEXT("instance"), Name + TEXT("2"), Name), 1)
				|| !CommandContract::Json::TryCanonicalize(Document.ToSharedRef(), File.Value, Error)) return false;
		}
		if (bAddNew && AddedOwned)
		{
			// New input has no instance key; only its subsequent ordinary export does.
			if (bNativeName)
			{
				AddedOwned->RemoveField(TEXT("name"));
				AddedOwned->SetStringField(TEXT("instance"), NewRuleName);
				auto Limits = MakeShared<FJsonObject>(); Limits->SetStringField(TEXT("type"), TEXT("NiagaraValidationRule_EmitterCountAndPlatformSet[]")); Limits->SetArrayField(TEXT("value"), {});
				auto Severity = MakeShared<FJsonObject>(); Severity->SetStringField(TEXT("type"), TEXT("ENiagaraValidationSeverity")); Severity->SetStringField(TEXT("value"), TEXT("Warning"));
				auto Validation = MakeShared<FJsonObject>(); Validation->SetObjectField(TEXT("EmitterCountLimits"), Limits); Validation->SetObjectField(TEXT("Severity"), Severity);
				auto Properties = MakeShared<FJsonObject>(); Properties->SetObjectField(TEXT("Validation"), Validation);
				AddedOwned->SetObjectField(TEXT("properties"), Properties);
			}
			else { AddedOwned->RemoveField(TEXT("name")); AddedOwned->SetStringField(TEXT("instance"), NoEvents->GetClass()->GetDisplayNameText().ToString() + TEXT("2")); }
			if (!CommandContract::Json::TryCanonicalize(Document.ToSharedRef(), File.Value, Error)) return false;
		}
	}
	if (!Test.TestEqual(TEXT("the ordinary workspace performs exactly one predetermined edit"),
		ChangedLeaves, 1))
	{
		return false;
	}
	if (!Test.TestTrue(TEXT("the standalone Script import request canonicalizes"),
		TryBuildNiagaraDirectoryImportParams(
			ObjectPath, WorkspacePath, ParamsJson, Error)))
	{
		Test.AddError(Error);
		return false;
	}
	const TWeakObjectPtr<UNiagaraScript> BeforeImportWeak(Script);
	if (bRejectName)
	{
		Script->MarkPackageDirty();
		const auto Before = CaptureScriptNativeStructure(*Script, false);
		TArray<uint8> DiskBefore, DiskAfter;
		if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
		const auto Rejected = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ParamsJson);
		Test.TestFalse(TEXT("an unknown native name is rejected before real creation"), Rejected.IsOk());
		Test.TestTrue(TEXT("rejection identifies the unresolved native candidate"), Rejected.GetErrorJson().Contains(TEXT("candidate_name_not_found")));
		Test.TestTrue(TEXT("write-before rejection retains the original Script object and dirty state"), BeforeImportWeak.IsValid()
			&& FindObject<UNiagaraScript>(nullptr, *ObjectPath) == Script && Script->GetOutermost()->IsDirty());
		SameScriptNativeStructure(Test, Before, *Script, TEXT("unknown name leaves full native owned and graph state unchanged"), false);
		Test.TestTrue(TEXT("unknown name preserves both original external references"), FirstRuleReference->HardReference == NoEvents
			&& RetainedRuleReference->HardReference == ControlRule);
		Test.TestTrue(TEXT("unknown name never publishes different disk contents"), FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
		return !Test.HasAnyErrors();
	}
	Script = nullptr;
	NoEvents = nullptr;
	ControlRule = nullptr;
	const CommandEdge::FUeAgentCommandResult ImportResult = SubmitPublicCommand(
		Registry, Dispatch, TEXT("content_graph.import_workspace"), ParamsJson);
	if (!Test.TestTrue(TEXT("the public import updates the owned validation rule"),
		ImportResult.IsOk()))
	{
		Test.AddError(ImportResult.GetErrorJson());
		return false;
	}
	if (!Test.TestFalse(TEXT("public persistent replacement releases the old standalone Script object"),
		BeforeImportWeak.IsValid()))
	{
		return false;
	}

	UNiagaraScript* Published = FindObject<UNiagaraScript>(nullptr, *ObjectPath);
	if (!Test.TestNotNull(TEXT("public import loads a fresh published Script"), Published))
	{
		return false;
	}
	UPackage* const PublishedPackage = Published->GetOutermost();
	Test.TestFalse(TEXT("the published Script package is clean"), PublishedPackage->IsDirty());
	const TWeakObjectPtr<UNiagaraScript> PublishedWeak(Published);
	const TWeakObjectPtr<UPackage> PublishedPackageWeak(PublishedPackage);
	Published = nullptr;
	TArray<FReloadPackageData> ReloadRequests{FReloadPackageData(PublishedPackage, LOAD_None)};
	TArray<UPackage*> ReloadedPackages;
	::ReloadPackages(ReloadRequests, ReloadedPackages);
	if (!Test.TestFalse(TEXT("external native replacement releases the prior Script"), PublishedWeak.IsValid())
		|| !Test.TestFalse(TEXT("external native replacement releases the prior package"), PublishedPackageWeak.IsValid())
		|| !Test.TestEqual(TEXT("external native replacement returns one persistent package"), ReloadedPackages.Num(), 1)
		|| !Test.TestNotNull(TEXT("the independent replacement package loads"), ReloadedPackages[0])) return false;
	UNiagaraScript* const Reloaded = LoadObject<UNiagaraScript>(nullptr, *ObjectPath);
	if (!Test.TestNotNull(TEXT("the persisted standalone Script resolves from the fresh load"), Reloaded)
		|| !Test.TestEqual(TEXT("the validation rule count is preserved"),
			Reloaded->ValidationRules.Num(), bClear ? 0 : (bRemoveFirst ? 1 : (bAddNew ? 3 : 2))))
	{
		return false;
	}
	if (bClear)
	{
		Test.TestFalse(TEXT("the first removed rule does not survive publication"), RemovedRuleWeak.IsValid());
		Test.TestFalse(TEXT("the second removed rule does not survive publication"), ControlRuleWeak.IsValid());
		Test.TestTrue(TEXT("external references to removed owned rules are cleared"), !FirstRuleReference->HardReference && !RetainedRuleReference->HardReference);
		Test.TestTrue(TEXT("clear is read from the independent clean persistent package"), Reloaded->GetOutermost() == ReloadedPackages[0] && !Reloaded->GetOutermost()->IsDirty());
		SameScriptNativeStructure(Test, ExpectedNative, *Reloaded, TEXT("clear preserves all non-rule Script, graph and pin content"));
		if (!TryBuildNiagaraExportParams(ObjectPath, ReadbackPath, ParamsJson, Error)
			|| !TrySubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), ParamsJson, Error)) { Test.AddError(Error); return false; }
		Test.TestTrue(TEXT("explicit empty array re-exports exactly without removed owned content"),
			CaptureVisibleOwnerFiles(ReadbackPath, FString(), true).OrderIndependentCompareEqual(ExpectedVisible));
		return !Test.HasAnyErrors();
	}
	if (bAddNew)
	{
		const auto* Added = Reloaded->ValidationRules[2].Get();
		if (!Test.TestTrue(TEXT("the requested concrete native rule is persisted"), Added && Added->GetClass() ==
			(bNativeName ? UNiagaraValidationRule_EmitterCount::StaticClass() : UNiagaraValidationRule_NoEvents::StaticClass()))) return false;
		Test.TestTrue(TEXT("both original references survive public and external package replacement"),
			FirstRuleReference->HardReference == Reloaded->ValidationRules[0] && RetainedRuleReference->HardReference == Reloaded->ValidationRules[1]);
		Test.TestTrue(TEXT("existing ordered identities remain exact"), Reloaded->ValidationRules[0]->GetPathName(Reloaded) == BaselineNoEventsPath
			&& Reloaded->ValidationRules[1]->GetPathName(Reloaded) == BaselineControlPath);
		Test.TestTrue(TEXT("new identity is distinct and owned by the new persistent Script"), Added->GetOuter() == Reloaded
			&& Added != Reloaded->ValidationRules[0] && Added != Reloaded->ValidationRules[1] && Reloaded->GetOutermost() == ReloadedPackages[0]);
		auto ActualNative = CaptureScriptNativeStructure(*Reloaded);
		TMap<FString, FString> ExpectedAdded, ActualAdded;
		const FString OldPrefix = TEXT("owned/") + BaselineNoEventsPath + TEXT("/");
		const FString NewPrefix = TEXT("owned/") + Added->GetPathName(Reloaded) + TEXT("/");
		for (const auto& Field : ExpectedNative) if (Field.Key.StartsWith(OldPrefix)) ExpectedAdded.Add(Field.Key.RightChop(OldPrefix.Len()), Field.Value);
		if (bNativeName)
		{
			ExpectedAdded = {{TEXT("@class"), UNiagaraValidationRule_EmitterCount::StaticClass()->GetPathName()},
				{TEXT("Severity"), TEXT("Warning")}, {TEXT("EmitterCountLimits"), TEXT("")}, {TEXT("bIsConfigDisabled"), TEXT("False")}};
			Test.TestTrue(TEXT("minimal input receives the native default severity and empty limits"),
				CastChecked<UNiagaraValidationRule_EmitterCount>(Added)->Severity == ENiagaraValidationSeverity::Warning
				&& CastChecked<UNiagaraValidationRule_EmitterCount>(Added)->EmitterCountLimits.IsEmpty());
		}
		else ExpectedAdded[TEXT("Severity")] = TEXT("Error");
		for (auto It = ActualNative.CreateIterator(); It; ++It) if (It.Key().StartsWith(NewPrefix)) { ActualAdded.Add(It.Key().RightChop(NewPrefix.Len()), It.Value()); It.RemoveCurrent(); }
		Test.TestTrue(bNativeName ? TEXT("every new rule field matches the native default specification")
			: TEXT("every new rule field matches the independent pre-edit same-class specification"), ActualAdded.OrderIndependentCompareEqual(ExpectedAdded));
		ActualNative.Remove(TEXT("root/ValidationRules")); ExpectedNative.Remove(TEXT("root/ValidationRules"));
		Test.TestTrue(TEXT("all other native root, owned, graph and pin content remains exact"), ActualNative.OrderIndependentCompareEqual(ExpectedNative));
		if (!TryBuildNiagaraExportParams(ObjectPath, ReadbackPath, ParamsJson, Error)
			|| !TrySubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), ParamsJson, Error)) { Test.AddError(Error); return false; }
		Test.TestTrue(TEXT("new instance re-export retains exact business files and complete resolved definitions"),
			SameScriptVisibleFiles(Test, ExpectedVisible, CaptureVisibleOwnerFiles(ReadbackPath, FString(), true), ExpectedDefinitions));
		return !Test.HasAnyErrors();
	}
	if (bRemoveFirst)
	{
		auto* Retained = Reloaded->ValidationRules[0].Get();
		if (!Test.TestNotNull(TEXT("the original second rule moves to index zero"), Retained)) return false;
		Test.TestTrue(TEXT("the retained instance keeps its exact native class"), Retained->GetClass() ==
			(bSameClass ? UNiagaraValidationRule_NoEvents::StaticClass() : UNiagaraValidationRule_NoWarmupTime::StaticClass()));
		Test.TestEqual(TEXT("the retained rule preserves its original owned path"), Retained->GetPathName(Reloaded), BaselineControlPath);
		Test.TestTrue(TEXT("the external reference follows the original retained rule through both reloads"), RetainedRuleReference->HardReference == Retained);
		Test.TestFalse(TEXT("the removed original rule no longer survives publication"), RemovedRuleWeak.IsValid());
		Test.TestTrue(TEXT("the retained rule belongs to the independently loaded Script"), Retained->GetOuter() == Reloaded);
		SameScriptNativeStructure(Test, ExpectedNative, *Reloaded, TEXT("removal preserves all remaining native Script content"));
		Test.TestFalse(TEXT("the independently loaded removal result is clean"), Reloaded->GetOutermost()->IsDirty());
		if (!TryBuildNiagaraExportParams(ObjectPath, ReadbackPath, ParamsJson, Error)
			|| !TrySubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), ParamsJson, Error))
		{ Test.AddError(Error); return false; }
		const auto ActualVisible = CaptureVisibleOwnerFiles(ReadbackPath, FString(), true);
		Test.TestTrue(TEXT("public re-export contains only the predetermined deletion and shift"), SameScriptVisibleFiles(Test, ExpectedVisible, ActualVisible, ExpectedDefinitions));
		return !Test.HasAnyErrors();
	}
	UNiagaraValidationRule_NoEvents* const ReloadedNoEvents =
		Reloaded->ValidationRules.IsValidIndex(0)
			? Cast<UNiagaraValidationRule_NoEvents>(Reloaded->ValidationRules[0])
			: nullptr;
	UNiagaraValidationRule_NoWarmupTime* const ReloadedControl =
		Reloaded->ValidationRules.IsValidIndex(1)
			? Cast<UNiagaraValidationRule_NoWarmupTime>(Reloaded->ValidationRules[1])
			: nullptr;
	if (!Test.TestNotNull(TEXT("the reloaded first rule retains NoEvents identity"),
		ReloadedNoEvents)
		|| !Test.TestNotNull(TEXT("the unedited rule retains its native class"),
			ReloadedControl))
	{
		return false;
	}
	Test.TestEqual(TEXT("only Severity changes through the public owned-property path"),
		ReloadedNoEvents->Severity, ENiagaraValidationSeverity::Error);
	Test.TestEqual(TEXT("the NoEvents owned path remains stable"),
		ReloadedNoEvents->GetPathName(Reloaded), BaselineNoEventsPath);
	Test.TestEqual(TEXT("the control rule owned path remains stable"),
		ReloadedControl->GetPathName(Reloaded), BaselineControlPath);
	Test.TestTrue(TEXT("both rules remain directly owned by the standalone Script"),
		ReloadedNoEvents->GetOuter() == Reloaded
			&& ReloadedControl->GetOuter() == Reloaded);
	Test.TestEqual(TEXT("the unedited Platforms struct remains exact"),
		ExportPlatforms(*ReloadedNoEvents), BaselinePlatforms);
	Test.TestTrue(TEXT("the Script belongs to the independent persistent package"),
		Reloaded->GetOutermost() == ReloadedPackages[0]);
	SameScriptNativeStructure(Test, ExpectedNative, *Reloaded,
		TEXT("all native Script fields, owned rules, graph and pins retain the exact expected content"));
	Test.TestFalse(TEXT("the independently reloaded Script package is clean"),
		Reloaded->GetOutermost()->IsDirty());

	if (!Test.TestTrue(TEXT("the persisted standalone Script independently re-exports"),
		TryBuildNiagaraExportParams(ObjectPath, ReadbackPath, ParamsJson, Error))
		|| !Test.TestTrue(TEXT("the public Script readback export succeeds"),
			TrySubmitPublicCommand(Registry, Dispatch,
				TEXT("content_graph.export_workspace"), ParamsJson, Error)))
	{
		Test.AddError(Error);
		return false;
	}
	Test.TestTrue(TEXT("the public readback differs only at the predetermined Severity leaf"),
		CaptureVisibleOwnerFiles(ReadbackPath, FString(), true)
			.OrderIndependentCompareEqual(ExpectedVisible));
	if (Test.HasAnyErrors()) return false;

	// Exercise the newly selected resident-package recovery path with three
	// distinct states: persisted Error, dirty Info, and published Warning.
	TArray<uint8> DiskBeforeFailure;
	if (!Test.TestTrue(TEXT("the original Script disk is captured before dirty edits"),
		FFileHelper::LoadFileToArray(DiskBeforeFailure, *Filename))) return false;
	Reloaded->Modify();
	ReloadedNoEvents->Severity = ENiagaraValidationSeverity::Info;
	ReloadedNoEvents->Platforms.QualityLevelMask = 3;
	auto* DirtySource = CastChecked<UNiagaraScriptSource>(Reloaded->GetLatestSource());
	if (!Test.TestTrue(TEXT("the dirty Script retains real authoring nodes"),
		DirtySource->NodeGraph && !DirtySource->NodeGraph->Nodes.IsEmpty())) return false;
	DirtySource->NodeGraph->Nodes[0]->NodeComment = TEXT("dirty Script content distinct from disk");
	Reloaded->MarkPackageDirty();
	const auto DirtyNative = CaptureScriptNativeStructure(*Reloaded, false);
	auto DesiredNative = CaptureScriptNativeStructure(*Reloaded);
	DesiredNative[SeverityKey] = TEXT("Warning");
	const TWeakObjectPtr<UNiagaraScript> DirtyWeak(Reloaded);
	const TWeakObjectPtr<UPackage> DirtyPackageWeak(Reloaded->GetOutermost());

	const auto SeverityDescriptor = MakeShared<FJsonObject>();
	SeverityDescriptor->SetStringField(TEXT("property_path"), TEXT("Severity"));
	SeverityDescriptor->SetStringField(TEXT("value"), TEXT("Warning"));
	const auto EditedOwned = MakeShared<FJsonObject>();
	EditedOwned->SetArrayField(TEXT("properties"), {MakeShared<FJsonValueObject>(SeverityDescriptor)});
	const auto EditedValue = MakeShared<FJsonObject>();
	EditedValue->SetObjectField(TEXT("owned_object"), EditedOwned);
	const auto ControlValue = MakeShared<FJsonObject>();
	ControlValue->SetObjectField(TEXT("owned_object"), MakeShared<FJsonObject>());
	const auto RulesDescriptor = MakeShared<FJsonObject>();
	RulesDescriptor->SetStringField(TEXT("property_path"), TEXT("ValidationRules"));
	RulesDescriptor->SetArrayField(TEXT("value"), {
		MakeShared<FJsonValueObject>(EditedValue), MakeShared<FJsonValueObject>(ControlValue)});
	UeAgentNiagaraAuthoring::FAssetApplyRequest FailedRequest;
	FailedRequest.AssetPath = PackageName;
	FailedRequest.AssetKind = TEXT("script");
	FailedRequest.bCompactValues = true;
	FailedRequest.Properties = {MakeShared<FJsonValueObject>(RulesDescriptor)};
	const auto Observer = MakeShared<FCreateTextStageObserver>();
	bool bObservedDirtyCheckpoint = false;
	bool bObservedDesiredBeforePublication = false;
	TWeakObjectPtr<UNiagaraScript> StagedWeak;
	Observer->BeforeStage = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure&)
	{
		auto* Current = FindObject<UNiagaraScript>(nullptr, *ObjectPath);
		TArray<uint8> Disk;
		const bool bOriginalDisk = FFileHelper::LoadFileToArray(Disk, *Filename) && Disk == DiskBeforeFailure;
		if (!Current || Current->ValidationRules.Num() != 2) return false;
		const auto* Rule = Cast<UNiagaraValidationRule_NoEvents>(Current->ValidationRules[0]);
		if (!Rule) return false;
		if (Rule->Severity == ENiagaraValidationSeverity::Info)
			bObservedDirtyCheckpoint = bOriginalDisk && Current->GetOutermost()->IsDirty()
				&& SameScriptNativeStructure(Test, DirtyNative, *Current, TEXT("checkpoint sees full dirty Script"), false);
		else if (Rule->Severity == ENiagaraValidationSeverity::Warning)
		{
			StagedWeak = Current;
			bObservedDesiredBeforePublication = bOriginalDisk
				&& SameScriptNativeStructure(Test, DesiredNative, *Current, TEXT("forward stage sees desired Script"));
		}
		return true;
	};
	FailedRequest.PackageStager = Observer;
	bool bReachedFreshPublished = false;
	TWeakObjectPtr<UNiagaraScript> FinalWeak;
	FailedRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
	{
		auto* Current = FindObject<UNiagaraScript>(nullptr, *ObjectPath);
		TArray<uint8> Disk;
		FinalWeak = Current;
		const auto* Rule = Current && Current->ValidationRules.Num() == 2
			? Cast<UNiagaraValidationRule_NoEvents>(Current->ValidationRules[0]) : nullptr;
		bReachedFreshPublished = Current && !DirtyWeak.IsValid() && !DirtyPackageWeak.IsValid()
			&& !StagedWeak.IsValid() && !Current->GetOutermost()->IsDirty()
			&& Current->ValidationRules.Num() == 2
			&& Rule && Rule->Severity == ENiagaraValidationSeverity::Warning
			&& FFileHelper::LoadFileToArray(Disk, *Filename) && Disk != DiskBeforeFailure
			&& SameScriptNativeStructure(Test, DesiredNative, *Current, TEXT("final sees clean persistent new Script"));
		Failure.Diagnostic.Code = TEXT("t208_script_rule_final_readback_rejected");
		Failure.Diagnostic.PublicMessage = TEXT("intentional rejection after native persistent package replacement");
		return false;
	};
	UeAgentNiagaraAuthoring::FAssetApplyResult FailedResult;
	Test.TestFalse(TEXT("late owned Script rejection returns failure"),
		UeAgentNiagaraAuthoring::ApplyAsset(FailedRequest, FailedResult, TEXT("t208-script-rule-late")));
	Test.TestEqual(TEXT("failure originates in the intended persistent callback"), FailedResult.Diagnostic.Code,
		FString(TEXT("t208_script_rule_final_readback_rejected")));
	Test.TestTrue(TEXT("checkpoint captures original dirty native contents and untouched disk"), bObservedDirtyCheckpoint);
	Test.TestTrue(TEXT("forward stage sees desired contents before disk publication"), bObservedDesiredBeforePublication);
	Test.TestTrue(TEXT("rejection follows complete desired fresh persistent readback"), bReachedFreshPublished);
	Test.TestTrue(TEXT("late Script rejection reports successful restoration"),
		FailedResult.bRollbackAttempted && FailedResult.bRollbackSucceeded);
	auto* Restored = FindObject<UNiagaraScript>(nullptr, *ObjectPath);
	if (Test.TestNotNull(TEXT("rollback publishes the restored Script before fixture cleanup"), Restored))
	{
		Test.TestFalse(TEXT("rollback releases the rejected persistent object"), FinalWeak.IsValid());
		Test.TestTrue(TEXT("rollback restores the dirty package flag"), Restored->GetOutermost()->IsDirty());
		SameScriptNativeStructure(Test, DirtyNative, *Restored,
			TEXT("rollback restores every dirty Script and owned leaf exactly"), false);
	}
	TArray<uint8> DiskAfterFailure;
	Test.TestTrue(TEXT("rollback restores the distinct original Script disk byte for byte"),
		FFileHelper::LoadFileToArray(DiskAfterFailure, *Filename) && DiskAfterFailure == DiskBeforeFailure);
	return !Test.HasAnyErrors();
}

} // namespace

bool FUeAgentT208NiagaraScriptValidationRulesOwnedPropertyVerticalTest::RunTest(const FString& Parameters)
{
	return RunScriptValidationRulesTest(*this, false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT208NiagaraScriptValidationRulesRemovalVerticalTest,
	"UAI.Root.T208.Niagara.ScriptValidationRulesRemovalVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208NiagaraScriptValidationRulesRemovalVerticalTest::RunTest(const FString& Parameters)
{
	return RunScriptValidationRulesTest(*this, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT208NiagaraScriptSameClassRuleRemovalVerticalTest,
	"UAI.Root.T208.Niagara.ScriptSameClassRuleRemovalVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208NiagaraScriptSameClassRuleRemovalVerticalTest::RunTest(const FString& Parameters)
{
	return RunScriptValidationRulesTest(*this, true, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT208NiagaraScriptRuleRemovalLateVerticalTest,
	"UAI.Root.T208.Niagara.ScriptRuleRemovalLateVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208NiagaraScriptRuleRemovalLateVerticalTest::RunTest(const FString& Parameters)
{
	return RunScriptValidationRulesTest(*this, true, false, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208NiagaraScriptRuleAdditionVerticalTest,
	"UAI.Root.T208.Niagara.ScriptRuleAdditionVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208NiagaraScriptRuleAdditionVerticalTest::RunTest(const FString& Parameters)
{
	return RunScriptValidationRulesTest(*this, false, false, false, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208NiagaraScriptRulesClearVerticalTest,
	"UAI.Root.T208.Niagara.ScriptRulesClearVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208NiagaraScriptRulesClearVerticalTest::RunTest(const FString& Parameters)
{
	return RunScriptValidationRulesTest(*this, false, false, false, false, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208NiagaraScriptRuleNativeNameCreationTest,
	"UAI.Root.T208.Niagara.ScriptRuleNativeNameCreation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208NiagaraScriptRuleNativeNameCreationTest::RunTest(const FString& Parameters)
{
	return RunScriptValidationRulesTest(*this, false, false, false, true, false, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208NiagaraScriptRuleNativeNameRejectionTest,
	"UAI.Root.T208.Niagara.ScriptRuleNativeNameRejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208NiagaraScriptRuleNativeNameRejectionTest::RunTest(const FString& Parameters)
{
	return RunScriptValidationRulesTest(*this, false, false, false, true, false, true, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208NiagaraScriptRuleNativeCreationLateTest,
	"UAI.Root.T208.Niagara.ScriptRuleNativeCreationLate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208NiagaraScriptRuleNativeCreationLateTest::RunTest(const FString& Parameters)
{
	return RunScriptValidationRulesTest(*this, false, false, true, true, false, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208NiagaraScriptRuleNativeContextRecheckTest,
	"UAI.Root.T208.Niagara.ScriptRuleNativeContextRecheck",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208NiagaraScriptRuleNativeContextRecheckTest::RunTest(const FString& Parameters)
{
	return RunScriptValidationRulesTest(*this, false, false, true, true, false, true, false, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208NiagaraOwnedNativePickerTest,
	"UAI.Root.T208.Niagara.OwnedNativePicker",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208NiagaraOwnedNativePickerTest::RunTest(const FString& Parameters)
{
	auto* Factory = NewObject<UNiagaraModuleScriptFactory>();
	TStrongObjectPtr<UNiagaraScript> Script(Cast<UNiagaraScript>(Factory->FactoryCreateNew(
		UNiagaraScript::StaticClass(), GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UNiagaraScript::StaticClass()),
		RF_Transient | RF_Transactional, nullptr, GWarn)));
	if (!TestNotNull(TEXT("native factory produces the component Script"), Script.Get())) return false;
	const auto* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
	if (!TestTrue(TEXT("the Script has a legitimate native source and graph"), Source && Source->NodeGraph)) return false;
	auto* Existing = NewObject<UNiagaraValidationRule_NoEvents>(Script.Get(), NAME_None, RF_Transactional);
	Existing->Severity = ENiagaraValidationSeverity::Error;
	Existing->Platforms.QualityLevelMask = 5;
	Script->ValidationRules = {Existing};
	const FString ExistingPath = Existing->GetPathName(Script.Get());
	const auto BeforeQuery = CaptureScriptNativeStructure(*Script, false);
	const bool bBeforeDirty = Script->GetOutermost()->IsDirty();
	TStrongObjectPtr<UNiagaraScript> Scratch(Cast<UNiagaraScript>(StaticDuplicateObject(Script.Get(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UNiagaraScript::StaticClass()))));
	if (!TestNotNull(TEXT("a full native owner copy provides prospective query context"), Scratch.Get())) return false;
	Scratch->ValidationRules.Add(nullptr);
	// UE5.6 SSingleProperty rebuilds array children although its public header
	// still says arrays are unsupported. This test establishes only this version.
	// No Script class-layout override is registered here.
	auto& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	auto ScratchView = PropertyEditor.CreateSingleProperty(Scratch.Get(), TEXT("ValidationRules"), FSinglePropertyParams());
	if (!TestTrue(TEXT("SingleProperty exposes the scratch array"), ScratchView.IsValid() && ScratchView->GetPropertyHandle().IsValid() && ScratchView->GetPropertyHandle()->AsArray().IsValid())) return false;
	auto ScratchElement = ScratchView->GetPropertyHandle()->AsArray()->GetElement(1);
	if (!TestTrue(TEXT("prospective slot exposes an initialized object handle"), ScratchElement->IsValidHandle() && CastField<FObjectPropertyBase>(ScratchElement->GetProperty()))) return false;
	auto ScratchPicker = StaticCastSharedRef<SClassViewer>(PropertyCustomizationHelpers::MakeEditInlineObjectClassPicker(ScratchElement, FOnClassPicked()));
	UClass* NewClass = UNiagaraValidationRule_EmitterCount::StaticClass();
	TestTrue(TEXT("the native picker allows the new concrete rule"), ScratchPicker->IsClassAllowed(NewClass));
	TestFalse(TEXT("the same native picker rejects an unrelated class"), ScratchPicker->IsClassAllowed(UNiagaraScript::StaticClass()));
	TestFalse(TEXT("the same native picker rejects the abstract rule base"), ScratchPicker->IsClassAllowed(UNiagaraValidationRule::StaticClass()));
	// Observe the same unloaded-class metadata sources as ClassViewer. The scan
	// runs after Editor initialization and never loads the Blueprint assets.
	auto& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	AssetRegistry.SearchAllAssets(true);
	AssetRegistry.WaitForCompletion();
	TestTrue(TEXT("the complete mounted registry search is finished"), AssetRegistry.IsSearchAllAssets() && !AssetRegistry.IsLoadingAssets());
	TSet<FTopLevelAssetPath> Derived;
	AssetRegistry.GetDerivedClassNames({UNiagaraValidationRule::StaticClass()->GetClassPathName()}, {}, Derived);
	TArray<FAssetData> Blueprints, GeneratedClasses;
	AssetRegistry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Blueprints, true);
	AssetRegistry.GetAssetsByClass(UBlueprintGeneratedClass::StaticClass()->GetClassPathName(), GeneratedClasses, true);
	TSet<FTopLevelAssetPath> MatchingBlueprintClasses;
	for (const auto& Asset : Blueprints)
	{
		const auto Path = FEditorClassUtils::GetClassPathNameFromAssetTag(Asset);
		if (Derived.Contains(Path)) MatchingBlueprintClasses.Add(Path);
	}
	for (const auto& Asset : GeneratedClasses)
	{
		const auto Path = Asset.GetSoftObjectPath().GetAssetPath();
		if (Derived.Contains(Path)) MatchingBlueprintClasses.Add(Path);
	}
	AddInfo(FString::Printf(TEXT("Native picker registry scope: Blueprint records=%d, generated-class records=%d, matching ValidationRule class paths=%d; no assets loaded by this observation"),
		Blueprints.Num(), GeneratedClasses.Num(), MatchingBlueprintClasses.Num()));
	TMap<UClass*, bool> ExpectedAllowed;
	for (TObjectIterator<UClass> It; It; ++It)
		if (It->IsChildOf(UNiagaraValidationRule::StaticClass())) ExpectedAllowed.Add(*It, ScratchPicker->IsClassAllowed(*It));
	for (const auto& Path : MatchingBlueprintClasses)
		if (!FindObject<UClass>(nullptr, *Path.ToString())) { AddError(TEXT("an unloaded relevant class requires a separately bounded load before completeness can be claimed")); return false; }
	SameScriptNativeStructure(*this, BeforeQuery, *Script, TEXT("prospective query leaves the complete original Script untouched"), false);
	TestTrue(TEXT("query preserves existing identity and dirty state without adding a real slot"), Script->ValidationRules.Num() == 1
		&& Script->ValidationRules[0] == Existing && Existing->GetPathName(Script.Get()) == ExistingPath && Script->GetOutermost()->IsDirty() == bBeforeDirty);
	Script->ValidationRules.Add(nullptr);
	auto RealView = PropertyEditor.CreateSingleProperty(Script.Get(), TEXT("ValidationRules"), FSinglePropertyParams());
	if (!TestTrue(TEXT("SingleProperty exposes the actual array without a class-layout override"), RealView.IsValid() && RealView->GetPropertyHandle().IsValid() && RealView->GetPropertyHandle()->AsArray().IsValid())) return false;
	auto Element = RealView->GetPropertyHandle()->AsArray()->GetElement(1);
	auto Picker = StaticCastSharedRef<SClassViewer>(PropertyCustomizationHelpers::MakeEditInlineObjectClassPicker(Element, FOnClassPicked()));
	for (const auto& Candidate : ExpectedAllowed)
		TestEqual(TEXT("every relevant class has the same native scratch and actual decision"), Picker->IsClassAllowed(Candidate.Key), Candidate.Value);
	AddInfo(FString::Printf(TEXT("Native prospective owner comparison: %d loaded relevant classes checked"), ExpectedAllowed.Num()));
	PropertyCustomizationHelpers::CreateNewInstanceOfEditInlineObjectClass(Element, NewClass);
	auto* Created = Cast<UNiagaraValidationRule_EmitterCount>(Script->ValidationRules[1]);
	if (!TestNotNull(TEXT("public native helper creates the selected rule"), Created)) return false;
	TestTrue(TEXT("native creation uses the real Script owner"), Created->GetOuter() == Script.Get());
	TestTrue(TEXT("new native fields equal the class default object"), Created->Severity == ENiagaraValidationSeverity::Warning && Created->EmitterCountLimits.IsEmpty());
	TestTrue(TEXT("native creation preserves the existing object and non-default content"), Script->ValidationRules[0] == Existing
		&& Existing->GetPathName(Script.Get()) == ExistingPath && Existing->Severity == ENiagaraValidationSeverity::Error && Existing->Platforms.QualityLevelMask == 5);
	return !HasAnyErrors();
}
