/**
* @file traversal_by_index.c
* @author rigensen
* @brief 
* @date 三 10/ 9 14:00:13 2019
*/

#include <stdio.h>
#include <time.h>
#include <dirent.h>
#include <sys/time.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdlib.h>

int64_t get_time_us()
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return tv.tv_sec*(int64_t)(1000000)+tv.tv_usec;
}

int main()
{
    struct stat stat_buf;
    FILE *fp = fopen("./index.txt", "r");
    size_t size = 0;
    char *buf_ptr = NULL, *buf_end = NULL, *forward = NULL, *ori=NULL;
    int i = 0;
    int64_t start,end;

    start = get_time_us();
    stat("./index.txt", &stat_buf);
    size = (size_t)stat_buf.st_size;
    buf_ptr = (char *)calloc( 1, (size_t)size );
    if ( !buf_ptr )
        return 0;
    fread(buf_ptr,size,1,fp);
    buf_end = buf_ptr+(int)size;
    forward = buf_ptr;
    ori = buf_ptr;
    while( buf_ptr < buf_end) {
        while( *forward != '\n' && forward < buf_end )
            forward++;
        *forward = '\0';
        printf("[%05d] %s\n", i++, buf_ptr );
        buf_ptr = forward+1;
    }
    free(ori);
    fclose(fp);
    end = get_time_us();
    printf("\ncost time:%"PRId64"us\n", end-start);
    return 0;
}

