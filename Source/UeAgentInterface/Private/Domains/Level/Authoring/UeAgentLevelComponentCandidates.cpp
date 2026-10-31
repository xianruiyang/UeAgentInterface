#include "Domains/Level/Authoring/UeAgentLevelComponentCandidates.h"

#include "ComponentAssetBroker.h"
#include "Components/ActorComponent.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringAuthorability.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringCore/UeAgentCandidatePropertyDescription.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UeAgentLevelAuthoring
{
    bool ResolveComponentCreationCandidate(const TSharedPtr<FJsonObject>& Selector,
        FString& OutClassPath, FString& OutAssetOverride, UE::UAI::Contract::FDiagnostic& OutError)
    {
        OutClassPath.Reset();
        OutAssetOverride.Reset();
        UE::UAI::CandidateProperties::FRequest Request;
        if (!UE::UAI::CandidateProperties::ReadRequest(Selector, Request, OutError)) return false;
        UeAgentPaletteSearch::FCandidate Candidate;
        if (!UeAgentPaletteSearch::TryResolveActorComponentCandidate(Request.Name, FString(), Candidate, OutError, Request.Select)) return false;
        OutClassPath = Candidate.AuthoringSelectors.FindRef(TEXT("component_class"));
        OutAssetOverride = Candidate.AuthoringSelectors.FindRef(TEXT("asset_override"));
        UClass* Class = StaticLoadClass(UActorComponent::StaticClass(), nullptr, *OutClassPath);
        if (!Class)
        {
            OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("level_component_candidate_class_unavailable"),
                TEXT("The selected native component class could not be loaded."), TEXT("candidate_resolve"));
            OutError.CandidateName = Request.Name;
            return false;
        }
        const auto Eligibility = EvaluateComponentClassAuthorability(*Class);
        if (!Eligibility.bAuthorable) { OutError = Eligibility.Diagnostic; return false; }
        return true;
    }

    bool QueryComponentCandidates(const TSharedPtr<FJsonObject>& Params,
        FString& OutPayload, UE::UAI::Contract::FDiagnostic& OutError)
    {
        using namespace UE::UAI;
        FString Target;
        Params->TryGetStringField(TEXT("target"), Target);
        AActor* Owner = FindObject<AActor>(nullptr, *Target);
        if (!Owner && !Target.IsEmpty()) Owner = LoadObject<AActor>(nullptr, *Target);
        if (!Owner || !Owner->GetWorld())
        {
            OutError = Contract::FDiagnostic::Failure(TEXT("level_component_candidate_owner_invalid"),
                TEXT("Use the owning Level Actor's object_ref from content_graph.items as target."), TEXT("candidate_query"));
            OutError.TargetPath = Target;
            OutError.Source.JsonPath = TEXT("/target");
            return false;
        }
        const auto Eligibility = EvaluateActorAuthorability(*Owner->GetWorld(), *Owner);
        if (!Eligibility.bAuthorable) { OutError = Eligibility.Diagnostic; return false; }
        const auto Scope = MakeShared<FJsonObject>();
        Scope->SetStringField(TEXT("target"), Owner->GetPathName());
        Scope->SetStringField(TEXT("world"), Owner->GetWorld()->GetPathName());
        if (Params->HasField(TEXT("describe")))
        {
            const TSharedPtr<FJsonObject>* Description = nullptr;
            CandidateProperties::FRequest Request;
            if (!Params->TryGetObjectField(TEXT("describe"), Description))
            {
                OutError = Contract::FDiagnostic::Failure(TEXT("level_component_candidate_description_invalid"),
                    TEXT("describe must be an object containing name, optional select and fields."), TEXT("candidate_query"));
                return false;
            }
            if (!CandidateProperties::ReadRequest(*Description, Request, OutError)) return false;
            FString ClassPath, AssetOverride;
            if (!ResolveComponentCreationCandidate(*Description, ClassPath, AssetOverride, OutError)) return false;
            UClass* Class = StaticLoadClass(UActorComponent::StaticClass(), nullptr, *ClassPath);
            if (!Owner->IsA(Class->ClassWithin))
            {
                OutError = Contract::FDiagnostic::Failure(TEXT("level_component_owner_class_incompatible"),
                    TEXT("The selected component requires a different native outer class."), TEXT("candidate_query"));
                return false;
            }
            UActorComponent* Defaults = Class->GetDefaultObject<UActorComponent>();
            TStrongObjectPtr<AActor> ScratchOwner;
            TStrongObjectPtr<UActorComponent> Prototype;
            if (!AssetOverride.IsEmpty())
            {
                ScratchOwner.Reset(DuplicateObject<AActor>(Owner, GetTransientPackage()));
                Prototype.Reset(NewObject<UActorComponent>(ScratchOwner.Get(), Class));
                Defaults = Prototype.Get();
                UObject* Asset = LoadObject<UObject>(nullptr, *AssetOverride);
                if (!Asset || !FComponentAssetBrokerage::AssignAssetToComponent(Prototype.Get(), Asset))
                {
                    OutError = Contract::FDiagnostic::Failure(TEXT("level_component_candidate_asset_unsupported"),
                        TEXT("The native component asset broker could not assign the selected candidate asset."), TEXT("candidate_query"));
                    OutError.CandidateName = Request.Name;
                    return false;
                }
            }
            UeAgentAuthoringState::FEditablePropertyReadOptions Policy;
            Policy.ExcludedProperties = ComponentStructuralProperties(*Defaults);
            Policy.Limits.bUseInstanceEditability = true;
            TSharedPtr<FJsonObject> Described;
            if (!CandidateProperties::DescribeObject(*Defaults, Request, MoveTemp(Policy), Described, OutError)) return false;
            CandidateProperties::SetCreationSupport(*Described, true,
                TEXT("Add a named row to this Actor's Level components collection; properties may initialize the component in the same import."));
            if (!CandidateProperties::BuildPayload(Request, Described, OutPayload, OutError)) return false;
        }
        else
        {
            UeAgentPaletteSearch::FSearchParams Search;
            Search.CatalogType = TEXT("actor_component");
            Params->TryGetStringField(TEXT("query"), Search.SearchText);
            UeAgentPaletteSearch::FSearchResult Result;
            if (!UeAgentPaletteSearch::ExecuteSearch(Search, Result, OutError)) return false;
            TArray<ContentDiscovery::FNameSelectionEntry> Names;
            for (const auto& Candidate : Result.MatchingCandidates)
            {
                UClass* Class = StaticLoadClass(UActorComponent::StaticClass(), nullptr,
                    *Candidate.AuthoringSelectors.FindRef(TEXT("component_class")));
                if (Class && Owner->IsA(Class->ClassWithin) && EvaluateComponentClassAuthorability(*Class).bAuthorable)
                    Names.Add({Candidate.Name.IsEmpty() ? Candidate.DisplayName : Candidate.Name, Candidate.Identity.CanonicalKey, {}});
            }
            if (!ContentDiscovery::TryBuildCandidateNamesOnlyPayload(Names,
                ContentDiscovery::ENameComparison::IgnoreCase, OutPayload, OutError)) return false;
        }
        return ContentDiscovery::TryAddDiscoveryScope(Scope, OutPayload, OutError);
    }
}
