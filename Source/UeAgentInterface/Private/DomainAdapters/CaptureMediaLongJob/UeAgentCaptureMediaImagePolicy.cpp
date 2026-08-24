#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaImagePolicy.h"

#include <initializer_list>

#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "LegacyMechanics/UeAgentLegacyMechanics.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	using FMechanics = bool (FUeAgentLegacyMechanics::*)(
		const FUeAgentRequestContext&,
		TSharedPtr<FJsonObject>&,
		FString&) const;
	using FArtifactMechanics = bool (FUeAgentLegacyMechanics::*)(
		const FUeAgentRequestContext&,
		TSharedPtr<FJsonObject>&,
		TArray<TSharedPtr<FJsonValue>>&,
		FString&) const;

	namespace
	{
		struct FPolicyRoute final
		{
			FUeAgentCaptureMediaImagePolicyDescriptor Descriptor;
			FString InternalCommandId;
			FMechanics Mechanics = nullptr;
			FArtifactMechanics ArtifactMechanics = nullptr;
		};

		FString TransformCandidatesParameterSchema()
		{
			return TEXT(
				"{\"$defs\":{\"rotator\":{\"additionalProperties\":false,"
				"\"properties\":{\"pitch\":{\"type\":\"number\"},"
				"\"roll\":{\"type\":\"number\"},\"yaw\":{\"type\":\"number\"}},"
				"\"required\":[\"pitch\",\"yaw\",\"roll\"],\"type\":\"object\"},"
				"\"vector3\":{\"additionalProperties\":false,\"properties\":{"
				"\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},"
				"\"z\":{\"type\":\"number\"}},\"required\":[\"x\",\"y\",\"z\"],"
				"\"type\":\"object\"}},\"additionalProperties\":false,\"properties\":{"
				"\"candidates\":{\"items\":{\"additionalProperties\":false,"
				"\"properties\":{\"label\":{\"minLength\":1,\"type\":\"string\"},"
				"\"relative_to_original\":{\"type\":\"boolean\"},\"transform\":{"
				"\"additionalProperties\":false,\"minProperties\":1,\"properties\":{"
				"\"location\":{\"$ref\":\"#/$defs/vector3\"},\"rotation\":{"
				"\"$ref\":\"#/$defs/rotator\"},\"scale\":{\"$ref\":\"#/$defs/vector3\"}},"
				"\"type\":\"object\"}},\"required\":[\"transform\"],\"type\":\"object\"},"
				"\"maxItems\":15,\"minItems\":1,\"type\":\"array\"},"
				"\"format\":{\"enum\":[\"png\",\"jpg\"],\"type\":\"string\"},"
				"\"frame_sheet_padding\":{\"maximum\":256,\"minimum\":0,\"type\":\"integer\"},"
				"\"max_size\":{\"maximum\":8192,\"minimum\":64,\"type\":\"integer\"},"
				"\"output_file\":{\"minLength\":1,\"type\":\"string\"},"
				"\"quality\":{\"maximum\":100,\"minimum\":1,\"type\":\"integer\"},"
				"\"warmup_frames\":{\"maximum\":8,\"minimum\":0,\"type\":\"integer\"}},"
				"\"required\":[\"candidates\"],\"type\":\"object\"}");
		}

		const TArray<FPolicyRoute>& Routes()
		{
			static const TArray<FPolicyRoute> Value = {
				{{TEXT("policy:image-capture:anim-blueprint/v1"),
					TEXT("mechanics:capture:anim-blueprint-editor/v1")},
					TEXT("anim_blueprint_screenshot"),
					&FUeAgentLegacyMechanics::CmdBlueprintScreenshot},
				{{TEXT("policy:image-capture:anim-sequence/v1"),
					TEXT("mechanics:capture:anim-sequence-preview/v1")},
					TEXT("anim_sequence_screenshot"),
					&FUeAgentLegacyMechanics::CmdAnimSequenceScreenshot},
				{{TEXT("policy:image-capture:behavior-tree/v1"),
					TEXT("mechanics:capture:behavior-tree-widget/v1")},
					TEXT("behavior_tree_screenshot"),
					&FUeAgentLegacyMechanics::CmdBehaviorTreeScreenshot},
				{{TEXT("policy:image-capture:blueprint/v1"),
					TEXT("mechanics:capture:blueprint-editor/v1")},
					TEXT("blueprint_screenshot"),
					&FUeAgentLegacyMechanics::CmdBlueprintScreenshot},
				{{TEXT("policy:image-capture:control-rig/v1"),
					TEXT("mechanics:capture:control-rig-editor/v1")},
					TEXT("control_rig_screenshot"),
					&FUeAgentLegacyMechanics::CmdControlRigScreenshot},
				{{TEXT("policy:image-capture:level-sequence/v1"),
					TEXT("mechanics:capture:level-sequence-evaluation/v1")},
					TEXT("sequence_screenshot"),
					&FUeAgentLegacyMechanics::CmdSequenceScreenshot},
				{{TEXT("policy:image-capture:metasound/v1"),
					TEXT("mechanics:capture:metasound-graph-widget/v1")},
					TEXT("metasound_screenshot"),
					&FUeAgentLegacyMechanics::CmdMetaSoundScreenshot},
				{{TEXT("policy:image-capture:niagara/v1"),
					TEXT("mechanics:capture:niagara-preview/v1")},
					TEXT("niagara_screenshot"),
					&FUeAgentLegacyMechanics::CmdNiagaraScreenshot},
				{{TEXT("policy:image-capture:pcg-viewport/v1"),
					TEXT("mechanics:capture:pcg-framed-viewport/v1")},
					TEXT("pcg_screenshot"),
					&FUeAgentLegacyMechanics::CmdPCGScreenshot},
				{{TEXT("policy:image-capture:sound-cue/v1"),
					TEXT("mechanics:capture:sound-cue-graph-widget/v1")},
					TEXT("sound_cue_screenshot"),
					&FUeAgentLegacyMechanics::CmdSoundCueScreenshot},
				{{TEXT("policy:image-capture:state-tree/v1"),
					TEXT("mechanics:capture:state-tree-widget/v1")},
					TEXT("state_tree_screenshot"),
					&FUeAgentLegacyMechanics::CmdStateTreeScreenshot},
				{{TEXT("policy:image-capture:texture/v1"),
					TEXT("mechanics:capture:texture-source/v1")},
					TEXT("texture_screenshot"),
					&FUeAgentLegacyMechanics::CmdTextureScreenshot},
				{{TEXT("policy:image-capture:transform-candidates/v1"),
					TEXT("mechanics:capture:transform-candidates-preview/v1"),
					TransformCandidatesParameterSchema(),
					TEXT("Use subject_ref capture-subject:actor:<exact Actor object path> and provide 1-15 labeled candidates. The policy captures the original plus each candidate from the current Level Viewport into one frame sheet, restores Actor transform, package dirty state, Actor selection, and viewport realtime state, and does not persist or choose a candidate."),
					TEXT("Poll operation.status until terminal. Success publishes one frame-sheet artifact and guarantees the preview state was restored; inspect the sheet, then submit the chosen transform separately and release the operation."),
					TEXT("image_execute(client, {subject_ref, policy_id='policy:image-capture:transform-candidates/v1', parameters={'candidates': [...]}})")},
					TEXT("probe_actor_transform_candidates"),
					nullptr,
					&FUeAgentLegacyMechanics::CmdProbeActorTransformCandidates},
				{{TEXT("policy:image-capture:umg/v1"),
					TEXT("mechanics:capture:umg-offscreen-widget/v1")},
					TEXT("umg_screenshot"),
					&FUeAgentLegacyMechanics::CmdUmgScreenshot},
				{{TEXT("policy:image-capture:viewport-buffer/v1"),
					TEXT("mechanics:capture:level-viewport-buffer/v1")},
					TEXT("screenshot_viewport_buffer"),
					&FUeAgentLegacyMechanics::CmdScreenshotViewportBuffer},
				{{TEXT("policy:image-capture:viewport/v1"),
					TEXT("mechanics:capture:level-viewport-visualization/v1")},
					TEXT("screenshot_viewport"),
					&FUeAgentLegacyMechanics::CmdScreenshotViewport},
			};
			return Value;
		}

		const FPolicyRoute* FindRoute(const FString& PolicyId)
		{
			return Routes().FindByPredicate(
				[&PolicyId](const FPolicyRoute& Route)
				{
					return Route.Descriptor.GetPolicyId() == PolicyId;
				});
		}

		bool TryProjectParameters(
			const FPolicyRoute& Route,
			const FUeAgentCaptureMediaLongJobTypedRequest& Request,
			const FString& PreferredOutputPath,
			TSharedPtr<FJsonObject>& OutParameters,
			FString& OutRequestedFinalPath,
			FString& OutFormat,
			FString& OutError)
		{
			OutParameters.Reset();
			TSharedPtr<FJsonValue> JsonValue;
			if (!StateSemantic::CanonicalJson::TryToJsonValue(
					Request.GetParameters().Get(), JsonValue, OutError)
				|| !JsonValue.IsValid()
				|| JsonValue->Type != EJson::Object)
			{
				OutError = TEXT("capture_image_parameters_invalid:") + OutError;
				return false;
			}
			OutParameters = MakeShared<FJsonObject>();
			OutParameters->Values = JsonValue->AsObject()->Values;
			FString RequestedOutput;
			if (OutParameters->TryGetStringField(
					TEXT("output_file"), RequestedOutput)
				&& !RequestedOutput.TrimStartAndEnd().IsEmpty())
			{
				OutRequestedFinalPath = UeAgentJsonDiagnostics::
					ResolveProjectRelativeFilePath(RequestedOutput);
			}
			const FString& Subject = Request.GetSubjectRef();
			if (Subject.StartsWith(TEXT("capture-subject:asset:"))
				&& !OutParameters->HasField(TEXT("asset_path")))
			{
				OutParameters->SetStringField(
					TEXT("asset_path"), Subject.Mid(22));
			}
			else if (Subject.StartsWith(TEXT("capture-subject:actor:")))
			{
				const FString ActorRef = Subject.Mid(22);
				if (Route.Descriptor.GetPolicyId()
					== TEXT("policy:image-capture:transform-candidates/v1"))
				{
					OutParameters->SetStringField(TEXT("id"), ActorRef);
					OutParameters->SetBoolField(TEXT("create_frame_sheet"), true);
					OutParameters->SetBoolField(TEXT("write_individual_frames"), false);
					OutParameters->SetBoolField(TEXT("tick_editor_world"), false);
					OutParameters->SetNumberField(TEXT("editor_world_tick_frames"), 0);
				}
				else if (!OutParameters->HasField(TEXT("actor")))
				{
					OutParameters->SetStringField(TEXT("actor"), ActorRef);
				}
			}
			OutFormat = TEXT("png");
			OutParameters->TryGetStringField(TEXT("format"), OutFormat);
			OutFormat.TrimStartAndEndInline();
			OutFormat.ToLowerInline();
			if (OutFormat == TEXT("jpeg"))
			{
				OutFormat = TEXT("jpg");
			}
			if (OutFormat != TEXT("png") && OutFormat != TEXT("jpg"))
			{
				OutError = TEXT("capture_image_format_unsupported:") + OutFormat;
				return false;
			}
			if (!OutRequestedFinalPath.IsEmpty())
			{
				FString Extension = FPaths::GetExtension(
					OutRequestedFinalPath).ToLower();
				if (Extension == TEXT("jpeg"))
				{
					Extension = TEXT("jpg");
				}
				if (Extension.IsEmpty())
				{
					OutRequestedFinalPath += TEXT(".") + OutFormat;
				}
				else if (Extension != OutFormat)
				{
					OutError = TEXT("capture_image_output_extension_mismatch:")
						+ Extension + TEXT(":") + OutFormat;
					return false;
				}
			}
			OutParameters->SetStringField(TEXT("format"), OutFormat);
			OutParameters->SetStringField(
				TEXT("output_file"),
				FPaths::ChangeExtension(PreferredOutputPath, OutFormat));
			if (Route.Descriptor.GetPolicyId()
				== TEXT("policy:image-capture:transform-candidates/v1"))
			{
				if (!TryNormalizeTransformCandidateParameters(
						OutParameters, OutError))
				{
					return false;
				}
				OutParameters->SetStringField(
					TEXT("frame_sheet_output_file"),
					FPaths::ChangeExtension(PreferredOutputPath, OutFormat));
			}
			return true;
		}

		bool TryResolveProducedPath(
			const TSharedPtr<FJsonObject>& Result,
			const TSharedPtr<FJsonObject>& Parameters,
			FString& OutPath)
		{
			OutPath.Reset();
			if (Result.IsValid())
			{
				for (const TCHAR* Field
					: {TEXT("frame_sheet_file_path"), TEXT("file_path"),
						TEXT("output_file"), TEXT("path")})
				{
					if (Result->TryGetStringField(Field, OutPath)
						&& FPaths::FileExists(OutPath))
					{
						return true;
					}
				}
			}
			return Parameters.IsValid()
				&& Parameters->TryGetStringField(TEXT("output_file"), OutPath)
				&& FPaths::FileExists(OutPath);
		}
	}

	FUeAgentCaptureMediaImagePolicyDescriptor::
	FUeAgentCaptureMediaImagePolicyDescriptor(
		FString InPolicyId,
		FString InMechanicsId,
		FString InParameterSchemaJson,
		FString InParameterGuidance,
		FString InResultGuidance,
		FString InMinimumExample)
		: PolicyId(MoveTemp(InPolicyId))
		, MechanicsId(MoveTemp(InMechanicsId))
		, ParameterSchemaJson(MoveTemp(InParameterSchemaJson))
		, ParameterGuidance(MoveTemp(InParameterGuidance))
		, ResultGuidance(MoveTemp(InResultGuidance))
		, MinimumExample(MoveTemp(InMinimumExample))
	{
	}

	const FString&
	FUeAgentCaptureMediaImagePolicyDescriptor::GetPolicyId() const
	{
		return PolicyId;
	}

	const FString&
	FUeAgentCaptureMediaImagePolicyDescriptor::GetMechanicsId() const
	{
		return MechanicsId;
	}

	const FString& FUeAgentCaptureMediaImagePolicyDescriptor::
	GetParameterSchemaJson() const
	{
		return ParameterSchemaJson;
	}

	const FString& FUeAgentCaptureMediaImagePolicyDescriptor::
	GetParameterGuidance() const
	{
		return ParameterGuidance;
	}

	const FString& FUeAgentCaptureMediaImagePolicyDescriptor::
	GetResultGuidance() const
	{
		return ResultGuidance;
	}

	const FString& FUeAgentCaptureMediaImagePolicyDescriptor::
	GetMinimumExample() const
	{
		return MinimumExample;
	}

	bool FUeAgentCaptureMediaImagePolicyDescriptor::HasDiscoveryContract() const
	{
		return !ParameterSchemaJson.IsEmpty()
			&& !ParameterGuidance.IsEmpty()
			&& !ResultGuidance.IsEmpty()
			&& !MinimumExample.IsEmpty();
	}

	bool FUeAgentCaptureMediaImagePolicyDescriptor::IsValid() const
	{
		const bool bHasAnyDiscoveryField = !ParameterSchemaJson.IsEmpty()
			|| !ParameterGuidance.IsEmpty()
			|| !ResultGuidance.IsEmpty()
			|| !MinimumExample.IsEmpty();
		return PolicyId.StartsWith(TEXT("policy:image-capture:"))
			&& MechanicsId.StartsWith(TEXT("mechanics:capture:"))
			&& (!bHasAnyDiscoveryField || HasDiscoveryContract());
	}

	const TArray<FUeAgentCaptureMediaImagePolicyDescriptor>&
	GetCaptureMediaImagePolicies()
	{
		static const TArray<FUeAgentCaptureMediaImagePolicyDescriptor> Policies = []
		{
			TArray<FUeAgentCaptureMediaImagePolicyDescriptor> Value;
			for (const FPolicyRoute& Route : Routes())
			{
				Value.Add(Route.Descriptor);
			}
			Value.Sort([](const auto& A, const auto& B)
			{
				return A.GetPolicyId() < B.GetPolicyId();
			});
			return Value;
		}();
		return Policies;
	}

	const FUeAgentCaptureMediaImagePolicyDescriptor*
	FindCaptureMediaImagePolicy(const FString& PolicyId)
	{
		return GetCaptureMediaImagePolicies().FindByPredicate(
			[&PolicyId](const FUeAgentCaptureMediaImagePolicyDescriptor& Policy)
			{
				return Policy.GetPolicyId() == PolicyId;
			});
	}

	bool TryNormalizeTransformCandidateParameters(
		const TSharedPtr<FJsonObject>& Parameters,
		FString& OutError)
	{
		OutError.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
		if (!Parameters.IsValid()
			|| !Parameters->TryGetArrayField(TEXT("candidates"), Candidates)
			|| !Candidates)
		{
			OutError = TEXT("capture_transform_candidates_missing_candidates");
			return false;
		}
		for (const TSharedPtr<FJsonValue>& CandidateValue : *Candidates)
		{
			const TSharedPtr<FJsonObject>* Candidate = nullptr;
			if (!CandidateValue.IsValid()
				|| !CandidateValue->TryGetObject(Candidate)
				|| !Candidate || !Candidate->IsValid())
			{
				OutError = TEXT("capture_transform_candidate_invalid");
				return false;
			}
			TSharedPtr<FJsonObject> Transform = *Candidate;
			const TSharedPtr<FJsonObject>* Nested = nullptr;
			if ((*Candidate)->TryGetObjectField(TEXT("transform"), Nested)
				&& Nested && Nested->IsValid())
			{
				Transform = *Nested;
			}
			auto NormalizeField = [&Transform, &OutError](
				const TCHAR* Field,
				std::initializer_list<const TCHAR*> ComponentNames)
			{
				const TSharedPtr<FJsonObject>* Named = nullptr;
				if (!Transform->TryGetObjectField(Field, Named)
					|| !Named || !Named->IsValid()
					|| (*Named)->HasField(TEXT("components")))
				{
					return true;
				}
				TArray<TSharedPtr<FJsonValue>> Components;
				for (const TCHAR* ComponentName : ComponentNames)
				{
					double Component = 0.0;
					if (!(*Named)->TryGetNumberField(ComponentName, Component))
					{
						OutError = TEXT("capture_transform_candidate_component_missing:")
							+ FString(Field) + TEXT(":") + ComponentName;
						return false;
					}
					Components.Add(MakeShared<FJsonValueNumber>(Component));
				}
				const TSharedRef<FJsonObject> Canonical = MakeShared<FJsonObject>();
				Canonical->SetArrayField(TEXT("components"), MoveTemp(Components));
				Transform->SetObjectField(Field, Canonical);
				return true;
			};
			if (!NormalizeField(TEXT("location"), {TEXT("x"), TEXT("y"), TEXT("z")})
				|| !NormalizeField(
					TEXT("rotation"), {TEXT("pitch"), TEXT("yaw"), TEXT("roll")})
				|| !NormalizeField(TEXT("scale"), {TEXT("x"), TEXT("y"), TEXT("z")}))
			{
				return false;
			}
		}
		return true;
	}

	bool TryExecuteCaptureMediaImagePolicy(
		const FUeAgentCaptureMediaLongJobTypedRequest& Request,
		const FString& PreferredOutputPath,
		FString& OutProducedPath,
		FString& OutRequestedFinalPath,
		FString& OutFormat,
		FString& OutError)
	{
		OutProducedPath.Reset();
		OutRequestedFinalPath.Reset();
		OutFormat.Reset();
		OutError.Reset();
		if (!IsInGameThread())
		{
			OutError = TEXT("capture_image_game_thread_required");
			return false;
		}
		const FPolicyRoute* Route = FindRoute(Request.GetPolicyId());
		if (!Route
			|| !Route->Descriptor.IsValid()
			|| (!Route->Mechanics && !Route->ArtifactMechanics))
		{
			OutError = TEXT("capture_image_policy_unsupported:")
				+ Request.GetPolicyId();
			return false;
		}
		if (!FApp::CanEverRender())
		{
			OutError = TEXT("capture_image_rhi_unavailable:")
				+ Request.GetPolicyId();
			return false;
		}
		TSharedPtr<FJsonObject> Parameters;
		if (!TryProjectParameters(
				*Route,
				Request,
				PreferredOutputPath,
				Parameters,
				OutRequestedFinalPath,
				OutFormat,
				OutError))
		{
			return false;
		}
		FUeAgentRequestContext Context;
		Context.RequestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Context.Command = Route->InternalCommandId;
		Context.Params = Parameters;
		TSharedPtr<FJsonObject> Result;
		const FUeAgentLegacyMechanics MechanicsOwner;
		bool bExecuted = false;
		if (Route->Mechanics)
		{
			bExecuted = (MechanicsOwner.*Route->Mechanics)(
				Context, Result, OutError);
		}
		else
		{
			TArray<TSharedPtr<FJsonValue>> LegacyArtifacts;
			bExecuted = (MechanicsOwner.*Route->ArtifactMechanics)(
				Context, Result, LegacyArtifacts, OutError);
		}
		if (!bExecuted)
		{
			OutError = Route->Descriptor.GetMechanicsId() + TEXT(":") + OutError;
			return false;
		}
		if (Route->Descriptor.GetPolicyId()
			== TEXT("policy:image-capture:transform-candidates/v1"))
		{
			bool bRestored = false;
			bool bDirtyRestored = false;
			bool bSelectionRestored = false;
			bool bViewportRestored = false;
			if (!Result.IsValid()
				|| !Result->TryGetBoolField(TEXT("restored"), bRestored)
				|| !Result->TryGetBoolField(
					TEXT("dirty_state_restored"), bDirtyRestored)
				|| !Result->TryGetBoolField(
					TEXT("selection_restored"), bSelectionRestored)
				|| !Result->TryGetBoolField(
					TEXT("viewport_state_restored"), bViewportRestored)
				|| !bRestored
				|| !bDirtyRestored
				|| !bSelectionRestored
				|| !bViewportRestored)
			{
				OutError = TEXT("capture_transform_candidates_restore_unverified");
				return false;
			}
		}
		if (!TryResolveProducedPath(Result, Parameters, OutProducedPath))
		{
			OutError = TEXT("capture_image_output_missing:")
				+ Route->Descriptor.GetMechanicsId();
			return false;
		}
		OutProducedPath = FPaths::ConvertRelativePathToFull(OutProducedPath);
		return IFileManager::Get().FileSize(*OutProducedPath) > 0;
	}
}
