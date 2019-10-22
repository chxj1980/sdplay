/**
* @file transfer.c
* @author rigensen
* @brief  lst - live stream transport
* @date 一 10/14 18:42:54 2019
*/

#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include "IOTCAPIs.h"
#include "AVAPIs.h"
#include "P2PCam/AVFRAMEINFO.h"
#include "P2PCam/AVIOCTRLDEFs.h"
#include "transfer.h"
#include "dbg.h"
#include "public.h"

#define MAX_CLIENT_NUMBER       128
#define MAX_SIZE_IOCTRL_BUF     1024

typedef struct {
    const char *uid;
    const char *dev_name;
    const char *passwd;
    int login_success;
    pthread_t login_tid;
} lst_info_t;

static lst_info_t g_lst_info;

static void login_cb(unsigned int info)
{
    if((info & 0x04)) {
        LOGI("I can be connected via Internet");
    } else if((info & 0x08)) {
        LOGI("I am be banned by IOTC Server because UID multi-login");
    }
}

static void *login_thread(void *arg)
{
    int ret = 0;

    ASSERT( g_lst_info.dev_name );
    ASSERT( g_lst_info.uid );
    ASSERT( g_lst_info.passwd );

    for (;;) {
        ret = IOTC_Device_Login( g_lst_info.uid, g_lst_info.dev_name, g_lst_info.passwd );
        if (ret == IOTC_ER_NoERROR) {
            LOGI("login success");
            g_lst_info.login_success = 1;
            break;
        } else {
            LOGE("ret = %d\n", ret );
            sleep(1);
        }
    }

    return NULL;
}

int lst_login_success()
{
    return g_lst_info.login_success;
}

int lst_init(const char *uid, const char *dev_name, const char *passwd)
{
    int ret = 0;

    ASSERT( uid );
    ASSERT( dev_name );
    ASSERT( passwd );

    g_lst_info.uid = strdup(uid);
    g_lst_info.dev_name = strdup(dev_name);
    g_lst_info.passwd = strdup(passwd);

    IOTC_Set_Max_Session_Number(MAX_CLIENT_NUMBER);
    ret = IOTC_Initialize2(0);
    if(ret != IOTC_ER_NoERROR) {
        LOGE("IOTC_Initialize2(), ret=[%d]\n", ret);
        return -1;
    }
    IOTC_Get_Login_Info_ByCallBackFn( login_cb );
    avInitialize( MAX_CLIENT_NUMBER*3 );
    pthread_create( &g_lst_info.login_tid, NULL, login_thread, NULL );

    return 0;
}

int lst_send_data( int ch, uint8_t *header, int hdr_len, uint8_t *data, int len)
{
    int ret = 0;

    ret = avSendFrameData( ch, (const char *)data, len, (void *)header, hdr_len );
    if ( ret < 0 ) {
        LOGE("avSendFrameData error");
        return -1;
    }

    return 0;
}

int lst_listen( int timeout )
{
    int sid = 0;

    sid = IOTC_Listen( timeout );
    if ( sid < 0 ) {
        LOGE("IOTC_Listen() error, sid = %d", sid );
        sleep(1);
        return -1;
    }

    return sid;
}

int lst_create_data_channel( int sid, auth_cb_t cb )
{
    int resend=-1;
    int index = avServStart3( sid, cb, 0, 0, 0, &resend);
    struct st_SInfo s_info;

    if ( index < 0 ) {
        IOTC_Session_Close(sid);
        return -1;
    }

    if( IOTC_Session_Check(sid, &s_info) == IOTC_ER_NoERROR ) {
        char *mode[3] = {"P2P", "RLY", "LAN"};

        if( isdigit( s_info.RemoteIP[0] ) )
            LOGI("Client is from[IP:%s, Port:%d] Mode[%s] VPG[%d:%d:%d] VER[%X] NAT[%d] AES[%d]",
                   s_info.RemoteIP,
                   s_info.RemotePort,
                   mode[(int)s_info.Mode],
                   s_info.VID,
                   s_info.PID,
                   s_info.GID,
                   s_info.IOTCVersion,
                   s_info.NatType,
                   s_info.isSecure);
    }
    return index;
}

int lst_recv_ioctl( int ch, unsigned int *out_cmd, char *out_data, int max_size, unsigned int timeout )
{
    int ret = 0;
    unsigned int cmd = 0;

    ASSERT(out_data);

    ret = avRecvIOCtrl( ch, &cmd, out_data, max_size, timeout );
    if ( ret < 0 ) {
        if ( ret == AV_ER_TIMEOUT ) {
            return LST_ERR_TIMEOUT;
        } else {
            LOGE("avRecvIOCtrl error, ret = %d", ret);
            return -1;
        }
    }

    LOGI("recv cmd:0x%x", cmd );
    switch( cmd ) {
    case IOTYPE_USER_IPCAM_START:
        *out_cmd = LST_START_PLAY;
        break;
    case IOTYPE_USER_IPCAM_STOP:
        *out_cmd = LST_STOP_PLAY;
        break;
    case IOTYPE_USER_IPCAM_LISTEVENT_REQ:
        *out_cmd = LST_USER_IPCAM_LISTEVENT_REQ;
        break;
    case IOTYPE_USER_IPCAM_RECORD_PLAYCONTROL:
        *out_cmd = LST_USER_IPCAM_RECORD_PLAYCONTROL;
        break;
    case IOTYPE_USER_IPCAM_AUDIOSTART:
        *out_cmd = LST_USER_IPCAM_AUDIOSTART;
        break;
    case IOTYPE_USER_IPCAM_PTZ_COMMAND:
        *out_cmd = LST_USER_IPCAM_PTZ_COMMAND;
        break;
    default:
        break;
    }

    return 0;
}

