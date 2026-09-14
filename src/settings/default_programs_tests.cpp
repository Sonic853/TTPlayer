#include "ttplayer/settings/file_association.h"
#include "ttplayer/settings/settings.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace ttplayer::settings;
namespace {
void Require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
std::wstring Read(const std::wstring& path, const wchar_t* name = nullptr) {
    wchar_t value[32768]{}; DWORD size = sizeof(value);
    const auto result = RegGetValueW(HKEY_CURRENT_USER,path.c_str(),name,RRF_RT_REG_SZ,
                                    nullptr,value,&size);
    return result == ERROR_SUCCESS ? value : L"";
}
void Write(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY key{};
    Require(RegCreateKeyExW(HKEY_CURRENT_USER,path.c_str(),0,nullptr,0,KEY_SET_VALUE,
        nullptr,&key,nullptr) == ERROR_SUCCESS,"create fixture registry key");
    const auto result = RegSetValueExW(key,name,0,REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)));
    RegCloseKey(key);
    Require(result == ERROR_SUCCESS,"write fixture registry value");
}
bool Exists(const std::wstring& path, const wchar_t* name) {
    DWORD size{};
    return RegGetValueW(HKEY_CURRENT_USER,path.c_str(),name,RRF_RT_ANY,nullptr,nullptr,&size) == ERROR_SUCCESS;
}
struct Fixture {
    std::wstring root = L"Software\\TTPlayerRebuild\\AssociationTests\\" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    ~Fixture() { RegDeleteTreeW(HKEY_CURRENT_USER,root.c_str()); }
};
}
int wmain() {
    try {
        struct SettingsFixture {
            std::filesystem::path path=std::filesystem::temp_directory_path() /
                (L"TTPlayer-association-reminder-"+std::to_wstring(GetCurrentProcessId())+L"-"+
                 std::to_wstring(GetTickCount64())+L".xml");
            ~SettingsFixture() { std::error_code error; std::filesystem::remove(path,error); }
        } setting_file;
        { std::ofstream fixture(setting_file.path); fixture << "<ttplayer><Player/></ttplayer>"; }
        auto preferences=LoadLegacyXml(setting_file.path);
        Require(!preferences.player.suppress_association_reminder,"legacy/default configurations must still show reminders");
        preferences.player.suppress_association_reminder=true;
        preferences.player.check_association=false;
        preferences.player.auto_associate=true;
        SaveWindowState(setting_file.path,preferences);
        auto restored=LoadLegacyXml(setting_file.path);
        Require(restored.player.suppress_association_reminder && !restored.player.check_association &&
            restored.player.auto_associate,"reminder preference roundtrip changed independent startup association options");
        preferences.player.suppress_association_reminder=false;
        SaveWindowState(setting_file.path,preferences);
        Require(!LoadLegacyXml(setting_file.path).player.suppress_association_reminder,"cannot persist re-enabled association reminders");
        Require(SelectDefaultAppsTarget(6,1,7601) == DefaultAppsTarget::control_panel,"Windows 7 route");
        Require(SelectDefaultAppsTarget(6,2,9200) == DefaultAppsTarget::control_panel,"Windows 8 route");
        Require(SelectDefaultAppsTarget(6,3,9600) == DefaultAppsTarget::control_panel,"Windows 8.1 route");
        Require(SelectDefaultAppsTarget(10,0,19045) == DefaultAppsTarget::settings,"Windows 10 route");
        Require(SelectDefaultAppsTarget(10,0,22000,1816) == DefaultAppsTarget::settings,"early Windows 11 route");
        Require(SelectDefaultAppsTarget(10,0,22000,1817) == DefaultAppsTarget::application_settings,"Windows 11 21H2 update route");
        Require(SelectDefaultAppsTarget(10,0,22621,1555) == DefaultAppsTarget::application_settings,"Windows 11 22H2 update route");
        Require(SelectDefaultAppsTarget(10,0,26100) == DefaultAppsTarget::application_settings,"Windows 11 24H2 route");
        Fixture fixture;
        FileAssociationBackendOptions options;
        options.current_user_classes_subkey = fixture.root + L"\\Classes";
        options.notify_shell = false;
        const auto classes = options.current_user_classes_subkey;
        const auto caps = classes + L"\\Registration\\Capabilities";
        const auto registered = classes + L"\\Registration\\RegisteredApplications";
        const std::filesystem::path exe = L"C:\\TTPlayer association fixture\\TTPlayerRebuild.exe";
        FileAssociationBackend backend(exe,L"TTPlayer test",options);
        Write(classes+L"\\.mp3",nullptr,L"Other.Audio");
        Write(classes+L"\\.mp3\\OpenWithProgids",L"Other.Audio",L"");
        Write(classes+L"\\Audio.mp3\\shell\\open\\command",nullptr,L"original.exe %1");
        Write(fixture.root+L"\\UserChoice",L"ProgId",L"Other.Audio");
        Write(fixture.root+L"\\UserChoice",L"Hash",L"unchanged");
        Require(!backend.QueryExtension(L"mp3").effective,"unregistered candidate must not appear effective");
        Require(!Exists(registered,L"TTPlayerRebuild"),"QueryExtension wrote a registration");
        Require(!backend.RegisterApplication({{L"bad;ext",L"",{}}}),"invalid catalog accepted");
        Require(!Exists(registered,L"TTPlayerRebuild"),"invalid catalog was partially registered");
        const std::vector<AssociableExtension> formats{{L"MP3",L"MPEG audio",{}},{L"mp3",L"duplicate",{}},{L"flac",L"FLAC audio",{}}};
        auto result = backend.RegisterApplication(formats);
        Require(result && result.changed,"register default-program candidate");
        Require(Read(registered,L"TTPlayerRebuild") == caps,"missing RegisteredApplications pointer");
        Require(Read(caps+L"\\FileAssociations",L".mp3") == L"TTPlayerRebuild.Audio.mp3","capabilities mapping mismatch");
        Require(Read(caps,L"ApplicationDescription").size() > 0,"Default Programs requires a description");
        Require(Read(classes+L"\\TTPlayerRebuild.Audio.mp3\\shell\\open\\command") ==
            L"\"C:\\TTPlayer association fixture\\TTPlayerRebuild.exe\" \"%1\"","command quoting mismatch");
        Require(Read(classes+L"\\.mp3") == L"Other.Audio","candidate registration changed default");
        Require(Read(classes+L"\\Audio.mp3\\shell\\open\\command") == L"original.exe %1","overwrote original player ProgID");
        Require(Exists(classes+L"\\.mp3\\OpenWithProgids",L"TTPlayerRebuild.Audio.mp3"),"Open With entry missing");
        result = backend.RegisterApplication(formats);
        Require(result && !result.changed,"candidate registration is not idempotent");
        result = backend.SetExtensionIcon(L"mp3",L"custom.ico,0");
        Require(result.Succeeded(),"candidate icon cannot be edited before becoming default");
        Require(backend.RegisterApplication(formats).Succeeded(),"refresh catalog");
        Require(Read(classes+L"\\TTPlayerRebuild.Audio.mp3\\DefaultIcon") == L"custom.ico,0","refresh discarded chosen icon");
        FileAssociationBackend stranger(L"C:\\other-install\\TTPlayerRebuild.exe",L"other",options);
        Require(!stranger.RegisterApplication(formats) && !stranger.UnregisterApplication(),"foreign installation ownership ignored");
        Require(!backend.OpenDefaultPrograms(nullptr),"isolated tests may not launch live Default Apps");
        Require(backend.UnregisterApplication().Succeeded(),"unregister private candidate");
        Require(!Exists(registered,L"TTPlayerRebuild"),"RegisteredApplications not removed");
        Require(!Exists(classes+L"\\.mp3\\OpenWithProgids",L"TTPlayerRebuild.Audio.mp3"),"Open With not removed");
        Require(Exists(classes+L"\\.mp3\\OpenWithProgids",L"Other.Audio"),"removed another Open With candidate");
        Require(Read(classes+L"\\.mp3") == L"Other.Audio" &&
            Read(fixture.root+L"\\UserChoice",L"Hash") == L"unchanged","cleanup changed defaults or protected choice");
        Require(backend.UnregisterApplication().Succeeded(),"unregister not idempotent");
        auto old_options = options;
        old_options.prog_id_prefix = L"Audio";
        FileAssociationBackend old_backend(exe,L"TTPlayer test",old_options);
        Require(old_backend.SetExtensionAssociation(L"mp3",true).Succeeded(),"create owned pre-Capabilities fixture");
        Require(Read(classes+L"\\.mp3") == L"Audio.mp3","legacy fixture not installed");
        Require(backend.UnregisterApplication(formats).Succeeded(),"remove pre-Capabilities owned registrations");
        Require(Read(classes+L"\\.mp3") == L"Other.Audio","legacy removal lost previous default");
        Require(Read(classes+L"\\Audio.mp3\\shell\\open\\command") == L"original.exe %1","legacy removal lost original command backup");
        Require(backend.UnregisterApplication(formats).Succeeded(),"legacy removal not idempotent");
        std::cout << "Default Programs: Win7/8/8.1/10/11 routing, isolated registration,\n"
                     "candidate-only writes, icon persistence, ownership and cleanup passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
