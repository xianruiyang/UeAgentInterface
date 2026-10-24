#include "DomainAdapters/Blueprint/Component/UeAgentBlueprintComponentMechanics.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"

#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/BlueprintUmgGraph/UeAgentBlueprintUmgGraphNativeHook.h"
#include "Dom/JsonObject.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonValue.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringOps.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"

namespace UE::UAI::DomainAdapters::Blueprint::Component
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::DomainAdapters::BlueprintUmgGraph;

	namespace
	{
		bool ForwardDiagnostic(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
		{
			OutError = MoveTemp(Diagnostic);
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
			return false;
		}

		UE::UAI::Contract::FDiagnostic QueryFact(const FString& Code, const FString& Target,
			const FString& Reason, const FString& Path, TSharedPtr<FJsonValue> Actual,
			TSharedPtr<FJsonValue> Expected, const FString& Name = FString(), const FString& Kind = TEXT("native_result"))
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("candidate_query"));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.TargetPath = Target;
			Diagnostic.CandidateName = Name;
			Diagnostic.PropertyPath = Path;
			Diagnostic.SourceKind = Kind;
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			return Diagnostic;
		}

		

		UBlueprint* LoadBlueprint(const FString& Target,
			FUeAgentBlueprintComponentMechanicsError& OutError)
		{
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Target);
			if (!Blueprint || !Blueprint->SimpleConstructionScript)
			{
				ForwardDiagnostic(OutError, QueryFact(TEXT("blueprint_component_target_unresolved"), Target,
					Blueprint ? TEXT("The Blueprint target has no Simple Construction Script.") : TEXT("The Blueprint target could not be loaded."),
					Blueprint ? TEXT("/scs/available") : TEXT("/target/available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), Target);
				return nullptr;
			}
			return Blueprint;
		}

		bool TryCaptureScs(UBlueprint& Blueprint, FString& OutRevision,
			FString& OutEvidence,
			FUeAgentBlueprintComponentMechanicsError& OutError)
		{
			FBlueprintUmgGraphTopology Topology;
			FBlueprintUmgGraphAdapterError AdapterError;
			FBlueprintUmgGraphNativeHook Hook;
			if (!Hook.TryCaptureBlueprintScs(Blueprint, Blueprint.GetPathName(),
					Topology, OutRevision, OutEvidence, AdapterError))
			{
				return ForwardDiagnostic(OutError, MoveTemp(AdapterError), Blueprint.GetPathName());
			}
			return true;
		}

		TSharedPtr<FJsonObject> VectorObject(const FVector& Value)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("x"), Value.X);
			Result->SetNumberField(TEXT("y"), Value.Y);
			Result->SetNumberField(TEXT("z"), Value.Z);
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> BuildExistingComponents(
			const UBlueprint& Blueprint)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			if (!Blueprint.SimpleConstructionScript)
			{
				return Result;
			}
			TMap<const USCS_Node*, const USCS_Node*> Parents;
			TMap<const USCS_Node*, int32> Orders;
			const TArray<USCS_Node*>& Roots = Blueprint.SimpleConstructionScript->GetRootNodes();
			for (int32 Index = 0; Index < Roots.Num(); ++Index)
			{
				if (Roots[Index])
				{
					Orders.Add(Roots[Index], Index);
				}
			}
			for (USCS_Node* Parent : Blueprint.SimpleConstructionScript->GetAllNodes())
			{
				if (!Parent)
				{
					continue;
				}
				const TArray<USCS_Node*>& Children = Parent->GetChildNodes();
				for (int32 Index = 0; Index < Children.Num(); ++Index)
				{
					if (Children[Index])
					{
						Parents.Add(Children[Index], Parent);
						Orders.Add(Children[Index], Index);
					}
				}
			}

			TArray<USCS_Node*> Nodes = Blueprint.SimpleConstructionScript->GetAllNodes();
			Nodes.Sort([](const USCS_Node& Left, const USCS_Node& Right)
			{
				return Left.GetVariableName().LexicalLess(Right.GetVariableName());
			});
			for (USCS_Node* Node : Nodes)
			{
				if (!Node)
				{
					continue;
				}
				const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
				Entry->SetStringField(TEXT("component_class"), Node->ComponentTemplate
					? Node->ComponentTemplate->GetClass()->GetPathName() : FString());
				Entry->SetStringField(TEXT("variable_guid"), Node->VariableGuid.IsValid()
					? Node->VariableGuid.ToString(EGuidFormats::DigitsWithHyphensLower)
					: FString());
				const USCS_Node* const* Parent = Parents.Find(Node);
				if (Parent && *Parent)
				{
					Entry->SetStringField(TEXT("parent"),
						(*Parent)->GetVariableName().ToString());
				}
				else
				{
					Entry->SetField(TEXT("parent"), MakeShared<FJsonValueNull>());
				}
				Entry->SetNumberField(TEXT("order"), Orders.FindRef(Node));
				const TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				if (const USceneComponent* Scene = Cast<USceneComponent>(Node->ComponentTemplate))
				{
					Properties->SetObjectField(TEXT("relative_location"),
						VectorObject(Scene->GetRelativeLocation()));
				}
				if (const UPointLightComponent* Light = Cast<UPointLightComponent>(Node->ComponentTemplate))
				{
					Properties->SetNumberField(TEXT("intensity"), Light->Intensity);
				}
				Entry->SetObjectField(TEXT("properties"), Properties);
				Result.Add(MakeShared<FJsonValueObject>(Entry));
			}
			return Result;
		}
	}

	bool TryQueryBlueprintComponentCandidates(
		const FUeAgentBlueprintComponentCandidateQuery& Query,
		FString& OutPayloadJson,
		FUeAgentBlueprintComponentMechanicsError& OutError)
	{
		OutPayloadJson.Reset();
		OutError = {};
		ON_SCOPE_EXIT
		{
			if (OutError.IsValid() && OutError.TargetPath.IsEmpty()) OutError.TargetPath = Query.Target.TrimStartAndEnd();
		};
		FString Target = Query.Target.TrimStartAndEnd();
		UE::UAI::ContentDiscovery::FContentDiscoveryError TargetError;
		if (!UE::UAI::DomainAdapters::ContentDiscovery::TryResolveEditorContentReference(Target, TEXT("asset"), TargetError))
			return ForwardDiagnostic(OutError, TargetError, Target);
		const FString Parent = Query.Parent.TrimStartAndEnd();
		UBlueprint* Blueprint = LoadBlueprint(Target, OutError);
		if (!Blueprint)
		{
			return false;
		}
		if (!Parent.IsEmpty()
			&& !UeAgentBlueprintAuthoringOps::FindSCSNodeByAnyName(Blueprint->SimpleConstructionScript, Parent)
			&& !Cast<USceneComponent>(UeAgentBlueprintAuthoringOps::ResolveInheritedComponent(*Blueprint, Parent)))
		{
			auto Diagnostic = QueryFact(TEXT("blueprint_component_parent_unresolved"), Target,
				TEXT("The parent is neither a local SCS component nor an inherited scene component."),
				TEXT("/parent"), MakeShared<FJsonValueString>(Parent), MakeShared<FJsonValueString>(TEXT("local or inherited scene component name")), Parent, TEXT("command_input"));
			Diagnostic.Source.JsonPath = TEXT("/parent");
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Parent);
		}
		UeAgentPaletteSearch::FSearchParams SearchParams;
		SearchParams.CatalogType = TEXT("actor_component");
		SearchParams.SearchText = Query.Query.TrimStartAndEnd();
		SearchParams.AssetPath = Blueprint->GetPathName();
		SearchParams.bContextSensitive = true;
		UeAgentPaletteSearch::FSearchResult SearchResult;
		UE::UAI::Contract::FDiagnostic SearchError;
		if (!UeAgentPaletteSearch::ExecuteSearch(SearchParams, SearchResult, SearchError))
		{
			return ForwardDiagnostic(OutError, MoveTemp(SearchError), Target);
		}
		TArray<UE::UAI::ContentDiscovery::FNameSelectionEntry> Entries;
		UE::UAI::Contract::FDiagnostic ProjectionError;
		Entries.Reserve(SearchResult.MatchingCandidates.Num());
		for (const UeAgentPaletteSearch::FCandidate& Match : SearchResult.MatchingCandidates)
		{
			Entries.Add({Match.Name.IsEmpty() ? Match.DisplayName : Match.Name, Match.Identity.CanonicalKey, {}});
		}
		UE::UAI::ContentDiscovery::FNameDiscoveryCompleteness Completeness;
		Completeness.TotalMatches = SearchResult.TotalMatching;
		Completeness.ReturnedCount = Entries.Num();
		if (!UE::UAI::ContentDiscovery::TryValidateNameDiscoveryCompleteness(
				Completeness, ProjectionError))
		{
			return ForwardDiagnostic(OutError, MoveTemp(ProjectionError), FString());
		}
		if (!UE::UAI::ContentDiscovery::TryBuildCandidateNamesOnlyPayload(
			Entries,
			UE::UAI::ContentDiscovery::ENameComparison::IgnoreCase,
			OutPayloadJson, ProjectionError))
		{
			return ForwardDiagnostic(OutError, MoveTemp(ProjectionError), FString());
		}
		const auto Scope = MakeShared<FJsonObject>();
		Scope->SetStringField(TEXT("target"), Target);
		if (!Parent.IsEmpty()) Scope->SetStringField(TEXT("parent"), Parent);
		if (!UE::UAI::ContentDiscovery::TryAddDiscoveryScope(Scope, OutPayloadJson, ProjectionError))
			return ForwardDiagnostic(OutError, MoveTemp(ProjectionError), Target);
		return true;
	}
}
