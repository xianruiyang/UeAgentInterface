#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialNative.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	namespace
	{
		bool Fail(
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			const FString& Code,
			const TCHAR* Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, const TCHAR* Expected)
		{
			OutError = Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_ports"));
			OutError.PropertyPath = Path;
			OutError.ActualValuePresent = Actual.IsValid();
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			return false;
		}

		bool MissingFamily(FUeAgentRigSimulationSpatialExecutionError& OutError, const FString& CapabilityId)
		{
			return Fail(OutError, TEXT("rig_simulation_spatial_family_port_unavailable"),
				TEXT("No native port family supports the requested capability."), TEXT("capability_id"),
				MakeShared<FJsonValueString>(CapabilityId), TEXT("capability supported by a native port family"));
		}

		const FUeAgentRigSimulationSpatialNativePorts* FindFamily(
			const TArray<FUeAgentRigSimulationSpatialNativePorts>& Families,
			const FString& CapabilityId)
		{
			return Families.FindByPredicate(
				[&CapabilityId](const FUeAgentRigSimulationSpatialNativePorts& Ports)
				{
					return Ports.Supports(CapabilityId);
				});
		}

		class FCompositeReadback final
			: public IUeAgentRigSimulationSpatialReadbackPort
		{
		public:
			explicit FCompositeReadback(
				TArray<FUeAgentRigSimulationSpatialNativePorts> InFamilies)
				: Families(MoveTemp(InFamilies)) {}

			bool TryRead(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const TOptional<UE::UAI::Discovery::FUeAgentCandidateBinding>& Target,
				EUeAgentRigSimulationSpatialReadPhase Phase,
				const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
				TOptional<FUeAgentRigSimulationSpatialReadbackResult>& OutReadback,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				const auto* Family = FindFamily(Families, Contract.GetCapabilityId());
				return Family
					? Family->Readback->TryRead(Contract, Request, Target, Phase,
						Context, OutReadback, OutError)
					: MissingFamily(OutError, Contract.GetCapabilityId());
			}

		private:
			TArray<FUeAgentRigSimulationSpatialNativePorts> Families;
		};

		class FCompositeMutation final
			: public IUeAgentRigSimulationSpatialMutationPort
		{
		public:
			explicit FCompositeMutation(
				TArray<FUeAgentRigSimulationSpatialNativePorts> InFamilies)
				: Families(MoveTemp(InFamilies)) {}

			bool TryMutate(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const TArray<UE::UAI::BindingGateway::Operation::FUeAgentOperationTargetBinding>& Targets,
				const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
				FUeAgentRigSimulationSpatialMutationResult& OutResult,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				const auto* Family = FindFamily(Families, Contract.GetCapabilityId());
				return Family
					? Family->Mutation->TryMutate(Contract, Request, Targets, Context,
						OutResult, OutError)
					: MissingFamily(OutError, Contract.GetCapabilityId());
			}

		private:
			TArray<FUeAgentRigSimulationSpatialNativePorts> Families;
		};

		class FCompositeSettle final
			: public IUeAgentRigSimulationSpatialSettlePort
		{
		public:
			explicit FCompositeSettle(
				TArray<FUeAgentRigSimulationSpatialNativePorts> InFamilies)
				: Families(MoveTemp(InFamilies)) {}

			bool TrySettle(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialMutationResult& Mutation,
				const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				const auto* Family = FindFamily(Families, Contract.GetCapabilityId());
				return Family
					? Family->Settle->TrySettle(Contract, Mutation, Context, OutError)
					: MissingFamily(OutError, Contract.GetCapabilityId());
			}

		private:
			TArray<FUeAgentRigSimulationSpatialNativePorts> Families;
		};

		class FCompositeComparator final
			: public IUeAgentRigSimulationSpatialResultComparator
		{
		public:
			explicit FCompositeComparator(
				TArray<FUeAgentRigSimulationSpatialNativePorts> InFamilies)
				: Families(MoveTemp(InFamilies)) {}

			bool TryCompare(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot& Before,
				const FUeAgentRigSimulationSpatialReadbackResult& After,
				const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
				UE::UAI::Verification::FVerificationOutcome& OutOutcome,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				const auto* Family = FindFamily(Families, Contract.GetCapabilityId());
				return Family
					? Family->Comparator->TryCompare(Contract, Request, Before, After,
						Context, OutOutcome, OutError)
					: MissingFamily(OutError, Contract.GetCapabilityId());
			}

		private:
			TArray<FUeAgentRigSimulationSpatialNativePorts> Families;
		};
	}

	bool TryComposeRigSimulationSpatialNativePorts(
		TArray<FUeAgentRigSimulationSpatialNativePorts> FamilyPorts,
		FUeAgentRigSimulationSpatialNativePorts& OutPorts,
		FUeAgentRigSimulationSpatialExecutionError& OutError)
	{
		OutPorts = {};
		OutError = {};
		if (FamilyPorts.IsEmpty())
			return Fail(OutError, TEXT("rig_simulation_spatial_native_family_ports_empty"),
				TEXT("No native port families were supplied."), TEXT("families"),
				MakeShared<FJsonValueNumber>(0), TEXT("at least one native port family"));
		for (int32 Index = 0; Index < FamilyPorts.Num(); ++Index)
		{
			const FUeAgentRigSimulationSpatialNativePorts& Family = FamilyPorts[Index];
			const FString Prefix = FString::Printf(TEXT("families[%d]"), Index);
			if (!Family.TryValidate(OutError))
			{
				OutError.PropertyPath = OutError.PropertyPath.IsEmpty() ? Prefix : Prefix + TEXT(".") + OutError.PropertyPath;
				return false;
			}
			for (const FString& CapabilityId : Family.SupportedCapabilities)
			{
				if (OutPorts.SupportedCapabilities.Contains(CapabilityId))
					return Fail(OutError,
						TEXT("rig_simulation_spatial_native_capability_owner_ambiguous"),
						TEXT("This capability is provided by more than one native port family."),
						Prefix + TEXT(".supported_capabilities"), MakeShared<FJsonValueString>(CapabilityId),
						TEXT("one native port family per capability"));
				OutPorts.SupportedCapabilities.Add(CapabilityId);
			}
		}
		OutPorts.Readback = MakeShared<FCompositeReadback>(FamilyPorts);
		OutPorts.Mutation = MakeShared<FCompositeMutation>(FamilyPorts);
		OutPorts.Settle = MakeShared<FCompositeSettle>(FamilyPorts);
		OutPorts.Comparator = MakeShared<FCompositeComparator>(MoveTemp(FamilyPorts));
		return OutPorts.TryValidate(OutError);
	}
}
