#include "Core/BindingGateway/Query/UeAgentQueryBinding.h"

#include "Core/BindingGateway/Ticket/UeAgentBindingTicketJson.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::BindingGateway::Query
{
	namespace
	{
		using UE::UAI::ContextResource::FUeAgentCancellationRequested;
		using UE::UAI::ContextResource::FUeAgentOperationDeadlineExceeded;
		using UE::UAI::ContextResource::FUeAgentResourceBudgetExceeded;
		using UE::UAI::Discovery::EUeAgentCandidateOperation;
		using UE::UAI::Discovery::EUeAgentCatalogCompleteness;
		using UE::UAI::Discovery::EUeAgentDiscoveryMatchMode;
		using UE::UAI::Discovery::EUeAgentMaterializationPolicy;
		using UE::UAI::Discovery::FUeAgentDiscoveryError;
		using UE::UAI::Projection::FUeAgentProjectionError;
		using UE::UAI::Projection::FUeAgentProjectionQueryResult;
		using UE::UAI::Projection::FUeAgentProjectionSourceValue;

		void SetError(
			FUeAgentQueryBindingError& OutError,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			OutError = {};
			OutError.Code = Code;
			OutError.SubjectId = SubjectId;
			OutError.Detail = Detail;
		}

		void CopyError(
			const FUeAgentDiscoveryError& Source,
			FUeAgentQueryBindingError& OutError)
		{
			OutError.Code = Source.Code;
			OutError.SubjectId = Source.SubjectId;
			OutError.Detail = Source.Detail;
			OutError.ResourceDimension = Source.ResourceDimension;
			OutError.ResourceLimit = Source.ResourceLimit;
			OutError.ResourceObserved = Source.ResourceObserved;
		}

		void CopyError(
			const FUeAgentProjectionError& Source,
			FUeAgentQueryBindingError& OutError)
		{
			OutError.Code = Source.Code;
			OutError.SubjectId = Source.SubjectId;
			OutError.Detail = Source.Detail;
			OutError.ResourceDimension = Source.ResourceDimension;
			OutError.ResourceLimit = Source.ResourceLimit;
			OutError.ResourceObserved = Source.ResourceObserved;
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
		bValid = Session.IsValid() && Query.IsValid() && Projection.IsValid();
	}

	const FUeAgentQuerySessionIdentity& FUeAgentCatalogSearchQuery::GetSession() const { return Session; }
	const FUeAgentDiscoveryQuery& FUeAgentCatalogSearchQuery::GetQuery() const { return Query; }
	const FUeAgentProjectionRequest& FUeAgentCatalogSearchQuery::GetProjection() const { return Projection; }
	bool FUeAgentCatalogSearchQuery::IsValid() const { return bValid; }

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
		bValid = Session.IsValid()
			&& Catalog->IsValid()
			&& !CandidateId.IsEmpty()
			&& Projection.IsValid();
	}

	const FUeAgentQuerySessionIdentity& FUeAgentCatalogResolveQuery::GetSession() const { return Session; }
	const TSharedRef<const FUeAgentCatalogSnapshot>& FUeAgentCatalogResolveQuery::GetCatalog() const { return Catalog; }
	const FString& FUeAgentCatalogResolveQuery::GetCandidateId() const { return CandidateId; }
	const FUeAgentProjectionRequest& FUeAgentCatalogResolveQuery::GetProjection() const { return Projection; }
	bool FUeAgentCatalogResolveQuery::IsValid() const { return bValid; }

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
		bValid = Session.IsValid()
			&& Ticket.IsValid()
			&& Binding.IsValid()
			&& Projection.IsValid();
	}

	const FUeAgentQuerySessionIdentity& FUeAgentStateReadQuery::GetSession() const { return Session; }
	const FUeAgentQueryTicket& FUeAgentStateReadQuery::GetTicket() const { return Ticket; }
	const FUeAgentCandidateBinding& FUeAgentStateReadQuery::GetBinding() const { return Binding; }
	EUeAgentStatePlane FUeAgentStateReadQuery::GetStatePlane() const { return StatePlane; }
	const FUeAgentProjectionRequest& FUeAgentStateReadQuery::GetProjection() const { return Projection; }
	bool FUeAgentStateReadQuery::IsValid() const { return bValid; }

	FUeAgentQueryBindingPayload::FUeAgentQueryBindingPayload(
		FUeAgentCatalogSearchQuery InQuery)
		: Kind(EUeAgentQueryBindingPayloadKind::CatalogSearch)
		, CatalogSearch(MakeShared<FUeAgentCatalogSearchQuery>(MoveTemp(InQuery)))
	{
	}

	FUeAgentQueryBindingPayload::FUeAgentQueryBindingPayload(
		FUeAgentCatalogResolveQuery InQuery)
		: Kind(EUeAgentQueryBindingPayloadKind::CatalogResolve)
		, CatalogResolve(MakeShared<FUeAgentCatalogResolveQuery>(MoveTemp(InQuery)))
	{
	}

	FUeAgentQueryBindingPayload::FUeAgentQueryBindingPayload(
		FUeAgentStateReadQuery InQuery)
		: Kind(EUeAgentQueryBindingPayloadKind::StateRead)
		, StateRead(MakeShared<FUeAgentStateReadQuery>(MoveTemp(InQuery)))
	{
	}

	FUeAgentQueryBindingPayload::FUeAgentQueryBindingPayload(
		FUeAgentDomainQuery InQuery)
		: Kind(EUeAgentQueryBindingPayloadKind::DomainQuery)
		, DomainQuery(
			MakeShared<FUeAgentDomainQuery>(MoveTemp(InQuery)))
	{
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
		switch (Kind)
		{
		case EUeAgentQueryBindingPayloadKind::CatalogSearch:
			return CatalogSearch.IsValid() && CatalogSearch->IsValid()
				&& !CatalogResolve.IsValid() && !StateRead.IsValid();
		case EUeAgentQueryBindingPayloadKind::CatalogResolve:
			return CatalogResolve.IsValid() && CatalogResolve->IsValid()
				&& !CatalogSearch.IsValid() && !StateRead.IsValid();
		case EUeAgentQueryBindingPayloadKind::StateRead:
			return StateRead.IsValid() && StateRead->IsValid()
				&& !CatalogSearch.IsValid() && !CatalogResolve.IsValid()
				&& !DomainQuery.IsValid();
		case EUeAgentQueryBindingPayloadKind::DomainQuery:
			return DomainQuery.IsValid() && DomainQuery->IsValid()
				&& !CatalogSearch.IsValid()
				&& !CatalogResolve.IsValid()
				&& !StateRead.IsValid();
		default:
			return false;
		}
	}

	FUeAgentQueryBindingResponse::FUeAgentQueryBindingResponse(
		TSharedRef<const FUeAgentProjectedResponse> InProjection,
		TOptional<FUeAgentQueryTicket> InTicket,
		TSharedPtr<const FUeAgentProjectionSourceValue> InSource)
		: Projection(MoveTemp(InProjection))
		, Ticket(MoveTemp(InTicket))
		, Source(MoveTemp(InSource))
	{
		bValid = Projection.IsValid()
			&& Projection->IsValid()
			&& (!Source.IsValid() || Source->IsValid())
			&& (!Ticket.IsSet() || Ticket->IsValid());
	}

	const TSharedPtr<const FUeAgentProjectedResponse>& FUeAgentQueryBindingResponse::GetProjection() const { return Projection; }
	const TOptional<FUeAgentQueryTicket>& FUeAgentQueryBindingResponse::GetTicket() const { return Ticket; }
	const TSharedPtr<const FUeAgentProjectionSourceValue>&
	FUeAgentQueryBindingResponse::GetSource() const { return Source; }
	bool FUeAgentQueryBindingResponse::IsValid() const { return bValid; }

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
			SetError(
				OutError,
				Error.Code,
				Error.SubjectId,
				Error.Detail);
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
			SetError(
				OutError,
				Error.Code,
				Error.SubjectId,
				Error.Detail);
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
			SetError(
				OutError,
				Error.Code,
				Error.SubjectId,
				Error.Detail);
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
			const FUeAgentCatalogSnapshot& Catalog)
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

		TSharedRef<FJsonObject> ProjectionJson(
			const FUeAgentProjectionRequest& Projection)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("profile_id"), Projection.GetProfile().GetProfileId());
			Object->SetBoolField(TEXT("include_defaults"), Projection.GetProfile().IncludesDefaults());
			Object->SetBoolField(TEXT("include_details"), Projection.GetProfile().IncludesDetails());
			Object->SetBoolField(TEXT("include_provenance"), Projection.GetProfile().IncludesProvenance());
			if (Projection.GetPage().HasMaxItems())
			{
				Object->SetNumberField(
					TEXT("max_items"),
					Projection.GetPage().GetMaxItems());
			}
			else
			{
				Object->SetField(
					TEXT("max_items"),
					MakeShared<FJsonValueNull>());
			}
			Object->SetStringField(TEXT("continuation_token"), Projection.GetPage().GetCursor());
			if (Projection.GetTokenBudget().IsSet())
			{
				Object->SetNumberField(TEXT("token_budget"), static_cast<double>(Projection.GetTokenBudget().GetValue()));
			}
			else
			{
				Object->SetField(TEXT("token_budget"), MakeShared<FJsonValueNull>());
			}
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
			FUeAgentResourceBudgetExceeded Error;
			if (Resources.TryCharge(Dimension, Amount, Path, Error))
			{
				return true;
			}
			OutError.Code = Error.Code;
			OutError.SubjectId = Error.Path;
			OutError.Detail = TEXT("C3Q payload codec resource budget exceeded.");
			OutError.ResourceDimension = Error.Dimension;
			OutError.ResourceLimit = Error.Limit;
			OutError.ResourceObserved = Error.Observed;
			return false;
		}

		bool ParseSession(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<FUeAgentQuerySessionIdentity>& Out,
			FUeAgentQueryBindingError& OutError)
		{
			UE::UAI::BindingGateway::Ticket::
				FUeAgentBindingTicketJsonError CodecError;
			if (!UE::UAI::BindingGateway::Ticket::
					FUeAgentBindingTicketJsonCodec::TryParseSession(
						Object,
						Out,
						CodecError))
			{
				SetError(
					OutError,
					CodecError.Code.IsEmpty()
						? TEXT("query_payload_invalid")
						: CodecError.Code,
					CodecError.SubjectId,
					CodecError.Detail);
				return false;
			}
			return true;
		}

		bool ParseProjection(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<FUeAgentProjectionRequest>& Out,
			FUeAgentQueryBindingError& OutError)
		{
			if (!Object.IsValid()
				|| !Object->HasTypedField<EJson::String>(TEXT("profile_id"))
				|| !Object->HasTypedField<EJson::Boolean>(TEXT("include_defaults"))
				|| !Object->HasTypedField<EJson::Boolean>(TEXT("include_details"))
				|| !Object->HasTypedField<EJson::Boolean>(TEXT("include_provenance"))
				|| !Object->HasField(TEXT("max_items")))
			{
				SetError(OutError, TEXT("query_payload_invalid"), TEXT("projection"), TEXT("Projection fields are missing or have the wrong type."));
				return false;
			}
			TOptional<int64> TokenBudget;
			double TokenBudgetNumber = 0.0;
			if (Object->TryGetNumberField(TEXT("token_budget"), TokenBudgetNumber))
			{
				TokenBudget = static_cast<int64>(TokenBudgetNumber);
			}
			FString Continuation;
			Object->TryGetStringField(TEXT("continuation_token"), Continuation);
			const TSharedPtr<FJsonValue>& MaxItemsValue =
				Object->Values.FindChecked(TEXT("max_items"));
			UE::UAI::Projection::FUeAgentPageRequest Page;
			if (MaxItemsValue->Type == EJson::Number)
			{
				Page = UE::UAI::Projection::FUeAgentPageRequest(
					static_cast<int32>(
						MaxItemsValue->AsNumber()),
					Continuation);
			}
			else if (MaxItemsValue->Type == EJson::Null)
			{
				Page = Continuation.IsEmpty()
					? UE::UAI::Projection::FUeAgentPageRequest()
					: UE::UAI::Projection::FUeAgentPageRequest(
						Continuation);
			}
			else
			{
				SetError(
					OutError,
					TEXT("query_payload_invalid"),
					TEXT("projection.max_items"),
					TEXT("max_items must be a number or null."));
				return false;
			}
			FUeAgentProjectionRequest Projection(
				UE::UAI::Projection::FUeAgentProjectionProfile(
					Object->GetStringField(TEXT("profile_id")),
					Object->GetBoolField(TEXT("include_defaults")),
					Object->GetBoolField(TEXT("include_details")),
					Object->GetBoolField(TEXT("include_provenance"))),
				MoveTemp(Page),
				TokenBudget,
				{},
				{});
			if (!Projection.IsValid())
			{
				SetError(OutError, TEXT("query_payload_invalid"), TEXT("projection"), TEXT("Projection request is invalid."));
				return false;
			}
			Out.Emplace(MoveTemp(Projection));
			return true;
		}

		bool ParseDescriptor(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<FUeAgentDescriptorIdentity>& Out)
		{
			if (!Object.IsValid()
				|| !Object->HasTypedField<EJson::String>(TEXT("descriptor_id"))
				|| !Object->HasTypedField<EJson::Number>(TEXT("version"))
				|| !Object->HasTypedField<EJson::String>(TEXT("descriptor_hash")))
			{
				return false;
			}
			FUeAgentDescriptorIdentity Value(
				Object->GetStringField(TEXT("descriptor_id")),
				static_cast<int32>(Object->GetNumberField(TEXT("version"))),
				Object->GetStringField(TEXT("descriptor_hash")));
			if (!Value.IsValid())
			{
				return false;
			}
			Out.Emplace(MoveTemp(Value));
			return true;
		}

		bool ParseIdentity(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<UE::UAI::StateSemantic::FUeAgentCanonicalIdentity>& Out)
		{
			if (!Object.IsValid())
			{
				return false;
			}
			for (const TCHAR* Field : {
				TEXT("semantic_id"),
				TEXT("target_kind"),
				TEXT("owner_id"),
				TEXT("selector"),
				TEXT("object_role"),
				TEXT("generation"),
			})
			{
				if (!Object->HasTypedField<EJson::String>(Field))
				{
					return false;
				}
			}
			UE::UAI::StateSemantic::FUeAgentCanonicalIdentity Value(
				Object->GetStringField(TEXT("semantic_id")),
				Object->GetStringField(TEXT("target_kind")),
				Object->GetStringField(TEXT("owner_id")),
				Object->GetStringField(TEXT("selector")),
				Object->GetStringField(TEXT("object_role")),
				Object->GetStringField(TEXT("generation")));
			if (!Value.IsValid())
			{
				return false;
			}
			Out.Emplace(MoveTemp(Value));
			return true;
		}

		bool ParseCatalogRevision(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<FUeAgentCatalogRevision>& Out)
		{
			if (!Object.IsValid()
				|| !Object->HasTypedField<EJson::String>(TEXT("source_id"))
				|| !Object->HasTypedField<EJson::String>(TEXT("generation"))
				|| !Object->HasTypedField<EJson::Number>(TEXT("sequence")))
			{
				return false;
			}
			FUeAgentCatalogRevision Value(
				Object->GetStringField(TEXT("source_id")),
				Object->GetStringField(TEXT("generation")),
				static_cast<int64>(Object->GetNumberField(TEXT("sequence"))));
			if (!Value.IsValid())
			{
				return false;
			}
			Out.Emplace(MoveTemp(Value));
			return true;
		}

		bool ParseSummary(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<UE::UAI::Discovery::FUeAgentCandidateSummary>& Out)
		{
			if (!Object.IsValid()
				|| !Object->HasTypedField<EJson::Object>(TEXT("identity"))
				|| !Object->HasTypedField<EJson::Object>(TEXT("descriptor"))
				|| !Object->HasTypedField<EJson::String>(TEXT("catalog_kind"))
				|| !Object->HasTypedField<EJson::String>(TEXT("display_name"))
				|| !Object->HasTypedField<EJson::String>(TEXT("summary"))
				|| !Object->HasTypedField<EJson::Array>(TEXT("category_path")))
			{
				return false;
			}
			TOptional<UE::UAI::StateSemantic::FUeAgentCanonicalIdentity> Identity;
			TOptional<FUeAgentDescriptorIdentity> Descriptor;
			if (!ParseIdentity(Object->GetObjectField(TEXT("identity")), Identity)
				|| !ParseDescriptor(Object->GetObjectField(TEXT("descriptor")), Descriptor))
			{
				return false;
			}
			TArray<FString> CategoryPath;
			for (const TSharedPtr<FJsonValue>& Item : Object->GetArrayField(TEXT("category_path")))
			{
				if (!Item.IsValid() || Item->Type != EJson::String)
				{
					return false;
				}
				CategoryPath.Add(Item->AsString());
			}
			UE::UAI::Discovery::FUeAgentCandidateSummary Value(
				Identity.GetValue(),
				Descriptor.GetValue(),
				Object->GetStringField(TEXT("catalog_kind")),
				Object->GetStringField(TEXT("display_name")),
				Object->GetStringField(TEXT("summary")),
				MoveTemp(CategoryPath));
			if (!Value.IsValid())
			{
				return false;
			}
			Out.Emplace(MoveTemp(Value));
			return true;
		}

		bool ParseCatalog(
			const TSharedPtr<FJsonObject>& Object,
			FUeAgentResourceTracker& Resources,
			TSharedPtr<const FUeAgentCatalogSnapshot>& Out,
			FUeAgentQueryBindingError& OutError)
		{
			if (!Object.IsValid()
				|| !Object->HasTypedField<EJson::String>(TEXT("catalog_id"))
				|| !Object->HasTypedField<EJson::String>(TEXT("catalog_kind"))
				|| !Object->HasTypedField<EJson::Object>(TEXT("revision"))
				|| !Object->HasTypedField<EJson::String>(TEXT("completeness"))
				|| !Object->HasTypedField<EJson::Array>(TEXT("candidates")))
			{
				return false;
			}
			TOptional<FUeAgentCatalogRevision> Revision;
			if (!ParseCatalogRevision(Object->GetObjectField(TEXT("revision")), Revision))
			{
				return false;
			}
			TArray<UE::UAI::Discovery::FUeAgentCandidateSummary> Candidates;
			for (const TSharedPtr<FJsonValue>& Item : Object->GetArrayField(TEXT("candidates")))
			{
				TOptional<UE::UAI::Discovery::FUeAgentCandidateSummary> Summary;
				if (!Item.IsValid()
					|| Item->Type != EJson::Object
					|| !ParseSummary(Item->AsObject(), Summary))
				{
					return false;
				}
				Candidates.Add(Summary.GetValue());
			}
			const FString Completeness = Object->GetStringField(TEXT("completeness"));
			EUeAgentCatalogCompleteness TypedCompleteness;
			if (Completeness == TEXT("complete")) TypedCompleteness = EUeAgentCatalogCompleteness::Complete;
			else if (Completeness == TEXT("partial")) TypedCompleteness = EUeAgentCatalogCompleteness::Partial;
			else if (Completeness == TEXT("degraded")) TypedCompleteness = EUeAgentCatalogCompleteness::Degraded;
			else return false;
			FUeAgentDiscoveryError DiscoveryError;
			if (!FUeAgentCatalogSnapshot::TryCreate(
					Object->GetStringField(TEXT("catalog_id")),
					Object->GetStringField(TEXT("catalog_kind")),
					Revision.GetValue(),
					TypedCompleteness,
					Candidates,
					{},
					Resources,
					Out,
					DiscoveryError))
			{
				CopyError(DiscoveryError, OutError);
				return false;
			}
			return true;
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
				TEXT("value_kind"),
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
			const UE::UAI::Projection::FUeAgentProjectedSnapshot& Snapshot)
		{
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
			else
			{
				Object->SetField(
					TEXT("provenance"),
					MakeShared<FJsonValueNull>());
			}

			TArray<TSharedPtr<FJsonValue>> Entries;
			Entries.Reserve(Snapshot.GetEntries().Num());
			for (const auto& Entry : Snapshot.GetEntries())
			{
				TSharedRef<FJsonObject> EntryObject =
					MakeShared<FJsonObject>();
				EntryObject->SetObjectField(
					TEXT("identity"),
					IdentityJson(Entry.GetIdentity()));
				EntryObject->SetObjectField(
					TEXT("type"),
					TypeJson(Entry.GetType()));
				EntryObject->SetObjectField(
					TEXT("value"),
					CanonicalValueJson(Entry.GetValue().Get()));
				EntryObject->SetObjectField(
					TEXT("observed_revision"),
					RevisionJson(Entry.GetObservedRevision()));
				if (Entry.GetDefault().IsSet())
				{
					EntryObject->SetObjectField(
						TEXT("default"),
						DefaultJson(Entry.GetDefault().GetValue()));
				}
				else
				{
					EntryObject->SetField(
						TEXT("default"),
						MakeShared<FJsonValueNull>());
				}
				if (Entry.GetDetails().IsSet())
				{
					EntryObject->SetObjectField(
						TEXT("details"),
						DetailsJson(Entry.GetDetails().GetValue()));
				}
				else
				{
					EntryObject->SetField(
						TEXT("details"),
						MakeShared<FJsonValueNull>());
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
			PageObject->SetBoolField(TEXT("has_more"), Page.HasMore());
			PageObject->SetStringField(
				TEXT("next_cursor"),
				Page.GetNextCursor());
			Object->SetObjectField(TEXT("page"), PageObject);
			Object->SetArrayField(
				TEXT("omitted_sections"),
				StringArrayJson(Snapshot.GetOmittedSections()));
			return Object;
		}

		bool ParseStatePlane(
			const FString& Value,
			EUeAgentStatePlane& Out)
		{
			return UE::UAI::Contract::TryParseStatePlane(
				Value,
				Out);
		}

		FString ExecutionWorldTypeName(
			const UE::UAI::ContextResource::
				EUeAgentExecutionWorldType WorldType)
		{
			using UE::UAI::ContextResource::
				EUeAgentExecutionWorldType;
			switch (WorldType)
			{
			case EUeAgentExecutionWorldType::Editor:
				return TEXT("editor");
			case EUeAgentExecutionWorldType::Pie:
				return TEXT("pie");
			case EUeAgentExecutionWorldType::SimulateInEditor:
				return TEXT("simulate_in_editor");
			case EUeAgentExecutionWorldType::Game:
				return TEXT("game");
			case EUeAgentExecutionWorldType::Preview:
				return TEXT("preview");
			default:
				return TEXT("invalid");
			}
		}

		bool ParseExecutionWorldType(
			const FString& Text,
			UE::UAI::ContextResource::
				EUeAgentExecutionWorldType& Out)
		{
			using UE::UAI::ContextResource::
				EUeAgentExecutionWorldType;
			if (Text == TEXT("editor"))
			{
				Out = EUeAgentExecutionWorldType::Editor;
			}
			else if (Text == TEXT("pie"))
			{
				Out = EUeAgentExecutionWorldType::Pie;
			}
			else if (Text == TEXT("simulate_in_editor"))
			{
				Out =
					EUeAgentExecutionWorldType::SimulateInEditor;
			}
			else if (Text == TEXT("game"))
			{
				Out = EUeAgentExecutionWorldType::Game;
			}
			else if (Text == TEXT("preview"))
			{
				Out = EUeAgentExecutionWorldType::Preview;
			}
			else
			{
				return false;
			}
			return true;
		}

		TSharedRef<FJsonObject> ExecutionContextJson(
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextInput& Context)
		{
			TSharedRef<FJsonObject> Object =
				MakeShared<FJsonObject>();
			Object->SetStringField(
				TEXT("project_instance_id"),
				Context.ProjectInstanceId);
			Object->SetStringField(
				TEXT("editor_instance_id"),
				Context.EditorInstanceId);
			Object->SetStringField(
				TEXT("state_plane"),
				StatePlaneName(Context.StatePlane));
			Object->SetStringField(
				TEXT("world_type"),
				ExecutionWorldTypeName(Context.WorldType));
			Object->SetStringField(
				TEXT("world_identity"),
				Context.WorldIdentity);
			Object->SetStringField(
				TEXT("level_identity"),
				Context.LevelIdentity);
			Object->SetArrayField(
				TEXT("data_layer_identities"),
				StringArrayJson(Context.DataLayerIdentities));
			Object->SetStringField(
				TEXT("world_partition_identity"),
				Context.WorldPartitionIdentity);
			if (Context.PieInstanceId.IsSet())
			{
				Object->SetNumberField(
					TEXT("pie_instance_id"),
					Context.PieInstanceId.GetValue());
			}
			else
			{
				Object->SetField(
					TEXT("pie_instance_id"),
					MakeShared<FJsonValueNull>());
			}
			Object->SetStringField(TEXT("net_mode"), Context.NetMode);
			Object->SetStringField(TEXT("net_role"), Context.NetRole);
			Object->SetStringField(
				TEXT("asset_editor_identity"),
				Context.AssetEditorIdentity);
			Object->SetStringField(
				TEXT("viewport_identity"),
				Context.ViewportIdentity);
			Object->SetArrayField(
				TEXT("selection_identities"),
				StringArrayJson(Context.SelectionIdentities));
			Object->SetStringField(
				TEXT("context_revision"),
				Context.ContextRevision);
			Object->SetBoolField(
				TEXT("resolved_from_current"),
				Context.bResolvedFromCurrent);
			return Object;
		}

		bool ParseStringArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			TArray<FString>& Out)
		{
			Out.Reset();
			if (!Object.IsValid()
				|| !Object->HasTypedField<EJson::Array>(Field))
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value :
				Object->GetArrayField(Field))
			{
				if (!Value.IsValid()
					|| Value->Type != EJson::String)
				{
					return false;
				}
				Out.Add(Value->AsString());
			}
			return true;
		}

		bool ParseExecutionContext(
			const TSharedPtr<FJsonObject>& Object,
			UE::UAI::ContextResource::
				FUeAgentExecutionContextInput& Out,
			FUeAgentQueryBindingError& OutError)
		{
			if (!Object.IsValid())
			{
				SetError(
					OutError,
					TEXT("query_payload_invalid"),
					TEXT("execution_context"),
					TEXT("Execution context object is missing."));
				return false;
			}
			for (const TCHAR* Field : {
				TEXT("project_instance_id"),
				TEXT("editor_instance_id"),
				TEXT("state_plane"),
				TEXT("world_type"),
				TEXT("world_identity"),
				TEXT("level_identity"),
				TEXT("world_partition_identity"),
				TEXT("net_mode"),
				TEXT("net_role"),
				TEXT("asset_editor_identity"),
				TEXT("viewport_identity"),
				TEXT("context_revision"),
			})
			{
				if (!Object->HasTypedField<EJson::String>(Field))
				{
					SetError(
						OutError,
						TEXT("query_payload_invalid"),
						Field,
						TEXT(
							"Execution context string field is missing."));
					return false;
				}
			}
			if (!Object->HasTypedField<EJson::Boolean>(
					TEXT("resolved_from_current"))
				|| !ParseStringArray(
					Object,
					TEXT("data_layer_identities"),
					Out.DataLayerIdentities)
				|| !ParseStringArray(
					Object,
					TEXT("selection_identities"),
					Out.SelectionIdentities))
			{
				SetError(
					OutError,
					TEXT("query_payload_invalid"),
					TEXT("execution_context"),
					TEXT(
						"Execution context arrays or resolution flag are invalid."));
				return false;
			}
			Out.ProjectInstanceId =
				Object->GetStringField(TEXT("project_instance_id"));
			Out.EditorInstanceId =
				Object->GetStringField(TEXT("editor_instance_id"));
			if (!ParseStatePlane(
					Object->GetStringField(TEXT("state_plane")),
					Out.StatePlane)
				|| !ParseExecutionWorldType(
					Object->GetStringField(TEXT("world_type")),
					Out.WorldType))
			{
				SetError(
					OutError,
					TEXT("query_payload_invalid"),
					TEXT("execution_context"),
					TEXT(
						"Execution state plane or world type is invalid."));
				return false;
			}
			Out.WorldIdentity =
				Object->GetStringField(TEXT("world_identity"));
			Out.LevelIdentity =
				Object->GetStringField(TEXT("level_identity"));
			Out.WorldPartitionIdentity =
				Object->GetStringField(
					TEXT("world_partition_identity"));
			Out.NetMode = Object->GetStringField(TEXT("net_mode"));
			Out.NetRole = Object->GetStringField(TEXT("net_role"));
			Out.AssetEditorIdentity =
				Object->GetStringField(
					TEXT("asset_editor_identity"));
			Out.ViewportIdentity =
				Object->GetStringField(TEXT("viewport_identity"));
			Out.ContextRevision =
				Object->GetStringField(TEXT("context_revision"));
			Out.bResolvedFromCurrent =
				Object->GetBoolField(TEXT("resolved_from_current"));
			const TSharedPtr<FJsonValue>* Pie =
				Object->Values.Find(TEXT("pie_instance_id"));
			if (!Pie)
			{
				SetError(
					OutError,
					TEXT("query_payload_invalid"),
					TEXT("pie_instance_id"),
					TEXT("PIE instance field is missing."));
				return false;
			}
			if ((*Pie)->Type == EJson::Number)
			{
				Out.PieInstanceId =
					static_cast<int32>((*Pie)->AsNumber());
			}
			else if ((*Pie)->Type != EJson::Null)
			{
				SetError(
					OutError,
					TEXT("query_payload_invalid"),
					TEXT("pie_instance_id"),
					TEXT(
						"PIE instance must be an integer or null."));
				return false;
			}
			return true;
		}

		bool ParseContractValue(
			const TSharedPtr<FJsonValue>& Value,
			FUeAgentContractValue& Out,
			FUeAgentQueryBindingError& OutError)
		{
			FString Wrapped;
			const TSharedRef<FJsonObject> Wrapper =
				MakeShared<FJsonObject>();
			if (Value.IsValid())
			{
				Wrapper->SetField(TEXT("v"), Value);
			}
			if (!Value.IsValid()
				|| !SerializeJson(Wrapper, Wrapped)
				|| !Wrapped.StartsWith(TEXT("{\"v\":"))
				|| !Wrapped.EndsWith(TEXT("}")))
			{
				SetError(
					OutError,
					TEXT("query_payload_invalid"),
					TEXT("input"),
					TEXT("Domain query input is invalid JSON."));
				return false;
			}
			Out = FUeAgentContractValue(
				Wrapped.Mid(5, Wrapped.Len() - 6));
			return true;
		}
	}

	bool FUeAgentQueryPayloadCodec::TryDecode(
		const FUeAgentContractValue& Value,
		FUeAgentResourceTracker& Resources,
		TOptional<FUeAgentQueryBindingPayload>& OutPayload,
		FUeAgentQueryBindingError& OutError)
	{
		OutPayload.Reset();
		OutError = {};
		if (!Value.IsValid()
			|| !ChargeCodec(
				Resources,
				EUeAgentResourceDimension::InputBytes,
				UE::UAI::ContextResource::MeasureText(Value.GetJsonValue()).Utf8Bytes,
				TEXT("c3q.payload.decode"),
				OutError))
		{
			if (!OutError.IsValid())
			{
				SetError(OutError, TEXT("query_payload_invalid"), TEXT("payload"), TEXT("Payload JSON is empty."));
			}
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Value.GetJsonValue());
		if (!FJsonSerializer::Deserialize(Reader, Root)
			|| !Root.IsValid()
			|| !Root->HasTypedField<EJson::String>(TEXT("query_kind"))
			|| !Root->HasTypedField<EJson::Object>(TEXT("session"))
			|| !Root->HasTypedField<EJson::Object>(TEXT("projection")))
		{
			SetError(OutError, TEXT("query_payload_invalid"), TEXT("payload"), TEXT("Payload must be an object with query_kind, session, and projection."));
			return false;
		}
		if (!ChargeCodec(Resources, EUeAgentResourceDimension::Nodes, 1, TEXT("c3q.payload.decode.root"), OutError))
		{
			return false;
		}
		TOptional<FUeAgentQuerySessionIdentity> Session;
		TOptional<FUeAgentProjectionRequest> Projection;
		if (!ParseSession(Root->GetObjectField(TEXT("session")), Session, OutError)
			|| !ParseProjection(Root->GetObjectField(TEXT("projection")), Projection, OutError))
		{
			return false;
		}
		const FString Kind = Root->GetStringField(TEXT("query_kind"));
		if (Kind == TEXT("catalog_search"))
		{
			if (!Root->HasTypedField<EJson::Object>(TEXT("query")))
			{
				SetError(OutError, TEXT("query_payload_invalid"), TEXT("query"), TEXT("Catalog search query object is missing."));
				return false;
			}
			const TSharedPtr<FJsonObject> Query = Root->GetObjectField(TEXT("query"));
			if (!Query->HasTypedField<EJson::String>(TEXT("catalog_kind"))
				|| !Query->HasTypedField<EJson::String>(TEXT("search_text"))
				|| !Query->HasTypedField<EJson::String>(TEXT("match_mode"))
				|| !Query->HasTypedField<EJson::String>(TEXT("materialization_policy")))
			{
				SetError(OutError, TEXT("query_payload_invalid"), TEXT("query"), TEXT("Catalog search fields are missing."));
				return false;
			}
			const FString MatchMode = Query->GetStringField(TEXT("match_mode"));
			const FString Materialization = Query->GetStringField(TEXT("materialization_policy"));
			if (MatchMode != TEXT("broad") && MatchMode != TEXT("exact"))
			{
				SetError(OutError, TEXT("query_payload_invalid"), TEXT("query.match_mode"), TEXT("Unknown discovery match mode."));
				return false;
			}
			if (Materialization != TEXT("index_only") && Materialization != TEXT("allow_materialization"))
			{
				SetError(OutError, TEXT("query_payload_invalid"), TEXT("query.materialization_policy"), TEXT("Unknown materialization policy."));
				return false;
			}
			FUeAgentCatalogSearchQuery Typed(
				Session.GetValue(),
				FUeAgentDiscoveryQuery(
					Query->GetStringField(TEXT("catalog_kind")),
					Query->GetStringField(TEXT("search_text")),
					MatchMode == TEXT("broad")
						? EUeAgentDiscoveryMatchMode::Broad
						: EUeAgentDiscoveryMatchMode::Exact,
					Materialization == TEXT("index_only")
						? EUeAgentMaterializationPolicy::IndexOnly
						: EUeAgentMaterializationPolicy::AllowMaterialization),
				Projection.GetValue());
			if (!Typed.IsValid())
			{
				SetError(OutError, TEXT("query_payload_invalid"), TEXT("catalog_search"), TEXT("Typed catalog search is invalid."));
				return false;
			}
			OutPayload.Emplace(MoveTemp(Typed));
			return true;
		}
		if (Kind == TEXT("catalog_resolve"))
		{
			if (!Root->HasTypedField<EJson::Object>(TEXT("catalog"))
				|| !Root->HasTypedField<EJson::String>(TEXT("candidate_id")))
			{
				SetError(OutError, TEXT("query_payload_invalid"), TEXT("catalog_resolve"), TEXT("Catalog and candidate_id are required."));
				return false;
			}
			TSharedPtr<const FUeAgentCatalogSnapshot> Catalog;
			if (!ParseCatalog(
					Root->GetObjectField(TEXT("catalog")),
					Resources,
					Catalog,
					OutError))
			{
				if (!OutError.IsValid())
				{
					SetError(OutError, TEXT("query_payload_invalid"), TEXT("catalog"), TEXT("Catalog snapshot is invalid."));
				}
				return false;
			}
			FUeAgentCatalogResolveQuery Typed(
				Session.GetValue(),
				Catalog.ToSharedRef(),
				Root->GetStringField(TEXT("candidate_id")),
				Projection.GetValue());
			if (!Typed.IsValid())
			{
				SetError(OutError, TEXT("query_payload_invalid"), TEXT("catalog_resolve"), TEXT("Typed exact resolve is invalid."));
				return false;
			}
			OutPayload.Emplace(MoveTemp(Typed));
			return true;
		}
		if (Kind == TEXT("state_read"))
		{
			if (!Root->HasTypedField<EJson::Object>(TEXT("ticket"))
				|| !Root->HasTypedField<EJson::Object>(TEXT("binding"))
				|| !Root->HasTypedField<EJson::String>(TEXT("state_plane")))
			{
				SetError(OutError, TEXT("query_payload_invalid"), TEXT("state_read"), TEXT("Ticket, binding, and state_plane are required."));
				return false;
			}
			TOptional<FUeAgentCandidateBinding> Binding;
			TOptional<FUeAgentQueryTicket> Ticket;
			UE::UAI::BindingGateway::Ticket::
				FUeAgentBindingTicketJsonError CodecError;
			if (!UE::UAI::BindingGateway::Ticket::
					FUeAgentBindingTicketJsonCodec::TryParseBinding(
						Root->GetObjectField(TEXT("binding")),
						Binding,
						CodecError)
				|| !UE::UAI::BindingGateway::Ticket::
					FUeAgentBindingTicketJsonCodec::TryParseTicket(
						Root->GetObjectField(TEXT("ticket")),
						Ticket,
						CodecError))
			{
				SetError(
					OutError,
					CodecError.Code.IsEmpty()
						? TEXT("query_payload_invalid")
						: CodecError.Code,
					CodecError.SubjectId,
					CodecError.Detail);
				return false;
			}
			EUeAgentStatePlane StatePlane =
				EUeAgentStatePlane::AuthoringMemory;
			if (!Ticket.IsSet()
				|| !ParseStatePlane(Root->GetStringField(TEXT("state_plane")), StatePlane))
			{
				SetError(OutError, TEXT("query_payload_invalid"), TEXT("state_read"), TEXT("Ticket or state plane is invalid."));
				return false;
			}
			FUeAgentStateReadQuery Typed(
				Session.GetValue(),
				Ticket.GetValue(),
				Binding.GetValue(),
				StatePlane,
				Projection.GetValue());
			if (!Typed.IsValid())
			{
				SetError(OutError, TEXT("query_payload_invalid"), TEXT("state_read"), TEXT("Typed state read is invalid."));
				return false;
			}
			OutPayload.Emplace(MoveTemp(Typed));
			return true;
		}
		if (Kind == TEXT("domain_query"))
		{
			if (!Root->HasTypedField<EJson::Object>(
					TEXT("execution_context")))
			{
				SetError(
					OutError,
					TEXT("query_payload_invalid"),
					TEXT("execution_context"),
					TEXT(
						"Domain query execution context is required."));
				return false;
			}
			const TSharedPtr<FJsonValue>* InputValue =
				Root->Values.Find(TEXT("input"));
			FUeAgentContractValue Input(TEXT("{}"));
			UE::UAI::ContextResource::
				FUeAgentExecutionContextInput ExecutionContext;
			if (!InputValue
				|| !ParseContractValue(
					*InputValue,
					Input,
					OutError)
				|| !ParseExecutionContext(
					Root->GetObjectField(
						TEXT("execution_context")),
					ExecutionContext,
					OutError))
			{
				return false;
			}
			TOptional<FUeAgentDomainQueryTarget> Target;
			if (const TSharedPtr<FJsonValue>* TargetValue =
				Root->Values.Find(TEXT("target")))
			{
				if ((*TargetValue)->Type == EJson::Object)
				{
					const TSharedPtr<FJsonObject> TargetObject =
						(*TargetValue)->AsObject();
					if (!TargetObject->HasTypedField<EJson::Object>(
							TEXT("ticket"))
						|| !TargetObject
							->HasTypedField<EJson::Object>(
								TEXT("binding")))
					{
						SetError(
							OutError,
							TEXT("query_payload_invalid"),
							TEXT("target"),
							TEXT(
								"Domain target requires ticket and binding."));
						return false;
					}
					TOptional<FUeAgentCandidateBinding> Binding;
					TOptional<FUeAgentQueryTicket> Ticket;
					UE::UAI::BindingGateway::Ticket::
						FUeAgentBindingTicketJsonError CodecError;
					if (!UE::UAI::BindingGateway::Ticket::
							FUeAgentBindingTicketJsonCodec::
								TryParseBinding(
									TargetObject
										->GetObjectField(
											TEXT("binding")),
									Binding,
									CodecError)
						|| !UE::UAI::BindingGateway::Ticket::
							FUeAgentBindingTicketJsonCodec::
								TryParseTicket(
									TargetObject
										->GetObjectField(
											TEXT("ticket")),
									Ticket,
									CodecError)
						|| !Binding.IsSet()
						|| !Ticket.IsSet())
					{
						SetError(
							OutError,
							CodecError.Code.IsEmpty()
								? TEXT("query_payload_invalid")
								: CodecError.Code,
							CodecError.SubjectId,
							CodecError.Detail);
						return false;
					}
					Target.Emplace(
						Ticket.GetValue(),
						Binding.GetValue());
				}
				else if ((*TargetValue)->Type != EJson::Null)
				{
					SetError(
						OutError,
						TEXT("query_payload_invalid"),
						TEXT("target"),
						TEXT(
							"Domain target must be an object or null."));
					return false;
				}
			}
			FUeAgentDomainQuery Typed(
				Session.GetValue(),
				MoveTemp(ExecutionContext),
				MoveTemp(Input),
				MoveTemp(Target),
				Projection.GetValue());
			if (!Typed.IsValid())
			{
				SetError(
					OutError,
					TEXT("query_payload_invalid"),
					TEXT("domain_query"),
					TEXT("Typed domain query is invalid."));
				return false;
			}
			OutPayload.Emplace(MoveTemp(Typed));
			return true;
		}
		SetError(OutError, TEXT("query_payload_kind_unsupported"), Kind, TEXT("Unknown C3Q payload kind."));
		return false;
	}

	bool FUeAgentQueryPayloadCodec::TryEncode(
		const FUeAgentQueryBindingPayload& Payload,
		FUeAgentResourceTracker& Resources,
		FUeAgentContractValue& OutValue,
		FUeAgentQueryBindingError& OutError)
	{
		OutError = {};
		if (!Payload.IsValid())
		{
			SetError(OutError, TEXT("query_payload_invalid"), TEXT("payload"), TEXT("Typed C3Q payload is invalid."));
			return false;
		}
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		switch (Payload.GetKind())
		{
		case EUeAgentQueryBindingPayloadKind::CatalogSearch:
		{
			const FUeAgentCatalogSearchQuery& Typed = *Payload.GetCatalogSearch();
			Root->SetStringField(TEXT("query_kind"), TEXT("catalog_search"));
			Root->SetObjectField(
				TEXT("session"),
				UE::UAI::BindingGateway::Ticket::
					FUeAgentBindingTicketJsonCodec::SessionToJson(
						Typed.GetSession()));
			TSharedRef<FJsonObject> Query = MakeShared<FJsonObject>();
			Query->SetStringField(TEXT("catalog_kind"), Typed.GetQuery().GetCatalogKind());
			Query->SetStringField(TEXT("search_text"), Typed.GetQuery().GetSearchText());
			Query->SetStringField(
				TEXT("match_mode"),
				Typed.GetQuery().GetMatchMode() == EUeAgentDiscoveryMatchMode::Broad
					? TEXT("broad")
					: TEXT("exact"));
			Query->SetStringField(
				TEXT("materialization_policy"),
				Typed.GetQuery().GetMaterializationPolicy() == EUeAgentMaterializationPolicy::IndexOnly
					? TEXT("index_only")
					: TEXT("allow_materialization"));
			Root->SetObjectField(TEXT("query"), Query);
			Root->SetObjectField(TEXT("projection"), ProjectionJson(Typed.GetProjection()));
			break;
		}
		case EUeAgentQueryBindingPayloadKind::CatalogResolve:
		{
			const FUeAgentCatalogResolveQuery& Typed = *Payload.GetCatalogResolve();
			Root->SetStringField(TEXT("query_kind"), TEXT("catalog_resolve"));
			Root->SetObjectField(
				TEXT("session"),
				UE::UAI::BindingGateway::Ticket::
					FUeAgentBindingTicketJsonCodec::SessionToJson(
						Typed.GetSession()));
			Root->SetObjectField(TEXT("catalog"), CatalogJson(Typed.GetCatalog().Get()));
			Root->SetStringField(TEXT("candidate_id"), Typed.GetCandidateId());
			Root->SetObjectField(TEXT("projection"), ProjectionJson(Typed.GetProjection()));
			break;
		}
		case EUeAgentQueryBindingPayloadKind::StateRead:
		{
			const FUeAgentStateReadQuery& Typed = *Payload.GetStateRead();
			Root->SetStringField(TEXT("query_kind"), TEXT("state_read"));
			Root->SetObjectField(
				TEXT("session"),
				UE::UAI::BindingGateway::Ticket::
					FUeAgentBindingTicketJsonCodec::SessionToJson(
						Typed.GetSession()));
			Root->SetObjectField(
				TEXT("ticket"),
				UE::UAI::BindingGateway::Ticket::
					FUeAgentBindingTicketJsonCodec::TicketToJson(
						Typed.GetTicket()));
			Root->SetObjectField(
				TEXT("binding"),
				UE::UAI::BindingGateway::Ticket::
					FUeAgentBindingTicketJsonCodec::BindingToJson(
						Typed.GetBinding()));
			Root->SetStringField(TEXT("state_plane"), StatePlaneName(Typed.GetStatePlane()));
			Root->SetObjectField(TEXT("projection"), ProjectionJson(Typed.GetProjection()));
			break;
		}
		case EUeAgentQueryBindingPayloadKind::DomainQuery:
		{
			const FUeAgentDomainQuery& Typed =
				*Payload.GetDomainQuery();
			Root->SetStringField(
				TEXT("query_kind"),
				TEXT("domain_query"));
			Root->SetObjectField(
				TEXT("session"),
				UE::UAI::BindingGateway::Ticket::
					FUeAgentBindingTicketJsonCodec::SessionToJson(
						Typed.GetSession()));
			Root->SetObjectField(
				TEXT("execution_context"),
				ExecutionContextJson(
					Typed.GetExecutionContext()));
			TSharedPtr<FJsonValue> Input;
			const TSharedRef<TJsonReader<>> InputReader =
				TJsonReaderFactory<>::Create(
					Typed.GetInput().GetJsonValue());
			if (!FJsonSerializer::Deserialize(
					InputReader,
					Input,
					FJsonSerializer::EFlags::
						StoreNumbersAsStrings)
				|| !Input.IsValid())
			{
				SetError(
					OutError,
					TEXT("query_payload_invalid"),
					TEXT("input"),
					TEXT(
						"Domain query input must be valid JSON."));
				return false;
			}
			Root->SetField(TEXT("input"), Input);
			if (Typed.GetTarget().IsSet())
			{
				const FUeAgentDomainQueryTarget& Target =
					Typed.GetTarget().GetValue();
				TSharedRef<FJsonObject> TargetObject =
					MakeShared<FJsonObject>();
				TargetObject->SetObjectField(
					TEXT("ticket"),
					UE::UAI::BindingGateway::Ticket::
						FUeAgentBindingTicketJsonCodec::
							TicketToJson(Target.GetTicket()));
				TargetObject->SetObjectField(
					TEXT("binding"),
					UE::UAI::BindingGateway::Ticket::
						FUeAgentBindingTicketJsonCodec::
							BindingToJson(Target.GetBinding()));
				Root->SetObjectField(
					TEXT("target"),
					TargetObject);
			}
			else
			{
				Root->SetField(
					TEXT("target"),
					MakeShared<FJsonValueNull>());
			}
			Root->SetObjectField(
				TEXT("projection"),
				ProjectionJson(Typed.GetProjection()));
			break;
		}
		default:
			SetError(OutError, TEXT("query_payload_invalid"), TEXT("payload"), TEXT("Unknown typed payload kind."));
			return false;
		}
		FString Text;
		if (!SerializeJson(Root, Text)
			|| !ChargeCodec(
				Resources,
				EUeAgentResourceDimension::OutputBytes,
				UE::UAI::ContextResource::MeasureText(Text).Utf8Bytes,
				TEXT("c3q.payload.encode"),
				OutError))
		{
			if (!OutError.IsValid())
			{
				SetError(OutError, TEXT("query_payload_encode_failed"), TEXT("payload"), TEXT("Failed to serialize typed C3Q payload."));
			}
			return false;
		}
		OutValue = FUeAgentContractValue(MoveTemp(Text));
		return true;
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
			SetError(OutError, TEXT("query_response_invalid"), TEXT("response"), TEXT("Typed C3Q response is invalid."));
			return false;
		}
		const FUeAgentProjectedResponse& Projection = *Response.GetProjection();
		TSharedRef<FJsonObject> ProjectionObject = MakeShared<FJsonObject>();
		ProjectionObject->SetObjectField(TEXT("descriptor"), DescriptorJson(Projection.GetDescriptorIdentity()));
		ProjectionObject->SetStringField(TEXT("request_id"), Projection.GetRequestId());
		ProjectionObject->SetStringField(
			TEXT("result_kind"),
			Projection.GetResultKind() == UE::UAI::Projection::EUeAgentProjectionResultKind::Query
				? TEXT("query")
				: TEXT("operation"));
		ProjectionObject->SetStringField(TEXT("profile_id"), Projection.GetProfileId());
		ProjectionObject->SetStringField(
			TEXT("completeness"),
			Projection.GetCompleteness() == UE::UAI::Projection::EUeAgentProjectionCompleteness::Complete
				? TEXT("complete")
				: TEXT("partial_page"));
		TSharedRef<FJsonObject> TokenEstimate = MakeShared<FJsonObject>();
		TokenEstimate->SetStringField(
			TEXT("kind"),
			Projection.GetTokenEstimate().GetKind() == UE::UAI::Projection::EUeAgentTokenEstimateKind::Exact
				? TEXT("exact")
				: TEXT("conservative"));
		TokenEstimate->SetNumberField(TEXT("tokens"), static_cast<double>(Projection.GetTokenEstimate().GetTokens()));
		TokenEstimate->SetNumberField(TEXT("utf8_bytes"), static_cast<double>(Projection.GetTokenEstimate().GetUtf8Bytes()));
		ProjectionObject->SetObjectField(TEXT("token_estimate"), TokenEstimate);

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
				ProjectedSnapshotJson(*ProjectedPayload.GetSnapshot()));
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
			SetError(OutError, TEXT("query_response_invalid"), TEXT("projection.payload"), TEXT("Projected payload has no recognized typed value."));
			return false;
		}
		ProjectionObject->SetObjectField(TEXT("payload"), Payload);

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("projection"), ProjectionObject);
		if (Response.GetTicket().IsSet())
		{
			Root->SetObjectField(
				TEXT("ticket"),
				UE::UAI::BindingGateway::Ticket::
					FUeAgentBindingTicketJsonCodec::TicketToJson(
						Response.GetTicket().GetValue()));
		}
		else
		{
			Root->SetField(TEXT("ticket"), MakeShared<FJsonValueNull>());
		}
		FString Text;
		if (!SerializeJson(Root, Text)
			|| !ChargeCodec(
				Resources,
				EUeAgentResourceDimension::OutputBytes,
				UE::UAI::ContextResource::MeasureText(Text).Utf8Bytes,
				TEXT("c3q.response.encode"),
				OutError))
		{
			if (!OutError.IsValid())
			{
				SetError(OutError, TEXT("query_response_encode_failed"), TEXT("response"), TEXT("Failed to serialize typed C3Q response."));
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
		if (!DescriptorIdentity.IsValid()
			|| !RegistryHash.StartsWith(TEXT("sha256:")))
		{
			SetError(
				OutError,
				TEXT("query_binding_invalid"),
				DescriptorIdentity.GetDescriptorId(),
				TEXT("A valid descriptor identity and registry hash are required."));
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
		if (DescriptorRegistry->GetDescriptors().IsEmpty()
			|| !DescriptorRegistry->GetRegistryHash().StartsWith(
				TEXT("sha256:")))
		{
			SetError(
				OutError,
				TEXT("query_binding_invalid"),
				TEXT("descriptor_registry"),
				TEXT(
					"A compiled descriptor registry is required."));
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
			SetError(
				OutError,
				TEXT("query_domain_binding_unavailable"),
				Request.GetRequestId(),
				TEXT(
					"Domain query requires descriptor and provider registries."));
			return false;
		}
		if (!CallerContext.IsValid() || !Query.IsValid())
		{
			SetError(
				OutError,
				TEXT("query_request_invalid"),
				Request.GetRequestId(),
				TEXT(
					"Typed domain request or caller context is invalid."));
			return false;
		}

		FUeAgentCancellationRequested CancellationError;
		if (!Cancellation.TryCheckpoint(CancellationError))
		{
			SetError(
				OutError,
				TEXT("query_cancelled"),
				Request.GetRequestId(),
				CancellationError.Reason);
			return false;
		}
		FUeAgentOperationDeadlineExceeded DeadlineError;
		if (!Deadline.TryCheckpoint(
				TEXT("c3q.domain.start"),
				DeadlineError))
		{
			SetError(
				OutError,
				TEXT("query_deadline_exceeded"),
				Request.GetRequestId(),
				DeadlineError.Stage);
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
			SetError(
				OutError,
				Code,
				Request.GetDescriptorIdentity().GetDescriptorId(),
				TEXT(
					"Request descriptor does not exactly match the registry."));
			return false;
		}
		const TOptional<UE::UAI::Contract::FUeAgentQueryDescriptor>
			Descriptor =
				DescriptorRegistry->FindQueryExact(
					Request.GetDescriptorIdentity());
		if (!Descriptor.IsSet())
		{
			SetError(
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
			SetError(
				OutError,
				TEXT("query_session_unavailable"),
				Request.GetRequestId(),
				TEXT("Current server session is invalid."));
			return false;
		}
		if (CurrentSession.GetRegistryHash() != RegistryHash
			|| RequestedSession.GetRegistryHash()
				!= CurrentSession.GetRegistryHash())
		{
			SetError(
				OutError,
				TEXT("registry_hash_mismatch"),
				Request.GetRequestId(),
				TEXT(
					"Domain query registry hash is stale."));
			return false;
		}
		if (RequestedSession.GetServerSessionId()
			!= CurrentSession.GetServerSessionId())
		{
			SetError(
				OutError,
				TEXT("server_session_stale"),
				Request.GetRequestId(),
				TEXT(
					"Domain query belongs to another server session."));
			return false;
		}
		if (RequestedSession.GetRegistryEpoch()
			!= CurrentSession.GetRegistryEpoch())
		{
			SetError(
				OutError,
				TEXT("registry_epoch_stale"),
				Request.GetRequestId(),
				TEXT("Domain query registry epoch is stale."));
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
					Error.Code = TEXT("query_cancelled");
					Error.InstancePath = InstancePath;
					Error.Detail = Cancelled.Reason;
					return false;
				}
				FUeAgentOperationDeadlineExceeded Expired;
				if (!Deadline.TryCheckpoint(
						TEXT("c3q.domain.schema"),
						Expired))
				{
					Error.Code =
						TEXT("query_deadline_exceeded");
					Error.InstancePath = InstancePath;
					Error.Detail = Expired.Stage;
					return false;
				}
				FUeAgentResourceBudgetExceeded BudgetError;
				if (!Resources->TryCharge(
						EUeAgentResourceDimension::WorkUnits,
						WorkUnits,
						TEXT("c3q.domain.schema"),
						BudgetError))
				{
					Error.Code = BudgetError.Code;
					Error.InstancePath = InstancePath;
					Error.Detail =
						TEXT(
							"Contract schema work budget exceeded.");
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
			SetError(
				OutError,
				SchemaError.Code,
				SchemaError.InstancePath,
				SchemaError.Detail);
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
			SetError(
				OutError,
				ExecutionError.Code,
				Request.GetRequestId(),
				TEXT(
					"Explicit execution context violates descriptor policy."));
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
			SetError(
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
			SetError(
				OutError,
				OperationContextError.Code,
				Request.GetRequestId(),
				TEXT(
					"OperationContext composition failed."));
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
			return false;
		}
		if (!Source.IsValid() || !Source->IsValid())
		{
			SetError(
				OutError,
				TEXT("query_provider_result_missing"),
				Descriptor->GetProviderBinding(),
				TEXT(
					"Provider succeeded without a projection source."));
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
			CopyError(ProjectionError, OutError);
			return false;
		}
		if (!Projected.IsValid())
		{
			SetError(
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
		OutResult = MakeShared<FUeAgentQueryBindingResult>(
			Descriptor->GetIdentity(),
			Request.GetRequestId(),
			MoveTemp(Response));
		return OutResult->IsValid();
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
			SetError(
				OutError,
				TEXT("query_cancelled"),
				Request.GetRequestId(),
				CancellationError.Reason);
			return false;
		}
		FUeAgentOperationDeadlineExceeded DeadlineError;
		if (!Deadline.TryCheckpoint(TEXT("c3q.query_binding.start"), DeadlineError))
		{
			SetError(
				OutError,
				TEXT("query_deadline_exceeded"),
				Request.GetRequestId(),
				DeadlineError.Stage);
			return false;
		}
		if (!Request.IsValid() || !CallerContext.IsValid())
		{
			SetError(
				OutError,
				TEXT("query_request_invalid"),
				Request.GetRequestId(),
				TEXT("Typed request or caller context is invalid."));
			return false;
		}
		if (!(Request.GetDescriptorIdentity() == DescriptorIdentity))
		{
			SetError(
				OutError,
				TEXT("descriptor_hash_mismatch"),
				Request.GetDescriptorIdentity().GetDescriptorId(),
				TEXT("Request descriptor identity differs from the QueryBinding descriptor."));
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
			SetError(OutError, TEXT("query_payload_invalid"), Request.GetRequestId(), TEXT("Unknown typed query kind."));
			return false;
		}
		const FUeAgentQuerySessionIdentity CurrentSession = SessionSource->GetCurrent();
		if (!CurrentSession.IsValid())
		{
			SetError(OutError, TEXT("query_session_unavailable"), Request.GetRequestId(), TEXT("Current server session is invalid."));
			return false;
		}
		if (CurrentSession.GetRegistryHash() != RegistryHash)
		{
			SetError(OutError, TEXT("registry_hash_mismatch"), Request.GetRequestId(), TEXT("Current registry hash differs from the QueryBinding composition hash."));
			return false;
		}
		if (RequestedSession->GetServerSessionId() != CurrentSession.GetServerSessionId())
		{
			SetError(OutError, TEXT("server_session_stale"), Request.GetRequestId(), TEXT("Request belongs to another server session."));
			return false;
		}
		if (RequestedSession->GetRegistryEpoch() != CurrentSession.GetRegistryEpoch())
		{
			SetError(OutError, TEXT("registry_epoch_stale"), Request.GetRequestId(), TEXT("Request registry epoch is stale."));
			return false;
		}
		if (RequestedSession->GetRegistryHash() != CurrentSession.GetRegistryHash())
		{
			SetError(OutError, TEXT("registry_hash_mismatch"), Request.GetRequestId(), TEXT("Request registry hash is stale."));
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
				CopyError(DiscoveryError, OutError);
				return false;
			}
			if (!Catalog.IsValid())
			{
				SetError(OutError, TEXT("query_discovery_result_missing"), Request.GetRequestId(), TEXT("Discovery succeeded without a catalog."));
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
				CopyError(ProjectionError, OutError);
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
				CopyError(DiscoveryError, OutError);
				return false;
			}
			if (!Binding.IsValid())
			{
				SetError(OutError, TEXT("query_discovery_result_missing"), Query.GetCandidateId(), TEXT("Exact resolve succeeded without a candidate binding."));
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
				CopyError(ProjectionError, OutError);
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
				CopyError(DiscoveryError, OutError);
				return false;
			}
			if (!Snapshot.IsValid())
			{
				SetError(OutError, TEXT("query_discovery_result_missing"), Query.GetBinding().GetBindingId(), TEXT("State read succeeded without a snapshot."));
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
				CopyError(ProjectionError, OutError);
				return false;
			}
			break;
		}
		default:
			checkNoEntry();
			return false;
		}

		if (!Projected.IsValid())
		{
			SetError(OutError, TEXT("query_projection_result_missing"), Request.GetRequestId(), TEXT("Projection succeeded without a response."));
			return false;
		}
		FUeAgentQueryBindingResponse Response(Projected.ToSharedRef(), MoveTemp(Ticket));
		OutResult = MakeShared<FUeAgentQueryBindingResult>(
			DescriptorIdentity,
			Request.GetRequestId(),
			MoveTemp(Response));
		return OutResult->IsValid();
	}
}
