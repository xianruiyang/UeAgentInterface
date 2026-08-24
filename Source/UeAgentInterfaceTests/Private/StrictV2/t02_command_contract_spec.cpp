#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if __has_include("Core/Contract/Command/UeAgentCommandContractModel.h") \
	&& __has_include("CommandRegistry/UeAgentImmutableCommandRegistry.h") \
	&& __has_include("CommandEdge/UeAgentExactCommandDispatch.h")
#define UAI_STRICT_V2_T02_IMPLEMENTATION_AVAILABLE 1
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/Command/UeAgentCommandContractModel.h"
#else
#define UAI_STRICT_V2_T02_IMPLEMENTATION_AVAILABLE 0
#endif

namespace
{
	static FString ProductSource(const FString& RelativePath)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT("UeAgentInterface/Source/UeAgentInterface"),
			RelativePath));
	}

	static bool LoadSource(
		FAutomationTestBase& Test,
		const FString& RelativePath,
		FString& OutSource)
	{
		return Test.TestTrue(
			*FString::Printf(TEXT("load source %s"), *RelativePath),
			FFileHelper::LoadFileToString(
				OutSource,
				*ProductSource(RelativePath)));
	}

#if UAI_STRICT_V2_T02_IMPLEMENTATION_AVAILABLE
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Contract;

	static FUeAgentCommandDeclaration MakeDeclaration(
		const FString& CommandId = TEXT("sample.execute"),
		const FString& PublicOwner = TEXT("sample-operation-owner"))
	{
		FUeAgentCommandDeclaration Result;
		Result.CommandId = CommandId;
		Result.PythonNamespace = TEXT("sample");
		Result.PythonFunction = CommandId == TEXT("sample.execute")
			? TEXT("execute")
			: TEXT("inspect");
		Result.ContractVersion = 3;
		Result.PublicOwner = PublicOwner;
		Result.Availability = EUeAgentCommandAvailability::Enabled;
		Result.SideEffect = EUeAgentCommandSideEffect::AssetState;
		Result.StateLayers = {TEXT("persisted_asset")};
		Result.InputFields = {
			{
				TEXT("target"),
				TEXT("string"),
				true,
				false,
				TEXT("Stable target identity returned by public discovery."),
			},
		};
		Result.ResultFields = {
			{
				TEXT("revision"),
				TEXT("string"),
				true,
				false,
				TEXT("Persisted revision independently read after execution."),
			},
		};
		Result.Errors = {
			{
				TEXT("sample_target_missing"),
				TEXT("The exact target does not exist in the selected state layer."),
			},
		};
		Result.Purpose.Summary = TEXT("Apply one exact sample operation.");
		Result.Purpose.Target = TEXT("A stable sample target identity.");
		Result.Purpose.Preconditions = {
			TEXT("The target identity was obtained from public discovery."),
		};
		Result.Purpose.ParameterGuidance =
			TEXT("Pass the exact target identity without alias mapping.");
		Result.Purpose.ResultGuidance =
			TEXT("Returns the independently read persisted revision.");
		Result.Purpose.FailureGuidance =
			TEXT("Failures preserve the declared error code and do not fall back.");
		Result.Purpose.SideEffectGuidance =
			TEXT("Mutates the persisted asset state exactly once.");
		Result.Purpose.MinimumExample =
			TEXT("sample.execute(target='/Game/Sample.Sample')");
		Result.Purpose.RelatedFlows = {TEXT("sample.discover")};
		Result.Purpose.Keywords = {TEXT("sample"), TEXT("apply")};
		Result.Purpose.Synonyms = {TEXT("update sample")};
		Result.TutorialReferences.Add(
			TEXT("uai-docs:sample-command#execute"));
		return Result;
	}

	class FNeverCancelled final : public IUeAgentCancellationSignal
	{
	public:
		virtual bool IsCancelled() const override
		{
			return false;
		}
	};

	class FRecordingExactHandler final : public IUeAgentExactCommandHandler
	{
	public:
		FRecordingExactHandler(FString InCommandId, FString InOwner)
			: CommandId(MoveTemp(InCommandId))
			, Owner(MoveTemp(InOwner))
		{
		}

		virtual const FString& GetCommandId() const override
		{
			return CommandId;
		}

		virtual const FString& GetPublicOwner() const override
		{
			return Owner;
		}

		virtual FUeAgentCommandResult Submit(
			const FUeAgentCommandRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
		{
			++Calls;
			return FUeAgentCommandResult::Success(
				Request,
				TEXT("{\"revision\":\"revision-4\"}"));
		}

		int32 Calls = 0;

	private:
		FString CommandId;
		FString Owner;
	};

	static TOptional<FUeAgentCommandRequest> RequestFor(
		const FString& CommandId,
		const FString& ContractHash)
	{
		const FString Body =
			TEXT("{\"command_binding_hash\":\"")
			+ ContractHash
			+ TEXT("\",\"command_id\":\"")
			+ CommandId
			+ TEXT("\",\"correlation_id\":\"corr-t02\",")
			  TEXT("\"message_kind\":\"command_request\",")
			  TEXT("\"params\":{\"target\":\"/Game/Sample.Sample\"},")
			  TEXT("\"request_id\":\"req-t02\",")
			  TEXT("\"schema\":\"uai.command.v1\",\"version\":1}");
		const FTCHARToUTF8 Converted(*Body);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Converted.Get()),
			Converted.Length());
		FUeAgentCommandProtocolCodec Codec;
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		TOptional<FUeAgentCommandRequest> Result;
		Codec.DecodeRequest(Bytes, Result, Diagnostic);
		return Result;
	}
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUaiStrictV2T02RegistryProjectionTest,
	"UAI.StrictV2.T02.COMMAND.CONTRACT.RegistryProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUaiStrictV2T02RegistryProjectionTest::RunTest(const FString& Parameters)
{
#if !UAI_STRICT_V2_T02_IMPLEMENTATION_AVAILABLE
	AddError(TEXT("strict_v2_t02_immutable_command_registry_missing"));
	return false;
#else
	FUeAgentCommandDeclaration Source = MakeDeclaration();
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	FString Error;
	TestTrue(
		TEXT("complete declarations freeze into the sole registry"),
		FUeAgentImmutableCommandRegistry::TryFreeze(
			{Source},
			Registry,
			Error));
	if (!Registry.IsValid())
	{
		AddError(Error);
		return false;
	}

	const FUeAgentFrozenCommandContract* Contract =
		Registry->FindExact(TEXT("sample.execute"));
	TestNotNull(TEXT("exact command is present"), Contract);
	if (!Contract)
	{
		return false;
	}
	TestEqual(TEXT("contract version is owned"), Contract->GetContractVersion(), 3);
	TestEqual(
		TEXT("public owner is owned"),
		Contract->GetPublicOwner(),
		FString(TEXT("sample-operation-owner")));
	TestEqual(TEXT("input schema is owned"), Contract->GetInputFields().Num(), 1);
	TestEqual(TEXT("result schema is owned"), Contract->GetResultFields().Num(), 1);
	TestEqual(TEXT("error set is owned"), Contract->GetErrors().Num(), 1);
	TestEqual(TEXT("state layers are owned"), Contract->GetStateLayers().Num(), 1);
	TestEqual(
		TEXT("purpose contract is owned"),
		Contract->GetPurpose().MinimumExample,
		Source.Purpose.MinimumExample);
	TestEqual(
		TEXT("tutorial references are owned"),
		Contract->GetTutorialReferences(),
		Source.TutorialReferences);

	FUeAgentCommandDeclaration TutorialChanged = Source;
	TutorialChanged.TutorialReferences.Add(
		TEXT("uai-docs:sample-command#advanced"));
	TSharedPtr<const FUeAgentImmutableCommandRegistry>
		TutorialChangedRegistry;
	TestTrue(
		TEXT("tutorial metadata change freezes"),
		FUeAgentImmutableCommandRegistry::TryFreeze(
			{TutorialChanged},
			TutorialChangedRegistry,
			Error));
	if (TutorialChangedRegistry.IsValid())
	{
		const FUeAgentFrozenCommandContract* TutorialChangedContract =
			TutorialChangedRegistry->FindExact(TEXT("sample.execute"));
		TestNotNull(
			TEXT("tutorial metadata variant remains executable"),
			TutorialChangedContract);
		if (TutorialChangedContract)
		{
			TestEqual(
				TEXT("tutorial metadata does not change executable binding hash"),
				TutorialChangedContract->GetContractHash(),
				Contract->GetContractHash());
			TestTrue(
				TEXT("tutorial metadata changes registry identity"),
				TutorialChangedRegistry->GetRegistryHash()
					!= Registry->GetRegistryHash());
		}
	}

	Source.CommandId = TEXT("mutated.after.freeze");
	Source.Purpose.Summary = TEXT("mutated after freeze");
	TestNotNull(
		TEXT("registry is detached from mutable declarations"),
		Registry->FindExact(TEXT("sample.execute")));
	TestNull(
		TEXT("post-freeze mutation cannot add an executable identity"),
		Registry->FindExact(TEXT("mutated.after.freeze")));

	FString Projection;
	TestTrue(
		TEXT("registry emits its deterministic projection"),
		Registry->BuildProjection(Projection, Error));
	TestTrue(
		TEXT("projection carries C++ registry provenance"),
		Projection.Contains(TEXT("cpp_immutable_command_registry")));
	for (const FString& Required : {
		TEXT("sample.execute"),
		TEXT("sample-operation-owner"),
		TEXT("contract_version"),
		TEXT("input_fields"),
		TEXT("result_fields"),
		TEXT("errors"),
		TEXT("side_effect"),
		TEXT("state_layers"),
		TEXT("purpose"),
		TEXT("minimum_example"),
		TEXT("tutorial_references"),
		TEXT("uai-docs:sample-command#execute"),
	})
	{
		TestTrue(
			*FString::Printf(TEXT("projection contains %s"), *Required),
			Projection.Contains(Required));
	}
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUaiStrictV2T02ValidationFailuresTest,
	"UAI.StrictV2.T02.COMMAND.CONTRACT.ValidationFailures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUaiStrictV2T02ValidationFailuresTest::RunTest(const FString& Parameters)
{
#if !UAI_STRICT_V2_T02_IMPLEMENTATION_AVAILABLE
	AddError(TEXT("strict_v2_t02_contract_validation_missing"));
	return false;
#else
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Rejected;
	FString Error;
	FUeAgentCommandDeclaration Incomplete = MakeDeclaration();
	Incomplete.Purpose.FailureGuidance.Reset();
	TestFalse(
		TEXT("incomplete purpose contracts are rejected"),
		FUeAgentImmutableCommandRegistry::TryFreeze(
			{Incomplete}, Rejected, Error));
	TestEqual(
		TEXT("incomplete contract has a stable error"),
		Error,
		FString(TEXT("command_contract_incomplete")));

	FUeAgentCommandDeclaration First = MakeDeclaration();
	FUeAgentCommandDeclaration DuplicateId = MakeDeclaration();
	DuplicateId.PythonFunction = TEXT("different_function");
	TestFalse(
		TEXT("duplicate exact command identities are rejected"),
		FUeAgentImmutableCommandRegistry::TryFreeze(
			{First, DuplicateId}, Rejected, Error));
	TestEqual(
		TEXT("duplicate identity has a stable error"),
		Error,
		FString(TEXT("command_contract_id_duplicate")));

	FUeAgentCommandDeclaration DuplicateBinding =
		MakeDeclaration(TEXT("sample.inspect"), TEXT("sample-query-owner"));
	DuplicateBinding.PythonFunction = TEXT("execute");
	TestFalse(
		TEXT("two commands cannot own one Python binding"),
		FUeAgentImmutableCommandRegistry::TryFreeze(
			{First, DuplicateBinding}, Rejected, Error));
	TestEqual(
		TEXT("duplicate binding has a stable error"),
		Error,
		FString(TEXT("command_python_binding_duplicate")));

	FUeAgentCommandDeclaration DuplicateError = MakeDeclaration();
	const FUeAgentCommandErrorDeclaration ErrorCopy =
		DuplicateError.Errors[0];
	DuplicateError.Errors.Add(ErrorCopy);
	TestFalse(
		TEXT("duplicate declared errors are rejected"),
		FUeAgentImmutableCommandRegistry::TryFreeze(
			{DuplicateError}, Rejected, Error));
	TestEqual(
		TEXT("duplicate error has a stable error"),
		Error,
		FString(TEXT("command_contract_error_duplicate")));

	FUeAgentCommandDeclaration InvalidSideEffect = MakeDeclaration();
	InvalidSideEffect.SideEffect =
		static_cast<EUeAgentCommandSideEffect>(255);
	TestFalse(
		TEXT("unknown side-effect values are rejected before freeze"),
		FUeAgentImmutableCommandRegistry::TryFreeze(
			{InvalidSideEffect}, Rejected, Error));
	TestEqual(
		TEXT("unknown side effect has a stable error"),
		Error,
		FString(TEXT("command_contract_side_effect_invalid")));

	FUeAgentCommandDeclaration MissingTutorial = MakeDeclaration();
	MissingTutorial.TutorialReferences.Reset();
	TestFalse(
		TEXT("commands without tutorial references are rejected"),
		FUeAgentImmutableCommandRegistry::TryFreeze(
			{MissingTutorial}, Rejected, Error));
	TestEqual(
		TEXT("missing tutorial reference has a stable error"),
		Error,
		FString(TEXT("command_tutorial_reference_invalid")));

	FUeAgentCommandDeclaration MalformedTutorial = MakeDeclaration();
	MalformedTutorial.TutorialReferences = {
		TEXT("uai-docs:sample-command"),
	};
	TestFalse(
		TEXT("malformed tutorial references are rejected"),
		FUeAgentImmutableCommandRegistry::TryFreeze(
			{MalformedTutorial}, Rejected, Error));
	TestEqual(
		TEXT("malformed tutorial reference has a stable error"),
		Error,
		FString(TEXT("command_tutorial_reference_invalid")));
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUaiStrictV2T02ExactUniqueDispatchTest,
	"UAI.StrictV2.T02.COMMAND.CONTRACT.ExactUniqueDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUaiStrictV2T02ExactUniqueDispatchTest::RunTest(const FString& Parameters)
{
#if !UAI_STRICT_V2_T02_IMPLEMENTATION_AVAILABLE
	AddError(TEXT("strict_v2_t02_exact_dispatch_missing"));
	return false;
#else
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	FString Error;
	TestTrue(
		TEXT("registry freezes"),
		FUeAgentImmutableCommandRegistry::TryFreeze(
			{MakeDeclaration()}, Registry, Error));
	const FUeAgentFrozenCommandContract* Contract =
		Registry->FindExact(TEXT("sample.execute"));
	if (!Contract)
	{
		AddError(TEXT("fixture contract missing"));
		return false;
	}

	const TSharedRef<FRecordingExactHandler> Handler =
		MakeShared<FRecordingExactHandler>(
			TEXT("sample.execute"),
			TEXT("sample-operation-owner"));
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	TestTrue(
		TEXT("one exact handler and one public owner compose"),
		FUeAgentExactCommandDispatch::TryCreate(
			Registry.ToSharedRef(), {Handler}, Dispatch, Error));
	if (!Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const TOptional<FUeAgentCommandRequest> Exact =
		RequestFor(TEXT("sample.execute"), Contract->GetContractHash());
	TestTrue(TEXT("exact request decodes"), Exact.IsSet());
	const FUeAgentCallerContext Caller(
		TEXT("strict-v2-t02"),
		{TEXT("uai:operation")},
		TEXT("corr-t02"));
	FUeAgentCommandResult Result = Dispatch->Submit(
		Exact.GetValue(), Caller, MakeShared<FNeverCancelled>());
	TestTrue(TEXT("exact request reaches its sole handler"), Result.IsOk());
	TestEqual(TEXT("handler runs exactly once"), Handler->Calls, 1);

	const TOptional<FUeAgentCommandRequest> Prefix =
		RequestFor(TEXT("sample"), Contract->GetContractHash());
	Result = Dispatch->Submit(
		Prefix.GetValue(), Caller, MakeShared<FNeverCancelled>());
	TestFalse(TEXT("prefix cannot execute"), Result.IsOk());
	TestEqual(TEXT("prefix does not reach handler"), Handler->Calls, 1);

	const TOptional<FUeAgentCommandRequest> Alias =
		RequestFor(TEXT("sample.update"), Contract->GetContractHash());
	Result = Dispatch->Submit(
		Alias.GetValue(), Caller, MakeShared<FNeverCancelled>());
	TestFalse(TEXT("undeclared alias cannot execute"), Result.IsOk());
	TestEqual(TEXT("alias does not reach handler"), Handler->Calls, 1);

	const TOptional<FUeAgentCommandRequest> Stale =
		RequestFor(
			TEXT("sample.execute"),
			TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"));
	Result = Dispatch->Submit(
		Stale.GetValue(), Caller, MakeShared<FNeverCancelled>());
	TestFalse(TEXT("stale contract projection cannot execute"), Result.IsOk());
	TestEqual(TEXT("stale binding does not reach handler"), Handler->Calls, 1);

	TSharedPtr<const FUeAgentExactCommandDispatch> Rejected;
	const TSharedRef<FRecordingExactHandler> WrongOwner =
		MakeShared<FRecordingExactHandler>(
			TEXT("sample.execute"),
			TEXT("parallel-owner"));
	TestFalse(
		TEXT("a handler with a second public owner is rejected"),
		FUeAgentExactCommandDispatch::TryCreate(
			Registry.ToSharedRef(), {WrongOwner}, Rejected, Error));
	TestEqual(
		TEXT("owner mismatch has a stable error"),
		Error,
		FString(TEXT("command_handler_owner_mismatch")));
	TestFalse(
		TEXT("duplicate handlers are rejected"),
		FUeAgentExactCommandDispatch::TryCreate(
			Registry.ToSharedRef(), {Handler, Handler}, Rejected, Error));
	TestFalse(
		TEXT("missing enabled handler is rejected"),
		FUeAgentExactCommandDispatch::TryCreate(
			Registry.ToSharedRef(), {}, Rejected, Error));
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUaiStrictV2T02ActiveAuthorityBoundaryTest,
	"UAI.StrictV2.T02.COMMAND.CONTRACT.ActiveAuthorityBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUaiStrictV2T02ActiveAuthorityBoundaryTest::RunTest(const FString& Parameters)
{
	FString Registration;
	if (!LoadSource(
		*this,
		TEXT("Private/Module/CommandRegistration/Core/UeAgentCoreCommandRegistration.cpp"),
		Registration))
	{
		AddError(TEXT("strict_v2_t02_core_registration_missing"));
		return false;
	}
	TestTrue(
		TEXT("core registration consumes the immutable registry"),
		Registration.Contains(TEXT("FUeAgentImmutableCommandRegistry")));
	TestTrue(
		TEXT("core registration consumes the exact dispatcher"),
		Registration.Contains(TEXT("FUeAgentExactCommandDispatch")));
	TestFalse(
		TEXT("core registration has no prefix, fallback, or alias execution path"),
		Registration.Contains(TEXT("StartsWith"))
			|| Registration.Contains(TEXT("fallback"))
			|| Registration.Contains(TEXT("alias")));
	return !HasAnyErrors();
}
