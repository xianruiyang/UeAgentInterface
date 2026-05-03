// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"

#include "Utilities/UeAgentJsonDiagnostics.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "DataLayer/DataLayerFactory.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorLevelUtils.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingAlwaysLoaded.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerInstanceWithAsset.h"
#include "WorldPartition/DataLayer/DataLayerUtils.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/ErrorHandling/WorldPartitionStreamingGenerationErrorHandler.h"
#include "WorldPartition/HLOD/HLODActor.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/HLOD/HLODLayerFactory.h"
#include "WorldPartition/HLOD/HLODModifier.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterActorList.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterShape.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstanceViewInterface.h"
#include "WorldPartition/WorldPartitionActorLoaderInterface.h"

namespace UeAgentLevelTopology
{
	static constexpr const TCHAR* TopologySchema = TEXT("uai.level_topology.v1");
	static constexpr const TCHAR* HLODLayerSchema = TEXT("uai.hlod_layer.v1");

	struct FStoredWorldPartitionLoader
	{
		FString LoaderId;
		FString Kind;
		FString Label;
		FString WorldPath;
		TUniquePtr<IWorldPartitionActorLoaderInterface::ILoaderAdapter> Adapter;
	};

	static TMap<FString, TUniquePtr<FStoredWorldPartitionLoader>>& GetLoaderRegistry()
	{
		static TMap<FString, TUniquePtr<FStoredWorldPartitionLoader>> Registry;
		return Registry;
	}

	static TMap<FString, UObject*>& GetSessionAssetRegistry()
	{
		static TMap<FString, UObject*> Registry;
		return Registry;
	}

	static FString NormalizePackagePath(const FString& InPath)
	{
		FString Path = InPath;
		Path.TrimStartAndEndInline();
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		int32 DotIndex = INDEX_NONE;
		if (Path.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			Path = Path.Left(DotIndex);
		}
		return Path;
	}

	static FString ToObjectPath(const FString& InPath)
	{
		const FString PackagePath = NormalizePackagePath(InPath);
		if (PackagePath.IsEmpty())
		{
			return FString();
		}
		if (InPath.Contains(TEXT(".")))
		{
			return InPath;
		}
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
		return AssetName.IsEmpty() ? FString() : FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
	}

	static UObject* LoadObjectByPath(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		const FString PackagePath = NormalizePackagePath(InPath);
		if (UObject** SessionAsset = GetSessionAssetRegistry().Find(PackagePath))
		{
			if (IsValid(*SessionAsset))
			{
				return *SessionAsset;
			}
			GetSessionAssetRegistry().Remove(PackagePath);
		}
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		if (!PackagePath.StartsWith(TEXT("/Script/")) && !FPackageName::DoesPackageExist(PackagePath))
		{
			return nullptr;
		}
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	template<typename T>
	static T* LoadTypedAsset(const FString& InPath)
	{
		return Cast<T>(LoadObjectByPath(InPath));
	}

	static bool AssetExists(const FString& InPath)
	{
		if (LoadObjectByPath(InPath))
		{
			return true;
		}
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return false;
		}
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		return AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
	}

	static bool IsValidLongPackagePath(const FString& InPath)
	{
		const FString Path = NormalizePackagePath(InPath);
		return !Path.IsEmpty() && FPackageName::IsValidLongPackageName(Path);
	}

	static FString ObjectPackagePath(UObject* Object)
	{
		return Object && Object->GetOutermost() ? Object->GetOutermost()->GetName() : FString();
	}

	static FString JsonToString(const TSharedPtr<FJsonObject>& Obj, bool bPretty = true)
	{
		FString Out;
		if (!Obj.IsValid())
		{
			return Out;
		}
		if (bPretty)
		{
			TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		}
		else
		{
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		}
		return Out;
	}

	static bool ParseJsonObject(const FString& Text, TSharedPtr<FJsonObject>& OutObj, FString& OutError)
	{
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
		{
			OutError = TEXT("json_parse_failed");
			return false;
		}
		return true;
	}

	static FString ResolveFilePath(const FString& InPath)
	{
		return UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(InPath);
	}

	static bool LoadJsonFromParams(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutRoot, FString& OutSource, FString& OutError)
	{
		OutRoot.Reset();
		OutSource = TEXT("params");
		if (!Params.IsValid())
		{
			OutError = TEXT("missing_params");
			return false;
		}

		const TSharedPtr<FJsonObject>* JsonObj = nullptr;
		const TCHAR* ObjectKeys[] = { TEXT("json"), TEXT("topology"), TEXT("content") };
		for (const TCHAR* Key : ObjectKeys)
		{
			if (Params->TryGetObjectField(Key, JsonObj) && JsonObj && JsonObj->IsValid())
			{
				OutRoot = *JsonObj;
				OutSource = Key;
				return true;
			}
		}

		FString JsonText;
		if (Params->TryGetStringField(TEXT("json_text"), JsonText) && !JsonText.TrimStartAndEnd().IsEmpty())
		{
			OutSource = TEXT("json_text");
			return ParseJsonObject(JsonText, OutRoot, OutError);
		}

		FString JsonFile;
		if (Params->TryGetStringField(TEXT("json_file"), JsonFile) || Params->TryGetStringField(TEXT("input_file"), JsonFile))
		{
			JsonFile = ResolveFilePath(JsonFile);
			if (!FFileHelper::LoadFileToString(JsonText, *JsonFile))
			{
				OutError = TEXT("json_file_read_failed");
				return false;
			}
			OutSource = JsonFile;
			return ParseJsonObject(JsonText, OutRoot, OutError);
		}

		OutError = TEXT("missing_json");
		return false;
	}

	static bool SaveJsonIfRequested(const TSharedPtr<FJsonObject>& Obj, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, FString& OutError)
	{
		if (!Obj.IsValid() || !Params.IsValid())
		{
			return true;
		}
		FString OutputFile;
		if (!Params->TryGetStringField(TEXT("output_file"), OutputFile) || OutputFile.TrimStartAndEnd().IsEmpty())
		{
			return true;
		}
		OutputFile = ResolveFilePath(OutputFile);
		const FString OutputDir = FPaths::GetPath(OutputFile);
		if (!OutputDir.IsEmpty())
		{
			IFileManager::Get().MakeDirectory(*OutputDir, true);
		}
		if (!FFileHelper::SaveStringToFile(JsonToString(Obj, true), *OutputFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = TEXT("output_file_write_failed");
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), OutputFile);
		return true;
	}

	static TSharedPtr<FJsonObject> MakeIssue(const FString& Severity, const FString& Code, const FString& Path, const FString& Message)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), Severity);
		Issue->SetStringField(TEXT("code"), Code);
		Issue->SetStringField(TEXT("path"), Path);
		Issue->SetStringField(TEXT("message"), Message);
		return Issue;
	}

	static void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Severity, const FString& Code, const FString& Path, const FString& Message)
	{
		Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(Severity, Code, Path, Message)));
	}

	static int32 CountIssues(const TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Severity)
	{
		int32 Count = 0;
		for (const TSharedPtr<FJsonValue>& Value : Issues)
		{
			const TSharedPtr<FJsonObject> Issue = Value.IsValid() ? Value->AsObject() : nullptr;
			FString IssueSeverity;
			if (Issue.IsValid() && Issue->TryGetStringField(TEXT("severity"), IssueSeverity) && IssueSeverity.Equals(Severity, ESearchCase::IgnoreCase))
			{
				++Count;
			}
		}
		return Count;
	}

	static TArray<FString> ReadStringArray(const TSharedPtr<FJsonObject>& Obj, const FString& Key)
	{
		TArray<FString> Result;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetArrayField(Key, Values) || !Values)
		{
			return Result;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid())
			{
				continue;
			}
			if (Value->Type == EJson::String)
			{
				Result.Add(Value->AsString());
			}
		}
		return Result;
	}

	static TSharedPtr<FJsonObject> MakeVectorJson(const FVector& V)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), V.X);
		Obj->SetNumberField(TEXT("y"), V.Y);
		Obj->SetNumberField(TEXT("z"), V.Z);
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& R)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), R.Pitch);
		Obj->SetNumberField(TEXT("yaw"), R.Yaw);
		Obj->SetNumberField(TEXT("roll"), R.Roll);
		return Obj;
	}

	static bool TryReadVectorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonValue>* Value = UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
		return Value && Value->IsValid() && UeAgentJsonDiagnostics::TryReadVectorValue(*Value, OutValue);
	}

	static bool TryReadRotatorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FRotator& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonValue>* Value = UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
		return Value && Value->IsValid() && UeAgentJsonDiagnostics::TryReadRotatorValue(*Value, OutValue);
	}

	static AActor* FindActorByNameOrLabel(UWorld* World, const FString& NameOrLabel)
	{
		if (!World || NameOrLabel.IsEmpty())
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
			if (Actor->GetName().Equals(NameOrLabel, ESearchCase::IgnoreCase)
				|| Actor->GetActorLabel().Equals(NameOrLabel, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
		}
		return nullptr;
	}

	static bool TryReadBox(const TSharedPtr<FJsonObject>& Obj, FBox& OutBox)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		FVector Center;
		FVector Extent;
		if (TryReadVectorField(Obj, TEXT("center"), Center) && TryReadVectorField(Obj, TEXT("extent"), Extent))
		{
			OutBox = FBox::BuildAABB(Center, Extent);
			return true;
		}
		FVector Min;
		FVector Max;
		if (TryReadVectorField(Obj, TEXT("min"), Min) && TryReadVectorField(Obj, TEXT("max"), Max))
		{
			OutBox = FBox(Min, Max);
			return OutBox.IsValid != 0;
		}
		return false;
	}

	static TSharedPtr<FJsonObject> BoxToJson(const FBox& Box)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		const bool bIsValid = Box.IsValid != 0;
		Obj->SetBoolField(TEXT("valid"), bIsValid);
		if (bIsValid)
		{
			Obj->SetObjectField(TEXT("min"), MakeVectorJson(Box.Min));
			Obj->SetObjectField(TEXT("max"), MakeVectorJson(Box.Max));
			Obj->SetObjectField(TEXT("center"), MakeVectorJson(Box.GetCenter()));
			Obj->SetObjectField(TEXT("extent"), MakeVectorJson(Box.GetExtent()));
		}
		return Obj;
	}

	static bool TryReadTransform(const TSharedPtr<FJsonObject>& Obj, FTransform& OutTransform)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		FVector Scale = FVector::OneVector;
		bool bAny = false;
		bAny |= TryReadVectorField(Obj, TEXT("location"), Location);
		bAny |= TryReadRotatorField(Obj, TEXT("rotation"), Rotation);
		bAny |= TryReadVectorField(Obj, TEXT("scale"), Scale);
		if (bAny)
		{
			OutTransform = FTransform(Rotation, Location, Scale);
		}
		return bAny;
	}

	static TSharedPtr<FJsonObject> TransformToJson(const FTransform& Transform)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("location"), MakeVectorJson(Transform.GetLocation()));
		Obj->SetObjectField(TEXT("rotation"), MakeRotatorJson(Transform.Rotator()));
		Obj->SetObjectField(TEXT("scale"), MakeVectorJson(Transform.GetScale3D()));
		return Obj;
	}

	static bool SavePackages(const TArray<UPackage*>& Packages, TArray<FString>& OutSavedPackages, FString& OutError)
	{
		TArray<UPackage*> ValidPackages;
		for (UPackage* Package : Packages)
		{
			if (Package)
			{
				ValidPackages.AddUnique(Package);
			}
		}
		if (ValidPackages.IsEmpty())
		{
			return true;
		}
		if (!UEditorLoadingAndSavingUtils::SavePackages(ValidPackages, false))
		{
			OutError = TEXT("save_packages_failed");
			return false;
		}
		for (UPackage* Package : ValidPackages)
		{
			OutSavedPackages.Add(Package->GetName());
		}
		return true;
	}

	static UClass* ResolveStreamingClass(const FString& ClassPath)
	{
		if (ClassPath.TrimStartAndEnd().IsEmpty())
		{
			return ULevelStreamingDynamic::StaticClass();
		}
		UObject* Obj = LoadObjectByPath(ClassPath);
		UClass* Class = Cast<UClass>(Obj);
		if (!Class)
		{
			Class = FindObject<UClass>(nullptr, *ClassPath);
		}
		return Class && Class->IsChildOf(ULevelStreaming::StaticClass()) ? Class : nullptr;
	}

	static ULevelStreaming* FindStreamingLevel(UWorld* World, const FString& LevelPackageOrName)
	{
		if (!World)
		{
			return nullptr;
		}
		const FString Needle = NormalizePackagePath(LevelPackageOrName);
		for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
		{
			if (!StreamingLevel)
			{
				continue;
			}
			const FString PackageName = NormalizePackagePath(StreamingLevel->GetWorldAssetPackageName());
			const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
			const FString LoadedLevelName = StreamingLevel->GetLoadedLevel() ? StreamingLevel->GetLoadedLevel()->GetOutermost()->GetName() : FString();
			if (PackageName.Equals(Needle, ESearchCase::IgnoreCase)
				|| AssetName.Equals(LevelPackageOrName, ESearchCase::IgnoreCase)
				|| LoadedLevelName.Equals(Needle, ESearchCase::IgnoreCase))
			{
				return StreamingLevel;
			}
		}
		return nullptr;
	}

	static TSharedPtr<FJsonObject> BuildStreamingLevelJson(ULevelStreaming* StreamingLevel, UWorld* World)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!StreamingLevel)
		{
			return Obj;
		}
		ULevel* LoadedLevel = StreamingLevel->GetLoadedLevel();
		const FString PackageName = NormalizePackagePath(StreamingLevel->GetWorldAssetPackageName());
		Obj->SetStringField(TEXT("level_package"), PackageName);
		Obj->SetStringField(TEXT("asset_name"), FPackageName::GetLongPackageAssetName(PackageName));
		Obj->SetStringField(TEXT("streaming_class"), StreamingLevel->GetClass()->GetPathName());
		Obj->SetBoolField(TEXT("loaded"), StreamingLevel->IsLevelLoaded());
		Obj->SetBoolField(TEXT("visible"), StreamingLevel->IsLevelVisible());
		Obj->SetBoolField(TEXT("should_be_loaded"), StreamingLevel->ShouldBeLoaded());
		Obj->SetBoolField(TEXT("should_be_visible"), StreamingLevel->ShouldBeVisible());
		Obj->SetBoolField(TEXT("is_current"), World && LoadedLevel && World->GetCurrentLevel() == LoadedLevel);
		Obj->SetObjectField(TEXT("transform"), TransformToJson(StreamingLevel->LevelTransform));
		Obj->SetNumberField(TEXT("actor_count"), LoadedLevel ? LoadedLevel->Actors.Num() : 0);
		Obj->SetBoolField(TEXT("dirty"), LoadedLevel && LoadedLevel->GetOutermost() ? LoadedLevel->GetOutermost()->IsDirty() : false);
		return Obj;
	}

	static bool ResolveActors(UWorld* World, const TSharedPtr<FJsonObject>& Params, TArray<AActor*>& OutActors, TArray<TSharedPtr<FJsonValue>>& OutResults, FString& OutError)
	{
		OutActors.Reset();
		OutResults.Reset();
		if (!World || !Params.IsValid())
		{
			OutError = TEXT("missing_world_or_params");
			return false;
		}

		TArray<FString> ActorIds = ReadStringArray(Params, TEXT("actor_ids"));
		if (ActorIds.IsEmpty())
		{
			ActorIds = ReadStringArray(Params, TEXT("actors"));
		}
		if (ActorIds.IsEmpty())
		{
			FString SingleActor;
			if (Params->TryGetStringField(TEXT("actor_id"), SingleActor) || Params->TryGetStringField(TEXT("actor"), SingleActor))
			{
				ActorIds.Add(SingleActor);
			}
		}

		FString FolderRoot;
		FString ManagedTag;
		Params->TryGetStringField(TEXT("folder_root"), FolderRoot);
		Params->TryGetStringField(TEXT("managed_tag"), ManagedTag);

		if (ActorIds.IsEmpty() && FolderRoot.IsEmpty())
		{
			const TSharedPtr<FJsonObject>* Selection = nullptr;
			if (Params->TryGetObjectField(TEXT("actor_selection"), Selection) && Selection && Selection->IsValid())
			{
				ActorIds = ReadStringArray(*Selection, TEXT("ids"));
				(*Selection)->TryGetStringField(TEXT("folder_root"), FolderRoot);
				(*Selection)->TryGetStringField(TEXT("managed_tag"), ManagedTag);
			}
		}

		if (ActorIds.IsEmpty() && FolderRoot.IsEmpty())
		{
			OutError = TEXT("missing_actor_selection");
			return false;
		}

		for (const FString& ActorId : ActorIds)
		{
			AActor* Actor = FindActorByNameOrLabel(World, ActorId);
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("actor_id"), ActorId);
			Item->SetBoolField(TEXT("found"), Actor != nullptr);
			if (Actor)
			{
				OutActors.AddUnique(Actor);
				Item->SetStringField(TEXT("name"), Actor->GetName());
				Item->SetStringField(TEXT("label"), Actor->GetActorLabel());
				Item->SetStringField(TEXT("level"), Actor->GetLevel() && Actor->GetLevel()->GetOutermost() ? Actor->GetLevel()->GetOutermost()->GetName() : FString());
			}
			OutResults.Add(MakeShared<FJsonValueObject>(Item));
		}

		if (!FolderRoot.IsEmpty())
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!Actor)
				{
					continue;
				}
				const FString FolderPath = Actor->GetFolderPath().ToString();
				if (!FolderPath.StartsWith(FolderRoot, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (!ManagedTag.IsEmpty() && !Actor->Tags.Contains(FName(*ManagedTag)))
				{
					continue;
				}
				OutActors.AddUnique(Actor);
			}
		}

		int32 MaxActors = 1000;
		double MaxActorsNumber = 0.0;
		if (Params->TryGetNumberField(TEXT("max_actors"), MaxActorsNumber))
		{
			MaxActors = FMath::Max(1, static_cast<int32>(MaxActorsNumber));
		}
		if (OutActors.Num() > MaxActors)
		{
			OutError = TEXT("actor_selection_exceeds_max_actors");
			return false;
		}
		if (OutActors.IsEmpty())
		{
			OutError = TEXT("actor_selection_empty");
			return false;
		}
		return true;
	}

	static UDataLayerEditorSubsystem* GetDataLayerSubsystem()
	{
		return GEditor ? GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>() : nullptr;
	}

	static AWorldDataLayers* FindWorldDataLayers(UWorld* World)
	{
		if (!World)
		{
			return nullptr;
		}
		for (TActorIterator<AWorldDataLayers> It(World); It; ++It)
		{
			return *It;
		}
		return nullptr;
	}

	static bool SupportsDataLayerInstanceAuthoring(UWorld* World)
	{
		return World && World->PersistentLevel && World->PersistentLevel->IsUsingExternalObjects();
	}

	static AWorldDataLayers* GetOrCreateWorldDataLayers(UWorld* World)
	{
		if (AWorldDataLayers* Existing = FindWorldDataLayers(World))
		{
			return Existing;
		}
		if (!SupportsDataLayerInstanceAuthoring(World))
		{
			return nullptr;
		}
		return AWorldDataLayers::Create(World);
	}

	static UDataLayerInstance* FindDataLayerInstance(UWorld* World, const FString& LayerNameOrPath)
	{
		UDataLayerEditorSubsystem* Subsystem = GetDataLayerSubsystem();
		if (Subsystem && !LayerNameOrPath.IsEmpty())
		{
			if (UDataLayerInstance* ByName = Subsystem->GetDataLayerInstance(FName(*LayerNameOrPath)))
			{
				return ByName;
			}
			if (UDataLayerAsset* Asset = LoadTypedAsset<UDataLayerAsset>(LayerNameOrPath))
			{
				if (UDataLayerInstance* ByAsset = Subsystem->GetDataLayerInstance(Asset))
				{
					return ByAsset;
				}
			}
		}
		if (World)
		{
			if (AWorldDataLayers* WorldDataLayers = FindWorldDataLayers(World))
			{
				if (!LayerNameOrPath.IsEmpty())
				{
					if (const UDataLayerInstance* ByName = WorldDataLayers->GetDataLayerInstance(FName(*LayerNameOrPath)))
					{
						return const_cast<UDataLayerInstance*>(ByName);
					}
					if (UDataLayerAsset* Asset = LoadTypedAsset<UDataLayerAsset>(LayerNameOrPath))
					{
						if (const UDataLayerInstance* ByAsset = WorldDataLayers->GetDataLayerInstance(Asset))
						{
							return const_cast<UDataLayerInstance*>(ByAsset);
						}
					}
				}
				UDataLayerInstance* Found = nullptr;
				WorldDataLayers->ForEachDataLayerInstance([&Found, &LayerNameOrPath](UDataLayerInstance* Instance)
				{
					if (!Instance || Found)
					{
						return false;
					}
					const FString ShortName = Instance->GetDataLayerShortName();
					const FString FullName = Instance->GetDataLayerFullName();
					if (Instance->GetDataLayerFName().ToString().Equals(LayerNameOrPath, ESearchCase::IgnoreCase)
						|| ShortName.Equals(LayerNameOrPath, ESearchCase::IgnoreCase)
						|| FullName.Equals(LayerNameOrPath, ESearchCase::IgnoreCase))
					{
						Found = Instance;
						return false;
					}
					return true;
				});
				return Found;
			}
		}
		return nullptr;
	}

	static bool NamesEqualAsSet(const TArray<FName>& A, const TArray<FName>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (const FName& Name : A)
		{
			if (!B.Contains(Name))
			{
				return false;
			}
		}
		return true;
	}

	static bool ActorHasDataLayerAssetDirect(AActor* Actor, const UDataLayerAsset* Asset)
	{
		if (!Actor || !Asset)
		{
			return false;
		}
		FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(AActor::StaticClass(), AActor::GetDataLayerAssetsPropertyName());
		if (!ArrayProperty)
		{
			return false;
		}
		FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(ArrayProperty->Inner);
		if (!SoftObjectProperty)
		{
			return false;
		}
		void* ArrayPtr = ArrayProperty->ContainerPtrToValuePtr<void>(Actor);
		FScriptArrayHelper Helper(ArrayProperty, ArrayPtr);
		const FSoftObjectPath AssetPath(Asset);
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			const FSoftObjectPtr Existing = SoftObjectProperty->GetPropertyValue(Helper.GetRawPtr(Index));
			if (Existing.ToSoftObjectPath() == AssetPath || Existing.Get() == Asset)
			{
				return true;
			}
		}
		return false;
	}

	static bool ActorContainsDataLayerInstance(AActor* Actor, UDataLayerInstance* Layer)
	{
		if (!Actor || !Layer)
		{
			return false;
		}
		if (const UDataLayerAsset* Asset = Layer->GetAsset())
		{
			return ActorHasDataLayerAssetDirect(Actor, Asset)
				|| Actor->GetDataLayerInstanceNames().Contains(Layer->GetDataLayerFName());
		}
		if (Actor->ContainsDataLayer(Layer))
		{
			return true;
		}
		return Actor->GetDataLayerInstanceNames().Contains(Layer->GetDataLayerFName());
	}

	static bool ModifyActorDataLayerAssetDirect(AActor* Actor, const UDataLayerAsset* Asset, bool bAdd)
	{
		if (!Actor || !Asset)
		{
			return false;
		}
		FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(AActor::StaticClass(), AActor::GetDataLayerAssetsPropertyName());
		if (!ArrayProperty)
		{
			return false;
		}
		FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(ArrayProperty->Inner);
		if (!SoftObjectProperty)
		{
			return false;
		}
		void* ArrayPtr = ArrayProperty->ContainerPtrToValuePtr<void>(Actor);
		FScriptArrayHelper Helper(ArrayProperty, ArrayPtr);
		const FSoftObjectPath AssetPath(Asset);
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			const FSoftObjectPtr Existing = SoftObjectProperty->GetPropertyValue(Helper.GetRawPtr(Index));
			if (Existing.ToSoftObjectPath() == AssetPath || Existing.Get() == Asset)
			{
				if (bAdd)
				{
					return false;
				}
				Actor->Modify();
				Helper.RemoveValues(Index);
				return true;
			}
		}
		if (!bAdd)
		{
			return false;
		}
		Actor->Modify();
		const int32 NewIndex = Helper.AddValue();
		SoftObjectProperty->SetPropertyValue(Helper.GetRawPtr(NewIndex), FSoftObjectPtr(Asset));
		return true;
	}

	static bool ClearActorDataLayerAssetsDirect(AActor* Actor)
	{
		if (!Actor)
		{
			return false;
		}
		FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(AActor::StaticClass(), AActor::GetDataLayerAssetsPropertyName());
		if (!ArrayProperty)
		{
			return false;
		}
		void* ArrayPtr = ArrayProperty->ContainerPtrToValuePtr<void>(Actor);
		FScriptArrayHelper Helper(ArrayProperty, ArrayPtr);
		if (Helper.Num() <= 0)
		{
			return false;
		}
		Actor->Modify();
		Helper.EmptyValues();
		return true;
	}

	static bool AddActorToDataLayerInstanceSafe(AActor* Actor, UDataLayerInstance* Layer)
	{
		if (!Actor || !Layer)
		{
			return false;
		}
		if (const UDataLayerAsset* Asset = Layer->GetAsset())
		{
			return ModifyActorDataLayerAssetDirect(Actor, Asset, true);
		}
		return false;
	}

	static bool RemoveActorFromDataLayerInstanceSafe(AActor* Actor, UDataLayerInstance* Layer)
	{
		if (!Actor || !Layer)
		{
			return false;
		}
		if (const UDataLayerAsset* Asset = Layer->GetAsset())
		{
			return ModifyActorDataLayerAssetDirect(Actor, Asset, false);
		}
		return false;
	}

	static FString DataLayerTypeToString(EDataLayerType Type)
	{
		switch (Type)
		{
		case EDataLayerType::Runtime: return TEXT("Runtime");
		case EDataLayerType::Editor: return TEXT("Editor");
		default: return TEXT("Unknown");
		}
	}

	static bool ParseDataLayerType(const FString& Text, EDataLayerType& OutType)
	{
		if (Text.Equals(TEXT("Runtime"), ESearchCase::IgnoreCase))
		{
			OutType = EDataLayerType::Runtime;
			return true;
		}
		if (Text.Equals(TEXT("Editor"), ESearchCase::IgnoreCase))
		{
			OutType = EDataLayerType::Editor;
			return true;
		}
		if (Text.Equals(TEXT("Unknown"), ESearchCase::IgnoreCase))
		{
			OutType = EDataLayerType::Unknown;
			return true;
		}
		return false;
	}

	static bool ParseDataLayerRuntimeState(const FString& Text, EDataLayerRuntimeState& OutState)
	{
		if (Text.Equals(TEXT("Unloaded"), ESearchCase::IgnoreCase))
		{
			OutState = EDataLayerRuntimeState::Unloaded;
			return true;
		}
		if (Text.Equals(TEXT("Loaded"), ESearchCase::IgnoreCase))
		{
			OutState = EDataLayerRuntimeState::Loaded;
			return true;
		}
		if (Text.Equals(TEXT("Activated"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("Active"), ESearchCase::IgnoreCase))
		{
			OutState = EDataLayerRuntimeState::Activated;
			return true;
		}
		return false;
	}

	static bool ImportPropertyText(UObject* Object, const FString& PropertyName, const FString& ValueText, TSharedPtr<FJsonObject>& OutResult)
	{
		OutResult = MakeShared<FJsonObject>();
		OutResult->SetStringField(TEXT("property_name"), PropertyName);
		OutResult->SetStringField(TEXT("requested_value_text"), ValueText);
		if (!Object)
		{
			OutResult->SetStringField(TEXT("property_import_status"), TEXT("object_null"));
			return false;
		}
		FProperty* Property = Object->GetClass()->FindPropertyByName(FName(*PropertyName));
		if (!Property)
		{
			OutResult->SetStringField(TEXT("property_import_status"), TEXT("property_not_found"));
			return false;
		}
		Object->Modify();
		const TCHAR* Result = Property->ImportText_Direct(*ValueText, Property->ContainerPtrToValuePtr<void>(Object), Object, PPF_None);
		FString ReadBack;
		Property->ExportText_Direct(ReadBack, Property->ContainerPtrToValuePtr<void>(Object), Property->ContainerPtrToValuePtr<void>(Object), Object, PPF_None);
		const bool bImported = Result != nullptr;
		OutResult->SetStringField(TEXT("applied_value_text"), ReadBack);
		OutResult->SetStringField(TEXT("property_import_status"), bImported ? TEXT("imported_and_read_back") : TEXT("import_failed"));
		OutResult->SetBoolField(TEXT("property_import_verified"), bImported);
		OutResult->SetBoolField(TEXT("value_text_exact_match"), ReadBack.Equals(ValueText, ESearchCase::CaseSensitive));
		OutResult->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
		return bImported;
	}

	static bool ApplyDataLayerStateFields(
		UWorld* World,
		UDataLayerEditorSubsystem* Subsystem,
		UDataLayerInstance* Instance,
		const TSharedPtr<FJsonObject>& Params,
		TArray<TSharedPtr<FJsonValue>>& Warnings,
		bool& bChanged,
		FString& OutError)
	{
		if (!Instance || !Params.IsValid())
		{
			OutError = TEXT("data_layer_instance_not_found");
			return false;
		}

		bool bVisible = false;
		if (Params->TryGetBoolField(TEXT("visible"), bVisible))
		{
			const bool bBefore = Instance->IsVisible();
			if (Subsystem)
			{
				Subsystem->SetDataLayerVisibility(Instance, bVisible);
			}
			else
			{
				Instance->SetVisible(bVisible);
			}
			bChanged |= (bBefore != Instance->IsVisible());
		}

		bool bLoadedInEditor = false;
		if (Params->TryGetBoolField(TEXT("loaded_in_editor"), bLoadedInEditor))
		{
			const bool bBefore = Instance->IsLoadedInEditor();
			if (Subsystem)
			{
				Subsystem->SetDataLayerIsLoadedInEditor(Instance, bLoadedInEditor, true);
			}
			else
			{
				Instance->SetIsLoadedInEditor(bLoadedInEditor, true);
			}
			bChanged |= (bBefore != Instance->IsLoadedInEditor());
		}

		bool bInitiallyVisible = false;
		if (Params->TryGetBoolField(TEXT("initially_visible"), bInitiallyVisible))
		{
			const bool bBefore = Instance->IsInitiallyVisible();
			if (Subsystem)
			{
				Subsystem->SetDataLayerIsInitiallyVisible(Instance, bInitiallyVisible);
			}
			else
			{
				Instance->SetIsInitiallyVisible(bInitiallyVisible);
			}
			bChanged |= (bBefore != Instance->IsInitiallyVisible());
		}

		FString RuntimeStateText;
		if (Params->TryGetStringField(TEXT("initial_runtime_state"), RuntimeStateText))
		{
			EDataLayerRuntimeState RuntimeState = EDataLayerRuntimeState::Unloaded;
			if (!ParseDataLayerRuntimeState(RuntimeStateText, RuntimeState))
			{
				OutError = TEXT("data_layer_runtime_state_invalid");
				return false;
			}
			const EDataLayerRuntimeState Before = Instance->GetInitialRuntimeState();
			if (Subsystem)
			{
				Subsystem->SetDataLayerInitialRuntimeState(Instance, RuntimeState);
			}
			else
			{
				Instance->SetInitialRuntimeState(RuntimeState);
			}
			bChanged |= (Before != Instance->GetInitialRuntimeState());
		}

		FString ParentName;
		if (Params->TryGetStringField(TEXT("parent"), ParentName))
		{
			UDataLayerInstance* Parent = nullptr;
			if (!ParentName.TrimStartAndEnd().IsEmpty())
			{
				Parent = FindDataLayerInstance(World, ParentName);
				if (!Parent)
				{
					OutError = TEXT("data_layer_parent_not_found");
					return false;
				}
			}
			UDataLayerInstance* Before = Instance->GetParent();
			if (!Instance->SetParent(Parent))
			{
				OutError = TEXT("data_layer_parent_set_failed");
				return false;
			}
			bChanged |= (Before != Instance->GetParent());
		}

		FString ShortName;
		if (Params->TryGetStringField(TEXT("short_name"), ShortName) && !ShortName.TrimStartAndEnd().IsEmpty())
		{
			const FString Before = Instance->GetDataLayerShortName();
			bool bShortNameChanged = false;
			if (Subsystem)
			{
				bShortNameChanged = Subsystem->SetDataLayerShortName(Instance, ShortName);
			}
			if (!bShortNameChanged && Instance->CanEditDataLayerShortName())
			{
				bShortNameChanged = FDataLayerUtils::SetDataLayerShortName(Instance, ShortName);
			}
			if (!bShortNameChanged && !Before.Equals(ShortName, ESearchCase::CaseSensitive))
			{
				AddIssue(
					Warnings,
					TEXT("warning"),
					TEXT("data_layer_short_name_not_editable"),
					TEXT("$.short_name"),
					TEXT("This DataLayer instance does not expose editable short-name semantics; asset-backed DataLayers use their DataLayerAsset name."));
			}
			bChanged |= !Before.Equals(Instance->GetDataLayerShortName(), ESearchCase::CaseSensitive);
		}

		Instance->MarkPackageDirty();
		if (AWorldDataLayers* WorldDataLayers = Instance->GetDirectOuterWorldDataLayers())
		{
			WorldDataLayers->MarkPackageDirty();
		}
		return true;
	}

	static TSharedPtr<FJsonObject> DataLayerAssetToJson(UDataLayerAsset* Asset)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("schema"), TEXT("uai.data_layer_asset.v1"));
		Obj->SetStringField(TEXT("asset_path"), Asset ? ObjectPackagePath(Asset) : FString());
		Obj->SetStringField(TEXT("object_path"), Asset ? Asset->GetPathName() : FString());
		Obj->SetStringField(TEXT("type"), Asset ? DataLayerTypeToString(Asset->GetType()) : FString());
		if (Asset)
		{
			const FColor Color = Asset->GetDebugColor();
			TSharedPtr<FJsonObject> ColorJson = MakeShared<FJsonObject>();
			ColorJson->SetNumberField(TEXT("r"), Color.R);
			ColorJson->SetNumberField(TEXT("g"), Color.G);
			ColorJson->SetNumberField(TEXT("b"), Color.B);
			ColorJson->SetNumberField(TEXT("a"), Color.A);
			Obj->SetObjectField(TEXT("debug_color"), ColorJson);
			Obj->SetBoolField(TEXT("is_runtime"), Asset->IsRuntime());
			Obj->SetBoolField(TEXT("is_client_only"), Asset->IsClientOnly());
			Obj->SetBoolField(TEXT("is_server_only"), Asset->IsServerOnly());
			Obj->SetBoolField(TEXT("supports_actor_filters"), Asset->SupportsActorFilters());
		}
		return Obj;
	}

	static TSharedPtr<FJsonObject> DataLayerInstanceToJson(UDataLayerInstance* Instance, UWorld* World, bool bIncludeActors)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Instance)
		{
			return Obj;
		}
		Obj->SetStringField(TEXT("instance_name"), Instance->GetDataLayerFName().ToString());
		Obj->SetStringField(TEXT("short_name"), Instance->GetDataLayerShortName());
		Obj->SetStringField(TEXT("full_name"), Instance->GetDataLayerFullName());
		Obj->SetStringField(TEXT("type"), DataLayerTypeToString(Instance->GetType()));
		const EDataLayerRuntimeState InitialRuntimeState = Instance->GetInitialRuntimeState();
		Obj->SetStringField(TEXT("initial_runtime_state"), GetDataLayerRuntimeStateName(InitialRuntimeState));
		Obj->SetStringField(TEXT("runtime_state"), GetDataLayerRuntimeStateName(InitialRuntimeState));
		Obj->SetStringField(TEXT("runtime_state_source"), TEXT("initial_runtime_state_editor_safe"));
		Obj->SetBoolField(TEXT("loaded_in_editor"), Instance->IsLoadedInEditor());
		Obj->SetBoolField(TEXT("effective_loaded_in_editor"), Instance->IsEffectiveLoadedInEditor());
		Obj->SetBoolField(TEXT("initially_visible"), Instance->IsInitiallyVisible());
		Obj->SetStringField(TEXT("asset_path"), Instance->GetAsset() ? ObjectPackagePath(const_cast<UDataLayerAsset*>(Instance->GetAsset())) : FString());
		if (bIncludeActors && World)
		{
			TArray<TSharedPtr<FJsonValue>> ActorValues;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (Actor && ActorContainsDataLayerInstance(Actor, Instance))
				{
					TSharedPtr<FJsonObject> ActorJson = MakeShared<FJsonObject>();
					ActorJson->SetStringField(TEXT("name"), Actor->GetName());
					ActorJson->SetStringField(TEXT("label"), Actor->GetActorLabel());
					ActorValues.Add(MakeShared<FJsonValueObject>(ActorJson));
				}
			}
			Obj->SetArrayField(TEXT("actors"), ActorValues);
			Obj->SetNumberField(TEXT("actor_count"), ActorValues.Num());
		}
		return Obj;
	}

	static FString HLODLayerTypeToString(EHLODLayerType Type)
	{
		switch (Type)
		{
		case EHLODLayerType::Instancing: return TEXT("Instancing");
		case EHLODLayerType::MeshMerge: return TEXT("MeshMerge");
		case EHLODLayerType::MeshSimplify: return TEXT("MeshSimplify");
		case EHLODLayerType::MeshApproximate: return TEXT("MeshApproximate");
		case EHLODLayerType::Custom: return TEXT("Custom");
		default: return TEXT("Unknown");
		}
	}

	static bool ParseHLODLayerType(const FString& Text, EHLODLayerType& OutType)
	{
		if (Text.Equals(TEXT("Instancing"), ESearchCase::IgnoreCase))
		{
			OutType = EHLODLayerType::Instancing;
			return true;
		}
		if (Text.Equals(TEXT("MeshMerge"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("MergedMesh"), ESearchCase::IgnoreCase))
		{
			OutType = EHLODLayerType::MeshMerge;
			return true;
		}
		if (Text.Equals(TEXT("MeshSimplify"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("SimplifiedMesh"), ESearchCase::IgnoreCase))
		{
			OutType = EHLODLayerType::MeshSimplify;
			return true;
		}
		if (Text.Equals(TEXT("MeshApproximate"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("ApproximatedMesh"), ESearchCase::IgnoreCase))
		{
			OutType = EHLODLayerType::MeshApproximate;
			return true;
		}
		if (Text.Equals(TEXT("Custom"), ESearchCase::IgnoreCase))
		{
			OutType = EHLODLayerType::Custom;
			return true;
		}
		return false;
	}

	static TSharedPtr<FJsonObject> HLODLayerToJson(UHLODLayer* Layer)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("schema"), HLODLayerSchema);
		Obj->SetStringField(TEXT("asset_path"), Layer ? ObjectPackagePath(Layer) : FString());
		Obj->SetStringField(TEXT("object_path"), Layer ? Layer->GetPathName() : FString());
		if (Layer)
		{
			Obj->SetStringField(TEXT("layer_type"), HLODLayerTypeToString(Layer->GetLayerType()));
			Obj->SetBoolField(TEXT("is_spatially_loaded"), Layer->IsSpatiallyLoaded());
			Obj->SetNumberField(TEXT("cell_size"), Layer->GetCellSize());
			Obj->SetNumberField(TEXT("loading_range"), Layer->GetLoadingRange());
			Obj->SetStringField(TEXT("parent_layer"), Layer->GetParentLayer() ? ObjectPackagePath(Layer->GetParentLayer()) : FString());
			Obj->SetStringField(TEXT("hlod_builder_class"), Layer->GetHLODBuilderClass() ? Layer->GetHLODBuilderClass()->GetPathName() : FString());
			Obj->SetStringField(TEXT("hlod_actor_class"), Layer->GetHLODActorClass() ? Layer->GetHLODActorClass()->GetPathName() : FString());
			Obj->SetStringField(TEXT("hlod_modifier_class"), Layer->GetHLODModifierClass() ? Layer->GetHLODModifierClass()->GetPathName() : FString());
		}
		return Obj;
	}

	static bool CreateOrLoadAssetWithFactory(const FString& AssetPath, UClass* AssetClass, UFactory* Factory, const FString& OverwritePolicy, UObject*& OutAsset, FString& OutError)
	{
		OutAsset = LoadObjectByPath(AssetPath);
		if (OutAsset)
		{
			if (!OutAsset->IsA(AssetClass))
			{
				OutError = TEXT("existing_asset_class_mismatch");
				return false;
			}
			if (OverwritePolicy.Equals(TEXT("fail"), ESearchCase::IgnoreCase))
			{
				OutError = TEXT("asset_already_exists");
				return false;
			}
			return true;
		}
		if (!IsValidLongPackagePath(AssetPath))
		{
			OutError = TEXT("asset_path_invalid");
			return false;
		}
		const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		OutAsset = AssetToolsModule.Get().CreateAsset(AssetName, PackagePath, AssetClass, Factory);
		if (!OutAsset)
		{
			OutError = TEXT("asset_create_failed");
			return false;
		}
		if (!OutAsset->IsRooted())
		{
			OutAsset->AddToRoot();
		}
		GetSessionAssetRegistry().Add(NormalizePackagePath(AssetPath), OutAsset);
		GetSessionAssetRegistry().Add(ObjectPackagePath(OutAsset), OutAsset);
		return true;
	}

	static bool IsNullRHI()
	{
		return FParse::Param(FCommandLine::Get(), TEXT("NullRHI")) || !FApp::CanEverRender();
	}

	static bool RunCommandletProcess(const FString& ExtraArgs, double TimeoutSeconds, TSharedPtr<FJsonObject>& OutData, FString& OutError)
	{
		const FString EditorCmd = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::EngineDir(), TEXT("Binaries/Win64/UnrealEditor-Cmd.exe")));
		const FString ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
		const FString LogFile = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectLogDir(), FString::Printf(TEXT("UAI_LevelTopology_Commandlet_%s.log"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
		const FString Args = FString::Printf(TEXT("\"%s\" %s -unattended -nop4 -nosplash -NoSound -log=\"%s\""), *ProjectPath, *ExtraArgs, *LogFile);
		OutData->SetStringField(TEXT("commandlet_exe"), EditorCmd);
		OutData->SetStringField(TEXT("commandlet_args"), Args);
		OutData->SetStringField(TEXT("log_file"), LogFile);

		void* ReadPipe = nullptr;
		void* WritePipe = nullptr;
		FPlatformProcess::CreatePipe(ReadPipe, WritePipe);
		FProcHandle Proc = FPlatformProcess::CreateProc(*EditorCmd, *Args, true, false, false, nullptr, 0, nullptr, WritePipe, ReadPipe);
		if (!Proc.IsValid())
		{
			FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
			OutError = TEXT("commandlet_start_failed");
			return false;
		}
		const double StartTime = FPlatformTime::Seconds();
		FString Output;
		bool bTimedOut = false;
		while (FPlatformProcess::IsProcRunning(Proc))
		{
			Output += FPlatformProcess::ReadPipe(ReadPipe);
			if ((FPlatformTime::Seconds() - StartTime) > TimeoutSeconds)
			{
				bTimedOut = true;
				FPlatformProcess::TerminateProc(Proc, true);
				break;
			}
			FPlatformProcess::Sleep(0.1f);
		}
		Output += FPlatformProcess::ReadPipe(ReadPipe);
		int32 ReturnCode = 0;
		FPlatformProcess::GetProcReturnCode(Proc, &ReturnCode);
		FPlatformProcess::CloseProc(Proc);
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		OutData->SetNumberField(TEXT("return_code"), ReturnCode);
		OutData->SetBoolField(TEXT("timed_out"), bTimedOut);
		OutData->SetStringField(TEXT("stdout_tail"), Output.Right(4096));
		if (bTimedOut)
		{
			OutError = TEXT("long_task_timeout");
			return false;
		}
		if (ReturnCode != 0)
		{
			OutError = TEXT("commandlet_failed");
			return false;
		}
		return true;
	}

	class FCollectingWorldPartitionErrorHandler final : public IStreamingGenerationErrorHandler
	{
	public:
		TArray<TSharedPtr<FJsonValue>> Issues;

		void Add(const FString& Code, const FString& Actor, const FString& Message)
		{
			TSharedPtr<FJsonObject> Issue = MakeIssue(TEXT("error"), Code, Actor, Message);
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}

		FString ActorName(const IWorldPartitionActorDescInstanceView& ActorDescView)
		{
			return IStreamingGenerationErrorHandler::GetFullActorName(ActorDescView);
		}

		virtual void OnInvalidRuntimeGrid(const IWorldPartitionActorDescInstanceView& ActorDescView, FName GridName) override
		{
			Add(TEXT("invalid_runtime_grid"), ActorName(ActorDescView), GridName.ToString());
		}

		virtual void OnInvalidReference(const IWorldPartitionActorDescInstanceView& ActorDescView, const FGuid& ReferenceGuid, IWorldPartitionActorDescInstanceView*) override
		{
			Add(TEXT("invalid_reference"), ActorName(ActorDescView), ReferenceGuid.ToString(EGuidFormats::DigitsWithHyphens));
		}

		virtual void OnInvalidReferenceGridPlacement(const IWorldPartitionActorDescInstanceView& ActorDescView, const IWorldPartitionActorDescInstanceView& ReferenceActorDescView) override
		{
			Add(TEXT("invalid_reference_grid_placement"), ActorName(ActorDescView), ActorName(ReferenceActorDescView));
		}

		virtual void OnInvalidReferenceDataLayers(const IWorldPartitionActorDescInstanceView& ActorDescView, const IWorldPartitionActorDescInstanceView& ReferenceActorDescView, EDataLayerInvalidReason) override
		{
			Add(TEXT("invalid_reference_data_layers"), ActorName(ActorDescView), ActorName(ReferenceActorDescView));
		}

		virtual void OnInvalidReferenceRuntimeGrid(const IWorldPartitionActorDescInstanceView& ActorDescView, const IWorldPartitionActorDescInstanceView& ReferenceActorDescView) override
		{
			Add(TEXT("invalid_reference_runtime_grid"), ActorName(ActorDescView), ActorName(ReferenceActorDescView));
		}

		virtual void OnDataLayersLoadFilterMismatch(const IWorldPartitionActorDescInstanceView& ActorDescView) override
		{
			Add(TEXT("data_layers_load_filter_mismatch"), ActorName(ActorDescView), TEXT(""));
		}

		virtual void OnInvalidWorldReference(const IWorldPartitionActorDescInstanceView& ActorDescView, EWorldReferenceInvalidReason) override
		{
			Add(TEXT("invalid_world_reference"), ActorName(ActorDescView), TEXT(""));
		}

		virtual void OnInvalidReferenceDataLayerAsset(const UDataLayerInstanceWithAsset* DataLayerInstance) override
		{
			Add(TEXT("invalid_reference_data_layer_asset"), DataLayerInstance ? DataLayerInstance->GetPathName() : FString(), TEXT(""));
		}

		virtual void OnDataLayerHierarchyTypeMismatch(const UDataLayerInstance* DataLayerInstance, const UDataLayerInstance*, EDataLayerHierarchyInvalidReason) override
		{
			Add(TEXT("data_layer_hierarchy_type_mismatch"), DataLayerInstance ? DataLayerInstance->GetPathName() : FString(), TEXT(""));
		}

		virtual void OnInvalidWorldDataLayersReference(const AWorldDataLayers* WorldDataLayers, const UDataLayerInstance* DataLayerInstance, const FText& Reason) override
		{
			Add(TEXT("invalid_world_data_layers_reference"), WorldDataLayers ? WorldDataLayers->GetPathName() : FString(), FString::Printf(TEXT("%s %s"), DataLayerInstance ? *DataLayerInstance->GetPathName() : TEXT(""), *Reason.ToString()));
		}

		virtual void OnDataLayerAssetConflict(const UDataLayerInstanceWithAsset* DataLayerInstance, const UDataLayerInstanceWithAsset* ConflictingDataLayerInstance) override
		{
			Add(TEXT("data_layer_asset_conflict"), DataLayerInstance ? DataLayerInstance->GetPathName() : FString(), ConflictingDataLayerInstance ? ConflictingDataLayerInstance->GetPathName() : FString());
		}

		virtual void OnInvalidDataLayerAssetType(const UDataLayerInstanceWithAsset* DataLayerInstance, const UDataLayerAsset* DataLayerAsset) override
		{
			Add(TEXT("invalid_data_layer_asset_type"), DataLayerInstance ? DataLayerInstance->GetPathName() : FString(), DataLayerAsset ? DataLayerAsset->GetPathName() : FString());
		}

		virtual void OnActorNeedsResave(const IWorldPartitionActorDescInstanceView& ActorDescView) override
		{
			Add(TEXT("actor_needs_resave"), ActorName(ActorDescView), TEXT(""));
		}

		virtual void OnLevelInstanceInvalidWorldAsset(const IWorldPartitionActorDescInstanceView& ActorDescView, FName WorldAsset, ELevelInstanceInvalidReason) override
		{
			Add(TEXT("level_instance_invalid_world_asset"), ActorName(ActorDescView), WorldAsset.ToString());
		}

		virtual void OnInvalidActorFilterReference(const IWorldPartitionActorDescInstanceView& ActorDescView, const IWorldPartitionActorDescInstanceView& ReferenceActorDescView) override
		{
			Add(TEXT("invalid_actor_filter_reference"), ActorName(ActorDescView), ActorName(ReferenceActorDescView));
		}

		virtual void OnInvalidHLODLayer(const IWorldPartitionActorDescInstanceView& ActorDescView) override
		{
			Add(TEXT("invalid_hlod_layer"), ActorName(ActorDescView), TEXT(""));
		}
	};
}

bool FUeAgentHttpServer::ExecuteLevelTopologyCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("level_topology_export_json")) return CmdLevelTopologyExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_topology_validate_json")) return CmdLevelTopologyValidateJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_topology_diff_json")) return CmdLevelTopologyDiffJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_topology_apply_json")) return CmdLevelTopologyApplyJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_topology_set_actor_assignments")) return CmdLevelTopologySetActorAssignments(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_streaming_query")) return CmdLevelStreamingQuery(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_streaming_create_level")) return CmdLevelStreamingCreateLevel(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_streaming_add_existing")) return CmdLevelStreamingAddExisting(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_streaming_remove")) return CmdLevelStreamingRemove(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_streaming_set_state")) return CmdLevelStreamingSetState(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_streaming_move_actors")) return CmdLevelStreamingMoveActors(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_streaming_save")) return CmdLevelStreamingSave(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_streaming_validate")) return CmdLevelStreamingValidate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("data_layer_query")) return CmdDataLayerQuery(Ctx, OutData, OutError);
	if (CommandLower == TEXT("data_layer_create_asset")) return CmdDataLayerCreateAsset(Ctx, OutData, OutError);
	if (CommandLower == TEXT("data_layer_apply_asset_json")) return CmdDataLayerApplyAssetJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("data_layer_create_instance")) return CmdDataLayerCreateInstance(Ctx, OutData, OutError);
	if (CommandLower == TEXT("data_layer_delete_instance")) return CmdDataLayerDeleteInstance(Ctx, OutData, OutError);
	if (CommandLower == TEXT("data_layer_set_state")) return CmdDataLayerSetState(Ctx, OutData, OutError);
	if (CommandLower == TEXT("data_layer_set_actor_membership")) return CmdDataLayerSetActorMembership(Ctx, OutData, OutError);
	if (CommandLower == TEXT("data_layer_validate")) return CmdDataLayerValidate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("world_partition_query")) return CmdWorldPartitionQuery(Ctx, OutData, OutError);
	if (CommandLower == TEXT("world_partition_set_streaming_enabled")) return CmdWorldPartitionSetStreamingEnabled(Ctx, OutData, OutError);
	if (CommandLower == TEXT("world_partition_load_region")) return CmdWorldPartitionLoadRegion(Ctx, OutData, OutError);
	if (CommandLower == TEXT("world_partition_unload_region")) return CmdWorldPartitionUnloadRegion(Ctx, OutData, OutError);
	if (CommandLower == TEXT("world_partition_load_actors")) return CmdWorldPartitionLoadActors(Ctx, OutData, OutError);
	if (CommandLower == TEXT("world_partition_unload_actors")) return CmdWorldPartitionUnloadActors(Ctx, OutData, OutError);
	if (CommandLower == TEXT("world_partition_list_loaded_regions")) return CmdWorldPartitionListLoadedRegions(Ctx, OutData, OutError);
	if (CommandLower == TEXT("world_partition_check_errors")) return CmdWorldPartitionCheckErrors(Ctx, OutData, OutError);
	if (CommandLower == TEXT("world_partition_generate_streaming")) return CmdWorldPartitionGenerateStreaming(Ctx, OutData, OutError);
	if (CommandLower == TEXT("world_partition_resave_actors")) return CmdWorldPartitionResaveActors(Ctx, OutData, OutError);
	if (CommandLower == TEXT("hlod_layer_create_json")) return CmdHLODLayerCreateJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("hlod_layer_export_json")) return CmdHLODLayerExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("hlod_layer_validate_json")) return CmdHLODLayerValidateJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("hlod_layer_apply_json")) return CmdHLODLayerApplyJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("hlod_assign_actors")) return CmdHLODAssignActors(Ctx, OutData, OutError);
	if (CommandLower == TEXT("hlod_query_actors")) return CmdHLODQueryActors(Ctx, OutData, OutError);
	if (CommandLower == TEXT("hlod_build")) return CmdHLODBuild(Ctx, OutData, OutError);
	if (CommandLower == TEXT("hlod_validate_build")) return CmdHLODValidateBuild(Ctx, OutData, OutError);

	OutError = TEXT("unknown_level_topology_command");
	return false;
}

bool FUeAgentHttpServer::CmdLevelStreamingQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FString FilterPackage;
	JsonTryGetString(Ctx.Params, TEXT("level_package"), FilterPackage);
	TArray<TSharedPtr<FJsonValue>> Levels;
	for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
	{
		if (!StreamingLevel)
		{
			continue;
		}
		if (!FilterPackage.IsEmpty() && !UeAgentLevelTopology::NormalizePackagePath(StreamingLevel->GetWorldAssetPackageName()).Equals(UeAgentLevelTopology::NormalizePackagePath(FilterPackage), ESearchCase::IgnoreCase))
		{
			continue;
		}
		Levels.Add(MakeShared<FJsonValueObject>(UeAgentLevelTopology::BuildStreamingLevelJson(StreamingLevel, World)));
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.level_streaming.query.v1"));
	OutData->SetStringField(TEXT("world"), World->GetPathName());
	OutData->SetArrayField(TEXT("streaming_levels"), Levels);
	OutData->SetNumberField(TEXT("count"), Levels.Num());
	return true;
}

bool FUeAgentHttpServer::CmdLevelStreamingCreateLevel(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FString LevelPackage;
	if (!JsonTryGetString(Ctx.Params, TEXT("level_package"), LevelPackage) && !JsonTryGetString(Ctx.Params, TEXT("package_path"), LevelPackage))
	{
		OutError = TEXT("missing_level_package");
		return false;
	}
	LevelPackage = UeAgentLevelTopology::NormalizePackagePath(LevelPackage);
	if (!UeAgentLevelTopology::IsValidLongPackagePath(LevelPackage))
	{
		OutError = TEXT("streaming_level_package_invalid");
		return false;
	}
	if (UeAgentLevelTopology::FindStreamingLevel(World, LevelPackage))
	{
		OutError = TEXT("streaming_level_already_exists");
		return false;
	}
	FString StreamingClassPath;
	JsonTryGetString(Ctx.Params, TEXT("streaming_class"), StreamingClassPath);
	UClass* StreamingClass = UeAgentLevelTopology::ResolveStreamingClass(StreamingClassPath);
	if (!StreamingClass)
	{
		OutError = TEXT("streaming_class_invalid");
		return false;
	}
	bool bUseExternalActors = false;
	bool bCreateWorldPartition = false;
	bool bEnableWorldPartitionStreaming = true;
	bool bMoveSelectedActors = false;
	bool bSaveAfterCreate = false;
	bool bTransaction = true;
	JsonTryGetBool(Ctx.Params, TEXT("use_external_actors"), bUseExternalActors);
	JsonTryGetBool(Ctx.Params, TEXT("create_world_partition"), bCreateWorldPartition);
	JsonTryGetBool(Ctx.Params, TEXT("enable_world_partition_streaming"), bEnableWorldPartitionStreaming);
	JsonTryGetBool(Ctx.Params, TEXT("move_selected_actors"), bMoveSelectedActors);
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	JsonTryGetBool(Ctx.Params, TEXT("transaction"), bTransaction);

	FTransform LevelTransform = FTransform::Identity;
	TSharedPtr<FJsonObject> TransformJson;
	if (JsonTryGetObject(Ctx.Params, TEXT("transform"), TransformJson))
	{
		UeAgentLevelTopology::TryReadTransform(TransformJson, LevelTransform);
	}

	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UeAgentInterface", "LevelStreamingCreate", "UeAgentInterface Level Streaming Create"));
	}
	UEditorLevelUtils::FCreateNewStreamingLevelForWorldParams CreateParams(StreamingClass, LevelPackage);
	CreateParams.bUseExternalActors = bUseExternalActors;
	CreateParams.bCreateWorldPartition = bCreateWorldPartition;
	CreateParams.bEnableWorldPartitionStreaming = bEnableWorldPartitionStreaming;
	CreateParams.bUseSaveAs = false;
	CreateParams.Transform = LevelTransform;
	ULevelStreaming* StreamingLevel = UEditorLevelUtils::CreateNewStreamingLevelForWorld(*World, CreateParams);
	if (!StreamingLevel)
	{
		OutError = TEXT("streaming_level_create_failed");
		return false;
	}
	if (bMoveSelectedActors)
	{
		UEditorLevelUtils::MoveSelectedActorsToLevel(StreamingLevel, false);
	}
	TArray<FString> SavedPackages;
	if (bSaveAfterCreate)
	{
		TArray<UPackage*> Packages;
		if (StreamingLevel->GetLoadedLevel() && StreamingLevel->GetLoadedLevel()->GetOutermost())
		{
			Packages.Add(StreamingLevel->GetLoadedLevel()->GetOutermost());
		}
		if (!UeAgentLevelTopology::SavePackages(Packages, SavedPackages, OutError))
		{
			return false;
		}
	}
	OutData = UeAgentLevelTopology::BuildStreamingLevelJson(StreamingLevel, World);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.level_streaming.create_result.v1"));
	OutData->SetBoolField(TEXT("created"), true);
	OutData->SetBoolField(TEXT("transaction"), bTransaction);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	TArray<TSharedPtr<FJsonValue>> SavedValues;
	for (const FString& SavedPackage : SavedPackages)
	{
		SavedValues.Add(MakeShared<FJsonValueString>(SavedPackage));
	}
	OutData->SetArrayField(TEXT("saved_packages"), SavedValues);
	return true;
}

bool FUeAgentHttpServer::CmdLevelStreamingAddExisting(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FString LevelPackage;
	if (!JsonTryGetString(Ctx.Params, TEXT("level_package"), LevelPackage) && !JsonTryGetString(Ctx.Params, TEXT("package_path"), LevelPackage))
	{
		OutError = TEXT("missing_level_package");
		return false;
	}
	LevelPackage = UeAgentLevelTopology::NormalizePackagePath(LevelPackage);
	if (!UeAgentLevelTopology::IsValidLongPackagePath(LevelPackage))
	{
		OutError = TEXT("streaming_level_package_invalid");
		return false;
	}
	if (!UeAgentLevelTopology::AssetExists(LevelPackage))
	{
		OutError = TEXT("streaming_level_asset_not_found");
		return false;
	}
	FString StreamingClassPath;
	JsonTryGetString(Ctx.Params, TEXT("streaming_class"), StreamingClassPath);
	UClass* StreamingClass = UeAgentLevelTopology::ResolveStreamingClass(StreamingClassPath);
	if (!StreamingClass)
	{
		OutError = TEXT("streaming_class_invalid");
		return false;
	}
	FTransform LevelTransform = FTransform::Identity;
	TSharedPtr<FJsonObject> TransformJson;
	if (JsonTryGetObject(Ctx.Params, TEXT("transform"), TransformJson))
	{
		UeAgentLevelTopology::TryReadTransform(TransformJson, LevelTransform);
	}
	ULevelStreaming* StreamingLevel = UEditorLevelUtils::AddLevelToWorld(World, *LevelPackage, StreamingClass, LevelTransform);
	if (!StreamingLevel)
	{
		OutError = TEXT("streaming_level_add_failed");
		return false;
	}
	OutData = UeAgentLevelTopology::BuildStreamingLevelJson(StreamingLevel, World);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.level_streaming.add_existing_result.v1"));
	OutData->SetBoolField(TEXT("added"), true);
	return true;
}

bool FUeAgentHttpServer::CmdLevelStreamingRemove(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FString LevelPackage;
	if (!JsonTryGetString(Ctx.Params, TEXT("level_package"), LevelPackage) && !JsonTryGetString(Ctx.Params, TEXT("target_level"), LevelPackage))
	{
		OutError = TEXT("missing_level_package");
		return false;
	}
	ULevelStreaming* StreamingLevel = UeAgentLevelTopology::FindStreamingLevel(World, LevelPackage);
	if (!StreamingLevel || !StreamingLevel->GetLoadedLevel())
	{
		OutError = TEXT("streaming_level_not_found");
		return false;
	}
	bool bClearSelection = true;
	bool bResetTransactionBuffer = true;
	bool bDeleteAsset = false;
	bool bTransaction = true;
	JsonTryGetBool(Ctx.Params, TEXT("clear_selection"), bClearSelection);
	JsonTryGetBool(Ctx.Params, TEXT("reset_transaction_buffer"), bResetTransactionBuffer);
	JsonTryGetBool(Ctx.Params, TEXT("delete_asset"), bDeleteAsset);
	JsonTryGetBool(Ctx.Params, TEXT("transaction"), bTransaction);
	const FString RemovedPackage = UeAgentLevelTopology::NormalizePackagePath(StreamingLevel->GetWorldAssetPackageName());
	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UeAgentInterface", "LevelStreamingRemove", "UeAgentInterface Level Streaming Remove"));
	}
	const bool bRemoved = UEditorLevelUtils::RemoveLevelFromWorld(StreamingLevel->GetLoadedLevel(), bClearSelection, bResetTransactionBuffer);
	int32 DeletedAssets = 0;
	if (bRemoved && bDeleteAsset)
	{
		if (UObject* Asset = UeAgentLevelTopology::LoadObjectByPath(RemovedPackage))
		{
			TArray<UObject*> ObjectsToDelete;
			ObjectsToDelete.Add(Asset);
			DeletedAssets = ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete);
		}
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.level_streaming.remove_result.v1"));
	OutData->SetStringField(TEXT("level_package"), RemovedPackage);
	OutData->SetBoolField(TEXT("removed"), bRemoved);
	OutData->SetBoolField(TEXT("delete_asset"), bDeleteAsset);
	OutData->SetNumberField(TEXT("deleted_asset_count"), DeletedAssets);
	OutData->SetBoolField(TEXT("transaction"), bTransaction);
	return bRemoved;
}

bool FUeAgentHttpServer::CmdLevelStreamingSetState(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FString LevelPackage;
	if (!JsonTryGetString(Ctx.Params, TEXT("level_package"), LevelPackage) && !JsonTryGetString(Ctx.Params, TEXT("target_level"), LevelPackage))
	{
		OutError = TEXT("missing_level_package");
		return false;
	}
	ULevelStreaming* StreamingLevel = UeAgentLevelTopology::FindStreamingLevel(World, LevelPackage);
	if (!StreamingLevel)
	{
		OutError = TEXT("streaming_level_not_found");
		return false;
	}
	bool bTransaction = true;
	JsonTryGetBool(Ctx.Params, TEXT("transaction"), bTransaction);
	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UeAgentInterface", "LevelStreamingSetState", "UeAgentInterface Level Streaming Set State"));
	}
	StreamingLevel->Modify();
	bool bLoaded = false;
	if (JsonTryGetBool(Ctx.Params, TEXT("loaded"), bLoaded))
	{
		StreamingLevel->SetShouldBeLoaded(bLoaded);
	}
	bool bVisible = false;
	if (JsonTryGetBool(Ctx.Params, TEXT("visible"), bVisible))
	{
		StreamingLevel->SetShouldBeVisible(bVisible);
		if (StreamingLevel->GetLoadedLevel())
		{
			UEditorLevelUtils::SetLevelVisibility(StreamingLevel->GetLoadedLevel(), bVisible, true, ELevelVisibilityDirtyMode::ModifyOnChange);
		}
	}
	bool bCurrent = false;
	if (JsonTryGetBool(Ctx.Params, TEXT("current"), bCurrent) && bCurrent)
	{
		if (!StreamingLevel->GetLoadedLevel())
		{
			OutError = TEXT("streaming_level_not_loaded");
			return false;
		}
		UEditorLevelUtils::MakeLevelCurrent(StreamingLevel);
	}
	TSharedPtr<FJsonObject> TransformJson;
	FTransform LevelTransform = FTransform::Identity;
	if (JsonTryGetObject(Ctx.Params, TEXT("transform"), TransformJson) && UeAgentLevelTopology::TryReadTransform(TransformJson, LevelTransform))
	{
		StreamingLevel->LevelTransform = LevelTransform;
	}
	OutData = UeAgentLevelTopology::BuildStreamingLevelJson(StreamingLevel, World);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.level_streaming.state_result.v1"));
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("transaction"), bTransaction);
	return true;
}

bool FUeAgentHttpServer::CmdLevelStreamingMoveActors(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FString TargetLevel;
	if (!JsonTryGetString(Ctx.Params, TEXT("target_level"), TargetLevel) && !JsonTryGetString(Ctx.Params, TEXT("level_package"), TargetLevel))
	{
		OutError = TEXT("missing_target_level");
		return false;
	}
	ULevelStreaming* StreamingLevel = UeAgentLevelTopology::FindStreamingLevel(World, TargetLevel);
	if (!StreamingLevel || !StreamingLevel->GetLoadedLevel())
	{
		OutError = TEXT("streaming_level_not_loaded");
		return false;
	}
	TArray<AActor*> Actors;
	TArray<TSharedPtr<FJsonValue>> ActorResults;
	if (!UeAgentLevelTopology::ResolveActors(World, Ctx.Params, Actors, ActorResults, OutError))
	{
		return false;
	}
	bool bWarnAboutReferences = false;
	bool bWarnAboutRenaming = false;
	bool bMoveAllOrFail = false;
	bool bTransaction = true;
	JsonTryGetBool(Ctx.Params, TEXT("warn_about_references"), bWarnAboutReferences);
	JsonTryGetBool(Ctx.Params, TEXT("warn_about_renaming"), bWarnAboutRenaming);
	JsonTryGetBool(Ctx.Params, TEXT("move_all_or_fail"), bMoveAllOrFail);
	JsonTryGetBool(Ctx.Params, TEXT("transaction"), bTransaction);
	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UeAgentInterface", "LevelStreamingMoveActors", "UeAgentInterface Level Streaming Move Actors"));
	}
	TArray<AActor*> MovedActors;
	const int32 MovedCount = UEditorLevelUtils::MoveActorsToLevel(Actors, StreamingLevel->GetLoadedLevel(), bWarnAboutReferences, bWarnAboutRenaming, bMoveAllOrFail, &MovedActors);
	TArray<TSharedPtr<FJsonValue>> MovedValues;
	for (AActor* Actor : MovedActors)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Actor ? Actor->GetName() : FString());
		Item->SetStringField(TEXT("label"), Actor ? Actor->GetActorLabel() : FString());
		Item->SetStringField(TEXT("level"), Actor && Actor->GetLevel() && Actor->GetLevel()->GetOutermost() ? Actor->GetLevel()->GetOutermost()->GetName() : FString());
		MovedValues.Add(MakeShared<FJsonValueObject>(Item));
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.level_streaming.move_actors_result.v1"));
	OutData->SetStringField(TEXT("target_level"), UeAgentLevelTopology::NormalizePackagePath(StreamingLevel->GetWorldAssetPackageName()));
	OutData->SetNumberField(TEXT("matched_count"), Actors.Num());
	OutData->SetNumberField(TEXT("moved_count"), MovedCount);
	OutData->SetArrayField(TEXT("actor_results"), ActorResults);
	OutData->SetArrayField(TEXT("moved_actors"), MovedValues);
	OutData->SetBoolField(TEXT("transaction"), bTransaction);
	return MovedCount > 0 || Actors.IsEmpty();
}

bool FUeAgentHttpServer::CmdLevelStreamingSave(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<UPackage*> Packages;
	TArray<FString> LevelPackages = UeAgentLevelTopology::ReadStringArray(Ctx.Params, TEXT("level_packages"));
	if (LevelPackages.IsEmpty())
	{
		FString SingleLevel;
		if (JsonTryGetString(Ctx.Params, TEXT("level_package"), SingleLevel) || JsonTryGetString(Ctx.Params, TEXT("target_level"), SingleLevel))
		{
			LevelPackages.Add(SingleLevel);
		}
	}
	for (const FString& LevelPackage : LevelPackages)
	{
		if (ULevelStreaming* StreamingLevel = UeAgentLevelTopology::FindStreamingLevel(World, LevelPackage))
		{
			if (StreamingLevel->GetLoadedLevel() && StreamingLevel->GetLoadedLevel()->GetOutermost())
			{
				Packages.AddUnique(StreamingLevel->GetLoadedLevel()->GetOutermost());
			}
		}
	}
	if (Packages.IsEmpty())
	{
		OutError = TEXT("streaming_level_not_found");
		return false;
	}
	TArray<FString> SavedPackages;
	if (!UeAgentLevelTopology::SavePackages(Packages, SavedPackages, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> SavedValues;
	for (const FString& SavedPackage : SavedPackages)
	{
		SavedValues.Add(MakeShared<FJsonValueString>(SavedPackage));
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.level_streaming.save_result.v1"));
	OutData->SetArrayField(TEXT("saved_packages"), SavedValues);
	OutData->SetNumberField(TEXT("saved_count"), SavedPackages.Num());
	return true;
}

bool FUeAgentHttpServer::CmdLevelStreamingValidate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	FString LevelPackage;
	JsonTryGetString(Ctx.Params, TEXT("level_package"), LevelPackage);
	ULevelStreaming* StreamingLevel = LevelPackage.IsEmpty() ? nullptr : UeAgentLevelTopology::FindStreamingLevel(World, LevelPackage);
	if (!LevelPackage.IsEmpty() && !StreamingLevel)
	{
		UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("streaming_level_not_found"), TEXT("$.level_package"), TEXT("Streaming level was not found in the current world."));
	}
	bool ExpectedLoaded = false;
	if (JsonTryGetBool(Ctx.Params, TEXT("expected_loaded"), ExpectedLoaded) && StreamingLevel && StreamingLevel->IsLevelLoaded() != ExpectedLoaded)
	{
		UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("streaming_loaded_mismatch"), TEXT("$.expected_loaded"), TEXT("Streaming level loaded state does not match."));
	}
	bool ExpectedVisible = false;
	if (JsonTryGetBool(Ctx.Params, TEXT("expected_visible"), ExpectedVisible) && StreamingLevel && StreamingLevel->IsLevelVisible() != ExpectedVisible)
	{
		UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("streaming_visible_mismatch"), TEXT("$.expected_visible"), TEXT("Streaming level visible state does not match."));
	}
	TArray<AActor*> Actors;
	TArray<TSharedPtr<FJsonValue>> ActorResults;
	FString ActorError;
	if (Ctx.Params->HasField(TEXT("actor_ids")) && UeAgentLevelTopology::ResolveActors(World, Ctx.Params, Actors, ActorResults, ActorError))
	{
		for (AActor* Actor : Actors)
		{
			if (StreamingLevel && Actor && Actor->GetLevel() != StreamingLevel->GetLoadedLevel())
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("actor_level_mismatch"), Actor->GetActorLabel(), TEXT("Actor is not in expected streaming level."));
			}
		}
	}
	const int32 ErrorCount = UeAgentLevelTopology::CountIssues(Issues, TEXT("error"));
	OutData->SetStringField(TEXT("schema"), TEXT("uai.level_streaming.validate_result.v1"));
	OutData->SetStringField(TEXT("status"), ErrorCount == 0 ? TEXT("valid") : TEXT("invalid"));
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), ErrorCount);
	OutData->SetArrayField(TEXT("actor_results"), ActorResults);
	return ErrorCount == 0;
}

bool FUeAgentHttpServer::CmdDataLayerCreateAsset(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	AssetPath = UeAgentLevelTopology::NormalizePackagePath(AssetPath);
	FString OverwritePolicy = TEXT("fail");
	JsonTryGetString(Ctx.Params, TEXT("overwrite_policy"), OverwritePolicy);
	bool bSaveAfterCreate = false;
	bool bTransaction = true;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	JsonTryGetBool(Ctx.Params, TEXT("transaction"), bTransaction);
	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UeAgentInterface", "DataLayerCreateAsset", "UeAgentInterface DataLayer Create Asset"));
	}
	UDataLayerFactory* Factory = NewObject<UDataLayerFactory>();
	UObject* CreatedAsset = nullptr;
	if (!UeAgentLevelTopology::CreateOrLoadAssetWithFactory(AssetPath, UDataLayerAsset::StaticClass(), Factory, OverwritePolicy, CreatedAsset, OutError))
	{
		return false;
	}
	UDataLayerAsset* DataLayerAsset = Cast<UDataLayerAsset>(CreatedAsset);
	if (!DataLayerAsset)
	{
		OutError = TEXT("data_layer_asset_create_failed");
		return false;
	}
	DataLayerAsset->Modify();
	FString TypeText;
	if (JsonTryGetString(Ctx.Params, TEXT("type"), TypeText))
	{
		EDataLayerType Type = EDataLayerType::Runtime;
		if (!UeAgentLevelTopology::ParseDataLayerType(TypeText, Type))
		{
			OutError = TEXT("data_layer_type_invalid");
			return false;
		}
		DataLayerAsset->SetType(Type);
	}
	FString DebugColorText;
	if (JsonTryGetString(Ctx.Params, TEXT("debug_color"), DebugColorText))
	{
		FColor DebugColor;
		if (DebugColor.InitFromString(DebugColorText))
		{
			DataLayerAsset->SetDebugColor(DebugColor);
		}
	}
	FString LoadFilterText;
	if (JsonTryGetString(Ctx.Params, TEXT("load_filter"), LoadFilterText))
	{
		TSharedPtr<FJsonObject> ImportResult;
		UeAgentLevelTopology::ImportPropertyText(DataLayerAsset, TEXT("LoadFilter"), LoadFilterText, ImportResult);
	}
	DataLayerAsset->OnCreated();
	DataLayerAsset->MarkPackageDirty();
	TArray<FString> SavedPackages;
	if (bSaveAfterCreate)
	{
		TArray<UPackage*> Packages;
		Packages.Add(DataLayerAsset->GetOutermost());
		if (!UeAgentLevelTopology::SavePackages(Packages, SavedPackages, OutError))
		{
			return false;
		}
	}
	OutData = UeAgentLevelTopology::DataLayerAssetToJson(DataLayerAsset);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.data_layer.create_asset_result.v1"));
	OutData->SetBoolField(TEXT("created_or_updated"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	OutData->SetBoolField(TEXT("transaction"), bTransaction);
	return true;
}

bool FUeAgentHttpServer::CmdDataLayerApplyAssetJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString Source;
	if (!UeAgentLevelTopology::LoadJsonFromParams(Ctx.Params, Root, Source, OutError))
	{
		return false;
	}
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath))
	{
		Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	FString OverwritePolicy = TEXT("replace");
	JsonTryGetString(Ctx.Params, TEXT("overwrite_policy"), OverwritePolicy);
	Params->SetStringField(TEXT("overwrite_policy"), OverwritePolicy);
	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	Params->SetBoolField(TEXT("save_after_create"), bSaveAfterApply);
	bool bTransaction = true;
	JsonTryGetBool(Ctx.Params, TEXT("transaction"), bTransaction);
	Params->SetBoolField(TEXT("transaction"), bTransaction);
	FString TypeText;
	if (Root->TryGetStringField(TEXT("type"), TypeText))
	{
		Params->SetStringField(TEXT("type"), TypeText);
	}
	FString LoadFilterText;
	if (Root->TryGetStringField(TEXT("load_filter"), LoadFilterText))
	{
		Params->SetStringField(TEXT("load_filter"), LoadFilterText);
	}
	const TSharedPtr<FJsonObject>* ColorObj = nullptr;
	if (Root->TryGetObjectField(TEXT("debug_color"), ColorObj) && ColorObj && ColorObj->IsValid())
	{
		const int32 R = static_cast<int32>((*ColorObj)->GetNumberField(TEXT("r")));
		const int32 G = static_cast<int32>((*ColorObj)->GetNumberField(TEXT("g")));
		const int32 B = static_cast<int32>((*ColorObj)->GetNumberField(TEXT("b")));
		const int32 A = (*ColorObj)->HasField(TEXT("a")) ? static_cast<int32>((*ColorObj)->GetNumberField(TEXT("a"))) : 255;
		Params->SetStringField(TEXT("debug_color"), FString::Printf(TEXT("(R=%d,G=%d,B=%d,A=%d)"), R, G, B, A));
	}
	FUeAgentRequestContext SubCtx = Ctx;
	SubCtx.Params = Params;
	const bool bOk = CmdDataLayerCreateAsset(SubCtx, OutData, OutError);
	if (OutData.IsValid())
	{
		OutData->SetStringField(TEXT("source"), Source);
		OutData->SetStringField(TEXT("status"), bOk ? TEXT("applied") : TEXT("apply_failed"));
	}
	return bOk;
}

bool FUeAgentHttpServer::CmdDataLayerCreateInstance(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	UDataLayerEditorSubsystem* Subsystem = UeAgentLevelTopology::GetDataLayerSubsystem();
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UDataLayerAsset* DataLayerAsset = UeAgentLevelTopology::LoadTypedAsset<UDataLayerAsset>(AssetPath);
	if (!DataLayerAsset)
	{
		OutError = TEXT("data_layer_asset_not_found");
		return false;
	}
	if (!UeAgentLevelTopology::SupportsDataLayerInstanceAuthoring(World))
	{
		OutError = TEXT("level_external_objects_required_for_data_layer");
		return false;
	}
	AWorldDataLayers* WorldDataLayers = UeAgentLevelTopology::GetOrCreateWorldDataLayers(World);
	if (!WorldDataLayers)
	{
		OutError = TEXT("world_data_layers_unavailable");
		return false;
	}
	if (const UDataLayerInstance* ExistingConst = WorldDataLayers->GetDataLayerInstance(DataLayerAsset))
	{
		UDataLayerInstance* Existing = const_cast<UDataLayerInstance*>(ExistingConst);
		TArray<TSharedPtr<FJsonValue>> Warnings;
		bool bChanged = false;
		if (!UeAgentLevelTopology::ApplyDataLayerStateFields(World, Subsystem, Existing, Ctx.Params, Warnings, bChanged, OutError))
		{
			return false;
		}
		OutData = UeAgentLevelTopology::DataLayerInstanceToJson(Existing, World, true);
		OutData->SetStringField(TEXT("schema"), TEXT("uai.data_layer.create_instance_result.v1"));
		OutData->SetBoolField(TEXT("created"), false);
		OutData->SetBoolField(TEXT("changed"), bChanged);
		OutData->SetArrayField(TEXT("warnings"), Warnings);
		OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
		return true;
	}
	bool bPrivate = false;
	bool bTransaction = true;
	JsonTryGetBool(Ctx.Params, TEXT("private"), bPrivate);
	JsonTryGetBool(Ctx.Params, TEXT("transaction"), bTransaction);
	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UeAgentInterface", "DataLayerCreateInstance", "UeAgentInterface DataLayer Create Instance"));
	}
	UDataLayerInstance* Instance = nullptr;
	if (Subsystem)
	{
		FDataLayerCreationParameters Parameters;
		Parameters.DataLayerAsset = DataLayerAsset;
		Parameters.WorldDataLayers = WorldDataLayers;
		Parameters.bIsPrivate = bPrivate;
		Instance = Subsystem->CreateDataLayerInstance(Parameters);
	}
	else
	{
		WorldDataLayers->Modify();
		Instance = WorldDataLayers->CreateDataLayer<UDataLayerInstanceWithAsset>(DataLayerAsset);
	}
	if (!Instance)
	{
		OutError = TEXT("data_layer_instance_create_failed");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Warnings;
	bool bChanged = true;
	if (!UeAgentLevelTopology::ApplyDataLayerStateFields(World, Subsystem, Instance, Ctx.Params, Warnings, bChanged, OutError))
	{
		return false;
	}
	OutData = UeAgentLevelTopology::DataLayerInstanceToJson(Instance, World, true);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.data_layer.create_instance_result.v1"));
	OutData->SetBoolField(TEXT("created"), true);
	OutData->SetBoolField(TEXT("changed"), bChanged);
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetBoolField(TEXT("transaction"), bTransaction);
	return true;
}

bool FUeAgentHttpServer::CmdDataLayerDeleteInstance(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	UDataLayerEditorSubsystem* Subsystem = UeAgentLevelTopology::GetDataLayerSubsystem();
	FString Layer;
	if (!JsonTryGetString(Ctx.Params, TEXT("layer"), Layer) && !JsonTryGetString(Ctx.Params, TEXT("instance_name"), Layer))
	{
		OutError = TEXT("missing_layer");
		return false;
	}
	UDataLayerInstance* Instance = UeAgentLevelTopology::FindDataLayerInstance(World, Layer);
	if (!Instance)
	{
		OutError = TEXT("data_layer_instance_not_found");
		return false;
	}
	const FString InstanceName = Instance->GetDataLayerFName().ToString();
	const UDataLayerAsset* Asset = Instance->GetAsset();
	bool bDeleteAsset = false;
	bool bTransaction = true;
	JsonTryGetBool(Ctx.Params, TEXT("delete_asset"), bDeleteAsset);
	JsonTryGetBool(Ctx.Params, TEXT("transaction"), bTransaction);
	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UeAgentInterface", "DataLayerDeleteInstance", "UeAgentInterface DataLayer Delete Instance"));
	}
	if (Subsystem)
	{
		Subsystem->DeleteDataLayer(Instance);
	}
	else
	{
		AWorldDataLayers* WorldDataLayers = UeAgentLevelTopology::FindWorldDataLayers(World);
		if (!WorldDataLayers || !WorldDataLayers->RemoveDataLayer(Instance, true))
		{
			OutError = TEXT("data_layer_instance_delete_failed");
			return false;
		}
		WorldDataLayers->MarkPackageDirty();
	}
	int32 DeletedAssetCount = 0;
	if (bDeleteAsset && Asset)
	{
		TArray<UObject*> ObjectsToDelete;
		ObjectsToDelete.Add(const_cast<UDataLayerAsset*>(Asset));
		DeletedAssetCount = ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete);
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.data_layer.delete_instance_result.v1"));
	OutData->SetStringField(TEXT("instance_name"), InstanceName);
	OutData->SetBoolField(TEXT("deleted"), true);
	OutData->SetBoolField(TEXT("delete_asset"), bDeleteAsset);
	OutData->SetNumberField(TEXT("deleted_asset_count"), DeletedAssetCount);
	OutData->SetBoolField(TEXT("transaction"), bTransaction);
	return true;
}

bool FUeAgentHttpServer::CmdDataLayerSetState(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	UDataLayerEditorSubsystem* Subsystem = UeAgentLevelTopology::GetDataLayerSubsystem();
	FString Layer;
	if (!JsonTryGetString(Ctx.Params, TEXT("layer"), Layer) && !JsonTryGetString(Ctx.Params, TEXT("instance_name"), Layer))
	{
		OutError = TEXT("missing_layer");
		return false;
	}
	UDataLayerInstance* Instance = UeAgentLevelTopology::FindDataLayerInstance(World, Layer);
	if (!Instance)
	{
		OutError = TEXT("data_layer_instance_not_found");
		return false;
	}
	bool bTransaction = true;
	JsonTryGetBool(Ctx.Params, TEXT("transaction"), bTransaction);
	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UeAgentInterface", "DataLayerSetState", "UeAgentInterface DataLayer Set State"));
	}
	TArray<TSharedPtr<FJsonValue>> Warnings;
	bool bChanged = false;
	if (!UeAgentLevelTopology::ApplyDataLayerStateFields(World, Subsystem, Instance, Ctx.Params, Warnings, bChanged, OutError))
	{
		return false;
	}
	OutData = UeAgentLevelTopology::DataLayerInstanceToJson(Instance, World, true);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.data_layer.state_result.v1"));
	OutData->SetBoolField(TEXT("changed"), bChanged);
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetBoolField(TEXT("transaction"), bTransaction);
	return true;
}

bool FUeAgentHttpServer::CmdDataLayerSetActorMembership(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	UDataLayerEditorSubsystem* Subsystem = UeAgentLevelTopology::GetDataLayerSubsystem();
	TArray<AActor*> Actors;
	TArray<TSharedPtr<FJsonValue>> ActorResults;
	if (!UeAgentLevelTopology::ResolveActors(World, Ctx.Params, Actors, ActorResults, OutError))
	{
		return false;
	}
	TArray<FString> LayerNames = UeAgentLevelTopology::ReadStringArray(Ctx.Params, TEXT("layers"));
	if (LayerNames.IsEmpty())
	{
		FString SingleLayer;
		if (JsonTryGetString(Ctx.Params, TEXT("layer"), SingleLayer))
		{
			LayerNames.Add(SingleLayer);
		}
	}
	FString Mode = TEXT("add");
	JsonTryGetString(Ctx.Params, TEXT("mode"), Mode);
	TArray<UDataLayerInstance*> Layers;
	for (const FString& LayerName : LayerNames)
	{
		UDataLayerInstance* Layer = UeAgentLevelTopology::FindDataLayerInstance(World, LayerName);
		if (!Layer)
		{
			OutError = TEXT("data_layer_instance_not_found");
			return false;
		}
		Layers.Add(Layer);
	}
	bool bTransaction = true;
	JsonTryGetBool(Ctx.Params, TEXT("transaction"), bTransaction);
	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UeAgentInterface", "DataLayerMembership", "UeAgentInterface DataLayer Membership"));
	}
	int32 ChangedCount = 0;
	int32 ReadbackFailureCount = 0;
	TArray<TSharedPtr<FJsonValue>> Results;
	for (AActor* Actor : Actors)
	{
		if (!Actor)
		{
			continue;
		}
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Actor->GetName());
		Result->SetStringField(TEXT("label"), Actor->GetActorLabel());
		TArray<FName> BeforeNames = Actor->GetDataLayerInstanceNames();
		TArray<bool> BeforeContains;
		for (UDataLayerInstance* Layer : Layers)
		{
			BeforeContains.Add(UeAgentLevelTopology::ActorContainsDataLayerInstance(Actor, Layer));
		}
		Actor->Modify();
		bool bChanged = false;
		if (Mode.Equals(TEXT("clear"), ESearchCase::IgnoreCase))
		{
			if (Subsystem)
			{
				bChanged = Subsystem->RemoveActorsFromAllDataLayers({ Actor });
			}
			else
			{
				bChanged = Actor->RemoveAllDataLayers().Num() > 0;
			}
			for (UDataLayerInstance* Layer : Layers)
			{
				if (Layer && UeAgentLevelTopology::ActorContainsDataLayerInstance(Actor, Layer))
				{
					bChanged |= UeAgentLevelTopology::RemoveActorFromDataLayerInstanceSafe(Actor, Layer);
				}
			}
			bChanged |= UeAgentLevelTopology::ClearActorDataLayerAssetsDirect(Actor);
		}
		else if (Mode.Equals(TEXT("replace"), ESearchCase::IgnoreCase))
		{
			if (Subsystem)
			{
				Subsystem->RemoveActorsFromAllDataLayers({ Actor });
				bChanged = Layers.IsEmpty() ? !BeforeNames.IsEmpty() : Subsystem->AddActorsToDataLayers({ Actor }, Layers);
			}
			else
			{
				Actor->RemoveAllDataLayers();
				for (UDataLayerInstance* Layer : Layers)
				{
					if (Layer)
					{
						UeAgentLevelTopology::AddActorToDataLayerInstanceSafe(Actor, Layer);
					}
				}
			}
			for (UDataLayerInstance* Layer : Layers)
			{
				if (Layer && !UeAgentLevelTopology::ActorContainsDataLayerInstance(Actor, Layer))
				{
					bChanged |= UeAgentLevelTopology::AddActorToDataLayerInstanceSafe(Actor, Layer);
				}
			}
		}
		else if (Mode.Equals(TEXT("remove"), ESearchCase::IgnoreCase))
		{
			if (Subsystem)
			{
				bChanged = Layers.IsEmpty() ? false : Subsystem->RemoveActorsFromDataLayers({ Actor }, Layers);
			}
			else
			{
				for (UDataLayerInstance* Layer : Layers)
				{
					if (Layer)
					{
						UeAgentLevelTopology::RemoveActorFromDataLayerInstanceSafe(Actor, Layer);
					}
				}
			}
			for (UDataLayerInstance* Layer : Layers)
			{
				if (Layer && UeAgentLevelTopology::ActorContainsDataLayerInstance(Actor, Layer))
				{
					bChanged |= UeAgentLevelTopology::RemoveActorFromDataLayerInstanceSafe(Actor, Layer);
				}
			}
		}
		else
		{
			if (Subsystem)
			{
				bChanged = Layers.IsEmpty() ? false : Subsystem->AddActorsToDataLayers({ Actor }, Layers);
			}
			else
			{
				for (UDataLayerInstance* Layer : Layers)
				{
					if (Layer)
					{
						UeAgentLevelTopology::AddActorToDataLayerInstanceSafe(Actor, Layer);
					}
				}
			}
			for (UDataLayerInstance* Layer : Layers)
			{
				if (Layer && !UeAgentLevelTopology::ActorContainsDataLayerInstance(Actor, Layer))
				{
					bChanged |= UeAgentLevelTopology::AddActorToDataLayerInstanceSafe(Actor, Layer);
				}
			}
		}
		Actor->MarkPackageDirty();
		Actor->MarkComponentsRenderStateDirty();
		TArray<FName> AfterNames = Actor->GetDataLayerInstanceNames();
		TArray<TSharedPtr<FJsonValue>> MembershipValues;
		bool bReadbackMatches = true;
		for (int32 LayerIndex = 0; LayerIndex < Layers.Num(); ++LayerIndex)
		{
			UDataLayerInstance* Layer = Layers[LayerIndex];
			const bool bAfterContains = UeAgentLevelTopology::ActorContainsDataLayerInstance(Actor, Layer);
			const bool bBeforeContains = BeforeContains.IsValidIndex(LayerIndex) ? BeforeContains[LayerIndex] : false;
			TSharedPtr<FJsonObject> Membership = MakeShared<FJsonObject>();
			Membership->SetStringField(TEXT("layer"), Layer ? Layer->GetDataLayerFName().ToString() : FString());
			Membership->SetBoolField(TEXT("before_contains"), bBeforeContains);
			Membership->SetBoolField(TEXT("after_contains"), bAfterContains);
			MembershipValues.Add(MakeShared<FJsonValueObject>(Membership));
			if (Mode.Equals(TEXT("remove"), ESearchCase::IgnoreCase) || Mode.Equals(TEXT("clear"), ESearchCase::IgnoreCase))
			{
				bReadbackMatches &= !bAfterContains;
			}
			else
			{
				bReadbackMatches &= bAfterContains;
			}
			bChanged |= (bBeforeContains != bAfterContains);
		}
		if (Mode.Equals(TEXT("clear"), ESearchCase::IgnoreCase) || (Mode.Equals(TEXT("replace"), ESearchCase::IgnoreCase) && Layers.IsEmpty()))
		{
			bReadbackMatches &= !Actor->HasDataLayers();
		}
		if (!bReadbackMatches)
		{
			++ReadbackFailureCount;
		}
		if (!Subsystem || !bChanged)
		{
			bChanged = !UeAgentLevelTopology::NamesEqualAsSet(BeforeNames, AfterNames);
		}
		if (bChanged)
		{
			++ChangedCount;
		}
		TArray<TSharedPtr<FJsonValue>> BeforeValues;
		for (const FName& Name : BeforeNames)
		{
			BeforeValues.Add(MakeShared<FJsonValueString>(Name.ToString()));
		}
		TArray<TSharedPtr<FJsonValue>> AfterValues;
		for (const FName& Name : AfterNames)
		{
			AfterValues.Add(MakeShared<FJsonValueString>(Name.ToString()));
		}
		Result->SetArrayField(TEXT("before"), BeforeValues);
		Result->SetArrayField(TEXT("after"), AfterValues);
		Result->SetArrayField(TEXT("memberships"), MembershipValues);
		Result->SetBoolField(TEXT("readback_matches"), bReadbackMatches);
		Result->SetBoolField(TEXT("changed"), bChanged);
		Results.Add(MakeShared<FJsonValueObject>(Result));
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.data_layer.membership_result.v1"));
	OutData->SetStringField(TEXT("mode"), Mode);
	OutData->SetNumberField(TEXT("matched_count"), Actors.Num());
	OutData->SetNumberField(TEXT("changed_count"), ChangedCount);
	OutData->SetNumberField(TEXT("readback_failure_count"), ReadbackFailureCount);
	OutData->SetArrayField(TEXT("actor_results"), ActorResults);
	OutData->SetArrayField(TEXT("results"), Results);
	OutData->SetBoolField(TEXT("transaction"), bTransaction);
	if (ReadbackFailureCount > 0)
	{
		OutError = TEXT("data_layer_membership_readback_failed");
		return false;
	}
	return true;
}

bool FUeAgentHttpServer::CmdDataLayerQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	bool bIncludeActors = false;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetBoolField(TEXT("include_actors"), bIncludeActors);
	}
	TArray<TSharedPtr<FJsonValue>> InstanceValues;
	if (AWorldDataLayers* WorldDataLayers = UeAgentLevelTopology::FindWorldDataLayers(World))
	{
		WorldDataLayers->ForEachDataLayerInstance([World, bIncludeActors, &InstanceValues](UDataLayerInstance* Instance)
		{
			InstanceValues.Add(MakeShared<FJsonValueObject>(UeAgentLevelTopology::DataLayerInstanceToJson(Instance, World, bIncludeActors)));
			return true;
		});
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.data_layer.query.v1"));
	OutData->SetArrayField(TEXT("data_layers"), InstanceValues);
	OutData->SetNumberField(TEXT("count"), InstanceValues.Num());
	return true;
}

bool FUeAgentHttpServer::CmdDataLayerValidate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	for (const FString& LayerName : UeAgentLevelTopology::ReadStringArray(Ctx.Params, TEXT("layers")))
	{
		if (!UeAgentLevelTopology::FindDataLayerInstance(World, LayerName))
		{
			UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("data_layer_instance_not_found"), LayerName, TEXT("DataLayer instance was not found."));
		}
	}
	TArray<AActor*> Actors;
	TArray<TSharedPtr<FJsonValue>> ActorResults;
	FString ActorError;
	if (Ctx.Params->HasField(TEXT("actor_ids")) && UeAgentLevelTopology::ResolveActors(World, Ctx.Params, Actors, ActorResults, ActorError))
	{
		TArray<FString> ExpectedMembership = UeAgentLevelTopology::ReadStringArray(Ctx.Params, TEXT("expect_actor_membership"));
		for (AActor* Actor : Actors)
		{
			for (const FString& Expected : ExpectedMembership)
			{
				UDataLayerInstance* Layer = UeAgentLevelTopology::FindDataLayerInstance(World, Expected);
				if (!Layer || !UeAgentLevelTopology::ActorContainsDataLayerInstance(Actor, Layer))
				{
					UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("actor_data_layer_membership_missing"), Actor ? Actor->GetActorLabel() : FString(), Expected);
				}
			}
		}
	}
	const int32 ErrorCount = UeAgentLevelTopology::CountIssues(Issues, TEXT("error"));
	OutData->SetStringField(TEXT("schema"), TEXT("uai.data_layer.validate_result.v1"));
	OutData->SetStringField(TEXT("status"), ErrorCount == 0 ? TEXT("valid") : TEXT("invalid"));
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), ErrorCount);
	OutData->SetArrayField(TEXT("actor_results"), ActorResults);
	return ErrorCount == 0;
}

bool FUeAgentHttpServer::CmdWorldPartitionQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	UWorldPartition* WorldPartition = World->GetWorldPartition();
	const bool bEnabled = WorldPartition != nullptr;
	OutData->SetStringField(TEXT("schema"), TEXT("uai.world_partition.query.v1"));
	OutData->SetStringField(TEXT("world"), World->GetPathName());
	OutData->SetBoolField(TEXT("enabled"), bEnabled);
	if (!WorldPartition)
	{
		OutData->SetBoolField(TEXT("streaming_enabled"), false);
		OutData->SetArrayField(TEXT("loaded_regions"), TArray<TSharedPtr<FJsonValue>>());
		return true;
	}
	OutData->SetStringField(TEXT("editor_name"), WorldPartition->GetWorldPartitionEditorName().ToString());
	OutData->SetBoolField(TEXT("streaming_enabled"), WorldPartition->IsStreamingEnabledInEditor());
	OutData->SetObjectField(TEXT("editor_world_bounds"), UeAgentLevelTopology::BoxToJson(WorldPartition->GetEditorWorldBounds()));
	OutData->SetObjectField(TEXT("runtime_world_bounds"), UeAgentLevelTopology::BoxToJson(WorldPartition->GetRuntimeWorldBounds()));
	TArray<TSharedPtr<FJsonValue>> RegionValues;
	for (const FBox& Region : WorldPartition->GetUserLoadedEditorRegions())
	{
		RegionValues.Add(MakeShared<FJsonValueObject>(UeAgentLevelTopology::BoxToJson(Region)));
	}
	OutData->SetArrayField(TEXT("loaded_regions"), RegionValues);
	OutData->SetNumberField(TEXT("loaded_region_count"), RegionValues.Num());
	return true;
}

bool FUeAgentHttpServer::CmdWorldPartitionSetStreamingEnabled(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	UWorldPartition* WorldPartition = World->GetWorldPartition();
	if (!WorldPartition)
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("world_partition_not_enabled"));
		OutError = TEXT("world_partition_not_enabled");
		return false;
	}
	bool bEnabled = true;
	if (!JsonTryGetBool(Ctx.Params, TEXT("enabled"), bEnabled))
	{
		OutError = TEXT("missing_enabled");
		return false;
	}
	bool bTransaction = true;
	JsonTryGetBool(Ctx.Params, TEXT("transaction"), bTransaction);
	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UeAgentInterface", "WorldPartitionSetStreaming", "UeAgentInterface World Partition Set Streaming"));
	}
	WorldPartition->Modify();
	WorldPartition->SetEnableStreaming(bEnabled);
	WorldPartition->OnEnableStreamingChanged();
	WorldPartition->MarkPackageDirty();
	OutData->SetStringField(TEXT("schema"), TEXT("uai.world_partition.streaming_result.v1"));
	OutData->SetBoolField(TEXT("streaming_enabled"), WorldPartition->IsStreamingEnabledInEditor());
	OutData->SetBoolField(TEXT("transaction"), bTransaction);
	return true;
}

bool FUeAgentHttpServer::CmdWorldPartitionLoadRegion(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	UWorldPartition* WorldPartition = World->GetWorldPartition();
	if (!WorldPartition)
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("world_partition_not_enabled"));
		OutError = TEXT("world_partition_not_enabled");
		return false;
	}
	bool bDryRun = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	TSharedPtr<FJsonObject> BoundsJson;
	if (!JsonTryGetObject(Ctx.Params, TEXT("bounds"), BoundsJson))
	{
		OutError = TEXT("missing_bounds");
		return false;
	}
	FBox Bounds;
	if (!UeAgentLevelTopology::TryReadBox(BoundsJson, Bounds))
	{
		OutError = TEXT("bounds_invalid");
		return false;
	}
	FString Label = TEXT("UAI_WP_Region");
	JsonTryGetString(Ctx.Params, TEXT("label"), Label);
	const FString LoaderId = FString::Printf(TEXT("wp_region_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	OutData->SetStringField(TEXT("schema"), TEXT("uai.world_partition.load_region_result.v1"));
	OutData->SetStringField(TEXT("loader_id"), LoaderId);
	OutData->SetStringField(TEXT("label"), Label);
	OutData->SetObjectField(TEXT("bounds"), UeAgentLevelTopology::BoxToJson(Bounds));
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	if (bDryRun)
	{
		OutData->SetBoolField(TEXT("loaded"), false);
		OutData->SetStringField(TEXT("status"), TEXT("planned"));
		return true;
	}
	TUniquePtr<UeAgentLevelTopology::FStoredWorldPartitionLoader> Entry = MakeUnique<UeAgentLevelTopology::FStoredWorldPartitionLoader>();
	Entry->LoaderId = LoaderId;
	Entry->Kind = TEXT("region");
	Entry->Label = Label;
	Entry->WorldPath = World->GetPathName();
	Entry->Adapter = MakeUnique<FLoaderAdapterShape>(World, Bounds, Label);
	Entry->Adapter->SetUserCreated(true);
	Entry->Adapter->Load();
	const bool bLoaded = Entry->Adapter->IsLoaded();
	UeAgentLevelTopology::GetLoaderRegistry().Add(LoaderId, MoveTemp(Entry));
	OutData->SetBoolField(TEXT("loaded"), bLoaded);
	OutData->SetStringField(TEXT("status"), bLoaded ? TEXT("loaded") : TEXT("load_requested"));
	return true;
}

bool FUeAgentHttpServer::CmdWorldPartitionUnloadRegion(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString LoaderId;
	if (!JsonTryGetString(Ctx.Params, TEXT("loader_id"), LoaderId))
	{
		OutError = TEXT("missing_loader_id");
		return false;
	}
	TUniquePtr<UeAgentLevelTopology::FStoredWorldPartitionLoader>* EntryPtr = UeAgentLevelTopology::GetLoaderRegistry().Find(LoaderId);
	if (!EntryPtr || !EntryPtr->IsValid())
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("world_partition_loader_not_found"));
		OutError = TEXT("world_partition_loader_not_found");
		return false;
	}
	(*EntryPtr)->Adapter->Unload();
	UeAgentLevelTopology::GetLoaderRegistry().Remove(LoaderId);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.world_partition.unload_region_result.v1"));
	OutData->SetStringField(TEXT("loader_id"), LoaderId);
	OutData->SetBoolField(TEXT("unloaded"), true);
	return true;
}

bool FUeAgentHttpServer::CmdWorldPartitionLoadActors(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	if (!World->GetWorldPartition())
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("world_partition_not_enabled"));
		OutError = TEXT("world_partition_not_enabled");
		return false;
	}
	TArray<FGuid> Guids;
	for (const FString& GuidText : UeAgentLevelTopology::ReadStringArray(Ctx.Params, TEXT("actor_guids")))
	{
		FGuid Guid;
		if (FGuid::Parse(GuidText, Guid))
		{
			Guids.Add(Guid);
		}
	}
	TArray<AActor*> Actors;
	TArray<TSharedPtr<FJsonValue>> ActorResults;
	FString ActorError;
	if (Ctx.Params->HasField(TEXT("actor_ids")) && UeAgentLevelTopology::ResolveActors(World, Ctx.Params, Actors, ActorResults, ActorError))
	{
		for (AActor* Actor : Actors)
		{
			if (Actor)
			{
				Guids.AddUnique(Actor->GetActorGuid());
			}
		}
	}
	if (Guids.IsEmpty())
	{
		OutError = TEXT("missing_actor_guids");
		return false;
	}
	bool bDryRun = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	const FString LoaderId = FString::Printf(TEXT("wp_actor_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	OutData->SetStringField(TEXT("schema"), TEXT("uai.world_partition.load_actors_result.v1"));
	OutData->SetStringField(TEXT("loader_id"), LoaderId);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetNumberField(TEXT("actor_guid_count"), Guids.Num());
	OutData->SetArrayField(TEXT("actor_results"), ActorResults);
	if (bDryRun)
	{
		OutData->SetStringField(TEXT("status"), TEXT("planned"));
		return true;
	}
	TUniquePtr<UeAgentLevelTopology::FStoredWorldPartitionLoader> Entry = MakeUnique<UeAgentLevelTopology::FStoredWorldPartitionLoader>();
	Entry->LoaderId = LoaderId;
	Entry->Kind = TEXT("actors");
	Entry->Label = TEXT("UAI_WP_Actors");
	Entry->WorldPath = World->GetPathName();
	TUniquePtr<FLoaderAdapterActorList> ActorList = MakeUnique<FLoaderAdapterActorList>(World);
	ActorList->AddActors(Guids);
	Entry->Adapter = MoveTemp(ActorList);
	Entry->Adapter->SetUserCreated(true);
	Entry->Adapter->Load();
	const bool bLoaded = Entry->Adapter->IsLoaded();
	UeAgentLevelTopology::GetLoaderRegistry().Add(LoaderId, MoveTemp(Entry));
	OutData->SetBoolField(TEXT("loaded"), bLoaded);
	OutData->SetStringField(TEXT("status"), bLoaded ? TEXT("loaded") : TEXT("load_requested"));
	return true;
}

bool FUeAgentHttpServer::CmdWorldPartitionUnloadActors(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdWorldPartitionUnloadRegion(Ctx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdWorldPartitionListLoadedRegions(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> UaiLoaders;
	for (const TPair<FString, TUniquePtr<UeAgentLevelTopology::FStoredWorldPartitionLoader>>& Pair : UeAgentLevelTopology::GetLoaderRegistry())
	{
		if (!Pair.Value.IsValid())
		{
			continue;
		}
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("loader_id"), Pair.Value->LoaderId);
		Obj->SetStringField(TEXT("kind"), Pair.Value->Kind);
		Obj->SetStringField(TEXT("label"), Pair.Value->Label);
		Obj->SetStringField(TEXT("world"), Pair.Value->WorldPath);
		Obj->SetBoolField(TEXT("loaded"), Pair.Value->Adapter.IsValid() && Pair.Value->Adapter->IsLoaded());
		UaiLoaders.Add(MakeShared<FJsonValueObject>(Obj));
	}
	TArray<TSharedPtr<FJsonValue>> UserRegions;
	if (UWorldPartition* WorldPartition = World->GetWorldPartition())
	{
		for (const FBox& Region : WorldPartition->GetUserLoadedEditorRegions())
		{
			UserRegions.Add(MakeShared<FJsonValueObject>(UeAgentLevelTopology::BoxToJson(Region)));
		}
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.world_partition.loaded_regions.v1"));
	OutData->SetArrayField(TEXT("uai_loaders"), UaiLoaders);
	OutData->SetArrayField(TEXT("user_loaded_regions"), UserRegions);
	return true;
}

bool FUeAgentHttpServer::CmdWorldPartitionCheckErrors(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	UWorldPartition* WorldPartition = World->GetWorldPartition();
	if (!WorldPartition)
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("world_partition_not_enabled"));
		OutError = TEXT("world_partition_not_enabled");
		return false;
	}
	bool bEnableStreaming = WorldPartition->IsStreamingEnabledInEditor();
	JsonTryGetBool(Ctx.Params, TEXT("enable_streaming"), bEnableStreaming);
	UeAgentLevelTopology::FCollectingWorldPartitionErrorHandler Handler;
	UWorldPartition::FCheckForErrorsParams Params;
	Params.SetErrorHandler(&Handler);
	Params.SetActorDescContainerInstanceCollection(WorldPartition);
	Params.SetEnableStreaming(bEnableStreaming);
	UWorldPartition::CheckForErrors(Params);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.world_partition.check_errors_result.v1"));
	OutData->SetArrayField(TEXT("issues"), Handler.Issues);
	OutData->SetNumberField(TEXT("error_count"), Handler.Issues.Num());
	OutData->SetStringField(TEXT("status"), Handler.Issues.IsEmpty() ? TEXT("valid") : TEXT("invalid"));
	return true;
}

bool FUeAgentHttpServer::CmdWorldPartitionGenerateStreaming(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	UWorldPartition* WorldPartition = World->GetWorldPartition();
	if (!WorldPartition)
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("world_partition_not_enabled"));
		OutError = TEXT("world_partition_not_enabled");
		return false;
	}
	bool bDryRun = true;
	bool bAllowWrite = false;
	bool bAllowCommandlet = false;
	double TimeoutSeconds = 600.0;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("allow_write"), bAllowWrite);
	JsonTryGetBool(Ctx.Params, TEXT("allow_commandlet"), bAllowCommandlet);
	JsonTryGetNumber(Ctx.Params, TEXT("timeout_seconds"), TimeoutSeconds);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.world_partition.generate_streaming_result.v1"));
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("allow_write"), bAllowWrite);
	OutData->SetBoolField(TEXT("allow_commandlet"), bAllowCommandlet);
	OutData->SetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
	const FString Args = TEXT("-run=WorldPartitionBuilderCommandlet -Builder=WorldPartitionBuilder -AllowCommandletRendering");
	OutData->SetStringField(TEXT("commandlet_plan"), Args);
	if (bDryRun || !bAllowWrite)
	{
		OutData->SetStringField(TEXT("status"), TEXT("planned"));
		return true;
	}
	if (!bAllowCommandlet)
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("requires_commandlet"));
		OutError = TEXT("requires_commandlet");
		return false;
	}
	const bool bOk = UeAgentLevelTopology::RunCommandletProcess(Args, TimeoutSeconds, OutData, OutError);
	OutData->SetStringField(TEXT("status"), bOk ? TEXT("generated") : TEXT("failed"));
	return bOk;
}

bool FUeAgentHttpServer::CmdWorldPartitionResaveActors(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	bool bExecute = false;
	bool bAllowCommandlet = false;
	double TimeoutSeconds = 600.0;
	JsonTryGetBool(Ctx.Params, TEXT("execute"), bExecute);
	JsonTryGetBool(Ctx.Params, TEXT("allow_commandlet"), bAllowCommandlet);
	JsonTryGetNumber(Ctx.Params, TEXT("timeout_seconds"), TimeoutSeconds);
	const FString Args = TEXT("-run=WorldPartitionBuilderCommandlet -Builder=WorldPartitionResaveActorsBuilder");
	OutData->SetStringField(TEXT("schema"), TEXT("uai.world_partition.resave_actors_result.v1"));
	OutData->SetStringField(TEXT("commandlet_plan"), Args);
	OutData->SetBoolField(TEXT("execute"), bExecute);
	OutData->SetBoolField(TEXT("allow_commandlet"), bAllowCommandlet);
	if (!bExecute)
	{
		OutData->SetStringField(TEXT("status"), TEXT("planned"));
		return true;
	}
	if (!bAllowCommandlet)
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("requires_commandlet"));
		OutError = TEXT("requires_commandlet");
		return false;
	}
	const bool bOk = UeAgentLevelTopology::RunCommandletProcess(Args, TimeoutSeconds, OutData, OutError);
	OutData->SetStringField(TEXT("status"), bOk ? TEXT("resaved") : TEXT("failed"));
	return bOk;
}

bool FUeAgentHttpServer::CmdHLODLayerApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString Source;
	if (!UeAgentLevelTopology::LoadJsonFromParams(Ctx.Params, Root, Source, OutError))
	{
		return false;
	}
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath))
	{
		Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString OverwritePolicy = TEXT("replace");
	JsonTryGetString(Ctx.Params, TEXT("overwrite_policy"), OverwritePolicy);
	bool bSaveAfterApply = false;
	bool bTransaction = true;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	JsonTryGetBool(Ctx.Params, TEXT("transaction"), bTransaction);
	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UeAgentInterface", "HLODLayerApply", "UeAgentInterface HLOD Layer Apply"));
	}
	UHLODLayerFactory* Factory = NewObject<UHLODLayerFactory>();
	UObject* AssetObject = nullptr;
	if (!UeAgentLevelTopology::CreateOrLoadAssetWithFactory(UeAgentLevelTopology::NormalizePackagePath(AssetPath), UHLODLayer::StaticClass(), Factory, OverwritePolicy, AssetObject, OutError))
	{
		return false;
	}
	UHLODLayer* Layer = Cast<UHLODLayer>(AssetObject);
	if (!Layer)
	{
		OutError = TEXT("hlod_layer_create_failed");
		return false;
	}
	Layer->Modify();
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	FString LayerTypeText;
	if (Root->TryGetStringField(TEXT("layer_type"), LayerTypeText))
	{
		EHLODLayerType LayerType = EHLODLayerType::Instancing;
		if (!UeAgentLevelTopology::ParseHLODLayerType(LayerTypeText, LayerType))
		{
			OutError = TEXT("hlod_layer_type_invalid");
			return false;
		}
		Layer->SetLayerType(LayerType);
	}
	bool bIsSpatiallyLoaded = true;
	if (Root->TryGetBoolField(TEXT("is_spatially_loaded"), bIsSpatiallyLoaded))
	{
		Layer->SetIsSpatiallyLoaded(bIsSpatiallyLoaded);
	}
	double CellSize = 0.0;
	if (Root->TryGetNumberField(TEXT("cell_size"), CellSize))
	{
		TSharedPtr<FJsonObject> ImportResult;
		UeAgentLevelTopology::ImportPropertyText(Layer, TEXT("CellSize"), FString::FromInt(static_cast<int32>(CellSize)), ImportResult);
		PropertyResults.Add(MakeShared<FJsonValueObject>(ImportResult));
	}
	double LoadingRange = 0.0;
	if (Root->TryGetNumberField(TEXT("loading_range"), LoadingRange))
	{
		TSharedPtr<FJsonObject> ImportResult;
		UeAgentLevelTopology::ImportPropertyText(Layer, TEXT("LoadingRange"), FString::SanitizeFloat(LoadingRange), ImportResult);
		PropertyResults.Add(MakeShared<FJsonValueObject>(ImportResult));
	}
	FString ParentLayerPath;
	if (Root->TryGetStringField(TEXT("parent_layer"), ParentLayerPath) && !ParentLayerPath.IsEmpty())
	{
		Layer->SetParentLayer(UeAgentLevelTopology::LoadTypedAsset<UHLODLayer>(ParentLayerPath));
	}
	Layer->MarkPackageDirty();
	TArray<FString> SavedPackages;
	if (bSaveAfterApply)
	{
		TArray<UPackage*> Packages;
		Packages.Add(Layer->GetOutermost());
		if (!UeAgentLevelTopology::SavePackages(Packages, SavedPackages, OutError))
		{
			return false;
		}
	}
	OutData = UeAgentLevelTopology::HLODLayerToJson(Layer);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.hlod_layer.apply_result.v1"));
	OutData->SetStringField(TEXT("status"), TEXT("applied"));
	OutData->SetStringField(TEXT("source"), Source);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	OutData->SetBoolField(TEXT("transaction"), bTransaction);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	return true;
}

bool FUeAgentHttpServer::CmdHLODLayerCreateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdHLODLayerApplyJson(Ctx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdHLODLayerExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UHLODLayer* Layer = UeAgentLevelTopology::LoadTypedAsset<UHLODLayer>(AssetPath);
	if (!Layer)
	{
		OutError = TEXT("hlod_layer_not_found");
		return false;
	}
	OutData = UeAgentLevelTopology::HLODLayerToJson(Layer);
	return UeAgentLevelTopology::SaveJsonIfRequested(OutData, Ctx.Params, OutData, OutError);
}

bool FUeAgentHttpServer::CmdHLODLayerValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString Source;
	if (!UeAgentLevelTopology::LoadJsonFromParams(Ctx.Params, Root, Source, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	FString AssetPath;
	if (!Root->TryGetStringField(TEXT("asset_path"), AssetPath) && !JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath))
	{
		UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("missing_asset_path"), TEXT("$.asset_path"), TEXT("HLODLayer asset path is required."));
	}
	else
	{
		AssetPath = UeAgentLevelTopology::NormalizePackagePath(AssetPath);
		if (!UeAgentLevelTopology::IsValidLongPackagePath(AssetPath))
		{
			UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("hlod_layer_path_invalid"), TEXT("$.asset_path"), TEXT("HLODLayer asset path is invalid."));
		}
		else if (UObject* ExistingAsset = UeAgentLevelTopology::LoadObjectByPath(AssetPath))
		{
			if (!ExistingAsset->IsA<UHLODLayer>())
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("hlod_layer_type_mismatch"), TEXT("$.asset_path"), TEXT("Existing asset is not an HLODLayer."));
			}
		}
	}
	FString LayerTypeText;
	if (Root->TryGetStringField(TEXT("layer_type"), LayerTypeText))
	{
		EHLODLayerType ParsedType = EHLODLayerType::Instancing;
		if (!UeAgentLevelTopology::ParseHLODLayerType(LayerTypeText, ParsedType))
		{
			UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("hlod_layer_type_invalid"), TEXT("$.layer_type"), TEXT("Unknown HLOD layer type."));
		}
	}
	FString ParentLayerPath;
	if (Root->TryGetStringField(TEXT("parent_layer"), ParentLayerPath) && !ParentLayerPath.IsEmpty() && !UeAgentLevelTopology::LoadTypedAsset<UHLODLayer>(ParentLayerPath))
	{
		UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("hlod_parent_layer_not_found"), TEXT("$.parent_layer"), TEXT("Parent HLOD layer was not found."));
	}
	const int32 ErrorCount = UeAgentLevelTopology::CountIssues(Issues, TEXT("error"));
	OutData->SetStringField(TEXT("schema"), TEXT("uai.hlod_layer.validate_result.v1"));
	OutData->SetStringField(TEXT("source"), Source);
	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("status"), ErrorCount == 0 ? TEXT("valid") : TEXT("invalid"));
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), ErrorCount);
	return ErrorCount == 0;
}

bool FUeAgentHttpServer::CmdHLODAssignActors(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<AActor*> Actors;
	TArray<TSharedPtr<FJsonValue>> ActorResults;
	if (!UeAgentLevelTopology::ResolveActors(World, Ctx.Params, Actors, ActorResults, OutError))
	{
		return false;
	}
	FString HLODLayerPath;
	JsonTryGetString(Ctx.Params, TEXT("hlod_layer"), HLODLayerPath);
	UHLODLayer* Layer = HLODLayerPath.IsEmpty() ? nullptr : UeAgentLevelTopology::LoadTypedAsset<UHLODLayer>(HLODLayerPath);
	if (!HLODLayerPath.IsEmpty() && !Layer)
	{
		OutError = TEXT("hlod_layer_not_found");
		return false;
	}
	bool bTransaction = true;
	JsonTryGetBool(Ctx.Params, TEXT("transaction"), bTransaction);
	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UeAgentInterface", "HLODAssignActors", "UeAgentInterface HLOD Assign Actors"));
	}
	FString RuntimeGrid;
	JsonTryGetString(Ctx.Params, TEXT("runtime_grid"), RuntimeGrid);
	bool bIsSpatiallyLoaded = true;
	const bool bSetSpatial = JsonTryGetBool(Ctx.Params, TEXT("is_spatially_loaded"), bIsSpatiallyLoaded);
	TArray<TSharedPtr<FJsonValue>> Results;
	for (AActor* Actor : Actors)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Actor ? Actor->GetName() : FString());
		Result->SetStringField(TEXT("label"), Actor ? Actor->GetActorLabel() : FString());
		if (Actor)
		{
			Actor->Modify();
			Actor->SetHLODLayer(Layer);
			if (!RuntimeGrid.IsEmpty())
			{
				Actor->SetRuntimeGrid(FName(*RuntimeGrid));
			}
			if (bSetSpatial)
			{
				Actor->SetIsSpatiallyLoaded(bIsSpatiallyLoaded);
			}
			Actor->MarkPackageDirty();
			Result->SetStringField(TEXT("hlod_layer"), Actor->GetHLODLayer() ? UeAgentLevelTopology::ObjectPackagePath(Actor->GetHLODLayer()) : FString());
			Result->SetStringField(TEXT("runtime_grid"), Actor->GetRuntimeGrid().ToString());
			Result->SetBoolField(TEXT("is_spatially_loaded"), Actor->GetIsSpatiallyLoaded());
			Result->SetBoolField(TEXT("changed"), true);
		}
		Results.Add(MakeShared<FJsonValueObject>(Result));
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.hlod.assign_actors_result.v1"));
	OutData->SetNumberField(TEXT("matched_count"), Actors.Num());
	OutData->SetArrayField(TEXT("actor_results"), ActorResults);
	OutData->SetArrayField(TEXT("results"), Results);
	OutData->SetBoolField(TEXT("transaction"), bTransaction);
	return true;
}

bool FUeAgentHttpServer::CmdHLODQueryActors(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<AActor*> Actors;
	TArray<TSharedPtr<FJsonValue>> ActorResults;
	if (!UeAgentLevelTopology::ResolveActors(World, Ctx.Params, Actors, ActorResults, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> ActorValues;
	for (AActor* Actor : Actors)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Actor ? Actor->GetName() : FString());
		Obj->SetStringField(TEXT("label"), Actor ? Actor->GetActorLabel() : FString());
		Obj->SetStringField(TEXT("hlod_layer"), Actor && Actor->GetHLODLayer() ? UeAgentLevelTopology::ObjectPackagePath(Actor->GetHLODLayer()) : FString());
		Obj->SetStringField(TEXT("runtime_grid"), Actor ? Actor->GetRuntimeGrid().ToString() : FString());
		Obj->SetBoolField(TEXT("is_spatially_loaded"), Actor && Actor->GetIsSpatiallyLoaded());
		ActorValues.Add(MakeShared<FJsonValueObject>(Obj));
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.hlod.query_actors_result.v1"));
	OutData->SetArrayField(TEXT("actors"), ActorValues);
	OutData->SetArrayField(TEXT("actor_results"), ActorResults);
	OutData->SetNumberField(TEXT("count"), ActorValues.Num());
	return true;
}

bool FUeAgentHttpServer::CmdHLODBuild(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString Step = TEXT("stats");
	JsonTryGetString(Ctx.Params, TEXT("step"), Step);
	if (!Step.Equals(TEXT("setup"), ESearchCase::IgnoreCase)
		&& !Step.Equals(TEXT("build"), ESearchCase::IgnoreCase)
		&& !Step.Equals(TEXT("finalize"), ESearchCase::IgnoreCase)
		&& !Step.Equals(TEXT("delete"), ESearchCase::IgnoreCase)
		&& !Step.Equals(TEXT("stats"), ESearchCase::IgnoreCase))
	{
		OutData->SetStringField(TEXT("schema"), TEXT("uai.hlod.build_result.v1"));
		OutData->SetStringField(TEXT("step"), Step);
		OutData->SetStringField(TEXT("error_code"), TEXT("hlod_build_step_invalid"));
		OutError = TEXT("hlod_build_step_invalid");
		return false;
	}
	bool bExecute = false;
	bool bReportOnly = true;
	bool bAllowCommandlet = false;
	bool bAllowRHI = false;
	double TimeoutSeconds = 1800.0;
	JsonTryGetBool(Ctx.Params, TEXT("execute"), bExecute);
	JsonTryGetBool(Ctx.Params, TEXT("report_only"), bReportOnly);
	JsonTryGetBool(Ctx.Params, TEXT("allow_commandlet"), bAllowCommandlet);
	JsonTryGetBool(Ctx.Params, TEXT("allow_rhi"), bAllowRHI);
	JsonTryGetNumber(Ctx.Params, TEXT("timeout_seconds"), TimeoutSeconds);
	FString BuilderFlag = TEXT("-DumpHLODStats");
	if (Step.Equals(TEXT("setup"), ESearchCase::IgnoreCase)) BuilderFlag = TEXT("-SetupHLODs");
	if (Step.Equals(TEXT("build"), ESearchCase::IgnoreCase)) BuilderFlag = TEXT("-BuildHLODs");
	if (Step.Equals(TEXT("delete"), ESearchCase::IgnoreCase)) BuilderFlag = TEXT("-DeleteHLODs");
	if (Step.Equals(TEXT("finalize"), ESearchCase::IgnoreCase)) BuilderFlag = TEXT("-FinalizeHLODs");
	const FString Args = FString::Printf(TEXT("-run=WorldPartitionBuilderCommandlet -Builder=WorldPartitionHLODsBuilder %s"), *BuilderFlag);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.hlod.build_result.v1"));
	OutData->SetStringField(TEXT("step"), Step);
	OutData->SetBoolField(TEXT("execute"), bExecute);
	OutData->SetBoolField(TEXT("report_only"), bReportOnly);
	OutData->SetBoolField(TEXT("allow_commandlet"), bAllowCommandlet);
	OutData->SetBoolField(TEXT("allow_rhi"), bAllowRHI);
	OutData->SetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
	OutData->SetStringField(TEXT("commandlet_plan"), Args);
	if (!bExecute || bReportOnly)
	{
		OutData->SetStringField(TEXT("status"), TEXT("planned"));
		return true;
	}
	if (!bAllowCommandlet)
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("requires_commandlet"));
		OutError = TEXT("requires_commandlet");
		return false;
	}
	if (!bAllowRHI && UeAgentLevelTopology::IsNullRHI() && (Step.Equals(TEXT("build"), ESearchCase::IgnoreCase) || Step.Equals(TEXT("setup"), ESearchCase::IgnoreCase)))
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("requires_rhi"));
		OutError = TEXT("requires_rhi");
		return false;
	}
	const bool bOk = UeAgentLevelTopology::RunCommandletProcess(Args, TimeoutSeconds, OutData, OutError);
	OutData->SetStringField(TEXT("status"), bOk ? TEXT("executed") : TEXT("failed"));
	return bOk;
}

bool FUeAgentHttpServer::CmdHLODValidateBuild(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	int32 HLODActorCount = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetClass()->GetName().Contains(TEXT("WorldPartitionHLOD")))
		{
			++HLODActorCount;
		}
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.hlod.validate_build_result.v1"));
	OutData->SetNumberField(TEXT("hlod_actor_count"), HLODActorCount);
	OutData->SetStringField(TEXT("status"), TEXT("checked"));
	OutData->SetArrayField(TEXT("issues"), TArray<TSharedPtr<FJsonValue>>());
	return true;
}

bool FUeAgentHttpServer::CmdLevelTopologySetActorAssignments(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Results;
	TSharedPtr<FJsonObject> Assignments;
	if (!JsonTryGetObject(Ctx.Params, TEXT("assignments"), Assignments))
	{
		Assignments = Ctx.Params;
	}
	if (Assignments->HasField(TEXT("streaming_level")))
	{
		FString TargetLevel;
		Assignments->TryGetStringField(TEXT("streaming_level"), TargetLevel);
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_level"), TargetLevel);
		if (Ctx.Params->HasField(TEXT("actor_ids")))
		{
			Params->SetArrayField(TEXT("actor_ids"), Ctx.Params->GetArrayField(TEXT("actor_ids")));
		}
		FUeAgentRequestContext SubCtx = Ctx;
		SubCtx.Params = Params;
		TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
		FString SubError;
		const bool bOk = CmdLevelStreamingMoveActors(SubCtx, SubData, SubError);
		SubData->SetStringField(TEXT("operation"), TEXT("streaming_level"));
		SubData->SetBoolField(TEXT("ok"), bOk);
		SubData->SetStringField(TEXT("error"), SubError);
		Results.Add(MakeShared<FJsonValueObject>(SubData));
		if (!bOk)
		{
			OutError = SubError;
			OutData->SetArrayField(TEXT("results"), Results);
			return false;
		}
	}
	TSharedPtr<FJsonObject> DataLayersObj;
	if (JsonTryGetObject(Assignments, TEXT("data_layers"), DataLayersObj))
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FString Mode = TEXT("replace");
		DataLayersObj->TryGetStringField(TEXT("mode"), Mode);
		Params->SetStringField(TEXT("mode"), Mode);
		if (DataLayersObj->HasField(TEXT("layers")))
		{
			Params->SetArrayField(TEXT("layers"), DataLayersObj->GetArrayField(TEXT("layers")));
		}
		if (Ctx.Params->HasField(TEXT("actor_ids")))
		{
			Params->SetArrayField(TEXT("actor_ids"), Ctx.Params->GetArrayField(TEXT("actor_ids")));
		}
		FUeAgentRequestContext SubCtx = Ctx;
		SubCtx.Params = Params;
		TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
		FString SubError;
		const bool bOk = CmdDataLayerSetActorMembership(SubCtx, SubData, SubError);
		SubData->SetStringField(TEXT("operation"), TEXT("data_layers"));
		SubData->SetBoolField(TEXT("ok"), bOk);
		SubData->SetStringField(TEXT("error"), SubError);
		Results.Add(MakeShared<FJsonValueObject>(SubData));
		if (!bOk)
		{
			OutError = SubError;
			OutData->SetArrayField(TEXT("results"), Results);
			return false;
		}
	}
	if (Assignments->HasField(TEXT("hlod_layer")) || Assignments->HasField(TEXT("world_partition")))
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		if (Ctx.Params->HasField(TEXT("actor_ids")))
		{
			Params->SetArrayField(TEXT("actor_ids"), Ctx.Params->GetArrayField(TEXT("actor_ids")));
		}
		FString HLODLayer;
		if (Assignments->TryGetStringField(TEXT("hlod_layer"), HLODLayer))
		{
			Params->SetStringField(TEXT("hlod_layer"), HLODLayer);
		}
		TSharedPtr<FJsonObject> WpObj;
		if (JsonTryGetObject(Assignments, TEXT("world_partition"), WpObj))
		{
			FString RuntimeGrid;
			if (WpObj->TryGetStringField(TEXT("runtime_grid"), RuntimeGrid))
			{
				Params->SetStringField(TEXT("runtime_grid"), RuntimeGrid);
			}
			bool bSpatial = true;
			if (WpObj->TryGetBoolField(TEXT("is_spatially_loaded"), bSpatial))
			{
				Params->SetBoolField(TEXT("is_spatially_loaded"), bSpatial);
			}
		}
		FUeAgentRequestContext SubCtx = Ctx;
		SubCtx.Params = Params;
		TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
		FString SubError;
		const bool bOk = CmdHLODAssignActors(SubCtx, SubData, SubError);
		SubData->SetStringField(TEXT("operation"), TEXT("hlod_world_partition_actor_fields"));
		SubData->SetBoolField(TEXT("ok"), bOk);
		SubData->SetStringField(TEXT("error"), SubError);
		Results.Add(MakeShared<FJsonValueObject>(SubData));
		if (!bOk)
		{
			OutError = SubError;
			OutData->SetArrayField(TEXT("results"), Results);
			return false;
		}
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.level_topology.assignment_result.v1"));
	OutData->SetArrayField(TEXT("results"), Results);
	OutData->SetBoolField(TEXT("changed"), Results.Num() > 0);
	return true;
}

bool FUeAgentHttpServer::CmdLevelTopologyExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	bool bIncludeStreaming = true;
	bool bIncludeDataLayers = true;
	bool bIncludeWorldPartition = true;
	bool bIncludeHLOD = true;
	bool bIncludeAssignments = false;
	double AssignmentLimitNumber = 100.0;
	JsonTryGetBool(Ctx.Params, TEXT("include_streaming_levels"), bIncludeStreaming);
	JsonTryGetBool(Ctx.Params, TEXT("include_data_layers"), bIncludeDataLayers);
	JsonTryGetBool(Ctx.Params, TEXT("include_world_partition"), bIncludeWorldPartition);
	JsonTryGetBool(Ctx.Params, TEXT("include_hlod"), bIncludeHLOD);
	JsonTryGetBool(Ctx.Params, TEXT("include_actor_assignments"), bIncludeAssignments);
	JsonTryGetNumber(Ctx.Params, TEXT("actor_assignment_limit"), AssignmentLimitNumber);
	OutData->SetStringField(TEXT("schema"), UeAgentLevelTopology::TopologySchema);
	TSharedPtr<FJsonObject> WorldObj = MakeShared<FJsonObject>();
	WorldObj->SetStringField(TEXT("path"), World->GetPathName());
	WorldObj->SetStringField(TEXT("package"), World->GetOutermost() ? World->GetOutermost()->GetName() : FString());
	OutData->SetObjectField(TEXT("world"), WorldObj);
	if (bIncludeStreaming)
	{
		TSharedPtr<FJsonObject> StreamData = MakeShared<FJsonObject>();
		FUeAgentRequestContext SubCtx = Ctx;
		SubCtx.Params = MakeShared<FJsonObject>();
		FString SubError;
		CmdLevelStreamingQuery(SubCtx, StreamData, SubError);
		OutData->SetArrayField(TEXT("streaming_levels"), StreamData->GetArrayField(TEXT("streaming_levels")));
	}
	if (bIncludeDataLayers)
	{
		TSharedPtr<FJsonObject> DataLayerData = MakeShared<FJsonObject>();
		FUeAgentRequestContext SubCtx = Ctx;
		SubCtx.Params = MakeShared<FJsonObject>();
		SubCtx.Params->SetBoolField(TEXT("include_actors"), false);
		FString SubError;
		CmdDataLayerQuery(SubCtx, DataLayerData, SubError);
		OutData->SetArrayField(TEXT("data_layers"), DataLayerData->GetArrayField(TEXT("data_layers")));
	}
	if (bIncludeWorldPartition)
	{
		TSharedPtr<FJsonObject> WpData = MakeShared<FJsonObject>();
		FUeAgentRequestContext SubCtx = Ctx;
		SubCtx.Params = MakeShared<FJsonObject>();
		FString SubError;
		CmdWorldPartitionQuery(SubCtx, WpData, SubError);
		OutData->SetObjectField(TEXT("world_partition"), WpData);
	}
	if (bIncludeHLOD)
	{
		TSharedPtr<FJsonObject> HLODObj = MakeShared<FJsonObject>();
		HLODObj->SetBoolField(TEXT("available"), true);
		OutData->SetObjectField(TEXT("hlod"), HLODObj);
	}
	if (bIncludeAssignments)
	{
		TArray<TSharedPtr<FJsonValue>> AssignmentValues;
		const int32 AssignmentLimit = FMath::Max(1, static_cast<int32>(AssignmentLimitNumber));
		for (TActorIterator<AActor> It(World); It && AssignmentValues.Num() < AssignmentLimit; ++It)
		{
			AActor* Actor = *It;
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Actor->GetName());
			Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
			Obj->SetStringField(TEXT("level"), Actor->GetLevel() && Actor->GetLevel()->GetOutermost() ? Actor->GetLevel()->GetOutermost()->GetName() : FString());
			Obj->SetStringField(TEXT("runtime_grid"), Actor->GetRuntimeGrid().ToString());
			Obj->SetBoolField(TEXT("is_spatially_loaded"), Actor->GetIsSpatiallyLoaded());
			Obj->SetStringField(TEXT("hlod_layer"), Actor->GetHLODLayer() ? UeAgentLevelTopology::ObjectPackagePath(Actor->GetHLODLayer()) : FString());
			TArray<TSharedPtr<FJsonValue>> DlValues;
			for (const FName& DlName : Actor->GetDataLayerInstanceNames())
			{
				DlValues.Add(MakeShared<FJsonValueString>(DlName.ToString()));
			}
			Obj->SetArrayField(TEXT("data_layers"), DlValues);
			AssignmentValues.Add(MakeShared<FJsonValueObject>(Obj));
		}
		OutData->SetArrayField(TEXT("actor_assignments"), AssignmentValues);
	}
	TSharedPtr<FJsonObject> FeatureGates = MakeShared<FJsonObject>();
	FeatureGates->SetBoolField(TEXT("level_streaming"), true);
	FeatureGates->SetBoolField(TEXT("data_layer_editor"), UeAgentLevelTopology::GetDataLayerSubsystem() != nullptr);
	FeatureGates->SetBoolField(TEXT("world_partition"), World->GetWorldPartition() != nullptr);
	FeatureGates->SetBoolField(TEXT("hlod"), true);
	OutData->SetObjectField(TEXT("feature_gates"), FeatureGates);
	return UeAgentLevelTopology::SaveJsonIfRequested(OutData, Ctx.Params, OutData, OutError);
}

bool FUeAgentHttpServer::CmdLevelTopologyValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString Source;
	if (!UeAgentLevelTopology::LoadJsonFromParams(Ctx.Params, Root, Source, OutError))
	{
		return false;
	}
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	const TArray<TSharedPtr<FJsonValue>>* StreamingLevels = nullptr;
	if (Root->TryGetArrayField(TEXT("streaming_levels"), StreamingLevels) && StreamingLevels)
	{
		for (int32 Index = 0; Index < StreamingLevels->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Item = (*StreamingLevels)[Index].IsValid() ? (*StreamingLevels)[Index]->AsObject() : nullptr;
			FString LevelPackage;
			if (!Item.IsValid() || !Item->TryGetStringField(TEXT("level_package"), LevelPackage) || !UeAgentLevelTopology::IsValidLongPackagePath(LevelPackage))
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("streaming_level_package_invalid"), FString::Printf(TEXT("$.streaming_levels[%d].level_package"), Index), TEXT("Streaming level package path is invalid."));
				continue;
			}
			FString Operation = TEXT("ensure");
			Item->TryGetStringField(TEXT("operation"), Operation);
			if (Operation.Equals(TEXT("add_existing"), ESearchCase::IgnoreCase) && !UeAgentLevelTopology::AssetExists(LevelPackage))
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("streaming_level_asset_not_found"), FString::Printf(TEXT("$.streaming_levels[%d].level_package"), Index), TEXT("add_existing requires an existing level package."));
			}
			if ((Operation.Equals(TEXT("remove"), ESearchCase::IgnoreCase) || Operation.Equals(TEXT("set_state"), ESearchCase::IgnoreCase)) && !UeAgentLevelTopology::FindStreamingLevel(World, LevelPackage))
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("streaming_level_not_found"), FString::Printf(TEXT("$.streaming_levels[%d].level_package"), Index), TEXT("Requested streaming level is not present in the current world."));
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* DataLayers = nullptr;
	if (Root->TryGetArrayField(TEXT("data_layers"), DataLayers) && DataLayers)
	{
		for (int32 Index = 0; Index < DataLayers->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Item = (*DataLayers)[Index].IsValid() ? (*DataLayers)[Index]->AsObject() : nullptr;
			FString AssetPath;
			if (!Item.IsValid() || !Item->TryGetStringField(TEXT("asset_path"), AssetPath))
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("warning"), TEXT("data_layer_asset_not_specified"), FString::Printf(TEXT("$.data_layers[%d].asset_path"), Index), TEXT("DataLayer asset path is not specified."));
				continue;
			}
			if (!UeAgentLevelTopology::IsValidLongPackagePath(AssetPath))
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("data_layer_asset_path_invalid"), FString::Printf(TEXT("$.data_layers[%d].asset_path"), Index), TEXT("DataLayer asset path is not a valid long package name."));
			}
			bool bCreateInstance = false;
			Item->TryGetBoolField(TEXT("create_instance"), bCreateInstance);
			if (bCreateInstance && !UeAgentLevelTopology::SupportsDataLayerInstanceAuthoring(World))
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("level_external_objects_required_for_data_layer"), FString::Printf(TEXT("$.data_layers[%d]"), Index), TEXT("DataLayer instance authoring requires the persistent level to use external objects."));
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* ActorAssignments = nullptr;
	if (Root->TryGetArrayField(TEXT("actor_assignments"), ActorAssignments) && ActorAssignments)
	{
		for (int32 Index = 0; Index < ActorAssignments->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Item = (*ActorAssignments)[Index].IsValid() ? (*ActorAssignments)[Index]->AsObject() : nullptr;
			TArray<AActor*> Actors;
			TArray<TSharedPtr<FJsonValue>> ActorResults;
			FString ActorError;
			if (!Item.IsValid() || !UeAgentLevelTopology::ResolveActors(World, Item, Actors, ActorResults, ActorError) || Actors.IsEmpty())
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("actor_assignment_selection_invalid"), FString::Printf(TEXT("$.actor_assignments[%d]"), Index), ActorError.IsEmpty() ? FString(TEXT("Actor assignment has no valid actor selection.")) : ActorError);
			}
		}
	}
	const TSharedPtr<FJsonObject>* WorldPartitionObj = nullptr;
	if (Root->TryGetObjectField(TEXT("world_partition"), WorldPartitionObj) && WorldPartitionObj && WorldPartitionObj->IsValid())
	{
		const bool bRequestsWpWrite = (*WorldPartitionObj)->HasField(TEXT("streaming_enabled"))
			|| (*WorldPartitionObj)->HasField(TEXT("load_regions"))
			|| (*WorldPartitionObj)->HasField(TEXT("load_actors"))
			|| (*WorldPartitionObj)->HasField(TEXT("generate_streaming"));
		if (bRequestsWpWrite && !World->GetWorldPartition())
		{
			UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("world_partition_not_enabled"), TEXT("$.world_partition"), TEXT("The current world is not a World Partition world."));
		}
	}
	const TSharedPtr<FJsonObject>* HLODObj = nullptr;
	if (Root->TryGetObjectField(TEXT("hlod"), HLODObj) && HLODObj && HLODObj->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Layers = nullptr;
		if ((*HLODObj)->TryGetArrayField(TEXT("layers"), Layers) && Layers)
		{
			for (int32 Index = 0; Index < Layers->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Item = (*Layers)[Index].IsValid() ? (*Layers)[Index]->AsObject() : nullptr;
				FString AssetPath;
				if (!Item.IsValid() || !Item->TryGetStringField(TEXT("asset_path"), AssetPath) || !UeAgentLevelTopology::IsValidLongPackagePath(AssetPath))
				{
					UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("hlod_layer_path_invalid"), FString::Printf(TEXT("$.hlod.layers[%d].asset_path"), Index), TEXT("HLODLayer asset path is invalid."));
				}
				FString LayerTypeText;
				if (Item.IsValid() && Item->TryGetStringField(TEXT("layer_type"), LayerTypeText))
				{
					EHLODLayerType ParsedType = EHLODLayerType::Instancing;
					if (!UeAgentLevelTopology::ParseHLODLayerType(LayerTypeText, ParsedType))
					{
						UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("hlod_layer_type_invalid"), FString::Printf(TEXT("$.hlod.layers[%d].layer_type"), Index), TEXT("Unknown HLOD layer type."));
					}
				}
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* Tasks = nullptr;
	if (Root->TryGetArrayField(TEXT("tasks"), Tasks) && Tasks)
	{
		for (int32 Index = 0; Index < Tasks->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Task = (*Tasks)[Index].IsValid() ? (*Tasks)[Index]->AsObject() : nullptr;
			FString Type;
			if (!Task.IsValid() || (!Task->TryGetStringField(TEXT("type"), Type) && !Task->TryGetStringField(TEXT("command"), Type)))
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("task_type_missing"), FString::Printf(TEXT("$.tasks[%d]"), Index), TEXT("Topology task requires type or command."));
			}
			else if (!Type.Equals(TEXT("hlod_build"), ESearchCase::IgnoreCase)
				&& !Type.Equals(TEXT("world_partition_generate_streaming"), ESearchCase::IgnoreCase)
				&& !Type.Equals(TEXT("world_partition_resave_actors"), ESearchCase::IgnoreCase))
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("task_type_unsupported"), FString::Printf(TEXT("$.tasks[%d].type"), Index), TEXT("Unsupported topology task type."));
			}
		}
	}
	const int32 ErrorCount = UeAgentLevelTopology::CountIssues(Issues, TEXT("error"));
	OutData->SetStringField(TEXT("schema"), TEXT("uai.level_topology.validate_result.v1"));
	OutData->SetStringField(TEXT("source"), Source);
	OutData->SetStringField(TEXT("status"), ErrorCount == 0 ? TEXT("valid") : TEXT("invalid"));
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), ErrorCount);
	return ErrorCount == 0;
}

bool FUeAgentHttpServer::CmdLevelTopologyDiffJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString Source;
	if (!UeAgentLevelTopology::LoadJsonFromParams(Ctx.Params, Root, Source, OutError))
	{
		return false;
	}
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Diffs;
	auto AddDiff = [&Diffs](const FString& Type, const FString& Path, const FString& Message)
	{
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetStringField(TEXT("type"), Type);
		Diff->SetStringField(TEXT("path"), Path);
		Diff->SetStringField(TEXT("message"), Message);
		Diffs.Add(MakeShared<FJsonValueObject>(Diff));
		return Diff;
	};

	const TArray<TSharedPtr<FJsonValue>>* StreamingLevels = nullptr;
	if (Root->TryGetArrayField(TEXT("streaming_levels"), StreamingLevels) && StreamingLevels)
	{
		TSet<FString> DesiredStreamingPackages;
		for (const TSharedPtr<FJsonValue>& Value : *StreamingLevels)
		{
			const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
			FString LevelPackage;
			if (!Item.IsValid() || !Item->TryGetStringField(TEXT("level_package"), LevelPackage))
			{
				continue;
			}
			LevelPackage = UeAgentLevelTopology::NormalizePackagePath(LevelPackage);
			DesiredStreamingPackages.Add(LevelPackage);
			ULevelStreaming* StreamingLevel = UeAgentLevelTopology::FindStreamingLevel(World, LevelPackage);
			if (!StreamingLevel)
			{
				TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("streaming_level_added"), TEXT("$.streaming_levels"), TEXT("Target streaming level is missing from the current world."));
				Diff->SetStringField(TEXT("level_package"), LevelPackage);
				continue;
			}
			bool ExpectedLoaded = false;
			if (Item->TryGetBoolField(TEXT("loaded"), ExpectedLoaded) && StreamingLevel->IsLevelLoaded() != ExpectedLoaded)
			{
				TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("streaming_state_changed"), LevelPackage, TEXT("Streaming loaded state differs."));
				Diff->SetStringField(TEXT("field"), TEXT("loaded"));
				Diff->SetBoolField(TEXT("current"), StreamingLevel->IsLevelLoaded());
				Diff->SetBoolField(TEXT("target"), ExpectedLoaded);
			}
			bool ExpectedVisible = false;
			if (Item->TryGetBoolField(TEXT("visible"), ExpectedVisible) && StreamingLevel->IsLevelVisible() != ExpectedVisible)
			{
				TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("streaming_state_changed"), LevelPackage, TEXT("Streaming visible state differs."));
				Diff->SetStringField(TEXT("field"), TEXT("visible"));
				Diff->SetBoolField(TEXT("current"), StreamingLevel->IsLevelVisible());
				Diff->SetBoolField(TEXT("target"), ExpectedVisible);
			}
		}
		for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
		{
			if (!StreamingLevel)
			{
				continue;
			}
			const FString CurrentPackage = UeAgentLevelTopology::NormalizePackagePath(StreamingLevel->GetWorldAssetPackageName());
			if (!CurrentPackage.IsEmpty() && !DesiredStreamingPackages.Contains(CurrentPackage))
			{
				TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("streaming_level_removed"), CurrentPackage, TEXT("Current streaming level is not present in the target topology."));
				Diff->SetStringField(TEXT("level_package"), CurrentPackage);
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* DataLayers = nullptr;
	if (Root->TryGetArrayField(TEXT("data_layers"), DataLayers) && DataLayers)
	{
		TArray<UDataLayerInstance*> DesiredInstances;
		for (int32 Index = 0; Index < DataLayers->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Item = (*DataLayers)[Index].IsValid() ? (*DataLayers)[Index]->AsObject() : nullptr;
			if (!Item.IsValid())
			{
				continue;
			}
			FString LayerKey;
			if (!Item->TryGetStringField(TEXT("layer"), LayerKey) && !Item->TryGetStringField(TEXT("instance_name"), LayerKey))
			{
				Item->TryGetStringField(TEXT("asset_path"), LayerKey);
			}
			if (LayerKey.IsEmpty())
			{
				continue;
			}
			UDataLayerInstance* Instance = UeAgentLevelTopology::FindDataLayerInstance(World, LayerKey);
			if (!Instance)
			{
				TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("data_layer_added"), FString::Printf(TEXT("$.data_layers[%d]"), Index), TEXT("Target DataLayer instance is missing from the current world."));
				Diff->SetStringField(TEXT("layer"), LayerKey);
				continue;
			}
			DesiredInstances.AddUnique(Instance);
			FString ExpectedRuntimeState;
			if (Item->TryGetStringField(TEXT("initial_runtime_state"), ExpectedRuntimeState))
			{
				EDataLayerRuntimeState ParsedState = EDataLayerRuntimeState::Unloaded;
				if (UeAgentLevelTopology::ParseDataLayerRuntimeState(ExpectedRuntimeState, ParsedState) && Instance->GetInitialRuntimeState() != ParsedState)
				{
					TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("data_layer_state_changed"), LayerKey, TEXT("DataLayer initial runtime state differs."));
					Diff->SetStringField(TEXT("field"), TEXT("initial_runtime_state"));
					Diff->SetStringField(TEXT("current"), GetDataLayerRuntimeStateName(Instance->GetInitialRuntimeState()));
					Diff->SetStringField(TEXT("target"), ExpectedRuntimeState);
				}
			}
			bool ExpectedVisible = false;
			if (Item->TryGetBoolField(TEXT("visible"), ExpectedVisible) && Instance->IsVisible() != ExpectedVisible)
			{
				TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("data_layer_state_changed"), LayerKey, TEXT("DataLayer visibility differs."));
				Diff->SetStringField(TEXT("field"), TEXT("visible"));
				Diff->SetBoolField(TEXT("current"), Instance->IsVisible());
				Diff->SetBoolField(TEXT("target"), ExpectedVisible);
			}
			bool ExpectedLoaded = false;
			if (Item->TryGetBoolField(TEXT("loaded_in_editor"), ExpectedLoaded) && Instance->IsLoadedInEditor() != ExpectedLoaded)
			{
				TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("data_layer_state_changed"), LayerKey, TEXT("DataLayer editor loaded state differs."));
				Diff->SetStringField(TEXT("field"), TEXT("loaded_in_editor"));
				Diff->SetBoolField(TEXT("current"), Instance->IsLoadedInEditor());
				Diff->SetBoolField(TEXT("target"), ExpectedLoaded);
			}
		}
		if (AWorldDataLayers* WorldDataLayers = UeAgentLevelTopology::FindWorldDataLayers(World))
		{
			WorldDataLayers->ForEachDataLayerInstance([&DesiredInstances, &AddDiff](UDataLayerInstance* Instance)
			{
				if (Instance && !DesiredInstances.Contains(Instance))
				{
					TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("data_layer_removed"), Instance->GetDataLayerFName().ToString(), TEXT("Current DataLayer instance is not present in the target topology."));
					Diff->SetStringField(TEXT("instance_name"), Instance->GetDataLayerFName().ToString());
				}
				return true;
			});
		}
	}

	const TSharedPtr<FJsonObject>* WorldPartitionObj = nullptr;
	if (Root->TryGetObjectField(TEXT("world_partition"), WorldPartitionObj) && WorldPartitionObj && WorldPartitionObj->IsValid())
	{
		if (UWorldPartition* WorldPartition = World->GetWorldPartition())
		{
			bool ExpectedStreaming = false;
			if ((*WorldPartitionObj)->TryGetBoolField(TEXT("streaming_enabled"), ExpectedStreaming) && WorldPartition->IsStreamingEnabledInEditor() != ExpectedStreaming)
			{
				TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("world_partition_streaming_changed"), TEXT("$.world_partition.streaming_enabled"), TEXT("World Partition streaming enabled state differs."));
				Diff->SetBoolField(TEXT("current"), WorldPartition->IsStreamingEnabledInEditor());
				Diff->SetBoolField(TEXT("target"), ExpectedStreaming);
			}
			const TArray<TSharedPtr<FJsonValue>>* LoadRegions = nullptr;
			if ((*WorldPartitionObj)->TryGetArrayField(TEXT("load_regions"), LoadRegions) && LoadRegions && LoadRegions->Num() > 0)
			{
				AddDiff(TEXT("world_partition_region_load_changed"), TEXT("$.world_partition.load_regions"), TEXT("Target topology requests World Partition editor region loaders."));
			}
		}
		else
		{
			AddDiff(TEXT("world_partition_streaming_changed"), TEXT("$.world_partition"), TEXT("Target topology references World Partition but the current world is not partitioned."));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* ActorAssignments = nullptr;
	if (Root->TryGetArrayField(TEXT("actor_assignments"), ActorAssignments) && ActorAssignments)
	{
		for (int32 Index = 0; Index < ActorAssignments->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Item = (*ActorAssignments)[Index].IsValid() ? (*ActorAssignments)[Index]->AsObject() : nullptr;
			if (!Item.IsValid())
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* AssignmentsPtr = nullptr;
			const TSharedPtr<FJsonObject> Assignments = (Item->TryGetObjectField(TEXT("assignments"), AssignmentsPtr) && AssignmentsPtr && AssignmentsPtr->IsValid()) ? *AssignmentsPtr : Item;
			TArray<AActor*> Actors;
			TArray<TSharedPtr<FJsonValue>> ActorResults;
			FString ActorError;
			if (!UeAgentLevelTopology::ResolveActors(World, Item, Actors, ActorResults, ActorError))
			{
				AddDiff(TEXT("actor_assignment_selection_invalid"), FString::Printf(TEXT("$.actor_assignments[%d]"), Index), ActorError);
				continue;
			}
			for (AActor* Actor : Actors)
			{
				if (!Actor)
				{
					continue;
				}
				const FString ActorLabel = Actor->GetActorLabel();
				FString TargetLevel;
				if (Assignments->TryGetStringField(TEXT("streaming_level"), TargetLevel))
				{
					const FString CurrentLevel = Actor->GetLevel() && Actor->GetLevel()->GetOutermost() ? UeAgentLevelTopology::NormalizePackagePath(Actor->GetLevel()->GetOutermost()->GetName()) : FString();
					if (!CurrentLevel.Equals(UeAgentLevelTopology::NormalizePackagePath(TargetLevel), ESearchCase::IgnoreCase))
					{
						TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("actor_level_changed"), ActorLabel, TEXT("Actor level assignment differs."));
						Diff->SetStringField(TEXT("current"), CurrentLevel);
						Diff->SetStringField(TEXT("target"), UeAgentLevelTopology::NormalizePackagePath(TargetLevel));
					}
				}
				TArray<FString> ExpectedDataLayers = UeAgentLevelTopology::ReadStringArray(Assignments, TEXT("data_layers"));
				if (!ExpectedDataLayers.IsEmpty())
				{
					bool bMembershipDiffers = false;
					for (const FString& ExpectedLayer : ExpectedDataLayers)
					{
						UDataLayerInstance* Layer = UeAgentLevelTopology::FindDataLayerInstance(World, ExpectedLayer);
						if (!Layer || !UeAgentLevelTopology::ActorContainsDataLayerInstance(Actor, Layer))
						{
							bMembershipDiffers = true;
							break;
						}
					}
					if (bMembershipDiffers || Actor->GetDataLayerInstanceNames().Num() != ExpectedDataLayers.Num())
					{
						TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("actor_data_layer_membership_changed"), ActorLabel, TEXT("Actor DataLayer membership differs."));
						Diff->SetNumberField(TEXT("current_count"), Actor->GetDataLayerInstanceNames().Num());
						Diff->SetNumberField(TEXT("target_count"), ExpectedDataLayers.Num());
					}
				}
				FString HLODLayerPath;
				if (Assignments->TryGetStringField(TEXT("hlod_layer"), HLODLayerPath))
				{
					const FString CurrentHLOD = Actor->GetHLODLayer() ? UeAgentLevelTopology::ObjectPackagePath(Actor->GetHLODLayer()) : FString();
					if (!CurrentHLOD.Equals(UeAgentLevelTopology::NormalizePackagePath(HLODLayerPath), ESearchCase::IgnoreCase))
					{
						TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("actor_hlod_layer_changed"), ActorLabel, TEXT("Actor HLODLayer assignment differs."));
						Diff->SetStringField(TEXT("current"), CurrentHLOD);
						Diff->SetStringField(TEXT("target"), UeAgentLevelTopology::NormalizePackagePath(HLODLayerPath));
					}
				}
				const TSharedPtr<FJsonObject>* WpActorObj = nullptr;
				if (Assignments->TryGetObjectField(TEXT("world_partition"), WpActorObj) && WpActorObj && WpActorObj->IsValid())
				{
					FString RuntimeGrid;
					if ((*WpActorObj)->TryGetStringField(TEXT("runtime_grid"), RuntimeGrid) && !Actor->GetRuntimeGrid().ToString().Equals(RuntimeGrid, ESearchCase::IgnoreCase))
					{
						TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("world_partition_streaming_changed"), ActorLabel, TEXT("Actor runtime grid differs."));
						Diff->SetStringField(TEXT("field"), TEXT("runtime_grid"));
						Diff->SetStringField(TEXT("current"), Actor->GetRuntimeGrid().ToString());
						Diff->SetStringField(TEXT("target"), RuntimeGrid);
					}
					bool bSpatial = true;
					if ((*WpActorObj)->TryGetBoolField(TEXT("is_spatially_loaded"), bSpatial) && Actor->GetIsSpatiallyLoaded() != bSpatial)
					{
						TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("world_partition_streaming_changed"), ActorLabel, TEXT("Actor spatial loading flag differs."));
						Diff->SetStringField(TEXT("field"), TEXT("is_spatially_loaded"));
						Diff->SetBoolField(TEXT("current"), Actor->GetIsSpatiallyLoaded());
						Diff->SetBoolField(TEXT("target"), bSpatial);
					}
				}
			}
		}
	}

	const TSharedPtr<FJsonObject>* HLODObj = nullptr;
	if (Root->TryGetObjectField(TEXT("hlod"), HLODObj) && HLODObj && HLODObj->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Layers = nullptr;
		if ((*HLODObj)->TryGetArrayField(TEXT("layers"), Layers) && Layers)
		{
			for (int32 Index = 0; Index < Layers->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Item = (*Layers)[Index].IsValid() ? (*Layers)[Index]->AsObject() : nullptr;
				FString AssetPath;
				if (Item.IsValid() && Item->TryGetStringField(TEXT("asset_path"), AssetPath) && !UeAgentLevelTopology::AssetExists(AssetPath))
				{
					TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("hlod_layer_asset_changed"), FString::Printf(TEXT("$.hlod.layers[%d]"), Index), TEXT("Target HLODLayer asset is missing."));
					Diff->SetStringField(TEXT("asset_path"), UeAgentLevelTopology::NormalizePackagePath(AssetPath));
				}
			}
		}
		bool bBuildRequired = false;
		if ((*HLODObj)->TryGetBoolField(TEXT("build_required"), bBuildRequired) && bBuildRequired)
		{
			AddDiff(TEXT("hlod_build_required"), TEXT("$.hlod.build_required"), TEXT("Target topology explicitly requests an HLOD build task."));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Tasks = nullptr;
	if (Root->TryGetArrayField(TEXT("tasks"), Tasks) && Tasks)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Tasks)
		{
			const TSharedPtr<FJsonObject> Task = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Type;
			if (Task.IsValid() && (Task->TryGetStringField(TEXT("type"), Type) || Task->TryGetStringField(TEXT("command"), Type)) && Type.Equals(TEXT("hlod_build"), ESearchCase::IgnoreCase))
			{
				AddDiff(TEXT("hlod_build_required"), TEXT("$.tasks"), TEXT("Target topology requests an HLOD build task."));
			}
		}
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.level_topology.diff_result.v1"));
	OutData->SetStringField(TEXT("source"), Source);
	OutData->SetArrayField(TEXT("diffs"), Diffs);
	OutData->SetNumberField(TEXT("diff_count"), Diffs.Num());
	OutData->SetBoolField(TEXT("changed"), Diffs.Num() > 0);
	return true;
}

bool FUeAgentHttpServer::CmdLevelTopologyApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString Source;
	if (!UeAgentLevelTopology::LoadJsonFromParams(Ctx.Params, Root, Source, OutError))
	{
		return false;
	}
	bool bDryRun = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	TArray<TSharedPtr<FJsonValue>> Results;
	if (bDryRun)
	{
		OutData->SetStringField(TEXT("schema"), TEXT("uai.level_topology.apply_result.v1"));
		OutData->SetStringField(TEXT("status"), TEXT("planned"));
		OutData->SetBoolField(TEXT("dry_run"), true);
		OutData->SetArrayField(TEXT("results"), Results);
		return true;
	}
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* StreamingLevels = nullptr;
	if (Root->TryGetArrayField(TEXT("streaming_levels"), StreamingLevels) && StreamingLevels)
	{
		for (const TSharedPtr<FJsonValue>& Value : *StreamingLevels)
		{
			const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Item.IsValid())
			{
				continue;
			}
			FUeAgentRequestContext SubCtx = Ctx;
			SubCtx.Params = Item;
			TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
			FString SubError;
			FString Operation = TEXT("ensure");
			Item->TryGetStringField(TEXT("operation"), Operation);
			bool bOk = false;
			if (Operation.Equals(TEXT("remove"), ESearchCase::IgnoreCase))
			{
				bOk = CmdLevelStreamingRemove(SubCtx, SubData, SubError);
			}
			else if (Operation.Equals(TEXT("set_state"), ESearchCase::IgnoreCase))
			{
				bOk = CmdLevelStreamingSetState(SubCtx, SubData, SubError);
			}
			else if (Operation.Equals(TEXT("add_existing"), ESearchCase::IgnoreCase))
			{
				bOk = CmdLevelStreamingAddExisting(SubCtx, SubData, SubError);
			}
			else
			{
				FString LevelPackage;
				Item->TryGetStringField(TEXT("level_package"), LevelPackage);
				if (ULevelStreaming* Existing = UeAgentLevelTopology::FindStreamingLevel(World, LevelPackage))
				{
					SubData = UeAgentLevelTopology::BuildStreamingLevelJson(Existing, World);
					SubData->SetStringField(TEXT("status"), TEXT("already_present"));
					bOk = true;
				}
				else
				{
					bOk = CmdLevelStreamingCreateLevel(SubCtx, SubData, SubError);
				}
			}
			if (bOk && !Operation.Equals(TEXT("remove"), ESearchCase::IgnoreCase) && !Operation.Equals(TEXT("set_state"), ESearchCase::IgnoreCase)
				&& (Item->HasField(TEXT("loaded")) || Item->HasField(TEXT("visible")) || Item->HasField(TEXT("current")) || Item->HasField(TEXT("transform"))))
			{
				TSharedPtr<FJsonObject> StateData = MakeShared<FJsonObject>();
				FString StateError;
				const bool bStateOk = CmdLevelStreamingSetState(SubCtx, StateData, StateError);
				SubData->SetObjectField(TEXT("state_result"), StateData);
				SubData->SetBoolField(TEXT("state_ok"), bStateOk);
				SubData->SetStringField(TEXT("state_error"), StateError);
				bOk = bStateOk;
				if (!bOk)
				{
					SubError = StateError;
				}
			}
			SubData->SetStringField(TEXT("operation"), TEXT("streaming_level"));
			SubData->SetBoolField(TEXT("ok"), bOk);
			SubData->SetStringField(TEXT("error"), SubError);
			Results.Add(MakeShared<FJsonValueObject>(SubData));
			if (!bOk)
			{
				OutError = SubError;
				OutData->SetArrayField(TEXT("results"), Results);
				return false;
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* DataLayers = nullptr;
	if (Root->TryGetArrayField(TEXT("data_layers"), DataLayers) && DataLayers)
	{
		for (const TSharedPtr<FJsonValue>& Value : *DataLayers)
		{
			const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Item.IsValid())
			{
				continue;
			}
			FUeAgentRequestContext SubCtx = Ctx;
			SubCtx.Params = Item;
			TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
			FString SubError;
			bool bOk = true;
			FString Operation = TEXT("ensure");
			Item->TryGetStringField(TEXT("operation"), Operation);
			if (Operation.Equals(TEXT("delete"), ESearchCase::IgnoreCase))
			{
				bOk = CmdDataLayerDeleteInstance(SubCtx, SubData, SubError);
			}
			else if (Item->HasField(TEXT("asset_path")))
			{
				bOk = CmdDataLayerCreateAsset(SubCtx, SubData, SubError);
			}
			bool bCreateInstance = false;
			Item->TryGetBoolField(TEXT("create_instance"), bCreateInstance);
			if (bOk && !Operation.Equals(TEXT("delete"), ESearchCase::IgnoreCase) && bCreateInstance)
			{
				bOk = CmdDataLayerCreateInstance(SubCtx, SubData, SubError);
			}
			if (bOk && !Operation.Equals(TEXT("delete"), ESearchCase::IgnoreCase)
				&& (Item->HasField(TEXT("visible")) || Item->HasField(TEXT("loaded_in_editor")) || Item->HasField(TEXT("initial_runtime_state")) || Item->HasField(TEXT("initially_visible")) || Item->HasField(TEXT("short_name"))))
			{
				TSharedPtr<FJsonObject> StateParams = MakeShared<FJsonObject>();
				FString LayerKey;
				if (!Item->TryGetStringField(TEXT("layer"), LayerKey) && !Item->TryGetStringField(TEXT("instance_name"), LayerKey))
				{
					Item->TryGetStringField(TEXT("asset_path"), LayerKey);
				}
				StateParams->SetStringField(TEXT("layer"), LayerKey);
				bool BoolValue = false;
				FString StringValue;
				if (Item->TryGetBoolField(TEXT("visible"), BoolValue)) StateParams->SetBoolField(TEXT("visible"), BoolValue);
				if (Item->TryGetBoolField(TEXT("loaded_in_editor"), BoolValue)) StateParams->SetBoolField(TEXT("loaded_in_editor"), BoolValue);
				if (Item->TryGetBoolField(TEXT("initially_visible"), BoolValue)) StateParams->SetBoolField(TEXT("initially_visible"), BoolValue);
				if (Item->TryGetStringField(TEXT("initial_runtime_state"), StringValue)) StateParams->SetStringField(TEXT("initial_runtime_state"), StringValue);
				if (Item->TryGetStringField(TEXT("short_name"), StringValue)) StateParams->SetStringField(TEXT("short_name"), StringValue);
				FUeAgentRequestContext StateCtx = Ctx;
				StateCtx.Params = StateParams;
				TSharedPtr<FJsonObject> StateData = MakeShared<FJsonObject>();
				FString StateError;
				const bool bStateOk = CmdDataLayerSetState(StateCtx, StateData, StateError);
				SubData->SetObjectField(TEXT("state_result"), StateData);
				SubData->SetBoolField(TEXT("state_ok"), bStateOk);
				SubData->SetStringField(TEXT("state_error"), StateError);
				bOk = bStateOk;
				if (!bOk)
				{
					SubError = StateError;
				}
			}
			if (bOk && !Operation.Equals(TEXT("delete"), ESearchCase::IgnoreCase) && Item->HasField(TEXT("actor_ids"))
				&& (Item->HasField(TEXT("layers")) || Item->HasField(TEXT("data_layers"))))
			{
				TSharedPtr<FJsonObject> MembershipParams = MakeShared<FJsonObject>();
				MembershipParams->SetArrayField(TEXT("actor_ids"), Item->GetArrayField(TEXT("actor_ids")));
				if (Item->HasField(TEXT("layers")))
				{
					MembershipParams->SetArrayField(TEXT("layers"), Item->GetArrayField(TEXT("layers")));
				}
				else
				{
					MembershipParams->SetArrayField(TEXT("layers"), Item->GetArrayField(TEXT("data_layers")));
				}
				FString Mode = TEXT("replace");
				Item->TryGetStringField(TEXT("membership_mode"), Mode);
				MembershipParams->SetStringField(TEXT("mode"), Mode);
				FUeAgentRequestContext MembershipCtx = Ctx;
				MembershipCtx.Params = MembershipParams;
				TSharedPtr<FJsonObject> MembershipData = MakeShared<FJsonObject>();
				FString MembershipError;
				const bool bMembershipOk = CmdDataLayerSetActorMembership(MembershipCtx, MembershipData, MembershipError);
				SubData->SetObjectField(TEXT("membership_result"), MembershipData);
				SubData->SetBoolField(TEXT("membership_ok"), bMembershipOk);
				SubData->SetStringField(TEXT("membership_error"), MembershipError);
				bOk = bMembershipOk;
				if (!bOk)
				{
					SubError = MembershipError;
				}
			}
			SubData->SetStringField(TEXT("operation"), TEXT("data_layer"));
			SubData->SetBoolField(TEXT("ok"), bOk);
			SubData->SetStringField(TEXT("error"), SubError);
			Results.Add(MakeShared<FJsonValueObject>(SubData));
			if (!bOk)
			{
				OutError = SubError;
				OutData->SetArrayField(TEXT("results"), Results);
				return false;
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* ActorAssignments = nullptr;
	if (Root->TryGetArrayField(TEXT("actor_assignments"), ActorAssignments) && ActorAssignments)
	{
		for (const TSharedPtr<FJsonValue>& Value : *ActorAssignments)
		{
			const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Item.IsValid())
			{
				continue;
			}
			FUeAgentRequestContext SubCtx = Ctx;
			SubCtx.Params = Item;
			TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
			FString SubError;
			const bool bOk = CmdLevelTopologySetActorAssignments(SubCtx, SubData, SubError);
			SubData->SetStringField(TEXT("operation"), TEXT("actor_assignment"));
			SubData->SetBoolField(TEXT("ok"), bOk);
			SubData->SetStringField(TEXT("error"), SubError);
			Results.Add(MakeShared<FJsonValueObject>(SubData));
			if (!bOk)
			{
				OutError = SubError;
				OutData->SetArrayField(TEXT("results"), Results);
				return false;
			}
		}
	}
	const TSharedPtr<FJsonObject>* WorldPartitionObj = nullptr;
	if (Root->TryGetObjectField(TEXT("world_partition"), WorldPartitionObj) && WorldPartitionObj && WorldPartitionObj->IsValid())
	{
		if ((*WorldPartitionObj)->HasField(TEXT("streaming_enabled")))
		{
			FUeAgentRequestContext SubCtx = Ctx;
			SubCtx.Params = *WorldPartitionObj;
			TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
			FString SubError;
			const bool bOk = CmdWorldPartitionSetStreamingEnabled(SubCtx, SubData, SubError);
			SubData->SetStringField(TEXT("operation"), TEXT("world_partition_set_streaming_enabled"));
			SubData->SetBoolField(TEXT("ok"), bOk);
			SubData->SetStringField(TEXT("error"), SubError);
			Results.Add(MakeShared<FJsonValueObject>(SubData));
			if (!bOk)
			{
				OutError = SubError;
				OutData->SetArrayField(TEXT("results"), Results);
				return false;
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* LoadRegions = nullptr;
		if ((*WorldPartitionObj)->TryGetArrayField(TEXT("load_regions"), LoadRegions) && LoadRegions)
		{
			for (const TSharedPtr<FJsonValue>& Value : *LoadRegions)
			{
				const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Item.IsValid())
				{
					continue;
				}
				FUeAgentRequestContext SubCtx = Ctx;
				SubCtx.Params = Item;
				TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
				FString SubError;
				const bool bOk = CmdWorldPartitionLoadRegion(SubCtx, SubData, SubError);
				SubData->SetStringField(TEXT("operation"), TEXT("world_partition_load_region"));
				SubData->SetBoolField(TEXT("ok"), bOk);
				SubData->SetStringField(TEXT("error"), SubError);
				Results.Add(MakeShared<FJsonValueObject>(SubData));
				if (!bOk)
				{
					OutError = SubError;
					OutData->SetArrayField(TEXT("results"), Results);
					return false;
				}
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* LoadActors = nullptr;
		if ((*WorldPartitionObj)->TryGetArrayField(TEXT("load_actors"), LoadActors) && LoadActors)
		{
			for (const TSharedPtr<FJsonValue>& Value : *LoadActors)
			{
				const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Item.IsValid())
				{
					continue;
				}
				FUeAgentRequestContext SubCtx = Ctx;
				SubCtx.Params = Item;
				TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
				FString SubError;
				const bool bOk = CmdWorldPartitionLoadActors(SubCtx, SubData, SubError);
				SubData->SetStringField(TEXT("operation"), TEXT("world_partition_load_actors"));
				SubData->SetBoolField(TEXT("ok"), bOk);
				SubData->SetStringField(TEXT("error"), SubError);
				Results.Add(MakeShared<FJsonValueObject>(SubData));
				if (!bOk)
				{
					OutError = SubError;
					OutData->SetArrayField(TEXT("results"), Results);
					return false;
				}
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* UnloadRegions = nullptr;
		if ((*WorldPartitionObj)->TryGetArrayField(TEXT("unload_regions"), UnloadRegions) && UnloadRegions)
		{
			for (const TSharedPtr<FJsonValue>& Value : *UnloadRegions)
			{
				const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Item.IsValid())
				{
					continue;
				}
				FUeAgentRequestContext SubCtx = Ctx;
				SubCtx.Params = Item;
				TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
				FString SubError;
				const bool bOk = CmdWorldPartitionUnloadRegion(SubCtx, SubData, SubError);
				SubData->SetStringField(TEXT("operation"), TEXT("world_partition_unload_region"));
				SubData->SetBoolField(TEXT("ok"), bOk);
				SubData->SetStringField(TEXT("error"), SubError);
				Results.Add(MakeShared<FJsonValueObject>(SubData));
				if (!bOk)
				{
					OutError = SubError;
					OutData->SetArrayField(TEXT("results"), Results);
					return false;
				}
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* UnloadActors = nullptr;
		if ((*WorldPartitionObj)->TryGetArrayField(TEXT("unload_actors"), UnloadActors) && UnloadActors)
		{
			for (const TSharedPtr<FJsonValue>& Value : *UnloadActors)
			{
				const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Item.IsValid())
				{
					continue;
				}
				FUeAgentRequestContext SubCtx = Ctx;
				SubCtx.Params = Item;
				TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
				FString SubError;
				const bool bOk = CmdWorldPartitionUnloadActors(SubCtx, SubData, SubError);
				SubData->SetStringField(TEXT("operation"), TEXT("world_partition_unload_actors"));
				SubData->SetBoolField(TEXT("ok"), bOk);
				SubData->SetStringField(TEXT("error"), SubError);
				Results.Add(MakeShared<FJsonValueObject>(SubData));
				if (!bOk)
				{
					OutError = SubError;
					OutData->SetArrayField(TEXT("results"), Results);
					return false;
				}
			}
		}
		const TSharedPtr<FJsonObject>* GenerateStreaming = nullptr;
		if ((*WorldPartitionObj)->TryGetObjectField(TEXT("generate_streaming"), GenerateStreaming) && GenerateStreaming && GenerateStreaming->IsValid())
		{
			FUeAgentRequestContext SubCtx = Ctx;
			SubCtx.Params = *GenerateStreaming;
			TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
			FString SubError;
			const bool bOk = CmdWorldPartitionGenerateStreaming(SubCtx, SubData, SubError);
			SubData->SetStringField(TEXT("operation"), TEXT("world_partition_generate_streaming"));
			SubData->SetBoolField(TEXT("ok"), bOk);
			SubData->SetStringField(TEXT("error"), SubError);
			Results.Add(MakeShared<FJsonValueObject>(SubData));
			if (!bOk)
			{
				OutError = SubError;
				OutData->SetArrayField(TEXT("results"), Results);
				return false;
			}
		}
	}
	const TSharedPtr<FJsonObject>* HLODObj = nullptr;
	if (Root->TryGetObjectField(TEXT("hlod"), HLODObj) && HLODObj && HLODObj->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* HLODLayers = nullptr;
		if ((*HLODObj)->TryGetArrayField(TEXT("layers"), HLODLayers) && HLODLayers)
		{
			for (const TSharedPtr<FJsonValue>& Value : *HLODLayers)
			{
				const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Item.IsValid())
				{
					continue;
				}
				TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
				Params->SetObjectField(TEXT("json"), Item);
				FString AssetPath;
				if (Item->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					Params->SetStringField(TEXT("asset_path"), AssetPath);
				}
				FUeAgentRequestContext SubCtx = Ctx;
				SubCtx.Params = Params;
				TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
				FString SubError;
				const bool bOk = CmdHLODLayerApplyJson(SubCtx, SubData, SubError);
				SubData->SetStringField(TEXT("operation"), TEXT("hlod_layer_apply_json"));
				SubData->SetBoolField(TEXT("ok"), bOk);
				SubData->SetStringField(TEXT("error"), SubError);
				Results.Add(MakeShared<FJsonValueObject>(SubData));
				if (!bOk)
				{
					OutError = SubError;
					OutData->SetArrayField(TEXT("results"), Results);
					return false;
				}
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* HLODAssignments = nullptr;
		if ((*HLODObj)->TryGetArrayField(TEXT("actor_assignments"), HLODAssignments) && HLODAssignments)
		{
			for (const TSharedPtr<FJsonValue>& Value : *HLODAssignments)
			{
				const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Item.IsValid())
				{
					continue;
				}
				FUeAgentRequestContext SubCtx = Ctx;
				SubCtx.Params = Item;
				TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
				FString SubError;
				const bool bOk = CmdHLODAssignActors(SubCtx, SubData, SubError);
				SubData->SetStringField(TEXT("operation"), TEXT("hlod_assign_actors"));
				SubData->SetBoolField(TEXT("ok"), bOk);
				SubData->SetStringField(TEXT("error"), SubError);
				Results.Add(MakeShared<FJsonValueObject>(SubData));
				if (!bOk)
				{
					OutError = SubError;
					OutData->SetArrayField(TEXT("results"), Results);
					return false;
				}
			}
		}
		const TSharedPtr<FJsonObject>* HLODBuild = nullptr;
		if ((*HLODObj)->TryGetObjectField(TEXT("build"), HLODBuild) && HLODBuild && HLODBuild->IsValid())
		{
			FUeAgentRequestContext SubCtx = Ctx;
			SubCtx.Params = *HLODBuild;
			TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
			FString SubError;
			const bool bOk = CmdHLODBuild(SubCtx, SubData, SubError);
			SubData->SetStringField(TEXT("operation"), TEXT("hlod_build"));
			SubData->SetBoolField(TEXT("ok"), bOk);
			SubData->SetStringField(TEXT("error"), SubError);
			Results.Add(MakeShared<FJsonValueObject>(SubData));
			if (!bOk)
			{
				OutError = SubError;
				OutData->SetArrayField(TEXT("results"), Results);
				return false;
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* Tasks = nullptr;
	if (Root->TryGetArrayField(TEXT("tasks"), Tasks) && Tasks)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Tasks)
		{
			const TSharedPtr<FJsonObject> Task = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Task.IsValid())
			{
				continue;
			}
			FString Type;
			Task->TryGetStringField(TEXT("type"), Type);
			if (Type.IsEmpty())
			{
				Task->TryGetStringField(TEXT("command"), Type);
			}
			FUeAgentRequestContext SubCtx = Ctx;
			SubCtx.Params = Task;
			TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
			FString SubError;
			bool bOk = false;
			if (Type.Equals(TEXT("hlod_build"), ESearchCase::IgnoreCase))
			{
				bOk = CmdHLODBuild(SubCtx, SubData, SubError);
			}
			else if (Type.Equals(TEXT("world_partition_generate_streaming"), ESearchCase::IgnoreCase))
			{
				bOk = CmdWorldPartitionGenerateStreaming(SubCtx, SubData, SubError);
			}
			else if (Type.Equals(TEXT("world_partition_resave_actors"), ESearchCase::IgnoreCase))
			{
				bOk = CmdWorldPartitionResaveActors(SubCtx, SubData, SubError);
			}
			else
			{
				SubError = TEXT("task_type_unsupported");
			}
			SubData->SetStringField(TEXT("operation"), Type);
			SubData->SetBoolField(TEXT("ok"), bOk);
			SubData->SetStringField(TEXT("error"), SubError);
			Results.Add(MakeShared<FJsonValueObject>(SubData));
			if (!bOk)
			{
				OutError = SubError;
				OutData->SetArrayField(TEXT("results"), Results);
				return false;
			}
		}
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.level_topology.apply_result.v1"));
	OutData->SetStringField(TEXT("source"), Source);
	OutData->SetStringField(TEXT("status"), TEXT("applied"));
	OutData->SetBoolField(TEXT("dry_run"), false);
	OutData->SetArrayField(TEXT("results"), Results);
	OutData->SetBoolField(TEXT("changed"), Results.Num() > 0);
	return true;
}
