#include "dfx_registry_helper.h"
#include "ttplayer/core/text.h"
#include "ttplayer/update/update.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace ttplayer::audio::detail {
namespace {
constexpr size_t kLimit = 1024 * 1024;
struct Api { const char* name; unsigned arguments; };
constexpr Api kApis[] = {
    {"RegOpenKeyExA",5}, {"RegOpenKeyExW",5}, {"RegCreateKeyExA",9}, {"RegCreateKeyExW",9},
    {"RegQueryValueExA",6}, {"RegQueryValueExW",6}, {"RegSetValueExA",6}, {"RegSetValueExW",6},
    {"RegCloseKey",1}, {"RegFlushKey",1}, {"RegDeleteValueA",2}, {"RegDeleteValueW",2},
    {"RegDeleteKeyA",2}, {"RegDeleteKeyW",2}, {"RegEnumKeyExA",8}, {"RegEnumKeyExW",8},
    {"RegEnumValueA",8}, {"RegEnumValueW",8}, {"RegQueryInfoKeyA",12}, {"RegQueryInfoKeyW",12},
    {"RegOpenKeyA",3}, {"RegOpenKeyW",3}, {"RegCreateKeyA",3}, {"RegCreateKeyW",3},
    {"FindWindowA",2}, {"SHGetSpecialFolderPathA",4},
    {"CreateProcessA",10},{"CreateProcessW",10},{"ShellExecuteA",6},{"ShellExecuteW",6},
    {"ShellExecuteExA",1},{"ShellExecuteExW",1},{"WinExec",2}
};
}
struct DfxRegistryHelper::Impl {
    std::filesystem::path executable;
    Call call;
    std::thread worker;
    std::atomic<bool> stopping{}, running{};
    std::mutex mutex;
    std::vector<std::wstring> diagnostics;
    std::vector<std::wstring> trace;
    HANDLE process{};
    DWORD process_id{};
    std::map<DWORD,HANDLE> threads;
    std::map<DWORD,Api> stubs;
    std::map<DWORD,std::string> module_names;
    DWORD stub_memory{}, resolver{}, original_resolver{}, image{};
    bool initial_breakpoint{};
    void Record(std::wstring message) { std::lock_guard lock(mutex); diagnostics.push_back(std::move(message)); }
    void Trace(std::wstring message) {std::lock_guard lock(mutex);if(trace.size()<2048)trace.push_back(std::move(message));}
    HANDLE OwnThread(HANDLE borrowed) {
        HANDLE owned{};
        if(!DuplicateHandle(GetCurrentProcess(),borrowed,GetCurrentProcess(),&owned,0,FALSE,DUPLICATE_SAME_ACCESS))
            throw std::runtime_error("Cannot retain DFX helper thread");
        return owned;
    }
    void Read(DWORD address, void* output, size_t bytes) {
        SIZE_T done{};
        if (bytes && (!address || bytes > kLimit ||
            !ReadProcessMemory(process, reinterpret_cast<void*>(address), output, bytes, &done) || done != bytes))
            throw std::runtime_error("Invalid helper read");
    }
    void Write(DWORD address, const void* input, size_t bytes) {
        SIZE_T done{};
        if (bytes && (!address || bytes > kLimit ||
            !WriteProcessMemory(process, reinterpret_cast<void*>(address), input, bytes, &done) || done != bytes))
            throw std::runtime_error("Invalid helper write");
    }
    template<class T> T Read(DWORD address) { T value{}; Read(address,&value,sizeof(value)); return value; }
    std::vector<BYTE> String(DWORD address, bool wide) {
        std::vector<BYTE> result;
        if (!address) return result;
        for (size_t i=0;i<32768;++i) {
            const auto value = wide ? Read<WORD>(address + static_cast<DWORD>(i*2)) : Read<BYTE>(address + static_cast<DWORD>(i));
            result.push_back(static_cast<BYTE>(value));
            if (wide) result.push_back(static_cast<BYTE>(value >> 8));
            if (!value) return result;
        }
        throw std::runtime_error("Unterminated helper string");
    }
    DWORD Stub(Api api) {
        for (const auto& [address, existing] : stubs) if (std::strcmp(api.name,existing.name)==0) return address;
        const DWORD address = stub_memory + static_cast<DWORD>(stubs.size()*16);
        if (stubs.size() >= 128) throw std::runtime_error("Too many helper API stubs");
        const BYTE code[] = {0xcc,0xc2,static_cast<BYTE>(api.arguments*4),0};
        Write(address,code,sizeof(code));
        if (!FlushInstructionCache(process,reinterpret_cast<void*>(address),sizeof(code)))
            throw std::runtime_error("Cannot publish helper API stub");
        stubs.emplace(address,api); return address;
    }
    std::string ModuleName(DWORD base) {
        if(auto found=module_names.find(base);found!=module_names.end())return found->second;
        const auto dos=Read<IMAGE_DOS_HEADER>(base);
        if(dos.e_magic!=IMAGE_DOS_SIGNATURE)return {};
        const auto nt=Read<IMAGE_NT_HEADERS32>(base+dos.e_lfanew);
        const auto rva=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        if(!rva)return {};
        const auto directory=Read<IMAGE_EXPORT_DIRECTORY>(base+rva);
        const auto name=String(base+directory.Name,false);
        return module_names[base]=reinterpret_cast<const char*>(name.data());
    }
    void Install() {
        const auto dos=Read<IMAGE_DOS_HEADER>(image);
        if(dos.e_magic!=IMAGE_DOS_SIGNATURE) throw std::runtime_error("Invalid helper image");
        const auto nt=Read<IMAGE_NT_HEADERS32>(image+dos.e_lfanew);
        if(nt.Signature!=IMAGE_NT_SIGNATURE || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_I386)
            throw std::runtime_error("Wrong helper architecture");
        stub_memory=reinterpret_cast<DWORD>(VirtualAllocEx(process,nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
        if(!stub_memory) throw std::runtime_error("Cannot create helper API transport");
        // This SHA-256-pinned helper aliases OriginalFirstThunk and FirstThunk.
        // The loader has overwritten its import names at the initial breakpoint.
        // Its on-disk GetProcAddress IAT entry is RVA 0x1B514.
        const auto slot=image+0x1b514;
        original_resolver=Read<DWORD>(slot);
        if(!original_resolver)throw std::runtime_error("Missing helper resolver");
        resolver=Stub({"GetProcAddress",2});
        DWORD previous{};
        if(!VirtualProtectEx(process,reinterpret_cast<void*>(slot),4,PAGE_READWRITE,&previous))
            throw std::runtime_error("Cannot intercept helper resolver");
        Write(slot,&resolver,4);
        DWORD ignored{}; VirtualProtectEx(process,reinterpret_cast<void*>(slot),4,previous,&ignored);
    }
    void Dispatch(const Api& api, CONTEXT& context) {
        std::array<DWORD,13> stack{}; Read(context.Esp,stack.data(),(api.arguments+1)*4);
        if(std::strcmp(api.name,"GetProcAddress")==0) {
            if(stack[2]>0xffff) {
                const auto raw=String(stack[2],false);
                const std::string name(reinterpret_cast<const char*>(raw.data()));
                const auto library=ModuleName(stack[1]);
                Trace(L"resolve "+core::Utf8ToWide(library+"!"+name));
                if(name=="GetProcAddress") {context.Eax=resolver;return;}
                if(name=="FindWindowA" || name=="SHGetSpecialFolderPathA" || name.starts_with("CreateProcess") || name.starts_with("ShellExecute") || name=="WinExec") {
                    for(const auto& known:kApis) if(name==known.name){context.Eax=Stub(known);return;}
                    throw std::runtime_error("Uncovered helper process API");
                }
                // The packed helper obtains its runtime imports through this
                // resolver. Return the broker before its first Reg* call.
                if(name.starts_with("Reg") && _stricmp(library.c_str(),"advapi32.dll")==0) {
                    for(const auto& known:kApis) if(name==known.name) {
                        context.Eax=Stub(known); return;
                    }
                    Record(L"Unsupported DFX helper registry API: "+core::Utf8ToWide(name));
                    throw std::runtime_error("Uncovered helper registry API");
                }
            } else if(_stricmp(ModuleName(stack[1]).c_str(),"advapi32.dll")==0)
                throw std::runtime_error("DFX helper ordinal registry APIs are not supported");
            context.Eip=original_resolver; // Tail-call the real resolver.
            return;
        }
        std::array<ULONG_PTR,12> args{};
        for(unsigned i=0;i<api.arguments;++i) args[i]=stack[i+1];
        std::array<std::vector<BYTE>,12> buffers;
        struct Output {unsigned argument; DWORD remote; size_t bytes; bool success_only;};
        std::vector<Output> outputs;
        const bool wide=std::string_view(api.name).ends_with('W');
        const auto string=[&](unsigned index) {
            if(!args[index]) return;
            buffers[index]=String(static_cast<DWORD>(args[index]),wide);
            args[index]=reinterpret_cast<ULONG_PTR>(buffers[index].data());
        };
        const auto output=[&](unsigned index,size_t size,bool /*input*/=false,bool success_only=false) {
            if(!args[index]) return;
            if(size>kLimit) throw std::runtime_error("Helper buffer too large");
            const auto remote=static_cast<DWORD>(args[index]);
            buffers[index].resize(std::max<size_t>(size,1));
            // Preserve bytes beyond the actual result and optional outputs on
            // failure; a short registry value must not zero the caller's tail.
            Read(remote,buffers[index].data(),size);
            args[index]=reinterpret_cast<ULONG_PTR>(buffers[index].data());
            outputs.push_back({index,remote,size,success_only});
        };
        const std::string_view name(api.name);
        Trace(L"call "+core::Utf8ToWide(name));
        if(name.starts_with("CreateProcess") || name.starts_with("ShellExecute") || name=="WinExec")
            throw std::runtime_error("DFX helper child processes are not supported");
        if(name=="FindWindowA") {
            string(0);string(1);context.Eax=static_cast<DWORD>(call(name,args));return;
        }
        if(name=="SHGetSpecialFolderPathA") {
            output(1,MAX_PATH);context.Eax=static_cast<DWORD>(call(name,args));
            if(!context.Eax)Record(L"DFX settings cannot access its local data directory (the legacy helper requires an ANSI or short path).");
            if(context.Eax)for(const auto& item:outputs)Write(item.remote,buffers[item.argument].data(),std::strlen(reinterpret_cast<const char*>(buffers[item.argument].data()))+1);
            return;
        }
        const auto characters=[&](DWORD size)->size_t {
            const size_t width=wide?2:1;
            if(size>kLimit/width)throw std::runtime_error("Helper string buffer too large");
            return static_cast<size_t>(size)*width;
        };
        if(name.starts_with("RegOpenKeyEx")) {string(1);output(4,4);}
        else if(name.starts_with("RegCreateKeyEx")) {string(1);string(3);output(7,4);output(8,4);}
        else if(name.starts_with("RegOpenKey") || name.starts_with("RegCreateKey")) {string(1);output(2,4);}
        else if(name.starts_with("RegQueryValueEx")) {
            string(1); output(3,4); const auto size=args[5]?Read<DWORD>(static_cast<DWORD>(args[5])):0;
            output(4,size,false,true);output(5,4,true);
        } else if(name.starts_with("RegSetValueEx")) {
            string(1);
            if(args[5]>kLimit)throw std::runtime_error("Helper value too large");
            if(args[4]) {buffers[4].resize(std::max<ULONG_PTR>(args[5],1));Read(static_cast<DWORD>(args[4]),buffers[4].data(),args[5]);args[4]=reinterpret_cast<ULONG_PTR>(buffers[4].data());}
        } else if(name.starts_with("RegDelete")) string(1);
        else if(name.starts_with("RegEnumKeyEx")) {
            const auto size=args[3]?Read<DWORD>(static_cast<DWORD>(args[3])):0;
            output(2,characters(size),false,true);output(3,4,true);
            const auto cls=args[6]?Read<DWORD>(static_cast<DWORD>(args[6])):0;
            output(5,characters(cls),false,true);output(6,4,true);output(7,8);
        } else if(name.starts_with("RegEnumValue")) {
            const auto size=args[3]?Read<DWORD>(static_cast<DWORD>(args[3])):0;
            output(2,characters(size),false,true);output(3,4,true);output(5,4);
            const auto data=args[7]?Read<DWORD>(static_cast<DWORD>(args[7])):0;
            output(6,data,false,true);output(7,4,true);
        } else if(name.starts_with("RegQueryInfoKey")) {
            const auto size=args[2]?Read<DWORD>(static_cast<DWORD>(args[2])):0;
            output(1,characters(size),false,true);output(2,4,true);
            for(unsigned i=4;i<11;++i) output(i,4);
            output(11,8);
        }
        context.Eax=static_cast<DWORD>(call(name,args));
        for(const auto& item:outputs)
            if(!item.success_only || context.Eax==ERROR_SUCCESS) Write(item.remote,buffers[item.argument].data(),item.bytes);
    }
    void Run() noexcept {
        PROCESS_INFORMATION child{};
        try {
            STARTUPINFOW startup{sizeof(startup)};
            auto command=L"\""+executable.native()+L"\"";
            if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,
                DEBUG_ONLY_THIS_PROCESS,nullptr,executable.parent_path().c_str(),&startup,&child))
                throw std::runtime_error("Cannot start DFX helper");
            process=child.hProcess; process_id=child.dwProcessId;
            CloseHandle(child.hThread); running=true;
            bool exited{};
            while(!exited) {
                if(stopping) TerminateProcess(process,ERROR_CANCELLED);
                DEBUG_EVENT event{};
                if(!WaitForDebugEvent(&event,100)) {
                    if(GetLastError()==ERROR_SEM_TIMEOUT) continue;
                    throw std::runtime_error("DFX helper transport failed");
                }
                DWORD continuation=DBG_CONTINUE;
                try {
                    switch(event.dwDebugEventCode) {
                    case CREATE_PROCESS_DEBUG_EVENT:
                        image=reinterpret_cast<DWORD>(event.u.CreateProcessInfo.lpBaseOfImage);
                        if(event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
                        // Debug-event process/thread handles belong to Windows:
                        // ContinueDebugEvent closes them at exit. Retain our own
                        // duplicates so cleanup cannot double-close a reused handle.
                        threads[event.dwThreadId]=OwnThread(event.u.CreateProcessInfo.hThread);
                        break;
                    case CREATE_THREAD_DEBUG_EVENT:threads[event.dwThreadId]=OwnThread(event.u.CreateThread.hThread);break;
                    case EXIT_THREAD_DEBUG_EVENT:
                        if(threads.contains(event.dwThreadId)) {CloseHandle(threads[event.dwThreadId]);threads.erase(event.dwThreadId);} break;
                    case LOAD_DLL_DEBUG_EVENT:if(event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);break;
                    case EXCEPTION_DEBUG_EVENT: {
                        const auto& exception=event.u.Exception.ExceptionRecord;
                        const auto address=reinterpret_cast<DWORD>(exception.ExceptionAddress);
                        if(exception.ExceptionCode==EXCEPTION_BREAKPOINT && stubs.contains(address)) {
                            CONTEXT context{};context.ContextFlags=CONTEXT_FULL;
                            const auto thread=threads.at(event.dwThreadId);
                            if(!GetThreadContext(thread,&context)) throw std::runtime_error("Cannot read helper context");
                            context.Eip=address+1;
                            Dispatch(stubs.at(address),context);
                            if(!SetThreadContext(thread,&context)) throw std::runtime_error("Cannot reply to helper");
                        } else if(exception.ExceptionCode==EXCEPTION_BREAKPOINT && !initial_breakpoint) {
                            initial_breakpoint=true;Install();
                        } else continuation=DBG_EXCEPTION_NOT_HANDLED; // Preserve the packer's SEH, too.
                        break;
                    }
                    case EXIT_PROCESS_DEBUG_EVENT:
                        if(event.u.ExitProcess.dwExitCode && !stopping)
                            Record(L"DFX settings exited with code "+std::to_wstring(event.u.ExitProcess.dwExitCode));
                        exited=true;break;
                    default:break;
                    }
                } catch(...) {ContinueDebugEvent(event.dwProcessId,event.dwThreadId,DBG_CONTINUE);throw;}
                if(!ContinueDebugEvent(event.dwProcessId,event.dwThreadId,continuation))
                    throw std::runtime_error("Cannot resume DFX helper");
            }
        } catch(const std::exception& error) {
            Record(L"DFX helper stopped: "+core::Utf8ToWide(error.what()));
            if(process) {TerminateProcess(process,ERROR_INVALID_DATA);DebugActiveProcessStop(process_id);WaitForSingleObject(process,2000);}
        }
        for(const auto& [id,thread]:threads) CloseHandle(thread);
        threads.clear();if(process) CloseHandle(process);process=nullptr;running=false;
    }
};
DfxRegistryHelper::DfxRegistryHelper(const std::filesystem::path& executable,Call call):impl_(std::make_unique<Impl>()) {
    if(update::Sha256(executable)!="0952aff8450deaa20b379c0caba804c55d0e30aebaaed1a98aa7b42559a60877")
        throw std::runtime_error("Unsupported DFX settings helper");
    impl_->executable=executable;impl_->call=std::move(call);impl_->running=true;
    impl_->worker=std::thread([this]{impl_->Run();});
}
DfxRegistryHelper::~DfxRegistryHelper() {impl_->stopping=true;if(impl_->worker.joinable())impl_->worker.join();}
bool DfxRegistryHelper::Running() const {return impl_->running;}
std::vector<std::wstring> DfxRegistryHelper::Diagnostics() {std::lock_guard lock(impl_->mutex);return std::exchange(impl_->diagnostics,{});}
std::vector<std::wstring> DfxRegistryHelper::Trace() {std::lock_guard lock(impl_->mutex);return impl_->trace;}
}
