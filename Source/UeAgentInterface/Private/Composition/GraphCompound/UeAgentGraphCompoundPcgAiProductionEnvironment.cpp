#include "Domains/SmartObject/Authoring/UeAgentSmartObjectDocument.h"
#include "Misc/ScopeExit.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringPackageReadback.h"
#include "Domains/SmartObject/Authoring/UeAgentSmartObjectAuthoringState.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAiBindings.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AIGraphNode.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "Blueprint/StateTreeConditionBlueprintBase.h"
#include "Blueprint/StateTreeEvaluatorBlueprintBase.h"
#include "Blueprint/StateTreeNodeBlueprintBase.h"

#include "Domains/StateTree/Authoring/UeAgentStateTreeDocument.h"
#include "Domains/Blackboard/Authoring/UeAgentBlackboardDocument.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeAuthoringState.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "Components/StateTreeAIComponentSchema.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/Verification/UeAgentIndependentReadback.h"
#include "DomainAdapters/PcgAiNodeGraph/UeAgentPcgAiNodeGraphNativeHook.h"
#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeNativeGraph.h"
#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeGraphDocument.h"
#include "Domains/Eqs/Authoring/UeAgentEqsGraphDocument.h"
#include "Domains/Eqs/Authoring/UeAgentEqsNativeGraph.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeNativeCapture.h"
#include "Domains/SmartObject/Authoring/UeAgentSmartObjectNativeCapture.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringRegistration.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQueryGraphNode_Option.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Factories/Factory.h"
#include "GameFramework/Actor.h"
#include "InstancedStruct.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "PCGComponent.h"
#include "PCGContext.h"
#include "PCGDataAsset.h"
#include "PCGGraph.h"
#include "PCGLevelToAsset.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "QueryOwners/Pcg/UeAgentPcgNodeCandidateQuery.h"
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#include "Data/PCGPointArrayData.h"
#include "Data/PCGPointData.h"
#include "SmartObjectDefinition.h"
#include "StateTree.h"
#include "StateTreeConditionBase.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeFactory.h"
#include "StateTreeNodeBase.h"
#include "StateTreeState.h"
#include "StateTreeTaskBase.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace AssetDurable;
	using namespace ContextResource;
	using namespace Contract;
	using namespace DomainAdapters::PcgAiNodeGraph;
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
		
		FDiagnostic NativeFailure(
			const TCHAR* Code, const FString& Reason, const FString& Target,
			const FString& Field, TSharedPtr<FJsonValue> Actual,
			TSharedPtr<FJsonValue> Expected)
		{
			FDiagnostic Error = FDiagnostic::Failure(Code, Reason, TEXT("graph_compound"));
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = Target;
			Error.PropertyPath = Field;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		TSharedPtr<FJsonValue> ClassPathValue(const UClass* Class)
		{
			if (Class) return MakeShared<FJsonValueString>(Class->GetPathName());
			return MakeShared<FJsonValueNull>();
		}

		FDiagnostic NativeTypeFailure(
			const TCHAR* Code, const FString& Reason, const FString& Target,
			const FString& Field, const UObject* Actual, const UClass* Expected)
		{
			return NativeFailure(Code, Reason, Target, Field,
				ClassPathValue(Actual ? Actual->GetClass() : nullptr),
				ClassPathValue(Expected));
		}


		struct FAssetCreateSpec final
		{
			FString CapabilityId;
			FString TargetKind;
			FString FactoryClassPath;
			UClass* AssetClass = nullptr;
			bool bUseStateTreeAiSchema = false;
		};

		const TArray<FAssetCreateSpec>& AssetCreateSpecs()
		{
			static const TArray<FAssetCreateSpec> Specs = {
				{TEXT("ai.behavior_tree.asset.create"),
					TEXT("behavior_tree"),
					TEXT("/Script/BehaviorTreeEditor.BehaviorTreeFactory"),
					UBehaviorTree::StaticClass(), false},
				{TEXT("ai.blackboard.asset.create"),
					TEXT("blackboard"),
					TEXT("/Script/BehaviorTreeEditor.BlackboardDataFactory"),
					UBlackboardData::StaticClass(), false},
				{TEXT("ai.eqs.asset.create"),
					TEXT("eqs"),
					TEXT("/Script/EnvironmentQueryEditor.EnvironmentQueryFactory"),
					UEnvQuery::StaticClass(), false},
				{TEXT("ai.smart_object_definition.asset.create"),
					TEXT("smart_object_definition"),
					TEXT("/Script/SmartObjectsEditorModule.SmartObjectDefinitionFactory"),
					USmartObjectDefinition::StaticClass(), false},
				{TEXT("ai.state_tree.asset.create"),
					TEXT("state_tree"),
					TEXT("/Script/StateTreeEditorModule.StateTreeFactory"),
					UStateTree::StaticClass(), true},
				{TEXT("pcg.graph.asset.create"),
					TEXT("pcg_graph"),
					TEXT("/Script/PCGEditor.PCGGraphFactory"),
					UPCGGraph::StaticClass(), false},
			};
			return Specs;
		}

		FString ReadbackOracleId(const FAssetCreateSpec& Spec)
		{
			return TEXT("uai.readback.graph_compound.pcg_ai.")
				+ Spec.TargetKind + TEXT("_create");
		}

		

		template <typename RequestType>
		const FAssetCreateSpec* FindAssetCreateSpec(
			const FPcgAnimationGraphRouteProfile& Profile,
			const RequestType& Request)
		{
			if (Profile.GetInputKind()
				!= EPcgAnimationGraphInputKind::AssetCreateSimple)
			{
				return nullptr;
			}
			for (const FAssetCreateSpec& Spec : AssetCreateSpecs())
			{
				if (Profile.GetCapabilityId() == Spec.CapabilityId
					&& Profile.GetTargetKind() == Spec.TargetKind
					&& Request.GetTargetKind() == Spec.TargetKind)
				{
					return &Spec;
				}
			}
			return nullptr;
		}

		UObject* CreateAssetWithExactFactory(
			const FAssetCreateSpec& Spec,
			UPackage& Package,
			const FName AssetName,
			FGraphCompoundRuntimeError& OutError)
		{
			UFactory* Factory = nullptr;
			if (Spec.bUseStateTreeAiSchema)
			{
				UStateTreeFactory* const StateTreeFactory = NewObject<UStateTreeFactory>();
				if (StateTreeFactory)
				{
					StateTreeFactory->SetSchemaClass(UStateTreeAIComponentSchema::StaticClass());
					Factory = StateTreeFactory;
				}
			}
			else
			{
				UClass* const FactoryClass = LoadObject<UClass>(nullptr, *Spec.FactoryClassPath);
				if (!FactoryClass || !FactoryClass->IsChildOf(UFactory::StaticClass()))
				{
					OutError = NativeFailure(TEXT("graph_compound_pcg_ai_factory_unavailable"),
						!FactoryClass ? TEXT("The configured asset factory class could not be loaded.")
							: TEXT("The configured asset factory class does not derive from UFactory."),
						Spec.CapabilityId, TEXT("factory.class"),
						MakeShared<FJsonValueString>(Spec.FactoryClassPath),
						ClassPathValue(UFactory::StaticClass()));
					return nullptr;
				}
				Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
			}
			if (!Factory)
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_ai_factory_unavailable"),
					TEXT("The asset factory instance could not be created."),
					Spec.CapabilityId, TEXT("factory.present"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = Spec.FactoryClassPath;
				return nullptr;
			}
			if (Factory->GetClass()->GetPathName() != Spec.FactoryClassPath)
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_ai_factory_unavailable"),
					TEXT("The created factory class differs from the configured factory class."),
					Spec.CapabilityId, TEXT("factory.class"), ClassPathValue(Factory->GetClass()),
					MakeShared<FJsonValueString>(Spec.FactoryClassPath));
				return nullptr;
			}
			if (Factory->GetSupportedClass() != Spec.AssetClass)
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_ai_factory_unavailable"),
					TEXT("The factory supports a different asset class."),
					Spec.CapabilityId, TEXT("factory.supported_class"),
					ClassPathValue(Factory->GetSupportedClass()), ClassPathValue(Spec.AssetClass));
				return nullptr;
			}
			UObject* const Created = Factory->FactoryCreateNew(
				Spec.AssetClass, &Package, AssetName,
				RF_Public | RF_Standalone | RF_Transactional,
				nullptr, GWarn);
			if (!Created || Created->GetClass() != Spec.AssetClass)
			{
				OutError = NativeTypeFailure(TEXT("graph_compound_pcg_ai_factory_failed"),
					!Created ? TEXT("The asset factory returned no asset.")
						: TEXT("The asset factory returned an unexpected asset class."),
					AssetName.ToString(), TEXT("asset.class"), Created, Spec.AssetClass);
				OutError.CandidateName = Spec.FactoryClassPath;
				OutError.InternalContext = Package.GetName();
				return nullptr;
			}
			return Created;
		}

		FString AssetName(const FString& TargetRef)
		{
			return TargetRef.Contains(TEXT("."))
				? FPackageName::ObjectPathToObjectName(TargetRef)
				: FPackageName::GetLongPackageAssetName(TargetRef);
		}

		FString ObjectPath(const FString& TargetRef)
		{
			return TargetRef.Contains(TEXT("."))
				? TargetRef
				: TargetRef + TEXT(".") + AssetName(TargetRef);
		}

		FString PackageName(const FString& TargetRef)
		{
			return FPackageName::ObjectPathToPackageName(TargetRef);
		}

		UObject* FindTopLevelAsset(
			UPackage& Package,
			const FString& TargetRef)
		{
			UObject* const Asset = FindObject<UObject>(
				&Package, *AssetName(TargetRef));
			return Asset && Asset->GetOutermost() == &Package
				? Asset : nullptr;
		}

		bool TryPackageFilename(
			const FString& PackageName, FString& OutFilename, FDiagnostic& OutError)
		{
			OutFilename.Reset();
			FText PathReason;
			if (!FPackageName::IsValidLongPackageName(PackageName, false, &PathReason))
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
					PathReason.IsEmpty() ? TEXT("The target is not a valid long package name.") : PathReason.ToString(),
					PackageName, TEXT("package_name"), MakeShared<FJsonValueString>(PackageName),
					MakeShared<FJsonValueString>(TEXT("valid long package name")));
				return false;
			}
			if (FPackageName::GetPackageMountPoint(PackageName).IsNone())
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
					TEXT("The target package has no registered mount point."),
					PackageName, TEXT("package.mount_point_present"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			if (FPackageName::GetLongPackageAssetName(PackageName).IsEmpty())
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
					TEXT("The target package has no asset name."),
					PackageName, TEXT("package.asset_name"), MakeShared<FJsonValueString>(FString()),
					MakeShared<FJsonValueString>(TEXT("non-empty asset name")));
				return false;
			}
			if (!FPackageName::TryConvertLongPackageNameToFilename(
					PackageName, OutFilename, FPackageName::GetAssetPackageExtension()))
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
					TEXT("The target package could not be converted to a filename."),
					PackageName, TEXT("package.filename_resolved"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			return true;
		}

		bool TryCheckTargetAbsent(
			const FGraphCompoundPcgAiTargetRequest& Request,
			FString& OutFilename,
			FGraphCompoundRuntimeError& OutError)
		{
			if (!Request.IsValid())
			{
				OutError = Request.GetValidationDiagnostic();
				return false;
			}
			if (!TryPackageFilename(Request.GetTargetRef(), OutFilename, OutError)) return false;
			bool bTargetPresent = false;
			if (!HjsonWorkspace::DomainAuthoring::
					TryInspectObjectTargetPresence(
						Request.GetTargetRef(), bTargetPresent))
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
					TEXT("The target asset presence could not be inspected."),
					Request.GetTargetRef(), TEXT("target.presence_inspected"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			if (bTargetPresent)
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_ai_target_already_exists"),
					TEXT("The target asset already exists."),
					Request.GetTargetRef(), TEXT("target.present"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
				return false;
			}
			return true;
		}

		bool TryCreateAbsentSnapshot(
			const FGraphCompoundPcgAiTargetRequest& Request,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError)
		{
			ON_SCOPE_EXIT { MarkGraphCompoundNativeDiagnostic(OutError); };
			OutSnapshot.Reset();
			FString Filename;
			if (!TryCheckTargetAbsent(Request, Filename, OutError))
			{
				return false;
			}
			FUeAgentStateSemanticError Error;
			const bool bCreated = FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("graph-compound:c4:pcg-ai-absent:")
						+ Request.GetTargetRef(),
					EUeAgentStatePlane::Persisted,
					FUeAgentStateRevision(
						TEXT("graph-compound:c4:pcg-ai-asset-create"),
						TEXT("absent:") + Request.GetTargetRef(),
						0),
					FUeAgentTemporalPoint(
						TEXT("utc-seconds"),
						FString::Printf(
							TEXT("%lld"),
							FDateTime::UtcNow().ToUnixTimestamp())),
					FUeAgentStateProvenance(
						TEXT("independent_readback"),
						Request.GetTargetRef(),
						TEXT("file-absence:") + Filename),
					{}, Resources, OutSnapshot, Error);
			if (!bCreated || !OutSnapshot.IsValid())
			{
				OutError = Error.IsEmpty() ? NativeFailure(
					TEXT("graph_compound_pcg_ai_snapshot_invalid"),
					bCreated ? TEXT("Absent-target snapshot construction returned without a snapshot.")
						: TEXT("Absent-target snapshot construction failed without a diagnostic."),
					Request.GetTargetRef(), bCreated ? TEXT("snapshot.present") : TEXT("snapshot.create_succeeded"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)) : Error;
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetTargetRef();
				return false;
			}
			return true;
		}


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

		bool IsSchemaMutationProfile(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FGraphCompoundPcgAiTargetRequest& Request)
		{
			return (Profile.GetCapabilityId()
					== TEXT("ai.behavior_tree.graph.apply")
				&& Profile.GetInputKind()
					== EPcgAnimationGraphInputKind::GraphEdits
				&& Profile.GetTargetKind() == TEXT("behavior_tree")
				&& Request.GetTargetKind() == TEXT("behavior_tree"))
				|| (Profile.GetCapabilityId()
					== TEXT("ai.blackboard.schema.apply")
				&& Profile.GetInputKind()
					== EPcgAnimationGraphInputKind::BlackboardSchema
				&& Profile.GetTargetKind() == TEXT("blackboard")
				&& Request.GetTargetKind() == TEXT("blackboard"))
				|| (Profile.GetCapabilityId()
					== TEXT("ai.eqs.graph.apply")
				&& Profile.GetInputKind()
					== EPcgAnimationGraphInputKind::GraphEdits
				&& Profile.GetTargetKind() == TEXT("eqs")
				&& Request.GetTargetKind() == TEXT("eqs"))
				|| (Profile.GetCapabilityId()
					== TEXT("ai.smart_object_definition.apply")
				&& Profile.GetInputKind()
					== EPcgAnimationGraphInputKind::SmartObjectDefinition
				&& Profile.GetTargetKind() == TEXT("smart_object_definition")
				&& Request.GetTargetKind() == TEXT("smart_object_definition"))
				|| (Profile.GetCapabilityId()
					== TEXT("ai.state_tree.hierarchy.apply")
				&& Profile.GetInputKind()
					== EPcgAnimationGraphInputKind::GraphEdits
				&& Profile.GetTargetKind() == TEXT("state_tree")
				&& Request.GetTargetKind() == TEXT("state_tree"))
				|| (Profile.GetCapabilityId() == TEXT("pcg.graph.apply")
					&& Profile.GetInputKind()
						== EPcgAnimationGraphInputKind::GraphEdits
					&& Profile.GetTargetKind() == TEXT("pcg_graph")
					&& Request.GetTargetKind() == TEXT("pcg_graph"))
				|| (Profile.GetCapabilityId()
					== TEXT("pcg.graph_instance.apply")
					&& Profile.GetInputKind()
						== EPcgAnimationGraphInputKind::PcgGraphInstance
					&& Profile.GetTargetKind() == TEXT("pcg_graph_instance")
					&& Request.GetTargetKind() == TEXT("pcg_graph_instance"))
				|| (Profile.GetCapabilityId()
					== TEXT("pcg.graph_parameters.apply")
					&& Profile.GetInputKind()
						== EPcgAnimationGraphInputKind::PcgParameters
					&& Profile.GetTargetKind() == TEXT("pcg_graph")
					&& Request.GetTargetKind() == TEXT("pcg_graph"))
				|| (Profile.GetCapabilityId()
					== TEXT("pcg.component.apply")
					&& Profile.GetInputKind()
						== EPcgAnimationGraphInputKind::PcgComponentBinding
					&& Profile.GetTargetKind() == TEXT("pcg_component")
					&& Request.GetTargetKind() == TEXT("pcg_component"))
				|| (Profile.GetCapabilityId()
					== TEXT("pcg.component.world_binding.apply")
					&& Profile.GetInputKind()
						== EPcgAnimationGraphInputKind::PcgWorldBinding
					&& Profile.GetTargetKind() == TEXT("pcg_component")
					&& Request.GetTargetKind() == TEXT("pcg_component"))
				|| (Profile.GetCapabilityId()
					== TEXT("pcg.data_asset.apply")
					&& Profile.GetInputKind()
						== EPcgAnimationGraphInputKind::PcgDataAsset
					&& Profile.GetTargetKind() == TEXT("pcg_data_asset")
					&& Request.GetTargetKind() == TEXT("pcg_data_asset"));
		}

		bool IsPcgDataFromLevelProfile(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FGraphCompoundPcgAiTargetRequest& Request)
		{
			return Profile.GetCapabilityId()
					== TEXT("pcg.data_asset.create_from_level")
				&& Profile.GetInputKind()
					== EPcgAnimationGraphInputKind::PcgDataFromLevel
				&& Profile.GetTargetKind() == TEXT("pcg_data_asset")
				&& Request.GetTargetKind() == TEXT("pcg_data_asset");
		}

		bool IsPcgComponentMutationProfile(
			const FPcgAnimationGraphRouteProfile& Profile)
		{
			return Profile.GetTargetKind() == TEXT("pcg_component")
				&& ((Profile.GetCapabilityId() == TEXT("pcg.component.apply")
						&& Profile.GetInputKind()
							== EPcgAnimationGraphInputKind::PcgComponentBinding)
					|| (Profile.GetCapabilityId()
							== TEXT("pcg.component.world_binding.apply")
						&& Profile.GetInputKind()
							== EPcgAnimationGraphInputKind::PcgWorldBinding));
		}

		UPCGComponent* FindExactLoadedPcgComponent(const FString& TargetRef)
		{
			UPCGComponent* const Component = TargetRef.IsEmpty()
				? nullptr : FindObject<UPCGComponent>(nullptr, *TargetRef);
			return Component && Component->GetPathName() == TargetRef
				? Component : nullptr;
		}

		bool TryPublicPcgGenerationPolicy(
			const EPCGComponentGenerationTrigger Policy,
			FString& OutPolicy)
		{
			switch (Policy)
			{
			case EPCGComponentGenerationTrigger::GenerateOnDemand:
				OutPolicy = TEXT("on_demand");
				return true;
			case EPCGComponentGenerationTrigger::GenerateOnLoad:
				OutPolicy = TEXT("on_load");
				return true;
			case EPCGComponentGenerationTrigger::GenerateAtRuntime:
				OutPolicy = TEXT("runtime");
				return true;
			default:
				OutPolicy.Reset();
				return false;
			}
		}

		bool TryNativePcgGenerationPolicy(
			const FString& PublicPolicy,
			FString& OutNativePolicy,
			EPCGComponentGenerationTrigger& OutPolicy)
		{
			if (PublicPolicy == TEXT("on_demand"))
			{
				OutNativePolicy = TEXT("generate_on_demand");
				OutPolicy = EPCGComponentGenerationTrigger::GenerateOnDemand;
				return true;
			}
			if (PublicPolicy == TEXT("on_load"))
			{
				OutNativePolicy = TEXT("generate_on_load");
				OutPolicy = EPCGComponentGenerationTrigger::GenerateOnLoad;
				return true;
			}
			if (PublicPolicy == TEXT("runtime"))
			{
				OutNativePolicy = TEXT("generate_at_runtime");
				OutPolicy = EPCGComponentGenerationTrigger::GenerateAtRuntime;
				return true;
			}
			OutNativePolicy.Reset();
			return false;
		}

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

		bool TryCanonicalDouble(
			const FUeAgentCanonicalValue& Value,
			double& OutValue)
		{
			if (Value.GetKind() != EUeAgentValueKind::Integer
				&& Value.GetKind() != EUeAgentValueKind::Decimal)
			{
				return false;
			}
			return TryParseFiniteDecimal(Value.GetLexeme(), OutValue);
		}




		class FRequiredCanonicalInput final
		{
		public:
			FRequiredCanonicalInput(const TCHAR* InCode, FString InTarget, FString InSemanticId,
				FString InValuePointer, const bool bInGenerated, FDiagnostic& InError, TOptional<int32> InIndex)
				: Code(InCode), Target(MoveTemp(InTarget)), SemanticId(MoveTemp(InSemanticId)),
				ValuePointer(MoveTemp(InValuePointer)), bGenerated(bInGenerated), Error(InError), OperationIndex(InIndex)
			{}

			bool Fail(const FString& Reason, const FString& RelativePointer,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected) const
			{
				Error = FDiagnostic::Failure(Code, Reason, TEXT("graph_compound"));
				Error.TargetPath = Target;
				Error.SubjectId = SemanticId;
				Error.OperationIndex = OperationIndex;
				Error.PropertyPath = RelativePointer;
				Error.ActualValuePresent = Actual.IsValid();
				Error.ExpectedValuePresent = Expected.IsValid();
				Error.ActualValue = MoveTemp(Actual);
				Error.ExpectedValue = MoveTemp(Expected);
				Error.Source.JsonPath = RelativePointer.IsEmpty() ? TEXT("/") : RelativePointer;
				if (bGenerated || ValuePointer.IsEmpty()) MarkGraphCompoundNativeDiagnostic(Error);
				else BindGraphCompoundInputValueDiagnostic(Error, ValuePointer);
				return false;
			}

			FRequiredCanonicalInput At(const FString& RelativePointer) const
			{
				return FRequiredCanonicalInput(Code, Target, SemanticId,
					(ValuePointer == TEXT("/") ? FString() : ValuePointer) + RelativePointer,
					bGenerated || ValuePointer.IsEmpty(), Error, OperationIndex);
			}

			bool RequireFields(const FUeAgentCanonicalValue& Object,
				std::initializer_list<const TCHAR*> Fields, const bool bExact = true) const
			{
				if (Object.GetKind() != EUeAgentValueKind::Object)
					return Fail(TEXT("The mutation value must be an object."), FString(),
						CanonicalInputFact(&Object), MakeShared<FJsonValueString>(TEXT("object")));
				if (bExact)
				{
					// Diagnose the failed field set without changing the exact-count predicate.
					if (Object.GetObjectItems().Num() != static_cast<int32>(Fields.size()))
					{
						for (const TCHAR* Field : Fields)
							if (!FindObjectValue(Object, Field))
								return MissingField(Field);
						for (const auto& Item : Object.GetObjectItems())
						{
							bool bAllowed = false;
							for (const TCHAR* Field : Fields) if (Item.Key == Field) { bAllowed = true; break; }
							if (!bAllowed)
								return Fail(TEXT("The mutation value contains an unsupported field."),
									FieldPointer(Item.Key), CanonicalInputFact(&Item.Value.Get()), nullptr);
						}
						return Fail(TEXT("The mutation value has an incorrect field count."), FString(),
							MakeShared<FJsonValueNumber>(Object.GetObjectItems().Num()),
							MakeShared<FJsonValueNumber>(static_cast<int32>(Fields.size())));
					}
					for (const TCHAR* Field : Fields)
						if (!FindObjectValue(Object, Field)) return MissingField(Field);
				}
				else
				{
					for (const auto& Item : Object.GetObjectItems())
					{
						bool bAllowed = false;
						for (const TCHAR* Field : Fields) if (Item.Key == Field) { bAllowed = true; break; }
						if (!bAllowed)
							return Fail(TEXT("The mutation value contains an unsupported field."),
								FieldPointer(Item.Key), CanonicalInputFact(&Item.Value.Get()), nullptr);
					}
				}
				return true;
			}

			bool ReadString(const FUeAgentCanonicalValue& Object, const TCHAR* Field, FString& Out) const
			{
				const FUeAgentCanonicalValue* Value = FindObjectValue(Object, Field);
				if (!Value || Value->GetKind() != EUeAgentValueKind::String)
				{
					Out.Reset();
					return Fail(Value ? TEXT("The mutation field must be a string.")
						: TEXT("The required mutation field is missing."),
						FieldPointer(Field), CanonicalInputFact(Value),
						MakeShared<FJsonValueString>(TEXT("non-empty string")));
				}
				Out = Value->GetText();
				if (Out.IsEmpty())
					return Fail(TEXT("The mutation string field must not be empty."),
						FieldPointer(Field), CanonicalInputFact(Value),
						MakeShared<FJsonValueString>(TEXT("non-empty string")));
				return true;
			}

			bool ReadStringArray(const FUeAgentCanonicalValue& Object, const TCHAR* Field, TArray<FString>& Out) const
			{
				Out.Reset();
				const FUeAgentCanonicalValue* Value = FindObjectValue(Object, Field);
				if (!Value || Value->GetKind() != EUeAgentValueKind::Array)
					return Fail(Value ? TEXT("The mutation field must be an array.")
						: TEXT("The required mutation field is missing."),
						FieldPointer(Field), CanonicalInputFact(Value),
						MakeShared<FJsonValueString>(TEXT("array of non-empty strings")));
				for (int32 Index = 0; Index < Value->GetArrayItems().Num(); ++Index)
				{
					const FUeAgentCanonicalValue& Item = Value->GetArrayItems()[Index].Get();
					if (Item.GetKind() != EUeAgentValueKind::String || Item.GetText().IsEmpty())
					{
						Out.Reset();
						return Fail(TEXT("The mutation array item must be a non-empty string."),
							FieldPointer(Field) + FString::Printf(TEXT("/%d"), Index),
							CanonicalInputFact(&Item), MakeShared<FJsonValueString>(TEXT("non-empty string")));
					}
					Out.Add(Item.GetText());
				}
				return true;
			}

			bool ReadInteger(const FUeAgentCanonicalValue& Object, const TCHAR* Field, int32& Out,
				const bool bSigned = false) const
			{
				const FUeAgentCanonicalValue* Value = FindObjectValue(Object, Field);
				const int64 Minimum = bSigned ? MIN_int32 : 0;
				const TSharedPtr<FJsonValue> Expected = MakeShared<FJsonValueString>(FString::Printf(
					TEXT("integer in [%lld, %d]"), Minimum, MAX_int32));
				if (!Value || (Value->GetKind() != EUeAgentValueKind::Integer
					&& Value->GetKind() != EUeAgentValueKind::Decimal))
					return Fail(Value ? TEXT("The mutation field must be numeric.")
						: TEXT("The required mutation field is missing."),
						FieldPointer(Field), CanonicalInputFact(Value), Expected);
				int64 Parsed = 0;
				if (!TryParseInt64Number(Value->GetLexeme(), Parsed) || Parsed < Minimum || Parsed > MAX_int32)
					return Fail(TEXT("The mutation number is not an integer within the supported range."),
						FieldPointer(Field), CanonicalInputFact(Value), Expected);
				Out = static_cast<int32>(Parsed);
				return true;
			}

			bool ReadVector2(const FUeAgentCanonicalValue& Object, const TCHAR* Field, float& OutX, float& OutY) const
			{
				const FUeAgentCanonicalValue* Value = FindObjectValue(Object, Field);
				if (!Value || Value->GetKind() != EUeAgentValueKind::Array || Value->GetArrayItems().Num() != 2)
					return Fail(TEXT("The mutation position must be an array with exactly two coordinates."),
						FieldPointer(Field), CanonicalInputFact(Value),
						MakeShared<FJsonValueString>(TEXT("array of two finite float-range numbers")));
				double Parsed[2] = {0.0, 0.0};
				for (int32 Index = 0; Index < 2; ++Index)
				{
					const FUeAgentCanonicalValue& Coordinate = Value->GetArrayItems()[Index].Get();
					if (!TryCanonicalDouble(Coordinate, Parsed[Index]))
						return Fail(TEXT("The mutation coordinate must be a finite number."),
							FieldPointer(Field) + FString::Printf(TEXT("/%d"), Index), CanonicalInputFact(&Coordinate),
							MakeShared<FJsonValueString>(TEXT("finite number")));
				}
				for (int32 Index = 0; Index < 2; ++Index)
					if (Parsed[Index] < -static_cast<double>(MAX_flt) || Parsed[Index] > static_cast<double>(MAX_flt))
						return Fail(TEXT("The mutation coordinate is outside the float range."),
							FieldPointer(Field) + FString::Printf(TEXT("/%d"), Index),
							CanonicalInputFact(&Value->GetArrayItems()[Index].Get()),
							MakeShared<FJsonValueString>(FString::Printf(TEXT("number in [%g, %g]"), -MAX_flt, MAX_flt)));
				OutX = static_cast<float>(Parsed[0]);
				OutY = static_cast<float>(Parsed[1]);
				return true;
			}

		private:
			static FString FieldPointer(FString Field)
			{
				Field.ReplaceInline(TEXT("~"), TEXT("~0"));
				Field.ReplaceInline(TEXT("/"), TEXT("~1"));
				return TEXT("/") + Field;
			}
			bool MissingField(const FString& Field) const
			{
				return Fail(TEXT("The required mutation field is missing."), FieldPointer(Field),
					nullptr, MakeShared<FJsonValueString>(TEXT("required field")));
			}
			const TCHAR* Code;
			FString Target;
			FString SemanticId;
			FString ValuePointer;
			bool bGenerated;
			FDiagnostic& Error;
			TOptional<int32> OperationIndex;
		};

		bool TryMakeCanonicalDouble(
			const double Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError)
		{
			if (!FMath::IsFinite(Value))
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_parameter_value_invalid"),
					TEXT("The native parameter value is not finite."),
					FString(), TEXT("value"), MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Value)),
					MakeShared<FJsonValueString>(TEXT("finite double")));
				return false;
			}
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryDecimal(
					FString::Printf(TEXT("%.17g"), Value), Resources, OutValue, StateError))
			{
				SetError(OutError, StateError, FString());
				return false;
			}
			return true;
		}

		FString PcgParameterSemanticId(
			const FPropertyBagPropertyDesc& Desc)
		{
			return FString::Printf(TEXT("property:parameter:%s:%s:%s"),
				*Desc.ID.ToString(EGuidFormats::DigitsWithHyphensLower),
				*Desc.Name.ToString(),
				Desc.ValueType == EPropertyBagPropertyType::Double
					? TEXT("double") : TEXT("unsupported"));
		}

		FString StateTreeStateSemanticId(const FGuid& StateGuid)
		{
			return TEXT("state:") + StateGuid.ToString(
				EGuidFormats::DigitsWithHyphensLower);
		}

		bool TryParseStateTreeStateIdentity(
			const FString& SemanticId,
			FGuid& OutGuid)
		{
			const FString Prefix = TEXT("state:");
			return SemanticId.StartsWith(Prefix)
				&& FGuid::Parse(SemanticId.RightChop(Prefix.Len()), OutGuid)
				&& OutGuid.IsValid()
				&& SemanticId == StateTreeStateSemanticId(OutGuid);
		}

		bool TryCollectStateTreeStates(
			UStateTreeEditorData& EditorData,
			TMap<FGuid, UStateTreeState*>& OutStates,
			TMap<FGuid, FString>& OutParents, FDiagnostic& OutError)
		{
			OutStates.Reset();
			OutParents.Reset();
			TArray<UeAgentStateTreeAuthoring::FStateCapture> States;
			if (!UeAgentStateTreeAuthoring::CaptureHierarchy(EditorData, States, OutError))
			{
				MarkGraphCompoundNativeDiagnostic(OutError);
				return false;
			}
			for (const auto& Entry : States)
			{
				OutStates.Add(Entry.State->ID, const_cast<UStateTreeState*>(Entry.State));
				OutParents.Add(Entry.State->ID, Entry.Parent ? StateTreeStateSemanticId(Entry.Parent->ID) : TEXT("root"));
			}
			return true;
		}

		bool TryCanonicalObject(
			const TSharedRef<FJsonObject>& Object,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError)
		{
			ON_SCOPE_EXIT { MarkGraphCompoundNativeDiagnostic(OutError); };
			if (!CanonicalJson::TryFromJsonValue(
					MakeShared<FJsonValueObject>(Object), Resources, OutValue, OutError))
			{
				return false;
			}
			return true;
		}

		const FString PcgDataContentRevisionTagPrefix =
			TEXT("uai.content_revision=");

		FString PcgDataContentRevision(
			const UPCGDataAsset& DataAsset,
			const UPCGPointData& PointData)
		{
			if (DataAsset.Data.TaggedData.Num() == 1)
			{
				for (const FString& Tag : DataAsset.Data.TaggedData[0].Tags)
				{
					if (Tag.StartsWith(PcgDataContentRevisionTagPrefix))
					{
						const FString Revision =
							Tag.Mid(PcgDataContentRevisionTagPrefix.Len());
						if (!Revision.IsEmpty())
						{
							return Revision;
						}
					}
				}
			}
			return TEXT("md5:") + FMD5::HashAnsiString(*FString::Printf(
				TEXT("%s|%s|%d"), *DataAsset.GetPathName(),
				*DataAsset.ObjectPath.ToString(), PointData.GetPoints().Num()));
		}

		bool TryCapturePcgDataAssetValue(
			UPCGDataAsset& DataAsset,
			FUeAgentResourceTracker& Resources,
			FString& OutSemanticId,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError)
		{
			OutSemanticId.Reset();
			OutValue.Reset();
			if (DataAsset.Data.TaggedData.Num() != 1)
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_data_asset_content_invalid"),
					TEXT("The PCG data asset must contain exactly one tagged data entry."),
					DataAsset.GetPathName(), TEXT("tagged_data.count"),
					MakeShared<FJsonValueNumber>(DataAsset.Data.TaggedData.Num()),
					MakeShared<FJsonValueNumber>(1));
				return false;
			}
			const UPCGPointData* const PointData = Cast<UPCGPointData>(
				DataAsset.Data.TaggedData[0].Data.Get());
			if (!PointData || PointData->GetClass() != UPCGPointData::StaticClass())
			{
				OutError = NativeTypeFailure(TEXT("graph_compound_pcg_data_asset_content_invalid"),
					TEXT("The tagged data entry must have the exact PCGPointData class."),
					DataAsset.GetPathName(), TEXT("tagged_data[0].class"),
					DataAsset.Data.TaggedData[0].Data.Get(), UPCGPointData::StaticClass());
				return false;
			}
			if (!DataAsset.ObjectPath.IsValid())
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_data_asset_content_invalid"),
					TEXT("The PCG data asset source object path is invalid."),
					DataAsset.GetPathName(), TEXT("source_ref"),
					MakeShared<FJsonValueString>(DataAsset.ObjectPath.ToString()),
					MakeShared<FJsonValueString>(TEXT("valid soft object path")));
				return false;
			}

			const FString DataTypeRef = PointData->GetClass()->GetPathName();
			OutSemanticId = TEXT("property:data_content:") + DataTypeRef;
			const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(
				TEXT("content_revision"),
				PcgDataContentRevision(DataAsset, *PointData));
			Value->SetStringField(TEXT("data_type_ref"), DataTypeRef);
			Value->SetStringField(
				TEXT("source_ref"), DataAsset.ObjectPath.ToString());
			const TSharedRef<FJsonObject> Content = MakeShared<FJsonObject>();
			Content->SetNumberField(
				TEXT("point_count"), PointData->GetPoints().Num());
			Value->SetObjectField(TEXT("value"), Content);
			return TryCanonicalObject(Value, Resources, OutValue, OutError);
		}

		struct FCapturedMutationValue final
		{
			FString SemanticId;
			FUeAgentCanonicalValuePtr Value;
			bool bLocal = true;
		};

		FString BehaviorTreeNodeSemanticId(const UEdGraphNode& Node)
		{
			return Node.IsA<UBehaviorTreeGraphNode_Root>()
				? TEXT("node:root")
				: TEXT("node:") + Node.NodeGuid.ToString(
					EGuidFormats::DigitsWithHyphensLower);
		}

		int32 DirectionPinIndex(
			const UEdGraphNode& Node,
			const UEdGraphPin& Pin,
			const EEdGraphPinDirection Direction)
		{
			int32 Index = 0;
			for (const UEdGraphPin* Candidate : Node.Pins)
			{
				if (Candidate && Candidate->Direction == Direction)
				{
					if (Candidate == &Pin)
					{
						return Index;
					}
					++Index;
				}
			}
			return INDEX_NONE;
		}

		FString BehaviorTreeRelationSemanticId(
			const FString& FromItem,
			const int32 FromIndex,
			const FString& ToItem,
			const int32 ToIndex)
		{
			return FString::Printf(
				TEXT("relation:%s:out:%d->%s:in:%d"),
				*FromItem, FromIndex, *ToItem, ToIndex);
		}

		bool TryCaptureMutationValues(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FString& TargetRef,
			FUeAgentResourceTracker& Resources,
			TArray<FCapturedMutationValue>& OutValues,
			FString& OutGeneration,
			FGraphCompoundRuntimeError& OutError)
		{
			ON_SCOPE_EXIT { MarkGraphCompoundNativeDiagnostic(OutError); };
			OutValues.Reset();
			OutGeneration.Reset();
			UObject* const Target = IsPcgComponentMutationProfile(Profile)
				? FindExactLoadedPcgComponent(TargetRef)
				: LoadObject<UObject>(nullptr, *TargetRef);
			if (!Target || Target->GetPathName() != TargetRef)
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
					!Target ? TEXT("The native PCG/AI target could not be found or loaded.")
						: TEXT("The resolved native target path differs from the requested target."),
					TargetRef, TEXT("target.path"),
					MakeShared<FJsonValueString>(Target ? Target->GetPathName() : FString()),
					MakeShared<FJsonValueString>(TargetRef));
				return false;
			}

			if (Profile.GetCapabilityId()
				== TEXT("ai.behavior_tree.graph.apply"))
			{
				UBehaviorTree* const Tree = Cast<UBehaviorTree>(Target);
				if (!Tree)
				{
					OutError = NativeTypeFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
						TEXT("The native target is not an instance of the required asset class."),
						TargetRef, TEXT("target.class"), Target, UBehaviorTree::StaticClass());
					return false;
				}
#if WITH_EDITORONLY_DATA
				const UBehaviorTreeGraph* const Graph =
					Cast<UBehaviorTreeGraph>(Tree->BTGraph);
				TMap<const UEdGraphNode*, FString> NodeIds;
				if (Graph)
				{
					for (const UEdGraphNode* Node : Graph->Nodes)
					{
						if (!Node || (!Node->NodeGuid.IsValid()
							&& !Node->IsA<UBehaviorTreeGraphNode_Root>()))
						{
							continue;
						}
						const FString SemanticId =
							BehaviorTreeNodeSemanticId(*Node);
						const TSharedRef<FJsonObject> Definition =
							MakeShared<FJsonObject>();
						Definition->SetStringField(
							TEXT("carrier_type_ref"), Node->GetClass()->GetPathName());
						if (const UAIGraphNode* const AiNode = Cast<UAIGraphNode>(Node))
						{
							if (AiNode->NodeInstance)
							{
								Definition->SetStringField(
									TEXT("payload_type_ref"),
									AiNode->NodeInstance->GetClass()->GetPathName());
							}
						}
						FUeAgentCanonicalValuePtr Value;
						if (!TryCanonicalObject(
								Definition, Resources, Value, OutError))
						{
							return false;
						}
						OutValues.Add({SemanticId, Value, true});
						NodeIds.Add(Node, SemanticId);
						const UBehaviorTreeGraphNode* const BehaviorNode =
							Cast<UBehaviorTreeGraphNode>(Node);
						if (BehaviorNode)
						{
							for (const UAIGraphNode* SubNode : BehaviorNode->SubNodes)
							{
								if (!SubNode || !SubNode->NodeGuid.IsValid()
									|| !SubNode->NodeInstance)
								{
									continue;
								}
								const TSharedRef<FJsonObject> SubDefinition =
									MakeShared<FJsonObject>();
								SubDefinition->SetStringField(
									TEXT("carrier_type_ref"),
									SubNode->GetClass()->GetPathName());
								SubDefinition->SetStringField(
									TEXT("payload_type_ref"),
									SubNode->NodeInstance->GetClass()->GetPathName());
								if (!TryCanonicalObject(
										SubDefinition, Resources, Value, OutError))
								{
									return false;
								}
								OutValues.Add({
									TEXT("node:") + SubNode->NodeGuid.ToString(
										EGuidFormats::DigitsWithHyphensLower),
									Value,
									true});
							}
						}
					}
					for (const UEdGraphNode* Node : Graph->Nodes)
					{
						const FString* const FromItem = NodeIds.Find(Node);
						if (!Node || !FromItem)
						{
							continue;
						}
						for (const UEdGraphPin* FromPin : Node->Pins)
						{
							if (!FromPin || FromPin->Direction != EGPD_Output)
							{
								continue;
							}
							for (const UEdGraphPin* ToPin : FromPin->LinkedTo)
							{
								const FString* const ToItem = ToPin
									? NodeIds.Find(ToPin->GetOwningNode()) : nullptr;
								const int32 FromIndex = DirectionPinIndex(
									*Node, *FromPin, EGPD_Output);
								const int32 ToIndex = ToPin
									? DirectionPinIndex(*ToPin->GetOwningNode(),
										*ToPin, EGPD_Input) : INDEX_NONE;
								if (!ToItem || FromIndex < 0 || ToIndex < 0)
								{
									continue;
								}
								const TSharedRef<FJsonObject> Definition =
									MakeShared<FJsonObject>();
								Definition->SetStringField(
									TEXT("from_item_ref"), *FromItem);
								Definition->SetNumberField(
									TEXT("from_output_index"), FromIndex);
								Definition->SetStringField(
									TEXT("to_item_ref"), *ToItem);
								Definition->SetNumberField(
									TEXT("to_input_index"), ToIndex);
								FUeAgentCanonicalValuePtr Value;
								if (!TryCanonicalObject(
										Definition, Resources, Value, OutError))
								{
									return false;
								}
								OutValues.Add({
									BehaviorTreeRelationSemanticId(
										*FromItem, FromIndex, *ToItem, ToIndex),
									Value, true});
							}
						}
					}
				}
				if (Tree->BlackboardAsset)
				{
					FUeAgentCanonicalValuePtr Value;
					FUeAgentStateSemanticError StateError;
					if (!FUeAgentCanonicalValue::TryString(
							Tree->BlackboardAsset->GetPathName(),
							Resources,
							Value,
							StateError))
					{
						SetError(OutError, StateError, TargetRef);
						return false;
					}
					OutValues.Add({TEXT("binding:blackboard_asset"), Value, true});
				}
#endif
			}
			else if (Profile.GetCapabilityId()
				== TEXT("ai.blackboard.schema.apply"))
			{
				UBlackboardData* const Blackboard = Cast<UBlackboardData>(Target);
				if (!Blackboard)
				{
					OutError = NativeTypeFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
						TEXT("The native target is not an instance of the required asset class."),
						TargetRef, TEXT("target.class"), Target, UBlackboardData::StaticClass());
					return false;
				}
				auto AddEntries = [&] (
					const TArray<FBlackboardEntry>& Entries,
					const TCHAR* Ownership,
					const bool bLocal) -> bool
				{
					for (const FBlackboardEntry& Entry : Entries)
					{
						if (Entry.EntryName.IsNone() || !Entry.KeyType)
						{
							continue;
						}
						const FString KeyType =
							Entry.KeyType->GetClass()->GetPathName();
						const FString SemanticId =
							TEXT("item:blackboard_entry:")
							+ Entry.EntryName.ToString() + TEXT(":")
							+ KeyType + TEXT(":") + Ownership;
						const TSharedRef<FJsonObject> Definition =
							MakeShared<FJsonObject>();
						Definition->SetStringField(TEXT("key_type_ref"), KeyType);
						Definition->SetStringField(TEXT("ownership"), Ownership);
						FUeAgentCanonicalValuePtr Value;
						if (!TryCanonicalObject(
								Definition, Resources, Value, OutError))
						{
							return false;
						}
						OutValues.Add({SemanticId, Value, bLocal});
					}
					return true;
				};
				if (!AddEntries(Blackboard->Keys, TEXT("local"), true))
				{
					return false;
				}
#if WITH_EDITORONLY_DATA
				if (!AddEntries(Blackboard->ParentKeys, TEXT("inherited"), false))
				{
					return false;
				}
#endif
			}
			else if (Profile.GetCapabilityId()
				== TEXT("ai.eqs.graph.apply"))
			{
				UEnvQuery* const Query = Cast<UEnvQuery>(Target);
				if (!Query)
				{
					OutError = NativeTypeFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
						TEXT("The native target is not an instance of the required asset class."),
						TargetRef, TEXT("target.class"), Target, UEnvQuery::StaticClass());
					return false;
				}
#if WITH_EDITORONLY_DATA
				UeAgentEqsAuthoring::FGraphCapture Capture;
				UE::UAI::Contract::FDiagnostic CaptureError;
				if (!UeAgentEqsAuthoring::CaptureGraph(*Query, Capture, CaptureError))
				{
					SetError(OutError, CaptureError, TargetRef);
					return false;
				}
				{
					for (const auto& CapturedNode : Capture.Nodes)
					{
						const UEnvironmentQueryGraphNode_Option* const OptionNode =
							Cast<UEnvironmentQueryGraphNode_Option>(CapturedNode.NativeNode);
						const UEnvQueryOption* const Option = OptionNode
							? Cast<UEnvQueryOption>(OptionNode->NodeInstance.Get())
							: nullptr;
						if (!OptionNode || !OptionNode->NodeGuid.IsValid()
							|| !Option || !Option->Generator)
						{
							continue;
						}
						const FString& SemanticId = CapturedNode.Id;
						const TSharedRef<FJsonObject> Definition =
							MakeShared<FJsonObject>();
						Definition->SetStringField(
							TEXT("carrier_type_ref"),
							OptionNode->GetClass()->GetPathName());
						Definition->SetStringField(
							TEXT("payload_type_ref"),
							Option->Generator->GetClass()->GetPathName());
						FUeAgentCanonicalValuePtr Value;
						if (!TryCanonicalObject(
								Definition, Resources, Value, OutError))
						{
							return false;
						}
						OutValues.Add({SemanticId, Value, true});

						const TSharedRef<FJsonObject> Property =
							MakeShared<FJsonObject>();
						Property->SetStringField(
							TEXT("property_path"), TEXT("NodePosX"));
						Property->SetStringField(
							TEXT("value_type"), TEXT("int32"));
						Property->SetNumberField(
							TEXT("value"), OptionNode->NodePosX);
						if (!TryCanonicalObject(
								Property, Resources, Value, OutError))
						{
							return false;
						}
						OutValues.Add({
							TEXT("property:") + SemanticId + TEXT(":NodePosX"),
							Value, true});
					}
				}
#endif
			}
			else if (Profile.GetCapabilityId()
				== TEXT("ai.state_tree.hierarchy.apply"))
			{
#if WITH_EDITORONLY_DATA
				UStateTree* const Tree = Cast<UStateTree>(Target);
				UStateTreeEditorData* const EditorData = Tree
					? Cast<UStateTreeEditorData>(Tree->EditorData) : nullptr;
				TMap<FGuid, UStateTreeState*> States;
				TMap<FGuid, FString> Parents;
				if (!Tree || !EditorData)
				{
					OutError = !Tree
						? NativeTypeFailure(TEXT("graph_compound_state_tree_hierarchy_invalid"),
							TEXT("The native hierarchy target is not a StateTree."),
							TargetRef, TEXT("target.class"), Target, UStateTree::StaticClass())
						: NativeTypeFailure(TEXT("graph_compound_state_tree_hierarchy_invalid"),
							TEXT("The StateTree has no valid StateTree editor data."),
							TargetRef, TEXT("editor_data.class"), Tree->EditorData, UStateTreeEditorData::StaticClass());
					return false;
				}
				if (!TryCollectStateTreeStates(*EditorData, States, Parents, OutError))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
					return false;
				}
				auto AddStateTreeNodes = [&] (
					const TArray<FStateTreeEditorNode>& Nodes,
					const FString& OwnerState,
					const FString& Placement) -> bool
				{
					for (const FStateTreeEditorNode& Node : Nodes)
					{
						if (!Node.ID.IsValid())
						{
							continue;
						}
						const FString TypeRef = Node.InstanceObject
							? Node.InstanceObject->GetClass()->GetPathName()
							: (Node.Node.GetScriptStruct()
								? Node.Node.GetScriptStruct()->GetPathName()
								: FString());
						if (TypeRef.IsEmpty())
						{
							continue;
						}
						const TSharedRef<FJsonObject> Definition =
							MakeShared<FJsonObject>();
						Definition->SetStringField(
							TEXT("owner_state_ref"), OwnerState);
						Definition->SetStringField(
							TEXT("node_class_ref"), TypeRef);
						Definition->SetStringField(TEXT("placement"), Placement);
						FUeAgentCanonicalValuePtr Value;
						if (!TryCanonicalObject(
								Definition, Resources, Value, OutError))
						{
							return false;
						}
						OutValues.Add({
							TEXT("node:") + Node.ID.ToString(
								EGuidFormats::DigitsWithHyphensLower),
							Value,
							true});
					}
					return true;
				};
				if (!AddStateTreeNodes(
						EditorData->GlobalTasks, TEXT("global"), TEXT("global_task"))
					|| !AddStateTreeNodes(
						EditorData->Evaluators, TEXT("global"), TEXT("evaluator")))
				{
					return false;
				}
				for (const TPair<FGuid, UStateTreeState*>& Pair : States)
				{
					UStateTreeState* const State = Pair.Value;
					const FString StateIdentity =
						StateTreeStateSemanticId(Pair.Key);
					const TSharedRef<FJsonObject> Hierarchy =
						MakeShared<FJsonObject>();
					Hierarchy->SetStringField(TEXT("native_guid"),
						Pair.Key.ToString(EGuidFormats::DigitsWithHyphensLower));
					Hierarchy->SetStringField(TEXT("parent_identity"),
						Parents.FindChecked(Pair.Key));
					Hierarchy->SetStringField(TEXT("payload_type_ref"),
						State->GetClass()->GetPathName());
					FUeAgentCanonicalValuePtr Value;
					if (!TryCanonicalObject(Hierarchy, Resources, Value, OutError))
					{
						return false;
					}
					OutValues.Add({StateIdentity, Value, true});

					const TSharedRef<FJsonObject> Property =
						MakeShared<FJsonObject>();
					Property->SetStringField(TEXT("property_path"), TEXT("Name"));
					Property->SetStringField(TEXT("value_type"), TEXT("name"));
					Property->SetStringField(TEXT("value"), State->Name.ToString());
					if (!TryCanonicalObject(Property, Resources, Value, OutError))
					{
						return false;
					}
					OutValues.Add({TEXT("property:") + StateIdentity
						+ TEXT(":Name"), Value, true});
					if (!AddStateTreeNodes(
							State->Tasks, StateIdentity, TEXT("state_task"))
						|| !AddStateTreeNodes(
							State->EnterConditions,
							StateIdentity,
							TEXT("state_condition")))
					{
						return false;
					}

					if (!State->Children.IsEmpty())
					{
						TArray<TSharedPtr<FJsonValue>> OrderedIdentities;
						OrderedIdentities.Reserve(State->Children.Num());
						for (const UStateTreeState* Child : State->Children)
						{
							if (!Child || !Child->ID.IsValid())
							{
								OutError = NativeFailure(TEXT("graph_compound_state_tree_hierarchy_invalid"),
									!Child ? TEXT("The StateTree child entry is null.")
										: TEXT("The StateTree child has an invalid identity."),
									TargetRef, FString::Printf(TEXT("%s.children[%d].id"), *StateIdentity, OrderedIdentities.Num()),
									MakeShared<FJsonValueString>(Child ? Child->ID.ToString() : FString()),
									MakeShared<FJsonValueString>(TEXT("child state with a valid GUID")));
								OutError.SubjectId = StateIdentity;
								return false;
							}
							OrderedIdentities.Add(MakeShared<FJsonValueString>(
								StateTreeStateSemanticId(Child->ID)));
						}
						const TSharedRef<FJsonObject> Order =
							MakeShared<FJsonObject>();
						Order->SetStringField(TEXT("owner_identity"), StateIdentity);
						Order->SetArrayField(TEXT("ordered_identities"),
							MoveTemp(OrderedIdentities));
						if (!TryCanonicalObject(Order, Resources, Value, OutError))
						{
							return false;
						}
						OutValues.Add({TEXT("order:") + StateIdentity
							+ TEXT(":children"), Value, true});
					}
				}
#else
				OutError = NativeFailure(TEXT("graph_compound_state_tree_editor_data_unavailable"),
					TEXT("StateTree hierarchy capture requires a build with editor-only data."),
					TargetRef, TEXT("editor_only_data.available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
#endif
			}
			else if (Profile.GetCapabilityId()
				== TEXT("ai.smart_object_definition.apply"))
			{
				USmartObjectDefinition* const Definition =
					Cast<USmartObjectDefinition>(Target);
				if (!Definition)
				{
					OutError = NativeTypeFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
						TEXT("The native target is not an instance of the required asset class."),
						TargetRef, TEXT("target.class"), Target, USmartObjectDefinition::StaticClass());
					return false;
				}
				TSharedPtr<FJsonValue> Document; UE::UAI::Contract::FDiagnostic CaptureError;
				if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, Document, CaptureError))
				{ SetError(OutError, CaptureError, TargetRef); return false; }
				FUeAgentCanonicalValuePtr FullDocument;
				if (!TryCanonicalObject(Document->AsObject().ToSharedRef(), Resources, FullDocument, OutError)) return false;
				// The internal slot operation retains its compatibility value shape,
				// while preflight revision covers the complete canonical document.
				OutValues.Add({TEXT("smart_object:document"), FullDocument, true});
				const auto Items = Document->AsObject()->GetObjectField(TEXT("items"));
				for (const auto& Pair : Items->Values)
				{
					if (Pair.Value->AsObject()->GetStringField(TEXT("kind")) != TEXT("slot")) continue;
					const TSharedPtr<FJsonObject>* Behavior = nullptr;
					const FString BehaviorId = TEXT("behavior:") + Pair.Key + TEXT(":0");
					const auto ValueObject = MakeShared<FJsonObject>();
					ValueObject->SetStringField(TEXT("behavior_type_ref"), Items->TryGetObjectField(BehaviorId, Behavior)
						&& !(*Behavior)->GetStringField(TEXT("class_path")).IsEmpty() ? (*Behavior)->GetStringField(TEXT("class_path")) : TEXT("none"));
					ValueObject->SetStringField(TEXT("data_type_ref"), FSmartObjectSlotDefinition::StaticStruct()->GetPathName());
					FUeAgentCanonicalValuePtr Value;
					if (!TryCanonicalObject(ValueObject, Resources, Value, OutError)) return false;
					OutValues.Add({TEXT("item:slot:") + Pair.Key, Value, true});
				}
			}
			else if (IsPcgComponentMutationProfile(Profile))
			{
				UPCGComponent* const Component = Cast<UPCGComponent>(Target);
				AActor* const Owner = Component ? Component->GetOwner() : nullptr;
				UWorld* const World = Component ? Component->GetWorld() : nullptr;
				UPCGGraphInstance* const Binding = Component
					? Component->GetGraphInstance() : nullptr;
				UPCGGraphInterface* const BoundGraph = Binding
					? Binding->Graph.Get() : nullptr;
				FString PublicPolicy;
				if (!Component)
				{
					OutError = NativeTypeFailure(TEXT("graph_compound_pcg_component_world_invalid"),
						TEXT("The native target is not a PCG component."),
						TargetRef, TEXT("target.class"), Target, UPCGComponent::StaticClass());
					return false;
				}
				if (!Owner || !World)
				{
					OutError = NativeFailure(TEXT("graph_compound_pcg_component_world_invalid"),
						!Owner ? TEXT("The PCG component has no owning actor.") : TEXT("The PCG component has no world."),
						TargetRef, !Owner ? TEXT("owner.present") : TEXT("world.present"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				if (Owner->GetWorld() != World)
				{
					OutError = NativeFailure(TEXT("graph_compound_pcg_component_world_invalid"),
						TEXT("The PCG component and its owner belong to different worlds."),
						TargetRef, TEXT("owner.world"),
						MakeShared<FJsonValueString>(Owner->GetWorld() ? Owner->GetWorld()->GetPathName() : FString()),
						MakeShared<FJsonValueString>(World->GetPathName()));
					return false;
				}
				if (!GEngine || !GEngine->GetWorldContextFromWorld(World))
				{
					OutError = NativeFailure(TEXT("graph_compound_pcg_component_world_invalid"),
						!GEngine ? TEXT("The engine is unavailable for PCG world lookup.")
							: TEXT("The PCG component world has no registered world context."),
						TargetRef, !GEngine ? TEXT("engine.present") : TEXT("world.context_present"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				if (!TryPublicPcgGenerationPolicy(Component->GenerationTrigger, PublicPolicy))
				{
					OutError = NativeFailure(TEXT("graph_compound_pcg_component_world_invalid"),
						TEXT("The PCG component generation trigger has no supported public policy."),
						TargetRef, TEXT("generation_trigger"),
						MakeShared<FJsonValueNumber>(static_cast<int32>(Component->GenerationTrigger)),
						MakeShared<FJsonValueString>(TEXT("supported PCG generation policy")));
					return false;
				}
				const TSharedRef<FJsonObject> BindingValue =
					MakeShared<FJsonObject>();
				BindingValue->SetStringField(
					TEXT("actor_ref"), Owner->GetPathName());
				BindingValue->SetStringField(
					TEXT("component_ref"), Component->GetPathName());
				BindingValue->SetStringField(
					TEXT("generation_policy"), PublicPolicy);
				BindingValue->SetStringField(TEXT("graph_ref"),
					BoundGraph ? BoundGraph->GetPathName() : TEXT("none"));
				FUeAgentCanonicalValuePtr Value;
				if (!TryCanonicalObject(
						BindingValue, Resources, Value, OutError))
				{
					return false;
				}
				OutValues.Add({
					TEXT("binding:pcg_component_graph"), Value, true});
			}
			else if (Profile.GetCapabilityId()
				== TEXT("pcg.data_asset.apply"))
			{
				UPCGDataAsset* const DataAsset = Cast<UPCGDataAsset>(Target);
				FString SemanticId;
				FUeAgentCanonicalValuePtr Value;
				if (!DataAsset)
				{
					OutError = NativeTypeFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
						TEXT("The native target is not a PCG data asset."),
						TargetRef, TEXT("target.class"), Target, UPCGDataAsset::StaticClass());
					return false;
				}
				if (!TryCapturePcgDataAssetValue(*DataAsset, Resources, SemanticId, Value, OutError))
					return false;
				OutValues.Add({MoveTemp(SemanticId), Value, true});
			}
			else if (Profile.GetCapabilityId() == TEXT("pcg.graph.apply")
				|| Profile.GetCapabilityId()
					== TEXT("pcg.graph_parameters.apply"))
			{
				UPCGGraph* const Graph = Cast<UPCGGraph>(Target);
				if (!Graph)
				{
					OutError = NativeTypeFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
						TEXT("The native target is not an instance of the required asset class."),
						TargetRef, TEXT("target.class"), Target, UPCGGraph::StaticClass());
					return false;
				}
				if (Profile.GetCapabilityId() == TEXT("pcg.graph.apply"))
				{
					for (const UPCGNode* Node : Graph->GetNodes())
					{
						if (!Node)
						{
							continue;
						}
						const FString SemanticId = Node->HasAuthoredTitle()
							? TEXT("pcg_node:")
								+ Node->GetAuthoredTitleName().ToString()
							: TEXT("pcg.node:") + Node->GetPathName(Graph);
						const TSharedRef<FJsonObject> Definition =
							MakeShared<FJsonObject>();
						Definition->SetStringField(TEXT("carrier_type_ref"),
							Node->GetClass()->GetPathName());
						const UPCGSettings* const Settings = Node->GetSettings();
						Definition->SetStringField(TEXT("settings_type_ref"),
							Settings ? Settings->GetClass()->GetPathName()
								: TEXT("none"));
						FUeAgentCanonicalValuePtr Value;
						if (!TryCanonicalObject(
								Definition, Resources, Value, OutError))
						{
							return false;
						}
						OutValues.Add({SemanticId, Value, true});
					}
				}
				const FInstancedPropertyBag* const Bag =
					Graph->GetUserParametersStruct();
				const UPropertyBag* const BagStruct = Bag
					? Bag->GetPropertyBagStruct() : nullptr;
				if (Bag && BagStruct)
				{
					for (const FPropertyBagPropertyDesc& Desc
						: BagStruct->GetPropertyDescs())
					{
						if (!Desc.ID.IsValid() || Desc.Name.IsNone()
							|| Desc.ValueType
								!= EPropertyBagPropertyType::Double)
						{
							continue;
						}
						const TValueOrError<double, EPropertyBagResult> Readback =
							Bag->GetValueDouble(Desc.Name);
						FUeAgentCanonicalValuePtr Value;
						if (!Readback.IsValid())
						{
							OutError = NativeFailure(TEXT("graph_compound_pcg_parameter_readback_failed"),
								TEXT("The native double parameter could not be read from the property bag."),
								TargetRef, Desc.Name.ToString(),
								MakeShared<FJsonValueNumber>(static_cast<int32>(Readback.GetError())),
								MakeShared<FJsonValueString>(TEXT("successful double readback")));
							OutError.SubjectId = PcgParameterSemanticId(Desc);
							return false;
						}
						if (!TryMakeCanonicalDouble(Readback.GetValue(), Resources, Value, OutError))
						{
							if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
							OutError.CandidateName = Desc.Name.ToString();
							OutError.SubjectId = PcgParameterSemanticId(Desc);
							return false;
						}
						OutValues.Add({PcgParameterSemanticId(Desc), Value, true});
					}
				}
			}
			else if (Profile.GetCapabilityId()
				== TEXT("pcg.graph_instance.apply"))
			{
				UPCGGraphInstance* const Instance = Cast<UPCGGraphInstance>(Target);
				if (!Instance || !Instance->Graph)
				{
					OutError = !Instance
						? NativeTypeFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
							TEXT("The native target is not a PCG graph instance."),
							TargetRef, TEXT("target.class"), Target, UPCGGraphInstance::StaticClass())
						: NativeFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
							TEXT("The PCG graph instance has no bound graph."),
							TargetRef, TEXT("graph.present"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				FUeAgentCanonicalValuePtr BindingValue;
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryString(
						Instance->Graph->GetPathName(), Resources,
						BindingValue, StateError))
				{
					SetError(OutError, StateError, TargetRef);
					return false;
				}
				OutValues.Add({TEXT("binding:graph_source"), BindingValue, true});
				const FInstancedPropertyBag* const Bag =
					Instance->GetUserParametersStruct();
				if (Bag)
				{
					const UPropertyBag* const BagStruct =
						Instance->ParametersOverrides.Parameters
							.GetPropertyBagStruct();
					for (const FPropertyBagPropertyDesc& Desc
						: BagStruct->GetPropertyDescs())
					{
						if (!Desc.ID.IsValid() || Desc.Name.IsNone()
							|| Desc.ValueType
								!= EPropertyBagPropertyType::Double)
						{
							continue;
						}
						const TValueOrError<double, EPropertyBagResult> Readback =
							Bag->GetValueDouble(Desc.Name);
						FUeAgentCanonicalValuePtr Value;
						if (!Readback.IsValid())
						{
							OutError = NativeFailure(TEXT("graph_compound_pcg_parameter_readback_failed"),
								TEXT("The native double parameter could not be read from the property bag."),
								TargetRef, Desc.Name.ToString(),
								MakeShared<FJsonValueNumber>(static_cast<int32>(Readback.GetError())),
								MakeShared<FJsonValueString>(TEXT("successful double readback")));
							OutError.SubjectId = PcgParameterSemanticId(Desc);
							return false;
						}
						if (!TryMakeCanonicalDouble(Readback.GetValue(), Resources, Value, OutError))
						{
							if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
							OutError.CandidateName = Desc.Name.ToString();
							OutError.SubjectId = PcgParameterSemanticId(Desc);
							return false;
						}
						const FProperty* const Property = BagStruct
							? BagStruct->FindPropertyByName(Desc.Name) : nullptr;
						const bool bOverridden = Property
							&& Instance->ParametersOverrides
								.IsPropertyOverridden(Property);
						OutValues.Add({
							PcgParameterSemanticId(Desc), Value, bOverridden});
					}
				}
			}
			else
			{
				OutError = NativeFailure(TEXT("graph_compound_production_binding_unimplemented"),
					TEXT("No native PCG/AI mutation capture supports this capability."),
					TargetRef, TEXT("capability_id"),
					MakeShared<FJsonValueString>(Profile.GetCapabilityId()),
					MakeShared<FJsonValueString>(TEXT("supported PCG/AI mutation capability")));
				return false;
			}

			TArray<FString> RevisionLines;
			for (const FCapturedMutationValue& Captured : OutValues)
			{
				TSharedPtr<FJsonValue> JsonValue;
				FString Canonical;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!CanonicalJson::TryToJsonValue(
						*Captured.Value, JsonValue, JsonError)
					|| !Json::TryCanonicalize(JsonValue, Canonical, JsonError))
				{
					SetError(OutError, JsonError, Captured.SemanticId);
					return false;
				}
				RevisionLines.Add(Captured.SemanticId + TEXT("|") + Canonical);
			}
			RevisionLines.Sort();
			OutGeneration = TEXT("md5:")
				+ FMD5::HashAnsiString(*FString::Join(RevisionLines, TEXT("\n")));
			return true;
		}

		bool TryCaptureMutationSnapshot(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FGraphCompoundPcgAiTargetRequest& Request,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError)
		{
			ON_SCOPE_EXIT { MarkGraphCompoundNativeDiagnostic(OutError); };
			TArray<FCapturedMutationValue> Values;
			FString Generation;
			if (!TryCaptureMutationValues(
					Profile, Request.GetTargetRef(), Resources,
					Values, Generation, OutError))
			{
				return false;
			}
			TArray<FUeAgentStateEntry> Entries;
			for (const FCapturedMutationValue& Captured : Values)
			{
				FUeAgentStateEntry Entry(
					FUeAgentCanonicalIdentity(
						Captured.SemanticId, Request.GetTargetKind(),
						Request.GetTargetRef(), Captured.SemanticId,
						TEXT("graph_compound"), Generation),
					FUeAgentCanonicalType(
						TEXT("uai.graph_compound.state.v1"),
						Captured.Value->GetKind(), false,
						Captured.Value->GetKind() == EUeAgentValueKind::Object
							? TEXT("object")
							: Captured.Value->GetKind() == EUeAgentValueKind::Array
								? TEXT("array") : TEXT("scalar")),
					Captured.Value.ToSharedRef(),
					FUeAgentDefaultValue(
						EUeAgentDefaultSourceKind::DomainNative,
						Request.GetTargetRef(),
						TEXT("independent pcg-ai readback"),
						Captured.Value.ToSharedRef()),
					FUeAgentStateDetails(
						Captured.SemanticId,
						TEXT("GraphCompound independent current state"),
						{TEXT("GraphCompound")}, true, TEXT("available"), {}),
					FUeAgentStateRevision(
						TEXT("graph-compound:c4:pcg-ai-mutation"),
						Generation, 0),
					Captured.bLocal
						? EUeAgentAuthoredPresence::Present
						: EUeAgentAuthoredPresence::Absent,
					Captured.bLocal
						? EUeAgentOverrideState::Overridden
						: EUeAgentOverrideState::Inherited);
				if (!Entry.IsValid())
				{
					OutError = Entry.GetValidationDiagnostic();
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetTargetRef();
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Captured.SemanticId;
					return false;
				}
				Entries.Add(MoveTemp(Entry));
			}
			FUeAgentStateSemanticError StateError;
			const bool bSessionComponent =
				IsPcgComponentMutationProfile(Profile);
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("graph-compound:c4:pcg-ai-mutation:")
						+ Request.GetTargetRef(),
					bSessionComponent
						? EUeAgentStatePlane::RuntimeInstance
						: EUeAgentStatePlane::Persisted,
					FUeAgentStateRevision(
						TEXT("graph-compound:c4:pcg-ai-mutation"),
						Generation, 0),
					FUeAgentTemporalPoint(
						TEXT("utc-seconds"), FString::Printf(TEXT("%lld"),
							FDateTime::UtcNow().ToUnixTimestamp())),
					FUeAgentStateProvenance(
						TEXT("independent_readback"), Request.GetTargetRef(),
						(bSessionComponent
							? TEXT("loaded-session-state:")
							: TEXT("reloadable-native-state:")) + Generation),
					MoveTemp(Entries), Resources, OutSnapshot, StateError))
			{
				OutError = StateError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
				return false;
			}
			if (!OutSnapshot.IsValid())
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_ai_snapshot_invalid"),
					TEXT("PCG/AI mutation snapshot construction returned without a snapshot."),
					Request.GetTargetRef(), TEXT("snapshot.present"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			return true;
		}

		bool TryRollbackCreate(
			const FString& TargetRef,
			const bool bNotifyAssetRegistry,
			const FAssetDurableSessionRef& Session,
			FPackageAssetDurableBackend& Backend,
			const int64 FencingToken,
			FUeAgentResourceTracker& Resources,
			UE::UAI::Contract::FDiagnostic& OutDetail)
		{
			return UeAgentAuthoringOperationApply::TryRestorePackageDurability(
				TargetRef, bNotifyAssetRegistry, Session, Backend, FencingToken,
				Resources, OutDetail);
		}

		enum class EPcgAiSchemaMutationKind : uint8
		{
			BehaviorRoot,
			BehaviorComposite,
			BehaviorTask,
			BehaviorService,
			BehaviorDecorator,
			BehaviorBlackboardBinding,
			BehaviorRelation,
			BlackboardEntry,
			EqsOption,
			EqsProperty,
			StateTreeHierarchy,
			StateTreeOrder,
			StateTreeProperty,
			StateTreeNode,
			SmartObjectSlot,
			PcgGraphNode,
			PcgGraphBinding,
			PcgDoubleParameter,
			PcgComponentBinding,
			PcgDataAsset,
		};

		struct FPcgAiSchemaMutationOperation final
		{
			EPcgAiSchemaMutationKind Kind =
				EPcgAiSchemaMutationKind::BehaviorRoot;
			FString SemanticId;
			FUeAgentCanonicalValuePtr Value;
			FString InputPointer;
			FString TargetPath;
			TArray<FString> Dependencies;
			FGuid NodeGuid;
			UClass* PayloadClass = nullptr;
			UScriptStruct* PayloadStruct = nullptr;
			FString FromItem;
			FString OwnerItem;
			FString ObjectRef;
			int32 FromIndex = INDEX_NONE;
			FString ToItem;
			int32 ToIndex = INDEX_NONE;
			FName EntryName;
			UClass* KeyTypeClass = nullptr;
			int32 SignedIntegerValue = 0;
			FGuid StateGuid;
			FGuid ParentStateGuid;
			TArray<FGuid> OrderedStateGuids;
			FName StateName;
			FString Placement;
			FGuid SlotGuid;
			UClass* BehaviorClass = nullptr;
			UClass* SettingsClass = nullptr;
			UClass* CreatedNodeClass = nullptr;
			FString CandidateName;
			FString CandidateId;
			TMap<FString, FString> CandidateSelect;
			float PositionX = 0.0f;
			float PositionY = 0.0f;
			UPCGGraphInterface* GraphInterface = nullptr;
			FString ActorRef;
			FString PublicGenerationPolicy;
			FString NativeGenerationPolicy;
			EPCGComponentGenerationTrigger NativeGenerationTrigger =
				EPCGComponentGenerationTrigger::GenerateOnLoad;
			FGuid ParameterGuid;
			FName ParameterName;
			double DoubleValue = 0.0;
			FString ContentRevision;
			FSoftObjectPath SourceRef;
			int32 PointCount = 0;
		};

		bool TryParsePcgDoubleParameterIdentity(
			const FString& SemanticId,
			FGuid& OutGuid,
			FName& OutName)
		{
			TArray<FString> Parts;
			SemanticId.ParseIntoArray(Parts, TEXT(":"), false);
			if (Parts.Num() != 5
				|| Parts[0] != TEXT("property")
				|| Parts[1] != TEXT("parameter")
				|| Parts[4] != TEXT("double")
				|| !FGuid::Parse(Parts[2], OutGuid)
				|| !OutGuid.IsValid())
			{
				return false;
			}
			OutName = FName(*Parts[3]);
			return !OutName.IsNone()
				&& SemanticId == FString::Printf(
					TEXT("property:parameter:%s:%s:double"),
					*OutGuid.ToString(EGuidFormats::DigitsWithHyphensLower),
					*OutName.ToString());
		}

		bool HasEntryName(
			const UBlackboardData& Blackboard,
			const FName EntryName)
		{
			if (Blackboard.Keys.ContainsByPredicate(
				[EntryName](const FBlackboardEntry& Entry)
				{
					return Entry.EntryName == EntryName;
				}))
			{
				return true;
			}
#if WITH_EDITORONLY_DATA
			return Blackboard.ParentKeys.ContainsByPredicate(
				[EntryName](const FBlackboardEntry& Entry)
				{
					return Entry.EntryName == EntryName;
				});
#else
			return false;
#endif
		}


		bool TryParseSchemaMutationOperations(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FGraphCompoundPcgAiOperationRequest& Request,
			const FPlanningPreflightPlanRef& Plan,
			UObject& Target,
			TArray<FPcgAiSchemaMutationOperation>& OutOperations,
			FGraphCompoundRuntimeError& OutError)
		{
			OutOperations.Reset();
			TOptional<int32> CurrentEditIndex;
			auto FailPlan = [&](const FString& Reason, const FString& Field,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
				const FString& SemanticId = FString())
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_ai_plan_invalid"),
					Reason, Request.GetTargetRef(), Field, MoveTemp(Actual), MoveTemp(Expected));
				OutError.SubjectId = SemanticId;
				OutError.OperationIndex = CurrentEditIndex;
				return false;
			};
			auto IntentValue = [](const EChangeIntent Intent) -> TSharedPtr<FJsonValue>
			{
				switch (Intent)
				{
				case EChangeIntent::Create: return MakeShared<FJsonValueString>(TEXT("create"));
				case EChangeIntent::Update: return MakeShared<FJsonValueString>(TEXT("update"));
				case EChangeIntent::Replace: return MakeShared<FJsonValueString>(TEXT("replace"));
				case EChangeIntent::Delete: return MakeShared<FJsonValueString>(TEXT("delete"));
				default: return MakeShared<FJsonValueNumber>(static_cast<int32>(Intent));
				}
			};
			const TArray<FGraphCompoundCanonicalEdit>& Edits =
				Request.GetPayload().GetEdits();
			const TArray<FPlannedChange>& Changes =
				Plan->GetChangeSet().GetChanges();
			const bool bPcgGraphInstanceProfile =
				Profile.GetCapabilityId()
					== TEXT("pcg.graph_instance.apply");
			const bool bPcgComponentProfile =
				IsPcgComponentMutationProfile(Profile);
			const FUeAgentDescriptorIdentity& ActualDescriptor = Plan->GetDescriptorIdentity();
			const FUeAgentDescriptorIdentity& ExpectedDescriptor =
				Profile.GetExecutionContract().GetDescriptorIdentity();
			if (ActualDescriptor != ExpectedDescriptor)
			{
				if (ActualDescriptor.GetDescriptorId() != ExpectedDescriptor.GetDescriptorId())
					return FailPlan(TEXT("The plan descriptor id does not match the route descriptor."),
						TEXT("descriptor_identity.descriptor_id"),
						MakeShared<FJsonValueString>(ActualDescriptor.GetDescriptorId()),
						MakeShared<FJsonValueString>(ExpectedDescriptor.GetDescriptorId()));
				if (ActualDescriptor.GetVersion() != ExpectedDescriptor.GetVersion())
					return FailPlan(TEXT("The plan descriptor version does not match the route descriptor."),
						TEXT("descriptor_identity.version"),
						MakeShared<FJsonValueNumber>(ActualDescriptor.GetVersion()),
						MakeShared<FJsonValueNumber>(ExpectedDescriptor.GetVersion()));
				FailPlan(TEXT("The plan descriptor hash does not match the route descriptor."),
					TEXT("descriptor_identity.descriptor_hash"), nullptr, nullptr);
				OutError.ActualEvidence = MakeShared<FJsonValueString>(ActualDescriptor.GetDescriptorHash());
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedDescriptor.GetDescriptorHash());
				return false;
			}
			if (Edits.IsEmpty())
				return FailPlan(TEXT("The mutation request contains no edits."),
					TEXT("edits.count"), MakeShared<FJsonValueNumber>(0),
					MakeShared<FJsonValueString>(TEXT("at least one edit")));
			if (!bPcgGraphInstanceProfile && Edits.Num() != Changes.Num())
				return FailPlan(TEXT("The planned change count differs from the requested edit count."),
					TEXT("changes.count"), MakeShared<FJsonValueNumber>(Changes.Num()),
					MakeShared<FJsonValueNumber>(Edits.Num()));
			if (bPcgGraphInstanceProfile && (Changes.IsEmpty() || Changes.Num() > Edits.Num()))
				return FailPlan(TEXT("The graph instance plan must contain between one change and the requested edit count."),
					TEXT("changes.count"), MakeShared<FJsonValueNumber>(Changes.Num()),
					MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
						MakeShared<FJsonValueNumber>(1), MakeShared<FJsonValueNumber>(Edits.Num())}));

			TMap<FString, const FPlannedChange*> PlannedById;
			TSet<FString> ProposedNodeIds;
			TMap<FString, FString> ProposedBehaviorNodeCarriers;
			TSet<FString> ProposedStateIds;
			TMap<FString, FString> ProposedStateParents;
			for (const FPlannedChange& Change : Changes)
			{
				const FString& ChangeId = Change.GetIdentity().GetSemanticId();
				if (Change.GetIdentity().GetOwnerId() != Request.GetTargetRef())
					return FailPlan(TEXT("The planned change belongs to a different target."),
						FString::Printf(TEXT("changes[%d].identity.owner_id"), PlannedById.Num()),
						MakeShared<FJsonValueString>(Change.GetIdentity().GetOwnerId()),
						MakeShared<FJsonValueString>(Request.GetTargetRef()), ChangeId);
				if (PlannedById.Contains(ChangeId))
					return FailPlan(TEXT("The plan contains a duplicate change semantic id."),
						FString::Printf(TEXT("changes[%d].identity.semantic_id"), PlannedById.Num()),
						MakeShared<FJsonValueString>(ChangeId),
						MakeShared<FJsonValueString>(TEXT("unique semantic id")), ChangeId);
				PlannedById.Add(
					Change.GetIdentity().GetSemanticId(), &Change);
			}
			for (const FGraphCompoundCanonicalEdit& Edit : Edits)
			{
				ON_SCOPE_EXIT { Request.GetPayload().LocateDiagnostic(OutError, Edit.GetIdentity().GetSemanticId()); };
				if (Edit.GetElementKind() == EGraphCompoundElementKind::Item)
				{
					bool bConnectableNode = true;
					if (Profile.GetCapabilityId()
						== TEXT("ai.behavior_tree.graph.apply"))
					{
						FString CarrierType;
						bConnectableNode = Edit.GetProposedAfter().IsValid()
							&& TryCanonicalStringField(
								*Edit.GetProposedAfter(),
								TEXT("carrier_type_ref"),
								CarrierType)
							&& CarrierType
								!= TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Service")
							&& CarrierType
								!= TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Decorator");
					}
					if (bConnectableNode)
					{
						const FString NodeId =
							Edit.GetIdentity().GetSemanticId();
						ProposedNodeIds.Add(NodeId);
						if (Profile.GetCapabilityId()
							== TEXT("ai.behavior_tree.graph.apply"))
						{
							FString CarrierType;
							if (Edit.GetProposedAfter().IsValid()
								&& TryCanonicalStringField(
									*Edit.GetProposedAfter(),
									TEXT("carrier_type_ref"),
									CarrierType))
							{
								ProposedBehaviorNodeCarriers.Add(
									NodeId, MoveTemp(CarrierType));
							}
						}
					}
				}
				else if (Edit.GetElementKind()
					== EGraphCompoundElementKind::Hierarchy)
				{
					const FString StateIdentity =
						Edit.GetIdentity().GetSemanticId();
					ProposedStateIds.Add(StateIdentity);
					FString ParentIdentity;
					if (Edit.GetProposedAfter().IsValid()
						&& TryCanonicalStringField(*Edit.GetProposedAfter(),
							TEXT("parent_identity"), ParentIdentity))
					{
						ProposedStateParents.Add(StateIdentity, ParentIdentity);
					}
				}
			}

			for (const FGraphCompoundCanonicalEdit& Edit : Edits)
			{
				ON_SCOPE_EXIT { Request.GetPayload().LocateDiagnostic(OutError, Edit.GetIdentity().GetSemanticId()); };
				const FString SemanticId = Edit.GetIdentity().GetSemanticId();
				CurrentEditIndex = static_cast<int32>(&Edit - Edits.GetData());
				const FPlannedChange* const* const Planned =
					PlannedById.Find(SemanticId);
				const bool bExactPcgBindingNoOp =
					bPcgGraphInstanceProfile && !Planned
					&& SemanticId == TEXT("binding:graph_source")
					&& Edit.GetElementKind()
						== EGraphCompoundElementKind::Binding
					&& Edit.GetProposedAfter().IsValid()
					&& Edit.GetProposedAfter()->GetKind()
						== EUeAgentValueKind::String
					&& Cast<UPCGGraphInstance>(&Target)
					&& Cast<UPCGGraphInstance>(&Target)->Graph
					&& Cast<UPCGGraphInstance>(&Target)->Graph->GetPathName()
						== Edit.GetProposedAfter()->GetText();
				const bool bPcgUpdateProfile =
					Profile.GetCapabilityId()
						== TEXT("pcg.graph_instance.apply")
					|| Profile.GetCapabilityId()
						== TEXT("pcg.graph_parameters.apply")
					|| bPcgComponentProfile;
				const bool bPcgDataAssetProfile =
					Profile.GetCapabilityId()
						== TEXT("pcg.data_asset.apply");
				const EChangeIntent RequiredIntent = bPcgDataAssetProfile
					? EChangeIntent::Replace
					: (bPcgUpdateProfile || (Profile.GetCapabilityId() == TEXT("ai.state_tree.hierarchy.apply")
						&& Edit.GetElementKind() == EGraphCompoundElementKind::Order)
						? EChangeIntent::Update : EChangeIntent::Create);
				const bool bBehaviorBindingEdit =
					Profile.GetCapabilityId()
						== TEXT("ai.behavior_tree.graph.apply")
					&& Edit.GetElementKind()
						== EGraphCompoundElementKind::Binding;
				const bool bIntentAllowed = bBehaviorBindingEdit
					? (Edit.GetIntent() == EChangeIntent::Create
						|| Edit.GetIntent() == EChangeIntent::Update
						|| Edit.GetIntent() == EChangeIntent::Replace)
					: Edit.GetIntent() == RequiredIntent;
				if (!Planned && !bExactPcgBindingNoOp)
					return FailPlan(TEXT("The requested edit has no corresponding planned change."),
						TEXT("planned_change.present"), MakeShared<FJsonValueBoolean>(false),
						MakeShared<FJsonValueBoolean>(true), SemanticId);
				if (Planned && !*Planned)
					return FailPlan(TEXT("The plan index contains a null change."),
						TEXT("planned_change.present"), MakeShared<FJsonValueBoolean>(false),
						MakeShared<FJsonValueBoolean>(true), SemanticId);
				if (Planned && (*Planned)->GetIntent() != Edit.GetIntent())
					return FailPlan(TEXT("The planned change intent differs from the requested edit intent."),
						TEXT("planned_change.intent"), IntentValue((*Planned)->GetIntent()),
						IntentValue(Edit.GetIntent()), SemanticId);
				if (!bIntentAllowed)
				{
					const TSharedPtr<FJsonValue> ExpectedIntent = bBehaviorBindingEdit
						? TSharedPtr<FJsonValue>(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
							IntentValue(EChangeIntent::Create), IntentValue(EChangeIntent::Update),
							IntentValue(EChangeIntent::Replace)}))
						: IntentValue(RequiredIntent);
					return FailPlan(TEXT("The edit intent is not supported for this mutation kind."),
						TEXT("edit.intent"), IntentValue(Edit.GetIntent()), ExpectedIntent, SemanticId);
				}
				if (Edit.GetIdentity().GetOwnerId() != Request.GetTargetRef())
					return FailPlan(TEXT("The edit belongs to a different target."),
						TEXT("edit.identity.owner_id"),
						MakeShared<FJsonValueString>(Edit.GetIdentity().GetOwnerId()),
						MakeShared<FJsonValueString>(Request.GetTargetRef()), SemanticId);
				if (!Edit.GetProposedAfter().IsValid())
					return FailPlan(TEXT("The mutation edit has no proposed value."),
						TEXT("edit.proposed_value_present"), MakeShared<FJsonValueBoolean>(false),
						MakeShared<FJsonValueBoolean>(true), SemanticId);

				FPcgAiSchemaMutationOperation Operation;
				Operation.SemanticId = SemanticId;
				Operation.Value = Edit.GetProposedAfter();
				Operation.TargetPath = Request.GetTargetRef();
				if (const FString* Pointer = Request.GetPayload().FindInputPointer(SemanticId)) Operation.InputPointer = *Pointer;
				for (const FUeAgentCanonicalIdentity& Dependency
					: Edit.GetDependencies())
				{
					if (Dependency.GetOwnerId() != Request.GetTargetRef())
					{
						OutError = NativeFailure(TEXT("graph_compound_pcg_ai_dependency_invalid"),
							TEXT("The edit dependency belongs to a different target."),
							Request.GetTargetRef(),
							FString::Printf(TEXT("dependencies[%d].identity.owner_id"), Operation.Dependencies.Num()),
							MakeShared<FJsonValueString>(Dependency.GetOwnerId()),
							MakeShared<FJsonValueString>(Request.GetTargetRef()));
						OutError.SubjectId = SemanticId;
						OutError.OperationIndex = CurrentEditIndex;
						OutError.CandidateName = Dependency.GetSemanticId();
						return false;
					}
					Operation.Dependencies.Add(Dependency.GetSemanticId());
				}

				auto ValueInput = [&](const TCHAR* Code)
				{
					return FRequiredCanonicalInput(Code, Request.GetTargetRef(), SemanticId,
						Operation.InputPointer, bPcgComponentProfile, OutError, CurrentEditIndex);
				};
				auto FailOperation = [&](const TCHAR* Code, const FString& Reason,
					const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
				{
					OutError = NativeFailure(Code, Reason, Request.GetTargetRef(), Field, MoveTemp(Actual), MoveTemp(Expected));
					OutError.SubjectId = SemanticId;
					OutError.OperationIndex = CurrentEditIndex;
					return false;
				};
				if (Profile.GetCapabilityId()
					== TEXT("ai.behavior_tree.graph.apply"))
				{
					if (!Cast<UBehaviorTree>(&Target))
					{
						OutError = NativeTypeFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
							TEXT("The native mutation target is not a BehaviorTree."),
							Request.GetTargetRef(), TEXT("target.class"), &Target, UBehaviorTree::StaticClass());
						OutError.OperationIndex = CurrentEditIndex;
						return false;
					}
					const FUeAgentCanonicalValue& Value = *Operation.Value;
					if (Edit.GetElementKind()
						== EGraphCompoundElementKind::Item)
					{
						FString CarrierType;
						if (SemanticId == TEXT("node:root"))
						{
							const auto Input = ValueInput(TEXT("graph_compound_behavior_tree_root_invalid"));
							if (!Operation.Dependencies.IsEmpty())
								return FailOperation(TEXT("graph_compound_behavior_tree_root_invalid"),
									TEXT("The BehaviorTree root must not declare dependencies."),
									TEXT("dependencies.count"), MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()),
									MakeShared<FJsonValueNumber>(0));
							if (!Input.RequireFields(Value, {TEXT("carrier_type_ref")})
								|| !Input.ReadString(Value, TEXT("carrier_type_ref"), CarrierType)) return false;
							if (CarrierType != TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Root"))
								return Input.Fail(TEXT("The BehaviorTree root requires the root graph-node carrier class."),
									TEXT("/carrier_type_ref"), MakeShared<FJsonValueString>(CarrierType),
									MakeShared<FJsonValueString>(TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Root")));
							Operation.Kind =
								EPcgAiSchemaMutationKind::BehaviorRoot;
						}
						else
						{
							FString PayloadType;
							const FUeAgentCanonicalValue* const Candidate =
								FindObjectValue(Value, TEXT("candidate"));
							FString CandidateName;
							FString CandidateId;
							const bool bHasCandidateName = Candidate
								&& TryCanonicalStringField(
									*Candidate, TEXT("name"), CandidateName);
							const bool bHasCandidateId = Candidate
								&& TryCanonicalStringField(
									*Candidate, TEXT("id"), CandidateId);
							const bool bNativeSelector = Candidate != nullptr;
							const auto Input = ValueInput(TEXT("graph_compound_behavior_tree_node_invalid"));
							if (!SemanticId.StartsWith(TEXT("node:"))
								|| !FGuid::Parse(SemanticId.RightChop(5), Operation.NodeGuid)
								|| !Operation.NodeGuid.IsValid()
								|| SemanticId != TEXT("node:") + Operation.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower))
								return FailOperation(TEXT("graph_compound_behavior_tree_node_invalid"),
									TEXT("The BehaviorTree node identity must contain a valid canonical GUID."),
									TEXT("identity.semantic_id"), MakeShared<FJsonValueString>(SemanticId),
									MakeShared<FJsonValueString>(TEXT("node:<lowercase hyphenated GUID>")));
							if (bNativeSelector)
							{
								if (!Input.RequireFields(Value,
										{TEXT("candidate"), TEXT("carrier_type_ref"), TEXT("payload_type_ref"), TEXT("position")}, false)
									|| !Input.At(TEXT("/candidate")).RequireFields(*Candidate,
										{TEXT("name"), TEXT("id"), TEXT("select")}, false)) return false;
								if (!bHasCandidateName && !bHasCandidateId)
								{
									const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
									if (const auto* Name = FindObjectValue(*Candidate, TEXT("name")))
										Actual->SetField(TEXT("name"), CanonicalInputFact(Name));
									if (const auto* Id = FindObjectValue(*Candidate, TEXT("id")))
										Actual->SetField(TEXT("id"), CanonicalInputFact(Id));
									return Input.Fail(TEXT("The candidate requires a non-empty string name or id."),
										TEXT("/candidate"), MakeShared<FJsonValueObject>(Actual),
										MakeShared<FJsonValueString>(TEXT("at least one non-empty string: name or id")));
								}
							}
							else if (!Input.RequireFields(Value, {TEXT("carrier_type_ref"), TEXT("payload_type_ref")})) return false;
							if (!Input.ReadString(Value, TEXT("carrier_type_ref"), CarrierType)
								|| !Input.ReadString(Value, TEXT("payload_type_ref"), PayloadType)) return false;
							Operation.PayloadClass =
								LoadObject<UClass>(nullptr, *PayloadType);
							UClass* RequiredBase = nullptr;
							if (CarrierType
								== TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Composite"))
							{
								RequiredBase = UBTCompositeNode::StaticClass();
								Operation.Kind =
									EPcgAiSchemaMutationKind::BehaviorComposite;
							}
							else if (CarrierType
								== TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Task"))
							{
								RequiredBase = UBTTaskNode::StaticClass();
								Operation.Kind =
									EPcgAiSchemaMutationKind::BehaviorTask;
							}
							else if (CarrierType
								== TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Service"))
							{
								RequiredBase = UBTService::StaticClass();
								Operation.Kind =
									EPcgAiSchemaMutationKind::BehaviorService;
							}
							else if (CarrierType
								== TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Decorator"))
							{
								RequiredBase = UBTDecorator::StaticClass();
								Operation.Kind =
									EPcgAiSchemaMutationKind::BehaviorDecorator;
							}
							const auto PayloadInput = ValueInput(TEXT("graph_compound_behavior_tree_payload_invalid"));
							if (!RequiredBase)
								return PayloadInput.Fail(TEXT("The BehaviorTree carrier class is not supported for a non-root node."),
									TEXT("/carrier_type_ref"), MakeShared<FJsonValueString>(CarrierType),
									MakeShared<FJsonValueString>(TEXT("BehaviorTreeGraphNode_Composite, Task, Service or Decorator")));
							if (!Operation.PayloadClass)
								return PayloadInput.Fail(TEXT("The BehaviorTree payload class could not be loaded."),
									TEXT("/payload_type_ref"), MakeShared<FJsonValueString>(PayloadType),
									MakeShared<FJsonValueString>(TEXT("loadable class derived from ") + RequiredBase->GetPathName()));
							if (!Operation.PayloadClass->IsChildOf(RequiredBase))
								return PayloadInput.Fail(TEXT("The BehaviorTree payload class does not derive from the required node base."),
									TEXT("/payload_type_ref"), ClassPathValue(Operation.PayloadClass), ClassPathValue(RequiredBase));
							if (Operation.PayloadClass->HasAnyClassFlags(CLASS_Abstract))
								return PayloadInput.Fail(TEXT("The BehaviorTree payload class is abstract and cannot be instantiated."),
									TEXT("/payload_type_ref"), ClassPathValue(Operation.PayloadClass),
									MakeShared<FJsonValueString>(TEXT("non-abstract class derived from ") + RequiredBase->GetPathName()));
							if (bNativeSelector)
							{
								TSharedPtr<FJsonValue> CandidateJson;
								UE::UAI::Contract::FDiagnostic JsonError;
								HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector Selector;
								HjsonWorkspace::FWorkspaceError SelectorError;
								if (!CanonicalJson::TryToJsonValue(*Candidate, CandidateJson, JsonError))
								{
									SetError(OutError, JsonError, SemanticId);
									if (!Operation.InputPointer.IsEmpty())
										UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError,
											(Operation.InputPointer == TEXT("/") ? FString() : Operation.InputPointer) + TEXT("/candidate"));
									if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetTargetRef();
									return false;
								}
								if (!CandidateJson.IsValid() || CandidateJson->Type != EJson::Object)
								{
									OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_behavior_tree_candidate_invalid"),
										TEXT("The Behavior Tree candidate selector must be an object."), TEXT("native_graph"));
									OutError.ActualValue = MakeShared<FJsonValueString>(!CandidateJson.IsValid() ? TEXT("missing")
										: CandidateJson->Type == EJson::Array ? TEXT("array") : CandidateJson->Type == EJson::String ? TEXT("string")
										: CandidateJson->Type == EJson::Number ? TEXT("number") : CandidateJson->Type == EJson::Boolean ? TEXT("boolean") : TEXT("null"));
									OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("candidate selector object"));
									OutError.Source.JsonPath = TEXT("/candidate");
									if (!Operation.InputPointer.IsEmpty())
										UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError,
											Operation.InputPointer == TEXT("/") ? FString() : Operation.InputPointer);
									OutError.TargetPath = Request.GetTargetRef();
									OutError.SubjectId = SemanticId;
									return false;
								}
								if (!HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(
										CandidateJson->AsObject(), TEXT("/candidate"), Selector, SelectorError))
								{
									SetError(OutError, SelectorError.GetDiagnostic(), SemanticId);
									if (!Operation.InputPointer.IsEmpty())
										UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError,
											Operation.InputPointer == TEXT("/") ? FString() : Operation.InputPointer);
									if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetTargetRef();
									return false;
								}
								Operation.CandidateSelect = MoveTemp(Selector.Select);
								QueryOwners::AiBehavior::FAiBehaviorCandidate
									ResolvedCandidate;
								QueryOwners::AiBehavior::FAiBehaviorCandidateQueryError
									CandidateError;
								const bool bCandidateResolved = QueryOwners::AiBehavior::TryResolveAiBehaviorCandidate(
										Target,
										CandidateName,
										CandidateId,
										ResolvedCandidate,
										CandidateError, Operation.CandidateSelect);
								if (!bCandidateResolved || !ResolvedCandidate.bSupported
									|| ResolvedCandidate.PayloadType != PayloadType
									|| ((Operation.Kind
											== EPcgAiSchemaMutationKind::BehaviorComposite)
										&& ResolvedCandidate.Kind
											!= TEXT("behavior_composite"))
									|| ((Operation.Kind
											== EPcgAiSchemaMutationKind::BehaviorTask)
										&& ResolvedCandidate.Kind
											!= TEXT("behavior_task")))
								{
									if (!bCandidateResolved) OutError = CandidateError;
									else if (!ResolvedCandidate.bSupported)
											OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("graph_compound_behavior_tree_candidate_invalid"), ResolvedCandidate.SupportDiagnostic);
									else
									{
										OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_behavior_tree_candidate_invalid"),
											ResolvedCandidate.PayloadType != PayloadType
												? TEXT("The resolved AI candidate payload class differs from the graph definition.")
												: TEXT("The resolved AI candidate kind differs from the requested Behavior Tree node kind."), TEXT("native_graph"));
										OutError.SourceKind = TEXT("native_result");
										auto Actual = MakeShared<FJsonObject>();
										Actual->SetStringField(TEXT("kind"), ResolvedCandidate.Kind);
										Actual->SetStringField(TEXT("payload_type"), FPackageName::ObjectPathToObjectName(ResolvedCandidate.PayloadType));
										auto Expected = MakeShared<FJsonObject>();
										Expected->SetStringField(TEXT("payload_type"), FPackageName::ObjectPathToObjectName(PayloadType));
										if (Operation.Kind == EPcgAiSchemaMutationKind::BehaviorComposite) Expected->SetStringField(TEXT("kind"), TEXT("behavior_composite"));
										else if (Operation.Kind == EPcgAiSchemaMutationKind::BehaviorTask) Expected->SetStringField(TEXT("kind"), TEXT("behavior_task"));
										OutError.ActualValue = MakeShared<FJsonValueObject>(Actual);
										OutError.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
										OutError.ActualEvidence = MakeShared<FJsonValueString>(ResolvedCandidate.PayloadType);
										OutError.ExpectedEvidence = MakeShared<FJsonValueString>(PayloadType);
									}
									if (OutError.IsEmpty())
									{
										OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_behavior_tree_candidate_invalid"),
											TEXT("The AI candidate resolver failed without providing a diagnostic."), TEXT("native_graph"));
										OutError.SourceKind = TEXT("native_result");
									}
									if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CandidateName;
									if (!Operation.InputPointer.IsEmpty())
										UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError,
											(Operation.InputPointer == TEXT("/") ? FString() : Operation.InputPointer) + TEXT("/candidate"));
									if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetTargetRef();
									if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SemanticId;
									return false;
								}
								Operation.CandidateName = CandidateName;
								Operation.CandidateId = CandidateId;
								if (FindObjectValue(Value, TEXT("position"))
									&& !ValueInput(TEXT("graph_compound_behavior_tree_position_invalid")).ReadVector2(
										Value, TEXT("position"), Operation.PositionX, Operation.PositionY)) return false;
							}
							const bool bSubNode =
								Operation.Kind == EPcgAiSchemaMutationKind::BehaviorService
								|| Operation.Kind == EPcgAiSchemaMutationKind::BehaviorDecorator;
							if ((!bSubNode && !Operation.Dependencies.IsEmpty())
								|| (bSubNode && Operation.Dependencies.Num() != 1))
								return FailOperation(TEXT("graph_compound_behavior_tree_placement_invalid"),
									TEXT("The BehaviorTree node has an incorrect dependency count for its carrier kind."),
									TEXT("dependencies.count"), MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()),
									MakeShared<FJsonValueNumber>(bSubNode ? 1 : 0));
							if (bSubNode && !ProposedNodeIds.Contains(Operation.Dependencies[0]))
								return FailOperation(TEXT("graph_compound_behavior_tree_placement_invalid"),
									TEXT("The BehaviorTree subnode owner is not a proposed connectable node."),
									TEXT("owner_item_ref"), MakeShared<FJsonValueString>(Operation.Dependencies[0]),
									MakeShared<FJsonValueString>(TEXT("proposed connectable node")));
							if (bSubNode)
							{
								Operation.OwnerItem = Operation.Dependencies[0];
							}
						}
					}
					else if (Edit.GetElementKind()
						== EGraphCompoundElementKind::Binding)
					{
						if (SemanticId != TEXT("binding:blackboard_asset"))
							return FailOperation(TEXT("graph_compound_behavior_tree_blackboard_binding_invalid"),
								TEXT("The BehaviorTree binding uses an unsupported semantic id."),
								TEXT("identity.semantic_id"), MakeShared<FJsonValueString>(SemanticId),
								MakeShared<FJsonValueString>(TEXT("binding:blackboard_asset")));
						if (!Operation.Dependencies.IsEmpty())
							return FailOperation(TEXT("graph_compound_behavior_tree_blackboard_binding_invalid"),
								TEXT("The BehaviorTree blackboard binding must not declare dependencies."),
								TEXT("dependencies.count"), MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()),
								MakeShared<FJsonValueNumber>(0));
						if (Value.GetKind() != EUeAgentValueKind::String || Value.GetText().IsEmpty())
							return ValueInput(TEXT("graph_compound_behavior_tree_blackboard_binding_invalid")).Fail(
								TEXT("The BehaviorTree blackboard binding value must be a non-empty string reference."),
								FString(), CanonicalInputFact(&Value), MakeShared<FJsonValueString>(TEXT("non-empty blackboard reference")));
						Operation.ObjectRef = Value.GetText();
						Operation.Kind =
							EPcgAiSchemaMutationKind::BehaviorBlackboardBinding;
					}
					else if (Edit.GetElementKind()
						== EGraphCompoundElementKind::Relation)
					{
						const auto Input = ValueInput(TEXT("graph_compound_behavior_tree_relation_invalid"));
						if (!Input.RequireFields(Value,
								{TEXT("from_item_ref"), TEXT("from_output_index"), TEXT("to_item_ref"), TEXT("to_input_index")})
							|| !Input.ReadString(Value, TEXT("from_item_ref"), Operation.FromItem)
							|| !Input.ReadInteger(Value, TEXT("from_output_index"), Operation.FromIndex)
							|| !Input.ReadString(Value, TEXT("to_item_ref"), Operation.ToItem)
							|| !Input.ReadInteger(Value, TEXT("to_input_index"), Operation.ToIndex)) return false;
						const FString ExpectedRelationId = BehaviorTreeRelationSemanticId(
							Operation.FromItem, Operation.FromIndex, Operation.ToItem, Operation.ToIndex);
						if (SemanticId != ExpectedRelationId)
							return FailOperation(TEXT("graph_compound_behavior_tree_relation_invalid"),
								TEXT("The BehaviorTree relation identity differs from its endpoints."),
								TEXT("identity.semantic_id"), MakeShared<FJsonValueString>(SemanticId),
								MakeShared<FJsonValueString>(ExpectedRelationId));
						if (Operation.Dependencies.Num() != 2)
							return FailOperation(TEXT("graph_compound_behavior_tree_relation_invalid"),
								TEXT("The BehaviorTree relation requires two dependencies."),
								TEXT("dependencies.count"), MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()),
								MakeShared<FJsonValueNumber>(2));
						if (!Operation.Dependencies.Contains(Operation.FromItem))
							return FailOperation(TEXT("graph_compound_behavior_tree_relation_invalid"),
								TEXT("The BehaviorTree relation source is missing from its dependencies."),
								TEXT("dependencies.from_item_ref"), MakeShared<FJsonValueString>(Operation.FromItem),
								MakeShared<FJsonValueString>(TEXT("declared dependency")));
						if (!Operation.Dependencies.Contains(Operation.ToItem))
							return FailOperation(TEXT("graph_compound_behavior_tree_relation_invalid"),
								TEXT("The BehaviorTree relation destination is missing from its dependencies."),
								TEXT("dependencies.to_item_ref"), MakeShared<FJsonValueString>(Operation.ToItem),
								MakeShared<FJsonValueString>(TEXT("declared dependency")));
						if (!ProposedNodeIds.Contains(Operation.FromItem))
							return Input.Fail(TEXT("The BehaviorTree relation source is not a proposed connectable node."),
								TEXT("/from_item_ref"), MakeShared<FJsonValueString>(Operation.FromItem),
								MakeShared<FJsonValueString>(TEXT("proposed connectable node")));
						if (!ProposedNodeIds.Contains(Operation.ToItem))
							return Input.Fail(TEXT("The BehaviorTree relation destination is not a proposed connectable node."),
								TEXT("/to_item_ref"), MakeShared<FJsonValueString>(Operation.ToItem),
								MakeShared<FJsonValueString>(TEXT("proposed connectable node")));
						if (Operation.FromItem == TEXT("node:root")
							&& ProposedBehaviorNodeCarriers.FindRef(
								Operation.ToItem)
								!= TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Composite"))
						{
							FailOperation(TEXT("graph_compound_behavior_tree_root_target_invalid"),
								TEXT("The BehaviorTree root output requires a composite target node."),
								TEXT("target_node.carrier_type_ref"),
								MakeShared<FJsonValueString>(ProposedBehaviorNodeCarriers.FindRef(Operation.ToItem)),
								MakeShared<FJsonValueString>(TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Composite")));
							OutError.CandidateName = Operation.ToItem;
							return false;
						}
						Operation.Kind =
							EPcgAiSchemaMutationKind::BehaviorRelation;
					}
					else
					{
						return FailOperation(TEXT("graph_compound_behavior_tree_edit_kind_invalid"),
							TEXT("BehaviorTree mutations support only item, binding and relation elements."),
							TEXT("element_kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetElementKind())),
							MakeShared<FJsonValueString>(TEXT("item, binding or relation")));
					}
				}
				else if (Profile.GetCapabilityId()
					== TEXT("ai.blackboard.schema.apply"))
				{
					UBlackboardData* const Blackboard =
						Cast<UBlackboardData>(&Target);
					FString KeyType;
					FString Ownership;
					const auto Input = ValueInput(TEXT("graph_compound_blackboard_entry_invalid"));
					if (!Blackboard)
					{
						OutError = NativeTypeFailure(TEXT("graph_compound_blackboard_entry_invalid"),
							TEXT("The native mutation target is not a BlackboardData asset."),
							Request.GetTargetRef(), TEXT("target.class"), &Target, UBlackboardData::StaticClass());
						OutError.OperationIndex = CurrentEditIndex;
						return false;
					}
					if (Edit.GetElementKind() != EGraphCompoundElementKind::Item)
						return FailOperation(TEXT("graph_compound_blackboard_entry_invalid"),
							TEXT("Blackboard mutations require item elements."), TEXT("element_kind"),
							MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetElementKind())),
							MakeShared<FJsonValueString>(TEXT("item")));
					if (!Operation.Dependencies.IsEmpty())
						return FailOperation(TEXT("graph_compound_blackboard_entry_invalid"),
							TEXT("A Blackboard entry must not declare dependencies."), TEXT("dependencies.count"),
							MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()), MakeShared<FJsonValueNumber>(0));
					if (!Input.RequireFields(*Operation.Value, {TEXT("key_type_ref"), TEXT("ownership")})
						|| !Input.ReadString(*Operation.Value, TEXT("key_type_ref"), KeyType)
						|| !Input.ReadString(*Operation.Value, TEXT("ownership"), Ownership)) return false;
					if (Ownership != TEXT("local"))
						return Input.Fail(TEXT("A new Blackboard entry must have local ownership."), TEXT("/ownership"),
							MakeShared<FJsonValueString>(Ownership), MakeShared<FJsonValueString>(TEXT("local")));
					const FString Prefix = TEXT("item:blackboard_entry:");
					const FString Suffix = TEXT(":") + KeyType + TEXT(":") + Ownership;
					if (!SemanticId.StartsWith(Prefix) || !SemanticId.EndsWith(Suffix)
						|| SemanticId.Len() <= Prefix.Len() + Suffix.Len())
						return FailOperation(TEXT("graph_compound_blackboard_identity_invalid"),
							TEXT("The Blackboard entry identity must contain its non-empty name, key type and ownership."),
							TEXT("identity.semantic_id"), MakeShared<FJsonValueString>(SemanticId),
							MakeShared<FJsonValueString>(Prefix + TEXT("<non-empty entry name>") + Suffix));
					Operation.EntryName = FName(*SemanticId.Mid(
						Prefix.Len(), SemanticId.Len() - Prefix.Len() - Suffix.Len()));
					Operation.KeyTypeClass = LoadObject<UClass>(nullptr, *KeyType);
					if (Operation.EntryName.IsNone())
						return FailOperation(TEXT("graph_compound_blackboard_entry_conflict"),
							TEXT("The Blackboard entry name resolves to the reserved None name."),
							TEXT("entry.name"), MakeShared<FJsonValueString>(Operation.EntryName.ToString()),
							MakeShared<FJsonValueString>(TEXT("non-None entry name")));
					const auto TypeInput = ValueInput(TEXT("graph_compound_blackboard_entry_conflict"));
					if (!Operation.KeyTypeClass)
						return TypeInput.Fail(TEXT("The Blackboard key class could not be loaded."), TEXT("/key_type_ref"),
							MakeShared<FJsonValueString>(KeyType),
							MakeShared<FJsonValueString>(TEXT("loadable class derived from ") + UBlackboardKeyType::StaticClass()->GetPathName()));
					if (!Operation.KeyTypeClass->IsChildOf(UBlackboardKeyType::StaticClass()))
						return TypeInput.Fail(TEXT("The Blackboard key class does not derive from BlackboardKeyType."),
							TEXT("/key_type_ref"), ClassPathValue(Operation.KeyTypeClass), ClassPathValue(UBlackboardKeyType::StaticClass()));
					if (Operation.KeyTypeClass->HasAnyClassFlags(CLASS_Abstract))
						return TypeInput.Fail(TEXT("The Blackboard key class is abstract and cannot be instantiated."),
							TEXT("/key_type_ref"), ClassPathValue(Operation.KeyTypeClass),
							MakeShared<FJsonValueString>(TEXT("non-abstract BlackboardKeyType class")));
					if (HasEntryName(*Blackboard, Operation.EntryName))
						return FailOperation(TEXT("graph_compound_blackboard_entry_conflict"),
							TEXT("A Blackboard entry with this name already exists."), TEXT("entry.name"),
							MakeShared<FJsonValueString>(Operation.EntryName.ToString()),
							MakeShared<FJsonValueString>(TEXT("unused entry name")));
					Operation.Kind =
						EPcgAiSchemaMutationKind::BlackboardEntry;
				}
				else if (Profile.GetCapabilityId() == TEXT("ai.eqs.graph.apply"))
				{
					if (!Cast<UEnvQuery>(&Target))
					{
						OutError = NativeTypeFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
							TEXT("The native mutation target is not an EnvQuery asset."),
							Request.GetTargetRef(), TEXT("target.class"), &Target, UEnvQuery::StaticClass());
						OutError.OperationIndex = CurrentEditIndex;
						return false;
					}
					const FUeAgentCanonicalValue& Value = *Operation.Value;
					if (Edit.GetElementKind() == EGraphCompoundElementKind::Item)
					{
						FString CarrierType;
						FString PayloadType;
						const auto Input = ValueInput(TEXT("graph_compound_eqs_option_invalid"));
						if (!Operation.Dependencies.IsEmpty())
							return FailOperation(TEXT("graph_compound_eqs_option_invalid"),
								TEXT("An EQS option node must not declare dependencies."), TEXT("dependencies.count"),
								MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()), MakeShared<FJsonValueNumber>(0));
						if (!SemanticId.StartsWith(TEXT("node:"))
							|| !FGuid::Parse(SemanticId.RightChop(5), Operation.NodeGuid)
							|| !Operation.NodeGuid.IsValid()
							|| SemanticId != TEXT("node:") + Operation.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower))
							return FailOperation(TEXT("graph_compound_eqs_option_invalid"),
								TEXT("The EQS node identity must contain a valid canonical GUID."),
								TEXT("identity.semantic_id"), MakeShared<FJsonValueString>(SemanticId),
								MakeShared<FJsonValueString>(TEXT("node:<lowercase hyphenated GUID>")));
						if (!Input.RequireFields(Value, {TEXT("carrier_type_ref"), TEXT("payload_type_ref")})
							|| !Input.ReadString(Value, TEXT("carrier_type_ref"), CarrierType)) return false;
						if (CarrierType != TEXT("/Script/EnvironmentQueryEditor.EnvironmentQueryGraphNode_Option"))
							return Input.Fail(TEXT("An EQS option requires the option graph-node carrier class."),
								TEXT("/carrier_type_ref"), MakeShared<FJsonValueString>(CarrierType),
								MakeShared<FJsonValueString>(TEXT("/Script/EnvironmentQueryEditor.EnvironmentQueryGraphNode_Option")));
						if (!Input.ReadString(Value, TEXT("payload_type_ref"), PayloadType)) return false;
						Operation.PayloadClass = LoadObject<UClass>(nullptr, *PayloadType);
						const auto PayloadInput = ValueInput(TEXT("graph_compound_eqs_generator_invalid"));
						if (!Operation.PayloadClass)
							return PayloadInput.Fail(TEXT("The EQS generator class could not be loaded."),
								TEXT("/payload_type_ref"), MakeShared<FJsonValueString>(PayloadType),
								MakeShared<FJsonValueString>(TEXT("loadable class derived from ") + UEnvQueryGenerator::StaticClass()->GetPathName()));
						if (!Operation.PayloadClass->IsChildOf(UEnvQueryGenerator::StaticClass()))
							return PayloadInput.Fail(TEXT("The EQS generator class does not derive from EnvQueryGenerator."),
								TEXT("/payload_type_ref"), ClassPathValue(Operation.PayloadClass), ClassPathValue(UEnvQueryGenerator::StaticClass()));
						if (Operation.PayloadClass->HasAnyClassFlags(CLASS_Abstract))
							return PayloadInput.Fail(TEXT("The EQS generator class is abstract and cannot be instantiated."),
								TEXT("/payload_type_ref"), ClassPathValue(Operation.PayloadClass),
								MakeShared<FJsonValueString>(TEXT("non-abstract EnvQueryGenerator class")));
						Operation.Kind = EPcgAiSchemaMutationKind::EqsOption;
					}
					else if (Edit.GetElementKind() == EGraphCompoundElementKind::Property)
					{
						FString PropertyPath;
						FString ValueType;
						const auto Input = ValueInput(TEXT("graph_compound_eqs_property_invalid"));
						if (Operation.Dependencies.Num() != 1)
							return FailOperation(TEXT("graph_compound_eqs_property_invalid"),
								TEXT("An EQS node property requires exactly one node dependency."), TEXT("dependencies.count"),
								MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()), MakeShared<FJsonValueNumber>(1));
						if (!Input.RequireFields(Value, {TEXT("property_path"), TEXT("value_type"), TEXT("value")})
							|| !Input.ReadString(Value, TEXT("property_path"), PropertyPath)) return false;
						if (PropertyPath != TEXT("NodePosX"))
							return Input.Fail(TEXT("This EQS mutation supports only the NodePosX property."),
								TEXT("/property_path"), MakeShared<FJsonValueString>(PropertyPath), MakeShared<FJsonValueString>(TEXT("NodePosX")));
						if (!Input.ReadString(Value, TEXT("value_type"), ValueType)) return false;
						if (ValueType != TEXT("int32"))
							return Input.Fail(TEXT("The EQS NodePosX property requires the int32 value type."),
								TEXT("/value_type"), MakeShared<FJsonValueString>(ValueType), MakeShared<FJsonValueString>(TEXT("int32")));
						if (!Input.ReadInteger(Value, TEXT("value"), Operation.SignedIntegerValue, true)) return false;
						Operation.FromItem = Operation.Dependencies[0];
						if (!ProposedNodeIds.Contains(Operation.FromItem))
							return FailOperation(TEXT("graph_compound_eqs_property_identity_invalid"),
								TEXT("The EQS property owner is not a proposed node."), TEXT("owner_item_ref"),
								MakeShared<FJsonValueString>(Operation.FromItem), MakeShared<FJsonValueString>(TEXT("proposed node")));
						const FString ExpectedId = TEXT("property:") + Operation.FromItem + TEXT(":NodePosX");
						if (SemanticId != ExpectedId)
							return FailOperation(TEXT("graph_compound_eqs_property_identity_invalid"),
								TEXT("The EQS property identity differs from its owner and property path."),
								TEXT("identity.semantic_id"), MakeShared<FJsonValueString>(SemanticId), MakeShared<FJsonValueString>(ExpectedId));
						Operation.Kind = EPcgAiSchemaMutationKind::EqsProperty;
					}
					else
					{
						return FailOperation(TEXT("graph_compound_eqs_edit_kind_invalid"),
							TEXT("EQS mutations support only item and property elements."), TEXT("element_kind"),
							MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetElementKind())),
							MakeShared<FJsonValueString>(TEXT("item or property")));
					}
				}
				else if (Profile.GetCapabilityId()
					== TEXT("ai.state_tree.hierarchy.apply"))
				{
#if WITH_EDITORONLY_DATA
					UStateTree* const Tree = Cast<UStateTree>(&Target);
					UStateTreeEditorData* const EditorData = Tree
						? Cast<UStateTreeEditorData>(Tree->EditorData) : nullptr;
					TMap<FGuid, UStateTreeState*> States;
					TMap<FGuid, FString> Parents;
					if (!Tree || !EditorData)
					{
						OutError = !Tree
							? NativeTypeFailure(TEXT("graph_compound_state_tree_hierarchy_invalid"),
								TEXT("The native hierarchy target is not a StateTree."),
								Request.GetTargetRef(), TEXT("target.class"), &Target, UStateTree::StaticClass())
							: NativeTypeFailure(TEXT("graph_compound_state_tree_hierarchy_invalid"),
								TEXT("The StateTree has no valid StateTree editor data."),
								Request.GetTargetRef(), TEXT("editor_data.class"), Tree->EditorData, UStateTreeEditorData::StaticClass());
						return false;
					}
					if (!TryCollectStateTreeStates(*EditorData, States, Parents, OutError))
					{
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetTargetRef();
						return false;
					}
					const FUeAgentCanonicalValue& Value = *Operation.Value;
					if (Edit.GetElementKind()
						== EGraphCompoundElementKind::Item)
					{
						FString OwnerState;
						FString NodeClass;
						const auto Input = ValueInput(TEXT("graph_compound_state_tree_node_invalid"));
						if (!SemanticId.StartsWith(TEXT("node:"))
							|| !FGuid::Parse(SemanticId.RightChop(5), Operation.NodeGuid)
							|| !Operation.NodeGuid.IsValid()
							|| SemanticId != TEXT("node:") + Operation.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower))
							return FailOperation(TEXT("graph_compound_state_tree_node_invalid"),
								TEXT("The StateTree node identity must contain a valid canonical GUID."),
								TEXT("identity.semantic_id"), MakeShared<FJsonValueString>(SemanticId),
								MakeShared<FJsonValueString>(TEXT("node:<lowercase hyphenated GUID>")));
						if (!Input.RequireFields(Value, {TEXT("owner_state_ref"), TEXT("node_class_ref"), TEXT("placement")})
							|| !Input.ReadString(Value, TEXT("owner_state_ref"), OwnerState)
							|| !Input.ReadString(Value, TEXT("node_class_ref"), NodeClass)
							|| !Input.ReadString(Value, TEXT("placement"), Operation.Placement)) return false;
						const bool bStateOwned = Operation.Placement == TEXT("state_task")
							|| Operation.Placement == TEXT("state_condition");
						const auto PlacementInput = ValueInput(TEXT("graph_compound_state_tree_node_placement_invalid"));
						if (bStateOwned)
						{
							if (Operation.Dependencies.Num() != 1)
								return FailOperation(TEXT("graph_compound_state_tree_node_placement_invalid"),
									TEXT("A state-owned StateTree node requires exactly one state dependency."),
									TEXT("dependencies.count"), MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()),
									MakeShared<FJsonValueNumber>(1));
							if (Operation.Dependencies[0] != OwnerState)
								return FailOperation(TEXT("graph_compound_state_tree_node_placement_invalid"),
									TEXT("The StateTree node dependency differs from its owner state."),
									TEXT("dependencies/0"), MakeShared<FJsonValueString>(Operation.Dependencies[0]),
									MakeShared<FJsonValueString>(OwnerState));
							if (!TryParseStateTreeStateIdentity(OwnerState, Operation.StateGuid))
								return PlacementInput.Fail(TEXT("The StateTree node owner is not a valid state identity."),
									TEXT("/owner_state_ref"), MakeShared<FJsonValueString>(OwnerState),
									MakeShared<FJsonValueString>(TEXT("state:<lowercase hyphenated GUID>")));
							if (!States.Contains(Operation.StateGuid) && !ProposedStateIds.Contains(OwnerState))
								return PlacementInput.Fail(TEXT("The StateTree node owner is neither an existing nor a proposed state."),
									TEXT("/owner_state_ref"), MakeShared<FJsonValueString>(OwnerState),
									MakeShared<FJsonValueString>(TEXT("existing or proposed state")));
						}
						else
						{
							if (OwnerState != TEXT("global"))
								return PlacementInput.Fail(TEXT("A non-state-owned StateTree node requires the global owner."),
									TEXT("/owner_state_ref"), MakeShared<FJsonValueString>(OwnerState),
									MakeShared<FJsonValueString>(TEXT("global")));
							if (!Operation.Dependencies.IsEmpty())
								return FailOperation(TEXT("graph_compound_state_tree_node_placement_invalid"),
									TEXT("A global StateTree node must not declare state dependencies."),
									TEXT("dependencies.count"), MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()),
									MakeShared<FJsonValueNumber>(0));
							if (Operation.Placement != TEXT("global_task") && Operation.Placement != TEXT("evaluator"))
								return PlacementInput.Fail(TEXT("A global StateTree node requires global_task or evaluator placement."),
									TEXT("/placement"), MakeShared<FJsonValueString>(Operation.Placement),
									MakeShared<FJsonValueString>(TEXT("global_task or evaluator")));
						}
						Operation.PayloadClass =
							LoadObject<UClass>(nullptr, *NodeClass);
						if (!Operation.PayloadClass)
						{
							Operation.PayloadStruct =
								LoadObject<UScriptStruct>(nullptr, *NodeClass);
						}
						const bool bBlueprintNode = Operation.PayloadClass
							&& Operation.PayloadClass->IsChildOf(
								UStateTreeNodeBlueprintBase::StaticClass())
							&& !Operation.PayloadClass->HasAnyClassFlags(CLASS_Abstract);
						const bool bNativeNode = Operation.PayloadStruct
							&& (Operation.PayloadStruct->IsChildOf(
									FStateTreeTaskBase::StaticStruct())
								|| Operation.PayloadStruct->IsChildOf(
									FStateTreeConditionBase::StaticStruct())
								|| Operation.PayloadStruct->IsChildOf(
									FStateTreeEvaluatorBase::StaticStruct()));
						if (!bBlueprintNode && !bNativeNode)
						{
							const FString Reason = Operation.PayloadClass
								? (!Operation.PayloadClass->IsChildOf(UStateTreeNodeBlueprintBase::StaticClass())
									? TEXT("The StateTree node class does not derive from StateTreeNodeBlueprintBase.")
									: TEXT("The StateTree node class is abstract and cannot be instantiated."))
								: (Operation.PayloadStruct
									? TEXT("The StateTree node struct does not derive from a supported task, condition or evaluator base.")
									: TEXT("The StateTree node reference could not be loaded as a class or script struct."));
							return ValueInput(TEXT("graph_compound_state_tree_node_class_invalid")).Fail(
								Reason, TEXT("/node_class_ref"), MakeShared<FJsonValueString>(NodeClass),
								MakeShared<FJsonValueString>(TEXT("non-abstract StateTreeNodeBlueprintBase class or StateTreeTaskBase/StateTreeConditionBase/StateTreeEvaluatorBase struct")));
						}
						Operation.Kind =
							EPcgAiSchemaMutationKind::StateTreeNode;
					}
					else if (Edit.GetElementKind()
						== EGraphCompoundElementKind::Hierarchy)
					{
						FString NativeGuid;
						FString ParentIdentity;
						FString PayloadType;
						const auto Input = ValueInput(TEXT("graph_compound_state_tree_hierarchy_invalid"));
						if (!Operation.Dependencies.IsEmpty())
							return FailOperation(TEXT("graph_compound_state_tree_hierarchy_invalid"),
								TEXT("A StateTree hierarchy insertion must not declare dependencies."),
								TEXT("dependencies.count"), MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()), MakeShared<FJsonValueNumber>(0));
						if (!TryParseStateTreeStateIdentity(SemanticId, Operation.StateGuid))
							return FailOperation(TEXT("graph_compound_state_tree_hierarchy_invalid"),
								TEXT("The hierarchy identity must contain a valid canonical state GUID."),
								TEXT("identity.semantic_id"), MakeShared<FJsonValueString>(SemanticId),
								MakeShared<FJsonValueString>(TEXT("state:<lowercase hyphenated GUID>")));
						if (States.Contains(Operation.StateGuid))
							return FailOperation(TEXT("graph_compound_state_tree_hierarchy_invalid"),
								TEXT("A StateTree state with this identity already exists."),
								TEXT("identity.semantic_id"), MakeShared<FJsonValueString>(SemanticId),
								MakeShared<FJsonValueString>(TEXT("unused state identity")));
						if (!Input.RequireFields(Value, {TEXT("native_guid"), TEXT("parent_identity"), TEXT("payload_type_ref")})
							|| !Input.ReadString(Value, TEXT("native_guid"), NativeGuid)) return false;
						if (NativeGuid != Operation.StateGuid.ToString(EGuidFormats::DigitsWithHyphensLower))
							return Input.Fail(TEXT("The StateTree native GUID differs from the hierarchy identity."),
								TEXT("/native_guid"), MakeShared<FJsonValueString>(NativeGuid),
								MakeShared<FJsonValueString>(Operation.StateGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
						if (!Input.ReadString(Value, TEXT("parent_identity"), ParentIdentity)) return false;
						if (!TryParseStateTreeStateIdentity(ParentIdentity, Operation.ParentStateGuid))
							return Input.Fail(TEXT("The hierarchy parent is not a valid canonical state identity."),
								TEXT("/parent_identity"), MakeShared<FJsonValueString>(ParentIdentity),
								MakeShared<FJsonValueString>(TEXT("state:<lowercase hyphenated GUID>")));
						if (!States.Contains(Operation.ParentStateGuid))
							return Input.Fail(TEXT("The hierarchy parent does not identify an existing state."),
								TEXT("/parent_identity"), MakeShared<FJsonValueString>(ParentIdentity),
								MakeShared<FJsonValueString>(TEXT("existing state")));
						if (!Input.ReadString(Value, TEXT("payload_type_ref"), PayloadType)) return false;
						if (PayloadType != UStateTreeState::StaticClass()->GetPathName())
							return Input.Fail(TEXT("The hierarchy payload requires the StateTreeState class."),
								TEXT("/payload_type_ref"), MakeShared<FJsonValueString>(PayloadType),
								ClassPathValue(UStateTreeState::StaticClass()));
						Operation.Kind =
							EPcgAiSchemaMutationKind::StateTreeHierarchy;
					}
					else if (Edit.GetElementKind()
						== EGraphCompoundElementKind::Order)
					{
						FString OwnerIdentity;
						TArray<FString> OrderedIdentities;
						const FString Prefix = TEXT("order:");
						const FString Suffix = TEXT(":children");
						const auto Input = ValueInput(TEXT("graph_compound_state_tree_order_invalid"));
						if (!Input.RequireFields(Value, {TEXT("owner_identity"), TEXT("ordered_identities")})
							|| !Input.ReadString(Value, TEXT("owner_identity"), OwnerIdentity)) return false;
						if (!TryParseStateTreeStateIdentity(OwnerIdentity, Operation.ParentStateGuid))
							return Input.Fail(TEXT("The StateTree order owner is not a valid canonical state identity."),
								TEXT("/owner_identity"), MakeShared<FJsonValueString>(OwnerIdentity),
								MakeShared<FJsonValueString>(TEXT("state:<lowercase hyphenated GUID>")));
						if (!States.Contains(Operation.ParentStateGuid))
							return Input.Fail(TEXT("The StateTree order owner does not identify an existing state."),
								TEXT("/owner_identity"), MakeShared<FJsonValueString>(OwnerIdentity),
								MakeShared<FJsonValueString>(TEXT("existing state")));
						if (SemanticId != Prefix + OwnerIdentity + Suffix)
							return FailOperation(TEXT("graph_compound_state_tree_order_invalid"),
								TEXT("The StateTree order identity differs from its owner."),
								TEXT("identity.semantic_id"), MakeShared<FJsonValueString>(SemanticId),
								MakeShared<FJsonValueString>(Prefix + OwnerIdentity + Suffix));
						if (!Input.ReadStringArray(Value, TEXT("ordered_identities"), OrderedIdentities)) return false;
						TSet<FString> OrderedSet;
						TSet<FString> RequiredDependencies;
						for (int32 OrderedIndex = 0; OrderedIndex < OrderedIdentities.Num(); ++OrderedIndex)
						{
							const FString& OrderedIdentity = OrderedIdentities[OrderedIndex];
							const FString ItemPointer = FString::Printf(TEXT("/ordered_identities/%d"), OrderedIndex);
							FGuid OrderedGuid;
							if (OrderedSet.Contains(OrderedIdentity))
								return Input.Fail(TEXT("The StateTree child order contains a duplicate state identity."),
									ItemPointer, MakeShared<FJsonValueString>(OrderedIdentity),
									MakeShared<FJsonValueString>(TEXT("unique child state identity")));
							if (!TryParseStateTreeStateIdentity(OrderedIdentity, OrderedGuid))
								return Input.Fail(TEXT("The ordered child is not a valid canonical state identity."),
									ItemPointer, MakeShared<FJsonValueString>(OrderedIdentity),
									MakeShared<FJsonValueString>(TEXT("state:<lowercase hyphenated GUID>")));
							OrderedSet.Add(OrderedIdentity);
							if (UStateTreeState* const* Existing = States.Find(OrderedGuid))
							{
								if (!(*Existing))
									return Input.Fail(TEXT("The ordered child resolves to a missing native state object."),
										ItemPointer, MakeShared<FJsonValueString>(OrderedIdentity),
										MakeShared<FJsonValueString>(TEXT("existing child state object")));
								const FString ActualParent = Parents.FindRef(OrderedGuid);
								if (ActualParent != OwnerIdentity)
								{
									Input.Fail(TEXT("The ordered existing state belongs to a different parent."),
										ItemPointer, MakeShared<FJsonValueString>(ActualParent), MakeShared<FJsonValueString>(OwnerIdentity));
									OutError.CandidateName = OrderedIdentity;
									return false;
								}
							}
							else if (ProposedStateIds.Contains(OrderedIdentity)
								&& ProposedStateParents.FindRef(OrderedIdentity) == OwnerIdentity)
							{
								RequiredDependencies.Add(OrderedIdentity);
							}
							else
							{
								if (!ProposedStateIds.Contains(OrderedIdentity))
									return Input.Fail(TEXT("The ordered child is neither an existing nor a proposed state."),
										ItemPointer, MakeShared<FJsonValueString>(OrderedIdentity),
										MakeShared<FJsonValueString>(TEXT("existing or proposed child state")));
								Input.Fail(TEXT("The ordered proposed state belongs to a different parent."),
									ItemPointer, MakeShared<FJsonValueString>(ProposedStateParents.FindRef(OrderedIdentity)),
									MakeShared<FJsonValueString>(OwnerIdentity));
								OutError.CandidateName = OrderedIdentity;
								return false;
							}
							Operation.OrderedStateGuids.Add(OrderedGuid);
						}
						for (const TPair<FGuid, UStateTreeState*>& Pair : States)
						{
							if (Parents.FindRef(Pair.Key) == OwnerIdentity
								&& !OrderedSet.Contains(StateTreeStateSemanticId(Pair.Key)))
								return Input.Fail(TEXT("The StateTree child order omits an existing child state."),
									TEXT("/ordered_identities"), nullptr,
									MakeShared<FJsonValueString>(StateTreeStateSemanticId(Pair.Key)));
						}
						for (const TPair<FString, FString>& Pair : ProposedStateParents)
						{
							if (Pair.Value == OwnerIdentity && !OrderedSet.Contains(Pair.Key))
								return Input.Fail(TEXT("The StateTree child order omits a proposed child state."),
									TEXT("/ordered_identities"), nullptr, MakeShared<FJsonValueString>(Pair.Key));
						}
						if (Operation.Dependencies.Num() != RequiredDependencies.Num())
							return FailOperation(TEXT("graph_compound_state_tree_order_invalid"),
								TEXT("The StateTree order dependency count differs from the proposed child count."),
								TEXT("dependencies.count"), MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()),
								MakeShared<FJsonValueNumber>(RequiredDependencies.Num()));
						for (const FString& Dependency : Operation.Dependencies)
						{
							if (!RequiredDependencies.Contains(Dependency))
								return FailOperation(TEXT("graph_compound_state_tree_order_invalid"),
									TEXT("The StateTree order dependency is not one of its proposed children."),
									TEXT("dependency.identity"), MakeShared<FJsonValueString>(Dependency),
									MakeShared<FJsonValueString>(TEXT("proposed child of ") + OwnerIdentity));
						}
						Operation.Kind = EPcgAiSchemaMutationKind::StateTreeOrder;
					}
					else if (Edit.GetElementKind()
						== EGraphCompoundElementKind::Property)
					{
						FString PropertyPath;
						FString ValueType;
						FString StateName;
						const auto Input = ValueInput(TEXT("graph_compound_state_tree_property_invalid"));
						if (Operation.Dependencies.Num() != 1)
							return FailOperation(TEXT("graph_compound_state_tree_property_invalid"),
								TEXT("A StateTree property requires exactly one state dependency."),
								TEXT("dependencies.count"), MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()), MakeShared<FJsonValueNumber>(1));
						if (!ProposedStateIds.Contains(Operation.Dependencies[0]))
							return FailOperation(TEXT("graph_compound_state_tree_property_invalid"),
								TEXT("The StateTree property owner is not a proposed state."),
								TEXT("dependencies/0"), MakeShared<FJsonValueString>(Operation.Dependencies[0]),
								MakeShared<FJsonValueString>(TEXT("proposed state")));
						const FString ExpectedId = TEXT("property:") + Operation.Dependencies[0] + TEXT(":Name");
						if (SemanticId != ExpectedId)
							return FailOperation(TEXT("graph_compound_state_tree_property_invalid"),
								TEXT("The StateTree property identity differs from its owner and Name property."),
								TEXT("identity.semantic_id"), MakeShared<FJsonValueString>(SemanticId), MakeShared<FJsonValueString>(ExpectedId));
						if (!TryParseStateTreeStateIdentity(Operation.Dependencies[0], Operation.StateGuid))
							return FailOperation(TEXT("graph_compound_state_tree_property_invalid"),
								TEXT("The StateTree property dependency is not a valid canonical state identity."),
								TEXT("dependencies/0"), MakeShared<FJsonValueString>(Operation.Dependencies[0]),
								MakeShared<FJsonValueString>(TEXT("state:<lowercase hyphenated GUID>")));
						if (!Input.RequireFields(Value, {TEXT("property_path"), TEXT("value_type"), TEXT("value")})
							|| !Input.ReadString(Value, TEXT("property_path"), PropertyPath)) return false;
						if (PropertyPath != TEXT("Name"))
							return Input.Fail(TEXT("This StateTree mutation supports only the Name property."),
								TEXT("/property_path"), MakeShared<FJsonValueString>(PropertyPath), MakeShared<FJsonValueString>(TEXT("Name")));
						if (!Input.ReadString(Value, TEXT("value_type"), ValueType)) return false;
						if (ValueType != TEXT("name"))
							return Input.Fail(TEXT("The StateTree Name property requires the name value type."),
								TEXT("/value_type"), MakeShared<FJsonValueString>(ValueType), MakeShared<FJsonValueString>(TEXT("name")));
						if (!Input.ReadString(Value, TEXT("value"), StateName)) return false;
						Operation.StateName = FName(*StateName);
						if (Operation.StateName.IsNone())
							return Input.Fail(TEXT("The StateTree name resolves to the reserved None name."),
								TEXT("/value"), MakeShared<FJsonValueString>(StateName),
								MakeShared<FJsonValueString>(TEXT("non-None name")));
						Operation.Kind = EPcgAiSchemaMutationKind::StateTreeProperty;
					}
					else
					{
						return FailOperation(TEXT("graph_compound_state_tree_edit_kind_invalid"),
							TEXT("StateTree mutations support only item, hierarchy, order and property elements."),
							TEXT("element_kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetElementKind())),
							MakeShared<FJsonValueString>(TEXT("item, hierarchy, order or property")));
					}
#else
					return FailOperation(TEXT("graph_compound_state_tree_editor_data_unavailable"),
						TEXT("StateTree hierarchy mutation requires an editor build with editor-only data."),
						TEXT("WITH_EDITORONLY_DATA"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
#endif
				}
				else if (Profile.GetCapabilityId() == TEXT("ai.smart_object_definition.apply"))
				{
					FString BehaviorType;
					FString DataType;
					const FString Prefix = TEXT("item:slot:");
					const auto Input = ValueInput(TEXT("graph_compound_smart_object_slot_invalid"));
					if (!Cast<USmartObjectDefinition>(&Target))
					{
						OutError = NativeTypeFailure(TEXT("graph_compound_smart_object_slot_invalid"),
							TEXT("The native mutation target is not a SmartObjectDefinition asset."),
							Request.GetTargetRef(), TEXT("target.class"), &Target, USmartObjectDefinition::StaticClass());
						OutError.OperationIndex = CurrentEditIndex;
						return false;
					}
					if (Edit.GetElementKind() != EGraphCompoundElementKind::Item)
						return FailOperation(TEXT("graph_compound_smart_object_slot_invalid"),
							TEXT("SmartObject slot mutations require item elements."), TEXT("element_kind"),
							MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetElementKind())),
							MakeShared<FJsonValueString>(TEXT("item")));
					if (!Operation.Dependencies.IsEmpty())
						return FailOperation(TEXT("graph_compound_smart_object_slot_invalid"),
							TEXT("A SmartObject slot must not declare dependencies."), TEXT("dependencies.count"),
							MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()), MakeShared<FJsonValueNumber>(0));
					if (!SemanticId.StartsWith(Prefix)
						|| !FGuid::Parse(SemanticId.RightChop(Prefix.Len()), Operation.SlotGuid)
						|| !Operation.SlotGuid.IsValid()
						|| SemanticId != Prefix + Operation.SlotGuid.ToString(EGuidFormats::DigitsWithHyphensLower))
						return FailOperation(TEXT("graph_compound_smart_object_slot_invalid"),
							TEXT("The SmartObject slot identity must contain a valid canonical GUID."),
							TEXT("identity.semantic_id"), MakeShared<FJsonValueString>(SemanticId),
							MakeShared<FJsonValueString>(Prefix + TEXT("<lowercase hyphenated GUID>")));
					if (!Input.RequireFields(*Operation.Value, {TEXT("behavior_type_ref"), TEXT("data_type_ref")})
						|| !Input.ReadString(*Operation.Value, TEXT("behavior_type_ref"), BehaviorType)
						|| !Input.ReadString(*Operation.Value, TEXT("data_type_ref"), DataType)) return false;
					if (DataType != FSmartObjectSlotDefinition::StaticStruct()->GetPathName())
						return Input.Fail(TEXT("The SmartObject slot requires the SmartObjectSlotDefinition data type."),
							TEXT("/data_type_ref"), MakeShared<FJsonValueString>(DataType),
							MakeShared<FJsonValueString>(FSmartObjectSlotDefinition::StaticStruct()->GetPathName()));
					Operation.BehaviorClass = LoadObject<UClass>(nullptr, *BehaviorType);
					const auto BehaviorInput = ValueInput(TEXT("graph_compound_smart_object_behavior_invalid"));
					if (!Operation.BehaviorClass)
						return BehaviorInput.Fail(TEXT("The SmartObject behavior class could not be loaded."),
							TEXT("/behavior_type_ref"), MakeShared<FJsonValueString>(BehaviorType),
							MakeShared<FJsonValueString>(TEXT("loadable class derived from ") + USmartObjectBehaviorDefinition::StaticClass()->GetPathName()));
					if (!Operation.BehaviorClass->IsChildOf(USmartObjectBehaviorDefinition::StaticClass()))
						return BehaviorInput.Fail(TEXT("The SmartObject behavior class does not derive from SmartObjectBehaviorDefinition."),
							TEXT("/behavior_type_ref"), ClassPathValue(Operation.BehaviorClass), ClassPathValue(USmartObjectBehaviorDefinition::StaticClass()));
					if (Operation.BehaviorClass->HasAnyClassFlags(CLASS_Abstract))
						return BehaviorInput.Fail(TEXT("The SmartObject behavior class is abstract and cannot be instantiated."),
							TEXT("/behavior_type_ref"), ClassPathValue(Operation.BehaviorClass),
							MakeShared<FJsonValueString>(TEXT("non-abstract SmartObjectBehaviorDefinition class")));
					Operation.Kind = EPcgAiSchemaMutationKind::SmartObjectSlot;
				}
				else if (bPcgComponentProfile)
				{
					UPCGComponent* const Component = Cast<UPCGComponent>(&Target);
					AActor* const Owner = Component ? Component->GetOwner() : nullptr;
					UWorld* const World = Component ? Component->GetWorld() : nullptr;
					FString GraphRef;
					FString ComponentRef;
					const bool bWorldBinding = Profile.GetInputKind()
						== EPcgAnimationGraphInputKind::PcgWorldBinding;
					const auto Input = ValueInput(TEXT("graph_compound_pcg_component_binding_invalid"));
					if (!Component)
					{
						OutError = NativeTypeFailure(TEXT("graph_compound_pcg_component_binding_invalid"),
							TEXT("The native binding target is not a PCGComponent."),
							Request.GetTargetRef(), TEXT("target.class"), &Target, UPCGComponent::StaticClass());
						OutError.OperationIndex = CurrentEditIndex;
						return false;
					}
					if (!Owner)
						return FailOperation(TEXT("graph_compound_pcg_component_binding_invalid"),
							TEXT("The PCG component has no owning actor."), TEXT("component.owner"),
							nullptr, MakeShared<FJsonValueString>(TEXT("owning actor")));
					if (!World)
						return FailOperation(TEXT("graph_compound_pcg_component_binding_invalid"),
							TEXT("The PCG component has no world."), TEXT("component.world"),
							nullptr, MakeShared<FJsonValueString>(TEXT("world")));
					if (Owner->GetWorld() != World)
						return FailOperation(TEXT("graph_compound_pcg_component_binding_invalid"),
							TEXT("The PCG component and its owner belong to different worlds."), TEXT("owner.world"),
							Owner->GetWorld() ? MakeShared<FJsonValueString>(Owner->GetWorld()->GetPathName()) : TSharedPtr<FJsonValue>(),
							MakeShared<FJsonValueString>(World->GetPathName()));
					if (!GEngine)
						return FailOperation(TEXT("graph_compound_pcg_component_binding_invalid"),
							TEXT("The engine is unavailable for PCG world-context lookup."), TEXT("engine"),
							nullptr, MakeShared<FJsonValueString>(TEXT("engine instance")));
					if (!GEngine->GetWorldContextFromWorld(World))
						return FailOperation(TEXT("graph_compound_pcg_component_binding_invalid"),
							TEXT("The PCG component world has no registered engine world context."), TEXT("world_context"),
							MakeShared<FJsonValueString>(World->GetPathName()), MakeShared<FJsonValueString>(TEXT("world with registered context")));
					if (Edit.GetElementKind() != EGraphCompoundElementKind::Binding)
						return FailOperation(TEXT("graph_compound_pcg_component_binding_invalid"),
							TEXT("PCG component graph mutations require binding elements."), TEXT("element_kind"),
							MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetElementKind())), MakeShared<FJsonValueString>(TEXT("binding")));
					if (SemanticId != TEXT("binding:pcg_component_graph"))
						return FailOperation(TEXT("graph_compound_pcg_component_binding_invalid"),
							TEXT("The PCG component binding uses an unsupported identity."),
							TEXT("identity.semantic_id"), MakeShared<FJsonValueString>(SemanticId),
							MakeShared<FJsonValueString>(TEXT("binding:pcg_component_graph")));
					if (!Operation.Dependencies.IsEmpty())
						return FailOperation(TEXT("graph_compound_pcg_component_binding_invalid"),
							TEXT("The PCG component binding must not declare dependencies."), TEXT("dependencies.count"),
							MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()), MakeShared<FJsonValueNumber>(0));
					if (!Input.RequireFields(*Operation.Value, {TEXT("actor_ref"), TEXT("component_ref"), TEXT("generation_policy"), TEXT("graph_ref")})
						|| !Input.ReadString(*Operation.Value, TEXT("actor_ref"), Operation.ActorRef)
						|| !Input.ReadString(*Operation.Value, TEXT("component_ref"), ComponentRef)) return false;
					if (ComponentRef != Component->GetPathName())
						return Input.Fail(TEXT("The binding component reference differs from the native target."),
							TEXT("/component_ref"), MakeShared<FJsonValueString>(ComponentRef), MakeShared<FJsonValueString>(Component->GetPathName()));
					if (!Input.ReadString(*Operation.Value, TEXT("generation_policy"), Operation.PublicGenerationPolicy)) return false;
					if (!TryNativePcgGenerationPolicy(Operation.PublicGenerationPolicy,
							Operation.NativeGenerationPolicy, Operation.NativeGenerationTrigger))
						return Input.Fail(TEXT("The PCG generation policy is not supported."), TEXT("/generation_policy"),
							MakeShared<FJsonValueString>(Operation.PublicGenerationPolicy),
							MakeShared<FJsonValueString>(TEXT("on_demand, on_load or runtime")));
					if (!Input.ReadString(*Operation.Value, TEXT("graph_ref"), GraphRef)) return false;
					if (bWorldBinding && Operation.ActorRef != Owner->GetPathName())
						return ValueInput(TEXT("graph_compound_pcg_component_owner_mismatch")).Fail(
							TEXT("The world binding actor reference differs from the component owner."),
							TEXT("/actor_ref"), MakeShared<FJsonValueString>(Operation.ActorRef), MakeShared<FJsonValueString>(Owner->GetPathName()));
					if (!bWorldBinding)
					{
						Operation.ActorRef = Owner->GetPathName();
					}
					Operation.GraphInterface = LoadObject<UPCGGraphInterface>(nullptr, *GraphRef);
					const auto GraphInput = ValueInput(TEXT("graph_compound_pcg_component_graph_invalid"));
					if (!Operation.GraphInterface)
						return GraphInput.Fail(TEXT("The PCG graph reference could not be loaded as a graph interface."),
							TEXT("/graph_ref"), MakeShared<FJsonValueString>(GraphRef),
							MakeShared<FJsonValueString>(TEXT("loadable PCGGraphInterface")));
					if (Operation.GraphInterface->GetPathName() != GraphRef)
						return GraphInput.Fail(TEXT("The loaded PCG graph path differs from the requested reference."),
							TEXT("/graph_ref"), MakeShared<FJsonValueString>(GraphRef),
							MakeShared<FJsonValueString>(Operation.GraphInterface->GetPathName()));
					if (!Operation.GraphInterface->GetGraph())
						return GraphInput.Fail(TEXT("The PCG graph interface has no graph."),
							TEXT("/graph_ref"), MakeShared<FJsonValueString>(GraphRef),
							MakeShared<FJsonValueString>(TEXT("PCGGraphInterface with a graph")));
					Operation.Kind =
						EPcgAiSchemaMutationKind::PcgComponentBinding;
				}
				else if (Profile.GetCapabilityId() == TEXT("pcg.graph.apply"))
				{
					UPCGGraph* const Graph = Cast<UPCGGraph>(&Target);
					const FString Prefix = TEXT("pcg_node:");
					const FString AuthoredIdentity =
						SemanticId.StartsWith(Prefix)
							? SemanticId.RightChop(Prefix.Len()) : FString();
					const FUeAgentCanonicalValue* const Candidate =
						Operation.Value.IsValid()
							? FindObjectValue(*Operation.Value, TEXT("candidate"))
							: nullptr;
					const auto Input = ValueInput(TEXT("graph_compound_pcg_graph_node_invalid"));
					if (!Graph)
					{
						OutError = NativeTypeFailure(TEXT("graph_compound_pcg_graph_node_invalid"),
							TEXT("The native mutation target is not a PCGGraph."),
							Request.GetTargetRef(), TEXT("target.class"), &Target, UPCGGraph::StaticClass());
						OutError.OperationIndex = CurrentEditIndex;
						return false;
					}
					if (Edit.GetElementKind() != EGraphCompoundElementKind::Item)
						return FailOperation(TEXT("graph_compound_pcg_graph_node_invalid"),
							TEXT("PCG graph node mutations require item elements."), TEXT("element_kind"),
							MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetElementKind())), MakeShared<FJsonValueString>(TEXT("item")));
					if (!Operation.Dependencies.IsEmpty())
						return FailOperation(TEXT("graph_compound_pcg_graph_node_invalid"),
							TEXT("A PCG graph node must not declare dependencies."), TEXT("dependencies.count"),
							MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()), MakeShared<FJsonValueNumber>(0));
					if (AuthoredIdentity.IsEmpty() || AuthoredIdentity.Contains(TEXT(":")))
						return FailOperation(TEXT("graph_compound_pcg_graph_node_invalid"),
							TEXT("The PCG node identity requires a non-empty authored name without colons."),
							TEXT("identity.semantic_id"), MakeShared<FJsonValueString>(SemanticId),
							MakeShared<FJsonValueString>(TEXT("pcg_node:<non-empty name without ':'>")));
					if (!Input.RequireFields(*Operation.Value, {TEXT("candidate"), TEXT("position")}, false)) return false;
					if (!Candidate)
						return Input.Fail(TEXT("The required PCG node candidate field is missing."), TEXT("/candidate"),
							nullptr, MakeShared<FJsonValueString>(TEXT("candidate selector object")));
					TSharedPtr<FJsonValue> CandidateJson;
					UE::UAI::Contract::FDiagnostic JsonError;
					HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector Selector;
					HjsonWorkspace::FWorkspaceError SelectorError;
					if (!CanonicalJson::TryToJsonValue(*Candidate, CandidateJson, JsonError))
					{
						SetError(OutError, JsonError, SemanticId);
						if (!Operation.InputPointer.IsEmpty())
							BindGraphCompoundInputValueDiagnostic(OutError,
								(Operation.InputPointer == TEXT("/") ? FString() : Operation.InputPointer) + TEXT("/candidate"));
						else MarkGraphCompoundNativeDiagnostic(OutError);
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetTargetRef();
						return false;
					}
					if (!CandidateJson.IsValid() || CandidateJson->Type != EJson::Object)
						return Input.Fail(CandidateJson.IsValid()
								? TEXT("The PCG candidate selector must be an object.")
								: TEXT("Canonical conversion returned no PCG candidate selector value."),
							TEXT("/candidate"), CandidateJson.IsValid() ? CanonicalInputFact(Candidate) : nullptr,
							MakeShared<FJsonValueString>(TEXT("candidate selector object")));
					if (!HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(
							CandidateJson->AsObject(), TEXT("/candidate"), Selector, SelectorError))
					{
						SetError(OutError, SelectorError.GetDiagnostic(), SemanticId);
						if (!Operation.InputPointer.IsEmpty())
							UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError,
								Operation.InputPointer == TEXT("/") ? FString() : Operation.InputPointer);
						return false;
					}
					for (const UPCGNode* Node : Graph->GetNodes())
					{
						if (Node && Node->HasAuthoredTitle()
							&& Node->GetAuthoredTitleName().ToString()
								== AuthoredIdentity)
						{
							return FailOperation(TEXT("graph_compound_create_target_exists"),
								TEXT("A PCG node with this authored name already exists."), TEXT("node.authored_name"),
								MakeShared<FJsonValueString>(AuthoredIdentity), MakeShared<FJsonValueString>(TEXT("unused authored name")));
						}
					}
					if (FindObjectValue(*Operation.Value, TEXT("position"))
						&& !ValueInput(TEXT("graph_compound_pcg_graph_position_invalid")).ReadVector2(
							*Operation.Value, TEXT("position"), Operation.PositionX, Operation.PositionY)) return false;
					QueryOwners::Pcg::FPcgNodeCandidate ResolvedCandidate;
					QueryOwners::Pcg::FPcgNodeCandidateQueryError CandidateError;
					const bool bCandidateResolved = QueryOwners::Pcg::TryResolvePcgNodeCandidate(
							*Graph, Selector.Name, FString(),
							ResolvedCandidate, CandidateError, Selector.Select);
					if (!bCandidateResolved || !ResolvedCandidate.bSupported)
					{
						OutError = CandidateError.IsValid() ? CandidateError : ResolvedCandidate.SupportDiagnostic;
						if (OutError.IsEmpty())
						{
							OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_candidate_action_kind_unsupported"),
								bCandidateResolved ? TEXT("The selected PCG candidate has no supported native creation action.")
									: TEXT("The PCG candidate could not be resolved and returned no diagnostic."), TEXT("native_graph"));
							OutError.SourceKind = TEXT("native_result");
							OutError.CandidateName = Selector.Name;
						}
						if (!Operation.InputPointer.IsEmpty())
							UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError,
								(Operation.InputPointer == TEXT("/") ? FString() : Operation.InputPointer) + TEXT("/candidate"));
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetTargetRef();
						if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SemanticId;
						return false;
					}
					Operation.CandidateName = Selector.Name;
					Operation.CandidateSelect = MoveTemp(Selector.Select);
					Operation.FromItem = AuthoredIdentity;
					Operation.Kind = EPcgAiSchemaMutationKind::PcgGraphNode;
				}
				else if (Profile.GetCapabilityId()
					== TEXT("pcg.graph_instance.apply"))
				{
					UPCGGraphInstance* const Instance =
						Cast<UPCGGraphInstance>(&Target);
					if (!Instance)
					{
						OutError = NativeTypeFailure(TEXT("graph_compound_pcg_graph_instance_invalid"),
							TEXT("The native mutation target is not a PCGGraphInstance."),
							Request.GetTargetRef(), TEXT("target.class"), &Target, UPCGGraphInstance::StaticClass());
						OutError.OperationIndex = CurrentEditIndex;
						return false;
					}
					if (!Operation.Dependencies.IsEmpty())
						return FailOperation(TEXT("graph_compound_pcg_graph_instance_invalid"),
							TEXT("PCG graph instance mutations must not declare dependencies."), TEXT("dependencies.count"),
							MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()), MakeShared<FJsonValueNumber>(0));
					if (Edit.GetElementKind()
						== EGraphCompoundElementKind::Binding)
					{
						const auto Input = ValueInput(TEXT("graph_compound_pcg_graph_binding_invalid"));
						if (SemanticId != TEXT("binding:graph_source"))
							return FailOperation(TEXT("graph_compound_pcg_graph_binding_invalid"),
								TEXT("The PCG graph instance binding uses an unsupported identity."),
								TEXT("identity.semantic_id"), MakeShared<FJsonValueString>(SemanticId),
								MakeShared<FJsonValueString>(TEXT("binding:graph_source")));
						if (Operation.Value->GetKind() != EUeAgentValueKind::String)
							return Input.Fail(TEXT("The PCG graph source binding value must be a string."),
								FString(), CanonicalInputFact(Operation.Value.Get()), MakeShared<FJsonValueString>(TEXT("string graph reference")));
						const FString GraphRef = Operation.Value->GetText();
						Operation.GraphInterface = LoadObject<UPCGGraphInterface>(nullptr, *GraphRef);
						if (!Operation.GraphInterface)
							return Input.Fail(TEXT("The PCG graph source could not be loaded as a graph interface."),
								FString(), MakeShared<FJsonValueString>(GraphRef), MakeShared<FJsonValueString>(TEXT("loadable PCGGraphInterface")));
						if (Operation.GraphInterface->GetPathName() != GraphRef)
							return Input.Fail(TEXT("The loaded PCG graph path differs from the requested source."),
								FString(), MakeShared<FJsonValueString>(GraphRef), MakeShared<FJsonValueString>(Operation.GraphInterface->GetPathName()));
						if (!Instance->CanGraphInterfaceBeSet(Operation.GraphInterface))
							return Input.Fail(TEXT("The requested graph source would create a recursive PCG graph-instance hierarchy."),
								FString(), MakeShared<FJsonValueString>(GraphRef), MakeShared<FJsonValueString>(TEXT("graph source that does not recurse to this instance")));
						Operation.Kind =
							EPcgAiSchemaMutationKind::PcgGraphBinding;
					}
					else if (Edit.GetElementKind()
						== EGraphCompoundElementKind::Property)
					{
						if (!TryParsePcgDoubleParameterIdentity(SemanticId, Operation.ParameterGuid, Operation.ParameterName))
							return FailOperation(TEXT("graph_compound_pcg_parameter_invalid"),
								TEXT("The PCG double parameter identity is not in its canonical form."),
								TEXT("identity.semantic_id"), MakeShared<FJsonValueString>(SemanticId),
								MakeShared<FJsonValueString>(TEXT("property:parameter:<lowercase hyphenated GUID>:<non-None name>:double")));
						if (!TryCanonicalDouble(*Operation.Value, Operation.DoubleValue))
							return ValueInput(TEXT("graph_compound_pcg_parameter_invalid")).Fail(
								TEXT("The PCG double parameter value must be a finite number."),
								FString(), CanonicalInputFact(Operation.Value.Get()), MakeShared<FJsonValueString>(TEXT("finite number")));
						const FInstancedPropertyBag* const Bag =
							Instance->GetUserParametersStruct();
						const FPropertyBagPropertyDesc* const Desc = Bag
							? Bag->FindPropertyDescByID(Operation.ParameterGuid)
							: nullptr;
						if (!Desc)
							return FailOperation(TEXT("graph_compound_pcg_parameter_identity_invalid"),
								TEXT("The PCG parameter descriptor was not found by its GUID."),
								TEXT("parameters/") + Operation.ParameterGuid.ToString(EGuidFormats::DigitsWithHyphensLower),
								nullptr, MakeShared<FJsonValueString>(Operation.ParameterName.ToString()));
						if (Desc->Name != Operation.ParameterName)
							return FailOperation(TEXT("graph_compound_pcg_parameter_identity_invalid"),
								TEXT("The PCG parameter name differs from the descriptor found by GUID."),
								TEXT("parameter.name"), MakeShared<FJsonValueString>(Desc->Name.ToString()),
								MakeShared<FJsonValueString>(Operation.ParameterName.ToString()));
						if (Desc->ValueType != EPropertyBagPropertyType::Double)
							return FailOperation(TEXT("graph_compound_pcg_parameter_identity_invalid"),
								TEXT("The PCG parameter descriptor is not a double property."),
								TEXT("parameter.") + Operation.ParameterName.ToString() + TEXT(".type"),
								MakeShared<FJsonValueNumber>(static_cast<int32>(Desc->ValueType)),
								MakeShared<FJsonValueNumber>(static_cast<int32>(EPropertyBagPropertyType::Double)));
						Operation.Kind =
							EPcgAiSchemaMutationKind::PcgDoubleParameter;
					}
					else
					{
						return FailOperation(TEXT("graph_compound_pcg_graph_instance_invalid"),
							TEXT("PCG graph instance mutations support only binding and property elements."),
							TEXT("element_kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetElementKind())),
							MakeShared<FJsonValueString>(TEXT("binding or property")));
					}
				}
				else if (Profile.GetCapabilityId()
					== TEXT("pcg.graph_parameters.apply"))
				{
					UPCGGraph* const Graph = Cast<UPCGGraph>(&Target);
					if (!Graph)
					{
						OutError = NativeTypeFailure(TEXT("graph_compound_pcg_parameter_invalid"),
							TEXT("The native parameter mutation target is not a PCGGraph."),
							Request.GetTargetRef(), TEXT("target.class"), &Target, UPCGGraph::StaticClass());
						OutError.OperationIndex = CurrentEditIndex;
						return false;
					}
					if (Edit.GetElementKind() != EGraphCompoundElementKind::Property)
						return FailOperation(TEXT("graph_compound_pcg_parameter_invalid"),
							TEXT("PCG graph parameter mutations require property elements."), TEXT("element_kind"),
							MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetElementKind())), MakeShared<FJsonValueString>(TEXT("property")));
					if (!Operation.Dependencies.IsEmpty())
						return FailOperation(TEXT("graph_compound_pcg_parameter_invalid"),
							TEXT("PCG graph parameter mutations must not declare dependencies."), TEXT("dependencies.count"),
							MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()), MakeShared<FJsonValueNumber>(0));
					if (!TryParsePcgDoubleParameterIdentity(SemanticId, Operation.ParameterGuid, Operation.ParameterName))
						return FailOperation(TEXT("graph_compound_pcg_parameter_invalid"),
							TEXT("The PCG double parameter identity is not in its canonical form."),
							TEXT("identity.semantic_id"), MakeShared<FJsonValueString>(SemanticId),
							MakeShared<FJsonValueString>(TEXT("property:parameter:<lowercase hyphenated GUID>:<non-None name>:double")));
					if (!TryCanonicalDouble(*Operation.Value, Operation.DoubleValue))
						return ValueInput(TEXT("graph_compound_pcg_parameter_invalid")).Fail(
							TEXT("The PCG double parameter value must be a finite number."),
							FString(), CanonicalInputFact(Operation.Value.Get()), MakeShared<FJsonValueString>(TEXT("finite number")));
					const FInstancedPropertyBag* const Bag =
						Graph->GetUserParametersStruct();
					const FPropertyBagPropertyDesc* const Desc = Bag
						? Bag->FindPropertyDescByID(Operation.ParameterGuid)
						: nullptr;
					if (!Desc)
						return FailOperation(TEXT("graph_compound_pcg_parameter_identity_invalid"),
							TEXT("The PCG parameter descriptor was not found by its GUID."),
							TEXT("parameters/") + Operation.ParameterGuid.ToString(EGuidFormats::DigitsWithHyphensLower),
							nullptr, MakeShared<FJsonValueString>(Operation.ParameterName.ToString()));
					if (Desc->Name != Operation.ParameterName)
						return FailOperation(TEXT("graph_compound_pcg_parameter_identity_invalid"),
							TEXT("The PCG parameter name differs from the descriptor found by GUID."),
							TEXT("parameter.name"), MakeShared<FJsonValueString>(Desc->Name.ToString()),
							MakeShared<FJsonValueString>(Operation.ParameterName.ToString()));
					if (Desc->ValueType != EPropertyBagPropertyType::Double)
						return FailOperation(TEXT("graph_compound_pcg_parameter_identity_invalid"),
							TEXT("The PCG parameter descriptor is not a double property."),
							TEXT("parameter.") + Operation.ParameterName.ToString() + TEXT(".type"),
							MakeShared<FJsonValueNumber>(static_cast<int32>(Desc->ValueType)),
							MakeShared<FJsonValueNumber>(static_cast<int32>(EPropertyBagPropertyType::Double)));
					Operation.Kind =
						EPcgAiSchemaMutationKind::PcgDoubleParameter;
				}
				else if (Profile.GetCapabilityId()
					== TEXT("pcg.data_asset.apply"))
				{
					const FString DataTypeRef =
						UPCGPointData::StaticClass()->GetPathName();
					const FUeAgentCanonicalValue* const Content =
						FindObjectValue(*Operation.Value, TEXT("value"));
					FString ParsedDataTypeRef;
					FString ParsedSourceRef;
					const auto Input = ValueInput(TEXT("graph_compound_pcg_data_asset_input_invalid"));
					if (!Input.ReadString(*Operation.Value, TEXT("data_type_ref"), ParsedDataTypeRef)) return false;
					if (ParsedDataTypeRef != DataTypeRef)
						return ValueInput(TEXT("graph_compound_pcg_data_type_unsupported")).Fail(
							TEXT("This PCG data mutation supports only PCGPointData."),
							TEXT("/data_type_ref"), MakeShared<FJsonValueString>(ParsedDataTypeRef), MakeShared<FJsonValueString>(DataTypeRef));
					if (!Cast<UPCGDataAsset>(&Target))
					{
						OutError = NativeTypeFailure(TEXT("graph_compound_pcg_data_asset_input_invalid"),
							TEXT("The native mutation target is not a PCGDataAsset."),
							Request.GetTargetRef(), TEXT("target.class"), &Target, UPCGDataAsset::StaticClass());
						OutError.OperationIndex = CurrentEditIndex;
						return false;
					}
					if (Edit.GetElementKind() != EGraphCompoundElementKind::Property)
						return FailOperation(TEXT("graph_compound_pcg_data_asset_input_invalid"),
							TEXT("PCG data content mutations require property elements."), TEXT("element_kind"),
							MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetElementKind())), MakeShared<FJsonValueString>(TEXT("property")));
					if (!Operation.Dependencies.IsEmpty())
						return FailOperation(TEXT("graph_compound_pcg_data_asset_input_invalid"),
							TEXT("PCG data content must not declare dependencies."), TEXT("dependencies.count"),
							MakeShared<FJsonValueNumber>(Operation.Dependencies.Num()), MakeShared<FJsonValueNumber>(0));
					if (SemanticId != TEXT("property:data_content:") + DataTypeRef)
						return FailOperation(TEXT("graph_compound_pcg_data_asset_input_invalid"),
							TEXT("The PCG data content identity differs from its data type."),
							TEXT("identity.semantic_id"), MakeShared<FJsonValueString>(SemanticId),
							MakeShared<FJsonValueString>(TEXT("property:data_content:") + DataTypeRef));
					if (!Input.RequireFields(*Operation.Value, {TEXT("data_type_ref"), TEXT("source_ref"), TEXT("value")})
						|| !Input.ReadString(*Operation.Value, TEXT("source_ref"), ParsedSourceRef)) return false;
					if (!Content)
						return Input.Fail(TEXT("The required PCG data content value is missing."), TEXT("/value"),
							nullptr, MakeShared<FJsonValueString>(TEXT("object with point_count")));
					const auto ContentInput = Input.At(TEXT("/value"));
					if (!ContentInput.RequireFields(*Content, {TEXT("point_count")})
						|| !ContentInput.ReadInteger(*Content, TEXT("point_count"), Operation.PointCount)) return false;
					if (Operation.PointCount <= 0)
						return ContentInput.Fail(TEXT("PCG point data requires a positive point count."),
							TEXT("/point_count"), MakeShared<FJsonValueNumber>(Operation.PointCount),
							MakeShared<FJsonValueString>(TEXT("integer greater than zero")));
					Operation.SourceRef = FSoftObjectPath(ParsedSourceRef);
					if (!Operation.SourceRef.IsValid())
						return ValueInput(TEXT("graph_compound_pcg_data_asset_source_invalid")).Fail(
							TEXT("The PCG data source reference is not a valid soft object path."),
							TEXT("/source_ref"), MakeShared<FJsonValueString>(ParsedSourceRef),
							MakeShared<FJsonValueString>(TEXT("valid soft object path")));
					Operation.Kind = EPcgAiSchemaMutationKind::PcgDataAsset;
				}
				else
				{
					return FailOperation(TEXT("graph_compound_production_binding_unimplemented"),
						TEXT("No PCG/AI production mutation parser is implemented for this capability."),
						TEXT("capability_id"), MakeShared<FJsonValueString>(Profile.GetCapabilityId()),
						MakeShared<FJsonValueString>(TEXT("implemented PCG/AI mutation capability")));
				}
				OutOperations.Add(MoveTemp(Operation));
			}
			return true;
		}


		bool TryApplySchemaMutationOperations(
			UObject& Target,
			TArray<FPcgAiSchemaMutationOperation>& Operations,
			FPcgAiNodeGraphNativeHook& NativeHook,
			FString& OutRevision,
			FGraphCompoundRuntimeError& OutError)
		{
			OutRevision.Reset();
			bool bApplyMayHaveStarted = false;
			auto NumberFact = [](const double Value) -> TSharedPtr<FJsonValue>
			{
				if (FMath::IsFinite(Value)) return MakeShared<FJsonValueNumber>(Value);
				return MakeShared<FJsonValueString>(::LexToString(Value));
			};
			auto FailApply = [&](const TCHAR* Code, const FString& Reason, const FString& Field,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
				const FPcgAiSchemaMutationOperation* Operation = nullptr, const bool bMutationMayHaveStarted = false)
			{
				OutError = NativeFailure(Code, Reason, Target.GetPathName(), Field, MoveTemp(Actual), MoveTemp(Expected));
				OutError.MutationMayHaveStarted = bMutationMayHaveStarted || bApplyMayHaveStarted;
				if (Operation)
				{
					OutError.SubjectId = Operation->SemanticId;
					OutError.OperationIndex = static_cast<int32>(Operation - Operations.GetData());
				}
				return false;
			};
			auto FailNativeApply = [&](const TCHAR* Code, const FString& Reason, const FString& Field,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
				const FPcgAiSchemaMutationOperation& Operation, const FDiagnostic& NativeError)
			{
				if (!NativeError.IsEmpty())
				{
					SetError(OutError, NativeError, Operation.SemanticId);
					if (bApplyMayHaveStarted) OutError.MutationMayHaveStarted = true;
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target.GetPathName();
					if (!OutError.OperationIndex.IsSet())
						OutError.OperationIndex = static_cast<int32>(&Operation - Operations.GetData());
					return false;
				}
				return FailApply(Code, Reason, Field, MoveTemp(Actual), MoveTemp(Expected), &Operation, true);
			};
			if (UPCGComponent* const Component = Cast<UPCGComponent>(&Target))
			{
				if (Operations.Num() != 1)
					return FailApply(TEXT("graph_compound_pcg_component_binding_invalid"),
						TEXT("PCG component apply requires exactly one operation."), TEXT("operations.count"),
						MakeShared<FJsonValueNumber>(Operations.Num()), MakeShared<FJsonValueNumber>(1));
				if (Operations[0].Kind != EPcgAiSchemaMutationKind::PcgComponentBinding)
					return FailApply(TEXT("graph_compound_pcg_component_binding_invalid"),
						TEXT("PCG component apply requires a component graph-binding operation."), TEXT("operation.kind"),
						MakeShared<FJsonValueNumber>(static_cast<int32>(Operations[0].Kind)),
						MakeShared<FJsonValueString>(TEXT("PcgComponentBinding")), &Operations[0]);
				if (!Operations[0].GraphInterface)
					return FailApply(TEXT("graph_compound_pcg_component_binding_invalid"),
						TEXT("The PCG component binding operation has no graph interface."), TEXT("operation.graph_interface"),
						nullptr, MakeShared<FJsonValueString>(TEXT("PCGGraphInterface")), &Operations[0]);
				const FPcgAiSchemaMutationOperation& Operation = Operations[0];
				FPcgAiNodeGraphNativeMutationReceipt ApplyReceipt;
				FPcgAiNodeGraphBarrierReceipt BarrierReceipt;
				FPcgAiNodeGraphComponentBindingIdentity Identity;
				FString Evidence;
				FPcgAiNodeGraphAdapterError NativeError;
				bApplyMayHaveStarted = true;
				if (!NativeHook.TryBindAndCapturePcgComponent(
						*Component, *Operation.GraphInterface, Operation.NativeGenerationPolicy,
						ApplyReceipt, BarrierReceipt, Identity, OutRevision, Evidence, NativeError))
					return FailNativeApply(TEXT("graph_compound_pcg_component_refresh_incomplete"),
						TEXT("The native PCG component bind-and-capture call failed without a diagnostic."),
						TEXT("native_call.succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Operation, NativeError);
				if (!BarrierReceipt.IsComplete())
					return FailNativeApply(TEXT("graph_compound_pcg_component_refresh_incomplete"),
						TEXT("The native PCG component barrier did not complete."), TEXT("barrier.complete"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Operation, NativeError);
				if (BarrierReceipt.GetErrorCount() != 0)
					return FailNativeApply(TEXT("graph_compound_pcg_component_refresh_incomplete"),
						TEXT("The native PCG component barrier reported errors."), TEXT("barrier.error_count"),
						MakeShared<FJsonValueNumber>(BarrierReceipt.GetErrorCount()), MakeShared<FJsonValueNumber>(0), Operation, NativeError);
				if (!Component->GetGraphInstance())
					return FailNativeApply(TEXT("graph_compound_pcg_component_refresh_incomplete"),
						TEXT("The PCG component has no graph instance after binding."), TEXT("component.graph_instance"),
						nullptr, MakeShared<FJsonValueString>(TEXT("PCGGraphInstance")), Operation, NativeError);
				if (Component->GetGraphInstance()->Graph != Operation.GraphInterface)
					return FailNativeApply(TEXT("graph_compound_pcg_component_refresh_incomplete"),
						TEXT("The PCG component readback graph differs from the requested binding."), TEXT("component.graph"),
						Component->GetGraphInstance()->Graph ? MakeShared<FJsonValueString>(Component->GetGraphInstance()->Graph->GetPathName()) : TSharedPtr<FJsonValue>(),
						MakeShared<FJsonValueString>(Operation.GraphInterface->GetPathName()), Operation, NativeError);
				if (Component->GenerationTrigger != Operation.NativeGenerationTrigger)
					return FailNativeApply(TEXT("graph_compound_pcg_component_refresh_incomplete"),
						TEXT("The PCG component readback generation trigger differs from the requested trigger."),
						TEXT("component.generation_trigger"), MakeShared<FJsonValueNumber>(static_cast<int32>(Component->GenerationTrigger)),
						MakeShared<FJsonValueNumber>(static_cast<int32>(Operation.NativeGenerationTrigger)), Operation, NativeError);
				if (Identity.GetActorIdentity() != Operation.ActorRef)
					return FailNativeApply(TEXT("graph_compound_pcg_component_refresh_incomplete"),
						TEXT("The captured PCG component owner differs from the requested actor."), TEXT("identity.actor_ref"),
						MakeShared<FJsonValueString>(Identity.GetActorIdentity()), MakeShared<FJsonValueString>(Operation.ActorRef), Operation, NativeError);
				if (Identity.GetComponentIdentity() != Component->GetPathName())
					return FailNativeApply(TEXT("graph_compound_pcg_component_refresh_incomplete"),
						TEXT("The captured PCG component identity differs from the native target."), TEXT("identity.component_ref"),
						MakeShared<FJsonValueString>(Identity.GetComponentIdentity()), MakeShared<FJsonValueString>(Component->GetPathName()), Operation, NativeError);
				if (Identity.GetGraphIdentity() != Operation.GraphInterface->GetGraph()->GetPathName())
					return FailNativeApply(TEXT("graph_compound_pcg_component_refresh_incomplete"),
						TEXT("The captured PCG graph identity differs from the requested graph."), TEXT("identity.graph_ref"),
						MakeShared<FJsonValueString>(Identity.GetGraphIdentity()),
						MakeShared<FJsonValueString>(Operation.GraphInterface->GetGraph()->GetPathName()), Operation, NativeError);
				if (Identity.GetGenerationPolicy() != Operation.NativeGenerationPolicy)
					return FailNativeApply(TEXT("graph_compound_pcg_component_refresh_incomplete"),
						TEXT("The captured PCG generation policy differs from the requested native policy."), TEXT("identity.generation_policy"),
						MakeShared<FJsonValueString>(Identity.GetGenerationPolicy()),
						MakeShared<FJsonValueString>(Operation.NativeGenerationPolicy), Operation, NativeError);
				return true;
			}
			if (UPCGDataAsset* const DataAsset = Cast<UPCGDataAsset>(&Target))
			{
				if (Operations.Num() != 1)
					return FailApply(TEXT("graph_compound_pcg_data_asset_input_invalid"),
						TEXT("PCG data asset apply requires exactly one operation."), TEXT("operations.count"),
						MakeShared<FJsonValueNumber>(Operations.Num()), MakeShared<FJsonValueNumber>(1));
				if (Operations[0].Kind != EPcgAiSchemaMutationKind::PcgDataAsset)
					return FailApply(TEXT("graph_compound_pcg_data_asset_input_invalid"),
						TEXT("PCG data asset apply requires a data-content operation."), TEXT("operation.kind"),
						MakeShared<FJsonValueNumber>(static_cast<int32>(Operations[0].Kind)),
						MakeShared<FJsonValueString>(TEXT("PcgDataAsset")), &Operations[0]);
				FPcgAiSchemaMutationOperation& Operation = Operations[0];
				bApplyMayHaveStarted = true;
				UPCGPointData* const PointData = NewObject<UPCGPointData>(
					DataAsset, NAME_None, RF_Transactional);
				if (!PointData)
					return FailApply(TEXT("graph_compound_pcg_data_asset_allocation_failed"),
						TEXT("The PCG point-data object could not be allocated in the data asset."), TEXT("point_data"),
						nullptr, ClassPathValue(UPCGPointData::StaticClass()), &Operation);
				TArray<FPCGPoint> Points;
				Points.Reserve(Operation.PointCount);
				for (int32 Index = 0; Index < Operation.PointCount; ++Index)
				{
					Points.Emplace(
						FTransform(FVector(Index * 100.0, 0.0, 0.0)),
						1.0f, Index + 1);
				}
				PointData->SetPoints(Points);
				Operation.ContentRevision = TEXT("md5:") + FMD5::HashAnsiString(
					*FString::Printf(TEXT("%s|%s|%s|%d"),
						*DataAsset->GetPathName(),
						*UPCGPointData::StaticClass()->GetPathName(),
						*Operation.SourceRef.ToString(),
						Operation.PointCount));

				FPcgAiNodeGraphNativeMutationReceipt ApplyReceipt;
				FPcgAiNodeGraphBarrierReceipt BarrierReceipt;
				FPcgAiNodeGraphDataAssetIdentity Identity;
				FString Evidence;
				FPcgAiNodeGraphAdapterError NativeError;
				if (!NativeHook.TrySetAndCapturePcgDataAsset(
						*DataAsset, *PointData, Operation.SourceRef,
						ApplyReceipt, BarrierReceipt, Identity, Evidence, NativeError))
					return FailNativeApply(TEXT("graph_compound_pcg_data_asset_apply_failed"),
						TEXT("The native PCG data set-and-capture call failed without a diagnostic."),
						TEXT("native_call.succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Operation, NativeError);
				if (!BarrierReceipt.IsComplete())
					return FailNativeApply(TEXT("graph_compound_pcg_data_asset_apply_failed"),
						TEXT("The native PCG data asset barrier did not complete."), TEXT("barrier.complete"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Operation, NativeError);
				if (BarrierReceipt.GetErrorCount() != 0)
					return FailNativeApply(TEXT("graph_compound_pcg_data_asset_apply_failed"),
						TEXT("The native PCG data asset barrier reported errors."), TEXT("barrier.error_count"),
						MakeShared<FJsonValueNumber>(BarrierReceipt.GetErrorCount()), MakeShared<FJsonValueNumber>(0), Operation, NativeError);
				if (Identity.GetDataAssetIdentity() != DataAsset->GetPathName())
					return FailNativeApply(TEXT("graph_compound_pcg_data_asset_apply_failed"),
						TEXT("The captured PCG data asset identity differs from the native target."), TEXT("identity.data_asset_ref"),
						MakeShared<FJsonValueString>(Identity.GetDataAssetIdentity()),
						MakeShared<FJsonValueString>(DataAsset->GetPathName()), Operation, NativeError);
				if (Identity.GetDataTypeIdentity() != UPCGPointData::StaticClass()->GetPathName())
					return FailNativeApply(TEXT("graph_compound_pcg_data_asset_apply_failed"),
						TEXT("The captured PCG data type differs from PCGPointData."), TEXT("identity.data_type_ref"),
						MakeShared<FJsonValueString>(Identity.GetDataTypeIdentity()), ClassPathValue(UPCGPointData::StaticClass()), Operation, NativeError);
				if (Identity.GetSourceIdentity() != Operation.SourceRef.ToString())
					return FailNativeApply(TEXT("graph_compound_pcg_data_asset_apply_failed"),
						TEXT("The captured PCG data source differs from the requested source."), TEXT("identity.source_ref"),
						MakeShared<FJsonValueString>(Identity.GetSourceIdentity()),
						MakeShared<FJsonValueString>(Operation.SourceRef.ToString()), Operation, NativeError);
				DataAsset->Data.TaggedData[0].Tags = {
					PcgDataContentRevisionTagPrefix
						+ Operation.ContentRevision};
				DataAsset->Data.ComputeCrcs(true);
				OutRevision = Operation.ContentRevision;
				return true;
			}
			if (UPCGGraphInstance* const Instance =
					Cast<UPCGGraphInstance>(&Target))
			{
				UPCGGraphInterface* RequestedGraph = nullptr;
				for (const FPcgAiSchemaMutationOperation& Operation : Operations)
				{
					if (Operation.Kind
						== EPcgAiSchemaMutationKind::PcgGraphBinding)
					{
						if (RequestedGraph)
							return FailApply(TEXT("graph_compound_pcg_graph_binding_invalid"),
								TEXT("PCG graph instance apply contains more than one graph binding."),
								TEXT("operation.graph_binding"), MakeShared<FJsonValueString>(RequestedGraph->GetPathName()),
								MakeShared<FJsonValueString>(TEXT("exactly one graph binding")), &Operation);
						if (!Operation.GraphInterface)
							return FailApply(TEXT("graph_compound_pcg_graph_binding_invalid"),
								TEXT("The PCG graph binding operation has no graph interface."),
								TEXT("operation.graph_interface"), nullptr,
								MakeShared<FJsonValueString>(TEXT("PCGGraphInterface")), &Operation);
						RequestedGraph = Operation.GraphInterface;
					}
					else if (Operation.Kind
						!= EPcgAiSchemaMutationKind::PcgDoubleParameter)
					{
						return FailApply(TEXT("graph_compound_pcg_graph_instance_invalid"),
							TEXT("PCG graph instance apply supports only graph-binding and double-parameter operations."),
							TEXT("operation.kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Operation.Kind)),
							MakeShared<FJsonValueString>(TEXT("PcgGraphBinding or PcgDoubleParameter")), &Operation);
					}
				}
				if (!RequestedGraph)
					return FailApply(TEXT("graph_compound_pcg_graph_binding_invalid"),
						TEXT("PCG graph instance apply has no requested graph binding."), TEXT("requested_graph"),
						nullptr, MakeShared<FJsonValueString>(TEXT("graph binding")));
				if (!Instance->CanGraphInterfaceBeSet(RequestedGraph))
					return FailApply(TEXT("graph_compound_pcg_graph_binding_invalid"),
						TEXT("The requested graph source would create a recursive PCG graph-instance hierarchy."),
						TEXT("requested_graph"), MakeShared<FJsonValueString>(RequestedGraph->GetPathName()),
						MakeShared<FJsonValueString>(TEXT("graph source that does not recurse to this instance")));
				bApplyMayHaveStarted = true;
				Instance->Modify();
				Instance->SetGraph(RequestedGraph);
				if (Instance->Graph != RequestedGraph)
					return FailApply(TEXT("graph_compound_pcg_graph_binding_failed"),
						TEXT("The PCG graph instance readback differs from the graph passed to SetGraph."), TEXT("graph"),
						Instance->Graph ? MakeShared<FJsonValueString>(Instance->Graph->GetPathName()) : TSharedPtr<FJsonValue>(),
						MakeShared<FJsonValueString>(RequestedGraph->GetPathName()), nullptr, true);
				TArray<FString> RevisionLines = {
					TEXT("binding:graph_source|") + RequestedGraph->GetPathName()};
				for (const FPcgAiSchemaMutationOperation& Operation : Operations)
				{
					if (Operation.Kind
						!= EPcgAiSchemaMutationKind::PcgDoubleParameter)
					{
						continue;
					}
					FInstancedPropertyBag& OverrideBag =
						Instance->ParametersOverrides.Parameters;
					const FPropertyBagPropertyDesc* const Desc =
						OverrideBag.FindPropertyDescByID(Operation.ParameterGuid);
					const UPropertyBag* const BagStruct =
						OverrideBag.GetPropertyBagStruct();
					const FProperty* const Property = BagStruct
						? BagStruct->FindPropertyByName(Operation.ParameterName)
						: nullptr;
					if (!Desc)
						return FailApply(TEXT("graph_compound_pcg_parameter_identity_invalid"),
							TEXT("The PCG override parameter descriptor was not found by GUID."),
							TEXT("parameters/") + Operation.ParameterGuid.ToString(EGuidFormats::DigitsWithHyphensLower),
							nullptr, MakeShared<FJsonValueString>(Operation.ParameterName.ToString()), &Operation, true);
					if (Desc->Name != Operation.ParameterName)
						return FailApply(TEXT("graph_compound_pcg_parameter_identity_invalid"),
							TEXT("The PCG override parameter name differs from the descriptor found by GUID."),
							TEXT("parameter.name"), MakeShared<FJsonValueString>(Desc->Name.ToString()),
							MakeShared<FJsonValueString>(Operation.ParameterName.ToString()), &Operation, true);
					if (Desc->ValueType != EPropertyBagPropertyType::Double)
						return FailApply(TEXT("graph_compound_pcg_parameter_identity_invalid"),
							TEXT("The PCG override parameter descriptor is not a double property."),
							TEXT("parameter.") + Operation.ParameterName.ToString() + TEXT(".type"),
							MakeShared<FJsonValueNumber>(static_cast<int32>(Desc->ValueType)),
							MakeShared<FJsonValueNumber>(static_cast<int32>(EPropertyBagPropertyType::Double)), &Operation, true);
					if (!Property)
						return FailApply(TEXT("graph_compound_pcg_parameter_identity_invalid"),
							TEXT("The PCG override property was not found by name in the property bag."),
							TEXT("parameter.property"), nullptr,
							MakeShared<FJsonValueString>(Operation.ParameterName.ToString()), &Operation, true);
					Instance->UpdatePropertyOverride(Property, true);
					const EPropertyBagResult WriteResult = Instance->SetGraphParameter<double>(
						Operation.ParameterName, Operation.DoubleValue);
					if (WriteResult != EPropertyBagResult::Success)
						return FailApply(TEXT("graph_compound_pcg_parameter_write_failed"),
							TEXT("The native PCG double-parameter setter returned a failure result."),
							TEXT("parameter.") + Operation.ParameterName.ToString() + TEXT(".write_result"),
							MakeShared<FJsonValueNumber>(static_cast<int32>(WriteResult)),
							MakeShared<FJsonValueNumber>(static_cast<int32>(EPropertyBagResult::Success)), &Operation, true);
					const TValueOrError<double, EPropertyBagResult> Readback =
						Instance->GetGraphParameter<double>(Operation.ParameterName);
					if (!Instance->ParametersOverrides.IsPropertyOverridden(Property))
						return FailApply(TEXT("graph_compound_pcg_parameter_refresh_incomplete"),
							TEXT("The PCG parameter was not marked overridden after the update."),
							TEXT("parameter.") + Operation.ParameterName.ToString() + TEXT(".overridden"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), &Operation, true);
					if (!Readback.IsValid())
						return FailApply(TEXT("graph_compound_pcg_parameter_refresh_incomplete"),
							TEXT("The native PCG double-parameter getter returned a failure result."),
							TEXT("parameter.") + Operation.ParameterName.ToString() + TEXT(".read_result"),
							MakeShared<FJsonValueNumber>(static_cast<int32>(Readback.GetError())),
							MakeShared<FJsonValueNumber>(static_cast<int32>(EPropertyBagResult::Success)), &Operation, true);
					if (!FMath::IsNearlyEqual(Readback.GetValue(), Operation.DoubleValue))
						return FailApply(TEXT("graph_compound_pcg_parameter_refresh_incomplete"),
							TEXT("The PCG parameter readback differs from the requested value."),
							TEXT("parameter.") + Operation.ParameterName.ToString() + TEXT(".value"),
							NumberFact(Readback.GetValue()), NumberFact(Operation.DoubleValue), &Operation, true);
					RevisionLines.Add(FString::Printf(TEXT("%s|%.17g"),
						*Operation.SemanticId, Operation.DoubleValue));
				}
				RevisionLines.Sort();
				OutRevision = TEXT("md5:") + FMD5::HashAnsiString(
					*FString::Join(RevisionLines, TEXT("\n")));
				return true;
			}

			if (UPCGGraph* const Graph = Cast<UPCGGraph>(&Target))
			{
				for (FPcgAiSchemaMutationOperation& Operation : Operations)
				{
					if (Operation.Kind == EPcgAiSchemaMutationKind::PcgGraphNode)
					{
						UPCGNode* Node = nullptr;
						QueryOwners::Pcg::FPcgNodeCandidate ResolvedCandidate;
						QueryOwners::Pcg::FPcgNodeCandidateQueryError CandidateError;
						bApplyMayHaveStarted = true;
						const bool bCandidatePerformed = QueryOwners::Pcg::TryPerformPcgNodeCandidate(
							*Graph, Operation.CandidateName, FString(),
							FVector2f(Operation.PositionX, Operation.PositionY),
							Node, ResolvedCandidate, CandidateError, Operation.CandidateSelect);
						if (!bCandidatePerformed || !Node)
						{
							OutError = CandidateError.IsValid() ? CandidateError : UE::UAI::Contract::FDiagnostic::Failure(
								TEXT("graph_compound_pcg_graph_node_create_failed"),
								bCandidatePerformed
									? TEXT("The PCG candidate action completed without returning a node.")
									: TEXT("The PCG candidate action failed without providing a diagnostic."), TEXT("native_graph"));
							if (!CandidateError.IsValid())
							{
								OutError.SourceKind = TEXT("native_result");
								OutError.ActualValuePresent = false;
								OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("PCG node"));
								OutError.MutationMayHaveStarted = true;
							}
							if (!Operation.InputPointer.IsEmpty() && OutError.SourceKind.IsEmpty())
							{
								const FString RootPointer = Operation.InputPointer == TEXT("/") ? FString() : Operation.InputPointer;
								if (OutError.PropertyPath == TEXT("x") || OutError.PropertyPath == TEXT("y"))
								{
									OutError.Source.JsonPath = OutError.PropertyPath == TEXT("x") ? TEXT("/0") : TEXT("/1");
									UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, RootPointer + TEXT("/position"));
								}
								else UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, RootPointer + TEXT("/candidate"));
								OutError.SourceKind = TEXT("related_input");
							}
							if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Operation.TargetPath;
							if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Operation.SemanticId;
							return false;
						}
						Node->Modify();
						Node->NodeTitle = FName(*Operation.FromItem);
						Operation.CreatedNodeClass = Node->GetClass();
						const UPCGSettings* const Settings = Node->GetSettings();
						Operation.SettingsClass = Settings
							? Settings->GetClass() : nullptr;
						Graph->MarkPackageDirty();
					}
					else if (Operation.Kind
						== EPcgAiSchemaMutationKind::PcgDoubleParameter)
					{
						FPcgAiNodeGraphNativeMutationReceipt ApplyReceipt;
						FPcgAiNodeGraphBarrierReceipt BarrierReceipt;
						FPcgAiNodeGraphParameterIdentity Identity;
						FString ParameterRevision;
						FString Evidence;
						FPcgAiNodeGraphAdapterError NativeError;
						bApplyMayHaveStarted = true;
						if (!NativeHook.TrySetPcgDoubleParameter(
								*Graph, Operation.ParameterGuid, Operation.ParameterName, Operation.DoubleValue,
								ApplyReceipt, BarrierReceipt, Identity, ParameterRevision, Evidence, NativeError))
							return FailNativeApply(TEXT("graph_compound_pcg_parameter_write_failed"),
								TEXT("The native PCG parameter setter failed without a diagnostic."),
								TEXT("parameter.") + Operation.ParameterName.ToString() + TEXT(".native_call.succeeded"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Operation, NativeError);
						if (!BarrierReceipt.IsComplete())
							return FailNativeApply(TEXT("graph_compound_pcg_parameter_write_failed"),
								TEXT("The native PCG parameter barrier did not complete."),
								TEXT("parameter.") + Operation.ParameterName.ToString() + TEXT(".barrier.complete"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Operation, NativeError);
						if (BarrierReceipt.GetErrorCount() != 0)
							return FailNativeApply(TEXT("graph_compound_pcg_parameter_write_failed"),
								TEXT("The native PCG parameter barrier reported errors."),
								TEXT("parameter.") + Operation.ParameterName.ToString() + TEXT(".barrier.error_count"),
								MakeShared<FJsonValueNumber>(BarrierReceipt.GetErrorCount()), MakeShared<FJsonValueNumber>(0), Operation, NativeError);
					}
					else
					{
						return FailApply(TEXT("graph_compound_pcg_graph_operation_invalid"),
							TEXT("PCG graph apply supports only node-creation and double-parameter operations."),
							TEXT("operation.kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Operation.Kind)),
							MakeShared<FJsonValueString>(TEXT("PcgGraphNode or PcgDoubleParameter")), &Operation);
					}
				}
				FPcgAiNodeGraphBarrierReceipt BarrierReceipt;
				FPcgAiNodeGraphTopology Topology;
				TArray<FPcgAiNodeGraphParameterIdentity> Parameters;
				FString Evidence;
				FPcgAiNodeGraphAdapterError NativeError;
				if (!NativeHook.TryCompileAndCapturePcgGraph(
						*Graph, BarrierReceipt, Topology, Parameters,
						OutRevision, Evidence, NativeError))
				{
					SetError(OutError, MoveTemp(NativeError), Graph->GetPathName());
					return false;
				}
				return true;
			}


			return FailApply(TEXT("graph_compound_pcg_ai_target_invalid"),
				TEXT("The native target class has no supported PCG schema mutation executor."), TEXT("target.class"),
				ClassPathValue(Target.GetClass()), MakeShared<FJsonValueString>(TEXT("PCGComponent, PCGDataAsset, PCGGraphInstance or PCGGraph")));
		}

		bool TryBuildSchemaMutationAggregate(
			const TArray<FPcgAiSchemaMutationOperation>& Operations,
			const TMap<FString, FUeAgentCanonicalValueRef>* ActualValues,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FVerificationError& OutError)
		{
			auto FailAggregate = [&](const TCHAR* Code, const FString& Reason, const FString& Field,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
				const FPcgAiSchemaMutationOperation& Operation)
			{
				OutError = NativeFailure(Code, Reason, Operation.TargetPath, Field, MoveTemp(Actual), MoveTemp(Expected));
				OutError.Stage = TEXT("verification");
				OutError.SubjectId = Operation.SemanticId;
				OutError.CandidateName = Operation.SemanticId;
				OutError.OperationIndex = static_cast<int32>(&Operation - Operations.GetData());
				return false;
			};
			auto PreserveAggregateError = [&](const FDiagnostic& Cause, const FPcgAiSchemaMutationOperation& Operation)
			{
				OutError = Cause;
				MarkGraphCompoundNativeDiagnostic(OutError);
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Operation.TargetPath;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Operation.SemanticId;
				if (!OutError.OperationIndex.IsSet())
					OutError.OperationIndex = static_cast<int32>(&Operation - Operations.GetData());
				return false;
			};
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
			for (const FPcgAiSchemaMutationOperation& Operation : Operations)
			{
				const FUeAgentCanonicalValueRef* const Actual = ActualValues
					? ActualValues->Find(Operation.SemanticId) : nullptr;
				if (ActualValues && !Actual)
					return FailAggregate(TEXT("graph_compound_pcg_ai_readback_missing"),
						TEXT("Independent readback did not return a value for this mutation."),
						TEXT("readback.value"), nullptr, CanonicalInputFact(Operation.Value.Get()), Operation);
				FUeAgentCanonicalValuePtr Expected = Operation.Value;
				if (!ActualValues
					&& Operation.Kind == EPcgAiSchemaMutationKind::PcgDataAsset)
				{
					FUeAgentCanonicalValuePtr RevisionValue;
					FUeAgentStateSemanticError StateError;
					if (Operation.ContentRevision.IsEmpty())
						return FailAggregate(TEXT("graph_compound_pcg_ai_readback_expected_invalid"),
							TEXT("The PCG data operation has no content revision for expected readback."),
							TEXT("content_revision"), nullptr,
							MakeShared<FJsonValueString>(TEXT("non-empty content revision")), Operation);
					if (!FUeAgentCanonicalValue::TryString(
							Operation.ContentRevision, Resources, RevisionValue, StateError))
						return PreserveAggregateError(StateError, Operation);
					TArray<TPair<FString, FUeAgentCanonicalValueRef>>
						ExpectedItems = Operation.Value->GetObjectItems();
					ExpectedItems.Emplace(
						TEXT("content_revision"), RevisionValue.ToSharedRef());
					if (!FUeAgentCanonicalValue::TryObject(
							ExpectedItems, Resources, Expected, StateError))
						return PreserveAggregateError(StateError, Operation);
				}
				if (!ActualValues
					&& Operation.Kind == EPcgAiSchemaMutationKind::PcgGraphNode)
				{
					if (!Operation.CreatedNodeClass)
						return FailAggregate(TEXT("graph_compound_pcg_ai_readback_expected_invalid"),
							TEXT("The created PCG node class is missing from the mutation result."),
							TEXT("created_node.class"), nullptr, ClassPathValue(UPCGNode::StaticClass()), Operation);
					if (!Operation.SettingsClass)
						return FailAggregate(TEXT("graph_compound_pcg_ai_readback_expected_invalid"),
							TEXT("The created PCG node settings class is missing from the mutation result."),
							TEXT("created_node.settings_class"), nullptr, ClassPathValue(UPCGSettings::StaticClass()), Operation);
					const TSharedRef<FJsonObject> Definition =
						MakeShared<FJsonObject>();
					Definition->SetStringField(
						TEXT("carrier_type_ref"),
						Operation.CreatedNodeClass->GetPathName());
					Definition->SetStringField(
						TEXT("settings_type_ref"),
						Operation.SettingsClass->GetPathName());
					UE::UAI::Contract::FDiagnostic CanonicalError;
					if (!CanonicalJson::TryFromJsonValue(
							MakeShared<FJsonValueObject>(Definition), Resources, Expected, CanonicalError))
						return PreserveAggregateError(CanonicalError, Operation);
				}
				else if (!ActualValues
					&& (Operation.Kind
							== EPcgAiSchemaMutationKind::BehaviorComposite
						|| Operation.Kind
							== EPcgAiSchemaMutationKind::BehaviorTask)
					&& (!Operation.CandidateName.IsEmpty()
						|| !Operation.CandidateId.IsEmpty()))
				{
					if (!Operation.PayloadClass)
						return FailAggregate(TEXT("graph_compound_pcg_ai_readback_expected_invalid"),
							TEXT("The BehaviorTree candidate operation has no payload class for expected readback."),
							TEXT("operation.payload_class"), nullptr,
							MakeShared<FJsonValueString>(TEXT("resolved BehaviorTree node payload class")), Operation);
					const TSharedRef<FJsonObject> Definition =
						MakeShared<FJsonObject>();
					Definition->SetStringField(
						TEXT("carrier_type_ref"),
						Operation.Kind
							== EPcgAiSchemaMutationKind::BehaviorComposite
								? UBehaviorTreeGraphNode_Composite::StaticClass()
									->GetPathName()
								: UBehaviorTreeGraphNode_Task::StaticClass()
									->GetPathName());
					Definition->SetStringField(
						TEXT("payload_type_ref"),
						Operation.PayloadClass->GetPathName());
					UE::UAI::Contract::FDiagnostic CanonicalError;
					if (!CanonicalJson::TryFromJsonValue(
							MakeShared<FJsonValueObject>(Definition), Resources, Expected, CanonicalError))
						return PreserveAggregateError(CanonicalError, Operation);
				}
				Items.Emplace(Operation.SemanticId,
					Actual ? *Actual : Expected.ToSharedRef());
			}
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryObject(Items, Resources, OutValue, StateError))
			{
				OutError = StateError;
				MarkGraphCompoundNativeDiagnostic(OutError);
				return false;
			}
			return true;
		}

		const FString SchemaMutationReadbackOracleId =
			TEXT("uai.readback.graph_compound.pcg_ai.schema_mutation");

		class FPcgAiSchemaMutationReadbackOracle final
			: public IIndependentReadbackOracle
		{
		public:
			FPcgAiSchemaMutationReadbackOracle(
				FPcgAnimationGraphRouteProfile InProfile,
				FString InTargetRef,
				TArray<FPcgAiSchemaMutationOperation> InOperations,
				bool bInUnloadBeforeRead = true)
				: Profile(MoveTemp(InProfile))
				, TargetRef(MoveTemp(InTargetRef))
				, Operations(MoveTemp(InOperations))
				, bUnloadBeforeRead(bInUnloadBeforeRead)
			{
			}

			const FString& GetOracleId() const override
			{
				return SchemaMutationReadbackOracleId;
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
				if (bUnloadBeforeRead && !UeAgentAuthoringOperationApply::TryReloadPersistedPackage(TargetRef, Detail))
				{
					OutError = Detail.IsEmpty()
						? NativeFailure(TEXT("graph_compound_pcg_ai_asset_unload_failed"),
							TEXT("The persisted package reload failed without providing a diagnostic."),
							TargetRef, TEXT("package.reload.succeeded"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true))
						: Detail;
					return false;
				}
				TArray<FCapturedMutationValue> Captured;
				FString Generation;
				FGraphCompoundRuntimeError RuntimeError;
				if (!TryCaptureMutationValues(
						Profile, TargetRef, Resources,
						Captured, Generation, RuntimeError))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(RuntimeError, RuntimeError.InternalContext);
					return true;
				}
				TMap<FString, FUeAgentCanonicalValueRef> ActualValues;
				for (const FCapturedMutationValue& Value : Captured)
				{
					ActualValues.Add(Value.SemanticId, Value.Value.ToSharedRef());
				}
				FUeAgentCanonicalValuePtr Aggregate;
				if (!TryBuildSchemaMutationAggregate(
						Operations, &ActualValues, Resources,
						Aggregate, OutError))
				{
					return false;
				}
				OutObservation = FIndependentReadbackObservation::Available(
					Aggregate.ToSharedRef(),
					TEXT("reload:") + Request.GetSubjectRef()
						+ TEXT(":") + Generation);
				return true;
			}

		private:
			FPcgAnimationGraphRouteProfile Profile;
			FString TargetRef;
			TArray<FPcgAiSchemaMutationOperation> Operations;
			bool bUnloadBeforeRead = true;
		};

		const FString PcgComponentMutationReadbackOracleId =
			TEXT("uai.readback.graph_compound.pcg_ai.pcg_component_mutation");

		class FPcgAiPcgComponentMutationReadbackOracle final
			: public IIndependentReadbackOracle
		{
		public:
			FPcgAiPcgComponentMutationReadbackOracle(
				FPcgAnimationGraphRouteProfile InProfile,
				FString InTargetRef,
				TArray<FPcgAiSchemaMutationOperation> InOperations)
				: Profile(MoveTemp(InProfile))
				, TargetRef(MoveTemp(InTargetRef))
				, Operations(MoveTemp(InOperations))
			{
			}

			const FString& GetOracleId() const override
			{
				return PcgComponentMutationReadbackOracleId;
			}

			bool TryRead(
				const FIndependentReadbackRequest& Request,
				FUeAgentResourceTracker& Resources,
				FIndependentReadbackObservation& OutObservation,
				FVerificationError& OutError) override
			{
				OutObservation = FIndependentReadbackObservation();
				OutError = FVerificationError();
				TArray<FCapturedMutationValue> Captured;
				FString Generation;
				FGraphCompoundRuntimeError RuntimeError;
				if (!FindExactLoadedPcgComponent(TargetRef)
					|| !TryCaptureMutationValues(
						Profile, TargetRef, Resources,
						Captured, Generation, RuntimeError))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						UE::UAI::Contract::FDiagnostic::WrapFailure(
							RuntimeError.IsValid() ? RuntimeError.Code
								: TEXT("graph_compound_pcg_ai_target_invalid"),
							RuntimeError, TEXT("independent_readback")));
					return true;
				}
				TMap<FString, FUeAgentCanonicalValueRef> ActualValues;
				for (const FCapturedMutationValue& Value : Captured)
				{
					ActualValues.Add(
						Value.SemanticId, Value.Value.ToSharedRef());
				}
				FUeAgentCanonicalValuePtr Aggregate;
				if (!TryBuildSchemaMutationAggregate(
						Operations, &ActualValues, Resources,
						Aggregate, OutError))
				{
					return false;
				}
				OutObservation = FIndependentReadbackObservation::Available(
					Aggregate.ToSharedRef(),
					TEXT("session-readback:") + Request.GetSubjectRef()
						+ TEXT(":") + Generation);
				return true;
			}

		private:
			FPcgAnimationGraphRouteProfile Profile;
			FString TargetRef;
			TArray<FPcgAiSchemaMutationOperation> Operations;
		};

		class FAssetCreateReadbackOracle final
			: public IIndependentReadbackOracle
		{
		public:
			FAssetCreateReadbackOracle(
				FString InTargetRef,
				FString InTargetKind,
				FString InOracleId,
				UClass& InAssetClass)
				: TargetRef(MoveTemp(InTargetRef))
				, TargetKind(MoveTemp(InTargetKind))
				, OracleId(MoveTemp(InOracleId))
				, AssetClass(&InAssetClass)
			{
			}

			const FString& GetOracleId() const override
			{
				return OracleId;
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
					OutError = Detail.IsEmpty()
						? NativeFailure(TEXT("graph_compound_pcg_ai_asset_unload_failed"),
							TEXT("The persisted package reload failed without providing a diagnostic."),
							TargetRef, TEXT("package.reload.succeeded"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true))
						: Detail;
					return false;
				}
				UObject* const Asset = LoadObject<UObject>(
					nullptr, *ObjectPath(TargetRef));
				if (!Asset)
				{
					OutError = NativeFailure(TEXT("graph_compound_pcg_ai_asset_reload_failed"),
						TEXT("The persisted asset could not be loaded for independent readback."),
						TargetRef, TEXT("asset"), nullptr, ClassPathValue(AssetClass));
					OutError.Stage = TEXT("verification");
					return false;
				}
				auto ObserveMismatch = [&](const FString& Reason, const FString& Field,
					TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
				{
					FDiagnostic Diagnostic = NativeFailure(TEXT("graph_compound_pcg_ai_asset_reload_mismatch"),
						Reason, TargetRef, Field, MoveTemp(Actual), MoveTemp(Expected));
					Diagnostic.Stage = TEXT("independent_readback");
					OutObservation = FIndependentReadbackObservation::Unavailable(MoveTemp(Diagnostic), TEXT("reload:") + TargetRef);
					return true;
				};
				if (Asset->GetClass() != AssetClass)
					return ObserveMismatch(TEXT("The reloaded asset class differs from the expected class."),
						TEXT("asset.class"), ClassPathValue(Asset->GetClass()), ClassPathValue(AssetClass));
				if (Asset->GetOutermost()->GetName() != TargetRef)
					return ObserveMismatch(TEXT("The reloaded asset belongs to a different package."),
						TEXT("asset.package"), MakeShared<FJsonValueString>(Asset->GetOutermost()->GetName()),
						MakeShared<FJsonValueString>(TargetRef));
				if (Asset->GetOutermost()->IsDirty())
					return ObserveMismatch(TEXT("The reloaded asset package is dirty."),
						TEXT("asset.package.dirty"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));

				const TSharedRef<FJsonObject> Definition =
					MakeShared<FJsonObject>();
				Definition->SetStringField(
					TEXT("asset_kind"), TargetKind);
				FUeAgentCanonicalValuePtr Actual;
				UE::UAI::Contract::FDiagnostic CanonicalError;
				if (!CanonicalJson::TryFromJsonValue(
						MakeShared<FJsonValueObject>(Definition),
						Resources, Actual, CanonicalError)
					|| !Actual.IsValid())
				{
					OutError = CanonicalError;
					return false;
				}
				OutObservation = FIndependentReadbackObservation::Available(
					Actual.ToSharedRef(),
					TEXT("reload:") + Request.GetSubjectRef() + TEXT(":")
						+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
				return true;
			}

		private:
			FString TargetRef;
			FString TargetKind;
			FString OracleId;
			UClass* AssetClass = nullptr;
		};

		bool TrySerializeResult(
			const FString& TargetRef,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("target_ref"), TargetRef);
			FString Canonical;
			UE::UAI::Contract::FDiagnostic JsonError;
			if (!Json::TryCanonicalize(Root, Canonical, JsonError))
			{
				SetError(OutError, JsonError, TargetRef);
				return false;
			}
			OutResult = FUeAgentContractValue(MoveTemp(Canonical));
			return true;
		}

		bool TryRestorePcgComponentMutation(
			UPCGComponent& Component,
			UPCGGraphInterface* BaselineGraph,
			const EPCGComponentGenerationTrigger BaselinePolicy,
			const bool bBaselinePackageDirty,
			UE::UAI::Contract::FDiagnostic& OutDetail)
		{
			OutDetail.Reset();
			Component.SetGraph(BaselineGraph);
			Component.GenerationTrigger = BaselinePolicy;
			UPackage* const Package = Component.GetOutermost();
			if (Package)
			{
				Package->SetDirtyFlag(bBaselinePackageDirty);
			}
			const UPCGGraphInterface* const RestoredGraph =
				Component.GetGraphInstance()
					? Component.GetGraphInstance()->Graph.Get() : nullptr;
			if (RestoredGraph != BaselineGraph
				|| Component.GenerationTrigger != BaselinePolicy
				|| (Package && Package->IsDirty() != bBaselinePackageDirty))
			{
				OutDetail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_native_readback_failed"), TEXT("pcg component rollback readback mismatch"), TEXT("native_readback"));
				return false;
			}
			return true;
		}

		bool TryExecutePcgComponentMutation(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FGraphCompoundPcgAiOperationRequest& Request,
			const FPlanningPreflightPlanRef& Plan,
			const FUeAgentCallerContext& CallerContext,
			TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
			FPcgAiNodeGraphNativeHook& NativeHook,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			if (!IsInGameThread() || Cancellation->IsCancelled())
			{
				const bool bCancelled = Cancellation->IsCancelled();
				OutError = NativeFailure(bCancelled
						? TEXT("graph_compound_pcg_ai_cancelled") : TEXT("graph_compound_pcg_ai_thread_invalid"),
					bCancelled ? TEXT("The PCG component mutation was cancelled before execution.")
						: TEXT("PCG component mutation must execute on the game thread."),
					Request.GetTargetRef(), bCancelled ? TEXT("cancellation.requested") : TEXT("execution.game_thread"),
					MakeShared<FJsonValueBoolean>(bCancelled), MakeShared<FJsonValueBoolean>(!bCancelled));
				OutError.SubjectId = Profile.GetCapabilityId();
				return false;
			}
			UPCGComponent* const Component =
				FindExactLoadedPcgComponent(Request.GetTargetRef());
			if (!Component)
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
					TEXT("No loaded PCG component exactly matches the requested target reference."),
					Request.GetTargetRef(), TEXT("target"), nullptr, ClassPathValue(UPCGComponent::StaticClass()));
				return false;
			}

			TArray<FPcgAiSchemaMutationOperation> Operations;
			if (!TryParseSchemaMutationOperations(
					Profile, Request, Plan, *Component, Operations, OutError))
			{
				return false;
			}
			FUeAgentCanonicalSnapshotPtr Current;
			if (!TryCaptureMutationSnapshot(
					Profile, Request.GetTarget(), *Resources, Current, OutError))
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

			UPCGGraphInterface* const BaselineGraph =
				Component->GetGraphInstance()
					? Component->GetGraphInstance()->Graph.Get() : nullptr;
			const EPCGComponentGenerationTrigger BaselinePolicy =
				Component->GenerationTrigger;
			UPackage* const Package = Component->GetOutermost();
			const bool bBaselinePackageDirty = Package && Package->IsDirty();
			auto Rollback = [&]()
			{
				UE::UAI::Contract::FDiagnostic Detail;
				if (!TryRestorePcgComponentMutation(
						*Component, BaselineGraph, BaselinePolicy,
						bBaselinePackageDirty, Detail))
				{
					OutError.Append(Detail);
				}
			};

			FString Revision;
			if (!TryApplySchemaMutationOperations(
					*Component, Operations, NativeHook, Revision, OutError))
			{
				Rollback();
				return false;
			}
			if (Package)
			{
				Package->SetDirtyFlag(bBaselinePackageDirty);
			}
			if (Cancellation->IsCancelled())
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_ai_cancelled"),
					TEXT("The PCG component mutation was cancelled after apply."),
					Request.GetTargetRef(), TEXT("cancellation.requested"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
				OutError.SubjectId = Profile.GetCapabilityId();
				OutError.MutationMayHaveStarted = true;
				Rollback();
				return false;
			}

			FUeAgentCanonicalValuePtr Expected;
			FVerificationError VerifyError;
			if (!TryBuildSchemaMutationAggregate(
					Operations, nullptr, *Resources, Expected, VerifyError))
			{
				OutError = VerifyError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
				Rollback();
				return false;
			}
			TSharedPtr<FIndependentReadbackService> Readback;
			const TSharedRef<FPcgAiPcgComponentMutationReadbackOracle,
				ESPMode::ThreadSafe> Oracle =
				MakeShared<FPcgAiPcgComponentMutationReadbackOracle,
					ESPMode::ThreadSafe>(
						Profile, Request.GetTargetRef(), Operations);
			FIndependentReadbackOutcome Outcome;
			if (!FIndependentReadbackService::TryCreate({Oracle}, Readback, VerifyError))
			{
				OutError = VerifyError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetTargetRef();
				Rollback();
				return false;
			}
			if (!Readback->TryObserve(
					FIndependentReadbackRequest(
						TEXT("graph-compound-pcg-ai:") + Plan->GetRequestId(),
						PcgComponentMutationReadbackOracleId,
						Request.GetTargetRef(), Expected.ToSharedRef()),
					*Resources, Outcome, VerifyError))
			{
				OutError = Outcome.GetFailureDiagnostic(VerifyError);
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetTargetRef();
				Rollback();
				return false;
			}
			return TrySerializeResult(
				Request.GetTargetRef(), OutResult, OutError);
		}

		bool BuildStateTreeMutationDocument(UStateTree& Tree,
			const TArray<FPcgAiSchemaMutationOperation>& Operations, TSharedPtr<FJsonObject>& Document, UE::UAI::Contract::FDiagnostic& Error)
		{
			auto FailDocument = [&](const TCHAR* Code, const FString& Reason, const FString& Field,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
				const FPcgAiSchemaMutationOperation& Operation)
			{
				Error = NativeFailure(Code, Reason, Tree.GetPathName(), Field, MoveTemp(Actual), MoveTemp(Expected));
				Error.SubjectId = Operation.SemanticId;
				Error.OperationIndex = static_cast<int32>(&Operation - Operations.GetData());
				return false;
			};
			auto PreserveDocumentError = [&](const FDiagnostic& Cause)
			{
				Error = Cause;
				MarkGraphCompoundNativeDiagnostic(Error);
				if (Error.TargetPath.IsEmpty()) Error.TargetPath = Tree.GetPathName();
				return false;
			};
			TSharedPtr<FJsonValue> Captured;
			if (!UeAgentStateTreeAuthoring::ReadTreeDocument(Tree, Captured, Error)) return PreserveDocumentError(Error);
			Document = Captured->AsObject();
			const auto Items = Document->GetObjectField(TEXT("items"));
			auto Id = [](const FGuid& Guid) { return Guid.ToString(EGuidFormats::DigitsWithHyphensLower); };
			// Preserve the legacy operation contract, but project it into the complete
			// authoring document. Only the Reflection transaction may mutate the asset.
			for (const auto Kind : {EPcgAiSchemaMutationKind::StateTreeHierarchy, EPcgAiSchemaMutationKind::StateTreeNode})
				for (const auto& Operation : Operations)
				{
					if (Operation.Kind != Kind) continue;
					const bool IsState = Kind == EPcgAiSchemaMutationKind::StateTreeHierarchy;
					const FString ItemId = Id(IsState ? Operation.StateGuid : Operation.NodeGuid);
					FString Placement = Operation.Placement;
					if (Placement == TEXT("state_task")) Placement = TEXT("task");
					if (Placement == TEXT("state_condition")) Placement = TEXT("enter_condition");
					const FString Parent = IsState ? Id(Operation.ParentStateGuid)
						: (Placement == TEXT("task") || Placement == TEXT("enter_condition")) ? Id(Operation.StateGuid) : FString();
					if (Items->HasField(ItemId))
						return FailDocument(TEXT("state_tree_mutation_identity_invalid"),
							TEXT("The generated StateTree document already contains this item identity."),
							TEXT("items.id"), MakeShared<FJsonValueString>(ItemId),
							MakeShared<FJsonValueString>(TEXT("unused item identity")), Operation);
					if (!Parent.IsEmpty() && !Items->HasField(Parent))
						return FailDocument(TEXT("state_tree_mutation_identity_invalid"),
							TEXT("The generated StateTree document does not contain the item's parent."),
							TEXT("items.") + ItemId + TEXT(".parent"), MakeShared<FJsonValueString>(Parent),
							MakeShared<FJsonValueString>(TEXT("existing parent item identity")), Operation);
					const FString CandidateKind = IsState ? TEXT("state_tree_state")
						: Placement == TEXT("enter_condition") ? TEXT("state_tree_condition")
						: Placement == TEXT("evaluator") ? TEXT("state_tree_evaluator") : TEXT("state_tree_task");
					const UStruct* Type = Operation.PayloadClass ? static_cast<const UStruct*>(Operation.PayloadClass) : Operation.PayloadStruct;
					TSharedPtr<FJsonObject> Item, Parameters;
					HjsonWorkspace::FWorkspaceError Failure;
					if (!UeAgentStateTreeAuthoring::BuildTreeItemDefaults(Tree, TEXT("None"), CandidateKind,
						Type, Placement, Item, Parameters, Failure))
					{ return PreserveDocumentError(Failure.GetDiagnostic()); }
					int32 Index = 0;
					for (const auto& Existing : Items->Values)
					{
						const auto Row = Existing.Value->AsObject();
						FString ExistingPlacement;
						Row->TryGetStringField(TEXT("placement"), ExistingPlacement);
						if (Row->GetStringField(TEXT("kind")) == (IsState ? TEXT("state") : TEXT("node"))
							&& Row->GetStringField(TEXT("parent")) == Parent && (IsState || ExistingPlacement == Placement))
							Index = FMath::Max(Index, Row->GetIntegerField(TEXT("index")) + 1);
					}
					Item->SetStringField(TEXT("id"), ItemId);
					Item->SetStringField(TEXT("parent"), Parent);
					Item->SetNumberField(TEXT("index"), Index);
					Items->SetObjectField(ItemId, Item);
					if (Parameters)
					{
						Parameters->SetStringField(TEXT("parent"), ItemId);
						Items->SetObjectField(Parameters->GetStringField(TEXT("id")), Parameters);
					}
				}
			for (const auto& Operation : Operations)
			{
				if (Operation.Kind == EPcgAiSchemaMutationKind::StateTreeProperty)
					Items->GetObjectField(Id(Operation.StateGuid))->SetStringField(TEXT("label"), Operation.StateName.ToString());
				else if (Operation.Kind == EPcgAiSchemaMutationKind::StateTreeOrder)
					for (int32 Index = 0; Index < Operation.OrderedStateGuids.Num(); ++Index)
						Items->GetObjectField(Id(Operation.OrderedStateGuids[Index]))->SetNumberField(TEXT("index"), Index);
				else if (Operation.Kind != EPcgAiSchemaMutationKind::StateTreeHierarchy && Operation.Kind != EPcgAiSchemaMutationKind::StateTreeNode)
					return FailDocument(TEXT("state_tree_mutation_kind_invalid"),
						TEXT("StateTree document projection received an unsupported mutation kind."),
						TEXT("operation.kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Operation.Kind)),
						MakeShared<FJsonValueString>(TEXT("StateTreeHierarchy, StateTreeNode, StateTreeProperty or StateTreeOrder")), Operation);
			}
			if (!UeAgentStateTreeAuthoring::ValidateTreeDocument(MakeShared<FJsonValueObject>(Document), Error))
				return PreserveDocumentError(Error);
			return true;
		}

		bool TryExecuteAiGraphMutation(
			UObject*& Target,
			const FPcgAnimationGraphRouteProfile& RouteProfile,
			const FGraphCompoundPcgAiOperationRequest& Request,
			const FPlanningPreflightPlanRef& Plan,
			const TArray<FPcgAiSchemaMutationOperation>& Operations,
			TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			auto FailGeneratedOperation = [&](const TCHAR* Code, const FString& Reason,
				const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
				const FPcgAiSchemaMutationOperation& Operation)
			{
				OutError = NativeFailure(Code, Reason, Request.GetTargetRef(), Field, MoveTemp(Actual), MoveTemp(Expected));
				OutError.SubjectId = Operation.SemanticId;
				OutError.OperationIndex = static_cast<int32>(&Operation - Operations.GetData());
				return false;
			};
			auto PreserveGeneratedError = [&](const FDiagnostic& Cause,
				const FPcgAiSchemaMutationOperation* Operation = nullptr)
			{
				OutError = Cause;
				MarkGraphCompoundNativeDiagnostic(OutError);
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetTargetRef();
				if (OutError.SubjectId.IsEmpty())
					OutError.SubjectId = Operation ? Operation->SemanticId : Request.GetTargetRef();
				if (Operation && !OutError.OperationIndex.IsSet())
					OutError.OperationIndex = static_cast<int32>(Operation - Operations.GetData());
				return false;
			};
			TSharedPtr<FJsonObject> Graph;
			UE::UAI::Contract::FDiagnostic Error;
			if (Target->IsA<UBehaviorTree>())
			{
				using namespace UeAgentBehaviorTreeAuthoring;
				TArray<FMutation> NativeMutations;
				for (const FPcgAiSchemaMutationOperation& Operation : Operations)
				{
					FMutation& Mutation = NativeMutations.AddDefaulted_GetRef();
					Mutation.Identity = Operation.SemanticId;
					Mutation.Node.PayloadClass = Operation.PayloadClass;
					Mutation.Node.CandidateName = Operation.CandidateName;
					Mutation.Node.CandidateId = Operation.CandidateId;
					Mutation.Node.CandidateSelect = Operation.CandidateSelect;
					Mutation.Node.Location = FVector2f(Operation.PositionX, Operation.PositionY);
					Mutation.Node.NodeGuid = Operation.NodeGuid;
					Mutation.ParentId = Operation.OwnerItem;
					Mutation.ObjectRef = Operation.ObjectRef;
					Mutation.FromId = Operation.FromItem;
					Mutation.ToId = Operation.ToItem;
					Mutation.FromIndex = Operation.FromIndex;
					Mutation.ToIndex = Operation.ToIndex;
					switch (Operation.Kind)
					{
					case EPcgAiSchemaMutationKind::BehaviorRoot:
						Mutation.Kind = EMutationKind::Root; break;
					case EPcgAiSchemaMutationKind::BehaviorComposite:
						Mutation.Node.GraphNodeClass = UBehaviorTreeGraphNode_Composite::StaticClass(); break;
					case EPcgAiSchemaMutationKind::BehaviorTask:
						Mutation.Node.GraphNodeClass = UBehaviorTreeGraphNode_Task::StaticClass(); break;
					case EPcgAiSchemaMutationKind::BehaviorService:
						Mutation.Kind = EMutationKind::Attachment;
						Mutation.Node.GraphNodeClass = UBehaviorTreeGraphNode_Service::StaticClass(); break;
					case EPcgAiSchemaMutationKind::BehaviorDecorator:
						Mutation.Kind = EMutationKind::Attachment;
						Mutation.Node.GraphNodeClass = UBehaviorTreeGraphNode_Decorator::StaticClass(); break;
					case EPcgAiSchemaMutationKind::BehaviorBlackboardBinding:
						Mutation.Kind = EMutationKind::BlackboardBinding; break;
					case EPcgAiSchemaMutationKind::BehaviorRelation:
						Mutation.Kind = EMutationKind::Connection; break;
					default:
						return FailGeneratedOperation(TEXT("behavior_tree_mutation_kind_invalid"),
							TEXT("BehaviorTree document projection received an unsupported mutation kind."),
							TEXT("operation.kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Operation.Kind)),
							MakeShared<FJsonValueString>(TEXT("BehaviorRoot, BehaviorComposite, BehaviorTask, BehaviorService, BehaviorDecorator, BehaviorBlackboardBinding or BehaviorRelation")), Operation);
					}
				}
				if (!UeAgentReflectionAuthoring::RegisterAuthoring(Error))
				{
					return PreserveGeneratedError(Error);
				}
				if (!BuildGraphDocumentFromMutations(*CastChecked<UBehaviorTree>(Target), NativeMutations, Graph, Error))
				{
					return PreserveGeneratedError(Error);
				}
			}
			else if (UStateTree* Tree = Cast<UStateTree>(Target))
			{
				if (!UeAgentReflectionAuthoring::RegisterAuthoring(Error)
					|| !BuildStateTreeMutationDocument(*Tree, Operations, Graph, Error))
				{ return PreserveGeneratedError(Error); }
			}
			else if (UBlackboardData* Blackboard = Cast<UBlackboardData>(Target))
			{
				TSharedPtr<FJsonValue> Document;
				if (!UeAgentReflectionAuthoring::RegisterAuthoring(Error)
					|| !UeAgentBlackboardAuthoring::ReadDocument(*Blackboard, Document, Error))
				{ return PreserveGeneratedError(Error); }
				Graph = Document->AsObject();
				const auto Keys = Graph->GetObjectField(TEXT("blackboard_keys"));
				for (const auto& Operation : Operations)
				{
					if (Operation.Kind != EPcgAiSchemaMutationKind::BlackboardEntry)
						return FailGeneratedOperation(TEXT("blackboard_mutation_kind_invalid"),
							TEXT("Blackboard document projection requires a BlackboardEntry operation."),
							TEXT("operation.kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Operation.Kind)),
							MakeShared<FJsonValueString>(TEXT("BlackboardEntry")), Operation);
					if (!Operation.KeyTypeClass)
						return FailGeneratedOperation(TEXT("blackboard_mutation_kind_invalid"),
							TEXT("The Blackboard entry operation has no key type class."),
							TEXT("operation.key_type_class"), nullptr, ClassPathValue(UBlackboardKeyType::StaticClass()), Operation);
					const auto Key = MakeShared<FJsonObject>();
					Key->SetStringField(TEXT("name"), Operation.EntryName.ToString());
					Key->SetStringField(TEXT("key_type_class"), Operation.KeyTypeClass->GetPathName());
					Keys->SetObjectField(Operation.EntryName.ToString(), Key);
				}
			}
			else if (auto* Definition = Cast<USmartObjectDefinition>(Target))
			{
				TSharedPtr<FJsonValue> Document;
				if (!UeAgentReflectionAuthoring::RegisterAuthoring(Error)
					|| !UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, Document, Error))
				{ return PreserveGeneratedError(Error); }
				Graph = Document->AsObject();
				const auto Items = Graph->GetObjectField(TEXT("items"));
				for (const auto& Operation : Operations)
				{
					if (Operation.Kind != EPcgAiSchemaMutationKind::SmartObjectSlot)
						return FailGeneratedOperation(TEXT("smart_object_mutation_kind_invalid"),
							TEXT("SmartObject document projection requires a SmartObjectSlot operation."),
							TEXT("operation.kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Operation.Kind)),
							MakeShared<FJsonValueString>(TEXT("SmartObjectSlot")), Operation);
					if (!Operation.BehaviorClass)
						return FailGeneratedOperation(TEXT("smart_object_mutation_kind_invalid"),
							TEXT("The SmartObject slot operation has no behavior class."),
							TEXT("operation.behavior_class"), nullptr, ClassPathValue(USmartObjectBehaviorDefinition::StaticClass()), Operation);
					const FString SlotId = Operation.SlotGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
					if (!Items->HasField(SlotId))
					{
						TSharedPtr<FJsonObject> Slot;
						if (!UeAgentSmartObjectAuthoring::BuildDefinitionItemDefaults(*Definition, TEXT("Slot"), TEXT("slot"),
							FSmartObjectSlotDefinition::StaticStruct(), Slot, Error))
						{ return PreserveGeneratedError(Error, &Operation); }
						int32 Index = 0;
						for (const auto& Pair : Items->Values)
							if (Pair.Value->AsObject()->GetStringField(TEXT("kind")) == TEXT("slot")) ++Index;
						Slot->SetStringField(TEXT("id"), SlotId); Slot->SetStringField(TEXT("parent"), TEXT("root"));
						Slot->SetNumberField(TEXT("index"), Index); Items->SetObjectField(SlotId, Slot);
					}
					const FString BehaviorId = TEXT("behavior:") + SlotId + TEXT(":0");
					const TSharedPtr<FJsonObject>* Existing = nullptr;
					if (Items->TryGetObjectField(BehaviorId, Existing)
						&& (*Existing)->GetStringField(TEXT("class_path")) == Operation.BehaviorClass->GetPathName()) continue;
					TSharedPtr<FJsonObject> Behavior;
					if (!UeAgentSmartObjectAuthoring::BuildDefinitionItemDefaults(*Definition, TEXT("Behavior"), TEXT("behavior"),
						Operation.BehaviorClass, Behavior, Error))
					{ return PreserveGeneratedError(Error, &Operation); }
					Behavior->SetStringField(TEXT("id"), BehaviorId); Behavior->SetStringField(TEXT("parent"), SlotId);
					Behavior->SetNumberField(TEXT("index"), 0); Items->SetObjectField(BehaviorId, Behavior);
				}
			}
			else
			{
				TMap<FString, UClass*> NewOptions;
				TMap<FString, int32> Positions;
				for (const auto& Operation : Operations)
				{
					if (Operation.Kind == EPcgAiSchemaMutationKind::EqsOption)
						NewOptions.Add(Operation.SemanticId, Operation.PayloadClass);
					else if (Operation.Kind == EPcgAiSchemaMutationKind::EqsProperty)
						Positions.Add(Operation.FromItem, Operation.SignedIntegerValue);
					else
						return FailGeneratedOperation(TEXT("eqs_mutation_kind_invalid"),
							TEXT("EQS document projection supports only option and property operations."),
							TEXT("operation.kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Operation.Kind)),
							MakeShared<FJsonValueString>(TEXT("EqsOption or EqsProperty")), Operation);
				}
				if (!UeAgentReflectionAuthoring::RegisterAuthoring(Error)
					|| !UeAgentEqsAuthoring::BuildGraphDocumentFromOptionChanges(
						*CastChecked<UEnvQuery>(Target), NewOptions, Positions, Graph, Error))
				{ return PreserveGeneratedError(Error); }
			}
			const FString TypeId = Target->IsA<UBehaviorTree>() ? TEXT("behavior_tree")
				: Target->IsA<UStateTree>() ? TEXT("state_tree")
				: Target->IsA<UBlackboardData>() ? TEXT("blackboard")
				: Target->IsA<USmartObjectDefinition>() ? TEXT("smart_object") : TEXT("eqs");
			const auto* Profile = UeAgentReflectionAuthoring::FindProfileByTypeId(TypeId);
			if (!Profile)
			{
				OutError = NativeFailure(TEXT("ai_graph_apply_profile_missing"),
					TEXT("The reflection authoring profile required by the AI target type is not registered."),
					Request.GetTargetRef(), TEXT("reflection.profile.type_id"),
					MakeShared<FJsonValueString>(TypeId), MakeShared<FJsonValueString>(TEXT("registered authoring profile")));
				return false;
			}
			UeAgentReflectionAuthoring::FAssetApplyRequest ApplyRequest;
			ApplyRequest.TypeId = Profile->TypeId;
			ApplyRequest.Operation = UeAgentApplyInputContract::EOperation::Update;
			ApplyRequest.AssetPath = PackageName(Request.GetTargetRef());
			ApplyRequest.ProviderExtension = MakeShared<FJsonObject>();
			ApplyRequest.ProviderExtension->SetObjectField(Profile->ProfileStateField, Graph);
			ApplyRequest.bCompactValues = true;
			ApplyRequest.bSave = true;
			FUeAgentCanonicalValuePtr Expected;
			FVerificationError VerifyError;
			TSharedPtr<FIndependentReadbackService> Readback;
			const auto Oracle = MakeShared<FPcgAiSchemaMutationReadbackOracle, ESPMode::ThreadSafe>(
				RouteProfile, Request.GetTargetRef(), Operations, false);
			if (!TryBuildSchemaMutationAggregate(Operations, nullptr, *Resources, Expected, VerifyError)
				|| !FIndependentReadbackService::TryCreate({Oracle}, Readback, VerifyError))
			{
				OutError = VerifyError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
				return false;
			}
			ApplyRequest.FinalReadback = [Readback, Expected, Resources, TargetRef = Request.GetTargetRef(),
				RequestId = Plan->GetRequestId()](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
			{
				FIndependentReadbackOutcome Outcome;
				FVerificationError Error;
				if (!Readback->TryObserve(FIndependentReadbackRequest(
					TEXT("graph-compound-pcg-ai:") + RequestId, SchemaMutationReadbackOracleId,
					TargetRef, Expected.ToSharedRef()), *Resources, Outcome, Error))
				{
					Failure.Diagnostic = Outcome.GetFailureDiagnostic(Error);
					return false;
				}
				return true;
			};
			// Reflection owns checkpoint, expected/actual graph comparison and persistent readback.
			Target = nullptr;
			UeAgentReflectionAuthoring::FAssetApplyResult Result;
			if (!UeAgentReflectionAuthoring::ApplyAsset(ApplyRequest, Result, Plan->GetRequestId()))
			{
				return PreserveGeneratedError(Result.Diagnostic);
			}
			return TrySerializeResult(Request.GetTargetRef(), OutResult, OutError);
		}

		bool TryExecuteSchemaMutation(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FGraphCompoundPcgAiOperationRequest& Request,
			const FPlanningPreflightPlanRef& Plan,
			const FUeAgentCallerContext& CallerContext,
			TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
			const FString& JournalRoot,
			FPcgAiNodeGraphNativeHook& NativeHook,
			FPackageAssetDurableBackend& DurableBackend,
			int64& LastFencingToken,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			if (IsPcgComponentMutationProfile(Profile))
			{
				return TryExecutePcgComponentMutation(
					Profile, Request, Plan, CallerContext,
					MoveTemp(Resources), Cancellation, NativeHook,
					OutResult, OutError);
			}
			if (!IsInGameThread() || Cancellation->IsCancelled())
			{
				const bool bCancelled = Cancellation->IsCancelled();
				OutError = NativeFailure(bCancelled
						? TEXT("graph_compound_pcg_ai_cancelled") : TEXT("graph_compound_pcg_ai_thread_invalid"),
					bCancelled ? TEXT("The PCG/AI schema mutation was cancelled before execution.")
						: TEXT("PCG/AI schema mutation must execute on the game thread."),
					Request.GetTargetRef(), bCancelled ? TEXT("cancellation.requested") : TEXT("execution.game_thread"),
					MakeShared<FJsonValueBoolean>(bCancelled), MakeShared<FJsonValueBoolean>(!bCancelled));
				OutError.SubjectId = Profile.GetCapabilityId();
				return false;
			}
			UObject* Target = LoadObject<UObject>(nullptr, *Request.GetTargetRef());
			if (!Target)
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
					TEXT("The PCG/AI mutation target could not be loaded."),
					Request.GetTargetRef(), TEXT("target"), nullptr, MakeShared<FJsonValueString>(TEXT("loadable target object")));
				return false;
			}
			if (Target->GetPathName() != Request.GetTargetRef())
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
					TEXT("The loaded PCG/AI target path differs from the requested reference."),
					Request.GetTargetRef(), TEXT("target.path"), MakeShared<FJsonValueString>(Target->GetPathName()),
					MakeShared<FJsonValueString>(Request.GetTargetRef()));
				return false;
			}
			TArray<FPcgAiSchemaMutationOperation> Operations;
			if (!TryParseSchemaMutationOperations(
					Profile, Request, Plan, *Target, Operations, OutError))
			{
				return false;
			}
			FUeAgentCanonicalSnapshotPtr Current;
			if (!TryCaptureMutationSnapshot(
					Profile, Request.GetTarget(), *Resources, Current, OutError))
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

			if (Target->IsA<UBehaviorTree>() || Target->IsA<UEnvQuery>() || Target->IsA<UStateTree>()
				|| Target->IsA<UBlackboardData>() || Target->IsA<USmartObjectDefinition>())
			{
				return TryExecuteAiGraphMutation(Target, Profile, Request, Plan, Operations, Resources, OutResult, OutError);
			}

			const int64 FencingToken = ++LastFencingToken;
			const FString JournalOwner =
				TEXT("graph-compound-pcg-ai:") + Plan->GetRequestId();
			const FString TargetPackage = PackageName(Request.GetTargetRef());
			const FAssetDurableRequest DurableRequest(
				Plan,
				{FAssetDurableTarget(
					Plan->GetChangeSet().GetChanges()[0].GetIdentity(),
					TargetPackage, EPackageHeaderKind::Asset,
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
				OutError = DurableError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
				return false;
			}

			FString Revision;
			if (!TryApplySchemaMutationOperations(
					*Target, Operations, NativeHook, Revision, OutError))
			{
				Target = nullptr;
				UE::UAI::Contract::FDiagnostic RollbackDetail;
				TryRollbackCreate(
					Request.GetTargetRef(), false, Prepared.ToSharedRef(),
					DurableBackend, FencingToken, *Resources, RollbackDetail);
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
				TryRollbackCreate(
					Request.GetTargetRef(), false, Prepared.ToSharedRef(),
					DurableBackend, FencingToken, *Resources, RollbackDetail);
				SetError(OutError, DurableError, Request.GetTargetRef(), RollbackDetail);
				return false;
			}
			Target->GetOutermost()->SetDirtyFlag(false);
			Target = nullptr;
			const FAssetDurableSessionRef Persisted =
				PersistedSession(Prepared.ToSharedRef(), DurableReceipt);

			FUeAgentCanonicalValuePtr Expected;
			FVerificationError VerifyError;
			if (!TryBuildSchemaMutationAggregate(
					Operations, nullptr, *Resources, Expected, VerifyError))
			{
				UE::UAI::Contract::FDiagnostic RollbackDetail;
				TryRollbackCreate(
					Request.GetTargetRef(), false, Persisted,
					DurableBackend, FencingToken, *Resources, RollbackDetail);
				SetError(OutError, VerifyError, Request.GetTargetRef(), RollbackDetail);
				return false;
			}
			TSharedPtr<FIndependentReadbackService> Readback;
			const TSharedRef<FPcgAiSchemaMutationReadbackOracle,
				ESPMode::ThreadSafe> Oracle =
				MakeShared<FPcgAiSchemaMutationReadbackOracle,
					ESPMode::ThreadSafe>(Profile, Request.GetTargetRef(), Operations);
			FIndependentReadbackOutcome Outcome;
			const bool bReadbackCreated = FIndependentReadbackService::TryCreate({Oracle}, Readback, VerifyError);
			const bool bObserved = bReadbackCreated && Readback->TryObserve(
				FIndependentReadbackRequest(
					TEXT("graph-compound-pcg-ai:") + Plan->GetRequestId(),
					SchemaMutationReadbackOracleId, Request.GetTargetRef(), Expected.ToSharedRef()),
				*Resources, Outcome, VerifyError);
			if (!bObserved)
			{
				UE::UAI::Contract::FDiagnostic RollbackDetail;
				TryRollbackCreate(
					Request.GetTargetRef(), false, Persisted,
					DurableBackend, FencingToken, *Resources, RollbackDetail);
				OutError = bReadbackCreated ? Outcome.GetFailureDiagnostic(VerifyError) : VerifyError;
				OutError.Append(RollbackDetail);
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetTargetRef();
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
				TryRollbackCreate(
					Request.GetTargetRef(), false, Persisted,
					DurableBackend, FencingToken, *Resources, RollbackDetail);
				SetError(OutError, DurableError, Request.GetTargetRef(), RollbackDetail);
				return false;
			}
			return TrySerializeResult(
				Request.GetTargetRef(),
				OutResult, OutError);
		}

		const FString PcgLevelDataAssetReadbackOracleId =
			TEXT("uai.readback.graph_compound.pcg_ai.level_data_asset");

		UClass* LoadSupportedPcgLevelPointDataClass(
			const FString& DataTypeRef)
		{
			UClass* const DataClass = LoadObject<UClass>(nullptr, *DataTypeRef);
			return DataClass == UPCGPointData::StaticClass()
				|| DataClass == UPCGPointArrayData::StaticClass()
				? DataClass : nullptr;
		}

		bool TryBuildPcgLevelDataAssetValue(
			const FString& DataTypeRef,
			const FString& SourceRef,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError)
		{
			const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("data_type_ref"), DataTypeRef);
			Value->SetStringField(TEXT("source_ref"), SourceRef);
			Value->SetNumberField(TEXT("dataset_count"), 2);
			return TryCanonicalObject(Value, Resources, OutValue, OutError);
		}

		class FPcgLevelDataAssetReadbackOracle final
			: public IIndependentReadbackOracle
		{
		public:
			FPcgLevelDataAssetReadbackOracle(
				FString InTargetRef,
				FString InDataTypeRef,
				FString InSourceRef)
				: TargetRef(MoveTemp(InTargetRef))
				, DataTypeRef(MoveTemp(InDataTypeRef))
				, SourceRef(MoveTemp(InSourceRef))
			{
			}

			const FString& GetOracleId() const override
			{
				return PcgLevelDataAssetReadbackOracleId;
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
					OutError = Detail.IsEmpty()
						? NativeFailure(TEXT("graph_compound_pcg_level_asset_unload_failed"),
							TEXT("The persisted PCG level-data package reload failed without providing a diagnostic."),
							TargetRef, TEXT("package.reload.succeeded"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true))
						: Detail;
					return false;
				}
				UPCGDataAsset* const Asset = LoadObject<UPCGDataAsset>(
					nullptr, *ObjectPath(TargetRef));
				if (!Asset)
				{
					OutError = NativeFailure(TEXT("graph_compound_pcg_level_asset_reload_failed"),
						TEXT("The persisted PCG level-data asset could not be loaded for independent readback."),
						TargetRef, TEXT("asset"), nullptr, ClassPathValue(UPCGDataAsset::StaticClass()));
					OutError.Stage = TEXT("verification");
					return false;
				}
				FDiagnostic Mismatch;
				auto SetMismatch = [&](const FString& Reason, const FString& Field,
					TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
				{
					Mismatch = NativeFailure(TEXT("graph_compound_pcg_level_asset_reload_mismatch"),
						Reason, TargetRef, Field, MoveTemp(Actual), MoveTemp(Expected));
					Mismatch.Stage = TEXT("independent_readback");
				};
				if (Asset->GetOutermost()->GetName() != PackageName(TargetRef))
					SetMismatch(TEXT("The reloaded PCG data asset belongs to a different package."),
						TEXT("asset.package"), MakeShared<FJsonValueString>(Asset->GetOutermost()->GetName()),
						MakeShared<FJsonValueString>(PackageName(TargetRef)));
				else if (Asset->GetOutermost()->IsDirty())
					SetMismatch(TEXT("The reloaded PCG data asset package is dirty."),
						TEXT("asset.package.dirty"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
				else if (Asset->ObjectPath.ToString() != SourceRef)
					SetMismatch(TEXT("The reloaded PCG data asset source differs from the requested source level."),
						TEXT("asset.source_ref"), MakeShared<FJsonValueString>(Asset->ObjectPath.ToString()),
						MakeShared<FJsonValueString>(SourceRef));
				else if (Asset->Data.TaggedData.Num() != 2)
					SetMismatch(TEXT("The reloaded PCG level-data asset does not contain exactly two tagged data items."),
						TEXT("asset.tagged_data.count"), MakeShared<FJsonValueNumber>(Asset->Data.TaggedData.Num()),
						MakeShared<FJsonValueNumber>(2));
				// Keep class loading at the same point, including when earlier readback facts differ.
				UClass* const ExpectedDataClass = LoadSupportedPcgLevelPointDataClass(DataTypeRef);
				if (Mismatch.IsEmpty())
				{
					for (int32 Index = 0; Index < Asset->Data.TaggedData.Num(); ++Index)
					{
						const FPCGTaggedData& Tagged = Asset->Data.TaggedData[Index];
						const FString Field = FString::Printf(TEXT("asset.tagged_data/%d"), Index);
						if (!Tagged.Data)
						{
							SetMismatch(TEXT("The reloaded PCG tagged data item has no data object."),
								Field, nullptr, MakeShared<FJsonValueString>(DataTypeRef));
							break;
						}
						if (Tagged.Data->GetClass() != ExpectedDataClass)
						{
							SetMismatch(ExpectedDataClass
									? TEXT("The reloaded PCG tagged data class differs from the requested point-data class.")
									: TEXT("The expected PCG point-data class is unavailable or unsupported during readback."),
								Field + TEXT("/class"), ClassPathValue(Tagged.Data->GetClass()),
								MakeShared<FJsonValueString>(DataTypeRef));
							break;
						}
					}
				}
				if (!Mismatch.IsEmpty())
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(MoveTemp(Mismatch), TEXT("reload:") + TargetRef);
					return true;
				}
				FUeAgentCanonicalValuePtr Actual;
				FGraphCompoundRuntimeError ValueError;
				if (!TryBuildPcgLevelDataAssetValue(
						DataTypeRef, SourceRef, Resources, Actual, ValueError))
				{
					OutError = ValueError;
					return false;
				}
				OutObservation = FIndependentReadbackObservation::Available(
					Actual.ToSharedRef(),
					TEXT("reload:") + Request.GetSubjectRef() + TEXT(":")
						+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
				return true;
			}

		private:
			FString TargetRef;
			FString DataTypeRef;
			FString SourceRef;
		};

		bool TryParsePcgDataFromLevel(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FGraphCompoundPcgAiOperationRequest& Request,
			const FPlanningPreflightPlanRef& Plan,
			FString& OutDataTypeRef,
			FString& OutSourceRef,
			FGraphCompoundRuntimeError& OutError)
		{
			OutDataTypeRef.Reset();
			OutSourceRef.Reset();
			const TArray<FGraphCompoundCanonicalEdit>& Edits =
				Request.GetPayload().GetEdits();
			const TArray<FPlannedChange>& Changes =
				Plan->GetChangeSet().GetChanges();
			TOptional<int32> EditIndex;
			auto FailPlan = [&](const FString& Reason, const FString& Field,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
			{
				OutError = NativeFailure(TEXT("graph_compound_pcg_data_from_level_plan_invalid"),
					Reason, Request.GetTargetRef(), Field, MoveTemp(Actual), MoveTemp(Expected));
				OutError.OperationIndex = EditIndex;
				return false;
			};
			if (Profile.GetCapabilityId() != TEXT("pcg.data_asset.create_from_level"))
				return FailPlan(TEXT("The level-data plan uses an unsupported capability."),
					TEXT("capability_id"), MakeShared<FJsonValueString>(Profile.GetCapabilityId()),
					MakeShared<FJsonValueString>(TEXT("pcg.data_asset.create_from_level")));
			if (Profile.GetInputKind() != EPcgAnimationGraphInputKind::PcgDataFromLevel)
				return FailPlan(TEXT("The level-data plan uses an incorrect route input kind."),
					TEXT("profile.input_kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Profile.GetInputKind())),
					MakeShared<FJsonValueNumber>(static_cast<int32>(EPcgAnimationGraphInputKind::PcgDataFromLevel)));
			if (Profile.GetTargetKind() != TEXT("pcg_data_asset"))
				return FailPlan(TEXT("The level-data route does not target PCG data assets."),
					TEXT("profile.target_kind"), MakeShared<FJsonValueString>(Profile.GetTargetKind()),
					MakeShared<FJsonValueString>(TEXT("pcg_data_asset")));
			if (Request.GetTarget().GetTargetKind() != TEXT("pcg_data_asset"))
				return FailPlan(TEXT("The level-data request does not target a PCG data asset."),
					TEXT("request.target_kind"), MakeShared<FJsonValueString>(Request.GetTarget().GetTargetKind()),
					MakeShared<FJsonValueString>(TEXT("pcg_data_asset")));
			const FUeAgentDescriptorIdentity& ActualDescriptor = Plan->GetDescriptorIdentity();
			const FUeAgentDescriptorIdentity& ExpectedDescriptor =
				Profile.GetExecutionContract().GetDescriptorIdentity();
			if (ActualDescriptor != ExpectedDescriptor)
			{
				if (ActualDescriptor.GetDescriptorId() != ExpectedDescriptor.GetDescriptorId())
					return FailPlan(TEXT("The plan descriptor id does not match the route descriptor."),
						TEXT("descriptor_identity.descriptor_id"),
						MakeShared<FJsonValueString>(ActualDescriptor.GetDescriptorId()),
						MakeShared<FJsonValueString>(ExpectedDescriptor.GetDescriptorId()));
				if (ActualDescriptor.GetVersion() != ExpectedDescriptor.GetVersion())
					return FailPlan(TEXT("The plan descriptor version does not match the route descriptor."),
						TEXT("descriptor_identity.version"),
						MakeShared<FJsonValueNumber>(ActualDescriptor.GetVersion()),
						MakeShared<FJsonValueNumber>(ExpectedDescriptor.GetVersion()));
				FailPlan(TEXT("The plan descriptor hash does not match the route descriptor."),
					TEXT("descriptor_identity.descriptor_hash"), nullptr, nullptr);
				OutError.ActualEvidence = MakeShared<FJsonValueString>(ActualDescriptor.GetDescriptorHash());
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedDescriptor.GetDescriptorHash());
				return false;
			}
			if (Edits.Num() != 1)
				return FailPlan(TEXT("The level-data request requires exactly one edit."),
					TEXT("edits.count"), MakeShared<FJsonValueNumber>(Edits.Num()), MakeShared<FJsonValueNumber>(1));
			if (Changes.Num() != 1)
				return FailPlan(TEXT("The level-data plan requires exactly one change."),
					TEXT("changes.count"), MakeShared<FJsonValueNumber>(Changes.Num()), MakeShared<FJsonValueNumber>(1));
			EditIndex = 0;
			const FGraphCompoundCanonicalEdit& Edit = Edits[0];
			if (Edit.GetElementKind() != EGraphCompoundElementKind::Target)
				return FailPlan(TEXT("The level-data edit must target the asset itself."),
					TEXT("edit.element_kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetElementKind())),
					MakeShared<FJsonValueString>(TEXT("target")));
			if (Edit.GetIntent() != EChangeIntent::Create)
				return FailPlan(TEXT("The level-data edit must use create intent."),
					TEXT("edit.intent"), MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetIntent())),
					MakeShared<FJsonValueString>(TEXT("create")));
			if (Edit.GetIdentity().GetOwnerId() != Request.GetTargetRef())
				return FailPlan(TEXT("The level-data edit owner differs from the requested asset."),
					TEXT("edit.identity.owner_id"), MakeShared<FJsonValueString>(Edit.GetIdentity().GetOwnerId()),
					MakeShared<FJsonValueString>(Request.GetTargetRef()));
			const FString SemanticId = Edit.GetIdentity().GetSemanticId();
			if (SemanticId != TEXT("target:") + Request.GetTargetRef())
				return FailPlan(TEXT("The level-data edit identity differs from the requested asset."),
					TEXT("edit.identity.semantic_id"), MakeShared<FJsonValueString>(SemanticId),
					MakeShared<FJsonValueString>(TEXT("target:") + Request.GetTargetRef()));
			if (!Edit.GetProposedAfter().IsValid())
				return FailPlan(TEXT("The level-data edit has no proposed value."),
					TEXT("edit.proposed_after"), nullptr, MakeShared<FJsonValueString>(TEXT("proposed data specification")));
			const FString* ValuePointer = Request.GetPayload().FindInputPointer(SemanticId);
			const FRequiredCanonicalInput Input(TEXT("graph_compound_pcg_data_from_level_plan_invalid"),
				Request.GetTargetRef(), SemanticId, ValuePointer ? *ValuePointer : FString(), false, OutError, EditIndex);
			if (!Input.RequireFields(*Edit.GetProposedAfter(), {TEXT("data_type_ref"), TEXT("source_level_ref")})
				|| !Input.ReadString(*Edit.GetProposedAfter(), TEXT("data_type_ref"), OutDataTypeRef)
				|| !Input.ReadString(*Edit.GetProposedAfter(), TEXT("source_level_ref"), OutSourceRef)) return false;
			if (Changes[0].GetIdentity().GetOwnerId() != Request.GetTargetRef())
				return FailPlan(TEXT("The planned level-data change owner differs from the requested asset."),
					TEXT("change.identity.owner_id"), MakeShared<FJsonValueString>(Changes[0].GetIdentity().GetOwnerId()),
					MakeShared<FJsonValueString>(Request.GetTargetRef()));
			if (Changes[0].GetIntent() != EChangeIntent::Create)
				return FailPlan(TEXT("The planned level-data change must use create intent."),
					TEXT("change.intent"), MakeShared<FJsonValueNumber>(static_cast<int32>(Changes[0].GetIntent())),
					MakeShared<FJsonValueString>(TEXT("create")));
			UClass* const DataClass = LoadObject<UClass>(nullptr, *OutDataTypeRef);
			const bool bSupportedDataClass = DataClass == UPCGPointData::StaticClass()
				|| DataClass == UPCGPointArrayData::StaticClass();
			const FRequiredCanonicalInput TypeInput(TEXT("graph_compound_pcg_data_type_unsupported"),
				Request.GetTargetRef(), SemanticId, ValuePointer ? *ValuePointer : FString(), false, OutError, EditIndex);
			if (!DataClass)
				return TypeInput.Fail(TEXT("The PCG level-data class reference could not be loaded."),
					TEXT("/data_type_ref"), MakeShared<FJsonValueString>(OutDataTypeRef),
					MakeShared<FJsonValueString>(TEXT("loadable PCGPointData or PCGPointArrayData class")));
			if (!bSupportedDataClass)
				return TypeInput.Fail(TEXT("The PCG level-data class is not one of the supported exact point-data classes."),
					TEXT("/data_type_ref"), ClassPathValue(DataClass),
					MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
						ClassPathValue(UPCGPointData::StaticClass()), ClassPathValue(UPCGPointArrayData::StaticClass())}));
			return true;
		}

		bool TryExecutePcgDataFromLevel(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FGraphCompoundPcgAiOperationRequest& Request,
			const FPlanningPreflightPlanRef& Plan,
			const FUeAgentCallerContext& CallerContext,
			TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
			const FString& JournalRoot,
			FPackageAssetDurableBackend& DurableBackend,
			int64& LastFencingToken,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			if (!IsInGameThread() || Cancellation->IsCancelled())
			{
				const bool bCancelled = Cancellation->IsCancelled();
				OutError = NativeFailure(bCancelled
						? TEXT("graph_compound_pcg_ai_cancelled") : TEXT("graph_compound_pcg_ai_thread_invalid"),
					bCancelled ? TEXT("The PCG level-data creation was cancelled before execution.")
						: TEXT("PCG level-data creation must execute on the game thread."),
					Request.GetTargetRef(), bCancelled ? TEXT("cancellation.requested") : TEXT("execution.game_thread"),
					MakeShared<FJsonValueBoolean>(bCancelled), MakeShared<FJsonValueBoolean>(!bCancelled));
				OutError.SubjectId = Profile.GetCapabilityId();
				return false;
			}
			FString DataTypeRef;
			FString SourceRef;
			if (!TryParsePcgDataFromLevel(
					Profile, Request, Plan, DataTypeRef, SourceRef, OutError))
			{
				return false;
			}
			UWorld* const SourceWorld =
				FindObject<UWorld>(nullptr, *SourceRef)
					? FindObject<UWorld>(nullptr, *SourceRef)
					: LoadObject<UWorld>(nullptr, *SourceRef);
			if (!SourceWorld || SourceWorld->GetPathName() != SourceRef)
			{
				const FString SemanticId = TEXT("target:") + Request.GetTargetRef();
				const FString* ValuePointer = Request.GetPayload().FindInputPointer(SemanticId);
				const FRequiredCanonicalInput Input(TEXT("graph_compound_pcg_source_level_invalid"),
					Request.GetTargetRef(), SemanticId, ValuePointer ? *ValuePointer : FString(), false, OutError, 0);
				return Input.Fail(SourceWorld
						? TEXT("The loaded source world path differs from the requested level reference.")
						: TEXT("The source level reference could not be loaded as a world."),
					TEXT("/source_level_ref"), MakeShared<FJsonValueString>(SourceWorld ? SourceWorld->GetPathName() : SourceRef),
					MakeShared<FJsonValueString>(SourceWorld ? SourceRef : TEXT("loadable world reference")));
			}

			FUeAgentCanonicalSnapshotPtr Current;
			if (!TryCreateAbsentSnapshot(
					Request.GetTarget(), *Resources, Current, OutError))
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
				TEXT("graph-compound-pcg-ai:") + Plan->GetRequestId();
			const FAssetDurableRequest DurableRequest(
				Plan,
				{FAssetDurableTarget(
					Plan->GetChangeSet().GetChanges()[0].GetIdentity(),
					Request.GetTargetRef(), EPackageHeaderKind::Asset,
					EAssetDurableIntent::Save,
					ERollbackGrade::DurableBackup)},
				FencingToken, JournalOwner, EAtomicityMode::CompensatedSaga);
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
			UPCGDataAsset* Asset = Package
				? NewObject<UPCGDataAsset>(
					Package, *AssetName(Request.GetTargetRef()),
					RF_Public | RF_Standalone | RF_Transactional)
				: nullptr;
			UPCGLevelToAsset* const Exporter =
				NewObject<UPCGLevelToAsset>(GetTransientPackage());
			UClass* const ExpectedDataClass =
				LoadSupportedPcgLevelPointDataClass(DataTypeRef);
			const bool bPreviousPointArrayEnabled =
				CVarPCGEnablePointArrayData.GetValueOnGameThread();
			CVarPCGEnablePointArrayData->Set(
				ExpectedDataClass == UPCGPointArrayData::StaticClass(),
				ECVF_SetByCode);
			if (Exporter)
			{
				Exporter->SetWorld(SourceWorld);
			}
			const bool bNativeExported = Asset && Exporter
				&& Exporter->Export(Request.GetTargetRef(), Asset);
			CVarPCGEnablePointArrayData->Set(
				bPreviousPointArrayEnabled, ECVF_SetByCode);
			FDiagnostic ExportError;
			auto FailExport = [&](const FString& Reason, const FString& Field,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
			{
				ExportError = NativeFailure(TEXT("graph_compound_pcg_level_export_failed"),
					Reason, Request.GetTargetRef(), Field, MoveTemp(Actual), MoveTemp(Expected));
				if (Package) ExportError.MutationMayHaveStarted = true;
			};
			if (!Package)
				FailExport(TEXT("The destination package could not be created for level-data export."),
					TEXT("package"), nullptr, MakeShared<FJsonValueString>(Request.GetTargetRef()));
			else if (!Asset)
				FailExport(TEXT("The destination PCG data asset could not be created."),
					TEXT("asset"), nullptr, ClassPathValue(UPCGDataAsset::StaticClass()));
			else if (!Exporter)
				FailExport(TEXT("The PCG level exporter could not be created."),
					TEXT("exporter"), nullptr, ClassPathValue(UPCGLevelToAsset::StaticClass()));
			else if (!bNativeExported)
				FailExport(TEXT("The native PCG level exporter reported failure."),
					TEXT("export.succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			else if (Asset->GetPathName() != ObjectPath(Request.GetTargetRef()))
				FailExport(TEXT("The exported PCG data asset path differs from the requested destination."),
					TEXT("asset.path"), MakeShared<FJsonValueString>(Asset->GetPathName()),
					MakeShared<FJsonValueString>(ObjectPath(Request.GetTargetRef())));
			else if (Asset->ObjectPath.ToString() != SourceRef)
				FailExport(TEXT("The exported PCG data asset source differs from the requested level."),
					TEXT("asset.source_ref"), MakeShared<FJsonValueString>(Asset->ObjectPath.ToString()),
					MakeShared<FJsonValueString>(SourceRef));
			else if (Asset->Data.TaggedData.Num() != 2)
				FailExport(TEXT("The exported PCG level-data asset does not contain exactly two tagged data items."),
					TEXT("asset.tagged_data.count"), MakeShared<FJsonValueNumber>(Asset->Data.TaggedData.Num()),
					MakeShared<FJsonValueNumber>(2));
			else
			{
				for (int32 Index = 0; Index < Asset->Data.TaggedData.Num(); ++Index)
				{
					const FPCGTaggedData& Tagged = Asset->Data.TaggedData[Index];
					const FString Field = FString::Printf(TEXT("asset.tagged_data/%d"), Index);
					if (!Tagged.Data)
					{
						FailExport(TEXT("The exported PCG tagged data item has no data object."),
							Field, nullptr, MakeShared<FJsonValueString>(DataTypeRef));
						break;
					}
					if (Tagged.Data->GetClass() != ExpectedDataClass)
					{
						FailExport(ExpectedDataClass
								? TEXT("The exported PCG tagged data class differs from the requested point-data class.")
								: TEXT("The expected PCG point-data class is unavailable or unsupported during export."),
							Field + TEXT("/class"), ClassPathValue(Tagged.Data->GetClass()),
							MakeShared<FJsonValueString>(DataTypeRef));
						break;
					}
				}
			}
			if (!ExportError.IsEmpty())
			{
				Asset = nullptr;
				Package = nullptr;
				UE::UAI::Contract::FDiagnostic RollbackDetail;
				TryRollbackCreate(
					Request.GetTargetRef(), true, Prepared.ToSharedRef(),
					DurableBackend, FencingToken, *Resources, RollbackDetail);
				SetError(OutError, ExportError, Request.GetTargetRef(), RollbackDetail);
				return false;
			}
			Asset->SetFlags(RF_Public | RF_Standalone);
			FAssetRegistryModule::AssetCreated(Asset);
			Package->MarkPackageDirty();
			FDurableReceipt DurableReceipt;
			if (!TryPersistAssetDurabilityForVerification(
					Prepared.ToSharedRef(), DurableBackend, FencingToken,
					*Resources, DurableReceipt, DurableError))
			{
				Asset = nullptr;
				Package = nullptr;
				UE::UAI::Contract::FDiagnostic RollbackDetail;
				TryRollbackCreate(
					Request.GetTargetRef(), true, Prepared.ToSharedRef(),
					DurableBackend, FencingToken, *Resources, RollbackDetail);
				SetError(OutError, DurableError, Request.GetTargetRef(), RollbackDetail);
				return false;
			}
			Package->SetDirtyFlag(false);
			Asset = nullptr;
			Package = nullptr;
			const FAssetDurableSessionRef Persisted =
				PersistedSession(Prepared.ToSharedRef(), DurableReceipt);

			FUeAgentCanonicalValuePtr Expected;
			if (!TryBuildPcgLevelDataAssetValue(
					DataTypeRef, SourceRef, *Resources, Expected, OutError))
			{
				return false;
			}
			TSharedPtr<FIndependentReadbackService> Readback;
			FVerificationError VerifyError;
			const TSharedRef<FPcgLevelDataAssetReadbackOracle,
				ESPMode::ThreadSafe> Oracle =
				MakeShared<FPcgLevelDataAssetReadbackOracle,
					ESPMode::ThreadSafe>(
						Request.GetTargetRef(), DataTypeRef, SourceRef);
			FIndependentReadbackOutcome Outcome;
			const bool bReadbackCreated = FIndependentReadbackService::TryCreate({Oracle}, Readback, VerifyError);
			const bool bObserved = bReadbackCreated && Readback->TryObserve(
				FIndependentReadbackRequest(
					TEXT("graph-compound-pcg-ai:") + Plan->GetRequestId(),
					PcgLevelDataAssetReadbackOracleId, Request.GetTargetRef(), Expected.ToSharedRef()),
				*Resources, Outcome, VerifyError);
			if (!bObserved)
			{
				UE::UAI::Contract::FDiagnostic RollbackDetail;
				TryRollbackCreate(
					Request.GetTargetRef(), true, Persisted,
					DurableBackend, FencingToken, *Resources, RollbackDetail);
				OutError = bReadbackCreated ? Outcome.GetFailureDiagnostic(VerifyError) : VerifyError;
				OutError.Append(RollbackDetail);
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetTargetRef();
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
				TryRollbackCreate(
					Request.GetTargetRef(), true, Persisted,
					DurableBackend, FencingToken, *Resources, RollbackDetail);
				SetError(OutError, DurableError, Request.GetTargetRef(), RollbackDetail);
				return false;
			}
			return TrySerializeResult(
				Request.GetTargetRef(),
				OutResult, OutError);
		}

		class FPcgAiProductionEnvironment final
			: public IGraphCompoundPcgAiEnvironment
		{
		public:
			explicit FPcgAiProductionEnvironment(
				const FGraphCompoundPcgAiProductionConfig& Config)
				: JournalRoot(Config.DurableJournalRoot.IsEmpty()
					? FPaths::ProjectSavedDir()
						/ TEXT("UeAgentInterface/GraphCompoundDurable")
					: Config.DurableJournalRoot)
			{
			}

			bool TryCaptureSnapshot(
				const FPcgAnimationGraphRouteProfile& Profile,
				const FGraphCompoundPcgAiTargetRequest& Request,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
					Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentCanonicalSnapshotPtr& OutSnapshot,
				FGraphCompoundRuntimeError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				const bool bAssetCreate =
					FindAssetCreateSpec(Profile, Request) != nullptr
					|| IsPcgDataFromLevelProfile(Profile, Request);
				if (!bAssetCreate && !IsSchemaMutationProfile(Profile, Request))
				{
					const TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
					Binding->SetStringField(TEXT("capability_id"), Profile.GetCapabilityId());
					Binding->SetNumberField(TEXT("input_kind"), static_cast<int32>(Profile.GetInputKind()));
					Binding->SetStringField(TEXT("profile_target_kind"), Profile.GetTargetKind());
					Binding->SetStringField(TEXT("request_target_kind"), Request.GetTargetKind());
					OutError = NativeFailure(TEXT("graph_compound_production_binding_unimplemented"),
						TEXT("No PCG/AI production binding supports this capability, input kind and target kind combination."),
						Request.GetTargetRef(), TEXT("production.binding"), MakeShared<FJsonValueObject>(Binding),
						MakeShared<FJsonValueString>(TEXT("registered PCG/AI asset-create, level-data or schema-mutation binding")));
					OutError.SubjectId = Profile.GetCapabilityId();
					return false;
				}
				if (!IsInGameThread() || Cancellation->IsCancelled())
				{
					const bool bCancelled = Cancellation->IsCancelled();
					OutError = NativeFailure(bCancelled
							? TEXT("graph_compound_pcg_ai_cancelled") : TEXT("graph_compound_pcg_ai_thread_invalid"),
						bCancelled ? TEXT("The PCG/AI snapshot capture was cancelled before execution.")
							: TEXT("PCG/AI snapshot capture must execute on the game thread."),
						Request.GetTargetRef(), bCancelled ? TEXT("cancellation.requested") : TEXT("execution.game_thread"),
						MakeShared<FJsonValueBoolean>(bCancelled), MakeShared<FJsonValueBoolean>(!bCancelled));
					OutError.SubjectId = Profile.GetCapabilityId();
					return false;
				}
				return bAssetCreate
					? TryCreateAbsentSnapshot(
						Request, *Resources, OutSnapshot, OutError)
					: TryCaptureMutationSnapshot(
						Profile, Request, *Resources, OutSnapshot, OutError);
			}

			bool TryExecute(
				const FPcgAnimationGraphRouteProfile& Profile,
				const FGraphCompoundPcgAiOperationRequest& Request,
				const FPlanningPreflightPlanRef& Plan,
				const FUeAgentCallerContext& CallerContext,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
					Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentContractValue& OutResult,
				FGraphCompoundRuntimeError& OutError) override
			{
				OutResult = FUeAgentContractValue(TEXT("{}"));
				OutError = {};
				if (IsPcgDataFromLevelProfile(Profile, Request.GetTarget()))
				{
					return TryExecutePcgDataFromLevel(
						Profile, Request, Plan, CallerContext,
						MoveTemp(Resources), Cancellation, JournalRoot,
						DurableBackend, LastFencingToken,
						OutResult, OutError);
				}
				const FAssetCreateSpec* const Spec =
					FindAssetCreateSpec(Profile, Request);
				if (!Spec)
				{
					if (IsSchemaMutationProfile(Profile, Request.GetTarget()))
					{
						return TryExecuteSchemaMutation(
							Profile, Request, Plan, CallerContext,
							MoveTemp(Resources), Cancellation, JournalRoot,
							NativeHook, DurableBackend, LastFencingToken,
							OutResult, OutError);
					}
					const TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
					Binding->SetStringField(TEXT("capability_id"), Profile.GetCapabilityId());
					Binding->SetNumberField(TEXT("input_kind"), static_cast<int32>(Profile.GetInputKind()));
					Binding->SetStringField(TEXT("profile_target_kind"), Profile.GetTargetKind());
					Binding->SetStringField(TEXT("request_target_kind"), Request.GetTarget().GetTargetKind());
					OutError = NativeFailure(TEXT("graph_compound_production_binding_unimplemented"),
						TEXT("No PCG/AI production binding supports this capability, input kind and target kind combination."),
						Request.GetTargetRef(), TEXT("production.binding"), MakeShared<FJsonValueObject>(Binding),
						MakeShared<FJsonValueString>(TEXT("registered PCG/AI asset-create, level-data or schema-mutation binding")));
					OutError.SubjectId = Profile.GetCapabilityId();
					return false;
				}
				if (!IsInGameThread() || Cancellation->IsCancelled())
				{
					const bool bCancelled = Cancellation->IsCancelled();
					OutError = NativeFailure(bCancelled
							? TEXT("graph_compound_pcg_ai_cancelled") : TEXT("graph_compound_pcg_ai_thread_invalid"),
						bCancelled ? TEXT("The PCG/AI asset creation was cancelled before execution.")
							: TEXT("PCG/AI asset creation must execute on the game thread."),
						Request.GetTargetRef(), bCancelled ? TEXT("cancellation.requested") : TEXT("execution.game_thread"),
						MakeShared<FJsonValueBoolean>(bCancelled), MakeShared<FJsonValueBoolean>(!bCancelled));
					OutError.SubjectId = Profile.GetCapabilityId();
					return false;
				}
				auto FailPlan = [&](const FString& Reason, const FString& Field,
					TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
				{
					OutError = NativeFailure(TEXT("graph_compound_pcg_ai_plan_invalid"),
						Reason, Request.GetTargetRef(), Field, MoveTemp(Actual), MoveTemp(Expected));
					return false;
				};
				const FUeAgentDescriptorIdentity& ActualDescriptor = Plan->GetDescriptorIdentity();
				const FUeAgentDescriptorIdentity& ExpectedDescriptor =
					Profile.GetExecutionContract().GetDescriptorIdentity();
				if (ActualDescriptor != ExpectedDescriptor)
				{
					if (ActualDescriptor.GetDescriptorId() != ExpectedDescriptor.GetDescriptorId())
						return FailPlan(TEXT("The plan descriptor id does not match the route descriptor."),
							TEXT("descriptor_identity.descriptor_id"),
							MakeShared<FJsonValueString>(ActualDescriptor.GetDescriptorId()),
							MakeShared<FJsonValueString>(ExpectedDescriptor.GetDescriptorId()));
					if (ActualDescriptor.GetVersion() != ExpectedDescriptor.GetVersion())
						return FailPlan(TEXT("The plan descriptor version does not match the route descriptor."),
							TEXT("descriptor_identity.version"),
							MakeShared<FJsonValueNumber>(ActualDescriptor.GetVersion()),
							MakeShared<FJsonValueNumber>(ExpectedDescriptor.GetVersion()));
					FailPlan(TEXT("The plan descriptor hash does not match the route descriptor."),
						TEXT("descriptor_identity.descriptor_hash"), nullptr, nullptr);
					OutError.ActualEvidence = MakeShared<FJsonValueString>(ActualDescriptor.GetDescriptorHash());
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedDescriptor.GetDescriptorHash());
					return false;
				}
				const auto& Changes = Plan->GetChangeSet().GetChanges();
				if (Changes.Num() != 1)
					return FailPlan(TEXT("The asset-create plan requires exactly one change."),
						TEXT("changes.count"), MakeShared<FJsonValueNumber>(Changes.Num()), MakeShared<FJsonValueNumber>(1));
				if (Changes[0].GetIntent() != EChangeIntent::Create)
				{
					FailPlan(TEXT("The asset-create plan change must use create intent."),
						TEXT("change.intent"), MakeShared<FJsonValueNumber>(static_cast<int32>(Changes[0].GetIntent())),
						MakeShared<FJsonValueString>(TEXT("create")));
					OutError.OperationIndex = 0;
					return false;
				}
				if (Changes[0].GetIdentity().GetOwnerId() != Request.GetTargetRef())
				{
					FailPlan(TEXT("The asset-create plan change owner differs from the requested asset."),
						TEXT("change.identity.owner_id"), MakeShared<FJsonValueString>(Changes[0].GetIdentity().GetOwnerId()),
						MakeShared<FJsonValueString>(Request.GetTargetRef()));
					OutError.OperationIndex = 0;
					return false;
				}

				FUeAgentCanonicalSnapshotPtr Current;
				if (!TryCreateAbsentSnapshot(
						Request.GetTarget(), *Resources, Current, OutError))
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
					TEXT("graph-compound-pcg-ai:") + Plan->GetRequestId();
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
				FPcgAiNodeGraphAdapterError NativeError;
				FPcgAiNodeGraphNativeMutationReceipt ApplyReceipt;
				FPcgAiNodeGraphBarrierReceipt BarrierReceipt;
				FPcgAiNodeGraphAssetIdentity Identity;
				FString NativeRevision;
				FString NativeEvidence;
				if (Package)
				{
					CreatedAsset = CreateAssetWithExactFactory(
						*Spec, *Package,
						*AssetName(Request.GetTargetRef()), OutError);
				}
				const bool bCaptured = CreatedAsset
					&& CreatedAsset->GetClass() == Spec->AssetClass
					&& NativeHook.TryCaptureOwnedAssetObject(
						*Package, Request.GetTargetKind(),
						*Spec->AssetClass,
						*AssetName(Request.GetTargetRef()), *CreatedAsset,
						ApplyReceipt, BarrierReceipt, Identity,
						NativeRevision, NativeEvidence, NativeError);
				const bool bCreated = bCaptured
					&& BarrierReceipt.IsComplete()
					&& BarrierReceipt.GetErrorCount() == 0;
				if (!bCreated)
				{
					FDiagnostic Failure = !OutError.IsEmpty() ? OutError : NativeError;
					if (Failure.IsEmpty())
					{
						auto FailCreate = [&](const FString& Reason, const FString& Field,
							TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
						{
							Failure = NativeFailure(TEXT("graph_compound_pcg_ai_factory_failed"),
								Reason, Request.GetTargetRef(), Field, MoveTemp(Actual), MoveTemp(Expected));
						};
						if (!Package)
							FailCreate(TEXT("The destination package could not be created for the PCG/AI asset."),
								TEXT("package"), nullptr, MakeShared<FJsonValueString>(Request.GetTargetRef()));
						else if (!CreatedAsset)
							FailCreate(TEXT("The PCG/AI factory returned no asset and no diagnostic."),
								TEXT("asset"), nullptr, ClassPathValue(Spec->AssetClass));
						else if (CreatedAsset->GetClass() != Spec->AssetClass)
							FailCreate(TEXT("The PCG/AI factory returned an asset with a different class."),
								TEXT("asset.class"), ClassPathValue(CreatedAsset->GetClass()), ClassPathValue(Spec->AssetClass));
						else if (!bCaptured)
							FailCreate(TEXT("Native capture of the created PCG/AI asset failed without a diagnostic."),
								TEXT("asset.capture.succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						else if (!BarrierReceipt.IsComplete())
							FailCreate(TEXT("The native barrier for the created PCG/AI asset did not complete."),
								TEXT("barrier.complete"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						else
							FailCreate(TEXT("The native barrier for the created PCG/AI asset reported errors."),
								TEXT("barrier.error_count"), MakeShared<FJsonValueNumber>(BarrierReceipt.GetErrorCount()),
								MakeShared<FJsonValueNumber>(0));
					}
					if (Package) Failure.MutationMayHaveStarted = true;
					CreatedAsset = nullptr;
					Package = nullptr;
					UE::UAI::Contract::FDiagnostic RollbackDetail;
					TryRollbackCreate(
						Request.GetTargetRef(), true,
						Prepared.ToSharedRef(), DurableBackend,
						FencingToken, *Resources, RollbackDetail);
					SetError(OutError, Failure, Request.GetTargetRef(), RollbackDetail);
					return false;
				}

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
				CreatedAsset->GetOutermost()->SetDirtyFlag(false);
				CreatedAsset = nullptr;
				Package = nullptr;

				const FAssetDurableSessionRef Persisted =
					PersistedSession(Prepared.ToSharedRef(), DurableReceipt);
				const FUeAgentCanonicalValuePtr& Expected =
					Plan->GetChangeSet().GetChanges()[0].GetAfter();
				TSharedPtr<FIndependentReadbackService> Readback;
				FVerificationError VerifyError;
				const FString OracleId = ReadbackOracleId(*Spec);
				const TSharedRef<FAssetCreateReadbackOracle,
					ESPMode::ThreadSafe> Oracle =
					MakeShared<FAssetCreateReadbackOracle,
						ESPMode::ThreadSafe>(
							Request.GetTargetRef(), Spec->TargetKind,
							OracleId, *Spec->AssetClass);
				FIndependentReadbackOutcome Outcome;
				if (!Expected.IsValid())
				{
					VerifyError = NativeFailure(TEXT("graph_compound_pcg_ai_readback_failed"),
						TEXT("The asset-create plan has no expected value for independent readback."),
						Request.GetTargetRef(), TEXT("changes/0/after"), nullptr,
						MakeShared<FJsonValueString>(TEXT("planned asset value")));
					VerifyError.OperationIndex = 0;
					VerifyError.Stage = TEXT("verification");
				}
				const bool bReadbackCreated = Expected.IsValid()
					&& FIndependentReadbackService::TryCreate({Oracle}, Readback, VerifyError);
				const bool bObserved = bReadbackCreated && Readback->TryObserve(
					FIndependentReadbackRequest(
						TEXT("graph-compound-pcg-ai:") + Plan->GetRequestId(),
						OracleId, Request.GetTargetRef(), Expected.ToSharedRef()),
					*Resources, Outcome, VerifyError);
				if (!bObserved)
				{
					UE::UAI::Contract::FDiagnostic RollbackDetail;
					TryRollbackCreate(
						Request.GetTargetRef(), true, Persisted,
						DurableBackend, FencingToken,
						*Resources, RollbackDetail);
					OutError = bReadbackCreated ? Outcome.GetFailureDiagnostic(VerifyError) : VerifyError;
					OutError.Append(RollbackDetail);
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetTargetRef();
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
						Request.GetTargetRef(), true, Persisted,
						DurableBackend, FencingToken,
						*Resources, RollbackDetail);
					SetError(OutError, DurableError, Request.GetTargetRef(), RollbackDetail);
					return false;
				}
				return TrySerializeResult(
					Request.GetTargetRef(),
					OutResult, OutError);
			}

		private:
			FString JournalRoot;
			FPcgAiNodeGraphNativeHook NativeHook;
			FPackageAssetDurableBackend DurableBackend;
			int64 LastFencingToken = 0;
		};
	}

	bool TryCreatePcgAiGraphProductionEnvironment(
		const FGraphCompoundPcgAiProductionConfig& Config,
		TSharedPtr<IGraphCompoundPcgAiEnvironment>& OutEnvironment,
		FGraphCompoundRuntimeError& OutError)
	{
		OutEnvironment.Reset();
		OutError = {};
		OutEnvironment = MakeShared<FPcgAiProductionEnvironment>(Config);
		return true;
	}
}
