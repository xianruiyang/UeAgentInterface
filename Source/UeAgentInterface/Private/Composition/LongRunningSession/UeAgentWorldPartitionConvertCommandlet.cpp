#include "Composition/LongRunningSession/UeAgentWorldPartitionConvertCommandlet.h"

#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "WorldPartition/HLOD/HLODLayer.h"

DEFINE_LOG_CATEGORY_STATIC(LogUeAgentPartitionConvert, Log, All);

int32 UUeAgentWorldPartitionConvertCommandlet::Main(const FString& Params)
{
	TArray<FString> Tokens, Switches;
	TMap<FString, FString> Arguments;
	ParseCommandLine(*Params, Tokens, Switches, Arguments);
	if (Tokens.Num() != 1 || !FPackageName::IsValidLongPackageName(Tokens[0])
		|| !Switches.Contains(TEXT("ConversionSuffix"))
		|| Switches.Contains(TEXT("DeleteSourceLevels"))
		|| Switches.Contains(TEXT("OnlyMergeSubLevels"))
		|| Switches.Contains(TEXT("GenerateIni")))
	{
		UE_LOG(LogUeAgentPartitionConvert, Error, TEXT("UAI conversion requires one saved map and ConversionSuffix; source deletion, merge-only and ini generation are not supported."));
		return 1;
	}

	const FString Source = Tokens[0];
	const FString Destination = Source + GetConversionSuffix(false);
	const FString AssetRoot = Destination + TEXT("_Assets");
	const FString* DataLayerFolder = Arguments.Find(TEXT("DataLayerAssetFolder"));
	const FString* FoliageFolder = Arguments.Find(TEXT("FoliageTypePath"));
	if (!DataLayerFolder || *DataLayerFolder != AssetRoot / TEXT("DataLayers")
		|| !FoliageFolder || *FoliageFolder != AssetRoot / TEXT("Foliage"))
	{
		UE_LOG(LogUeAgentPartitionConvert, Error, TEXT("Conversion asset folders must be scoped beneath %s."), *AssetRoot);
		return 1;
	}

	// Native conversion removes previous output packages before loading actors.
	// Check every referenced streaming map before delegating, including maps
	// retained as level instances because they own level-script references.
	TArray<FString> Pending{Source};
	TSet<FString> Visited;
	for (int32 Index = 0; Index < Pending.Num(); ++Index)
	{
		const FString PackageName = Pending[Index];
		if (Visited.Contains(PackageName)) continue;
		Visited.Add(PackageName);
		const FString Output = PackageName + GetConversionSuffix(false);
		const FString ConfigFile = FPackageName::LongPackageNameToFilename(PackageName, TEXT(".ini"));
		if (IFileManager::Get().FileExists(*ConfigFile) || !HLODLayersForActorClasses.IsEmpty())
		{
			UE_LOG(LogUeAgentPartitionConvert, Error, TEXT("Custom conversion ini/class HLOD mappings require a separate reviewed native conversion; they may write referenced Blueprint packages: %s"), *ConfigFile);
			return 1;
		}
		TArray<FString> OutputDirectories = ULevel::GetExternalObjectsPaths(Output);
		if (PackageName == Source) OutputDirectories.Add(AssetRoot);
		if (FPackageName::DoesPackageExist(Output))
		{
			UE_LOG(LogUeAgentPartitionConvert, Error, TEXT("Conversion output already exists; it has not been overwritten: %s"), *Output);
			return 1;
		}
		for (const FString& Directory : OutputDirectories)
		{
			if (IFileManager::Get().DirectoryExists(*FPackageName::LongPackageNameToFilename(Directory)))
			{
				UE_LOG(LogUeAgentPartitionConvert, Error, TEXT("Conversion output directory already exists; it has not been cleared: %s"), *Directory);
				return 1;
			}
		}
		UWorld* Map = LoadWorld(PackageName);
		if (!Map || Map->IsPartitionedWorld())
		{
			UE_LOG(LogUeAgentPartitionConvert, Error, TEXT("Conversion requires ordinary non-partitioned source maps: %s"), *PackageName);
			return 1;
		}
		for (ULevelStreaming* Streaming : Map->GetStreamingLevels())
		{
			if (Streaming && ShouldConvertStreamingLevel(Streaming))
				Pending.Add(Streaming->GetWorldAssetPackageName());
		}
	}
	// UE duplicates the default HLOD layer chain before renaming the source
	// World, so these generated packages use the source package prefix.
	if (!Switches.Contains(TEXT("DisableStreaming")))
	{
		for (UHLODLayer* Layer = UHLODLayer::GetEngineDefaultHLODLayersSetup(); Layer; Layer = Layer->GetParentLayer())
		{
			const FString Output = Source + TEXT("_") + Layer->GetName();
			if (FPackageName::DoesPackageExist(Output))
			{
				UE_LOG(LogUeAgentPartitionConvert, Error, TEXT("Generated HLOD layer output already exists; it has not been overwritten: %s"), *Output);
				return 1;
			}
		}
	}
	UE_LOG(LogUeAgentPartitionConvert, Display, TEXT("UAI_PARTITION_CONVERSION source=%s destination=%s asset_root=%s source_preserved=true"), *Source, *Destination, *AssetRoot);
	const int32 Result = Super::Main(Params);
	UE_LOG(LogUeAgentPartitionConvert, Display, TEXT("UAI_PARTITION_CONVERSION_RESULT=%d report_only=%s destination=%s"), Result,
		Switches.Contains(TEXT("ReportOnly")) ? TEXT("true") : TEXT("false"), *Destination);
	return Result;
}
