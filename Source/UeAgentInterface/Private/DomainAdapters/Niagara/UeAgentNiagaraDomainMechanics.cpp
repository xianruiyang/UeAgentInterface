#include "DomainAdapters/Niagara/UeAgentNiagaraDomainMechanics.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringApplyProvider.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringRegistration.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "QueryOwners/Niagara/UeAgentNiagaraModuleCandidateQuery.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::Niagara
{
	using namespace CommandContract;
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;

	namespace
	{
		const FString SchemaId = TEXT("uai.niagara.edit/1");
		const FString SystemType = TEXT("/Script/Niagara.NiagaraSystem");
		const FString EmitterType = TEXT("/Script/Niagara.NiagaraEmitter");
		const FString ScriptType = TEXT("/Script/Niagara.NiagaraScript");

		bool Fail(
			FWorkspaceError& OutError,
			FString Code,
			FString Source = FString(),
			FString Pointer = TEXT("/"))
		{
			OutError.Set(MoveTemp(Code), MoveTemp(Source), MoveTemp(Pointer));
			return false;
		}

		FString PackageIdentity(const FString& Target)
		{
			return Target.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Target)
				: Target;
		}

		FString ExpectedKind(const FString& RootType)
		{
			if (RootType == SystemType) return TEXT("system");
			if (RootType == EmitterType) return TEXT("emitter");
			if (RootType == ScriptType) return TEXT("script");
			return FString();
		}

		bool TryRead(
			const FString& Target,
			const FString& RootType,
			FString& OutPayload,
			FString& OutRevision,
			TSharedPtr<FJsonObject>* OutSnapshot,
			UObject** OutObject,
			FWorkspaceError& OutError)
		{
			UObject* Owner = nullptr;
			DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture Capture;
			if (!TryCaptureObject(
					Target, RootType, Owner, Capture, OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Snapshot;
			if (!TryBuildCaptureSnapshot(
					Target, RootType, Capture, Snapshot, OutError))
			{
				return false;
			}
			OutRevision = Capture.Revision;
			if (OutSnapshot) *OutSnapshot = Snapshot;
			if (OutObject) *OutObject = Owner;
			return TryCanonicalizeObject(Snapshot, OutPayload, OutError);
		}

		bool ValidateMissingTarget(
			const FString& Target,
			FWorkspaceError& OutError)
		{
			bool bPresent = false;
			if (!TryInspectObjectTargetPresence(Target, bPresent))
			{
				return Fail(
					OutError, TEXT("niagara_domain_target_invalid"), Target);
			}
			if (bPresent)
			{
				return Fail(
					OutError, TEXT("niagara_domain_stale_revision"),
					Target, TEXT("existing"));
			}
			return true;
		}

		bool CandidateObject(
			const TSharedPtr<FJsonObject>& Module,
			const FString& Pointer,
			TSharedPtr<FJsonObject>& OutCandidate,
			bool& bOutPresent,
			FWorkspaceError& OutError)
		{
			OutCandidate.Reset();
			bOutPresent = Module.IsValid()
				&& Module->HasField(TEXT("candidate"));
			if (!bOutPresent) return true;
			const TSharedPtr<FJsonObject>* Candidate = nullptr;
			if (!Module->TryGetObjectField(TEXT("candidate"), Candidate)
				|| !Candidate || !Candidate->IsValid())
			{
				return Fail(
					OutError, TEXT("domain_hjson_candidate_binding_invalid"),
					FString(), Pointer);
			}
			OutCandidate = *Candidate;
			return true;
		}

		bool NormalizeModuleCandidates(
			UObject& TargetObject,
			const TSharedPtr<FJsonObject>& ProviderExtension,
			FWorkspaceError& OutError)
		{
			if (!ProviderExtension.IsValid()
				|| !ProviderExtension->HasField(TEXT("stages")))
			{
				return true;
			}
			const TSharedPtr<FJsonObject>* Stages = nullptr;
			if (!ProviderExtension->TryGetObjectField(TEXT("stages"), Stages)
				|| !Stages || !Stages->IsValid())
			{
				return Fail(
					OutError, TEXT("niagara_domain_stages_invalid"),
					FString(), TEXT("/root/document/provider_extension/stages"));
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& StagePair
				: (*Stages)->Values)
			{
				const TSharedPtr<FJsonObject> Stage =
					StagePair.Value.IsValid()
						? StagePair.Value->AsObject() : nullptr;
				const TSharedPtr<FJsonObject>* StageTarget = nullptr;
				FString Usage;
				if (!Stage.IsValid()
					|| !Stage->TryGetObjectField(TEXT("target"), StageTarget)
					|| !StageTarget || !StageTarget->IsValid()
					|| !(*StageTarget)->TryGetStringField(TEXT("usage"), Usage)
					|| Usage.TrimStartAndEnd().IsEmpty())
				{
					return Fail(
						OutError, TEXT("niagara_domain_stage_target_invalid"),
						StagePair.Key);
				}
				if (!Stage->HasField(TEXT("modules"))) continue;
				const TSharedPtr<FJsonObject>* Modules = nullptr;
				if (!Stage->TryGetObjectField(TEXT("modules"), Modules)
					|| !Modules || !Modules->IsValid())
				{
					return Fail(
						OutError, TEXT("niagara_domain_modules_invalid"),
						StagePair.Key);
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& ModulePair
					: (*Modules)->Values)
				{
					const TSharedPtr<FJsonObject> Module =
						ModulePair.Value.IsValid()
							? ModulePair.Value->AsObject() : nullptr;
					const FString Pointer =
						TEXT("/root/document/provider_extension/stages/")
						+ StagePair.Key + TEXT("/modules/") + ModulePair.Key
						+ TEXT("/candidate");
					TSharedPtr<FJsonObject> Candidate;
					bool bPresent = false;
					if (!CandidateObject(
							Module, Pointer, Candidate, bPresent, OutError))
					{
						return false;
					}
					if (!bPresent) continue;
					bool bRemove = false;
					if (Module->TryGetBoolField(TEXT("remove"), bRemove)
						&& bRemove)
					{
						return Fail(
							OutError,
							TEXT("niagara_candidate_remove_conflict"),
							ModulePair.Key,
							Pointer);
					}
					FDomainHjsonCandidateBinding Binding;
					if (!TryReadCandidateBinding(
							Candidate, Pointer, Binding, OutError))
					{
						return false;
					}
					if (!Binding.SupportsOperation(TEXT("create")))
					{
						return Fail(
							OutError,
							TEXT("niagara_candidate_operation_unsupported"),
							Binding.Name,
							Pointer + TEXT("/supported_operations"));
					}
					QueryOwners::Niagara::FNiagaraModuleCandidateQueryError
						CandidateError;
					if (!QueryOwners::Niagara::
							IsCurrentNiagaraModuleCandidateBinding(
								TargetObject,
								Binding.Name,
								Binding.Kind,
								Binding.SupportedOperations,
								Binding.InputContract,
								Binding.PlacementContract,
								Binding.RequiredContext,
								Usage,
								Binding.BindingRevision,
								CandidateError))
					{
						return Fail(
							OutError,
							CandidateError.Code.IsEmpty()
								? FString(TEXT("niagara_candidate_binding_invalid"))
								: CandidateError.Code,
							CandidateError.SubjectId,
							CandidateError.Detail);
					}
					FString Existing;
					if (Module->TryGetStringField(
							TEXT("script_asset_path"), Existing)
						&& Existing != Binding.Name)
					{
						return Fail(
							OutError, TEXT("niagara_candidate_script_conflict"),
							Existing, Pointer);
					}
					if (Module->TryGetStringField(TEXT("module_kind"), Existing)
						&& !Existing.IsEmpty()
						&& !Existing.Equals(
							TEXT("script_module"), ESearchCase::IgnoreCase))
					{
						return Fail(
							OutError, TEXT("niagara_candidate_kind_conflict"),
							Existing, Pointer);
					}
					Module->SetStringField(
						TEXT("script_asset_path"), Binding.Name);
					Module->SetStringField(
						TEXT("module_kind"), TEXT("script_module"));
					Module->RemoveField(TEXT("candidate"));
				}
			}
			return true;
		}

		bool PrepareDocument(
			const FString& Target,
			const FString& RootType,
			const bool bExists,
			UObject* ExistingTarget,
			const TSharedPtr<FJsonObject>& Source,
			TSharedPtr<FJsonObject>& OutDocument,
			FWorkspaceError& OutError)
		{
			OutDocument.Reset();
			if (!Source.IsValid())
			{
				return Fail(
					OutError, TEXT("niagara_domain_document_required"));
			}
			OutDocument = MakeShared<FJsonObject>(*Source);
			FString Operation;
			const TSharedPtr<FJsonObject>* SourceTarget = nullptr;
			FString AssetPath;
			if (!OutDocument->TryGetStringField(TEXT("operation"), Operation)
				|| !OutDocument->TryGetObjectField(
					TEXT("target"), SourceTarget)
				|| !SourceTarget || !SourceTarget->IsValid()
				|| !(*SourceTarget)->TryGetStringField(
					TEXT("asset_path"), AssetPath))
			{
				return Fail(
					OutError, TEXT("niagara_domain_document_identity_required"));
			}
			Operation.TrimStartAndEndInline();
			Operation.ToLowerInline();
			const bool bOperationValid = bExists
				? Operation == TEXT("update")
				: (Operation == TEXT("create") || Operation == TEXT("clone"));
			if (!bOperationValid)
			{
				return Fail(
					OutError, TEXT("niagara_domain_operation_revision_conflict"),
					Operation,
					bExists ? TEXT("update") : TEXT("create|clone"));
			}
			if (!PackageIdentity(AssetPath).Equals(
					PackageIdentity(Target), ESearchCase::IgnoreCase))
			{
				return Fail(
					OutError, TEXT("niagara_domain_target_identity_mismatch"),
					AssetPath, Target);
			}
			const FString Kind = ExpectedKind(RootType);
			FString DeclaredKind;
			if ((*SourceTarget)->TryGetStringField(
					TEXT("asset_kind"), DeclaredKind)
				&& !DeclaredKind.Equals(Kind, ESearchCase::IgnoreCase))
			{
				return Fail(
					OutError, TEXT("niagara_domain_asset_kind_mismatch"),
					DeclaredKind, Kind);
			}
			const TSharedPtr<FJsonObject> TargetObject =
				MakeShared<FJsonObject>(**SourceTarget);
			TargetObject->SetStringField(
				TEXT("asset_path"), PackageIdentity(Target));
			TargetObject->SetStringField(TEXT("asset_kind"), Kind);
			OutDocument->SetObjectField(TEXT("target"), TargetObject);

			TSharedPtr<FJsonObject> Extension = MakeShared<FJsonObject>();
			const TSharedPtr<FJsonObject>* SourceExtension = nullptr;
			if (OutDocument->HasField(TEXT("provider_extension")))
			{
				if (!OutDocument->TryGetObjectField(
						TEXT("provider_extension"), SourceExtension)
					|| !SourceExtension || !SourceExtension->IsValid())
				{
					return Fail(
						OutError,
						TEXT("niagara_domain_provider_extension_invalid"));
				}
				Extension = MakeShared<FJsonObject>(**SourceExtension);
			}
			bool Flag = false;
			if ((Extension->TryGetBoolField(TEXT("compile"), Flag) && !Flag)
				|| (Extension->TryGetBoolField(TEXT("save"), Flag) && !Flag))
			{
				return Fail(
					OutError,
					TEXT("niagara_domain_durable_lifecycle_required"));
			}
			Extension->SetBoolField(TEXT("compile"), true);
			Extension->SetBoolField(TEXT("save"), true);
			if (bExists)
			{
				if (!ExistingTarget
					|| !NormalizeModuleCandidates(
						*ExistingTarget, Extension, OutError))
				{
					return false;
				}
			}
			else if (Extension->HasField(TEXT("stages")))
			{
				UClass* const RootClass = LoadObject<UClass>(nullptr, *RootType);
				TStrongObjectPtr<UObject> CandidateContext(
					RootClass
						? NewObject<UObject>(
							GetTransientPackage(), RootClass, NAME_None,
							RF_Transient)
						: nullptr);
				if (!CandidateContext.IsValid()
					|| !NormalizeModuleCandidates(
						*CandidateContext, Extension, OutError))
				{
					if (OutError.GetCode().IsEmpty())
					{
						return Fail(
							OutError,
							TEXT("niagara_candidate_context_create_failed"),
							RootType);
					}
					return false;
				}
			}
			OutDocument->SetObjectField(TEXT("provider_extension"), Extension);
			return true;
		}

		bool TryImport(
			const FString& Target,
			const FString& RootType,
			const FString& CanonicalParamsJson,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			const FString Kind = ExpectedKind(RootType);
			if (Kind.IsEmpty())
			{
				return Fail(
					OutError, TEXT("niagara_domain_root_type_invalid"), RootType);
			}
			FResolvedDomainHjsonDocument Resolved;
			if (!TryDecodeResolvedDocument(
					Target,
					CanonicalParamsJson,
					FDomainHjsonDocumentContract(
						TEXT("niagara"), SchemaId, RootType),
					Resolved,
					OutError))
			{
				return false;
			}
			const bool bExists = Resolved.ExpectedRevision != TEXT("missing");
			UObject* ExistingTarget = nullptr;
			if (bExists)
			{
				FString BeforePayload;
				FString CurrentRevision;
				if (!TryRead(
						Target, RootType, BeforePayload, CurrentRevision,
						nullptr, &ExistingTarget, OutError))
				{
					return false;
				}
				if (Resolved.ExpectedRevision != CurrentRevision)
				{
					return Fail(
						OutError, TEXT("niagara_domain_stale_revision"),
						Target, CurrentRevision);
				}
			}
			else if (!ValidateMissingTarget(Target, OutError))
			{
				return false;
			}

			const TSharedPtr<FJsonObject>* DocumentField = nullptr;
			if (!Resolved.Root->TryGetObjectField(
					TEXT("document"), DocumentField)
				|| !DocumentField || !DocumentField->IsValid())
			{
				return Fail(
					OutError, TEXT("niagara_domain_document_required"));
			}
			TSharedPtr<FJsonObject> Document;
			if (!PrepareDocument(
					Target, RootType, bExists, ExistingTarget,
					*DocumentField, Document, OutError))
			{
				return false;
			}

			FString RegistrationError;
			if (!UeAgentNiagaraAuthoring::RegisterAuthoring(
					RegistrationError))
			{
				return Fail(
					OutError,
					TEXT("niagara_domain_authoring_registration_failed"),
					RegistrationError);
			}
			UeAgentNiagaraAuthoring::FAssetApplyResult ApplyResult;
			const FString RequestId = TEXT("niagara-domain-")
				+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			if (!UeAgentNiagaraAuthoring::ApplyAssetInlineDocument(
					Document,
					Kind,
					PackageIdentity(Target),
					ApplyResult,
					RequestId))
			{
				return Fail(
					OutError,
					ApplyResult.ErrorCode.IsEmpty()
						? FString(TEXT("niagara_domain_authoring_failed"))
						: ApplyResult.ErrorCode,
					ApplyResult.Phase,
					ApplyResult.ErrorDetail);
			}
			if (!ApplyResult.CanReportComplete()
				|| !ApplyResult.Asset
				|| !PackageIdentity(ApplyResult.AssetPath).Equals(
					PackageIdentity(Target), ESearchCase::IgnoreCase))
			{
				return Fail(
					OutError,
					TEXT("niagara_domain_authoring_receipt_invalid"),
					ApplyResult.AssetPath,
					UeAgentNiagaraAuthoring::GetApplyFailureMessage(ApplyResult));
			}

			UPackage* const Package = ApplyResult.Asset->GetOutermost();
			ApplyResult.Asset = nullptr;
			FText UnloadError;
			if (!Package
				|| !UPackageTools::UnloadPackages({Package}, UnloadError, true))
			{
				return Fail(
					OutError, TEXT("niagara_domain_unload_failed"),
					Target, UnloadError.ToString());
			}

			FString AfterPayload;
			FString AfterRevision;
			TSharedPtr<FJsonObject> AfterSnapshot;
			if (!TryRead(
					Target, RootType, AfterPayload, AfterRevision,
					&AfterSnapshot, nullptr, OutError))
			{
				return false;
			}
			FString CompileStatus;
			if (!AfterSnapshot.IsValid()
				|| !AfterSnapshot->TryGetStringField(
					TEXT("compile_status"), CompileStatus)
				|| CompileStatus != TEXT("valid"))
			{
				return Fail(
					OutError, TEXT("niagara_domain_compile_not_settled"),
					Target, CompileStatus);
			}

			const TSharedPtr<FJsonObject> Mutation = MakeShared<FJsonObject>();
			Mutation->SetStringField(
				TEXT("operation"), ApplyResult.ResolvedOperation);
			Mutation->SetStringField(TEXT("asset_kind"), ApplyResult.AssetKind);
			Mutation->SetStringField(
				TEXT("compile_status"), ApplyResult.CompileStatus);
			Mutation->SetNumberField(
				TEXT("compile_error_count"), ApplyResult.CompileErrorCount);
			Mutation->SetNumberField(
				TEXT("stack_issue_count"), ApplyResult.StackIssueCount);
			Mutation->SetNumberField(
				TEXT("mutations_applied"), ApplyResult.MutationsApplied);
			Mutation->SetBoolField(
				TEXT("editor_isolation_performed"),
				ApplyResult.bEditorIsolationPerformed);
			Mutation->SetNumberField(
				TEXT("closed_editor_count"), ApplyResult.ClosedEditorCount);
			TArray<TSharedPtr<FJsonValue>> CompiledTargets;
			for (const FString& CompiledTarget : ApplyResult.CompiledTargets)
			{
				CompiledTargets.Add(
					MakeShared<FJsonValueString>(CompiledTarget));
			}
			Mutation->SetArrayField(
				TEXT("compiled_targets"), MoveTemp(CompiledTargets));

			FDomainHjsonImportLifecycleReceipt LifecycleReceipt;
			LifecycleReceipt.bMutationApplied = true;
			LifecycleReceipt.bCompileSettled = true;
			LifecycleReceipt.bPackageSaved = true;
			LifecycleReceipt.bUnloadedReloaded = true;
			LifecycleReceipt.PersistenceReceiptId = TEXT("reload:")
				+ ObjectPathFromPackageIdentity(Target)
				+ TEXT(":") + AfterRevision;
			return TryBuildImportSuccessPayload(
				TEXT("niagara-domain-provider"),
				Target,
				AfterRevision,
				LifecycleReceipt,
				AfterSnapshot,
				Mutation,
				OutPayload,
				OutError);
		}
	}

	bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& RootTypeIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		FWorkspaceError& OutError)
	{
		OutPayloadJson.Reset();
		bOutReadOnly = false;
		OutError.Reset();
		TSharedPtr<FJsonObject> Params;
		FString JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			return Fail(
				OutError, TEXT("niagara_domain_params_invalid"), JsonError);
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| ObjectPathFromPackageIdentity(Target)
				!= ObjectPathFromPackageIdentity(TargetIdentity))
		{
			return Fail(
				OutError, TEXT("niagara_domain_target_identity_mismatch"),
				TargetIdentity, Target);
		}
		if (ExpectedKind(RootTypeIdentity).IsEmpty())
		{
			return Fail(
				OutError, TEXT("niagara_domain_root_type_invalid"),
				RootTypeIdentity);
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(
				Target, RootTypeIdentity, CanonicalParamsJson,
				OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			return Fail(
				OutError, TEXT("niagara_domain_export_not_registered"), Target);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			FString Revision;
			bOutReadOnly = true;
			return TryRead(
				Target, RootTypeIdentity, OutPayloadJson, Revision,
				nullptr, nullptr, OutError);
		}
		return Fail(
			OutError, TEXT("niagara_domain_command_not_supported"),
			CommandId);
	}
}
