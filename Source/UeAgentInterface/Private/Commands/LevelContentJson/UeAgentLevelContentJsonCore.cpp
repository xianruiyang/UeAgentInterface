// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentLevelContentJsonCore.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchAuthoring.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Utilities/UeAgentAuthoringFileIO.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/UeAgentStructuredAuthoringIO.h"
#include "Utilities/UeAgentSpaceContract.h"
#include "Utilities/AuthoringExport/UeAgentExportPublisher.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <initializer_list>

namespace UeAgentLevelContentJson
{
	const TCHAR* LevelContentSchema = TEXT("uai.level_content.v1");
	const TCHAR* QuerySchema = TEXT("uai.level_content.query.v1");
	const TCHAR* SnapshotSchema = TEXT("uai.level_content.snapshot.v1");
	const TCHAR* ValidationSchema = TEXT("uai.level_content.validation.v1");
	const TCHAR* PlanSchema = TEXT("uai.level_content.plan.v1");
	const TCHAR* ApplySchema = TEXT("uai.level_content.apply_result.v1");
	const TCHAR* DiffSchema = TEXT("uai.level_content.diff.v1");
	const TCHAR* DeleteScopeSchema = TEXT("uai.level_content.delete_scope.v1");
	const TCHAR* MergeSchema = TEXT("uai.level_content.merge.v1");
	const TCHAR* RepairSchema = TEXT("uai.level_content.repair.v1");

	FString JsonToString(const TSharedPtr<FJsonObject>& Obj, bool bPretty)
	{
		FString Out;
		if (!Obj.IsValid())
		{
			return Out;
		}
		UeAgentJsonDiagnostics::SerializeJsonObjectCompact(Obj, Out);
		return Out;
	}

	FString JsonValueToString(const TSharedPtr<FJsonValue>& Value)
	{
		FString Out;
		if (!Value.IsValid())
		{
			return Out;
		}
		UeAgentJsonDiagnostics::SerializeJsonValueCompact(Value, Out);
		return Out;
	}

	bool ParseJsonObject(const FString& Text, TSharedPtr<FJsonObject>& OutObj, FString& OutError)
	{
		TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
		{
			OutError = TEXT("json_parse_failed");
			return false;
		}
		return true;
	}

	TSharedPtr<FJsonObject> MakeIssue(const FString& Severity, const FString& Code, const FString& Path, const FString& Message)
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

	bool HasErrorIssue(const TArray<TSharedPtr<FJsonValue>>& Issues)
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

	bool IsTruthyString(const FString& Text)
	{
		return Text.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("1"));
	}

	TSharedPtr<FJsonObject> MakeVectorJson(const FVector& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		return Obj;
	}

	TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), Value.Pitch);
		Obj->SetNumberField(TEXT("yaw"), Value.Yaw);
		Obj->SetNumberField(TEXT("roll"), Value.Roll);
		return Obj;
	}

	TSharedPtr<FJsonObject> MakeSpaceContractJson(const FString& Subject, const FString& Frame)
	{
		return UeAgentSpaceContract::Make3D(Subject, Frame);
	}

	FString NormalizeFolder(const FString& Folder)
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

	FString ResolveFilePath(const FString& Path)
	{
		FString CleanPath = Path;
		CleanPath.TrimStartAndEndInline();
		if (CleanPath.IsEmpty() || FPaths::IsRelative(CleanPath) == false)
		{
			return CleanPath;
		}
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), CleanPath);
	}

	FString ResolveOutputAuthoringFilePath(const FString& Path)
	{
		FString Resolved = ResolveFilePath(Path);
		if (FPaths::GetExtension(Resolved, false).Equals(TEXT("json"), ESearchCase::IgnoreCase))
		{
			Resolved = FPaths::ChangeExtension(Resolved, TEXT("hjson"));
		}
		return Resolved;
	}

	bool IsLegacyJsonFilePath(const FString& Path)
	{
		return FPaths::GetExtension(Path, false).Equals(TEXT("json"), ESearchCase::IgnoreCase);
	}

	bool SaveJsonIfRequested(const TSharedPtr<FJsonObject>& Obj, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, FString& OutError)
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

		OutputFile = ResolveOutputAuthoringFilePath(OutputFile);
		UeAgentExportPublisher::FFilePlan FilePlan;
		FilePlan.RelativePath = FPaths::GetCleanFilename(OutputFile);
		FilePlan.Root = Obj;
		UeAgentExportPublisher::FPublishContext PublishContext;
		PublishContext.ContractId = TEXT("uai.export.level_content.single_file");
		PublishContext.ContractVersion = TEXT("1");
		PublishContext.ProfileId = TEXT("llm_authoring");
		UeAgentExportPublisher::FPublishResult PublishResult;
		FString PublishErrorCode;
		FString PublishErrorDetail;
		if (!UeAgentExportPublisher::PublishSingleFile(
			OutputFile,
			FilePlan,
			PublishContext,
			PublishResult,
			PublishErrorCode,
			PublishErrorDetail))
		{
			OutError = PublishErrorDetail.IsEmpty() ? PublishErrorCode : PublishErrorDetail;
			return false;
		}
		if (PublishResult.WrittenFiles.Num() == 1)
		{
			OutputFile = PublishResult.WrittenFiles[0];
		}

		TSharedPtr<FJsonObject> Compact = MakeShared<FJsonObject>();
		auto CopyString = [&Obj, &Compact](const TCHAR* FieldName)
		{
			FString Value;
			if (Obj->TryGetStringField(FieldName, Value))
			{
				Compact->SetStringField(FieldName, Value);
			}
		};
		auto CopyNumber = [&Obj, &Compact](const TCHAR* FieldName)
		{
			double Value = 0.0;
			if (Obj->TryGetNumberField(FieldName, Value))
			{
				Compact->SetNumberField(FieldName, Value);
			}
		};
		auto CopyBool = [&Obj, &Compact](const TCHAR* FieldName)
		{
			bool bValue = false;
			if (Obj->TryGetBoolField(FieldName, bValue))
			{
				Compact->SetBoolField(FieldName, bValue);
			}
		};
		auto CopyObject = [&Obj, &Compact](const TCHAR* FieldName)
		{
			const TSharedPtr<FJsonObject>* Value = nullptr;
			if (Obj->TryGetObjectField(FStringView(FieldName), Value) && Value && Value->IsValid())
			{
				Compact->SetObjectField(FieldName, *Value);
			}
		};

		CopyString(TEXT("schema"));
		CopyString(TEXT("query_schema"));
		CopyBool(TEXT("snapshot"));
		CopyBool(TEXT("actor_only"));
		CopyNumber(TEXT("visited_actor_count"));
		CopyNumber(TEXT("resource_count"));
		CopyObject(TEXT("world"));
		CopyObject(TEXT("organization"));
		CopyObject(TEXT("feature_gates"));

		if (!Compact->HasField(TEXT("resource_count")))
		{
			const TArray<TSharedPtr<FJsonValue>>* Resources = nullptr;
			if (Obj->TryGetArrayField(TEXT("resources"), Resources) && Resources)
			{
				Compact->SetNumberField(TEXT("resource_count"), Resources->Num());
			}
		}
		OutData = Compact;
		OutData->SetStringField(TEXT("output_file"), OutputFile);
		OutData->SetStringField(TEXT("output_format"), TEXT("hjson_authoring"));
		OutData->SetBoolField(TEXT("output_written_full"), true);
		OutData->SetBoolField(TEXT("output_compacted"), true);
		OutData->SetBoolField(TEXT("resources_omitted"), true);
		OutData->SetStringField(TEXT("entry_file"), FPaths::GetCleanFilename(OutputFile));
		TArray<TSharedPtr<FJsonValue>> WrittenRelativeValues;
		for (const FString& RelativePath : PublishResult.WrittenRelativeFiles)
		{
			WrittenRelativeValues.Add(MakeShared<FJsonValueString>(RelativePath));
		}
		OutData->SetArrayField(TEXT("written_relative_files"), WrittenRelativeValues);
		if (PublishResult.Receipt.IsValid())
		{
			OutData->SetObjectField(TEXT("publisher"), PublishResult.Receipt.ToSharedRef());
		}
		return true;
	}

	bool ResolveContentObject(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutContent, FString& OutSource, FString& OutError)
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
				if (IsLegacyJsonFilePath(JsonFile))
				{
					OutError = FString::Printf(TEXT("hjson_file_required:%s"), *JsonFile);
					return false;
				}
				OutSource = JsonFile;
				UeAgentAuthoringFileIO::FLoadOptions Options;
				return UeAgentAuthoringFileIO::LoadObject(JsonFile, OutContent, Options, OutError);
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

	bool TryGetArray(const TSharedPtr<FJsonObject>& Obj, const FString& Key, const TArray<TSharedPtr<FJsonValue>>*& OutArray)
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

	bool TryReadVectorValue(const TSharedPtr<FJsonValue>& Value, FVector& Out)
	{
		return UeAgentJsonDiagnostics::TryReadVectorValue(Value, Out);
	}

	bool TryReadRotatorValue(const TSharedPtr<FJsonValue>& Value, FRotator& Out)
	{
		return UeAgentJsonDiagnostics::TryReadRotatorValue(Value, Out);
	}

	bool TryGetVectorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& Out)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Key);
		return TryReadVectorValue(Value, Out);
	}

	bool TryGetRotatorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FRotator& Out)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Key);
		return TryReadRotatorValue(Value, Out);
	}

	TSharedPtr<FJsonValue> MakeStringArrayValue(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Arr.Add(MakeShared<FJsonValueString>(Value));
		}
		return MakeShared<FJsonValueArray>(Arr);
	}

	TArray<FString> ReadStringArray(const TSharedPtr<FJsonObject>& Obj, const FString& Key)
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

	TArray<FString> ReadTags(const TSharedPtr<FJsonObject>& Obj)
	{
		return ReadStringArray(Obj, TEXT("tags"));
	}

	bool ActorHasTagString(AActor* Actor, const FString& Tag)
	{
		return Actor && !Tag.IsEmpty() && Actor->Tags.Contains(FName(*Tag));
	}

	FString MakeManagedIdTag(const FString& Id)
	{
		return UeAgentLevelAuthoring::MakeManagedActorTag(Id);
	}

	FString GetManagedId(AActor* Actor)
	{
		return Actor ? UeAgentLevelAuthoring::GetManagedActorId(*Actor) : FString();
	}

	FString ActorStableId(AActor* Actor)
	{
		const FString ManagedId = GetManagedId(Actor);
		if (!ManagedId.IsEmpty())
		{
			return ManagedId;
		}
		return Actor ? Actor->GetActorLabel() : FString();
	}

	bool FolderMatches(const FString& ActorFolder, const FString& RootFolder, bool bIncludeChildFolders)
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

	AActor* FindActorByNameLabelOrManagedId(UWorld* World, const FString& Id)
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

	FString GetResourceId(const TSharedPtr<FJsonObject>& Resource)
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

	FString GetResourceOperation(const TSharedPtr<FJsonObject>& Resource, const FString& DefaultOperation)
	{
		FString Operation;
		if (TryGetStringAny(Resource, { TEXT("operation"), TEXT("op") }, Operation) && !Operation.TrimStartAndEnd().IsEmpty())
		{
			return Operation.TrimStartAndEnd().ToLower();
		}
		return DefaultOperation;
	}

	FString GetResourceCategory(const TSharedPtr<FJsonObject>& Resource)
	{
		FString Category;
		if (TryGetStringAny(Resource, { TEXT("category") }, Category) && !Category.TrimStartAndEnd().IsEmpty())
		{
			return Category.TrimStartAndEnd().ToLower();
		}
		return TEXT("spatial");
	}

	FString GetResourceKind(const TSharedPtr<FJsonObject>& Resource)
	{
		FString Kind;
		if (TryGetStringAny(Resource, { TEXT("kind"), TEXT("type") }, Kind) && !Kind.TrimStartAndEnd().IsEmpty())
		{
			return Kind.TrimStartAndEnd().ToLower();
		}
		return TEXT("actor");
	}

	FString GetClassPathForKind(const FString& Kind)
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

	FString GetClassPathForResource(const TSharedPtr<FJsonObject>& Resource)
	{
		FString ClassPath;
		if (TryGetStringAny(Resource, { TEXT("class"), TEXT("class_path"), TEXT("actor_class") }, ClassPath) && !ClassPath.TrimStartAndEnd().IsEmpty())
		{
			return ClassPath.TrimStartAndEnd();
		}
		FString CatalogName;
		if (TryGetStringAny(Resource, { TEXT("catalog_name"), TEXT("name") }, CatalogName) && !CatalogName.TrimStartAndEnd().IsEmpty())
		{
			UeAgentPaletteSearch::FCandidate CatalogCandidate;
			FString ResolveError;
			if (UeAgentPaletteSearch::ResolveCatalogNameToCandidate(TEXT("placeable_actor"), CatalogName, CatalogCandidate, ResolveError))
			{
				return CatalogCandidate.FullName.TrimStartAndEnd();
			}
		}
		return GetClassPathForKind(GetResourceKind(Resource));
	}

	bool IsSpatialActorResource(const TSharedPtr<FJsonObject>& Resource)
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

	void CollectResourceObjects(const TSharedPtr<FJsonObject>& Content, TArray<TSharedPtr<FJsonObject>>& OutResources)
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

	TSharedPtr<FJsonObject> GetScope(const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params)
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

	FString GetScopeFolderRoot(const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Scope = GetScope(Content, Params);
		FString FolderRoot;
		if (TryGetStringAny(Scope, { TEXT("folder_root"), TEXT("folder_path"), TEXT("folder") }, FolderRoot))
		{
			return NormalizeFolder(FolderRoot);
		}
		return FString();
	}

	FString GetManagedTag(const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Scope = GetScope(Content, Params);
		FString ManagedTag;
		if (TryGetStringAny(Scope, { TEXT("managed_tag") }, ManagedTag) && !ManagedTag.TrimStartAndEnd().IsEmpty())
		{
			return ManagedTag.TrimStartAndEnd();
		}
		return TEXT("UAI.Managed");
	}

	TArray<FString> GetDefaultTags(const TSharedPtr<FJsonObject>& Content)
	{
		TArray<FString> Tags;
		TSharedPtr<FJsonObject> Defaults;
		if (TryGetObject(Content, TEXT("defaults"), Defaults))
		{
			Tags = ReadTags(Defaults);
		}
		return Tags;
	}

	FString GetDefaultFolderPath(const TSharedPtr<FJsonObject>& Content)
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

	UActorComponent* FindComponentByName(AActor* Actor, const FString& ComponentName)
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
}
