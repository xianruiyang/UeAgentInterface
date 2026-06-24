// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"
#include "UeAgentLevelContentJsonOps.h"

#include "ActorFactories/ActorFactory.h"
#include "Builders/CubeBuilder.h"
#include "Camera/CameraActor.h"
#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorActorFolders.h"
#include "Engine/Light.h"
#include "Engine/OverlapResult.h"
#include "Engine/PointLight.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TargetPoint.h"
#include "Engine/TriggerBox.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include <initializer_list>

namespace UeAgentLevelContentJson
{
	static const TCHAR* LevelContentSchema = TEXT("uai.level_content.v1");
	static const TCHAR* QuerySchema = TEXT("uai.level_content.query.v1");
	static const TCHAR* SnapshotSchema = TEXT("uai.level_content.snapshot.v1");
	static const TCHAR* ValidationSchema = TEXT("uai.level_content.validation.v1");
	static const TCHAR* PlanSchema = TEXT("uai.level_content.plan.v1");
	static const TCHAR* ApplySchema = TEXT("uai.level_content.apply_result.v1");
	static const TCHAR* DiffSchema = TEXT("uai.level_content.diff.v1");
	static const TCHAR* DeleteScopeSchema = TEXT("uai.level_content.delete_scope.v1");
	static const TCHAR* MergeSchema = TEXT("uai.level_content.merge.v1");
	static const TCHAR* RepairSchema = TEXT("uai.level_content.repair.v1");

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

	static FString JsonValueToString(const TSharedPtr<FJsonValue>& Value)
	{
		FString Out;
		if (!Value.IsValid())
		{
			return Out;
		}
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Value, TEXT(""), Writer);
		return Out;
	}

	static bool ParseJsonObject(const FString& Text, TSharedPtr<FJsonObject>& OutObj, FString& OutError)
	{
		TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
		{
			OutError = TEXT("json_parse_failed");
			return false;
		}
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

	static bool HasErrorIssue(const TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		for (const TSharedPtr<FJsonValue>& Value : Issues)
		{
			const TSharedPtr<FJsonObject> Issue = Value.IsValid() ? Value->AsObject() : nullptr;
			if (Issue.IsValid() && Issue->GetStringField(TEXT("severity")).Equals(TEXT("error"), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static bool IsTruthyString(const FString& Text)
	{
		return Text.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("1"));
	}

	static TSharedPtr<FJsonObject> MakeVectorJson(const FVector& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), Value.Pitch);
		Obj->SetNumberField(TEXT("yaw"), Value.Yaw);
		Obj->SetNumberField(TEXT("roll"), Value.Roll);
		return Obj;
	}

	static FString NormalizeFolder(const FString& Folder)
	{
		FString Result = Folder.TrimStartAndEnd();
		Result.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Result.StartsWith(TEXT("/")))
		{
			Result.RightChopInline(1, EAllowShrinking::No);
		}
		while (Result.EndsWith(TEXT("/")))
		{
			Result.LeftChopInline(1, EAllowShrinking::No);
		}
		return Result;
	}

	static FString ResolveFilePath(const FString& Path)
	{
		FString CleanPath = Path;
		CleanPath.TrimStartAndEndInline();
		if (CleanPath.IsEmpty() || FPaths::IsRelative(CleanPath) == false)
		{
			return CleanPath;
		}
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), CleanPath);
	}

	static bool SaveJsonIfRequested(const TSharedPtr<FJsonObject>& Obj, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, FString& OutError)
	{
		if (!Params.IsValid() || !Obj.IsValid())
		{
			return true;
		}

		FString OutputFile;
		if (!Params->TryGetStringField(TEXT("output_file"), OutputFile) || OutputFile.TrimStartAndEnd().IsEmpty())
		{
			return true;
		}

		OutputFile = ResolveFilePath(OutputFile);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputFile), true);
		const FString Text = JsonToString(Obj, true);
		if (!FFileHelper::SaveStringToFile(Text, *OutputFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = TEXT("output_file_write_failed");
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), OutputFile);
		return true;
	}

	static bool ResolveContentObject(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutContent, FString& OutSource, FString& OutError)
	{
		OutContent.Reset();
		OutSource = TEXT("params");
		if (!Params.IsValid())
		{
			OutError = TEXT("missing_params");
			return false;
		}

		const TCHAR* ObjectKeys[] = { TEXT("content"), TEXT("level_content"), TEXT("json"), TEXT("target_json") };
		for (const TCHAR* Key : ObjectKeys)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Params->TryGetObjectField(Key, Obj) && Obj && Obj->IsValid())
			{
				OutContent = *Obj;
				OutSource = Key;
				return true;
			}
		}

		FString JsonText;
		const TCHAR* TextKeys[] = { TEXT("json_text"), TEXT("content_json"), TEXT("target_json_text") };
		for (const TCHAR* Key : TextKeys)
		{
			if (Params->TryGetStringField(Key, JsonText) && !JsonText.TrimStartAndEnd().IsEmpty())
			{
				OutSource = Key;
				return ParseJsonObject(JsonText, OutContent, OutError);
			}
		}

		FString JsonFile;
		const TCHAR* FileKeys[] = { TEXT("json_file"), TEXT("file_path"), TEXT("input_file"), TEXT("content_file") };
		for (const TCHAR* Key : FileKeys)
		{
			if (Params->TryGetStringField(Key, JsonFile) && !JsonFile.TrimStartAndEnd().IsEmpty())
			{
				JsonFile = ResolveFilePath(JsonFile);
				if (!FFileHelper::LoadFileToString(JsonText, *JsonFile))
				{
					OutError = TEXT("json_file_read_failed");
					return false;
				}
				OutSource = JsonFile;
				return ParseJsonObject(JsonText, OutContent, OutError);
			}
		}

		if (Params->HasField(TEXT("schema")) || Params->HasField(TEXT("resources")) || Params->HasField(TEXT("actors")) || Params->HasField(TEXT("operations")))
		{
			OutContent = Params;
			return true;
		}

		OutError = TEXT("missing_level_content_json");
		return false;
	}

	bool TryGetStringAny(const TSharedPtr<FJsonObject>& Obj, std::initializer_list<const TCHAR*> Keys, FString& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		for (const TCHAR* Key : Keys)
		{
			if (Obj->TryGetStringField(Key, OutValue))
			{
				return true;
			}
		}
		return false;
	}

	bool TryGetBoolAny(const TSharedPtr<FJsonObject>& Obj, std::initializer_list<const TCHAR*> Keys, bool& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		for (const TCHAR* Key : Keys)
		{
			if (Obj->TryGetBoolField(Key, OutValue))
			{
				return true;
			}
			FString Text;
			if (Obj->TryGetStringField(Key, Text))
			{
				OutValue = IsTruthyString(Text);
				return true;
			}
		}
		return false;
	}

	static bool TryGetArray(const TSharedPtr<FJsonObject>& Obj, const FString& Key, const TArray<TSharedPtr<FJsonValue>>*& OutArray)
	{
		OutArray = nullptr;
		return Obj.IsValid() && Obj->TryGetArrayField(Key, OutArray) && OutArray;
	}

	bool TryGetObject(const TSharedPtr<FJsonObject>& Obj, const FString& Key, TSharedPtr<FJsonObject>& OutObj)
	{
		OutObj.Reset();
		const TSharedPtr<FJsonObject>* Found = nullptr;
		if (Obj.IsValid() && Obj->TryGetObjectField(Key, Found) && Found && Found->IsValid())
		{
			OutObj = *Found;
			return true;
		}
		return false;
	}

	static bool TryReadVectorValue(const TSharedPtr<FJsonValue>& Value, FVector& Out)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
			if (Arr.Num() < 3)
			{
				return false;
			}
			Out.X = Arr[0].IsValid() ? Arr[0]->AsNumber() : 0.0;
			Out.Y = Arr[1].IsValid() ? Arr[1]->AsNumber() : 0.0;
			Out.Z = Arr[2].IsValid() ? Arr[2]->AsNumber() : 0.0;
			return true;
		}
		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (!Obj.IsValid())
			{
				return false;
			}
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			if ((!Obj->TryGetNumberField(TEXT("x"), X) && !Obj->TryGetNumberField(TEXT("X"), X)) ||
				(!Obj->TryGetNumberField(TEXT("y"), Y) && !Obj->TryGetNumberField(TEXT("Y"), Y)) ||
				(!Obj->TryGetNumberField(TEXT("z"), Z) && !Obj->TryGetNumberField(TEXT("Z"), Z)))
			{
				return false;
			}
			Out = FVector(X, Y, Z);
			return true;
		}
		return false;
	}

	static bool TryReadRotatorValue(const TSharedPtr<FJsonValue>& Value, FRotator& Out)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
			if (Arr.Num() < 3)
			{
				return false;
			}
			Out.Pitch = Arr[0].IsValid() ? Arr[0]->AsNumber() : 0.0;
			Out.Yaw = Arr[1].IsValid() ? Arr[1]->AsNumber() : 0.0;
			Out.Roll = Arr[2].IsValid() ? Arr[2]->AsNumber() : 0.0;
			return true;
		}
		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (!Obj.IsValid())
			{
				return false;
			}
			double Pitch = 0.0;
			double Yaw = 0.0;
			double Roll = 0.0;
			if ((!Obj->TryGetNumberField(TEXT("pitch"), Pitch) && !Obj->TryGetNumberField(TEXT("Pitch"), Pitch)) ||
				(!Obj->TryGetNumberField(TEXT("yaw"), Yaw) && !Obj->TryGetNumberField(TEXT("Yaw"), Yaw)) ||
				(!Obj->TryGetNumberField(TEXT("roll"), Roll) && !Obj->TryGetNumberField(TEXT("Roll"), Roll)))
			{
				return false;
			}
			Out = FRotator(Pitch, Yaw, Roll);
			return true;
		}
		return false;
	}

	static bool TryGetVectorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& Out)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Key);
		return TryReadVectorValue(Value, Out);
	}

	static bool TryGetRotatorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FRotator& Out)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Key);
		return TryReadRotatorValue(Value, Out);
	}

	static TSharedPtr<FJsonValue> MakeStringArrayValue(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Arr.Add(MakeShared<FJsonValueString>(Value));
		}
		return MakeShared<FJsonValueArray>(Arr);
	}

	static TArray<FString> ReadStringArray(const TSharedPtr<FJsonObject>& Obj, const FString& Key)
	{
		TArray<FString> Result;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!TryGetArray(Obj, Key, Values) || !Values)
		{
			return Result;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid())
			{
				continue;
			}
			const FString Text = Value->AsString().TrimStartAndEnd();
			if (!Text.IsEmpty())
			{
				Result.Add(Text);
			}
		}
		return Result;
	}

	static TArray<FString> ReadTags(const TSharedPtr<FJsonObject>& Obj)
	{
		return ReadStringArray(Obj, TEXT("tags"));
	}

	static bool ActorHasTagString(AActor* Actor, const FString& Tag)
	{
		return Actor && !Tag.IsEmpty() && Actor->Tags.Contains(FName(*Tag));
	}

	static FString MakeManagedIdTag(const FString& Id)
	{
		return FString::Printf(TEXT("UAI.Id=%s"), *Id);
	}

	static FString GetManagedId(AActor* Actor)
	{
		if (!Actor)
		{
			return FString();
		}
		for (const FName& Tag : Actor->Tags)
		{
			const FString Text = Tag.ToString();
			if (Text.StartsWith(TEXT("UAI.Id=")))
			{
				return Text.Mid(7);
			}
			if (Text.StartsWith(TEXT("UAI.Id:")))
			{
				return Text.Mid(7);
			}
		}
		return FString();
	}

	static FString ActorStableId(AActor* Actor)
	{
		const FString ManagedId = GetManagedId(Actor);
		if (!ManagedId.IsEmpty())
		{
			return ManagedId;
		}
		return Actor ? Actor->GetActorLabel() : FString();
	}

	static bool FolderMatches(const FString& ActorFolder, const FString& RootFolder, bool bIncludeChildFolders)
	{
		const FString Folder = NormalizeFolder(ActorFolder);
		const FString Root = NormalizeFolder(RootFolder);
		if (Root.IsEmpty())
		{
			return true;
		}
		if (Folder.Equals(Root, ESearchCase::IgnoreCase))
		{
			return true;
		}
		return bIncludeChildFolders && Folder.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase);
	}

	static AActor* FindActorByNameLabelOrManagedId(UWorld* World, const FString& Id)
	{
		if (!World || Id.TrimStartAndEnd().IsEmpty())
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsActorBeingDestroyed())
			{
				continue;
			}
			if (Actor->GetName().Equals(Id, ESearchCase::IgnoreCase) ||
				Actor->GetActorLabel().Equals(Id, ESearchCase::IgnoreCase) ||
				Actor->GetPathName().Equals(Id, ESearchCase::IgnoreCase) ||
				GetManagedId(Actor).Equals(Id, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
		}
		return nullptr;
	}

	static FString GetResourceId(const TSharedPtr<FJsonObject>& Resource)
	{
		FString Id;
		if (TryGetStringAny(Resource, { TEXT("id"), TEXT("name"), TEXT("managed_id") }, Id))
		{
			return Id.TrimStartAndEnd();
		}
		TSharedPtr<FJsonObject> Match;
		if (TryGetObject(Resource, TEXT("match"), Match))
		{
			if (TryGetStringAny(Match, { TEXT("managed_id"), TEXT("label"), TEXT("object_path") }, Id))
			{
				return Id.TrimStartAndEnd();
			}
		}
		return FString();
	}

	static FString GetResourceOperation(const TSharedPtr<FJsonObject>& Resource, const FString& DefaultOperation)
	{
		FString Operation;
		if (TryGetStringAny(Resource, { TEXT("operation"), TEXT("op") }, Operation) && !Operation.TrimStartAndEnd().IsEmpty())
		{
			return Operation.TrimStartAndEnd().ToLower();
		}
		return DefaultOperation;
	}

	static FString GetResourceCategory(const TSharedPtr<FJsonObject>& Resource)
	{
		FString Category;
		if (TryGetStringAny(Resource, { TEXT("category") }, Category) && !Category.TrimStartAndEnd().IsEmpty())
		{
			return Category.TrimStartAndEnd().ToLower();
		}
		return TEXT("spatial");
	}

	static FString GetResourceKind(const TSharedPtr<FJsonObject>& Resource)
	{
		FString Kind;
		if (TryGetStringAny(Resource, { TEXT("kind"), TEXT("type") }, Kind) && !Kind.TrimStartAndEnd().IsEmpty())
		{
			return Kind.TrimStartAndEnd().ToLower();
		}
		return TEXT("actor");
	}

	static FString GetClassPathForKind(const FString& Kind)
	{
		if (Kind == TEXT("static_mesh") || Kind == TEXT("static_mesh_actor"))
		{
			return TEXT("/Script/Engine.StaticMeshActor");
		}
		if (Kind == TEXT("point_light") || Kind == TEXT("light"))
		{
			return TEXT("/Script/Engine.PointLight");
		}
		if (Kind == TEXT("camera") || Kind == TEXT("camera_actor"))
		{
			return TEXT("/Script/Engine.CameraActor");
		}
		if (Kind == TEXT("ambient_sound") || Kind == TEXT("audio"))
		{
			return TEXT("/Script/Engine.AmbientSound");
		}
		if (Kind == TEXT("trigger_box") || Kind == TEXT("trigger"))
		{
			return TEXT("/Script/Engine.TriggerBox");
		}
		if (Kind == TEXT("nav_mesh_bounds_volume") || Kind == TEXT("navmesh_bounds") || Kind == TEXT("nav_bounds"))
		{
			return TEXT("/Script/NavigationSystem.NavMeshBoundsVolume");
		}
		if (Kind == TEXT("probe") || Kind == TEXT("target_point") || Kind == TEXT("validation_anchor"))
		{
			return TEXT("/Script/Engine.TargetPoint");
		}
		if (Kind == TEXT("niagara") || Kind == TEXT("niagara_actor"))
		{
			return TEXT("/Script/Niagara.NiagaraActor");
		}
		if (Kind == TEXT("actor") || Kind == TEXT("custom"))
		{
			return TEXT("/Script/Engine.Actor");
		}
		return FString();
	}

	static FString GetClassPathForResource(const TSharedPtr<FJsonObject>& Resource)
	{
		FString ClassPath;
		if (TryGetStringAny(Resource, { TEXT("class"), TEXT("class_path"), TEXT("actor_class") }, ClassPath) && !ClassPath.TrimStartAndEnd().IsEmpty())
		{
			return ClassPath.TrimStartAndEnd();
		}
		return GetClassPathForKind(GetResourceKind(Resource));
	}

	static bool IsSpatialActorResource(const TSharedPtr<FJsonObject>& Resource)
	{
		const FString Category = GetResourceCategory(Resource);
		const FString Kind = GetResourceKind(Resource);
		if (Category == TEXT("spatial"))
		{
			return !GetClassPathForResource(Resource).IsEmpty();
		}
		if (Category == TEXT("validation") && (Kind == TEXT("probe") || Kind == TEXT("target_point")))
		{
			return true;
		}
		return false;
	}

	static void CollectResourceObjects(const TSharedPtr<FJsonObject>& Content, TArray<TSharedPtr<FJsonObject>>& OutResources)
	{
		OutResources.Reset();
		if (!Content.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Resources = nullptr;
		if (TryGetArray(Content, TEXT("resources"), Resources) && Resources)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Resources)
			{
				TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
				if (Obj.IsValid())
				{
					OutResources.Add(Obj);
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* LegacyActors = nullptr;
		if (TryGetArray(Content, TEXT("actors"), LegacyActors) && LegacyActors)
		{
			for (const TSharedPtr<FJsonValue>& Value : *LegacyActors)
			{
				TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
				if (Obj.IsValid())
				{
					OutResources.Add(Obj);
				}
			}
		}
	}

	static TSharedPtr<FJsonObject> GetScope(const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Scope;
		if (TryGetObject(Params, TEXT("scope"), Scope))
		{
			return Scope;
		}
		if (TryGetObject(Content, TEXT("scope"), Scope))
		{
			return Scope;
		}
		return MakeShared<FJsonObject>();
	}

	static FString GetScopeFolderRoot(const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Scope = GetScope(Content, Params);
		FString FolderRoot;
		if (TryGetStringAny(Scope, { TEXT("folder_root"), TEXT("folder_path"), TEXT("folder") }, FolderRoot))
		{
			return NormalizeFolder(FolderRoot);
		}
		return FString();
	}

	static FString GetManagedTag(const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Scope = GetScope(Content, Params);
		FString ManagedTag;
		if (TryGetStringAny(Scope, { TEXT("managed_tag") }, ManagedTag) && !ManagedTag.TrimStartAndEnd().IsEmpty())
		{
			return ManagedTag.TrimStartAndEnd();
		}
		return TEXT("UAI.Managed");
	}

	static TArray<FString> GetDefaultTags(const TSharedPtr<FJsonObject>& Content)
	{
		TArray<FString> Tags;
		TSharedPtr<FJsonObject> Defaults;
		if (TryGetObject(Content, TEXT("defaults"), Defaults))
		{
			Tags = ReadTags(Defaults);
		}
		return Tags;
	}

	static FString GetDefaultFolderPath(const TSharedPtr<FJsonObject>& Content)
	{
		TSharedPtr<FJsonObject> Defaults;
		FString Folder;
		if (TryGetObject(Content, TEXT("defaults"), Defaults) &&
			TryGetStringAny(Defaults, { TEXT("folder_path") }, Folder))
		{
			return NormalizeFolder(Folder);
		}
		return FString();
	}

	static UActorComponent* FindComponentByName(AActor* Actor, const FString& ComponentName)
	{
		const FString CleanName = ComponentName.TrimStartAndEnd();
		if (!Actor || CleanName.IsEmpty())
		{
			return nullptr;
		}
		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (!Component)
			{
				continue;
			}
			const UClass* ComponentClass = Component->GetClass();
			const FString ClassName = ComponentClass ? ComponentClass->GetName() : FString();
			if (Component->GetName().Equals(CleanName, ESearchCase::IgnoreCase) ||
				Component->GetPathName().Equals(CleanName, ESearchCase::IgnoreCase) ||
				ClassName.Equals(CleanName, ESearchCase::IgnoreCase))
			{
				return Component;
			}
		}
		return nullptr;
	}

	static FProperty* FindPropertyByNameLoose(UStruct* StructType, const FString& Segment)
	{
		if (!StructType || Segment.TrimStartAndEnd().IsEmpty())
		{
			return nullptr;
		}
		const FString Normalized = Segment.TrimStartAndEnd();
		const FString WithoutB = Normalized.StartsWith(TEXT("b")) ? Normalized.Mid(1) : Normalized;
		for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			const FString PropertyName = Property->GetName();
			const FString PropertyWithoutB = PropertyName.StartsWith(TEXT("b")) ? PropertyName.Mid(1) : PropertyName;
			if (PropertyName.Equals(Normalized, ESearchCase::IgnoreCase) ||
				PropertyWithoutB.Equals(WithoutB, ESearchCase::IgnoreCase) ||
				Property->GetAuthoredName().Equals(Normalized, ESearchCase::IgnoreCase))
			{
				return Property;
			}
		}
		return nullptr;
	}

	static bool ResolvePropertyPath(UObject* Obj, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr)
	{
		OutProperty = nullptr;
		OutValuePtr = nullptr;
		if (!Obj || PropertyPath.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}

		TArray<FString> Segments;
		PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() <= 0)
		{
			return false;
		}

		UStruct* CurrentStruct = Obj->GetClass();
		void* CurrentContainer = Obj;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FProperty* Property = FindPropertyByNameLoose(CurrentStruct, Segments[Index]);
			if (!Property)
			{
				return false;
			}

			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
			if (Index == Segments.Num() - 1)
			{
				OutProperty = Property;
				OutValuePtr = ValuePtr;
				return true;
			}

			FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (StructProperty)
			{
				CurrentStruct = StructProperty->Struct;
				CurrentContainer = ValuePtr;
				continue;
			}
			FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
			if (ObjectProperty)
			{
				UObject* ObjectValue = ObjectProperty->GetObjectPropertyValue(ValuePtr);
				if (!ObjectValue)
				{
					return false;
				}
				CurrentStruct = ObjectValue->GetClass();
				CurrentContainer = ObjectValue;
				continue;
			}
			return false;
		}
		return false;
	}

	static bool ResolveObjectPropertyPath(UObject* Obj, const FString& PropertyPath, UObject*& OutTargetObject, FString& OutResolvedPropertyPath, FProperty*& OutProperty, void*& OutValuePtr)
	{
		OutTargetObject = Obj;
		OutResolvedPropertyPath = PropertyPath;
		OutProperty = nullptr;
		OutValuePtr = nullptr;
		if (!Obj || PropertyPath.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}

		if (AActor* Actor = Cast<AActor>(Obj))
		{
			TArray<FString> Segments;
			PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
			if (Segments.Num() > 1)
			{
				if (UActorComponent* Component = FindComponentByName(Actor, Segments[0]))
				{
					FString Remainder = Segments[1];
					for (int32 Index = 2; Index < Segments.Num(); ++Index)
					{
						Remainder += TEXT(".");
						Remainder += Segments[Index];
					}
					if (ResolvePropertyPath(Component, Remainder, OutProperty, OutValuePtr) && OutProperty && OutValuePtr)
					{
						OutTargetObject = Component;
						OutResolvedPropertyPath = Remainder;
						return true;
					}
					return false;
				}
			}
		}

		return ResolvePropertyPath(Obj, PropertyPath, OutProperty, OutValuePtr) && OutProperty && OutValuePtr;
	}

	enum class EPrimitivePhysicsVirtualProperty : uint8
	{
		None,
		SimulatePhysics,
		EnableGravity,
		UseCCD,
		MassOverrideKg,
		LinearDamping,
		AngularDamping,
		CollisionEnabled
	};
	static bool ResolvePrimitivePhysicsVirtualPropertyTarget(UObject* Obj, const FString& PropertyPath, UPrimitiveComponent*& OutPrimitiveComponent, FString& OutResolvedPropertyPath, EPrimitivePhysicsVirtualProperty& OutKind);
	static bool ExportPrimitivePhysicsVirtualPropertyText(UPrimitiveComponent* PrimitiveComponent, EPrimitivePhysicsVirtualProperty Kind, FString& OutText);

	static bool ExportPropertyText(UObject* Obj, const FString& PropertyPath, FString& OutText, FString& OutClass)
	{
		UPrimitiveComponent* PrimitiveComponent = nullptr;
		FString VirtualResolvedPropertyPath;
		EPrimitivePhysicsVirtualProperty Kind = EPrimitivePhysicsVirtualProperty::None;
		if (ResolvePrimitivePhysicsVirtualPropertyTarget(Obj, PropertyPath, PrimitiveComponent, VirtualResolvedPropertyPath, Kind) &&
			ExportPrimitivePhysicsVirtualPropertyText(PrimitiveComponent, Kind, OutText))
		{
			OutClass = TEXT("PrimitivePhysicsVirtualProperty");
			return true;
		}
		UObject* TargetObject = nullptr;
		FString ResolvedPropertyPath;
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolveObjectPropertyPath(Obj, PropertyPath, TargetObject, ResolvedPropertyPath, Property, ValuePtr) || !TargetObject || !Property || !ValuePtr)
		{
			return false;
		}
		Property->ExportTextItem_Direct(OutText, ValuePtr, nullptr, TargetObject, PPF_None);
		OutClass = Property->GetClass() ? Property->GetClass()->GetName() : FString();
		return true;
	}

	static bool ReadLinearColorValue(const TSharedPtr<FJsonValue>& Value, FString& OutText)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Arr;
		if (Value->Type == EJson::Array)
		{
			Arr = Value->AsArray();
		}
		else if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			const TSharedPtr<FJsonValue> Inner = Obj.IsValid() ? Obj->TryGetField(TEXT("value")) : nullptr;
			if (Inner.IsValid() && Inner->Type == EJson::Array)
			{
				Arr = Inner->AsArray();
			}
		}
		if (Arr.Num() < 3)
		{
			return false;
		}
		const double R = Arr[0].IsValid() ? Arr[0]->AsNumber() : 0.0;
		const double G = Arr[1].IsValid() ? Arr[1]->AsNumber() : 0.0;
		const double B = Arr[2].IsValid() ? Arr[2]->AsNumber() : 0.0;
		const double A = Arr.Num() > 3 && Arr[3].IsValid() ? Arr[3]->AsNumber() : 1.0;
		OutText = FString::Printf(TEXT("(R=%g,G=%g,B=%g,A=%g)"), R, G, B, A);
		return true;
	}

	static FString JsonValueToImportText(const TSharedPtr<FJsonValue>& Value, UWorld* World)
	{
		if (!Value.IsValid())
		{
			return TEXT("None");
		}
		if (Value->Type == EJson::Boolean)
		{
			return Value->AsBool() ? TEXT("true") : TEXT("false");
		}
		if (Value->Type == EJson::Number)
		{
			return FString::SanitizeFloat(Value->AsNumber(), 6);
		}
		if (Value->Type == EJson::String)
		{
			return Value->AsString();
		}
		if (Value->Type == EJson::Array)
		{
			FVector Vector;
			if (TryReadVectorValue(Value, Vector))
			{
				return FString::Printf(TEXT("(X=%g,Y=%g,Z=%g)"), Vector.X, Vector.Y, Vector.Z);
			}
			return JsonValueToString(Value);
		}
		if (Value->Type != EJson::Object)
		{
			return JsonValueToString(Value);
		}

		const TSharedPtr<FJsonObject> Obj = Value->AsObject();
		if (!Obj.IsValid())
		{
			return TEXT("None");
		}

		FString ValueText;
		if (Obj->TryGetStringField(TEXT("value_text"), ValueText))
		{
			return ValueText;
		}
		if (Obj->TryGetStringField(TEXT("asset"), ValueText) || Obj->TryGetStringField(TEXT("object"), ValueText) ||
			Obj->TryGetStringField(TEXT("class"), ValueText))
		{
			return ValueText;
		}
		FString ActorId;
		if (Obj->TryGetStringField(TEXT("actor_id"), ActorId) && World)
		{
			if (AActor* Actor = FindActorByNameLabelOrManagedId(World, ActorId))
			{
				return Actor->GetPathName();
			}
			return ActorId;
		}
		const TSharedPtr<FJsonValue> InnerValue = Obj->TryGetField(TEXT("value"));
		FString TypeText;
		Obj->TryGetStringField(TEXT("type"), TypeText);
		if (InnerValue.IsValid())
		{
			if (TypeText.Equals(TEXT("LinearColor"), ESearchCase::IgnoreCase) ||
				TypeText.Equals(TEXT("Color"), ESearchCase::IgnoreCase))
			{
				FString ColorText;
				if (ReadLinearColorValue(InnerValue, ColorText))
				{
					return ColorText;
				}
			}
			if (TypeText.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase))
			{
				FRotator Rotator;
				if (TryReadRotatorValue(InnerValue, Rotator))
				{
					return FString::Printf(TEXT("(Pitch=%g,Yaw=%g,Roll=%g)"), Rotator.Pitch, Rotator.Yaw, Rotator.Roll);
				}
			}
			return JsonValueToImportText(InnerValue, World);
		}
		return JsonValueToString(Value);
	}

	static TSharedPtr<FJsonObject> MakePropertyResult(const FString& Target, const FString& PropertyPath, const FString& Status, const FString& Requested, const FString& ReadBack, const FString& Error)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("target"), Target);
		Result->SetStringField(TEXT("property_path"), PropertyPath);
		Result->SetStringField(TEXT("status"), Status);
		Result->SetStringField(TEXT("requested_value_text"), Requested);
		Result->SetStringField(TEXT("property_value_read_back"), ReadBack);
		Result->SetBoolField(TEXT("property_import_verified"), Error.IsEmpty() && !ReadBack.IsEmpty());
		if (!Error.IsEmpty())
		{
			Result->SetStringField(TEXT("error"), Error);
		}
		return Result;
	}

	static FString JoinPathSegments(const TArray<FString>& Segments, int32 StartIndex)
	{
		FString Result;
		for (int32 Index = StartIndex; Index < Segments.Num(); ++Index)
		{
			if (!Result.IsEmpty())
			{
				Result += TEXT(".");
			}
			Result += Segments[Index];
		}
		return Result;
	}

	static EPrimitivePhysicsVirtualProperty GetPrimitivePhysicsVirtualPropertyKind(const FString& PropertyPath)
	{
		FString Normalized = PropertyPath.TrimStartAndEnd();
		if (Normalized.StartsWith(TEXT("BodyInstance."), ESearchCase::IgnoreCase))
		{
			Normalized = Normalized.Mid(13);
		}
		if (Normalized.Equals(TEXT("bSimulatePhysics"), ESearchCase::IgnoreCase) ||
			Normalized.Equals(TEXT("SimulatePhysics"), ESearchCase::IgnoreCase) ||
			Normalized.Equals(TEXT("bInstanceSimulatePhysics"), ESearchCase::IgnoreCase))
		{
			return EPrimitivePhysicsVirtualProperty::SimulatePhysics;
		}
		if (Normalized.Equals(TEXT("bEnableGravity"), ESearchCase::IgnoreCase) ||
			Normalized.Equals(TEXT("EnableGravity"), ESearchCase::IgnoreCase))
		{
			return EPrimitivePhysicsVirtualProperty::EnableGravity;
		}
		if (Normalized.Equals(TEXT("bUseCCD"), ESearchCase::IgnoreCase) ||
			Normalized.Equals(TEXT("UseCCD"), ESearchCase::IgnoreCase))
		{
			return EPrimitivePhysicsVirtualProperty::UseCCD;
		}
		if (Normalized.Equals(TEXT("MassInKgOverride"), ESearchCase::IgnoreCase) ||
			Normalized.Equals(TEXT("MassOverrideKg"), ESearchCase::IgnoreCase) ||
			Normalized.Equals(TEXT("MassOverrideInKg"), ESearchCase::IgnoreCase))
		{
			return EPrimitivePhysicsVirtualProperty::MassOverrideKg;
		}
		if (Normalized.Equals(TEXT("LinearDamping"), ESearchCase::IgnoreCase))
		{
			return EPrimitivePhysicsVirtualProperty::LinearDamping;
		}
		if (Normalized.Equals(TEXT("AngularDamping"), ESearchCase::IgnoreCase))
		{
			return EPrimitivePhysicsVirtualProperty::AngularDamping;
		}
		if (Normalized.Equals(TEXT("CollisionEnabled"), ESearchCase::IgnoreCase))
		{
			return EPrimitivePhysicsVirtualProperty::CollisionEnabled;
		}
		return EPrimitivePhysicsVirtualProperty::None;
	}

	static bool ResolvePrimitivePhysicsVirtualPropertyTarget(UObject* Obj, const FString& PropertyPath, UPrimitiveComponent*& OutPrimitiveComponent, FString& OutResolvedPropertyPath, EPrimitivePhysicsVirtualProperty& OutKind)
	{
		OutPrimitiveComponent = nullptr;
		OutResolvedPropertyPath.Reset();
		OutKind = EPrimitivePhysicsVirtualProperty::None;
		if (!Obj || PropertyPath.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}

		if (UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(Obj))
		{
			const EPrimitivePhysicsVirtualProperty Kind = GetPrimitivePhysicsVirtualPropertyKind(PropertyPath);
			if (Kind != EPrimitivePhysicsVirtualProperty::None)
			{
				OutPrimitiveComponent = PrimitiveComponent;
				OutResolvedPropertyPath = PropertyPath.TrimStartAndEnd();
				OutKind = Kind;
				return true;
			}
		}

		if (AActor* Actor = Cast<AActor>(Obj))
		{
			TArray<FString> Segments;
			PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
			if (Segments.Num() > 1)
			{
				if (UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(FindComponentByName(Actor, Segments[0])))
				{
					const FString Remainder = JoinPathSegments(Segments, 1);
					const EPrimitivePhysicsVirtualProperty Kind = GetPrimitivePhysicsVirtualPropertyKind(Remainder);
					if (Kind != EPrimitivePhysicsVirtualProperty::None)
					{
						OutPrimitiveComponent = PrimitiveComponent;
						OutResolvedPropertyPath = Remainder;
						OutKind = Kind;
						return true;
					}
				}
			}
		}

		return false;
	}

	static bool TryReadBoolImportValue(const TSharedPtr<FJsonValue>& Value, bool& OutValue)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Boolean)
		{
			OutValue = Value->AsBool();
			return true;
		}
		if (Value->Type == EJson::Number)
		{
			OutValue = !FMath::IsNearlyZero(Value->AsNumber());
			return true;
		}
		if (Value->Type == EJson::String)
		{
			const FString Text = Value->AsString().TrimStartAndEnd();
			if (Text.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("1"), ESearchCase::IgnoreCase))
			{
				OutValue = true;
				return true;
			}
			if (Text.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("0"), ESearchCase::IgnoreCase))
			{
				OutValue = false;
				return true;
			}
		}
		return false;
	}

	static bool TryReadNumberImportValue(const TSharedPtr<FJsonValue>& Value, double& OutValue)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Number)
		{
			OutValue = Value->AsNumber();
			return true;
		}
		if (Value->Type == EJson::String)
		{
			return LexTryParseString(OutValue, *Value->AsString().TrimStartAndEnd());
		}
		return false;
	}

	static bool TryParseCollisionEnabledValue(const FString& Text, ECollisionEnabled::Type& OutValue)
	{
		const FString Clean = Text.TrimStartAndEnd();
		if (Clean.Equals(TEXT("NoCollision"), ESearchCase::IgnoreCase))
		{
			OutValue = ECollisionEnabled::NoCollision;
			return true;
		}
		if (Clean.Equals(TEXT("QueryOnly"), ESearchCase::IgnoreCase))
		{
			OutValue = ECollisionEnabled::QueryOnly;
			return true;
		}
		if (Clean.Equals(TEXT("PhysicsOnly"), ESearchCase::IgnoreCase))
		{
			OutValue = ECollisionEnabled::PhysicsOnly;
			return true;
		}
		if (Clean.Equals(TEXT("QueryAndPhysics"), ESearchCase::IgnoreCase))
		{
			OutValue = ECollisionEnabled::QueryAndPhysics;
			return true;
		}
		if (Clean.Equals(TEXT("ProbeOnly"), ESearchCase::IgnoreCase))
		{
			OutValue = ECollisionEnabled::ProbeOnly;
			return true;
		}
		if (Clean.Equals(TEXT("QueryAndProbe"), ESearchCase::IgnoreCase))
		{
			OutValue = ECollisionEnabled::QueryAndProbe;
			return true;
		}
		return false;
	}

	static FString CollisionEnabledToString(ECollisionEnabled::Type Value)
	{
		switch (Value)
		{
		case ECollisionEnabled::NoCollision:
			return TEXT("NoCollision");
		case ECollisionEnabled::QueryOnly:
			return TEXT("QueryOnly");
		case ECollisionEnabled::PhysicsOnly:
			return TEXT("PhysicsOnly");
		case ECollisionEnabled::QueryAndPhysics:
			return TEXT("QueryAndPhysics");
		case ECollisionEnabled::ProbeOnly:
			return TEXT("ProbeOnly");
		case ECollisionEnabled::QueryAndProbe:
			return TEXT("QueryAndProbe");
		default:
			return FString::FromInt(static_cast<int32>(Value));
		}
	}

	static bool ExportPrimitivePhysicsVirtualPropertyText(UPrimitiveComponent* PrimitiveComponent, EPrimitivePhysicsVirtualProperty Kind, FString& OutText)
	{
		if (!PrimitiveComponent)
		{
			return false;
		}
		switch (Kind)
		{
		case EPrimitivePhysicsVirtualProperty::SimulatePhysics:
			OutText = PrimitiveComponent->IsSimulatingPhysics() ? TEXT("true") : TEXT("false");
			return true;
		case EPrimitivePhysicsVirtualProperty::EnableGravity:
			OutText = PrimitiveComponent->IsGravityEnabled() ? TEXT("true") : TEXT("false");
			return true;
		case EPrimitivePhysicsVirtualProperty::UseCCD:
			OutText = PrimitiveComponent->BodyInstance.bUseCCD ? TEXT("true") : TEXT("false");
			return true;
		case EPrimitivePhysicsVirtualProperty::MassOverrideKg:
			OutText = FString::SanitizeFloat(PrimitiveComponent->BodyInstance.GetMassOverride(), 6);
			return true;
		case EPrimitivePhysicsVirtualProperty::LinearDamping:
			OutText = FString::SanitizeFloat(PrimitiveComponent->GetLinearDamping(), 6);
			return true;
		case EPrimitivePhysicsVirtualProperty::AngularDamping:
			OutText = FString::SanitizeFloat(PrimitiveComponent->GetAngularDamping(), 6);
			return true;
		case EPrimitivePhysicsVirtualProperty::CollisionEnabled:
			OutText = CollisionEnabledToString(PrimitiveComponent->GetCollisionEnabled());
			return true;
		default:
			return false;
		}
	}

	static FString SanitizeNameImportText(const FString& ImportText);
	static void RefreshObjectAfterPropertyImport(UObject* TargetObject, const FString& ResolvedPropertyPath, const FString& ImportText, TSharedPtr<FJsonObject>& InOutResult);
	static bool SetSkeletalMeshComponentAssetFromJson(UObject* TargetObject, const FString& PropertyPath, const FString& ResolvedPropertyPath, FProperty* Property, void* ValuePtr, const FString& ImportText, TSharedPtr<FJsonObject>& OutResult, FString& OutError);

	static bool SetPrimitivePhysicsVirtualPropertyFromJson(UObject* Obj, const FString& PropertyPath, const TSharedPtr<FJsonValue>& Value, UWorld* World, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		UPrimitiveComponent* PrimitiveComponent = nullptr;
		FString ResolvedPropertyPath;
		EPrimitivePhysicsVirtualProperty Kind = EPrimitivePhysicsVirtualProperty::None;
		if (!ResolvePrimitivePhysicsVirtualPropertyTarget(Obj, PropertyPath, PrimitiveComponent, ResolvedPropertyPath, Kind))
		{
			return false;
		}

		const FString ImportText = JsonValueToImportText(Value, World);
		PrimitiveComponent->Modify();
		bool bApplied = false;
		switch (Kind)
		{
		case EPrimitivePhysicsVirtualProperty::SimulatePhysics:
		{
			bool bValue = false;
			if (!TryReadBoolImportValue(Value, bValue))
			{
				OutError = TEXT("invalid_bool_value");
				break;
			}
			PrimitiveComponent->SetSimulatePhysics(bValue);
			bApplied = true;
			break;
		}
		case EPrimitivePhysicsVirtualProperty::EnableGravity:
		{
			bool bValue = false;
			if (!TryReadBoolImportValue(Value, bValue))
			{
				OutError = TEXT("invalid_bool_value");
				break;
			}
			PrimitiveComponent->SetEnableGravity(bValue);
			bApplied = true;
			break;
		}
		case EPrimitivePhysicsVirtualProperty::UseCCD:
		{
			bool bValue = false;
			if (!TryReadBoolImportValue(Value, bValue))
			{
				OutError = TEXT("invalid_bool_value");
				break;
			}
			PrimitiveComponent->SetAllUseCCD(bValue);
			bApplied = true;
			break;
		}
		case EPrimitivePhysicsVirtualProperty::MassOverrideKg:
		{
			double NumberValue = 0.0;
			if (!TryReadNumberImportValue(Value, NumberValue))
			{
				OutError = TEXT("invalid_number_value");
				break;
			}
			PrimitiveComponent->SetMassOverrideInKg(NAME_None, static_cast<float>(NumberValue), NumberValue > 0.0);
			bApplied = true;
			break;
		}
		case EPrimitivePhysicsVirtualProperty::LinearDamping:
		{
			double NumberValue = 0.0;
			if (!TryReadNumberImportValue(Value, NumberValue))
			{
				OutError = TEXT("invalid_number_value");
				break;
			}
			PrimitiveComponent->SetLinearDamping(static_cast<float>(NumberValue));
			bApplied = true;
			break;
		}
		case EPrimitivePhysicsVirtualProperty::AngularDamping:
		{
			double NumberValue = 0.0;
			if (!TryReadNumberImportValue(Value, NumberValue))
			{
				OutError = TEXT("invalid_number_value");
				break;
			}
			PrimitiveComponent->SetAngularDamping(static_cast<float>(NumberValue));
			bApplied = true;
			break;
		}
		case EPrimitivePhysicsVirtualProperty::CollisionEnabled:
		{
			ECollisionEnabled::Type CollisionEnabled = ECollisionEnabled::NoCollision;
			if (!TryParseCollisionEnabledValue(SanitizeNameImportText(ImportText), CollisionEnabled))
			{
				OutError = TEXT("invalid_collision_enabled");
				break;
			}
			PrimitiveComponent->SetCollisionEnabled(CollisionEnabled);
			bApplied = true;
			break;
		}
		default:
			OutError = TEXT("unsupported_primitive_physics_virtual_property");
			break;
		}

		if (!bApplied)
		{
			OutResult = MakePropertyResult(PrimitiveComponent->GetPathName(), PropertyPath, TEXT("import_failed"), ImportText, TEXT(""), OutError);
			return true;
		}

		PrimitiveComponent->PostEditChange();
		PrimitiveComponent->MarkPackageDirty();
		FString ReadBack;
		ExportPrimitivePhysicsVirtualPropertyText(PrimitiveComponent, Kind, ReadBack);
		OutResult = MakePropertyResult(PrimitiveComponent->GetPathName(), PropertyPath, TEXT("imported_and_read_back"), ImportText, ReadBack, TEXT(""));
		OutResult->SetStringField(TEXT("resolved_property_path"), ResolvedPropertyPath);
		OutResult->SetStringField(TEXT("property_assignment_method"), TEXT("primitive_physics_setter"));
		OutResult->SetBoolField(TEXT("primitive_physics_setter_applied"), true);
		RefreshObjectAfterPropertyImport(PrimitiveComponent, ResolvedPropertyPath, ImportText, OutResult);
		return true;
	}

	static FString SanitizeNameImportText(const FString& ImportText)
	{
		FString Clean = ImportText.TrimStartAndEnd();
		if (Clean.StartsWith(TEXT("Name="), ESearchCase::IgnoreCase))
		{
			Clean = Clean.Mid(5).TrimStartAndEnd();
		}
		Clean.TrimQuotesInline();
		return Clean;
	}

	static FString NormalizeObjectPathImportText(const FString& ImportText)
	{
		FString Clean = SanitizeNameImportText(ImportText);
		if (Clean.StartsWith(TEXT("Object="), ESearchCase::IgnoreCase))
		{
			Clean = Clean.Mid(7).TrimStartAndEnd();
		}
		Clean.TrimQuotesInline();

		int32 FirstQuote = INDEX_NONE;
		int32 LastQuote = INDEX_NONE;
		if (Clean.FindChar(TEXT('\''), FirstQuote) && Clean.FindLastChar(TEXT('\''), LastQuote) && LastQuote > FirstQuote)
		{
			Clean = Clean.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1).TrimStartAndEnd();
		}

		if (Clean.StartsWith(TEXT("/")) && !Clean.Contains(TEXT(".")))
		{
			const FString AssetName = FPackageName::GetLongPackageAssetName(Clean);
			if (!AssetName.IsEmpty())
			{
				Clean = FString::Printf(TEXT("%s.%s"), *Clean, *AssetName);
			}
		}
		return Clean;
	}

	static bool IsSkeletalMeshComponentAssetPropertyPath(const FString& ResolvedPropertyPath)
	{
		const FString Normalized = ResolvedPropertyPath.TrimStartAndEnd();
		return Normalized.Equals(TEXT("SkeletalMesh"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("SkeletalMeshAsset"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("SkinnedAsset"), ESearchCase::IgnoreCase);
	}

	static bool SetSkeletalMeshComponentAssetFromJson(UObject* TargetObject, const FString& PropertyPath, const FString& ResolvedPropertyPath, FProperty* Property, void* ValuePtr, const FString& ImportText, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		USkeletalMeshComponent* SkeletalMeshComponent = Cast<USkeletalMeshComponent>(TargetObject);
		if (!SkeletalMeshComponent || !IsSkeletalMeshComponentAssetPropertyPath(ResolvedPropertyPath))
		{
			return false;
		}

		const FString MeshObjectPath = NormalizeObjectPathImportText(ImportText);
		USkeletalMesh* Mesh = nullptr;
		if (!MeshObjectPath.IsEmpty()
			&& !MeshObjectPath.Equals(TEXT("None"), ESearchCase::IgnoreCase)
			&& !MeshObjectPath.Equals(TEXT("null"), ESearchCase::IgnoreCase))
		{
			Mesh = Cast<USkeletalMesh>(StaticLoadObject(USkeletalMesh::StaticClass(), nullptr, *MeshObjectPath));
			if (!Mesh)
			{
				OutError = TEXT("skeletal_mesh_asset_not_found");
				OutResult = MakePropertyResult(TargetObject->GetPathName(), PropertyPath, TEXT("import_failed"), ImportText, TEXT(""), OutError);
				OutResult->SetStringField(TEXT("resolved_property_path"), ResolvedPropertyPath);
				OutResult->SetStringField(TEXT("property_assignment_method"), TEXT("skeletal_mesh_component_setter"));
				OutResult->SetStringField(TEXT("normalized_object_path"), MeshObjectPath);
				return true;
			}
		}

		SkeletalMeshComponent->Modify();
		SkeletalMeshComponent->SetSkeletalMesh(Mesh, true);
		SkeletalMeshComponent->UpdateBounds();
		SkeletalMeshComponent->MarkRenderTransformDirty();
		SkeletalMeshComponent->MarkRenderStateDirty();
		SkeletalMeshComponent->PostEditChange();
		SkeletalMeshComponent->MarkPackageDirty();

		FString Exported;
		if (Property && ValuePtr)
		{
			Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, TargetObject, PPF_None);
		}
		if (Exported.IsEmpty())
		{
			Exported = Mesh ? Mesh->GetPathName() : TEXT("None");
		}

		OutResult = MakePropertyResult(TargetObject->GetPathName(), PropertyPath, TEXT("imported_and_read_back"), ImportText, Exported, TEXT(""));
		OutResult->SetStringField(TEXT("resolved_property_path"), ResolvedPropertyPath);
		OutResult->SetStringField(TEXT("property_assignment_method"), TEXT("skeletal_mesh_component_setter"));
		OutResult->SetBoolField(TEXT("skeletal_mesh_setter_applied"), true);
		OutResult->SetStringField(TEXT("normalized_object_path"), MeshObjectPath);
		OutResult->SetStringField(TEXT("skeletal_mesh_path_read_back"), Mesh ? Mesh->GetPathName() : TEXT("None"));
		RefreshObjectAfterPropertyImport(TargetObject, ResolvedPropertyPath, ImportText, OutResult);
		Exported.Reset();
		if (Property && ValuePtr)
		{
			Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, TargetObject, PPF_None);
			if (!Exported.IsEmpty())
			{
				OutResult->SetStringField(TEXT("property_value_read_back"), Exported);
			}
		}
		return true;
	}

	static bool PropertyPathTouchesPrimitiveCollisionState(const FString& PropertyPath)
	{
		const FString Normalized = PropertyPath.TrimStartAndEnd();
		return Normalized.StartsWith(TEXT("BodyInstance."), ESearchCase::IgnoreCase)
			|| Normalized.Contains(TEXT("Collision"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("CanCharacterStepUpOn"), ESearchCase::IgnoreCase);
	}

	static bool IsPrimitiveCollisionProfilePropertyPath(const FString& PropertyPath)
	{
		const FString Normalized = PropertyPath.TrimStartAndEnd();
		return Normalized.Equals(TEXT("BodyInstance.CollisionProfileName"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("CollisionProfileName"), ESearchCase::IgnoreCase);
	}

	static void RefreshObjectAfterPropertyImport(UObject* TargetObject, const FString& ResolvedPropertyPath, const FString& ImportText, TSharedPtr<FJsonObject>& InOutResult)
	{
		if (!TargetObject || !InOutResult.IsValid())
		{
			return;
		}

		UActorComponent* Component = Cast<UActorComponent>(TargetObject);
		UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(TargetObject);
		if (!Component)
		{
			return;
		}

		bool bRenderStateRefreshed = false;
		bool bPrimitiveCollisionStateRefreshed = false;
		bool bCollisionProfileSetterApplied = false;

		const FString NormalizedPropertyPath = ResolvedPropertyPath.TrimStartAndEnd();
		if (PrimitiveComponent)
		{
			if (IsPrimitiveCollisionProfilePropertyPath(NormalizedPropertyPath))
			{
				const FString ProfileName = SanitizeNameImportText(ImportText);
				if (!ProfileName.IsEmpty())
				{
					PrimitiveComponent->SetCollisionProfileName(FName(*ProfileName));
					if (ProfileName.Equals(TEXT("NoCollision"), ESearchCase::IgnoreCase))
					{
						PrimitiveComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
						InOutResult->SetBoolField(TEXT("collision_enabled_no_collision_applied"), true);
					}
					bCollisionProfileSetterApplied = true;
				}
			}

			if (PropertyPathTouchesPrimitiveCollisionState(NormalizedPropertyPath))
			{
				PrimitiveComponent->RecreatePhysicsState();
				PrimitiveComponent->UpdateOverlaps();
				PrimitiveComponent->UpdateBounds();
				PrimitiveComponent->MarkRenderStateDirty();
				PrimitiveComponent->MarkRenderTransformDirty();
				bPrimitiveCollisionStateRefreshed = true;
				bRenderStateRefreshed = true;
			}
			else
			{
				PrimitiveComponent->MarkRenderStateDirty();
				bRenderStateRefreshed = true;
			}
		}
		else if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
		{
			SceneComponent->MarkRenderStateDirty();
			bRenderStateRefreshed = true;
		}

		if (bCollisionProfileSetterApplied)
		{
			InOutResult->SetBoolField(TEXT("collision_profile_setter_applied"), true);
		}
		if (bPrimitiveCollisionStateRefreshed)
		{
			InOutResult->SetBoolField(TEXT("primitive_collision_state_refreshed"), true);
		}
		if (bRenderStateRefreshed)
		{
			InOutResult->SetBoolField(TEXT("render_state_refreshed"), true);
		}
	}

	static bool SetObjectPropertyFromJson(UObject* Obj, const FString& PropertyPath, const TSharedPtr<FJsonValue>& Value, UWorld* World, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		OutResult = MakeShared<FJsonObject>();
		if (!Obj)
		{
			OutError = TEXT("property_target_null");
			OutResult = MakePropertyResult(TEXT(""), PropertyPath, TEXT("target_null"), TEXT(""), TEXT(""), OutError);
			return false;
		}
		if (SetPrimitivePhysicsVirtualPropertyFromJson(Obj, PropertyPath, Value, World, OutResult, OutError))
		{
			return OutError.IsEmpty();
		}
		UObject* TargetObject = nullptr;
		FString ResolvedPropertyPath;
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolveObjectPropertyPath(Obj, PropertyPath, TargetObject, ResolvedPropertyPath, Property, ValuePtr) || !TargetObject || !Property || !ValuePtr)
		{
			OutError = TEXT("property_not_found");
			OutResult = MakePropertyResult(Obj->GetPathName(), PropertyPath, TEXT("property_not_found"), TEXT(""), TEXT(""), OutError);
			return false;
		}

		const FString ImportText = JsonValueToImportText(Value, World);
		TargetObject->Modify();
		if (SetSkeletalMeshComponentAssetFromJson(TargetObject, PropertyPath, ResolvedPropertyPath, Property, ValuePtr, ImportText, OutResult, OutError))
		{
			return OutError.IsEmpty();
		}
		if (UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(TargetObject))
		{
			if (IsPrimitiveCollisionProfilePropertyPath(ResolvedPropertyPath))
			{
				const FString ProfileName = SanitizeNameImportText(ImportText);
				if (ProfileName.IsEmpty())
				{
					OutError = TEXT("empty_collision_profile_name");
					OutResult = MakePropertyResult(TargetObject->GetPathName(), PropertyPath, TEXT("import_failed"), ImportText, TEXT(""), OutError);
					return false;
				}

				PrimitiveComponent->SetCollisionProfileName(FName(*ProfileName));
				TargetObject->PostEditChange();
				TargetObject->MarkPackageDirty();

				FString Exported;
				Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, TargetObject, PPF_None);
				OutResult = MakePropertyResult(TargetObject->GetPathName(), PropertyPath, TEXT("imported_and_read_back"), ImportText, Exported, TEXT(""));
				OutResult->SetStringField(TEXT("resolved_property_path"), ResolvedPropertyPath);
				OutResult->SetStringField(TEXT("property_assignment_method"), TEXT("collision_profile_setter"));
				OutResult->SetBoolField(TEXT("collision_profile_setter_applied"), true);
				RefreshObjectAfterPropertyImport(TargetObject, ResolvedPropertyPath, ImportText, OutResult);
				Exported.Reset();
				Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, TargetObject, PPF_None);
				OutResult->SetStringField(TEXT("property_value_read_back"), Exported);
				return true;
			}
		}
		if (!Property->ImportText_Direct(*ImportText, ValuePtr, TargetObject, PPF_None))
		{
			OutError = TEXT("property_import_failed");
			OutResult = MakePropertyResult(TargetObject->GetPathName(), PropertyPath, TEXT("import_failed"), ImportText, TEXT(""), OutError);
			return false;
		}
		TargetObject->PostEditChange();
		TargetObject->MarkPackageDirty();

		FString Exported;
		Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, TargetObject, PPF_None);
		OutResult = MakePropertyResult(TargetObject->GetPathName(), PropertyPath, TEXT("imported_and_read_back"), ImportText, Exported, TEXT(""));
		OutResult->SetStringField(TEXT("resolved_property_path"), ResolvedPropertyPath);
		RefreshObjectAfterPropertyImport(TargetObject, ResolvedPropertyPath, ImportText, OutResult);
		Exported.Reset();
		Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, TargetObject, PPF_None);
		OutResult->SetStringField(TEXT("property_value_read_back"), Exported);
		return true;
	}

	static bool ValidatePropertyExists(UObject* Obj, const FString& PropertyPath)
	{
		UPrimitiveComponent* PrimitiveComponent = nullptr;
		FString VirtualResolvedPropertyPath;
		EPrimitivePhysicsVirtualProperty Kind = EPrimitivePhysicsVirtualProperty::None;
		if (ResolvePrimitivePhysicsVirtualPropertyTarget(Obj, PropertyPath, PrimitiveComponent, VirtualResolvedPropertyPath, Kind))
		{
			return true;
		}
		UObject* TargetObject = nullptr;
		FString ResolvedPropertyPath;
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		return ResolveObjectPropertyPath(Obj, PropertyPath, TargetObject, ResolvedPropertyPath, Property, ValuePtr) && TargetObject && Property && ValuePtr;
	}

	static bool ActorMatchesText(AActor* Actor, const FString& Query, bool bContains)
	{
		if (!Actor || Query.TrimStartAndEnd().IsEmpty())
		{
			return true;
		}
		const FString CleanQuery = Query.TrimStartAndEnd();
		const FString Label = Actor->GetActorLabel();
		const FString Name = Actor->GetName();
		const FString Path = Actor->GetPathName();
		const FString ManagedId = GetManagedId(Actor);
		if (bContains)
		{
			return Label.Contains(CleanQuery, ESearchCase::IgnoreCase) ||
				Name.Contains(CleanQuery, ESearchCase::IgnoreCase) ||
				Path.Contains(CleanQuery, ESearchCase::IgnoreCase) ||
				ManagedId.Contains(CleanQuery, ESearchCase::IgnoreCase);
		}
		return Label.Equals(CleanQuery, ESearchCase::IgnoreCase) ||
			Name.Equals(CleanQuery, ESearchCase::IgnoreCase) ||
			Path.Equals(CleanQuery, ESearchCase::IgnoreCase) ||
			ManagedId.Equals(CleanQuery, ESearchCase::IgnoreCase);
	}

	static bool TryReadBoundsSpec(const TSharedPtr<FJsonObject>& Obj, FBox& OutBounds)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		FVector Min;
		FVector Max;
		if ((TryGetVectorField(Obj, TEXT("min"), Min) || TryGetVectorField(Obj, TEXT("minimum"), Min)) &&
			(TryGetVectorField(Obj, TEXT("max"), Max) || TryGetVectorField(Obj, TEXT("maximum"), Max)))
		{
			OutBounds = FBox(Min, Max);
			return OutBounds.IsValid != 0;
		}
		FVector Center;
		FVector Extent;
		if (TryGetVectorField(Obj, TEXT("center"), Center) &&
			(TryGetVectorField(Obj, TEXT("extent"), Extent) || TryGetVectorField(Obj, TEXT("extents"), Extent) || TryGetVectorField(Obj, TEXT("half_extent"), Extent)))
		{
			OutBounds = FBox::BuildAABB(Center, Extent);
			return OutBounds.IsValid != 0;
		}
		return false;
	}

	static bool ActorMatchesBounds(AActor* Actor, const TSharedPtr<FJsonObject>& Selection)
	{
		TSharedPtr<FJsonObject> BoundsObj;
		if (!TryGetObject(Selection, TEXT("bounds"), BoundsObj))
		{
			return true;
		}
		FBox QueryBounds(ForceInit);
		if (!TryReadBoundsSpec(BoundsObj, QueryBounds) || QueryBounds.IsValid == 0)
		{
			return true;
		}
		const FBox ActorBounds = Actor ? Actor->GetComponentsBoundingBox(true) : FBox(ForceInit);
		if (ActorBounds.IsValid == 0)
		{
			return false;
		}
		FString Mode = TEXT("intersects");
		TryGetStringAny(BoundsObj, { TEXT("mode"), TEXT("bounds_mode"), TEXT("match") }, Mode);
		Mode = Mode.TrimStartAndEnd().ToLower();
		if (Mode == TEXT("contains_origin") || Mode == TEXT("contains_actor_origin"))
		{
			return QueryBounds.IsInside(Actor->GetActorLocation());
		}
		if (Mode == TEXT("contains") || Mode == TEXT("contains_actor_bounds"))
		{
			return QueryBounds.IsInside(ActorBounds.Min) && QueryBounds.IsInside(ActorBounds.Max);
		}
		if (Mode == TEXT("actor_contains") || Mode == TEXT("actor_contains_bounds"))
		{
			return ActorBounds.IsInside(QueryBounds.Min) && ActorBounds.IsInside(QueryBounds.Max);
		}
		return ActorBounds.Intersect(QueryBounds);
	}

	static bool ActorMatchesDataLayerFallback(AActor* Actor, const TArray<FString>& DataLayers)
	{
		if (!Actor || DataLayers.Num() == 0)
		{
			return true;
		}

		FString DataLayerText;
		FString PropertyClass;
		const bool bHasReflectedDataLayerText =
			ExportPropertyText(Actor, TEXT("DataLayerAssets"), DataLayerText, PropertyClass) ||
			ExportPropertyText(Actor, TEXT("DataLayers"), DataLayerText, PropertyClass);
		for (const FString& Layer : DataLayers)
		{
			const FString CleanLayer = Layer.TrimStartAndEnd();
			if (CleanLayer.IsEmpty())
			{
				continue;
			}
			const bool bMatched =
				(bHasReflectedDataLayerText && DataLayerText.Contains(CleanLayer, ESearchCase::IgnoreCase)) ||
				ActorHasTagString(Actor, CleanLayer) ||
				ActorHasTagString(Actor, FString::Printf(TEXT("DataLayer=%s"), *CleanLayer)) ||
				ActorHasTagString(Actor, FString::Printf(TEXT("DataLayer:%s"), *CleanLayer));
			if (!bMatched)
			{
				return false;
			}
		}
		return true;
	}

	static bool ActorMatchesQuery(AActor* Actor, const TSharedPtr<FJsonObject>& Selection)
	{
		if (!Actor || Actor->IsActorBeingDestroyed())
		{
			return false;
		}
		if (!Selection.IsValid())
		{
			return true;
		}

		FString Mode;
		TryGetStringAny(Selection, { TEXT("mode") }, Mode);
		Mode = Mode.TrimStartAndEnd().ToLower();
		if (Mode == TEXT("selected"))
		{
			if (!Actor->IsSelected())
			{
				return false;
			}
		}

		FString ExactText;
		if (TryGetStringAny(Selection, { TEXT("label"), TEXT("actor_label"), TEXT("name"), TEXT("actor") }, ExactText) && !ActorMatchesText(Actor, ExactText, false))
		{
			return false;
		}
		FString ContainsText;
		if (TryGetStringAny(Selection, { TEXT("label_contains"), TEXT("name_contains"), TEXT("search") }, ContainsText) && !ActorMatchesText(Actor, ContainsText, true))
		{
			return false;
		}

		FString FolderPath;
		if ((Mode == TEXT("folder") || Selection->HasField(TEXT("folder_path"))) &&
			TryGetStringAny(Selection, { TEXT("folder_path"), TEXT("folder_root") }, FolderPath))
		{
			bool bIncludeChildFolders = true;
			TryGetBoolAny(Selection, { TEXT("include_child_folders") }, bIncludeChildFolders);
			if (!FolderMatches(Actor->GetFolderPath().ToString(), FolderPath, bIncludeChildFolders))
			{
				return false;
			}
		}

		const TArray<FString> TagsAll = ReadStringArray(Selection, TEXT("tags_all"));
		for (const FString& Tag : TagsAll)
		{
			if (!ActorHasTagString(Actor, Tag))
			{
				return false;
			}
		}
		const TArray<FString> TagsAny = ReadStringArray(Selection, TEXT("tags_any"));
		if (TagsAny.Num() > 0)
		{
			bool bMatchedAny = false;
			for (const FString& Tag : TagsAny)
			{
				if (ActorHasTagString(Actor, Tag))
				{
					bMatchedAny = true;
					break;
				}
			}
			if (!bMatchedAny)
			{
				return false;
			}
		}

		TArray<FString> ActorIds = ReadStringArray(Selection, TEXT("actors"));
		if (ActorIds.Num() == 0)
		{
			ActorIds = ReadStringArray(Selection, TEXT("ids"));
		}
		if (ActorIds.Num() > 0)
		{
			bool bMatched = false;
			for (const FString& Id : ActorIds)
			{
				if (Actor->GetName().Equals(Id, ESearchCase::IgnoreCase) ||
					Actor->GetActorLabel().Equals(Id, ESearchCase::IgnoreCase) ||
					GetManagedId(Actor).Equals(Id, ESearchCase::IgnoreCase))
				{
					bMatched = true;
					break;
				}
			}
			if (!bMatched)
			{
				return false;
			}
		}

		TArray<FString> DataLayers = ReadStringArray(Selection, TEXT("data_layers"));
		FString DataLayer;
		if (TryGetStringAny(Selection, { TEXT("data_layer") }, DataLayer) && !DataLayer.TrimStartAndEnd().IsEmpty())
		{
			DataLayers.Add(DataLayer.TrimStartAndEnd());
		}
		if (!ActorMatchesDataLayerFallback(Actor, DataLayers))
		{
			return false;
		}

		TArray<FString> ClassAny = ReadStringArray(Selection, TEXT("class_any"));
		if (ClassAny.Num() == 0)
		{
			ClassAny = ReadStringArray(Selection, TEXT("classes"));
		}
		FString SingleClass;
		if (TryGetStringAny(Selection, { TEXT("class"), TEXT("class_path") }, SingleClass))
		{
			ClassAny.Add(SingleClass);
		}
		if (ClassAny.Num() > 0)
		{
			const FString ActorClass = Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString();
			bool bMatched = false;
			for (const FString& ClassText : ClassAny)
			{
				if (ActorClass.Equals(ClassText, ESearchCase::IgnoreCase) ||
					ActorClass.Contains(ClassText, ESearchCase::IgnoreCase))
				{
					bMatched = true;
					break;
				}
			}
			if (!bMatched)
			{
				return false;
			}
		}

		if (!ActorMatchesBounds(Actor, Selection))
		{
			return false;
		}

		FVector Center;
		double Radius = 0.0;
		if (Selection->TryGetNumberField(TEXT("radius"), Radius) && TryGetVectorField(Selection, TEXT("center"), Center))
		{
			if (FVector::Distance(Actor->GetActorLocation(), Center) > Radius)
			{
				return false;
			}
		}

		return true;
	}

	static TSharedPtr<FJsonObject> MakeBoundsJson(const FBox& Box)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("valid"), Box.IsValid != 0);
		if (Box.IsValid != 0)
		{
			Obj->SetObjectField(TEXT("min"), MakeVectorJson(Box.Min));
			Obj->SetObjectField(TEXT("max"), MakeVectorJson(Box.Max));
			Obj->SetObjectField(TEXT("center"), MakeVectorJson(Box.GetCenter()));
			Obj->SetObjectField(TEXT("extent"), MakeVectorJson(Box.GetExtent()));
		}
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeComponentJson(UActorComponent* Component)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Component ? Component->GetName() : FString());
		Obj->SetStringField(TEXT("path"), Component ? Component->GetPathName() : FString());
		Obj->SetStringField(TEXT("class"), (Component && Component->GetClass()) ? Component->GetClass()->GetPathName() : FString());
		Obj->SetBoolField(TEXT("registered"), Component ? Component->IsRegistered() : false);
		Obj->SetBoolField(TEXT("is_scene_component"), Cast<USceneComponent>(Component) != nullptr);
		if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
		{
			Obj->SetObjectField(TEXT("relative_location"), MakeVectorJson(SceneComponent->GetRelativeLocation()));
			Obj->SetObjectField(TEXT("relative_rotation"), MakeRotatorJson(SceneComponent->GetRelativeRotation()));
			Obj->SetObjectField(TEXT("relative_scale"), MakeVectorJson(SceneComponent->GetRelativeScale3D()));
			Obj->SetStringField(TEXT("attach_parent"), SceneComponent->GetAttachParent() ? SceneComponent->GetAttachParent()->GetName() : FString());
		}
		if (USplineComponent* SplineComponent = Cast<USplineComponent>(Component))
		{
			const int32 PointCount = SplineComponent->GetNumberOfSplinePoints();
			TArray<TSharedPtr<FJsonValue>> Points;
			Points.Reserve(PointCount);
			for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
			{
				TSharedPtr<FJsonObject> Point = MakeShared<FJsonObject>();
				Point->SetNumberField(TEXT("index"), PointIndex);
				Point->SetObjectField(TEXT("location"), MakeVectorJson(SplineComponent->GetLocationAtSplinePoint(PointIndex, ESplineCoordinateSpace::Local)));
				Point->SetObjectField(TEXT("arrive_tangent"), MakeVectorJson(SplineComponent->GetArriveTangentAtSplinePoint(PointIndex, ESplineCoordinateSpace::Local)));
				Point->SetObjectField(TEXT("leave_tangent"), MakeVectorJson(SplineComponent->GetLeaveTangentAtSplinePoint(PointIndex, ESplineCoordinateSpace::Local)));
				Point->SetStringField(TEXT("type"), UEnum::GetValueAsString(SplineComponent->GetSplinePointType(PointIndex)));
				Points.Add(MakeShared<FJsonValueObject>(Point));
			}
			Obj->SetNumberField(TEXT("spline_point_count"), PointCount);
			Obj->SetBoolField(TEXT("spline_closed_loop"), SplineComponent->IsClosedLoop());
			Obj->SetArrayField(TEXT("spline_points"), Points);
		}
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeActorResourceJson(AActor* Actor, const TSharedPtr<FJsonObject>& Projection, bool bSnapshot)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Actor)
		{
			return Obj;
		}

		Obj->SetStringField(TEXT("id"), ActorStableId(Actor));
		Obj->SetStringField(TEXT("managed_id"), GetManagedId(Actor));
		Obj->SetStringField(TEXT("name"), Actor->GetName());
		Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Obj->SetStringField(TEXT("object_path"), Actor->GetPathName());
		Obj->SetStringField(TEXT("class"), Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());
		Obj->SetStringField(TEXT("category"), TEXT("spatial"));
		Obj->SetStringField(TEXT("kind"), TEXT("actor"));
		Obj->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());
		Obj->SetObjectField(TEXT("transform"), MakeShared<FJsonObject>());
		TSharedPtr<FJsonObject> Transform = Obj->GetObjectField(TEXT("transform"));
		Transform->SetObjectField(TEXT("location"), MakeVectorJson(Actor->GetActorLocation()));
		Transform->SetObjectField(TEXT("rotation"), MakeRotatorJson(Actor->GetActorRotation()));
		Transform->SetObjectField(TEXT("scale"), MakeVectorJson(Actor->GetActorScale3D()));

		TArray<TSharedPtr<FJsonValue>> Tags;
		for (const FName& Tag : Actor->Tags)
		{
			Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		Obj->SetArrayField(TEXT("tags"), Tags);

		if (Actor->GetAttachParentActor())
		{
			TSharedPtr<FJsonObject> Attachments = MakeShared<FJsonObject>();
			Attachments->SetStringField(TEXT("parent_actor"), ActorStableId(Actor->GetAttachParentActor()));
			Obj->SetObjectField(TEXT("attachments"), Attachments);
		}

		bool bIncludeBounds = bSnapshot;
		bool bIncludeComponents = bSnapshot;
		if (Projection.IsValid())
		{
			TryGetBoolAny(Projection, { TEXT("include_bounds") }, bIncludeBounds);
			TryGetBoolAny(Projection, { TEXT("include_components") }, bIncludeComponents);
		}
		if (bIncludeBounds)
		{
			Obj->SetObjectField(TEXT("bounds"), MakeBoundsJson(Actor->GetComponentsBoundingBox(true)));
		}
		if (bIncludeComponents)
		{
			TArray<UActorComponent*> Components;
			Actor->GetComponents(Components);
			TArray<TSharedPtr<FJsonValue>> ComponentValues;
			for (UActorComponent* Component : Components)
			{
				if (Component)
				{
					ComponentValues.Add(MakeShared<FJsonValueObject>(MakeComponentJson(Component)));
				}
			}
			Obj->SetArrayField(TEXT("components"), ComponentValues);
		}

		const TArray<TSharedPtr<FJsonValue>>* PropertyPaths = nullptr;
		if (Projection.IsValid() && Projection->TryGetArrayField(TEXT("include_properties"), PropertyPaths) && PropertyPaths)
		{
			TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
			for (const TSharedPtr<FJsonValue>& Value : *PropertyPaths)
			{
				const FString Path = Value.IsValid() ? Value->AsString() : FString();
				if (Path.IsEmpty())
				{
					continue;
				}
				FString ReadBack;
				FString PropertyClass;
				if (ExportPropertyText(Actor, Path, ReadBack, PropertyClass))
				{
					TSharedPtr<FJsonObject> Property = MakeShared<FJsonObject>();
					Property->SetStringField(TEXT("value_text"), ReadBack);
					Property->SetStringField(TEXT("property_class"), PropertyClass);
					Properties->SetObjectField(Path, Property);
				}
			}
			Obj->SetObjectField(TEXT("properties"), Properties);
		}
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildWorldInfo(UWorld* World)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("map_name"), World ? World->GetMapName() : FString());
		Obj->SetStringField(TEXT("package_name"), (World && World->GetOutermost()) ? World->GetOutermost()->GetName() : FString());
		Obj->SetBoolField(TEXT("dirty"), (World && World->GetOutermost()) ? World->GetOutermost()->IsDirty() : false);
		TArray<TSharedPtr<FJsonValue>> StreamingLevels;
		if (World)
		{
			for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
			{
				if (!StreamingLevel)
				{
					continue;
				}
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("package_name"), StreamingLevel->GetWorldAssetPackageName());
				Item->SetBoolField(TEXT("loaded"), StreamingLevel->IsLevelLoaded());
				Item->SetBoolField(TEXT("visible"), StreamingLevel->IsLevelVisible());
				StreamingLevels.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		Obj->SetArrayField(TEXT("streaming_levels"), StreamingLevels);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildFeatureGateReport(const TSharedPtr<FJsonObject>& Content)
	{
		TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetBoolField(TEXT("world_partition_runtime_region_write"), false);
		Report->SetBoolField(TEXT("streaming_level_mutation"), false);
		Report->SetBoolField(TEXT("data_layer_mutation"), false);
		Report->SetBoolField(TEXT("hlod_build"), false);
		Report->SetBoolField(TEXT("lighting_build"), false);
		Report->SetBoolField(TEXT("pcg_generate"), false);
		Report->SetStringField(TEXT("policy"), TEXT("ordinary_spatial_actor_crud_remains_available; gated systems return structured feature_unavailable unless implemented by dedicated modules"));
		return Report;
	}

	static bool QueryLevelContent(UWorld* World, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutJson, bool bActorOnly, bool bSnapshot)
	{
		TSharedPtr<FJsonObject> Selection;
		if (Params.IsValid())
		{
			TryGetObject(Params, TEXT("selection"), Selection);
		}
		TSharedPtr<FJsonObject> Projection;
		if (Params.IsValid())
		{
			TryGetObject(Params, TEXT("projection"), Projection);
		}
		if (!Projection.IsValid())
		{
			Projection = MakeShared<FJsonObject>();
			bool bIncludeComponents = false;
			bool bIncludeBounds = false;
			if (Params.IsValid())
			{
				TryGetBoolAny(Params, { TEXT("include_components") }, bIncludeComponents);
				TryGetBoolAny(Params, { TEXT("include_bounds") }, bIncludeBounds);
			}
			Projection->SetBoolField(TEXT("include_components"), bIncludeComponents);
			Projection->SetBoolField(TEXT("include_bounds"), bIncludeBounds);
		}

		TArray<TSharedPtr<FJsonValue>> Resources;
		int32 Visited = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsActorBeingDestroyed())
			{
				continue;
			}
			++Visited;
			if (!ActorMatchesQuery(Actor, Selection))
			{
				continue;
			}
			Resources.Add(MakeShared<FJsonValueObject>(MakeActorResourceJson(Actor, Projection, bSnapshot)));
		}

		OutJson = MakeShared<FJsonObject>();
		OutJson->SetStringField(TEXT("schema"), bSnapshot ? SnapshotSchema : LevelContentSchema);
		OutJson->SetStringField(TEXT("query_schema"), QuerySchema);
		OutJson->SetBoolField(TEXT("actor_only"), bActorOnly);
		OutJson->SetObjectField(TEXT("world"), BuildWorldInfo(World));
		OutJson->SetNumberField(TEXT("visited_actor_count"), Visited);
		OutJson->SetNumberField(TEXT("resource_count"), Resources.Num());
		OutJson->SetArrayField(TEXT("resources"), Resources);
		OutJson->SetObjectField(TEXT("organization"), MakeShared<FJsonObject>());
		OutJson->SetObjectField(TEXT("feature_gates"), BuildFeatureGateReport(nullptr));
		return true;
	}

	static bool ValidateAttachmentGraph(const TArray<TSharedPtr<FJsonObject>>& Resources, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		TMap<FString, FString> ParentById;
		for (const TSharedPtr<FJsonObject>& Resource : Resources)
		{
			const FString Id = GetResourceId(Resource);
			if (Id.IsEmpty())
			{
				continue;
			}
			TSharedPtr<FJsonObject> Attachments;
			if (TryGetObject(Resource, TEXT("attachments"), Attachments))
			{
				FString ParentActor;
				if (TryGetStringAny(Attachments, { TEXT("parent_actor"), TEXT("parent_id") }, ParentActor) && !ParentActor.TrimStartAndEnd().IsEmpty())
				{
					ParentById.Add(Id, ParentActor.TrimStartAndEnd());
				}
			}
		}

		for (const TPair<FString, FString>& Pair : ParentById)
		{
			TSet<FString> Seen;
			FString Cursor = Pair.Key;
			while (ParentById.Contains(Cursor))
			{
				if (Seen.Contains(Cursor))
				{
					AddIssue(Issues, TEXT("error"), TEXT("attachment_cycle"), TEXT("resources.attachments"), FString::Printf(TEXT("Attachment cycle includes %s"), *Cursor));
					return false;
				}
				Seen.Add(Cursor);
				Cursor = ParentById[Cursor];
			}
		}
		return true;
	}

	static void ValidateOrganizationAndTasks(UWorld* World, const TSharedPtr<FJsonObject>& Content, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		TSharedPtr<FJsonObject> Scope;
		if (Content.IsValid())
		{
			TryGetObject(Content, TEXT("scope"), Scope);
		}
		if (Scope.IsValid())
		{
			TSharedPtr<FJsonObject> WorldPartition;
			if (TryGetObject(Scope, TEXT("world_partition"), WorldPartition))
			{
				bool bAllowUnloadedChanges = false;
				TryGetBoolAny(WorldPartition, { TEXT("allow_unloaded_actor_changes") }, bAllowUnloadedChanges);
				if (!bAllowUnloadedChanges && WorldPartition->HasField(TEXT("load_region")))
				{
					AddIssue(Issues, TEXT("warning"), TEXT("world_partition_region_report_only"), TEXT("scope.world_partition"), TEXT("World Partition load region is validated as report-only by this command"));
				}
			}
			TSharedPtr<FJsonObject> Streaming;
			if (TryGetObject(Scope, TEXT("streaming"), Streaming))
			{
				const TArray<FString> Required = ReadStringArray(Streaming, TEXT("required_loaded_levels"));
				for (const FString& RequiredLevel : Required)
				{
					bool bFoundLoaded = false;
					if (World)
					{
						for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
						{
							if (StreamingLevel && StreamingLevel->GetWorldAssetPackageName().Contains(RequiredLevel) && StreamingLevel->IsLevelLoaded())
							{
								bFoundLoaded = true;
								break;
							}
						}
					}
					if (!bFoundLoaded)
					{
						AddIssue(Issues, TEXT("error"), TEXT("streaming_level_not_loaded"), TEXT("scope.streaming.required_loaded_levels"), RequiredLevel);
					}
				}
			}
		}

		TSharedPtr<FJsonObject> Organization;
		if (TryGetObject(Content, TEXT("organization"), Organization))
		{
			if (Organization->HasField(TEXT("hlod")) || Organization->HasField(TEXT("hlod_layers")))
			{
				AddIssue(Issues, TEXT("warning"), TEXT("feature_unavailable_hlod_assignment_report_only"), TEXT("organization.hlod"), TEXT("HLOD assignment is reported and validated structurally; build is gated"));
			}
			if (Organization->HasField(TEXT("data_layers")))
			{
				AddIssue(Issues, TEXT("warning"), TEXT("feature_unavailable_data_layer_mutation_report_only"), TEXT("organization.data_layers"), TEXT("Data Layer mutation is gated; actor CRUD still applies"));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Tasks = nullptr;
		if (Content.IsValid() && TryGetArray(Content, TEXT("tasks"), Tasks) && Tasks)
		{
			for (int32 Index = 0; Index < Tasks->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Task = (*Tasks)[Index].IsValid() ? (*Tasks)[Index]->AsObject() : nullptr;
				if (!Task.IsValid())
				{
					AddIssue(Issues, TEXT("error"), TEXT("task_invalid"), FString::Printf(TEXT("tasks[%d]"), Index), TEXT("Task must be an object"));
					continue;
				}
				FString Type;
				TryGetStringAny(Task, { TEXT("type"), TEXT("kind"), TEXT("task") }, Type);
				Type = Type.ToLower();
				double TimeoutSeconds = 0.0;
				if (Task->TryGetNumberField(TEXT("timeout_seconds"), TimeoutSeconds) && TimeoutSeconds < 0.0)
				{
					AddIssue(Issues, TEXT("error"), TEXT("task_invalid_timeout"), FString::Printf(TEXT("tasks[%d].timeout_seconds"), Index), TEXT("timeout_seconds must be >= 0"));
				}
				if (Type == TEXT("navmesh_build") || Type == TEXT("cleanup_empty_folders") || Type == TEXT("report") || Type == TEXT("validate"))
				{
					continue;
				}
				if (Type == TEXT("hlod_build") || Type == TEXT("build_hlod") || Type == TEXT("lighting_build") || Type == TEXT("reflection_capture_update") || Type == TEXT("pcg_generate") ||
					Type == TEXT("load_streaming_level_for_edit") || Type == TEXT("unload_streaming_level_after_apply"))
				{
					bool bRequired = false;
					TryGetBoolAny(Task, { TEXT("required") }, bRequired);
					AddIssue(Issues, bRequired ? TEXT("error") : TEXT("warning"), TEXT("feature_unavailable_task_report_only"), FString::Printf(TEXT("tasks[%d]"), Index), Type);
					continue;
				}
				AddIssue(Issues, TEXT("error"), TEXT("unsupported_task_type"), FString::Printf(TEXT("tasks[%d]"), Index), Type);
			}
		}
	}

	static bool ValidateLevelContent(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, bool bActorOnly, TSharedPtr<FJsonObject>& OutData)
	{
		TArray<TSharedPtr<FJsonValue>> Issues;
		TArray<TSharedPtr<FJsonValue>> ResourcesJson;
		TArray<TSharedPtr<FJsonObject>> Resources;
		CollectResourceObjects(Content, Resources);

		FString Schema;
		if (Content.IsValid() && Content->TryGetStringField(TEXT("schema"), Schema) && !Schema.Equals(LevelContentSchema, ESearchCase::IgnoreCase))
		{
			AddIssue(Issues, TEXT("error"), TEXT("unsupported_schema"), TEXT("schema"), Schema);
		}

		TSet<FString> ResourceIds;
		for (int32 Index = 0; Index < Resources.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>& Resource = Resources[Index];
			const FString Path = FString::Printf(TEXT("resources[%d]"), Index);
			const FString Id = GetResourceId(Resource);
			if (Id.IsEmpty())
			{
				AddIssue(Issues, TEXT("error"), TEXT("missing_resource_id"), Path + TEXT(".id"), TEXT("Each resource requires id"));
			}
			else if (ResourceIds.Contains(Id))
			{
				AddIssue(Issues, TEXT("error"), TEXT("duplicate_resource_id"), Path + TEXT(".id"), Id);
			}
			else
			{
				ResourceIds.Add(Id);
			}

			const FString Category = GetResourceCategory(Resource);
			const FString Kind = GetResourceKind(Resource);
			const FString Operation = GetResourceOperation(Resource, TEXT("upsert"));
			const bool bActorResource = IsSpatialActorResource(Resource);
			if (bActorOnly && !bActorResource && Operation != TEXT("reference"))
			{
				AddIssue(Issues, TEXT("error"), TEXT("actor_alias_non_actor_resource"), Path, Kind);
			}
			if (Resource->HasField(TEXT("lods")) || Resource->HasField(TEXT("static_mesh_lods")))
			{
				AddIssue(Issues, TEXT("error"), TEXT("asset_lod_out_of_level_scope"), Path, TEXT("StaticMesh internal LOD must use StaticMesh asset commands"));
			}
			if (!bActorResource)
			{
				if (Category == TEXT("organization") || Category == TEXT("level_container") || Category == TEXT("terrain") || Kind.Contains(TEXT("hlod")) || Kind.Contains(TEXT("streaming")))
				{
					AddIssue(Issues, Operation == TEXT("reference") ? TEXT("warning") : TEXT("error"), TEXT("resource_kind_report_only_or_unsupported_for_mutation"), Path, Kind);
				}
				else
				{
					AddIssue(Issues, TEXT("error"), TEXT("unknown_category_or_kind"), Path, Category + TEXT(":") + Kind);
				}
				continue;
			}

			const FString ClassPath = GetClassPathForResource(Resource);
			UClass* ActorClass = LoadClass<AActor>(nullptr, *ClassPath);
			if (!ActorClass)
			{
				const FString Code = (Kind == TEXT("pcg_volume") || Kind == TEXT("smart_object")) ? TEXT("plugin_unavailable_or_class_missing") : TEXT("actor_class_not_found");
				AddIssue(Issues, TEXT("error"), Code, Path + TEXT(".class"), ClassPath);
			}

			if (Kind == TEXT("custom"))
			{
				bool bStrictAdapter = false;
				TryGetBoolAny(Resource, { TEXT("strict_adapter") }, bStrictAdapter);
				FString Adapter;
				TryGetStringAny(Resource, { TEXT("adapter") }, Adapter);
				if (!Adapter.IsEmpty() && bStrictAdapter)
				{
					AddIssue(Issues, TEXT("error"), TEXT("custom_adapter_not_registered"), Path + TEXT(".adapter"), Adapter);
				}
				else if (Adapter.IsEmpty())
				{
					AddIssue(Issues, TEXT("warning"), TEXT("custom_actor_reflection_fallback"), Path, TEXT("No adapter specified; reflection fallback will be used"));
				}
			}

			if (ActorClass)
			{
				UObject* CDO = ActorClass->GetDefaultObject();
				TSharedPtr<FJsonObject> Properties;
				if (TryGetObject(Resource, TEXT("properties"), Properties))
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
					{
						if (!ValidatePropertyExists(CDO, Pair.Key))
						{
							AddIssue(Issues, TEXT("error"), TEXT("property_not_found"), Path + TEXT(".properties.") + Pair.Key, Pair.Key);
						}
					}
				}
			}
		}

		ValidateAttachmentGraph(Resources, Issues);
		ValidateOrganizationAndTasks(World, Content, Issues);

		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("schema"), ValidationSchema);
		OutData->SetBoolField(TEXT("actor_only"), bActorOnly);
		OutData->SetNumberField(TEXT("resource_count"), Resources.Num());
		OutData->SetArrayField(TEXT("issues"), Issues);
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetBoolField(TEXT("valid"), !HasErrorIssue(Issues));
		OutData->SetObjectField(TEXT("feature_gates"), BuildFeatureGateReport(Content));
		return true;
	}

	static TArray<FString> ReadProtectionTags(const TSharedPtr<FJsonObject>& Content)
	{
		TArray<FString> Tags;
		TSharedPtr<FJsonObject> Protection;
		if (TryGetObject(Content, TEXT("protection"), Protection))
		{
			Tags = ReadStringArray(Protection, TEXT("protected_tags"));
		}
		const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
		if (Content.IsValid() && TryGetArray(Content, TEXT("operations"), Operations) && Operations)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Operations)
			{
				const TSharedPtr<FJsonObject> Op = Value.IsValid() ? Value->AsObject() : nullptr;
				FString Type;
				if (Op.IsValid() && TryGetStringAny(Op, { TEXT("type"), TEXT("operation") }, Type) && Type.Equals(TEXT("protect"), ESearchCase::IgnoreCase))
				{
					Tags.Append(ReadStringArray(Op, TEXT("tags")));
					FString Tag;
					if (TryGetStringAny(Op, { TEXT("tag") }, Tag) && !Tag.IsEmpty())
					{
						Tags.Add(Tag);
					}
				}
			}
		}
		return Tags;
	}

	static bool ActorIsProtected(AActor* Actor, const TArray<FString>& ProtectionTags)
	{
		for (const FString& Tag : ProtectionTags)
		{
			if (ActorHasTagString(Actor, Tag))
			{
				return true;
			}
		}
		return false;
	}

	static TSharedPtr<FJsonObject> BuildEvidenceReport(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, const FString& Phase, bool bDryRun, bool bAllowFileWrite, FString& OutError)
	{
		TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("schema"), TEXT("uai.level_content.evidence.v1"));
		Report->SetStringField(TEXT("phase"), Phase);
		Report->SetBoolField(TEXT("dry_run"), bDryRun);
		Report->SetObjectField(TEXT("world"), BuildWorldInfo(World));

		TSharedPtr<FJsonObject> Evidence;
		if (!TryGetObject(Content, TEXT("evidence"), Evidence))
		{
			Report->SetBoolField(TEXT("report_requested"), false);
			Report->SetStringField(TEXT("status"), TEXT("not_requested"));
			return Report;
		}

		bool bReportRequested = false;
		TryGetBoolAny(Evidence, { TEXT("report"), TEXT("enabled"), TEXT("capture_report") }, bReportRequested);
		const TArray<TSharedPtr<FJsonValue>>* ViewBookmarks = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Screenshots = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Captures = nullptr;
		const bool bHasViewBookmarks = TryGetArray(Evidence, TEXT("view_bookmarks"), ViewBookmarks) && ViewBookmarks;
		const bool bHasScreenshots = TryGetArray(Evidence, TEXT("screenshots"), Screenshots) && Screenshots;
		const bool bHasCaptures = TryGetArray(Evidence, TEXT("captures"), Captures) && Captures;
		bReportRequested = bReportRequested || bHasViewBookmarks || bHasScreenshots || bHasCaptures;

		Report->SetBoolField(TEXT("report_requested"), bReportRequested);
		Report->SetStringField(TEXT("status"), bReportRequested ? TEXT("report_generated") : TEXT("not_requested"));
		Report->SetStringField(TEXT("visual_capture_status"), TEXT("report_only"));
		Report->SetStringField(TEXT("visual_capture_reason"), TEXT("viewport screenshots and GPU readback remain delegated to dedicated crash-guarded capture commands"));
		if (bHasViewBookmarks)
		{
			Report->SetArrayField(TEXT("view_bookmarks"), *ViewBookmarks);
			Report->SetNumberField(TEXT("view_bookmark_count"), ViewBookmarks->Num());
		}
		if (bHasScreenshots)
		{
			Report->SetArrayField(TEXT("screenshots"), *Screenshots);
			Report->SetNumberField(TEXT("screenshot_count"), Screenshots->Num());
		}
		if (bHasCaptures)
		{
			Report->SetArrayField(TEXT("captures"), *Captures);
			Report->SetNumberField(TEXT("capture_count"), Captures->Num());
		}

		FString OutputFile;
		if (bAllowFileWrite && (TryGetStringAny(Evidence, { TEXT("output_file"), TEXT("report_file"), TEXT("evidence_file") }, OutputFile)) && !OutputFile.TrimStartAndEnd().IsEmpty())
		{
			OutputFile = ResolveFilePath(OutputFile);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputFile), true);
			if (!FFileHelper::SaveStringToFile(JsonToString(Report, true), *OutputFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutError = TEXT("evidence_output_file_write_failed");
				Report->SetStringField(TEXT("status"), TEXT("write_failed"));
				Report->SetStringField(TEXT("error"), OutError);
				return Report;
			}
			Report->SetStringField(TEXT("output_file"), OutputFile);
		}
		return Report;
	}

	static TSharedPtr<FJsonObject> BuildPlan(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, bool bActorOnly)
	{
		TSharedPtr<FJsonObject> Plan = MakeShared<FJsonObject>();
		Plan->SetStringField(TEXT("schema"), PlanSchema);
		Plan->SetBoolField(TEXT("actor_only"), bActorOnly);
		Plan->SetObjectField(TEXT("world"), BuildWorldInfo(World));
		TArray<TSharedPtr<FJsonValue>> Steps;
		TArray<TSharedPtr<FJsonObject>> Resources;
		CollectResourceObjects(Content, Resources);
		const FString DefaultMode = Params.IsValid() && Params->HasField(TEXT("mode")) ? Params->GetStringField(TEXT("mode")).ToLower() : TEXT("upsert");
		for (int32 Index = 0; Index < Resources.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>& Resource = Resources[Index];
			TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
			const FString Id = GetResourceId(Resource);
			const FString Operation = GetResourceOperation(Resource, DefaultMode == TEXT("create_only") ? TEXT("create") : TEXT("upsert"));
			Step->SetStringField(TEXT("id"), Id);
			Step->SetStringField(TEXT("operation"), Operation);
			Step->SetStringField(TEXT("category"), GetResourceCategory(Resource));
			Step->SetStringField(TEXT("kind"), GetResourceKind(Resource));
			Step->SetStringField(TEXT("class"), GetClassPathForResource(Resource));
			Step->SetBoolField(TEXT("actor_resource"), IsSpatialActorResource(Resource));
			AActor* Existing = FindActorByNameLabelOrManagedId(World, Id);
			Step->SetBoolField(TEXT("exists"), Existing != nullptr);
			if (Existing)
			{
				Step->SetStringField(TEXT("existing_object_path"), Existing->GetPathName());
			}
			Steps.Add(MakeShared<FJsonValueObject>(Step));
		}

		const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
		if (Content.IsValid() && TryGetArray(Content, TEXT("operations"), Operations) && Operations)
		{
			for (int32 Index = 0; Index < Operations->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Op = (*Operations)[Index].IsValid() ? (*Operations)[Index]->AsObject() : nullptr;
				if (!Op.IsValid())
				{
					continue;
				}
				TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
				FString Type;
				TryGetStringAny(Op, { TEXT("type"), TEXT("operation") }, Type);
				Step->SetStringField(TEXT("operation"), Type);
				Step->SetStringField(TEXT("phase"), TEXT("explicit_operation"));
				Step->SetNumberField(TEXT("operation_index"), Index);
				if (Type.Equals(TEXT("convert"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("migrate"), ESearchCase::IgnoreCase))
				{
					Step->SetStringField(TEXT("execution"), TEXT("report_only_destructive_conversion_requires_dedicated_adapter"));
				}
				else if (Type.Equals(TEXT("clone"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("attach"), ESearchCase::IgnoreCase) ||
					Type.Equals(TEXT("detach"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("set_folder"), ESearchCase::IgnoreCase) ||
					Type.Equals(TEXT("add_tag"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("remove_tag"), ESearchCase::IgnoreCase) ||
					Type.Equals(TEXT("align"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("delete"), ESearchCase::IgnoreCase))
				{
					Step->SetStringField(TEXT("execution"), TEXT("editor_safe_operation"));
				}
				else if (Type.Equals(TEXT("screenshot"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("evidence"), ESearchCase::IgnoreCase))
				{
					Step->SetStringField(TEXT("execution"), TEXT("report_only_visual_capture"));
				}
				Steps.Add(MakeShared<FJsonValueObject>(Step));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Tasks = nullptr;
		if (Content.IsValid() && TryGetArray(Content, TEXT("tasks"), Tasks) && Tasks)
		{
			for (int32 Index = 0; Index < Tasks->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Task = (*Tasks)[Index].IsValid() ? (*Tasks)[Index]->AsObject() : nullptr;
				if (!Task.IsValid())
				{
					continue;
				}
				FString Type;
				TryGetStringAny(Task, { TEXT("type"), TEXT("kind"), TEXT("task") }, Type);
				Type = Type.ToLower();
				TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
				Step->SetStringField(TEXT("phase"), TEXT("explicit_task"));
				Step->SetStringField(TEXT("task"), Type);
				Step->SetNumberField(TEXT("task_index"), Index);
				if (Type == TEXT("navmesh_build") || Type == TEXT("cleanup_empty_folders"))
				{
					Step->SetStringField(TEXT("execution"), TEXT("editor_safe_task"));
				}
				else
				{
					Step->SetStringField(TEXT("execution"), TEXT("feature_gated_report_only"));
				}
				Steps.Add(MakeShared<FJsonValueObject>(Step));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Validations = nullptr;
		if (Content.IsValid() && TryGetArray(Content, TEXT("validation"), Validations) && Validations)
		{
			for (int32 Index = 0; Index < Validations->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Validation = (*Validations)[Index].IsValid() ? (*Validations)[Index]->AsObject() : nullptr;
				if (!Validation.IsValid())
				{
					continue;
				}
				FString Type;
				TryGetStringAny(Validation, { TEXT("type"), TEXT("kind") }, Type);
				TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
				Step->SetStringField(TEXT("phase"), TEXT("validation"));
				Step->SetStringField(TEXT("validation"), Type.ToLower());
				Step->SetNumberField(TEXT("validation_index"), Index);
				Step->SetStringField(TEXT("execution"), TEXT("read_only_world_probe"));
				Steps.Add(MakeShared<FJsonValueObject>(Step));
			}
		}

		TSharedPtr<FJsonObject> Evidence;
		if (TryGetObject(Content, TEXT("evidence"), Evidence))
		{
			TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
			Step->SetStringField(TEXT("phase"), TEXT("evidence"));
			Step->SetStringField(TEXT("operation"), TEXT("evidence_report"));
			Step->SetStringField(TEXT("execution"), TEXT("report_only_visual_capture"));
			Steps.Add(MakeShared<FJsonValueObject>(Step));

			FString EvidenceError;
			Plan->SetObjectField(TEXT("evidence"), BuildEvidenceReport(World, Content, Params, TEXT("plan"), true, false, EvidenceError));
		}
		Plan->SetNumberField(TEXT("step_count"), Steps.Num());
		Plan->SetArrayField(TEXT("steps"), Steps);
		Plan->SetObjectField(TEXT("feature_gates"), BuildFeatureGateReport(Content));
		return Plan;
	}

	static bool ApplyStaticMeshAdapter(AActor* Actor, const TSharedPtr<FJsonObject>& Resource, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		FString MeshPath;
		if (!TryGetStringAny(Resource, { TEXT("static_mesh"), TEXT("mesh") }, MeshPath) || MeshPath.TrimStartAndEnd().IsEmpty())
		{
			TSharedPtr<FJsonObject> Asset;
			if (TryGetObject(Resource, TEXT("asset"), Asset))
			{
				TryGetStringAny(Asset, { TEXT("static_mesh"), TEXT("mesh") }, MeshPath);
			}
		}
		if (MeshPath.TrimStartAndEnd().IsEmpty())
		{
			TSharedPtr<FJsonObject> Properties;
			if (TryGetObject(Resource, TEXT("properties"), Properties))
			{
				TSharedPtr<FJsonObject> StaticMeshObj;
				if (TryGetObject(Properties, TEXT("StaticMesh"), StaticMeshObj))
				{
					StaticMeshObj->TryGetStringField(TEXT("asset"), MeshPath);
				}
			}
		}
		if (MeshPath.TrimStartAndEnd().IsEmpty())
		{
			return true;
		}
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
		if (!Mesh)
		{
			OutError = TEXT("static_mesh_not_found");
			return false;
		}
		UStaticMeshComponent* Component = nullptr;
		if (AStaticMeshActor* StaticMeshActor = Cast<AStaticMeshActor>(Actor))
		{
			Component = StaticMeshActor->GetStaticMeshComponent();
		}
		if (!Component)
		{
			Component = Actor ? Actor->FindComponentByClass<UStaticMeshComponent>() : nullptr;
		}
		if (!Component)
		{
			OutError = TEXT("static_mesh_component_not_found");
			return false;
		}
		Actor->Modify();
		Component->Modify();
		Component->SetStaticMesh(Mesh);
		Component->MarkRenderStateDirty();
		OutResult->SetStringField(TEXT("static_mesh"), MeshPath);
		return true;
	}

	static bool ApplyActorTransform(AActor* Actor, const TSharedPtr<FJsonObject>& Resource, TSharedPtr<FJsonObject>& OutResult)
	{
		if (!Actor || !Resource.IsValid())
		{
			return false;
		}
		TSharedPtr<FJsonObject> Transform;
		if (!TryGetObject(Resource, TEXT("transform"), Transform))
		{
			return true;
		}
		FVector Location = Actor->GetActorLocation();
		FRotator Rotation = Actor->GetActorRotation();
		FVector Scale = Actor->GetActorScale3D();
		const bool bHasLocation = TryGetVectorField(Transform, TEXT("location"), Location);
		const bool bHasRotation = TryGetRotatorField(Transform, TEXT("rotation"), Rotation);
		const bool bHasScale = TryGetVectorField(Transform, TEXT("scale"), Scale);
		if (!bHasLocation && !bHasRotation && !bHasScale)
		{
			return true;
		}
		Actor->Modify();
		Actor->SetActorTransform(FTransform(Rotation, Location, Scale), false, nullptr, ETeleportType::TeleportPhysics);
		TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
		Applied->SetObjectField(TEXT("location"), MakeVectorJson(Actor->GetActorLocation()));
		Applied->SetObjectField(TEXT("rotation"), MakeRotatorJson(Actor->GetActorRotation()));
		Applied->SetObjectField(TEXT("scale"), MakeVectorJson(Actor->GetActorScale3D()));
		OutResult->SetObjectField(TEXT("transform"), Applied);
		return true;
	}

	static bool TryReadNavMeshBoundsSpec(const TSharedPtr<FJsonObject>& Resource, AActor* Actor, bool bActorCreatedThisCall, FBox& OutBounds, FRotator& OutRotation, bool& bOutExplicitBounds)
	{
		bOutExplicitBounds = false;
		OutBounds = FBox(ForceInit);
		if (!Resource.IsValid() || !Actor)
		{
			return false;
		}

		TSharedPtr<FJsonObject> BoundsObj;
		if ((TryGetObject(Resource, TEXT("bounds"), BoundsObj) ||
			TryGetObject(Resource, TEXT("nav_bounds"), BoundsObj) ||
			TryGetObject(Resource, TEXT("brush_bounds"), BoundsObj)) &&
			TryReadBoundsSpec(BoundsObj, OutBounds) &&
			OutBounds.IsValid != 0)
		{
			bOutExplicitBounds = true;
		}
		else if (TryReadBoundsSpec(Resource, OutBounds) && OutBounds.IsValid != 0)
		{
			bOutExplicitBounds = true;
		}
		else if (bActorCreatedThisCall)
		{
			const FVector Scale = Actor->GetActorScale3D();
			FVector Extent(
				FMath::Abs(Scale.X) * 100.0,
				FMath::Abs(Scale.Y) * 100.0,
				FMath::Abs(Scale.Z) * 100.0);
			if (Extent.X <= KINDA_SMALL_NUMBER)
			{
				Extent.X = 100.0;
			}
			if (Extent.Y <= KINDA_SMALL_NUMBER)
			{
				Extent.Y = 100.0;
			}
			if (Extent.Z <= KINDA_SMALL_NUMBER)
			{
				Extent.Z = 100.0;
			}
			OutBounds = FBox::BuildAABB(Actor->GetActorLocation(), Extent);
		}
		else
		{
			return false;
		}

		FVector Center = OutBounds.GetCenter();
		FVector Extent = OutBounds.GetExtent();
		Extent = FVector(FMath::Abs(Extent.X), FMath::Abs(Extent.Y), FMath::Abs(Extent.Z));
		if (Extent.X <= KINDA_SMALL_NUMBER || Extent.Y <= KINDA_SMALL_NUMBER || Extent.Z <= KINDA_SMALL_NUMBER)
		{
			return false;
		}
		OutBounds = FBox::BuildAABB(Center, Extent);

		OutRotation = Actor->GetActorRotation();
		TSharedPtr<FJsonObject> Transform;
		if (TryGetObject(Resource, TEXT("transform"), Transform))
		{
			TryGetRotatorField(Transform, TEXT("rotation"), OutRotation);
		}
		return true;
	}

	static bool ApplyNavMeshBoundsAdapter(AActor* Actor, const TSharedPtr<FJsonObject>& Resource, UWorld* World, bool bActorCreatedThisCall, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		ANavMeshBoundsVolume* Volume = Cast<ANavMeshBoundsVolume>(Actor);
		if (!Volume)
		{
			return true;
		}

		FBox RequestedBounds(ForceInit);
		FRotator RequestedRotation = Volume->GetActorRotation();
		bool bExplicitBounds = false;
		if (!TryReadNavMeshBoundsSpec(Resource, Actor, bActorCreatedThisCall, RequestedBounds, RequestedRotation, bExplicitBounds))
		{
			if (bActorCreatedThisCall)
			{
				OutError = TEXT("navmesh_bounds_extent_invalid");
				return false;
			}
			return true;
		}

		const FVector Center = RequestedBounds.GetCenter();
		const FVector Extent = RequestedBounds.GetExtent();
		if (Extent.X <= KINDA_SMALL_NUMBER || Extent.Y <= KINDA_SMALL_NUMBER || Extent.Z <= KINDA_SMALL_NUMBER)
		{
			OutError = TEXT("navmesh_bounds_extent_invalid");
			return false;
		}

		bool bUpdateNavigationBounds = true;
		TryGetBoolAny(Resource, { TEXT("update_navigation_bounds"), TEXT("update_nav_bounds") }, bUpdateNavigationBounds);

		Volume->Modify();
		Volume->SetActorLocation(Center);
		Volume->SetActorRotation(RequestedRotation);
		Volume->SetActorScale3D(FVector::OneVector);

		UCubeBuilder* Builder = NewObject<UCubeBuilder>(GetTransientPackage());
		if (!Builder)
		{
			OutError = TEXT("navmesh_bounds_builder_create_failed");
			return false;
		}
		Builder->X = Extent.X * 2.0f;
		Builder->Y = Extent.Y * 2.0f;
		Builder->Z = Extent.Z * 2.0f;
		UActorFactory::CreateBrushForVolumeActor(Volume, Builder);

		Volume->SetActorLocation(Center);
		Volume->SetActorRotation(RequestedRotation);
		Volume->SetActorScale3D(FVector::OneVector);

		bool bNavBoundsUpdated = false;
		if (bUpdateNavigationBounds)
		{
			if (UNavigationSystemV1* NavSys = World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr)
			{
				NavSys->OnNavigationBoundsUpdated(Volume);
				bNavBoundsUpdated = true;
			}
		}

		TSharedPtr<FJsonObject> AppliedTransform = MakeShared<FJsonObject>();
		AppliedTransform->SetObjectField(TEXT("location"), MakeVectorJson(Volume->GetActorLocation()));
		AppliedTransform->SetObjectField(TEXT("rotation"), MakeRotatorJson(Volume->GetActorRotation()));
		AppliedTransform->SetObjectField(TEXT("scale"), MakeVectorJson(Volume->GetActorScale3D()));
		OutResult->SetObjectField(TEXT("transform"), AppliedTransform);
		OutResult->SetObjectField(TEXT("bounds"), MakeBoundsJson(Volume->GetComponentsBoundingBox(true)));
		OutResult->SetObjectField(TEXT("requested_center"), MakeVectorJson(Center));
		OutResult->SetObjectField(TEXT("requested_extent"), MakeVectorJson(Extent));
		OutResult->SetObjectField(TEXT("builder_size"), MakeVectorJson(FVector(Builder->X, Builder->Y, Builder->Z)));
		OutResult->SetBoolField(TEXT("nav_bounds_explicit"), bExplicitBounds);
		OutResult->SetBoolField(TEXT("update_navigation_bounds"), bUpdateNavigationBounds);
		OutResult->SetBoolField(TEXT("nav_bounds_updated"), bNavBoundsUpdated);
		return true;
	}

	static ESplinePointType::Type ParseSplinePointType(const FString& TypeText)
	{
		const FString Normalized = TypeText.TrimStartAndEnd().ToLower();
		if (Normalized == TEXT("linear") || Normalized == TEXT("esplinepointtype::linear"))
		{
			return ESplinePointType::Linear;
		}
		if (Normalized == TEXT("constant") || Normalized == TEXT("esplinepointtype::constant"))
		{
			return ESplinePointType::Constant;
		}
		if (Normalized == TEXT("curveclamped") || Normalized == TEXT("curve_clamped") || Normalized == TEXT("esplinepointtype::curveclamped"))
		{
			return ESplinePointType::CurveClamped;
		}
		if (Normalized == TEXT("curvecustomtangent") || Normalized == TEXT("curve_custom_tangent") || Normalized == TEXT("esplinepointtype::curvecustomtangent"))
		{
			return ESplinePointType::CurveCustomTangent;
		}
		return ESplinePointType::Curve;
	}

	static bool ApplySplineComponentSpec(USplineComponent* SplineComponent, const TSharedPtr<FJsonObject>& ComponentSpec, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
		if (!SplineComponent || !ComponentSpec.IsValid() || !TryGetArray(ComponentSpec, TEXT("spline_points"), Points) || !Points)
		{
			return true;
		}

		FString SpaceText = TEXT("local");
		TryGetStringAny(ComponentSpec, { TEXT("spline_coordinate_space"), TEXT("coordinate_space") }, SpaceText);
		const ESplineCoordinateSpace::Type CoordinateSpace = SpaceText.TrimStartAndEnd().Equals(TEXT("world"), ESearchCase::IgnoreCase)
			? ESplineCoordinateSpace::World
			: ESplineCoordinateSpace::Local;

		bool bClosedLoop = SplineComponent->IsClosedLoop();
		TryGetBoolAny(ComponentSpec, { TEXT("spline_closed_loop"), TEXT("closed_loop") }, bClosedLoop);

		SplineComponent->Modify();
		SplineComponent->ClearSplinePoints(false);
		for (int32 PointIndex = 0; PointIndex < Points->Num(); ++PointIndex)
		{
			const TSharedPtr<FJsonObject> Point = (*Points)[PointIndex].IsValid() ? (*Points)[PointIndex]->AsObject() : nullptr;
			if (!Point.IsValid())
			{
				OutError = TEXT("spline_point_not_object");
				return false;
			}

			FVector Location = FVector::ZeroVector;
			if (!TryGetVectorField(Point, TEXT("location"), Location) && !TryGetVectorField(Point, TEXT("position"), Location))
			{
				OutError = TEXT("spline_point_missing_location");
				return false;
			}

			SplineComponent->AddSplinePoint(Location, CoordinateSpace, false);

			FString TypeText;
			if (TryGetStringAny(Point, { TEXT("type"), TEXT("point_type") }, TypeText))
			{
				SplineComponent->SetSplinePointType(PointIndex, ParseSplinePointType(TypeText), false);
			}

			FVector ArriveTangent = FVector::ZeroVector;
			FVector LeaveTangent = FVector::ZeroVector;
			const bool bHasArriveTangent = TryGetVectorField(Point, TEXT("arrive_tangent"), ArriveTangent);
			const bool bHasLeaveTangent = TryGetVectorField(Point, TEXT("leave_tangent"), LeaveTangent);
			if (bHasArriveTangent || bHasLeaveTangent)
			{
				SplineComponent->SetTangentsAtSplinePoint(PointIndex, ArriveTangent, LeaveTangent, CoordinateSpace, false);
			}
		}

		SplineComponent->SetClosedLoop(bClosedLoop, false);
		SplineComponent->UpdateSpline();
		SplineComponent->MarkPackageDirty();

		OutResult->SetBoolField(TEXT("spline_points_applied"), true);
		OutResult->SetNumberField(TEXT("spline_point_count"), SplineComponent->GetNumberOfSplinePoints());
		OutResult->SetBoolField(TEXT("spline_closed_loop"), SplineComponent->IsClosedLoop());
		return true;
	}

	static bool ApplyComponentSpec(AActor* Actor, const TSharedPtr<FJsonObject>& ComponentSpec, UWorld* World, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		OutResult = MakeShared<FJsonObject>();
		if (!Actor || !ComponentSpec.IsValid())
		{
			OutError = TEXT("invalid_component_spec");
			return false;
		}
		FString Name;
		TryGetStringAny(ComponentSpec, { TEXT("name"), TEXT("id") }, Name);
		if (Name.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("component_missing_name");
			return false;
		}
		Name = Name.TrimStartAndEnd();

		UActorComponent* Component = FindComponentByName(Actor, Name);
		FString Operation = GetResourceOperation(ComponentSpec, TEXT("upsert"));
		if (!Component)
		{
			if (Operation == TEXT("patch"))
			{
				OutError = TEXT("component_not_found");
				return false;
			}
			FString ClassPath;
			if (!TryGetStringAny(ComponentSpec, { TEXT("class"), TEXT("class_path") }, ClassPath) || ClassPath.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("component_missing_class");
				return false;
			}
			UClass* ComponentClass = LoadClass<UActorComponent>(nullptr, *ClassPath);
			if (!ComponentClass)
			{
				OutError = TEXT("component_class_not_found");
				return false;
			}
			Component = NewObject<UActorComponent>(Actor, ComponentClass, FName(*Name), RF_Transactional);
			if (!Component)
			{
				OutError = TEXT("component_create_failed");
				return false;
			}
			Actor->AddInstanceComponent(Component);
			Component->OnComponentCreated();
			if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
			{
				USceneComponent* Parent = Actor->GetRootComponent();
				FString ParentName;
				if (TryGetStringAny(ComponentSpec, { TEXT("parent") }, ParentName) && !ParentName.IsEmpty())
				{
					Parent = Cast<USceneComponent>(FindComponentByName(Actor, ParentName));
				}
				if (!Parent)
				{
					Actor->SetRootComponent(SceneComponent);
				}
				else
				{
					SceneComponent->AttachToComponent(Parent, FAttachmentTransformRules::KeepRelativeTransform);
				}
			}
			Component->RegisterComponent();
			OutResult->SetStringField(TEXT("create_status"), TEXT("created"));
		}
		else
		{
			OutResult->SetStringField(TEXT("create_status"), TEXT("existing"));
		}

		if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
		{
			TSharedPtr<FJsonObject> Transform;
			if (TryGetObject(ComponentSpec, TEXT("transform"), Transform))
			{
				FVector RelativeLocation = SceneComponent->GetRelativeLocation();
				FRotator RelativeRotation = SceneComponent->GetRelativeRotation();
				FVector RelativeScale = SceneComponent->GetRelativeScale3D();
				const bool bHasLocation = TryGetVectorField(Transform, TEXT("relative_location"), RelativeLocation);
				const bool bHasRotation = TryGetRotatorField(Transform, TEXT("relative_rotation"), RelativeRotation);
				const bool bHasScale = TryGetVectorField(Transform, TEXT("relative_scale"), RelativeScale);
				if (bHasLocation || bHasRotation || bHasScale)
				{
					SceneComponent->Modify();
					SceneComponent->SetRelativeLocation(RelativeLocation);
					SceneComponent->SetRelativeRotation(RelativeRotation);
					SceneComponent->SetRelativeScale3D(RelativeScale);
				}
			}
		}

		if (USplineComponent* SplineComponent = Cast<USplineComponent>(Component))
		{
			if (!ApplySplineComponentSpec(SplineComponent, ComponentSpec, OutResult, OutError))
			{
				return false;
			}
		}

		TArray<TSharedPtr<FJsonValue>> PropertyResults;
		TSharedPtr<FJsonObject> Properties;
		if (TryGetObject(ComponentSpec, TEXT("properties"), Properties))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
			{
				TSharedPtr<FJsonObject> PropertyResult;
				FString PropertyError;
				if (!SetObjectPropertyFromJson(Component, Pair.Key, Pair.Value, World, PropertyResult, PropertyError))
				{
					PropertyResults.Add(MakeShared<FJsonValueObject>(PropertyResult));
					OutError = PropertyError;
					return false;
				}
				PropertyResults.Add(MakeShared<FJsonValueObject>(PropertyResult));
			}
		}
		OutResult->SetStringField(TEXT("name"), Component->GetName());
		OutResult->SetStringField(TEXT("class"), Component->GetClass() ? Component->GetClass()->GetPathName() : FString());
		OutResult->SetArrayField(TEXT("property_results"), PropertyResults);
		return true;
	}

	static bool ApplyResource(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Resource, const FString& Mode, const TArray<FString>& ProtectionTags, TMap<FString, AActor*>& ActorById, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		OutResult = MakeShared<FJsonObject>();
		const FString Id = GetResourceId(Resource);
		const FString Operation = GetResourceOperation(Resource, Mode == TEXT("create_only") ? TEXT("create") : TEXT("upsert"));
		OutResult->SetStringField(TEXT("id"), Id);
		OutResult->SetStringField(TEXT("operation"), Operation);
		OutResult->SetStringField(TEXT("kind"), GetResourceKind(Resource));

		if (!IsSpatialActorResource(Resource))
		{
			OutResult->SetStringField(TEXT("status"), TEXT("report_only_or_unsupported_resource"));
			OutError = TEXT("unsupported_resource_kind");
			return false;
		}

		AActor* Actor = FindActorByNameLabelOrManagedId(World, Id);
		bool bActorCreatedThisCall = false;
		if (Operation == TEXT("delete"))
		{
			if (!Actor)
			{
				OutResult->SetStringField(TEXT("status"), TEXT("missing_noop"));
				return true;
			}
			if (ActorIsProtected(Actor, ProtectionTags))
			{
				OutError = TEXT("actor_protected");
				OutResult->SetStringField(TEXT("status"), TEXT("blocked_by_protection"));
				return false;
			}
			Actor->Modify();
			Actor->Destroy();
			OutResult->SetStringField(TEXT("status"), TEXT("deleted"));
			return true;
		}

		if (Operation == TEXT("clone"))
		{
			FString SourceActorId;
			if (!TryGetStringAny(Resource, { TEXT("source_actor"), TEXT("from_actor") }, SourceActorId) || SourceActorId.IsEmpty())
			{
				OutError = TEXT("clone_missing_source_actor");
				return false;
			}
			AActor* SourceActor = FindActorByNameLabelOrManagedId(World, SourceActorId);
			if (!SourceActor)
			{
				OutError = TEXT("clone_source_actor_not_found");
				return false;
			}
			UEditorActorSubsystem* ActorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
			if (!ActorSubsystem)
			{
				OutError = TEXT("editor_actor_subsystem_unavailable");
				return false;
			}
			FVector Offset = FVector::ZeroVector;
			TryGetVectorField(Resource, TEXT("offset"), Offset);
			Actor = ActorSubsystem->DuplicateActor(SourceActor, World, Offset);
			if (!Actor)
			{
				OutError = TEXT("clone_failed");
				return false;
			}
			OutResult->SetStringField(TEXT("clone_source"), SourceActorId);
		}
		else if (!Actor)
		{
			if (Mode == TEXT("patch") || Operation == TEXT("patch"))
			{
				OutError = TEXT("actor_not_found_for_patch");
				return false;
			}
			const FString ClassPath = GetClassPathForResource(Resource);
			UClass* ActorClass = LoadClass<AActor>(nullptr, *ClassPath);
			if (!ActorClass)
			{
				OutError = TEXT("actor_class_not_found");
				return false;
			}
			FVector Location = FVector::ZeroVector;
			FRotator Rotation = FRotator::ZeroRotator;
			FVector Scale(1.0, 1.0, 1.0);
			TSharedPtr<FJsonObject> Transform;
			if (TryGetObject(Resource, TEXT("transform"), Transform))
			{
				TryGetVectorField(Transform, TEXT("location"), Location);
				TryGetRotatorField(Transform, TEXT("rotation"), Rotation);
				TryGetVectorField(Transform, TEXT("scale"), Scale);
			}
			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags |= RF_Transactional;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Actor = World->SpawnActor<AActor>(ActorClass, FTransform(Rotation, Location, Scale), SpawnParams);
			if (!Actor)
			{
				OutError = TEXT("spawn_failed");
				return false;
			}
			bActorCreatedThisCall = true;
			OutResult->SetStringField(TEXT("create_status"), TEXT("created"));
		}
		else if (Mode == TEXT("create_only") || Operation == TEXT("create"))
		{
			OutError = TEXT("actor_already_exists");
			return false;
		}
		else
		{
			OutResult->SetStringField(TEXT("create_status"), TEXT("existing"));
		}

		if (!Actor)
		{
			OutError = TEXT("actor_unresolved");
			return false;
		}
		if (ActorIsProtected(Actor, ProtectionTags) && (Operation == TEXT("patch") || Operation == TEXT("upsert")))
		{
			OutError = TEXT("actor_protected");
			OutResult->SetStringField(TEXT("status"), TEXT("blocked_by_protection"));
			return false;
		}

		FString Label;
		if (TryGetStringAny(Resource, { TEXT("label") }, Label) && !Label.TrimStartAndEnd().IsEmpty())
		{
			Actor->SetActorLabel(Label.TrimStartAndEnd());
		}
		else if (Operation == TEXT("clone") && !Id.IsEmpty())
		{
			Actor->SetActorLabel(Id);
		}

		FString FolderPath;
		if (!TryGetStringAny(Resource, { TEXT("folder_path") }, FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
		{
			FolderPath = GetDefaultFolderPath(Content);
		}
		if (FolderPath.TrimStartAndEnd().IsEmpty())
		{
			FolderPath = GetScopeFolderRoot(Content, Params);
		}
		FolderPath = NormalizeFolder(FolderPath);
		if (!FolderPath.IsEmpty())
		{
			Actor->Modify();
			Actor->SetFolderPath(FName(*FolderPath));
		}

		TArray<FString> Tags = GetDefaultTags(Content);
		Tags.Append(ReadTags(Resource));
		const FString ManagedTag = GetManagedTag(Content, Params);
		if (!ManagedTag.IsEmpty())
		{
			Tags.AddUnique(ManagedTag);
		}
		if (!Id.IsEmpty())
		{
			Tags.AddUnique(MakeManagedIdTag(Id));
		}
		for (const FString& Tag : Tags)
		{
			const FString CleanTag = Tag.TrimStartAndEnd();
			if (!CleanTag.IsEmpty())
			{
				Actor->Tags.AddUnique(FName(*CleanTag));
			}
		}

		ApplyActorTransform(Actor, Resource, OutResult);
		if (!ApplyNavMeshBoundsAdapter(Actor, Resource, World, bActorCreatedThisCall, OutResult, OutError))
		{
			return false;
		}
		if (!ApplyStaticMeshAdapter(Actor, Resource, OutResult, OutError))
		{
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> PropertyResults;
		TSharedPtr<FJsonObject> Properties;
		if (TryGetObject(Resource, TEXT("properties"), Properties))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
			{
				if (Pair.Key.Equals(TEXT("StaticMesh"), ESearchCase::IgnoreCase))
				{
					continue;
				}
				TSharedPtr<FJsonObject> PropertyResult;
				FString PropertyError;
				if (!SetObjectPropertyFromJson(Actor, Pair.Key, Pair.Value, World, PropertyResult, PropertyError))
				{
					PropertyResults.Add(MakeShared<FJsonValueObject>(PropertyResult));
					OutError = PropertyError;
					return false;
				}
				PropertyResults.Add(MakeShared<FJsonValueObject>(PropertyResult));
			}
		}

		TArray<TSharedPtr<FJsonValue>> ComponentResults;
		const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
		if (TryGetArray(Resource, TEXT("components"), Components) && Components)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Components)
			{
				const TSharedPtr<FJsonObject> ComponentSpec = Value.IsValid() ? Value->AsObject() : nullptr;
				TSharedPtr<FJsonObject> ComponentResult;
				if (!ApplyComponentSpec(Actor, ComponentSpec, World, ComponentResult, OutError))
				{
					ComponentResults.Add(MakeShared<FJsonValueObject>(ComponentResult));
					return false;
				}
				ComponentResults.Add(MakeShared<FJsonValueObject>(ComponentResult));
			}
			ApplyActorTransform(Actor, Resource, OutResult);
		}

		Actor->MarkPackageDirty();
		ActorById.Add(Id, Actor);
		OutResult->SetStringField(TEXT("status"), TEXT("applied"));
		OutResult->SetStringField(TEXT("actor_name"), Actor->GetName());
		OutResult->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
		OutResult->SetStringField(TEXT("object_path"), Actor->GetPathName());
		OutResult->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());
		OutResult->SetArrayField(TEXT("property_results"), PropertyResults);
		OutResult->SetArrayField(TEXT("component_results"), ComponentResults);
		return true;
	}

	static bool ApplyAttachments(UWorld* World, const TArray<TSharedPtr<FJsonObject>>& Resources, const TMap<FString, AActor*>& ActorById, TArray<TSharedPtr<FJsonValue>>& Results, FString& OutError)
	{
		for (const TSharedPtr<FJsonObject>& Resource : Resources)
		{
			const FString Id = GetResourceId(Resource);
			TSharedPtr<FJsonObject> Attachments;
			if (!TryGetObject(Resource, TEXT("attachments"), Attachments))
			{
				continue;
			}
			FString ParentId;
			if (!TryGetStringAny(Attachments, { TEXT("parent_actor"), TEXT("parent_id") }, ParentId) || ParentId.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}
			AActor* Child = ActorById.Contains(Id) ? ActorById[Id] : FindActorByNameLabelOrManagedId(World, Id);
			AActor* Parent = ActorById.Contains(ParentId) ? ActorById[ParentId] : FindActorByNameLabelOrManagedId(World, ParentId);
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("operation"), TEXT("attach"));
			Result->SetStringField(TEXT("child_id"), Id);
			Result->SetStringField(TEXT("parent_id"), ParentId);
			if (!Child || !Parent)
			{
				Result->SetStringField(TEXT("status"), TEXT("actor_not_found"));
				Results.Add(MakeShared<FJsonValueObject>(Result));
				OutError = TEXT("attachment_actor_not_found");
				return false;
			}
			Child->Modify();
			Child->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
			Result->SetStringField(TEXT("status"), TEXT("attached"));
			Results.Add(MakeShared<FJsonValueObject>(Result));
		}
		return true;
	}

	static bool DeleteManagedMissing(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, const TSet<FString>& TargetIds, const TArray<FString>& ProtectionTags, bool bDryRun, TArray<TSharedPtr<FJsonValue>>& Results, FString& OutError)
	{
		const FString FolderRoot = GetScopeFolderRoot(Content, Params);
		const FString ManagedTag = GetManagedTag(Content, Params);
		if (FolderRoot.IsEmpty() && ManagedTag.IsEmpty())
		{
			OutError = TEXT("delete_scope_requires_folder_or_managed_tag");
			return false;
		}

		TArray<AActor*> ToDelete;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsActorBeingDestroyed())
			{
				continue;
			}
			if (!FolderMatches(Actor->GetFolderPath().ToString(), FolderRoot, true))
			{
				continue;
			}
			if (!ManagedTag.IsEmpty() && !ActorHasTagString(Actor, ManagedTag))
			{
				continue;
			}
			const FString Id = GetManagedId(Actor);
			if (!Id.IsEmpty() && TargetIds.Contains(Id))
			{
				continue;
			}
			if (ActorIsProtected(Actor, ProtectionTags))
			{
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("operation"), TEXT("delete_missing"));
				Item->SetStringField(TEXT("status"), TEXT("blocked_by_protection"));
				Item->SetStringField(TEXT("actor"), Actor->GetActorLabel());
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}
			ToDelete.Add(Actor);
		}

		for (AActor* Actor : ToDelete)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("operation"), TEXT("delete_missing"));
			Item->SetStringField(TEXT("actor"), Actor->GetActorLabel());
			Item->SetStringField(TEXT("object_path"), Actor->GetPathName());
			if (bDryRun)
			{
				Item->SetStringField(TEXT("status"), TEXT("dry_run"));
			}
			else
			{
				Actor->Modify();
				Actor->Destroy();
				Item->SetStringField(TEXT("status"), TEXT("deleted"));
			}
			Results.Add(MakeShared<FJsonValueObject>(Item));
		}
		return true;
	}

	static AActor* ResolveOperationActor(UWorld* World, const TSharedPtr<FJsonObject>& Op, std::initializer_list<const TCHAR*> Keys, FString& OutRef)
	{
		OutRef.Reset();
		if (!TryGetStringAny(Op, Keys, OutRef) || OutRef.TrimStartAndEnd().IsEmpty())
		{
			return nullptr;
		}
		OutRef = OutRef.TrimStartAndEnd();
		return FindActorByNameLabelOrManagedId(World, OutRef);
	}

	static TArray<FString> ReadOperationTags(const TSharedPtr<FJsonObject>& Op)
	{
		TArray<FString> Tags = ReadStringArray(Op, TEXT("tags"));
		FString Tag;
		if (TryGetStringAny(Op, { TEXT("tag") }, Tag) && !Tag.TrimStartAndEnd().IsEmpty())
		{
			Tags.Add(Tag.TrimStartAndEnd());
		}
		return Tags;
	}

	static void ApplyOperationPatchToActor(AActor* Actor, UWorld* World, const TSharedPtr<FJsonObject>& Patch, TSharedPtr<FJsonObject>& Result, FString& OutError)
	{
		if (!Actor || !Patch.IsValid())
		{
			return;
		}
		ApplyActorTransform(Actor, Patch, Result);
		FString FolderPath;
		if (TryGetStringAny(Patch, { TEXT("folder_path"), TEXT("folder"), TEXT("target_folder") }, FolderPath) && !FolderPath.TrimStartAndEnd().IsEmpty())
		{
			Actor->Modify();
			Actor->SetFolderPath(FName(*NormalizeFolder(FolderPath)));
			Result->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());
		}
		for (const FString& Tag : ReadOperationTags(Patch))
		{
			const FString CleanTag = Tag.TrimStartAndEnd();
			if (!CleanTag.IsEmpty())
			{
				Actor->Tags.AddUnique(FName(*CleanTag));
			}
		}
		TSharedPtr<FJsonObject> Properties;
		if (TryGetObject(Patch, TEXT("properties"), Properties))
		{
			TArray<TSharedPtr<FJsonValue>> PropertyResults;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
			{
				TSharedPtr<FJsonObject> PropertyResult;
				FString PropertyError;
				if (!SetObjectPropertyFromJson(Actor, Pair.Key, Pair.Value, World, PropertyResult, PropertyError))
				{
					PropertyResults.Add(MakeShared<FJsonValueObject>(PropertyResult));
					OutError = PropertyError;
					Result->SetArrayField(TEXT("property_results"), PropertyResults);
					return;
				}
				PropertyResults.Add(MakeShared<FJsonValueObject>(PropertyResult));
			}
			Result->SetArrayField(TEXT("property_results"), PropertyResults);
		}
	}

	static bool ExecuteOperations(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, bool bDryRun, const TArray<FString>& ProtectionTags, TArray<TSharedPtr<FJsonValue>>& Results, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
		if (!Content.IsValid() || !TryGetArray(Content, TEXT("operations"), Operations) || !Operations)
		{
			return true;
		}
		for (int32 Index = 0; Index < Operations->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Op = (*Operations)[Index].IsValid() ? (*Operations)[Index]->AsObject() : nullptr;
			if (!Op.IsValid())
			{
				continue;
			}
			FString Type;
			TryGetStringAny(Op, { TEXT("type"), TEXT("operation") }, Type);
			Type = Type.ToLower();
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("operation"), Type);
			Result->SetNumberField(TEXT("operation_index"), Index);

			if (Type == TEXT("protect"))
			{
				Result->SetStringField(TEXT("status"), TEXT("active"));
			}
			else if (Type == TEXT("delete"))
			{
				FString ActorRef;
				AActor* Actor = ResolveOperationActor(World, Op, { TEXT("actor"), TEXT("id"), TEXT("actor_id"), TEXT("target_actor") }, ActorRef);
				Result->SetStringField(TEXT("actor_ref"), ActorRef);
				if (!Actor)
				{
					OutError = TEXT("operation_actor_not_found");
					Result->SetStringField(TEXT("status"), TEXT("actor_not_found"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				if (ActorIsProtected(Actor, ProtectionTags))
				{
					OutError = TEXT("actor_protected");
					Result->SetStringField(TEXT("status"), TEXT("blocked_by_protection"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				if (bDryRun)
				{
					Result->SetStringField(TEXT("status"), TEXT("dry_run"));
				}
				else
				{
					Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
					Actor->Modify();
					Actor->Destroy();
					Result->SetStringField(TEXT("status"), TEXT("deleted"));
				}
			}
			else if (Type == TEXT("clone"))
			{
				FString SourceRef;
				AActor* SourceActor = ResolveOperationActor(World, Op, { TEXT("from_actor"), TEXT("source_actor"), TEXT("source_id"), TEXT("template_id") }, SourceRef);
				FString TargetId;
				TryGetStringAny(Op, { TEXT("id"), TEXT("target_id"), TEXT("new_id") }, TargetId);
				TargetId = TargetId.TrimStartAndEnd();
				Result->SetStringField(TEXT("source_actor"), SourceRef);
				Result->SetStringField(TEXT("id"), TargetId);
				if (!SourceActor)
				{
					OutError = TEXT("clone_source_actor_not_found");
					Result->SetStringField(TEXT("status"), TEXT("source_actor_not_found"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				if (TargetId.IsEmpty())
				{
					OutError = TEXT("clone_missing_target_id");
					Result->SetStringField(TEXT("status"), TEXT("missing_target_id"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				if (FindActorByNameLabelOrManagedId(World, TargetId))
				{
					OutError = TEXT("clone_target_already_exists");
					Result->SetStringField(TEXT("status"), TEXT("target_already_exists"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				if (bDryRun)
				{
					Result->SetStringField(TEXT("status"), TEXT("dry_run"));
				}
				else
				{
					UEditorActorSubsystem* ActorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
					if (!ActorSubsystem)
					{
						OutError = TEXT("editor_actor_subsystem_unavailable");
						Result->SetStringField(TEXT("status"), TEXT("editor_actor_subsystem_unavailable"));
						Results.Add(MakeShared<FJsonValueObject>(Result));
						return false;
					}
					FVector Offset = FVector::ZeroVector;
					TryGetVectorField(Op, TEXT("offset"), Offset);
					AActor* ClonedActor = ActorSubsystem->DuplicateActor(SourceActor, World, Offset);
					if (!ClonedActor)
					{
						OutError = TEXT("clone_failed");
						Result->SetStringField(TEXT("status"), TEXT("clone_failed"));
						Results.Add(MakeShared<FJsonValueObject>(Result));
						return false;
					}
					FString Label;
					if (TryGetStringAny(Op, { TEXT("label") }, Label) && !Label.TrimStartAndEnd().IsEmpty())
					{
						ClonedActor->SetActorLabel(Label.TrimStartAndEnd());
					}
					else
					{
						ClonedActor->SetActorLabel(TargetId);
					}
					const FString ManagedTag = GetManagedTag(Content, Params);
					if (!ManagedTag.IsEmpty())
					{
						ClonedActor->Tags.AddUnique(FName(*ManagedTag));
					}
					ClonedActor->Tags.AddUnique(FName(*MakeManagedIdTag(TargetId)));
					for (const FString& Tag : ReadOperationTags(Op))
					{
						if (!Tag.TrimStartAndEnd().IsEmpty())
						{
							ClonedActor->Tags.AddUnique(FName(*Tag.TrimStartAndEnd()));
						}
					}
					ApplyOperationPatchToActor(ClonedActor, World, Op, Result, OutError);
					TSharedPtr<FJsonObject> Patch;
					if (OutError.IsEmpty() && TryGetObject(Op, TEXT("patch"), Patch))
					{
						ApplyOperationPatchToActor(ClonedActor, World, Patch, Result, OutError);
					}
					if (!OutError.IsEmpty())
					{
						Result->SetStringField(TEXT("status"), TEXT("patch_failed"));
						Results.Add(MakeShared<FJsonValueObject>(Result));
						return false;
					}
					ClonedActor->MarkPackageDirty();
					Result->SetStringField(TEXT("status"), TEXT("cloned"));
					Result->SetStringField(TEXT("actor"), ClonedActor->GetActorLabel());
					Result->SetStringField(TEXT("object_path"), ClonedActor->GetPathName());
				}
			}
			else if (Type == TEXT("attach"))
			{
				FString ChildRef;
				FString ParentRef;
				AActor* Child = ResolveOperationActor(World, Op, { TEXT("child"), TEXT("child_actor"), TEXT("child_id"), TEXT("actor"), TEXT("id"), TEXT("source_actor") }, ChildRef);
				AActor* Parent = ResolveOperationActor(World, Op, { TEXT("parent"), TEXT("parent_actor"), TEXT("parent_id"), TEXT("target_actor"), TEXT("target") }, ParentRef);
				Result->SetStringField(TEXT("child_ref"), ChildRef);
				Result->SetStringField(TEXT("parent_ref"), ParentRef);
				if (!Child || !Parent)
				{
					OutError = TEXT("attachment_actor_not_found");
					Result->SetStringField(TEXT("status"), TEXT("actor_not_found"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				if (bDryRun)
				{
					Result->SetStringField(TEXT("status"), TEXT("dry_run"));
				}
				else
				{
					Child->Modify();
					Child->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
					Result->SetStringField(TEXT("status"), TEXT("attached"));
				}
			}
			else if (Type == TEXT("detach"))
			{
				FString ActorRef;
				AActor* Actor = ResolveOperationActor(World, Op, { TEXT("actor"), TEXT("child"), TEXT("child_actor"), TEXT("id"), TEXT("actor_id") }, ActorRef);
				Result->SetStringField(TEXT("actor_ref"), ActorRef);
				if (!Actor)
				{
					OutError = TEXT("operation_actor_not_found");
					Result->SetStringField(TEXT("status"), TEXT("actor_not_found"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				if (bDryRun)
				{
					Result->SetStringField(TEXT("status"), TEXT("dry_run"));
				}
				else
				{
					Actor->Modify();
					Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
					Result->SetStringField(TEXT("status"), TEXT("detached"));
				}
			}
			else if (Type == TEXT("set_folder"))
			{
				FString ActorRef;
				AActor* Actor = ResolveOperationActor(World, Op, { TEXT("actor"), TEXT("id"), TEXT("actor_id"), TEXT("target_actor") }, ActorRef);
				FString FolderPath;
				TryGetStringAny(Op, { TEXT("folder_path"), TEXT("folder"), TEXT("target_folder") }, FolderPath);
				Result->SetStringField(TEXT("actor_ref"), ActorRef);
				Result->SetStringField(TEXT("folder_path"), NormalizeFolder(FolderPath));
				if (!Actor || FolderPath.TrimStartAndEnd().IsEmpty())
				{
					OutError = !Actor ? TEXT("operation_actor_not_found") : TEXT("folder_path_missing");
					Result->SetStringField(TEXT("status"), !Actor ? TEXT("actor_not_found") : TEXT("folder_path_missing"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				if (bDryRun)
				{
					Result->SetStringField(TEXT("status"), TEXT("dry_run"));
				}
				else
				{
					Actor->Modify();
					Actor->SetFolderPath(FName(*NormalizeFolder(FolderPath)));
					Result->SetStringField(TEXT("status"), TEXT("folder_set"));
				}
			}
			else if (Type == TEXT("add_tag") || Type == TEXT("remove_tag"))
			{
				FString ActorRef;
				AActor* Actor = ResolveOperationActor(World, Op, { TEXT("actor"), TEXT("id"), TEXT("actor_id"), TEXT("target_actor") }, ActorRef);
				TArray<FString> Tags = ReadOperationTags(Op);
				Result->SetStringField(TEXT("actor_ref"), ActorRef);
				TArray<TSharedPtr<FJsonValue>> TagValues;
				for (const FString& Tag : Tags)
				{
					TagValues.Add(MakeShared<FJsonValueString>(Tag));
				}
				Result->SetArrayField(TEXT("tags"), TagValues);
				if (!Actor || Tags.Num() == 0)
				{
					OutError = !Actor ? TEXT("operation_actor_not_found") : TEXT("operation_tags_missing");
					Result->SetStringField(TEXT("status"), !Actor ? TEXT("actor_not_found") : TEXT("tags_missing"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				if (bDryRun)
				{
					Result->SetStringField(TEXT("status"), TEXT("dry_run"));
				}
				else
				{
					Actor->Modify();
					for (const FString& Tag : Tags)
					{
						const FString CleanTag = Tag.TrimStartAndEnd();
						if (CleanTag.IsEmpty())
						{
							continue;
						}
						if (Type == TEXT("add_tag"))
						{
							Actor->Tags.AddUnique(FName(*CleanTag));
						}
						else
						{
							Actor->Tags.Remove(FName(*CleanTag));
						}
					}
					Result->SetStringField(TEXT("status"), Type == TEXT("add_tag") ? TEXT("tag_added") : TEXT("tag_removed"));
				}
			}
			else if (Type == TEXT("align"))
			{
				FString SourceRef;
				FString TargetRef;
				AActor* SourceActor = ResolveOperationActor(World, Op, { TEXT("source_actor"), TEXT("source_id"), TEXT("actor"), TEXT("id") }, SourceRef);
				AActor* TargetActor = ResolveOperationActor(World, Op, { TEXT("target_actor"), TEXT("target_id"), TEXT("target") }, TargetRef);
				FString Method = TEXT("copy_location");
				TryGetStringAny(Op, { TEXT("method") }, Method);
				Method = Method.TrimStartAndEnd().ToLower();
				Result->SetStringField(TEXT("source_actor"), SourceRef);
				Result->SetStringField(TEXT("target_actor"), TargetRef);
				Result->SetStringField(TEXT("method"), Method);
				if (!SourceActor || !TargetActor)
				{
					OutError = TEXT("align_actor_not_found");
					Result->SetStringField(TEXT("status"), TEXT("actor_not_found"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				FVector Offset = FVector::ZeroVector;
				TryGetVectorField(Op, TEXT("offset"), Offset);
				FVector TargetLocation = TargetActor->GetActorLocation();
				if (Method == TEXT("bounds_center") || Method == TEXT("by_bounds") || Method == TEXT("actor_bounds_center"))
				{
					const FBox TargetBounds = TargetActor->GetComponentsBoundingBox(true);
					if (TargetBounds.IsValid != 0)
					{
						TargetLocation = TargetBounds.GetCenter();
					}
				}
				else if (!(Method == TEXT("copy_location") || Method == TEXT("actor_to_actor") || Method == TEXT("location")))
				{
					Result->SetStringField(TEXT("status"), TEXT("report_only"));
					Result->SetStringField(TEXT("reason"), TEXT("complex face/vertex/surface alignment remains delegated to dedicated geometry alignment commands"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					continue;
				}
				if (bDryRun)
				{
					Result->SetStringField(TEXT("status"), TEXT("dry_run"));
				}
				else
				{
					SourceActor->Modify();
					SourceActor->SetActorLocation(TargetLocation + Offset, false, nullptr, ETeleportType::TeleportPhysics);
					Result->SetStringField(TEXT("status"), TEXT("aligned"));
					Result->SetObjectField(TEXT("location"), MakeVectorJson(SourceActor->GetActorLocation()));
				}
			}
			else if (Type == TEXT("screenshot") || Type == TEXT("evidence"))
			{
				Result->SetStringField(TEXT("status"), TEXT("report_only_visual_capture"));
				Result->SetStringField(TEXT("reason"), TEXT("use dedicated screenshot_viewport or screenshot_viewport_buffer for crash-guarded capture"));
			}
			else if (Type == TEXT("convert") || Type == TEXT("migrate") || Type == TEXT("convert_report"))
			{
				Result->SetStringField(TEXT("status"), TEXT("report_only"));
				Result->SetStringField(TEXT("reason"), TEXT("destructive conversion requires dedicated adapter and explicit resource ownership"));
			}
			else if (Type == TEXT("remap_report"))
			{
				Result->SetStringField(TEXT("status"), TEXT("report_only"));
				Result->SetStringField(TEXT("reason"), TEXT("remap_report is diagnostic-only; use remap or retarget for bounded property text remap"));
			}
			else if (Type == TEXT("remap") || Type == TEXT("retarget"))
			{
				FString PropertyPath;
				FString FromText;
				FString ToText;
				TryGetStringAny(Op, { TEXT("property_path"), TEXT("property") }, PropertyPath);
				TryGetStringAny(Op, { TEXT("from"), TEXT("from_text") }, FromText);
				TryGetStringAny(Op, { TEXT("to"), TEXT("to_text") }, ToText);
				double MaxTouchedD = 100.0;
				Op->TryGetNumberField(TEXT("max_resources_touched"), MaxTouchedD);
				const int32 MaxTouched = FMath::Max(0, (int32)MaxTouchedD);
				int32 Matched = 0;
				int32 Touched = 0;
				const FString FolderRoot = GetScopeFolderRoot(Content, Params);
				const FString ManagedTag = GetManagedTag(Content, Params);
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (!Actor || !FolderMatches(Actor->GetFolderPath().ToString(), FolderRoot, true) || !ActorHasTagString(Actor, ManagedTag) || ActorIsProtected(Actor, ProtectionTags))
					{
						continue;
					}
					FString ReadBack;
					FString PropertyClass;
					if (!ExportPropertyText(Actor, PropertyPath, ReadBack, PropertyClass) || !ReadBack.Contains(FromText))
					{
						continue;
					}
					++Matched;
					if (Matched > MaxTouched)
					{
						Result->SetStringField(TEXT("status"), TEXT("degraded_to_report"));
						Result->SetStringField(TEXT("reason"), TEXT("max_resources_touched_exceeded"));
						Result->SetNumberField(TEXT("matched"), Matched);
						Results.Add(MakeShared<FJsonValueObject>(Result));
						return true;
					}
					if (!bDryRun)
					{
						const FString NewText = ReadBack.Replace(*FromText, *ToText, ESearchCase::CaseSensitive);
						TSharedPtr<FJsonObject> PropertyResult;
						FString PropertyError;
						if (!SetObjectPropertyFromJson(Actor, PropertyPath, MakeShared<FJsonValueString>(NewText), World, PropertyResult, PropertyError))
						{
							OutError = PropertyError;
							return false;
						}
					}
					++Touched;
				}
				Result->SetStringField(TEXT("status"), bDryRun ? TEXT("dry_run") : TEXT("applied"));
				Result->SetNumberField(TEXT("matched"), Matched);
				Result->SetNumberField(TEXT("touched"), Touched);
			}
			else if (Type == TEXT("repair") || Type == TEXT("repair_report"))
			{
				Result->SetStringField(TEXT("status"), TEXT("delegated_to_level_content_repair_json"));
			}
			else if (!Type.IsEmpty())
			{
				Result->SetStringField(TEXT("status"), TEXT("unsupported_operation"));
			}
			Results.Add(MakeShared<FJsonValueObject>(Result));
		}
		return true;
	}

	static bool ExecuteTasks(UWorld* World, const TSharedPtr<FJsonObject>& Content, bool bDryRun, TArray<TSharedPtr<FJsonValue>>& Results, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Tasks = nullptr;
		if (!Content.IsValid() || !TryGetArray(Content, TEXT("tasks"), Tasks) || !Tasks)
		{
			return true;
		}
		for (int32 Index = 0; Index < Tasks->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Task = (*Tasks)[Index].IsValid() ? (*Tasks)[Index]->AsObject() : nullptr;
			if (!Task.IsValid())
			{
				continue;
			}
			FString Type;
			TryGetStringAny(Task, { TEXT("type"), TEXT("kind"), TEXT("task") }, Type);
			Type = Type.ToLower();
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("task"), Type);
			Result->SetNumberField(TEXT("task_index"), Index);
			if (Type == TEXT("navmesh_build"))
			{
				if (bDryRun)
				{
					Result->SetStringField(TEXT("status"), TEXT("dry_run"));
				}
				else if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
				{
					NavSys->Build();
					Result->SetStringField(TEXT("status"), TEXT("executed"));
				}
				else
				{
					Result->SetStringField(TEXT("status"), TEXT("feature_unavailable"));
					Result->SetStringField(TEXT("reason"), TEXT("navigation_system_unavailable"));
				}
			}
			else if (Type == TEXT("cleanup_empty_folders"))
			{
				Result->SetStringField(TEXT("status"), bDryRun ? TEXT("dry_run") : TEXT("handled_by_repair_or_existing_folder_cleanup"));
			}
			else if (Type == TEXT("hlod_build") || Type == TEXT("build_hlod") || Type == TEXT("lighting_build") || Type == TEXT("reflection_capture_update") || Type == TEXT("pcg_generate") ||
				Type == TEXT("load_streaming_level_for_edit") || Type == TEXT("unload_streaming_level_after_apply"))
			{
				Result->SetStringField(TEXT("status"), TEXT("feature_unavailable"));
				Result->SetStringField(TEXT("reason"), TEXT("gated_build_task_report_only"));
			}
			else
			{
				Result->SetStringField(TEXT("status"), TEXT("unsupported_task"));
			}
			Results.Add(MakeShared<FJsonValueObject>(Result));
		}
		return true;
	}

	static ECollisionChannel CollisionChannelFromName(const FString& ChannelName, ECollisionChannel DefaultChannel)
	{
		const FString Clean = ChannelName.TrimStartAndEnd().ToLower();
		if (Clean == TEXT("camera"))
		{
			return ECC_Camera;
		}
		if (Clean == TEXT("pawn"))
		{
			return ECC_Pawn;
		}
		if (Clean == TEXT("worldstatic") || Clean == TEXT("world_static"))
		{
			return ECC_WorldStatic;
		}
		if (Clean == TEXT("worlddynamic") || Clean == TEXT("world_dynamic"))
		{
			return ECC_WorldDynamic;
		}
		if (Clean == TEXT("physicsbody") || Clean == TEXT("physics_body"))
		{
			return ECC_PhysicsBody;
		}
		return DefaultChannel;
	}

	static bool ExecuteValidations(UWorld* World, const TSharedPtr<FJsonObject>& Content, TArray<TSharedPtr<FJsonValue>>& Results)
	{
		const TArray<TSharedPtr<FJsonValue>>* Validations = nullptr;
		if (!Content.IsValid() || !TryGetArray(Content, TEXT("validation"), Validations) || !Validations)
		{
			return true;
		}
		for (int32 Index = 0; Index < Validations->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Validation = (*Validations)[Index].IsValid() ? (*Validations)[Index]->AsObject() : nullptr;
			if (!Validation.IsValid())
			{
				continue;
			}
			FString Type;
			TryGetStringAny(Validation, { TEXT("type"), TEXT("kind") }, Type);
			Type = Type.ToLower();
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("validation"), Type);
			Result->SetNumberField(TEXT("validation_index"), Index);
			if (Type == TEXT("actor_exists"))
			{
				FString Id;
				TryGetStringAny(Validation, { TEXT("id"), TEXT("actor_id"), TEXT("actor") }, Id);
				AActor* Actor = FindActorByNameLabelOrManagedId(World, Id);
				Result->SetBoolField(TEXT("passed"), Actor != nullptr);
				Result->SetStringField(TEXT("actor"), Actor ? Actor->GetActorLabel() : FString());
			}
			else if (Type == TEXT("property_readback"))
			{
				FString Id;
				FString PropertyPath;
				TryGetStringAny(Validation, { TEXT("id"), TEXT("actor_id") }, Id);
				TryGetStringAny(Validation, { TEXT("property"), TEXT("property_path") }, PropertyPath);
				AActor* Actor = FindActorByNameLabelOrManagedId(World, Id);
				FString ReadBack;
				FString PropertyClass;
				const bool bOk = Actor && ExportPropertyText(Actor, PropertyPath, ReadBack, PropertyClass);
				Result->SetBoolField(TEXT("passed"), bOk);
				Result->SetStringField(TEXT("value_text"), ReadBack);
			}
			else if (Type == TEXT("trace") || Type == TEXT("line_trace"))
			{
				FVector Start;
				FVector End;
				if (!TryGetVectorField(Validation, TEXT("start"), Start) || !TryGetVectorField(Validation, TEXT("end"), End))
				{
					Result->SetBoolField(TEXT("passed"), false);
					Result->SetStringField(TEXT("status"), TEXT("missing_start_or_end"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					continue;
				}
				FString ChannelName = TEXT("visibility");
				TryGetStringAny(Validation, { TEXT("trace_channel"), TEXT("channel") }, ChannelName);
				FHitResult Hit;
				FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentLevelContentTraceValidation), true);
				const bool bHit = World && World->LineTraceSingleByChannel(Hit, Start, End, CollisionChannelFromName(ChannelName, ECC_Visibility), QueryParams);
				bool bExpectedHit = false;
				const bool bHasExpectedHit = TryGetBoolAny(Validation, { TEXT("expected_hit"), TEXT("should_hit") }, bExpectedHit);
				Result->SetBoolField(TEXT("hit"), bHit);
				Result->SetBoolField(TEXT("passed"), !bHasExpectedHit || bHit == bExpectedHit);
				Result->SetObjectField(TEXT("start"), MakeVectorJson(Start));
				Result->SetObjectField(TEXT("end"), MakeVectorJson(End));
				if (bHit)
				{
					Result->SetStringField(TEXT("hit_actor"), Hit.GetActor() ? Hit.GetActor()->GetActorLabel() : FString());
					Result->SetObjectField(TEXT("hit_location"), MakeVectorJson(Hit.ImpactPoint));
				}
			}
			else if (Type == TEXT("overlap") || Type == TEXT("box_overlap"))
			{
				FVector Center;
				FVector Extent(50.0, 50.0, 50.0);
				if (!TryGetVectorField(Validation, TEXT("center"), Center))
				{
					Result->SetBoolField(TEXT("passed"), false);
					Result->SetStringField(TEXT("status"), TEXT("missing_center"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					continue;
				}
				TryGetVectorField(Validation, TEXT("extent"), Extent);
				TArray<FOverlapResult> Overlaps;
				FCollisionObjectQueryParams ObjectParams;
				ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
				ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);
				ObjectParams.AddObjectTypesToQuery(ECC_PhysicsBody);
				ObjectParams.AddObjectTypesToQuery(ECC_Pawn);
				FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentLevelContentOverlapValidation), true);
				const bool bHasBlockingOverlap = World && World->OverlapMultiByObjectType(Overlaps, Center, FQuat::Identity, ObjectParams, FCollisionShape::MakeBox(Extent), QueryParams);
				TSet<FString> ActorLabels;
				for (const FOverlapResult& Overlap : Overlaps)
				{
					if (AActor* Actor = Overlap.GetActor())
					{
						ActorLabels.Add(Actor->GetActorLabel());
					}
				}
				double MinCount = -1.0;
				double MaxCount = -1.0;
				Validation->TryGetNumberField(TEXT("min_count"), MinCount);
				Validation->TryGetNumberField(TEXT("max_count"), MaxCount);
				const int32 Count = ActorLabels.Num() > 0 ? ActorLabels.Num() : Overlaps.Num();
				bool bPassed = true;
				if (MinCount >= 0.0)
				{
					bPassed = bPassed && Count >= (int32)MinCount;
				}
				if (MaxCount >= 0.0)
				{
					bPassed = bPassed && Count <= (int32)MaxCount;
				}
				Result->SetBoolField(TEXT("has_overlap"), bHasBlockingOverlap);
				Result->SetNumberField(TEXT("overlap_count"), Count);
				Result->SetBoolField(TEXT("passed"), bPassed);
			}
			else if (Type == TEXT("sweep") || Type == TEXT("capsule_sweep") || Type == TEXT("sweep_capsule"))
			{
				FVector Start;
				FVector End;
				if (!TryGetVectorField(Validation, TEXT("start"), Start) || !TryGetVectorField(Validation, TEXT("end"), End))
				{
					Result->SetBoolField(TEXT("passed"), false);
					Result->SetStringField(TEXT("status"), TEXT("missing_start_or_end"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					continue;
				}
				double Radius = 34.0;
				double HalfHeight = 88.0;
				Validation->TryGetNumberField(TEXT("radius"), Radius);
				Validation->TryGetNumberField(TEXT("half_height"), HalfHeight);
				TArray<FHitResult> Hits;
				FCollisionObjectQueryParams ObjectParams;
				ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
				ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);
				FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentLevelContentSweepValidation), true);
				const bool bHit = World && World->SweepMultiByObjectType(Hits, Start, End, FQuat::Identity, ObjectParams, FCollisionShape::MakeCapsule((float)Radius, (float)HalfHeight), QueryParams);
				bool bExpectedHit = false;
				const bool bHasExpectedHit = TryGetBoolAny(Validation, { TEXT("expected_hit"), TEXT("should_hit") }, bExpectedHit);
				Result->SetBoolField(TEXT("hit"), bHit);
				Result->SetNumberField(TEXT("hit_count"), Hits.Num());
				Result->SetBoolField(TEXT("passed"), !bHasExpectedHit || bHit == bExpectedHit);
			}
			else if (Type == TEXT("nav_path") || Type == TEXT("navmesh_path") || Type == TEXT("nav_find_path"))
			{
				FVector Start;
				FVector End;
				if (!TryGetVectorField(Validation, TEXT("start"), Start) || !TryGetVectorField(Validation, TEXT("end"), End))
				{
					Result->SetBoolField(TEXT("passed"), false);
					Result->SetStringField(TEXT("status"), TEXT("missing_start_or_end"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					continue;
				}
				UNavigationSystemV1* NavSys = World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
				if (!NavSys)
				{
					Result->SetBoolField(TEXT("passed"), false);
					Result->SetStringField(TEXT("status"), TEXT("feature_unavailable"));
					Result->SetStringField(TEXT("reason"), TEXT("navigation_system_unavailable"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					continue;
				}
				UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(World, Start, End);
				const bool bPathFound = Path && Path->IsValid();
				bool bExpectedPath = false;
				const bool bHasExpectedPath = TryGetBoolAny(Validation, { TEXT("expected_path_found"), TEXT("should_find_path") }, bExpectedPath);
				Result->SetBoolField(TEXT("path_found"), bPathFound);
				Result->SetBoolField(TEXT("partial"), Path ? Path->IsPartial() : false);
				Result->SetNumberField(TEXT("point_count"), Path ? Path->PathPoints.Num() : 0);
				Result->SetBoolField(TEXT("passed"), !bHasExpectedPath || bPathFound == bExpectedPath);
			}
			else if (Type == TEXT("streaming_loaded") || Type == TEXT("streaming_state"))
			{
				FString LevelName;
				TryGetStringAny(Validation, { TEXT("level"), TEXT("level_name"), TEXT("package"), TEXT("name") }, LevelName);
				bool bFound = false;
				bool bLoaded = false;
				bool bVisible = false;
				if (World)
				{
					for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
					{
						if (!StreamingLevel)
						{
							continue;
						}
						const FString PackageName = StreamingLevel->GetWorldAssetPackageName();
						if (LevelName.IsEmpty() || PackageName.Contains(LevelName, ESearchCase::IgnoreCase) || StreamingLevel->GetName().Contains(LevelName, ESearchCase::IgnoreCase))
						{
							bFound = true;
							bLoaded = StreamingLevel->IsLevelLoaded();
							bVisible = StreamingLevel->IsLevelVisible();
							Result->SetStringField(TEXT("matched_level"), PackageName);
							break;
						}
					}
				}
				bool bExpectedLoaded = false;
				bool bExpectedVisible = false;
				const bool bHasExpectedLoaded = TryGetBoolAny(Validation, { TEXT("expected_loaded"), TEXT("loaded") }, bExpectedLoaded);
				const bool bHasExpectedVisible = TryGetBoolAny(Validation, { TEXT("expected_visible"), TEXT("visible") }, bExpectedVisible);
				bool bPassed = bFound;
				if (bHasExpectedLoaded)
				{
					bPassed = bPassed && bLoaded == bExpectedLoaded;
				}
				if (bHasExpectedVisible)
				{
					bPassed = bPassed && bVisible == bExpectedVisible;
				}
				Result->SetBoolField(TEXT("found"), bFound);
				Result->SetBoolField(TEXT("loaded"), bLoaded);
				Result->SetBoolField(TEXT("visible"), bVisible);
				Result->SetBoolField(TEXT("passed"), bPassed);
				if (!bFound)
				{
					Result->SetStringField(TEXT("status"), TEXT("level_not_found"));
				}
			}
			else if (Type == TEXT("data_layer_membership"))
			{
				FString Id;
				TryGetStringAny(Validation, { TEXT("id"), TEXT("actor_id"), TEXT("actor") }, Id);
				AActor* Actor = FindActorByNameLabelOrManagedId(World, Id);
				TArray<FString> ExpectedLayers = ReadStringArray(Validation, TEXT("expected_layers"));
				ExpectedLayers.Append(ReadStringArray(Validation, TEXT("data_layers")));
				FString SingleLayer;
				if (TryGetStringAny(Validation, { TEXT("data_layer"), TEXT("expected_layer") }, SingleLayer) && !SingleLayer.TrimStartAndEnd().IsEmpty())
				{
					ExpectedLayers.Add(SingleLayer.TrimStartAndEnd());
				}
				const bool bPassed = Actor && ActorMatchesDataLayerFallback(Actor, ExpectedLayers);
				Result->SetBoolField(TEXT("passed"), bPassed);
				Result->SetStringField(TEXT("actor"), Actor ? Actor->GetActorLabel() : FString());
				Result->SetStringField(TEXT("status"), TEXT("checked_with_reflection_or_datalayer_tags"));
			}
			else if (Type == TEXT("feature_gate"))
			{
				TSharedPtr<FJsonObject> Gates = BuildFeatureGateReport(Content);
				TArray<FString> RequestedFeatures = ReadStringArray(Validation, TEXT("features"));
				FString SingleFeature;
				if (TryGetStringAny(Validation, { TEXT("feature"), TEXT("gate"), TEXT("name") }, SingleFeature) && !SingleFeature.TrimStartAndEnd().IsEmpty())
				{
					RequestedFeatures.Add(SingleFeature.TrimStartAndEnd());
				}

				bool bRequired = false;
				TryGetBoolAny(Validation, { TEXT("required"), TEXT("must_be_available") }, bRequired);

				bool bAllAvailable = true;
				TArray<TSharedPtr<FJsonValue>> FeatureResults;
				for (const FString& RequestedFeature : RequestedFeatures)
				{
					const FString Feature = RequestedFeature.TrimStartAndEnd().ToLower();
					FString GateKey = Feature;
					if (Feature == TEXT("world_partition") || Feature == TEXT("runtime_region"))
					{
						GateKey = TEXT("world_partition_runtime_region_write");
					}
					else if (Feature == TEXT("streaming") || Feature == TEXT("streaming_level"))
					{
						GateKey = TEXT("streaming_level_mutation");
					}
					else if (Feature == TEXT("data_layer"))
					{
						GateKey = TEXT("data_layer_mutation");
					}
					else if (Feature == TEXT("hlod"))
					{
						GateKey = TEXT("hlod_build");
					}
					else if (Feature == TEXT("lighting"))
					{
						GateKey = TEXT("lighting_build");
					}
					else if (Feature == TEXT("pcg"))
					{
						GateKey = TEXT("pcg_generate");
					}

					bool bAvailable = false;
					if (Feature == TEXT("navigation") || Feature == TEXT("nav") || Feature == TEXT("navmesh"))
					{
						bAvailable = World && FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) != nullptr;
						GateKey = TEXT("navigation_system");
					}
					else
					{
						Gates->TryGetBoolField(GateKey, bAvailable);
					}
					bAllAvailable = bAllAvailable && bAvailable;

					TSharedPtr<FJsonObject> FeatureResult = MakeShared<FJsonObject>();
					FeatureResult->SetStringField(TEXT("feature"), Feature);
					FeatureResult->SetStringField(TEXT("gate"), GateKey);
					FeatureResult->SetBoolField(TEXT("available"), bAvailable);
					FeatureResults.Add(MakeShared<FJsonValueObject>(FeatureResult));
				}

				const bool bPassed = RequestedFeatures.Num() == 0 || !bRequired || bAllAvailable;
				Result->SetBoolField(TEXT("passed"), bPassed);
				Result->SetBoolField(TEXT("required"), bRequired);
				Result->SetObjectField(TEXT("feature_gates"), Gates);
				Result->SetArrayField(TEXT("features"), FeatureResults);
				Result->SetStringField(TEXT("status"), RequestedFeatures.Num() == 0 ? TEXT("feature_gate_report") : (bAllAvailable ? TEXT("available") : (bRequired ? TEXT("feature_unavailable") : TEXT("feature_unavailable_optional"))));
			}
			else if (Type == TEXT("hlod_assignment") || Type == TEXT("lod_state"))
			{
				Result->SetBoolField(TEXT("passed"), false);
				Result->SetStringField(TEXT("status"), TEXT("feature_unavailable_report_only"));
			}
			else
			{
				Result->SetBoolField(TEXT("passed"), true);
				Result->SetStringField(TEXT("status"), TEXT("structural_only"));
			}
			Results.Add(MakeShared<FJsonValueObject>(Result));
		}
		return true;
	}

	static TSharedPtr<FJsonObject> DiffAgainstWorld(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, bool bActorOnly)
	{
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetStringField(TEXT("schema"), DiffSchema);
		Diff->SetBoolField(TEXT("actor_only"), bActorOnly);
		TArray<TSharedPtr<FJsonValue>> Creates;
		TArray<TSharedPtr<FJsonValue>> Updates;
		TArray<TSharedPtr<FJsonValue>> Deletes;
		TArray<TSharedPtr<FJsonObject>> Resources;
		CollectResourceObjects(Content, Resources);
		int32 CurrentCount = 0;
		TSet<FString> TargetIds;
		for (const TSharedPtr<FJsonObject>& Resource : Resources)
		{
			const FString Id = GetResourceId(Resource);
			if (!Id.IsEmpty())
			{
				TargetIds.Add(Id);
			}
			AActor* Actor = FindActorByNameLabelOrManagedId(World, Id);
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("id"), Id);
			Item->SetStringField(TEXT("kind"), GetResourceKind(Resource));
			if (!Actor)
			{
				Item->SetStringField(TEXT("diff_type"), TEXT("create"));
				Creates.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}
			++CurrentCount;
			bool bChanged = false;
			const FString ClassPath = GetClassPathForResource(Resource);
			if (!ClassPath.IsEmpty() && Actor->GetClass() && !Actor->GetClass()->GetPathName().Equals(ClassPath, ESearchCase::IgnoreCase))
			{
				Item->SetStringField(TEXT("class_current"), Actor->GetClass()->GetPathName());
				Item->SetStringField(TEXT("class_target"), ClassPath);
				bChanged = true;
			}
			FString Folder;
			if (TryGetStringAny(Resource, { TEXT("folder_path") }, Folder) && !NormalizeFolder(Actor->GetFolderPath().ToString()).Equals(NormalizeFolder(Folder), ESearchCase::IgnoreCase))
			{
				Item->SetStringField(TEXT("folder_current"), Actor->GetFolderPath().ToString());
				Item->SetStringField(TEXT("folder_target"), NormalizeFolder(Folder));
				bChanged = true;
			}
			FString Label;
			if (TryGetStringAny(Resource, { TEXT("label") }, Label) && !Label.TrimStartAndEnd().IsEmpty() && !Actor->GetActorLabel().Equals(Label.TrimStartAndEnd(), ESearchCase::CaseSensitive))
			{
				Item->SetStringField(TEXT("label_current"), Actor->GetActorLabel());
				Item->SetStringField(TEXT("label_target"), Label.TrimStartAndEnd());
				bChanged = true;
			}
			TSharedPtr<FJsonObject> Transform;
			if (TryGetObject(Resource, TEXT("transform"), Transform))
			{
				FVector TargetLocation;
				if (TryGetVectorField(Transform, TEXT("location"), TargetLocation) && !Actor->GetActorLocation().Equals(TargetLocation, 0.1))
				{
					Item->SetObjectField(TEXT("location_current"), MakeVectorJson(Actor->GetActorLocation()));
					Item->SetObjectField(TEXT("location_target"), MakeVectorJson(TargetLocation));
					bChanged = true;
				}
				FRotator TargetRotation;
				if (TryGetRotatorField(Transform, TEXT("rotation"), TargetRotation) && !Actor->GetActorRotation().Equals(TargetRotation, 0.1))
				{
					Item->SetObjectField(TEXT("rotation_current"), MakeRotatorJson(Actor->GetActorRotation()));
					Item->SetObjectField(TEXT("rotation_target"), MakeRotatorJson(TargetRotation));
					bChanged = true;
				}
				FVector TargetScale;
				if (TryGetVectorField(Transform, TEXT("scale"), TargetScale) && !Actor->GetActorScale3D().Equals(TargetScale, 0.001))
				{
					Item->SetObjectField(TEXT("scale_current"), MakeVectorJson(Actor->GetActorScale3D()));
					Item->SetObjectField(TEXT("scale_target"), MakeVectorJson(TargetScale));
					bChanged = true;
				}
			}
			for (const FString& Tag : ReadTags(Resource))
			{
				if (!Tag.TrimStartAndEnd().IsEmpty() && !ActorHasTagString(Actor, Tag.TrimStartAndEnd()))
				{
					Item->SetStringField(TEXT("missing_tag"), Tag.TrimStartAndEnd());
					bChanged = true;
					break;
				}
			}
			TSharedPtr<FJsonObject> Attachments;
			if (TryGetObject(Resource, TEXT("attachments"), Attachments))
			{
				FString ParentId;
				if (TryGetStringAny(Attachments, { TEXT("parent_actor"), TEXT("parent_id") }, ParentId) && !ParentId.TrimStartAndEnd().IsEmpty())
				{
					AActor* Parent = Actor->GetAttachParentActor();
					const FString CurrentParentId = Parent ? ActorStableId(Parent) : FString();
					if (!CurrentParentId.Equals(ParentId.TrimStartAndEnd(), ESearchCase::IgnoreCase))
					{
						Item->SetStringField(TEXT("parent_current"), CurrentParentId);
						Item->SetStringField(TEXT("parent_target"), ParentId.TrimStartAndEnd());
						bChanged = true;
					}
				}
			}
			if (bChanged)
			{
				Item->SetStringField(TEXT("diff_type"), TEXT("update"));
				Updates.Add(MakeShared<FJsonValueObject>(Item));
			}
		}

		const FString DeletePolicy = Params.IsValid() && Params->HasField(TEXT("delete_policy")) ? Params->GetStringField(TEXT("delete_policy")).ToLower() : FString();
		if (DeletePolicy == TEXT("only_managed_missing") || DeletePolicy == TEXT("replace_scope_managed"))
		{
			const FString FolderRoot = GetScopeFolderRoot(Content, Params);
			const FString ManagedTag = GetManagedTag(Content, Params);
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!Actor || !FolderMatches(Actor->GetFolderPath().ToString(), FolderRoot, true) || !ActorHasTagString(Actor, ManagedTag))
				{
					continue;
				}
				const FString Id = GetManagedId(Actor);
				if (!Id.IsEmpty() && !TargetIds.Contains(Id))
				{
					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("id"), Id);
					Item->SetStringField(TEXT("actor"), Actor->GetActorLabel());
					Item->SetStringField(TEXT("diff_type"), TEXT("delete"));
					Deletes.Add(MakeShared<FJsonValueObject>(Item));
				}
			}
		}

		TArray<TSharedPtr<FJsonValue>> Diffs;
		Diffs.Append(Creates);
		Diffs.Append(Updates);
		Diffs.Append(Deletes);
		Diff->SetArrayField(TEXT("create"), Creates);
		Diff->SetArrayField(TEXT("update"), Updates);
		Diff->SetArrayField(TEXT("delete"), Deletes);
		Diff->SetArrayField(TEXT("diffs"), Diffs);
		Diff->SetNumberField(TEXT("create_count"), Creates.Num());
		Diff->SetNumberField(TEXT("update_count"), Updates.Num());
		Diff->SetNumberField(TEXT("delete_count"), Deletes.Num());
		Diff->SetNumberField(TEXT("current_count"), CurrentCount);
		Diff->SetNumberField(TEXT("target_count"), Resources.Num());
		Diff->SetBoolField(TEXT("empty"), Creates.Num() == 0 && Updates.Num() == 0 && Deletes.Num() == 0);
		Diff->SetObjectField(TEXT("feature_gates"), BuildFeatureGateReport(Content));
		return Diff;
	}

	TSharedPtr<FJsonObject> BuildRepairDiagnosis(UWorld* World, const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("schema"), RepairSchema);
		FString Prefix;
		if (Params.IsValid())
		{
			TryGetStringAny(Params, { TEXT("folder_path_prefix"), TEXT("folder_root") }, Prefix);
		}
		Prefix = NormalizeFolder(Prefix);
		TArray<TSharedPtr<FJsonValue>> EmptyFolders;
		FActorFolders& ActorFolders = FActorFolders::Get();
		TArray<FFolder> AllFolders;
		ActorFolders.ForEachFolder(*World, [&AllFolders](const FFolder& Folder)
		{
			AllFolders.Add(Folder);
			return true;
		});
		TSet<FString> UsedFolders;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			const FString FolderPath = Actor->GetFolderPath().ToString();
			if (!FolderPath.IsEmpty())
			{
				UsedFolders.Add(NormalizeFolder(FolderPath));
			}
		}
		for (const FFolder& Folder : AllFolders)
		{
			const FString FolderPath = NormalizeFolder(Folder.ToString());
			if (!FolderMatches(FolderPath, Prefix, true))
			{
				continue;
			}
			if (!UsedFolders.Contains(FolderPath))
			{
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("repair"), TEXT("empty_folder"));
				Item->SetStringField(TEXT("folder_path"), FolderPath);
				Item->SetStringField(TEXT("safety"), TEXT("safe"));
				EmptyFolders.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		Data->SetArrayField(TEXT("diagnostics"), EmptyFolders);
		Data->SetNumberField(TEXT("diagnostic_count"), EmptyFolders.Num());
		Data->SetObjectField(TEXT("feature_gates"), BuildFeatureGateReport(nullptr));
		return Data;
	}

	TSharedPtr<FJsonObject> MergeThreeWay(const TSharedPtr<FJsonObject>& Base, const TSharedPtr<FJsonObject>& Current, const TSharedPtr<FJsonObject>& Target)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), MergeSchema);
		TArray<TSharedPtr<FJsonValue>> Conflicts;
		TSharedPtr<FJsonObject> Merged = MakeShared<FJsonObject>();
		Merged->SetStringField(TEXT("schema"), LevelContentSchema);

		auto ResourceMap = [](const TSharedPtr<FJsonObject>& Content)
		{
			TMap<FString, TSharedPtr<FJsonObject>> Map;
			TArray<TSharedPtr<FJsonObject>> Resources;
			CollectResourceObjects(Content, Resources);
			for (const TSharedPtr<FJsonObject>& Resource : Resources)
			{
				const FString Id = GetResourceId(Resource);
				if (!Id.IsEmpty())
				{
					Map.Add(Id, Resource);
				}
			}
			return Map;
		};

		const TMap<FString, TSharedPtr<FJsonObject>> BaseMap = ResourceMap(Base);
		const TMap<FString, TSharedPtr<FJsonObject>> CurrentMap = ResourceMap(Current);
		const TMap<FString, TSharedPtr<FJsonObject>> TargetMap = ResourceMap(Target);
		TSet<FString> AllIds;
		for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : BaseMap) AllIds.Add(Pair.Key);
		for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : CurrentMap) AllIds.Add(Pair.Key);
		for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : TargetMap) AllIds.Add(Pair.Key);

		TArray<TSharedPtr<FJsonValue>> MergedResources;
		for (const FString& Id : AllIds)
		{
			const FString BaseText = BaseMap.Contains(Id) ? JsonToString(BaseMap[Id], false) : FString();
			const FString CurrentText = CurrentMap.Contains(Id) ? JsonToString(CurrentMap[Id], false) : FString();
			const FString TargetText = TargetMap.Contains(Id) ? JsonToString(TargetMap[Id], false) : FString();
			if (!BaseText.Equals(CurrentText) && !BaseText.Equals(TargetText) && !CurrentText.Equals(TargetText))
			{
				TSharedPtr<FJsonObject> Conflict = MakeShared<FJsonObject>();
				Conflict->SetStringField(TEXT("id"), Id);
				Conflict->SetStringField(TEXT("base"), BaseText);
				Conflict->SetStringField(TEXT("current"), CurrentText);
				Conflict->SetStringField(TEXT("target"), TargetText);
				Conflicts.Add(MakeShared<FJsonValueObject>(Conflict));
				continue;
			}
			TSharedPtr<FJsonObject> Chosen = TargetMap.Contains(Id) ? TargetMap[Id] : (CurrentMap.Contains(Id) ? CurrentMap[Id] : BaseMap.FindRef(Id));
			if (Chosen.IsValid())
			{
				MergedResources.Add(MakeShared<FJsonValueObject>(Chosen));
			}
		}
		Merged->SetArrayField(TEXT("resources"), MergedResources);
		Result->SetObjectField(TEXT("merged_json"), Merged);
		Result->SetArrayField(TEXT("conflicts"), Conflicts);
		Result->SetNumberField(TEXT("conflict_count"), Conflicts.Num());
		Result->SetBoolField(TEXT("can_apply"), Conflicts.Num() == 0);
		return Result;
	}
}

bool FUeAgentHttpServer::ExecuteLevelContentQueryJsonInternal(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError, bool bActorOnly) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TSharedPtr<FJsonObject> Query;
	if (!UeAgentLevelContentJson::QueryLevelContent(World, Ctx.Params, Query, bActorOnly, false))
	{
		OutError = TEXT("level_content_query_failed");
		return false;
	}
	OutData = Query;
	return UeAgentLevelContentJson::SaveJsonIfRequested(Query, Ctx.Params, OutData, OutError);
}

bool FUeAgentHttpServer::ExecuteLevelContentSnapshotJsonInternal(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError, bool bActorOnly) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TSharedPtr<FJsonObject> Snapshot;
	if (!UeAgentLevelContentJson::QueryLevelContent(World, Ctx.Params, Snapshot, bActorOnly, true))
	{
		OutError = TEXT("level_content_snapshot_failed");
		return false;
	}
	OutData = Snapshot;
	return UeAgentLevelContentJson::SaveJsonIfRequested(Snapshot, Ctx.Params, OutData, OutError);
}

bool FUeAgentHttpServer::ExecuteLevelContentValidateJsonInternal(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError, bool bActorOnly) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TSharedPtr<FJsonObject> Content;
	FString Source;
	if (!UeAgentLevelContentJson::ResolveContentObject(Ctx.Params, Content, Source, OutError))
	{
		return false;
	}
	UeAgentLevelContentJson::ValidateLevelContent(World, Content, Ctx.Params, bActorOnly, OutData);
	OutData->SetStringField(TEXT("source"), Source);
	return true;
}

bool FUeAgentHttpServer::CmdLevelContentPlanJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TSharedPtr<FJsonObject> Content;
	FString Source;
	if (!UeAgentLevelContentJson::ResolveContentObject(Ctx.Params, Content, Source, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Validation;
	UeAgentLevelContentJson::ValidateLevelContent(World, Content, Ctx.Params, false, Validation);
	OutData = UeAgentLevelContentJson::BuildPlan(World, Content, Ctx.Params, false);
	OutData->SetStringField(TEXT("source"), Source);
	OutData->SetObjectField(TEXT("validation"), Validation);
	OutData->SetBoolField(TEXT("can_apply"), Validation.IsValid() && Validation->GetBoolField(TEXT("valid")));
	return true;
}

bool FUeAgentHttpServer::ExecuteLevelContentApplyJsonInternal(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError, bool bActorOnly) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TSharedPtr<FJsonObject> Content;
	FString Source;
	if (!UeAgentLevelContentJson::ResolveContentObject(Ctx.Params, Content, Source, OutError))
	{
		return false;
	}

	bool bDryRun = false;
	UeAgentLevelContentJson::TryGetBoolAny(Ctx.Params, { TEXT("dry_run") }, bDryRun);
	bool bPreflight = true;
	UeAgentLevelContentJson::TryGetBoolAny(Ctx.Params, { TEXT("preflight") }, bPreflight);
	bool bTransaction = true;
	UeAgentLevelContentJson::TryGetBoolAny(Ctx.Params, { TEXT("transaction") }, bTransaction);
	FString Mode = TEXT("upsert");
	UeAgentLevelContentJson::TryGetStringAny(Ctx.Params, { TEXT("mode") }, Mode);
	Mode = Mode.TrimStartAndEnd().ToLower();
	if (Mode.IsEmpty())
	{
		Mode = TEXT("upsert");
	}

	TSharedPtr<FJsonObject> Validation;
	UeAgentLevelContentJson::ValidateLevelContent(World, Content, Ctx.Params, bActorOnly, Validation);
	if (bPreflight && Validation.IsValid() && !Validation->GetBoolField(TEXT("valid")))
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("schema"), UeAgentLevelContentJson::ApplySchema);
		OutData->SetStringField(TEXT("source"), Source);
		OutData->SetStringField(TEXT("status"), TEXT("preflight_failed"));
		OutData->SetObjectField(TEXT("validation"), Validation);
		OutError = TEXT("level_content_preflight_failed");
		return false;
	}

	if (bDryRun)
	{
		OutData = UeAgentLevelContentJson::BuildPlan(World, Content, Ctx.Params, bActorOnly);
		OutData->SetStringField(TEXT("source"), Source);
		OutData->SetBoolField(TEXT("dry_run"), true);
		OutData->SetObjectField(TEXT("validation"), Validation);
		return true;
	}

	TArray<TSharedPtr<FJsonObject>> Resources;
	UeAgentLevelContentJson::CollectResourceObjects(Content, Resources);
	TArray<TSharedPtr<FJsonValue>> Results;
	TArray<TSharedPtr<FJsonValue>> AttachmentResults;
	TArray<TSharedPtr<FJsonValue>> OperationResults;
	TArray<TSharedPtr<FJsonValue>> TaskResults;
	TArray<TSharedPtr<FJsonValue>> ValidationResults;
	TSharedPtr<FJsonObject> EvidenceResults;
	TMap<FString, AActor*> ActorById;
	TSet<FString> TargetIds;
	const TArray<FString> ProtectionTags = UeAgentLevelContentJson::ReadProtectionTags(Content);
	bool bMayNeedRollback = false;

	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UeAgentInterface", "LevelContentApplyJsonTx", "UeAgentInterface Level Content Apply JSON"));
	}

	bool bOk = true;
	FString LocalError;
	for (const TSharedPtr<FJsonObject>& Resource : Resources)
	{
		const FString Id = UeAgentLevelContentJson::GetResourceId(Resource);
		if (!Id.IsEmpty())
		{
			TargetIds.Add(Id);
		}
		TSharedPtr<FJsonObject> Result;
		if (!UeAgentLevelContentJson::ApplyResource(World, Content, Ctx.Params, Resource, Mode, ProtectionTags, ActorById, Result, LocalError))
		{
			Results.Add(MakeShared<FJsonValueObject>(Result));
			bOk = false;
			break;
		}
		bMayNeedRollback = true;
		Results.Add(MakeShared<FJsonValueObject>(Result));
	}

	if (bOk)
	{
		bOk = UeAgentLevelContentJson::ApplyAttachments(World, Resources, ActorById, AttachmentResults, LocalError);
	}

	FString DeletePolicy;
	UeAgentLevelContentJson::TryGetStringAny(Ctx.Params, { TEXT("delete_policy") }, DeletePolicy);
	DeletePolicy = DeletePolicy.ToLower();
	if (bOk && (Mode == TEXT("replace_scope") || DeletePolicy == TEXT("only_managed_missing") || DeletePolicy == TEXT("replace_scope_managed")))
	{
		bOk = UeAgentLevelContentJson::DeleteManagedMissing(World, Content, Ctx.Params, TargetIds, ProtectionTags, false, Results, LocalError);
	}

	if (bOk)
	{
		bOk = UeAgentLevelContentJson::ExecuteOperations(World, Content, Ctx.Params, false, ProtectionTags, OperationResults, LocalError);
	}
	if (bOk)
	{
		bOk = UeAgentLevelContentJson::ExecuteTasks(World, Content, false, TaskResults, LocalError);
	}
	if (bOk)
	{
		UeAgentLevelContentJson::ExecuteValidations(World, Content, ValidationResults);
	}
	if (bOk)
	{
		FString EvidenceError;
		EvidenceResults = UeAgentLevelContentJson::BuildEvidenceReport(World, Content, Ctx.Params, TEXT("post_apply"), false, true, EvidenceError);
		if (!EvidenceError.IsEmpty())
		{
			LocalError = EvidenceError;
			bOk = false;
		}
	}

	if (!bOk && Transaction && bMayNeedRollback)
	{
		Transaction.Reset();
		if (GEditor)
		{
			GEditor->UndoTransaction();
		}
	}
	else if (!bOk && Transaction)
	{
		Transaction->Cancel();
		Transaction.Reset();
	}
	else
	{
		Transaction.Reset();
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), UeAgentLevelContentJson::ApplySchema);
	OutData->SetStringField(TEXT("source"), Source);
	OutData->SetStringField(TEXT("status"), bOk ? TEXT("applied") : TEXT("failed"));
	OutData->SetBoolField(TEXT("success"), bOk);
	OutData->SetBoolField(TEXT("dry_run"), false);
	OutData->SetBoolField(TEXT("transaction"), bTransaction);
	OutData->SetBoolField(TEXT("rollback_attempted"), !bOk && bTransaction);
	OutData->SetObjectField(TEXT("validation"), Validation);
	OutData->SetArrayField(TEXT("results"), Results);
	OutData->SetArrayField(TEXT("resource_results"), Results);
	OutData->SetArrayField(TEXT("attachment_results"), AttachmentResults);
	OutData->SetArrayField(TEXT("operation_results"), OperationResults);
	OutData->SetArrayField(TEXT("task_results"), TaskResults);
	OutData->SetArrayField(TEXT("validation_results"), ValidationResults);
	OutData->SetBoolField(TEXT("saved"), false);
	if (EvidenceResults.IsValid())
	{
		OutData->SetObjectField(TEXT("evidence_results"), EvidenceResults);
	}
	OutData->SetObjectField(TEXT("feature_gates"), UeAgentLevelContentJson::BuildFeatureGateReport(Content));
	if (!bOk)
	{
		OutData->SetStringField(TEXT("error"), LocalError);
		OutError = LocalError.IsEmpty() ? TEXT("level_content_apply_failed") : LocalError;
		return false;
	}

	bool bSaveLevel = false;
	UeAgentLevelContentJson::TryGetBoolAny(Ctx.Params, { TEXT("save_level") }, bSaveLevel);
	TSharedPtr<FJsonObject> SavePolicy;
	if (UeAgentLevelContentJson::TryGetObject(Content, TEXT("save_policy"), SavePolicy))
	{
		UeAgentLevelContentJson::TryGetBoolAny(SavePolicy, { TEXT("save_level") }, bSaveLevel);
	}
	if (bSaveLevel)
	{
		FString SaveError;
		TSharedPtr<FJsonObject> SaveData = MakeShared<FJsonObject>();
		FUeAgentRequestContext SaveCtx = Ctx;
		SaveCtx.Params = MakeShared<FJsonObject>();
		const bool bSaved = CmdSaveCurrentLevel(SaveCtx, SaveData, SaveError);
		OutData->SetBoolField(TEXT("saved"), bSaved);
		OutData->SetBoolField(TEXT("save_level_ok"), bSaved);
		if (!bSaved)
		{
			OutData->SetStringField(TEXT("save_level_error"), SaveError);
		}
	}
	return true;
}

bool FUeAgentHttpServer::ExecuteLevelContentDiffJsonInternal(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError, bool bActorOnly) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TSharedPtr<FJsonObject> Content;
	FString Source;
	if (!UeAgentLevelContentJson::ResolveContentObject(Ctx.Params, Content, Source, OutError))
	{
		return false;
	}
	OutData = UeAgentLevelContentJson::DiffAgainstWorld(World, Content, Ctx.Params, bActorOnly);
	OutData->SetStringField(TEXT("source"), Source);
	return true;
}

bool FUeAgentHttpServer::CmdLevelContentDeleteScope(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TSharedPtr<FJsonObject> Content = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Scope = MakeShared<FJsonObject>();
	FString FolderRoot;
	UeAgentLevelContentJson::TryGetStringAny(Ctx.Params, { TEXT("folder_root"), TEXT("folder_path") }, FolderRoot);
	Scope->SetStringField(TEXT("folder_root"), UeAgentLevelContentJson::NormalizeFolder(FolderRoot));
	FString ManagedTag = TEXT("UAI.Managed");
	UeAgentLevelContentJson::TryGetStringAny(Ctx.Params, { TEXT("managed_tag") }, ManagedTag);
	Scope->SetStringField(TEXT("managed_tag"), ManagedTag);
	Content->SetObjectField(TEXT("scope"), Scope);

	bool bDryRun = true;
	UeAgentLevelContentJson::TryGetBoolAny(Ctx.Params, { TEXT("dry_run") }, bDryRun);
	TArray<TSharedPtr<FJsonValue>> Results;
	TSet<FString> TargetIds;
	TArray<FString> ProtectionTags = UeAgentLevelContentJson::ReadStringArray(Ctx.Params, TEXT("protected_tags"));
	if (!UeAgentLevelContentJson::DeleteManagedMissing(World, Content, Ctx.Params, TargetIds, ProtectionTags, bDryRun, Results, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), UeAgentLevelContentJson::DeleteScopeSchema);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetStringField(TEXT("folder_root"), UeAgentLevelContentJson::NormalizeFolder(FolderRoot));
	OutData->SetStringField(TEXT("managed_tag"), ManagedTag);
	OutData->SetArrayField(TEXT("results"), Results);
	OutData->SetNumberField(TEXT("matched_count"), Results.Num());
	TArray<TSharedPtr<FJsonValue>> Actors;
	TArray<TSharedPtr<FJsonValue>> Blocked;
	int32 DeletedCount = 0;
	for (const TSharedPtr<FJsonValue>& ResultValue : Results)
	{
		const TSharedPtr<FJsonObject> ResultObject = ResultValue.IsValid() ? ResultValue->AsObject() : nullptr;
		if (!ResultObject.IsValid())
		{
			continue;
		}
		const FString Status = ResultObject->GetStringField(TEXT("status"));
		if (Status == TEXT("blocked_by_protection"))
		{
			Blocked.Add(ResultValue);
			continue;
		}
		FString ActorLabel;
		if (UeAgentLevelContentJson::TryGetStringAny(ResultObject, { TEXT("actor") }, ActorLabel))
		{
			Actors.Add(MakeShared<FJsonValueString>(ActorLabel));
		}
		if (Status == TEXT("deleted"))
		{
			++DeletedCount;
		}
	}
	OutData->SetNumberField(TEXT("deleted_count"), DeletedCount);
	OutData->SetArrayField(TEXT("actors"), Actors);
	OutData->SetArrayField(TEXT("blocked"), Blocked);
	return true;
}

