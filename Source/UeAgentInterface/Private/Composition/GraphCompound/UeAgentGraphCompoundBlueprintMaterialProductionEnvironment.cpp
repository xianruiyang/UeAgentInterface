#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialBindings.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundNiagaraLifecycleProduction.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundNiagaraTypedMutationProduction.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Composition/HjsonImport/UeAgentHjsonImportOperationRuntime.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/Verification/UeAgentIndependentReadback.h"
#include "DomainAdapters/BlueprintCommands/UeAgentBlueprintCommandMechanicsBoundary.h"
#include "DomainAdapters/BlueprintUmgGraph/UeAgentBlueprintUmgGraphNativeHook.h"
#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphNativeHook.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringState.h"
#include "Domains/Material/Authoring/UeAgentMaterialGraphAuthoring.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Texture.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace AssetDurable;
	using namespace ContextResource;
	using namespace Contract;
	using namespace DomainAdapters::BlueprintUmgGraph;
	using namespace DomainAdapters::MaterialNiagaraGraph;
	using namespace HjsonImport;
	using namespace HjsonWorkspace;
	using namespace PlanningPreflight;
	using namespace StateSemantic;
	using namespace Verification;

	namespace
	{
		const FString AssetCreateReadbackOracleId =
			TEXT("uai.readback.graph_compound.asset_create");
		const FString MaterialMutationReadbackOracleId =
			TEXT("uai.readback.graph_compound.material_mutation");

		void SetError(
			FGraphCompoundRuntimeError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		bool TryResolveHjsonWorkspace(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundHjsonSourceRequest& Request,
			const FString& RequestId,
			FUeAgentResourceTracker& Resources,
			FResolvedSemanticWorkspacePtr& OutWorkspace,
			FGraphCompoundRuntimeError& OutError)
		{
			OutWorkspace.Reset();
			TOptional<FWorkspaceSource> Source;
			switch (Request.GetSourceKind())
			{
			case EGraphCompoundHjsonSourceKind::Directory:
				Source.Emplace(FWorkspaceSource::Directory(
					Request.GetSourceLocator()));
				break;
			case EGraphCompoundHjsonSourceKind::SingleFile:
				Source.Emplace(FWorkspaceSource::SingleFile(
					Request.GetSourceLocator()));
				break;
			case EGraphCompoundHjsonSourceKind::Inline:
				Source.Emplace(FWorkspaceSource::Inline(
					Request.GetInlineHjson(),
					TEXT("index.hjson"),
					TEXT("graph-compound:") + RequestId));
				break;
			default:
				break;
			}
			if (!Source.IsSet())
			{
				SetError(OutError,
					TEXT("graph_compound_hjson_source_invalid"),
					Profile.GetCapabilityId());
				return false;
			}

			FWorkspaceError WorkspaceError;
			if (!TryResolveEditedSemanticWorkspace(
					Source.GetValue(),
					ESemanticWorkspaceMode::State,
					Resources,
					OutWorkspace,
					WorkspaceError)
				|| !OutWorkspace.IsValid())
			{
				SetError(OutError,
					WorkspaceError.GetCode().IsEmpty()
						? TEXT("graph_compound_hjson_workspace_invalid")
						: WorkspaceError.GetCode(),
					WorkspaceError.GetSourceUnit(),
					WorkspaceError.GetJsonPointer());
				return false;
			}
			return true;
		}

		bool TrySerializeHjsonResult(
			const FGraphCompoundHjsonSourceRequest& Request,
			const FUeAgentHjsonImportC3OResult& OperationResult,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			const FUeAgentHjsonTargetImportReceipt& Receipt =
				OperationResult.ImportReceipt;
			const TSharedRef<FJsonObject> Import = MakeShared<FJsonObject>();
			Import->SetStringField(
				TEXT("composition_kind"), Request.GetCompositionKind());
			Import->SetStringField(TEXT("target"), Request.GetTargetRef());
			Import->SetStringField(
				TEXT("workspace_fingerprint"),
				Receipt.GetWorkspaceFingerprint());
			Import->SetStringField(
				TEXT("pre_revision"), Receipt.GetPreRevision());
			Import->SetStringField(
				TEXT("persisted_revision"), Receipt.GetPersistedRevision());
			Import->SetBoolField(
				TEXT("mutation_applied"), Receipt.WasMutationApplied());
			Import->SetBoolField(
				TEXT("compile_settled"), Receipt.WasCompileSettled());
			Import->SetBoolField(
				TEXT("package_saved"), Receipt.WasPackageSaved());
			Import->SetBoolField(
				TEXT("unloaded_reloaded"), Receipt.WasUnloadedReloaded());
			Import->SetStringField(
				TEXT("operation_id"), OperationResult.OperationId);
			Import->SetStringField(
				TEXT("plan_fingerprint"), OperationResult.PlanFingerprint);
			Import->SetStringField(
				TEXT("verification_status"),
				OperationResult.VerificationStatus);
			Import->SetBoolField(TEXT("dry_run"), Request.IsDryRun());

			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetObjectField(TEXT("import"), Import);
			FString Canonical;
			FString Error;
			if (!Json::TryCanonicalize(Root, Canonical, Error))
			{
				SetError(OutError,
					TEXT("graph_compound_hjson_result_invalid"),
					Request.GetTargetRef(), Error);
				return false;
			}
			OutResult = FUeAgentContractValue(MoveTemp(Canonical));
			return true;
		}

		FString ObjectPath(const FGraphCompoundAssetCreateRequest& Request)
		{
			return Request.GetTargetRef() + TEXT(".") + Request.GetAssetName();
		}

		bool TryPackageFilename(
			const FString& PackageName,
			FString& OutFilename)
		{
			OutFilename.Reset();
			return FPackageName::IsValidLongPackageName(PackageName)
				&& !FPackageName::GetPackageMountPoint(PackageName).IsNone()
				&& FPackageName::TryConvertLongPackageNameToFilename(
					PackageName,
					OutFilename,
					FPackageName::GetAssetPackageExtension());
		}

		bool TryCheckTargetAbsent(
			const FGraphCompoundAssetCreateRequest& Request,
			FString& OutFilename,
			FGraphCompoundRuntimeError& OutError)
		{
			if (!Request.IsValid()
				|| !TryPackageFilename(Request.GetTargetRef(), OutFilename))
			{
				SetError(OutError,
					TEXT("graph_compound_asset_target_invalid"),
					Request.GetTargetRef());
				return false;
			}
			if (FindPackage(nullptr, *Request.GetTargetRef())
				|| IFileManager::Get().FileExists(*OutFilename))
			{
				SetError(OutError,
					TEXT("graph_compound_asset_target_already_exists"),
					Request.GetTargetRef());
				return false;
			}
			return true;
		}

		bool TryCreateAbsentSnapshot(
			const FGraphCompoundAssetCreateRequest& Request,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError)
		{
			OutSnapshot.Reset();
			FString Filename;
			if (!TryCheckTargetAbsent(Request, Filename, OutError))
			{
				return false;
			}
			const FUeAgentStateRevision Revision(
				TEXT("graph-compound:c4:asset-create"),
				TEXT("absent:") + Request.GetTargetRef(),
				0);
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("graph-compound:c4:absent:")
						+ Request.GetTargetRef(),
					EUeAgentStatePlane::Persisted,
					Revision,
					FUeAgentTemporalPoint(
						TEXT("utc-seconds"),
						FString::Printf(TEXT("%lld"),
							FDateTime::UtcNow().ToUnixTimestamp())),
					FUeAgentStateProvenance(
						TEXT("independent_readback"),
						Request.GetTargetRef(),
						TEXT("file-absence:") + Filename),
					{},
					Resources,
					OutSnapshot,
					Error))
			{
				SetError(OutError, Error.Code, Request.GetTargetRef(),
					Error.Path);
				return false;
			}
			return OutSnapshot.IsValid();
		}

		UObject* FindTopLevelAsset(
			UPackage& Package,
			const FString& AssetName)
		{
			UObject* const Asset = FindObject<UObject>(&Package, *AssetName);
			return Asset && Asset->GetOutermost() == &Package ? Asset : nullptr;
		}

		bool TryUnloadCreatedPackage(
			const FGraphCompoundAssetCreateRequest& Request,
			const bool bNotifyAssetRegistry,
			FString& OutDetail)
		{
			OutDetail.Reset();
			UPackage* const Package =
				FindPackage(nullptr, *Request.GetTargetRef());
			if (!Package)
			{
				return true;
			}
			if (bNotifyAssetRegistry)
			{
				if (UObject* const Asset =
					FindTopLevelAsset(*Package, Request.GetAssetName()))
				{
					FAssetRegistryModule::AssetDeleted(Asset);
				}
			}
			Package->SetDirtyFlag(false);
			FText Error;
			if (!UPackageTools::UnloadPackages({Package}, Error, true))
			{
				OutDetail = Error.ToString();
				return false;
			}
			CollectGarbage(RF_NoFlags, true);
			if (FindPackage(nullptr, *Request.GetTargetRef()))
			{
				OutDetail = TEXT("package remains loaded after unload");
				return false;
			}
			return true;
		}

		FString ExpectedDefinition(
			const FGraphCompoundAssetCreateRequest& Request)
		{
			switch (Request.GetInputKind())
			{
			case EGraphCompoundTypedInputKind::BlueprintAssetCreate:
				return TEXT("blueprint:") + Request.GetParentClassRef();
			case EGraphCompoundTypedInputKind::UmgAssetCreate:
				return TEXT("widget_blueprint");
			case EGraphCompoundTypedInputKind::MaterialAssetCreate:
				return Request.GetMaterialKind();
			case EGraphCompoundTypedInputKind::MaterialInstanceAssetCreate:
				return TEXT("material_instance:")
					+ Request.GetParentMaterialRef();
			default:
				return FString();
			}
		}

		bool TryValidateReloadedAsset(
			const FGraphCompoundAssetCreateRequest& Request,
			FString& OutDiagnostic)
		{
			OutDiagnostic.Reset();
			UObject* const Asset =
				LoadObject<UObject>(nullptr, *ObjectPath(Request));
			if (!Asset)
			{
				OutDiagnostic = TEXT("graph_compound_asset_reload_failed");
				return false;
			}
			if (Asset->GetOutermost()->IsDirty())
			{
				OutDiagnostic =
					TEXT("graph_compound_asset_reload_state_mismatch");
				return false;
			}
			if (Request.GetInputKind()
				== EGraphCompoundTypedInputKind::MaterialAssetCreate)
			{
				const UClass* const ExpectedClass =
					Request.GetMaterialKind() == TEXT("material_function")
						? UMaterialFunction::StaticClass()
						: UMaterial::StaticClass();
				if (Asset->GetClass() != ExpectedClass)
				{
					OutDiagnostic =
						TEXT("graph_compound_asset_reload_class_mismatch");
					return false;
				}
				return true;
			}
			if (Request.GetInputKind()
				== EGraphCompoundTypedInputKind::MaterialInstanceAssetCreate)
			{
				UMaterialInstanceConstant* const MaterialInstance =
					Cast<UMaterialInstanceConstant>(Asset);
				UMaterialInterface* const ExpectedParent =
					LoadObject<UMaterialInterface>(
						nullptr, *Request.GetParentMaterialRef());
				if (!MaterialInstance
					|| Asset->GetClass()
						!= UMaterialInstanceConstant::StaticClass()
					|| !ExpectedParent
					|| MaterialInstance->Parent != ExpectedParent)
				{
					OutDiagnostic =
						TEXT("graph_compound_asset_reload_state_mismatch");
					return false;
				}
				return true;
			}

			UBlueprint* const Blueprint = Cast<UBlueprint>(Asset);
			const bool bBlueprintRoute = Request.GetInputKind()
				== EGraphCompoundTypedInputKind::BlueprintAssetCreate;
			if (!Blueprint
				|| (bBlueprintRoute
					&& Asset->GetClass() != UBlueprint::StaticClass())
				|| (!bBlueprintRoute
					&& Asset->GetClass() != UWidgetBlueprint::StaticClass()))
			{
				OutDiagnostic =
					TEXT("graph_compound_asset_reload_class_mismatch");
				return false;
			}
			UClass* const ExpectedParent = bBlueprintRoute
				? LoadObject<UClass>(nullptr, *Request.GetParentClassRef())
				: UUserWidget::StaticClass();
			if (!ExpectedParent || Blueprint->ParentClass != ExpectedParent
				|| !Blueprint->GeneratedClass
				|| !Blueprint->GeneratedClass->IsChildOf(ExpectedParent)
				|| Blueprint->Status != BS_UpToDate)
			{
				OutDiagnostic =
					TEXT("graph_compound_asset_reload_state_mismatch");
				return false;
			}
			if (!bBlueprintRoute
				&& !CastChecked<UWidgetBlueprint>(Blueprint)->WidgetTree)
			{
				OutDiagnostic =
					TEXT("graph_compound_widget_tree_reload_missing");
				return false;
			}
			return true;
		}

		class FAssetCreateReadbackOracle final
			: public IIndependentReadbackOracle
		{
		public:
			explicit FAssetCreateReadbackOracle(
				FGraphCompoundAssetCreateRequest InRequest)
				: Request(MoveTemp(InRequest))
			{
			}

			const FString& GetOracleId() const override
			{
				return AssetCreateReadbackOracleId;
			}

			bool TryRead(
				const FIndependentReadbackRequest& ReadbackRequest,
				FUeAgentResourceTracker& Resources,
				FIndependentReadbackObservation& OutObservation,
				FVerificationError& OutError) override
			{
				OutObservation = FIndependentReadbackObservation();
				OutError = FVerificationError();
				FString Detail;
				if (!TryUnloadCreatedPackage(Request, false, Detail))
				{
					OutObservation =
						FIndependentReadbackObservation::Unavailable(
							TEXT("graph_compound_asset_unload_failed"),
							Detail);
					return true;
				}
				FString Diagnostic;
				if (!TryValidateReloadedAsset(Request, Diagnostic))
				{
					OutObservation =
						FIndependentReadbackObservation::Unavailable(
							Diagnostic,
							TEXT("reload:") + Request.GetTargetRef());
					return true;
				}
				FUeAgentCanonicalValuePtr Actual;
				FUeAgentStateSemanticError Error;
				if (!FUeAgentCanonicalValue::TryString(
						ExpectedDefinition(Request), Resources, Actual, Error)
					|| !Actual.IsValid())
				{
					OutError = FVerificationError(Error.Code, Error.Path);
					return false;
				}
				OutObservation = FIndependentReadbackObservation::Available(
					Actual.ToSharedRef(),
					TEXT("reload:") + ReadbackRequest.GetSubjectRef()
						+ TEXT(":")
						+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
				return true;
			}

		private:
			FGraphCompoundAssetCreateRequest Request;
		};

		FAssetDurableSessionRef PersistedSession(
			const FAssetDurableSessionRef& Prepared,
			const FDurableReceipt& Receipt)
		{
			return MakeShared<const FAssetDurableSession>(
				Prepared->GetRequest(),
				Prepared->GetInventory(),
				Prepared->GetJournalId(),
				Prepared->GetJournalPath(),
				Prepared->GetJournalRoot(),
				Prepared->GetTransactionRoot(),
				Prepared->GetStagingRoot(),
				Receipt.GetPhaseHistory());
		}

		bool TryRollbackCreate(
			const FGraphCompoundAssetCreateRequest& Request,
			const bool bNotifyAssetRegistry,
			const FAssetDurableSessionRef& Session,
			FPackageAssetDurableBackend& Backend,
			const int64 FencingToken,
			FUeAgentResourceTracker& Resources,
			FString& OutDetail)
		{
			FString UnloadDetail;
			const bool bMemoryClean = TryUnloadCreatedPackage(
				Request, bNotifyAssetRegistry, UnloadDetail);
			FDurableReceipt RollbackReceipt;
			FAssetDurableError Error;
			const bool bDurableClean = TryRollbackAssetDurability(
				Session, Backend, FencingToken, bMemoryClean,
				Resources, RollbackReceipt, Error);
			OutDetail = !UnloadDetail.IsEmpty()
				? UnloadDetail
				: Error.GetCode() + TEXT(":") + Error.GetDetail();
			return bMemoryClean && bDurableClean;
		}

		FString ParameterSemanticId(
			const FMaterialNiagaraGraphParameterIdentity& Identity)
		{
			FString Type;
			switch (Identity.GetParameterType())
			{
			case EMaterialNiagaraGraphParameterType::Scalar:
				Type = TEXT("scalar");
				break;
			case EMaterialNiagaraGraphParameterType::Vector:
				Type = TEXT("vector");
				break;
			case EMaterialNiagaraGraphParameterType::Texture:
				Type = TEXT("texture");
				break;
			case EMaterialNiagaraGraphParameterType::StaticSwitch:
				Type = TEXT("static_switch");
				break;
			case EMaterialNiagaraGraphParameterType::StaticComponentMask:
				Type = TEXT("static_component_mask");
				break;
			}
			return FString::Printf(
				TEXT("property:%s:%s:%d:%s"),
				*Identity.GetParameterName(),
				*Identity.GetParameterAssociation(),
				Identity.GetParameterIndex(),
				*Type);
		}

		bool TryCanonicalNumber(
			const double Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError,
			const FString& Subject)
		{
			FUeAgentStateSemanticError Error;
			if (!FMath::IsFinite(Value)
				|| !FUeAgentCanonicalValue::TryDecimal(
					FString::SanitizeFloat(Value),
					Resources, OutValue, Error))
			{
				SetError(OutError,
					Error.Code.IsEmpty()
						? TEXT("graph_compound_material_number_invalid")
						: Error.Code,
					Subject, Error.Detail);
				return false;
			}
			return true;
		}

		bool TryCanonicalColor(
			const FLinearColor& Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError,
			const FString& Subject)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const double Component : {
				static_cast<double>(Value.R),
				static_cast<double>(Value.G),
				static_cast<double>(Value.B),
				static_cast<double>(Value.A)})
			{
				FUeAgentCanonicalValuePtr Item;
				if (!TryCanonicalNumber(
						Component, Resources, Item, OutError, Subject))
				{
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryArray(
					Items, Resources, OutValue, Error))
			{
				SetError(OutError, Error.Code, Subject, Error.Detail);
				return false;
			}
			return true;
		}

		bool TryCanonicalEffectiveParameter(
			const FMaterialNiagaraGraphEffectiveParameter& Parameter,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError)
		{
			const FString Subject =
				Parameter.GetIdentity().GetParameterKey();
			FUeAgentStateSemanticError Error;
			switch (Parameter.GetIdentity().GetParameterType())
			{
			case EMaterialNiagaraGraphParameterType::Scalar:
				return TryCanonicalNumber(
					Parameter.GetScalarValue(), Resources,
					OutValue, OutError, Subject);
			case EMaterialNiagaraGraphParameterType::Vector:
				return TryCanonicalColor(
					Parameter.GetVectorValue(), Resources,
					OutValue, OutError, Subject);
			case EMaterialNiagaraGraphParameterType::Texture:
				if (FUeAgentCanonicalValue::TryString(
						Parameter.GetTextureRef(), Resources,
						OutValue, Error))
				{
					return true;
				}
				break;
			case EMaterialNiagaraGraphParameterType::StaticSwitch:
				if (FUeAgentCanonicalValue::TryBoolean(
						Parameter.GetBooleanValue(), Resources,
						OutValue, Error))
				{
					return true;
				}
				break;
			default:
				SetError(OutError,
					TEXT("graph_compound_material_parameter_type_unsupported"),
					Subject);
				return false;
			}
			SetError(OutError, Error.Code, Subject, Error.Detail);
			return false;
		}

		bool TryCanonicalEffectiveParameterState(
			const FMaterialNiagaraGraphEffectiveParameter& Parameter,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError)
		{
			FUeAgentCanonicalValuePtr EffectiveValue;
			if (!TryCanonicalEffectiveParameter(
					Parameter, Resources, EffectiveValue, OutError))
			{
				return false;
			}
			FUeAgentCanonicalValuePtr OverrideState;
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryString(
					Parameter.IsOverridden()
						? TEXT("overridden") : TEXT("inherited"),
					Resources, OverrideState, Error)
				|| !FUeAgentCanonicalValue::TryObject(
					{{TEXT("effective_value"), EffectiveValue.ToSharedRef()},
						{TEXT("override_state"), OverrideState.ToSharedRef()}},
					Resources, OutValue, Error))
			{
				SetError(OutError, Error.Code,
					Parameter.GetIdentity().GetParameterKey(), Error.Path);
				return false;
			}
			return OutValue.IsValid();
		}

		bool TryAppendMutationStateEntry(
			const FString& TargetKind,
			const FString& TargetRef,
			const FString& SemanticId,
			const FString& Generation,
			const FUeAgentCanonicalValueRef& Value,
			const bool bOverridden,
			TArray<FUeAgentStateEntry>& OutEntries,
			FGraphCompoundRuntimeError& OutError)
		{
			const FString ContainerKind =
				Value->GetKind() == EUeAgentValueKind::Array
					? TEXT("array")
					: Value->GetKind() == EUeAgentValueKind::Object
						? TEXT("object")
						: TEXT("scalar");
			FUeAgentStateEntry Entry(
				FUeAgentCanonicalIdentity(
					SemanticId, TargetKind, TargetRef, SemanticId,
					TEXT("graph_compound"), Generation),
				FUeAgentCanonicalType(
					TEXT("uai.graph_compound.state.v1"),
					Value->GetKind(), false, ContainerKind),
				Value,
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::DomainNative,
					TargetRef,
					TEXT("independent material readback"),
					Value),
				FUeAgentStateDetails(
					SemanticId,
					TEXT("GraphCompound independent current state"),
					{TEXT("GraphCompound")}, true,
					TEXT("available"), {}),
				FUeAgentStateRevision(
					TEXT("graph-compound:c4:material"), Generation, 0),
				bOverridden
					? EUeAgentAuthoredPresence::Present
					: EUeAgentAuthoredPresence::Absent,
				bOverridden
					? EUeAgentOverrideState::Overridden
					: EUeAgentOverrideState::Inherited);
			if (!Entry.IsValid())
			{
				SetError(OutError,
					TEXT("graph_compound_material_state_entry_invalid"),
					SemanticId);
				return false;
			}
			OutEntries.Add(MoveTemp(Entry));
			return true;
		}

		UObject* TryResolveMaterialMutationTarget(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FString& TargetRef,
			FGraphCompoundRuntimeError& OutError)
		{
			UObject* const Target =
				LoadObject<UObject>(nullptr, *TargetRef);
			const bool bValid =
				(Profile.GetTargetKind() == TEXT("material")
					&& Cast<UMaterial>(Target))
				|| (Profile.GetTargetKind() == TEXT("material_function")
					&& Cast<UMaterialFunction>(Target))
				|| (Profile.GetTargetKind() == TEXT("material_instance")
					&& Cast<UMaterialInstanceConstant>(Target));
			if (!Target || !bValid || Target->GetPathName() != TargetRef)
			{
				SetError(OutError,
					TEXT("graph_compound_material_target_invalid"),
					TargetRef, Profile.GetTargetKind());
				return nullptr;
			}
			return Target;
		}

		bool TryCaptureMaterialMutationSnapshot(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FString& TargetRef,
			FMaterialNiagaraGraphNativeHook& Hook,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError)
		{
			OutSnapshot.Reset();
			UObject* const Target =
				TryResolveMaterialMutationTarget(Profile, TargetRef, OutError);
			if (!Target)
			{
				return false;
			}
			FString Generation;
			FString Evidence;
			TArray<FUeAgentStateEntry> Entries;
			FMaterialNiagaraGraphAdapterError NativeError;
			if (UMaterialInstanceConstant* const Instance =
				Cast<UMaterialInstanceConstant>(Target))
			{
				TArray<FMaterialNiagaraGraphEffectiveParameter> Parameters;
				if (!Hook.TryCaptureMaterialInstanceEffectiveParameters(
						*Instance, Parameters, Generation,
						Evidence, NativeError))
				{
					SetError(OutError, NativeError.GetCode(),
						TargetRef, NativeError.GetDetail());
					return false;
				}
				for (const FMaterialNiagaraGraphEffectiveParameter& Parameter
					: Parameters)
				{
					FUeAgentCanonicalValuePtr Value;
					if (!TryCanonicalEffectiveParameterState(
							Parameter, Resources, Value, OutError)
						|| !TryAppendMutationStateEntry(
							Profile.GetTargetKind(), TargetRef,
							ParameterSemanticId(Parameter.GetIdentity()),
							Generation, Value.ToSharedRef(),
							Parameter.IsOverridden(), Entries, OutError))
					{
						return false;
					}
				}
			}
			else
			{
				FMaterialNiagaraGraphTopology Topology;
				if (!Hook.TryCaptureMaterialGraph(
						*Target, TargetRef, Topology,
						Generation, Evidence, NativeError))
				{
					SetError(OutError, NativeError.GetCode(),
						TargetRef, NativeError.GetDetail());
					return false;
				}
				const FMaterialExpressionCollection* const Collection =
					UeAgentMaterialAuthoring::GetExpressionCollection(*Target);
				for (const FMaterialNiagaraGraphItemIdentity& Item
					: Topology.GetItems())
				{
					FString Definition = Item.GetStableBuiltinRole();
					if (!Item.GetNativeGuid().IsEmpty() && Collection)
					{
						FGuid Guid;
						if (!FGuid::Parse(Item.GetNativeGuid(), Guid))
						{
							SetError(OutError,
								TEXT("graph_compound_material_item_identity_invalid"),
								Item.GetItemKey());
							return false;
						}
						for (UMaterialExpression* const Expression
							: Collection->Expressions)
						{
							if (Expression
								&& Expression->MaterialExpressionGuid == Guid)
							{
								Definition = Expression->GetClass()->GetPathName();
								break;
							}
						}
					}
					FUeAgentCanonicalValuePtr Value;
					FUeAgentStateSemanticError StateError;
					if (Definition.IsEmpty()
						|| !FUeAgentCanonicalValue::TryString(
							Definition, Resources, Value, StateError)
						|| !TryAppendMutationStateEntry(
							Profile.GetTargetKind(), TargetRef,
							Item.GetItemKey(), Generation,
							Value.ToSharedRef(), true, Entries, OutError))
					{
						if (OutError.Code.IsEmpty())
						{
							SetError(OutError, StateError.Code,
								Item.GetItemKey(), StateError.Detail);
						}
						return false;
					}
				}
			}

			const FUeAgentStateRevision Revision(
				TEXT("graph-compound:c4:material"), Generation, 0);
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("graph-compound:c4:material:") + TargetRef,
					EUeAgentStatePlane::Persisted,
					Revision,
					FUeAgentTemporalPoint(
						TEXT("utc-seconds"),
						FString::Printf(TEXT("%lld"),
							FDateTime::UtcNow().ToUnixTimestamp())),
					FUeAgentStateProvenance(
						TEXT("independent_readback"),
						TargetRef, Evidence),
					MoveTemp(Entries), Resources,
					OutSnapshot, StateError))
			{
				SetError(OutError, StateError.Code,
					TargetRef, StateError.Detail);
				return false;
			}
			return OutSnapshot.IsValid();
		}

		enum class EMaterialMutationOperationKind : uint8
		{
			Item,
			Property,
			Relation,
			Parameter,
		};

		struct FMaterialMutationOperation
		{
			EMaterialMutationOperationKind Kind =
				EMaterialMutationOperationKind::Item;
			FString SemanticId;
			EChangeIntent Intent = EChangeIntent::Update;
			FUeAgentCanonicalValuePtr Value;
			TArray<FString> Dependencies;
			FString ClassRef;
			FGuid ExpressionGuid;
			int32 PositionX = 0;
			int32 PositionY = 0;
			FString OwnerItemRef;
			FString PropertyPath;
			FString ValueType;
			FString FromItemRef;
			int32 FromOutputIndex = INDEX_NONE;
			FString ToItemRef;
			FString ToInputRef;
			FMaterialParameterInfo ParameterInfo;
			EMaterialNiagaraGraphParameterType ParameterType =
				EMaterialNiagaraGraphParameterType::Scalar;
		};

		const FUeAgentCanonicalValue* FindObjectValue(
			const FUeAgentCanonicalValue& Object,
			const FString& Field)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				if (Item.Key == Field)
				{
					return &Item.Value.Get();
				}
			}
			return nullptr;
		}

		bool HasExactObjectFields(
			const FUeAgentCanonicalValue& Object,
			std::initializer_list<const TCHAR*> Fields)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object
				|| Object.GetObjectItems().Num()
					!= static_cast<int32>(Fields.size()))
			{
				return false;
			}
			for (const TCHAR* const Field : Fields)
			{
				if (!FindObjectValue(Object, Field))
				{
					return false;
				}
			}
			return true;
		}

		bool TryCanonicalStringField(
			const FUeAgentCanonicalValue& Object,
			const TCHAR* Field,
			FString& OutValue)
		{
			const FUeAgentCanonicalValue* const Value =
				FindObjectValue(Object, Field);
			if (!Value || Value->GetKind() != EUeAgentValueKind::String)
			{
				OutValue.Reset();
				return false;
			}
			OutValue = Value->GetText();
			return !OutValue.IsEmpty();
		}

		bool TryCanonicalNumberValue(
			const FUeAgentCanonicalValue& Value,
			double& OutNumber)
		{
			if (Value.GetKind() != EUeAgentValueKind::Integer
				&& Value.GetKind() != EUeAgentValueKind::Decimal)
			{
				return false;
			}
			OutNumber = FCString::Atod(*Value.GetLexeme());
			return FMath::IsFinite(OutNumber);
		}

		bool TryCanonicalIntegerField(
			const FUeAgentCanonicalValue& Object,
			const TCHAR* Field,
			int32& OutValue)
		{
			const FUeAgentCanonicalValue* const Value =
				FindObjectValue(Object, Field);
			double Number = 0.0;
			if (!Value || !TryCanonicalNumberValue(*Value, Number)
				|| Number < MIN_int32 || Number > MAX_int32
				|| !FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number)))
			{
				return false;
			}
			OutValue = static_cast<int32>(Number);
			return true;
		}

		bool TryCanonicalLinearColor(
			const FUeAgentCanonicalValue& Value,
			FLinearColor& OutColor)
		{
			if (Value.GetKind() != EUeAgentValueKind::Array
				|| Value.GetArrayItems().Num() != 4)
			{
				return false;
			}
			double Components[4] = {};
			for (int32 Index = 0; Index < 4; ++Index)
			{
				if (!TryCanonicalNumberValue(
						Value.GetArrayItems()[Index].Get(), Components[Index]))
				{
					return false;
				}
			}
			OutColor = FLinearColor(
				Components[0], Components[1], Components[2], Components[3]);
			return true;
		}

		const FUeAgentCanonicalValue* FindObjectValue(
			const FUeAgentCanonicalValue& Object,
			const FString& Field);

		bool HasExactObjectFields(
			const FUeAgentCanonicalValue& Object,
			std::initializer_list<const TCHAR*> Fields);

		const FUeAgentCanonicalValue* TryGetParameterEffectiveValue(
			const FUeAgentCanonicalValue& State)
		{
			if (!HasExactObjectFields(
					State, {TEXT("effective_value"), TEXT("override_state")}))
			{
				return nullptr;
			}
			const FUeAgentCanonicalValue* const OverrideState =
				FindObjectValue(State, TEXT("override_state"));
			if (!OverrideState
				|| OverrideState->GetKind() != EUeAgentValueKind::String
				|| OverrideState->GetText() != TEXT("overridden"))
			{
				return nullptr;
			}
			return FindObjectValue(State, TEXT("effective_value"));
		}

		UMaterialExpression* FindExpressionBySemanticId(
			UObject& Owner,
			const FString& SemanticId)
		{
			if (!SemanticId.StartsWith(TEXT("expression:")))
			{
				return nullptr;
			}
			FGuid Guid;
			if (!FGuid::Parse(SemanticId.RightChop(11), Guid))
			{
				return nullptr;
			}
			const FMaterialExpressionCollection* const Collection =
				UeAgentMaterialAuthoring::GetExpressionCollection(Owner);
			if (!Collection)
			{
				return nullptr;
			}
			for (UMaterialExpression* const Expression : Collection->Expressions)
			{
				if (Expression && Expression->MaterialExpressionGuid == Guid)
				{
					return Expression;
				}
			}
			return nullptr;
		}

		bool TryParseParameterSemanticId(
			const FString& SemanticId,
			FMaterialParameterInfo& OutInfo,
			EMaterialNiagaraGraphParameterType& OutType)
		{
			if (!SemanticId.StartsWith(TEXT("property:")))
			{
				return false;
			}
			FString PrefixAndName;
			FString Type;
			if (!SemanticId.Split(TEXT(":"), &PrefixAndName, &Type,
				ESearchCase::CaseSensitive, ESearchDir::FromEnd))
			{
				return false;
			}
			FString PrefixNameAssociation;
			FString IndexText;
			if (!PrefixAndName.Split(TEXT(":"), &PrefixNameAssociation, &IndexText,
				ESearchCase::CaseSensitive, ESearchDir::FromEnd))
			{
				return false;
			}
			FString PrefixAndNameOnly;
			FString Association;
			if (!PrefixNameAssociation.Split(TEXT(":"), &PrefixAndNameOnly,
				&Association, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
				|| !PrefixAndNameOnly.StartsWith(TEXT("property:")))
			{
				return false;
			}
			const FString Name = PrefixAndNameOnly.RightChop(9);
			int32 Index = INDEX_NONE;
			if (Name.IsEmpty() || !LexTryParseString(Index, *IndexText))
			{
				return false;
			}
			EMaterialParameterAssociation NativeAssociation =
				EMaterialParameterAssociation::GlobalParameter;
			if (Association == TEXT("global"))
			{
				NativeAssociation = EMaterialParameterAssociation::GlobalParameter;
			}
			else if (Association == TEXT("layer"))
			{
				NativeAssociation = EMaterialParameterAssociation::LayerParameter;
			}
			else if (Association == TEXT("blend"))
			{
				NativeAssociation = EMaterialParameterAssociation::BlendParameter;
			}
			else
			{
				return false;
			}
			if ((NativeAssociation
					== EMaterialParameterAssociation::GlobalParameter
					&& Index != INDEX_NONE)
				|| (NativeAssociation
						!= EMaterialParameterAssociation::GlobalParameter
					&& Index < 0))
			{
				return false;
			}
			if (Type == TEXT("scalar"))
			{
				OutType = EMaterialNiagaraGraphParameterType::Scalar;
			}
			else if (Type == TEXT("vector"))
			{
				OutType = EMaterialNiagaraGraphParameterType::Vector;
			}
			else if (Type == TEXT("texture"))
			{
				OutType = EMaterialNiagaraGraphParameterType::Texture;
			}
			else if (Type == TEXT("static_switch"))
			{
				OutType = EMaterialNiagaraGraphParameterType::StaticSwitch;
			}
			else
			{
				return false;
			}
			OutInfo = FMaterialParameterInfo(*Name, NativeAssociation, Index);
			return true;
		}

		bool TryValidateExpressionProperty(
			UClass& ExpressionClass,
			const FString& PropertyPath,
			const FString& ValueType)
		{
			FProperty* const Property = ExpressionClass.FindPropertyByName(*PropertyPath);
			if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit))
			{
				return false;
			}
			if (ValueType == TEXT("linear_color"))
			{
				const FStructProperty* const StructProperty =
					CastField<FStructProperty>(Property);
				return StructProperty
					&& StructProperty->Struct == TBaseStructure<FLinearColor>::Get();
			}
			return ValueType == TEXT("name") && CastField<FNameProperty>(Property);
		}

		bool TryOrderMaterialMutationOperations(
			const TArray<FMaterialMutationOperation>& Operations,
			TArray<int32>& OutOrder,
			FGraphCompoundRuntimeError& OutError)
		{
			OutOrder.Reset();
			TMap<FString, int32> Indices;
			for (int32 Index = 0; Index < Operations.Num(); ++Index)
			{
				if (Indices.Contains(Operations[Index].SemanticId))
				{
					SetError(OutError,
						TEXT("graph_compound_material_edit_identity_duplicate"),
						Operations[Index].SemanticId);
					return false;
				}
				Indices.Add(Operations[Index].SemanticId, Index);
			}
			TArray<int32> Remaining;
			for (int32 Index = 0; Index < Operations.Num(); ++Index)
			{
				Remaining.Add(Index);
			}
			TSet<FString> Completed;
			while (!Remaining.IsEmpty())
			{
				bool bProgress = false;
				for (int32 Position = Remaining.Num() - 1; Position >= 0; --Position)
				{
					const FMaterialMutationOperation& Operation =
						Operations[Remaining[Position]];
					bool bReady = true;
					for (const FString& Dependency : Operation.Dependencies)
					{
						if (Indices.Contains(Dependency)
							&& !Completed.Contains(Dependency))
						{
							bReady = false;
							break;
						}
					}
					if (bReady)
					{
						OutOrder.Add(Remaining[Position]);
						Completed.Add(Operation.SemanticId);
						Remaining.RemoveAt(Position);
						bProgress = true;
					}
				}
				if (!bProgress)
				{
					SetError(OutError,
						TEXT("graph_compound_material_dependency_cycle"));
					return false;
				}
			}
			return true;
		}

		UClass* ResolveProposedExpressionClass(
			UObject& Owner,
			const FString& SemanticId,
			const TMap<FString, UClass*>& ProposedClasses)
		{
			if (UClass* const* const Proposed = ProposedClasses.Find(SemanticId))
			{
				return *Proposed;
			}
			if (UMaterialExpression* const Existing =
				FindExpressionBySemanticId(Owner, SemanticId))
			{
				return Existing->GetClass();
			}
			return nullptr;
		}

		bool TryParseMaterialMutationOperations(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FString& TargetRef,
			UObject& Target,
			const FGraphCompoundCanonicalPayload& Payload,
			const FPlanningPreflightPlanRef& Plan,
			TArray<FMaterialMutationOperation>& OutOperations,
			TArray<int32>& OutOrder,
			FGraphCompoundRuntimeError& OutError)
		{
			OutOperations.Reset();
			OutOrder.Reset();
			const TArray<FGraphCompoundCanonicalEdit>& Edits = Payload.GetEdits();
			const TArray<FPlannedChange>& Changes =
				Plan->GetChangeSet().GetChanges();
			if (Plan->GetDescriptorIdentity()
					!= Profile.GetExecutionContract().GetDescriptorIdentity()
				|| Edits.IsEmpty() || Edits.Num() != Changes.Num())
			{
				SetError(OutError,
					TEXT("graph_compound_material_plan_invalid"), TargetRef,
					FString::Printf(TEXT("descriptor=%d edits=%d changes=%d"),
						Plan->GetDescriptorIdentity()
							== Profile.GetExecutionContract().GetDescriptorIdentity(),
						Edits.Num(), Changes.Num()));
				return false;
			}

			TMap<FString, const FPlannedChange*> PlannedById;
			for (const FPlannedChange& Change : Changes)
			{
				if (Change.GetIdentity().GetOwnerId() != TargetRef
					|| PlannedById.Contains(Change.GetIdentity().GetSemanticId()))
				{
					SetError(OutError,
						TEXT("graph_compound_material_plan_invalid"),
						Change.GetIdentity().GetSemanticId(),
						TEXT("owner=") + Change.GetIdentity().GetOwnerId()
							+ TEXT(" target=") + TargetRef);
					return false;
				}
				PlannedById.Add(Change.GetIdentity().GetSemanticId(), &Change);
			}

			TMap<FString, UClass*> ProposedClasses;
			for (const FGraphCompoundCanonicalEdit& Edit : Edits)
			{
				const FString SemanticId = Edit.GetIdentity().GetSemanticId();
				const FPlannedChange* const* const Planned =
					PlannedById.Find(SemanticId);
				if (!Planned || !*Planned
					|| (*Planned)->GetIntent() != Edit.GetIntent()
					|| Edit.GetIdentity().GetOwnerId() != TargetRef
					|| !Edit.GetProposedAfter().IsValid())
				{
					SetError(OutError,
						TEXT("graph_compound_material_plan_invalid"), SemanticId,
						FString::Printf(TEXT("planned=%d intent=%d owner=%s target=%s proposed=%d"),
							Planned && *Planned,
							Planned && *Planned
								? static_cast<int32>((*Planned)->GetIntent())
								: -1,
							*Edit.GetIdentity().GetOwnerId(), *TargetRef,
							Edit.GetProposedAfter().IsValid()));
					return false;
				}

				FMaterialMutationOperation Operation;
				Operation.SemanticId = SemanticId;
				Operation.Intent = Edit.GetIntent();
				Operation.Value = Edit.GetProposedAfter();
				for (const FUeAgentCanonicalIdentity& Dependency
					: Edit.GetDependencies())
				{
					Operation.Dependencies.Add(Dependency.GetSemanticId());
				}

				if (Profile.GetTypedInputKind()
					== EGraphCompoundTypedInputKind::MaterialInstanceParameters)
				{
					Operation.Kind = EMaterialMutationOperationKind::Parameter;
					if (Edit.GetElementKind()
							!= EGraphCompoundElementKind::Property
						|| Edit.GetIntent() != EChangeIntent::Update
						|| !Operation.Dependencies.IsEmpty()
						|| !TryParseParameterSemanticId(
							SemanticId, Operation.ParameterInfo,
							Operation.ParameterType))
					{
						SetError(OutError,
							TEXT("graph_compound_material_parameter_invalid"),
							SemanticId);
						return false;
					}
					OutOperations.Add(MoveTemp(Operation));
					continue;
				}

				if (Edit.GetElementKind() == EGraphCompoundElementKind::Item)
				{
					Operation.Kind = EMaterialMutationOperationKind::Item;
					const FUeAgentCanonicalValue& Value = *Operation.Value;
					const FUeAgentCanonicalValue* const Position =
						FindObjectValue(Value, TEXT("position"));
					if (Edit.GetIntent() != EChangeIntent::Create
						|| !Operation.Dependencies.IsEmpty()
						|| !SemanticId.StartsWith(TEXT("expression:"))
						|| !FGuid::Parse(SemanticId.RightChop(11),
							Operation.ExpressionGuid)
						|| !HasExactObjectFields(Value,
							{TEXT("class_ref"), TEXT("position")})
						|| !TryCanonicalStringField(
							Value, TEXT("class_ref"), Operation.ClassRef)
						|| !Position
						|| !HasExactObjectFields(
							*Position, {TEXT("x"), TEXT("y")})
						|| !TryCanonicalIntegerField(
							*Position, TEXT("x"), Operation.PositionX)
						|| !TryCanonicalIntegerField(
							*Position, TEXT("y"), Operation.PositionY)
						|| FindExpressionBySemanticId(Target, SemanticId))
					{
						SetError(OutError,
							TEXT("graph_compound_material_item_invalid"), SemanticId);
						return false;
					}
					UClass* const ExpressionClass =
						LoadObject<UClass>(nullptr, *Operation.ClassRef);
					if (!ExpressionClass
						|| !ExpressionClass->IsChildOf(
							UMaterialExpression::StaticClass())
						|| ExpressionClass->HasAnyClassFlags(CLASS_Abstract))
					{
						SetError(OutError,
							TEXT("graph_compound_material_expression_class_invalid"),
							Operation.ClassRef);
						return false;
					}
					ProposedClasses.Add(SemanticId, ExpressionClass);
				}
				else if (Edit.GetElementKind()
					== EGraphCompoundElementKind::Property)
				{
					Operation.Kind = EMaterialMutationOperationKind::Property;
					const FUeAgentCanonicalValue& Value = *Operation.Value;
					if ((Edit.GetIntent() != EChangeIntent::Create
							&& Edit.GetIntent() != EChangeIntent::Update)
						|| Operation.Dependencies.Num() != 1
						|| !HasExactObjectFields(Value,
							{TEXT("property_path"), TEXT("value_type"),
								TEXT("value")})
						|| !TryCanonicalStringField(Value,
							TEXT("property_path"), Operation.PropertyPath)
						|| !TryCanonicalStringField(Value,
							TEXT("value_type"), Operation.ValueType))
					{
						SetError(OutError,
							TEXT("graph_compound_material_property_fields_invalid"),
							SemanticId);
						return false;
					}
					Operation.OwnerItemRef = Operation.Dependencies[0];
					if (SemanticId != TEXT("property:")
							+ Operation.OwnerItemRef + TEXT(":")
							+ Operation.PropertyPath)
					{
						SetError(OutError,
							TEXT("graph_compound_material_property_identity_invalid"),
							SemanticId);
						return false;
					}
					if (Operation.ValueType != TEXT("linear_color")
						&& Operation.ValueType != TEXT("name"))
					{
						SetError(OutError,
							TEXT("graph_compound_material_property_value_type_invalid"),
							SemanticId, Operation.ValueType);
						return false;
					}
				}
				else if (Edit.GetElementKind()
					== EGraphCompoundElementKind::Relation)
				{
					Operation.Kind = EMaterialMutationOperationKind::Relation;
					const FUeAgentCanonicalValue& Value = *Operation.Value;
					if (Edit.GetIntent() != EChangeIntent::Create
						|| !HasExactObjectFields(Value,
							{TEXT("from_item_ref"), TEXT("from_output_index"),
								TEXT("to_item_ref"), TEXT("to_input_ref")})
						|| !TryCanonicalStringField(Value,
							TEXT("from_item_ref"), Operation.FromItemRef)
						|| !TryCanonicalIntegerField(Value,
							TEXT("from_output_index"), Operation.FromOutputIndex)
						|| Operation.FromOutputIndex < 0
						|| !TryCanonicalStringField(Value,
							TEXT("to_item_ref"), Operation.ToItemRef)
						|| !TryCanonicalStringField(Value,
							TEXT("to_input_ref"), Operation.ToInputRef))
					{
						SetError(OutError,
							TEXT("graph_compound_material_relation_fields_invalid"),
							SemanticId);
						return false;
					}
					const FString ExpectedId = FString::Printf(
						TEXT("link:%s:%d->%s:%s"), *Operation.FromItemRef,
						Operation.FromOutputIndex, *Operation.ToItemRef,
						*Operation.ToInputRef);
					if (SemanticId != ExpectedId)
					{
						SetError(OutError,
							TEXT("graph_compound_material_relation_identity_invalid"),
							SemanticId);
						return false;
					}
				}
				else
				{
					SetError(OutError,
						TEXT("graph_compound_material_edit_kind_invalid"), SemanticId);
					return false;
				}
				OutOperations.Add(MoveTemp(Operation));
			}

			for (FMaterialMutationOperation& Operation : OutOperations)
			{
				if (Operation.Kind == EMaterialMutationOperationKind::Property)
				{
					UClass* const OwnerClass = ResolveProposedExpressionClass(
						Target, Operation.OwnerItemRef, ProposedClasses);
					const FUeAgentCanonicalValue* const PropertyValue =
						FindObjectValue(*Operation.Value, TEXT("value"));
					FLinearColor Color;
					const bool bValueValid = PropertyValue
						&& ((Operation.ValueType == TEXT("linear_color")
								&& TryCanonicalLinearColor(*PropertyValue, Color))
							|| (Operation.ValueType == TEXT("name")
								&& PropertyValue->GetKind()
									== EUeAgentValueKind::String
								&& !PropertyValue->GetText().IsEmpty()));
					if (!OwnerClass || !bValueValid
						|| !TryValidateExpressionProperty(
							*OwnerClass, Operation.PropertyPath,
							Operation.ValueType))
					{
						SetError(OutError,
							TEXT("graph_compound_material_property_invalid"),
							Operation.SemanticId);
						return false;
					}
				}
				else if (Operation.Kind
					== EMaterialMutationOperationKind::Relation)
				{
					UClass* const FromClass = ResolveProposedExpressionClass(
						Target, Operation.FromItemRef, ProposedClasses);
					UMaterialExpression* const FromCdo = FromClass
						? Cast<UMaterialExpression>(FromClass->GetDefaultObject())
						: nullptr;
					bool bEndpointValid = FromCdo
						&& FromCdo->GetOutputs().IsValidIndex(
							Operation.FromOutputIndex);
					if (Operation.ToItemRef == TEXT("material:root"))
					{
						EMaterialProperty Property = MP_MAX;
						bEndpointValid = bEndpointValid && Cast<UMaterial>(&Target)
							&& UeAgentMaterialAuthoring::ParseMaterialProperty(
								Operation.ToInputRef, Property);
					}
					else
					{
						UClass* const ToClass = ResolveProposedExpressionClass(
							Target, Operation.ToItemRef, ProposedClasses);
						UMaterialExpression* const ToCdo = ToClass
							? Cast<UMaterialExpression>(ToClass->GetDefaultObject())
							: nullptr;
						FExpressionInput* Input = nullptr;
						bEndpointValid = bEndpointValid && ToCdo
							&& UeAgentMaterialAuthoring::ResolveExpressionInput(
								*ToCdo, Operation.ToInputRef, Input) && Input;
					}
					if (!bEndpointValid)
					{
						SetError(OutError,
							TEXT("graph_compound_material_relation_endpoint_invalid"),
							Operation.SemanticId);
						return false;
					}
				}
				else if (Operation.Kind
					== EMaterialMutationOperationKind::Parameter)
				{
					const FUeAgentCanonicalValue* const Value =
						TryGetParameterEffectiveValue(*Operation.Value);
					FLinearColor Color;
					double Number = 0.0;
					const bool bValid = Value && (
						(Operation.ParameterType
								== EMaterialNiagaraGraphParameterType::Scalar
							&& TryCanonicalNumberValue(*Value, Number))
						|| (Operation.ParameterType
								== EMaterialNiagaraGraphParameterType::Vector
							&& TryCanonicalLinearColor(*Value, Color))
						|| (Operation.ParameterType
								== EMaterialNiagaraGraphParameterType::Texture
							&& Value->GetKind() == EUeAgentValueKind::String
							&& LoadObject<UTexture>(nullptr, *Value->GetText()))
						|| (Operation.ParameterType
								== EMaterialNiagaraGraphParameterType::StaticSwitch
							&& Value->GetKind() == EUeAgentValueKind::Boolean));
					if (!bValid)
					{
						SetError(OutError,
							TEXT("graph_compound_material_parameter_value_invalid"),
							Operation.SemanticId);
						return false;
					}
				}
			}
			return TryOrderMaterialMutationOperations(
				OutOperations, OutOrder, OutError);
		}

		bool TryApplyMaterialMutationOperations(
			UObject& Target,
			const TArray<FMaterialMutationOperation>& Operations,
			const TArray<int32>& Order,
			FMaterialNiagaraGraphNativeHook& Hook,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
			FGraphCompoundRuntimeError& OutError)
		{
			for (const int32 Index : Order)
			{
				if (!Operations.IsValidIndex(Index))
				{
					SetError(OutError,
						TEXT("graph_compound_material_operation_order_invalid"));
					return false;
				}
				if (Cancellation->IsCancelled())
				{
					SetError(OutError,
						TEXT("graph_compound_material_mutation_cancelled"));
					return false;
				}
				const FMaterialMutationOperation& Operation = Operations[Index];
				FMaterialNiagaraGraphNativeMutationReceipt Receipt;
				FMaterialNiagaraGraphAdapterError Error;
				bool bApplied = false;
				switch (Operation.Kind)
				{
				case EMaterialMutationOperationKind::Item:
				{
					UClass* const ExpressionClass =
						LoadObject<UClass>(nullptr, *Operation.ClassRef);
					UMaterialExpression* Expression = nullptr;
					bApplied = ExpressionClass
						&& Hook.TryAddMaterialExpression(
							Target, *ExpressionClass, Operation.ExpressionGuid,
							Operation.PositionX, Operation.PositionY,
							Expression, Receipt, Error)
						&& Expression;
					break;
				}
				case EMaterialMutationOperationKind::Property:
				{
					UMaterialExpression* const Expression =
						FindExpressionBySemanticId(Target, Operation.OwnerItemRef);
					const FUeAgentCanonicalValue* const Value =
						FindObjectValue(*Operation.Value, TEXT("value"));
					if (Expression && Value
						&& Operation.ValueType == TEXT("linear_color"))
					{
						FLinearColor Color;
						bApplied = TryCanonicalLinearColor(*Value, Color)
							&& Hook.TrySetMaterialExpressionLinearColorProperty(
								*Expression, Operation.PropertyPath, Color,
								Receipt, Error);
					}
					else if (Expression && Value
						&& Operation.ValueType == TEXT("name")
						&& Value->GetKind() == EUeAgentValueKind::String)
					{
						bApplied = Hook.TrySetMaterialExpressionNameProperty(
							*Expression, Operation.PropertyPath, *Value->GetText(),
							Receipt, Error);
					}
					break;
				}
				case EMaterialMutationOperationKind::Relation:
				{
					UMaterialExpression* const From =
						FindExpressionBySemanticId(Target, Operation.FromItemRef);
					if (From && Operation.ToItemRef == TEXT("material:root"))
					{
						EMaterialProperty Property = MP_MAX;
						bApplied = UeAgentMaterialAuthoring::ParseMaterialProperty(
								Operation.ToInputRef, Property)
							&& Hook.TryConnectMaterialExpressionToPropertyByIndex(
								*From, Operation.FromOutputIndex, Property,
								Receipt, Error);
					}
					else if (From)
					{
						UMaterialExpression* const To =
							FindExpressionBySemanticId(Target, Operation.ToItemRef);
						bApplied = To && Hook.TryConnectMaterialExpressionsByIndex(
							*From, Operation.FromOutputIndex, *To,
							Operation.ToInputRef, Receipt, Error);
					}
					break;
				}
				case EMaterialMutationOperationKind::Parameter:
				{
					UMaterialInstanceConstant* const Instance =
						Cast<UMaterialInstanceConstant>(&Target);
					const FUeAgentCanonicalValue* const Value =
						TryGetParameterEffectiveValue(*Operation.Value);
					if (!Instance || !Value)
					{
						break;
					}
					switch (Operation.ParameterType)
					{
					case EMaterialNiagaraGraphParameterType::Scalar:
					{
						double Number = 0.0;
						bApplied = TryCanonicalNumberValue(*Value, Number)
							&& Hook.TrySetMaterialInstanceScalarParameter(
								*Instance, Operation.ParameterInfo,
								static_cast<float>(Number), Receipt, Error);
						break;
					}
					case EMaterialNiagaraGraphParameterType::Vector:
					{
						FLinearColor Color;
						bApplied = TryCanonicalLinearColor(*Value, Color)
							&& Hook.TrySetMaterialInstanceVectorParameter(
								*Instance, Operation.ParameterInfo, Color,
								Receipt, Error);
						break;
					}
					case EMaterialNiagaraGraphParameterType::Texture:
					{
						UTexture* const Texture = Value->GetKind()
							== EUeAgentValueKind::String
							? LoadObject<UTexture>(nullptr, *Value->GetText())
							: nullptr;
						bApplied = Texture
							&& Hook.TrySetMaterialInstanceTextureParameter(
								*Instance, Operation.ParameterInfo, *Texture,
								Receipt, Error);
						break;
					}
					case EMaterialNiagaraGraphParameterType::StaticSwitch:
						bApplied = Value->GetKind() == EUeAgentValueKind::Boolean
							&& Hook.TrySetMaterialInstanceStaticSwitchParameter(
								*Instance, Operation.ParameterInfo,
								Value->GetBoolean(), Receipt, Error);
						break;
					default:
						break;
					}
					break;
				}
				}
				if (!bApplied)
				{
					SetError(OutError,
						Error.GetCode().IsEmpty()
							? TEXT("graph_compound_material_mutation_failed")
							: Error.GetCode(),
						Operation.SemanticId, Error.GetDetail());
					return false;
				}
			}
			FMaterialNiagaraGraphCompileSettleReceipt Settle;
			FMaterialNiagaraGraphAdapterError SettleError;
			if (!Hook.TryCompileAndSettleMaterial(Target, Settle, SettleError)
				|| !Settle.IsCompilationComplete()
				|| Settle.GetCompileErrorCount() != 0)
			{
				SetError(OutError,
					SettleError.GetCode().IsEmpty()
						? TEXT("graph_compound_material_settle_failed")
						: SettleError.GetCode(),
					Target.GetPathName(), SettleError.GetDetail());
				return false;
			}
			return true;
		}

		bool TryUnloadMaterialMutationTarget(
			const FString& TargetRef,
			FString& OutDetail)
		{
			OutDetail.Reset();
			const FString PackageName =
				FPackageName::ObjectPathToPackageName(TargetRef);
			UPackage* const Package = FindPackage(nullptr, *PackageName);
			if (!Package)
			{
				return true;
			}
			Package->SetDirtyFlag(false);
			FText Error;
			if (!UPackageTools::UnloadPackages({Package}, Error, true))
			{
				OutDetail = Error.ToString();
				return false;
			}
			CollectGarbage(RF_NoFlags, true);
			if (FindPackage(nullptr, *PackageName))
			{
				OutDetail = TEXT("package remains loaded after unload");
				return false;
			}
			return true;
		}

		bool TryRollbackMaterialMutation(
			const FString& TargetRef,
			const FAssetDurableSessionRef& Session,
			FPackageAssetDurableBackend& Backend,
			const int64 FencingToken,
			FUeAgentResourceTracker& Resources,
			FString& OutDetail)
		{
			FString UnloadDetail;
			const bool bMemoryClean =
				TryUnloadMaterialMutationTarget(TargetRef, UnloadDetail);
			FDurableReceipt Receipt;
			FAssetDurableError Error;
			const bool bDurableClean = TryRollbackAssetDurability(
				Session, Backend, FencingToken, bMemoryClean,
				Resources, Receipt, Error);
			OutDetail = !UnloadDetail.IsEmpty()
				? UnloadDetail
				: Error.GetCode() + TEXT(":") + Error.GetDetail();
			return bMemoryClean && bDurableClean;
		}

		bool TryBuildMaterialMutationAggregate(
			const TArray<FMaterialMutationOperation>& Operations,
			const TMap<FString, FUeAgentCanonicalValueRef>* ActualValues,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FVerificationError& OutError)
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
			Items.Reserve(Operations.Num());
			for (const FMaterialMutationOperation& Operation : Operations)
			{
				const FUeAgentCanonicalValueRef* const Actual = ActualValues
					? ActualValues->Find(Operation.SemanticId)
					: nullptr;
				if (ActualValues && !Actual)
				{
					OutError = FVerificationError(
						TEXT("graph_compound_material_readback_missing"),
						Operation.SemanticId);
					return false;
				}
				Items.Emplace(Operation.SemanticId,
					Actual ? *Actual : Operation.Value.ToSharedRef());
			}
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryObject(
					Items, Resources, OutValue, Error))
			{
				OutError = FVerificationError(Error.Code, Error.Path);
				return false;
			}
			return OutValue.IsValid();
		}

		bool TryVerifyReloadedGraphMutation(
			UObject& Target,
			const TArray<FMaterialMutationOperation>& Operations,
			TMap<FString, FUeAgentCanonicalValueRef>& OutActualValues,
			FUeAgentResourceTracker& Resources,
			FString& OutDiagnostic)
		{
			OutActualValues.Reset();
			OutDiagnostic.Reset();
			for (const FMaterialMutationOperation& Operation : Operations)
			{
				if (Operation.Kind == EMaterialMutationOperationKind::Item)
				{
					UMaterialExpression* const Expression =
						FindExpressionBySemanticId(Target, Operation.SemanticId);
					if (!Expression
						|| Expression->GetClass()->GetPathName()
							!= Operation.ClassRef
						|| Expression->MaterialExpressionEditorX
							!= Operation.PositionX
						|| Expression->MaterialExpressionEditorY
							!= Operation.PositionY)
					{
						OutDiagnostic =
							TEXT("graph_compound_material_item_readback_mismatch");
						return false;
					}
					OutActualValues.Add(
						Operation.SemanticId, Operation.Value.ToSharedRef());
				}
				else if (Operation.Kind
					== EMaterialMutationOperationKind::Property)
				{
					UMaterialExpression* const Expression =
						FindExpressionBySemanticId(Target, Operation.OwnerItemRef);
					FProperty* const Property = Expression
						? Expression->GetClass()->FindPropertyByName(
							*Operation.PropertyPath)
						: nullptr;
					const FUeAgentCanonicalValue* const Expected =
						FindObjectValue(*Operation.Value, TEXT("value"));
					bool bMatches = false;
					if (Expression && Property && Expected
						&& Operation.ValueType == TEXT("linear_color"))
					{
						const FStructProperty* const StructProperty =
							CastField<FStructProperty>(Property);
						FLinearColor ExpectedColor;
						if (StructProperty
							&& StructProperty->Struct
								== TBaseStructure<FLinearColor>::Get()
							&& TryCanonicalLinearColor(*Expected, ExpectedColor))
						{
							const FLinearColor* const Actual =
								StructProperty->ContainerPtrToValuePtr<FLinearColor>(
									Expression);
							bMatches = Actual && Actual->Equals(ExpectedColor);
						}
					}
					else if (Expression && Property && Expected
						&& Operation.ValueType == TEXT("name"))
					{
						const FNameProperty* const NameProperty =
							CastField<FNameProperty>(Property);
						bMatches = NameProperty
							&& Expected->GetKind() == EUeAgentValueKind::String
							&& NameProperty->GetPropertyValue_InContainer(Expression)
								.ToString() == Expected->GetText();
					}
					if (!bMatches)
					{
						OutDiagnostic =
							TEXT("graph_compound_material_property_readback_mismatch");
						return false;
					}
					OutActualValues.Add(
						Operation.SemanticId, Operation.Value.ToSharedRef());
				}
				else if (Operation.Kind
					== EMaterialMutationOperationKind::Relation)
				{
					UMaterialExpression* const From =
						FindExpressionBySemanticId(Target, Operation.FromItemRef);
					FExpressionInput* Input = nullptr;
					if (Operation.ToItemRef == TEXT("material:root"))
					{
						EMaterialProperty Property = MP_MAX;
						UMaterial* const Material = Cast<UMaterial>(&Target);
						if (Material
							&& UeAgentMaterialAuthoring::ParseMaterialProperty(
								Operation.ToInputRef, Property))
						{
							Input = Material->GetExpressionInputForProperty(Property);
						}
					}
					else if (UMaterialExpression* const To =
						FindExpressionBySemanticId(Target, Operation.ToItemRef))
					{
						UeAgentMaterialAuthoring::ResolveExpressionInput(
							*To, Operation.ToInputRef, Input);
					}
					if (!From || !Input || Input->Expression != From
						|| Input->OutputIndex != Operation.FromOutputIndex)
					{
						OutDiagnostic =
							TEXT("graph_compound_material_relation_readback_mismatch");
						return false;
					}
					OutActualValues.Add(
						Operation.SemanticId, Operation.Value.ToSharedRef());
				}
			}
			return true;
		}

		class FMaterialMutationReadbackOracle final
			: public IIndependentReadbackOracle
		{
		public:
			FMaterialMutationReadbackOracle(
				FGraphCompoundDomainRouteProfile InProfile,
				FString InTargetRef,
				TArray<FMaterialMutationOperation> InOperations)
				: Profile(MoveTemp(InProfile))
				, TargetRef(MoveTemp(InTargetRef))
				, Operations(MoveTemp(InOperations))
			{
			}

			const FString& GetOracleId() const override
			{
				return MaterialMutationReadbackOracleId;
			}

			bool TryRead(
				const FIndependentReadbackRequest& Request,
				FUeAgentResourceTracker& Resources,
				FIndependentReadbackObservation& OutObservation,
				FVerificationError& OutError) override
			{
				OutObservation = FIndependentReadbackObservation();
				OutError = FVerificationError();
				FString Detail;
				if (!TryUnloadMaterialMutationTarget(TargetRef, Detail))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						TEXT("graph_compound_material_unload_failed"), Detail);
					return true;
				}
				FGraphCompoundRuntimeError ResolveError;
				UObject* const Target =
					TryResolveMaterialMutationTarget(Profile, TargetRef, ResolveError);
				if (!Target || Target->GetOutermost()->IsDirty())
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						ResolveError.Code.IsEmpty()
							? TEXT("graph_compound_material_reload_failed")
							: ResolveError.Code,
						ResolveError.Detail);
					return true;
				}

				TMap<FString, FUeAgentCanonicalValueRef> ActualValues;
				if (Profile.GetTypedInputKind()
					== EGraphCompoundTypedInputKind::MaterialInstanceParameters)
				{
					UMaterialInstanceConstant* const Instance =
						Cast<UMaterialInstanceConstant>(Target);
					TArray<FMaterialNiagaraGraphEffectiveParameter> Parameters;
					FString Revision;
					FString Evidence;
					FMaterialNiagaraGraphAdapterError NativeError;
					if (!Instance
						|| !Hook.TryCaptureMaterialInstanceEffectiveParameters(
							*Instance, Parameters, Revision, Evidence, NativeError))
					{
						OutObservation =
							FIndependentReadbackObservation::Unavailable(
								NativeError.GetCode().IsEmpty()
									? TEXT("graph_compound_material_parameter_readback_failed")
									: NativeError.GetCode(),
								NativeError.GetDetail());
						return true;
					}
					TMap<FString, const FMaterialNiagaraGraphEffectiveParameter*>
						ByIdentity;
					for (const FMaterialNiagaraGraphEffectiveParameter& Parameter
						: Parameters)
					{
						ByIdentity.Add(ParameterSemanticId(Parameter.GetIdentity()),
							&Parameter);
					}
					for (const FMaterialMutationOperation& Operation : Operations)
					{
						const FMaterialNiagaraGraphEffectiveParameter* const* Found =
							ByIdentity.Find(Operation.SemanticId);
						FUeAgentCanonicalValuePtr Actual;
						FGraphCompoundRuntimeError CanonicalError;
						if (!Found || !*Found || !(*Found)->IsOverridden()
							|| !TryCanonicalEffectiveParameterState(
								**Found, Resources, Actual, CanonicalError))
						{
							OutObservation =
								FIndependentReadbackObservation::Unavailable(
									TEXT("graph_compound_material_parameter_readback_mismatch"),
									Operation.SemanticId);
							return true;
						}
						ActualValues.Add(Operation.SemanticId, Actual.ToSharedRef());
					}
				}
				else if (!TryVerifyReloadedGraphMutation(
					*Target, Operations, ActualValues, Resources, Detail))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						Detail, TargetRef);
					return true;
				}

				FUeAgentCanonicalValuePtr Actual;
				if (!TryBuildMaterialMutationAggregate(
						Operations, &ActualValues, Resources, Actual, OutError))
				{
					return false;
				}
				OutObservation = FIndependentReadbackObservation::Available(
					Actual.ToSharedRef(),
					TEXT("reload:") + Request.GetSubjectRef() + TEXT(":")
						+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
				return true;
			}

		private:
			FGraphCompoundDomainRouteProfile Profile;
			FString TargetRef;
			TArray<FMaterialMutationOperation> Operations;
			FMaterialNiagaraGraphNativeHook Hook;
		};

		UObject* TryResolveEventOwner(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundEventQueryRequest& Request,
			FGraphCompoundRuntimeError& OutError)
		{
			if (Profile.GetCapabilityId()
				== TEXT("blueprint.component.events.discover"))
			{
				UBlueprint* const Blueprint =
					LoadObject<UBlueprint>(nullptr, *Request.GetTargetRef());
				if (!Blueprint || !Blueprint->SimpleConstructionScript
					|| !Request.GetOwnerItemRef().StartsWith(
						TEXT("component:")))
				{
					SetError(OutError,
						TEXT("graph_compound_blueprint_event_owner_invalid"),
						Request.GetOwnerItemRef());
					return nullptr;
				}
				const FString VariableName =
					Request.GetOwnerItemRef().RightChop(10);
				for (USCS_Node* const Node
					: Blueprint->SimpleConstructionScript->GetAllNodes())
				{
					if (Node && Node->GetVariableName().ToString() == VariableName
						&& Node->ComponentTemplate)
					{
						return Node->ComponentTemplate;
					}
				}
			}
			else if (Profile.GetCapabilityId()
				== TEXT("umg.widget.events.discover"))
			{
				UWidgetBlueprint* const Blueprint =
					LoadObject<UWidgetBlueprint>(
						nullptr, *Request.GetTargetRef());
				if (!Blueprint || !Blueprint->WidgetTree
					|| !Request.GetOwnerItemRef().StartsWith(TEXT("widget:")))
				{
					SetError(OutError,
						TEXT("graph_compound_umg_event_owner_invalid"),
						Request.GetOwnerItemRef());
					return nullptr;
				}
				const FString WidgetName =
					Request.GetOwnerItemRef().RightChop(7);
				if (UWidget* const Widget =
					Blueprint->WidgetTree->FindWidget(*WidgetName))
				{
					return Widget;
				}
			}
			if (OutError.Code.IsEmpty())
			{
				SetError(OutError,
					TEXT("graph_compound_event_owner_not_found"),
					Request.GetOwnerItemRef());
			}
			return nullptr;
		}

		bool TrySerializeEvents(
			const TArray<FBlueprintUmgGraphEventIdentity>& Events,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Reserve(Events.Num());
			for (const FBlueprintUmgGraphEventIdentity& Event : Events)
			{
				const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
				Object->SetStringField(TEXT("event_ref"),
					Event.GetEventIdentity());
				Object->SetStringField(TEXT("native_field_name"),
					Event.GetNativeFieldName());
				Object->SetStringField(TEXT("signature_ref"),
					Event.GetSignatureIdentity());
				Values.Add(MakeShared<FJsonValueObject>(Object));
			}
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetArrayField(TEXT("events"), MoveTemp(Values));
			FString Canonical;
			FString Error;
			if (!Json::TryCanonicalize(Root, Canonical, Error))
			{
				SetError(OutError,
					TEXT("graph_compound_event_result_invalid"),
					FString(), Error);
				return false;
			}
			OutResult = FUeAgentContractValue(MoveTemp(Canonical));
			return true;
		}

		bool TrySerializeBlueprintComponentEvents(
			const DomainAdapters::BlueprintCommands::
				FBlueprintComponentEventDiscoveryResult& Result,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			TArray<TSharedPtr<FJsonValue>> Events;
			Events.Reserve(Result.Events.Num());
			for (const DomainAdapters::BlueprintCommands::
				FBlueprintComponentEventRecord& Event : Result.Events)
			{
				const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
				Object->SetStringField(TEXT("event_ref"), Event.EventRef);
				Object->SetStringField(
					TEXT("native_field_name"), Event.NativeFieldName);
				Object->SetStringField(
					TEXT("signature_ref"), Event.SignatureRef);
				Object->SetStringField(
					TEXT("display_name"), Event.DisplayName);
				Object->SetStringField(TEXT("category"), Event.Category);
				Object->SetBoolField(TEXT("hidden"), Event.bHidden);
				Object->SetBoolField(
					TEXT("already_bound"), Event.bAlreadyBound);
				Events.Add(MakeShared<FJsonValueObject>(Object));
			}
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("asset_path"), Result.AssetPath);
			Root->SetStringField(TEXT("component_name"), Result.ComponentName);
			Root->SetStringField(TEXT("target_class"), Result.TargetClassRef);
			Root->SetBoolField(TEXT("component_property_found"),
				Result.bComponentPropertyFound);
			Root->SetBoolField(TEXT("can_author_component_bound_event"),
				Result.bCanAuthorComponentBoundEvent);
			Root->SetBoolField(TEXT("compiled_before_query"),
				Result.bCompiledBeforeQuery);
			Root->SetNumberField(
				TEXT("total_event_count"), Result.TotalEventCount);
			Root->SetNumberField(
				TEXT("matched_event_count"), Result.MatchedEventCount);
			Root->SetNumberField(
				TEXT("returned_event_count"), Result.Events.Num());
			Root->SetBoolField(TEXT("truncated"), Result.bTruncated);
			Root->SetArrayField(TEXT("events"), MoveTemp(Events));
			FString Canonical;
			FString Error;
			if (!Json::TryCanonicalize(Root, Canonical, Error))
			{
				SetError(OutError,
					TEXT("graph_compound_blueprint_event_result_invalid"),
					Result.AssetPath, Error);
				return false;
			}
			OutResult = FUeAgentContractValue(MoveTemp(Canonical));
			return true;
		}

		bool TrySerializeCreateResult(
			const FString& TargetRef,
			const FString& Revision,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("revision"), Revision);
			Root->SetStringField(TEXT("target_ref"), TargetRef);
			FString Canonical;
			FString Error;
			if (!Json::TryCanonicalize(Root, Canonical, Error))
			{
				SetError(OutError,
					TEXT("graph_compound_asset_result_invalid"),
					TargetRef, Error);
				return false;
			}
			OutResult = FUeAgentContractValue(MoveTemp(Canonical));
			return true;
		}

		bool TrySerializeMaterialMutationResult(
			const FString& TargetRef,
			const int32 ChangeCount,
			const FString& EvidenceId,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			(void)ChangeCount;
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("revision"), EvidenceId);
			Root->SetStringField(TEXT("target_ref"), TargetRef);
			FString Canonical;
			FString Error;
			if (!Json::TryCanonicalize(Root, Canonical, Error))
			{
				SetError(OutError,
					TEXT("graph_compound_material_result_invalid"),
					TargetRef, Error);
				return false;
			}
			OutResult = FUeAgentContractValue(MoveTemp(Canonical));
			return true;
		}

		class FBlueprintMaterialProductionEnvironment final
			: public IGraphCompoundBlueprintMaterialEnvironment
		{
		public:
			explicit FBlueprintMaterialProductionEnvironment(
				FGraphCompoundBlueprintMaterialProductionConfig InConfig)
				: Config(MoveTemp(InConfig))
				, JournalRoot(Config.DurableJournalRoot.IsEmpty()
					? FPaths::ProjectSavedDir()
						/ TEXT("UeAgentInterface/GraphCompoundDurable")
					: Config.DurableJournalRoot)
				, NiagaraLifecycle(
					JournalRoot, MaterialNativeHook,
					DurableBackend, LastFencingToken)
				, NiagaraTypedMutation(
					JournalRoot, MaterialNativeHook,
					DurableBackend, LastFencingToken)
			{
			}

			bool TryQuery(
				const FGraphCompoundDomainRouteProfile& Profile,
				const FGraphCompoundEventQueryRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentContractValue& OutResult,
				FGraphCompoundRuntimeError& OutError) override
			{
				(void)CallerContext;
				(void)Resources;
				OutResult = FUeAgentContractValue(TEXT("{}"));
				OutError = {};
				if (!IsInGameThread())
				{
					SetError(OutError,
						TEXT("graph_compound_query_thread_invalid"),
						Profile.GetCapabilityId());
					return false;
				}
				if (Cancellation->IsCancelled())
				{
					SetError(OutError, TEXT("graph_compound_query_cancelled"),
						Profile.GetCapabilityId());
					return false;
				}
				if (Profile.GetCapabilityId()
					== TEXT("blueprint.component.events.discover"))
				{
					FString ObjectPath = Request.GetTargetRef();
					if (!ObjectPath.Contains(TEXT(".")))
					{
						const FString AssetName =
							FPackageName::GetLongPackageAssetName(ObjectPath);
						ObjectPath += TEXT(".") + AssetName;
					}
					UBlueprint* const Blueprint =
						LoadObject<UBlueprint>(nullptr, *ObjectPath);
					if (!Blueprint)
					{
						SetError(OutError,
							TEXT("graph_compound_blueprint_event_target_invalid"),
							Request.GetTargetRef());
						return false;
					}
					DomainAdapters::BlueprintCommands::
						FBlueprintComponentEventDiscoveryRequest NativeRequest;
					NativeRequest.ComponentName =
						Request.GetOwnerItemRef().RightChop(10);
					NativeRequest.Query = Request.GetQuery();
					NativeRequest.Limit = Request.GetLimit();
					NativeRequest.bOnlyUnbound = Request.IsOnlyUnbound();
					NativeRequest.bIncludeHidden = Request.IsIncludeHidden();
					NativeRequest.bCompileBeforeQuery =
						Request.IsCompileBeforeQuery();
					DomainAdapters::BlueprintCommands::
						FBlueprintComponentEventDiscoveryResult NativeResult;
					FString NativeError;
					if (!DomainAdapters::BlueprintCommands::
						TryDiscoverComponentEvents(
							*Blueprint, NativeRequest, NativeResult, NativeError))
					{
						SetError(OutError, NativeError, Request.GetTargetRef());
						return false;
					}
					return TrySerializeBlueprintComponentEvents(
						NativeResult, OutResult, OutError);
				}
				UObject* const Owner =
					TryResolveEventOwner(Profile, Request, OutError);
				if (!Owner)
				{
					return false;
				}
				TArray<FBlueprintUmgGraphEventIdentity> Events;
				FBlueprintUmgGraphAdapterError Error;
				if (!NativeHook.TryDiscoverAssignableEvents(
						*Owner, Request.GetOwnerItemRef(), Events, Error))
				{
					SetError(OutError, Error.GetCode(),
						Request.GetOwnerItemRef(), Error.GetDetail());
					return false;
				}
				return TrySerializeEvents(Events, OutResult, OutError);
			}

			bool TryCaptureTypedOperationSnapshot(
				const FGraphCompoundDomainRouteProfile& Profile,
				const FString& TargetRef,
				const TOptional<FGraphCompoundNiagaraMutationSelector>&
					NiagaraSelector,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentCanonicalSnapshotPtr& OutSnapshot,
				FGraphCompoundRuntimeError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				if (!IsInGameThread())
				{
					SetError(OutError,
						TEXT("graph_compound_typed_snapshot_thread_invalid"),
						Profile.GetCapabilityId());
					return false;
				}
				if (Cancellation->IsCancelled())
				{
					SetError(OutError,
						TEXT("graph_compound_typed_snapshot_cancelled"),
						TargetRef);
					return false;
				}
				if (Profile.GetTypedInputKind()
					== EGraphCompoundTypedInputKind::NiagaraCompileRefreshSettle)
				{
					return NiagaraLifecycle.TryCaptureCompileSnapshot(
						Profile, TargetRef, *Resources, OutSnapshot, OutError);
				}
				if (NiagaraSelector.IsSet())
				{
					return NiagaraTypedMutation.TryCaptureSnapshot(
						Profile, NiagaraSelector.GetValue(), *Resources,
						OutSnapshot, OutError);
				}
				if (Profile.GetTypedInputKind()
							!= EGraphCompoundTypedInputKind::GraphEdits
					&& Profile.GetTypedInputKind()
						!= EGraphCompoundTypedInputKind::MaterialInstanceParameters)
				{
					SetError(OutError,
						TEXT("graph_compound_material_snapshot_binding_unavailable"),
						Profile.GetCapabilityId());
					return false;
				}
				return TryCaptureMaterialMutationSnapshot(
					Profile, TargetRef, MaterialNativeHook,
					*Resources, OutSnapshot, OutError);
			}

			bool TryExecuteTypedOperation(
				const FGraphCompoundDomainRouteProfile& Profile,
				const FString& TargetRef,
				const TOptional<FGraphCompoundNiagaraMutationSelector>&
					NiagaraSelector,
				const FGraphCompoundCanonicalPayload& Payload,
				const FPlanningPreflightPlanRef& Plan,
				const FUeAgentCallerContext& CallerContext,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentContractValue& OutResult,
				FGraphCompoundRuntimeError& OutError) override
			{
				OutResult = FUeAgentContractValue(TEXT("{}"));
				OutError = {};
				if (Profile.GetTypedInputKind()
					== EGraphCompoundTypedInputKind::NiagaraCompileRefreshSettle)
				{
					return NiagaraLifecycle.TryExecuteCompile(
						Profile, TargetRef, Payload, Plan, CallerContext,
						*Resources, Cancellation, OutResult, OutError);
				}
				if (NiagaraSelector.IsSet())
				{
					return NiagaraTypedMutation.TryExecute(
						Profile, NiagaraSelector.GetValue(), Payload, Plan,
						CallerContext, Resources, Cancellation,
						OutResult, OutError);
				}
				if (!IsInGameThread())
				{
					SetError(OutError,
						TEXT("graph_compound_material_mutation_thread_invalid"),
						Profile.GetCapabilityId());
					return false;
				}
				if (Cancellation->IsCancelled())
				{
					SetError(OutError,
						TEXT("graph_compound_material_mutation_cancelled"), TargetRef);
					return false;
				}
				UObject* Target =
					TryResolveMaterialMutationTarget(Profile, TargetRef, OutError);
				if (!Target)
				{
					return false;
				}
				TArray<FMaterialMutationOperation> Operations;
				TArray<int32> Order;
				if (!TryParseMaterialMutationOperations(
						Profile, TargetRef, *Target, Payload, Plan,
						Operations, Order, OutError))
				{
					return false;
				}

				FUeAgentCanonicalSnapshotPtr Current;
				if (!TryCaptureMaterialMutationSnapshot(
						Profile, TargetRef, MaterialNativeHook,
						*Resources, Current, OutError))
				{
					return false;
				}
				FPlanningPreflightError RevalidateError;
				if (!TryRevalidatePlan(
						Plan, Current.ToSharedRef(), CallerContext,
						*Resources, RevalidateError))
				{
					SetError(OutError, RevalidateError.GetCode(),
						RevalidateError.GetIdentityKey(),
						RevalidateError.GetDetail());
					return false;
				}

				const int64 FencingToken = ++LastFencingToken;
				const FString JournalOwner =
					TEXT("graph-compound:") + Plan->GetRequestId();
				const FString PackageName =
					FPackageName::ObjectPathToPackageName(TargetRef);
				const FAssetDurableRequest DurableRequest(
					Plan,
					{FAssetDurableTarget(
						Plan->GetChangeSet().GetChanges()[0].GetIdentity(),
						PackageName, EPackageHeaderKind::Asset,
						EAssetDurableIntent::Save,
						ERollbackGrade::DurableBackup)},
					FencingToken, JournalOwner,
					EAtomicityMode::CompensatedSaga);
				FAssetDurableSessionPtr Prepared;
				FAssetDurableError DurableError;
				if (!TryPrepareAssetDurability(
						DurableRequest, DurableBackend, JournalRoot,
						*Resources, Prepared, DurableError)
					|| !Prepared.IsValid())
				{
					SetError(OutError, DurableError.GetCode(),
						TargetRef, DurableError.GetDetail());
					return false;
				}

				if (!TryApplyMaterialMutationOperations(
						*Target, Operations, Order, MaterialNativeHook,
						Cancellation, OutError))
				{
					Target = nullptr;
					FString RollbackDetail;
					TryRollbackMaterialMutation(
						TargetRef, Prepared.ToSharedRef(), DurableBackend,
						FencingToken, *Resources, RollbackDetail);
					OutError.Detail += TEXT(";") + RollbackDetail;
					return false;
				}
				Target->GetOutermost()->MarkPackageDirty();
				FDurableReceipt DurableReceipt;
				if (!TryPersistAssetDurability(
						Prepared.ToSharedRef(), DurableBackend, FencingToken,
						*Resources, DurableReceipt, DurableError))
				{
					Target = nullptr;
					FString RollbackDetail;
					TryRollbackMaterialMutation(
						TargetRef, Prepared.ToSharedRef(), DurableBackend,
						FencingToken, *Resources, RollbackDetail);
					SetError(OutError, DurableError.GetCode(), TargetRef,
						DurableError.GetDetail() + TEXT(";") + RollbackDetail);
					return false;
				}
				Target->GetOutermost()->SetDirtyFlag(false);
				Target = nullptr;
				const FAssetDurableSessionRef Persisted =
					PersistedSession(Prepared.ToSharedRef(), DurableReceipt);

				FUeAgentCanonicalValuePtr Expected;
				FVerificationError VerifyError;
				if (!TryBuildMaterialMutationAggregate(
						Operations, nullptr, *Resources, Expected, VerifyError))
				{
					FString RollbackDetail;
					TryRollbackMaterialMutation(
						TargetRef, Persisted, DurableBackend, FencingToken,
						*Resources, RollbackDetail);
					SetError(OutError, VerifyError.GetCode(), TargetRef,
						VerifyError.GetDetail() + TEXT(";") + RollbackDetail);
					return false;
				}
				TSharedPtr<FIndependentReadbackService> Readback;
				const TSharedRef<FMaterialMutationReadbackOracle,
					ESPMode::ThreadSafe> Oracle =
					MakeShared<FMaterialMutationReadbackOracle,
						ESPMode::ThreadSafe>(Profile, TargetRef, Operations);
				FIndependentReadbackOutcome Outcome;
				if (!FIndependentReadbackService::TryCreate(
						{Oracle}, Readback, VerifyError)
					|| !Readback.IsValid()
					|| !Readback->TryVerify(
						FIndependentReadbackRequest(
							TEXT("graph-compound:") + Plan->GetRequestId(),
							MaterialMutationReadbackOracleId,
							TargetRef, Expected.ToSharedRef()),
						*Resources, Outcome, VerifyError)
					|| Outcome.GetStatus() != EVerificationStatus::Verified)
				{
					FString RollbackDetail;
					TryRollbackMaterialMutation(
						TargetRef, Persisted, DurableBackend, FencingToken,
						*Resources, RollbackDetail);
					SetError(OutError,
						VerifyError.GetCode().IsEmpty()
							? TEXT("graph_compound_material_readback_failed")
							: VerifyError.GetCode(),
						TargetRef, Outcome.GetDiagnosticCode()
							+ TEXT(";") + RollbackDetail);
					return false;
				}

				const FDurableVerificationAcknowledgement Acknowledgement(
					DurableReceipt.GetJournalId(), Plan->GetFingerprint(),
					Prepared->GetInventory().GetInventoryHash(),
					Outcome.GetEvidenceId(), JournalOwner, FencingToken);
				if (!TryAcknowledgeAssetDurability(
						Persisted, Acknowledgement, FencingToken,
						*Resources, DurableError))
				{
					FString RollbackDetail;
					TryRollbackMaterialMutation(
						TargetRef, Persisted, DurableBackend, FencingToken,
						*Resources, RollbackDetail);
					SetError(OutError, DurableError.GetCode(), TargetRef,
						DurableError.GetDetail() + TEXT(";") + RollbackDetail);
					return false;
				}
				return TrySerializeMaterialMutationResult(
					TargetRef, Operations.Num(), Outcome.GetEvidenceId(),
					OutResult, OutError);
			}

			bool TryCaptureAssetCreateSnapshot(
				const FGraphCompoundDomainRouteProfile& Profile,
				const FGraphCompoundAssetCreateRequest& Request,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentCanonicalSnapshotPtr& OutSnapshot,
				FGraphCompoundRuntimeError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				const bool bNiagaraAsset =
					Request.GetInputKind()
						== EGraphCompoundTypedInputKind::NiagaraEmitterAssetCreate
					|| Request.GetInputKind()
						== EGraphCompoundTypedInputKind::NiagaraSystemAssetCreate
					|| Request.GetInputKind()
						== EGraphCompoundTypedInputKind::NiagaraAssetDuplicate;
				if (!IsInGameThread()
					|| (!bNiagaraAsset
						&& Request.GetInputKind()
							!= EGraphCompoundTypedInputKind::BlueprintAssetCreate
						&& Request.GetInputKind()
							!= EGraphCompoundTypedInputKind::UmgAssetCreate
						&& Request.GetInputKind()
							!= EGraphCompoundTypedInputKind::MaterialAssetCreate
						&& Request.GetInputKind()
							!= EGraphCompoundTypedInputKind::MaterialInstanceAssetCreate))
				{
					SetError(OutError,
						TEXT("graph_compound_asset_create_binding_unavailable"),
						Profile.GetCapabilityId());
					return false;
				}
				if (Cancellation->IsCancelled())
				{
					SetError(OutError,
						TEXT("graph_compound_asset_create_cancelled"),
						Profile.GetCapabilityId());
					return false;
				}
				if (bNiagaraAsset)
				{
					return NiagaraLifecycle.TryCaptureAssetCreateSnapshot(
						Profile, Request, *Resources, OutSnapshot, OutError);
				}
				return TryCreateAbsentSnapshot(
					Request, *Resources, OutSnapshot, OutError);
			}

			bool TryExecuteAssetCreate(
				const FGraphCompoundDomainRouteProfile& Profile,
				const FGraphCompoundAssetCreateRequest& Request,
				const FPlanningPreflightPlanRef& Plan,
				const FUeAgentCallerContext& CallerContext,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentContractValue& OutResult,
				FGraphCompoundRuntimeError& OutError) override
			{
				OutResult = FUeAgentContractValue(TEXT("{}"));
				OutError = {};
				const bool bNiagaraAsset =
					Request.GetInputKind()
						== EGraphCompoundTypedInputKind::NiagaraEmitterAssetCreate
					|| Request.GetInputKind()
						== EGraphCompoundTypedInputKind::NiagaraSystemAssetCreate
					|| Request.GetInputKind()
						== EGraphCompoundTypedInputKind::NiagaraAssetDuplicate;
				if (bNiagaraAsset)
				{
					return NiagaraLifecycle.TryExecuteAssetCreate(
						Profile, Request, Plan, CallerContext, *Resources,
						Cancellation, OutResult, OutError);
				}
				if (!IsInGameThread() || Cancellation->IsCancelled())
				{
					SetError(OutError,
						Cancellation->IsCancelled()
							? TEXT("graph_compound_asset_create_cancelled")
							: TEXT("graph_compound_asset_create_thread_invalid"),
						Profile.GetCapabilityId());
					return false;
				}
				if (Plan->GetDescriptorIdentity()
						!= Profile.GetExecutionContract().GetDescriptorIdentity()
					|| Plan->GetChangeSet().GetChanges().Num() != 1
					|| Plan->GetChangeSet().GetChanges()[0].GetIntent()
						!= EChangeIntent::Create
					|| Plan->GetChangeSet().GetChanges()[0]
						.GetIdentity().GetOwnerId() != Request.GetTargetRef())
				{
					SetError(OutError,
						TEXT("graph_compound_asset_create_plan_invalid"),
						Request.GetTargetRef());
					return false;
				}

				UClass* ParentClass = nullptr;
				UMaterialInterface* ParentMaterial = nullptr;
				if (Request.GetInputKind()
					== EGraphCompoundTypedInputKind::BlueprintAssetCreate)
				{
					ParentClass = LoadObject<UClass>(
						nullptr, *Request.GetParentClassRef());
					if (!ParentClass
						|| !FKismetEditorUtilities::CanCreateBlueprintOfClass(
							ParentClass))
					{
						SetError(OutError,
							TEXT("graph_compound_blueprint_parent_invalid"),
							Request.GetParentClassRef());
						return false;
					}
				}
				else if (Request.GetInputKind()
					== EGraphCompoundTypedInputKind::MaterialInstanceAssetCreate)
				{
					ParentMaterial = LoadObject<UMaterialInterface>(
						nullptr, *Request.GetParentMaterialRef());
					if (!ParentMaterial)
					{
						SetError(OutError,
							TEXT("graph_compound_material_parent_invalid"),
							Request.GetParentMaterialRef());
						return false;
					}
				}

				FUeAgentCanonicalSnapshotPtr Current;
				if (!TryCreateAbsentSnapshot(Request, *Resources, Current, OutError))
				{
					return false;
				}
				FPlanningPreflightError RevalidateError;
				if (!TryRevalidatePlan(
						Plan, Current.ToSharedRef(), CallerContext,
						*Resources, RevalidateError))
				{
					SetError(OutError, RevalidateError.GetCode(),
						RevalidateError.GetIdentityKey(),
						RevalidateError.GetDetail());
					return false;
				}

				const int64 FencingToken = ++LastFencingToken;
				const FString JournalOwner =
					TEXT("graph-compound:") + Plan->GetRequestId();
				const FAssetDurableRequest DurableRequest(
					Plan,
					{FAssetDurableTarget(
						Plan->GetChangeSet().GetChanges()[0].GetIdentity(),
						Request.GetTargetRef(),
						EPackageHeaderKind::Asset,
						EAssetDurableIntent::Save,
						ERollbackGrade::DurableBackup)},
					FencingToken,
					JournalOwner,
					EAtomicityMode::CompensatedSaga);
				FAssetDurableSessionPtr Prepared;
				FAssetDurableError DurableError;
				if (!TryPrepareAssetDurability(
						DurableRequest, DurableBackend, JournalRoot,
						*Resources, Prepared, DurableError)
					|| !Prepared.IsValid())
				{
					SetError(OutError, DurableError.GetCode(),
						Request.GetTargetRef(), DurableError.GetDetail());
					return false;
				}

				UPackage* Package = CreatePackage(*Request.GetTargetRef());
				UObject* CreatedAsset = nullptr;
				FString NativeErrorCode;
				FString NativeErrorDetail;
				bool bCreated = false;
				if (Package)
				{
					switch (Request.GetInputKind())
					{
					case EGraphCompoundTypedInputKind::BlueprintAssetCreate:
					{
						UBlueprint* Blueprint = nullptr;
						FBlueprintUmgGraphAdapterError NativeError;
						bCreated = NativeHook.TryCreateBlueprint(
							*Package, *Request.GetAssetName(), *ParentClass,
							Blueprint, NativeError);
						CreatedAsset = Blueprint;
						NativeErrorCode = NativeError.GetCode();
						NativeErrorDetail = NativeError.GetDetail();
						break;
					}
					case EGraphCompoundTypedInputKind::UmgAssetCreate:
					{
						UWidgetBlueprint* WidgetBlueprint = nullptr;
						FBlueprintUmgGraphAdapterError NativeError;
						bCreated = NativeHook.TryCreateWidgetBlueprint(
							*Package, *Request.GetAssetName(),
							WidgetBlueprint, NativeError);
						CreatedAsset = WidgetBlueprint;
						NativeErrorCode = NativeError.GetCode();
						NativeErrorDetail = NativeError.GetDetail();
						break;
					}
					case EGraphCompoundTypedInputKind::MaterialAssetCreate:
					{
						CreatedAsset = UeAgentMaterialAuthoring::CreateFactorySeed(
							Request.GetMaterialKind(),
							Package,
							*Request.GetAssetName(),
							RF_Public | RF_Standalone | RF_Transactional,
							NativeErrorDetail);
						const UClass* const ExpectedClass =
							Request.GetMaterialKind() == TEXT("material_function")
								? UMaterialFunction::StaticClass()
								: UMaterial::StaticClass();
						bCreated = CreatedAsset
							&& CreatedAsset->GetClass() == ExpectedClass;
						if (!bCreated)
						{
							NativeErrorCode =
								Request.GetMaterialKind()
									== TEXT("material_function")
									? FString(TEXT(
										"graph_compound_material_function_factory_failed"))
									: FString(TEXT(
										"graph_compound_material_factory_failed"));
						}
						break;
					}
					case EGraphCompoundTypedInputKind::MaterialInstanceAssetCreate:
					{
						UMaterialInstanceConstantFactoryNew* const Factory =
							NewObject<UMaterialInstanceConstantFactoryNew>();
						if (Factory)
						{
							Factory->InitialParent = ParentMaterial;
							CreatedAsset = Factory->FactoryCreateNew(
								UMaterialInstanceConstant::StaticClass(), Package,
								*Request.GetAssetName(),
								RF_Public | RF_Standalone | RF_Transactional,
								nullptr, GWarn);
						}
						UMaterialInstanceConstant* const MaterialInstance =
							Cast<UMaterialInstanceConstant>(CreatedAsset);
						bCreated = MaterialInstance
							&& CreatedAsset->GetClass()
								== UMaterialInstanceConstant::StaticClass()
							&& MaterialInstance->Parent == ParentMaterial;
						if (!bCreated)
						{
							NativeErrorCode =
								TEXT("graph_compound_material_instance_factory_failed");
						}
						break;
					}
					default:
						NativeErrorCode =
							TEXT("graph_compound_asset_create_binding_unavailable");
						break;
					}
				}

				bool bSettled = false;
				if (bCreated && CreatedAsset)
				{
					if (UBlueprint* const Blueprint = Cast<UBlueprint>(CreatedAsset))
					{
						FBlueprintUmgGraphCompileSettleReceipt Settle;
						FBlueprintUmgGraphAdapterError NativeError;
						bSettled = NativeHook.TryCompileAndSettle(
							*Blueprint, Settle, NativeError)
							&& Settle.IsCompileComplete()
							&& Settle.GetErrorCount() == 0;
						NativeErrorCode = NativeError.GetCode();
						NativeErrorDetail = NativeError.GetDetail();
					}
					else
					{
						FMaterialNiagaraGraphCompileSettleReceipt Settle;
						FMaterialNiagaraGraphAdapterError NativeError;
						bSettled = MaterialNativeHook.TryCompileAndSettleMaterial(
							*CreatedAsset, Settle, NativeError)
							&& Settle.IsCompilationComplete()
							&& Settle.GetCompileErrorCount() == 0;
						NativeErrorCode = NativeError.GetCode();
						NativeErrorDetail = NativeError.GetDetail();
					}
				}
				if (!bCreated || !CreatedAsset || !bSettled)
				{
					CreatedAsset = nullptr;
					Package = nullptr;
					FString RollbackDetail;
					TryRollbackCreate(
						Request, false, Prepared.ToSharedRef(),
						DurableBackend, FencingToken, *Resources,
						RollbackDetail);
					SetError(OutError,
						NativeErrorCode.IsEmpty()
							? TEXT("graph_compound_asset_create_failed")
							: NativeErrorCode,
						Request.GetTargetRef(),
						NativeErrorDetail + TEXT(";") + RollbackDetail);
					return false;
				}
				CreatedAsset->SetFlags(RF_Public | RF_Standalone);
				CreatedAsset->GetOutermost()->MarkPackageDirty();
				FAssetRegistryModule::AssetCreated(CreatedAsset);

				FDurableReceipt DurableReceipt;
				if (!TryPersistAssetDurability(
						Prepared.ToSharedRef(), DurableBackend,
						FencingToken, *Resources,
						DurableReceipt, DurableError))
				{
					CreatedAsset = nullptr;
					Package = nullptr;
					FString UnloadDetail;
					TryUnloadCreatedPackage(Request, true, UnloadDetail);
					SetError(OutError, DurableError.GetCode(),
						Request.GetTargetRef(),
						DurableError.GetDetail() + TEXT(";") + UnloadDetail);
					return false;
				}
				CreatedAsset->GetOutermost()->SetDirtyFlag(false);
				CreatedAsset = nullptr;
				Package = nullptr;

				const FAssetDurableSessionRef Persisted =
					PersistedSession(Prepared.ToSharedRef(), DurableReceipt);
				const FUeAgentCanonicalValuePtr& Expected =
					Plan->GetChangeSet().GetChanges()[0].GetAfter();
				TSharedPtr<FIndependentReadbackService> Readback;
				FVerificationError VerifyError;
				const TSharedRef<FAssetCreateReadbackOracle,
					ESPMode::ThreadSafe> Oracle =
					MakeShared<FAssetCreateReadbackOracle,
						ESPMode::ThreadSafe>(Request);
				FIndependentReadbackOutcome Outcome;
				if (!Expected.IsValid()
					|| !FIndependentReadbackService::TryCreate(
						{Oracle}, Readback, VerifyError)
					|| !Readback.IsValid()
					|| !Readback->TryVerify(
						FIndependentReadbackRequest(
							TEXT("graph-compound:") + Plan->GetRequestId(),
							AssetCreateReadbackOracleId,
							Request.GetTargetRef(),
							Expected.ToSharedRef()),
						*Resources, Outcome, VerifyError)
					|| Outcome.GetStatus() != EVerificationStatus::Verified)
				{
					FString RollbackDetail;
					TryRollbackCreate(
						Request, true, Persisted, DurableBackend,
						FencingToken, *Resources, RollbackDetail);
					SetError(OutError,
						VerifyError.GetCode().IsEmpty()
							? TEXT("graph_compound_asset_readback_failed")
							: VerifyError.GetCode(),
						Request.GetTargetRef(),
						Outcome.GetDiagnosticCode() + TEXT(";")
							+ RollbackDetail);
					return false;
				}

				const FDurableVerificationAcknowledgement Acknowledgement(
					DurableReceipt.GetJournalId(),
					Plan->GetFingerprint(),
					Prepared->GetInventory().GetInventoryHash(),
					Outcome.GetEvidenceId(),
					JournalOwner,
					FencingToken);
				if (!TryAcknowledgeAssetDurability(
						Persisted, Acknowledgement, FencingToken,
						*Resources, DurableError))
				{
					FString RollbackDetail;
					TryRollbackCreate(
						Request, true, Persisted, DurableBackend,
						FencingToken, *Resources, RollbackDetail);
					SetError(OutError, DurableError.GetCode(),
						Request.GetTargetRef(),
						DurableError.GetDetail() + TEXT(";")
							+ RollbackDetail);
					return false;
				}
				return TrySerializeCreateResult(
					Request.GetTargetRef(), Outcome.GetEvidenceId(),
					OutResult, OutError);
			}

			bool TryExecuteHjson(
				const FGraphCompoundDomainRouteProfile& Profile,
				const FGraphCompoundHjsonSourceRequest& Request,
				const FUeAgentDescriptorIdentity& DescriptorIdentity,
				const FString& RequestId,
				const FString& IdempotencyKey,
				const FUeAgentCallerContext& CallerContext,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentContractValue& OutResult,
				FGraphCompoundRuntimeError& OutError) override
			{
				(void)DescriptorIdentity;
				(void)IdempotencyKey;
				OutResult = FUeAgentContractValue(TEXT("{}"));
				OutError = {};
				if (!IsInGameThread())
				{
					SetError(OutError,
						TEXT("graph_compound_hjson_thread_invalid"),
						Profile.GetCapabilityId());
					return false;
				}
				if (Cancellation->IsCancelled())
				{
					SetError(OutError,
						TEXT("graph_compound_hjson_cancelled"),
						Profile.GetCapabilityId());
					return false;
				}
				if (!Config.HjsonRuntime.IsValid())
				{
					SetError(OutError,
						TEXT("graph_compound_hjson_runtime_unavailable"),
						Profile.GetCapabilityId());
					return false;
				}

				FResolvedSemanticWorkspacePtr Workspace;
				if (!TryResolveHjsonWorkspace(
						Profile, Request, RequestId, *Resources,
						Workspace, OutError)
					|| !Workspace.IsValid())
				{
					return false;
				}

				const auto Clock = []()
				{
					return FPlatformTime::Seconds();
				};
				FUeAgentCancellationSource CancellationSource(Clock);
				if (Cancellation->IsCancelled())
				{
					CancellationSource.Request(
						TEXT("graph_compound_hjson_cancelled"));
				}
				const FUeAgentOperationDeadline Deadline =
					FUeAgentOperationDeadline::FromTimeout(
						Config.OperationTimeoutSeconds, Clock);
				FUeAgentHjsonImportC3OResult OperationResult;
				FUeAgentHjsonImportTargetError ImportError;
				if (!Config.HjsonRuntime->TrySubmit(
						FUeAgentHjsonTargetImportRequest(
							Request.GetCompositionKind(),
							Request.GetTargetRef(),
							Workspace.ToSharedRef(),
							Request.GetIntent(),
							Request.GetExpectedRevision(),
							Request.IsDryRun()),
						RequestId,
						CallerContext,
						Resources,
						Deadline,
						CancellationSource.GetToken(),
						OperationResult,
						ImportError)
					|| !OperationResult.IsValid())
				{
					SetError(OutError,
						ImportError.Code.IsEmpty()
							? TEXT("graph_compound_hjson_operation_incomplete")
							: ImportError.Code,
						ImportError.SubjectId,
						ImportError.Detail);
					return false;
				}
				return TrySerializeHjsonResult(
					Request, OperationResult, OutResult, OutError);
			}

		private:
			FGraphCompoundBlueprintMaterialProductionConfig Config;
			FString JournalRoot;
			FBlueprintUmgGraphNativeHook NativeHook;
			FMaterialNiagaraGraphNativeHook MaterialNativeHook;
			FPackageAssetDurableBackend DurableBackend;
			int64 LastFencingToken = 0;
			FGraphCompoundNiagaraLifecycleProduction NiagaraLifecycle;
			FGraphCompoundNiagaraTypedMutationProduction NiagaraTypedMutation;
		};
	}

	bool TryCreateBlueprintMaterialGraphProductionEnvironment(
		const FGraphCompoundBlueprintMaterialProductionConfig& Config,
		TSharedPtr<IGraphCompoundBlueprintMaterialEnvironment>&
			OutEnvironment,
		FGraphCompoundRuntimeError& OutError)
	{
		OutEnvironment.Reset();
		OutError = {};
		if (!Config.IsValid())
		{
			SetError(OutError,
				TEXT("graph_compound_production_config_invalid"),
				TEXT("blueprint_material"));
			return false;
		}
		OutEnvironment = MakeShared<FBlueprintMaterialProductionEnvironment>(
			Config);
		return OutEnvironment.IsValid();
	}
}
