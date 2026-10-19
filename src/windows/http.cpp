#include "windows/http.h"
#include <windows.h>
#include <winhttp.h>

namespace minima {
namespace {
struct Internet {HINTERNET handle=nullptr;~Internet(){if(handle)WinHttpCloseHandle(handle);}};
}
HttpResult http_request(const std::wstring& url,const HttpOptions& options) {
    HttpResult result;
    URL_COMPONENTS parts{};parts.dwStructSize=sizeof(parts);
    wchar_t host[256]{},path[2048]{},extra[2048]{};
    parts.lpszHostName=host;parts.dwHostNameLength=256;parts.lpszUrlPath=path;parts.dwUrlPathLength=2048;parts.lpszExtraInfo=extra;parts.dwExtraInfoLength=2048;
    if(!WinHttpCrackUrl(url.c_str(),0,0,&parts)||(parts.nScheme!=INTERNET_SCHEME_HTTPS&&parts.nScheme!=INTERNET_SCHEME_HTTP))return result;
    const wchar_t* agent=options.agent?options.agent:L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) MinimaDock";
    Internet session;
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
    session.handle=WinHttpOpen(agent,WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
#endif
    if(!session.handle)session.handle=WinHttpOpen(agent,WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session.handle)return result;
    const int connect=options.timeout_ms/2>1000?options.timeout_ms/2:1000;
    WinHttpSetTimeouts(session.handle,connect,connect,options.timeout_ms,options.timeout_ms);
    Internet connection;connection.handle=WinHttpConnect(session.handle,host,parts.nPort,0);if(!connection.handle)return result;
    const std::wstring object=std::wstring(path)+extra;
    Internet request;request.handle=WinHttpOpenRequest(connection.handle,L"GET",object.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,parts.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0);
    if(!request.handle)return result;
    const std::wstring headers=options.language.empty()?std::wstring():L"Accept-Language: "+options.language+L"\r\n";
    if(!WinHttpSendRequest(request.handle,headers.empty()?WINHTTP_NO_ADDITIONAL_HEADERS:headers.c_str(),headers.empty()?0:static_cast<DWORD>(-1L),WINHTTP_NO_REQUEST_DATA,0,0,0)||
       !WinHttpReceiveResponse(request.handle,nullptr))return result;
    DWORD status=0,size=sizeof(status);
    if(!WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX))return result;
    if(status!=200){result.status=static_cast<int>(status);return result;}
    std::string body;
    for(;;) {
        DWORD available=0;if(!WinHttpQueryDataAvailable(request.handle,&available))return result;
        if(!available)break;if(body.size()+available>4*1024*1024)return result;
        const auto at=body.size();body.resize(at+available);DWORD read=0;
        if(!WinHttpReadData(request.handle,body.data()+at,available,&read))return result;
        body.resize(at+read);
    }
    result.status=200;result.body=std::move(body);
    return result;
}
}
