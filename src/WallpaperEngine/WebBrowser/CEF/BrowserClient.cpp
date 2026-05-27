#include "BrowserClient.h"

using namespace WallpaperEngine::WebBrowser::CEF;

BrowserClient::BrowserClient (CefRefPtr<CefRenderHandler> ptr) : m_renderHandler (std::move (ptr)) { }

CefRefPtr<CefRenderHandler> BrowserClient::GetRenderHandler () { return m_renderHandler; }

void BrowserClient::OnAfterCreated (CefRefPtr<CefBrowser> browser) {
    m_browser = browser;
    m_created.store (true);
}

void BrowserClient::OnBeforeClose (CefRefPtr<CefBrowser> browser) {
    m_browser = nullptr;
    m_closed.store (true);
}