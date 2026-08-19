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
		UE_LOG(
			LogUeAgentWorldPartitionActorGuidResave,
			Error,
			TEXT("The target world does not use World Partition."));
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
				bLoadedActorValid = false;
				return false;
			}
			++LoadedActorCount;
			PackagesToSave.AddUnique(Package);
			return true;
		},
		Parameters);
	if (!bLoadedActorValid || LoadedActorCount != ActorGuids.Num()
		|| !bSaveSucceeded)
	{
		UE_LOG(
			LogUeAgentWorldPartitionActorGuidResave,
			Error,
			TEXT("Loaded package count does not match the exact ActorGuids set."));
		return false;
	}
	return PackagesToSave.IsEmpty()
		|| SavePackages(PackagesToSave, PackageHelper);
}
