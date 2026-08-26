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
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQueryGraph.h"
#include "EnvironmentQueryGraphNode_Option.h"
#include "EnvironmentQueryGraphNode_Root.h"
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
#include "PackageTools.h"
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
				UStateTreeFactory* const StateTreeFactory =
					NewObject<UStateTreeFactory>();
				if (StateTreeFactory)
				{
					StateTreeFactory->SetSchemaClass(
						UStateTreeAIComponentSchema::StaticClass());
					Factory = StateTreeFactory;
				}
			}
			else
			{
				UClass* const FactoryClass = LoadObject<UClass>(
					nullptr, *Spec.FactoryClassPath);
				if (FactoryClass && FactoryClass->IsChildOf(UFactory::StaticClass()))
				{
					Factory = NewObject<UFactory>(
						GetTransientPackage(), FactoryClass);
				}
			}
			if (!Factory || Factory->GetClass()->GetPathName()
					!= Spec.FactoryClassPath
				|| Factory->GetSupportedClass() != Spec.AssetClass)
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_ai_factory_unavailable"),
					Package.GetName(), Spec.FactoryClassPath);
				return nullptr;
			}
			UObject* const Created = Factory->FactoryCreateNew(
				Spec.AssetClass, &Package, AssetName,
				RF_Public | RF_Standalone | RF_Transactional,
				nullptr, GWarn);
			if (!Created || Created->GetClass() != Spec.AssetClass)
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_ai_factory_failed"),
					Package.GetName(), Spec.FactoryClassPath);
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
			const FString& PackageName,
			FString& OutFilename)
		{
			OutFilename.Reset();
			return FPackageName::IsValidLongPackageName(PackageName)
				&& !FPackageName::GetPackageMountPoint(PackageName).IsNone()
				&& !FPackageName::GetLongPackageAssetName(PackageName).IsEmpty()
				&& FPackageName::TryConvertLongPackageNameToFilename(
					PackageName,
					OutFilename,
					FPackageName::GetAssetPackageExtension());
		}

		bool TryCheckTargetAbsent(
			const FGraphCompoundPcgAiTargetRequest& Request,
			FString& OutFilename,
			FGraphCompoundRuntimeError& OutError)
		{
			if (!Request.IsValid()
				|| !TryPackageFilename(Request.GetTargetRef(), OutFilename))
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_ai_target_invalid"),
					Request.GetTargetRef());
				return false;
			}
			bool bTargetPresent = false;
			if (!HjsonWorkspace::DomainAuthoring::
					TryInspectObjectTargetPresence(
						Request.GetTargetRef(), bTargetPresent))
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_ai_target_invalid"),
					Request.GetTargetRef());
				return false;
			}
			if (bTargetPresent)
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_ai_target_already_exists"),
					Request.GetTargetRef());
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
			OutSnapshot.Reset();
			FString Filename;
			if (!TryCheckTargetAbsent(Request, Filename, OutError))
			{
				return false;
			}
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalSnapshot::TryCreate(
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
					{}, Resources, OutSnapshot, Error)
				|| !OutSnapshot.IsValid())
			{
				SetError(OutError, Error.Code,
					Request.GetTargetRef(), Error.Path);
				return false;
			}
			return true;
		}

		bool TryUnloadTarget(
			const FString& TargetRef,
			const bool bNotifyAssetRegistry,
			FString& OutDetail)
		{
			OutDetail.Reset();
			const FString TargetPackageName = PackageName(TargetRef);
			UPackage* const Package = FindPackage(nullptr, *TargetPackageName);
			if (!Package)
			{
				return true;
			}
			if (bNotifyAssetRegistry)
			{
				if (UObject* const Asset = FindTopLevelAsset(*Package, TargetRef))
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
			if (FindPackage(nullptr, *TargetPackageName))
			{
				OutDetail = TEXT("package remains loaded after unload");
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

		bool HasOnlyObjectFields(
			const FUeAgentCanonicalValue& Object,
			std::initializer_list<const TCHAR*> Fields)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return false;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				bool bAllowed = false;
				for (const TCHAR* const Field : Fields)
				{
					if (Item.Key == Field)
					{
						bAllowed = true;
						break;
					}
				}
				if (!bAllowed)
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

		bool TryCanonicalIntegerField(
			const FUeAgentCanonicalValue& Object,
			const TCHAR* Field,
			int32& OutValue)
		{
			const FUeAgentCanonicalValue* const Value =
				FindObjectValue(Object, Field);
			if (!Value
				|| (Value->GetKind() != EUeAgentValueKind::Integer
					&& Value->GetKind() != EUeAgentValueKind::Decimal))
			{
				return false;
			}
			const double Parsed = FCString::Atod(*Value->GetLexeme());
			if (!FMath::IsFinite(Parsed)
				|| Parsed < 0.0 || Parsed > static_cast<double>(MAX_int32)
				|| !FMath::IsNearlyEqual(
					Parsed, FMath::RoundToDouble(Parsed)))
			{
				return false;
			}
			OutValue = static_cast<int32>(Parsed);
			return true;
		}

		bool TryCanonicalSignedIntegerField(
			const FUeAgentCanonicalValue& Object,
			const TCHAR* Field,
			int32& OutValue)
		{
			const FUeAgentCanonicalValue* const Value =
				FindObjectValue(Object, Field);
			if (!Value
				|| (Value->GetKind() != EUeAgentValueKind::Integer
					&& Value->GetKind() != EUeAgentValueKind::Decimal))
			{
				return false;
			}
			const double Parsed = FCString::Atod(*Value->GetLexeme());
			if (!FMath::IsFinite(Parsed)
				|| Parsed < static_cast<double>(MIN_int32)
				|| Parsed > static_cast<double>(MAX_int32)
				|| !FMath::IsNearlyEqual(
					Parsed, FMath::RoundToDouble(Parsed)))
			{
				return false;
			}
			OutValue = static_cast<int32>(Parsed);
			return true;
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
			OutValue = FCString::Atod(*Value.GetLexeme());
			return FMath::IsFinite(OutValue);
		}

		bool TryCanonicalVector2Field(
			const FUeAgentCanonicalValue& Object,
			const TCHAR* Field,
			float& OutX,
			float& OutY)
		{
			const FUeAgentCanonicalValue* const Value =
				FindObjectValue(Object, Field);
			if (!Value || Value->GetKind() != EUeAgentValueKind::Array
				|| Value->GetArrayItems().Num() != 2)
			{
				return false;
			}
			double X = 0.0;
			double Y = 0.0;
			if (!TryCanonicalDouble(Value->GetArrayItems()[0].Get(), X)
				|| !TryCanonicalDouble(Value->GetArrayItems()[1].Get(), Y)
				|| X < -static_cast<double>(MAX_flt)
				|| X > static_cast<double>(MAX_flt)
				|| Y < -static_cast<double>(MAX_flt)
				|| Y > static_cast<double>(MAX_flt))
			{
				return false;
			}
			OutX = static_cast<float>(X);
			OutY = static_cast<float>(Y);
			return true;
		}

		bool TryMakeCanonicalDouble(
			const double Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			if (!FMath::IsFinite(Value)
				|| !FUeAgentCanonicalValue::TryDecimal(
					FString::Printf(TEXT("%.17g"), Value),
					Resources, OutValue, StateError))
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_parameter_value_invalid"),
					FString(), StateError.Detail);
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

		bool TryCanonicalStringArrayField(
			const FUeAgentCanonicalValue& Object,
			const TCHAR* Field,
			TArray<FString>& OutValues)
		{
			OutValues.Reset();
			const FUeAgentCanonicalValue* const Value =
				FindObjectValue(Object, Field);
			if (!Value || Value->GetKind() != EUeAgentValueKind::Array)
			{
				return false;
			}
			for (const FUeAgentCanonicalValueRef& Item : Value->GetArrayItems())
			{
				if (Item->GetKind() != EUeAgentValueKind::String
					|| Item->GetText().IsEmpty())
				{
					OutValues.Reset();
					return false;
				}
				OutValues.Add(Item->GetText());
			}
			return true;
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
			TMap<FGuid, FString>& OutParents)
		{
			OutStates.Reset();
			OutParents.Reset();
			struct FPendingState
			{
				UStateTreeState* State = nullptr;
				FString ParentIdentity;
			};
			TArray<FPendingState> Pending;
			TSet<const UStateTreeState*> Visited;
			for (int32 Index = EditorData.SubTrees.Num() - 1; Index >= 0; --Index)
			{
				Pending.Add({EditorData.SubTrees[Index], TEXT("root")});
			}
			while (!Pending.IsEmpty())
			{
				const FPendingState Current = Pending.Pop(EAllowShrinking::No);
				if (!Current.State || Visited.Contains(Current.State)
					|| !Current.State->ID.IsValid()
					|| OutStates.Contains(Current.State->ID))
				{
					return false;
				}
				Visited.Add(Current.State);
				OutStates.Add(Current.State->ID, Current.State);
				OutParents.Add(Current.State->ID, Current.ParentIdentity);
				const FString Parent = StateTreeStateSemanticId(Current.State->ID);
				for (int32 Index = Current.State->Children.Num() - 1;
					Index >= 0; --Index)
				{
					Pending.Add({Current.State->Children[Index], Parent});
				}
			}
			return true;
		}

		bool TryCanonicalObject(
			const TSharedRef<FJsonObject>& Object,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError)
		{
			FString Error;
			if (!CanonicalJson::TryFromJsonValue(
					MakeShared<FJsonValueObject>(Object),
					Resources, OutValue, Error)
				|| !OutValue.IsValid())
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_ai_state_value_invalid"),
					FString(), Error);
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
				SetError(OutError,
					TEXT("graph_compound_pcg_data_asset_content_invalid"),
					DataAsset.GetPathName());
				return false;
			}
			const UPCGPointData* const PointData = Cast<UPCGPointData>(
				DataAsset.Data.TaggedData[0].Data.Get());
			if (!PointData
				|| PointData->GetClass() != UPCGPointData::StaticClass()
				|| !DataAsset.ObjectPath.IsValid())
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_data_asset_content_invalid"),
					DataAsset.GetPathName());
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

		FString EqsOptionSemanticId(const UEdGraphNode& Node)
		{
			return Node.NodeGuid.IsValid()
				? TEXT("node:") + Node.NodeGuid.ToString(
					EGuidFormats::DigitsWithHyphensLower)
				: FString();
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
			OutValues.Reset();
			OutGeneration.Reset();
			UObject* const Target = IsPcgComponentMutationProfile(Profile)
				? FindExactLoadedPcgComponent(TargetRef)
				: LoadObject<UObject>(nullptr, *TargetRef);
			if (!Target || Target->GetPathName() != TargetRef)
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_ai_target_invalid"), TargetRef);
				return false;
			}

			if (Profile.GetCapabilityId()
				== TEXT("ai.behavior_tree.graph.apply"))
			{
				UBehaviorTree* const Tree = Cast<UBehaviorTree>(Target);
				if (!Tree)
				{
					SetError(OutError,
						TEXT("graph_compound_pcg_ai_target_invalid"), TargetRef);
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
						SetError(OutError,
							TEXT("graph_compound_behavior_tree_blackboard_binding_invalid"),
							TargetRef,
							StateError.Detail);
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
					SetError(OutError,
						TEXT("graph_compound_pcg_ai_target_invalid"), TargetRef);
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
					SetError(OutError,
						TEXT("graph_compound_pcg_ai_target_invalid"), TargetRef);
					return false;
				}
#if WITH_EDITORONLY_DATA
				const UEnvironmentQueryGraph* const Graph =
					Cast<UEnvironmentQueryGraph>(Query->EdGraph);
				if (Graph)
				{
					for (const UEdGraphNode* Node : Graph->Nodes)
					{
						const UEnvironmentQueryGraphNode_Option* const OptionNode =
							Cast<UEnvironmentQueryGraphNode_Option>(Node);
						const UEnvQueryOption* const Option = OptionNode
							? Cast<UEnvQueryOption>(OptionNode->NodeInstance.Get())
							: nullptr;
						if (!OptionNode || !OptionNode->NodeGuid.IsValid()
							|| !Option || !Option->Generator)
						{
							continue;
						}
						const FString SemanticId =
							EqsOptionSemanticId(*OptionNode);
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
				if (!Tree || !EditorData
					|| !TryCollectStateTreeStates(*EditorData, States, Parents))
				{
					SetError(OutError,
						TEXT("graph_compound_state_tree_hierarchy_invalid"),
						TargetRef);
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
								SetError(OutError,
									TEXT("graph_compound_state_tree_hierarchy_invalid"),
									StateIdentity);
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
				SetError(OutError,
					TEXT("graph_compound_state_tree_editor_data_unavailable"),
					TargetRef);
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
					SetError(OutError,
						TEXT("graph_compound_pcg_ai_target_invalid"), TargetRef);
					return false;
				}
				for (const FSmartObjectSlotDefinition& Slot
					: Definition->GetSlots())
				{
#if WITH_EDITORONLY_DATA
					if (!Slot.ID.IsValid())
					{
						continue;
					}
					const FString SlotGuid = Slot.ID.ToString(
						EGuidFormats::DigitsWithHyphensLower);
#else
					continue;
#endif
					const USmartObjectBehaviorDefinition* const Behavior =
						Slot.BehaviorDefinitions.IsEmpty()
							? nullptr : Slot.BehaviorDefinitions[0].Get();
					const TSharedRef<FJsonObject> ValueObject =
						MakeShared<FJsonObject>();
					ValueObject->SetStringField(
						TEXT("behavior_type_ref"), Behavior
							? Behavior->GetClass()->GetPathName() : TEXT("none"));
					ValueObject->SetStringField(
						TEXT("data_type_ref"),
						FSmartObjectSlotDefinition::StaticStruct()->GetPathName());
					FUeAgentCanonicalValuePtr Value;
					if (!TryCanonicalObject(
							ValueObject, Resources, Value, OutError))
					{
						return false;
					}
					OutValues.Add({
						TEXT("item:slot:") + SlotGuid, Value, true});
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
				if (!Component || !Owner || !World
					|| Owner->GetWorld() != World
					|| !GEngine || !GEngine->GetWorldContextFromWorld(World)
					|| !TryPublicPcgGenerationPolicy(
						Component->GenerationTrigger, PublicPolicy))
				{
					SetError(OutError,
						TEXT("graph_compound_pcg_component_world_invalid"),
						TargetRef);
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
				if (!DataAsset
					|| !TryCapturePcgDataAssetValue(
						*DataAsset, Resources, SemanticId, Value, OutError))
				{
					if (OutError.Code.IsEmpty())
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_ai_target_invalid"),
							TargetRef);
					}
					return false;
				}
				OutValues.Add({MoveTemp(SemanticId), Value, true});
			}
			else if (Profile.GetCapabilityId() == TEXT("pcg.graph.apply")
				|| Profile.GetCapabilityId()
					== TEXT("pcg.graph_parameters.apply"))
			{
				UPCGGraph* const Graph = Cast<UPCGGraph>(Target);
				if (!Graph)
				{
					SetError(OutError,
						TEXT("graph_compound_pcg_ai_target_invalid"), TargetRef);
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
						if (!Readback.IsValid()
							|| !TryMakeCanonicalDouble(
								Readback.GetValue(), Resources, Value, OutError))
						{
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
					SetError(OutError,
						TEXT("graph_compound_pcg_ai_target_invalid"), TargetRef);
					return false;
				}
				FUeAgentCanonicalValuePtr BindingValue;
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryString(
						Instance->Graph->GetPathName(), Resources,
						BindingValue, StateError))
				{
					SetError(OutError,
						TEXT("graph_compound_pcg_graph_binding_invalid"),
						TargetRef, StateError.Detail);
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
						if (!Readback.IsValid()
							|| !TryMakeCanonicalDouble(
								Readback.GetValue(), Resources, Value, OutError))
						{
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
				SetError(OutError,
					TEXT("graph_compound_production_binding_unimplemented"),
					Profile.GetCapabilityId());
				return false;
			}

			TArray<FString> RevisionLines;
			for (const FCapturedMutationValue& Captured : OutValues)
			{
				TSharedPtr<FJsonValue> JsonValue;
				FString Canonical;
				FString JsonError;
				if (!CanonicalJson::TryToJsonValue(
						*Captured.Value, JsonValue, JsonError)
					|| !Json::TryCanonicalize(JsonValue, Canonical, JsonError))
				{
					SetError(OutError,
						TEXT("graph_compound_pcg_ai_state_value_invalid"),
						Captured.SemanticId, JsonError);
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
					SetError(OutError,
						TEXT("graph_compound_pcg_ai_state_entry_invalid"),
						Captured.SemanticId);
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
				SetError(OutError, StateError.Code,
					Request.GetTargetRef(), StateError.Detail);
				return false;
			}
			return OutSnapshot.IsValid();
		}

		bool TryRollbackCreate(
			const FString& TargetRef,
			const bool bNotifyAssetRegistry,
			const FAssetDurableSessionRef& Session,
			FPackageAssetDurableBackend& Backend,
			const int64 FencingToken,
			FUeAgentResourceTracker& Resources,
			FString& OutDetail)
		{
			FString UnloadDetail;
			const bool bMemoryClean = TryUnloadTarget(
				TargetRef, bNotifyAssetRegistry, UnloadDetail);
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

		bool TryInitializeStateTreeBlueprintNode(
			FStateTreeEditorNode& OutNode,
			UObject& Outer,
			UClass* NodeClass,
			UScriptStruct* NodeStruct,
			const FString& Placement,
			FGraphCompoundRuntimeError& OutError)
		{
			OutNode.Reset();
			if (NodeStruct)
			{
				const UScriptStruct* RequiredBase = nullptr;
				if (Placement == TEXT("state_task")
					|| Placement == TEXT("global_task"))
				{
					RequiredBase = FStateTreeTaskBase::StaticStruct();
				}
				else if (Placement == TEXT("state_condition"))
				{
					RequiredBase = FStateTreeConditionBase::StaticStruct();
				}
				else if (Placement == TEXT("evaluator"))
				{
					RequiredBase = FStateTreeEvaluatorBase::StaticStruct();
				}
				if (!RequiredBase || !NodeStruct->IsChildOf(RequiredBase))
				{
					SetError(OutError,
						TEXT("graph_compound_state_tree_node_struct_invalid"),
						NodeStruct->GetPathName(), Placement);
					return false;
				}
				OutNode.Node.InitializeAs(NodeStruct);
				const FStateTreeNodeBase* NodeBase =
					reinterpret_cast<const FStateTreeNodeBase*>(
						OutNode.Node.GetMemory());
				const UScriptStruct* InstanceType = NodeBase
					? Cast<UScriptStruct>(NodeBase->GetInstanceDataType())
					: nullptr;
				if (InstanceType)
				{
					OutNode.Instance.InitializeAs(InstanceType);
				}
				return true;
			}
			if (!NodeClass)
			{
				SetError(OutError,
					TEXT("graph_compound_state_tree_node_type_invalid"),
					Placement);
				return false;
			}
			if (Placement == TEXT("state_task")
				|| Placement == TEXT("global_task"))
			{
				if (!NodeClass->IsChildOf(
					UStateTreeTaskBlueprintBase::StaticClass()))
				{
					SetError(OutError,
						TEXT("graph_compound_state_tree_node_class_invalid"),
						NodeClass->GetPathName(), Placement);
					return false;
				}
				OutNode.Node.InitializeAs<FStateTreeBlueprintTaskWrapper>();
				OutNode.Node.GetMutable<FStateTreeBlueprintTaskWrapper>().TaskClass =
					NodeClass;
				OutNode.InstanceObject = NewObject<UStateTreeTaskBlueprintBase>(
					&Outer, NodeClass, NAME_None, RF_Transactional);
			}
			else if (Placement == TEXT("state_condition"))
			{
				if (!NodeClass->IsChildOf(
					UStateTreeConditionBlueprintBase::StaticClass()))
				{
					SetError(OutError,
						TEXT("graph_compound_state_tree_node_class_invalid"),
						NodeClass->GetPathName(), Placement);
					return false;
				}
				OutNode.Node.InitializeAs<FStateTreeBlueprintConditionWrapper>();
				OutNode.Node.GetMutable<FStateTreeBlueprintConditionWrapper>().ConditionClass =
					NodeClass;
				OutNode.InstanceObject = NewObject<UStateTreeConditionBlueprintBase>(
					&Outer, NodeClass, NAME_None, RF_Transactional);
			}
			else if (Placement == TEXT("evaluator"))
			{
				if (!NodeClass->IsChildOf(
					UStateTreeEvaluatorBlueprintBase::StaticClass()))
				{
					SetError(OutError,
						TEXT("graph_compound_state_tree_node_class_invalid"),
						NodeClass->GetPathName(), Placement);
					return false;
				}
				OutNode.Node.InitializeAs<FStateTreeBlueprintEvaluatorWrapper>();
				OutNode.Node.GetMutable<FStateTreeBlueprintEvaluatorWrapper>().EvaluatorClass =
					NodeClass;
				OutNode.InstanceObject = NewObject<UStateTreeEvaluatorBlueprintBase>(
					&Outer, NodeClass, NAME_None, RF_Transactional);
			}
			else
			{
				SetError(OutError,
					TEXT("graph_compound_state_tree_node_placement_invalid"),
					Placement);
				return false;
			}
			if (!OutNode.InstanceObject)
			{
				SetError(OutError,
					TEXT("graph_compound_state_tree_node_create_failed"),
					NodeClass->GetPathName(), Placement);
				return false;
			}
			return true;
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
			const TArray<FGraphCompoundCanonicalEdit>& Edits =
				Request.GetPayload().GetEdits();
			const TArray<FPlannedChange>& Changes =
				Plan->GetChangeSet().GetChanges();
			const bool bPcgGraphInstanceProfile =
				Profile.GetCapabilityId()
					== TEXT("pcg.graph_instance.apply");
			const bool bPcgComponentProfile =
				IsPcgComponentMutationProfile(Profile);
			if (Plan->GetDescriptorIdentity()
					!= Profile.GetExecutionContract().GetDescriptorIdentity()
				|| Edits.IsEmpty()
				|| (!bPcgGraphInstanceProfile
					&& Edits.Num() != Changes.Num())
				|| (bPcgGraphInstanceProfile
					&& (Changes.IsEmpty() || Changes.Num() > Edits.Num())))
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_ai_plan_invalid"),
					Request.GetTargetRef());
				return false;
			}

			TMap<FString, const FPlannedChange*> PlannedById;
			TSet<FString> ProposedNodeIds;
			TMap<FString, FString> ProposedBehaviorNodeCarriers;
			TSet<FString> ProposedStateIds;
			TMap<FString, FString> ProposedStateParents;
			for (const FPlannedChange& Change : Changes)
			{
				if (Change.GetIdentity().GetOwnerId() != Request.GetTargetRef()
					|| PlannedById.Contains(
						Change.GetIdentity().GetSemanticId()))
				{
					SetError(OutError,
						TEXT("graph_compound_pcg_ai_plan_invalid"),
						Change.GetIdentity().GetSemanticId());
					return false;
				}
				PlannedById.Add(
					Change.GetIdentity().GetSemanticId(), &Change);
			}
			for (const FGraphCompoundCanonicalEdit& Edit : Edits)
			{
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
				const FString SemanticId = Edit.GetIdentity().GetSemanticId();
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
					: (bPcgUpdateProfile
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
				if ((!Planned && !bExactPcgBindingNoOp)
					|| (Planned && (!*Planned
						|| (*Planned)->GetIntent() != Edit.GetIntent()))
					|| !bIntentAllowed
					|| Edit.GetIdentity().GetOwnerId() != Request.GetTargetRef()
					|| !Edit.GetProposedAfter().IsValid())
				{
					SetError(OutError,
						TEXT("graph_compound_pcg_ai_plan_invalid"), SemanticId);
					return false;
				}

				FPcgAiSchemaMutationOperation Operation;
				Operation.SemanticId = SemanticId;
				Operation.Value = Edit.GetProposedAfter();
				for (const FUeAgentCanonicalIdentity& Dependency
					: Edit.GetDependencies())
				{
					if (Dependency.GetOwnerId() != Request.GetTargetRef())
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_ai_dependency_invalid"),
							Dependency.GetSemanticId());
						return false;
					}
					Operation.Dependencies.Add(Dependency.GetSemanticId());
				}

				if (Profile.GetCapabilityId()
					== TEXT("ai.behavior_tree.graph.apply"))
				{
					if (!Cast<UBehaviorTree>(&Target))
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_ai_target_invalid"),
							Request.GetTargetRef());
						return false;
					}
					const FUeAgentCanonicalValue& Value = *Operation.Value;
					if (Edit.GetElementKind()
						== EGraphCompoundElementKind::Item)
					{
						FString CarrierType;
						if (SemanticId == TEXT("node:root"))
						{
							if (!Operation.Dependencies.IsEmpty()
								|| !HasExactObjectFields(
									Value, {TEXT("carrier_type_ref")})
								|| !TryCanonicalStringField(Value,
									TEXT("carrier_type_ref"), CarrierType)
								|| CarrierType
									!= TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Root"))
							{
								SetError(OutError,
									TEXT("graph_compound_behavior_tree_root_invalid"),
									SemanticId);
								return false;
							}
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
							if (!SemanticId.StartsWith(TEXT("node:"))
								|| !FGuid::Parse(SemanticId.RightChop(5),
									Operation.NodeGuid)
								|| !Operation.NodeGuid.IsValid()
								|| SemanticId != TEXT("node:")
									+ Operation.NodeGuid.ToString(
										EGuidFormats::DigitsWithHyphensLower)
								|| (bNativeSelector
									? (!HasOnlyObjectFields(
										Value,
										{TEXT("candidate"),
											TEXT("carrier_type_ref"),
											TEXT("payload_type_ref"),
											TEXT("position")})
										|| !HasOnlyObjectFields(
											*Candidate,
											{TEXT("name"), TEXT("id")})
										|| (!bHasCandidateName && !bHasCandidateId))
									: !HasExactObjectFields(
										Value,
										{TEXT("carrier_type_ref"),
											TEXT("payload_type_ref")}))
								|| !TryCanonicalStringField(Value,
									TEXT("carrier_type_ref"), CarrierType)
								|| !TryCanonicalStringField(Value,
									TEXT("payload_type_ref"), PayloadType))
							{
								SetError(OutError,
									TEXT("graph_compound_behavior_tree_node_invalid"),
									SemanticId);
								return false;
							}
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
							if (!RequiredBase
								|| !Operation.PayloadClass
								|| !Operation.PayloadClass->IsChildOf(RequiredBase)
								|| Operation.PayloadClass->HasAnyClassFlags(CLASS_Abstract))
							{
								SetError(OutError,
									TEXT("graph_compound_behavior_tree_payload_invalid"),
									PayloadType);
								return false;
							}
							if (bNativeSelector)
							{
								QueryOwners::AiBehavior::FAiBehaviorCandidate
									ResolvedCandidate;
								QueryOwners::AiBehavior::FAiBehaviorCandidateQueryError
									CandidateError;
								if (!QueryOwners::AiBehavior::TryResolveAiBehaviorCandidate(
										Target,
										CandidateName,
										CandidateId,
										ResolvedCandidate,
										CandidateError)
									|| !ResolvedCandidate.bSupported
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
									SetError(
										OutError,
										CandidateError.Code.IsEmpty()
											? TEXT("graph_compound_behavior_tree_candidate_invalid")
											: CandidateError.Code,
										SemanticId,
										CandidateError.Detail);
									return false;
								}
								Operation.CandidateName = ResolvedCandidate.Name;
								Operation.CandidateId = ResolvedCandidate.Id;
								if (FindObjectValue(Value, TEXT("position"))
									&& !TryCanonicalVector2Field(
										Value,
										TEXT("position"),
										Operation.PositionX,
										Operation.PositionY))
								{
									SetError(
										OutError,
										TEXT("graph_compound_behavior_tree_position_invalid"),
										SemanticId);
									return false;
								}
							}
							const bool bSubNode =
								Operation.Kind == EPcgAiSchemaMutationKind::BehaviorService
								|| Operation.Kind == EPcgAiSchemaMutationKind::BehaviorDecorator;
							if ((!bSubNode && !Operation.Dependencies.IsEmpty())
								|| (bSubNode && (Operation.Dependencies.Num() != 1
									|| !ProposedNodeIds.Contains(Operation.Dependencies[0]))))
							{
								SetError(OutError,
									TEXT("graph_compound_behavior_tree_placement_invalid"),
									SemanticId);
								return false;
							}
							if (bSubNode)
							{
								Operation.OwnerItem = Operation.Dependencies[0];
							}
						}
					}
					else if (Edit.GetElementKind()
						== EGraphCompoundElementKind::Binding)
					{
						if (SemanticId != TEXT("binding:blackboard_asset")
							|| !Operation.Dependencies.IsEmpty()
							|| Value.GetKind() != EUeAgentValueKind::String
							|| Value.GetText().IsEmpty())
						{
							SetError(OutError,
								TEXT("graph_compound_behavior_tree_blackboard_binding_invalid"),
								SemanticId);
							return false;
						}
						Operation.ObjectRef = Value.GetText();
						Operation.Kind =
							EPcgAiSchemaMutationKind::BehaviorBlackboardBinding;
					}
					else if (Edit.GetElementKind()
						== EGraphCompoundElementKind::Relation)
					{
						if (!HasExactObjectFields(Value,
								{TEXT("from_item_ref"),
									TEXT("from_output_index"),
									TEXT("to_item_ref"),
									TEXT("to_input_index")})
							|| !TryCanonicalStringField(Value,
								TEXT("from_item_ref"), Operation.FromItem)
							|| !TryCanonicalIntegerField(Value,
								TEXT("from_output_index"), Operation.FromIndex)
							|| !TryCanonicalStringField(Value,
								TEXT("to_item_ref"), Operation.ToItem)
							|| !TryCanonicalIntegerField(Value,
								TEXT("to_input_index"), Operation.ToIndex)
							|| SemanticId != BehaviorTreeRelationSemanticId(
								Operation.FromItem, Operation.FromIndex,
								Operation.ToItem, Operation.ToIndex)
							|| Operation.Dependencies.Num() != 2
							|| !Operation.Dependencies.Contains(Operation.FromItem)
							|| !Operation.Dependencies.Contains(Operation.ToItem)
							|| !ProposedNodeIds.Contains(Operation.FromItem)
							|| !ProposedNodeIds.Contains(Operation.ToItem))
						{
							SetError(OutError,
								TEXT("graph_compound_behavior_tree_relation_invalid"),
								SemanticId);
							return false;
						}
						if (Operation.FromItem == TEXT("node:root")
							&& ProposedBehaviorNodeCarriers.FindRef(
								Operation.ToItem)
								!= TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Composite"))
						{
							SetError(
								OutError,
								TEXT("graph_compound_behavior_tree_root_target_invalid"),
								Operation.ToItem,
								TEXT("the BehaviorTree root output requires a composite node"));
							return false;
						}
						Operation.Kind =
							EPcgAiSchemaMutationKind::BehaviorRelation;
					}
					else
					{
						SetError(OutError,
							TEXT("graph_compound_behavior_tree_edit_kind_invalid"),
							SemanticId);
						return false;
					}
				}
				else if (Profile.GetCapabilityId()
					== TEXT("ai.blackboard.schema.apply"))
				{
					UBlackboardData* const Blackboard =
						Cast<UBlackboardData>(&Target);
					FString KeyType;
					FString Ownership;
					if (!Blackboard
						|| Edit.GetElementKind()
							!= EGraphCompoundElementKind::Item
						|| !Operation.Dependencies.IsEmpty()
						|| !HasExactObjectFields(*Operation.Value,
							{TEXT("key_type_ref"), TEXT("ownership")})
						|| !TryCanonicalStringField(*Operation.Value,
							TEXT("key_type_ref"), KeyType)
						|| !TryCanonicalStringField(*Operation.Value,
							TEXT("ownership"), Ownership)
						|| Ownership != TEXT("local"))
					{
						SetError(OutError,
							TEXT("graph_compound_blackboard_entry_invalid"),
							SemanticId);
						return false;
					}
					const FString Prefix = TEXT("item:blackboard_entry:");
					const FString Suffix = TEXT(":") + KeyType
						+ TEXT(":") + Ownership;
					if (!SemanticId.StartsWith(Prefix)
						|| !SemanticId.EndsWith(Suffix)
						|| SemanticId.Len() <= Prefix.Len() + Suffix.Len())
					{
						SetError(OutError,
							TEXT("graph_compound_blackboard_identity_invalid"),
							SemanticId);
						return false;
					}
					Operation.EntryName = FName(*SemanticId.Mid(
						Prefix.Len(), SemanticId.Len()
							- Prefix.Len() - Suffix.Len()));
					Operation.KeyTypeClass =
						LoadObject<UClass>(nullptr, *KeyType);
					if (Operation.EntryName.IsNone()
						|| !Operation.KeyTypeClass
						|| !Operation.KeyTypeClass->IsChildOf(
							UBlackboardKeyType::StaticClass())
						|| Operation.KeyTypeClass->HasAnyClassFlags(CLASS_Abstract)
						|| HasEntryName(*Blackboard, Operation.EntryName))
					{
						SetError(OutError,
							TEXT("graph_compound_blackboard_entry_conflict"),
							SemanticId);
						return false;
					}
					Operation.Kind =
						EPcgAiSchemaMutationKind::BlackboardEntry;
				}
				else if (Profile.GetCapabilityId()
					== TEXT("ai.eqs.graph.apply"))
				{
					if (!Cast<UEnvQuery>(&Target))
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_ai_target_invalid"),
							Request.GetTargetRef());
						return false;
					}
					const FUeAgentCanonicalValue& Value = *Operation.Value;
					if (Edit.GetElementKind()
						== EGraphCompoundElementKind::Item)
					{
						FString CarrierType;
						FString PayloadType;
						if (!Operation.Dependencies.IsEmpty()
							|| !SemanticId.StartsWith(TEXT("node:"))
							|| !FGuid::Parse(SemanticId.RightChop(5),
								Operation.NodeGuid)
							|| !Operation.NodeGuid.IsValid()
							|| SemanticId != TEXT("node:")
								+ Operation.NodeGuid.ToString(
									EGuidFormats::DigitsWithHyphensLower)
							|| !HasExactObjectFields(Value,
								{TEXT("carrier_type_ref"),
									TEXT("payload_type_ref")})
							|| !TryCanonicalStringField(Value,
								TEXT("carrier_type_ref"), CarrierType)
							|| CarrierType
								!= TEXT("/Script/EnvironmentQueryEditor.EnvironmentQueryGraphNode_Option")
							|| !TryCanonicalStringField(Value,
								TEXT("payload_type_ref"), PayloadType))
						{
							SetError(OutError,
								TEXT("graph_compound_eqs_option_invalid"), SemanticId);
							return false;
						}
						Operation.PayloadClass =
							LoadObject<UClass>(nullptr, *PayloadType);
						if (!Operation.PayloadClass
							|| !Operation.PayloadClass->IsChildOf(
								UEnvQueryGenerator::StaticClass())
							|| Operation.PayloadClass->HasAnyClassFlags(CLASS_Abstract))
						{
							SetError(OutError,
								TEXT("graph_compound_eqs_generator_invalid"),
								PayloadType);
							return false;
						}
						Operation.Kind = EPcgAiSchemaMutationKind::EqsOption;
					}
					else if (Edit.GetElementKind()
						== EGraphCompoundElementKind::Property)
					{
						FString PropertyPath;
						FString ValueType;
						if (Operation.Dependencies.Num() != 1
							|| !HasExactObjectFields(Value,
								{TEXT("property_path"), TEXT("value_type"),
									TEXT("value")})
							|| !TryCanonicalStringField(Value,
								TEXT("property_path"), PropertyPath)
							|| PropertyPath != TEXT("NodePosX")
							|| !TryCanonicalStringField(Value,
								TEXT("value_type"), ValueType)
							|| ValueType != TEXT("int32")
							|| !TryCanonicalSignedIntegerField(Value,
								TEXT("value"), Operation.SignedIntegerValue))
						{
							SetError(OutError,
								TEXT("graph_compound_eqs_property_invalid"), SemanticId);
							return false;
						}
						Operation.FromItem = Operation.Dependencies[0];
						if (!ProposedNodeIds.Contains(Operation.FromItem)
							|| SemanticId != TEXT("property:")
								+ Operation.FromItem + TEXT(":NodePosX"))
						{
							SetError(OutError,
								TEXT("graph_compound_eqs_property_identity_invalid"),
								SemanticId);
							return false;
						}
						Operation.Kind = EPcgAiSchemaMutationKind::EqsProperty;
					}
					else
					{
						SetError(OutError,
							TEXT("graph_compound_eqs_edit_kind_invalid"), SemanticId);
						return false;
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
					if (!Tree || !EditorData
						|| !TryCollectStateTreeStates(*EditorData, States, Parents))
					{
						SetError(OutError,
							TEXT("graph_compound_state_tree_hierarchy_invalid"),
							Request.GetTargetRef());
						return false;
					}
					const FUeAgentCanonicalValue& Value = *Operation.Value;
					if (Edit.GetElementKind()
						== EGraphCompoundElementKind::Item)
					{
						FString OwnerState;
						FString NodeClass;
						if (!SemanticId.StartsWith(TEXT("node:"))
							|| !FGuid::Parse(SemanticId.RightChop(5),
								Operation.NodeGuid)
							|| !Operation.NodeGuid.IsValid()
							|| SemanticId != TEXT("node:")
								+ Operation.NodeGuid.ToString(
									EGuidFormats::DigitsWithHyphensLower)
							|| !HasExactObjectFields(Value,
								{TEXT("owner_state_ref"),
									TEXT("node_class_ref"),
									TEXT("placement")})
							|| !TryCanonicalStringField(Value,
								TEXT("owner_state_ref"), OwnerState)
							|| !TryCanonicalStringField(Value,
								TEXT("node_class_ref"), NodeClass)
							|| !TryCanonicalStringField(Value,
								TEXT("placement"), Operation.Placement))
						{
							SetError(OutError,
								TEXT("graph_compound_state_tree_node_invalid"),
								SemanticId);
							return false;
						}
						const bool bStateOwned =
							Operation.Placement == TEXT("state_task")
							|| Operation.Placement == TEXT("state_condition");
						if ((bStateOwned
								&& (Operation.Dependencies.Num() != 1
									|| Operation.Dependencies[0] != OwnerState
									|| !TryParseStateTreeStateIdentity(
										OwnerState, Operation.StateGuid)
									|| (!States.Contains(Operation.StateGuid)
										&& !ProposedStateIds.Contains(OwnerState))))
							|| (!bStateOwned
								&& (OwnerState != TEXT("global")
									|| !Operation.Dependencies.IsEmpty()
									|| (Operation.Placement != TEXT("global_task")
										&& Operation.Placement != TEXT("evaluator")))))
						{
							SetError(OutError,
								TEXT("graph_compound_state_tree_node_placement_invalid"),
								SemanticId, OwnerState);
							return false;
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
							SetError(OutError,
								TEXT("graph_compound_state_tree_node_class_invalid"),
								NodeClass);
							return false;
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
						if (!Operation.Dependencies.IsEmpty()
							|| !TryParseStateTreeStateIdentity(
								SemanticId, Operation.StateGuid)
							|| States.Contains(Operation.StateGuid)
							|| !HasExactObjectFields(Value,
								{TEXT("native_guid"), TEXT("parent_identity"),
									TEXT("payload_type_ref")})
							|| !TryCanonicalStringField(Value,
								TEXT("native_guid"), NativeGuid)
							|| NativeGuid != Operation.StateGuid.ToString(
								EGuidFormats::DigitsWithHyphensLower)
							|| !TryCanonicalStringField(Value,
								TEXT("parent_identity"), ParentIdentity)
							|| !TryParseStateTreeStateIdentity(
								ParentIdentity, Operation.ParentStateGuid)
							|| !States.Contains(Operation.ParentStateGuid)
							|| !TryCanonicalStringField(Value,
								TEXT("payload_type_ref"), PayloadType)
							|| PayloadType != UStateTreeState::StaticClass()->GetPathName())
						{
							SetError(OutError,
								TEXT("graph_compound_state_tree_hierarchy_invalid"),
								SemanticId);
							return false;
						}
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
						if (!HasExactObjectFields(Value,
								{TEXT("owner_identity"),
									TEXT("ordered_identities")})
							|| !TryCanonicalStringField(Value,
								TEXT("owner_identity"), OwnerIdentity)
							|| !TryParseStateTreeStateIdentity(
								OwnerIdentity, Operation.ParentStateGuid)
							|| !States.Contains(Operation.ParentStateGuid)
							|| SemanticId != Prefix + OwnerIdentity + Suffix
							|| !TryCanonicalStringArrayField(Value,
								TEXT("ordered_identities"), OrderedIdentities))
						{
							SetError(OutError,
								TEXT("graph_compound_state_tree_order_invalid"),
								SemanticId);
							return false;
						}
						TSet<FString> OrderedSet;
						TSet<FString> RequiredDependencies;
						for (const FString& OrderedIdentity : OrderedIdentities)
						{
							FGuid OrderedGuid;
							if (OrderedSet.Contains(OrderedIdentity)
								|| !TryParseStateTreeStateIdentity(
									OrderedIdentity, OrderedGuid))
							{
								SetError(OutError,
									TEXT("graph_compound_state_tree_order_invalid"),
									SemanticId);
								return false;
							}
							OrderedSet.Add(OrderedIdentity);
							if (UStateTreeState* const* Existing = States.Find(OrderedGuid))
							{
								if (!(*Existing)
									|| Parents.FindRef(OrderedGuid) != OwnerIdentity)
								{
									SetError(OutError,
										TEXT("graph_compound_state_tree_order_invalid"),
										SemanticId);
									return false;
								}
							}
							else if (ProposedStateIds.Contains(OrderedIdentity)
								&& ProposedStateParents.FindRef(OrderedIdentity)
									== OwnerIdentity)
							{
								RequiredDependencies.Add(OrderedIdentity);
							}
							else
							{
								SetError(OutError,
									TEXT("graph_compound_state_tree_order_invalid"),
									SemanticId);
								return false;
							}
							Operation.OrderedStateGuids.Add(OrderedGuid);
						}
						for (const TPair<FGuid, UStateTreeState*>& Pair : States)
						{
							if (Parents.FindRef(Pair.Key) == OwnerIdentity
								&& !OrderedSet.Contains(
									StateTreeStateSemanticId(Pair.Key)))
							{
								SetError(OutError,
									TEXT("graph_compound_state_tree_order_invalid"),
									SemanticId);
								return false;
							}
						}
						for (const TPair<FString, FString>& Pair : ProposedStateParents)
						{
							if (Pair.Value == OwnerIdentity
								&& !OrderedSet.Contains(Pair.Key))
							{
								SetError(OutError,
									TEXT("graph_compound_state_tree_order_invalid"),
									SemanticId);
								return false;
							}
						}
						if (Operation.Dependencies.Num()
								!= RequiredDependencies.Num())
						{
							SetError(OutError,
								TEXT("graph_compound_state_tree_order_invalid"),
								SemanticId);
							return false;
						}
						for (const FString& Dependency : Operation.Dependencies)
						{
							if (!RequiredDependencies.Contains(Dependency))
							{
								SetError(OutError,
									TEXT("graph_compound_state_tree_order_invalid"),
									SemanticId);
								return false;
							}
						}
						Operation.Kind = EPcgAiSchemaMutationKind::StateTreeOrder;
					}
					else if (Edit.GetElementKind()
						== EGraphCompoundElementKind::Property)
					{
						FString PropertyPath;
						FString ValueType;
						FString StateName;
						if (Operation.Dependencies.Num() != 1
							|| !ProposedStateIds.Contains(Operation.Dependencies[0])
							|| SemanticId != TEXT("property:")
								+ Operation.Dependencies[0] + TEXT(":Name")
							|| !TryParseStateTreeStateIdentity(
								Operation.Dependencies[0], Operation.StateGuid)
							|| !HasExactObjectFields(Value,
								{TEXT("property_path"), TEXT("value_type"),
									TEXT("value")})
							|| !TryCanonicalStringField(Value,
								TEXT("property_path"), PropertyPath)
							|| PropertyPath != TEXT("Name")
							|| !TryCanonicalStringField(Value,
								TEXT("value_type"), ValueType)
							|| ValueType != TEXT("name")
							|| !TryCanonicalStringField(Value,
								TEXT("value"), StateName))
						{
							SetError(OutError,
								TEXT("graph_compound_state_tree_property_invalid"),
								SemanticId);
							return false;
						}
						Operation.StateName = FName(*StateName);
						if (Operation.StateName.IsNone())
						{
							SetError(OutError,
								TEXT("graph_compound_state_tree_property_invalid"),
								SemanticId);
							return false;
						}
						Operation.Kind = EPcgAiSchemaMutationKind::StateTreeProperty;
					}
					else
					{
						SetError(OutError,
							TEXT("graph_compound_state_tree_edit_kind_invalid"),
							SemanticId);
						return false;
					}
#else
					SetError(OutError,
						TEXT("graph_compound_state_tree_editor_data_unavailable"),
						Request.GetTargetRef());
					return false;
#endif
				}
				else if (Profile.GetCapabilityId()
					== TEXT("ai.smart_object_definition.apply"))
				{
					FString BehaviorType;
					FString DataType;
					const FString Prefix = TEXT("item:slot:");
					if (!Cast<USmartObjectDefinition>(&Target)
						|| Edit.GetElementKind()
							!= EGraphCompoundElementKind::Item
						|| !Operation.Dependencies.IsEmpty()
						|| !SemanticId.StartsWith(Prefix)
						|| !FGuid::Parse(SemanticId.RightChop(Prefix.Len()),
							Operation.SlotGuid)
						|| !Operation.SlotGuid.IsValid()
						|| SemanticId != Prefix
							+ Operation.SlotGuid.ToString(
								EGuidFormats::DigitsWithHyphensLower)
						|| !HasExactObjectFields(*Operation.Value,
							{TEXT("behavior_type_ref"), TEXT("data_type_ref")})
						|| !TryCanonicalStringField(*Operation.Value,
							TEXT("behavior_type_ref"), BehaviorType)
						|| !TryCanonicalStringField(*Operation.Value,
							TEXT("data_type_ref"), DataType)
						|| DataType
							!= FSmartObjectSlotDefinition::StaticStruct()->GetPathName())
					{
						SetError(OutError,
							TEXT("graph_compound_smart_object_slot_invalid"),
							SemanticId);
						return false;
					}
					Operation.BehaviorClass =
						LoadObject<UClass>(nullptr, *BehaviorType);
					if (!Operation.BehaviorClass
						|| !Operation.BehaviorClass->IsChildOf(
							USmartObjectBehaviorDefinition::StaticClass())
						|| Operation.BehaviorClass->HasAnyClassFlags(CLASS_Abstract))
					{
						SetError(OutError,
							TEXT("graph_compound_smart_object_behavior_invalid"),
							BehaviorType);
						return false;
					}
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
					if (!Component || !Owner || !World
						|| Owner->GetWorld() != World
						|| !GEngine || !GEngine->GetWorldContextFromWorld(World)
						|| Edit.GetElementKind()
							!= EGraphCompoundElementKind::Binding
						|| SemanticId != TEXT("binding:pcg_component_graph")
						|| !Operation.Dependencies.IsEmpty()
						|| !HasExactObjectFields(*Operation.Value,
							{TEXT("actor_ref"), TEXT("component_ref"),
								TEXT("generation_policy"), TEXT("graph_ref")})
						|| !TryCanonicalStringField(*Operation.Value,
							TEXT("actor_ref"), Operation.ActorRef)
						|| !TryCanonicalStringField(*Operation.Value,
							TEXT("component_ref"), ComponentRef)
						|| ComponentRef != Component->GetPathName()
						|| !TryCanonicalStringField(*Operation.Value,
							TEXT("generation_policy"),
							Operation.PublicGenerationPolicy)
						|| !TryNativePcgGenerationPolicy(
							Operation.PublicGenerationPolicy,
							Operation.NativeGenerationPolicy,
							Operation.NativeGenerationTrigger)
						|| !TryCanonicalStringField(*Operation.Value,
							TEXT("graph_ref"), GraphRef))
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_component_binding_invalid"),
							SemanticId);
						return false;
					}
					if (bWorldBinding
						&& Operation.ActorRef != Owner->GetPathName())
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_component_owner_mismatch"),
							Operation.ActorRef, Owner->GetPathName());
						return false;
					}
					if (!bWorldBinding)
					{
						Operation.ActorRef = Owner->GetPathName();
					}
					Operation.GraphInterface =
						LoadObject<UPCGGraphInterface>(nullptr, *GraphRef);
					if (!Operation.GraphInterface
						|| Operation.GraphInterface->GetPathName() != GraphRef
						|| !Operation.GraphInterface->GetGraph())
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_component_graph_invalid"),
							GraphRef);
						return false;
					}
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
					FString CandidateName;
					FString CandidateId;
					const bool bHasName = Candidate
						&& TryCanonicalStringField(
							*Candidate, TEXT("name"), CandidateName);
					const bool bHasId = Candidate
						&& TryCanonicalStringField(
							*Candidate, TEXT("id"), CandidateId);
					if (!Graph
						|| Edit.GetElementKind()
							!= EGraphCompoundElementKind::Item
						|| !Operation.Dependencies.IsEmpty()
						|| AuthoredIdentity.IsEmpty()
						|| AuthoredIdentity.Contains(TEXT(":"))
						|| !HasOnlyObjectFields(
							*Operation.Value,
							{TEXT("candidate"), TEXT("position")})
						|| !Candidate
						|| !HasOnlyObjectFields(
							*Candidate, {TEXT("name"), TEXT("id")})
						|| (!bHasName && !bHasId))
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_graph_node_invalid"),
							SemanticId);
						return false;
					}
					for (const UPCGNode* Node : Graph->GetNodes())
					{
						if (Node && Node->HasAuthoredTitle()
							&& Node->GetAuthoredTitleName().ToString()
								== AuthoredIdentity)
						{
							SetError(OutError,
								TEXT("graph_compound_create_target_exists"),
								SemanticId);
							return false;
						}
					}
					if (FindObjectValue(*Operation.Value, TEXT("position"))
						&& !TryCanonicalVector2Field(
							*Operation.Value, TEXT("position"),
							Operation.PositionX, Operation.PositionY))
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_graph_position_invalid"),
							SemanticId);
						return false;
					}
					QueryOwners::Pcg::FPcgNodeCandidate ResolvedCandidate;
					QueryOwners::Pcg::FPcgNodeCandidateQueryError CandidateError;
					if (!QueryOwners::Pcg::TryResolvePcgNodeCandidate(
							*Graph, CandidateName, CandidateId,
							ResolvedCandidate, CandidateError)
						|| !ResolvedCandidate.bSupported)
					{
						SetError(
							OutError,
							CandidateError.Code.IsEmpty()
								? TEXT("pcg_candidate_action_kind_unsupported")
								: CandidateError.Code,
							SemanticId,
							CandidateError.Detail.IsEmpty()
								? ResolvedCandidate.ActionKind
								: CandidateError.Detail);
						return false;
					}
					Operation.CandidateName = ResolvedCandidate.Name;
					Operation.CandidateId = ResolvedCandidate.Id;
					Operation.FromItem = AuthoredIdentity;
					Operation.Kind = EPcgAiSchemaMutationKind::PcgGraphNode;
				}
				else if (Profile.GetCapabilityId()
					== TEXT("pcg.graph_instance.apply"))
				{
					UPCGGraphInstance* const Instance =
						Cast<UPCGGraphInstance>(&Target);
					if (!Instance || !Operation.Dependencies.IsEmpty())
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_graph_instance_invalid"),
							SemanticId);
						return false;
					}
					if (Edit.GetElementKind()
						== EGraphCompoundElementKind::Binding)
					{
						if (SemanticId != TEXT("binding:graph_source")
							|| Operation.Value->GetKind()
								!= EUeAgentValueKind::String)
						{
							SetError(OutError,
								TEXT("graph_compound_pcg_graph_binding_invalid"),
								SemanticId);
							return false;
						}
						const FString GraphRef = Operation.Value->GetText();
						Operation.GraphInterface =
							LoadObject<UPCGGraphInterface>(nullptr, *GraphRef);
						if (!Operation.GraphInterface
							|| Operation.GraphInterface->GetPathName() != GraphRef
							|| !Instance->CanGraphInterfaceBeSet(
								Operation.GraphInterface))
						{
							SetError(OutError,
								TEXT("graph_compound_pcg_graph_binding_invalid"),
								GraphRef);
							return false;
						}
						Operation.Kind =
							EPcgAiSchemaMutationKind::PcgGraphBinding;
					}
					else if (Edit.GetElementKind()
						== EGraphCompoundElementKind::Property)
					{
						if (!TryParsePcgDoubleParameterIdentity(
								SemanticId, Operation.ParameterGuid,
								Operation.ParameterName)
							|| !TryCanonicalDouble(
								*Operation.Value, Operation.DoubleValue))
						{
							SetError(OutError,
								TEXT("graph_compound_pcg_parameter_invalid"),
								SemanticId);
							return false;
						}
						const FInstancedPropertyBag* const Bag =
							Instance->GetUserParametersStruct();
						const FPropertyBagPropertyDesc* const Desc = Bag
							? Bag->FindPropertyDescByID(Operation.ParameterGuid)
							: nullptr;
						if (!Desc || Desc->Name != Operation.ParameterName
							|| Desc->ValueType
								!= EPropertyBagPropertyType::Double)
						{
							SetError(OutError,
								TEXT("graph_compound_pcg_parameter_identity_invalid"),
								SemanticId);
							return false;
						}
						Operation.Kind =
							EPcgAiSchemaMutationKind::PcgDoubleParameter;
					}
					else
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_graph_instance_invalid"),
							SemanticId);
						return false;
					}
				}
				else if (Profile.GetCapabilityId()
					== TEXT("pcg.graph_parameters.apply"))
				{
					UPCGGraph* const Graph = Cast<UPCGGraph>(&Target);
					if (!Graph
						|| Edit.GetElementKind()
							!= EGraphCompoundElementKind::Property
						|| !Operation.Dependencies.IsEmpty()
						|| !TryParsePcgDoubleParameterIdentity(
							SemanticId, Operation.ParameterGuid,
							Operation.ParameterName)
						|| !TryCanonicalDouble(
							*Operation.Value, Operation.DoubleValue))
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_parameter_invalid"),
							SemanticId);
						return false;
					}
					const FInstancedPropertyBag* const Bag =
						Graph->GetUserParametersStruct();
					const FPropertyBagPropertyDesc* const Desc = Bag
						? Bag->FindPropertyDescByID(Operation.ParameterGuid)
						: nullptr;
					if (!Desc || Desc->Name != Operation.ParameterName
						|| Desc->ValueType != EPropertyBagPropertyType::Double)
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_parameter_identity_invalid"),
							SemanticId);
						return false;
					}
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
					if (!TryCanonicalStringField(
							*Operation.Value, TEXT("data_type_ref"),
							ParsedDataTypeRef))
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_data_asset_input_invalid"),
							SemanticId);
						return false;
					}
					if (ParsedDataTypeRef != DataTypeRef)
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_data_type_unsupported"),
							ParsedDataTypeRef);
						return false;
					}
					if (!Cast<UPCGDataAsset>(&Target)
						|| Edit.GetElementKind()
							!= EGraphCompoundElementKind::Property
						|| !Operation.Dependencies.IsEmpty()
						|| SemanticId
							!= TEXT("property:data_content:") + DataTypeRef
						|| !HasExactObjectFields(
							*Operation.Value,
							{TEXT("content_revision"),
								TEXT("data_type_ref"), TEXT("source_ref"),
								TEXT("value")})
						|| !TryCanonicalStringField(
							*Operation.Value, TEXT("content_revision"),
							Operation.ContentRevision)
						|| !TryCanonicalStringField(
							*Operation.Value, TEXT("source_ref"),
							ParsedSourceRef)
						|| !Content
						|| !HasExactObjectFields(*Content, {TEXT("point_count")})
						|| !TryCanonicalIntegerField(
							*Content, TEXT("point_count"), Operation.PointCount)
						|| Operation.PointCount <= 0)
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_data_asset_input_invalid"),
							SemanticId);
						return false;
					}
					Operation.SourceRef = FSoftObjectPath(ParsedSourceRef);
					if (!Operation.SourceRef.IsValid())
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_data_asset_source_invalid"),
							ParsedSourceRef);
						return false;
					}
					Operation.Kind = EPcgAiSchemaMutationKind::PcgDataAsset;
				}
				else
				{
					SetError(OutError,
						TEXT("graph_compound_production_binding_unimplemented"),
						Profile.GetCapabilityId());
					return false;
				}
				OutOperations.Add(MoveTemp(Operation));
			}
			return true;
		}

		UEdGraphNode* FindBehaviorNode(
			UBehaviorTreeGraph& Graph,
			const FString& SemanticId)
		{
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				if (Node && BehaviorTreeNodeSemanticId(*Node) == SemanticId)
				{
					return Node;
				}
			}
			return nullptr;
		}

		UEnvironmentQueryGraphNode_Option* FindEqsOptionNode(
			UEnvironmentQueryGraph& Graph,
			const FString& SemanticId)
		{
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				UEnvironmentQueryGraphNode_Option* const Option =
					Cast<UEnvironmentQueryGraphNode_Option>(Node);
				if (Option && EqsOptionSemanticId(*Option) == SemanticId)
				{
					return Option;
				}
			}
			return nullptr;
		}

		UEdGraphPin* FindDirectionPin(
			UEdGraphNode& Node,
			const EEdGraphPinDirection Direction,
			const int32 DirectionIndex)
		{
			int32 Index = 0;
			for (UEdGraphPin* Pin : Node.Pins)
			{
				if (Pin && Pin->Direction == Direction)
				{
					if (Index == DirectionIndex)
					{
						return Pin;
					}
					++Index;
				}
			}
			return nullptr;
		}

		bool TryApplySchemaMutationOperations(
			UObject& Target,
			TArray<FPcgAiSchemaMutationOperation>& Operations,
			FPcgAiNodeGraphNativeHook& NativeHook,
			FString& OutRevision,
			FGraphCompoundRuntimeError& OutError)
		{
			OutRevision.Reset();
			if (UPCGComponent* const Component = Cast<UPCGComponent>(&Target))
			{
				if (Operations.Num() != 1
					|| Operations[0].Kind
						!= EPcgAiSchemaMutationKind::PcgComponentBinding
					|| !Operations[0].GraphInterface)
				{
					SetError(OutError,
						TEXT("graph_compound_pcg_component_binding_invalid"),
						Component->GetPathName());
					return false;
				}
				const FPcgAiSchemaMutationOperation& Operation = Operations[0];
				FPcgAiNodeGraphNativeMutationReceipt ApplyReceipt;
				FPcgAiNodeGraphBarrierReceipt BarrierReceipt;
				FPcgAiNodeGraphComponentBindingIdentity Identity;
				FString Evidence;
				FPcgAiNodeGraphAdapterError NativeError;
				if (!NativeHook.TryBindAndCapturePcgComponent(
						*Component, *Operation.GraphInterface,
						Operation.NativeGenerationPolicy,
						ApplyReceipt, BarrierReceipt, Identity,
						OutRevision, Evidence, NativeError)
					|| !BarrierReceipt.IsComplete()
					|| BarrierReceipt.GetErrorCount() != 0
					|| !Component->GetGraphInstance()
					|| Component->GetGraphInstance()->Graph
						!= Operation.GraphInterface
					|| Component->GenerationTrigger
						!= Operation.NativeGenerationTrigger
					|| Identity.GetActorIdentity() != Operation.ActorRef
					|| Identity.GetComponentIdentity()
						!= Component->GetPathName()
					|| Identity.GetGraphIdentity()
						!= Operation.GraphInterface->GetGraph()->GetPathName()
					|| Identity.GetGenerationPolicy()
						!= Operation.NativeGenerationPolicy)
				{
					SetError(OutError,
						NativeError.GetCode().IsEmpty()
							? TEXT("graph_compound_pcg_component_refresh_incomplete")
							: NativeError.GetCode(),
						Operation.SemanticId, NativeError.GetDetail());
					return false;
				}
				return true;
			}
			if (UPCGDataAsset* const DataAsset = Cast<UPCGDataAsset>(&Target))
			{
				if (Operations.Num() != 1
					|| Operations[0].Kind
						!= EPcgAiSchemaMutationKind::PcgDataAsset)
				{
					SetError(OutError,
						TEXT("graph_compound_pcg_data_asset_input_invalid"),
						DataAsset->GetPathName());
					return false;
				}
				const FPcgAiSchemaMutationOperation& Operation = Operations[0];
				UPCGPointData* const PointData = NewObject<UPCGPointData>(
					DataAsset, NAME_None, RF_Transactional);
				if (!PointData)
				{
					SetError(OutError,
						TEXT("graph_compound_pcg_data_asset_allocation_failed"),
						DataAsset->GetPathName());
					return false;
				}
				TArray<FPCGPoint> Points;
				Points.Reserve(Operation.PointCount);
				for (int32 Index = 0; Index < Operation.PointCount; ++Index)
				{
					Points.Emplace(
						FTransform(FVector(Index * 100.0, 0.0, 0.0)),
						1.0f, Index + 1);
				}
				PointData->SetPoints(Points);

				FPcgAiNodeGraphNativeMutationReceipt ApplyReceipt;
				FPcgAiNodeGraphBarrierReceipt BarrierReceipt;
				FPcgAiNodeGraphDataAssetIdentity Identity;
				FString Evidence;
				FPcgAiNodeGraphAdapterError NativeError;
				if (!NativeHook.TrySetAndCapturePcgDataAsset(
						*DataAsset, *PointData, Operation.SourceRef,
						ApplyReceipt, BarrierReceipt, Identity,
						Evidence, NativeError)
					|| !BarrierReceipt.IsComplete()
					|| BarrierReceipt.GetErrorCount() != 0
					|| Identity.GetDataAssetIdentity()
						!= DataAsset->GetPathName()
					|| Identity.GetDataTypeIdentity()
						!= UPCGPointData::StaticClass()->GetPathName()
					|| Identity.GetSourceIdentity()
						!= Operation.SourceRef.ToString())
				{
					SetError(OutError,
						NativeError.GetCode().IsEmpty()
							? TEXT("graph_compound_pcg_data_asset_apply_failed")
							: NativeError.GetCode(),
						Operation.SemanticId, NativeError.GetDetail());
					return false;
				}
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
						if (RequestedGraph
							|| !Operation.GraphInterface)
						{
							SetError(OutError,
								TEXT("graph_compound_pcg_graph_binding_invalid"),
								Operation.SemanticId);
							return false;
						}
						RequestedGraph = Operation.GraphInterface;
					}
					else if (Operation.Kind
						!= EPcgAiSchemaMutationKind::PcgDoubleParameter)
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_graph_instance_invalid"),
							Operation.SemanticId);
						return false;
					}
				}
				if (!RequestedGraph
					|| !Instance->CanGraphInterfaceBeSet(RequestedGraph))
				{
					SetError(OutError,
						TEXT("graph_compound_pcg_graph_binding_invalid"),
						Instance->GetPathName());
					return false;
				}
				Instance->Modify();
				Instance->SetGraph(RequestedGraph);
				if (Instance->Graph != RequestedGraph)
				{
					SetError(OutError,
						TEXT("graph_compound_pcg_graph_binding_failed"),
						RequestedGraph->GetPathName());
					return false;
				}
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
					if (!Desc || Desc->Name != Operation.ParameterName
						|| Desc->ValueType != EPropertyBagPropertyType::Double
						|| !Property)
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_parameter_identity_invalid"),
							Operation.SemanticId);
						return false;
					}
					Instance->UpdatePropertyOverride(Property, true);
					if (Instance->SetGraphParameter<double>(
							Operation.ParameterName, Operation.DoubleValue)
						!= EPropertyBagResult::Success)
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_parameter_write_failed"),
							Operation.SemanticId);
						return false;
					}
					const TValueOrError<double, EPropertyBagResult> Readback =
						Instance->GetGraphParameter<double>(Operation.ParameterName);
					if (!Instance->ParametersOverrides.IsPropertyOverridden(Property)
						|| !Readback.IsValid()
						|| !FMath::IsNearlyEqual(
							Readback.GetValue(), Operation.DoubleValue))
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_parameter_refresh_incomplete"),
							Operation.SemanticId);
						return false;
					}
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
						if (!QueryOwners::Pcg::TryPerformPcgNodeCandidate(
								*Graph,
								Operation.CandidateName,
								Operation.CandidateId,
								FVector2f(
									Operation.PositionX, Operation.PositionY),
								Node,
								ResolvedCandidate,
								CandidateError)
							|| !Node)
						{
							SetError(OutError,
								CandidateError.Code.IsEmpty()
									? TEXT("graph_compound_pcg_graph_node_create_failed")
									: CandidateError.Code,
								Operation.SemanticId, CandidateError.Detail);
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
						if (!NativeHook.TrySetPcgDoubleParameter(
								*Graph, Operation.ParameterGuid,
								Operation.ParameterName, Operation.DoubleValue,
								ApplyReceipt, BarrierReceipt, Identity,
								ParameterRevision, Evidence, NativeError)
							|| !BarrierReceipt.IsComplete()
							|| BarrierReceipt.GetErrorCount() != 0)
						{
							SetError(OutError,
								NativeError.GetCode().IsEmpty()
									? TEXT("graph_compound_pcg_parameter_write_failed")
									: NativeError.GetCode(),
								Operation.SemanticId, NativeError.GetDetail());
							return false;
						}
					}
					else
					{
						SetError(OutError,
							TEXT("graph_compound_pcg_graph_operation_invalid"),
							Operation.SemanticId);
						return false;
					}
				}
				FPcgAiNodeGraphBarrierReceipt BarrierReceipt;
				FPcgAiNodeGraphTopology Topology;
				TArray<FPcgAiNodeGraphParameterIdentity> Parameters;
				FString Evidence;
				FPcgAiNodeGraphAdapterError NativeError;
				if (!NativeHook.TryCompileAndCapturePcgGraph(
						*Graph, BarrierReceipt, Topology, Parameters,
						OutRevision, Evidence, NativeError)
					|| !BarrierReceipt.IsComplete()
					|| BarrierReceipt.GetErrorCount() != 0)
				{
					SetError(OutError,
						NativeError.GetCode().IsEmpty()
							? TEXT("graph_compound_pcg_graph_compile_failed")
							: NativeError.GetCode(),
						Graph->GetPathName(), NativeError.GetDetail());
					return false;
				}
				return true;
			}

			if (UBehaviorTree* const Tree = Cast<UBehaviorTree>(&Target))
			{
#if WITH_EDITORONLY_DATA
				UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(Tree->BTGraph);
				if (!Graph)
				{
					Graph = NewObject<UBehaviorTreeGraph>(
						Tree, TEXT("BehaviorTreeGraph"), RF_Transactional);
					if (!Graph)
					{
						SetError(OutError,
							TEXT("graph_compound_behavior_tree_graph_create_failed"),
							Tree->GetPathName());
						return false;
					}
					Tree->BTGraph = Graph;
					Graph->OnCreated();
					Graph->Initialize();
				}
				for (const FPcgAiSchemaMutationOperation& Operation : Operations)
				{
					if (Operation.Kind == EPcgAiSchemaMutationKind::BehaviorRoot)
					{
						UBehaviorTreeGraphNode_Root* const Node =
							NewObject<UBehaviorTreeGraphNode_Root>(Graph);
						Node->CreateNewGuid();
						Node->PostPlacedNewNode();
						Node->AllocateDefaultPins();
						Graph->AddNode(Node, false, false);
					}
					else if (Operation.Kind
						== EPcgAiSchemaMutationKind::BehaviorComposite)
					{
						if (!Operation.CandidateName.IsEmpty()
							|| !Operation.CandidateId.IsEmpty())
						{
							UBehaviorTreeGraphNode* NativeNode = nullptr;
							QueryOwners::AiBehavior::FAiBehaviorCandidate
								ResolvedCandidate;
							QueryOwners::AiBehavior::FAiBehaviorCandidateQueryError
								CandidateError;
							if (!QueryOwners::AiBehavior::TryPerformBehaviorTreeCandidate(
									*Tree,
									Operation.CandidateName,
									Operation.CandidateId,
									FVector2f(
										Operation.PositionX,
										Operation.PositionY),
									NativeNode,
									ResolvedCandidate,
									CandidateError)
								|| !NativeNode
								|| !NativeNode->IsA<
									UBehaviorTreeGraphNode_Composite>()
								|| !NativeNode->NodeInstance
								|| NativeNode->NodeInstance->GetClass()
									!= Operation.PayloadClass)
							{
								SetError(
									OutError,
									CandidateError.Code.IsEmpty()
										? TEXT("graph_compound_behavior_tree_native_action_failed")
										: CandidateError.Code,
									Operation.SemanticId,
									CandidateError.Detail);
								return false;
							}
							NativeNode->Modify();
							NativeNode->NodeGuid = Operation.NodeGuid;
						}
						else
						{
							UBehaviorTreeGraphNode_Composite* const Node =
								NewObject<UBehaviorTreeGraphNode_Composite>(Graph);
							Node->NodeInstance = NewObject<UBTCompositeNode>(
								Tree,
								Operation.PayloadClass,
								NAME_None,
								RF_Transactional);
							if (!Node->NodeInstance)
							{
								SetError(OutError,
									TEXT("graph_compound_behavior_tree_payload_create_failed"),
									Operation.SemanticId);
								return false;
							}
							Node->PostPlacedNewNode();
							Node->NodeGuid = Operation.NodeGuid;
							Node->AllocateDefaultPins();
							Graph->AddNode(Node, false, false);
						}
					}
					else if (Operation.Kind
						== EPcgAiSchemaMutationKind::BehaviorTask)
					{
						if (!Operation.CandidateName.IsEmpty()
							|| !Operation.CandidateId.IsEmpty())
						{
							UBehaviorTreeGraphNode* NativeNode = nullptr;
							QueryOwners::AiBehavior::FAiBehaviorCandidate
								ResolvedCandidate;
							QueryOwners::AiBehavior::FAiBehaviorCandidateQueryError
								CandidateError;
							if (!QueryOwners::AiBehavior::TryPerformBehaviorTreeCandidate(
									*Tree,
									Operation.CandidateName,
									Operation.CandidateId,
									FVector2f(
										Operation.PositionX,
										Operation.PositionY),
									NativeNode,
									ResolvedCandidate,
									CandidateError)
								|| !NativeNode
								|| !NativeNode->IsA<UBehaviorTreeGraphNode_Task>()
								|| !NativeNode->NodeInstance
								|| NativeNode->NodeInstance->GetClass()
									!= Operation.PayloadClass)
							{
								SetError(
									OutError,
									CandidateError.Code.IsEmpty()
										? TEXT("graph_compound_behavior_tree_native_action_failed")
										: CandidateError.Code,
									Operation.SemanticId,
									CandidateError.Detail);
								return false;
							}
							NativeNode->Modify();
							NativeNode->NodeGuid = Operation.NodeGuid;
						}
						else
						{
							UBehaviorTreeGraphNode_Task* const Node =
								NewObject<UBehaviorTreeGraphNode_Task>(Graph);
							Node->NodeInstance = NewObject<UBTTaskNode>(
								Tree,
								Operation.PayloadClass,
								NAME_None,
								RF_Transactional);
							if (!Node->NodeInstance)
							{
								SetError(OutError,
									TEXT("graph_compound_behavior_tree_payload_create_failed"),
									Operation.SemanticId);
								return false;
							}
							Node->PostPlacedNewNode();
							Node->NodeGuid = Operation.NodeGuid;
							Node->AllocateDefaultPins();
							Graph->AddNode(Node, false, false);
						}
					}
					else if (Operation.Kind
						== EPcgAiSchemaMutationKind::BehaviorBlackboardBinding)
					{
						UBlackboardData* const Blackboard =
							LoadObject<UBlackboardData>(nullptr, *Operation.ObjectRef);
						if (!Blackboard)
						{
							SetError(OutError,
								TEXT("graph_compound_behavior_tree_blackboard_unresolved"),
								Operation.ObjectRef);
							return false;
						}
						Tree->Modify();
						Tree->BlackboardAsset = Blackboard;
					}
				}
				for (const FPcgAiSchemaMutationOperation& Operation : Operations)
				{
					const bool bService = Operation.Kind
						== EPcgAiSchemaMutationKind::BehaviorService;
					const bool bDecorator = Operation.Kind
						== EPcgAiSchemaMutationKind::BehaviorDecorator;
					if (!bService && !bDecorator)
					{
						continue;
					}
					UBehaviorTreeGraphNode* const OwnerNode =
						Cast<UBehaviorTreeGraphNode>(
							FindBehaviorNode(*Graph, Operation.OwnerItem));
					UBehaviorTreeGraphNode* const SubNode = bService
						? static_cast<UBehaviorTreeGraphNode*>(
							NewObject<UBehaviorTreeGraphNode_Service>(Graph))
						: static_cast<UBehaviorTreeGraphNode*>(
							NewObject<UBehaviorTreeGraphNode_Decorator>(Graph));
					if (!OwnerNode || !SubNode)
					{
						SetError(OutError,
							TEXT("graph_compound_behavior_tree_subnode_owner_invalid"),
							Operation.OwnerItem);
						return false;
					}
					SubNode->NodeInstance = NewObject<UBTNode>(
						Tree, Operation.PayloadClass, NAME_None, RF_Transactional);
					if (!SubNode->NodeInstance)
					{
						SetError(OutError,
							TEXT("graph_compound_behavior_tree_subnode_create_failed"),
							Operation.SemanticId);
						return false;
					}
					OwnerNode->AddSubNode(SubNode, Graph);
					SubNode->NodeGuid = Operation.NodeGuid;
				}
				for (const FPcgAiSchemaMutationOperation& Operation : Operations)
				{
					if (Operation.Kind
						!= EPcgAiSchemaMutationKind::BehaviorRelation)
					{
						continue;
					}
					UEdGraphNode* const FromNode =
						FindBehaviorNode(*Graph, Operation.FromItem);
					UEdGraphNode* const ToNode =
						FindBehaviorNode(*Graph, Operation.ToItem);
					UEdGraphPin* const FromPin = FromNode
						? FindDirectionPin(*FromNode, EGPD_Output,
							Operation.FromIndex) : nullptr;
					UEdGraphPin* const ToPin = ToNode
						? FindDirectionPin(*ToNode, EGPD_Input,
							Operation.ToIndex) : nullptr;
					if (!FromPin || !ToPin)
					{
						SetError(OutError,
							TEXT("graph_compound_behavior_tree_pin_invalid"),
							Operation.SemanticId);
						return false;
					}
					const UEdGraphSchema* const Schema = Graph->GetSchema();
					if (!Schema || !Schema->TryCreateConnection(FromPin, ToPin))
					{
						SetError(
							OutError,
							TEXT("graph_compound_behavior_tree_connection_rejected"),
							Operation.SemanticId,
							TEXT("the native BehaviorTree schema rejected the connection"));
						return false;
					}
				}
				FPcgAiNodeGraphNativeMutationReceipt ApplyReceipt;
				FPcgAiNodeGraphBarrierReceipt BarrierReceipt;
				FPcgAiNodeGraphTopology Topology;
				FString Evidence;
				FPcgAiNodeGraphAdapterError NativeError;
				if (!NativeHook.TryCompileAndCaptureBehaviorTree(
						*Tree, ApplyReceipt, BarrierReceipt, Topology,
						OutRevision, Evidence, NativeError)
					|| !BarrierReceipt.IsComplete()
					|| BarrierReceipt.GetErrorCount() != 0)
				{
					SetError(OutError,
						NativeError.GetCode().IsEmpty()
							? TEXT("graph_compound_behavior_tree_compile_failed")
							: NativeError.GetCode(),
						Tree->GetPathName(), NativeError.GetDetail());
					return false;
				}
				return true;
#else
				SetError(OutError,
					TEXT("graph_compound_behavior_tree_editor_data_unavailable"),
					Tree->GetPathName());
				return false;
#endif
			}

			if (UStateTree* const Tree = Cast<UStateTree>(&Target))
			{
#if WITH_EDITORONLY_DATA
				UStateTreeEditorData* const EditorData =
					Cast<UStateTreeEditorData>(Tree->EditorData);
				TMap<FGuid, UStateTreeState*> States;
				TMap<FGuid, FString> Parents;
				if (!EditorData
					|| !TryCollectStateTreeStates(*EditorData, States, Parents))
				{
					SetError(OutError,
						TEXT("graph_compound_state_tree_hierarchy_invalid"),
						Tree->GetPathName());
					return false;
				}
				Tree->Modify();
				EditorData->Modify();
				for (const FPcgAiSchemaMutationOperation& Operation : Operations)
				{
					if (Operation.Kind
						!= EPcgAiSchemaMutationKind::StateTreeHierarchy)
					{
						continue;
					}
					UStateTreeState* const* const Parent =
						States.Find(Operation.ParentStateGuid);
					if (!Parent || !*Parent || States.Contains(Operation.StateGuid))
					{
						SetError(OutError,
							TEXT("graph_compound_state_tree_hierarchy_invalid"),
							Operation.SemanticId);
						return false;
					}
					(*Parent)->Modify();
					UStateTreeState& Child = (*Parent)->AddChildState(NAME_None);
					Child.Modify();
					Child.ID = Operation.StateGuid;
					States.Add(Operation.StateGuid, &Child);
				}
				for (const FPcgAiSchemaMutationOperation& Operation : Operations)
				{
					if (Operation.Kind
						!= EPcgAiSchemaMutationKind::StateTreeNode)
					{
						continue;
					}
					FStateTreeEditorNode* EditorNode = nullptr;
					UObject* NodeOuter = Tree;
					if (Operation.Placement == TEXT("state_task")
						|| Operation.Placement == TEXT("state_condition"))
					{
						UStateTreeState* const* const State =
							States.Find(Operation.StateGuid);
						if (!State || !*State)
						{
							SetError(OutError,
								TEXT("graph_compound_state_tree_node_owner_invalid"),
								Operation.SemanticId);
							return false;
						}
						(*State)->Modify();
						NodeOuter = *State;
						EditorNode = Operation.Placement == TEXT("state_task")
							? &(*State)->Tasks.AddDefaulted_GetRef()
							: &(*State)->EnterConditions.AddDefaulted_GetRef();
					}
					else if (Operation.Placement == TEXT("global_task"))
					{
						EditorNode = &EditorData->GlobalTasks.AddDefaulted_GetRef();
					}
					else if (Operation.Placement == TEXT("evaluator"))
					{
						EditorNode = &EditorData->Evaluators.AddDefaulted_GetRef();
					}
					if (!EditorNode
						|| (!Operation.PayloadClass && !Operation.PayloadStruct)
						|| !TryInitializeStateTreeBlueprintNode(
							*EditorNode,
							*NodeOuter,
							Operation.PayloadClass,
							Operation.PayloadStruct,
							Operation.Placement,
							OutError))
					{
						if (OutError.Code.IsEmpty())
						{
							SetError(OutError,
								TEXT("graph_compound_state_tree_node_create_failed"),
								Operation.SemanticId);
						}
						return false;
					}
					EditorNode->ID = Operation.NodeGuid;
				}
				for (const FPcgAiSchemaMutationOperation& Operation : Operations)
				{
					if (Operation.Kind
						!= EPcgAiSchemaMutationKind::StateTreeOrder)
					{
						continue;
					}
					UStateTreeState* const* const Owner =
						States.Find(Operation.ParentStateGuid);
					if (!Owner || !*Owner
						|| (*Owner)->Children.Num()
							!= Operation.OrderedStateGuids.Num())
					{
						SetError(OutError,
							TEXT("graph_compound_state_tree_order_invalid"),
							Operation.SemanticId);
						return false;
					}
					TArray<TObjectPtr<UStateTreeState>> OrderedChildren;
					OrderedChildren.Reserve(Operation.OrderedStateGuids.Num());
					for (const FGuid& StateGuid : Operation.OrderedStateGuids)
					{
						UStateTreeState* const* const State = States.Find(StateGuid);
						if (!State || !*State || !(*Owner)->Children.Contains(*State))
						{
							SetError(OutError,
								TEXT("graph_compound_state_tree_order_invalid"),
								Operation.SemanticId);
							return false;
						}
						OrderedChildren.Add(*State);
					}
					(*Owner)->Modify();
					(*Owner)->Children = MoveTemp(OrderedChildren);
				}
				for (const FPcgAiSchemaMutationOperation& Operation : Operations)
				{
					if (Operation.Kind
						!= EPcgAiSchemaMutationKind::StateTreeProperty)
					{
						continue;
					}
					UStateTreeState* const* const State = States.Find(Operation.StateGuid);
					if (!State || !*State)
					{
						SetError(OutError,
							TEXT("graph_compound_state_tree_property_invalid"),
							Operation.SemanticId);
						return false;
					}
					(*State)->Modify();
					(*State)->Name = Operation.StateName;
				}
				FPcgAiNodeGraphNativeMutationReceipt ApplyReceipt;
				FPcgAiNodeGraphBarrierReceipt BarrierReceipt;
				FPcgAiNodeGraphTopology Topology;
				FString Evidence;
				FPcgAiNodeGraphAdapterError NativeError;
				if (!NativeHook.TryCompileAndCaptureStateTree(
						*Tree, ApplyReceipt, BarrierReceipt, Topology,
						OutRevision, Evidence, NativeError)
					|| !BarrierReceipt.IsComplete()
					|| BarrierReceipt.GetErrorCount() != 0)
				{
					SetError(OutError,
						NativeError.GetCode().IsEmpty()
							? TEXT("graph_compound_state_tree_compile_failed")
							: NativeError.GetCode(),
						Tree->GetPathName(), NativeError.GetDetail());
					return false;
				}
				return true;
#else
				SetError(OutError,
					TEXT("graph_compound_state_tree_editor_data_unavailable"),
					Tree->GetPathName());
				return false;
#endif
			}

			if (UEnvQuery* const Query = Cast<UEnvQuery>(&Target))
			{
#if WITH_EDITORONLY_DATA
				UEnvironmentQueryGraph* Graph =
					Cast<UEnvironmentQueryGraph>(Query->EdGraph);
				if (!Graph)
				{
					Graph = NewObject<UEnvironmentQueryGraph>(
						Query, TEXT("EnvironmentQueryGraph"), RF_Transactional);
					if (!Graph)
					{
						SetError(OutError,
							TEXT("graph_compound_eqs_graph_create_failed"),
							Query->GetPathName());
						return false;
					}
					Query->EdGraph = Graph;
					Graph->Initialize();
				}
				UEnvironmentQueryGraphNode_Root* Root = nullptr;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (UEnvironmentQueryGraphNode_Root* const Candidate =
							Cast<UEnvironmentQueryGraphNode_Root>(Node))
					{
						Root = Candidate;
						break;
					}
				}
				if (!Root)
				{
					Root = NewObject<UEnvironmentQueryGraphNode_Root>(Graph);
					Root->CreateNewGuid();
					Root->PostPlacedNewNode();
					Root->AllocateDefaultPins();
					Graph->AddNode(Root, false, false);
				}
				for (const FPcgAiSchemaMutationOperation& Operation : Operations)
				{
					if (Operation.Kind != EPcgAiSchemaMutationKind::EqsOption)
					{
						continue;
					}
					UEnvironmentQueryGraphNode_Option* const Node =
						NewObject<UEnvironmentQueryGraphNode_Option>(Graph);
					UEnvQueryOption* const Option = NewObject<UEnvQueryOption>(
						Query, NAME_None, RF_Transactional);
					if (!Node || !Option)
					{
						SetError(OutError,
							TEXT("graph_compound_eqs_option_create_failed"),
							Operation.SemanticId);
						return false;
					}
					Option->Generator = NewObject<UEnvQueryGenerator>(
						Option, Operation.PayloadClass, NAME_None, RF_Transactional);
					if (!Option->Generator)
					{
						SetError(OutError,
							TEXT("graph_compound_eqs_generator_create_failed"),
							Operation.SemanticId);
						return false;
					}
					Node->NodeInstance = Option;
					Node->PostPlacedNewNode();
					Node->NodeGuid = Operation.NodeGuid;
					Node->AllocateDefaultPins();
					Graph->AddNode(Node, false, false);
					if (!Root->GetOutputPin() || !Node->GetInputPin())
					{
						SetError(OutError,
							TEXT("graph_compound_eqs_pin_invalid"),
							Operation.SemanticId);
						return false;
					}
					Root->GetOutputPin()->MakeLinkTo(Node->GetInputPin());
				}
				for (const FPcgAiSchemaMutationOperation& Operation : Operations)
				{
					if (Operation.Kind != EPcgAiSchemaMutationKind::EqsProperty)
					{
						continue;
					}
					UEnvironmentQueryGraphNode_Option* const Node =
						FindEqsOptionNode(*Graph, Operation.FromItem);
					if (!Node)
					{
						SetError(OutError,
							TEXT("graph_compound_eqs_property_owner_missing"),
							Operation.SemanticId);
						return false;
					}
					Node->Modify();
					Node->NodePosX = Operation.SignedIntegerValue;
				}
				Graph->NotifyGraphChanged();
				FPcgAiNodeGraphNativeMutationReceipt ApplyReceipt;
				FPcgAiNodeGraphBarrierReceipt BarrierReceipt;
				FPcgAiNodeGraphTopology Topology;
				FString Evidence;
				FPcgAiNodeGraphAdapterError NativeError;
				if (!NativeHook.TryCompileAndCaptureEqs(
						*Query, ApplyReceipt, BarrierReceipt, Topology,
						OutRevision, Evidence, NativeError)
					|| !BarrierReceipt.IsComplete()
					|| BarrierReceipt.GetErrorCount() != 0)
				{
					SetError(OutError,
						NativeError.GetCode().IsEmpty()
							? TEXT("graph_compound_eqs_compile_failed")
							: NativeError.GetCode(),
						Query->GetPathName(), NativeError.GetDetail());
					return false;
				}
				return true;
#else
				SetError(OutError,
					TEXT("graph_compound_eqs_editor_data_unavailable"),
					Query->GetPathName());
				return false;
#endif
			}

			if (USmartObjectDefinition* const Definition =
					Cast<USmartObjectDefinition>(&Target))
			{
				for (const FPcgAiSchemaMutationOperation& Operation : Operations)
				{
					if (Operation.Kind
						!= EPcgAiSchemaMutationKind::SmartObjectSlot)
					{
						SetError(OutError,
							TEXT("graph_compound_smart_object_operation_invalid"),
							Operation.SemanticId);
						return false;
					}
					FPcgAiNodeGraphNativeMutationReceipt ApplyReceipt;
					FPcgAiNodeGraphBarrierReceipt BarrierReceipt;
					FPcgAiNodeGraphTopology Topology;
					FString Evidence;
					FPcgAiNodeGraphAdapterError NativeError;
					if (!NativeHook.TryAddValidateAndCaptureSmartObjectSlot(
							*Definition, Operation.SlotGuid,
							*Operation.BehaviorClass, ApplyReceipt, BarrierReceipt,
							Topology, OutRevision, Evidence, NativeError)
						|| !BarrierReceipt.IsComplete()
						|| BarrierReceipt.GetErrorCount() != 0)
					{
						SetError(OutError,
							NativeError.GetCode().IsEmpty()
								? TEXT("graph_compound_smart_object_mutation_failed")
								: NativeError.GetCode(),
							Operation.SemanticId, NativeError.GetDetail());
						return false;
					}
				}
				return true;
			}

			UBlackboardData* const Blackboard = Cast<UBlackboardData>(&Target);
			if (!Blackboard)
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_ai_target_invalid"),
					Target.GetPathName());
				return false;
			}
			for (const FPcgAiSchemaMutationOperation& Operation : Operations)
			{
				FPcgAiNodeGraphNativeMutationReceipt ApplyReceipt;
				FPcgAiNodeGraphBarrierReceipt BarrierReceipt;
				FPcgAiNodeGraphBlackboardEntryIdentity Identity;
				FString Evidence;
				FPcgAiNodeGraphAdapterError NativeError;
				if (!NativeHook.TryAddAndCaptureBlackboardEntry(
						*Blackboard, Operation.EntryName,
						*Operation.KeyTypeClass, ApplyReceipt, BarrierReceipt,
						Identity, OutRevision, Evidence, NativeError)
					|| !BarrierReceipt.IsComplete()
					|| BarrierReceipt.GetErrorCount() != 0)
				{
					SetError(OutError,
						NativeError.GetCode().IsEmpty()
							? TEXT("graph_compound_blackboard_mutation_failed")
							: NativeError.GetCode(),
						Operation.SemanticId, NativeError.GetDetail());
					return false;
				}
			}
			return true;
		}

		bool TryBuildSchemaMutationAggregate(
			const TArray<FPcgAiSchemaMutationOperation>& Operations,
			const TMap<FString, FUeAgentCanonicalValueRef>* ActualValues,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FVerificationError& OutError)
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
			for (const FPcgAiSchemaMutationOperation& Operation : Operations)
			{
				const FUeAgentCanonicalValueRef* const Actual = ActualValues
					? ActualValues->Find(Operation.SemanticId) : nullptr;
				if (ActualValues && !Actual)
				{
					OutError = FVerificationError(
						TEXT("graph_compound_pcg_ai_readback_missing"),
						Operation.SemanticId);
					return false;
				}
				FUeAgentCanonicalValuePtr Expected = Operation.Value;
				if (!ActualValues
					&& Operation.Kind == EPcgAiSchemaMutationKind::PcgGraphNode)
				{
					if (!Operation.CreatedNodeClass || !Operation.SettingsClass)
					{
						OutError = FVerificationError(
							TEXT("graph_compound_pcg_ai_readback_expected_invalid"),
							Operation.SemanticId);
						return false;
					}
					const TSharedRef<FJsonObject> Definition =
						MakeShared<FJsonObject>();
					Definition->SetStringField(
						TEXT("carrier_type_ref"),
						Operation.CreatedNodeClass->GetPathName());
					Definition->SetStringField(
						TEXT("settings_type_ref"),
						Operation.SettingsClass->GetPathName());
					FString CanonicalError;
					if (!CanonicalJson::TryFromJsonValue(
							MakeShared<FJsonValueObject>(Definition),
							Resources, Expected, CanonicalError)
						|| !Expected.IsValid())
					{
						OutError = FVerificationError(
							TEXT("graph_compound_pcg_ai_readback_expected_invalid"),
							CanonicalError);
						return false;
					}
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
					{
						OutError = FVerificationError(
							TEXT("graph_compound_pcg_ai_readback_expected_invalid"),
							Operation.SemanticId);
						return false;
					}
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
					FString CanonicalError;
					if (!CanonicalJson::TryFromJsonValue(
							MakeShared<FJsonValueObject>(Definition),
							Resources, Expected, CanonicalError)
						|| !Expected.IsValid())
					{
						OutError = FVerificationError(
							TEXT("graph_compound_pcg_ai_readback_expected_invalid"),
							CanonicalError);
						return false;
					}
				}
				Items.Emplace(Operation.SemanticId,
					Actual ? *Actual : Expected.ToSharedRef());
			}
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryObject(
					Items, Resources, OutValue, StateError))
			{
				OutError = FVerificationError(
					StateError.Code, StateError.Detail);
				return false;
			}
			return OutValue.IsValid();
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
				TArray<FPcgAiSchemaMutationOperation> InOperations)
				: Profile(MoveTemp(InProfile))
				, TargetRef(MoveTemp(InTargetRef))
				, Operations(MoveTemp(InOperations))
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
				FString Detail;
				if (!TryUnloadTarget(TargetRef, false, Detail))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						TEXT("graph_compound_pcg_ai_asset_unload_failed"), Detail);
					return true;
				}
				TArray<FCapturedMutationValue> Captured;
				FString Generation;
				FGraphCompoundRuntimeError RuntimeError;
				if (!TryCaptureMutationValues(
						Profile, TargetRef, Resources,
						Captured, Generation, RuntimeError))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						RuntimeError.Code, RuntimeError.Detail);
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
						RuntimeError.Code.IsEmpty()
							? TEXT("graph_compound_pcg_ai_target_invalid")
							: RuntimeError.Code,
						RuntimeError.Detail);
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
				FString Detail;
				if (!TryUnloadTarget(TargetRef, false, Detail))
				{
					OutObservation =
						FIndependentReadbackObservation::Unavailable(
							TEXT("graph_compound_pcg_ai_asset_unload_failed"),
							Detail);
					return true;
				}
				UObject* const Asset = LoadObject<UObject>(
					nullptr, *ObjectPath(TargetRef));
				if (!Asset || Asset->GetClass() != AssetClass
					|| Asset->GetOutermost()->GetName() != TargetRef
					|| Asset->GetOutermost()->IsDirty())
				{
					OutObservation =
						FIndependentReadbackObservation::Unavailable(
							TEXT("graph_compound_pcg_ai_asset_reload_mismatch"),
							TEXT("reload:") + TargetRef);
					return true;
				}

				const TSharedRef<FJsonObject> Definition =
					MakeShared<FJsonObject>();
				Definition->SetStringField(
					TEXT("asset_kind"), TargetKind);
				FUeAgentCanonicalValuePtr Actual;
				FString CanonicalError;
				if (!CanonicalJson::TryFromJsonValue(
						MakeShared<FJsonValueObject>(Definition),
						Resources, Actual, CanonicalError)
					|| !Actual.IsValid())
				{
					OutError = FVerificationError(
						TEXT("graph_compound_pcg_ai_readback_value_invalid"),
						CanonicalError);
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
			const FString& Revision,
			const FString& PersistenceReceiptId,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("revision"), Revision);
			Root->SetStringField(TEXT("target_ref"), TargetRef);
			if (!PersistenceReceiptId.IsEmpty())
			{
				Root->SetStringField(
					TEXT("persistence_receipt_id"), PersistenceReceiptId);
			}
			FString Canonical;
			FString JsonError;
			if (!Json::TryCanonicalize(Root, Canonical, JsonError))
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_ai_result_invalid"),
					TargetRef, JsonError);
				return false;
			}
			OutResult = FUeAgentContractValue(MoveTemp(Canonical));
			return true;
		}

		bool TrySerializeResult(
			const FString& TargetRef,
			const FString& Revision,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			return TrySerializeResult(
				TargetRef, Revision, FString(), OutResult, OutError);
		}

		bool TryRestorePcgComponentMutation(
			UPCGComponent& Component,
			UPCGGraphInterface* BaselineGraph,
			const EPCGComponentGenerationTrigger BaselinePolicy,
			const bool bBaselinePackageDirty,
			FString& OutDetail)
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
				OutDetail = TEXT("pcg component rollback readback mismatch");
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
				SetError(OutError,
					Cancellation->IsCancelled()
						? TEXT("graph_compound_pcg_ai_cancelled")
						: TEXT("graph_compound_pcg_ai_thread_invalid"),
					Profile.GetCapabilityId());
				return false;
			}
			UPCGComponent* const Component =
				FindExactLoadedPcgComponent(Request.GetTargetRef());
			if (!Component)
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_ai_target_invalid"),
					Request.GetTargetRef());
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
				SetError(OutError, RevalidateError.GetCode(),
					RevalidateError.GetIdentityKey(),
					RevalidateError.GetDetail());
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
				FString Detail;
				if (!TryRestorePcgComponentMutation(
						*Component, BaselineGraph, BaselinePolicy,
						bBaselinePackageDirty, Detail))
				{
					OutError.Detail += OutError.Detail.IsEmpty()
						? Detail : TEXT(";") + Detail;
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
				SetError(OutError,
					TEXT("graph_compound_pcg_ai_cancelled"),
					Profile.GetCapabilityId());
				Rollback();
				return false;
			}

			FUeAgentCanonicalValuePtr Expected;
			FVerificationError VerifyError;
			if (!TryBuildSchemaMutationAggregate(
					Operations, nullptr, *Resources, Expected, VerifyError))
			{
				SetError(OutError, VerifyError.GetCode(),
					Request.GetTargetRef(), VerifyError.GetDetail());
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
			if (!FIndependentReadbackService::TryCreate(
					{Oracle}, Readback, VerifyError)
				|| !Readback.IsValid()
				|| !Readback->TryVerify(
					FIndependentReadbackRequest(
						TEXT("graph-compound-pcg-ai:") + Plan->GetRequestId(),
						PcgComponentMutationReadbackOracleId,
						Request.GetTargetRef(), Expected.ToSharedRef()),
					*Resources, Outcome, VerifyError)
				|| Outcome.GetStatus() != EVerificationStatus::Verified)
			{
				SetError(OutError,
					VerifyError.GetCode().IsEmpty()
						? TEXT("graph_compound_pcg_ai_readback_failed")
						: VerifyError.GetCode(),
					Request.GetTargetRef(), Outcome.GetDiagnosticCode());
				Rollback();
				return false;
			}
			return TrySerializeResult(
				Request.GetTargetRef(), Revision, OutResult, OutError);
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
				SetError(OutError,
					Cancellation->IsCancelled()
						? TEXT("graph_compound_pcg_ai_cancelled")
						: TEXT("graph_compound_pcg_ai_thread_invalid"),
					Profile.GetCapabilityId());
				return false;
			}
			UObject* Target = LoadObject<UObject>(nullptr, *Request.GetTargetRef());
			if (!Target || Target->GetPathName() != Request.GetTargetRef())
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_ai_target_invalid"),
					Request.GetTargetRef());
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
				SetError(OutError, RevalidateError.GetCode(),
					RevalidateError.GetIdentityKey(),
					RevalidateError.GetDetail());
				return false;
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
				SetError(OutError, DurableError.GetCode(),
					Request.GetTargetRef(), DurableError.GetDetail());
				return false;
			}

			FString Revision;
			if (!TryApplySchemaMutationOperations(
					*Target, Operations, NativeHook, Revision, OutError))
			{
				Target = nullptr;
				FString RollbackDetail;
				TryRollbackCreate(
					Request.GetTargetRef(), false, Prepared.ToSharedRef(),
					DurableBackend, FencingToken, *Resources, RollbackDetail);
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
				TryRollbackCreate(
					Request.GetTargetRef(), false, Prepared.ToSharedRef(),
					DurableBackend, FencingToken, *Resources, RollbackDetail);
				SetError(OutError, DurableError.GetCode(),
					Request.GetTargetRef(), DurableError.GetDetail()
						+ TEXT(";") + RollbackDetail);
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
				FString RollbackDetail;
				TryRollbackCreate(
					Request.GetTargetRef(), false, Persisted,
					DurableBackend, FencingToken, *Resources, RollbackDetail);
				SetError(OutError, VerifyError.GetCode(),
					Request.GetTargetRef(), VerifyError.GetDetail()
						+ TEXT(";") + RollbackDetail);
				return false;
			}
			TSharedPtr<FIndependentReadbackService> Readback;
			const TSharedRef<FPcgAiSchemaMutationReadbackOracle,
				ESPMode::ThreadSafe> Oracle =
				MakeShared<FPcgAiSchemaMutationReadbackOracle,
					ESPMode::ThreadSafe>(Profile, Request.GetTargetRef(), Operations);
			FIndependentReadbackOutcome Outcome;
			if (!FIndependentReadbackService::TryCreate(
					{Oracle}, Readback, VerifyError)
				|| !Readback.IsValid()
				|| !Readback->TryVerify(
					FIndependentReadbackRequest(
						TEXT("graph-compound-pcg-ai:") + Plan->GetRequestId(),
						SchemaMutationReadbackOracleId,
						Request.GetTargetRef(), Expected.ToSharedRef()),
					*Resources, Outcome, VerifyError)
				|| Outcome.GetStatus() != EVerificationStatus::Verified)
			{
				FString RollbackDetail;
				TryRollbackCreate(
					Request.GetTargetRef(), false, Persisted,
					DurableBackend, FencingToken, *Resources, RollbackDetail);
				SetError(OutError,
					VerifyError.GetCode().IsEmpty()
						? TEXT("graph_compound_pcg_ai_readback_failed")
						: VerifyError.GetCode(),
					Request.GetTargetRef(), Outcome.GetDiagnosticCode()
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
				TryRollbackCreate(
					Request.GetTargetRef(), false, Persisted,
					DurableBackend, FencingToken, *Resources, RollbackDetail);
				SetError(OutError, DurableError.GetCode(),
					Request.GetTargetRef(), DurableError.GetDetail()
						+ TEXT(";") + RollbackDetail);
				return false;
			}
			return TrySerializeResult(
				Request.GetTargetRef(), Revision, Outcome.GetEvidenceId(),
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
				FString Detail;
				if (!TryUnloadTarget(TargetRef, false, Detail))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						TEXT("graph_compound_pcg_level_asset_unload_failed"),
						Detail);
					return true;
				}
				UPCGDataAsset* const Asset = LoadObject<UPCGDataAsset>(
					nullptr, *ObjectPath(TargetRef));
				bool bExactData = Asset
					&& Asset->GetOutermost()->GetName() == PackageName(TargetRef)
					&& !Asset->GetOutermost()->IsDirty()
					&& Asset->ObjectPath.ToString() == SourceRef
					&& Asset->Data.TaggedData.Num() == 2;
				UClass* const ExpectedDataClass =
					LoadSupportedPcgLevelPointDataClass(DataTypeRef);
				if (bExactData)
				{
					for (const FPCGTaggedData& Tagged : Asset->Data.TaggedData)
					{
						bExactData = bExactData
							&& Tagged.Data
							&& Tagged.Data->GetClass() == ExpectedDataClass;
					}
				}
				if (!bExactData)
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						TEXT("graph_compound_pcg_level_asset_reload_mismatch"),
						TEXT("reload:") + TargetRef);
					return true;
				}
				FUeAgentCanonicalValuePtr Actual;
				FGraphCompoundRuntimeError ValueError;
				if (!TryBuildPcgLevelDataAssetValue(
						DataTypeRef, SourceRef, Resources, Actual, ValueError)
					|| !Actual.IsValid())
				{
					OutError = FVerificationError(
						ValueError.Code, ValueError.Detail);
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
			if (!IsPcgDataFromLevelProfile(Profile, Request.GetTarget())
				|| Plan->GetDescriptorIdentity()
					!= Profile.GetExecutionContract().GetDescriptorIdentity()
				|| Edits.Num() != 1 || Changes.Num() != 1
				|| Edits[0].GetElementKind()
					!= EGraphCompoundElementKind::Target
				|| Edits[0].GetIntent() != EChangeIntent::Create
				|| Edits[0].GetIdentity().GetOwnerId()
					!= Request.GetTargetRef()
				|| Edits[0].GetIdentity().GetSemanticId()
					!= TEXT("target:") + Request.GetTargetRef()
				|| !Edits[0].GetProposedAfter().IsValid()
				|| !HasExactObjectFields(
					*Edits[0].GetProposedAfter(),
					{TEXT("data_type_ref"), TEXT("source_level_ref")})
				|| !TryCanonicalStringField(
					*Edits[0].GetProposedAfter(),
					TEXT("data_type_ref"), OutDataTypeRef)
				|| !TryCanonicalStringField(
					*Edits[0].GetProposedAfter(),
					TEXT("source_level_ref"), OutSourceRef)
				|| Changes[0].GetIdentity()
						.GetOwnerId() != Request.GetTargetRef()
				|| Changes[0].GetIntent() != EChangeIntent::Create)
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_data_from_level_plan_invalid"),
					Request.GetTargetRef());
				return false;
			}
			if (!LoadSupportedPcgLevelPointDataClass(OutDataTypeRef))
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_data_type_unsupported"),
					OutDataTypeRef);
				return false;
			}
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
				SetError(OutError,
					Cancellation->IsCancelled()
						? TEXT("graph_compound_pcg_ai_cancelled")
						: TEXT("graph_compound_pcg_ai_thread_invalid"),
					Profile.GetCapabilityId());
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
				SetError(OutError,
					TEXT("graph_compound_pcg_source_level_invalid"), SourceRef);
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
				SetError(OutError, RevalidateError.GetCode(),
					RevalidateError.GetIdentityKey(), RevalidateError.GetDetail());
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
					*Resources, Prepared, DurableError)
				|| !Prepared.IsValid())
			{
				SetError(OutError, DurableError.GetCode(),
					Request.GetTargetRef(), DurableError.GetDetail());
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
			const bool bExported = bNativeExported
				&& Asset->GetPathName() == ObjectPath(Request.GetTargetRef())
				&& Asset->ObjectPath.ToString() == SourceRef
				&& Asset->Data.TaggedData.Num() == 2;
			bool bExactData = bExported;
			if (bExactData)
			{
				for (const FPCGTaggedData& Tagged : Asset->Data.TaggedData)
				{
					bExactData = bExactData && Tagged.Data
						&& Tagged.Data->GetClass() == ExpectedDataClass;
				}
			}
			if (!bExactData)
			{
				Asset = nullptr;
				Package = nullptr;
				FString RollbackDetail;
				TryRollbackCreate(
					Request.GetTargetRef(), true, Prepared.ToSharedRef(),
					DurableBackend, FencingToken, *Resources, RollbackDetail);
				SetError(OutError,
					TEXT("graph_compound_pcg_level_export_failed"),
					Request.GetTargetRef(), RollbackDetail);
				return false;
			}
			Asset->SetFlags(RF_Public | RF_Standalone);
			FAssetRegistryModule::AssetCreated(Asset);
			Package->MarkPackageDirty();
			FDurableReceipt DurableReceipt;
			if (!TryPersistAssetDurability(
					Prepared.ToSharedRef(), DurableBackend, FencingToken,
					*Resources, DurableReceipt, DurableError))
			{
				Asset = nullptr;
				Package = nullptr;
				FString RollbackDetail;
				TryRollbackCreate(
					Request.GetTargetRef(), true, Prepared.ToSharedRef(),
					DurableBackend, FencingToken, *Resources, RollbackDetail);
				SetError(OutError, DurableError.GetCode(),
					Request.GetTargetRef(), DurableError.GetDetail()
						+ TEXT(";") + RollbackDetail);
				return false;
			}
			Package->SetDirtyFlag(false);
			Asset = nullptr;
			Package = nullptr;
			const FAssetDurableSessionRef Persisted =
				PersistedSession(Prepared.ToSharedRef(), DurableReceipt);

			FUeAgentCanonicalValuePtr Expected;
			if (!TryBuildPcgLevelDataAssetValue(
					DataTypeRef, SourceRef, *Resources, Expected, OutError)
				|| !Expected.IsValid())
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
			if (!FIndependentReadbackService::TryCreate(
					{Oracle}, Readback, VerifyError)
				|| !Readback.IsValid()
				|| !Readback->TryVerify(
					FIndependentReadbackRequest(
						TEXT("graph-compound-pcg-ai:") + Plan->GetRequestId(),
						PcgLevelDataAssetReadbackOracleId,
						Request.GetTargetRef(), Expected.ToSharedRef()),
					*Resources, Outcome, VerifyError)
				|| Outcome.GetStatus() != EVerificationStatus::Verified)
			{
				FString RollbackDetail;
				TryRollbackCreate(
					Request.GetTargetRef(), true, Persisted,
					DurableBackend, FencingToken, *Resources, RollbackDetail);
				SetError(OutError,
					VerifyError.GetCode().IsEmpty()
						? TEXT("graph_compound_pcg_level_readback_failed")
						: VerifyError.GetCode(),
					Request.GetTargetRef(), Outcome.GetDiagnosticCode()
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
				TryRollbackCreate(
					Request.GetTargetRef(), true, Persisted,
					DurableBackend, FencingToken, *Resources, RollbackDetail);
				SetError(OutError, DurableError.GetCode(),
					Request.GetTargetRef(), DurableError.GetDetail()
						+ TEXT(";") + RollbackDetail);
				return false;
			}
			const FString Revision = TEXT("md5:") + FMD5::HashAnsiString(
				*(Request.GetTargetRef() + TEXT("|") + SourceRef
					+ TEXT("|") + DataTypeRef));
			return TrySerializeResult(
				Request.GetTargetRef(), Revision, Outcome.GetEvidenceId(),
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
					SetError(OutError,
						TEXT("graph_compound_production_binding_unimplemented"),
						Profile.GetCapabilityId());
					return false;
				}
				if (!IsInGameThread() || Cancellation->IsCancelled())
				{
					SetError(OutError,
						Cancellation->IsCancelled()
							? TEXT("graph_compound_pcg_ai_cancelled")
							: TEXT("graph_compound_pcg_ai_thread_invalid"),
						Profile.GetCapabilityId());
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
					SetError(OutError,
						TEXT("graph_compound_production_binding_unimplemented"),
						Profile.GetCapabilityId());
					return false;
				}
				if (!IsInGameThread() || Cancellation->IsCancelled())
				{
					SetError(OutError,
						Cancellation->IsCancelled()
							? TEXT("graph_compound_pcg_ai_cancelled")
							: TEXT("graph_compound_pcg_ai_thread_invalid"),
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
						TEXT("graph_compound_pcg_ai_plan_invalid"),
						Request.GetTargetRef());
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
					SetError(OutError, RevalidateError.GetCode(),
						RevalidateError.GetIdentityKey(),
						RevalidateError.GetDetail());
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
						*Resources, Prepared, DurableError)
					|| !Prepared.IsValid())
				{
					SetError(OutError, DurableError.GetCode(),
						Request.GetTargetRef(), DurableError.GetDetail());
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
				const bool bCreated = CreatedAsset
					&& CreatedAsset->GetClass() == Spec->AssetClass
					&& NativeHook.TryCaptureOwnedAssetObject(
						*Package, Request.GetTargetKind(),
						*Spec->AssetClass,
						*AssetName(Request.GetTargetRef()), *CreatedAsset,
						ApplyReceipt, BarrierReceipt, Identity,
						NativeRevision, NativeEvidence, NativeError)
					&& BarrierReceipt.IsComplete()
					&& BarrierReceipt.GetErrorCount() == 0;
				if (!bCreated)
				{
					CreatedAsset = nullptr;
					Package = nullptr;
					FString RollbackDetail;
					TryRollbackCreate(
						Request.GetTargetRef(), true,
						Prepared.ToSharedRef(), DurableBackend,
						FencingToken, *Resources, RollbackDetail);
					SetError(OutError,
						!OutError.Code.IsEmpty()
							? OutError.Code
							: (NativeError.GetCode().IsEmpty()
								? TEXT("graph_compound_pcg_ai_factory_failed")
								: NativeError.GetCode()),
						Request.GetTargetRef(),
						!OutError.Detail.IsEmpty()
							? OutError.Detail + TEXT(";") + RollbackDetail
							: NativeError.GetDetail() + TEXT(";")
								+ RollbackDetail);
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
					TryUnloadTarget(
						Request.GetTargetRef(), true, UnloadDetail);
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
				const FString OracleId = ReadbackOracleId(*Spec);
				const TSharedRef<FAssetCreateReadbackOracle,
					ESPMode::ThreadSafe> Oracle =
					MakeShared<FAssetCreateReadbackOracle,
						ESPMode::ThreadSafe>(
							Request.GetTargetRef(), Spec->TargetKind,
							OracleId, *Spec->AssetClass);
				FIndependentReadbackOutcome Outcome;
				if (!Expected.IsValid()
					|| !FIndependentReadbackService::TryCreate(
						{Oracle}, Readback, VerifyError)
					|| !Readback.IsValid()
					|| !Readback->TryVerify(
						FIndependentReadbackRequest(
							TEXT("graph-compound-pcg-ai:")
								+ Plan->GetRequestId(),
							OracleId,
							Request.GetTargetRef(),
							Expected.ToSharedRef()),
						*Resources, Outcome, VerifyError)
					|| Outcome.GetStatus() != EVerificationStatus::Verified)
				{
					FString RollbackDetail;
					TryRollbackCreate(
						Request.GetTargetRef(), true, Persisted,
						DurableBackend, FencingToken,
						*Resources, RollbackDetail);
					SetError(OutError,
						VerifyError.GetCode().IsEmpty()
							? TEXT("graph_compound_pcg_ai_readback_failed")
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
						Request.GetTargetRef(), true, Persisted,
						DurableBackend, FencingToken,
						*Resources, RollbackDetail);
					SetError(OutError, DurableError.GetCode(),
						Request.GetTargetRef(),
						DurableError.GetDetail() + TEXT(";")
							+ RollbackDetail);
					return false;
				}
				return TrySerializeResult(
					Request.GetTargetRef(), NativeRevision,
					Outcome.GetEvidenceId(),
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
		if (!Config.IsValid())
		{
			SetError(OutError,
				TEXT("graph_compound_production_config_invalid"),
				TEXT("pcg_ai_node_graph"));
			return false;
		}
		OutEnvironment = MakeShared<FPcgAiProductionEnvironment>(Config);
		return OutEnvironment.IsValid();
	}
}
