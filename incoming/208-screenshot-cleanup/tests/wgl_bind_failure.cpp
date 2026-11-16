// Copyright 2026 Ji Mao Cai
// SPDX-License-Identifier: Apache-2.0
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

struct Probe {
    const wchar_t* name=L"ModAVD209HiddenWglProbe";
    HINSTANCE instance=GetModuleHandleW(nullptr);
    ATOM registered=0;
    HWND window=nullptr;
    HDC dc=nullptr;
    HGLRC retiring=nullptr,caller=nullptr;
    ~Probe() {
        wglMakeCurrent(nullptr,nullptr);
        if(retiring)wglDeleteContext(retiring);
        if(caller)wglDeleteContext(caller);
        if(dc)ReleaseDC(window,dc);
        if(window)DestroyWindow(window);
        if(registered)UnregisterClassW(name,instance);
    }
    bool init() {
        WNDCLASSW cls{};cls.style=CS_OWNDC;cls.lpfnWndProc=DefWindowProcW;
        cls.hInstance=instance;cls.lpszClassName=name;
        registered=RegisterClassW(&cls);
        if(!registered)return false;
        window=CreateWindowW(name,L"",WS_POPUP,0,0,1,1,nullptr,nullptr,instance,nullptr);
        if(!window)return false;
        dc=GetDC(window);
        PIXELFORMATDESCRIPTOR pfd{};pfd.nSize=sizeof(pfd);pfd.nVersion=1;
        pfd.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;
        pfd.iPixelType=PFD_TYPE_RGBA;pfd.cColorBits=24;pfd.cDepthBits=24;
        int format=ChoosePixelFormat(dc,&pfd);
        if(!format||!SetPixelFormat(dc,format,&pfd))return false;
        retiring=wglCreateContext(dc);caller=wglCreateContext(dc);
        return retiring && caller && wglMakeCurrent(dc,caller);
    }
};

int main() {
    Probe probe;
    if(!probe.init()) { std::fprintf(stderr,"WGL setup failed: 0x%lx\n",GetLastError());return 2; }
    std::printf("vendor=%s\nrenderer=%s\nversion=%s\n",glGetString(GL_VENDOR),glGetString(GL_RENDERER),glGetString(GL_VERSION));
    std::atomic<int> ready{0};std::atomic<bool> done{false};DWORD workerError=0;
    std::thread owner([&]{
        bool bound=wglMakeCurrent(probe.dc,probe.retiring)!=FALSE;
        workerError=bound?0:GetLastError();ready=bound?1:-1;
        while(!done)std::this_thread::yield();
        if(bound)wglMakeCurrent(nullptr,nullptr);
    });
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(!ready && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    bool failedAsExpected=false,restored=false;
    DWORD error=0;bool currentCleared=false;
    if(ready==1) {
        SetLastError(0);
        failedAsExpected=wglMakeCurrent(probe.dc,probe.retiring)==FALSE;
        error=GetLastError();currentCleared=wglGetCurrentContext()==nullptr;
        restored=wglMakeCurrent(probe.dc,probe.caller)!=FALSE && wglGetCurrentContext()==probe.caller;
    }
    done=true;owner.join();
    std::printf("workerBound=%d workerError=0x%lx failedBind=%d error=0x%lx currentCleared=%d callerRestored=%d\n",ready.load(),workerError,failedAsExpected,error,currentCleared,restored);
    wglMakeCurrent(nullptr,nullptr);
    bool retiringDeleted=wglDeleteContext(probe.retiring)!=FALSE;
    if(retiringDeleted)probe.retiring=nullptr;
    bool callerDeleted=wglDeleteContext(probe.caller)!=FALSE;
    if(callerDeleted)probe.caller=nullptr;
    return ready==1 && failedAsExpected && restored && retiringDeleted && callerDeleted?0:1;
}
