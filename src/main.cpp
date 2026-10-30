// DnsSwitch - cambio, test e ripristino DNS per Windows
// C++17, Win32 puro, nessuna dipendenza esterna.
//
// Funzioni:
//  - elenco schede di rete attive con DNS attuali (manuali / automatici)
//  - elenco server DNS (servers.txt accanto all'exe, creato al primo avvio)
//  - test di latenza in parallelo (query UDP/53 reali, media su 4 query)
//  - applicazione DNS IPv4 + IPv6 (SetInterfaceDnsSettings, fallback netsh)
//  - ripristino automatico (DHCP)
//  - svuotamento cache DNS (DnsFlushResolverCache, fallback ipconfig)

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uxtheme.lib")
#endif

// ---------------------------------------------------------------------------
// Costanti e stato
// ---------------------------------------------------------------------------

static const wchar_t* kAppName = L"DnsSwitch";

enum : int {
    IDC_ADAPTER = 100,
    IDC_REFRESH,
    IDC_CURRENT,
    IDC_LIST,
    IDC_LBL_V4A, IDC_E_V4A,
    IDC_LBL_V4B, IDC_E_V4B,
    IDC_LBL_V6A, IDC_E_V6A,
    IDC_LBL_V6B, IDC_E_V6B,
    IDC_TEST,
    IDC_APPLY,
    IDC_RESTORE,
    IDC_FLUSH,
    IDC_EDITLIST,
};

#define WM_APP_RESULT   (WM_APP + 1)   // wParam = indice server, lParam = ms (-1 timeout)
#define WM_APP_TESTDONE (WM_APP + 2)

enum : int { LAT_NONE = -2, LAT_TIMEOUT = -1, LAT_RUNNING = -3 };

struct DnsServer {
    std::wstring name, v4a, v4b, v6a, v6b;
    int latency = LAT_NONE;
    bool isAuto = false;        // voce "Automatico (DHCP / router)": applicarla = tornare ai DNS automatici
};

struct Adapter {
    std::wstring friendlyName, description;
    std::string  adapterName;   // "{GUID}" usato nel registro
    GUID         guid{};
    bool         hasV6 = false;
    std::vector<std::wstring> dns;        // DNS in uso ora
    std::vector<std::wstring> dhcpDns;    // DNS IPv4 forniti dal DHCP (anche se ora sono impostati a mano)
    std::vector<std::wstring> gateways;   // gateway IPv4 (router)
};

enum : int { IDC_LBL_ADAPTER = 200, IDC_STATUS };

static const COLORREF kActive = RGB(0, 95, 184);   // server in uso

static HINSTANCE g_inst;
static HWND g_main, g_cbAdapter, g_lblCurrent, g_list, g_status;
static HWND g_eV4a, g_eV4b, g_eV6a, g_eV6b;
static HWND g_btnTest, g_btnApply, g_btnRestore, g_btnFlush, g_btnRefresh, g_btnEditList;
static HFONT g_font, g_fontBold;
static int g_dpi = 96;

static std::vector<DnsServer> g_servers;
static std::vector<Adapter>   g_adapters;
static std::atomic<bool>      g_testing{false};
static int                    g_testDone = 0;
static int                    g_sortCol = -1;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

// ---------------------------------------------------------------------------
// Utilità
// ---------------------------------------------------------------------------

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string Narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring ExeDir() {
    wchar_t p[MAX_PATH]{};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

static std::wstring SysDir() {
    wchar_t p[MAX_PATH]{};
    GetSystemDirectoryW(p, MAX_PATH);
    return std::wstring(p) + L"\\";
}

static std::wstring ErrText(DWORD e) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = buf ? Trim(buf) : L"Errore sconosciuto";
    if (buf) LocalFree(buf);
    return s + L" (" + std::to_wstring(e) + L")";
}

static bool IsIPv4(const std::wstring& s) { IN_ADDR a;  return InetPtonW(AF_INET,  s.c_str(), &a) == 1; }
static bool IsIPv6(const std::wstring& s) { IN6_ADDR a; return InetPtonW(AF_INET6, s.c_str(), &a) == 1; }

static std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    if (n) GetWindowTextW(h, &s[0], n + 1);
    return Trim(s);
}

static void SetStatus(const std::wstring& s) {
    if (g_status) SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)s.c_str());
}

template <class F>
static F LoadFn(const wchar_t* dll, const char* name) {
    HMODULE h = GetModuleHandleW(dll);
    if (!h) h = LoadLibraryW(dll);
    if (!h) return nullptr;
    return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(h, name)));
}

static DWORD RunHidden(std::wstring cmd) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return GetLastError();
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

// ---------------------------------------------------------------------------
// Elenco server (servers.txt)
// ---------------------------------------------------------------------------

static const wchar_t* kDefaultServers =
LR"(# Elenco server DNS di DnsSwitch
# Formato: Nome;IPv4 primario;IPv4 secondario;IPv6 primario;IPv6 secondario
# I campi vuoti sono ammessi. Le righe che iniziano con # sono commenti.
Cloudflare;1.1.1.1;1.0.0.1;2606:4700:4700::1111;2606:4700:4700::1001
Cloudflare (blocco malware);1.1.1.2;1.0.0.2;2606:4700:4700::1112;2606:4700:4700::1002
Cloudflare (famiglia);1.1.1.3;1.0.0.3;2606:4700:4700::1113;2606:4700:4700::1003
Google;8.8.8.8;8.8.4.4;2001:4860:4860::8888;2001:4860:4860::8844
Quad9;9.9.9.9;149.112.112.112;2620:fe::fe;2620:fe::9
Quad9 (senza filtro);9.9.9.10;149.112.112.10;2620:fe::10;2620:fe::fe:10
OpenDNS;208.67.222.222;208.67.220.220;2620:119:35::35;2620:119:53::53
OpenDNS FamilyShield;208.67.222.123;208.67.220.123;;
AdGuard (blocco pubblicità);94.140.14.14;94.140.15.15;2a10:50c0::ad1:ff;2a10:50c0::ad2:ff
AdGuard (senza filtro);94.140.14.140;94.140.14.141;2a10:50c0::1:ff;2a10:50c0::2:ff
AdGuard (famiglia);94.140.14.15;94.140.15.16;2a10:50c0::bad1:ff;2a10:50c0::bad2:ff
Control D (senza filtro);76.76.2.0;76.76.10.0;2606:1a40::;2606:1a40:1::
CleanBrowsing (sicurezza);185.228.168.9;185.228.169.9;2a0d:2a00:1::2;2a0d:2a00:2::2
CleanBrowsing (famiglia);185.228.168.168;185.228.169.168;2a0d:2a00:1::;2a0d:2a00:2::
DNS4EU (senza filtro);86.54.11.100;86.54.11.200;;
Comodo Secure DNS;8.26.56.26;8.20.247.20;;
Level3;4.2.2.1;4.2.2.2;;
)";

static std::wstring ServersPath() { return ExeDir() + L"servers.txt"; }

static bool WriteUtf8File(const std::wstring& path, const std::wstring& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string s = Narrow(text);
    std::string out;
    for (char c : s) { if (c == '\n') out += '\r'; out += c; }
    DWORD wr = 0;
    BOOL ok = WriteFile(h, out.data(), (DWORD)out.size(), &wr, nullptr);
    CloseHandle(h);
    return ok && wr == out.size();
}

static bool ReadUtf8File(const std::wstring& path, std::wstring& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::string s((size_t)std::min<LONGLONG>(sz.QuadPart, 4 * 1024 * 1024), '\0');
    DWORD rd = 0;
    if (!s.empty()) ReadFile(h, &s[0], (DWORD)s.size(), &rd, nullptr);
    CloseHandle(h);
    s.resize(rd);
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        s.erase(0, 3);
    text = Widen(s);
    return true;
}

static void ParseServers(const std::wstring& text, std::vector<DnsServer>& out) {
    out.clear();
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = Trim(text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos));
        pos = (nl == std::wstring::npos) ? text.size() + 1 : nl + 1;
        if (line.empty() || line[0] == L'#') continue;

        std::vector<std::wstring> f;
        size_t p = 0;
        for (;;) {
            size_t sc = line.find(L';', p);
            f.push_back(Trim(line.substr(p, sc == std::wstring::npos ? std::wstring::npos : sc - p)));
            if (sc == std::wstring::npos) break;
            p = sc + 1;
        }
        f.resize(5);
        DnsServer d;
        d.name = f[0];
        d.v4a = IsIPv4(f[1]) ? f[1] : L"";
        d.v4b = IsIPv4(f[2]) ? f[2] : L"";
        d.v6a = IsIPv6(f[3]) ? f[3] : L"";
        d.v6b = IsIPv6(f[4]) ? f[4] : L"";
        if (d.name.empty() || (d.v4a.empty() && d.v6a.empty())) continue;
        out.push_back(d);
    }
}

static void LoadServers() {
    std::wstring text;
    if (!ReadUtf8File(ServersPath(), text)) {
        text = kDefaultServers;
        WriteUtf8File(ServersPath(), text);   // lo crea per poterlo modificare
    }
    ParseServers(text, g_servers);
    if (g_servers.empty()) ParseServers(kDefaultServers, g_servers);
}

// ---------------------------------------------------------------------------
// Schede di rete
// ---------------------------------------------------------------------------

static std::wstring SockaddrToStr(const SOCKADDR* sa) {
    wchar_t s[INET6_ADDRSTRLEN]{};
    if (sa->sa_family == AF_INET)
        InetNtopW(AF_INET, (void*)&((const sockaddr_in*)sa)->sin_addr, s, INET6_ADDRSTRLEN);
    else if (sa->sa_family == AF_INET6)
        InetNtopW(AF_INET6, (void*)&((const sockaddr_in6*)sa)->sin6_addr, s, INET6_ADDRSTRLEN);
    return s;
}

static void LoadAdapters() {
    g_adapters.clear();
    ULONG size = 32 * 1024, ret = 0;
    std::vector<unsigned char> buf;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_INCLUDE_GATEWAYS;
    for (int attempt = 0; attempt < 2; attempt++) {
        for (int i = 0; i < 4; i++) {
            buf.resize(size);
            ret = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
            if (ret != ERROR_BUFFER_OVERFLOW) break;
        }
        if (ret == NO_ERROR) break;
        flags &= ~GAA_FLAG_INCLUDE_GATEWAYS;   // ripiego senza gateway
    }
    if (ret != NO_ERROR) return;

    for (auto a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
        if (a->OperStatus != IfOperStatusUp) continue;
        Adapter ad;
        ad.friendlyName = a->FriendlyName ? a->FriendlyName : L"";
        ad.description  = a->Description ? a->Description : L"";
        ad.adapterName  = a->AdapterName ? a->AdapterName : "";
        ad.hasV6        = (a->Flags & IP_ADAPTER_IPV6_ENABLED) != 0;
        if (ConvertInterfaceLuidToGuid(&a->Luid, &ad.guid) != NO_ERROR) continue;
        for (auto d = a->FirstDnsServerAddress; d; d = d->Next) {
            std::wstring s = SockaddrToStr(d->Address.lpSockaddr);
            if (s.empty() || s.rfind(L"fec0:0:0:ffff::", 0) == 0) continue;   // segnaposto IPv6 di Windows
            ad.dns.push_back(s);
        }
        for (auto g = a->FirstGatewayAddress; g; g = g->Next) {
            std::wstring s = SockaddrToStr(g->Address.lpSockaddr);
            if (IsIPv4(s) && s != L"0.0.0.0") ad.gateways.push_back(s);
        }
        g_adapters.push_back(ad);
    }
}

static std::wstring RegIfValue(const Adapter& ad, bool v6, const wchar_t* name) {
    std::wstring key = std::wstring(v6 ? L"SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters\\Interfaces\\"
                                       : L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\")
                       + Widen(ad.adapterName);
    wchar_t val[2048]{};
    DWORD sz = sizeof(val);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), name, RRF_RT_REG_SZ, nullptr, val, &sz) != ERROR_SUCCESS)
        return L"";
    return Trim(val);
}

static bool IsStaticDns(const Adapter& ad, bool v6) {
    return !RegIfValue(ad, v6, L"NameServer").empty();
}

// DNS IPv4 ricevuti dal DHCP: Windows li conserva in DhcpNameServer anche quando i DNS sono manuali
static void LoadDhcpDns(Adapter& ad) {
    std::wstring v = RegIfValue(ad, false, L"DhcpNameServer");
    for (auto& c : v) if (c == L',') c = L' ';
    size_t p = 0;
    while (p < v.size()) {
        size_t e = v.find(L' ', p);
        std::wstring ip = Trim(v.substr(p, e == std::wstring::npos ? std::wstring::npos : e - p));
        if (IsIPv4(ip) && std::find(ad.dhcpDns.begin(), ad.dhcpDns.end(), ip) == ad.dhcpDns.end())
            ad.dhcpDns.push_back(ip);
        if (e == std::wstring::npos) break;
        p = e + 1;
    }
}

static Adapter* CurrentAdapter() {
    int sel = (int)SendMessageW(g_cbAdapter, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g_adapters.size()) return nullptr;
    return &g_adapters[sel];
}

static bool IsActiveServer(const DnsServer& s) {
    Adapter* ad = CurrentAdapter();
    if (!ad) return false;
    if (s.isAuto) return !IsStaticDns(*ad, false);
    for (auto& d : ad->dns)
        if ((!s.v4a.empty() && d == s.v4a) || (!s.v6a.empty() && _wcsicmp(d.c_str(), s.v6a.c_str()) == 0))
            return true;
    return false;
}

static void FillList();
static void SortServers(int col);
static void SelectRow(int i);

// Ricrea in cima all'elenco la voce con i DNS automatici (DHCP / router) della scheda selezionata.
static void RefreshAutoEntry() {
    int oldLatency = LAT_NONE;
    std::wstring oldIp;
    for (auto& s : g_servers) if (s.isAuto) { oldLatency = s.latency; oldIp = s.v4a; }
    g_servers.erase(std::remove_if(g_servers.begin(), g_servers.end(), [](const DnsServer& s) { return s.isAuto; }),
                    g_servers.end());

    Adapter* ad = CurrentAdapter();
    if (!ad) return;
    std::vector<std::wstring> v4 = ad->dhcpDns, v6;
    if (v4.empty() && !IsStaticDns(*ad, false))
        for (auto& d : ad->dns) if (IsIPv4(d)) v4.push_back(d);
    if (v4.empty()) v4 = ad->gateways;   // ultima risorsa: il router stesso fa quasi sempre da DNS
    if (ad->hasV6 && !IsStaticDns(*ad, true))
        for (auto& d : ad->dns) if (IsIPv6(d)) v6.push_back(d);
    if (v4.empty() && v6.empty()) return;

    DnsServer s;
    s.isAuto = true;
    s.name = L"Automatico (DHCP / router)";
    if (v4.size() > 0) s.v4a = v4[0];
    if (v4.size() > 1) s.v4b = v4[1];
    if (v6.size() > 0) s.v6a = v6[0];
    if (v6.size() > 1) s.v6b = v6[1];
    if (s.v4a == oldIp) s.latency = oldLatency;   // non perde il risultato del test dopo un aggiornamento
    g_servers.insert(g_servers.begin(), s);
}

static void UpdateCurrentLabel() {
    if (!g_testing) {
        // ricostruisce la voce automatica mantenendo ordinamento e selezione
        std::wstring selName;
        int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
        if (sel >= 0 && sel < (int)g_servers.size()) selName = g_servers[sel].name;
        RefreshAutoEntry();
        if (g_sortCol >= 0) SortServers(g_sortCol);
        FillList();
        for (int i = 0; i < (int)g_servers.size(); i++)
            if (!selName.empty() && g_servers[i].name == selName) { SelectRow(i); break; }
    }
    Adapter* ad = CurrentAdapter();
    if (!ad) {
        SetWindowTextW(g_lblCurrent, L"Nessuna scheda di rete attiva.");
    } else {
        std::wstring s = L"DNS attuali:  ";
        if (ad->dns.empty()) s += L"(nessuno)";
        for (size_t i = 0; i < ad->dns.size(); i++) { if (i) s += L",  "; s += ad->dns[i]; }
        s += L"      ·      IPv4 ";
        s += IsStaticDns(*ad, false) ? L"manuale" : L"automatico";
        if (ad->hasV6) { s += L",  IPv6 "; s += IsStaticDns(*ad, true) ? L"manuale" : L"automatico"; }
        SetWindowTextW(g_lblCurrent, s.c_str());
    }
    InvalidateRect(g_list, nullptr, FALSE);   // aggiorna l'evidenziazione del server in uso
}

static void RefreshAdapters() {
    GUID keep{};
    bool hadSel = false;
    if (Adapter* ad = CurrentAdapter()) { keep = ad->guid; hadSel = true; }

    LoadAdapters();
    for (auto& a : g_adapters) LoadDhcpDns(a);
    SendMessageW(g_cbAdapter, CB_RESETCONTENT, 0, 0);
    int selIdx = 0;
    for (size_t i = 0; i < g_adapters.size(); i++) {
        std::wstring t = g_adapters[i].friendlyName + L"   —   " + g_adapters[i].description;
        SendMessageW(g_cbAdapter, CB_ADDSTRING, 0, (LPARAM)t.c_str());
        if (hadSel && IsEqualGUID(g_adapters[i].guid, keep)) selIdx = (int)i;
    }
    if (!g_adapters.empty()) SendMessageW(g_cbAdapter, CB_SETCURSEL, selIdx, 0);
    UpdateCurrentLabel();
}

// ---------------------------------------------------------------------------
// Cambio DNS
// ---------------------------------------------------------------------------

// Struttura di netioapi.h (Windows 10 2004+). Definita qui per non dipendere dall'SDK.
struct DnsIfSettingsV1 {
    ULONG   Version;
    ULONG64 Flags;
    PWSTR   Domain;
    PWSTR   NameServer;
    PWSTR   SearchList;
    ULONG   RegistrationEnabled;
    ULONG   RegisterAdapterName;
    ULONG   EnableLLMNR;
    ULONG   QueryAdapterName;
    PWSTR   ProfileNameServer;
};
static const ULONG64 kDnsSettingIPv6       = 0x0001;
static const ULONG64 kDnsSettingNameServer = 0x0002;
typedef DWORD(WINAPI* PFN_SetInterfaceDnsSettings)(GUID, const DnsIfSettingsV1*);

static DWORD SetDnsApi(const GUID& g, bool v6, const std::wstring& list) {
    static auto fn = LoadFn<PFN_SetInterfaceDnsSettings>(L"iphlpapi.dll", "SetInterfaceDnsSettings");
    if (!fn) return ERROR_PROC_NOT_FOUND;
    std::wstring tmp = list;     // vuota = torna ai DNS automatici
    DnsIfSettingsV1 s{};
    s.Version = 1;
    s.Flags = kDnsSettingNameServer | (v6 ? kDnsSettingIPv6 : 0);
    s.NameServer = &tmp[0];
    return fn(g, &s);
}

// Fallback per Windows precedenti a 10 2004
static DWORD SetDnsNetsh(const std::wstring& ifName, bool v6, const std::vector<std::wstring>& servers) {
    std::wstring netsh = L"\"" + SysDir() + L"netsh.exe\" interface " + (v6 ? L"ipv6" : L"ipv4");
    std::wstring base = netsh + L" set dnsservers name=\"" + ifName + L"\" ";
    if (servers.empty()) return RunHidden(base + L"source=dhcp");
    DWORD r = RunHidden(base + L"source=static address=" + servers[0] + L" register=primary validate=no");
    for (size_t i = 1; i < servers.size() && r == 0; i++)
        r = RunHidden(netsh + L" add dnsservers name=\"" + ifName + L"\" address=" + servers[i] +
                      L" index=" + std::to_wstring(i + 1) + L" validate=no");
    return r;
}

static DWORD SetFamily(const Adapter& ad, bool v6, const std::vector<std::wstring>& list, bool& usedNetsh) {
    std::wstring joined;
    for (auto& x : list) { if (!joined.empty()) joined += L","; joined += x; }
    DWORD r = SetDnsApi(ad.guid, v6, joined);
    if (r == ERROR_PROC_NOT_FOUND) { usedNetsh = true; r = SetDnsNetsh(ad.friendlyName, v6, list); }
    return r;
}

static bool FlushDnsCache() {
    typedef BOOL(WINAPI* PFN_Flush)();
    if (auto fn = LoadFn<PFN_Flush>(L"dnsapi.dll", "DnsFlushResolverCache"))
        if (fn()) return true;
    return RunHidden(L"\"" + SysDir() + L"ipconfig.exe\" /flushdns") == 0;
}

// Applica (o ripristina, se entrambe le liste sono vuote). Restituisce messaggio d'errore vuoto se ok.
static std::wstring ApplyDns(const Adapter& ad, const std::vector<std::wstring>& v4, const std::vector<std::wstring>& v6) {
    bool netsh = false;
    DWORD r = SetFamily(ad, false, v4, netsh);
    if (r != NO_ERROR) return L"IPv4: " + (netsh ? L"netsh ha restituito " + std::to_wstring(r) : ErrText(r));
    if (ad.hasV6) {
        // senza IPv6 nel profilo, l'IPv6 torna automatico: evita di lasciare DNS manuali di un altro provider
        r = SetFamily(ad, true, v6, netsh);
        if (r != NO_ERROR) return L"IPv6: " + (netsh ? L"netsh ha restituito " + std::to_wstring(r) : ErrText(r));
    }
    FlushDnsCache();
    return L"";
}

// ---------------------------------------------------------------------------
// Test di latenza
// ---------------------------------------------------------------------------

static const char* kTestHosts[] = { "www.google.com", "www.microsoft.com", "www.cloudflare.com",
                                    "www.wikipedia.org", "www.amazon.com" };

static std::vector<unsigned char> BuildQuery(unsigned short id, const char* host) {
    std::vector<unsigned char> q = { (unsigned char)(id >> 8), (unsigned char)(id & 0xFF),
                                     0x01, 0x00,   // query standard, ricorsione richiesta
                                     0, 1, 0, 0, 0, 0, 0, 0 };
    const char* p = host;
    while (*p) {
        const char* dot = std::strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : std::strlen(p);
        q.push_back((unsigned char)len);
        q.insert(q.end(), p, p + len);
        p += len;
        if (*p == '.') p++;
    }
    q.push_back(0);
    q.insert(q.end(), { 0, 1, 0, 1 });   // tipo A, classe IN
    return q;
}

static int MeasureServer(const std::wstring& ip) {
    sockaddr_storage ss{};
    int sslen = 0;
    if (IsIPv4(ip)) {
        auto s4 = (sockaddr_in*)&ss;
        s4->sin_family = AF_INET;
        s4->sin_port = htons(53);
        InetPtonW(AF_INET, ip.c_str(), &s4->sin_addr);
        sslen = sizeof(sockaddr_in);
    } else if (IsIPv6(ip)) {
        auto s6 = (sockaddr_in6*)&ss;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(53);
        InetPtonW(AF_INET6, ip.c_str(), &s6->sin6_addr);
        sslen = sizeof(sockaddr_in6);
    } else {
        return LAT_TIMEOUT;
    }

    SOCKET s = socket(ss.ss_family, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return LAT_TIMEOUT;
    DWORD to = 1500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    if (connect(s, (sockaddr*)&ss, sslen) != 0) { closesocket(s); return LAT_TIMEOUT; }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    unsigned short seed = (unsigned short)(GetTickCount() ^ (GetCurrentThreadId() << 4));
    double total = 0;
    int ok = 0;
    const int kAttempts = 4;

    for (int i = 0; i < kAttempts; i++) {
        unsigned short id = (unsigned short)(seed + i * 7919);
        auto q = BuildQuery(id, kTestHosts[i % (sizeof(kTestHosts) / sizeof(kTestHosts[0]))]);
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        if (send(s, (const char*)q.data(), (int)q.size(), 0) == SOCKET_ERROR) continue;
        unsigned char resp[1500];
        for (;;) {
            int n = recv(s, (char*)resp, sizeof(resp), 0);
            if (n == SOCKET_ERROR) break;   // timeout
            if (n >= 12 && resp[0] == (id >> 8) && resp[1] == (id & 0xFF) && (resp[2] & 0x80)) {
                QueryPerformanceCounter(&t1);
                total += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
                ok++;
                break;
            }
        }
    }
    closesocket(s);
    if (!ok) return LAT_TIMEOUT;
    int ms = (int)(total / ok + 0.5);
    return ms < 1 ? 1 : ms;
}

// ---------------------------------------------------------------------------
// ListView
// ---------------------------------------------------------------------------

static std::wstring LatencyText(int l) {
    if (l == LAT_NONE) return L"—";
    if (l == LAT_RUNNING) return L"…";
    if (l == LAT_TIMEOUT) return L"●  timeout";
    return L"●  " + std::to_wstring(l) + L" ms";
}

static void SetCell(int row, int col, const std::wstring& t) {
    LVITEMW it{};
    it.iSubItem = col;
    it.pszText = (LPWSTR)t.c_str();
    SendMessageW(g_list, LVM_SETITEMTEXTW, row, (LPARAM)&it);
}

static void FillList() {
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    for (int i = 0; i < (int)g_servers.size(); i++) {
        const DnsServer& s = g_servers[i];
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = (LPWSTR)s.name.c_str();
        SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        SetCell(i, 1, s.v4a);
        SetCell(i, 2, s.v4b);
        SetCell(i, 3, s.v6a.empty() ? L"—" : s.v6a + (s.v6b.empty() ? L"" : L",  " + s.v6b));
        SetCell(i, 4, LatencyText(s.latency));
    }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, nullptr, TRUE);
}

static void SortServers(int col) {
    auto latKey = [](int l) { return l >= 0 ? l : (l == LAT_TIMEOUT ? 1000000 : 2000000); };
    if (col == 4)
        std::stable_sort(g_servers.begin(), g_servers.end(),
                         [&](const DnsServer& a, const DnsServer& b) { return latKey(a.latency) < latKey(b.latency); });
    else if (col == 0)
        std::stable_sort(g_servers.begin(), g_servers.end(),
                         [](const DnsServer& a, const DnsServer& b) {
                             if (a.isAuto != b.isAuto) return a.isAuto;   // la voce automatica resta in cima
                             return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
                         });
    g_sortCol = col;
}

static void SelectRow(int i) {
    ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(g_list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g_list, i, FALSE);
}

static void FillEditsFrom(const DnsServer& s) {
    SetWindowTextW(g_eV4a, s.v4a.c_str());
    SetWindowTextW(g_eV4b, s.v4b.c_str());
    SetWindowTextW(g_eV6a, s.v6a.c_str());
    SetWindowTextW(g_eV6b, s.v6b.c_str());
}

// ---------------------------------------------------------------------------
// Azioni
// ---------------------------------------------------------------------------

static void EnableActions(bool on) {
    for (HWND h : { g_btnTest, g_btnApply, g_btnRestore, g_btnRefresh, g_btnEditList, g_cbAdapter })
        EnableWindow(h, on);
}

static void StartTest() {
    if (g_testing || g_servers.empty()) return;
    g_testing = true;
    g_testDone = 0;
    std::vector<std::wstring> ips;
    for (auto& s : g_servers) {
        ips.push_back(!s.v4a.empty() ? s.v4a : s.v6a);
        s.latency = LAT_RUNNING;
    }
    FillList();
    EnableActions(false);
    SetStatus(L"Test in corso…");

    HWND hwnd = g_main;
    std::thread([hwnd, ips]() {
        std::atomic<int> next{0};
        size_t nw = std::min<size_t>(8, ips.size());
        std::vector<std::thread> pool;
        for (size_t w = 0; w < nw; w++)
            pool.emplace_back([&]() {
                for (;;) {
                    int i = next++;
                    if (i >= (int)ips.size()) break;
                    PostMessageW(hwnd, WM_APP_RESULT, (WPARAM)i, (LPARAM)MeasureServer(ips[i]));
                }
            });
        for (auto& t : pool) t.join();
        PostMessageW(hwnd, WM_APP_TESTDONE, 0, 0);
    }).detach();
}

static void OnRestore();

static void OnApply() {
    Adapter* ad = CurrentAdapter();
    if (!ad) { MessageBoxW(g_main, L"Seleziona una scheda di rete.", kAppName, MB_ICONWARNING); return; }

    std::wstring a = GetText(g_eV4a), b = GetText(g_eV4b), c = GetText(g_eV6a), d = GetText(g_eV6b);

    // voce "Automatico" selezionata e campi non modificati: si torna ai DNS automatici,
    // così se il router cambia DNS la scheda li segue
    int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (sel >= 0 && sel < (int)g_servers.size() && g_servers[sel].isAuto) {
        const DnsServer& s = g_servers[sel];
        if (a == s.v4a && b == s.v4b && c == s.v6a && d == s.v6b) { OnRestore(); return; }
    }

    for (auto* p : { &a, &b }) if (!p->empty() && !IsIPv4(*p)) {
        MessageBoxW(g_main, (L"Indirizzo IPv4 non valido: " + *p).c_str(), kAppName, MB_ICONWARNING); return;
    }
    for (auto* p : { &c, &d }) if (!p->empty() && !IsIPv6(*p)) {
        MessageBoxW(g_main, (L"Indirizzo IPv6 non valido: " + *p).c_str(), kAppName, MB_ICONWARNING); return;
    }
    if (a.empty() && c.empty()) {
        MessageBoxW(g_main, L"Inserisci almeno un DNS primario (IPv4 o IPv6), oppure scegli un server dall'elenco.",
                    kAppName, MB_ICONWARNING);
        return;
    }
    std::vector<std::wstring> v4, v6;
    if (!a.empty()) v4.push_back(a);
    if (!b.empty()) v4.push_back(b);
    if (!c.empty()) v6.push_back(c);
    if (!d.empty()) v6.push_back(d);

    std::wstring name = ad->friendlyName;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    std::wstring err = ApplyDns(*ad, v4, v6);
    SetCursor(old);
    if (!err.empty()) {
        MessageBoxW(g_main, (L"Impossibile applicare i DNS.\n\n" + err).c_str(), kAppName, MB_ICONERROR);
        SetStatus(L"Errore durante l'applicazione dei DNS.");
        return;
    }
    RefreshAdapters();
    SetStatus(L"DNS applicati a \"" + name + L"\" e cache svuotata.");
}

static void OnRestore() {
    Adapter* ad = CurrentAdapter();
    if (!ad) { MessageBoxW(g_main, L"Seleziona una scheda di rete.", kAppName, MB_ICONWARNING); return; }
    std::wstring name = ad->friendlyName;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    std::wstring err = ApplyDns(*ad, {}, {});
    SetCursor(old);
    if (!err.empty()) {
        MessageBoxW(g_main, (L"Impossibile ripristinare i DNS automatici.\n\n" + err).c_str(), kAppName, MB_ICONERROR);
        return;
    }
    RefreshAdapters();
    SetStatus(L"DNS di \"" + name + L"\" riportati su automatico (DHCP).");
}

static void OnFlush() {
    SetStatus(FlushDnsCache() ? L"Cache DNS svuotata." : L"Impossibile svuotare la cache DNS.");
}

static void OnEditList() {
    if (GetFileAttributesW(ServersPath().c_str()) == INVALID_FILE_ATTRIBUTES)
        WriteUtf8File(ServersPath(), kDefaultServers);   // assicura che il file esista
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    std::wstring notepad = SysDir() + L"notepad.exe";
    std::wstring arg = L"\"" + ServersPath() + L"\"";
    sei.lpFile = notepad.c_str();
    sei.lpParameters = arg.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei) && sei.hProcess) CloseHandle(sei.hProcess);
    SetStatus(L"Dopo aver salvato servers.txt premi \"Aggiorna\" per ricaricare l'elenco.");
}

static void OnRefresh() {
    LoadServers();
    if (g_sortCol >= 0) SortServers(g_sortCol);
    FillList();
    RefreshAdapters();
    SetStatus(std::to_wstring(g_servers.size()) + L" server caricati, " +
              std::to_wstring(g_adapters.size()) + L" schede attive.");
}

// ---------------------------------------------------------------------------
// Finestra (controlli nativi)
// ---------------------------------------------------------------------------

static HWND Ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_main,
                             (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

static void Move(HDWP& dw, int id, int x, int y, int w, int h) {
    dw = DeferWindowPos(dw, GetDlgItem(g_main, id), nullptr, x, y, std::max(w, 1), std::max(h, 1),
                        SWP_NOZORDER | SWP_NOACTIVATE);
}

// Layout calcolato dall'alto (scheda di rete) e dal basso (pulsanti e campi):
// la lista occupa esattamente lo spazio che resta, quindi nulla finisce sotto la barra di stato.
static void Layout() {
    RECT rc;
    GetClientRect(g_main, &rc);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT rs;
    GetWindowRect(g_status, &rs);
    const int W = rc.right;
    const int H = rc.bottom - (rs.bottom - rs.top);

    const int m = S(10), gap = S(8);
    const int row = S(24);        // combo e campi
    const int btnH = S(28);
    const int lbl = S(105), lblH = S(18), lblOff = (row - lblH) / 2 + S(1);

    HDWP dw = BeginDeferWindowPos(24);

    // --- in alto: scheda di rete
    int y = m;
    int bw = S(110);
    Move(dw, IDC_LBL_ADAPTER, m, y + lblOff, lbl, lblH);
    Move(dw, IDC_ADAPTER, m + lbl, y, W - 2 * m - lbl - bw - gap, S(300));
    Move(dw, IDC_REFRESH, W - m - bw, y - S(2), bw, btnH);
    y += btnH + S(4);
    Move(dw, IDC_CURRENT, m + lbl, y, W - 2 * m - lbl, lblH);
    y += lblH + S(10);
    const int listTop = y;

    // --- in basso: pulsanti, poi due righe di campi
    int yb = H - m - btnH;
    const int ids[] = { IDC_TEST, IDC_APPLY, IDC_RESTORE, IDC_FLUSH, IDC_EDITLIST };
    const int n = 5;
    int bwAll = (W - 2 * m - gap * (n - 1)) / n;
    for (int i = 0; i < n; i++) Move(dw, ids[i], m + i * (bwAll + gap), yb, bwAll, btnH);

    int y2 = yb - S(12) - row;
    int y1 = y2 - S(6) - row;
    int colW = (W - 2 * m - S(20)) / 2;
    int x2 = m + colW + S(20);
    int ew = colW - lbl;
    Move(dw, IDC_LBL_V4A, m, y1 + lblOff, lbl, lblH);  Move(dw, IDC_E_V4A, m + lbl, y1, ew, row);
    Move(dw, IDC_LBL_V6A, x2, y1 + lblOff, lbl, lblH); Move(dw, IDC_E_V6A, x2 + lbl, y1, ew, row);
    Move(dw, IDC_LBL_V4B, m, y2 + lblOff, lbl, lblH);  Move(dw, IDC_E_V4B, m + lbl, y2, ew, row);
    Move(dw, IDC_LBL_V6B, x2, y2 + lblOff, lbl, lblH); Move(dw, IDC_E_V6B, x2 + lbl, y2, ew, row);

    // --- in mezzo: la lista
    int listH = std::max(y1 - S(12) - listTop, S(80));
    int listW = W - 2 * m;
    Move(dw, IDC_LIST, m, listTop, listW, listH);

    EndDeferWindowPos(dw);

    // colonna IPv6 elastica
    int fixed = S(210) + S(115) + S(115) + S(95) + GetSystemMetrics(SM_CXVSCROLL) + S(6);
    ListView_SetColumnWidth(g_list, 3, std::max(S(150), listW - fixed));
}

static void CreateFonts() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;   // Segoe UI 9 pt sui Windows moderni
    g_font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_BOLD;
    g_fontBold = CreateFontIndirectW(&lf);
}

static void CreateControls() {
    Ctl(L"STATIC", L"Scheda di rete:", SS_NOPREFIX, IDC_LBL_ADAPTER);
    g_cbAdapter  = Ctl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_ADAPTER);
    g_btnRefresh = Ctl(L"BUTTON", L"Aggiorna", BS_PUSHBUTTON | WS_TABSTOP, IDC_REFRESH);
    g_lblCurrent = Ctl(L"STATIC", L"", SS_ENDELLIPSIS | SS_NOPREFIX, IDC_CURRENT);

    g_list = Ctl(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, IDC_LIST,
                 WS_EX_CLIENTEDGE);
    SetWindowTheme(g_list, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    // righe un po' più alte tramite una image list "vuota"
    HIMAGELIST il = ImageList_Create(1, S(22), ILC_COLOR32, 1, 0);
    ListView_SetImageList(g_list, il, LVSIL_SMALL);
    struct { const wchar_t* t; int w; } cols[] = {
        { L"Nome", 210 }, { L"Primario", 115 }, { L"Secondario", 115 }, { L"IPv6", 220 }, { L"Latenza", 95 } };
    for (int i = 0; i < 5; i++) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (LPWSTR)cols[i].t;
        c.cx = S(cols[i].w);
        SendMessageW(g_list, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
    }

    Ctl(L"STATIC", L"IPv4 primario:", SS_NOPREFIX, IDC_LBL_V4A);
    g_eV4a = Ctl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_E_V4A, WS_EX_CLIENTEDGE);
    Ctl(L"STATIC", L"IPv4 secondario:", SS_NOPREFIX, IDC_LBL_V4B);
    g_eV4b = Ctl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_E_V4B, WS_EX_CLIENTEDGE);
    Ctl(L"STATIC", L"IPv6 primario:", SS_NOPREFIX, IDC_LBL_V6A);
    g_eV6a = Ctl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_E_V6A, WS_EX_CLIENTEDGE);
    Ctl(L"STATIC", L"IPv6 secondario:", SS_NOPREFIX, IDC_LBL_V6B);
    g_eV6b = Ctl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_E_V6B, WS_EX_CLIENTEDGE);
    SendMessageW(g_eV4a, EM_SETCUEBANNER, TRUE, (LPARAM)L"es. 1.1.1.1");
    SendMessageW(g_eV6a, EM_SETCUEBANNER, TRUE, (LPARAM)L"facoltativo");

    g_btnTest     = Ctl(L"BUTTON", L"Test velocità",         BS_PUSHBUTTON | WS_TABSTOP, IDC_TEST);
    g_btnApply    = Ctl(L"BUTTON", L"Applica DNS",           BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_APPLY);
    g_btnRestore  = Ctl(L"BUTTON", L"Ripristina automatico", BS_PUSHBUTTON | WS_TABSTOP, IDC_RESTORE);
    g_btnFlush    = Ctl(L"BUTTON", L"Svuota cache DNS",      BS_PUSHBUTTON | WS_TABSTOP, IDC_FLUSH);
    g_btnEditList = Ctl(L"BUTTON", L"Modifica elenco…",      BS_PUSHBUTTON | WS_TABSTOP, IDC_EDITLIST);

    g_status = Ctl(STATUSCLASSNAMEW, L"", SBARS_SIZEGRIP, IDC_STATUS);
}

static COLORREF LatencyColor(int l) {
    if (l == LAT_TIMEOUT) return RGB(200, 30, 30);
    if (l < 0)            return GetSysColor(COLOR_GRAYTEXT);
    if (l <= 30)          return RGB(0, 140, 40);
    if (l <= 80)          return RGB(100, 140, 0);
    if (l <= 150)         return RGB(210, 120, 0);
    return RGB(200, 30, 30);
}

static LRESULT OnNotify(NMHDR* nh) {
    if (nh->idFrom != IDC_LIST) return 0;
    switch (nh->code) {
    case LVN_ITEMCHANGED: {
        auto nm = (NMLISTVIEW*)nh;
        if ((nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED) &&
            nm->iItem >= 0 && nm->iItem < (int)g_servers.size())
            FillEditsFrom(g_servers[nm->iItem]);
        break;
    }
    case LVN_COLUMNCLICK: {
        auto nm = (NMLISTVIEW*)nh;
        if (!g_testing && (nm->iSubItem == 0 || nm->iSubItem == 4)) { SortServers(nm->iSubItem); FillList(); }
        break;
    }
    case NM_DBLCLK:
        SetFocus(g_btnApply);
        break;
    case NM_CUSTOMDRAW: {
        auto cd = (NMLVCUSTOMDRAW*)nh;
        switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:     return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
            size_t i = (size_t)cd->nmcd.dwItemSpec;
            HFONT f = g_font;
            cd->clrText = GetSysColor(COLOR_WINDOWTEXT);
            if (i < g_servers.size()) {
                if (cd->iSubItem == 0 && IsActiveServer(g_servers[i])) {
                    f = g_fontBold;              // server attualmente in uso
                    cd->clrText = kActive;
                } else if (cd->iSubItem == 4) {
                    cd->clrText = LatencyColor(g_servers[i].latency);
                }
            }
            SelectObject(cd->nmcd.hdc, f);
            return CDRF_NEWFONT;
        }
        }
        break;
    }
    }
    return 0;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_main = hwnd;
        CreateControls();
        LoadServers();
        FillList();
        RefreshAdapters();
        SetStatus(std::to_wstring(g_servers.size()) + L" server caricati. Premi \"Test velocità\" per misurarli.");
        return 0;

    case WM_SIZE:
        Layout();
        return 0;

    case WM_GETMINMAXINFO: {
        auto mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = S(760);
        mmi->ptMinTrackSize.y = S(480);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
        if (GetDlgCtrlID((HWND)lp) == IDC_CURRENT) {
            HDC dc = (HDC)wp;
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ADAPTER:  if (HIWORD(wp) == CBN_SELCHANGE) UpdateCurrentLabel(); break;
        case IDC_REFRESH:  OnRefresh(); break;
        case IDC_TEST:     StartTest(); break;
        case IDC_APPLY:    OnApply(); break;
        case IDC_RESTORE:  OnRestore(); break;
        case IDC_FLUSH:    OnFlush(); break;
        case IDC_EDITLIST: OnEditList(); break;
        }
        return 0;

    case WM_NOTIFY:
        return OnNotify((NMHDR*)lp);

    case WM_APP_RESULT: {
        size_t i = (size_t)wp;
        if (i < g_servers.size()) {
            g_servers[i].latency = (int)lp;
            SetCell((int)i, 4, LatencyText((int)lp));
        }
        SetStatus(L"Test in corso… " + std::to_wstring(++g_testDone) + L"/" + std::to_wstring(g_servers.size()));
        return 0;
    }

    case WM_APP_TESTDONE: {
        g_testing = false;
        SortServers(4);
        FillList();
        EnableActions(true);
        if (!g_servers.empty() && g_servers[0].latency >= 0) {
            SelectRow(0);
            SetStatus(L"Test completato. Il più veloce è " + g_servers[0].name + L" (" +
                      std::to_wstring(g_servers[0].latency) + L" ms): premi \"Applica DNS\" per usarlo.");
        } else {
            SetStatus(L"Test completato: nessun server ha risposto. Controlla la connessione.");
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow) {
    g_inst = hInst;
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    CreateFonts();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                 GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"DnsSwitchMain";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"DnsSwitch — cambio e test DNS",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, S(880), S(600),
                                nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    DeleteObject(g_font);
    DeleteObject(g_fontBold);
    WSACleanup();
    return (int)m.wParam;
}
