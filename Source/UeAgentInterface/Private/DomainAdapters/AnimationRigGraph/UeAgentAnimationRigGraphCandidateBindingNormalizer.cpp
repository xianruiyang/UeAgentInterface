#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphCandidateBindingNormalizer.h"

#include "ControlRigBlueprint.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "QueryOwners/AnimationRigGraph/UeAgentAnimationRigGraphCandidateQuery.h"
#include "UObject/Object.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::AnimationRigGraph
{
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;
	using namespace QueryOwners::AnimationRigGraph;

	namespace
	{
		bool Fail(
			FWorkspaceError& OutError,
			FString Code,
			const FString& TargetIdentity,
			FString Pointer)
		{
			OutError.Set(MoveTemp(Code), TargetIdentity, MoveTemp(Pointer));
			return false;
		}

		bool TrySetExactString(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			const FString& Expected,
			const FString& TargetIdentity,
			const FString& Pointer,
			FWorkspaceError& OutError)
		{
			FString Existing;
			if (Object->HasField(Field)
				&& (!Object->TryGetStringField(Field, Existing)
					|| Existing != Expected))
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_normalization_conflict"),
					TargetIdentity,
					Pointer + TEXT("/") + Field);
			}
			Object->SetStringField(Field, Expected);
			return true;
		}

		bool TryResolveNativeElementKind(
			const FString& SemanticId,
			FString& OutElementKind)
		{
			if (SemanticId.StartsWith(TEXT("rigvm_node:"))
				&& SemanticId.Len() > 11)
			{
				OutElementKind = TEXT("item");
				return true;
			}
			if (SemanticId.StartsWith(TEXT("rig_element:control:"))
				&& SemanticId.Len() > 20)
			{
				OutElementKind = TEXT("hierarchy");
				return true;
			}
			OutElementKind.Reset();
			return false;
		}

		bool HasOnlyCandidateSelectorFields(
			const TSharedPtr<FJsonObject>& Candidate)
		{
			if (!Candidate.IsValid())
			{
				return false;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field
				: Candidate->Values)
			{
				if (Field.Key != TEXT("name") && Field.Key != TEXT("id"))
				{
					return false;
				}
			}
			return true;
		}
	}

	bool TryNormalizeAnimationRigGraphCandidateBindings(
		const FString& TargetIdentity,
		const TSharedPtr<FJsonObject>& NativeParams,
		FWorkspaceError& OutError)
	{
		if (!NativeParams.IsValid())
		{
			return Fail(
				OutError,
				TEXT("animation_rig_graph_native_params_invalid"),
				TargetIdentity,
				TEXT("/root/operation/params"));
		}
		const TArray<TSharedPtr<FJsonValue>>* Edits = nullptr;
		if (!NativeParams->HasField(TEXT("edits")))
		{
			return true;
		}
		if (!NativeParams->TryGetArrayField(TEXT("edits"), Edits) || !Edits)
		{
			return Fail(
				OutError,
				TEXT("animation_rig_graph_candidate_edits_invalid"),
				TargetIdentity,
				TEXT("/root/operation/params/edits"));
		}
		UControlRigBlueprint* const Target = Cast<UControlRigBlueprint>(
			StaticLoadObject(
				UObject::StaticClass(),
				nullptr,
				*ObjectPathFromPackageIdentity(TargetIdentity)));

		for (int32 Index = 0; Index < Edits->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Edit = (*Edits)[Index].IsValid()
				? (*Edits)[Index]->AsObject()
				: nullptr;
			const FString Pointer = FString::Printf(
				TEXT("/root/operation/params/edits/%d"), Index);
			if (!Edit.IsValid())
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_edit_invalid"),
					TargetIdentity,
					Pointer);
			}
			FString SemanticId;
			FString NativeElementKind;
			if (!Edit->TryGetStringField(TEXT("semantic_id"), SemanticId)
				|| !TryResolveNativeElementKind(
					SemanticId, NativeElementKind)
				|| !TrySetExactString(
					Edit,
					TEXT("identity"),
					SemanticId,
					TargetIdentity,
					Pointer,
					OutError)
				|| !TrySetExactString(
					Edit,
					TEXT("element_kind"),
					NativeElementKind,
					TargetIdentity,
					Pointer,
					OutError))
			{
				if (OutError.GetCode().IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("animation_rig_graph_edit_identity_invalid"),
						TargetIdentity,
						Pointer + TEXT("/semantic_id"));
				}
				return false;
			}

			FString Intent;
			if (!Edit->TryGetStringField(TEXT("intent"), Intent)
				|| Intent.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_context_invalid"),
					TargetIdentity,
					Pointer + TEXT("/intent"));
			}
			const bool bNativeCreate = SemanticId.StartsWith(TEXT("rigvm_node:"))
				&& (Intent == TEXT("create") || Intent == TEXT("replace"));
			if (!bNativeCreate)
			{
				Edit->RemoveField(TEXT("semantic_id"));
				continue;
			}

			if (!Target)
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_target_unresolved"),
					TargetIdentity,
					TEXT("/target"));
			}
			const TSharedPtr<FJsonObject>* ValueField = nullptr;
			const TSharedPtr<FJsonObject>* CandidateField = nullptr;
			if (!Edit->TryGetObjectField(TEXT("value"), ValueField)
				|| !ValueField || !ValueField->IsValid()
				|| !(*ValueField)->TryGetObjectField(
					TEXT("candidate"), CandidateField)
				|| !CandidateField || !CandidateField->IsValid())
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_selector_required"),
					TargetIdentity,
					Pointer + TEXT("/value/candidate"));
			}
			const TSharedPtr<FJsonObject> Value = *ValueField;
			const TSharedPtr<FJsonObject> Candidate = *CandidateField;
			const FString CandidatePointer =
				Pointer + TEXT("/value/candidate");
			if (!HasOnlyCandidateSelectorFields(Candidate))
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_selector_field_unsupported"),
					TargetIdentity,
					CandidatePointer);
			}
			FDomainHjsonCandidateSelector Selector;
			if (!TryReadCandidateSelector(
					Candidate, CandidatePointer, Selector, OutError))
			{
				return false;
			}

			const TSharedPtr<FJsonObject>* Position = nullptr;
			double X = 0.0;
			double Y = 0.0;
			if (!Value->TryGetObjectField(TEXT("position"), Position)
				|| !Position || !Position->IsValid()
				|| !(*Position)->TryGetNumberField(TEXT("x"), X)
				|| !(*Position)->TryGetNumberField(TEXT("y"), Y)
				|| !FMath::IsFinite(X) || !FMath::IsFinite(Y))
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_position_required"),
					TargetIdentity,
					Pointer + TEXT("/value/position"));
			}

			FAnimationRigGraphCandidate Resolved;
			FAnimationRigGraphCandidateQueryError QueryError;
			if (!TryResolveAnimationRigGraphCandidate(
					*Target,
					Selector.Name,
					Selector.Id,
					Resolved,
					QueryError))
			{
				return Fail(
					OutError,
					QueryError.Code.IsEmpty()
						? FString(TEXT(
							"animation_rig_graph_candidate_selector_invalid"))
						: QueryError.Code,
					TargetIdentity,
					CandidatePointer);
			}
			if (!Resolved.bSupported)
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_action_kind_unsupported"),
					TargetIdentity,
					CandidatePointer);
			}
			Candidate->SetStringField(TEXT("name"), Resolved.Name);
			Candidate->SetStringField(TEXT("id"), Resolved.Id);
			Edit->RemoveField(TEXT("semantic_id"));
		}
		OutError.Reset();
		return true;
	}
}
