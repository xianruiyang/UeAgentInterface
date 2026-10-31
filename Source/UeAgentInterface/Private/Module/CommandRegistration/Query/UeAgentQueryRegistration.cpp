#include "Module/CommandRegistration/Query/UeAgentQueryRegistration.h"

#include "CommandHandlers/Query/UeAgentQueryHandlers.h"
#include "Core/Contract/Discovery/UeAgentContractExample.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "DomainAdapters/EditorProject/UeAgentEditorProjectMechanicsBoundary.h"

namespace UE::UAI::Module::CommandRegistration::Query
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::Query;
	using namespace CommandRegistry;
	using namespace DomainAdapters::Query;
	using namespace Module::CommandRegistration::Active;

	namespace
	{
		const FString Owner = TEXT("query-command-owner");

		FUeAgentCommandFieldDeclaration Field(
			const TCHAR* Name,
			const TCHAR* WireType,
			const bool bRequired,
			const TCHAR* Description,
			FString SchemaJson = {},
			const bool bNullable = false)
		{
			return {
				Name,
				WireType,
				bRequired,
				bNullable,
				Description,
				MoveTemp(SchemaJson),
			};
		}

		FUeAgentCommandErrorDeclaration Error(
			const TCHAR* Code,
			const TCHAR* Description)
		{
			return {Code, Description};
		}

		bool TryDeclaration(
			const FQueryRoute& Route,
			const Contract::FUeAgentOperationDescriptor& Descriptor,
			const Contract::FUeAgentContractSchema& InputContract,
			FUeAgentCommandDeclaration& OutDeclaration,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TOptional<Contract::FUeAgentContractValue> EmptyInput;
			Contract::FUeAgentContractSchemaError EmptyInputError;
			const bool bInputRequired = !Route.bUsesObjectPath
				&& !InputContract.TryValidateAndCanonicalize(
					Contract::FUeAgentContractValue(TEXT("{}")),
					[](int64, const FString&, Contract::FUeAgentContractSchemaError&) { return true; },
					EmptyInput, EmptyInputError);
			FString MinimumInput;
			Contract::FUeAgentContractValue CanonicalMinimum{FString()};
			if (bInputRequired
				&& !Contract::TryBuildMinimalContractExample(
					InputContract,
					CanonicalMinimum,
					MinimumInput,
					OutError))
			{
				// A discovery example is not an additional command-availability gate.
				// Keep the required named argument when its value needs caller data.
				MinimumInput = TEXT("input");
				OutError.Reset();
			}

			FUeAgentCommandDeclaration Result;
			const bool bContentGraph =
				Route.CommandId.StartsWith(TEXT("content_graph."));
			const BindingGateway::Query::FQueryPublicProjectionDescriptor*
				PublicProjection = BindingGateway::Query::
					FindQueryPublicProjectionDescriptor(
						Route.PublicProjectionId, &OutError);
			if (!PublicProjection)
			{
				if (OutError.IsEmpty())
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("query_public_projection_invalid"),
						TEXT("The command references an unregistered public projection descriptor."), TEXT("registration"), Route.PublicProjectionId);
				OutError.CandidateName = Route.CommandId;
				return false;
			}
			Result.CommandId = Route.CommandId;
			Result.PythonNamespace = bContentGraph
				? TEXT("content_graph")
				: TEXT("query");
			Result.PythonFunction = Route.PythonFunction;
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {
				Route.CommandId.StartsWith(TEXT("asset."))
					|| Route.CommandId.StartsWith(TEXT("reference."))
					|| bContentGraph
					? FString(TEXT("asset_current_state"))
					: FString(TEXT("editor_current_state")),
			};

			if (Route.bUsesObjectPath)
			{
				Result.InputFields.Add(Field(
					TEXT("object_path"),
					TEXT("string"),
					true,
					TEXT("Exact object path used unchanged by the server-owned asset read mechanics.")));
			}
			else if (Route.bRequiresTarget)
			{
				Result.InputFields.Add(Field(
					TEXT("target"),
					TEXT("string"),
					true,
					TEXT("Exact target identity; no client or server alias guessing is permitted.")));
			}
			if (!Route.bUsesObjectPath)
			{
				Result.InputFields.Add(Field(
					TEXT("input"),
					TEXT("object"),
					bInputRequired,
					TEXT("Command-specific query fields consumed by the exact UE provider."),
					Descriptor.GetInputSchema().GetJsonValue()));
			}
			Result.InputFields.Add(Field(
				TEXT("context"),
				TEXT("object"),
				false,
				TEXT("Validated execution context overrides; an explicit empty selection clears the inherited selection.")));
			Result.InputFields.Add(Field(
				TEXT("projection"),
				TEXT("object"),
				false,
				TEXT("Projection profile and include_defaults, include_details, include_provenance options; field types are validated before querying.")));

			const bool bHasCapabilityObservation = DomainAdapters::EditorProject::IsEditorProjectQueryRoute(
				Descriptor.GetQueryBinding().GetValue().GetProviderBinding(), TEXT("target.read"));
			for (const BindingGateway::Query::FQueryPublicResultField&
				PublicField : PublicProjection->ResultFields)
			{
				Result.ResultFields.Add(Field(
					*PublicField.Name,
					*PublicField.WireType,
					PublicField.bRequired && !bHasCapabilityObservation,
					*PublicField.Description,
					{}, PublicField.bNullable));
			}
			if (bHasCapabilityObservation)
			{
				Result.ResultFields.Add(Field(TEXT("observation"), TEXT("object"), false,
					TEXT("Required for explicit editor_project_capability requests; ordinary queries return their normal result fields instead.")));
			}
			Result.Errors = {
				Error(TEXT("query_authority_required"), TEXT("The caller lacks uai:query authority.")),
				Error(TEXT("query_cancelled"), TEXT("The request was cancelled before execution.")),
				Error(TEXT("query_params_invalid"), TEXT("The public parameter object is invalid.")),
				Error(TEXT("query_target_required"), TEXT("The exact target or object_path is missing.")),
				Error(TEXT("query_thread_invalid"), TEXT("Real UE mechanics were requested outside the game thread.")),
				Error(TEXT("query_source_not_read_only"), TEXT("A provider could not attest read-only behavior.")),
				Error(TEXT("query_command_not_supported"), TEXT("The exact route has no active provider mechanics.")),
			};

			Result.Purpose.Summary = FString::Printf(
				TEXT("Read %s from current Unreal Engine state through the unique QueryService owner."),
				*Route.CommandId);
			Result.Purpose.Target = Route.bRequiresTarget
				? TEXT("One exact UE target in an explicit execution context and state layer.")
				: TEXT("The current Editor, World, project or configured runtime state.");
			Result.Purpose.Preconditions = {
				TEXT("The UAI Editor service is active and the caller has uai:query authority."),
			};
			Result.Purpose.ParameterGuidance = Route.bRequiresTarget
				? TEXT("Pass the exact target returned by a public search or known UE identity; never rewrite it into an alias.")
				: TEXT("Pass only command-specific input, explicit context and bounded projection fields when needed.");
			Result.Purpose.ResultGuidance =
				PublicProjection->ResultGuidance;
			if (bHasCapabilityObservation)
				Result.Purpose.ResultGuidance += TEXT(" Explicit editor_project_capability requests return observation instead of the ordinary query fields; native values remain structured and SDK Result removes protocol custody from model output.");
			Result.Purpose.FailureGuidance = TEXT("Missing targets, unsupported mechanics, provider errors, cancellation and stale context remain distinct typed failures.");
			Result.Purpose.SideEffectGuidance = TEXT("Strictly read-only; a provider that cannot attest this is rejected.");
			TArray<FString> ExampleArguments{TEXT("client")};
			if (Route.bUsesObjectPath)
			{
				ExampleArguments.Add(TEXT("object_path"));
			}
			else if (Route.bRequiresTarget)
			{
				ExampleArguments.Add(TEXT("target"));
			}
			if (bInputRequired)
			{
				ExampleArguments.Add(TEXT("input=") + MinimumInput);
			}
			Result.Purpose.MinimumExample = FString::Printf(
				TEXT("%s(%s)"),
				*Route.PythonFunction,
				*FString::Join(ExampleArguments, TEXT(", ")));
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.usage"),
			};
			Result.Purpose.Keywords = {
				TEXT("query"),
				TEXT("read-only"),
				Route.CommandId,
			};
			Result.Purpose.Synonyms = {Route.PythonFunction};
			OutDeclaration = MoveTemp(Result);
			return true;
		}

		bool TryBuildDeclarations(
			const Contract::FUeAgentDescriptorRegistry& DescriptorRegistry,
			TArray<FUeAgentCommandDeclaration>& OutDeclarations,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutDeclarations.Reset();
			OutError.Reset();
			const TArray<FQueryRoute>& Routes = GetNonHjsonQueryRoutes();
			TSet<FString> UsedDescriptorIds;
			OutDeclarations.Reserve(Routes.Num());
			for (const FQueryRoute& Route : Routes)
			{
				const FString DescriptorId = TEXT("uai.query.")
					+ Route.MechanicsCommandId;
				const Contract::FUeAgentOperationDescriptor* Descriptor =
					DescriptorRegistry.FindByDescriptorId(DescriptorId);
				const Contract::FUeAgentContractSchema* InputContract = Descriptor
					? DescriptorRegistry.FindCompiledInputSchemaExact(
						Descriptor->GetIdentity())
					: nullptr;
				if (!Route.TryValidate(OutError))
				{
					OutDeclarations.Reset();
					return false;
				}
				if (!Descriptor
					|| !InputContract
					|| UsedDescriptorIds.Contains(DescriptorId)
					|| Descriptor->GetOperationKind()
						!= Contract::EUeAgentDescriptorOperationKind::Query
					|| !Descriptor->GetQueryBinding().IsSet())
				{
					OutDeclarations.Reset();
					const TCHAR* Reason = !Descriptor ? TEXT("The query route's operation descriptor is not registered.")
						: !InputContract ? TEXT("The query route has no compiled input contract.")
						: UsedDescriptorIds.Contains(DescriptorId) ? TEXT("Two query routes reference the same operation descriptor.")
						: Descriptor->GetOperationKind() != Contract::EUeAgentDescriptorOperationKind::Query
							? TEXT("The route's operation descriptor is not a query.") : TEXT("The operation descriptor has no query binding.");
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("query_route_descriptor_mismatch"), Reason, TEXT("registration"), DescriptorId);
					OutError.CandidateName = Route.CommandId;
					return false;
				}
				FUeAgentCommandDeclaration Command;
				if (!TryDeclaration(
						Route,
						*Descriptor,
						*InputContract,
						Command,
						OutError))
				{
					OutDeclarations.Reset();
					return false;
				}
				UsedDescriptorIds.Add(DescriptorId);
				OutDeclarations.Add(MoveTemp(Command));
			}
			return true;
		}

		class FQueryContribution final
			: public IUeAgentActiveCommandContribution
		{
		public:
			explicit FQueryContribution(
				TSharedRef<IUeAgentQueryService> InService,
				TArray<FUeAgentCommandDeclaration> InDeclarations)
				: Service(MoveTemp(InService))
				, Declarations(MoveTemp(InDeclarations))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Value = TEXT("query");
				return Value;
			}

			virtual void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Append(Declarations);
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				return TryCreateQueryHandlers(Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentQueryService> Service;
			TArray<FUeAgentCommandDeclaration> Declarations;
		};

		bool TryCreateDomainCapability(
			TSharedPtr<
				const DomainCapability::FDomainCapabilityContribution>&
					OutContribution,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TArray<
				BindingGateway::Query::
					FUeAgentDomainQueryProviderRegistration> Providers;
			if (!TryCreateProductionQueryProviderRegistrations(
					Providers,
					OutError))
			{
				OutContribution.Reset();
				return false;
			}
			DomainCapability::FDomainCapabilityDeclaration Declaration =
				DomainCapability::
					MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
						TEXT("query"));
			Declaration.Query.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Query.Providers = MoveTemp(Providers);
			return DomainCapability::FDomainCapabilityContribution::
				TryCreate(
					MoveTemp(Declaration),
					OutContribution,
					OutError);
		}

		const DomainCapability::
			FDomainCapabilityContributionFactoryRegistration
				GQueryDomainCapabilityRegistration(
					TEXT("query"),
					&TryCreateDomainCapability);

		const FUeAgentActiveCommandContributionFactoryRegistration
			GQueryContributionRegistration(
				TEXT("query"),
				&TryCreateProductionQueryContribution);
	}

	bool TryCreateQueryContribution(
		TSharedRef<IUeAgentQueryService> Service,
		TSharedRef<const Contract::FUeAgentDescriptorRegistry>
			DescriptorRegistry,
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		TArray<FUeAgentCommandDeclaration> Declarations;
		if (!TryBuildDeclarations(
				DescriptorRegistry.Get(),
				Declarations,
				OutError))
		{
			OutContribution.Reset();
			return false;
		}
		OutContribution = MakeShared<FQueryContribution>(
			MoveTemp(Service),
			MoveTemp(Declarations));
		return true;
	}

	bool TryCreateProductionQueryContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		FProductionQueryComposition Production;
		if (!TryCreateProductionQueryService(Production, OutError)
			|| !Production.IsValid())
		{
			OutContribution.Reset();
			return false;
		}
		return TryCreateQueryContribution(
			Production.Service.ToSharedRef(),
			Production.DescriptorRegistry.ToSharedRef(),
			OutContribution,
			OutError);
	}

	bool TryCreateQueryRegistration(
		TSharedRef<IUeAgentQueryService> Service,
		TSharedRef<const Contract::FUeAgentDescriptorRegistry>
			DescriptorRegistry,
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		TSharedPtr<IUeAgentActiveCommandContribution> Contribution;
		FUeAgentActiveCommandComposition Composition;
		if (!TryCreateQueryContribution(
				Service,
				DescriptorRegistry,
				Contribution,
				OutError)
			|| !Composition.TryAdd(Contribution.ToSharedRef(), OutError)
			|| !Composition.TryCompose(OutRegistry, OutDispatch, OutError))
		{
			OutRegistry.Reset();
			OutDispatch.Reset();
			return false;
		}
		return true;
	}
}
