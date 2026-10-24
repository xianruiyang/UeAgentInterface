#include "DomainAdapters/Niagara/UeAgentNiagaraDomainMechanics.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/ContentDiscovery/UeAgentReadOnlyCandidateBatch.h"

#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringApplyProvider.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringDescriptors.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringExport.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringRegistration.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringStateAccessors.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "NiagaraEmitter.h"
#include "PackageTools.h"
#include "QueryOwners/Niagara/UeAgentNiagaraModuleCandidateQuery.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentNativeNormalizationIntent.h"

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

		bool Fail(FWorkspaceError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic,
            const FString& Source = FString(), const FString& Pointer = TEXT("/"))
        {
            OutError.SetFailure(MoveTemp(Diagnostic), Source, Pointer);
            return false;
        }



		FString NiagaraPointer(const FString& Parent, const FString& Field)
		{
			return Parent + TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}

		TSharedPtr<FJsonValue> NiagaraField(const TSharedPtr<FJsonObject>& Object, const FString& Field)
		{
			const TSharedPtr<FJsonValue>* Value = Object.IsValid() ? Object->Values.Find(Field) : nullptr;
			return Value ? *Value : nullptr;
		}

		TSharedPtr<FJsonValue> NiagaraJsonFact(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid()) return MakeShared<FJsonValueNull>();
			if (Value->Type != EJson::Object && Value->Type != EJson::Array) return Value;
			const TSharedRef<FJsonObject> Shape = MakeShared<FJsonObject>();
			Shape->SetStringField(TEXT("kind"), Value->Type == EJson::Object ? TEXT("object") : TEXT("array"));
			Shape->SetNumberField(TEXT("count"), Value->Type == EJson::Object ? Value->AsObject()->Values.Num() : Value->AsArray().Num());
			return MakeShared<FJsonValueObject>(Shape);
		}

		UE::UAI::Contract::FDiagnostic NiagaraFact(
			const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Target = FString(), const FString& Kind = TEXT("authored_value"))
		{
			auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Path,
				MoveTemp(Expected), MoveTemp(Actual), TEXT("authoring_apply"), Reason);
			Error.TargetPath = Target;
			Error.SourceKind = Kind;
			if (Kind == TEXT("native_result")) { Error.Source.JsonPath.Reset(); Error.SemanticPointer.Reset(); }
			return Error;
		}

		UE::UAI::Contract::FDiagnostic NiagaraNativeFact(
			const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target = FString())
		{
			return NiagaraFact(Code, Reason, Path, MoveTemp(Actual), MoveTemp(Expected), Target, TEXT("native_result"));
		}

		UE::UAI::Contract::FDiagnostic NiagaraFieldFact(
			const FString& Code, const FString& Reason, const FString& Path,
			const TSharedPtr<FJsonObject>& Owner, const FString& Field,
			TSharedPtr<FJsonValue> Expected, const FString& Target = FString())
		{
			auto Error = NiagaraFact(Code, Reason, Path, NiagaraJsonFact(NiagaraField(Owner, Field)), MoveTemp(Expected), Target);
			if (Owner.IsValid()) UeAgentAuthoringDiagnostics::FScopedSourceLocations::Locate(*Owner, Field, Error.Source);
			return Error;
		}

		bool TryCompareNiagaraProjectionValues(
			const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual, const FString& Path,
			StateSemantic::CanonicalJson::FValueComparison& OutDifference, UE::UAI::Contract::FDiagnostic& OutError)
		{
			using namespace StateSemantic::CanonicalJson;
			OutError = UE::UAI::Contract::FDiagnostic();
			const FDeclaredValueComparison NumberText = [&](const TSharedPtr<FJsonValue>& Left,
				const TSharedPtr<FJsonValue>& Right, const FString& NumberPath) -> TOptional<FValueComparison>
			{
				if (Left->Type != EJson::Number || Right->Type != EJson::Number) return {};
				FString Before, After;
				if (!Left->TryGetString(Before) || !Right->TryGetString(After))
				{
					OutError = NiagaraFact(TEXT("niagara_semantic_value_invalid"),
						TEXT("A number in the Niagara projection cannot expose its canonical JSON text."), NumberPath,
						MakeShared<FJsonValueString>(TEXT("number without canonical text")),
						MakeShared<FJsonValueString>(TEXT("serializable JSON number")));
					return FValueComparison::Different(FString(), Left, Right);
				}
				// Preserve the existing canonical-text predicate without serializing containers.
				return Before == After ? FValueComparison() : FValueComparison::Different(FString(), Left, Right);
			};
			OutDifference = CompareJsonValuesWithCase(Expected, Actual, ESearchCase::IgnoreCase, Path, &NumberText);
			return !OutError.IsValid();
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
				return Fail(OutError, NiagaraNativeFact(TEXT("niagara_domain_target_invalid"),
					TEXT("The Niagara target could not be inspected as an Unreal asset identity."), TEXT("/target/inspection_succeeded"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target));
			}
			if (bPresent)
			{
				return Fail(OutError, NiagaraNativeFact(TEXT("niagara_domain_stale_revision"),
					TEXT("The Niagara target already exists, but the document expects a missing target."), TEXT("/target/exists"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Target));
			}
			return true;
		}

		bool CandidateObject(
			const TSharedPtr<FJsonObject>& Module,
			const FString& Pointer,
			TSharedPtr<FJsonObject>& OutCandidate,
			bool& bOutPresent,
			FWorkspaceError& OutError,
			const FString& Target)
		{
			OutCandidate.Reset();
			bOutPresent = Module.IsValid()
				&& Module->HasField(TEXT("candidate"));
			if (!bOutPresent) return true;
			const TSharedPtr<FJsonObject>* Candidate = nullptr;
			if (!Module->TryGetObjectField(TEXT("candidate"), Candidate)
				|| !Candidate || !Candidate->IsValid())
			{
				return Fail(OutError, NiagaraFieldFact(TEXT("domain_hjson_candidate_binding_invalid"),
					TEXT("The module candidate selector must be an object."), Pointer, Module, TEXT("candidate"),
					MakeShared<FJsonValueString>(TEXT("object")), Target));
			}
			OutCandidate = *Candidate;
			return true;
		}

		bool NormalizeModuleCandidates(
			UObject& TargetObject,
			const TSharedPtr<FJsonObject>& ProviderExtension,
			FWorkspaceError& OutError,
			const FString& Target)
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
				return Fail(OutError, NiagaraFieldFact(TEXT("niagara_domain_stages_invalid"),
					TEXT("Niagara stages must be a keyed object."), TEXT("/root/document/provider_extension/stages"),
					ProviderExtension, TEXT("stages"), MakeShared<FJsonValueString>(TEXT("object")), Target));
			}
			ContentDiscovery::FReadOnlyCandidateBatch CandidateBatch;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& StagePair
				: (*Stages)->Values)
			{
				const FString StagePointer = NiagaraPointer(TEXT("/root/document/provider_extension/stages"), StagePair.Key);
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
					if (!Stage.IsValid())
						return Fail(OutError, NiagaraFieldFact(TEXT("niagara_domain_stage_target_invalid"),
							TEXT("A Niagara stage must be an object."), StagePointer, *Stages, StagePair.Key,
							MakeShared<FJsonValueString>(TEXT("object")), Target));
					if (!StageTarget || !StageTarget->IsValid())
						return Fail(OutError, NiagaraFieldFact(TEXT("niagara_domain_stage_target_invalid"),
							TEXT("A Niagara stage requires a target object."), StagePointer + TEXT("/target"), Stage, TEXT("target"),
							MakeShared<FJsonValueString>(TEXT("object")), Target));
					return Fail(OutError, NiagaraFieldFact(TEXT("niagara_domain_stage_target_invalid"),
						TEXT("The Niagara stage target requires a non-empty usage string."), StagePointer + TEXT("/target/usage"),
						*StageTarget, TEXT("usage"), MakeShared<FJsonValueString>(TEXT("non-empty string")), Target));
				}
				QueryOwners::Niagara::FNiagaraModuleCandidateContext
					CandidateContext;
				CandidateContext.TargetUsage = Usage;

				CandidateContext.SourceKind.Reset();
				CandidateContext.Source.JsonPath = StagePointer + TEXT("/target");
				CandidateContext.TargetUsageSource.JsonPath = StagePointer + TEXT("/target/usage");
				UeAgentAuthoringDiagnostics::FScopedSourceLocations::Locate(**StageTarget, FString(), CandidateContext.Source);
				UeAgentAuthoringDiagnostics::FScopedSourceLocations::Locate(**StageTarget, TEXT("usage"), CandidateContext.TargetUsageSource);
				(*StageTarget)->TryGetStringField(
					TEXT("emitter_name"), CandidateContext.EmitterName);
				(*StageTarget)->TryGetStringField(
					TEXT("stage_name"), CandidateContext.StageName);
				if (!Stage->HasField(TEXT("modules"))) continue;
				const TSharedPtr<FJsonObject>* Modules = nullptr;
				if (!Stage->TryGetObjectField(TEXT("modules"), Modules)
					|| !Modules || !Modules->IsValid())
				{
					return Fail(OutError, NiagaraFieldFact(TEXT("niagara_domain_modules_invalid"),
						TEXT("Niagara stage modules must be a keyed object."), StagePointer + TEXT("/modules"),
						Stage, TEXT("modules"), MakeShared<FJsonValueString>(TEXT("object")), Target));
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& ModulePair
					: (*Modules)->Values)
				{
					const TSharedPtr<FJsonObject> Module =
						ModulePair.Value.IsValid()
							? ModulePair.Value->AsObject() : nullptr;
					const FString ModulePointer = NiagaraPointer(StagePointer + TEXT("/modules"), ModulePair.Key);
					const FString Pointer = ModulePointer + TEXT("/candidate");
					TSharedPtr<FJsonObject> Candidate;
					bool bPresent = false;
					if (!CandidateObject(
							Module, Pointer, Candidate, bPresent, OutError, Target))
					{
						return false;
					}
					if (!bPresent) continue;
					bool bRemove = false;
					if (Module->TryGetBoolField(TEXT("remove"), bRemove)
						&& bRemove)
					{
						return Fail(OutError, NiagaraFieldFact(TEXT("niagara_candidate_remove_conflict"),
							TEXT("A candidate cannot be resolved for a module marked for removal."), ModulePointer + TEXT("/remove"),
							Module, TEXT("remove"), MakeShared<FJsonValueBoolean>(false), Target));
					}
					FDomainHjsonCandidateSelector Selector;
					if (!TryReadCandidateSelector(
							Candidate, Pointer, Selector, OutError))
					{
						return false;
					}
					if (Selector.Name.IsEmpty())
					{
						return Fail(OutError, NiagaraFieldFact(TEXT("niagara_candidate_name_required"),
							TEXT("The Niagara module candidate requires a non-empty name."), Pointer + TEXT("/name"),
							Candidate, TEXT("name"), MakeShared<FJsonValueString>(TEXT("non-empty candidate name")), Target));
					}
					FString ActionKindAssertion;
					if (Candidate->TryGetStringField(
							TEXT("action_kind"), ActionKindAssertion)
						&& !ActionKindAssertion.IsEmpty()
						&& !ActionKindAssertion.Equals(
							TEXT("asset_module"), ESearchCase::CaseSensitive))
					{
						return Fail(OutError, NiagaraFieldFact(TEXT("niagara_candidate_action_kind_conflict"),
							TEXT("The Niagara module candidate action kind must be asset_module."), Pointer + TEXT("/action_kind"),
							Candidate, TEXT("action_kind"), MakeShared<FJsonValueString>(TEXT("asset_module")), Target));
					}
					QueryOwners::Niagara::FNiagaraModuleCandidate
						ResolvedCandidate;
					QueryOwners::Niagara::FNiagaraModuleCandidateContext
						ResolvedContext;
					QueryOwners::Niagara::FNiagaraModuleCandidateQueryError
						CandidateError;
					if (!QueryOwners::Niagara::
							TryResolveNiagaraModuleCandidate(
								TargetObject,
								Selector.Name,
								CandidateContext,
								ResolvedCandidate,
								ResolvedContext,
								CandidateError, &CandidateBatch, Selector.Select))
					{
						OutError.SetFailure(CandidateError, FString(), Pointer);
						return false;
					}
					FString Existing;
					if (Module->TryGetStringField(
							TEXT("script_asset_path"), Existing)
						&& Existing != ResolvedCandidate.ScriptAssetPath)
					{
						return Fail(OutError, NiagaraFieldFact(TEXT("niagara_candidate_script_conflict"),
							TEXT("The module script path does not match the resolved candidate."), ModulePointer + TEXT("/script_asset_path"),
							Module, TEXT("script_asset_path"), MakeShared<FJsonValueString>(ResolvedCandidate.ScriptAssetPath), Target));
					}
					FString CandidatePathAssertion;
					if (Candidate->TryGetStringField(
							TEXT("script_asset_path"),
							CandidatePathAssertion)
						&& !CandidatePathAssertion.IsEmpty()
						&& CandidatePathAssertion
							!= ResolvedCandidate.ScriptAssetPath)
					{
						return Fail(OutError, NiagaraFieldFact(TEXT("niagara_candidate_script_conflict"),
							TEXT("The candidate script assertion does not match the resolved script."), Pointer + TEXT("/script_asset_path"),
							Candidate, TEXT("script_asset_path"), MakeShared<FJsonValueString>(ResolvedCandidate.ScriptAssetPath), Target));
					}
					if (Module->TryGetStringField(TEXT("module_kind"), Existing)
						&& !Existing.IsEmpty()
						&& !Existing.Equals(
							TEXT("script_module"), ESearchCase::IgnoreCase))
					{
						return Fail(OutError, NiagaraFieldFact(TEXT("niagara_candidate_kind_conflict"),
							TEXT("A resolved Niagara asset module must have module_kind script_module."), ModulePointer + TEXT("/module_kind"),
							Module, TEXT("module_kind"), MakeShared<FJsonValueString>(TEXT("script_module")), Target));
					}
					Module->SetStringField(
						TEXT("script_asset_path"),
						ResolvedCandidate.ScriptAssetPath);
					Module->SetStringField(
						TEXT("module_kind"), TEXT("script_module"));
					const TSharedPtr<FJsonObject> CanonicalCandidate =
						MakeShared<FJsonObject>();
					CanonicalCandidate->SetStringField(
						TEXT("name"), ResolvedCandidate.Name);
					Module->SetObjectField(
						TEXT("candidate"), CanonicalCandidate);
					(*StageTarget)->SetStringField(
						TEXT("usage"), ResolvedContext.TargetUsage);
					(*StageTarget)->SetStringField(
						TEXT("stage_name"), ResolvedContext.StageName);
				if (!ResolvedContext.EmitterName.IsEmpty())
				{
					(*StageTarget)->SetStringField(
						TEXT("emitter_name"),
						ResolvedContext.EmitterName);
					}
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
				return Fail(OutError, NiagaraFact(TEXT("niagara_domain_document_required"),
					TEXT("The Niagara edit document must be an object."), TEXT("/document"),
					MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("object")), Target));
			}
			OutDocument = MakeShared<FJsonObject>(*Source);
			FString Operation;
			const TSharedPtr<FJsonObject>* SourceTarget = nullptr;
			FString AssetPath;
			if (!OutDocument->TryGetStringField(TEXT("operation"), Operation))
				return Fail(OutError, NiagaraFieldFact(TEXT("niagara_domain_document_identity_required"),
					TEXT("The Niagara edit operation must be a string."), TEXT("/document/operation"),
					Source, TEXT("operation"), MakeShared<FJsonValueString>(TEXT("string")), Target));
			if (!OutDocument->TryGetObjectField(TEXT("target"), SourceTarget) || !SourceTarget || !SourceTarget->IsValid())
				return Fail(OutError, NiagaraFieldFact(TEXT("niagara_domain_document_identity_required"),
					TEXT("The Niagara edit target must be an object."), TEXT("/document/target"),
					Source, TEXT("target"), MakeShared<FJsonValueString>(TEXT("object")), Target));
			if (!(*SourceTarget)->TryGetStringField(TEXT("asset_path"), AssetPath))
				return Fail(OutError, NiagaraFieldFact(TEXT("niagara_domain_document_identity_required"),
					TEXT("The Niagara edit target must declare a string asset path."), TEXT("/document/target/asset_path"),
					*SourceTarget, TEXT("asset_path"), MakeShared<FJsonValueString>(TEXT("string asset path")), Target));
			Operation.TrimStartAndEndInline();
			Operation.ToLowerInline();
			const bool bOperationValid = bExists
				? Operation == TEXT("update")
				: (Operation == TEXT("create") || Operation == TEXT("clone"));
			if (!bOperationValid)
			{
				return Fail(OutError, NiagaraFieldFact(TEXT("niagara_domain_operation_revision_conflict"),
					TEXT("The edit operation does not match whether the target asset exists."), TEXT("/document/operation"),
					Source, TEXT("operation"), MakeShared<FJsonValueString>(bExists ? TEXT("update") : TEXT("create|clone")), Target));
			}
			if (!PackageIdentity(AssetPath).Equals(
					PackageIdentity(Target), ESearchCase::IgnoreCase))
			{
				return Fail(OutError, NiagaraFieldFact(TEXT("niagara_domain_target_identity_mismatch"),
					TEXT("The document asset path does not identify the requested Niagara target."), TEXT("/document/target/asset_path"),
					*SourceTarget, TEXT("asset_path"), MakeShared<FJsonValueString>(PackageIdentity(Target)), Target));
			}
			const FString Kind = ExpectedKind(RootType);
			FString DeclaredKind;
			if ((*SourceTarget)->TryGetStringField(
					TEXT("asset_kind"), DeclaredKind)
				&& !DeclaredKind.Equals(Kind, ESearchCase::IgnoreCase))
			{
				return Fail(OutError, NiagaraFieldFact(TEXT("niagara_domain_asset_kind_mismatch"),
					TEXT("The declared asset kind does not match the target Niagara class."), TEXT("/document/target/asset_kind"),
					*SourceTarget, TEXT("asset_kind"), MakeShared<FJsonValueString>(Kind), Target));
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
					return Fail(OutError, NiagaraFieldFact(TEXT("niagara_domain_provider_extension_invalid"),
						TEXT("The Niagara provider extension must be an object."), TEXT("/document/provider_extension"),
						Source, TEXT("provider_extension"), MakeShared<FJsonValueString>(TEXT("object")), Target));
				}
				Extension = MakeShared<FJsonObject>(**SourceExtension);
			}
			for (const TCHAR* Field : {TEXT("compile"), TEXT("save")})
			{
				bool Flag = false;
				if (Extension->TryGetBoolField(Field, Flag) && !Flag)
					return Fail(OutError, NiagaraFieldFact(TEXT("niagara_domain_durable_lifecycle_required"),
						TEXT("Niagara workspace imports require compilation and saving."), NiagaraPointer(TEXT("/document/provider_extension"), Field),
						Extension, Field, MakeShared<FJsonValueBoolean>(true), Target));
			}
			Extension->SetBoolField(TEXT("compile"), true);
			Extension->SetBoolField(TEXT("save"), true);
			if (bExists)
			{
				if (!ExistingTarget)
					return Fail(OutError, NiagaraNativeFact(TEXT("niagara_domain_target_invalid"),
						TEXT("The existing Niagara target is unavailable for module candidate resolution."), TEXT("/target/available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target));
				if (!NormalizeModuleCandidates(*ExistingTarget, Extension, OutError, Target)) return false;
			}
			else if (Extension->HasField(TEXT("stages")))
			{
				UClass* const RootClass = LoadObject<UClass>(nullptr, *RootType);
				if (!RootClass)
					return Fail(OutError, NiagaraNativeFact(TEXT("niagara_candidate_context_create_failed"),
						TEXT("The Niagara root class could not be loaded for candidate resolution."), TEXT("/root_type"),
						MakeShared<FJsonValueString>(RootType), MakeShared<FJsonValueString>(TEXT("loadable Niagara class")), Target));
				TStrongObjectPtr<UObject> CandidateContext(NewObject<UObject>(
					GetTransientPackage(), RootClass, NAME_None, RF_Transient));
				if (!CandidateContext.IsValid())
					return Fail(OutError, NiagaraNativeFact(TEXT("niagara_candidate_context_create_failed"),
						TEXT("A temporary Niagara object could not be allocated for candidate resolution."), TEXT("/candidate_context/available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target));
				if (!NormalizeModuleCandidates(*CandidateContext, Extension, OutError, Target)) return false;
			}
			OutDocument->SetObjectField(TEXT("provider_extension"), Extension);
			return true;
		}

		bool TryProjectSemanticStages(
			const FAuthoringModelRef& DesiredModel,
			const FAuthoringModelRef& CurrentModel,
			const TArray<UeAgentAuthoringState::FReadRequest>& CapturedRequests,
			const UObject* RootObject,
			const Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection,
			TSharedPtr<FJsonObject>& OutStages,
			TSharedPtr<FJsonObject>& OutEmitters,
			TSharedPtr<FJsonObject>& OutUserParameters,
			FWorkspaceError& OutError,
			const FResolvedSemanticWorkspace& Workspace)
		{
			using namespace UeAgentNiagaraAuthoring;
			TSet<FString> ChangedStages;
			TArray<const FSemanticUnit*> ChangedDynamicInputs;
			OutEmitters = MakeShared<FJsonObject>();
			OutUserParameters.Reset();
			const FString AssetTarget = RootObject ? RootObject->GetPathName() : FString();
			auto LocateFailure = [&](FWorkspaceError& Destination, UE::UAI::Contract::FDiagnostic Error,
				const FString& UnitId, const FString& Pointer)
			{
				if (Error.TargetPath.IsEmpty()) Error.TargetPath = AssetTarget;
				const FString Leaf = !Error.Source.JsonPath.IsEmpty() ? Error.Source.JsonPath
					: (!Error.SemanticPointer.IsEmpty() ? Error.SemanticPointer : Pointer);
				Fail(Destination, MoveTemp(Error), UnitId, Pointer);
				if (const auto* Source = Workspace.FindValueSource(UnitId, Leaf)) Destination.Locate(*Source);
				return false;
			};
			auto SemanticFail = [&](UE::UAI::Contract::FDiagnostic Error, const FString& UnitId, const FString& Pointer = FString())
			{
				return LocateFailure(OutError, MoveTemp(Error), UnitId, Pointer);
			};
			auto DifferenceDiagnostic = [&](const TCHAR* Code, const TCHAR* Reason,
				const StateSemantic::CanonicalJson::FValueComparison& Difference)
			{
				auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
				Difference.ApplyTo(Error);
				Error.PropertyPath = Difference.Path;
				Error.SemanticPointer = Difference.Path;
				Error.SourceKind = TEXT("authored_value");
				Error.TargetPath = AssetTarget;
				return Error;
			};
			auto CompareValues = [&](const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual,
				const FString& UnitId, const FString& Pointer, StateSemantic::CanonicalJson::FValueComparison& Difference)
			{
				UE::UAI::Contract::FDiagnostic Error;
				return TryCompareNiagaraProjectionValues(Expected, Actual, Pointer, Difference, Error)
					|| SemanticFail(MoveTemp(Error), UnitId, Pointer);
			};
			auto BindingFailure = [&](const TCHAR* Code, const TCHAR* Reason, const FString& Pointer,
				const bool bCapturedEntry, const FSemanticUnit* Bound, const FString& ExpectedType,
				const bool bCapturedRequest, const bool bOwnerMatches = true)
			{
				const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
				const TSharedRef<FJsonObject> Expected = MakeShared<FJsonObject>();
				Actual->SetBoolField(TEXT("captured_entry"), bCapturedEntry);
				Actual->SetBoolField(TEXT("bound_unit"), Bound != nullptr);
				Actual->SetStringField(TEXT("semantic_type"), Bound ? Bound->GetSemanticTypeRef() : FString());
				Actual->SetBoolField(TEXT("captured_request"), bCapturedRequest);
				Actual->SetBoolField(TEXT("owner_matches"), bOwnerMatches);
				Expected->SetBoolField(TEXT("captured_entry"), true);
				Expected->SetBoolField(TEXT("bound_unit"), true);
				Expected->SetStringField(TEXT("semantic_type"), ExpectedType);
				Expected->SetBoolField(TEXT("captured_request"), true);
				Expected->SetBoolField(TEXT("owner_matches"), true);
				return NiagaraFact(Code, Reason, Pointer, MakeShared<FJsonValueObject>(Actual),
					MakeShared<FJsonValueObject>(Expected), AssetTarget);
			};
			auto ProjectValues = [&](const FSemanticUnit& Unit, TSharedRef<FJsonObject> Object)
			{
				for (const auto& Field : Unit.GetValues())
				{
					TSharedPtr<FJsonValue> Value;
					UE::UAI::Contract::FDiagnostic Error;
					if (!StateSemantic::CanonicalJson::TryToJsonValue(*Field.Value, Value, Error))
					{
						const FString FieldPointer = NiagaraPointer(FString(), Field.Key);
						if (Error.IsValid()) UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, FieldPointer, false, true);
						else Error = NiagaraFact(TEXT("niagara_semantic_value_invalid"),
							TEXT("The semantic field could not be projected to a JSON value."), FieldPointer,
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), AssetTarget);
						return SemanticFail(MoveTemp(Error), Unit.GetUnitId(), FieldPointer);
					}
					Object->SetField(Field.Key, MoveTemp(Value));
				}
				return true;
			};
			auto FindCaptured = [&](const FSemanticUnit& Unit)
			{
				return CapturedRequests.FindByPredicate([&](const UeAgentAuthoringState::FReadRequest& Request)
				{
					return Request.SemanticId == Unit.GetSemanticTypeRef()
						&& Request.TargetIdentity.CanonicalKey == Unit.GetTargetIdentityKey();
				});
			};
			for (const FString& UnitId : Projection.ChangedUnitIds)
			{
				const FSemanticUnit* Unit = DesiredModel->FindUnit(UnitId);
				if (!Unit) Unit = CurrentModel->FindUnit(UnitId);
				if (!Unit) return SemanticFail(NiagaraFact(TEXT("niagara_semantic_unit_missing"),
					TEXT("The changed semantic unit is absent from both the desired and captured models."), TEXT("/unit/available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), UnitId);
				if (Unit->GetRole() == TEXT("emitter_hierarchy")
					&& Unit->GetUnitKind() == ESemanticUnitKind::StructureState
					&& Unit->GetValues().Contains(TEXT("emitters")))
				{
					const FSemanticUnit* BeforeUnit = CurrentModel->FindUnit(UnitId);
					const TSharedRef<FJsonObject> Desired = MakeShared<FJsonObject>();
					const TSharedRef<FJsonObject> Before = MakeShared<FJsonObject>();
					if (!BeforeUnit)
						return SemanticFail(NiagaraFact(TEXT("niagara_semantic_emitter_collection_missing"),
							TEXT("The captured emitter collection unit is unavailable."), TEXT("/emitters"),
							MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("captured emitter collection"))), UnitId);
					if (!ProjectValues(*Unit, Desired) || !ProjectValues(*BeforeUnit, Before)) return false;
					const TSharedPtr<FJsonObject>* DesiredItems = nullptr;
					const TSharedPtr<FJsonObject>* BeforeItems = nullptr;
					if (!Desired->TryGetObjectField(TEXT("emitters"), DesiredItems))
						return SemanticFail(NiagaraFieldFact(TEXT("niagara_semantic_emitter_collection_missing"),
							TEXT("The desired emitter collection must be an object."), TEXT("/emitters"),
							Desired, TEXT("emitters"), MakeShared<FJsonValueString>(TEXT("object"))), UnitId);
					if (!Before->TryGetObjectField(TEXT("emitters"), BeforeItems))
						return SemanticFail(NiagaraFieldFact(TEXT("niagara_semantic_emitter_collection_missing"),
							TEXT("The captured emitter collection must be an object."), TEXT("/emitters"),
							Before, TEXT("emitters"), MakeShared<FJsonValueString>(TEXT("object"))), UnitId);
					FWorkspaceError EmitterErrors;
					for (const auto& Entry : (*DesiredItems)->Values)
					{
						FWorkspaceError EmitterError;
						auto ProjectEmitter = [&]() -> bool
						{
							const FString Pointer = NiagaraPointer(TEXT("/emitters"), Entry.Key);
							auto EmitterFail = [&](UE::UAI::Contract::FDiagnostic Error, const FString& Field = FString())
							{
								return LocateFailure(EmitterError, MoveTemp(Error), UnitId, Pointer + Field);
							};
							if (!Entry.Value.IsValid() || Entry.Value->Type != EJson::Object)
								return EmitterFail(NiagaraFact(TEXT("niagara_semantic_emitter_invalid"),
									TEXT("The emitter collection item must be an object."), Pointer,
									NiagaraJsonFact(Entry.Value), MakeShared<FJsonValueString>(TEXT("object"))));
							const TSharedPtr<FJsonObject> Row = Entry.Value->AsObject();
							const TSharedPtr<FJsonObject>* Captured = nullptr;
							(*BeforeItems)->TryGetObjectField(Entry.Key, Captured);
							FString Operation;
							if (!Row->TryGetStringField(TEXT("operation"), Operation))
								return EmitterFail(NiagaraFieldFact(TEXT("niagara_semantic_emitter_operation_missing"),
									TEXT("The emitter item must declare a string operation."), Pointer + TEXT("/operation"),
									Row, TEXT("operation"), MakeShared<FJsonValueString>(TEXT("create|update|remove"))));
							if (Captured)
							{
								StateSemantic::CanonicalJson::FValueComparison Difference;
								UE::UAI::Contract::FDiagnostic CompareError;
								if (!TryCompareNiagaraProjectionValues(MakeShared<FJsonValueObject>(*Captured),
									MakeShared<FJsonValueObject>(Row), Pointer, Difference, CompareError))
									return EmitterFail(MoveTemp(CompareError));
								if (Difference.bEquivalent) return true;
							}
							const TSharedRef<FJsonObject> Owner = MakeShared<FJsonObject>();
							const TSharedRef<FJsonObject> OwnerTarget = MakeShared<FJsonObject>();
							FString OwnerKey = Entry.Key;
							if (Operation == TEXT("create"))
							{
								FString SourcePath;
								if (Captured)
									return EmitterFail(NiagaraFieldFact(TEXT("niagara_semantic_emitter_source_required"),
										TEXT("An existing emitter cannot use the create operation."), Pointer + TEXT("/operation"),
										Row, TEXT("operation"), MakeShared<FJsonValueString>(TEXT("update|remove"))));
								if (!Row->TryGetStringField(TEXT("source_asset_path"), SourcePath) || SourcePath.IsEmpty())
									return EmitterFail(NiagaraFieldFact(TEXT("niagara_semantic_emitter_source_required"),
										TEXT("A new emitter requires a non-empty source asset path."), Pointer + TEXT("/source_asset_path"),
										Row, TEXT("source_asset_path"), MakeShared<FJsonValueString>(TEXT("non-empty source asset path"))));
								OwnerTarget->SetStringField(TEXT("emitter_name"), Entry.Key);
							}
							else if (Operation == TEXT("update") || Operation == TEXT("remove"))
							{
								FString BoundId;
								Row->TryGetStringField(TEXT("unit_id"), BoundId);
								const FSemanticUnit* Bound = CurrentModel->FindUnit(BoundId);
								const auto* CapturedOwner = Bound ? FindCaptured(*Bound) : nullptr;
								if (!Captured || !Bound || Bound->GetSemanticTypeRef() != EmitterOwnerSemanticId || !CapturedOwner)
									return EmitterFail(BindingFailure(TEXT("niagara_semantic_emitter_binding_missing"),
									TEXT("The emitter item cannot be bound to a captured emitter owner."), Pointer,
									Captured != nullptr, Bound, EmitterOwnerSemanticId, CapturedOwner != nullptr));
								OwnerKey = CapturedOwner->TargetIdentity.GetSelector(TEXT("emitter_owner_id"));
								for (const TCHAR* Selector : {TEXT("emitter_name"), TEXT("emitter_handle_id")})
								{
									const FString Value = CapturedOwner->TargetIdentity.GetSelector(Selector);
									if (!Value.IsEmpty()) OwnerTarget->SetStringField(Selector, Value);
								}
								if (Operation == TEXT("remove")) Owner->SetBoolField(TEXT("remove"), true);
							}
							else return EmitterFail(NiagaraFieldFact(TEXT("niagara_semantic_emitter_operation_invalid"),
								TEXT("The emitter operation is unsupported."), Pointer + TEXT("/operation"),
								Row, TEXT("operation"), MakeShared<FJsonValueString>(TEXT("create|update|remove"))));
							Owner->SetObjectField(TEXT("target"), OwnerTarget);
							if (Operation != TEXT("remove"))
							{
								// Reusing an unchanged source binding while editing enabled must
								// not trigger a parent merge or replace local emitter overrides.
								bool bSyncSource = Operation == TEXT("create") || Row->HasField(TEXT("source_sync_mode"));
								for (const TCHAR* Field : {TEXT("source_asset_path"), TEXT("emitter_version")})
								{
									const TSharedPtr<FJsonValue>* Value = Row->Values.Find(Field);
									const TSharedPtr<FJsonValue>* Previous = Captured ? (*Captured)->Values.Find(Field) : nullptr;
									if (Value)
									{
										if (!Previous) bSyncSource = true;
										else
										{
											StateSemantic::CanonicalJson::FValueComparison Difference;
											UE::UAI::Contract::FDiagnostic CompareError;
											if (!TryCompareNiagaraProjectionValues(*Previous, *Value, NiagaraPointer(Pointer, Field), Difference, CompareError))
												return EmitterFail(MoveTemp(CompareError));
											if (!Difference.bEquivalent) bSyncSource = true;
										}
									}
								}
								for (const TCHAR* Field : {TEXT("enabled"), TEXT("source_asset_path"), TEXT("source_sync_mode"), TEXT("emitter_version")})
								{
									const TSharedPtr<FJsonValue>* Value = Row->Values.Find(Field);
									if (Value && (FString(Field) == TEXT("enabled") || bSyncSource)) Owner->SetField(Field, *Value);
								}
							}
							Workspace.AttachInlinePropertySources(UnitId, Pointer, *Owner);
							const TSharedPtr<FJsonObject>* ExistingPayload = nullptr;
							if (OutEmitters->TryGetObjectField(OwnerKey, ExistingPayload) && ExistingPayload)
							{
								for (const auto& Field : Owner->Values) (*ExistingPayload)->SetField(Field.Key, Field.Value);
							}
							else OutEmitters->SetObjectField(OwnerKey, Owner);
							return true;
						};
						if (!ProjectEmitter()) EmitterErrors.Append(EmitterError);
					}
					if (EmitterErrors.HasErrors()) { OutError = MoveTemp(EmitterErrors); return false; }
				}
				else if (Unit->GetRole() == TEXT("user_parameters")
					&& Unit->GetUnitKind() == ESemanticUnitKind::StructureState)
				{
					const FSemanticUnit* BeforeUnit = CurrentModel->FindUnit(UnitId);
					const TSharedRef<FJsonObject> Desired = MakeShared<FJsonObject>();
					const TSharedRef<FJsonObject> Before = MakeShared<FJsonObject>();
					if (!BeforeUnit)
						return SemanticFail(NiagaraFact(TEXT("niagara_semantic_user_parameter_collection_missing"),
							TEXT("The captured user parameter collection unit is unavailable."), TEXT("/user_parameters"),
							MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("captured user parameter collection"))), UnitId);
					if (!ProjectValues(*Unit, Desired) || !ProjectValues(*BeforeUnit, Before)) return false;
					const TSharedPtr<FJsonObject>* DesiredItems = nullptr;
					const TSharedPtr<FJsonObject>* BeforeItems = nullptr;
					if (!Desired->TryGetObjectField(TEXT("user_parameters"), DesiredItems))
						return SemanticFail(NiagaraFieldFact(TEXT("niagara_semantic_user_parameter_collection_missing"),
							TEXT("The desired user parameter collection must be an object."), TEXT("/user_parameters"),
							Desired, TEXT("user_parameters"), MakeShared<FJsonValueString>(TEXT("object"))), UnitId);
					if (!Before->TryGetObjectField(TEXT("user_parameters"), BeforeItems))
						return SemanticFail(NiagaraFieldFact(TEXT("niagara_semantic_user_parameter_collection_missing"),
							TEXT("The captured user parameter collection must be an object."), TEXT("/user_parameters"),
							Before, TEXT("user_parameters"), MakeShared<FJsonValueString>(TEXT("object"))), UnitId);
					for (const auto& Entry : (*DesiredItems)->Values)
					{
						const FString Pointer = NiagaraPointer(TEXT("/user_parameters"), Entry.Key);
						if (!Entry.Value.IsValid() || Entry.Value->Type != EJson::Object)
							return SemanticFail(NiagaraFact(TEXT("niagara_semantic_user_parameter_invalid"),
								TEXT("The user parameter item must be an object."), Pointer,
								NiagaraJsonFact(Entry.Value), MakeShared<FJsonValueString>(TEXT("object"))), UnitId);
						const TSharedPtr<FJsonObject> Row = Entry.Value->AsObject();
						const TSharedPtr<FJsonObject>* Captured = nullptr;
						(*BeforeItems)->TryGetObjectField(Entry.Key, Captured);
						FString Operation;
						if (!Row->TryGetStringField(TEXT("operation"), Operation))
							return SemanticFail(NiagaraFieldFact(TEXT("niagara_semantic_user_parameter_operation_missing"),
								TEXT("The user parameter item must declare a string operation."), Pointer + TEXT("/operation"),
								Row, TEXT("operation"), MakeShared<FJsonValueString>(TEXT("create|update|remove"))), UnitId);
						if (Captured)
						{
							StateSemantic::CanonicalJson::FValueComparison Difference;
							if (!CompareValues(MakeShared<FJsonValueObject>(*Captured), MakeShared<FJsonValueObject>(Row),
								UnitId, Pointer, Difference)) return false;
							if (Difference.bEquivalent) continue;
						}
						FString Name = Entry.Key;
						if (Operation == TEXT("update") || Operation == TEXT("remove"))
						{
							FString BoundId;
							Row->TryGetStringField(TEXT("unit_id"), BoundId);
							const FSemanticUnit* Bound = CurrentModel->FindUnit(BoundId);
							const auto* Request = Bound ? FindCaptured(*Bound) : nullptr;
							if (!Captured || !Bound || Bound->GetSemanticTypeRef() != UserParameterSemanticId || !Request)
								return SemanticFail(BindingFailure(TEXT("niagara_semantic_user_parameter_binding_missing"),
									TEXT("The user parameter item cannot be bound to a captured parameter."), Pointer,
									Captured != nullptr, Bound, UserParameterSemanticId, Request != nullptr), UnitId);
							Name = Request->TargetIdentity.GetSelector(TEXT("parameter_name"));
						}
						else if (Operation != TEXT("create") || Captured)
							return SemanticFail(NiagaraFieldFact(TEXT("niagara_semantic_user_parameter_operation_invalid"),
								TEXT("The parameter operation is unsupported or requests creation of an existing parameter."), Pointer + TEXT("/operation"),
								Row, TEXT("operation"), MakeShared<FJsonValueString>(Captured ? TEXT("update|remove") : TEXT("create"))), UnitId);
						const TSharedRef<FJsonObject> Parameter = MakeShared<FJsonObject>();
						if (Operation == TEXT("remove")) Parameter->SetBoolField(TEXT("remove"), true);
						else
						{
							for (const TCHAR* Field : {TEXT("type"), TEXT("value"), TEXT("has_value")})
								if (const auto* Value = Row->Values.Find(Field)) Parameter->SetField(Field, *Value);
						}
						Workspace.AttachInlinePropertySources(UnitId, Pointer, *Parameter);
						if (!OutUserParameters.IsValid()) OutUserParameters = MakeShared<FJsonObject>();
						OutUserParameters->SetObjectField(Name, Parameter);
					}
				}
				else if (Unit->GetSemanticTypeRef() == DynamicInputSemanticId)
					ChangedDynamicInputs.Add(Unit);
				else if (Unit->GetSemanticTypeRef() == EventHandlerSemanticId)
				{
					const auto* Request = FindCaptured(*Unit);
					if (!Request) return SemanticFail(NiagaraFact(TEXT("niagara_semantic_event_handler_binding_missing"),
						TEXT("The event handler has no matching captured read request."), TEXT("/captured_request"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), UnitId);
					const auto& Identity = Request->TargetIdentity;
					const FString OwnerId = Identity.GetSelector(TEXT("emitter_owner_id"));
					const FString EventId = Identity.GetSelector(TEXT("script_usage_id"));
					const TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
					if (!ProjectValues(*Unit, Event)) return false;
					const FSemanticUnit* BeforeUnit = CurrentModel->FindUnit(UnitId);
					const TSharedRef<FJsonObject> Before = MakeShared<FJsonObject>();
					if (!BeforeUnit) return SemanticFail(NiagaraFact(TEXT("niagara_semantic_event_handler_binding_missing"),
						TEXT("The captured event handler unit is unavailable."), TEXT("/captured_unit"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), UnitId);
					if (!ProjectValues(*BeforeUnit, Before)) return false;
					for (const TCHAR* Field : {TEXT("name"), TEXT("event_index")})
					{
						const FString Pointer = NiagaraPointer(FString(), Field);
						const auto* Value = Event->Values.Find(Field);
						const auto* Captured = Before->Values.Find(Field);
						if (!Value || !Captured)
							return SemanticFail(DifferenceDiagnostic(TEXT("niagara_semantic_event_identity_read_only"),
								TEXT("The event identity field must be present in both the desired and captured event."),
								StateSemantic::CanonicalJson::FValueComparison::Different(Pointer,
									Captured ? *Captured : nullptr, Value ? *Value : nullptr)), UnitId);
						StateSemantic::CanonicalJson::FValueComparison Difference;
						if (!CompareValues(*Captured, *Value, UnitId, Pointer, Difference)) return false;
						if (!Difference.bEquivalent)
							return SemanticFail(DifferenceDiagnostic(TEXT("niagara_semantic_event_identity_read_only"),
								TEXT("The event identity field is read-only."), Difference), UnitId);
						Event->RemoveField(Field);
					}
					TSharedPtr<FJsonObject> Owner;
					const TSharedPtr<FJsonObject>* ExistingOwner = nullptr;
					if (OutEmitters->TryGetObjectField(OwnerId, ExistingOwner)) Owner = *ExistingOwner;
					else
					{
						Owner = MakeShared<FJsonObject>();
						const TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
						for (const TCHAR* Selector : {TEXT("emitter_name"), TEXT("emitter_handle_id")})
						{
							const FString Value = Identity.GetSelector(Selector);
							if (!Value.IsEmpty()) Target->SetStringField(Selector, Value);
						}
						Owner->SetObjectField(TEXT("target"), Target);
						OutEmitters->SetObjectField(OwnerId, Owner);
					}
					TSharedPtr<FJsonObject> Events;
					const TSharedPtr<FJsonObject>* ExistingEvents = nullptr;
					if (Owner->TryGetObjectField(TEXT("event_handlers"), ExistingEvents)) Events = *ExistingEvents;
					else { Events = MakeShared<FJsonObject>(); Owner->SetObjectField(TEXT("event_handlers"), Events); }
					Workspace.AttachInlinePropertySources(UnitId, FString(), *Event);
					Events->SetObjectField(EventId, Event);
				}
				else if (Unit->GetSemanticTypeRef() == ModuleSemanticId)
					return SemanticFail(NiagaraFact(TEXT("niagara_semantic_module_collection_required"),
						TEXT("Edit modules through their owning stage module collection."), TEXT("/semantic_type"),
						MakeShared<FJsonValueString>(Unit->GetSemanticTypeRef()), MakeShared<FJsonValueString>(StackSemanticId)), UnitId);
				else if (Unit->GetSemanticTypeRef() == StackSemanticId)
					ChangedStages.Add(UnitId);
				else if (Unit->GetSemanticTypeRef() == RendererSemanticId)
				{
					const auto* Request = FindCaptured(*Unit);
					if (!Request)
						return SemanticFail(NiagaraFact(TEXT("niagara_semantic_renderer_binding_missing"),
							TEXT("The renderer has no matching captured read request."), TEXT("/captured_request"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), UnitId);
					const auto& Identity = Request->TargetIdentity;
					const FString OwnerId = Identity.GetSelector(TEXT("emitter_owner_id"));
					TSharedPtr<FJsonObject> Owner;
					const TSharedPtr<FJsonObject>* ExistingOwner = nullptr;
					if (OutEmitters->TryGetObjectField(OwnerId, ExistingOwner) && ExistingOwner)
						Owner = *ExistingOwner;
					else
					{
						Owner = MakeShared<FJsonObject>();
						const TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
						for (const TCHAR* Selector : {TEXT("emitter_name"), TEXT("emitter_handle_id")})
						{
							const FString Value = Identity.GetSelector(Selector);
							if (!Value.IsEmpty()) Target->SetStringField(Selector, Value);
						}
						Owner->SetObjectField(TEXT("target"), Target);
						OutEmitters->SetObjectField(OwnerId, Owner);
					}
					TSharedPtr<FJsonObject> Renderers;
					const TSharedPtr<FJsonObject>* ExistingRenderers = nullptr;
					if (Owner->TryGetObjectField(TEXT("renderers"), ExistingRenderers) && ExistingRenderers)
						Renderers = *ExistingRenderers;
					else
					{
						Renderers = MakeShared<FJsonObject>();
						Owner->SetObjectField(TEXT("renderers"), Renderers);
					}
					// The provider applies a complete renderer collection. Preserve
					// unchanged siblings when editing one renderer's properties.
					for (const FSemanticUnit& Sibling : DesiredModel->GetUnits())
					{
						if (Sibling.GetSemanticTypeRef() != RendererSemanticId) continue;
						const auto* SiblingRequest = FindCaptured(Sibling);
						if (!SiblingRequest || SiblingRequest->TargetIdentity.GetSelector(TEXT("emitter_owner_id")) != OwnerId)
							continue;
						const TSharedRef<FJsonObject> Renderer = MakeShared<FJsonObject>();
						if (!ProjectValues(Sibling, Renderer)) return false;
						// Keep the complete collection identity, but do not replay captured
						// properties over HandleVariableRemoved's native binding cleanup.
						if (const FSemanticUnit* BeforeUnit = CurrentModel->FindUnit(Sibling.GetUnitId()))
						{
							const TSharedRef<FJsonObject> BeforeRenderer = MakeShared<FJsonObject>();
							if (!ProjectValues(*BeforeUnit, BeforeRenderer)) return false;
							const TSharedPtr<FJsonObject> *BeforeProperties = nullptr, *Properties = nullptr;
							FString OldClass, NewClass;
							BeforeRenderer->TryGetStringField(TEXT("renderer_class"), OldClass);
							Renderer->TryGetStringField(TEXT("renderer_class"), NewClass);
							if (OldClass == NewClass && BeforeRenderer->TryGetObjectField(TEXT("properties"), BeforeProperties)
								&& Renderer->TryGetObjectField(TEXT("properties"), Properties))
							{
								TSharedPtr<FJsonObject> Changes;
								UE::UAI::Contract::FDiagnostic Detail;
								if (!UeAgentDetailsAuthoringService::ProjectPropertyDocumentChanges(*BeforeProperties, *Properties, Changes, Detail))
								{
									UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Detail, TEXT("/properties"), false, true);
									return SemanticFail(MoveTemp(Detail), Sibling.GetUnitId(), TEXT("/properties"));
								}
								Renderer->SetObjectField(TEXT("properties"), Changes);
							}
						}
						Workspace.AttachInlinePropertySources(Sibling.GetUnitId(), FString(), *Renderer);
						Renderers->SetObjectField(SiblingRequest->TargetIdentity.GetSelector(TEXT("renderer_name")), Renderer);
					}
				}
				else if (Unit->GetUnitKind() == ESemanticUnitKind::PropertySet)
				{
					const auto* Request = CapturedRequests.FindByPredicate([&](const UeAgentAuthoringState::FReadRequest& Read)
					{
						return Read.TargetIdentity.CanonicalKey == Unit->GetTargetIdentityKey()
							|| (Read.SnapshotIdentityOverride.IsSet()
								&& Read.SnapshotIdentityOverride->CanonicalKey == Unit->GetTargetIdentityKey());
					});
					const TSharedPtr<FJsonValue>* Property = Projection.PropertyChangesByUnitId.Find(UnitId);
					FString PropertyPath;
					if (!Request)
						return SemanticFail(NiagaraFact(TEXT("niagara_semantic_property_change_binding_missing"),
							TEXT("The property set has no matching captured read request."), TEXT("/captured_request"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), UnitId);
					if (!Property || !Property->IsValid() || (*Property)->Type != EJson::Object)
						return SemanticFail(NiagaraFact(TEXT("niagara_semantic_property_change_binding_missing"),
							TEXT("The projected property change must be an object."), TEXT("/property_change"),
							NiagaraJsonFact(Property ? *Property : nullptr), MakeShared<FJsonValueString>(TEXT("object"))), UnitId);
					if (!(*Property)->AsObject()->TryGetStringField(TEXT("property_path"), PropertyPath))
						return SemanticFail(NiagaraFieldFact(TEXT("niagara_semantic_property_change_binding_missing"),
							TEXT("The projected property change must declare a string property path."), TEXT("/property_path"),
							(*Property)->AsObject(), TEXT("property_path"), MakeShared<FJsonValueString>(TEXT("string property path"))), UnitId);
					if (Request->RootObject == RootObject)
					{
						if (!Projection.RootObjectProperties.ContainsByPredicate([&](const TSharedPtr<FJsonValue>& RootProperty)
						{
							FString RootPath;
							return RootProperty.IsValid() && RootProperty->Type == EJson::Object
								&& RootProperty->AsObject()->TryGetStringField(TEXT("property_path"), RootPath) && RootPath == PropertyPath;
						})) return SemanticFail(NiagaraFact(TEXT("niagara_semantic_root_property_projection_missing"),
							TEXT("The root property change is absent from the projected root property collection."),
							TEXT("/property_path"), MakeShared<FJsonValueString>(PropertyPath),
							MakeShared<FJsonValueString>(TEXT("property path included in the root property projection"))), UnitId);
						continue;
					}
					if (Request->RootStruct != FVersionedNiagaraEmitterData::StaticStruct())
						return SemanticFail(NiagaraFact(TEXT("niagara_semantic_scoped_property_not_supported"),
							TEXT("Scoped Niagara property edits require versioned emitter data."), TEXT("/root_struct"),
							Request->RootStruct ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Request->RootStruct->GetPathName()))
								: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
							MakeShared<FJsonValueString>(FVersionedNiagaraEmitterData::StaticStruct()->GetPathName())), UnitId);
					if (!Request->RootData)
						return SemanticFail(NiagaraFact(TEXT("niagara_semantic_scoped_property_not_supported"),
							TEXT("The captured emitter property data is unavailable."), TEXT("/root_data/available"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), UnitId);
					if (!Cast<UNiagaraEmitter>(const_cast<UObject*>(Request->NotificationOwner)))
						return SemanticFail(NiagaraFact(TEXT("niagara_semantic_scoped_property_not_supported"),
							TEXT("The scoped property notification owner must be a Niagara emitter."), TEXT("/notification_owner/class"),
							Request->NotificationOwner ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Request->NotificationOwner->GetClass()->GetPathName()))
								: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
							MakeShared<FJsonValueString>(EmitterType)), UnitId);
					const auto& BoundIdentity = Request->SnapshotIdentityOverride.IsSet()
						? Request->SnapshotIdentityOverride.GetValue() : Request->TargetIdentity;
					const FString OwnerId = BoundIdentity.GetSelector(TEXT("emitter_owner_id"));
					const FString EmitterName = BoundIdentity.GetSelector(TEXT("emitter_name"));
					if (OwnerId.IsEmpty())
						return SemanticFail(NiagaraFact(TEXT("niagara_semantic_emitter_property_binding_missing"),
							TEXT("The scoped property identity has no emitter owner binding."), TEXT("/emitter_owner_binding/present"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), UnitId);
					TSharedPtr<FJsonObject> Owner;
					const TSharedPtr<FJsonObject>* ExistingOwner = nullptr;
					if (OutEmitters->TryGetObjectField(OwnerId, ExistingOwner) && ExistingOwner)
						Owner = *ExistingOwner;
					else
					{
						Owner = MakeShared<FJsonObject>();
						const TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
						if (!EmitterName.IsEmpty()) Target->SetStringField(TEXT("emitter_name"), EmitterName);
						const FString HandleId = BoundIdentity.GetSelector(TEXT("emitter_handle_id"));
						if (!HandleId.IsEmpty()) Target->SetStringField(TEXT("emitter_handle_id"), HandleId);
						Owner->SetObjectField(TEXT("target"), Target);
						OutEmitters->SetObjectField(OwnerId, Owner);
					}
					TSharedPtr<FJsonObject> Properties;
					const TSharedPtr<FJsonObject>* ExistingProperties = nullptr;
					if (Owner->TryGetObjectField(TEXT("properties"), ExistingProperties) && ExistingProperties)
						Properties = *ExistingProperties;
					else
					{
						Properties = MakeShared<FJsonObject>();
						Properties->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
						Owner->SetObjectField(TEXT("properties"), Properties);
					}
					const TArray<TSharedPtr<FJsonValue>>* ExistingDescriptors = nullptr;
					TArray<TSharedPtr<FJsonValue>> Descriptors;
					if (Properties->TryGetArrayField(TEXT("properties"), ExistingDescriptors) && ExistingDescriptors)
						Descriptors = *ExistingDescriptors;
					Descriptors.Add(*Property);
					Properties->SetArrayField(TEXT("properties"), Descriptors);
					Properties->SetNumberField(TEXT("property_count"), Descriptors.Num());
				}
				else
					return SemanticFail(NiagaraFact(TEXT("niagara_semantic_surface_not_supported"),
						TEXT("The changed semantic type has no Niagara workspace projection."), TEXT("/semantic_type"),
						MakeShared<FJsonValueString>(Unit->GetSemanticTypeRef()),
						MakeShared<FJsonValueString>(TEXT("supported emitter, user parameter, event, renderer, property, stage or dynamic input surface"))), UnitId);
			}
			if (!Projection.StructuralEdits.IsEmpty())
				return SemanticFail(NiagaraFact(TEXT("niagara_semantic_module_collection_required"),
					TEXT("Direct structural edits must be expressed through the owning Niagara module collection."), TEXT("/structural_edits/count"),
					MakeShared<FJsonValueNumber>(Projection.StructuralEdits.Num()), MakeShared<FJsonValueNumber>(0)), FString());
			OutStages = MakeShared<FJsonObject>();
			FWorkspaceError ModuleFieldErrors;
			auto PointerToken = [](FString Token) {
				Token.ReplaceInline(TEXT("~"), TEXT("~0"));
				Token.ReplaceInline(TEXT("/"), TEXT("~1"));
				return Token;
			};
			for (const FString& StageId : ChangedStages)
			{
				const FSemanticUnit* StageUnit = DesiredModel->FindUnit(StageId);
				if (!StageUnit || StageUnit->GetSemanticTypeRef() != StackSemanticId)
					return SemanticFail(NiagaraFact(TEXT("niagara_semantic_stage_missing"),
						TEXT("The changed stage must resolve to a desired Niagara stack unit."), TEXT("/semantic_type"),
						StageUnit ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(StageUnit->GetSemanticTypeRef()))
							: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
						MakeShared<FJsonValueString>(StackSemanticId)), StageId);
				const TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
				if (!ProjectValues(*StageUnit, Target)) return false;
				const FSemanticUnit* BeforeStage = CurrentModel->FindUnit(StageId);
				const TSharedRef<FJsonObject> BeforeTarget = MakeShared<FJsonObject>();
				if (!BeforeStage)
					return SemanticFail(NiagaraFact(TEXT("niagara_semantic_stage_missing"),
						TEXT("The captured stage unit is unavailable."), TEXT("/captured_unit"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), StageId);
				if (!ProjectValues(*BeforeStage, BeforeTarget)) return false;
				const TSharedPtr<FJsonObject>* DesiredModulesField = nullptr;
				const TSharedPtr<FJsonObject>* BeforeModulesField = nullptr;
				if (!Target->TryGetObjectField(TEXT("modules"), DesiredModulesField))
					return SemanticFail(NiagaraFieldFact(TEXT("niagara_semantic_modules_missing"),
						TEXT("The desired stage module collection must be an object."), TEXT("/modules"),
						Target, TEXT("modules"), MakeShared<FJsonValueString>(TEXT("object"))), StageId);
				if (!BeforeTarget->TryGetObjectField(TEXT("modules"), BeforeModulesField))
					return SemanticFail(NiagaraFieldFact(TEXT("niagara_semantic_modules_missing"),
						TEXT("The captured stage module collection must be an object."), TEXT("/modules"),
						BeforeTarget, TEXT("modules"), MakeShared<FJsonValueString>(TEXT("object"))), StageId);
				const TSharedPtr<FJsonObject> DesiredModules = *DesiredModulesField;
				const TSharedPtr<FJsonObject> BeforeModules = *BeforeModulesField;
				Target->RemoveField(TEXT("modules"));
				BeforeTarget->RemoveField(TEXT("modules"));
				StateSemantic::CanonicalJson::FValueComparison StageDifference;
				if (!CompareValues(MakeShared<FJsonValueObject>(BeforeTarget), MakeShared<FJsonValueObject>(Target),
					StageId, FString(), StageDifference)) return false;
				if (!StageDifference.bEquivalent)
					return SemanticFail(DifferenceDiagnostic(TEXT("niagara_semantic_stage_edit_not_supported"),
						TEXT("Stage identity fields are read-only; edit its module collection instead."), StageDifference), StageId);
				const UeAgentAuthoringState::FReadRequest* CapturedStage = FindCaptured(*StageUnit);
				if (!CapturedStage) return SemanticFail(NiagaraFact(TEXT("niagara_semantic_stage_binding_missing"),
					TEXT("The stage has no matching captured read request."), TEXT("/captured_request"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), StageId);
				for (const TCHAR* Selector : {TEXT("usage_id"), TEXT("emitter_handle_id")})
				{
					const FString Value = CapturedStage->TargetIdentity.GetSelector(Selector);
					if (!Value.IsEmpty()) Target->SetStringField(Selector, Value);
				}
				const TSharedRef<FJsonObject> Modules = MakeShared<FJsonObject>();
				const TSharedRef<FJsonObject> CreatedModuleIdentities = MakeShared<FJsonObject>();
				bool bStageFieldError = false;
				for (const auto& Entry : DesiredModules->Values)
				{
					const FString ModulePointer = NiagaraPointer(TEXT("/modules"), Entry.Key);
					if (!Entry.Value.IsValid() || Entry.Value->Type != EJson::Object)
						return SemanticFail(NiagaraFact(TEXT("niagara_semantic_module_invalid"),
							TEXT("The stage module item must be an object."), ModulePointer,
							NiagaraJsonFact(Entry.Value), MakeShared<FJsonValueString>(TEXT("object"))), StageId);
					const TSharedRef<FJsonObject> Module = MakeShared<FJsonObject>(*Entry.Value->AsObject());
					const TSharedPtr<FJsonObject>* BeforeField = nullptr;
					BeforeModules->TryGetObjectField(Entry.Key, BeforeField);
					if (BeforeField)
					{
						StateSemantic::CanonicalJson::FValueComparison Difference;
						if (!CompareValues(MakeShared<FJsonValueObject>(*BeforeField), MakeShared<FJsonValueObject>(Module),
							StageId, ModulePointer, Difference)) return false;
						if (Difference.bEquivalent) continue;
					}
					FString Operation;
					if (!Module->TryGetStringField(TEXT("operation"), Operation))
						return SemanticFail(NiagaraFieldFact(TEXT("niagara_semantic_module_operation_missing"),
							TEXT("The module item must declare a string operation."), ModulePointer + TEXT("/operation"),
							Module, TEXT("operation"), MakeShared<FJsonValueString>(TEXT("create|update|remove"))), StageId);
					bool bModuleFieldError = false;
					for (const auto& Field : Module->Values)
					{
						if (Field.Key == TEXT("candidate") || Field.Key == TEXT("module_index")
							|| Field.Key == TEXT("inputs") || Field.Key == TEXT("assignment_targets")
							|| Field.Key == TEXT("module_kind") || Field.Key == TEXT("operation")) continue;
						if (Operation == TEXT("create") && Field.Key == TEXT("id")) continue;
						const TSharedPtr<FJsonValue>* BeforeValue = BeforeField ? (*BeforeField)->Values.Find(Field.Key) : nullptr;
						const FString Pointer = NiagaraPointer(ModulePointer, Field.Key);
						StateSemantic::CanonicalJson::FValueComparison Difference;
						if (!CompareValues(BeforeValue ? *BeforeValue : nullptr, Field.Value, StageId, Pointer, Difference)) return false;
						if (!BeforeValue || !Difference.bEquivalent)
						{
							if (Difference.bEquivalent)
								Difference = StateSemantic::CanonicalJson::FValueComparison::Different(Pointer, nullptr, Field.Value);
							FWorkspaceError FieldError;
							LocateFailure(FieldError, DifferenceDiagnostic(TEXT("niagara_semantic_module_field_not_supported"),
								TEXT("This module field is read-only or is not supported by module collection edits."), Difference), StageId, Pointer);
							ModuleFieldErrors.Append(FieldError);
							bModuleFieldError = bStageFieldError = true;
						}
					}
					if (bModuleFieldError) continue;
					if (Operation == TEXT("update") || Operation == TEXT("remove"))
					{
						FString ModuleId;
						Module->TryGetStringField(TEXT("unit_id"), ModuleId);
						const FSemanticUnit* Bound = CurrentModel->FindUnit(ModuleId);
						const auto* CapturedModule = Bound ? FindCaptured(*Bound) : nullptr;
						if (!BeforeField || !Bound || Bound->GetOwnerRef() != StageId
							|| Bound->GetSemanticTypeRef() != ModuleSemanticId || !CapturedModule)
							return SemanticFail(BindingFailure(TEXT("niagara_semantic_module_binding_missing"),
								TEXT("The module item cannot be bound to a captured module in this stage."), ModulePointer,
								BeforeField != nullptr, Bound, ModuleSemanticId, CapturedModule != nullptr,
								Bound && Bound->GetOwnerRef() == StageId), StageId);
						Module->SetStringField(TEXT("module_guid"), CapturedModule->TargetIdentity.GetSelector(TEXT("module_guid")));
						if (Operation == TEXT("remove")) Module->SetBoolField(TEXT("remove"), true);
					}
					else if (Operation != TEXT("create") || BeforeField)
						return SemanticFail(NiagaraFieldFact(TEXT("niagara_semantic_module_operation_invalid"),
							TEXT("The module operation is unsupported or requests creation of an existing module."), ModulePointer + TEXT("/operation"),
							Module, TEXT("operation"), MakeShared<FJsonValueString>(BeforeField ? TEXT("update|remove") : TEXT("create"))), StageId);
					else
					{
						// A visible new-item key is a request-local alias, not a native
						// identity. Reusing it after deletion must still create a new node.
						const FString NewGuid = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
						Module->SetStringField(TEXT("module_guid"), NewGuid);
						auto Selectors = CapturedStage->TargetIdentity.Selectors;
						Selectors.Add(TEXT("module_guid"), NewGuid);
						UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
						UE::UAI::Contract::FDiagnostic IdentityError;
						if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(TEXT("niagara_module_identity"),
							TEXT("niagara_module"), Selectors, Identity, IdentityError))
							return SemanticFail(MoveTemp(IdentityError), StageId, ModulePointer);
						const auto Argument = MakeShared<FJsonObject>();
						Argument->SetStringField(TEXT("id"), Identity.CanonicalKey);
						CreatedModuleIdentities->SetObjectField(Entry.Key, Argument);
					}
					Module->RemoveField(TEXT("unit_id"));
					Module->RemoveField(TEXT("id"));
					Module->RemoveField(TEXT("operation"));
					Module->RemoveField(TEXT("name"));
					if (Module->HasField(TEXT("candidate"))) Module->RemoveField(TEXT("script_asset_path"));
					Workspace.AttachInlinePropertySources(StageId,
						TEXT("/modules/") + PointerToken(Entry.Key), *Module, TEXT("name"));
					Modules->SetObjectField(Entry.Key, Module);
				}
				if (bStageFieldError) continue;
				if (!CreatedModuleIdentities->Values.IsEmpty())
					UeAgentNativeNormalizationIntent::RecordKeyedArguments(TEXT("modules"),
						MakeShared<FJsonValueObject>(DesiredModules), CreatedModuleIdentities,
						CapturedStage->TargetIdentity.CanonicalKey);
				const TSharedRef<FJsonObject> Stage = MakeShared<FJsonObject>();
				if (Modules->Values.IsEmpty())
					return SemanticFail(NiagaraFact(TEXT("niagara_semantic_stage_change_unconsumed"),
						TEXT("The changed stage produced no supported module edits."), TEXT("/modules/edit_count"),
						MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one module edit"))), StageId);
				Stage->SetObjectField(TEXT("target"), Target);
				Stage->SetStringField(TEXT("operation"), TEXT("update"));
				Stage->SetObjectField(TEXT("modules"), Modules);
				Workspace.AttachInlinePropertySources(StageId, FString(), *Stage);
				OutStages->SetObjectField(StageId, Stage);
			}
			if (ModuleFieldErrors.HasErrors()) { OutError = MoveTemp(ModuleFieldErrors); return false; }
			for (const FSemanticUnit* DynamicUnit : ChangedDynamicInputs)
			{
				const auto* Request = FindCaptured(*DynamicUnit);
				if (!Request) return SemanticFail(NiagaraFact(TEXT("niagara_semantic_dynamic_input_binding_missing"),
					TEXT("The dynamic input has no matching captured read request."), TEXT("/captured_request"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), DynamicUnit->GetUnitId());
				const auto& Identity = Request->TargetIdentity;
				const auto* StageRequest = CapturedRequests.FindByPredicate([&](const auto& Candidate) {
					return Candidate.SemanticId == StackSemanticId
						&& Candidate.TargetIdentity.GetSelector(TEXT("asset_path")) == Identity.GetSelector(TEXT("asset_path"))
						&& Candidate.TargetIdentity.GetSelector(TEXT("stage_owner_id")) == Identity.GetSelector(TEXT("stage_owner_id"))
						&& Candidate.TargetIdentity.GetSelector(TEXT("usage")) == Identity.GetSelector(TEXT("usage"))
						&& Candidate.TargetIdentity.GetSelector(TEXT("usage_id")) == Identity.GetSelector(TEXT("usage_id")); });
				if (!StageRequest) return SemanticFail(NiagaraFact(TEXT("niagara_semantic_dynamic_input_stage_missing"),
					TEXT("No captured stage request matches the dynamic input's asset, owner and usage selectors."), TEXT("/captured_stage_request"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), DynamicUnit->GetUnitId());
				const auto* StageUnit = CurrentModel->GetUnits().FindByPredicate([&](const auto& Candidate) {
					return Candidate.GetSemanticTypeRef() == StackSemanticId && Candidate.GetTargetIdentityKey() == StageRequest->TargetIdentity.CanonicalKey; });
				if (!StageUnit) return SemanticFail(NiagaraFact(TEXT("niagara_semantic_dynamic_input_stage_missing"),
					TEXT("The captured stage request has no matching current stack unit."), TEXT("/captured_stage_unit"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), DynamicUnit->GetUnitId());
				TSharedPtr<FJsonObject> Stage;
				const TSharedPtr<FJsonObject>* ExistingStage = nullptr;
				if (OutStages->TryGetObjectField(StageUnit->GetUnitId(), ExistingStage)) Stage = *ExistingStage;
				else
				{
					Stage = MakeShared<FJsonObject>();
					const TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
					if (!ProjectValues(*StageUnit, Target)) return false;
					Target->RemoveField(TEXT("modules"));
					for (const TCHAR* Selector : {TEXT("usage_id"), TEXT("emitter_handle_id")}) {
						const FString Value = StageRequest->TargetIdentity.GetSelector(Selector);
						if (!Value.IsEmpty()) Target->SetStringField(Selector, Value); }
					Stage->SetObjectField(TEXT("target"), Target);
					Stage->SetObjectField(TEXT("modules"), MakeShared<FJsonObject>());
					Stage->SetStringField(TEXT("operation"), TEXT("update"));
					OutStages->SetObjectField(StageUnit->GetUnitId(), Stage);
				}
				const TSharedPtr<FJsonObject>* Modules = nullptr;
				if (!Stage->TryGetObjectField(TEXT("modules"), Modules))
					return SemanticFail(NiagaraFieldFact(TEXT("niagara_semantic_modules_missing"),
						TEXT("The projected dynamic input stage has no module collection object."), TEXT("/modules"),
						Stage, TEXT("modules"), MakeShared<FJsonValueString>(TEXT("object"))), StageUnit->GetUnitId());
				const FString ModuleGuid = Identity.GetSelector(TEXT("module_guid"));
				TSharedPtr<FJsonObject> Module;
				for (const auto& Entry : (*Modules)->Values) {
					if (!Entry.Value.IsValid() || Entry.Value->Type != EJson::Object) continue;
					FString Guid; Entry.Value->AsObject()->TryGetStringField(TEXT("module_guid"), Guid);
					if (Guid == ModuleGuid) { Module = Entry.Value->AsObject(); break; } }
				if (!Module) { Module = MakeShared<FJsonObject>(); Module->SetStringField(TEXT("module_guid"), ModuleGuid); (*Modules)->SetObjectField(ModuleGuid, Module); }
				TSharedPtr<FJsonObject> DynamicInputs;
				const TSharedPtr<FJsonObject>* ExistingInputs = nullptr;
				if (Module->TryGetObjectField(TEXT("dynamic_inputs"), ExistingInputs)) DynamicInputs = *ExistingInputs;
				else { DynamicInputs = MakeShared<FJsonObject>(); Module->SetObjectField(TEXT("dynamic_inputs"), DynamicInputs); }
				const TSharedRef<FJsonObject> Dynamic = MakeShared<FJsonObject>();
				if (!ProjectValues(*DynamicUnit, Dynamic)) return false;
				const FSemanticUnit* BeforeDynamicUnit = CurrentModel->FindUnit(DynamicUnit->GetUnitId());
				const TSharedRef<FJsonObject> BeforeDynamic = MakeShared<FJsonObject>();
				if (!BeforeDynamicUnit)
					return SemanticFail(NiagaraFact(TEXT("niagara_semantic_dynamic_input_binding_missing"),
						TEXT("The captured dynamic input unit is unavailable."), TEXT("/captured_unit"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), DynamicUnit->GetUnitId());
				if (!ProjectValues(*BeforeDynamicUnit, BeforeDynamic)) return false;
				for (const TCHAR* Field : {TEXT("name"), TEXT("input_name")}) {
					const auto* Value = Dynamic->Values.Find(Field);
					const auto* Captured = BeforeDynamic->Values.Find(Field);
					const FString Pointer = NiagaraPointer(FString(), Field);
					if (!Value || !Captured)
						return SemanticFail(DifferenceDiagnostic(TEXT("niagara_semantic_dynamic_input_identity_read_only"),
							TEXT("The dynamic input identity field must be present in both desired and captured inputs."),
							StateSemantic::CanonicalJson::FValueComparison::Different(Pointer,
								Captured ? *Captured : nullptr, Value ? *Value : nullptr)), DynamicUnit->GetUnitId());
					StateSemantic::CanonicalJson::FValueComparison Difference;
					if (!CompareValues(*Captured, *Value, DynamicUnit->GetUnitId(), Pointer, Difference)) return false;
					if (!Difference.bEquivalent)
						return SemanticFail(DifferenceDiagnostic(TEXT("niagara_semantic_dynamic_input_identity_read_only"),
							TEXT("The dynamic input identity field is read-only."), Difference), DynamicUnit->GetUnitId()); }
				Dynamic->RemoveField(TEXT("name"));
				Workspace.AttachInlinePropertySources(DynamicUnit->GetUnitId(), FString(), *Dynamic);
				DynamicInputs->SetObjectField(Identity.GetSelector(TEXT("input_name")), Dynamic);
			}
			return true;
		}

		bool TryImportSemanticWorkspace(
			const FString& Target,
			const FResolvedDomainHjsonDocument& Document,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			using namespace UE::UAI::Composition::HjsonImport;

			if (!Document.SemanticWorkspace.IsValid())
			{
				return Fail(OutError, NiagaraNativeFact(TEXT("niagara_semantic_workspace_missing"),
					TEXT("The resolved Niagara import has no semantic workspace."), TEXT("/semantic_workspace/available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target));
			}
			UObject* Asset = nullptr;
			DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture TargetCapture;
			if (!TryCaptureObject(
					Target, TArray<FString>{SystemType, EmitterType, ScriptType},
					Asset, TargetCapture, OutError))
			{
				return false;
			}
			const FString RootType = Asset
				? Asset->GetClass()->GetPathName() : FString();
			const FString AssetKind = UeAgentNiagaraAuthoring::GetAssetKind(Asset);
			if (!Asset || ExpectedKind(RootType).IsEmpty() || AssetKind.IsEmpty())
			{
				if (!Asset)
					return Fail(OutError, NiagaraNativeFact(TEXT("niagara_domain_root_type_invalid"),
						TEXT("The captured Niagara asset is unavailable."), TEXT("/target/available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target));
				if (ExpectedKind(RootType).IsEmpty())
					return Fail(OutError, NiagaraNativeFact(TEXT("niagara_domain_root_type_invalid"),
						TEXT("The target class is not a supported Niagara asset class."), TEXT("/root_type"),
						MakeShared<FJsonValueString>(RootType), MakeShared<FJsonValueString>(SystemType + TEXT("|") + EmitterType + TEXT("|") + ScriptType), Target));
				return Fail(OutError, NiagaraNativeFact(TEXT("niagara_domain_root_type_invalid"),
					TEXT("The captured asset has no Niagara asset kind."), TEXT("/asset_kind"),
					MakeShared<FJsonValueString>(AssetKind), MakeShared<FJsonValueString>(ExpectedKind(RootType)), Target));
			}
			UE::UAI::Contract::FDiagnostic RegistrationError;
			if (!UeAgentNiagaraAuthoring::RegisterAuthoring(RegistrationError))
			{
				return Fail(OutError, RegistrationError.IsValid() ? MoveTemp(RegistrationError) : NiagaraNativeFact(
					TEXT("niagara_domain_authoring_registration_failed"), TEXT("Niagara authoring registration did not succeed."),
					TEXT("/authoring_registration/succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target));
			}

			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			UE::UAI::Contract::FDiagnostic CaptureError;
			if (!UeAgentNiagaraAuthoring::BuildAssetExportSnapshotReadRequests(
					*Asset, Requests, CaptureError))
			{
				OutError.SetFailure(CaptureError, Target);
				return false;
			}
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
			{
				OutError.SetFailure(Capture.Diagnostic, Target);
				return false;
			}

			ContextResource::FUeAgentResourceTracker Resources =
				MakeSemanticWorkspaceResourceTracker();
			StateSemantic::FUeAgentCanonicalSnapshotPtr CanonicalSnapshot;
			FAuthoringModelPtr Model;
			const FString CompositionKind = ResolveTargetCompositionKind(
				TEXT("niagara"), AssetKind);
			if (!TryBuildCanonicalAuthoringCapture(
					CompositionKind, TEXT("llm_authoring"), Capture.Snapshot,
					Resources, CanonicalSnapshot, Model, OutError)) return false;
			if (!CanonicalSnapshot.IsValid())
				return Fail(OutError, NiagaraNativeFact(TEXT("niagara_semantic_capture_failed"),
					TEXT("Canonical authoring capture returned no snapshot."), TEXT("/canonical_snapshot/available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target));
			if (!Model.IsValid())
				return Fail(OutError, NiagaraNativeFact(TEXT("niagara_semantic_capture_failed"),
					TEXT("Canonical authoring capture returned no semantic model."), TEXT("/semantic_model/available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target));

			Capture.Snapshot.ReleaseNativeValues();
			const FString TargetPackage = PackageIdentity(Target);
			const FString TargetObjectPath = ObjectPathFromPackageIdentity(Target);
			return TryCoordinateSemanticWorkspaceImport(
				Document.SemanticWorkspace.ToSharedRef(), Model.ToSharedRef(),
				CanonicalSnapshot.ToSharedRef(),
				Contract::FUeAgentDescriptorIdentity(
					TEXT("niagara.semantic_workspace"), 1,
					Model->GetSemanticFingerprint()),
				TargetObjectPath, Resources,
				{
					TEXT("niagara-domain-provider"), Target, TargetObjectPath,
					TEXT("niagara-semantic"),
					TEXT("niagara_semantic_prepare_failed"),
					TEXT("niagara_semantic_apply_failed"),
					TEXT("niagara_semantic_readback_payload_invalid"),
					TEXT("niagara_domain_compile_not_settled"),
					[TargetObjectPath](TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error)
					{
						UObject* Native = StaticLoadObject(UObject::StaticClass(), nullptr, *TargetObjectPath);
						if (!Native)
						{
							Error = NiagaraNativeFact(TEXT("niagara_semantic_capture_failed"),
								TEXT("The Niagara target could not be reloaded for semantic readback."), TEXT("/target/available"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TargetObjectPath);
							return false;
						}
						return UeAgentNiagaraAuthoring::BuildAssetExportSnapshotReadRequests(*Native, Requests, Error);
					}},
				[AssetKind, TargetPackage, &Asset, &Model, &Requests](
					const FString& RequestId,
					const FPreparedAuthoringImport& Prepared,
					FSemanticWorkspaceChangeProjection&& Projection,
					FRootPropertyApplyOutcome& OutOutcome)
				{
					UeAgentNiagaraAuthoring::FAssetApplyRequest ApplyRequest;
					FWorkspaceError ProjectionError;
					if (!TryProjectSemanticStages(Prepared.GetWorkspace()->GetModel(), Model.ToSharedRef(),
						Requests, Asset, Projection, ApplyRequest.Stages, ApplyRequest.Emitters, ApplyRequest.UserParameters, ProjectionError, Prepared.GetWorkspace().Get()))
					{
						OutOutcome.Diagnostic = ProjectionError.GetDiagnostic();
						OutOutcome.Phase = TEXT("semantic_projection");
						return false;
					}
					const TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
					if (ApplyRequest.Stages.IsValid()) Extension->SetObjectField(TEXT("stages"), ApplyRequest.Stages);
					if (ApplyRequest.Emitters.IsValid() && !ApplyRequest.Emitters->Values.IsEmpty())
						Extension->SetObjectField(TEXT("emitters"), ApplyRequest.Emitters);
					if (!Asset)
					{
						OutOutcome.Diagnostic = NiagaraNativeFact(TEXT("niagara_domain_target_invalid"),
							TEXT("The Niagara target is unavailable for semantic module candidate resolution."), TEXT("/target/available"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TargetPackage);
						OutOutcome.Phase = TEXT("candidate_resolution");
						return false;
					}
					if (!NormalizeModuleCandidates(*Asset, Extension, ProjectionError, TargetPackage))
					{
						OutOutcome.Diagnostic = ProjectionError.GetDiagnostic();
						OutOutcome.Phase = TEXT("candidate_resolution");
						return false;
					}
					ApplyRequest.Operation = UeAgentApplyInputContract::EOperation::Update;
					ApplyRequest.AssetPath = TargetPackage;
					ApplyRequest.AssetKind = AssetKind;
					ApplyRequest.Properties = MoveTemp(Projection.RootObjectProperties);
					ApplyRequest.bCompile = true;
					ApplyRequest.bSave = true;
					ApplyRequest.bCompactValues = true;
					ApplyRequest.FinalReadback = OutOutcome.FinalReadback;
					Asset = nullptr;
					UeAgentNiagaraAuthoring::FAssetApplyResult ApplyResult;
					const bool bApplied = UeAgentNiagaraAuthoring::ApplyAsset(
						ApplyRequest, ApplyResult, RequestId);
					OutOutcome.bApplied = bApplied && ApplyResult.IsApplied();
					OutOutcome.Phase = MoveTemp(ApplyResult.Phase);
					OutOutcome.Diagnostic = MoveTemp(ApplyResult.Diagnostic);
					ApplyResult.Asset = nullptr;
					Asset = nullptr;
					return bApplied;
				},
				[&Target, &RootType](FString& OutReadbackPayload,
					FString& OutRevision, FWorkspaceError& OutReadError)
				{
					return TryRead(
						Target, RootType, OutReadbackPayload, OutRevision,
						nullptr, nullptr, OutReadError);
				},
				OutPayload, OutError);
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
				return Fail(OutError, NiagaraNativeFact(TEXT("niagara_domain_root_type_invalid"),
					TEXT("The root class is not a supported Niagara asset class."), TEXT("/root_type"),
					MakeShared<FJsonValueString>(RootType),
					MakeShared<FJsonValueString>(SystemType + TEXT("|") + EmitterType + TEXT("|") + ScriptType), Target));
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
			UE::UAI::HjsonWorkspace::DomainAuthoring::FScopedDomainHjsonDiagnosticSource DiagnosticSource(Resolved, OutError);
			if (Resolved.bSemanticWorkspaceDirectory)
			{
				return TryImportSemanticWorkspace(
					Target, Resolved, OutPayload, OutError);
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
					auto Error = NiagaraNativeFact(TEXT("niagara_domain_stale_revision"),
						TEXT("The Niagara asset changed after the workspace was captured."), TEXT("/target/revision_matches_expected"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target);
					Error.ExpectedEvidence = MakeShared<FJsonValueString>(Resolved.ExpectedRevision);
					Error.ActualEvidence = MakeShared<FJsonValueString>(CurrentRevision);
					return Fail(OutError, MoveTemp(Error));
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
				return Fail(OutError, NiagaraFieldFact(TEXT("niagara_domain_document_required"),
					TEXT("The Niagara workspace root must contain an edit document object."), TEXT("/document"),
					Resolved.Root, TEXT("document"), MakeShared<FJsonValueString>(TEXT("object")), Target));
			}
			TSharedPtr<FJsonObject> Document;
			if (!PrepareDocument(
					Target, RootType, bExists, ExistingTarget,
					*DocumentField, Document, OutError))
			{
				return false;
			}

			UE::UAI::Contract::FDiagnostic RegistrationError;
			if (!UeAgentNiagaraAuthoring::RegisterAuthoring(
					RegistrationError))
			{
				return Fail(OutError, RegistrationError.IsValid() ? MoveTemp(RegistrationError) : NiagaraNativeFact(
					TEXT("niagara_domain_authoring_registration_failed"), TEXT("Niagara authoring registration did not succeed."),
					TEXT("/authoring_registration/succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target));
			}
			UeAgentNiagaraAuthoring::FAssetApplyResult ApplyResult;
			FString AfterPayload;
			FString AfterRevision;
			TSharedPtr<FJsonObject> AfterSnapshot;
			auto FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
			{
				FWorkspaceError ReadError;
				if (!TryRead(Target, RootType, AfterPayload, AfterRevision, &AfterSnapshot, nullptr, ReadError)
					|| !TryValidateReadbackDiagnostics(AfterSnapshot, Target, TEXT("niagara_domain_compile_not_settled"), ReadError))
				{
					Failure.Diagnostic = ReadError.GetDiagnostic();
					return false;
				}
				return true;
			};
			const FString RequestId = TEXT("niagara-domain-")
				+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			if (!UeAgentNiagaraAuthoring::ApplyAssetInlineDocument(
					Document,
					Kind,
					PackageIdentity(Target),
					ApplyResult,
					RequestId, true, FinalReadback))
			{
				if (ApplyResult.Diagnostic.TargetPath.IsEmpty()) ApplyResult.Diagnostic.TargetPath = Target;
				OutError.SetFailure(MoveTemp(ApplyResult.Diagnostic));
				return false;
			}
			if (!ApplyResult.CanReportComplete()
				|| !ApplyResult.Asset
				|| !PackageIdentity(ApplyResult.AssetPath).Equals(
					PackageIdentity(Target), ESearchCase::IgnoreCase))
			{
				UE::UAI::Contract::FDiagnostic Error;
				if (!ApplyResult.CanReportComplete())
				{
					const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
					const TSharedRef<FJsonObject> Expected = MakeShared<FJsonObject>();
					Actual->SetBoolField(TEXT("saved"), ApplyResult.bSaved);
					Expected->SetBoolField(TEXT("saved"), true);
					if (ApplyResult.ResolvedOperation != TEXT("remove"))
					{
						Actual->SetStringField(TEXT("compile_status"), ApplyResult.CompileStatus);
						Actual->SetBoolField(TEXT("compilation_complete"), ApplyResult.bCompilationComplete);
						Actual->SetNumberField(TEXT("compile_error_count"), ApplyResult.CompileErrorCount);
						Actual->SetNumberField(TEXT("stack_issue_count"), ApplyResult.StackIssueCount);
						Actual->SetNumberField(TEXT("compiled_target_count"), ApplyResult.CompiledTargets.Num());
						Expected->SetStringField(TEXT("compile_status"), TEXT("complete"));
						Expected->SetBoolField(TEXT("compilation_complete"), true);
						Expected->SetNumberField(TEXT("compile_error_count"), 0);
						Expected->SetNumberField(TEXT("stack_issue_count"), 0);
						Expected->SetStringField(TEXT("compiled_target_count"), TEXT("at least one"));
					}
					Error = NiagaraNativeFact(TEXT("niagara_domain_authoring_receipt_invalid"),
						TEXT("The Niagara import did not return a durable completion receipt."), TEXT("/receipt"),
						MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected), Target);
				}
				else if (!ApplyResult.Asset)
					Error = NiagaraNativeFact(TEXT("niagara_domain_authoring_receipt_invalid"),
						TEXT("The completed Niagara import returned no asset object."), TEXT("/receipt/asset_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target);
				else
					Error = NiagaraNativeFact(TEXT("niagara_domain_authoring_receipt_invalid"),
						TEXT("The completed Niagara import returned a different asset path."), TEXT("/receipt/asset_path"),
						MakeShared<FJsonValueString>(ApplyResult.AssetPath), MakeShared<FJsonValueString>(PackageIdentity(Target)), Target);
				if (ApplyResult.Diagnostic.IsValid())
					Error.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(MoveTemp(ApplyResult.Diagnostic));
				return Fail(OutError, MoveTemp(Error));
			}

			ApplyResult.Asset = nullptr;

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
			LifecycleReceipt.bNativeUpdateSettled = true;
			LifecycleReceipt.bPackageSaved = true;
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
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			return Fail(OutError, JsonError.IsValid() ? MoveTemp(JsonError) : NiagaraFact(
				TEXT("niagara_domain_params_invalid"), TEXT("The Niagara command parameters must be a JSON object."), TEXT("/params"),
				MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("object")), TargetIdentity, TEXT("command_input")));
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target))
			return Fail(OutError, NiagaraFact(TEXT("niagara_domain_target_identity_mismatch"),
				TEXT("The Niagara command target must be a string asset path."), TEXT("/target"),
				NiagaraJsonFact(NiagaraField(Params, TEXT("target"))), MakeShared<FJsonValueString>(TargetIdentity), TargetIdentity, TEXT("command_input")));
		if (ObjectPathFromPackageIdentity(Target) != ObjectPathFromPackageIdentity(TargetIdentity))
			return Fail(OutError, NiagaraFact(TEXT("niagara_domain_target_identity_mismatch"),
				TEXT("The command target does not identify the requested Niagara asset."), TEXT("/target"),
				MakeShared<FJsonValueString>(Target), MakeShared<FJsonValueString>(TargetIdentity), TargetIdentity, TEXT("command_input")));
		if (ExpectedKind(RootTypeIdentity).IsEmpty())
		{
			return Fail(OutError, NiagaraFact(TEXT("niagara_domain_root_type_invalid"),
				TEXT("The command root class is not a supported Niagara asset class."), TEXT("/root_type"),
				MakeShared<FJsonValueString>(RootTypeIdentity),
				MakeShared<FJsonValueString>(SystemType + TEXT("|") + EmitterType + TEXT("|") + ScriptType), Target, TEXT("command_input")));
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(
				Target, RootTypeIdentity, CanonicalParamsJson,
				OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			bOutReadOnly = true;
			FString WorkspacePath;
			if (!Params->TryGetStringField(TEXT("workspace_path"), WorkspacePath)
				|| WorkspacePath.IsEmpty())
			{
				return Fail(OutError, NiagaraFact(TEXT("niagara_domain_export_path_required"),
					TEXT("Niagara workspace export requires a non-empty workspace path."), TEXT("/workspace_path"),
					NiagaraJsonFact(NiagaraField(Params, TEXT("workspace_path"))),
					MakeShared<FJsonValueString>(TEXT("non-empty workspace path")), Target, TEXT("command_input")));
			}
			UObject* Asset = nullptr;
			DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture Capture;
			if (!TryCaptureObject(
					Target, RootTypeIdentity, Asset, Capture, OutError))
			{
				return false;
			}
			UE::UAI::Contract::FDiagnostic RegistrationError;
			if (!UeAgentNiagaraAuthoring::RegisterAuthoring(RegistrationError))
			{
				return Fail(OutError, RegistrationError.IsValid() ? MoveTemp(RegistrationError) : NiagaraNativeFact(
					TEXT("niagara_domain_authoring_registration_failed"), TEXT("Niagara authoring registration did not succeed."),
					TEXT("/authoring_registration/succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target));
			}
			UeAgentExportWorkspaceWriter::FWorkspaceResult ExportResult;
			if (!UeAgentNiagaraAuthoring::ExportAssetWorkspace(*Asset, WorkspacePath, TEXT("llm_authoring"), ExportResult))
			{
				OutError.SetFailure(ExportResult.Diagnostic, Target);
				return false;
			}
			const TSharedPtr<FJsonObject> Result = ExportResult.ToPublicJsonObject();
			return TryCanonicalizeObject(Result, OutPayloadJson, OutError);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			FString Revision;
			bOutReadOnly = true;
			return TryRead(
				Target, RootTypeIdentity, OutPayloadJson, Revision,
				nullptr, nullptr, OutError);
		}
		return Fail(OutError, NiagaraFact(TEXT("niagara_domain_command_not_supported"),
			TEXT("The Niagara domain adapter cannot execute this command family."), TEXT("/command"),
			MakeShared<FJsonValueString>(CommandId), MakeShared<FJsonValueString>(TEXT("content_graph.*")), Target, TEXT("command_input")));
	}
}
