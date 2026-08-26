#include "Misc/AutomationTest.h"

#if __has_include("Core/Discovery/Command/UeAgentCommandDiscovery.h") \
	&& __has_include("DomainAdapters/QueryCalibration/UeAgentQueryCalibrationSource.h") \
	&& __has_include("Module/CommandRegistration/QueryCalibration/UeAgentQueryCalibrationRegistration.h")
#define UAI_T05_QUERY_CALIBRATION_AVAILABLE 1
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/Discovery/Command/UeAgentCommandDiscovery.h"
#include "DomainAdapters/QueryCalibration/UeAgentQueryCalibrationSource.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Module/CommandRegistration/QueryCalibration/UeAgentQueryCalibrationRegistration.h"
#else
#define UAI_T05_QUERY_CALIBRATION_AVAILABLE 0
#endif

#if UAI_T05_QUERY_CALIBRATION_AVAILABLE
namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery::Command;
	using namespace UE::UAI::DomainAdapters::QueryCalibration;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Module::CommandRegistration::QueryCalibration;

	class FNeverCancelled final : public IUeAgentCancellationSignal
	{
	public:
		virtual bool IsCancelled() const override
		{
			return false;
		}
	};

	class FFixtureQuerySource final : public IUeAgentQueryCalibrationSource
	{
	public:
		virtual bool Read(
			const FQueryCalibrationRequest& Request,
			FQueryCalibrationPage& OutPage,
			FQueryCalibrationError& OutError) const override
		{
			OutPage = {};
			OutError = {};
			++ReadCount;
			if (!GetQueryCalibrationCategoryIds().Contains(Request.Category))
			{
				OutError.Code = TEXT("query_category_unknown");
				OutError.Detail = Request.Category;
				return false;
			}

			OutPage.Category = Request.Category;
			OutPage.SourceId = TEXT("strict_v2_t05_fixture");
			OutPage.StateLayer = TEXT("fixture_current_state");
			OutPage.Revision = TEXT("fixture-r1");
			OutPage.bReadOnly = true;
			OutPage.Completeness = EQueryCalibrationCompleteness::Complete;
			OutPage.Items = {
				FQueryCalibrationItem{
					Request.Category + TEXT(":one"),
					TEXT("first"),
					Request.Category,
					{{TEXT("value"), TEXT("one")}}},
				FQueryCalibrationItem{
					Request.Category + TEXT(":two"),
					TEXT("second"),
					Request.Category,
					{{TEXT("value"), TEXT("two")}}},
			};
			if (Request.Category == TEXT("reference"))
			{
				OutPage.Completeness =
					EQueryCalibrationCompleteness::Partial;
				OutPage.MissingSources = {TEXT("fixture_optional_reference_provider")};
			}
			return true;
		}

		mutable int32 ReadCount = 0;
		mutable int32 MutationCount = 0;
	};

	struct FFixtureRegistration
	{
		TSharedRef<FFixtureQuerySource> Source =
			MakeShared<FFixtureQuerySource>();
		TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
		TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
		FString Error;
	};

	FFixtureRegistration MakeRegistration(FAutomationTestBase* Test)
	{
		FFixtureRegistration Fixture;
		Test->TestTrue(
			TEXT("query calibration registration composes"),
			TryCreateQueryCalibrationRegistration(
				Fixture.Source,
				Fixture.Registry,
				Fixture.Dispatch,
				Fixture.Error));
		Test->TestTrue(TEXT("immutable registry is available"), Fixture.Registry.IsValid());
		Test->TestTrue(TEXT("exact dispatch is available"), Fixture.Dispatch.IsValid());
		return Fixture;
	}

	bool DecodeRequest(
		const FString& CommandId,
		const FString& ContractHash,
		const FString& ParamsJson,
		TOptional<FUeAgentCommandRequest>& OutRequest)
	{
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t05\",")
			TEXT("\"request_id\":\"req-t05\",")
			TEXT("\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*CommandId,
			*ContractHash,
			*ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()),
			Encoded.Length());
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		return FUeAgentCommandProtocolCodec().DecodeRequest(
			Bytes,
			OutRequest,
			Diagnostic);
	}

	FUeAgentCommandResult SubmitExact(
		const TSharedRef<const FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const FString& ContractHash,
		const FString& ParamsJson)
	{
		TOptional<FUeAgentCommandRequest> Request;
		if (!DecodeRequest(CommandId, ContractHash, ParamsJson, Request)
			|| !Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t05"),
				TEXT("req-t05"),
				CommandId,
				ContractHash,
				TEXT("test_request_decode_failed"),
				TEXT("strict_v2 fixture could not decode its request"));
		}
		return Dispatch->Submit(
			Request.GetValue(),
			FUeAgentCallerContext(
				TEXT("strict-v2-t05"),
				{TEXT("uai:query")},
				TEXT("corr-t05")),
			MakeShared<FNeverCancelled>());
	}

	FUeAgentCommandResult Submit(
		const FFixtureRegistration& Fixture,
		const FString& CommandId,
		const FString& ContractHash,
		const FString& ParamsJson)
	{
		return SubmitExact(
			Fixture.Dispatch.ToSharedRef(),
			CommandId,
			ContractHash,
			ParamsJson);
	}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T05DiscoveryPurposeTest,
	"UAI.StrictV2.T05.QUERY.PUBLIC.CALIBRATION.DiscoveryPurpose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T05DiscoveryPurposeTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T05_QUERY_CALIBRATION_AVAILABLE
	TestTrue(TEXT("T05 query calibration is implemented"), false);
	return false;
#else
	FFixtureRegistration Fixture = MakeRegistration(this);
	if (!Fixture.Registry.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("calibration publishes exactly three commands"), Fixture.Registry->GetContracts().Num(), 3);

	TSharedPtr<const FCommandDiscoveryService> Discovery;
	FCommandDiscoveryError Error;
	TestTrue(
		TEXT("purpose discovery consumes the immutable registry"),
		FCommandDiscoveryService::TryCreate(
			Fixture.Registry.ToSharedRef(),
			Discovery,
			Error));
	if (!Discovery.IsValid())
	{
		return false;
	}

	FCommandSearchRequest Search;
	Search.Intent = TEXT("inspect the current editor runtime without changing assets");
	Search.Limit = 3;
	FCommandSearchPage Page;
	TestTrue(TEXT("unknown-name intent produces candidates"), Discovery->Search(Search, Page, Error));
	TestTrue(TEXT("search returns a small result"), Page.Candidates.Num() > 0 && Page.Candidates.Num() <= 3);
	const FCommandCandidate* Candidate = Page.Candidates.FindByPredicate(
		[](const FCommandCandidate& Item)
		{
			return Item.CommandId == TEXT("query.calibration.read");
		});
	TestNotNull(TEXT("runtime intent selects the exact calibration query"), Candidate);
	if (!Candidate)
	{
		return false;
	}
	TestTrue(TEXT("candidate explains its match"), !Candidate->MatchReason.IsEmpty());
	TestTrue(TEXT("candidate carries an exact contract identity"), Candidate->ContractHash.StartsWith(TEXT("sha256:")));
	TestFalse(TEXT("candidate is read-only"), Candidate->bHasSideEffects);

	FCommandDetail Detail;
	TestTrue(
		TEXT("exact id and hash open one complete purpose contract"),
		Discovery->GetDetail(
			Candidate->CommandId,
			Candidate->ContractHash,
			Detail,
			Error));
	TestEqual(TEXT("detail preserves exact id"), Detail.CommandId, Candidate->CommandId);
	TestTrue(TEXT("detail declares applicable target"), !Detail.Purpose.Target.IsEmpty());
	TestTrue(TEXT("detail declares preconditions"), !Detail.Purpose.Preconditions.IsEmpty());
	TestTrue(TEXT("detail declares parameter guidance"), !Detail.Purpose.ParameterGuidance.IsEmpty());
	TestTrue(TEXT("detail declares result guidance"), !Detail.Purpose.ResultGuidance.IsEmpty());
	TestTrue(TEXT("detail declares failure guidance"), !Detail.Purpose.FailureGuidance.IsEmpty());
	TestTrue(TEXT("detail declares side effects"), !Detail.Purpose.SideEffectGuidance.IsEmpty());
	TestTrue(TEXT("detail contains a minimal exact example"), !Detail.Purpose.MinimumExample.IsEmpty());
	TestEqual(TEXT("detail preserves the generated namespace"), Detail.PythonNamespace, FString(TEXT("query")));
	TestEqual(TEXT("detail preserves the generated method"), Detail.PythonFunction, FString(TEXT("calibration_read")));
	TestTrue(TEXT("detail exposes stable public tutorial references"), Detail.TutorialReferences.Num() >= 2);
	TestTrue(
		TEXT("detail references the public SDK guide"),
		Detail.TutorialReferences.Contains(TEXT("uai-docs:python-sdk-and-cli#command-discovery")));
	TestTrue(
		TEXT("detail references the installed skill guide"),
		Detail.TutorialReferences.Contains(TEXT("uai-skill:public-sdk-playbook#command-discovery")));

	const FUeAgentFrozenCommandContract* DetailContract =
		Fixture.Registry->FindExact(TEXT("query.command.detail"));
	TestNotNull(TEXT("detail command contract is exact"), DetailContract);
	if (!DetailContract)
	{
		return false;
	}
	const FString DetailParams = FString::Printf(
		TEXT("{\"command_id\":\"%s\",\"contract_hash\":\"%s\"}"),
		*Candidate->CommandId,
		*Candidate->ContractHash);
	const FUeAgentCommandResult DetailResult = Submit(
		Fixture,
		DetailContract->GetCommandId(),
		DetailContract->GetContractHash(),
		DetailParams);
	TestTrue(TEXT("detail returns through exact dispatch"), DetailResult.IsOk());
	TestTrue(
		TEXT("detail serializes the stable tutorial references"),
		DetailResult.GetPayloadJson().Contains(TEXT("tutorial_references")));
	TestTrue(
		TEXT("detail serializes named, generic, exact and CLI invocation templates"),
		DetailResult.GetPayloadJson().Contains(TEXT("invocation"))
		&& DetailResult.GetPayloadJson().Contains(TEXT("python_named"))
		&& DetailResult.GetPayloadJson().Contains(TEXT("python_generic"))
		&& DetailResult.GetPayloadJson().Contains(TEXT("python_exact"))
		&& DetailResult.GetPayloadJson().Contains(TEXT("cli_exact")));
	TestTrue(
		TEXT("named invocation is projected from the generated namespace and fields"),
		DetailResult.GetPayloadJson().Contains(
			TEXT("client.query.calibration_read(")));
	TestTrue(
		TEXT("purpose declares the negative scope boundary"),
		DetailResult.GetPayloadJson().Contains(TEXT("\"non_purpose\"")));
	TestFalse(
		TEXT("stale detail identity is rejected"),
		Discovery->GetDetail(
			Candidate->CommandId,
			TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
			Detail,
			Error));
	TestEqual(TEXT("stale detail failure is typed"), Error.Code, FString(TEXT("command_contract_version_mismatch")));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T05ActiveTutorialCompletenessTest,
	"UAI.StrictV2.T05.QUERY.PUBLIC.CALIBRATION.ActiveTutorialCompleteness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T05ActiveTutorialCompletenessTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T05_QUERY_CALIBRATION_AVAILABLE
	TestTrue(TEXT("T05 query calibration is implemented"), false);
	return false;
#else
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
			TEXT("the process active command composition freezes"),
			TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid()
		|| !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}
	const FUeAgentFrozenCommandContract* DetailContract =
		Registry->FindExact(TEXT("query.command.detail"));
	if (!TestNotNull(
			TEXT("the active registry exposes exact command detail"),
			DetailContract))
	{
		return false;
	}
	for (const FUeAgentFrozenCommandContract& Contract
		: Registry->GetContracts())
	{
		const FString Params = FString::Printf(
			TEXT("{\"command_id\":\"%s\",\"contract_hash\":\"%s\"}"),
			*Contract.GetCommandId(),
			*Contract.GetContractHash());
		const FUeAgentCommandResult Result = SubmitExact(
			Dispatch.ToSharedRef(),
			DetailContract->GetCommandId(),
			DetailContract->GetContractHash(),
			Params);
		TSharedPtr<FJsonObject> Payload;
		FString JsonError;
		if (!Result.IsOk()
			|| !UE::UAI::CommandContract::Json::TryParseObject(
				Result.GetPayloadJson(), Payload, JsonError)
			|| !Payload.IsValid())
		{
			AddError(TEXT("active command detail failed: ")
				+ Contract.GetCommandId() + TEXT(":")
				+ (Result.IsOk() ? JsonError : Result.GetErrorJson()));
			return false;
		}
		const TSharedPtr<FJsonObject>* Purpose = nullptr;
		const TSharedPtr<FJsonObject>* Invocation = nullptr;
		FString NonPurpose;
		FString PythonNamed;
		FString PythonGeneric;
		FString PythonExact;
		FString CliExact;
		const bool bComplete =
			Payload->GetStringField(TEXT("command_id"))
				== Contract.GetCommandId()
			&& Payload->GetStringField(TEXT("contract_hash"))
				== Contract.GetContractHash()
			&& Payload->HasTypedField<EJson::Boolean>(TEXT("read_only"))
			&& Payload->HasTypedField<EJson::Array>(TEXT("input_fields"))
			&& Payload->HasTypedField<EJson::Array>(TEXT("result_fields"))
			&& Payload->HasTypedField<EJson::Array>(TEXT("errors"))
			&& Payload->HasTypedField<EJson::Array>(TEXT("tutorial_references"))
			&& Payload->TryGetObjectField(TEXT("purpose"), Purpose)
			&& Purpose && Purpose->IsValid()
			&& (*Purpose)->TryGetStringField(TEXT("non_purpose"), NonPurpose)
			&& !NonPurpose.IsEmpty()
			&& Payload->TryGetObjectField(TEXT("invocation"), Invocation)
			&& Invocation && Invocation->IsValid()
			&& (*Invocation)->TryGetStringField(TEXT("python_named"), PythonNamed)
			&& !PythonNamed.IsEmpty()
			&& (*Invocation)->TryGetStringField(TEXT("python_generic"), PythonGeneric)
			&& !PythonGeneric.IsEmpty()
			&& (*Invocation)->TryGetStringField(TEXT("python_exact"), PythonExact)
			&& !PythonExact.IsEmpty()
			&& (*Invocation)->TryGetStringField(TEXT("cli_exact"), CliExact)
			&& !CliExact.IsEmpty();
		if (!bComplete)
		{
			AddError(TEXT("active command machine tutorial is incomplete: ")
				+ Contract.GetCommandId());
			return false;
		}
	}
	TestTrue(
		TEXT("every active command exposes one complete machine tutorial"),
		!Registry->GetContracts().IsEmpty());
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T05ActiveVariantDiscoveryTest,
	"UAI.StrictV2.T05.QUERY.PUBLIC.CALIBRATION.ActiveVariantDiscovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T05ActiveVariantDiscoveryTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T05_QUERY_CALIBRATION_AVAILABLE
	TestTrue(TEXT("T05 query calibration is implemented"), false);
	return false;
#else
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString CompositionError;
	if (!TestTrue(
		TEXT("the process active command composition freezes"),
		TryComposeProcessActiveCommandComposition(
			Registry,
			Dispatch,
			CompositionError))
		|| !Registry.IsValid())
	{
		AddError(CompositionError);
		return false;
	}

	TSharedPtr<const FCommandDiscoveryService> Discovery;
	FCommandDiscoveryError DiscoveryError;
	if (!TestTrue(
		TEXT("active command discovery consumes the immutable registry"),
		FCommandDiscoveryService::TryCreate(
			Registry.ToSharedRef(),
			Discovery,
			DiscoveryError))
		|| !Discovery.IsValid())
	{
		AddError(DiscoveryError.Code + TEXT(":") + DiscoveryError.Detail);
		return false;
	}
	const FUeAgentFrozenCommandContract* DetailContract =
		Registry->FindExact(TEXT("query.command.detail"));
	if (!TestNotNull(
		TEXT("the active registry exposes the public detail command"),
		DetailContract))
	{
		return false;
	}

	struct FVariantExpectation final
	{
		FString Intent;
		FString CommandId;
		FString VariantId;
	};
	const TArray<FVariantExpectation> Expectations = {
		{
			TEXT("set the current editor world context map"),
			TEXT("editor.session"),
			TEXT("policy:world-context:set-current/v1"),
		},
		{
			TEXT("capture the current level viewport image"),
			TEXT("capture.image.execute"),
			TEXT("policy:image-capture:viewport/v1"),
		},
	};

	for (const FVariantExpectation& Expectation : Expectations)
	{
		FCommandSearchRequest Search;
		Search.Intent = Expectation.Intent;
		Search.Limit = 3;
		FCommandSearchPage Page;
		if (!TestTrue(
			TEXT("natural-language intent produces active candidates"),
			Discovery->Search(Search, Page, DiscoveryError)))
		{
			AddError(DiscoveryError.Code + TEXT(":") + DiscoveryError.Detail);
			return false;
		}
		const FCommandCandidate* Candidate = Page.Candidates.FindByPredicate(
			[&Expectation](const FCommandCandidate& Item)
			{
				return Item.CommandId == Expectation.CommandId
					&& !Item.VariantId.IsEmpty();
			});
		if (!TestNotNull(
			TEXT("intent resolves the exact executable command variant"),
			Candidate))
		{
			return false;
		}
		TestEqual(
			TEXT("the first executable variant for the command matches the intent"),
			Candidate->VariantId,
			Expectation.VariantId);

		FCommandDetail Detail;
		if (!TestTrue(
			TEXT("candidate identity opens the same executable variant"),
			Discovery->GetDetail(
				Candidate->CommandId,
				Candidate->ContractHash,
				Candidate->VariantId,
				Detail,
				DiscoveryError)))
		{
			AddError(DiscoveryError.Code + TEXT(":") + DiscoveryError.Detail);
			return false;
		}
		TestEqual(
			TEXT("variant detail preserves the candidate variant identity"),
			Detail.VariantId,
			Expectation.VariantId);
		TestTrue(
			TEXT("variant detail exposes a complete request schema"),
			!Detail.RequestSchemaJson.IsEmpty());
		TSharedPtr<FJsonObject> RequestSchema;
		FString SchemaError;
		TestTrue(
			TEXT("variant request schema is canonical JSON"),
			UE::UAI::CommandContract::Json::TryParseObject(
				Detail.RequestSchemaJson,
				RequestSchema,
				SchemaError));
		FString SchemaType;
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		TestTrue(
			TEXT("variant request schema declares its complete object shape"),
			RequestSchema.IsValid()
				&& RequestSchema->TryGetStringField(TEXT("type"), SchemaType)
				&& SchemaType == TEXT("object")
				&& RequestSchema->TryGetObjectField(TEXT("properties"), Properties)
				&& Properties
				&& Properties->IsValid()
				&& RequestSchema->HasTypedField<EJson::Array>(TEXT("required")));
		TestTrue(
			TEXT("variant detail declares an applicable target"),
			!Detail.Purpose.Target.IsEmpty());
		TestTrue(
			TEXT("variant detail contains a minimal exact example"),
			!Detail.Purpose.MinimumExample.IsEmpty());

		const FString DetailParams = FString::Printf(
			TEXT("{\"command_id\":\"%s\",\"contract_hash\":\"%s\",\"variant_id\":\"%s\"}"),
			*Candidate->CommandId,
			*Candidate->ContractHash,
			*Candidate->VariantId);
		const FUeAgentCommandResult DetailResult = SubmitExact(
			Dispatch.ToSharedRef(),
			DetailContract->GetCommandId(),
			DetailContract->GetContractHash(),
			DetailParams);
		TestTrue(
			TEXT("variant detail is available through exact public dispatch"),
			DetailResult.IsOk());
		TSharedPtr<FJsonObject> DetailPayload;
		FString DetailJsonError;
		TestTrue(
			TEXT("public variant detail returns structured payload"),
			DetailResult.IsOk()
				&& UE::UAI::CommandContract::Json::TryParseObject(
					DetailResult.GetPayloadJson(),
					DetailPayload,
					DetailJsonError)
				&& DetailPayload.IsValid()
				&& DetailPayload->GetStringField(TEXT("variant_id"))
					== Candidate->VariantId
				&& DetailPayload->HasTypedField<EJson::Object>(
					TEXT("request_schema")));
	}

	const FUeAgentFrozenCommandContract* EditorSession =
		Registry->FindExact(TEXT("editor.session"));
	if (!TestNotNull(
		TEXT("editor session contract is exact"),
		EditorSession))
	{
		return false;
	}
	FCommandDetail UnknownDetail;
	TestFalse(
		TEXT("unknown variant identity is rejected"),
		Discovery->GetDetail(
			EditorSession->GetCommandId(),
			EditorSession->GetContractHash(),
			TEXT("policy:world-context:unknown/v1"),
			UnknownDetail,
			DiscoveryError));
	TestEqual(
		TEXT("unknown variant failure is typed"),
		DiscoveryError.Code,
		FString(TEXT("command_variant_not_registered_exact")));

	FCommandDetail GenericDetail;
	TestTrue(
		TEXT("the existing command-level detail overload remains compatible"),
		Discovery->GetDetail(
			EditorSession->GetCommandId(),
			EditorSession->GetContractHash(),
			GenericDetail,
			DiscoveryError));
	TestEqual(
		TEXT("command-level detail does not invent a variant identity"),
		GenericDetail.VariantId,
		FString());
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T05SevenKindsTest,
	"UAI.StrictV2.T05.QUERY.PUBLIC.CALIBRATION.SevenKinds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T05SevenKindsTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T05_QUERY_CALIBRATION_AVAILABLE
	TestTrue(TEXT("T05 query calibration is implemented"), false);
	return false;
#else
	FFixtureRegistration Fixture = MakeRegistration(this);
	if (!Fixture.Registry.IsValid() || !Fixture.Dispatch.IsValid())
	{
		return false;
	}
	const TArray<FString>& Categories = GetQueryCalibrationCategoryIds();
	TestEqual(TEXT("six state providers plus command discovery form seven query kinds"), Categories.Num() + 1, 7);
	const FUeAgentFrozenCommandContract* Contract =
		Fixture.Registry->FindExact(TEXT("query.calibration.read"));
	TestNotNull(TEXT("calibration read contract is exact"), Contract);
	if (!Contract)
	{
		return false;
	}

	for (const FString& Category : Categories)
	{
		const FString Params = FString::Printf(
			TEXT("{\"category\":\"%s\",\"limit\":1}"),
			*Category);
		const FUeAgentCommandResult Result = Submit(
			Fixture,
			Contract->GetCommandId(),
			Contract->GetContractHash(),
			Params);
		TestTrue(TEXT("each representative provider returns through exact dispatch"), Result.IsOk());
		TSharedPtr<FJsonObject> Payload;
		FString JsonError;
		TestTrue(
			TEXT("query result is canonical JSON"),
			UE::UAI::CommandContract::Json::TryParseObject(
				Result.GetPayloadJson(),
				Payload,
				JsonError));
		if (Payload.IsValid())
		{
			TestTrue(TEXT("query explicitly declares read-only"), Payload->GetBoolField(TEXT("read_only")));
			TestEqual(TEXT("query preserves its category"), Payload->GetStringField(TEXT("category")), Category);
			TestTrue(TEXT("query declares completeness"), Payload->HasTypedField<EJson::String>(TEXT("completeness")));
			TestTrue(TEXT("query declares a source"), Payload->HasTypedField<EJson::String>(TEXT("source_id")));
			TestTrue(TEXT("query declares a revision"), Payload->HasTypedField<EJson::String>(TEXT("revision")));
			TestTrue(TEXT("query returns at most the requested page"), Payload->GetArrayField(TEXT("items")).Num() <= 1);
		}
	}
	TestEqual(TEXT("every provider was read exactly once"), Fixture.Source->ReadCount, Categories.Num());
	TestEqual(TEXT("queries never invoke mutation"), Fixture.Source->MutationCount, 0);
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T05ExactFailureTest,
	"UAI.StrictV2.T05.QUERY.PUBLIC.CALIBRATION.ExactFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T05ExactFailureTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T05_QUERY_CALIBRATION_AVAILABLE
	TestTrue(TEXT("T05 query calibration is implemented"), false);
	return false;
#else
	FFixtureRegistration Fixture = MakeRegistration(this);
	const FUeAgentCommandResult Stale = Submit(
		Fixture,
		TEXT("query.calibration.read"),
		TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
		TEXT("{\"category\":\"runtime_probe\"}"));
	TestTrue(TEXT("stale exact binding is a protocol error"), Stale.IsProtocolError());
	TestTrue(TEXT("stale exact binding names the mismatch"), Stale.GetErrorJson().Contains(TEXT("command_contract_version_mismatch")));

	const FUeAgentCommandResult Unknown = Submit(
		Fixture,
		TEXT("query.calibration.unknown"),
		TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
		TEXT("{}"));
	TestTrue(TEXT("unknown exact id is a protocol error"), Unknown.IsProtocolError());
	TestTrue(TEXT("unknown exact id is not fuzzily resolved"), Unknown.GetErrorJson().Contains(TEXT("command_not_registered_exact")));
	return true;
#endif
}
