#include "DomainAdapters/CaptureMediaLongJob/UeAgentPcgViewportCapture.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"

#include "Dom/JsonObject.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentViewportCapture.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentWorldViewportCapture.h"
#include "Misc/Guid.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	using namespace CaptureDiagnostics;
	namespace
	{
		bool HasActorSelector(const TSharedPtr<FJsonObject>& Parameters)
		{
			return Parameters.IsValid()
				&& (Parameters->HasField(TEXT("actor"))
					|| Parameters->HasField(TEXT("actor_name"))
					|| Parameters->HasField(TEXT("actor_label")));
		}

		bool TryGetActorIdentity(
			const TSharedPtr<FJsonObject>& Parameters,
			FString& OutActorIdentity, FString& OutSourcePointer)
		{
			OutSourcePointer.Reset();
			if (!Parameters.IsValid())
			{
				return false;
			}
			for (const TCHAR* Field
				: {TEXT("actor"), TEXT("actor_name"), TEXT("actor_label")})
			{
				if (Parameters->TryGetStringField(Field, OutActorIdentity)
					&& !OutActorIdentity.IsEmpty())
				{
					OutSourcePointer = FString(TEXT("/")) + Field;
					return true;
				}
			}
			OutActorIdentity.Reset();
			return false;
		}

		void LocateFrameActorAlias(UE::UAI::Contract::FDiagnostic& Error, const FString& SourcePointer)
		{
			if (SourcePointer.IsEmpty() || Error.IsEmpty()) return;
			if (Error.SourceKind == TEXT("command_input") && Error.Source.SourceFile.IsEmpty())
			{
				for (FString* Pointer : { &Error.Source.JsonPath, &Error.SemanticPointer, &Error.PropertyPath })
					if (*Pointer == TEXT("/id")) *Pointer = SourcePointer;
			}
			for (auto& Child : Error.Diagnostics) LocateFrameActorAlias(Child, SourcePointer);
			if (Error.PublicCause.IsValid())
			{
				auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Error.PublicCause);
				LocateFrameActorAlias(*Cause, SourcePointer);
				Error.PublicCause = Cause;
			}
		}
	}

	bool TryCapturePcgViewportImage(
		const TSharedPtr<FJsonObject>& Parameters,
		const FString& AssetNotFoundError,
		TSharedPtr<FJsonObject>& OutData,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		(void)AssetNotFoundError;
		OutData.Reset();
		OutError.Reset();

		FUeAgentRequestContext Context;
		Context.RequestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Context.Command = TEXT("pcg_screenshot");
		Context.Params = Parameters;

		TSharedPtr<FJsonObject> FrameData;
		if (HasActorSelector(Parameters))
		{
			FUeAgentRequestContext FrameContext = Context;
			TSharedPtr<FJsonObject> FrameParameters = MakeShared<FJsonObject>();
			FrameParameters->Values = Parameters->Values;
			FString ActorSourcePointer;
			if (!FrameParameters->HasField(TEXT("id")))
			{
				FString ActorIdentity;
				if (TryGetActorIdentity(Parameters, ActorIdentity, ActorSourcePointer))
				{
					FrameParameters->SetStringField(TEXT("id"), ActorIdentity);
				}
			}
			FrameContext.Params = FrameParameters;
			if (!FWorldViewportCapture::FrameActor(
					FrameContext, FrameData, OutError))
			{
				LocateFrameActorAlias(OutError, ActorSourcePointer);
				return false;
			}
		}

		TSharedPtr<FJsonObject> ScreenshotData;
		if (!FViewportCapture::Capture(Context, ScreenshotData, OutError)) return false;
		if (!ScreenshotData.IsValid())
		{
			OutError = Native(TEXT("pcg_screenshot_result_missing"), TEXT("Viewport capture returned no screenshot result."),
				TEXT("capture_native"), TEXT("screenshot_result_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}

		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(
			TEXT("schema"), TEXT("ue_agent_interface.pcg.screenshot.v2"));
		OutData->SetObjectField(
			TEXT("frame_actor"),
			FrameData.IsValid() ? FrameData : MakeShared<FJsonObject>());
		OutData->SetObjectField(TEXT("screenshot"), ScreenshotData);
		FString FilePath;
		if (ScreenshotData->TryGetStringField(TEXT("file_path"), FilePath))
		{
			OutData->SetStringField(TEXT("file_path"), FilePath);
		}
		return true;
	}
}
