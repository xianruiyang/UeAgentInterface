#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"

#include "AIGraphTypes.h"
#include "AIGraphSchema.h"
#include "Algo/Sort.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/Decorators/BTDecorator_BlueprintBase.h"
#include "BehaviorTree/Services/BTService_BlueprintBase.h"
#include "BehaviorTree/Tasks/BTTask_BlueprintBase.h"
#include "Blueprint/BlueprintSupport.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "Blueprint/StateTreeConditionBlueprintBase.h"
#include "Blueprint/StateTreeEvaluatorBlueprintBase.h"
#include "Blueprint/StateTreeConsiderationBlueprintBase.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"
#include "Core/ContentDiscovery/UeAgentNativeNodeAction.h"
#include "Core/ContentDiscovery/UeAgentNativeTypeDiscovery.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQueryGraph.h"
#include "EnvironmentQueryGraphNode_Option.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "StructUtils/UserDefinedStruct.h"
#include "QueryOwners/AiBehavior/UeAgentEqsHeadlessSchema.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeEditorTypes.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeDecoratorGraph.h"
#include "BehaviorTreeDecoratorGraphNode.h"
#include "SmartObjectDefinition.h"
#include "SmartObjectAnnotation.h"
#include "StateTree.h"
#include "StateTreeConditionBase.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeTaskBase.h"
#include "StateTreeConsiderationBase.h"
#include "StateTreePropertyFunctionBase.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorModule.h"
#include "StateTreeNodeClassCache.h"
#include "StateTreeSchema.h"
#include "QueryOwners/AiBehavior/UeAgentBehaviorTreeHeadlessSchema.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include <openssl/sha.h>

void UUeAgentBehaviorTreeHeadlessSchema::InitializeHeadlessClassCache()
{
	if (HeadlessClassCache.IsValid())
	{
		return;
	}
	HeadlessClassCache = MakeShared<FGraphNodeClassHelper>(
		UBTNode::StaticClass());
	FGraphNodeClassHelper::AddObservedBlueprintClasses(
		UBTTask_BlueprintBase::StaticClass());
	FGraphNodeClassHelper::AddObservedBlueprintClasses(
		UBTDecorator_BlueprintBase::StaticClass());
	FGraphNodeClassHelper::AddObservedBlueprintClasses(
		UBTService_BlueprintBase::StaticClass());
	HeadlessClassCache->UpdateAvailableBlueprintClasses();
}

FGraphNodeClassHelper&
UUeAgentBehaviorTreeHeadlessSchema::GetClassCache() const
{
	check(HeadlessClassCache.IsValid());
	return *HeadlessClassCache;
}

FGraphNodeClassHelper&
UUeAgentBehaviorTreeHeadlessSchema::GetHeadlessClassCache() const
{
	return GetClassCache();
}

void UUeAgentBehaviorTreeDecoratorHeadlessSchema::SetClassCacheOwner(UUeAgentBehaviorTreeHeadlessSchema& Owner)
{
	ClassCacheOwner = &Owner;
}

FGraphNodeClassHelper& UUeAgentBehaviorTreeDecoratorHeadlessSchema::GetClassCache() const
{
	check(ClassCacheOwner);
	return ClassCacheOwner->GetHeadlessClassCache();
}

namespace UE::UAI::QueryOwners::AiBehavior
{
	using namespace ContentDiscovery;
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
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

		UE::UAI::Contract::FDiagnostic CandidateCause(FString Code, const FString& Subject,
			const UE::UAI::Contract::FDiagnostic& Cause, FString FallbackReason)
		{
			auto Diagnostic = Cause.IsEmpty()
				? CandidateDiagnostic(MoveTemp(Code), Subject, MoveTemp(FallbackReason))
				: UE::UAI::Contract::FDiagnostic::WrapFailure(MoveTemp(Code), Cause);
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Subject;
			return Diagnostic;
		}

		Contract::FDiagnostic MissingNative(FString Code, FString Subject, FString Reason,
			const FString& Expected, const FString& Name = FString(), int32 Index = INDEX_NONE)
		{
			auto Diagnostic = CandidateDiagnostic(MoveTemp(Code), MoveTemp(Subject), MoveTemp(Reason));
			Diagnostic.ActualValuePresent = false;
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			Diagnostic.CandidateName = Name;
			if (Index != INDEX_NONE) Diagnostic.OperationIndex = Index;
			return Diagnostic;
		}

		Contract::FDiagnostic ContextFailure(const FString& Code, const UObject& Target,
			const FAiBehaviorCandidateContext& Context, const FString& Reason, const FString& Expected)
		{
			auto Diagnostic = CandidateDiagnostic(Code, Target.GetPathName(), Reason);
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetBoolField(TEXT("parent_present"), Context.Parent != nullptr);
			Actual->SetBoolField(TEXT("graph_present"), Context.Graph != nullptr);
			Actual->SetStringField(TEXT("attachment_kind"), Context.AttachmentKind);
			if (Context.Parent) Actual->SetStringField(TEXT("parent_type"), Context.Parent->GetClass()->GetName());
			if (Context.Graph) Actual->SetStringField(TEXT("graph_type"), Context.Graph->GetClass()->GetName());
			Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			auto Evidence = MakeShared<FJsonObject>();
			if (Context.Parent) Evidence->SetStringField(TEXT("parent"), Context.Parent->GetPathName());
			if (Context.Graph) Evidence->SetStringField(TEXT("graph"), Context.Graph->GetPathName());
			Diagnostic.ActualEvidence = MakeShared<FJsonValueObject>(Evidence);
			return Diagnostic;
		}

		Contract::FDiagnostic GameThreadFailure(const UObject& Target)
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("ai_behavior_candidate_game_thread_required"), Target.GetPathName(),
				TEXT("AI candidates must be accessed on the Editor game thread."));
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(TEXT("other thread"));
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("game thread"));
			return Diagnostic;
		}

		Contract::FDiagnostic TargetTypeFailure(const UObject& Target, const TArray<FString>& Allowed)
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("ai_behavior_candidate_target_type_unsupported"), Target.GetPathName(),
				TEXT("The target asset type is not supported by this AI candidate catalog."));
			Diagnostic.PropertyPath = TEXT("target");
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(Target.GetClass()->GetName());
			Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Target.GetClass()->GetPathName());
			TArray<TSharedPtr<FJsonValue>> Expected;
			for (const auto& Type : Allowed) Expected.Add(MakeShared<FJsonValueString>(Type));
			Diagnostic.ExpectedValue = MakeShared<FJsonValueArray>(MoveTemp(Expected));
			return Diagnostic;
		}

		Contract::FDiagnostic MachineSelectorFailure(const FString& Id)
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("ai_behavior_candidate_machine_selector_not_supported"), Id,
				TEXT("AI candidates require a readable name and optional select fields; machine candidate identifiers are not accepted."));
			Diagnostic.SourceKind.Reset();
			Diagnostic.PropertyPath = TEXT("id");
			Diagnostic.Source.JsonPath = TEXT("/id");
			Diagnostic.ActualValuePresent = true;
			Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Id);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("readable name with optional select fields"));
			return Diagnostic;
		}

		Contract::FDiagnostic MissingNameFailure(const FString& Name)
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("ai_behavior_candidate_selector_required"), Name,
				TEXT("An AI candidate requires a non-empty readable name."));
			Diagnostic.SourceKind.Reset();
			Diagnostic.PropertyPath = TEXT("name");
			Diagnostic.Source.JsonPath = TEXT("/name");
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(Name);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty readable candidate name"));
			return Diagnostic;
		}

		Contract::FDiagnostic IndexFailure(int32 Index, int32 Count, const FString& Name)
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("ai_behavior_candidate_resolved_index_invalid"), Name,
				TEXT("The resolved AI candidate index is outside the current catalog."));
			Diagnostic.CandidateName = Name;
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetNumberField(TEXT("resolved_index"), Index);
			Actual->SetNumberField(TEXT("candidate_count"), Count);
			Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("index within the current candidate catalog"));
			return Diagnostic;
		}

		FString Sha256(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}

		bool MatchesKeywordSearch(
			const FString& Query,
			const FString& Identity,
			const FString& DisplayName,
			const TArray<FString>& ExtraTerms)
		{
			ContentDiscovery::FContentDiscoveryRequest Request;
			Request.Query = Query;
			Request.MatchMode =
				ContentDiscovery::EContentDiscoveryMatchMode::Text;
			int32 Score = 0;
			FString MatchReason;
			return ContentDiscovery::TryScoreContentDiscoveryMatch(
				Request,
				Identity,
				DisplayName,
				ExtraTerms,
				Score,
				MatchReason);
		}

		const FString BehaviorTreeCoverageScope =
			TEXT("behavior_tree_blank_background");

		struct FNativeAiActionEntry final
		{
			TSharedPtr<FEdGraphSchemaAction> Action;
			UEdGraphNode* BackgroundTemplate = nullptr;
			NativeNodeAction::EPinInitialization PinOrder = NativeNodeAction::EPinInitialization::AfterPlacement;
			FAiBehaviorCandidate Candidate;
			UClass* PayloadClass = nullptr;
		};

		struct FNativeAiActionCatalog final
		{
			UEdGraph* EditorGraph = nullptr;
			TStrongObjectPtr<UEdGraph> OwnedGraph;
			TStrongObjectPtr<UUeAgentBehaviorTreeHeadlessSchema> Schema;
			TStrongObjectPtr<UUeAgentEqsHeadlessSchema> EqsSchema;
			TStrongObjectPtr<UUeAgentBehaviorTreeDecoratorHeadlessSchema> DecoratorSchema;
			TStrongObjectPtr<UEdGraph> ActionOwner;
			TArray<FNativeAiActionEntry> Entries;
			int32 CompositeClassCount = 0;
			int32 TaskClassCount = 0;
		};

		bool SetCandidateIdentity(
			const FString& IdentityKind,
			const FString& Signature,
			FAiBehaviorCandidate& Candidate,
			FAiBehaviorCandidateQueryError& OutError)
		{
			UeAgentPaletteSearch::FCandidate Identity;
			Identity.Kind = IdentityKind;
			Identity.Name = Candidate.Name;
			Identity.DisplayName = Candidate.Label;
			Identity.CategoryPath = Candidate.Category;
			Identity.Tooltip = Candidate.Summary;
			Identity.Type = Candidate.ActionKind;
			Identity.FullName = Signature;
			UE::UAI::Contract::FDiagnostic IdentityError;
			if (Identity.Name.IsEmpty())
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("ai_behavior_candidate_identity_failed"), Signature,
					TEXT("The native AI candidate has no readable name for its identity."));
				Diagnostic.PropertyPath = TEXT("name");
				Diagnostic.ActualValue = MakeShared<FJsonValueString>(Candidate.Name);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty readable candidate name"));
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}
			if (!UeAgentPaletteSearch::SetCandidateIdentity(
					IdentityKind,
					Signature,
					Identity.Name,
					Identity,
					IdentityError))
			{
				return ForwardDiagnostic(OutError, CandidateCause(TEXT("ai_behavior_candidate_identity_failed"),
					Candidate.Name, IdentityError, TEXT("The native AI candidate could not be assigned an identity.")), Candidate.Name);
			}
			Candidate.Identity = Identity.Identity;
			Candidate.Id = Identity.GetPublicId();
			return true;
		}

		bool SetNativeCandidateIdentity(
			const FString& Signature,
			FAiBehaviorCandidate& Candidate,
			FAiBehaviorCandidateQueryError& OutError)
		{
			return SetCandidateIdentity(
				TEXT("behavior_tree_action"),
				Signature,
				Candidate,
				OutError);
		}

		bool BuildNativeNodeCandidate(
			const TSharedPtr<FEdGraphSchemaAction>& Action,
			FGraphNodeClassData& ClassData,
			const FString& CandidateKind,
			FAiBehaviorCandidate& OutCandidate,
			UClass*& OutPayloadClass,
			FAiBehaviorCandidateQueryError& OutError)
		{
			OutCandidate = {};
			OutPayloadClass = ClassData.GetClass(true);
			if (!Action.IsValid() || !OutPayloadClass)
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("ai_behavior_candidate_native_class_unresolved"), ClassData.GetClassName(),
					!Action.IsValid() ? TEXT("The AI class-cache entry has no native menu action.")
						: TEXT("The native AI menu action's payload class could not be loaded."), ClassData.GetPackageName());
				Diagnostic.CandidateName = Action.IsValid() ? Action->GetMenuDescription().ToString() : FString();
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetBoolField(TEXT("action_present"), Action.IsValid());
				Actual->SetBoolField(TEXT("payload_class_present"), OutPayloadClass != nullptr);
				Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("native menu action with a loadable payload class"));
				Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(ClassData.GetClassName() + TEXT("|") + ClassData.GetPackageName());
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}
			FString SemanticName = OutPayloadClass->GetName();
			for (const TCHAR* Prefix : {TEXT("BTTask_"), TEXT("BTService_"), TEXT("BTDecorator_"), TEXT("BTComposite_")})
			{
				SemanticName.RemoveFromStart(Prefix, ESearchCase::IgnoreCase);
			}
			const bool bEqs = CandidateKind.StartsWith(TEXT("eqs_"));
			OutCandidate.Name = bEqs ? Action->GetMenuDescription().ToString() : FName::NameToDisplayString(SemanticName, false);
			OutCandidate.Label = Action->GetMenuDescription().ToString();
			OutCandidate.Kind = CandidateKind;
			OutCandidate.ActionKind = bEqs ? TEXT("native_eqs_new_node") : TEXT("native_behavior_tree_new_node");
			OutCandidate.Category = Action->GetCategory().ToString();
			OutCandidate.Summary = Action->GetTooltipDescription().ToString();
			OutCandidate.PayloadType = OutPayloadClass->GetPathName();
			OutCandidate.SelectionFields.Add(TEXT("kind"), CandidateKind);
			OutCandidate.SelectionFields.Add(TEXT("class"), OutPayloadClass->GetName());
			OutCandidate.SelectionFields.Add(TEXT("class_source"), ContentDiscovery::MakeReadableDiscoveryPath(OutPayloadClass->GetPathName()));
			const FString PackageName = OutPayloadClass->GetOutermost()->GetName();
			TArray<FString> SourceSegments;
			PackageName.ParseIntoArray(SourceSegments, TEXT("/"), true);
			// The candidate already names the asset. Present its owning folder as readable
			// breadcrumbs; native classes use their module. Never return a class/package locator.
			if (!OutPayloadClass->HasAnyClassFlags(CLASS_Native) && SourceSegments.Num() > 1)
				SourceSegments.Pop();
			if (OutPayloadClass->HasAnyClassFlags(CLASS_Native) && SourceSegments.Num() > 1)
				SourceSegments.RemoveAt(0);
			OutCandidate.SelectionFields.Add(TEXT("source"), FString::Join(SourceSegments, TEXT(" > ")));
			OutCandidate.bSupported = true;
			OutCandidate.WriteSupport = TEXT("supported");
			OutCandidate.SupportReason = bEqs ? TEXT("native_eqs_schema_action") : TEXT("native_behavior_tree_schema_action");
			OutCandidate.Scopes = {OutCandidate.Category, CandidateKind};
			OutCandidate.SupportedOperations = {TEXT("create")};
			return SetCandidateIdentity(bEqs ? TEXT("eqs_action") : TEXT("behavior_tree_action"),
				(bEqs ? FString(TEXT("eqs_graph")) : BehaviorTreeCoverageScope) + TEXT("|") + CandidateKind
					+ TEXT("|") + OutCandidate.PayloadType,
				OutCandidate,
				OutError);
		}

		bool BuildNativeMenuCandidate(
			const TSharedPtr<FEdGraphSchemaAction>& Action,
			const FString& Slot,
			FAiBehaviorCandidate& OutCandidate,
			FAiBehaviorCandidateQueryError& OutError)
		{
			OutCandidate = {};
			if (!Action.IsValid())
			{
				return ForwardDiagnostic(OutError, MissingNative(TEXT("ai_behavior_candidate_native_action_invalid"), Slot,
					TEXT("The AI menu returned an empty action for its expected slot."), TEXT("native menu action"),
					FName::NameToDisplayString(Slot, false)));
			}
			OutCandidate.Name = FName::NameToDisplayString(Slot, false);
			OutCandidate.Label = Action->GetMenuDescription().ToString();
			OutCandidate.Kind = TEXT("behavior_menu_action");
			OutCandidate.ActionKind = TEXT("native_behavior_tree_") + Slot;
			OutCandidate.Category = Action->GetCategory().ToString();
			OutCandidate.Summary = Action->GetTooltipDescription().ToString();
			OutCandidate.bSupported = false;
			OutCandidate.WriteSupport = TEXT("search_only");
			OutCandidate.SupportReason =
				TEXT("native_menu_action_has_no_hjson_create_contract");
			OutCandidate.SupportDiagnostic = CandidateDiagnostic(TEXT("ai_behavior_candidate_action_kind_unsupported"), Slot,
				TEXT("This native AI menu action has no HJSON node-creation contract."));
			OutCandidate.SupportDiagnostic.CandidateName = OutCandidate.Name;
			OutCandidate.SupportDiagnostic.ActualValue = MakeShared<FJsonValueString>(OutCandidate.ActionKind);
			OutCandidate.SupportDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("native action with an HJSON creation contract"));
			return SetNativeCandidateIdentity(
				BehaviorTreeCoverageScope + TEXT("|menu_slot=") + Slot,
				OutCandidate,
				OutError);
		}

		bool TryBuildNativeBehaviorTreeCatalog(
			UBehaviorTree& Target,
			const bool bRequireTargetGraph,
			FNativeAiActionCatalog& OutCatalog,
			FAiBehaviorCandidateQueryError& OutError,
			const FAiBehaviorCandidateContext& Context)
		{
			OutCatalog.EditorGraph = nullptr;
			OutCatalog.OwnedGraph.Reset();
			OutCatalog.Schema.Reset();
			OutCatalog.DecoratorSchema.Reset();
			OutCatalog.ActionOwner.Reset();
			OutCatalog.Entries.Reset();
			OutCatalog.CompositeClassCount = 0;
			OutCatalog.TaskClassCount = 0;

#if WITH_EDITORONLY_DATA
			UBehaviorTreeGraph* EditorGraph =
				Cast<UBehaviorTreeGraph>(Target.BTGraph);
#else
			UBehaviorTreeGraph* EditorGraph = nullptr;
#endif
			if (!EditorGraph && !bRequireTargetGraph && Context.Parent)
				EditorGraph = Cast<UBehaviorTreeGraph>(Context.Parent->GetGraph());
			if (!EditorGraph && bRequireTargetGraph)
			{
				return ForwardDiagnostic(OutError, MissingNative(TEXT("ai_behavior_candidate_context_unavailable"), Target.GetPathName(),
					TEXT("The target BehaviorTree has no editor graph."), TEXT("BehaviorTree editor graph")));
			}
			if (!EditorGraph)
			{
				EditorGraph = NewObject<UBehaviorTreeGraph>(
					GetTransientPackage(),
					NAME_None,
					RF_Transient);
				if (!EditorGraph)
				{
					return ForwardDiagnostic(OutError, MissingNative(TEXT("ai_behavior_candidate_context_unavailable"), Target.GetPathName(),
						TEXT("A transient BehaviorTree graph could not be created."), TEXT("transient BehaviorTree graph")));
				}
				OutCatalog.OwnedGraph.Reset(EditorGraph);
			}

			UUeAgentBehaviorTreeHeadlessSchema* const HeadlessSchema =
				NewObject<UUeAgentBehaviorTreeHeadlessSchema>(
					GetTransientPackage());
			if (!HeadlessSchema)
			{
				return ForwardDiagnostic(OutError, MissingNative(TEXT("ai_behavior_candidate_context_unavailable"), Target.GetPathName(),
					TEXT("The headless BehaviorTree schema could not be created."), TEXT("headless BehaviorTree schema")));
			}
			HeadlessSchema->InitializeHeadlessClassCache();
			OutCatalog.Schema.Reset(HeadlessSchema);
			OutCatalog.EditorGraph = EditorGraph;
			if (Context.Graph && Context.Graph != EditorGraph)
			{
				if (!Context.Graph->bEditable)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("ai_behavior_candidate_graph_read_only"), Target.GetPathName(),
						TEXT("The selected BehaviorTree decorator graph is read-only."));
					Diagnostic.PropertyPath = TEXT("graph.editable");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Context.Graph->GetPathName());
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
				}
				const auto* Owner = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Context.Graph->GetOuter());
				bool bRegisteredOwner = false;
				int32 Registrations = 0;
				for (const UEdGraph* Graph : EditorGraph->SubGraphs) Registrations += Graph == Context.Graph;
				for (const UEdGraphNode* Node : EditorGraph->Nodes)
				{
					const auto* BehaviorNode = Cast<UBehaviorTreeGraphNode>(Node);
					if (BehaviorNode && BehaviorNode->SubNodes.Contains(Owner)) bRegisteredOwner = true;
				}
				if (Context.Parent || !Context.AttachmentKind.IsEmpty() || !Owner
					|| Owner->BoundGraph != Context.Graph || !bRegisteredOwner
					|| !Context.Graph->IsA<UBehaviorTreeDecoratorGraph>()
					|| Registrations != 1)
				{
					auto Diagnostic = ContextFailure(TEXT("ai_behavior_candidate_graph_context_invalid"), Target, Context,
						Context.Parent ? TEXT("A BehaviorTree decorator-graph context cannot also specify an attachment parent.")
							: !Context.AttachmentKind.IsEmpty() ? TEXT("A BehaviorTree decorator-graph context cannot also specify an attachment kind.")
							: !Owner ? TEXT("The selected decorator graph is not owned by a CompositeDecorator node.")
							: Owner->BoundGraph != Context.Graph ? TEXT("The CompositeDecorator does not own the selected graph as its bound graph.")
							: !bRegisteredOwner ? TEXT("The decorator graph's owner is not registered under the target BehaviorTree graph.")
							: !Context.Graph->IsA<UBehaviorTreeDecoratorGraph>() ? TEXT("The selected graph is not a BehaviorTree decorator graph.")
							: TEXT("The decorator graph must be registered exactly once under the target BehaviorTree graph."),
						TEXT("owned decorator graph registered once, without an attachment parent or kind"));
					Diagnostic.ActualValue->AsObject()->SetNumberField(TEXT("subgraph_registrations"), Registrations);
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
				}
				auto* Schema = NewObject<UUeAgentBehaviorTreeDecoratorHeadlessSchema>(GetTransientPackage());
				Schema->SetClassCacheOwner(*HeadlessSchema);
				OutCatalog.DecoratorSchema.Reset(Schema);
				OutCatalog.EditorGraph = Context.Graph;
				FGraphContextMenuBuilder Menu(Context.Graph);
				Schema->GetGraphContextActions(Menu);
				OutCatalog.ActionOwner.Reset(Menu.OwnerOfTemporaries);
				for (int32 Index = 0; Index < Menu.GetNumActions(); ++Index)
				{
					auto& Entry = OutCatalog.Entries.AddDefaulted_GetRef();
					Entry.Action = Menu.GetSchemaAction(Index);
					// This schema emits only FDecoratorSchemaAction_NewNode actions.
					const auto* Action = static_cast<const FDecoratorSchemaAction_NewNode*>(Entry.Action.Get());
					UEdGraphNode* Template = Action ? Action->NodeTemplate.Get() : nullptr;
					if (!Template) return ForwardDiagnostic(OutError, MissingNative(TEXT("ai_behavior_candidate_native_action_invalid"), Target.GetPathName(),
						TEXT("The decorator schema menu action has no node template."), TEXT("decorator node template"),
						Entry.Action.IsValid() ? Entry.Action->GetMenuDescription().ToString() : FString(), Index));
					Entry.BackgroundTemplate = Template;
					Entry.PinOrder = NativeNodeAction::EPinInitialization::BeforePlacement;
					const auto* Logic = FindFProperty<FByteProperty>(Template->GetClass(), TEXT("LogicMode"));
					if (Logic && Logic->Enum)
					{
						auto& Candidate = Entry.Candidate;
						Candidate.Name = Entry.Action->GetMenuDescription().ToString();
						Candidate.Label = Candidate.Name;
						Candidate.Kind = TEXT("behavior_decorator_logic");
						Candidate.ActionKind = TEXT("native_behavior_tree_new_node");
						Candidate.Category = Entry.Action->GetCategory().ToString();
						Candidate.Summary = Entry.Action->GetTooltipDescription().ToString();
						Candidate.LogicMode = Logic->Enum->GetNameStringByValue(Logic->GetPropertyValue_InContainer(Template));
						Candidate.SelectionFields.Add(TEXT("kind"), Candidate.Kind);
						Candidate.bSupported = true;
						Candidate.WriteSupport = TEXT("supported");
						Candidate.SupportedOperations = {TEXT("create")};
						if (!SetNativeCandidateIdentity(TEXT("behavior_tree_decorator_logic|") + Candidate.LogicMode, Candidate, OutError)) return false;
						TStrongObjectPtr<UEdGraphNode> Preview(DuplicateObject<UEdGraphNode>(Template, Menu.OwnerOfTemporaries));
						Preview->AllocateDefaultPins();
						for (const UEdGraphPin* Pin : Preview->Pins)
							if (Pin && Pin->Direction == EGPD_Input) ++Candidate.DefaultInputCount;
					}
					else
					{
						const auto* Property = FindFProperty<FStructProperty>(Template->GetClass(), TEXT("ClassData"));
						if (!Property || Property->Struct != FGraphNodeClassData::StaticStruct())
						{
							auto Diagnostic = CandidateDiagnostic(TEXT("ai_behavior_candidate_native_class_unresolved"), Target.GetPathName(),
								!Property ? TEXT("The decorator node template has no ClassData struct property.")
									: TEXT("The decorator node template's ClassData property has an unexpected struct type."));
							Diagnostic.CandidateName = Entry.Action->GetMenuDescription().ToString();
							Diagnostic.PropertyPath = TEXT("ClassData");
							Diagnostic.ActualValuePresent = Property != nullptr;
							if (Property && Property->Struct) Diagnostic.ActualValue = MakeShared<FJsonValueString>(Property->Struct->GetName());
							Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(FGraphNodeClassData::StaticStruct()->GetName());
							return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
						}
						auto* ClassData = Property->ContainerPtrToValuePtr<FGraphNodeClassData>(Template);
						if (!BuildNativeNodeCandidate(Entry.Action, *ClassData, TEXT("behavior_decorator_test"),
							Entry.Candidate, Entry.PayloadClass, OutError)) return false;
					}
					Entry.Candidate.GraphNodeType = Template->GetClass()->GetPathName();
				}
				return true;
			}
			const bool bAttachment = Context.Parent != nullptr || !Context.AttachmentKind.IsEmpty();
			if (bAttachment && (!Context.Parent
				|| Context.Parent->GetGraph() != EditorGraph
				|| (!Context.Parent->IsA<UBehaviorTreeGraphNode_Composite>()
					&& !Context.Parent->IsA<UBehaviorTreeGraphNode_Task>())
				|| (Context.AttachmentKind != TEXT("decorator") && Context.AttachmentKind != TEXT("service"))
				|| (Context.AttachmentKind == TEXT("service") && !EditorGraph->DoesSupportServices())
				|| (bRequireTargetGraph && !EditorGraph->Nodes.Contains(Context.Parent))))
			{
				return ForwardDiagnostic(OutError, ContextFailure(TEXT("ai_behavior_candidate_attachment_context_invalid"), Target, Context,
					!Context.Parent ? TEXT("A BehaviorTree attachment requires a parent node.")
						: Context.Parent->GetGraph() != EditorGraph ? TEXT("The BehaviorTree attachment parent belongs to a different graph.")
						: !Context.Parent->IsA<UBehaviorTreeGraphNode_Composite>() && !Context.Parent->IsA<UBehaviorTreeGraphNode_Task>() ? TEXT("A BehaviorTree attachment parent must be a Composite or Task node.")
						: Context.AttachmentKind != TEXT("decorator") && Context.AttachmentKind != TEXT("service") ? TEXT("A BehaviorTree attachment kind must be decorator or service.")
						: Context.AttachmentKind == TEXT("service") && !EditorGraph->DoesSupportServices() ? TEXT("The target BehaviorTree graph does not support service attachments.")
						: TEXT("The attachment parent is not registered in the target BehaviorTree graph."),
					TEXT("registered Composite/Task parent with a supported decorator/service attachment")));
			}

			TArray<FGraphNodeClassData> CompositeClasses;
			TArray<FGraphNodeClassData> TaskClasses;
			HeadlessSchema->GetHeadlessClassCache().GatherClasses(
				UBTCompositeNode::StaticClass(), CompositeClasses);
			HeadlessSchema->GetHeadlessClassCache().GatherClasses(
				UBTTaskNode::StaticClass(), TaskClasses);
			OutCatalog.CompositeClassCount = CompositeClasses.Num();
			OutCatalog.TaskClassCount = TaskClasses.Num();

			FGraphContextMenuBuilder MenuBuilder(EditorGraph);
			MenuBuilder.FromPin = nullptr;
			TArray<FGraphNodeClassData> AttachmentClasses;
			if (bAttachment)
			{
				const int32 Flags = Context.AttachmentKind == TEXT("decorator") ? ESubNode::Decorator : ESubNode::Service;
				UClass* GraphNodeClass = nullptr;
				HeadlessSchema->GetSubNodeClasses(Flags, AttachmentClasses, GraphNodeClass);
				MenuBuilder.SelectedObjects.Add(Context.Parent);
				HeadlessSchema->GetGraphNodeContextActions(MenuBuilder, Flags);
			}
			else HeadlessSchema->GetGraphContextActions(MenuBuilder);
			OutCatalog.ActionOwner.Reset(MenuBuilder.OwnerOfTemporaries);
			const int32 ExpectedActionCount = bAttachment
				? AttachmentClasses.Num() + (Context.AttachmentKind == TEXT("decorator") ? 1 : 0)
				: CompositeClasses.Num() + TaskClasses.Num() + 2;
			if (MenuBuilder.GetNumActions() != ExpectedActionCount)
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("ai_behavior_candidate_native_menu_shape_mismatch"), Target.GetPathName(),
					TEXT("The BehaviorTree schema menu action count differs from the native class-cache entries and fixed menu actions."));
				Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(MenuBuilder.GetNumActions());
				Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(ExpectedActionCount);
				auto Evidence = MakeShared<FJsonObject>();
				Evidence->SetNumberField(TEXT("composites"), CompositeClasses.Num());
				Evidence->SetNumberField(TEXT("tasks"), TaskClasses.Num());
				Evidence->SetNumberField(TEXT("attachment_classes"), AttachmentClasses.Num());
				Evidence->SetStringField(TEXT("attachment_kind"), Context.AttachmentKind);
				Diagnostic.ActualEvidence = MakeShared<FJsonValueObject>(Evidence);
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}

			auto AddNodeSegment = [&](TArray<FGraphNodeClassData>& Classes,
				const FString& Kind, const int32 Offset) -> bool
			{
				for (int32 Index = 0; Index < Classes.Num(); ++Index)
				{
					const TSharedPtr<FEdGraphSchemaAction>& Action =
						MenuBuilder.GetSchemaAction(Offset + Index);
					const FString ExpectedName = FName::NameToDisplayString(
						Classes[Index].ToString(), false);
					if (!Action.IsValid()
						|| Action->GetMenuDescription().ToString() != ExpectedName)
					{
						auto Diagnostic = CandidateDiagnostic(TEXT("ai_behavior_candidate_native_menu_shape_mismatch"), FString::FromInt(Offset + Index),
							!Action.IsValid() ? TEXT("The BehaviorTree class-cache menu segment contains an empty action.")
								: TEXT("The BehaviorTree menu action label differs from the expected class-cache entry."));
						Diagnostic.OperationIndex = Offset + Index;
						Diagnostic.PropertyPath = TEXT("menu_label");
						Diagnostic.ActualValuePresent = Action.IsValid();
						if (Action.IsValid()) Diagnostic.ActualValue = MakeShared<FJsonValueString>(Action->GetMenuDescription().ToString());
						Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(ExpectedName);
						return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
					}
					FNativeAiActionEntry& Entry =
						OutCatalog.Entries.AddDefaulted_GetRef();
					Entry.Action = Action;
					UAIGraphNode* NativeTemplate = bAttachment
						? static_cast<const FAISchemaAction_NewSubNode*>(Action.Get())->NodeTemplate.Get()
						: static_cast<const FAISchemaAction_NewNode*>(Action.Get())->NodeTemplate.Get();
					if (!NativeTemplate)
						return ForwardDiagnostic(OutError, MissingNative(TEXT("ai_behavior_candidate_native_template_missing"), ExpectedName,
							TEXT("The BehaviorTree native menu action has no node template."), TEXT("AI graph node template"), ExpectedName, Offset + Index));
					if (!bAttachment) Entry.BackgroundTemplate = NativeTemplate;
					if (!BuildNativeNodeCandidate(
							Action,
							Classes[Index],
							Kind,
							Entry.Candidate,
							Entry.PayloadClass,
							OutError))
					{
						return false;
					}
					Entry.Candidate.GraphNodeType = NativeTemplate->GetClass()->GetPathName();
				}
				return true;
			};

			if (bAttachment)
			{
				if (!AddNodeSegment(AttachmentClasses, TEXT("behavior_") + Context.AttachmentKind, 0)) return false;
				if (Context.AttachmentKind == TEXT("decorator"))
				{
					auto& Entry = OutCatalog.Entries.AddDefaulted_GetRef();
					Entry.Action = MenuBuilder.GetSchemaAction(AttachmentClasses.Num());
					if (!BuildNativeMenuCandidate(Entry.Action, TEXT("composite_decorator"), Entry.Candidate, OutError)) return false;
					Entry.Candidate.Kind = TEXT("behavior_composite_decorator");
					Entry.Candidate.GraphNodeType = UBehaviorTreeGraphNode_CompositeDecorator::StaticClass()->GetPathName();
					Entry.Candidate.bSupported = true;
					Entry.Candidate.WriteSupport = TEXT("supported");
					Entry.Candidate.SupportedOperations = {TEXT("create")};
					Entry.Candidate.SupportReason = TEXT("native_composite_decorator_with_owned_graph");
					Entry.Candidate.SupportDiagnostic.Reset();
				}
				return true;
			}

			if (!AddNodeSegment(
					CompositeClasses, TEXT("behavior_composite"), 0)
				|| !AddNodeSegment(
					TaskClasses,
					TEXT("behavior_task"),
					CompositeClasses.Num()))
			{
				return false;
			}

			const int32 MenuOffset =
				CompositeClasses.Num() + TaskClasses.Num();
			const TCHAR* const Slots[] = {TEXT("auto_arrange"), TEXT("add_comment")};
			for (int32 Index = 0; Index < 2; ++Index)
			{
				FNativeAiActionEntry& Entry =
					OutCatalog.Entries.AddDefaulted_GetRef();
				Entry.Action = MenuBuilder.GetSchemaAction(MenuOffset + Index);
				if (!BuildNativeMenuCandidate(
						Entry.Action, Slots[Index], Entry.Candidate, OutError))
				{
					return false;
				}
			}
			return true;
		}

		bool TryBuildNativeEqsCatalog(UEnvQuery& Target, bool bRequireTargetGraph,
			FNativeAiActionCatalog& Catalog, FAiBehaviorCandidateQueryError& Error,
			const FAiBehaviorCandidateContext& Context)
		{
			auto* Graph = Cast<UEnvironmentQueryGraph>(Target.EdGraph);
			if (!Graph && !bRequireTargetGraph && Context.Parent)
				Graph = Cast<UEnvironmentQueryGraph>(Context.Parent->GetGraph());
			if (!Graph && bRequireTargetGraph) return ForwardDiagnostic(Error, MissingNative(TEXT("ai_behavior_candidate_context_unavailable"), Target.GetPathName(),
				TEXT("The target EnvironmentQuery has no editor graph for native action execution."), TEXT("EnvironmentQuery editor graph")));
			if (!Graph)
			{
				Graph = NewObject<UEnvironmentQueryGraph>(GetTransientPackage(), NAME_None, RF_Transient);
				Catalog.OwnedGraph.Reset(Graph);
			}
			if ((Context.Graph && Context.Graph != Graph) || !Graph->bEditable)
			{
				auto Diagnostic = ContextFailure(TEXT("ai_behavior_candidate_graph_context_invalid"), Target, Context,
					Context.Graph && Context.Graph != Graph ? TEXT("The provided EQS graph context differs from the target query's editor graph.")
						: TEXT("The target EnvironmentQuery graph is read-only."), TEXT("editable target EnvironmentQuery graph"));
				Diagnostic.ActualValue->AsObject()->SetBoolField(TEXT("editable"), Graph->bEditable);
				Diagnostic.ActualValue->AsObject()->SetBoolField(TEXT("matches_target_graph"), !Context.Graph || Context.Graph == Graph);
				Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(Graph->GetPathName());
				return ForwardDiagnostic(Error, MoveTemp(Diagnostic));
			}
			const bool bAttachment = Context.Parent || !Context.AttachmentKind.IsEmpty();
			if (bAttachment && (!Context.Parent || !Context.Parent->IsA<UEnvironmentQueryGraphNode_Option>()
				|| Context.Parent->GetGraph() != Graph || Context.AttachmentKind != TEXT("test")
				|| (bRequireTargetGraph && !Graph->Nodes.Contains(Context.Parent))))
				return ForwardDiagnostic(Error, ContextFailure(TEXT("ai_behavior_candidate_attachment_context_invalid"), Target, Context,
					!Context.Parent ? TEXT("An EQS test attachment requires a parent Option node.")
						: !Context.Parent->IsA<UEnvironmentQueryGraphNode_Option>() ? TEXT("An EQS test attachment parent must be an Option node.")
						: Context.Parent->GetGraph() != Graph ? TEXT("The EQS attachment parent belongs to a different graph.")
						: Context.AttachmentKind != TEXT("test") ? TEXT("The EQS attachment kind must be test.")
						: TEXT("The EQS attachment parent is not registered in the target graph."),
					TEXT("registered Option parent in the target graph with attachment kind test")));
			Catalog.EditorGraph = Graph;
			Catalog.EqsSchema.Reset(NewObject<UUeAgentEqsHeadlessSchema>(GetTransientPackage()));
			Catalog.EqsSchema->InitializeHeadlessClassCache();
			FGraphContextMenuBuilder Menu(Graph);
			if (bAttachment)
			{
				Menu.SelectedObjects.Add(Context.Parent);
				Catalog.EqsSchema->GetGraphNodeContextActions(Menu, 0);
			}
			else Catalog.EqsSchema->GetGraphContextActions(Menu);
			Catalog.ActionOwner.Reset(Menu.OwnerOfTemporaries);
			for (int32 Index = 0; Index < Menu.GetNumActions(); ++Index)
			{
				const auto Action = Menu.GetSchemaAction(Index);
				if (!Action)
					return ForwardDiagnostic(Error, MissingNative(TEXT("ai_behavior_candidate_native_action_unsupported"), Target.GetPathName(),
						TEXT("The EnvironmentQuery menu returned an empty native action."), TEXT("native EQS menu action"), FString(), Index));
				UAIGraphNode* Template = bAttachment ? static_cast<FAISchemaAction_NewSubNode*>(Action.Get())->NodeTemplate.Get()
					: static_cast<FAISchemaAction_NewNode*>(Action.Get())->NodeTemplate.Get();
				if (!Template) return ForwardDiagnostic(Error, MissingNative(TEXT("ai_behavior_candidate_native_template_missing"), Target.GetPathName(),
					TEXT("The native EQS menu action has no node template."), bAttachment ? TEXT("EQS test node template") : TEXT("EQS generator node template"),
					Action->GetMenuDescription().ToString(), Index));
				auto& Entry = Catalog.Entries.AddDefaulted_GetRef();
				Entry.Action = Action;
				if (!bAttachment) Entry.BackgroundTemplate = Template;
				if (!BuildNativeNodeCandidate(Action, Template->ClassData, bAttachment ? TEXT("eqs_test") : TEXT("eqs_generator"),
					Entry.Candidate, Entry.PayloadClass, Error)) return false;
				Entry.Candidate.GraphNodeType = Template->GetClass()->GetPathName();
			}
			return true;
		}

		bool TryBuildStateTreeCatalog(UStateTree& Target, FNativeAiActionCatalog& Catalog,
			FAiBehaviorCandidateQueryError& Error, const FAiBehaviorCandidateContext& Context)
		{
			if (Context.Graph || Context.Parent || !Context.AttachmentKind.IsEmpty())
				return ForwardDiagnostic(Error, ContextFailure(TEXT("ai_behavior_candidate_attachment_context_invalid"), Target, Context,
					TEXT("StateTree candidate discovery does not accept graph or attachment context."), TEXT("empty graph, parent and attachment context")));
			const auto* Data = Cast<UStateTreeEditorData>(Target.EditorData);
			if (!Data || !Data->Schema) return ForwardDiagnostic(Error, MissingNative(TEXT("state_tree_candidate_schema_missing"), Target.GetPathName(),
				!Data ? TEXT("The target StateTree has no StateTree editor data.") : TEXT("The target StateTree editor data has no schema."),
				TEXT("StateTree editor data with a schema")));
			const auto Cache = FStateTreeEditorModule::GetModule().GetNodeClassCache();
			auto Add = [&](const FString& Name, const FString& Kind, const UStruct* Type) {
				auto& Candidate = Catalog.Entries.AddDefaulted_GetRef().Candidate;
				Candidate.Name = Name;
				Candidate.Label = Type ? Type->GetDisplayNameText().ToString() : Name;
				Candidate.Kind = Kind;
				Candidate.ActionKind = TEXT("state_tree_native_builder");
				Candidate.PayloadType = Type ? Type->GetPathName() : FString();
				Candidate.Category = Type ? Type->GetMetaData(TEXT("Category")) : TEXT("Structure");
				Candidate.SelectionFields.Add(TEXT("kind"), Kind);
				if (Type)
				{
					Candidate.SelectionFields.Add(TEXT("type"), Type->GetName());
					Candidate.SelectionFields.Add(TEXT("type_source"), MakeReadableDiscoveryPath(Type->GetPathName()));
				}
				Candidate.bSupported = true;
				Candidate.WriteSupport = TEXT("supported");
				Candidate.SupportReason = TEXT("native_class_cache_and_schema");
				if (Candidate.bSupported) Candidate.SupportedOperations = {TEXT("create")};
				return SetCandidateIdentity(TEXT("state_tree_native"), Kind + TEXT("|") + Candidate.PayloadType, Candidate, Error);
			};
			if (!Add(TEXT("State"), TEXT("state_tree_state"), nullptr)
				|| !Add(TEXT("Transition"), TEXT("state_tree_transition"), nullptr)
				|| !Add(TEXT("Binding"), TEXT("state_tree_binding"), nullptr)
				|| !Add(TEXT("Color"), TEXT("state_tree_color"), nullptr)) return false;
			struct FNodeFamily { UScriptStruct* Struct; UClass* Class; const TCHAR* Kind; bool Allowed; };
			const FNodeFamily Families[] = {
				{FStateTreeTaskBase::StaticStruct(), UStateTreeTaskBlueprintBase::StaticClass(), TEXT("state_tree_task"), true},
				{FStateTreeConditionBase::StaticStruct(), UStateTreeConditionBlueprintBase::StaticClass(), TEXT("state_tree_condition"), true},
				{FStateTreeEvaluatorBase::StaticStruct(), UStateTreeEvaluatorBlueprintBase::StaticClass(), TEXT("state_tree_evaluator"), Data->Schema->AllowEvaluators()},
				{FStateTreeConsiderationBase::StaticStruct(), UStateTreeConsiderationBlueprintBase::StaticClass(), TEXT("state_tree_consideration"), Data->Schema->AllowUtilityConsiderations()},
				{FStateTreePropertyFunctionBase::StaticStruct(), nullptr, TEXT("state_tree_property_function"), true}
			};
			for (const auto& Family : Families)
			{
				if (!Family.Allowed) continue;
				TArray<TSharedPtr<FStateTreeNodeClassData>> Types;
				Cache->GetStructs(Family.Struct, Types);
				if (Family.Class)
				{
					TArray<TSharedPtr<FStateTreeNodeClassData>> Classes;
					Cache->GetClasses(Family.Class, Classes);
					Types.Append(Classes);
				}
				for (const auto& Entry : Types)
				{
					UStruct* Type = Entry->GetStruct(true);
					if (!Type || Type == Family.Struct || Type == Family.Class || Type->HasMetaData(TEXT("Hidden"))
						|| Type->HasMetaData(TEXT("Deprecated"))) continue;
					if (const auto* Class = Cast<UClass>(Type))
					{
						if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
							|| !Data->Schema->IsClassAllowed(Class)) continue;
					}
					else if (const auto* Struct = Cast<UScriptStruct>(Type))
					{
						if (Struct == FStateTreeBlueprintTaskWrapper::StaticStruct()
							|| Struct == FStateTreeBlueprintConditionWrapper::StaticStruct()
							|| Struct == FStateTreeBlueprintEvaluatorWrapper::StaticStruct()
							|| Struct == FStateTreeBlueprintConsiderationWrapper::StaticStruct()
							|| !Data->Schema->IsStructAllowed(Struct)) continue;
					}
					if (!Add(FName::NameToDisplayString(Type->GetName(), false), Family.Kind, Type)) return false;
				}
			}
			Catalog.Entries.Sort([](const auto& A, const auto& B) { return A.Candidate.Id < B.Candidate.Id; });
			return true;
		}

		bool TryBuildSmartObjectCatalog(USmartObjectDefinition& Target, FNativeAiActionCatalog& Catalog,
			FAiBehaviorCandidateQueryError& Error, const FAiBehaviorCandidateContext& Context)
		{
			if (Context.Graph || Context.Parent || !Context.AttachmentKind.IsEmpty())
				return ForwardDiagnostic(Error, ContextFailure(TEXT("ai_behavior_candidate_attachment_context_invalid"), Target, Context,
					TEXT("SmartObject candidate discovery does not accept graph or attachment context."), TEXT("empty graph, parent and attachment context")));
			auto Add = [&](const FString& Name, const TCHAR* Kind, const UStruct* Type) {
				auto& Candidate = Catalog.Entries.AddDefaulted_GetRef().Candidate;
				Candidate.Name = Name; Candidate.Label = Name; Candidate.Kind = Kind;
				Candidate.ActionKind = TEXT("smart_object_native_structure");
				if (Type && Type->IsChildOf(FSmartObjectSlotAnnotation::StaticStruct())) Candidate.RequiredContext = TEXT("parent:slot");
				Candidate.PayloadType = Type ? Type->GetPathName() : FString();
				Candidate.SelectionFields.Add(TEXT("kind"), Kind);
				if (Type)
				{
					Candidate.SelectionFields.Add(TEXT("type"), Type->GetName());
					Candidate.SelectionFields.Add(TEXT("type_source"), MakeReadableDiscoveryPath(Type->GetPathName()));
				}
				Candidate.bSupported = true; Candidate.WriteSupport = TEXT("supported");
				Candidate.SupportReason = TEXT("smart_object_loaded_native_type_constraints");
				Candidate.SupportedOperations = {TEXT("create")};
				return SetCandidateIdentity(TEXT("smart_object_native"), Candidate.Kind + TEXT("|") + Candidate.PayloadType, Candidate, Error);
			};
			if (!Add(TEXT("Slot"), TEXT("smart_object_slot"), FSmartObjectSlotDefinition::StaticStruct())) return false;
			if (!Add(TEXT("Binding"), TEXT("smart_object_binding"), FSmartObjectDefinitionPropertyBinding::StaticStruct())) return false;
			for (TObjectIterator<UClass> It; It; ++It)
				if (It->IsChildOf(USmartObjectBehaviorDefinition::StaticClass())
					&& !It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
					&& It->GetPathName().StartsWith(TEXT("/Script/")))
					if (!Add(It->GetDisplayNameText().ToString(), TEXT("smart_object_behavior"), *It)) return false;
			for (TObjectIterator<UScriptStruct> It; It; ++It)
				if (*It != FSmartObjectDefinitionData::StaticStruct() && It->IsChildOf(FSmartObjectDefinitionData::StaticStruct())
					&& !It->HasMetaData(TEXT("Hidden")) && !It->HasMetaData(TEXT("Deprecated")) && !It->IsA<UUserDefinedStruct>())
					if (!Add(It->GetDisplayNameText().ToString(), TEXT("smart_object_definition_data"), *It)) return false;
			Catalog.Entries.Sort([](const auto& A, const auto& B) { return A.Candidate.Id < B.Candidate.Id; });
			return true;
		}

		bool TryBuildNativeAiCatalog(UObject& Target, bool bRequireTargetGraph,
			FNativeAiActionCatalog& Catalog, FAiBehaviorCandidateQueryError& Error,
			const FAiBehaviorCandidateContext& Context)
		{
			if (auto* Tree = Cast<UBehaviorTree>(&Target)) return TryBuildNativeBehaviorTreeCatalog(*Tree, bRequireTargetGraph, Catalog, Error, Context);
			if (auto* Query = Cast<UEnvQuery>(&Target)) return TryBuildNativeEqsCatalog(*Query, bRequireTargetGraph, Catalog, Error, Context);
			if (auto* Definition = Cast<USmartObjectDefinition>(&Target)) return TryBuildSmartObjectCatalog(*Definition, Catalog, Error, Context);
			return ForwardDiagnostic(Error, TargetTypeFailure(Target, {TEXT("BehaviorTree"), TEXT("EnvQuery"), TEXT("SmartObjectDefinition")}));
		}

		bool MatchesNativeSearch(
			const FAiBehaviorCandidate& Candidate,
			const FString& Query)
		{
			return MatchesKeywordSearch(
				Query,
				Candidate.Id,
				Candidate.Label.IsEmpty() ? Candidate.Name : Candidate.Label,
				{
					Candidate.Name,
					Candidate.Category,
					Candidate.Summary,
					Candidate.Kind,
					Candidate.ActionKind,
					Candidate.PayloadType,
					Candidate.WriteSupport,
					Candidate.InputContract,
					Candidate.PlacementContract,
					Candidate.RequiredContext,
				});
		}

		bool TrySelectNativeAiAction(
			FNativeAiActionCatalog& Catalog,
			const FString& Name,
			const FString& Id,
			FNativeAiActionEntry*& OutEntry,
			FAiBehaviorCandidateQueryError& OutError,
			const TMap<FString, FString>& Select)
		{
			OutEntry = nullptr;
			const FString CleanName = Name.TrimStartAndEnd();
			const FString CleanId = Id.TrimStartAndEnd();
			if (!CleanId.IsEmpty())
			{
				return ForwardDiagnostic(OutError, MachineSelectorFailure(Id), CleanId);
			}
			if (CleanName.IsEmpty())
			{
				return ForwardDiagnostic(OutError, MissingNameFailure(Name));
			}
			TArray<FNameSelectionEntry> Entries;
			for (const FNativeAiActionEntry& Entry : Catalog.Entries)
			{
				Entries.Add({Entry.Candidate.Name, Entry.Candidate.Id, Entry.Candidate.SelectionFields});
			}
			int32 ResolvedIndex = INDEX_NONE;
			UE::UAI::Contract::FDiagnostic ResolveError;
			if (!TryResolveNameSelection(Entries, CleanName, Select, ResolvedIndex, ResolveError))
			{
				return ForwardDiagnostic(OutError, ResolveError, CleanName);
			}
			if (!Catalog.Entries.IsValidIndex(ResolvedIndex))
			{
				return ForwardDiagnostic(OutError, IndexFailure(ResolvedIndex, Catalog.Entries.Num(), CleanName));
			}
			OutEntry = &Catalog.Entries[ResolvedIndex];
			return true;
		}

		bool IsSupportedTarget(const UObject& Target)
		{
			return Target.GetClass() == UBehaviorTree::StaticClass()
				|| Target.GetClass() == UBlackboardData::StaticClass()
				|| Target.GetClass() == UStateTree::StaticClass()
				|| Target.GetClass() == UEnvQuery::StaticClass()
				|| Target.GetClass() == USmartObjectDefinition::StaticClass();
		}

		bool BuildBlackboardCandidate(
			UObject& Target,
			const UClass& Class,
			FAiBehaviorCandidate& OutCandidate,
			FAiBehaviorCandidateQueryError& OutError)
		{
			OutCandidate = {};
			// Preserve existing native selectors; Blueprint names use the class picker's display name.
			OutCandidate.Name = Class.ClassGeneratedBy
				? Class.GetDisplayNameText().ToString() : FName::NameToDisplayString(Class.GetName(), false);
			OutCandidate.Label = Class.GetDisplayNameText().ToString();
			OutCandidate.Kind = TEXT("blackboard_key");
			OutCandidate.PayloadType = Class.GetPathName();
			OutCandidate.bSupported = true;
			OutCandidate.WriteSupport = TEXT("supported");
			OutCandidate.SupportReason = TEXT("native_blackboard_key_type");
			OutCandidate.SupportedOperations = {TEXT("create")};
			OutCandidate.Scopes = {OutCandidate.Kind, Class.GetOutermost()->GetName()};
			OutCandidate.SelectionFields.Add(TEXT("class"), Class.GetName());
			OutCandidate.SelectionFields.Add(TEXT("source"), Class.ClassGeneratedBy
				? Class.ClassGeneratedBy->GetName() : FPackageName::GetShortName(Class.GetOutermost()->GetName()));
			OutCandidate.SelectionFields.Add(TEXT("class_source"), MakeReadableDiscoveryPath(Class.GetPathName()));
			OutCandidate.InputContract = TEXT("uai.ai.blackboard_entry.create/1");
			OutCandidate.PlacementContract = TEXT("uai.ai.blackboard-entry-placement/1");
			OutCandidate.RequiredContext = TEXT("target_ref:entry_name:key_type_ref");
			OutCandidate.BindingRevision = Sha256(OutCandidate.Name + TEXT("|") + OutCandidate.Kind + TEXT("|")
				+ OutCandidate.InputContract + TEXT("|") + OutCandidate.PlacementContract + TEXT("|")
				+ OutCandidate.RequiredContext + TEXT("|") + Target.GetClass()->GetPathName());
			return SetCandidateIdentity(
				TEXT("ai_behavior_candidate"),
				Target.GetClass()->GetPathName() + TEXT("|")
					+ OutCandidate.Kind + TEXT("|") + Class.GetPathName(),
				OutCandidate,
				OutError);
		}

		bool TryBuildBlackboardCandidateCatalog(
			UObject& Target,
			TArray<FAiBehaviorCandidate>& OutCandidates,
			FAiBehaviorCandidateQueryError& OutError)
		{
			TArray<TStrongObjectPtr<UClass>> LoadedClasses;
			if (!TryLoadDerivedBlueprintClasses(*UBlackboardKeyType::StaticClass(), LoadedClasses, OutError)) return false;
			TArray<const UClass*> Matches;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* const Class = *It;
				// Use Blackboard's native KeyType picker policy after shared discovery loads derived assets.
				if (!Class->IsChildOf(UBlackboardKeyType::StaticClass())
					|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_HideDropDown | CLASS_Hidden
						| CLASS_Deprecated | CLASS_NewerVersionExists)
					|| !Class->HasAnyClassFlags(CLASS_EditInlineNew)
					|| FBlueprintSupport::IsClassPlaceholder(Class)
					|| FKismetEditorUtilities::IsClassABlueprintSkeleton(Class))
				{
					continue;
				}
				Matches.Add(Class);
			}
			Algo::SortBy(Matches, [](const UObject* Object)
			{
				return Object->GetPathName();
			});
			OutCandidates.Reset(Matches.Num());
			for (const UClass* Class : Matches)
			{
				FAiBehaviorCandidate& Candidate = OutCandidates.AddDefaulted_GetRef();
				if (!BuildBlackboardCandidate(
						Target, *Class, Candidate, OutError))
				{
					return false;
				}
			}
			return true;
		}

		bool TrySelectBlackboardCandidate(
			TArray<FAiBehaviorCandidate>& Catalog,
			const FString& Name,
			const FString& Id,
			FAiBehaviorCandidate*& OutCandidate,
			FAiBehaviorCandidateQueryError& OutError, const TMap<FString, FString>& Select)
		{
			OutCandidate = nullptr;
			const FString CleanName = Name.TrimStartAndEnd();
			const FString CleanId = Id.TrimStartAndEnd();
			if (!CleanId.IsEmpty())
			{
				return ForwardDiagnostic(OutError, MachineSelectorFailure(Id), CleanId);
			}
			if (CleanName.IsEmpty())
			{
				return ForwardDiagnostic(OutError, MissingNameFailure(Name));
			}
			TArray<FNameSelectionEntry> Entries;
			for (const FAiBehaviorCandidate& Candidate : Catalog)
			{
				Entries.Add({Candidate.Name, Candidate.Identity.CanonicalKey, Candidate.SelectionFields});
			}
			int32 ResolvedIndex = INDEX_NONE;
			UE::UAI::Contract::FDiagnostic ResolveError;
			if (!TryResolveNameSelection(Entries, CleanName, Select,
					ResolvedIndex, ResolveError, ENameComparison::IgnoreCase))
			{
				return ForwardDiagnostic(OutError, ResolveError, CleanName);
			}
			if (!Catalog.IsValidIndex(ResolvedIndex))
			{
				return ForwardDiagnostic(OutError, IndexFailure(ResolvedIndex, Catalog.Num(), CleanName));
			}
			OutCandidate = &Catalog[ResolvedIndex];
			return true;
		}

		bool ReadTargetRevision(
			UObject& Target,
			FString& OutRevision,
			FAiBehaviorCandidateQueryError& OutError)
		{
			if (Target.GetOutermost() == GetTransientPackage())
			{
				OutRevision = Sha256(TEXT("transient|") + Target.GetClass()->GetPathName());
				return true;
			}
			FUeAgentContentGraphCapture Capture;
			FUeAgentContentGraphReadError ReadError;
			const bool bCaptured = FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
				Target, Capture, ReadError);
			if (!bCaptured || Capture.Revision.IsEmpty())
			{
				if (!ReadError.IsEmpty()) return ForwardDiagnostic(OutError, MoveTemp(ReadError));
				auto Diagnostic = CandidateDiagnostic(TEXT("ai_behavior_candidate_target_readback_failed"), Target.GetPathName(),
					bCaptured ? TEXT("The AI target readback did not provide a revision.")
						: TEXT("The AI target readback failed without a diagnostic."));
				if (bCaptured)
				{
					Diagnostic.PropertyPath = TEXT("revision");
					Diagnostic.ActualValuePresent = false;
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("target revision"));
				}
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}
			OutRevision = Capture.Revision;
			return true;
		}
	}

	bool TryQueryAiBehaviorCandidates(
		UObject& Target,
		const FString& Query,
		FAiBehaviorCandidateQueryResult& OutResult,
		FAiBehaviorCandidateQueryError& OutError,
		const FString& CandidateId,
		const FAiBehaviorCandidateContext& Context)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return ForwardDiagnostic(OutError, GameThreadFailure(Target));
		}
		if (!IsSupportedTarget(Target))
		{
			return ForwardDiagnostic(OutError, TargetTypeFailure(Target,
				{TEXT("BehaviorTree"), TEXT("BlackboardData"), TEXT("StateTree"), TEXT("EnvQuery"), TEXT("SmartObjectDefinition")}));
		}
		if (!CandidateId.TrimStartAndEnd().IsEmpty())
		{
			return ForwardDiagnostic(OutError, MachineSelectorFailure(CandidateId), CandidateId);
		}

		OutResult.Target = Target.GetPathName();
		if (!ReadTargetRevision(Target, OutResult.TargetRevision, OutError)) return false;

		if (Target.IsA<UBehaviorTree>() || Target.IsA<UEnvQuery>() || Target.IsA<UStateTree>() || Target.IsA<USmartObjectDefinition>())
		{
			FNativeAiActionCatalog Catalog;
			const bool Built = Target.IsA<UStateTree>()
				? TryBuildStateTreeCatalog(*CastChecked<UStateTree>(&Target), Catalog, OutError, Context)
				: TryBuildNativeAiCatalog(Target, false, Catalog, OutError, Context);
			if (!Built)
			{
				return false;
			}
			if (Target.IsA<USmartObjectDefinition>())
				OutResult.CatalogDiagnostic = CandidateCatalogIncomplete(
					TEXT("SmartObject discovery enumerates fixed slot/binding entries and loaded native behavior/data types; it does not cover the complete native creation menu."), OutResult.Target);
			else if (Target.IsA<UStateTree>())
				OutResult.CatalogDiagnostic = CandidateCatalogIncomplete(
					TEXT("StateTree discovery enumerates schema class-cache types and fixed structural entries; complete native creation-menu coverage is unavailable."), OutResult.Target);
			OutResult.GraphName = Catalog.EditorGraph
				? Catalog.EditorGraph->GetName() : FString();
			TArray<int32> MatchedIndices;
			for (int32 EntryIndex = 0; EntryIndex < Catalog.Entries.Num(); ++EntryIndex)
			{
				if (MatchesNativeSearch(Catalog.Entries[EntryIndex].Candidate, Query))
				{
					MatchedIndices.Add(EntryIndex);
				}
			}
			OutResult.TotalMatches = MatchedIndices.Num();
			for (const int32 MatchedIndex : MatchedIndices)
			{
				OutResult.Candidates.Add(Catalog.Entries[MatchedIndex].Candidate);
			}
			return true;
		}

		if (Context.Graph || Context.Parent || !Context.AttachmentKind.IsEmpty())
			return ForwardDiagnostic(OutError, ContextFailure(TEXT("ai_behavior_candidate_attachment_context_invalid"), Target, Context,
				TEXT("Blackboard candidate discovery does not accept graph or attachment context."), TEXT("empty graph, parent and attachment context")));
		TArray<FAiBehaviorCandidate> Catalog;
		if (!TryBuildBlackboardCandidateCatalog(Target, Catalog, OutError))
		{
			return false;
		}
		TArray<int32> MatchedIndices;
		for (int32 CandidateIndex = 0; CandidateIndex < Catalog.Num(); ++CandidateIndex)
		{
			const FAiBehaviorCandidate& Candidate = Catalog[CandidateIndex];
			if (MatchesKeywordSearch(
					Query,
					Candidate.Id,
					Candidate.Label.IsEmpty() ? Candidate.Name : Candidate.Label,
					{Candidate.Name, Candidate.Kind, Candidate.InputContract,
					 Candidate.PlacementContract, Candidate.RequiredContext}))
			{
				MatchedIndices.Add(CandidateIndex);
			}
		}
		OutResult.TotalMatches = MatchedIndices.Num();
		for (int32 MatchIndex = 0; MatchIndex < MatchedIndices.Num(); ++MatchIndex)
		{
			FAiBehaviorCandidate Candidate = Catalog[MatchedIndices[MatchIndex]];
			OutResult.Candidates.Add(MoveTemp(Candidate));
		}
		return true;
	}

	static bool CandidateSelectionFailure(UObject& Target, const FString& Name,
		const FAiBehaviorCandidateContext& Context, FAiBehaviorCandidateQueryError& OutError)
	{
		const FString ContextText = Context.AttachmentKind.IsEmpty() ? FString(TEXT("AI menu"))
			: FString::Printf(TEXT("AI %s attachment menu"), *Context.AttachmentKind);
		OutError = UeAgentAuthoringDiagnostics::CandidateFailure(OutError.Code, Name, FString(), ContextText, TEXT("ai.behavior.candidates"), OutError);
		if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Target.GetPathName();
		return false;
	}

	bool TryResolveAiBehaviorCandidate(
		UObject& Target,
		const FString& Name,
		const FString& Id,
		FAiBehaviorCandidate& OutCandidate,
		FAiBehaviorCandidateQueryError& OutError,
		const TMap<FString, FString>& Select,
		const FAiBehaviorCandidateContext& Context)
	{
		OutCandidate = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return ForwardDiagnostic(OutError, GameThreadFailure(Target));
		}
		if (!IsSupportedTarget(Target))
		{
			return ForwardDiagnostic(OutError, TargetTypeFailure(Target,
				{TEXT("BehaviorTree"), TEXT("BlackboardData"), TEXT("StateTree"), TEXT("EnvQuery"), TEXT("SmartObjectDefinition")}));
		}
		if (Target.IsA<UBehaviorTree>() || Target.IsA<UEnvQuery>() || Target.IsA<UStateTree>() || Target.IsA<USmartObjectDefinition>())
		{
			FNativeAiActionCatalog Catalog;
			FNativeAiActionEntry* Entry = nullptr;
			const bool Built = Target.IsA<UStateTree>()
				? TryBuildStateTreeCatalog(*CastChecked<UStateTree>(&Target), Catalog, OutError, Context)
				: TryBuildNativeAiCatalog(Target, false, Catalog, OutError, Context);
			if (!Built) return false;
			if (!TrySelectNativeAiAction(
					Catalog, Name, Id, Entry, OutError, Select))
			{
				return CandidateSelectionFailure(Target, Name, Context, OutError);
			}
			OutCandidate = Entry->Candidate;
			return true;
		}

		if (Context.Graph || Context.Parent || !Context.AttachmentKind.IsEmpty())
			return ForwardDiagnostic(OutError, ContextFailure(TEXT("ai_behavior_candidate_attachment_context_invalid"), Target, Context,
				TEXT("Blackboard candidate resolution does not accept graph or attachment context."), TEXT("empty graph, parent and attachment context")));
		TArray<FAiBehaviorCandidate> Catalog;
		FAiBehaviorCandidate* Candidate = nullptr;
		if (!TryBuildBlackboardCandidateCatalog(Target, Catalog, OutError)) return false;
		if (!TrySelectBlackboardCandidate(
				Catalog, Name, Id, Candidate, OutError, Select))
		{
			return CandidateSelectionFailure(Target, Name, Context, OutError);
		}
		OutCandidate = *Candidate;
		return true;
	}

	bool TryPerformAiGraphCandidate(
		UObject& Target,
		const FString& Name,
		const FString& Id,
		const FVector2f& Location,
		UEdGraphNode*& OutNode,
		FAiBehaviorCandidate& OutCandidate,
		FAiBehaviorCandidateQueryError& OutError,
		const TMap<FString, FString>& Select,
		const FAiBehaviorCandidateContext& Context)
	{
		OutNode = nullptr;
		OutCandidate = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return ForwardDiagnostic(OutError, GameThreadFailure(Target));
		}
		if (!FMath::IsFinite(Location.X) || !FMath::IsFinite(Location.Y))
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("ai_behavior_candidate_position_invalid"), Target.GetPathName(),
				TEXT("An AI graph candidate position requires finite x and y coordinates."));
			Diagnostic.SourceKind.Reset();
			Diagnostic.PropertyPath = !FMath::IsFinite(Location.X) ? TEXT("x") : TEXT("y");
			Diagnostic.Source.JsonPath = TEXT("/") + Diagnostic.PropertyPath;
			auto Actual = MakeShared<FJsonObject>();
			const auto CoordinateValue = [](float Value) -> TSharedPtr<FJsonValue>
			{
				if (FMath::IsFinite(Value)) return MakeShared<FJsonValueNumber>(Value);
				return MakeShared<FJsonValueString>(LexToString(Value));
			};
			Actual->SetField(TEXT("x"), CoordinateValue(Location.X));
			Actual->SetField(TEXT("y"), CoordinateValue(Location.Y));
			Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("finite x and y"));
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
		}

		FNativeAiActionCatalog Catalog;
		FNativeAiActionEntry* Entry = nullptr;
		if (!TryBuildNativeAiCatalog(Target, true, Catalog, OutError, Context)) return false;
		if (!TrySelectNativeAiAction(
				Catalog, Name, Id, Entry, OutError, Select))
		{
			return CandidateSelectionFailure(Target, Name, Context, OutError);
		}
		if (!Entry->Candidate.bSupported)
		{
			return ForwardDiagnostic(OutError, Entry->Candidate.SupportDiagnostic, Entry->Candidate.ActionKind);
		}

		TSet<UEdGraphNode*> NodesBefore;
		const auto CaptureNodes = [&]()
		{
			TArray<UEdGraphNode*> Nodes;
			if (Context.Parent)
			{
				for (UAIGraphNode* Node : Context.Parent->SubNodes) if (Node) Nodes.Add(Node);
			}
			else for (UEdGraphNode* Node : Catalog.EditorGraph->Nodes) if (Node) Nodes.Add(Node);
			return Nodes;
		};
		for (UEdGraphNode* Node : CaptureNodes()) NodesBefore.Add(Node);
		UEdGraphNode* const Returned = Entry->BackgroundTemplate
			? NativeNodeAction::InvokeTemplate(*Entry->BackgroundTemplate,
				*Catalog.EditorGraph, nullptr, Location, Entry->PinOrder)
			: Entry->Action->PerformAction(Catalog.EditorGraph, nullptr, Location, false);
		UEdGraphNode* Created = Returned;
		if (Context.Parent)
		{
			for (UEdGraphNode* Node : CaptureNodes())
			{
				if (NodesBefore.Contains(Node)) continue;
				if (Created)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("ai_behavior_candidate_native_action_create_ambiguous"), Entry->Candidate.Id,
						Returned ? TEXT("The native AI attachment action provided both a returned node and an observed creation result.")
							: TEXT("The native AI attachment action produced multiple new nodes."));
					Diagnostic.CandidateName = Entry->Candidate.Name;
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetBoolField(TEXT("returned_node_present"), Returned != nullptr);
					Actual->SetBoolField(TEXT("returned_matches_observed_node"), Returned == Node);
					int32 NewNodes = 0;
					for (const UEdGraphNode* Observed : CaptureNodes()) if (!NodesBefore.Contains(Observed)) ++NewNodes;
					Actual->SetNumberField(TEXT("new_nodes"), NewNodes);
					Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("one unambiguous node creation result"));
					Diagnostic.MutationStarted = true;
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
				}
				Created = Node;
			}
		}
		const FObjectPropertyBase* InstanceProperty = Created
			? FindFProperty<FObjectPropertyBase>(Created->GetClass(), TEXT("NodeInstance")) : nullptr;
		const UObject* Instance = InstanceProperty
			? InstanceProperty->GetObjectPropertyValue_InContainer(Created) : nullptr;
		if (const auto* Option = Cast<UEnvQueryOption>(Instance)) Instance = Option->Generator;
		if (!Created
			|| NodesBefore.Contains(Created)
			|| !CaptureNodes().Contains(Created)
			|| (Entry->PayloadClass && (!Instance || Instance->GetClass() != Entry->PayloadClass))
			|| (!Entry->Candidate.GraphNodeType.IsEmpty() && Created->GetClass()->GetPathName() != Entry->Candidate.GraphNodeType))
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("ai_behavior_candidate_native_action_create_failed"), Entry->Candidate.Id,
				!Created ? TEXT("The native AI action did not produce a node.")
					: NodesBefore.Contains(Created) ? TEXT("The native AI action returned a node that existed before the action.")
					: !CaptureNodes().Contains(Created) ? TEXT("The created AI node is not registered under the requested graph or parent.")
					: Entry->PayloadClass && !Instance ? TEXT("The created AI node has no payload instance.")
					: Entry->PayloadClass && Instance->GetClass() != Entry->PayloadClass ? TEXT("The created AI node payload class differs from the selected candidate.")
					: TEXT("The created AI graph-node class differs from the selected candidate template."));
			Diagnostic.CandidateName = Entry->Candidate.Name;
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetBoolField(TEXT("node_present"), Created != nullptr);
			Actual->SetBoolField(TEXT("new_node"), Created && !NodesBefore.Contains(Created));
			Actual->SetBoolField(TEXT("registered_with_context"), Created && CaptureNodes().Contains(Created));
			if (Created) Actual->SetStringField(TEXT("graph_node_class"), Created->GetClass()->GetName());
			if (Instance) Actual->SetStringField(TEXT("payload_class"), Instance->GetClass()->GetName());
			auto Expected = MakeShared<FJsonObject>();
			Expected->SetBoolField(TEXT("new_registered_node"), true);
			if (Entry->PayloadClass) Expected->SetStringField(TEXT("payload_class"), Entry->PayloadClass->GetName());
			if (!Entry->Candidate.GraphNodeType.IsEmpty()) Expected->SetStringField(TEXT("graph_node_class"),
				FPackageName::ObjectPathToObjectName(Entry->Candidate.GraphNodeType));
			Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
			auto ActualEvidence = MakeShared<FJsonObject>();
			if (Created) ActualEvidence->SetStringField(TEXT("node"), Created->GetPathName());
			if (Instance) ActualEvidence->SetStringField(TEXT("payload"), Instance->GetPathName());
			auto ExpectedEvidence = MakeShared<FJsonObject>();
			ExpectedEvidence->SetStringField(TEXT("graph_node_class"), Entry->Candidate.GraphNodeType);
			if (Entry->PayloadClass) ExpectedEvidence->SetStringField(TEXT("payload_class"), Entry->PayloadClass->GetPathName());
			Diagnostic.ActualEvidence = MakeShared<FJsonValueObject>(ActualEvidence);
			Diagnostic.ExpectedEvidence = MakeShared<FJsonValueObject>(ExpectedEvidence);
			Diagnostic.MutationMayHaveStarted = true;
			for (const UEdGraphNode* Observed : CaptureNodes())
				if (!NodesBefore.Contains(Observed)) { Diagnostic.MutationStarted = true; break; }
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
		}
		OutNode = Created;
		OutCandidate = Entry->Candidate;
		return true;
	}


}
