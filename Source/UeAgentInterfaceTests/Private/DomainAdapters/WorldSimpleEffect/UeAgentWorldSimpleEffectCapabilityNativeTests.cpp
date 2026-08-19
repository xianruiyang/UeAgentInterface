#include "Misc/AutomationTest.h"

#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectCapabilities.h"


#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorLevelUtils.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/GarbageCollection.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerInstanceWithAsset.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/WorldPartition.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::GenericEffect;
	using namespace UE::UAI::DomainAdapters::WorldSimpleEffect;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	class FOwnedReferenceResolver final
		: public IUeAgentWorldSimpleEffectReferenceResolver
	{
	public:
		bool TryResolveReference(
			UWorld& World,
			const FString& CanonicalRef,
			UObject*& OutObject,
			FExecutionMutationError& OutError) override
		{
			++ResolveCalls;
			OutObject = Objects.FindRef(CanonicalRef);
			if (!IsValid(OutObject))
			{
				OutError = FExecutionMutationError(
					TEXT("world_simple_reference_unresolved"),
					CanonicalRef);
				return false;
			}
			return true;
		}

		void Bind(const FString& CanonicalRef, UObject* Object)
		{
			Objects.Add(CanonicalRef, Object);
		}

		TMap<FString, UObject*> Objects;
		int32 ResolveCalls = 0;
	};

	class FOwnedWorld final
	{
	public:
		FOwnedWorld()
		{
			const FString Suffix =
				FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Package = CreatePackage(
				*(TEXT("/Temp/UAI/WorldSimple/") + Suffix));
			World = UWorld::CreateWorld(
				EWorldType::EditorPreview,
				false,
				FName(TEXT("WorldSimpleOwnedWorld")),
				Package);
			if (World && GEngine)
			{
				GEngine->CreateNewWorldContext(EWorldType::EditorPreview)
					.SetCurrentWorld(World);
				bWorldContextRegistered = true;
			}
			if (!World || !World->PersistentLevel)
			{
				return;
			}

			World->PersistentLevel->SetUseExternalActors(true);
			WorldPartition =
				UWorldPartition::CreateOrRepairWorldPartition(
					World->GetWorldSettings());
			if (WorldPartition
				&& !WorldPartition->IsInitialized())
			{
				WorldPartition->Initialize(
					World,
					FTransform::Identity);
			}
			WorldDataLayers = World->GetWorldDataLayers();

			Actor = World->SpawnActor<AActor>(
				FVector::ZeroVector,
				FRotator::ZeroRotator);
			if (Actor)
			{
				Actor->Rename(TEXT("WorldSimpleOwnedActor"));
			}

			DataLayerAsset = NewObject<UDataLayerAsset>(
				Package,
				TEXT("WorldSimpleDataLayer"),
				RF_Transactional);
			if (DataLayerAsset)
			{
				DataLayerAsset->AddToRoot();
				DataLayerAsset->SetType(EDataLayerType::Runtime);
			}
			HlodLayer = NewObject<UHLODLayer>(
				Package,
				TEXT("WorldSimpleHLODLayer"),
				RF_Transactional);
			if (HlodLayer)
			{
				HlodLayer->AddToRoot();
			}
			Mesh = LoadObject<UStaticMesh>(
				nullptr,
				TEXT("/Engine/BasicShapes/Cube.Cube"));

			StreamingObjectPath =
				TEXT(
					"/Engine/Maps/Templates/"
					"Template_Default.Template_Default");
			StreamingAsset = LoadObject<UWorld>(
				nullptr,
				*StreamingObjectPath);
			if (StreamingAsset)
			{
				StreamingPackage =
					StreamingAsset->GetOutermost()->GetName();
				StreamingLevel =
					UEditorLevelUtils::AddLevelToWorld(
						World,
						*StreamingPackage,
						ULevelStreamingDynamic::StaticClass(),
						FTransform::Identity);
			}
			if (World && World->PersistentLevel)
			{
				World->SetCurrentLevel(World->PersistentLevel);
			}
			Package->SetDirtyFlag(false);
		}

		~FOwnedWorld()
		{
			if (Package)
			{
				Package->SetDirtyFlag(false);
			}
			if (World)
			{
				if (bWorldContextRegistered && GEngine)
				{
					GEngine->DestroyWorldContext(World);
					bWorldContextRegistered = false;
				}
				World->DestroyWorld(false);
				World->MarkAsGarbage();
			}
			if (HlodLayer && HlodLayer->IsRooted())
			{
				HlodLayer->RemoveFromRoot();
			}
			if (DataLayerAsset && DataLayerAsset->IsRooted())
			{
				DataLayerAsset->RemoveFromRoot();
			}
			if (Package)
			{
				Package->MarkAsGarbage();
			}
			CollectGarbage(RF_NoFlags, true);
		}

		bool IsValid() const
		{
			return Package
				&& World
				&& bWorldContextRegistered
				&& World->PersistentLevel
				&& WorldPartition
				&& WorldPartition->IsInitialized()
				&& WorldPartition->GetDataLayerManager()
				&& WorldDataLayers
				&& Actor
				&& DataLayerAsset
				&& HlodLayer
				&& Mesh
				&& StreamingLevel
				&& StreamingAsset;
		}

		UDataLayerInstance* CreateDataLayerInstance()
		{
			if (!WorldDataLayers || !DataLayerAsset)
			{
				return nullptr;
			}
			if (const UDataLayerInstance* Existing =
				WorldDataLayers->GetDataLayerInstance(
					DataLayerAsset))
			{
				return const_cast<UDataLayerInstance*>(
					Existing);
			}
			return WorldDataLayers
				->CreateDataLayer<UDataLayerInstanceWithAsset>(
					DataLayerAsset);
		}

		ULevelStreaming* FindStreamingRelation() const
		{
			if (!World)
			{
				return nullptr;
			}
			for (ULevelStreaming* Relation
				: World->GetStreamingLevels())
			{
				if (Relation
					&& Relation->GetWorldAssetPackageName()
						== StreamingPackage)
				{
					return Relation;
				}
			}
			return nullptr;
		}

		UPackage* Package = nullptr;
		UWorld* World = nullptr;
		bool bWorldContextRegistered = false;
		UWorldPartition* WorldPartition = nullptr;
		AWorldDataLayers* WorldDataLayers = nullptr;
		AActor* Actor = nullptr;
		UDataLayerAsset* DataLayerAsset = nullptr;
		UDataLayerInstance* DataLayerInstance = nullptr;
		UHLODLayer* HlodLayer = nullptr;
		UStaticMesh* Mesh = nullptr;
		FString StreamingObjectPath;
		FString StreamingPackage;
		ULevelStreaming* StreamingLevel = nullptr;
		UWorld* StreamingAsset = nullptr;
	};

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
	MakeResources()
	{
		return MakeShared<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe>(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1048576},
				{EUeAgentResourceDimension::OutputBytes, 1048576},
				{EUeAgentResourceDimension::StringBytes, 1048576},
				{EUeAgentResourceDimension::Nodes, 16384},
				{EUeAgentResourceDimension::Items, 16384},
				{EUeAgentResourceDimension::Files, 64},
				{EUeAgentResourceDimension::ReferenceEdges, 16384},
				{EUeAgentResourceDimension::WorkUnits, 262144},
				{
					EUeAgentResourceDimension::
						EstimatedMemoryBytes,
					33554432,
				},
			}));
	}

	bool TryCanonicalValue(
		const TSharedPtr<FJsonValue>& JsonValue,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError)
	{
		OutValue.Reset();
		OutError = {};
		if (!JsonValue.IsValid())
		{
			return false;
		}
		switch (JsonValue->Type)
		{
		case EJson::Null:
			return FUeAgentCanonicalValue::TryNull(
				Resources,
				OutValue,
				OutError);
		case EJson::Boolean:
			return FUeAgentCanonicalValue::TryBoolean(
				JsonValue->AsBool(),
				Resources,
				OutValue,
				OutError);
		case EJson::String:
			return FUeAgentCanonicalValue::TryString(
				JsonValue->AsString(),
				Resources,
				OutValue,
				OutError);
		case EJson::Number:
			return FUeAgentCanonicalValue::TryDecimal(
				FString::SanitizeFloat(
					JsonValue->AsNumber()),
				Resources,
				OutValue,
				OutError);
		case EJson::Array:
			{
				TArray<FUeAgentCanonicalValueRef> Items;
				for (const TSharedPtr<FJsonValue>& Item
					: JsonValue->AsArray())
				{
					FUeAgentCanonicalValuePtr CanonicalItem;
					if (!TryCanonicalValue(
							Item,
							Resources,
							CanonicalItem,
							OutError))
					{
						return false;
					}
					Items.Add(CanonicalItem.ToSharedRef());
				}
				return FUeAgentCanonicalValue::TryArray(
					Items,
					Resources,
					OutValue,
					OutError);
			}
		case EJson::Object:
			{
				TArray<
					TPair<FString, FUeAgentCanonicalValueRef>>
					Items;
				for (const TPair<
					FString,
					TSharedPtr<FJsonValue>>& Pair
					: JsonValue->AsObject()->Values)
				{
					FUeAgentCanonicalValuePtr CanonicalItem;
					if (!TryCanonicalValue(
							Pair.Value,
							Resources,
							CanonicalItem,
							OutError))
					{
						return false;
					}
					Items.Emplace(
						Pair.Key,
						CanonicalItem.ToSharedRef());
				}
				return FUeAgentCanonicalValue::TryObject(
					Items,
					Resources,
					OutValue,
					OutError);
			}
		default:
			return false;
		}
	}

	FUeAgentCanonicalValuePtr CanonicalInput(
		FAutomationTestBase* Test,
		const FString& Json,
		FUeAgentResourceTracker& Resources)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Json);
		if (!Test->TestTrue(
				TEXT("parse canonical native input"),
				FJsonSerializer::Deserialize(Reader, Object)
					&& Object.IsValid()))
		{
			return nullptr;
		}
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		Test->TestTrue(
			TEXT("convert native input to C4 canonical value"),
			TryCanonicalValue(
				MakeShared<FJsonValueObject>(Object),
				Resources,
				Value,
				Error));
		return Value;
	}

	FString NativeOperationType(const FString& CapabilityId)
	{
		if (CapabilityId == TEXT("world.actor.create")
			|| CapabilityId == TEXT("world.wall.create_with_opening")
			|| CapabilityId
				== TEXT("world.data_layer_instance.create")
			|| CapabilityId
				== TEXT("world.streaming_relation.create"))
		{
			return TEXT("world.entity.create");
		}
		if (CapabilityId
				== TEXT("world.data_layer_relation.update")
			|| CapabilityId
				== TEXT("world.actor_hlod_layer.update"))
		{
			return TEXT("world.membership.update");
		}
		return TEXT("world.state.update");
	}

	bool HasCanonicalObjectField(
		const FUeAgentCanonicalValuePtr& Value,
		const FString& Field)
	{
		if (!Value.IsValid()
			|| Value->GetKind() != EUeAgentValueKind::Object)
		{
			return false;
		}
		for (const TPair<
			FString,
			FUeAgentCanonicalValueRef>& Item
			: Value->GetObjectItems())
		{
			if (Item.Key == Field)
			{
				return true;
			}
		}
		return false;
	}

	FUeAgentCanonicalSnapshotPtr MakeCurrentSnapshot(
		FAutomationTestBase* Test,
		const FUeAgentOperationDescriptor& Descriptor,
		UWorld& World,
		const FString& CurrentStateJson,
		FUeAgentResourceTracker& Resources)
	{
		const FUeAgentStateRevision Revision(
			TEXT("world-simple-e2"),
			TEXT("owned-worker:r1"),
			1);
		TArray<FUeAgentStateEntry> Entries;
		if (!CurrentStateJson.IsEmpty())
		{
			FUeAgentCanonicalValuePtr Current =
				CanonicalInput(
					Test,
					CurrentStateJson,
					Resources);
			FUeAgentCanonicalValuePtr Default;
			FUeAgentStateSemanticError ValueError;
			if (!Current.IsValid()
				|| !FUeAgentCanonicalValue::TryObject(
					{},
					Resources,
					Default,
					ValueError))
			{
				Test->AddError(
					TEXT(
						"world_simple_current_snapshot_value_failed:")
					+ ValueError.Code);
				return nullptr;
			}
			Entries.Emplace(
				FUeAgentCanonicalIdentity(
					TEXT("uai.world.simple.current:")
						+ Descriptor.GetCapabilityId(),
					NativeOperationType(
						Descriptor.GetCapabilityId()),
					World.GetPathName(),
					Descriptor.GetCapabilityId(),
					TEXT("current_state"),
					TEXT("owned-worker:r1")),
				FUeAgentCanonicalType(
					TEXT("world_simple_current_state"),
					EUeAgentValueKind::Object,
					false,
					TEXT("object")),
				Current.ToSharedRef(),
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::Schema,
					TEXT(
						"world-simple:e2:current-state-default"),
					TEXT(
						"test-only default; before is independent readback"),
					Default.ToSharedRef()),
				FUeAgentStateDetails(
					Descriptor.GetCapabilityId(),
					TEXT(""),
					{TEXT("WorldSimpleEffect")},
					true,
					TEXT("available"),
					{}),
				Revision);
		}

		FUeAgentCanonicalSnapshotPtr Snapshot;
		FUeAgentStateSemanticError Error;
		if (!Test->TestTrue(
				TEXT("create independent current-state snapshot"),
				FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("snapshot:world-simple-e2:")
						+ Descriptor.GetCapabilityId(),
					EUeAgentStatePlane::AuthoringMemory,
					Revision,
					FUeAgentTemporalPoint(
						TEXT("owned-worker-clock"),
						TEXT("1")),
					FUeAgentStateProvenance(
						TEXT("independent_readback"),
						TEXT("owned-world-native-api"),
						TEXT(
							"world-simple-e2:prestate-readback")),
					Entries,
					Resources,
					Snapshot,
					Error)))
		{
			Test->AddError(
				TEXT("world_simple_snapshot_failed:")
				+ Error.Code);
			return nullptr;
		}
		return Snapshot;
	}

	FUeAgentExecutionContextRefOwner MakeExecutionContext(
		const FUeAgentDescriptorIdentity& Descriptor,
		UWorld& World)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId =
			TEXT("project:world-simple-e2");
		Input.EditorInstanceId =
			TEXT("editor:owned-worker");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.WorldType = EUeAgentExecutionWorldType::Preview;
		Input.WorldIdentity = World.GetPathName();
		Input.LevelIdentity =
			World.PersistentLevel->GetPathName();
		Input.ContextRevision =
			FString::Printf(
				TEXT("owned-world:%u"),
				World.GetUniqueID());
		Input.bResolvedFromCurrent = false;

		FUeAgentExecutionContextRefPtr Context;
		FUeAgentExecutionContextError Error;
		check(FUeAgentExecutionContextRef::TryCreate(
			Input,
			Descriptor,
			EUeAgentRequestKind::Operation,
			FUeAgentExecutionContextPolicy(
				{EUeAgentStatePlane::AuthoringMemory},
				{
					EUeAgentExecutionContextFacet::Level,
					EUeAgentExecutionContextFacet::World,
				},
				EUeAgentContextResolutionPolicy::ExplicitOnly,
				EUeAgentTransientContextPolicy::
					TemporaryMutationAllowed),
			Context,
			Error));
		return Context.ToSharedRef();
	}

	struct FNativeRequest
	{
		FNativeRequest(
			TSharedRef<
				FUeAgentResourceTracker,
				ESPMode::ThreadSafe> InResources,
			FPlanningPreflightPlanRef InPlan,
			FUeAgentExecutionContextRefOwner InContext)
			: Resources(MoveTemp(InResources))
			, Cancellation([this]() { return Now; })
			, Plan(MoveTemp(InPlan))
			, Context(MoveTemp(InContext))
		{
		}

		FExecutionMutationRequest MakeRequest()
		{
			return FExecutionMutationRequest(
				TEXT("operation:world-simple-e2"),
				TEXT("owner:world-simple-e2"),
				Plan,
				Context,
				Resources,
				FUeAgentOperationDeadline::FromTimeout(
					30.0,
					[this]() { return Now; }),
				Cancellation.GetToken(),
				EExecutionThreadAffinity::GameThread,
				EExecutionReentrancyPolicy::RejectSameTarget);
		}

		double Now = FPlatformTime::Seconds();
		TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
			Resources;
		FUeAgentCancellationSource Cancellation;
		FPlanningPreflightPlanRef Plan;
		FUeAgentExecutionContextRefOwner Context;
	};

	FUeAgentGenericEffectBinding MakeBinding(
		const FUeAgentOperationDescriptor& Descriptor,
		const FUeAgentCanonicalIdentity& Identity)
	{
		return FUeAgentGenericEffectBinding(
			FUeAgentCandidateBinding(
				TEXT("binding:") + Descriptor.GetCapabilityId(),
				FUeAgentCandidateSummary(
					Identity,
					Descriptor.GetIdentity(),
					TEXT("world_simple"),
					Descriptor.GetCapabilityId(),
					TEXT("owned WorldSimple target"),
					{TEXT("World"), TEXT("SimpleAuthoring")}),
				FUeAgentCatalogRevision(
					TEXT("world-simple-e2"),
					TEXT("owned-worker:r1"),
					1),
				TEXT("uai://world-simple/")
					+ Descriptor.GetCapabilityId(),
				TEXT("world-read"),
				{
					EUeAgentCandidateOperation::Create,
					EUeAgentCandidateOperation::Update,
				},
				TEXT("fixture:world-simple-e2:binding")),
			TEXT("UAI.WorldSimple:CanonicalObject"),
			FUeAgentCanonicalType(
				TEXT("world_simple_canonical_object"),
				EUeAgentValueKind::Object,
				false,
				TEXT("object")),
			TEXT("fixture:world-simple-e2:type-binding"));
	}

	struct FAppliedCapability
	{
		TSharedPtr<IUeAgentWorldSimpleEffectNativeHook> Hook;
		TUniquePtr<FNativeRequest> Fixture;
		TOptional<FExecutionMutationRequest> Request;
		FUeAgentGenericEffectApplyResult Result;
	};

	bool ApplyCapability(
		FAutomationTestBase* Test,
		const TSharedRef<
			FOwnedReferenceResolver,
			ESPMode::ThreadSafe>& Resolver,
		const TSharedRef<const FUeAgentDescriptorRegistry>& Registry,
		UWorld& World,
		const FString& DescriptorId,
		const FString& InputJson,
		const FString& CurrentStateJson,
		FAppliedCapability& OutApplied,
		const bool bExpectApply = true)
	{
		const FUeAgentOperationDescriptor* Descriptor =
			Registry->FindByDescriptorId(DescriptorId);
		if (!Test->TestNotNull(
				*(DescriptorId + TEXT(" descriptor exists")),
				Descriptor))
		{
			return false;
		}

		TSharedPtr<const FUeAgentContractSchema> CompiledInput;
		FUeAgentContractSchemaError SchemaError;
		if (!Test->TestTrue(
				*(DescriptorId
					+ TEXT(" input schema compiles before planning")),
				FUeAgentContractSchema::TryCompile(
					Descriptor->GetInputSchema(),
					CompiledInput,
					SchemaError))
			|| !CompiledInput.IsValid())
		{
			Test->AddError(
				DescriptorId + TEXT(":")
				+ SchemaError.Code);
			return false;
		}
		TOptional<FUeAgentContractValue> CanonicalPayload;
		SchemaError = {};
		if (!Test->TestTrue(
				*(DescriptorId
					+ TEXT(" public input canonicalizes once in C0")),
				CompiledInput->TryValidateAndCanonicalize(
					FUeAgentContractValue(InputJson),
					[](int64,
						const FString&,
						FUeAgentContractSchemaError&)
					{
						return true;
					},
					CanonicalPayload,
					SchemaError))
			|| !CanonicalPayload.IsSet())
		{
			Test->AddError(
				DescriptorId + TEXT(":")
				+ SchemaError.Code);
			return false;
		}

		const TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe> Resources = MakeResources();
		const FUeAgentCanonicalSnapshotPtr CurrentSnapshot =
			MakeCurrentSnapshot(
				Test,
				*Descriptor,
				World,
				CurrentStateJson,
				*Resources);
		if (!CurrentSnapshot.IsValid())
		{
			return false;
		}
		const FUeAgentExecutionContextRefOwner Context =
			MakeExecutionContext(
				Descriptor->GetIdentity(),
				World);
		const FUeAgentOperationRequest OperationRequest(
			Descriptor->GetIdentity(),
			TEXT("request:world-simple-e2"),
			TEXT("idempotency:world-simple-e2"),
			CanonicalPayload.GetValue());
		TOptional<FChangeSetDraft> Draft;
		FExecutionMutationError Error;
		if (!Test->TestTrue(
				*(DescriptorId
					+ TEXT(
						" descriptor builder creates typed draft")),
				TryBuildWorldSimpleEffectChangeSetDraft(
					OperationRequest,
					*Context,
					CurrentSnapshot.ToSharedRef(),
					*Resources,
					Draft,
					Error))
			|| !Draft.IsSet()
			|| !Draft->IsValid())
		{
			Test->AddError(
				DescriptorId + TEXT(":")
				+ Error.GetCode());
			return false;
		}

		const FString ExpectedNativeType =
			NativeOperationType(
				Descriptor->GetCapabilityId());
		const TArray<FChangeDraft>& DraftChanges =
			Draft->GetChanges();
		Test->TestTrue(
			*(DescriptorId
				+ TEXT(" expands to independently lockable changes")),
			!DraftChanges.IsEmpty());
		if (Descriptor->GetCapabilityId()
			== TEXT("world.wall.create_with_opening"))
		{
			Test->TestEqual(
				TEXT(
					"wall request is fully expanded before mutation"),
				DraftChanges.Num(),
				4);
		}
		for (const FChangeDraft& Change : DraftChanges)
		{
			Test->TestEqual(
				*(DescriptorId
					+ TEXT(" uses one of three native change types")),
				Change.GetIdentity().GetTargetKind(),
				ExpectedNativeType);
			Test->TestTrue(
				*(DescriptorId
					+ TEXT(" has a typed proposed state")),
				Change.GetProposedAfter().IsValid());
			for (const FString& PublicEnvelopeField
				: TArray<FString>{
				TEXT("changes"),
				TEXT("collection_refs"),
				TEXT("initial_state"),
				TEXT("opening"),
				TEXT("segment_template"),
				TEXT("subject_refs"),
				TEXT("target_refs"),
				TEXT("wall")})
			{
				Test->TestFalse(
					*(DescriptorId
						+ TEXT(
							" removes public envelope before native")),
					HasCanonicalObjectField(
						Change.GetProposedAfter(),
						PublicEnvelopeField));
			}
			if (Change.GetIntent() != EChangeIntent::Create)
			{
				Test->TestTrue(
					*(DescriptorId
						+ TEXT(
							" update copies independent before state")),
					Change.GetExpectedBefore().IsValid());
			}
		}

		FPlanningResult PlanningResult;
		FPlanningPreflightError PlanningError;
		if (!Test->TestTrue(
				*(DescriptorId + TEXT(" freezes through shared C6")),
				TryPlanChanges(
					FPlanningRequest(
						Draft.GetValue(),
						FUeAgentCallerContext(
							TEXT("agent:world-simple-e2"),
							{TEXT("uai:world:write")},
							TEXT(
								"correlation:world-simple-e2")),
						false),
					*Resources,
					PlanningResult,
					PlanningError))
			|| !PlanningResult.IsReady()
			|| !PlanningResult.GetPlan().IsValid())
		{
			Test->AddError(
				DescriptorId + TEXT(":")
				+ PlanningError.GetCode());
			return false;
		}

		TSharedPtr<IUeAgentWorldSimpleEffectNativeHook> Hook;
		Error = FExecutionMutationError();
		if (!Test->TestTrue(
				*(DescriptorId + TEXT(" native hook creates")),
				FUeAgentWorldSimpleEffectNativeHookFactory::
					TryCreate(
						Resolver,
						Hook,
						Error)))
		{
			Test->AddError(
				DescriptorId + TEXT(":") + Error.GetCode());
			return false;
		}

		OutApplied.Fixture = MakeUnique<FNativeRequest>(
			Resources,
			PlanningResult.GetPlan().ToSharedRef(),
			Context);
		OutApplied.Request.Emplace(
			OutApplied.Fixture->MakeRequest());
		OutApplied.Hook = MoveTemp(Hook);
		TArray<FUeAgentGenericEffectBinding> Bindings;
		for (const FPlannedChange& Change
			: OutApplied.Fixture->Plan
				->GetChangeSet()
				.GetChanges())
		{
			Bindings.Add(
				MakeBinding(
					*Descriptor,
					Change.GetIdentity()));
		}
		Error = FExecutionMutationError();
		const bool bApplied =
			OutApplied.Hook->TryApply(
				World,
				OutApplied.Request.GetValue(),
				FLockFenceLease(
					TEXT("owner:world-simple-e2"),
					{World.GetPathName()},
					1),
				Bindings,
				*OutApplied.Fixture->Resources,
				OutApplied.Result,
				Error);
		Test->TestEqual(
			*(DescriptorId + TEXT(" native apply matches expectation")),
			bApplied,
			bExpectApply);
		if (bApplied != bExpectApply)
		{
			Test->AddError(
				DescriptorId + TEXT(":") + Error.GetCode());
		}
		Test->TestEqual(
			*(DescriptorId
				+ TEXT(" mutation-start fact matches apply result")),
			OutApplied.Result.DidMutationStart(),
			bExpectApply);
		return bApplied;
	}

	void RollbackCapability(
		FAutomationTestBase* Test,
		UWorld& World,
		const FString& DescriptorId,
		FAppliedCapability& Applied)
	{
		bool bConfirmed = false;
		FExecutionMutationError Error;
		Test->TestTrue(
			*(DescriptorId + TEXT(" rollback executes")),
			Applied.Hook->TryRollback(
				World,
				Applied.Request.GetValue(),
				Applied.Result,
				*Applied.Fixture->Resources,
				bConfirmed,
				Error));
		Test->TestTrue(
			*(DescriptorId + TEXT(" rollback is confirmed")),
			bConfirmed);
	}

	int32 CountWallSegments(UWorld& World)
	{
		int32 Count = 0;
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			if (It->GetActorLabel().StartsWith(
				TEXT("WorldSimpleWall_")))
			{
				++Count;
			}
		}
		return Count;
	}

	AActor* FindActorByTag(UWorld& World, const FName Tag)
	{
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			if (It->Tags.Contains(Tag))
			{
				return *It;
			}
		}
		return nullptr;
	}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSimpleEffectTenCapabilitiesOwnedWorldTest,
	"UeAgentInterface.DomainAdapters.WorldSimpleEffect.E2.Native."
		"TenCapabilitiesOwnedWorld",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSimpleEffectTenCapabilitiesOwnedWorldTest::RunTest(
	const FString& Parameters)
{
	if (!TestTrue(
			TEXT("native capability test owns the game thread"),
			IsInGameThread()))
	{
		return false;
	}

	FOwnedWorld Owner;
	if (!TestTrue(
			TEXT("owned worker world is valid"),
			Owner.IsValid()))
	{
		return false;
	}
	const TSharedRef<
		FOwnedReferenceResolver,
		ESPMode::ThreadSafe> Resolver =
			MakeShared<
				FOwnedReferenceResolver,
				ESPMode::ThreadSafe>();
	Resolver->Bind(
		TEXT("/Script/Engine.StaticMeshActor"),
		AStaticMeshActor::StaticClass());
	Resolver->Bind(
		TEXT("/Engine/BasicShapes/Cube.Cube"),
		Owner.Mesh);
	Resolver->Bind(
		TEXT("uai://asset/data-layer"),
		Owner.DataLayerAsset);
	Resolver->Bind(
		TEXT("uai://actor/owned"),
		Owner.Actor);
	Resolver->Bind(
		TEXT("uai://asset/hlod-layer"),
		Owner.HlodLayer);
	Resolver->Bind(
		TEXT("uai://asset/streaming-level"),
		Owner.StreamingAsset);
	Resolver->Bind(
		TEXT("/Script/Engine.LevelStreamingDynamic"),
		ULevelStreamingDynamic::StaticClass());
	Resolver->Bind(
		TEXT("/Script/Engine.GameModeBase"),
		AGameModeBase::StaticClass());

	TSharedPtr<const FUeAgentDescriptorRegistry>
		MutableRegistry;
	FString RegistryError;
	if (!TestTrue(
			TEXT("create WorldSimpleEffect production registry"),
			TryCreateWorldSimpleEffectDescriptorRegistry(
				MutableRegistry,
				RegistryError))
		|| !MutableRegistry.IsValid())
	{
		AddError(
			TEXT("world_simple_registry_failed:")
			+ RegistryError);
		return false;
	}
	const TSharedRef<const FUeAgentDescriptorRegistry> Registry =
		MutableRegistry.ToSharedRef();

	FAppliedCapability Applied;
	const FName CreatedActorTag(TEXT("UAIActorCreateTest"));
	if (ApplyCapability(
			this,
			Resolver,
			Registry,
			*Owner.World,
			TEXT("uai.operation.world.actor.create"),
			TEXT(
				"{\"actor_class_ref\":\"/Script/Engine.StaticMeshActor\"," 
				"\"folder_path\":\"UAI/Tests\"," 
				"\"label\":\"WorldSimpleActorCreate\"," 
				"\"placement\":{\"basis_evidence\":"
				"\"explicit UE world frame\"," 
				"\"frame_id\":\"ue.world\"," 
				"\"rotation_quaternion_xyzw\":[0,0,0,1],"
				"\"scale\":[1,1,1],\"translation\":[125,-75,50],"
				"\"unit\":\"centimeter\"},"
				"\"save_level\":false,"
				"\"tags\":[\"UAIActorCreateTest\"]}"),
			FString(),
			Applied))
	{
		AActor* CreatedActor = FindActorByTag(*Owner.World, CreatedActorTag);
		TestNotNull(TEXT("actor create readback finds the real actor"), CreatedActor);
		if (CreatedActor)
		{
			TestTrue(TEXT("actor create resolves the requested class"),
				CreatedActor->IsA<AStaticMeshActor>());
			TestEqual(TEXT("actor create writes the requested label"),
				CreatedActor->GetActorLabel(), FString(TEXT("WorldSimpleActorCreate")));
			TestEqual(TEXT("actor create writes the requested folder"),
				CreatedActor->GetFolderPath(), FName(TEXT("UAI/Tests")));
			TestEqual(TEXT("actor create writes the requested translation"),
				CreatedActor->GetActorLocation(), FVector(125.0, -75.0, 50.0));
		}
		RollbackCapability(
			this,
			*Owner.World,
			TEXT("uai.operation.world.actor.create"),
			Applied);
		TestNull(TEXT("actor create rollback removes the actor"),
			FindActorByTag(*Owner.World, CreatedActorTag));
	}

	Applied = {};
	const bool bDirtyBeforeRejectedWall =
		Owner.Package->IsDirty();
	TestFalse(
		TEXT(
			"unresolved wall reference is rejected after planning"),
		ApplyCapability(
			this,
			Resolver,
			Registry,
			*Owner.World,
			TEXT(
				"uai.operation.world.wall."
				"create_with_opening"),
			TEXT(
				"{\"opening\":{\"center\":[0,0],"
				"\"size\":[100,200]},"
				"\"plane\":{\"basis_evidence\":"
				"\"explicit UE world frame\","
				"\"center\":[0,0,100],"
				"\"frame_id\":\"ue.world\","
				"\"normal\":[1,0,0],"
				"\"unit\":\"centimeter\","
				"\"up\":[0,0,1]},"
				"\"segment_template\":{"
				"\"actor_class_ref\":"
				"\"/Script/Engine.StaticMeshActor\","
				"\"label_prefix\":\"WorldSimpleWall_\","
				"\"mesh_ref\":\"uai://missing/mesh\"},"
				"\"wall\":{\"height\":300,"
				"\"thickness\":20,\"width\":400}}"),
			FString(),
			Applied,
			false));
	TestEqual(
		TEXT(
			"failed wall pre-resolves every reference before SpawnActor"),
		CountWallSegments(*Owner.World),
		0);
	TestEqual(
		TEXT("failed wall leaves package dirty state unchanged"),
		Owner.Package->IsDirty(),
		bDirtyBeforeRejectedWall);

	Applied = {};
	if (ApplyCapability(
			this,
			Resolver,
			Registry,
			*Owner.World,
			TEXT(
				"uai.operation.world.wall."
				"create_with_opening"),
			TEXT(
				"{\"opening\":{\"center\":[0,0],"
				"\"size\":[100,200]},"
				"\"plane\":{\"basis_evidence\":"
				"\"explicit UE world frame\","
				"\"center\":[0,0,100],"
				"\"frame_id\":\"ue.world\","
				"\"normal\":[1,0,0],"
				"\"unit\":\"centimeter\","
				"\"up\":[0,0,1]},"
				"\"segment_template\":{"
				"\"actor_class_ref\":"
				"\"/Script/Engine.StaticMeshActor\","
				"\"label_prefix\":\"WorldSimpleWall_\","
				"\"mesh_ref\":"
				"\"/Engine/BasicShapes/Cube.Cube\"},"
				"\"wall\":{\"height\":300,"
				"\"thickness\":20,\"width\":400}}"),
			FString(),
			Applied))
	{
		TestEqual(
			TEXT("wall readback has four real segments"),
			CountWallSegments(*Owner.World),
			4);
		RollbackCapability(
			this,
			*Owner.World,
			TEXT(
				"uai.operation.world.wall."
				"create_with_opening"),
			Applied);
		TestEqual(
			TEXT("wall rollback removes every segment"),
			CountWallSegments(*Owner.World),
			0);
	}

	Applied = {};
	if (ApplyCapability(
			this,
			Resolver,
			Registry,
			*Owner.World,
			TEXT(
				"uai.operation.world."
				"data_layer_instance.create"),
			TEXT(
				"{\"asset_ref\":\"uai://asset/data-layer\","
				"\"initial_state\":{"
				"\"initial_runtime_state\":\"activated\","
				"\"initially_visible\":true}}"),
			FString(),
			Applied))
	{
		Owner.DataLayerInstance =
			const_cast<UDataLayerInstance*>(
				Owner.WorldDataLayers
					->GetDataLayerInstance(
						Owner.DataLayerAsset));
		TestNotNull(
			TEXT("DataLayer independent readback finds instance"),
			Owner.DataLayerInstance);
		if (Owner.DataLayerInstance)
		{
			TestEqual(
				TEXT("DataLayer initial runtime state reads back"),
				Owner.DataLayerInstance->GetInitialRuntimeState(),
				EDataLayerRuntimeState::Activated);
		}
		RollbackCapability(
			this,
			*Owner.World,
			TEXT(
				"uai.operation.world."
				"data_layer_instance.create"),
			Applied);
		TestNull(
			TEXT("DataLayer create rollback removes instance"),
			Owner.WorldDataLayers->GetDataLayerInstance(
				Owner.DataLayerAsset));
	}

	Owner.DataLayerInstance = Owner.CreateDataLayerInstance();
	TestNotNull(
		TEXT("membership fixture creates DataLayer instance"),
		Owner.DataLayerInstance);
	Resolver->Bind(
		TEXT("uai://data-layer/owned"),
		Owner.DataLayerInstance);

	Applied = {};
	if (Owner.DataLayerInstance
		&& ApplyCapability(
			this,
			Resolver,
			Registry,
			*Owner.World,
			TEXT(
				"uai.operation.world."
				"data_layer_relation.update"),
			TEXT(
				"{\"collection_refs\":["
				"\"uai://data-layer/owned\"],"
				"\"mode\":\"replace\","
				"\"relation_kind\":\"actor_membership\","
				"\"subject_refs\":[\"uai://actor/owned\"]}"),
			TEXT("{\"collection_refs\":[]}"),
			Applied))
	{
		TestTrue(
			TEXT("DataLayer membership reads from real actor"),
			Owner.Actor->GetDataLayerInstanceNames().Contains(
				Owner.DataLayerInstance->GetDataLayerFName()));
		RollbackCapability(
			this,
			*Owner.World,
			TEXT(
				"uai.operation.world."
				"data_layer_relation.update"),
			Applied);
		TestFalse(
			TEXT("DataLayer membership rollback restores actor"),
			Owner.Actor->GetDataLayerInstanceNames().Contains(
				Owner.DataLayerInstance->GetDataLayerFName()));
	}

	Applied = {};
	if (Owner.DataLayerInstance
		&& ApplyCapability(
			this,
			Resolver,
			Registry,
			*Owner.World,
			TEXT(
				"uai.operation.world."
				"data_layer_state.update"),
			TEXT(
				"{\"changes\":{\"initial_runtime_state\":"
				"\"loaded\",\"visible\":false},"
				"\"target_refs\":["
				"\"uai://data-layer/owned\"]}"),
			FString::Printf(
				TEXT(
					"{\"initial_runtime_state\":%d,"
					"\"visible\":%s}"),
				static_cast<int32>(
					Owner.DataLayerInstance
						->GetInitialRuntimeState()),
				Owner.DataLayerInstance->IsVisible()
					? TEXT("true")
					: TEXT("false")),
			Applied))
	{
		TestEqual(
			TEXT("DataLayer state reads from real instance"),
			Owner.DataLayerInstance->GetInitialRuntimeState(),
			EDataLayerRuntimeState::Loaded);
		TestFalse(
			TEXT("DataLayer visibility reads from real instance"),
			Owner.DataLayerInstance->IsVisible());
		RollbackCapability(
			this,
			*Owner.World,
			TEXT(
				"uai.operation.world."
				"data_layer_state.update"),
			Applied);
	}

	Applied = {};
	if (ApplyCapability(
			this,
			Resolver,
			Registry,
			*Owner.World,
			TEXT(
				"uai.operation.world."
				"actor_hlod_layer.update"),
			TEXT(
				"{\"collection_ref\":"
				"\"uai://asset/hlod-layer\","
				"\"mode\":\"replace\","
				"\"subject_refs\":[\"uai://actor/owned\"]}"),
			TEXT("{\"collection_ref\":null}"),
			Applied))
	{
		TestEqual(
			TEXT("HLOD membership reads from real actor"),
			Owner.Actor->GetHLODLayer(),
			Owner.HlodLayer);
		RollbackCapability(
			this,
			*Owner.World,
			TEXT(
				"uai.operation.world."
				"actor_hlod_layer.update"),
			Applied);
		TestNull(
			TEXT("HLOD rollback restores empty membership"),
			Owner.Actor->GetHLODLayer());
	}

	Applied = {};
	if (ApplyCapability(
			this,
			Resolver,
			Registry,
			*Owner.World,
			TEXT(
				"uai.operation.world."
				"actor_streaming_policy.update"),
			TEXT(
				"{\"changes\":{\"is_spatially_loaded\":false,"
				"\"runtime_grid\":{\"intent\":\"set\","
				"\"value\":\"OwnedGrid\"}},"
				"\"target_refs\":[\"uai://actor/owned\"]}"),
			FString::Printf(
				TEXT(
					"{\"is_spatially_loaded\":%s,"
					"\"runtime_grid\":\"%s\"}"),
				Owner.Actor->GetIsSpatiallyLoaded()
					? TEXT("true")
					: TEXT("false"),
				*Owner.Actor->GetRuntimeGrid().ToString()),
			Applied))
	{
		TestEqual(
			TEXT("runtime grid reads from real actor"),
			Owner.Actor->GetRuntimeGrid(),
			FName(TEXT("OwnedGrid")));
		TestFalse(
			TEXT("spatially-loaded reads from real actor"),
			Owner.Actor->GetIsSpatiallyLoaded());
		RollbackCapability(
			this,
			*Owner.World,
			TEXT(
				"uai.operation.world."
				"actor_streaming_policy.update"),
			Applied);
	}

	if (Owner.StreamingLevel
		&& Owner.StreamingLevel->GetLoadedLevel())
	{
		UEditorLevelUtils::RemoveLevelFromWorld(
			Owner.StreamingLevel->GetLoadedLevel(),
			true,
			false);
	}
	Owner.StreamingLevel = nullptr;
	Owner.StreamingAsset = LoadObject<UWorld>(
		nullptr,
		*Owner.StreamingObjectPath);
	TestNotNull(
		TEXT("streaming asset re-resolves after relation removal"),
		Owner.StreamingAsset);
	if (Owner.StreamingAsset)
	{
		Resolver->Bind(
			TEXT("uai://asset/streaming-level"),
			Owner.StreamingAsset);
	}
	Applied = {};
	if (ApplyCapability(
			this,
			Resolver,
			Registry,
			*Owner.World,
			TEXT(
				"uai.operation.world."
				"streaming_relation.create"),
			TEXT(
				"{\"level_asset_ref\":"
				"\"uai://asset/streaming-level\","
				"\"placement\":{\"basis_evidence\":"
				"\"explicit UE world frame\","
				"\"frame_id\":\"ue.world\","
				"\"rotation_quaternion_xyzw\":[0,0,0,1],"
				"\"scale\":[1,1,1],"
				"\"translation\":[100,200,0],"
				"\"unit\":\"centimeter\"},"
				"\"streaming_class_ref\":"
				"\"/Script/Engine.LevelStreamingDynamic\"}"),
			FString(),
			Applied))
	{
		Owner.StreamingLevel = Owner.FindStreamingRelation();
		TestNotNull(
			TEXT("streaming relation reads from real world"),
			Owner.StreamingLevel);
		if (Owner.StreamingLevel)
		{
			TestTrue(
				TEXT("streaming placement reads back"),
				Owner.StreamingLevel->LevelTransform
					.GetLocation()
					.Equals(FVector(100.0, 200.0, 0.0)));
		}
		RollbackCapability(
			this,
			*Owner.World,
			TEXT(
				"uai.operation.world."
				"streaming_relation.create"),
			Applied);
		TestNull(
			TEXT("streaming relation rollback removes relation"),
			Owner.FindStreamingRelation());
	}

	Owner.StreamingLevel = UEditorLevelUtils::AddLevelToWorld(
		Owner.World,
		*Owner.StreamingPackage,
		ULevelStreamingDynamic::StaticClass(),
		FTransform::Identity);
	TestNotNull(
		TEXT("streaming state fixture re-adds relation"),
		Owner.StreamingLevel);
	if (Owner.StreamingLevel)
	{
		Owner.World->SetCurrentLevel(
			Owner.World->PersistentLevel);
		ULevel* const CurrentLevelBeforeStreamingState =
			Owner.World->GetCurrentLevel();
		Resolver->Bind(
			TEXT("uai://streaming-relation/owned"),
			Owner.StreamingLevel);
		Applied = {};
		if (ApplyCapability(
				this,
				Resolver,
				Registry,
				*Owner.World,
				TEXT(
					"uai.operation.world."
					"streaming_relation.state.update"),
				TEXT(
					"{\"changes\":{\"loaded\":true,"
					"\"placement\":{\"basis_evidence\":"
					"\"explicit UE world frame\","
					"\"frame_id\":\"ue.world\","
					"\"rotation_quaternion_xyzw\":"
					"[0,0,0,1],\"scale\":[1,1,1],"
					"\"translation\":[10,20,30],"
					"\"unit\":\"centimeter\"},"
					"\"visible\":true},"
					"\"target_ref\":"
					"\"uai://streaming-relation/owned\"}"),
				FString::Printf(
					TEXT(
						"{\"loaded\":%s,"
						"\"translation\":[%s,%s,%s],"
						"\"visible\":%s}"),
					Owner.StreamingLevel->ShouldBeLoaded()
						? TEXT("true")
						: TEXT("false"),
					*FString::SanitizeFloat(
						Owner.StreamingLevel->LevelTransform
							.GetLocation().X),
					*FString::SanitizeFloat(
						Owner.StreamingLevel->LevelTransform
							.GetLocation().Y),
					*FString::SanitizeFloat(
						Owner.StreamingLevel->LevelTransform
							.GetLocation().Z),
					Owner.StreamingLevel->ShouldBeVisible()
						? TEXT("true")
						: TEXT("false")),
				Applied))
		{
			TestTrue(
				TEXT("streaming loaded intent reads back"),
				Owner.StreamingLevel->ShouldBeLoaded());
			TestTrue(
				TEXT("streaming visible intent reads back"),
				Owner.StreamingLevel->ShouldBeVisible());
			TestTrue(
				TEXT("streaming state placement reads back"),
				Owner.StreamingLevel->LevelTransform
					.GetLocation()
					.Equals(FVector(10.0, 20.0, 30.0)));
			TestEqual(
				TEXT("streaming state never changes current level"),
				Owner.World->GetCurrentLevel(),
				CurrentLevelBeforeStreamingState);
			RollbackCapability(
				this,
				*Owner.World,
				TEXT(
					"uai.operation.world."
					"streaming_relation.state.update"),
				Applied);
		}
	}

	const bool bOriginalWorldPartitionStreaming =
		Owner.WorldPartition->IsStreamingEnabledInEditor();
	Applied = {};
	if (ApplyCapability(
			this,
			Resolver,
			Registry,
			*Owner.World,
			TEXT(
				"uai.operation.world."
				"world_partition_streaming.update"),
			bOriginalWorldPartitionStreaming
				? TEXT("{\"enabled\":false}")
				: TEXT("{\"enabled\":true}"),
			bOriginalWorldPartitionStreaming
				? TEXT("{\"enabled\":true}")
				: TEXT("{\"enabled\":false}"),
			Applied))
	{
		TestEqual(
			TEXT(
				"World Partition streaming flag reads from "
				"the exact real world"),
			Owner.WorldPartition->IsStreamingEnabledInEditor(),
			!bOriginalWorldPartitionStreaming);
		RollbackCapability(
			this,
			*Owner.World,
			TEXT(
				"uai.operation.world."
					"world_partition_streaming.update"),
			Applied);
		TestEqual(
			TEXT("World Partition rollback restores flag"),
			Owner.WorldPartition->IsStreamingEnabledInEditor(),
			bOriginalWorldPartitionStreaming);
	}

	AWorldSettings* const WorldSettings = Owner.World->GetWorldSettings();
	const TSubclassOf<AGameModeBase> OriginalGameMode =
		WorldSettings ? WorldSettings->DefaultGameMode : nullptr;
	Applied = {};
	if (WorldSettings
		&& ApplyCapability(
			this,
			Resolver,
			Registry,
			*Owner.World,
			TEXT("uai.operation.world.level_game_mode.update"),
			TEXT(
				"{\"game_mode_class_ref\":"
				"\"/Script/Engine.GameModeBase\","
				"\"save_level\":false}"),
			TEXT(
				"{\"game_mode_class_ref\":\"\","
				"\"save_level\":false,"
				"\"state_kind\":\"level_game_mode\"}"),
			Applied))
	{
		TestEqual(
			TEXT("level GameMode override reads from exact WorldSettings"),
			WorldSettings->DefaultGameMode.Get(),
			AGameModeBase::StaticClass());
		RollbackCapability(
			this,
			*Owner.World,
			TEXT("uai.operation.world.level_game_mode.update"),
			Applied);
		TestEqual(
			TEXT("level GameMode rollback restores WorldSettings"),
			WorldSettings->DefaultGameMode,
			OriginalGameMode);
	}

	TestEqual(
		TEXT("production registry owns exactly eleven descriptors"),
		Registry->GetDescriptors().Num(),
		11);
	return true;
}
