#include "DomainAdapters/CaptureMediaLongJob/UeAgentLevelViewportImageCapture.h"

#include "Core/EditorCapture/UeAgentSlateWindowCapture.h"
#include "Core/EditorViewport/UeAgentViewportRenderSize.h"
#include "CoreGlobals.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageArtifactService.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentViewportCapture.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentWorldViewportCaptureMechanics.h"
#include "Dom/JsonObject.h"
#include "LevelEditorViewport.h"
#include "Slate/SceneViewport.h"
#include "Utilities/UeAgentEditorCPUThrottleScope.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	using namespace CaptureDiagnostics;

	class FLevelViewportImageCapture::FImpl final
	{
	public:
		FUeAgentRequestContext Context;
		FLevelEditorViewportClient* Client = nullptr;
		TSharedPtr<FSceneViewport> Viewport;
		EditorCapture::SlateWindow::FScopedViewportExtent Extent;
		TUniquePtr<EditorRuntime::FScopedCPUThrottleExemption> CPUThrottleExemption;
		uint64 LastEngineFrame = GFrameCounter;
		int32 InitializationDraws = 0;
		bool bCold = false;
		bool bCaptureAttempted = false;
		bool bCancelled = false;

		void Restore()
		{
			Extent.Restore();
			CPUThrottleExemption.Reset();
		}
	};

	FLevelViewportImageCapture::FLevelViewportImageCapture(TUniquePtr<FImpl> InImpl)
		: Impl(MoveTemp(InImpl)) {}
	FLevelViewportImageCapture::~FLevelViewportImageCapture() = default;

	bool FLevelViewportImageCapture::TryCreate(const FUeAgentRequestContext& Context,
		TSharedPtr<IViewportFrameCapture>& OutCapture, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutCapture.Reset();
		FIntPoint ColdSize = FIntPoint::ZeroValue;
		FString Source;
		if (Context.Params && Context.Params->HasField(TEXT("cold_start_render_size")))
		{
			const TSharedPtr<FJsonObject>* SizeObject = nullptr;
			double Width = 0, Height = 0;
			if (!Context.Params->TryGetObjectField(TEXT("cold_start_render_size"), SizeObject)
				|| !SizeObject || !SizeObject->IsValid()
				|| !(*SizeObject)->TryGetNumberField(TEXT("width"), Width)
				|| !(*SizeObject)->TryGetNumberField(TEXT("height"), Height)
				|| !FMath::IsFinite(Width) || !FMath::IsFinite(Height)
				|| Width != FMath::RoundToDouble(Width) || Height != FMath::RoundToDouble(Height)
				|| Width < 1 || Height < 1 || Width > 4096 || Height > 4096)
			{
				OutError = Input(TEXT("cold_start_render_size_invalid"),
					TEXT("Cold-start render width and height must be integers from 1 to 4096."),
					TEXT("capture_prepare"), TEXT("/cold_start_render_size"),
					Context.Params->TryGetField(TEXT("cold_start_render_size")),
					MakeShared<FJsonValueString>(TEXT("{width: integer 1..4096, height: integer 1..4096}")));
				return false;
			}
			ColdSize = FIntPoint(static_cast<int32>(Width), static_cast<int32>(Height));
			Source = TEXT("cold_start_request");
		}
		auto State = MakeUnique<FImpl>();
		State->Context = Context;
		State->Client = WorldViewportMechanics::GetPreferredLevelViewportClient();
		State->Viewport = WorldViewportMechanics::GetLevelSceneViewport(State->Client);
		State->bCold = State->Viewport && (State->Viewport->GetSizeXY().X <= 0 || State->Viewport->GetSizeXY().Y <= 0);
		if (State->bCold && ColdSize == FIntPoint::ZeroValue)
			ColdSize = EditorViewport::ResolveZeroViewportExtent(State->Viewport, Source);
		if (!State->Extent.Prepare(State->Viewport, &ColdSize, Source, OutError)) return false;
		if (State->bCold) State->CPUThrottleExemption = MakeUnique<EditorRuntime::FScopedCPUThrottleExemption>();
		OutCapture = MakeShareable(new FLevelViewportImageCapture(MoveTemp(State)));
		return true;
	}

	bool FLevelViewportImageCapture::CanExecuteImmediately() const { return !Impl->bCold; }

	bool FLevelViewportImageCapture::Tick(TSharedPtr<FJsonObject>& OutData, bool& bOutComplete,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		bOutComplete = false;
		if (Impl->bCancelled || WorldViewportMechanics::GetPreferredLevelViewportClient() != Impl->Client
			|| WorldViewportMechanics::GetLevelSceneViewport(Impl->Client) != Impl->Viewport)
		{
			OutError = Native(TEXT("viewport_capture_context_changed"),
				TEXT("The selected level viewport changed or its capture was cancelled during initialization."),
				TEXT("capture_native"), TEXT("active_viewport_matches"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		if (Impl->bCold)
		{
			if (Impl->LastEngineFrame == GFrameCounter) return true;
			Impl->LastEngineFrame = GFrameCounter;
			// Separate Editor frames allow renderer/streaming work to consume the
			// first native view. Repeated Draw calls in one GT task cannot do that.
			if (Impl->InitializationDraws < 2)
			{
				Impl->Viewport->Invalidate();
				Impl->Viewport->Draw(false);
				++Impl->InitializationDraws;
				return true;
			}
		}
		Impl->bCaptureAttempted = true;
		if (!FViewportCapture::Capture(Impl->Context, OutData, OutError)) return false;
		Impl->Restore();
		const TSharedPtr<FJsonObject>* CaptureInfo = nullptr;
		if (OutData && OutData->TryGetObjectField(TEXT("capture"), CaptureInfo) && CaptureInfo && *CaptureInfo)
		{
			auto Initialization = Impl->Extent.Describe();
			Initialization->SetNumberField(TEXT("engine_frames_observed"), Impl->bCold ? Impl->InitializationDraws + 1 : 0);
			Initialization->SetNumberField(TEXT("initialization_draws"), Impl->InitializationDraws);
			(*CaptureInfo)->SetObjectField(TEXT("viewport_initialization"), Initialization);
		}
		bOutComplete = true;
		return true;
	}

	void FLevelViewportImageCapture::Cancel()
	{
		Impl->bCancelled = true;
		Impl->Restore();
		if (Impl->bCaptureAttempted)
		{
			// Context comes only from the image policy's prepared execution: its
			// output_file is native staging, never the caller's publication path.
			FString NativePath;
			if (Impl->Context.Params && Impl->Context.Params->TryGetStringField(TEXT("output_file"), NativePath))
				DiscardProducedCaptureFile(NativePath);
			Impl->bCaptureAttempted = false;
		}
	}
}
