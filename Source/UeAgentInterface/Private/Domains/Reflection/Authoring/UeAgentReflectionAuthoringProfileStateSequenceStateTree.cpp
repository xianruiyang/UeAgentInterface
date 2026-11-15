// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileStateSequenceStateTree.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileState.h"
#include "Logging/TokenizedMessage.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationNative.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationRequests.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditingSubsystem.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeAuthoringState.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeDocument.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UeAgentReflectionAuthoring::ProfileStateSequenceStateTree
{
	namespace
	{
		using namespace UE::UAI::DomainAdapters::TimelineAnimation;
		UE::UAI::Contract::FDiagnostic StructureRequired(const TSharedPtr<FJsonValue>& Value)
		{
			auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("sequence_structure_invalid"),
				TEXT("structure"), MakeShared<FJsonValueString>(TEXT("an object")), Value,
				TEXT("timeline_authoring"), TEXT("Sequence structure must be an object."));
			Error.SourceKind = TEXT("authored_value");
			Error.Source.InternalJsonPath = TEXT("/");
			return Error;
		}
		enum class EProfileStateKind : uint8
		{
			Unsupported,
			Sequence,
			StateTree,
		};

		EProfileStateKind GetKind(const FProfile& Profile)
		{
			if (Profile.TypeId.Equals(TEXT("sequence"), ESearchCase::IgnoreCase))
			{
				return EProfileStateKind::Sequence;
			}
			if (Profile.TypeId.Equals(TEXT("state_tree"), ESearchCase::IgnoreCase))
			{
				return EProfileStateKind::StateTree;
			}
			return EProfileStateKind::Unsupported;
		}

		bool FailUnsupported(const FProfile& Profile, UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("reflection_profile_state_surface_unsupported"),
				TEXT("This profile state owner supports only Sequence and StateTree assets."), TEXT("profile_state"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("profile_type");
			OutError.ActualValue = MakeShared<FJsonValueString>(Profile.TypeId);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("sequence or state_tree"));
			return false;
		}

		bool ReadSequence(
			UObject& Asset,
			const FString& SemanticAssetPath,
			TSharedPtr<FJsonValue>& OutValue,
			bool& bOutAuthoringVisible,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const ULevelSequence* Sequence = Cast<ULevelSequence>(&Asset);
			if (!Sequence)
			{
				OutError = ProfileState::TargetFailure(TEXT("sequence_asset_type_invalid"), Asset, *ULevelSequence::StaticClass());
				return false;
			}
			TSharedPtr<FJsonObject> Object;
			FUeAgentTimelineAnimationExecutionError Error;
			if (!TryReadLevelSequenceStructure(
					*Sequence, SemanticAssetPath, Object, Error))
			{
				if (!TryReadLevelSequencePersistentState(*Sequence, Object, Error))
				{
					OutError = Error;
					return false;
				}
				bOutAuthoringVisible = false;
			}
			else bOutAuthoringVisible = true;
			OutValue = MakeShared<FJsonValueObject>(Object);
			OutError.Reset();
			return true;
		}

		bool ApplySequence(
			UObject& Asset,
			const TSharedPtr<FJsonValue>& Value,
			const FString& SemanticAssetPath,
			UE::UAI::Contract::FDiagnostic& OutError, const bool bPreserveExistingStructure,
			UE::UAI::Authoring::Timeline::FMovieSceneCreationState* MovieSceneCreation)
		{
			ULevelSequence* Sequence = Cast<ULevelSequence>(&Asset);
			if (!Sequence)
			{
				OutError = ProfileState::TargetFailure(TEXT("sequence_asset_type_invalid"), Asset, *ULevelSequence::StaticClass());
				return false;
			}
			if (!Value.IsValid() || Value->Type != EJson::Object || !Value->AsObject().IsValid())
			{
				OutError = StructureRequired(Value);
				return false;
			}
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (Object.IsValid()
				&& Object->HasTypedField<EJson::String>(TEXT("state_kind")))
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("sequence_persistent_state_not_authorable"),
					TEXT("state_kind"), MakeShared<FJsonValueString>(TEXT("an authorable sequence structure without persistent state_kind")),
					Object->TryGetField(TEXT("state_kind")), TEXT("timeline_authoring"),
					TEXT("Persistent sequence snapshots are read-only; provide the authorable sequence structure."));
				OutError.SourceKind = TEXT("authored_value");
				OutError.Source.InternalJsonPath = TEXT("/state_kind");
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Object, TEXT("state_kind"));
				return false;
			}
			FTimelineAnimationStructure Structure;
			FUeAgentTimelineAnimationExecutionError Error;
			if (!TryParseTimelineAnimationStructure(Object, Structure, Error)
				|| !(bPreserveExistingStructure
					? TryUpdateLevelSequenceValues(*Sequence, Structure, SemanticAssetPath, Error, MovieSceneCreation)
					: TryApplyLevelSequenceStructure(*Sequence, Structure, SemanticAssetPath, Error)))
			{
				OutError = Error;
				return false;
			}
			Sequence->Modify();
			Sequence->MarkPackageDirty();
			OutError.Reset();
			return true;
		}

	}

	bool Supports(const FProfile& Profile)
	{
		return GetKind(Profile) != EProfileStateKind::Unsupported;
	}

	bool Validate(
		const FProfile& Profile,
		const TSharedPtr<FJsonValue>& Value,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		switch (GetKind(Profile))
		{
		case EProfileStateKind::Sequence:
		{
			if (!Value.IsValid() || Value->Type != EJson::Object || !Value->AsObject().IsValid())
			{
				OutError = StructureRequired(Value);
				return false;
			}
			FTimelineAnimationStructure Structure;
			FUeAgentTimelineAnimationExecutionError Error;
			if (TryParseTimelineAnimationStructure(Value->AsObject(), Structure, Error))
			{
				OutError.Reset();
				return true;
			}
			OutError = Error;
			return false;
		}
		case EProfileStateKind::StateTree:
		{
			return UeAgentStateTreeAuthoring::ValidateTreeDocument(Value, OutError);
		}
		default:
			return FailUnsupported(Profile, OutError);
		}
	}

	bool Read(
		const FProfile& Profile,
		UObject& Asset,
		const FString& SemanticAssetPath,
		TSharedPtr<FJsonValue>& OutValue,
		bool& bOutAuthoringVisible,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return Read(Profile, Asset, SemanticAssetPath, OutValue, bOutAuthoringVisible, OutError, nullptr);
	}

	bool Read(const FProfile& Profile, UObject& Asset, const FString& SemanticAssetPath, TSharedPtr<FJsonValue>& OutValue,
		bool& bOutAuthoringVisible, UE::UAI::Contract::FDiagnostic& OutError,
		TOptional<UeAgentAuthoringState::FStateSnapshot>* OutStructure)
	{
		OutValue.Reset();
		bOutAuthoringVisible = true;
		OutError.Reset();
		switch (GetKind(Profile))
		{
		case EProfileStateKind::Sequence:
			return ReadSequence(
				Asset, SemanticAssetPath, OutValue,
				bOutAuthoringVisible, OutError);
		case EProfileStateKind::StateTree:
		{
			UStateTree* Tree = Cast<UStateTree>(&Asset);
			if (!Tree) { OutError = ProfileState::TargetFailure(TEXT("state_tree_asset_type_invalid"), Asset, *UStateTree::StaticClass()); return false; }
			return UeAgentStateTreeAuthoring::ReadTreeDocument(*Tree, OutValue, OutError, OutStructure);
		}
		default:
			return FailUnsupported(Profile, OutError);
		}
	}

	bool Apply(
		const FProfile& Profile,
		UObject& Asset,
		const TSharedPtr<FJsonValue>& Value,
		const FString& SemanticAssetPath,
		UE::UAI::Contract::FDiagnostic& OutError, const bool bPreserveExistingStructure, UPackage* TextIdentityPackage,
		UE::UAI::Authoring::Timeline::FMovieSceneCreationState* MovieSceneCreation)
	{
		OutError.Reset();
		switch (GetKind(Profile))
		{
		case EProfileStateKind::Sequence:
			return ApplySequence(
				Asset, Value, SemanticAssetPath, OutError, bPreserveExistingStructure, MovieSceneCreation);
		case EProfileStateKind::StateTree:
		{
			UStateTree* Tree = Cast<UStateTree>(&Asset);
			if (!Tree) { OutError = ProfileState::TargetFailure(TEXT("state_tree_asset_type_invalid"), Asset, *UStateTree::StaticClass()); return false; }
			return UeAgentStateTreeAuthoring::ApplyTreeDocumentForAuthoring(*Tree, Value, OutError, TextIdentityPackage);
		}
		default:
			return FailUnsupported(Profile, OutError);
		}
	}

	bool CompileIfNeeded(
		const FProfile& Profile,
		UObject& Asset,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		switch (GetKind(Profile))
		{
		case EProfileStateKind::Sequence:
			return true;
		case EProfileStateKind::StateTree:
		{
			UStateTree* StateTree = Cast<UStateTree>(&Asset);
			if (!StateTree)
			{
				OutError = ProfileState::TargetFailure(TEXT("state_tree_asset_type_invalid"), Asset, *UStateTree::StaticClass());
				return false;
			}
#if WITH_EDITOR
			FStateTreeCompilerLog CompileLog;
			if (!UStateTreeEditingSubsystem::CompileStateTree(StateTree, CompileLog))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("state_tree_compile_failed"),
					TEXT("The native StateTree compiler failed."), TEXT("compile"));
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = Asset.GetPathName();
				OutError.PropertyPath = TEXT("compile_succeeded");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				for (const auto& Message : CompileLog.ToTokenizedMessages())
				{
					if (Message->GetSeverity() != EMessageSeverity::Error) continue;
					FString Reason = Message->ToText().ToString();
					if (Reason.IsEmpty()) Reason = TEXT("The StateTree compiler reported an error without a message.");
					auto Cause = UE::UAI::Contract::FDiagnostic::Failure(TEXT("state_tree_compile_failed"), MoveTemp(Reason), TEXT("compile"));
					Cause.SourceKind = TEXT("native_result");
					Cause.TargetPath = Asset.GetPathName();
					OutError.Diagnostics.Add(MoveTemp(Cause));
				}
				if (OutError.Diagnostics.IsEmpty())
					OutError.PublicMessage = TEXT("The native StateTree compiler failed without an error message.");
				return false;
			}
			StateTree->MarkPackageDirty();
			return true;
#else
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("state_tree_compile_unavailable"),
				TEXT("StateTree compilation requires an Editor build."), TEXT("compile"));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = Asset.GetPathName();
			OutError.PropertyPath = TEXT("editor_build");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
#endif
		}
		default:
			return FailUnsupported(Profile, OutError);
		}
	}
}
