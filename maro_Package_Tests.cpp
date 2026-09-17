#include "maro_CLive_Maro_Package.hpp"

namespace
{
class maro_TestServiceProvider :
    public ATL::CComObjectRootEx<ATL::CComSingleThreadModel>, public IServiceProvider
{
public:
    BEGIN_COM_MAP(maro_TestServiceProvider)
        COM_INTERFACE_ENTRY(IServiceProvider)
    END_COM_MAP()

    STDMETHOD(QueryService)(REFGUID, REFIID, void** maro_object) override
    {
        ++maro_queries;
        if (maro_object == nullptr) return E_POINTER;
        *maro_object = nullptr;
        return E_NOINTERFACE;
    }

    unsigned maro_queries = 0;
};
}

bool maro_TestDeferredCommands()
{
    const HRESULT maro_apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool maro_passed = false;
    {
        ATL::CComObject<maro_TestServiceProvider>* maro_rawProvider = nullptr;
        ATL::CComObject<Maro_CLive_Maro_Package>* maro_rawPackage = nullptr;
        if (SUCCEEDED(ATL::CComObject<maro_TestServiceProvider>::CreateInstance(&maro_rawProvider)))
        {
            ATL::CComPtr<maro_TestServiceProvider> maro_provider = maro_rawProvider;
            if (SUCCEEDED(ATL::CComObject<Maro_CLive_Maro_Package>::CreateInstance(&maro_rawPackage)))
            {
                ATL::CComPtr<Maro_CLive_Maro_Package> maro_package = maro_rawPackage;
                maro_passed = SUCCEEDED(maro_package->SetSite(maro_provider));
                for (int maro_round = 0; maro_round < 100; ++maro_round)
                {
                    for (DWORD maro_command = 256; maro_command <= 259; ++maro_command)
                    {
                        const HRESULT maro_result = maro_package->Exec(
                            &Maro_CLive_Maro_CommandSet, maro_command, 0, nullptr, nullptr);
                        maro_passed = SUCCEEDED(maro_result) && maro_passed;
                    }
                }
                OLECMD maro_update{Maro_CLive_Maro_CommandUpdate, 0};
                maro_passed = SUCCEEDED(maro_package->QueryStatus(
                    &Maro_CLive_Maro_CommandSet, 1, &maro_update, nullptr)) && maro_passed;
                maro_passed = maro_update.cmdf == OLECMDF_SUPPORTED && maro_provider->maro_queries == 0 &&
                    maro_package->Exec(&GUID_NULL, 256, 0, nullptr, nullptr) == OLECMDERR_E_UNKNOWNGROUP &&
                    maro_package->Exec(&Maro_CLive_Maro_CommandSet, 260, 0, nullptr, nullptr) == OLECMDERR_E_NOTSUPPORTED &&
                    maro_package->Exec(nullptr, 256, 0, nullptr, nullptr) == E_POINTER && maro_passed;
                maro_package->Close();
                maro_passed = maro_package->Exec(&Maro_CLive_Maro_CommandSet, 258, 0, nullptr, nullptr) ==
                    E_UNEXPECTED && maro_provider->maro_queries == 0 && maro_passed;
            }
        }
    }
    if (SUCCEEDED(maro_apartment)) CoUninitialize();
    return maro_passed;
}
