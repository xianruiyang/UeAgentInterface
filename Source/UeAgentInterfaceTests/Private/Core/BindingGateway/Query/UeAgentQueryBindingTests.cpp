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
			Fixture->GetObjectField(TEXT("session"));
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
		return FUeAgentResourceTracker();
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

	static FUeAgentCanonicalValueRef CanonicalString(
		const FString& Text,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(
			Text,
			Resources,
			Value,
			Error));
		return Value.ToSharedRef();
	}

	static FUeAgentCanonicalValueRef CanonicalArray(
		TArray<FUeAgentCanonicalValueRef> Items,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryArray(
			Items,
			Resources,
			Value,
			Error));
		return Value.ToSharedRef();
	}

	static FUeAgentCanonicalValueRef CanonicalObject(
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryObject(
			Items,
			Resources,
			Value,
			Error));
		return Value.ToSharedRef();
	}

	static TSharedPtr<const FUeAgentCanonicalSnapshot>
		NestedProjectionFixtureSnapshot(
			FAutomationTestBase* Test,
			FUeAgentResourceTracker& Resources,
			const FString& NativeObservationJson = FString())
	{
		const FUeAgentCanonicalValueRef Keys = CanonicalArray({
			CanonicalString(TEXT("10"), Resources),
			CanonicalString(TEXT("20"), Resources)}, Resources);
		const FUeAgentCanonicalValueRef Section = CanonicalObject({
			{TEXT("section_ref"), CanonicalString(TEXT("section:0"), Resources)},
			{TEXT("section_type"), CanonicalString(TEXT("FloatSection"), Resources)},
			{TEXT("key_count"), CanonicalString(TEXT("2"), Resources)},
			{TEXT("keys"), Keys},
			{TEXT("native_evidence"), CanonicalString(TEXT("machine-only"), Resources)}},
			Resources);
		const FUeAgentCanonicalValueRef Track = CanonicalObject({
			{TEXT("track_ref"), CanonicalString(TEXT("track:0"), Resources)},
			{TEXT("track_type"), CanonicalString(TEXT("FloatTrack"), Resources)},
			{TEXT("section_count"), CanonicalString(TEXT("1"), Resources)},
			{TEXT("sections"), CanonicalArray({Section}, Resources)},
			{TEXT("object_path"), CanonicalString(TEXT("machine-only"), Resources)}},
			Resources);
		const FUeAgentCanonicalValueRef FirstBinding = CanonicalObject({
			{TEXT("binding_ref"), CanonicalString(TEXT("binding:0"), Resources)},
			{TEXT("name"), CanonicalString(TEXT("First"), Resources)},
			{TEXT("track_count"), CanonicalString(TEXT("1"), Resources)},
			{TEXT("tracks"), CanonicalArray({Track}, Resources)},
			{TEXT("revision"), CanonicalString(TEXT("machine-only"), Resources)}},
			Resources);
		const FUeAgentCanonicalValueRef SecondBinding = CanonicalObject({
			{TEXT("binding_ref"), CanonicalString(TEXT("binding:1"), Resources)},
			{TEXT("name"), CanonicalString(TEXT("Second"), Resources)},
			{TEXT("track_count"), CanonicalString(TEXT("0"), Resources)},
			{TEXT("tracks"), CanonicalArray({}, Resources)}},
			Resources);
		const FUeAgentCanonicalValueRef Structure = CanonicalObject({
			{TEXT("display_rate"), CanonicalObject({
				{TEXT("numerator"), CanonicalString(TEXT("30"), Resources)},
				{TEXT("denominator"), CanonicalString(TEXT("1"), Resources)},
				{TEXT("private_rate"), CanonicalString(TEXT("machine-only"), Resources)}},
				Resources)},
			{TEXT("bindings"), CanonicalArray(
				{FirstBinding, SecondBinding}, Resources)},
			{TEXT("root_tracks"), CanonicalArray({}, Resources)},
			{TEXT("internal_state"), CanonicalString(TEXT("machine-only"), Resources)}},
			Resources);
		const FUeAgentStateRevision Revision(TEXT("fixture"), TEXT("1"), 1);
		const bool bNativeObservation = !NativeObservationJson.IsEmpty();
		const FUeAgentCanonicalValueRef EntryValue = bNativeObservation
			? CanonicalString(NativeObservationJson, Resources) : Structure;
		const FUeAgentStateEntry Entry(
			FUeAgentCanonicalIdentity(
				TEXT("fixture.nested.structure"),
				TEXT("fixture"),
				TEXT("fixture-owner"),
				bNativeObservation ? TEXT("observation_json") : TEXT("structure"),
				TEXT("fixture.structure"),
				TEXT("1")),
			FUeAgentCanonicalType(
				TEXT("fixture.nested.structure"),
				bNativeObservation ? EUeAgentValueKind::String : EUeAgentValueKind::Object,
				false,
				bNativeObservation ? TEXT("scalar") : TEXT("object")),
			EntryValue,
			FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::Factory,
				TEXT("fixture"),
				TEXT("fixture"),
				EntryValue),
			FUeAgentStateDetails(
				TEXT("Nested fixture"),
				TEXT("Nested public projection fixture"),
				{TEXT("Fixture")},
				false,
				TEXT("available"),
				{}),
			Revision);
		TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
		FUeAgentStateSemanticError Error;
		Test->TestTrue(
			TEXT("create nested public projection snapshot"),
			FUeAgentCanonicalSnapshot::TryCreate(
				TEXT("fixture-nested-projection"),
				EUeAgentStatePlane::AuthoringMemory,
				Revision,
				FUeAgentTemporalPoint(TEXT("fixture-clock"), TEXT("1")),
				FUeAgentStateProvenance(
					TEXT("independent_readback"),
					TEXT("fixture"),
					TEXT("fixture")),
				{Entry},
				Resources,
				Snapshot,
				Error));
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
		})
	{
		TestTrue(
			*FString::Printf(TEXT("fixture owns %s"), *Required),
			Cases.Contains(Required));
	}
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
	TestFalse(
		TEXT("encoded broad response omits an unavailable ticket"),
		ResponseObject->HasField(TEXT("ticket")));
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
	TestTrue(
		*FString::Printf(
			TEXT("full projected fixture stays within bounded character budget (chars=%d)"),
			EncodedResponse.GetJsonValue().Len()),
		EncodedResponse.GetJsonValue().Len() <= 8192);
	TSharedPtr<FJsonObject> ResponseObject;
	const TSharedRef<TJsonReader<>> ResponseReader =
		TJsonReaderFactory<>::Create(EncodedResponse.GetJsonValue());
	TestTrue(
		TEXT("encoded state response is an object"),
		FJsonSerializer::Deserialize(
			ResponseReader,
			ResponseObject)
			&& ResponseObject.IsValid());
	FUeAgentContractValue PublicResponse(TEXT("{}"));
	const FQueryPublicProjectionDescriptor* ContentGraphProjection =
		FindQueryPublicProjectionDescriptor(TEXT("content_graph_entries"));
	TestNotNull(
		TEXT("content graph public projection descriptor is registered"),
		ContentGraphProjection);
	TestNull(
		TEXT("unknown public projection descriptor fails closed"),
		FindQueryPublicProjectionDescriptor(
			TEXT("unknown_public_projection")));
	for (const FString& CommandId : {
		TEXT("graph.view.read"),
		TEXT("viewport.ray.deproject"),
		TEXT("viewport.ray.trace"),
		TEXT("viewport.actor.pick"),
		TEXT("viewport.pixel_scale.read"),
		TEXT("viewport.state.read"),
		TEXT("viewport.warning.list"),
		TEXT("viewport.view_mode.list"),
		TEXT("viewport.instance.list"),
		TEXT("timeline.sequence.read")})
	{
		TestNotNull(
			TEXT("TimelineSpatial query has an explicit public projection: ")
				+ CommandId,
			FindQueryPublicProjectionDescriptorForCommand(CommandId));
	}
	const FQueryPublicProjectionDescriptor* ViewModeProjection =
		FindQueryPublicProjectionDescriptorForCommand(
			TEXT("viewport.view_mode.list"));
	const FQueryPublicProjectionDescriptor* InstanceProjection =
		FindQueryPublicProjectionDescriptorForCommand(
			TEXT("viewport.instance.list"));
	if (ViewModeProjection && InstanceProjection && ContentGraphProjection)
	{
		TestEqual(
			TEXT("single-record public projection pages complete records"),
			QueryPublicProjectionPageUnit(*ViewModeProjection),
			EUeAgentProjectionPageUnit::SingleRecord);
		TestEqual(
			TEXT("snapshot record arrays page by owner record"),
			QueryPublicProjectionPageUnit(*InstanceProjection),
			EUeAgentProjectionPageUnit::OwnerRecord);
		TestEqual(
			TEXT("legacy snapshot entries retain entry paging"),
			QueryPublicProjectionPageUnit(*ContentGraphProjection),
			EUeAgentProjectionPageUnit::Entry);
	}
	if (!ContentGraphProjection)
	{
		return false;
	}
	TestTrue(
		TEXT("public content graph response uses the direct business projection"),
		FUeAgentQueryPayloadCodec::TryEncodePublic(
			*ContentGraphProjection,
			TEXT("content_graph.structure"),
			TEXT("content_graph.structure"),
			true,
			Result->GetValue(),
			Resources,
			PublicResponse,
			Error));
	TSharedPtr<FJsonObject> PublicObject;
	const TSharedRef<TJsonReader<>> PublicReader =
		TJsonReaderFactory<>::Create(PublicResponse.GetJsonValue());
	TestTrue(
		TEXT("public content graph response is an object"),
		FJsonSerializer::Deserialize(PublicReader, PublicObject)
			&& PublicObject.IsValid());
	if (PublicObject.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* PublicEntries = nullptr;
		TestTrue(
			TEXT("public content graph response directly exposes entries"),
			PublicObject->TryGetArrayField(TEXT("entries"), PublicEntries)
				&& PublicEntries
				&& PublicEntries->Num() == 1);
		if (PublicEntries && PublicEntries->Num() == 1)
		{
			const TSharedPtr<FJsonObject> PublicEntry =
				(*PublicEntries)[0]->AsObject();
			TestTrue(
				TEXT("scalar content graph entry uses its short business key"),
				PublicEntry.IsValid()
					&& PublicEntry->Values.Num() == 1
					&& PublicEntry->HasField(TEXT("DA_Query")));
			if (PublicEntry.IsValid())
			{
				TestEqual(
					TEXT("scalar content graph entry keeps the scalar value"),
					PublicEntry->GetStringField(TEXT("DA_Query")),
					FString(TEXT("fixture-state-value")));
				for (const FString& MachineEntryField : {
					TEXT("id"), TEXT("owner_id"), TEXT("container_id"),
					TEXT("guid"), TEXT("hash"), TEXT("revision"),
					TEXT("cursor"), TEXT("ticket")})
				{
					TestFalse(
						*FString::Printf(
							TEXT("public entry omits machine field %s"),
							*MachineEntryField),
						PublicEntry->HasField(MachineEntryField));
				}
			}
		}
		for (const FString& MachineEnvelopeField : {
			TEXT("command_id"),
			TEXT("mechanics_command_id"),
			TEXT("read_only"),
			TEXT("query_result"),
			TEXT("projection"),
			TEXT("snapshot_id"),
			TEXT("revision"),
		})
		{
			TestFalse(
				*FString::Printf(
					TEXT("public content graph response omits %s"),
					*MachineEnvelopeField),
				PublicObject->HasField(MachineEnvelopeField));
		}
		TestFalse(
			TEXT("complete public content graph response omits paging noise"),
			PublicObject->HasField(TEXT("more")));
	}

	const FQueryPublicProjectionDescriptor DynamicProjection = {
		TEXT("fixture_dynamic_fields"),
		EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
		TEXT("objects"),
		{},
		{
			{TEXT("objects"), TEXT("array"), true, TEXT("Fixture records with explicitly admitted dynamic fields.")},
			{TEXT("more"), TEXT("object"), false, TEXT("Partial result counts.")},
		},
		TEXT("Only producer-scoped dynamic fields are public."),
		false,
		{
			TEXT("fixture.asset:"),
			TEXT("data_asset"),
			TEXT("fields"),
		},
	};
	TestTrue(
		TEXT("a producer-scoped dynamic public descriptor is valid"),
		DynamicProjection.IsValid());
	FUeAgentContractValue DynamicResponse(TEXT("{}"));
	TestTrue(
		TEXT("a producer-scoped dynamic field uses the shared public codec"),
		FUeAgentQueryPayloadCodec::TryEncodePublic(
			DynamicProjection,
			TEXT("fixture.dynamic"),
			TEXT("fixture.dynamic"),
			true,
			Result->GetValue(),
			Resources,
			DynamicResponse,
			Error));
	TSharedPtr<FJsonObject> DynamicObject;
	const TSharedRef<TJsonReader<>> DynamicReader =
		TJsonReaderFactory<>::Create(DynamicResponse.GetJsonValue());
	TestTrue(
		TEXT("dynamic public response is an object"),
		FJsonSerializer::Deserialize(DynamicReader, DynamicObject)
			&& DynamicObject.IsValid());
	if (DynamicObject.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* DynamicRecords = nullptr;
		TestTrue(
			TEXT("dynamic public response contains one grouped record"),
			DynamicObject->Values.Num() == 1
				&& DynamicObject->TryGetArrayField(TEXT("objects"), DynamicRecords)
				&& DynamicRecords
				&& DynamicRecords->Num() == 1);
		if (DynamicRecords && DynamicRecords->Num() == 1)
		{
			const TSharedPtr<FJsonObject> Record =
				(*DynamicRecords)[0]->AsObject();
			const TSharedPtr<FJsonObject>* Fields = nullptr;
			TestTrue(
				TEXT("dynamic values are nested under fields without owner identity"),
				Record.IsValid()
					&& Record->Values.Num() == 1
					&& Record->TryGetObjectField(TEXT("fields"), Fields)
					&& Fields
					&& Fields->IsValid()
					&& (*Fields)->Values.Num() == 1
					&& (*Fields)->GetStringField(TEXT("DA_Query"))
						== TEXT("fixture-state-value")
					&& !Record->HasField(TEXT("owner_id")));
		}
	}

	FQueryPublicProjectionDescriptor MismatchedDynamicProjection =
		DynamicProjection;
	MismatchedDynamicProjection.DynamicFields.SemanticIdPrefix =
		TEXT("unrelated.asset:");
	FUeAgentContractValue MismatchedDynamicResponse(TEXT("{}"));
	TestTrue(
		TEXT("a mismatched producer prefix remains a valid empty public result"),
		MismatchedDynamicProjection.IsValid()
			&& FUeAgentQueryPayloadCodec::TryEncodePublic(
				MismatchedDynamicProjection,
				TEXT("fixture.dynamic"),
				TEXT("fixture.dynamic"),
				true,
				Result->GetValue(),
				Resources,
				MismatchedDynamicResponse,
				Error));
	TSharedPtr<FJsonObject> MismatchedDynamicObject;
	const TSharedRef<TJsonReader<>> MismatchedDynamicReader =
		TJsonReaderFactory<>::Create(
			MismatchedDynamicResponse.GetJsonValue());
	const TArray<TSharedPtr<FJsonValue>>* MismatchedRecords = nullptr;
	TestTrue(
		TEXT("a mismatched producer prefix cannot leak a snapshot selector"),
		FJsonSerializer::Deserialize(
			MismatchedDynamicReader,
			MismatchedDynamicObject)
			&& MismatchedDynamicObject.IsValid()
			&& MismatchedDynamicObject->TryGetArrayField(
				TEXT("objects"), MismatchedRecords)
			&& MismatchedRecords
			&& MismatchedRecords->IsEmpty());

	FQueryPublicProjectionDescriptor IncompleteDynamicProjection =
		DynamicProjection;
	IncompleteDynamicProjection.DynamicFields.TargetKind.Empty();
	TestFalse(
		TEXT("an incomplete dynamic producer scope fails closed"),
		IncompleteDynamicProjection.IsValid());
	FQueryPublicProjectionDescriptor CollidingDynamicProjection =
		DynamicProjection;
	CollidingDynamicProjection.FieldProjections.Add(
		{TEXT("fixture"), TEXT("fields"), false});
	TestFalse(
		TEXT("a dynamic fields container cannot collide with a static field"),
		CollidingDynamicProjection.IsValid());

	FQueryPublicProjectionDescriptor OwnerProjection = DynamicProjection;
	OwnerProjection.Id = TEXT("fixture_owner_projection");
	OwnerProjection.OwnerPublicName = TEXT("object_path");
	TestTrue(
		TEXT("an explicit business owner projection is valid"),
		OwnerProjection.IsValid());
	FUeAgentContractValue OwnerResponse(TEXT("{}"));
	TestTrue(
		TEXT("an explicit business owner uses the shared public codec"),
		FUeAgentQueryPayloadCodec::TryEncodePublic(
			OwnerProjection,
			TEXT("fixture.owner"),
			TEXT("fixture.owner"),
			true,
			Result->GetValue(),
			Resources,
			OwnerResponse,
			Error));
	TSharedPtr<FJsonObject> OwnerObject;
	const TSharedRef<TJsonReader<>> OwnerReader =
		TJsonReaderFactory<>::Create(OwnerResponse.GetJsonValue());
	const TArray<TSharedPtr<FJsonValue>>* OwnerRecords = nullptr;
	TestTrue(
		TEXT("the opted-in business owner is emitted once"),
		FJsonSerializer::Deserialize(OwnerReader, OwnerObject)
			&& OwnerObject.IsValid()
			&& OwnerObject->TryGetArrayField(TEXT("objects"), OwnerRecords)
			&& OwnerRecords
			&& OwnerRecords->Num() == 1
			&& (*OwnerRecords)[0]->AsObject().IsValid()
			&& (*OwnerRecords)[0]->AsObject()->GetStringField(TEXT("object_path"))
				== TEXT("/Game/Fixture"));
	FQueryPublicProjectionDescriptor CollidingOwnerProjection =
		OwnerProjection;
	CollidingOwnerProjection.OwnerPublicName = TEXT("fields");
	TestFalse(
		TEXT("an owner field cannot collide with another public field"),
		CollidingOwnerProjection.IsValid());

	FQueryPublicFieldProjection NestedObject{
		TEXT("structure"), TEXT("structure"), true,
		EUeAgentQueryPublicIdentityField::Selector,
		{
			{TEXT("display_rate"), TEXT("display_rate"), true,
				EUeAgentQueryPublicIdentityField::Selector,
				{
					{TEXT("numerator"), TEXT("numerator"), true},
					{TEXT("denominator"), TEXT("denominator"), true},
				}, false},
			{TEXT("bindings"), TEXT("bindings"), false,
				EUeAgentQueryPublicIdentityField::Selector,
				{
					{TEXT("binding_ref"), TEXT("binding_ref"), true},
					{TEXT("name"), TEXT("name"), true},
					{TEXT("track_count"), TEXT("track_count"), true},
					{TEXT("tracks"), TEXT("tracks"), false,
						EUeAgentQueryPublicIdentityField::Selector,
						{
							{TEXT("track_ref"), TEXT("track_ref"), true},
							{TEXT("track_type"), TEXT("track_type"), true},
							{TEXT("section_count"), TEXT("section_count"), true},
							{TEXT("sections"), TEXT("sections"), false,
								EUeAgentQueryPublicIdentityField::Selector,
								{
									{TEXT("section_ref"), TEXT("section_ref"), true},
									{TEXT("section_type"), TEXT("section_type"), true},
									{TEXT("key_count"), TEXT("key_count"), false},
									{TEXT("keys"), TEXT("keys"), false,
										EUeAgentQueryPublicIdentityField::Selector,
										{}, true},
								}, true},
						}, true},
				}, true},
			{TEXT("root_tracks"), TEXT("root_tracks"), false,
				EUeAgentQueryPublicIdentityField::Selector, {}, true},
		}, false};
	FQueryPublicProjectionDescriptor NestedProjection{
		TEXT("fixture_nested_projection"),
		EUeAgentQueryPublicProjectionStrategy::SingleRecord,
		TEXT("record"),
		{{TEXT("fixture.structure"), TEXT("structure"), true,
			EUeAgentQueryPublicIdentityField::ObjectRole,
			NestedObject.Children, false}},
		{{TEXT("structure"), TEXT("object"), true,
			TEXT("Bounded nested structure fixture.")}},
		TEXT("Only explicitly admitted nested fields are public."),
		false, {}, FString(), {}, FString(), {TEXT("fixture.nested")}};
	TestTrue(
		TEXT("nested object and array item schema is valid"),
		NestedProjection.IsValid());
	FQueryPublicProjectionDescriptor DuplicateNested = NestedProjection;
	DuplicateNested.FieldProjections[0].Children.Add(
		{TEXT("display_rate"), TEXT("other"), false});
	TestFalse(
		TEXT("nested duplicate selectors fail closed"),
		DuplicateNested.IsValid());
	FQueryPublicProjectionDescriptor DuplicateNestedName = NestedProjection;
	DuplicateNestedName.FieldProjections[0].Children.Add(
		{TEXT("other"), TEXT("display_rate"), false});
	TestFalse(
		TEXT("nested duplicate public names fail closed"),
		DuplicateNestedName.IsValid());

	const FQueryPublicProjectionDescriptor* ObservationProjection =
		FindQueryPublicProjectionDescriptor(TEXT("editor_project_observation"));
	TestNotNull(TEXT("shared capability observation projection is registered"), ObservationProjection);
	FUeAgentResourceTracker ObservationResources = MakeTracker(Fixture);
	const auto ObservationSnapshot = NestedProjectionFixtureSnapshot(this, ObservationResources,
		TEXT("{\"editor_project_capability\":\"target.read\",\"target_ref\":\"fixture-target\","
			"\"entries\":{\"Hash\":\"business\",\"Identity\":[\"first\",\"second\"]},"
			"\"merged_values\":{\"Identity\":{\"values\":{\"Hash\":\"config-value\"},\"source_file\":\"private\"}},"
			"\"profile_id\":\"private\",\"output_file\":\"private\"}"));
	if (ObservationProjection && ObservationSnapshot.IsValid())
	{
		const FUeAgentProjectionQueryResult ObservationResult(FixtureDescriptor(),
			TEXT("capability-observation"), FUeAgentProjectionSourceValue(ObservationSnapshot.ToSharedRef()));
		const FUeAgentProjectionRequest ObservationRequest(
			FUeAgentProjectionProfile(TEXT("compact"), false, false, false), {}, {},
			QueryPublicProjectionPageUnit(*ObservationProjection));
		const FUeAgentCancellationSource ObservationCancellation([]() { return 100.0; });
		TSharedPtr<const FUeAgentProjectedResponse> ObservationProjected;
		FUeAgentProjectionError ObservationError;
		TestTrue(TEXT("project one complete capability observation"), Graph->Projector->TryProject(
			ObservationResult, ObservationRequest, ObservationResources,
			FUeAgentOperationDeadline::FromTimeout(30.0, []() { return 100.0; }),
			ObservationCancellation.GetToken(), ObservationProjected, ObservationError));
		if (ObservationProjected.IsValid())
		{
			FUeAgentContractValue ObservationEncoded(TEXT("{}"));
			const FUeAgentQueryBindingResponse ObservationResponse(ObservationProjected.ToSharedRef(), {});
			TestTrue(TEXT("encode complete capability observation"), FUeAgentQueryPayloadCodec::TryEncodePublic(
				*ObservationProjection, TEXT("config.section"), TEXT("asset_project.read.config_section"), true,
				ObservationResponse, ObservationResources, ObservationEncoded, Error));
			TSharedPtr<FJsonObject> ObservationPublic;
			const auto ObservationReader = TJsonReaderFactory<>::Create(ObservationEncoded.GetJsonValue());
			TestTrue(TEXT("capability observation is structured JSON"),
				FJsonSerializer::Deserialize(ObservationReader, ObservationPublic) && ObservationPublic.IsValid());
			if (ObservationPublic.IsValid() && ObservationPublic->HasTypedField<EJson::Object>(TEXT("observation")))
			{
				const auto Observation = ObservationPublic->GetObjectField(TEXT("observation"));
				if (TestTrue(TEXT("capability observation retains entries"),
					Observation->HasTypedField<EJson::Object>(TEXT("entries"))))
				{
					const auto Entries = Observation->GetObjectField(TEXT("entries"));
					FString HashValue;
					TestTrue(TEXT("business Hash key survives native observation projection"),
						Entries->TryGetStringField(TEXT("Hash"), HashValue) && HashValue == TEXT("business"));
					const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
					TestTrue(TEXT("repeated native config values preserve order"),
						Entries->TryGetArrayField(TEXT("Identity"), Values) && Values && Values->Num() == 2
						&& (*Values)[0]->AsString() == TEXT("first") && (*Values)[1]->AsString() == TEXT("second"));
				}
				const TSharedPtr<FJsonObject>* Sections = nullptr;
				const TSharedPtr<FJsonObject>* Section = nullptr;
				const TSharedPtr<FJsonObject>* Values = nullptr;
				if (TestTrue(TEXT("keyed object map retains its shared value schema"),
					Observation->TryGetObjectField(TEXT("merged_values"), Sections) && Sections
					&& (*Sections)->TryGetObjectField(TEXT("Identity"), Section) && Section
					&& (*Section)->TryGetObjectField(TEXT("values"), Values) && Values))
				{
					FString HashValue;
					TestTrue(TEXT("business key survives in nested map"),
						(*Values)->TryGetStringField(TEXT("Hash"), HashValue) && HashValue == TEXT("config-value"));
					TestTrue(TEXT("native observation retains requested source data for the SDK"), (*Section)->HasField(TEXT("source_file")));
				}
				TestTrue(TEXT("lossless native observation retains custody for SDK-owned projection"), Observation->HasField(TEXT("output_file")));
			}
			else TestTrue(TEXT("capability observation must be present"), false);
		}
	}

	FUeAgentResourceTracker NestedResources = MakeTracker(Fixture);
	const TSharedPtr<const FUeAgentCanonicalSnapshot> NestedSnapshot =
		NestedProjectionFixtureSnapshot(this, NestedResources);
	if (!NestedSnapshot.IsValid())
	{
		return false;
	}
	const FUeAgentProjectionQueryResult NestedProjectionResult(
		FixtureDescriptor(),
		TEXT("nested-public-projection"),
		FUeAgentProjectionSourceValue(NestedSnapshot.ToSharedRef()));
	const FUeAgentProjectionRequest NestedProjectionRequest(
		FUeAgentProjectionProfile(TEXT("compact"), false, false, false),
		{},
		{},
		EUeAgentProjectionPageUnit::SingleRecord);
	const FUeAgentCancellationSource NestedCancellation([]() { return 100.0; });
	TSharedPtr<const FUeAgentProjectedResponse> NestedProjected;
	FUeAgentProjectionError NestedProjectionError;
	TestTrue(
		TEXT("project nested public projection fixture"),
		Graph->Projector->TryProject(
			NestedProjectionResult,
			NestedProjectionRequest,
			NestedResources,
			FUeAgentOperationDeadline::FromTimeout(30.0, []() { return 100.0; }),
			NestedCancellation.GetToken(),
			NestedProjected,
			NestedProjectionError));
	FUeAgentContractValue NestedEncoded(TEXT("{}"));
	if (NestedProjected.IsValid())
	{
		const FUeAgentQueryBindingResponse NestedResponse(
			NestedProjected.ToSharedRef(),
			{});
		TestTrue(
			TEXT("encode nested public projection fixture"),
			FUeAgentQueryPayloadCodec::TryEncodePublic(
				NestedProjection,
				TEXT("fixture.nested"),
				TEXT("fixture.nested"),
				true,
				NestedResponse,
				NestedResources,
				NestedEncoded,
				Error));
	}
	TSharedPtr<FJsonObject> NestedPublic;
	const TSharedRef<TJsonReader<>> NestedReader =
		TJsonReaderFactory<>::Create(NestedEncoded.GetJsonValue());
	TestTrue(
		TEXT("nested public projection is valid JSON"),
		FJsonSerializer::Deserialize(NestedReader, NestedPublic)
			&& NestedPublic.IsValid());
	if (NestedPublic.IsValid())
	{
		const TSharedPtr<FJsonObject> Structure =
			NestedPublic->GetObjectField(TEXT("structure"));
		const TSharedPtr<FJsonObject> DisplayRate =
			Structure->GetObjectField(TEXT("display_rate"));
		TestFalse(
			TEXT("nested object rejects undeclared fields"),
			DisplayRate->HasField(TEXT("private_rate"))
				|| Structure->HasField(TEXT("internal_state")));
		const TArray<TSharedPtr<FJsonValue>>* RootTracks = nullptr;
		TestTrue(
			TEXT("observed empty nested arrays remain present"),
			Structure->TryGetArrayField(TEXT("root_tracks"), RootTracks) && RootTracks && RootTracks->IsEmpty());
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		TestTrue(
			TEXT("nested array remains complete"),
			Structure->TryGetArrayField(TEXT("bindings"), Bindings)
				&& Bindings
				&& Bindings->Num() == 2);
		if (Bindings && !Bindings->IsEmpty())
		{
			const TSharedPtr<FJsonObject> Binding = (*Bindings)[0]->AsObject();
			TestFalse(
				TEXT("nested array item rejects undeclared fields"),
				Binding->HasField(TEXT("revision")));
			const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
			TestTrue(
				TEXT("nested object-array chain remains available"),
				Binding->TryGetArrayField(TEXT("tracks"), Tracks)
					&& Tracks && Tracks->Num() == 1
					&& (*Tracks)[0]->AsObject()->TryGetArrayField(
						TEXT("sections"), Sections)
					&& Sections && Sections->Num() == 1
					&& (*Sections)[0]->AsObject()->TryGetArrayField(
						TEXT("keys"), Keys)
					&& Keys && Keys->Num() == 2);
			if (Keys && Keys->Num() == 2)
			{
				TestEqual(
					TEXT("scalar array item survives nested projection"),
					(*Keys)[0]->AsString(),
					FString(TEXT("10")));
				TestEqual(
					TEXT("second scalar array item survives nested projection"),
					(*Keys)[1]->AsString(),
					FString(TEXT("20")));
			}
			TestFalse(
				TEXT("deep nested item rejects undeclared fields"),
				(*Tracks)[0]->AsObject()->HasField(TEXT("object_path"))
					|| (*Sections)[0]->AsObject()->HasField(TEXT("native_evidence")));
		}
	}
	if (NestedProjected.IsValid())
	{
		FQueryPublicProjectionDescriptor MismatchedProjection =
			NestedProjection;
		MismatchedProjection.FieldProjections[0].Children[0].bArray = true;
		const FUeAgentQueryBindingResponse NestedResponse(
			NestedProjected.ToSharedRef(),
			{});
		FUeAgentContractValue MismatchedEncoded(TEXT("{}"));
		TestFalse(
			TEXT("a required nested type mismatch fails public encoding"),
			FUeAgentQueryPayloadCodec::TryEncodePublic(
				MismatchedProjection,
				TEXT("fixture.nested"),
				TEXT("fixture.nested"),
				true,
				NestedResponse,
				NestedResources,
				MismatchedEncoded,
				Error));
		TestEqual(TEXT("nested type mismatch has a structured reason"), Error.Code,
			FString(TEXT("query_projection_field_invalid")));
		TestEqual(TEXT("nested type mismatch identifies the declared public field"), Error.PropertyPath,
			FString(TEXT("structure.display_rate")));
	}

	const TSharedPtr<FJsonObject> SnapshotObject =
		ResponseObject
			->GetObjectField(TEXT("projection"))
			->GetObjectField(TEXT("payload"))
			->GetObjectField(TEXT("snapshot"));
	const TSharedPtr<FJsonObject> ProjectionObject =
		ResponseObject->GetObjectField(TEXT("projection"));
	for (const FString& InternalField : {
		TEXT("descriptor"),
		TEXT("request_id"),
		TEXT("result_kind"),
		TEXT("token_estimate"),
	})
	{
		TestFalse(
			*FString::Printf(
				TEXT("full projection omits internal field %s"),
				*InternalField),
			ProjectionObject->HasField(InternalField));
	}
	TestFalse(
		TEXT("encoded response omits internal binding ticket"),
		ResponseObject->HasField(TEXT("ticket")));
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
		TestTrue(
			TEXT("full profile preserves observed revision"),
			EncodedEntries[0]
				->AsObject()
				->HasTypedField<EJson::Object>(TEXT("observed_revision")));
		TestTrue(
			TEXT("full profile preserves projected defaults"),
			EncodedEntries[0]
				->AsObject()
				->HasTypedField<EJson::Object>(TEXT("default")));
		TestTrue(
			TEXT("full profile preserves projected details"),
			EncodedEntries[0]
				->AsObject()
				->HasTypedField<EJson::Object>(TEXT("details")));
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
	const FUeAgentQueryBindingRequest CompactRequest(
		FixtureDescriptor(),
		TEXT("query-binding-state-compact"),
		FUeAgentQueryBindingPayload(
			FUeAgentStateReadQuery(
				FixtureSession(Fixture),
				Ticket.GetValue(),
				Candidate,
				EUeAgentStatePlane::AuthoringMemory,
				FUeAgentProjectionRequest(
					FUeAgentProjectionProfile(
						TEXT("compact"),
						false,
						false,
						false),
					{},
					{}))));
	FUeAgentResourceTracker CompactResources = MakeTracker(Fixture);
	TSharedPtr<const FUeAgentQueryBindingResult> CompactResult;
	TestTrue(
		TEXT("compact state read projects through the same C3Q boundary"),
		Graph->Binding->TrySubmit(
			CompactRequest,
			FixtureCaller(),
			CompactResources,
			Deadline,
			Cancellation,
			CompactResult,
			Error));
	FUeAgentContractValue CompactEncoded(TEXT("{}"));
	TestTrue(
		TEXT("compact state response encodes"),
		FUeAgentQueryPayloadCodec::TryEncode(
			CompactResult->GetValue(),
			CompactResources,
			CompactEncoded,
			Error));
	TSharedPtr<FJsonObject> CompactResponseObject;
	const TSharedRef<TJsonReader<>> CompactResponseReader =
		TJsonReaderFactory<>::Create(CompactEncoded.GetJsonValue());
	TestTrue(
		TEXT("encoded compact response is an object"),
		FJsonSerializer::Deserialize(
			CompactResponseReader,
			CompactResponseObject)
			&& CompactResponseObject.IsValid());
	const TSharedPtr<FJsonObject> CompactSnapshotObject =
		CompactResponseObject
			->GetObjectField(TEXT("projection"))
			->GetObjectField(TEXT("payload"))
			->GetObjectField(TEXT("snapshot"));
	const TSharedPtr<FJsonObject> CompactProjectionObject =
		CompactResponseObject->GetObjectField(TEXT("projection"));
	for (const FString& InternalField : {
		TEXT("descriptor"),
		TEXT("request_id"),
		TEXT("result_kind"),
		TEXT("token_estimate"),
	})
	{
		TestFalse(
			*FString::Printf(
				TEXT("compact projection omits internal field %s"),
				*InternalField),
			CompactProjectionObject->HasField(InternalField));
	}
	TestEqual(
		TEXT("compact projection identifies its profile once"),
		CompactProjectionObject->GetStringField(TEXT("profile_id")),
		FString(TEXT("compact")));
	TestFalse(
		TEXT("compact snapshot omits unavailable provenance"),
		CompactSnapshotObject->HasField(TEXT("provenance")));
	const TArray<TSharedPtr<FJsonValue>>& CompactEntries =
		CompactSnapshotObject->GetArrayField(TEXT("entries"));
	TestEqual(TEXT("compact state keeps the entry"), CompactEntries.Num(), 1);
	if (CompactEntries.Num() == 1)
	{
		const TSharedPtr<FJsonObject> CompactEntry =
			CompactEntries[0]->AsObject();
		TestFalse(
			TEXT("compact entry omits the expanded canonical identity"),
			CompactEntry->HasField(TEXT("identity")));
		TestFalse(
			TEXT("compact entry omits type metadata duplicated by value kind"),
			CompactEntry->HasField(TEXT("type")));
		TestEqual(
			TEXT("compact entry keeps the directly consumable owner identity"),
			CompactEntry->GetStringField(TEXT("owner_id")),
			Candidate.GetSummary().GetIdentity().GetOwnerId());
		TestEqual(
			TEXT("compact entry keeps the field selector"),
			CompactEntry->GetStringField(TEXT("selector")),
			Candidate.GetSummary().GetIdentity().GetSelector());
		TestTrue(
			TEXT("compact entry keeps the canonical value"),
			CompactEntry->HasTypedField<EJson::Object>(TEXT("value")));
		TestFalse(
			TEXT("compact entry omits snapshot-equal observed revision"),
			CompactEntry->HasField(TEXT("observed_revision")));
		TestFalse(
			TEXT("compact entry omits unavailable default"),
			CompactEntry->HasField(TEXT("default")));
		TestFalse(
			TEXT("compact entry omits unavailable details"),
			CompactEntry->HasField(TEXT("details")));
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
