#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialViewportWarnings.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"

namespace UE::UAI::DomainAdapters::WorldSpatial::Private
{
	namespace
	{
		bool FolderFailure(UE::UAI::Contract::FDiagnostic& OutError, const TCHAR* Code,
			const TCHAR* Reason, const TCHAR* Path, const TSharedPtr<FJsonValue>& Actual,
			const TSharedPtr<FJsonValue>& Expected)
		{
			OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Path, Expected, Actual, TEXT("native_execution"), Reason);
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
	}

	FString NormalizeFolderPathForMatch(const FString& InFolderPath)
	{
		FString Normalized = InFolderPath.TrimStartAndEnd();
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Normalized.RemoveFromStart(TEXT("/"))) {}
		while (Normalized.RemoveFromEnd(TEXT("/"))) {}
		return Normalized;
	}

	bool DoesActorMatchFolderPath(AActor* Actor, const FString& NormalizedFolderPath, bool bIncludeChildFolders)
	{
		if (!Actor || NormalizedFolderPath.IsEmpty()) return false;
		const FString ActorFolderPath = NormalizeFolderPathForMatch(Actor->GetFolderPath().ToString());
		if (ActorFolderPath.IsEmpty()) return false;
		if (ActorFolderPath.Equals(NormalizedFolderPath, ESearchCase::IgnoreCase)) return true;
		return bIncludeChildFolders && ActorFolderPath.StartsWith(NormalizedFolderPath + TEXT("/"), ESearchCase::IgnoreCase);
	}

	bool GetFolderActorBounds(UWorld* World, const FString& FolderPath, bool bIncludeChildFolders, FBox& OutCombinedBounds, TArray<AActor*>& OutActors, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!World) return FolderFailure(OutError, TEXT("missing_world"), TEXT("An Editor world is required to resolve folder bounds."),
			TEXT("/world"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("available Editor world")));
		const FString NormalizedFolderPath = NormalizeFolderPathForMatch(FolderPath);
		if (NormalizedFolderPath.IsEmpty()) return FolderFailure(OutError, TEXT("missing_folder_path"), TEXT("The folder path is empty after trimming separators and whitespace."),
			TEXT("/folder_path"), MakeShared<FJsonValueString>(FolderPath), MakeShared<FJsonValueString>(TEXT("nonempty normalized folder path")));
		OutCombinedBounds = FBox(EForceInit::ForceInit);
		OutActors.Reset();
		bool bFoundFolderActor = false;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsActorBeingDestroyed() || !DoesActorMatchFolderPath(Actor, NormalizedFolderPath, bIncludeChildFolders)) continue;
			bFoundFolderActor = true;
			const FBox ActorBounds = Actor->GetComponentsBoundingBox(true);
			if (!ActorBounds.IsValid) continue;
			OutCombinedBounds += ActorBounds;
			OutActors.Add(Actor);
		}
		if (OutActors.Num() <= 0 || !OutCombinedBounds.IsValid)
		{
			const TSharedRef<FJsonObject> Query = MakeShared<FJsonObject>();
			Query->SetStringField(TEXT("folder_path"), FolderPath);
			Query->SetBoolField(TEXT("include_child_folders"), bIncludeChildFolders);
			Query->SetBoolField(TEXT("found_matching_actor"), bFoundFolderActor);
			Query->SetNumberField(TEXT("valid_bounds_actor_count"), OutActors.Num());
			if (OutActors.Num() <= 0)
				return FolderFailure(OutError, bFoundFolderActor ? TEXT("folder_actor_bounds_invalid") : TEXT("folder_has_no_actors"),
					bFoundFolderActor ? TEXT("Actors match the folder, but none has valid component bounds.") : TEXT("No live Actors match the requested folder scope."),
					TEXT("/folder_path"), MakeShared<FJsonValueObject>(Query), MakeShared<FJsonValueString>(TEXT("at least one matching Actor with valid bounds")));
			return FolderFailure(OutError, TEXT("folder_bounds_invalid"), TEXT("The folder Actors did not produce a valid combined bounding box."),
				TEXT("/bounds"), MakeShared<FJsonValueObject>(Query), MakeShared<FJsonValueString>(TEXT("valid combined native bounds")));
		}
		return true;
	}

	bool GetActorsInFolder(UWorld* World, const FString& FolderPath, bool bIncludeChildFolders, TArray<AActor*>& OutActors, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!World) return FolderFailure(OutError, TEXT("missing_world"), TEXT("An Editor world is required to resolve folder Actors."),
			TEXT("/world"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("available Editor world")));
		const FString NormalizedFolderPath = NormalizeFolderPathForMatch(FolderPath);
		if (NormalizedFolderPath.IsEmpty()) return FolderFailure(OutError, TEXT("missing_folder_path"), TEXT("The folder path is empty after trimming separators and whitespace."),
			TEXT("/folder_path"), MakeShared<FJsonValueString>(FolderPath), MakeShared<FJsonValueString>(TEXT("nonempty normalized folder path")));
		OutActors.Reset();
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor && !Actor->IsActorBeingDestroyed() && DoesActorMatchFolderPath(Actor, NormalizedFolderPath, bIncludeChildFolders)) OutActors.Add(Actor);
		}
		return true;
	}
}
