#pragma once

#include <functional>
#include <string>
#include <vector>

#ifndef FILE_DEVICE_UNKNOWN
#define FILE_DEVICE_UNKNOWN 0x00000022
#endif

#include <ddk/d4iface.h>
#include <ddk/d4drvif.h>
#include <sudovda/sudovda.h>

namespace VDISPLAY {
	enum class DRIVER_STATUS {
		UNKNOWN              = 1,
		OK                   = 0,
		FAILED               = -1,
		VERSION_INCOMPATIBLE = -2,
		WATCHDOG_FAILED      = -3
	};

	extern HANDLE SUDOVDA_DRIVER_HANDLE;

	/**
	 * @brief One display of a requested arrangement, in the client's canvas coordinates.
	 */
	struct LayoutEntry {
		std::wstring deviceName;
		int x;
		int y;
		int width;
		int height;
		bool primary;
	};

	LONG getDeviceSettings(const wchar_t* deviceName, DEVMODEW& devMode);
	LONG changeDisplaySettings(const wchar_t* deviceName, int width, int height, int refresh_rate);
	LONG changeDisplaySettings2(const wchar_t* deviceName, int width, int height, int refresh_rate, bool bApplyIsolated=false);

	/**
	 * @brief Arrange several virtual displays in one shot, preserving their relative geometry.
	 *
	 * The whole arrangement is placed as a block clear of the displays already on the desktop
	 * and applied in a single SetDisplayConfig call, because the intermediate states of a
	 * display-by-display move overlap and Windows refuses those.
	 *
	 * @param layout The displays to arrange; every one of them must currently be attached.
	 * @param refresh_rate Refresh rate in millihertz, applied to all of them.
	 * @return ERROR_SUCCESS, or a Windows error code.
	 */
	LONG applyVirtualDisplayLayout(const std::vector<LayoutEntry>& layout, int refresh_rate);
	std::wstring getPrimaryDisplay();
	bool setPrimaryDisplay(const wchar_t* primaryDeviceName);
	bool getDisplayHDRByName(const wchar_t* displayName);
	bool setDisplayHDRByName(const wchar_t* displayName, bool enableAdvancedColor);

	void closeVDisplayDevice();
	DRIVER_STATUS openVDisplayDevice();
	bool startPingThread(std::function<void()> failCb);
	bool setRenderAdapterByName(const std::wstring& adapterName);
	std::wstring createVirtualDisplay(
		const char* s_client_uid,
		const char* s_client_name,
		uint32_t width,
		uint32_t height,
		uint32_t fps,
		const GUID& guid
	);
	bool removeVirtualDisplay(const GUID& guid);

	std::vector<std::wstring> matchDisplay(std::wstring sMatch);
}
