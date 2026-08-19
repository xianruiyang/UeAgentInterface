#include "Misc/AutomationTest.h"

#if __has_include("CommandRegistry/UeAgentActiveCommandManifest.h") \
	&& __has_include("Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h") \
	&& __has_include("Module/CommandRegistration/QueryCalibration/UeAgentQueryCalibrationRegistration.h")
#define UAI_T35_ACTIVE_COMMAND_COMPOSITION_AVAILABLE 1
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentActiveCommandManifest.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "DomainAdapters/QueryCalibration/UeAgentQueryCalibrationSource.h"
#include "HAL/PlatformMisc.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Module/CommandRegistration/QueryCalibration/UeAgentQueryCalibrationRegistration.h"
#else
#define UAI_T35_ACTIVE_COMMAND_COMPOSITION_AVAILABLE 0
#endif

#if UAI_T35_ACTIVE_COMMAND_COMPOSITION_AVAILABLE
namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Contract;
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
			if (!GetQueryCalibrationCategoryIds().Contains(Request.Category))
			{
				OutError.Code = TEXT("query_category_unknown");
				OutError.Detail = Request.Category;
				return false;
			}
			OutPage.Category = Request.Category;
			OutPage.SourceId = TEXT("strict_v2_t35_fixture");
			OutPage.StateLayer = TEXT("fixture_current_state");
			OutPage.Revision = TEXT("fixture-r1");
			OutPage.bReadOnly = true;
			OutPage.Completeness = EQueryCalibrationCompleteness::Complete;
			return true;
		}
	};

	FUeAgentCommandFieldDeclaration Field(
		const TCHAR* Name,
		const TCHAR* WireType,
		const bool bRequired,
		const TCHAR* Description)
	{
		return {Name, WireType, bRequired, false, Description};
	}

	FUeAgentCommandDeclaration SyntheticDeclaration()
	{
		FUeAgentCommandDeclaration Result;
		Result.CommandId = TEXT("query.synthetic.extra");
		Result.PythonNamespace = TEXT("synthetic");
		Result.PythonFunction = TEXT("extra");
		Result.ContractVersion = 1;
		Result.PublicOwner = TEXT("strict-v2-t35-synthetic-owner");
		Result.Availability = EUeAgentCommandAvailability::Enabled;
		Result.SideEffect = EUeAgentCommandSideEffect::None;
		Result.StateLayers = {TEXT("fixture_current_state")};
		Result.ResultFields = {
			Field(TEXT("synthetic"), TEXT("boolean"), true, TEXT("True for the composition fixture.")),
		};
		Result.Errors = {
			{TEXT("synthetic_failure"), TEXT("The synthetic handler failed.")},
		};
		Result.Purpose.Summary = TEXT("Read an extra synthetic command contributed outside query calibration.");
		Result.Purpose.Target = TEXT("The strict_v2 active composition fixture.");
		Result.Purpose.Preconditions = {TEXT("The fixture contribution is active.")};
		Result.Purpose.ParameterGuidance = TEXT("No parameters are accepted.");
		Result.Purpose.ResultGuidance = TEXT("Use the synthetic boolean only as test evidence.");
		Result.Purpose.FailureGuidance = TEXT("Failures are typed and never fall back.");
		Result.Purpose.SideEffectGuidance = TEXT("Read-only and fixture-local.");
		Result.Purpose.MinimumExample = TEXT("execute_exact('query.synthetic.extra', {})");
		Result.Purpose.RelatedFlows = {TEXT("query.command.search")};
		Result.Purpose.Keywords = {TEXT("synthetic"), TEXT("extra"), TEXT("composition")};
		Result.Purpose.Synonyms = {TEXT("extra contributed command")};
		return Result;
	}

	class FSyntheticHandler final : public IUeAgentExactCommandHandler
	{
	public:
		virtual const FString& GetCommandId() const override
		{
			static const FString Value = TEXT("query.synthetic.extra");
			return Value;
		}

		virtual const FString& GetPublicOwner() const override
		{
			static const FString Value = TEXT("strict-v2-t35-synthetic-owner");
			return Value;
		}

		virtual FUeAgentCommandResult Submit(
			const FUeAgentCommandRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
		{
		return FUeAgentCommandResult::Success(
			Request,
			TEXT("{\"synthetic\":true}"));
		}
	};

	class FSyntheticContribution final : public IUeAgentActiveCommandContribution
	{
	public:
		explicit FSyntheticContribution(FString InId = TEXT("strict-v2-t35-synthetic"))
			: Id(MoveTemp(InId))
		{
		}

		virtual const FString& GetContributionId() const override
		{
			return Id;
		}

		virtual void CollectDeclarations(
			TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
		{
			OutDeclarations.Add(SyntheticDeclaration());
		}

		virtual bool CreateHandlers(
			TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
			TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
			FString& OutError) const override
		{
			if (!Registry->FindExact(TEXT("query.command.search"))
				|| !Registry->FindExact(TEXT("query.synthetic.extra")))
			{
				OutError = TEXT("final_registry_not_visible_to_contribution");
				return false;
			}
			OutHandlers.Add(MakeShared<FSyntheticHandler>());
			return true;
		}

	private:
		FString Id;
	};

	bool DecodeRequest(
		const FString& CommandId,
		const FString& ContractHash,
		const FString& ParamsJson,
		TOptional<FUeAgentCommandRequest>& OutRequest)
	{
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t35\",")
			TEXT("\"request_id\":\"req-t35\",")
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

	FUeAgentCommandResult Submit(
		const TSharedRef<const FUeAgentExactCommandDispatch> Dispatch,
		const FUeAgentFrozenCommandContract& Contract,
		const FString& ParamsJson)
	{
		TOptional<FUeAgentCommandRequest> Request;
		if (!DecodeRequest(
			Contract.GetCommandId(),
			Contract.GetContractHash(),
			ParamsJson,
			Request)
			|| !Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t35"),
				TEXT("req-t35"),
				Contract.GetCommandId(),
				Contract.GetContractHash(),
				TEXT("test_request_decode_failed"),
				TEXT("strict_v2 fixture could not decode its request"));
		}
		return Dispatch->Submit(
			Request.GetValue(),
			FUeAgentCallerContext(
				TEXT("strict-v2-t35"),
				{TEXT("uai:query")},
				TEXT("corr-t35")),
			MakeShared<FNeverCancelled>());
	}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T35TwoPhaseCompositionTest,
	"UAI.StrictV2.T35.ACTIVE.COMMAND.COMPOSITION.TwoPhase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T35TwoPhaseCompositionTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T35_ACTIVE_COMMAND_COMPOSITION_AVAILABLE
	TestTrue(TEXT("T35 active command composition is implemented"), false);
	return false;
#else
	FUeAgentActiveCommandComposition Composition;
	FString Error;
	TSharedPtr<IUeAgentActiveCommandContribution> QueryContribution;
	TestTrue(
		TEXT("query calibration exposes a contribution instead of freezing a private registry"),
		TryCreateQueryCalibrationContribution(
			MakeShared<FFixtureQuerySource>(),
			QueryContribution,
			Error));
	if (!QueryContribution.IsValid())
	{
		return false;
	}
	TestTrue(
		TEXT("query contribution is accepted before sealing"),
		Composition.TryAdd(QueryContribution.ToSharedRef(), Error));
	TestTrue(
		TEXT("an independent domain contribution is accepted before sealing"),
		Composition.TryAdd(MakeShared<FSyntheticContribution>(), Error));

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	TestTrue(
		TEXT("all declarations freeze once before handlers are created"),
		Composition.TryCompose(Registry, Dispatch, Error));
	TestTrue(TEXT("composition is sealed after compose"), Composition.IsSealed());
	TestTrue(TEXT("one immutable registry is produced"), Registry.IsValid());
	TestTrue(TEXT("one exact dispatch is produced"), Dispatch.IsValid());
	if (!Registry.IsValid() || !Dispatch.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("the final registry contains both contributions"), Registry->GetContracts().Num(), 4);

	const FUeAgentFrozenCommandContract* Search =
		Registry->FindExact(TEXT("query.command.search"));
	TestNotNull(TEXT("query discovery remains in the aggregate registry"), Search);
	if (!Search)
	{
		return false;
	}
	const FUeAgentCommandResult SearchResult = Submit(
		Dispatch.ToSharedRef(),
		*Search,
		TEXT("{\"intent\":\"extra synthetic command\",\"limit\":3}"));
	TestTrue(TEXT("query discovery dispatch succeeds against the aggregate registry"), SearchResult.IsOk());
	TestTrue(
		TEXT("T05 discovery sees commands from later independent contributions"),
		SearchResult.GetPayloadJson().Contains(TEXT("query.synthetic.extra")));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T35SealingFailureTest,
	"UAI.StrictV2.T35.ACTIVE.COMMAND.COMPOSITION.SealingFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T35SealingFailureTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T35_ACTIVE_COMMAND_COMPOSITION_AVAILABLE
	TestTrue(TEXT("T35 active command composition is implemented"), false);
	return false;
#else
	FString Error;
	FUeAgentActiveCommandComposition DuplicateContribution;
	TestTrue(
		TEXT("first contribution id is accepted"),
		DuplicateContribution.TryAdd(
			MakeShared<FSyntheticContribution>(),
			Error));
	TestFalse(
		TEXT("duplicate contribution id is rejected before compose"),
		DuplicateContribution.TryAdd(
			MakeShared<FSyntheticContribution>(),
			Error));
	TestTrue(
		TEXT("duplicate contribution failure is typed"),
		Error.StartsWith(TEXT("active_command_contribution_duplicate:")));

	FUeAgentActiveCommandComposition DuplicateCommand;
	TestTrue(
		TEXT("first command contribution is accepted"),
		DuplicateCommand.TryAdd(
			MakeShared<FSyntheticContribution>(TEXT("synthetic-a")),
			Error));
	TestTrue(
		TEXT("different contribution id may declare independently"),
		DuplicateCommand.TryAdd(
			MakeShared<FSyntheticContribution>(TEXT("synthetic-b")),
			Error));
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	TestFalse(
		TEXT("duplicate command declarations prevent activation"),
		DuplicateCommand.TryCompose(Registry, Dispatch, Error));
	TestTrue(TEXT("failed compose still seals the catalog"), DuplicateCommand.IsSealed());
	TestFalse(TEXT("failed compose publishes no registry"), Registry.IsValid());
	TestFalse(TEXT("failed compose publishes no dispatch"), Dispatch.IsValid());

	TestFalse(
		TEXT("late contributions are rejected after the activation boundary"),
		DuplicateCommand.TryAdd(
			MakeShared<FSyntheticContribution>(TEXT("synthetic-late")),
			Error));
	TestTrue(
		TEXT("late contribution failure is typed"),
		Error.StartsWith(TEXT("active_command_composition_sealed:")));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T35FinalManifestAuthorityTest,
	"UAI.StrictV2.T35.ACTIVE.COMMAND.COMPOSITION.FinalManifestAuthority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T35FinalManifestAuthorityTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T35_ACTIVE_COMMAND_COMPOSITION_AVAILABLE
	TestTrue(TEXT("T35 final-registry manifest projection is implemented"), false);
	return false;
#else
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	TestTrue(
		TEXT("the process composes one final immutable registry"),
		TryComposeProcessActiveCommandComposition(
			Registry,
			Dispatch,
			Error));
	if (!Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(FString::Printf(
			TEXT("process composition unavailable: %s"),
			*Error));
		return false;
	}

	TestTrue(
		TEXT("the final registry contains the post-migration command surface"),
		Registry->GetContracts().Num() > 329);
	TestNull(
		TEXT("the replaced catalog command is absent from the final authority"),
		Registry->FindExact(TEXT("reference.catalog_search")));
	TestNotNull(
		TEXT("intent discovery is in the final authority"),
		Registry->FindExact(TEXT("query.command.search")));
	TestNotNull(
		TEXT("usage detail is in the final authority"),
		Registry->FindExact(TEXT("query.command.detail")));
	TestNotNull(
		TEXT("calibration readback is in the final authority"),
		Registry->FindExact(TEXT("query.calibration.read")));

	const FString ManifestPath = GetDefaultCommandManifestPath();
	TestTrue(TEXT("the formal manifest path is available"), !ManifestPath.IsEmpty());
	const bool bUpdateManifest =
		FPlatformMisc::GetEnvironmentVariable(
			TEXT("UAI_UPDATE_COMMAND_MANIFEST")) == TEXT("1");
	if (bUpdateManifest)
	{
		TestTrue(
			TEXT("the explicit development update path writes only the final-registry projection"),
			FUeAgentActiveCommandManifest::TryWrite(
				Registry.ToSharedRef(),
				GetUaiCommandManifestPluginVersion(),
				GetUaiCommandManifestBuildId(),
				ManifestPath,
				Error));
	}
	TestTrue(
		TEXT("the committed formal manifest is an exact projection of the final registry"),
		FUeAgentActiveCommandManifest::TryVerify(
			Registry.ToSharedRef(),
			GetUaiCommandManifestPluginVersion(),
			GetUaiCommandManifestBuildId(),
			ManifestPath,
			Error));
	if (!Error.IsEmpty())
	{
		AddError(FString::Printf(
			TEXT("final manifest verification failed: %s"),
			*Error));
	}

	FUeAgentActiveCommandComposition SyntheticComposition;
	TSharedPtr<IUeAgentActiveCommandContribution> SyntheticQueryContribution;
	TestTrue(
		TEXT("the mismatch fixture includes the query discovery contribution"),
		TryCreateQueryCalibrationContribution(
			MakeShared<FFixtureQuerySource>(),
			SyntheticQueryContribution,
			Error));
	if (!SyntheticQueryContribution.IsValid())
	{
		return false;
	}
	TestTrue(
		TEXT("the mismatch fixture accepts query discovery"),
		SyntheticComposition.TryAdd(
			SyntheticQueryContribution.ToSharedRef(),
			Error));
	TestTrue(
		TEXT("a deliberately different contribution can be composed"),
		SyntheticComposition.TryAdd(
			MakeShared<FSyntheticContribution>(),
			Error));
	TSharedPtr<const FUeAgentImmutableCommandRegistry> SyntheticRegistry;
	TSharedPtr<const FUeAgentExactCommandDispatch> SyntheticDispatch;
	TestTrue(
		TEXT("the deliberately different registry freezes"),
		SyntheticComposition.TryCompose(
			SyntheticRegistry,
			SyntheticDispatch,
			Error));
	if (!SyntheticRegistry.IsValid())
	{
		return false;
	}
	TestFalse(
		TEXT("a registry mismatch is rejected instead of publishing a partial surface"),
		FUeAgentActiveCommandManifest::TryVerify(
			SyntheticRegistry.ToSharedRef(),
			GetUaiCommandManifestPluginVersion(),
			GetUaiCommandManifestBuildId(),
			ManifestPath,
			Error));
	TestEqual(
		TEXT("the mismatch has a stable failure code"),
		Error,
		FString(TEXT("active_command_manifest_projection_mismatch")));
	return true;
#endif
}
