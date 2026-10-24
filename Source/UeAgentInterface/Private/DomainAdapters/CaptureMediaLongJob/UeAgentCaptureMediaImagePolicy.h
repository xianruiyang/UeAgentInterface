#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentMechanicsRequestContext.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaLongJobAdapter.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	class UEAGENTINTERFACE_API
	FUeAgentCaptureMediaImagePolicyDescriptor final
	{
	public:
		FUeAgentCaptureMediaImagePolicyDescriptor(
			FString InPolicyId,
			FString InMechanicsId,
			FString InParameterSchemaJson = FString(),
			FString InParameterGuidance = FString(),
			FString InResultGuidance = FString(),
			FString InMinimumExample = FString(),
			FString InSubjectSchemaJson = FString());

		const FString& GetPolicyId() const;
		const FString& GetMechanicsId() const;
		const FString& GetParameterSchemaJson() const;
		const FString& GetSubjectSchemaJson() const;
		const FString& GetParameterGuidance() const;
		const FString& GetResultGuidance() const;
		const FString& GetMinimumExample() const;
		bool IsValid() const;
		bool TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const;

	private:
		FString PolicyId;
		FString MechanicsId;
		FString ParameterSchemaJson;
		FString SubjectSchemaJson;
		FString ParameterGuidance;
		FString ResultGuidance;
		FString MinimumExample;
	};

	UEAGENTINTERFACE_API const TArray<
		FUeAgentCaptureMediaImagePolicyDescriptor>&
	GetCaptureMediaImagePolicies();

	UEAGENTINTERFACE_API const FUeAgentCaptureMediaImagePolicyDescriptor*
	FindCaptureMediaImagePolicy(const FString& PolicyId);

	UEAGENTINTERFACE_API bool TryNormalizeTransformCandidateParameters(
		const TSharedPtr<FJsonObject>& Parameters,
		UE::UAI::Contract::FDiagnostic& OutError,
		TMap<FString, UE::UAI::Contract::FSourceLocation>* ParameterSources = nullptr);

	struct UEAGENTINTERFACE_API FUeAgentCaptureMediaImageExecutionResult final
	{
		FString ProducedPath;
		FString RequestedFinalPath;
		FString Format;
		TArray<FString> AdditionalProducedPaths;
		TSharedPtr<FJsonObject> CaptureReceipt;

		bool IsValid() const;
		bool TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const;
	};

	struct UEAGENTINTERFACE_API FUeAgentPreparedCaptureMediaImageExecution final
	{
		FUeAgentRequestContext Context;
		FString RequestedFinalPath;
		FString Format;
		// Typed provenance owned by parameter projection; an empty entry marks a native/config-derived subtree.
		TMap<FString, UE::UAI::Contract::FSourceLocation> ParameterSources;

		bool IsValid() const;
		void LocateDiagnosticSources(UE::UAI::Contract::FDiagnostic& Diagnostic) const;
		bool TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const;
	};

	UEAGENTINTERFACE_API bool IsViewportSequenceCapturePolicy(
		const FString& PolicyId);

	UEAGENTINTERFACE_API bool TryPrepareCaptureMediaImagePolicyExecution(
		const FUeAgentCaptureMediaLongJobTypedRequest& Request,
		const FString& PreferredOutputPath,
		FUeAgentPreparedCaptureMediaImageExecution& OutPrepared,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryFinalizeCaptureMediaImagePolicyExecution(
		const FUeAgentCaptureMediaLongJobTypedRequest& Request,
		const FUeAgentPreparedCaptureMediaImageExecution& Prepared,
		const TSharedPtr<FJsonObject>& MechanicsResult,
		FUeAgentCaptureMediaImageExecutionResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError);

	/**
	 * Routes one validated image policy to its native capture owner. The
	 * adapter owns policy/subject translation and output confinement only.
	 */
	UEAGENTINTERFACE_API bool TryExecuteCaptureMediaImagePolicy(
		const FUeAgentCaptureMediaLongJobTypedRequest& Request,
		const FString& PreferredOutputPath,
		FUeAgentCaptureMediaImageExecutionResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError);
}
