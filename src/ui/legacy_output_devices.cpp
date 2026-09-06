#include "legacy_output_devices.h"
#include "ttplayer/audio/asio_sink.h"
#include "ttplayer/audio/native_output_contract.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

#include <windows.h>
#include <cfgmgr32.h>
#include <mmsystem.h>
#include <dsound.h>
#include <objbase.h>
#include <winioctl.h>
#include <ks.h>
#include <ksmedia.h>
#include <setupapi.h>

namespace ttplayer::ui::detail {
namespace {

constexpr GUID kKsCategoryAudio{
    0x6994ad04, 0x93ef, 0x11d0,
    {0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96}};
constexpr GUID kKsCategoryRender{
    0x65e8773e, 0x8f56, 0x11d0,
    {0xa3, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96}};
constexpr GUID kKsPropertySetPin{
    0x8c134960, 0x51ad, 0x11cf,
    {0x87, 0x8a, 0x94, 0xf8, 0x01, 0xc1, 0x00, 0x00}};
constexpr GUID kKsPropertySetTopology{
    0x720d4ac0, 0x7533, 0x11d0,
    {0xa5, 0xd6, 0x28, 0xdb, 0x04, 0xc1, 0x00, 0x00}};
constexpr GUID kKsDataFormatTypeAudio{
    0x73647561, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kKsDataFormatSubtypePcm{
    0x00000001, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kKsDataFormatSpecifierWaveFormatEx{
    0x05589f81, 0xc356, 0x11ce,
    {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};
constexpr GUID kKsNodeTypeVolume{
    0x3a5acc00, 0xc557, 0x11d0,
    {0x8a, 0x2b, 0x00, 0xa0, 0xc9, 0x25, 0x5a, 0xc1}};
constexpr GUID kKsPinInterfaceStandardStreaming{
    0x1a8766a0, 0x62ce, 0x11cf,
    {0xa5, 0xd6, 0x28, 0xdb, 0x04, 0xc1, 0x00, 0x00}};
constexpr GUID kKsPinMediumStandardDevio{
    0x4747b320, 0x62ce, 0x11cf,
    {0xa5, 0xd6, 0x28, 0xdb, 0x04, 0xc1, 0x00, 0x00}};

constexpr std::uint32_t kOutputDeviceProbeMagic = 0x44505454; // TTPD
constexpr std::uint32_t kOutputDeviceProbeVersion = 1;
constexpr wchar_t kDefaultDirectSoundKey[] =
    L"{DEF00000-9C6D-47ED-AAF1-4DDA8F2B5C03}";

struct ScopedDeviceInfoSet {
    HDEVINFO value{INVALID_HANDLE_VALUE};
    ~ScopedDeviceInfoSet() {
        if (value != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(value);
    }
    ScopedDeviceInfoSet(const ScopedDeviceInfoSet&) = delete;
    ScopedDeviceInfoSet& operator=(const ScopedDeviceInfoSet&) = delete;
    ScopedDeviceInfoSet() = default;
};

struct ScopedHandle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~ScopedHandle() {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle() = default;
};

struct ScopedRegistryKey {
    HKEY value{};
    ~ScopedRegistryKey() {
        if (value) RegCloseKey(value);
    }
    ScopedRegistryKey(const ScopedRegistryKey&) = delete;
    ScopedRegistryKey& operator=(const ScopedRegistryKey&) = delete;
    ScopedRegistryKey() = default;
};

struct KsInterfaceRecord {
    std::wstring instance_id;
    std::wstring path;
    std::wstring name;
    std::wstring module;
};

bool ReadProbeBytes(HANDLE file, void* destination, DWORD bytes) {
    auto* cursor = static_cast<BYTE*>(destination);
    while (bytes != 0) {
        DWORD read{};
        if (!ReadFile(file, cursor, bytes, &read, nullptr) || read == 0)
            return false;
        cursor += read;
        bytes -= read;
    }
    return true;
}

bool WriteProbeBytes(HANDLE file, const void* source, DWORD bytes) {
    const auto* cursor = static_cast<const BYTE*>(source);
    while (bytes != 0) {
        DWORD written{};
        if (!WriteFile(file, cursor, bytes, &written, nullptr) || written == 0)
            return false;
        cursor += written;
        bytes -= written;
    }
    return true;
}

bool ReadProbeString(HANDLE file, std::wstring& value) {
    std::uint32_t size{};
    if (!ReadProbeBytes(file, &size, sizeof(size)) || size > 32768)
        return false;
    value.resize(size);
    return size == 0 || ReadProbeBytes(
        file, value.data(), size * static_cast<DWORD>(sizeof(wchar_t)));
}

bool WriteProbeString(HANDLE file, std::wstring_view value) {
    if (value.size() > 32768) return false;
    const auto size = static_cast<std::uint32_t>(value.size());
    return WriteProbeBytes(file, &size, sizeof(size)) &&
           (size == 0 || WriteProbeBytes(
               file, value.data(), size * static_cast<DWORD>(sizeof(wchar_t))));
}

std::wstring FoldCase(std::wstring text) {
    if (!text.empty()) CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

std::wstring GuidString(const GUID& value) {
    std::array<wchar_t, 64> buffer{};
    return StringFromGUID2(value, buffer.data(),
                           static_cast<int>(buffer.size())) > 0
        ? std::wstring(buffer.data()) : std::wstring{};
}

BOOL CALLBACK CollectDirectSoundOutputDevice(
    LPGUID identifier, LPCWSTR description, LPCWSTR module, LPVOID context) {
    auto* devices = static_cast<std::vector<LegacyOutputDevice>*>(context);
    if (!devices || !description) return TRUE;

    LegacyOutputDevice entry;
    entry.backend = 1;
    entry.name = description;
    if (module) entry.module = module;
    if (identifier) {
        entry.class_id = *identifier;
        entry.has_class_id = true;
        entry.key = GuidString(*identifier);
    } else {
        entry.key = kDefaultDirectSoundKey;
    }
    // FUN_0049943C only appends the callback name and copies its GUID. The
    // selected row is opened later by FUN_0049989D -> FUN_004C3ABE. Opening
    // every endpoint here lets one bad driver erase the whole catalogue when
    // the isolated discovery helper reaches its deadline.
    devices->push_back(std::move(entry));
    return TRUE;
}

std::wstring BackendKey(int backend, size_t ordinal) {
    GUID key{};
    key.Data1 = (static_cast<DWORD>(backend) << 16) |
                (static_cast<DWORD>(ordinal) & 0xffffu);
    return GuidString(key);
}

std::wstring QueryRegistryString(HKEY key, const wchar_t* value_name) {
    DWORD type{};
    DWORD bytes{};
    if (RegQueryValueExW(key, value_name, nullptr, &type, nullptr, &bytes) !=
            ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
        return {};
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1);
    if (RegQueryValueExW(key, value_name, nullptr, &type,
            reinterpret_cast<LPBYTE>(buffer.data()), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    buffer.back() = L'\0';
    std::wstring value(buffer.data());
    if (type != REG_EXPAND_SZ || value.empty()) return value;

    const DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (required == 0) return value;
    std::vector<wchar_t> expanded(required);
    if (ExpandEnvironmentStringsW(value.c_str(), expanded.data(), required) == 0)
        return value;
    return std::wstring(expanded.data());
}

std::wstring QueryDeviceProperty(HDEVINFO set, SP_DEVINFO_DATA& device,
                                 DWORD property) {
    DWORD type{};
    DWORD required{};
    SetupDiGetDeviceRegistryPropertyW(set, &device, property, &type, nullptr,
                                      0, &required);
    if (required < sizeof(wchar_t)) return {};
    std::vector<BYTE> buffer(required + sizeof(wchar_t), 0);
    if (!SetupDiGetDeviceRegistryPropertyW(set, &device, property, &type,
                                           buffer.data(), required, nullptr) ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return {};
    }
    return reinterpret_cast<const wchar_t*>(buffer.data());
}

std::wstring QueryInterfaceFriendlyName(HDEVINFO set,
                                         SP_DEVICE_INTERFACE_DATA& interface_data) {
    ScopedRegistryKey key;
    key.value = SetupDiOpenDeviceInterfaceRegKey(
        set, &interface_data, 0, KEY_QUERY_VALUE);
    if (key.value == INVALID_HANDLE_VALUE) {
        key.value = nullptr;
        return {};
    }
    return QueryRegistryString(key.value, L"FriendlyName");
}

std::vector<KsInterfaceRecord> EnumerateKsInterfaces(
    const GUID& category, const GUID* required_alias = nullptr) {
    std::vector<KsInterfaceRecord> result;
    ScopedDeviceInfoSet set;
    set.value = SetupDiGetClassDevsW(
        &category, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set.value == INVALID_HANDLE_VALUE) return result;

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(set.value, nullptr, &category, index,
                                         &interface_data)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            continue;
        }

        // FUN_004E0029 explicitly resolves
        // SetupDiGetDeviceInterfaceAlias, and FUN_004F0CB0 walks the first
        // category then requires an alias for each following category.  An
        // instance-ID intersection is weaker: modern SysAudio endpoints can
        // expose separate AUDIO and RENDER interfaces on one devnode without
        // those interfaces being aliases.
        SP_DEVICE_INTERFACE_DATA selected_interface = interface_data;
        if (required_alias) {
            SP_DEVICE_INTERFACE_DATA alias{};
            alias.cbSize = sizeof(alias);
            if (!SetupDiGetDeviceInterfaceAlias(
                    set.value, &interface_data, required_alias, &alias)) {
                continue;
            }
            selected_interface = alias;
        }

        DWORD required{};
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        SetupDiGetDeviceInterfaceDetailW(
            set.value, &selected_interface, nullptr, 0, &required, &device);
        if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<BYTE> detail_storage(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
            detail_storage.data());
        detail->cbSize = sizeof(*detail);
        device.cbSize = sizeof(device);
        if (!SetupDiGetDeviceInterfaceDetailW(
                set.value, &selected_interface, detail, required, nullptr,
                &device)) {
            continue;
        }

        std::array<wchar_t, MAX_DEVICE_ID_LEN> instance{};
        if (!SetupDiGetDeviceInstanceIdW(set.value, &device, instance.data(),
                                         static_cast<DWORD>(instance.size()),
                                         nullptr)) {
            continue;
        }

        KsInterfaceRecord item;
        item.instance_id = instance.data();
        item.path = detail->DevicePath;
        item.name = QueryInterfaceFriendlyName(set.value,
                                               selected_interface);
        if (item.name.empty())
            item.name = QueryDeviceProperty(set.value, device,
                                            SPDRP_FRIENDLYNAME);
        if (item.name.empty())
            item.name = QueryDeviceProperty(set.value, device, SPDRP_DEVICEDESC);
        item.module = QueryDeviceProperty(set.value, device, SPDRP_SERVICE);
        result.push_back(std::move(item));
    }
    return result;
}

constexpr DWORD kKsPropertyTimeoutMilliseconds = 1500;
constexpr ULONGLONG kKsCatalogTimeoutMilliseconds = 3000;

struct PendingKsPropertyIo {
    OVERLAPPED asynchronous{};
    HANDLE event{};
    std::vector<BYTE> request;
    std::vector<BYTE> output;

    ~PendingKsPropertyIo() {
        if (event) CloseHandle(event);
    }
};

DWORD WINAPI ReapTimedOutKsPropertyIo(void* parameter) {
    std::unique_ptr<PendingKsPropertyIo> pending(
        static_cast<PendingKsPropertyIo*>(parameter));
    // CancelIoEx only requests cancellation.  Keep OVERLAPPED, its event and
    // both driver buffers alive until the kernel reports final completion.
    static_cast<void>(WaitForSingleObject(pending->event, INFINITE));
    return 0;
}

void DeferTimedOutKsPropertyIo(
    std::unique_ptr<PendingKsPropertyIo> pending) {
    auto* raw = pending.release();
    const HANDLE reaper = CreateThread(nullptr, 0, ReapTimedOutKsPropertyIo,
                                       raw, 0, nullptr);
    if (reaper) {
        CloseHandle(reaper);
        return;
    }
    // A failed reaper allocation must not turn a timeout into use-after-free.
    // Deliberately retain this one small request until process exit; closing
    // the filter still asks the kernel to finish/cancel the outstanding I/O.
}

DWORD RemainingKsPropertyWait(ULONGLONG deadline) {
    if (deadline == 0) return kKsPropertyTimeoutMilliseconds;
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) return 0;
    return static_cast<DWORD>(std::min<ULONGLONG>(
        kKsPropertyTimeoutMilliseconds, deadline - now));
}

bool QueryKsProperty(HANDLE filter, const void* request, DWORD request_bytes,
                     void* output, DWORD output_bytes, DWORD* returned = nullptr,
                     ULONGLONG deadline = 0) {
    // FUN_004F28A8 opens KS filters with FILE_FLAG_OVERLAPPED.  Use the same
    // I/O contract and put a finite ceiling around a broken miniport: the
    // property sheet must not inherit an unbounded kernel-driver wait.
    const DWORD wait_milliseconds = RemainingKsPropertyWait(deadline);
    if (wait_milliseconds == 0 || !request || request_bytes == 0 ||
        (output_bytes != 0 && !output)) {
        if (returned) *returned = 0;
        SetLastError(wait_milliseconds == 0 ? ERROR_TIMEOUT
                                             : ERROR_INVALID_PARAMETER);
        return false;
    }

    auto pending = std::make_unique<PendingKsPropertyIo>();
    pending->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!pending->event) return false;
    pending->asynchronous.hEvent = pending->event;
    const auto* request_begin = static_cast<const BYTE*>(request);
    pending->request.assign(request_begin, request_begin + request_bytes);
    pending->output.resize(output_bytes);

    const auto copy_output = [&](DWORD bytes) {
        if (!output || pending->output.empty() || bytes == 0) return;
        std::memcpy(output, pending->output.data(),
                    std::min<size_t>(bytes, pending->output.size()));
    };
    DWORD transferred{};
    if (DeviceIoControl(
            filter, IOCTL_KS_PROPERTY, pending->request.data(), request_bytes,
            pending->output.empty() ? nullptr : pending->output.data(),
            output_bytes, &transferred, &pending->asynchronous)) {
        copy_output(transferred);
        if (returned) *returned = transferred;
        return true;
    }
    DWORD error = GetLastError();
    if (error != ERROR_IO_PENDING) {
        if (returned) *returned = transferred;
        SetLastError(error);
        return false;
    }
    if (WaitForSingleObject(pending->event, wait_milliseconds) !=
            WAIT_OBJECT_0) {
        CancelIoEx(filter, &pending->asynchronous);
        if (WaitForSingleObject(pending->event, 100) != WAIT_OBJECT_0)
            DeferTimedOutKsPropertyIo(std::move(pending));
        if (returned) *returned = 0;
        SetLastError(ERROR_TIMEOUT);
        return false;
    }
    const BOOL completed = GetOverlappedResult(
        filter, &pending->asynchronous, &transferred, FALSE);
    error = completed ? ERROR_SUCCESS : GetLastError();
    if (completed) copy_output(transferred);
    if (returned) *returned = transferred;
    if (!completed) SetLastError(error);
    return completed != FALSE;
}

template <typename T>
bool QueryKsPinValue(HANDLE filter, ULONG pin, ULONG property, T& output,
                     ULONGLONG deadline) {
    KSP_PIN request{};
    request.Property.Set = kKsPropertySetPin;
    request.Property.Id = property;
    request.Property.Flags = KSPROPERTY_TYPE_GET;
    request.PinId = pin;
    return QueryKsProperty(filter, &request, sizeof(request), &output,
                           sizeof(output), nullptr, deadline);
}

std::vector<BYTE> QueryKsPinBlob(HANDLE filter, ULONG pin, ULONG property,
                                 ULONGLONG deadline) {
    KSP_PIN request{};
    request.Property.Set = kKsPropertySetPin;
    request.Property.Id = property;
    request.Property.Flags = KSPROPERTY_TYPE_GET;
    request.PinId = pin;

    // KS miniports commonly return ERROR_MORE_DATA with the exact byte count,
    // but a few older WDM drivers do not.  Bounded growth keeps discovery from
    // allocating an untrusted driver-reported size without limit.
    DWORD bytes{};
    QueryKsProperty(filter, &request, sizeof(request), nullptr, 0, &bytes,
                    deadline);
    size_t capacity = std::max<size_t>(bytes, 4096);
    constexpr size_t kMaximumPropertyBytes = 1024 * 1024;
    while (capacity <= kMaximumPropertyBytes) {
        std::vector<BYTE> result(capacity);
        DWORD returned{};
        if (QueryKsProperty(filter, &request, sizeof(request), result.data(),
                            static_cast<DWORD>(result.size()), &returned,
                            deadline)) {
            if (returned > result.size()) return {};
            result.resize(returned);
            return result;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_MORE_DATA && error != ERROR_INSUFFICIENT_BUFFER)
            return {};
        if (returned > capacity && returned <= kMaximumPropertyBytes)
            capacity = returned;
        else
            capacity *= 2;
    }
    return {};
}

bool KsPinHasIdentifier(HANDLE filter, ULONG pin, ULONG property,
                        const GUID& expected, ULONGLONG deadline) {
    const auto blob = QueryKsPinBlob(filter, pin, property, deadline);
    if (blob.size() < sizeof(KSMULTIPLE_ITEM)) return false;
    const auto* multiple = reinterpret_cast<const KSMULTIPLE_ITEM*>(
        blob.data());
    const size_t available = blob.size() - sizeof(KSMULTIPLE_ITEM);
    if (multiple->Count > available / sizeof(KSIDENTIFIER)) return false;
    const auto* identifiers = reinterpret_cast<const KSIDENTIFIER*>(
        multiple + 1);
    for (ULONG index = 0; index < multiple->Count; ++index) {
        // FUN_004EFEBC compares the identifier Set and requires Id == 0;
        // Flags are intentionally not part of that acceptance test.
        if (IsEqualGUID(identifiers[index].Set, expected) &&
            identifiers[index].Id == 0) {
            return true;
        }
    }
    return false;
}

std::optional<bool> QueryKsHardwareVolume(HANDLE filter,
                                           ULONGLONG deadline) {
    KSPROPERTY request{};
    request.Set = kKsPropertySetTopology;
    request.Id = KSPROPERTY_TOPOLOGY_NODES;
    request.Flags = KSPROPERTY_TYPE_GET;
    std::vector<BYTE> data(64 * 1024);
    DWORD returned{};
    if (!QueryKsProperty(filter, &request, sizeof(request), data.data(),
                         static_cast<DWORD>(data.size()), &returned,
                         deadline) ||
        returned < sizeof(KSMULTIPLE_ITEM)) {
        return std::nullopt;
    }
    const auto* multiple = reinterpret_cast<const KSMULTIPLE_ITEM*>(data.data());
    const size_t available = returned - sizeof(KSMULTIPLE_ITEM);
    if (multiple->Count > available / sizeof(GUID)) return false;
    const auto* nodes = reinterpret_cast<const GUID*>(multiple + 1);
    for (ULONG index = 0; index < multiple->Count; ++index) {
        if (IsEqualGUID(nodes[index], kKsNodeTypeVolume)) return true;
    }
    return false;
}

bool QueryKsAudioCapabilities(std::wstring_view path,
                              std::array<std::wstring, 4>& details,
                              ULONGLONG deadline) {
    ScopedHandle filter;
    // Exact FUN_004F28A8 CreateFileW contract: GENERIC_READ|GENERIC_WRITE,
    // no sharing, OPEN_EXISTING and FILE_ATTRIBUTE_NORMAL|
    // FILE_FLAG_OVERLAPPED.  A permissive read-only/shared fallback admits
    // SysAudio endpoint aliases which the original rejects and visibly adds
    // duplicate devices to options page 260.
    filter.value = CreateFileW(std::wstring(path).c_str(),
        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (filter.value == INVALID_HANDLE_VALUE) return false;

    KSPROPERTY pin_count_request{};
    pin_count_request.Set = kKsPropertySetPin;
    pin_count_request.Id = KSPROPERTY_PIN_CTYPES;
    pin_count_request.Flags = KSPROPERTY_TYPE_GET;
    ULONG pin_count{};
    if (!QueryKsProperty(filter.value, &pin_count_request,
                         sizeof(pin_count_request), &pin_count,
                         sizeof(pin_count), nullptr, deadline) ||
        pin_count > 4096) {
        return false;
    }

    ULONG maximum_channels{};
    ULONG minimum_bits = std::numeric_limits<ULONG>::max();
    ULONG maximum_bits{};
    ULONG minimum_rate = std::numeric_limits<ULONG>::max();
    ULONG maximum_rate{};
    bool found_render_audio{};
    for (ULONG pin = 0; pin < pin_count; ++pin) {
        if (GetTickCount64() >= deadline) return false;
        KSPIN_DATAFLOW flow{};
        KSPIN_COMMUNICATION communication{};
        if (!QueryKsPinValue(filter.value, pin, KSPROPERTY_PIN_DATAFLOW, flow,
                             deadline) ||
            flow != KSPIN_DATAFLOW_IN ||
            !QueryKsPinValue(filter.value, pin,
                             KSPROPERTY_PIN_COMMUNICATION, communication,
                             deadline) ||
            communication != KSPIN_COMMUNICATION_SINK ||
            !KsPinHasIdentifier(filter.value, pin,
                                KSPROPERTY_PIN_INTERFACES,
                                kKsPinInterfaceStandardStreaming, deadline) ||
            !KsPinHasIdentifier(filter.value, pin,
                                KSPROPERTY_PIN_MEDIUMS,
                                kKsPinMediumStandardDevio, deadline)) {
            continue;
        }
        auto ranges = QueryKsPinBlob(filter.value, pin,
                                     KSPROPERTY_PIN_DATARANGES, deadline);
        if (ranges.size() < sizeof(KSMULTIPLE_ITEM)) continue;
        const auto* multiple = reinterpret_cast<const KSMULTIPLE_ITEM*>(
            ranges.data());
        size_t offset = sizeof(KSMULTIPLE_ITEM);
        for (ULONG range_index = 0; range_index < multiple->Count;
             ++range_index) {
            if (offset + sizeof(KSDATARANGE) > ranges.size()) break;
            const auto* range = reinterpret_cast<const KSDATARANGE*>(
                ranges.data() + offset);
            if (range->FormatSize < sizeof(KSDATARANGE) ||
                range->FormatSize > ranges.size() - offset) {
                break;
            }
            if (IsEqualGUID(range->MajorFormat, kKsDataFormatTypeAudio) &&
                IsEqualGUID(range->SubFormat, kKsDataFormatSubtypePcm) &&
                IsEqualGUID(range->Specifier,
                            kKsDataFormatSpecifierWaveFormatEx) &&
                range->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
                const auto* audio = reinterpret_cast<const KSDATARANGE_AUDIO*>(
                    range);
                found_render_audio = true;
                maximum_channels = std::max(maximum_channels,
                                             audio->MaximumChannels);
                minimum_bits = std::min(minimum_bits,
                                         audio->MinimumBitsPerSample);
                maximum_bits = std::max(maximum_bits,
                                         audio->MaximumBitsPerSample);
                minimum_rate = std::min(minimum_rate,
                                         audio->MinimumSampleFrequency);
                maximum_rate = std::max(maximum_rate,
                                         audio->MaximumSampleFrequency);
            }
            offset += range->FormatSize;
        }
    }
    if (!found_render_audio) return false;

    details[0] = std::to_wstring(maximum_channels);
    if (minimum_bits != std::numeric_limits<ULONG>::max()) {
        details[1] = minimum_bits == maximum_bits
            ? std::to_wstring(maximum_bits) + L" Bits"
            : std::to_wstring(minimum_bits) + L" - " +
                std::to_wstring(maximum_bits) + L" Bits";
    }
    if (minimum_rate != std::numeric_limits<ULONG>::max()) {
        details[2] = minimum_rate == maximum_rate
            ? std::to_wstring(maximum_rate) + L" Hz"
            : std::to_wstring(minimum_rate) + L" - " +
                std::to_wstring(maximum_rate) + L" Hz";
    }
    const auto hardware_volume = QueryKsHardwareVolume(filter.value, deadline);
    if (!hardware_volume) return false;
    details[3] = *hardware_volume ? L"1" : L"0";
    return true;
}

} // namespace

NativeOutputResolution ResolveLegacyNativeOutputDevice(
    std::wstring_view text, std::span<const LegacyOutputDevice> catalog) {
    using audio::OutputBackend;
    const auto key = audio::ParseOutputDeviceKey(text);
    if (!key) return {{}, NativeOutputResolutionError::invalid_key};
    if (key->backend != OutputBackend::kernel_streaming &&
        key->backend != OutputBackend::asio)
        return {{}, NativeOutputResolutionError::wrong_backend};

    const LegacyOutputDevice* found{};
    for (const auto& entry : catalog) {
        const auto entry_key = audio::ParseOutputDeviceKey(entry.key);
        if (!entry_key || !IsEqualGUID(entry_key->identifier, key->identifier))
            continue;
        if (found) return {{}, NativeOutputResolutionError::ambiguous_device};
        found = &entry;
    }
    if (!found) return {{}, NativeOutputResolutionError::missing_device};
    if (found->backend != static_cast<int>(key->backend) ||
        found->module.empty() ||
        found->module.find(L'\0') != std::wstring::npos ||
        (key->backend == OutputBackend::asio &&
         (!found->has_class_id || IsEqualGUID(found->class_id, GUID{}))))
        return {{}, NativeOutputResolutionError::invalid_descriptor};
    return {*found, NativeOutputResolutionError::none};
}

std::vector<LegacyOutputDevice> EnumerateKernelStreamingOutputDevices() {
    const auto render = EnumerateKsInterfaces(
        kKsCategoryAudio, &kKsCategoryRender);
    const ULONGLONG deadline = GetTickCount64() +
                               kKsCatalogTimeoutMilliseconds;

    std::vector<LegacyOutputDevice> result;
    std::set<std::wstring> published_paths;
    for (const auto& item : render) {
        if (GetTickCount64() >= deadline) break;
        if (!published_paths.insert(FoldCase(item.path)).second) {
            continue;
        }
        LegacyOutputDevice entry;
        entry.backend = 2;
        entry.name = item.name;
        entry.module = item.path;
        // A filter is published only after its render pin reports an audio
        // KSDATARANGE.  This is the public-API equivalent of the original
        // FUN_004f0cb0 Audio+Render category/pin intersection and prevents
        // topology/capture-only interfaces from becoming fake outputs.
        if (entry.name.empty() ||
            !QueryKsAudioCapabilities(item.path, entry.details, deadline)) {
            continue;
        }
        const size_t ordinal = result.size();
        if (ordinal > 0xffffu) break;
        entry.key = BackendKey(2, ordinal);
        result.push_back(std::move(entry));
    }
    return result;
}

std::vector<LegacyOutputDevice> EnumerateAsioOutputDevices() {
    std::vector<LegacyOutputDevice> result;
    ScopedRegistryKey asio_root;
    // FUN_004e1c87 is a 32-bit binary and opens exactly HKLM\SOFTWARE\ASIO
    // with KEY_READ.  Do not merge registry views or scan unrelated COM
    // classes: the process registry view is part of the legacy ABI.
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ,
                      &asio_root.value) != ERROR_SUCCESS) {
        return result;
    }

    for (DWORD index = 0;; ++index) {
        std::array<wchar_t, 256> subkey_name{};
        DWORD name_length = static_cast<DWORD>(subkey_name.size());
        const LSTATUS enumerated = RegEnumKeyExW(
            asio_root.value, index, subkey_name.data(), &name_length, nullptr,
            nullptr, nullptr, nullptr);
        if (enumerated == ERROR_NO_MORE_ITEMS) break;
        if (enumerated != ERROR_SUCCESS) continue;

        ScopedRegistryKey driver_key;
        if (RegOpenKeyExW(asio_root.value, subkey_name.data(), 0, KEY_READ,
                          &driver_key.value) != ERROR_SUCCESS) {
            continue;
        }
        const std::wstring clsid_text = QueryRegistryString(
            driver_key.value, L"CLSID");
        GUID class_id{};
        if (clsid_text.empty() ||
            FAILED(CLSIDFromString(clsid_text.c_str(), &class_id))) {
            continue;
        }
        const std::wstring canonical_clsid = GuidString(class_id);
        if (canonical_clsid.empty()) continue;

        ScopedRegistryKey server_key;
        const std::wstring server_path = L"CLSID\\" + canonical_clsid +
                                         L"\\InprocServer32";
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, server_path.c_str(), 0, KEY_READ,
                          &server_key.value) != ERROR_SUCCESS) {
            continue;
        }
        const std::wstring module = QueryRegistryString(server_key.value, L"");
        if (module.empty()) continue;

        const size_t ordinal = result.size();
        if (ordinal > 0xffffu) break;
        LegacyOutputDevice entry;
        entry.backend = 3;
        entry.key = BackendKey(3, ordinal);
        entry.name.assign(subkey_name.data(), name_length);
        entry.module = module;
        entry.class_id = class_id;
        entry.has_class_id = true;
        // FUN_004E1C87 stops at descriptor discovery. Do not activate an
        // arbitrary third-party DLL while populating the combo; details are
        // filled after selection, as in FUN_0049989D -> FUN_004E201C.
        result.push_back(std::move(entry));
    }
    return result;
}

bool PopulateLegacyOutputDeviceDetails(
    LegacyOutputDevice& device, std::wstring* diagnostic) {
    if (diagnostic) diagnostic->clear();
    std::array<std::wstring, 4> details;

    if (device.backend == 0) {
        WAVEOUTCAPSW capabilities{};
        const MMRESULT result = waveOutGetDevCapsW(
            device.wave_device_id, &capabilities, sizeof(capabilities));
        if (result != MMSYSERR_NOERROR) {
            if (diagnostic) *diagnostic = L"waveOutGetDevCapsW failed";
            return false;
        }
        details[0] = capabilities.szPname;
        if (capabilities.dwSupport != 0) {
            details[1] =
                (capabilities.dwSupport & WAVECAPS_VOLUME) != 0
                ? L"1" : L"0";
            details[2] =
                (capabilities.dwSupport & WAVECAPS_LRVOLUME) != 0
                ? L"1" : L"0";
            details[3] =
                (capabilities.dwSupport & WAVECAPS_SAMPLEACCURATE) != 0
                ? L"1" : L"0";
        }
    } else if (device.backend == 1) {
        if (device.has_class_id && IsEqualGUID(device.class_id, GUID{})) {
            if (diagnostic) *diagnostic = L"invalid DirectSound GUID";
            return false;
        }
        DSCAPS capabilities{};
        capabilities.dwSize = sizeof(capabilities);
        IDirectSound8* direct_sound{};
        const GUID* identifier = device.has_class_id
            ? &device.class_id : nullptr;
        HRESULT result = DirectSoundCreate8(identifier, &direct_sound, nullptr);
        if (SUCCEEDED(result) && direct_sound) {
            result = direct_sound->GetCaps(&capabilities);
        } else if (SUCCEEDED(result)) {
            result = E_FAIL;
        }
        if (direct_sound) direct_sound->Release();
        if (FAILED(result)) {
            if (diagnostic)
                *diagnostic = L"DirectSound capability query failed";
            return false;
        }
        details[0] =
            (capabilities.dwFlags & DSCAPS_CERTIFIED) != 0 ? L"1" : L"0";
        // Preserve the original FUN_0049989D spelling.
        details[1] =
            (capabilities.dwFlags & DSCAPS_EMULDRIVER) != 0
            ? L"Emluator" : L"Device";
        details[2] = std::to_wstring(
            capabilities.dwMinSecondarySampleRate) + L" - " +
            std::to_wstring(capabilities.dwMaxSecondarySampleRate) + L" Hz";
        details[3] = L"Free " + std::to_wstring(
            capabilities.dwFreeHwMixingAllBuffers) + L" / Max " +
            std::to_wstring(capabilities.dwMaxHwMixingAllBuffers);
    } else if (device.backend == 2) {
        if (device.module.empty() ||
            !QueryKsAudioCapabilities(device.module, details, 0)) {
            if (diagnostic)
                *diagnostic = L"Kernel Streaming capability query failed";
            return false;
        }
    } else if (device.backend == 3) {
        if (!device.has_class_id || IsEqualGUID(device.class_id, GUID{}) ||
            device.name.empty() || device.module.empty()) {
            if (diagnostic) *diagnostic = L"invalid ASIO descriptor";
            return false;
        }
        audio::AsioDriverCapabilities capabilities;
        std::wstring asio_diagnostic;
        if (!audio::ProbeAsioDriverCapabilities(
                device.name, device.module, device.class_id, capabilities,
                asio_diagnostic)) {
            if (diagnostic) *diagnostic = std::move(asio_diagnostic);
            // FUN_004E201C returns the descriptor even when FUN_004E207B
            // cannot activate it. FUN_0049989D consequently formats the
            // zero-initialized cache as 0 Channels / 0 Bits / 0 Samples
            // instead of removing the row or failing the whole catalogue.
        }
        details = FormatAsioOutputDeviceDetails(capabilities);
    } else {
        if (diagnostic) *diagnostic = L"unsupported output backend";
        return false;
    }

    device.details = std::move(details);
    return true;
}

std::array<std::wstring, 4> FormatAsioOutputDeviceDetails(
    const audio::AsioDriverCapabilities& capabilities) {
    std::array<std::wstring, 4> details;
    details[0] = std::to_wstring(capabilities.output_channels) + L" Channels";
    for (const long sample_rate : capabilities.supported_sample_rates) {
        details[1] += std::to_wstring(sample_rate);
        details[1] += L' ';
    }
    if (!details[1].empty()) details[1] += L"Hz";
    details[2] = std::to_wstring(capabilities.first_output_valid_bits) +
                 L" Bits";
    details[3] = std::to_wstring(capabilities.preferred_buffer_frames) +
                 L" Samples";
    return details;
}

std::vector<LegacyOutputDevice>
EnumerateWaveAndDirectSoundOutputDevices() {
    std::vector<LegacyOutputDevice> result;

    // FUN_004991F1 starts at UINT(-1): Wave Mapper first, followed by every
    // concrete waveOut device.  Store the raw support flags so the UI process
    // only localizes Yes/No and never has to re-enter a driver for details.
    const UINT wave_count = waveOutGetNumDevs();
    for (UINT ordinal = 0; ordinal <= wave_count; ++ordinal) {
        const UINT device_id = ordinal == 0 ? WAVE_MAPPER : ordinal - 1;
        WAVEOUTCAPSW capabilities{};
        if (waveOutGetDevCapsW(device_id, &capabilities,
                               sizeof(capabilities)) != MMSYSERR_NOERROR) {
            continue;
        }
        LegacyOutputDevice entry;
        entry.backend = 0;
        entry.wave_device_id = device_id;
        entry.key = BackendKey(0, ordinal);
        entry.name = capabilities.szPname;
        entry.details[0] = capabilities.szPname;
        if (capabilities.dwSupport != 0) {
            entry.details[1] =
                (capabilities.dwSupport & WAVECAPS_VOLUME) != 0
                ? L"1" : L"0";
            entry.details[2] =
                (capabilities.dwSupport & WAVECAPS_LRVOLUME) != 0
                ? L"1" : L"0";
            entry.details[3] =
                (capabilities.dwSupport & WAVECAPS_SAMPLEACCURATE) != 0
                ? L"1" : L"0";
        }
        result.push_back(std::move(entry));
    }

    static_cast<void>(DirectSoundEnumerateW(
        CollectDirectSoundOutputDevice, &result));

    return result;
}

std::vector<LegacyOutputDevice> EnumerateLegacyOutputDevices() {
    auto result = EnumerateWaveAndDirectSoundOutputDevices();

    auto append = [&result](std::vector<LegacyOutputDevice> devices) {
        result.insert(result.end(),
                      std::make_move_iterator(devices.begin()),
                      std::make_move_iterator(devices.end()));
    };
    append(EnumerateKernelStreamingOutputDevices());
    append(EnumerateAsioOutputDevices());
    return result;
}

bool WriteLegacyOutputDeviceProbe(
    const std::filesystem::path& output,
    const std::vector<LegacyOutputDevice>& devices) {
    const HANDLE file = CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const auto count = static_cast<std::uint32_t>(devices.size());
    bool good = devices.size() <= 4096 &&
                WriteProbeBytes(file, &kOutputDeviceProbeMagic,
                                sizeof(kOutputDeviceProbeMagic)) &&
                WriteProbeBytes(file, &kOutputDeviceProbeVersion,
                                sizeof(kOutputDeviceProbeVersion)) &&
                WriteProbeBytes(file, &count, sizeof(count));
    for (const auto& device : devices) {
        const auto backend = static_cast<std::int32_t>(device.backend);
        const auto wave_device = static_cast<std::uint32_t>(
            device.wave_device_id);
        const std::uint32_t has_class_id = device.has_class_id ? 1U : 0U;
        good = good &&
               WriteProbeBytes(file, &backend, sizeof(backend)) &&
               WriteProbeBytes(file, &wave_device, sizeof(wave_device)) &&
               WriteProbeBytes(file, &has_class_id, sizeof(has_class_id)) &&
               WriteProbeBytes(file, &device.class_id,
                               sizeof(device.class_id)) &&
               WriteProbeString(file, device.key) &&
               WriteProbeString(file, device.name) &&
               WriteProbeString(file, device.module);
        for (const auto& detail : device.details)
            good = good && WriteProbeString(file, detail);
    }
    if (good) good = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return good;
}

std::optional<std::vector<LegacyOutputDevice>>
ReadLegacyOutputDeviceProbe(const std::filesystem::path& input) {
    const HANDLE file = CreateFileW(input.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    std::uint32_t magic{}, version{}, count{};
    bool good = ReadProbeBytes(file, &magic, sizeof(magic)) &&
                ReadProbeBytes(file, &version, sizeof(version)) &&
                ReadProbeBytes(file, &count, sizeof(count)) &&
                magic == kOutputDeviceProbeMagic &&
                version == kOutputDeviceProbeVersion && count <= 4096;
    std::vector<LegacyOutputDevice> devices;
    if (good) devices.reserve(count);
    for (std::uint32_t index = 0; good && index < count; ++index) {
        LegacyOutputDevice device;
        std::int32_t backend{};
        std::uint32_t wave_device{}, has_class_id{};
        good = ReadProbeBytes(file, &backend, sizeof(backend)) &&
               ReadProbeBytes(file, &wave_device, sizeof(wave_device)) &&
               ReadProbeBytes(file, &has_class_id, sizeof(has_class_id)) &&
               ReadProbeBytes(file, &device.class_id,
                              sizeof(device.class_id)) &&
               backend >= 0 && backend <= 3 && has_class_id <= 1 &&
               ReadProbeString(file, device.key) &&
               ReadProbeString(file, device.name) &&
               ReadProbeString(file, device.module);
        for (auto& detail : device.details)
            good = good && ReadProbeString(file, detail);
        if (good) {
            device.backend = backend;
            device.wave_device_id = wave_device;
            device.has_class_id = has_class_id != 0;
            devices.push_back(std::move(device));
        }
    }
    CloseHandle(file);
    if (!good) return std::nullopt;
    // A helper snapshot supplies native paths/CLSIDs to the options catalog.
    // Do not publish a KS/ASIO row whose persisted identity cannot resolve
    // back to precisely that descriptor (including duplicate-key failures).
    for (const auto& device : devices) {
        if ((device.backend == 2 || device.backend == 3) &&
            !ResolveLegacyNativeOutputDevice(device.key, devices).device)
            return std::nullopt;
    }
    return devices;
}

} // namespace ttplayer::ui::detail
