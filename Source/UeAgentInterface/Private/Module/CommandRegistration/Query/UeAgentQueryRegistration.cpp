#include "Module/CommandRegistration/Query/UeAgentQueryRegistration.h"

#include "CommandHandlers/Query/UeAgentQueryHandlers.h"
#include "Core/Contract/Discovery/UeAgentContractExample.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

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
			FString SchemaJson = {})
		{
			return {
				Name,
				WireType,
				bRequired,
				false,
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

		bool TryGetRequiredNames(
			const TSharedPtr<FJsonObject>& Schema,
			TArray<FString>& OutNames,
			FString& OutError)
		{
			OutNames.Reset();
			if (!Schema.IsValid())
			{
				OutError = TEXT("query_input_schema_missing");
				return false;
			}
			if (!Schema->HasField(TEXT("required")))
			{
				return true;
			}
			const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
			if (!Schema->TryGetArrayField(TEXT("required"), Required)
				|| !Required)
			{
				OutError = TEXT("query_input_schema_required_invalid");
				return false;
			}
			TSet<FString> Seen;
			for (const TSharedPtr<FJsonValue>& Value : *Required)
			{
				if (!Value.IsValid() || Value->Type != EJson::String)
				{
					OutError = TEXT("query_input_schema_required_invalid");
					return false;
				}
				const FString Name = Value->AsString();
				if (Name.IsEmpty() || Seen.Contains(Name))
				{
					OutError = TEXT("query_input_schema_required_invalid:") + Name;
					return false;
				}
				Seen.Add(Name);
				OutNames.Add(Name);
			}
			return true;
		}

		bool TryDeclaration(
			const FQueryRoute& Route,
			const Contract::FUeAgentOperationDescriptor& Descriptor,
			const Contract::FUeAgentContractSchema& InputContract,
			FUeAgentCommandDeclaration& OutDeclaration,
			FString& OutError)
		{
			TSharedPtr<FJsonObject> InputSchema;
			if (!Contract::Json::TryParseObject(
					Descriptor.GetInputSchema().GetJsonValue(),
					InputSchema,
					OutError)
				|| !InputSchema.IsValid())
			{
				OutError = TEXT("query_input_schema_invalid:")
					+ Route.CommandId + TEXT(":") + OutError;
				return false;
			}
			TArray<FString> RequiredInputNames;
			if (!TryGetRequiredNames(
					InputSchema,
					RequiredInputNames,
					OutError))
			{
				OutError = TEXT("query_input_schema_invalid:")
					+ Route.CommandId + TEXT(":") + OutError;
				return false;
			}
			const bool bInputRequired = !RequiredInputNames.IsEmpty()
				&& !Route.bUsesObjectPath;
			FString MinimumInput;
			Contract::FUeAgentContractValue CanonicalMinimum{FString()};
			if (bInputRequired
				&& !Contract::TryBuildMinimalContractExample(
					InputContract,
					CanonicalMinimum,
					MinimumInput,
					OutError))
			{
				OutError = TEXT("query_input_tutorial_unavailable:")
					+ Route.CommandId + TEXT(":") + OutError;
				return false;
			}

			FUeAgentCommandDeclaration Result;
			Result.CommandId = Route.CommandId;
			Result.PythonNamespace = TEXT("query");
			Result.PythonFunction = Route.PythonFunction;
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {
				Route.CommandId.StartsWith(TEXT("asset."))
					|| Route.CommandId.StartsWith(TEXT("reference."))
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
				TEXT("Explicit execution context and state-plane overrides.")));
			Result.InputFields.Add(Field(
				TEXT("projection"),
				TEXT("object"),
				false,
				TEXT("Field, provenance and bounded paging projection; no hard total-token rejection.")));

			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true, TEXT("Exact public command identity.")),
				Field(TEXT("mechanics_command_id"), TEXT("string"), true, TEXT("Exact server-owned mechanics route used for the read.")),
				Field(TEXT("read_only"), TEXT("boolean"), true, TEXT("Always true for a successful Query.")),
				Field(TEXT("query_result"), TEXT("object"), true, TEXT("Typed projection with source identity, state revision and completeness.")),
			};
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
			Result.Purpose.ResultGuidance = TEXT("Use command_id, mechanics_command_id, state revision and projection completeness to bound every conclusion.");
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
				TEXT("query.command.detail"),
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
			FString& OutError)
		{
			OutDeclarations.Reset();
			OutError.Reset();
			const TArray<FQueryRoute>& Routes = GetNonHjsonQueryRoutes();
			if (DescriptorRegistry.GetDescriptors().Num() != Routes.Num())
			{
				OutError = TEXT("query_descriptor_inventory_mismatch");
				return false;
			}
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
				if (!Route.IsValid()
					|| !Descriptor
					|| !InputContract
					|| UsedDescriptorIds.Contains(DescriptorId)
					|| Descriptor->GetOperationKind()
						!= Contract::EUeAgentDescriptorOperationKind::Query
					|| !Descriptor->GetQueryBinding().IsSet())
				{
					OutDeclarations.Reset();
					OutError = TEXT("query_route_descriptor_mismatch:")
						+ Route.CommandId + TEXT(":") + DescriptorId;
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
			if (UsedDescriptorIds.Num()
				!= DescriptorRegistry.GetDescriptors().Num())
			{
				OutDeclarations.Reset();
				OutError = TEXT("query_descriptor_route_coverage_incomplete");
				return false;
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
				FString& OutError) const override
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
			FString& OutError)
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
		FString& OutError)
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
		FString& OutError)
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
		FString& OutError)
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
