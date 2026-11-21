#include "guiding/ascom_mount.h"

#include <format>
#include <stdexcept>

#if defined(_WIN32) && defined(SELENEWATCH_WITH_ASCOM)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>

#include <map>
#include <vector>

namespace {

std::wstring wide(const std::string& text)
{
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

std::string narrow(const wchar_t* text)
{
    if (!text || !*text) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::string hresultText(HRESULT result)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(result), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text = length ? narrow(buffer) : std::string{};
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return std::format("0x{:08X}{}{}", static_cast<unsigned>(result), text.empty() ? "" : " ", text);
}

// A VARIANT that clears itself.
struct Variant
{
    VARIANT value;
    Variant() { VariantInit(&value); }
    ~Variant() { VariantClear(&value); }
    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;
};

// Joins a COM apartment for as long as it lives, on the thread that made it.
class ComApartment
{
public:
    explicit ComApartment(DWORD model)
    {
        const HRESULT result = CoInitializeEx(nullptr, model);
        // S_FALSE: already joined, still to be balanced. RPC_E_CHANGED_MODE:
        // the thread is in the other apartment, which works for these calls.
        joined_ = SUCCEEDED(result);
        if (FAILED(result) && result != RPC_E_CHANGED_MODE)
            throw std::runtime_error("COM could not be started on this thread: " + hresultText(result));
    }
    ~ComApartment()
    {
        if (joined_) CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool joined_{false};
};

// Late-bound calls on one IDispatch object, by member name.
class Dispatch
{
public:
    Dispatch(const std::string& progId, const char* what)
    {
        CLSID id{};
        HRESULT result = CLSIDFromProgID(wide(progId).c_str(), &id);
        if (FAILED(result))
            throw std::runtime_error(std::format("{} '{}' is not registered on this computer ({}).", what, progId, hresultText(result)));
        result = CoCreateInstance(id, nullptr, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, IID_IDispatch,
            reinterpret_cast<void**>(&object_));
        if (FAILED(result))
        {
            throw std::runtime_error(std::format("{} '{}' could not be started ({}). A 32-bit driver that loads into the "
                "program cannot be used by 64-bit SeleneWatch; use a driver that runs as its own process, or ASCOM Remote.",
                what, progId, hresultText(result)));
        }
    }
    ~Dispatch()
    {
        if (object_) object_->Release();
    }
    Dispatch(const Dispatch&) = delete;
    Dispatch& operator=(const Dispatch&) = delete;

    void get(const wchar_t* name, Variant& result)
    {
        DISPPARAMS parameters{};
        invoke(name, DISPATCH_PROPERTYGET, parameters, &result.value);
    }

    bool getBool(const wchar_t* name)
    {
        Variant value;
        get(name, value);
        convert(value, VT_BOOL, name);
        return value.value.boolVal != VARIANT_FALSE;
    }

    int getInt(const wchar_t* name)
    {
        Variant value;
        get(name, value);
        convert(value, VT_I4, name);
        return value.value.lVal;
    }

    double getDouble(const wchar_t* name)
    {
        Variant value;
        get(name, value);
        convert(value, VT_R8, name);
        return value.value.dblVal;
    }

    // Takes ownership of value's contents.
    void put(const wchar_t* name, Variant& value)
    {
        DISPID named = DISPID_PROPERTYPUT;
        DISPPARAMS parameters{&value.value, &named, 1, 1};
        invoke(name, DISPATCH_PROPERTYPUT, parameters, nullptr);
    }

    // Arguments in their natural order; IDispatch wants them reversed.
    void call(const wchar_t* name, std::vector<VARIANT> arguments, Variant* result = nullptr)
    {
        std::vector<VARIANT> reversed(arguments.rbegin(), arguments.rend());
        DISPPARAMS parameters{reversed.data(), nullptr, static_cast<UINT>(reversed.size()), 0};
        try
        {
            invoke(name, DISPATCH_METHOD, parameters, result ? &result->value : nullptr);
        }
        catch (...)
        {
            for (VARIANT& argument : reversed) VariantClear(&argument);
            throw;
        }
        for (VARIANT& argument : reversed) VariantClear(&argument);
    }

private:
    DISPID id(const wchar_t* name)
    {
        if (const auto found = ids_.find(name); found != ids_.end()) return found->second;
        DISPID id = DISPID_UNKNOWN;
        LPOLESTR names[] = {const_cast<LPOLESTR>(name)};
        const HRESULT result = object_->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id);
        if (FAILED(result))
            throw std::runtime_error(std::format("The driver has no member {} ({}).", narrow(name), hresultText(result)));
        ids_.emplace(name, id);
        return id;
    }

    void invoke(const wchar_t* name, WORD flags, DISPPARAMS& parameters, VARIANT* result)
    {
        EXCEPINFO exception{};
        UINT argumentError = 0;
        const HRESULT outcome = object_->Invoke(id(name), IID_NULL, LOCALE_USER_DEFAULT, flags, &parameters, result, &exception, &argumentError);
        if (SUCCEEDED(outcome)) return;
        std::string reason = hresultText(outcome);
        if (outcome == DISP_E_EXCEPTION)
        {
            if (exception.pfnDeferredFillIn) exception.pfnDeferredFillIn(&exception);
            if (exception.bstrDescription) reason = narrow(exception.bstrDescription);
            if (exception.scode) reason += std::format(" (0x{:08X})", static_cast<unsigned>(exception.scode));
            SysFreeString(exception.bstrSource);
            SysFreeString(exception.bstrDescription);
            SysFreeString(exception.bstrHelpFile);
        }
        throw std::runtime_error(std::format("{}: {}", narrow(name), reason));
    }

    static void convert(Variant& value, VARTYPE type, const wchar_t* name)
    {
        const HRESULT result = VariantChangeType(&value.value, &value.value, 0, type);
        if (FAILED(result)) throw std::runtime_error(std::format("{} returned a value of an unexpected type.", narrow(name)));
    }

    IDispatch* object_{nullptr};
    std::map<std::wstring, DISPID> ids_;
};

VARIANT boolVariant(bool value)
{
    VARIANT out;
    VariantInit(&out);
    out.vt = VT_BOOL;
    out.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    return out;
}

VARIANT intVariant(int value)
{
    VARIANT out;
    VariantInit(&out);
    out.vt = VT_I4;
    out.lVal = value;
    return out;
}

VARIANT stringVariant(const std::string& value)
{
    VARIANT out;
    VariantInit(&out);
    out.vt = VT_BSTR;
    out.bstrVal = SysAllocString(wide(value).c_str());
    return out;
}

} // namespace

struct AscomMount::Driver
{
    explicit Driver(const std::string& progId)
        : apartment(COINIT_MULTITHREADED)
        , telescope(progId, "The ASCOM telescope driver")
    {
    }

    // Declared first: joined before the driver is made, left after it is released.
    ComApartment apartment;
    Dispatch telescope;
    // Read once at connect: a capability does not change while connected.
    bool canSetTracking{false};
};

AscomMount::AscomMount(std::string progId)
    : progId_(std::move(progId))
    , driver_(std::make_unique<Driver>(progId_))
{
}

AscomMount::~AscomMount() = default;

void AscomMount::connect()
{
    Variant on;
    on.value = boolVariant(true);
    driver_->telescope.put(L"Connected", on);
    if (!driver_->telescope.getBool(L"Connected"))
        throw std::runtime_error("The driver did not connect. Check the mount's cable, power, and driver settings.");
    try
    {
        driver_->canSetTracking = driver_->telescope.getBool(L"CanSetTracking");
    }
    catch (const std::runtime_error&)
    {
        driver_->canSetTracking = false;
    }
}

void AscomMount::disconnect()
{
    Variant off;
    off.value = boolVariant(false);
    driver_->telescope.put(L"Connected", off);
}

MountState AscomMount::state()
{
    Dispatch& telescope = driver_->telescope;
    MountState state;
    state.connected = telescope.getBool(L"Connected");
    if (!state.connected) return state;
    state.canPulseGuide = telescope.getBool(L"CanPulseGuide");
    state.canSetTracking = driver_->canSetTracking;
    state.tracking = telescope.getBool(L"Tracking");
    state.slewing = telescope.getBool(L"Slewing");
    state.parked = telescope.getBool(L"AtPark");
    if (state.canPulseGuide) state.pulseGuiding = telescope.getBool(L"IsPulseGuiding");
    // Optional members: a driver that does not implement one leaves it unknown.
    try
    {
        const int side = telescope.getInt(L"SideOfPier");
        state.pierSide = side == 0 ? PierSide::East : side == 1 ? PierSide::West : PierSide::Unknown;
    }
    catch (const std::runtime_error&)
    {
    }
    try
    {
        state.altAzimuth = telescope.getInt(L"AlignmentMode") == 0;
    }
    catch (const std::runtime_error&)
    {
    }
    try
    {
        state.guideRateRa = telescope.getDouble(L"GuideRateRightAscension");
    }
    catch (const std::runtime_error&)
    {
    }
    try
    {
        state.guideRateDec = telescope.getDouble(L"GuideRateDeclination");
    }
    catch (const std::runtime_error&)
    {
    }
    return state;
}

void AscomMount::pulseGuide(GuideDirection direction, int milliseconds)
{
    driver_->telescope.call(L"PulseGuide", {intVariant(static_cast<int>(direction)), intVariant(milliseconds)});
}

void AscomMount::stopTracking()
{
    Variant off;
    off.value = boolVariant(false);
    driver_->telescope.put(L"Tracking", off);
}

bool ascomAvailable()
{
    CLSID id{};
    return SUCCEEDED(CLSIDFromProgID(L"ASCOM.Utilities.Chooser", &id));
}

std::optional<std::string> chooseAscomTelescope(const std::string& current, std::string& error)
{
    try
    {
        const ComApartment apartment(COINIT_APARTMENTTHREADED);
        Dispatch chooser("ASCOM.Utilities.Chooser", "The ASCOM chooser");
        Variant type;
        type.value = stringVariant("Telescope");
        chooser.put(L"DeviceType", type);
        Variant chosen;
        chooser.call(L"Choose", {stringVariant(current)}, &chosen);
        if (chosen.value.vt != VT_BSTR) return std::string{};
        return narrow(chosen.value.bstrVal);
    }
    catch (const std::exception& failure)
    {
        error = failure.what();
        return std::nullopt;
    }
}

#else

struct AscomMount::Driver
{
};

AscomMount::AscomMount(std::string progId)
    : progId_(std::move(progId))
{
    throw std::runtime_error("This build of SeleneWatch has no ASCOM adapter.");
}

AscomMount::~AscomMount() = default;
void AscomMount::connect() {}
void AscomMount::disconnect() {}
MountState AscomMount::state() { return {}; }
void AscomMount::pulseGuide(GuideDirection, int) {}
void AscomMount::stopTracking() {}

bool ascomAvailable()
{
    return false;
}

std::optional<std::string> chooseAscomTelescope(const std::string&, std::string& error)
{
    error = "This build of SeleneWatch has no ASCOM adapter.";
    return std::nullopt;
}

#endif
