#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeletePreflight.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "DomainAdapters/ContentOperations/UeAgentDestructiveContentPlanningBoundary.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace UE::UAI::DomainAdapters::AssetDestructiveImport
{
	using namespace DomainAdapters::ContentOperations;

	namespace
	{
		UE::UAI::Contract::FDiagnostic PreflightFact(const FString& Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Context = FString())
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("asset_delete_preflight"), Context);
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.PropertyPath = Path;
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			return Diagnostic;
		}

		void Block(FUeAgentAssetDeletePreparedTarget& Target, UE::UAI::Contract::FDiagnostic Diagnostic)
        {
            if (Target.Diagnostic.IsEmpty())
            {
                Target.Diagnostic = MoveTemp(Diagnostic);
                if (Target.Diagnostic.TargetPath.IsEmpty()) Target.Diagnostic.TargetPath = Target.ObjectPath;
            }
        }

	}

	FUeAgentAssetDeletePreflightResult PrepareAssetDeleteSelection(
		const FUeAgentAssetDeleteSelectionRequest& Request)
	{
		const FUeAgentAssetDeleteSelectionResult Selection =
			ResolveAssetDeleteSelection(Request);
		FUeAgentAssetDeletePreflightResult Result;
		Result.Diagnostic = Selection.Diagnostic;
		if (!Selection.IsSuccess())
		{
			return Result;
		}

		Result.Targets.Reserve(Selection.Items.Num());
		TMap<FString, int32> SelectedPackageToIndex;
		TSet<FString> OwnedExternalPackageNames;
		for (const FUeAgentAssetDeleteSelectionItem& Item : Selection.Items)
		{
			FUeAgentAssetDeletePreparedTarget& Target =
				Result.Targets.AddDefaulted_GetRef();
			Target.PackageName = Item.PackageName;
			Target.ObjectPath = Item.ObjectPath;
			Target.Sources = Item.Sources;
			SelectedPackageToIndex.Add(Target.PackageName, Result.Targets.Num() - 1);

			UE::UAI::Contract::FDiagnostic ExternalError;
			if (!TryCollectOwnedWorldExternalPackages(
					Target.PackageName,
					Target.OwnedExternalPackages,
					ExternalError))
			{
				Block(Target, MoveTemp(ExternalError));
			}
			Target.OwnedExternalPackages.Sort();
			for (const FString& ExternalPackage : Target.OwnedExternalPackages)
			{
				OwnedExternalPackageNames.Add(ExternalPackage);
			}
		}

		IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
		for (FUeAgentAssetDeletePreparedTarget& Target : Result.Targets)
		{
			if (!Target.IsExecutable())
			{
				continue;
			}

			FString Filename;
			if (!FPackageName::DoesPackageExist(Target.PackageName, &Filename))
			{
				Block(Target, PreflightFact(TEXT("asset_delete_target_missing"), TEXT("The selected asset package does not exist on disk."),
					TEXT("/package/exists"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target.PackageName));
				continue;
			}
			if (IFileManager::Get().IsReadOnly(*Filename))
			{
				Block(Target, PreflightFact(TEXT("asset_delete_package_read_only"), TEXT("The selected asset package file is read-only."),
					TEXT("/package/read_only"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Filename));
				continue;
			}
			if (const UPackage* const Package =
					FindPackage(nullptr, *Target.PackageName))
			{
				if (Package->IsDirty())
				{
					Block(Target, PreflightFact(TEXT("asset_delete_dirty_package"), TEXT("The selected asset package has unsaved changes."),
						TEXT("/package/dirty"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Target.PackageName));
					continue;
				}
			}
			const UWorld* const CurrentEditorWorld = GEditor
				? GEditor->GetEditorWorldContext().World()
				: nullptr;
			if (CurrentEditorWorld
				&& CurrentEditorWorld->GetOutermost()->GetName()
					== Target.PackageName)
			{
				Block(Target, PreflightFact(TEXT("asset_delete_current_world_open"), TEXT("The selected world is the currently open Editor world."),
					TEXT("/world/is_current_editor_world"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Target.ObjectPath));
				continue;
			}

			for (const FString& ExternalPackage : Target.OwnedExternalPackages)
			{
				FString ExternalFilename;
				if (FPackageName::DoesPackageExist(
						ExternalPackage, &ExternalFilename)
					&& IFileManager::Get().IsReadOnly(*ExternalFilename))
				{
					auto Diagnostic = PreflightFact(TEXT("asset_delete_package_read_only"), TEXT("An external package owned by the selected world is read-only."),
						TEXT("/external_package/read_only"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), ExternalFilename);
					Diagnostic.CandidateName = ExternalPackage;
					Block(Target, MoveTemp(Diagnostic));
					break;
				}
				if (const UPackage* const ExternalLoadedPackage =
						FindPackage(nullptr, *ExternalPackage))
				{
					if (ExternalLoadedPackage->IsDirty())
					{
						auto Diagnostic = PreflightFact(TEXT("asset_delete_dirty_package"), TEXT("An external package owned by the selected world has unsaved changes."),
							TEXT("/external_package/dirty"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), ExternalPackage);
						Diagnostic.CandidateName = ExternalPackage;
						Block(Target, MoveTemp(Diagnostic));
						break;
					}
				}
			}
			if (!Target.IsExecutable())
			{
				continue;
			}

			TArray<FName> ReferencerNames;
			// False means the registry has no dependency node for this package,
			// which is a valid empty-referencer result rather than a query failure.
			Registry.GetReferencers(FName(*Target.PackageName), ReferencerNames);
			for (const FName ReferencerName : ReferencerNames)
			{
				Target.Referencers.Add(ReferencerName.ToString());
			}
			Target.Referencers.Sort();
		}

		bool bClosureChanged = true;
		while (bClosureChanged)
		{
			bClosureChanged = false;
			for (FUeAgentAssetDeletePreparedTarget& Target : Result.Targets)
			{
				if (!Target.IsExecutable())
				{
					continue;
				}
				for (const FString& Referencer : Target.Referencers)
				{
					if (Referencer == Target.PackageName
						|| OwnedExternalPackageNames.Contains(Referencer))
					{
						continue;
					}
					const int32* const ReferencerIndex =
						SelectedPackageToIndex.Find(Referencer);
					if (!ReferencerIndex
						|| !Result.Targets[*ReferencerIndex].IsExecutable())
					{
						auto Diagnostic = PreflightFact(TEXT("asset_delete_package_referenced"),
							ReferencerIndex ? TEXT("A referencing package is selected but cannot be deleted.") : TEXT("A referencing package is outside the executable delete selection."),
							TEXT("/referencers/blocking_package"), MakeShared<FJsonValueString>(Referencer),
							MakeShared<FJsonValueString>(TEXT("referencer included in the executable delete selection")));
						if (ReferencerIndex) Diagnostic.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(Result.Targets[*ReferencerIndex].Diagnostic);
						Block(Target, MoveTemp(Diagnostic));
						bClosureChanged = true;
						break;
					}
				}
			}
		}
		return Result;
	}
}
