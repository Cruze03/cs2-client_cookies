#include <stdio.h>
#include "ctimer.h"
#include "cookies.h"
#include "metamod_oslink.h"
#include "schemasystem/schemasystem.h"

#include <iomanip>
#include <sstream>

cookies g_cookies;
PLUGIN_EXPOSE(cookies, g_cookies);
IVEngineServer2* engine = nullptr;

float g_flUniversalTime;
float g_flLastTickedTime;
bool g_bHasTicked;

IMySQLClient *g_pMysqlClient;
ISQLiteClient *g_pSqliteClient;
IMySQLConnection* g_pConnectionMysql;
ISQLConnection* g_pConnectionSQLite;
bool g_bSQLite = false;

CookiesApi* g_pCookiesApi = nullptr;
ICookiesApi* g_pCookiesCore = nullptr;

std::map<std::string, std::string> g_mapCookies[64];

CGameEntitySystem* GameEntitySystem()
{
#ifdef WIN32
	static int offset = 88;
#else
	static int offset = 80;
#endif
	return *reinterpret_cast<CGameEntitySystem**>((uintptr_t)(g_pGameResourceServiceServer)+offset);
}

// Will return null between map end & new map startup, null check if necessary!
CGlobalVars* GetGlobals()
{
	return engine->GetServerGlobals();
}

SH_DECL_HOOK0_void(IServerGameDLL, GameServerSteamAPIActivated, SH_NOATTRIB, 0);
SH_DECL_HOOK3_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool, bool, bool);
SH_DECL_HOOK5_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0, CPlayerSlot, ENetworkDisconnectionReason, const char *, uint64, const char *);
SH_DECL_HOOK4_void(IServerGameClients, ClientPutInServer, SH_NOATTRIB, 0, CPlayerSlot, char const *, int, uint64);
SH_DECL_HOOK6_void(IServerGameClients, OnClientConnected, SH_NOATTRIB, 0, CPlayerSlot, const char*, uint64, const char *, const char *, bool);
SH_DECL_HOOK6(IServerGameClients, ClientConnect, SH_NOATTRIB, 0, bool, CPlayerSlot, const char*, uint64, const char *, bool, CBufferString *);

bool cookies::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameClients, IServerGameClients, SOURCE2GAMECLIENTS_INTERFACE_VERSION);

	SH_ADD_HOOK(IServerGameDLL, GameServerSteamAPIActivated, g_pSource2Server, SH_MEMBER(this, &cookies::OnGameServerSteamAPIActivated), false);
	SH_ADD_HOOK(IServerGameDLL, GameFrame, g_pSource2Server, SH_MEMBER(this, &cookies::GameFrame), true);
	SH_ADD_HOOK(IServerGameClients, ClientDisconnect, g_pSource2GameClients, SH_MEMBER(this, &cookies::OnClientDisconnect), true);
	SH_ADD_HOOK(IServerGameClients, ClientConnect, g_pSource2GameClients, SH_MEMBER(this, &cookies::Hook_ClientConnect), false );

	g_SMAPI->AddListener( this, this );

	g_pCookiesApi = new CookiesApi();
	g_pCookiesCore = g_pCookiesApi;

	new CTimer(1.0f, []()
	{
		for(int i = 0; i < 64; i++)
		{
			if (!m_Players[i] || m_Players[i]->IsAuthenticated())
				continue;

			if(engine->IsClientFullyAuthenticated(CPlayerSlot(i)))
			{
				m_Players[i]->SetAuthenticated(true);
				m_Players[i]->SetSteamId(m_Players[i]->GetUnauthenticatedSteamId());
				OnClientAuthorized(i, m_Players[i]->GetUnauthenticatedSteamId64());
				ConMsg("[%s] %i slot authenticated %lli\n", g_PLAPI->GetLogTag(), i, m_Players[i]->GetUnauthenticatedSteamId64());
			}
		}
		return 1.0f;
	});

	return true;
}

bool cookies::Unload(char *error, size_t maxlen)
{
	ConVar_Unregister();

	SH_REMOVE_HOOK(IServerGameDLL, GameServerSteamAPIActivated, g_pSource2Server, SH_MEMBER(this, &cookies::OnGameServerSteamAPIActivated), false);
	SH_REMOVE_HOOK(IServerGameDLL, GameFrame, g_pSource2Server, SH_MEMBER(this, &cookies::GameFrame), true);
	SH_REMOVE_HOOK(IServerGameClients, ClientDisconnect, g_pSource2GameClients, SH_MEMBER(this, &cookies::OnClientDisconnect), true);
	SH_REMOVE_HOOK(IServerGameClients, ClientConnect, g_pSource2GameClients, SH_MEMBER(this, &cookies::Hook_ClientConnect), false );

	return true;
}

void cookies::OnGameServerSteamAPIActivated()
{
	m_CallbackValidateAuthTicketResponse.Register(this, &cookies::OnValidateAuthTicketHook);
}

bool cookies::Hook_ClientConnect( CPlayerSlot slot, const char *pszName, uint64 xuid, const char *pszNetworkID, bool unk1, CBufferString *pRejectReason )
{
	Player *pPlayer = new Player(slot.Get());
	pPlayer->SetUnauthenticatedSteamId(new CSteamID(xuid));
	m_Players[slot.Get()] = pPlayer;
	RETURN_META_VALUE(MRES_IGNORED, true);
}

void cookies::OnClientDisconnect( CPlayerSlot slot, ENetworkDisconnectionReason reason, const char *pszName, uint64 xuid, const char *pszNetworkID )
{
	int iSlot = slot.Get();

	delete m_Players[iSlot];
	m_Players[iSlot] = nullptr;
}

int g_iDelayAuthFailKick = 30;
void cookies::OnValidateAuthTicketHook(ValidateAuthTicketResponse_t *pResponse)
{
	uint64 iSteamId = pResponse->m_SteamID.ConvertToUint64();
	for (int i = 0; i < 64; i++)
	{
		if (!m_Players[i] || !(m_Players[i]->GetUnauthenticatedSteamId64() == iSteamId))
			continue;
		switch (pResponse->m_eAuthSessionResponse)
		{
			case k_EAuthSessionResponseOK:
			{
				if(m_Players[i]->IsAuthenticated())
					return;
				m_Players[i]->SetAuthenticated(true);
				m_Players[i]->SetSteamId(m_Players[i]->GetUnauthenticatedSteamId());
				OnClientAuthorized(i, m_Players[i]->GetUnauthenticatedSteamId64());
				ConMsg("[%s] %i slot authenticated %lli\n", g_PLAPI->GetLogTag(), i, m_Players[i]->GetUnauthenticatedSteamId64());
				return;
			}

			case k_EAuthSessionResponseAuthTicketInvalid:
			case k_EAuthSessionResponseAuthTicketInvalidAlreadyUsed:
			{
				if (!g_iDelayAuthFailKick)
					return;

				// g_pUtilsApi->PrintToChat(i, g_vecPhrases["AuthTicketInvalid"].c_str());
				[[fallthrough]];
			}

			default:
			{
				if (!g_iDelayAuthFailKick)
					return;

				// g_pUtilsApi->PrintToChat(i, g_vecPhrases["AuthFailed"].c_str(), g_iDelayAuthFailKick);

				new CTimer(g_iDelayAuthFailKick, [i]()
				{
					// engine->DisconnectClient(i, NETWORK_DISCONNECT_KICKED_NOSTEAMLOGIN);
					return -1.f;
				});
			}
		}
	}
}

void cookies::GameFrame(bool simulating, bool bFirstTick, bool bLastTick)
{
	if(!GetGlobals()) return;

	if (simulating && g_bHasTicked)
	{
		g_flUniversalTime += GetGlobals()->curtime - g_flLastTickedTime;
	}

	g_flLastTickedTime = GetGlobals()->curtime;
	g_bHasTicked = true;

	for (int i = g_timers.Count() - 1; i >= 0; i--)
	{
		auto timer = g_timers[i];

		if (timer->m_flLastExecute == -1)
			timer->m_flLastExecute = g_flUniversalTime;

		// Timer execute 
		if (timer->m_flLastExecute + timer->m_flInterval <= g_flUniversalTime)
		{
			if (!timer->Execute())
			{
				delete timer;
				g_timers.Remove(i);
			}
			else
			{
				timer->m_flLastExecute = g_flUniversalTime;
			}
		}
	}
}

void* cookies::OnMetamodQuery(const char* iface, int* ret)
{
	if (!strcmp(iface, COOKIES_INTERFACE))
	{
		*ret = META_IFACE_OK;
		return g_pCookiesCore;
	}

	*ret = META_IFACE_FAILED;
	return nullptr;
}

void LoadConfig() {
	KeyValues* pKVConfig = new KeyValues("Databases");
	if (!pKVConfig->LoadFromFile(g_pFullFileSystem, "addons/configs/databases.cfg")) {
		ErrorLog("[%s] Failed to load databases config 'addons/config/databases.cfg'", g_PLAPI->GetLogTag());
		return;
	}
	g_bSQLite = false;
	const char* szDatabase = "cookies";
	pKVConfig = pKVConfig->FindKey("cookies", false);
	char szPath[256];
	if (pKVConfig) {
		const char* szDriver = pKVConfig->GetString("driver", "mysql");
		szDatabase = pKVConfig->GetString("database", "cookies");
		if(!strcmp(szDriver, "mysql")) {
			MySQLConnectionInfo info;
			info.host = pKVConfig->GetString("host", nullptr);
			info.user = pKVConfig->GetString("user", nullptr);
			info.pass = pKVConfig->GetString("pass", nullptr);
			info.database = pKVConfig->GetString("database", nullptr);
			info.port = pKVConfig->GetInt("port");
			g_pConnectionMysql = g_pMysqlClient->CreateMySQLConnection(info);
			g_pConnectionMysql->Connect([](bool connect) {
				if (!connect) {
					META_CONPRINT("Failed to connect the mysql database\n");
				} else {
					g_pConnectionMysql->Query("CREATE TABLE IF NOT EXISTS `client_cookies` ( \
						`id` INT PRIMARY KEY AUTO_INCREMENT, \
						`steamid` BIGINT NOT NULL, \
						`cookie_name` VARCHAR(255) NOT NULL, \
						`cookie_data` TEXT NOT NULL, \
						UNIQUE KEY `steamid_cookie_name` (`steamid`, `cookie_name`)\
					);", [](ISQLQuery* test){});
				}
			});
			return;
		}
	}
	g_bSQLite = true;
	g_SMAPI->Format(szPath, sizeof(szPath), "addons/data/%s.sqlite3", szDatabase);
	SQLiteConnectionInfo info;
	info.database = szPath;
	g_pConnectionSQLite = g_pSqliteClient->CreateSQLiteConnection(info);
	g_pConnectionSQLite->Connect([](bool connect) {
		if (!connect) {
			META_CONPRINT("Failed to connect the sqlite database\n");
		} else {
			g_pConnectionSQLite->Query("CREATE TABLE IF NOT EXISTS `client_cookies` ( \
				`id` INTEGER PRIMARY KEY AUTOINCREMENT, \
				`steamid` INTEGER NOT NULL, \
				`cookie_name` TEXT NOT NULL, \
				`cookie_data` TEXT NOT NULL, \
				UNIQUE(`steamid`, `cookie_name`)\
			);", [](ISQLQuery* test){});
		}
	});
}

void ProcessQueryResult(int iSlot, ISQLQuery* pQuery)
{
    ISQLResult* pResult = pQuery->GetResultSet();
    if (pResult->GetRowCount() > 0)
    {
        while (pResult->MoreRows())
        {
            if(pResult->FetchRow())
            {
                std::string sCookieName = pResult->GetString(2);
                std::string sCookieData = pResult->GetString(3);
                g_mapCookies[iSlot][sCookieName] = sCookieData;
            }
        }
    }
	g_pCookiesApi->CallClientCookieLoaded(iSlot);
}

void GetClientCookies(int iSlot, uint64 iSteamID64)
{
    char szBuffer[256];
    g_SMAPI->Format(szBuffer, sizeof(szBuffer), "SELECT * FROM client_cookies WHERE steamid = %lld", iSteamID64);
    if(g_bSQLite) {
        g_pConnectionSQLite->Query(szBuffer, [iSlot](ISQLQuery* pQuery) {
            ProcessQueryResult(iSlot, pQuery);
        });
    } else {
        g_pConnectionMysql->Query(szBuffer, [iSlot](ISQLQuery* pQuery) {
            ProcessQueryResult(iSlot, pQuery);
        });
    }
}

void OnClientAuthorized(int iSlot, uint64 iSteamID64)
{
	g_mapCookies[iSlot].clear();
	GetClientCookies(iSlot, iSteamID64);
}

void cookies::AllPluginsLoaded()
{
	char error[64];
	int ret;
	ISQLInterface* pInterface = (ISQLInterface *)g_SMAPI->MetaFactory(SQLMM_INTERFACE, &ret, nullptr);
	if (ret == META_IFACE_FAILED) {
		ErrorLog("[%s] Missing SQL plugin", g_PLAPI->GetLogTag());
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}
	g_pMysqlClient = pInterface->GetMySQLClient();
	g_pSqliteClient = pInterface->GetSQLiteClient();
	LoadConfig();
}

void CookiesApi::SetCookie(int iSlot, const char* sCookieName, const char* sData)
{
	if(g_bSQLite) {
		char szBuffer[256];
		g_SMAPI->Format(szBuffer, sizeof(szBuffer), "INSERT OR REPLACE INTO client_cookies (steamid, cookie_name, cookie_data) VALUES (%lld, '%s', '%s')", engine->GetClientXUID(iSlot), sCookieName, sData);
		g_pConnectionSQLite->Query(szBuffer, [](ISQLQuery* pQuery){});
	} else {
		char szBuffer[256];
		g_SMAPI->Format(szBuffer, sizeof(szBuffer), "INSERT INTO client_cookies (steamid, cookie_name, cookie_data) VALUES (%lld, '%s', '%s') ON DUPLICATE KEY UPDATE cookie_data = '%s'", engine->GetClientXUID(iSlot), sCookieName, sData, sData);
		g_pConnectionMysql->Query(szBuffer, [](ISQLQuery* pQuery){});
	}
	g_mapCookies[iSlot][sCookieName] = sData;
}

const char* CookiesApi::GetCookie(int iSlot, const char* sCookieName)
{
	auto it = g_mapCookies[iSlot].find(sCookieName);
	if(it != g_mapCookies[iSlot].end())
	{
		return it->second.c_str();
	}
	return "";
}

std::string formatCurrentTime() {
    std::time_t currentTime = std::time(nullptr);
    std::tm* localTime = std::localtime(&currentTime);
    std::ostringstream formattedTime;
    formattedTime << std::put_time(localTime, "%m/%d/%Y - %H:%M:%S");
    return formattedTime.str();
}

std::string formatCurrentTime2() {
    std::time_t currentTime = std::time(nullptr);
    std::tm* localTime = std::localtime(&currentTime);
    std::ostringstream formattedTime;
    formattedTime << std::put_time(localTime, "error_%m-%d-%Y");
    return formattedTime.str();
}

void ErrorLog(const char* msg, ...)
{
	va_list args;
	va_start(args, msg);

	char buf[1024];
	V_vsnprintf(buf, sizeof(buf), msg, args);
	va_end(args);

	ConColorMsg(Color(255, 0, 0, 255), "[Error] %s\n", buf);

	char szPath[256], szBuffer[2048];
	g_SMAPI->PathFormat(szPath, sizeof(szPath), "%s/addons/logs/cookies-%s.txt", g_SMAPI->GetBaseDir(), formatCurrentTime2().c_str());
	g_SMAPI->Format(szBuffer, sizeof(szBuffer), "L %s: %s\n", formatCurrentTime().c_str(), buf);

	FILE* pFile = fopen(szPath, "a");
	if (pFile)
	{
		fputs(szBuffer, pFile);
		fclose(pFile);
	}
}

///////////////////////////////////////
const char* cookies::GetLicense()
{
	return "GPL";
}

const char* cookies::GetVersion()
{
	return "CR-1.1";
}

const char* cookies::GetDate()
{
	return __DATE__;
}

const char *cookies::GetLogTag()
{
	return "cookies";
}

const char* cookies::GetAuthor()
{
	return "Pisex, Cruze";
}

const char* cookies::GetDescription()
{
	return "cookies";
}

const char* cookies::GetName()
{
	return "Client Cookies";
}

const char* cookies::GetURL()
{
	return "https://discord.gg/g798xERK5Y | https://github.com/cruze03";
}
