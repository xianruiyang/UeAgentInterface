#include "LegacyMechanics/UeAgentLegacyMechanics.h"

#include "Utilities/UeAgentAuthoringFolderIO.h"

#include "UeAgentHttpServer_AIOps.h"

#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "EngineUtils.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SmartObjectComponent.h"
#include "SmartObjectDefinition.h"
#include "SmartObjectRequestTypes.h"
#include "SmartObjectRuntime.h"
#include "SmartObjectSubsystem.h"
#include "SmartObjectTypes.h"
#include "StateTree.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/IToolkit.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Widgets/SWidget.h"

namespace UeAgentAIOps
{
	static TMap<FString, FSmartObjectRequestResult> GSmartObjectFindResults;
	static TMap<FString, FSmartObjectClaimHandle> GSmartObjectClaims;

	bool ResetSmartObjectSlots(USmartObjectDefinition* Definition, FString& OutError)
	{
		if (!Definition)
		{
			OutError = TEXT("smart_object_definition_not_found");
			return false;
		}
		FArrayProperty* SlotsProperty = FindFProperty<FArrayProperty>(Definition->GetClass(), TEXT("Slots"));
		if (!SlotsProperty)
		{
			OutError = TEXT("smart_object_slots_property_not_found");
			return false;
		}
		void* SlotsPtr = SlotsProperty->ContainerPtrToValuePtr<void>(Definition);
		FScriptArrayHelper Helper(SlotsProperty, SlotsPtr);
		Helper.EmptyValues();
		return true;
	}

	FSmartObjectSlotDefinition& AddSmartObjectSlot(USmartObjectDefinition* Definition)
	{
		return Definition->DebugAddSlot();
	}

	static bool CreateSmartObjectBehaviorDefinition(UObject* Outer, const TSharedPtr<FJsonObject>& BehaviorObj, USmartObjectBehaviorDefinition*& OutBehavior, TArray<TSharedPtr<FJsonValue>>& PropertyResults, FString& OutError)
	{
		OutBehavior = nullptr;
		if (!Outer || !BehaviorObj.IsValid())
		{
			OutError = TEXT("invalid_smart_object_behavior_definition");
			return false;
		}
		FString BehaviorClassPath;
		if (!BehaviorObj->TryGetStringField(TEXT("class"), BehaviorClassPath) || BehaviorClassPath.IsEmpty())
		{
			OutError = TEXT("missing_smart_object_behavior_definition_class");
			return false;
		}
		UClass* BehaviorClass = ResolveClass(BehaviorClassPath);
		if (!BehaviorClass || !BehaviorClass->IsChildOf(USmartObjectBehaviorDefinition::StaticClass()) || BehaviorClass->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = TEXT("smart_object_behavior_definition_class_not_found");
			return false;
		}
		OutBehavior = NewObject<USmartObjectBehaviorDefinition>(Outer, BehaviorClass, NAME_None, RF_Transactional);
		if (!OutBehavior)
		{
			OutError = TEXT("smart_object_behavior_definition_create_failed");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (BehaviorObj->TryGetArrayField(TEXT("properties"), Properties) && Properties)
		{
			for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
			{
				const TSharedPtr<FJsonObject>* PropObjPtr = nullptr;
				if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropObjPtr) || !PropObjPtr || !PropObjPtr->IsValid())
				{
					continue;
				}
				FString PropertyName;
				(*PropObjPtr)->TryGetStringField(TEXT("property_path"), PropertyName);
				const TSharedPtr<FJsonValue> CanonicalValue = (*PropObjPtr)->TryGetField(TEXT("value"));
				if (PropertyName.IsEmpty())
				{
					OutError = TEXT("missing_smart_object_behavior_property_name");
					return false;
				}
				FString ImportError;
				if (!CanonicalValue.IsValid())
				{
					OutError = TEXT("missing_smart_object_behavior_property_value");
					return false;
				}
				if (!ImportPropertyValue(OutBehavior->GetClass(), OutBehavior, OutBehavior, PropertyName, *PropObjPtr, ImportError))
				{
					OutError = ImportError;
					return false;
				}
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("owner"), OutBehavior->GetName());
				Result->SetStringField(TEXT("property_path"), PropertyName);
				Result->SetField(TEXT("requested_value"), CanonicalValue);
				Result->SetStringField(TEXT("status"), TEXT("applied"));
				PropertyResults.Add(MakeShared<FJsonValueObject>(Result));
			}
		}
		return true;
	}

	bool AddSmartObjectBehaviorDefinitions(UObject* Outer, const TArray<TSharedPtr<FJsonValue>>* Behaviors, TArray<TObjectPtr<USmartObjectBehaviorDefinition>>& Target, TArray<TSharedPtr<FJsonValue>>& PropertyResults, FString& OutError)
	{
		if (!Behaviors)
		{
			return true;
		}
		for (const TSharedPtr<FJsonValue>& BehaviorValue : *Behaviors)
		{
			const TSharedPtr<FJsonObject>* BehaviorObjPtr = nullptr;
			if (!BehaviorValue.IsValid() || !BehaviorValue->TryGetObject(BehaviorObjPtr) || !BehaviorObjPtr || !BehaviorObjPtr->IsValid())
			{
				continue;
			}
			USmartObjectBehaviorDefinition* Behavior = nullptr;
			if (!CreateSmartObjectBehaviorDefinition(Outer, *BehaviorObjPtr, Behavior, PropertyResults, OutError))
			{
				return false;
			}
			Target.Add(Behavior);
		}
		return true;
	}

	static bool ResetDefaultSmartObjectBehaviorDefinitions(USmartObjectDefinition* Definition, FString& OutError)
	{
		FArrayProperty* DefaultsProperty = Definition ? FindFProperty<FArrayProperty>(Definition->GetClass(), TEXT("DefaultBehaviorDefinitions")) : nullptr;
		if (!DefaultsProperty)
		{
			OutError = TEXT("smart_object_default_behaviors_property_not_found");
			return false;
		}
		void* DefaultsPtr = DefaultsProperty->ContainerPtrToValuePtr<void>(Definition);
		FScriptArrayHelper Helper(DefaultsProperty, DefaultsPtr);
		Helper.EmptyValues();
		return true;
	}

	static bool AddDefaultSmartObjectBehaviorDefinition(USmartObjectDefinition* Definition, USmartObjectBehaviorDefinition* Behavior, FString& OutError)
	{
		FArrayProperty* DefaultsProperty = Definition ? FindFProperty<FArrayProperty>(Definition->GetClass(), TEXT("DefaultBehaviorDefinitions")) : nullptr;
		FObjectPropertyBase* ObjectProperty = DefaultsProperty ? CastField<FObjectPropertyBase>(DefaultsProperty->Inner) : nullptr;
		if (!DefaultsProperty || !ObjectProperty)
		{
			OutError = TEXT("smart_object_default_behaviors_property_not_found");
			return false;
		}
		void* DefaultsPtr = DefaultsProperty->ContainerPtrToValuePtr<void>(Definition);
		FScriptArrayHelper Helper(DefaultsProperty, DefaultsPtr);
		const int32 NewIndex = Helper.AddValue();
		ObjectProperty->SetObjectPropertyValue(Helper.GetRawPtr(NewIndex), Behavior);
		return true;
	}

	bool ApplyDefaultSmartObjectBehaviorDefinitions(USmartObjectDefinition* Definition, const TArray<TSharedPtr<FJsonValue>>* Behaviors, TArray<TSharedPtr<FJsonValue>>& PropertyResults, FString& OutError)
	{
		if (!Behaviors)
		{
			return true;
		}
		if (!ResetDefaultSmartObjectBehaviorDefinitions(Definition, OutError))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& BehaviorValue : *Behaviors)
		{
			const TSharedPtr<FJsonObject>* BehaviorObjPtr = nullptr;
			if (!BehaviorValue.IsValid() || !BehaviorValue->TryGetObject(BehaviorObjPtr) || !BehaviorObjPtr || !BehaviorObjPtr->IsValid())
			{
				continue;
			}
			USmartObjectBehaviorDefinition* Behavior = nullptr;
			if (!CreateSmartObjectBehaviorDefinition(Definition, *BehaviorObjPtr, Behavior, PropertyResults, OutError))
			{
				return false;
			}
			if (!AddDefaultSmartObjectBehaviorDefinition(Definition, Behavior, OutError))
			{
				return false;
			}
		}
		return true;
	}

	bool ParseSmartObjectQueryBox(UWorld* World, const TSharedPtr<FJsonObject>& Params, FBox& OutBox, AActor*& OutAgent)
	{
		OutAgent = nullptr;
		FString AgentName;
		Params->TryGetStringField(TEXT("agent"), AgentName);
		if (!AgentName.IsEmpty() && World)
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->GetName().Equals(AgentName, ESearchCase::IgnoreCase) || It->GetActorLabel().Equals(AgentName, ESearchCase::IgnoreCase))
				{
					OutAgent = *It;
					break;
				}
			}
		}
		const TSharedPtr<FJsonObject>* BoxObjPtr = nullptr;
		if (Params->TryGetObjectField(TEXT("query_bounds"), BoxObjPtr) && BoxObjPtr && BoxObjPtr->IsValid())
		{
			FVector Min;
			FVector Max;
			FVector Center;
			FVector Extent;
			if (JsonTryGetVector(*BoxObjPtr, TEXT("min"), Min) && JsonTryGetVector(*BoxObjPtr, TEXT("max"), Max))
			{
				OutBox = FBox(Min, Max);
				return true;
			}
			if (JsonTryGetVector(*BoxObjPtr, TEXT("center"), Center) && JsonTryGetVector(*BoxObjPtr, TEXT("extent"), Extent))
			{
				OutBox = FBox::BuildAABB(Center, Extent);
				return true;
			}
		}
		double Radius = 2000.0;
		Params->TryGetNumberField(TEXT("radius"), Radius);
		const FVector Center = OutAgent ? OutAgent->GetActorLocation() : FVector::ZeroVector;
		OutBox = FBox::BuildAABB(Center, FVector(Radius));
		return true;
	}

	void RegisterSmartObjectComponentsInWorld(UWorld* World, USmartObjectSubsystem* Subsystem)
	{
		if (!World || !Subsystem)
		{
			return;
		}
#if WITH_SMARTOBJECT_DEBUG
		Subsystem->DebugInitializeRuntime();
#endif
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<USmartObjectComponent*> Components;
			It->GetComponents<USmartObjectComponent>(Components);
			for (USmartObjectComponent* Component : Components)
			{
				if (Component && Component->GetDefinition())
				{
					Subsystem->RegisterSmartObject(Component);
				}
			}
		}
	}

	void CollectSmartObjectActorsForQuery(UWorld* World, USmartObjectSubsystem* Subsystem, const FBox& QueryBox, TArray<AActor*>& OutActors, TArray<TSharedPtr<FJsonValue>>& OutDiagnostics)
	{
		OutActors.Reset();
		OutDiagnostics.Reset();
		if (!World || !Subsystem)
		{
			return;
		}
		TSet<AActor*> SeenActors;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			TArray<USmartObjectComponent*> Components;
			Actor->GetComponents<USmartObjectComponent>(Components);
			for (USmartObjectComponent* Component : Components)
			{
				if (!Component || !Component->GetDefinition())
				{
					continue;
				}
				const FBox ComponentBounds = Component->GetSmartObjectBounds();
				const FVector ComponentLocation = Component->GetComponentLocation();
				const FVector ActorLocation = Actor->GetActorLocation();
				const bool bInQuery = QueryBox.Intersect(ComponentBounds) || QueryBox.IsInside(ComponentLocation) || QueryBox.IsInside(ActorLocation);
				TSharedPtr<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
				Diagnostic->SetStringField(TEXT("owner"), Actor->GetName());
				Diagnostic->SetStringField(TEXT("component"), Component->GetName());
				Diagnostic->SetStringField(TEXT("definition"), Component->GetDefinition()->GetPathName());
				Diagnostic->SetObjectField(TEXT("actor_location"), VecToJson(ActorLocation));
				Diagnostic->SetObjectField(TEXT("component_location"), VecToJson(ComponentLocation));
				Diagnostic->SetObjectField(TEXT("bounds_min"), VecToJson(ComponentBounds.Min));
				Diagnostic->SetObjectField(TEXT("bounds_max"), VecToJson(ComponentBounds.Max));
				Diagnostic->SetBoolField(TEXT("in_query"), bInQuery);
				Diagnostic->SetBoolField(TEXT("bound_to_simulation"), Component->IsBoundToSimulation());
				Diagnostic->SetStringField(TEXT("registered_handle"), LexToString(Component->GetRegisteredHandle()));
				OutDiagnostics.Add(MakeShared<FJsonValueObject>(Diagnostic));
				if (bInQuery && !SeenActors.Contains(Actor))
				{
					SeenActors.Add(Actor);
					OutActors.Add(Actor);
				}
			}
		}
	}

	int32 CreateSmartObjectRuntimeEntriesForQuery(UWorld* World, USmartObjectSubsystem* Subsystem, const FBox& QueryBox, const FConstStructView UserData)
	{
		if (!World || !Subsystem)
		{
			return 0;
		}
#if WITH_SMARTOBJECT_DEBUG
		Subsystem->DebugInitializeRuntime();
#endif
		int32 CreatedCount = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<USmartObjectComponent*> Components;
			It->GetComponents<USmartObjectComponent>(Components);
			for (USmartObjectComponent* Component : Components)
			{
				const USmartObjectDefinition* Definition = Component ? Component->GetDefinition() : nullptr;
				if (!Component || !Definition)
				{
					continue;
				}
				const FBox ComponentBounds = Component->GetSmartObjectBounds();
				const FVector ComponentLocation = Component->GetComponentLocation();
				const AActor* Owner = Component->GetOwner();
				const FVector OwnerLocation = Owner ? Owner->GetActorLocation() : ComponentLocation;
				if (!QueryBox.Intersect(ComponentBounds) && !QueryBox.IsInside(ComponentLocation) && !QueryBox.IsInside(OwnerLocation))
				{
					continue;
				}
				if (Definition->HasBeenValidated() == false)
				{
					Definition->Validate();
				}
				if (!Definition->IsDefinitionValid())
				{
					continue;
				}
				const FTransform RuntimeTransform = Owner ? Owner->GetActorTransform() : Component->GetComponentTransform();
				const FSmartObjectHandle Handle = Subsystem->CreateSmartObject(*Definition, RuntimeTransform, UserData);
				if (Handle.IsValid())
				{
					++CreatedCount;
				}
			}
		}
		return CreatedCount;
	}
}

bool FUeAgentLegacyMechanics::CmdSmartObjectDefinitionCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	USmartObjectDefinition* Definition = nullptr;
	if (!UeAgentAIOps::CreateAsset<USmartObjectDefinition>(AssetPath, Definition, OutError) || !Definition)
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Tags = nullptr;
	if (!Ctx.Params->TryGetArrayField(TEXT("activity_tags"), Tags))
	{
		Ctx.Params->TryGetArrayField(TEXT("tags"), Tags);
	}
	if (Tags)
	{
		Definition->SetActivityTags(UeAgentAIOps::GameplayTagsFromJsonArray(Tags));
	}
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	const TArray<TSharedPtr<FJsonValue>>* DefaultBehaviors = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("default_behavior_definitions"), DefaultBehaviors))
	{
		if (!UeAgentAIOps::ApplyDefaultSmartObjectBehaviorDefinitions(Definition, DefaultBehaviors, PropertyResults, OutError))
		{
			return false;
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("slots"), Slots) && Slots)
	{
		for (const TSharedPtr<FJsonValue>& SlotValue : *Slots)
		{
			const TSharedPtr<FJsonObject>* SlotObjPtr = nullptr;
			if (!SlotValue.IsValid() || !SlotValue->TryGetObject(SlotObjPtr) || !SlotObjPtr || !SlotObjPtr->IsValid())
			{
				continue;
			}
			FSmartObjectSlotDefinition& Slot = UeAgentAIOps::AddSmartObjectSlot(Definition);
			FVector Offset = FVector::ZeroVector;
			FRotator Rotation = FRotator::ZeroRotator;
			FTransform LocalTransform;
			if (UeAgentAIOps::TryGetTransform(*SlotObjPtr, TEXT("local_transform"), LocalTransform))
			{
				Offset = LocalTransform.GetLocation();
				Rotation = LocalTransform.Rotator();
			}
			else
			{
				UeAgentAIOps::JsonTryGetVector(*SlotObjPtr, TEXT("offset"), Offset);
				UeAgentAIOps::JsonTryGetRotator(*SlotObjPtr, TEXT("rotation"), Rotation);
			}
			Slot.Offset = FVector3f(Offset);
			Slot.Rotation = FRotator3f(Rotation);
			bool bEnabled = true;
			(*SlotObjPtr)->TryGetBoolField(TEXT("enabled"), bEnabled);
			Slot.bEnabled = bEnabled;
#if WITH_EDITORONLY_DATA
			FString Name;
			if ((*SlotObjPtr)->TryGetStringField(TEXT("name"), Name))
			{
				Slot.Name = FName(*Name);
			}
#endif
			const TArray<TSharedPtr<FJsonValue>>* SlotTags = nullptr;
			if (!(*SlotObjPtr)->TryGetArrayField(TEXT("activity_tags"), SlotTags))
			{
				(*SlotObjPtr)->TryGetArrayField(TEXT("tags"), SlotTags);
			}
			if (SlotTags)
			{
				Slot.ActivityTags = UeAgentAIOps::GameplayTagsFromJsonArray(SlotTags);
			}
			if ((*SlotObjPtr)->TryGetArrayField(TEXT("runtime_tags"), SlotTags))
			{
				Slot.RuntimeTags = UeAgentAIOps::GameplayTagsFromJsonArray(SlotTags);
			}
			const TArray<TSharedPtr<FJsonValue>>* Behaviors = nullptr;
			if ((*SlotObjPtr)->TryGetArrayField(TEXT("behavior_definitions"), Behaviors))
			{
				if (!UeAgentAIOps::AddSmartObjectBehaviorDefinitions(Definition, Behaviors, Slot.BehaviorDefinitions, PropertyResults, OutError))
				{
					return false;
				}
			}
		}
	}
	else
	{
		UeAgentAIOps::AddSmartObjectSlot(Definition);
	}
	Definition->MarkPackageDirty();
	bool bSave = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSave);
	if (bSave && !UeAgentAIOps::SaveAssetPackage(Definition, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportSmartObjectDefinition(Definition, true);
	OutData->SetBoolField(TEXT("created_asset"), true);
	OutData->SetBoolField(TEXT("saved"), bSave);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentLegacyMechanics::CmdSmartObjectDefinitionGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	USmartObjectDefinition* Definition = UeAgentAIOps::LoadAsset<USmartObjectDefinition>(AssetPath);
	if (!Definition)
	{
		OutError = TEXT("smart_object_definition_not_found");
		return false;
	}
	OutData = UeAgentAIOps::ExportSmartObjectDefinition(Definition, true);
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentLegacyMechanics::CmdSmartObjectDefinitionExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdSmartObjectDefinitionGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	if (!OutputFile.TrimStartAndEnd().IsEmpty())
	{
		FString SavedPath;
		if (!UeAgentAIOps::SaveAuthoringFile(OutputFile, OutData, SavedPath, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), SavedPath);
	}
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentLegacyMechanics::CmdSmartObjectDefinitionValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString JsonFile;
	if (!JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile) || JsonFile.IsEmpty())
	{
		OutError = TEXT("missing_json_file");
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString ResolvedPath;
	if (!UeAgentAIOps::LoadAuthoringFile(JsonFile, Root, ResolvedPath, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	UeAgentAIOps::ValidateSmartObjectDefinitionJson(Root, Issues);
	OutData->SetStringField(TEXT("profile"), UeAgentAIOps::SmartObjectDefinitionProfile);
	OutData->SetStringField(TEXT("json_file"), ResolvedPath);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("validate_failed"));
	UeAgentAIOps::SetDiagnostics(OutData, Issues);
	return Issues.Num() == 0;
}

bool FUeAgentLegacyMechanics::CmdSmartObjectDefinitionApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString JsonFile;
	JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile);
	TSharedPtr<FJsonObject> Root;
	FString ResolvedPath;
	if (!JsonFile.IsEmpty())
	{
		if (!UeAgentAIOps::LoadAuthoringFile(JsonFile, Root, ResolvedPath, OutError))
		{
			return false;
		}
	}
	else
	{
		Root = Ctx.Params;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!UeAgentAIOps::ValidateSmartObjectDefinitionJson(Root, Issues))
	{
		OutData->SetStringField(TEXT("status"), TEXT("validate_failed"));
		UeAgentAIOps::SetDiagnostics(OutData, Issues);
		return false;
	}
	bool bDryRun = false;
	bool bValidateOnly = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("validate_only"), bValidateOnly);
	if (bDryRun || bValidateOnly)
	{
		OutData->SetStringField(TEXT("profile"), UeAgentAIOps::SmartObjectDefinitionProfile);
		OutData->SetStringField(TEXT("status"), TEXT("ok"));
		OutData->SetBoolField(TEXT("dry_run"), bDryRun);
		OutData->SetBoolField(TEXT("validate_only"), bValidateOnly);
		UeAgentAIOps::SetDiagnostics(OutData, Issues);
		return true;
	}
	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	bool bCreateIfMissing = false;
	bool bAllowDestructive = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	JsonTryGetBool(Ctx.Params, TEXT("allow_destructive"), bAllowDestructive);
	USmartObjectDefinition* Definition = UeAgentAIOps::LoadAsset<USmartObjectDefinition>(AssetPath);
	bool bCreated = false;
	if (!Definition)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("smart_object_definition_not_found");
			return false;
		}
		if (!UeAgentAIOps::CreateAsset<USmartObjectDefinition>(AssetPath, Definition, OutError) || !Definition)
		{
			return false;
		}
		bCreated = true;
	}
	if (Definition->GetSlots().Num() > 0 && !bCreated && !bAllowDestructive)
	{
		OutError = TEXT("smart_object_slot_rebuild_requires_allow_destructive");
		return false;
	}
	Definition->Modify();
	const TArray<TSharedPtr<FJsonValue>>* Tags = nullptr;
	if (!Root->TryGetArrayField(TEXT("activity_tags"), Tags))
	{
		Root->TryGetArrayField(TEXT("tags"), Tags);
	}
	if (Tags)
	{
		Definition->SetActivityTags(UeAgentAIOps::GameplayTagsFromJsonArray(Tags));
	}
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	const TArray<TSharedPtr<FJsonValue>>* DefaultBehaviors = nullptr;
	if (Root->TryGetArrayField(TEXT("default_behavior_definitions"), DefaultBehaviors))
	{
		if (!UeAgentAIOps::ApplyDefaultSmartObjectBehaviorDefinitions(Definition, DefaultBehaviors, PropertyResults, OutError))
		{
			return false;
		}
	}
	if (!UeAgentAIOps::ResetSmartObjectSlots(Definition, OutError))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
	Root->TryGetArrayField(TEXT("slots"), Slots);
	if (Slots)
	{
		for (const TSharedPtr<FJsonValue>& SlotValue : *Slots)
		{
			const TSharedPtr<FJsonObject>* SlotObjPtr = nullptr;
			if (!SlotValue.IsValid() || !SlotValue->TryGetObject(SlotObjPtr) || !SlotObjPtr || !SlotObjPtr->IsValid())
			{
				continue;
			}
			FSmartObjectSlotDefinition& Slot = UeAgentAIOps::AddSmartObjectSlot(Definition);
			FVector Offset = FVector::ZeroVector;
			FRotator Rotation = FRotator::ZeroRotator;
			FTransform LocalTransform;
			if (UeAgentAIOps::TryGetTransform(*SlotObjPtr, TEXT("local_transform"), LocalTransform))
			{
				Offset = LocalTransform.GetLocation();
				Rotation = LocalTransform.Rotator();
			}
			else
			{
				UeAgentAIOps::JsonTryGetVector(*SlotObjPtr, TEXT("offset"), Offset);
				UeAgentAIOps::JsonTryGetRotator(*SlotObjPtr, TEXT("rotation"), Rotation);
			}
			Slot.Offset = FVector3f(Offset);
			Slot.Rotation = FRotator3f(Rotation);
			bool bEnabled = true;
			(*SlotObjPtr)->TryGetBoolField(TEXT("enabled"), bEnabled);
			Slot.bEnabled = bEnabled;
#if WITH_EDITORONLY_DATA
			FString Name;
			if ((*SlotObjPtr)->TryGetStringField(TEXT("name"), Name))
			{
				Slot.Name = FName(*Name);
			}
#endif
			const TArray<TSharedPtr<FJsonValue>>* SlotTags = nullptr;
			if (!(*SlotObjPtr)->TryGetArrayField(TEXT("activity_tags"), SlotTags))
			{
				(*SlotObjPtr)->TryGetArrayField(TEXT("tags"), SlotTags);
			}
			if (SlotTags)
			{
				Slot.ActivityTags = UeAgentAIOps::GameplayTagsFromJsonArray(SlotTags);
			}
			if ((*SlotObjPtr)->TryGetArrayField(TEXT("runtime_tags"), SlotTags))
			{
				Slot.RuntimeTags = UeAgentAIOps::GameplayTagsFromJsonArray(SlotTags);
			}
			const TArray<TSharedPtr<FJsonValue>>* Behaviors = nullptr;
			if ((*SlotObjPtr)->TryGetArrayField(TEXT("behavior_definitions"), Behaviors) && Behaviors)
			{
				if (!UeAgentAIOps::AddSmartObjectBehaviorDefinitions(Definition, Behaviors, Slot.BehaviorDefinitions, PropertyResults, OutError))
				{
					return false;
				}
			}
		}
	}
	Definition->MarkPackageDirty();
	Definition->PostEditChange();
	bool bSave = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSave);
	if (bSave && !UeAgentAIOps::SaveAssetPackage(Definition, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportSmartObjectDefinition(Definition, true);
	OutData->SetStringField(TEXT("json_file"), ResolvedPath);
	OutData->SetBoolField(TEXT("created_asset"), bCreated);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSave);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentLegacyMechanics::CmdSmartObjectValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	OutData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Issues;
	FString DefinitionPath;
	JsonTryGetString(Ctx.Params, TEXT("definition_asset"), DefinitionPath);
	USmartObjectDefinition* Definition = DefinitionPath.IsEmpty() ? nullptr : UeAgentAIOps::LoadAsset<USmartObjectDefinition>(DefinitionPath);
	if (!Definition)
	{
		UeAgentAIOps::AddIssue(Issues, TEXT("definition_not_found"), TEXT("Smart Object Definition was not found."), TEXT("$.definition_asset"));
	}
	else
	{
		OutData->SetObjectField(TEXT("definition"), UeAgentAIOps::ExportSmartObjectDefinition(Definition, true));
		TArray<TPair<EMessageSeverity::Type, FText>> ValidationMessages;
		if (!Definition->Validate(&ValidationMessages))
		{
			for (const TPair<EMessageSeverity::Type, FText>& Message : ValidationMessages)
			{
				UeAgentAIOps::AddIssue(Issues, TEXT("definition_validation_failed"), Message.Value.ToString(), TEXT("$.definition_asset"));
			}
		}
	}
	UWorld* World = GetEditorWorld(OutError);
	if (World)
	{
		FString ActorName;
		JsonTryGetString(Ctx.Params, TEXT("owner_actor_or_blueprint"), ActorName);
		AActor* Actor = ActorName.IsEmpty() ? nullptr : FindActorByNameOrLabel(World, ActorName);
		if (Actor)
		{
			USmartObjectComponent* Component = Actor->FindComponentByClass<USmartObjectComponent>();
			if (!Component)
			{
				UeAgentAIOps::AddIssue(Issues, TEXT("smart_object_component_missing"), TEXT("Owner actor has no SmartObjectComponent."), TEXT("$.owner_actor_or_blueprint"));
			}
			else
			{
				OutData->SetStringField(TEXT("component_owner"), Actor->GetName());
				OutData->SetStringField(TEXT("component_definition"), Component->GetDefinition() ? Component->GetDefinition()->GetPathName() : FString());
				if (Definition && Component->GetDefinition() != Definition)
				{
					UeAgentAIOps::AddIssue(Issues, TEXT("component_definition_mismatch"), TEXT("SmartObjectComponent references a different definition."), TEXT("$.owner_actor_or_blueprint"));
				}
			}
		}
	}
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("validate_failed"));
	UeAgentAIOps::SetDiagnostics(OutData, Issues);
	return Issues.Num() == 0;
}

bool FUeAgentLegacyMechanics::CmdSmartObjectFind(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	USmartObjectSubsystem* Subsystem = USmartObjectSubsystem::GetCurrent(World);
	if (!Subsystem)
	{
		OutError = TEXT("smart_object_subsystem_not_found");
		return false;
	}
	UeAgentAIOps::RegisterSmartObjectComponentsInWorld(World, Subsystem);
	AActor* Agent = nullptr;
	FBox QueryBox(ForceInitToZero);
	UeAgentAIOps::ParseSmartObjectQueryBox(World, Ctx.Params, QueryBox, Agent);
	FSmartObjectRequestFilter Filter;
	const TArray<TSharedPtr<FJsonValue>>* RequiredTags = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("required_tags"), RequiredTags))
	{
		Filter.UserTags = UeAgentAIOps::GameplayTagsFromJsonArray(RequiredTags);
	}
	bool bIncludeClaimed = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_claimed"), bIncludeClaimed);
	Filter.bShouldIncludeClaimedSlots = bIncludeClaimed;
	FSmartObjectRequest Request(QueryBox, Filter);
	TArray<FSmartObjectRequestResult> Results;
	FSmartObjectActorUserData UserData(Agent);
	const FConstStructView UserDataView = Agent ? FConstStructView::Make(UserData) : FConstStructView();
	Subsystem->FindSmartObjects(Request, Results, UserDataView);
	TArray<AActor*> CandidateActors;
	TArray<TSharedPtr<FJsonValue>> CandidateDiagnostics;
	UeAgentAIOps::CollectSmartObjectActorsForQuery(World, Subsystem, QueryBox, CandidateActors, CandidateDiagnostics);
	bool bUsedActorListFallback = false;
	int32 CreatedRuntimeEntries = 0;
	if (Results.Num() == 0)
	{
		CreatedRuntimeEntries = UeAgentAIOps::CreateSmartObjectRuntimeEntriesForQuery(World, Subsystem, QueryBox, UserDataView);
		if (CreatedRuntimeEntries > 0)
		{
			Results.Reset();
			Subsystem->FindSmartObjects(Request, Results, UserDataView);
		}
	}
	if (Results.Num() == 0 && CandidateActors.Num() > 0)
	{
		Subsystem->FindSmartObjectsInList(Filter, CandidateActors, Results, UserDataView);
		bUsedActorListFallback = Results.Num() > 0;
	}
	int32 MaxResults = 16;
	double MaxResultsNumber = MaxResults;
	JsonTryGetNumber(Ctx.Params, TEXT("max_results"), MaxResultsNumber);
	MaxResults = (int32)FMath::Clamp(MaxResultsNumber, 1.0, 1024.0);
	TArray<TSharedPtr<FJsonValue>> ResultsJson;
	UeAgentAIOps::GSmartObjectFindResults.Reset();
	const int32 EmitCount = FMath::Min(MaxResults, Results.Num());
	for (int32 Index = 0; Index < EmitCount; ++Index)
	{
		const FString ResultId = FString::Printf(TEXT("result_%d"), Index);
		UeAgentAIOps::GSmartObjectFindResults.Add(ResultId, Results[Index]);
		ResultsJson.Add(MakeShared<FJsonValueObject>(UeAgentAIOps::SmartObjectRequestResultToJson(Results[Index], Subsystem, ResultId)));
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetObjectField(TEXT("query_min"), VecToJson(QueryBox.Min));
	OutData->SetObjectField(TEXT("query_max"), VecToJson(QueryBox.Max));
	OutData->SetStringField(TEXT("agent"), Agent ? Agent->GetName() : FString());
	OutData->SetNumberField(TEXT("result_count"), Results.Num());
	OutData->SetNumberField(TEXT("created_runtime_entries_for_query"), CreatedRuntimeEntries);
	OutData->SetNumberField(TEXT("candidate_actor_count"), CandidateActors.Num());
	OutData->SetBoolField(TEXT("used_actor_list_fallback"), bUsedActorListFallback);
	OutData->SetArrayField(TEXT("candidate_components"), CandidateDiagnostics);
	OutData->SetBoolField(TEXT("truncated"), Results.Num() > EmitCount);
	OutData->SetArrayField(TEXT("results"), ResultsJson);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentLegacyMechanics::CmdSmartObjectClaim(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	USmartObjectSubsystem* Subsystem = USmartObjectSubsystem::GetCurrent(World);
	if (!Subsystem)
	{
		OutError = TEXT("smart_object_subsystem_not_found");
		return false;
	}
	FString ResultId;
	JsonTryGetString(Ctx.Params, TEXT("result_id"), ResultId);
	if (ResultId.IsEmpty())
	{
		ResultId = TEXT("result_0");
	}
	const FSmartObjectRequestResult* Result = UeAgentAIOps::GSmartObjectFindResults.Find(ResultId);
	if (!Result || !Result->IsValid())
	{
		OutError = TEXT("smart_object_find_result_not_found");
		return false;
	}
	FString PriorityText;
	JsonTryGetString(Ctx.Params, TEXT("claim_priority"), PriorityText);
	const ESmartObjectClaimPriority Priority = UeAgentAIOps::EnumFromString(PriorityText, ESmartObjectClaimPriority::Normal);
	if (!Subsystem->CanBeClaimed(Result->SlotHandle, Priority))
	{
		OutError = TEXT("smart_object_slot_not_claimable");
		return false;
	}
	FString AgentName;
	JsonTryGetString(Ctx.Params, TEXT("agent"), AgentName);
	AActor* Agent = AgentName.IsEmpty() ? nullptr : FindActorByNameOrLabel(World, AgentName);
	FSmartObjectActorUserData UserData(Agent);
	const FConstStructView UserDataView = Agent ? FConstStructView::Make(UserData) : FConstStructView();
	const FSmartObjectClaimHandle Claim = Subsystem->MarkSlotAsClaimed(Result->SlotHandle, Priority, UserDataView);
	if (!Claim.IsValid())
	{
		OutError = TEXT("smart_object_claim_failed");
		return false;
	}
	const FString ClaimId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UeAgentAIOps::GSmartObjectClaims.Add(ClaimId, Claim);
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("claim_handle"), ClaimId);
	OutData->SetStringField(TEXT("claim_debug"), LexToString(Claim));
	OutData->SetObjectField(TEXT("result"), UeAgentAIOps::SmartObjectRequestResultToJson(*Result, Subsystem, ResultId));
	if (TOptional<FTransform> SlotTransform = Subsystem->GetSlotTransform(Claim))
	{
		OutData->SetObjectField(TEXT("slot_transform"), UeAgentAIOps::TransformToJson(*SlotTransform));
	}
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentLegacyMechanics::CmdSmartObjectRelease(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	USmartObjectSubsystem* Subsystem = USmartObjectSubsystem::GetCurrent(World);
	if (!Subsystem)
	{
		OutError = TEXT("smart_object_subsystem_not_found");
		return false;
	}
	FString ClaimId;
	if (!JsonTryGetString(Ctx.Params, TEXT("claim_handle"), ClaimId) || ClaimId.IsEmpty())
	{
		OutError = TEXT("missing_claim_handle");
		return false;
	}
	FSmartObjectClaimHandle Claim;
	if (!UeAgentAIOps::GSmartObjectClaims.RemoveAndCopyValue(ClaimId, Claim))
	{
		OutError = TEXT("smart_object_claim_handle_not_found");
		return false;
	}
	const bool bReleased = Subsystem->MarkSlotAsFree(Claim);
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("claim_handle"), ClaimId);
	OutData->SetBoolField(TEXT("released"), bReleased);
	OutData->SetStringField(TEXT("status"), bReleased ? TEXT("ok") : TEXT("release_failed"));
	return bReleased;
}

bool FUeAgentLegacyMechanics::CmdSmartObjectRuntimeSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Components;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		TArray<USmartObjectComponent*> SmartComponents;
		It->GetComponents<USmartObjectComponent>(SmartComponents);
		for (USmartObjectComponent* Component : SmartComponents)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("owner"), (*It) ? (*It)->GetName() : FString());
			Obj->SetStringField(TEXT("component"), Component ? Component->GetName() : FString());
			Obj->SetStringField(TEXT("definition"), (Component && Component->GetDefinition()) ? Component->GetDefinition()->GetPathName() : FString());
			Obj->SetBoolField(TEXT("bound_to_simulation"), Component && Component->IsBoundToSimulation());
			if (Component)
			{
				Obj->SetStringField(TEXT("registered_handle"), LexToString(Component->GetRegisteredHandle()));
			}
			Components.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}
	OutData->SetArrayField(TEXT("components"), Components);
	OutData->SetNumberField(TEXT("component_count"), Components.Num());
	OutData->SetNumberField(TEXT("cached_find_result_count"), UeAgentAIOps::GSmartObjectFindResults.Num());
	OutData->SetNumberField(TEXT("active_claim_count"), UeAgentAIOps::GSmartObjectClaims.Num());
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentLegacyMechanics::CmdSmartObjectRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> FindData;
	if (!CmdSmartObjectFind(Ctx, FindData, OutError))
	{
		return false;
	}
	bool bExpectClaimable = true;
	JsonTryGetBool(Ctx.Params, TEXT("expect_claimable"), bExpectClaimable);
	const int32 ResultCount = static_cast<int32>(FindData->GetNumberField(TEXT("result_count")));
	OutData = MakeShared<FJsonObject>();
	OutData->SetObjectField(TEXT("find"), FindData);
	OutData->SetObjectField(TEXT("find_result"), FindData);
	OutData->SetBoolField(TEXT("expect_claimable"), bExpectClaimable);
	OutData->SetBoolField(TEXT("found_any"), ResultCount > 0);
	bool bExpectReachable = false;
	JsonTryGetBool(Ctx.Params, TEXT("expect_reachable"), bExpectReachable);
	OutData->SetBoolField(TEXT("expect_reachable"), bExpectReachable);
	if (ResultCount <= 0)
	{
		OutData->SetBoolField(TEXT("probe_passed"), !bExpectClaimable);
		TSharedPtr<FJsonObject> NavigationResult = MakeShared<FJsonObject>();
		NavigationResult->SetBoolField(TEXT("evaluated"), false);
		NavigationResult->SetStringField(TEXT("reason"), TEXT("no_smart_object_result"));
		OutData->SetObjectField(TEXT("navigation_result"), NavigationResult);
		OutData->SetArrayField(TEXT("cleanup_results"), TArray<TSharedPtr<FJsonValue>>());
		OutData->SetStringField(TEXT("status"), TEXT("ok"));
		return true;
	}
	TSharedPtr<FJsonObject> ClaimParams = MakeShared<FJsonObject>();
	ClaimParams->SetStringField(TEXT("result_id"), TEXT("result_0"));
	FString Agent;
	if (JsonTryGetString(Ctx.Params, TEXT("agent"), Agent))
	{
		ClaimParams->SetStringField(TEXT("agent"), Agent);
	}
	FUeAgentRequestContext ClaimCtx = Ctx;
	ClaimCtx.Params = ClaimParams;
	TSharedPtr<FJsonObject> ClaimData;
	FString ClaimError;
	const bool bClaimed = CmdSmartObjectClaim(ClaimCtx, ClaimData, ClaimError);
	OutData->SetBoolField(TEXT("claimed"), bClaimed);
	TSharedPtr<FJsonObject> NavigationResult = MakeShared<FJsonObject>();
	NavigationResult->SetBoolField(TEXT("evaluated"), false);
	bool bReachable = false;
	bool bReachabilityEvaluated = false;
	if (bClaimed)
	{
		OutData->SetObjectField(TEXT("claim"), ClaimData);
		OutData->SetObjectField(TEXT("claim_result"), ClaimData);
		FString AgentName;
		JsonTryGetString(Ctx.Params, TEXT("agent"), AgentName);
		const TSharedPtr<FJsonObject>* SlotTransformObj = nullptr;
		if (!AgentName.IsEmpty() && ClaimData.IsValid() && ClaimData->TryGetObjectField(TEXT("slot_transform"), SlotTransformObj) && SlotTransformObj && SlotTransformObj->IsValid())
		{
			FString NavError;
			UWorld* World = GetEditorWorld(NavError);
			AActor* AgentActor = World ? FindActorByNameOrLabel(World, AgentName) : nullptr;
			if (AgentActor)
			{
				FVector SlotLocation = FVector::ZeroVector;
				const TSharedPtr<FJsonObject>* TranslationObj = nullptr;
				if ((*SlotTransformObj)->TryGetObjectField(TEXT("translation"), TranslationObj) && TranslationObj && TranslationObj->IsValid())
				{
					double X = 0.0;
					double Y = 0.0;
					double Z = 0.0;
					(*TranslationObj)->TryGetNumberField(TEXT("x"), X);
					(*TranslationObj)->TryGetNumberField(TEXT("y"), Y);
					(*TranslationObj)->TryGetNumberField(TEXT("z"), Z);
					SlotLocation = FVector(X, Y, Z);
					TSharedPtr<FJsonObject> NavParams = MakeShared<FJsonObject>();
					NavParams->SetObjectField(TEXT("start"), UeAgentAIOps::VecToJson(AgentActor->GetActorLocation()));
					NavParams->SetObjectField(TEXT("end"), UeAgentAIOps::VecToJson(SlotLocation));
					NavParams->SetBoolField(TEXT("allow_partial"), false);
					NavParams->SetBoolField(TEXT("allow_projection_failure"), true);
					FUeAgentRequestContext NavCtx = Ctx;
					NavCtx.Params = NavParams;
					TSharedPtr<FJsonObject> PathData;
					const bool bNavOk = CmdNavmeshFindPath(NavCtx, PathData, NavError);
					NavigationResult->SetBoolField(TEXT("evaluated"), true);
					NavigationResult->SetBoolField(TEXT("command_ok"), bNavOk);
					if (bNavOk && PathData.IsValid())
					{
						NavigationResult->SetObjectField(TEXT("path"), PathData);
						PathData->TryGetBoolField(TEXT("path_found"), bReachable);
					}
					else
					{
						NavigationResult->SetStringField(TEXT("error"), NavError);
					}
					bReachabilityEvaluated = true;
				}
			}
			else
			{
				NavigationResult->SetStringField(TEXT("reason"), TEXT("agent_not_found"));
			}
		}
		else
		{
			NavigationResult->SetStringField(TEXT("reason"), AgentName.IsEmpty() ? TEXT("missing_agent") : TEXT("missing_slot_transform"));
		}
		TSharedPtr<FJsonObject> ReleaseParams = MakeShared<FJsonObject>();
		ReleaseParams->SetStringField(TEXT("claim_handle"), ClaimData->GetStringField(TEXT("claim_handle")));
		FUeAgentRequestContext ReleaseCtx = Ctx;
		ReleaseCtx.Params = ReleaseParams;
		TSharedPtr<FJsonObject> ReleaseData;
		FString ReleaseError;
		const bool bReleased = CmdSmartObjectRelease(ReleaseCtx, ReleaseData, ReleaseError);
		OutData->SetBoolField(TEXT("released"), bReleased);
		if (!ReleaseData.IsValid())
		{
			ReleaseData = MakeShared<FJsonObject>();
			ReleaseData->SetStringField(TEXT("error"), ReleaseError);
			ReleaseData->SetStringField(TEXT("status"), TEXT("release_failed"));
		}
		OutData->SetObjectField(TEXT("release"), ReleaseData);
		OutData->SetObjectField(TEXT("release_result"), ReleaseData);
		TArray<TSharedPtr<FJsonValue>> CleanupResults;
		CleanupResults.Add(MakeShared<FJsonValueObject>(ReleaseData));
		OutData->SetArrayField(TEXT("cleanup_results"), CleanupResults);
		const bool bReachabilityPassed = !bExpectReachable || (bReachabilityEvaluated && bReachable);
		OutData->SetBoolField(TEXT("probe_passed"), (bExpectClaimable ? (bClaimed && bReleased) : false) && bReachabilityPassed);
	}
	else
	{
		OutData->SetStringField(TEXT("claim_error"), ClaimError);
		OutData->SetObjectField(TEXT("claim_result"), MakeShared<FJsonObject>());
		OutData->SetArrayField(TEXT("cleanup_results"), TArray<TSharedPtr<FJsonValue>>());
		OutData->SetBoolField(TEXT("probe_passed"), !bExpectClaimable);
	}
	NavigationResult->SetBoolField(TEXT("path_found"), bReachable);
	OutData->SetObjectField(TEXT("navigation_result"), NavigationResult);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentLegacyMechanics::CmdAIBehaviorStackExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput) || FolderInput.IsEmpty())
	{
		FolderInput = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("AIBehaviorStack"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}
	const FString FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderInput);
	if (!UeAgentAuthoringFolderIO::ResetExportFolder(FolderPath, OutError))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Manifest;
	FString ManifestFile;
	FString ManifestResolvedPath;
	JsonTryGetString(Ctx.Params, TEXT("manifest_file"), ManifestFile);
	if (!ManifestFile.IsEmpty())
	{
		if (!UeAgentAIOps::LoadAuthoringFile(ManifestFile, Manifest, ManifestResolvedPath, OutError))
		{
			return false;
		}
	}
	if (!Manifest.IsValid())
	{
		Manifest = MakeShared<FJsonObject>();
	}
	Manifest->SetStringField(TEXT("profile"), UeAgentAIOps::AIStackProfile);
	Manifest->SetNumberField(TEXT("schema_version"), 1);
	FString StackName;
	if (JsonTryGetString(Ctx.Params, TEXT("name"), StackName) && !StackName.IsEmpty())
	{
		Manifest->SetStringField(TEXT("name"), StackName);
	}
	else if (!Manifest->HasField(TEXT("name")))
	{
		Manifest->SetStringField(TEXT("name"), TEXT("AIBehaviorStack"));
	}

	const TCHAR* Keys[] = { TEXT("behavior_tree_asset"), TEXT("blackboard_asset"), TEXT("state_tree_asset"), TEXT("ai_controller_blueprint"), TEXT("pawn_blueprint") };
	for (const TCHAR* Key : Keys)
	{
		FString Value;
		if (JsonTryGetString(Ctx.Params, Key, Value) && !Value.IsEmpty())
		{
			Manifest->SetStringField(Key, Value);
		}
	}

	auto CollectStringArray = [&](const TCHAR* ArrayField, const TCHAR* SingleField)
	{
		TArray<FString> Items;
		const TArray<TSharedPtr<FJsonValue>>* ArrayValues = nullptr;
		if ((Ctx.Params->TryGetArrayField(ArrayField, ArrayValues) || Manifest->TryGetArrayField(ArrayField, ArrayValues)) && ArrayValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *ArrayValues)
			{
				FString Item;
				if (Value.IsValid() && Value->TryGetString(Item) && !Item.IsEmpty())
				{
					Items.Add(Item);
				}
			}
		}
		FString SingleValue;
		if (SingleField && (JsonTryGetString(Ctx.Params, SingleField, SingleValue) || Manifest->TryGetStringField(SingleField, SingleValue)) && !SingleValue.IsEmpty())
		{
			Items.AddUnique(SingleValue);
		}
		return Items;
	};
	auto SetStringArrayField = [&](const TCHAR* ArrayField, const TArray<FString>& Items)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Item : Items)
		{
			Values.Add(MakeShared<FJsonValueString>(Item));
		}
		Manifest->SetArrayField(ArrayField, Values);
	};
	auto AppendPrefixedFiles = [](TArray<TSharedPtr<FJsonValue>>& Target, const FString& Prefix, const TArray<TSharedPtr<FJsonValue>>& LocalFiles)
	{
		for (const TSharedPtr<FJsonValue>& Value : LocalFiles)
		{
			FString FileName;
			if (Value.IsValid() && Value->TryGetString(FileName))
			{
				Target.Add(MakeShared<FJsonValueString>(FPaths::Combine(Prefix, FileName).Replace(TEXT("\\"), TEXT("/"))));
			}
		}
	};
	auto SaveStackJson = [&](const FString& RelativeFile, const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& WrittenFiles)
	{
		return UeAgentAIOps::SaveTrackedFolderAuthoringFile(FolderPath, RelativeFile, Root, WrittenFiles, OutError);
	};

	TArray<TSharedPtr<FJsonValue>> WrittenFiles;
	bool bIncludeAssetSummaries = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_asset_summaries"), bIncludeAssetSummaries);

	FString BehaviorTreePath;
	Manifest->TryGetStringField(TEXT("behavior_tree_asset"), BehaviorTreePath);
	FString BlackboardPath;
	Manifest->TryGetStringField(TEXT("blackboard_asset"), BlackboardPath);
	if (!BehaviorTreePath.IsEmpty())
	{
		UBehaviorTree* Tree = UeAgentAIOps::LoadAsset<UBehaviorTree>(BehaviorTreePath);
		if (!Tree)
		{
			OutError = TEXT("behavior_tree_not_found");
			return false;
		}
		if (BlackboardPath.IsEmpty() && Tree->BlackboardAsset)
		{
			BlackboardPath = Tree->BlackboardAsset->GetPathName();
			Manifest->SetStringField(TEXT("blackboard_asset"), BlackboardPath);
		}
		if (bIncludeAssetSummaries)
		{
			TArray<TSharedPtr<FJsonValue>> LocalFiles;
			const FString RelativeFolder = FPaths::Combine(TEXT("behavior_tree"), UeAgentAIOps::AssetFolderNameFromPath(BehaviorTreePath)).Replace(TEXT("\\"), TEXT("/"));
			if (!UeAgentAIOps::SaveBehaviorTreeFolderContents(FPaths::Combine(FolderPath, RelativeFolder), Tree, LocalFiles, OutError))
			{
				return false;
			}
			AppendPrefixedFiles(WrittenFiles, RelativeFolder, LocalFiles);
		}
	}
	if (!BlackboardPath.IsEmpty() && bIncludeAssetSummaries)
	{
		UBlackboardData* Blackboard = UeAgentAIOps::LoadAsset<UBlackboardData>(BlackboardPath);
		if (!Blackboard)
		{
			OutError = TEXT("blackboard_not_found");
			return false;
		}
		const FString RelativeFile = FPaths::Combine(TEXT("blackboard"), UeAgentAIOps::AssetFolderNameFromPath(BlackboardPath) + TEXT(".hjson")).Replace(TEXT("\\"), TEXT("/"));
		if (!SaveStackJson(RelativeFile, UeAgentAIOps::ExportBlackboard(Blackboard, true), WrittenFiles))
		{
			return false;
		}
	}

	FString StateTreePath;
	Manifest->TryGetStringField(TEXT("state_tree_asset"), StateTreePath);
	if (!StateTreePath.IsEmpty() && bIncludeAssetSummaries)
	{
		UStateTree* Tree = UeAgentAIOps::LoadAsset<UStateTree>(StateTreePath);
		if (!Tree)
		{
			OutError = TEXT("state_tree_not_found");
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> LocalFiles;
		const FString RelativeFolder = FPaths::Combine(TEXT("state_tree"), UeAgentAIOps::AssetFolderNameFromPath(StateTreePath)).Replace(TEXT("\\"), TEXT("/"));
		if (!UeAgentAIOps::SaveStateTreeFolderContents(FPaths::Combine(FolderPath, RelativeFolder), Tree, LocalFiles, OutError))
		{
			return false;
		}
		AppendPrefixedFiles(WrittenFiles, RelativeFolder, LocalFiles);
	}

	TArray<FString> EqsAssets = CollectStringArray(TEXT("eqs_assets"), TEXT("eqs_asset"));
	SetStringArrayField(TEXT("eqs_assets"), EqsAssets);
	for (const FString& EqsPath : EqsAssets)
	{
		if (!bIncludeAssetSummaries)
		{
			continue;
		}
		UEnvQuery* Query = UeAgentAIOps::LoadAsset<UEnvQuery>(EqsPath);
		if (!Query)
		{
			OutError = TEXT("eqs_query_not_found");
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> LocalFiles;
		const FString RelativeFolder = FPaths::Combine(TEXT("eqs"), UeAgentAIOps::AssetFolderNameFromPath(EqsPath)).Replace(TEXT("\\"), TEXT("/"));
		if (!UeAgentAIOps::SaveEqsFolderContents(FPaths::Combine(FolderPath, RelativeFolder), Query, LocalFiles, OutError))
		{
			return false;
		}
		AppendPrefixedFiles(WrittenFiles, RelativeFolder, LocalFiles);
	}

	TArray<FString> SmartObjectDefinitions = CollectStringArray(TEXT("smart_object_definitions"), TEXT("smart_object_definition_asset"));
	SetStringArrayField(TEXT("smart_object_definitions"), SmartObjectDefinitions);
	TArray<TSharedPtr<FJsonValue>> SmartObjectDefinitionValues;
	for (const FString& DefinitionPath : SmartObjectDefinitions)
	{
		SmartObjectDefinitionValues.Add(MakeShared<FJsonValueString>(DefinitionPath));
		if (!bIncludeAssetSummaries)
		{
			continue;
		}
		USmartObjectDefinition* Definition = UeAgentAIOps::LoadAsset<USmartObjectDefinition>(DefinitionPath);
		if (!Definition)
		{
			OutError = TEXT("smart_object_definition_not_found");
			return false;
		}
		const FString RelativeFile = FPaths::Combine(TEXT("smart_objects"), UeAgentAIOps::AssetFolderNameFromPath(DefinitionPath) + TEXT(".hjson")).Replace(TEXT("\\"), TEXT("/"));
		const FString SmartObjectEntryDirectory = FPaths::Combine(FolderPath, TEXT("smart_objects"));
		OutError.Reset();
		TSharedPtr<FJsonObject> DefinitionJson = UeAgentAIOps::ExportSmartObjectDefinition(Definition, true, &SmartObjectEntryDirectory, &OutError);
		if (!OutError.IsEmpty())
		{
			return false;
		}
		if (!SaveStackJson(RelativeFile, DefinitionJson, WrittenFiles))
		{
			return false;
		}
	}

	auto SaveNodeSummaries = [&](const TCHAR* ArrayField, const FString& RelativeFolder)
	{
		TArray<FString> Assets = CollectStringArray(ArrayField, nullptr);
		SetStringArrayField(ArrayField, Assets);
		for (const FString& AssetPath : Assets)
		{
			UObject* Asset = UeAgentAIOps::LoadAssetObject(AssetPath);
			if (!Asset)
			{
				OutError = TEXT("node_blueprint_not_found");
				return false;
			}
			UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
			UClass* GeneratedClass = nullptr;
			if (Blueprint)
			{
				GeneratedClass = Blueprint->GeneratedClass.Get();
			}
			else
			{
				GeneratedClass = Cast<UClass>(Asset);
			}
			TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
			Summary->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
			Summary->SetStringField(TEXT("object_path"), Asset->GetPathName());
			Summary->SetStringField(TEXT("generated_class"), GeneratedClass ? GeneratedClass->GetPathName() : FString());
			Summary->SetStringField(TEXT("parent_class"), (GeneratedClass && GeneratedClass->GetSuperClass()) ? GeneratedClass->GetSuperClass()->GetPathName() : FString());
			const FString RelativeFile = FPaths::Combine(RelativeFolder, UeAgentAIOps::AssetFolderNameFromPath(AssetPath) + TEXT(".summary.hjson")).Replace(TEXT("\\"), TEXT("/"));
			if (!SaveStackJson(RelativeFile, Summary, WrittenFiles))
			{
				return false;
			}
		}
		return true;
	};
	if (!SaveNodeSummaries(TEXT("bt_node_blueprints"), TEXT("bt_node_blueprints")) ||
		!SaveNodeSummaries(TEXT("state_tree_node_blueprints"), TEXT("state_tree_node_blueprints")))
	{
		return false;
	}

	TSharedPtr<FJsonObject> PerceptionSetup = MakeShared<FJsonObject>();
	PerceptionSetup->SetStringField(TEXT("profile"), TEXT("ue_agent_interface.ai_perception_setup.v1"));
	PerceptionSetup->SetNumberField(TEXT("schema_version"), 1);
	PerceptionSetup->SetArrayField(TEXT("stimuli_sources"), TArray<TSharedPtr<FJsonValue>>());
	if (!SaveStackJson(TEXT("perception/perception_setup.hjson"), PerceptionSetup, WrittenFiles))
	{
		return false;
	}
	TSharedPtr<FJsonObject> NavigationValidation = MakeShared<FJsonObject>();
	NavigationValidation->SetStringField(TEXT("profile"), TEXT("ue_agent_interface.navigation_validation.v1"));
	NavigationValidation->SetNumberField(TEXT("schema_version"), 1);
	NavigationValidation->SetArrayField(TEXT("path_probes"), TArray<TSharedPtr<FJsonValue>>());
	if (!SaveStackJson(TEXT("navigation/navigation_validation.hjson"), NavigationValidation, WrittenFiles))
	{
		return false;
	}
	TSharedPtr<FJsonObject> SmartObjectSetup = MakeShared<FJsonObject>();
	SmartObjectSetup->SetStringField(TEXT("profile"), TEXT("ue_agent_interface.smart_object_setup.v1"));
	SmartObjectSetup->SetNumberField(TEXT("schema_version"), 1);
	SmartObjectSetup->SetArrayField(TEXT("definitions"), SmartObjectDefinitionValues);
	SmartObjectSetup->SetArrayField(TEXT("components"), TArray<TSharedPtr<FJsonValue>>());
	if (!SaveStackJson(TEXT("smart_objects/smart_object_setup.hjson"), SmartObjectSetup, WrittenFiles))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!SaveStackJson(TEXT("validation/checks.hjson"), UeAgentAIOps::MakeValidationChecks(Issues), WrittenFiles))
	{
		return false;
	}
	if (!SaveStackJson(TEXT("validation/coverage_report.hjson"), UeAgentAIOps::MakeCoverageReport(), WrittenFiles))
	{
		return false;
	}
	WrittenFiles.Insert(MakeShared<FJsonValueString>(TEXT("manifest.hjson")), 0);
	Manifest->SetArrayField(TEXT("written_files"), WrittenFiles);
	if (!UeAgentAIOps::SaveFolderAuthoringFile(FolderPath, TEXT("manifest.hjson"), Manifest, OutError))
	{
		return false;
	}
	OutData = Manifest;
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentLegacyMechanics::CmdAIBehaviorStackValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput) || FolderInput.IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	const FString FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderInput);
	TSharedPtr<FJsonObject> AuthoringRejection;
	FString AuthoringError;
	if (UeAgentDetailsAuthoring::BuildAuthoringPropertiesFolderRejectionIfPresent(
		FolderPath,
		UeAgentDetailsAuthoring::MakeOwnerInfoForObject(nullptr, TEXT("ai_behavior_stack_folder"), FString(), TEXT("ai_behavior_stack_validate_folder")),
		TEXT("details.index_file"),
		AuthoringRejection,
		AuthoringError))
	{
		OutData = AuthoringRejection;
		OutError = AuthoringError.IsEmpty() ? FString(TEXT("authoring_properties_rejected")) : AuthoringError;
		return false;
	}
	TSharedPtr<FJsonObject> Manifest;
	if (!UeAgentAIOps::LoadFolderAuthoringFile(FolderPath, TEXT("manifest.hjson"), Manifest, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	TArray<TSharedPtr<FJsonValue>> AssetReferenceResults;
	auto CheckAsset = [&](const TCHAR* Field, UClass* ExpectedClass)
	{
		FString Path;
		if (Manifest->TryGetStringField(Field, Path) && !Path.IsEmpty())
		{
			UObject* Asset = UeAgentAIOps::LoadAssetObject(Path);
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("field"), Field);
			Result->SetStringField(TEXT("asset_path"), Path);
			Result->SetStringField(TEXT("expected_class"), ExpectedClass ? ExpectedClass->GetPathName() : FString());
			Result->SetStringField(TEXT("actual_class"), Asset ? Asset->GetClass()->GetPathName() : FString());
			Result->SetBoolField(TEXT("ok"), Asset && (!ExpectedClass || Asset->IsA(ExpectedClass)));
			AssetReferenceResults.Add(MakeShared<FJsonValueObject>(Result));
			if (!Asset || !Asset->IsA(ExpectedClass))
			{
				UeAgentAIOps::AddIssue(Issues, TEXT("asset_missing_or_type_mismatch"), FString::Printf(TEXT("%s is missing or has wrong type."), Field), Field);
			}
		}
	};
	auto CheckAssetArray = [&](const TCHAR* Field, UClass* ExpectedClass)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Manifest->TryGetArrayField(Field, Values) || !Values)
		{
			return;
		}
		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			FString Path;
			if (!(*Values)[Index].IsValid() || !(*Values)[Index]->TryGetString(Path) || Path.IsEmpty())
			{
				UeAgentAIOps::AddIssue(Issues, TEXT("json_type_mismatch"), FString::Printf(TEXT("%s item must be a non-empty string."), Field), FString::Printf(TEXT("%s[%d]"), Field, Index));
				continue;
			}
			UObject* Asset = UeAgentAIOps::LoadAssetObject(Path);
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("field"), FString::Printf(TEXT("%s[%d]"), Field, Index));
			Result->SetStringField(TEXT("asset_path"), Path);
			Result->SetStringField(TEXT("expected_class"), ExpectedClass ? ExpectedClass->GetPathName() : FString());
			Result->SetStringField(TEXT("actual_class"), Asset ? Asset->GetClass()->GetPathName() : FString());
			Result->SetBoolField(TEXT("ok"), Asset && (!ExpectedClass || Asset->IsA(ExpectedClass)));
			AssetReferenceResults.Add(MakeShared<FJsonValueObject>(Result));
			if (!Asset || !Asset->IsA(ExpectedClass))
			{
				UeAgentAIOps::AddIssue(Issues, TEXT("asset_missing_or_type_mismatch"), FString::Printf(TEXT("%s item is missing or has wrong type."), Field), FString::Printf(TEXT("%s[%d]"), Field, Index));
			}
		}
	};
	CheckAsset(TEXT("behavior_tree_asset"), UBehaviorTree::StaticClass());
	CheckAsset(TEXT("blackboard_asset"), UBlackboardData::StaticClass());
	CheckAsset(TEXT("state_tree_asset"), UStateTree::StaticClass());
	CheckAsset(TEXT("eqs_asset"), UEnvQuery::StaticClass());
	CheckAsset(TEXT("smart_object_definition_asset"), USmartObjectDefinition::StaticClass());
	CheckAssetArray(TEXT("eqs_assets"), UEnvQuery::StaticClass());
	CheckAssetArray(TEXT("smart_object_definitions"), USmartObjectDefinition::StaticClass());
	CheckAssetArray(TEXT("bt_node_blueprints"), UObject::StaticClass());
	CheckAssetArray(TEXT("state_tree_node_blueprints"), UObject::StaticClass());
	const TArray<TSharedPtr<FJsonValue>>* WrittenFiles = nullptr;
	if (Manifest->TryGetArrayField(TEXT("written_files"), WrittenFiles) && WrittenFiles)
	{
		for (int32 Index = 0; Index < WrittenFiles->Num(); ++Index)
		{
			FString RelativeFile;
			if (!(*WrittenFiles)[Index].IsValid() || !(*WrittenFiles)[Index]->TryGetString(RelativeFile) || RelativeFile.IsEmpty())
			{
				UeAgentAIOps::AddIssue(Issues, TEXT("json_type_mismatch"), TEXT("written_files item must be a non-empty string."), FString::Printf(TEXT("written_files[%d]"), Index));
				continue;
			}
			if (!FPaths::FileExists(FPaths::Combine(FolderPath, RelativeFile)))
			{
				UeAgentAIOps::AddIssue(Issues, TEXT("exported_file_missing"), FString::Printf(TEXT("Exported stack file is missing: %s"), *RelativeFile), FString::Printf(TEXT("written_files[%d]"), Index));
			}
		}
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("profile"), UeAgentAIOps::AIStackProfile);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("validate_failed"));
	OutData->SetArrayField(TEXT("asset_reference_results"), AssetReferenceResults);
	UeAgentAIOps::SetDiagnostics(OutData, Issues);
	return Issues.Num() == 0;
}

bool FUeAgentLegacyMechanics::CmdAIBehaviorStackRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	OutData = MakeShared<FJsonObject>();
	double DurationSeconds = 0.0;
	double SampleIntervalSeconds = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("duration_seconds"), DurationSeconds);
	JsonTryGetNumber(Ctx.Params, TEXT("sample_interval_seconds"), SampleIntervalSeconds);
	TSharedPtr<FJsonObject> NavData;
	FString LocalError;
	if (CmdNavigationRuntimeSnapshot(Ctx, NavData, LocalError))
	{
		OutData->SetObjectField(TEXT("navigation"), NavData);
	}
	else
	{
		OutData->SetStringField(TEXT("navigation_error"), LocalError);
	}
	TSharedPtr<FJsonObject> SmartData;
	LocalError.Reset();
	if (CmdSmartObjectRuntimeSnapshot(Ctx, SmartData, LocalError))
	{
		OutData->SetObjectField(TEXT("smart_objects"), SmartData);
	}
	else
	{
		OutData->SetStringField(TEXT("smart_object_error"), LocalError);
	}
	TSharedPtr<FJsonObject> BTData;
	LocalError.Reset();
	if (CmdBehaviorTreeRuntimeSnapshot(Ctx, BTData, LocalError))
	{
		OutData->SetObjectField(TEXT("behavior_tree"), BTData);
	}
	else
	{
		OutData->SetStringField(TEXT("behavior_tree_error"), LocalError);
	}
	TSharedPtr<FJsonObject> StateTreeData;
	LocalError.Reset();
	if (CmdStateTreeRuntimeSnapshot(Ctx, StateTreeData, LocalError))
	{
		OutData->SetObjectField(TEXT("state_tree"), StateTreeData);
	}
	else
	{
		OutData->SetStringField(TEXT("state_tree_error"), LocalError);
	}
	TSharedPtr<FJsonObject> PerceptionData;
	LocalError.Reset();
	if (CmdAIPerceptionRuntimeSnapshot(Ctx, PerceptionData, LocalError))
	{
		OutData->SetObjectField(TEXT("perception"), PerceptionData);
	}
	else
	{
		OutData->SetStringField(TEXT("perception_error"), LocalError);
	}
	TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
	if (NavData.IsValid())
	{
		Sample->SetObjectField(TEXT("navigation"), NavData);
	}
	if (SmartData.IsValid())
	{
		Sample->SetObjectField(TEXT("smart_objects"), SmartData);
	}
	if (BTData.IsValid())
	{
		Sample->SetObjectField(TEXT("behavior_tree"), BTData);
	}
	if (StateTreeData.IsValid())
	{
		Sample->SetObjectField(TEXT("state_tree"), StateTreeData);
	}
	if (PerceptionData.IsValid())
	{
		Sample->SetObjectField(TEXT("perception"), PerceptionData);
	}
	TArray<TSharedPtr<FJsonValue>> Samples;
	Samples.Add(MakeShared<FJsonValueObject>(Sample));
	TArray<TSharedPtr<FJsonValue>> ExpectationResults;
	auto AddExpectation = [&ExpectationResults](const FString& Kind, const FString& Expected, const bool bPassed, const FString& Observed)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("kind"), Kind);
		Result->SetStringField(TEXT("expected"), Expected);
		Result->SetBoolField(TEXT("passed"), bPassed);
		Result->SetStringField(TEXT("observed"), Observed);
		ExpectationResults.Add(MakeShared<FJsonValueObject>(Result));
	};
	const TArray<TSharedPtr<FJsonValue>>* ExpectedBlackboardKeys = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("expected_blackboard_keys"), ExpectedBlackboardKeys) && ExpectedBlackboardKeys)
	{
		TSet<FString> ObservedKeys;
		const TArray<TSharedPtr<FJsonValue>>* BlackboardValues = nullptr;
		if (BTData.IsValid() && BTData->TryGetArrayField(TEXT("blackboard"), BlackboardValues) && BlackboardValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *BlackboardValues)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (Value.IsValid() && Value->TryGetObject(Obj) && Obj && Obj->IsValid())
				{
					ObservedKeys.Add((*Obj)->GetStringField(TEXT("name")));
				}
			}
		}
		for (const TSharedPtr<FJsonValue>& ExpectedValue : *ExpectedBlackboardKeys)
		{
			FString ExpectedName;
			if (ExpectedValue.IsValid() && ExpectedValue->TryGetString(ExpectedName))
			{
				AddExpectation(TEXT("blackboard_key"), ExpectedName, ObservedKeys.Contains(ExpectedName), FString::Join(ObservedKeys.Array(), TEXT(",")));
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* ExpectedPerception = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("expected_perception"), ExpectedPerception) && ExpectedPerception)
	{
		TSet<FString> CurrentActors;
		const TArray<TSharedPtr<FJsonValue>>* CurrentValues = nullptr;
		if (PerceptionData.IsValid() && PerceptionData->TryGetArrayField(TEXT("currently_perceived_actors"), CurrentValues) && CurrentValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *CurrentValues)
			{
				FString Name;
				if (Value.IsValid() && Value->TryGetString(Name))
				{
					CurrentActors.Add(Name);
				}
			}
		}
		for (const TSharedPtr<FJsonValue>& ExpectedValue : *ExpectedPerception)
		{
			FString ExpectedActor;
			if (ExpectedValue.IsValid() && ExpectedValue->TryGetString(ExpectedActor))
			{
				AddExpectation(TEXT("perception_actor"), ExpectedActor, CurrentActors.Contains(ExpectedActor), FString::Join(CurrentActors.Array(), TEXT(",")));
			}
		}
	}
	bool bExpectationsPassed = true;
	for (const TSharedPtr<FJsonValue>& ResultValue : ExpectationResults)
	{
		const TSharedPtr<FJsonObject>* ResultObj = nullptr;
		if (ResultValue.IsValid() && ResultValue->TryGetObject(ResultObj) && ResultObj && ResultObj->IsValid())
		{
			bool bPassed = false;
			(*ResultObj)->TryGetBoolField(TEXT("passed"), bPassed);
			bExpectationsPassed = bExpectationsPassed && bPassed;
		}
	}
	OutData->SetNumberField(TEXT("duration_seconds"), DurationSeconds);
	OutData->SetNumberField(TEXT("sample_interval_seconds"), SampleIntervalSeconds);
	OutData->SetNumberField(TEXT("sample_count"), Samples.Num());
	OutData->SetArrayField(TEXT("samples"), Samples);
	OutData->SetArrayField(TEXT("expectation_results"), ExpectationResults);
	OutData->SetArrayField(TEXT("cleanup_results"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetBoolField(TEXT("probe_passed"), bExpectationsPassed);
	OutData->SetStringField(TEXT("profile"), UeAgentAIOps::AIStackProfile);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}
