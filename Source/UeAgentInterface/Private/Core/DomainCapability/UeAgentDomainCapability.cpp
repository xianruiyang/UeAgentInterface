#include "Core/DomainCapability/UeAgentDomainCapability.h"

#include "Core/BindingGateway/Query/UeAgentQueryBindingError.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

namespace UE::UAI::DomainCapability
{
	using namespace BindingGateway;
	using namespace Module::CommandRegistration;

	namespace
	{
		bool IsExactIdentity(const FString& Value)
		{
			FString Normalized = Value;
			Normalized.TrimStartAndEndInline();
			return !Normalized.IsEmpty() && Normalized == Value;
		}

		bool Fail(FString& OutError, const FString& Code)
		{
			OutError = Code;
			return false;
		}

		bool ValidateSupport(
			const FString& DomainId,
			const TCHAR* Category,
			const EDomainCapabilitySupport Support,
			const bool bHasPayload,
			FString& OutError)
		{
			if (Support == EDomainCapabilitySupport::Undeclared)
			{
				return Fail(
					OutError,
					TEXT("domain_capability_support_undeclared:")
						+ DomainId + TEXT(":") + Category);
			}
			if (Support == EDomainCapabilitySupport::Unsupported
				&& bHasPayload)
			{
				return Fail(
					OutError,
					TEXT("domain_capability_unsupported_payload:")
						+ DomainId + TEXT(":") + Category);
			}
			if (Support == EDomainCapabilitySupport::Supported
				&& !bHasPayload)
			{
				return Fail(
					OutError,
					TEXT("domain_capability_supported_payload_missing:")
						+ DomainId + TEXT(":") + Category);
			}
			return true;
		}

		bool AddUniqueIdentity(
			TSet<FString>& Seen,
			const FString& Identity,
			const TCHAR* ErrorPrefix,
			FString& OutError)
		{
			if (!IsExactIdentity(Identity))
			{
				return Fail(
					OutError,
					FString(ErrorPrefix) + TEXT("_invalid:") + Identity);
			}
			if (Seen.Contains(Identity))
			{
				return Fail(
					OutError,
					FString(ErrorPrefix) + TEXT("_conflict:") + Identity);
			}
			Seen.Add(Identity);
			return true;
		}

		FString HjsonRouteKey(const FDomainHjsonRoute& Route)
		{
			return Route.CommandId + TEXT("|")
				+ Route.SchemaId + TEXT("|")
				+ Route.TargetKind + TEXT("|")
				+ Route.RootTypeIdentity + TEXT("|")
				+ FString::FromInt(static_cast<int32>(Route.Role));
		}

		bool ValidateDeclaration(
			const FDomainCapabilityDeclaration& Declaration,
			FString& OutError)
		{
			OutError.Reset();
			if (!IsExactIdentity(Declaration.DomainId))
			{
				return Fail(
					OutError,
					TEXT("domain_capability_domain_id_invalid:")
						+ Declaration.DomainId);
			}

			if (!ValidateSupport(
				Declaration.DomainId,
				TEXT("command"),
				Declaration.Command.Support,
				!Declaration.Command.Contributions.IsEmpty(),
				OutError)
				|| !ValidateSupport(
					Declaration.DomainId,
					TEXT("query"),
					Declaration.Query.Support,
					!Declaration.Query.Providers.IsEmpty(),
					OutError)
				|| !ValidateSupport(
					Declaration.DomainId,
					TEXT("discovery"),
					Declaration.Discovery.Support,
					Declaration.Discovery.Sources.Num()
						+ Declaration.Discovery.ReadPorts.Num() > 0,
					OutError))
			{
				return false;
			}
			if (!ValidateSupport(
				Declaration.DomainId,
				TEXT("hjson"),
				Declaration.Hjson.Support,
				Declaration.Hjson.Providers.Num()
					+ Declaration.Hjson.Routes.Num() > 0,
				OutError)
				|| !ValidateSupport(
					Declaration.DomainId,
					TEXT("composition"),
					Declaration.Composition.Support,
					!Declaration.Composition.Contracts.IsEmpty(),
					OutError)
				|| !ValidateSupport(
					Declaration.DomainId,
					TEXT("initialization"),
					Declaration.Initialization.Support,
					!Declaration.Initialization.ContractIds.IsEmpty(),
					OutError)
				|| !ValidateSupport(
					Declaration.DomainId,
					TEXT("operation"),
					Declaration.Operation.Support,
					!Declaration.Operation.Operations.IsEmpty(),
					OutError)
				|| !ValidateSupport(
					Declaration.DomainId,
					TEXT("layout"),
					Declaration.Layout.Support,
					!Declaration.Layout.Layouts.IsEmpty(),
					OutError)
				|| !ValidateSupport(
					Declaration.DomainId,
					TEXT("state"),
					Declaration.State.Support,
					!Declaration.State.Planes.IsEmpty(),
					OutError)
				|| !ValidateSupport(
					Declaration.DomainId,
					TEXT("surface"),
					Declaration.Surface.Support,
					!Declaration.Surface.Surfaces.IsEmpty(),
					OutError)
				|| !ValidateSupport(
					Declaration.DomainId,
					TEXT("migration"),
					Declaration.Migration.Support,
					!Declaration.Migration.OwnerId.IsEmpty(),
					OutError))
			{
				return false;
			}

			TSet<FString> ContributionIds;
			TSet<FString> CommandIds;
			for (const TSharedRef<Active::IUeAgentActiveCommandContribution>&
				Contribution : Declaration.Command.Contributions)
			{
				if (!AddUniqueIdentity(
					ContributionIds,
					Contribution->GetContributionId(),
					TEXT("domain_capability_command_contribution"),
					OutError))
				{
					return false;
				}
				TArray<CommandContract::FUeAgentCommandDeclaration>
					Declarations;
				Contribution->CollectDeclarations(Declarations);
				if (Declarations.IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("domain_capability_command_declaration_missing:")
							+ Contribution->GetContributionId());
				}
				for (const CommandContract::FUeAgentCommandDeclaration& Command :
					Declarations)
				{
					if (!AddUniqueIdentity(
						CommandIds,
						Command.CommandId,
						TEXT("domain_capability_command_id"),
						OutError))
					{
						return false;
					}
				}
			}

			TSet<FString> QueryBindings;
			for (const Query::FUeAgentDomainQueryProviderRegistration& Provider :
				Declaration.Query.Providers)
			{
				if (!Provider.IsValid()
					|| !AddUniqueIdentity(
						QueryBindings,
						Provider.GetProviderBinding(),
						TEXT("domain_capability_query_binding"),
						OutError))
				{
					if (OutError.IsEmpty())
					{
						OutError = TEXT("domain_capability_query_provider_invalid:")
							+ Provider.GetProviderBinding();
					}
					return false;
				}
			}

			TSet<FString> SurfaceIds;
			for (const FDomainSemanticSurfaceEntry& Surface :
				Declaration.Surface.Surfaces)
			{
				if (!AddUniqueIdentity(
					SurfaceIds,
					Surface.SurfaceId,
					TEXT("domain_capability_surface"),
					OutError))
				{
					return false;
				}
				if (!IsExactIdentity(Surface.MechanicsId))
				{
					return Fail(
						OutError,
						TEXT("domain_capability_surface_mechanics_missing:")
							+ Surface.SurfaceId);
				}
				if (!IsExactIdentity(Surface.ReadPortId))
				{
					return Fail(
						OutError,
						TEXT("domain_capability_surface_readback_missing:")
							+ Surface.SurfaceId);
				}
			}

			TSet<FString> InitializationContracts;
			for (const FString& ContractId :
				Declaration.Initialization.ContractIds)
			{
				if (!AddUniqueIdentity(
					InitializationContracts,
					ContractId,
					TEXT("domain_capability_initialization_contract"),
					OutError))
				{
					return false;
				}
			}

			TSet<FString> HjsonProviderIds;
			for (const TSharedRef<
				HjsonWorkspace::IImportInitializationDomainProvider>& Provider :
				Declaration.Hjson.Providers)
			{
				if (!AddUniqueIdentity(
					HjsonProviderIds,
					Provider->GetProviderId(),
					TEXT("domain_capability_hjson_provider"),
					OutError))
				{
					return false;
				}
			}
			TSet<FString> HjsonRoutes;
			for (const FDomainHjsonRoute& Route : Declaration.Hjson.Routes)
			{
				if (!HjsonProviderIds.Contains(Route.ProviderId)
					|| !IsExactIdentity(Route.CommandId)
					|| !IsExactIdentity(Route.SchemaId)
					|| !IsExactIdentity(Route.TargetKind)
					|| !IsExactIdentity(Route.RootTypeIdentity))
				{
					return Fail(
						OutError,
						TEXT("domain_capability_hjson_route_invalid:")
							+ Route.RootTypeIdentity);
				}
				if (!AddUniqueIdentity(
					HjsonRoutes,
					HjsonRouteKey(Route),
					TEXT("domain_capability_hjson_route"),
					OutError))
				{
					return false;
				}
				if (Route.bCreate
					&& (Declaration.Initialization.Support
							!= EDomainCapabilitySupport::Supported
						|| !InitializationContracts.Contains(
							Route.InitializationContractId)))
				{
					return Fail(
						OutError,
						TEXT("domain_capability_create_initialization_missing:")
							+ Route.RootTypeIdentity);
				}
			}

			TSet<FString> CompositionTargets;
			for (const HjsonWorkspace::FTargetCompositionContract& Contract :
				Declaration.Composition.Contracts)
			{
				FString ContractError;
				if (!HjsonWorkspace::ValidateTargetCompositionContract(
					Contract,
					ContractError))
				{
					return Fail(
						OutError,
						TEXT("domain_capability_composition_invalid:")
							+ Contract.TargetKind + TEXT(":") + ContractError);
				}
				if (!AddUniqueIdentity(
					CompositionTargets,
					Contract.TargetKind,
					TEXT("domain_capability_composition_target"),
					OutError))
				{
					return false;
				}
			}

			TSet<FString> OperationIds;
			for (const FDomainOperationEntry& Operation :
				Declaration.Operation.Operations)
			{
				if (!AddUniqueIdentity(
					OperationIds,
					Operation.OperationId,
					TEXT("domain_capability_operation"),
					OutError))
				{
					return false;
				}
				if (Operation.TerminalPolicy
					== EDomainOperationTerminalPolicy::Undeclared)
				{
					return Fail(
						OutError,
						TEXT("domain_capability_write_terminal_policy_missing:")
							+ Operation.OperationId);
				}
				if (Operation.ExecutionPolicy
						== EDomainOperationExecutionPolicy::Undeclared
					|| Operation.TransientStatePolicy
						== EDomainTransientStatePolicy::Undeclared)
				{
					return Fail(
						OutError,
						TEXT("domain_capability_operation_state_policy_missing:")
							+ Operation.OperationId);
				}
				if (!Operation.InvocationFactory.IsValid())
				{
					return Fail(
						OutError,
						TEXT("domain_capability_operation_factory_missing:")
							+ Operation.OperationId);
				}
				if (Operation.InvocationFactory->GetOperationId()
					!= Operation.OperationId)
				{
					return Fail(
						OutError,
						TEXT("domain_capability_operation_factory_identity_mismatch:")
							+ Operation.OperationId + TEXT(":")
							+ Operation.InvocationFactory->GetOperationId());
				}
				if (!Operation.bIndependentReadback)
				{
					return Fail(
						OutError,
						TEXT("domain_capability_write_readback_missing:")
							+ Operation.OperationId);
				}
				if (Operation.TransientStatePolicy
						== EDomainTransientStatePolicy::None
					&& !Operation.bPersists)
				{
					return Fail(
						OutError,
						TEXT("domain_capability_write_persistence_missing:")
							+ Operation.OperationId);
				}
				if (Operation.TransientStatePolicy
						== EDomainTransientStatePolicy::RecoverableSession
					&& Operation.bPersists)
				{
					return Fail(
						OutError,
						TEXT("domain_capability_transient_persistence_conflict:")
							+ Operation.OperationId);
				}
			}

			TSet<FString> ReadPortIds;
			for (const TSharedRef<Discovery::IUeAgentStateReadPort>& ReadPort :
				Declaration.Discovery.ReadPorts)
			{
				if (!AddUniqueIdentity(
					ReadPortIds,
					ReadPort->GetPortId(),
					TEXT("domain_capability_read_port"),
					OutError))
				{
					return false;
				}
			}
			TSet<FString> SourceIds;
			TSet<FString> CatalogKinds;
			for (const FDomainDiscoverySource& Source :
				Declaration.Discovery.Sources)
			{
				if (!AddUniqueIdentity(
					SourceIds,
					Source.Source->GetSourceId(),
					TEXT("domain_capability_catalog_source"),
					OutError))
				{
					return false;
				}
				for (const FString& Kind : Source.Source->GetCatalogKinds())
				{
					if (!AddUniqueIdentity(
						CatalogKinds,
						Kind,
						TEXT("domain_capability_catalog_kind"),
						OutError))
					{
						return false;
					}
				}
				if (!ReadPortIds.Contains(Source.ReadPortId))
				{
					return Fail(
						OutError,
						TEXT("domain_capability_candidate_read_port_missing:")
							+ Source.ReadPortId);
				}
			}

			TSet<FString> LayoutProviderIds;
			TSet<FString> LayoutGraphIdentities;
			for (const FDomainLayoutEntry& Layout :
				Declaration.Layout.Layouts)
			{
				if (!AddUniqueIdentity(
					LayoutProviderIds,
					Layout.ProviderId,
					TEXT("domain_capability_layout_provider"),
					OutError)
					|| !AddUniqueIdentity(
						LayoutGraphIdentities,
						Layout.GraphIdentity,
						TEXT("domain_capability_layout_graph"),
						OutError))
				{
					return false;
				}
				if (!IsExactIdentity(Layout.MechanicsId)
					|| !IsExactIdentity(Layout.ReadPortId)
					|| !Layout.bPersists
					|| !Layout.bIndependentReadback)
				{
					return Fail(
						OutError,
						TEXT("domain_capability_layout_readback_missing:")
							+ Layout.GraphIdentity);
				}
			}

			TSet<uint8> StatePlanes;
			for (const StateSemantic::EUeAgentStatePlane Plane :
				Declaration.State.Planes)
			{
				const uint8 Value = static_cast<uint8>(Plane);
				if (StatePlanes.Contains(Value))
				{
					return Fail(
						OutError,
						TEXT("domain_capability_state_plane_conflict:")
							+ FString::FromInt(Value));
				}
				StatePlanes.Add(Value);
			}

			const int32 ActiveCapabilityCount =
				(Declaration.Command.Support
					== EDomainCapabilitySupport::Supported)
				+ (Declaration.Query.Support
					== EDomainCapabilitySupport::Supported)
				+ (Declaration.Discovery.Support
					== EDomainCapabilitySupport::Supported)
				+ (Declaration.Hjson.Support
					== EDomainCapabilitySupport::Supported)
				+ (Declaration.Composition.Support
					== EDomainCapabilitySupport::Supported)
				+ (Declaration.Initialization.Support
					== EDomainCapabilitySupport::Supported)
				+ (Declaration.Operation.Support
					== EDomainCapabilitySupport::Supported)
				+ (Declaration.Layout.Support
					== EDomainCapabilitySupport::Supported)
				+ (Declaration.State.Support
					== EDomainCapabilitySupport::Supported)
				+ (Declaration.Surface.Support
					== EDomainCapabilitySupport::Supported);
			if (Declaration.Migration.Support
					== EDomainCapabilitySupport::Supported
				&& ActiveCapabilityCount == 0)
			{
				return Fail(
					OutError,
					TEXT("domain_capability_migration_owner_without_active_contribution:")
						+ Declaration.Migration.OwnerId);
			}
			if (ActiveCapabilityCount == 0)
			{
				return Fail(
					OutError,
					TEXT("domain_capability_no_supported_capability:")
						+ Declaration.DomainId);
			}
			return true;
		}

		struct FProcessFactoryEntry final
		{
			FString DomainId;
			FDomainCapabilityContributionFactory DomainFactory = nullptr;
			struct FCommandFactoryEntry final
			{
				FString ContributionId;
				Active::FUeAgentActiveCommandContributionFactory Factory = nullptr;
			};
			struct FHjsonFactoryEntry final
			{
				FString ProviderId;
				HjsonWorkspace::FImportInitializationDomainProviderFactory
					Factory = nullptr;
			};
			TArray<FCommandFactoryEntry> CommandFactories;
			TArray<FHjsonFactoryEntry> HjsonFactories;
		};

		struct FProcessCatalog final
		{
			FCriticalSection Mutex;
			bool bSealed = false;
			bool bAuthorityBuildInProgress = false;
			TArray<FProcessFactoryEntry> Entries;
			TArray<FString> Errors;
			TSharedPtr<const FFrozenDomainCapabilitySet> Frozen;
			TSharedPtr<const FDomainCapabilityAuthorities> Authorities;
		};

		FProcessCatalog& ProcessCatalog()
		{
			static FProcessCatalog Catalog;
			return Catalog;
		}

		FProcessFactoryEntry& FindOrAddProcessDomain(
			FProcessCatalog& Catalog,
			const FString& DomainId)
		{
			if (FProcessFactoryEntry* Existing =
				Catalog.Entries.FindByPredicate(
					[&DomainId](const FProcessFactoryEntry& Entry)
					{
						return Entry.DomainId == DomainId;
					}))
			{
				return *Existing;
			}
			FProcessFactoryEntry& Added = Catalog.Entries.AddDefaulted_GetRef();
			Added.DomainId = DomainId;
			return Added;
		}

		bool RegisterProcessFactory(
			const FString& DomainId,
			const FDomainCapabilityContributionFactory Factory)
		{
			FProcessCatalog& Catalog = ProcessCatalog();
			FScopeLock Lock(&Catalog.Mutex);
			if (Catalog.bSealed)
			{
				Catalog.Errors.AddUnique(
					TEXT("domain_capability_process_catalog_sealed:")
						+ DomainId);
				return false;
			}
			if (!IsExactIdentity(DomainId) || !Factory)
			{
				Catalog.Errors.AddUnique(
					TEXT("domain_capability_factory_invalid:") + DomainId);
				return false;
			}
			FProcessFactoryEntry& Entry =
				FindOrAddProcessDomain(Catalog, DomainId);
			if (Entry.DomainFactory)
			{
				Catalog.Errors.AddUnique(
					TEXT("domain_capability_factory_duplicate:") + DomainId);
				return false;
			}
			Entry.DomainFactory = Factory;
			return true;
		}

		bool RegisterProcessCommandFactory(
			const FString& DomainId,
			const FString& ContributionId,
			const Active::FUeAgentActiveCommandContributionFactory Factory)
		{
			FProcessCatalog& Catalog = ProcessCatalog();
			FScopeLock Lock(&Catalog.Mutex);
			if (Catalog.bSealed)
			{
				Catalog.Errors.AddUnique(
					TEXT("domain_capability_process_catalog_sealed:")
						+ DomainId);
				return false;
			}
			if (!IsExactIdentity(DomainId)
				|| !IsExactIdentity(ContributionId)
				|| !Factory)
			{
				Catalog.Errors.AddUnique(
					TEXT("domain_capability_command_factory_invalid:")
						+ DomainId + TEXT(":") + ContributionId);
				return false;
			}
			for (const FProcessFactoryEntry& Entry : Catalog.Entries)
			{
				if (Entry.CommandFactories.ContainsByPredicate(
					[&ContributionId](
						const FProcessFactoryEntry::FCommandFactoryEntry& Item)
					{
						return Item.ContributionId == ContributionId;
					}))
				{
					Catalog.Errors.AddUnique(
						TEXT("domain_capability_command_factory_duplicate:")
							+ ContributionId);
					return false;
				}
			}
			FProcessFactoryEntry::FCommandFactoryEntry Added;
			Added.ContributionId = ContributionId;
			Added.Factory = Factory;
			FindOrAddProcessDomain(Catalog, DomainId).CommandFactories.Add(
				MoveTemp(Added));
			return true;
		}

		bool RegisterProcessHjsonFactory(
			const FString& DomainId,
			const FString& ProviderId,
			const HjsonWorkspace::
				FImportInitializationDomainProviderFactory Factory)
		{
			FProcessCatalog& Catalog = ProcessCatalog();
			FScopeLock Lock(&Catalog.Mutex);
			if (Catalog.bSealed)
			{
				Catalog.Errors.AddUnique(
					TEXT("domain_capability_process_catalog_sealed:")
						+ DomainId);
				return false;
			}
			if (!IsExactIdentity(DomainId)
				|| !IsExactIdentity(ProviderId)
				|| !Factory)
			{
				Catalog.Errors.AddUnique(
					TEXT("domain_capability_hjson_factory_invalid:")
						+ DomainId + TEXT(":") + ProviderId);
				return false;
			}
			for (const FProcessFactoryEntry& Entry : Catalog.Entries)
			{
				if (Entry.HjsonFactories.ContainsByPredicate(
					[&ProviderId](
						const FProcessFactoryEntry::FHjsonFactoryEntry& Item)
					{
						return Item.ProviderId == ProviderId;
					}))
				{
					Catalog.Errors.AddUnique(
						TEXT("domain_capability_hjson_factory_duplicate:")
							+ ProviderId);
					return false;
				}
			}
			FProcessFactoryEntry::FHjsonFactoryEntry Added;
			Added.ProviderId = ProviderId;
			Added.Factory = Factory;
			FindOrAddProcessDomain(Catalog, DomainId).HjsonFactories.Add(
				MoveTemp(Added));
			return true;
		}
	}

	FDomainOperationInvocationError::FDomainOperationInvocationError(
		FString InCode,
		FString InSubjectId,
		FString InDetail)
		: Code(MoveTemp(InCode))
		, SubjectId(MoveTemp(InSubjectId))
		, Detail(MoveTemp(InDetail))
	{
	}

	const FString& FDomainOperationInvocationError::GetCode() const
	{
		return Code;
	}

	const FString& FDomainOperationInvocationError::GetSubjectId() const
	{
		return SubjectId;
	}

	const FString& FDomainOperationInvocationError::GetDetail() const
	{
		return Detail;
	}

	bool FDomainOperationInvocationError::IsPresent() const
	{
		return !Code.IsEmpty();
	}

	bool FDomainOperationInvocation::TryCreate(
		TSharedRef<const Operation::FOperationLifecyclePlan> InPlan,
		TSharedRef<const Operation::FOperationImpactAuthorization>
			InAuthorization,
		TSharedRef<Operation::FOperationLifecycleService> InService,
		TSharedRef<IDomainOperationInvocationContext> InContext,
		FDomainOperationInvocation& OutInvocation,
		FDomainOperationInvocationError& OutError)
	{
		OutInvocation = FDomainOperationInvocation();
		OutError = FDomainOperationInvocationError();
		if (!InPlan->IsValid())
		{
			OutError = FDomainOperationInvocationError(
				TEXT("domain_operation_invocation_plan_invalid"),
				InPlan->GetPlanId());
			return false;
		}
		if (!InAuthorization->IsValid()
			|| !InAuthorization->Authorizes(*InPlan))
		{
			OutError = FDomainOperationInvocationError(
				TEXT("domain_operation_invocation_authorization_invalid"),
				InPlan->GetPlanId());
			return false;
		}
		OutInvocation.Plan = MoveTemp(InPlan);
		OutInvocation.Authorization = MoveTemp(InAuthorization);
		OutInvocation.Service = MoveTemp(InService);
		OutInvocation.Context = MoveTemp(InContext);
		return true;
	}

	bool FDomainOperationInvocation::IsValid() const
	{
		return Plan.IsValid()
			&& Authorization.IsValid()
			&& Service.IsValid()
			&& Context.IsValid()
			&& Plan->IsValid()
			&& Authorization->IsValid()
			&& Authorization->Authorizes(*Plan);
	}

	TSharedRef<const Operation::FOperationLifecyclePlan>
	FDomainOperationInvocation::GetPlan() const
	{
		check(Plan.IsValid());
		return Plan.ToSharedRef();
	}

	TSharedRef<const Operation::FOperationImpactAuthorization>
	FDomainOperationInvocation::GetAuthorization() const
	{
		check(Authorization.IsValid());
		return Authorization.ToSharedRef();
	}

	TSharedRef<Operation::FOperationLifecycleService>
	FDomainOperationInvocation::GetService() const
	{
		check(Service.IsValid());
		return Service.ToSharedRef();
	}

	TSharedRef<IDomainOperationInvocationContext>
	FDomainOperationInvocation::GetContext() const
	{
		check(Context.IsValid());
		return Context.ToSharedRef();
	}

	FDomainDiscoverySource::FDomainDiscoverySource(
		TSharedRef<Discovery::IUeAgentCatalogSource> InSource,
		FString InReadPortId)
		: Source(MoveTemp(InSource))
		, ReadPortId(MoveTemp(InReadPortId))
	{
	}

	FDomainCapabilityDeclaration
	MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
		FString DomainId)
	{
		FDomainCapabilityDeclaration Declaration;
		Declaration.DomainId = MoveTemp(DomainId);
		Declaration.Command.Support = EDomainCapabilitySupport::Unsupported;
		Declaration.Query.Support = EDomainCapabilitySupport::Unsupported;
		Declaration.Discovery.Support = EDomainCapabilitySupport::Unsupported;
		Declaration.Hjson.Support = EDomainCapabilitySupport::Unsupported;
		Declaration.Composition.Support = EDomainCapabilitySupport::Unsupported;
		Declaration.Initialization.Support =
			EDomainCapabilitySupport::Unsupported;
		Declaration.Operation.Support = EDomainCapabilitySupport::Unsupported;
		Declaration.Layout.Support = EDomainCapabilitySupport::Unsupported;
		Declaration.State.Support = EDomainCapabilitySupport::Unsupported;
		Declaration.Surface.Support = EDomainCapabilitySupport::Unsupported;
		Declaration.Migration.Support = EDomainCapabilitySupport::Unsupported;
		return Declaration;
	}

	FDomainCapabilityContribution::FDomainCapabilityContribution(
		FDomainCapabilityDeclaration InDeclaration)
		: Declaration(MoveTemp(InDeclaration))
	{
	}

	bool FDomainCapabilityContribution::TryCreate(
		FDomainCapabilityDeclaration Declaration,
		TSharedPtr<const FDomainCapabilityContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		if (!ValidateDeclaration(Declaration, OutError))
		{
			return false;
		}
		OutContribution = TSharedPtr<const FDomainCapabilityContribution>(
			new FDomainCapabilityContribution(MoveTemp(Declaration)));
		return true;
	}

	const FString& FDomainCapabilityContribution::GetDomainId() const
	{
		return Declaration.DomainId;
	}

	const FDomainCapabilityDeclaration&
	FDomainCapabilityContribution::GetDeclaration() const
	{
		return Declaration;
	}

	const TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>&
	FDomainCapabilityAuthorities::GetCommandRegistry() const
	{
		return CommandRegistry;
	}

	const TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>&
	FDomainCapabilityAuthorities::GetCommandDispatch() const
	{
		return CommandDispatch;
	}

	const TSharedPtr<const HjsonWorkspace::
		FImportInitializationDomainProviderRegistry>&
	FDomainCapabilityAuthorities::GetHjsonRegistry() const
	{
		return HjsonRegistry;
	}

	FFrozenDomainCapabilitySet::FFrozenDomainCapabilitySet(
		TArray<TSharedRef<const FDomainCapabilityContribution>>
			InContributions)
		: Contributions(MoveTemp(InContributions))
	{
	}

	int32 FFrozenDomainCapabilitySet::Num() const
	{
		return Contributions.Num();
	}

	const TArray<TSharedRef<const FDomainCapabilityContribution>>&
	FFrozenDomainCapabilitySet::GetContributions() const
	{
		return Contributions;
	}

	TSharedPtr<const IDomainOperationInvocationFactory>
	FFrozenDomainCapabilitySet::FindOperationFactory(
		const FString& OperationId) const
	{
		for (const TSharedRef<const FDomainCapabilityContribution>&
			Contribution : Contributions)
		{
			for (const FDomainOperationEntry& Entry :
				Contribution->GetDeclaration().Operation.Operations)
			{
				if (Entry.OperationId == OperationId)
				{
					return Entry.InvocationFactory;
				}
			}
		}
		return nullptr;
	}

	bool FFrozenDomainCapabilitySet::TryBuildAuthorities(
		FDomainCapabilityAuthorities& OutAuthorities,
		FString& OutError) const
	{
		OutAuthorities = FDomainCapabilityAuthorities();
		OutError.Reset();
		FDomainCapabilityAuthorities Built;
		TArray<TSharedRef<Active::IUeAgentActiveCommandContribution>>
			CommandContributions;
		TArray<Query::FUeAgentDomainQueryProviderRegistration>
			QueryProviders;
		TArray<TSharedRef<Discovery::IUeAgentCatalogSource>> CatalogSources;
		TArray<TSharedRef<Discovery::IUeAgentStateReadPort>> ReadPorts;
		TArray<TSharedRef<
			HjsonWorkspace::IImportInitializationDomainProvider>>
			HjsonProviders;
		TArray<HjsonWorkspace::FTargetCompositionContract>
			CompositionContracts;

		for (const TSharedRef<const FDomainCapabilityContribution>&
			Contribution : Contributions)
		{
			const FDomainCapabilityDeclaration& Declaration =
				Contribution->GetDeclaration();
			CommandContributions.Append(Declaration.Command.Contributions);
			QueryProviders.Append(Declaration.Query.Providers);
			for (const FDomainDiscoverySource& Source :
				Declaration.Discovery.Sources)
			{
				CatalogSources.Add(Source.Source);
			}
			ReadPorts.Append(Declaration.Discovery.ReadPorts);
			HjsonProviders.Append(Declaration.Hjson.Providers);
			CompositionContracts.Append(Declaration.Composition.Contracts);
		}

		if (!QueryProviders.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("domain_capability_query_projection_not_connected"));
		}
		if (!CatalogSources.IsEmpty() || !ReadPorts.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("domain_capability_discovery_projection_not_connected"));
		}

		if (!HjsonProviders.IsEmpty())
		{
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!HjsonWorkspace::
				TryFreezeImportInitializationDomainProviders(
					HjsonProviders,
					Built.HjsonRegistry,
					WorkspaceError))
			{
				return Fail(
					OutError,
					TEXT("domain_capability_hjson_projection_failed:")
						+ WorkspaceError.GetCode());
			}
		}

		if (!CommandContributions.IsEmpty()
			&& !Active::TryComposeActiveCommandContributions(
				CommandContributions,
				Built.CommandRegistry,
				Built.CommandDispatch,
				OutError))
		{
			return false;
		}

		if (!CompositionContracts.IsEmpty()
			&& !HjsonWorkspace::FTargetCompositionRegistry::Get().
				RegisterContracts(MoveTemp(CompositionContracts), OutError))
		{
			return false;
		}

		OutAuthorities = MoveTemp(Built);
		return true;
	}

	bool FDomainCapabilityComposition::TryAdd(
		TSharedRef<const FDomainCapabilityContribution> Contribution,
		FString& OutError)
	{
		OutError.Reset();
		if (bSealed)
		{
			return Fail(
				OutError,
				TEXT("domain_capability_composition_sealed:")
					+ Contribution->GetDomainId());
		}
		if (DomainIds.Contains(Contribution->GetDomainId()))
		{
			return Fail(
				OutError,
				TEXT("domain_capability_domain_conflict:")
					+ Contribution->GetDomainId());
		}
		DomainIds.Add(Contribution->GetDomainId());
		Contributions.Add(MoveTemp(Contribution));
		return true;
	}

	bool FDomainCapabilityComposition::TryFreeze(
		TSharedPtr<const FFrozenDomainCapabilitySet>& OutFrozen,
		FString& OutError)
	{
		OutFrozen.Reset();
		OutError.Reset();
		if (bSealed)
		{
			return Fail(
				OutError,
				TEXT("domain_capability_composition_sealed:freeze"));
		}
		bSealed = true;
		if (Contributions.IsEmpty())
		{
			return Fail(OutError, TEXT("domain_capability_composition_empty"));
		}

		TSet<FString> CommandContributionIds;
		TSet<FString> CommandIds;
		TSet<FString> QueryBindings;
		TSet<FString> CatalogSourceIds;
		TSet<FString> CatalogKinds;
		TSet<FString> ReadPortIds;
		TSet<FString> HjsonProviderIds;
		TSet<FString> HjsonRouteIds;
		TSet<FString> CompositionTargets;
		TSet<FString> InitializationContracts;
		TSet<FString> OperationIds;
		TSet<FString> LayoutProviderIds;
		TSet<FString> LayoutGraphIdentities;
		TSet<FString> SurfaceIds;

		for (const TSharedRef<const FDomainCapabilityContribution>&
			Contribution : Contributions)
		{
			const FDomainCapabilityDeclaration& Declaration =
				Contribution->GetDeclaration();
			for (const TSharedRef<Active::IUeAgentActiveCommandContribution>&
				CommandContribution : Declaration.Command.Contributions)
			{
				if (!AddUniqueIdentity(
					CommandContributionIds,
					CommandContribution->GetContributionId(),
					TEXT("domain_capability_command_contribution"),
					OutError))
				{
					return false;
				}
				TArray<CommandContract::FUeAgentCommandDeclaration>
					Declarations;
				CommandContribution->CollectDeclarations(Declarations);
				for (const CommandContract::FUeAgentCommandDeclaration& Command :
					Declarations)
				{
					if (!AddUniqueIdentity(
						CommandIds,
						Command.CommandId,
						TEXT("domain_capability_command_id"),
						OutError))
					{
						return false;
					}
				}
			}
			for (const Query::FUeAgentDomainQueryProviderRegistration& Provider :
				Declaration.Query.Providers)
			{
				if (!AddUniqueIdentity(
					QueryBindings,
					Provider.GetProviderBinding(),
					TEXT("domain_capability_query_binding"),
					OutError))
				{
					return false;
				}
			}
			for (const FDomainDiscoverySource& Source :
				Declaration.Discovery.Sources)
			{
				if (!AddUniqueIdentity(
					CatalogSourceIds,
					Source.Source->GetSourceId(),
					TEXT("domain_capability_catalog_source"),
					OutError))
				{
					return false;
				}
				for (const FString& Kind : Source.Source->GetCatalogKinds())
				{
					if (!AddUniqueIdentity(
						CatalogKinds,
						Kind,
						TEXT("domain_capability_catalog_kind"),
						OutError))
					{
						return false;
					}
				}
			}
			for (const TSharedRef<Discovery::IUeAgentStateReadPort>& ReadPort :
				Declaration.Discovery.ReadPorts)
			{
				if (!AddUniqueIdentity(
					ReadPortIds,
					ReadPort->GetPortId(),
					TEXT("domain_capability_read_port"),
					OutError))
				{
					return false;
				}
			}
			for (const TSharedRef<
				HjsonWorkspace::IImportInitializationDomainProvider>& Provider :
				Declaration.Hjson.Providers)
			{
				if (!AddUniqueIdentity(
					HjsonProviderIds,
					Provider->GetProviderId(),
					TEXT("domain_capability_hjson_provider"),
					OutError))
				{
					return false;
				}
			}
			for (const FDomainHjsonRoute& Route : Declaration.Hjson.Routes)
			{
				if (!AddUniqueIdentity(
					HjsonRouteIds,
					HjsonRouteKey(Route),
					TEXT("domain_capability_hjson_route"),
					OutError))
				{
					return false;
				}
			}
			for (const HjsonWorkspace::FTargetCompositionContract& Contract :
				Declaration.Composition.Contracts)
			{
				if (!AddUniqueIdentity(
					CompositionTargets,
					Contract.TargetKind,
					TEXT("domain_capability_composition_target"),
					OutError))
				{
					return false;
				}
			}
			for (const FString& ContractId :
				Declaration.Initialization.ContractIds)
			{
				if (!AddUniqueIdentity(
					InitializationContracts,
					ContractId,
					TEXT("domain_capability_initialization_contract"),
					OutError))
				{
					return false;
				}
			}
			for (const FDomainOperationEntry& Operation :
				Declaration.Operation.Operations)
			{
				if (!AddUniqueIdentity(
					OperationIds,
					Operation.OperationId,
					TEXT("domain_capability_operation"),
					OutError))
				{
					return false;
				}
			}
			for (const FDomainLayoutEntry& Layout : Declaration.Layout.Layouts)
			{
				if (!AddUniqueIdentity(
					LayoutProviderIds,
					Layout.ProviderId,
					TEXT("domain_capability_layout_provider"),
					OutError)
					|| !AddUniqueIdentity(
						LayoutGraphIdentities,
						Layout.GraphIdentity,
						TEXT("domain_capability_layout_graph"),
						OutError))
				{
					return false;
				}
			}
			for (const FDomainSemanticSurfaceEntry& Surface :
				Declaration.Surface.Surfaces)
			{
				if (!AddUniqueIdentity(
					SurfaceIds,
					Surface.SurfaceId,
					TEXT("domain_capability_surface"),
					OutError))
				{
					return false;
				}
			}
		}

		TArray<TSharedRef<const FDomainCapabilityContribution>> Sorted =
			Contributions;
		Sorted.Sort(
			[](const TSharedRef<const FDomainCapabilityContribution>& Left,
				const TSharedRef<const FDomainCapabilityContribution>& Right)
			{
				return Left->GetDomainId() < Right->GetDomainId();
			});
		OutFrozen = TSharedPtr<const FFrozenDomainCapabilitySet>(
			new FFrozenDomainCapabilitySet(MoveTemp(Sorted)));
		return true;
	}

	bool FDomainCapabilityComposition::IsSealed() const
	{
		return bSealed;
	}

	FDomainCapabilityContributionFactoryRegistration::
		FDomainCapabilityContributionFactoryRegistration(
			const TCHAR* DomainId,
			const FDomainCapabilityContributionFactory Factory)
	{
		RegisterProcessFactory(
			DomainId ? FString(DomainId) : FString(),
			Factory);
	}

	bool RegisterProcessDomainCommandCapabilityFactory(
		const FString& DomainId,
		const FString& ContributionId,
		const Active::FUeAgentActiveCommandContributionFactory Factory)
	{
		return RegisterProcessCommandFactory(
			DomainId,
			ContributionId,
			Factory);
	}

	bool RegisterProcessDomainHjsonCapabilityFactory(
		const FString& DomainId,
		const FString& ProviderId,
		const HjsonWorkspace::
			FImportInitializationDomainProviderFactory Factory)
	{
		return RegisterProcessHjsonFactory(DomainId, ProviderId, Factory);
	}

	bool TryFreezeProcessDomainCapabilities(
		TSharedPtr<const FFrozenDomainCapabilitySet>& OutFrozen,
		FString& OutError)
	{
		OutFrozen.Reset();
		OutError.Reset();
		TArray<FProcessFactoryEntry> Entries;
		{
			FProcessCatalog& Catalog = ProcessCatalog();
			FScopeLock Lock(&Catalog.Mutex);
			if (Catalog.Frozen.IsValid())
			{
				OutFrozen = Catalog.Frozen;
				return true;
			}
			Catalog.bSealed = true;
			if (!Catalog.Errors.IsEmpty())
			{
				OutError = Catalog.Errors[0];
				return false;
			}
			Entries = Catalog.Entries;
		}
		Entries.Sort(
			[](const FProcessFactoryEntry& Left,
				const FProcessFactoryEntry& Right)
			{
				return Left.DomainId < Right.DomainId;
			});
		FDomainCapabilityComposition Composition;
		for (const FProcessFactoryEntry& Entry : Entries)
		{
			FDomainCapabilityDeclaration Declaration =
				MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
					Entry.DomainId);
			if (Entry.DomainFactory)
			{
				TSharedPtr<const FDomainCapabilityContribution> BaseContribution;
				FString FactoryError;
				if (!Entry.DomainFactory(BaseContribution, FactoryError)
					|| !BaseContribution.IsValid())
				{
					return Fail(
						OutError,
						TEXT("domain_capability_factory_failed:")
							+ Entry.DomainId + TEXT(":") + FactoryError);
				}
				if (BaseContribution->GetDomainId() != Entry.DomainId)
				{
					return Fail(
						OutError,
						TEXT("domain_capability_factory_identity_mismatch:")
							+ Entry.DomainId + TEXT(":")
							+ BaseContribution->GetDomainId());
				}
				Declaration = BaseContribution->GetDeclaration();
			}

			TArray<FProcessFactoryEntry::FCommandFactoryEntry> CommandFactories =
				Entry.CommandFactories;
			CommandFactories.Sort(
				[](const FProcessFactoryEntry::FCommandFactoryEntry& Left,
				   const FProcessFactoryEntry::FCommandFactoryEntry& Right)
				{
					return Left.ContributionId < Right.ContributionId;
				});
			for (const FProcessFactoryEntry::FCommandFactoryEntry& CommandFactory :
				CommandFactories)
			{
				TSharedPtr<Active::IUeAgentActiveCommandContribution>
					CommandContribution;
				FString FactoryError;
				if (!CommandFactory.Factory(CommandContribution, FactoryError)
					|| !CommandContribution.IsValid())
				{
					return Fail(
						OutError,
						TEXT("domain_capability_command_factory_failed:")
							+ CommandFactory.ContributionId + TEXT(":")
							+ FactoryError);
				}
				if (CommandContribution->GetContributionId()
					!= CommandFactory.ContributionId)
				{
					return Fail(
						OutError,
						TEXT("domain_capability_command_factory_identity_mismatch:")
							+ CommandFactory.ContributionId + TEXT(":")
							+ CommandContribution->GetContributionId());
				}
				if (Declaration.Command.Contributions.ContainsByPredicate(
					[&CommandFactory](
						const TSharedRef<Active::
							IUeAgentActiveCommandContribution>& Existing)
					{
						return Existing->GetContributionId()
							== CommandFactory.ContributionId;
					}))
				{
					return Fail(
						OutError,
						TEXT("domain_capability_command_factory_duplicate:")
							+ CommandFactory.ContributionId);
				}
				Declaration.Command.Support = EDomainCapabilitySupport::Supported;
				Declaration.Command.Contributions.Add(
					CommandContribution.ToSharedRef());
			}

			TArray<FProcessFactoryEntry::FHjsonFactoryEntry> HjsonFactories =
				Entry.HjsonFactories;
			HjsonFactories.Sort(
				[](const FProcessFactoryEntry::FHjsonFactoryEntry& Left,
				   const FProcessFactoryEntry::FHjsonFactoryEntry& Right)
				{
					return Left.ProviderId < Right.ProviderId;
				});
			for (const FProcessFactoryEntry::FHjsonFactoryEntry& HjsonFactory :
				HjsonFactories)
			{
				TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
					Provider;
				HjsonWorkspace::FWorkspaceError FactoryError;
				if (!HjsonFactory.Factory(Provider, FactoryError)
					|| !Provider.IsValid())
				{
					return Fail(
						OutError,
						TEXT("domain_capability_hjson_factory_failed:")
							+ HjsonFactory.ProviderId + TEXT(":")
							+ FactoryError.GetCode());
				}
				if (Provider->GetProviderId() != HjsonFactory.ProviderId)
				{
					return Fail(
						OutError,
						TEXT("domain_capability_hjson_factory_identity_mismatch:")
							+ HjsonFactory.ProviderId + TEXT(":")
							+ Provider->GetProviderId());
				}
				if (Declaration.Hjson.Providers.ContainsByPredicate(
					[&HjsonFactory](
						const TSharedRef<HjsonWorkspace::
							IImportInitializationDomainProvider>& Existing)
					{
						return Existing->GetProviderId()
							== HjsonFactory.ProviderId;
					}))
				{
					return Fail(
						OutError,
						TEXT("domain_capability_hjson_factory_duplicate:")
							+ HjsonFactory.ProviderId);
				}
				Declaration.Hjson.Support = EDomainCapabilitySupport::Supported;
				Declaration.Hjson.Providers.Add(Provider.ToSharedRef());
			}

			TSharedPtr<const FDomainCapabilityContribution> Contribution;
			if (!FDomainCapabilityContribution::TryCreate(
					MoveTemp(Declaration),
					Contribution,
					OutError))
			{
				return false;
			}
			if (!Composition.TryAdd(Contribution.ToSharedRef(), OutError))
			{
				return false;
			}
		}
		TSharedPtr<const FFrozenDomainCapabilitySet> BuiltFrozen;
		if (!Composition.TryFreeze(BuiltFrozen, OutError))
		{
			return false;
		}
		FProcessCatalog& Catalog = ProcessCatalog();
		FScopeLock Lock(&Catalog.Mutex);
		if (!Catalog.Frozen.IsValid())
		{
			Catalog.Frozen = BuiltFrozen;
		}
		OutFrozen = Catalog.Frozen;
		return true;
	}

	bool TryGetOrBuildProcessDomainCapabilityAuthorities(
		FDomainCapabilityAuthorities& OutAuthorities,
		FString& OutError)
	{
		OutAuthorities = FDomainCapabilityAuthorities();
		OutError.Reset();
		{
			FProcessCatalog& Catalog = ProcessCatalog();
			FScopeLock Lock(&Catalog.Mutex);
			if (Catalog.Authorities.IsValid())
			{
				OutAuthorities = *Catalog.Authorities;
				return true;
			}
			if (Catalog.bAuthorityBuildInProgress)
			{
				return Fail(
					OutError,
					TEXT("domain_capability_process_authority_reentrant"));
			}
			Catalog.bAuthorityBuildInProgress = true;
		}

		TSharedPtr<const FFrozenDomainCapabilitySet> Frozen;
		FDomainCapabilityAuthorities BuiltAuthorities;
		const bool bBuilt = TryFreezeProcessDomainCapabilities(
				Frozen,
				OutError)
			&& Frozen.IsValid()
			&& Frozen->TryBuildAuthorities(BuiltAuthorities, OutError);

		FProcessCatalog& Catalog = ProcessCatalog();
		FScopeLock Lock(&Catalog.Mutex);
		Catalog.bAuthorityBuildInProgress = false;
		if (!bBuilt)
		{
			return false;
		}
		if (!Catalog.Authorities.IsValid())
		{
			Catalog.Authorities =
				MakeShared<const FDomainCapabilityAuthorities>(BuiltAuthorities);
		}
		OutAuthorities = *Catalog.Authorities;
		return true;
	}
}
