#include "DomainAdapters/QueryCalibration/UeAgentQueryCalibrationSource.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"

namespace UE::UAI::DomainAdapters::QueryCalibration
{
	namespace
	{
		const TArray<FString> CategoryIds = {
			TEXT("capability"),
			TEXT("content_search"),
			TEXT("candidate_catalog"),
			TEXT("state"),
			TEXT("reference"),
			TEXT("runtime_probe"),
		};

		FString BoolText(const bool bValue)
		{
			return bValue ? TEXT("true") : TEXT("false");
		}

		bool MatchesSearch(const FString& SearchText, const FString& Value)
		{
			return SearchText.IsEmpty()
				|| Value.Contains(SearchText, ESearchCase::IgnoreCase);
		}

		UWorld* CurrentEditorWorld()
		{
			if (!GEditor)
			{
				return nullptr;
			}
			return GEditor->GetEditorWorldContext().World();
		}

		FString WorldRevision(const UWorld* World)
		{
			if (!World)
			{
				return TEXT("world-unavailable");
			}
			return FString::Printf(
				TEXT("%s:actors-%d:dirty-%s"),
				*World->GetPathName(),
				World->GetActorCount(),
				World->GetPackage() && World->GetPackage()->IsDirty()
					? TEXT("true")
					: TEXT("false"));
		}

		FQueryCalibrationItem MakeItem(
			FString Identity,
			FString DisplayName,
			FString Kind,
			TMap<FString, FString> Fields)
		{
			FQueryCalibrationItem Item;
			Item.Identity = MoveTemp(Identity);
			Item.DisplayName = MoveTemp(DisplayName);
			Item.Kind = MoveTemp(Kind);
			Item.Fields = MoveTemp(Fields);
			return Item;
		}

		class FProductionQueryCalibrationSource final
			: public IUeAgentQueryCalibrationSource
		{
		public:
			virtual bool Read(
				const FQueryCalibrationRequest& Request,
				FQueryCalibrationPage& OutPage,
				FQueryCalibrationError& OutError) const override
			{
				OutPage = {};
				OutError = {};
				if (!IsInGameThread())
				{
					OutError.Code = TEXT("query_thread_invalid");
					OutError.Detail = TEXT("real UE query mechanics require the game thread");
					return false;
				}
				if (!CategoryIds.Contains(Request.Category))
				{
					OutError.Code = TEXT("query_category_unknown");
					OutError.Detail = Request.Category;
					return false;
				}

				OutPage.Category = Request.Category;
				OutPage.SourceId = TEXT("ue_editor_query_calibration");
				OutPage.StateLayer = TEXT("editor_current_state");
				OutPage.bReadOnly = true;
				OutPage.Completeness = EQueryCalibrationCompleteness::Complete;
				const UWorld* World = CurrentEditorWorld();
				OutPage.Revision = WorldRevision(World);

				if (Request.Category == TEXT("capability"))
				{
					ReadCapability(Request, OutPage, World);
				}
				else if (Request.Category == TEXT("content_search"))
				{
					ReadContent(Request, OutPage);
				}
				else if (Request.Category == TEXT("candidate_catalog"))
				{
					ReadCandidates(Request, OutPage, World);
				}
				else if (Request.Category == TEXT("state"))
				{
					ReadState(Request, OutPage, World);
				}
				else if (Request.Category == TEXT("reference"))
				{
					ReadReferences(Request, OutPage, World);
				}
				else
				{
					ReadRuntime(Request, OutPage, World);
				}
				OutPage.TotalItems = OutPage.Items.Num();
				return true;
			}

		private:
			static void AddIfMatched(
				const FQueryCalibrationRequest& Request,
				FQueryCalibrationPage& Page,
				FQueryCalibrationItem Item)
			{
				if (MatchesSearch(Request.SearchText, Item.Identity)
					|| MatchesSearch(Request.SearchText, Item.DisplayName)
					|| MatchesSearch(Request.SearchText, Item.Kind))
				{
					Page.Items.Add(MoveTemp(Item));
				}
			}

			static void MarkUnavailable(
				FQueryCalibrationPage& Page,
				const FString& Source)
			{
				Page.Completeness = EQueryCalibrationCompleteness::Degraded;
				Page.MissingSources.AddUnique(Source);
			}

			static void ReadCapability(
				const FQueryCalibrationRequest& Request,
				FQueryCalibrationPage& Page,
				const UWorld* World)
			{
				Page.StateLayer = TEXT("engine_and_editor_capability");
				Page.Revision = FEngineVersion::Current().ToString();
				AddIfMatched(Request, Page, MakeItem(
					TEXT("ue:engine"),
					TEXT("Unreal Engine"),
					TEXT("engine_capability"),
					{
						{TEXT("version"), FEngineVersion::Current().ToString()},
						{TEXT("editor"), BoolText(GIsEditor)},
						{TEXT("unattended"), BoolText(FApp::IsUnattended())},
					}));
				AddIfMatched(Request, Page, MakeItem(
					TEXT("ue:editor_world"),
					TEXT("Current editor world"),
					TEXT("world_capability"),
					{
						{TEXT("available"), BoolText(World != nullptr)},
						{TEXT("identity"), World ? World->GetPathName() : FString()},
					}));
				if (!World)
				{
					MarkUnavailable(Page, TEXT("editor_world"));
				}
			}

			static void ReadContent(
				const FQueryCalibrationRequest& Request,
				FQueryCalibrationPage& Page)
			{
				Page.StateLayer = TEXT("asset_registry_current");
				FAssetRegistryModule& Module =
					FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				TArray<FAssetData> Assets;
				Module.Get().GetAssetsByPath(FName(TEXT("/Game")), Assets, true, false);
				Assets.Sort([](const FAssetData& Left, const FAssetData& Right)
				{
					return Left.GetObjectPathString() < Right.GetObjectPathString();
				});
				const int32 Capacity = 1000;
				for (const FAssetData& Asset : Assets)
				{
					if (Page.Items.Num() >= Capacity)
					{
						Page.Completeness = EQueryCalibrationCompleteness::Partial;
						Page.MissingSources.AddUnique(TEXT("asset_registry_results_after_1000"));
						break;
					}
					AddIfMatched(Request, Page, MakeItem(
						Asset.GetObjectPathString(),
						Asset.AssetName.ToString(),
						TEXT("asset"),
						{
							{TEXT("class_path"), Asset.AssetClassPath.ToString()},
							{TEXT("package_name"), Asset.PackageName.ToString()},
						}));
				}
				Page.Revision = FString::Printf(TEXT("asset-registry-count-%d"), Assets.Num());
			}

			static void ReadCandidates(
				const FQueryCalibrationRequest& Request,
				FQueryCalibrationPage& Page,
				const UWorld* World)
			{
				Page.StateLayer = TEXT("loaded_world_actor_classes");
				if (!World)
				{
					MarkUnavailable(Page, TEXT("editor_world"));
					return;
				}
				TSet<FString> Seen;
				for (TActorIterator<AActor> It(const_cast<UWorld*>(World)); It; ++It)
				{
					const UClass* Class = It->GetClass();
					if (!Class || Seen.Contains(Class->GetPathName()))
					{
						continue;
					}
					Seen.Add(Class->GetPathName());
					AddIfMatched(Request, Page, MakeItem(
						Class->GetPathName(),
						Class->GetName(),
						TEXT("actor_class_candidate"),
						{
							{TEXT("class_path"), Class->GetPathName()},
							{TEXT("source"), TEXT("loaded_editor_world")},
						}));
				}
				Page.Items.Sort([](const FQueryCalibrationItem& Left, const FQueryCalibrationItem& Right)
				{
					return Left.Identity < Right.Identity;
				});
			}

			static void ReadState(
				const FQueryCalibrationRequest& Request,
				FQueryCalibrationPage& Page,
				const UWorld* World)
			{
				Page.StateLayer = TEXT("editor_world_current");
				if (!World)
				{
					MarkUnavailable(Page, TEXT("editor_world"));
					return;
				}
				AddIfMatched(Request, Page, MakeItem(
					World->GetPathName(),
					World->GetMapName(),
					TEXT("world_state"),
					{
						{TEXT("actor_count"), FString::FromInt(World->GetActorCount())},
						{TEXT("package_dirty"), BoolText(World->GetPackage() && World->GetPackage()->IsDirty())},
						{TEXT("world_type"), LexToString(World->WorldType)},
					}));
			}

			static void ReadReferences(
				const FQueryCalibrationRequest& Request,
				FQueryCalibrationPage& Page,
				const UWorld* World)
			{
				Page.StateLayer = TEXT("editor_world_reference_graph");
				if (!World)
				{
					MarkUnavailable(Page, TEXT("editor_world"));
					return;
				}
				AddIfMatched(Request, Page, MakeItem(
					World->GetPathName(), World->GetName(), TEXT("world_reference"),
					{{TEXT("package"), World->GetPackage()->GetName()}}));
				if (World->PersistentLevel)
				{
					AddIfMatched(Request, Page, MakeItem(
						World->PersistentLevel->GetPathName(),
						World->PersistentLevel->GetName(),
						TEXT("persistent_level_reference"),
						{{TEXT("outer_world"), World->GetPathName()}}));
				}
				else
				{
					MarkUnavailable(Page, TEXT("persistent_level"));
				}
				if (const AWorldSettings* Settings = World->GetWorldSettings())
				{
					AddIfMatched(Request, Page, MakeItem(
						Settings->GetPathName(),
						Settings->GetName(),
						TEXT("world_settings_reference"),
						{{TEXT("outer_world"), World->GetPathName()}}));
				}
			}

			static void ReadRuntime(
				const FQueryCalibrationRequest& Request,
				FQueryCalibrationPage& Page,
				const UWorld* World)
			{
				Page.StateLayer = TEXT("editor_process_runtime");
				Page.Revision = FString::Printf(
					TEXT("process-%u:%s"),
					FPlatformProcess::GetCurrentProcessId(),
					*WorldRevision(World));
				AddIfMatched(Request, Page, MakeItem(
					FString::Printf(TEXT("process:%u"), FPlatformProcess::GetCurrentProcessId()),
					TEXT("Unreal Editor process"),
					TEXT("runtime_probe"),
					{
						{TEXT("game_thread"), BoolText(IsInGameThread())},
						{TEXT("editor_world"), BoolText(World != nullptr)},
						{TEXT("pie_running"), BoolText(GEditor && GEditor->PlayWorld != nullptr)},
						{TEXT("engine_available"), BoolText(GEngine != nullptr)},
					}));
			}
		};
	}

	const TArray<FString>& GetQueryCalibrationCategoryIds()
	{
		return CategoryIds;
	}

	bool TryCreateProductionQueryCalibrationSource(
		TSharedPtr<IUeAgentQueryCalibrationSource>& OutSource,
		FString& OutError)
	{
		OutError.Reset();
		OutSource = MakeShared<FProductionQueryCalibrationSource>();
		return true;
	}
}
