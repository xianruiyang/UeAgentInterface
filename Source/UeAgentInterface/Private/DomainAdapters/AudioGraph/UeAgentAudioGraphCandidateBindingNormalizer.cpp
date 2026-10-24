#include "DomainAdapters/AudioGraph/UeAgentAudioGraphCandidateBindingNormalizer.h"
#include "Core/ContentDiscovery/UeAgentReadOnlyCandidateBatch.h"

#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MetasoundSource.h"
#include "QueryOwners/AudioGraph/UeAgentAudioGraphCandidateQuery.h"
#include "Sound/SoundCue.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::AudioGraph
{
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;
	using namespace QueryOwners::AudioGraph;

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
					TEXT("audio_graph_candidate_normalization_conflict"), TEXT("The asserted candidate field differs from its resolved value."),
					TargetIdentity,
					Pointer + TEXT("/") + Field, Object->TryGetField(Field), MakeShared<FJsonValueString>(Expected));
			}
			Object->SetStringField(Field, Expected);
			return true;
		}

		bool TrySetExactNumber(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			const int32 Expected,
			const FString& TargetIdentity,
			const FString& Pointer,
			FWorkspaceError& OutError)
		{
			double Existing = 0.0;
			if (Object->HasField(Field)
				&& (!Object->TryGetNumberField(Field, Existing)
					|| !FMath::IsNearlyEqual(Existing, Expected)))
			{
				return Fail(
					OutError,
					TEXT("audio_graph_candidate_normalization_conflict"), TEXT("The asserted candidate field differs from its resolved value."),
					TargetIdentity,
					Pointer + TEXT("/") + Field, Object->TryGetField(Field), MakeShared<FJsonValueNumber>(Expected));
			}
			Object->SetNumberField(Field, Expected);
			return true;
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

	bool TryNormalizeAudioGraphCandidateBindings(
		const FString& TargetIdentity,
		const TSharedPtr<FJsonObject>& NativeParams,
		FWorkspaceError& OutError)
	{
		if (!NativeParams.IsValid())
		{
			return Fail(
				OutError,
				TEXT("audio_graph_native_params_invalid"), TEXT("The audio normalization request has no parameter object."),
				TargetIdentity,
				TEXT("/root/operation/params"));
		}
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!NativeParams->HasField(TEXT("nodes")))
		{
			return true;
		}
		if (!NativeParams->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
		{
			return Fail(
				OutError,
				TEXT("audio_graph_candidate_nodes_invalid"), TEXT("Audio nodes must be an array."),
				TargetIdentity,
				TEXT("/root/operation/params/nodes"), NativeParams->TryGetField(TEXT("nodes")), MakeShared<FJsonValueString>(TEXT("array")));
		}

		UObject* const TargetObject = StaticLoadObject(
			UObject::StaticClass(),
			nullptr,
			*ObjectPathFromPackageIdentity(TargetIdentity));
		if (!TargetObject)
		{
			return Fail(
				OutError,
				TEXT("audio_graph_candidate_target_unresolved"), TEXT("The audio candidate target could not be loaded."),
				TargetIdentity,
				TEXT("/target"));
		}

		ContentDiscovery::FReadOnlyCandidateBatch CandidateBatch;
		for (int32 Index = 0; Index < Nodes->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Node = (*Nodes)[Index].IsValid()
				? (*Nodes)[Index]->AsObject() : nullptr;
			const FString Pointer = FString::Printf(
				TEXT("/root/operation/params/nodes/%d"), Index);
			if (!Node.IsValid())
			{
				return Fail(
					OutError,
					TEXT("audio_graph_candidate_node_invalid"), TEXT("Each audio node must be an object."),
					TargetIdentity,
					Pointer, (*Nodes)[Index], MakeShared<FJsonValueString>(TEXT("object")));
			}
			const TSharedPtr<FJsonObject>* CandidateField = nullptr;
			if (!Node->HasField(TEXT("candidate")))
			{
				continue;
			}
			if (!Node->TryGetObjectField(TEXT("candidate"), CandidateField)
				|| !CandidateField || !CandidateField->IsValid())
			{
				return Fail(
					OutError,
					TEXT("audio_graph_candidate_invalid"), TEXT("The audio candidate selector must be an object."),
					TargetIdentity,
					Pointer + TEXT("/candidate"), Node->TryGetField(TEXT("candidate")), MakeShared<FJsonValueString>(TEXT("candidate selector object")));
			}

			FString Intent;
			if (!Node->TryGetStringField(TEXT("intent"), Intent)
				|| Intent != TEXT("create"))
			{
				return Fail(
					OutError,
					TEXT("audio_graph_candidate_operation_unsupported"), TEXT("An audio candidate can be used only with the create intent."),
					TargetIdentity,
					Pointer + TEXT("/intent"), Node->TryGetField(TEXT("intent")), MakeShared<FJsonValueString>(TEXT("create")));
			}
			const TSharedPtr<FJsonObject> Candidate = *CandidateField;
			const FString CandidatePointer = Pointer + TEXT("/candidate");
			if (TargetObject->GetClass() == USoundCue::StaticClass())
			{
				FString UnknownField;
				if (!HasOnlyCandidateSelectorFields(Candidate, UnknownField))
					return Fail(OutError, TEXT("audio_graph_candidate_selector_field_unsupported"), TEXT("The candidate selector contains an unsupported field."), TargetIdentity, CandidatePointer + TEXT("/") + EscapeJsonPointerSegment(UnknownField), MakeShared<FJsonValueString>(UnknownField), MakeShared<FJsonValueString>(TEXT("name or select")));
				FDomainHjsonCandidateSelector Selector;
				if (!TryReadCandidateSelector(Candidate, CandidatePointer, Selector, OutError)) return false;
				FAudioGraphCandidateQueryError QueryError;
				FAudioGraphCandidate Selected;
				if (!TryResolveSoundCueCandidate(*TargetObject, Selector.Name, Selected, QueryError, &CandidateBatch, Selector.Select))
				{
					OutError.SetFailure(QueryError, TargetIdentity, CandidatePointer);
					return false;
				}
				const FString NativeClassName = Selected.NativeClassName.IsEmpty() ? Selected.Name : Selected.NativeClassName;
				if (!TrySetExactString(Node, TEXT("native_class_ref"), NativeClassName, TargetIdentity, Pointer, OutError)) return false;
				Node->RemoveField(TEXT("candidate"));
				continue;
			}
			if (TargetObject->GetClass() == UMetaSoundSource::StaticClass())
			{
				FString UnknownField;
				if (!HasOnlyCandidateSelectorFields(Candidate, UnknownField))
				{
					return Fail(
						OutError,
						TEXT("audio_graph_candidate_selector_field_unsupported"), TEXT("The candidate selector contains an unsupported field."),
						TargetIdentity,
						CandidatePointer + TEXT("/") + EscapeJsonPointerSegment(UnknownField), MakeShared<FJsonValueString>(UnknownField), MakeShared<FJsonValueString>(TEXT("name or select")));
				}
				FDomainHjsonCandidateSelector Selector;
				if (!TryReadCandidateSelector(
					Candidate, CandidatePointer, Selector, OutError))
				{
					return false;
				}
				FAudioGraphCandidate Resolved;
				FAudioGraphCandidateQueryError QueryError;
				if (!TryResolveAudioGraphCandidate(
						*TargetObject,
						Selector.Name,
						Resolved,
						QueryError, &CandidateBatch, Selector.Select))
				{
					OutError.SetFailure(QueryError, TargetIdentity, CandidatePointer);
					return false;
				}
				if (!Resolved.bSupported
					|| !Resolved.SupportedOperations.Contains(TEXT("create")))
				{
					return Fail(
						OutError,
						TEXT("audio_graph_candidate_action_kind_unsupported"), Resolved.SupportReason.IsEmpty()
							? TEXT("The selected audio candidate does not support creation in this context.") : Resolved.SupportReason,
						TargetIdentity,
						CandidatePointer, MakeShared<FJsonValueString>(Selector.Name), MakeShared<FJsonValueString>(TEXT("candidate supporting create")));
				}
				if (!TrySetExactString(
						Node,
						TEXT("class_name"),
						Resolved.NativeClassName.IsEmpty() ? Resolved.Name : Resolved.NativeClassName,
						TargetIdentity,
						Pointer,
						OutError)
					|| !TrySetExactNumber(
						Node,
						TEXT("major_version"),
						Resolved.MajorVersion,
						TargetIdentity,
						Pointer,
						OutError)
					|| !TrySetExactNumber(
						Node,
						TEXT("minor_version"),
						Resolved.MinorVersion,
						TargetIdentity,
						Pointer,
						OutError))
				{
					return false;
				}
				if (!Node->HasField(TEXT("node_guid")))
				{
					Node->SetStringField(
						TEXT("node_guid"),
						FGuid::NewGuid().ToString(
							EGuidFormats::DigitsWithHyphensLower));
				}
				Node->RemoveField(TEXT("candidate"));
				continue;
			}

			return Fail(
				OutError,
				TEXT("audio_graph_candidate_target_type_unsupported"), TEXT("Candidate normalization supports SoundCue and MetaSoundSource targets."),
				TargetIdentity,
				CandidatePointer, MakeShared<FJsonValueString>(TargetObject->GetClass()->GetPathName()), MakeShared<FJsonValueString>(TEXT("SoundCue or MetaSoundSource")));
		}
		OutError.Reset();
		return true;
	}
}
