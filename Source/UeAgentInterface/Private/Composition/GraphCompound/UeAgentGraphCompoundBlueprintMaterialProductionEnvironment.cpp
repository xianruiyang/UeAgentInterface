#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialBindings.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringPackageReadback.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundNiagaraLifecycleProduction.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundNiagaraTypedMutationProduction.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
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
#include "UObject/Package.h"
#include "UObject/Class.h"
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
	using namespace PlanningPreflight;
	using namespace StateSemantic;
	using namespace Verification;

	namespace
	{
        void SetError(FGraphCompoundRuntimeError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic,
            FString SubjectId = FString(), const UE::UAI::Contract::FDiagnostic& Recovery = UE::UAI::Contract::FDiagnostic())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = MoveTemp(SubjectId);
            OutError.Append(Recovery);
        }

		const FString AssetCreateReadbackOracleId =
			TEXT("uai.readback.graph_compound.asset_create");
		const FString MaterialMutationReadbackOracleId =
			TEXT("uai.readback.graph_compound.material_mutation");

		

        

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
			if (!Request.IsValid())
			{
				SetError(OutError, Request.GetValidationDiagnostic(), Request.GetTargetRef());
				return false;
			}
			if (!TryPackageFilename(Request.GetTargetRef(), OutFilename))
			{
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_asset_target_invalid"),
					TEXT("The asset target is not a valid mounted long package name."), TEXT("graph_compound"));
				Diagnostic.PropertyPath = TEXT("target_ref");
				Diagnostic.ActualValue = MakeShared<FJsonValueString>(Request.GetTargetRef());
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("valid mounted long package name"));
				Diagnostic.TargetPath = Request.GetTargetRef();
				SetError(OutError, MoveTemp(Diagnostic), Request.GetTargetRef());
				return false;
			}
			bool bTargetPresent = false;
			if (!HjsonWorkspace::DomainAuthoring::
				TryInspectObjectTargetPresence(
					Request.GetTargetRef(), bTargetPresent))
			{
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_asset_target_invalid"),
					TEXT("The native asset presence query could not inspect the requested target."), TEXT("graph_compound"));
				Diagnostic.SourceKind = TEXT("native_result");
				Diagnostic.PropertyPath = TEXT("target_inspected");
				Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				Diagnostic.TargetPath = Request.GetTargetRef();
				SetError(OutError, MoveTemp(Diagnostic), Request.GetTargetRef());
				return false;
			}
			if (bTargetPresent)
			{
				SetError(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_asset_target_already_exists"), TEXT("target_ref already resolves to an existing asset."), TEXT("graph_compound"), FString()), Request.GetTargetRef());
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
				SetError(OutError, Error, Request.GetTargetRef());
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
			UE::UAI::Contract::FDiagnostic& OutDiagnostic)
		{
			OutDiagnostic.Reset();
			UObject* const Asset =
				LoadObject<UObject>(nullptr, *ObjectPath(Request));
			if (!Asset)
			{
				OutDiagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_asset_reload_failed"), TEXT("graph compound asset reload failed."), TEXT("native_readback"));
				return false;
			}
			if (Asset->GetOutermost()->IsDirty())
			{
				OutDiagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_asset_reload_state_mismatch"), TEXT("graph compound asset reload state mismatch."), TEXT("native_readback"));
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
					OutDiagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_asset_reload_class_mismatch"), TEXT("graph compound asset reload class mismatch."), TEXT("native_readback"));
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
					OutDiagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_asset_reload_state_mismatch"), TEXT("graph compound asset reload state mismatch."), TEXT("native_readback"));
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
				OutDiagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_asset_reload_class_mismatch"), TEXT("graph compound asset reload class mismatch."), TEXT("native_readback"));
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
				OutDiagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_asset_reload_state_mismatch"), TEXT("graph compound asset reload state mismatch."), TEXT("native_readback"));
				return false;
			}
			if (!bBlueprintRoute
				&& !CastChecked<UWidgetBlueprint>(Blueprint)->WidgetTree)
			{
				OutDiagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_widget_tree_reload_missing"), TEXT("graph compound widget tree reload missing."), TEXT("native_readback"));
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
				UE::UAI::Contract::FDiagnostic Detail;
				if (!UeAgentAuthoringOperationApply::TryReloadPersistedPackage(Request.GetTargetRef(), Detail))
				{
					OutError = MoveTemp(Detail);
					return false;
				}
				if (!LoadObject<UObject>(nullptr, *ObjectPath(Request)))
				{
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_asset_reload_failed"), TEXT("The persisted asset could not be loaded for independent readback."), TEXT("verification"));
					Diagnostic.PropertyPath = TEXT("asset_available");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = Request.GetTargetRef();
					SetError(OutError, MoveTemp(Diagnostic), Request.GetTargetRef());
					return false;
				}
				UE::UAI::Contract::FDiagnostic Diagnostic;
				if (!TryValidateReloadedAsset(Request, Diagnostic))
				{
					OutObservation =
						FIndependentReadbackObservation::Unavailable(Diagnostic, TEXT("reload:") + Request.GetTargetRef());
					return true;
				}
				FUeAgentCanonicalValuePtr Actual;
				FUeAgentStateSemanticError Error;
				if (!FUeAgentCanonicalValue::TryString(
						ExpectedDefinition(Request), Resources, Actual, Error)
					|| !Actual.IsValid())
				{
					OutError = Error;
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
			UE::UAI::Contract::FDiagnostic& OutDetail)
		{
			return UeAgentAuthoringOperationApply::TryRestorePackageDurability(
				Request.GetTargetRef(), bNotifyAssetRegistry, Session, Backend, FencingToken,
				Resources, OutDetail);
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
			if (!FMath::IsFinite(Value))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_material_number_invalid"),
					TEXT("The material parameter value must be finite."), TEXT("native_graph"));
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("parameter_value");
				OutError.ActualValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Value));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("finite number"));
				OutError.SubjectId = Subject;
				return false;
			}
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryDecimal(
				FString::SanitizeFloat(Value), Resources, OutValue, Error))
			{
				OutError = MoveTemp(Error);
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
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
				OutError = Error;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
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
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_material_parameter_type_unsupported"), TEXT("The material parameter type cannot be represented by the graph state adapter."), TEXT("graph_compound"));
				Diagnostic.PropertyPath = TEXT("parameter_type");
				Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Parameter.GetIdentity().GetParameterType()));
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("Scalar, Vector, Texture, or StaticSwitch"));
				Diagnostic.SourceKind = TEXT("native_result");
				SetError(OutError, MoveTemp(Diagnostic), Subject);
				return false;
			}
			OutError = Error;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
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
				SetError(OutError, Error, Parameter.GetIdentity().GetParameterKey());
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
				SetError(OutError, Entry.GetValidationDiagnostic(), SemanticId);
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
				SetError(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_material_target_invalid"), TEXT("target_ref does not identify the required material asset."), TEXT("graph_compound"), Profile.GetTargetKind()), TargetRef);
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
					if (NativeError.TargetPath.IsEmpty()) NativeError.TargetPath = TargetRef;
					SetError(OutError, NativeError, TargetRef);
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
					if (NativeError.TargetPath.IsEmpty()) NativeError.TargetPath = TargetRef;
					SetError(OutError, NativeError, TargetRef);
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
							auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_material_item_identity_invalid"), TEXT("The material item contains an invalid native expression GUID."), TEXT("graph_compound"));
							Diagnostic.PropertyPath = TEXT("expression_guid_valid");
							Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
							Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
							Diagnostic.SourceKind = TEXT("native_result");
							Diagnostic.TargetPath = TargetRef;
							SetError(OutError, MoveTemp(Diagnostic), Item.GetItemKey());
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
							OutError = StateError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Item.GetItemKey();
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
				OutError = StateError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = TargetRef;
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
			FString CandidateName;
			TMap<FString, FString> CandidateSelect;
			FString CandidateId;
			FString GraphName;
			FString NodeGuid;
			FString PinGuid;
			FString PinName;
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

		bool RejectMaterialInput(FGraphCompoundRuntimeError& OutError, const FString& Code,
			const FString& Reason, const FString& Pointer, const FUeAgentCanonicalValue* Actual,
			const FString& Expected)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("graph_compound"));
			OutError.PropertyPath = Pointer;
			OutError.Source.JsonPath = Pointer;
			OutError.SemanticPointer = Pointer;
			OutError.ActualValue = CanonicalInputFact(Actual);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			OutError.ActualValuePresent = Actual != nullptr;
			OutError.ExpectedValuePresent = true;
			return false;
		}

		bool TryReadMaterialObjectFields(const FUeAgentCanonicalValue& Value,
			std::initializer_list<const TCHAR*> Fields, const bool bRequired,
			const FString& Pointer, const TCHAR* Code, FGraphCompoundRuntimeError& OutError)
		{
			if (Value.GetKind() != EUeAgentValueKind::Object)
				return RejectMaterialInput(OutError, Code, TEXT("The material edit value must be an object."),
					Pointer, &Value, TEXT("object"));
			if (bRequired)
				for (const TCHAR* Field : Fields)
					if (!FindObjectValue(Value, Field))
						return RejectMaterialInput(OutError, Code, TEXT("A required material edit field is missing."),
							Pointer + TEXT("/") + Field, nullptr, TEXT("present field"));
			for (const auto& Item : Value.GetObjectItems())
			{
				bool bAllowed = false;
				for (const TCHAR* Field : Fields) if (Item.Key == Field) { bAllowed = true; break; }
				if (!bAllowed)
				{
					const FString Token = Item.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
					return RejectMaterialInput(OutError, Code, TEXT("The material edit contains an unsupported field."),
						Pointer + TEXT("/") + Token, &Item.Value.Get(), TEXT("field absent"));
				}
			}
			return true;
		}

		bool TryReadMaterialCandidateSelector(
			const FUeAgentCanonicalValue& Value,
			FString& OutName,
			TMap<FString, FString>& OutSelect,
			FString& OutId,
			FGraphCompoundRuntimeError& OutError)
		{
			OutName.Reset();
			OutSelect.Reset();
			OutId.Reset();
			const auto* Candidate = FindObjectValue(Value, TEXT("candidate"));
			const TCHAR* Code = TEXT("graph_compound_material_item_invalid");
			if (!Candidate || Candidate->GetKind() != EUeAgentValueKind::Object)
				return RejectMaterialInput(OutError, Code, TEXT("The material candidate selector must be an object."),
					TEXT("/candidate"), Candidate, TEXT("object"));
			const auto* Name = FindObjectValue(*Candidate, TEXT("name"));
			const auto* Id = FindObjectValue(*Candidate, TEXT("id"));
			const auto* Select = FindObjectValue(*Candidate, TEXT("select"));
			if (Name && Name->GetKind() != EUeAgentValueKind::String)
				return RejectMaterialInput(OutError, Code, TEXT("The candidate name must be a string."),
					TEXT("/candidate/name"), Name, TEXT("string"));
			if (Id && Id->GetKind() != EUeAgentValueKind::String)
				return RejectMaterialInput(OutError, Code, TEXT("The candidate id must be a string."),
					TEXT("/candidate/id"), Id, TEXT("string"));
			if (Select && Select->GetKind() != EUeAgentValueKind::Object)
				return RejectMaterialInput(OutError, Code, TEXT("The candidate select field must be an object."),
					TEXT("/candidate/select"), Select, TEXT("object"));
			if (const auto* LegacyId = FindObjectValue(*Candidate, TEXT("candidate_id")))
				return RejectMaterialInput(OutError, Code, TEXT("candidate_id is not supported inside a material candidate selector."),
					TEXT("/candidate/candidate_id"), LegacyId, TEXT("use id"));
			OutName = Name ? Name->GetText().TrimStartAndEnd() : FString();
			OutId = Id ? Id->GetText().TrimStartAndEnd() : FString();
			if (Select)
			{
				for (const auto& Field : Select->GetObjectItems())
				{
					if (Field.Value->GetKind() != EUeAgentValueKind::String)
					{
						const FString Token = Field.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
						return RejectMaterialInput(OutError, Code, TEXT("Candidate disambiguation values must be strings."),
							FString(TEXT("/candidate/select/")) + Token, &Field.Value.Get(), TEXT("string"));
					}
					OutSelect.Add(Field.Key, Field.Value->GetText());
				}
				auto JsonCandidate = MakeShared<FJsonObject>();
				JsonCandidate->SetStringField(TEXT("name"), OutName);
				auto JsonSelect = MakeShared<FJsonObject>();
				for (const TPair<FString, FString>& Field : OutSelect)
					JsonSelect->SetStringField(Field.Key, Field.Value);
				JsonCandidate->SetObjectField(TEXT("select"), JsonSelect);
				HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector Parsed;
				HjsonWorkspace::FWorkspaceError SelectorError;
				if (!HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(
					JsonCandidate, TEXT("candidate"), Parsed, SelectorError))
				{
					OutError = SelectorError.GetDiagnostic();
					return false;
				}
				OutName = MoveTemp(Parsed.Name);
				OutSelect = MoveTemp(Parsed.Select);
			}
			if (!OutName.IsEmpty() || !OutId.IsEmpty()) return true;
			return RejectMaterialInput(OutError, Code, TEXT("The candidate selector requires a nonempty name or id."),
				TEXT("/candidate"), Candidate, TEXT("nonempty name or id"));
		}

		bool TryReadOptionalMaterialCandidateContext(
			const FUeAgentCanonicalValue& Value,
			FMaterialMutationOperation& OutOperation,
			FGraphCompoundRuntimeError& OutError)
		{
			const auto* Context = FindObjectValue(Value, TEXT("candidate_context"));
			if (!Context) return true;
			const TCHAR* Code = TEXT("graph_compound_material_item_invalid");
			if (!TryReadMaterialObjectFields(*Context,
				{TEXT("graph_name"), TEXT("node_guid"), TEXT("pin_guid"), TEXT("pin_name")},
				false, TEXT("/value/candidate_context"), Code, OutError)) return false;
			auto ReadOptional = [Context, Code, &OutError](const TCHAR* Field, FString& OutValue)
			{
				const auto* FieldValue = FindObjectValue(*Context, Field);
				if (!FieldValue) { OutValue.Reset(); return true; }
				if (FieldValue->GetKind() != EUeAgentValueKind::String)
					return RejectMaterialInput(OutError, Code, TEXT("The material candidate context field must be a string."),
						FString(TEXT("/value/candidate_context/")) + Field, FieldValue, TEXT("string"));
				OutValue = FieldValue->GetText().TrimStartAndEnd();
				return true;
			};
			return ReadOptional(TEXT("graph_name"), OutOperation.GraphName)
				&& ReadOptional(TEXT("node_guid"), OutOperation.NodeGuid)
				&& ReadOptional(TEXT("pin_guid"), OutOperation.PinGuid)
				&& ReadOptional(TEXT("pin_name"), OutOperation.PinName);
		}

		bool TryCanonicalLinearColor(
			const FUeAgentCanonicalValue& Value,
			FLinearColor& OutColor,
			FGraphCompoundRuntimeError* OutError = nullptr,
			const FString& Pointer = FString())
		{
			if (Value.GetKind() != EUeAgentValueKind::Array
				|| Value.GetArrayItems().Num() != 4)
			{
				if (OutError) RejectMaterialInput(*OutError, TEXT("graph_compound_material_color_invalid"),
					TEXT("A linear color requires exactly four numeric components."), Pointer, &Value, TEXT("array of four finite numbers"));
				return false;
			}
			double Components[4] = {};
			for (int32 Index = 0; Index < 4; ++Index)
			{
				if (!TryCanonicalNumberValue(
						Value.GetArrayItems()[Index].Get(), Components[Index]))
				{
					if (OutError) RejectMaterialInput(*OutError, TEXT("graph_compound_material_color_invalid"),
						TEXT("The linear color component must be a finite number."), Pointer + TEXT("/") + ::LexToString(Index),
						&Value.GetArrayItems()[Index].Get(), TEXT("finite number"));
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

		const FUeAgentCanonicalValue* TryGetParameterEffectiveValue(
			const FUeAgentCanonicalValue& State, FGraphCompoundRuntimeError& OutError)
		{
			if (!TryReadMaterialObjectFields(State, {TEXT("effective_value"), TEXT("override_state")},
				true, FString(), TEXT("graph_compound_material_parameter_state_invalid"), OutError))
			{
				OutError.SourceKind = TEXT("native_result");
				OutError.Source.JsonPath.Reset();
				OutError.SemanticPointer.Reset();
				return nullptr;
			}
			const FUeAgentCanonicalValue* const OverrideState = FindObjectValue(State, TEXT("override_state"));
			if (OverrideState->GetKind() != EUeAgentValueKind::String || OverrideState->GetText() != TEXT("overridden"))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_material_parameter_state_invalid"),
					TEXT("The proposed parameter state must explicitly represent an overridden value."), TEXT("graph_compound"));
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("override_state");
				OutError.ActualValue = CanonicalInputFact(OverrideState);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("overridden"));
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
			const FString& ValueType,
			FGraphCompoundRuntimeError& OutError)
		{
			FProperty* const Property = ExpressionClass.FindPropertyByName(*PropertyPath);
			const FStructProperty* const StructProperty = CastField<FStructProperty>(Property);
			const bool bTypeMatches = ValueType == TEXT("linear_color")
				? StructProperty && StructProperty->Struct == TBaseStructure<FLinearColor>::Get()
				: ValueType == TEXT("name") && CastField<FNameProperty>(Property);
			if (Property && Property->HasAnyPropertyFlags(CPF_Edit) && bTypeMatches) return true;
			OutError = UE::UAI::Contract::FDiagnostic::Failure(
				TEXT("graph_compound_material_property_invalid"),
				!Property ? TEXT("The material expression class has no property with the requested name.")
					: !Property->HasAnyPropertyFlags(CPF_Edit) ? TEXT("The requested material expression property is not editable.")
					: TEXT("The reflected property type does not match the declared value type."), TEXT("graph_compound"));
			OutError.Source.JsonPath = TEXT("/value/property_path");
			OutError.SemanticPointer = OutError.Source.JsonPath;
			OutError.PropertyPath = PropertyPath;
			OutError.ActualValue = MakeShared<FJsonValueString>(
				!Property || !Property->HasAnyPropertyFlags(CPF_Edit) ? PropertyPath : Property->GetCPPType());
			OutError.ExpectedValue = MakeShared<FJsonValueString>(
				!Property || !Property->HasAnyPropertyFlags(CPF_Edit) ? TEXT("existing editable property") : *ValueType);
			return false;
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
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_material_edit_identity_duplicate"),
						TEXT("More than one prepared material operation uses the same identity."), TEXT("graph_compound"));
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.PropertyPath = TEXT("operation_identity");
					Diagnostic.ActualValue = MakeShared<FJsonValueString>(Operations[Index].SemanticId);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("unique operation identity"));
					SetError(OutError, MoveTemp(Diagnostic), Operations[Index].SemanticId);
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
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_material_dependency_cycle"),
						TEXT("A dependency cycle prevents the remaining material edits from being ordered."), TEXT("graph_compound"));
					TArray<TSharedPtr<FJsonValue>> Pending;
					for (const int32 PendingIndex : Remaining)
					{
						const auto& PendingOperation = Operations[PendingIndex];
						auto Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("identity"), PendingOperation.SemanticId);
						TArray<TSharedPtr<FJsonValue>> Dependencies;
						for (const FString& Dependency : PendingOperation.Dependencies)
							if (Indices.Contains(Dependency) && !Completed.Contains(Dependency))
								Dependencies.Add(MakeShared<FJsonValueString>(Dependency));
						Item->SetArrayField(TEXT("unresolved_dependencies"), Dependencies);
						Pending.Add(MakeShared<FJsonValueObject>(Item));
					}
					Diagnostic.PropertyPath = TEXT("/depends_on");
					Diagnostic.Source.JsonPath = Diagnostic.PropertyPath;
					Diagnostic.SemanticPointer = Diagnostic.PropertyPath;
					Diagnostic.ActualValue = MakeShared<FJsonValueArray>(Pending);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("acyclic material edit dependencies"));
					SetError(OutError, MoveTemp(Diagnostic), Operations[Remaining[0]].SemanticId);
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
			auto Adopt = [&](const FString& SemanticId, const FString& InputSuffix = FString(),
				const FString& ValueRoot = FString(TEXT("/")))
			{
				if (const FString* Pointer = Payload.FindInputPointer(SemanticId))
					BindGraphCompoundInputValueDiagnostic(OutError, *Pointer + InputSuffix, ValueRoot);
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SemanticId;
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
				return false;
			};
			auto Reject = [&](const TCHAR* Code, const FString& Reason, const FString& SemanticId,
				const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
				const bool bNative = false)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("graph_compound"));
				OutError.SubjectId = SemanticId;
				OutError.TargetPath = TargetRef;
				OutError.PropertyPath = Field;
				OutError.ActualValuePresent = Actual.IsValid();
				OutError.ExpectedValuePresent = Expected.IsValid();
				OutError.ActualValue = MoveTemp(Actual);
				OutError.ExpectedValue = MoveTemp(Expected);
				if (bNative) OutError.SourceKind = TEXT("native_result");
				else if (Payload.FindInputPointer(SemanticId))
				{
					OutError.Source.JsonPath = Field;
					OutError.SemanticPointer = Field;
					Adopt(SemanticId);
				}
				return false;
			};
			if (Plan->GetDescriptorIdentity() != Profile.GetExecutionContract().GetDescriptorIdentity())
				return Reject(TEXT("graph_compound_material_plan_invalid"), TEXT("The material plan descriptor differs from the execution profile."), FString(), TEXT("descriptor_matches"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true);
			if (Edits.IsEmpty())
				return Reject(TEXT("graph_compound_material_plan_invalid"), TEXT("The material mutation contains no edits."), FString(), TEXT("edit_count"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one edit")), true);
			if (Edits.Num() != Changes.Num())
				return Reject(TEXT("graph_compound_material_plan_invalid"), TEXT("The number of material edits differs from the number of planned changes."), FString(), TEXT("change_count"), MakeShared<FJsonValueNumber>(Changes.Num()), MakeShared<FJsonValueNumber>(Edits.Num()), true);

			TMap<FString, const FPlannedChange*> PlannedById;
			for (const FPlannedChange& Change : Changes)
			{
				if (Change.GetIdentity().GetOwnerId() != TargetRef)
					return Reject(TEXT("graph_compound_material_plan_invalid"), TEXT("A planned material change targets a different owner."), Change.GetIdentity().GetSemanticId(), TEXT("owner_matches_target"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true);
				if (PlannedById.Contains(Change.GetIdentity().GetSemanticId()))
					return Reject(TEXT("graph_compound_material_plan_invalid"), TEXT("The plan repeats a material edit identity."), Change.GetIdentity().GetSemanticId(), TEXT("planned_identity"), MakeShared<FJsonValueString>(Change.GetIdentity().GetSemanticId()), MakeShared<FJsonValueString>(TEXT("unique edit identity")), true);
				PlannedById.Add(Change.GetIdentity().GetSemanticId(), &Change);
			}

			TMap<FString, UClass*> ProposedClasses;
			for (const FGraphCompoundCanonicalEdit& Edit : Edits)
			{
				const FString SemanticId = Edit.GetIdentity().GetSemanticId();
				const FPlannedChange* const* const Planned =
					PlannedById.Find(SemanticId);
				if (!Planned || !*Planned)
					return Reject(TEXT("graph_compound_material_plan_invalid"), TEXT("The material edit has no matching planned change."), SemanticId, TEXT("planned_change_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true);
				if ((*Planned)->GetIntent() != Edit.GetIntent())
					return Reject(TEXT("graph_compound_material_plan_invalid"), TEXT("The planned change intent differs from the material edit intent."), SemanticId, TEXT("intent"), MakeShared<FJsonValueNumber>(static_cast<int32>((*Planned)->GetIntent())), MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetIntent())), true);
				if (Edit.GetIdentity().GetOwnerId() != TargetRef)
					return Reject(TEXT("graph_compound_material_plan_invalid"), TEXT("The material edit owner differs from the execution target."), SemanticId, TEXT("owner_matches_target"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true);
				if (!Edit.GetProposedAfter().IsValid())
					return Reject(TEXT("graph_compound_material_plan_invalid"), TEXT("The material edit has no proposed value."), SemanticId, TEXT("proposed_value_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true);

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
					if (Edit.GetElementKind() != EGraphCompoundElementKind::Property)
						return Reject(TEXT("graph_compound_material_parameter_invalid"), TEXT("A material instance parameter edit must be a property edit."), SemanticId, TEXT("element_kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetElementKind())), MakeShared<FJsonValueString>(TEXT("property")), true);
					if (Edit.GetIntent() != EChangeIntent::Update)
						return Reject(TEXT("graph_compound_material_parameter_invalid"), TEXT("A material instance parameter edit must use update intent."), SemanticId, TEXT("intent"), MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetIntent())), MakeShared<FJsonValueString>(TEXT("update")), true);
					if (!Operation.Dependencies.IsEmpty())
						return Reject(TEXT("graph_compound_material_parameter_invalid"), TEXT("A material instance parameter edit cannot declare dependencies."), SemanticId, TEXT("dependency_count"), MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()), MakeShared<FJsonValueNumber>(0), true);
					if (!TryParseParameterSemanticId(SemanticId, Operation.ParameterInfo, Operation.ParameterType))
						return Reject(TEXT("graph_compound_material_parameter_invalid"), TEXT("The material parameter identity cannot be parsed."), SemanticId, TEXT("parameter_identity"), MakeShared<FJsonValueString>(SemanticId), MakeShared<FJsonValueString>(TEXT("valid material parameter identity")), true);
					OutOperations.Add(MoveTemp(Operation));
					continue;
				}

				if (Edit.GetElementKind() == EGraphCompoundElementKind::Item)
				{
					Operation.Kind = EMaterialMutationOperationKind::Item;
					const FUeAgentCanonicalValue& Value = *Operation.Value;
					const FUeAgentCanonicalValue* const Position =
						FindObjectValue(Value, TEXT("position"));
					const TCHAR* ItemCode = TEXT("graph_compound_material_item_invalid");
					if (Edit.GetIntent() != EChangeIntent::Create)
						return Reject(TEXT("graph_compound_material_item_invalid"), TEXT("A material expression item must use create intent."), SemanticId, TEXT("/intent"), MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetIntent())), MakeShared<FJsonValueString>(TEXT("create")));
					if (!Operation.Dependencies.IsEmpty())
						return Reject(TEXT("graph_compound_material_item_invalid"), TEXT("A new material expression item cannot declare dependencies."), SemanticId, TEXT("/depends_on"), MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()), MakeShared<FJsonValueNumber>(0));
					if (!SemanticId.StartsWith(TEXT("expression:")) || !FGuid::Parse(SemanticId.RightChop(11), Operation.ExpressionGuid))
						return Reject(TEXT("graph_compound_material_item_invalid"), TEXT("The expression identity must contain a valid GUID after expression:."), SemanticId, TEXT("/identity"), MakeShared<FJsonValueString>(SemanticId), MakeShared<FJsonValueString>(TEXT("expression:<guid>")));
					if (!TryReadMaterialObjectFields(Value,
						{TEXT("candidate"), TEXT("candidate_context"), TEXT("class_ref"), TEXT("position")},
						false, TEXT("/value"), ItemCode, OutError)) return Adopt(SemanticId);
					if (!TryReadMaterialCandidateSelector(Value, Operation.CandidateName, Operation.CandidateSelect, Operation.CandidateId, OutError))
						return Adopt(SemanticId, TEXT("/value/candidate"), TEXT("/candidate"));
					if (!TryReadOptionalMaterialCandidateContext(Value, Operation, OutError)) return Adopt(SemanticId);
					if (Position)
					{
						if (!TryReadMaterialObjectFields(*Position, {TEXT("x"), TEXT("y")},
							true, TEXT("/value/position"), ItemCode, OutError)) return Adopt(SemanticId);
						if (!TryCanonicalIntegerField(*Position, TEXT("x"), Operation.PositionX))
							return Reject(TEXT("graph_compound_material_item_invalid"), TEXT("The material expression x position must be an int32 integer."), SemanticId, TEXT("/value/position/x"), CanonicalInputFact(FindObjectValue(*Position, TEXT("x"))), MakeShared<FJsonValueString>(TEXT("int32 integer")));
						if (!TryCanonicalIntegerField(*Position, TEXT("y"), Operation.PositionY))
							return Reject(TEXT("graph_compound_material_item_invalid"), TEXT("The material expression y position must be an int32 integer."), SemanticId, TEXT("/value/position/y"), CanonicalInputFact(FindObjectValue(*Position, TEXT("y"))), MakeShared<FJsonValueString>(TEXT("int32 integer")));
					}
					if (FindExpressionBySemanticId(Target, SemanticId))
						return Reject(TEXT("graph_compound_material_item_invalid"), TEXT("A material expression already exists with the requested identity."), SemanticId, TEXT("/identity"), MakeShared<FJsonValueString>(SemanticId), MakeShared<FJsonValueString>(TEXT("unused expression identity")));
					if (FindObjectValue(Value, TEXT("class_ref")) && !TryCanonicalStringField(Value, TEXT("class_ref"), Operation.ClassRef))
						return Reject(TEXT("graph_compound_material_class_assertion_invalid"), TEXT("The expression class assertion must be a nonempty string."), SemanticId, TEXT("/value/class_ref"), CanonicalInputFact(FindObjectValue(Value, TEXT("class_ref"))), MakeShared<FJsonValueString>(TEXT("nonempty string")));

					UeAgentPaletteSearch::FSearchParams CandidateParams;
					CandidateParams.CatalogType = TEXT("material_node");
					CandidateParams.AssetPath = Target.GetPathName();
					CandidateParams.GraphName = Operation.GraphName.IsEmpty()
						? (Cast<UMaterial>(&Target)
							? FString(TEXT("MaterialGraph"))
							: FString(TEXT("MaterialFunctionGraph")))
						: Operation.GraphName;
					CandidateParams.NodeGuid = Operation.NodeGuid;
					CandidateParams.PinGuid = Operation.PinGuid;
					CandidateParams.PinName = Operation.PinName;
					UeAgentPaletteSearch::FCandidate ResolvedCandidate;
					UE::UAI::Contract::FDiagnostic ResolveError;
					if (!UeAgentPaletteSearch::TryResolveMaterialActionCandidate(
							CandidateParams,
							Operation.CandidateName,
							Operation.CandidateSelect,
							Operation.CandidateId,
							ResolvedCandidate,
							ResolveError))
					{
						SetError(OutError, MoveTemp(ResolveError), SemanticId);
						return false;
					}
					if (!ResolvedCandidate.bSupportedInContext)
					{
						SetError(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(Cast<UMaterialFunction>(&Target)
								? TEXT("material_function_native_action_lifecycle_unsupported")
								: TEXT("material_action_candidate_not_executable_in_context"), ResolvedCandidate.SupportDiagnostic), SemanticId);
						return false;
					}
					const FString ResolvedClass =
						ResolvedCandidate.AuthoringSelectors.FindRef(
							TEXT("expression_class"));
					if (ResolvedClass.IsEmpty()
						|| (!Operation.ClassRef.IsEmpty()
							&& !Operation.ClassRef.Equals(
								ResolvedClass, ESearchCase::CaseSensitive)))
					{
						return Reject(TEXT("graph_compound_material_candidate_class_mismatch"),
							ResolvedClass.IsEmpty() ? TEXT("The candidate does not identify an expression class.")
								: TEXT("The candidate expression class differs from the requested class assertion."),
							SemanticId, Operation.ClassRef.IsEmpty() ? TEXT("/value/candidate") : TEXT("/value/class_ref"), MakeShared<FJsonValueString>(ResolvedClass),
							MakeShared<FJsonValueString>(Operation.ClassRef.IsEmpty() ? TEXT("nonempty expression class") : Operation.ClassRef));
					}
					Operation.ClassRef = ResolvedClass;
					Operation.CandidateName = ResolvedCandidate.Name;
					Operation.CandidateId = ResolvedCandidate.GetPublicId();
					Operation.GraphName = CandidateParams.GraphName;
					UClass* const ExpressionClass =
						LoadObject<UClass>(nullptr, *ResolvedClass);
					if (!ExpressionClass
						|| !ExpressionClass->IsChildOf(
							UMaterialExpression::StaticClass())
						|| ExpressionClass->HasAnyClassFlags(CLASS_Abstract))
					{
						return Reject(TEXT("graph_compound_material_expression_class_invalid"),
							!ExpressionClass ? TEXT("The candidate expression class could not be loaded.")
								: !ExpressionClass->IsChildOf(UMaterialExpression::StaticClass())
									? TEXT("The candidate class is not a MaterialExpression subclass.")
									: TEXT("The candidate expression class is abstract."),
							SemanticId, TEXT("/value/candidate"), MakeShared<FJsonValueString>(Operation.ClassRef),
							MakeShared<FJsonValueString>(TEXT("loadable concrete MaterialExpression subclass")));
					}
					ProposedClasses.Add(SemanticId, ExpressionClass);
				}
				else if (Edit.GetElementKind()
					== EGraphCompoundElementKind::Property)
				{
					Operation.Kind = EMaterialMutationOperationKind::Property;
					const FUeAgentCanonicalValue& Value = *Operation.Value;
					if (Edit.GetIntent() != EChangeIntent::Create && Edit.GetIntent() != EChangeIntent::Update)
						return Reject(TEXT("graph_compound_material_property_fields_invalid"), TEXT("A material property edit must use create or update intent."), SemanticId, TEXT("/intent"), MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetIntent())), MakeShared<FJsonValueString>(TEXT("create or update")));
					if (Operation.Dependencies.Num() != 1)
						return Reject(TEXT("graph_compound_material_property_fields_invalid"), TEXT("A material property edit requires exactly one owner dependency."), SemanticId, TEXT("/depends_on"), MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()), MakeShared<FJsonValueNumber>(1));
					if (!TryReadMaterialObjectFields(Value, {TEXT("property_path"), TEXT("value_type"), TEXT("value")},
						true, TEXT("/value"), TEXT("graph_compound_material_property_fields_invalid"), OutError)) return Adopt(SemanticId);
					if (!TryCanonicalStringField(Value, TEXT("property_path"), Operation.PropertyPath))
						return Reject(TEXT("graph_compound_material_property_fields_invalid"), TEXT("The material property path must be a nonempty string."), SemanticId, TEXT("/value/property_path"), CanonicalInputFact(FindObjectValue(Value, TEXT("property_path"))), MakeShared<FJsonValueString>(TEXT("nonempty string")));
					if (!TryCanonicalStringField(Value, TEXT("value_type"), Operation.ValueType))
						return Reject(TEXT("graph_compound_material_property_fields_invalid"), TEXT("The material property value type must be a nonempty string."), SemanticId, TEXT("/value/value_type"), CanonicalInputFact(FindObjectValue(Value, TEXT("value_type"))), MakeShared<FJsonValueString>(TEXT("nonempty string")));
					Operation.OwnerItemRef = Operation.Dependencies[0];
					if (SemanticId != TEXT("property:")
							+ Operation.OwnerItemRef + TEXT(":")
							+ Operation.PropertyPath)
					{
						return Reject(TEXT("graph_compound_material_property_identity_invalid"), TEXT("The material property identity does not match its owner and property path."), SemanticId, TEXT("/identity"), MakeShared<FJsonValueString>(SemanticId), MakeShared<FJsonValueString>(TEXT("property:") + Operation.OwnerItemRef + TEXT(":") + Operation.PropertyPath));
					}
					if (Operation.ValueType != TEXT("linear_color")
						&& Operation.ValueType != TEXT("name"))
					{
						return Reject(TEXT("graph_compound_material_property_value_type_invalid"), TEXT("The material property value type is unsupported."), SemanticId, TEXT("/value/value_type"), MakeShared<FJsonValueString>(Operation.ValueType), MakeShared<FJsonValueString>(TEXT("linear_color or name")));
					}
				}
				else if (Edit.GetElementKind()
					== EGraphCompoundElementKind::Relation)
				{
					Operation.Kind = EMaterialMutationOperationKind::Relation;
					const FUeAgentCanonicalValue& Value = *Operation.Value;
					if (Edit.GetIntent() != EChangeIntent::Create)
						return Reject(TEXT("graph_compound_material_relation_fields_invalid"), TEXT("A material connection edit must use create intent."), SemanticId, TEXT("/intent"), MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetIntent())), MakeShared<FJsonValueString>(TEXT("create")));
					if (!TryReadMaterialObjectFields(Value,
						{TEXT("from_item_ref"), TEXT("from_output_index"), TEXT("to_item_ref"), TEXT("to_input_ref")},
						true, TEXT("/value"), TEXT("graph_compound_material_relation_fields_invalid"), OutError)) return Adopt(SemanticId);
					if (!TryCanonicalStringField(Value, TEXT("from_item_ref"), Operation.FromItemRef))
						return Reject(TEXT("graph_compound_material_relation_fields_invalid"), TEXT("The connection source item must be a nonempty reference."), SemanticId, TEXT("/value/from_item_ref"), CanonicalInputFact(FindObjectValue(Value, TEXT("from_item_ref"))), MakeShared<FJsonValueString>(TEXT("nonempty string")));
					if (!TryCanonicalIntegerField(Value, TEXT("from_output_index"), Operation.FromOutputIndex) || Operation.FromOutputIndex < 0)
						return Reject(TEXT("graph_compound_material_relation_fields_invalid"), TEXT("The connection output index must be a nonnegative int32 integer."), SemanticId, TEXT("/value/from_output_index"), CanonicalInputFact(FindObjectValue(Value, TEXT("from_output_index"))), MakeShared<FJsonValueString>(TEXT("nonnegative int32 integer")));
					if (!TryCanonicalStringField(Value, TEXT("to_item_ref"), Operation.ToItemRef))
						return Reject(TEXT("graph_compound_material_relation_fields_invalid"), TEXT("The connection destination item must be a nonempty reference."), SemanticId, TEXT("/value/to_item_ref"), CanonicalInputFact(FindObjectValue(Value, TEXT("to_item_ref"))), MakeShared<FJsonValueString>(TEXT("nonempty string")));
					if (!TryCanonicalStringField(Value, TEXT("to_input_ref"), Operation.ToInputRef))
						return Reject(TEXT("graph_compound_material_relation_fields_invalid"), TEXT("The connection destination input must be a nonempty reference."), SemanticId, TEXT("/value/to_input_ref"), CanonicalInputFact(FindObjectValue(Value, TEXT("to_input_ref"))), MakeShared<FJsonValueString>(TEXT("nonempty string")));
					const FString ExpectedId = FString::Printf(
						TEXT("link:%s:%d->%s:%s"), *Operation.FromItemRef,
						Operation.FromOutputIndex, *Operation.ToItemRef,
						*Operation.ToInputRef);
					if (SemanticId != ExpectedId)
					{
						return Reject(TEXT("graph_compound_material_relation_identity_invalid"), TEXT("The connection identity does not match its declared endpoints."), SemanticId, TEXT("/identity"), MakeShared<FJsonValueString>(SemanticId), MakeShared<FJsonValueString>(ExpectedId));
					}
				}
				else
				{
					return Reject(TEXT("graph_compound_material_edit_kind_invalid"), TEXT("This material mutation binding does not support the edit element kind."), SemanticId, TEXT("/element_kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetElementKind())), MakeShared<FJsonValueString>(TEXT("item, property, or relation")));
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
					if (!OwnerClass)
						return Reject(TEXT("graph_compound_material_property_invalid"), TEXT("The property owner does not resolve to a material expression."),
							Operation.SemanticId, TEXT("/depends_on/0"), MakeShared<FJsonValueString>(Operation.OwnerItemRef), MakeShared<FJsonValueString>(TEXT("existing or proposed expression")));
					if (!PropertyValue)
						return Reject(TEXT("graph_compound_material_property_invalid"), TEXT("The material property value is missing."),
							Operation.SemanticId, TEXT("/value/value"), nullptr, MakeShared<FJsonValueString>(TEXT("present value")));
					if (Operation.ValueType == TEXT("linear_color"))
					{
						FLinearColor Color;
						if (!TryCanonicalLinearColor(*PropertyValue, Color, &OutError, TEXT("/value/value"))) return Adopt(Operation.SemanticId);
					}
					else if (PropertyValue->GetKind() != EUeAgentValueKind::String || PropertyValue->GetText().IsEmpty())
						return Reject(TEXT("graph_compound_material_property_invalid"), TEXT("The material name property requires a nonempty string."),
							Operation.SemanticId, TEXT("/value/value"), CanonicalInputFact(PropertyValue), MakeShared<FJsonValueString>(TEXT("nonempty string")));
					if (!TryValidateExpressionProperty(*OwnerClass, Operation.PropertyPath, Operation.ValueType, OutError))
						return Adopt(Operation.SemanticId);
				}
				else if (Operation.Kind
					== EMaterialMutationOperationKind::Relation)
				{
					UClass* const FromClass = ResolveProposedExpressionClass(
						Target, Operation.FromItemRef, ProposedClasses);
					UMaterialExpression* const FromCdo = FromClass
						? Cast<UMaterialExpression>(FromClass->GetDefaultObject())
						: nullptr;
					const TCHAR* Code = TEXT("graph_compound_material_relation_endpoint_invalid");
					if (!FromCdo)
						return Reject(Code, TEXT("The connection source does not resolve to a material expression."), Operation.SemanticId,
							TEXT("/value/from_item_ref"), MakeShared<FJsonValueString>(Operation.FromItemRef), MakeShared<FJsonValueString>(TEXT("existing or proposed expression")));
					if (!FromCdo->GetOutputs().IsValidIndex(Operation.FromOutputIndex))
						return Reject(Code, TEXT("The connection source output index is outside the expression's outputs."), Operation.SemanticId,
							TEXT("/value/from_output_index"), MakeShared<FJsonValueNumber>(Operation.FromOutputIndex),
							MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= index < %d"), FromCdo->GetOutputs().Num())));
					if (Operation.ToItemRef == TEXT("material:root"))
					{
						if (!Cast<UMaterial>(&Target))
							return Reject(Code, TEXT("A material root connection requires a Material target."), Operation.SemanticId,
								TEXT("/value/to_item_ref"), MakeShared<FJsonValueString>(Target.GetClass()->GetPathName()),
								MakeShared<FJsonValueString>(UMaterial::StaticClass()->GetPathName()));
						EMaterialProperty Property = MP_MAX;
						if (!UeAgentMaterialAuthoring::ParseMaterialProperty(Operation.ToInputRef, Property))
							return Reject(Code, TEXT("The destination name does not identify a material root input."), Operation.SemanticId,
								TEXT("/value/to_input_ref"), MakeShared<FJsonValueString>(Operation.ToInputRef), MakeShared<FJsonValueString>(TEXT("valid material property name")));
					}
					else
					{
						UClass* const ToClass = ResolveProposedExpressionClass(Target, Operation.ToItemRef, ProposedClasses);
						UMaterialExpression* const ToCdo = ToClass ? Cast<UMaterialExpression>(ToClass->GetDefaultObject()) : nullptr;
						if (!ToCdo)
							return Reject(Code, TEXT("The connection destination does not resolve to a material expression."), Operation.SemanticId,
								TEXT("/value/to_item_ref"), MakeShared<FJsonValueString>(Operation.ToItemRef), MakeShared<FJsonValueString>(TEXT("existing or proposed expression")));
						FExpressionInput* Input = nullptr;
						if (!UeAgentMaterialAuthoring::ResolveExpressionInput(*ToCdo, Operation.ToInputRef, Input) || !Input)
							return Reject(Code, TEXT("The destination expression has no resolvable input with the requested reference."), Operation.SemanticId,
								TEXT("/value/to_input_ref"), MakeShared<FJsonValueString>(Operation.ToInputRef), MakeShared<FJsonValueString>(TEXT("valid expression input reference")));
					}
				}
				else if (Operation.Kind
					== EMaterialMutationOperationKind::Parameter)
				{
					const FUeAgentCanonicalValue* const Value = TryGetParameterEffectiveValue(*Operation.Value, OutError);
					if (!Value) return Adopt(Operation.SemanticId);
					const TCHAR* Code = TEXT("graph_compound_material_parameter_value_invalid");
					switch (Operation.ParameterType)
					{
					case EMaterialNiagaraGraphParameterType::Scalar:
					{
						double Number = 0.0;
						if (!TryCanonicalNumberValue(*Value, Number))
							return Reject(Code, TEXT("The scalar parameter value must be a finite number."), Operation.SemanticId,
								TEXT("/value"), CanonicalInputFact(Value), MakeShared<FJsonValueString>(TEXT("finite number")));
						break;
					}
					case EMaterialNiagaraGraphParameterType::Vector:
					{
						FLinearColor Color;
						if (!TryCanonicalLinearColor(*Value, Color, &OutError, TEXT("/value"))) return Adopt(Operation.SemanticId);
						break;
					}
					case EMaterialNiagaraGraphParameterType::Texture:
						if (Value->GetKind() != EUeAgentValueKind::String || !LoadObject<UTexture>(nullptr, *Value->GetText()))
							return Reject(Code, TEXT("The texture parameter value must name a loadable Texture asset."), Operation.SemanticId,
								TEXT("/value"), CanonicalInputFact(Value), MakeShared<FJsonValueString>(TEXT("loadable Texture asset path")));
						break;
					case EMaterialNiagaraGraphParameterType::StaticSwitch:
						if (Value->GetKind() != EUeAgentValueKind::Boolean)
							return Reject(Code, TEXT("The static switch parameter value must be a boolean."), Operation.SemanticId,
								TEXT("/value"), CanonicalInputFact(Value), MakeShared<FJsonValueString>(TEXT("boolean")));
						break;
					default:
						return Reject(Code, TEXT("The material parameter type is unsupported by this mutation binding."), Operation.SemanticId,
							TEXT("parameter_type"), MakeShared<FJsonValueNumber>(static_cast<int32>(Operation.ParameterType)),
							MakeShared<FJsonValueString>(TEXT("scalar, vector, texture, or static switch")), true);
					}
				}
			}
			if (!TryOrderMaterialMutationOperations(OutOperations, OutOrder, OutError)) return Adopt(OutError.SubjectId);
			return true;
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
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_material_operation_order_invalid"), TEXT("The ordered material operation index is outside the operation array."), TEXT("graph_compound"));
					Diagnostic.PropertyPath = TEXT("operation_index");
					Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(Index);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= index < %d"), Operations.Num()));
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = Target.GetPathName();
					SetError(OutError, MoveTemp(Diagnostic), FString());
					return false;
				}
				if (Cancellation->IsCancelled())
				{
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_material_mutation_cancelled"), TEXT("The graph operation was cancelled before the next native action."), TEXT("graph_compound"));
					Diagnostic.PropertyPath = TEXT("cancelled");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(true);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = Target.GetPathName();
					SetError(OutError, MoveTemp(Diagnostic), FString());
					return false;
				}
				const FMaterialMutationOperation& Operation = Operations[Index];
				FMaterialNiagaraGraphNativeMutationReceipt Receipt;
				FMaterialNiagaraGraphAdapterError Error;
				bool bApplied = false;
				auto RejectNative = [&](const FString& Reason, const FString& Field,
					TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
				{
					Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_material_mutation_failed"), Reason, TEXT("graph_compound"));
					Error.SourceKind = TEXT("native_result");
					Error.PropertyPath = Field;
					Error.ActualValue = MoveTemp(Actual);
					Error.ExpectedValue = MoveTemp(Expected);
				};
				switch (Operation.Kind)
				{
				case EMaterialMutationOperationKind::Item:
				{
					UMaterialExpression* Expression = nullptr;
					UeAgentPaletteSearch::FSearchParams CandidateParams;
					CandidateParams.CatalogType = TEXT("material_node");
					CandidateParams.AssetPath = Target.GetPathName();
					CandidateParams.GraphName = Operation.GraphName;
					CandidateParams.NodeGuid = Operation.NodeGuid;
					CandidateParams.PinGuid = Operation.PinGuid;
					CandidateParams.PinName = Operation.PinName;
					UeAgentPaletteSearch::FCandidate ResolvedCandidate;
					UE::UAI::Contract::FDiagnostic CandidateError;
					bApplied = UeAgentPaletteSearch::
						TryPerformMaterialActionCandidate(
							CandidateParams,
							Operation.CandidateName,
							Operation.CandidateSelect,
							Operation.CandidateId,
							FVector2f(
								Operation.PositionX,
								Operation.PositionY),
							Expression,
							ResolvedCandidate,
							CandidateError);
					if (!bApplied)
					{
						Error = MoveTemp(CandidateError);
                        if (Error.SemanticId.IsEmpty()) Error.SemanticId = Operation.SemanticId;
						break;
					}
					Expression->Modify();
					Expression->MaterialExpressionGuid =
						Operation.ExpressionGuid;
					Target.MarkPackageDirty();
					break;
				}
				case EMaterialMutationOperationKind::Property:
				{
					UMaterialExpression* const Expression = FindExpressionBySemanticId(Target, Operation.OwnerItemRef);
					const FUeAgentCanonicalValue* const Value = FindObjectValue(*Operation.Value, TEXT("value"));
					if (!Expression)
					{
						RejectNative(TEXT("The property owner expression is unavailable during mutation."), TEXT("owner_item_ref"),
							MakeShared<FJsonValueString>(Operation.OwnerItemRef), MakeShared<FJsonValueString>(TEXT("existing material expression")));
						break;
					}
					if (!Value)
					{
						RejectMaterialInput(Error, TEXT("graph_compound_material_property_invalid"), TEXT("The material property value is missing."),
							TEXT("/value/value"), nullptr, TEXT("present value"));
						break;
					}
					if (Operation.ValueType == TEXT("linear_color"))
					{
						FLinearColor Color;
						bApplied = TryCanonicalLinearColor(*Value, Color, &Error, TEXT("/value/value"))
							&& Hook.TrySetMaterialExpressionLinearColorProperty(*Expression, Operation.PropertyPath, Color, Receipt, Error);
					}
					else if (Operation.ValueType == TEXT("name"))
					{
						if (Value->GetKind() != EUeAgentValueKind::String)
						{
							RejectMaterialInput(Error, TEXT("graph_compound_material_property_invalid"), TEXT("The material name property value must be a string."),
								TEXT("/value/value"), Value, TEXT("string"));
							break;
						}
						bApplied = Hook.TrySetMaterialExpressionNameProperty(*Expression, Operation.PropertyPath, *Value->GetText(), Receipt, Error);
					}
					else
						RejectNative(TEXT("The prepared property value type is unsupported."), TEXT("value_type"),
							MakeShared<FJsonValueString>(Operation.ValueType), MakeShared<FJsonValueString>(TEXT("linear_color or name")));
					break;
				}
				case EMaterialMutationOperationKind::Relation:
				{
					UMaterialExpression* const From = FindExpressionBySemanticId(Target, Operation.FromItemRef);
					if (!From)
					{
						RejectNative(TEXT("The connection source expression is unavailable during mutation."), TEXT("from_item_ref"),
							MakeShared<FJsonValueString>(Operation.FromItemRef), MakeShared<FJsonValueString>(TEXT("existing material expression")));
						break;
					}
					if (Operation.ToItemRef == TEXT("material:root"))
					{
						EMaterialProperty Property = MP_MAX;
						if (!UeAgentMaterialAuthoring::ParseMaterialProperty(Operation.ToInputRef, Property))
						{
							RejectNative(TEXT("The prepared destination reference does not identify a material root input."), TEXT("to_input_ref"),
								MakeShared<FJsonValueString>(Operation.ToInputRef), MakeShared<FJsonValueString>(TEXT("valid material property name")));
							break;
						}
						bApplied = Hook.TryConnectMaterialExpressionToPropertyByIndex(*From, Operation.FromOutputIndex, Property, Receipt, Error);
					}
					else
					{
						UMaterialExpression* const To = FindExpressionBySemanticId(Target, Operation.ToItemRef);
						if (!To)
						{
							RejectNative(TEXT("The connection destination expression is unavailable during mutation."), TEXT("to_item_ref"),
								MakeShared<FJsonValueString>(Operation.ToItemRef), MakeShared<FJsonValueString>(TEXT("existing material expression")));
							break;
						}
						bApplied = Hook.TryConnectMaterialExpressionsByIndex(*From, Operation.FromOutputIndex, *To, Operation.ToInputRef, Receipt, Error);
					}
					break;
				}
				case EMaterialMutationOperationKind::Parameter:
				{
					UMaterialInstanceConstant* const Instance = Cast<UMaterialInstanceConstant>(&Target);
					const FUeAgentCanonicalValue* const Value = TryGetParameterEffectiveValue(*Operation.Value, Error);
					if (!Instance)
					{
						RejectNative(TEXT("Parameter mutation requires a MaterialInstanceConstant target."), TEXT("target_class"),
							MakeShared<FJsonValueString>(Target.GetClass()->GetPathName()), MakeShared<FJsonValueString>(UMaterialInstanceConstant::StaticClass()->GetPathName()));
						break;
					}
					if (!Value) break;
					switch (Operation.ParameterType)
					{
					case EMaterialNiagaraGraphParameterType::Scalar:
					{
						double Number = 0.0;
						if (!TryCanonicalNumberValue(*Value, Number))
						{
							RejectMaterialInput(Error, TEXT("graph_compound_material_parameter_value_invalid"), TEXT("The scalar parameter value must be a finite number."),
								TEXT("/value"), Value, TEXT("finite number"));
							break;
						}
						bApplied = Hook.TrySetMaterialInstanceScalarParameter(*Instance, Operation.ParameterInfo, static_cast<float>(Number), Receipt, Error);
						break;
					}
					case EMaterialNiagaraGraphParameterType::Vector:
					{
						FLinearColor Color;
						bApplied = TryCanonicalLinearColor(*Value, Color, &Error, TEXT("/value"))
							&& Hook.TrySetMaterialInstanceVectorParameter(*Instance, Operation.ParameterInfo, Color, Receipt, Error);
						break;
					}
					case EMaterialNiagaraGraphParameterType::Texture:
					{
						UTexture* const Texture = Value->GetKind() == EUeAgentValueKind::String ? LoadObject<UTexture>(nullptr, *Value->GetText()) : nullptr;
						if (!Texture)
						{
							RejectMaterialInput(Error, TEXT("graph_compound_material_parameter_value_invalid"), TEXT("The texture parameter asset could not be loaded."),
								TEXT("/value"), Value, TEXT("loadable Texture asset path"));
							break;
						}
						bApplied = Hook.TrySetMaterialInstanceTextureParameter(*Instance, Operation.ParameterInfo, *Texture, Receipt, Error);
						break;
					}
					case EMaterialNiagaraGraphParameterType::StaticSwitch:
						if (Value->GetKind() != EUeAgentValueKind::Boolean)
						{
							RejectMaterialInput(Error, TEXT("graph_compound_material_parameter_value_invalid"), TEXT("The static switch parameter value must be a boolean."),
								TEXT("/value"), Value, TEXT("boolean"));
							break;
						}
						bApplied = Hook.TrySetMaterialInstanceStaticSwitchParameter(*Instance, Operation.ParameterInfo, Value->GetBoolean(), Receipt, Error);
						break;
					default:
						RejectNative(TEXT("The prepared material parameter type is unsupported."), TEXT("parameter_type"),
							MakeShared<FJsonValueNumber>(static_cast<int32>(Operation.ParameterType)), MakeShared<FJsonValueString>(TEXT("scalar, vector, texture, or static switch")));
						break;
					}
					break;
				}
				default:
					RejectNative(TEXT("The prepared material operation kind is unsupported."), TEXT("operation_kind"),
						MakeShared<FJsonValueNumber>(static_cast<int32>(Operation.Kind)), MakeShared<FJsonValueString>(TEXT("item, property, relation, or parameter")));
					break;
				}
				if (!bApplied)
				{
					SetError(OutError, MoveTemp(Error), Operation.SemanticId);
					return false;
				}
			}
			FMaterialNiagaraGraphCompileSettleReceipt Settle;
			FMaterialNiagaraGraphAdapterError SettleError;
			if (!Hook.TryCompileAndSettleMaterial(Target, Settle, SettleError))
			{
				SetError(OutError, MoveTemp(SettleError), Target.GetPathName());
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
			UE::UAI::Contract::FDiagnostic& OutDetail)
		{
			return UeAgentAuthoringOperationApply::TryRestorePackageDurability(
				TargetRef, false, Session, Backend, FencingToken,
				Resources, OutDetail);
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
					OutError = FVerificationError::Failure(TEXT("graph_compound_material_readback_missing"),
						TEXT("Independent readback did not contain a value for this material operation."), TEXT("verification"));
					OutError.SourceKind = TEXT("native_result");
					OutError.SubjectId = Operation.SemanticId;
					OutError.PropertyPath = TEXT("readback_value_present");
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return false;
				}
				Items.Emplace(Operation.SemanticId,
					Actual ? *Actual : Operation.Value.ToSharedRef());
			}
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryObject(
					Items, Resources, OutValue, Error))
			{
				OutError = Error;
				return false;
			}
			return OutValue.IsValid();
		}

		bool TryVerifyReloadedGraphMutation(
			UObject& Target,
			const TArray<FMaterialMutationOperation>& Operations,
			TMap<FString, FUeAgentCanonicalValueRef>& OutActualValues,
			FUeAgentResourceTracker& Resources,
			UE::UAI::Contract::FDiagnostic& OutDiagnostic)
		{
			OutActualValues.Reset();
			OutDiagnostic.Reset();
			auto Mismatch = [&](const TCHAR* Code, const FString& Reason, const FMaterialMutationOperation& Operation,
				const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
			{
				OutDiagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_readback"));
				OutDiagnostic.SourceKind = TEXT("native_result");
				OutDiagnostic.TargetPath = Target.GetPathName();
				OutDiagnostic.SubjectId = Operation.SemanticId;
				OutDiagnostic.PropertyPath = Field;
				OutDiagnostic.ActualValuePresent = Actual.IsValid();
				OutDiagnostic.ExpectedValuePresent = Expected.IsValid();
				OutDiagnostic.ActualValue = MoveTemp(Actual);
				OutDiagnostic.ExpectedValue = MoveTemp(Expected);
				return false;
			};
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
						const TCHAR* Code = TEXT("graph_compound_material_item_readback_mismatch");
						if (!Expression)
							return Mismatch(Code, TEXT("The created expression is absent from persisted readback."), Operation, TEXT("expression_present"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						if (Expression->GetClass()->GetPathName() != Operation.ClassRef)
							return Mismatch(Code, TEXT("The persisted expression class differs from the requested class."), Operation, TEXT("class_ref"),
								MakeShared<FJsonValueString>(Expression->GetClass()->GetPathName()), MakeShared<FJsonValueString>(Operation.ClassRef));
						const bool bXDiffers = Expression->MaterialExpressionEditorX != Operation.PositionX;
						return Mismatch(Code, TEXT("The persisted expression position differs from the requested position."), Operation,
							bXDiffers ? TEXT("position/x") : TEXT("position/y"),
							MakeShared<FJsonValueNumber>(bXDiffers ? Expression->MaterialExpressionEditorX : Expression->MaterialExpressionEditorY),
							MakeShared<FJsonValueNumber>(bXDiffers ? Operation.PositionX : Operation.PositionY));
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
					const TCHAR* Code = TEXT("graph_compound_material_property_readback_mismatch");
					if (!Expression || !Property || !Expected)
						return Mismatch(Code, !Expression ? TEXT("The property owner expression is absent from persisted readback.")
							: !Property ? TEXT("The property is absent from the persisted expression class.") : TEXT("The prepared operation has no expected property value."),
							Operation, !Expression ? TEXT("owner_expression_present") : !Property ? Operation.PropertyPath : TEXT("expected_value_present"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					if (Operation.ValueType == TEXT("linear_color"))
					{
						const FStructProperty* const StructProperty = CastField<FStructProperty>(Property);
						if (!StructProperty || StructProperty->Struct != TBaseStructure<FLinearColor>::Get())
							return Mismatch(Code, TEXT("The persisted property is not a linear color."), Operation, Operation.PropertyPath,
								MakeShared<FJsonValueString>(Property->GetCPPType()), MakeShared<FJsonValueString>(TEXT("FLinearColor")));
						FLinearColor ExpectedColor;
						if (!TryCanonicalLinearColor(*Expected, ExpectedColor, &OutDiagnostic, TEXT("/value/value")))
						{
							OutDiagnostic.SourceKind = TEXT("native_result");
							OutDiagnostic.Source.JsonPath.Reset();
							OutDiagnostic.SemanticPointer.Reset();
							OutDiagnostic.SubjectId = Operation.SemanticId;
							OutDiagnostic.TargetPath = Target.GetPathName();
							return false;
						}
						const FLinearColor* const Actual = StructProperty->ContainerPtrToValuePtr<FLinearColor>(Expression);
						if (!Actual)
							return Mismatch(Code, TEXT("The persisted linear color property has no readable value."), Operation, Operation.PropertyPath,
								nullptr, MakeShared<FJsonValueString>(TEXT("readable linear color")));
						if (!Actual->Equals(ExpectedColor))
						{
							auto ColorFact = [](const FLinearColor& Color)
							{
								auto Component = [](const float Number) -> TSharedPtr<FJsonValue>
								{
									return FMath::IsFinite(Number) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Number))
										: MakeShared<FJsonValueString>(::LexToString(Number));
								};
								TArray<TSharedPtr<FJsonValue>> Values = {Component(Color.R), Component(Color.G), Component(Color.B), Component(Color.A)};
								return MakeShared<FJsonValueArray>(Values);
							};
							return Mismatch(Code, TEXT("The persisted linear color differs from the requested value."), Operation, Operation.PropertyPath,
								ColorFact(*Actual), ColorFact(ExpectedColor));
						}
					}
					else if (Operation.ValueType == TEXT("name"))
					{
						const FNameProperty* const NameProperty = CastField<FNameProperty>(Property);
						if (!NameProperty)
							return Mismatch(Code, TEXT("The persisted property is not a name."), Operation, Operation.PropertyPath,
								MakeShared<FJsonValueString>(Property->GetCPPType()), MakeShared<FJsonValueString>(TEXT("FName")));
						if (Expected->GetKind() != EUeAgentValueKind::String)
							return Mismatch(Code, TEXT("The prepared name property value is not a string."), Operation, TEXT("expected/") + Operation.PropertyPath,
								CanonicalInputFact(Expected), MakeShared<FJsonValueString>(TEXT("string")));
						const FString Actual = NameProperty->GetPropertyValue_InContainer(Expression).ToString();
						if (Actual != Expected->GetText())
							return Mismatch(Code, TEXT("The persisted name differs from the requested value."), Operation, Operation.PropertyPath,
								MakeShared<FJsonValueString>(Actual), MakeShared<FJsonValueString>(Expected->GetText()));
					}
					else
						return Mismatch(Code, TEXT("The prepared property value type is unsupported for readback."), Operation, TEXT("value_type"),
							MakeShared<FJsonValueString>(Operation.ValueType), MakeShared<FJsonValueString>(TEXT("linear_color or name")));
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
						const TCHAR* Code = TEXT("graph_compound_material_relation_readback_mismatch");
						if (!From)
							return Mismatch(Code, TEXT("The connection source expression is absent from persisted readback."), Operation, TEXT("from_item_ref"),
								nullptr, MakeShared<FJsonValueString>(Operation.FromItemRef));
						if (!Input)
							return Mismatch(Code, TEXT("The persisted destination input cannot be resolved."), Operation, TEXT("to_input_ref"),
								nullptr, MakeShared<FJsonValueString>(Operation.ToItemRef + TEXT(":") + Operation.ToInputRef));
						if (Input->Expression != From)
							return Mismatch(Code, TEXT("The persisted connection points to a different source expression."), Operation, TEXT("from_item_ref"),
								MakeShared<FJsonValueString>(Input->Expression ? TEXT("expression:") + Input->Expression->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower) : FString()),
								MakeShared<FJsonValueString>(Operation.FromItemRef));
						return Mismatch(Code, TEXT("The persisted connection uses a different source output index."), Operation, TEXT("from_output_index"),
							MakeShared<FJsonValueNumber>(Input->OutputIndex), MakeShared<FJsonValueNumber>(Operation.FromOutputIndex));
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
				UE::UAI::Contract::FDiagnostic Detail;
				if (!UeAgentAuthoringOperationApply::TryReloadPersistedPackage(TargetRef, Detail))
				{
					OutError = MoveTemp(Detail);
					return false;
				}
				FGraphCompoundRuntimeError ResolveError;
				UObject* const Target =
					TryResolveMaterialMutationTarget(Profile, TargetRef, ResolveError);
				if (!Target)
				{
					OutError = ResolveError;
					return false;
				}
				if (Target->GetOutermost()->IsDirty())
				{
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_material_reload_failed"),
						TEXT("The reloaded material package is dirty, so it cannot provide independent persisted-state evidence."), TEXT("independent_readback"));
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = TargetRef;
					Diagnostic.PropertyPath = TEXT("package_dirty");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(true);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
					OutObservation = FIndependentReadbackObservation::Unavailable(MoveTemp(Diagnostic));
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
						if (!Instance)
						{
							NativeError = UE::UAI::Contract::FDiagnostic::Failure(
								TEXT("graph_compound_material_parameter_readback_failed"),
								TEXT("Effective material parameters require a material instance constant target."), TEXT("independent_readback"));
							NativeError.SourceKind = TEXT("native_result");
							NativeError.PropertyPath = TEXT("/target/type");
							NativeError.ActualValuePresent = true;
							NativeError.ExpectedValuePresent = true;
							NativeError.ActualValue = MakeShared<FJsonValueString>(Target->GetClass()->GetPathName());
							NativeError.ExpectedValue = MakeShared<FJsonValueString>(UMaterialInstanceConstant::StaticClass()->GetPathName());
						}
						if (NativeError.TargetPath.IsEmpty()) NativeError.TargetPath = TargetRef;
						OutObservation = FIndependentReadbackObservation::Unavailable(MoveTemp(NativeError));
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
						if (!Found || !*Found || !(*Found)->IsOverridden())
						{
							auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_material_parameter_readback_mismatch"),
								!Found || !*Found ? TEXT("The parameter is absent from independent native readback.")
									: TEXT("The parameter exists but its native value is not overridden."), TEXT("independent_readback"));
							Diagnostic.SourceKind = TEXT("native_result");
							Diagnostic.TargetPath = TargetRef;
							Diagnostic.PropertyPath = !Found || !*Found ? TEXT("parameter_present") : TEXT("overridden");
							Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
							Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
							OutObservation = FIndependentReadbackObservation::Unavailable(MoveTemp(Diagnostic), Operation.SemanticId);
							return true;
						}
						if (!TryCanonicalEffectiveParameterState(**Found, Resources, Actual, CanonicalError))
						{
							if (CanonicalError.TargetPath.IsEmpty()) CanonicalError.TargetPath = TargetRef;
							OutObservation = FIndependentReadbackObservation::Unavailable(MoveTemp(CanonicalError), Operation.SemanticId);
							return true;
						}
						ActualValues.Add(Operation.SemanticId, Actual.ToSharedRef());
					}
				}
				else if (!TryVerifyReloadedGraphMutation(
					*Target, Operations, ActualValues, Resources, Detail))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(Detail, TargetRef);
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
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_blueprint_event_owner_invalid"),
						!Blueprint ? TEXT("The event target could not be loaded as a Blueprint.")
							: !Blueprint->SimpleConstructionScript ? TEXT("The Blueprint has no simple construction script for component event discovery.")
							: TEXT("The component event owner reference must start with component:."), TEXT("graph_compound"));
					Diagnostic.TargetPath = Request.GetTargetRef();
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.PropertyPath = !Blueprint ? TEXT("blueprint_loaded") : !Blueprint->SimpleConstructionScript ? TEXT("construction_script_present") : TEXT("owner_item_ref");
					Diagnostic.ActualValue = !Blueprint || !Blueprint->SimpleConstructionScript
						? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)) : MakeShared<FJsonValueString>(Request.GetOwnerItemRef());
					Diagnostic.ExpectedValue = !Blueprint || !Blueprint->SimpleConstructionScript
						? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)) : MakeShared<FJsonValueString>(TEXT("component:<name>"));
					SetError(OutError, MoveTemp(Diagnostic), Request.GetOwnerItemRef());
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
					SetError(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_umg_event_owner_invalid"), TEXT("target_ref must resolve to a Widget Blueprint and owner_item_ref must use widget:<name>."), TEXT("graph_compound"), FString()), Request.GetOwnerItemRef());
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
				SetError(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_event_owner_not_found"), TEXT("owner_item_ref does not identify a widget in the target Widget Blueprint."), TEXT("graph_compound"), FString()), Request.GetOwnerItemRef());
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
			UE::UAI::Contract::FDiagnostic Error;
			if (!Json::TryCanonicalize(Root, Canonical, Error))
			{
				SetError(OutError, Error, FString());
				return false;
			}
			OutResult = FUeAgentContractValue(MoveTemp(Canonical));
			return true;
		}

		bool TrySerializeUmgWidgetEvents(
			const TArray<FBlueprintUmgGraphEventIdentity>& Events,
			const FString& Query,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			TArray<ContentDiscovery::FScopedNameDiscoveryEntry> Entries;
			Entries.Reserve(Events.Num());
			for (const FBlueprintUmgGraphEventIdentity& Event : Events)
			{
				ContentDiscovery::FScopedNameDiscoveryEntry Entry;
				Entry.Name = Event.GetNativeFieldName();
				Entry.Identity = Event.GetEventIdentity();
				Entry.Scopes.Add(TEXT("event"));
				Entries.Add(MoveTemp(Entry));
			}
			TArray<int32> MatchedIndices;
			for (int32 Index = 0; Index < Events.Num(); ++Index)
			{
				ContentDiscovery::FContentDiscoveryRequest Request;
				Request.Query = Query;
				Request.MatchMode =
					ContentDiscovery::EContentDiscoveryMatchMode::Text;
				int32 Score = 0;
				FString MatchReason;
				if (ContentDiscovery::TryScoreContentDiscoveryMatch(
					Request,
					Events[Index].GetNativeFieldName(),
					Events[Index].GetNativeFieldName(),
					{Events[Index].GetSignatureIdentity()},
					Score,
					MatchReason))
				{
					MatchedIndices.Add(Index);
				}
			}
			UE::UAI::Contract::FDiagnostic ScopeError;
			FString Payload;
			if (!ContentDiscovery::TryBuildScopedNamesOnlyPayloadForIndices(
				Entries,
				MatchedIndices,
				ContentDiscovery::ENameComparison::IgnoreCase,
				Payload,
				ScopeError))
			{
				SetError(OutError, MoveTemp(ScopeError), FString());
				return false;
			}
			OutResult = FUeAgentContractValue(MoveTemp(Payload));
			return true;
		}

		bool TrySerializeBlueprintComponentEvents(
			const DomainAdapters::BlueprintCommands::
				FBlueprintComponentEventDiscoveryResult& Result,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			TArray<ContentDiscovery::FScopedNameDiscoveryEntry> Entries;
			TArray<int32> MatchedIndices;
			Entries.Reserve(Result.Events.Num());
			MatchedIndices.Reserve(Result.Events.Num());
			for (int32 Index = 0; Index < Result.Events.Num(); ++Index)
			{
				const DomainAdapters::BlueprintCommands::
					FBlueprintComponentEventRecord& Event = Result.Events[Index];
				ContentDiscovery::FScopedNameDiscoveryEntry Entry;
				Entry.Identity = Event.EventRef;
				Entry.Name = Event.DisplayName.IsEmpty()
					? Event.NativeFieldName : Event.DisplayName;
				if (!Result.ComponentName.TrimStartAndEnd().IsEmpty())
				{
					Entry.Scopes.Add(Result.ComponentName);
				}
				Entries.Add(MoveTemp(Entry));
				MatchedIndices.Add(Index);
			}
			FString Payload;
			UE::UAI::Contract::FDiagnostic Error;
			if (!ContentDiscovery::TryBuildScopedNamesOnlyPayloadForIndices(
				Entries, MatchedIndices,
				ContentDiscovery::ENameComparison::IgnoreCase,
				Payload, Error))
			{
				SetError(OutError, Error, Result.ComponentName);
				return false;
			}
			OutResult = FUeAgentContractValue(MoveTemp(Payload));
			return true;
		}

		bool TrySerializeCreateResult(
			const FString& TargetRef,
			const UeAgentAuthoringWorkspaceTransaction::FFinalReadbackEvidence& Evidence,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("target_ref"), TargetRef);
			Root->SetObjectField(TEXT("operation_stages"), Evidence.ToJsonObject());
			FString Canonical;
			UE::UAI::Contract::FDiagnostic Error;
			if (!Json::TryCanonicalize(Root, Canonical, Error))
			{
				SetError(OutError, Error, TargetRef);
				return false;
			}
			OutResult = FUeAgentContractValue(MoveTemp(Canonical));
			return true;
		}

		bool TrySerializeMaterialMutationResult(
			const FString& TargetRef,
			const int32 ChangeCount,
			const FString& /*EvidenceId*/,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			(void)ChangeCount;
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("target_ref"), TargetRef);
			FString Canonical;
			UE::UAI::Contract::FDiagnostic Error;
			if (!Json::TryCanonicalize(Root, Canonical, Error))
			{
				SetError(OutError, Error, TargetRef);
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
				, DurableBackend(FString(), Config.AdditionalDurableContentRoots)
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
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_query_thread_invalid"), TEXT("Graph queries require the game thread."), TEXT("graph_compound"));
					Diagnostic.PropertyPath = TEXT("game_thread");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = Request.GetTargetRef();
					SetError(OutError, MoveTemp(Diagnostic), Profile.GetCapabilityId());
					return false;
				}
				if (Cancellation->IsCancelled())
				{
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_query_cancelled"), TEXT("The graph operation was cancelled before the next native action."), TEXT("graph_compound"));
					Diagnostic.PropertyPath = TEXT("cancelled");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(true);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = Request.GetTargetRef();
					SetError(OutError, MoveTemp(Diagnostic), Profile.GetCapabilityId());
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
						auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_blueprint_event_target_invalid"),
							TEXT("asset_path could not be loaded as a Blueprint for component event discovery."), TEXT("target_resolve"));
						Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(UBlueprint::StaticClass()->GetPathName());
						Diagnostic.ActualValue = MakeShared<FJsonValueString>(Request.GetTargetRef());
						Diagnostic.TargetPath = Request.GetTargetRef();
						Diagnostic.PropertyPath = TEXT("/asset_path");
						Diagnostic.Source.JsonPath = TEXT("/asset_path");
						Diagnostic.SourceKind = TEXT("command_input");
						SetError(OutError, MoveTemp(Diagnostic), Request.GetTargetRef());
						return false;
					}
					DomainAdapters::BlueprintCommands::
						FBlueprintComponentEventDiscoveryRequest NativeRequest;
					NativeRequest.ComponentName =
						Request.GetOwnerItemRef().RightChop(10);
					NativeRequest.ComponentNameSource.JsonPath = TEXT("/component_name");
					NativeRequest.ComponentNameSourceKind = TEXT("command_input");
					NativeRequest.Query = Request.GetQuery();
					NativeRequest.bOnlyUnbound = Request.IsOnlyUnbound();
					NativeRequest.bIncludeHidden = Request.IsIncludeHidden();
					NativeRequest.bCompileBeforeQuery =
						Request.IsCompileBeforeQuery();
					DomainAdapters::BlueprintCommands::
						FBlueprintComponentEventDiscoveryResult NativeResult;
					UE::UAI::Contract::FDiagnostic NativeError;
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
					OutError = Error;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetOwnerItemRef();
					return false;
				}
				if (Profile.GetCapabilityId()
					== TEXT("umg.widget.events.discover"))
				{
					return TrySerializeUmgWidgetEvents(
						Events,
						Request.GetQuery(),
						OutResult,
						OutError);
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
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_typed_snapshot_thread_invalid"), TEXT("Graph snapshot capture requires the game thread."), TEXT("graph_compound"));
					Diagnostic.PropertyPath = TEXT("game_thread");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = TargetRef;
					SetError(OutError, MoveTemp(Diagnostic), Profile.GetCapabilityId());
					return false;
				}
				if (Cancellation->IsCancelled())
				{
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_typed_snapshot_cancelled"), TEXT("The graph operation was cancelled before the next native action."), TEXT("graph_compound"));
					Diagnostic.PropertyPath = TEXT("cancelled");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(true);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = TargetRef;
					SetError(OutError, MoveTemp(Diagnostic), TargetRef);
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
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_material_snapshot_binding_unavailable"), TEXT("The graph profile has no material snapshot binding for this input kind."), TEXT("graph_compound"));
					Diagnostic.PropertyPath = TEXT("typed_input_kind");
					Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Profile.GetTypedInputKind()));
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("GraphEdits or MaterialInstanceParameters"));
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = TargetRef;
					SetError(OutError, MoveTemp(Diagnostic), Profile.GetCapabilityId());
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
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_material_mutation_thread_invalid"), TEXT("Material graph mutation requires the game thread."), TEXT("graph_compound"));
					Diagnostic.PropertyPath = TEXT("game_thread");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = TargetRef;
					SetError(OutError, MoveTemp(Diagnostic), Profile.GetCapabilityId());
					return false;
				}
				if (Cancellation->IsCancelled())
				{
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_material_mutation_cancelled"), TEXT("The graph operation was cancelled before the next native action."), TEXT("graph_compound"));
					Diagnostic.PropertyPath = TEXT("cancelled");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(true);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = TargetRef;
					SetError(OutError, MoveTemp(Diagnostic), TargetRef);
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
					OutError = RevalidateError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = RevalidateError.SubjectId;
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
						*Resources, Prepared, DurableError))
				{
					OutError = DurableError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = TargetRef;
					return false;
				}

				if (!TryApplyMaterialMutationOperations(
						*Target, Operations, Order, MaterialNativeHook,
						Cancellation, OutError))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
					if (const FString* InputPointer = Payload.FindInputPointer(OutError.SubjectId))
						BindGraphCompoundInputValueDiagnostic(OutError, *InputPointer);
					Target = nullptr;
					UE::UAI::Contract::FDiagnostic RollbackDetail;
					TryRollbackMaterialMutation(
						TargetRef, Prepared.ToSharedRef(), DurableBackend,
						FencingToken, *Resources, RollbackDetail);
					OutError.Append(RollbackDetail);
					return false;
				}
				Target->GetOutermost()->MarkPackageDirty();
				FDurableReceipt DurableReceipt;
				if (!TryPersistAssetDurabilityForVerification(
						Prepared.ToSharedRef(), DurableBackend, FencingToken,
						*Resources, DurableReceipt, DurableError))
				{
					Target = nullptr;
					UE::UAI::Contract::FDiagnostic RollbackDetail;
					TryRollbackMaterialMutation(
						TargetRef, Prepared.ToSharedRef(), DurableBackend,
						FencingToken, *Resources, RollbackDetail);
					SetError(OutError, DurableError, TargetRef, RollbackDetail);
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
					UE::UAI::Contract::FDiagnostic RollbackDetail;
					TryRollbackMaterialMutation(
						TargetRef, Persisted, DurableBackend, FencingToken,
						*Resources, RollbackDetail);
					SetError(OutError, VerifyError, TargetRef, RollbackDetail);
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
					|| !Readback->TryObserve(
						FIndependentReadbackRequest(
							TEXT("graph-compound:") + Plan->GetRequestId(),
							MaterialMutationReadbackOracleId,
							TargetRef, Expected.ToSharedRef()),
						*Resources, Outcome, VerifyError))
				{
					UE::UAI::Contract::FDiagnostic RollbackDetail;
					TryRollbackMaterialMutation(
						TargetRef, Persisted, DurableBackend, FencingToken,
						*Resources, RollbackDetail);
					SetError(OutError, MoveTemp(VerifyError), TargetRef, RollbackDetail);
					return false;
				}

				const FDurableVerificationAcknowledgement Acknowledgement(
					DurableReceipt.GetJournalId(), Plan->GetFingerprint(),
					Prepared->GetInventory().GetInventoryHash(),
					Outcome.GetEvidenceId(), JournalOwner, FencingToken, Outcome.GetStatus() == EVerificationStatus::Verified);
				if (!TryAcknowledgeAssetDurability(
						Persisted, Acknowledgement, FencingToken,
						*Resources, DurableError))
				{
					UE::UAI::Contract::FDiagnostic RollbackDetail;
					TryRollbackMaterialMutation(
						TargetRef, Persisted, DurableBackend, FencingToken,
						*Resources, RollbackDetail);
					SetError(OutError, DurableError, TargetRef, RollbackDetail);
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
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_asset_create_binding_unavailable"),
						!IsInGameThread() ? TEXT("Asset creation snapshots must be captured on the game thread.")
							: TEXT("This asset creation binding does not support the requested input kind."), TEXT("graph_compound"));
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = Request.GetTargetRef();
					Diagnostic.PropertyPath = !IsInGameThread() ? TEXT("game_thread") : TEXT("input_kind");
					Diagnostic.ActualValue = !IsInGameThread()
						? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false))
						: MakeShared<FJsonValueNumber>(static_cast<int32>(Request.GetInputKind()));
					Diagnostic.ExpectedValue = !IsInGameThread()
						? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true))
						: MakeShared<FJsonValueString>(TEXT("Blueprint, UMG, Material, MaterialInstance, or Niagara asset creation"));
					SetError(OutError, MoveTemp(Diagnostic), Profile.GetCapabilityId());
					return false;
				}
				if (Cancellation->IsCancelled())
				{
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_asset_create_cancelled"), TEXT("The graph operation was cancelled before the next native action."), TEXT("graph_compound"));
					Diagnostic.PropertyPath = TEXT("cancelled");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(true);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = Request.GetTargetRef();
					SetError(OutError, MoveTemp(Diagnostic), Profile.GetCapabilityId());
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
					const bool bCancelled = Cancellation->IsCancelled();
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(bCancelled
						? TEXT("graph_compound_asset_create_cancelled") : TEXT("graph_compound_asset_create_thread_invalid"),
						bCancelled ? TEXT("Asset creation was cancelled before mutation.")
							: TEXT("Native asset creation must run on the game thread."), TEXT("graph_compound"));
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = Request.GetTargetRef();
					Diagnostic.PropertyPath = bCancelled ? TEXT("cancelled") : TEXT("game_thread");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(bCancelled);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(!bCancelled);
					SetError(OutError, MoveTemp(Diagnostic), Profile.GetCapabilityId());
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
					const auto& Changes = Plan->GetChangeSet().GetChanges();
					const bool bDescriptorMatches = Plan->GetDescriptorIdentity() == Profile.GetExecutionContract().GetDescriptorIdentity();
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_asset_create_plan_invalid"),
						!bDescriptorMatches ? TEXT("The asset creation plan descriptor differs from the execution profile.")
							: Changes.Num() != 1 ? TEXT("Asset creation requires exactly one planned change.")
							: Changes[0].GetIntent() != EChangeIntent::Create ? TEXT("The planned asset change must use create intent.")
							: TEXT("The planned change owner differs from the asset creation target."), TEXT("graph_compound"));
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = Request.GetTargetRef();
					if (!bDescriptorMatches || (Changes.Num() == 1 && Changes[0].GetIntent() == EChangeIntent::Create))
					{
						Diagnostic.PropertyPath = !bDescriptorMatches ? TEXT("descriptor_matches") : TEXT("owner_matches_target");
						Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
						Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					}
					else
					{
						Diagnostic.PropertyPath = Changes.Num() != 1 ? TEXT("change_count") : TEXT("intent");
						Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(Changes.Num() != 1 ? Changes.Num() : static_cast<int32>(Changes[0].GetIntent()));
						Diagnostic.ExpectedValue = Changes.Num() != 1 ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(1))
							: MakeShared<FJsonValueString>(TEXT("create"));
					}
					SetError(OutError, MoveTemp(Diagnostic), Request.GetTargetRef());
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
						auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_blueprint_parent_invalid"),
							!ParentClass ? TEXT("The Blueprint parent class could not be loaded.") : TEXT("The loaded parent class does not support Blueprint creation."), TEXT("graph_compound"));
						Diagnostic.SourceKind = TEXT("command_input");
						Diagnostic.Source.JsonPath = TEXT("/parent_class_ref");
						Diagnostic.SemanticPointer = Diagnostic.Source.JsonPath;
						Diagnostic.PropertyPath = TEXT("parent_class_ref");
						Diagnostic.TargetPath = Request.GetTargetRef();
						Diagnostic.ActualValue = MakeShared<FJsonValueString>(Request.GetParentClassRef());
						Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("loadable Blueprint-capable class"));
						SetError(OutError, MoveTemp(Diagnostic), Request.GetParentClassRef());
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
						auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_material_parent_invalid"),
							TEXT("The material instance parent could not be loaded as a MaterialInterface."), TEXT("graph_compound"));
						Diagnostic.SourceKind = TEXT("command_input");
						Diagnostic.Source.JsonPath = TEXT("/parent_material_ref");
						Diagnostic.SemanticPointer = Diagnostic.Source.JsonPath;
						Diagnostic.PropertyPath = TEXT("parent_material_ref");
						Diagnostic.TargetPath = Request.GetTargetRef();
						Diagnostic.ActualValue = MakeShared<FJsonValueString>(Request.GetParentMaterialRef());
						Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("loadable MaterialInterface asset"));
						SetError(OutError, MoveTemp(Diagnostic), Request.GetParentMaterialRef());
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
					OutError = RevalidateError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = RevalidateError.SubjectId;
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
						*Resources, Prepared, DurableError))
				{
					OutError = DurableError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
					return false;
				}

				UPackage* Package = CreatePackage(*Request.GetTargetRef());
				UObject* CreatedAsset = nullptr;
				
				UE::UAI::Contract::FDiagnostic CreationError;
				auto RejectCreation = [&](const FString& Code, const FString& Reason, const FString& Field,
					TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
				{
					CreationError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("asset_create"));
					CreationError.SourceKind = TEXT("native_result");
					CreationError.TargetPath = Request.GetTargetRef();
					CreationError.PropertyPath = Field;
					CreationError.ActualValue = MoveTemp(Actual);
					CreationError.ExpectedValue = MoveTemp(Expected);
				};
				bool bCreated = false;
				if (!Package)
					RejectCreation(TEXT("graph_compound_asset_create_package_failed"), TEXT("Unreal could not create the target package."),
						TEXT("package_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
						CreationError = NativeError;
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
						CreationError = NativeError;
						break;
					}
					case EGraphCompoundTypedInputKind::MaterialAssetCreate:
					{
						CreatedAsset = UeAgentMaterialAuthoring::CreateFactorySeed(
							Request.GetMaterialKind(),
							Package,
							*Request.GetAssetName(),
							RF_Public | RF_Standalone | RF_Transactional,
							CreationError);
						const UClass* const ExpectedClass =
							Request.GetMaterialKind() == TEXT("material_function")
								? UMaterialFunction::StaticClass()
								: UMaterial::StaticClass();
						bCreated = CreatedAsset
							&& CreatedAsset->GetClass() == ExpectedClass;
						if (CreatedAsset && !bCreated)
							RejectCreation(Request.GetMaterialKind() == TEXT("material_function")
								? TEXT("graph_compound_material_function_factory_failed") : TEXT("graph_compound_material_factory_failed"),
								TEXT("The material factory returned an unexpected asset class."), TEXT("asset_class"),
								MakeShared<FJsonValueString>(CreatedAsset->GetClass()->GetPathName()), MakeShared<FJsonValueString>(ExpectedClass->GetPathName()));
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
							const TCHAR* Code = TEXT("graph_compound_material_instance_factory_failed");
							if (!Factory || !CreatedAsset)
								RejectCreation(Code, !Factory ? TEXT("Unreal could not create the material instance factory.") : TEXT("The material instance factory returned no asset."),
									!Factory ? TEXT("factory_created") : TEXT("asset_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
							else if (!MaterialInstance || CreatedAsset->GetClass() != UMaterialInstanceConstant::StaticClass())
								RejectCreation(Code, TEXT("The material instance factory returned an unexpected asset class."), TEXT("asset_class"),
									MakeShared<FJsonValueString>(CreatedAsset->GetClass()->GetPathName()), MakeShared<FJsonValueString>(UMaterialInstanceConstant::StaticClass()->GetPathName()));
							else
								RejectCreation(Code, TEXT("The created material instance has a different parent from the requested parent."), TEXT("parent_material_ref"),
									MakeShared<FJsonValueString>(MaterialInstance->Parent ? MaterialInstance->Parent->GetPathName() : FString()),
									MakeShared<FJsonValueString>(ParentMaterial->GetPathName()));
						}
						break;
					}
					default:
						RejectCreation(TEXT("graph_compound_asset_create_binding_unavailable"), TEXT("This binding cannot create the requested asset kind."), TEXT("input_kind"),
							MakeShared<FJsonValueNumber>(static_cast<int32>(Request.GetInputKind())),
							MakeShared<FJsonValueString>(TEXT("Blueprint, UMG, Material, or MaterialInstance asset creation")));
						break;
					}
				}

				UeAgentAuthoringWorkspaceTransaction::FFinalReadbackEvidence OperationEvidence;
OperationEvidence.bMutationPhaseCompleted = bCreated;
				OperationEvidence.bNativeUpdateRequested = true;
				bool bSettled = false;
if (bCreated)
				{
					if (UBlueprint* const Blueprint = Cast<UBlueprint>(CreatedAsset))
					{
						FBlueprintUmgGraphCompileSettleReceipt Settle;
						FBlueprintUmgGraphAdapterError NativeError;
						bSettled = NativeHook.TryCompileAndSettle(
							*Blueprint, Settle, NativeError);
						CreationError = NativeError;
					}
					else
					{
						FMaterialNiagaraGraphCompileSettleReceipt Settle;
						FMaterialNiagaraGraphAdapterError NativeError;
						bSettled = MaterialNativeHook.TryCompileAndSettleMaterial(
							*CreatedAsset, Settle, NativeError);
						CreationError = NativeError;
					}
				}
if (!bCreated || !bSettled)
				{
					CreatedAsset = nullptr;
					Package = nullptr;
					UE::UAI::Contract::FDiagnostic RollbackDetail;
					TryRollbackCreate(
						Request, false, Prepared.ToSharedRef(),
						DurableBackend, FencingToken, *Resources,
						RollbackDetail);
					SetError(OutError, CreationError, Request.GetTargetRef(), RollbackDetail);
					return false;
				}
				OperationEvidence.bNativeUpdateCompleted = bSettled;
				CreatedAsset->SetFlags(RF_Public | RF_Standalone);
				CreatedAsset->GetOutermost()->MarkPackageDirty();
				FAssetRegistryModule::AssetCreated(CreatedAsset);

				FDurableReceipt DurableReceipt;
				if (!TryPersistAssetDurabilityForVerification(
						Prepared.ToSharedRef(), DurableBackend,
						FencingToken, *Resources,
						DurableReceipt, DurableError))
				{
					CreatedAsset = nullptr;
					Package = nullptr;
					UE::UAI::Contract::FDiagnostic UnloadDetail;
					UeAgentAuthoringOperationApply::TryRestorePackageDurability(
						Request.GetTargetRef(), true, Prepared.ToSharedRef(),
						DurableBackend, FencingToken, *Resources, UnloadDetail);
					SetError(OutError, DurableError, Request.GetTargetRef(), UnloadDetail);
					return false;
				}
				OperationEvidence.bSaveCompleted = true;
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
				if (!Expected.IsValid())
				{
					VerifyError = FVerificationError::Failure(TEXT("graph_compound_asset_readback_failed"),
						TEXT("The asset creation plan has no expected state for independent readback."), TEXT("native_graph"));
					VerifyError.SourceKind = TEXT("native_result");
					VerifyError.TargetPath = Request.GetTargetRef();
					VerifyError.PropertyPath = TEXT("expected_state_present");
					VerifyError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					VerifyError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				}
				if (!Expected.IsValid()
					|| !FIndependentReadbackService::TryCreate(
						{Oracle}, Readback, VerifyError)
					|| !Readback->TryObserve(
						FIndependentReadbackRequest(
							TEXT("graph-compound:") + Plan->GetRequestId(),
							AssetCreateReadbackOracleId,
							Request.GetTargetRef(),
							Expected.ToSharedRef()),
						*Resources, Outcome, VerifyError))
				{
					UE::UAI::Contract::FDiagnostic RollbackDetail;
					TryRollbackCreate(
						Request, true, Persisted, DurableBackend,
						FencingToken, *Resources, RollbackDetail);
					SetError(OutError, MoveTemp(VerifyError), Request.GetTargetRef(), RollbackDetail);
					return false;
				}

				const FDurableVerificationAcknowledgement Acknowledgement(
					DurableReceipt.GetJournalId(),
					Plan->GetFingerprint(),
					Prepared->GetInventory().GetInventoryHash(),
					Outcome.GetEvidenceId(),
					JournalOwner,
					FencingToken, Outcome.GetStatus() == EVerificationStatus::Verified);
				if (!TryAcknowledgeAssetDurability(
						Persisted, Acknowledgement, FencingToken,
						*Resources, DurableError))
				{
					UE::UAI::Contract::FDiagnostic RollbackDetail;
					TryRollbackCreate(
						Request, true, Persisted, DurableBackend,
						FencingToken, *Resources, RollbackDetail);
					SetError(OutError, DurableError, Request.GetTargetRef(), RollbackDetail);
					return false;
				}
				return TrySerializeCreateResult(
					Request.GetTargetRef(),
					OperationEvidence,
					OutResult, OutError);
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
		OutEnvironment = MakeShared<FBlueprintMaterialProductionEnvironment>(
			Config);
		return OutEnvironment.IsValid();
	}
}
