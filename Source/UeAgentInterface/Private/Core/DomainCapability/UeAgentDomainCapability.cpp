#include "Core/DomainCapability/UeAgentDomainCapability.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/BindingGateway/Query/UeAgentQueryBindingError.h"
#include "Dom/JsonValue.h"
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

		Contract::FDiagnostic InvalidDeclaration(const FString& Code, const FString& Reason, const FString& Path)
		{
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("domain_capability"));
			Error.PropertyPath = Path;
			return Error;
		}

		template <typename T>
		FString ItemField(const TArray<T>& Items, const T& Item, const FString& Collection, const FString& Field)
		{
			const FString Path = FString::Printf(TEXT("%s[%d]"), *Collection, static_cast<int32>(&Item - Items.GetData()));
			return Field.IsEmpty() ? Path : Path + TEXT(".") + Field;
		}

		bool Fail(Contract::FDiagnostic& OutError, const FString& Code, const FString& Reason,
			const FString& Path, const FString& InternalIdentity = FString())
		{
			OutError = InvalidDeclaration(Code, Reason, Path);
			OutError.SubjectId = InternalIdentity;
			return false;
		}

		Contract::FDiagnostic RegistrationFailure(const TCHAR* Code, const FString& Reason,
			const FString& Path, const FString& DomainId, const FString& ContributionId = FString())
		{
			auto Error = InvalidDeclaration(Code, Reason, Path);
			Error.Stage = TEXT("registration");
			Error.CandidateName = DomainId;
			Error.SubjectId = ContributionId;
			return Error;
		}

		bool ValidateSupport(
			const FString& DomainId,
			const TCHAR* Category,
			const EDomainCapabilitySupport Support,
			const bool bHasPayload,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (Support == EDomainCapabilitySupport::Undeclared)
			{
				return Fail(
					OutError,
					TEXT("domain_capability_support_undeclared"),
					TEXT("The domain must explicitly declare whether this capability is supported."),
					FString(Category) + TEXT(".support"), DomainId);
			}
			if (Support == EDomainCapabilitySupport::Unsupported
				&& bHasPayload)
			{
				return Fail(
					OutError,
					TEXT("domain_capability_unsupported_payload"),
					TEXT("The domain declares this capability unsupported but supplies capability entries."), Category, DomainId);
			}
			if (Support == EDomainCapabilitySupport::Supported
				&& !bHasPayload)
			{
				return Fail(
					OutError,
					TEXT("domain_capability_supported_payload_missing"),
					TEXT("The domain declares this capability supported but supplies no capability entries."), Category, DomainId);
			}
			return true;
		}

		bool AddUniqueIdentity(
			TSet<FString>& Seen,
			const FString& Identity,
			const TCHAR* ErrorPrefix,
			Contract::FDiagnostic& OutError,
			const FString& Path,
			const bool bPublicIdentity)
		{
			if (!IsExactIdentity(Identity))
			{
				OutError = InvalidDeclaration(FString(ErrorPrefix) + TEXT("_invalid"),
					TEXT("The declared identifier must be non-empty and have no surrounding whitespace."), Path);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty text without surrounding whitespace"));
			}
			else if (Seen.Contains(Identity))
			{
				OutError = InvalidDeclaration(FString(ErrorPrefix) + TEXT("_conflict"),
					TEXT("More than one capability entry declares the same identifier."), Path);
			}
			else
			{
				Seen.Add(Identity);
				return true;
			}
			if (bPublicIdentity) OutError.ActualValue = MakeShared<FJsonValueString>(Identity);
			else OutError.ActualEvidence = MakeShared<FJsonValueString>(Identity);
			return false;
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			if (!IsExactIdentity(Declaration.DomainId))
			{
				return Fail(OutError, TEXT("domain_capability_domain_id_invalid"),
					TEXT("The domain identifier must be non-empty and have no surrounding whitespace."), TEXT("domain_id"), Declaration.DomainId);
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
						+ Declaration.Discovery.ReadPorts.Num()
						+ Declaration.Discovery.ContentProviders.Num() > 0,
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
					OutError, ItemField(Declaration.Command.Contributions, Contribution, TEXT("command.contributions"), TEXT("contribution_id")), false))
				{
					return false;
				}
				TArray<CommandContract::FUeAgentCommandDeclaration>
					Declarations;
				Contribution->CollectDeclarations(Declarations);
				if (Declarations.IsEmpty())
				{
					return Fail(OutError, TEXT("domain_capability_command_declaration_missing"),
					TEXT("The command contribution supplies no command declarations."), ItemField(Declaration.Command.Contributions, Contribution, TEXT("command.contributions"), TEXT("commands")), Contribution->GetContributionId());
				}
				for (const CommandContract::FUeAgentCommandDeclaration& Command :
					Declarations)
				{
					if (!AddUniqueIdentity(
						CommandIds,
						Command.CommandId,
						TEXT("domain_capability_command_id"),
						OutError, ItemField(Declarations, Command, ItemField(Declaration.Command.Contributions, Contribution, TEXT("command.contributions"), TEXT("commands")), TEXT("command_id")), true))
					{
						return false;
					}
				}
			}

			TSet<FString> QueryBindings;
			for (const Query::FUeAgentDomainQueryProviderRegistration& Provider :
				Declaration.Query.Providers)
			{
				if (!Provider.IsValid())
				{
					OutError = Provider.GetValidationDiagnostic();
					OutError.PropertyPath = ItemField(Declaration.Query.Providers, Provider, TEXT("query.providers"), OutError.PropertyPath);
					return false;
				}
				if (!AddUniqueIdentity(
						QueryBindings,
						Provider.GetProviderBinding(),
						TEXT("domain_capability_query_binding"),
						OutError, ItemField(Declaration.Query.Providers, Provider, TEXT("query.providers"), TEXT("provider_binding")), false))
				{
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
					OutError, ItemField(Declaration.Surface.Surfaces, Surface, TEXT("surface.surfaces"), TEXT("surface_id")), false))
				{
					return false;
				}
				if (!IsExactIdentity(Surface.MechanicsId))
				{
					return Fail(OutError, TEXT("domain_capability_surface_mechanics_missing"),
					TEXT("The semantic surface requires a non-empty mechanics identifier without surrounding whitespace."), ItemField(Declaration.Surface.Surfaces, Surface, TEXT("surface.surfaces"), TEXT("mechanics_id")), Surface.SurfaceId);
				}
				if (!IsExactIdentity(Surface.ReadPortId))
				{
					return Fail(OutError, TEXT("domain_capability_surface_readback_missing"),
					TEXT("The semantic surface requires a non-empty read port identifier without surrounding whitespace."), ItemField(Declaration.Surface.Surfaces, Surface, TEXT("surface.surfaces"), TEXT("read_port_id")), Surface.SurfaceId);
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
					OutError, ItemField(Declaration.Initialization.ContractIds, ContractId, TEXT("initialization.contract_ids"), TEXT("")), false))
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
					OutError, ItemField(Declaration.Hjson.Providers, Provider, TEXT("hjson.providers"), TEXT("provider_id")), false))
				{
					return false;
				}
			}
			TSet<FString> HjsonRoutes;
			for (const FDomainHjsonRoute& Route : Declaration.Hjson.Routes)
			{
				if (!HjsonProviderIds.Contains(Route.ProviderId))
				{
					return Fail(OutError, TEXT("domain_capability_hjson_route_invalid"),
						TEXT("The HJSON route refers to a provider that is not registered in this domain."), ItemField(Declaration.Hjson.Routes, Route, TEXT("hjson.routes"), TEXT("provider_id")), Route.ProviderId);
				}
				const TPair<const TCHAR*, const FString*> RouteFields[] = {
					{TEXT("command_id"), &Route.CommandId}, {TEXT("schema_id"), &Route.SchemaId},
					{TEXT("target_kind"), &Route.TargetKind}, {TEXT("root_type_identity"), &Route.RootTypeIdentity}};
				for (const auto& Field : RouteFields)
				{
					if (!IsExactIdentity(*Field.Value))
					{
						OutError = InvalidDeclaration(TEXT("domain_capability_hjson_route_invalid"),
							TEXT("The HJSON route field must be non-empty and have no surrounding whitespace."), ItemField(Declaration.Hjson.Routes, Route, TEXT("hjson.routes"), Field.Key));
						OutError.ActualValue = MakeShared<FJsonValueString>(*Field.Value);
						return false;
					}
				}
				if (!AddUniqueIdentity(
					HjsonRoutes,
					HjsonRouteKey(Route),
					TEXT("domain_capability_hjson_route"),
					OutError, ItemField(Declaration.Hjson.Routes, Route, TEXT("hjson.routes"), TEXT("")), false))
				{
					return false;
				}
				if (Route.bCreate
					&& (Declaration.Initialization.Support
							!= EDomainCapabilitySupport::Supported
						|| !InitializationContracts.Contains(
							Route.InitializationContractId)))
				{
					return Fail(OutError, TEXT("domain_capability_create_initialization_missing"),
					TEXT("A create route requires supported initialization and a registered initialization contract."), ItemField(Declaration.Hjson.Routes, Route, TEXT("hjson.routes"), TEXT("initialization_contract_id")), Route.InitializationContractId);
				}
			}

			TSet<FString> CompositionTargets;
			for (const HjsonWorkspace::FTargetCompositionContract& Contract :
				Declaration.Composition.Contracts)
			{
				UE::UAI::Contract::FDiagnostic ContractError;
				if (!HjsonWorkspace::ValidateTargetCompositionContract(
					Contract,
					ContractError))
				{
					OutError = ContractError; return false;
				}
				if (!AddUniqueIdentity(
					CompositionTargets,
					Contract.TargetKind,
					TEXT("domain_capability_composition_target"),
					OutError, ItemField(Declaration.Composition.Contracts, Contract, TEXT("composition.contracts"), TEXT("target_kind")), true))
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
					OutError, ItemField(Declaration.Operation.Operations, Operation, TEXT("operation.operations"), TEXT("operation_id")), false))
				{
					return false;
				}
				if (Operation.TerminalPolicy
					== EDomainOperationTerminalPolicy::Undeclared)
				{
					return Fail(OutError, TEXT("domain_capability_write_terminal_policy_missing"),
					TEXT("The operation must declare its terminal outcome policy."), ItemField(Declaration.Operation.Operations, Operation, TEXT("operation.operations"), TEXT("terminal_policy")), Operation.OperationId);
				}
				if (Operation.ExecutionPolicy
						== EDomainOperationExecutionPolicy::Undeclared
					|| Operation.TransientStatePolicy
						== EDomainTransientStatePolicy::Undeclared)
				{
					return Fail(OutError, TEXT("domain_capability_operation_state_policy_missing"),
						Operation.ExecutionPolicy == EDomainOperationExecutionPolicy::Undeclared
							? TEXT("The operation must declare its execution policy.") : TEXT("The operation must declare its transient state policy."),
						Operation.ExecutionPolicy == EDomainOperationExecutionPolicy::Undeclared
							? ItemField(Declaration.Operation.Operations, Operation, TEXT("operation.operations"), TEXT("execution_policy")) : ItemField(Declaration.Operation.Operations, Operation, TEXT("operation.operations"), TEXT("transient_state_policy")), Operation.OperationId);
				}
				if (!Operation.InvocationFactory.IsValid())
				{
					return Fail(OutError, TEXT("domain_capability_operation_factory_missing"),
					TEXT("The operation has no invocation factory."), ItemField(Declaration.Operation.Operations, Operation, TEXT("operation.operations"), TEXT("invocation_factory")), Operation.OperationId);
				}
				if (Operation.InvocationFactory->GetOperationId()
					!= Operation.OperationId)
				{
					OutError = InvalidDeclaration(TEXT("domain_capability_operation_factory_identity_mismatch"),
						TEXT("The invocation factory identifies a different operation from its declaration."), ItemField(Declaration.Operation.Operations, Operation, TEXT("operation.operations"), TEXT("invocation_factory.operation_id")));
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Operation.InvocationFactory->GetOperationId());
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Operation.OperationId);
					return false;
				}
				if (!Operation.bIndependentReadback)
				{
					return Fail(OutError, TEXT("domain_capability_write_readback_missing"),
					TEXT("The operation must declare independent readback or observation."), ItemField(Declaration.Operation.Operations, Operation, TEXT("operation.operations"), TEXT("independent_readback")), Operation.OperationId);
				}
				if (Operation.TransientStatePolicy
						== EDomainTransientStatePolicy::None
					&& !Operation.bPersists)
				{
					return Fail(OutError, TEXT("domain_capability_write_persistence_missing"),
					TEXT("An operation without transient state must provide durable persistence."), ItemField(Declaration.Operation.Operations, Operation, TEXT("operation.operations"), TEXT("persists")), Operation.OperationId);
				}
				if (Operation.TransientStatePolicy
						== EDomainTransientStatePolicy::RecoverableSession
					&& Operation.bPersists)
				{
					return Fail(OutError, TEXT("domain_capability_transient_persistence_conflict"),
					TEXT("A recoverable session operation cannot also declare durable persistence."), ItemField(Declaration.Operation.Operations, Operation, TEXT("operation.operations"), TEXT("persists")), Operation.OperationId);
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
					OutError, ItemField(Declaration.Discovery.ReadPorts, ReadPort, TEXT("discovery.read_ports"), TEXT("port_id")), false))
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
					OutError, ItemField(Declaration.Discovery.Sources, Source, TEXT("discovery.sources"), TEXT("source_id")), false))
				{
					return false;
				}
				for (const FString& Kind : Source.Source->GetCatalogKinds())
				{
					if (!AddUniqueIdentity(
						CatalogKinds,
						Kind,
						TEXT("domain_capability_catalog_kind"),
						OutError, ItemField(Source.Source->GetCatalogKinds(), Kind, ItemField(Declaration.Discovery.Sources, Source, TEXT("discovery.sources"), TEXT("catalog_kinds")), FString()), true))
					{
						return false;
					}
				}
				if (!ReadPortIds.Contains(Source.ReadPortId))
				{
					return Fail(OutError, TEXT("domain_capability_candidate_read_port_missing"),
					TEXT("The discovery source refers to a read port that is not registered in this domain."), ItemField(Declaration.Discovery.Sources, Source, TEXT("discovery.sources"), TEXT("read_port_id")), Source.ReadPortId);
				}
			}
			TSet<FString> ContentProviderIds;
			for (const TSharedRef<ContentDiscovery::
				IUeAgentContentDiscoveryProvider>& Provider :
				Declaration.Discovery.ContentProviders)
			{
				if (!AddUniqueIdentity(
						ContentProviderIds,
						Provider->GetProviderId(),
						TEXT("domain_capability_content_discovery_provider"),
						OutError, ItemField(Declaration.Discovery.ContentProviders, Provider, TEXT("discovery.content_providers"), TEXT("provider_id")), false)
					|| Provider->GetContentKinds().IsEmpty())
				{
					if (OutError.IsEmpty())
					{
						Fail(OutError, TEXT("domain_capability_content_discovery_kind_missing"),
							TEXT("The content discovery provider must declare at least one supported content kind."), ItemField(Declaration.Discovery.ContentProviders, Provider, TEXT("discovery.content_providers"), TEXT("content_kinds")), Provider->GetProviderId());
					}
					return false;
				}
				TSet<FString> ProviderKinds;
				for (const FString& Kind : Provider->GetContentKinds())
				{
					if (!AddUniqueIdentity(
							ProviderKinds,
							Kind,
							TEXT("domain_capability_content_discovery_kind"),
							OutError, ItemField(Provider->GetContentKinds(), Kind, ItemField(Declaration.Discovery.ContentProviders, Provider, TEXT("discovery.content_providers"), TEXT("content_kinds")), FString()), true))
					{
						return false;
					}
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
					OutError, ItemField(Declaration.Layout.Layouts, Layout, TEXT("layout.layouts"), TEXT("provider_id")), false)
					|| !AddUniqueIdentity(
						LayoutGraphIdentities,
						Layout.GraphIdentity,
						TEXT("domain_capability_layout_graph"),
						OutError, ItemField(Declaration.Layout.Layouts, Layout, TEXT("layout.layouts"), TEXT("graph_identity")), false))
				{
					return false;
				}
				if (!IsExactIdentity(Layout.MechanicsId))
					return Fail(OutError, TEXT("domain_capability_layout_readback_missing"), TEXT("The layout requires a non-empty mechanics identifier without surrounding whitespace."), ItemField(Declaration.Layout.Layouts, Layout, TEXT("layout.layouts"), TEXT("mechanics_id")), Layout.GraphIdentity);
				if (!IsExactIdentity(Layout.ReadPortId))
					return Fail(OutError, TEXT("domain_capability_layout_readback_missing"), TEXT("The layout requires a non-empty read port identifier without surrounding whitespace."), ItemField(Declaration.Layout.Layouts, Layout, TEXT("layout.layouts"), TEXT("read_port_id")), Layout.GraphIdentity);
				if (!Layout.bPersists)
					return Fail(OutError, TEXT("domain_capability_layout_readback_missing"), TEXT("The layout must declare durable persistence."), ItemField(Declaration.Layout.Layouts, Layout, TEXT("layout.layouts"), TEXT("persists")), Layout.GraphIdentity);
				if (!Layout.bIndependentReadback)
					return Fail(OutError, TEXT("domain_capability_layout_readback_missing"), TEXT("The layout must declare independent readback."), ItemField(Declaration.Layout.Layouts, Layout, TEXT("layout.layouts"), TEXT("independent_readback")), Layout.GraphIdentity);
			}

			TSet<uint8> StatePlanes;
			for (const StateSemantic::EUeAgentStatePlane& Plane :
				Declaration.State.Planes)
			{
				const uint8 Value = static_cast<uint8>(Plane);
				if (StatePlanes.Contains(Value))
				{
					OutError = InvalidDeclaration(TEXT("domain_capability_state_plane_conflict"),
						TEXT("The same state plane is declared more than once."), ItemField(Declaration.State.Planes, Plane, TEXT("state.planes"), FString()));
					OutError.ActualValue = MakeShared<FJsonValueNumber>(Value);
					return false;
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
			if (ActiveCapabilityCount == 0)
			{
				return Fail(OutError, TEXT("domain_capability_no_supported_capability"),
					TEXT("The domain declares no supported capability."), TEXT("support"), Declaration.DomainId);
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
			TArray<FCommandFactoryEntry> CommandFactories;
		};

		struct FProcessCatalog final
		{
			FCriticalSection Mutex;
			bool bSealed = false;
			bool bAuthorityBuildInProgress = false;
			TArray<FProcessFactoryEntry> Entries;
			TArray<UE::UAI::Contract::FDiagnostic> Errors;
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
				Catalog.Errors.Add(RegistrationFailure(TEXT("domain_capability_process_catalog_sealed"), TEXT("The process capability catalog is sealed and cannot accept another factory."), TEXT("factories"), DomainId));
				return false;
			}
			if (!IsExactIdentity(DomainId) || !Factory)
			{
				Catalog.Errors.Add(RegistrationFailure(TEXT("domain_capability_factory_invalid"),
					!IsExactIdentity(DomainId) ? TEXT("The factory domain identifier must be non-empty and have no surrounding whitespace.") : TEXT("The domain factory callback is missing."),
					!IsExactIdentity(DomainId) ? TEXT("domain_id") : TEXT("factory"), DomainId));
				return false;
			}
			FProcessFactoryEntry& Entry =
				FindOrAddProcessDomain(Catalog, DomainId);
			if (Entry.DomainFactory)
			{
				Catalog.Errors.Add(RegistrationFailure(TEXT("domain_capability_factory_duplicate"), TEXT("A factory is already registered for this domain."), TEXT("factory"), DomainId));
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
				Catalog.Errors.Add(RegistrationFailure(TEXT("domain_capability_process_catalog_sealed"), TEXT("The process capability catalog is sealed and cannot accept another command factory."), TEXT("command.factories"), DomainId, ContributionId));
				return false;
			}
			if (!IsExactIdentity(DomainId)
				|| !IsExactIdentity(ContributionId)
				|| !Factory)
			{
				Catalog.Errors.Add(RegistrationFailure(TEXT("domain_capability_command_factory_invalid"),
					!IsExactIdentity(DomainId) ? TEXT("The command factory domain identifier must be non-empty and have no surrounding whitespace.")
					: !IsExactIdentity(ContributionId) ? TEXT("The command contribution identifier must be non-empty and have no surrounding whitespace.")
					: TEXT("The command factory callback is missing."),
					!IsExactIdentity(DomainId) ? TEXT("domain_id") : !IsExactIdentity(ContributionId) ? TEXT("command.factories.contribution_id") : TEXT("command.factories.callback"), DomainId, ContributionId));
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
					Catalog.Errors.Add(RegistrationFailure(TEXT("domain_capability_command_factory_duplicate"), TEXT("A command factory is already registered for this contribution."), TEXT("command.factories.contribution_id"), DomainId, ContributionId));
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
			OutError = InPlan->GetValidationDiagnostic();
			return false;
		}
		if (!InAuthorization->TryAuthorize(*InPlan, OutError))
		{
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = InPlan->GetPlanId();
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		if (!ValidateDeclaration(Declaration, OutError))
		{
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Declaration.DomainId;
			if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("domain_capability_declaration");
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

	const TSharedPtr<const BindingGateway::Query::
		FUeAgentDomainQueryProviderRegistry>&
	FDomainCapabilityAuthorities::GetQueryProviderRegistry() const
	{
		return QueryProviderRegistry;
	}

	const TSharedPtr<const Discovery::FUeAgentDiscoveryService>&
	FDomainCapabilityAuthorities::GetDiscoveryService() const
	{
		return DiscoveryService;
	}

	const TSharedPtr<const ContentDiscovery::
		FUeAgentContentDiscoveryService>&
	FDomainCapabilityAuthorities::GetContentDiscoveryService() const
	{
		return ContentDiscoveryService;
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
		UE::UAI::Contract::FDiagnostic& OutError) const
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
		TArray<TSharedRef<ContentDiscovery::
			IUeAgentContentDiscoveryProvider>> ContentProviders;
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
			ContentProviders.Append(
				Declaration.Discovery.ContentProviders);
			HjsonProviders.Append(Declaration.Hjson.Providers);
			CompositionContracts.Append(Declaration.Composition.Contracts);
		}

		if (!QueryProviders.IsEmpty())
		{
			BindingGateway::Query::FUeAgentQueryBindingError QueryError;
			if (!BindingGateway::Query::
				FUeAgentDomainQueryProviderRegistry::TryCreate(
					MoveTemp(QueryProviders),
					Built.QueryProviderRegistry,
					QueryError))
			{
				OutError = Contract::FDiagnostic::WrapFailure(TEXT("domain_capability_query_projection_failed"), QueryError,
					TEXT("domain_capability"), TEXT("The domain query registry could not be initialized."));
				return false;
			}
		}
		if (!CatalogSources.IsEmpty() || !ReadPorts.IsEmpty())
		{
			Discovery::FUeAgentDiscoveryError DiscoveryError;
			if (!Discovery::FUeAgentDiscoveryService::TryCreate(
					CatalogSources,
					ReadPorts,
					Built.DiscoveryService,
					DiscoveryError))
			{
				OutError = Contract::FDiagnostic::WrapFailure(TEXT("domain_capability_discovery_projection_failed"), DiscoveryError,
					TEXT("domain_capability"), TEXT("The discovery service could not be initialized."));
				return false;
			}
		}
		if (!ContentProviders.IsEmpty())
		{
			ContentDiscovery::FContentDiscoveryError DiscoveryError;
			if (!ContentDiscovery::FUeAgentContentDiscoveryService::TryCreate(
					MoveTemp(ContentProviders),
					Built.ContentDiscoveryService,
					DiscoveryError))
			{
				OutError = Contract::FDiagnostic::WrapFailure(
					TEXT("domain_capability_content_discovery_projection_failed"), DiscoveryError,
					TEXT("domain_capability"), TEXT("The content discovery service could not be initialized."));
				return false;
			}
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
				OutError = WorkspaceError.GetDiagnostic();
				return false;
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		if (bSealed)
		{
			return Fail(OutError, TEXT("domain_capability_composition_sealed"),
					TEXT("The capability composition is sealed and cannot accept another contribution."), TEXT("contributions"), Contribution->GetDomainId());
		}
		if (DomainIds.Contains(Contribution->GetDomainId()))
		{
			return Fail(OutError, TEXT("domain_capability_domain_conflict"),
					TEXT("More than one contribution declares the same domain."), TEXT("contributions.domain_id"), Contribution->GetDomainId());
		}
		DomainIds.Add(Contribution->GetDomainId());
		Contributions.Add(MoveTemp(Contribution));
		return true;
	}

	bool FDomainCapabilityComposition::TryFreeze(
		TSharedPtr<const FFrozenDomainCapabilitySet>& OutFrozen,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutFrozen.Reset();
		OutError.Reset();
		if (bSealed)
		{
			return Fail(OutError, TEXT("domain_capability_composition_sealed"), TEXT("The capability composition has already been frozen."), TEXT("composition"));
		}
		bSealed = true;
		if (Contributions.IsEmpty())
		{
			return Fail(OutError, TEXT("domain_capability_composition_empty"), TEXT("The capability composition contains no domain contributions."), TEXT("contributions"));
		}

		TSet<FString> CommandContributionIds;
		TSet<FString> CommandIds;
		TSet<FString> QueryBindings;
		TSet<FString> CatalogSourceIds;
		TSet<FString> CatalogKinds;
		TSet<FString> ReadPortIds;
		TSet<FString> ContentDiscoveryProviderIds;
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
			const FString ContributionPath = ItemField(Contributions, Contribution, TEXT("contributions"), FString());
			for (const TSharedRef<Active::IUeAgentActiveCommandContribution>&
				CommandContribution : Declaration.Command.Contributions)
			{
				if (!AddUniqueIdentity(
					CommandContributionIds,
					CommandContribution->GetContributionId(),
					TEXT("domain_capability_command_contribution"),
					OutError, ItemField(Declaration.Command.Contributions, CommandContribution, ContributionPath + TEXT(".command.contributions"), TEXT("contribution_id")), false))
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
						OutError, ItemField(Declarations, Command, ItemField(Declaration.Command.Contributions, CommandContribution, ContributionPath + TEXT(".command.contributions"), TEXT("commands")), TEXT("command_id")), true))
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
					OutError, ItemField(Declaration.Query.Providers, Provider, ContributionPath + TEXT(".query.providers"), TEXT("provider_binding")), false))
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
					OutError, ItemField(Declaration.Discovery.Sources, Source, ContributionPath + TEXT(".discovery.sources"), TEXT("source_id")), false))
				{
					return false;
				}
				for (const FString& Kind : Source.Source->GetCatalogKinds())
				{
					if (!AddUniqueIdentity(
						CatalogKinds,
						Kind,
						TEXT("domain_capability_catalog_kind"),
						OutError, ItemField(Source.Source->GetCatalogKinds(), Kind, ItemField(Declaration.Discovery.Sources, Source, ContributionPath + TEXT(".discovery.sources"), TEXT("catalog_kinds")), FString()), true))
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
					OutError, ItemField(Declaration.Discovery.ReadPorts, ReadPort, ContributionPath + TEXT(".discovery.read_ports"), TEXT("port_id")), false))
				{
					return false;
				}
			}
			for (const TSharedRef<ContentDiscovery::
				IUeAgentContentDiscoveryProvider>& Provider :
				Declaration.Discovery.ContentProviders)
			{
				if (!AddUniqueIdentity(
						ContentDiscoveryProviderIds,
						Provider->GetProviderId(),
						TEXT("domain_capability_content_discovery_provider"),
						OutError, ItemField(Declaration.Discovery.ContentProviders, Provider, ContributionPath + TEXT(".discovery.content_providers"), TEXT("provider_id")), false))
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
					OutError, ItemField(Declaration.Hjson.Providers, Provider, ContributionPath + TEXT(".hjson.providers"), TEXT("provider_id")), false))
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
					OutError, ItemField(Declaration.Hjson.Routes, Route, ContributionPath + TEXT(".hjson.routes"), TEXT("")), false))
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
					OutError, ItemField(Declaration.Composition.Contracts, Contract, ContributionPath + TEXT(".composition.contracts"), TEXT("target_kind")), true))
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
					OutError, ItemField(Declaration.Initialization.ContractIds, ContractId, ContributionPath + TEXT(".initialization.contract_ids"), TEXT("")), false))
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
					OutError, ItemField(Declaration.Operation.Operations, Operation, ContributionPath + TEXT(".operation.operations"), TEXT("operation_id")), false))
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
					OutError, ItemField(Declaration.Layout.Layouts, Layout, ContributionPath + TEXT(".layout.layouts"), TEXT("provider_id")), false)
					|| !AddUniqueIdentity(
						LayoutGraphIdentities,
						Layout.GraphIdentity,
						TEXT("domain_capability_layout_graph"),
						OutError, ItemField(Declaration.Layout.Layouts, Layout, ContributionPath + TEXT(".layout.layouts"), TEXT("graph_identity")), false))
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
					OutError, ItemField(Declaration.Surface.Surfaces, Surface, ContributionPath + TEXT(".surface.surfaces"), TEXT("surface_id")), false))
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

	bool TryFreezeProcessDomainCapabilities(
		TSharedPtr<const FFrozenDomainCapabilitySet>& OutFrozen,
		UE::UAI::Contract::FDiagnostic& OutError)
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
				for (const auto& Diagnostic : Catalog.Errors) OutError.Append(Diagnostic);
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
			const FString FactoryPath = ItemField(Entries, Entry, TEXT("factories"), FString());
			FDomainCapabilityDeclaration Declaration =
				MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
					Entry.DomainId);
			if (Entry.DomainFactory)
			{
				TSharedPtr<const FDomainCapabilityContribution> BaseContribution;
				UE::UAI::Contract::FDiagnostic FactoryError;
				const bool bCreated = Entry.DomainFactory(BaseContribution, FactoryError);
				if (!bCreated || !BaseContribution.IsValid())
				{
					OutError = FactoryError;
					if (OutError.IsEmpty())
						OutError = RegistrationFailure(TEXT("domain_capability_factory_failed"),
							bCreated ? TEXT("The domain factory reported success without returning a contribution.") : TEXT("The domain factory failed without returning a diagnostic."), FactoryPath, Entry.DomainId);
					return false;
				}
				if (BaseContribution->GetDomainId() != Entry.DomainId)
				{
					OutError = InvalidDeclaration(TEXT("domain_capability_factory_identity_mismatch"), TEXT("The factory returned a contribution for a different domain."), FactoryPath + TEXT(".domain_id"));
					OutError.ActualValue = MakeShared<FJsonValueString>(BaseContribution->GetDomainId());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(Entry.DomainId);
					return false;
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
				const FString CommandFactoryPath = ItemField(CommandFactories, CommandFactory, FactoryPath + TEXT(".command_factories"), FString());
				TSharedPtr<Active::IUeAgentActiveCommandContribution>
					CommandContribution;
				UE::UAI::Contract::FDiagnostic FactoryError;
				const bool bCreated = CommandFactory.Factory(CommandContribution, FactoryError);
				if (!bCreated || !CommandContribution.IsValid())
				{
					OutError = FactoryError;
					if (OutError.IsEmpty())
						OutError = RegistrationFailure(TEXT("domain_capability_command_factory_failed"),
							bCreated ? TEXT("The command factory reported success without returning a contribution.") : TEXT("The command factory failed without returning a diagnostic."),
							CommandFactoryPath, Entry.DomainId, CommandFactory.ContributionId);
					return false;
				}
				if (CommandContribution->GetContributionId()
					!= CommandFactory.ContributionId)
				{
					OutError = InvalidDeclaration(TEXT("domain_capability_command_factory_identity_mismatch"), TEXT("The command factory returned a different contribution from the one registered."), CommandFactoryPath + TEXT(".contribution_id"));
					OutError.CandidateName = Entry.DomainId;
					OutError.ActualEvidence = MakeShared<FJsonValueString>(CommandContribution->GetContributionId());
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(CommandFactory.ContributionId);
					return false;
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
					return Fail(OutError, TEXT("domain_capability_command_factory_duplicate"),
						TEXT("The command factory duplicates a contribution already supplied by the domain."), CommandFactoryPath + TEXT(".contribution_id"), CommandFactory.ContributionId);
				}
				Declaration.Command.Support = EDomainCapabilitySupport::Supported;
				Declaration.Command.Contributions.Add(
					CommandContribution.ToSharedRef());
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
				return Fail(OutError, TEXT("domain_capability_process_authority_reentrant"),
					TEXT("Domain capability authorities are already being built by an active call."), TEXT("authorities"));
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
