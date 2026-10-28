#include "ophion_client.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>
#include <cstring>
#include <cfloat>
#include <algorithm>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static UINT resize_width, resize_height;
static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd,msg,wp,lp)) return 1;
    if (msg==WM_SIZE && wp!=SIZE_MINIMIZED) {
        resize_width=LOWORD(lp); resize_height=HIWORD(lp); return 0;
    }
    if (msg==WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (msg==WM_SYSCOMMAND && (wp & 0xfff0)==SC_KEYMENU) return 0;
    return DefWindowProcW(hwnd,msg,wp,lp);
}
template<class T> static void release(T*& p) { if(p) { p->Release(); p=nullptr; } }
struct graphics {
    ID3D11Device *device=nullptr;
    ID3D11DeviceContext *context=nullptr;
    IDXGISwapChain *swap=nullptr;
    ID3D11RenderTargetView *target=nullptr;
    ~graphics() { release(target); release(swap); release(context); release(device); }
    bool render_target() {
        ID3D11Texture2D *back=nullptr;
        if(FAILED(swap->GetBuffer(0,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&back)))) return false;
        HRESULT hr=device->CreateRenderTargetView(back,nullptr,&target); back->Release();
        return SUCCEEDED(hr);
    }
    bool init(HWND window) {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount=2; desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.OutputWindow=window;
        desc.SampleDesc.Count=1; desc.Windowed=TRUE; desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        D3D_FEATURE_LEVEL level;
        HRESULT hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,
            nullptr,0,D3D11_SDK_VERSION,&desc,&swap,&device,&level,&context);
        if(FAILED(hr)) hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,
            nullptr,0,D3D11_SDK_VERSION,&desc,&swap,&device,&level,&context);
        return SUCCEEDED(hr) && render_target();
    }
};
enum class action { connect, disconnect, refresh, ping, create_buffer, release_buffer,
    read, write, arm, access, disarm };
struct snapshot {
    bool connected=false, registered=false;
    unsigned cpus=1;
    OPH_PACKET version{}, stats{}, reply{};
    std::vector<OPH_EVENT> events;
    uint64_t dropped=0;
    std::string message="Disconnected. Open the device to start diagnostics.";
    float latency_us=0;
};
struct controller {
    std::unique_ptr<ophion::client> connection;
    std::unique_ptr<ophion::buffer> memory;
    uint64_t cursor[64]{};
    snapshot state;
    snapshot run(action command,unsigned cpu) {
        try {
            auto start=std::chrono::steady_clock::now();
            if(command==action::connect) {
                auto opened=std::make_unique<ophion::client>();
                state.version=opened->call(OPH_GET_VERSION);
                state.cpus=static_cast<unsigned>(state.version.args[2]);
                if(!state.cpus || state.cpus>64) throw std::runtime_error("Invalid CPU count");
                connection=std::move(opened);
                std::memset(cursor,0,sizeof(cursor)); state.events.clear(); state.dropped=0;
            } else if(command==action::disconnect) { memory.reset(); connection.reset(); }
            else if(!connection) throw std::runtime_error("Device is disconnected");
            else switch(command) {
            case action::ping: state.reply=connection->call(OPH_PING,cpu); break;
            case action::create_buffer: memory=std::make_unique<ophion::buffer>(*connection); break;
            case action::release_buffer: memory.reset(); break;
            case action::read: state.reply=memory->read(0,16,cpu); break;
            case action::write: {
                const uint64_t sample=0x4f5048494f4e0001ULL;
                memory->write(0,&sample,sizeof(sample),cpu); state.reply=memory->read(0,16,cpu); break;
            }
            case action::arm: memory->arm(); break;
            case action::disarm: memory->disarm(); break;
            case action::access: {
                DWORD_PTR previous=SetThreadAffinityMask(GetCurrentThread(),1ULL<<cpu);
                if(!previous) throw std::system_error(GetLastError(),std::system_category(),"Affinity");
                memory->data()[128]=0x42;
                SetThreadAffinityMask(GetCurrentThread(),previous); break;
            }
            default: break;
            }
            state.latency_us=static_cast<float>(std::chrono::duration<double,std::micro>(
                std::chrono::steady_clock::now()-start).count());
            if(connection) {
                state.stats=connection->call(OPH_GET_STATS,cpu);
                auto batch=connection->call(OPH_GET_EVENTS,cpu,cursor[cpu]);
                cursor[cpu]=batch.args[0]; state.dropped+=batch.args[2];
                if(batch.args[1]>OPH_EVENT_BATCH) throw std::runtime_error("Invalid event count");
                for(uint64_t i=0;i<batch.args[1];++i) {
                    OPH_EVENT e{}; std::memcpy(&e,batch.data+i*sizeof(e),sizeof(e)); state.events.push_back(e);
                }
                if(state.events.size()>256) state.events.erase(state.events.begin(),state.events.end()-256);
            }
            state.message="Request completed.";
        } catch(const std::exception& e) { state.message=e.what(); }
        state.connected=bool(connection); state.registered=bool(memory);
        return state;
    }
};

int WINAPI WinMain(HINSTANCE instance,HINSTANCE,LPSTR,int) {
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW cls{}; cls.cbSize=sizeof(cls); cls.lpfnWndProc=window_proc;
    cls.hInstance=instance; cls.lpszClassName=L"OphionLabConsole";
    cls.hCursor=LoadCursor(nullptr,IDC_ARROW);
    if(!RegisterClassExW(&cls)) return 1;
    HWND window=CreateWindowExW(0,cls.lpszClassName,L"Ophion | Laboratory Diagnostics",
        WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1100,780,nullptr,nullptr,instance,nullptr);
    if(!window) { UnregisterClassW(cls.lpszClassName,instance); return 1; }
    graphics gpu;
    if(!gpu.init(window)) { DestroyWindow(window); UnregisterClassW(cls.lpszClassName,instance); return 2; }
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    auto& style=ImGui::GetStyle(); style.WindowPadding=ImVec2(20,20);
    style.FramePadding=ImVec2(12,8); style.ItemSpacing=ImVec2(10,10);
    style.FrameRounding=4; style.WindowRounding=0;
    float scale=ImGui_ImplWin32_GetDpiScaleForHwnd(window);
    style.ScaleAllSizes(scale); ImGui::GetIO().FontGlobalScale=1.15f*scale;
    ImGui_ImplWin32_Init(window); ImGui_ImplDX11_Init(gpu.device,gpu.context);
    ShowWindow(window,SW_SHOWDEFAULT); UpdateWindow(window);
    controller worker; snapshot view;
    std::future<snapshot> pending;
    bool busy=false, done=false; int cpu=0;
    float latencies[120]{}; int next_latency=0;
    auto submit=[&](action command) {
        if(busy) return;
        busy=true;
        pending=std::async(std::launch::async,[&,command,selected=cpu]{return worker.run(command,selected);});
    };
    while(!done) {
        MSG msg;
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg); if(msg.message==WM_QUIT) done=true;
        }
        if(done) break;
        if(busy && pending.wait_for(std::chrono::milliseconds(0))==std::future_status::ready) {
            view=pending.get(); busy=false;
            latencies[next_latency++ % 120]=view.latency_us;
            cpu=std::min(cpu,static_cast<int>(view.cpus)-1);
        }
        if(resize_width && resize_height) {
            gpu.context->OMSetRenderTargets(0,nullptr,nullptr); release(gpu.target);
            HRESULT hr=gpu.swap->ResizeBuffers(0,resize_width,resize_height,DXGI_FORMAT_UNKNOWN,0);
            resize_width=resize_height=0;
            if(FAILED(hr)||!gpu.render_target()) break;
        }
        if(IsIconic(window)) { Sleep(20); continue; }
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Ophion",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove);
        ImGui::TextUnformatted("OPHION / LABORATORY DIAGNOSTICS");
        ImGui::TextUnformatted(view.connected ? "Device connected" : "Device disconnected");
        ImGui::TextWrapped("%s",view.message.c_str());
        ImGui::BeginDisabled(busy);
        if(ImGui::Button(view.connected?"Disconnect":"Connect")) submit(view.connected?action::disconnect:action::connect);
        ImGui::SameLine(); ImGui::TextUnformatted(busy?"Processing request...":"Ready");
        ImGui::BeginDisabled(!view.connected);
        ImGui::SliderInt("Logical CPU",&cpu,0,std::max(0,static_cast<int>(view.cpus)-1));
        if(ImGui::Button("Ping VMX-root")) submit(action::ping);
        ImGui::SameLine(); if(ImGui::Button("Refresh statistics")) submit(action::refresh);
        ImGui::Separator();
        ImGui::Text("Protocol: %llu   Root sequence: %llu   Timing compensation: %s",
            view.version.args[0],view.stats.root_sequence,view.version.args[3]?"Enabled":"Disabled");
        ImGui::Text("Handler ticks (raw TSC): total %llu / maximum %llu",view.stats.args[0],view.stats.args[1]);
        ImGui::PlotLines("Request time (us)",latencies,120,next_latency%120,nullptr,0,FLT_MAX,ImVec2(0,90));
        ImGui::Separator();
        if(ImGui::Button(view.registered?"Release lab page":"Register lab page"))
            submit(view.registered?action::release_buffer:action::create_buffer);
        ImGui::BeginDisabled(!view.registered);
        if(ImGui::Button("Root read")) submit(action::read);
        ImGui::SameLine(); if(ImGui::Button("Root write sample")) submit(action::write);
        if(ImGui::Button("Arm first-write observation")) submit(action::arm);
        ImGui::SameLine(); if(ImGui::Button("Generate write on selected CPU")) submit(action::access);
        ImGui::SameLine(); if(ImGui::Button("Disarm")) submit(action::disarm);
        ImGui::TextUnformatted("One registered 4 KiB page. One write event per CPU per arm.");
        if(view.reply.operation==OPH_READ) {
            ImGui::TextUnformatted("First 16 bytes:");
            for(unsigned i=0;i<16;++i) { ImGui::SameLine(); ImGui::Text("%02X",view.reply.data[i]); }
        }
        ImGui::EndDisabled(); ImGui::EndDisabled(); ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::Text("Events retained: %u / ring overwrites reported: %llu",static_cast<unsigned>(view.events.size()),view.dropped);
        if(ImGui::BeginTable("events",5,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY,ImVec2(0,190))) {
            for(const char *title : {"CPU","Sequence","Type","Page offset","Raw TSC"}) ImGui::TableSetupColumn(title);
            ImGui::TableHeadersRow();
            for(auto it=view.events.rbegin();it!=view.events.rend();++it) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%u",it->cpu);
                ImGui::TableNextColumn(); ImGui::Text("%llu",it->sequence);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->kind==OPH_EVENT_WRITE?"Write":"Unexpected EPT");
                ImGui::TableNextColumn(); ImGui::Text("0x%llX",it->offset);
                ImGui::TableNextColumn(); ImGui::Text("%llu",it->tsc);
            }
            ImGui::EndTable();
        }
        ImGui::End(); ImGui::Render();
        const float background[]={0.07f,0.08f,0.10f,1};
        gpu.context->OMSetRenderTargets(1,&gpu.target,nullptr);
        gpu.context->ClearRenderTargetView(gpu.target,background);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        HRESULT present=gpu.swap->Present(1,0);
        if(present==DXGI_ERROR_DEVICE_REMOVED || present==DXGI_ERROR_DEVICE_RESET) break;
    }
    if(busy) pending.wait();
    worker.memory.reset(); worker.connection.reset();
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    DestroyWindow(window); UnregisterClassW(cls.lpszClassName,instance);
    return 0;
}
