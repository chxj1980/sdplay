/**
* @file traversal_readline.c
* @author rigensen
* @brief 
* @date 三 10/ 9 14:32:45 2019
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
    FILE *fp = fopen("./index.txt", "r");
    int i = 0;
    int64_t start,end;
    ssize_t read;
    char *line = NULL;
    size_t len = 0;

    start = get_time_us();
    if ( !fp )
        return 0;
    while( (read = getline(&line, &len, fp) ) != -1 ) {
        printf("[%05d] %s", i++, line );
    }
    fclose(fp);
    end = get_time_us();
    printf("\ncost time:%"PRId64"us\n", end-start);
    return 0;
}

