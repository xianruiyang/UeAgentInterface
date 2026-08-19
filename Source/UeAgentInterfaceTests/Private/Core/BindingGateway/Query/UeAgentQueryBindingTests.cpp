#include "Misc/AutomationTest.h"

#include "Core/BindingGateway/Query/UeAgentQueryBinding.h"


#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	using namespace UE::UAI::BindingGateway::Query;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::StateSemantic;

	static FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(),
			TEXT(
				"Plugins/UeAgentInterface/Source/UeAgentInterfaceTests/"
				"Private/Core/BindingGateway/Query/Fixtures/"
				"query_binding_cases.json")));
	}

	static TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load shared C3Q fixture"),
				FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse shared C3Q fixture"),
				FJsonSerializer::Deserialize(Reader, Fixture)
					&& Fixture.IsValid()))
		{
			return nullptr;
		}
		Test->TestEqual(
			TEXT("fixture schema"),
			Fixture->GetStringField(TEXT("schema")),
			FString(TEXT("uai.c3q.query_binding.test_fixture.v1")));
		return Fixture;
	}

	static FString JsonObjectText(const TSharedPtr<FJsonObject>& Object)
	{
		FString Result;
		const TSharedRef<
			TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<
				TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Result;
	}

	static FUeAgentDescriptorIdentity FixtureDescriptor()
	{
		return FUeAgentDescriptorIdentity(
			TEXT("fixture.query_binding.read"),
			1,
			TEXT("sha256:1111111111111111111111111111111111111111111111111111111111111111"));
	}

	static FUeAgentDescriptorIdentity FixtureCandidateDescriptor()
	{
		return FUeAgentDescriptorIdentity(
			TEXT("fixture.reference_asset.update"),
			1,
			TEXT("sha256:56e13b1eceeae251e8d6f1d18b993c3949420e5524adf18ede12b652cfdc666b"));
	}

	static FUeAgentQuerySessionIdentity FixtureSession(
		const TSharedPtr<FJsonObject>& Fixture)
	{
		const TSharedPtr<FJsonObject> Row =
			Fixture->GetObjectField(TEXT("payload_codec"))
				->GetObjectField(TEXT("catalog_search_request"))
				->GetObjectField(TEXT("session"));
		return FUeAgentQuerySessionIdentity(
			Row->GetStringField(TEXT("server_session_id")),
			static_cast<int64>(
				Row->GetNumberField(TEXT("registry_epoch"))),
			Row->GetStringField(TEXT("registry_hash")));
	}

	static FUeAgentCandidateBinding FixtureBinding(
		const TSharedPtr<FJsonObject>& Fixture)
	{
		const TSharedPtr<FJsonObject> Catalog =
			Fixture->GetObjectField(TEXT("catalog"));
		const TSharedPtr<FJsonObject> Candidate =
			Catalog->GetObjectField(TEXT("candidate"));
		const TSharedPtr<FJsonObject> Identity =
			Candidate->GetObjectField(TEXT("identity"));
		const TSharedPtr<FJsonObject> Revision =
			Catalog->GetObjectField(TEXT("revision"));

		TArray<FString> CategoryPath;
		for (const TSharedPtr<FJsonValue>& Value :
			Candidate->GetArrayField(TEXT("category_path")))
		{
			CategoryPath.Add(Value->AsString());
		}
		TArray<EUeAgentCandidateOperation> Operations;
		for (const TSharedPtr<FJsonValue>& Value :
			Candidate->GetArrayField(TEXT("supported_operations")))
		{
			if (Value->AsString() == TEXT("read"))
			{
				Operations.Add(EUeAgentCandidateOperation::Read);
			}
			else if (Value->AsString() == TEXT("update"))
			{
				Operations.Add(EUeAgentCandidateOperation::Update);
			}
		}

		return FUeAgentCandidateBinding(
			Candidate->GetStringField(TEXT("binding_id")),
			FUeAgentCandidateSummary(
				FUeAgentCanonicalIdentity(
					Identity->GetStringField(TEXT("semantic_id")),
					Identity->GetStringField(TEXT("target_kind")),
					Identity->GetStringField(TEXT("owner_id")),
					Identity->GetStringField(TEXT("selector")),
					Identity->GetStringField(TEXT("object_role")),
					Identity->GetStringField(TEXT("generation"))),
				FixtureCandidateDescriptor(),
				Catalog->GetStringField(TEXT("catalog_kind")),
				Candidate->GetStringField(TEXT("display_name")),
				Candidate->GetStringField(TEXT("summary")),
				MoveTemp(CategoryPath)),
			FUeAgentCatalogRevision(
				Revision->GetStringField(TEXT("source_id")),
				Revision->GetStringField(TEXT("generation")),
				static_cast<int64>(
					Revision->GetNumberField(TEXT("sequence")))),
			Candidate->GetStringField(TEXT("public_ref")),
			Candidate->GetStringField(TEXT("read_port_id")),
			MoveTemp(Operations),
			Candidate->GetStringField(TEXT("binding_evidence")));
	}

	static FUeAgentCallerContext FixtureCaller()
	{
		return FUeAgentCallerContext(
			TEXT("fixture-agent"),
			{TEXT("generic:read")},
			TEXT("corr-query-binding-1"));
	}

	static FUeAgentResourceTracker MakeTracker(
		const TSharedPtr<FJsonObject>& Fixture)
	{
		const TSharedPtr<FJsonObject> Budget =
			Fixture->GetObjectField(TEXT("resource_budget"));
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes,
				static_cast<int64>(
					Budget->GetNumberField(TEXT("string_bytes")))},
			{EUeAgentResourceDimension::OutputBytes,
				static_cast<int64>(
					Budget->GetNumberField(TEXT("string_bytes")))},
			{EUeAgentResourceDimension::Nodes,
				static_cast<int64>(
					Budget->GetNumberField(TEXT("nodes")))},
			{EUeAgentResourceDimension::Items,
				static_cast<int64>(
					Budget->GetNumberField(TEXT("items")))},
			{EUeAgentResourceDimension::StringBytes,
				static_cast<int64>(
					Budget->GetNumberField(TEXT("string_bytes")))},
			{EUeAgentResourceDimension::WorkUnits,
				static_cast<int64>(
					Budget->GetNumberField(TEXT("work_units")))},
			{EUeAgentResourceDimension::EstimatedMemoryBytes,
				static_cast<int64>(
					Budget->GetNumberField(TEXT("estimated_memory_bytes")))},
		}));
	}

	static FUeAgentProjectionRequest FixtureProjection(
		const TSharedPtr<FJsonObject>& Fixture)
	{
		const TSharedPtr<FJsonObject> Row =
			Fixture->GetObjectField(TEXT("projection"));
		return FUeAgentProjectionRequest(
			FUeAgentProjectionProfile(
				Row->GetStringField(TEXT("profile_id")),
				Row->GetBoolField(TEXT("include_defaults")),
				Row->GetBoolField(TEXT("include_details")),
				Row->GetBoolField(TEXT("include_provenance"))),
			FUeAgentPageRequest(
				static_cast<int32>(
					Row->GetNumberField(TEXT("max_items"))),
				TEXT("")),
			TOptional<int64>(static_cast<int64>(
				Row->GetNumberField(TEXT("token_budget")))),
			{},
			{});
	}

	static TSharedPtr<const FUeAgentCatalogSnapshot> FixtureCatalog(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Fixture,
		FUeAgentResourceTracker& Resources)
	{
		const FUeAgentCandidateBinding Binding = FixtureBinding(Fixture);
		TSharedPtr<const FUeAgentCatalogSnapshot> Catalog;
		FUeAgentDiscoveryError Error;
		Test->TestTrue(
			TEXT("create typed C2 fixture catalog"),
			FUeAgentCatalogSnapshot::TryCreate(
				Fixture->GetObjectField(TEXT("catalog"))
					->GetStringField(TEXT("catalog_id")),
				Fixture->GetObjectField(TEXT("catalog"))
					->GetStringField(TEXT("catalog_kind")),
				Binding.GetCatalogRevision(),
				EUeAgentCatalogCompleteness::Complete,
				{Binding.GetSummary()},
				{},
				Resources,
				Catalog,
				Error));
		return Catalog;
	}

	static TSharedPtr<const FUeAgentCanonicalSnapshot> FixtureSnapshot(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Fixture,
		FUeAgentResourceTracker& Resources)
	{
		const TSharedPtr<FJsonObject> Row =
			Fixture->GetObjectField(TEXT("snapshot"));
		const TSharedPtr<FJsonObject> RevisionRow =
			Row->GetObjectField(TEXT("revision"));
		const FUeAgentStateRevision Revision(
			RevisionRow->GetStringField(TEXT("source_id")),
			RevisionRow->GetStringField(TEXT("generation")),
			static_cast<int64>(
				RevisionRow->GetNumberField(TEXT("sequence"))));
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError StateError;
		Test->TestTrue(
			TEXT("create typed C4 fixture value"),
			FUeAgentCanonicalValue::TryString(
				Row->GetStringField(TEXT("value")),
				Resources,
				Value,
				StateError));
		const FUeAgentCandidateBinding Binding = FixtureBinding(Fixture);
		const FUeAgentStateEntry Entry(
			Binding.GetSummary().GetIdentity(),
			FUeAgentCanonicalType(
				TEXT("core.string"),
				EUeAgentValueKind::String,
				false,
				TEXT("scalar")),
			Value.ToSharedRef(),
			FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::Factory,
				TEXT("fixture-default"),
				TEXT("fixture-default-readback"),
				Value.ToSharedRef()),
			FUeAgentStateDetails(
				TEXT("Fixture State"),
				TEXT("Typed fixture state"),
				{TEXT("Fixture"), TEXT("QueryBinding")},
				false,
				TEXT("available"),
				{}),
			Revision);
		const TSharedPtr<FJsonObject> Time =
			Row->GetObjectField(TEXT("captured_at"));
		const TSharedPtr<FJsonObject> Provenance =
			Row->GetObjectField(TEXT("provenance"));
		TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
		Test->TestTrue(
			TEXT("create independent typed C4 snapshot"),
			FUeAgentCanonicalSnapshot::TryCreate(
				Row->GetStringField(TEXT("snapshot_id")),
				EUeAgentStatePlane::AuthoringMemory,
				Revision,
				FUeAgentTemporalPoint(
					Time->GetStringField(TEXT("clock_id")),
					Time->GetStringField(TEXT("tick"))),
				FUeAgentStateProvenance(
					Provenance->GetStringField(TEXT("source_kind")),
					Provenance->GetStringField(TEXT("source_id")),
					Provenance->GetStringField(TEXT("evidence_id"))),
				{Entry},
				Resources,
				Snapshot,
				StateError));
		return Snapshot;
	}

	class FFixtureCatalogSource final : public IUeAgentCatalogSource
	{
	public:
		FFixtureCatalogSource(
			TSharedRef<const FUeAgentCatalogSnapshot> InCatalog,
			FUeAgentCandidateBinding InBinding)
			: Catalog(MoveTemp(InCatalog))
			, Binding(MoveTemp(InBinding))
			, Kinds({TEXT("fixture_asset")})
		{
		}

		virtual const FString& GetSourceId() const override
		{
			return SourceId;
		}

		virtual const TArray<FString>& GetCatalogKinds() const override
		{
			return Kinds;
		}

		virtual bool TrySearch(
			const FUeAgentDiscoveryQuery& Query,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentCatalogSnapshot>& OutCatalog,
			FUeAgentDiscoveryError& OutError) override
		{
			++SearchCalls;
			LastResources = &Resources;
			LastDeadline = &Deadline;
			LastCancellation = &Cancellation;
			OutCatalog = Catalog;
			return true;
		}

		virtual bool TryResolveExact(
			const FString& CandidateId,
			const FUeAgentCatalogRevision& ExpectedRevision,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentCandidateBinding>& OutBinding,
			FUeAgentDiscoveryError& OutError) override
		{
			++ResolveCalls;
			LastResources = &Resources;
			LastDeadline = &Deadline;
			LastCancellation = &Cancellation;
			OutBinding =
				MakeShared<FUeAgentCandidateBinding>(Binding);
			return true;
		}

		int32 SearchCalls = 0;
		int32 ResolveCalls = 0;
		FUeAgentResourceTracker* LastResources = nullptr;
		const FUeAgentOperationDeadline* LastDeadline = nullptr;
		const FUeAgentCancellationToken* LastCancellation = nullptr;

	private:
		FString SourceId = TEXT("fixture-catalog-source");
		TSharedRef<const FUeAgentCatalogSnapshot> Catalog;
		FUeAgentCandidateBinding Binding;
		TArray<FString> Kinds;
	};

	class FFixtureReadPort final : public IUeAgentStateReadPort
	{
	public:
		explicit FFixtureReadPort(
			TSharedRef<const FUeAgentCanonicalSnapshot> InSnapshot)
			: Snapshot(MoveTemp(InSnapshot))
		{
		}

		virtual const FString& GetPortId() const override
		{
			return PortId;
		}

		virtual bool TryRead(
			const FUeAgentStateReadRequest& Request,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentDiscoveryError& OutError) override
		{
			++ReadCalls;
			LastResources = &Resources;
			LastDeadline = &Deadline;
			LastCancellation = &Cancellation;
			OutSnapshot = Snapshot;
			return true;
		}

		int32 ReadCalls = 0;
		FUeAgentResourceTracker* LastResources = nullptr;
		const FUeAgentOperationDeadline* LastDeadline = nullptr;
		const FUeAgentCancellationToken* LastCancellation = nullptr;

	private:
		FString PortId = TEXT("generic-read");
		TSharedRef<const FUeAgentCanonicalSnapshot> Snapshot;
	};

	class FFixtureTokenEstimator final : public IUeAgentTokenEstimator
	{
	public:
		virtual bool TryEstimate(
			const FUeAgentProjectedPayload& Payload,
			const TArray<FUeAgentArtifactDescriptor>& Artifacts,
			const TArray<FUeAgentProjectionDiagnostic>& Diagnostics,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FUeAgentTokenEstimate& OutEstimate,
			FUeAgentProjectionError& OutError) override
		{
			++Calls;
			LastResources = &Resources;
			LastDeadline = &Deadline;
			LastCancellation = &Cancellation;
			OutEstimate = FUeAgentTokenEstimate(
				EUeAgentTokenEstimateKind::Exact,
				32,
				64);
			return true;
		}

		int32 Calls = 0;
		FUeAgentResourceTracker* LastResources = nullptr;
		const FUeAgentOperationDeadline* LastDeadline = nullptr;
		const FUeAgentCancellationToken* LastCancellation = nullptr;
	};

	class FFixtureSessionSource final : public IUeAgentQuerySessionSource
	{
	public:
		explicit FFixtureSessionSource(
			FUeAgentQuerySessionIdentity InSession)
			: Session(MoveTemp(InSession))
		{
		}

		virtual FUeAgentQuerySessionIdentity GetCurrent() const override
		{
			++Calls;
			return Session;
		}

		void SetCurrent(FUeAgentQuerySessionIdentity InSession)
		{
			Session = MoveTemp(InSession);
		}

		mutable int32 Calls = 0;

	private:
		FUeAgentQuerySessionIdentity Session;
	};

	struct FFixtureBindingGraph
	{
		TSharedRef<FFixtureCatalogSource> CatalogSource;
		TSharedRef<FFixtureReadPort> ReadPort;
		TSharedRef<FFixtureTokenEstimator> Estimator;
		TSharedRef<FFixtureSessionSource> SessionSource;
		TSharedPtr<const FUeAgentDiscoveryService> Discovery;
		TSharedPtr<const FUeAgentResponseProjector> Projector;
		TSharedPtr<const FUeAgentQueryTicketService> Tickets;
		TSharedPtr<const FUeAgentQueryBinding> Binding;
	};

	static TOptional<FFixtureBindingGraph> BuildBindingGraph(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Fixture)
	{
		FUeAgentResourceTracker BuildResources = MakeTracker(Fixture);
		const TSharedPtr<const FUeAgentCatalogSnapshot> Catalog =
			FixtureCatalog(Test, Fixture, BuildResources);
		const TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot =
			FixtureSnapshot(Test, Fixture, BuildResources);
		if (!Catalog.IsValid() || !Snapshot.IsValid())
		{
			return {};
		}

		const TSharedRef<FFixtureCatalogSource> CatalogSource =
			MakeShared<FFixtureCatalogSource>(
				Catalog.ToSharedRef(),
				FixtureBinding(Fixture));
		const TSharedRef<FFixtureReadPort> ReadPort =
			MakeShared<FFixtureReadPort>(Snapshot.ToSharedRef());
		TSharedPtr<const FUeAgentDiscoveryService> Discovery;
		FUeAgentDiscoveryError DiscoveryError;
		Test->TestTrue(
			TEXT("create C2 service"),
			FUeAgentDiscoveryService::TryCreate(
				{CatalogSource},
				{ReadPort},
				Discovery,
				DiscoveryError));

		const TSharedRef<FFixtureTokenEstimator> Estimator =
			MakeShared<FFixtureTokenEstimator>();
		TSharedPtr<const FUeAgentResponseProjector> Projector;
		FUeAgentProjectionError ProjectionError;
		Test->TestTrue(
			TEXT("create C10 projector"),
			FUeAgentResponseProjector::TryCreate(
				Estimator,
				Projector,
				ProjectionError));

		const TSharedRef<FFixtureSessionSource> SessionSource =
			MakeShared<FFixtureSessionSource>(
				FixtureSession(Fixture));
		TSharedPtr<const FUeAgentQueryTicketService> Tickets;
		FUeAgentQueryBindingError QueryError;
		Test->TestTrue(
			TEXT("create C3Q ticket service"),
			FUeAgentQueryTicketService::TryCreate(
				TEXT("fixture-query-ticket-integrity-key"),
				30.0,
				[]() { return 100.0; },
				[]() { return FGuid::NewGuid().ToString(EGuidFormats::Digits); },
				Tickets,
				QueryError));
		TSharedPtr<const FUeAgentQueryBinding> QueryBinding;
		Test->TestTrue(
			TEXT("create C3Q typed binding"),
			FUeAgentQueryBinding::TryCreate(
				FixtureDescriptor(),
				FixtureSession(Fixture).GetRegistryHash(),
				Discovery.ToSharedRef(),
				Projector.ToSharedRef(),
				SessionSource,
				Tickets.ToSharedRef(),
				QueryBinding,
				QueryError));
		if (!Discovery.IsValid()
			|| !Projector.IsValid()
			|| !Tickets.IsValid()
			|| !QueryBinding.IsValid())
		{
			return {};
		}

		return FFixtureBindingGraph{
			CatalogSource,
			ReadPort,
			Estimator,
			SessionSource,
			Discovery,
			Projector,
			Tickets,
			QueryBinding,
		};
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentQueryBindingCppPublicContractTest,
	"UeAgentInterface.Core.BindingGateway.Query.PublicContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentQueryBindingCppPublicContractTest::RunTest(
	const FString& Parameters)
{
	using FSubmitSignature = bool (FUeAgentQueryBinding::*)(
		const FUeAgentQueryBindingRequest&,
		const FUeAgentCallerContext&,
		FUeAgentResourceTracker&,
		const FUeAgentOperationDeadline&,
		const FUeAgentCancellationToken&,
		TSharedPtr<const FUeAgentQueryBindingResult>&,
		FUeAgentQueryBindingError&) const;
	const FSubmitSignature Submit = &FUeAgentQueryBinding::TrySubmit;
	TestTrue(TEXT("one typed QueryBinding submit path"), Submit != nullptr);

	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const FUeAgentQuerySessionIdentity Session = FixtureSession(Fixture);
	TestTrue(TEXT("session identity is immutable and valid"), Session.IsValid());
	TestEqual(
		TEXT("session epoch remains integral"),
		Session.GetRegistryEpoch(),
		static_cast<int64>(17));

	const TArray<TSharedPtr<FJsonValue>> CaseIds =
		Fixture->GetArrayField(TEXT("case_ids"));
	TSet<FString> Cases;
	for (const TSharedPtr<FJsonValue>& Value : CaseIds)
	{
		Cases.Add(Value->AsString());
	}
	for (const FString& Required : {
			TEXT("catalog_search_projection"),
			TEXT("exact_candidate_projection"),
			TEXT("state_read_ticket_projection"),
			TEXT("query_ticket_binding"),
			TEXT("cancel_before_query"),
			TEXT("typed_error_propagation"),
			TEXT("typed_projection_error_propagation"),
			TEXT("query_payload_codec_conformance"),
		})
	{
		TestTrue(
			*FString::Printf(TEXT("fixture owns %s"), *Required),
			Cases.Contains(Required));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentQueryBindingCppPayloadCodecTest,
	"UeAgentInterface.Core.BindingGateway.Query.PayloadCodec",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentQueryBindingCppPayloadCodecTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> CodecFixture =
		Fixture->GetObjectField(TEXT("payload_codec"));
	TestEqual(
		TEXT("C3Q payload codec schema"),
		CodecFixture->GetStringField(TEXT("schema")),
		FString(TEXT("uai.c3q.query_payload.v1")));

	const FString WireText = JsonObjectText(
		CodecFixture->GetObjectField(TEXT("catalog_search_request")));
	FUeAgentResourceTracker Resources = MakeTracker(Fixture);
	TOptional<FUeAgentQueryBindingPayload> Payload;
	FUeAgentQueryBindingError Error;
	TestTrue(
		TEXT("decode protocol-neutral payload into typed C3Q query"),
		FUeAgentQueryPayloadCodec::TryDecode(
			FUeAgentContractValue(WireText),
			Resources,
			Payload,
			Error));
	TestTrue(TEXT("typed payload returned"), Payload.IsSet());
	TestEqual(
		TEXT("payload kind"),
		Payload->GetKind(),
		EUeAgentQueryBindingPayloadKind::CatalogSearch);
	TestEqual(
		TEXT("typed search catalog kind"),
		Payload->GetCatalogSearch()->GetQuery().GetCatalogKind(),
		FString(TEXT("fixture_asset")));
	TestEqual(
		TEXT("typed search session"),
		Payload->GetCatalogSearch()->GetSession().GetServerSessionId(),
		FString(TEXT("fixture-server-session-17")));

	FUeAgentContractValue Encoded(TEXT("{}"));
	TestTrue(
		TEXT("typed C3Q query returns to the same domain payload schema"),
		FUeAgentQueryPayloadCodec::TryEncode(
			Payload.GetValue(),
			Resources,
			Encoded,
			Error));
	TSharedPtr<FJsonObject> RoundTrip;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Encoded.GetJsonValue());
	TestTrue(
		TEXT("encoded C3Q payload is an object"),
		FJsonSerializer::Deserialize(Reader, RoundTrip)
			&& RoundTrip.IsValid());
	TestEqual(
		TEXT("round-trip discriminant"),
		RoundTrip->GetStringField(TEXT("query_kind")),
		FString(TEXT("catalog_search")));

	FUeAgentResourceTracker CatalogResources = MakeTracker(Fixture);
	const TSharedPtr<const FUeAgentCatalogSnapshot> Catalog =
		FixtureCatalog(this, Fixture, CatalogResources);
	const FUeAgentQueryBindingPayload ResolvePayload(
		FUeAgentCatalogResolveQuery(
			FixtureSession(Fixture),
			Catalog.ToSharedRef(),
			Catalog->GetCandidates()[0].GetIdentity().GetCanonicalKey(),
			FixtureProjection(Fixture)));
	FUeAgentContractValue EncodedResolve(TEXT("{}"));
	TestTrue(
		TEXT("encode typed exact resolve payload"),
		FUeAgentQueryPayloadCodec::TryEncode(
			ResolvePayload,
			Resources,
			EncodedResolve,
			Error));
	TOptional<FUeAgentQueryBindingPayload> DecodedResolve;
	TestTrue(
		TEXT("decode exact resolve payload through the same C3Q codec"),
		FUeAgentQueryPayloadCodec::TryDecode(
			EncodedResolve,
			Resources,
			DecodedResolve,
			Error));
	TestTrue(TEXT("typed exact resolve returned"), DecodedResolve.IsSet());
	TestEqual(
		TEXT("exact resolve round-trip kind"),
		DecodedResolve->GetKind(),
		EUeAgentQueryBindingPayloadKind::CatalogResolve);
	TestEqual(
		TEXT("exact resolve candidate survives round trip"),
		DecodedResolve->GetCatalogResolve()->GetCandidateId(),
		ResolvePayload.GetCatalogResolve()->GetCandidateId());

	const FUeAgentCandidateBinding Candidate = FixtureBinding(Fixture);
	TSharedPtr<const FUeAgentQueryTicketService> TicketService;
	TestTrue(
		TEXT("create codec ticket service"),
		FUeAgentQueryTicketService::TryCreate(
			TEXT("fixture-query-ticket-integrity-key"),
			30.0,
			[]() { return 100.0; },
			[]() { return FString(TEXT("fixture-codec-ticket")); },
			TicketService,
			Error));
	TOptional<FUeAgentQueryTicket> Ticket;
	TestTrue(
		TEXT("issue codec state-read ticket"),
		TicketService->TryIssue(
			Candidate,
			FixtureCandidateDescriptor(),
			FixtureSession(Fixture),
			FixtureCaller(),
			{},
			Ticket,
			Error));
	const FUeAgentQueryBindingPayload StatePayload(
		FUeAgentStateReadQuery(
			FixtureSession(Fixture),
			Ticket.GetValue(),
			Candidate,
			EUeAgentStatePlane::AuthoringMemory,
			FixtureProjection(Fixture)));
	FUeAgentContractValue EncodedState(TEXT("{}"));
	TestTrue(
		TEXT("encode typed state-read payload"),
		FUeAgentQueryPayloadCodec::TryEncode(
			StatePayload,
			Resources,
			EncodedState,
			Error));
	TOptional<FUeAgentQueryBindingPayload> DecodedState;
	TestTrue(
		TEXT("decode state-read payload through the same C3Q codec"),
		FUeAgentQueryPayloadCodec::TryDecode(
			EncodedState,
			Resources,
			DecodedState,
			Error));
	TestTrue(TEXT("typed state-read returned"), DecodedState.IsSet());
	TestEqual(
		TEXT("state-read round-trip kind"),
		DecodedState->GetKind(),
		EUeAgentQueryBindingPayloadKind::StateRead);
	TestEqual(
		TEXT("state-read binding survives round trip"),
		DecodedState->GetStateRead()->GetBinding().GetBindingId(),
		Candidate.GetBindingId());
	TestEqual(
		TEXT("state-read ticket survives round trip"),
		DecodedState->GetStateRead()->GetTicket().GetTicketId(),
		Ticket->GetTicketId());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentQueryBindingCppTicketContractTest,
	"UeAgentInterface.Core.BindingGateway.Query.TicketContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentQueryBindingCppTicketContractTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const FUeAgentCandidateBinding Binding = FixtureBinding(Fixture);
	const FUeAgentQuerySessionIdentity Session = FixtureSession(Fixture);
	const FUeAgentCallerContext Caller = FixtureCaller();

	TSharedPtr<const FUeAgentQueryTicketService> Service;
	FUeAgentQueryBindingError Error;
	TestTrue(
		TEXT("create ticket service with host-owned clock and key"),
		FUeAgentQueryTicketService::TryCreate(
			TEXT("fixture-query-ticket-integrity-key"),
			30.0,
			[]() { return 100.0; },
			[]() { return FString(TEXT("fixture-ticket-1")); },
			Service,
			Error));
	TOptional<FUeAgentQueryTicket> Ticket;
	TestTrue(
		TEXT("issue immutable candidate ticket"),
		Service->TryIssue(
			Binding,
			FixtureCandidateDescriptor(),
			Session,
			Caller,
			{},
			Ticket,
			Error));
	TestTrue(TEXT("ticket returned"), Ticket.IsSet());
	TestEqual(
		TEXT("ticket candidate binding"),
		Ticket->GetCandidateBindingId(),
		Binding.GetBindingId());
	TestEqual(
		TEXT("ticket catalog revision"),
		Ticket->GetCatalogRevision(),
		Binding.GetCatalogRevision());
	TestEqual(TEXT("ticket issue time"), Ticket->GetIssuedAt(), 100.0);
	TestEqual(TEXT("ticket expiry"), Ticket->GetExpiresAt(), 130.0);
	TestTrue(
		TEXT("ticket fingerprint is signed"),
		Ticket->GetCandidateFingerprint().StartsWith(TEXT("sha256:"))
			&& Ticket->GetSignature().StartsWith(TEXT("sha256:")));
	TestTrue(
		TEXT("unchanged session/caller/binding validates"),
		Service->TryValidate(
			Ticket.GetValue(),
			Binding,
			FixtureCandidateDescriptor(),
			Session,
			Caller,
			Error));

	using namespace UE::UAI::BindingGateway::Ticket;
	TSharedPtr<const FUeAgentOperationTicketService> SharedService;
	FUeAgentOperationTicketError SharedError;
	TestTrue(
		TEXT("create one shared C3 ticket owner"),
		FUeAgentOperationTicketService::TryCreate(
			TEXT("fixture-shared-ticket-integrity-key"),
			30.0,
			[]() { return 100.0; },
			[]() { return FString(TEXT("fixture-shared-ticket")); },
			SharedService,
			SharedError));
	TSharedPtr<const FUeAgentQueryTicketService> QueryView;
	TestTrue(
		TEXT("C3Q wraps the shared ticket owner without cloning it"),
		FUeAgentQueryTicketService::TryCreateFromShared(
			SharedService.ToSharedRef(),
			QueryView,
			Error));
	TOptional<FUeAgentQueryTicket> SharedTicket;
	TestTrue(
		TEXT("C3Q issues the candidate OperationDescriptor ticket"),
		QueryView->TryIssue(
			Binding,
			FixtureCandidateDescriptor(),
			Session,
			Caller,
			{},
			SharedTicket,
			Error));
	TestTrue(
		TEXT("the same C3 owner validates the C3Q ticket for C3O"),
		SharedService->TryValidate(
			SharedTicket.GetValue(),
			Binding,
			FixtureCandidateDescriptor(),
			Session,
			Caller,
			SharedError));

	const FUeAgentQuerySessionIdentity StaleSession(
		Session.GetServerSessionId(),
		Session.GetRegistryEpoch() + 1,
		Session.GetRegistryHash());
	TestFalse(
		TEXT("registry epoch mismatch is rejected before read"),
		Service->TryValidate(
			Ticket.GetValue(),
			Binding,
			FixtureCandidateDescriptor(),
			StaleSession,
			Caller,
			Error));
	TestEqual(
		TEXT("stable stale code"),
		Error.Code,
		FString(TEXT("registry_epoch_stale")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentQueryBindingCppTypedBoundaryTest,
	"UeAgentInterface.Core.BindingGateway.Query.TypedBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentQueryBindingCppTypedBoundaryTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const FUeAgentQuerySessionIdentity Session = FixtureSession(Fixture);
	const TSharedPtr<FJsonObject> Search =
		Fixture->GetObjectField(TEXT("catalog"))
			->GetObjectField(TEXT("query"));
	const TSharedPtr<FJsonObject> Projection =
		Fixture->GetObjectField(TEXT("projection"));

	const FUeAgentCatalogSearchQuery TypedSearch(
		Session,
		FUeAgentDiscoveryQuery(
			Search->GetStringField(TEXT("catalog_kind")),
			Search->GetStringField(TEXT("search_text")),
			EUeAgentDiscoveryMatchMode::Broad,
			EUeAgentMaterializationPolicy::IndexOnly),
		FUeAgentProjectionRequest(
			FUeAgentProjectionProfile(
				Projection->GetStringField(TEXT("profile_id")),
				Projection->GetBoolField(TEXT("include_defaults")),
				Projection->GetBoolField(TEXT("include_details")),
				Projection->GetBoolField(TEXT("include_provenance"))),
			FUeAgentPageRequest(
				static_cast<int32>(
					Projection->GetNumberField(TEXT("max_items"))),
				TEXT("")),
			static_cast<int64>(
				Projection->GetNumberField(TEXT("token_budget"))),
			{},
			{}));
	const FUeAgentQueryBindingPayload Payload(TypedSearch);
	const FUeAgentQueryBindingRequest Request(
		FixtureDescriptor(),
		TEXT("query-binding-typed-1"),
		Payload);
	TestTrue(TEXT("typed C0 request is valid"), Request.IsValid());
	TestEqual(
		TEXT("C2 query remains typed"),
		Request.GetPayload()
			.GetCatalogSearch()
			->GetQuery()
			.GetMatchMode(),
		EUeAgentDiscoveryMatchMode::Broad);
	TestEqual(
		TEXT("broad query is not a resolve or state-read payload"),
		Request.GetPayload().GetKind(),
		EUeAgentQueryBindingPayloadKind::CatalogSearch);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentQueryBindingCppCatalogFlowsTest,
	"UeAgentInterface.Core.BindingGateway.Query.CatalogFlows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentQueryBindingCppCatalogFlowsTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	TOptional<FFixtureBindingGraph> Graph =
		BuildBindingGraph(this, Fixture);
	if (!Graph.IsSet())
	{
		return false;
	}

	FUeAgentResourceTracker Resources = MakeTracker(Fixture);
	const FUeAgentMonotonicClock Clock = []() { return 100.0; };
	const FUeAgentOperationDeadline Deadline =
		FUeAgentOperationDeadline::FromTimeout(30.0, Clock);
	const FUeAgentCancellationSource CancellationSource(Clock);
	const FUeAgentCancellationToken Cancellation =
		CancellationSource.GetToken();
	const TSharedPtr<FJsonObject> Query =
		Fixture->GetObjectField(TEXT("catalog"))
			->GetObjectField(TEXT("query"));

	const FUeAgentQueryBindingRequest SearchRequest(
		FixtureDescriptor(),
		TEXT("query-binding-search-1"),
		FUeAgentQueryBindingPayload(
			FUeAgentCatalogSearchQuery(
				FixtureSession(Fixture),
				FUeAgentDiscoveryQuery(
					Query->GetStringField(TEXT("catalog_kind")),
					Query->GetStringField(TEXT("search_text")),
					EUeAgentDiscoveryMatchMode::Broad,
					EUeAgentMaterializationPolicy::IndexOnly),
				FixtureProjection(Fixture))));
	TSharedPtr<const FUeAgentQueryBindingResult> SearchResult;
	FUeAgentQueryBindingError Error;
	TestTrue(
		TEXT("broad query enters one C2 call and one C10 projection"),
		Graph->Binding->TrySubmit(
			SearchRequest,
			FixtureCaller(),
			Resources,
			Deadline,
			Cancellation,
			SearchResult,
			Error));
	TestTrue(TEXT("typed broad result returned"), SearchResult.IsValid());
	TestTrue(
		TEXT("broad projection contains typed C2 catalog"),
		SearchResult->GetValue()
			.GetProjection()
			->GetPayload()
			.GetCatalog()
			.IsValid());
	TestFalse(
		TEXT("broad search does not issue ticket"),
		SearchResult->GetValue().GetTicket().IsSet());
	FUeAgentContractValue EncodedResponse(TEXT("{}"));
	TestTrue(
		TEXT("typed QueryBinding response encodes at the C3Q boundary"),
		FUeAgentQueryPayloadCodec::TryEncode(
			SearchResult->GetValue(),
			Resources,
			EncodedResponse,
			Error));
	TSharedPtr<FJsonObject> ResponseObject;
	const TSharedRef<TJsonReader<>> ResponseReader =
		TJsonReaderFactory<>::Create(
			EncodedResponse.GetJsonValue());
	TestTrue(
		TEXT("encoded response is an object"),
		FJsonSerializer::Deserialize(
			ResponseReader,
			ResponseObject)
			&& ResponseObject.IsValid());
	TestTrue(
		TEXT("encoded response contains projection"),
		ResponseObject->HasTypedField<EJson::Object>(
			TEXT("projection")));
	TestTrue(
		TEXT("encoded broad response contains explicit null ticket"),
		ResponseObject->HasTypedField<EJson::Null>(
			TEXT("ticket")));
	TestEqual(
		TEXT("one search only"),
		Graph->CatalogSource->SearchCalls,
		1);
	TestEqual(
		TEXT("no speculative exact resolve"),
		Graph->CatalogSource->ResolveCalls,
		0);
	TestEqual(TEXT("one projection only"), Graph->Estimator->Calls, 1);
	TestTrue(
		TEXT("same C1 tracker reaches C2 and C10"),
		Graph->CatalogSource->LastResources == &Resources
			&& Graph->Estimator->LastResources == &Resources);
	TestTrue(
		TEXT("same C1 deadline reaches C2 and C10"),
		Graph->CatalogSource->LastDeadline == &Deadline
			&& Graph->Estimator->LastDeadline == &Deadline);
	TestTrue(
		TEXT("same C1 cancellation reaches C2 and C10"),
		Graph->CatalogSource->LastCancellation == &Cancellation
			&& Graph->Estimator->LastCancellation == &Cancellation);

	FUeAgentResourceTracker CatalogResources = MakeTracker(Fixture);
	const TSharedPtr<const FUeAgentCatalogSnapshot> Catalog =
		FixtureCatalog(this, Fixture, CatalogResources);
	const FString CandidateId =
		Catalog->GetCandidates()[0].GetIdentity().GetCanonicalKey();
	const FUeAgentQueryBindingRequest ResolveRequest(
		FixtureDescriptor(),
		TEXT("query-binding-resolve-1"),
		FUeAgentQueryBindingPayload(
			FUeAgentCatalogResolveQuery(
				FixtureSession(Fixture),
				Catalog.ToSharedRef(),
				CandidateId,
				FixtureProjection(Fixture))));
	FUeAgentResourceTracker ResolveResources = MakeTracker(Fixture);
	TSharedPtr<const FUeAgentQueryBindingResult> ResolveResult;
	TestTrue(
		TEXT("exact resolve uses observed catalog and projects binding"),
		Graph->Binding->TrySubmit(
			ResolveRequest,
			FixtureCaller(),
			ResolveResources,
			Deadline,
			Cancellation,
			ResolveResult,
			Error));
	TestTrue(
		TEXT("exact projection contains typed C2 binding"),
		ResolveResult->GetValue()
			.GetProjection()
			->GetPayload()
			.GetCandidateBinding()
			.IsValid());
	TestTrue(
		TEXT("exact resolve issues candidate ticket"),
		ResolveResult->GetValue().GetTicket().IsSet());
	TestEqual(
		TEXT("one exact resolve"),
		Graph->CatalogSource->ResolveCalls,
		1);
	TestEqual(
		TEXT("state read remains untouched"),
		Graph->ReadPort->ReadCalls,
		0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentQueryBindingCppStateAndGuardsTest,
	"UeAgentInterface.Core.BindingGateway.Query.StateAndGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentQueryBindingCppStateAndGuardsTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	TOptional<FFixtureBindingGraph> Graph =
		BuildBindingGraph(this, Fixture);
	if (!Graph.IsSet())
	{
		return false;
	}

	const FUeAgentCandidateBinding Candidate =
		FixtureBinding(Fixture);
	TOptional<FUeAgentQueryTicket> Ticket;
	FUeAgentQueryBindingError Error;
	TestTrue(
		TEXT("issue state-read ticket"),
		Graph->Tickets->TryIssue(
			Candidate,
			FixtureCandidateDescriptor(),
			FixtureSession(Fixture),
			FixtureCaller(),
			{},
			Ticket,
			Error));
	const FUeAgentQueryBindingRequest Request(
		FixtureDescriptor(),
		TEXT("query-binding-state-1"),
		FUeAgentQueryBindingPayload(
			FUeAgentStateReadQuery(
				FixtureSession(Fixture),
				Ticket.GetValue(),
				Candidate,
				EUeAgentStatePlane::AuthoringMemory,
				FUeAgentProjectionRequest(
					FUeAgentProjectionProfile(
						TEXT("full"),
						true,
						true,
						true),
					FUeAgentPageRequest(25, TEXT("")),
					TOptional<int64>(512),
					{},
					{}))));
	const FUeAgentMonotonicClock Clock = []() { return 100.0; };
	const FUeAgentOperationDeadline Deadline =
		FUeAgentOperationDeadline::FromTimeout(30.0, Clock);
	const FUeAgentCancellationSource CancellationSource(Clock);
	const FUeAgentCancellationToken Cancellation =
		CancellationSource.GetToken();
	FUeAgentResourceTracker Resources = MakeTracker(Fixture);
	TSharedPtr<const FUeAgentQueryBindingResult> Result;
	TestTrue(
		TEXT("ticketed state read projects independent C4 snapshot"),
		Graph->Binding->TrySubmit(
			Request,
			FixtureCaller(),
			Resources,
			Deadline,
			Cancellation,
			Result,
			Error));
	TestEqual(TEXT("one state read"), Graph->ReadPort->ReadCalls, 1);
	TestTrue(
		TEXT("state projection contains typed C4 snapshot"),
		Result->GetValue()
			.GetProjection()
			->GetPayload()
			.GetSnapshot()
			.IsValid());
	TestTrue(
		TEXT("state read refreshes ticket"),
		Result->GetValue().GetTicket().IsSet());
	TestTrue(
		TEXT("refreshed ticket binds observed StateRevision"),
		Result->GetValue()
			.GetTicket()
			->GetTargetRevision()
			.IsSet());
	TestEqual(
		TEXT("ticket revision equals projected snapshot revision"),
		Result->GetValue()
			.GetTicket()
			->GetTargetRevision()
			.GetValue(),
		Result->GetValue()
			.GetProjection()
			->GetPayload()
			.GetSnapshot()
			->GetRevision());

	FUeAgentContractValue EncodedResponse(TEXT("{}"));
	TestTrue(
		TEXT("state response encodes complete projected readback"),
		FUeAgentQueryPayloadCodec::TryEncode(
			Result->GetValue(),
			Resources,
			EncodedResponse,
			Error));
	TSharedPtr<FJsonObject> ResponseObject;
	const TSharedRef<TJsonReader<>> ResponseReader =
		TJsonReaderFactory<>::Create(EncodedResponse.GetJsonValue());
	TestTrue(
		TEXT("encoded state response is an object"),
		FJsonSerializer::Deserialize(
			ResponseReader,
			ResponseObject)
			&& ResponseObject.IsValid());
	const TSharedPtr<FJsonObject> SnapshotObject =
		ResponseObject
			->GetObjectField(TEXT("projection"))
			->GetObjectField(TEXT("payload"))
			->GetObjectField(TEXT("snapshot"));
	TestEqual(
		TEXT("state plane survives the external boundary"),
		SnapshotObject->GetStringField(TEXT("state_plane")),
		FString(TEXT("authoring_memory")));
	TestEqual(
		TEXT("independent readback provenance survives the external boundary"),
		SnapshotObject
			->GetObjectField(TEXT("provenance"))
			->GetStringField(TEXT("source_kind")),
		FString(TEXT("independent_readback")));
	const TArray<TSharedPtr<FJsonValue>>& EncodedEntries =
		SnapshotObject->GetArrayField(TEXT("entries"));
	TestEqual(
		TEXT("projected state entries survive the external boundary"),
		EncodedEntries.Num(),
		1);
	if (EncodedEntries.Num() == 1)
	{
		const TSharedPtr<FJsonObject> ValueObject =
			EncodedEntries[0]
				->AsObject()
				->GetObjectField(TEXT("value"));
		TestEqual(
			TEXT("canonical value kind survives the external boundary"),
			ValueObject->GetStringField(TEXT("kind")),
			FString(TEXT("string")));
		TestEqual(
			TEXT("canonical state value survives the external boundary"),
			ValueObject->GetStringField(TEXT("text")),
			Fixture
				->GetObjectField(TEXT("snapshot"))
				->GetStringField(TEXT("value")));
	}

	const int32 ReadsBeforeCancel = Graph->ReadPort->ReadCalls;
	FUeAgentCancellationSource CancelledSource(Clock);
	CancelledSource.Request(TEXT("fixture-cancel-before-query"));
	const FUeAgentCancellationToken Cancelled =
		CancelledSource.GetToken();
	FUeAgentResourceTracker CancelledResources = MakeTracker(Fixture);
	TSharedPtr<const FUeAgentQueryBindingResult> CancelledResult;
	TestFalse(
		TEXT("cancelled request stops before C2 and C10"),
		Graph->Binding->TrySubmit(
			Request,
			FixtureCaller(),
			CancelledResources,
			Deadline,
			Cancelled,
			CancelledResult,
			Error));
	TestEqual(
		TEXT("cancelled request has stable code"),
		Error.Code,
		FString(TEXT("query_cancelled")));
	TestEqual(
		TEXT("cancelled request does not read"),
		Graph->ReadPort->ReadCalls,
		ReadsBeforeCancel);

	const FUeAgentQuerySessionIdentity Current =
		FixtureSession(Fixture);
	Graph->SessionSource->SetCurrent(
		FUeAgentQuerySessionIdentity(
			Current.GetServerSessionId(),
			Current.GetRegistryEpoch() + 1,
			Current.GetRegistryHash()));
	FUeAgentResourceTracker StaleResources = MakeTracker(Fixture);
	TSharedPtr<const FUeAgentQueryBindingResult> StaleResult;
	TestFalse(
		TEXT("stale registry epoch stops before C2"),
		Graph->Binding->TrySubmit(
			Request,
			FixtureCaller(),
			StaleResources,
			Deadline,
			Cancellation,
			StaleResult,
			Error));
	TestEqual(
		TEXT("stale session code"),
		Error.Code,
		FString(TEXT("registry_epoch_stale")));
	TestEqual(
		TEXT("stale session does not read"),
		Graph->ReadPort->ReadCalls,
		ReadsBeforeCancel);
	return true;
}

