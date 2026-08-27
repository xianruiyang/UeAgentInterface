#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"

namespace UE::UAI::DomainAdapters::ContentDiscovery
{
	using namespace UE::UAI::ContentDiscovery;

	namespace
	{
		constexpr int32 MaxProviderItems = 100000;

		void SetError(
			FContentDiscoveryError& OutError,
			const FString& Code,
			const FString& SubjectId = FString(),
			const FString& Detail = FString())
		{
			OutError = {};
			OutError.Code = Code;
			OutError.SubjectId = SubjectId;
			OutError.Detail = Detail;
		}

		FString Sha1(const FString& Value)
		{
			const FTCHARToUTF8 Utf8(*Value);
			uint8 Digest[FSHA1::DigestSize]{};
			FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
			return TEXT("sha1:")
				+ BytesToHex(Digest, FSHA1::DigestSize).ToLower();
		}

		FString RootPathFromRequest(const FContentDiscoveryRequest& Request)
		{
			FString Candidate = Request.PathPrefix;
			if (Candidate.IsEmpty()
				&& Request.MatchMode == EContentDiscoveryMatchMode::Exact
				&& Request.Query.StartsWith(TEXT("/")))
			{
				Candidate = Request.Query;
			}
			if (!Candidate.StartsWith(TEXT("/")))
			{
				return TEXT("/Game");
			}
			int32 Separator = INDEX_NONE;
			if (Candidate.FindChar(TEXT('/'), Separator)
				&& Separator == 0)
			{
				const int32 Next = Candidate.Find(
					TEXT("/"),
					ESearchCase::CaseSensitive,
					ESearchDir::FromStart,
					1);
				return Next == INDEX_NONE
					? Candidate.Left(Candidate.Find(TEXT(".")) == INDEX_NONE
						? Candidate.Len() : Candidate.Find(TEXT(".")))
					: Candidate.Left(Next);
			}
			return TEXT("/Game");
		}

		bool BuildPage(
			const FString& ProviderId,
			const FString& Revision,
			TArray<FContentDiscoveryItem> Items,
			const int32 Offset,
			const int32 Limit,
			FContentDiscoveryProviderPage& OutPage,
			FContentDiscoveryError& OutError)
		{
			OutPage = {};
			OutError = {};
			Items.Sort(
				[](const FContentDiscoveryItem& Left,
				   const FContentDiscoveryItem& Right)
				{
					return Left.MatchScore != Right.MatchScore
						? Left.MatchScore > Right.MatchScore
						: Left.Identity < Right.Identity;
				});
			if (Offset < 0 || Offset > Items.Num())
			{
				SetError(
					OutError,
					TEXT("content_search_cursor_invalid"),
					ProviderId,
					FString::FromInt(Offset));
				return false;
			}
			const int32 End = FMath::Min(Offset + Limit, Items.Num());
			OutPage.ProviderId = ProviderId;
			OutPage.Revision = Revision;
			OutPage.TotalMatches = Items.Num();
			OutPage.NextOffset = End < Items.Num() ? End : -1;
			OutPage.bComplete = OutPage.NextOffset == -1;
			OutPage.bReadOnly = true;
			for (int32 Index = Offset; Index < End; ++Index)
			{
				OutPage.Items.Add(MoveTemp(Items[Index]));
			}
			return true;
		}

		bool FilterAndScore(
			const FContentDiscoveryRequest& Request,
			FContentDiscoveryItem Item,
			TArray<FContentDiscoveryItem>& OutItems)
		{
			if (!MatchesContentDiscoveryFilters(Request, Item))
			{
				return true;
			}
			if (!TryScoreContentDiscoveryMatch(
					Request,
					Item.Identity,
					Item.DisplayName,
					Item.SearchTerms,
					Item.MatchScore,
					Item.MatchReason))
			{
				return true;
			}
			OutItems.Add(MoveTemp(Item));
			return true;
		}

		class FEditorAssetContentDiscoveryProvider final
			: public IUeAgentContentDiscoveryProvider
		{
		public:
			FEditorAssetContentDiscoveryProvider()
				: Kinds({TEXT("asset")})
			{
			}

			const FString& GetProviderId() const override
			{
				return ProviderId;
			}

			const TArray<FString>& GetContentKinds() const override
			{
				return Kinds;
			}

			bool TrySearch(
				const FContentDiscoveryRequest& Request,
				const int32 Offset,
				const int32 Limit,
				FContentDiscoveryProviderPage& OutPage,
				FContentDiscoveryError& OutError) override
			{
				OutPage = {};
				OutError = {};
				if (!IsInGameThread())
				{
					SetError(
						OutError,
						TEXT("content_search_thread_invalid"),
						ProviderId,
						TEXT("Asset Registry discovery requires the UE game thread."));
					return false;
				}
				FAssetRegistryModule& Module =
					FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
						TEXT("AssetRegistry"));
				TArray<FAssetData> Assets;
				const FString Root = RootPathFromRequest(Request);
				Module.Get().GetAssetsByPath(
					FName(*Root),
					Assets,
					true,
					false);
				if (Assets.Num() > MaxProviderItems)
				{
					SetError(
						OutError,
						TEXT("content_search_provider_capacity_exceeded"),
						ProviderId,
						FString::FromInt(Assets.Num()));
					return false;
				}
				Assets.Sort(
					[](const FAssetData& Left, const FAssetData& Right)
					{
						return Left.GetObjectPathString()
							< Right.GetObjectPathString();
					});
				FString RevisionMaterial = Root;
				for (const FAssetData& Asset : Assets)
				{
					RevisionMaterial += TEXT("\x1e")
						+ Asset.GetObjectPathString()
						+ TEXT("\x1f")
						+ Asset.AssetClassPath.ToString();
				}
				const FString Revision = Sha1(RevisionMaterial);
				TArray<FContentDiscoveryItem> Matches;
				for (const FAssetData& Asset : Assets)
				{
					FContentDiscoveryItem Item;
					Item.Identity = Asset.GetObjectPathString();
					Item.DisplayName = Asset.AssetName.ToString();
					Item.Domain = TEXT("asset_registry");
					Item.ContentKind = TEXT("asset");
					Item.ClassPath = Asset.AssetClassPath.ToString();
					Item.OwnerIdentity = Asset.PackageName.ToString();
					Item.StateLayer = TEXT("asset_registry_current");
					Item.Revision = Revision;
					Item.DetailCommandId = TEXT("asset.summary");
					Item.DetailTargetIdentity = Item.Identity;
					Item.DetailInput = MakeShared<FJsonObject>();
					Item.SupportedOperations = {TEXT("query")};
					Item.SearchTerms = {
						Asset.PackageName.ToString(),
						Asset.PackagePath.ToString(),
						Asset.AssetClassPath.ToString(),
					};
					FilterAndScore(Request, MoveTemp(Item), Matches);
				}
				return BuildPage(
					ProviderId,
					Revision,
					MoveTemp(Matches),
					Offset,
					Limit,
					OutPage,
					OutError);
			}

		private:
			FString ProviderId = TEXT("editor.asset_registry.content");
			TArray<FString> Kinds;
		};

		class FEditorWorldContentDiscoveryProvider final
			: public IUeAgentContentDiscoveryProvider
		{
		public:
			FEditorWorldContentDiscoveryProvider()
				: Kinds({TEXT("actor"), TEXT("component")})
			{
			}

			const FString& GetProviderId() const override
			{
				return ProviderId;
			}

			const TArray<FString>& GetContentKinds() const override
			{
				return Kinds;
			}

			bool TrySearch(
				const FContentDiscoveryRequest& Request,
				const int32 Offset,
				const int32 Limit,
				FContentDiscoveryProviderPage& OutPage,
				FContentDiscoveryError& OutError) override
			{
				OutPage = {};
				OutError = {};
				if (!IsInGameThread())
				{
					SetError(
						OutError,
						TEXT("content_search_thread_invalid"),
						ProviderId,
						TEXT("Editor World discovery requires the UE game thread."));
					return false;
				}
				UWorld* World = GEditor
					? GEditor->GetEditorWorldContext().World()
					: nullptr;
				if (!IsValid(World))
				{
					SetError(
						OutError,
						TEXT("content_search_world_unavailable"),
						ProviderId);
					return false;
				}
				TArray<FContentDiscoveryItem> AllItems;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (!IsValid(Actor) || !IsValid(Actor->GetClass()))
					{
						continue;
					}
					FContentDiscoveryItem ActorItem;
					ActorItem.Identity = Actor->GetPathName();
					ActorItem.DisplayName = Actor->GetActorLabel();
					ActorItem.Domain = TEXT("editor_world");
					ActorItem.ContentKind = TEXT("actor");
					ActorItem.ClassPath = Actor->GetClass()->GetPathName();
					ActorItem.OwnerIdentity = World->GetPathName();
					ActorItem.StateLayer = TEXT("editor_world_current");
					ActorItem.DetailCommandId = TEXT("content_graph.items");
					ActorItem.DetailTargetIdentity = World->GetPathName();
					ActorItem.DetailInput = MakeShared<FJsonObject>();
					ActorItem.DetailInput->SetArrayField(
						TEXT("item_ids"),
						{MakeShared<FJsonValueString>(ActorItem.Identity)});
					ActorItem.SupportedOperations = {TEXT("query")};
					ActorItem.SearchTerms = {
						Actor->GetName(),
						ActorItem.ClassPath,
					};
					AllItems.Add(MoveTemp(ActorItem));

					TInlineComponentArray<UActorComponent*> Components(Actor);
					for (UActorComponent* Component : Components)
					{
						if (!IsValid(Component)
							|| !IsValid(Component->GetClass()))
						{
							continue;
						}
						FContentDiscoveryItem ComponentItem;
						ComponentItem.Identity = Component->GetPathName();
						ComponentItem.DisplayName = Component->GetName();
						ComponentItem.Domain = TEXT("editor_world");
						ComponentItem.ContentKind = TEXT("component");
						ComponentItem.ClassPath =
							Component->GetClass()->GetPathName();
						ComponentItem.OwnerIdentity = Actor->GetPathName();
						ComponentItem.StateLayer = TEXT("editor_world_current");
						ComponentItem.DetailCommandId = TEXT("content_graph.properties");
						ComponentItem.DetailTargetIdentity = World->GetPathName();
						ComponentItem.DetailInput = MakeShared<FJsonObject>();
						ComponentItem.DetailInput->SetArrayField(
							TEXT("owner_ids"),
							{MakeShared<FJsonValueString>(ComponentItem.Identity)});
						ComponentItem.SupportedOperations = {TEXT("query")};
						ComponentItem.SearchTerms = {
							Actor->GetActorLabel(),
							ComponentItem.ClassPath,
						};
						AllItems.Add(MoveTemp(ComponentItem));
					}
					if (AllItems.Num() > MaxProviderItems)
					{
						SetError(
							OutError,
							TEXT("content_search_provider_capacity_exceeded"),
							ProviderId,
							FString::FromInt(AllItems.Num()));
						return false;
					}
				}
				AllItems.Sort(
					[](const FContentDiscoveryItem& Left,
					   const FContentDiscoveryItem& Right)
					{
						return Left.Identity < Right.Identity;
					});
				FString RevisionMaterial = World->GetPathName();
				for (const FContentDiscoveryItem& Item : AllItems)
				{
					RevisionMaterial += TEXT("\x1e") + Item.Identity
						+ TEXT("\x1f") + Item.ClassPath;
				}
				const FString Revision = Sha1(RevisionMaterial);
				TArray<FContentDiscoveryItem> Matches;
				for (FContentDiscoveryItem& Item : AllItems)
				{
					Item.Revision = Revision;
					FilterAndScore(Request, MoveTemp(Item), Matches);
				}
				return BuildPage(
					ProviderId,
					Revision,
					MoveTemp(Matches),
					Offset,
					Limit,
					OutPage,
					OutError);
			}

		private:
			FString ProviderId = TEXT("editor.world.content");
			TArray<FString> Kinds;
		};
	}

	bool TryCreateEditorAssetContentDiscoveryProvider(
		TSharedPtr<IUeAgentContentDiscoveryProvider>& OutProvider,
		FString& OutError)
	{
		OutProvider = MakeShared<FEditorAssetContentDiscoveryProvider>();
		OutError.Reset();
		return true;
	}

	bool TryCreateEditorWorldContentDiscoveryProvider(
		TSharedPtr<IUeAgentContentDiscoveryProvider>& OutProvider,
		FString& OutError)
	{
		OutProvider = MakeShared<FEditorWorldContentDiscoveryProvider>();
		OutError.Reset();
		return true;
	}
}
