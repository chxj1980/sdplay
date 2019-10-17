/**
* @file transfer.c
* @author rigensen
* @brief  lst - live stream transport
* @date 一 10/14 18:42:54 2019
*/

#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include "IOTCAPIs.h"
#include "AVAPIs.h"
#include "P2PCam/AVFRAMEINFO.h"
#include "P2PCam/AVIOCTRLDEFs.h"
#include "transfer.h"

#define MAX_CLIENT_NUMBER       128
#define MAX_SIZE_IOCTRL_BUF     1024

typedef struct {
    const char *uid;
    const char *dev_name;
    const char *passwd;
    int login_success;
    pthread_t login_tid;
} data_channel_info_t;

static data_channel_info_t g_data_ch_info;

static void login_cb(unsigned int info)
{
    if((info & 0x04)) {
        LOGI("I can be connected via Internet\n");
    } else if((info & 0x08)) {
        LOGI("I am be banned by IOTC Server because UID multi-login\n");
    }
}

static void *login_thread(void *arg)
{
    int ret = 0;

    ASSERT( g_data_ch_info.dev_name );
    ASSERT( g_data_ch_info.uid );
    ASSERT( g_data_ch_info.passwd );

    for (;;) {
        ret = IOTC_Device_Login( g_data_ch_info.uid, g_data_ch_info.dev_name, g_data_ch_info.passwd );
        if (ret == IOTC_ER_NoERROR) {
            LOGI("login success");
            g_sdplay_info.login_success = 1;
            break;
        } else {
            LOGE("ret = %d\n", ret );
            sleep(1);
        }
    }
}

int lst_init(const char *uid, const char *dev_name, const char *passwd)
{
    int ret = 0;

    ASSERT( uid );
    ASSERT( dev_name );
    ASSERT( passwd );

    g_data_ch_info.uid = strdup(uid);
    g_data_ch_info.dev_name = strdup(dev_name);
    g_data_ch_info.passwd = strdup(passwd);

    IOTC_Set_Max_Session_Number(MAX_CLIENT_NUMBER);
    ret = IOTC_Initialize2(0);
    if(ret != IOTC_ER_NoERROR) {
        LOGE("IOTC_Initialize2(), ret=[%d]\n", ret);
        return -1;
    }
    IOTC_Get_Login_Info_ByCallBackFn( login_cb );
    avInitialize( MAX_CLIENT_NUMBER*3 );
    pthread_create( &g_sdplay_info.login_tid, NULL, login_thread, NULL );

    return 0;
}

int lst_send_data( int ch, uint8_t *header, int hdr_len, uint8_t *data, int len)
{
    int ret = 0;

    ret = avSendFrameData( ch, data, len, (void *)header, hdr_len );
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
        LOGE("IOTC_Listen() error, sid = %d\n", sid );
        return -1;
    }

    return sid;
}

int lst_create_av_channel( int sid, auth_cb_t cb )
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
            LOGI("Client is from[IP:%s, Port:%d] Mode[%s] VPG[%d:%d:%d] VER[%X] NAT[%d] AES[%d]\n",
                   s_info.RemoteIP,
                   s_info.RemotePort,
                   mode[(int)Sinfo.Mode],
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

    ret = avRecvIOCtrl( nChannel, &cmd, data, nMaxSize, nTimeout );
    if ( ret < 0 ) {
        if ( ret == AV_ER_TIMEOUT ) {
            return LINK_TIMEOUT;
        } else {
            LOGE("get cmd error");
            return -1;
        }
    }

    LOGI("cmd = 0x%x\n", cmd );
    switch( cmd ) {
    case IOTYPE_USER_IPCAM_START:
        *out_cmd = LST_START_PLAY;
        break;
    case IOTYPE_USER_IPCAM_STOP:
        *out_cmd = LST_STOP_PLAY;
        break;
    default:
        break;
    }

    return 0;
}

