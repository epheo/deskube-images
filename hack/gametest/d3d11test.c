/*
 * Minimal Direct3D 11 program for the Proton path: clears the screen to
 * a colour whose green channel stays 51, and counts presented frames.
 * Build it with -mwindows: as a console program Wine opens a console
 * window next to it, and gamescope may show that one instead.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
	return DefWindowProcA(h, m, w, l);
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 8;
	const char *out = argc > 2 ? argv[2] : "d3d11test.out";
	FILE *f = fopen(out, "w");
	WNDCLASSA wc = {0};
	wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d3d11test";
	RegisterClassA(&wc);
	int W = GetSystemMetrics(SM_CXSCREEN), H = GetSystemMetrics(SM_CYSCREEN);
	HWND hwnd = CreateWindowExA(0, "d3d11test", "d3d11test", WS_POPUP | WS_VISIBLE, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);

	DXGI_SWAP_CHAIN_DESC sd = {0};
	sd.BufferCount = 2; sd.BufferDesc.Width = W; sd.BufferDesc.Height = H;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd;
	sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	IDXGISwapChain *sc = NULL; ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL;
	D3D_FEATURE_LEVEL fl = 0;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
						   D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
	if (FAILED(hr)) { fprintf(f, "create failed hr=0x%08lx\n", (unsigned long)hr); fclose(f); return 1; }

	IDXGIDevice *xd = NULL; IDXGIAdapter *ad = NULL; DXGI_ADAPTER_DESC desc = {0};
	ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&xd);
	IDXGIDevice_GetAdapter(xd, &ad);
	IDXGIAdapter_GetDesc(ad, &desc);
	char name[128]; WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), NULL, NULL);

	ID3D11Texture2D *bb = NULL; ID3D11RenderTargetView *rtv = NULL;
	IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
	ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);

	DWORD start = GetTickCount(), now = start; long frames = 0;
	while ((now = GetTickCount()) - start < (DWORD)secs * 1000) {
		MSG msg;
		while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
		float t = (now - start) / 1000.0f;
		float c[4] = {0.5f + 0.5f * (float)sin(t), 0.2f, 0.5f + 0.5f * (float)cos(t), 1.0f};
		ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
		ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, c);
		IDXGISwapChain_Present(sc, 1, 0);
		frames++;
	}
	fprintf(f, "adapter=%s feature_level=0x%x size=%dx%d frames=%ld seconds=%d fps=%.1f\n",
		name, fl, W, H, frames, secs, frames / (double)secs);
	fclose(f);
	return 0;
}
