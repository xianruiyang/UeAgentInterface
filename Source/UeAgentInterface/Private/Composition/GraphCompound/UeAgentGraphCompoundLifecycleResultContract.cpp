#include "Composition/GraphCompound/UeAgentGraphCompoundLifecycleResultContract.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::Composition::GraphCompound::LifecycleResultContract
{
	namespace
	{
		TSharedRef<FJsonObject> StringSchema(const bool bNonEmpty)
		{
			const TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("string"));
			if (bNonEmpty)
			{
				Schema->SetNumberField(TEXT("minLength"), 1);
			}
			return Schema;
		}

		TSharedRef<FJsonObject> BooleanSchema()
		{
			const TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("boolean"));
			return Schema;
		}
	}

	FString BuildMutationLifecycleResultSchema(
		const bool bRequireIndependentReadbackEvidence)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), TEXT("object"));
		Root->SetBoolField(TEXT("additionalProperties"), false);

		const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
		const TSharedRef<FJsonObject> TerminalStatus = StringSchema(true);
		TerminalStatus->SetArrayField(TEXT("enum"), {
			MakeShared<FJsonValueString>(TEXT("succeeded"))});
		Properties->SetObjectField(TEXT("terminal_status"), TerminalStatus);
		Properties->SetObjectField(TEXT("target_ref"), StringSchema(true));
		Properties->SetObjectField(TEXT("revision"), StringSchema(true));
		Properties->SetObjectField(TEXT("mutation_applied"), BooleanSchema());
		Properties->SetObjectField(TEXT("compile_settled"), BooleanSchema());
		Properties->SetObjectField(TEXT("saved"), BooleanSchema());
		Properties->SetObjectField(TEXT("reloaded"), BooleanSchema());
		Properties->SetObjectField(
			TEXT("persistence_receipt_id"), StringSchema(true));

		TArray<TSharedPtr<FJsonValue>> Required = {
			MakeShared<FJsonValueString>(TEXT("terminal_status")),
			MakeShared<FJsonValueString>(TEXT("target_ref")),
			MakeShared<FJsonValueString>(TEXT("revision")),
			MakeShared<FJsonValueString>(TEXT("mutation_applied")),
			MakeShared<FJsonValueString>(TEXT("compile_settled")),
			MakeShared<FJsonValueString>(TEXT("saved")),
			MakeShared<FJsonValueString>(TEXT("reloaded")),
			MakeShared<FJsonValueString>(TEXT("persistence_receipt_id"))};
		if (bRequireIndependentReadbackEvidence)
		{
			Properties->SetObjectField(
				TEXT("independent_readback_evidence_id"), StringSchema(true));
			Required.Add(MakeShared<FJsonValueString>(
				TEXT("independent_readback_evidence_id")));
		}
		Root->SetObjectField(TEXT("properties"), Properties);
		Root->SetArrayField(TEXT("required"), MoveTemp(Required));

		FString Canonical;
		FString Error;
		return Contract::Json::TryCanonicalize(Root, Canonical, Error)
			? Canonical : FString();
	}
}
