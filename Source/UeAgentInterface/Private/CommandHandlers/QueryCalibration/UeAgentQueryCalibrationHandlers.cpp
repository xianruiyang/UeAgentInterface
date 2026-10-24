#include "CommandHandlers/QueryCalibration/UeAgentQueryCalibrationHandlers.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/Contract/Discovery/UeAgentContractExample.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::CommandHandlers::QueryCalibration
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace Contract;
	using namespace Discovery::Command;

	namespace
	{
		const FString Owner = TEXT("query-calibration-command-owner");

		bool Canonical(
			const TSharedPtr<FJsonObject>& Object,
			FString& OutJson,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			return Json::TryCanonicalize(Object, OutJson, OutError);
		}

		FUeAgentCommandResult Failure(const FUeAgentCommandRequest& Request, UE::UAI::Contract::FDiagnostic Diagnostic,
            const FString& Subject = FString())
        {
            if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Subject;
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
        }

		bool HasQueryAuthority(const FUeAgentCallerContext& Caller)
		{
			return Caller.IsValid()
				&& Caller.GetAuthorityScopes().Contains(TEXT("uai:query"));
		}

		FString PythonNamedInvocation(const FCommandDetail& Detail)
		{
			const FString& MinimumExample =
				Detail.Purpose.MinimumExample;
			if (MinimumExample.StartsWith(TEXT("client."))
				|| MinimumExample.StartsWith(TEXT("result."))
				|| MinimumExample.StartsWith(TEXT("workspace.")))
			{
				return MinimumExample;
			}
			if (Detail.PythonNamespace.IsEmpty()
				|| Detail.PythonFunction.IsEmpty())
			{
				return MinimumExample;
			}
			TArray<FString> Arguments;
			for (const FUeAgentCommandFieldDeclaration& Field
				: Detail.InputFields)
			{
				if (Field.bRequired)
				{
				FString Value = Field.Name;
				if (Field.WireType == TEXT("object") && !Field.SchemaJson.IsEmpty())
				{
					TSharedPtr<const FUeAgentContractSchema> Schema;
					FUeAgentContractSchemaError SchemaError;
					FUeAgentContractValue CanonicalExample{FString()};
					FString Literal;
					UE::UAI::Contract::FDiagnostic ExampleError;
					if (FUeAgentContractSchema::TryCompile(
						FUeAgentContractValue(Field.SchemaJson), Schema, SchemaError)
						&& TryBuildMinimalContractExample(*Schema, CanonicalExample, Literal, ExampleError))
					{
						Value = MoveTemp(Literal);
					}
				}
				Arguments.Add(Field.Name + TEXT("=") + Value);
				}
			}
			FString Invocation = FString::Printf(
				TEXT("client.%s.%s(%s)"),
				*Detail.PythonNamespace,
				*Detail.PythonFunction,
				*FString::Join(Arguments, TEXT(", ")));
			return Invocation;
		}

		class FCommandSearchHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FCommandSearchHandler(
				TSharedRef<const FCommandDiscoveryService> InDiscovery)
				: Discovery(MoveTemp(InDiscovery))
			{
			}

			virtual const FString& GetCommandId() const override
			{
				static const FString Id = TEXT("query.command.search");
				return Id;
			}

			virtual const FString& GetPublicOwner() const override
			{
				return Owner;
			}

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (!HasQueryAuthority(Caller))
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("query_authority_required"), TEXT("uai:query is required"), TEXT("query")));
				}
				if (Cancellation->IsCancelled())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("query_cancelled"), TEXT("request was cancelled"), TEXT("query")));
				}
				FCommandSearchRequest Search;
				const TSharedPtr<FJsonObject>& Params = Request.GetParams();
				if (!Params.IsValid() || !Params->TryGetStringField(TEXT("intent"), Search.Intent))
				{
					return Failure(Request, MakeInputDiagnostic(TEXT("command_search_params_invalid"), TEXT("/intent"),
							TEXT("type"), TEXT("intent must be a string describing the command to find.")));
				}
				FCommandSearchPage Page;
				FCommandDiscoveryError Error;
				if (!Discovery->Search(Search, Page, Error))
				{
					return Failure(Request, Error);
				}
				TArray<TSharedPtr<FJsonValue>> Candidates;
				for (const FCommandCandidate& Candidate : Page.Candidates)
				{
					Candidates.Add(MakeShared<FJsonValueString>(Candidate.CommandId));
				}
				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetArrayField(TEXT("commands"), MoveTemp(Candidates));
				FString PayloadJson;
				UE::UAI::Contract::FDiagnostic ErrorText;
				if (!Canonical(Payload, PayloadJson, ErrorText))
				{
					return Failure(Request, ErrorText);
				}
				return FUeAgentCommandResult::Success(Request, MoveTemp(PayloadJson));
			}

		private:
			TSharedRef<const FCommandDiscoveryService> Discovery;
		};

		class FCommandUsageHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FCommandUsageHandler(
				TSharedRef<const FCommandDiscoveryService> InDiscovery)
				: Discovery(MoveTemp(InDiscovery))
			{
			}

			virtual const FString& GetCommandId() const override
			{
				static const FString Id = TEXT("query.command.usage");
				return Id;
			}

			virtual const FString& GetPublicOwner() const override
			{
				return Owner;
			}

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (!HasQueryAuthority(Caller))
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("query_authority_required"), TEXT("uai:query is required"), TEXT("query")));
				}
				if (Cancellation->IsCancelled())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("query_cancelled"), TEXT("request was cancelled"), TEXT("query")));
				}
				const TSharedPtr<FJsonObject>& Params = Request.GetParams();
				const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
				if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("commands"), Values)
					|| !Values || Values->Num() < 1)
				{
					auto Diagnostic = MakeInputDiagnostic(TEXT("command_usage_params_invalid"), TEXT("/commands"),
						Values ? TEXT("minItems") : TEXT("type"), TEXT("commands must be a non-empty array of command names."));
					Diagnostic.CandidateName = Request.GetCommandId();
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty array of command name strings"));
					Diagnostic.ActualValue = Params.IsValid() ? Params->TryGetField(TEXT("commands")) : nullptr;
					Diagnostic.ActualValuePresent = Diagnostic.ActualValue.IsValid();
					return Failure(Request, MoveTemp(Diagnostic));
				}
				TArray<FString> Commands;
				TMap<FString, int32> InputIndices;
				for (int32 Index = 0; Index < Values->Num(); ++Index)
				{
					const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
					FString Command;
					if (!Value.IsValid() || !Value->TryGetString(Command) || Command.IsEmpty())
					{
						auto Diagnostic = MakeInputDiagnostic(TEXT("command_usage_params_invalid"), FString::Printf(TEXT("/commands/%d"), Index),
							TEXT("type"), TEXT("Each commands element must be a non-empty command name string."));
						Diagnostic.CandidateName = Request.GetCommandId();
						Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty command name string"));
						Diagnostic.ActualValue = Value;
						Diagnostic.ActualValuePresent = Value.IsValid();
						return Failure(Request, MoveTemp(Diagnostic));
					}
					if (!InputIndices.Contains(Command))
					{
						Commands.Add(Command);
						InputIndices.Add(Command, Index);
					}
				}
				const TSharedPtr<FJsonObject> Usage = MakeShared<FJsonObject>();
				for (const FString& Command : Commands)
				{
					const auto RejectCommand = [&](FDiagnostic Diagnostic)
					{
						if (Diagnostic.CandidateName.IsEmpty()) Diagnostic.CandidateName = Command;
						if (Diagnostic.Source.JsonPath.IsEmpty())
							Diagnostic.Source.JsonPath = FString::Printf(TEXT("/commands/%d"), InputIndices.FindChecked(Command));
						return Failure(Request, MoveTemp(Diagnostic));
					};
					FCommandDetail Detail;
					FCommandDiscoveryError Error;
					if (!Discovery->GetDetail(Command, FString(), Detail, Error))
					{
						return RejectCommand(MoveTemp(Error));
					}
					if (Detail.LifecycleStatus != TEXT("active"))
					{
						return RejectCommand(FDiagnostic::Failure(TEXT("command_usage_inactive"),
							TEXT("Usage is available only for active commands."), TEXT("command_usage")));
					}
					const FString Invocation = PythonNamedInvocation(Detail);
					if (Invocation.IsEmpty())
					{
						return RejectCommand(FDiagnostic::Failure(TEXT("command_usage_unavailable"),
							TEXT("The registered command has no generated Python invocation."), TEXT("command_usage")));
					}
					Usage->SetStringField(Command, Invocation);
				}
				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetObjectField(TEXT("usage"), Usage);
				FString PayloadJson;
				UE::UAI::Contract::FDiagnostic ErrorText;
				if (!Canonical(Payload, PayloadJson, ErrorText))
				{
					return Failure(Request, ErrorText);
				}
				return FUeAgentCommandResult::Success(Request, MoveTemp(PayloadJson));
			}

		private:
			TSharedRef<const FCommandDiscoveryService> Discovery;
		};

	}

	bool TryCreateQueryCalibrationHandlers(
		TSharedRef<const FCommandDiscoveryService> Discovery,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers = {
			MakeShared<FCommandSearchHandler>(Discovery),
			MakeShared<FCommandUsageHandler>(Discovery),
		};
		return true;
	}
}
