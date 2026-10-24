#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphCandidateBindingNormalizer.h"
#include "Core/ContentDiscovery/UeAgentReadOnlyCandidateBatch.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

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
			const FString& Reason,
			const FString& TargetIdentity,
			FString Pointer,
            const TSharedPtr<FJsonValue>& Actual = nullptr,
            const TSharedPtr<FJsonValue>& Expected = nullptr)
		{
			auto Detail = UE::UAI::Contract::FDiagnostic::Failure(MoveTemp(Code), Reason, TEXT("candidate_normalization"));
            Detail.TargetPath = TargetIdentity;
            Detail.Source.InternalJsonPath = MoveTemp(Pointer);
            Detail.ActualValue = Actual;
            Detail.ExpectedValue = Expected;
            if (Expected.IsValid()) Detail.ActualValuePresent = Actual.IsValid();
            OutError.SetFailure(MoveTemp(Detail));
			return false;
		}

		bool TrySetExactString(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			const FString& Expected,
			const bool bPublicValue,
			const FString& TargetIdentity,
			const FString& Pointer,
			FWorkspaceError& OutError)
		{
			FString Existing;
			if (Object->HasField(Field)
				&& (!Object->TryGetStringField(Field, Existing)
					|| Existing != Expected))
			{
				auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_rig_graph_candidate_normalization_conflict"),
                    TEXT("The asserted candidate field differs from its resolved value."), TEXT("candidate_normalization"));
                Detail.TargetPath = TargetIdentity;
                Detail.Source.InternalJsonPath = Pointer + TEXT("/") + Field;
                Detail.ActualEvidence = Object->TryGetField(Field);
                Detail.ExpectedEvidence = MakeShared<FJsonValueString>(Expected);
                if (bPublicValue) { Detail.ActualValue = Detail.ActualEvidence; Detail.ExpectedValue = Detail.ExpectedEvidence; }
                OutError.SetFailure(MoveTemp(Detail));
                return false;
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
			const TSharedPtr<FJsonObject>& Candidate, FString& OutUnknownField)
		{
			if (!Candidate.IsValid())
			{
				return false;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field
				: Candidate->Values)
			{
				if (Field.Key != TEXT("name") && Field.Key != TEXT("select"))
				{
					OutUnknownField = Field.Key;
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
				TEXT("animation_rig_graph_native_params_invalid"), TEXT("The rig normalization request has no parameter object."),
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
				TEXT("animation_rig_graph_candidate_edits_invalid"), TEXT("Rig edits must be an array."),
				TargetIdentity,
				TEXT("/root/operation/params/edits"), NativeParams->TryGetField(TEXT("edits")), MakeShared<FJsonValueString>(TEXT("array")));
		}
		UControlRigBlueprint* const Target = Cast<UControlRigBlueprint>(
			StaticLoadObject(
				UObject::StaticClass(),
				nullptr,
				*ObjectPathFromPackageIdentity(TargetIdentity)));

		ContentDiscovery::FReadOnlyCandidateBatch CandidateBatch;
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
					TEXT("animation_rig_graph_candidate_edit_invalid"), TEXT("Each rig edit must be an object."),
					TargetIdentity,
					Pointer, (*Edits)[Index], MakeShared<FJsonValueString>(TEXT("object")));
			}
			FString SemanticId;
			FString NativeElementKind;
			if (!Edit->TryGetStringField(TEXT("semantic_id"), SemanticId)
				|| !TryResolveNativeElementKind(
					SemanticId, NativeElementKind)
				|| !TrySetExactString(
					Edit,
					TEXT("identity"),
					SemanticId, false,
					TargetIdentity,
					Pointer,
					OutError)
				|| !TrySetExactString(
					Edit,
					TEXT("element_kind"),
					NativeElementKind, true,
					TargetIdentity,
					Pointer,
					OutError))
			{
				if (OutError.GetCode().IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("animation_rig_graph_edit_identity_invalid"), TEXT("The rig edit has no recognized node or control identity."),
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
					TEXT("animation_rig_graph_candidate_context_invalid"), TEXT("The rig edit requires a non-empty intent string."),
					TargetIdentity,
					Pointer + TEXT("/intent"), Edit->TryGetField(TEXT("intent")), MakeShared<FJsonValueString>(TEXT("non-empty intent string")));
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
					TEXT("animation_rig_graph_candidate_target_unresolved"), TEXT("The candidate target could not be loaded as a Control Rig Blueprint."),
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
					TEXT("animation_rig_graph_candidate_selector_required"), TEXT("Creating or replacing a rig node requires a candidate selector object inside value."),
					TargetIdentity,
					Pointer + TEXT("/value/candidate"));
			}
			const TSharedPtr<FJsonObject> Value = *ValueField;
			const TSharedPtr<FJsonObject> Candidate = *CandidateField;
			const FString CandidatePointer =
				Pointer + TEXT("/value/candidate");
			FString UnknownField;
			if (!HasOnlyCandidateSelectorFields(Candidate, UnknownField))
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_candidate_selector_field_unsupported"), TEXT("The candidate selector contains an unsupported field."),
					TargetIdentity,
					CandidatePointer + TEXT("/") + EscapeJsonPointerSegment(UnknownField), MakeShared<FJsonValueString>(UnknownField), MakeShared<FJsonValueString>(TEXT("name or select")));
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
					TEXT("animation_rig_graph_candidate_position_required"), TEXT("The rig node position requires finite numeric x and y coordinates."),
					TargetIdentity,
					Pointer + TEXT("/value/position"), Value->TryGetField(TEXT("position")), MakeShared<FJsonValueString>(TEXT("object with finite numeric x and y")));
			}

			FAnimationRigGraphCandidate Resolved;
			FAnimationRigGraphCandidateQueryError QueryError;
			if (!TryResolveAnimationRigGraphCandidate(
					*Target,
					Selector.Name,
					Resolved,
					QueryError, &CandidateBatch, Selector.Select))
			{
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(QueryError, CandidatePointer);
				if (QueryError.TargetPath.IsEmpty()) QueryError.TargetPath = TargetIdentity;
				OutError.SetFailure(MoveTemp(QueryError));
				return false;
			}
			if (!Resolved.bSupported)
			{
				auto Diagnostic = Resolved.SupportDiagnostic;
				if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = TargetIdentity;
				OutError.SetFailure(MoveTemp(Diagnostic));
				return false;
			}
			Candidate->SetStringField(TEXT("name"), Resolved.Name);
			Edit->RemoveField(TEXT("semantic_id"));
		}
		OutError.Reset();
		return true;
	}
}
