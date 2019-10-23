/**
* @file src/sdplay.c
* @author rigensen
* @brief  ts list in sd card playback over tutk data channel
*         sdp : sd play
* @date 二 10/ 8 12:16:19 2019
*/

/*
 * 1.删除策略 √
 *      1.1 检查sd卡是否满了
 * 2.tutk传输 √
 *      2.1 引入中间层
 * 3.片段信息保存 √
 * 4.片段信息发送 √
 * 5.测试计划
 *      5.1 ts的存储
 * 6.一些之前gos的信令，比如sd卡格式化,获取设备状态等
 * 7.有些地方操作文件没有加锁 √
 * 8.回放支持多通道
 * 9.tutk信令部分
 * */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/stat.h>
#include <assert.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <inttypes.h>
#include "transfer.h"
#include "md5.h"
#include "transfer.h"
#include "dbg.h"
#include "sdplay.h"
#include "public.h"

#define SDPLAY_DBG 0

#define PARAM_CHECK_AND_RETURN( cond )  if ( !(cond) ) { LOGE("check "#cond" error"); return -1; }
#define PTR_CHECK_AND_RETURN( ptr ) if ( !(ptr) ) { LOGE("check pointer "#ptr" error"); return -1; }
#define INDEX_DB "tsindexdb"
#define SEGMENT_DB_FILENAME "segmentdb"
#define INDEX_DB_TMP "tsindexdb.tmp"
#define LENGTH_PER_RECORD 64
#define PKT_HDR_LEN 52
#define SD_SPACE_THREHOLD (1024*1024)// 1M
//#define DELETE_TS_COUNT (1024) // each time sd full,delete 1024 ts
#define DELETE_TS_COUNT (2) // each time sd full,delete 1024 ts
#define TIME_IN_SEC_LEN 10
#define _TIME_STR(t) "0"#t
#define TIME_STR(t) _TIME_STR(t)
#define TIME_FILL_ZERO_LEN TIME_STR(TIME_IN_SEC_LEN)
#define SEGMENT_RECORD_LEN (TIME_IN_SEC_LEN*2+1)
#define MAX_PKT_SIZE (1024*1024) /*  avServSetResendSize()函数最大发送为 1024KB 字节  */
#define TS_MD5_LEN 33
#define MAX_CHANNEL 8

enum {
    FIND_START = 1,
    FIND_END
};

typedef enum {
    AVIOCTRL_EVENT_ALL = 0x00,          // all event type(general APP-->IPCamera)
    AVIOCTRL_EVENT_MOTIONDECT = 0x01,   // motion detect start//==s==
    AVIOCTRL_EVENT_VIDEOLOST = 0x02,    // video lost alarm
    AVIOCTRL_EVENT_IOALARM = 0x03,      // io alarmin start //---s--
    AVIOCTRL_EVENT_MOTIONPASS = 0x04,   // motion detect end  //==e==
    AVIOCTRL_EVENT_VIDEORESUME = 0x05,  // video resume
    AVIOCTRL_EVENT_IOALARMPASS = 0x06,  // IO alarmin end   //---e--
    AVIOCTRL_EVENT_MOVIE = 0x07,
    AVIOCTRL_EVENT_TIME_LAPSE = 0x08,
    AVIOCTRL_EVENT_EMERGENCY = 0x09,
    AVIOCTRL_EVENT_EXPT_REBOOT = 0x10,  // system exception reboot
    AVIOCTRL_EVENT_SDFAULT = 0x11,      // sd record exception
    AVIOCTRL_EVENT_FULLTIME_RECORDING = 0x12,
    AVIOCTRL_EVENT_PIR = 0x13,
    AVIOCTRL_EVENT_RINGBELL = 0x14,
    AVIOCTRL_EVENT_SOUND = 0x15,
} ENUM_EVENTTYPE;


typedef struct {
    const char *ts_path;
    const char *sd_mount_path;
    int ts_delete_count_when_full;
    int channels[MAX_CHANNEL];
    int active_ch_num;
    int running;
    pthread_mutex_t mutex;
    pthread_mutex_t segment_db_mutex;
} sdplay_info_t;

typedef struct {
    unsigned short year;    // The number of year.
    unsigned char month;    // The number of months since January, in the range 1 to 12.
    unsigned char day;      // The day of the month, in the range 1 to 31.
    unsigned char wday;     // The number of days since Sunday, in the range 0 to 6. (Sunday = 0, Monday = 1, ...)
    unsigned char hour;     // The number of hours past midnight, in the range 0 to 23.
    unsigned char minute;   // The number of minutes after the hour, in the range 0 to 59.
    unsigned char second;   // The number of seconds after the minute, in the range 0 to 59.
} time_day_t;

typedef struct {
    unsigned int channel;       // Camera Index
    time_day_t st_starttime;       // Search event from ...
    time_day_t st_endtime;         // ... to (search event)
    unsigned int utc_starttime;  // utc time Search event from ...
    unsigned int utc_endtime;    // utc time ... to (search event)
    unsigned char event;        // event type, refer to ENUM_EVENTTYPE
    unsigned char status;       // 0x00: Recording file exists, Event unreaded
                                // 0x01: Recording file exists, Event readed
                                // 0x02: No Recording file in the event
    unsigned char reserved[2];
} ioctl_list_event_req_t;

typedef struct {
    time_day_t st_starttime;
    time_day_t st_endtime;
    unsigned int utc_starttime;     // UTC Time
    unsigned int utc_endtime;       // UTC Time
    unsigned char event;
    unsigned char status;   // 0x00: Recording file exists, Event unreaded
                            // 0x01: Recording file exists, Event readed
                            // 0x02: No Recording file in the event
    unsigned char reserved[2];
} av_event_t;

typedef struct {
    unsigned int channel;       // Camera Index
    unsigned int total;         // Total event amount in this search session
    unsigned char index;        // package index, 0,1,2...;
                                // because avSendIOCtrl() send package up to 1024 bytes one time, you may want split search results to serveral package to send.
    unsigned char endflag;      // end flag; endFlag = 1 means this package is the last one.
    unsigned char count;        // how much events in this package
    unsigned char reserved[1];
    av_event_t events[1];        // The first memory address of the events in this package
} ioctl_list_event_resp_t;

static sdplay_info_t g_sdplay_info;

static int auth_callback( char *user, char *passwd )
{
    ASSERT( user );
    ASSERT( passwd );

    if ( strncmp(user, "admin", 5) == 0
            && strncmp(passwd, "12345", 5) == 0 ) 
        return 1;

    return 0;
}

static int list_event_handle(int ch,char *data)
{
    int i;
    ioctl_list_event_req_t *req =  (ioctl_list_event_req_t *)data;
    ioctl_list_event_resp_t *eventlist = NULL;

    ASSERT(data);

    LOGI("event:%d", req->event);
    LOGI("channel:%d",req->channel);
    LOGI("status:%d",req->status);
    LOGI("starttime:%d", req->utc_starttime);
    LOGI("endtime:%d", req->utc_endtime);
    LOGI("st.stattime.year = %d", req->st_starttime.year );
    LOGI("st.stattime.month = %d", req->st_starttime.month );
    LOGI("st.stattime.day = %d", req->st_starttime.day );
    LOGI("st.stattime.hour = %d", req->st_starttime.hour );
    LOGI("st.stattime.minute = %d", req->st_starttime.minute );
    LOGI("st.stattime.second = %d", req->st_starttime.second );

    LOGI("st.endtime.year = %d", req->st_endtime.year );
    LOGI("st.endtime.month = %d", req->st_endtime.month );
    LOGI("st.endtime.day = %d", req->st_endtime.day );
    LOGI("st.endtime.hour = %d", req->st_endtime.hour );
    LOGI("st.endtime.minute = %d", req->st_endtime.minute );
    LOGI("st.endtime.second = %d", req->st_endtime.second );
    eventlist = (ioctl_list_event_resp_t *)malloc(sizeof(ioctl_list_event_resp_t)+sizeof(av_event_t)*3);
    memset(eventlist, 0, sizeof(ioctl_list_event_resp_t));
    eventlist->total = 1;
    eventlist->index = 0;
    eventlist->endflag = 1;
    eventlist->count = 3;
    for(i=0;i<eventlist->count;i++) {
        eventlist->events[i].utc_starttime = 1571801203 - 120 - i*60;
        eventlist->events[i].utc_endtime = 1571801203 - i*60;
        eventlist->events[i].event = AVIOCTRL_EVENT_MOTIONDECT;
        eventlist->events[i].status = 0;
    }
    lst_send_ioctl(ch, LST_USER_IPCAM_LISTEVENT_RESP, (char*)eventlist, sizeof(ioctl_list_event_resp_t)+sizeof(av_event_t)*3);

    return 0;
}

static int cmd_handle(int ch, int cmd, char *data)
{
    switch(cmd) {
        case LST_USER_IPCAM_RECORD_PLAYCONTROL:
            LOGI("LST_USER_IPCAM_RECORD_PLAYCONTROL");
            break;
        case LST_USER_IPCAM_LISTEVENT_REQ:
            LOGI("LST_USER_IPCAM_LISTEVENT_REQ");
            if (list_event_handle(ch, data) < 0)
                goto err;
            break;
        case LST_START_PLAY:
            LOGI("LST_START_PLAY");
            break;
        case LST_USER_IPCAM_AUDIOSTART:
            LOGI("LST_USER_IPCAM_AUDIOSTART");
            break;
        case LST_USER_IPCAM_PTZ_COMMAND:
            LOGI("LST_USER_IPCAM_PTZ_COMMAND");
            break;
        default:
            break;
    }

    return 0;
err:
    return -1;
}

static void *ioctl_thread(void *arg)
{
    int sid = (int)arg, ch = 0;
    int ret = 0;

    if ((ch = lst_create_data_channel(sid, auth_callback )) < 0)
        return NULL;
    while( g_sdplay_info.running ) {
        unsigned int cmd = 0;
        char data[1024] = {0};

        ret = lst_recv_ioctl(ch, &cmd, data, sizeof(data), 1000); 
        if ( ret < 0 ) {
            if ( ret == LST_ERR_TIMEOUT )
                continue;
            return NULL;
        }
        if (cmd_handle(ch, cmd, data) < 0 ) {
            LOGE("cmd_handle error");
            return NULL;
        }
    }

    return 0;
}

static void *sdplay_thread(void *arg)
{
    int sid = 0;
    pthread_t tid;

    (void)arg;

    while( !lst_login_success() )
        usleep(200);

    while( g_sdplay_info.running ) {
        LOGI("start to listen");
        if ( (sid = lst_listen(0)) < 0 ) {
            LOGE("lst_listen error");
            continue;
        }
        LOGI("get connection from client, sid:%d", sid);
        pthread_create(&tid, NULL, ioctl_thread, (void*)(uintptr_t)sid );
    }

    return NULL;
}

static inline int get_db_filename( char *out_db_filename, int buflen )
{
    snprintf( out_db_filename, buflen, "%s/%s", g_sdplay_info.ts_path, INDEX_DB );
    return 0;
}

int sdp_init( const char *ts_path,
        const char *sd_mount_path,
        const char *uid,
        const char *dev_name,
        const char *passwd )
{
    pthread_t tid;

    ASSERT( ts_path );
    ASSERT( sd_mount_path );
    ASSERT( uid );
    ASSERT( dev_name );
    ASSERT( passwd );

    g_sdplay_info.running = 1;
    g_sdplay_info.ts_path = strdup(ts_path);
    g_sdplay_info.sd_mount_path = strdup(sd_mount_path);
    g_sdplay_info.ts_delete_count_when_full = DELETE_TS_COUNT;
    pthread_mutex_init( &g_sdplay_info.mutex, NULL );
    pthread_mutex_init( &g_sdplay_info.segment_db_mutex, NULL );
    lst_init( uid, dev_name, passwd );
    pthread_create(&tid, NULL, sdplay_thread, NULL);

    return 0;
}

static int get_sd_free_space(unsigned long long *free_space)
{
    struct statfs statbuf;

    ASSERT( g_sdplay_info.sd_mount_path );
    ASSERT( free_space);

#if SDPLAY_DBG
    *free_space = 1024;
    return 0;
#endif

    if ( statfs(g_sdplay_info.sd_mount_path, &statbuf) < 0 ) {
        LOGE("statfs error");
        return -1;
    }
    *free_space = (statbuf.f_bfree)*(statbuf.f_bsize);

    return 0;
}

static int add_record_to_index_db( const char *ts_name )
{
    FILE *fp = NULL;
    char db_file[256] = { 0 };

    ASSERT( ts_name );
    ASSERT( g_sdplay_info.ts_path );

    LOGI("called");

    snprintf( db_file, sizeof(db_file), "%s/%s", g_sdplay_info.ts_path, INDEX_DB );
    pthread_mutex_lock( &g_sdplay_info.mutex );
    if ( (fp = fopen( db_file, "a" )) == NULL ) {
        LOGE("open file %s error", db_file);
        pthread_mutex_unlock( &g_sdplay_info.mutex );
        return -1;
    }
    fwrite(ts_name, strlen(ts_name), 1, fp);
    fwrite("\n", 1, 1, fp);
    fclose(fp);
    pthread_mutex_unlock( &g_sdplay_info.mutex );

    return 0;
}

static int remove_records_from_index_db()
{
    int i = 0;
    char *linep = NULL;
    size_t len = 0;
    ssize_t read = 0;
    FILE *fp_old = NULL, *fp_new = NULL;
    char db_file[256] = { 0 };
    char new_db_file[256] = { 0 };
    struct stat stat_buf;

    ASSERT( g_sdplay_info.ts_path );
    LOGI("called");

    snprintf( db_file, sizeof(db_file), "%s/%s", g_sdplay_info.ts_path, INDEX_DB );
    snprintf( new_db_file, sizeof(db_file), "%s/%s", g_sdplay_info.ts_path, INDEX_DB_TMP );
    pthread_mutex_lock( &g_sdplay_info.mutex );
    if( stat(db_file, &stat_buf) != 0 ) {
        LOGE("get file %s stat error", db_file );
        pthread_mutex_unlock( &g_sdplay_info.mutex );
        return -1;
    }
    if( stat_buf.st_size == 0 ) {
        LOGE("file %s size is 0", db_file);
        pthread_mutex_unlock( &g_sdplay_info.mutex );
        return -1;
    }
    fp_old = fopen(db_file, "r");
    if ( !fp_old ) {
        LOGE("open file %s error", db_file);
        pthread_mutex_unlock( &g_sdplay_info.mutex );
        return -1;
    }
    fp_new = fopen( new_db_file, "w");
    if ( !fp_new ) {
        fclose( fp_old );
        LOGE("open file %s error", new_db_file);
        return -1;
    }
    while( (read = getline( &linep, &len, fp_old)) != -1 ) {
        if ( ++i != g_sdplay_info.ts_delete_count_when_full ) {
            continue;
        }
        fwrite(linep, len, 1, fp_new);
    }
    fclose(fp_old);
    fclose(fp_new);
    remove(db_file);
    rename(new_db_file, db_file);
    pthread_mutex_unlock( &g_sdplay_info.mutex );
    return 0;
}

static int release_sd_space()
{
    int i = 0;
    FILE *fp = NULL;
    char db_file[256] = { 0 };
    size_t len = 0;
    char *line = NULL;

    CALL( get_db_filename(db_file, sizeof(db_file)) );
    if ( (fp = fopen(db_file, "r")) == NULL ) {
        LOGE("open file %s error", db_file );
        return -1;
    }
    for (i = 0; i < g_sdplay_info.ts_delete_count_when_full; ++i) {
        CALL( getline(&line, &len, fp) );
        if ( !line ) {
            LOGE("check line error");
            fclose( fp );
            return -1;
        }
        if( remove(line) < 0 ) {
            fclose( fp );
            return -1;
        }
    }
    fclose( fp );

    return 0;
}

int sdp_save_ts(const uint8_t *ts_buf, size_t size, int starttime, int endtime)
{
    char filename[512] = { 0 };
    FILE *fp = NULL;
    unsigned long long free_space = 0;
    int ret = 0;

    ASSERT( ts_buf );

    CALL(get_sd_free_space(&free_space));
    if ( free_space < SD_SPACE_THREHOLD ) {
        release_sd_space();
        remove_records_from_index_db();
    }
    snprintf( filename, sizeof(filename), "%d-%d.ts", starttime, endtime );
    if( (fp = fopen(filename, "w")) == NULL ){
        LOGE("open %s error, %s", filename, strerror(errno) );
        return -1;
    }
    ret = fwrite( ts_buf, size, 1, fp);
    LOGI("ret = %d", ret );
    fclose(fp);
    CALL( add_record_to_index_db(filename) );
    CALL(get_sd_free_space(&free_space));

    return 0;
}

static int get_file_size( const char *file )
{
    struct stat stat_buf;

    ASSERT( file );

    if ( stat( file, &stat_buf ) != 0 ) {
        LOGE("get file %s stat error", file );
        return -1;
    }
    return( (int)stat_buf.st_size );
}

static inline int parse_one_record( char *record, int *starttime, int *endtime )
{
    ASSERT( record );
    ASSERT( starttime );
    ASSERT( endtime );

    sscanf( record, "%d-%d", starttime, endtime );

    return 0;
}

static int _find_location_in_db(const char *db_file, int find_type, int64_t time, int record_len, int total_record_count)
{
    int found = 0;
    int cur_pos = 0;
    FILE *fp = NULL;
    size_t len = 0;
    char *line = NULL;
    int record_start_time = 0, record_end_time = 0;
    int pre_record_starttime = 0, pre_record_endtime = 0;

    ASSERT( total_record_count );
    ASSERT( record_len );

    if ( (fp = fopen(db_file, "r")) == NULL ) {
        LOGE("open file %s error", db_file );
        return -1;
    }

    cur_pos = (total_record_count/2)*record_len;
    while( !found ) {
        fseek( fp, cur_pos - record_len, SEEK_SET);
        if ( getline( &line, &len, fp) < 0 ) {
            LOGE("getline error");
            fclose( fp );
            return -1;
        }
        parse_one_record( line, &pre_record_starttime , &pre_record_endtime );
        if ( getline( &line, &len, fp) < 0 ) {
            LOGE("getline error");
            return -1;
        }
        parse_one_record( line, &record_start_time, &record_end_time );
        if ( find_type == FIND_START ) {
            if ( time <= record_start_time && time >= pre_record_endtime ) {
                fclose( fp );
                return cur_pos;
            } else {
                cur_pos = cur_pos/2;
            }
        } else {
            if ( time >= pre_record_endtime && time <= record_start_time ) {
                fclose( fp );
                return cur_pos;
            } else {
                cur_pos = cur_pos + (total_record_count - cur_pos)/2;
            }
        }
    }

    fclose( fp );
    return -1;
}

static int get_record_info_in_db(const char *db_file, int *_record_len, int *total_record_count)
{
    size_t record_len = 0;
    int filesize = 0, record_count = 0;
    char *line = NULL;
    FILE *fp = NULL;

    ASSERT( db_file );
    ASSERT( record_len );
    ASSERT( total_record_count );

    if ( (filesize = get_file_size(db_file)) <= 0 ) 
        return -1;
    if ( filesize == 0 ) {
        LOGE("file %s empty", db_file);
        return ERR_FILE_EMPTY;
    }
    if ( (fp = fopen(db_file, "r")) == NULL ) {
        LOGE("open file %s error", db_file );
        return -1;
    }
    CALL( getline( &line, &record_len, fp) );
    if ( !record_len ) {
        LOGE("get record from %s error", db_file);
        return -1;
    }
    record_count = filesize/record_len;
    *_record_len = record_len;
    *total_record_count = record_count;
    fclose( fp );

    return 0;
}

static int find_location_in_db(char *db_file, int64_t starttime, int64_t endtime, int *start, int *end)
{
    int total_record_count = 0, record_len = 0;
    int start_pos = 0, end_pos = 0;

    ASSERT( start );
    ASSERT( end );

    CALL(get_db_filename(db_file, sizeof(db_file)));
    CALL( get_record_info_in_db(db_file, &record_len, &total_record_count) );
    CALL( start_pos = _find_location_in_db(db_file, FIND_START,  starttime, record_len, total_record_count) );
    CALL( end_pos = _find_location_in_db(db_file, FIND_END, endtime, record_len, total_record_count) );
    *start = start_pos;
    *end = end_pos;

    return 0;
}

static int calc_ts_md5( uint8_t *inbuf, size_t inlen, char *outbuf )
{
    MD5_CONTEXT ctx;
    int i = 0;

    ASSERT( inbuf );
    ASSERT( inlen );
    ASSERT( outbuf );

    md5_init (&ctx);
    md5_write(&ctx, inbuf, inlen );
    md5_final(&ctx);
    for (i = 0; i < 16; ++i) {
       sprintf( outbuf + strlen(outbuf), "%02x", ctx.buf[i] ); 
    }

    return 0;
}

static int gen_packet_header(
        int pkt_idx, 
        int endflg, 
        int starttime, 
        int endtime,
        int len,
        char *md5,
        uint8_t *out_pkt_hdr )
{
#define PKT_HDR_APPEND_INT(val) *(uint32_t *)out_pkt_hdr = htonl(val); out_pkt_hdr += 4;

    ASSERT(out_pkt_hdr);

    PKT_HDR_APPEND_INT( pkt_idx );
    PKT_HDR_APPEND_INT( endflg );
    if (pkt_idx != 0 && endflg) {
        PKT_HDR_APPEND_INT( endtime );
    } else {
        PKT_HDR_APPEND_INT( starttime );
    }
    PKT_HDR_APPEND_INT( len );
    if (endflg) 
        memcpy(out_pkt_hdr, md5, TS_MD5_LEN);

    return 0;
}


static inline FILE *open_index_db(const char *mode)
{
    char db_file[256] = {0};
    FILE *fp = NULL;

    get_db_filename( db_file, sizeof(db_file));
    fp = fopen(db_file, mode);
    if ( !fp ) {
        LOGE("open file %s error", db_file);
    }
    return fp;
}

static int read_file_to_buf(const char *file, uint8_t **outbuf, int *outsize)
{
    FILE *fp = NULL;
    int filesize = 0;

    ASSERT( file );
    ASSERT( outbuf );
    ASSERT( outsize );

    if ( (fp = fopen(file, "r")) == NULL) {
        LOGE("open file %s error", file );
        goto err;
    }
    CALL( filesize = get_file_size(file) );
    *outbuf = (uint8_t *)calloc(1, filesize);
    if ( !(*outbuf) ) {
        LOGE("calloc error, size:%d", filesize);
        goto err;
    }
    if ( fread( *outbuf, filesize, 1, fp) < 0 ) {
        LOGE("fread error");
        goto err;
    }
    *outsize = filesize;
    fclose(fp);

    return 0;
err:
    if (*outbuf)
        free(outbuf);
    if (fp)
        fclose(fp);
    return -1;
}

static int send_pkt(
        int ch,
        int pkt_idx,
        int endflg,
        int starttime,
        int endtime,
        char *md5,
        uint8_t *pkt,
        int pkt_len )
{
    uint8_t pkt_hdr[PKT_HDR_LEN] = {0};
    int i = 0;

    CALL( gen_packet_header(pkt_idx, endflg, starttime, endtime, pkt_len, md5, pkt_hdr) );
    for (i=0; i<g_sdplay_info.active_ch_num; i++) {
        CALL( lst_send_data( ch, pkt_hdr, sizeof(pkt_hdr), pkt, pkt_len));
    }
    return 0;
}

static int send_ts(int ch, const char *ts_file, int starttime, int endtime)
{
    uint8_t *buf_ptr = NULL, *save= NULL;
    int filesize = 0;
    FILE *fp = NULL;
    int pkt_count = 0, i = 0;
    char md5[TS_MD5_LEN] = {0};

    ASSERT( ts_file );

    if(read_file_to_buf(ts_file, &buf_ptr, &filesize) < 0)
        goto err;
    save = buf_ptr;
    if( calc_ts_md5(buf_ptr, filesize, md5) < 0)
        goto err;
    pkt_count = filesize/MAX_PKT_SIZE;
    if (pkt_count == 1) {
        if( send_pkt(ch, i, 1, starttime, endtime, md5, buf_ptr, filesize )<0)
            goto err;
    } else {
        for (i=0; i<pkt_count; i++) {
            if( send_pkt(ch, i, 0, starttime, endtime, md5, buf_ptr, MAX_PKT_SIZE ) < 0 )
                goto err;
            buf_ptr += MAX_PKT_SIZE;
        }
        if( send_pkt(ch, i, 1, starttime, endtime, md5, buf_ptr, filesize-(pkt_count*MAX_PKT_SIZE) ) < 0)
            goto err;
    }

    fclose(fp);
    free(save);
    return 0;
err:
    if (fp)
        fclose(fp);
    if (save)
        free(save);
    return -1;
}

int sdp_send_ts_list(int ch, int starttime, int endtime)
{
    int start_pos = 0, end_pos = 0;
    FILE *db_fp = NULL;
    int count = 0, i = 0;
    char *line = NULL;
    size_t len = 0;
    int ts_starttime = 0, ts_endtime = 0;
    char db_file[256] = {0};

    ASSERT( g_sdplay_info.ts_path );
    ASSERT( endtime );

    pthread_mutex_lock( &g_sdplay_info.mutex );
    db_fp = open_index_db("r");
    if ( !db_fp ) 
        goto err;
    if (get_db_filename(db_file, sizeof(db_file)) < 0)
        goto err;
    if( find_location_in_db(db_file, starttime, endtime, &start_pos, &end_pos) < 0)
        goto err;
    count = end_pos - start_pos;
    ASSERT( count );
    for (i = 0; i < count; ++i) {
        if ( getline( &line, &len, db_fp) < 0 ) {
            LOGE("getline error");
            free(line);
            goto err;
        }
        if ( parse_one_record(line, &ts_starttime, &ts_endtime) < 0 )
            goto err;
        if( send_ts(ch, line, ts_starttime, ts_endtime) < 0 ) 
            goto err;
        free(line);
    }
    fclose( db_fp );
    pthread_mutex_unlock( &g_sdplay_info.mutex );

    return 0;

err:
    if (db_fp)
        fclose(db_fp);
    pthread_mutex_unlock( &g_sdplay_info.mutex );
    return -1;
}

static int get_segment_db_filename( char *out_filename, int buflen )
{
    ASSERT( out_filename );
    ASSERT( g_sdplay_info.ts_path );

    snprintf( out_filename, buflen, "%s/%s", g_sdplay_info.ts_path, SEGMENT_DB_FILENAME );
    return 0;
}

int sdp_save_segment_info(int starttime, int endtime)
{
    FILE *fp = NULL;
    char segment_db_file[256] = { 0 };
    char line[SEGMENT_RECORD_LEN] = {0};

    CALL( get_segment_db_filename( segment_db_file, sizeof(segment_db_file)) );
    pthread_mutex_lock(&g_sdplay_info.segment_db_mutex);
    snprintf(line, sizeof(line), "%" TIME_FILL_ZERO_LEN "d-%" TIME_FILL_ZERO_LEN "d\n", starttime, endtime );
    if ( (fp = fopen( segment_db_file, "a") ) == NULL ) {
        LOGE("open file %s error", segment_db_file);
        pthread_mutex_unlock(&g_sdplay_info.segment_db_mutex);
        return -1;
    }
    fwrite(line, strlen(line), 1, fp);
    fclose(fp);
    pthread_mutex_unlock(&g_sdplay_info.segment_db_mutex);
    return 0;
}

int sdp_send_segment_list(int ch, int in_starttime, int in_endtime)
{
    char segment_db_file[256] = { 0 };
    FILE *fp = NULL;
    int start_pos = 0, end_pos = 0, count = 0;
    int starttime = 0, endtime = 0;
    char *line = NULL;
    size_t len = 0;
    int record_len = 0, i = 0;
    uint8_t time_buf[8] = {0};

    CALL( get_segment_db_filename(segment_db_file, sizeof(segment_db_file)) );
    pthread_mutex_lock(&g_sdplay_info.segment_db_mutex);
    if ( get_record_info_in_db(segment_db_file, &record_len, &count) < 0 ) {
        LOGE("get record info error");
        pthread_mutex_unlock(&g_sdplay_info.segment_db_mutex);
        goto err;
    }
    CALL( find_location_in_db(segment_db_file, in_starttime, in_endtime, &start_pos, &end_pos) );
    if ( ( fp = fopen(segment_db_file, "r") ) == NULL ) {
        LOGE("open file %s error", segment_db_file );
        goto err;
    }
    for (i = 0; i < count; ++i) {
        if ( getline(&line, &len, fp) < 0 ) {
            LOGE("getline error");
            goto err;
        }
        if ( !line ) {
            LOGE("get one line segment error");
            goto err;
        }
        if( parse_one_record(line, &starttime, &endtime ) < 0 ) {
            goto err;
        }
        *(int*)&time_buf = htonl(starttime);
        *(int*)(&time_buf + 4) = htonl(endtime);
        if ( lst_send_data(ch, NULL, 0, time_buf, sizeof(time_buf)) <0 )
            goto err;
    }
    pthread_mutex_unlock(&g_sdplay_info.segment_db_mutex);

    return 0;
err:
    if (fp) 
        fclose( fp );
    pthread_mutex_unlock(&g_sdplay_info.segment_db_mutex);
    return -1;
}

