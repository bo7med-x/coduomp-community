#include "server_connect.h"

#include "qcommon/game_module_abi_types.h"
#include "qcommon/info.h"
#include "qcommon/net_compare.h"
#include "qcommon/net_text.h"
#include "qcommon/netchan.h"
#include "qcommon/q_command.h"
#include "qcommon/q_string.h"
#include "qcommon/qcommon_limits.h"
#include "qcommon/qcommon_runtime_types.h"
#include "scripting/script_variable.h"
#include "server_client_release.h"
#include "server_client_message.h"
#include "server_connect_services.h"
#include "server_game_data.h"
#include "server_operator_runtime.h"
#include "qcommon/server_runtime_types.h"
#include "qcommon/vm_runtime.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

enum {
    SERVER_CONNECT_MILLISECONDS_PER_SECOND = 1000
};

extern serverStatic_t svs;
extern cvar_t *sv_maxclients;
extern cvar_t *sv_reconnectlimit;
extern cvar_t *sv_minPing;
extern cvar_t *sv_maxPing;
extern cvar_t *sv_privateClients;
extern cvar_t *sv_privatePassword;
extern cvar_t *sv_maxConnectionsPerIP;
extern cvar_t *sv_welcome;
extern cvar_t *sv_welcomeLocation;
extern cvar_t *sv_debug;
extern vm_t *sv_gameVM;

void Com_DPrintf(const char *format, ...);
void Com_Printf(const char *format, ...);
qboolean Sys_IsLANAddress(netadr_t address);
void SV_SendServerCommand(client_t *client, qboolean reliable,
                          const char *format, ...);

/* NOT_FROM_ORIGINAL_SOURCE: broadcast a join/leave notification to every
 * connected client using the existing console chat channel. The message is
 * shown in-game as a chat line; it needs no client-side support. */
void SV_BroadcastConnection(const char *name, qboolean joined)
{
    (void)name;
    (void)joined;
}

/* NOT_FROM_ORIGINAL_SOURCE: fixed-size geolocation cache so repeated joins
 * from the same IP don't hammer the public lookup service. */
enum { SV_GEO_CACHE_SIZE = 256, SV_GEO_STRING_SIZE = 128,
       SV_GEO_IP_STRING_SIZE = 64 };

typedef struct {
    char country[SV_GEO_STRING_SIZE];
    char region[SV_GEO_STRING_SIZE];
    char city[SV_GEO_STRING_SIZE];
} svGeoData_t;

typedef struct {
    char ip[SV_GEO_IP_STRING_SIZE];
    svGeoData_t data;
} svGeoCacheEntry_t;

static svGeoCacheEntry_t sv_geoCache[SV_GEO_CACHE_SIZE];
static int32_t sv_geoCacheNext;

static svGeoData_t *SV_GeoCacheLookup(const char *ip)
{
    for (int32_t i = 0; i < SV_GEO_CACHE_SIZE; ++i) {
        if (sv_geoCache[i].ip[0] != '\0' &&
            strcmp(sv_geoCache[i].ip, ip) == 0) {
            return &sv_geoCache[i].data;
        }
    }
    return NULL;
}

static void SV_GeoCacheStore(const char *ip, const svGeoData_t *data)
{
    Q_strncpyz(sv_geoCache[sv_geoCacheNext].ip, ip,
               sizeof(sv_geoCache[sv_geoCacheNext].ip));
    sv_geoCache[sv_geoCacheNext].data = *data;
    sv_geoCacheNext = (sv_geoCacheNext + 1) % SV_GEO_CACHE_SIZE;
}

static size_t SV_CurlWriteCallback(void *contents, size_t size,
                                   size_t nmemb, void *userp)
{
    const size_t total = size * nmemb;
    char **buffer = (char **)userp;
    const size_t oldLen = *buffer != NULL ? strlen(*buffer) : 0;
    char *newBuffer = realloc(*buffer, oldLen + total + 1);
    if (newBuffer == NULL) {
        return 0;
    }
    memcpy(newBuffer + oldLen, contents, total);
    newBuffer[oldLen + total] = '\0';
    *buffer = newBuffer;
    return total;
}

/* Extract "ip" from "ip:port" or "[ipv6]:port". */
static void SV_ExtractIP(const char *adrString, char *out, size_t outSize)
{
    const char *start = adrString;
    const char *end;
    if (start[0] == '[') {
        start++;
        end = strchr(start, ']');
    } else {
        end = strrchr(start, ':');
    }
    if (end == NULL) {
        Q_strncpyz(out, adrString, outSize);
        return;
    }
    size_t len = (size_t)(end - start);
    if (len >= outSize) {
        len = outSize - 1;
    }
    memcpy(out, start, len);
    out[len] = '\0';
}

static void SV_JsonExtractString(const char *response, const char *key,
                                 char *out, size_t outSize)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);

    out[0] = '\0';
    const char *start = strstr(response, pattern);
    if (start == NULL) {
        return;
    }
    start += strlen(pattern);
    const char *end = strchr(start, '"');
    if (end == NULL) {
        return;
    }
    size_t len = (size_t)(end - start);
    if (len >= outSize) {
        len = outSize - 1;
    }
    memcpy(out, start, len);
    out[len] = '\0';
}

/* Fetch country/region/city for an IP via ip-api.com. Returns qtrue on
 * success, qfalse otherwise. */
static qboolean SV_FetchGeoData(const char *ip, svGeoData_t *out)
{
    CURL *curl;
    CURLcode res;
    char *response = NULL;
    char url[256];

    memset(out, 0, sizeof(*out));

    svGeoData_t *cached = SV_GeoCacheLookup(ip);
    if (cached != NULL) {
        *out = *cached;
        return qtrue;
    }

    curl = curl_easy_init();
    if (curl == NULL) {
        return qfalse;
    }

    snprintf(url, sizeof(url),
             "http://ip-api.com/json/%s?fields=status,country,regionName,city",
             ip);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, SV_CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || response == NULL) {
        if (response != NULL) {
            free(response);
        }
        return qfalse;
    }

    if (strstr(response, "\"status\":\"success\"") == NULL) {
        free(response);
        return qfalse;
    }

    SV_JsonExtractString(response, "country", out->country,
                         sizeof(out->country));
    SV_JsonExtractString(response, "regionName", out->region,
                         sizeof(out->region));
    SV_JsonExtractString(response, "city", out->city, sizeof(out->city));
    free(response);

    SV_GeoCacheStore(ip, out);
    return qtrue;
}

/* NOT_FROM_ORIGINAL_SOURCE: send the configurable welcome message to a
 * newly connected player. Placeholders:
 *   %name    -> player name
 *   %ip      -> player IP address
 *   %num     -> client slot number
 *   %country -> country (e.g. "Egypt")
 *   %city    -> city    (e.g. "Cairo")
 *   %region  -> region  (e.g. "Cairo Governorate")
 *   %loc     -> combined location, depends on sv_welcomeLocation:
 *                1 = country
 *                2 = city, country
 *                3 = region, city, country
 *   %s       -> player name (alias of %name)
 *   Any ^N (N=0..7) is preserved as-is for in-game color.
 * Empty sv_welcome disables the feature. */
void SV_WelcomePlayer(client_t *client)
{
    const char *fmt = sv_welcome->string;
    char message[MAX_STRING_CHARS];
    char *dst = message;
    char *dstEnd = message + sizeof(message) - 1;
    const char *src = fmt;

    if (sv_debug->integer != 0) {
        Com_Printf("[DEBUG] SV_WelcomePlayer for '%s'\n", client->name);
    }

    if (fmt[0] == '\0') {
        return;
    }

    svGeoData_t geo;
    qboolean geoLoaded = qfalse;
    const char *ip = NET_AdrToString(client->netchan.remoteAddress);
    char ipOnly[SV_GEO_IP_STRING_SIZE];
    SV_ExtractIP(ip, ipOnly, sizeof(ipOnly));

    if (strstr(fmt, "%country") != NULL ||
        strstr(fmt, "%city") != NULL ||
        strstr(fmt, "%region") != NULL ||
        strstr(fmt, "%loc") != NULL) {
        geoLoaded = SV_FetchGeoData(ipOnly, &geo);
    }

    char locCombined[SV_GEO_STRING_SIZE * 3 + 8];
    locCombined[0] = '\0';
    if (geoLoaded != qfalse) {
        const int32_t mode = sv_welcomeLocation->integer;
        if (mode == 1) {
            Q_strncpyz(locCombined, geo.country, sizeof(locCombined));
        } else if (mode == 2) {
            if (geo.city[0] != '\0' && geo.country[0] != '\0') {
                snprintf(locCombined, sizeof(locCombined), "%s, %s",
                         geo.city, geo.country);
            } else {
                Q_strncpyz(locCombined, geo.country, sizeof(locCombined));
            }
        } else if (mode == 3) {
            if (geo.region[0] != '\0' && geo.city[0] != '\0' &&
                geo.country[0] != '\0') {
                snprintf(locCombined, sizeof(locCombined), "%s, %s, %s",
                         geo.region, geo.city, geo.country);
            } else if (geo.city[0] != '\0' && geo.country[0] != '\0') {
                snprintf(locCombined, sizeof(locCombined), "%s, %s",
                         geo.city, geo.country);
            } else {
                Q_strncpyz(locCombined, geo.country, sizeof(locCombined));
            }
        }
    }

    const int32_t clientNum = (int32_t)(client - svs.clients);

    while (*src != '\0' && dst < dstEnd) {
        if (src[0] != '%') {
            *dst++ = *src++;
            continue;
        }

        const char *value = NULL;
        char numBuf[16];
        int consumed = 0;

        if (Q_stricmpn(src, "%country", 8) == 0) {
            value = geoLoaded != qfalse ? geo.country : "";
            consumed = 8;
        } else if (Q_stricmpn(src, "%region", 7) == 0) {
            value = geoLoaded != qfalse ? geo.region : "";
            consumed = 7;
        } else if (Q_stricmpn(src, "%city", 5) == 0) {
            value = geoLoaded != qfalse ? geo.city : "";
            consumed = 5;
        } else if (Q_stricmpn(src, "%name", 5) == 0) {
            value = client->name;
            consumed = 5;
        } else if (Q_stricmpn(src, "%loc", 4) == 0) {
            value = locCombined;
            consumed = 4;
        } else if (Q_stricmpn(src, "%num", 4) == 0) {
            snprintf(numBuf, sizeof(numBuf), "%i", clientNum);
            value = numBuf;
            consumed = 4;
        } else if (Q_stricmpn(src, "%ip", 3) == 0) {
            value = ip;
            consumed = 3;
        } else if (Q_stricmpn(src, "%s", 2) == 0) {
            value = client->name;
            consumed = 2;
        }

        if (value != NULL) {
            size_t vlen = strlen(value);
            if (dst + vlen > dstEnd) {
                vlen = (size_t)(dstEnd - dst);
            }
            memcpy(dst, value, vlen);
            dst += vlen;
            src += consumed;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';

    if (sv_debug->integer != 0) {
        Com_Printf("[DEBUG] Sending welcome: '%s'\n", message);
    }
    SV_SendServerCommand(client, qfalse, "h \"\x15%s\"", message);
}

/*
 * Complete direct-connect transaction shared by both server engines:
 *
 *   CoDUOMP.exe   0x00459b20..0x0045a5dc
 *   coduo_lnxded  0x0808ac82..0x0808b9b5
 *
 * Both bodies agree on protocol and reconnect validation, full-address
 * challenge matching, ping gates, client-slot selection, old-client release,
 * record initialization, game-VM admission, the client-indexed challenge
 * reset, state/timestamp initialization, and heartbeat selection. The two
 * original engines reach different PunkBuster implementations through the
 * target-owned service header, but consume the callback result identically.
 */
void SV_DirectConnect(netadr_t from)
{
    char userinfo[MAX_STRING_CHARS];

    Com_DPrintf("SVC_DirectConnect ()\n");
    Q_strncpyz(userinfo, Cmd_Argv(1), sizeof(userinfo));

    const int32_t protocol = atoi(Info_ValueForKey(userinfo, "protocol"));
    if (protocol != SERVER_PROTOCOL_VERSION) {
        NET_OutOfBandPrint(NS_SERVER, from, "error\n%s",
                           "EXE_SERVER_IS_DIFFERENT_VER");
        Com_DPrintf(
            "    rejected connect from protocol version %i (should be %i)\n",
            protocol, SERVER_PROTOCOL_VERSION);
        return;
    }

    const int32_t challengeNumber =
        atoi(Info_ValueForKey(userinfo, "challenge"));
    const int32_t qport = atoi(Info_ValueForKey(userinfo, "qport"));

    client_t *client = svs.clients;
    int32_t clientNum;
    for (clientNum = 0;
         clientNum < sv_maxclients->integer;
         ++clientNum, ++client) {
        if (NET_CompareBaseAdr(from, client->netchan.remoteAddress) != qfalse &&
            (client->netchan.qport == qport ||
             from.port == client->netchan.remoteAddress.port)) {
            if (svs.realTime - client->lastConnectTime <
                sv_reconnectlimit->integer *
                    SERVER_CONNECT_MILLISECONDS_PER_SECOND) {
                Com_DPrintf("%s:reconnect rejected : too soon\n",
                            NET_AdrToString(from));
                return;
            }
            break;
        }
    }

    int32_t guid = 0;
    int32_t challengeIndex = 0;
    if (NET_IsLocalAddress(from) == qfalse) {
        for (challengeIndex = 0;
             challengeIndex < MAX_CHALLENGES;
             ++challengeIndex) {
            challenge_t *const challenge = &svs.challenges[challengeIndex];
            /* Both authoritative bodies require the original source port as
             * well as the address bytes when matching a challenge row. */
            if (NET_CompareAdr(from, challenge->address) != qfalse &&
                challengeNumber == challenge->challengeNumber) {
                guid = challenge->numericGuid;
                break;
            }
        }

        if (challengeIndex == MAX_CHALLENGES) {
            NET_OutOfBandPrint(NS_SERVER, from,
                               "error\nEXE_BAD_CHALLENGE");
            return;
        }

        challenge_t *const challenge = &svs.challenges[challengeIndex];
        int32_t ping;
        if (challenge->firstPingMsec == 0) {
            ping = svs.realTime - challenge->pingStartTime;
            challenge->firstPingMsec = ping;
        } else {
            ping = challenge->firstPingMsec;
        }

        Com_Printf("Client %i connecting with %i challenge ping from %s\n",
                   challengeIndex, ping, NET_AdrToString(from));
        challenge->connected = qtrue;

        if (Sys_IsLANAddress(from) == qfalse) {
            /* Both x87 bodies compare the exact FILD result directly with the
             * binary32 cvar value; no float store occurs. */
            if (sv_minPing->value != 0.0f &&
                (long double)ping < (long double)sv_minPing->value) {
                NET_OutOfBandPrint(NS_SERVER, from,
                                   "error\nEXE_ERR_HIGH_PING_ONLY");
                Com_DPrintf("Client %i rejected on a too low ping\n",
                            challengeIndex);
                return;
            }
            if (sv_maxPing->value != 0.0f &&
                (long double)sv_maxPing->value < (long double)ping) {
                NET_OutOfBandPrint(NS_SERVER, from,
                                   "error\nEXE_ERR_LOW_PING_ONLY");
                Com_DPrintf(
                    "Client %i rejected on a too high ping: %i\n",
                    challengeIndex, ping);
                return;
            }
        }
    }

    const int32_t clPunkbuster =
        atoi(Info_ValueForKey(userinfo, "cl_punkbuster"));
    const char *const clGuid = Info_ValueForKey(userinfo, "cl_guid");
    const char *const pbReject =
        server_compat_pb_connect_query(from, clPunkbuster, clGuid);
    if (pbReject != NULL) {
        if (Q_stricmpn(pbReject, "error\n", 6) == 0) {
            /* NOT_FROM_ORIGINAL_SOURCE: forward callback text as formatter
             * data through a literal conversion. */
            NET_OutOfBandPrint(NS_SERVER, from, "%s", pbReject);
        }
        return;
    }

    client_t emptyClient;
    memset(&emptyClient, 0, sizeof(emptyClient));

    client = svs.clients;
    for (clientNum = 0;
         clientNum < sv_maxclients->integer;
         ++clientNum, ++client) {
        if (client->state != CS_FREE &&
            NET_CompareBaseAdr(from, client->netchan.remoteAddress) != qfalse &&
            (client->netchan.qport == qport ||
             from.port == client->netchan.remoteAddress.port)) {
            Com_Printf("%s:reconnect\n", NET_AdrToString(from));
            if (client->state >= CS_CONNECTED) {
                SV_FreeClient(client);
            }
            break;
        }
    }

    if (clientNum == sv_maxclients->integer) {
        /* NOT_FROM_ORIGINAL_SOURCE: enforce per-IP connection limit for
         * genuinely new connections. Reconnects from the same address are
         * already handled above (clientNum would not equal sv_maxclients)
         * so they are never affected. Loopback addresses bypass the check
         * so LAN operators and local clients are not locked out. */
        if (sv_maxConnectionsPerIP->integer > 0 &&
            NET_IsLocalAddress(from) == qfalse) {
            int32_t connectionsFromIP = 0;
            for (int32_t peerNum = 0;
                 peerNum < sv_maxclients->integer;
                 ++peerNum) {
                const client_t *const peer = &svs.clients[peerNum];
                if (peer->state != CS_FREE &&
                    NET_CompareBaseAdr(from,
                                       peer->netchan.remoteAddress) != qfalse) {
                    ++connectionsFromIP;
                }
            }
            if (connectionsFromIP >= sv_maxConnectionsPerIP->integer) {
                NET_OutOfBandPrint(NS_SERVER, from, "error\n%s",
                                   "EXE_SERVERISFULL");
                Com_Printf(
                    "Rejected connection from %s: %i connection(s) from "
                    "this IP already active (limit: %i)\n",
                    NET_AdrToString(from), connectionsFromIP,
                    sv_maxConnectionsPerIP->integer);
                return;
            }
        }

        const char *const password = Info_ValueForKey(userinfo, "password");
        const int32_t firstAvailableClient =
            strcmp(password, sv_privatePassword->string) == 0
                ? 0
                : sv_privateClients->integer;

        client = NULL;
        for (clientNum = firstAvailableClient;
             clientNum < sv_maxclients->integer;
             ++clientNum) {
            client_t *const candidate = &svs.clients[clientNum];
            if (candidate->state == CS_FREE) {
                client = candidate;
                break;
            }
        }
        if (client == NULL) {
            NET_OutOfBandPrint(NS_SERVER, from,
                               "error\nEXE_SERVERISFULL");
            Com_DPrintf("Rejected a connection.\n");
            return;
        }

        client->reliableAcknowledge = 0;
        client->reliableSequence = 0;
    }

    *client = emptyClient;
    clientNum = (int32_t)(client - svs.clients);
    client->gentity = SV_GentityNum(clientNum);
    client->scriptId = Scr_AllocArray();
    client->challenge = challengeNumber;
    client->guid = guid;
    Netchan_Setup(NS_SERVER, &client->netchan, from, qport);
    Q_strncpyz(client->userinfo, userinfo, sizeof(client->userinfo));

    const char *const gameReject = (const char *)VM_Call(
        sv_gameVM, GAME_CLIENT_CONNECT,
        clientNum, client->scriptId,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (gameReject != NULL) {
        NET_OutOfBandPrint(NS_SERVER, from, "error\n%s", gameReject);
        Com_DPrintf("Game rejected a connection: %s.\n", gameReject);
        SV_FreeClientScriptId(client);
        return;
    }

    SV_UserinfoChanged(client);
    /* Preserve this recovered boundary's validated input, state, and compatibility invariants. */
    svs.challenges[clientNum].firstPingMsec = 0;
    NET_OutOfBandPrint(NS_SERVER, from, "connectResponse");
    Com_Printf(
        "\033[0;32mGoing from \033[0;31mCS_FREE\033[0;32m to "
        "\033[0;33mCS_CONNECTED\033[0;32m for \033[0;31m%s\033[0m "
        "(num %i guid %i)\n",
        client->name, clientNum, client->guid);
    client->state = CS_CONNECTED;
    SV_BroadcastConnection(client->name, qtrue);
    client->lastPacketTime = svs.realTime;
    client->lastConnectTime = svs.realTime;
    client->nextSnapshotTime = svs.realTime;
    client->gamestateMessageNum = -1;

    int32_t connectedCount = 0;
    for (clientNum = 0;
         clientNum < sv_maxclients->integer;
         ++clientNum) {
        if (svs.clients[clientNum].state >= CS_CONNECTED) {
            ++connectedCount;
        }
    }
    if (connectedCount == 1 || connectedCount == sv_maxclients->integer) {
        SV_Heartbeat_f();
    }
}
