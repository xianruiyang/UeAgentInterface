#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"
#include <initializer_list>

class FJsonObject;

namespace UeAgentAuthoringJsonFields
{
    using UE::UAI::Contract::FDiagnostic;
    struct FFieldType
    {
        const TCHAR* Name;
        EJson Type;
        bool bReferencePath = false;
    };

    /** Missing fields retain the owner's existing default/patch semantics. */
    UEAGENTINTERFACE_API bool ValidateOptionalFields(const FJsonObject& Object,
        std::initializer_list<FFieldType> Fields, FDiagnostic& OutError, const FString& Prefix = FString());

    /** Complete native records diagnose undeclared fields instead of ignoring them. */
    UEAGENTINTERFACE_API bool ValidateDeclaredFields(const FJsonObject& Object,
        std::initializer_list<FFieldType> Fields, FDiagnostic& OutError, const FString& Prefix = FString());

    /** Present references use the existing codec; null means clear, not absent. */
    UEAGENTINTERFACE_API bool ReadReferenceField(const FJsonObject& Object,
        const TCHAR* Field, FString& OutPath, FDiagnostic& OutError);
    UEAGENTINTERFACE_API bool ReadInt32Field(const FJsonObject& Object,
        const TCHAR* Field, int32& OutValue, FDiagnostic& OutError);
    UEAGENTINTERFACE_API bool ReadFloat32Field(const FJsonObject& Object,
        const TCHAR* Field, float& OutValue, FDiagnostic& OutError);
    UEAGENTINTERFACE_API bool ReadFloat64Field(const FJsonObject& Object,
        const TCHAR* Field, double& OutValue, FDiagnostic& OutError);
}
