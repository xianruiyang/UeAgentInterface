#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/ActorComponent.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
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
        FScopedNameDiscoveryEntry MakeObjectDiscoveryEntry(const UObject& Object)
        {
            const AActor* Actor = Cast<AActor>(&Object);
            const UActorComponent* Component = Cast<UActorComponent>(&Object);
            FScopedNameDiscoveryEntry Entry;
            Entry.Identity = Object.GetPathName();
            Entry.Name = Actor ? Actor->GetActorLabel() : Object.GetName();
            Entry.Scopes.Add(Actor ? TEXT("actor") : Component ? TEXT("component") : TEXT("object"));
            const FString OwnerScope = Actor ? Actor->GetClass()->GetDisplayNameText().ToString()
                : Component && Component->GetOwner() ? Component->GetOwner()->GetActorLabel() : FString();
            if (IsReadableDiscoveryName(OwnerScope)) Entry.Scopes.Add(OwnerScope);
            const UObject* ScopeOwner = Actor ? &Object
                : Component && Component->GetOwner() ? Component->GetOwner() : Object.GetOuter();
            AppendReadableDiscoveryPathScopes(ScopeOwner ? ScopeOwner->GetPathName() : Entry.Identity, Entry.Scopes);
            return Entry;
        }
    }

    bool TryResolveScopedObjectReference(TConstArrayView<UObject*> Objects,
        const FString& Reference, UObject*& OutObject, UE::UAI::Contract::FDiagnostic& OutError)
    {
        OutObject = nullptr;
        OutError.Reset();
        TArray<UObject*> Candidates;
        TArray<FScopedNameDiscoveryEntry> Names;
        for (UObject* Object : Objects)
        {
            if (!IsValid(Object)) continue;
            if (Object->GetPathName() == Reference) { OutObject = Object; return true; }
            FScopedNameDiscoveryEntry Entry = MakeObjectDiscoveryEntry(*Object);
            Names.Add(Entry);
            Candidates.Add(Object);
            // Object name and Actor label are alternate representations of the
            // same native identity, resolved by the same owner as discovery.
            if (Object->GetName() != Entry.Name)
            {
                Entry.Name = Object->GetName();
                Names.Add(MoveTemp(Entry));
                Candidates.Add(Object);
            }
        }
        int32 Index = INDEX_NONE;
        if (TryResolveScopedDiscoveryName(Names, Reference, ENameComparison::IgnoreCase, Index, OutError))
        {
            OutObject = Candidates[Index];
            return true;
        }
        if (OutError.Code == TEXT("name_discovery_semantic_scope_ambiguous"))
            OutError.Code = TEXT("object_reference_ambiguous");
        OutError.TargetPath = Reference;
        OutError.SourceKind = TEXT("command_input");
        return false;
    }

	bool TryResolveEditorContentReference(FString& InOutReference,
		const FString& RequiredKind, FContentDiscoveryError& OutError)
	{
		OutError = {};
		InOutReference.TrimStartAndEndInline();
		if (InOutReference.StartsWith(TEXT("/")) || InOutReference.StartsWith(TEXT("uai://"))) return true;
		DomainCapability::FDomainCapabilityAuthorities Authorities;
		UE::UAI::Contract::FDiagnostic AuthorityError;
		if (!DomainCapability::TryGetOrBuildProcessDomainCapabilityAuthorities(Authorities, AuthorityError)
			|| !Authorities.GetContentDiscoveryService())
		{
			OutError = AuthorityError.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("content_search_authority_unavailable"), TEXT("The content search service is unavailable."), TEXT("content_search")) : AuthorityError;
			return false;
		}
		FContentDiscoveryItem Item;
		if (!Authorities.GetContentDiscoveryService()->TryResolveReadableName(InOutReference, Item, OutError)) return false;
		if (!RequiredKind.IsEmpty() && Item.ContentKind != RequiredKind)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("content_reference_kind_mismatch"), FString::Printf(TEXT("Expected content kind %s, but the selected content is %s."), *RequiredKind, *Item.ContentKind), TEXT("content_search"));
			OutError.SubjectId = InOutReference;
			OutError.CandidateName = InOutReference;
			OutError.PropertyPath = TEXT("content_kind");
			OutError.ActualValue = MakeShared<FJsonValueString>(Item.ContentKind);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(RequiredKind);
			return false;
		}
		InOutReference = Item.Identity;
		return true;
	}

	namespace
	{
		void SetError(FContentDiscoveryError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId = FString())
        { OutError = MoveTemp(Diagnostic); if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SubjectId; }

		FString Sha1(const FString& Value)
		{
			const FTCHARToUTF8 Utf8(*Value);
			uint8 Digest[FSHA1::DigestSize]{};
			FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
			return TEXT("sha1:")
				+ BytesToHex(Digest, FSHA1::DigestSize).ToLower();
		}

		void AddPublicScopeIfReadable(
			FContentDiscoveryItem& Item,
			const FString& Scope)
		{
			if (IsReadableDiscoveryName(Scope))
			{
				Item.PublicDiscoveryScopes.AddUnique(Scope);
			}
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
				FContentDiscoveryProviderPage& OutPage,
				FContentDiscoveryError& OutError) override
			{
				OutPage = {};
				OutError = {};
				if (!IsInGameThread())
				{
					SetError(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("content_search_thread_invalid"), TEXT("Asset discovery must run on the Unreal game thread."), TEXT("content_search")), ProviderId);
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
				TArray<FContentDiscoveryItem> Items;
				for (const FAssetData& Asset : Assets)
				{
					FContentDiscoveryItem Item;
					Item.Identity = Asset.GetObjectPathString();
					Item.DisplayName = Asset.AssetName.ToString();
					Item.Domain = TEXT("asset_registry");
					Item.ContentKind = TEXT("asset");
					Item.ClassPath = Asset.AssetClassPath.ToString();
					Item.OwnerIdentity = Asset.PackageName.ToString();
					AddPublicScopeIfReadable(Item, Item.ContentKind);
					AddPublicScopeIfReadable(
						Item, Asset.AssetClassPath.GetAssetName().ToString());
					AppendReadableDiscoveryPathScopes(Asset.PackageName.ToString(), Item.PublicDiscoveryScopes);
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
					Items.Add(MoveTemp(Item));
				}
				return TryBuildContentDiscoveryProviderPage(
					ProviderId,
					Revision,
					Request,
					MoveTemp(Items),
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
				FContentDiscoveryProviderPage& OutPage,
				FContentDiscoveryError& OutError) override
			{
				OutPage = {};
				OutError = {};
				if (!IsInGameThread())
				{
					SetError(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("content_search_thread_invalid"), TEXT("Actor and component discovery must run on the Unreal game thread."), TEXT("content_search")), ProviderId);
					return false;
				}
				UWorld* World = GEditor
					? GEditor->GetEditorWorldContext().World()
					: nullptr;
				if (!IsValid(World))
				{
					SetError(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("content_search_world_unavailable"), TEXT("Actor and component discovery requires an available Editor world."), TEXT("content_search")), ProviderId);
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
					const auto ActorName = MakeObjectDiscoveryEntry(*Actor);
					ActorItem.Identity = ActorName.Identity;
					ActorItem.DisplayName = ActorName.Name;
					ActorItem.PublicDiscoveryScopes = ActorName.Scopes;
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
						const auto ComponentName = MakeObjectDiscoveryEntry(*Component);
						ComponentItem.Identity = ComponentName.Identity;
						ComponentItem.DisplayName = ComponentName.Name;
						ComponentItem.PublicDiscoveryScopes = ComponentName.Scopes;
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
				for (FContentDiscoveryItem& Item : AllItems)
				{
					Item.Revision = Revision;
				}
				return TryBuildContentDiscoveryProviderPage(
					ProviderId,
					Revision,
					Request,
					MoveTemp(AllItems),
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutProvider = MakeShared<FEditorAssetContentDiscoveryProvider>();
		OutError.Reset();
		return true;
	}

	bool TryCreateEditorWorldContentDiscoveryProvider(
		TSharedPtr<IUeAgentContentDiscoveryProvider>& OutProvider,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutProvider = MakeShared<FEditorWorldContentDiscoveryProvider>();
		OutError.Reset();
		return true;
	}
}
