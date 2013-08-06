#ifndef SENDINFO_H
#define SENDINFO_H

#include <stdio.h>
#include <openssl/ssl.h>
#include <vector>
#include "helper.h"

using namespace std;

/*
 * send_info: store the information of the send list
 * path: file's path
 * file_type: file or director
 * md: the md5 of the file
 */
class send_info
{
private:
    char *path;
    char file_type;
    unsigned char md[SHA_DIGEST_LENGTH];
public:
    char *get_path();
    char get_file_type();
    unsigned char *get_md();
    send_info(const char *path, char file_type, unsigned char *md);
};

/*
 * scan_dir: scan the project and store the important information in the send_list member
 * dir_path: configue's path
 * send_list: the send list
 * file_count: the count of all file
 */
class scan_dir
{
private:
    char *dir_path;
    vector<send_info> send_list;
public:
    void sha1(const char*path, unsigned char *md);
    void scan_the_dir(const char* dir, int depth);
    void send_file_list(SSL *ssl);
    scan_dir(const char *dir_path);
};
#endif
