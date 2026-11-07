#pragma once

#include "iop_service.h"

namespace ps2x::iop::detail
{
    std::unique_ptr<IopService> createAdxRnaService(IopHost &host);
    std::unique_ptr<IopService> createPadManService(IopHost &host);
    std::unique_ptr<IopService> createCdvdfsvService(IopHost &host);
    std::unique_ptr<IopService> createDbcmanService(IopHost &host);
    std::unique_ptr<IopService> createFileioService(IopHost &host);
    std::unique_ptr<IopService> createIopHeapService(IopHost &host);
    std::unique_ptr<IopService> createLibSdService(IopHost &host);
    std::unique_ptr<IopService> createMcservService(IopHost &host);
    std::unique_ptr<IopService> createSmodService(IopHost &host);
    std::unique_ptr<IopService> createSndDrvService(IopHost &host);
}
