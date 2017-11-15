#include "targetver.h"
#include <stdlib.h>
#include <windows.h>
#include <Wininet.h>
#include "ras.h"
#include "raserror.h"
#include <tchar.h>
#include <stdio.h>
#include "common.h"
#include <string>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "rasapi32.lib")

void reportWindowsError(const char* action, const char* connName) {
  LPTSTR pErrMsg = NULL;
  DWORD errCode = GetLastError();
  FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER|
      FORMAT_MESSAGE_IGNORE_INSERTS |
      FORMAT_MESSAGE_FROM_HMODULE|
      FORMAT_MESSAGE_FROM_SYSTEM|
      FORMAT_MESSAGE_ARGUMENT_ARRAY,
      GetModuleHandle(_T("wininet.dll")),
      errCode,
      LANG_NEUTRAL,
      pErrMsg,
      0,
      NULL);
  if (NULL != connName) {
    fprintf(stderr, "Error %s for connection '%s': %lu %s\n",
        action, connName, errCode, pErrMsg);
  } else {
    fprintf(stderr, "Error %s: %lu %s\n", action, errCode, pErrMsg);
  }
}

// Stolen from https://github.com/getlantern/winproxy Figure out which Dial-Up
// or VPN connection is active; in a normal LAN connection, this should return
// NULL. NOTE: For some reason this method fails when compiled in Debug mode
// but works every time in Release mode.
// TODO: we may want to find all active connections instead of the first one.
LPTSTR findActiveConnection() {

	DWORD dwCb = 0;
	DWORD dwRet = ERROR_SUCCESS;
	DWORD dwConnections = 0;
	LPRASCONN lpRasConn = NULL;
	RASCONNSTATUS rasconnstatus;
	rasconnstatus.dwSize = sizeof(RASCONNSTATUS);

	// Call RasEnumConnections with lpRasConn = NULL. dwCb is returned with the required buffer size and 
	// a return code of ERROR_BUFFER_TOO_SMALL
	dwRet = RasEnumConnections(lpRasConn, &dwCb, &dwConnections);

	// The following error is expected and is used to determine the required buffer size, as
	// returned in dwCb. See https://msdn.microsoft.com/en-us/library/windows/desktop/aa377284(v=vs.85).aspx
	if (dwRet == ERROR_BUFFER_TOO_SMALL) {
		// Allocate the memory needed for the array of RAS structure(s).
		lpRasConn = (LPRASCONN)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwCb);
		if (lpRasConn == NULL) {
			wprintf(L"HeapAlloc failed!\n");
			return NULL;
		}
		// The first RASCONN structure in the array must contain the RASCONN structure size
		lpRasConn[0].dwSize = sizeof(RASCONN);

		// Call RasEnumConnections to enumerate active connections
		dwRet = RasEnumConnections(lpRasConn, &dwCb, &dwConnections);

		// If successful, print the names of the active connections.
		if (ERROR_SUCCESS == dwRet) {
			DWORD i;
			for (i = 0; i < dwConnections; i++) {
				RasGetConnectStatus(lpRasConn[i].hrasconn, &rasconnstatus);
				if (rasconnstatus.rasconnstate == RASCS_Connected) {
					return lpRasConn[i].szEntryName;
				}
			}
		}
		//Deallocate memory for the connection buffer
		HeapFree(GetProcessHeap(), 0, lpRasConn);
		lpRasConn = NULL;
		return NULL;
	}

	return NULL;
}

int initialize(INTERNET_PER_CONN_OPTION_LIST* options) {
  DWORD dwBufferSize = sizeof(INTERNET_PER_CONN_OPTION_LIST);
  options->dwSize = dwBufferSize;
  // NULL for LAN, connection name otherwise.
  options->pszConnection = findActiveConnection();

  options->dwOptionCount = 3;
  options->dwOptionError = 0;
  options->pOptions = new INTERNET_PER_CONN_OPTION[3];
  if(!options->pOptions) {
    return NO_MEMORY;
  }
  options->pOptions[0].dwOption = INTERNET_PER_CONN_FLAGS;
  options->pOptions[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
  options->pOptions[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
  return RET_NO_ERROR;
}

int query(INTERNET_PER_CONN_OPTION_LIST* options) {
  DWORD dwBufferSize = sizeof(INTERNET_PER_CONN_OPTION_LIST);
  if(!InternetQueryOption(NULL, INTERNET_OPTION_PER_CONNECTION_OPTION, options, &dwBufferSize)) {
    reportWindowsError("querying options", options->pszConnection ? options->pszConnection : "LAN");
    return SYSCALL_FAILED;
  }
  return RET_NO_ERROR;
}

int show()
{
  INTERNET_PER_CONN_OPTION_LIST options;
  int ret = initialize(&options);
  if (ret != RET_NO_ERROR) {
    return ret;
  }
  ret = query(&options);
  if (ret != RET_NO_ERROR) {
    return ret;
  }
  if ((options.pOptions[0].Value.dwValue & PROXY_TYPE_PROXY) > 0) {
    if (options.pOptions[1].Value.pszValue != NULL) {
      printf("%s\n", options.pOptions[1].Value.pszValue);
    }
  }
  return ret;
}

int toggleProxy(bool turnOn, const char *host, const char *port)
{
  INTERNET_PER_CONN_OPTION_LIST options;
  int ret = initialize(&options);
  if (ret != RET_NO_ERROR) {
    return ret;
  }

  std::string ph = std::string(host);
  std::string pp = std::string(port);
  std::string proxyStr = ph + ":" + pp;
 
  LPSTR proxy = _strdup(proxyStr.c_str());

  if (turnOn) {
    options.pOptions[0].Value.dwValue = PROXY_TYPE_DIRECT | PROXY_TYPE_PROXY;
    options.pOptions[1].Value.pszValue = proxy;
  }
  else {
    if (strlen(host) == 0) {
      goto turnOff;
    }
    ret = query(&options);
    if (ret != RET_NO_ERROR) {
      goto cleanup;
    }
    // we turn proxy off only if the option is set and proxy address has the
    // provided prefix.
    if ((options.pOptions[0].Value.dwValue & PROXY_TYPE_PROXY) == 0
        || options.pOptions[1].Value.pszValue == NULL
        || strncmp(proxy, options.pOptions[1].Value.pszValue, strlen(proxy)) != 0) {
      goto cleanup;
    }
    // fall through
turnOff:
    options.pOptions[0].Value.dwValue = PROXY_TYPE_DIRECT;
    options.pOptions[1].Value.pszValue = "";
    options.pOptions[2].Value.pszValue = "";
  }

  DWORD dwBufferSize = sizeof(INTERNET_PER_CONN_OPTION_LIST);
  BOOL result = InternetSetOption(NULL,
      INTERNET_OPTION_PER_CONNECTION_OPTION,
      &options,
      dwBufferSize);
  if (!result) {
    reportWindowsError("setting options", options.pszConnection ? options.pszConnection : "LAN");
    ret = SYSCALL_FAILED;
    goto cleanup;
  }
  result = InternetSetOption(NULL, INTERNET_OPTION_SETTINGS_CHANGED, NULL, 0);
  if (!result) {
    reportWindowsError("propagating changes", NULL);
    ret = SYSCALL_FAILED;
    goto cleanup;
  }
  result = InternetSetOption(NULL, INTERNET_OPTION_REFRESH , NULL, 0);
  if (!result) {
    reportWindowsError("refreshing", NULL);
    ret = SYSCALL_FAILED;
    goto cleanup;
  }

cleanup:
  delete[] options.pOptions;
  delete proxy;
  return ret;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
	switch (message) {
		case WM_ENDSESSION:
			printf("Session ending\n");
			toggleProxy(false, "", "");
			break;
		default:
			return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

// courtesy of https://social.msdn.microsoft.com/Forums/windowsdesktop/en-US/abf09824-4e4c-4f2c-ae1e-5981f06c9c6e/windows-7-console-application-has-no-way-of-trapping-logoffshutdown-event?forum=windowscompatibility
void createInvisibleWindow()
{
  HWND hwnd;
  WNDCLASS wc={0};
  wc.lpfnWndProc=(WNDPROC)WndProc;
  wc.hInstance=GetModuleHandle(NULL);
  wc.hIcon=LoadIcon(GetModuleHandle(NULL), "SysproxyWindow");
  wc.lpszClassName="SysproxyWindow";
  RegisterClass(&wc);

  hwnd=CreateWindowEx(0,"SysproxyWindow","SysproxyWindow",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,(HWND) NULL, (HMENU) NULL, GetModuleHandle(NULL), (LPVOID) NULL);
  if(!hwnd)
    printf("FAILED to create window!!!  %Iu\n",GetLastError());
}

DWORD WINAPI runInvisibleWindowThread(LPVOID lpParam)
{
  MSG msg;
  createInvisibleWindow();
  while (GetMessage(&msg,(HWND) NULL , 0 , 0))
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
  return 0;
}

void setupSystemShutdownHandler()
{
  // Create an invisible window so that we can respond to system shutdown and
  // make sure that we finish setting the system proxy to off.
  DWORD tid;
  HANDLE hInvisiblethread=CreateThread(NULL, 0, runInvisibleWindowThread, NULL, 0, &tid);
  if (hInvisiblethread == NULL)
  {
    printf("FAILED to create thread for invisible window!!!  %Iu\n",GetLastError());
  }
}
