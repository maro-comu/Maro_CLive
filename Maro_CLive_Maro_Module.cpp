#include "Maro_CLive_Maro_Package.hpp"

#include <atlbase.h>
#include <atlcom.h>

class Maro_CLive_Maro_Module final : public ATL::CAtlDllModuleT<Maro_CLive_Maro_Module>
{
};

Maro_CLive_Maro_Module Maro_Module;

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved)
{
    return Maro_Module.DllMain(reason, reserved);
}

STDAPI DllCanUnloadNow()
{
    return Maro_Module.DllCanUnloadNow();
}

STDAPI DllGetClassObject(REFCLSID classId, REFIID interfaceId, void** object)
{
    return Maro_Module.DllGetClassObject(classId, interfaceId, object);
}

STDAPI DllRegisterServer()
{
    return S_OK;
}

STDAPI DllUnregisterServer()
{
    return S_OK;
}
