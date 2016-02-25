/* session.c  -- Session functions
 *
 * Copyright 2012-2016 AOL Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this Software except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <arpa/inet.h>
#include "moloch.h"

/******************************************************************************/
extern MolochConfig_t        config;
extern uint32_t              pluginsCbs;
extern time_t                lastPacketSecs;

/******************************************************************************/

static int                   tagsField;
static int                   protocolField;

static MolochSessionHead_t   closingQ[MOLOCH_MAX_PACKET_THREADS];
MolochSessionHead_t          tcpWriteQ[MOLOCH_MAX_PACKET_THREADS];

typedef HASHP_VAR(h_, MolochSessionHash_t, MolochSessionHead_t);

static MolochSessionHead_t   sessionsQ[MOLOCH_MAX_PACKET_THREADS][SESSION_MAX];
static MolochSessionHash_t   sessions[MOLOCH_MAX_PACKET_THREADS][SESSION_MAX];

typedef struct molochsescmd {
    struct molochsescmd *cmd_next, *cmd_prev;

    MolochSession_t *session;
    MolochSesCmd     cmd;
    gpointer         uw1;
    gpointer         uw2;
    MolochCmd_func   func;
} MolochSesCmd_t;

typedef struct {
    struct molochsescmd *cmd_next, *cmd_prev;
    int                  cmd_count;
    MOLOCH_LOCK_EXTERN(lock);
} MolochSesCmdHead_t;

static MolochSesCmdHead_t   sessionCmds[MOLOCH_MAX_PACKET_THREADS];

/******************************************************************************/
void moloch_session_id (char *buf, uint32_t addr1, uint16_t port1, uint32_t addr2, uint16_t port2)
{
    if (addr1 < addr2) {
        *(uint32_t *)buf = addr1;
        *(uint16_t *)(buf+4) = port1;
        *(uint32_t *)(buf+6) = addr2;
        *(uint16_t *)(buf+10) = port2;
    } else if (addr1 > addr2) {
        *(uint32_t *)buf = addr2;
        *(uint16_t *)(buf+4) = port2;
        *(uint32_t *)(buf+6) = addr1;
        *(uint16_t *)(buf+10) = port1;
    } else if (ntohs(port1) < ntohs(port2)) {
        *(uint32_t *)buf = addr1;
        *(uint16_t *)(buf+4) = port1;
        *(uint32_t *)(buf+6) = addr2;
        *(uint16_t *)(buf+10) = port2;
    } else {
        *(uint32_t *)buf = addr2;
        *(uint16_t *)(buf+4) = port2;
        *(uint32_t *)(buf+6) = addr1;
        *(uint16_t *)(buf+10) = port1;
    }
}
/******************************************************************************/
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#define int_ntoa(x)     inet_ntoa(*((struct in_addr *)(int*)&x))

char *moloch_session_id_string (int protocol, uint32_t addr1, int port1, uint32_t addr2, int port2)
{
    static char buf[1000];
    int         len;

    if (addr1 < addr2) {
        len = snprintf(buf, sizeof(buf), "%d;%s:%i,", protocol, int_ntoa(addr1), port1);
        snprintf(buf+len, sizeof(buf) - len, "%s:%i", int_ntoa(addr2), port2);
    } else if (addr1 > addr2) {
        len = snprintf(buf, sizeof(buf), "%d;%s:%i,", protocol, int_ntoa(addr2), port2);
        snprintf(buf+len, sizeof(buf) - len, "%s:%i", int_ntoa(addr1), port1);
    } else if (port1 < port2) {
        len = snprintf(buf, sizeof(buf), "%d;%s:%i,", protocol, int_ntoa(addr1), port1);
        snprintf(buf+len, sizeof(buf) - len, "%s:%i", int_ntoa(addr2), port2);
    } else {
        len = snprintf(buf, sizeof(buf), "%d;%s:%i,", protocol, int_ntoa(addr2), port2);
        snprintf(buf+len, sizeof(buf) - len, "%s:%i", int_ntoa(addr1), port1);
    }

    return buf;
}
/******************************************************************************/
/* Must match moloch_session_cmp and moloch_session_id
 * a1 0-3
 * p1 4-5
 * a2 6-9
 * p2 10-11
 */
uint32_t moloch_session_hash(const void *key)
{
    unsigned char *p = (unsigned char *)key;
    //return ((p[2] << 16 | p[3] << 8 | p[4]) * 59) ^ (p[8] << 16 | p[9] << 8 |  p[10]);
    return (((p[1]<<24) ^ (p[2]<<18) ^ (p[3]<<12) ^ (p[4]<<6) ^ p[5]) * 13) ^ (p[8]<<24|p[9]<<16 | p[10]<<8 | p[11]);
}

/******************************************************************************/
int moloch_session_cmp(const void *keyv, const void *elementv)
{
    MolochSession_t *session = (MolochSession_t *)elementv;

    return (*(uint64_t *)keyv     == session->sessionIda && 
            *(uint32_t *)(keyv+8) == session->sessionIdb);
}
/******************************************************************************/
void moloch_session_add_cmd(MolochSession_t *session, MolochSesCmd icmd, gpointer uw1, gpointer uw2, MolochCmd_func func)
{
    MolochSesCmd_t *cmd = MOLOCH_TYPE_ALLOC(MolochSesCmd_t);
    cmd->cmd = icmd;
    cmd->session = session;
    cmd->uw1 = uw1;
    cmd->uw2 = uw2;
    cmd->func = func;
    MOLOCH_LOCK(sessionCmds[session->thread].lock);
    DLL_PUSH_TAIL(cmd_, &sessionCmds[session->thread], cmd);
    MOLOCH_UNLOCK(sessionCmds[session->thread].lock);
    moloch_packet_thread_wake(session->thread);
}
/******************************************************************************/
void moloch_session_get_tag_cb(void *sessionV, int tagType, const char *tagName, uint32_t tag, gboolean async)
{
    MolochSession_t *session = sessionV;

    if (tag == 0) {
        LOG("ERROR - Not adding tag %s type %d couldn't get tag num", tagName, tagType);
        moloch_session_decr_outstanding(session);
    } else if (async) {
        moloch_session_add_cmd(session, MOLOCH_SES_CMD_ADD_TAG, (gpointer)(long)tagType, (gpointer)(long)tag, NULL);
    } else {
        moloch_field_int_add(tagType, session, tag);
        moloch_session_decr_outstanding(session);
    }

}
/******************************************************************************/
gboolean moloch_session_has_tag(MolochSession_t *session, const char *tagName)
{
    uint32_t tagValue;

    if (!session->fields[tagsField])
        return FALSE;

    if ((tagValue = moloch_db_peek_tag(tagName)) == 0)
        return FALSE;

    MolochInt_t          *hint;
    HASH_FIND_INT(i_, *(session->fields[tagsField]->ihash), tagValue, hint);
    return hint != 0;
}
/******************************************************************************/
void moloch_session_add_protocol(MolochSession_t *session, const char *protocol)
{
    moloch_field_string_add(protocolField, session, protocol, -1, TRUE);
}
/******************************************************************************/
gboolean moloch_session_has_protocol(MolochSession_t *session, const char *protocol)
{
    if (!session->fields[protocolField])
        return FALSE;

    MolochString_t          *hstring;
    HASH_FIND(s_, *(session->fields[protocolField]->shash), protocol, hstring);
    return hstring != 0;
}
/******************************************************************************/
void moloch_session_add_tag(MolochSession_t *session, const char *tag) {
    moloch_session_incr_outstanding(session);
    moloch_db_get_tag(session, tagsField, tag, moloch_session_get_tag_cb);

    if (session->stopSaving == 0 && HASH_COUNT(s_, config.dontSaveTags)) {
        MolochString_t *tstring;

        HASH_FIND(s_, config.dontSaveTags, tag, tstring);
        if (tstring) {
            session->stopSaving = (int)(long)tstring->uw;
        }
    }
}

/******************************************************************************/
void moloch_session_add_tag_type(MolochSession_t *session, int tagtype, const char *tag) {
    moloch_session_incr_outstanding(session);
    moloch_db_get_tag(session, tagtype, tag, moloch_session_get_tag_cb);

    if (session->stopSaving == 0 && HASH_COUNT(s_, config.dontSaveTags)) {
        MolochString_t *tstring;

        HASH_FIND(s_, config.dontSaveTags, tag, tstring);
        if (tstring) {
            session->stopSaving = (long)tstring->uw;
        }
    }
}
/******************************************************************************/
// session should already be locked
void moloch_session_mark_for_close (MolochSession_t *session, int ses)
{
    session->closingQ = 1;
    session->saveTime = session->lastPacket.tv_sec + 5;
    DLL_REMOVE(q_, &sessionsQ[session->thread][ses], session);
    DLL_PUSH_TAIL(q_, &closingQ[session->thread], session);

    if (session->tcp_next) {
        DLL_REMOVE(tcp_, &tcpWriteQ[session->thread], session);
    }
}
/******************************************************************************/
void moloch_session_free (MolochSession_t *session)
{
    if (session->tcp_next) {
        DLL_REMOVE(tcp_, &tcpWriteQ[session->thread], session);
    }

    g_array_free(session->filePosArray, TRUE);
    g_array_free(session->fileLenArray, TRUE);
    g_array_free(session->fileNumArray, TRUE);

    if (session->rootId)
        g_free(session->rootId);

    if (session->parserInfo) {
        int i;
        for (i = 0; i < session->parserNum; i++) {
            if (session->parserInfo[i].parserFreeFunc)
                session->parserInfo[i].parserFreeFunc(session, session->parserInfo[i].uw);
        }
        free(session->parserInfo);
    }

    if (session->pluginData)
        MOLOCH_SIZE_FREE(pluginData, session->pluginData);
    moloch_field_free(session);

    moloch_packet_tcp_free(session);

    MOLOCH_TYPE_FREE(MolochSession_t, session);
}
/******************************************************************************/
LOCAL void moloch_session_save(MolochSession_t *session)
{
    if (session->h_next) {
        HASH_REMOVE(h_, sessions[session->thread][session->ses], session);
        session->h_next = 0;
    }

    if (session->closingQ)
        DLL_REMOVE(q_, &closingQ[session->thread], session);
    else
        DLL_REMOVE(q_, &sessionsQ[session->thread][session->ses], session);
    session->q_next = 0;

    moloch_packet_tcp_free(session);

    if (session->parserInfo) {
        int i;
        for (i = 0; i < session->parserNum; i++) {
            if (session->parserInfo[i].parserSaveFunc)
                session->parserInfo[i].parserSaveFunc(session, session->parserInfo[i].uw, TRUE);
        }
    }

    if (pluginsCbs & MOLOCH_PLUGIN_PRE_SAVE)
        moloch_plugins_cb_pre_save(session, TRUE);

    if (session->tcp_next) {
        DLL_REMOVE(tcp_, &tcpWriteQ[session->thread], session);
    }

    if (session->outstandingQueries > 0) {
        session->needSave = 1;
        return;
    }

    moloch_db_save_session(session, TRUE);
    moloch_session_free(session);
}
/******************************************************************************/
// session should already be locked
void moloch_session_mid_save(MolochSession_t *session, uint32_t tv_sec)
{
    if (session->parserInfo) {
        int i;
        for (i = 0; i < session->parserNum; i++) {
            if (session->parserInfo[i].parserSaveFunc)
                session->parserInfo[i].parserSaveFunc(session, session->parserInfo[i].uw, FALSE);
        }
    }

    if (pluginsCbs & MOLOCH_PLUGIN_PRE_SAVE)
        moloch_plugins_cb_pre_save(session, FALSE);

#ifdef FIXLATER
    /* If we are parsing pcap its ok to pause and make sure all tags are loaded */
    while (session->outstandingQueries > 0 && config.pcapReadOffline) {
        g_main_context_iteration (g_main_context_default(), TRUE);
    }
#endif

    if (!session->rootId) {
        session->rootId = "ROOT";
    }

    moloch_db_save_session(session, FALSE);
    g_array_set_size(session->filePosArray, 0);
    g_array_set_size(session->fileLenArray, 0);
    g_array_set_size(session->fileNumArray, 0);
    session->lastFileNum = 0;

    if (session->tcp_next) {
        DLL_MOVE_TAIL(tcp_, &tcpWriteQ[session->thread], session);
    }

    session->saveTime = tv_sec + config.tcpSaveTimeout;
    session->bytes[0] = 0;
    session->bytes[1] = 0;
    session->databytes[0] = 0;
    session->databytes[1] = 0;
    session->packets[0] = 0;
    session->packets[1] = 0;
}
/******************************************************************************/
// session should already be locked
gboolean moloch_session_decr_outstanding(MolochSession_t *session)
{
    session->outstandingQueries--;
    if (session->needSave && session->outstandingQueries == 0) {
        session->needSave = 0; /* Stop endless loop if plugins add tags */
        moloch_db_save_session(session, TRUE);
        moloch_session_free(session);
        return FALSE;
    }

    return TRUE;
}
/******************************************************************************/
int moloch_session_close_outstanding()
{
    int count = 0;
    int t;
    for (t = 0; t < config.packetThreads; t++) {
        count += DLL_COUNT(q_, &closingQ[t]);
    }
    return count;
}
/******************************************************************************/
int moloch_session_cmd_outstanding()
{
    int count = 0;
    int t;
    for (t = 0; t < config.packetThreads; t++) {
        count += DLL_COUNT(cmd_, &sessionCmds[t]);
    }
    return count;
}
/******************************************************************************/
MolochSession_t *moloch_session_find(int ses, char *sessionId)
{
    MolochSession_t *session;

    uint32_t hash = moloch_session_hash(sessionId);
    int      thread = hash % config.packetThreads;

    HASH_FIND_HASH(h_, sessions[thread][ses], hash, sessionId, session);
    return session;
}
/******************************************************************************/
// Should only be used by packet, lots of side effects
MolochSession_t *moloch_session_find_or_create(int ses, char *sessionId, int *isNew)
{
    MolochSession_t *session;

    uint32_t hash = moloch_session_hash(sessionId);
    int      thread = hash % config.packetThreads;

    HASH_FIND_HASH(h_, sessions[thread][ses], hash, sessionId, session);

    if (session) {
        if (!session->closingQ) {
            DLL_MOVE_TAIL(q_, &sessionsQ[thread][ses], session);
        }
        *isNew = 0;
        return session;
    }
    *isNew = 1;

    session = MOLOCH_TYPE_ALLOC0(MolochSession_t);
    session->ses = ses;

    memcpy(&session->sessionIda, sessionId, 8);
    memcpy(&session->sessionIdb, sessionId+8, 4);

    HASH_ADD_HASH(h_, sessions[thread][ses], hash, sessionId, session);
    DLL_PUSH_TAIL(q_, &sessionsQ[thread][ses], session);

    session->filePosArray = g_array_sized_new(FALSE, FALSE, sizeof(uint64_t), 100);
    session->fileLenArray = g_array_sized_new(FALSE, FALSE, sizeof(uint16_t), 100);
    session->fileNumArray = g_array_new(FALSE, FALSE, 4);
    session->fields = MOLOCH_SIZE_ALLOC0(fields, sizeof(MolochField_t *)*config.maxField);
    session->maxFields = config.maxField;
    session->thread = thread;
    DLL_INIT(td_, &session->tcpData);
    if (config.numPlugins > 0)
        session->pluginData = MOLOCH_SIZE_ALLOC0(pluginData, sizeof(void *)*config.numPlugins);

    return session;
}
/******************************************************************************/
uint32_t moloch_session_monitoring()
{
    uint32_t count = 0;
    int      i;

    for (i = 0; i < config.packetThreads; i++) {
        count += HASH_COUNT(h_, sessions[i][SESSION_TCP]) + HASH_COUNT(h_, sessions[i][SESSION_UDP]) + HASH_COUNT(h_, sessions[i][SESSION_ICMP]);
    }
    return count;
}
/******************************************************************************/
void moloch_session_process_commands(int thread)
{
    // Commands
    MolochSesCmd_t *cmd = 0;
    while (1) {
        MOLOCH_LOCK(sessionCmds[thread].lock);
        DLL_POP_HEAD(cmd_, &sessionCmds[thread], cmd);
        MOLOCH_UNLOCK(sessionCmds[thread].lock);
        if (!cmd)
            break;

        switch (cmd->cmd) {
        case MOLOCH_SES_CMD_ADD_TAG:
            moloch_field_int_add((long)cmd->uw1, cmd->session, (long)cmd->uw2);
            moloch_session_decr_outstanding(cmd->session);
            break;
        case MOLOCH_SES_CMD_FUNC:
            cmd->func(cmd->session, cmd->uw1, cmd->uw2);
            break;
        default:
            LOG ("Unknown cmd %d", cmd->cmd);
        }
        MOLOCH_TYPE_FREE(MolochSesCmd_t, cmd);
    }

    // Closing Q
    MolochSession_t *session;
    while (1) {
        session = DLL_PEEK_HEAD(q_, &closingQ[thread]);

        if (session && session->saveTime < (uint64_t)lastPacketSecs) {
            moloch_session_save(session);
        } else {
            break;
        }
    }

#ifdef FIXLATER    
    // Sessions Idle Long Time
    int ses;
    for (ses = 0; ses < SESSION_MAX; ses++) {
        while (1) {
            session = DLL_PEEK_HEAD(q_, &sessionsQ[thread][ses]);

            if (session && (DLL_COUNT(q_, &sessionsQ[thread][ses]) > (int)config.maxStreams ||
                            ((uint64_t)session->lastPacket.tv_sec + config.timeouts[ses] < (uint64_t)lastPacketSecs))) {

                moloch_session_save(session);
            } else {
                break;
            }
        }
    }

    // TCP Sessions Open Long Time
    while (1) {
        session = DLL_PEEK_HEAD(tcp_, &tcpWriteQ[thread]);

        if (session && (uint64_t)session->saveTime < (uint64_t)lastPacketSecs) {
            moloch_session_mid_save(session, lastPacketSecs);
        } else {
            break;
        }
    }
#endif

}

/******************************************************************************/
int moloch_session_watch_count(int ses)
{
    int count = 0;
    int t;

    for (t = 0; t < config.packetThreads; t++) {
        count += DLL_COUNT(q_, &sessionsQ[t][ses]);
    }
    return count;
}

/******************************************************************************/
int moloch_session_idle_seconds(int ses)
{
    int idle = 0;
    int tmp;
    int t;

    for (t = 0; t < config.packetThreads; t++) {
        MolochSession_t *session = DLL_PEEK_HEAD(q_, &sessionsQ[t][ses]);
        if (!session)
            continue;

        tmp = lastPacketSecs - (session->lastPacket.tv_sec + config.timeouts[ses]);
        if (tmp > idle)
            idle = tmp;
    }
    return idle;
}

/******************************************************************************/
void moloch_session_init()
{
    uint32_t primes[] = {10007, 49999, 99991, 199799, 400009, 500009, 732209, 1092757, 1299827, 1500007, 1987411, 2999999};

    int p;
    for (p = 0; p < 12; p++) {
        if (primes[p] >= config.maxStreams/2)
            break;
    }
    if (p == 12) p = 11;

    tagsField = moloch_field_by_db("ta");

    protocolField = moloch_field_define("general", "termfield",
        "protocols", "Protocols", "prot-term",
        "Protocols set for session",
        MOLOCH_FIELD_TYPE_STR_HASH,  MOLOCH_FIELD_FLAG_COUNT | MOLOCH_FIELD_FLAG_LINKED_SESSIONS,
        NULL);

    if (config.debug)
        LOG("session hash size %d", primes[p]);

    int t;
    for (t = 0; t < config.packetThreads; t++) {
        HASHP_INIT(h_, sessions[t][SESSION_UDP], primes[p], moloch_session_hash, moloch_session_cmp);
        HASHP_INIT(h_, sessions[t][SESSION_TCP], primes[p], moloch_session_hash, moloch_session_cmp);
        HASHP_INIT(h_, sessions[t][SESSION_ICMP], primes[p], moloch_session_hash, moloch_session_cmp);
        DLL_INIT(q_, &sessionsQ[t][SESSION_UDP]);
        DLL_INIT(q_, &sessionsQ[t][SESSION_TCP]);
        DLL_INIT(q_, &sessionsQ[t][SESSION_ICMP]);
        DLL_INIT(tcp_, &tcpWriteQ[t]);
        DLL_INIT(q_, &closingQ[t]);
        DLL_INIT(cmd_, &sessionCmds[t]);
        MOLOCH_LOCK_INIT(sessionCmds[t].lock);
    }

    moloch_add_can_quit(moloch_session_cmd_outstanding);
    moloch_add_can_quit(moloch_session_close_outstanding);
}
/******************************************************************************/
/* Only called on main thread. Wait for all packet threads to be empty and then 
 * start the save process on sessions.
 */
void moloch_session_flush()
{
    moloch_packet_flush();

    MolochSession_t *session;
    int thread;
    int i;
    for (thread = 0; thread < config.packetThreads; thread++) {
        for (i = 0; i < SESSION_MAX; i++) {
            HASH_FORALL_POP_HEAD(h_, sessions[thread][i], session,
                session->h_next = 0;
                moloch_session_save(session);
            );
        }
    }
}
/******************************************************************************/
void moloch_session_exit()
{
    int counts[SESSION_MAX] = {0, 0, 0};

    int t;

    for (t = 0; t < config.packetThreads; t++) {
        counts[SESSION_TCP] += sessionsQ[t][SESSION_TCP].q_count;
        counts[SESSION_UDP] += sessionsQ[t][SESSION_UDP].q_count;
        counts[SESSION_ICMP] += sessionsQ[t][SESSION_ICMP].q_count;
    }

    config.exiting = 1;
    LOG("sessions: %d tcp: %d udp: %d icmp: %d",
            moloch_session_monitoring(),
            counts[SESSION_TCP],
            counts[SESSION_UDP],
            counts[SESSION_ICMP]);

    moloch_session_flush();

    if (!config.dryRun && config.copyPcap) {
        moloch_writer_exit();
    }
}