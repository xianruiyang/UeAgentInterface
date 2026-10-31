#include "CommandEdge/UeAgentCommandParameterValidation.h"
#include "Core/Contract/Command/UeAgentCommandContractModel.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::CommandEdge
{
bool IsCommandWireValue(const FString& WireType, bool bNullable, const TSharedPtr<FJsonValue>& Value)
{
    if (!Value) return false;
    if (Value->Type == EJson::Null) return bNullable;
    if (WireType == TEXT("any")) return true;
    if (WireType == TEXT("boolean")) return Value->Type == EJson::Boolean;
    if (WireType == TEXT("integer"))
    {
        FString Lexeme; int64 Integer = 0;
        return StateSemantic::CanonicalJson::TryGetNumberLexeme(*Value, Lexeme)
            && StateSemantic::TryParseInt64Number(Lexeme, Integer);
    }
    if (WireType == TEXT("number")) return Value->Type == EJson::Number;
    if (WireType == TEXT("string")) return Value->Type == EJson::String;
    if (WireType == TEXT("object")) return Value->Type == EJson::Object;
    if (WireType == TEXT("array")) return Value->Type == EJson::Array;
    return false;
}
bool TryValidateCommandParameters(const CommandContract::FUeAgentFrozenCommandContract& Command,
    const TSharedPtr<FJsonObject>& Params, Contract::FDiagnostic& Error)
{
    Error.Reset();
    auto Fail = [&](const TCHAR* Code, const FString& Message, const FString& Name,
        const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual)
    {
        Error = Contract::FDiagnostic::Failure(Code, Message, TEXT("command_protocol"));
        Error.CandidateName = Command.GetCommandId();
        Error.SourceKind = TEXT("command_input");
        Error.PropertyPath = Name;
        Error.Source.JsonPath = TEXT("/params") + (Name.IsEmpty() ? FString() : TEXT("/") + Name.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
        Error.ExpectedValue = Expected; Error.ActualValue = Actual;
        Error.ExpectedValuePresent = Expected.IsValid(); Error.ActualValuePresent = Actual.IsValid();
        return false;
    };
    if (!Params) return Fail(TEXT("command_params_invalid"), TEXT("Command params must be an object."), {}, MakeShared<FJsonValueString>(TEXT("object")), nullptr);
    TSet<FString> Names;
    for (const auto& Field : Command.GetInputFields())
    {
        Names.Add(Field.Name);
        const auto Value = Params->TryGetField(Field.Name);
        const FString Expected = Field.WireType + (Field.bNullable ? TEXT(" or null") : TEXT(""));
        if (!Value)
        {
            if (Field.bRequired) return Fail(TEXT("command_param_required"), TEXT("Required parameter '") + Field.Name + TEXT("' is missing; expected ") + Expected + TEXT("."), Field.Name, MakeShared<FJsonValueString>(Expected), nullptr);
        }
        else if (!IsCommandWireValue(Field.WireType, Field.bNullable, Value))
            return Fail(TEXT("command_param_type_invalid"), TEXT("Parameter '") + Field.Name + TEXT("' must be ") + Expected + TEXT("."), Field.Name, MakeShared<FJsonValueString>(Expected), Value);
    }
    for (const auto& Field : Params->Values)
        if (!Names.Contains(Field.Key)) return Fail(TEXT("command_param_unknown"), TEXT("Unknown parameter '") + Field.Key + TEXT("'."), Field.Key, nullptr, Field.Value);
    return true;
}
}
