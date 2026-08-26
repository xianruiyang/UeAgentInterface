#include "QueryOwners/AnimationRigGraph/UeAgentAnimationRigGraphCandidateQuery.h"

#include "BlueprintActionDatabase.h"
#include "BlueprintActionFilter.h"
#include "BlueprintNodeSignature.h"
#include "BlueprintNodeSpawner.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "ControlRigBlueprint.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "EdGraph/NodeSpawners/RigVMEdGraphTemplateNodeSpawner.h"
#include "EdGraph/NodeSpawners/RigVMEdGraphUnitNodeSpawner.h"
#include "EdGraph/RigVMEdGraph.h"
#include "EdGraph/RigVMEdGraphNode.h"
#include "Editor/RigVMActionMenuBuilder.h"
#include "Editor/RigVMActionMenuItem.h"
#include "Editor/RigVMActionMenuUtils.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/RigVMGraph.h"
#include "UObject/UnrealType.h"

#include <openssl/sha.h>

namespace UE::UAI::QueryOwners::AnimationRigGraph
{
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
		const FString CandidateKind = TEXT("rigvm_action");
		const FString CoverageScope =
			TEXT("control_rig_default_graph_blank_context");

		struct FNativeActionEntry final
		{
			TSharedPtr<FEdGraphSchemaAction> Action;
			FString Signature;
			FAnimationRigGraphCandidate Candidate;
		};

		struct FNativeActionCatalog final
		{
			URigVMEdGraph* EditorGraph = nullptr;
			TArray<FNativeActionEntry> Entries;
		};

		bool Fail(
			FAnimationRigGraphCandidateQueryError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		FString Sha256(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length(),
				Digest);
			FString Hex;
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}

		FString Normalize(const FString& Value)
		{
			FString Result;
			for (const TCHAR Character : Value.ToLower())
			{
				if (FChar::IsAlnum(Character))
				{
					Result.AppendChar(Character);
				}
			}
			return Result;
		}

		bool ReadTargetRevision(
			UControlRigBlueprint& Target,
			FString& OutRevision,
			FAnimationRigGraphCandidateQueryError& OutError)
		{
			if (Target.GetOutermost() == GetTransientPackage())
			{
				OutRevision = Sha256(
					TEXT("transient|") + Target.GetClass()->GetPathName());
				return true;
			}
			FUeAgentContentGraphCapture Capture;
			FUeAgentContentGraphReadError ReadError;
			if (FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
					Target, Capture, ReadError)
				&& !Capture.Revision.IsEmpty())
			{
				OutRevision = Capture.Revision;
				return true;
			}
			const UPackage* const Package = Target.GetOutermost();
			if (Package && Package != GetTransientPackage())
			{
				OutRevision = FString::Printf(
					TEXT("%s:%s:%s"),
					*Package->GetPersistentGuid().ToString(EGuidFormats::Digits),
					Package->IsDirty() ? TEXT("dirty") : TEXT("clean"),
					*Sha256(Target.GetClass()->GetPathName()
						+ TEXT("|") + Target.GetPathName()));
				return true;
			}
			return Fail(
				OutError,
				TEXT("animation_rig_graph_candidate_target_readback_failed"),
				Target.GetPathName(),
				ReadError.Code);
		}

		bool TryPrepareNativeContext(
			UControlRigBlueprint& Target,
			URigVMEdGraph*& OutEditorGraph,
			FAnimationRigGraphCandidateQueryError& OutError)
		{
			OutEditorGraph = nullptr;
			URigVMGraph* const Model = Target.GetDefaultModel();
			URigVMEdGraph* const EditorGraph = Model
				? Cast<URigVMEdGraph>(Target.GetEdGraph(Model))
				: nullptr;
			if (!Model || !EditorGraph)
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_context_unavailable"),
					Target.GetPathName(),
					TEXT("default RigVM model/editor graph is unavailable"));
			}

			// This is the same public delegate initialization used by the editor.
			// It does not open an asset window and is required by native spawners.
			EditorGraph->InitializeFromBlueprint(&Target);

			UClass* const ActionKey = Target.GetClass();
			FBlueprintActionDatabase& ActionDatabase =
				FBlueprintActionDatabase::Get();
			const FBlueprintActionDatabase::FActionRegistry& Registry =
				ActionDatabase.GetAllActions();
			if (!Registry.Contains(ActionKey)
				|| Registry.FindChecked(ActionKey).IsEmpty())
			{
				ActionDatabase.RefreshClassActions(ActionKey);
			}
			OutEditorGraph = EditorGraph;
			return true;
		}

		bool TryBuildSpawnerSemanticSignature(
			const FEdGraphSchemaAction& Action,
			const UBlueprintNodeSpawner& Spawner,
			FString& OutSignature,
			FAnimationRigGraphCandidateQueryError& OutError)
		{
			FString SpawnerIdentity;
			if (Spawner.IsA<URigVMEdGraphTemplateNodeSpawner>())
			{
				// UE 5.6 passes a delimiterless value to FBlueprintNodeSignature
				// for this spawner, so every template reports `(None="")`.
				// TemplateNotation is the native semantic input to Invoke(), but
				// the engine exposes no getter. Read only that reflected property;
				// never fall back to transient object names or menu ordering.
				const FNameProperty* const NotationProperty =
					FindFProperty<FNameProperty>(
						Spawner.GetClass(), TEXT("TemplateNotation"));
				const FName Notation = NotationProperty
					? NotationProperty->GetPropertyValue_InContainer(&Spawner)
					: NAME_None;
				if (Notation.IsNone())
				{
					return Fail(
						OutError,
						TEXT("animation_rig_graph_candidate_template_identity_unavailable"),
						Action.GetMenuDescription().ToString(),
						TEXT("native TemplateNotation is unavailable"));
				}
				SpawnerIdentity = TEXT("template_notation=")
					+ Notation.ToString();
			}
			else
			{
				const FBlueprintNodeSignature NativeSignature =
					Spawner.GetSpawnerSignature();
				if (!NativeSignature.IsValid())
				{
					return Fail(
						OutError,
						TEXT("animation_rig_graph_candidate_spawner_identity_invalid"),
						Action.GetMenuDescription().ToString());
				}
				SpawnerIdentity = TEXT("spawner_signature=")
					+ NativeSignature.ToString();
			}

			OutSignature = TEXT("action_type=") + Action.GetTypeId().ToString()
				+ TEXT("|spawner_class=") + Spawner.GetClass()->GetPathName()
				+ TEXT("|") + SpawnerIdentity;
			return true;
		}

		bool BuildCandidate(
			const FEdGraphSchemaAction& Action,
			FAnimationRigGraphCandidate& OutCandidate,
			FString& OutSignature,
			FAnimationRigGraphCandidateQueryError& OutError)
		{
			if (Action.GetTypeId() != FRigVMActionMenuItem::StaticGetTypeId())
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_native_action_unsupported"),
					Action.GetTypeId().ToString(),
					TEXT("native RigVM creation menu returned a non-spawner action"));
			}
			const FRigVMActionMenuItem& Item =
				static_cast<const FRigVMActionMenuItem&>(Action);
			const UBlueprintNodeSpawner* const Spawner = Item.GetRawAction();
			if (!Spawner)
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_spawner_identity_invalid"),
					Action.GetMenuDescription().ToString());
			}
			if (!TryBuildSpawnerSemanticSignature(
					Action, *Spawner, OutSignature, OutError))
			{
				return false;
			}
			UeAgentPaletteSearch::FCandidate IdentityCandidate;
			IdentityCandidate.Kind = CandidateKind;
			IdentityCandidate.Name =
				Action.GetMenuDescription().ToString();
			IdentityCandidate.DisplayName = IdentityCandidate.Name;
			IdentityCandidate.CategoryPath = Action.GetCategory().ToString();
			IdentityCandidate.Tooltip =
				Action.GetTooltipDescription().ToString();
			IdentityCandidate.Keywords = Action.GetKeywords().ToString();
			IdentityCandidate.Type = Spawner->GetClass()->GetPathName();
			IdentityCandidate.FullName = OutSignature;
			FString IdentityError;
			if (IdentityCandidate.Name.IsEmpty()
				|| !UeAgentPaletteSearch::SetCandidateIdentity(
					TEXT("rigvm_action"),
					OutSignature,
					IdentityCandidate.Name,
					IdentityCandidate,
					IdentityError))
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_identity_failed"),
					Action.GetMenuDescription().ToString(),
					IdentityError);
			}

			OutCandidate = {};
			OutCandidate.Id = IdentityCandidate.GetPublicId();
			OutCandidate.Name = IdentityCandidate.Name;
			OutCandidate.Label = IdentityCandidate.DisplayName;
			OutCandidate.Kind = CandidateKind;
			OutCandidate.ActionKind = IdentityCandidate.Type;
			OutCandidate.Category = IdentityCandidate.CategoryPath;
			OutCandidate.Summary = IdentityCandidate.Tooltip;
			OutCandidate.bSupported =
				Spawner->IsA<URigVMEdGraphUnitNodeSpawner>();
			OutCandidate.WriteSupport = OutCandidate.bSupported
				? FString(TEXT("supported"))
				: FString(TEXT("search_only"));
			OutCandidate.SupportReason = OutCandidate.bSupported
				? FString(TEXT("native_rigvm_unit_spawner"))
				: FString(TEXT("native_action_kind_not_yet_persisted"));
			if (OutCandidate.bSupported)
			{
				OutCandidate.SupportedOperations = {TEXT("create")};
			}
			return true;
		}

		bool TryBuildNativeCatalog(
			UControlRigBlueprint& Target,
			FNativeActionCatalog& OutCatalog,
			FAnimationRigGraphCandidateQueryError& OutError)
		{
			OutCatalog = {};
			if (!TryPrepareNativeContext(
					Target, OutCatalog.EditorGraph, OutError))
			{
				return false;
			}

			FBlueprintActionContext Context;
			Context.Blueprints.Add(&Target);
			Context.Graphs.Add(OutCatalog.EditorGraph);
			FRigVMActionMenuBuilder MenuBuilder;
			FRigVMActionMenuUtils::MakeContextMenu(
				Context, true, 0, MenuBuilder);

			TSet<FString> SeenSignatures;
			TSet<FString> SeenIds;
			for (int32 Index = 0; Index < MenuBuilder.GetNumActions(); ++Index)
			{
				const TSharedPtr<FEdGraphSchemaAction>& Action =
					MenuBuilder.GetSchemaAction(Index);
				if (!Action.IsValid())
				{
					return Fail(
						OutError,
						TEXT("animation_rig_graph_candidate_native_action_invalid"),
						FString::FromInt(Index));
				}
				FNativeActionEntry& Entry =
					OutCatalog.Entries.AddDefaulted_GetRef();
				Entry.Action = Action;
				if (!BuildCandidate(
						*Action, Entry.Candidate, Entry.Signature, OutError))
				{
					return false;
				}
				if (SeenSignatures.Contains(Entry.Signature)
					|| SeenIds.Contains(Entry.Candidate.Id))
				{
					return Fail(
						OutError,
						TEXT("animation_rig_graph_candidate_identity_ambiguous"),
						Entry.Candidate.Id,
						Entry.Candidate.Name);
				}
				SeenSignatures.Add(Entry.Signature);
				SeenIds.Add(Entry.Candidate.Id);
			}
			return true;
		}

		bool MatchesSearch(
			const FAnimationRigGraphCandidate& Candidate,
			const FString& Query)
		{
			const FString Needle = Normalize(Query);
			return Needle.IsEmpty()
				|| Normalize(
					Candidate.Name + TEXT("|") + Candidate.Category
					+ TEXT("|") + Candidate.Summary + TEXT("|")
					+ Candidate.ActionKind).Contains(Needle);
		}

		bool TrySelectNativeAction(
			FNativeActionCatalog& Catalog,
			const FString& Name,
			const FString& Id,
			FNativeActionEntry*& OutEntry,
			FAnimationRigGraphCandidateQueryError& OutError)
		{
			OutEntry = nullptr;
			const FString CleanName = Name.TrimStartAndEnd();
			const FString CleanId = Id.TrimStartAndEnd();
			if (CleanName.IsEmpty() && CleanId.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_selector_required"));
			}
			TArray<FNativeActionEntry*> Matches;
			for (FNativeActionEntry& Entry : Catalog.Entries)
			{
				if (!CleanId.IsEmpty()
					&& !Entry.Candidate.Id.Equals(
						CleanId, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (!CleanName.IsEmpty()
					&& !Entry.Candidate.Name.Equals(
						CleanName, ESearchCase::CaseSensitive))
				{
					continue;
				}
				Matches.Add(&Entry);
			}
			if (Matches.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_not_found"),
					CleanId.IsEmpty() ? CleanName : CleanId);
			}
			if (Matches.Num() != 1)
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_ambiguous"),
					CleanName,
					FString::FromInt(Matches.Num()));
			}
			OutEntry = Matches[0];
			return true;
		}
	}

	bool TryQueryAnimationRigGraphCandidates(
		UObject& Target,
		const FString& Query,
		FAnimationRigGraphCandidateQueryResult& OutResult,
		FAnimationRigGraphCandidateQueryError& OutError,
		const int32 Limit,
		const int32 Cursor,
		const FString& CandidateId)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(
				OutError,
				TEXT("animation_rig_graph_candidate_game_thread_required"),
				Target.GetPathName());
		}
		UControlRigBlueprint* const Blueprint =
			Cast<UControlRigBlueprint>(&Target);
		if (!Blueprint)
		{
			return Fail(
				OutError,
				TEXT("animation_rig_graph_candidate_target_type_unsupported"),
				Target.GetClass()->GetPathName(),
				TEXT("native RigVM creation candidates require a ControlRigBlueprint"));
		}
		if (Limit < 1 || Limit > 64 || Cursor < 0)
		{
			return Fail(
				OutError,
				TEXT("animation_rig_graph_candidate_page_invalid"),
				Target.GetPathName(),
				TEXT("limit must be 1..64 and cursor must be non-negative"));
		}

		OutResult.Target = Target.GetPathName();
		if (!ReadTargetRevision(*Blueprint, OutResult.TargetRevision, OutError))
		{
			return false;
		}
		FNativeActionCatalog Catalog;
		if (!TryBuildNativeCatalog(*Blueprint, Catalog, OutError))
		{
			return false;
		}
		OutResult.GraphName = Catalog.EditorGraph->GetName();
		OutResult.CoverageScope = CoverageScope;
		OutResult.NativeActionCount = Catalog.Entries.Num();
		for (const FNativeActionEntry& Entry : Catalog.Entries)
		{
			if (Entry.Candidate.bSupported)
			{
				++OutResult.ExecutableActionCount;
			}
		}
		OutResult.bCoverageComplete = true;
		OutResult.bCreationCoverageComplete =
			OutResult.NativeActionCount == OutResult.ExecutableActionCount;

		TArray<const FAnimationRigGraphCandidate*> Matches;
		const FString CleanCandidateId = CandidateId.TrimStartAndEnd();
		for (const FNativeActionEntry& Entry : Catalog.Entries)
		{
			if (!CleanCandidateId.IsEmpty()
				&& !Entry.Candidate.Id.Equals(
					CleanCandidateId, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (MatchesSearch(Entry.Candidate, Query))
			{
				Matches.Add(&Entry.Candidate);
			}
		}
		OutResult.TotalMatches = Matches.Num();
		const int32 Start = FMath::Min(Cursor, Matches.Num());
		const int32 End = FMath::Min(Start + Limit, Matches.Num());
		for (int32 Index = Start; Index < End; ++Index)
		{
			OutResult.Candidates.Add(*Matches[Index]);
		}
		OutResult.bHasMore = End < OutResult.TotalMatches;
		OutResult.bComplete = !OutResult.bHasMore;
		OutResult.NextCursor = OutResult.bHasMore ? End : INDEX_NONE;
		return true;
	}

	bool TryResolveAnimationRigGraphCandidate(
		UControlRigBlueprint& Target,
		const FString& Name,
		const FString& Id,
		FAnimationRigGraphCandidate& OutCandidate,
		FAnimationRigGraphCandidateQueryError& OutError)
	{
		OutCandidate = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(
				OutError,
				TEXT("animation_rig_graph_candidate_game_thread_required"),
				Target.GetPathName());
		}
		FNativeActionCatalog Catalog;
		FNativeActionEntry* Entry = nullptr;
		if (!TryBuildNativeCatalog(Target, Catalog, OutError)
			|| !TrySelectNativeAction(Catalog, Name, Id, Entry, OutError))
		{
			return false;
		}
		OutCandidate = Entry->Candidate;
		return true;
	}

	bool TryPerformAnimationRigGraphCandidate(
		UControlRigBlueprint& Target,
		const FString& Name,
		const FString& Id,
		const FVector2f& Location,
		URigVMUnitNode*& OutNode,
		FAnimationRigGraphCandidate& OutCandidate,
		FAnimationRigGraphCandidateQueryError& OutError)
	{
		OutNode = nullptr;
		OutCandidate = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(
				OutError,
				TEXT("animation_rig_graph_candidate_game_thread_required"),
				Target.GetPathName());
		}
		if (!FMath::IsFinite(Location.X) || !FMath::IsFinite(Location.Y))
		{
			return Fail(
				OutError,
				TEXT("animation_rig_graph_candidate_position_invalid"));
		}

		FNativeActionCatalog Catalog;
		FNativeActionEntry* Entry = nullptr;
		if (!TryBuildNativeCatalog(Target, Catalog, OutError)
			|| !TrySelectNativeAction(Catalog, Name, Id, Entry, OutError))
		{
			return false;
		}
		if (!Entry->Candidate.bSupported)
		{
			return Fail(
				OutError,
				TEXT("animation_rig_graph_candidate_action_kind_unsupported"),
				Entry->Candidate.ActionKind,
				Entry->Candidate.SupportReason);
		}

		URigVMGraph* const Model = Target.GetDefaultModel();
		TSet<URigVMNode*> NodesBefore;
		if (!Model)
		{
			return Fail(
				OutError,
				TEXT("animation_rig_graph_candidate_context_unavailable"),
				Target.GetPathName());
		}
		for (URigVMNode* Node : Model->GetNodes())
		{
			if (Node)
			{
				NodesBefore.Add(Node);
			}
		}

		UEdGraphNode* const Returned = Entry->Action->PerformAction(
			Catalog.EditorGraph, nullptr, Location, false);
		URigVMEdGraphNode* const EditorNode =
			Cast<URigVMEdGraphNode>(Returned);
		URigVMUnitNode* const UnitNode = EditorNode
			? Cast<URigVMUnitNode>(EditorNode->GetModelNode())
			: nullptr;
		if (!UnitNode || NodesBefore.Contains(UnitNode)
			|| !Model->GetNodes().Contains(UnitNode))
		{
			return Fail(
				OutError,
				TEXT("animation_rig_graph_candidate_native_action_create_failed"),
				Entry->Candidate.Id,
				FString::Printf(
					TEXT("returned=%s model_unit=%s"),
					Returned ? TEXT("true") : TEXT("false"),
					UnitNode ? TEXT("true") : TEXT("false")));
		}
		OutNode = UnitNode;
		OutCandidate = Entry->Candidate;
		return true;
	}
}
