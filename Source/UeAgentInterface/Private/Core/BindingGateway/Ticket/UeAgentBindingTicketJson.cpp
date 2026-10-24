#include "Core/BindingGateway/Ticket/UeAgentBindingTicketJson.h"

#include "Dom/JsonValue.h"

namespace UE::UAI::BindingGateway::Ticket
{
	namespace
	{
		void Fail(
			FUeAgentBindingTicketJsonError& OutError,
			const FString& SubjectId,
			const FString& Detail)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("binding_ticket_json_invalid"), Detail, TEXT("binding_ticket_json"), FString(), SubjectId);
			OutError.PropertyPath = SubjectId;
			OutError.Source.JsonPath = SubjectId;
		}

		void LocateDiagnostic(FUeAgentBindingTicketJsonError& Error, const FString& InputPath, const FString& SemanticRoot = FString())
		{
			FString Relative = Error.PropertyPath;
			if (Relative.IsEmpty() && !SemanticRoot.IsEmpty() && Error.SemanticPointer.StartsWith(SemanticRoot))
				Relative = Error.SemanticPointer.Mid(SemanticRoot.Len());
			Relative.ReplaceInline(TEXT("/"), TEXT("."));
			Relative.RemoveFromStart(TEXT("."));
			Error.Source.JsonPath = InputPath + (Relative.IsEmpty() ? FString() : TEXT(".") + Relative);
		}

		const TCHAR* JsonTypeName(EJson Type)
		{
			switch (Type)
			{
			case EJson::Null: return TEXT("null");
			case EJson::String: return TEXT("string");
			case EJson::Number: return TEXT("number");
			case EJson::Boolean: return TEXT("boolean");
			case EJson::Array: return TEXT("array");
			case EJson::Object: return TEXT("object");
			default: return TEXT("missing");
			}
		}

		bool RequireFields(const TSharedPtr<FJsonObject>& Object, const FString& Path,
			const TArray<const TCHAR*>& Strings, const TArray<const TCHAR*>& Numbers,
			const TArray<const TCHAR*>& Objects, const TArray<const TCHAR*>& Arrays,
			FUeAgentBindingTicketJsonError& OutError)
		{
			if (!Object.IsValid())
			{
				Fail(OutError, Path, TEXT("A JSON object is required."));
				OutError.ActualValue = MakeShared<FJsonValueString>(TEXT("missing"));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
				return false;
			}
			auto Require = [&](const TArray<const TCHAR*>& Fields, EJson Type)
			{
				for (const TCHAR* Field : Fields)
				{
					const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
					if (Value.IsValid() && Value->Type == Type) continue;
					Fail(OutError, Path + TEXT(".") + Field,
						Value.IsValid() ? TEXT("The JSON field has the wrong type.") : TEXT("The required JSON field is missing."));
					OutError.ActualValue = MakeShared<FJsonValueString>(Value.IsValid() ? JsonTypeName(Value->Type) : TEXT("missing"));
					OutError.ExpectedValue = MakeShared<FJsonValueString>(JsonTypeName(Type));
					OutError.ActualEvidence = Value;
					return false;
				}
				return true;
			};
			return Require(Strings, EJson::String) && Require(Numbers, EJson::Number)
				&& Require(Objects, EJson::Object) && Require(Arrays, EJson::Array);
		}

		TArray<TSharedPtr<FJsonValue>> Strings(
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

		TSharedRef<FJsonObject> SummaryToJson(
			const Discovery::FUeAgentCandidateSummary& Summary)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetObjectField(
				TEXT("identity"),
				FUeAgentBindingTicketJsonCodec::IdentityToJson(
					Summary.GetIdentity()));
			Object->SetObjectField(
				TEXT("descriptor"),
				FUeAgentBindingTicketJsonCodec::DescriptorToJson(
					Summary.GetDescriptorIdentity()));
			Object->SetStringField(
				TEXT("catalog_kind"),
				Summary.GetCatalogKind());
			Object->SetStringField(
				TEXT("display_name"),
				Summary.GetDisplayName());
			Object->SetStringField(TEXT("summary"), Summary.GetSummary());
			Object->SetArrayField(
				TEXT("category_path"),
				Strings(Summary.GetCategoryPath()));
			return Object;
		}

		bool TryParseSummary(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<Discovery::FUeAgentCandidateSummary>& OutSummary,
			FUeAgentBindingTicketJsonError& OutError, const FString& InputPath)
		{
			OutSummary.Reset();
			if (!RequireFields(Object, InputPath, {TEXT("catalog_kind"), TEXT("display_name"), TEXT("summary")}, {}, {TEXT("identity"), TEXT("descriptor")}, {TEXT("category_path")}, OutError))
			{
				return false;
			}
			TOptional<StateSemantic::FUeAgentCanonicalIdentity>
				Identity;
			TOptional<Contract::FUeAgentDescriptorIdentity> Descriptor;
			if (!FUeAgentBindingTicketJsonCodec::TryParseIdentity(
					Object->GetObjectField(TEXT("identity")),
					Identity,
					OutError, InputPath + TEXT(".identity"))
				|| !FUeAgentBindingTicketJsonCodec::TryParseDescriptor(
					Object->GetObjectField(TEXT("descriptor")),
					Descriptor,
					OutError, InputPath + TEXT(".descriptor")))
			{
				return false;
			}
			TArray<FString> CategoryPath;
			for (const TSharedPtr<FJsonValue>& Item :
				Object->GetArrayField(TEXT("category_path")))
			{
				if (!Item.IsValid() || Item->Type != EJson::String)
				{
					Fail(
						OutError,
						InputPath + FString::Printf(TEXT(".category_path[%d]"), CategoryPath.Num()),
						TEXT("category path must contain strings"));
					OutError.ActualValue = MakeShared<FJsonValueString>(Item.IsValid() ? JsonTypeName(Item->Type) : TEXT("missing"));
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("string"));
					return false;
				}
				CategoryPath.Add(Item->AsString());
			}
			Discovery::FUeAgentCandidateSummary Summary(
				Identity.GetValue(),
				Descriptor.GetValue(),
				Object->GetStringField(TEXT("catalog_kind")),
				Object->GetStringField(TEXT("display_name")),
				Object->GetStringField(TEXT("summary")),
				MoveTemp(CategoryPath));
			if (!Summary.IsValid())
			{
				OutError = *Summary.GetValidationDiagnostic();
				LocateDiagnostic(OutError, InputPath);
				return false;
			}
			OutSummary.Emplace(MoveTemp(Summary));
			return true;
		}

		FString OperationName(
			const Discovery::EUeAgentCandidateOperation Operation)
		{
			using Discovery::EUeAgentCandidateOperation;
			switch (Operation)
			{
			case EUeAgentCandidateOperation::Read: return TEXT("read");
			case EUeAgentCandidateOperation::Create: return TEXT("create");
			case EUeAgentCandidateOperation::Update: return TEXT("update");
			case EUeAgentCandidateOperation::Replace: return TEXT("replace");
			case EUeAgentCandidateOperation::Delete: return TEXT("delete");
			default: return FString();
			}
		}

		bool TryParseOperation(
			const FString& Name,
			Discovery::EUeAgentCandidateOperation& OutOperation)
		{
			using Discovery::EUeAgentCandidateOperation;
			if (Name == TEXT("read"))
			{
				OutOperation = EUeAgentCandidateOperation::Read;
			}
			else if (Name == TEXT("create"))
			{
				OutOperation = EUeAgentCandidateOperation::Create;
			}
			else if (Name == TEXT("update"))
			{
				OutOperation = EUeAgentCandidateOperation::Update;
			}
			else if (Name == TEXT("replace"))
			{
				OutOperation = EUeAgentCandidateOperation::Replace;
			}
			else if (Name == TEXT("delete"))
			{
				OutOperation = EUeAgentCandidateOperation::Delete;
			}
			else
			{
				return false;
			}
			return true;
		}
	}

	bool FUeAgentBindingTicketJsonCodec::TryParseSession(
		const TSharedPtr<FJsonObject>& Object,
		TOptional<FUeAgentOperationSessionIdentity>& OutSession,
		FUeAgentBindingTicketJsonError& OutError, const FString& InputPath)
	{
		OutSession.Reset();
		OutError = {};
		if (!RequireFields(Object, InputPath, {TEXT("server_session_id"), TEXT("registry_hash")}, {TEXT("registry_epoch")}, {}, {}, OutError))
		{
			return false;
		}
		FUeAgentOperationSessionIdentity Session(
			Object->GetStringField(TEXT("server_session_id")),
			static_cast<int64>(
				Object->GetNumberField(TEXT("registry_epoch"))),
			Object->GetStringField(TEXT("registry_hash")));
		if (!Session.IsValid())
		{
			OutError = Session.GetValidationDiagnostic();
			LocateDiagnostic(OutError, InputPath);
			return false;
		}
		OutSession.Emplace(MoveTemp(Session));
		return true;
	}

	TSharedRef<FJsonObject>
	FUeAgentBindingTicketJsonCodec::SessionToJson(
		const FUeAgentOperationSessionIdentity& Session)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(
			TEXT("server_session_id"),
			Session.GetServerSessionId());
		Object->SetNumberField(
			TEXT("registry_epoch"),
			static_cast<double>(Session.GetRegistryEpoch()));
		Object->SetStringField(
			TEXT("registry_hash"),
			Session.GetRegistryHash());
		return Object;
	}

	bool FUeAgentBindingTicketJsonCodec::TryParseDescriptor(
		const TSharedPtr<FJsonObject>& Object,
		TOptional<Contract::FUeAgentDescriptorIdentity>& OutDescriptor,
		FUeAgentBindingTicketJsonError& OutError, const FString& InputPath)
	{
		OutDescriptor.Reset();
		OutError = {};
		if (!RequireFields(Object, InputPath, {TEXT("descriptor_id"), TEXT("descriptor_hash")}, {TEXT("version")}, {}, {}, OutError))
		{
			return false;
		}
		Contract::FUeAgentDescriptorIdentity Descriptor(
			Object->GetStringField(TEXT("descriptor_id")),
			static_cast<int32>(
				Object->GetNumberField(TEXT("version"))),
			Object->GetStringField(TEXT("descriptor_hash")));
		if (!Descriptor.IsValid())
		{
			OutError = Descriptor.GetValidationDiagnostic();
			LocateDiagnostic(OutError, InputPath);
			return false;
		}
		OutDescriptor.Emplace(MoveTemp(Descriptor));
		return true;
	}

	TSharedRef<FJsonObject>
	FUeAgentBindingTicketJsonCodec::DescriptorToJson(
		const Contract::FUeAgentDescriptorIdentity& Descriptor)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(
			TEXT("descriptor_id"),
			Descriptor.GetDescriptorId());
		Object->SetNumberField(
			TEXT("version"),
			Descriptor.GetVersion());
		Object->SetStringField(
			TEXT("descriptor_hash"),
			Descriptor.GetDescriptorHash());
		return Object;
	}

	bool FUeAgentBindingTicketJsonCodec::TryParseIdentity(
		const TSharedPtr<FJsonObject>& Object,
		TOptional<StateSemantic::FUeAgentCanonicalIdentity>& OutIdentity,
		FUeAgentBindingTicketJsonError& OutError, const FString& InputPath)
	{
		OutIdentity.Reset();
		OutError = {};
		if (!RequireFields(Object, InputPath, {TEXT("semantic_id"), TEXT("target_kind"), TEXT("owner_id"), TEXT("selector"), TEXT("object_role"), TEXT("generation")}, {}, {}, {}, OutError))
		{
			return false;
		}
		StateSemantic::FUeAgentCanonicalIdentity Identity(
			Object->GetStringField(TEXT("semantic_id")),
			Object->GetStringField(TEXT("target_kind")),
			Object->GetStringField(TEXT("owner_id")),
			Object->GetStringField(TEXT("selector")),
			Object->GetStringField(TEXT("object_role")),
			Object->GetStringField(TEXT("generation")));
		if (!Identity.IsValid())
		{
			OutError = Identity.GetValidationDiagnostic();
			LocateDiagnostic(OutError, InputPath, TEXT("/identity"));
			return false;
		}
		OutIdentity.Emplace(MoveTemp(Identity));
		return true;
	}

	TSharedRef<FJsonObject>
	FUeAgentBindingTicketJsonCodec::IdentityToJson(
		const StateSemantic::FUeAgentCanonicalIdentity& Identity)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(
			TEXT("semantic_id"),
			Identity.GetSemanticId());
		Object->SetStringField(
			TEXT("target_kind"),
			Identity.GetTargetKind());
		Object->SetStringField(TEXT("owner_id"), Identity.GetOwnerId());
		Object->SetStringField(TEXT("selector"), Identity.GetSelector());
		Object->SetStringField(
			TEXT("object_role"),
			Identity.GetObjectRole());
		Object->SetStringField(
			TEXT("generation"),
			Identity.GetGeneration());
		return Object;
	}

	bool FUeAgentBindingTicketJsonCodec::TryParseCatalogRevision(
		const TSharedPtr<FJsonObject>& Object,
		TOptional<Discovery::FUeAgentCatalogRevision>& OutRevision,
		FUeAgentBindingTicketJsonError& OutError, const FString& InputPath)
	{
		OutRevision.Reset();
		OutError = {};
		if (!RequireFields(Object, InputPath, {TEXT("source_id"), TEXT("generation")}, {TEXT("sequence")}, {}, {}, OutError))
		{
			return false;
		}
		Discovery::FUeAgentCatalogRevision Revision(
			Object->GetStringField(TEXT("source_id")),
			Object->GetStringField(TEXT("generation")),
			static_cast<int64>(
				Object->GetNumberField(TEXT("sequence"))));
		if (!Revision.IsValid())
		{
			OutError = *Revision.GetValidationDiagnostic();
			LocateDiagnostic(OutError, InputPath);
			return false;
		}
		OutRevision.Emplace(MoveTemp(Revision));
		return true;
	}

	TSharedRef<FJsonObject>
	FUeAgentBindingTicketJsonCodec::CatalogRevisionToJson(
		const Discovery::FUeAgentCatalogRevision& Revision)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("source_id"), Revision.GetSourceId());
		Object->SetStringField(
			TEXT("generation"),
			Revision.GetGeneration());
		Object->SetNumberField(
			TEXT("sequence"),
			static_cast<double>(Revision.GetSequence()));
		return Object;
	}

	bool FUeAgentBindingTicketJsonCodec::TryParseStateRevision(
		const TSharedPtr<FJsonObject>& Object,
		TOptional<StateSemantic::FUeAgentStateRevision>& OutRevision,
		FUeAgentBindingTicketJsonError& OutError, const FString& InputPath)
	{
		OutRevision.Reset();
		OutError = {};
		if (!RequireFields(Object, InputPath, {TEXT("source_id"), TEXT("generation")}, {TEXT("sequence")}, {}, {}, OutError))
		{
			return false;
		}
		StateSemantic::FUeAgentStateRevision Revision(
			Object->GetStringField(TEXT("source_id")),
			Object->GetStringField(TEXT("generation")),
			static_cast<int64>(
				Object->GetNumberField(TEXT("sequence"))));
		if (!Revision.IsValid())
		{
			OutError = Revision.GetValidationDiagnostic();
			LocateDiagnostic(OutError, InputPath);
			return false;
		}
		OutRevision.Emplace(MoveTemp(Revision));
		return true;
	}

	TSharedRef<FJsonObject>
	FUeAgentBindingTicketJsonCodec::StateRevisionToJson(
		const StateSemantic::FUeAgentStateRevision& Revision)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("source_id"), Revision.GetSourceId());
		Object->SetStringField(
			TEXT("generation"),
			Revision.GetGeneration());
		Object->SetNumberField(
			TEXT("sequence"),
			static_cast<double>(Revision.GetSequence()));
		return Object;
	}

	bool FUeAgentBindingTicketJsonCodec::TryParseBinding(
		const TSharedPtr<FJsonObject>& Object,
		TOptional<Discovery::FUeAgentCandidateBinding>& OutBinding,
		FUeAgentBindingTicketJsonError& OutError, const FString& InputPath)
	{
		OutBinding.Reset();
		OutError = {};
		if (!RequireFields(Object, InputPath, {TEXT("binding_id"), TEXT("public_ref"), TEXT("read_port_id"), TEXT("binding_evidence")}, {}, {TEXT("summary"), TEXT("catalog_revision")}, {TEXT("supported_operations")}, OutError))
		{
			return false;
		}
		TOptional<Discovery::FUeAgentCandidateSummary> Summary;
		TOptional<Discovery::FUeAgentCatalogRevision> Revision;
		if (!TryParseSummary(
				Object->GetObjectField(TEXT("summary")),
				Summary,
				OutError, InputPath + TEXT(".summary"))
			|| !TryParseCatalogRevision(
				Object->GetObjectField(TEXT("catalog_revision")),
				Revision,
				OutError, InputPath + TEXT(".catalog_revision")))
		{
			return false;
		}
		TArray<Discovery::EUeAgentCandidateOperation> Operations;
		for (const TSharedPtr<FJsonValue>& Item :
			Object->GetArrayField(TEXT("supported_operations")))
		{
			Discovery::EUeAgentCandidateOperation Operation =
				Discovery::EUeAgentCandidateOperation::Read;
			if (!Item.IsValid()
				|| Item->Type != EJson::String
				|| !TryParseOperation(Item->AsString(), Operation))
			{
				Fail(
					OutError,
					InputPath + FString::Printf(TEXT(".supported_operations[%d]"), Operations.Num()),
					TEXT("candidate operation must be read, create, update, replace or delete"));
				OutError.ActualValue = Item.IsValid() && Item->Type == EJson::String ? Item : MakeShared<FJsonValueString>(Item.IsValid() ? JsonTypeName(Item->Type) : TEXT("missing"));
				return false;
			}
			Operations.Add(Operation);
		}
		Discovery::FUeAgentCandidateBinding Binding(
			Object->GetStringField(TEXT("binding_id")),
			Summary.GetValue(),
			Revision.GetValue(),
			Object->GetStringField(TEXT("public_ref")),
			Object->GetStringField(TEXT("read_port_id")),
			MoveTemp(Operations),
			Object->GetStringField(TEXT("binding_evidence")));
		if (!Binding.IsValid())
		{
			OutError = *Binding.GetValidationDiagnostic();
			LocateDiagnostic(OutError, InputPath);
			return false;
		}
		OutBinding.Emplace(MoveTemp(Binding));
		return true;
	}

	TSharedRef<FJsonObject>
	FUeAgentBindingTicketJsonCodec::BindingToJson(
		const Discovery::FUeAgentCandidateBinding& Binding)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(
			TEXT("binding_id"),
			Binding.GetBindingId());
		Object->SetObjectField(
			TEXT("summary"),
			SummaryToJson(Binding.GetSummary()));
		Object->SetObjectField(
			TEXT("catalog_revision"),
			CatalogRevisionToJson(Binding.GetCatalogRevision()));
		Object->SetStringField(
			TEXT("public_ref"),
			Binding.GetPublicRef());
		Object->SetStringField(
			TEXT("read_port_id"),
			Binding.GetReadPortId());
		TArray<TSharedPtr<FJsonValue>> Operations;
		for (const Discovery::EUeAgentCandidateOperation Operation :
			Binding.GetSupportedOperations())
		{
			Operations.Add(
				MakeShared<FJsonValueString>(
					OperationName(Operation)));
		}
		Object->SetArrayField(
			TEXT("supported_operations"),
			MoveTemp(Operations));
		Object->SetStringField(
			TEXT("binding_evidence"),
			Binding.GetBindingEvidence());
		return Object;
	}

	bool FUeAgentBindingTicketJsonCodec::TryParseTicket(
		const TSharedPtr<FJsonObject>& Object,
		TOptional<FUeAgentOperationTicket>& OutTicket,
		FUeAgentBindingTicketJsonError& OutError, const FString& InputPath)
	{
		OutTicket.Reset();
		OutError = {};
		if (!RequireFields(Object, InputPath, {TEXT("ticket_id"), TEXT("candidate_binding_id"), TEXT("candidate_fingerprint"), TEXT("server_session_id"), TEXT("registry_hash"), TEXT("context_fingerprint"), TEXT("signature")}, {TEXT("registry_epoch"), TEXT("issued_at"), TEXT("expires_at")}, {TEXT("descriptor"), TEXT("candidate_identity"), TEXT("catalog_revision")}, {}, OutError))
		{
			return false;
		}
		TOptional<Contract::FUeAgentDescriptorIdentity> Descriptor;
		TOptional<StateSemantic::FUeAgentCanonicalIdentity> Identity;
		TOptional<Discovery::FUeAgentCatalogRevision> CatalogRevision;
		if (!TryParseDescriptor(
				Object->GetObjectField(TEXT("descriptor")),
				Descriptor,
				OutError, InputPath + TEXT(".descriptor"))
			|| !TryParseIdentity(
				Object->GetObjectField(TEXT("candidate_identity")),
				Identity,
				OutError, InputPath + TEXT(".candidate_identity"))
			|| !TryParseCatalogRevision(
				Object->GetObjectField(TEXT("catalog_revision")),
				CatalogRevision,
				OutError, InputPath + TEXT(".catalog_revision")))
		{
			return false;
		}
		TOptional<StateSemantic::FUeAgentStateRevision>
			TargetRevision;
		if (Object->HasTypedField<EJson::Object>(
				TEXT("target_revision"))
			&& !TryParseStateRevision(
				Object->GetObjectField(TEXT("target_revision")),
				TargetRevision,
				OutError, InputPath + TEXT(".target_revision")))
		{
			return false;
		}
		FUeAgentOperationTicket Ticket(
			Object->GetStringField(TEXT("ticket_id")),
			Descriptor.GetValue(),
			Identity.GetValue(),
			Object->GetStringField(TEXT("candidate_binding_id")),
			Object->GetStringField(TEXT("candidate_fingerprint")),
			CatalogRevision.GetValue(),
			Object->GetStringField(TEXT("server_session_id")),
			static_cast<int64>(
				Object->GetNumberField(TEXT("registry_epoch"))),
			Object->GetStringField(TEXT("registry_hash")),
			Object->GetStringField(TEXT("context_fingerprint")),
			Object->GetStringField(TEXT("signature")),
			Object->GetNumberField(TEXT("issued_at")),
			Object->GetNumberField(TEXT("expires_at")),
			MoveTemp(TargetRevision));
		if (!Ticket.IsValid())
		{
			OutError = Ticket.GetValidationDiagnostic();
			LocateDiagnostic(OutError, InputPath);
			return false;
		}
		OutTicket.Emplace(MoveTemp(Ticket));
		return true;
	}

	TSharedRef<FJsonObject>
	FUeAgentBindingTicketJsonCodec::TicketToJson(
		const FUeAgentOperationTicket& Ticket)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("ticket_id"), Ticket.GetTicketId());
		Object->SetObjectField(
			TEXT("descriptor"),
			DescriptorToJson(Ticket.GetDescriptorIdentity()));
		Object->SetObjectField(
			TEXT("candidate_identity"),
			IdentityToJson(Ticket.GetCandidateIdentity()));
		Object->SetStringField(
			TEXT("candidate_binding_id"),
			Ticket.GetCandidateBindingId());
		Object->SetStringField(
			TEXT("candidate_fingerprint"),
			Ticket.GetCandidateFingerprint());
		Object->SetObjectField(
			TEXT("catalog_revision"),
			CatalogRevisionToJson(Ticket.GetCatalogRevision()));
		Object->SetStringField(
			TEXT("server_session_id"),
			Ticket.GetServerSessionId());
		Object->SetNumberField(
			TEXT("registry_epoch"),
			static_cast<double>(Ticket.GetRegistryEpoch()));
		Object->SetStringField(
			TEXT("registry_hash"),
			Ticket.GetRegistryHash());
		Object->SetStringField(
			TEXT("context_fingerprint"),
			Ticket.GetContextFingerprint());
		Object->SetStringField(
			TEXT("signature"),
			Ticket.GetSignature());
		Object->SetNumberField(
			TEXT("issued_at"),
			Ticket.GetIssuedAt());
		Object->SetNumberField(
			TEXT("expires_at"),
			Ticket.GetExpiresAt());
		if (Ticket.GetTargetRevision().IsSet())
		{
			Object->SetObjectField(
				TEXT("target_revision"),
				StateRevisionToJson(
					Ticket.GetTargetRevision().GetValue()));
		}
		else
		{
			Object->SetField(
				TEXT("target_revision"),
				MakeShared<FJsonValueNull>());
		}
		return Object;
	}
}
