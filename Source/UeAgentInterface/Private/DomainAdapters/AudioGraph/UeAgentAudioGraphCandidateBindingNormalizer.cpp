#include "DomainAdapters/AudioGraph/UeAgentAudioGraphCandidateBindingNormalizer.h"

#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "QueryOwners/AudioGraph/UeAgentAudioGraphCandidateQuery.h"
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
					TEXT("audio_graph_candidate_field_required"),
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
					TEXT("audio_graph_candidate_normalization_conflict"),
					TargetIdentity,
					Pointer + TEXT("/") + Field);
			}
			Object->SetStringField(Field, Expected);
			return true;
		}

		bool TryReadVersion(
			const TSharedPtr<FJsonObject>& Candidate,
			const TCHAR* Field,
			const bool bRequired,
			int32& OutVersion,
			const FString& TargetIdentity,
			const FString& Pointer,
			FWorkspaceError& OutError)
		{
			OutVersion = 0;
			if (!Candidate->HasField(Field))
			{
				return !bRequired || Fail(
					OutError,
					TEXT("audio_graph_candidate_version_required"),
					TargetIdentity,
					Pointer + TEXT("/") + Field);
			}
			double Number = 0.0;
			if (!Candidate->TryGetNumberField(Field, Number)
				|| !FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number))
				|| Number < 0.0
				|| Number > static_cast<double>(MAX_int32))
			{
				return Fail(
					OutError,
					TEXT("audio_graph_candidate_version_invalid"),
					TargetIdentity,
					Pointer + TEXT("/") + Field);
			}
			OutVersion = static_cast<int32>(FMath::RoundToDouble(Number));
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
					TEXT("audio_graph_candidate_normalization_conflict"),
					TargetIdentity,
					Pointer + TEXT("/") + Field);
			}
			Object->SetNumberField(Field, Expected);
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
				TEXT("audio_graph_native_params_invalid"),
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
				TEXT("audio_graph_candidate_nodes_invalid"),
				TargetIdentity,
				TEXT("/root/operation/params/nodes"));
		}

		UObject* const TargetObject = StaticLoadObject(
			UObject::StaticClass(),
			nullptr,
			*ObjectPathFromPackageIdentity(TargetIdentity));
		if (!TargetObject)
		{
			return Fail(
				OutError,
				TEXT("audio_graph_candidate_target_unresolved"),
				TargetIdentity,
				TEXT("/target"));
		}

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
					TEXT("audio_graph_candidate_node_invalid"),
					TargetIdentity,
					Pointer);
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
					TEXT("audio_graph_candidate_invalid"),
					TargetIdentity,
					Pointer + TEXT("/candidate"));
			}

			FString Intent;
			if (!Node->TryGetStringField(TEXT("intent"), Intent)
				|| Intent != TEXT("create"))
			{
				return Fail(
					OutError,
					TEXT("audio_graph_candidate_operation_unsupported"),
					TargetIdentity,
					Pointer + TEXT("/intent"));
			}
			const TSharedPtr<FJsonObject> Candidate = *CandidateField;
			FDomainHjsonCandidateBinding Binding;
			if (!TryReadCandidateBinding(
					Candidate,
					Pointer + TEXT("/candidate"),
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
					Pointer + TEXT("/candidate"),
					OutError)
				|| !TryRequiredString(
					Candidate,
					TEXT("target_revision"),
					CandidateTargetRevision,
					TargetIdentity,
					Pointer + TEXT("/candidate"),
					OutError))
			{
				return false;
			}
			const bool bMetaSound = Binding.Kind == TEXT("metasound_node");
			int32 MajorVersion = 0;
			int32 MinorVersion = 0;
			if (!TryReadVersion(
					Candidate,
					TEXT("major_version"),
					bMetaSound,
					MajorVersion,
					TargetIdentity,
					Pointer + TEXT("/candidate"),
					OutError)
				|| !TryReadVersion(
					Candidate,
					TEXT("minor_version"),
					bMetaSound,
					MinorVersion,
					TargetIdentity,
					Pointer + TEXT("/candidate"),
					OutError))
			{
				return false;
			}
			FAudioGraphCandidateQueryError QueryError;
			if (!IsCurrentAudioGraphCandidateBinding(
					*TargetObject,
					CandidateTargetRef,
					CandidateTargetRevision,
					Binding.Name,
					Binding.Kind,
					Binding.SupportedOperations,
					Binding.InputContract,
					Binding.PlacementContract,
					Binding.RequiredContext,
					MajorVersion,
					MinorVersion,
					Binding.BindingRevision,
					QueryError))
			{
				return Fail(
					OutError,
					QueryError.Code.IsEmpty()
						? FString(TEXT("audio_graph_candidate_binding_invalid"))
						: QueryError.Code,
					TargetIdentity,
					Pointer + TEXT("/candidate/binding_revision"));
			}

			FString NodeIdentity;
			const TCHAR* IdentityField = bMetaSound
				? TEXT("node_guid") : TEXT("object_path");
			if (!Node->TryGetStringField(IdentityField, NodeIdentity)
				|| NodeIdentity.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("audio_graph_candidate_context_required"),
					TargetIdentity,
					Pointer + TEXT("/") + IdentityField);
			}
			if (bMetaSound)
			{
				if (!TrySetExactString(
						Node,
						TEXT("class_name"),
						Binding.Name,
						TargetIdentity,
						Pointer,
						OutError)
					|| !TrySetExactNumber(
						Node,
						TEXT("major_version"),
						MajorVersion,
						TargetIdentity,
						Pointer,
						OutError)
					|| !TrySetExactNumber(
						Node,
						TEXT("minor_version"),
						MinorVersion,
						TargetIdentity,
						Pointer,
						OutError))
				{
					return false;
				}
			}
			else if (!TrySetExactString(
				Node,
				TEXT("native_class_ref"),
				Binding.Name,
				TargetIdentity,
				Pointer,
				OutError))
			{
				return false;
			}
			Node->RemoveField(TEXT("candidate"));
		}
		OutError.Reset();
		return true;
	}
}
