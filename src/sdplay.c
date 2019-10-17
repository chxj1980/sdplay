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
 * 2.tutk传输
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

#define RED                  "\e[0;31m"
#define NONE                 "\e[0m"

#define LOGE( args...) do { \
    printf( RED"| %20s | ERROR | %s:%d(%s)# "NONE, __FILE_NAME__, __LINE__, __FUNCTION__); \
    printf(args); \
    printf("\n"); \
} while(0)

#define PARAM_CHECK_AND_RETURN( cond )  if ( !(cond) ) { LOGE("check "#cond" error"); return -1; }
#define PTR_CHECK_AND_RETURN( ptr ) if ( !(ptr) ) { LOGE("check pointer "#ptr" error"); return -1; }
#define CALL( func ) if( !(func) ) { LOGE("call "#func" error");return -1; }
#define ASSERT assert
#define INDEX_DB "tsindexdb"
#define SEGMENT_DB_FILENAME "segmentdb"
#define INDEX_DB_TMP "tsindexdb.tmp"
#define LENGTH_PER_RECORD 64
#define PKT_HDR_LEN 10
#define SD_SPACE_THREHOLD (1024*1024)// 1M
#define DELETE_TS_COUNT (1024) // each time sd full,delete 1024 ts
#define TIME_IN_SEC_LEN (10)
#define TIME_STR(t) #t
#define TIME_FILL_ZERO_LEN "0"TIME_STR(TIME_IN_SEC_LEN)

enum {
    FIND_START = 1,
    FIND_END
};

typedef struct {
    const char *ts_path;
    const char *sd_mount_path;
    int ts_delete_count_when_full;
    pthread_mutex_t mutex;
    pthread_mutex_t segment_db_mutex;
} sdplay_info_t;

static sdplay_info_t g_sdplay_info;

int sdp_init( const char *ts_path, const char *sd_mount_path )
{
    ASSERT( ts_path );
    ASSERT( sd_mount_path );

    g_sdplay_info.ts_path = strdup(ts_path);
    g_sdplay_info.sd_mount_path = strdup(sd_mount_path);
    g_sdplay_info.ts_delete_count_when_full = DELETE_TS_COUNT;
    pthread_mutex_init( &g_sdplay_info.mutex, NULL );
    pthread_mutex_init( &g_sdplay_info.segment_db_mutex, NULL );
    lst_init();

    return 0;
}

static int get_sd_free_space(unsigned long long *free_space)
{
    struct statfs statbuf;

    ASSERT( g_sdplay_info.sd_mount_path );
    ASSERT( free_space);

    if ( statfs(g_sdplay_info.sd_mount_path, &statbuf) < 0 ) {
        LOGE("statfs error");
        return -1;
    }
    *frees_space = (statbuf.f_bfree)*(statbuf.f_bsize);

    return 0;
}

static int add_record_to_index_db( const char *ts_name )
{
    FILE *fp = NULL;
    char db_file[256] = { 0 };

    ASSERT( ts_name );
    ASSERT( g_sdplay_info.ts_path );

    snprintf( db_file, sizeof(db_file), "%s/%s", g_sdplay_info.ts_path, INDEX_DB );
    pthread_mutex_lock( &g_sdplay_info.mutex );
    if ( (fp = fopen( db_file, "a" )) == NULL ) {
        LOGE("open file %s error", db_file);
        pthread_mutex_unlock( &g_sdplay_info.mutex );
        return -1;
    }
    fwrite(ts_name, strlen(ts_name), 1, fp);
    fclose(fp);
    pthread_mutex_unlock( &g_sdplay_info.mutex );

    return 0;
}

static int remove_records_from_index_db()
{
    int i = 0;
    char *linep = NULL;
    size_t len = 0;
    ssize_t read = NULL;
    FILE *fp_old = NULL, fp_new = NULL;
    char db_file[256] = { 0 };
    char new_db_file[256] = { 0 };
    struct stat stat_buf;

    ASSERT( number );
    ASSERT( g_sdplay_info.ts_path );

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

static int get_db_filename( char *out_db_filename, int buflen )
{
    ASSERT( out_db_filename );
    ASSERT( g_sdplay_info.ts_path );

    snprintf( out_db_filename, buflen, "%s/%s", g_sdplay_info.ts_path, INDEX_DB );
    return 0;
}

static int release_sd_space()
{
    int i = 0;
    FILE *fp = NULL;
    char db_file[256] = { 0 };
    size_t len = 0;
    char *line = NULL;

    CALL( get_db_filename(db_file) );
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

int sdp_save_ts(const uint8_t *ts_buf, size_t size, int starttime, int endtime, segment_info_t *seg_info )
{
    char filename[512] = { 0 };
    FILE *fp = NULL;
    unsigned long long free_space = 0;

    PARAM_CHECK_AND_RETURN(ts_buf || seg_info);

    if ( free_space < SD_SPACE_THREHOLD ) {
        release_sd_space();
        remove_records_from_index_db();
    }
    snprintf( filename, sizeof(filename), "%d-%d.ts", starttime, endtime );
    CALL( fp = fopen(filename, "w") );
    fwrite(fp, ts_buf, size, 1, fp);
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

static int parse_one_record( char *record, int *starttime, int *endtime )
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
            goto err_close_file; 
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
    int filesize = 0;
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
    *total_record_count = record_cound;
    fclose( fp );

    return 0;
}

static int find_location_in_db(const char *db_file, int64_t starttime, int64_t endtime, int *start, int *end)
{
    int total_record_count = 0, record_len = 0;
    int start_pos = 0, end_pos = 0;

    ASSERT( start );
    ASSERT( end );
    ASSERT( db_file );

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

static int gen_packet_header( uint8_t *pkt_hdr, char *md5, int ts_starttime, int ts_len, int islast )
{
    ASSERT( pkt_hdr );

    *(int *)pkt_hdr = htonl(ts_starttime);  pkt_hdr += 4;
    *(int *)pkt_hdr = htonl(ts_len);        pkt_hdr += 4;
    memcpy(pkt_hdr, md5, strlen(md5));      pkt_hdr += 33;
    *pkt_hdr = islast;

    return 0;
}

int sdp_send_ts_list(int64_t starttime, int64_t endtime)
{
    int start_pos = 0, end_pos = 0;
    FILE *db_fp = NULL, *ts_fp = NULL;
    char db_file[256] = { 0 };
    int count = 0, i = 0, filesize = 0;
    char *line = NULL;
    size_t len = 0;
    uint8_t pkt_hdr[PKT_HDR_LEN] = {0};
    uint8_t *buf_ptr = NULL;
    char md5[33] = { 0 };

    ASSERT( g_sdplay_info.ts_path );
    ASSERT( endtime );

    pthread_mutex_lock( &g_sdplay_info.mutex );
    snprintf( db_file, sizeof(db_file), "%s/%s", g_sdplay_info.ts_path, INDEX_DB );
    if ( (db_fp = fopen(db_file, "r") ) == NULL) {
        LOGE("open file %s error", db_file );
        pthread_mutex_unlock( &g_sdplay_info.mutex );
        return -1;
    }
    if( find_location_in_db(db_file,starttime, endtime, &start_pos, &end_pos) < 0){
        pthread_mutex_unlock( &g_sdplay_info.mutex );
        return -1;
    }
    count = end_pos - start_pos;
    ASSERT( count );
    for (i = 0; i < count; ++i) {
        if ( getline( &line, &len, db_fp) < 0 ) {
            fclose( db_fp );
            pthread_mutex_unlock( &g_sdplay_info.mutex );
            LOGE("getline error");
            return -1;
        }
        CALL( filesize = get_file_size(line) );
        if ( (ts_fp = fopen(line, "r")) == NULL) {
            fclose( db_fp );
            pthread_mutex_unlock( &g_sdplay_info.mutex );
            LOGE("open file %s error", line );
            return -1;
        }
        buf_ptr = (uint8_t *)malloc(filesize);
        memset( buf_ptr, 0, filesize );
        PTR_CHECK_AND_RETURN( buf_ptr );
        if ( fread( buf_ptr, filesize, 1, fp) < 0 ) {
            fclose( db_fp );
            LOGE("fread error");
            pthread_mutex_unlock( &g_sdplay_info.mutex );
            return -1;
        }
        CALL( calc_ts_md5(buf_ptr, filesize, md5 ) );
        CALL( gen_packet_header( pkt_hdr, md5, starttime, filesize, i == count-1 ) );
        CALL( lst_send_data(g_sdplay_info.ch, pkt_hdr, sizeof(pkt_hdr), buf_ptr, filesize));
        fclose(ts_fp);
    }
    fclose( db_fp );
    pthread_mutex_unlock( &g_sdplay_info.mutex );

    return 0;
}

static int get_segment_db_filename( char *out_filename, int buflen )
{
    ASSERT( out_filename );
    ASSERT( g_sdplay_info.ts_path );

    snprintf( out_filename, buflen, "%s/%s", g_sdplay_info.ts_path, SEGMENT_DB_FILENAME );
    return 0;
}

int sdp_save_segment_info(int64_t starttime, int64_t endtime)
{
    FILE *fp = NULL;
    char segment_db_file[256] = { 0 };
    char line[SEGMENT_RECORD_LEN] = {0};

    CALL( get_segment_db_filename( segment_db_file, sizeof(segment_db_file)) );
    pthread_mutex_lock(&g_sdplay_info.segment_db_mutex);
    snprintf(line, sizeof(line), "%"TIME_IN_SEC_LEN PRId64"-%"TIME_IN_SEC_LEN PRId64"\n", starttime, endtime );
    if ( (fp = fopen( segment_db_file, "a") ) == NULL ) {
        LOGE("open file %s error", segment_db_file);
        pthread_mutex_unlock(&g_sdplay_info.segment_db_mutex);
        return -1;
    }
    fwrite(line, strlen(line), 1, fp);
    fclsoe(fp);
    pthread_mutex_unlock(&g_sdplay_info.segment_db_mutex);
    return 0;
}

int sdp_send_segment_list(int64_t starttime, int64_t endtime)
{
    char segment_db_file[256] = { 0 };
    FILE *fp = NULL;
    int start_pos = 0, end_pos = 0, count = 0;
    int starttime = 0, endtime = 0;
    char *line = NULL;
    size_t len = 0;
    int record_len = 0, count = 0;
    char count_str[4] = {0};
    char time_str[4] = {0};

    CALL( get_segment_db_filename(segment_db_file, sizeof(segment_db_file)) );
    pthread_mutex_lock(&g_sdplay_info.segment_db_mutex);
    if ( get_record_info_in_db(segment_db_file, &record_len, &count) < 0 ) {
        LOGE("get record info error");
        pthread_mutex_unlock(&g_sdplay_info.segment_db_mutex);
        goto err;
    }
    *(int*)&count_str = htonl(count);
    if (datachannel_send_data( count_str, sizeof(count_str) )<0) {
        LOGE("send data error");
        goto err;
    }
    CALL( find_location_in_db(segment_db_file, starttime, endtime, &start_pos, &end_pos) );
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
        *(int*)&time_str = htonl(starttime);
        if (datachannel_send_data( time_str, sizeof(time_str)) < 0 ) 
            goto err;
        *(int*)&endtime = htonl(starttime);
        if (datachannel_send_data( time_str, sizeof(time_str)) < 0 ) 
            goto err;
    }
    pthread_mutex_unlock(&g_sdplay_info.segment_db_mutex);

    return 0;
err:
    if (fp) fclose( fp );
    pthread_mutex_unlock(&g_sdplay_info.segment_db_mutex);
    return -1;
}

