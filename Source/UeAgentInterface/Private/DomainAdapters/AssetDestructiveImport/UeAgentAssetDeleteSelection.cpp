#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeleteSelection.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::AssetDestructiveImport
{
	namespace
	{
		FUeAgentAssetDeleteSelectionResult Fail(UE::UAI::Contract::FDiagnostic Diagnostic)
		{
			FUeAgentAssetDeleteSelectionResult Result;
			Result.Diagnostic = MoveTemp(Diagnostic);
			return Result;
		}

		UE::UAI::Contract::FDiagnostic SelectionFact(const FString& Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Kind)
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("asset_delete_selection"));
			Diagnostic.SourceKind = Kind;
			Diagnostic.PropertyPath = Path;
			if (Kind == TEXT("command_input")) Diagnostic.Source.JsonPath = Path;
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			return Diagnostic;
		}

		bool TryNormalizeRef(
			FString InputRef,
			FString& OutNormalizedRef,
			FString& OutPackageName,
			bool& bOutObjectPath)
		{
			InputRef.TrimStartAndEndInline();
			OutNormalizedRef = MoveTemp(InputRef);
			OutPackageName.Reset();
			bOutObjectPath = OutNormalizedRef.Contains(TEXT("."));
			if (bOutObjectPath)
			{
				if (!FPackageName::IsValidObjectPath(OutNormalizedRef))
				{
					return false;
				}
				OutPackageName =
					FPackageName::ObjectPathToPackageName(OutNormalizedRef);
			}
			else
			{
				while (OutNormalizedRef.Len() > FString(TEXT("/Game")).Len()
					&& OutNormalizedRef.EndsWith(TEXT("/")))
				{
					OutNormalizedRef.LeftChopInline(1, EAllowShrinking::No);
				}
				OutPackageName = OutNormalizedRef;
			}
			return OutPackageName == TEXT("/Game")
				|| (OutPackageName.StartsWith(TEXT("/Game/"))
					&& FPackageName::IsValidLongPackageName(
						OutPackageName, true));
		}

		void AddSelection(
			const FAssetData& Asset,
			const FString& InputRef,
			const bool bFromFolder,
			TArray<FUeAgentAssetDeleteSelectionItem>& InOutItems,
			TMap<FString, int32>& InOutObjectPathToIndex)
		{
			const FString ObjectPath = Asset.GetSoftObjectPath().ToString();
			if (int32* const ExistingIndex =
				InOutObjectPathToIndex.Find(ObjectPath))
			{
				TArray<FUeAgentAssetDeleteSelectionSource>& Sources =
					InOutItems[*ExistingIndex].Sources;
				if (!Sources.ContainsByPredicate(
					[&InputRef, bFromFolder](
						const FUeAgentAssetDeleteSelectionSource& Source)
					{
						return Source.InputRef == InputRef
							&& Source.bFromFolder == bFromFolder;
					}))
				{
					Sources.Add({InputRef, bFromFolder});
				}
				return;
			}

			FUeAgentAssetDeleteSelectionItem& Item = InOutItems.AddDefaulted_GetRef();
			Item.PackageName = Asset.PackageName.ToString();
			Item.ObjectPath = ObjectPath;
			Item.Sources.Add({InputRef, bFromFolder});
			InOutObjectPathToIndex.Add(ObjectPath, InOutItems.Num() - 1);
		}
	}

	FUeAgentAssetDeleteSelectionResult ResolveAssetDeleteSelection(
		const FUeAgentAssetDeleteSelectionRequest& Request)
	{
		if (Request.Targets.IsEmpty())
		{
			auto Diagnostic = SelectionFact(TEXT("asset_delete_selection_targets_empty"), TEXT("Asset deletion requires at least one target reference."),
				TEXT("/targets"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one reference")), TEXT("command_input"));
			Diagnostic.PropertyPath = TEXT("/targets/count");
			return Fail(MoveTemp(Diagnostic));
		}

		IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
		if (Registry.IsLoadingAssets())
		{
			return Fail(SelectionFact(TEXT("asset_delete_selection_registry_query_failed"), TEXT("Asset registry initial discovery has not completed."),
				TEXT("/asset_registry/discovery_complete"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("native_result")));
		}

		FUeAgentAssetDeleteSelectionResult Result;
		TMap<FString, int32> ObjectPathToIndex;
		for (int32 TargetIndex = 0; TargetIndex < Request.Targets.Num(); ++TargetIndex)
		{
			FString InputRef;
			FString PackageName;
			bool bObjectPath = false;
			if (!TryNormalizeRef(
					Request.Targets[TargetIndex],
					InputRef,
					PackageName,
					bObjectPath))
			{
				auto Diagnostic = SelectionFact(TEXT("asset_delete_selection_ref_invalid"), TEXT("The target must be a valid asset, package or folder reference under /Game."),
					FString::Printf(TEXT("/targets/%d"), TargetIndex), MakeShared<FJsonValueString>(Request.Targets[TargetIndex]),
					MakeShared<FJsonValueString>(TEXT("valid /Game asset, package or folder reference")), TEXT("command_input"));
				Diagnostic.OperationIndex = TargetIndex;
				return Fail(MoveTemp(Diagnostic));
			}

			TArray<FAssetData> PackageAssets;
			Registry.GetAssetsByPackageName(
				FName(*PackageName), PackageAssets, false);
			PackageAssets.RemoveAll([](const FAssetData& Asset)
			{
				return !Asset.IsValid();
			});

			if (bObjectPath)
			{
				const FAssetData* ExactAsset = PackageAssets.FindByPredicate(
					[&InputRef](const FAssetData& Asset)
					{
						return Asset.GetSoftObjectPath().ToString() == InputRef;
					});
				if (!ExactAsset)
				{
					auto Diagnostic = SelectionFact(TEXT("asset_delete_selection_ref_invalid"), TEXT("The asset registry contains no asset at the requested object path."),
						FString::Printf(TEXT("/targets/%d"), TargetIndex), MakeShared<FJsonValueString>(InputRef),
						MakeShared<FJsonValueString>(TEXT("existing asset object path")), TEXT("command_input"));
					Diagnostic.TargetPath = InputRef;
					Diagnostic.OperationIndex = TargetIndex;
					return Fail(MoveTemp(Diagnostic));
				}
				AddSelection(*ExactAsset, InputRef, false,
					Result.Items, ObjectPathToIndex);
				continue;
			}

			if (PackageAssets.Num() > 1)
			{
				TArray<FString> ObjectPaths;
				for (const FAssetData& Asset : PackageAssets)
				{
					ObjectPaths.Add(Asset.GetSoftObjectPath().ToString());
				}
				ObjectPaths.Sort();
				auto Diagnostic = SelectionFact(TEXT("asset_delete_selection_asset_ambiguous"), TEXT("The package contains multiple assets; use an explicit object path."),
					FString::Printf(TEXT("/targets/%d"), TargetIndex), MakeShared<FJsonValueNumber>(PackageAssets.Num()), MakeShared<FJsonValueNumber>(1), TEXT("command_input"));
				Diagnostic.TargetPath = InputRef;
				Diagnostic.OperationIndex = TargetIndex;
				TArray<TSharedPtr<FJsonValue>> Choices;
				for (const auto& Path : ObjectPaths) Choices.Add(MakeShared<FJsonValueString>(Path));
				Diagnostic.CandidateChoices = MakeShared<FJsonValueArray>(MoveTemp(Choices));
				return Fail(MoveTemp(Diagnostic));
			}
			if (PackageAssets.Num() == 1)
			{
				AddSelection(PackageAssets[0], InputRef, false,
					Result.Items, ObjectPathToIndex);
				continue;
			}

			TArray<FAssetData> FolderAssets;
			Registry.GetAssetsByPath(
				FName(*PackageName), FolderAssets, Request.bRecursive, false);
			FolderAssets.RemoveAll([](const FAssetData& Asset)
			{
				return !Asset.IsValid();
			});
			if (FolderAssets.IsEmpty())
			{
				auto Diagnostic = SelectionFact(TEXT("asset_delete_selection_folder_empty_or_missing"),
					Request.bRecursive ? TEXT("No assets match this folder with recursive search enabled.") : TEXT("No assets match this folder with recursive search disabled."),
					FString::Printf(TEXT("/targets/%d"), TargetIndex), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one matching asset")), TEXT("command_input"));
				Diagnostic.TargetPath = InputRef;
				Diagnostic.OperationIndex = TargetIndex;
				return Fail(MoveTemp(Diagnostic));
			}
			for (const FAssetData& Asset : FolderAssets)
			{
				AddSelection(Asset, InputRef, true,
					Result.Items, ObjectPathToIndex);
			}
		}

		for (FUeAgentAssetDeleteSelectionItem& Item : Result.Items)
		{
			Item.Sources.Sort([](
				const FUeAgentAssetDeleteSelectionSource& A,
				const FUeAgentAssetDeleteSelectionSource& B)
			{
				if (A.InputRef != B.InputRef)
				{
					return A.InputRef < B.InputRef;
				}
				return A.bFromFolder < B.bFromFolder;
			});
		}
		Result.Items.Sort([](
			const FUeAgentAssetDeleteSelectionItem& A,
			const FUeAgentAssetDeleteSelectionItem& B)
		{
			if (A.PackageName != B.PackageName)
			{
				return A.PackageName < B.PackageName;
			}
			return A.ObjectPath < B.ObjectPath;
		});
		return Result;
	}
}
