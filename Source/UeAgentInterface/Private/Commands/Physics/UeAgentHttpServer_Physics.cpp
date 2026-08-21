// Copyright Epic Games, Inc. All Rights Reserved.

#include "LegacyMechanics/UeAgentLegacyMechanics.h"

#include "Commands/Assets/UeAgentReflectionAuthoringCommandBridge.h"
#include "Utilities/UeAgentAuthoringFileIO.h"
#include "Utilities/UeAgentAuthoringFolderIO.h"
#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentFolderDetailsExport.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/UeAgentStructuredAuthoringIO.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "BodySetupEnums.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/CollisionProfile.h"
#include "Engine/EngineTypes.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Misc/EngineVersion.h"
#include "GameFramework/Actor.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintActor.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UeAgentPhysicsOps
{
	static constexpr const TCHAR* PhysicalMaterialSchema = TEXT("ue_agent_interface.physical_material.v1");
	static constexpr const TCHAR* PhysicsConstraintSchema = TEXT("ue_agent_interface.physics_constraint.v1");
	static constexpr const TCHAR* PhysicsAssetSchema = TEXT("ue_agent_interface.physics_asset.v1");
	static constexpr const TCHAR* PhysicsComponentSchema = TEXT("ue_agent_interface.physics_component.v1");
	static constexpr const TCHAR* PhysicsProbeSchema = TEXT("ue_agent_interface.physics_runtime_probe.v1");

	static FString NormalizeAssetPath(const FString& InPath)
	{
		FString OutPath = InPath;
		OutPath.TrimStartAndEndInline();
		OutPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		int32 DotIndex = INDEX_NONE;
		if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			OutPath = OutPath.Left(DotIndex);
		}
		return OutPath;
	}

	static FString ToObjectPath(const FString& InPath)
	{
		FString ObjectPath = InPath;
		ObjectPath.TrimStartAndEndInline();
		ObjectPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (ObjectPath.IsEmpty())
		{
			return FString();
		}
		if (ObjectPath.Contains(TEXT(".")))
		{
			return ObjectPath;
		}
		const FString AssetPath = NormalizeAssetPath(ObjectPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		return AssetName.IsEmpty() ? FString() : FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	static UObject* LoadAssetObject(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		const FString PackagePath = NormalizeAssetPath(InPath);
		if (!FPackageName::DoesPackageExist(PackagePath))
		{
			return nullptr;
		}
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	template<typename TObjectType>
	static TObjectType* LoadAsset(const FString& InPath)
	{
		return Cast<TObjectType>(LoadAssetObject(InPath));
	}

	static FString AssetPathForObject(UObject* Object)
	{
		return Object && Object->GetOutermost() ? Object->GetOutermost()->GetName() : FString();
	}

	static bool SaveAssetPackage(UObject* Asset, FString& OutError)
	{
		if (!Asset || !Asset->GetOutermost())
		{
			OutError = TEXT("asset_not_found");
			return false;
		}
		TArray<UPackage*> Packages;
		Packages.Add(Asset->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(Packages, false))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
		return true;
	}

	static bool BackupAssetObject(UObject* SourceAsset, FString& OutBackupAssetPath, FString& OutBackupObjectPath, FString& OutError)
	{
		OutBackupAssetPath.Reset();
		OutBackupObjectPath.Reset();
		if (!SourceAsset || !SourceAsset->GetOutermost())
		{
			OutError = TEXT("asset_not_found");
			return false;
		}
		const FString BackupAssetName = FString::Printf(TEXT("%s_Backup_%s"), *SourceAsset->GetName(), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		const FString BackupPackageName = FString::Printf(TEXT("/Game/__UeAgentInterfaceBackups/%s"), *BackupAssetName);
		UPackage* BackupPackage = CreatePackage(*BackupPackageName);
		if (!BackupPackage)
		{
			OutError = TEXT("backup_create_package_failed");
			return false;
		}
		UObject* BackupObject = StaticDuplicateObject(SourceAsset, BackupPackage, *BackupAssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!BackupObject)
		{
			OutError = TEXT("backup_duplicate_failed");
			return false;
		}
		FAssetRegistryModule::AssetCreated(BackupObject);
		BackupObject->MarkPackageDirty();
		BackupPackage->MarkPackageDirty();
		OutBackupAssetPath = BackupPackageName;
		OutBackupObjectPath = BackupObject->GetPathName();
		return true;
	}

	static bool SaveAuthoringObjectToFile(const FString& OutputFile, const TSharedPtr<FJsonObject>& Object, FString& OutResolvedPath, FString& OutError)
	{
		UeAgentAuthoringFileIO::FSaveOptions Options;
		Options.SourceCommand = TEXT("physics_json_export");
		return UeAgentAuthoringFileIO::SaveObject(OutputFile, Object, Options, OutResolvedPath, OutError);
	}

	static bool LoadAuthoringObjectFromFile(const FString& InputFile, TSharedPtr<FJsonObject>& OutJson, FString& OutResolvedPath, FString& OutError)
	{
		UeAgentAuthoringFileIO::FLoadOptions Options;
		Options.bRehydrateGroupedDetails = true;
		return UeAgentAuthoringFileIO::LoadObject(InputFile, OutJson, Options, OutResolvedPath, OutError);
	}

	static bool GetInputJsonObject(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutJson, FString& OutSource, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* InlineObject = nullptr;
		if (Ctx.Params.IsValid() && Ctx.Params->TryGetObjectField(TEXT("json"), InlineObject) && InlineObject && InlineObject->IsValid())
		{
			OutJson = *InlineObject;
			OutSource = TEXT("inline_json");
			return true;
		}
		FString JsonFile;
		if (Ctx.Params.IsValid() && Ctx.Params->TryGetStringField(TEXT("json_file"), JsonFile) && !JsonFile.TrimStartAndEnd().IsEmpty())
		{
			return LoadAuthoringObjectFromFile(JsonFile, OutJson, OutSource, OutError);
		}
		OutError = TEXT("missing_json_or_json_file");
		return false;
	}

	static FString HashJsonObject(const TSharedPtr<FJsonObject>& Object)
	{
		FString JsonText;
		if (Object.IsValid())
		{
			UeAgentJsonDiagnostics::SerializeJsonObjectCompact(Object, JsonText);
		}
		return FString::Printf(TEXT("md5:%s"), *FMD5::HashAnsiString(*JsonText));
	}

	static FString CleanEnumName(FString Value)
	{
		int32 ScopeIndex = INDEX_NONE;
		if (Value.FindLastChar(TEXT(':'), ScopeIndex))
		{
			Value = Value.Mid(ScopeIndex + 1);
		}
		return Value;
	}

	template<typename TEnum>
	static FString EnumToString(TEnum Value)
	{
		if (const UEnum* Enum = StaticEnum<TEnum>())
		{
			return CleanEnumName(Enum->GetNameStringByValue(static_cast<int64>(Value)));
		}
		return FString::FromInt(static_cast<int32>(Value));
	}

	static FString LinearMotionToString(ELinearConstraintMotion Motion)
	{
		switch (Motion)
		{
		case LCM_Free: return TEXT("Free");
		case LCM_Limited: return TEXT("Limited");
		case LCM_Locked: return TEXT("Locked");
		default: return TEXT("Unknown");
		}
	}

	static FString AngularMotionToString(EAngularConstraintMotion Motion)
	{
		switch (Motion)
		{
		case ACM_Free: return TEXT("Free");
		case ACM_Limited: return TEXT("Limited");
		case ACM_Locked: return TEXT("Locked");
		default: return TEXT("Unknown");
		}
	}

	static bool ParseLinearMotion(const FString& Input, ELinearConstraintMotion& OutMotion)
	{
		if (Input.Equals(TEXT("Free"), ESearchCase::IgnoreCase) || Input.Equals(TEXT("LCM_Free"), ESearchCase::IgnoreCase)) { OutMotion = LCM_Free; return true; }
		if (Input.Equals(TEXT("Limited"), ESearchCase::IgnoreCase) || Input.Equals(TEXT("LCM_Limited"), ESearchCase::IgnoreCase)) { OutMotion = LCM_Limited; return true; }
		if (Input.Equals(TEXT("Locked"), ESearchCase::IgnoreCase) || Input.Equals(TEXT("LCM_Locked"), ESearchCase::IgnoreCase)) { OutMotion = LCM_Locked; return true; }
		return false;
	}

	static bool ParseAngularMotion(const FString& Input, EAngularConstraintMotion& OutMotion)
	{
		if (Input.Equals(TEXT("Free"), ESearchCase::IgnoreCase) || Input.Equals(TEXT("ACM_Free"), ESearchCase::IgnoreCase)) { OutMotion = ACM_Free; return true; }
		if (Input.Equals(TEXT("Limited"), ESearchCase::IgnoreCase) || Input.Equals(TEXT("ACM_Limited"), ESearchCase::IgnoreCase)) { OutMotion = ACM_Limited; return true; }
		if (Input.Equals(TEXT("Locked"), ESearchCase::IgnoreCase) || Input.Equals(TEXT("ACM_Locked"), ESearchCase::IgnoreCase)) { OutMotion = ACM_Locked; return true; }
		return false;
	}

	static FString CombineModeToString(EFrictionCombineMode::Type Mode)
	{
		switch (Mode)
		{
		case EFrictionCombineMode::Average: return TEXT("Average");
		case EFrictionCombineMode::Min: return TEXT("Min");
		case EFrictionCombineMode::Multiply: return TEXT("Multiply");
		case EFrictionCombineMode::Max: return TEXT("Max");
		default: return TEXT("Unknown");
		}
	}

	static bool ParseCombineMode(const FString& Input, EFrictionCombineMode::Type& OutMode)
	{
		if (Input.Equals(TEXT("Average"), ESearchCase::IgnoreCase)) { OutMode = EFrictionCombineMode::Average; return true; }
		if (Input.Equals(TEXT("Min"), ESearchCase::IgnoreCase)) { OutMode = EFrictionCombineMode::Min; return true; }
		if (Input.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase)) { OutMode = EFrictionCombineMode::Multiply; return true; }
		if (Input.Equals(TEXT("Max"), ESearchCase::IgnoreCase)) { OutMode = EFrictionCombineMode::Max; return true; }
		return false;
	}

	static FString SurfaceTypeToString(EPhysicalSurface SurfaceType)
	{
		if (const UEnum* Enum = StaticEnum<EPhysicalSurface>())
		{
			return Enum->GetNameStringByValue(static_cast<int64>(SurfaceType));
		}
		return FString::Printf(TEXT("SurfaceType%d"), static_cast<int32>(SurfaceType));
	}

	static bool ParseSurfaceType(const FString& Input, EPhysicalSurface& OutSurface)
	{
		if (Input.IsEmpty())
		{
			OutSurface = SurfaceType_Default;
			return true;
		}
		if (const UEnum* Enum = StaticEnum<EPhysicalSurface>())
		{
			int64 Value = Enum->GetValueByNameString(Input);
			if (Value == INDEX_NONE)
			{
				Value = Enum->GetValueByNameString(TEXT("EPhysicalSurface::") + Input);
			}
			if (Value != INDEX_NONE)
			{
				OutSurface = static_cast<EPhysicalSurface>(Value);
				return true;
			}
		}
		return false;
	}

	static TSharedPtr<FJsonObject> VecToJson(const FVector& V)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), V.X);
		Obj->SetNumberField(TEXT("y"), V.Y);
		Obj->SetNumberField(TEXT("z"), V.Z);
		return Obj;
	}

	static bool JsonArrayToVector(const TArray<TSharedPtr<FJsonValue>>* Array, FVector& OutVector)
	{
		if (!Array || Array->Num() != 3)
		{
			return false;
		}
		OutVector = FVector((*Array)[0]->AsNumber(), (*Array)[1]->AsNumber(), (*Array)[2]->AsNumber());
		return true;
	}

	static TSharedPtr<FJsonObject> BoxToJson(const FBox& Box)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("min"), VecToJson(Box.Min));
		Obj->SetObjectField(TEXT("max"), VecToJson(Box.Max));
		Obj->SetObjectField(TEXT("center"), VecToJson(Box.GetCenter()));
		Obj->SetObjectField(TEXT("extent"), VecToJson(Box.GetExtent()));
		return Obj;
	}

	static FString ActorIdentifier(AActor* Actor)
	{
		if (!Actor)
		{
			return FString();
		}
#if WITH_EDITOR
		const FString Label = Actor->GetActorLabel();
		if (!Label.IsEmpty())
		{
			return Label;
		}
#endif
		return Actor->GetName();
	}

	static AActor* FindActorInWorld(UWorld* World, const FString& NameOrLabel)
	{
		if (!World)
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			if (Actor->GetName().Equals(NameOrLabel, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
#if WITH_EDITOR
			if (Actor->GetActorLabel().Equals(NameOrLabel, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
#endif
		}
		return nullptr;
	}

	static UPrimitiveComponent* FindPrimitiveComponent(AActor* Actor, const FString& ComponentName)
	{
		if (!Actor)
		{
			return nullptr;
		}
		TArray<UPrimitiveComponent*> Components;
		Actor->GetComponents<UPrimitiveComponent>(Components);
		if (Components.IsEmpty())
		{
			return nullptr;
		}
		FString Name = ComponentName;
		Name.TrimStartAndEndInline();
		if (Name.IsEmpty())
		{
			if (UPrimitiveComponent* RootPrimitive = Cast<UPrimitiveComponent>(Actor->GetRootComponent()))
			{
				return RootPrimitive;
			}
			return Components[0];
		}
		for (UPrimitiveComponent* Component : Components)
		{
			if (Component && (Component->GetName().Equals(Name, ESearchCase::IgnoreCase) || Component->GetClass()->GetName().Equals(Name, ESearchCase::IgnoreCase)))
			{
				return Component;
			}
		}
		return nullptr;
	}

	static void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Severity, const FString& Code, const FString& Message, const FString& Path = FString())
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), Severity);
		Issue->SetStringField(TEXT("code"), Code);
		Issue->SetStringField(TEXT("message"), Message);
		if (!Path.IsEmpty())
		{
			Issue->SetStringField(TEXT("path"), Path);
		}
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	}

	static bool HasErrorIssue(const TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		for (const TSharedPtr<FJsonValue>& IssueValue : Issues)
		{
			const TSharedPtr<FJsonObject>* IssueObj = nullptr;
			if (IssueValue.IsValid() && IssueValue->TryGetObject(IssueObj) && IssueObj && IssueObj->IsValid())
			{
				FString Severity;
				if ((*IssueObj)->TryGetStringField(TEXT("severity"), Severity) && Severity == TEXT("error"))
				{
					return true;
				}
			}
		}
		return false;
	}
}

bool FUeAgentLegacyMechanics::ExecutePhysicsCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("physics_capabilities_query")) return CmdPhysicsCapabilitiesQuery(Ctx, OutData, OutError);
	if (CommandLower == TEXT("physics_component_query")) return CmdPhysicsComponentQuery(Ctx, OutData, OutError);
	if (CommandLower == TEXT("physics_component_validate")) return CmdPhysicsComponentValidate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("physics_component_plan_patch")) return CmdPhysicsComponentPlanPatch(Ctx, OutData, OutError);
	if (CommandLower == TEXT("physics_runtime_probe")) return CmdPhysicsRuntimeProbe(Ctx, OutData, OutError);
	if (CommandLower == TEXT("physical_material_export_json")) return CmdPhysicalMaterialExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("physical_material_validate_json")) return CmdPhysicalMaterialValidateJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("physical_material_apply_json")) return CmdPhysicalMaterialApplyJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("physics_constraint_export_json")) return CmdPhysicsConstraintExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("physics_constraint_validate_json")) return CmdPhysicsConstraintValidateJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("physics_constraint_apply_json")) return CmdPhysicsConstraintApplyJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("physics_asset_export_folder")) return UeAgentReflectionAuthoringCommandBridge::Export(TEXT("physics_asset"), Ctx.Params, OutData, OutError);
	if (CommandLower == TEXT("physics_asset_validate_folder")) return UeAgentReflectionAuthoringCommandBridge::Validate(TEXT("physics_asset"), Ctx.Params, OutData, OutError);
	if (CommandLower == TEXT("physics_asset_apply_folder")) return UeAgentReflectionAuthoringCommandBridge::Apply(TEXT("physics_asset"), Ctx.Params, OutData, OutError, Ctx.RequestId);

	OutError = TEXT("unknown_physics_command");
	return false;
}

bool FUeAgentLegacyMechanics::CmdPhysicsCapabilitiesQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	bool bIncludeProjectSettings = true;
	bool bIncludeCollisionProfiles = true;
	bool bIncludeOptionalPluginStatus = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_project_settings"), bIncludeProjectSettings);
	JsonTryGetBool(Ctx.Params, TEXT("include_collision_profiles"), bIncludeCollisionProfiles);
	JsonTryGetBool(Ctx.Params, TEXT("include_optional_plugin_status"), bIncludeOptionalPluginStatus);

	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.physics_capabilities.v1"));
	OutData->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
	OutData->SetBoolField(TEXT("baseline_physics_available"), true);

	TArray<TSharedPtr<FJsonValue>> Classes;
	for (const TCHAR* ClassName : { TEXT("/Script/Engine.PrimitiveComponent"), TEXT("/Script/PhysicsCore.PhysicalMaterial"), TEXT("/Script/Engine.PhysicsConstraintComponent"), TEXT("/Script/Engine.PhysicsConstraintActor"), TEXT("/Script/Engine.PhysicsAsset") })
	{
		Classes.Add(MakeShared<FJsonValueString>(ClassName));
	}
	OutData->SetArrayField(TEXT("available_classes"), Classes);

	if (bIncludeCollisionProfiles)
	{
		TArray<TSharedPtr<FJsonValue>> Profiles;
		TArray<TSharedPtr<FName>> ProfileNames;
		UCollisionProfile::Get()->GetProfileNames(ProfileNames);
		for (const TSharedPtr<FName>& ProfileName : ProfileNames)
		{
			if (ProfileName.IsValid())
			{
				Profiles.Add(MakeShared<FJsonValueString>(ProfileName->ToString()));
			}
		}
		OutData->SetArrayField(TEXT("collision_profiles"), Profiles);

		TArray<TSharedPtr<FJsonValue>> Channels;
		for (int32 ChannelIndex = 0; ChannelIndex < ECollisionChannel::ECC_MAX; ++ChannelIndex)
		{
			Channels.Add(MakeShared<FJsonValueString>(UeAgentPhysicsOps::EnumToString(static_cast<ECollisionChannel>(ChannelIndex))));
		}
		OutData->SetArrayField(TEXT("collision_channels"), Channels);
	}

	if (bIncludeProjectSettings)
	{
		TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
		const UPhysicsSettings* SettingsObj = UPhysicsSettings::Get();
		if (SettingsObj)
		{
			Settings->SetStringField(TEXT("class"), SettingsObj->GetClass()->GetPathName());
			Settings->SetNumberField(TEXT("max_physics_delta_time"), SettingsObj->MaxPhysicsDeltaTime);
		}
		OutData->SetObjectField(TEXT("physics_settings_summary"), Settings);
	}

	if (bIncludeOptionalPluginStatus)
	{
		TArray<TSharedPtr<FJsonValue>> Excluded;
		for (const TCHAR* PluginName : { TEXT("ChaosVehiclesPlugin"), TEXT("ChaosModularVehicle"), TEXT("GeometryCollectionPlugin"), TEXT("FieldSystemPlugin"), TEXT("ChaosCaching"), TEXT("ChaosFlesh"), TEXT("ChaosClothAsset"), TEXT("HairStrands"), TEXT("PhysicsControl"), TEXT("ChaosVD") })
		{
			TSharedPtr<FJsonObject> Plugin = MakeShared<FJsonObject>();
			Plugin->SetStringField(TEXT("plugin"), PluginName);
			Plugin->SetStringField(TEXT("reason"), TEXT("optional_plugin_not_part_of_baseline_physics"));
			Excluded.Add(MakeShared<FJsonValueObject>(Plugin));
		}
		OutData->SetArrayField(TEXT("excluded_optional_plugins"), Excluded);
	}

	OutData->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
	return true;
}

static TSharedPtr<FJsonObject> UeAgentPhysicsPrimitiveToJson(UPrimitiveComponent* Component, bool bIncludeResponses, bool bIncludeMaterial, bool bIncludeRuntime)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("schema"), UeAgentPhysicsOps::PhysicsComponentSchema);
	Obj->SetBoolField(TEXT("is_primitive_component"), Component != nullptr);
	if (!Component)
	{
		return Obj;
	}

	AActor* Owner = Component->GetOwner();
	Obj->SetStringField(TEXT("actor_id"), UeAgentPhysicsOps::ActorIdentifier(Owner));
	Obj->SetStringField(TEXT("actor_name"), Owner ? Owner->GetName() : FString());
	Obj->SetStringField(TEXT("component"), Component->GetName());
	Obj->SetStringField(TEXT("component_class"), Component->GetClass()->GetPathName());
	Obj->SetStringField(TEXT("component_path"), Component->GetPathName());
	Obj->SetStringField(TEXT("mobility"), UeAgentPhysicsOps::EnumToString(Component->Mobility.GetValue()));
	Obj->SetBoolField(TEXT("simulate_physics"), Component->IsSimulatingPhysics());
	Obj->SetStringField(TEXT("collision_enabled"), UeAgentPhysicsOps::EnumToString(Component->GetCollisionEnabled()));
	Obj->SetStringField(TEXT("collision_profile_name"), Component->GetCollisionProfileName().ToString());
	Obj->SetStringField(TEXT("object_type"), UeAgentPhysicsOps::EnumToString(Component->GetCollisionObjectType()));
	double MassKg = 0.0;
	if (Component->IsSimulatingPhysics())
	{
		MassKg = Component->GetMass();
	}
	else if (UBodySetup* BodySetup = Component->GetBodySetup())
	{
		MassKg = BodySetup->CalculateMass(Component);
	}
	Obj->SetNumberField(TEXT("mass_kg"), MassKg);
	Obj->SetNumberField(TEXT("linear_damping"), Component->GetLinearDamping());
	Obj->SetNumberField(TEXT("angular_damping"), Component->GetAngularDamping());
	Obj->SetBoolField(TEXT("enable_gravity"), Component->IsGravityEnabled());
	Obj->SetObjectField(TEXT("bounds"), UeAgentPhysicsOps::BoxToJson(Component->Bounds.GetBox()));

	if (FBodyInstance* Body = Component->GetBodyInstance())
	{
		Obj->SetBoolField(TEXT("mass_override"), Body->bOverrideMass);
		Obj->SetNumberField(TEXT("mass_override_kg"), Body->GetMassOverride());
		Obj->SetBoolField(TEXT("use_ccd"), Body->bUseCCD);
		if (bIncludeRuntime)
		{
			Obj->SetBoolField(TEXT("awake"), Body->IsInstanceAwake());
			Obj->SetBoolField(TEXT("sleeping"), !Body->IsInstanceAwake());
		}
	}

	if (bIncludeResponses)
	{
		TSharedPtr<FJsonObject> Responses = MakeShared<FJsonObject>();
		for (int32 ChannelIndex = 0; ChannelIndex < ECollisionChannel::ECC_MAX; ++ChannelIndex)
		{
			const ECollisionChannel Channel = static_cast<ECollisionChannel>(ChannelIndex);
			Responses->SetStringField(UeAgentPhysicsOps::EnumToString(Channel), UeAgentPhysicsOps::EnumToString(Component->GetCollisionResponseToChannel(Channel)));
		}
		Obj->SetObjectField(TEXT("responses"), Responses);
	}

	if (bIncludeMaterial)
	{
		TSharedPtr<FJsonObject> Mat = MakeShared<FJsonObject>();
		UPhysicalMaterial* PhysMat = Component->GetBodyInstance() ? Component->GetBodyInstance()->GetSimplePhysicalMaterial() : nullptr;
		Mat->SetStringField(TEXT("asset_path"), PhysMat ? UeAgentPhysicsOps::AssetPathForObject(PhysMat) : FString());
		Mat->SetStringField(TEXT("object_path"), PhysMat ? PhysMat->GetPathName() : FString());
		Obj->SetObjectField(TEXT("physical_material"), Mat);
	}

	TSharedPtr<FJsonObject> PatchHint = MakeShared<FJsonObject>();
	PatchHint->SetStringField(TEXT("command"), TEXT("level_content_apply_json"));
	PatchHint->SetStringField(TEXT("component_property_prefix"), Component->GetName() + TEXT("."));
	Obj->SetObjectField(TEXT("level_content_patch_hint"), PatchHint);
	return Obj;
}

bool FUeAgentLegacyMechanics::CmdPhysicsComponentQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString ActorId;
	if (!JsonTryGetString(Ctx.Params, TEXT("actor_id"), ActorId) || ActorId.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_actor_id");
		return false;
	}
	FString ComponentName;
	JsonTryGetString(Ctx.Params, TEXT("component"), ComponentName);
	bool bIncludeResponses = true;
	bool bIncludeMaterial = true;
	bool bIncludeRuntime = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_collision_responses"), bIncludeResponses);
	JsonTryGetBool(Ctx.Params, TEXT("include_material"), bIncludeMaterial);
	JsonTryGetBool(Ctx.Params, TEXT("include_runtime_state"), bIncludeRuntime);

	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	AActor* Actor = FindActorByNameOrLabel(World, ActorId);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}
	UPrimitiveComponent* Component = UeAgentPhysicsOps::FindPrimitiveComponent(Actor, ComponentName);
	if (!Component)
	{
		OutError = TEXT("physics_not_primitive_component");
		return false;
	}
	OutData = UeAgentPhysicsPrimitiveToJson(Component, bIncludeResponses, bIncludeMaterial, bIncludeRuntime);
	return true;
}

bool FUeAgentLegacyMechanics::CmdPhysicsComponentValidate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> QueryData = MakeShared<FJsonObject>();
	if (!CmdPhysicsComponentQuery(Ctx, QueryData, OutError))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Expected = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetObjectField(TEXT("expected"), Expected) || !Expected || !Expected->IsValid())
	{
		OutError = TEXT("missing_expected");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> Mismatches;
	auto AddCheck = [&Checks, &Mismatches](const FString& Name, bool bPassed, const FString& Actual, const FString& ExpectedValue)
	{
		TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
		Check->SetStringField(TEXT("name"), Name);
		Check->SetBoolField(TEXT("passed"), bPassed);
		Check->SetStringField(TEXT("actual"), Actual);
		Check->SetStringField(TEXT("expected"), ExpectedValue);
		Checks.Add(MakeShared<FJsonValueObject>(Check));
		if (!bPassed)
		{
			Mismatches.Add(MakeShared<FJsonValueObject>(Check));
		}
	};

	bool BoolExpected = false;
	if ((*Expected)->TryGetBoolField(TEXT("simulate_physics"), BoolExpected))
	{
		const bool Actual = QueryData->GetBoolField(TEXT("simulate_physics"));
		AddCheck(TEXT("simulate_physics"), Actual == BoolExpected, Actual ? TEXT("true") : TEXT("false"), BoolExpected ? TEXT("true") : TEXT("false"));
	}
	if ((*Expected)->TryGetBoolField(TEXT("enable_gravity"), BoolExpected))
	{
		const bool Actual = QueryData->GetBoolField(TEXT("enable_gravity"));
		AddCheck(TEXT("enable_gravity"), Actual == BoolExpected, Actual ? TEXT("true") : TEXT("false"), BoolExpected ? TEXT("true") : TEXT("false"));
	}
	FString StringExpected;
	if ((*Expected)->TryGetStringField(TEXT("collision_enabled"), StringExpected))
	{
		const FString Actual = QueryData->GetStringField(TEXT("collision_enabled"));
		AddCheck(TEXT("collision_enabled"), Actual.Equals(StringExpected, ESearchCase::IgnoreCase), Actual, StringExpected);
	}
	if ((*Expected)->TryGetStringField(TEXT("collision_profile_name"), StringExpected))
	{
		const FString Actual = QueryData->GetStringField(TEXT("collision_profile_name"));
		AddCheck(TEXT("collision_profile_name"), Actual.Equals(StringExpected, ESearchCase::IgnoreCase), Actual, StringExpected);
	}
	double NumberExpected = 0.0;
	if ((*Expected)->TryGetNumberField(TEXT("min_mass_kg"), NumberExpected))
	{
		const double Actual = QueryData->GetNumberField(TEXT("mass_kg"));
		AddCheck(TEXT("min_mass_kg"), Actual >= NumberExpected, FString::SanitizeFloat(Actual), FString::SanitizeFloat(NumberExpected));
	}

	OutData->SetStringField(TEXT("schema"), UeAgentPhysicsOps::PhysicsComponentSchema);
	OutData->SetBoolField(TEXT("ok"), Mismatches.Num() == 0);
	OutData->SetArrayField(TEXT("checks"), Checks);
	OutData->SetArrayField(TEXT("mismatches"), Mismatches);
	OutData->SetObjectField(TEXT("query"), QueryData);
	if (Mismatches.Num() > 0)
	{
		OutError = TEXT("physics_component_validation_failed");
		return false;
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPhysicsComponentPlanPatch(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString ActorId;
	if (!JsonTryGetString(Ctx.Params, TEXT("actor_id"), ActorId) || ActorId.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_actor_id");
		return false;
	}
	FString ComponentName;
	JsonTryGetString(Ctx.Params, TEXT("component"), ComponentName);
	if (ComponentName.TrimStartAndEnd().IsEmpty())
	{
		ComponentName = TEXT("StaticMeshComponent");
	}
	const TSharedPtr<FJsonObject>* Desired = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetObjectField(TEXT("desired"), Desired) || !Desired || !Desired->IsValid())
	{
		OutError = TEXT("missing_desired");
		return false;
	}

	FString ActorClassPath;
	UWorld* World = GetEditorWorld(OutError);
	if (World)
	{
		if (AActor* Actor = UeAgentPhysicsOps::FindActorInWorld(World, ActorId))
		{
			if (Actor->GetClass())
			{
				ActorClassPath = Actor->GetClass()->GetPathName();
			}
			if (UPrimitiveComponent* Component = UeAgentPhysicsOps::FindPrimitiveComponent(Actor, ComponentName))
			{
				ComponentName = Component->GetName();
			}
		}
	}
	OutError.Reset();

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	auto SetBool = [&Properties, &ComponentName, Desired](const TCHAR* JsonKey, const TCHAR* Prop)
	{
		bool Value = false;
		if ((*Desired)->TryGetBoolField(JsonKey, Value))
		{
			Properties->SetBoolField(ComponentName + TEXT(".") + Prop, Value);
		}
	};
	SetBool(TEXT("simulate_physics"), TEXT("BodyInstance.bSimulatePhysics"));
	SetBool(TEXT("enable_gravity"), TEXT("BodyInstance.bEnableGravity"));
	SetBool(TEXT("use_ccd"), TEXT("BodyInstance.bUseCCD"));
	double NumberValue = 0.0;
	if ((*Desired)->TryGetNumberField(TEXT("mass_override_kg"), NumberValue))
	{
		Properties->SetNumberField(ComponentName + TEXT(".BodyInstance.MassInKgOverride"), NumberValue);
	}
	if ((*Desired)->TryGetNumberField(TEXT("linear_damping"), NumberValue))
	{
		Properties->SetNumberField(ComponentName + TEXT(".BodyInstance.LinearDamping"), NumberValue);
	}
	if ((*Desired)->TryGetNumberField(TEXT("angular_damping"), NumberValue))
	{
		Properties->SetNumberField(ComponentName + TEXT(".BodyInstance.AngularDamping"), NumberValue);
	}
	FString StringValue;
	if ((*Desired)->TryGetStringField(TEXT("collision_profile_name"), StringValue))
	{
		Properties->SetStringField(ComponentName + TEXT(".BodyInstance.CollisionProfileName"), StringValue);
	}
	if ((*Desired)->TryGetStringField(TEXT("collision_enabled"), StringValue))
	{
		Properties->SetStringField(ComponentName + TEXT(".BodyInstance.CollisionEnabled"), StringValue);
	}

	TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
	Resource->SetStringField(TEXT("id"), ActorId);
	Resource->SetStringField(TEXT("operation"), TEXT("patch"));
	if (!ActorClassPath.IsEmpty())
	{
		Resource->SetStringField(TEXT("class"), ActorClassPath);
	}
	Resource->SetObjectField(TEXT("properties"), Properties);
	TArray<TSharedPtr<FJsonValue>> Resources;
	Resources.Add(MakeShared<FJsonValueObject>(Resource));
	TSharedPtr<FJsonObject> Patch = MakeShared<FJsonObject>();
	Patch->SetStringField(TEXT("schema"), TEXT("uai.level_content.v1"));
	Patch->SetArrayField(TEXT("resources"), Resources);

	OutData->SetStringField(TEXT("schema"), UeAgentPhysicsOps::PhysicsComponentSchema);
	OutData->SetObjectField(TEXT("properties"), Properties);
	OutData->SetObjectField(TEXT("level_content_patch"), Patch);
	OutData->SetArrayField(TEXT("unsupported_fields"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetBoolField(TEXT("requires_mobility_change"), false);
	return true;
}

static TSharedPtr<FJsonObject> UeAgentPhysicsPhysicalMaterialToJson(UPhysicalMaterial* Material)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema"), UeAgentPhysicsOps::PhysicalMaterialSchema);
	Root->SetStringField(TEXT("asset_path"), UeAgentPhysicsOps::AssetPathForObject(Material));
	Root->SetStringField(TEXT("object_path"), Material ? Material->GetPathName() : FString());
	Root->SetStringField(TEXT("class"), Material ? Material->GetClass()->GetPathName() : TEXT("/Script/PhysicsCore.PhysicalMaterial"));
	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	if (Material)
	{
		Props->SetNumberField(TEXT("friction"), Material->Friction);
		Props->SetNumberField(TEXT("static_friction"), Material->StaticFriction);
		Props->SetNumberField(TEXT("restitution"), Material->Restitution);
		Props->SetNumberField(TEXT("density"), Material->Density);
		Props->SetNumberField(TEXT("raise_mass_to_power"), Material->RaiseMassToPower);
		Props->SetBoolField(TEXT("override_friction_combine_mode"), Material->bOverrideFrictionCombineMode);
		Props->SetStringField(TEXT("friction_combine_mode"), UeAgentPhysicsOps::CombineModeToString(Material->FrictionCombineMode));
		Props->SetBoolField(TEXT("override_restitution_combine_mode"), Material->bOverrideRestitutionCombineMode);
		Props->SetStringField(TEXT("restitution_combine_mode"), UeAgentPhysicsOps::CombineModeToString(Material->RestitutionCombineMode));
		Props->SetStringField(TEXT("surface_type"), UeAgentPhysicsOps::SurfaceTypeToString(Material->SurfaceType));
	}
	Root->SetObjectField(TEXT("properties"), Props);
	return Root;
}

static bool UeAgentPhysicsValidatePhysicalMaterialJson(const TSharedPtr<FJsonObject>& Root, bool bStrict, TArray<TSharedPtr<FJsonValue>>& Issues, FString& OutAssetPath)
{
	if (!Root.IsValid())
	{
		UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("json_root_invalid"), TEXT("Root JSON object is invalid."));
		return false;
	}
	Root->TryGetStringField(TEXT("asset_path"), OutAssetPath);
	OutAssetPath.TrimStartAndEndInline();
	if (OutAssetPath.IsEmpty())
	{
		UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("missing_asset_path"), TEXT("PhysicalMaterial JSON requires asset_path."), TEXT("asset_path"));
	}
	const TSharedPtr<FJsonObject>* Props = nullptr;
	if (!Root->TryGetObjectField(TEXT("properties"), Props) || !Props || !Props->IsValid())
	{
		UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("missing_properties"), TEXT("PhysicalMaterial JSON requires properties object."), TEXT("properties"));
	}
	else
	{
		static const TSet<FString> KnownProps = {
			TEXT("friction"), TEXT("static_friction"), TEXT("restitution"), TEXT("density"), TEXT("raise_mass_to_power"),
			TEXT("override_friction_combine_mode"), TEXT("friction_combine_mode"),
			TEXT("override_restitution_combine_mode"), TEXT("restitution_combine_mode"), TEXT("surface_type")
		};
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Props)->Values)
		{
			if (bStrict && !KnownProps.Contains(Pair.Key))
			{
				UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("unknown_field"), FString::Printf(TEXT("Unknown property '%s'."), *Pair.Key), TEXT("properties.") + Pair.Key);
			}
		}
		double Number = 0.0;
		if ((*Props)->TryGetNumberField(TEXT("friction"), Number) && Number < 0.0)
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("invalid_friction"), TEXT("friction must be >= 0."), TEXT("properties.friction"));
		}
		if ((*Props)->TryGetNumberField(TEXT("static_friction"), Number) && Number < 0.0)
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("invalid_static_friction"), TEXT("static_friction must be >= 0."), TEXT("properties.static_friction"));
		}
		if ((*Props)->TryGetNumberField(TEXT("restitution"), Number) && (Number < 0.0 || Number > 1.0))
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("invalid_restitution"), TEXT("restitution must be between 0 and 1."), TEXT("properties.restitution"));
		}
		if ((*Props)->TryGetNumberField(TEXT("density"), Number) && Number <= 0.0)
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("invalid_density"), TEXT("density must be > 0."), TEXT("properties.density"));
		}
		FString StringValue;
		EFrictionCombineMode::Type CombineMode;
		if ((*Props)->TryGetStringField(TEXT("friction_combine_mode"), StringValue) && !UeAgentPhysicsOps::ParseCombineMode(StringValue, CombineMode))
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("invalid_friction_combine_mode"), TEXT("friction_combine_mode is invalid."), TEXT("properties.friction_combine_mode"));
		}
		if ((*Props)->TryGetStringField(TEXT("restitution_combine_mode"), StringValue) && !UeAgentPhysicsOps::ParseCombineMode(StringValue, CombineMode))
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("invalid_restitution_combine_mode"), TEXT("restitution_combine_mode is invalid."), TEXT("properties.restitution_combine_mode"));
		}
		EPhysicalSurface Surface;
		if ((*Props)->TryGetStringField(TEXT("surface_type"), StringValue) && !UeAgentPhysicsOps::ParseSurfaceType(StringValue, Surface))
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_material_surface_type_unknown"), TEXT("surface_type is invalid."), TEXT("properties.surface_type"));
		}
	}
	return !UeAgentPhysicsOps::HasErrorIssue(Issues);
}

static UPhysicalMaterial* UeAgentPhysicsCreatePhysicalMaterial(const FString& AssetPathInput, bool bCreateIfMissing, bool& bOutCreated, FString& OutError)
{
	bOutCreated = false;
	if (UPhysicalMaterial* Existing = UeAgentPhysicsOps::LoadAsset<UPhysicalMaterial>(AssetPathInput))
	{
		return Existing;
	}
	if (!bCreateIfMissing)
	{
		OutError = TEXT("physical_material_not_found");
		return nullptr;
	}
	const FString AssetPath = UeAgentPhysicsOps::NormalizeAssetPath(AssetPathInput);
	if (!FPackageName::IsValidLongPackageName(AssetPath))
	{
		OutError = TEXT("invalid_asset_path");
		return nullptr;
	}
	UPackage* Package = CreatePackage(*AssetPath);
	const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
	UPhysicalMaterial* Material = NewObject<UPhysicalMaterial>(Package, UPhysicalMaterial::StaticClass(), *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!Material)
	{
		OutError = TEXT("create_physical_material_failed");
		return nullptr;
	}
	FAssetRegistryModule::AssetCreated(Material);
	Material->MarkPackageDirty();
	bOutCreated = true;
	return Material;
}

static bool UeAgentPhysicsApplyPhysicalMaterialProps(UPhysicalMaterial* Material, const TSharedPtr<FJsonObject>& Props, TArray<TSharedPtr<FJsonValue>>& Results, FString& OutError)
{
	if (!Material || !Props.IsValid())
	{
		OutError = TEXT("invalid_physical_material_apply_target");
		return false;
	}
	auto Add = [&Results](const FString& Name)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("property"), Name);
		Result->SetStringField(TEXT("status"), TEXT("applied"));
		Results.Add(MakeShared<FJsonValueObject>(Result));
	};
	double Number = 0.0;
	bool BoolValue = false;
	FString StringValue;
	if (Props->TryGetNumberField(TEXT("friction"), Number)) { Material->Friction = static_cast<float>(Number); Add(TEXT("friction")); }
	if (Props->TryGetNumberField(TEXT("static_friction"), Number)) { Material->StaticFriction = static_cast<float>(Number); Add(TEXT("static_friction")); }
	if (Props->TryGetNumberField(TEXT("restitution"), Number)) { Material->Restitution = static_cast<float>(Number); Add(TEXT("restitution")); }
	if (Props->TryGetNumberField(TEXT("density"), Number)) { Material->Density = static_cast<float>(Number); Add(TEXT("density")); }
	if (Props->TryGetNumberField(TEXT("raise_mass_to_power"), Number)) { Material->RaiseMassToPower = static_cast<float>(Number); Add(TEXT("raise_mass_to_power")); }
	if (Props->TryGetBoolField(TEXT("override_friction_combine_mode"), BoolValue)) { Material->bOverrideFrictionCombineMode = BoolValue; Add(TEXT("override_friction_combine_mode")); }
	if (Props->TryGetBoolField(TEXT("override_restitution_combine_mode"), BoolValue)) { Material->bOverrideRestitutionCombineMode = BoolValue; Add(TEXT("override_restitution_combine_mode")); }
	if (Props->TryGetStringField(TEXT("friction_combine_mode"), StringValue))
	{
		EFrictionCombineMode::Type Mode;
		if (!UeAgentPhysicsOps::ParseCombineMode(StringValue, Mode)) { OutError = TEXT("invalid_friction_combine_mode"); return false; }
		Material->FrictionCombineMode = Mode;
		Add(TEXT("friction_combine_mode"));
	}
	if (Props->TryGetStringField(TEXT("restitution_combine_mode"), StringValue))
	{
		EFrictionCombineMode::Type Mode;
		if (!UeAgentPhysicsOps::ParseCombineMode(StringValue, Mode)) { OutError = TEXT("invalid_restitution_combine_mode"); return false; }
		Material->RestitutionCombineMode = Mode;
		Add(TEXT("restitution_combine_mode"));
	}
	if (Props->TryGetStringField(TEXT("surface_type"), StringValue))
	{
		EPhysicalSurface Surface;
		if (!UeAgentPhysicsOps::ParseSurfaceType(StringValue, Surface)) { OutError = TEXT("physics_material_surface_type_unknown"); return false; }
		Material->SurfaceType = Surface;
		Add(TEXT("surface_type"));
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPhysicalMaterialExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UPhysicalMaterial* Material = UeAgentPhysicsOps::LoadAsset<UPhysicalMaterial>(AssetPath);
	if (!Material)
	{
		OutError = TEXT("physical_material_not_found");
		return false;
	}
	OutData = UeAgentPhysicsPhysicalMaterialToJson(Material);
	FString OutputFile;
	if (JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile) && !OutputFile.TrimStartAndEnd().IsEmpty())
	{
		FString ResolvedPath;
		if (!UeAgentPhysicsOps::SaveAuthoringObjectToFile(OutputFile, OutData, ResolvedPath, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), ResolvedPath);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPhysicalMaterialValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString Source;
	if (!UeAgentPhysicsOps::GetInputJsonObject(Ctx, Root, Source, OutError))
	{
		return false;
	}
	bool bStrict = true;
	JsonTryGetBool(Ctx.Params, TEXT("strict"), bStrict);
	TArray<TSharedPtr<FJsonValue>> Issues;
	FString AssetPath;
	const bool bValid = UeAgentPhysicsValidatePhysicalMaterialJson(Root, bStrict, Issues, AssetPath);
	OutData->SetStringField(TEXT("schema"), UeAgentPhysicsOps::PhysicalMaterialSchema);
	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetBoolField(TEXT("valid"), bValid);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("issue_count"), Issues.Num());
	if (!bValid)
	{
		OutError = TEXT("physical_material_json_invalid");
		return false;
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPhysicalMaterialApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString Source;
	if (!UeAgentPhysicsOps::GetInputJsonObject(Ctx, Root, Source, OutError))
	{
		return false;
	}
	bool bStrict = true;
	bool bCreateIfMissing = true;
	bool bSaveAfterApply = false;
	bool bBackup = false;
	JsonTryGetBool(Ctx.Params, TEXT("strict"), bStrict);
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	JsonTryGetBool(Ctx.Params, TEXT("backup"), bBackup);

	TArray<TSharedPtr<FJsonValue>> Issues;
	FString AssetPath;
	if (!UeAgentPhysicsValidatePhysicalMaterialJson(Root, bStrict, Issues, AssetPath))
	{
		OutData->SetArrayField(TEXT("issues"), Issues);
		OutError = TEXT("physical_material_json_invalid");
		return false;
	}
	bool bCreated = false;
	UPhysicalMaterial* Material = UeAgentPhysicsCreatePhysicalMaterial(AssetPath, bCreateIfMissing, bCreated, OutError);
	if (!Material)
	{
		return false;
	}
	FString BackupAssetPath;
	FString BackupObjectPath;
	if (bBackup && !bCreated && !UeAgentPhysicsOps::BackupAssetObject(Material, BackupAssetPath, BackupObjectPath, OutError))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Props = nullptr;
	Root->TryGetObjectField(TEXT("properties"), Props);
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	{
		FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "PhysicalMaterialApply", "UeAgentInterface PhysicalMaterial Apply"));
		Material->Modify();
		if (!UeAgentPhysicsApplyPhysicalMaterialProps(Material, Props ? *Props : nullptr, PropertyResults, OutError))
		{
			return false;
		}
		Material->PostEditChange();
		Material->MarkPackageDirty();
	}
	bool bSaved = false;
	if (bSaveAfterApply)
	{
		if (!UeAgentPhysicsOps::SaveAssetPackage(Material, OutError))
		{
			return false;
		}
		bSaved = true;
	}
	TSharedPtr<FJsonObject> Readback = UeAgentPhysicsPhysicalMaterialToJson(Material);
	OutData->SetStringField(TEXT("schema"), UeAgentPhysicsOps::PhysicalMaterialSchema);
	OutData->SetStringField(TEXT("asset_path"), UeAgentPhysicsOps::AssetPathForObject(Material));
	OutData->SetStringField(TEXT("object_path"), Material->GetPathName());
	OutData->SetBoolField(TEXT("asset_created"), bCreated);
	OutData->SetBoolField(TEXT("backup_created"), !BackupAssetPath.IsEmpty());
	OutData->SetStringField(TEXT("backup_asset_path"), BackupAssetPath);
	OutData->SetStringField(TEXT("backup_object_path"), BackupObjectPath);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetObjectField(TEXT("readback"), Readback);
	OutData->SetStringField(TEXT("readback_hash"), UeAgentPhysicsOps::HashJsonObject(Readback));
	OutData->SetBoolField(TEXT("surface_type_verified"), true);
	OutData->SetBoolField(TEXT("saved"), bSaved);
	return true;
}

namespace
{
	struct FUeAgentPhysicsEndpoint
	{
		FString ActorId;
		FString ComponentName;
		FString BoneName;
		AActor* Actor = nullptr;
		UPrimitiveComponent* Component = nullptr;
	};

	static bool UeAgentPhysicsJsonHasUnsupportedOptionalField(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FString& OutKey, FString& OutPath)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		static const TArray<FString> UnsupportedTokens = {
			TEXT("vehicle"),
			TEXT("geometry_collection"),
			TEXT("field_system"),
			TEXT("chaos_cache"),
			TEXT("chaos_caching"),
			TEXT("chaos_flesh"),
			TEXT("cloth_asset"),
			TEXT("groom"),
			TEXT("hair"),
			TEXT("physics_control"),
			TEXT("chaos_visual_debugger")
		};
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
		{
			const FString KeyLower = Pair.Key.ToLower();
			for (const FString& Token : UnsupportedTokens)
			{
				if (KeyLower.Contains(Token))
				{
					OutKey = Pair.Key;
					OutPath = Path.IsEmpty() ? Pair.Key : Path + TEXT(".") + Pair.Key;
					return true;
				}
			}

			const TSharedPtr<FJsonObject>* ChildObj = nullptr;
			if (Pair.Value.IsValid() && Pair.Value->TryGetObject(ChildObj) && ChildObj && ChildObj->IsValid())
			{
				if (UeAgentPhysicsJsonHasUnsupportedOptionalField(*ChildObj, Path.IsEmpty() ? Pair.Key : Path + TEXT(".") + Pair.Key, OutKey, OutPath))
				{
					return true;
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (Pair.Value.IsValid() && Pair.Value->TryGetArray(Array) && Array)
			{
				for (int32 Index = 0; Index < Array->Num(); ++Index)
				{
					const TSharedPtr<FJsonValue>& Item = (*Array)[Index];
					const TSharedPtr<FJsonObject>* ArrayObj = nullptr;
					if (Item.IsValid() && Item->TryGetObject(ArrayObj) && ArrayObj && ArrayObj->IsValid()
						&& UeAgentPhysicsJsonHasUnsupportedOptionalField(*ArrayObj, FString::Printf(TEXT("%s.%s[%d]"), *Path, *Pair.Key, Index), OutKey, OutPath))
					{
						return true;
					}
				}
			}
		}
		return false;
	}

	static TSharedPtr<FJsonObject> UeAgentPhysicsEndpointToJson(const FUeAgentPhysicsEndpoint& Endpoint)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("actor_id"), Endpoint.ActorId);
		Obj->SetStringField(TEXT("component"), Endpoint.ComponentName);
		Obj->SetStringField(TEXT("bone_name"), Endpoint.BoneName);
		Obj->SetBoolField(TEXT("resolved"), Endpoint.Actor != nullptr && Endpoint.Component != nullptr);
		Obj->SetStringField(TEXT("actor_name"), Endpoint.Actor ? Endpoint.Actor->GetName() : FString());
		Obj->SetStringField(TEXT("component_path"), Endpoint.Component ? Endpoint.Component->GetPathName() : FString());
		return Obj;
	}

	static bool UeAgentPhysicsReadEndpoint(const TSharedPtr<FJsonObject>& Obj, const FString& Path, UWorld* World, FUeAgentPhysicsEndpoint& OutEndpoint, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Obj.IsValid())
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_constraint_endpoint_unresolved"), TEXT("Endpoint object is missing."), Path);
			return false;
		}
		Obj->TryGetStringField(TEXT("actor_id"), OutEndpoint.ActorId);
		Obj->TryGetStringField(TEXT("actor"), OutEndpoint.ActorId);
		Obj->TryGetStringField(TEXT("component"), OutEndpoint.ComponentName);
		Obj->TryGetStringField(TEXT("bone_name"), OutEndpoint.BoneName);
		OutEndpoint.ActorId.TrimStartAndEndInline();
		OutEndpoint.ComponentName.TrimStartAndEndInline();
		OutEndpoint.BoneName.TrimStartAndEndInline();
		if (OutEndpoint.ActorId.IsEmpty())
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_constraint_endpoint_unresolved"), TEXT("Endpoint actor_id is required."), Path + TEXT(".actor_id"));
			return false;
		}
		OutEndpoint.Actor = UeAgentPhysicsOps::FindActorInWorld(World, OutEndpoint.ActorId);
		if (!OutEndpoint.Actor)
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_constraint_endpoint_unresolved"), TEXT("Endpoint actor could not be resolved."), Path + TEXT(".actor_id"));
			return false;
		}
		OutEndpoint.Component = UeAgentPhysicsOps::FindPrimitiveComponent(OutEndpoint.Actor, OutEndpoint.ComponentName);
		if (!OutEndpoint.Component)
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_constraint_endpoint_unresolved"), TEXT("Endpoint component is not a PrimitiveComponent."), Path + TEXT(".component"));
			return false;
		}
		if (!OutEndpoint.BoneName.IsEmpty())
		{
			if (USkeletalMeshComponent* SkelComp = Cast<USkeletalMeshComponent>(OutEndpoint.Component))
			{
				USkeletalMesh* Mesh = SkelComp->GetSkeletalMeshAsset();
				if (!Mesh || Mesh->GetRefSkeleton().FindBoneIndex(FName(*OutEndpoint.BoneName)) == INDEX_NONE)
				{
					UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_constraint_endpoint_unresolved"), TEXT("Endpoint bone_name does not exist on SkeletalMeshComponent."), Path + TEXT(".bone_name"));
					return false;
				}
			}
		}
		return true;
	}

	static UPhysicsConstraintComponent* UeAgentPhysicsFindConstraintComponent(AActor* Actor, const FString& ComponentName)
	{
		if (!Actor)
		{
			return nullptr;
		}
		auto IsConstraintAlias = [](const FString& Name)
		{
			return Name.IsEmpty() ||
				Name.Equals(TEXT("ConstraintComp"), ESearchCase::IgnoreCase) ||
				Name.Equals(TEXT("PhysicsConstraintComponent"), ESearchCase::IgnoreCase) ||
				Name.Equals(TEXT("DefaultConstraintComponent"), ESearchCase::IgnoreCase);
		};
		if (APhysicsConstraintActor* ConstraintActor = Cast<APhysicsConstraintActor>(Actor))
		{
			if (UPhysicsConstraintComponent* ConstraintComp = ConstraintActor->GetConstraintComp())
			{
				if (IsConstraintAlias(ComponentName) ||
					ConstraintComp->GetName().Equals(ComponentName, ESearchCase::IgnoreCase) ||
					ConstraintComp->GetFName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
				{
					return ConstraintComp;
				}
			}
		}
		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (UPhysicsConstraintComponent* ConstraintComp = Cast<UPhysicsConstraintComponent>(Component))
			{
				if (IsConstraintAlias(ComponentName) ||
					ConstraintComp->GetName().Equals(ComponentName, ESearchCase::IgnoreCase) ||
					ConstraintComp->GetFName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
				{
					return ConstraintComp;
				}
			}
		}
		return nullptr;
	}

	static TSharedPtr<FJsonObject> UeAgentPhysicsConstraintInstanceToJson(const FConstraintInstance& ConstraintInstance)
	{
		TSharedPtr<FJsonObject> Constraint = MakeShared<FJsonObject>();
		Constraint->SetBoolField(TEXT("disable_collision"), ConstraintInstance.IsCollisionDisabled());
		TSharedPtr<FJsonObject> Linear = MakeShared<FJsonObject>();
		Linear->SetStringField(TEXT("x_motion"), UeAgentPhysicsOps::LinearMotionToString(ConstraintInstance.GetLinearXMotion()));
		Linear->SetStringField(TEXT("y_motion"), UeAgentPhysicsOps::LinearMotionToString(ConstraintInstance.GetLinearYMotion()));
		Linear->SetStringField(TEXT("z_motion"), UeAgentPhysicsOps::LinearMotionToString(ConstraintInstance.GetLinearZMotion()));
		Linear->SetNumberField(TEXT("limit"), ConstraintInstance.ProfileInstance.LinearLimit.Limit);
		Constraint->SetObjectField(TEXT("linear"), Linear);

		TSharedPtr<FJsonObject> Angular = MakeShared<FJsonObject>();
		Angular->SetStringField(TEXT("swing1_motion"), UeAgentPhysicsOps::AngularMotionToString(ConstraintInstance.GetAngularSwing1Motion()));
		Angular->SetNumberField(TEXT("swing1_limit_degrees"), ConstraintInstance.GetAngularSwing1Limit());
		Angular->SetStringField(TEXT("swing2_motion"), UeAgentPhysicsOps::AngularMotionToString(ConstraintInstance.GetAngularSwing2Motion()));
		Angular->SetNumberField(TEXT("swing2_limit_degrees"), ConstraintInstance.GetAngularSwing2Limit());
		Angular->SetStringField(TEXT("twist_motion"), UeAgentPhysicsOps::AngularMotionToString(ConstraintInstance.GetAngularTwistMotion()));
		Angular->SetNumberField(TEXT("twist_limit_degrees"), ConstraintInstance.GetAngularTwistLimit());
		Constraint->SetObjectField(TEXT("angular"), Angular);

		TSharedPtr<FJsonObject> Projection = MakeShared<FJsonObject>();
		Projection->SetBoolField(TEXT("enabled"), ConstraintInstance.IsProjectionEnabled());
		Constraint->SetObjectField(TEXT("projection"), Projection);
		return Constraint;
	}

	static TSharedPtr<FJsonObject> UeAgentPhysicsConstraintComponentToJson(UPhysicsConstraintComponent* ConstraintComponent)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), UeAgentPhysicsOps::PhysicsConstraintSchema);
		if (!ConstraintComponent)
		{
			Root->SetBoolField(TEXT("resolved"), false);
			return Root;
		}
		AActor* Owner = ConstraintComponent->GetOwner();
		TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetStringField(TEXT("actor_id"), UeAgentPhysicsOps::ActorIdentifier(Owner));
		Target->SetStringField(TEXT("actor_name"), Owner ? Owner->GetName() : FString());
		Target->SetStringField(TEXT("component"), ConstraintComponent->GetName());
		Root->SetObjectField(TEXT("target"), Target);

		UPrimitiveComponent* Component1 = nullptr;
		UPrimitiveComponent* Component2 = nullptr;
		FName BoneName1 = NAME_None;
		FName BoneName2 = NAME_None;
		ConstraintComponent->GetConstrainedComponents(Component1, BoneName1, Component2, BoneName2);
		TSharedPtr<FJsonObject> Constrained = MakeShared<FJsonObject>();
		FUeAgentPhysicsEndpoint Endpoint1;
		Endpoint1.Actor = Component1 ? Component1->GetOwner() : nullptr;
		Endpoint1.Component = Component1;
		Endpoint1.ActorId = UeAgentPhysicsOps::ActorIdentifier(Endpoint1.Actor);
		Endpoint1.ComponentName = Component1 ? Component1->GetName() : FString();
		Endpoint1.BoneName = BoneName1.IsNone() ? FString() : BoneName1.ToString();
		FUeAgentPhysicsEndpoint Endpoint2;
		Endpoint2.Actor = Component2 ? Component2->GetOwner() : nullptr;
		Endpoint2.Component = Component2;
		Endpoint2.ActorId = UeAgentPhysicsOps::ActorIdentifier(Endpoint2.Actor);
		Endpoint2.ComponentName = Component2 ? Component2->GetName() : FString();
		Endpoint2.BoneName = BoneName2.IsNone() ? FString() : BoneName2.ToString();
		Constrained->SetObjectField(TEXT("component1"), UeAgentPhysicsEndpointToJson(Endpoint1));
		Constrained->SetObjectField(TEXT("component2"), UeAgentPhysicsEndpointToJson(Endpoint2));
		Root->SetObjectField(TEXT("constrained"), Constrained);
		Root->SetObjectField(TEXT("constraint"), UeAgentPhysicsConstraintInstanceToJson(ConstraintComponent->ConstraintInstance));
		Root->SetBoolField(TEXT("resolved"), true);
		return Root;
	}

	static bool UeAgentPhysicsValidateConstraintSettings(const TSharedPtr<FJsonObject>& ConstraintObj, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!ConstraintObj.IsValid())
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), TEXT("Constraint JSON requires constraint object."), TEXT("constraint"));
			return false;
		}
		FString UnsupportedKey;
		FString UnsupportedPath;
		if (UeAgentPhysicsJsonHasUnsupportedOptionalField(ConstraintObj, TEXT("constraint"), UnsupportedKey, UnsupportedPath))
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_optional_plugin_required"), FString::Printf(TEXT("Field '%s' belongs to an optional physics plugin outside baseline scope."), *UnsupportedKey), UnsupportedPath);
		}

		const TSharedPtr<FJsonObject>* Linear = nullptr;
		if (ConstraintObj->TryGetObjectField(TEXT("linear"), Linear) && Linear && Linear->IsValid())
		{
			FString Motion;
			ELinearConstraintMotion ParsedMotion;
			for (const TCHAR* Key : { TEXT("x_motion"), TEXT("y_motion"), TEXT("z_motion") })
			{
				if ((*Linear)->TryGetStringField(Key, Motion) && !UeAgentPhysicsOps::ParseLinearMotion(Motion, ParsedMotion))
				{
					UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_constraint_invalid_motion"), FString::Printf(TEXT("%s is invalid."), Key), FString(TEXT("constraint.linear.")) + Key);
				}
			}
			double Limit = 0.0;
			if ((*Linear)->TryGetNumberField(TEXT("limit"), Limit) && Limit < 0.0)
			{
				UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_constraint_invalid_motion"), TEXT("linear limit must be >= 0."), TEXT("constraint.linear.limit"));
			}
		}

		const TSharedPtr<FJsonObject>* Angular = nullptr;
		if (ConstraintObj->TryGetObjectField(TEXT("angular"), Angular) && Angular && Angular->IsValid())
		{
			FString Motion;
			EAngularConstraintMotion ParsedMotion;
			for (const TCHAR* Key : { TEXT("swing1_motion"), TEXT("swing2_motion"), TEXT("twist_motion") })
			{
				if ((*Angular)->TryGetStringField(Key, Motion) && !UeAgentPhysicsOps::ParseAngularMotion(Motion, ParsedMotion))
				{
					UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_constraint_invalid_motion"), FString::Printf(TEXT("%s is invalid."), Key), FString(TEXT("constraint.angular.")) + Key);
				}
			}
			for (const TCHAR* Key : { TEXT("swing1_limit_degrees"), TEXT("swing2_limit_degrees"), TEXT("twist_limit_degrees") })
			{
				double Limit = 0.0;
				if ((*Angular)->TryGetNumberField(Key, Limit) && Limit < 0.0)
				{
					UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_constraint_invalid_motion"), FString::Printf(TEXT("%s must be >= 0."), Key), FString(TEXT("constraint.angular.")) + Key);
				}
			}
		}
		return !UeAgentPhysicsOps::HasErrorIssue(Issues);
	}

	static bool UeAgentPhysicsApplyConstraintSettings(UPhysicsConstraintComponent* ConstraintComponent, const TSharedPtr<FJsonObject>& ConstraintObj, FString& OutError)
	{
		if (!ConstraintComponent || !ConstraintObj.IsValid())
		{
			OutError = TEXT("invalid_constraint_apply_target");
			return false;
		}
		bool bDisableCollision = false;
		if (ConstraintObj->TryGetBoolField(TEXT("disable_collision"), bDisableCollision))
		{
			ConstraintComponent->SetDisableCollision(bDisableCollision);
		}
		const TSharedPtr<FJsonObject>* Linear = nullptr;
		if (ConstraintObj->TryGetObjectField(TEXT("linear"), Linear) && Linear && Linear->IsValid())
		{
			double Limit = ConstraintComponent->ConstraintInstance.ProfileInstance.LinearLimit.Limit;
			(*Linear)->TryGetNumberField(TEXT("limit"), Limit);
			FString MotionText;
			ELinearConstraintMotion Motion;
			if ((*Linear)->TryGetStringField(TEXT("x_motion"), MotionText) && UeAgentPhysicsOps::ParseLinearMotion(MotionText, Motion))
			{
				ConstraintComponent->SetLinearXLimit(Motion, static_cast<float>(Limit));
			}
			if ((*Linear)->TryGetStringField(TEXT("y_motion"), MotionText) && UeAgentPhysicsOps::ParseLinearMotion(MotionText, Motion))
			{
				ConstraintComponent->SetLinearYLimit(Motion, static_cast<float>(Limit));
			}
			if ((*Linear)->TryGetStringField(TEXT("z_motion"), MotionText) && UeAgentPhysicsOps::ParseLinearMotion(MotionText, Motion))
			{
				ConstraintComponent->SetLinearZLimit(Motion, static_cast<float>(Limit));
			}
		}
		const TSharedPtr<FJsonObject>* Angular = nullptr;
		if (ConstraintObj->TryGetObjectField(TEXT("angular"), Angular) && Angular && Angular->IsValid())
		{
			FString MotionText;
			EAngularConstraintMotion Motion;
			double Limit = 0.0;
			if ((*Angular)->TryGetStringField(TEXT("swing1_motion"), MotionText) && UeAgentPhysicsOps::ParseAngularMotion(MotionText, Motion))
			{
				Limit = ConstraintComponent->ConstraintInstance.GetAngularSwing1Limit();
				(*Angular)->TryGetNumberField(TEXT("swing1_limit_degrees"), Limit);
				ConstraintComponent->SetAngularSwing1Limit(Motion, static_cast<float>(Limit));
			}
			if ((*Angular)->TryGetStringField(TEXT("swing2_motion"), MotionText) && UeAgentPhysicsOps::ParseAngularMotion(MotionText, Motion))
			{
				Limit = ConstraintComponent->ConstraintInstance.GetAngularSwing2Limit();
				(*Angular)->TryGetNumberField(TEXT("swing2_limit_degrees"), Limit);
				ConstraintComponent->SetAngularSwing2Limit(Motion, static_cast<float>(Limit));
			}
			if ((*Angular)->TryGetStringField(TEXT("twist_motion"), MotionText) && UeAgentPhysicsOps::ParseAngularMotion(MotionText, Motion))
			{
				Limit = ConstraintComponent->ConstraintInstance.GetAngularTwistLimit();
				(*Angular)->TryGetNumberField(TEXT("twist_limit_degrees"), Limit);
				ConstraintComponent->SetAngularTwistLimit(Motion, static_cast<float>(Limit));
			}
		}
		const TSharedPtr<FJsonObject>* Projection = nullptr;
		if (ConstraintObj->TryGetObjectField(TEXT("projection"), Projection) && Projection && Projection->IsValid())
		{
			bool bProjectionEnabled = false;
			if ((*Projection)->TryGetBoolField(TEXT("enabled"), bProjectionEnabled))
			{
				ConstraintComponent->SetProjectionEnabled(bProjectionEnabled);
			}
		}
		ConstraintComponent->PostEditChange();
		ConstraintComponent->MarkPackageDirty();
		return true;
	}
}

bool FUeAgentLegacyMechanics::CmdPhysicsConstraintExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString ActorId;
	if (!JsonTryGetString(Ctx.Params, TEXT("actor_id"), ActorId) || ActorId.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_actor_id");
		return false;
	}
	FString ComponentName;
	JsonTryGetString(Ctx.Params, TEXT("component"), ComponentName);
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	AActor* Actor = UeAgentPhysicsOps::FindActorInWorld(World, ActorId);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}
	UPhysicsConstraintComponent* ConstraintComponent = UeAgentPhysicsFindConstraintComponent(Actor, ComponentName);
	if (!ConstraintComponent)
	{
		OutError = TEXT("physics_constraint_component_unresolved");
		return false;
	}
	OutData = UeAgentPhysicsConstraintComponentToJson(ConstraintComponent);
	FString OutputFile;
	if (JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile) && !OutputFile.TrimStartAndEnd().IsEmpty())
	{
		FString ResolvedPath;
		if (!UeAgentPhysicsOps::SaveAuthoringObjectToFile(OutputFile, OutData, ResolvedPath, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), ResolvedPath);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPhysicsConstraintValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString Source;
	if (!UeAgentPhysicsOps::GetInputJsonObject(Ctx, Root, Source, OutError))
	{
		return false;
	}
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	FString UnsupportedKey;
	FString UnsupportedPath;
	if (UeAgentPhysicsJsonHasUnsupportedOptionalField(Root, FString(), UnsupportedKey, UnsupportedPath))
	{
		UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_optional_plugin_required"), FString::Printf(TEXT("Field '%s' belongs to an optional physics plugin outside baseline scope."), *UnsupportedKey), UnsupportedPath);
	}

	const TSharedPtr<FJsonObject>* Constrained = nullptr;
	if (!Root->TryGetObjectField(TEXT("constrained"), Constrained) || !Constrained || !Constrained->IsValid())
	{
		UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), TEXT("Constraint JSON requires constrained object."), TEXT("constrained"));
	}
	else
	{
		const TSharedPtr<FJsonObject>* EndpointObj = nullptr;
		FUeAgentPhysicsEndpoint Endpoint;
		if ((*Constrained)->TryGetObjectField(TEXT("component1"), EndpointObj) && EndpointObj)
		{
			UeAgentPhysicsReadEndpoint(*EndpointObj, TEXT("constrained.component1"), World, Endpoint, Issues);
		}
		else
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_constraint_endpoint_unresolved"), TEXT("component1 is required."), TEXT("constrained.component1"));
		}
		Endpoint = FUeAgentPhysicsEndpoint();
		if ((*Constrained)->TryGetObjectField(TEXT("component2"), EndpointObj) && EndpointObj)
		{
			UeAgentPhysicsReadEndpoint(*EndpointObj, TEXT("constrained.component2"), World, Endpoint, Issues);
		}
		else
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_constraint_endpoint_unresolved"), TEXT("component2 is required."), TEXT("constrained.component2"));
		}
	}
	const TSharedPtr<FJsonObject>* ConstraintObj = nullptr;
	Root->TryGetObjectField(TEXT("constraint"), ConstraintObj);
	UeAgentPhysicsValidateConstraintSettings(ConstraintObj ? *ConstraintObj : nullptr, Issues);

	const bool bValid = !UeAgentPhysicsOps::HasErrorIssue(Issues);
	OutData->SetStringField(TEXT("schema"), UeAgentPhysicsOps::PhysicsConstraintSchema);
	OutData->SetBoolField(TEXT("valid"), bValid);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("issue_count"), Issues.Num());
	if (!bValid)
	{
		OutError = TEXT("physics_constraint_json_invalid");
		return false;
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPhysicsConstraintApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString Source;
	if (!UeAgentPhysicsOps::GetInputJsonObject(Ctx, Root, Source, OutError))
	{
		return false;
	}
	bool bCreateIfMissing = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	TSharedPtr<FJsonObject> ValidateData = MakeShared<FJsonObject>();
	if (!CmdPhysicsConstraintValidateJson(Ctx, ValidateData, OutError))
	{
		OutData->SetObjectField(TEXT("validation"), ValidateData);
		return false;
	}
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* TargetObj = nullptr;
	const bool bHasTarget = Root->TryGetObjectField(TEXT("target"), TargetObj) && TargetObj && TargetObj->IsValid();
	FString TargetActorId;
	FString ComponentName = TEXT("ConstraintComp");
	if (bHasTarget)
	{
		(*TargetObj)->TryGetStringField(TEXT("actor_id"), TargetActorId);
		(*TargetObj)->TryGetStringField(TEXT("actor"), TargetActorId);
		(*TargetObj)->TryGetStringField(TEXT("component"), ComponentName);
	}
	if (TargetActorId.TrimStartAndEnd().IsEmpty())
	{
		TargetActorId = TEXT("UAI_PhysicsConstraint");
	}

	AActor* Actor = UeAgentPhysicsOps::FindActorInWorld(World, TargetActorId);
	bool bActorCreated = false;
	if (!Actor && bCreateIfMissing)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.ObjectFlags |= RF_Transactional;
		FVector Location(0.0, 0.0, 100.0);
		if (bHasTarget)
		{
			const TArray<TSharedPtr<FJsonValue>>* LocationArray = nullptr;
			if ((*TargetObj)->TryGetArrayField(TEXT("location"), LocationArray))
			{
				UeAgentPhysicsOps::JsonArrayToVector(LocationArray, Location);
			}
		}
		Actor = World->SpawnActor<APhysicsConstraintActor>(APhysicsConstraintActor::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
		if (Actor)
		{
			Actor->SetActorLabel(TargetActorId);
			bActorCreated = true;
		}
	}
	if (!Actor)
	{
		OutError = TEXT("physics_constraint_target_unresolved");
		return false;
	}
	UPhysicsConstraintComponent* ConstraintComponent = UeAgentPhysicsFindConstraintComponent(Actor, ComponentName);
	if (!ConstraintComponent)
	{
		OutError = TEXT("physics_constraint_component_unresolved");
		return false;
	}

	const TSharedPtr<FJsonObject>* Constrained = nullptr;
	const TSharedPtr<FJsonObject>* EndpointObj = nullptr;
	Root->TryGetObjectField(TEXT("constrained"), Constrained);
	FUeAgentPhysicsEndpoint Endpoint1;
	FUeAgentPhysicsEndpoint Endpoint2;
	TArray<TSharedPtr<FJsonValue>> EndpointIssues;
	if (!Constrained || !(*Constrained)->TryGetObjectField(TEXT("component1"), EndpointObj) || !UeAgentPhysicsReadEndpoint(*EndpointObj, TEXT("constrained.component1"), World, Endpoint1, EndpointIssues))
	{
		OutData->SetArrayField(TEXT("endpoint_issues"), EndpointIssues);
		OutError = TEXT("physics_constraint_endpoint_unresolved");
		return false;
	}
	if (!(*Constrained)->TryGetObjectField(TEXT("component2"), EndpointObj) || !UeAgentPhysicsReadEndpoint(*EndpointObj, TEXT("constrained.component2"), World, Endpoint2, EndpointIssues))
	{
		OutData->SetArrayField(TEXT("endpoint_issues"), EndpointIssues);
		OutError = TEXT("physics_constraint_endpoint_unresolved");
		return false;
	}

	const TSharedPtr<FJsonObject>* ConstraintObj = nullptr;
	Root->TryGetObjectField(TEXT("constraint"), ConstraintObj);
	{
		FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "PhysicsConstraintApply", "UeAgentInterface PhysicsConstraint Apply"));
		Actor->Modify();
		ConstraintComponent->Modify();
		ConstraintComponent->SetConstrainedComponents(
			Endpoint1.Component,
			Endpoint1.BoneName.IsEmpty() ? NAME_None : FName(*Endpoint1.BoneName),
			Endpoint2.Component,
			Endpoint2.BoneName.IsEmpty() ? NAME_None : FName(*Endpoint2.BoneName));
		if (!UeAgentPhysicsApplyConstraintSettings(ConstraintComponent, ConstraintObj ? *ConstraintObj : nullptr, OutError))
		{
			return false;
		}
		Actor->MarkPackageDirty();
	}

	OutData = UeAgentPhysicsConstraintComponentToJson(ConstraintComponent);
	OutData->SetBoolField(TEXT("actor_created"), bActorCreated);
	OutData->SetBoolField(TEXT("set_constrained_components_applied"), true);
	TSharedPtr<FJsonObject> EndpointResolution = MakeShared<FJsonObject>();
	EndpointResolution->SetObjectField(TEXT("component1"), UeAgentPhysicsEndpointToJson(Endpoint1));
	EndpointResolution->SetObjectField(TEXT("component2"), UeAgentPhysicsEndpointToJson(Endpoint2));
	OutData->SetObjectField(TEXT("endpoint_resolution"), EndpointResolution);
	OutData->SetObjectField(TEXT("validation"), ValidateData);
	return true;
}

namespace
{
	struct FUeAgentPhysicsAssetFolder
	{
		FString FolderPath;
		TSharedPtr<FJsonObject> AssetJson;
		TSharedPtr<FJsonObject> BodiesJson;
		TSharedPtr<FJsonObject> ConstraintsJson;
		TSharedPtr<FJsonObject> ProfilesJson;
		FString AssetPath;
		FString SkeletalMeshPath;
		USkeletalMesh* SkeletalMesh = nullptr;
		TSet<FName> DeclaredBodyNames;
	};

	static FString UeAgentPhysicsResolveFolderPath(const FString& FolderPath)
	{
		return UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath).Replace(TEXT("\\"), TEXT("/"));
	}

	static bool UeAgentPhysicsLoadFolderAuthoringFile(const FString& FolderPath, const FString& FileName, bool bRequired, TSharedPtr<FJsonObject>& OutJson, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		const FString FilePath = FPaths::Combine(FolderPath, FileName);
		if (!IFileManager::Get().FileExists(*FilePath))
		{
			if (bRequired)
			{
				UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("json_file_not_found"), FString::Printf(TEXT("%s is required."), *FileName), FileName);
			}
			OutJson.Reset();
			return !bRequired;
		}
		FString ResolvedPath;
		FString Error;
		if (!UeAgentPhysicsOps::LoadAuthoringObjectFromFile(FilePath, OutJson, ResolvedPath, Error))
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), Error, FString::Printf(TEXT("%s could not be loaded."), *FileName), FileName);
			return false;
		}
		return true;
	}

	static bool UeAgentPhysicsReadVectorObjectOrArray(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, FVector& OutVector)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Obj->TryGetArrayField(FieldName, Array))
		{
			return UeAgentPhysicsOps::JsonArrayToVector(Array, OutVector);
		}
		const TSharedPtr<FJsonObject>* VecObj = nullptr;
		if (Obj->TryGetObjectField(FieldName, VecObj) && VecObj && VecObj->IsValid())
		{
			double X = OutVector.X;
			double Y = OutVector.Y;
			double Z = OutVector.Z;
			(*VecObj)->TryGetNumberField(TEXT("x"), X);
			(*VecObj)->TryGetNumberField(TEXT("y"), Y);
			(*VecObj)->TryGetNumberField(TEXT("z"), Z);
			(*VecObj)->TryGetNumberField(TEXT("X"), X);
			(*VecObj)->TryGetNumberField(TEXT("Y"), Y);
			(*VecObj)->TryGetNumberField(TEXT("Z"), Z);
			OutVector = FVector(X, Y, Z);
			return true;
		}
		return false;
	}

	static TSharedPtr<FJsonObject> UeAgentPhysicsMakeShapeTransformJson(const FVector& Location, const FRotator& Rotation)
	{
		TSharedPtr<FJsonObject> Transform = MakeShared<FJsonObject>();
		Transform->SetObjectField(TEXT("location"), UeAgentPhysicsOps::VecToJson(Location));
		Transform->SetObjectField(TEXT("rotation"), UeAgentPhysicsOps::VecToJson(FVector(Rotation.Pitch, Rotation.Yaw, Rotation.Roll)));
		Transform->SetObjectField(TEXT("scale"), UeAgentPhysicsOps::VecToJson(FVector::OneVector));
		return Transform;
	}

	static void UeAgentPhysicsReadShapeTransform(const TSharedPtr<FJsonObject>& ShapeObj, FVector& OutLocation, FRotator& OutRotation)
	{
		const TSharedPtr<FJsonObject>* Transform = nullptr;
		if (ShapeObj.IsValid() && ShapeObj->TryGetObjectField(TEXT("transform"), Transform) && Transform && Transform->IsValid())
		{
			UeAgentPhysicsReadVectorObjectOrArray(*Transform, TEXT("location"), OutLocation);
			FVector RotVector(OutRotation.Pitch, OutRotation.Yaw, OutRotation.Roll);
			if (UeAgentPhysicsReadVectorObjectOrArray(*Transform, TEXT("rotation"), RotVector))
			{
				OutRotation = FRotator(RotVector.X, RotVector.Y, RotVector.Z);
			}
		}
	}

	static FString UeAgentPhysicsTypeToString(EPhysicsType Type)
	{
		switch (Type)
		{
		case PhysType_Kinematic: return TEXT("Kinematic");
		case PhysType_Simulated: return TEXT("Simulated");
		case PhysType_Default:
		default:
			return TEXT("Default");
		}
	}

	static bool UeAgentPhysicsParsePhysicsType(const FString& Input, EPhysicsType& OutType)
	{
		const FString Normalized = Input.TrimStartAndEnd().ToLower();
		if (Normalized.IsEmpty() || Normalized == TEXT("default") || Normalized == TEXT("phystype_default"))
		{
			OutType = PhysType_Default;
			return true;
		}
		if (Normalized == TEXT("kinematic") || Normalized == TEXT("phystype_kinematic"))
		{
			OutType = PhysType_Kinematic;
			return true;
		}
		if (Normalized == TEXT("simulated") || Normalized == TEXT("simulate") || Normalized == TEXT("phystype_simulated"))
		{
			OutType = PhysType_Simulated;
			return true;
		}
		return false;
	}

	static TSharedPtr<FJsonObject> UeAgentPhysicsBodySetupToJson(USkeletalBodySetup* BodySetup)
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		if (!BodySetup)
		{
			return Body;
		}
		Body->SetStringField(TEXT("bone_name"), BodySetup->BoneName.ToString());
		Body->SetStringField(TEXT("body_name"), BodySetup->BoneName.ToString());
		Body->SetStringField(TEXT("physics_type"), UeAgentPhysicsTypeToString(BodySetup->PhysicsType));
		Body->SetNumberField(TEXT("sphere_count"), BodySetup->AggGeom.SphereElems.Num());
		Body->SetNumberField(TEXT("box_count"), BodySetup->AggGeom.BoxElems.Num());
		Body->SetNumberField(TEXT("capsule_count"), BodySetup->AggGeom.SphylElems.Num());
		Body->SetNumberField(TEXT("convex_count"), BodySetup->AggGeom.ConvexElems.Num());
		UeAgentAuthoringJson::ApplyAuthoringMetadata(Body, TEXT("bone_name"), BodySetup->BoneName.ToString(), BodySetup->BoneName.ToString(), TEXT("Physics Asset|Bodies"), TEXT("physics_body"));

		TArray<TSharedPtr<FJsonValue>> Shapes;
		for (const FKSphereElem& SphereElem : BodySetup->AggGeom.SphereElems)
		{
			TSharedPtr<FJsonObject> Shape = MakeShared<FJsonObject>();
			Shape->SetStringField(TEXT("type"), TEXT("sphere"));
			Shape->SetNumberField(TEXT("radius"), SphereElem.Radius);
			Shape->SetObjectField(TEXT("transform"), UeAgentPhysicsMakeShapeTransformJson(SphereElem.Center, FRotator::ZeroRotator));
			UeAgentAuthoringJson::ApplyAuthoringMetadata(Shape, TEXT("shape_index"), FString::FromInt(Shapes.Num()), TEXT("Sphere"), TEXT("Physics Asset|Bodies|Shapes"), TEXT("physics_primitive_shape"));
			Shapes.Add(MakeShared<FJsonValueObject>(Shape));
		}
		for (const FKBoxElem& BoxElem : BodySetup->AggGeom.BoxElems)
		{
			TSharedPtr<FJsonObject> Shape = MakeShared<FJsonObject>();
			Shape->SetStringField(TEXT("type"), TEXT("box"));
			Shape->SetNumberField(TEXT("x"), BoxElem.X);
			Shape->SetNumberField(TEXT("y"), BoxElem.Y);
			Shape->SetNumberField(TEXT("z"), BoxElem.Z);
			Shape->SetObjectField(TEXT("transform"), UeAgentPhysicsMakeShapeTransformJson(BoxElem.Center, BoxElem.Rotation));
			UeAgentAuthoringJson::ApplyAuthoringMetadata(Shape, TEXT("shape_index"), FString::FromInt(Shapes.Num()), TEXT("Box"), TEXT("Physics Asset|Bodies|Shapes"), TEXT("physics_primitive_shape"));
			Shapes.Add(MakeShared<FJsonValueObject>(Shape));
		}
		for (const FKSphylElem& CapsuleElem : BodySetup->AggGeom.SphylElems)
		{
			TSharedPtr<FJsonObject> Shape = MakeShared<FJsonObject>();
			Shape->SetStringField(TEXT("type"), TEXT("capsule"));
			Shape->SetNumberField(TEXT("radius"), CapsuleElem.Radius);
			Shape->SetNumberField(TEXT("length"), CapsuleElem.Length);
			Shape->SetObjectField(TEXT("transform"), UeAgentPhysicsMakeShapeTransformJson(CapsuleElem.Center, CapsuleElem.Rotation));
			UeAgentAuthoringJson::ApplyAuthoringMetadata(Shape, TEXT("shape_index"), FString::FromInt(Shapes.Num()), TEXT("Capsule"), TEXT("Physics Asset|Bodies|Shapes"), TEXT("physics_primitive_shape"));
			Shapes.Add(MakeShared<FJsonValueObject>(Shape));
		}
		Body->SetArrayField(TEXT("primitive_shapes"), Shapes);
		return Body;
	}

	static TSharedPtr<FJsonObject> UeAgentPhysicsAssetReadbackToJson(UPhysicsAsset* PhysicsAsset)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), UeAgentPhysicsOps::PhysicsAssetSchema);
		Root->SetStringField(TEXT("asset_path"), UeAgentPhysicsOps::AssetPathForObject(PhysicsAsset));
		Root->SetStringField(TEXT("object_path"), PhysicsAsset ? PhysicsAsset->GetPathName() : FString());
		USkeletalMesh* PreviewMesh = PhysicsAsset ? PhysicsAsset->GetPreviewMesh() : nullptr;
		Root->SetStringField(TEXT("skeletal_mesh"), PreviewMesh ? UeAgentPhysicsOps::AssetPathForObject(PreviewMesh) : FString());

		TArray<TSharedPtr<FJsonValue>> Bodies;
		if (PhysicsAsset)
		{
			for (USkeletalBodySetup* BodySetup : PhysicsAsset->SkeletalBodySetups)
			{
				Bodies.Add(MakeShared<FJsonValueObject>(UeAgentPhysicsBodySetupToJson(BodySetup)));
			}
		}
		Root->SetArrayField(TEXT("bodies"), Bodies);
		Root->SetNumberField(TEXT("body_count"), Bodies.Num());

		TArray<TSharedPtr<FJsonValue>> Constraints;
		if (PhysicsAsset)
		{
			for (UPhysicsConstraintTemplate* ConstraintTemplate : PhysicsAsset->ConstraintSetup)
			{
				if (!ConstraintTemplate)
				{
					continue;
				}
				TSharedPtr<FJsonObject> Constraint = MakeShared<FJsonObject>();
				const FConstraintInstance& Instance = ConstraintTemplate->DefaultInstance;
				Constraint->SetStringField(TEXT("name"), Instance.JointName.ToString());
				Constraint->SetStringField(TEXT("child_body"), Instance.ConstraintBone1.ToString());
				Constraint->SetStringField(TEXT("parent_body"), Instance.ConstraintBone2.ToString());
				Constraint->SetObjectField(TEXT("constraint"), UeAgentPhysicsConstraintInstanceToJson(Instance));
				UeAgentAuthoringJson::ApplyAuthoringMetadata(Constraint, TEXT("name"), Instance.JointName.ToString(), Instance.JointName.ToString(), TEXT("Physics Asset|Constraints"), TEXT("physics_constraint"));
				Constraints.Add(MakeShared<FJsonValueObject>(Constraint));
			}
		}
		Root->SetArrayField(TEXT("constraints"), Constraints);
		Root->SetNumberField(TEXT("constraint_count"), Constraints.Num());

		TSharedPtr<FJsonObject> Profiles = MakeShared<FJsonObject>();
#if WITH_EDITORONLY_DATA
		TArray<TSharedPtr<FJsonValue>> ConstraintProfiles;
		TArray<TSharedPtr<FJsonValue>> PhysicalProfiles;
		if (PhysicsAsset)
		{
			for (const FName& ProfileName : PhysicsAsset->ConstraintProfiles)
			{
				ConstraintProfiles.Add(MakeShared<FJsonValueString>(ProfileName.ToString()));
			}
			for (const FName& ProfileName : PhysicsAsset->PhysicalAnimationProfiles)
			{
				PhysicalProfiles.Add(MakeShared<FJsonValueString>(ProfileName.ToString()));
			}
		}
		Profiles->SetArrayField(TEXT("constraint_profiles"), ConstraintProfiles);
		Profiles->SetArrayField(TEXT("physical_animation_profiles"), PhysicalProfiles);
#endif
		Root->SetObjectField(TEXT("profiles"), Profiles);
		return Root;
	}

	static bool UeAgentPhysicsValidateShape(const TSharedPtr<FJsonObject>& ShapeObj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!ShapeObj.IsValid())
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("json_invalid_item"), TEXT("Shape entry must be an object."), Path);
			return false;
		}
		FString Type;
		if (!ShapeObj->TryGetStringField(TEXT("type"), Type) || Type.TrimStartAndEnd().IsEmpty())
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), TEXT("Shape type is required."), Path + TEXT(".type"));
			return false;
		}
		Type = Type.ToLower();
		double Number = 0.0;
		if (Type == TEXT("sphere"))
		{
			if (!ShapeObj->TryGetNumberField(TEXT("radius"), Number) || Number <= 0.0)
			{
				UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("invalid_shape_dimension"), TEXT("sphere radius must be > 0."), Path + TEXT(".radius"));
			}
		}
		else if (Type == TEXT("box"))
		{
			for (const TCHAR* Key : { TEXT("x"), TEXT("y"), TEXT("z") })
			{
				if (!ShapeObj->TryGetNumberField(Key, Number) || Number <= 0.0)
				{
					UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("invalid_shape_dimension"), FString::Printf(TEXT("box %s must be > 0."), Key), FString::Printf(TEXT("%s.%s"), *Path, Key));
				}
			}
		}
		else if (Type == TEXT("capsule") || Type == TEXT("sphyl"))
		{
			if (!ShapeObj->TryGetNumberField(TEXT("radius"), Number) || Number <= 0.0)
			{
				UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("invalid_shape_dimension"), TEXT("capsule radius must be > 0."), Path + TEXT(".radius"));
			}
			if (!ShapeObj->TryGetNumberField(TEXT("length"), Number) || Number <= 0.0)
			{
				UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("invalid_shape_dimension"), TEXT("capsule length must be > 0."), Path + TEXT(".length"));
			}
		}
		else
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("unsupported_shape_type"), FString::Printf(TEXT("Shape type '%s' is not supported."), *Type), Path + TEXT(".type"));
		}
		return !UeAgentPhysicsOps::HasErrorIssue(Issues);
	}

	static bool UeAgentPhysicsValidateProfilesJson(const TSharedPtr<FJsonObject>& ProfilesJson, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!ProfilesJson.IsValid())
		{
			return true;
		}
		for (const TCHAR* ArrayKey : { TEXT("constraint_profiles"), TEXT("physical_animation_profiles") })
		{
			const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
			if (!ProfilesJson->TryGetArrayField(ArrayKey, Names) || !Names)
			{
				continue;
			}
			TSet<FString> Seen;
			for (int32 Index = 0; Index < Names->Num(); ++Index)
			{
				FString Name = (*Names)[Index].IsValid() ? (*Names)[Index]->AsString() : FString();
				Name.TrimStartAndEndInline();
				if (Name.IsEmpty())
				{
					UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("invalid_profile_name"), TEXT("Profile name cannot be empty."), FString::Printf(TEXT("profiles.hjson.%s[%d]"), ArrayKey, Index));
				}
				if (Seen.Contains(Name))
				{
					UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("duplicate_profile_name"), FString::Printf(TEXT("Profile '%s' is duplicated."), *Name), FString::Printf(TEXT("profiles.hjson.%s[%d]"), ArrayKey, Index));
				}
				Seen.Add(Name);
			}
		}
		return !UeAgentPhysicsOps::HasErrorIssue(Issues);
	}

	static bool UeAgentPhysicsLoadAndValidatePhysicsAssetFolder(const FString& FolderPathInput, bool bAllowDestructiveReplace, FUeAgentPhysicsAssetFolder& OutFolder, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		OutFolder = FUeAgentPhysicsAssetFolder();
		OutFolder.FolderPath = UeAgentPhysicsResolveFolderPath(FolderPathInput);
		if (OutFolder.FolderPath.IsEmpty())
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("missing_folder_path"), TEXT("folder_path is required."), TEXT("folder_path"));
			return false;
		}
		UeAgentPhysicsLoadFolderAuthoringFile(OutFolder.FolderPath, TEXT("asset.hjson"), true, OutFolder.AssetJson, Issues);
		UeAgentPhysicsLoadFolderAuthoringFile(OutFolder.FolderPath, TEXT("bodies.hjson"), true, OutFolder.BodiesJson, Issues);
		UeAgentPhysicsLoadFolderAuthoringFile(OutFolder.FolderPath, TEXT("constraints.hjson"), false, OutFolder.ConstraintsJson, Issues);
		UeAgentPhysicsLoadFolderAuthoringFile(OutFolder.FolderPath, TEXT("profiles.hjson"), false, OutFolder.ProfilesJson, Issues);
		if (UeAgentPhysicsOps::HasErrorIssue(Issues))
		{
			return false;
		}
		FString UnsupportedKey;
		FString UnsupportedPath;
		for (const TPair<TSharedPtr<FJsonObject>, FString> JsonWithPath : {
			TPair<TSharedPtr<FJsonObject>, FString>(OutFolder.AssetJson, TEXT("asset.hjson")),
			TPair<TSharedPtr<FJsonObject>, FString>(OutFolder.BodiesJson, TEXT("bodies.hjson")),
			TPair<TSharedPtr<FJsonObject>, FString>(OutFolder.ConstraintsJson, TEXT("constraints.hjson")),
			TPair<TSharedPtr<FJsonObject>, FString>(OutFolder.ProfilesJson, TEXT("profiles.hjson")) })
		{
			if (UeAgentPhysicsJsonHasUnsupportedOptionalField(JsonWithPath.Key, JsonWithPath.Value, UnsupportedKey, UnsupportedPath))
			{
				UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("unsupported_apply_profile"), FString::Printf(TEXT("Field '%s' requires an optional physics plugin and is outside baseline PhysicsAsset scope."), *UnsupportedKey), UnsupportedPath);
			}
		}

		OutFolder.AssetJson->TryGetStringField(TEXT("asset_path"), OutFolder.AssetPath);
		OutFolder.AssetJson->TryGetStringField(TEXT("skeletal_mesh"), OutFolder.SkeletalMeshPath);
		OutFolder.AssetPath.TrimStartAndEndInline();
		OutFolder.SkeletalMeshPath.TrimStartAndEndInline();
		if (OutFolder.AssetPath.IsEmpty() || !FPackageName::IsValidLongPackageName(UeAgentPhysicsOps::NormalizeAssetPath(OutFolder.AssetPath)))
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("invalid_asset_path"), TEXT("asset.hjson asset_path must be a valid long package name."), TEXT("asset.hjson.asset_path"));
		}
		if (OutFolder.SkeletalMeshPath.IsEmpty())
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_asset_missing_skeleton"), TEXT("asset.hjson skeletal_mesh is required."), TEXT("asset.hjson.skeletal_mesh"));
		}
		else
		{
			OutFolder.SkeletalMesh = UeAgentPhysicsOps::LoadAsset<USkeletalMesh>(OutFolder.SkeletalMeshPath);
			if (!OutFolder.SkeletalMesh)
			{
				UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_asset_missing_skeleton"), TEXT("skeletal_mesh could not be loaded."), TEXT("asset.hjson.skeletal_mesh"));
			}
		}

		bool bReplaceAll = false;
		OutFolder.BodiesJson->TryGetBoolField(TEXT("replace_all"), bReplaceAll);
		if (bReplaceAll && !bAllowDestructiveReplace)
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("destructive_replace_requires_confirmation"), TEXT("replace_all=true requires allow_destructive_replace=true."), TEXT("bodies.hjson.replace_all"));
		}
		const TArray<TSharedPtr<FJsonValue>>* Bodies = nullptr;
		if (!OutFolder.BodiesJson->TryGetArrayField(TEXT("bodies"), Bodies) || !Bodies)
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), TEXT("bodies.hjson requires bodies array."), TEXT("bodies.hjson.bodies"));
		}
		else
		{
			for (int32 Index = 0; Index < Bodies->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> BodyObj = (*Bodies)[Index].IsValid() ? (*Bodies)[Index]->AsObject() : nullptr;
				const FString BodyPath = FString::Printf(TEXT("bodies.hjson.bodies[%d]"), Index);
				if (!BodyObj.IsValid())
				{
					UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("json_invalid_item"), TEXT("Body entry must be an object."), BodyPath);
					continue;
				}
				FString BoneName;
				BodyObj->TryGetStringField(TEXT("bone_name"), BoneName);
				if (BoneName.TrimStartAndEnd().IsEmpty())
				{
					UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), TEXT("Body bone_name is required."), BodyPath + TEXT(".bone_name"));
				}
				else
				{
					const FName BoneFName(*BoneName);
					if (OutFolder.SkeletalMesh && OutFolder.SkeletalMesh->GetRefSkeleton().FindBoneIndex(BoneFName) == INDEX_NONE)
					{
						UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_asset_body_bone_missing"), FString::Printf(TEXT("Bone '%s' does not exist on skeletal_mesh."), *BoneName), BodyPath + TEXT(".bone_name"));
					}
					OutFolder.DeclaredBodyNames.Add(BoneFName);
				}
				FString PhysicsType;
				EPhysicsType ParsedType;
				if (BodyObj->TryGetStringField(TEXT("physics_type"), PhysicsType) && !UeAgentPhysicsParsePhysicsType(PhysicsType, ParsedType))
				{
					UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("invalid_physics_type"), TEXT("physics_type must be Default, Kinematic, or Simulated."), BodyPath + TEXT(".physics_type"));
				}
				const TArray<TSharedPtr<FJsonValue>>* Shapes = nullptr;
				if (BodyObj->TryGetArrayField(TEXT("primitive_shapes"), Shapes) && Shapes)
				{
					for (int32 ShapeIndex = 0; ShapeIndex < Shapes->Num(); ++ShapeIndex)
					{
						UeAgentPhysicsValidateShape((*Shapes)[ShapeIndex].IsValid() ? (*Shapes)[ShapeIndex]->AsObject() : nullptr, FString::Printf(TEXT("%s.primitive_shapes[%d]"), *BodyPath, ShapeIndex), Issues);
					}
				}
			}
		}

		if (OutFolder.ConstraintsJson.IsValid())
		{
			bool bReplaceConstraints = false;
			OutFolder.ConstraintsJson->TryGetBoolField(TEXT("replace_all"), bReplaceConstraints);
			if (bReplaceConstraints && !bAllowDestructiveReplace)
			{
				UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("destructive_replace_requires_confirmation"), TEXT("constraints replace_all=true requires allow_destructive_replace=true."), TEXT("constraints.hjson.replace_all"));
			}
			const TArray<TSharedPtr<FJsonValue>>* Constraints = nullptr;
			if (OutFolder.ConstraintsJson->TryGetArrayField(TEXT("constraints"), Constraints) && Constraints)
			{
				for (int32 Index = 0; Index < Constraints->Num(); ++Index)
				{
					const TSharedPtr<FJsonObject> ConstraintObj = (*Constraints)[Index].IsValid() ? (*Constraints)[Index]->AsObject() : nullptr;
					const FString ConstraintPath = FString::Printf(TEXT("constraints.hjson.constraints[%d]"), Index);
					if (!ConstraintObj.IsValid())
					{
						UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("json_invalid_item"), TEXT("Constraint entry must be an object."), ConstraintPath);
						continue;
					}
					FString ParentBody;
					FString ChildBody;
					ConstraintObj->TryGetStringField(TEXT("parent_body"), ParentBody);
					ConstraintObj->TryGetStringField(TEXT("child_body"), ChildBody);
					if (!OutFolder.DeclaredBodyNames.Contains(FName(*ParentBody)))
					{
						UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_asset_constraint_body_missing"), TEXT("parent_body is not declared in bodies.hjson."), ConstraintPath + TEXT(".parent_body"));
					}
					if (!OutFolder.DeclaredBodyNames.Contains(FName(*ChildBody)))
					{
						UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_asset_constraint_body_missing"), TEXT("child_body is not declared in bodies.hjson."), ConstraintPath + TEXT(".child_body"));
					}
					TSharedPtr<FJsonObject> ConstraintSettings = MakeShared<FJsonObject>();
					const TSharedPtr<FJsonObject>* Linear = nullptr;
					if (ConstraintObj->TryGetObjectField(TEXT("linear"), Linear) && Linear)
					{
						ConstraintSettings->SetObjectField(TEXT("linear"), *Linear);
					}
					const TSharedPtr<FJsonObject>* Angular = nullptr;
					if (ConstraintObj->TryGetObjectField(TEXT("angular"), Angular) && Angular)
					{
						ConstraintSettings->SetObjectField(TEXT("angular"), *Angular);
					}
					UeAgentPhysicsValidateConstraintSettings(ConstraintSettings, Issues);
				}
			}
		}
		UeAgentPhysicsValidateProfilesJson(OutFolder.ProfilesJson, Issues);
		return !UeAgentPhysicsOps::HasErrorIssue(Issues);
	}

	static UPhysicsAsset* UeAgentPhysicsCreateOrLoadPhysicsAsset(const FString& AssetPathInput, USkeletalMesh* SkeletalMesh, bool bCreateIfMissing, bool& bOutCreated, FString& OutError)
	{
		bOutCreated = false;
		if (UPhysicsAsset* Existing = UeAgentPhysicsOps::LoadAsset<UPhysicsAsset>(AssetPathInput))
		{
			if (SkeletalMesh)
			{
				Existing->SetPreviewMesh(SkeletalMesh, true);
			}
			return Existing;
		}
		if (!bCreateIfMissing)
		{
			OutError = TEXT("physics_asset_not_found");
			return nullptr;
		}
		const FString AssetPath = UeAgentPhysicsOps::NormalizeAssetPath(AssetPathInput);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return nullptr;
		}
		UPackage* Package = CreatePackage(*AssetPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPhysicsAsset* PhysicsAsset = NewObject<UPhysicsAsset>(Package, UPhysicsAsset::StaticClass(), *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!PhysicsAsset)
		{
			OutError = TEXT("create_physics_asset_failed");
			return nullptr;
		}
		if (SkeletalMesh)
		{
			PhysicsAsset->SetPreviewMesh(SkeletalMesh, true);
		}
		FAssetRegistryModule::AssetCreated(PhysicsAsset);
		PhysicsAsset->MarkPackageDirty();
		bOutCreated = true;
		return PhysicsAsset;
	}

	static USkeletalBodySetup* UeAgentPhysicsFindOrCreateBodySetup(UPhysicsAsset* PhysicsAsset, FName BoneName, bool& bOutCreated)
	{
		bOutCreated = false;
		if (!PhysicsAsset)
		{
			return nullptr;
		}
		for (USkeletalBodySetup* BodySetup : PhysicsAsset->SkeletalBodySetups)
		{
			if (BodySetup && BodySetup->BoneName == BoneName)
			{
				return BodySetup;
			}
		}
		const FName ObjectName = MakeUniqueObjectName(PhysicsAsset, USkeletalBodySetup::StaticClass(), BoneName);
		USkeletalBodySetup* BodySetup = NewObject<USkeletalBodySetup>(PhysicsAsset, USkeletalBodySetup::StaticClass(), ObjectName, RF_Transactional);
		if (BodySetup)
		{
			BodySetup->BoneName = BoneName;
			PhysicsAsset->SkeletalBodySetups.Add(BodySetup);
			bOutCreated = true;
		}
		return BodySetup;
	}

	static bool UeAgentPhysicsApplyBodyShapes(USkeletalBodySetup* BodySetup, const TSharedPtr<FJsonObject>& BodyObj, FString& OutError)
	{
		if (!BodySetup || !BodyObj.IsValid())
		{
			OutError = TEXT("invalid_body_setup_apply_target");
			return false;
		}
		FString PhysicsTypeText;
		EPhysicsType PhysicsType = PhysType_Default;
		if (BodyObj->TryGetStringField(TEXT("physics_type"), PhysicsTypeText) && !UeAgentPhysicsParsePhysicsType(PhysicsTypeText, PhysicsType))
		{
			OutError = TEXT("invalid_physics_type");
			return false;
		}
		BodySetup->PhysicsType = PhysicsType;
		BodySetup->CollisionTraceFlag = CTF_UseDefault;
		BodySetup->CollisionReponse = EBodyCollisionResponse::BodyCollision_Enabled;
		BodySetup->AggGeom.EmptyElements();
		const TArray<TSharedPtr<FJsonValue>>* Shapes = nullptr;
		if (BodyObj->TryGetArrayField(TEXT("primitive_shapes"), Shapes) && Shapes)
		{
			for (const TSharedPtr<FJsonValue>& ShapeValue : *Shapes)
			{
				const TSharedPtr<FJsonObject> ShapeObj = ShapeValue.IsValid() ? ShapeValue->AsObject() : nullptr;
				if (!ShapeObj.IsValid())
				{
					continue;
				}
				FString Type;
				ShapeObj->TryGetStringField(TEXT("type"), Type);
				Type = Type.ToLower();
				FVector Location = FVector::ZeroVector;
				FRotator Rotation = FRotator::ZeroRotator;
				UeAgentPhysicsReadShapeTransform(ShapeObj, Location, Rotation);
				double Number = 0.0;
				if (Type == TEXT("sphere"))
				{
					FKSphereElem SphereElem;
					ShapeObj->TryGetNumberField(TEXT("radius"), Number);
					SphereElem.Radius = FMath::Max(1.0f, static_cast<float>(Number));
					SphereElem.Center = Location;
					BodySetup->AggGeom.SphereElems.Add(SphereElem);
				}
				else if (Type == TEXT("box"))
				{
					FKBoxElem BoxElem;
					ShapeObj->TryGetNumberField(TEXT("x"), Number);
					BoxElem.X = FMath::Max(1.0f, static_cast<float>(Number));
					ShapeObj->TryGetNumberField(TEXT("y"), Number);
					BoxElem.Y = FMath::Max(1.0f, static_cast<float>(Number));
					ShapeObj->TryGetNumberField(TEXT("z"), Number);
					BoxElem.Z = FMath::Max(1.0f, static_cast<float>(Number));
					BoxElem.Center = Location;
					BoxElem.Rotation = Rotation;
					BodySetup->AggGeom.BoxElems.Add(BoxElem);
				}
				else if (Type == TEXT("capsule") || Type == TEXT("sphyl"))
				{
					FKSphylElem CapsuleElem;
					ShapeObj->TryGetNumberField(TEXT("radius"), Number);
					CapsuleElem.Radius = FMath::Max(1.0f, static_cast<float>(Number));
					ShapeObj->TryGetNumberField(TEXT("length"), Number);
					CapsuleElem.Length = FMath::Max(1.0f, static_cast<float>(Number));
					CapsuleElem.Center = Location;
					CapsuleElem.Rotation = Rotation;
					BodySetup->AggGeom.SphylElems.Add(CapsuleElem);
				}
			}
		}
		BodySetup->BodySetupGuid = FGuid::NewGuid();
		BodySetup->InvalidatePhysicsData();
		return true;
	}

	static void UeAgentPhysicsApplyConstraintInstanceFromJson(FConstraintInstance& Instance, const TSharedPtr<FJsonObject>& ConstraintObj)
	{
		if (!ConstraintObj.IsValid())
		{
			return;
		}
		bool bDisableCollision = false;
		if (ConstraintObj->TryGetBoolField(TEXT("disable_collision"), bDisableCollision))
		{
			Instance.SetDisableCollision(bDisableCollision);
		}
		const TSharedPtr<FJsonObject>* Linear = nullptr;
		if (ConstraintObj->TryGetObjectField(TEXT("linear"), Linear) && Linear && Linear->IsValid())
		{
			double Limit = Instance.ProfileInstance.LinearLimit.Limit;
			(*Linear)->TryGetNumberField(TEXT("limit"), Limit);
			FString MotionText;
			ELinearConstraintMotion XMotion = Instance.GetLinearXMotion();
			ELinearConstraintMotion YMotion = Instance.GetLinearYMotion();
			ELinearConstraintMotion ZMotion = Instance.GetLinearZMotion();
			if ((*Linear)->TryGetStringField(TEXT("x_motion"), MotionText))
			{
				UeAgentPhysicsOps::ParseLinearMotion(MotionText, XMotion);
			}
			if ((*Linear)->TryGetStringField(TEXT("y_motion"), MotionText))
			{
				UeAgentPhysicsOps::ParseLinearMotion(MotionText, YMotion);
			}
			if ((*Linear)->TryGetStringField(TEXT("z_motion"), MotionText))
			{
				UeAgentPhysicsOps::ParseLinearMotion(MotionText, ZMotion);
			}
			Instance.SetLinearLimits(XMotion, YMotion, ZMotion, static_cast<float>(Limit));
		}
		const TSharedPtr<FJsonObject>* Angular = nullptr;
		if (ConstraintObj->TryGetObjectField(TEXT("angular"), Angular) && Angular && Angular->IsValid())
		{
			FString MotionText;
			EAngularConstraintMotion Motion;
			double Limit = 0.0;
			if ((*Angular)->TryGetStringField(TEXT("swing1_motion"), MotionText) && UeAgentPhysicsOps::ParseAngularMotion(MotionText, Motion))
			{
				Limit = Instance.GetAngularSwing1Limit();
				(*Angular)->TryGetNumberField(TEXT("swing1_limit_degrees"), Limit);
				Instance.SetAngularSwing1Limit(Motion, static_cast<float>(Limit));
			}
			if ((*Angular)->TryGetStringField(TEXT("swing2_motion"), MotionText) && UeAgentPhysicsOps::ParseAngularMotion(MotionText, Motion))
			{
				Limit = Instance.GetAngularSwing2Limit();
				(*Angular)->TryGetNumberField(TEXT("swing2_limit_degrees"), Limit);
				Instance.SetAngularSwing2Limit(Motion, static_cast<float>(Limit));
			}
			if ((*Angular)->TryGetStringField(TEXT("twist_motion"), MotionText) && UeAgentPhysicsOps::ParseAngularMotion(MotionText, Motion))
			{
				Limit = Instance.GetAngularTwistLimit();
				(*Angular)->TryGetNumberField(TEXT("twist_limit_degrees"), Limit);
				Instance.SetAngularTwistLimit(Motion, static_cast<float>(Limit));
			}
		}
		const TSharedPtr<FJsonObject>* Projection = nullptr;
		if (ConstraintObj->TryGetObjectField(TEXT("projection"), Projection) && Projection && Projection->IsValid())
		{
			bool bProjection = false;
			if ((*Projection)->TryGetBoolField(TEXT("enabled"), bProjection))
			{
				Instance.ProfileInstance.bEnableProjection = bProjection;
			}
		}
	}
}

bool FUeAgentLegacyMechanics::CmdPhysicsAssetExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentReflectionAuthoringCommandBridge::Export(TEXT("physics_asset"), Ctx.Params, OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdPhysicsAssetValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentReflectionAuthoringCommandBridge::Validate(TEXT("physics_asset"), Ctx.Params, OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdPhysicsAssetApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentReflectionAuthoringCommandBridge::Apply(TEXT("physics_asset"), Ctx.Params, OutData, OutError, Ctx.RequestId);
}

bool FUeAgentLegacyMechanics::CmdPhysicsRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	double DurationSeconds = 0.1;
	double FixedDeltaSeconds = 1.0 / 60.0;
	JsonTryGetNumber(Ctx.Params, TEXT("duration_seconds"), DurationSeconds);
	JsonTryGetNumber(Ctx.Params, TEXT("fixed_delta_seconds"), FixedDeltaSeconds);
	DurationSeconds = FMath::Clamp(DurationSeconds, 0.0, 5.0);
	FixedDeltaSeconds = FMath::Clamp(FixedDeltaSeconds, 0.001, 0.05);
	const int32 TickCount = FMath::Clamp(FMath::CeilToInt(DurationSeconds / FixedDeltaSeconds), 0, 300);

	const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("targets"), Targets) || !Targets || Targets->Num() == 0)
	{
		OutError = TEXT("missing_targets");
		return false;
	}

	struct FProbeTargetState
	{
		FString ActorId;
		FString ComponentName;
		AActor* Actor = nullptr;
		UPrimitiveComponent* Component = nullptr;
		FTransform OriginalTransform;
		FVector OriginalLinearVelocity = FVector::ZeroVector;
		FVector OriginalAngularVelocity = FVector::ZeroVector;
		FVector StartLocation = FVector::ZeroVector;
	};

	TArray<FProbeTargetState> States;
	TArray<TSharedPtr<FJsonValue>> Samples;
	TArray<TSharedPtr<FJsonValue>> Issues;
	for (int32 Index = 0; Index < Targets->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> TargetObj = (*Targets)[Index].IsValid() ? (*Targets)[Index]->AsObject() : nullptr;
		if (!TargetObj.IsValid())
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("invalid_probe_target"), TEXT("Probe target must be an object."), FString::Printf(TEXT("targets[%d]"), Index));
			continue;
		}
		FString ActorId;
		FString ComponentName;
		TargetObj->TryGetStringField(TEXT("actor_id"), ActorId);
		TargetObj->TryGetStringField(TEXT("actor"), ActorId);
		TargetObj->TryGetStringField(TEXT("component"), ComponentName);
		ActorId.TrimStartAndEndInline();
		AActor* Actor = UeAgentPhysicsOps::FindActorInWorld(World, ActorId);
		UPrimitiveComponent* Component = Actor ? UeAgentPhysicsOps::FindPrimitiveComponent(Actor, ComponentName) : nullptr;
		if (!Actor || !Component)
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_component_unresolved"), TEXT("Probe target actor/component could not be resolved."), FString::Printf(TEXT("targets[%d]"), Index));
			continue;
		}
		if (!Component->IsRegistered() || !Component->IsSimulatingPhysics())
		{
			UeAgentPhysicsOps::AddIssue(Issues, TEXT("error"), TEXT("physics_probe_unstable_world"), TEXT("Probe target must be registered and simulating physics."), FString::Printf(TEXT("targets[%d]"), Index));
			continue;
		}
		FProbeTargetState State;
		State.ActorId = ActorId;
		State.ComponentName = ComponentName.IsEmpty() ? Component->GetName() : ComponentName;
		State.Actor = Actor;
		State.Component = Component;
		State.OriginalTransform = Actor->GetActorTransform();
		State.OriginalLinearVelocity = Component->GetPhysicsLinearVelocity();
		State.OriginalAngularVelocity = Component->GetPhysicsAngularVelocityInDegrees();
		State.StartLocation = Actor->GetActorLocation();
		States.Add(State);

		const TArray<TSharedPtr<FJsonValue>>* ImpulseArray = nullptr;
		if (TargetObj->TryGetArrayField(TEXT("initial_impulse"), ImpulseArray))
		{
			FVector Impulse = FVector::ZeroVector;
			if (UeAgentPhysicsOps::JsonArrayToVector(ImpulseArray, Impulse))
			{
				Component->WakeRigidBody();
				Component->AddImpulse(Impulse, NAME_None, true);
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* InitialVelocityArray = nullptr;
		if (TargetObj->TryGetArrayField(TEXT("initial_velocity"), InitialVelocityArray))
		{
			FVector InitialVelocity = FVector::ZeroVector;
			if (UeAgentPhysicsOps::JsonArrayToVector(InitialVelocityArray, InitialVelocity))
			{
				Component->WakeRigidBody();
				Component->SetPhysicsLinearVelocity(InitialVelocity);
			}
		}
	}

	if (UeAgentPhysicsOps::HasErrorIssue(Issues))
	{
		OutData->SetStringField(TEXT("schema"), UeAgentPhysicsOps::PhysicsProbeSchema);
		OutData->SetArrayField(TEXT("issues"), Issues);
		OutError = TEXT("physics_probe_unstable_world");
		return false;
	}

	for (int32 TickIndex = 0; TickIndex < TickCount; ++TickIndex)
	{
		World->Tick(ELevelTick::LEVELTICK_All, static_cast<float>(FixedDeltaSeconds));
	}

	TMap<FString, TSharedPtr<FJsonObject>> SampleByKey;
	for (const FProbeTargetState& State : States)
	{
		TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
		Sample->SetStringField(TEXT("actor_id"), State.ActorId);
		Sample->SetStringField(TEXT("component"), State.ComponentName);
		const FVector EndLocation = State.Actor ? State.Actor->GetActorLocation() : FVector::ZeroVector;
		const FVector LinearVelocity = State.Component ? State.Component->GetPhysicsLinearVelocity() : FVector::ZeroVector;
		Sample->SetObjectField(TEXT("start_location"), UeAgentPhysicsOps::VecToJson(State.StartLocation));
		Sample->SetObjectField(TEXT("location"), UeAgentPhysicsOps::VecToJson(EndLocation));
		Sample->SetObjectField(TEXT("velocity"), UeAgentPhysicsOps::VecToJson(LinearVelocity));
		Sample->SetNumberField(TEXT("z_delta"), EndLocation.Z - State.StartLocation.Z);
		Sample->SetNumberField(TEXT("velocity_size"), LinearVelocity.Size());
		if (FBodyInstance* Body = State.Component ? State.Component->GetBodyInstance() : nullptr)
		{
			Sample->SetBoolField(TEXT("awake"), Body->IsInstanceAwake());
		}
		Samples.Add(MakeShared<FJsonValueObject>(Sample));
		SampleByKey.Add(State.ActorId + TEXT("|") + State.ComponentName, Sample);
	}

	TArray<TSharedPtr<FJsonValue>> ExpectationResults;
	const TArray<TSharedPtr<FJsonValue>>* Expectations = nullptr;
	bool bExpectationsPassed = true;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("expect"), Expectations) && Expectations)
	{
		for (int32 Index = 0; Index < Expectations->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> ExpectObj = (*Expectations)[Index].IsValid() ? (*Expectations)[Index]->AsObject() : nullptr;
			if (!ExpectObj.IsValid())
			{
				continue;
			}
			FString ActorId;
			FString ComponentName;
			ExpectObj->TryGetStringField(TEXT("actor_id"), ActorId);
			ExpectObj->TryGetStringField(TEXT("actor"), ActorId);
			ExpectObj->TryGetStringField(TEXT("component"), ComponentName);
			if (ComponentName.IsEmpty())
			{
				ComponentName = TEXT("StaticMeshComponent");
			}
			TSharedPtr<FJsonObject>* SamplePtr = SampleByKey.Find(ActorId + TEXT("|") + ComponentName);
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("actor_id"), ActorId);
			Result->SetStringField(TEXT("component"), ComponentName);
			Result->SetBoolField(TEXT("sample_found"), SamplePtr != nullptr);
			bool bPassed = SamplePtr != nullptr;
			if (SamplePtr && SamplePtr->IsValid())
			{
				double MinZDelta = 0.0;
				if (ExpectObj->TryGetNumberField(TEXT("min_z_delta"), MinZDelta))
				{
					const double Actual = (*SamplePtr)->GetNumberField(TEXT("z_delta"));
					const bool bCheckPassed = Actual >= MinZDelta;
					Result->SetNumberField(TEXT("z_delta"), Actual);
					Result->SetNumberField(TEXT("min_z_delta"), MinZDelta);
					bPassed = bPassed && bCheckPassed;
				}
				double MinVelocity = 0.0;
				if (ExpectObj->TryGetNumberField(TEXT("min_velocity_size"), MinVelocity))
				{
					const double Actual = (*SamplePtr)->GetNumberField(TEXT("velocity_size"));
					const bool bCheckPassed = Actual >= MinVelocity;
					Result->SetNumberField(TEXT("velocity_size"), Actual);
					Result->SetNumberField(TEXT("min_velocity_size"), MinVelocity);
					bPassed = bPassed && bCheckPassed;
				}
			}
			Result->SetBoolField(TEXT("passed"), bPassed);
			ExpectationResults.Add(MakeShared<FJsonValueObject>(Result));
			bExpectationsPassed = bExpectationsPassed && bPassed;
		}
	}

	FString Cleanup = TEXT("restore_transforms");
	JsonTryGetString(Ctx.Params, TEXT("cleanup"), Cleanup);
	bool bRestored = false;
	if (Cleanup.Equals(TEXT("restore_transforms"), ESearchCase::IgnoreCase) || Cleanup.Equals(TEXT("restore"), ESearchCase::IgnoreCase))
	{
		for (const FProbeTargetState& State : States)
		{
			if (State.Actor && State.Component)
			{
				State.Actor->SetActorTransform(State.OriginalTransform, false, nullptr, ETeleportType::TeleportPhysics);
				State.Component->SetPhysicsLinearVelocity(State.OriginalLinearVelocity);
				State.Component->SetPhysicsAngularVelocityInDegrees(State.OriginalAngularVelocity);
				State.Component->WakeRigidBody();
			}
		}
		bRestored = true;
	}

	OutData->SetStringField(TEXT("schema"), UeAgentPhysicsOps::PhysicsProbeSchema);
	OutData->SetStringField(TEXT("world_tick_mode"), TickCount > 0 ? TEXT("editor_world_tick") : TEXT("impulse_readback_only"));
	OutData->SetNumberField(TEXT("duration_seconds"), DurationSeconds);
	OutData->SetNumberField(TEXT("fixed_delta_seconds"), FixedDeltaSeconds);
	OutData->SetNumberField(TEXT("tick_count"), TickCount);
	OutData->SetArrayField(TEXT("samples"), Samples);
	OutData->SetArrayField(TEXT("expectation_results"), ExpectationResults);
	OutData->SetBoolField(TEXT("expectations_passed"), bExpectationsPassed);
	OutData->SetBoolField(TEXT("restored"), bRestored);
	OutData->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
	if (!bExpectationsPassed)
	{
		OutError = TEXT("physics_probe_expectation_failed");
		return false;
	}
	return true;
}
