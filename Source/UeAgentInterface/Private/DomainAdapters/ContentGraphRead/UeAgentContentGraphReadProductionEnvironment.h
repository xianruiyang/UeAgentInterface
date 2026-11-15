#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadNative.h"

class UBlueprint;

namespace UE::UAI::DomainAdapters::ContentGraphRead
{
	class UEAGENTINTERFACE_API
		FUeAgentContentGraphReadProductionEnvironment final
		: public IUeAgentContentGraphReadEnvironment
	{
	public:
		// Native Blueprint graph state for shared graph operations, including
		// subclasses whose public asset domain has a different projection.
		static bool TryCaptureBlueprintRevision(
			UBlueprint& Blueprint,
			FString& OutRevision,
			FUeAgentContentGraphReadError& OutError);

		static bool TryCaptureOwner(
			UObject& Owner,
			FUeAgentContentGraphCapture& OutCapture,
			FUeAgentContentGraphReadError& OutError);
		/** Selected reflected settings, with current native node/type context.
		 * Uses the same TextureGraph projection as full capture. */
		static bool TryCaptureTextureGraphSettings(UObject& Owner,
			const TMap<int32, TSet<FString>>& FieldsByNode,
			FUeAgentContentGraphCapture& OutCapture, FUeAgentContentGraphReadError& OutError,
			bool bIncludeTopology = false);

		virtual bool TryCapture(
			const UE::UAI::Discovery::FUeAgentCandidateBinding& Target,
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			FUeAgentContentGraphCapture& OutCapture,
			FUeAgentContentGraphReadError& OutError) override;

		virtual bool TryCaptureDiagnostics(
			FUeAgentContentGraphCapture& Capture,
			FUeAgentContentGraphReadError& OutError) override;
		virtual bool TryCapturePropertyTopology(
			const UE::UAI::Discovery::FUeAgentCandidateBinding& Target,
			const UE::UAI::ContextResource::FUeAgentExecutionContextRef& Context,
			FUeAgentContentGraphCapture& OutCapture, bool& bDeferredProperties,
			FUeAgentContentGraphReadError& OutError) override;
		virtual bool TryCaptureSelectedProperties(FUeAgentContentGraphCapture& Capture,
			const TSet<FString>& Owners, const TSet<FString>& Fields,
			FUeAgentContentGraphReadError& OutError) override;
		virtual bool TryCaptureItemTopology(
			const UE::UAI::Discovery::FUeAgentCandidateBinding& Target,
			const UE::UAI::ContextResource::FUeAgentExecutionContextRef& Context,
			FUeAgentContentGraphCapture& OutCapture, FUeAgentContentGraphReadError& OutError, bool bIncludeGeometry = true) override;
	};
}
