/**
* @file src/sdplay.c
* @author rigensen
* @brief  ts list in sd card playback over tutk data channel
*         sdp : sd play
* @date 二 10/ 8 12:16:19 2019
*/
#include <stdio.h>
#include <errno.h>
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
#define TS_INDEX_DB "tsindexdb"
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
#define SEGMENT_RECORD_LEN (TIME_IN_SEC_LEN*2+1+1+1)
#define MAX_PKT_SIZE (1024*1024) /*  avServSetResendSize()函数最大发送为 1024KB 字节  */
#define TS_MD5_LEN 33
#define MAX_CLIENT_NUM 8

enum {
    JUDGE_CURRENT = 1,
    JUDGE_NEXT,
    JUDGE_LEFT,
    JUDGE_RIGHT
};

enum {
    PLAYBACK_STS_PLAY,
    PLAYBACK_STS_PAUSE,
    PLAYBACK_STS_STOP,
};

typedef struct {
    int av_index;
    int playback_ch;
    int playback_sts;
} av_client_t;

typedef struct {
    const char *sd_mount_path;
    const char *user;
    const char *passwd;
    char *ts_dbfile;
    char *segment_dbfile;
    int ts_delete_count_when_full;
    int active_ch_num;
    int running;
    pthread_mutex_t ts_db_mutex;
    pthread_mutex_t segment_db_mutex;
    av_client_t clients[MAX_CLIENT_NUM];
} sdplay_info_t;

typedef struct {
    unsigned int index;        /* package index, 0,1,2...;
                                * avServSetResendSize()函数最大发送为
                                * 1024KB 字节，index 表示拆分发送的索引
                                */
    unsigned int endflag;      /* endFlag=1 时表示当前TS文件最后一个片段 */
    unsigned int utctime;      /* app 定位时间 */
    unsigned int length;       /* file size */
    unsigned char md5_str[33]; /* md5 校验 */
    unsigned char reserved[3];
} tag_frame_header_t;

typedef struct {
    int sid;
    int starttime;
} playback_info_t;

static int get_record_info_in_db(const char *db_file, int *out_record_len, int *total_record_count);
static int read_file_to_buf(const char *file, uint8_t **outbuf, int *outsize);
static int calc_ts_md5( uint8_t *inbuf, size_t inlen, char *outbuf );
static int get_file_size( const char *file );
static int find_start_pos(char *db_file, int starttime);
static int send_ts(int ch, const char *ts_file, int starttime, int endtime);
static inline int parse_one_record(char *record, int *starttime, int *endtime);

static sdplay_info_t g_sdplay_info;

static int auth_callback( char *user, char *passwd )
{
    ASSERT( user );
    ASSERT( passwd );

    if ( strncmp(user, g_sdplay_info.user, strlen(g_sdplay_info.user)) == 0
            && strncmp(passwd, g_sdplay_info.passwd, strlen(g_sdplay_info.passwd)) == 0 ) 
        return 1;

    return 0;
}

static int list_event_handle(int ch,char *data)
{
    SMsgAVIoctrlListEventReq *req = (SMsgAVIoctrlListEventReq *)data;

    ASSERT(data);

    LOGI("event:%d", req->event);
    LOGI("channel:%d",req->channel);
    LOGI("status:%d",req->status);
    LOGI("starttime:%d", req->utcStartTime);
    LOGI("endtime:%d", req->utcEndTime);

    if (sdp_send_segment_list(ch, req->utcStartTime, req->utcEndTime) < 0)
        return -ERRINTERNAL;

    return 0;
}

static void *tslist_playback_thread(void *arg)
{
    playback_info_t *playback_info_ptr = (playback_info_t *)arg;
    int sid = playback_info_ptr->sid;
    int start_pos = 0;
    int av_index = lst_create_data_channel2(sid, g_sdplay_info.user, g_sdplay_info.passwd, g_sdplay_info.clients[sid].playback_ch);
    SMsgAVIoctrlPlayRecordResp res;
    FILE *fp = NULL;
    char *line = NULL;
    size_t len = 0;
    int ts_starttime = 0, ts_endtime = 0;

    if (av_index < 0)
        return NULL;
    pthread_mutex_lock(&g_sdplay_info.ts_db_mutex);
    fp = fopen(g_sdplay_info.ts_dbfile, "r");
    if (!fp) {
        LOGE("open file %s error", g_sdplay_info.ts_dbfile);
        return NULL;
    }
    start_pos = find_start_pos(g_sdplay_info.ts_dbfile, playback_info_ptr->starttime);
    if (start_pos < 0)
        goto err_unlock;
    while(g_sdplay_info.clients[sid].playback_sts == PLAYBACK_STS_PLAY) {
        if (getline(&line, &len, fp) < 0 ) {
            LOGE("getline error");
            goto err_unlock;
        }
        if (parse_one_record(line, &ts_starttime, &ts_endtime) < 0)
            goto err_free;
        if (send_ts(av_index, line, ts_starttime, ts_endtime) < 0)
            goto err_free;
        free(line);
    }

    res.command = AVIOCTRL_RECORD_PLAY_END;
    if (lst_send_ioctl(
                av_index, 
                LST_USER_IPCAM_RECORD_PLAYCONTROL_RESP,
                (const char *)&res,
                sizeof(SMsgAVIoctrlPlayRecordResp)) < 0)
        return NULL;
    LOGI("send AVIOCTRL_RECORD_PLAY_END");

err_free:
    free(line);
err_unlock:
    pthread_mutex_unlock(&g_sdplay_info.ts_db_mutex);
    fclose(fp);
    return NULL;
}

static int playcontrol_handle(int sid, int ch, char *data)
{
    SMsgAVIoctrlPlayRecord *req = (SMsgAVIoctrlPlayRecord *)data;
    SMsgAVIoctrlPlayRecordResp res;
    pthread_t tid;
    playback_info_t *playback_info_ptr;

    LOGI("cmd:%d",req->command);
    LOGI("utctime:%d", req->utcTime);

    res.command = AVIOCTRL_RECORD_PLAY_START;
    if (req->command == AVIOCTRL_RECORD_PLAY_START){
        if (g_sdplay_info.clients[sid].playback_ch < 0) {
            g_sdplay_info.clients[sid].playback_ch = lst_session_get_free_channel(sid);
            res.result = g_sdplay_info.clients[sid].playback_ch;
        } else
            res.result = -1;
        if (res.result >= 0) {
            playback_info_ptr = (playback_info_t *)calloc(1, sizeof(playback_info_t));
            if (!playback_info_ptr)
                return -ERRNOMEM;
            playback_info_ptr->sid = sid;
            playback_info_ptr->starttime = req->utcTime;
            pthread_create(&tid, NULL, tslist_playback_thread, (void *)playback_info_ptr);
        }
        if (lst_send_ioctl(
                    ch,
                    LST_USER_IPCAM_RECORD_PLAYCONTROL_RESP,
                    (const char *)&res,
                    sizeof(SMsgAVIoctrlPlayRecordResp)) < 0)
            return -ERRINTERNAL;
    }

    return 0;
}

static int cmd_handle(int sid, int ch, int cmd, char *data)
{
    switch(cmd) {
        case LST_USER_IPCAM_RECORD_PLAYCONTROL:
            LOGI("LST_USER_IPCAM_RECORD_PLAYCONTROL");
            if (playcontrol_handle(sid, ch, data) < 0)
                goto err;
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
    g_sdplay_info.clients[sid].av_index = ch;
    while( g_sdplay_info.running ) {
        unsigned int cmd = 0;
        char data[1024] = {0};

        ret = lst_recv_ioctl(ch, &cmd, data, sizeof(data), 1000); 
        if ( ret < 0 ) {
            if ( ret == LST_ERR_TIMEOUT )
                continue;
            return NULL;
        }
        if (cmd_handle(sid, ch, cmd, data) < 0 ) {
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

int sdp_init( const char *ts_path,
        const char *sd_mount_path,
        const char *uid,
        const char *dev_name,
        const char *passwd)
{
    pthread_t tid;
    int i = 0;

    ASSERT( ts_path );
    ASSERT( sd_mount_path );
    ASSERT( uid );
    ASSERT( dev_name );
    ASSERT( passwd );

    for (i=0; i<MAX_CLIENT_NUM; i++) {
        g_sdplay_info.clients[i].playback_ch = -1;
    }
    g_sdplay_info.ts_dbfile = (char *)calloc(1, strlen(ts_path)+strlen(TS_INDEX_DB)+2);
    if ( !g_sdplay_info.ts_dbfile)
        return -ERRNOMEM;
    sprintf(g_sdplay_info.ts_dbfile, "%s/%s", ts_path, TS_INDEX_DB);
    g_sdplay_info.segment_dbfile = (char*)calloc(1, strlen(ts_path)+strlen(SEGMENT_DB_FILENAME)+2);
    if (!g_sdplay_info.segment_dbfile)
        return -ERRNOMEM;
    sprintf(g_sdplay_info.segment_dbfile, "%s/%s", ts_path, SEGMENT_DB_FILENAME);
    g_sdplay_info.running = 1;
    g_sdplay_info.sd_mount_path = strdup(sd_mount_path);
    g_sdplay_info.user = strdup(dev_name);
    g_sdplay_info.passwd = strdup(passwd);
    g_sdplay_info.ts_delete_count_when_full = DELETE_TS_COUNT;
    pthread_mutex_init( &g_sdplay_info.ts_db_mutex, NULL );
    pthread_mutex_init( &g_sdplay_info.segment_db_mutex, NULL );
    lst_init( uid, dev_name, passwd, MAX_CLIENT_NUM );
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

    ASSERT( ts_name );
    ASSERT( g_sdplay_info.ts_dbfile );

    LOGI("called");

    pthread_mutex_lock( &g_sdplay_info.ts_db_mutex );
    if ( (fp = fopen(g_sdplay_info.ts_dbfile, "a")) == NULL ) {
        LOGE("open file %s error", g_sdplay_info.ts_dbfile);
        pthread_mutex_unlock( &g_sdplay_info.ts_db_mutex );
        return -1;
    }
    fwrite(ts_name, strlen(ts_name), 1, fp);
    fwrite("\n", 1, 1, fp);
    fclose(fp);
    pthread_mutex_unlock( &g_sdplay_info.ts_db_mutex );

    return 0;
}

static int remove_records_from_index_db()
{
    int i = 0;
    char *linep = NULL;
    size_t len = 0;
    ssize_t read = 0;
    FILE *fp_old = NULL, *fp_new = NULL;
    char new_db_file[256] = { 0 };
    struct stat stat_buf;

    ASSERT( g_sdplay_info.ts_dbfile );
    LOGI("called");

    snprintf( new_db_file, sizeof(new_db_file), "/tmp/%s", INDEX_DB_TMP );
    pthread_mutex_lock( &g_sdplay_info.ts_db_mutex );
    if( stat(g_sdplay_info.ts_dbfile, &stat_buf) != 0 ) {
        LOGE("get file %s stat error", g_sdplay_info.ts_dbfile );
        pthread_mutex_unlock( &g_sdplay_info.ts_db_mutex );
        return -1;
    }
    if( stat_buf.st_size == 0 ) {
        LOGE("file %s size is 0", g_sdplay_info.ts_dbfile);
        pthread_mutex_unlock( &g_sdplay_info.ts_db_mutex );
        return -1;
    }
    fp_old = fopen(g_sdplay_info.ts_dbfile, "r");
    if ( !fp_old ) {
        LOGE("open file %s error", g_sdplay_info.ts_dbfile);
        pthread_mutex_unlock( &g_sdplay_info.ts_db_mutex );
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
    remove(g_sdplay_info.ts_dbfile);
    rename(new_db_file, g_sdplay_info.ts_dbfile);
    pthread_mutex_unlock(&g_sdplay_info.ts_db_mutex);
    return 0;
}

static int release_sd_space()
{
    int i = 0;
    FILE *fp = NULL;
    char db_file[256] = { 0 };
    size_t len = 0;
    char *line = NULL;

    if ( (fp = fopen(g_sdplay_info.ts_dbfile, "r")) == NULL ) {
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

static int get_times( FILE *fp, int pos, int *starttime, int *endtime, int *next_starttime, int *next_endtime)
{
    char *line = NULL;
    size_t len = 0;

    fseek( fp, pos, SEEK_SET);
    if ( getline(&line, &len, fp) < 0 ) {
        LOGE("getline error, %s, line:%s", strerror(errno), line);
        return -ERRINTERNAL;
    }
    parse_one_record(line, starttime, endtime);
    if ( getline( &line, &len, fp) < 0 ) {
        LOGE("getline error");
        return -ERRINTERNAL;
    }
    parse_one_record(line, next_starttime, next_endtime);
    return 0;
}

static int binary_search_pos( const char *db_file, int time, int (*judge_cb)(FILE *fp, int pos, int time))
{
    int low = 0,high,mid;
    int record_len = 0,total = 0, ret = 0;
    FILE *fp = NULL;

    LOGI("db_file:%s", db_file);
    if ( get_record_info_in_db(db_file, &record_len, &total) < 0 ) 
        return -ERRINTERNAL;
    high = total-1;

    fp = fopen(db_file, "r");
    if (!fp) {
        LOGE("open %s error", db_file);
        return -ERRINTERNAL;
    }

    while(low <= high) {
        mid = (low+high)/2;
        ret = judge_cb(fp, mid*record_len, time);
        if (ret < 0) {
            LOGE("judge_cb error");
            goto err_close_file;
        }
        LOGI("ret:%d, mid:%d, low:%d, high:%d", ret, mid, low, high);
        if (ret == JUDGE_CURRENT)
            return mid*record_len;
        else if (ret == JUDGE_NEXT)
            return (mid+1)*record_len;
        else if (ret == JUDGE_LEFT)
            high = mid - 1;
        else if (ret == JUDGE_RIGHT)
            low = mid + 1;
        else {
            LOGE("judge_cb error");
            goto err_close_file;
        }
    }
    if (mid == 0 || mid == total)
        return mid*record_len;

    LOGE("binary search error,mid:%d",mid);
    return -ERRINTERNAL;
err_close_file:
    fclose(fp);
    return -ERRINTERNAL;
}

static int find_start_judge_callback(FILE *fp, int pos, int time)
{
    int starttime, endtime, next_starttime, next_endtime;

    if (get_times(fp, pos, &starttime, &endtime, &next_starttime, &next_endtime) < 0)
        return -ERRINTERNAL;
    if (time == starttime ||
            (time > starttime && time < next_starttime))
        return JUDGE_CURRENT;
    if (time == next_starttime)
        return JUDGE_NEXT;
    if (time > starttime)
        return JUDGE_RIGHT;
    if (time < starttime)
        return JUDGE_LEFT;

    LOGE("unexpect error");
    return -ERRINTERNAL;
}

static int find_end_judge_callback(FILE *fp, int pos, int time)
{
    int starttime, endtime, next_starttime, next_endtime;

    if (get_times(fp, pos, &starttime, &endtime, &next_starttime, &next_endtime) < 0)
        return -ERRINTERNAL;
    LOGI("time:%d endtime:%d next:%d",time, endtime, next_endtime);
    if (time == endtime ||
            (time > endtime && time < next_endtime))
        return JUDGE_CURRENT;
    if (time == next_endtime)
        return JUDGE_NEXT;
    if (time > endtime)
        return JUDGE_RIGHT;
    if (time < endtime)
        return JUDGE_LEFT;

    LOGE("unexpect error");
    return -ERRINTERNAL;
}

static int find_start_pos(char *db_file, int starttime)
{
    return(binary_search_pos(db_file, starttime, find_start_judge_callback));
}

static int find_end_pos(char *db_file, int endtime)
{
    return(binary_search_pos(db_file, endtime, find_end_judge_callback));
}

static int get_record_info_in_db(const char *db_file, int *out_record_len, int *total_record_count)
{
    size_t record_len = 0;
    ssize_t ret = 0;
    int filesize = 0, record_count = 0;
    char *line = NULL;
    FILE *fp = NULL;

    ASSERT( db_file );
    ASSERT( out_record_len );
    ASSERT( total_record_count );

    if ( (filesize = get_file_size(db_file)) <= 0 ) {
        LOGE("get_file_size error");
        return -1;
    }
    if ( filesize == 0 ) {
        LOGE("file %s empty", db_file);
        return ERR_FILE_EMPTY;
    }
    if ( (fp = fopen(db_file, "r")) == NULL ) {
        LOGE("open file %s error", db_file );
        return -1;
    }
    ret = getline( &line, &record_len, fp);
    if (ret < 0) {
        LOGE("get record from %s error", db_file);
        return -1;
    }
    record_count = filesize/(int)ret;
    *out_record_len = (int)ret;
    *total_record_count = record_count;
    fclose( fp );

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

static inline FILE *open_ts_index_db(const char *mode)
{
    FILE *fp = fopen(g_sdplay_info.ts_dbfile, mode);

    if ( !fp ) {
        LOGE("open file %s error", g_sdplay_info.ts_dbfile);
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
    int i = 0;
    tag_frame_header_t hdr;

    hdr.index = pkt_idx;
    hdr.endflag = endflg;
    hdr.length = pkt_len;
    if (pkt_idx > 0 && endflg)
        hdr.utctime = endtime;
    else
        hdr.utctime = starttime;
    if (endflg)
        memcpy(hdr.md5_str, md5, TS_MD5_LEN);

    return(lst_send_data(ch, (uint8_t *)&hdr, sizeof(hdr), pkt, pkt_len));
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
        if(send_pkt(ch, i, 1, starttime, endtime, md5, buf_ptr, filesize)<0)
            goto err;
    } else {
        for (i=0; i<pkt_count; i++) {
            if(send_pkt(ch, i, 0, starttime, endtime, md5, buf_ptr, MAX_PKT_SIZE) < 0 )
                goto err;
            buf_ptr += MAX_PKT_SIZE;
        }
        if(send_pkt(ch, i, 1, starttime, endtime, md5, buf_ptr, filesize-(pkt_count*MAX_PKT_SIZE)) < 0)
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

int sdp_save_segment_info(int starttime, int endtime)
{
    FILE *fp = NULL;
    char line[SEGMENT_RECORD_LEN] = {0};

    if (starttime < 0 || endtime < 0)
        return -ERRINVAL;
    pthread_mutex_lock(&g_sdplay_info.segment_db_mutex);
    snprintf(line, sizeof(line), "%" TIME_FILL_ZERO_LEN "d-%" TIME_FILL_ZERO_LEN "d\n", starttime, endtime );
    if ( (fp = fopen(g_sdplay_info.segment_dbfile, "a") ) == NULL ) {
        LOGE("open file %s error", g_sdplay_info.segment_dbfile);
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
    FILE *fp = NULL;
    int start_pos = 0, end_pos = 0, count = 0;
    int starttime = 0, endtime = 0;
    char *line = NULL;
    size_t len = 0;
    int record_len = 0, i = 0, total = 0, ret = -ERRINTERNAL;
    SMsgAVIoctrlListEventResp *eventlist = NULL;

    pthread_mutex_lock(&g_sdplay_info.segment_db_mutex);
    if ( get_record_info_in_db(g_sdplay_info.segment_dbfile, &record_len, &total) < 0 ) {
        LOGE("get record info error");
        goto err_unlock;
    }
    LOGI("total:%d", total);
    LOGI("in_starttime:%d", in_starttime);
    LOGI("in_endtime:%d", in_endtime);
    start_pos = find_start_pos(g_sdplay_info.segment_dbfile, in_starttime);
    if (start_pos < 0)
        goto err_unlock;
    LOGI("start:%d", start_pos);
    end_pos = find_end_pos(g_sdplay_info.segment_dbfile, in_endtime);
    if (end_pos < 0)
        goto err_unlock;
    LOGI("start:%d end:%d", start_pos, end_pos);
    if ((fp = fopen(g_sdplay_info.segment_dbfile, "r") ) == NULL) {
        LOGE("open file %s error", g_sdplay_info.segment_dbfile );
        goto err_close_file;
    }
    ASSERT(record_len);
    count = (end_pos - start_pos)/record_len + 1;
    LOGI("segment count:%d", count);
    eventlist = (SMsgAVIoctrlListEventResp *)malloc(sizeof(SMsgAVIoctrlListEventResp)+sizeof(SAvEvent)*count);
    if (!eventlist) {
        LOGE("malloc error");
        goto err_close_file;
    }
    memset(eventlist, 0, sizeof(SMsgAVIoctrlListEventResp)+sizeof(SAvEvent)*count);
    eventlist->total = 1;
    eventlist->index = 0;
    eventlist->endflag = 1;
    eventlist->count = count;
    if(fseek(fp, start_pos, SEEK_SET) < 0) {
        LOGE("fseek error");
        goto err_free_buf;
    }
    for (i = 0; i < count; ++i) {
        if ( getline(&line, &len, fp) < 0 ) {
            LOGE("getline error");
            goto err_free_buf;
        }
        if ( !line ) {
            LOGE("get one line segment error");
            goto err_free_buf;
        }
        if( parse_one_record(line, &starttime, &endtime) < 0 )
            goto err_free_buf;
        eventlist->stEvent[i].utcStartTime = starttime;
        eventlist->stEvent[i].utcEndTime = endtime;
        eventlist->stEvent[i].event = AVIOCTRL_EVENT_MOTIONDECT;
        eventlist->stEvent[i].status = 0;
    }
    if (lst_send_ioctl(
                ch,
                LST_USER_IPCAM_LISTEVENT_RESP,
                (char*)eventlist,
                sizeof(SMsgAVIoctrlListEventResp)+sizeof(SAvEvent)*count) < 0)
        goto err_free_buf;
    ret = 0;
err_free_buf:
    free(eventlist);
err_close_file:
    fclose(fp);
err_unlock:
    pthread_mutex_unlock(&g_sdplay_info.segment_db_mutex);
    return ret;
}

