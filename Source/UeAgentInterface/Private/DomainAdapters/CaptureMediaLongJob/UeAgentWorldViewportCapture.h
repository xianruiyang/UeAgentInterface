#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentMechanicsRequestContext.h"

class AActor;
class FJsonObject;
class FJsonValue;
class UWorld;

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	/** Owns World Actor preview and Level Viewport framing used by capture policies. */
	class UEAGENTINTERFACE_API FWorldViewportCapture final
	{
	public:
		static bool ProbeActorTransformCandidates(
			const FUeAgentRequestContext& Context,
			TSharedPtr<FJsonObject>& OutData,
			TArray<TSharedPtr<FJsonValue>>& OutArtifacts,
			UE::UAI::Contract::FDiagnostic& OutError);

		static bool FrameActor(
			const FUeAgentRequestContext& Context,
			TSharedPtr<FJsonObject>& OutData,
			UE::UAI::Contract::FDiagnostic& OutError);

	private:
		static bool FrameActorCore(
			const FUeAgentRequestContext& Context,
			TSharedPtr<FJsonObject>& OutData,
			UE::UAI::Contract::FDiagnostic& OutError);
		static UWorld* GetEditorWorld(UE::UAI::Contract::FDiagnostic& OutError);
		static AActor* FindActorByNameOrLabel(
			UWorld* World,
			const FString& NameOrLabel,
			UE::UAI::Contract::FDiagnostic& OutError);
		static bool JsonTryGetString(
			const TSharedPtr<FJsonObject>& Object,
			const FString& Key,
			FString& OutValue);
		static bool JsonTryGetNumber(
			const TSharedPtr<FJsonObject>& Object,
			const FString& Key,
			double& OutValue);
		static bool JsonTryGetBool(
			const TSharedPtr<FJsonObject>& Object,
			const FString& Key,
			bool& OutValue);
		static TSharedPtr<FJsonObject> VecToJson(const FVector& Value);
		static TSharedPtr<FJsonObject> RotToJson(const FRotator& Value);
	};
}
