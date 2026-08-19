#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialNative.h"

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	namespace
	{
		bool Fail(
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			const FString& Code,
			const FString& Subject = FString())
		{
			OutError = {};
			OutError.Code = Code;
			OutError.SubjectId = Subject;
			return false;
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
				TSharedPtr<const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				const auto* Family = FindFamily(Families, Contract.GetCapabilityId());
				return Family
					? Family->Readback->TryRead(Contract, Request, Target, Phase,
						Context, OutSnapshot, OutError)
					: Fail(OutError, TEXT("rig_simulation_spatial_family_port_unavailable"),
						Contract.GetCapabilityId());
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
					: Fail(OutError, TEXT("rig_simulation_spatial_family_port_unavailable"),
						Contract.GetCapabilityId());
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
					: Fail(OutError, TEXT("rig_simulation_spatial_family_port_unavailable"),
						Contract.GetCapabilityId());
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
				const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot& After,
				const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
				UE::UAI::Verification::FVerificationOutcome& OutOutcome,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				const auto* Family = FindFamily(Families, Contract.GetCapabilityId());
				return Family
					? Family->Comparator->TryCompare(Contract, Request, Before, After,
						Context, OutOutcome, OutError)
					: Fail(OutError, TEXT("rig_simulation_spatial_family_port_unavailable"),
						Contract.GetCapabilityId());
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
			return Fail(OutError, TEXT("rig_simulation_spatial_native_family_ports_empty"));
		for (const FUeAgentRigSimulationSpatialNativePorts& Family : FamilyPorts)
		{
			if (!Family.IsValid())
				return Fail(OutError,
					TEXT("rig_simulation_spatial_native_family_ports_invalid"));
			for (const FString& CapabilityId : Family.SupportedCapabilities)
			{
				if (OutPorts.SupportedCapabilities.Contains(CapabilityId))
					return Fail(OutError,
						TEXT("rig_simulation_spatial_native_capability_owner_ambiguous"),
						CapabilityId);
				OutPorts.SupportedCapabilities.Add(CapabilityId);
			}
		}
		OutPorts.Readback = MakeShared<FCompositeReadback>(FamilyPorts);
		OutPorts.Mutation = MakeShared<FCompositeMutation>(FamilyPorts);
		OutPorts.Settle = MakeShared<FCompositeSettle>(FamilyPorts);
		OutPorts.Comparator = MakeShared<FCompositeComparator>(MoveTemp(FamilyPorts));
		return OutPorts.IsValid() ? true : Fail(OutError,
			TEXT("rig_simulation_spatial_native_composite_ports_unavailable"));
	}
}
