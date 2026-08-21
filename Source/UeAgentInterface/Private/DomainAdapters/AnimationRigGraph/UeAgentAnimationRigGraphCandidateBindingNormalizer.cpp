#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphCandidateBindingNormalizer.h"

#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "QueryOwners/AnimationRigGraph/UeAgentAnimationRigGraphCandidateQuery.h"
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

		bool TryRequiredString(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			FString& OutValue,
			const FString& TargetIdentity,
			const FString& Pointer,
			FWorkspaceError& OutError)
		{
			if (!Object.IsValid()
				|| !Object->TryGetStringField(Field, OutValue)
				|| OutValue.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_field_required"),
					TargetIdentity,
					Pointer + TEXT("/") + Field);
			}
			return true;
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
		UObject* const TargetObject = StaticLoadObject(
			UObject::StaticClass(),
			nullptr,
			*ObjectPathFromPackageIdentity(TargetIdentity));
		if (!TargetObject)
		{
			return Fail(
				OutError,
				TEXT("animation_rig_graph_candidate_target_unresolved"),
				TargetIdentity,
				TEXT("/target"));
		}

		for (int32 Index = 0; Index < Edits->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Edit = (*Edits)[Index].IsValid()
				? (*Edits)[Index]->AsObject() : nullptr;
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
				|| !TryResolveNativeElementKind(SemanticId, NativeElementKind)
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
			const TSharedPtr<FJsonObject>* ValueField = nullptr;
			if (!Edit->TryGetObjectField(TEXT("value"), ValueField)
				|| !ValueField || !ValueField->IsValid()
				|| !(*ValueField)->HasField(TEXT("candidate")))
			{
				Edit->RemoveField(TEXT("semantic_id"));
				continue;
			}
			const TSharedPtr<FJsonObject> Value = *ValueField;
			const TSharedPtr<FJsonObject>* CandidateField = nullptr;
			if (!Value->TryGetObjectField(TEXT("candidate"), CandidateField)
				|| !CandidateField || !CandidateField->IsValid())
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_invalid"),
					TargetIdentity,
					Pointer + TEXT("/value/candidate"));
			}
			FString Intent;
			if (!Edit->TryGetStringField(TEXT("intent"), Intent)
				|| Intent != TEXT("create")
				|| !SemanticId.StartsWith(TEXT("rigvm_node:"))
				|| SemanticId.Len() == 11)
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_context_invalid"),
					TargetIdentity,
					Pointer);
			}
			const TSharedPtr<FJsonObject> Candidate = *CandidateField;
			FDomainHjsonCandidateBinding Binding;
			if (!TryReadCandidateBinding(
					Candidate,
					Pointer + TEXT("/value/candidate"),
					Binding,
					OutError))
			{
				return false;
			}
			FString CandidateTargetRef;
			FString CandidateTargetRevision;
			if (!TryRequiredString(
					Candidate,
					TEXT("target_ref"),
					CandidateTargetRef,
					TargetIdentity,
					Pointer + TEXT("/value/candidate"),
					OutError)
				|| !TryRequiredString(
					Candidate,
					TEXT("target_revision"),
					CandidateTargetRevision,
					TargetIdentity,
					Pointer + TEXT("/value/candidate"),
					OutError))
			{
				return false;
			}
			FAnimationRigGraphCandidateQueryError QueryError;
			if (!IsCurrentAnimationRigGraphCandidateBinding(
					*TargetObject,
					CandidateTargetRef,
					CandidateTargetRevision,
					Binding.Name,
					Binding.Kind,
					Binding.SupportedOperations,
					Binding.InputContract,
					Binding.PlacementContract,
					Binding.RequiredContext,
					Binding.BindingRevision,
					QueryError))
			{
				return Fail(
					OutError,
					QueryError.Code.IsEmpty()
						? FString(TEXT(
							"animation_rig_graph_candidate_binding_invalid"))
						: QueryError.Code,
					TargetIdentity,
					Pointer + TEXT("/value/candidate/binding_revision"));
			}
			const TSharedPtr<FJsonObject>* Position = nullptr;
			double X = 0.0;
			double Y = 0.0;
			if (!Value->TryGetObjectField(TEXT("position"), Position)
				|| !Position || !Position->IsValid()
				|| !(*Position)->TryGetNumberField(TEXT("x"), X)
				|| !(*Position)->TryGetNumberField(TEXT("y"), Y)
				|| !FMath::IsFinite(X)
				|| !FMath::IsFinite(Y))
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_position_required"),
					TargetIdentity,
					Pointer + TEXT("/value/position"));
			}
			if (!TrySetExactString(
					Value,
					TEXT("script_struct_ref"),
					Binding.Name,
					TargetIdentity,
					Pointer + TEXT("/value"),
					OutError))
			{
				return false;
			}
			FString Method;
			if (!Value->TryGetStringField(TEXT("method"), Method))
			{
				Value->SetStringField(TEXT("method"), TEXT("Execute"));
			}
			else if (Method.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_method_invalid"),
					TargetIdentity,
					Pointer + TEXT("/value/method"));
			}
			Value->RemoveField(TEXT("candidate"));
			Edit->RemoveField(TEXT("semantic_id"));
		}
		OutError.Reset();
		return true;
	}
}
