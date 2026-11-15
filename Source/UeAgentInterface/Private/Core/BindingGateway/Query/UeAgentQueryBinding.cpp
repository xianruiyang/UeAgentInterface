#include "Core/BindingGateway/Query/UeAgentQueryBinding.h"

#include "Core/BindingGateway/Ticket/UeAgentBindingTicketJson.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Utilities/UeAgentCurveJson.h"

namespace UE::UAI::BindingGateway::Query
{
	namespace
	{
		using UE::UAI::ContextResource::FUeAgentCancellationRequested;
		using UE::UAI::ContextResource::FUeAgentOperationDeadlineExceeded;
		using UE::UAI::ContextResource::FUeAgentResourceChargeError;
		using UE::UAI::Discovery::EUeAgentCandidateOperation;
		using UE::UAI::Discovery::EUeAgentCatalogCompleteness;
		using UE::UAI::Discovery::EUeAgentDiscoveryMatchMode;
		using UE::UAI::Discovery::EUeAgentMaterializationPolicy;
		using UE::UAI::Discovery::FUeAgentDiscoveryError;
		using UE::UAI::Projection::FUeAgentProjectionError;
		using UE::UAI::Projection::FUeAgentProjectionQueryResult;
		using UE::UAI::Projection::FUeAgentProjectionSourceValue;

		void SetFailure(
			FUeAgentQueryBindingError& OutError,
			const FString& Code,
			const FString& InternalSubject,
			const FString& PublicReason,
			const FString& PropertyPath = FString())
		{
			OutError = FUeAgentQueryBindingError::Failure(Code, PublicReason, TEXT("query_binding"), FString(), InternalSubject);
			OutError.PropertyPath = PropertyPath;
		}

		template <typename TRequest>
		bool ValidateRequest(const TRequest& Request, const FUeAgentCallerContext& Caller, FUeAgentQueryBindingError& OutError)
		{
			if (!Request.GetDescriptorIdentity().IsValid()) OutError = Request.GetDescriptorIdentity().GetValidationDiagnostic();
			else if (Request.GetRequestId().IsEmpty())
				SetFailure(OutError, TEXT("query_request_invalid"), FString(), TEXT("The query request identifier must not be empty."), TEXT("request_id"));
			else if (!Request.GetPayload().IsValid()) OutError = Request.GetPayload().GetValidationDiagnostic();
			else if (!Caller.IsValid()) OutError = Caller.GetValidationDiagnostic();
			else return true;
			return false;
		}

		void ForwardDiagnostic(const UE::UAI::Contract::FDiagnostic& Source, FUeAgentQueryBindingError& OutError)
		{
			OutError = Source;
		}




	}

	FUeAgentCatalogSearchQuery::FUeAgentCatalogSearchQuery(
		FUeAgentQuerySessionIdentity InSession,
		FUeAgentDiscoveryQuery InQuery,
		FUeAgentProjectionRequest InProjection)
		: Session(MoveTemp(InSession))
		, Query(MoveTemp(InQuery))
		, Projection(MoveTemp(InProjection))
	{
		if (!Session.IsValid()) ValidationDiagnostic = Session.GetValidationDiagnostic();
		else if (!Query.IsValid()) ValidationDiagnostic = *Query.GetValidationDiagnostic();
		else if (!Projection.IsValid()) ValidationDiagnostic = Projection.GetValidationDiagnostic();
	}

	const FUeAgentQuerySessionIdentity& FUeAgentCatalogSearchQuery::GetSession() const { return Session; }
	const FUeAgentDiscoveryQuery& FUeAgentCatalogSearchQuery::GetQuery() const { return Query; }
	const FUeAgentProjectionRequest& FUeAgentCatalogSearchQuery::GetProjection() const { return Projection; }
	bool FUeAgentCatalogSearchQuery::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FUeAgentQueryBindingError& FUeAgentCatalogSearchQuery::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	FUeAgentCatalogResolveQuery::FUeAgentCatalogResolveQuery(
		FUeAgentQuerySessionIdentity InSession,
		TSharedRef<const FUeAgentCatalogSnapshot> InCatalog,
		FString InCandidateId,
		FUeAgentProjectionRequest InProjection)
		: Session(MoveTemp(InSession))
		, Catalog(MoveTemp(InCatalog))
		, CandidateId(MoveTemp(InCandidateId))
		, Projection(MoveTemp(InProjection))
	{
		CandidateId.TrimStartAndEndInline();
		if (!Session.IsValid()) ValidationDiagnostic = Session.GetValidationDiagnostic();
		else if (!Catalog->IsValid())
		{
			ValidationDiagnostic = FUeAgentQueryBindingError::Failure(TEXT("query_catalog_invalid"),
				TEXT("Candidate resolution requires a successfully created catalog snapshot."), TEXT("query_binding"));
			ValidationDiagnostic.PropertyPath = TEXT("catalog");
		}
		else if (CandidateId.IsEmpty())
		{
			ValidationDiagnostic = FUeAgentQueryBindingError::Failure(TEXT("query_candidate_id_missing"),
				TEXT("Candidate resolution requires a non-empty candidate identifier."), TEXT("query_binding"));
			ValidationDiagnostic.PropertyPath = TEXT("candidate_id");
		}
		else if (!Projection.IsValid()) ValidationDiagnostic = Projection.GetValidationDiagnostic();
	}

	const FUeAgentQuerySessionIdentity& FUeAgentCatalogResolveQuery::GetSession() const { return Session; }
	const TSharedRef<const FUeAgentCatalogSnapshot>& FUeAgentCatalogResolveQuery::GetCatalog() const { return Catalog; }
	const FString& FUeAgentCatalogResolveQuery::GetCandidateId() const { return CandidateId; }
	const FUeAgentProjectionRequest& FUeAgentCatalogResolveQuery::GetProjection() const { return Projection; }
	bool FUeAgentCatalogResolveQuery::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FUeAgentQueryBindingError& FUeAgentCatalogResolveQuery::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	FUeAgentStateReadQuery::FUeAgentStateReadQuery(
		FUeAgentQuerySessionIdentity InSession,
		FUeAgentQueryTicket InTicket,
		FUeAgentCandidateBinding InBinding,
		EUeAgentStatePlane InStatePlane,
		FUeAgentProjectionRequest InProjection)
		: Session(MoveTemp(InSession))
		, Ticket(MoveTemp(InTicket))
		, Binding(MoveTemp(InBinding))
		, StatePlane(InStatePlane)
		, Projection(MoveTemp(InProjection))
	{
		if (!Session.IsValid()) ValidationDiagnostic = Session.GetValidationDiagnostic();
		else if (!Ticket.IsValid()) ValidationDiagnostic = Ticket.GetValidationDiagnostic();
		else if (!Binding.IsValid()) ValidationDiagnostic = *Binding.GetValidationDiagnostic();
		else if (!Projection.IsValid()) ValidationDiagnostic = Projection.GetValidationDiagnostic();
	}

	const FUeAgentQuerySessionIdentity& FUeAgentStateReadQuery::GetSession() const { return Session; }
	const FUeAgentQueryTicket& FUeAgentStateReadQuery::GetTicket() const { return Ticket; }
	const FUeAgentCandidateBinding& FUeAgentStateReadQuery::GetBinding() const { return Binding; }
	EUeAgentStatePlane FUeAgentStateReadQuery::GetStatePlane() const { return StatePlane; }
	const FUeAgentProjectionRequest& FUeAgentStateReadQuery::GetProjection() const { return Projection; }
	bool FUeAgentStateReadQuery::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FUeAgentQueryBindingError& FUeAgentStateReadQuery::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	FUeAgentQueryBindingPayload::FUeAgentQueryBindingPayload(
		FUeAgentCatalogSearchQuery InQuery)
		: Kind(EUeAgentQueryBindingPayloadKind::CatalogSearch)
		, CatalogSearch(MakeShared<FUeAgentCatalogSearchQuery>(MoveTemp(InQuery)))
	{
		ValidationDiagnostic = CatalogSearch->GetValidationDiagnostic();
	}

	FUeAgentQueryBindingPayload::FUeAgentQueryBindingPayload(
		FUeAgentCatalogResolveQuery InQuery)
		: Kind(EUeAgentQueryBindingPayloadKind::CatalogResolve)
		, CatalogResolve(MakeShared<FUeAgentCatalogResolveQuery>(MoveTemp(InQuery)))
	{
		ValidationDiagnostic = CatalogResolve->GetValidationDiagnostic();
	}

	FUeAgentQueryBindingPayload::FUeAgentQueryBindingPayload(
		FUeAgentStateReadQuery InQuery)
		: Kind(EUeAgentQueryBindingPayloadKind::StateRead)
		, StateRead(MakeShared<FUeAgentStateReadQuery>(MoveTemp(InQuery)))
	{
		ValidationDiagnostic = StateRead->GetValidationDiagnostic();
	}

	FUeAgentQueryBindingPayload::FUeAgentQueryBindingPayload(
		FUeAgentDomainQuery InQuery)
		: Kind(EUeAgentQueryBindingPayloadKind::DomainQuery)
		, DomainQuery(
			MakeShared<FUeAgentDomainQuery>(MoveTemp(InQuery)))
	{
		ValidationDiagnostic = DomainQuery->GetValidationDiagnostic();
	}

	EUeAgentQueryBindingPayloadKind FUeAgentQueryBindingPayload::GetKind() const { return Kind; }
	const TSharedPtr<const FUeAgentCatalogSearchQuery>& FUeAgentQueryBindingPayload::GetCatalogSearch() const { return CatalogSearch; }
	const TSharedPtr<const FUeAgentCatalogResolveQuery>& FUeAgentQueryBindingPayload::GetCatalogResolve() const { return CatalogResolve; }
	const TSharedPtr<const FUeAgentStateReadQuery>& FUeAgentQueryBindingPayload::GetStateRead() const { return StateRead; }
	const TSharedPtr<const FUeAgentDomainQuery>&
	FUeAgentQueryBindingPayload::GetDomainQuery() const
	{
		return DomainQuery;
	}

	bool FUeAgentQueryBindingPayload::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	const FUeAgentQueryBindingError& FUeAgentQueryBindingPayload::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	FUeAgentQueryBindingResponse::FUeAgentQueryBindingResponse(
		TSharedRef<const FUeAgentProjectedResponse> InProjection,
		TOptional<FUeAgentQueryTicket> InTicket,
		TSharedPtr<const FUeAgentProjectionSourceValue> InSource)
		: Projection(MoveTemp(InProjection))
		, Ticket(MoveTemp(InTicket))
		, Source(MoveTemp(InSource))
	{
		if (!Projection->IsValid()) ValidationDiagnostic = Projection->GetValidationDiagnostic();
		else if (Source.IsValid() && !Source->IsValid())
		{
			ValidationDiagnostic = FUeAgentQueryBindingError::Failure(TEXT("query_response_invalid"),
				TEXT("The query response source must contain exactly one source variant."), TEXT("query_binding"));
			ValidationDiagnostic.PropertyPath = TEXT("source");
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(
				static_cast<int32>(Source->GetSnapshot().IsValid()) + static_cast<int32>(Source->GetCatalog().IsValid())
				+ static_cast<int32>(Source->GetCandidateBinding().IsValid()) + static_cast<int32>(Source->GetValue().IsValid())
				+ static_cast<int32>(Source->GetComparison().IsValid()));
			ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
		}
		else if (Ticket.IsSet() && !Ticket->IsValid()) ValidationDiagnostic = Ticket->GetValidationDiagnostic();
	}

	const TSharedPtr<const FUeAgentProjectedResponse>& FUeAgentQueryBindingResponse::GetProjection() const { return Projection; }
	TArray<Contract::FDiagnostic> FUeAgentQueryBindingResponse::GetDiagnostics() const
	{
		TArray<Contract::FDiagnostic> Diagnostics;
		if (!Projection.IsValid()) return Diagnostics;
		for (const auto& Diagnostic : Projection->GetDiagnostics()) Diagnostics.Add(Diagnostic.GetDiagnostic());
		const auto& Catalog = Projection->GetPayload().GetCatalog();
		if (Catalog.IsValid())
			for (const auto& Diagnostic : Catalog->GetDiagnostics()) Diagnostics.Add(Diagnostic.GetDiagnostic());
		return Diagnostics;
	}
	const TOptional<FUeAgentQueryTicket>& FUeAgentQueryBindingResponse::GetTicket() const { return Ticket; }
	const TSharedPtr<const FUeAgentProjectionSourceValue>&
	FUeAgentQueryBindingResponse::GetSource() const { return Source; }
	bool FUeAgentQueryBindingResponse::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FUeAgentQueryBindingError& FUeAgentQueryBindingResponse::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	FUeAgentQueryTicketService::FUeAgentQueryTicketService(
		TSharedRef<
			const UE::UAI::BindingGateway::Ticket::
				FUeAgentOperationTicketService> InSharedService)
		: SharedService(MoveTemp(InSharedService))
	{
	}

	bool FUeAgentQueryTicketService::TryCreate(
		const FString& IntegrityKey,
		double TtlSeconds,
		FUeAgentMonotonicClock Clock,
		FUeAgentQueryTicketIdFactory TicketIdFactory,
		TSharedPtr<const FUeAgentQueryTicketService>& OutService,
		FUeAgentQueryBindingError& OutError)
	{
		using namespace UE::UAI::BindingGateway::Ticket;
		TSharedPtr<const FUeAgentOperationTicketService> Shared;
		FUeAgentOperationTicketError Error;
		if (!FUeAgentOperationTicketService::TryCreate(
				IntegrityKey,
				TtlSeconds,
				MoveTemp(Clock),
				MoveTemp(TicketIdFactory),
				Shared,
				Error))
		{
			OutError = Error;
			return false;
		}
		OutService = MakeShareable(
			new FUeAgentQueryTicketService(
				Shared.ToSharedRef()));
		return true;
	}

	bool FUeAgentQueryTicketService::TryCreateFromShared(
		TSharedRef<
			const UE::UAI::BindingGateway::Ticket::
				FUeAgentOperationTicketService> SharedService,
		TSharedPtr<const FUeAgentQueryTicketService>& OutService,
		FUeAgentQueryBindingError& OutError)
	{
		OutError = {};
		OutService = MakeShareable(
			new FUeAgentQueryTicketService(MoveTemp(SharedService)));
		return true;
	}

	bool FUeAgentQueryTicketService::TryIssue(
		const FUeAgentCandidateBinding& Binding,
		const FUeAgentDescriptorIdentity& DescriptorIdentity,
		const FUeAgentQuerySessionIdentity& Session,
		const FUeAgentCallerContext& CallerContext,
		TOptional<FUeAgentStateRevision> TargetRevision,
		TOptional<FUeAgentQueryTicket>& OutTicket,
		FUeAgentQueryBindingError& OutError) const
	{
		UE::UAI::BindingGateway::Ticket::
			FUeAgentOperationTicketError Error;
		if (!SharedService->TryIssue(
				Binding,
				DescriptorIdentity,
				Session,
				CallerContext,
				MoveTemp(TargetRevision),
				OutTicket,
				Error))
		{
			OutError = Error;
			return false;
		}
		return true;
	}

	bool FUeAgentQueryTicketService::TryValidate(
		const FUeAgentQueryTicket& Ticket,
		const FUeAgentCandidateBinding& Binding,
		const FUeAgentDescriptorIdentity& DescriptorIdentity,
		const FUeAgentQuerySessionIdentity& Session,
		const FUeAgentCallerContext& CallerContext,
		FUeAgentQueryBindingError& OutError) const
	{
		UE::UAI::BindingGateway::Ticket::
			FUeAgentOperationTicketError Error;
		if (!SharedService->TryValidate(
				Ticket,
				Binding,
				DescriptorIdentity,
				Session,
				CallerContext,
				Error))
		{
			OutError = Error;
			return false;
		}
		return true;
	}

	namespace
	{
		TSharedRef<FJsonObject> RevisionJson(
			const FUeAgentCatalogRevision& Revision)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("source_id"), Revision.GetSourceId());
			Object->SetStringField(TEXT("generation"), Revision.GetGeneration());
			Object->SetNumberField(TEXT("sequence"), static_cast<double>(Revision.GetSequence()));
			return Object;
		}

		TSharedRef<FJsonObject> RevisionJson(
			const FUeAgentStateRevision& Revision)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("source_id"), Revision.GetSourceId());
			Object->SetStringField(TEXT("generation"), Revision.GetGeneration());
			Object->SetNumberField(TEXT("sequence"), static_cast<double>(Revision.GetSequence()));
			return Object;
		}

		TSharedRef<FJsonObject> IdentityJson(
			const UE::UAI::StateSemantic::FUeAgentCanonicalIdentity& Identity)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("semantic_id"), Identity.GetSemanticId());
			Object->SetStringField(TEXT("target_kind"), Identity.GetTargetKind());
			Object->SetStringField(TEXT("owner_id"), Identity.GetOwnerId());
			Object->SetStringField(TEXT("selector"), Identity.GetSelector());
			Object->SetStringField(TEXT("object_role"), Identity.GetObjectRole());
			Object->SetStringField(TEXT("generation"), Identity.GetGeneration());
			return Object;
		}

		TSharedRef<FJsonObject> DescriptorJson(
			const FUeAgentDescriptorIdentity& Descriptor)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("descriptor_id"), Descriptor.GetDescriptorId());
			Object->SetNumberField(TEXT("version"), Descriptor.GetVersion());
			Object->SetStringField(TEXT("descriptor_hash"), Descriptor.GetDescriptorHash());
			return Object;
		}

		TSharedRef<FJsonObject> SummaryJson(
			const UE::UAI::Discovery::FUeAgentCandidateSummary& Summary)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetObjectField(TEXT("identity"), IdentityJson(Summary.GetIdentity()));
			Object->SetObjectField(TEXT("descriptor"), DescriptorJson(Summary.GetDescriptorIdentity()));
			Object->SetStringField(TEXT("catalog_kind"), Summary.GetCatalogKind());
			Object->SetStringField(TEXT("display_name"), Summary.GetDisplayName());
			Object->SetStringField(TEXT("summary"), Summary.GetSummary());
			TArray<TSharedPtr<FJsonValue>> Categories;
			for (const FString& Category : Summary.GetCategoryPath())
			{
				Categories.Add(MakeShared<FJsonValueString>(Category));
			}
			Object->SetArrayField(TEXT("category_path"), Categories);
			return Object;
		}

		TSharedRef<FJsonObject> CatalogJson(
			const UE::UAI::Projection::FUeAgentProjectedCatalog& Catalog)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("catalog_id"), Catalog.GetCatalogId());
			Object->SetStringField(TEXT("catalog_kind"), Catalog.GetCatalogKind());
			Object->SetObjectField(TEXT("revision"), RevisionJson(Catalog.GetRevision()));
			switch (Catalog.GetCompleteness())
			{
			case EUeAgentCatalogCompleteness::Complete:
				Object->SetStringField(TEXT("completeness"), TEXT("complete"));
				break;
			case EUeAgentCatalogCompleteness::Partial:
				Object->SetStringField(TEXT("completeness"), TEXT("partial"));
				break;
			default:
				Object->SetStringField(TEXT("completeness"), TEXT("degraded"));
				break;
			}
			TArray<TSharedPtr<FJsonValue>> Candidates;
			for (const auto& Candidate : Catalog.GetCandidates())
			{
				Candidates.Add(MakeShared<FJsonValueObject>(SummaryJson(Candidate)));
			}
			Object->SetArrayField(TEXT("candidates"), Candidates);
			return Object;
		}

		bool SerializeJson(
			const TSharedRef<FJsonObject>& Object,
			FString& OutText)
		{
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutText);
			return FJsonSerializer::Serialize(Object, Writer);
		}

		bool ChargeCodec(
			FUeAgentResourceTracker& Resources,
			EUeAgentResourceDimension Dimension,
			int64 Amount,
			const FString& Path,
			FUeAgentQueryBindingError& OutError)
		{
			FUeAgentResourceChargeError Error;
			if (Resources.TryCharge(Dimension, Amount, Path, Error))
			{
				return true;
			}
			OutError = Error;
			return false;
		}

		FString StatePlaneName(EUeAgentStatePlane Plane)
		{
			const TCHAR* Value =
				UE::UAI::Contract::LexToString(Plane);
			return *Value ? Value : TEXT("unknown");
		}

		FString ValueKindName(
			UE::UAI::StateSemantic::EUeAgentValueKind Kind)
		{
			using UE::UAI::StateSemantic::EUeAgentValueKind;
			switch (Kind)
			{
			case EUeAgentValueKind::Null: return TEXT("null");
			case EUeAgentValueKind::Boolean: return TEXT("boolean");
			case EUeAgentValueKind::String: return TEXT("string");
			case EUeAgentValueKind::Integer: return TEXT("integer");
			case EUeAgentValueKind::Decimal: return TEXT("decimal");
			case EUeAgentValueKind::Array: return TEXT("array");
			case EUeAgentValueKind::Object: return TEXT("object");
			case EUeAgentValueKind::Temporal: return TEXT("temporal");
			case EUeAgentValueKind::Spatial: return TEXT("spatial");
			default: return TEXT("unknown");
			}
		}

		FString DefaultSourceKindName(
			UE::UAI::StateSemantic::EUeAgentDefaultSourceKind Kind)
		{
			using UE::UAI::StateSemantic::EUeAgentDefaultSourceKind;
			switch (Kind)
			{
			case EUeAgentDefaultSourceKind::Factory: return TEXT("factory");
			case EUeAgentDefaultSourceKind::ClassDefaultObject:
				return TEXT("class_default_object");
			case EUeAgentDefaultSourceKind::Constructor:
				return TEXT("constructor");
			case EUeAgentDefaultSourceKind::StructInitializer:
				return TEXT("struct_initializer");
			case EUeAgentDefaultSourceKind::Schema: return TEXT("schema");
			case EUeAgentDefaultSourceKind::DomainNative:
				return TEXT("domain_native");
			case EUeAgentDefaultSourceKind::PostCreateReadback:
				return TEXT("post_create_readback");
			default: return TEXT("unknown");
			}
		}

		TSharedRef<FJsonObject> TemporalJson(
			const UE::UAI::StateSemantic::FUeAgentTemporalPoint& Temporal)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("clock_id"), Temporal.GetClockId());
			Object->SetStringField(TEXT("tick"), Temporal.GetTick());
			Object->SetStringField(
				TEXT("rate_numerator"),
				Temporal.GetRateNumerator());
			Object->SetStringField(
				TEXT("rate_denominator"),
				Temporal.GetRateDenominator());
			Object->SetStringField(TEXT("state"), Temporal.GetState());
			return Object;
		}

		TArray<TSharedPtr<FJsonValue>> StringArrayJson(
			const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Values.Num());
			for (const FString& Value : Values)
			{
				Result.Add(MakeShared<FJsonValueString>(Value));
			}
			return Result;
		}

		TSharedRef<FJsonObject> CanonicalValueJson(
			const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Value)
		{
			using UE::UAI::StateSemantic::EUeAgentSpatialQuantityKind;
			using UE::UAI::StateSemantic::EUeAgentValueKind;
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(
				TEXT("kind"),
				ValueKindName(Value.GetKind()));
			switch (Value.GetKind())
			{
			case EUeAgentValueKind::Null:
				break;
			case EUeAgentValueKind::Boolean:
				Object->SetBoolField(TEXT("boolean"), Value.GetBoolean());
				break;
			case EUeAgentValueKind::String:
				Object->SetStringField(TEXT("text"), Value.GetText());
				break;
			case EUeAgentValueKind::Integer:
			case EUeAgentValueKind::Decimal:
				Object->SetStringField(TEXT("lexeme"), Value.GetLexeme());
				break;
			case EUeAgentValueKind::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				Items.Reserve(Value.GetArrayItems().Num());
				for (const auto& Item : Value.GetArrayItems())
				{
					Items.Add(MakeShared<FJsonValueObject>(
						CanonicalValueJson(Item.Get())));
				}
				Object->SetArrayField(TEXT("items"), MoveTemp(Items));
				break;
			}
			case EUeAgentValueKind::Object:
			{
				TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
				for (const auto& Item : Value.GetObjectItems())
				{
					Fields->SetObjectField(
						Item.Key,
						CanonicalValueJson(Item.Value.Get()));
				}
				Object->SetObjectField(TEXT("fields"), Fields);
				break;
			}
			case EUeAgentValueKind::Temporal:
				if (Value.GetTemporal().IsValid())
				{
					Object->SetObjectField(
						TEXT("temporal"),
						TemporalJson(*Value.GetTemporal()));
				}
				break;
			case EUeAgentValueKind::Spatial:
				if (Value.GetSpatial().IsValid())
				{
					const auto& Spatial = *Value.GetSpatial();
					const auto& Frame = Spatial.GetFrame();
					TSharedRef<FJsonObject> FrameObject =
						MakeShared<FJsonObject>();
					FrameObject->SetStringField(
						TEXT("frame_id"),
						Frame.GetFrameId());
					FrameObject->SetStringField(
						TEXT("basis_frame_id"),
						Frame.GetBasisFrameId());
					FrameObject->SetNumberField(
						TEXT("dimension"),
						Frame.GetDimension());
					TArray<TSharedPtr<FJsonValue>> Basis;
					for (const TArray<FString>& Row : Frame.GetBasis())
					{
						Basis.Add(MakeShared<FJsonValueArray>(
							StringArrayJson(Row)));
					}
					FrameObject->SetArrayField(TEXT("basis"), MoveTemp(Basis));
					FrameObject->SetArrayField(
						TEXT("origin"),
						StringArrayJson(Frame.GetOrigin()));
					FrameObject->SetStringField(TEXT("unit"), Frame.GetUnit());
					FrameObject->SetStringField(
						TEXT("basis_evidence"),
						Frame.GetBasisEvidence());
					FrameObject->SetObjectField(
						TEXT("observed_at"),
						TemporalJson(Frame.GetObservedAt()));

					Object->SetStringField(
						TEXT("subject"),
						Spatial.GetSubject());
					switch (Spatial.GetQuantityKind())
					{
					case EUeAgentSpatialQuantityKind::Point:
						Object->SetStringField(
							TEXT("quantity_kind"),
							TEXT("point"));
						break;
					case EUeAgentSpatialQuantityKind::Direction:
						Object->SetStringField(
							TEXT("quantity_kind"),
							TEXT("direction"));
						break;
					case EUeAgentSpatialQuantityKind::Normal:
						Object->SetStringField(
							TEXT("quantity_kind"),
							TEXT("normal"));
						break;
					case EUeAgentSpatialQuantityKind::Transform:
						Object->SetStringField(
							TEXT("quantity_kind"),
							TEXT("transform"));
						break;
					default:
						Object->SetStringField(
							TEXT("quantity_kind"),
							TEXT("unknown"));
						break;
					}
					Object->SetObjectField(TEXT("frame"), FrameObject);
					Object->SetArrayField(
						TEXT("components"),
						StringArrayJson(Spatial.GetComponents()));
					Object->SetObjectField(
						TEXT("observed_at"),
						TemporalJson(Spatial.GetObservedAt()));
					Object->SetArrayField(
						TEXT("conversion_evidence"),
						StringArrayJson(
							Spatial.GetConversionEvidence()));
				}
				break;
			default:
				break;
			}
			return Object;
		}

		TSharedRef<FJsonObject> TypeJson(
			const UE::UAI::StateSemantic::FUeAgentCanonicalType& Type)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("type_id"), Type.GetTypeId());
			Object->SetStringField(
				TEXT("kind"),
				ValueKindName(Type.GetValueKind()));
			Object->SetBoolField(TEXT("nullable"), Type.IsNullable());
			Object->SetStringField(
				TEXT("container_kind"),
				Type.GetContainerKind());
			return Object;
		}

		TSharedRef<FJsonObject> DefaultJson(
			const UE::UAI::StateSemantic::FUeAgentDefaultValue& Default)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(
				TEXT("source_kind"),
				DefaultSourceKindName(Default.GetSourceKind()));
			Object->SetStringField(
				TEXT("source_identity"),
				Default.GetSourceIdentity());
			Object->SetStringField(
				TEXT("provenance"),
				Default.GetProvenance());
			Object->SetObjectField(
				TEXT("value"),
				CanonicalValueJson(Default.GetValue().Get()));
			return Object;
		}

		TSharedRef<FJsonObject> DetailsJson(
			const UE::UAI::StateSemantic::FUeAgentStateDetails& Details)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(
				TEXT("display_name"),
				Details.GetDisplayName());
			Object->SetStringField(TEXT("tooltip"), Details.GetTooltip());
			Object->SetArrayField(
				TEXT("category_path"),
				StringArrayJson(Details.GetCategoryPath()));
			Object->SetBoolField(TEXT("editable"), Details.IsEditable());
			Object->SetStringField(
				TEXT("availability"),
				Details.GetAvailability());
			TSharedRef<FJsonObject> Constraints = MakeShared<FJsonObject>();
			for (const auto& Pair : Details.GetConstraints())
			{
				Constraints->SetStringField(Pair.Key, Pair.Value);
			}
			Object->SetObjectField(TEXT("constraints"), Constraints);
			return Object;
		}

		TSharedRef<FJsonObject> ProjectedSnapshotJson(
			const UE::UAI::Projection::FUeAgentProjectedSnapshot& Snapshot,
			const FString& ProfileId)
		{
			const bool bCompactProfile = ProfileId == TEXT("compact");
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(
				TEXT("snapshot_id"),
				Snapshot.GetSnapshotId());
			Object->SetStringField(
				TEXT("state_plane"),
				StatePlaneName(Snapshot.GetStatePlane()));
			Object->SetObjectField(
				TEXT("revision"),
				RevisionJson(Snapshot.GetRevision()));
			Object->SetObjectField(
				TEXT("captured_at"),
				TemporalJson(Snapshot.GetCapturedAt()));
			if (Snapshot.GetProvenance().IsSet())
			{
				const auto& Provenance =
					Snapshot.GetProvenance().GetValue();
				TSharedRef<FJsonObject> ProvenanceObject =
					MakeShared<FJsonObject>();
				ProvenanceObject->SetStringField(
					TEXT("source_kind"),
					Provenance.GetSourceKind());
				ProvenanceObject->SetStringField(
					TEXT("source_id"),
					Provenance.GetSourceId());
				ProvenanceObject->SetStringField(
					TEXT("evidence_id"),
					Provenance.GetEvidenceId());
				Object->SetObjectField(
					TEXT("provenance"),
					ProvenanceObject);
			}
			TArray<TSharedPtr<FJsonValue>> Entries;
			Entries.Reserve(Snapshot.GetEntries().Num());
			for (const auto& Entry : Snapshot.GetEntries())
			{
				TSharedRef<FJsonObject> EntryObject =
					MakeShared<FJsonObject>();
				if (bCompactProfile)
				{
					EntryObject->SetStringField(
						TEXT("owner_id"),
						Entry.GetIdentity().GetOwnerId());
					EntryObject->SetStringField(
						TEXT("selector"),
						Entry.GetIdentity().GetSelector());
				}
				else
				{
					EntryObject->SetObjectField(
						TEXT("identity"),
						IdentityJson(Entry.GetIdentity()));
					EntryObject->SetObjectField(
						TEXT("type"),
						TypeJson(Entry.GetType()));
				}
				EntryObject->SetObjectField(
					TEXT("value"),
					CanonicalValueJson(Entry.GetValue().Get()));
				if (!bCompactProfile
					|| Entry.GetObservedRevision() != Snapshot.GetRevision())
				{
					EntryObject->SetObjectField(
						TEXT("observed_revision"),
						RevisionJson(Entry.GetObservedRevision()));
				}
				if (Entry.GetDefault().IsSet())
				{
					EntryObject->SetObjectField(
						TEXT("default"),
						DefaultJson(Entry.GetDefault().GetValue()));
				}
				if (Entry.GetDetails().IsSet())
				{
					EntryObject->SetObjectField(
						TEXT("details"),
						DetailsJson(Entry.GetDetails().GetValue()));
				}
				Entries.Add(MakeShared<FJsonValueObject>(EntryObject));
			}
			Object->SetArrayField(TEXT("entries"), MoveTemp(Entries));

			const auto& Page = Snapshot.GetPage();
			TSharedRef<FJsonObject> PageObject = MakeShared<FJsonObject>();
			PageObject->SetNumberField(
				TEXT("returned_items"),
				Page.GetReturnedItems());
			PageObject->SetNumberField(
				TEXT("total_items"),
				Page.GetTotalItems());
			Object->SetObjectField(TEXT("page"), PageObject);
			Object->SetArrayField(
				TEXT("omitted_sections"),
				StringArrayJson(Snapshot.GetOmittedSections()));
			return Object;
		}

		bool IsMachineOnlyContentGraphField(const FString& FieldName)
		{
			const FString Lower = FieldName.ToLower();
			return Lower == TEXT("id")
				|| Lower == TEXT("owner_path")
				|| Lower == TEXT("owner_id")
				|| Lower == TEXT("container_id")
				|| Lower == TEXT("parent_container_id")
				|| Lower == TEXT("item_id")
				|| Lower == TEXT("relation_id")
				|| Lower == TEXT("source_item_id")
				|| Lower == TEXT("target_item_id")
				|| Lower == TEXT("endpoint_id")
				|| Lower == TEXT("asset_path")
				|| Lower.Contains(TEXT("guid"))
				|| Lower.Contains(TEXT("hash"))
				|| Lower.Contains(TEXT("digest"))
				|| Lower.Contains(TEXT("fingerprint"))
				|| Lower.Contains(TEXT("revision"))
				|| Lower.Contains(TEXT("cursor"))
				|| Lower.Contains(TEXT("ticket"))
				|| Lower.Contains(TEXT("authorization"))
				|| Lower.Contains(TEXT("correlation"));
		}

		bool IsOpaqueMachineString(const FString& Text)
		{
			FString Value = Text;
			Value.TrimStartAndEndInline();
			if (Value.Len() < 20)
			{
				return false;
			}
			int32 HexCharacters = 0;
			int32 LongestHexRun = 0;
			int32 CurrentHexRun = 0;
			int32 OtherCharacters = 0;
			for (const TCHAR Character : Value)
			{
				if (FChar::IsHexDigit(Character))
				{
					++HexCharacters;
					LongestHexRun = FMath::Max(LongestHexRun, ++CurrentHexRun);
				}
				else
				{
					CurrentHexRun = 0;
					if (Character != TEXT('-') && Character != TEXT('_')
						&& Character != TEXT('{') && Character != TEXT('}'))
					{
						++OtherCharacters;
					}
				}
			}
			return LongestHexRun >= 20
				|| (HexCharacters >= 24 && OtherCharacters == 0);
		}

		bool IsIdentityBearingContentGraphField(const FString& FieldName)
		{
			const FString Lower = FieldName.ToLower();
			return Lower == TEXT("from")
				|| Lower == TEXT("to")
				|| Lower == TEXT("source")
				|| Lower == TEXT("target")
				|| Lower == TEXT("owner")
				|| Lower.EndsWith(TEXT("_id"))
				|| Lower.EndsWith(TEXT("_key"));
		}

		enum class EPublicReferenceContext : uint8
		{
			None,
			LevelActor,
			LevelAttachment,
			ActorReference,
			PropertyValue,
			PropertyMembers,
			CurveAssetReference
		};

		EPublicReferenceContext ChildReferenceContext(
			const EPublicReferenceContext Context,
			const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Object,
			const FString& Key)
		{
			if (Context == EPublicReferenceContext::PropertyMembers)
				return EPublicReferenceContext::PropertyValue;
			if (Context == EPublicReferenceContext::PropertyValue)
			{
				if (Key == TEXT("fields") || Key == TEXT("items") || Key == TEXT("entries"))
					return EPublicReferenceContext::PropertyMembers;
				if (Key == TEXT("key") || Key == TEXT("value"))
					return EPublicReferenceContext::PropertyValue;
				if (Key.Equals(TEXT("asset_reference"), ESearchCase::CaseSensitive))
				{
					const auto HasString = [&Object](const TCHAR* Field, const TCHAR* Expected)
					{
						return Object.GetObjectItems().ContainsByPredicate([Field, Expected](const auto& Pair)
						{
							return Pair.Key.Equals(Field, ESearchCase::CaseSensitive)
								&& Pair.Value->GetKind() == UE::UAI::StateSemantic::EUeAgentValueKind::String
								&& Pair.Value->GetText().Equals(Expected, ESearchCase::CaseSensitive);
						});
					};
					if (HasString(TEXT("schema"), UeAgentCurveJson::SchemaName)
						&& HasString(TEXT("curve_kind"), TEXT("asset_reference")))
						return EPublicReferenceContext::CurveAssetReference;
				}
			}
			if (Context == EPublicReferenceContext::LevelActor && Key == TEXT("attachment"))
				return EPublicReferenceContext::LevelAttachment;
			if ((Context == EPublicReferenceContext::LevelActor && Key == TEXT("managed_id"))
				|| (Context == EPublicReferenceContext::LevelAttachment && Key == TEXT("parent_managed_id")))
				return EPublicReferenceContext::ActorReference;
			return EPublicReferenceContext::None;
		}

		TSharedPtr<FJsonValue> PublicCanonicalValueJson(
			const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Value,
			const bool bFilterMachineFields,
			const FString& FieldName = FString(),
			const EPublicReferenceContext ReferenceContext = EPublicReferenceContext::None)
		{
			using UE::UAI::StateSemantic::EUeAgentValueKind;
			switch (Value.GetKind())
			{
			case EUeAgentValueKind::Null:
				return MakeShared<FJsonValueNull>();
			case EUeAgentValueKind::Boolean:
				return MakeShared<FJsonValueBoolean>(Value.GetBoolean());
			case EUeAgentValueKind::String:
				if (bFilterMachineFields
					&& ReferenceContext != EPublicReferenceContext::ActorReference
					&& IsIdentityBearingContentGraphField(FieldName)
					&& IsOpaqueMachineString(Value.GetText()))
				{
					return nullptr;
				}
				return MakeShared<FJsonValueString>(Value.GetText());
			case EUeAgentValueKind::Integer:
			case EUeAgentValueKind::Decimal:
				// Preserve the canonical lexeme as a JSON number, so query values
				// remain valid typed authoring inputs without caller-side coercion.
				return MakeShared<FJsonValueNumberString>(Value.GetLexeme());
			case EUeAgentValueKind::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				Items.Reserve(Value.GetArrayItems().Num());
				for (const auto& Item : Value.GetArrayItems())
				{
					TSharedPtr<FJsonValue> Projected = PublicCanonicalValueJson(
						Item.Get(),
						bFilterMachineFields,
						FieldName, ReferenceContext == EPublicReferenceContext::PropertyMembers
							? EPublicReferenceContext::PropertyValue : EPublicReferenceContext::None);
					if (Projected.IsValid())
					{
						Items.Add(MoveTemp(Projected));
					}
				}
				return MakeShared<FJsonValueArray>(MoveTemp(Items));
			}
			case EUeAgentValueKind::Object:
			{
				TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
				for (const auto& Item : Value.GetObjectItems())
				{
					const bool bCurveAssetPath = ReferenceContext == EPublicReferenceContext::CurveAssetReference
						&& Item.Key.Equals(TEXT("asset_path"), ESearchCase::CaseSensitive)
						&& Item.Value->GetKind() == EUeAgentValueKind::String;
					if (bFilterMachineFields && !bCurveAssetPath && IsMachineOnlyContentGraphField(Item.Key))
					{
						continue;
					}
					TSharedPtr<FJsonValue> Projected = PublicCanonicalValueJson(
							Item.Value.Get(),
							bFilterMachineFields,
							Item.Key,
							ChildReferenceContext(ReferenceContext, Value, Item.Key));
					if (Projected.IsValid())
					{
						Fields->SetField(Item.Key, MoveTemp(Projected));
					}
				}
				return MakeShared<FJsonValueObject>(Fields);
			}
			case EUeAgentValueKind::Temporal:
				return MakeShared<FJsonValueObject>(
					CanonicalValueJson(Value));
			case EUeAgentValueKind::Spatial:
				if (Value.GetSpatial().IsValid())
				{
					const auto& Spatial = *Value.GetSpatial();
					TSharedRef<FJsonObject> Point = MakeShared<FJsonObject>();
					Point->SetStringField(
						TEXT("frame_id"),
						Spatial.GetFrame().GetFrameId());
					Point->SetStringField(
						TEXT("unit"),
						Spatial.GetFrame().GetUnit());
					Point->SetArrayField(
						TEXT("components"),
						StringArrayJson(Spatial.GetComponents()));
					return MakeShared<FJsonValueObject>(Point);
				}
				return nullptr;
			default:
				return MakeShared<FJsonValueNull>();
			}
		}

		FString PublicEntryKey(
			const UE::UAI::Projection::FUeAgentProjectedEntry& Entry)
		{
			for (const TCHAR* Selector : {
				TEXT("property_path"),
				TEXT("field_name"),
				TEXT("public_name"),
				TEXT("name"),
				TEXT("selector")})
			{
				FString Value = Entry.GetIdentity().GetSelector(Selector);
				Value.TrimStartAndEndInline();
				if (!Value.IsEmpty() && !IsOpaqueMachineString(Value))
				{
					return Value == TEXT("domain_id")
						? FString(TEXT("domain"))
						: Value;
				}
			}
			if (Entry.GetDetails().IsSet())
			{
				FString Value = Entry.GetDetails()->GetDisplayName();
				Value.TrimStartAndEndInline();
				if (!Value.IsEmpty() && !IsOpaqueMachineString(Value))
				{
					return Value;
				}
			}
			return FString();
		}

		TSharedPtr<FJsonValue> PublicEntryJson(
			const UE::UAI::Projection::FUeAgentProjectedEntry& Entry,
			const bool bFilterMachineFields)
		{
			const bool bLevelActorItem = Entry.GetIdentity().GetSemanticId() == TEXT("uai.semantic.level.actor")
				&& Entry.GetIdentity().GetObjectRole() == TEXT("item");
			TSharedPtr<FJsonValue> Value = PublicCanonicalValueJson(
				Entry.GetValue().Get(),
				bFilterMachineFields, FString(),
				bLevelActorItem ? EPublicReferenceContext::LevelActor
					: Entry.GetIdentity().GetSelector(TEXT("section")) == TEXT("properties")
						? EPublicReferenceContext::PropertyValue : EPublicReferenceContext::None);
			if (!Value.IsValid())
			{
				return nullptr;
			}
			const FString Key = PublicEntryKey(Entry);
			if (bFilterMachineFields
				&& IsMachineOnlyContentGraphField(Key))
			{
				return nullptr;
			}
			const bool bProperty = !Entry.GetIdentity()
				.GetSelector(TEXT("property_path")).IsEmpty();
			if (Value->Type != EJson::Object || bProperty)
			{
				if (Key.IsEmpty())
				{
					return Value;
				}
				TSharedRef<FJsonObject> Named = MakeShared<FJsonObject>();
				Named->SetField(Key, MoveTemp(Value));
				return MakeShared<FJsonValueObject>(Named);
			}
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			const bool bRemovedAllFields = bFilterMachineFields && Object.IsValid() && Object->Values.IsEmpty()
				&& Entry.GetValue()->GetKind() == UE::UAI::StateSemantic::EUeAgentValueKind::Object
				&& !Entry.GetValue()->GetObjectItems().IsEmpty();
			return bRemovedAllFields
				? nullptr
				: Value;
		}

		FString PublicProjectionIdentityValue(
			const UE::UAI::Projection::FUeAgentProjectedEntry& Entry,
			const EUeAgentQueryPublicIdentityField IdentityField)
		{
			return IdentityField
				== EUeAgentQueryPublicIdentityField::ObjectRole
					? Entry.GetIdentity().GetObjectRole()
					: Entry.GetIdentity().GetSelector(TEXT("selector"));
		}

		TSharedRef<FJsonObject> PublicProjectedSnapshotJson(
			const UE::UAI::Projection::FUeAgentProjectedSnapshot& Snapshot,
			const bool bFilterMachineFields,
			const bool bGroupBySection)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Entries;
			TMap<FString, TArray<TSharedPtr<FJsonValue>>> EntriesBySection;
			Entries.Reserve(Snapshot.GetEntries().Num());
			for (const auto& Entry : Snapshot.GetEntries())
			{
				TSharedPtr<FJsonValue> Value = PublicEntryJson(
					Entry,
					bFilterMachineFields);
				if (Value.IsValid())
				{
					if (bGroupBySection)
					{
						FString Section =
							Entry.GetIdentity().GetSelector(TEXT("section"));
						if (Section.IsEmpty() && Entry.GetDetails().IsSet()
							&& !Entry.GetDetails()->GetCategoryPath().IsEmpty())
						{
							Section = Entry.GetDetails()
								->GetCategoryPath().Last();
						}
						Section = Section.ToLower();
						if (Section.IsEmpty())
						{
							Section = TEXT("other");
						}
						EntriesBySection.FindOrAdd(Section).Add(MoveTemp(Value));
					}
					else
					{
						Entries.Add(MoveTemp(Value));
					}
				}
			}
			if (bGroupBySection)
			{
				TSharedRef<FJsonObject> Sections = MakeShared<FJsonObject>();
				TArray<FString> SectionNames;
				EntriesBySection.GetKeys(SectionNames);
				SectionNames.Sort();
				for (const FString& Section : SectionNames)
				{
					Sections->SetArrayField(
						Section,
						MoveTemp(EntriesBySection.FindChecked(Section)));
				}
				Object->SetObjectField(TEXT("sections"), Sections);
			}
			else
			{
				Object->SetArrayField(TEXT("entries"), MoveTemp(Entries));
			}

			return Object;
		}

		bool IsEmptyOptionalPublicValue(
			const TSharedPtr<FJsonValue>& Value,
			const FQueryPublicFieldProjection& Field)
		{
			// Presence belongs to the native observation. A returned empty business
			// container is a result, distinct from an option that was not requested.
			if (Value.IsValid() && (Field.bAuthoringValue || (Field.bNullable && Value->IsNull())
				|| (!Value->IsNull() && (Field.bArray || Field.bScalarMap || Field.bStructuredValue))))
			{
				return false;
			}
			if (!Value.IsValid() || Value->IsNull())
			{
				return true;
			}
			if (Value->Type == EJson::String)
			{
				return Value->AsString().IsEmpty();
			}
			if (Value->Type == EJson::Array)
			{
				return Value->AsArray().IsEmpty();
			}
			if (Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Object = Value->AsObject();
				return !Object.IsValid() || Object->Values.IsEmpty();
			}
			return false;
		}

		TSharedPtr<FJsonValue> ProjectNestedPublicValue(
			const TSharedPtr<FJsonValue>& Source,
			const FQueryPublicFieldProjection& Field,
			FUeAgentQueryBindingError& OutError,
			const FString& PublicPath)
		{
			auto Invalid = [&](const FString& Reason) -> TSharedPtr<FJsonValue>
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(
					TEXT("query_projection_field_invalid"), Reason, TEXT("query_projection"));
				OutError.PropertyPath = PublicPath;
				return nullptr;
			};
			if (!Source.IsValid()) return Invalid(TEXT("The query result contains an invalid value."));
			if (Field.bAuthoringValue) return Source;
			if (Source->IsNull())
			{
				if (Field.bNullable) return Source;
				if (Field.bRequired) return Invalid(TEXT("A required query result field is null."));
				return nullptr;
			}
			// Some native observers serialize their structured result into one
			// canonical string. Decode only when the public schema declares a
			// structure, then preserve a declared complete value or apply its
			// admitted-field projection recursively.
			if (Source->Type == EJson::String && (Field.bArray || Field.bScalarMap || Field.bStructuredValue || !Field.Children.IsEmpty()))
			{
				TSharedPtr<FJsonValue> Decoded;
				const auto Reader = TJsonReaderFactory<>::Create(Source->AsString());
				if (!FJsonSerializer::Deserialize(Reader, Decoded) || !Decoded.IsValid()
					|| (Decoded->Type != EJson::Object && Decoded->Type != EJson::Array))
					return Invalid(TEXT("The query result field does not contain valid structured JSON."));
				return ProjectNestedPublicValue(Decoded, Field, OutError, PublicPath);
			}
			if (Field.bStructuredValue)
				return Source->Type == EJson::Object || Source->Type == EJson::Array
					? Source : Invalid(TEXT("Expected an object or array in the query result."));
			if (Field.bScalarMap)
			{
				if (Source->Type != EJson::Object || !Source->AsObject().IsValid())
					return Invalid(TEXT("Expected an object of scalar values in the query result."));
				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				for (const auto& Pair : Source->AsObject()->Values)
				{
					if (!Pair.Value.IsValid() || Pair.Value->Type == EJson::Object || Pair.Value->Type == EJson::Array)
					{
						Invalid(TEXT("Expected a scalar map value in the query result."));
						OutError.PropertyPath = PublicPath + TEXT(".") + Pair.Key;
						return nullptr;
					}
					Result->SetField(Pair.Key, Pair.Value);
				}
				return MakeShared<FJsonValueObject>(Result);
			}
			if (Field.bArray)
			{
				if (Source->Type != EJson::Array)
				{
					return Invalid(TEXT("Expected an array in the query result."));
				}
				TArray<TSharedPtr<FJsonValue>> ProjectedItems;
				for (const TSharedPtr<FJsonValue>& Item : Source->AsArray())
				{
					const FString ItemPath = FString::Printf(TEXT("%s[%d]"), *PublicPath, ProjectedItems.Num());
					if (!Item.IsValid())
					{
						Invalid(TEXT("The query result array contains an invalid value."));
						OutError.PropertyPath = ItemPath;
						return nullptr;
					}
					if (Field.Children.IsEmpty())
					{
						if (Item->Type != EJson::Object
							&& Item->Type != EJson::Array)
						{
							ProjectedItems.Add(Item);
						}
						else
						{
							Invalid(TEXT("Expected a scalar array item in the query result."));
							OutError.PropertyPath = ItemPath;
							return nullptr;
						}
						continue;
					}
					if (Item->Type != EJson::Object
						|| !Item->AsObject().IsValid())
					{
						Invalid(TEXT("Expected an object array item in the query result."));
						OutError.PropertyPath = ItemPath;
						return nullptr;
					}
					TSharedRef<FJsonObject> ProjectedItem =
						MakeShared<FJsonObject>();
					for (const FQueryPublicFieldProjection& Child : Field.Children)
					{
						const TSharedPtr<FJsonValue>* ChildValue =
							Item->AsObject()->Values.Find(Child.Selector);
						if (!ChildValue)
						{
							if (Child.bRequired)
							{
								Invalid(TEXT("A query result record is missing a required field."));
								OutError.PropertyPath = ItemPath + TEXT(".") + Child.PublicName;
								return nullptr;
							}
							continue;
						}
						TSharedPtr<FJsonValue> Projected =
							ProjectNestedPublicValue(
								*ChildValue,
								Child, OutError, ItemPath + TEXT(".") + Child.PublicName);
						if (!OutError.IsEmpty()) return nullptr;
						if (Projected.IsValid()
							&& (Child.bRequired
								|| !IsEmptyOptionalPublicValue(Projected, Child)))
						{
							ProjectedItem->SetField(
								Child.PublicName,
								MoveTemp(Projected));
						}
					}
					ProjectedItems.Add(MakeShared<FJsonValueObject>(ProjectedItem));
				}
				return MakeShared<FJsonValueArray>(MoveTemp(ProjectedItems));
			}
			if (!Field.Children.IsEmpty())
			{
				if (Source->Type != EJson::Object
					|| !Source->AsObject().IsValid())
				{
					return Invalid(TEXT("Expected an object in the query result."));
				}
				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				for (const FQueryPublicFieldProjection& Child : Field.Children)
				{
					const TSharedPtr<FJsonValue>* ChildValue =
						Source->AsObject()->Values.Find(Child.Selector);
					if (!ChildValue)
					{
						if (Child.bRequired)
						{
							Invalid(TEXT("A query result record is missing a required field."));
							OutError.PropertyPath = PublicPath + TEXT(".") + Child.PublicName;
							return nullptr;
						}
						continue;
					}
					TSharedPtr<FJsonValue> Projected = ProjectNestedPublicValue(
						*ChildValue,
						Child, OutError, PublicPath + TEXT(".") + Child.PublicName);
					if (!OutError.IsEmpty()) return nullptr;
					if (Projected.IsValid()
						&& (Child.bRequired
							|| !IsEmptyOptionalPublicValue(Projected, Child)))
					{
						Result->SetField(Child.PublicName, MoveTemp(Projected));
					}
				}
				return TSharedPtr<FJsonValue>(
					MakeShared<FJsonValueObject>(Result));
			}
			return Source->Type == EJson::Object
				|| Source->Type == EJson::Array
				? Invalid(TEXT("Expected a scalar value in the query result."))
				: Source;
		}

		void SetPublicRecordField(
			const TSharedRef<FJsonObject>& Record,
			const FQueryPublicFieldProjection& Field,
			TSharedPtr<FJsonValue> Value)
		{
			TSharedRef<FJsonObject> Container = Record;
			for (const FString& Name : Field.PublicContainerPath)
			{
				const TSharedPtr<FJsonObject>* Existing = nullptr;
				if (Container->TryGetObjectField(Name, Existing)
					&& Existing && Existing->IsValid())
				{
					Container = Existing->ToSharedRef();
				}
				else
				{
					const TSharedRef<FJsonObject> Child = MakeShared<FJsonObject>();
					Container->SetObjectField(Name, Child);
					Container = Child;
				}
			}
			Container->SetField(Field.PublicName, MoveTemp(Value));
		}

		TSharedRef<FJsonObject> PublicGroupedRecordsJson(
			const UE::UAI::Projection::FUeAgentProjectedSnapshot& Snapshot,
			const FQueryPublicProjectionDescriptor& Descriptor, FUeAgentQueryBindingError& OutError)
		{
			struct FRecordFields final
			{
				TMap<FString, TSharedPtr<FJsonValue>> Values;
				TMap<FString, TSharedPtr<FJsonValue>> DynamicValues;
			};

			TMap<FString, FRecordFields> FieldsByOwner;
			for (const auto& Entry : Snapshot.GetEntries())
			{
				const FString OwnerId =
					Entry.GetIdentity().GetOwnerId();
				const auto& Identity = Entry.GetIdentity();
				const FQueryPublicFieldProjection* Field =
					Descriptor.FieldProjections.FindByPredicate(
						[&Identity](const FQueryPublicFieldProjection& Candidate)
						{
							return Candidate.Selector
								== (Candidate.IdentityField
										== EUeAgentQueryPublicIdentityField::ObjectRole
									? Identity.GetObjectRole()
									: Identity.GetSelector(TEXT("selector")));
						});
				const FString Selector = Identity.GetSelector(TEXT("selector"));
				const bool bDynamicField =
					!Field
					&& Descriptor.DynamicFields.IsValid()
					&& Identity.GetTargetKind()
						== Descriptor.DynamicFields.TargetKind
					&& Identity.GetSemanticId().StartsWith(
						Descriptor.DynamicFields.SemanticIdPrefix,
						ESearchCase::CaseSensitive)
					&& !Selector.IsEmpty();
				if (!Field && !bDynamicField)
				{
					continue;
				}
				if (OwnerId.IsEmpty())
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("query_projection_owner_missing"),
						TEXT("A field selected for the query result has no owning record."), TEXT("query_projection"));
					return MakeShared<FJsonObject>();
				}
				// Record eligibility is established by a declared field, not by unrelated snapshot entries.
				FRecordFields& Fields = FieldsByOwner.FindOrAdd(OwnerId);
				TSharedPtr<FJsonValue> Value = Field
					? ProjectNestedPublicValue(
						PublicCanonicalValueJson(Entry.GetValue().Get(), false),
						*Field, OutError, FString::Join(Field->PublicContainerPath, TEXT("."))
							+ (Field->PublicContainerPath.IsEmpty() ? TEXT("") : TEXT(".")) + Field->PublicName)
					: PublicCanonicalValueJson(
						Entry.GetValue().Get(), false, Selector);
				if (!OutError.IsEmpty()) return MakeShared<FJsonObject>();
				if (Field
					&& !Field->bRequired
					&& IsEmptyOptionalPublicValue(Value, *Field))
				{
					continue;
				}
				if (Value.IsValid())
				{
					if (Field)
					{
						Fields.Values.Add(
							Field->Selector,
							MoveTemp(Value));
					}
					else
					{
						Fields.DynamicValues.Add(
							Selector,
							MoveTemp(Value));
					}
				}
			}

			TArray<FString> Owners;
			FieldsByOwner.GetKeys(Owners);
			Owners.Sort();
			TArray<TSharedPtr<FJsonValue>> Records;
			Records.Reserve(Owners.Num());
			for (const FString& Owner : Owners)
			{
				FRecordFields& Fields = FieldsByOwner.FindChecked(Owner);
				const FQueryPublicFieldProjection* MissingRequired =
					Descriptor.FieldProjections.FindByPredicate(
						[&Fields](const FQueryPublicFieldProjection& Field)
						{
							return Field.bRequired
							&& !Fields.Values.Contains(Field.Selector);
						});
				if (MissingRequired)
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("query_projection_required_field_missing"),
						FString::Printf(TEXT("A query result record is missing required field '%s'."), *MissingRequired->PublicName),
						TEXT("query_projection"));
					OutError.PropertyPath = MissingRequired->PublicName;
					OutError.InternalContext = Owner;
					return MakeShared<FJsonObject>();
				}
				TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
				if (!Descriptor.OwnerPublicName.IsEmpty())
				{
					Record->SetStringField(
						Descriptor.OwnerPublicName,
						Owner);
				}
				for (const FQueryPublicFieldProjection& Field
					: Descriptor.FieldProjections)
				{
					TSharedPtr<FJsonValue>* Value =
						Fields.Values.Find(Field.Selector);
					if (Value && Value->IsValid())
					{
						SetPublicRecordField(
							Record, Field,
							MoveTemp(*Value));
					}
				}
				if (!Fields.DynamicValues.IsEmpty())
				{
					TArray<FString> DynamicNames;
					Fields.DynamicValues.GetKeys(DynamicNames);
					DynamicNames.Sort();
					TSharedRef<FJsonObject> DynamicObject =
						MakeShared<FJsonObject>();
					for (const FString& DynamicName : DynamicNames)
					{
						TSharedPtr<FJsonValue>* Value =
							Fields.DynamicValues.Find(DynamicName);
						if (Value && Value->IsValid())
						{
							DynamicObject->SetField(
								DynamicName,
								MoveTemp(*Value));
						}
					}
					Record->SetObjectField(
						Descriptor.DynamicFields.PublicContainerName,
						DynamicObject);
				}
				if (Descriptor.bFlattenSingleFieldRecords
					&& Record->Values.Num() == 1)
				{
					auto It = Record->Values.CreateIterator();
					Records.Add(It.Value());
				}
				else
				{
					Records.Add(MakeShared<FJsonValueObject>(Record));
				}
			}

			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetArrayField(
				Descriptor.CollectionField,
				MoveTemp(Records));
			return Object;
		}

		TSharedRef<FJsonObject> PublicSingleRecordJson(
			const UE::UAI::Projection::FUeAgentProjectedSnapshot& Snapshot,
			const FQueryPublicProjectionDescriptor& Descriptor, FUeAgentQueryBindingError& OutError)
		{
			const TSharedRef<FJsonObject> Grouped =
				PublicGroupedRecordsJson(Snapshot, Descriptor, OutError);
			if (!OutError.IsEmpty()) return Grouped;
			const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
			if (Grouped->TryGetArrayField(Descriptor.CollectionField, Records)
				&& Records
				&& Records->Num() == 1
				&& (*Records)[0].IsValid()
				&& (*Records)[0]->Type == EJson::Object
				&& (*Records)[0]->AsObject().IsValid())
			{
				return (*Records)[0]->AsObject().ToSharedRef();
			}
			const int32 Count = Records ? Records->Num() : 0;
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("query_projection_single_record_invalid"),
				FString::Printf(TEXT("Expected exactly one query result object; received %d records."), Count), TEXT("query_projection"));
			OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
			OutError.ActualValue = MakeShared<FJsonValueNumber>(Count);
			return MakeShared<FJsonObject>();
		}

		TSharedRef<FJsonObject> PublicSnapshotRecordArrayJson(
			const UE::UAI::Projection::FUeAgentProjectedSnapshot& Snapshot,
			const FQueryPublicProjectionDescriptor& Descriptor, FUeAgentQueryBindingError& OutError)
		{
			auto Invalid = [&](const FString& Path, const FString& Reason) -> TSharedRef<FJsonObject>
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(
					TEXT("query_projection_record_invalid"), Reason, TEXT("query_projection"));
				OutError.PropertyPath = Path;
				return MakeShared<FJsonObject>();
			};
			TArray<TSharedPtr<FJsonValue>> Records;
			for (const auto& Entry : Snapshot.GetEntries())
			{
				if (PublicProjectionIdentityValue(
						Entry,
						Descriptor.SnapshotArrayIdentityField)
					!= Descriptor.SnapshotArraySelector)
				{
					continue;
				}
				const TSharedPtr<FJsonValue> Value = PublicCanonicalValueJson(
					Entry.GetValue().Get(), false);
				if (!Value.IsValid() || Value->Type != EJson::Array)
				{
					return Invalid(Descriptor.CollectionField, TEXT("Expected an array of query result records."));
				}
				for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
				{
					const FString RecordPath = FString::Printf(TEXT("%s[%d]"), *Descriptor.CollectionField, Records.Num());
					if (!Item.IsValid() || Item->Type != EJson::Object)
					{
						return Invalid(RecordPath, TEXT("Expected a query result record object."));
					}
					const TSharedPtr<FJsonObject> Source = Item->AsObject();
					if (!Source.IsValid())
					{
						return Invalid(RecordPath, TEXT("The query result record object is invalid."));
					}
					TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
					for (const FQueryPublicFieldProjection& Field
						: Descriptor.FieldProjections)
					{
						const TSharedPtr<FJsonValue>* FieldValue =
							Source->Values.Find(Field.Selector);
						if (!FieldValue || !FieldValue->IsValid()
							|| ((*FieldValue)->IsNull() && !Field.bNullable))
						{
							if (Field.bRequired)
							{
								return Invalid(RecordPath + TEXT(".") + Field.PublicName,
									TEXT("A query result record is missing a required field."));
							}
							continue;
						}
						TSharedPtr<FJsonValue> Projected =
							ProjectNestedPublicValue(
								*FieldValue,
								Field, OutError, RecordPath + TEXT(".") + Field.PublicName);
						if (!OutError.IsEmpty()) return MakeShared<FJsonObject>();
						if (!Projected.IsValid())
						{
							continue;
						}
						if (Field.bRequired
							|| !IsEmptyOptionalPublicValue(Projected, Field))
						{
							Record->SetField(
								Field.PublicName,
								MoveTemp(Projected));
						}
					}
					Records.Add(MakeShared<FJsonValueObject>(Record));
				}
			}
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetArrayField(Descriptor.CollectionField, MoveTemp(Records));
			return Object;
		}

	}

	namespace
	{
		TSharedPtr<FJsonObject> BuildTypedResponseJson(const FUeAgentQueryBindingResponse& Response, FUeAgentQueryBindingError& OutError)
		{
			const FUeAgentProjectedResponse& Projection = *Response.GetProjection();
			TSharedRef<FJsonObject> ProjectionObject = MakeShared<FJsonObject>();
			ProjectionObject->SetStringField(TEXT("profile_id"), Projection.GetProfileId());
			TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
			const auto& ProjectedPayload = Projection.GetPayload();
			if (ProjectedPayload.GetCatalog().IsValid())
			{
				Payload->SetStringField(TEXT("kind"), TEXT("catalog"));
				Payload->SetObjectField(TEXT("catalog"), CatalogJson(*ProjectedPayload.GetCatalog()));
			}
			else if (ProjectedPayload.GetCandidateBinding().IsValid())
			{
				Payload->SetStringField(TEXT("kind"), TEXT("candidate_binding"));
				Payload->SetObjectField(
					TEXT("candidate_binding"),
					UE::UAI::BindingGateway::Ticket::
						FUeAgentBindingTicketJsonCodec::BindingToJson(
							*ProjectedPayload.GetCandidateBinding()));
			}
			else if (ProjectedPayload.GetSnapshot().IsValid())
			{
				Payload->SetStringField(TEXT("kind"), TEXT("snapshot"));
				Payload->SetObjectField(
					TEXT("snapshot"),
					ProjectedSnapshotJson(
						*ProjectedPayload.GetSnapshot(),
						Projection.GetProfileId()));
			}
			else if (ProjectedPayload.GetValue().IsValid())
			{
				Payload->SetStringField(TEXT("kind"), TEXT("value"));
			}
			else if (ProjectedPayload.GetComparison().IsValid())
			{
				Payload->SetStringField(TEXT("kind"), TEXT("comparison"));
			}
			else
			{
				SetFailure(OutError, TEXT("query_response_invalid"), TEXT("projection.payload"), TEXT("Projected payload has no recognized typed value."));
				return nullptr;
			}
			ProjectionObject->SetObjectField(TEXT("payload"), Payload);

			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetObjectField(TEXT("projection"), ProjectionObject);
			return Root;
		}
	}

	bool FUeAgentQueryPayloadCodec::TryEncode(
		const FUeAgentQueryBindingResponse& Response,
		FUeAgentResourceTracker& Resources,
		FUeAgentContractValue& OutValue,
		FUeAgentQueryBindingError& OutError)
	{
		OutError = {};
		if (!Response.IsValid())
		{
			OutError = Response.GetValidationDiagnostic();
			return false;
		}
		const TSharedPtr<FJsonObject> Root = BuildTypedResponseJson(Response, OutError);
		if (!Root.IsValid()) return false;
		FString Text;
		if (!SerializeJson(Root.ToSharedRef(), Text)
			|| !ChargeCodec(
				Resources,
				EUeAgentResourceDimension::OutputBytes,
				UE::UAI::ContextResource::MeasureText(Text).Utf8Bytes,
				TEXT("c3q.response.encode"),
				OutError))
		{
			if (OutError.IsEmpty())
			{
				SetFailure(OutError, TEXT("query_response_encode_failed"), TEXT("response"), TEXT("Failed to serialize typed C3Q response."));
			}
			return false;
		}
		OutValue = FUeAgentContractValue(MoveTemp(Text));
		return true;
	}

	bool FUeAgentQueryPayloadCodec::TryEncodePublic(
		const FQueryPublicProjectionDescriptor& PublicProjection,
		const FString& CommandId,
		const FString& MechanicsCommandId,
		bool bReadOnly,
		const FUeAgentQueryBindingResponse& Response,
		FUeAgentResourceTracker& Resources,
		FUeAgentContractValue& OutValue,
		FUeAgentQueryBindingError& OutError)
	{
		OutError = {};
		if (!Response.IsValid())
		{
			OutError = Response.GetValidationDiagnostic();
			return false;
		}
		if (!PublicProjection.TryValidate(OutError, TEXT("public_projection")))
		{
			OutError.SubjectId = PublicProjection.Id;
			return false;
		}

		if (PublicProjection.Strategy
			== EUeAgentQueryPublicProjectionStrategy::LegacyTypedEnvelope)
		{
			const TSharedPtr<FJsonObject> TypedObject = BuildTypedResponseJson(Response, OutError);
			if (!TypedObject.IsValid()) return false;
			TSharedRef<FJsonObject> LegacyRoot = MakeShared<FJsonObject>();
			LegacyRoot->SetStringField(TEXT("command_id"), CommandId);
			LegacyRoot->SetStringField(
				TEXT("mechanics_command_id"),
				MechanicsCommandId);
			LegacyRoot->SetBoolField(TEXT("read_only"), bReadOnly);
			LegacyRoot->SetObjectField(TEXT("query_result"), TypedObject);
			FString LegacyText;
			if (!SerializeJson(LegacyRoot, LegacyText))
			{
				SetFailure(
					OutError,
					TEXT("query_response_encode_failed"),
					TEXT("response"),
					TEXT("Failed to serialize the public Query response."));
				return false;
			}
			if (!ChargeCodec(Resources, EUeAgentResourceDimension::OutputBytes,
				UE::UAI::ContextResource::MeasureText(LegacyText).Utf8Bytes,
				TEXT("c3q.response.public.envelope.encode"), OutError))
			{
				return false;
			}
			OutValue = FUeAgentContractValue(MoveTemp(LegacyText));
			return true;
		}

		const FUeAgentProjectedResponse& Projection =
			*Response.GetProjection();
		const auto& Snapshot = Projection.GetPayload().GetSnapshot();
		if (!Snapshot.IsValid())
		{
			SetFailure(
				OutError,
				TEXT("query_response_invalid"),
				TEXT("projection.payload"),
				TEXT("The selected public Query projection requires a snapshot payload."));
			return false;
		}
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		switch (PublicProjection.Strategy)
		{
		case EUeAgentQueryPublicProjectionStrategy::SnapshotEntries:
			Root = PublicProjectedSnapshotJson(*Snapshot, true, false);
			break;
		case EUeAgentQueryPublicProjectionStrategy::SnapshotSections:
			Root = PublicProjectedSnapshotJson(*Snapshot, true, true);
			break;
		case EUeAgentQueryPublicProjectionStrategy::GroupedRecords:
			Root = PublicGroupedRecordsJson(*Snapshot, PublicProjection, OutError);
			if (!OutError.IsEmpty()) return false;
			break;
		case EUeAgentQueryPublicProjectionStrategy::SingleRecord:
			Root = PublicSingleRecordJson(*Snapshot, PublicProjection, OutError);
			if (!OutError.IsEmpty()) return false;
			break;
		case EUeAgentQueryPublicProjectionStrategy::SnapshotRecordArray:
			Root = PublicSnapshotRecordArrayJson(
				*Snapshot,
				PublicProjection, OutError);
			if (!OutError.IsEmpty()) return false;
			break;
		default:
			SetFailure(
				OutError,
				TEXT("query_response_invalid"),
				CommandId,
				TEXT("The public Query result kind is unsupported."));
			return false;
		}
		FString Text;
		if (!SerializeJson(Root, Text)
			|| !ChargeCodec(
				Resources,
				EUeAgentResourceDimension::OutputBytes,
				UE::UAI::ContextResource::MeasureText(Text).Utf8Bytes,
				TEXT("c3q.response.public.encode"),
				OutError))
		{
			if (OutError.IsEmpty())
			{
				SetFailure(
					OutError,
					TEXT("query_response_encode_failed"),
					TEXT("response"),
					TEXT("Failed to serialize the public Query response."));
			}
			return false;
		}
		OutValue = FUeAgentContractValue(MoveTemp(Text));
		return true;
	}

	FUeAgentQueryBinding::FUeAgentQueryBinding(
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		FString InRegistryHash,
		TSharedRef<const UE::UAI::Discovery::FUeAgentDiscoveryService> InDiscovery,
		TSharedRef<const UE::UAI::Projection::FUeAgentResponseProjector> InProjector,
		TSharedRef<IUeAgentQuerySessionSource> InSessionSource,
		TSharedRef<const FUeAgentQueryTicketService> InTicketService)
		: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, RegistryHash(MoveTemp(InRegistryHash))
		, Discovery(MoveTemp(InDiscovery))
		, Projector(MoveTemp(InProjector))
		, SessionSource(MoveTemp(InSessionSource))
		, TicketService(MoveTemp(InTicketService))
	{
	}

	FUeAgentQueryBinding::FUeAgentQueryBinding(
		TSharedRef<
			const UE::UAI::Contract::FUeAgentDescriptorRegistry>
			InDescriptorRegistry,
		TSharedRef<const FUeAgentDomainQueryProviderRegistry>
			InDomainProviders,
		TSharedRef<const UE::UAI::Discovery::FUeAgentDiscoveryService>
			InDiscovery,
		TSharedRef<const UE::UAI::Projection::FUeAgentResponseProjector>
			InProjector,
		TSharedRef<IUeAgentQuerySessionSource> InSessionSource,
		TSharedRef<const FUeAgentQueryTicketService> InTicketService)
		: DescriptorIdentity(
			InDescriptorRegistry->GetDescriptors()[0].GetIdentity())
		, RegistryHash(InDescriptorRegistry->GetRegistryHash())
		, DescriptorRegistry(MoveTemp(InDescriptorRegistry))
		, DomainProviders(MoveTemp(InDomainProviders))
		, Discovery(MoveTemp(InDiscovery))
		, Projector(MoveTemp(InProjector))
		, SessionSource(MoveTemp(InSessionSource))
		, TicketService(MoveTemp(InTicketService))
	{
	}

	bool FUeAgentQueryBinding::TryCreate(
		FUeAgentDescriptorIdentity DescriptorIdentity,
		FString RegistryHash,
		TSharedRef<const UE::UAI::Discovery::FUeAgentDiscoveryService> Discovery,
		TSharedRef<const UE::UAI::Projection::FUeAgentResponseProjector> Projector,
		TSharedRef<IUeAgentQuerySessionSource> SessionSource,
		TSharedRef<const FUeAgentQueryTicketService> TicketService,
		TSharedPtr<const FUeAgentQueryBinding>& OutBinding,
		FUeAgentQueryBindingError& OutError)
	{
		OutBinding.Reset();
		OutError = {};
		RegistryHash.TrimStartAndEndInline();
		if (!DescriptorIdentity.IsValid())
		{
			OutError = DescriptorIdentity.GetValidationDiagnostic();
			return false;
		}
		if (!RegistryHash.StartsWith(TEXT("sha256:")))
		{
			SetFailure(OutError, TEXT("query_binding_invalid"), DescriptorIdentity.GetDescriptorId(), TEXT("The registry hash must start with sha256:."), TEXT("registry_hash"));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(RegistryHash);
			return false;
		}
		OutBinding = MakeShareable(
			new FUeAgentQueryBinding(
				MoveTemp(DescriptorIdentity),
				MoveTemp(RegistryHash),
				MoveTemp(Discovery),
				MoveTemp(Projector),
				MoveTemp(SessionSource),
				MoveTemp(TicketService)));
		return true;
	}

	bool FUeAgentQueryBinding::TryCreate(
		TSharedRef<
			const UE::UAI::Contract::FUeAgentDescriptorRegistry>
			DescriptorRegistry,
		TSharedRef<const FUeAgentDomainQueryProviderRegistry>
			DomainProviders,
		TSharedRef<const UE::UAI::Discovery::FUeAgentDiscoveryService>
			Discovery,
		TSharedRef<const UE::UAI::Projection::FUeAgentResponseProjector>
			Projector,
		TSharedRef<IUeAgentQuerySessionSource> SessionSource,
		TSharedRef<const FUeAgentQueryTicketService> TicketService,
		TSharedPtr<const FUeAgentQueryBinding>& OutBinding,
		FUeAgentQueryBindingError& OutError)
	{
		OutBinding.Reset();
		OutError = {};
		if (DescriptorRegistry->GetDescriptors().IsEmpty())
		{
			SetFailure(OutError, TEXT("query_binding_invalid"), TEXT("descriptor_registry"), TEXT("The query binding requires at least one compiled descriptor."), TEXT("descriptors"));
			OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
			return false;
		}
		if (!DescriptorRegistry->GetRegistryHash().StartsWith(TEXT("sha256:")))
		{
			SetFailure(OutError, TEXT("query_binding_invalid"), TEXT("descriptor_registry"), TEXT("The compiled registry hash must start with sha256:."), TEXT("registry_hash"));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(DescriptorRegistry->GetRegistryHash());
			return false;
		}
		OutBinding = MakeShareable(new FUeAgentQueryBinding(
			MoveTemp(DescriptorRegistry),
			MoveTemp(DomainProviders),
			MoveTemp(Discovery),
			MoveTemp(Projector),
			MoveTemp(SessionSource),
			MoveTemp(TicketService)));
		return true;
	}

	bool FUeAgentQueryBinding::TrySubmit(
		const FUeAgentQueryBindingRequest& Request,
		const FUeAgentCallerContext& CallerContext,
		TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe> Resources,
		FUeAgentOperationDeadline Deadline,
		FUeAgentCancellationToken Cancellation,
		TSharedPtr<const FUeAgentQueryBindingResult>& OutResult,
		FUeAgentQueryBindingError& OutError) const
	{
		if (!Request.IsValid()
			|| Request.GetPayload().GetKind()
				!= EUeAgentQueryBindingPayloadKind::DomainQuery)
		{
			return TrySubmit(
				Request,
				CallerContext,
				Resources.Get(),
				Deadline,
				Cancellation,
				OutResult,
				OutError);
		}

		OutResult.Reset();
		OutError = {};
		const FUeAgentDomainQuery& Query =
			*Request.GetPayload().GetDomainQuery();
		if (!DescriptorRegistry.IsValid()
			|| !DomainProviders.IsValid())
		{
			SetFailure(
				OutError,
				TEXT("query_domain_binding_unavailable"),
				Request.GetRequestId(),
				TEXT(
					"Domain query requires descriptor and provider registries."));
			return false;
		}
		if (!ValidateRequest(Request, CallerContext, OutError))
		{
			return false;
		}

		FUeAgentCancellationRequested CancellationError;
		if (!Cancellation.TryCheckpoint(CancellationError))
		{
			OutError = CancellationError;
			OutError.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(CancellationError);
			OutError.Code = TEXT("query_cancelled");
			OutError.SubjectId = Request.GetRequestId();
			return false;
		}

		using UE::UAI::Contract::
			EUeAgentDescriptorCompatibility;
		const EUeAgentDescriptorCompatibility Compatibility =
			DescriptorRegistry->CheckCompatibility(
				Request.GetDescriptorIdentity());
		if (Compatibility
			!= EUeAgentDescriptorCompatibility::Exact)
		{
			const FString Code =
				Compatibility
						== EUeAgentDescriptorCompatibility::
							HashIncompatible
					? TEXT("descriptor_hash_mismatch")
					: Compatibility
							== EUeAgentDescriptorCompatibility::
								VersionIncompatible
						? TEXT("descriptor_version_mismatch")
						: TEXT("descriptor_not_found");
			SetFailure(
				OutError,
				Code,
				Request.GetDescriptorIdentity().GetDescriptorId(),
				TEXT(
					"Request descriptor does not exactly match the registry."));
			OutError.PropertyPath = TEXT("descriptor");
			OutError.ActualEvidence = MakeShared<FJsonValueObject>(Ticket::FUeAgentBindingTicketJsonCodec::DescriptorToJson(Request.GetDescriptorIdentity()));
			return false;
		}
		const TOptional<UE::UAI::Contract::FUeAgentQueryDescriptor>
			Descriptor =
				DescriptorRegistry->FindQueryExact(
					Request.GetDescriptorIdentity());
		if (!Descriptor.IsSet())
		{
			SetFailure(
				OutError,
				TEXT("descriptor_query_required"),
				Request.GetDescriptorIdentity().GetDescriptorId(),
				TEXT(
					"Domain query descriptor is not a query."));
			return false;
		}

		const FUeAgentQuerySessionIdentity CurrentSession =
			SessionSource->GetCurrent();
		const FUeAgentQuerySessionIdentity& RequestedSession =
			Query.GetSession();
		if (!CurrentSession.IsValid())
		{
			OutError = CurrentSession.GetValidationDiagnostic();
			return false;
		}
		if (CurrentSession.GetRegistryHash() != RegistryHash
			|| RequestedSession.GetRegistryHash()
				!= CurrentSession.GetRegistryHash())
		{
			SetFailure(
				OutError,
				TEXT("registry_hash_mismatch"),
				Request.GetRequestId(),
				TEXT(
					"Domain query registry hash is stale."));
			OutError.PropertyPath = TEXT("session.registry_hash");
			OutError.ActualEvidence = MakeShared<FJsonValueString>(CurrentSession.GetRegistryHash() != RegistryHash ? CurrentSession.GetRegistryHash() : RequestedSession.GetRegistryHash());
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(CurrentSession.GetRegistryHash() != RegistryHash ? RegistryHash : CurrentSession.GetRegistryHash());
			return false;
		}
		if (RequestedSession.GetServerSessionId()
			!= CurrentSession.GetServerSessionId())
		{
			SetFailure(
				OutError,
				TEXT("server_session_stale"),
				Request.GetRequestId(),
				TEXT(
					"Domain query belongs to another server session."));
			OutError.PropertyPath = TEXT("session.server_session_id");
			OutError.ActualEvidence = MakeShared<FJsonValueString>(RequestedSession.GetServerSessionId());
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(CurrentSession.GetServerSessionId());
			return false;
		}
		if (RequestedSession.GetRegistryEpoch()
			!= CurrentSession.GetRegistryEpoch())
		{
			SetFailure(
				OutError,
				TEXT("registry_epoch_stale"),
				Request.GetRequestId(),
				TEXT("Domain query registry epoch is stale."));
			OutError.PropertyPath = TEXT("session.registry_epoch");
			OutError.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), RequestedSession.GetRegistryEpoch()));
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), CurrentSession.GetRegistryEpoch()));
			return false;
		}

		TOptional<FUeAgentContractValue> CanonicalInput;
		UE::UAI::Contract::FUeAgentContractSchemaError SchemaError;
		auto SchemaCheckpoint =
			[&Resources, &Deadline, &Cancellation](
				const int64 WorkUnits,
				const FString& InstancePath,
				UE::UAI::Contract::
					FUeAgentContractSchemaError& Error)
			{
				FUeAgentCancellationRequested Cancelled;
				if (!Cancellation.TryCheckpoint(Cancelled))
				{
					Error = Cancelled;
					Error.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(Cancelled);
					Error.Code = TEXT("query_cancelled");
					Error.Source.JsonPath = InstancePath;
					return false;
				}
				FUeAgentResourceChargeError ChargeError;
				if (!Resources->TryCharge(
						EUeAgentResourceDimension::WorkUnits,
						WorkUnits,
						TEXT("c3q.domain.schema"),
						ChargeError))
				{
					Error = ChargeError;
					Error.Source.JsonPath = InstancePath;
					return false;
				}
				return true;
			};
		if (!Descriptor->GetCompiledInputSchema()
				.TryValidateAndCanonicalize(
					Query.GetInput(),
					MoveTemp(SchemaCheckpoint),
					CanonicalInput,
					SchemaError))
		{
			OutError = SchemaError;
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SchemaError.Source.JsonPath;
			return false;
		}

		UE::UAI::ContextResource::
			FUeAgentExecutionContextRefPtr ExecutionContext;
		UE::UAI::ContextResource::
			FUeAgentExecutionContextError ExecutionError;
		if (!UE::UAI::ContextResource::
				FUeAgentExecutionContextRef::TryCreate(
					Query.GetExecutionContext(),
					Descriptor->GetIdentity(),
					UE::UAI::ContextResource::
						EUeAgentRequestKind::Query,
					Descriptor->GetExecutionContextPolicy(),
					ExecutionContext,
					ExecutionError))
		{
			OutError = ExecutionError;
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetRequestId();
			return false;
		}

		TOptional<FUeAgentCandidateBinding> Target;
		if (Query.GetTarget().IsSet())
		{
			const FUeAgentDomainQueryTarget& DomainTarget =
				Query.GetTarget().GetValue();
			if (!TicketService->TryValidate(
					DomainTarget.GetTicket(),
					DomainTarget.GetBinding(),
					Descriptor->GetIdentity(),
					CurrentSession,
					CallerContext,
					OutError))
			{
				return false;
			}
			Target = DomainTarget.GetBinding();
		}

		const TSharedPtr<IUeAgentDomainQueryProvider> Provider =
			DomainProviders->FindExact(
				Descriptor->GetProviderBinding());
		if (!Provider.IsValid())
		{
			SetFailure(
				OutError,
				TEXT("query_provider_not_found"),
				Descriptor->GetProviderBinding(),
				TEXT(
					"No provider is registered for the exact binding."));
			return false;
		}

		const UE::UAI::Contract::FUeAgentQueryRequest
			OperationRequest(
				Descriptor->GetIdentity(),
				Request.GetRequestId(),
				CanonicalInput.GetValue());
		TSharedPtr<UE::UAI::ContextResource::
			FUeAgentOperationContext> OperationContext;
		UE::UAI::ContextResource::
			FUeAgentOperationContextError OperationContextError;
		if (!UE::UAI::ContextResource::
				FUeAgentOperationContext::TryCreate(
					OperationRequest,
					CallerContext,
					ExecutionContext.ToSharedRef(),
					Resources,
					Deadline,
					Cancellation,
					OperationContext,
					OperationContextError))
		{
			OutError = OperationContextError;
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetRequestId();
			return false;
		}

		TSharedPtr<const FUeAgentProjectionSourceValue> Source;
		if (!Provider->TryQuery(
				Descriptor.GetValue(),
				CanonicalInput.GetValue(),
				Target,
				*OperationContext,
				Source,
				OutError))
		{
			if (OutError.IsEmpty()) SetFailure(OutError, TEXT("query_provider_failed"), Descriptor->GetProviderBinding(), TEXT("The query provider returned failure without a diagnostic."));
			return false;
		}
		if (!Source.IsValid() || !Source->IsValid())
		{
			SetFailure(
				OutError,
				TEXT("query_provider_result_missing"),
				Descriptor->GetProviderBinding(),
				Source.IsValid() ? TEXT("The provider projection source must contain exactly one typed value.") : TEXT("The provider succeeded without returning a projection source."), TEXT("projection.source"));
			return false;
		}

		const FUeAgentProjectionQueryResult ProjectionResult(
			Descriptor->GetIdentity(),
			Request.GetRequestId(),
			*Source);
		TSharedPtr<const FUeAgentProjectedResponse> Projected;
		FUeAgentProjectionError ProjectionError;
		if (!Projector->TryProject(
				ProjectionResult,
				Query.GetProjection(),
				Resources.Get(),
				Deadline,
				Cancellation,
				Projected,
				ProjectionError))
		{
			ForwardDiagnostic(ProjectionError, OutError);
			return false;
		}
		if (!Projected.IsValid())
		{
			SetFailure(
				OutError,
				TEXT("query_projection_result_missing"),
				Request.GetRequestId(),
				TEXT(
					"Projection succeeded without a response."));
			return false;
		}
		FUeAgentQueryBindingResponse Response(
			Projected.ToSharedRef(),
			{},
			Source);
		if (!Response.IsValid())
		{
			OutError = Response.GetValidationDiagnostic();
			return false;
		}
		OutResult = MakeShared<FUeAgentQueryBindingResult>(
			Descriptor->GetIdentity(),
			Request.GetRequestId(),
			MoveTemp(Response));
		return true;
	}

	bool FUeAgentQueryBinding::TrySubmit(
		const FUeAgentQueryBindingRequest& Request,
		const FUeAgentCallerContext& CallerContext,
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const FUeAgentQueryBindingResult>& OutResult,
		FUeAgentQueryBindingError& OutError) const
	{
		OutResult.Reset();
		OutError = {};

		FUeAgentCancellationRequested CancellationError;
		if (!Cancellation.TryCheckpoint(CancellationError))
		{
			OutError = CancellationError;
			OutError.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(CancellationError);
			OutError.Code = TEXT("query_cancelled");
			OutError.SubjectId = Request.GetRequestId();
			return false;
		}
		if (!ValidateRequest(Request, CallerContext, OutError))
		{
			return false;
		}
		if (!(Request.GetDescriptorIdentity() == DescriptorIdentity))
		{
			SetFailure(
				OutError,
				TEXT("descriptor_hash_mismatch"),
				Request.GetDescriptorIdentity().GetDescriptorId(),
				TEXT("Request descriptor identity differs from the QueryBinding descriptor."));
			OutError.PropertyPath = TEXT("descriptor");
			OutError.ActualEvidence = MakeShared<FJsonValueObject>(Ticket::FUeAgentBindingTicketJsonCodec::DescriptorToJson(Request.GetDescriptorIdentity()));
			return false;
		}

		const FUeAgentQueryBindingPayload& Payload = Request.GetPayload();
		const FUeAgentQuerySessionIdentity* RequestedSession = nullptr;
		switch (Payload.GetKind())
		{
		case EUeAgentQueryBindingPayloadKind::CatalogSearch:
			RequestedSession = &Payload.GetCatalogSearch()->GetSession();
			break;
		case EUeAgentQueryBindingPayloadKind::CatalogResolve:
			RequestedSession = &Payload.GetCatalogResolve()->GetSession();
			break;
		case EUeAgentQueryBindingPayloadKind::StateRead:
			RequestedSession = &Payload.GetStateRead()->GetSession();
			break;
		default:
			SetFailure(OutError, TEXT("query_payload_invalid"), Request.GetRequestId(), TEXT("This query binding requires catalog search, catalog resolve or state read; domain queries require shared request resources."), TEXT("payload.kind"));
			OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Payload.GetKind()));
			return false;
		}
		const FUeAgentQuerySessionIdentity CurrentSession = SessionSource->GetCurrent();
		if (!CurrentSession.IsValid())
		{
			OutError = CurrentSession.GetValidationDiagnostic();
			return false;
		}
		if (CurrentSession.GetRegistryHash() != RegistryHash)
		{
			SetFailure(OutError, TEXT("registry_hash_mismatch"), Request.GetRequestId(), TEXT("Current registry hash differs from the QueryBinding composition hash."));
			OutError.PropertyPath = TEXT("session.registry_hash");
			OutError.ActualEvidence = MakeShared<FJsonValueString>(CurrentSession.GetRegistryHash());
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(RegistryHash);
			return false;
		}
		if (RequestedSession->GetServerSessionId() != CurrentSession.GetServerSessionId())
		{
			SetFailure(OutError, TEXT("server_session_stale"), Request.GetRequestId(), TEXT("Request belongs to another server session."));
			OutError.PropertyPath = TEXT("session.server_session_id");
			OutError.ActualEvidence = MakeShared<FJsonValueString>(RequestedSession->GetServerSessionId());
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(CurrentSession.GetServerSessionId());
			return false;
		}
		if (RequestedSession->GetRegistryEpoch() != CurrentSession.GetRegistryEpoch())
		{
			SetFailure(OutError, TEXT("registry_epoch_stale"), Request.GetRequestId(), TEXT("Request registry epoch is stale."));
			OutError.PropertyPath = TEXT("session.registry_epoch");
			OutError.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), RequestedSession->GetRegistryEpoch()));
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), CurrentSession.GetRegistryEpoch()));
			return false;
		}
		if (RequestedSession->GetRegistryHash() != CurrentSession.GetRegistryHash())
		{
			SetFailure(OutError, TEXT("registry_hash_mismatch"), Request.GetRequestId(), TEXT("Request registry hash is stale."));
			OutError.PropertyPath = TEXT("session.registry_hash");
			OutError.ActualEvidence = MakeShared<FJsonValueString>(RequestedSession->GetRegistryHash());
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(CurrentSession.GetRegistryHash());
			return false;
		}

		TSharedPtr<const FUeAgentProjectedResponse> Projected;
		TOptional<FUeAgentQueryTicket> Ticket;
		FUeAgentDiscoveryError DiscoveryError;
		FUeAgentProjectionError ProjectionError;

		switch (Payload.GetKind())
		{
		case EUeAgentQueryBindingPayloadKind::CatalogSearch:
		{
			const FUeAgentCatalogSearchQuery& Query = *Payload.GetCatalogSearch();
			TSharedPtr<const FUeAgentCatalogSnapshot> Catalog;
			if (!Discovery->TrySearch(
					Query.GetQuery(),
					Resources,
					Deadline,
					Cancellation,
					Catalog,
					DiscoveryError))
			{
				ForwardDiagnostic(DiscoveryError, OutError);
				return false;
			}
			if (!Catalog.IsValid())
			{
				SetFailure(OutError, TEXT("query_discovery_result_missing"), Request.GetRequestId(), TEXT("Discovery succeeded without a catalog."));
				return false;
			}
			const FUeAgentProjectionQueryResult ProjectionResult(
				DescriptorIdentity,
				Request.GetRequestId(),
				FUeAgentProjectionSourceValue(Catalog.ToSharedRef()));
			if (!Projector->TryProject(
					ProjectionResult,
					Query.GetProjection(),
					Resources,
					Deadline,
					Cancellation,
					Projected,
					ProjectionError))
			{
				ForwardDiagnostic(ProjectionError, OutError);
				return false;
			}
			break;
		}
		case EUeAgentQueryBindingPayloadKind::CatalogResolve:
		{
			const FUeAgentCatalogResolveQuery& Query = *Payload.GetCatalogResolve();
			TSharedPtr<const FUeAgentCandidateBinding> Binding;
			if (!Discovery->TryResolveExact(
					Query.GetCatalog().Get(),
					Query.GetCandidateId(),
					Resources,
					Deadline,
					Cancellation,
					Binding,
					DiscoveryError))
			{
				ForwardDiagnostic(DiscoveryError, OutError);
				return false;
			}
			if (!Binding.IsValid())
			{
				SetFailure(OutError, TEXT("query_discovery_result_missing"), Query.GetCandidateId(), TEXT("Exact resolve succeeded without a candidate binding."));
				return false;
			}
			if (!TicketService->TryIssue(
					*Binding,
					Binding->GetSummary()
						.GetDescriptorIdentity(),
					CurrentSession,
					CallerContext,
					{},
					Ticket,
					OutError))
			{
				return false;
			}
			const FUeAgentProjectionQueryResult ProjectionResult(
				DescriptorIdentity,
				Request.GetRequestId(),
				FUeAgentProjectionSourceValue(Binding.ToSharedRef()));
			if (!Projector->TryProject(
					ProjectionResult,
					Query.GetProjection(),
					Resources,
					Deadline,
					Cancellation,
					Projected,
					ProjectionError))
			{
				ForwardDiagnostic(ProjectionError, OutError);
				return false;
			}
			break;
		}
		case EUeAgentQueryBindingPayloadKind::StateRead:
		{
			const FUeAgentStateReadQuery& Query = *Payload.GetStateRead();
			if (!TicketService->TryValidate(
					Query.GetTicket(),
					Query.GetBinding(),
					Query.GetBinding()
						.GetSummary()
						.GetDescriptorIdentity(),
					CurrentSession,
					CallerContext,
					OutError))
			{
				return false;
			}
			TSharedPtr<const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot> Snapshot;
			if (!Discovery->TryRead(
					Query.GetBinding(),
					Query.GetStatePlane(),
					Resources,
					Deadline,
					Cancellation,
					Snapshot,
					DiscoveryError))
			{
				ForwardDiagnostic(DiscoveryError, OutError);
				return false;
			}
			if (!Snapshot.IsValid())
			{
				SetFailure(OutError, TEXT("query_discovery_result_missing"), Query.GetBinding().GetBindingId(), TEXT("State read succeeded without a snapshot."));
				return false;
			}
			if (!TicketService->TryIssue(
					Query.GetBinding(),
					Query.GetBinding()
						.GetSummary()
						.GetDescriptorIdentity(),
					CurrentSession,
					CallerContext,
					Snapshot->GetRevision(),
					Ticket,
					OutError))
			{
				return false;
			}
			const FUeAgentProjectionQueryResult ProjectionResult(
				DescriptorIdentity,
				Request.GetRequestId(),
				FUeAgentProjectionSourceValue(Snapshot.ToSharedRef()));
			if (!Projector->TryProject(
					ProjectionResult,
					Query.GetProjection(),
					Resources,
					Deadline,
					Cancellation,
					Projected,
					ProjectionError))
			{
				ForwardDiagnostic(ProjectionError, OutError);
				return false;
			}
			break;
		}
		default:
			SetFailure(OutError, TEXT("query_payload_invalid"), Request.GetRequestId(), TEXT("This query binding does not support the requested payload kind."), TEXT("payload.kind"));
			OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Payload.GetKind()));
			return false;
		}

		if (!Projected.IsValid())
		{
			SetFailure(OutError, TEXT("query_projection_result_missing"), Request.GetRequestId(), TEXT("Projection succeeded without a response."));
			return false;
		}
		FUeAgentQueryBindingResponse Response(Projected.ToSharedRef(), MoveTemp(Ticket));
		if (!Response.IsValid())
		{
			OutError = Response.GetValidationDiagnostic();
			return false;
		}
		OutResult = MakeShared<FUeAgentQueryBindingResult>(
			DescriptorIdentity,
			Request.GetRequestId(),
			MoveTemp(Response));
		return true;
	}
}
