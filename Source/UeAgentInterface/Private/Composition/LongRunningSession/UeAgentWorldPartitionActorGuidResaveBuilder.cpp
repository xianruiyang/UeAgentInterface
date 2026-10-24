#include "Composition/LongRunningSession/UeAgentWorldPartitionActorGuidResaveBuilder.h"

#include "Commandlets/Commandlet.h"
#include "GameFramework/Actor.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogUeAgentWorldPartitionActorGuidResave, Log, All);

bool UUeAgentWorldPartitionActorGuidResaveBuilder::PreRun(
	UWorld* World,
	FPackageSourceControlHelper& PackageHelper)
{
	(void)PackageHelper;
	ActorGuids.Reset();
	if (!World || !World->GetWorldPartition())
	{
		UE_LOG(LogUeAgentWorldPartitionActorGuidResave, Error,
			TEXT("Actor GUID resave requires an existing world with World Partition; world=%s, world_present=%d, partition_present=%d."),
			*GetPathNameSafe(World), World != nullptr, World && World->GetWorldPartition() != nullptr);
		return false;
	}

	TArray<FString> Tokens;
	TArray<FString> Switches;
	UCommandlet::ParseCommandLine(*GetBuilderArgs(), Tokens, Switches);
	FString EncodedGuids;
	for (const FString& Switch : Switches)
	{
		if (Switch.StartsWith(TEXT("ActorGuids="), ESearchCase::CaseSensitive))
		{
			if (!EncodedGuids.IsEmpty())
			{
				UE_LOG(
					LogUeAgentWorldPartitionActorGuidResave,
					Error,
					TEXT("ActorGuids was specified more than once."));
				return false;
			}
			EncodedGuids = Switch.RightChop(11);
		}
	}

	TArray<FString> GuidTexts;
	EncodedGuids.ParseIntoArray(GuidTexts, TEXT("+"), true);
	TSet<FGuid> UniqueGuids;
	for (const FString& GuidText : GuidTexts)
	{
		FGuid Guid;
		if (!FGuid::Parse(GuidText, Guid) || !Guid.IsValid()
			|| UniqueGuids.Contains(Guid))
		{
			UE_LOG(
				LogUeAgentWorldPartitionActorGuidResave,
				Error,
				TEXT("ActorGuids contains an invalid or duplicate GUID: %s"),
				*GuidText);
			return false;
		}
		UniqueGuids.Add(Guid);
		ActorGuids.Add(Guid);
	}
	if (ActorGuids.IsEmpty())
	{
		UE_LOG(
			LogUeAgentWorldPartitionActorGuidResave,
			Error,
			TEXT("ActorGuids must contain at least one exact actor GUID."));
		return false;
	}
	return true;
}

bool UUeAgentWorldPartitionActorGuidResaveBuilder::RunInternal(
	UWorld* World,
	const FCellInfo& CellInfo,
	FPackageSourceControlHelper& PackageHelper)
{
	(void)CellInfo;
	UWorldPartition* Partition = World ? World->GetWorldPartition() : nullptr;
	if (!Partition)
	{
		UE_LOG(LogUeAgentWorldPartitionActorGuidResave, Error,
			TEXT("Actor GUID resave cannot run because World Partition is unavailable; world=%s, world_present=%d."),
			*GetPathNameSafe(World), World != nullptr);
		return false;
	}
	for (const FGuid& Guid : ActorGuids)
	{
		if (!Partition->GetActorDescInstance(Guid))
		{
			UE_LOG(
				LogUeAgentWorldPartitionActorGuidResave,
				Error,
				TEXT("Actor GUID is not present in the target World Partition: %s"),
				*Guid.ToString(EGuidFormats::DigitsWithHyphens));
			return false;
		}
	}

	TArray<UPackage*> PackagesToSave;
	bool bSaveSucceeded = true;
	bool bLoadedActorValid = true;
	int32 LoadedActorCount = 0;
	FWorldPartitionHelpers::FForEachActorWithLoadingParams Parameters;
	Parameters.ActorGuids = ActorGuids;
	Parameters.OnPreGarbageCollect =
		[&PackagesToSave, &PackageHelper, &bSaveSucceeded]()
		{
			if (!PackagesToSave.IsEmpty())
			{
				bSaveSucceeded = SavePackages(
					PackagesToSave, PackageHelper, true) && bSaveSucceeded;
				PackagesToSave.Reset();
			}
		};
	FWorldPartitionHelpers::ForEachActorWithLoading(
		Partition,
		[&PackagesToSave, &bLoadedActorValid, &LoadedActorCount](
			const FWorldPartitionActorDescInstance* ActorDesc)
		{
			AActor* Actor = ActorDesc ? ActorDesc->GetActor() : nullptr;
			UPackage* Package = Actor ? Actor->GetExternalPackage() : nullptr;
			if (!Package)
			{
				UE_LOG(LogUeAgentWorldPartitionActorGuidResave, Error,
					TEXT("Actor GUID resave could not obtain an external actor package; guid=%s, descriptor_present=%d, actor_present=%d, actor=%s."),
					ActorDesc ? *ActorDesc->GetGuid().ToString(EGuidFormats::DigitsWithHyphens) : TEXT("<missing>"),
					ActorDesc != nullptr, Actor != nullptr, *GetPathNameSafe(Actor));
				bLoadedActorValid = false;
				return false;
			}
			++LoadedActorCount;
			PackagesToSave.AddUnique(Package);
			return true;
		},
		Parameters);
	if (!bLoadedActorValid) return false;
	if (LoadedActorCount != ActorGuids.Num())
	{
		UE_LOG(LogUeAgentWorldPartitionActorGuidResave, Error,
			TEXT("Loaded actor count does not match the exact ActorGuids set; actual=%d, expected=%d."),
			LoadedActorCount, ActorGuids.Num());
		return false;
	}
	if (!bSaveSucceeded)
	{
		UE_LOG(LogUeAgentWorldPartitionActorGuidResave, Error,
			TEXT("Saving an actor package batch before garbage collection failed; see the preceding package/source-control diagnostics."));
		return false;
	}
	if (PackagesToSave.IsEmpty()) return true;
	if (SavePackages(PackagesToSave, PackageHelper)) return true;
	UE_LOG(LogUeAgentWorldPartitionActorGuidResave, Error,
		TEXT("Saving the final actor package batch failed; package_count=%d. See the preceding package/source-control diagnostics."),
		PackagesToSave.Num());
	return false;
}
