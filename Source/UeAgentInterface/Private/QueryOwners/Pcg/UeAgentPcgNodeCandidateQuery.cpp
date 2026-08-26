#include "QueryOwners/Pcg/UeAgentPcgNodeCandidateQuery.h"

#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "Internationalization/Text.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::QueryOwners::Pcg
{
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
		const FString CandidateKind = TEXT("pcg_node");
		const FName SupportedActionType(
			TEXT("FPCGEditorGraphSchemaAction_NewNativeElement"));

		struct FNativeActionEntry final
		{
			TSharedPtr<FEdGraphSchemaAction> Action;
			FPcgNodeCandidate Candidate;
			FString Signature;
		};

		struct FNativeActionCatalog final
		{
			TStrongObjectPtr<UEdGraph> EditorGraph;
			TArray<FNativeActionEntry> Entries;
		};

		bool Fail(
			FPcgNodeCandidateQueryError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
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

		bool IsSupportedTarget(const UObject& Target)
		{
			return Target.GetClass() == UPCGGraph::StaticClass();
		}

		bool ReadTargetRevision(
			UObject& Target,
			FString& OutRevision,
			FPcgNodeCandidateQueryError& OutError)
		{
			if (Target.GetOutermost() == GetTransientPackage())
			{
				OutRevision = TEXT("transient:")
					+ Target.GetClass()->GetPathName();
				return true;
			}
			FUeAgentContentGraphCapture Capture;
			FUeAgentContentGraphReadError ReadError;
			if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
					Target, Capture, ReadError)
				|| Capture.Revision.IsEmpty())
			{
				return Fail(
					OutError,
					ReadError.Code.IsEmpty()
						? FString(TEXT("pcg_candidate_target_readback_failed"))
						: ReadError.Code,
					ReadError.SubjectId,
					ReadError.Detail);
			}
			OutRevision = Capture.Revision;
			return true;
		}

		void AppendSignatureField(
			FString& Signature,
			const FString& Name,
			const FString& Value)
		{
			Signature += FString::Printf(
				TEXT("%d:%s=%d:%s|"),
				Name.Len(), *Name, Value.Len(), *Value);
		}

		FString StableText(const FText& Text)
		{
			FString Result;
			FTextStringHelper::WriteToBuffer(Result, Text, true, false);
			return Result;
		}

		FString RemovePointerAddresses(const FString& Input)
		{
			FString Result;
			Result.Reserve(Input.Len());
			for (int32 Index = 0; Index < Input.Len();)
			{
				if (Index + 2 < Input.Len()
					&& Input[Index] == TEXT('0')
					&& Input[Index + 1] == TEXT('x')
					&& FChar::IsHexDigit(Input[Index + 2]))
				{
					Result += TEXT("<address>");
					Index += 2;
					while (Index < Input.Len()
						&& FChar::IsHexDigit(Input[Index]))
					{
						++Index;
					}
					continue;
				}
				Result.AppendChar(Input[Index]);
				++Index;
			}
			return Result;
		}

		UScriptStruct* ResolveActionScriptStruct(const FName TypeId)
		{
			FString StructName = TypeId.ToString();
			if (StructName.StartsWith(TEXT("F")))
			{
				StructName.RightChopInline(1, EAllowShrinking::No);
			}
			return FindObject<UScriptStruct>(
				nullptr, *(TEXT("/Script/PCGEditor.") + StructName));
		}

		bool BuildActionSignature(
			const FEdGraphSchemaAction& Action,
			FString& OutSignature,
			FPcgNodeCandidateQueryError& OutError)
		{
			OutSignature.Reset();
			AppendSignatureField(
				OutSignature, TEXT("type_id"),
				Action.GetTypeId().ToString());
			AppendSignatureField(
				OutSignature, TEXT("category"),
				StableText(Action.GetCategory()));
			AppendSignatureField(
				OutSignature, TEXT("menu"),
				StableText(Action.GetMenuDescription()));
			AppendSignatureField(
				OutSignature, TEXT("tooltip"),
				StableText(Action.GetTooltipDescription()));
			AppendSignatureField(
				OutSignature, TEXT("keywords"),
				StableText(Action.GetKeywords()));
			AppendSignatureField(
				OutSignature, TEXT("grouping"), LexToString(Action.Grouping));
			AppendSignatureField(
				OutSignature, TEXT("section_id"), LexToString(Action.SectionID));

			UScriptStruct* const ScriptStruct = ResolveActionScriptStruct(
				Action.GetTypeId());
			if (!ScriptStruct)
			{
				return Fail(
					OutError,
					TEXT("pcg_candidate_action_struct_unresolved"),
					Action.GetTypeId().ToString());
			}
			TArray<const FProperty*> Properties;
			for (TFieldIterator<FProperty> It(
				ScriptStruct, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				Properties.Add(*It);
			}
			Properties.Sort([](const FProperty& Left, const FProperty& Right)
			{
				return Left.GetName().Compare(
					Right.GetName(), ESearchCase::CaseSensitive) < 0;
			});
			for (const FProperty* Property : Properties)
			{
				FString Exported;
				const void* const Value =
					Property->ContainerPtrToValuePtr<void>(&Action);
				Property->ExportTextItem_Direct(
					Exported, Value, nullptr, nullptr, PPF_None);
				AppendSignatureField(
					OutSignature,
					TEXT("property.") + Property->GetName(),
					RemovePointerAddresses(Exported));
			}
			return true;
		}

		bool BuildCandidate(
			const FEdGraphSchemaAction& Action,
			const FString& Signature,
			FPcgNodeCandidate& OutCandidate,
			FPcgNodeCandidateQueryError& OutError)
		{
			UeAgentPaletteSearch::FCandidate IdentityCandidate;
			IdentityCandidate.Kind = CandidateKind;
			IdentityCandidate.Name = Action.GetMenuDescription().ToString();
			IdentityCandidate.DisplayName = IdentityCandidate.Name;
			IdentityCandidate.CategoryPath = Action.GetCategory().ToString();
			IdentityCandidate.Tooltip =
				Action.GetTooltipDescription().ToString();
			IdentityCandidate.Keywords = Action.GetKeywords().ToString();
			IdentityCandidate.Type = Action.GetTypeId().ToString();
			IdentityCandidate.FullName = IdentityCandidate.Type + TEXT(":")
				+ FMD5::HashAnsiString(*Signature);
			FString IdentityError;
			if (!UeAgentPaletteSearch::SetCandidateIdentity(
					TEXT("pcg_node"),
					IdentityCandidate.FullName,
					IdentityCandidate.Name,
					IdentityCandidate,
					IdentityError))
			{
				return Fail(
					OutError,
					TEXT("pcg_candidate_identity_failed"),
					Action.GetTypeId().ToString(),
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
			OutCandidate.bSupported = Action.GetTypeId() == SupportedActionType;
			OutCandidate.WriteSupport = OutCandidate.bSupported
				? FString(TEXT("supported"))
				: FString(TEXT("search_only"));
			OutCandidate.SupportReason = OutCandidate.bSupported
				? FString(TEXT("native_schema_action"))
				: FString(TEXT("native_action_kind_not_yet_verified"));
			return true;
		}

		bool TryBuildNativeCatalog(
			UPCGGraph& Target,
			FNativeActionCatalog& OutCatalog,
			FPcgNodeCandidateQueryError& OutError)
		{
			OutCatalog = {};
			if (!FModuleManager::Get().LoadModulePtr<IModuleInterface>(
					TEXT("PCGEditor")))
			{
				return Fail(
					OutError, TEXT("pcg_candidate_editor_module_unavailable"));
			}
			UClass* const EditorGraphClass = FindObject<UClass>(
				nullptr, TEXT("/Script/PCGEditor.PCGEditorGraph"));
			UClass* const SchemaClass = FindObject<UClass>(
				nullptr, TEXT("/Script/PCGEditor.PCGEditorGraphSchema"));
			if (!EditorGraphClass
				|| !EditorGraphClass->IsChildOf(UEdGraph::StaticClass())
				|| !SchemaClass
				|| !SchemaClass->IsChildOf(UEdGraphSchema::StaticClass()))
			{
				return Fail(
					OutError, TEXT("pcg_candidate_editor_context_unavailable"));
			}
			UEdGraph* const EditorGraph = Cast<UEdGraph>(NewObject<UObject>(
				GetTransientPackage(), EditorGraphClass));
			FObjectPropertyBase* const PcgGraphProperty =
				FindFProperty<FObjectPropertyBase>(
					EditorGraphClass, TEXT("PCGGraph"));
			FClassProperty* const SchemaProperty = FindFProperty<FClassProperty>(
				UEdGraph::StaticClass(), TEXT("Schema"));
			if (!EditorGraph || !PcgGraphProperty || !SchemaProperty)
			{
				return Fail(
					OutError, TEXT("pcg_candidate_editor_context_unavailable"));
			}
			PcgGraphProperty->SetObjectPropertyValue_InContainer(
				EditorGraph, &Target);
			SchemaProperty->SetPropertyValue_InContainer(
				EditorGraph, SchemaClass);
			if (PcgGraphProperty->GetObjectPropertyValue_InContainer(EditorGraph)
					!= &Target
				|| EditorGraph->GetSchema() == nullptr)
			{
				return Fail(
					OutError, TEXT("pcg_candidate_editor_context_invalid"),
					Target.GetPathName());
			}
			OutCatalog.EditorGraph.Reset(EditorGraph);

			FGraphContextMenuBuilder MenuBuilder(EditorGraph);
			EditorGraph->GetSchema()->GetGraphContextActions(MenuBuilder);
			TSet<FString> SeenSignatures;
			TSet<FString> SeenIds;
			for (int32 Index = 0; Index < MenuBuilder.GetNumActions(); ++Index)
			{
				const TSharedPtr<FEdGraphSchemaAction>& Action =
					MenuBuilder.GetSchemaAction(Index);
				if (!Action.IsValid())
				{
					return Fail(
						OutError, TEXT("pcg_candidate_native_action_invalid"),
						FString::FromInt(Index));
				}
				FNativeActionEntry& Entry =
					OutCatalog.Entries.AddDefaulted_GetRef();
				Entry.Action = Action;
				if (!BuildActionSignature(*Action, Entry.Signature, OutError)
					|| !BuildCandidate(
						*Action, Entry.Signature, Entry.Candidate, OutError))
				{
					return false;
				}
				if (SeenSignatures.Contains(Entry.Signature)
					|| SeenIds.Contains(Entry.Candidate.Id))
				{
					return Fail(
						OutError, TEXT("pcg_candidate_identity_ambiguous"),
						Entry.Candidate.Id, Entry.Candidate.Name);
				}
				SeenSignatures.Add(Entry.Signature);
				SeenIds.Add(Entry.Candidate.Id);
			}
			return true;
		}

		bool MatchesSearch(
			const FPcgNodeCandidate& Candidate,
			const FString& Query)
		{
			const FString Needle = Normalize(Query);
			if (Needle.IsEmpty())
			{
				return true;
			}
			return Normalize(
				Candidate.Name + TEXT("|") + Candidate.Category + TEXT("|")
				+ Candidate.Summary + TEXT("|") + Candidate.ActionKind)
				.Contains(Needle);
		}

		bool TrySelectNativeAction(
			FNativeActionCatalog& Catalog,
			const FString& Name,
			const FString& Id,
			FNativeActionEntry*& OutEntry,
			FPcgNodeCandidateQueryError& OutError)
		{
			OutEntry = nullptr;
			const FString CleanName = Name.TrimStartAndEnd();
			const FString CleanId = Id.TrimStartAndEnd();
			if (CleanName.IsEmpty() && CleanId.IsEmpty())
			{
				return Fail(
					OutError, TEXT("pcg_candidate_selector_required"));
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
					OutError, TEXT("pcg_candidate_not_found"),
					CleanId.IsEmpty() ? CleanName : CleanId);
			}
			if (Matches.Num() != 1)
			{
				return Fail(
					OutError, TEXT("pcg_candidate_ambiguous"),
					CleanName, FString::FromInt(Matches.Num()));
			}
			OutEntry = Matches[0];
			return true;
		}
	}

	bool TryQueryPcgNodeCandidates(
		UObject& Target,
		const FString& Query,
		FPcgNodeCandidateQueryResult& OutResult,
		FPcgNodeCandidateQueryError& OutError,
		const int32 Limit,
		const int32 Cursor,
		const FString& CandidateId)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(
				OutError, TEXT("pcg_candidate_game_thread_required"),
				Target.GetPathName());
		}
		if (!IsSupportedTarget(Target))
		{
			return Fail(
				OutError, TEXT("pcg_candidate_target_type_unsupported"),
				Target.GetClass()->GetPathName());
		}
		if (Limit < 1 || Limit > 64 || Cursor < 0)
		{
			return Fail(
				OutError, TEXT("pcg_candidate_page_invalid"),
				Target.GetPathName(),
				TEXT("limit must be 1..64 and cursor must be non-negative"));
		}

		OutResult.Target = Target.GetPathName();
		if (!ReadTargetRevision(Target, OutResult.TargetRevision, OutError))
		{
			return false;
		}
		FNativeActionCatalog Catalog;
		if (!TryBuildNativeCatalog(
				static_cast<UPCGGraph&>(Target), Catalog, OutError))
		{
			return false;
		}
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

		TArray<const FPcgNodeCandidate*> Matches;
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

	bool TryResolvePcgNodeCandidate(
		UPCGGraph& Target,
		const FString& Name,
		const FString& Id,
		FPcgNodeCandidate& OutCandidate,
		FPcgNodeCandidateQueryError& OutError)
	{
		OutCandidate = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(
				OutError, TEXT("pcg_candidate_game_thread_required"),
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

	bool TryPerformPcgNodeCandidate(
		UPCGGraph& Target,
		const FString& Name,
		const FString& Id,
		const FVector2f& Location,
		UPCGNode*& OutNode,
		FPcgNodeCandidate& OutCandidate,
		FPcgNodeCandidateQueryError& OutError)
	{
		OutNode = nullptr;
		OutCandidate = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(
				OutError, TEXT("pcg_candidate_game_thread_required"),
				Target.GetPathName());
		}
		if (!FMath::IsFinite(Location.X) || !FMath::IsFinite(Location.Y))
		{
			return Fail(
				OutError, TEXT("pcg_candidate_position_invalid"));
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
				OutError, TEXT("pcg_candidate_action_kind_unsupported"),
				Entry->Candidate.ActionKind,
				Entry->Candidate.SupportReason);
		}

		TSet<UPCGNode*> NodesBefore;
		for (UPCGNode* Node : Target.GetNodes())
		{
			if (Node)
			{
				NodesBefore.Add(Node);
			}
		}
		UEdGraphNode* const ReturnedEditorNode = Entry->Action->PerformAction(
			Catalog.EditorGraph.Get(), nullptr, Location, false);
		TArray<UPCGNode*> CreatedNodes;
		for (UPCGNode* Node : Target.GetNodes())
		{
			if (Node && !NodesBefore.Contains(Node))
			{
				CreatedNodes.Add(Node);
			}
		}
		if (!ReturnedEditorNode || CreatedNodes.Num() != 1)
		{
			return Fail(
				OutError, TEXT("pcg_candidate_native_action_create_failed"),
				Entry->Candidate.Id,
				FString::Printf(
					TEXT("returned=%s created_nodes=%d"),
					ReturnedEditorNode ? TEXT("true") : TEXT("false"),
					CreatedNodes.Num()));
		}
		OutNode = CreatedNodes[0];
		OutCandidate = Entry->Candidate;
		return true;
	}
}
