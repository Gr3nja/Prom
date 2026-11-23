/*
 * prom.c - Prom: プロセスモニター (C言語 + WebView2)
 *
 * 役割分担:
 *   C   : プロセス/ディスク/ネットワークの情報収集、アイコン抽出、プロセス終了、
 *         ウィンドウ (半透明のアクリル効果)
 *   HTML: 画面表示 (ui/index.html)  ← デザインはここを編集するだけ
 *
 * 通信 (すべて文字列/JSON):
 *   JS -> C : "init" | "refresh" | "kill <pid>" | "killtree <pid> ..." | "open <pid>"
 *   C -> JS : {"type":"host"} | {"type":"update"} | {"type":"icons"} | {"type":"kill"}
 *
 * 起動オプション: prom.exe /noglass   半透明効果を使わない (不具合の切り分け用)
 */
#define UNICODE
#define _UNICODE
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dwmapi.h>
#include <strsafe.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "WebView2.h"

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif

#define URL_MAX 2084
#define MAX_SAMPLES 8192
#define ICON_PX 48      /* 送るアイコンの大きさ (px) */
#define ICON_BATCH 40   /* 1回の更新で新しく取り出すアイコンの最大数 */
#define ICON_QUEUE 2048 /* アイコン待ちの行列 (一度に溢れても取りこぼさない) */
#define MAX_SENT 16384

/* ui/index.html の --bg と合わせる (半透明が使えないときのタイトルバーの色) */
#define BG_DARK RGB(7, 7, 10)

static HWND g_hwnd;
static ICoreWebView2Controller *g_controller;
static ICoreWebView2 *g_webview;
static int g_numCpu = 1;
#define IDI_APP 1 /* prom.rc の "1 ICON" と対応 */

static BOOL g_glass; /* 半透明(アクリル)が有効か */
static BOOL g_noGlass;

/* ICoreWebView2Controller2 の IID (WebView2.h は C 向けに IID の実体を持たないため自前で定義) */
static const GUID kIID_Controller2 =
    {0xc979903e, 0xd4ca, 0x4228, {0x92, 0xeb, 0x47, 0xee, 0x3f, 0xa9, 0x6e, 0xab}};

/* ======================================================================
 *  文字列バッファ (JSON組み立て用)
 * ==================================================================== */
typedef struct
{
    wchar_t *p;
    size_t len, cap;
} Buf;

static BOOL BufReserve(Buf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap)
        return TRUE;
    size_t ncap = b->cap ? b->cap : 8192;
    while (ncap < b->len + extra + 1)
        ncap *= 2;
    wchar_t *np = (wchar_t *)realloc(b->p, ncap * sizeof(wchar_t));
    if (!np)
        return FALSE;
    b->p = np;
    b->cap = ncap;
    return TRUE;
}

static void BufAdd(Buf *b, const wchar_t *s, size_t n)
{
    if (!BufReserve(b, n))
        return;
    memcpy(b->p + b->len, s, n * sizeof(wchar_t));
    b->len += n;
    b->p[b->len] = 0;
}

static void BufStr(Buf *b, const wchar_t *s) { BufAdd(b, s, wcslen(s)); }

static void BufFmt(Buf *b, const wchar_t *fmt, ...)
{
    wchar_t tmp[512];
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfW(tmp, ARRAYSIZE(tmp), fmt, ap);
    va_end(ap);
    BufStr(b, tmp);
}

/* JSON文字列としてエスケープして追加 */
static void BufJsonStr(Buf *b, const wchar_t *s)
{
    BufAdd(b, L"\"", 1);
    for (; *s; s++)
    {
        switch (*s)
        {
        case L'"':
            BufStr(b, L"\\\"");
            break;
        case L'\\':
            BufStr(b, L"\\\\");
            break;
        default:
            if (*s < 0x20)
                BufFmt(b, L"\\u%04x", (unsigned)*s);
            else
                BufAdd(b, s, 1);
        }
    }
    BufAdd(b, L"\"", 1);
}

static void BufBase64(Buf *b, const BYTE *d, size_t n)
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < n; i += 3)
    {
        unsigned v = (unsigned)d[i] << 16;
        if (i + 1 < n)
            v |= (unsigned)d[i + 1] << 8;
        if (i + 2 < n)
            v |= d[i + 2];
        wchar_t c[4];
        c[0] = (wchar_t)T[(v >> 18) & 63];
        c[1] = (wchar_t)T[(v >> 12) & 63];
        c[2] = (i + 1 < n) ? (wchar_t)T[(v >> 6) & 63] : L'=';
        c[3] = (i + 2 < n) ? (wchar_t)T[v & 63] : L'=';
        BufAdd(b, c, 4);
    }
}

static void PostJson(const wchar_t *json)
{
    if (g_webview)
        g_webview->lpVtbl->PostWebMessageAsJson(g_webview, json);
}

/* ======================================================================
 *  アイコン: HICON -> RGBA -> PNG(無圧縮deflate) -> base64
 * ==================================================================== */
static DWORD g_crcTable[256];
static BOOL g_crcReady;

static DWORD Crc32(const BYTE *p, size_t n)
{
    if (!g_crcReady)
    {
        for (DWORD i = 0; i < 256; i++)
        {
            DWORD c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            g_crcTable[i] = c;
        }
        g_crcReady = TRUE;
    }
    DWORD crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        crc = g_crcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void Put32(BYTE *d, DWORD v)
{
    d[0] = (BYTE)(v >> 24);
    d[1] = (BYTE)(v >> 16);
    d[2] = (BYTE)(v >> 8);
    d[3] = (BYTE)v;
}

static BYTE *PutChunk(BYTE *d, const char *type, const BYTE *data, DWORD len)
{
    Put32(d, len);
    memcpy(d + 4, type, 4);
    if (len)
        memcpy(d + 8, data, len);
    Put32(d + 8 + len, Crc32(d + 4, 4 + (size_t)len)); /* CRC は type + data */
    return d + 12 + len;
}

/* RGBA(w x h) を PNG にする。呼び出し側が free する */
static BYTE *EncodePng(const BYTE *rgba, int w, int h, size_t *outLen)
{
    size_t stride = 1 + (size_t)w * 4;
    size_t rawLen = stride * (size_t)h;
    BYTE *raw = (BYTE *)malloc(rawLen);
    if (!raw)
        return NULL;
    for (int y = 0; y < h; y++)
    {
        raw[y * stride] = 0; /* フィルタなし */
        memcpy(raw + y * stride + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }

    /* zlib: 無圧縮(stored)ブロックで包む */
    size_t blocks = (rawLen + 65534) / 65535;
    size_t zLen = 2 + rawLen + blocks * 5 + 4;
    BYTE *z = (BYTE *)malloc(zLen);
    if (!z)
    {
        free(raw);
        return NULL;
    }
    size_t zp = 0, off = 0;
    z[zp++] = 0x78;
    z[zp++] = 0x01;
    while (off < rawLen)
    {
        size_t n = rawLen - off;
        if (n > 65535)
            n = 65535;
        z[zp++] = (off + n == rawLen) ? 1 : 0;
        z[zp++] = (BYTE)(n & 0xFF);
        z[zp++] = (BYTE)((n >> 8) & 0xFF);
        z[zp++] = (BYTE)(~n & 0xFF);
        z[zp++] = (BYTE)((~n >> 8) & 0xFF);
        memcpy(z + zp, raw + off, n);
        zp += n;
        off += n;
    }
    DWORD a = 1, b = 0;
    for (size_t i = 0; i < rawLen; i++)
    {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    Put32(z + zp, (b << 16) | a);
    zp += 4;
    free(raw);

    BYTE *png = (BYTE *)malloc(8 + 25 + 12 + zp + 12);
    if (!png)
    {
        free(z);
        return NULL;
    }
    static const BYTE sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    memcpy(png, sig, 8);
    BYTE ihdr[13];
    Put32(ihdr, (DWORD)w);
    Put32(ihdr + 4, (DWORD)h);
    ihdr[8] = 8;
    ihdr[9] = 6;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0; /* 8bit RGBA */
    BYTE *p = png + 8;
    p = PutChunk(p, "IHDR", ihdr, 13);
    p = PutChunk(p, "IDAT", z, (DWORD)zp);
    p = PutChunk(p, "IEND", NULL, 0);
    free(z);
    *outLen = (size_t)(p - png);
    return png;
}

/*
 * アイコンを黒背景・白背景の2回に描いて、差から透明度を求める。
 * (アイコンの内部形式に依存せず、正しいアルファが得られる)
 */
static BOOL RenderIconRGBA(HICON hi, BYTE *rgba)
{
    const size_t n = (size_t)ICON_PX * ICON_PX;
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = ICON_PX;
    bi.bmiHeader.biHeight = -ICON_PX; /* 上から下 */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    BYTE *black = (BYTE *)malloc(n * 4);
    BOOL ok = FALSE;

    if (dc && bmp && bits && black)
    {
        HGDIOBJ old = SelectObject(dc, bmp);

        memset(bits, 0x00, n * 4);
        DrawIconEx(dc, 0, 0, hi, ICON_PX, ICON_PX, 0, NULL, DI_NORMAL);
        GdiFlush();
        memcpy(black, bits, n * 4);

        memset(bits, 0xFF, n * 4);
        DrawIconEx(dc, 0, 0, hi, ICON_PX, ICON_PX, 0, NULL, DI_NORMAL);
        GdiFlush();

        const BYTE *white = (const BYTE *)bits; /* B,G,R,X の順 */
        for (size_t i = 0; i < n; i++)
        {
            const BYTE *k = black + i * 4, *w = white + i * 4;
            int diff = ((int)w[0] - k[0] + (int)w[1] - k[1] + (int)w[2] - k[2]) / 3;
            int a = 255 - diff;
            if (a < 0)
                a = 0;
            if (a > 255)
                a = 255;
            BYTE *o = rgba + i * 4;
            if (a == 0)
            {
                o[0] = o[1] = o[2] = o[3] = 0;
            }
            else
            {
                int r = k[2] * 255 / a, g = k[1] * 255 / a, bl = k[0] * 255 / a;
                o[0] = (BYTE)(r > 255 ? 255 : r);
                o[1] = (BYTE)(g > 255 ? 255 : g);
                o[2] = (BYTE)(bl > 255 ? 255 : bl);
                o[3] = (BYTE)a;
            }
        }
        SelectObject(dc, old);
        ok = TRUE;
    }
    free(black);
    if (bmp)
        DeleteObject(bmp);
    if (dc)
        DeleteDC(dc);
    return ok;
}

/* path が NULL のときは「汎用の実行ファイルアイコン」 */
static HICON LoadAppIcon(const wchar_t *path)
{
    HICON hi = NULL;
    if (path)
    {
        UINT got = PrivateExtractIconsW(path, 0, ICON_PX, ICON_PX, &hi, NULL, 1, 0);
        if (got != 0 && got != 0xFFFFFFFFu && hi)
            return hi;
        hi = NULL;
    }
    SHFILEINFOW sfi;
    ZeroMemory(&sfi, sizeof(sfi));
    if (path)
        SHGetFileInfoW(path, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON);
    else
        SHGetFileInfoW(L"x.exe", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES);
    return sfi.hIcon;
}

static void AppendIcon(Buf *b, BOOL *first, const wchar_t *key, HICON hi)
{
    static BYTE rgba[ICON_PX * ICON_PX * 4];
    if (!RenderIconRGBA(hi, rgba))
        return;
    size_t len = 0;
    BYTE *png = EncodePng(rgba, ICON_PX, ICON_PX, &len);
    if (!png)
        return;
    BufStr(b, *first ? L"{\"k\":" : L",{\"k\":");
    *first = FALSE;
    BufJsonStr(b, key);
    BufStr(b, L",\"d\":\"");
    BufBase64(b, png, len);
    BufStr(b, L"\"}");
    free(png);
}

/* 送信済みパスの管理 (パスのハッシュだけ覚える) */
static ULONGLONG g_sent[MAX_SENT];
static int g_sentCount;
static BOOL g_genericSent;

static ULONGLONG HashPath(const wchar_t *s)
{
    ULONGLONG h = 1469598103934665603ULL;
    for (; *s; s++)
    {
        wchar_t c = *s;
        if (c >= L'A' && c <= L'Z')
            c = (wchar_t)(c + 32);
        h ^= (ULONGLONG)c;
        h *= 1099511628211ULL;
    }
    return h;
}

static BOOL IconSent(ULONGLONG h)
{
    for (int i = 0; i < g_sentCount; i++)
        if (g_sent[i] == h)
            return TRUE;
    return FALSE;
}

/* まだ送っていないアイコンの待ち行列。
 * (以前は1回の更新で取り出せる数を超えた分を捨てていたため、起動直後に大量にあるプロセスの
 *  アイコンが取りこぼされていた。ここに溜めて、毎回少しずつ送る) */
static wchar_t g_iconQ[ICON_QUEUE][MAX_PATH * 2];
static ULONGLONG g_iconQH[ICON_QUEUE];
static int g_iconQCount;

static BOOL IconQueued(ULONGLONG h)
{
    for (int i = 0; i < g_iconQCount; i++)
        if (g_iconQH[i] == h)
            return TRUE;
    return FALSE;
}

/* ======================================================================
 *  ディスク (PDH) / ネットワーク (GetIfTable2)
 * ==================================================================== */
static PDH_HQUERY g_pdhQuery;
static PDH_HCOUNTER g_pdhIdle, g_pdhRead, g_pdhWrite;
static BOOL g_pdhOk;

static void DiskInit(void)
{
    if (PdhOpenQueryW(NULL, 0, &g_pdhQuery) != ERROR_SUCCESS)
        return;
    PDH_STATUS a = PdhAddEnglishCounterW(g_pdhQuery, L"\\PhysicalDisk(_Total)\\% Idle Time", 0, &g_pdhIdle);
    PDH_STATUS b = PdhAddEnglishCounterW(g_pdhQuery, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", 0, &g_pdhRead);
    PDH_STATUS c = PdhAddEnglishCounterW(g_pdhQuery, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", 0, &g_pdhWrite);
    g_pdhOk = (a == ERROR_SUCCESS && b == ERROR_SUCCESS && c == ERROR_SUCCESS);
    if (g_pdhOk)
        PdhCollectQueryData(g_pdhQuery);
}

static double PdhValue(PDH_HCOUNTER c)
{
    PDH_FMT_COUNTERVALUE v;
    if (PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, NULL, &v) == ERROR_SUCCESS &&
        (v.CStatus == PDH_CSTATUS_VALID_DATA || v.CStatus == PDH_CSTATUS_NEW_DATA))
        return v.doubleValue;
    return 0;
}

static void ReadDisk(double *active, double *rd, double *wr)
{
    *active = *rd = *wr = 0;
    if (!g_pdhOk || PdhCollectQueryData(g_pdhQuery) != ERROR_SUCCESS)
        return;
    double idle = PdhValue(g_pdhIdle);
    *active = 100.0 - idle;
    if (*active < 0)
        *active = 0;
    if (*active > 100)
        *active = 100;
    *rd = PdhValue(g_pdhRead);
    *wr = PdhValue(g_pdhWrite);
}

/* ---- GPU使用率 (PDH の "GPU Engine" カウンター。Windows 10 1709 以降) ----
 * タスクマネージャーと同じ考え方:
 *   全体   = 各エンジン (3D / Copy / VideoDecode ...) の使用率の合計のうち最大のもの
 *   プロセス = そのプロセスのエンジンごとの使用率のうち最大のもの */
#define GPU_PIDS 2048
#define GPU_ENGS 128
static PDH_HQUERY g_gpuQuery;
static PDH_HCOUNTER g_gpuCounter;
static BOOL g_gpuOk;
static DWORD g_gpuPid[GPU_PIDS];
static double g_gpuVal[GPU_PIDS];
static int g_gpuN;

static void GpuInit(void)
{
    if (PdhOpenQueryW(NULL, 0, &g_gpuQuery) != ERROR_SUCCESS)
        return;
    if (PdhAddEnglishCounterW(g_gpuQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_gpuCounter) == ERROR_SUCCESS)
    {
        g_gpuOk = TRUE;
        PdhCollectQueryData(g_gpuQuery);
    }
}

static double GpuOfPid(DWORD pid)
{
    for (int i = 0; i < g_gpuN; i++)
        if (g_gpuPid[i] == pid)
            return g_gpuVal[i];
    return 0;
}

static double ReadGpu(void)
{
    static wchar_t engKey[GPU_ENGS][96];
    static double engSum[GPU_ENGS];
    int engN = 0;
    g_gpuN = 0;
    if (!g_gpuOk || PdhCollectQueryData(g_gpuQuery) != ERROR_SUCCESS)
        return 0;

    DWORD size = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(g_gpuCounter, PDH_FMT_DOUBLE, &size, &count, NULL) != PDH_MORE_DATA || size == 0)
        return 0;
    PDH_FMT_COUNTERVALUE_ITEM_W *items = (PDH_FMT_COUNTERVALUE_ITEM_W *)malloc(size);
    if (!items)
        return 0;
    if (PdhGetFormattedCounterArrayW(g_gpuCounter, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS)
    {
        free(items);
        return 0;
    }

    for (DWORD i = 0; i < count; i++)
    {
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
            continue;
        double v = items[i].FmtValue.doubleValue;
        if (v <= 0)
            continue;
        const wchar_t *name = items[i].szName;
        if (!name)
            continue;
        /* 名前の形: pid_1234_luid_0x00000000_0x0000F2E5_phys_0_eng_0_engtype_3D */
        const wchar_t *pp = wcsstr(name, L"pid_");
        const wchar_t *lk = wcsstr(name, L"luid_");
        if (!pp || !lk)
            continue;
        DWORD pid = (DWORD)wcstoul(pp + 4, NULL, 10);

        int e = 0;
        while (e < engN && wcscmp(engKey[e], lk) != 0)
            e++;
        if (e == engN && engN < GPU_ENGS)
        {
            StringCchCopyW(engKey[engN], 96, lk);
            engSum[engN] = 0;
            engN++;
        }
        if (e < engN)
            engSum[e] += v;

        int k = 0;
        while (k < g_gpuN && g_gpuPid[k] != pid)
            k++;
        if (k == g_gpuN && g_gpuN < GPU_PIDS)
        {
            g_gpuPid[g_gpuN] = pid;
            g_gpuVal[g_gpuN] = 0;
            g_gpuN++;
        }
        if (k < g_gpuN && v > g_gpuVal[k])
            g_gpuVal[k] = v;
    }
    free(items);

    double total = 0;
    for (int e = 0; e < engN; e++)
        if (engSum[e] > total)
            total = engSum[e];
    for (int k = 0; k < g_gpuN; k++)
        if (g_gpuVal[k] > 100)
            g_gpuVal[k] = 100;
    return total > 100 ? 100 : total;
}

static ULONGLONG g_netInPrev, g_netOutPrev;
static BOOL g_netHave;

static void ReadNet(double *rx, double *tx, ULONGLONG elapsed100ns)
{
    *rx = *tx = 0;
    PMIB_IF_TABLE2 t = NULL;
    if (GetIfTable2(&t) != NO_ERROR || !t)
        return;

    ULONGLONG in = 0, out = 0;
    for (ULONG i = 0; i < t->NumEntries; i++)
    {
        const MIB_IF_ROW2 *r = &t->Table[i];
        if (r->Type == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        if (r->OperStatus != IfOperStatusUp)
            continue;
        if (!r->InterfaceAndOperStatusFlags.HardwareInterface)
            continue; /* 物理NICのみ */
        in += r->InOctets;
        out += r->OutOctets;
    }
    FreeMibTable(t);

    if (g_netHave && elapsed100ns > 0 && in >= g_netInPrev && out >= g_netOutPrev)
    {
        double sec = (double)elapsed100ns / 1e7;
        *rx = (double)(in - g_netInPrev) / sec;
        *tx = (double)(out - g_netOutPrev) / sec;
    }
    g_netInPrev = in;
    g_netOutPrev = out;
    g_netHave = TRUE;
}

/* ======================================================================
 *  プロセス情報の収集 → JSON を JS へ送信
 * ==================================================================== */
/* CPU時間(100ns) と プロセス作成時刻。pid+作成時刻で「同じプロセスか」を判定する */
typedef struct
{
    DWORD pid;
    ULONGLONG cpu;
    ULONGLONG ct;
} Sample;

static Sample g_prev[MAX_SAMPLES], g_cur[MAX_SAMPLES];
static int g_prevCount, g_curCount;
static ULONGLONG g_prevTime;

static ULONGLONG FtToU64(FILETIME ft)
{
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static const Sample *FindPrev(DWORD pid, ULONGLONG ct)
{
    for (int i = 0; i < g_prevCount; i++)
        if (g_prev[i].pid == pid && g_prev[i].ct == ct)
            return &g_prev[i];
    return NULL;
}

/* 固定ドライブの容量を JSON 配列にする: [{"n":"C:","t":合計,"f":空き},...] */
static void ReadDrives(wchar_t *out, size_t cch)
{
    StringCchCopyW(out, cch, L"[");
    DWORD mask = GetLogicalDrives();
    BOOL first = TRUE;
    for (int i = 0; i < 26; i++)
    {
        if (!(mask & (1u << i)))
            continue;
        wchar_t root[4] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED)
            continue;
        ULARGE_INTEGER avail, total, freeB;
        if (!GetDiskFreeSpaceExW(root, &avail, &total, &freeB))
            continue;
        wchar_t item[96];
        StringCchPrintfW(item, ARRAYSIZE(item), L"%ls{\"n\":\"%lc:\",\"t\":%llu,\"f\":%llu}",
                         first ? L"" : L",", root[0],
                         (unsigned long long)total.QuadPart, (unsigned long long)freeB.QuadPart);
        StringCchCatW(out, cch, item);
        first = FALSE;
    }
    StringCchCatW(out, cch, L"]");
}

/* 「アプリ」= 画面に見える通常のウィンドウを持つプロセス (タスクマネージャーと同じ考え方) */
static DWORD g_appPids[MAX_SAMPLES];
static int g_appCount;

static BOOL CALLBACK EnumAppWnd(HWND h, LPARAM lp)
{
    (void)lp;
    if (!IsWindowVisible(h))
        return TRUE;
    if (GetWindow(h, GW_OWNER))
        return TRUE; /* 子ダイアログなどは除く */
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
        return TRUE;
    RECT wr; /* 大きさのない窓は除く */
    if (!GetWindowRect(h, &wr) || wr.right - wr.left < 2 || wr.bottom - wr.top < 2)
        return TRUE;
    DWORD cloaked = 0; /* 最小化された UWP 等の隠れた窓 */
    DwmGetWindowAttribute(h, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked));
    if (cloaked)
        return TRUE;
    wchar_t cls[32];
    if (GetClassNameW(h, cls, ARRAYSIZE(cls)) && /* デスクトップ・タスクバー */
        (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
         wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0))
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid)
        return TRUE;
    for (int i = 0; i < g_appCount; i++)
        if (g_appPids[i] == pid)
            return TRUE;
    if (g_appCount < MAX_SAMPLES)
        g_appPids[g_appCount++] = pid;
    return TRUE;
}

static BOOL IsAppPid(DWORD pid)
{
    for (int i = 0; i < g_appCount; i++)
        if (g_appPids[i] == pid)
            return TRUE;
    return FALSE;
}

static void SendUpdate(void)
{
    if (!g_webview)
        return;

    static wchar_t drives[2048] = L"[]";
    static unsigned tickNo;

    if (tickNo++ % 5 == 0)
        ReadDrives(drives, ARRAYSIZE(drives)); /* 容量は頻繁に変わらないので5回に1回 */

    g_appCount = 0;
    EnumWindows(EnumAppWnd, 0);

    Buf b = {0};
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULONGLONG now = FtToU64(nowFt);
    ULONGLONG elapsed = g_prevTime ? now - g_prevTime : 0;

    /* システム全体のCPU使用率 */
    static ULONGLONG sIdle, sTotal;
    double sysCpu = 0;
    FILETIME fi, fk, fu;
    if (GetSystemTimes(&fi, &fk, &fu))
    {
        ULONGLONG idle = FtToU64(fi);
        ULONGLONG total = FtToU64(fk) + FtToU64(fu); /* kernel には idle が含まれる */
        if (sTotal && total > sTotal)
            sysCpu = (double)((total - sTotal) - (idle - sIdle)) * 100.0 / (double)(total - sTotal);
        sIdle = idle;
        sTotal = total;
    }
    if (sysCpu < 0)
        sysCpu = 0;
    if (sysCpu > 100)
        sysCpu = 100;

    MEMORYSTATUSEX ms;
    ZeroMemory(&ms, sizeof(ms));
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    double dActive, dRead, dWrite, nRx, nTx;
    ReadDisk(&dActive, &dRead, &dWrite);
    ReadNet(&nRx, &nTx, elapsed);
    double gpuUse = ReadGpu();

    BufFmt(&b,
           L"{\"type\":\"update\",\"cpu\":%.2f,\"cores\":%d,\"memTotal\":%llu,\"memUsed\":%llu,"
           L"\"disk\":{\"active\":%.2f,\"read\":%.0f,\"write\":%.0f},"
           L"\"net\":{\"rx\":%.0f,\"tx\":%.0f},\"drives\":",
           sysCpu, g_numCpu,
           (unsigned long long)ms.ullTotalPhys,
           (unsigned long long)(ms.ullTotalPhys - ms.ullAvailPhys),
           dActive, dRead, dWrite, nRx, nTx);
    BufStr(&b, drives);
    BufFmt(&b, L",\"gpuOk\":%ls,\"gpu\":%.2f,\"procs\":[", g_gpuOk ? L"true" : L"false", gpuUse);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    g_curCount = 0;

    if (snap != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(pe);
        BOOL first = TRUE;

        if (Process32FirstW(snap, &pe))
        {
            do
            {
                double cpuPct = -1; /* -1 = 取得不可 */
                unsigned long long mem = 0;
                BOOL isNew = FALSE; /* 初めて見るプロセス = パスを送る */
                wchar_t path[MAX_PATH * 2];
                path[0] = 0;

                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (h)
                {
                    FILETIME c, e, k, u;
                    if (GetProcessTimes(h, &c, &e, &k, &u))
                    {
                        ULONGLONG cpu = FtToU64(k) + FtToU64(u);
                        ULONGLONG ct = FtToU64(c);
                        if (g_curCount < MAX_SAMPLES)
                        {
                            g_cur[g_curCount].pid = pe.th32ProcessID;
                            g_cur[g_curCount].cpu = cpu;
                            g_cur[g_curCount].ct = ct;
                            g_curCount++;
                        }
                        const Sample *p = FindPrev(pe.th32ProcessID, ct);
                        if (p)
                        {
                            if (elapsed > 0 && cpu >= p->cpu)
                            {
                                cpuPct = (double)(cpu - p->cpu) * 100.0 / ((double)elapsed * g_numCpu);
                                if (cpuPct > 100)
                                    cpuPct = 100;
                            }
                        }
                        else
                        {
                            cpuPct = 0;
                            isNew = TRUE;
                        }
                    }

                    PROCESS_MEMORY_COUNTERS pmc;
                    pmc.cb = sizeof(pmc);
                    if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc)))
                        mem = pmc.WorkingSetSize;

                    if (isNew)
                    { /* パス取得は重いので、新しいプロセスのときだけ */
                        DWORD len = ARRAYSIZE(path);
                        if (!QueryFullProcessImageNameW(h, 0, path, &len))
                            path[0] = 0;
                    }
                    CloseHandle(h);
                }

                /* まだアイコンを送っていないパスを控える */
                if (path[0] && g_iconQCount < ICON_QUEUE && g_sentCount + g_iconQCount < MAX_SENT)
                {
                    ULONGLONG hv = HashPath(path);
                    if (!IconSent(hv) && !IconQueued(hv))
                    {
                        g_iconQH[g_iconQCount] = hv;
                        StringCchCopyW(g_iconQ[g_iconQCount], MAX_PATH * 2, path);
                        g_iconQCount++;
                    }
                }

                BufFmt(&b, L"%ls{\"pid\":%lu,\"pp\":%lu,\"th\":%lu,\"name\":",
                       first ? L"" : L",", pe.th32ProcessID, pe.th32ParentProcessID, pe.cntThreads);
                first = FALSE;
                BufJsonStr(&b, pe.szExeFile);
                BufFmt(&b, L",\"cpu\":%.2f,\"mem\":%llu", cpuPct, mem);
                if (IsAppPid(pe.th32ProcessID))
                    BufStr(&b, L",\"app\":1");
                {
                    double gv = GpuOfPid(pe.th32ProcessID);
                    if (gv >= 0.05)
                        BufFmt(&b, L",\"gpu\":%.1f", gv);
                }
                if (isNew)
                {
                    BufStr(&b, L",\"path\":");
                    BufJsonStr(&b, path);
                }
                BufStr(&b, L"}");
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    BufStr(&b, L"]}");

    memcpy(g_prev, g_cur, sizeof(Sample) * (size_t)g_curCount);
    g_prevCount = g_curCount;
    g_prevTime = now;

    if (b.p)
        PostJson(b.p);
    free(b.p);

    /* アイコンは一覧を送ったあとで、待ち行列から少しずつ送る */
    if (g_iconQCount > 0 || !g_genericSent)
    {
        Buf ib = {0};
        BOOL f = TRUE;
        BufStr(&ib, L"{\"type\":\"icons\",\"items\":[");
        if (!g_genericSent)
        {
            HICON hi = LoadAppIcon(NULL);
            if (hi)
            {
                AppendIcon(&ib, &f, L"*", hi);
                DestroyIcon(hi);
            }
            g_genericSent = TRUE;
        }
        int n = g_iconQCount < ICON_BATCH ? g_iconQCount : ICON_BATCH;
        for (int i = 0; i < n; i++)
        {
            HICON hi = LoadAppIcon(g_iconQ[i]);
            if (hi)
            {
                AppendIcon(&ib, &f, g_iconQ[i], hi);
                DestroyIcon(hi);
            }
            if (g_sentCount < MAX_SENT)
                g_sent[g_sentCount++] = g_iconQH[i]; /* 取れなかったものも再試行しない */
        }
        if (n > 0)
        {
            memmove(g_iconQ, g_iconQ + n, sizeof(g_iconQ[0]) * (size_t)(g_iconQCount - n));
            memmove(g_iconQH, g_iconQH + n, sizeof(g_iconQH[0]) * (size_t)(g_iconQCount - n));
            g_iconQCount -= n;
        }
        BufStr(&ib, L"]}");
        if (ib.p)
            PostJson(ib.p);
        free(ib.p);
    }
}

/* ======================================================================
 *  プロセスの終了 / 初期化 / ウィンドウ効果
 * ==================================================================== */
static void KillProcess(DWORD pid)
{
    BOOL ok = FALSE;
    DWORD err = 0;

    if (pid == 0 || pid == 4 || pid == GetCurrentProcessId())
    {
        err = ERROR_ACCESS_DENIED;
    }
    else
    {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h)
        {
            ok = TerminateProcess(h, 1);
            if (!ok)
                err = GetLastError();
            CloseHandle(h);
        }
        else
        {
            err = GetLastError();
        }
    }

    wchar_t msg[160];
    StringCchPrintfW(msg, ARRAYSIZE(msg),
                     L"{\"type\":\"kill\",\"pid\":%lu,\"ok\":%ls,\"error\":%lu}",
                     pid, ok ? L"true" : L"false", err);
    PostJson(msg);
}

/* プロセスツリー終了: 指定したプロセスとその子孫を、末端(子)から順に終了する。
 * PIDの使い回しで無関係なプロセスを巻き込まないよう、「子の作成時刻 >= 親の作成時刻」も確認する。 */
#define TREE_MAX 4096
typedef struct
{
    DWORD pid, ppid;
    ULONGLONG ct;
    BYTE mark;
} PNode;
static PNode g_nodes[TREE_MAX];
static int g_treeOrder[TREE_MAX];

static void KillTree(const wchar_t *args)
{
    DWORD roots[256];
    int rc = 0;
    for (const wchar_t *p = args; *p && rc < 256;)
    {
        while (*p == L' ')
            p++;
        if (!*p)
            break;
        wchar_t *end = NULL;
        DWORD v = (DWORD)wcstoul(p, &end, 10);
        if (end == p)
            break;
        roots[rc++] = v;
        p = end;
    }

    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe))
        {
            do
            {
                if (n >= TREE_MAX)
                    break;
                g_nodes[n].pid = pe.th32ProcessID;
                g_nodes[n].ppid = pe.th32ParentProcessID;
                g_nodes[n].ct = 0;
                g_nodes[n].mark = 0;
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (h)
                {
                    FILETIME c, e, k, u;
                    if (GetProcessTimes(h, &c, &e, &k, &u))
                        g_nodes[n].ct = FtToU64(c);
                    CloseHandle(h);
                }
                n++;
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    int oc = 0;
    for (int i = 0; i < n; i++)
        for (int r = 0; r < rc; r++)
            if (g_nodes[i].pid == roots[r])
            {
                g_nodes[i].mark = 1;
                g_treeOrder[oc++] = i;
                break;
            }
    for (int q = 0; q < oc; q++)
    { /* 幅優先で子孫を集める */
        const PNode parent = g_nodes[g_treeOrder[q]];
        for (int j = 0; j < n; j++)
        {
            if (g_nodes[j].mark || g_nodes[j].pid == 0)
                continue;
            if (g_nodes[j].ppid != parent.pid)
                continue;
            if (g_nodes[j].ct < parent.ct)
                continue; /* 親より古い = PID使い回しの別プロセス */
            g_nodes[j].mark = 1;
            g_treeOrder[oc++] = j;
        }
    }

    int total = 0, failed = 0;
    DWORD err = 0, self = GetCurrentProcessId();
    for (int q = oc - 1; q >= 0; q--)
    { /* 子 -> 親の順 */
        DWORD pid = g_nodes[g_treeOrder[q]].pid;
        if (pid == 0 || pid == 4 || pid == self)
            continue;
        total++;
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h)
        {
            if (!TerminateProcess(h, 1))
            {
                failed++;
                err = GetLastError();
            }
            CloseHandle(h);
        }
        else
        {
            failed++;
            err = GetLastError();
        }
    }

    wchar_t msg[200];
    StringCchPrintfW(msg, ARRAYSIZE(msg),
                     L"{\"type\":\"kill\",\"pid\":0,\"ok\":%ls,\"error\":%lu,\"count\":%d,\"failed\":%d}",
                     failed == 0 ? L"true" : L"false", err, total, failed);
    PostJson(msg);
}

/* ファイルの場所を開く: エクスプローラーでそのプロセスの exe を選択した状態で開く。
 * パスは JS からではなく PID から C が調べる (JS から任意のパスを渡せないようにするため)。 */
static void OpenLocation(DWORD pid)
{
    BOOL ok = FALSE;
    wchar_t path[MAX_PATH * 2];
    path[0] = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h)
    {
        DWORD len = ARRAYSIZE(path);
        if (!QueryFullProcessImageNameW(h, 0, path, &len))
            path[0] = 0;
        CloseHandle(h);
    }
    if (path[0])
    {
        wchar_t args[MAX_PATH * 2 + 16];
        StringCchPrintfW(args, ARRAYSIZE(args), L"/select,\"%ls\"", path);
        ok = (INT_PTR)ShellExecuteW(g_hwnd, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL) > 32;
    }
    PostJson(ok ? L"{\"type\":\"open\",\"ok\":true}" : L"{\"type\":\"open\",\"ok\":false}");
}

static void ApplyCaptionColor(void)
{
    COLORREF cap = BG_DARK;
    DwmSetWindowAttribute(g_hwnd, 35 /* DWMWA_CAPTION_COLOR (Win11) */, &cap, sizeof(cap));
}

/* Windows 11 が描く 1px の枠線を消す (最大化時に上端へ黒線が出るのを防ぐ) */
static void ApplyBorderColor(void)
{
    COLORREF none = 0xFFFFFFFE; /* DWMWA_COLOR_NONE */
    DwmSetWindowAttribute(g_hwnd, 34 /* DWMWA_BORDER_COLOR */, &none, sizeof(none));
}

/* 半透明(アクリル)の有効化。Windows 11 22H2 以降。使えなければ不透明のダークになる */
static void EnableGlass(void)
{
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    if (g_noGlass)
    {
        ApplyCaptionColor();
        return;
    }

    MARGINS m = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(g_hwnd, &m);
    int type = 3; /* DWMSBT_TRANSIENTWINDOW = アクリル */
    g_glass = SUCCEEDED(DwmSetWindowAttribute(g_hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &type, sizeof(type)));
    ApplyBorderColor();
    if (!g_glass)
    {
        MARGINS z = {0, 0, 0, 0};
        DwmExtendFrameIntoClientArea(g_hwnd, &z);
        ApplyCaptionColor();
    }
}

/* 切り替え後に枠・背景を描き直す (これがないと見た目が変わらないことがある) */
static void RefreshFrame(void)
{
    SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(g_hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

static void DisableGlass(void)
{
    int type = 1; /* DWMSBT_NONE */
    DwmSetWindowAttribute(g_hwnd, 38, &type, sizeof(type));
    MARGINS z = {0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(g_hwnd, &z);
    ApplyCaptionColor();
    ApplyBorderColor();
    g_glass = FALSE;
    RefreshFrame();
}

static BOOL g_wvTransparent = TRUE; /* WebView の背景を透明にできたか */

/* 半透明 <-> 不透明 の切り替え (ヘッダーのボタンから呼ばれる) */
static void SetGlass(BOOL on)
{
    if (!on || g_noGlass || !g_wvTransparent)
    {
        DisableGlass();
        return;
    }
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF def = 0xFFFFFFFF; /* DWMWA_COLOR_DEFAULT: DisableGlass で塗った不透明色を解除 */
    DwmSetWindowAttribute(g_hwnd, 35 /* DWMWA_CAPTION_COLOR */, &def, sizeof(def));
    MARGINS m = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(g_hwnd, &m);
    int type = 3; /* DWMSBT_TRANSIENTWINDOW = アクリル */
    g_glass = SUCCEEDED(DwmSetWindowAttribute(g_hwnd, 38, &type, sizeof(type)));
    if (!g_glass)
        DisableGlass();
    else
        RefreshFrame();
}

/* host メッセージ (半透明の状態・コア数・Windows フォルダ) を JS に送る */
static void PostHost(void)
{
    wchar_t winDir[MAX_PATH];
    if (!GetWindowsDirectoryW(winDir, MAX_PATH))
        winDir[0] = 0;
    Buf b = {0};
    BufFmt(&b, L"{\"type\":\"host\",\"glass\":%ls,\"cores\":%d,\"win\":",
           g_glass ? L"true" : L"false", g_numCpu);
    BufJsonStr(&b, winDir);
    BufStr(&b, L"}");
    if (b.p)
        PostJson(b.p);
    free(b.p);
}

static void OnInit(void)
{
    g_sentCount = 0;
    g_iconQCount = 0;
    g_genericSent = FALSE;
    g_prevCount = 0; /* 全プロセスを「新規」扱いにして、パスを送り直す */
    g_prevTime = 0;
    PostHost();
}

static void HandleMessage(const wchar_t *m)
{
    if (wcscmp(m, L"init") == 0)
        OnInit();
    else if (wcscmp(m, L"refresh") == 0)
    {
        if (!IsIconic(g_hwnd))
            SendUpdate(); /* 最小化中は何もしない (軽量化) */
    }
    else if (wcsncmp(m, L"glass ", 6) == 0)
    {
        SetGlass(m[6] == L'1');
        PostHost();
    }
    else if (wcsncmp(m, L"kill ", 5) == 0)
        KillProcess((DWORD)wcstoul(m + 5, NULL, 10));
    else if (wcsncmp(m, L"killtree ", 9) == 0)
        KillTree(m + 9);
    else if (wcsncmp(m, L"open ", 5) == 0)
        OpenLocation((DWORD)wcstoul(m + 5, NULL, 10));
}

/* ======================================================================
 *  WebView2 のコールバック (C で COM オブジェクトを実装)
 *
 *  どのハンドラもプロセス存続中ずっと使う静的オブジェクトなので、
 *  参照カウントは数えません。QueryInterface は簡略化のため、
 *  どの IID にも自分自身を返しています。
 * ==================================================================== */
#define DEFINE_STATIC_UNKNOWN(NAME, TYPE)                                           \
    static HRESULT STDMETHODCALLTYPE NAME##_QI(TYPE *This, REFIID riid, void **ppv) \
    {                                                                               \
        (void)riid;                                                                 \
        *ppv = This;                                                                \
        return S_OK;                                                                \
    }                                                                               \
    static ULONG STDMETHODCALLTYPE NAME##_AddRef(TYPE *This)                        \
    {                                                                               \
        (void)This;                                                                 \
        return 1;                                                                   \
    }                                                                               \
    static ULONG STDMETHODCALLTYPE NAME##_Release(TYPE *This)                       \
    {                                                                               \
        (void)This;                                                                 \
        return 1;                                                                   \
    }

static void ShowError(const wchar_t *text)
{
    MessageBoxW(g_hwnd, text, L"Prom", MB_OK | MB_ICONERROR);
}

static void ResizeWebView(void)
{
    if (!g_controller)
        return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_controller->lpVtbl->put_Bounds(g_controller, rc);
}

/* ---- JS からのメッセージ ---- */
DEFINE_STATIC_UNKNOWN(Msg, ICoreWebView2WebMessageReceivedEventHandler)

static HRESULT STDMETHODCALLTYPE Msg_Invoke(
    ICoreWebView2WebMessageReceivedEventHandler *This,
    ICoreWebView2 *sender,
    ICoreWebView2WebMessageReceivedEventArgs *args)
{
    (void)This;
    (void)sender;
    LPWSTR msg = NULL;
    if (SUCCEEDED(args->lpVtbl->TryGetWebMessageAsString(args, &msg)) && msg)
    {
        HandleMessage(msg);
        CoTaskMemFree(msg);
    }
    return S_OK;
}

static ICoreWebView2WebMessageReceivedEventHandlerVtbl g_msgVtbl =
    {Msg_QI, Msg_AddRef, Msg_Release, Msg_Invoke};
static ICoreWebView2WebMessageReceivedEventHandler g_msgHandler = {&g_msgVtbl};

/* ---- コントローラ作成完了 ---- */
DEFINE_STATIC_UNKNOWN(Ctrl, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)

static HRESULT STDMETHODCALLTYPE Ctrl_Invoke(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This,
    HRESULT hr, ICoreWebView2Controller *ctrl)
{
    (void)This;
    if (FAILED(hr) || !ctrl)
    {
        ShowError(L"WebView2 の初期化に失敗しました。");
        return hr;
    }
    g_controller = ctrl;
    ctrl->lpVtbl->AddRef(ctrl);
    ctrl->lpVtbl->get_CoreWebView2(ctrl, &g_webview);

    /* WebView の背景を透明にする (半透明ウィンドウの背景を透かすため) */
    BOOL transparentOk = FALSE;
    ICoreWebView2Controller2 *c2 = NULL;
    if (SUCCEEDED(ctrl->lpVtbl->QueryInterface(ctrl, &kIID_Controller2, (void **)&c2)) && c2)
    {
        COREWEBVIEW2_COLOR clear = {0, 0, 0, 0};
        transparentOk = SUCCEEDED(c2->lpVtbl->put_DefaultBackgroundColor(c2, clear));
        c2->lpVtbl->Release(c2);
    }
    g_wvTransparent = transparentOk;
    if (!transparentOk && g_glass)
        DisableGlass(); /* 透明にできないなら不透明のダークで表示 */

    ICoreWebView2Settings *st = NULL;
    if (SUCCEEDED(g_webview->lpVtbl->get_Settings(g_webview, &st)) && st)
    {
        st->lpVtbl->put_AreDefaultContextMenusEnabled(st, FALSE);
        st->lpVtbl->put_IsStatusBarEnabled(st, FALSE);
        st->lpVtbl->put_IsZoomControlEnabled(st, FALSE);
        st->lpVtbl->put_AreDevToolsEnabled(st, TRUE); /* 配布時は FALSE 推奨 (F12 で開発者ツール) */
        st->lpVtbl->Release(st);
    }

    EventRegistrationToken token;
    g_webview->lpVtbl->add_WebMessageReceived(g_webview, &g_msgHandler, &token);
    ResizeWebView();

    /* exe と同じフォルダの ui\index.html を表示 */
    wchar_t dir[MAX_PATH], page[MAX_PATH], url[URL_MAX];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    PathRemoveFileSpecW(dir);
    PathCombineW(page, dir, L"ui\\index.html");
    DWORD n = ARRAYSIZE(url);
    if (SUCCEEDED(UrlCreateFromPathW(page, url, &n, 0)))
        g_webview->lpVtbl->Navigate(g_webview, url);
    return S_OK;
}

static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl g_ctrlVtbl =
    {Ctrl_QI, Ctrl_AddRef, Ctrl_Release, Ctrl_Invoke};
static ICoreWebView2CreateCoreWebView2ControllerCompletedHandler g_ctrlHandler = {&g_ctrlVtbl};

/* ---- 環境作成完了 ---- */
DEFINE_STATIC_UNKNOWN(Env, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)

static HRESULT STDMETHODCALLTYPE Env_Invoke(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This,
    HRESULT hr, ICoreWebView2Environment *env)
{
    (void)This;
    if (FAILED(hr) || !env)
    {
        ShowError(L"WebView2 環境を作成できませんでした。\n"
                  L"WebView2 Runtime がインストールされているか確認してください。");
        return hr;
    }
    return env->lpVtbl->CreateCoreWebView2Controller(env, g_hwnd, &g_ctrlHandler);
}

static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl g_envVtbl =
    {Env_QI, Env_AddRef, Env_Release, Env_Invoke};
static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler g_envHandler = {&g_envVtbl};

/* ======================================================================
 *  ウィンドウ
 * ==================================================================== */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_SIZE:
        if (g_controller)
            g_controller->lpVtbl->put_IsVisible(g_controller, wp != SIZE_MINIMIZED);
        ResizeWebView();
        return 0;
    case WM_NCACTIVATE:
        /* 常にアクティブ状態として描かせる (非アクティブで半透明が不透明に戻るのを防ぐ試み) */
        return DefWindowProcW(hwnd, msg, TRUE, lp);
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = 960;
        mm->ptMinTrackSize.y = 600;
        return 0;
    }
    case WM_DESTROY:
        if (g_controller)
        {
            g_controller->lpVtbl->Close(g_controller);
            g_controller->lpVtbl->Release(g_controller);
            g_controller = NULL;
        }
        if (g_webview)
        {
            g_webview->lpVtbl->Release(g_webview);
            g_webview = NULL;
        }
        if (g_pdhQuery)
            PdhCloseQuery(g_pdhQuery);
        if (g_gpuQuery)
            PdhCloseQuery(g_gpuQuery);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    (void)hPrev;
    (void)cmd;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    g_noGlass = (wcsstr(GetCommandLineW(), L"/noglass") != NULL);

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_numCpu = si.dwNumberOfProcessors ? (int)si.dwNumberOfProcessors : 1;
    DiskInit();
    GpuInit();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    if (!wc.hIcon)
        wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);         /* prom.ico が埋め込まれていないとき */
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); /* 黒 = ガラス領域では透明扱い */
    wc.lpszClassName = L"PromWindow";
    RegisterClassW(&wc);

    UINT dpi = GetDpiForSystem();
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Prom",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             MulDiv(1240, dpi, 96), MulDiv(820, dpi, 96),
                             NULL, NULL, hInst, NULL);
    /* タイトルバー(小)とタスクバー/Alt+Tab(大)のアイコンを、DPI に合ったサイズで設定 */
    {
        UINT d = GetDpiForWindow(g_hwnd);
        HICON hBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                       GetSystemMetricsForDpi(SM_CXICON, d), GetSystemMetricsForDpi(SM_CYICON, d), LR_DEFAULTCOLOR);
        HICON hSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                         GetSystemMetricsForDpi(SM_CXSMICON, d), GetSystemMetricsForDpi(SM_CYSMICON, d), LR_DEFAULTCOLOR);
        if (hBig)
            SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)hBig);
        if (hSmall)
            SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);
    }
    EnableGlass();
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    /* ui\index.html の存在確認 */
    wchar_t dir[MAX_PATH], page[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    PathRemoveFileSpecW(dir);
    PathCombineW(page, dir, L"ui\\index.html");
    if (!PathFileExistsW(page))
    {
        ShowError(L"ui\\index.html が見つかりません。\nexe と同じフォルダに ui フォルダを置いてください。");
        return 1;
    }

    /* WebView2 のユーザーデータは %LOCALAPPDATA% 配下に置く (exe の場所が書き込み不可でも動くように) */
    wchar_t userData[MAX_PATH];
    PCWSTR userDataPtr = NULL;
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", userData, MAX_PATH);
    if (n > 0 && n < MAX_PATH - 20)
    {
        StringCchCatW(userData, MAX_PATH, L"\\PromWebView2");
        userDataPtr = userData;
    }

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(NULL, userDataPtr, NULL, &g_envHandler);
    if (FAILED(hr))
    {
        ShowError(L"WebView2 Runtime が見つかりません。\n"
                  L"https://developer.microsoft.com/microsoft-edge/webview2/ からインストールしてください。");
        return 1;
    }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return (int)m.wParam;
}