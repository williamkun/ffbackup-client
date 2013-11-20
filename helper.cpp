#include <stdio.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <stdlib.h>
#include <dirent.h>
#include <librsync.h>
#include "ffbuffer.h"
#include "helper.h"
#include "config.h"

#define FF_LITTLE_ENDIAN 0
#define FF_BIG_ENDIAN 1

const char *CFG_PATH = "/etc/ffbackup/client.cfg";
extern client_config g_config;

void dump_data(void *data, size_t size)
{
    unsigned char *ptr = (unsigned char *)data;
    size_t i;
    for(i = 0; i < size; ++i)
        fprintf(stderr, "%02X ", (int)ptr[i]);
}

int get_byte_order()
{
    uint16_t k = 0x0102;
    unsigned char *ptr = (unsigned char *)&k;
    if(ptr[0] == 0x02)
        return FF_LITTLE_ENDIAN;
    else
        return FF_BIG_ENDIAN;
}

uint16_t ntoh16(uint16_t net)
{
    return ntohs(net);
}

uint16_t hton16(uint16_t host)
{
    return htons(host);
}

uint32_t ntoh32(uint32_t net)
{
    return ntohl(net);
}

uint32_t hton32(uint32_t host)
{
    return htonl(host);
}

uint64_t ntoh64(uint64_t net)
{
    uint64_t u = net;
    if(get_byte_order() == FF_LITTLE_ENDIAN)
    {
        uint8_t *ptr_net = (uint8_t *)&net;
        uint8_t *ptr_u = (uint8_t *)&u;
        int i, j;
        for(i = 0, j = 7; i < 8; ++i, --j)
            ptr_u[i] = ptr_net[j];
    }
    return u;
}

uint64_t hton64(uint64_t host)
{
    uint64_t u = host;
    if(get_byte_order() == FF_LITTLE_ENDIAN)
    {
        uint8_t *ptr_host = (uint8_t *)&host;
        uint8_t *ptr_u = (uint8_t *)&u;
        int i, j;
        for(i = 0, j = 7; i < 8; ++i, --j)
            ptr_u[i] = ptr_host[j];
    }
    return u;
}

char *read_string(SSL *ssl)
{
    ffbuffer store;
    char buf[1];
    int ret;
    size_t ffbuffer_length = 0;
    char *str;

    while(1)
    {
        ret = SSL_read(ssl, buf, 1);
        switch( SSL_get_error( ssl, ret ) )
        {
            case SSL_ERROR_NONE:
                break;
            default:
                fputs("SSL_write error.\n", stderr);
                exit(1);
        }
        store.push_back(buf, 1);
        if(!buf[0])
            break;
    }
    ffbuffer_length = store.get_size();
    str = (char *)malloc(ffbuffer_length);
    if(!str)
    {
        fputs("malloc error.\n", stderr);
        exit(1);
    }
    store.get(str, 0, ffbuffer_length);
    return str;
}

//读取指定数量的数据，如果缓冲区没有足够的数据，则阻塞到有新数据到达
void ssl_read_wrapper(SSL *ssl, void *buffer, int num)
{
    int ret = 0;
    int pos = 0;
    char *ptr = (char *)buffer;

    while(pos < num)
    {
        ret = SSL_read(ssl, ptr + pos, num - pos);
        switch( SSL_get_error( ssl, ret ) )
        {
            case SSL_ERROR_NONE:
                break;
            default:
                fputs("SSL_read error.\n", stderr);
                exit(1);
        }
        pos += ret;
    }
}

void ssl_write_wrapper(SSL *ssl, const void *buffer, int num)
{
    int ret;

    ret = SSL_write(ssl, buffer, num);
    switch( SSL_get_error( ssl, ret ) )
    {
        case SSL_ERROR_NONE:
            break;
        default:
            fputs("SSL_write error.\n",stderr);
            exit(1);
    }
}

//获取一个文件列表（每个文件有路径和文件类型两个属性）
vector<file_info> get_server_list(SSL *ssl)
{
    vector<file_info> file_list;
    uint32_t file_count = 0;
    int i = 0;
    char file_type;
    char *file_path;

    ssl_read_wrapper(ssl, &file_count, 4);
    file_count = ntoh32(file_count);
    for(i = 0; i < (int)file_count; i++)
    {
        file_path = read_string(ssl);
        ssl_read_wrapper(ssl, &file_type, 1);
        file_info store(file_path, file_type);
        file_list.push_back(store);
        free(file_path);
    }
    return file_list;
}

void _scan_dir(const char *dir, int parent_index, vector<file_info> &local_list)
{
    DIR *dp;
    struct dirent *entry;
    struct stat statbuf;

    if((dp = opendir(dir)) == NULL || chdir(dir) < 0)
    {
        if(parent_index == -1)
            fputs("can not scan the directory.\n", stderr);
        return;
    }
    while((entry = readdir(dp)) != NULL)
    {
        lstat(entry->d_name, &statbuf);
        if(S_ISDIR(statbuf.st_mode))
        {
            if(strcmp(".",entry->d_name) == 0 || strcmp("..",entry->d_name) == 0)
                continue;
            string name;
            if(parent_index != -1)
            {
                name = local_list.at(parent_index).get_path();
                name += "/";
            }
            name += entry->d_name;
            local_list.push_back(file_info(name.c_str(),'d'));
            _scan_dir(entry->d_name, local_list.size() - 1, local_list);
        }
        else if(S_ISREG(statbuf.st_mode))
        {
            string name;
            if(parent_index != -1)
            {
                name = local_list.at(parent_index).get_path();
                name += "/";
            }
            name += entry->d_name;
            local_list.push_back(file_info(name.c_str(),'f'));
        }
    }
    if(chdir("..") == -1)
    {
        fputs("chdir error.\n", stderr);
        exit(1);
    }
    closedir(dp);
}

vector<file_info> get_local_list(const char *project_path)
{
    vector<file_info> file_list;

    _scan_dir(project_path, -1, file_list);
    return file_list;
}

void list_compare(vector<file_info>&local_list, vector<file_info>&server_list,
                  vector<file_info>&addition_list, vector<file_info>&diff_list,
                  vector<file_info>&deletion_list)
{
    size_t i = 0;
    size_t j = 0;
    vector<file_info> temp_list = server_list;
    bool found;

    while(i < local_list.size())
    {
        found = false;
        while(j < temp_list.size())
        {
            if(strcmp(local_list.at(i).get_path(), temp_list.at(j).get_path()) == 0)
            {
                if(local_list.at(i).get_file_type() == temp_list.at(j).get_file_type())
                {
                    if(local_list.at(i).get_file_type() == 'd')
                        temp_list.erase(temp_list.begin() + j);
                    else
                    {
                        diff_list.push_back(local_list.at(i));
                        temp_list.erase(temp_list.begin() + j);
                    }
                    found = true;
                    break;
                }
            }
            j++;
        }
        if(!found)
            addition_list.push_back(local_list.at(i));
        i++;
        j = 0;
    }
    deletion_list = temp_list;
}

//列表中的任意目录里面的文件都应该从列表中删除
//比如列表  {"src", "src/helper.cpp", "src/build", "LICENSE"}
//可简化为  {"src", "LICENSE"}
void simplify_deletion_list(vector<file_info>&deletion_list)
{
    vector<file_info> temp_list = deletion_list;
    const size_t max_buffer_size = 1024;
    char buffer[max_buffer_size];
    size_t buffer_length = 0;
    size_t i = 0;
    size_t j = 0;

    while(i < temp_list.size())
    {
        if(temp_list.at(i).get_file_type() == 'd')
        {
            strcpy(buffer, temp_list.at(i).get_path());
            buffer_length = strlen(buffer);
            strcpy(&buffer[buffer_length],"/");
            buffer_length++;
            j = 0;
            while( j < deletion_list.size())
            {
                if(strncmp(buffer, deletion_list.at(j).get_path(), buffer_length) == 0)
                {
                    deletion_list.erase(deletion_list.begin() + j);
                }
                else
                    j++;
            }
        }
        i++;
    }
}

void find_delta_list(vector<file_info> diff_list, vector<file_info>&delta_list)
{
    const size_t md_length = 20;
    unsigned char *sha;
    unsigned char temp[md_length];
    unsigned int i = 0;
    unsigned int j = 0;
    sha = (unsigned char *)malloc(sizeof(unsigned char) * 20);
    if(!sha)
    {
        fputs("Malloc error.\n",stderr);
        exit(1);
    }
    for(i = 0; i < diff_list.size(); i++)
    {
        sha = diff_list.at(i).get_sha1();
        get_file_sha1(diff_list.at(i).get_path(), temp);
        for(j = 0; j < md_length; j++)
        {
            if(temp[j] == sha[j]) continue;
            else break;
        }
        if(j < 20)
            delta_list.push_back(diff_list.at(i));
        j = 0;
    }
}

void get_file_sha1(const char* path, unsigned char *md)
{
    const char *project_path = g_config.get_backup_path();
    if(chdir(project_path) == -1)
    {
        fputs("Chdir error.\n",stderr);
        exit(1);
    }

    int pf = open(path,O_RDONLY);
    if(pf == -1)
    {
        fputs("File can not be open.\n",stderr);
        exit(1);
    }
    if(chdir("..") == -1)
    {
        fputs("Chdir error.\n",stderr);
        exit(1);
    }

    const size_t buffer_size = 2048;
    ssize_t ret;
    unsigned char data[buffer_size];
    SHA_CTX ctx;
    if(SHA1_Init(&ctx) == 0)
    {
        fputs("SHA1_Init error.\n",stderr);
        exit(1);
    }
    while( (ret = read(pf, data, buffer_size)) > 0)
    {
        if(SHA1_Update(&ctx,data,ret) == 0)
        {
            fputs("SHA1_Update error.\n",stderr);
            exit(1);
        }
    }
    if(SHA1_Final(md, &ctx) == 0)
    {
        fputs("SHA1_Final error.\n",stderr);
        exit(1);
    }
}

void send_file_delta(const char* new_file_path, const char *sig_file_path, SSL *ssl)
{
    FILE *sig_file;
    FILE *new_file;
    FILE *delta_file;
    size_t result = 0;
    const size_t max_buffer_size = 1024;
    char delta_buffer[max_buffer_size];
    long lsize = 0;
    uint64_t delta_length = 0;
    rs_result ret;
    rs_signature_t *sumset;
    rs_stats_t stats;

    sig_file = fopen(sig_file_path,"rb");
    new_file = fopen(new_file_path,"rb");
    delta_file = tmpfile();

    ret = rs_loadsig_file(sig_file, &sumset, &stats);
    if(ret != RS_DONE)
    {
        puts(rs_strerror(ret));
        exit(1);
    }
    rs_log_stats(&stats);
    if(rs_build_hash_table(sumset) != RS_DONE)
    {
        puts(rs_strerror(ret));
        exit(1);
    }
    if(rs_delta_file(sumset, new_file, delta_file, &stats) != RS_DONE)
    {
        puts(rs_strerror(ret));
        exit(1);
    }
    fseek (delta_file , 0 , SEEK_END);
    lsize = ftell(delta_file);
    if(lsize < 0)
    {
        fputs("Delta_file is wrong.\n",stderr);
        exit(1);
    }

    rewind(delta_file);
    delta_length = (uint64_t)lsize;
    delta_length = hton64(delta_length);

    ssl_write_wrapper(ssl, new_file_path, strlen(new_file_path) + 1);
    ssl_write_wrapper(ssl, &delta_length, 8);

    while(!feof(delta_file))
    {
        result = fread(delta_buffer, 1, max_buffer_size, delta_file);
        if(result > 0)
            ssl_write_wrapper(ssl, delta_buffer, result);
    }

    rs_log_stats(&stats);
    rs_free_sumset(sumset);
    fclose(sig_file);
    fclose(new_file);
    fclose(delta_file);
}

void send_file_addition(const char *project_path, const char *path, SSL *ssl)
{
    if(chdir(project_path) == -1)
    {
        fputs("Project path is wrong.\n",stderr);
        exit(1);
    }
    const size_t max_buffer_size = 1024;
    FILE *pf;
    struct stat file_info;
    uint64_t file_size;
    char buffer[max_buffer_size];
    size_t result= 0;
    if(stat(path,&file_info) == -1)
    {
        fputs("Stat error.\n",stderr);
        exit(1);
    }
    if(S_ISDIR(file_info.st_mode))
    {
        ssl_write_wrapper(ssl, path, strlen(path) + 1);
        ssl_write_wrapper(ssl, "d", 1);
    }
    if(S_ISREG(file_info.st_mode))
    {
        file_size = (uint64_t)file_info.st_size;
        pf = fopen(path, "rb");
        if(!pf)
        {
            fputs("File cannot be open.\n",stderr);
            exit(1);
        }
        file_size = hton64(file_size);
        ssl_write_wrapper(ssl, path, strlen(path) + 1);
        ssl_write_wrapper(ssl, "f", 1);
        ssl_write_wrapper(ssl, &file_size, 8);
        while(!feof(pf))
        {
            result = fread(buffer, 1, max_buffer_size, pf);
            if(result > 0)
                ssl_write_wrapper(ssl, buffer, result);
        }
        fclose(pf);
    }
    if(chdir("..") == -1)
    {
        fputs("Chdir error.\n",stderr);
        exit(1);
    }
}
