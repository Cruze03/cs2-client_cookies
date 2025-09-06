#ifndef _INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_
#define _INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_

#include <ISmmPlugin.h>
#include <sh_vector.h>
#include "utlvector.h"
#include "ehandle.h"
#include <iserver.h>
#include <entity2/entitysystem.h>
#include "igameevents.h"
#include "vector.h"
#include <deque>
#include <functional>
#include <utlstring.h>
#include <KeyValues.h>
#include "CCSPlayerController.h"
#include "include/mysql_mm.h"
#include "include/sqlite_mm.h"
#include "include/cookies.h"

#include "steam/isteamuser.h"
#include "steam/steam_api_common.h"
#include "steam/steamclientpublic.h"

class cookies final : public ISmmPlugin, public IMetamodListener
{
public:
	bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late);
	bool Unload(char* error, size_t maxlen);
	void AllPluginsLoaded();
	void* OnMetamodQuery(const char* iface, int* ret);

	void OnClientDisconnect( CPlayerSlot slot, ENetworkDisconnectionReason reason, const char *pszName, uint64 xuid, const char *pszNetworkID );
	void OnGameServerSteamAPIActivated();
	void GameFrame(bool simulating, bool bFirstTick, bool bLastTick);
	void OnValidateAuthTicketHook(ValidateAuthTicketResponse_t *pResponse);
	bool Hook_ClientConnect( CPlayerSlot slot, const char *pszName, uint64 xuid, const char *pszNetworkID, bool unk1, CBufferString *pRejectReason );

	STEAM_GAMESERVER_CALLBACK_MANUAL(cookies, OnValidateAuthTicket, ValidateAuthTicketResponse_t, m_CallbackValidateAuthTicketResponse);
private:
	const char* GetAuthor();
	const char* GetName();
	const char* GetDescription();
	const char* GetURL();
	const char* GetLicense();
	const char* GetVersion();
	const char* GetDate();
	const char* GetLogTag();
};

class CookiesApi : public ICookiesApi {
private:
	std::map<int, ClientCookieLoadedCallback> m_ClientCookieLoadedCallbacks;
public:
	void SetCookie(int iSlot, const char* sCookieName, const char* sData);
    const char* GetCookie(int iSlot, const char* sCookieName);
    void HookClientCookieLoaded(SourceMM::PluginId id, ClientCookieLoadedCallback callback) {
		m_ClientCookieLoadedCallbacks[id] = callback;
	}

	void CallClientCookieLoaded(int iSlot) {
		for (auto& callback : m_ClientCookieLoadedCallbacks) {
			if (callback.second) {
				callback.second(iSlot);
			}
		}
	}
};

class Player
{
public:
	Player(int iSlot) : m_iSlot(iSlot) {
		m_bAuthenticated = false;
		m_SteamID = nullptr;
	}
	bool IsAuthenticated() { return m_bAuthenticated; }

	uint64 GetUnauthenticatedSteamId64() { return m_UnauthenticatedSteamID->ConvertToUint64(); }
	const CSteamID* GetUnauthenticatedSteamId() { return m_UnauthenticatedSteamID; }

	uint64 GetSteamId64() { return m_SteamID?m_SteamID->ConvertToUint64():0; }
	const CSteamID* GetSteamId() { return m_SteamID; }

	void SetAuthenticated(bool bAuthenticated) { m_bAuthenticated = bAuthenticated; }

	void SetUnauthenticatedSteamId(const CSteamID* steamID) { m_UnauthenticatedSteamID = steamID; }

	void SetSteamId(const CSteamID* steamID) { m_SteamID = steamID; }
private:
	int m_iSlot;
	bool m_bAuthenticated = false;
	const CSteamID* m_UnauthenticatedSteamID;
	const CSteamID* m_SteamID;
};

Player* m_Players[64];

void OnClientAuthorized(int iSlot, uint64 iSteamID64);
std::string formatCurrentTime();
std::string formatCurrentTime2();
void ErrorLog(const char* msg, ...);

#endif //_INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_
