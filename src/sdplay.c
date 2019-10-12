/**
* @file src/sdplay.c
* @author rigensen
* @brief  ts list in sd card playback over tutk data channel
*         sdp : sd play
* @date 二 10/ 8 12:16:19 2019
*/

/*
 * sdk ts output to sd card(path need user to specify)
 * ========
 *  - check whether sd card full
 *  - add/delete/query
 *
 * segment list
 * =======
 *  - need to save segment info
 *
 * 删除策略？
 *
 * tutk调研
 *
 * */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/stat.h>
#include <assert.h>

#define RED                  "\e[0;31m"
#define NONE                 "\e[0m"

#define LOGE( args...) do { \
    printf( RED"| %20s | ERROR | %s:%d(%s)# "NONE, __FILE_NAME__, __LINE__, __FUNCTION__); \
    printf(args); \
    printf("\n"); \
} while(0)

#define PARAM_CHECK_AND_RETURN( cond )  if ( !(cond) ) { LOGE("check "#cond" error"); return -1; }
#define PTR_CHECK_AND_RETURN( ptr ) if ( !(ptr) ) { LOGE("check pointer "#ptr" error"); return -1; }
#define CALL_FUNC_AND_RETURN( func ) if( !(func) ) { LOGE("call "#func" error");return -1; }
#define ASSERT assert
#define INDEX_DB "tsindexdb"
#define INDEX_DB_TMP "tsindexdb.tmp"
#define LENGTH_PER_RECORD 64

typedef struct {
    const char *ts_path;
    pthread_mutex_t mutex;
} sdplay_info_t;

static sdplay_info_t g_sdplay_info;

int sdp_init( const char *ts_path )
{
    PARAM_CHECK_AND_RETURN( ts_path );

    g_sdplay_info.ts_path = strdup(ts_path);
    pthread_mutex_init( &g_sdplay_info.mutex, NULL );

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

static int remove_records_from_index_db( int number )
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
        if ( ++i != number ) {
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

int sdp_save_ts(const uint8_t *ts_buf, size_t size, int64_t starttime, int64_t endtime, segment_info_t *seg_info )
{
    char filename[512] = { 0 };
    FILE *fp = NULL;

    PARAM_CHECK_AND_RETURN(ts_buf || seg_info);
    snprintf( filename, sizeof(filename), "%s/%"PRId64"-%"PRId64".ts", starttime, endtime );
    CALL_FUNC_AND_RETURN( fp = fopen(filename, "w") );
    fwrite(fp, ts_buf, size, 1, fp);
    fclose(fp);
    CALL_FUNC_AND_RETURN( add_record_to_index_db(filename) );

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

static int find_location_in_db(int64_t starttime, int64_t endtime, int *start, int *end)
{
    int filesize = 0, found = 0;
    char db_file[256] = { 0 };

    ASSERT( start );
    ASSERT( end );

    snprintf( db_file, sizeof(db_file), "%s/%s", g_sdplay_info.ts_path, INDEX_DB );
    if ( (filesize = get_file_size(db_file)) <= 0 ) 
        return -1;
    while( !found ) {
    }

    return 0;
}

int sdp_send_ts_list(int64_t starttime, int64_t endtime)
{
    DIR *dir = NULL;
    struct dirent *node_ptr = NULL;

    ASSERT( g_sdplay_info.ts_path );
    ASSERT( endtime );
    /*1.find the location of start and end*/
    /*2.every time read one record,and send to remote*/

    return 0;
}

int sdp_send_segment_list(int64_t starttime, int64_t endtime)
{
    return 0;
}

