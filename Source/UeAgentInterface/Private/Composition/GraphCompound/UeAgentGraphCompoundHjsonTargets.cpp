#include "Composition/GraphCompound/UeAgentGraphCompoundHjsonTargets.h"

#include "Composition/HjsonImport/UeAgentHjsonImportOperationRuntime.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/HjsonWorkspace/UeAgentSnapshotComposition.h"
#include "DomainAdapters/BlueprintUmgGraph/UeAgentBlueprintUmgGraphNativeHook.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphNativeHook.h"
#include "Engine/Blueprint.h"
#include "FileHelpers.h"
#include "HAL/PlatformTime.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace UE::UAI::BindingGateway::Operation;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Verification;
	using namespace UE::UAI::Composition::HjsonImport;

	namespace
	{
		void SetError(
			FUeAgentHjsonImportTargetError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		void CopyError(
			const FUeAgentHjsonImportTargetError& Source,
			FUeAgentOperationBindingError& OutError)
		{
			OutError = {};
			OutError.Code = Source.Code;
			OutError.SubjectId = Source.SubjectId;
			OutError.Detail = Source.Detail;
		}

		bool Checkpoint(
			const FUeAgentOperationContext& Context,
			const FString& Stage,
			FUeAgentOperationBindingError& OutError)
		{
			FUeAgentCancellationRequested Cancelled;
			if (!Context.GetCancellationToken().TryCheckpoint(Cancelled))
			{
				OutError = {};
				OutError.Code = TEXT("operation_cancelled");
				OutError.SubjectId = Context.GetRequestId();
				OutError.Detail = Cancelled.Reason;
				return false;
			}
			FUeAgentOperationDeadlineExceeded Expired;
			if (!Context.GetDeadline().TryCheckpoint(Stage, Expired))
			{
				OutError = {};
				OutError.Code = TEXT("operation_deadline_exceeded");
				OutError.SubjectId = Context.GetRequestId();
				OutError.Detail = Expired.Stage;
				return false;
			}
			FUeAgentResourceBudgetExceeded Budget;
			if (!Context.GetResourceTracker().TryCharge(
					EUeAgentResourceDimension::WorkUnits,
					1,
					Stage,
					Budget))
			{
				OutError = {};
				OutError.Code = Budget.Code;
				OutError.SubjectId = Budget.Path;
				OutError.Detail = TEXT(
					"Graph/Compound HJSON operation work budget exceeded.");
				OutError.ResourceDimension = Budget.Dimension;
				OutError.ResourceLimit = Budget.Limit;
				OutError.ResourceObserved = Budget.Observed;
				return false;
			}
			return true;
		}

		bool ValidateCanonicalInput(
			const Contract::FUeAgentContractValue& CanonicalInput,
			const FUeAgentHjsonTargetImportRequest& Request,
			FUeAgentOperationBindingError& OutError)
		{
			TSharedPtr<FJsonObject> Root;
			FString ParseError;
			FString CompositionKind;
			FString Target;
			FString WorkspaceFingerprint;
			FString ExpectedRevision;
			FString Intent;
			bool bDryRun = false;
			if (!Contract::Json::TryParseObject(
					CanonicalInput.GetJsonValue(),
					Root,
					ParseError)
				|| !Root.IsValid()
				|| !Root->TryGetStringField(
					TEXT("composition_kind"),
					CompositionKind)
				|| !Root->TryGetStringField(TEXT("target"), Target)
				|| !Root->TryGetStringField(
					TEXT("workspace_fingerprint"),
					WorkspaceFingerprint)
				|| !Root->TryGetStringField(
					TEXT("expected_revision"),
					ExpectedRevision)
				|| !Root->TryGetStringField(TEXT("intent"), Intent)
				|| !Root->TryGetBoolField(TEXT("dry_run"), bDryRun)
				|| CompositionKind != Request.GetCompositionKind()
				|| Target != Request.GetTargetAssetPath()
				|| WorkspaceFingerprint
					!= Request.GetWorkspace()->GetWorkspaceFingerprint()
				|| ExpectedRevision != Request.GetExpectedBaseRevision()
				|| Intent != LexToString(Request.GetIntent())
				|| bDryRun != Request.IsDryRun())
			{
				OutError = {};
				OutError.Code = TEXT(
					"graph_compound_hjson_canonical_input_mismatch");
				OutError.SubjectId = Request.GetTargetAssetPath();
				OutError.Detail = ParseError;
				return false;
			}
			return true;
		}

		bool SavePackage(
			UObject& Asset,
			FUeAgentHjsonImportTargetError& OutError)
		{
			UPackage* Package = Asset.GetOutermost();
			if (!Package
				|| !UEditorLoadingAndSavingUtils::SavePackages(
					{Package},
					false))
			{
				SetError(
					OutError,
					TEXT("graph_compound_hjson_package_save_failed"),
					Asset.GetPathName());
				return false;
			}
			return true;
		}

		bool UnloadAndReload(
			UObject*& Asset,
			const FString& AssetPath,
			UClass& ExpectedClass,
			UObject*& OutReloaded,
			FUeAgentHjsonImportTargetError& OutError)
		{
			OutReloaded = nullptr;
			UPackage* Package = Asset ? Asset->GetOutermost() : nullptr;
			if (!Package)
			{
				SetError(
					OutError,
					TEXT("graph_compound_hjson_package_unload_failed"),
					AssetPath,
					TEXT("package_missing"));
				return false;
			}
			const TWeakObjectPtr<UObject> Original(Asset);
			Asset = nullptr;
			FText UnloadError;
			const bool bUnloaded = UPackageTools::UnloadPackages(
				{Package},
				UnloadError,
				true);
			CollectGarbage(RF_NoFlags, true);
			if (!bUnloaded || Original.IsValid())
			{
				FString Detail = UnloadError.ToString();
				if (!bUnloaded)
				{
					Detail += Detail.IsEmpty()
						? TEXT("unload_api_failed")
						: TEXT("; unload_api_failed");
				}
				if (Original.IsValid())
				{
					Detail += FString::Printf(
						TEXT("%ssurviving_object=%s"),
						Detail.IsEmpty() ? TEXT("") : TEXT("; "),
						*AssetPath);
				}
				SetError(
					OutError,
					TEXT("graph_compound_hjson_package_unload_failed"),
					AssetPath,
					MoveTemp(Detail));
				return false;
			}
			OutReloaded = StaticLoadObject(
				&ExpectedClass,
				nullptr,
				*AssetPath);
			if (!OutReloaded)
			{
				SetError(
					OutError,
					TEXT("graph_compound_hjson_package_reload_failed"),
					AssetPath);
				return false;
			}
			return true;
		}

		class INativeTargetPort
		{
		public:
			virtual ~INativeTargetPort() = default;
			virtual const FString& GetCompositionKind() const = 0;
			virtual UClass& GetExpectedClass() const = 0;
			virtual const FString& GetFieldId() const = 0;
			virtual const FString& GetFieldSemanticId() const = 0;
			virtual bool TryNormalizeDesired(
				const FUeAgentCanonicalValue& Value,
				FString& OutNormalized,
				FUeAgentHjsonImportTargetError& OutError) const = 0;
			virtual bool TryApply(
				UObject& Asset,
				const FString& Normalized,
				FString& OutEvidence,
				FUeAgentHjsonImportTargetError& OutError) const = 0;
			virtual bool TryCompileAndSettle(
				UObject& Asset,
				TArray<FString>& OutEvidence,
				FUeAgentHjsonImportTargetError& OutError) const = 0;
			virtual bool TryCapturePersisted(
				const UObject& Asset,
				const FString& Normalized,
				bool bCompareDesired,
				TMap<FString, FString>& OutValues,
				FUeAgentHjsonImportTargetError& OutError) const = 0;
		};

		class FBlueprintTargetPort final : public INativeTargetPort
		{
		public:
			FBlueprintTargetPort(
				FString InCompositionKind,
				UClass& InExpectedClass)
				: CompositionKind(MoveTemp(InCompositionKind))
				, ExpectedClass(&InExpectedClass)
			{
			}

			const FString& GetCompositionKind() const override
			{
				return CompositionKind;
			}
			UClass& GetExpectedClass() const override
			{
				return *ExpectedClass;
			}
			const FString& GetFieldId() const override
			{
				static const FString Value(TEXT("blueprint_description"));
				return Value;
			}
			const FString& GetFieldSemanticId() const override
			{
				static const FString Value(
					TEXT("uai.semantic.blueprint.asset.property"));
				return Value;
			}
			bool TryNormalizeDesired(
				const FUeAgentCanonicalValue& Value,
				FString& OutNormalized,
				FUeAgentHjsonImportTargetError& OutError) const override
			{
				OutNormalized.Reset();
				if (Value.GetKind() != EUeAgentValueKind::String)
				{
					SetError(
						OutError,
						TEXT("graph_compound_blueprint_description_invalid"),
						GetFieldId());
					return false;
				}
				OutNormalized = Value.GetText();
				return true;
			}
			bool TryApply(
				UObject& Asset,
				const FString& Normalized,
				FString& OutEvidence,
				FUeAgentHjsonImportTargetError& OutError) const override
			{
				OutEvidence.Reset();
				UBlueprint* Blueprint = Cast<UBlueprint>(&Asset);
				DomainAdapters::BlueprintUmgGraph::
					FBlueprintUmgGraphAdapterError Error;
				DomainAdapters::BlueprintUmgGraph::
					FBlueprintUmgGraphNativeMutationReceipt Receipt;
				DomainAdapters::BlueprintUmgGraph::
					FBlueprintUmgGraphNativeHook Hook;
				if (!Blueprint
					|| !Hook.TrySetBlueprintDescription(
						*Blueprint,
						Normalized,
						Receipt,
						Error))
				{
					SetError(
						OutError,
						Error.GetCode().IsEmpty()
							? TEXT("graph_compound_blueprint_apply_failed")
							: Error.GetCode(),
						Asset.GetPathName(),
						Error.GetDetail());
					return false;
				}
				OutEvidence = Receipt.GetEvidenceId();
				return !OutEvidence.IsEmpty();
			}
			bool TryCompileAndSettle(
				UObject& Asset,
				TArray<FString>& OutEvidence,
				FUeAgentHjsonImportTargetError& OutError) const override
			{
				OutEvidence.Reset();
				UBlueprint* Blueprint = Cast<UBlueprint>(&Asset);
				DomainAdapters::BlueprintUmgGraph::
					FBlueprintUmgGraphAdapterError Error;
				DomainAdapters::BlueprintUmgGraph::
					FBlueprintUmgGraphCompileSettleReceipt Receipt;
				DomainAdapters::BlueprintUmgGraph::
					FBlueprintUmgGraphNativeHook Hook;
				if (!Blueprint
					|| !Hook.TryCompileAndSettle(
						*Blueprint,
						Receipt,
						Error))
				{
					SetError(
						OutError,
						Error.GetCode().IsEmpty()
							? TEXT("graph_compound_blueprint_settle_failed")
							: Error.GetCode(),
						Asset.GetPathName(),
						Error.GetDetail());
					return false;
				}
				OutEvidence = {
					Receipt.GetCompileEvidenceId(),
					Receipt.GetSettleEvidenceId(),
				};
				return Receipt.IsCompileComplete()
					&& Receipt.GetErrorCount() == 0;
			}
			bool TryCapturePersisted(
				const UObject& Asset,
				const FString& Normalized,
				const bool bCompareDesired,
				TMap<FString, FString>& OutValues,
				FUeAgentHjsonImportTargetError& OutError) const override
			{
				OutValues.Reset();
				const UBlueprint* Blueprint = Cast<UBlueprint>(&Asset);
				if (!Blueprint)
				{
					SetError(
						OutError,
						TEXT("graph_compound_blueprint_readback_invalid"),
						Asset.GetPathName());
					return false;
				}
				OutValues.Add(GetFieldId(), Blueprint->BlueprintDescription);
				if (bCompareDesired
					&& Blueprint->BlueprintDescription != Normalized)
				{
					SetError(
						OutError,
						TEXT("graph_compound_hjson_persisted_mismatch"),
						GetFieldId());
					return false;
				}
				return true;
			}

		private:
			FString CompositionKind;
			UClass* ExpectedClass = nullptr;
		};

		bool TryCompileNiagaraAsset(
			UObject& Asset,
			TArray<FString>& OutEvidence,
			FUeAgentHjsonImportTargetError& OutError)
		{
			OutEvidence.Reset();
			DomainAdapters::MaterialNiagaraGraph::
				FMaterialNiagaraGraphAdapterError Error;
			DomainAdapters::MaterialNiagaraGraph::
				FMaterialNiagaraGraphCompileSettleReceipt Receipt;
			DomainAdapters::MaterialNiagaraGraph::
				FMaterialNiagaraGraphNativeHook Hook;
			if (!Hook.TryCompileAndSettleNiagara(Asset, Receipt, Error))
			{
				SetError(
					OutError,
					Error.GetCode().IsEmpty()
						? TEXT("graph_compound_niagara_settle_failed")
						: Error.GetCode(),
					Asset.GetPathName(),
					Error.GetDetail());
				return false;
			}
			OutEvidence = {
				Receipt.GetCompileEvidenceId(),
				Receipt.GetSettleEvidenceId(),
			};
			return Receipt.IsCompilationComplete()
				&& Receipt.GetCompileErrorCount() == 0
				&& Receipt.GetStackIssueCount() == 0;
		}

		class FNiagaraSystemTargetPort final : public INativeTargetPort
		{
		public:
			const FString& GetCompositionKind() const override
			{
				static const FString Value(TEXT("ue.niagara_system.asset"));
				return Value;
			}
			UClass& GetExpectedClass() const override
			{
				return *UNiagaraSystem::StaticClass();
			}
			const FString& GetFieldId() const override
			{
				static const FString Value(TEXT("warmup_time"));
				return Value;
			}
			const FString& GetFieldSemanticId() const override
			{
				static const FString Value(
					TEXT("uai.semantic.niagara.system.property"));
				return Value;
			}
			bool TryNormalizeDesired(
				const FUeAgentCanonicalValue& Value,
				FString& OutNormalized,
				FUeAgentHjsonImportTargetError& OutError) const override
			{
				OutNormalized.Reset();
				if (Value.GetKind() != EUeAgentValueKind::Decimal
					&& Value.GetKind() != EUeAgentValueKind::Integer)
				{
					SetError(
						OutError,
						TEXT("graph_compound_niagara_warmup_invalid"),
						GetFieldId());
					return false;
				}
				double Number = 0.0;
				if (!LexTryParseString(Number, *Value.GetLexeme())
					|| !FMath::IsFinite(Number)
					|| Number < 0.0
					|| Number > MAX_flt)
				{
					SetError(
						OutError,
						TEXT("graph_compound_niagara_warmup_invalid"),
						GetFieldId());
					return false;
				}
				OutNormalized = FString::SanitizeFloat(
					static_cast<float>(Number));
				return true;
			}
			bool TryApply(
				UObject& Asset,
				const FString& Normalized,
				FString& OutEvidence,
				FUeAgentHjsonImportTargetError& OutError) const override
			{
				OutEvidence.Reset();
				UNiagaraSystem* System = Cast<UNiagaraSystem>(&Asset);
				float WarmupTime = 0.0f;
				DomainAdapters::MaterialNiagaraGraph::
					FMaterialNiagaraGraphAdapterError Error;
				DomainAdapters::MaterialNiagaraGraph::
					FMaterialNiagaraGraphNativeMutationReceipt Receipt;
				DomainAdapters::MaterialNiagaraGraph::
					FMaterialNiagaraGraphNativeHook Hook;
				if (!System
					|| !LexTryParseString(WarmupTime, *Normalized)
					|| !Hook.TrySetNiagaraSystemWarmupTime(
						*System,
						WarmupTime,
						Receipt,
						Error))
				{
					SetError(
						OutError,
						Error.GetCode().IsEmpty()
							? TEXT("graph_compound_niagara_apply_failed")
							: Error.GetCode(),
						Asset.GetPathName(),
						Error.GetDetail());
					return false;
				}
				OutEvidence = Receipt.GetEvidenceId();
				return !OutEvidence.IsEmpty();
			}
			bool TryCompileAndSettle(
				UObject& Asset,
				TArray<FString>& OutEvidence,
				FUeAgentHjsonImportTargetError& OutError) const override
			{
				return TryCompileNiagaraAsset(Asset, OutEvidence, OutError);
			}
			bool TryCapturePersisted(
				const UObject& Asset,
				const FString& Normalized,
				const bool bCompareDesired,
				TMap<FString, FString>& OutValues,
				FUeAgentHjsonImportTargetError& OutError) const override
			{
				OutValues.Reset();
				const UNiagaraSystem* System = Cast<UNiagaraSystem>(&Asset);
				float Expected = 0.0f;
				if (!System || !LexTryParseString(Expected, *Normalized))
				{
					SetError(
						OutError,
						TEXT("graph_compound_niagara_readback_invalid"),
						Asset.GetPathName());
					return false;
				}
				OutValues.Add(
					GetFieldId(),
					FString::SanitizeFloat(System->GetWarmupTime()));
				const float WarmupTickDelta = System->GetWarmupTickDelta();
				const float ExpectedPersisted =
					FMath::IsNearlyZero(WarmupTickDelta)
						? Expected
						: WarmupTickDelta * FMath::FloorToInt(
							Expected / WarmupTickDelta);
				if (bCompareDesired
					&& !FMath::IsNearlyEqual(
						System->GetWarmupTime(),
						ExpectedPersisted))
				{
					SetError(
						OutError,
						TEXT("graph_compound_hjson_persisted_mismatch"),
						GetFieldId(),
						FString::Printf(
							TEXT("expected=%.9g;actual=%.9g;tick_delta=%.9g"),
							ExpectedPersisted,
							System->GetWarmupTime(),
							WarmupTickDelta));
					return false;
				}
				return true;
			}
		};

		class FNiagaraEmitterTargetPort final : public INativeTargetPort
		{
		public:
			const FString& GetCompositionKind() const override
			{
				static const FString Value(TEXT("ue.niagara_emitter.asset"));
				return Value;
			}
			UClass& GetExpectedClass() const override
			{
				return *UNiagaraEmitter::StaticClass();
			}
			const FString& GetFieldId() const override
			{
				static const FString Value(TEXT("local_space"));
				return Value;
			}
			const FString& GetFieldSemanticId() const override
			{
				static const FString Value(
					TEXT("uai.semantic.niagara.emitter.asset.property"));
				return Value;
			}
			bool TryNormalizeDesired(
				const FUeAgentCanonicalValue& Value,
				FString& OutNormalized,
				FUeAgentHjsonImportTargetError& OutError) const override
			{
				OutNormalized.Reset();
				if (Value.GetKind() != EUeAgentValueKind::Boolean)
				{
					SetError(
						OutError,
						TEXT("graph_compound_niagara_emitter_local_space_invalid"),
						GetFieldId());
					return false;
				}
				OutNormalized = Value.GetBoolean() ? TEXT("true") : TEXT("false");
				return true;
			}
			bool TryApply(
				UObject& Asset,
				const FString& Normalized,
				FString& OutEvidence,
				FUeAgentHjsonImportTargetError& OutError) const override
			{
				OutEvidence.Reset();
				UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(&Asset);
				DomainAdapters::MaterialNiagaraGraph::
					FMaterialNiagaraGraphAdapterError Error;
				DomainAdapters::MaterialNiagaraGraph::
					FMaterialNiagaraGraphNativeMutationReceipt Receipt;
				DomainAdapters::MaterialNiagaraGraph::
					FMaterialNiagaraGraphNativeHook Hook;
				if (!Emitter
					|| (Normalized != TEXT("true") && Normalized != TEXT("false"))
					|| !Hook.TrySetNiagaraEmitterLocalSpace(
						*Emitter,
						Normalized == TEXT("true"),
						Receipt,
						Error))
				{
					SetError(
						OutError,
						Error.GetCode().IsEmpty()
							? TEXT("graph_compound_niagara_emitter_apply_failed")
							: Error.GetCode(),
						Asset.GetPathName(),
						Error.GetDetail());
					return false;
				}
				OutEvidence = Receipt.GetEvidenceId();
				return !OutEvidence.IsEmpty();
			}
			bool TryCompileAndSettle(
				UObject& Asset,
				TArray<FString>& OutEvidence,
				FUeAgentHjsonImportTargetError& OutError) const override
			{
				return TryCompileNiagaraAsset(Asset, OutEvidence, OutError);
			}
			bool TryCapturePersisted(
				const UObject& Asset,
				const FString& Normalized,
				const bool bCompareDesired,
				TMap<FString, FString>& OutValues,
				FUeAgentHjsonImportTargetError& OutError) const override
			{
				OutValues.Reset();
				const UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(&Asset);
				const FVersionedNiagaraEmitterData* Data = Emitter
					? Emitter->GetLatestEmitterData()
					: nullptr;
				if (!Data
					|| (Normalized != TEXT("true") && Normalized != TEXT("false")))
				{
					SetError(
						OutError,
						TEXT("graph_compound_niagara_emitter_readback_invalid"),
						Asset.GetPathName());
					return false;
				}
				const bool bExpected = Normalized == TEXT("true");
				OutValues.Add(
					GetFieldId(),
					Data->bLocalSpace ? TEXT("true") : TEXT("false"));
				if (bCompareDesired && Data->bLocalSpace != bExpected)
				{
					SetError(
						OutError,
						TEXT("graph_compound_hjson_persisted_mismatch"),
						GetFieldId());
					return false;
				}
				return true;
			}
		};

		class FNiagaraScriptTargetPort final : public INativeTargetPort
		{
		public:
			const FString& GetCompositionKind() const override
			{
				static const FString Value(TEXT("ue.niagara_script.asset"));
				return Value;
			}
			UClass& GetExpectedClass() const override
			{
				return *UNiagaraScript::StaticClass();
			}
			const FString& GetFieldId() const override
			{
				static const FString Value(TEXT("description"));
				return Value;
			}
			const FString& GetFieldSemanticId() const override
			{
				static const FString Value(
					TEXT("uai.semantic.niagara.script.asset.property"));
				return Value;
			}
			bool TryNormalizeDesired(
				const FUeAgentCanonicalValue& Value,
				FString& OutNormalized,
				FUeAgentHjsonImportTargetError& OutError) const override
			{
				OutNormalized.Reset();
				if (Value.GetKind() != EUeAgentValueKind::String)
				{
					SetError(
						OutError,
						TEXT("graph_compound_niagara_script_description_invalid"),
						GetFieldId());
					return false;
				}
				OutNormalized = Value.GetText();
				return true;
			}
			bool TryApply(
				UObject& Asset,
				const FString& Normalized,
				FString& OutEvidence,
				FUeAgentHjsonImportTargetError& OutError) const override
			{
				OutEvidence.Reset();
				UNiagaraScript* Script = Cast<UNiagaraScript>(&Asset);
				DomainAdapters::MaterialNiagaraGraph::
					FMaterialNiagaraGraphAdapterError Error;
				DomainAdapters::MaterialNiagaraGraph::
					FMaterialNiagaraGraphNativeMutationReceipt Receipt;
				DomainAdapters::MaterialNiagaraGraph::
					FMaterialNiagaraGraphNativeHook Hook;
				if (!Script
					|| !Hook.TrySetNiagaraScriptDescription(
						*Script,
						FText::FromString(Normalized),
						Receipt,
						Error))
				{
					SetError(
						OutError,
						Error.GetCode().IsEmpty()
							? TEXT("graph_compound_niagara_script_apply_failed")
							: Error.GetCode(),
						Asset.GetPathName(),
						Error.GetDetail());
					return false;
				}
				OutEvidence = Receipt.GetEvidenceId();
				return !OutEvidence.IsEmpty();
			}
			bool TryCompileAndSettle(
				UObject& Asset,
				TArray<FString>& OutEvidence,
				FUeAgentHjsonImportTargetError& OutError) const override
			{
				return TryCompileNiagaraAsset(Asset, OutEvidence, OutError);
			}
			bool TryCapturePersisted(
				const UObject& Asset,
				const FString& Normalized,
				const bool bCompareDesired,
				TMap<FString, FString>& OutValues,
				FUeAgentHjsonImportTargetError& OutError) const override
			{
				OutValues.Reset();
				const UNiagaraScript* Script = Cast<UNiagaraScript>(&Asset);
				const FVersionedNiagaraScriptData* Data = Script
					? Script->GetLatestScriptData()
					: nullptr;
				if (!Data)
				{
					SetError(
						OutError,
						TEXT("graph_compound_niagara_script_readback_invalid"),
						Asset.GetPathName());
					return false;
				}
				const FString Persisted = Data->Description.ToString();
				OutValues.Add(GetFieldId(), Persisted);
				if (bCompareDesired && Persisted != Normalized)
				{
					SetError(
						OutError,
						TEXT("graph_compound_hjson_persisted_mismatch"),
						GetFieldId());
					return false;
				}
				return true;
			}
		};

		bool UnitEnvelopeMatches(
			const FSemanticUnit& Current,
			const FSemanticUnit& Edited)
		{
			return Current.GetUnitId() == Edited.GetUnitId()
				&& Current.GetUnitKind() == Edited.GetUnitKind()
				&& Current.GetOwnerRef() == Edited.GetOwnerRef()
				&& Current.GetSemanticTypeRef()
					== Edited.GetSemanticTypeRef()
				&& Current.GetRole() == Edited.GetRole()
				&& Current.GetFieldSetRef() == Edited.GetFieldSetRef()
				&& Current.GetItemRefs() == Edited.GetItemRefs()
				&& Current.GetRelationRefs() == Edited.GetRelationRefs()
				&& Current.GetOrderingRef() == Edited.GetOrderingRef()
				&& Current.GetLayoutRef() == Edited.GetLayoutRef()
				&& Current.GetDependencyRefs()
					== Edited.GetDependencyRefs()
				&& Current.GetProfileVisibility()
					== Edited.GetProfileVisibility()
				&& Current.GetSourceProvenance()
					== Edited.GetSourceProvenance()
				&& Current.GetTargetIdentityKey()
					== Edited.GetTargetIdentityKey();
		}

		FString DescribeUnitEnvelopeMismatch(
			const FSemanticUnit& Current,
			const FSemanticUnit& Edited)
		{
			TArray<FString> Differences;
			if (Current.GetUnitId() != Edited.GetUnitId()) Differences.Add(TEXT("unit_id"));
			if (Current.GetUnitKind() != Edited.GetUnitKind()) Differences.Add(TEXT("unit_kind"));
			if (Current.GetOwnerRef() != Edited.GetOwnerRef()) Differences.Add(TEXT("owner_ref"));
			if (Current.GetSemanticTypeRef() != Edited.GetSemanticTypeRef()) Differences.Add(TEXT("semantic_type_ref"));
			if (Current.GetRole() != Edited.GetRole()) Differences.Add(TEXT("role"));
			if (Current.GetFieldSetRef() != Edited.GetFieldSetRef()) Differences.Add(TEXT("field_set_ref"));
			if (Current.GetItemRefs() != Edited.GetItemRefs()) Differences.Add(TEXT("item_refs"));
			if (Current.GetRelationRefs() != Edited.GetRelationRefs()) Differences.Add(TEXT("relation_refs"));
			if (Current.GetOrderingRef() != Edited.GetOrderingRef()) Differences.Add(TEXT("ordering_ref"));
			if (Current.GetLayoutRef() != Edited.GetLayoutRef()) Differences.Add(TEXT("layout_ref"));
			if (Current.GetDependencyRefs() != Edited.GetDependencyRefs()) Differences.Add(TEXT("dependency_refs"));
			if (Current.GetProfileVisibility() != Edited.GetProfileVisibility()) Differences.Add(TEXT("profile_visibility"));
			if (Current.GetSourceProvenance() != Edited.GetSourceProvenance()) Differences.Add(TEXT("source_provenance"));
			if (Current.GetTargetIdentityKey() != Edited.GetTargetIdentityKey()) Differences.Add(TEXT("target_identity_key"));
			return FString::Join(Differences, TEXT(","));
		}

		const FUeAgentCanonicalValue* DirectValue(
			const FSemanticUnit& Unit,
			const FString& FieldId)
		{
			const FUeAgentCanonicalValueRef* Value =
				Unit.GetValues().Find(FieldId);
			return Value ? &Value->Get() : nullptr;
		}

		const FUeAgentCanonicalValueRef* EditableFieldValue(
			const FSemanticUnit& Unit,
			const FString& FieldId)
		{
			if (const FUeAgentCanonicalValueRef* External =
				Unit.GetValues().Find(FieldId))
			{
				return External;
			}
			if (Unit.GetValues().Num() == 1)
			{
				for (const TPair<FString, FUeAgentCanonicalValueRef>& Value
					: Unit.GetValues())
				{
					return &Value.Value;
				}
			}
			return nullptr;
		}

		bool ValidateTargetUnit(
			const FAuthoringModel& Model,
			const FUeAgentHjsonTargetImportRequest& Request,
			FUeAgentHjsonImportTargetError& OutError)
		{
			if (Model.GetTargetUnitIds().Num() != 1)
			{
				SetError(
					OutError,
					TEXT("graph_compound_hjson_target_count_invalid"),
					Request.GetCompositionKind());
				return false;
			}
			const FSemanticUnit* Target =
				Model.FindUnit(Model.GetTargetUnitIds()[0]);
			const FUeAgentCanonicalValue* AssetPath = Target
				? DirectValue(*Target, TEXT("asset_path"))
				: nullptr;
			if (!Target
				|| Target->GetUnitKind()
					!= ESemanticUnitKind::TargetState
				|| Target->GetSemanticTypeRef()
					!= Request.GetCompositionKind()
				|| !AssetPath
				|| AssetPath->GetKind() != EUeAgentValueKind::String
				|| AssetPath->GetText() != Request.GetTargetAssetPath())
			{
				SetError(
					OutError,
					TEXT("graph_compound_hjson_target_identity_mismatch"),
					Request.GetTargetAssetPath());
				return false;
			}
			return true;
		}

		const FSemanticUnit* FindEditableFieldUnit(
			const FAuthoringModel& Model,
			const INativeTargetPort& Port)
		{
			const FSemanticUnit* Found = nullptr;
			for (const FSemanticUnit& Unit : Model.GetUnits())
			{
				if (Unit.GetUnitKind()
						== ESemanticUnitKind::PropertySet
					&& Unit.GetRole() == TEXT("target_settings")
					&& Unit.GetFieldSetRef() == Port.GetFieldId()
					&& EditableFieldValue(Unit, Port.GetFieldId()))
				{
					if (Found)
					{
						return nullptr;
					}
					Found = &Unit;
				}
			}
			return Found;
		}

		FString DescribeFieldSearch(
			const FAuthoringModel& Model,
			const INativeTargetPort& Port)
		{
			int32 PropertySets = 0;
			int32 TargetSettings = 0;
			int32 MatchingFieldSets = 0;
			TArray<FString> Candidates;
			for (const FSemanticUnit& Unit : Model.GetUnits())
			{
				if (Unit.GetUnitKind() != ESemanticUnitKind::PropertySet)
				{
					continue;
				}
				++PropertySets;
				if (Unit.GetRole() == TEXT("target_settings"))
				{
					++TargetSettings;
				}
				if (Unit.GetFieldSetRef() == Port.GetFieldId())
				{
					++MatchingFieldSets;
				}
				if (Candidates.Num() < 6
					&& (Unit.GetRole() == TEXT("target_settings")
						|| Unit.GetFieldSetRef() == Port.GetFieldId()))
				{
					TArray<FString> Keys;
					Unit.GetValues().GetKeys(Keys);
					Keys.Sort();
					Candidates.Add(
						Unit.GetRole() + TEXT("|")
						+ Unit.GetFieldSetRef() + TEXT("|")
						+ Unit.GetSemanticTypeRef() + TEXT("|")
						+ FString::Join(Keys, TEXT(",")));
				}
			}
			return FString::Printf(
				TEXT("property_sets=%d;target_settings=%d;field_sets=%d;candidates=%s"),
				PropertySets,
				TargetSettings,
				MatchingFieldSets,
				*FString::Join(Candidates, TEXT(";")));
		}

		const FUeAgentStateEntry* FindFieldStateEntry(
			const FUeAgentCanonicalSnapshot& Snapshot,
			const INativeTargetPort& Port,
			const FString& TargetPath)
		{
			const FUeAgentStateEntry* Found = nullptr;
			for (const FUeAgentStateEntry& Entry : Snapshot.GetEntries())
			{
				const FUeAgentCanonicalIdentity& Identity =
					Entry.GetIdentity();
				if (Identity.GetSemanticId()
						== Port.GetFieldSemanticId()
					&& Identity.GetOwnerId() == TargetPath
					&& Identity.GetSelector(TEXT("property_path"))
						== Port.GetFieldId())
				{
					if (Found)
					{
						return nullptr;
					}
					Found = &Entry;
				}
			}
			return Found;
		}

		class FGraphCompoundHjsonOperation final
			: public IUeAgentHjsonImportOperation
		{
		public:
			class FState final
			{
			public:
				FState(
					FUeAgentHjsonTargetImportRequest InRequest,
					TSharedRef<const INativeTargetPort> InPort)
					: Request(MoveTemp(InRequest))
					, Port(MoveTemp(InPort))
				{
				}

				FUeAgentHjsonTargetImportRequest Request;
				TSharedRef<const INativeTargetPort> Port;
				const FUeAgentOperationContext* OperationContext = nullptr;
				uint8 CompletedStages = 0;
				UObject* Asset = nullptr;
				FUeAgentCanonicalSnapshotPtr CurrentSnapshot;
				FAuthoringModelPtr CurrentModel;
				TOptional<FUeAgentCanonicalIdentity> Identity;
				FUeAgentCanonicalValuePtr BeforeValue;
				FUeAgentCanonicalValuePtr AfterValue;
				FString DesiredValue;
				FString PreRevision;
				FString PersistedRevision;
				bool bMutationApplied = false;
				bool bCompileSettled = false;
				bool bPackageSaved = false;
				bool bUnloadedReloaded = false;
				TMap<FString, FString> PersistedValues;
				TArray<FString> EvidenceIds;
				FUeAgentHjsonTargetImportReceipt ImportReceipt;
			};

			static bool TryCreate(
				FUeAgentHjsonTargetImportRequest Request,
				TSharedRef<const INativeTargetPort> Port,
				TSharedPtr<IUeAgentHjsonImportOperation>& OutOperation,
				FUeAgentHjsonImportTargetError& OutError)
			{
				OutOperation.Reset();
				OutError = {};
				if (!Request.IsValid()
					|| Request.GetCompositionKind()
						!= Port->GetCompositionKind()
					|| !GetHjsonImportDescriptorIdentity(
						Request.GetCompositionKind()).IsValid())
				{
					SetError(
						OutError,
						TEXT("graph_compound_hjson_operation_request_invalid"),
						Request.GetCompositionKind());
					return false;
				}
				OutOperation = MakeShareable(
					new FGraphCompoundHjsonOperation(
						MakeShared<FState, ESPMode::ThreadSafe>(
							MoveTemp(Request),
							MoveTemp(Port))));
				return true;
			}

			bool TryPrepare(
				const Contract::FUeAgentContractValue& CanonicalInput,
				const TArray<FUeAgentOperationTargetBinding>& RequestedTargets,
				const FUeAgentOperationContext& Context,
				TOptional<FPlanningRequest>& OutPlanning,
				FUeAgentOperationBindingError& OutError) override
			{
				OutPlanning.Reset();
				OutError = {};
				if (!BindStage(Context, 0x01, OutError)
					|| !Checkpoint(
						Context,
						TEXT("graph_compound_hjson.prepare"),
						OutError)
					|| !RequestedTargets.IsEmpty()
					|| Context.GetDescriptorIdentity()
						!= GetHjsonImportDescriptorIdentity(
							State->Request.GetCompositionKind())
					|| !ValidateCanonicalInput(
						CanonicalInput,
						State->Request,
						OutError)
					|| !PrepareState(Context, OutError))
				{
					if (!OutError.IsValid())
					{
						OutError.Code = TEXT(
							"graph_compound_hjson_preparation_invalid");
						OutError.SubjectId =
							State->Request.GetTargetAssetPath();
					}
					return false;
				}

				FChangeDraft Change(
					State->Identity.GetValue(),
					State->Request.GetIntent(),
					State->BeforeValue,
					State->AfterValue,
					{
						TEXT("uai:authoring:hjson:import"),
					},
					TEXT("graph-compound-hjson-impact:")
						+ State->Request.GetWorkspace()
							->GetWorkspaceFingerprint(),
					{});
				FChangeSetDraft Draft(
					Context.GetDescriptorIdentity(),
					Context.GetRequestId(),
					Context.GetIdempotencyKey(),
					State->CurrentSnapshot.ToSharedRef(),
					{MoveTemp(Change)});
				OutPlanning.Emplace(
					MoveTemp(Draft),
					Context.GetCallerContext(),
					State->Request.IsDryRun());
				if (!OutPlanning->IsValid())
				{
					OutPlanning.Reset();
					OutError.Code = TEXT(
						"graph_compound_hjson_planning_request_invalid");
					OutError.SubjectId = Context.GetRequestId();
					return false;
				}
				return true;
			}

			bool TryPlan(
				const FPlanningRequest& Request,
				const FUeAgentOperationContext& Context,
				FPlanningResult& OutResult,
				FUeAgentOperationBindingError& OutError) override
			{
				OutResult = {};
				OutError = {};
				if (!BindStage(Context, 0x02, OutError)
					|| !Checkpoint(
						Context,
						TEXT("graph_compound_hjson.plan"),
						OutError))
				{
					return false;
				}
				FPlanningPreflightError Error;
				if (!TryPlanChanges(
						Request,
						Context.GetResourceTracker(),
						OutResult,
						Error))
				{
					OutError.Code = Error.GetCode();
					OutError.SubjectId = Error.GetIdentityKey();
					OutError.Detail = Error.GetDetail();
					return false;
				}
				return true;
			}

			bool TryExecute(
				const FPlanningResult& Planning,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FExecutionReceipt& OutReceipt,
				FUeAgentOperationBindingError& OutError) override
			{
				OutReceipt = {};
				OutError = {};
				if (!BindStage(Context, 0x04, OutError)
					|| !Checkpoint(
						Context,
						TEXT("graph_compound_hjson.execute"),
						OutError)
					|| !Planning.IsReady()
					|| !Planning.GetPlan().IsValid()
					|| Targets.Num() != 1
					|| !State->Identity.IsSet()
					|| Targets[0].GetBinding().GetSummary().GetIdentity()
						.GetCanonicalKey()
						!= State->Identity->GetCanonicalKey()
					|| !State->Asset)
				{
					if (!OutError.IsValid())
					{
						OutError.Code = TEXT(
							"graph_compound_hjson_execution_contract_invalid");
						OutError.SubjectId = Context.GetRequestId();
					}
					return false;
				}

				const bool bMutate = Planning.IsMutationPermitted();
				if (bMutate)
				{
					FUeAgentHjsonImportTargetError NativeError;
					FString ApplyEvidence;
					TArray<FString> SettleEvidence;
					if (!State->Port->TryApply(
							*State->Asset,
							State->DesiredValue,
							ApplyEvidence,
							NativeError)
						|| !State->Port->TryCompileAndSettle(
							*State->Asset,
							SettleEvidence,
							NativeError))
					{
						CopyError(NativeError, OutError);
						return false;
					}
					State->bMutationApplied = true;
					State->bCompileSettled = true;
					State->EvidenceIds.Add(ApplyEvidence);
					State->EvidenceIds.Append(SettleEvidence);
					if (!SavePackage(*State->Asset, NativeError))
					{
						CopyError(NativeError, OutError);
						return false;
					}
					State->bPackageSaved = true;
				}

				const FString OperationId =
					TEXT("hjson-import:") + Context.GetRequestId();
				OutReceipt = FExecutionReceipt(
					OperationId,
					EExecutionPhase::Terminal,
					bMutate
						? EExecutionStatus::PersistedUnverified
						: EExecutionStatus::AppliedInMemory,
					FString(),
					EExecutionCancellationMode::NotRequested,
					bMutate,
					{},
					!bMutate,
					{},
					bMutate
						? TArray<FString>({
							TEXT("executing"),
							TEXT("settling"),
							TEXT("persisting"),
							TEXT("terminal"),
						})
						: TArray<FString>({
							TEXT("executing"),
							TEXT("terminal"),
						}));
				return OutReceipt.IsValid();
			}

			bool TryVerify(
				const FExecutionReceipt& Receipt,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentOperationBindingError& OutError) override
			{
				OutOutcome = {};
				OutError = {};
				if (!BindStage(Context, 0x08, OutError)
					|| !Checkpoint(
						Context,
						TEXT("graph_compound_hjson.verify"),
						OutError)
					|| !Receipt.IsValid()
					|| Targets.Num() != 1
					|| !State->Identity.IsSet()
					|| Targets[0].GetBinding().GetSummary().GetIdentity()
						.GetCanonicalKey()
						!= State->Identity->GetCanonicalKey()
					|| !State->Asset)
				{
					if (!OutError.IsValid())
					{
						OutError.Code = TEXT(
							"graph_compound_hjson_verification_contract_invalid");
						OutError.SubjectId = Context.GetRequestId();
					}
					return false;
				}

				UObject* ObservedAsset = State->Asset;
				if (State->bMutationApplied)
				{
					FUeAgentHjsonImportTargetError ReloadError;
					if (!UnloadAndReload(
							State->Asset,
							State->Request.GetTargetAssetPath(),
							State->Port->GetExpectedClass(),
							ObservedAsset,
							ReloadError))
					{
						CopyError(ReloadError, OutError);
						return false;
					}
					State->Asset = ObservedAsset;
					State->bUnloadedReloaded = true;
				}

				FUeAgentHjsonImportTargetError NativeError;
				if (!State->Port->TryCapturePersisted(
						*ObservedAsset,
						State->DesiredValue,
						!State->Request.IsDryRun(),
						State->PersistedValues,
						NativeError))
				{
					CopyError(NativeError, OutError);
					return false;
				}
				DomainAdapters::ContentGraphRead::
					FUeAgentContentGraphCapture PersistedCapture;
				DomainAdapters::ContentGraphRead::
					FUeAgentContentGraphReadError CaptureError;
				if (!DomainAdapters::ContentGraphRead::
					FUeAgentContentGraphReadProductionEnvironment::
					TryCaptureOwner(
						*ObservedAsset,
						PersistedCapture,
						CaptureError))
				{
					OutError.Code = CaptureError.Code;
					OutError.SubjectId = CaptureError.SubjectId;
					OutError.Detail = CaptureError.Detail;
					return false;
				}
				State->PersistedRevision = PersistedCapture.Revision;
				if (State->Request.IsDryRun()
					&& State->PersistedRevision != State->PreRevision)
				{
					OutError.Code = TEXT(
						"graph_compound_hjson_dry_run_mutated_target");
					OutError.SubjectId =
						State->Request.GetTargetAssetPath();
					return false;
				}

				State->ImportReceipt = FUeAgentHjsonTargetImportReceipt(
					State->Request.GetCompositionKind(),
					State->Request.GetTargetAssetPath(),
					State->Request.GetWorkspace()->GetWorkspaceFingerprint(),
					State->PreRevision,
					State->PersistedRevision,
					State->bMutationApplied,
					State->bCompileSettled,
					State->bPackageSaved,
					State->bUnloadedReloaded,
					State->PersistedValues);
				if (!State->ImportReceipt.IsValid())
				{
					OutError.Code = TEXT(
						"graph_compound_hjson_import_receipt_invalid");
					OutError.SubjectId =
						State->Request.GetTargetAssetPath();
					return false;
				}

				const FString EvidenceId =
					TEXT("graph-compound-hjson-persisted:")
						+ State->PersistedRevision;
				State->EvidenceIds.Add(EvidenceId);
				State->EvidenceIds.Add(
					TEXT("hjson-import-context-shared"));
				OutOutcome = FVerificationOutcome(
					TEXT("graph-compound-hjson-verify:")
						+ Context.GetRequestId(),
					EVerificationStatus::Verified,
					EEffectOutcome::Complete,
					State->Request.IsDryRun()
						? TEXT("dry_run_verified")
						: FString(),
					false,
					{
						FTargetVerification(
							State->Identity.GetValue(),
							EVerificationOracleKind::Persisted,
							EVerificationStatus::Verified,
							TEXT("hjson-import-expected:")
								+ State->Request.GetWorkspace()
									->GetWorkspaceFingerprint(),
							TEXT("hjson-import-actual:")
								+ State->PersistedRevision,
							FUeAgentSemanticComparison(),
							true,
							{EvidenceId}),
					},
					State->EvidenceIds,
					{});
				return true;
			}

			const FUeAgentHjsonTargetImportReceipt&
			GetImportReceipt() const override
			{
				return State->ImportReceipt;
			}

			bool UsedOneOperationContext() const override
			{
				return State->OperationContext != nullptr
					&& State->CompletedStages == 0x0f;
			}

		private:
			explicit FGraphCompoundHjsonOperation(
				TSharedRef<FState, ESPMode::ThreadSafe> InState)
				: State(MoveTemp(InState))
			{
			}

			bool BindStage(
				const FUeAgentOperationContext& Context,
				const uint8 StageBit,
				FUeAgentOperationBindingError& OutError)
			{
				if (!State->OperationContext)
				{
					State->OperationContext = &Context;
				}
				if (State->OperationContext != &Context
					|| (State->CompletedStages & StageBit) != 0)
				{
					OutError = {};
					OutError.Code = TEXT(
						"graph_compound_hjson_operation_context_mismatch");
					OutError.SubjectId = Context.GetRequestId();
					OutError.Detail = TEXT(
						"prepare, plan, execute and verify must share one "
						"request-scoped OperationContext exactly once.");
					return false;
				}
				State->CompletedStages |= StageBit;
				return true;
			}

			bool PrepareState(
				const FUeAgentOperationContext& Context,
				FUeAgentOperationBindingError& OutError)
			{
				const FUeAgentHjsonTargetImportRequest& Request =
					State->Request;
				const FResolvedSemanticWorkspace& Workspace =
					*Request.GetWorkspace();
				FUeAgentHjsonImportTargetError Error;
				if (!Workspace.GetStructuralEdits().IsEmpty())
				{
					SetError(
						Error,
						TEXT("graph_compound_hjson_structural_edit_unsupported"),
						Request.GetCompositionKind(),
						TEXT(
							"No Graph/Compound structural operation target is "
							"active for this exact edit yet."));
					CopyError(Error, OutError);
					return false;
				}

				State->Asset = StaticLoadObject(
					&State->Port->GetExpectedClass(),
					nullptr,
					*Request.GetTargetAssetPath());
				if (!State->Asset
					|| !State->Asset->IsA(
						&State->Port->GetExpectedClass()))
				{
					SetError(
						Error,
						TEXT("graph_compound_hjson_target_not_found"),
						Request.GetTargetAssetPath(),
						State->Port->GetExpectedClass().GetPathName());
					CopyError(Error, OutError);
					return false;
				}

				DomainAdapters::ContentGraphRead::
					FUeAgentContentGraphCapture Capture;
				DomainAdapters::ContentGraphRead::
					FUeAgentContentGraphReadError CaptureError;
				if (!DomainAdapters::ContentGraphRead::
					FUeAgentContentGraphReadProductionEnvironment::
					TryCaptureOwner(
						*State->Asset,
						Capture,
						CaptureError)
					|| !DomainAdapters::ContentGraphRead::
						TryBuildContentGraphCompoundSnapshot(
							Capture,
							Context,
							State->CurrentSnapshot,
							CaptureError)
					|| !State->CurrentSnapshot.IsValid())
				{
					OutError.Code = CaptureError.Code;
					OutError.SubjectId = CaptureError.SubjectId;
					OutError.Detail = CaptureError.Detail;
					return false;
				}

				State->PreRevision =
					State->CurrentSnapshot->GetRevision().GetGeneration();
				if (State->PreRevision != Request.GetExpectedBaseRevision()
					|| Workspace.GetBaseStateRevision().GetGeneration()
						!= Request.GetExpectedBaseRevision()
					|| Workspace.GetModel()->GetBaseRevision()
						!= Request.GetExpectedBaseRevision())
				{
					SetError(
						Error,
						TEXT("graph_compound_hjson_base_revision_mismatch"),
						Request.GetTargetAssetPath(),
						State->PreRevision);
					CopyError(Error, OutError);
					return false;
				}

				FWorkspaceError WorkspaceError;
				if (!TryComposeSnapshotAuthoringModel(
						FSnapshotCompositionRequest(
							Request.GetCompositionKind(),
							Workspace.GetModel()->GetProfileId(),
							State->CurrentSnapshot.ToSharedRef()),
						Context.GetResourceTracker(),
						State->CurrentModel,
						WorkspaceError)
					|| !State->CurrentModel.IsValid())
				{
					OutError.Code = WorkspaceError.GetCode();
					OutError.SubjectId = WorkspaceError.GetSourceUnit();
					OutError.Detail = WorkspaceError.GetJsonPointer();
					return false;
				}

				const FAuthoringModel& Current = *State->CurrentModel;
				const FAuthoringModel& Edited = *Workspace.GetModel();
				if (Current.GetContractId() != Edited.GetContractId()
					|| Current.GetContractVersion()
						!= Edited.GetContractVersion()
					|| Current.GetProfileId() != Edited.GetProfileId()
					|| !ValidateTargetUnit(Current, Request, Error)
					|| !ValidateTargetUnit(Edited, Request, Error))
				{
					if (!Error.IsValid())
					{
						SetError(
							Error,
							TEXT("graph_compound_hjson_contract_mismatch"),
							Request.GetCompositionKind());
					}
					CopyError(Error, OutError);
					return false;
				}

				const FSemanticUnit* CurrentField =
					FindEditableFieldUnit(Current, *State->Port);
				const FSemanticUnit* EditedField = CurrentField
					? Edited.FindUnit(CurrentField->GetUnitId())
					: nullptr;
				if (!CurrentField
					|| !EditedField
					|| !UnitEnvelopeMatches(*CurrentField, *EditedField))
				{
					const FString Detail = !CurrentField
						? TEXT("current_field_missing:")
							+ DescribeFieldSearch(Current, *State->Port)
						: !EditedField
							? TEXT("edited_field_missing:")
								+ CurrentField->GetUnitId()
							: DescribeUnitEnvelopeMismatch(
								*CurrentField,
								*EditedField);
					SetError(
						Error,
						TEXT("graph_compound_hjson_field_owner_invalid"),
						State->Port->GetFieldId(),
						Detail);
					CopyError(Error, OutError);
					return false;
				}

				if (Current.GetUnits().Num() != Edited.GetUnits().Num())
				{
					SetError(
						Error,
						TEXT("graph_compound_hjson_unit_set_changed"),
						Request.GetCompositionKind());
					CopyError(Error, OutError);
					return false;
				}
				for (const FSemanticUnit& CurrentUnit : Current.GetUnits())
				{
					const FSemanticUnit* EditedUnit =
						Edited.FindUnit(CurrentUnit.GetUnitId());
					if (!EditedUnit
						|| !UnitEnvelopeMatches(CurrentUnit, *EditedUnit)
						|| (CurrentUnit.GetUnitId()
								!= CurrentField->GetUnitId()
							&& SemanticUnitContentHash(CurrentUnit)
								!= SemanticUnitContentHash(*EditedUnit)))
					{
						SetError(
							Error,
							TEXT("graph_compound_hjson_unsupported_change"),
							CurrentUnit.GetUnitId(),
							TEXT(
								"Only the exact active field target may differ; "
								"other unit, relation, ordering and layout state "
								"must remain unchanged."));
						CopyError(Error, OutError);
						return false;
					}
				}

				const FUeAgentCanonicalValueRef* DesiredRef = EditableFieldValue(
					*EditedField,
					State->Port->GetFieldId());
				const FUeAgentCanonicalValue* Desired = DesiredRef
					? &DesiredRef->Get()
					: nullptr;
				const FUeAgentStateEntry* CurrentEntry =
					FindFieldStateEntry(
						*State->CurrentSnapshot,
						*State->Port,
						Request.GetTargetAssetPath());
				if (!Desired || !CurrentEntry
					|| !State->Port->TryNormalizeDesired(
						*Desired,
						State->DesiredValue,
						Error))
				{
					if (!Error.IsValid())
					{
						SetError(
							Error,
							TEXT("graph_compound_hjson_field_value_invalid"),
							State->Port->GetFieldId());
					}
					CopyError(Error, OutError);
					return false;
				}
				State->Identity.Emplace(CurrentEntry->GetIdentity());
				State->BeforeValue = CurrentEntry->GetValue();
				State->AfterValue = *DesiredRef;
				return State->Identity->IsValid()
					&& State->BeforeValue.IsValid()
					&& State->AfterValue.IsValid();
			}

			TSharedRef<FState, ESPMode::ThreadSafe> State;
		};
	}

	const TArray<FString>& GetGraphCompoundHjsonCompositionKinds()
	{
		static const TArray<FString> Kinds = {
			TEXT("ue.blueprint.asset"),
			TEXT("ue.widget_blueprint.asset"),
			TEXT("ue.niagara_system.asset"),
			TEXT("ue.niagara_emitter.asset"),
			TEXT("ue.niagara_script.asset"),
		};
		return Kinds;
	}

	bool TryCreateBlueprintHjsonImportOperation(
		FUeAgentHjsonTargetImportRequest Request,
		TSharedPtr<IUeAgentHjsonImportOperation>& OutOperation,
		FUeAgentHjsonImportTargetError& OutError)
	{
		return FGraphCompoundHjsonOperation::TryCreate(
			MoveTemp(Request),
			MakeShared<FBlueprintTargetPort>(
				TEXT("ue.blueprint.asset"),
				*UBlueprint::StaticClass()),
			OutOperation,
			OutError);
	}

	bool TryCreateWidgetBlueprintHjsonImportOperation(
		FUeAgentHjsonTargetImportRequest Request,
		TSharedPtr<IUeAgentHjsonImportOperation>& OutOperation,
		FUeAgentHjsonImportTargetError& OutError)
	{
		return FGraphCompoundHjsonOperation::TryCreate(
			MoveTemp(Request),
			MakeShared<FBlueprintTargetPort>(
				TEXT("ue.widget_blueprint.asset"),
				*UWidgetBlueprint::StaticClass()),
			OutOperation,
			OutError);
	}

	bool TryCreateNiagaraSystemHjsonImportOperation(
		FUeAgentHjsonTargetImportRequest Request,
		TSharedPtr<IUeAgentHjsonImportOperation>& OutOperation,
		FUeAgentHjsonImportTargetError& OutError)
	{
		return FGraphCompoundHjsonOperation::TryCreate(
			MoveTemp(Request),
			MakeShared<FNiagaraSystemTargetPort>(),
			OutOperation,
			OutError);
	}

	bool TryCreateNiagaraEmitterHjsonImportOperation(
		FUeAgentHjsonTargetImportRequest Request,
		TSharedPtr<IUeAgentHjsonImportOperation>& OutOperation,
		FUeAgentHjsonImportTargetError& OutError)
	{
		return FGraphCompoundHjsonOperation::TryCreate(
			MoveTemp(Request),
			MakeShared<FNiagaraEmitterTargetPort>(),
			OutOperation,
			OutError);
	}

	bool TryCreateNiagaraScriptHjsonImportOperation(
		FUeAgentHjsonTargetImportRequest Request,
		TSharedPtr<IUeAgentHjsonImportOperation>& OutOperation,
		FUeAgentHjsonImportTargetError& OutError)
	{
		return FGraphCompoundHjsonOperation::TryCreate(
			MoveTemp(Request),
			MakeShared<FNiagaraScriptTargetPort>(),
			OutOperation,
			OutError);
	}
}
