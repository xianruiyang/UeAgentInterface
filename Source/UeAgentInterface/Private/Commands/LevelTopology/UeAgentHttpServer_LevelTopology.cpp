// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"

#include "UeAgentHttpServer_LevelTopologyOps.h"
#include "Utilities/UeAgentAuthoringFileIO.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/UeAgentStructuredAuthoringIO.h"
#include "Utilities/UeAgentUObjectProperties.h"

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
	static constexpr const TCHAR* HLODLayerSchema = TEXT("uai.hlod_layer.v1");

	static TMap<FString, UObject*>& GetSessionAssetRegistry()
	{
		static TMap<FString, UObject*> Registry;
		return Registry;
	}

	FString NormalizePackagePath(const FString& InPath)
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

	UObject* LoadObjectByPath(const FString& InPath)
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

	bool AssetExists(const FString& InPath)
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

	bool IsValidLongPackagePath(const FString& InPath)
	{
		const FString Path = NormalizePackagePath(InPath);
		return !Path.IsEmpty() && FPackageName::IsValidLongPackageName(Path);
	}

	FString ObjectPackagePath(UObject* Object)
	{
		return Object && Object->GetOutermost() ? Object->GetOutermost()->GetName() : FString();
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

	static FString ResolveAuthoringFilePath(const FString& InPath)
	{
		FString ResolvedPath = UeAgentStructuredAuthoringIO::NormalizeAuthoringPath(ResolveFilePath(InPath));
		if (FPaths::GetExtension(ResolvedPath, false).Equals(TEXT("json"), ESearchCase::IgnoreCase))
		{
			ResolvedPath = FPaths::SetExtension(ResolvedPath, TEXT("hjson"));
		}
		return ResolvedPath;
	}

	bool LoadJsonFromParams(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutRoot, FString& OutSource, FString& OutError)
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
			JsonFile = ResolveAuthoringFilePath(JsonFile);
			OutSource = JsonFile;
			UeAgentAuthoringFileIO::FLoadOptions Options;
			return UeAgentAuthoringFileIO::LoadObject(JsonFile, OutRoot, Options, OutError);
		}

		OutError = TEXT("missing_json");
		return false;
	}

	bool SaveJsonIfRequested(const TSharedPtr<FJsonObject>& Obj, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, FString& OutError)
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
		OutputFile = ResolveAuthoringFilePath(OutputFile);
		UeAgentAuthoringFileIO::FSaveOptions Options;
		Options.SourceCommand = TEXT("level_topology_export");
		Options.bExternalizeInlineDetails = false;
		FString SaveError;
		if (!UeAgentAuthoringFileIO::SaveObject(OutputFile, Obj, Options, &SaveError))
		{
			OutError = SaveError.IsEmpty() ? TEXT("output_file_write_failed") : SaveError;
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), OutputFile);
		OutData->SetStringField(TEXT("output_format"), TEXT("hjson_authoring"));
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

	void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Severity, const FString& Code, const FString& Path, const FString& Message)
	{
		Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(Severity, Code, Path, Message)));
	}

	int32 CountIssues(const TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Severity)
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

	TArray<FString> ReadStringArray(const TSharedPtr<FJsonObject>& Obj, const FString& Key)
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

	bool TryReadBox(const TSharedPtr<FJsonObject>& Obj, FBox& OutBox)
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

	TSharedPtr<FJsonObject> BoxToJson(const FBox& Box)
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

	bool SavePackages(const TArray<UPackage*>& Packages, TArray<FString>& OutSavedPackages, FString& OutError)
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

	ULevelStreaming* FindStreamingLevel(UWorld* World, const FString& LevelPackageOrName)
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

	TSharedPtr<FJsonObject> BuildStreamingLevelJson(ULevelStreaming* StreamingLevel, UWorld* World)
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

	bool ResolveActors(UWorld* World, const TSharedPtr<FJsonObject>& Params, TArray<AActor*>& OutActors, TArray<TSharedPtr<FJsonValue>>& OutResults, FString& OutError)
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

	UDataLayerEditorSubsystem* GetDataLayerSubsystem()
	{
		return GEditor ? GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>() : nullptr;
	}

	AWorldDataLayers* FindWorldDataLayers(UWorld* World)
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

	bool SupportsDataLayerInstanceAuthoring(UWorld* World)
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

	UDataLayerInstance* FindDataLayerInstance(UWorld* World, const FString& LayerNameOrPath)
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

	bool ActorContainsDataLayerInstance(AActor* Actor, UDataLayerInstance* Layer)
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

	bool ParseDataLayerRuntimeState(const FString& Text, EDataLayerRuntimeState& OutState)
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

	bool ApplyCanonicalProperty(UObject* Object, const FString& PropertyName, const TSharedPtr<FJsonValue>& CanonicalValue, TSharedPtr<FJsonObject>& OutResult)
	{
		if (!Object || PropertyName.IsEmpty() || !CanonicalValue.IsValid())
		{
			OutResult = MakeShared<FJsonObject>();
			OutResult->SetStringField(TEXT("status"), TEXT("invalid_canonical_property_request"));
			return false;
		}
		FProperty* Property = Object->GetClass()->FindPropertyByName(FName(*PropertyName));
		if (!Property)
		{
			OutResult = MakeShared<FJsonObject>();
			OutResult->SetStringField(TEXT("status"), TEXT("property_not_found"));
			return false;
		}
		UeAgentUObjectProperties::FExportOptions ExportOptions;
		TSharedPtr<FJsonObject> Descriptor = UeAgentUObjectProperties::BuildPropertyDescriptor(Object, Property, ExportOptions);
		Descriptor->SetField(TEXT("value"), CanonicalValue);
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
		Payload->SetArrayField(TEXT("properties"), { MakeShared<FJsonValueObject>(Descriptor) });
		OutResult = UeAgentUObjectProperties::ApplyObjectProperties(Object, Payload, UeAgentUObjectProperties::FApplyOptions());
		FString Status;
		return OutResult.IsValid() && OutResult->TryGetStringField(TEXT("status"), Status) && Status == TEXT("applied");
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

	bool ParseHLODLayerType(const FString& Text, EHLODLayerType& OutType)
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

	TSharedPtr<FJsonObject> HLODLayerToJson(UHLODLayer* Layer)
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

	bool CreateOrLoadAssetWithFactory(const FString& AssetPath, UClass* AssetClass, UFactory* Factory, const FString& OverwritePolicy, UObject*& OutAsset, FString& OutError)
	{
		if (!OverwritePolicy.Equals(TEXT("fail"), ESearchCase::IgnoreCase)
			&& !OverwritePolicy.Equals(TEXT("replace"), ESearchCase::IgnoreCase))
		{
			OutError = TEXT("overwrite_policy_invalid");
			return false;
		}
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

	bool IsNullRHI()
	{
		return FParse::Param(FCommandLine::Get(), TEXT("NullRHI")) || !FApp::CanEverRender();
	}

	bool RunCommandletProcess(const FString& ExtraArgs, double TimeoutSeconds, TSharedPtr<FJsonObject>& OutData, FString& OutError)
	{
		if (!FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0.0)
		{
			OutError = TEXT("timeout_seconds_invalid");
			return false;
		}
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

}

bool FUeAgentHttpServer::ExecuteLevelTopologyCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("level_topology_export_json")) return CmdLevelTopologyExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_topology_validate_json")) return CmdLevelTopologyValidateJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_topology_diff_json")) return CmdLevelTopologyDiffJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_topology_apply_json")) return CmdLevelTopologyApplyJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_streaming_query")) return CmdLevelStreamingQuery(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_streaming_create_level")) return CmdLevelStreamingCreateLevel(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_streaming_add_existing")) return CmdLevelStreamingAddExisting(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_streaming_remove")) return CmdLevelStreamingRemove(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_streaming_set_state")) return CmdLevelStreamingSetState(Ctx, OutData, OutError);
	if (CommandLower == TEXT("level_streaming_move_actors")) return CmdLevelStreamingMoveActors(Ctx, OutData, OutError);
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
	bool bResetTransactionBuffer = false;
	bool bDeleteAsset = false;
	bool bTransaction = true;
	bool bDryRun = true;
	bool bAllowDestructive = false;
	bool bAllowDelete = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_selection"), bClearSelection);
	JsonTryGetBool(Ctx.Params, TEXT("reset_transaction_buffer"), bResetTransactionBuffer);
	JsonTryGetBool(Ctx.Params, TEXT("delete_asset"), bDeleteAsset);
	JsonTryGetBool(Ctx.Params, TEXT("transaction"), bTransaction);
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("allow_destructive"), bAllowDestructive);
	JsonTryGetBool(Ctx.Params, TEXT("allow_delete"), bAllowDelete);
	const FString RemovedPackage = UeAgentLevelTopology::NormalizePackagePath(StreamingLevel->GetWorldAssetPackageName());
	UObject* AssetToDelete = nullptr;
	if (bDeleteAsset)
	{
		AssetToDelete = UeAgentLevelTopology::LoadObjectByPath(RemovedPackage);
		if (!AssetToDelete)
		{
			OutError = TEXT("streaming_level_asset_not_found");
			return false;
		}
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.level_streaming.remove_result.v1"));
	OutData->SetStringField(TEXT("level_package"), RemovedPackage);
	OutData->SetBoolField(TEXT("removed"), false);
	OutData->SetBoolField(TEXT("delete_asset"), bDeleteAsset);
	OutData->SetNumberField(TEXT("deleted_asset_count"), 0);
	OutData->SetBoolField(TEXT("transaction"), bTransaction);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("allow_destructive"), bAllowDestructive);
	OutData->SetBoolField(TEXT("allow_delete"), bAllowDelete);
	if (bDryRun)
	{
		OutData->SetStringField(TEXT("status"), TEXT("planned"));
		return true;
	}
	if (!bAllowDestructive)
	{
		OutData->SetStringField(TEXT("status"), TEXT("blocked"));
		OutData->SetStringField(TEXT("error_code"), TEXT("requires_allow_destructive"));
		OutError = TEXT("requires_allow_destructive");
		return false;
	}
	if (bDeleteAsset && !bAllowDelete)
	{
		OutData->SetStringField(TEXT("status"), TEXT("blocked"));
		OutData->SetStringField(TEXT("error_code"), TEXT("requires_allow_delete"));
		OutError = TEXT("requires_allow_delete");
		return false;
	}
	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UeAgentInterface", "LevelStreamingRemove", "UeAgentInterface Level Streaming Remove"));
	}
	const bool bRemoved = UEditorLevelUtils::RemoveLevelFromWorld(StreamingLevel->GetLoadedLevel(), bClearSelection, bResetTransactionBuffer);
	int32 DeletedAssets = 0;
	if (bRemoved && bDeleteAsset)
	{
		TArray<UObject*> ObjectsToDelete;
		ObjectsToDelete.Add(AssetToDelete);
		DeletedAssets = ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete);
	}
	OutData->SetBoolField(TEXT("removed"), bRemoved);
	OutData->SetNumberField(TEXT("deleted_asset_count"), DeletedAssets);
	if (!bRemoved)
	{
		OutData->SetStringField(TEXT("status"), TEXT("failed"));
		OutData->SetStringField(TEXT("error_code"), TEXT("streaming_level_remove_failed"));
		OutError = TEXT("streaming_level_remove_failed");
		return false;
	}
	if (bDeleteAsset && DeletedAssets <= 0)
	{
		OutData->SetStringField(TEXT("status"), TEXT("partial_failure"));
		OutData->SetStringField(TEXT("error_code"), TEXT("streaming_level_asset_delete_failed"));
		OutError = TEXT("streaming_level_asset_delete_failed");
		return false;
	}
	OutData->SetStringField(TEXT("status"), TEXT("removed"));
	return true;
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
	if (LevelPackage.TrimStartAndEnd().IsEmpty())
	{
		UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("missing_validation_expectation"), TEXT("$.level_package"), TEXT("level_streaming_validate requires level_package."));
	}
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
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("selected"), LoadFilterText);
		TSharedPtr<FJsonObject> ApplyResult;
		if (!UeAgentLevelTopology::ApplyCanonicalProperty(DataLayerAsset, TEXT("LoadFilter"), MakeShared<FJsonValueObject>(Value), ApplyResult))
		{
			OutError = TEXT("data_layer_load_filter_apply_failed");
			return false;
		}
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
	bool bDryRun = true;
	bool bAllowDestructive = false;
	bool bAllowDelete = false;
	JsonTryGetBool(Ctx.Params, TEXT("delete_asset"), bDeleteAsset);
	JsonTryGetBool(Ctx.Params, TEXT("transaction"), bTransaction);
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("allow_destructive"), bAllowDestructive);
	JsonTryGetBool(Ctx.Params, TEXT("allow_delete"), bAllowDelete);
	if (bDeleteAsset && !Asset)
	{
		OutError = TEXT("data_layer_asset_not_found");
		return false;
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.data_layer.delete_instance_result.v1"));
	OutData->SetStringField(TEXT("instance_name"), InstanceName);
	OutData->SetBoolField(TEXT("deleted"), false);
	OutData->SetBoolField(TEXT("delete_asset"), bDeleteAsset);
	OutData->SetNumberField(TEXT("deleted_asset_count"), 0);
	OutData->SetBoolField(TEXT("transaction"), bTransaction);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("allow_destructive"), bAllowDestructive);
	OutData->SetBoolField(TEXT("allow_delete"), bAllowDelete);
	if (bDryRun)
	{
		OutData->SetStringField(TEXT("status"), TEXT("planned"));
		return true;
	}
	if (!bAllowDestructive)
	{
		OutData->SetStringField(TEXT("status"), TEXT("blocked"));
		OutData->SetStringField(TEXT("error_code"), TEXT("requires_allow_destructive"));
		OutError = TEXT("requires_allow_destructive");
		return false;
	}
	if (!bAllowDelete)
	{
		OutData->SetStringField(TEXT("status"), TEXT("blocked"));
		OutData->SetStringField(TEXT("error_code"), TEXT("requires_allow_delete"));
		OutError = TEXT("requires_allow_delete");
		return false;
	}
	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UeAgentInterface", "DataLayerDeleteInstance", "UeAgentInterface DataLayer Delete Instance"));
	}
	bool bDeleted = false;
	if (Subsystem)
	{
		Subsystem->DeleteDataLayer(Instance);
		bDeleted = UeAgentLevelTopology::FindDataLayerInstance(World, InstanceName) == nullptr;
	}
	else
	{
		AWorldDataLayers* WorldDataLayers = UeAgentLevelTopology::FindWorldDataLayers(World);
		bDeleted = WorldDataLayers && WorldDataLayers->RemoveDataLayer(Instance, true);
		if (!bDeleted)
		{
			OutError = TEXT("data_layer_instance_delete_failed");
			OutData->SetStringField(TEXT("status"), TEXT("failed"));
			OutData->SetStringField(TEXT("error_code"), OutError);
			return false;
		}
		WorldDataLayers->MarkPackageDirty();
	}
	OutData->SetBoolField(TEXT("deleted"), bDeleted);
	if (!bDeleted)
	{
		OutData->SetStringField(TEXT("status"), TEXT("failed"));
		OutData->SetStringField(TEXT("error_code"), TEXT("data_layer_instance_delete_failed"));
		OutError = TEXT("data_layer_instance_delete_failed");
		return false;
	}
	int32 DeletedAssetCount = 0;
	if (bDeleteAsset && Asset)
	{
		TArray<UObject*> ObjectsToDelete;
		ObjectsToDelete.Add(const_cast<UDataLayerAsset*>(Asset));
		DeletedAssetCount = ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete);
	}
	OutData->SetNumberField(TEXT("deleted_asset_count"), DeletedAssetCount);
	if (bDeleteAsset && DeletedAssetCount <= 0)
	{
		OutData->SetStringField(TEXT("status"), TEXT("partial_failure"));
		OutData->SetStringField(TEXT("error_code"), TEXT("data_layer_asset_delete_failed"));
		OutError = TEXT("data_layer_asset_delete_failed");
		return false;
	}
	OutData->SetStringField(TEXT("status"), TEXT("deleted"));
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
	if (!Mode.Equals(TEXT("add"), ESearchCase::IgnoreCase)
		&& !Mode.Equals(TEXT("remove"), ESearchCase::IgnoreCase)
		&& !Mode.Equals(TEXT("replace"), ESearchCase::IgnoreCase)
		&& !Mode.Equals(TEXT("clear"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("data_layer_membership_mode_invalid");
		return false;
	}
	Mode = Mode.ToLower();
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
	const TArray<FString> LayerNames = UeAgentLevelTopology::ReadStringArray(Ctx.Params, TEXT("layers"));
	const TArray<FString> ExpectedMembership = UeAgentLevelTopology::ReadStringArray(Ctx.Params, TEXT("expect_actor_membership"));
	const bool bHasActorExpectation = Ctx.Params->HasField(TEXT("actor_ids")) && !ExpectedMembership.IsEmpty();
	if (LayerNames.IsEmpty() && !bHasActorExpectation)
	{
		UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("missing_validation_expectation"), TEXT("$"), TEXT("data_layer_validate requires layers or actor_ids with expect_actor_membership."));
	}
	for (const FString& LayerName : LayerNames)
	{
		if (!UeAgentLevelTopology::FindDataLayerInstance(World, LayerName))
		{
			UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("data_layer_instance_not_found"), LayerName, TEXT("DataLayer instance was not found."));
		}
	}
	TArray<AActor*> Actors;
	TArray<TSharedPtr<FJsonValue>> ActorResults;
	FString ActorError;
	if (bHasActorExpectation)
	{
		if (!UeAgentLevelTopology::ResolveActors(World, Ctx.Params, Actors, ActorResults, ActorError))
		{
			UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("actor_selection_invalid"), TEXT("$.actor_ids"), ActorError);
		}
		else
		{
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
	}
	const int32 ErrorCount = UeAgentLevelTopology::CountIssues(Issues, TEXT("error"));
	OutData->SetStringField(TEXT("schema"), TEXT("uai.data_layer.validate_result.v1"));
	OutData->SetStringField(TEXT("status"), ErrorCount == 0 ? TEXT("valid") : TEXT("invalid"));
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), ErrorCount);
	OutData->SetArrayField(TEXT("actor_results"), ActorResults);
	return ErrorCount == 0;
}
