#include "QueryOwners/AnimationBlueprint/UeAgentAnimationBlueprintQueryOwner.h"

#include "Algo/Sort.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimStateAliasNode.h"
#include "AnimStateConduitNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDocuments.h"
#include "Domains/Animation/UeAgentAnimBlueprintDomainBackend.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "Misc/PackageName.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::QueryOwners::AnimationBlueprint
{
	namespace
	{
		const FString AnimationGraphSurface = TEXT("animation_graph");
		const FString StateMachineGraphSurface = TEXT("state_machine_graph");
		const FString AnimationGraphCoverage =
			TEXT("anim_graph_native_state_machine_node_family");
		const FString StateMachineGraphCoverage =
			TEXT("new_state_machine_blank_graph_native_menu");

		struct FStateMachineActionEntry final
		{
			FAnimationBlueprintCandidate Candidate;
			TSharedPtr<FEdGraphSchemaAction> Action;
		};

		struct FStateMachineActionCatalog final
		{
			TStrongObjectPtr<UEdGraph> ActionOwner;
			TArray<FStateMachineActionEntry> Entries;
		};

		bool ForwardDiagnostic(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
		{
			OutError = MoveTemp(Diagnostic);
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
			return false;
		}

		UE::UAI::Contract::FDiagnostic CandidateDiagnostic(FString Code, FString Subject, FString PublicReason, FString InternalContext = FString())
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(MoveTemp(Code), MoveTemp(PublicReason), TEXT("candidate_query"), MoveTemp(InternalContext), MoveTemp(Subject));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.SourceKind = TEXT("native_result");
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic CandidateCause(FString Code, const FString& Subject, const UE::UAI::Contract::FDiagnostic& Cause, const FString& FallbackReason)
		{
			auto Diagnostic = Cause.IsEmpty() ? CandidateDiagnostic(MoveTemp(Code), Subject, FallbackReason)
				: Contract::FDiagnostic::WrapFailure(MoveTemp(Code), Cause);
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Subject;
			return Diagnostic;
		}

		Contract::FDiagnostic MissingIdentity(const FString& Code, const FAnimationBlueprintCandidate& Candidate)
		{
			auto Diagnostic = CandidateDiagnostic(Code, Candidate.NodeClassPath,
				TEXT("The native animation candidate has no server-side identity."));
			Diagnostic.CandidateName = Candidate.Name;
			Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			Diagnostic.PropertyPath = TEXT("identity_present");
			return Diagnostic;
		}

		void BindDiagnosticTarget(Contract::FDiagnostic& Diagnostic, const FString& NativeTarget, const FString& LogicalTarget)
		{
			if (LogicalTarget.IsEmpty()) return;
			TArray<Contract::FDiagnostic*> Pending{&Diagnostic};
			while (!Pending.IsEmpty())
			{
				auto& Issue = *Pending.Pop(EAllowShrinking::No);
				if (Issue.TargetPath.IsEmpty()) Issue.TargetPath = LogicalTarget;
				else if (!NativeTarget.IsEmpty() && Issue.TargetPath == NativeTarget && NativeTarget != LogicalTarget)
				{
					if (Issue.InternalContext.IsEmpty()) Issue.InternalContext = NativeTarget;
					Issue.TargetPath = LogicalTarget;
				}
				for (auto& Child : Issue.Diagnostics) Pending.Add(&Child);
				if (Issue.PublicCause)
				{
					TSharedPtr<Contract::FDiagnostic> Cause = MakeShared<Contract::FDiagnostic>(*Issue.PublicCause);
					Issue.PublicCause = Cause;
					Pending.Add(Cause.Get());
				}
			}
		}

		FString ObjectPath(const FString& Target)
		{
			if (Target.Contains(TEXT(".")))
			{
				return Target;
			}
			const FString Name = FPackageName::GetLongPackageAssetName(Target);
			return Name.IsEmpty() ? Target : Target + TEXT(".") + Name;
		}

		bool MatchesQuery(
			const FAnimationBlueprintCandidate& Candidate,
			const FString& Query)
		{
			ContentDiscovery::FContentDiscoveryRequest Request;
			Request.Query = Query;
			Request.MatchMode =
				ContentDiscovery::EContentDiscoveryMatchMode::Text;
			const TArray<FString> ExtraTerms{
				Candidate.Name,
				Candidate.Category,
				Candidate.NodeClassPath,
				Candidate.Summary,
				Candidate.ActionKind,
				Candidate.Surface,
				Candidate.Kind,
				Candidate.SourceOrigin,
			};
			int32 MatchScore = 0;
			FString MatchReason;
			return ContentDiscovery::TryScoreContentDiscoveryMatch(
				Request,
				Candidate.Id,
				Candidate.Label,
				ExtraTerms,
				MatchScore,
				MatchReason);
		}

		bool SetCandidateIdentity(
			const FString& CatalogType,
			const FString& Signature,
			FAnimationBlueprintCandidate& Candidate,
			FAnimationBlueprintCandidateQueryError& OutError)
		{
			UeAgentPaletteSearch::FCandidate Identity;
			Identity.Kind = Candidate.Kind;
			Identity.Name = Candidate.Name;
			Identity.DisplayName = Candidate.Label;
			Identity.CategoryPath = Candidate.Category;
			Identity.Tooltip = Candidate.Summary;
			Identity.Type = Candidate.ActionKind;
			Identity.FullName = Candidate.NodeClassPath;
			Identity.ObjectPath = Candidate.NodeClassPath;
			UE::UAI::Contract::FDiagnostic IdentityError;
			if (Candidate.Name.IsEmpty())
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("animation_blueprint_candidate_identity_failed"), Signature,
					TEXT("The native animation candidate has no readable name for its identity."));
				Diagnostic.PropertyPath = TEXT("name");
				Diagnostic.ActualValue = MakeShared<FJsonValueString>(Candidate.Name);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty readable candidate name"));
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}
			if (!UeAgentPaletteSearch::SetCandidateIdentity(
					CatalogType,
					Signature,
					Candidate.NodeClassPath.IsEmpty()
						? Candidate.Name
						: Candidate.NodeClassPath,
					Identity,
					IdentityError))
			{
				return ForwardDiagnostic(OutError, CandidateCause(TEXT("animation_blueprint_candidate_identity_failed"), Candidate.Name, IdentityError,
					TEXT("The native animation candidate could not be assigned an identity.")), Candidate.Name);
			}
			Candidate.Identity = Identity.Identity;
			Candidate.Id = Identity.GetPublicId();
			return true;
		}

		bool BuildAnimationGraphCatalog(
			UAnimBlueprint& Blueprint,
			UEdGraph& Graph,
			TArray<FAnimationBlueprintCandidate>& OutCandidates,
			FAnimationBlueprintCandidateQueryError& OutError)
		{
			UeAgentPaletteSearch::FSearchParams Params;
			Params.CatalogType = TEXT("blueprint_node");
			Params.AssetPath = Blueprint.GetPathName();
			Params.GraphName = Graph.GetName();
			Params.bContextSensitive = true;
			TArray<UeAgentPaletteSearch::FCandidate> RawCandidates;
			TArray<FString> Warnings;
			UE::UAI::Contract::FDiagnostic SearchError;
			if (!UeAgentPaletteSearch::CollectBlueprintCandidates(
					Params, RawCandidates, Warnings, SearchError))
			{
				return ForwardDiagnostic(OutError, CandidateCause(TEXT("animation_blueprint_candidate_catalog_failed"), Graph.GetPathName(), SearchError,
					TEXT("The native animation graph candidate catalog could not be collected.")), Graph.GetPathName());
			}

			OutCandidates.Reset();
			for (const UeAgentPaletteSearch::FCandidate& Raw : RawCandidates)
			{
				UClass* const NodeClass = FindObject<UClass>(
					nullptr, *Raw.ObjectPath);
				if (!NodeClass
					|| !NodeClass->IsChildOf(
						UAnimGraphNode_StateMachineBase::StaticClass()))
				{
					continue;
				}

				FAnimationBlueprintCandidate Candidate;
				Candidate.Identity = Raw.Identity;
				Candidate.Id = Raw.GetPublicId();
				Candidate.Name = Raw.Name;
				Candidate.Label = Raw.DisplayName.IsEmpty()
					? Raw.Name
					: Raw.DisplayName;
				Candidate.Surface = AnimationGraphSurface;
				Candidate.Kind = TEXT("anim_graph_state_machine_node");
				Candidate.ActionKind = TEXT("blueprint_node_spawner");
				Candidate.Category = Raw.CategoryPath;
				Candidate.Summary = Raw.Tooltip;
				Candidate.NodeClassPath = NodeClass->GetPathName();
				Candidate.SourceOrigin = Raw.SourceOrigin;
				Candidate.NativeSelectors = Raw.AuthoringSelectors;
				Candidate.SelectionFields.Add(TEXT("class"), NodeClass->GetName());
				Candidate.SelectionFields.Add(TEXT("class_source"), ContentDiscovery::MakeReadableDiscoveryPath(NodeClass->GetPathName()));
				if (ContentDiscovery::IsReadableDiscoveryName(Raw.SourceOrigin))
					Candidate.SelectionFields.Add(TEXT("source"), Raw.SourceOrigin);
				Candidate.bSupported =
					NodeClass == UAnimGraphNode_StateMachine::StaticClass();
				Candidate.WriteSupport = Candidate.bSupported
					? FString(TEXT("state_machine_create"))
					: FString(TEXT("search_only"));
				Candidate.SupportReason = Candidate.bSupported
					? FString(TEXT("native_blueprint_node_spawner"))
					: FString(TEXT("state_machine_node_variant_not_yet_supported"));
				if (!Candidate.bSupported)
				{
					Candidate.SupportDiagnostic = CandidateDiagnostic(TEXT("animation_blueprint_candidate_not_writable"), Candidate.NodeClassPath,
						TEXT("This animation graph node variant does not support state-machine creation through HJSON."));
					Candidate.SupportDiagnostic.CandidateName = Candidate.Name;
					Candidate.SupportDiagnostic.ActualValue = MakeShared<FJsonValueString>(NodeClass->GetName());
					Candidate.SupportDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(UAnimGraphNode_StateMachine::StaticClass()->GetName());
					Candidate.SupportDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(Candidate.NodeClassPath);
				}
				if (Candidate.Id.IsEmpty())
				{
					return ForwardDiagnostic(OutError, MissingIdentity(TEXT("animation_blueprint_candidate_identity_ambiguous"), Candidate));
				}
				OutCandidates.Add(MoveTemp(Candidate));
			}
			Algo::Sort(OutCandidates,
				[](const FAnimationBlueprintCandidate& A,
					const FAnimationBlueprintCandidate& B)
				{
					const int32 NameOrder = A.Label.Compare(B.Label);
					return NameOrder != 0
						? NameOrder < 0
						: A.NodeClassPath < B.NodeClassPath;
				});
			return true;
		}

		bool BuildStateMachineGraphCatalog(
			UAnimationStateMachineGraph& Graph,
			FStateMachineActionCatalog& OutCatalog,
			FAnimationBlueprintCandidateQueryError& OutError)
		{
			OutCatalog.ActionOwner.Reset();
			OutCatalog.Entries.Reset();
			const UAnimationStateMachineSchema* const Schema =
				Cast<UAnimationStateMachineSchema>(Graph.GetSchema());
			if (!Schema)
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("animation_blueprint_candidate_context_invalid"), Graph.GetPathName(),
					TEXT("The graph does not use the animation state-machine schema."));
				if (Graph.GetSchema()) Diagnostic.ActualValue = MakeShared<FJsonValueString>(Graph.GetSchema()->GetClass()->GetName());
				else Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(UAnimationStateMachineSchema::StaticClass()->GetName());
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}

			FGraphContextMenuBuilder MenuBuilder(&Graph);
			MenuBuilder.FromPin = nullptr;
			Schema->GetGraphContextActions(MenuBuilder);
			OutCatalog.ActionOwner.Reset(MenuBuilder.OwnerOfTemporaries);
			for (int32 Index = 0; Index < MenuBuilder.GetNumActions(); ++Index)
			{
				const TSharedPtr<FEdGraphSchemaAction> Action =
					MenuBuilder.GetSchemaAction(Index);
				if (!Action.IsValid())
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("animation_blueprint_candidate_native_action_invalid"), Graph.GetPathName(),
						TEXT("The state-machine menu returned an empty schema action."));
					Diagnostic.OperationIndex = Index;
					Diagnostic.ActualValuePresent = false;
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("native schema action"));
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
				}

				FStateMachineActionEntry& Entry =
					OutCatalog.Entries.AddDefaulted_GetRef();
				Entry.Action = Action;
				FAnimationBlueprintCandidate& Candidate = Entry.Candidate;
				// Menu descriptions are localized UI text.  TypeId is the stable,
				// readable schema-action semantic name; identity remains server-side.
				Candidate.Name = Action->GetTypeId().ToString();
				Candidate.Label = Candidate.Name;
				Candidate.Surface = StateMachineGraphSurface;
				Candidate.Category = Action->GetCategory().ToString();
				Candidate.Summary = Action->GetTooltipDescription().ToString();
				Candidate.SourceOrigin = TEXT("native_state_machine_schema");
				FString Signature = StateMachineGraphCoverage
					+ TEXT("|") + Action->GetTypeId().ToString();

				TArray<UObject*> ReferencedObjects;
				FReferenceFinder ReferenceFinder(
					ReferencedObjects,
					MenuBuilder.OwnerOfTemporaries,
					true,
					true,
					false,
					false);
				Action->AddReferencedObjects(ReferenceFinder);
				UEdGraphNode* NodeTemplate = nullptr;
				for (UObject* ReferencedObject : ReferencedObjects)
				{
					UEdGraphNode* const ReferencedNode =
						Cast<UEdGraphNode>(ReferencedObject);
					if (!ReferencedNode)
					{
						continue;
					}
					if (NodeTemplate && NodeTemplate != ReferencedNode)
					{
						auto Diagnostic = CandidateDiagnostic(TEXT("animation_blueprint_candidate_native_template_ambiguous"), Candidate.Name,
							TEXT("The native state-machine action references more than one graph-node template."));
						Diagnostic.CandidateName = Candidate.Name;
						Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(2);
						Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
						auto Templates = MakeShared<FJsonObject>();
						Templates->SetStringField(TEXT("first"), NodeTemplate->GetPathName());
						Templates->SetStringField(TEXT("second"), ReferencedNode->GetPathName());
						Diagnostic.ActualEvidence = MakeShared<FJsonValueObject>(Templates);
						return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
					}
					NodeTemplate = ReferencedNode;
				}

				if (NodeTemplate)
				{
					UClass* const NodeClass = NodeTemplate->GetClass();
					Candidate.NodeClassPath = NodeClass->GetPathName();
					Candidate.SelectionFields.Add(TEXT("class"), NodeClass->GetName());
					Candidate.SelectionFields.Add(TEXT("class_source"), ContentDiscovery::MakeReadableDiscoveryPath(Candidate.NodeClassPath));
					Signature += TEXT("|") + Candidate.NodeClassPath;
					Candidate.ActionKind = TEXT("state_machine_schema_new_node");
					if (NodeClass == UAnimStateNode::StaticClass())
					{
						Candidate.Kind = TEXT("animation_state");
						Candidate.bSupported = true;
					}
					else if (NodeClass == UAnimStateAliasNode::StaticClass())
					{
						Candidate.Kind = TEXT("animation_state_alias");
					}
					else if (NodeClass == UAnimStateConduitNode::StaticClass())
					{
						Candidate.Kind = TEXT("animation_state_conduit");
					}
					else if (NodeClass == UAnimStateEntryNode::StaticClass())
					{
						Candidate.Kind = TEXT("animation_state_entry");
					}
					else
					{
						Candidate.Kind = TEXT("animation_state_machine_node");
					}
					const FString KindName = Candidate.Kind == TEXT("animation_state")
						? TEXT("State") : Candidate.Kind == TEXT("animation_state_alias")
						? TEXT("State Alias") : Candidate.Kind == TEXT("animation_state_conduit")
						? TEXT("State Conduit") : Candidate.Kind == TEXT("animation_state_entry")
						? TEXT("State Entry") : TEXT("State Machine Node");
					Candidate.Name = KindName;
					Candidate.Label = KindName;
				}
				else
				{
					Candidate.Kind = TEXT("animation_state_machine_menu_action");
					Candidate.ActionKind = TEXT("state_machine_schema_comment");
					Candidate.Name = TEXT("Comment");
					Candidate.Label = Candidate.Name;
					Signature += TEXT("|") + Candidate.Name;
				}
				Candidate.WriteSupport = Candidate.bSupported
					? FString(TEXT("state_create"))
					: FString(TEXT("search_only"));
				Candidate.SupportReason = Candidate.bSupported
					? FString(TEXT("native_state_machine_schema_action"))
					: FString(TEXT("native_action_has_no_current_hjson_create_contract"));
				if (!Candidate.bSupported)
				{
					Candidate.SupportDiagnostic = CandidateDiagnostic(TEXT("animation_blueprint_candidate_not_writable"), Candidate.NodeClassPath,
						TEXT("This native state-machine action has no HJSON creation contract."));
					Candidate.SupportDiagnostic.CandidateName = Candidate.Name;
					Candidate.SupportDiagnostic.ActualValue = MakeShared<FJsonValueString>(Candidate.Kind);
					Candidate.SupportDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("animation_state"));
					Candidate.SupportDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(Candidate.NodeClassPath);
				}
				if (!SetCandidateIdentity(
						TEXT("animation_state_machine_graph"),
						Signature,
						Candidate,
						OutError))
				{
					return false;
				}
				if (Candidate.Id.IsEmpty())
				{
					return ForwardDiagnostic(OutError, MissingIdentity(TEXT("animation_blueprint_candidate_identity_missing"), Candidate));
				}
			}
			return true;
		}

			template <typename EntryType>
		bool ResolveEntry(
			const TArray<EntryType>& Entries,
			const FString& Name,
			const EntryType*& OutEntry,
			FAnimationBlueprintCandidateQueryError& OutError,
			const TMap<FString, FString>& Select)
		{
			OutEntry = nullptr;
			const FString CleanName = Name.TrimStartAndEnd();
			if (CleanName.IsEmpty() && Select.IsEmpty())
			{
				TArray<ContentDiscovery::FNameSelectionEntry> Choices;
				TArray<int32> SupportedIndices;
				for (const EntryType& Entry : Entries)
				{
					const int32 Index = Choices.Add({Entry.Candidate.Name,
						Entry.Candidate.Identity.CanonicalKey, Entry.Candidate.SelectionFields});
					if (Entry.Candidate.bSupported)
					{
						SupportedIndices.Add(Index);
					}
				}
				// Internal seed selection has the same equivalence rules as public named selection.
				TArray<ContentDiscovery::FNameSelectionGroup> SupportedGroups;
				UE::UAI::Contract::FDiagnostic GroupError;
				if (!ContentDiscovery::TryGroupNameSelectionEntries(Choices, SupportedIndices, SupportedGroups, GroupError))
					return ForwardDiagnostic(OutError, GroupError);
				if (SupportedGroups.Num() == 1)
				{
					OutEntry = &Entries[SupportedGroups[0].EntryIndices[0]];
					return true;
				}
				auto Diagnostic = CandidateDiagnostic(SupportedGroups.IsEmpty()
						? FString(TEXT("animation_blueprint_candidate_not_found"))
						: FString(TEXT("animation_blueprint_candidate_ambiguous")), FString(),
					SupportedGroups.IsEmpty() ? TEXT("The native catalog has no supported candidate for implicit selection.")
						: TEXT("Implicit selection requires exactly one supported candidate; provide a candidate name or selection fields."));
				Diagnostic.PropertyPath = TEXT("supported_candidate_count");
				Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(SupportedGroups.Num());
				Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
				if (!SupportedGroups.IsEmpty()) Diagnostic.SourceKind.Reset();
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}
			{
				TArray<ContentDiscovery::FNameSelectionEntry> Choices;
				for (const EntryType& Entry : Entries)
				{
					Choices.Add({Entry.Candidate.Name, Entry.Candidate.Identity.CanonicalKey, Entry.Candidate.SelectionFields});
				}
				int32 ResolvedIndex = INDEX_NONE;
				UE::UAI::Contract::FDiagnostic ResolveError;
				if (!ContentDiscovery::TryResolveNameSelection(Choices, CleanName, Select, ResolvedIndex, ResolveError, ContentDiscovery::ENameComparison::IgnoreCase))
					return ForwardDiagnostic(OutError, ResolveError, CleanName);
				if (!Entries.IsValidIndex(ResolvedIndex))
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("animation_blueprint_candidate_not_found"), CleanName,
						TEXT("The native candidate resolver returned an index outside the catalog."));
					Diagnostic.CandidateName = CleanName;
					Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(ResolvedIndex);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("index from 0 through %d in a catalog of %d entries"), Entries.Num() - 1, Entries.Num()));
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
				}
				const EntryType& Selected = Entries[ResolvedIndex];
				if (!Selected.Candidate.bSupported)
					return ForwardDiagnostic(OutError, Selected.Candidate.SupportDiagnostic, Selected.Candidate.Name);
				OutEntry = &Selected;
				return true;
			}
		}

		void RetireScratch(TStrongObjectPtr<UAnimBlueprint>& Scratch)
		{
			if (!Scratch.IsValid())
			{
				return;
			}
			Scratch->ClearFlags(RF_Public | RF_Standalone);
			Scratch->MarkAsGarbage();
			Scratch.Reset();
		}

		bool CreateScratch(
			const TSharedPtr<FJsonObject>& ClassSettings,
			TStrongObjectPtr<UAnimBlueprint>& OutScratch,
			FAnimationBlueprintCandidateQueryError& OutError)
		{
			const TSharedPtr<FJsonObject> SeedDocument = MakeShared<FJsonObject>();
			SeedDocument->SetObjectField(
				TEXT("settings"),
				ClassSettings.IsValid()
					? ClassSettings
					: MakeShared<FJsonObject>());
			UE::UAI::Contract::FDiagnostic NativeError;
			UeAgentAnimationAuthoring::FFactorySeedDiagnosticSource SeedSource;
			SeedSource.Settings.JsonPath = TEXT("/class_settings");
			SeedSource.InputSourceKind = TEXT("command_input");
			UObject* const Object = UeAgentAnimationAuthoring::CreateFactorySeed(
				UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint,
				GetTransientPackage(),
				*FString(TEXT("ABP_UAI_Candidate_")
					+ FGuid::NewGuid().ToString(EGuidFormats::Digits)),
				RF_Transient,
				SeedDocument,
				NativeError, SeedSource);
			OutScratch.Reset(Cast<UAnimBlueprint>(Object));
			if (!OutScratch.IsValid())
			{
				return ForwardDiagnostic(OutError, CandidateCause(TEXT("animation_blueprint_candidate_context_create_failed"), TEXT("AnimBlueprint"), NativeError,
					TEXT("The AnimBlueprint factory did not create a candidate-query context.")), TEXT("AnimBlueprint"));
			}
			return true;
		}

		void ProjectPage(
			TArray<FAnimationBlueprintCandidate>&& Catalog,
			const FString& Query,
			FAnimationBlueprintCandidateQueryResult& OutResult,
			FAnimationBlueprintCandidateQueryError& OutError)
		{
			TArray<int32> MatchedIndices;
			for (int32 CatalogIndex = 0; CatalogIndex < Catalog.Num(); ++CatalogIndex)
			{
				if (MatchesQuery(Catalog[CatalogIndex], Query))
				{
					MatchedIndices.Add(CatalogIndex);
				}
			}
			for (int32 MatchIndex = 0; MatchIndex < MatchedIndices.Num(); ++MatchIndex)
			{
				FAnimationBlueprintCandidate& Candidate =
					Catalog[MatchedIndices[MatchIndex]];
				OutResult.Candidates.Add(MoveTemp(Candidate));
			}
			OutResult.TotalMatches = MatchedIndices.Num();
			if (!OutResult.CatalogDiagnostic.IsEmpty() && OutResult.CatalogDiagnostic.TargetPath.IsEmpty())
				OutResult.CatalogDiagnostic.TargetPath = OutResult.Target;
		}
	}

	bool TryQueryAnimationBlueprintCandidates(
		const FString& Target,
		const FString& Surface,
		const TSharedPtr<FJsonObject>& ClassSettings,
		const FString& Query,
		FAnimationBlueprintCandidateQueryResult& OutResult,
		FAnimationBlueprintCandidateQueryError& OutError)
	{
		OutResult = {};
		OutError = {};
		FString NativeTarget;
		auto FailQuery = [&](Contract::FDiagnostic Diagnostic)
		{
			BindDiagnosticTarget(Diagnostic, NativeTarget, Target.TrimStartAndEnd());
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
		};
		if (!IsInGameThread())
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("animation_blueprint_candidate_game_thread_required"), Target,
				TEXT("Animation candidate discovery requires the game thread."));
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(TEXT("other_thread"));
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("game_thread"));
			return FailQuery(MoveTemp(Diagnostic));
		}
		if (Target.TrimStartAndEnd().IsEmpty())
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("animation_blueprint_candidate_target_required"), FString(),
				TEXT("Animation candidate discovery requires a non-empty target asset reference."));
			Diagnostic.SourceKind = TEXT("command_input");
			Diagnostic.Source.JsonPath = TEXT("/target");
			Diagnostic.PropertyPath = TEXT("target");
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(Target);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty asset reference"));
			return FailQuery(MoveTemp(Diagnostic));
		}
		const FString CleanSurface = Surface.TrimStartAndEnd().ToLower();
		if (CleanSurface != AnimationGraphSurface
			&& CleanSurface != StateMachineGraphSurface)
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("animation_blueprint_candidate_surface_invalid"), Surface,
				TEXT("The requested animation candidate surface is not supported."));
			Diagnostic.SourceKind = TEXT("command_input");
			Diagnostic.Source.JsonPath = TEXT("/surface");
			Diagnostic.PropertyPath = TEXT("surface");
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(Surface);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
				MakeShared<FJsonValueString>(AnimationGraphSurface), MakeShared<FJsonValueString>(StateMachineGraphSurface)});
			return FailQuery(MoveTemp(Diagnostic));
		}
		if (StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath(Target)))
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("animation_blueprint_candidate_target_already_exists"), Target,
				TEXT("Animation candidate discovery currently requires a target asset that does not yet exist."));
			Diagnostic.PropertyPath = TEXT("target_exists");
			Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(true);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
			return FailQuery(MoveTemp(Diagnostic));
		}

		TStrongObjectPtr<UAnimBlueprint> Scratch;
		if (!CreateScratch(ClassSettings, Scratch, OutError))
		{
			return FailQuery(MoveTemp(OutError));
		}
		NativeTarget = Scratch->GetPathName();
		UeAgentBlueprintAssetLifecycle::SetAuthoringContext(*Scratch, Target.TrimStartAndEnd(), nullptr);
		UEdGraph* const AnimGraph =
			UeAgentAnimBlueprintOps::ResolveAnyAnimBlueprintGraph(
				Scratch.Get(), TEXT("AnimGraph"));
		if (!AnimGraph)
		{
			RetireScratch(Scratch);
			auto Diagnostic = CandidateDiagnostic(TEXT("animation_blueprint_candidate_context_invalid"), Target,
				TEXT("The AnimBlueprint factory seed did not create its animation graph."));
			Diagnostic.ActualValuePresent = false;
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("AnimGraph"));
			return FailQuery(MoveTemp(Diagnostic));
		}

		TArray<FAnimationBlueprintCandidate> Catalog;
		if (CleanSurface == AnimationGraphSurface)
		{
			if (!BuildAnimationGraphCatalog(
					*Scratch, *AnimGraph, Catalog, OutError))
			{
				RetireScratch(Scratch);
				return FailQuery(MoveTemp(OutError));
			}
		}
		else
		{
			UEdGraphNode* MachineNode = nullptr;
			FAnimationBlueprintCandidate MachineCandidate;
			if (!TryPerformAnimationGraphStateMachineCandidate(
					*Scratch,
					*AnimGraph,
					FString(),
					FVector2f::ZeroVector,
					MachineNode,
					MachineCandidate,
					OutError))
			{
				RetireScratch(Scratch);
				return FailQuery(MoveTemp(OutError));
			}
			UAnimGraphNode_StateMachine* const StateMachineNode =
				Cast<UAnimGraphNode_StateMachine>(MachineNode);
			if (!StateMachineNode || !StateMachineNode->EditorStateMachineGraph)
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("animation_blueprint_candidate_context_invalid"), Target,
					!StateMachineNode ? TEXT("The native spawner did not create an animation StateMachine node.")
						: TEXT("The native StateMachine node has no editor state-machine graph."));
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetBoolField(TEXT("state_machine_node_present"), StateMachineNode != nullptr);
				Actual->SetBoolField(TEXT("state_machine_graph_present"), StateMachineNode && StateMachineNode->EditorStateMachineGraph != nullptr);
				Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("StateMachine node with an editor state-machine graph"));
				RetireScratch(Scratch);
				return FailQuery(MoveTemp(Diagnostic));
			}
			FStateMachineActionCatalog NativeCatalog;
			if (!BuildStateMachineGraphCatalog(
					*StateMachineNode->EditorStateMachineGraph,
					NativeCatalog,
					OutError))
			{
				RetireScratch(Scratch);
				return FailQuery(MoveTemp(OutError));
			}
			for (FStateMachineActionEntry& Entry : NativeCatalog.Entries)
			{
				Catalog.Add(MoveTemp(Entry.Candidate));
			}
		}

		OutResult.Target = Target.TrimStartAndEnd();
		OutResult.TargetRevision = TEXT("missing");
		OutResult.Surface = CleanSurface;
		ProjectPage(
			MoveTemp(Catalog),
			Query,
			OutResult,
			OutError);
		RetireScratch(Scratch);
		return OutError.Code.IsEmpty();
	}

	bool TryPerformAnimationGraphStateMachineCandidate(
		UAnimBlueprint& Blueprint,
		UEdGraph& Graph,
		const FString& Name,
		const FVector2f& Location,
		UEdGraphNode*& OutNode,
		FAnimationBlueprintCandidate& OutCandidate,
		FAnimationBlueprintCandidateQueryError& OutError,
		const TMap<FString, FString>& Select)
	{
		OutNode = nullptr;
		OutCandidate = {};
		OutError = {};
		auto ForwardFailure = [&](Contract::FDiagnostic Diagnostic)
		{
			BindDiagnosticTarget(Diagnostic, Blueprint.GetPathName(), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
		};
		TArray<FAnimationBlueprintCandidate> Catalog;
		if (!BuildAnimationGraphCatalog(
				Blueprint, Graph, Catalog, OutError))
		{
			return ForwardFailure(MoveTemp(OutError));
		}
		struct FProjectedEntry final
		{
			FAnimationBlueprintCandidate Candidate;
		};
		TArray<FProjectedEntry> Entries;
		for (FAnimationBlueprintCandidate& Candidate : Catalog)
		{
			Entries.Add({MoveTemp(Candidate)});
		}
		const FProjectedEntry* Selected = nullptr;
		if (!ResolveEntry(Entries, Name, Selected, OutError, Select))
		{
			OutError = UeAgentAuthoringDiagnostics::CandidateFailure(OutError.Code, Name, UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), FString::Printf(TEXT("animation graph '%s'"), *Graph.GetName()), TEXT("animation.state_machine.candidates"), OutError);
			return false;
		}

		UeAgentPaletteSearch::FSearchParams Params;
		Params.CatalogType = TEXT("blueprint_node");
		Params.AssetPath = Blueprint.GetPathName();
		Params.GraphName = Graph.GetName();
		Params.bContextSensitive = true;
		Params.RequiredAuthoringSelectors = Selected->Candidate.NativeSelectors;
		UeAgentPaletteSearch::FCandidate NativeCandidate;
		UE::UAI::Contract::FDiagnostic NativeError;
		if (!UeAgentPaletteSearch::TryPerformBlueprintActionCandidate(
				Params,
				Selected->Candidate.Name,
				FString(),
				Location,
				OutNode,
				NativeCandidate,
				NativeError))
		{
			return ForwardFailure(MoveTemp(NativeError));
		}
		OutCandidate = Selected->Candidate;
		return true;
	}

	bool TryPerformStateMachineGraphCandidate(
		UAnimationStateMachineGraph& Graph,
		const FString& Name,
		const FVector2f& Location,
		UEdGraphNode*& OutNode,
		FAnimationBlueprintCandidate& OutCandidate,
		FAnimationBlueprintCandidateQueryError& OutError,
		const TMap<FString, FString>& Select)
	{
		OutNode = nullptr;
		OutCandidate = {};
		OutError = {};
		const UBlueprint* Blueprint = Graph.GetTypedOuter<UBlueprint>();
		const FString TargetPath = Blueprint ? UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint) : FString();
		auto ForwardFailure = [&](Contract::FDiagnostic Diagnostic)
		{
			BindDiagnosticTarget(Diagnostic, Blueprint ? Blueprint->GetPathName() : FString(), TargetPath);
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
		};
		FStateMachineActionCatalog Catalog;
		if (!BuildStateMachineGraphCatalog(Graph, Catalog, OutError))
		{
			return ForwardFailure(MoveTemp(OutError));
		}
		const FStateMachineActionEntry* Selected = nullptr;
		if (!ResolveEntry(Catalog.Entries, Name, Selected, OutError, Select))
		{
			OutError = UeAgentAuthoringDiagnostics::CandidateFailure(OutError.Code, Name, TargetPath, TEXT("state-machine graph"), TEXT("animation.state_machine.candidates"), OutError);
			return false;
		}
		TSet<UEdGraphNode*> ExistingNodes;
		for (UEdGraphNode* Existing : Graph.Nodes)
		{
			if (Existing)
			{
				ExistingNodes.Add(Existing);
			}
		}
		OutNode = Selected->Action->PerformAction(
			&Graph, nullptr, Location, false);
		TArray<UEdGraphNode*> CreatedNodes;
		for (UEdGraphNode* Current : Graph.Nodes)
		{
			if (Current && !ExistingNodes.Contains(Current))
			{
				CreatedNodes.Add(Current);
			}
		}
		if (!OutNode)
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("animation_blueprint_candidate_create_failed"), Selected->Candidate.Id,
				TEXT("The native state-machine action did not return a graph node."));
			Diagnostic.CandidateName = Selected->Candidate.Name;
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetBoolField(TEXT("returned_node_present"), false);
			Actual->SetNumberField(TEXT("new_nodes"), CreatedNodes.Num());
			Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("one returned graph node"));
			Diagnostic.MutationMayHaveStarted = true;
			if (!CreatedNodes.IsEmpty()) Diagnostic.MutationStarted = true;
			return ForwardFailure(MoveTemp(Diagnostic));
		}
		if (CreatedNodes.Num() != 1 || CreatedNodes[0] != OutNode)
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("animation_blueprint_candidate_result_shape_unsupported"), Selected->Candidate.Id,
				CreatedNodes.Num() != 1 ? TEXT("The native state-machine action did not create exactly one graph node.")
					: TEXT("The returned state-machine node differs from the graph's newly created node."));
			Diagnostic.CandidateName = Selected->Candidate.Name;
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetNumberField(TEXT("new_nodes"), CreatedNodes.Num());
			Actual->SetBoolField(TEXT("returned_node_is_new"), CreatedNodes.Contains(OutNode));
			auto Expected = MakeShared<FJsonObject>();
			Expected->SetNumberField(TEXT("new_nodes"), 1);
			Expected->SetBoolField(TEXT("returned_node_is_new"), true);
			Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
			Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(OutNode->GetPathName());
			Diagnostic.MutationMayHaveStarted = true;
			if (!CreatedNodes.IsEmpty()) Diagnostic.MutationStarted = true;
			OutNode = nullptr;
			return ForwardFailure(MoveTemp(Diagnostic));
		}
		OutCandidate = Selected->Candidate;
		return true;
	}

	bool IsReadOnlyContentGraphCommand(const FString& CommandId)
	{
		return CommandId.StartsWith(TEXT("content_graph."))
			&& CommandId != TEXT("content_graph.import_workspace");
	}
}
