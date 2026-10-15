#pragma once
#include <windows.h>
#include <winhttp.h>
#include <optional>
#include <string>

namespace minima {
struct HttpHandle {HINTERNET handle=nullptr;~HttpHandle(){if(handle)WinHttpCloseHandle(handle);}};
// One HTTPS GET with a 4 MB cap; redirects are followed by WinHTTP.
inline std::optional<std::string> http_get(const std::wstring& url) {
    URL_COMPONENTS parts{};parts.dwStructSize=sizeof(parts);
    wchar_t host[256]{},path[2048]{},extra[2048]{};
    parts.lpszHostName=host;parts.dwHostNameLength=256;parts.lpszUrlPath=path;parts.dwUrlPathLength=2048;parts.lpszExtraInfo=extra;parts.dwExtraInfoLength=2048;
    if(!WinHttpCrackUrl(url.c_str(),0,0,&parts)||(parts.nScheme!=INTERNET_SCHEME_HTTPS&&parts.nScheme!=INTERNET_SCHEME_HTTP))return std::nullopt;
    constexpr wchar_t agent[]=L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) MinimaDock";
    HttpHandle session;
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
    session.handle=WinHttpOpen(agent,WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
#endif
    if(!session.handle)session.handle=WinHttpOpen(agent,WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session.handle)return std::nullopt;
    WinHttpSetTimeouts(session.handle,8000,8000,15000,15000);
    HttpHandle connection;connection.handle=WinHttpConnect(session.handle,host,parts.nPort,0);if(!connection.handle)return std::nullopt;
    const std::wstring object=std::wstring(path)+extra;
    HttpHandle request;request.handle=WinHttpOpenRequest(connection.handle,L"GET",object.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,parts.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0);
    if(!request.handle||!WinHttpSendRequest(request.handle,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request.handle,nullptr))return std::nullopt;
    DWORD status=0,size=sizeof(status);
    if(!WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)||status!=200)return std::nullopt;
    std::string body;
    for(;;) {
        DWORD available=0;if(!WinHttpQueryDataAvailable(request.handle,&available))return std::nullopt;
        if(!available)break;if(body.size()+available>4*1024*1024)return std::nullopt;
        const auto at=body.size();body.resize(at+available);DWORD read=0;
        if(!WinHttpReadData(request.handle,body.data()+at,available,&read))return std::nullopt;
        body.resize(at+read);
    }
    return body;
}
}
