#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::HjsonWorkspace
{
    class FWorkspaceError;

    /** One synchronous reference-selection consumer. Each addressed leaf keeps
     * its real byte/identity checks; the original directory binding is checked
     * once before accepting this selection and again by the source checkpoint.
     * Owns no contents, declarations, native objects or cross-request cache. */
    bool TryReadCapturedReferenceScope(TFunctionRef<bool()> Consumer, FWorkspaceError& Error);
}
