#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaImagePolicy.h"

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

	namespace
	{
		struct FPolicyRoute final
		{
			FUeAgentCaptureMediaImagePolicyDescriptor Descriptor;
			FString InternalCommandId;
			FMechanics Mechanics = nullptr;
		};

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
			else if (Subject.StartsWith(TEXT("capture-subject:actor:"))
				&& !OutParameters->HasField(TEXT("actor")))
			{
				OutParameters->SetStringField(TEXT("actor"), Subject.Mid(22));
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
					: {TEXT("file_path"), TEXT("output_file"), TEXT("path")})
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
		FString InMechanicsId)
		: PolicyId(MoveTemp(InPolicyId))
		, MechanicsId(MoveTemp(InMechanicsId))
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

	bool FUeAgentCaptureMediaImagePolicyDescriptor::IsValid() const
	{
		return PolicyId.StartsWith(TEXT("policy:image-capture:"))
			&& MechanicsId.StartsWith(TEXT("mechanics:capture:"));
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
		if (!Route || !Route->Descriptor.IsValid() || !Route->Mechanics)
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
		if (!(MechanicsOwner.*Route->Mechanics)(Context, Result, OutError))
		{
			OutError = Route->Descriptor.GetMechanicsId() + TEXT(":") + OutError;
			return false;
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
