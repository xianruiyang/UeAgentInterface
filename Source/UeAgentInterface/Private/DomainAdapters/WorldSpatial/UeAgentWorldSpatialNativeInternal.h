#pragma once

#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialNative.h"

namespace UE::UAI::DomainAdapters::WorldSpatial::Private
{
	using UE::UAI::BindingGateway::Operation::FUeAgentOperationTargetBinding;
	using UE::UAI::ContextResource::FUeAgentOperationContext;
	using UE::UAI::Discovery::FUeAgentCandidateBinding;
	using UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot;
	using UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr;
	using UE::UAI::StateSemantic::FUeAgentStateRevision;
	using UE::UAI::Verification::FVerificationOutcome;

	struct FObservedEntry
	{
		FString SemanticId;
		FString TargetKind;
		FString OwnerId;
		FString Selector;
		FString ObjectRole;
		FString TypeId;
		FUeAgentCanonicalValuePtr Value;
	};

	class IWorldSpatialNativeFamily
	{
	public:
		virtual ~IWorldSpatialNativeFamily() = default;
		virtual const FString& GetFamilyId() const = 0;

		virtual bool TryRead(
			const FUeAgentWorldSpatialExecutionChainContract& Contract,
			const FUeAgentWorldSpatialRequest& Request,
			const TOptional<FUeAgentCandidateBinding>& Target,
			EUeAgentWorldSpatialReadPhase Phase,
			const FUeAgentOperationContext& Context,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentWorldSpatialExecutionError& OutError) = 0;

		virtual bool TryMutate(
			const FUeAgentWorldSpatialExecutionChainContract& Contract,
			const FUeAgentWorldSpatialRequest& Request,
			const TArray<FUeAgentOperationTargetBinding>& Targets,
			const FUeAgentOperationContext& Context,
			FUeAgentWorldSpatialMutationResult& OutResult,
			FUeAgentWorldSpatialExecutionError& OutError) = 0;

		virtual bool TrySettle(
			const FUeAgentWorldSpatialExecutionChainContract& Contract,
			const FUeAgentWorldSpatialMutationResult& Mutation,
			const FUeAgentOperationContext& Context,
			FUeAgentWorldSpatialExecutionError& OutError) = 0;

		virtual bool TryCompare(
			const FUeAgentWorldSpatialExecutionChainContract& Contract,
			const FUeAgentWorldSpatialRequest& Request,
			const FUeAgentCanonicalSnapshot& Before,
			const FUeAgentCanonicalSnapshot& After,
			const FUeAgentOperationContext& Context,
			FVerificationOutcome& OutOutcome,
			FUeAgentWorldSpatialExecutionError& OutError) = 0;
	};

	using FWorldSpatialNativeFamilyRef =
		TSharedRef<IWorldSpatialNativeFamily>;

	FWorldSpatialNativeFamilyRef MakeGraphViewNativeFamily(
		const FUeAgentWorldSpatialNativeDependencies& Dependencies);
	FWorldSpatialNativeFamilyRef MakeGraphLayoutNativeFamily(
		const FUeAgentWorldSpatialNativeDependencies& Dependencies);
	FWorldSpatialNativeFamilyRef MakeObjectTransformNativeFamily(
		const FUeAgentWorldSpatialNativeDependencies& Dependencies);
	FWorldSpatialNativeFamilyRef MakeViewportProjectionNativeFamily(
		const FUeAgentWorldSpatialNativeDependencies& Dependencies);
	FWorldSpatialNativeFamilyRef MakeViewportSessionStateNativeFamily(
		const FUeAgentWorldSpatialNativeDependencies& Dependencies);

	bool Fail(
		FUeAgentWorldSpatialExecutionError& OutError,
		FString Code,
		FString SubjectId = FString(),
		FString Detail = FString());

	const FUeAgentWorldSpatialRequest* GetProductionRequest(
		const FUeAgentWorldSpatialTypedRequestRef& Request,
		FUeAgentWorldSpatialExecutionError& OutError);

	bool TryCheckRevision(
		IUeAgentWorldSpatialRevisionSource& Source,
		const FString& ExplicitRef,
		const FWorldSpatialRevision& Expected,
		FUeAgentWorldSpatialExecutionError& OutError);

	bool TryMakeDecimalValue(
		double Value,
		const FUeAgentOperationContext& Context,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentWorldSpatialExecutionError& OutError);

	bool TryMakeIntegerValue(
		int64 Value,
		const FUeAgentOperationContext& Context,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentWorldSpatialExecutionError& OutError);

	bool TryMakeCompiledValue(
		const FUeAgentWorldSpatialCompiledValue& Value,
		const FUeAgentOperationContext& Context,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentWorldSpatialExecutionError& OutError);

	bool TryMakeSnapshot(
		const FUeAgentWorldSpatialExecutionChainContract& Contract,
		EUeAgentWorldSpatialReadPhase Phase,
		const FString& StateFingerprint,
		const TArray<FObservedEntry>& Observations,
		const FUeAgentOperationContext& Context,
		TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
		FUeAgentWorldSpatialExecutionError& OutError,
		const FUeAgentStateRevision* RevisionOverride = nullptr);

	const UE::UAI::StateSemantic::FUeAgentStateEntry* FindEntryByRole(
		const FUeAgentCanonicalSnapshot& Snapshot,
		const FString& Role);

	bool TryReadCompiledValue(
		const FUeAgentCanonicalSnapshot& Snapshot,
		const FString& Role,
		FUeAgentWorldSpatialCompiledValue& OutValue,
		FUeAgentWorldSpatialExecutionError& OutError);

	bool TryReadDecimalValue(
		const FUeAgentCanonicalSnapshot& Snapshot,
		const FString& Role,
		double& OutValue,
		FUeAgentWorldSpatialExecutionError& OutError);

	void SetComparisonOutcome(
		const FUeAgentWorldSpatialExecutionChainContract& Contract,
		bool bMatches,
		const FString& EvidenceId,
		FVerificationOutcome& OutOutcome);

	bool NearlyEqualComponents(
		const TArray<double>& Left,
		const TArray<double>& Right,
		double Tolerance = 0.001);
}
